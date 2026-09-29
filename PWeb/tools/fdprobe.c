#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == '0') {
        int out = open("/tmp/fdprobe.out", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (out >= 0) {
            dup2(out, 1);
            dup2(out, 2);
            close(out);
        }
        int pfd[2];
        if (pipe(pfd) != 0) { perror("pipe"); return 1; }
        pid_t pid = fork();
        if (pid == 0) {
            int r = dup2(pfd[1], 1);
            int e1 = errno;
            fprintf(stderr, "child: dup2(pfd[1]=fd%d -> 1) ret=%d errno=%d\n",
                    pfd[1], r, e1);
            int n1 = open("/dev/null", O_WRONLY);
            fprintf(stderr, "child: open(/dev/null,w) = %d\n", n1);
            if (n1 >= 0) {
                int r2 = dup2(n1, 1);
                fprintf(stderr, "child: dup2(n1=%d -> 1) ret=%d errno=%d\n", n1, r2, errno);
                close(n1);
            }
            int r3 = dup2(pfd[1], 1);
            fprintf(stderr, "child: dup2(pfd[1]=fd%d -> 1) ret=%d errno=%d\n",
                    pfd[1], r3, errno);
            if (r3 == 0) {
                const char *msg = "FDPROBE-CHILD-OK\n";
                (void)write(1, msg, 15);
            }
            close(pfd[0]);
            close(pfd[1]);
            _exit(0);
        }
        close(pfd[1]);
        char buf[128]; ssize_t n;
        while ((n = read(pfd[0], buf, sizeof buf)) > 0) {
            fprintf(stderr, "parent saw: %.*s", (int)n, buf);
        }
        close(pfd[0]);
        int st; waitpid(pid, &st, 0);
        fprintf(stderr, "parent: child exit=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 0;
    }
    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd >= 0) { dup2(nullfd, 0); close(nullfd); }
    if (setsid() == 0) {
        execl(argv[0], argv[0], "0", (char*)NULL);
    }
    perror("exec");
    return 1;
}
