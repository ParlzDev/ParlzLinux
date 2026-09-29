/* free.c - 从 /proc/meminfo 读取内存信息。
 * 输出布局:
 *               total      used     free   shared  buffers
 * Mem:          <T>       <U>      <F>    <S>     <B>      (M)
 * Swap:         <st>     <su>      <sf>                    (M)
 *
 * 数值来自 /proc/meminfo(kB),换算成 MB 保留一位小数。
 * used = total - free - buffers - cached(与常规 free(1) 口径一致,
 * 不含 tmpfs 页缓存)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    unsigned long v = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        /* "MemTotal: 476968 kB":键名后紧跟 ':' 才匹配(避免
         * MemAvailable 误匹配 MemAvailable 之外的键)。 */
        if (strncmp(line, key, strlen(key)) == 0 &&
            line[strlen(key)] == ':') {
            char *p = line + strlen(key) + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            v = strtoul(p, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static void mb(unsigned long k)
{
    printf("%7.1f", k / 1024.0);
}

int main(void)
{
    unsigned long total = kb("MemTotal");
    unsigned long free_ = kb("MemFree");
    unsigned long avail = kb("MemAvailable");
    unsigned long buffers = kb("Buffers");
    unsigned long cached  = kb("Cached");
    unsigned long swap_total = kb("SwapTotal");
    unsigned long swap_free  = kb("SwapFree");

    /* used:total - free - buffers - cached(不低于 0) */
    unsigned long used = (total > free_ + buffers + cached)
                         ? total - free_ - buffers - cached : 0;

    printf("              total      used     free   shared  buffers\n");
    printf("Mem: ");
    mb(total);
    printf(" ");
    mb(used);
    printf(" ");
    mb(free_);
    printf("      0.0     ");
    mb(buffers);
    printf("\n");

    if (swap_total > 0) {
        unsigned long swap_used = (swap_total > swap_free)
                                  ? swap_total - swap_free : 0;
        printf("Swap: ");
        mb(swap_total);
        printf(" ");
        mb(swap_used);
        printf(" ");
        mb(swap_free);
        printf("\n");
    }

    printf("\navailable: %.1fM\n", avail / 1024.0);
    return 0;
}
