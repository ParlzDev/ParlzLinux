/* file.c - 显示文件类型(简易版)。
 * 用法: file <path>...
 * 按 magic 识别:bzImage/HdrS/ELF/脚本/设备节点/目录/普通文件。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char *describe(const char *path, struct stat *st)
{
    char m[8];
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(m, 1, 7, f);
        fclose(f);
        /* ELF 可执行 */
        if (n >= 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F')
            return "ELF executable";
        /* bzImage setup 头:"HdrS" 签名(偏移 0x202 前的 jump) */
        if (n >= 2 && m[0] == 0xEB && m[1] == 0x6A)
            return "bzImage kernel (Parlz setup)";
        /* shell 脚本 */
        if (n >= 2 && m[0] == '#' && m[1] == '!')
            return "script";
        /* gz */
        if (n >= 2 && (unsigned char)m[0] == 0x1f && (unsigned char)m[1] == 0x8b)
            return "gzip compressed";
    }
    /* 普通文件:读前 4KB,判断是否纯可打印 ASCII/UTF-8 文本。
     * 用字节级启发式:
     *   - 含 NUL → 二进制
     *   - 含 C0 控制字符(除 \t \n \r \f \v)→ 二进制
     *   - 其余字节若 >= 0x80,需构成合法 UTF-8 多字节序列;
     *     孤立的 0x80-0xBF 续字节或不合法首字节 → 二进制
     * 通过则按 ASCII 比例细分纯 ASCII / UTF-8 文本。 */
    if (S_ISREG(st->st_mode)) {
        FILE *f2 = fopen(path, "rb");
        if (f2) {
            char buf[4096];
            size_t read_n = fread(buf, 1, sizeof buf, f2);
            fclose(f2);
            unsigned char *p = (unsigned char *)buf;
            int is_ascii = 1, is_utf8 = 0, i = 0;
            while (i < (int)read_n) {
                unsigned char c = p[i];
                if (c == 0)
                    return "data (binary)";
                if (c >= 0x20 && c <= 0x7e) { i++; continue; }
                if (c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
                { i++; continue; }
                if (c < 0x20)
                    return "data (binary)";   /* 其它 C0 控制字符 */
                /* 0x80-0xFF:校验 UTF-8 序列 */
                if (c >= 0xC2 && c <= 0xDF) {          /* 2 字节 */
                    if (i + 1 >= (int)read_n)
                        break;
                    if ((p[i + 1] & 0xC0) != 0x80)
                        return "data (binary)";
                    i += 2; is_utf8 = 1; continue;
                }
                if (c >= 0xE0 && c <= 0xEF) {          /* 3 字节 */
                    if (i + 2 >= (int)read_n)
                        break;
                    if ((p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80)
                        return "data (binary)";
                    i += 3; is_utf8 = 1; continue;
                }
                if (c >= 0xF0 && c <= 0xF4) {          /* 4 字节 */
                    if (i + 3 >= (int)read_n)
                        break;
                    if ((p[i + 1] & 0xC0) != 0x80 ||
                        (p[i + 2] & 0xC0) != 0x80 ||
                        (p[i + 3] & 0xC0) != 0x80)
                        return "data (binary)";
                    i += 4; is_utf8 = 1; continue;
                }
                /* 0x80-0xC1 / 0xF5+ :非法(续字节无首字节,或超范围) */
                return "data (binary)";
            }
            if (is_utf8)
                return "text (UTF-8)";
            if (is_ascii)
                return "text (ASCII)";
            return "text";
        }
    }
    if (S_ISDIR(st->st_mode))
        return "directory";
    if (S_ISREG(st->st_mode) && st->st_size > 0)
        return "data (binary)";
    if (S_ISREG(st->st_mode))
        return "empty file";
    if (S_ISBLK(st->st_mode) || S_ISCHR(st->st_mode))
        return "device";
    if (S_ISLNK(st->st_mode))
        return "symlink";
    return "unknown";
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: file <path>...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) {
            perror(argv[i]);
            rc = 1;
            continue;
        }
        printf("%s: %s\n", argv[i], describe(argv[i], &st));
    }
    return rc;
}
