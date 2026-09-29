/* user.c - Parlz 账户管理(增/删/改口令)。
 *
 *   user add [用户名] [口令]     新增账户(add 不带给参数就交互式问)
 *   user rm  [用户名]           删除账户
 *   user upd [用户名] [口令]     改口令(不带给参数就交互式问)
 *
 * 账户表读写与口令散列都走 parlzauth.c(与 login 同一份实现):
 * /etc/parlz-auth 每行一个 "用户名:$6$…",不存明文。
 * 命令行给口令会进 ps / shell 历史,敏感场合直接用交互式。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parlzauth.h"

static void usage(const char *why)
{
    if (why && *why)
        fprintf(stderr, "user: %s\n", why);
    fprintf(stderr,
        "用法:\n"
        "  user add [用户名] [口令]   新增账户(缺参数则交互式询问)\n"
        "  user rm  [用户名]          删除账户\n"
        "  user upd [用户名] [口令]   修改口令(缺参数则交互式询问)\n"
        "  user                       显示本帮助与现有账户\n");
}

/* 交互式要口令:两次输入一致才返回 0 */
static int ask_new_pass(char *pass, size_t n)
{
    char again[PA_SECRET_MAX];
    printf("Password: ");
    fflush(stdout);
    int rc = pa_read_pass(pass, n);
    printf("\n");
    if (rc < 0) {
        fprintf(stderr, "user: 读取口令失败\n");
        return -1;
    }
    if (rc == 1 || !*pass) {
        fprintf(stderr, "user: 口令不能为空\n");
        return -1;
    }
    printf("Confirm password: ");
    fflush(stdout);
    rc = pa_read_pass(again, sizeof again);
    printf("\n");
    if (rc < 0) {
        fprintf(stderr, "user: 读取口令失败\n");
        return -1;
    }
    if (strcmp(pass, again) != 0) {
        fprintf(stderr, "user: 两次口令不一致\n");
        return -1;
    }
    return 0;
}

static int ask_name(char *name, size_t n, const char *prompt)
{
    printf("%s", prompt);
    fflush(stdout);
    if (pa_read_line(name, n) < 0 || !*name) {
        fprintf(stderr, "user: 用户名不能为空\n");
        return -1;
    }
    if (!pa_name_ok(name)) {
        fprintf(stderr, "user: 用户名不合法(不能含 ':'、空白, "
                        "不能以 '#' 开头, 长度 < %d)\n", PA_NAME_MAX);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        /* 裸 user: 打帮助,附现有账户(只列名字) */
        struct pa_user users[PA_MAX_USERS];
        int n = pa_load(users, PA_MAX_USERS, NULL);
        if (n > 0) {
            printf("现有账户(%d):", n);
            for (int i = 0; i < n; i++)
                printf(" %s", users[i].name);
            printf("\n\n");
        } else {
            printf("还没有账户(下次登录会要求创建第一个)\n\n");
        }
        usage(NULL);
        return n > 0 ? 0 : 1;
    }

    const char *sub = argv[1];
    int interactive = !isatty(STDIN_FILENO) ? 0 : 1;

    struct pa_user users[PA_MAX_USERS];
    int n = pa_load(users, PA_MAX_USERS, NULL);
    if (n < 0)
        n = 0;

    char name[PA_NAME_MAX], pass[PA_SECRET_MAX];

    if (!strcmp(sub, "add")) {
        if (argc >= 3)
            snprintf(name, sizeof name, "%s", argv[2]);
        else if (interactive) {
            if (ask_name(name, sizeof name, "Username: ") < 0)
                return 1;
        } else {
            usage("add 需要用户名(或交互式运行)");
            return 1;
        }
        if (!pa_name_ok(name)) {
            fprintf(stderr, "user: 用户名不合法\n");
            return 1;
        }
        if (pa_find(users, n, name) >= 0) {
            fprintf(stderr, "user: 账户 %s 已存在\n", name);
            return 1;
        }
        if (n >= PA_MAX_USERS) {
            fprintf(stderr, "user: 账户数已达上限 %d\n", PA_MAX_USERS);
            return 1;
        }
        if (argc >= 4)
            snprintf(pass, sizeof pass, "%s", argv[3]);
        else if (interactive) {
            if (ask_new_pass(pass, sizeof pass) < 0)
                return 1;
        } else {
            usage("add 需要口令(或交互式运行)");
            return 1;
        }
        if (!*pass) {
            fprintf(stderr, "user: 口令不能为空\n");
            return 1;
        }
        char digest[PA_DIGEST_LEN];
        if (pa_hash(pass, digest, sizeof digest) < 0) {
            fprintf(stderr, "user: 口令散列失败\n");
            return 1;
        }
        snprintf(users[n].name, PA_NAME_MAX, "%s", name);
        snprintf(users[n].secret, PA_SECRET_MAX, "%s", digest);
        if (pa_save(users, n + 1) < 0)
            return 1;
        printf("user: 已新增账户 %s\n", name);
        return 0;
    }

    if (!strcmp(sub, "rm")) {
        if (argc >= 3)
            snprintf(name, sizeof name, "%s", argv[2]);
        else if (interactive) {
            if (ask_name(name, sizeof name, "Username: ") < 0)
                return 1;
        } else {
            usage("rm 需要用户名(或交互式运行)");
            return 1;
        }
        int idx = pa_find(users, n, name);
        if (idx < 0) {
            fprintf(stderr, "user: 没有账户 %s\n", name);
            return 1;
        }
        for (int i = idx; i < n - 1; i++)
            users[i] = users[i + 1];
        if (pa_save(users, n - 1) < 0)
            return 1;
        printf("user: 已删除账户 %s\n", name);
        if (n - 1 == 0)
            printf("user: 账户表已空 —— 下次登录会要求重新创建第一个账户\n");
        return 0;
    }

    if (!strcmp(sub, "upd")) {
        if (argc >= 3)
            snprintf(name, sizeof name, "%s", argv[2]);
        else if (interactive) {
            if (ask_name(name, sizeof name, "Username: ") < 0)
                return 1;
        } else {
            usage("upd 需要用户名(或交互式运行)");
            return 1;
        }
        int idx = pa_find(users, n, name);
        if (idx < 0) {
            fprintf(stderr, "user: 没有账户 %s\n", name);
            return 1;
        }
        if (argc >= 4)
            snprintf(pass, sizeof pass, "%s", argv[3]);
        else if (interactive) {
            printf("新口令 for %s\n", name);
            if (ask_new_pass(pass, sizeof pass) < 0)
                return 1;
        } else {
            usage("upd 需要新口令(或交互式运行)");
            return 1;
        }
        if (!*pass) {
            fprintf(stderr, "user: 口令不能为空\n");
            return 1;
        }
        char digest[PA_DIGEST_LEN];
        if (pa_hash(pass, digest, sizeof digest) < 0) {
            fprintf(stderr, "user: 口令散列失败\n");
            return 1;
        }
        snprintf(users[idx].secret, PA_SECRET_MAX, "%s", digest);
        if (pa_save(users, n) < 0)
            return 1;
        printf("user: 已更新 %s 的口令\n", name);
        return 0;
    }

    usage("未知子命令");
    return 1;
}
