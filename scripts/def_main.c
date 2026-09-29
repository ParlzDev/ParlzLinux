/* def_main.c - 裸 DEFLATE 流解码测试入口(拼到 DEFLATE 段后编译) */
int main(void)
{
    FILE *f = fopen("/tmp/ctrl_deflate.bin", "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)sz);
    if (!fread(d, 1, (size_t)sz, f))
        return 1;
    fclose(f);
    size_t cap = (size_t)sz * 4 + 4096;
    unsigned char *dst = malloc(cap);
    long r = deflate_decompress(d, (size_t)sz, dst, cap);
    printf("deflate %s -> %ld B\n", r < 0 ? "FAIL" : "OK", r);
    if (r > 0)
        fwrite(dst, 1, (size_t)r, stdout);
    return 0;
}
