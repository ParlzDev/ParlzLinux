/* pweb.h - PWeb lightweight web server, shared types */
#ifndef PWEB_H
#define PWEB_H

#define PWEB_VERSION "0.5.0"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <io.h>
#  include <process.h>
#  include <pthread.h>
#  pragma comment(lib, "ws2_32.lib")
#  define PWEB_PLATFORM_WIN 1
#else
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <sys/wait.h>
#  include <pthread.h>
#endif

#if PWEB_PLATFORM_WIN
#define PWEB_SENENV(n,v,o) _putenv((n "=" v))
#define PWEB_FORK() 1
#define PWEB_EXECV(a,b) _spawnv(_P_WAIT,(a),(b))
#define PWEB_WAITPID(p,s) ((void)(p),(void)(s),0)
#define PWEB_CLOSEINT(F) _close((int)(F))
#define PWEB_PIPE(P) _pipe((P),8192,_O_BINARY)
#define PWEB_READ(F,B,N) _read((int)(F),(char*)(B),(unsigned int)(N))
#define PWEB_WRITE(F,B,N) _write((int)(F),(const char*)(B),(unsigned int)(N))
#define PWEB_DUP2(A,B) _dup2((int)(A),(int)(B))
static inline int pweb_thread_create(pthread_t *tid, void *(*f)(void*), void *arg)
{
    unsigned int h = _beginthreadex(NULL,0,(unsigned int(__cdecl*)(void*))f, arg,0,NULL);
    *tid = (pthread_t)h;
    return h ? 0 : -1;
}
#define PWEB_THREAD(TID,F,A) pweb_thread_create((pthread_t*)(TID),(F),(A))
#define PWEB_DETACH(ID) (void)(ID)
#define PWEB_EXIT(code) exit(code)
#else
#define PWEB_SENENV(n,v,o) setenv((n),(v),(o))
#define PWEB_FORK() fork()
#define PWEB_EXECV(a,b) execv((a),(b))
#define PWEB_WAITPID(p,s) waitpid((p),(s),0)
#define PWEB_CLOSEINT(F) close((F))
#define PWEB_PIPE(P) pipe((P))
#define PWEB_READ(F,B,N) read((F),(B),(N))
#define PWEB_WRITE(F,B,N) write((F),(B),(N))
#define PWEB_DUP2(A,B) dup2((A),(B))
static inline int pweb_thread_create(pthread_t *tid, void *(*f)(void*), void *arg)
{
    return pthread_create(tid, NULL, f, arg);
}
#define PWEB_THREAD(TID,F,A) pweb_thread_create((TID),(F),(A))
#define PWEB_DETACH(ID) pthread_detach((ID))
#define PWEB_EXIT(code) _exit(code)
#endif

#if PWEB_PLATFORM_WIN
#define PWEB_SOCKET(a,t,p)  ((int)socket((a),(t),(p)))
#define PWEB_CONNECT(FD,A,L) connect((SOCKET)(unsigned int)(FD),(A),(L))
#define PWEB_ACCEPT(FD,A,L)  ((int)accept((SOCKET)(unsigned int)(FD),(A),(L)))
#define PWEB_SEND(FD,B,N,F)  send((SOCKET)(unsigned int)(FD),(const char*)(B),(int)(N),(F))
#define PWEB_RECV(FD,B,N,F)  recv((SOCKET)(unsigned int)(FD),(char*)(B),(int)(N),(F))
#define PWEB_LISTEN(FD,Q)    listen((SOCKET)(unsigned int)(FD),(Q))
#define PWEB_BIND(FD,A,L)    bind((SOCKET)(unsigned int)(FD),(A),(L))
#define PWEB_SETSOCKOPT(FD,L,N,O,OL) setsockopt((SOCKET)(unsigned int)(FD),(L),(N),(const char*)(O),(OL))
#define PWEB_CLOSE(FD)       closesocket((SOCKET)(unsigned int)(FD))
#define PWEB_INET_NTOP(F,A,B,C) inet_ntop((F),(A),(B),(C))
#else
#define PWEB_SOCKET(a,t,p)  socket((a),(t),(p))
#define PWEB_CONNECT(FD,A,L) connect((FD),(A),(L))
#define PWEB_ACCEPT(FD,A,L)  accept((FD),(A),(L))
#define PWEB_SEND(FD,B,N,F)  send((FD),(B),(N),(F))
#define PWEB_RECV(FD,B,N,F)  recv((FD),(B),(N),(F))
#define PWEB_LISTEN(FD,Q)    listen((FD),(Q))
#define PWEB_BIND(FD,A,L)    bind((FD),(A),(L))
#define PWEB_SETSOCKOPT(FD,L,N,O,OL) setsockopt((FD),(L),(N),(O),(OL))
#define PWEB_CLOSE(FD)       close((FD))
#define PWEB_INET_NTOP(F,A,B,C) inet_ntop((F),(A),(B),(C))
#endif

#define PWEB_FD_SETS(FD,SET)  FD_SET((FD),(SET))
#define PWEB_FD_CLRS(FD,SET)  FD_CLR((FD),(SET))
#define PWEB_FD_ISS(FD,SET)   FD_ISSET((FD),(SET))
#define PWEB_SELECT(FD,RS,WS,ES,TV) select((FD),(RS),(WS),(ES),(TV))

/* ============ optional TLS client (proxy_pass https://) ============
 * Gated behind -DPWEB_SSL. Default build stays zero-dependency: when
 * PWEB_SSL is not defined, the macros collapse to stubs that make the
 * https proxy path return 501, and no OpenSSL include/link is required. */
