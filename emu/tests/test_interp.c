/*
 * Test interpreter mode by disabling JIT and running /bin/sh -c "echo OK"
 * with detailed instruction tracing around the crash point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include "emu.h"

/* Access internals for debugging */
extern void log_set_level(int level);

int main(int argc, char **argv)
{
    const char *rootfs = (argc > 1) ? argv[1] : "../rootfs/alpine";
    const char *cmd_argv[] = { "/bin/sh", "-c", "echo OK", NULL };
    const char *envp[] = {
        "HOME=/root",
        "PATH=/bin:/usr/bin:/sbin:/usr/sbin",
        "TERM=dumb",
        NULL
    };
    int term_fd, pid, rc;
    char buf[4096];
    struct pollfd pfd;

    rc = emu_init(rootfs);
    if (rc != 0) { fprintf(stderr, "emu_init failed\n"); return 1; }

    /* Force interpreter mode */
    emu_set_jit_enabled(0);
    fprintf(stderr, "Interpreter mode forced\n");

    pid = emu_spawn("/bin/sh", cmd_argv, envp, &term_fd);
    if (pid < 0) {
        fprintf(stderr, "emu_spawn failed: %s\n", emu_last_error());
        return 1;
    }
    fprintf(stderr, "Spawned pid=%d fd=%d\n", pid, term_fd);

    /* Read output */
    pfd.fd = term_fd;
    pfd.events = POLLIN;
    for (int i = 0; i < 50; i++) {
        rc = poll(&pfd, 1, 100);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(term_fd, buf, sizeof(buf)-1);
            if (n > 0) {
                buf[n] = '\0';
                fprintf(stderr, "OUTPUT: %s\n", buf);
                if (strstr(buf, "OK")) {
                    fprintf(stderr, "SUCCESS\n");
                    return 0;
                }
            }
        }
        if (pfd.revents & (POLLHUP|POLLERR)) break;
    }
    fprintf(stderr, "FAILED - no output\n");
    return 1;
}
