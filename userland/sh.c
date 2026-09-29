/* sh.c - Parlz 最小 shell:内建命令 + 管道 + 重定向 + execvp 回退。
 * 支持:
 *   sh <script>     非交互执行脚本
 *   内建: ls [-l] cd echo touch mount pwd clear help exit
 *   管道: cmd1 | cmd2 | ... (至多 5 段;每段可以是内建或外部命令)
 *   重定向: cmd > f / cmd >> f / cmd < f(作用于管道最后一段)
 * 外部命令走 execvp,argv 严格 NULL 结尾(内核对 NULL argv 告警的根因)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- 内建命令实现(返回 0;exit 返回 1 让调用方退出)---- */

/* run_code/sh -c 入口的前向声明(定义在脚本模式一节) */
static int run_code(const char *code);

/* ---- ls 颜色(ANSI 转义;NO_COLOR 或管道输出时关,防终端乱码)----
 * 目录蓝、普通文件绿、符号链接青。
 * 颜色开关:LS_COLORS=1 强制开(guest 串口 QEMU 交互场景,stdout 非 tty);
 *           NO_COLOR=1 强制关;都不设时按 isatty(stdout) 自动判断。 */
static int color_on(void)
{
    const char *nc = getenv("NO_COLOR");
    if (nc && *nc)
        return 0;
    if (getenv("LS_COLORS"))
        return 1;
    return isatty(STDOUT_FILENO);
}

/* 按 stat 类型取 ANSI 颜色(无匹配 = 普通)。目录蓝 / 文件绿 / 链接青。 */
static const char *type_color(int mode)
{
    if (S_ISDIR(mode))
        return "\033[34m";   /* 蓝 */
    if (S_ISREG(mode))
        return "\033[32m";   /* 绿 */
    if (S_ISLNK(mode))
        return "\033[36m";   /* 青 */
    return "";
}
#define ANSI_RESET "\033[0m"

static int builtin_ls(const char *p, int lflag)
{
    int col = color_on();
    /* 先 stat:若是文件则打印该文件本身;若是目录才列举 */
    struct stat st;
    if (p && lstat(p, &st) == 0 && !S_ISDIR(st.st_mode)) {
        if (lflag) {
            char ty = '-';
            if (S_ISLNK(st.st_mode)) ty = 'l';
            else if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))
                ty = 'b';
            else if (S_ISFIFO(st.st_mode)) ty = 'p';
            const char *base = strrchr(p, '/');
            const char *c = type_color(st.st_mode);
            if (col && c[0])
                printf("%c %d  %s%s%s\n", ty, (int)st.st_size,
                       c, base ? base + 1 : p, ANSI_RESET);
            else
                printf("%c %d  %s\n", ty, (int)st.st_size,
                       base ? base + 1 : p);
        } else {
            const char *c = type_color(st.st_mode);
            if (col && c[0])
                printf("%s%s%s\n", c, p, ANSI_RESET);
            else
                printf("%s\n", p);
        }
        return 0;
    }
    DIR *d = opendir(p ? p : ".");
    struct dirent *e;
    if (!d) { perror(p ? p : "."); return 1; }
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s",
                 p && strcmp(p, ".") ? p : ".", e->d_name);
        struct stat st;
        int has_st = (lstat(full, &st) == 0);
        if (lflag) {
            if (has_st) {
                char ty = '-';
                if (S_ISDIR(st.st_mode)) ty = 'd';
                else if (S_ISLNK(st.st_mode)) ty = 'l';
                else if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))
                    ty = 'b';
                const char *c = type_color(st.st_mode);
                if (col && c[0])
                    printf("%c %d  %s%s%s\n", ty, (int)st.st_size,
                           c, e->d_name, ANSI_RESET);
                else
                    printf("%c %d  %s\n", ty, (int)st.st_size, e->d_name);
            } else {
                printf("      ? %s\n", e->d_name);
            }
        } else {
            const char *c = has_st ? type_color(st.st_mode) : "";
            if (col && c && c[0])
                printf("%s%s%s\n", c, e->d_name, ANSI_RESET);
            else
                printf("%s\n", e->d_name);
        }
    }
    closedir(d);
    return 0;
}

static int builtin_echo(const char *p)
{
    printf("%s\n", p ? p : "");
    return 0;
}

#include <sys/mount.h>

static int builtin_mount(int argc, char **argv)
{
    /* mount(2):mount <src> <dst> <type> [data];无参数时打印 /proc/mounts。
     * 归为无状态命令(读 /proc/mounts 或调 mount(2),不动 shell 进程状态),
     * 可在管道子进程内执行,带参数真挂载、不带参数打印挂载表。 */
    if (argc >= 4) {
        if (mount(argv[1], argv[2], argv[3], 0,
                  argc > 4 ? argv[4] : NULL) < 0) {
            perror("mount");
            return 1;
        }
        printf("mounted %s on %s (%s)\n", argv[1], argv[2], argv[3]);
        return 0;
    }
    FILE *f = fopen("/proc/mounts", "r");
    char line[512];
    if (!f) { perror("/proc/mounts"); return 1; }
    while (fgets(line, sizeof line, f))
        puts(line);
    fclose(f);
    return 0;
}

static int builtin_touch(const char *p)
{
    if (!p) {
        fprintf(stderr, "touch: usage: touch <file>\n");
        return 1;
    }
    int fd = open(p, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        perror(p);
        return 1;
    }
    close(fd);
    return 0;
}

/* 判断是否为内建命令。
 * state=0:影响 shell 本体状态(cd/mount 等),不进管道;
 * state=1:只读写 stdin/stdout(ls/echo/touch 等),可进管道子进程。 */
static int is_builtin(const char *c, int state)
{
    static const char *stateless[] = {
        "ls", "echo", "touch", "pwd", "help", "clear", "mount", NULL
    };
    static const char *stateful[] = {
        "cd", "exit", NULL
    };
    const char **list = state ? stateful : stateless;
    for (int i = 0; list[i]; i++)
        if (!strcmp(c, list[i]))
            return 1;
    return 0;
}

struct redirect {
    int kind;        /* 1 = '>', 2 = '>>', 3 = '<' */
    const char *path;
};

/* 应用输出/输入重定向(子进程内调用)。
 * '<' 的语义就是把**子进程的** stdin 换成该文件(POSIX)。
 * 老实现是 dup2(fd,0) 之后又 dup2(3,0) 把管道读端换回去 —— 净效果等于没做,
 * 于是 `cat < f` 一直是空输出(实测), 管道段上还会被 keep_stdin 直接跳过。 */
