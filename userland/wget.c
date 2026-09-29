/* Parlz wget: 上游 GNU wget 1.6 移植版 CLI。
 * 协议/TLS/进度由 wget 官方源码 + OpenSSL 3.5.8 静态库提供;
 * 动态产物需 /lib64/ld-linux + /lib/toolchain, initramfs 已含。 */
#include "http_client.h"
int main(int argc, char **argv) { return http_cli(argc, argv, 1); }
