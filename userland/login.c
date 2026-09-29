/* login.c - Parlz 登录认证(多账户)。
 * 用法: login(无参数,交互)
 *   - /etc/parlz-auth 没有账户(首次进入):设置用户名 + 密码(两次确认),
 *     成功后直接放行;
 *   - 已有账户:提示 Username/Password 校验, 通过后放行。
 * 账户文件读写与口令散列都在 parlzauth.c(login 与 user 命令共用一份)。
 * 认证通过后把用户名写进 PA_WHOAMI_PATH —— login 是子进程, setenv 传不回
 * 启动 body, body 靠这个文件导出 $USER(以前它读的是文件第一行, 多账户下
 * 会张冠李戴)。
 * 密码读取:tty 时关回显逐字符读、'*' 占位(pa_read_pass)。
 * 非 tty(自动化脚本/串口非交互)与 cmdline login.skip 直接放行。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "parlzauth.h"

int main(void)
{
    /* 控制台重接: 宿主 pty 测试场景保留原 pty stdin(宿主 /dev/console
     * 与测试 pty 无关, 重接会吞掉自动化输入), 仅在以下情况重接:
     * 重接后 0/1/2 指回 /dev/console(guest ttyS0), 之后所有输出
     * (banner/探测/提示符)与交互认证落到串口。 */
    if (!isatty(STDIN_FILENO) || getenv("PARLZ_FORCE_CONSOLE") != NULL) {
        int cfd = open("/dev/console", O_RDWR | O_NOCTTY);
        if (cfd >= 0) {
            dup2(cfd, STDIN_FILENO);
            dup2(cfd, STDOUT_FILENO);
            dup2(cfd, STDERR_FILENO);
            if (cfd > 2)
                close(cfd);
            if (!isatty(STDIN_FILENO))
                printf("login: AUTH_DONE\n");
            fflush(stdout);
        }
    }

    /* cmdline 参数 login.skip:跳过认证(自动化脚本/调试用)。 */
    {
        FILE *f = fopen("/proc/cmdline", "r");
        char cbuf[1024] = "";
        if (f && fgets(cbuf, sizeof cbuf, f))
            cbuf[strcspn(cbuf, "\n")] = 0;
        if (f)
            fclose(f);
        if (strstr(cbuf, "login.skip")) {
            printf("login: cmdline 带 login.skip,跳过认证\n");
            fflush(stdout);
            return 0;
        }
    }

    /* 交互判定: 重接后(或原 tty 场景) isatty(0) 应为 1。
     * 非 tty(自动化 stdin=file/pipe 且 console 不可用)→ 放行不阻塞。 */
    if (!isatty(STDIN_FILENO)) {
        printf("login: 非 tty 环境,跳过认证\n");
        fflush(stdout);
        return 0;
    }

    struct pa_user users[PA_MAX_USERS];
    int was_legacy = 0;
    int n = pa_load(users, PA_MAX_USERS, &was_legacy);

    char name[PA_NAME_MAX], pass[PA_SECRET_MAX];

    if (n <= 0) {
        /* 首次进入:创建第一个账户 */
        printf("=== Parlz 首次进入,设置用户名与密码 ===\n");
        printf("Username: ");
        fflush(stdout);
        if (pa_read_line(name, sizeof name) < 0 || !*name) {
            fprintf(stderr, "login: 用户名不能为空\n");
            return 1;
        }
        if (!pa_name_ok(name)) {
            fprintf(stderr, "login: 用户名不合法(不能含 ':'、空白, "
                            "不能以 '#' 开头, 长度 < %d)\n", PA_NAME_MAX);
            return 1;
        }
        printf("Password: ");
        fflush(stdout);
        int prc = pa_read_pass(pass, sizeof pass);
        printf("\n");
        if (prc != 0 || !*pass) {
            fprintf(stderr, "login: 密码不能为空\n");
            return 1;
        }
        printf("Confirm password: ");
        fflush(stdout);
        /* 确认必须读进**密码宽度**的缓冲。早先复用过 32 字节的用户名缓冲:
         * 读取静默截断到 31 位 -> 长密码两次永不相等 -> 根本设不上。 */
        char pass3[PA_SECRET_MAX];
        prc = pa_read_pass(pass3, sizeof pass3);
        printf("\n");
        if (prc < 0) {
            fprintf(stderr, "login: 读取密码失败\n");
            return 1;
        }
        if (strcmp(pass3, pass) != 0) {
            fprintf(stderr, "login: 两次密码不一致,请重新设置\n");
            return 1;
        }
        char digest[PA_DIGEST_LEN];
        if (pa_hash(pass, digest, sizeof digest) < 0) {
            fprintf(stderr, "login: 口令散列失败,未写入凭证\n");
            return 1;
        }
        snprintf(users[0].name, PA_NAME_MAX, "%s", name);
        snprintf(users[0].secret, PA_SECRET_MAX, "%s", digest);
        if (pa_save(users, 1) < 0)
            return 1;
        pa_write_whoami(name);
        setenv("USER", name, 1);
        setenv("HOSTNAME", "parlz", 1);
        printf("已创建用户 %s,登录设置完成\n", name);
        fflush(stdout);
        return 0;
    }

    /* 已有账户:登录校验(3 次机会)。
     * 每轮打 "Parlz login: " + Username/Password 提示;失败打重试提示。 */
    for (int i = 0; i < 3; i++) {
        if (i > 0)
            printf("登录失败,请重试(%d/3)\n", i + 1);
        printf("Parlz login: Username: ");
        fflush(stdout);
        if (pa_read_line(name, sizeof name) < 0) {
            printf("\n");
            return 1;
        }
        printf("\nPassword: ");
        fflush(stdout);
        int prc = pa_read_pass(pass, sizeof pass);
        printf("\n");
        if (prc != 0 || !*pass) {
            if (i == 2) {
                fprintf(stderr, "login: 用户名或密码错误,拒绝登录\n");
                return 1;
            }
            continue;
        }
        int idx = pa_find(users, n, name);
        if (idx >= 0 && pa_verify(pass, users[idx].secret)) {
            /* 旧明文 / 旧两行格式: 登录成功当场迁到 $6$ 新格式 ——
             * 已经分发出去的盘上是明文, 不改兼容路径它们下次就登不进去。
             * 写失败(只读根)不拦登录。 */
            int dirty = was_legacy;
            if (pa_is_plain(users[idx].secret)) {
                char digest[PA_DIGEST_LEN];
                if (pa_hash(pass, digest, sizeof digest) == 0) {
                    snprintf(users[idx].secret, PA_SECRET_MAX, "%s", digest);
                    dirty = 1;
                }
            }
            if (dirty)
                pa_save(users, n);
            pa_write_whoami(users[idx].name);
            setenv("USER", users[idx].name, 1);
            setenv("HOSTNAME", "parlz", 1);
            printf("欢迎 %s\n", users[idx].name);
            fflush(stdout);
            return 0;
        }
        name[0] = pass[0] = 0;
        if (i == 2) {
            fprintf(stderr, "login: 用户名或密码错误,拒绝登录\n");
            return 1;
        }
    }
    return 1;
}