static void apply_redir(const struct redirect *redir, int nred)
{
    for (int i = 0; i < nred; i++) {
        int r = redir[i].kind;
        if (r == 1 || r == 2) {
            int fl = O_WRONLY | (r == 1 ? O_TRUNC : O_APPEND);
            int fd = open(redir[i].path, fl | O_CREAT, 0644);
            if (fd < 0) {
                perror(redir[i].path);
                _exit(1);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        } else {
            int fd = open(redir[i].path, O_RDONLY);
            if (fd < 0) {
                perror(redir[i].path);
                _exit(1);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
    }
}

/* ---- 终端前台权(最小 job control) ----
 * 每个前台命令放进**自己的进程组**,并把控制终端的前台组指过去。
 * 缺这一步时 bash/nano/vi 这类"要自己管作业"的程序会报
 *   cannot set terminal process group: Inappropriate ioctl for device
 *   no job control in this shell
 * —— 它们还留在 shell 的进程组里,自己 setpgid/tcsetpgrp 抢不到前台。
 * sh_interactive=0(脚本/sh -c)时整套逻辑跳过,行为与原来一致。 */
static int sh_interactive;
static int sh_exit_req;           /* 执行到内建 exit: 交互循环/脚本要收摊 */
static pid_t sh_fg_leader;        /* 当前占着终端的进程组(0=shell 自己) */

static void fg_give(pid_t pgid)
{
    if (!sh_interactive || pgid <= 0)
        return;
    if (tcsetpgrp(STDIN_FILENO, pgid) == 0)
        sh_fg_leader = pgid;
}

static void fg_take_back(void)
{
    if (sh_interactive && sh_fg_leader > 0) {
        tcsetpgrp(STDIN_FILENO, getpgrp());
        sh_fg_leader = 0;
    }
}

/* 子进程侧:pgid_for==0 表示自己当组长;否则加入 leader 的组。
 * leader 那位的 setpgid 是它自己fork后第一件事,可能比我们还晚一步,
 * 所以 ESRCH/EPERM 要重试(实测微秒级就成)。 */
static void child_setpg(pid_t pgid_for)
{
    for (int i = 0; i < 200; i++) {
        if (setpgid(0, pgid_for) == 0)
            return;
        if (errno != ESRCH && errno != EPERM)
            return;
        usleep(2000);
    }
}

/* 在一个(可能是管道的)子进程里执行一段命令:
 * stateless 内建 -> 直接调 builtin;stateful 内建/外部 -> 由调用方决定。
 * 返回 1 表示要求 shell 退出(exit)。 */
static int run_seg(int argc, char **argv, char *cwd, int state)
{
    const char *cmd = argv[0];

    /* stateless 内建(ls/echo/touch/pwd/help/clear):子进程直接执行 */
    if (is_builtin(cmd, state)) {
        int rc = 0;
        if (!strcmp(cmd, "exit"))
            rc = 1;
        else if (!strcmp(cmd, "help")) {
            printf("  ls [dir] | ls -l | cd [dir] | echo <text> | "
                   "touch <file> | mount | pwd | clear | exit\n"
                   "  ls 颜色: 目录蓝/文件绿/链接青(NO_COLOR=1 或管道时关)\n"
                   "  管道: cmd1 | cmd2 (至多 5 段)\n"
                   "  重定向: cmd > f | cmd >> f | cmd < f\n"
                   "  外部命令: nano / install / mkfs / curl / ifc / ifconfig ...\n");
        }
        else if (!strcmp(cmd, "ls")) {
            int lflag = 0;
            const char *arg = NULL;
            for (int i = 1; i < argc; i++) {
                if (!strcmp(argv[i], "-l"))
                    lflag = 1;
                else
                    arg = argv[i];
            }
            rc = builtin_ls(arg, lflag);
        }
        else if (!strcmp(cmd, "cd")) {
            if (argc > 1) {
                if (chdir(argv[1]) == 0)
                    getcwd(cwd, PATH_MAX);
                else
                    perror("cd");
            } else {
                /* cd 无参数 = 回主目录(HOME,缺省 /root) */
                const char *home = getenv("HOME");
                if (!home)
                    home = "/root";
                if (chdir(home) == 0)
                    getcwd(cwd, PATH_MAX);
                else
                    perror("cd");
            }
        }
        else if (!strcmp(cmd, "echo")) {
            /* POSIX 行为:echo 打印所有参数(空格分隔);argv 已按空白
             * 分词,这里把所有非空参数拼起来输出 */
            if (argc > 1) {
                for (int i = 1; i < argc; i++) {
                    if (i > 1)
                        printf(" ");
                    printf("%s", argv[i]);
                }
                printf("\n");
            } else
                printf("\n");
            rc = 0;
        }
        else if (!strcmp(cmd, "touch"))
            rc = builtin_touch(argc > 1 ? argv[1] : NULL);
        else if (!strcmp(cmd, "mount"))
            rc = builtin_mount(argc, argv);
        else if (!strcmp(cmd, "pwd")) {
            if (getcwd(cwd, PATH_MAX))
                printf("%s\n", cwd);
            else
                perror("getcwd");
        }
        else if (!strcmp(cmd, "clear")) {
            printf("\033[H\033[2J");
            rc = 0;
        }
        return rc;
    }

    /* 外部命令:argv 必须严格 NULL 结尾(修复 NULL argv 告警)。
     * 带斜杠的路径(./xxx、/bin/xxx)先按完整路径 execve(需可执行位),
     * 失败退回 execvp 的 PATH 查找。 */
    char *exec_args[12];
    int n = 0;
    exec_args[n++] = (char *)cmd;
    for (int i = 1; i < argc && n < 11; i++)
        exec_args[n++] = argv[i];
    exec_args[n] = NULL;
    /* PARLZ_ENV_DEBUG:交互 shell 里 export 一次后可在子进程看到实际
     * environ(LD_LIBRARY_PATH 等是否传入),定位 "工具链跑不起来" 类问题 */
    if (getenv("PARLZ_ENV_DEBUG")) {
        for (char **p = environ; *p; p++) {
            const char *tag = strstr(*p, "LIB") ? "<<" : "";
            fprintf(stderr, "  ENV%s %s\n", tag, *p);
        }
    }
    if (strchr(cmd, '/'))
        execve(cmd, exec_args, environ);
    execvp(cmd, exec_args);
    fprintf(stderr, "sh: %s: not found\n", cmd);
    _exit(127);
    return 0;      /* 不可达 */
}

/* 执行一行命令:分词 + 管道切段 + 重定向收集。
 * 返回 0 正常,1 需退出 shell。 */
/* 交互层信号标志 + 等子函数的前向声明(实现在文件末尾,
 * run_line 需要调用)。 */
volatile sig_atomic_t sigint_seen = 0;
static int wait_child_safely(pid_t pid);

/* 命令历史(上下键切换):最多 32 条。
 * hist[0..hist_n-1] 按时间顺序;hist_sel 是当前编辑行的索引
 * (hist_sel == -1 表示当前不在历史上;hist_sel == hist_n 表示
 *  正在编辑新命令(空)。
 * ↑(ESC[A): 旧 → 新(往上翻); ↓(ESC[B): 新 → 旧(往下翻)。
 * 实现:ESC + 字母识别方向键(无 readline 的最简方案)。
 * 历史只在交互模式用,非交互脚本不记录。 */
#define HIST_MAX 32
static char *hist[HIST_MAX] = {0};
static int hist_n = 0;
static int hist_sel = -1;          /* -1 = 非历史项 */

static void hist_record(const char *cmd)
{
    /* 去重相邻重复 */
    if (hist_n > 0 && hist[hist_n - 1] && !strcmp(hist[hist_n - 1], cmd))
        return;
    /* 满则挤掉最早一条 */
    if (hist_n >= HIST_MAX) {
        free(hist[0]);
        memmove(&hist[0], &hist[1], (HIST_MAX - 1) * sizeof(char *));
        hist_n = HIST_MAX - 1;
        for (int i = 0; i < hist_n; i++)
            hist[i] = hist[i + 1];
        hist_n--;
        hist[hist_n] = strdup(cmd);
        if (hist_sel >= 0)
            hist_sel--;
    } else {
        hist[hist_n] = strdup(cmd);
        hist_n++;
    }
}

static void hist_clear(void)
{
    for (int i = 0; i < HIST_MAX; i++)
        free(hist[i]);
    hist_n = 0;
    hist_sel = -1;
}

/* 把历史项写到行缓冲(linebuf 长度 maxlen)。
 * 返回 1 表示成功取到一条,0 表示越界(无更多历史)。
 * dir == -1: 往旧(↑); dir == +1: 往新(↓)。 */
static int hist_move(int dir, char *linebuf, size_t maxlen)
{
    if (hist_n == 0)
        return 0;
    int sel;
    if (hist_sel < 0 || hist_sel >= hist_n)
        sel = hist_n - 1;          /* 从新命令位置 → 最近一条 */
    else if (dir == -1)            /* ↑ 往旧 */
        sel = hist_sel - 1;
    else                          /* ↓ 往新 */
        sel = hist_sel + 1;
    if (sel < 0 || sel >= hist_n)
        return 0;
    hist_sel = sel;
    snprintf(linebuf, maxlen, "%s", hist[sel]);
    return 1;
}

#include <ctype.h>

/* 最近的子进程退出码(供 $? 展开;exit 为 1;未跑过命令为 0) */
static int last_rc = 0;

/* $VAR / ${VAR} / $? 展开:把 tok 中的 $ 序列替换为环境变量值,
 * 返回替换后的新串(tok 本身不动)。无 $ 时直接返回 tok(零拷贝)。
 * 已消除引号,故展开结果保留空格不再二次分词(与最小 shell 语义一致,
 * 够用即可;需要精确空格场景仍可用引号)。 */
static char *expand_dollars(const char *tok)
{
    if (!strchr(tok, '$'))
        return (char *)tok;
    size_t len = strlen(tok) + 64;
    char *out = malloc(len);
    if (!out)
        return (char *)tok;
    char *wp = out;
    const char *p = tok;
    while (*p) {
        if (*p == '$') {
            p++;
            if (*p == '?') {
                char tmp[16];
                int tl = snprintf(tmp, sizeof tmp, "%d", last_rc);
                memcpy(wp, tmp, (size_t)tl);
                wp += tl;
                p++;
                continue;
            }
            char name[32];
            size_t nl = 0;
            int brace = 0;
            if (*p == '{') {
                brace = 1;
                p++;
            }
            while (*p && nl < sizeof name - 1 &&
                   (isalpha((unsigned char)*p) ||
                    isdigit((unsigned char)*p) || *p == '_')) {
                name[nl++] = *p++;
            }
            name[nl] = 0;
            if (nl == 0) {         /* 裸 $ 或空名:原样保留 */
                *wp++ = '$';
                if (brace && *p == '}')
                    p++;
                continue;
            }
            const char *val = getenv(name);
            if (val) {
                while (*val)
                    *wp++ = *val++;
            }
            if (brace && *p == '}')
                p++;
        } else {
            *wp++ = *p++;
        }
    }
    *wp = 0;
    return out;
}

static int run_line(const char *line, char *cwd, size_t cwdlen)
{
    char buf[512];
    /* 词缓冲必须放在**函数作用域**: 词指针(pargv)一直要用到管道 fork 之后;
     * 若声明在下面的花括号块里, 块结束其生存期即终止, 后续使用是 UB ——
     * 换个编译开关(如 -fno-stack-protector)栈槽被复用, 词会变空
     * (实测: 带该开关编译时 `echo a | cat` 报 "sh: : not found")。 */
    char tbuf[512];

    if (!line || !*line)
        return 0;

    /* EINTR:被 SIGINT(Ctrl+C)打断时行读不完整,当空行跳过。
     * 同时打印 ^C 提示。 */
    while (*line == ' ' || *line == '\t' || *line == '\r')
        line++;
    if (*line == '#')
        return 0;

    snprintf(buf, sizeof buf, "%s", line);
    buf[strcspn(buf, "\r\n")] = '\0';    /* 串口/pty 常见 \r\n: 别把 CR 当命令词 */

    /* ---------- 行首 NAME=value 赋值: 立刻 putenv(本行可见 + 活过本行) ----------
     * 老实现把**指向局部 buf 的指针**直接交给 putenv, 函数一返回指针就悬空 ——
     * 实测 `X=hello` 之后 `echo got=$X` 打出空; 而且它只认大写名字
     * (`strspn(tok, "AB...Z_")`), `x=1` 这种常见写法干脆不是赋值。
     * 现在: 自己 malloc 一份拷贝再 putenv(与 export 分支同样的理由),
     * 名字接受大小写字母/下划线开头 + 数字。
     * 注意这在**子 shell fork 之前**完成, 所以管道里的子进程也继承得到。 */
    {
        char *p = buf;
        for (;;) {
            while (*p == ' ' || *p == '\t')
                p++;
            char *eq = strchr(p, '=');
            if (!eq || eq == p)
                break;
            if (!(isalpha((unsigned char)p[0]) || p[0] == '_'))
                break;
            int ok = 1;
            for (char *q = p; q < eq; q++)
                if (!(isalnum((unsigned char)*q) || *q == '_')) { ok = 0; break; }
            if (!ok)
                break;
            char *vstart = eq + 1, *vend;
            char qc = 0;
            if (*vstart == '"' || *vstart == '\'') {
                /* 值带引号: 到配对引号为止(可含空格); 未闭合就吃到行尾 */
                qc = *vstart;
                char *e2 = strchr(vstart + 1, qc);
                vend = e2 ? e2 + 1 : vstart + strlen(vstart);
            } else {
                vend = vstart;
                while (*vend && *vend != ' ' && *vend != '\t' &&
                       *vend != ';' && *vend != '|')
                    vend++;
            }
            char *vtxt = qc ? vstart + 1 : vstart;
            char *vfin = qc ? (vend > vtxt ? vend - 1 : vtxt) : vend;
            size_t nlen = (size_t)(eq - p);
            size_t vlen = (size_t)(vfin - vtxt);
            char *kv = malloc(nlen + 1 + vlen + 1);
            if (!kv)
                break;
            memcpy(kv, p, nlen);
            kv[nlen] = '=';
            memcpy(kv + nlen + 1, vtxt, vlen);
            kv[nlen + 1 + vlen] = 0;
            putenv(kv);
            /* 把这段赋值从行里**抹成空格**: 值已经进环境了, 不能让分词器
             * 再把它当命令词(否则 `X=1 cmd` 会去执行一个叫 "X=1" 的命令)。 */
            for (char *q = p; q < vend; q++)
                *q = ' ';
            if (!*vend || *vend == ';' || *vend == '|')
                break;                  /* 后面是命令/段尾, 别越界 */
            p = vend + 1;
        }
    }

    /* ---------- 分词: 一趟做完, 且**认引号** ----------
     * 引号是定界符: 引号内的空白、|、; 都当普通字符, 引号本身剥掉;
     * 单引号内不做 $ 展开(POSIX), 双引号内做。
     * 老实现是"先把引号全删掉, 再按空白切、按 | 劈", 一处根因带出三个
     * 用户可见的问题(实测): `echo "a  b"` 空格被折叠成 a b;
     * `sh -c "echo x"` 传过去是 [echo][x] 两个参数; `echo "a|b"` 被管道劈开。 */
    struct redirect redir[4];
    int nred = 0;
    char *pargv[6][8];
    int pargc[6];
    char *exp_buf[6][8];        /* $ 展开产生的堆内存, 本行结束统一 free */
    int seg = 0;                /* 当前段索引(0 起); 总段数 nseg = seg + 1 */
    int nseg = 0;               /* 分词结束后赋值, 供下游按"段数"使用 */
    for (int i = 0; i <= 5; i++) {
        pargc[i] = 0;
        for (int k = 0; k < 8; k++) {
            exp_buf[i][k] = 0;
        }
    }
    {
        char *p = buf;
        char *tw = tbuf;
        int pending_redir = 0;  /* 上一个词是裸的 >/>>/<, 本词是它的路径 */
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\r')
                p++;
            if (*p == '|') {                    /* 段分隔(只认引号外的) */
                if (seg < 5) {
                    seg++;
                    pargc[seg] = 0;
                } else {
                    fprintf(stderr, "sh: too many pipes (max 5)\n");
                }
                p++;
                continue;
            }
            if (*p == ';')
                p++;                            /* 兼容: 未切句时当空白看待 */
            if (!*p)
                break;
            if (tw - tbuf > (int)sizeof tbuf - 8)
                break;                          /* 词缓冲满: 丢弃剩余(超长行) */

            /* 收一个词(剥引号, 内容写进 tbuf) */
            char *wtok = tw;
            char q = 0;
            int single = 0;
            for (; *p; p++) {
                char c = *p;
                if (q) {
                    if (c == q) { q = 0; continue; }   /* 闭合引号: 丢掉 */
                    if (q == '\'')
                        single = 1;
                    *tw++ = c;
                    continue;
                }
                if (c == '\'' || c == '"') { q = c; continue; }
                if (c == ' ' || c == '\t' || c == '\r' || c == '|' || c == ';')
                    break;
                *tw++ = c;
            }
            *tw++ = 0;
            if (tw == wtok + 1) {               /* 空词(如 ""): 跳过 */
                tw = wtok;
                continue;
            }

            if (pending_redir) {                /* 该词是重定向的路径 */
                if (nred < 4)
                    redir[nred++] = (struct redirect){ .kind = pending_redir,
                                                       .path = wtok };
                else
                    fprintf(stderr, "sh: too many redirections\n");
                pending_redir = 0;
                continue;
            }
            if (wtok[0] == '>' || wtok[0] == '<') {
                /* '>' '>>' '<': 可独立成词(cmd > f), 也可与路径粘连(>f) */
                int kind = (wtok[0] == '<') ? 3 : (wtok[1] == '>' ? 2 : 1);
                char *path = wtok + (kind == 2 ? 2 : 1);
                if (*path) {
                    if (nred < 4)
                        redir[nred++] = (struct redirect){ .kind = kind,
                                                           .path = path };
                    else
                        fprintf(stderr, "sh: too many redirections\n");
                } else {
                    pending_redir = kind;
                }
                continue;
            }
            if (pargc[seg] < 7) {
                if (single) {
                    /* 单引号内不做 $ 展开(POSIX) */
                    pargv[seg][pargc[seg]] = wtok;
                } else {
                    char *et = expand_dollars(wtok);
                    pargv[seg][pargc[seg]] = et;
                    if (et != wtok)
                        exp_buf[seg][pargc[seg]] = et;
                }
                pargc[seg]++;
            }
        }
    }
    nseg = seg + 1;             /* 段数(1 起): 下游按"段数"用, 别混成索引 */

    /* 3) 单段:
     * - export:持久改 shell env(影响后续所有命令)
     * - stateful 内建(cd/exit/mount):shell 本体直接执行(改 shell 状态)
     * - 其他:fork;有重定向时 dup2
     * (行首的 NAME=value 已在 run_line 开头 putenv, 这里不再重复) */
    if (nseg == 1) {
        if (pargc[0] == 0)
            return 0;
        const char *cmd = pargv[0][0];
        /* cd 无参数 = 回主目录(HOME,缺省 /root) */
        if (!strcmp(cmd, "cd") && pargc[0] == 1) {
            const char *home = getenv("HOME");
            if (!home)
                home = "/root";
            if (chdir(home) == 0)
                getcwd(cwd, cwdlen);
            else
                perror("cd");
            return 0;
        }
        if (!strcmp(cmd, "export")) {
            for (int a = 1; a < pargc[0]; a++) {
                char *v = pargv[0][a];
                char *eq = strchr(v, '=');
                if (eq) {
                    /* export VAR=val:putenv 存的是指向 buf 的临时指针,
                     * 下一行 run_line 会覆写 buf → 必须复制一份再 putenv。
                     * 注意 v 已经过 expand_dollars,可能是堆指针,拷贝后
                     * 若原指针非 buf 内(由 pfx_env 或 expand_dollars 分配)
                     * 则无法安全释放,此处直接复制新串(泄漏可接受,
                     * export 调用次数少)。 */
                    char *copy = strdup(v);
                    if (copy)
                        putenv(copy);
                } else {
                    /* 裸 export VAR:打印现有值 */
                    char *cur = getenv(v);
                    if (cur)
                        printf("%s=%s\n", v, cur);
                }
            }
            return 0;
        }
        if (is_builtin(cmd, 1)) {          /* cd / exit / mount(改 shell 状态) */
            last_rc = 0;
            if (!strcmp(cmd, "exit")) {
                sh_exit_req = 1;
                if (pargc[0] > 1)          /* exit N: 退出码取 N(POSIX) */
                    last_rc = atoi(pargv[0][1]) & 0xff;
            }
            return run_seg(pargc[0], pargv[0], cwd, 1);
        }
        pid_t pid = fork();
        if (pid == 0) {
            child_setpg(0);
            chdir(cwd);
            if (nred > 0)
                apply_redir(redir, nred);
            /* 输出重定向到文件: stdout 变成普通文件, glibc 默认全缓冲
             * (4KB+ 才写盘) —— 外部命令(curl/nano/gcc)表现为"过一会
             * 才输出一次"。子进程一律无缓冲, 每次 write 立即落盘。 */
            if (nred > 0)
                setvbuf(stdout, NULL, _IONBF, 0);
            if (is_builtin(cmd, 0)) {      /* ls/echo/touch/pwd/help/clear */
                fflush(stdout);           /* 子进程 stdout 可能带缓冲,_exit 前必须 flush */
                int rc = run_seg(pargc[0], pargv[0], cwd, 0);
                fflush(stdout);
                _exit(rc ? 1 : 0);
            }
            char *ea[8];
            int n = 0;
            ea[n++] = pargv[0][0];
            for (int i = 1; i < pargc[0]; i++)
                ea[n++] = pargv[0][i];
            ea[n] = NULL;
            /* 路径执行(./xxx、/bin/xxx): execvp 只在 PATH 里找"无斜杠"
             * 的命令名; 带斜杠时先按完整路径 execve(需可执行位), 失败再
             * 退回 PATH 查找(绝对路径的 execvp 也会直接尝试原路径)。 */
            if (strchr(cmd, '/')) {
                char *argv2[8];
                int m = 0;
                argv2[m++] = pargv[0][0];
                for (int i = 1; i < pargc[0] && m < 7; i++)
                    argv2[m++] = pargv[0][i];
                argv2[m] = NULL;
                execve(cmd, argv2, environ);
            }
            execvp(cmd, ea);
            if (strchr(cmd, '/'))
                fprintf(stderr, "sh: %s: not found or not executable\n", cmd);
            else
                fprintf(stderr, "sh: %s: not found\n", cmd);
            _exit(127);
        }
        /* 父子都调 setpgid(pid,pid):只在子进程里调会有竞态 ——
         * 它可能已经 exec 了,那时父进程再 setpgid 就 EACCES。 */
        if (pid > 0)
            setpgid(pid, pid);
        fg_give(pid);
        int st = wait_child_safely(pid);
        fg_take_back();
        /* POSIX: 被信号杀掉的子进程 $? = 128+信号(Ctrl+C 后应是 130/137,
         * 之前一律算 0, 让人误以为 ^C 没生效) */
        last_rc = WIFEXITED(st) ? WEXITSTATUS(st)
                                : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 0);
        for (int k = 0; exp_buf[0][k]; k++)
            free(exp_buf[0][k]);
        return 0;
    }

    /* 4) 多段管道:每段 fork;内建段直接调 builtin,外段 execvp */
    int fds[10];
    for (int i = 0; i < nseg - 1; i++) {
        if (pipe(fds + i * 2) < 0) {
            perror("pipe");
            return 1;
        }
    }
    pid_t pids[6];
    pid_t pipe_leader = 0;
    for (int i = 0; i < nseg; i++) {
        pids[i] = 0;
        if (pargc[i] == 0)
            continue;
        pids[i] = fork();
        if (pids[i] < 0)
            return 1;
        if (pids[i] == 0) {
            /* 整条管道共用一个进程组:第一段当组长,后面加入。
             * pipe_leader 在 fork 前已由父进程赋好(写时复制给子)。 */
            child_setpg(pipe_leader);
            chdir(cwd);
            if (i > 0)
                dup2(fds[(i - 1) * 2], STDIN_FILENO);
            if (i < nseg - 1)
                dup2(fds[i * 2 + 1], STDOUT_FILENO);
            else
                apply_redir(redir, nred);
            for (int j = 0; j < nseg - 1; j++) {
                close(fds[j * 2]);
                close(fds[j * 2 + 1]);
            }
            int rc = run_seg(pargc[i], pargv[i], cwd, 0);
            fflush(stdout);
            _exit(rc ? 1 : 0);
        }
        if (pipe_leader == 0)
            pipe_leader = pids[i];
        /* 父进程也调一次:子进程可能已经 exec,那时这次会 EACCES 失败,
         * 但子进程侧的 child_setpg 已经生效了 —— 两边各挡一半竞态。 */
        setpgid(pids[i], pipe_leader);
    }
    fg_give(pipe_leader);
    for (int i = 0; i < nseg - 1; i++) {
        close(fds[i * 2]);
        close(fds[i * 2 + 1]);
    }
    int pstatus[6] = {0};
    for (int i = 0; i < nseg; i++) {
        if (pids[i] > 0)
            waitpid(pids[i], &pstatus[i], 0);
    }
    fg_take_back();
    /* $? 取管道最后一段的退出码(POSIX 语义; 被信号杀同样报 128+信号) */
    last_rc = WIFEXITED(pstatus[nseg - 1]) ? WEXITSTATUS(pstatus[nseg - 1])
              : (WIFSIGNALED(pstatus[nseg - 1]) ? 128 + WTERMSIG(pstatus[nseg - 1]) : 0);
    /* 释放各段 $? 展开的堆内存 */
    for (int i = 0; i < nseg; i++)
        for (int k = 0; exp_buf[i][k]; k++)
            free(exp_buf[i][k]);
    /* 管道各段是独立进程,若 Ctrl+C 打断,统一清理未退出的段 */
    if (sigint_seen) {
        for (int i = 0; i < nseg; i++) {
            if (pids[i] > 0 &&
                waitpid(pids[i], &pstatus[i], WNOHANG) == 0)
                kill(pids[i], SIGKILL);
        }
    }
    return 0;
}

/* 切句执行: 交互输入 / sh -c / 脚本文件三处共用。
 * 分句符是**引号外**的 ';' 或换行 —— 引号内的分号是普通字符。
 * 老实现只有 sh -c 会切句, 而且是盲切(strpbrk), 于是
 * `awk 'BEGIN{print 1; print 2}'` 被劈成两条命令; 交互路径干脆不切,
 * `echo a; echo b` 变成把 "a; echo b" 当参数传给 echo。
 * 返回 1 = 要求 shell 退出(exit / 致命错误)。 */
static int run_cmds(const char *code, char *cwd, size_t cwdlen)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", code);
    char *p = buf;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == ';' || *p == '\n')
            p++;
        if (!*p)
            return 0;
        char q = 0;
        char *e = p;
        for (; *e; e++) {
            if (q) {
                if (*e == q)
                    q = 0;
                continue;
            }
            if (*e == '\'' || *e == '"') { q = *e; continue; }
            if (*e == ';' || *e == '\n' || *e == '\r')
                break;
        }
        char save = *e;
        *e = 0;
        run_line(p, cwd, cwdlen);
        *e = save;
        /* 返回值 = **最后一条命令的退出码**(POSIX: sh -c 与脚本都按这个退),
         * "要求退出" 单独用 sh_exit_req 传递 —— 两者混在一个返回值里,
         * 会让 `sh -c 'ls /nope'` 之类的调用方永远看到 0。 */
        if (sh_exit_req)
            return last_rc;
        if (!save)
            return last_rc;
        p = e + 1;
    }
}

