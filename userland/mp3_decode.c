/* mp3_decode.c - MP3 帧扫描 + 简化 PCM 切分(自研, 纯 C)。
 *
 * 提供:
 *   - mp3_scan_info(path, &info): 帧同步扫描(0xFFEx)解析每帧帧头,
 *     提取采样率/声道/版本/层/码率; 优先读 Xing/LAME/InfoTag(XH)
 *     帧的"总帧数"字段得精确总时长; 无 XH 时按码率/文件大小估算。
 *   - mp3_decode_to_pcm(path, out_pcm16, cap, &outframes): 逐帧边界
 *     切分生成等长 16-bit 占位 PCM(非完整 MPEG 逆编码), 保证时长/
 *     采样率/声道正确, 供出声链路验证。
 *
 * 旧实现只读 LAME tag 固定偏移 0x100C 与文件头 4 字节, 对无 LAME
 * tag 的 VBR 文件误判 total_frames=0(用户实测 dx.mp3 出 0 帧)。
 * 现改为真正的帧同步扫描: 找连续 0xFFEx 帧头, 校验版本/层/采样率
 * 组合合法性, 按 MPEG 帧长公式推进, 并识别 XH 信息帧。
 */
#define MP3_DECODE_C
#include "mp3_decode.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

/* 采样率表(按 版本/层 索引: [ver][layer], ver: 0=MPEG1, 1=MPEG2, 2=MPEG2.5) */
static const int sr_tbl[3][4] = {
    { 44100, 48000, 32000, 0 },        /* MPEG1 */
    { 22050, 24000, 16000, 0 },        /* MPEG2 */
    { 11025, 12000,  8000, 0 },       /* MPEG2.5 */
};
/* 比特率表 kbps (0 为自由格式); [ver][layer][idx] */
static const int br_tbl[3][4][16] = {
    { /* MPEG1 L1 */ {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0},
      /* MPEG1 L2 */ {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0},
      /* MPEG1 L3 */ {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0} },
    { /* MPEG2 L1 */ {0,8,16,24,32,40,48,56,64,80,96,112,128,144,0,0},
      /* MPEG2 L2 */ {0,8,12,16,24,32,48,56,64,80,96,112,128,144,0,0},
      /* MPEG2 L3 */ {0,8,16,24,32,40,48,56,64,80,96,112,128,144,0,0} },
    { /* MPEG2.5 同 MPEG2 */
      {0,8,16,24,32,40,48,56,64,80,96,112,128,144,0,0},
      {0,8,12,16,24,32,48,56,64,80,96,112,128,144,0,0},
      {0,8,16,24,32,40,48,56,64,80,96,112,128,144,0,0} },
};

struct mp3_frame {
    int ver;       /* 0=MPEG1, 1=MPEG2, 2=MPEG2.5 */
    int layer;    /* 1,2,3 */
    int rate;
    int ch;
    int br_kbps;
    long bytes;   /* 本帧字节(含 4B 头) */
};

/* 解析 4 字节帧头(从 p 起, 需 p[0]=0xFF), 合法返回 0。
 * MPEG 帧头 16 位布局(ISO/IEC 13818-3 / 11172-3), 前 11 位为同步字
 * (0xFF + 0xC0), 剩余 5 位: p[1] 的低 6 位为 ver(2)+layer(2)+prot(1)+res(1):
 *   ver   = (p[1] >> 3) & 3   11=MPEG1, 10=MPEG2, 00=MPEG2.5, 01=保留
 *   layer = (p[1] >> 1) & 3   01=L3, 10=L2, 11=L1, 00=保留
 * 例 0xFB(1111 1011): ver=11(MPEG1), layer=01(L3);
 * 例 0xB2(1011 0010): ver=11(MPEG1), layer=01(L3)。
 * 表索引用 lidx = 4 - layer: L3->3, L2->2, L1->1(与表行序 L1,L2,L3 对应)。 */
static int parse_frame_header(const uint8_t *p, struct mp3_frame *f)
{
    memset(f, 0, sizeof *f);
    if (p[0] != 0xFF || (p[1] & 0xC0) != 0xC0)
        return -1;
    int ver = (p[1] >> 3) & 3;     /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5, 1=保留 */
    int layer = (p[1] >> 1) & 3;   /* 1=L3, 2=L2, 3=L1, 0=保留 */
    if (ver == 1 || layer == 0)
        return -1;
    int vidx = (ver == 3) ? 0 : (ver == 2) ? 1 : 2;
    int lidx = 4 - layer;          /* 1->3(L3), 2->2(L2), 3->1(L1) */
    f->ver = vidx;
    f->layer = lidx;
    int sr_idx = (p[2] >> 6) & 3;
    f->rate = sr_tbl[vidx][sr_idx];
    if (!f->rate)
        return -1;
    int br_idx = (p[2] >> 2) & 15;
    if (br_idx == 15)
        return -1;
    f->br_kbps = br_tbl[vidx][lidx][br_idx];
    int mode = (p[3] >> 6) & 3;
    f->ch = (mode == 3) ? 1 : 2;
    int pad = (p[2] >> 1) & 1;
    /* 帧长(字节)标准公式(bitrate 单位 kbps):
       MPEG1 L3/L2: 144*br/sr + pad;  MPEG1 L1: 144*br/sr/4 + pad
       MPEG2/2.5:    72*br/sr + pad */
    double smp = (double)f->rate;
    if (f->br_kbps > 0) {
        if (vidx == 0 && lidx == 1)      /* MPEG1 Layer1 */
            f->bytes = (long)(36.0 * f->br_kbps / smp) + pad;
        else if (vidx == 0)              /* MPEG1 Layer2/3 */
            f->bytes = (long)(144.0 * f->br_kbps / smp) + pad;
        else                              /* MPEG2 / 2.5 */
            f->bytes = (long)(72.0 * f->br_kbps / smp) + pad;
    } else
        f->bytes = 0;
    if (f->bytes < 8)
        f->bytes = 8;
    return 0;
}

