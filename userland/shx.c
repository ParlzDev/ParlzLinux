/* shx.c - /bin/sh 判别器。
 *
 * Parlz 有两个 shell:
 *   /bin/parlz-sh  自写最小 shell(Parlz 风格提示符,内建 ls/touch/clear)
 *   /bin/bash      GNU Bash(完整语法;作为 sh 调用时自动 POSIX 模式)
 *
 * 用户要求: 交互输入 sh 进 SH、输入 bash 进 BASH;而 #!/bin/sh 维护脚本
 * (dpkg 的 preinst/postinst、rpm 的 %pre/%post)需要完整 POSIX 语义。
 * 判别规则:
 *   无参数且 stdin 是 tty(交互)      -> exec /bin/parlz-sh
 *   带参数(脚本/-c)或 stdin 非 tty  -> exec /bin/bash(POSIX 语义)
 * init 的 execl("/bin/sh")(交互)由此进入 Parlz shell;脚本走 bash。
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    /* PATH 兜底:execl 链上可能无环境 */
    setenv("PATH", "/bin:/usr/bin:/sbin:/usr/sbin", 0);

    if (argc == 1 && isatty(STDIN_FILENO)) {
        argv[0] = (char *)"parlz-sh";
        execv("/bin/parlz-sh", argv);
        fprintf(stderr, "sh: exec /bin/parlz-sh: %s\n", strerror(errno));
        return 127;
    }
    execv("/bin/bash", argv);
    fprintf(stderr, "sh: exec /bin/bash: %s\n", strerror(errno));
    return 127;
}
