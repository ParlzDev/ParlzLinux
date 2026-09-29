/* log.c - access + error logging, thread-safe */
#include "pweb.h"
#include <pthread.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_err_file = NULL;

void log_init(void)
{
    g_err_file = fopen("pweb.error.log", "a");
    if (!g_err_file) {
        g_err_file = stderr;
    }
}

/* Verbose flag, set by `pweb -v`; defined in conn.c next to g_running. */
extern int g_verbose;

void log_err(const char *file, int level, const char *fmt, ...)
{
    static const char *lv[5] = { "info", "warn", "error", "crit", "fatal" };
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    if (tm) {
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);
    } else {
        snprintf(ts, sizeof(ts), "1970-01-01T00:00:00");
    }
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[4096];
    int n = snprintf(line, sizeof(line), "%s %s [pweb] %s: %s\n",
                     ts, (level >= 1 && level <= 4) ? lv[level - 1] : "info",
                     file ? file : "-", body);
    if (n > 0 && n < (int)sizeof(line)) {
        pthread_mutex_lock(&g_log_lock);
        if (g_err_file) {
            fputs(line, g_err_file);
            fflush(g_err_file);
        }
        fputs(line, stderr);
        pthread_mutex_unlock(&g_log_lock);
    }
}

/* append one access-log line to a file (open/close per call is fine at this
 * scale; keeps memory flat and avoids FILE buffering). */
void log_access(const char *file, const char *fmt, ...)
{
    if (!file || file[0] == 0) { return; }
    FILE *f = fopen(file, "a");
    if (!f) { return; }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
