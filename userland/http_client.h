/* Parlz 共享 HTTP/HTTPS 下载器；错误仅由命令行入口写 stderr。 */
#ifndef PARLZ_HTTP_CLIENT_H
#define PARLZ_HTTP_CLIENT_H
#include <stdint.h>
struct http_options {
    const char *url, *output, *cacert;
    int follow, insecure, fail_http, timeout;
};
struct http_result {
    int status;
    uint64_t bytes;
    char error[256];
};
/* 下载进度回调:随响应体逐块(got) 与 声明总量(total,0=未知)。
 * 仅由 http_cli 内部使用;回调实现须写 stderr 且自节流。 */
typedef void (*http_progress_cb)(uint64_t got, uint64_t total, void *ud);
struct http_progress {
    http_progress_cb cb;
    void *ud;
};
/* output 为 NULL 或 "-" 时写 stdout；文件下载成功后才原子替换目标。
 * timeout 是包含 DNS、重定向和全部 I/O 的总秒数，必须为 1..86400。
 * progress 非 NULL 且 progress->cb 非 NULL 时,响应体逐块回调进度。
 * 返回 0 成功；非零为 curl 风格错误码。 */
int http_download(const struct http_options *, struct http_result *,
                  const struct http_progress *);
int http_cli(int argc, char **argv, int wget_mode);
int http_cli(int argc, char **argv, int wget_mode);
#endif
