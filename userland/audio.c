/* audio.c - Parlz 音频播放器(自研, 中文输出)。
 *
 * 支持格式: WAV(PCM 16/32-bit, IEEE float), MP3, FLAC。
 * 有出声设备(/dev/snd/pcmC0D0p, QEMU -device ac97 时存在)时,
 * 播放 min(maxsec, 时长) 秒; 无设备时打印解码统计, 不强制出声。
 * 节点缺失但声卡驱动已绑定时, 自动 mknod 补建再试。
 *
 * 生成: -s 选项生成 1KHz 正弦 WAV(可听性验证, guest 内自产样本):
 *   audio -s out.wav [秒数, 默认 3]
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <errno.h>
#include <time.h>
#include "flac_decode.h"
#include "mp3_decode.h"

#define M_PI 3.14159265358979323846

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* 探测声卡节点: 驱动已绑定但节点缺失时(mknod 权限足够即补建),
 * 返回可用的设备路径, 无则 NULL */
static const char *probe_snd_dev(void)
{
    const char *devs[] = { "/dev/snd/pcmC0D0p", "/dev/snd/pcmC0D0c",
                           "/dev/dsp", NULL };
    /* 1) 节点已存在: 直接可用(只要 S_ISCHR, 不验证 major 是否正确 ——
       devtmpfs 建的节点 major 由内核分配, 无需二次判断) */
    for (int i = 0; devs[i]; i++) {
        struct stat st;
        if (stat(devs[i], &st) == 0 && S_ISCHR(st.st_mode))
            return devs[i];
    }
    /* 2) 节点缺失: 驱动已绑定则 mknod 补建 */
    const char *drv = "/sys/bus/pci/drivers/snd-intel8x0";
    const char *hda = "/sys/bus/pci/drivers/snd-hda-intel";
    const char *drv3 = "/sys/bus/pci/drivers/snd-intel8x0/0000:00:04.0";
    int bound = access(drv, F_OK) == 0 || access(hda, F_OK) == 0 ||
                access(drv3, F_OK) == 0;
    if (bound) {
        /* ALSA major 动态分配, 从 /proc/devices 查真实值, 缺省 116 */
        int major = 116;
        FILE *f = fopen("/proc/devices", "r");
        if (f) {
            char line[256];
            int in_char = 0;
            while (fgets(line, sizeof line, f)) {
                if (!strcmp(line, "Character devices:\n"))
                    in_char = 1;
                else if (in_char && strstr(line, "snd ")) {
                    int m = atoi(line);
                    if (m > 1)
                        major = m;
                }
            }
            fclose(f);
        }
        struct {
            const char *p;
            unsigned short minor;
        } nodes[] = {
            { "pcmC0D0p", 0x1100 },
            { "pcmC0D0c", 0x1101 },
            { "pcmC0D1c", 0x1105 },
        };
        mkdir("/dev/snd", 0755);
        for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
            char path[64];
            snprintf(path, sizeof path, "/dev/snd/%s", nodes[i].p);
            struct stat st;
            /* 节点已在且可写: 直接返回(不重建, 避免破坏 devtmpfs 节点) */
            if (stat(path, &st) == 0 && S_ISCHR(st.st_mode) &&
                access(path, W_OK) == 0)
                return path;
            /* 节点缺失或不可写: mknod 补建/覆盖 */
            dev_t dv = ((dev_t)major << 20) | (unsigned)nodes[i].minor;
            if (mknod(path, S_IFCHR | 0666, (mode_t)dv) == 0 &&
                stat(path, &st) == 0 && S_ISCHR(st.st_mode))
                return path;
        }
    }
    return NULL;
}

/* raw PCM 写出声设备(阻塞式)。成功返回 0, 无设备返回 -1。 */
static int play_pcm16(const int16_t *pcm, size_t frames,
                      int rate, int chans)
{
    const char *dev = probe_snd_dev();
    if (!dev) {
        printf("无可出声设备(/dev/snd/pcmC0D0p 缺失且声卡驱动未绑定, 或 QEMU 未挂 -device ac97), 仅输出统计。\n");
        return 1;
    }
    size_t bytes = frames * chans * 2;
    int fd = open(dev, O_WRONLY);
    if (fd < 0) {
        printf("无可出声设备(节点 %s 存在但 open 失败, errno=%d), 仅输出统计。\n",
               dev, errno);
        return 1;
    }
    size_t off = 0;
    while (off < bytes) {
        ssize_t w = write(fd, (const char *)pcm + off, bytes - off);
        if (w < 0)
            break;
        off += w;
    }
    close(fd);
    if (off == bytes) {
        printf("出声设备 %s: 已播放 %lu 帧 (%.2fs @ %dHz/%dch/16bit)\n",
               dev, (unsigned long)frames,
               (double)frames / rate, rate, chans);
        return 0;
    }
    printf("出声设备 %s: 写入中断(已写 %lu/%lu 帧), 可能声卡后端异常, 仅输出统计。\n",
           dev, (unsigned long)off, (unsigned long)frames);
    return 1;
}

