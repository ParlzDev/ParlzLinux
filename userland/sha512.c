/* sha512.c - 自包含 SHA-512(FIPS 180-4)+ 加盐口令哈希。
 * 从 TLS-SSH PazeSHA512 移植为独立版(不依赖 Paze 头),静态自包含,
 * 供 login.c 存储/校验口令。
 *
 * 两种哈希:
 *   - 一次性 SHA-512:口令+盐直接摘要(仅用于内部快速路径,不推荐长期存储)
 *   - 加盐迭代口令哈希(salted-iter):$parlz-sha512$<iters>$<salt>$<hex>
 *     反复摘要 N 轮拉伸成时间成本,慢到暴力破解不划算。格式自研,
 *     不与 crypt(3) 互操作 —— 本系统内自洽即可(login 自己生成、自己校验)。
 *
 * 头文件 sha512.h 声明对外接口。
 */
#define _GNU_SOURCE
#include "sha512.h"
#include <string.h>

/* ---------- 常量 ---------- */
static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c712353bULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};

#define RR(x,n)  (((x) >> (n)) | ((x) << (64-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define BS0(x) (RR(x,28)^RR(x,34)^RR(x,39))
#define BS1(x) (RR(x,14)^RR(x,18)^RR(x,41))
#define ss0(x) (RR(x,1)^RR(x,8)^((x)>>7))
#define ss1(x) (RR(x,19)^RR(x,61)^((x)>>6))

static uint64_t load64be(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void store64be(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--)
        p[i] = (uint8_t)(v & 0xff), v >>= 8;
}

static void sha512_compress(uint64_t h[8], const uint8_t blk[128])
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = load64be(blk + 8 * i);
    for (int i = 16; i < 80; i++)
        w[i] = ss1(w[i-2]) + w[i-7] + ss0(w[i-15]) + w[i-16];

    uint64_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = hh + BS1(e) + CH(e,f,g) + K[i] + w[i];
        uint64_t t2 = BS0(a) + MAJ(a,b,c);
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void sha512_init(sha512_ctx_t *c)
{
    c->h[0]=0x6a09e667f3bcc908ULL; c->h[1]=0xbb67ae8584caa73bULL;
    c->h[2]=0x3c6ef372fe94f82bULL; c->h[3]=0xa54ff53a5f1d36f1ULL;
    c->h[4]=0x510e527fade682d1ULL; c->h[5]=0x9b05688c2b3e6c1fULL;
    c->h[6]=0x1f83d9abfb41bd6bULL; c->h[7]=0x5be0cd19137e2179ULL;
    c->len_lo=0; c->len_hi=0; c->blen=0;
}

void sha512_update(sha512_ctx_t *c, const void *data, size_t n)
{
    const uint8_t *d = data;
    uint64_t add = (uint64_t)n;
    uint64_t newlo = c->len_lo + add;
    if (newlo < c->len_lo)
        c->len_hi++;
    c->len_lo = newlo;

    if (c->blen) {
        size_t need = 128 - c->blen;
        size_t take = n < need ? n : need;
        memcpy(c->buf + c->blen, d, take);
        c->blen += take; d += take; n -= take;
        if (c->blen == 128) {
            sha512_compress(c->h, c->buf);
            c->blen = 0;
        }
    }
    while (n >= 128) {
        sha512_compress(c->h, d);
        d += 128; n -= 128;
    }
    if (n) {
        memcpy(c->buf, d, n);
        c->blen = n;
    }
}

void sha512_final(sha512_ctx_t *c, uint8_t out[64])
{
    uint64_t lo = c->len_lo, hi = c->len_hi;
    uint64_t bits_lo = lo << 3;
    uint64_t bits_hi = (lo >> 61) | (hi << 3);
    c->buf[c->blen++] = 0x80;
    if (c->blen > 112) {
        while (c->blen < 128) c->buf[c->blen++] = 0;
        sha512_compress(c->h, c->buf);
        c->blen = 0;
    }
    while (c->blen < 112)
        c->buf[c->blen++] = 0;
    for (int i = 7; i >= 0; i--)
        c->buf[112 + (7-i)] = (uint8_t)(bits_hi >> (8*i));
    for (int i = 7; i >= 0; i--)
        c->buf[120 + (7-i)] = (uint8_t)(bits_lo >> (8*i));
    sha512_compress(c->h, c->buf);
    for (int i = 0; i < 8; i++)
        store64be(out + 8*i, c->h[i]);
}

