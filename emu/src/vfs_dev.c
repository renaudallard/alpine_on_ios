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

/* Major/minor device numbers for Linux-compatible stat results. */
#define DEV_NULL_MAJ	1
#define DEV_NULL_MIN	3
#define DEV_ZERO_MAJ	1
#define DEV_ZERO_MIN	5
#define DEV_RANDOM_MAJ	1
#define DEV_RANDOM_MIN	8
#define DEV_URANDOM_MAJ	1
#define DEV_URANDOM_MIN	9
#define DEV_TTY_MAJ	5
#define DEV_TTY_MIN	0
#define DEV_CONSOLE_MAJ	5
#define DEV_CONSOLE_MIN	1
#define DEV_PTMX_MAJ	5
#define DEV_PTMX_MIN	2
#define DEV_PTS0_MAJ	136
#define DEV_PTS0_MIN	0

#define MAKEDEV(maj, min)	(((uint64_t)(maj) << 8) | (uint64_t)(min))

/* Known device entries for readdir. */
static const char *dev_entries[] = {
	"null", "zero", "random", "urandom", "tty",
	"console", "ptmx", "pts", "fd", "stdin",
	"stdout", "stderr", "shm",
	NULL
};

static int
vfs_dev_open(void *ctx, const char *path, int flags, int mode)
{
	int	fd;

	(void)ctx;
	(void)mode;

	if (strcmp(path, "/null") == 0) {
		fd = open("/dev/null", flags);
		if (fd < 0)
			return (-errno);
		return (fd);
	}
	if (strcmp(path, "/zero") == 0) {
		fd = open("/dev/zero", flags);
		if (fd < 0)
			return (-errno);
		return (fd);
	}
	if (strcmp(path, "/urandom") == 0 || strcmp(path, "/random") == 0) {
		fd = open("/dev/urandom", flags);
		if (fd < 0)
			return (-errno);
		return (fd);
	}
	if (strcmp(path, "/tty") == 0 || strcmp(path, "/console") == 0) {
		/* Try host /dev/tty, fall back to /dev/null. */
		fd = open("/dev/tty", flags);
		if (fd < 0)
			fd = open("/dev/null", flags);
		if (fd < 0)
			return (-errno);
		return (fd);
	}
	if (strcmp(path, "/ptmx") == 0 || strcmp(path, "/pts/0") == 0) {
		fd = open("/dev/tty", flags);
		if (fd < 0)
			fd = open("/dev/null", flags);
		if (fd < 0)
			return (-errno);
		return (fd);
	}

	/* /dev/stdin, /dev/stdout, /dev/stderr: dup the fd. */
	if (strcmp(path, "/stdin") == 0)
		return (dup(STDIN_FILENO));
	if (strcmp(path, "/stdout") == 0)
		return (dup(STDOUT_FILENO));
	if (strcmp(path, "/stderr") == 0)
		return (dup(STDERR_FILENO));

	/* /dev/fd/N: dup the specified fd. */
	if (strncmp(path, "/fd/", 4) == 0) {
		int n;
		char *end;

		n = (int)strtol(path + 4, &end, 10);
		if (*end != '\0' || n < 0)
			return (-ENOENT);
		fd = dup(n);
		if (fd < 0)
			return (-errno);
		return (fd);
	}

	LOG_DBG("vfs_dev: unknown device %s", path);
	return (-ENOENT);
}

static int
vfs_dev_stat(void *ctx, const char *path, struct emu_stat *st)
{
	(void)ctx;

	memset(st, 0, sizeof(*st));
	st->st_ino = 1;
	st->st_nlink = 1;
	st->st_blksize = 4096;

	/* Root directory. */
	if (strcmp(path, "/") == 0) {
		st->st_mode = 0755 | 0040000;	/* S_IFDIR */
		st->st_nlink = 2;
		return (0);
	}

	/* Subdirectories. */
	if (strcmp(path, "/pts") == 0 ||
	    strcmp(path, "/fd") == 0 ||
	    strcmp(path, "/shm") == 0) {
		st->st_mode = 0755 | 0040000;	/* S_IFDIR */
		st->st_nlink = 2;
		return (0);
	}

	/* Symlinks. */
	if (strcmp(path, "/stdin") == 0 ||
	    strcmp(path, "/stdout") == 0 ||
	    strcmp(path, "/stderr") == 0 ||
	    strncmp(path, "/fd/", 4) == 0) {
		st->st_mode = 0777 | 0120000;	/* S_IFLNK */
		return (0);
	}

	/* Character devices. */
	if (strcmp(path, "/null") == 0) {
		st->st_mode = 0666 | 0020000;	/* S_IFCHR */
		st->st_rdev = MAKEDEV(DEV_NULL_MAJ, DEV_NULL_MIN);
		return (0);
	}
	if (strcmp(path, "/zero") == 0) {
		st->st_mode = 0666 | 0020000;
		st->st_rdev = MAKEDEV(DEV_ZERO_MAJ, DEV_ZERO_MIN);
		return (0);
	}
	if (strcmp(path, "/random") == 0) {
		st->st_mode = 0666 | 0020000;
		st->st_rdev = MAKEDEV(DEV_RANDOM_MAJ, DEV_RANDOM_MIN);
		return (0);
	}
	if (strcmp(path, "/urandom") == 0) {
		st->st_mode = 0666 | 0020000;
		st->st_rdev = MAKEDEV(DEV_URANDOM_MAJ, DEV_URANDOM_MIN);
		return (0);
	}
	if (strcmp(path, "/tty") == 0) {
		st->st_mode = 0666 | 0020000;
		st->st_rdev = MAKEDEV(DEV_TTY_MAJ, DEV_TTY_MIN);
		return (0);
	}
	if (strcmp(path, "/console") == 0) {
		st->st_mode = 0600 | 0020000;
		st->st_rdev = MAKEDEV(DEV_CONSOLE_MAJ, DEV_CONSOLE_MIN);
		return (0);
	}
	if (strcmp(path, "/ptmx") == 0) {
		st->st_mode = 0666 | 0020000;
		st->st_rdev = MAKEDEV(DEV_PTMX_MAJ, DEV_PTMX_MIN);
		return (0);
	}
	if (strcmp(path, "/pts/0") == 0) {
		st->st_mode = 0620 | 0020000;
		st->st_rdev = MAKEDEV(DEV_PTS0_MAJ, DEV_PTS0_MIN);
		return (0);
	}

	return (-ENOENT);
}

