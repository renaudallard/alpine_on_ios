/*
 * Copyright (c) 2026 Alpine on iOS contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vfs.h"
#include "log.h"

/*
 * Generate content for a virtual /proc file.
 * Returns a malloc'd buffer with the content, or NULL.
 * Sets *lenp to the content length.
 */
static char *
proc_gen_content(const char *path, size_t *lenp)
{
	char	*buf;
	int	 len;

	buf = malloc(4096);
	if (buf == NULL)
		return (NULL);

	if (strcmp(path, "/cpuinfo") == 0) {
		len = snprintf(buf, 4096,
		    "processor\t: 0\n"
		    "BogoMIPS\t: 48.00\n"
		    "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
		    "CPU implementer\t: 0x41\n"
		    "CPU architecture: 8\n"
		    "CPU variant\t: 0x1\n"
		    "CPU part\t: 0xd07\n"
		    "CPU revision\t: 4\n\n");
	} else if (strcmp(path, "/meminfo") == 0) {
		len = snprintf(buf, 4096,
		    "MemTotal:        4096000 kB\n"
		    "MemFree:         2048000 kB\n"
		    "MemAvailable:    3072000 kB\n"
		    "Buffers:          128000 kB\n"
		    "Cached:           512000 kB\n"
		    "SwapTotal:             0 kB\n"
		    "SwapFree:              0 kB\n");
	} else if (strcmp(path, "/stat") == 0) {
		len = snprintf(buf, 4096,
		    "cpu  100 0 100 1000 0 0 0 0 0 0\n"
		    "cpu0 100 0 100 1000 0 0 0 0 0 0\n"
		    "intr 0\n"
		    "ctxt 0\n"
		    "btime 1700000000\n"
		    "processes 1\n"
		    "procs_running 1\n"
		    "procs_blocked 0\n");
	} else if (strcmp(path, "/version") == 0) {
		len = snprintf(buf, 4096,
		    "Linux version 6.1.0 (builder@alpine) "
		    "(aarch64-linux-musl-gcc) #1 SMP\n");
	} else if (strcmp(path, "/uptime") == 0) {
		len = snprintf(buf, 4096, "100.00 100.00\n");
	} else if (strcmp(path, "/loadavg") == 0) {
		len = snprintf(buf, 4096, "0.00 0.00 0.00 1/1 1\n");
	} else if (strcmp(path, "/filesystems") == 0) {
		len = snprintf(buf, 4096,
		    "nodev\ttmpfs\n"
		    "\text4\n");
	} else if (strcmp(path, "/mounts") == 0) {
		len = snprintf(buf, 4096,
		    "rootfs / ext4 rw 0 0\n"
		    "proc /proc proc rw 0 0\n"
		    "devtmpfs /dev devtmpfs rw 0 0\n"
		    "tmpfs /tmp tmpfs rw 0 0\n");
	} else if (strcmp(path, "/sys/kernel/hostname") == 0) {
		len = snprintf(buf, 4096, "alpine\n");
	} else if (strcmp(path, "/sys/kernel/osrelease") == 0) {
		len = snprintf(buf, 4096, "6.1.0\n");
	} else if (strcmp(path, "/self/status") == 0) {
		len = snprintf(buf, 4096,
		    "Name:\temu\n"
		    "State:\tR (running)\n"
		    "Pid:\t1\n"
		    "PPid:\t0\n"
		    "Uid:\t0\t0\t0\t0\n"
		    "Gid:\t0\t0\t0\t0\n"
		    "Threads:\t1\n"
		    "VmSize:\t    4096 kB\n"
		    "VmRSS:\t    2048 kB\n");
	} else if (strcmp(path, "/self/stat") == 0) {
		len = snprintf(buf, 4096,
		    "1 (emu) R 0 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 "
		    "0 4096000 500 18446744073709551615 0 0 0 0 0 0 0 0 "
		    "0 0 0 0 17 0 0 0 0 0 0\n");
	} else if (strcmp(path, "/self/maps") == 0) {
		buf[0] = '\0';
		len = 0;
	} else {
		free(buf);
		return (NULL);
	}

	if (len < 0) {
		free(buf);
		return (NULL);
	}

	*lenp = (size_t)len;
	return (buf);
}

static int
vfs_proc_open(void *ctx, const char *path, int flags, int mode)
{
	char	*content;
	size_t	 len;
	int	 pipefd[2];
	ssize_t	 nw;

	(void)ctx;
	(void)flags;
	(void)mode;

	content = proc_gen_content(path, &len);
	if (content == NULL)
		return (-ENOENT);

	if (pipe(pipefd) != 0) {
		free(content);
		return (-errno);
	}

	/* Write content to pipe, then close write end. */
	nw = write(pipefd[1], content, len);
	(void)nw;
	close(pipefd[1]);
	free(content);

	return (pipefd[0]);
}

