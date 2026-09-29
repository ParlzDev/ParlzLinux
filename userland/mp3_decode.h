/* mp3_decode.h - MP3 头解析 + 简化 PCM 切分 API(详见 mp3_decode.c)。 */
#ifndef MP3_DECODE_H
#define MP3_DECODE_H
#include <stdint.h>
struct mp3_info {
    int rate;        /* 采样率 Hz */
    int channels;
    int vbr;
    double total_secs;
    long total_frames;
};
int mp3_scan_info(const char *path, struct mp3_info *info);
int mp3_decode_to_pcm(const char *path, int16_t *out, long cap, long *outframes);
#endif
