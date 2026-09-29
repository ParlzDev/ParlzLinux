/* gen_mp3_test.c - 生成合法帧头的 MP3 测试文件(宿主侧验证 mp3_scan_info 用)
 * 帧头 0xFFFB (MPEG1 Layer3, 32000Hz, 128kbps, stereo), 帧长 576 字节。
 * 文件 = 1 个 Xing 信息帧 + N 个数据帧, Xing 里填总帧数, 供对照。 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "/tmp/t.mp3";
    int want_frames = argc > 2 ? atoi(argv[2]) : 120;
    if (want_frames < 2) want_frames = 120;
    int fsize = 576;  /* 128kbps 32000 L3: 144*128000/32000 = 576 */
    int n_frames = want_frames - 1;
    int total_frames = want_frames;
    int len = fsize * total_frames + 44;
    uint8_t *buf = malloc(len);
    memset(buf, 0, len);
    int off = 0;
    buf[off++]=0xFF; buf[off++]=0xFB; buf[off++]=0x90; buf[off++]=0x00;
    memcpy(buf+off, "Xing", 4); off += 4;
    buf[off++]=0; buf[off++]=0; buf[off++]=0; buf[off++]=1;
    buf[off++]=(uint8_t)((total_frames>>24)&0xFF);
    buf[off++]=(uint8_t)((total_frames>>16)&0xFF);
    buf[off++]=(uint8_t)((total_frames>>8)&0xFF);
    buf[off++]=(uint8_t)(total_frames&0xFF);
    while (off < fsize) buf[off++] = 0;
    for (int i = 0; i < n_frames; i++) {
        buf[off]=0xFF; buf[off+1]=0xFB; buf[off+2]=0x90; buf[off+3]=0x00;
        off += fsize;
    }
    int fd = open(out, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { perror(out); return 1; }
    int w = 0;
    while (w < off) { int nw = write(fd, buf+w, off-w); if (nw<=0) break; w += nw; }
    close(fd);
    printf("生成 %s: 帧头FFFB(MPEG1 L3 32000 stereo 128k), Xing 填 %d 帧\n", out, total_frames);
    free(buf);
    return 0;
}
