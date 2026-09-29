/* flac_decode.h - FLAC 音频解码器(纯 C, 自研, 无外部依赖)。
 * 支持 FLAC 规范常见参数: PCM 8/16/24/32 bit, 1..8 声道,
 * Verbatim / Fixed(1..8 阶) / LPC(1..32 阶) 子帧,
 * 残差 Fixed/LPC(非交织 + 交织), 变长 Rice 码。
 * 输出: 交错 int32 PCM(32 位容器, 数值已按样本位深量化)。
 * 调用方自行 shift 到目标位深(16-bit 时 >>16)。
 */
#ifndef FLAC_DECODE_H
#define FLAC_DECODE_H
#include <stdint.h>
#include <stddef.h>

#define FLAC_MAX_CHANNELS 8
#define FLAC_MAX_FRAME_SLOTS 65536

/* FLAC 流元数据(由 flac_stream_parse 解析) */
struct flac_stream {
    const uint8_t *data;  /* 音频数据起点(跳过 metadata) */
    size_t size;
    unsigned sample_rate;
    unsigned channels;
    unsigned sample_bits; /* 每样本有效位宽(8..32) */
    unsigned blocksize;   /* 帧样本数(512..65536) */
};

/* 位流缓冲 */
struct flac_bitbuf {
    const uint8_t *p;
    const uint8_t *end;
    uint32_t cache;
    int nbits;
};

struct flac_decoder {
    const struct flac_stream *st;
    struct flac_bitbuf bb;
    int32_t lpcc[FLAC_MAX_CHANNELS][33]; /* LPC 系数(逐声道, 阶 0..32) */
};

/* 从 buf[0..size) 的 FLAC 文件解析流元数据。返回 0 成功, <0 失败 */
int flac_stream_parse(const uint8_t *buf, size_t size, struct flac_stream *st);

int flac_decoder_init(struct flac_decoder *dec, const struct flac_stream *st);

/* 解码 1 帧到 out(交错, 帧内 channels 个 int32)。
 * 返回本帧样本数; 0 = 流结束; <0 = 错误。 */
int flac_decoder_decode(struct flac_decoder *dec, int32_t *out, int cap);

#endif
