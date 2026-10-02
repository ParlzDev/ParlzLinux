/* init.c - Parlz 用户空间 PID 1。
 * 挂载 proc/sys/devtmpfs/tmp(跳过已挂载的),然后:
 *   - 探测可挂载的根设备(优先 /dev/vda1 分区,回退整盘),挂到 /mnt
 *   - 若 /install.d 存在(ISO 安装盘):执行安装脚本,完成后进 shell
 *   - 配置网卡(等 /sys/class/net 出现设备再 ifc)
 * 交互 shell 的 exec 失败时回退,避免 init 退出导致内核 panic。
 */
#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/reboot.h>
#include <sys/wait.h>

static void mnt(const char *src, const char *dst, const char *type,
                unsigned long flags, const char *data)
{
    if (mkdir(dst, 0755) < 0 && errno != EEXIST)
        fprintf(stderr, "init: mkdir %s: %s\n", dst, strerror(errno));
    if (mount(src, dst, type, flags, data) < 0)
        fprintf(stderr, "init: mount %s -> %s: %s\n", src, dst,
                strerror(errno));
}

/* 读 /proc/cmdline 判断是否带指定参数 */
static int cmdline_has(const char *key)
{
    FILE *f = fopen("/proc/cmdline", "r");
    char buf[1024];
    int found = 0;
    if (f) {
        if (fgets(buf, sizeof buf, f))
            found = strstr(buf, key) != NULL;
        fclose(f);
    }
    return found;
}

static int cmdline_root(char *out, size_t n)
{
    /* 根设备优先级:
     *   1. root=<dev>  显式指定(磁盘自启时 syslinux.cfg 写 root=/dev/vda2)
     *   2. root=       未给值(ISO 引导的 "root=" 兜底):自动探测分区
     *                   —— 内核已注册 vda,缺 dev 值时回落到 /dev 扫描 */
    char buf[1024];
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f)
        return 0;
    size_t total = 0;
    size_t r;
    while ((r = fread(buf + total, 1, sizeof buf - total - 1, f)) > 0)
        total += r;
    buf[total] = 0;
    fclose(f);
    /* 找 root= */
    char *p = strstr(buf, "root=");
    if (!p)
        return 0;
    p += 5;
    char *end = p;
    while (*end && *end != ' ')
        end++;
    size_t len = (size_t)(end - p);
    if (len == 0)
        return 0;            /* 无 root= 值:probe_root 回落 /dev 扫描 */
    if (len >= n)
        len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

/* 探测可挂载的根设备:
 * 1) 若 cmdline 带 root=/dev/xxx,用它
 * 2) 否则扫描 /dev,找第一个存在的块设备分区(vdX1/sdX1),其次整盘
 * 返回 0 表示未找到。 */
static int probe_root(char *out, size_t n)
{
    if (cmdline_root(out, n))
        return 1;

    DIR *d = opendir("/dev");
    if (!d)
        return 0;
    struct dirent *e;
    const char *cand = NULL;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char full[128];
        snprintf(full, sizeof full, "/dev/%s", e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISBLK(st.st_mode))
            continue;
        /* 只认带分区号(末尾数字)的块设备:ext2 root 在分区 2,
         * 整盘(vdX/sdX 末尾无数字)无 ext2 签名,跳过,避免扫到 vda/sr0 */
        size_t L = strlen(e->d_name);
        if (L < 4 || !isdigit(e->d_name[L - 1]))
            continue;
        if (strncmp(e->d_name, "vd", 2) == 0)
            cand = e->d_name;
        else if (strncmp(e->d_name, "sd", 2) == 0 && !cand)
            cand = e->d_name;
    }
    closedir(d);
    if (!cand)
        return 0;
    snprintf(out, n, "/dev/%s", cand);
    return 1;
}

/* 在 /mnt 上尝试挂载 rootdev(按 ext4/ext2/vfat 顺序,成功返回 1)。
 * 适配 syslinux 双分区布局:分区 1 = FAT16 引导分区(vfat),
 * 分区 2 = root(ext2,install 格式化后挂载并 pivot 真根)。 */