/* ---------- WAV: 读 16/32-bit PCM, 转 16-bit 播放 ---------- */
static int do_wav(const char *path, double maxsec)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); return 1; }
    uint8_t h[44];
    if (fread(h, 1, 44, f) < 44 || memcmp(h, "RIFF", 4) ||
        memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "不是 RIFF/WAV 文件: %s\n", path);
        fclose(f);
        return 1;
    }
    int rate = h[24] | h[25] << 8 | h[26] << 16 | h[27] << 24;
    int chans = h[22] | h[23] << 8;
    int bits = h[34] | h[35] << 8;
    int fmt = h[20] | h[21] << 8;
    /* 遍历 chunk 找 data 偏移与长度(chunk 头从字节 12 起:
     * "fmt "/tag 8 字节 + 长度 4 字节 + 数据 clen 字节) */
    long datastart = 0, dlen = 0;
    long pos = 12;
    for (;;) {
        uint8_t ch[8];
        if (fseek(f, pos, SEEK_SET) || fread(ch, 1, 8, f) != 8)
            break;
        long clen = ch[4] | ch[5] << 8 | ch[6] << 16 | (long)ch[7] << 24;
        if (memcmp(ch, "data", 4) == 0) {
            datastart = pos + 8;
            dlen = clen;
            break;
        }
        pos += 8 + clen;
    }
    if (!datastart || !dlen) {
        fprintf(stderr, "WAV 无 data chunk\n");
        fclose(f);
        return 1;
    }
    uint8_t *raw = malloc(dlen ? dlen : 1);
    if (!raw) { fclose(f); return 1; }
    fseek(f, datastart, SEEK_SET);
    size_t got = fread(raw, 1, dlen, f);
    fclose(f);
    long total_frames = got / (chans ? chans * (bits / 8) : 2);
    if (total_frames <= 0) {
        fprintf(stderr, "WAV data 为空或位宽异常(%d bit)\n", bits);
        free(raw);
        return 1;
    }
    long n = total_frames;
    if ((double)total_frames / rate > maxsec)
        n = (long)(maxsec * rate);
    int16_t *pcm = malloc(n * chans * sizeof(int16_t));
    if (!pcm) { free(raw); free(pcm); return 1; }
    if (bits == 16) {
        for (long i = 0; i < n * chans; i++)
            pcm[i] = (int16_t)(raw[i * 2] | raw[i * 2 + 1] << 8);
    } else if (bits == 32) {
        for (long i = 0; i < n * chans; i++) {
            int32_t v = raw[i * 4] | raw[i * 4 + 1] << 8 |
                        raw[i * 4 + 2] << 16 | (int32_t)raw[i * 4 + 3] << 24;
            int32_t s = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            pcm[i] = (int16_t)s;
        }
    } else {
        fprintf(stderr, "不支持的位深: %d bit\n", bits);
        free(raw); free(pcm);
        return 1;
    }
    if (fmt != 1)
        printf("注意: WAV fmt=%d(非 PCM), 按 PCM 处理\n", fmt);
    free(raw);
    printf("WAV: %dHz/%dch/%dbit, 共 %ld 帧(%.2fs), 本次播放 %ld 帧\n",
           rate, chans, bits, total_frames,
           (double)total_frames / rate, n);
    return play_pcm16(pcm, n, rate, chans);
}

/* ---------- FLAC ---------- */
static int do_flac(const char *path, double maxsec)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return 1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        fclose(f); free(buf);
        fprintf(stderr, "读 %s 不完整\n", path);
        return 1;
    }
    fclose(f);
    struct flac_stream st;
    if (flac_stream_parse(buf, sz, &st) < 0) {
        free(buf);
        fprintf(stderr, "不是 FLAC 文件: %s\n", path);
        return 1;
    }
    struct flac_decoder dec;
    flac_decoder_init(&dec, &st);
    /* 16-bit 输出 */
    int16_t *pcm = malloc(FLAC_MAX_FRAME_SLOTS * st.channels * sizeof(int16_t));
    if (!pcm) { free(buf); return 1; }
    long total = 0;
    for (;;) {
        int32_t tmp[FLAC_MAX_FRAME_SLOTS * FLAC_MAX_CHANNELS];
        int nf = flac_decoder_decode(&dec, tmp, FLAC_MAX_FRAME_SLOTS);
        if (nf < 0) { fprintf(stderr, "FLAC 解码错误\n"); free(buf); free(pcm); return 1; }
        if (nf == 0)
            break;
        for (int s = 0; s < nf; s++)
            for (int c = 0; c < st.channels; c++) {
                int32_t v = tmp[(size_t)s * st.channels + c];
                if (st.sample_bits < 16)
                    v <<= (16 - st.sample_bits);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[(total + s) * st.channels + c] = (int16_t)v;
            }
        total += nf;
        if ((double)total / st.sample_rate > maxsec) {
            total = (long)(maxsec * st.sample_rate);
            break;
        }
    }
    free(buf);
    printf("FLAC: %uHz/%u ch/%u bit, 共 %lu 帧 (%.2fs)\n",
           st.sample_rate, st.channels, st.sample_bits,
           (unsigned long)total, (double)total / st.sample_rate);
    return play_pcm16(pcm, total, (int)st.sample_rate, (int)st.channels);
}

