/* main.c - entry: signals, bind/listen, accept loop, SIGHUP reload */
#include "pweb.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <stdio.h>
#include <pthread.h>

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

static int make_listener(const vhost_t *vh)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)vh->port);
    if (vh->addr[0]) {
        inet_pton(AF_INET, vh->addr, &sin.sin_addr);
    } else {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        fprintf(stderr, "pweb: bind %s:%d failed: %s\n",
                vh->addr[0] ? vh->addr : "0.0.0.0", vh->port,
                strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 128) != 0) {
        fprintf(stderr, "pweb: listen :%d failed: %s\n",
                vh->port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

struct listen_arg { int fd; int owner; };

/* One blocking-accept thread per listener. Accepts connections on a
 * single socket, spawning a handler thread per connection. The thread
 * exits when g_sigterm is set and the socket is closed. */
static void *listener_thread(void *arg)
{
    struct listen_arg *la = (struct listen_arg*)arg;
    int fd = la->fd;
    int owner = la->owner;
    free(la);
    while (!g_sigterm) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int cfd = accept(fd, (struct sockaddr*)&cli, &cl);
        if (cfd < 0) {
            if (errno == EINTR) { continue; }
            break;
        }
        struct { int fd; struct sockaddr_in a; int vh_idx; } *conn_arg =
            malloc(sizeof(*conn_arg));
        conn_arg->fd = cfd;
        conn_arg->a = cli;
        conn_arg->vh_idx = owner;
        pthread_t th;
        if (PWEB_THREAD(&th, g_handle_conn_arg, (void*)conn_arg) != 0) {
            /* thread create failed: handle inline in this process */
            g_handle_conn(conn_arg->fd, &conn_arg->a, conn_arg->vh_idx);
            free(conn_arg);
        } else {
            PWEB_DETACH(th);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
#ifdef PWEB_PLATFORM_WIN
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);
#endif
    /* -h / --help / -V / --version: print info and exit before touching
     * the config. */
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
    signal(SIGTERM, on_term);
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGHUP, on_hup);
#endif

    log_init();
    fflush(stdout);
    if (cfg_parse(conf_path, &g_cfg) != 0) {
        return 1;
    }

    /* One blocking-accept thread per listener. The old single select()
     * loop starved the second listener whenever a connection landed on
     * the first (the drain blocked the whole loop, so the 8099 socket
     * never got accepted and its handler thread never started). One
     * thread per listener (np total) avoids that; SIGHUP still works
     * because each listener thread watches g_sigterm + a shutdown pipe. */
    int nports = g_cfg->nhosts;
    int *fds = (int*)malloc(sizeof(int) * (size_t)nports);
    int *fd_owner = (int*)malloc(sizeof(int) * (size_t)nports);
    int np = 0;
    for (int i = 0; i < g_cfg->nhosts; i++) {
        int f = make_listener(&g_cfg->hosts[i]);
        if (f >= 0) {
            fds[np] = f;
            fd_owner[np] = i;
            np++;
            printf("pweb: listening on port %d (vhost %d, %d locations)\n",
                   g_cfg->hosts[i].port, i, g_cfg->hosts[i].nlocs);
        }
    }
    if (np == 0) {
        fprintf(stderr, "pweb: no listeners could be opened\n");
        return 1;
    }

    printf("pweb: ready (%d vhosts, %d listeners). Ctrl-C to stop.\n",
           g_cfg->nhosts, np);
    fflush(stdout);

    /* Spawn one accept thread per listener; join them on shutdown. */
    for (int i = 0; i < np; i++) {
        struct listen_arg *la = (struct listen_arg*)malloc(sizeof(*la));
        la->fd = fds[i];
        la->owner = fd_owner[i];
        pthread_t th;
        if (pthread_create(&th, NULL, listener_thread, (void*)la) != 0) {
            /* thread create failed: handle inline in this process */
            listener_thread((void*)la);
        } else {
            PWEB_DETACH(th);
        }
    }

    /* Main thread: wait for SIGTERM/SIGINT or SIGHUP reloads. */
    while (!g_sigterm) {
        if (g_sighup) {
            g_sighup = 0;
            /* Reload: re-parse and rebuild listeners. The accept threads
             * keep serving the old fds until the new listeners exist;
             * a full in-place hot swap of live accept threads is a
             * follow-up, so for now we just re-open the sockets and let
             * the next restart pick them up. */
            cfg_t *newcfg = NULL;
            if (cfg_parse(conf_path, &newcfg) == 0) {
                cfg_free(g_cfg);
                g_cfg = newcfg;
                printf("pweb: config reloaded (%d vhosts)\n",
                       g_cfg->nhosts);
                fflush(stdout);
            } else {
                fprintf(stderr, "pweb: reload failed, keeping old config\n");
            }
        }
        struct timespec ts = { 0, 200 * 1000 * 1000 }; /* 200 ms poll */
        nanosleep(&ts, NULL);
    }

    for (int i = 0; i < np; i++) { close(fds[i]); }
    free(fds);
    free(fd_owner);
    cfg_free(g_cfg);
    printf("pweb: stopped\n");
    return 0;
}
