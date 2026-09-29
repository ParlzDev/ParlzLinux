# PWeb

A lightweight, portable C web server — a single ~8k-loc codebase with an
nginx-like config. It ships static files, 301 rewrites, reverse proxies,
CGI, gzip, basic auth, and virtual hosts, while running comfortably on a
0.5 core / 512 MB box.

## Features (≈ 60% of Nginx/Apache common surface)

| Feature | Status |
|---|---|
| Static file serving + MIME types | ✅ |
| Virtual hosts (multi-server, Host routing) | ✅ |
| `location` blocks (prefix + exact `=` + regex `~`) | ✅ |
| 301/302 rewrites with nginx capture groups (`$1`..`$9`) | ✅ (built-in regex engine) |
| Reverse proxy (`proxy_pass`) | ✅ (streaming, no buffering) |
| `proxy_pass https://` (TLS upstream) | ⚠️ `SSL=1` build only |
| `listen ... ssl;` + `ssl_certificate` | ⚠️ parsed; TLS needs `SSL=1` |
| CGI (`cgi_pass` → sh/perl/python) | ✅ POSIX / ⚠️ Windows (`.sh` via `_spawnv`) |
| gzip compression (`gzip on`, `gzip_min_length`) | ✅ (stored-deflate, zero CPU cost) |
| Basic auth (htpasswd, MD5 or plain) | ✅ |
| HTTP/1.1 keep-alive | ✅ |
| Windows multi-listener (one accept thread per vhost) | ✅ |
| Access + error logging | ✅ |
| Config hot-reload (SIGHUP POSIX / Ctrl-C Windows) | ✅ |
| `autoindex` | ✅ POSIX (Windows: placeholder page) |
| `client_max_body_size` | ✅ |
| 404/405/413/501/502 error pages | ✅ |

## Build

### POSIX (Linux / macOS)

```sh
make            # -> ./pweb  (zero-dependency)
./pweb pweb.conf
make SSL=1      # optional: link OpenSSL, enables proxy_pass https:// + listen ssl
```

### Windows (MinGW cross-compile from Linux)

```sh
F:/Path/MinGW64/bin/x86_64-w64-mingw32-gcc.exe \
  -std=c11 -O1 -Wall \
  _port_main.c _port_conn.c _port_config.c _port_http_parser.c \
  _port_handlers.c _port_util.c _port_mime.c _port_log.c _win_stubs.c \
  -I F:/Path/MinGW64/x86_64-w64-mingw32/include \
  -lws2_32 -o pweb.exe
./pweb.exe pweb.conf
```

可选 TLS 构建（`SSL=1`）：代码在 `pweb.h` 里由 `PWEB_SSL` 门控，默认构建
完全不引用 OpenSSL（零依赖、体积不变）。启用时加 `-DPWEB_SSL` 并链接
`C:/msys64/ucrt64` 的 `libssl.dll.a` / `libcrypto.dll.a`（需配套的
msys2 ucrt64 MinGW gcc，默认 4.9.2 工具链有 ABI 不匹配会链接失败）：

```sh
C:/msys64/ucrt64/bin/x86_64-w64-mingw32-gcc.exe \
  -std=c11 -O1 -DPWEB_SSL \
  _port_main.c ... _win_stubs.c -lssl -lcrypto -lws2_32 -o pweb_ssl.exe
```

The `_port_*.c` files are generated from the POSIX sources; the platform
differences (Winsock vs. POSIX sockets, `_beginthreadex` vs. `pthread`,
`closesocket` vs. `close`) are all bridged by `PWEB_*` macros in `pweb.h`.
A single source tree builds on both.

## Config

See `pweb.conf` for a full working example:

```nginx
events { worker_connections 1024; }
http {
    gzip on;
    gzip_min_length 1024;

    server {
        listen 8080 default_server;
        server_name localhost;
        root  ./web;
        autoindex on;

        location /oldsite { rewrite ^/oldsite/?(.*)$ /newsite/$1 last; }
        location /api     { proxy_pass http://127.0.0.1:8099; }
        location /cgi-bin { cgi_pass /bin/sh; root ./web; }
        location /        { root ./web; }

        # auth_basic "Restricted";
        # auth_user_file ./web/.htpasswd;
    }

    server { listen 8099; server_name api.localhost; root ./web; }
}
```

Supported directives: `listen [ssl]`, `ssl_certificate`, `server_name`,
`root`, `index`, `autoindex`, `gzip`, `gzip_min_length`,
`keepalive_timeout`, `client_max_body_size`, `access_log`, `error_log`,
`auth_basic`, `auth_user_file`, `location [~regex| =]`, `rewrite`,
`proxy_pass`, `cgi_pass`.

`rewrite` takes an optional 3rd token `last` / `break` (→ 302) or
`permanent` (→ 301, default). The replacement supports `$1`..`$9`
capture-group expansion and `$$` for a literal `$`.

## Runtime behaviour

- **One worker thread per connection; one blocking-accept thread per
  listener** (Windows) — flat memory, no per-conn buffering, no starvation
  of secondary listeners.
- **gzip via stored-deflate blocks** — zero compression CPU cost; the
  response is just CRC32 + raw bytes. Good for large static assets where
  the network transfer dominates.
- **Streaming proxy** — no body buffering; bytes are relayed as they arrive.
- **CGI** — `PWEB_FORK`/`PWEB_EXECV` (POSIX) or `_spawnv` (Windows); env
  vars set per-request via `PWEB_SENENV`.

## File layout

```
pweb.h          shared types + PWEB_* platform bridges
main.c          entry, signals, bind/listen, accept loop, SIGHUP reload
conn.c          per-connection accept + per-request dispatch
config.c        nginx-like config DSL parser
http_parser.c   request head + response head construction
handlers.c      static / redirect / proxy / cgi / gzip / auth / autoindex
util.c          str buffer, url encode/decode, b64, MD5, gzip (stored),
                bundled POSIX-subset regex engine (capture groups + $n sub)
mime.c          MIME type table
log.c           access + error log (POSIX file, Windows console)
_win_stubs.c    Windows-only: handle_proxy (Winsock), handle_cgi (501 stub)
pweb.conf       example config (2 vhosts, 4 locations)
web/            demo content (index.html, about.html, docs/, cgi-bin/)
Makefile        POSIX build
Makefile.win    MinGW cross-build
```

## 0.5 core / 512 MB footprint

- RSS ≈ 1–2 MB idle; ~50 KB per active connection (one thread stack +
  one 16 KB request buffer + one 16 KB proxy relay buffer).
- No dependency beyond libc + pthread (POSIX) or ws2_32 (Windows).
- No allocator growth: `str` buffer is grow-once, fixed-size headers.