/* 执行一段代码串(sh -c 参数) */
static int run_code(const char *code)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "/");
    return run_cmds(code, cwd, sizeof cwd);
}

/* 脚本模式 */
static int run_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 1;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "/");
    char line[512];
    int rc = 0;
    while (fgets(line, sizeof line, f)) {
        /* 空行/纯注释不执行也不回显 */
        char *t = line;
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '\0' || *t == '#' || *t == '\n' || *t == '\r')
            continue;
        /* 脚本内命令回显到 stderr(脚本自身输出走 stdout, 不混),
         * 便于用户看到每行执行了什么 */
        fputs(line, stderr);
        fflush(stderr);
        rc = run_cmds(line, cwd, sizeof cwd);   /* 最后一条命令的退出码 */
        if (sh_exit_req)
            break;
    }
    fclose(f);
    fflush(stdout);
    return rc;
}

/* ---- 交互层:Ctrl+C 中断 + user@host:cwd> 提示符 ---- */

/* SIGINT:默认终止进程。交互 shell 装 handler:只置位标志。
 * 行读/等子被打断(EINTR)时检查标志:杀当前子进程、打 ^C、回新提示符。 */
static void sigint_handler(int sig)
{
    (void)sig;
    sigint_seen = 1;
}

/* 等最近 fork 的子进程;若被 Ctrl+C 打断则 SIGKILL 它。 */
static int wait_child_safely(pid_t pid)
{
    int st;
    for (;;) {
        if (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            if (sigint_seen) {
                sigint_seen = 0;
                kill(pid, SIGKILL);
                if (waitpid(pid, &st, 0) < 0)
                    return -1;
            }
            continue;
        }
        break;
    }
    return st;
}

