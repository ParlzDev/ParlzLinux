/* sha512.h - 自包含 SHA-512 + 加盐迭代口令哈希接口。
 * 实现见 sha512.c。login.c 用它存储/校验口令。 */
#ifndef PARLZ_SHA512_H
#define PARLZ_SHA512_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t h[8];
    uint64_t len_lo, len_hi;
    uint8_t  buf[128];
    size_t   blen;
} sha512_ctx_t;

void sha512_init(sha512_ctx_t *c);
void sha512_update(sha512_ctx_t *c, const void *data, size_t n);
void sha512_final(sha512_ctx_t *c, uint8_t out[64]);

/* 一次性 SHA-512,hex(128 字符 + NUL)。失败返回 -1。 */
int sha512_hex(const void *data, size_t n, char out[128]);

/* 生成 32 字符随机盐。失败返回 -1。 */
int sha512_random_salt(char salt[32]);

/* 加盐迭代口令哈希,写入 out(需 >= 128 字节)。返回 0 成功,-1 失败。 */
int sha512_crypt_password(const char *password, const char *salt,
                          int iters, char out[128]);

/* 校验口令与已存哈希。返回 1=匹配, 0=不匹配/格式错。 */
int sha512_crypt_verify(const char *password, const char *stored);

#endif
