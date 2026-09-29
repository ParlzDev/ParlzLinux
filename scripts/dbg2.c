#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#define MAXSYM 288
struct bitbuf { const unsigned char *p; size_t n; size_t pos; unsigned reg; int bits; };
static unsigned bb_get(struct bitbuf *b, int cnt)
{
    while (b->bits < cnt) {
        if (b->pos >= b->n) return 0;
        b->reg |= (unsigned)b->p[b->pos++] << b->bits;
        b->bits += 8;
    }
    unsigned v = b->reg & ((1u << cnt) - 1);
    b->reg >>= cnt;
    b->bits -= cnt;
    return v;
}
static unsigned bb_peek(struct bitbuf *b, int cnt)
{
    unsigned sr = b->reg; int sb = b->bits; size_t sp = b->pos;
    unsigned v = bb_get(b, cnt);
    b->reg = sr; b->bits = sb; b->pos = sp;
    return v;
}
/* 打印诊断: 逐步读 3 个码长符号 */
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(sz);
    if (!fread(d, 1, sz, f)) return 1;
    fclose(f);
    struct bitbuf b = { .p = d, .n = sz, .pos = 0, .reg = 0, .bits = 0 };
    bb_get(&b, 1);  /* bfinal */
    bb_get(&b, 2);  /* btype */
    int hlit = bb_get(&b, 5) + 257;
    int hdist = bb_get(&b, 5) + 1;
    int hlen = bb_get(&b, 4) + 4;
    const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    unsigned char clens[19] = {0};
    for (int i = 0; i < hlen; i++)
        clens[order[i]] = bb_get(&b, 3);
    printf("hlit=%d hdist=%d hlen=%d\n", hlit, hdist, hlen);
    printf("clens (码长值->码长):");
    for (int i = 0; i < 19; i++)
        printf(" %d", clens[i]);
    printf("\n");
    /* 按"码长值升序"分配码值 (规范) */
    unsigned char hcount[32] = {0};
    for (int i = 0; i < 19; i++)
        hcount[clens[i]]++;
    unsigned start[16];
    start[1] = 0;
    for (int L = 2; L <= 15; L++)
        start[L] = (start[L-1] + hcount[L-1]) << 1;
    /* 码值按码长值升序分配(不是按 order 读取序!) */
    int symcode[19];  /* symcode[v] = 码长值 v 的码值 */
    memset(symcode, 0, sizeof symcode);
    /* 同码长组内按码长值升序分配 */
    unsigned nxt[16] = {0};
    for (int L = 1; L <= 15; L++)
        nxt[L] = start[L];
    for (int v = 0; v < 19; v++) {  /* 码长值 0..18 升序 */
        int L = clens[v];
        if (L >= 1 && L <= 15)
            symcode[v] = nxt[L]++;
    }
    printf("码长值->码值:");
    for (int v = 0; v < 19; v++)
        printf(" %d:%d", v, symcode[v]);
    printf("\n");
    /* 解码前 5 个码长符号, 显示每步 */
    for (int i = 0; i < 5; i++) {
        unsigned raw15 = bb_peek(&b, 15);
        int found = -1;
        for (int L = 15; L >= 1 && found < 0; L--) {
            unsigned rev = 0;
            for (int k = 0; k < L; k++)
                rev = (rev << 1) | ((raw15 >> k) & 1);
            for (int v = 0; v < 19; v++)
                if (clens[v] == L && symcode[v] == rev) {
                    found = v;
                    break;
                }
        }
        if (found >= 0)
            bb_get(&b, clens[found]);
        printf("  sym#%d: raw15=0x%04x found=码长值 %d\n", i, raw15, found);
    }
    return 0;
}