/* 生成提示符 "user@host:cwd> "。
 * user 取 $USER 或 /etc/passwd 里 uid 对应的;host 取 hostname(无
 * 网络时 fallback "parlz");cwd 取 getcwd,根目录显示 /。 */
static void make_prompt(char *out, size_t n, const char *cwd, int bash_mode)
{
    char user[32] = "parlz";
    const char *env_user = getenv("USER");
    if (env_user && *env_user)
        snprintf(user, sizeof user, "%s", env_user);
    else {
        uid_t uid = getuid();
        struct passwd *pw = getpwuid(uid);
        if (pw)
            snprintf(user, sizeof user, "%s", pw->pw_name);
    }
    /* 提示符 user 兜底:无 $USER/非交互自动化时读 /etc/parlz-auth 首行,
     * 让登录流测试(boot-verify 等)能直接 grep "tester@parlz:" 判定,
     * 不依赖提示符本身出现(串口回显缺失时)。 */
    if (!env_user || !*env_user) {
        FILE *af = fopen("/etc/parlz-auth", "r");
        if (af) {
            char abuf[64];
            if (fgets(abuf, sizeof abuf, af)) {
                abuf[strcspn(abuf, "\r\n")] = 0;
                if (*abuf)
                    snprintf(user, sizeof user, "%s", abuf);
            }
            fclose(af);
        }
    }
    char host[64] = "parlz";
    const char *env_host = getenv("HOSTNAME");
    if (env_host && *env_host)
        snprintf(host, sizeof host, "%s", env_host);
    else if (gethostname(host, sizeof host) < 0)
        host[0] = 0;
    const char *disp = host[0] ? host : "parlz";

    char cdisp[PATH_MAX];
    if (cwd && *cwd)
        snprintf(cdisp, sizeof cdisp, "%s", cwd);
    else
        strcpy(cdisp, "/");
    /* 主目录缩写 ~ */
    const char *home = getenv("HOME");
    if (home && !strcmp(cdisp, home))
        strcpy(cdisp, "~");

    if (bash_mode) {
        /* bash 风格: user@host:cwd$ (root 显示 #), 尾空格(与真实 bash 一致)。
         * 交互 raw 模式手动回显, 尾空格保证下一词不粘在 $/# 上;
         * 提示符本身不是输入, 行首 $ 不会被当作 shell 元字符。 */
        char mark = (getuid() == 0) ? '#' : '$';
        snprintf(out, n, "%s@%s:%s%c ", user, disp, cdisp, mark);
    } else {
        snprintf(out, n, "%s@%s:%s> ", user, disp, cdisp);
    }
}

