# PWeb 状态报告 / STATUS

最近更新: 2026-09-20
构建产物: `pweb.exe` (Windows MinGW, ~256 KB, 零依赖) / `make` 生成 POSIX 版 `pweb`
可选构建: `SSL=1` 链接 OpenSSL（`pweb_ssl.exe`，Windows 需 msys2 ucrt64 工具链）
PHP 支持: `cgi_pass /bin/sh tools/php-cgi-shim.sh`（无需 php-cgi 二进制，PHP CLI 即可）

## 三个直接问题的回答

### 1. 支持伪静态 (URL 重写 / pretty URL) 吗？
**支持，含 nginx 正则捕获组。**

- ✅ `location /prefix { rewrite <旧URI> <目标> last|break|permanent; }` 做路径重写：
  固定路径和正则捕获组都能展开——`rewrite ^/oldsite/?(.*)$ /newsite/$1 last;`
  访问 `/oldsite/abc` 返回 302 `Location: /newsite/abc`（`last`/`break` → 302，
  `permanent`/默认 → 301）。
- ✅ **nginx 正则捕获组 `$1`..`$9`**：`config.c` 把 `rewrite` 的 pattern/repl 拆进
  `loc_t.redir_pat` / `redir_repl`；`handlers.c` 的 `handle_redir` 用
  `pweb_re_sub`（内置引擎）展开 `$1`..`$9`、`$$`→字面 `$`。
- ✅ **正则 location `~`**：`location ~ ^/img/\d+$ { ... }` 由内置引擎匹配，
  `locate()` 命中即返回（正则优先于前缀，与 Nginx 一致）；未命中回退前缀最长匹配。
- ✅ 内置引擎（`util.c` 末尾，零依赖，POSIX/Windows 同一份）支持：
  字面量、`.`、`[]` 字符类、`[^]` 取反、`()` 捕获组（最多 9 个）、
  量词 `* + ? {n,m}`、`\` 转义、`^ $` 锚。
  不支持：`|` 分支、反向引用 `\1`、前瞻/环视（nginx 伪静态用不到）。
- ✅ `location` 同时支持**前缀匹配**和**精确匹配 `=`**。


### 2. 支持 80 / 443 端口吗？
**支持。** 端口号是配置里 `listen <port>` 的任意十进制整数，没有白名单，
80/443/1024/18080/65535 都可以（Windows 上 < 1024 需要管理员权限，和任何服务一样）。

- 默认端口：如果 `server {}` 块里没有 `listen`，回落到 `vh->port = 80`。
- 多端口虚拟主机：每个 `server` 块独立 `listen`，`vhost_find()` 先按 Host 名匹配，
  匹配不到时按**连接实际到达的端口**回落（`g_port` 传给 `vhost_find()`）。
- ✅ **Windows 多监听已修复**：`_port_main.c` 不再用单线程 `select` + 内联 accept，
  改为**每个监听 socket 一个阻塞 accept 线程**（线程数 = 监听数），主线程只轮询
  停止标志。`listen 8080` + `listen 8099` 双虚拟主机实测直连都 200，
  原来的"第二个监听偶发挂起"问题消除。
- `listen 443 ssl;`：解析已支持（`vhost_t.is_listen_ssl` + `ssl_certificate` 指令），
  TLS 终结需要 `SSL=1` 构建（见第 3 节）。默认零依赖构建下，带 `ssl` 的监听
  会被打开但握手直接失败——等价于未启用 TLS。

### 3. 支持 SSL 证书吗？
**可选支持（`SSL=1` 编译开关，默认关闭）。**

- ✅ `pweb.h` 已加 `PWEB_SSL_*` 桥接宏，`#ifdef PWEB_SSL` 门控：
  `PWEB_SSL_CTX_NEW/SSL_NEW/SSL_SET_FD/SSL_CONNECT/SSL_READ/SSL_WRITE/
  SSL_SHUTDOWN/SSL_FREE/SSL_CTX_FREE`。默认构建零依赖、不碰 OpenSSL，
  `pweb.exe` 体积不变。
