#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "emu.h"

static ssize_t
read_until(int fd, char *total, size_t *total_len, size_t maxlen,
    const char *marker, int timeout_ms)
{
	struct pollfd pfd;
	char buf[4096];
	ssize_t n;
	int elapsed = 0;

	pfd.fd = fd;
	pfd.events = POLLIN;

	while (elapsed < timeout_ms) {
		int rc = poll(&pfd, 1, 100);
		elapsed += 100;
		if (rc > 0 && (pfd.revents & POLLIN)) {
			n = read(fd, buf, sizeof(buf) - 1);
			if (n > 0) {
				buf[n] = '\0';
				if (*total_len + (size_t)n < maxlen - 1) {
					memcpy(total + *total_len, buf, (size_t)n);
					*total_len += (size_t)n;
					total[*total_len] = '\0';
				}
				if (marker && strstr(total, marker))
					return 1;
			}
		}
		if (pfd.revents & (POLLHUP | POLLERR))
			return -1;
	}
	return 0;
}

int main(int argc, char **argv) {
	const char *rootfs = (argc > 1) ? argv[1] : "rootfs/alpine";
	const char *cmd_argv[] = { "/bin/sh", NULL };
	const char *envp[] = {
		"HOME=/root",
		"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
		"TERM=dumb",
		NULL
	};
	int term_fd, pid, rc;
	char total[65536] = {0};
	size_t total_len = 0;

	rc = emu_init(rootfs);
	if (rc != 0) { fprintf(stderr, "emu_init failed\n"); return 1; }

	emu_set_jit_enabled(0);

	pid = emu_spawn("/bin/sh", cmd_argv, envp, &term_fd);
	if (pid < 0) { fprintf(stderr, "emu_spawn failed: %s\n", emu_last_error()); return 1; }

	/* Wait briefly for shell to start */
	read_until(term_fd, total, &total_len, sizeof(total), "$ ", 15000);

	/* Send ls */
	write(term_fd, "ls /\n", 5);
	total_len = 0; total[0] = '\0';
	rc = read_until(term_fd, total, &total_len, sizeof(total), "bin", 30000);
	fprintf(stderr, "=== ls output: rc=%d\n", rc);

	/* Send echo SECOND */
	write(term_fd, "echo SECOND\n", 12);
	total_len = 0; total[0] = '\0';
	rc = read_until(term_fd, total, &total_len, sizeof(total), "SECOND", 30000);
	fprintf(stderr, "=== echo output: rc=%d, got=%s\n", rc,
	    strstr(total, "SECOND") ? "yes" : "no");

	close(term_fd);
	emu_shutdown();
	return strstr(total, "SECOND") ? 0 : 1;
}
