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
#include <sys/time.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vfs.h"
#include "log.h"

static int
make_host_path(const char *root, const char *path, char *out, size_t outsiz)
{
	int	ret;

	if (path[0] == '/')
		ret = snprintf(out, outsiz, "%s%s", root, path);
	else
		ret = snprintf(out, outsiz, "%s/%s", root, path);

	if (ret < 0 || (size_t)ret >= outsiz)
		return (-1);
	return (0);
}

static void
stat_to_emu(const struct stat *hst, struct emu_stat *est)
{
	memset(est, 0, sizeof(*est));
	est->st_dev = hst->st_dev;
	est->st_ino = hst->st_ino;
	est->st_mode = hst->st_mode;
	est->st_nlink = hst->st_nlink;
	est->st_uid = hst->st_uid;
	est->st_gid = hst->st_gid;
	est->st_rdev = hst->st_rdev;
	est->st_size = hst->st_size;
	est->st_blksize = hst->st_blksize;
	est->st_blocks = hst->st_blocks;
#ifdef __APPLE__
	est->st_atime_sec = hst->st_atimespec.tv_sec;
	est->st_atime_nsec = hst->st_atimespec.tv_nsec;
	est->st_mtime_sec = hst->st_mtimespec.tv_sec;
	est->st_mtime_nsec = hst->st_mtimespec.tv_nsec;
	est->st_ctime_sec = hst->st_ctimespec.tv_sec;
	est->st_ctime_nsec = hst->st_ctimespec.tv_nsec;
#else
	est->st_atime_sec = hst->st_atim.tv_sec;
	est->st_atime_nsec = hst->st_atim.tv_nsec;
	est->st_mtime_sec = hst->st_mtim.tv_sec;
	est->st_mtime_nsec = hst->st_mtim.tv_nsec;
	est->st_ctime_sec = hst->st_ctim.tv_sec;
	est->st_ctime_nsec = hst->st_ctim.tv_nsec;
#endif
}

static int
vfs_real_open(void *ctx, const char *path, int flags, int mode)
{
	char	host[PATH_MAX];
	int	fd;

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	fd = open(host, flags, mode);
	if (fd < 0)
		return (-errno);
	return (fd);
}

static int
vfs_real_stat(void *ctx, const char *path, struct emu_stat *st)
{
	char		host[PATH_MAX];
	struct stat	hst;

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (lstat(host, &hst) != 0)
		return (-errno);

	stat_to_emu(&hst, st);
	return (0);
}

static int
vfs_real_readdir(void *ctx, const char *path, void *buf, size_t bufsiz,
    off_t *offset)
{
	char			host[PATH_MAX];
	DIR			*dp;
	struct dirent		*de;
	struct emu_dirent64	ent;
	uint8_t			*out;
	size_t			written, reclen, namelen;
	off_t			idx;

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	dp = opendir(host);
	if (dp == NULL)
		return (-errno);

	out = (uint8_t *)buf;
	written = 0;
	idx = 0;

	while ((de = readdir(dp)) != NULL) {
		if (idx < *offset) {
			idx++;
			continue;
		}

		namelen = strlen(de->d_name);
		if (namelen >= sizeof(ent.d_name))
			namelen = sizeof(ent.d_name) - 1;

		/* Compute record length: fixed fields + name + NUL,
		 * aligned to 8 bytes. */
		reclen = offsetof(struct emu_dirent64, d_name) + namelen + 1;
		reclen = (reclen + 7) & ~(size_t)7;

		if (written + reclen > bufsiz)
			break;

		memset(&ent, 0, sizeof(ent));
		ent.d_ino = de->d_ino;
		ent.d_off = idx + 1;
		ent.d_reclen = (uint16_t)reclen;
		ent.d_type = de->d_type;
		memcpy(ent.d_name, de->d_name, namelen);
		ent.d_name[namelen] = '\0';

		memcpy(out + written, &ent, reclen);
		written += reclen;
		idx++;
	}

	*offset = idx;
	closedir(dp);
	return ((int)written);
}

static ssize_t
vfs_real_readlink(void *ctx, const char *path, char *buf, size_t bufsiz)
{
	char		host[PATH_MAX];
	ssize_t		ret;

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	ret = readlink(host, buf, bufsiz);
	if (ret < 0)
		return (-errno);
	return (ret);
}

static int
vfs_real_mkdir(void *ctx, const char *path, int mode)
{
	char	host[PATH_MAX];

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (mkdir(host, mode) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_unlink(void *ctx, const char *path)
{
	char	host[PATH_MAX];

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (unlink(host) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_rmdir(void *ctx, const char *path)
{
	char	host[PATH_MAX];

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (rmdir(host) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_rename(void *ctx, const char *from, const char *to)
{
	char	hfrom[PATH_MAX], hto[PATH_MAX];

	if (make_host_path((const char *)ctx, from, hfrom, sizeof(hfrom)) != 0)
		return (-ENAMETOOLONG);
	if (make_host_path((const char *)ctx, to, hto, sizeof(hto)) != 0)
		return (-ENAMETOOLONG);

	if (rename(hfrom, hto) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_chmod(void *ctx, const char *path, int mode)
{
	char	host[PATH_MAX];

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (chmod(host, mode) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_access(void *ctx, const char *path, int mode)
{
	char	host[PATH_MAX];

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (access(host, mode) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_symlink(void *ctx, const char *target, const char *path)
{
	char	host[PATH_MAX];

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (symlink(target, host) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_link(void *ctx, const char *oldpath, const char *newpath)
{
	char	hold[PATH_MAX], hnew[PATH_MAX];

	if (make_host_path((const char *)ctx, oldpath, hold, sizeof(hold)) != 0)
		return (-ENAMETOOLONG);
	if (make_host_path((const char *)ctx, newpath, hnew, sizeof(hnew)) != 0)
		return (-ENAMETOOLONG);

	if (link(hold, hnew) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_truncate(void *ctx, const char *path, off_t length)
{
	char	host[PATH_MAX];

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (truncate(host, length) != 0)
		return (-errno);
	return (0);
}

static int
vfs_real_utimens(void *ctx, const char *path, const void *times)
{
	char	host[PATH_MAX];

	if (make_host_path((const char *)ctx, path, host, sizeof(host)) != 0)
		return (-ENAMETOOLONG);

	if (utimensat(AT_FDCWD, host, (const struct timespec *)times, 0) != 0)
		return (-errno);
	return (0);
}

vfs_ops_t vfs_real_ops = {
	.open		= vfs_real_open,
	.stat		= vfs_real_stat,
	.readdir	= vfs_real_readdir,
	.readlink	= vfs_real_readlink,
	.mkdir		= vfs_real_mkdir,
	.unlink		= vfs_real_unlink,
	.rmdir		= vfs_real_rmdir,
	.rename		= vfs_real_rename,
	.chmod		= vfs_real_chmod,
	.access		= vfs_real_access,
	.symlink	= vfs_real_symlink,
	.link		= vfs_real_link,
	.truncate	= vfs_real_truncate,
	.utimens	= vfs_real_utimens,
};
