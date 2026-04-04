#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "emu.h"

int main(int argc, char **argv) {
	const char *rootfs = (argc > 1) ? argv[1] : "rootfs/alpine";
	/* Two fork commands in -c mode */
	const char *cmd_argv[] = { "/bin/sh", "-c",
	    "ls /; echo MIDDLE; ls /; echo DONE", NULL };
	const char *envp[] = {
		"HOME=/root",
		"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
		"TERM=dumb",
		NULL
	};
	int term_fd, pid, rc;
	char buf[65536] = {0};
	size_t len = 0;
	struct pollfd pfd;

	rc = emu_init(rootfs);
	if (rc != 0) return 1;
	emu_set_jit_enabled(0);

	pid = emu_spawn("/bin/sh", cmd_argv, envp, &term_fd);
	if (pid < 0) { fprintf(stderr, "spawn failed: %s\n", emu_last_error()); return 1; }

	pfd.fd = term_fd;
	pfd.events = POLLIN;

	for (int i = 0; i < 300; i++) {
		rc = poll(&pfd, 1, 100);
		if (rc > 0 && (pfd.revents & POLLIN)) {
			ssize_t n = read(term_fd, buf + len, sizeof(buf) - len - 1);
			if (n > 0) { len += (size_t)n; buf[len] = '\0'; }
		}
		if (pfd.revents & (POLLHUP | POLLERR)) break;
		if (strstr(buf, "DONE")) break;
	}

	fprintf(stderr, "=== output:\n%s\n", buf);
	fprintf(stderr, "=== MIDDLE=%d DONE=%d\n",
	    strstr(buf, "MIDDLE") ? 1 : 0, strstr(buf, "DONE") ? 1 : 0);

	close(term_fd);
	emu_shutdown();
	return strstr(buf, "DONE") ? 0 : 1;
}
