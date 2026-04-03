/*
 * Integration test: spawn /bin/sh on Alpine rootfs and check output.
 */

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emu.h"

int
main(int argc, char **argv)
{
	const char	*rootfs;
	const char	*cmd_argv[] = { "/bin/sh", "-c", "echo HELLO", NULL };
	const char	*envp[] = {
	    "HOME=/root",
	    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
	    "TERM=dumb",
	    NULL
	};
	int		 term_fd, pid, rc;
	char		 buf[4096];
	ssize_t		 n;
	struct pollfd	 pfd;

	rootfs = (argc > 1) ? argv[1] : "rootfs/alpine";

	fprintf(stderr, "=== test_run: rootfs=%s\n", rootfs);

	rc = emu_init(rootfs);
	if (rc != 0) {
		fprintf(stderr, "emu_init failed\n");
		return 1;
	}
	fprintf(stderr, "=== emu_init OK\n");

	/* Enable JIT for native execution on aarch64 */
	emu_set_jit_enabled(1);
	fprintf(stderr, "=== JIT enabled=%d\n", emu_jit_enabled());

	pid = emu_spawn("/bin/sh", cmd_argv, envp, &term_fd);
	if (pid < 0) {
		fprintf(stderr, "emu_spawn failed: %d\n", pid);
		return 1;
	}
	fprintf(stderr, "=== emu_spawn OK, pid=%d term_fd=%d\n", pid, term_fd);

	/* Read output with a timeout */
	pfd.fd = term_fd;
	pfd.events = POLLIN;

	for (int i = 0; i < 50; i++) {	/* 5 seconds max */
		rc = poll(&pfd, 1, 100);
		if (rc > 0 && (pfd.revents & POLLIN)) {
			n = read(term_fd, buf, sizeof(buf) - 1);
			if (n > 0) {
				buf[n] = '\0';
				fprintf(stderr, "=== output (%zd bytes): ", n);
				for (ssize_t j = 0; j < n; j++) {
					if (buf[j] >= 0x20 && buf[j] < 0x7f)
						fputc(buf[j], stderr);
					else
						fprintf(stderr, "\\x%02x",
						    (unsigned char)buf[j]);
				}
				fputc('\n', stderr);

				if (strstr(buf, "HELLO") != NULL) {
					fprintf(stderr, "=== SUCCESS\n");
					close(term_fd);
					emu_shutdown();
					return 0;
				}
			}
		}
		if (pfd.revents & (POLLHUP | POLLERR))
			break;
	}

	fprintf(stderr, "=== TIMEOUT or no HELLO found\n");
	close(term_fd);
	emu_shutdown();
	return 1;
}