int sha512_hex(const void *data, size_t n, char out[128])
{
    uint8_t d[64];
    sha512_ctx_t c;
    sha512_init(&c);
    sha512_update(&c, data, n);
    sha512_final(&c, d);
    for (int i = 0; i < 64; i++)
        sprintf(out + 2*i, "%02x", d[i]);
    out[128] = 0;
    return 0;
}

/* 生成 16 字节随机盐,hex 化(32 字符) */
int sha512_random_salt(char salt[32])
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    uint8_t raw[16];
    size_t got = fread(raw, 1, 16, f);
    fclose(f);
    if (got != 16)
        return -1;
    for (int i = 0; i < 16; i++)
        sprintf(salt + 2*i, "%02x", raw[i]);
    salt[32] = 0;
    return 0;
}

/* 加盐迭代口令哈希。
 * 输出格式: $parlz-sha512$<iters>$<salt>$<hex512>
 * 算法: H = sha512(salt + password); 重复 iters 次: H = sha512(H + salt);
 * 取 H 的 hex。iters 越大越慢 —— 抗暴力破解。
 * 返回 0 成功,-1 失败。out 需 >= 32+33+64+16 ≈ 144 字节。 */
int sha512_crypt_password(const char *password, const char *salt,
                          int iters, char out[128])
{
    uint8_t h[64];
    sha512_ctx_t c;
    /* H = sha512(salt + password) */
    sha512_init(&c);
    sha512_update(&c, salt, strlen(salt));
    sha512_update(&c, password, strlen(password));
    sha512_final(&c, h);
    /* 迭代拉伸 */
    for (int i = 0; i < iters; i++) {
        sha512_init(&c);
        sha512_update(&c, h, 64);
        sha512_update(&c, salt, strlen(salt));
        sha512_final(&c, h);
    }
    char hex[128];
    for (int i = 0; i < 64; i++)
        sprintf(hex + 2*i, "%02x", h[i]);
    hex[128] = 0;
    snprintf(out, 128, "$parlz-sha512$%d$%s$%s", iters, salt, hex);
    return 0;
}

/* 校验口令是否匹配已存哈希串。
 * 格式 $parlz-sha512$<iters>$<salt>$<hex>;按同样算法重算并逐字符比对。
 * 返回 1=匹配, 0=不匹配/格式错。 */
int sha512_crypt_verify(const char *password, const char *stored)
{
    const char *p = stored;
    if (strncmp(p, "$parlz-sha512$", 14) != 0)
        return 0;
    p += 14;
    char num[32] = "";
    int iters = 0;
    size_t nl = 0;
    while (p[nl] && p[nl] != '$' && nl < 31)
        num[nl++] = p[nl++];
    num[nl] = 0;
    if (nl == 0 || p[nl] != '$')
        return 0;
    iters = atoi(num);
    if (iters < 1)
        iters = 1;
    p += nl + 1;
    const char *salt = p;
    const char *dollar = strchr(salt, '$');
    if (!dollar)
        return 0;
    size_t slen = dollar - salt;
    /* 存 hex */
    const char *stored_hex = dollar + 1;

    /* 重算 */
    char saltbuf[64];
    if (slen >= sizeof saltbuf)
        return 0;
    memcpy(saltbuf, salt, slen);
    saltbuf[slen] = 0;
    uint8_t h[64];
    sha512_ctx_t c;
    sha512_init(&c);
    sha512_update(&c, saltbuf, slen);
    sha512_update(&c, password, strlen(password));
    sha512_final(&c, h);
    for (int i = 0; i < iters; i++) {
        sha512_init(&c);
        sha512_update(&c, h, 64);
        sha512_update(&c, saltbuf, slen);
        sha512_final(&c, h);
    }
    char hex[128];
    for (int i = 0; i < 64; i++)
        sprintf(hex + 2*i, "%02x", h[i]);
    hex[128] = 0;
    return strcmp(hex, stored_hex) == 0;
}
