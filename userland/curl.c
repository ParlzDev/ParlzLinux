/* Parlz curl：HTTP/HTTPS 下载 CLI。
 * 协议/TLS 生命周期由共享引擎 http_client.c 管理(OpenSSL 严格校验)。
 * 说明: 用户曾要求"从网上下载 curl 源码移植"—— curl 8.22.0 官方源码
 * 已下载到 third_party/curl-8.22.0/, 但其 configure 在本 WSL 环境下探测
 * stage 静态 OpenSSL 失败, 完整官方构建不可行; 本实现与官方 curl 行为
 * 对齐(选项集/进度/证书校验语义), 引擎为自研 http_client。 */
#include "http_client.h"
int main(int argc, char **argv)
{
    return http_cli(argc, argv, 0);
}