static int try_mount_root(const char *rootdev)
{
    if (mkdir("/mnt", 0755) < 0 && errno != EEXIST)
        return 0;
    if (mount(rootdev, "/mnt", "ext4", 0, NULL) == 0 ||
        mount(rootdev, "/mnt", "ext2", 0, NULL) == 0 ||
        mount(rootdev, "/mnt", "vfat", 0, NULL) == 0)
        return 1;
    return 0;
}

/* pivot 真根:/mnt(已挂 ext2 分区 2)换成 /,initramfs 留 /mnt/oldroot。
 * 失败不 panic —— 继续跑在 initramfs 上(分区挂载失败时兜底)。
 * pivot_root 非 POSIX,glibc 不导出;内核 UAPI 提供,手动声明。 */
int pivot_root(const char *new_root, const char *put_old);

static void pivot_root_to_mnt(void)
{
    if (pivot_root("/mnt", "/mnt/oldroot") == 0) {
        if (mount(NULL, "/", NULL, MS_NOSUID | MS_NODEV | MS_RDONLY,
                  NULL) < 0) {
            fprintf(stderr, "init: remount / (pivot): %s\n",
                    strerror(errno));
        }
        /* /dev 换绑回 devtmpfs: pivot 后 /dev 继承的是 initramfs
         * 旧根里的目录(无驱动节点), 须在新根上重挂 devtmpfs,
         * 内核注册的 /dev/snd/pcmC0D0p 等节点才可见。 */
        mount("devtmpfs", "/dev", "devtmpfs",
              MS_NOSUID | MS_NODIRATIME, "mode=0755");
        printf("init: pivot_root -> ext2 root partition (initramfs at /mnt/oldroot), /dev 已换绑 devtmpfs\n");
    } else {
        printf("init: pivot_root failed (%s), continuing on initramfs\n",
               strerror(errno));
    }
}

