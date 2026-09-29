/* main.c - entry: signals, bind/listen, accept loop, SIGHUP reload */
#include "pweb.h"
#include <stdio.h>
#include <pthread.h>
#if PWEB_PLATFORM_WIN
#include <windows.h>
#endif

static volatile sig_atomic_t g_sigterm = 0;
static volatile sig_atomic_t g_sighup = 0;

static void on_term(int sig)
{
    (void)sig;
    g_sigterm = 1;
    g_running = 0;
}
static void on_hup(int sig)
{
    (void)sig;
    g_sighup = 1;
}

#if PWEB_PLATFORM_WIN
/* Windows: a Ctrl-C / close-event sets g_sigterm so the main loop exits and
 * stop_listeners() joins the per-listener accept threads. */
static BOOL WINAPI on_ctrl(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_sigterm = 1;
        g_running = 0;
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

static int make_listener(const vhost_t *vh)
{
    int fd = PWEB_SOCKET(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    PWEB_SETSOCKOPT(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)vh->port);
    if (vh->addr[0]) {
#ifdef PWEB_PLATFORM_WIN
        sin.sin_addr.s_addr = inet_addr(vh->addr);
#else
        inet_pton(AF_INET, vh->addr, &sin.sin_addr);
#endif
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (PWEB_BIND(fd, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        fprintf(stderr, "pweb: bind %s:%d failed\n",
                vh->addr[0] ? vh->addr : "0.0.0.0", vh->port);
        PWEB_CLOSE(fd);
        return -1;
    }
    if (PWEB_LISTEN(fd, 128) != 0) {
        fprintf(stderr, "pweb: listen :%d failed\n", vh->port);
        PWEB_CLOSE(fd);
        return -1;
    }
    return fd;
}

#ifdef PWEB_PLATFORM_WIN
/* ---------- Windows: one blocking-accept thread per listener ----------
 * The POSIX select model hangs with multiple virtual hosts: the while(1)
 * accept-drain on one listener starves the others and the inline-connection
 * fallback blocks the whole loop. Instead, each listener gets a dedicated
 * thread that blocks in accept() and spawns a connection-handler thread per
 * connection. Thread count = listener count (small). To shut down or reload
 * we set a stop flag and close the listener sockets, which makes the
 * blocking accept() return an error so the thread exits; we then join it. */

static volatile int g_stop = 0;
static int *g_fds = NULL;
static int g_np = 0;
static pthread_t *g_lts = NULL;

/* Per-connection argument passed to g_handle_conn_arg. */
struct conn_arg { int fd; struct sockaddr_in a; int vh_idx; };

struct listen_arg { int fd; int owner; };

static void *listener_thread(void *arg)
{
    struct listen_arg *la = (struct listen_arg*)arg;
    int fd = la->fd;
    int owner = la->owner;
    for (;;) {
        if (g_stop) { break; }
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int cfd = PWEB_ACCEPT(fd, (struct sockaddr*)&cli, &cl);
        if (cfd < 0) {
            if (g_stop) { break; }
            continue; /* transient error: keep accepting */
        }
        struct conn_arg *a = (struct conn_arg*)malloc(sizeof(*a));
        a->fd = cfd;
        a->a = cli;
        a->vh_idx = owner;
        void *th;
        if (PWEB_THREAD(&th, g_handle_conn_arg, a) != 0) {
            /* handler unavailable: handle inline in this listener thread
             * rather than dropping the connection */
            g_handle_conn(a->fd, &a->a, a->vh_idx);
            free(a);
        } else {
            PWEB_DETACH(th);
        }
    }
    PWEB_CLOSE(fd);
    free(la);
    return NULL;
}

/* Allocate and start one blocking-accept thread per vhost listener. */
static int start_listeners(const cfg_t *cfg, int n_open)
{
    int np = 0;
    g_fds = (int*)malloc(sizeof(int) * n_open);
    g_lts = (pthread_t*)calloc((size_t)n_open, sizeof(pthread_t));
    for (int i = 0; i < cfg->nhosts; i++) {
        int f = make_listener(&cfg->hosts[i]);
        if (f < 0) { continue; }
        g_fds[np] = f;
        np++;
        printf("pweb: listening on port %d (vhost %d, %d locations)\n",
               cfg->hosts[i].port, i, cfg->hosts[i].nlocs);
    }
    if (np == 0) {
        free(g_fds); free(g_lts);
        g_fds = NULL; g_lts = NULL; g_np = 0;
        return -1;
    }
    for (int i = 0; i < np; i++) {
        struct listen_arg *la = (struct listen_arg*)malloc(sizeof(*la));
        la->fd = g_fds[i];
        la->owner = i;   /* index into cfg->hosts (all makes succeed) */
        g_lts[i] = 0;
        if (PWEB_THREAD(&g_lts[i], listener_thread, (void*)la) != 0) {
            fprintf(stderr, "pweb: failed to start listener thread for "
                    "port %d\n", cfg->hosts[i].port);
            PWEB_CLOSE(g_fds[i]);
            g_fds[i] = -1;
            free(la);
        }
    }
    g_np = np;
    return np;
}

/* Stop all listener threads: set flag, close sockets, join. */
static void stop_listeners(void)
{
    g_stop = 1;
    if (g_fds) {
        for (int i = 0; i < g_np; i++) {
            if (g_fds[i] >= 0) { PWEB_CLOSE(g_fds[i]); g_fds[i] = -1; }
        }
    }
    if (g_lts) {
        for (int i = 0; i < g_np; i++) {
            if (i < g_np && g_lts[i] != 0) {
                void *rv;
                (void)pthread_join(g_lts[i], &rv);
            }
        }
    }
}

/* Tear down current listeners and restart with a freshly-parsed config. */
static void reload_listeners(const char *path)
{
    stop_listeners();
    free(g_fds); free(g_lts);
    g_fds = NULL; g_lts = NULL; g_np = 0;
    g_stop = 0;
    fflush(stdout);

    cfg_t *newcfg = NULL;
    if (cfg_parse(path, &newcfg) == 0) {
        cfg_free(g_cfg);
        g_cfg = newcfg;
        g_stop = 0;
        int np = start_listeners(g_cfg, g_cfg->nhosts);
        if (np > 0) {
            printf("pweb: config reloaded (%d vhosts, %d listeners)\n",
                   g_cfg->nhosts, np);
        }
    } else {
        fprintf(stderr, "pweb: reload failed, keeping old config\n");
    }
    fflush(stdout);
}
#endif /* PWEB_PLATFORM_WIN */

int main(int argc, char **argv)
{
    /* -h / --help / -V / --version / -v: print info, exit before config. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            printf("PWeb - lightweight C web server (nginx-like config)\n"
                   "Usage: pweb [OPTIONS] [CONFIG]\n"
                   "\n"
                   "OPTIONS:\n"
                   "  -h, --help       Show this help and exit\n"
                   "  -V, --version    Show version and exit\n"
                   "  -v               Enable verbose logging\n"
                   "\n"
                   "CONFIG: path to nginx-style config file (default: pweb.conf)\n"
                   "\n"
                   "Examples:\n"
                   "  pweb                 Run with ./pweb.conf\n"
                   "  pweb my.conf         Run with my.conf\n"
                   "  pweb -v my.conf      Run verbosely\n"
                   "  pweb -h              Show this help\n"
                   "\n"
                   "Supported config directives: events{}, http{}, server{},\n"
                   "listen, root, location, rewrite (regex $1..$9), proxy_pass\n"
                   "(incl. https:// when built with SSL=1), cgi_pass, autoindex,\n"
                   "gzip, keepalive, SIGHUP for live reload.\n");
            return 0;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("pweb " PWEB_VERSION "\n");
            return 0;
        }
        if (strcmp(a, "-v") == 0) {
            g_verbose = 1;
            continue;
        }
    }
    const char *conf_path = "pweb.conf";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { conf_path = argv[i]; break; }
    }

#ifdef PWEB_PLATFORM_WIN
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);
    signal(SIGTERM, on_term);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGHUP, on_hup);
#endif

    log_init();
    if (cfg_parse(conf_path, &g_cfg) != 0) {
        return 1;
    }

#ifdef PWEB_PLATFORM_WIN
    if (start_listeners(g_cfg, g_cfg->nhosts) <= 0) {
        fprintf(stderr, "pweb: no listeners could be opened\n");
        return 1;
    }
    printf("pweb: ready (%d vhosts, %d listeners, one accept thread each). "
           "Ctrl-C to stop.\n", g_cfg->nhosts, g_np);
    fflush(stdout);

    while (!g_sigterm) {
        if (g_sighup) {
            g_sighup = 0;
            reload_listeners(conf_path);
        }
        Sleep(250); /* poll flags; listener threads do the actual work */
    }
    stop_listeners();
    cfg_free(g_cfg);
    printf("pweb: stopped\n");
    return 0;
#else
    int nports = g_cfg->nhosts;
    int *fds = (int*)malloc(sizeof(int) * (size_t)nports);
    int np = 0;
    for (int i = 0; i < g_cfg->nhosts; i++) {
        int f = make_listener(&g_cfg->hosts[i]);
        if (f >= 0) {
            fds[np] = f;
            np++;
            printf("pweb: listening on port %d (vhost %d, %d locations)\n",
                   g_cfg->hosts[i].port, i, g_cfg->hosts[i].nlocs);
        }
    }
    if (np == 0) {
        fprintf(stderr, "pweb: no listeners could be opened\n");
        return 1;
    }
    int maxfd = 0;
    for (int i = 0; i < np; i++) {
        if (fds[i] > maxfd) { maxfd = fds[i]; }
    }
    printf("pweb: ready (%d vhosts, %d listeners). Ctrl-C to stop.\n",
           g_cfg->nhosts, np);
    fflush(stdout);

    while (!g_sigterm) {
        if (g_sighup) {
            g_sighup = 0;
            cfg_t *newcfg = NULL;
            if (cfg_parse(conf_path, &newcfg) == 0) {
                for (int i = 0; i < np; i++) { close(fds[i]); }
                np = 0;
                free(fds);
                cfg_free(g_cfg);
                g_cfg = newcfg;
                fds = (int*)malloc(sizeof(int) * (size_t)g_cfg->nhosts);
                for (int i = 0; i < g_cfg->nhosts; i++) {
                    int f = make_listener(&g_cfg->hosts[i]);
                    if (f >= 0) {
                        fds[np] = f;
                        np++;
                    }
                }
                maxfd = 0;
                for (int i = 0; i < np; i++) {
                    if (fds[i] > maxfd) { maxfd = fds[i]; }
                }
                printf("pweb: config reloaded (%d vhosts, %d listeners)\n",
                       g_cfg->nhosts, np);
                fflush(stdout);
            } else {
                fprintf(stderr, "pweb: reload failed, keeping old config\n");
            }
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        for (int i = 0; i < np; i++) { FD_SET(fds[i], &rfds); }
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) { continue; }
            break;
        }
        if (n == 0) { continue; }
        for (int i = 0; i < np; i++) {
            if (!FD_ISSET(fds[i], &rfds)) { continue; }
            while (1) {
                struct sockaddr_in cli;
                socklen_t cl = sizeof(cli);
                int cfd = accept(fds[i], (struct sockaddr*)&cli, &cl);
                if (cfd < 0) { break; }
                struct { int fd; struct sockaddr_in a; int vh_idx; } *arg =
                    (struct { int fd; struct sockaddr_in a; int vh_idx; })
                    malloc(sizeof(*arg));
                arg->fd = cfd;
                arg->a = cli;
                arg->vh_idx = i;
                void *th;
                if (PWEB_THREAD(&th, g_handle_conn_arg, arg) != 0) {
                    g_handle_conn(arg->fd, &arg->a, arg->vh_idx);
                    free(arg);
                } else {
                    PWEB_DETACH(th);
                }
            }
        }
    }

    for (int i = 0; i < np; i++) { close(fds[i]); }
    free(fds);
    cfg_free(g_cfg);
    printf("pweb: stopped\n");
    return 0;
#endif
}