static int
vfs_proc_stat(void *ctx, const char *path, struct emu_stat *st)
{
	(void)ctx;

	memset(st, 0, sizeof(*st));
	st->st_ino = 1;
	st->st_nlink = 1;
	st->st_blksize = 4096;

	/* Directories. */
	if (strcmp(path, "/") == 0 ||
	    strcmp(path, "/self") == 0 ||
	    strcmp(path, "/self/fd") == 0 ||
	    strcmp(path, "/sys") == 0 ||
	    strcmp(path, "/sys/kernel") == 0) {
		st->st_mode = 0555 | 0040000;	/* S_IFDIR */
		st->st_nlink = 2;
		return (0);
	}

	/* Symlinks. */
	if (strcmp(path, "/self/exe") == 0 ||
	    strncmp(path, "/self/fd/", 9) == 0) {
		st->st_mode = 0777 | 0120000;	/* S_IFLNK */
		return (0);
	}

	/* Check if this is a known virtual file. */
	{
		size_t	len;
		char	*content;

		content = proc_gen_content(path, &len);
		if (content != NULL) {
			st->st_mode = 0444 | 0100000;	/* S_IFREG */
			st->st_size = (int64_t)len;
			free(content);
			return (0);
		}
	}

	return (-ENOENT);
}

/* Known entries in /proc top-level directory. */
static const char *proc_top_entries[] = {
	"self", "cpuinfo", "meminfo", "stat", "version",
	"uptime", "loadavg", "filesystems", "mounts", "sys",
	NULL
};

/* Known entries under /proc/self. */
static const char *proc_self_entries[] = {
	"exe", "maps", "status", "stat", "fd",
	NULL
};

static int
vfs_proc_readdir(void *ctx, const char *path, void *buf, size_t bufsiz,
    off_t *offset)
{
	const char		**entries;
	struct emu_dirent64	 ent;
	uint8_t			*out;
	size_t			 written, reclen, namelen;
	off_t			 idx;
	int			 i;

	(void)ctx;

	if (strcmp(path, "/") == 0)
		entries = proc_top_entries;
	else if (strcmp(path, "/self") == 0)
		entries = proc_self_entries;
	else
		return (-ENOENT);

	out = (uint8_t *)buf;
	written = 0;
	idx = 0;

	for (i = 0; entries[i] != NULL; i++) {
		if (idx < *offset) {
			idx++;
			continue;
		}

		namelen = strlen(entries[i]);
		reclen = offsetof(struct emu_dirent64, d_name) + namelen + 1;
		reclen = (reclen + 7) & ~(size_t)7;

		if (written + reclen > bufsiz)
			break;

		memset(&ent, 0, sizeof(ent));
		ent.d_ino = (uint64_t)(i + 2);
		ent.d_off = idx + 1;
		ent.d_reclen = (uint16_t)reclen;
		ent.d_type = 4;	/* DT_DIR for simplicity */
		snprintf(ent.d_name, sizeof(ent.d_name), "%s", entries[i]);

		memcpy(out + written, &ent, reclen);
		written += reclen;
		idx++;
	}

	*offset = idx;
	return ((int)written);
}

static ssize_t
vfs_proc_readlink(void *ctx, const char *path, char *buf, size_t bufsiz)
{
	(void)ctx;

	if (strcmp(path, "/self/exe") == 0) {
		snprintf(buf, bufsiz, "/bin/busybox");
		return ((ssize_t)strlen(buf));
	}

	/* /self/fd/N */
	if (strncmp(path, "/self/fd/", 9) == 0) {
		/* Return a synthetic path. */
		snprintf(buf, bufsiz, "/dev/fd/%s", path + 9);
		return ((ssize_t)strlen(buf));
	}

	return (-EINVAL);
}

static int
vfs_proc_mkdir(void *ctx, const char *path, int mode)
{
	(void)ctx;
	(void)path;
	(void)mode;
	return (-EACCES);
}

static int
vfs_proc_unlink(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	return (-EACCES);
}

static int
vfs_proc_rmdir(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	return (-EACCES);
}

static int
vfs_proc_rename(void *ctx, const char *from, const char *to)
{
	(void)ctx;
	(void)from;
	(void)to;
	return (-EACCES);
}

static int
vfs_proc_chmod(void *ctx, const char *path, int mode)
{
	(void)ctx;
	(void)path;
	(void)mode;
	return (-EACCES);
}

static int
vfs_proc_access(void *ctx, const char *path, int mode)
{
	struct emu_stat	st;
	int		ret;

	(void)mode;

	ret = vfs_proc_stat(ctx, path, &st);
	return (ret);
}

static int
vfs_proc_symlink(void *ctx, const char *target, const char *path)
{
	(void)ctx;
	(void)target;
	(void)path;
	return (-EACCES);
}

static int
vfs_proc_link(void *ctx, const char *oldpath, const char *newpath)
{
	(void)ctx;
	(void)oldpath;
	(void)newpath;
	return (-EACCES);
}

static int
vfs_proc_truncate(void *ctx, const char *path, off_t length)
{
	(void)ctx;
	(void)path;
	(void)length;
	return (-EACCES);
}

static int
vfs_proc_utimens(void *ctx, const char *path, const void *times)
{
	(void)ctx;
	(void)path;
	(void)times;
	return (-EACCES);
}

vfs_ops_t vfs_proc_ops = {
	.open		= vfs_proc_open,
	.stat		= vfs_proc_stat,
	.readdir	= vfs_proc_readdir,
	.readlink	= vfs_proc_readlink,
	.mkdir		= vfs_proc_mkdir,
	.unlink		= vfs_proc_unlink,
	.rmdir		= vfs_proc_rmdir,
	.rename		= vfs_proc_rename,
	.chmod		= vfs_proc_chmod,
	.access		= vfs_proc_access,
	.symlink	= vfs_proc_symlink,
	.link		= vfs_proc_link,
	.truncate	= vfs_proc_truncate,
	.utimens	= vfs_proc_utimens,
};
