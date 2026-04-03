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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "syscall.h"
#include "process.h"
#include "memory.h"
#include "vfs.h"
#include "framebuffer.h"
#include "vfs_input.h"
#include "log.h"

/* Linux errno values (host errno may differ on non-Linux). */
#define LINUX_EPERM		1
#define LINUX_ENOENT		2
#define LINUX_ESRCH		3
#define LINUX_EINTR		4
#define LINUX_EIO		5
#define LINUX_ENXIO		6
#define LINUX_E2BIG		7
#define LINUX_ENOEXEC		8
#define LINUX_EBADF		9
#define LINUX_ECHILD		10
#define LINUX_EAGAIN		11
#define LINUX_ENOMEM		12
#define LINUX_EACCES		13
#define LINUX_EFAULT		14
#define LINUX_ENOTBLK		15
#define LINUX_EBUSY		16
#define LINUX_EEXIST		17
#define LINUX_EXDEV		18
#define LINUX_ENODEV		19
#define LINUX_ENOTDIR		20
#define LINUX_EISDIR		21
#define LINUX_EINVAL		22
#define LINUX_ENFILE		23
#define LINUX_EMFILE		24
#define LINUX_ENOTTY		25
#define LINUX_ETXTBSY		26
#define LINUX_EFBIG		27
#define LINUX_ENOSPC		28
#define LINUX_ESPIPE		29
#define LINUX_EROFS		30
#define LINUX_EMLINK		31
#define LINUX_EPIPE		32
#define LINUX_ERANGE		34
#define LINUX_ENOSYS		38
#define LINUX_ENOTEMPTY		39
#define LINUX_ELOOP		40
#define LINUX_ENOPROTOOPT	92
#define LINUX_ENOTSOCK		88

/* Linux AT_FDCWD */
#define LINUX_AT_FDCWD		(-100)

/* Linux open flags */
#define LINUX_O_RDONLY		0x0000
#define LINUX_O_WRONLY		0x0001
#define LINUX_O_RDWR		0x0002
#define LINUX_O_CREAT		0x0040
#define LINUX_O_EXCL		0x0080
#define LINUX_O_NOCTTY		0x0100
#define LINUX_O_TRUNC		0x0200
#define LINUX_O_APPEND		0x0400
#define LINUX_O_NONBLOCK	0x0800
#define LINUX_O_DIRECTORY	0x10000
#define LINUX_O_CLOEXEC		0x80000

/* Linux fcntl commands */
#define LINUX_F_DUPFD		0
#define LINUX_F_GETFD		1
#define LINUX_F_SETFD		2
#define LINUX_F_GETFL		3
#define LINUX_F_SETFL		4
#define LINUX_F_DUPFD_CLOEXEC	1030

#define LINUX_FD_CLOEXEC	1

/* Linux ioctl for terminal */
#define LINUX_TCGETS		0x5401
#define LINUX_TCSETS		0x5402
#define LINUX_TIOCGWINSZ	0x5413
#define LINUX_TIOCSWINSZ	0x5414
#define LINUX_TIOCGPGRP		0x540F
#define LINUX_TIOCSPGRP		0x5410

/* Linux AT_* flags */
#define LINUX_AT_REMOVEDIR		0x200
#define LINUX_AT_SYMLINK_NOFOLLOW	0x100
#define LINUX_AT_EMPTY_PATH		0x1000

/* Linux dirent type values */
#define LINUX_DT_UNKNOWN	0
#define LINUX_DT_FIFO		1
#define LINUX_DT_CHR		2
#define LINUX_DT_DIR		4
#define LINUX_DT_BLK		6
#define LINUX_DT_REG		8
#define LINUX_DT_LNK		10
#define LINUX_DT_SOCK		12

/*
 * Convert host errno to Linux errno.
 * On a Linux host these are the same, but we define this for portability.
 */
static int64_t
neg_errno(int host_errno_val)
{
	/* Map common host errno to Linux errno. */
	switch (host_errno_val) {
	case 0:		return 0;
#ifdef EPERM
	case EPERM:	return -LINUX_EPERM;
#endif
#ifdef ENOENT
	case ENOENT:	return -LINUX_ENOENT;
#endif
#ifdef EINTR
	case EINTR:	return -LINUX_EINTR;
#endif
#ifdef EIO
	case EIO:	return -LINUX_EIO;
#endif
#ifdef EBADF
	case EBADF:	return -LINUX_EBADF;
#endif
#ifdef EAGAIN
	case EAGAIN:	return -LINUX_EAGAIN;
#endif
#ifdef ENOMEM
	case ENOMEM:	return -LINUX_ENOMEM;
#endif
#ifdef EACCES
	case EACCES:	return -LINUX_EACCES;
#endif
#ifdef EFAULT
	case EFAULT:	return -LINUX_EFAULT;
#endif
#ifdef EEXIST
	case EEXIST:	return -LINUX_EEXIST;
#endif
#ifdef ENOTDIR
	case ENOTDIR:	return -LINUX_ENOTDIR;
#endif
#ifdef EISDIR
	case EISDIR:	return -LINUX_EISDIR;
#endif
#ifdef EINVAL
	case EINVAL:	return -LINUX_EINVAL;
#endif
#ifdef EMFILE
	case EMFILE:	return -LINUX_EMFILE;
#endif
#ifdef ENOSPC
	case ENOSPC:	return -LINUX_ENOSPC;
#endif
#ifdef EROFS
	case EROFS:	return -LINUX_EROFS;
#endif
#ifdef EPIPE
	case EPIPE:	return -LINUX_EPIPE;
#endif
#ifdef ERANGE
	case ERANGE:	return -LINUX_ERANGE;
#endif
#ifdef ENOSYS
	case ENOSYS:	return -LINUX_ENOSYS;
#endif
#ifdef ENOTEMPTY
	case ENOTEMPTY:	return -LINUX_ENOTEMPTY;
#endif
#ifdef ELOOP
	case ELOOP:	return -LINUX_ELOOP;
#endif
#ifdef EBUSY
	case EBUSY:	return -LINUX_EBUSY;
#endif
#ifdef ENXIO
	case ENXIO:	return -LINUX_ENXIO;
#endif
#ifdef ENOTTY
	case ENOTTY:	return -LINUX_ENOTTY;
#endif
#ifdef ESPIPE
	case ESPIPE:	return -LINUX_ESPIPE;
#endif
	default:	return -LINUX_EIO;
	}
}

/* Translate Linux O_* flags to host flags. */
static int
translate_open_flags(int linux_flags)
{
	int	flags;

	flags = linux_flags & 3;	/* O_RDONLY, O_WRONLY, O_RDWR */

	if (linux_flags & LINUX_O_CREAT)	flags |= O_CREAT;
	if (linux_flags & LINUX_O_EXCL)		flags |= O_EXCL;
	if (linux_flags & LINUX_O_TRUNC)	flags |= O_TRUNC;
	if (linux_flags & LINUX_O_APPEND)	flags |= O_APPEND;
	if (linux_flags & LINUX_O_NONBLOCK)	flags |= O_NONBLOCK;
#ifdef O_DIRECTORY
	if (linux_flags & LINUX_O_DIRECTORY)	flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
	if (linux_flags & LINUX_O_CLOEXEC)	flags |= O_CLOEXEC;
#endif
#ifdef O_NOCTTY
	if (linux_flags & LINUX_O_NOCTTY)	flags |= O_NOCTTY;
#endif

	return flags;
}