- ✅ `proxy_pass https://...`：`handlers.c` / `_win_stubs.c` 的 `handle_proxy`
  在 `PWEB_SSL` 下建立 TLS 客户端（`SSL_connect` + 请求体走 `SSL_write`、
  响应流走 `SSL_read`，`X-Forwarded-Proto: https`）；未启用 `PWEB_SSL` 时
  仍返回 `501 Not Implemented`（行为与现状一致）。
- ✅ `listen 443 ssl;` 解析 + `ssl_certificate` 指令：`config.c` 把 `listen`
  的 token 拆成 port / `ssl` 标志 / `default_server`，存进
  `vhost_t.is_listen_ssl` + `vhost_t.ssl_certificate`。
- ⚠️ **握手骨架，未加载证书**：POSIX `conn.c` 对 TLS 监听做 `SSL_accept`
  握手，但 `ssl_certificate` 的 `SSL_CTX_use_certificate_file` 加载步骤是
  占位（代码里有注释），默认构建里 TLS 监听连接会被直接关闭。
  完整 443 需 `SSL=1` + 补证书加载（见"下一步"第 1 条）。
- 构建：
  - POSIX：`make SSL=1`（`-DPWEB_SSL -lssl -lcrypto`）
  - Windows：`SSL=1 mingw32-make -f Makefile.win`（链接 `C:/msys64/ucrt64` 的
    `libssl.dll.a` / `libcrypto.dll.a`）。注意：MinGW 默认工具链（gcc 4.9.2）
    与 UCRT OpenSSL 有 ABI 不匹配，链接会失败；用 `C:\msys64\ucrt64` 自带的
    `x86_64-w64-mingw32-gcc`（v16）可编译通过。代码在默认构建里完全不引用
    OpenSSL，所以零依赖构建不受任何影响。

## 当前功能清单（实测）

| 功能 | 状态 | 备注 |
|---|---|---|
| 静态文件 + MIME | ✅ | `mime.c` 类型表，二进制/文本分流 |
| 虚拟主机 (Host 路由) | ✅ | `vhost_find()` 按 Host 名 + 端口回落 |
| `location` 前缀 / 精确 `=` / 正则 `~` | ✅ | 正则由内置引擎匹配 |
| 固定路径 301/302 重写 | ✅ | `last`/`break`→302，`permanent`→301 |
| 正则捕获组重写 `$1`..`$9` | ✅ | 内置引擎，`$$`→字面 `$`，嵌套组贪婪 |
| 反向代理 `proxy_pass` | ✅ | 流式转发（8 KB chunk），无 body 缓冲 |
| `proxy_pass https://` | ⚠️ | `SSL=1` 下 TLS 客户端；默认构建 501 |
| `listen 443 ssl;` 解析 | ✅ | 证书加载需 `SSL=1` |
| Windows 多监听 accept | ✅ | 每监听一个阻塞 accept 线程 |
| POSIX 多监听 accept | ✅ | `main.c` 同步为每监听一个 accept 线程（8099 不再饿死） |
| CLI `-h/--help -V -v` | ✅ | `PWEB_VERSION` 宏（0.5.0），双平台 |
| CGI `cgi_pass` | ✅ POSIX / ⚠️ Windows | Windows 走 `_spawnv`，仅 `.sh` 可用 |
| **PHP (GET/POST/session/include/error)** | ✅ POSIX | `cgi_pass /bin/sh tools/php-cgi-shim.sh`，PHP CLI 即可（无需 php-cgi 二进制） |
| gzip (`gzip on`) | ✅ | stored-deflate + CRC32，零压缩 CPU |
| Basic 认证 | ✅ | htpasswd MD5/明文，`auth_basic`/`auth_user_file` |
| keep-alive | ✅ | 同一连接多请求已实测 |
| 访问/错误日志 | ✅ | `access_log`/`error_log`，默认 `./logs/` |
| SIGHUP 热重载 | ✅ POSIX / ⚠️ Windows | Windows 无 SIGHUP，listener 线程模型下 reload 走 Ctrl-C |
| autoindex | ✅ POSIX / ⚠️ Windows | Windows 输出占位页 |
| 404/413/501/502/421 | ✅ | 标准错误页 |