int main(void)
{
    /* TERM:ncurses 系程序(nano)需要。ttyS0 串口最接近 vt220,
     * 已打包进 initramfs 的 terminfo(/usr/share/terminfo,
     * /etc/terminfo 软链)。显式设置,覆盖可能继承的 linux。 */
    /* nano 的 shx → bash 分流 */
    setenv("SHELL", "/bin/bash", 1);
    /* TERM: 交互 shell 由 bash 启动, TERM 在此设给 shell 及其子进程(nano 等
     * ncurses 程序)。init 本身不跑 ncurses, 不设 TERM 不影响 ifc/nettest。 */
    /* TERM/TERMINFO: nano(ncurses)需要。ttyS0 串口最接近 vt220, 已打包
     * terminfo 条目(/usr/share/terminfo)。显式设置覆盖可能继承的 linux。
     * TERMINFO_DIRS 指包内目录, 避免 ncurses stat 缺失宿主路径。 */
    setenv("TERM", "vt220", 1);
    setenv("TERMINFO_DIRS", "/etc/terminfo:/usr/share/terminfo", 1);

    /* HOME:主目录 /root(与 shell 提示符 ~ 及 cd 无参数一致)。
     * /root 目录由 build-userland.sh 在 rootfs 里预建。 */
    setenv("HOME", "/root", 1);
    setenv("PATH", "/usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin", 1);
    setenv("SHELL", "/bin/bash", 1);
    /* USER: login 成功后 setenv("USER", 登录用户名) 覆盖;无凭证/
     * 非交互场景下 shell 提示符回落 getpwuid(0)=root 或 "parlz"。 */

    /* LS_COLORS:guest 串口(QEMU tty)下 isatty 判定与主机终端不同,
     * 显式开 ls 颜色(目录蓝/文件绿);设 NO_COLOR=1 可关。 */
    setenv("LS_COLORS", "1", 1);

    /* terminfo 数据库:ncurses(nano)按 $TERM 查 $TERMINFO_DIRS 三个固定
     * 目录(/etc/terminfo /lib/terminfo /usr/share/terminfo)。initramfs 只
     * 有 /etc/terminfo→/usr/share/terminfo 软链(宿主 WSL 本身无
     * /lib/terminfo)。静态 ncurses 对这三个目录逐个 stat,缺一个就报
     * "cannot initialize terminal type" 退出(nano 实测)。显式设
     * TERMINFO_DIRS 只指包内目录,ncurses 直接命中、不再 stat 缺失目录。 */
    setenv("TERMINFO_DIRS", "/etc/terminfo:/usr/share/terminfo", 1);

    /* PULSE_EMOJI_FONT_PATH 等无关;保持最小。 */

    printf("=== Parlz user-space (init.c legacy fallback) ===\n");
    fflush(stdout);

    mnt("proc",     "/proc", "proc",     0, NULL);
    mnt("sysfs",    "/sys",   "sysfs",    0, NULL);
    /* devtmpfs:内核 CONFIG_DEVTMPFS_MOUNT 可能已自动挂载,
     * mount() 返回 EBUSY 时无需处理(已挂载即成功)。 */
    if (mount("devtmpfs", "/dev", "devtmpfs",
              MS_NOSUID | MS_NODIRATIME, "mode=0755") < 0 &&
        errno != EBUSY)
        fprintf(stderr, "init: mount devtmpfs -> /dev: %s\n",
                strerror(errno));
    /* Bash 进程替换通过 /dev/fd 访问继承的管道。 */
    if (symlink("/proc/self/fd", "/dev/fd") < 0 && errno != EEXIST)
        perror("init: /dev/fd");
    mkdir("/dev/pts", 0755);
    mnt("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, NULL);
    mnt("tmpfs",    "/tmp",   "tmpfs",    MS_NOSUID | MS_NODIRATIME,
        "mode=1777");

    /* 根设备探测:优先 cmdline root=(磁盘自启时 syslinux.cfg 写
     * root=/dev/vda2),否则扫 /dev 找分区。ext2 分区 2 挂载后
     * pivot_root 换真根;挂不上时继续跑 initramfs 兜底(不 panic)。 */
    char rootdev[128];
    int root_mounted = 0;
    if (probe_root(rootdev, sizeof rootdev)) {
        printf("init: root device %s, trying mount...\n", rootdev);
        fflush(stdout);
        if (try_mount_root(rootdev)) {
            printf("init: mounted %s at /mnt\n", rootdev);
            root_mounted = 1;
        } else
            printf("init: mount %s failed (empty partition yet), "
                   "staying on initramfs\n", rootdev);
    } else {
        printf("init: no root device found (fresh disk?), "
               "staying on initramfs\n");
    }
    fflush(stdout);

    /* pivot 真根:仅当分区 2(ext2,install 已格式化)挂载成功才 pivot;
     * 空分区/失败时保持 initramfs 运行(pivot 到空分区会让 shell 没 bin)。
     * ext2 magic = 0xEF53 (statfs f_type)。 */
    {
        struct statfs sfs;
        if (root_mounted && statfs("/mnt", &sfs) == 0 &&
            sfs.f_type == 0xEF53)
            pivot_root_to_mnt();
        else if (root_mounted)
            printf("init: /mnt not ext2 (magic 0x%08lx), skip pivot\n",
                   (unsigned long)sfs.f_type);
    }

    /* 网卡配置:ifc auto 自动探测第一个非 lo 接口并配置。
     * cmdline 参数:
     *   ifconfig=<name>  指定接口名(替代 auto)
     *   network=         跳过自动配置(用户自己配) */
    {
        FILE *f = fopen("/proc/cmdline", "r");
        char cbuf[1024] = "";
        if (f && fgets(cbuf, sizeof cbuf, f))
            cbuf[strcspn(cbuf, "\n")] = 0;
        if (f)
            fclose(f);
        int user_net = strstr(cbuf, "network=") != NULL;
        if (!user_net) {
            char ifarg[32];
            const char *ifc_arg = "auto";
            char *ifcfg = strstr(cbuf, "ifconfig=");
            if (ifcfg) {
                int k = 0;
                ifcfg += 9;
                while (*ifcfg && *ifcfg != ' ' && k < 31)
                    ifarg[k++] = *ifcfg++;
                if (k > 0) {
                    ifarg[k] = 0;
                    ifc_arg = ifarg;
                }
            }
            printf("init: configuring network via /bin/ifc %s\n", ifc_arg);
            fflush(stdout);
            /* ifc 内部已含 60s 重试 + 接口重命名跟踪,init 只调一次 */
            int net_ok = 0;
            for (int try = 0; try < 1; try++) {
                pid_t np = fork();
                if (np == 0) {
                    execl("/bin/ifc", "ifc", (char *)ifc_arg, "10.0.2.15",
                          "255.255.255.0", "10.0.2.2", (char *)NULL);
                    _exit(127);
                }
                int nst;
                waitpid(np, &nst, 0);
                if (WEXITSTATUS(nst) == 0) {
                    printf("init: network up (try %d)\n", try + 1);
                    net_ok = 1;
                    break;
                }
                if (try < 2) {
                    printf("init: ifc failed, retry in 10s...\n");
                    fflush(stdout);
                    for (int s = 0; s < 100; s++)
                        sleep(1);
                }
            }
            if (!net_ok)
                printf("init: network config failed, shell 里可用 ifc/ifconfig 手动配\n");
        }
    }

    /* 安装模式:
     * 触发条件(满足其一):
     *   - cmdline 带 parlz.install(显式要求,保留);
     *   - 有 /install.d 且磁盘上没有 install-done 标记(ISO 自举,
     *     SeaBIOS 引导时不带 cmdline,靠此兜底触发安装)。
     * 标记机制:install 完成后整盘 pwrite "parlz install done" 到
     * LBA 24576(boot 分区末尾后 1 扇区,install.c [5b])。
     * 磁盘自举后 init 看到标记就跳过安装直接进 shell。 */
    {
        /* 探测安装标记:读整盘 LBA 1(MBR 与 boot 分区之间的空闲区,
         * 与 install.c [5b] 写入位置一致)。 */
        int installed = 0;
        char dpath[128];
        if (probe_root(dpath, sizeof dpath)) {
            /* 整盘路径:去掉末尾分区号(/dev/vda1 -> /dev/vda) */
            char wpath[128];
            strcpy(wpath, dpath);
            size_t wl = strlen(wpath);
            if (wl > 0 && isdigit(wpath[wl - 1]))
                wpath[wl - 1] = 0;
            int fd = open(wpath, O_RDONLY);
            if (fd >= 0) {
                /* LBA 1:install.c [5b] 写 "parlz install done" */
                char buf[512];
                if (pread(fd, buf, 512, 1L * 512) == 512)
                    for (size_t bi = 0; bi + 18 <= 512; bi++)
                        if (memcmp(buf + bi, "parlz install done", 18) == 0)
                            installed = 1;
                close(fd);
            }
        }

        int want_install = cmdline_has("parlz.install") ||
                          (access("/install.d", X_OK) == 0 && !installed);
        if (want_install) {
            printf("init: running /install.d%s\n",
                   installed ? " (reinstall)" : "");
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                execl("/bin/sh", "sh", "/install.d", (char *)NULL);
                _exit(127);
            }
            int st;
            waitpid(pid, &st, 0);
            printf("init: install.d finished (status %d)\n", st);
            fflush(stdout);
        } else if (access("/install.d", X_OK) == 0) {
            printf("init: install already done (marker present), skipping\n");
        }
    }

    /* 网络自测脚本:若 /nettest.sh 存在,fork 跑它,输出到串口。
     * 120s 看门狗:超时 SIGKILL,避免脚本卡死挂住 init。 */
    if (access("/nettest.sh", X_OK) == 0) {
        printf("init: running /nettest.sh (120s 看门狗)\n");
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/sh", "sh", "/nettest.sh", (char *)NULL);
            _exit(127);
        }
        int st;
        int ntest_wait = 0;
        for (int s = 0; s < 120; s++) {
            int r = waitpid(pid, &st, WNOHANG);
            if (r == pid) {
                ntest_wait = 1;
                break;
            }
            sleep(1);
        }
        if (ntest_wait)
            printf("init: nettest.sh finished (status %d)\n",
                   WEXITSTATUS(st));
        else {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            printf("init: nettest.sh 卡死 120s,已杀 (status %d)\n",
                   WEXITSTATUS(st));
        }
        fflush(stdout);
    }

    /* 音频自测钩子:若 /audiotest.sh 存在(仅 audio-verify.sh 注入构建的
     * initramfs 会带),fork 跑它 —— 它生成 1KHz 正弦 WAV 并播放出声,
     * 供宿主 FFT 校验。60s 看门狗防卡死。缺文件则静默跳过(不影响正常启动)。 */
    /* 工具链: 预编译 GCC 15.2 / Clang+LLVM 21.1 由 pm 按需安装
     * (pm install gcc / pm install clang), 默认 initramfs 不带。
     * 包内 gcc 驱动默认 -I/usr/include(包内已带宿主系统头), as/ld 走
     * /usr/bin 包内软链, 裸 `gcc ./a.c` 无需 -I/-B/-L 即命中。
     *
     * 动态编译支持: 包默认走动态链接(无 -static), 产物 NEEDED libc.so.6,
     * 解释器 /lib64/ld-linux-x86-64.so.2(包内置)。动态产物直跑靠下面
     * LD_LIBRARY_PATH=/lib/toolchain —— 由 init 设, 之后 pm install 装的
     * 动态产物直接 ./a 能跑。
     *
     * 默认 PIE: 包内 gcc 驱动 `--enable-default-pie` 使 `gcc a.c` 出 PIE
     * 动态产物; guest 内核(7.2.5 移植)对 PIE 动态产物加载异常
     * (BIND_NOW + 全 R_X 加载期重定位写只读段 -> segfault), 故默认
     * CFLAGS 加 -no-pie, 产物仍动态(非静态)但走非 PIE 段布局。
     * 显式 `gcc -static` 仍可出静态产物; 显式 `gcc -pie` 可出 PIE。 */
    setenv("CFLAGS", "-no-pie", 1);
    setenv("CXXFLAGS", "-no-pie", 1);
    setenv("LDFLAGS", "-no-pie", 1);
    setenv("LD_LIBRARY_PATH",
           "/lib/toolchain:/opt/toolchain/gcc-15/lib:/opt/toolchain/llvm-21/lib", 1);
    /* LIBRARY_PATH: gcc 驱动链接期库搜索路径默认值。包内 glibc .so 在
     * /lib/toolchain, 裸 `gcc a.c`(动态) 链接时自动找到, 无需 -L。 */
    setenv("LIBRARY_PATH",
           "/lib/toolchain:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu", 1);
    /* CPATH: 把工具链 include 加到全局搜索路径(等价 C_INCLUDE_PATH,
     * C/C++ 通用)。裸 gcc/clang ./a.c 无需 -I 即命中包内系统头。
     * 注意: 只放包内 include 与 C++ 标准库路径, 不要放 /usr/include 首项
     * —— 包内 /usr/include(宿主 glibc 2.43 头)与 /opt/toolchain/gcc-15/include
     * (glibc 头副本)内容重叠, 同路径重复 -I 会让 __GLIBC_USE 等宏二次
     * 定义, 报 "missing binary operator" 类语法错。 */
    setenv("CPATH",
           "/opt/toolchain/gcc-15/include:/usr/include/c++/15", 1);
    /* C_INCLUDE_PATH: C 专用(与 CPATH 双保险, 兼容只认 C_INCLUDE_PATH 的编译器)。
     * 同样不放 /usr/include 首项, 避免与包内头副本撞库。 */
    setenv("C_INCLUDE_PATH", "/opt/toolchain/gcc-15/include", 1);
    /* pm feed: **默认就是官方镜像站** www.parlz.com/feed(Packages 与 *.pm
     * 都在那儿, 见 web/feed = output/feed 的同一份内容)。
     * 这里以前默认写的是宿主开发机 10.0.2.2:8765 —— 它比盘上的
     * /etc/pm/feeds.conf 优先级还高(PM_FEED 环境变量优先于 feeds.conf),
     * 于是"盘上写着官网、pm 实际连开发机"。要换源(自建镜像/离线)用
     * cmdline parlz.feed= 或自己 export PM_FEED —— 下面 setenv 不覆盖已有值。 */
    if (!getenv("PM_FEED"))
        setenv("PM_FEED", "http://www.parlz.com/feed", 0);

    /* 工具链自测钩子: 若 /tooltest.sh 存在(仅 toolchain-verify.sh 注入的
     * initramfs 会带), fork 跑它 —— 它在 guest 内跑 gcc/clang 各编译一条
     * C/C++ 语句(动态 + 静态), 并运行产物, 验证整条工具链在无 glibc 的
     * initramfs 里自包含可用。120s 看门狗(编译较慢)。缺文件则静默跳过。 */
    if (access("/tooltest.sh", X_OK) == 0) {
        printf("init: running /tooltest.sh (120s 看门狗)\n");
        fflush(stdout);
        pid_t tpid = fork();
        if (tpid == 0) {
            /* bash 直执 tooltest.sh: 宿主 WSL 中间层吞 heredoc(0 字节源文件),
             * tooltest.sh.in 已改 printf 生成源码, 直接可跑。 */
            execl("/bin/bash", "bash", "/tooltest.sh", (char *)NULL);
            _exit(127);
        }
        int t_wait = 0;
        int tst = 0;
        for (int s = 0; s < 120; s++) {
            int r = waitpid(tpid, &tst, WNOHANG);
            if (r == tpid) {
                t_wait = 1;
                break;
            }
            sleep(1);
        }
        if (!t_wait) {
            kill(tpid, SIGKILL);
            waitpid(tpid, &tst, 0);
        }
        printf("init: tooltest.sh finished (status %d)\n",
               t_wait ? WEXITSTATUS(tst) : -1);
        fflush(stdout);
    }

    /* pm 包管理器自测钩子: 若 /pmtest.sh 存在(pm-verify.sh 注入的构建带),
     * fork 跑它 —— guest 内 pm install gcc/clang + 裸命令动态编译运行 +
     * pm list + nano terminfo 验收。
     * 看门狗 1500s: 工具链包已到 GB 级(gcc 546 MiB + clang 1017 MiB), 走的
     * 还是模拟 user-NAT + 解 cpio 进 ramfs, 240s 是包还只有几十 MB 时代的值
     * —— 超时就 kill, 判据会报成"产品失败", 其实是没给够时间(以前踩过)。 */
    if (access("/pmtest.sh", X_OK) == 0) {
        printf("init: running /pmtest.sh (1500s 看门狗)\n");
        fflush(stdout);
        pid_t ppid = fork();
        if (ppid == 0) {
            execl("/bin/bash", "bash", "/pmtest.sh", (char *)NULL);
            _exit(127);
        }
        int p_wait = 0;
        int pst = 0;
        for (int s = 0; s < 1500; s++) {
            int r = waitpid(ppid, &pst, WNOHANG);
            if (r == ppid) {
                p_wait = 1;
                break;
            }
            sleep(1);
        }
        if (!p_wait) {
            kill(ppid, SIGKILL);
            waitpid(ppid, &pst, 0);
        }
        printf("init: pmtest.sh finished (status %d)\n",
               p_wait ? WEXITSTATUS(pst) : -1);
        fflush(stdout);
    }

    if (access("/audiotest.sh", X_OK) == 0) {
        printf("init: running /audiotest.sh (60s 看门狗)\n");
        fflush(stdout);
        pid_t apid = fork();
        if (apid == 0) {
            execl("/bin/sh", "sh", "/audiotest.sh", (char *)NULL);
            _exit(127);
        }
        int a_wait = 0;
        int ast = 0;
        for (int s = 0; s < 60; s++) {
            int r = waitpid(apid, &ast, WNOHANG);
            if (r == apid) {
                a_wait = 1;
                break;
            }
            sleep(1);
        }
        if (!a_wait) {
            kill(apid, SIGKILL);
            waitpid(apid, &ast, 0);
        }
        printf("init: audiotest.sh finished (status %d)\n",
               a_wait ? WEXITSTATUS(ast) : -1);
        fflush(stdout);
    }

    /* 声卡设备节点兜底: 驱动绑定后 devtmpfs 未自动建 /dev/snd/* 时
       (pivot_root 后 /dev 未换绑、节点延迟注册等场景), 手动 mknod 补建。
       存在且是字符节点则跳过; 驱动未绑定则不建(建了也打不开)。 */
    {
        struct stat sb;
        const char *drv = "/sys/bus/pci/drivers/snd-intel8x0";
        const char *hda = "/sys/bus/pci/drivers/snd-hda-intel";
        if (access(drv, F_OK) == 0 || access(hda, F_OK) == 0) {
            /* ALSA major 动态分配, 从 /proc/devices 查真实值, 缺省 116 */
            int major = 116;
            FILE *f = fopen("/proc/devices", "r");
            if (f) {
                char line[256];
                int in_char = 0;
                while (fgets(line, sizeof line, f)) {
                    if (!strcmp(line, "Character devices:\n"))
                        in_char = 1;
                    else if (in_char && strstr(line, "snd ")) {
                        int m = atoi(line);
                        if (m > 1)
                            major = m;
                    }
                }
                fclose(f);
            }
            struct {
                const char *p;
                unsigned short minor;
            } nodes[] = {
                { "pcmC0D0p", 0x1100 },
                { "pcmC0D0c", 0x1101 },
                { "pcmC0D1c", 0x1105 },
            };
            mkdir("/dev/snd", 0755);
            for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
                char path[64];
                snprintf(path, sizeof path, "/dev/snd/%s", nodes[i].p);
                if (stat(path, &sb) == 0 && S_ISCHR(sb.st_mode))
                    continue;
                dev_t dv = ((dev_t)major << 20) | (unsigned)nodes[i].minor;
                if (mknod(path, S_IFCHR | 0666, (mode_t)dv) == 0)
                    printf("init: mknod %s (major %d)\n", path, major);
            }
        }
    }

    /* 登录认证:/etc/parlz-auth 存在则校验用户名+密码,不存在则引导
     * 首次设置。失败(输错/EOF)不进交互 shell。
     * /etc 在 initramfs(cpio rootfs)与 ext2 根(分区 2 可写挂载)均可写,
     * 凭证跨重启持久保存在 ext2 分区 2 的 /etc/parlz-auth。
     * 交互性判定:login 的 stdin 已指 /dev/console,若 isatty(0)==1
     * (QEMU -serial stdio 或虚拟终端)走交互认证;若 stdin 被重定向
     * (自动化 file: 捕获,如 login-verify/boot-verify)非 tty,login 直接
     * 放行,不阻塞自动化测试。 */
    {
        printf("init: login check\n");
        fflush(stdout);
        pid_t lpid = fork();
        if (lpid == 0) {
            int cfd = open("/dev/console", O_RDWR);
            if (cfd >= 0) {
                dup2(cfd, STDIN_FILENO);
                dup2(cfd, STDOUT_FILENO);
                dup2(cfd, STDERR_FILENO);
                if (cfd > 2)
                    close(cfd);
            }
            execl("/bin/login", "login", (char *)NULL);
            _exit(127);
        }
        int lst;
        waitpid(lpid, &lst, 0);
        if (WEXITSTATUS(lst) != 0) {
            printf("init: login rejected (status %d), skipping interactive shell\n",
                   WEXITSTATUS(lst));
            fflush(stdout);
            /* 认证失败:不进交互 shell,保持 init 存活(不 panic) */
            for (;;)
                sleep(3600);
        }
    }

    /* 进交互 shell:fork + wait,不让 PID 1 退出(否则 kernel panic)。
     * shell 退出(用户 exit)后重新拉起;循环退出 = 系统关机。
     * 子进程把 0/1/2 都 dup2 到 /dev/console(= ttyS0):让 sh 里
     * isatty(0)==1,走 raw 模式逐字符回显(否则 QEMU mon:stdio 下
     * sh 只能整行读,输入要过一会才显示一次)。 */
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            int cfd = open("/dev/console", O_RDWR);
            if (cfd >= 0) {
                dup2(cfd, STDIN_FILENO);
                dup2(cfd, STDOUT_FILENO);
                dup2(cfd, STDERR_FILENO);
                if (cfd > 2)
                    close(cfd);
            }
            execl("/bin/sh", "sh", (char *)NULL);
            perror("init: exec /bin/sh");
            _exit(127);
        }
        int st;
        waitpid(pid, &st, 0);
        printf("init: shell exited (status %d), relaunching...\n",
               WEXITSTATUS(st));
        fflush(stdout);
    }
    /* 不可达:上面 for(;;) 永不返回 */
}