/* Resolve a guest path using the VFS. */
static int
resolve_path(emu_process_t *proc, int dirfd, uint64_t path_addr,
    char *host_path, size_t host_path_size)
{
	char	guest_path[PATH_MAX];
	char	abs_path[PATH_MAX];

	if (mem_read_str(proc->mem, path_addr, guest_path,
	    sizeof(guest_path)) != 0)
		return -LINUX_EFAULT;

	/* Make path absolute relative to cwd. */
	if (guest_path[0] != '/') {
		if (dirfd == LINUX_AT_FDCWD) {
			vfs_normalize_path(proc->cwd, guest_path,
			    abs_path, sizeof(abs_path));
		} else {
			/* dirfd-relative: not fully supported, use cwd. */
			vfs_normalize_path(proc->cwd, guest_path,
			    abs_path, sizeof(abs_path));
		}
	} else {
		vfs_normalize_path("/", guest_path, abs_path,
		    sizeof(abs_path));
	}

	return vfs_resolve(proc->vfs, abs_path, host_path, host_path_size);
}

/* Fill emu_stat from host stat. */
static void
stat_to_emu(struct stat *hst, struct emu_stat *est)
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
#ifdef __linux__
	est->st_atime_sec = hst->st_atim.tv_sec;
	est->st_atime_nsec = hst->st_atim.tv_nsec;
	est->st_mtime_sec = hst->st_mtim.tv_sec;
	est->st_mtime_nsec = hst->st_mtim.tv_nsec;
	est->st_ctime_sec = hst->st_ctim.tv_sec;
	est->st_ctime_nsec = hst->st_ctim.tv_nsec;
#else
	est->st_atime_sec = hst->st_atime;
	est->st_mtime_sec = hst->st_mtime;
	est->st_ctime_sec = hst->st_ctime;
#endif
}

/*
 * Try to open via a VFS virtual mount (e.g. /dev, /proc).
 * Returns the host fd on success, -1 if the path does not belong to a mount.
 */
static int
try_vfs_open(emu_process_t *proc, int dirfd, uint64_t path_addr,
    int linux_flags, int mode, int *fd_type)
{
	char		guest_path[PATH_MAX];
	char		abs_path[PATH_MAX];
	const char	*subpath;
	vfs_mount_t	*mnt;
	int		host_flags, hfd;

	if (mem_read_str(proc->mem, path_addr, guest_path,
	    sizeof(guest_path)) != 0)
		return (-1);

	if (guest_path[0] != '/') {
		if (dirfd == LINUX_AT_FDCWD)
			vfs_normalize_path(proc->cwd, guest_path,
			    abs_path, sizeof(abs_path));
		else
			vfs_normalize_path(proc->cwd, guest_path,
			    abs_path, sizeof(abs_path));
	} else {
		vfs_normalize_path("/", guest_path, abs_path,
		    sizeof(abs_path));
	}

	mnt = vfs_find_mount(proc->vfs, abs_path, &subpath);
	if (mnt == NULL || mnt->ops->open == NULL)
		return (-1);

	host_flags = translate_open_flags(linux_flags);
	hfd = mnt->ops->open(mnt->ctx, subpath, host_flags, mode);
	if (hfd < 0)
		return (hfd);	/* negative errno */

	/* Determine fd type for special devices. */
	*fd_type = FD_FILE;
	if (strcmp(subpath, "/fb0") == 0)
		*fd_type = FD_FB;
	else if (strcmp(subpath, "/input/event0") == 0)
		*fd_type = FD_INPUT;

	return (hfd);
}

static int64_t
do_openat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int		dirfd, linux_flags, mode, host_flags, hfd, efd;
	char		host_path[PATH_MAX];
	fd_entry_t	*fde;
	int		is_cloexec;
	int		fd_type;

	dirfd = (int)(int32_t)a0;
	linux_flags = (int)a2;
	mode = (int)a3;
	is_cloexec = (linux_flags & LINUX_O_CLOEXEC) != 0;

	/* Try virtual mounts first (e.g. /dev/fb0, /dev/input/event0). */
	fd_type = FD_FILE;
	hfd = try_vfs_open(proc, dirfd, a1, linux_flags, mode, &fd_type);
	if (hfd >= 0) {
		efd = fd_alloc(proc->fds, 0);
		if (efd < 0) {
			close(hfd);
			return -LINUX_EMFILE;
		}
		fde = fd_get(proc->fds, efd);
		fde->type = fd_type;
		fde->real_fd = hfd;
		fde->flags = linux_flags;
		fde->cloexec = is_cloexec;
		LOG_TRACE("openat(vfs): type=%d -> efd=%d hfd=%d",
		    fd_type, efd, hfd);
		return efd;
	}

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	host_flags = translate_open_flags(linux_flags);

	hfd = open(host_path, host_flags, mode);
	if (hfd < 0) {
		LOG_DBG("openat: open(%s) failed", host_path);
		return neg_errno(errno);
	}

	efd = fd_alloc(proc->fds, 0);
	if (efd < 0) {
		close(hfd);
		return -LINUX_EMFILE;
	}

	fde = fd_get(proc->fds, efd);
	fde->type = FD_FILE;
	fde->real_fd = hfd;
	fde->flags = linux_flags;
	fde->cloexec = is_cloexec;

	LOG_TRACE("openat: %s -> efd=%d hfd=%d", host_path, efd, hfd);
	return efd;
}

static int64_t
do_close(emu_process_t *proc, uint64_t a0)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	fd_close(proc->fds, fd);
	return 0;
}

static int64_t
do_read(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd;
	fd_entry_t	*fde;
	void		*buf;
	ssize_t		n;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	if (a2 == 0)
		return 0;

	buf = mem_translate(proc->mem, a1, a2, MEM_PROT_WRITE);
	if (buf == NULL)
		return -LINUX_EFAULT;

	n = read(fde->real_fd, buf, (size_t)a2);
	if (n < 0)
		return neg_errno(errno);
	return n;
}

static int64_t
do_write(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd;
	fd_entry_t	*fde;
	void		*buf;
	ssize_t		n;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	if (a2 == 0)
		return 0;

	buf = mem_translate(proc->mem, a1, a2, MEM_PROT_READ);
	if (buf == NULL)
		return -LINUX_EFAULT;

	n = write(fde->real_fd, buf, (size_t)a2);
	if (n < 0)
		return neg_errno(errno);
	return n;
}

static int64_t
do_readv(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd, iovcnt, i;
	fd_entry_t	*fde;
	ssize_t		total;

	fd = (int)a0;
	iovcnt = (int)a2;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	total = 0;
	for (i = 0; i < iovcnt; i++) {
		uint64_t	iov_base, iov_len;
		void		*buf;
		ssize_t		n;

		if (mem_read64(proc->mem, a1 + i * 16, &iov_base) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a1 + i * 16 + 8, &iov_len) != 0)
			return -LINUX_EFAULT;

		if (iov_len == 0)
			continue;

		buf = mem_translate(proc->mem, iov_base, iov_len,
		    MEM_PROT_WRITE);
		if (buf == NULL)
			return -LINUX_EFAULT;

		n = read(fde->real_fd, buf, (size_t)iov_len);
		if (n < 0)
			return total > 0 ? total : neg_errno(errno);
		total += n;
		if ((size_t)n < iov_len)
			break;
	}
	return total;
}

static int64_t
do_writev(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd, iovcnt, i;
	fd_entry_t	*fde;
	ssize_t		total;

	fd = (int)a0;
	iovcnt = (int)a2;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	total = 0;
	for (i = 0; i < iovcnt; i++) {
		uint64_t	iov_base, iov_len;
		void		*buf;
		ssize_t		n;

		if (mem_read64(proc->mem, a1 + i * 16, &iov_base) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a1 + i * 16 + 8, &iov_len) != 0)
			return -LINUX_EFAULT;

		if (iov_len == 0)
			continue;

		buf = mem_translate(proc->mem, iov_base, iov_len,
		    MEM_PROT_READ);
		if (buf == NULL)
			return -LINUX_EFAULT;

		n = write(fde->real_fd, buf, (size_t)iov_len);
		if (n < 0)
			return total > 0 ? total : neg_errno(errno);
		total += n;
		if ((size_t)n < iov_len)
			break;
	}
	return total;
}