int main(int argc, char *argv[])
{
    const char *applet = strrchr(argv[0], '/');
    applet = applet ? applet + 1 : argv[0];
    if (!strcmp(applet, "ls") || !strcmp(applet, "touch") ||
        !strcmp(applet, "clear")) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd))
            strcpy(cwd, "/");
        return run_seg(argc, argv, cwd, 0);
    }
/* shx 判别器: 交互 tty → /bin/parlz-sh(userland 最小 shell);
 * 脚本/-c  → /bin/bash(全 POSIX)。
 * 提示符 user@host: 由 make_prompt 里 sh.c 的 $USER/$HOSTNAME 输出。 */
    int bash_mode = (argc > 0 && argv[0] &&
                     strstr(argv[0], "bash") != NULL);
    /* QEMU 串口场景 stdout 默认全缓冲, 逐字符回显会攒在 stdio 缓冲
     * 里"过一会才刷一次"。setvbuf 必须在任何 I/O 前调用才生效 ——
     * 放到 main 最开头。交互与脚本模式都设, 脚本输出立即落盘。 */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* init 的 execl 不带环境,PATH 未设。显式指向 /usr/bin(工具链软链
     * 优先)+ /bin(静态命令兜底),保证 execvp 在 initramfs 里能找到外部
     * 命令。已有 PATH 则保留(init 正常已设 /usr/bin 在前)。 */
    if (!getenv("PATH"))
        putenv("PATH=/usr/bin:/usr/sbin:/bin:/sbin");
    /* 默认主目录 /root(提示符 ~ 与 cd 无参数都指这里);
     * 有 HOME 则保留。 */
    if (!getenv("HOME"))
        putenv("HOME=/root");

    if (argc > 1) {
        /* -c: 后一个参数是命令行(POSIX sh 语义)。guest 场景:
         * 脚本解释器头、外部程序调 shell 都用它。
         * 形如 sh -c 'cmd1; cmd2'(分号分隔)逐条 run_line 执行。 */
        int ci = 1;
        while (ci < argc && argv[ci][0] == '-' && argv[ci][1] != 0) {
            if (argv[ci][1] == 'c' && ci + 1 < argc)
                return run_code(argv[ci + 1]);
            ci++;
        }
        /* 非 -c: 有文件参数(或裸 -x 等)时按脚本文件执行 */
        const char *script = (argv[ci] && argv[ci][0] != '-')
                                ? argv[ci] : NULL;
        if (script)
            return run_script(script);
        /* 只有开关没有脚本: 交互模式 */
    }

    /* 交互模式:装 SIGINT handler(Ctrl+C 中断当前命令,shell 继续) */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

    char cwd[PATH_MAX];
    /* 交互 shell 默认从主目录(缺省 /root)起,而非 / */
    {
        const char *home = getenv("HOME");
        if (!home)
            home = "/root";
        if (chdir(home) != 0)
            home = "/";
    }
    if (!getcwd(cwd, sizeof cwd))
        strcpy(cwd, "/");
    char line[512];
    char prompt[PATH_MAX + 64];
        printf(bash_mode ? "Parlz bash - 'help' 查看命令, 'exit' 退出\n"
                         : "Parlz shell - 'help' 查看命令, 'exit' 退出\n");
    /* 交互 shell:开 raw 模式,让 ESC[A / ESC[B 被识别为上下键(历史)。
     * raw 关掉 ICANON/ECHO/ISIG,ICRNL 让 \n 统一,VMIN=1 读 1 字节即返回。
     * 非交互(脚本模式)不动。 */
    int interactive = isatty(STDIN_FILENO);
    sh_interactive = interactive;
    /* 交互时把自己扶成"会话主进程 + /dev/console 当控制终端"。
     * busybox-init 的 ::sysinit 子进程不是 session leader, /dev/console
     * 成不了控制终端 —— 终端 ISIG 产生的 SIGINT 只能发给控制终端的
     * 前台进程组, 没有 ctty 时谁都不发, Ctrl+C 于是杀不掉前台命令
     * (用户实测"Ctrl+C 不能强制退出进程")。setsid 后 TIOCSCTTY 接管。 */
    if (interactive) {
        if (getsid(0) != getpid())
            setsid();
        int cfd = open("/dev/console", O_RDWR);
        if (cfd >= 0) {
            if (ioctl(cfd, TIOCSCTTY, 0) == 0) {
                dup2(cfd, STDIN_FILENO);
                dup2(cfd, STDOUT_FILENO);
                dup2(cfd, STDERR_FILENO);
            }
            if (cfd > 2)
                close(cfd);
        }
    }
    struct termios saved_tio, raw_tio;
    int raw_on = 0;
    if (interactive) {
        if (tcgetattr(STDIN_FILENO, &saved_tio) == 0) {
            raw_tio = saved_tio;
            raw_tio.c_lflag &= ~(ICANON | ECHO | ISIG);
            raw_tio.c_lflag |= ICRNL;
            raw_tio.c_cc[VMIN] = 1;
            raw_tio.c_cc[VTIME] = 0;
            /* TCSANOW: 启动前已经在 tty 输入队列里的行(比如脚本喂进来的
             * 第一条命令)不能被丢掉 */
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio) == 0)
                raw_on = 1;
        }
    }
    int got_line = 0;
    for (;;) {
        make_prompt(prompt, sizeof prompt, cwd, bash_mode);
        printf("%s", prompt);
        fflush(stdout);
        got_line = 0;
        if (raw_on) {
            /* 逐字符读 + ESC[A / ESC[B 识别上下键历史;
             * Ctrl+C(0x03) 直接 ^C 回新提示符(不进 run_line)。 */
            int col = 0;
            line[0] = 0;
            int esc_seen = 0;
            int hist_active = 0;
            char hist_new[512] = {0};
            char cur[512] = {0};
            for (;;) {
                unsigned char c;
                ssize_t r = read(STDIN_FILENO, &c, 1);
                if (r <= 0) {
                    /* r==0 不该发生(VMIN=1);r<0:EOF 或 EINTR(Ctrl+C) */
                    if (errno == EINTR) {
                        printf("^C\n");
                        fflush(stdout);
                        line[0] = 0;
                        col = 0;
                        break;
                    }
                    if (line[0] || col > 0) {
                        got_line = 1;
                        break;
                    }
                    break;        /* 真 EOF */
                }
                if (esc_seen) {
                    /* ESC + 方向键序列 */
                    esc_seen = 0;
                    if (c == '[' || c == 'O') {
                        unsigned char seq;
                        ssize_t r2 = read(STDIN_FILENO, &seq, 1);
                        if (r2 > 0) {
                            if (seq == 'A') {
                                /* ↑ 往旧: 首次进历史编辑,后续逐条上翻 */
                                if (!hist_active) {
                                    hist_active = 1;
                                    hist_new[0] = 0;
                                    snprintf(hist_new, sizeof hist_new,
                                             "%s", line);
                                    col = 0;
                                    line[0] = 0;
                                }
                                if (!hist_move(-1, cur, sizeof cur)) {
                                    /* 越界(最旧): 恢复原行 */
                                    hist_active = 0;
                                    snprintf(line, sizeof line, "%s",
                                             hist_new);
                                    col = (int)strlen(line);
                                } else {
                                    snprintf(line, sizeof line, "%s", cur);
                                    col = (int)strlen(line);
                                    printf("\r\033[K%s%s", prompt, line);
                                    fflush(stdout);
                                }
                            } else if (seq == 'B') {
                                /* ↓ 往新 */
                                if (!hist_active) {
                                    /* 非历史态按 ↓: 无操作(已是新行) */
                                } else if (!hist_move(+1, cur, sizeof cur)) {
                                    /* 越界(最新): 回原行,退出历史编辑 */
                                    hist_active = 0;
                                    snprintf(line, sizeof line, "%s",
                                             hist_new);
                                    col = (int)strlen(line);
                                    printf("\r\033[K%s%s", prompt, line);
                                    fflush(stdout);
                                } else {
                                    snprintf(line, sizeof line, "%s", cur);
                                    col = (int)strlen(line);
                                    printf("\r\033[K%s%s", prompt, line);
                                    fflush(stdout);
                                }
                            }
                        }
                        continue;
                    }
                    /* 非方向键 ESC: 当普通 ESC 字符,忽略 */
                    continue;
                }
                if (c == 0x1b) {
                    esc_seen = 1;
                    continue;
                }
                if (c == 0x03) {
                    /* Ctrl+C: 打 ^C, 保留当前行内容 */
                    printf("^C\n");
                    fflush(stdout);
                    hist_active = 0;
                    break;
                }
                if (c == '\n' || c == '\r') {
                    /* 回车:提交。raw 模式(关 ICANON)下终端不做 \n→\r\n
                     * 回显转换, 这里补一个换行, 否则下一个提示符会
                     * 粘在本行末尾(用户看到 "parlz@(none):/> parlz@(none):/>") */
                    if (hist_active) {
                        /* 提交后不再编辑,清历史编辑态 */
                        hist_active = 0;
                    }
                    line[col] = 0;
                    got_line = 1;
                    fflush(stdout);
                    printf("\n");
                    break;
                }
                if (c == 0x7f || c == 0x08) {
                    /* 退格/Delete: 删一个字符并整行重打 */
                    if (col > 0) {
                        col--;
                        line[col] = 0;
                        /* 重打提示符 + 当前行(简化:整行重打) */
                        printf("\r\033[K%s%s", prompt, line);
                        fflush(stdout);
                    }
                    continue;
                }
                if (c >= 0x20 && c != 0x7f) {
                    /* 可打印字符: 追加到行尾并手动回显(raw 关了 ECHO) */
                    if (col < (int)sizeof line - 1) {
                        line[col++] = (char)c;
                        line[col] = 0;
                        fputc((int)c, stdout);
                        fflush(stdout);
                    }
                    continue;
                }
                /* 其他控制字符忽略 */
            }
            /* 行读结束: 清掉 raw 里可能的残留 ESC,回 canonical */
            if (got_line) {
                /* 提交: 记录历史(非空行才记) */
                int empty = 1;
                for (char *q = line; *q; q++)
                    if (*q != ' ' && *q != '\t') {
                        empty = 0;
                        break;
                    }
                if (!empty)
                    hist_record(line);
            }
        } else {
            /* 非交互: 保持旧 fgets 路径 */
            got_line = (fgets(line, sizeof line, stdin) != NULL);
            if (!got_line) {
                if (errno == EINTR && sigint_seen) {
                    sigint_seen = 0;
                    printf("^C\n");
                    fflush(stdout);
                    got_line = 0;
                    line[0] = 0;
                }
            }
            if (!got_line)
                break;
        }
        if (!got_line)
            break;
        if (sigint_seen) {
            sigint_seen = 0;
            printf("^C\n");
            fflush(stdout);
        }
        if (getcwd(cwd, sizeof cwd))      /* cd 可能改目录 */
            ;
        /* 执行命令前恢复规范模式: 子进程(bash/nano/外部命令)若继承
         * raw 终端,行编辑/实时回显会失效。读下一行前再进 raw。
         * ★ 两处都不能用 TCSAFLUSH —— 它会把"命令执行期间敲进来/粘进来
         * 的行"整段丢掉, 表现就是连贴三条命令只有第一条跑。进 raw 用
         * TCSANOW, 出 raw 用 TCSADRAIN(只等输出, 不清输入队列)。 */
        if (raw_on)
            tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_tio);
        /* 一行可以带多条命令(`cd /tmp; ls`), 分句在 run_cmds 里做(认引号);
         * "退出" 由 sh_exit_req 传(返回值是最后一条命令的退出码) */
        run_cmds(line, cwd, sizeof cwd);
        if (sh_exit_req)
            break;
        /* 恢复 raw 模式继续逐字符回显与方向键历史 */
        if (raw_on)
            tcsetattr(STDIN_FILENO, TCSANOW, &raw_tio);
        if (sigint_seen) {
            sigint_seen = 0;
            printf("^C\n");
            fflush(stdout);
        }
    }
    /* 恢复原始终端属性,避免 raw 模式泄漏到后续进程 */
    if (raw_on)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
    hist_clear();
    printf("bye\n");
    return 0;
}