#ifdef PWEB_SSL
#include <openssl/ssl.h>
#define PWEB_SSL_HAVE 1
#define PWEB_SSL_CTX_NEW()      (SSL_CTX_new(TLS_client_method()))
#define PWEB_SSL_NEW(C)        (SSL_new((C)))
#define PWEB_SSL_SET_FD(S,F)  (SSL_set_fd((S),(F)))
#define PWEB_SSL_CONNECT(S)   (SSL_connect((S)))
#define PWEB_SSL_WRITE(S,B,N) (SSL_write((S),(B),(N)))
#define PWEB_SSL_READ(S,B,N)  (SSL_read((S),(B),(N)))
#define PWEB_SSL_SHUTDOWN(S)  (SSL_shutdown((S)))
#define PWEB_SSL_FREE(S)      (SSL_free((S)))
#define PWEB_SSL_CTX_FREE(C)  (SSL_CTX_free((C)))
#else
#define PWEB_SSL_HAVE 0
#endif

/* ============ util ============ */
typedef struct { char *buf; size_t len; size_t cap; int owned; } str;
void  str_init(str *s, char *fixed, size_t cap);
void  str_init_m(str *s);
void  str_free(str *s);
int   str_append(str *s, const char *v);
int   str_appendf(str *s, const char *fmt, ...);
int   str_append_raw(str *s, const void *p, size_t n);
void  url_decode(char *s);
void  url_encode(str *out, const char *s, int is_path);
char *b64_decode(const char *src, size_t n, size_t *outlen);
void  md5_hex(const void *data, size_t n, char out[33]);
int   pweb_md5(const void *d, size_t n, unsigned char *out16);
char *pweb_gzip(const void *data, size_t n, size_t *outlen);
const char *casestrs(const char *hay, const char *needle);

/* bundled POSIX-subset regex engine (capture groups + $n substitution) */
int  pweb_re_compile(const char *pat);
int  pweb_re_match(const char *pat, const char *s, char caps[][512], int *ncaps);
int  pweb_re_sub(const char *pat, const char *s, const char *repl,
                 char *out, size_t n);

/* ============ mime ============ */
const char *mime_for(const char *path, int *is_binary);

/* ============ http_parser ============ */
typedef struct {
    char method[8];
    char target[2048];
    char version[8];
    struct { char k[64]; char v[512]; } hdrs[48];
    int n_hdrs;
    int content_length;
    int is_chunked;
    char *body;
} req_t;

int parse_request(req_t *req, char *buf, int *pos, int buflen);
const char *req_hdr(const req_t *r, const char *k);
int parse_http_response_head(char *buf, size_t len, int *status,
                             int *content_length, int *chunked,
                             size_t *head_end);
int reason_str(int status, char *out, size_t n);
char *read_body(int fd, int content_length, long max_body, int *cl_out);

/* ============ config ============ */
typedef enum { L_STATIC, L_PROXY, L_CGI, L_REDIR, L_DEFAULT } ltype_t;

typedef struct {
    char match[512];
    int  is_regex;
    ltype_t type;
    char root[512];
    char proxypass[512];
    char cgi_script[512];
    char redir[512];        /* legacy "regex repl" (kept for backward compat) */
    char redir_pat[256];    /* rewrite pattern (regex) */
    char redir_repl[256];   /* rewrite replacement ($1..$9 expandable) */
    int  redir_flag;       /* 0=301(permanent), 1=302(last/break) */
} loc_t;

typedef struct {
    int port;
    char addr[64];
    int is_default;
    int is_listen_ssl;     /* listen ... ssl;  -> terminate TLS on accept */
    char ssl_certificate[256]; /* ssl_certificate path (empty = default) */
    char root[512];
    char names[256];
    char index_list[128];
    int autoindex;
    int gzip;
    int gzip_min;
    int keepalive;
    long max_body;
    char access_log[256];
    char error_log[256];
    char auth_realm[64];
    char auth_file[256];
    loc_t *locs; int nlocs;
} vhost_t;

typedef struct {
    vhost_t *hosts; int nhosts;
} cfg_t;

int cfg_parse(const char *path, cfg_t **out);
void cfg_free(cfg_t *c);
const vhost_t *vhost_find(const cfg_t *c, const char *host, int port);

/* ============ log ============ */
void log_init(void);
void log_access(const char *file, const char *fmt, ...);
void log_err(const char *file, int level, const char *fmt, ...);

/* ============ conn ============ */
extern cfg_t *g_cfg;
extern volatile int g_running;
extern int g_verbose;
void g_handle_conn(int fd, const struct sockaddr_in *cli, int vh_idx);
void *g_handle_conn_arg(void *p);

/* ============ handlers ============ */
int handle_static(int fd, const vhost_t *vh, const loc_t *lc, const req_t *req,
                  const char *client_ip, const char *uri);
int handle_proxy(int fd, const loc_t *lc, const req_t *req,
                 const char *client_ip);
int handle_cgi(int fd, const vhost_t *vh, const loc_t *lc, const req_t *req,
               const char *client_ip);
int handle_redir(int fd, const loc_t *lc, const req_t *req, const char *uri);
int resp_send(int fd, int status, int keepalive, const char *body,
              const char *ctype, int extra_hdrs, char *hdrs);
int autoindex_page(str *s, const char *root, const char *uri, const char *parent);
int verify_basic_auth(const char *file, const char *b64auth);

#endif