static int64_t
do_pread64(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int		fd;
	fd_entry_t	*fde;
	void		*buf;
	ssize_t		n;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	buf = mem_translate(proc->mem, a1, a2, MEM_PROT_WRITE);
	if (buf == NULL)
		return -LINUX_EFAULT;

	n = pread(fde->real_fd, buf, (size_t)a2, (off_t)a3);
	if (n < 0)
		return neg_errno(errno);
	return n;
}

static int64_t
do_pwrite64(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int		fd;
	fd_entry_t	*fde;
	void		*buf;
	ssize_t		n;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	buf = mem_translate(proc->mem, a1, a2, MEM_PROT_READ);
	if (buf == NULL)
		return -LINUX_EFAULT;

	n = pwrite(fde->real_fd, buf, (size_t)a2, (off_t)a3);
	if (n < 0)
		return neg_errno(errno);
	return n;
}

static int64_t
do_lseek(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd;
	fd_entry_t	*fde;
	off_t		off;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	off = lseek(fde->real_fd, (off_t)(int64_t)a1, (int)a2);
	if (off < 0)
		return neg_errno(errno);
	return (int64_t)off;
}

static int64_t
do_ioctl(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	switch (a1) {
	case LINUX_TIOCGWINSZ: {
		/* Return a default window size. */
		uint16_t	ws[4];

		ws[0] = 24;	/* rows */
		ws[1] = 80;	/* cols */
		ws[2] = 0;	/* xpixel */
		ws[3] = 0;	/* ypixel */
		if (mem_copy_to(proc->mem, a2, ws, sizeof(ws)) != 0)
			return -LINUX_EFAULT;
		return 0;
	}
	case LINUX_TIOCSWINSZ:
		return 0;
	case LINUX_TCGETS: {
		/* Return a basic termios. */
		uint8_t	termios[60];

		memset(termios, 0, sizeof(termios));
		if (mem_copy_to(proc->mem, a2, termios, sizeof(termios)) != 0)
			return -LINUX_EFAULT;
		return 0;
	}
	case LINUX_TCSETS:
		return 0;
	case LINUX_TIOCGPGRP: {
		uint32_t	pgrp;

		pgrp = (uint32_t)proc->pgid;
		if (mem_copy_to(proc->mem, a2, &pgrp, sizeof(pgrp)) != 0)
			return -LINUX_EFAULT;
		return 0;
	}
	case LINUX_TIOCSPGRP:
		return 0;
	case FBIOGET_VSCREENINFO: {
		struct fb_var_screeninfo	vi;
		framebuffer_t			*fb;

		if (fde->type != FD_FB)
			break;
		fb = fb_get();
		if (!fb->active)
			return -LINUX_ENODEV;
		memset(&vi, 0, sizeof(vi));
		vi.xres = fb->width;
		vi.yres = fb->height;
		vi.xres_virtual = fb->width;
		vi.yres_virtual = fb->height;
		vi.bits_per_pixel = FB_BPP;
		/* BGRA layout */
		vi.blue.offset = 0;
		vi.blue.length = 8;
		vi.green.offset = 8;
		vi.green.length = 8;
		vi.red.offset = 16;
		vi.red.length = 8;
		vi.transp.offset = 24;
		vi.transp.length = 8;
		vi.height = 0xffffffff;	/* unknown */
		vi.width = 0xffffffff;
		if (mem_copy_to(proc->mem, a2, &vi, sizeof(vi)) != 0)
			return -LINUX_EFAULT;
		return 0;
	}
	case FBIOPUT_VSCREENINFO:
		if (fde->type != FD_FB)
			break;
		return 0;
	case FBIOGET_FSCREENINFO: {
		struct fb_fix_screeninfo	fi;
		framebuffer_t			*fb;

		if (fde->type != FD_FB)
			break;
		fb = fb_get();
		if (!fb->active)
			return -LINUX_ENODEV;
		memset(&fi, 0, sizeof(fi));
		snprintf(fi.id, sizeof(fi.id), "emufb");
		fi.smem_len = (uint32_t)fb->size;
		fi.type = 0;		/* FB_TYPE_PACKED_PIXELS */
		fi.visual = 2;		/* FB_VISUAL_TRUECOLOR */
		fi.line_length = fb->stride;
		if (mem_copy_to(proc->mem, a2, &fi, sizeof(fi)) != 0)
			return -LINUX_EFAULT;
		return 0;
	}
	default:
		LOG_DBG("ioctl: unhandled cmd 0x%llx", (unsigned long long)a1);
		return -LINUX_ENOTTY;
	}
	/* Fall through from fb ioctls on non-fb fds. */
	LOG_DBG("ioctl: unhandled cmd 0x%llx", (unsigned long long)a1);
	return -LINUX_ENOTTY;
}

static int64_t
do_fcntl(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	switch ((int)a1) {
	case LINUX_F_GETFD:
		return fde->cloexec ? LINUX_FD_CLOEXEC : 0;
	case LINUX_F_SETFD:
		fde->cloexec = (a2 & LINUX_FD_CLOEXEC) ? 1 : 0;
		return 0;
	case LINUX_F_GETFL:
		return fde->flags;
	case LINUX_F_SETFL:
		fde->flags = (int)a2;
		return 0;
	case LINUX_F_DUPFD:
	case LINUX_F_DUPFD_CLOEXEC: {
		int		newfd;
		fd_entry_t	*nfde;

		newfd = fd_alloc(proc->fds, (int)a2);
		if (newfd < 0)
			return -LINUX_EMFILE;
		nfde = fd_get(proc->fds, newfd);
		nfde->type = fde->type;
		nfde->real_fd = dup(fde->real_fd);
		nfde->flags = fde->flags;
		nfde->cloexec = ((int)a1 == LINUX_F_DUPFD_CLOEXEC) ? 1 : 0;
		return newfd;
	}
	default:
		LOG_DBG("fcntl: unhandled cmd %lld", (long long)a1);
		return -LINUX_EINVAL;
	}
}

static int64_t
do_dup(emu_process_t *proc, uint64_t a0)
{
	int		fd, newfd;
	fd_entry_t	*fde, *nfde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	newfd = fd_alloc(proc->fds, 0);
	if (newfd < 0)
		return -LINUX_EMFILE;

	nfde = fd_get(proc->fds, newfd);
	nfde->type = fde->type;
	nfde->real_fd = dup(fde->real_fd);
	nfde->flags = fde->flags;
	nfde->cloexec = 0;
	return newfd;
}

static int64_t
do_dup3(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		oldfd, newfd;
	fd_entry_t	*oldfde, *newfde;

	oldfd = (int)a0;
	newfd = (int)a1;

	if (oldfd == newfd)
		return -LINUX_EINVAL;

	oldfde = fd_get(proc->fds, oldfd);
	if (oldfde == NULL || oldfde->type == FD_NONE)
		return -LINUX_EBADF;

	if (newfd < 0 || newfd >= MAX_FDS)
		return -LINUX_EBADF;

	/* Close existing fd at newfd if open. */
	fd_close(proc->fds, newfd);

	/* Access entry directly; fd_get returns NULL for FD_NONE. */
	newfde = &proc->fds->fds[newfd];
	newfde->type = oldfde->type;
	newfde->real_fd = dup(oldfde->real_fd);
	newfde->flags = oldfde->flags;
	newfde->cloexec = (a2 & LINUX_O_CLOEXEC) ? 1 : 0;
	return newfd;
}