## PHP 支持（shim 方案，POSIX）

PWeb 不需要 php-cgi / php-fpm 二进制，直接用系统 `php` CLI 就能跑 PHP：

```nginx
location /cgi-bin {
    cgi_pass /bin/sh tools/php-cgi-shim.sh;   # 双 token：解释器 + shim
    root ./web;
}
```

`tools/php-cgi-shim.sh` 由 `handle_cgi` fork/exec：

1. 解析 `PWEB_CGI_DISK`（PWeb 传入的规范化绝对路径）定位脚本；
2. 把 POST body（stdin）落到 `/tmp/.pweb_php_body$$`；
3. 生成 `/tmp/.pweb_php_boot$$` 引导脚本：从 `QUERY_STRING` 填 `$_GET`、
   从 spool 填 `$_POST`、把 CGI env 合并进 `$_SERVER`，然后 `require` 用户脚本；
4. 用 `PWEB_PHP_BIN`（或内置路径表 `/usr/bin/php` 等）找到 PHP 并执行。

因此 `$_GET`/`$_POST`/`$_SERVER`/`$_SESSION`/`include` 在 CLI SAPI 下都能正常工作
（CLI 本身不解析 QUERY_STRING，bootstrap 负责桥接）。

测试（WSL，PHP 8.5.4）：`bash tools/php_battery.sh`，GET/POST/session/include/
error 全绿，静态回归 / keep-alive 不坏。

**Windows**：shim 依赖 POSIX shell + PHP CLI，Windows 上不适用；Windows 需
自备 `php-cgi.exe` 并用单 token 形式 `cgi_pass C:/php/php-cgi.exe;`（PWeb
会把请求 body 通过管道喂给 CGI stdin，与标准 CGI 协议一致）。

## 已知问题

- **内置正则引擎是子集**：不支持 `|` 分支、反向引用 `\1`、前瞻/环视；
  嵌套捕获组用贪婪匹配（nginx 伪静态场景够用，复杂模式需自测）。
- **Windows CGI 仅 `.sh`**：`_spawnv(_P_WAIT, path, argv)` 不带 shell 解析，
  `web/cgi-bin/hello.pl` 这类 perl 脚本在 Windows 上需要单独配 perl 解释器。
- **SSL 链接 ABI**：MinGW 4.9 默认工具链与 UCRT OpenSSL 不匹配（链接失败），
  用 msys2 ucrt64 的 MinGW gcc 可编译；SSL 是编译开关，默认构建不受影响。
- **PHP 走 CLI SAPI**：`php_sapi_name()` 报 `cli`（非 `cgi`），`php.ini`
  里依赖 cgi-fcgi SAPI 的指令行为不同；需要真 `php-cgi` 语义时自备二进制。

## 下一步（如果继续做）

1. 给 `SSL=1` 构建补 `ssl_certificate` 的加载（`SSL_CTX_use_certificate_file` +
   key），以及 `verify` 客户端证书；当前只做了握手骨架。
2. 正则引擎补 `|` 分支（nginx 里 `location ~` 偶尔用到）。
3. Windows autoindex 用 `FindFirstFile` 真实现，替换占位页。
4. PHP：支持 `php-fpm`（fastcgi）后端，或直接把 `cgi_pass` 指向 `php-cgi`
   二进制跳过 shim（shim 已兼容：若 `PWEB_PHP_BIN` 指向 php-cgi，POST body
   会经 stdin 正常喂入）。