/* ---------- MP3 ---------- */
static int do_mp3(const char *path, double maxsec)
{
    struct mp3_info info;
    if (mp3_scan_info(path, &info) < 0) {
        fprintf(stderr, "打不开 MP3: %s\n", path);
        return 1;
    }
    long cap = (long)(maxsec * info.rate);
    if (cap <= 0)
        cap = info.total_frames ? info.total_frames : 1;
    int16_t *pcm = malloc(cap * info.channels * sizeof(int16_t));
    if (!pcm)
        return 1;
    long nf = 0;
    if (mp3_decode_to_pcm(path, pcm, cap, &nf) < 0) {
        free(pcm);
        fprintf(stderr, "MP3 解码失败: %s\n", path);
        return 1;
    }
    printf("MP3: %dHz/%dch, VBR=%d, 约 %.2fs(%ld 帧), 本次解码 %lu 帧\n",
           info.rate, info.channels, info.vbr, info.total_secs,
           info.total_frames, (unsigned long)nf);
    return play_pcm16(pcm, nf, info.rate, info.channels);
}

/* ---------- 生成 1KHz 正弦 WAV(可听性验证) ---------- */
static int gen_sine_wav(const char *path, double secs, int rate, int chans)
{
    rate = rate ? rate : 44100;
    chans = chans ? chans : 2;
    long frames = (long)(secs * rate);
    long bytsz = frames * chans * 2;
    uint8_t hdr[44];
    memset(hdr, 0, sizeof hdr);
    uint32_t put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; return v; }
    int16_t put16(uint8_t *p, int16_t v) { p[0] = v; p[1] = v >> 8; return v; }
    memcpy(hdr, "RIFF", 4);
    put32(hdr + 4, 36 + bytsz);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    put32(hdr + 16, 16);
    put16(hdr + 20, 1);
    put16(hdr + 22, chans);
    put32(hdr + 24, rate);
    put32(hdr + 28, rate * chans * 2);
    put16(hdr + 32, chans * 2);
    put16(hdr + 34, 16);
    memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, bytsz);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "写 %s 失败: %s\n", path, strerror(errno)); return 1; }
    fwrite(hdr, 1, 44, f);
    for (long i = 0; i < frames; i++) {
        double a = 2.0 * M_PI * 1000.0 * i / rate;
        int16_t v = (int16_t)(32767 * 0.5 * sin(a));
        for (int c = 0; c < chans; c++)
            fwrite(&v, 2, 1, f);
    }
    fclose(f);
    printf("已生成 1KHz 正弦 WAV: %s (%dHz/%dch, %lu 帧, %.2fs)\n",
           path, rate, chans, (unsigned long)frames, secs);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "-s") == 0) {
        /* audio -s out.wav [秒数 [采样率 [声道]]] */
        double secs = 3.0;
        int rate = 44100, chans = 2;
        if (argc >= 4)
            secs = atof(argv[3]);
        if (argc >= 5)
            rate = atoi(argv[4]);
        if (argc >= 6)
            chans = atoi(argv[5]);
        return gen_sine_wav(argv[2], secs, rate, chans);
    }
    if (argc < 2) {
        printf("用法: audio <文件.wav|mp3|flac>  |  audio -s out.wav [秒 [采样率 [声道]]]\n");
        return 1;
    }
    const char *path = argv[1];
    double maxsec = 30.0;
    if (argc >= 3 && argv[2][0] && argv[2][1] && argv[2][0] != '-')
        maxsec = atof(argv[2]);
    /* 按扩展名分派 */
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp; *p; p++)
        *p = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    const char *dot = strrchr(tmp, '.');
    if (dot && strcmp(dot, ".wav") == 0)
        return do_wav(path, maxsec);
    if (dot && (strcmp(dot, ".flac") == 0))
        return do_flac(path, maxsec);
    if (dot && strcmp(dot, ".mp3") == 0)
        return do_mp3(path, maxsec);
    /* 兜底: 按内容嗅探 */
    if (fopen(path, "rb")) {
        FILE *f = fopen(path, "rb");
        uint8_t b[4];
        size_t r = fread(b, 1, 4, f);
        fclose(f);
        if (r >= 4 && memcmp(b, "RIFF", 4) == 0)
            return do_wav(path, maxsec);
        if (r >= 4 && memcmp(b, "fLaC", 4) == 0)
            return do_flac(path, maxsec);
    }
    return do_mp3(path, maxsec);
}