static int64_t
do_pipe2(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		pipefd[2], efd0, efd1;
	fd_entry_t	*fde;
	int		is_cloexec;

	if (pipe(pipefd) < 0)
		return neg_errno(errno);

	is_cloexec = (a1 & LINUX_O_CLOEXEC) ? 1 : 0;

	efd0 = fd_alloc(proc->fds, 0);
	if (efd0 < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -LINUX_EMFILE;
	}
	fde = fd_get(proc->fds, efd0);
	fde->type = FD_PIPE;
	fde->real_fd = pipefd[0];
	fde->cloexec = is_cloexec;

	efd1 = fd_alloc(proc->fds, 0);
	if (efd1 < 0) {
		fd_close(proc->fds, efd0);
		close(pipefd[1]);
		return -LINUX_EMFILE;
	}
	fde = fd_get(proc->fds, efd1);
	fde->type = FD_PIPE;
	fde->real_fd = pipefd[1];
	fde->cloexec = is_cloexec;

	/* Write the two fds to guest memory. */
	int32_t	fds[2];

	fds[0] = efd0;
	fds[1] = efd1;
	if (mem_copy_to(proc->mem, a0, fds, sizeof(fds)) != 0) {
		fd_close(proc->fds, efd0);
		fd_close(proc->fds, efd1);
		return -LINUX_EFAULT;
	}

	return 0;
}

static int64_t
do_getdents64(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

#ifdef __linux__
	{
		/*
		 * On Linux host, use the raw syscall. The format matches
		 * what the guest expects.
		 */
		void		*buf;
		long		n;

		buf = mem_translate(proc->mem, a1, a2, MEM_PROT_WRITE);
		if (buf == NULL)
			return -LINUX_EFAULT;

		n = syscall(SYS_getdents64, fde->real_fd, buf, (unsigned int)a2);
		if (n < 0)
			return neg_errno(errno);
		return n;
	}
#else
	{
		/*
		 * Non-Linux host: use opendir/readdir and build entries
		 * manually.
		 */
		DIR		*dp;
		struct dirent	*de;
		uint64_t	pos, end;
		int		hfd;

		hfd = fde->real_fd;

		/*
		 * Re-open the directory from the fd. This is a workaround
		 * since fdopendir takes ownership of the fd.
		 */
		int	dupfd;

		dupfd = dup(hfd);
		if (dupfd < 0)
			return neg_errno(errno);
		dp = fdopendir(dupfd);
		if (dp == NULL) {
			close(dupfd);
			return neg_errno(errno);
		}

		pos = a1;
		end = a1 + a2;

		while ((de = readdir(dp)) != NULL) {
			uint16_t	reclen;
			uint8_t		dtype;
			size_t		namelen;

			namelen = strlen(de->d_name);
			/* struct: ino(8) + off(8) + reclen(2) + type(1) + name + padding */
			reclen = (uint16_t)(8 + 8 + 2 + 1 + namelen + 1);
			reclen = (reclen + 7) & ~7;	/* align to 8 */

			if (pos + reclen > end)
				break;

			dtype = LINUX_DT_UNKNOWN;
#ifdef DT_REG
			if (de->d_type == DT_REG) dtype = LINUX_DT_REG;
			else if (de->d_type == DT_DIR) dtype = LINUX_DT_DIR;
			else if (de->d_type == DT_LNK) dtype = LINUX_DT_LNK;
			else if (de->d_type == DT_CHR) dtype = LINUX_DT_CHR;
			else if (de->d_type == DT_BLK) dtype = LINUX_DT_BLK;
			else if (de->d_type == DT_FIFO) dtype = LINUX_DT_FIFO;
			else if (de->d_type == DT_SOCK) dtype = LINUX_DT_SOCK;
#endif

			mem_write64(proc->mem, pos, de->d_ino);
			mem_write64(proc->mem, pos + 8, 0);	/* d_off */
			mem_write16(proc->mem, pos + 16, reclen);
			mem_write8(proc->mem, pos + 18, dtype);
			mem_copy_to(proc->mem, pos + 19, de->d_name,
			    namelen + 1);

			pos += reclen;
		}

		closedir(dp);
		return (int64_t)(pos - a1);
	}
#endif
}

static int64_t
do_newfstatat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int		dirfd, flags;
	char		host_path[PATH_MAX];
	struct stat	hst;
	struct emu_stat	est;

	dirfd = (int)(int32_t)a0;
	flags = (int)a3;

	/* Handle AT_EMPTY_PATH: stat the fd itself. */
	if (flags & LINUX_AT_EMPTY_PATH) {
		fd_entry_t	*fde;

		fde = fd_get(proc->fds, dirfd);
		if (fde == NULL || fde->type == FD_NONE)
			return -LINUX_EBADF;
		if (fstat(fde->real_fd, &hst) < 0)
			return neg_errno(errno);
		stat_to_emu(&hst, &est);
		if (mem_copy_to(proc->mem, a2, &est, sizeof(est)) != 0)
			return -LINUX_EFAULT;
		return 0;
	}

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if (flags & LINUX_AT_SYMLINK_NOFOLLOW) {
		if (lstat(host_path, &hst) < 0)
			return neg_errno(errno);
	} else {
		if (stat(host_path, &hst) < 0)
			return neg_errno(errno);
	}

	stat_to_emu(&hst, &est);
	if (mem_copy_to(proc->mem, a2, &est, sizeof(est)) != 0)
		return -LINUX_EFAULT;
	return 0;
}

static int64_t
do_fstat(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		fd;
	fd_entry_t	*fde;
	struct stat	hst;
	struct emu_stat	est;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	if (fstat(fde->real_fd, &hst) < 0)
		return neg_errno(errno);

	stat_to_emu(&hst, &est);
	if (mem_copy_to(proc->mem, a1, &est, sizeof(est)) != 0)
		return -LINUX_EFAULT;
	return 0;
}

static int64_t
do_readlinkat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int		dirfd;
	char		guest_path[PATH_MAX];
	char		host_path[PATH_MAX];
	char		linkbuf[PATH_MAX];
	ssize_t		n;

	dirfd = (int)(int32_t)a0;

	if (mem_read_str(proc->mem, a1, guest_path,
	    sizeof(guest_path)) != 0)
		return -LINUX_EFAULT;

	/* Handle /proc/self/exe. */
	if (strcmp(guest_path, "/proc/self/exe") == 0) {
		const char	*exe = "/bin/busybox";
		size_t		 len;

		len = strlen(exe);
		if (len > a3)
			len = (size_t)a3;
		if (mem_copy_to(proc->mem, a2, exe, len) != 0)
			return -LINUX_EFAULT;
		return (int64_t)len;
	}

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	n = readlink(host_path, linkbuf, sizeof(linkbuf) - 1);
	if (n < 0)
		return neg_errno(errno);

	if ((size_t)n > a3)
		n = (ssize_t)a3;
	if (mem_copy_to(proc->mem, a2, linkbuf, (size_t)n) != 0)
		return -LINUX_EFAULT;
	return n;
}