static int
vfs_dev_readdir(void *ctx, const char *path, void *buf, size_t bufsiz,
    off_t *offset)
{
	struct emu_dirent64	 ent;
	uint8_t			*out;
	size_t			 written, reclen, namelen;
	off_t			 idx;
	int			 i;

	(void)ctx;

	if (strcmp(path, "/") != 0)
		return (-ENOENT);

	out = (uint8_t *)buf;
	written = 0;
	idx = 0;

	for (i = 0; dev_entries[i] != NULL; i++) {
		if (idx < *offset) {
			idx++;
			continue;
		}

		namelen = strlen(dev_entries[i]);
		reclen = offsetof(struct emu_dirent64, d_name) + namelen + 1;
		reclen = (reclen + 7) & ~(size_t)7;

		if (written + reclen > bufsiz)
			break;

		memset(&ent, 0, sizeof(ent));
		ent.d_ino = (uint64_t)(i + 2);
		ent.d_off = idx + 1;
		ent.d_reclen = (uint16_t)reclen;
		ent.d_type = 2;	/* DT_CHR */
		snprintf(ent.d_name, sizeof(ent.d_name), "%s", dev_entries[i]);

		memcpy(out + written, &ent, reclen);
		written += reclen;
		idx++;
	}

	*offset = idx;
	return ((int)written);
}

static ssize_t
vfs_dev_readlink(void *ctx, const char *path, char *buf, size_t bufsiz)
{
	(void)ctx;

	if (strcmp(path, "/stdin") == 0) {
		snprintf(buf, bufsiz, "/proc/self/fd/0");
		return ((ssize_t)strlen(buf));
	}
	if (strcmp(path, "/stdout") == 0) {
		snprintf(buf, bufsiz, "/proc/self/fd/1");
		return ((ssize_t)strlen(buf));
	}
	if (strcmp(path, "/stderr") == 0) {
		snprintf(buf, bufsiz, "/proc/self/fd/2");
		return ((ssize_t)strlen(buf));
	}

	/* /dev/fd/N -> /proc/self/fd/N */
	if (strncmp(path, "/fd/", 4) == 0) {
		snprintf(buf, bufsiz, "/proc/self/fd/%s", path + 4);
		return ((ssize_t)strlen(buf));
	}

	return (-EINVAL);
}

static int
vfs_dev_mkdir(void *ctx, const char *path, int mode)
{
	(void)ctx;
	(void)path;
	(void)mode;
	return (-EACCES);
}

static int
vfs_dev_unlink(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	return (-EACCES);
}

static int
vfs_dev_rmdir(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	return (-EACCES);
}

static int
vfs_dev_rename(void *ctx, const char *from, const char *to)
{
	(void)ctx;
	(void)from;
	(void)to;
	return (-EACCES);
}

static int
vfs_dev_chmod(void *ctx, const char *path, int mode)
{
	(void)ctx;
	(void)path;
	(void)mode;
	return (-EACCES);
}

static int
vfs_dev_access(void *ctx, const char *path, int mode)
{
	struct emu_stat	st;

	(void)mode;

	return (vfs_dev_stat(ctx, path, &st));
}

static int
vfs_dev_symlink(void *ctx, const char *target, const char *path)
{
	(void)ctx;
	(void)target;
	(void)path;
	return (-EACCES);
}

static int
vfs_dev_link(void *ctx, const char *oldpath, const char *newpath)
{
	(void)ctx;
	(void)oldpath;
	(void)newpath;
	return (-EACCES);
}

static int
vfs_dev_truncate(void *ctx, const char *path, off_t length)
{
	(void)ctx;
	(void)path;
	(void)length;
	return (-EACCES);
}

static int
vfs_dev_utimens(void *ctx, const char *path, const void *times)
{
	(void)ctx;
	(void)path;
	(void)times;
	return (-EACCES);
}

vfs_ops_t vfs_dev_ops = {
	.open		= vfs_dev_open,
	.stat		= vfs_dev_stat,
	.readdir	= vfs_dev_readdir,
	.readlink	= vfs_dev_readlink,
	.mkdir		= vfs_dev_mkdir,
	.unlink		= vfs_dev_unlink,
	.rmdir		= vfs_dev_rmdir,
	.rename		= vfs_dev_rename,
	.chmod		= vfs_dev_chmod,
	.access		= vfs_dev_access,
	.symlink	= vfs_dev_symlink,
	.link		= vfs_dev_link,
	.truncate	= vfs_dev_truncate,
	.utimens	= vfs_dev_utimens,
};
