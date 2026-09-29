/* ps.c - 列出 /proc 下的进程。用法: ps
 * 输出: PID PPID STAT RSS KB CMD
 * 读 /proc/<pid>/stat、/proc/<pid>/status,遍历 /proc 找数字目录。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>

static int read_ppid(const char *pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/status", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    int ppid = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "PPid:", 5)) {
            ppid = atoi(line + 5);
            break;
        }
    }
    fclose(f);
    return ppid;
}

static long read_rss_kb(const char *pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/status", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "VmRSS:", 6)) {
            rss = atol(line + 6);
            break;
        }
    }
    fclose(f);
    return rss;
}

static void read_cmd(const char *pid, char *out, int outsz)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/cmdline", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    int c;
    int w = 0;
    while ((c = fgetc(f)) != EOF && w < outsz - 1) {
        if (c == 0) {
            if (w > 0)
                out[w++] = ' ';
        } else {
            out[w++] = (char)c;
        }
    }
    out[w] = 0;
    fclose(f);
}

int main(void)
{
    DIR *d = opendir("/proc");
    if (!d) {
        perror("/proc");
        return 1;
    }
    printf("%-8s %-6s %-6s %-10s %s\n", "PID", "PPID", "STAT", "RSS(KB)", "CMD");
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        int pid = atoi(e->d_name);
        int ppid = read_ppid(e->d_name);
        long rss = read_rss_kb(e->d_name);
        char stat_c = '?';
        char path[64];
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[512];
            if (fgets(line, sizeof line, f)) {
                char *p = strrchr(line, ')');
                if (p && p[1])
                    stat_c = p[2];
            }
            fclose(f);
        }
        char cmd[256];
        read_cmd(e->d_name, cmd, sizeof cmd);
        printf("%-8d %-6d %-6c %-10ld %s\n",
               pid, ppid, stat_c, rss, cmd);
    }
    closedir(d);
    return 0;
}