static int64_t
do_faccessat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		dirfd, mode;
	char		host_path[PATH_MAX];

	dirfd = (int)(int32_t)a0;
	mode = (int)a2;

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if (access(host_path, mode) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_mkdirat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		dirfd;
	char		host_path[PATH_MAX];

	dirfd = (int)(int32_t)a0;

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if (mkdir(host_path, (mode_t)a2) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_unlinkat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		dirfd, flags;
	char		host_path[PATH_MAX];

	dirfd = (int)(int32_t)a0;
	flags = (int)a2;

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if (flags & LINUX_AT_REMOVEDIR) {
		if (rmdir(host_path) < 0)
			return neg_errno(errno);
	} else {
		if (unlink(host_path) < 0)
			return neg_errno(errno);
	}
	return 0;
}

static int64_t
do_symlinkat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	char	target[PATH_MAX];
	char	host_path[PATH_MAX];
	int	dirfd;

	dirfd = (int)(int32_t)a1;

	if (mem_read_str(proc->mem, a0, target, sizeof(target)) != 0)
		return -LINUX_EFAULT;
	if (resolve_path(proc, dirfd, a2, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if (symlink(target, host_path) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_renameat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int	olddirfd, newdirfd;
	char	old_host[PATH_MAX], new_host[PATH_MAX];

	olddirfd = (int)(int32_t)a0;
	newdirfd = (int)(int32_t)a2;

	if (resolve_path(proc, olddirfd, a1, old_host,
	    sizeof(old_host)) != 0)
		return -LINUX_ENOENT;
	if (resolve_path(proc, newdirfd, a3, new_host,
	    sizeof(new_host)) != 0)
		return -LINUX_ENOENT;

	if (rename(old_host, new_host) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_fchmodat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int	dirfd;
	char	host_path[PATH_MAX];

	dirfd = (int)(int32_t)a0;

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if (chmod(host_path, (mode_t)a2) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_fchmod(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	if (fchmod(fde->real_fd, (mode_t)a1) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_fchownat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4)
{
	(void)a2;
	(void)a3;
	(void)a4;

	/* Stub: ownership changes silently succeed. */
	int	dirfd;
	char	host_path[PATH_MAX];

	dirfd = (int)(int32_t)a0;

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	return 0;
}

static int64_t
do_fchown(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	fd_entry_t	*fde;

	(void)a1;
	(void)a2;

	fde = fd_get(proc->fds, (int)a0);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;
	return 0;
}

static int64_t
do_ftruncate(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	if (ftruncate(fde->real_fd, (off_t)a1) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_truncate(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	char	host_path[PATH_MAX];

	if (resolve_path(proc, LINUX_AT_FDCWD, a0, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if (truncate(host_path, (off_t)a1) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_fsync(emu_process_t *proc, uint64_t a0)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	if (fsync(fde->real_fd) < 0)
		return neg_errno(errno);
	return 0;
}

static int64_t
do_statfs(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	/* Return a reasonable default statfs. */
	uint8_t	buf[120];

	(void)a0;

	memset(buf, 0, sizeof(buf));
	/* f_type = EXT4_SUPER_MAGIC */
	buf[0] = 0x53;
	buf[1] = 0xef;
	/* f_bsize = 4096 */
	buf[8] = 0x00;
	buf[9] = 0x10;

	if (mem_copy_to(proc->mem, a1, buf, sizeof(buf)) != 0)
		return -LINUX_EFAULT;
	return 0;
}

static int64_t
do_statx(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4)
{
	int		dirfd;
	char		host_path[PATH_MAX];
	struct stat	hst;
	uint8_t		stx[256];

	(void)a3;	/* mask */

	dirfd = (int)(int32_t)a0;

	if (resolve_path(proc, dirfd, a1, host_path,
	    sizeof(host_path)) != 0)
		return -LINUX_ENOENT;

	if ((int)a2 & LINUX_AT_SYMLINK_NOFOLLOW) {
		if (lstat(host_path, &hst) < 0)
			return neg_errno(errno);
	} else {
		if (stat(host_path, &hst) < 0)
			return neg_errno(errno);
	}

	/* Build a minimal statx structure. */
	memset(stx, 0, sizeof(stx));

	/* stx_mask (all basic fields valid) */
	uint32_t	mask = 0x7ff;

	memcpy(stx + 0, &mask, 4);

	/* stx_blksize */
	uint32_t	blksize = (uint32_t)hst.st_blksize;

	memcpy(stx + 4, &blksize, 4);

	/* stx_nlink at offset 16 */
	uint32_t	nlink = (uint32_t)hst.st_nlink;

	memcpy(stx + 16, &nlink, 4);

	/* stx_uid, stx_gid at offset 20, 24 */
	uint32_t	uid = hst.st_uid, gid = hst.st_gid;

	memcpy(stx + 20, &uid, 4);
	memcpy(stx + 24, &gid, 4);

	/* stx_mode at offset 28 */
	uint16_t	mode = (uint16_t)hst.st_mode;

	memcpy(stx + 28, &mode, 2);

	/* stx_ino at offset 32 */
	uint64_t	ino = hst.st_ino;

	memcpy(stx + 32, &ino, 8);

	/* stx_size at offset 40 */
	uint64_t	sz = (uint64_t)hst.st_size;

	memcpy(stx + 40, &sz, 8);

	/* stx_blocks at offset 48 */
	uint64_t	blocks = (uint64_t)hst.st_blocks;

	memcpy(stx + 48, &blocks, 8);

	if (mem_copy_to(proc->mem, a4, stx, 256) != 0)
		return -LINUX_EFAULT;
	return 0;
}

static int64_t
do_sendfile(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int		out_fd, in_fd;
	fd_entry_t	*out_fde, *in_fde;
	size_t		count, done;
	char		buf[8192];

	out_fd = (int)a0;
	in_fd = (int)a1;
	count = (size_t)a3;

	out_fde = fd_get(proc->fds, out_fd);
	in_fde = fd_get(proc->fds, in_fd);
	if (out_fde == NULL || in_fde == NULL)
		return -LINUX_EBADF;

	/* Handle offset pointer. */
	if (a2 != 0) {
		int64_t	off;

		if (mem_read64(proc->mem, a2, (uint64_t *)&off) != 0)
			return -LINUX_EFAULT;
		lseek(in_fde->real_fd, (off_t)off, SEEK_SET);
	}

	done = 0;
	while (done < count) {
		ssize_t	nr, nw;
		size_t	chunk;

		chunk = count - done;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);

		nr = read(in_fde->real_fd, buf, chunk);
		if (nr <= 0)
			break;

		nw = write(out_fde->real_fd, buf, (size_t)nr);
		if (nw < 0)
			return done > 0 ? (int64_t)done : neg_errno(errno);

		done += (size_t)nw;
		if (nw < nr)
			break;
	}

	/* Update offset if provided. */
	if (a2 != 0) {
		int64_t	off;

		off = lseek(in_fde->real_fd, 0, SEEK_CUR);
		mem_write64(proc->mem, a2, (uint64_t)off);
	}

	return (int64_t)done;
}

static int64_t
do_utimensat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	(void)proc;
	(void)a0;
	(void)a1;
	(void)a2;
	(void)a3;
	/* Stub: silently succeed. */
	return 0;
}

static int64_t
do_flock(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	(void)proc;
	(void)a0;
	(void)a1;
	/* Stub: silently succeed. */
	return 0;
}

static int64_t
do_linkat(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4)
{
	int	olddirfd, newdirfd;
	char	old_host[PATH_MAX], new_host[PATH_MAX];

	(void)a4;

	olddirfd = (int)(int32_t)a0;
	newdirfd = (int)(int32_t)a2;

	if (resolve_path(proc, olddirfd, a1, old_host,
	    sizeof(old_host)) != 0)
		return -LINUX_ENOENT;
	if (resolve_path(proc, newdirfd, a3, new_host,
	    sizeof(new_host)) != 0)
		return -LINUX_ENOENT;

	if (link(old_host, new_host) < 0)
		return neg_errno(errno);
	return 0;
}

/* --- epoll emulation via poll() --- */

#define MAX_EPOLL_ENTRIES	256

/* epoll event flags (Linux AArch64) */
#define LINUX_EPOLLIN		0x001
#define LINUX_EPOLLOUT		0x004
#define LINUX_EPOLLERR		0x008
#define LINUX_EPOLLHUP		0x010
#define LINUX_EPOLLRDHUP	0x2000
#define LINUX_EPOLLET		0x80000000u
#define LINUX_EPOLLONESHOT	0x40000000u

/* epoll_ctl operations */
#define LINUX_EPOLL_CTL_ADD	1
#define LINUX_EPOLL_CTL_DEL	2
#define LINUX_EPOLL_CTL_MOD	3

struct epoll_entry {
	int		fd;
	int		real_fd;
	uint32_t	events;
	uint64_t	data;
};

struct epoll_instance {
	struct epoll_entry	entries[MAX_EPOLL_ENTRIES];
	int			count;
};

static int64_t
do_epoll_create1(emu_process_t *proc, uint64_t a0)
{
	int			efd;
	fd_entry_t		*fde;
	struct epoll_instance	*ep;

	ep = calloc(1, sizeof(*ep));
	if (ep == NULL)
		return -LINUX_ENOMEM;

	efd = fd_alloc(proc->fds, 0);
	if (efd < 0) {
		free(ep);
		return -LINUX_EMFILE;
	}

	fde = fd_get(proc->fds, efd);
	fde->type = FD_EPOLL;
	fde->real_fd = -1;
	fde->flags = 0;
	fde->cloexec = ((int)a0 & LINUX_O_CLOEXEC) ? 1 : 0;
	fde->private = ep;

	return efd;
}

static int64_t
do_epoll_ctl(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int			epfd, op, tfd, i;
	fd_entry_t		*epfde, *tfde;
	struct epoll_instance	*ep;
	uint32_t		events;
	uint64_t		data;

	epfd = (int)a0;
	op = (int)a1;
	tfd = (int)a2;

	epfde = fd_get(proc->fds, epfd);
	if (epfde == NULL || epfde->type != FD_EPOLL)
		return -LINUX_EBADF;

	ep = epfde->private;
	if (ep == NULL)
		return -LINUX_EBADF;

	tfde = fd_get(proc->fds, tfd);
	if (tfde == NULL || tfde->type == FD_NONE)
		return -LINUX_EBADF;

	/* struct epoll_event: uint32 events at +0, uint64 data at +4 */
	if (op != LINUX_EPOLL_CTL_DEL && a3 != 0) {
		if (mem_read32(proc->mem, a3, &events) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, a3 + 4, &data) != 0)
			return -LINUX_EFAULT;
	} else {
		events = 0;
		data = 0;
	}

	switch (op) {
	case LINUX_EPOLL_CTL_ADD:
		if (ep->count >= MAX_EPOLL_ENTRIES)
			return -LINUX_ENOMEM;
		for (i = 0; i < ep->count; i++) {
			if (ep->entries[i].fd == tfd)
				return -LINUX_EEXIST;
		}
		ep->entries[ep->count].fd = tfd;
		ep->entries[ep->count].real_fd = tfde->real_fd;
		ep->entries[ep->count].events = events;
		ep->entries[ep->count].data = data;
		ep->count++;
		return 0;

	case LINUX_EPOLL_CTL_MOD:
		for (i = 0; i < ep->count; i++) {
			if (ep->entries[i].fd == tfd) {
				ep->entries[i].events = events;
				ep->entries[i].data = data;
				return 0;
			}
		}
		return -LINUX_ENOENT;

	case LINUX_EPOLL_CTL_DEL:
		for (i = 0; i < ep->count; i++) {
			if (ep->entries[i].fd == tfd) {
				ep->entries[i] =
				    ep->entries[ep->count - 1];
				ep->count--;
				return 0;
			}
		}
		return -LINUX_ENOENT;

	default:
		return -LINUX_EINVAL;
	}
}

static int64_t
do_epoll_pwait(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int			epfd, maxevents, timeout, i, n, ready;
	fd_entry_t		*epfde;
	struct epoll_instance	*ep;
	struct pollfd		*pfds;

	epfd = (int)a0;
	maxevents = (int)a2;
	timeout = (int)(int32_t)a3;

	epfde = fd_get(proc->fds, epfd);
	if (epfde == NULL || epfde->type != FD_EPOLL)
		return -LINUX_EBADF;

	ep = epfde->private;
	if (ep == NULL)
		return -LINUX_EBADF;

	if (maxevents <= 0)
		return -LINUX_EINVAL;

	if (ep->count == 0) {
		if (timeout == 0)
			return 0;
		if (timeout > 0) {
			struct timespec ts;

			ts.tv_sec = timeout / 1000;
			ts.tv_nsec = (timeout % 1000) * 1000000L;
			nanosleep(&ts, NULL);
		}
		return 0;
	}

	pfds = calloc((size_t)ep->count, sizeof(*pfds));
	if (pfds == NULL)
		return -LINUX_ENOMEM;

	for (i = 0; i < ep->count; i++) {
		pfds[i].fd = ep->entries[i].real_fd;
		pfds[i].events = 0;
		if (ep->entries[i].events & LINUX_EPOLLIN)
			pfds[i].events |= POLLIN;
		if (ep->entries[i].events & LINUX_EPOLLOUT)
			pfds[i].events |= POLLOUT;
		if (ep->entries[i].events & LINUX_EPOLLRDHUP)
			pfds[i].events |= POLLHUP;
	}

	n = poll(pfds, (nfds_t)ep->count, timeout);
	if (n < 0) {
		free(pfds);
		return neg_errno(errno);
	}

	ready = 0;
	for (i = 0; i < ep->count && ready < maxevents; i++) {
		uint32_t	revents;
		uint64_t	addr;

		if (pfds[i].revents == 0)
			continue;

		revents = 0;
		if (pfds[i].revents & POLLIN)
			revents |= LINUX_EPOLLIN;
		if (pfds[i].revents & POLLOUT)
			revents |= LINUX_EPOLLOUT;
		if (pfds[i].revents & POLLERR)
			revents |= LINUX_EPOLLERR;
		if (pfds[i].revents & POLLHUP)
			revents |= LINUX_EPOLLHUP;

		/* struct epoll_event: events(4) + data(8) = 12 bytes */
		addr = a1 + (uint64_t)ready * 12;
		if (mem_write32(proc->mem, addr, revents) != 0) {
			free(pfds);
			return -LINUX_EFAULT;
		}
		if (mem_write64(proc->mem, addr + 4,
		    ep->entries[i].data) != 0) {
			free(pfds);
			return -LINUX_EFAULT;
		}
		ready++;
	}

	free(pfds);
	return ready;
}

/* --- eventfd emulation via pipe --- */

struct eventfd_state {
	int	pipefd[2];
	int	semaphore;
};

#define LINUX_EFD_SEMAPHORE	1
#define LINUX_EFD_CLOEXEC	LINUX_O_CLOEXEC
#define LINUX_EFD_NONBLOCK	LINUX_O_NONBLOCK

static int64_t
do_eventfd2(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int			efd;
	fd_entry_t		*fde;
	struct eventfd_state	*evs;

	evs = calloc(1, sizeof(*evs));
	if (evs == NULL)
		return -LINUX_ENOMEM;

	if (pipe(evs->pipefd) < 0) {
		free(evs);
		return neg_errno(errno);
	}

	evs->semaphore = ((int)a1 & LINUX_EFD_SEMAPHORE) ? 1 : 0;

	if (a0 > 0) {
		uint64_t	initval;

		initval = a0;
		(void)write(evs->pipefd[1], &initval, sizeof(initval));
	}

	efd = fd_alloc(proc->fds, 0);
	if (efd < 0) {
		close(evs->pipefd[0]);
		close(evs->pipefd[1]);
		free(evs);
		return -LINUX_EMFILE;
	}

	fde = fd_get(proc->fds, efd);
	fde->type = FD_EVENT;
	fde->real_fd = evs->pipefd[0];
	fde->flags = 0;
	fde->cloexec = ((int)a1 & LINUX_EFD_CLOEXEC) ? 1 : 0;
	fde->private = evs;

	if ((int)a1 & LINUX_EFD_NONBLOCK) {
		int	fl;

		fl = fcntl(evs->pipefd[0], F_GETFL);
		fcntl(evs->pipefd[0], F_SETFL, fl | O_NONBLOCK);
		fl = fcntl(evs->pipefd[1], F_GETFL);
		fcntl(evs->pipefd[1], F_SETFL, fl | O_NONBLOCK);
	}

	return efd;
}

/* --- timerfd emulation via pipe + thread --- */

struct timerfd_state {
	int		pipefd[2];
	pthread_t	thread;
	int		running;
	int64_t		interval_ns;
	pthread_mutex_t	lock;
};

static void *
timerfd_thread(void *arg)
{
	struct timerfd_state	*tfs;
	struct timespec		 ts;
	uint64_t		 val;

	tfs = arg;
	val = 1;

	for (;;) {
		pthread_mutex_lock(&tfs->lock);
		if (!tfs->running || tfs->interval_ns <= 0) {
			pthread_mutex_unlock(&tfs->lock);
			break;
		}
		ts.tv_sec = tfs->interval_ns / 1000000000L;
		ts.tv_nsec = tfs->interval_ns % 1000000000L;
		pthread_mutex_unlock(&tfs->lock);

		nanosleep(&ts, NULL);

		pthread_mutex_lock(&tfs->lock);
		if (!tfs->running) {
			pthread_mutex_unlock(&tfs->lock);
			break;
		}
		pthread_mutex_unlock(&tfs->lock);

		(void)write(tfs->pipefd[1], &val, sizeof(val));
	}
	return NULL;
}

static int64_t
do_timerfd_create(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int			efd;
	fd_entry_t		*fde;
	struct timerfd_state	*tfs;

	(void)a0;	/* clockid */

	tfs = calloc(1, sizeof(*tfs));
	if (tfs == NULL)
		return -LINUX_ENOMEM;

	if (pipe(tfs->pipefd) < 0) {
		free(tfs);
		return neg_errno(errno);
	}

	{
		int fl;

		fl = fcntl(tfs->pipefd[0], F_GETFL);
		fcntl(tfs->pipefd[0], F_SETFL, fl | O_NONBLOCK);
	}

	pthread_mutex_init(&tfs->lock, NULL);
	tfs->running = 0;
	tfs->interval_ns = 0;

	efd = fd_alloc(proc->fds, 0);
	if (efd < 0) {
		close(tfs->pipefd[0]);
		close(tfs->pipefd[1]);
		free(tfs);
		return -LINUX_EMFILE;
	}

	fde = fd_get(proc->fds, efd);
	fde->type = FD_PIPE;
	fde->real_fd = tfs->pipefd[0];
	fde->flags = 0;
	fde->cloexec = ((int)a1 & LINUX_O_CLOEXEC) ? 1 : 0;
	fde->private = tfs;

	return efd;
}

static int64_t
do_timerfd_settime(emu_process_t *proc, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3)
{
	int			fd;
	fd_entry_t		*fde;
	struct timerfd_state	*tfs;
	uint64_t		sec, nsec;

	(void)a1;	/* flags */

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type == FD_NONE)
		return -LINUX_EBADF;

	tfs = fde->private;
	if (tfs == NULL)
		return -LINUX_EINVAL;

	if (a3 != 0) {
		uint8_t	zbuf[32];

		memset(zbuf, 0, sizeof(zbuf));
		mem_copy_to(proc->mem, a3, zbuf, sizeof(zbuf));
	}

	/* struct itimerspec: it_interval(16) + it_value(16) */
	if (a2 == 0)
		return -LINUX_EINVAL;

	if (mem_read64(proc->mem, a2, &sec) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a2 + 8, &nsec) != 0)
		return -LINUX_EFAULT;

	pthread_mutex_lock(&tfs->lock);

	if (tfs->running) {
		tfs->running = 0;
		pthread_mutex_unlock(&tfs->lock);
		pthread_join(tfs->thread, NULL);
		pthread_mutex_lock(&tfs->lock);
	}

	tfs->interval_ns = (int64_t)(sec * 1000000000ULL + nsec);

	if (tfs->interval_ns == 0) {
		uint64_t	val_sec, val_nsec;

		if (mem_read64(proc->mem, a2 + 16, &val_sec) == 0 &&
		    mem_read64(proc->mem, a2 + 24, &val_nsec) == 0) {
			int64_t val_ns;

			val_ns = (int64_t)(val_sec * 1000000000ULL +
			    val_nsec);
			if (val_ns > 0)
				tfs->interval_ns = val_ns;
		}
	}

	if (tfs->interval_ns > 0) {
		tfs->running = 1;
		pthread_create(&tfs->thread, NULL, timerfd_thread, tfs);
	}

	pthread_mutex_unlock(&tfs->lock);
	return 0;
}

/* --- ppoll --- */

static int64_t
do_ppoll(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int		nfds, i, n, timeout_ms;
	struct pollfd	*pfds;

	(void)a3;	/* sigmask */

	nfds = (int)a1;
	if (nfds < 0)
		return -LINUX_EINVAL;
	if (nfds == 0) {
		if (a2 != 0) {
			uint64_t	sec, nsec;
			struct timespec	ts;

			if (mem_read64(proc->mem, a2, &sec) != 0)
				return -LINUX_EFAULT;
			if (mem_read64(proc->mem, a2 + 8, &nsec) != 0)
				return -LINUX_EFAULT;
			ts.tv_sec = (time_t)sec;
			ts.tv_nsec = (long)nsec;
			nanosleep(&ts, NULL);
		}
		return 0;
	}

	pfds = calloc((size_t)nfds, sizeof(*pfds));
	if (pfds == NULL)
		return -LINUX_ENOMEM;

	/* struct pollfd: { int fd(4); short events(2); short revents(2); } = 8 */
	for (i = 0; i < nfds; i++) {
		uint32_t	fd_val;
		uint16_t	events_val;
		fd_entry_t	*fde;

		if (mem_read32(proc->mem, a0 + (uint64_t)i * 8,
		    &fd_val) != 0) {
			free(pfds);
			return -LINUX_EFAULT;
		}
		if (mem_read16(proc->mem, a0 + (uint64_t)i * 8 + 4,
		    &events_val) != 0) {
			free(pfds);
			return -LINUX_EFAULT;
		}

		pfds[i].fd = -1;
		pfds[i].events = (short)events_val;

		fde = fd_get(proc->fds, (int)(int32_t)fd_val);
		if (fde != NULL && fde->type != FD_NONE)
			pfds[i].fd = fde->real_fd;
	}

	timeout_ms = -1;
	if (a2 != 0) {
		uint64_t	sec, nsec;

		if (mem_read64(proc->mem, a2, &sec) == 0 &&
		    mem_read64(proc->mem, a2 + 8, &nsec) == 0)
			timeout_ms = (int)(sec * 1000 + nsec / 1000000);
	}

	n = poll(pfds, (nfds_t)nfds, timeout_ms);
	if (n < 0) {
		free(pfds);
		return neg_errno(errno);
	}

	for (i = 0; i < nfds; i++) {
		uint16_t	revents;

		revents = (uint16_t)pfds[i].revents;
		if (mem_write16(proc->mem, a0 + (uint64_t)i * 8 + 6,
		    revents) != 0) {
			free(pfds);
			return -LINUX_EFAULT;
		}
	}

	free(pfds);
	return n;
}

/* --- pselect6 --- */

static int64_t
do_pselect6(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4)
{
	int		nfds, i, n, maxfd;
	fd_set		rfds, wfds, efds;
	fd_set		*rp, *wp, *ep;
	struct timeval	tv;
	struct timeval	*tvp;
	uint8_t		bits_buf[128];

	nfds = (int)a0;
	if (nfds < 0 || nfds > 1024)
		return -LINUX_EINVAL;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	rp = NULL;
	wp = NULL;
	ep = NULL;
	maxfd = -1;

	if (a1 != 0) {
		if (mem_copy_from(proc->mem, bits_buf, a1, 128) != 0)
			return -LINUX_EFAULT;
		FD_ZERO(&rfds);
		for (i = 0; i < nfds; i++) {
			if (bits_buf[i / 8] & (1 << (i % 8))) {
				fd_entry_t *fde;

				fde = fd_get(proc->fds, i);
				if (fde != NULL && fde->type != FD_NONE &&
				    fde->real_fd >= 0) {
					FD_SET(fde->real_fd, &rfds);
					if (fde->real_fd > maxfd)
						maxfd = fde->real_fd;
				}
			}
		}
		rp = &rfds;
	}

	if (a2 != 0) {
		if (mem_copy_from(proc->mem, bits_buf, a2, 128) != 0)
			return -LINUX_EFAULT;
		FD_ZERO(&wfds);
		for (i = 0; i < nfds; i++) {
			if (bits_buf[i / 8] & (1 << (i % 8))) {
				fd_entry_t *fde;

				fde = fd_get(proc->fds, i);
				if (fde != NULL && fde->type != FD_NONE &&
				    fde->real_fd >= 0) {
					FD_SET(fde->real_fd, &wfds);
					if (fde->real_fd > maxfd)
						maxfd = fde->real_fd;
				}
			}
		}
		wp = &wfds;
	}

	if (a3 != 0) {
		if (mem_copy_from(proc->mem, bits_buf, a3, 128) != 0)
			return -LINUX_EFAULT;
		FD_ZERO(&efds);
		for (i = 0; i < nfds; i++) {
			if (bits_buf[i / 8] & (1 << (i % 8))) {
				fd_entry_t *fde;

				fde = fd_get(proc->fds, i);
				if (fde != NULL && fde->type != FD_NONE &&
				    fde->real_fd >= 0) {
					FD_SET(fde->real_fd, &efds);
					if (fde->real_fd > maxfd)
						maxfd = fde->real_fd;
				}
			}
		}
		ep = &efds;
	}

	tvp = NULL;
	if (a4 != 0) {
		uint64_t	sec, nsec;

		if (mem_read64(proc->mem, a4, &sec) == 0 &&
		    mem_read64(proc->mem, a4 + 8, &nsec) == 0) {
			tv.tv_sec = (time_t)sec;
			tv.tv_usec = (suseconds_t)(nsec / 1000);
			tvp = &tv;
		}
	}

	n = select(maxfd + 1, rp, wp, ep, tvp);
	if (n < 0)
		return neg_errno(errno);

	if (a1 != 0) {
		memset(bits_buf, 0, 128);
		for (i = 0; i < nfds; i++) {
			fd_entry_t *fde;

			fde = fd_get(proc->fds, i);
			if (fde != NULL && fde->type != FD_NONE &&
			    fde->real_fd >= 0 &&
			    FD_ISSET(fde->real_fd, &rfds))
				bits_buf[i / 8] |= (uint8_t)(1 << (i % 8));
		}
		mem_copy_to(proc->mem, a1, bits_buf, 128);
	}

	if (a2 != 0) {
		memset(bits_buf, 0, 128);
		for (i = 0; i < nfds; i++) {
			fd_entry_t *fde;

			fde = fd_get(proc->fds, i);
			if (fde != NULL && fde->type != FD_NONE &&
			    fde->real_fd >= 0 &&
			    FD_ISSET(fde->real_fd, &wfds))
				bits_buf[i / 8] |= (uint8_t)(1 << (i % 8));
		}
		mem_copy_to(proc->mem, a2, bits_buf, 128);
	}

	if (a3 != 0) {
		memset(bits_buf, 0, 128);
		for (i = 0; i < nfds; i++) {
			fd_entry_t *fde;

			fde = fd_get(proc->fds, i);
			if (fde != NULL && fde->type != FD_NONE &&
			    fde->real_fd >= 0 &&
			    FD_ISSET(fde->real_fd, &efds))
				bits_buf[i / 8] |= (uint8_t)(1 << (i % 8));
		}
		mem_copy_to(proc->mem, a3, bits_buf, 128);
	}

	return n;
}

int64_t
sys_file(emu_process_t *proc, int nr, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	(void)a5;

	switch (nr) {
	case SYS_OPENAT:
		return do_openat(proc, a0, a1, a2, a3);
	case SYS_CLOSE:
		return do_close(proc, a0);
	case SYS_READ:
		return do_read(proc, a0, a1, a2);
	case SYS_WRITE:
		return do_write(proc, a0, a1, a2);
	case SYS_READV:
		return do_readv(proc, a0, a1, a2);
	case SYS_WRITEV:
		return do_writev(proc, a0, a1, a2);
	case SYS_PREAD64:
		return do_pread64(proc, a0, a1, a2, a3);
	case SYS_PWRITE64:
		return do_pwrite64(proc, a0, a1, a2, a3);
	case SYS_LSEEK:
		return do_lseek(proc, a0, a1, a2);
	case SYS_IOCTL:
		return do_ioctl(proc, a0, a1, a2);
	case SYS_FCNTL:
		return do_fcntl(proc, a0, a1, a2);
	case SYS_DUP:
		return do_dup(proc, a0);
	case SYS_DUP3:
		return do_dup3(proc, a0, a1, a2);
	case SYS_PIPE2:
		return do_pipe2(proc, a0, a1);
	case SYS_GETDENTS64:
		return do_getdents64(proc, a0, a1, a2);
	case SYS_NEWFSTATAT:
		return do_newfstatat(proc, a0, a1, a2, a3);
	case SYS_FSTAT:
		return do_fstat(proc, a0, a1);
	case SYS_READLINKAT:
		return do_readlinkat(proc, a0, a1, a2, a3);
	case SYS_FACCESSAT:
	case SYS_FACCESSAT2:
		return do_faccessat(proc, a0, a1, a2);
	case SYS_MKDIRAT:
		return do_mkdirat(proc, a0, a1, a2);
	case SYS_UNLINKAT:
		return do_unlinkat(proc, a0, a1, a2);
	case SYS_SYMLINKAT:
		return do_symlinkat(proc, a0, a1, a2);
	case SYS_RENAMEAT:
	case SYS_RENAMEAT2:
		return do_renameat(proc, a0, a1, a2, a3);
	case SYS_FCHMODAT:
		return do_fchmodat(proc, a0, a1, a2);
	case SYS_FCHMOD:
		return do_fchmod(proc, a0, a1);
	case SYS_FCHOWNAT:
		return do_fchownat(proc, a0, a1, a2, a3, a4);
	case SYS_FCHOWN:
		return do_fchown(proc, a0, a1, a2);
	case SYS_FTRUNCATE:
		return do_ftruncate(proc, a0, a1);
	case SYS_TRUNCATE:
		return do_truncate(proc, a0, a1);
	case SYS_FSYNC:
	case SYS_FDATASYNC:
		return do_fsync(proc, a0);
	case SYS_UTIMENSAT:
		return do_utimensat(proc, a0, a1, a2, a3);
	case SYS_FLOCK:
		return do_flock(proc, a0, a1);
	case SYS_STATFS:
	case SYS_FSTATFS:
		return do_statfs(proc, a0, a1);
	case SYS_STATX:
		return do_statx(proc, a0, a1, a2, a3, a4);
	case SYS_SENDFILE:
		return do_sendfile(proc, a0, a1, a2, a3);
	case SYS_LINKAT:
		return do_linkat(proc, a0, a1, a2, a3, a4);
	case SYS_MKNODAT:
		return -LINUX_EPERM;
	case SYS_SYNC:
		sync();
		return 0;
	case SYS_EPOLL_CREATE1:
		return do_epoll_create1(proc, a0);
	case SYS_EPOLL_CTL:
		return do_epoll_ctl(proc, a0, a1, a2, a3);
	case SYS_EPOLL_PWAIT:
		return do_epoll_pwait(proc, a0, a1, a2, a3);
	case SYS_EVENTFD2:
		return do_eventfd2(proc, a0, a1);
	case SYS_TIMERFD_CREATE:
		return do_timerfd_create(proc, a0, a1);
	case SYS_TIMERFD_SETTIME:
		return do_timerfd_settime(proc, a0, a1, a2, a3);
	case SYS_PPOLL:
		return do_ppoll(proc, a0, a1, a2, a3);
	case SYS_PSELECT6:
		return do_pselect6(proc, a0, a1, a2, a3, a4);
	default:
		LOG_WARN("sys_file: unhandled nr=%d", nr);
		return -LINUX_ENOSYS;
	}
}