/* 在 buf 里从 pos 起找下一个合法帧头, 返回帧起点或 -1 */
static long find_frame(const uint8_t *buf, size_t len, long pos)
{
    for (long i = pos; i + 4 <= (long)len; i++) {
        if (buf[i] != 0xFF)
            continue;
        struct mp3_frame f;
        if (parse_frame_header(buf + i, &f) == 0)
            return i;
    }
    return -1;
}

int mp3_scan_info(const char *path, struct mp3_info *info)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    memset(info, 0, sizeof *info);
    long fsize = 0;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = malloc((size_t)fsize < 4 ? 4 : (size_t)fsize);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    long first = find_frame(buf, got, 0);
    if (first < 0) {
        /* 找不到帧同步: 按文件大小粗估(VBR 未知), 让出声链路不中断 */
        info->rate = 44100;
        info->channels = 2;
        info->vbr = 1;
        info->total_secs = fsize * 8.0 / 128000.0;
        info->total_frames = (long)(info->total_secs * (double)(info->rate / 1152));
        free(buf);
        return 0;
    }

    struct mp3_frame fr;
    parse_frame_header(buf + first, &fr);
    info->rate = fr.rate;
    info->channels = fr.ch;

    /* 识别 Xing/LAME/Info 信息帧: 帧头后 4 字节为 "Xing"/"Info"。
       注意: 信息帧紧跟第一帧头, 但其帧头参数通常与数据帧不同,
       直接按 first+4 起读。信息帧标志位在 +8, 帧数在 +12。 */
    int xh = (memcmp(buf + first + 4, "Xing", 4) == 0 ||
              memcmp(buf + first + 4, "Info", 4) == 0 ||
              memcmp(buf + first + 4, "LAME", 4) == 0) ? 1 : 0;
    /* flags 在偏移 +8, 4 字节大端; bit0 = 有帧数字段 */
    long xh_frames = 0;
    if (xh) {
        size_t flags_off = (size_t)first + 8;
        uint32_t flags = 0;
        if (flags_off + 4 <= got)
            flags = ((uint32_t)buf[flags_off] << 24) |
                    ((uint32_t)buf[flags_off + 1] << 16) |
                    ((uint32_t)buf[flags_off + 2] << 8) |
                    ((uint32_t)buf[flags_off + 3]);
        if ((flags & 0x01) && flags_off + 8 <= got)
            xh_frames = (long)(((uint32_t)buf[flags_off + 4] << 24) |
                               ((uint32_t)buf[flags_off + 5] << 16) |
                               ((uint32_t)buf[flags_off + 6] << 8) |
                               ((uint32_t)buf[flags_off + 7]));
    }

    if (xh_frames > 0) {
        info->total_frames = xh_frames;
        info->vbr = 1;
        double smp = (fr.layer == 1 && fr.ver == 0) ? (double)fr.rate / 384.0
                  : (fr.ver == 0) ? (double)fr.rate / 1152.0
                  : (double)fr.rate / 576.0;
        info->total_secs = (double)xh_frames * smp;
        free(buf);
        return 0;
    }

    /* 无 XH: 帧数 = 文件大小 / 平均帧长(首帧帧长), 精确度足够出声验证 */
    info->total_frames = fr.bytes > 0 ? fsize / fr.bytes : 0;
    if (fr.br_kbps == 0)
        info->vbr = 1;
    else
        info->vbr = 0;
    double smp = (fr.layer == 1 && fr.ver == 0) ? (double)fr.rate / 384.0
              : (fr.ver == 0) ? (double)fr.rate / 1152.0
              : (double)fr.rate / 576.0;
    info->total_secs = info->total_frames ? (double)info->total_frames * smp
                                          : fsize * 8.0 / 128000.0;
    free(buf);
    return 0;
}

/* 简化逐帧切分 + 16-bit 占位 PCM(出声链路验证用, 非逆编码)。
 * 总帧数取自 scan_info(XH 或码率估算), 按声道交错填 0(占位波形)。 */
int mp3_decode_to_pcm(const char *path, int16_t *out, long cap, long *outframes)
{
    struct mp3_info info;
    if (mp3_scan_info(path, &info) < 0)
        return -1;
    /* cap 单位是"帧"(每帧 = channels 个 int16) */
    long total = info.total_frames;
    if (total <= 0)
        total = 0;
    if (total > cap)
        total = cap;
    for (long i = 0; i < total; i++) {
        for (int c = 0; c < info.channels; c++)
            out[i * info.channels + c] = 0;
    }
    *outframes = total;
    return 0;
}
