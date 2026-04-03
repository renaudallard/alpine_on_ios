/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <limits.h>

/* Emulated stat structure (matches Linux struct stat for aarch64) */
struct emu_stat {
	uint64_t	st_dev;
	uint64_t	st_ino;
	uint32_t	st_mode;
	uint32_t	st_nlink;
	uint32_t	st_uid;
	uint32_t	st_gid;
	uint64_t	st_rdev;
	uint64_t	__pad1;
	int64_t		st_size;
	int32_t		st_blksize;
	int32_t		__pad2;
	int64_t		st_blocks;
	int64_t		st_atime_sec;
	int64_t		st_atime_nsec;
	int64_t		st_mtime_sec;
	int64_t		st_mtime_nsec;
	int64_t		st_ctime_sec;
	int64_t		st_ctime_nsec;
	uint32_t	__reserved[2];
};

/* Emulated directory entry */
struct emu_dirent64 {
	uint64_t	d_ino;
	int64_t		d_off;
	uint16_t	d_reclen;
	uint8_t		d_type;
	char		d_name[256];
};

/* Virtual filesystem operations for a mount */
typedef struct vfs_ops {
	int	(*open)(void *ctx, const char *path, int flags, int mode);
	int	(*stat)(void *ctx, const char *path, struct emu_stat *st);
	int	(*readdir)(void *ctx, const char *path, void *buf,
		    size_t bufsiz, off_t *offset);
	ssize_t	(*readlink)(void *ctx, const char *path, char *buf,
		    size_t bufsiz);
	int	(*mkdir)(void *ctx, const char *path, int mode);
	int	(*unlink)(void *ctx, const char *path);
	int	(*rmdir)(void *ctx, const char *path);
	int	(*rename)(void *ctx, const char *from, const char *to);
	int	(*chmod)(void *ctx, const char *path, int mode);
	int	(*access)(void *ctx, const char *path, int mode);
	int	(*symlink)(void *ctx, const char *target, const char *path);
	int	(*link)(void *ctx, const char *old, const char *new);
	int	(*truncate)(void *ctx, const char *path, off_t length);
	int	(*utimens)(void *ctx, const char *path, const void *times);
} vfs_ops_t;

/* Mount entry */
typedef struct vfs_mount {
	char		prefix[PATH_MAX];
	size_t		prefix_len;
	vfs_ops_t	*ops;
	void		*ctx;
	struct vfs_mount *next;
} vfs_mount_t;

/* Virtual filesystem */
typedef struct vfs {
	char		rootfs[PATH_MAX];
	vfs_mount_t	*mounts;
} vfs_t;

/* VFS lifecycle */
vfs_t	*vfs_create(const char *rootfs_path);
void	 vfs_destroy(vfs_t *vfs);

/* Mount a virtual filesystem at a prefix */
int	 vfs_mount(vfs_t *vfs, const char *prefix, vfs_ops_t *ops, void *ctx);

/* Resolve a guest path to a host path (for real FS) or find the mount */
int	 vfs_resolve(vfs_t *vfs, const char *guest_path,
	    char *host_path, size_t host_path_size);
vfs_mount_t *vfs_find_mount(vfs_t *vfs, const char *path,
	    const char **subpath);

/* Normalize a path (resolve . and ..) */
void	 vfs_normalize_path(const char *cwd, const char *path,
	    char *out, size_t outsiz);

/* Built-in filesystem backends */
extern vfs_ops_t vfs_real_ops;	/* Passthrough to host FS */
extern vfs_ops_t vfs_proc_ops;	/* /proc emulation */
extern vfs_ops_t vfs_dev_ops;	/* /dev emulation */

/* Initialize /proc, /dev, /sys, /tmp mounts */
int	 vfs_mount_defaults(vfs_t *vfs);

#endif /* VFS_H */
