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

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "vfs.h"
#include "log.h"

vfs_t *
vfs_create(const char *rootfs_path)
{
	vfs_t	*vfs;

	vfs = calloc(1, sizeof(*vfs));
	if (vfs == NULL)
		return (NULL);

	snprintf(vfs->rootfs, sizeof(vfs->rootfs), "%s", rootfs_path);
	vfs->mounts = NULL;

	if (vfs_mount_defaults(vfs) != 0) {
		LOG_WARN("vfs: failed to mount defaults");
	}

	return (vfs);
}

void
vfs_destroy(vfs_t *vfs)
{
	vfs_mount_t	*m, *next;

	if (vfs == NULL)
		return;

	for (m = vfs->mounts; m != NULL; m = next) {
		next = m->next;
		free(m);
	}
	free(vfs);
}

int
vfs_mount(vfs_t *vfs, const char *prefix, vfs_ops_t *ops, void *ctx)
{
	vfs_mount_t	*m, **pp;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return (-1);

	snprintf(m->prefix, sizeof(m->prefix), "%s", prefix);
	m->prefix_len = strlen(m->prefix);
	m->ops = ops;
	m->ctx = ctx;

	/* Insert sorted by prefix_len descending (longest prefix first). */
	for (pp = &vfs->mounts; *pp != NULL; pp = &(*pp)->next) {
		if ((*pp)->prefix_len < m->prefix_len)
			break;
	}
	m->next = *pp;
	*pp = m;

	LOG_DBG("vfs: mounted %s", prefix);
	return (0);
}

vfs_mount_t *
vfs_find_mount(vfs_t *vfs, const char *path, const char **subpath)
{
	vfs_mount_t	*m;
	size_t		 plen;

	for (m = vfs->mounts; m != NULL; m = m->next) {
		plen = m->prefix_len;

		if (strncmp(path, m->prefix, plen) != 0)
			continue;

		/* Path must match exactly or continue with '/'. */
		if (path[plen] == '\0') {
			if (subpath != NULL)
				*subpath = "/";
			return (m);
		}
		if (path[plen] == '/') {
			if (subpath != NULL)
				*subpath = path + plen;
			return (m);
		}
	}

	return (NULL);
}

/*
 * Build a host path from rootfs + guest_path, resolving symlinks
 * within the rootfs so that absolute symlink targets stay inside
 * the rootfs rather than escaping to the host filesystem.
 */
int
vfs_resolve(vfs_t *vfs, const char *guest_path, char *host_path,
    size_t host_path_size)
{
	const char	*sub;
	char		 resolved[PATH_MAX];
	char		 scratch[PATH_MAX];
	char		 link[PATH_MAX];
	char		*components[256];
	char		 pathbuf[PATH_MAX];
	char		*saveptr, *saveptr2;
	int		 ncomp, depth, i;
	ssize_t		 llen;
	struct stat	 st;

	/* Check virtual mounts first. */
	if (vfs_find_mount(vfs, guest_path, &sub) != NULL)
		return (-1);

	/* Normalize guest path (resolve . and ..) */
	vfs_normalize_path("/", guest_path, pathbuf, sizeof(pathbuf));

	/* Start with rootfs as base */
	if (snprintf(resolved, sizeof(resolved), "%s", vfs->rootfs) >=
	    (int)sizeof(resolved))
		return (-1);

	/* Split normalized path into components */
	if (strlcpy(scratch, pathbuf, sizeof(scratch)) >= sizeof(scratch))
		return (-1);

	ncomp = 0;
	for (char *tok = strtok_r(scratch, "/", &saveptr); tok != NULL;
	    tok = strtok_r(NULL, "/", &saveptr)) {
		if (ncomp >= 255)
			return (-1);
		components[ncomp++] = tok;
	}

	/* Resolve each path component, following symlinks within rootfs */
	depth = 0;
	for (i = 0; i < ncomp; i++) {
		/* Append component */
		if (snprintf(host_path, host_path_size, "%s/%s",
		    resolved, components[i]) >= (int)host_path_size)
			return (-1);

		/* Check if this component is a symlink */
		if (lstat(host_path, &st) == 0 && S_ISLNK(st.st_mode)) {
			if (++depth > 40)
				return (-1);	/* symlink loop */

			llen = readlink(host_path, link, sizeof(link) - 1);
			if (llen < 0)
				return (-1);
			link[llen] = '\0';

			if (link[0] == '/') {
				/* Absolute symlink: restart from rootfs */
				if (snprintf(resolved, sizeof(resolved),
				    "%s", vfs->rootfs) >=
				    (int)sizeof(resolved))
					return (-1);
			} else {
				/* Relative symlink: resolved stays as-is */
			}

			/* Parse symlink target and prepend remaining
			 * components to the work list. Build new
			 * component array from link + remaining. */
			char linkbuf[PATH_MAX];
			char *new_components[256];
			int new_ncomp = 0;

			if (strlcpy(linkbuf, link, sizeof(linkbuf)) >=
			    sizeof(linkbuf))
				return (-1);

			for (char *tok = strtok_r(linkbuf, "/", &saveptr2); tok != NULL;
			    tok = strtok_r(NULL, "/", &saveptr2)) {
				if (new_ncomp >= 255)
					return (-1);
				new_components[new_ncomp++] = tok;
			}

			/* Append remaining original components */
			for (int j = i + 1; j < ncomp; j++) {
				if (new_ncomp >= 255)
					return (-1);
				new_components[new_ncomp++] = components[j];
			}

			/* Copy back - need stable storage */
			static __thread char comp_store[PATH_MAX];
			char *p = comp_store;
			size_t rem = sizeof(comp_store);

			ncomp = new_ncomp;
			for (int j = 0; j < ncomp; j++) {
				size_t len = strlen(new_components[j]) + 1;
				if (len > rem)
					return (-1);
				memcpy(p, new_components[j], len);
				components[j] = p;
				p += len;
				rem -= len;
			}
			i = -1;	/* restart loop */
			continue;
		}

		/* Not a symlink (or doesn't exist yet), accept */
		if (strlcpy(resolved, host_path, sizeof(resolved)) >=
		    sizeof(resolved))
			return (-1);
	}

	if (strlcpy(host_path, resolved, host_path_size) >= host_path_size)
		return (-1);

	return (0);
}

void
vfs_normalize_path(const char *cwd, const char *path, char *out, size_t outsiz)
{
	char		 buf[PATH_MAX];
	const char	*p;
	char		*components[PATH_MAX / 2];
	int		 depth, i;
	size_t		 off;

	if (outsiz == 0)
		return;

	/* If path is relative, prepend cwd. */
	if (path[0] != '/') {
		snprintf(buf, sizeof(buf), "%s/%s", cwd, path);
		p = buf;
	} else {
		p = path;
	}

	depth = 0;
	while (*p != '\0') {
		/* Skip leading slashes. */
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;

		/* Find end of component. */
		const char *start = p;
		while (*p != '/' && *p != '\0')
			p++;

		size_t clen = (size_t)(p - start);

		if (clen == 1 && start[0] == '.') {
			/* Skip "." */
			continue;
		}
		if (clen == 2 && start[0] == '.' && start[1] == '.') {
			/* Pop one level for ".." */
			if (depth > 0)
				depth--;
			continue;
		}

		components[depth++] = (char *)start;
	}

	/* Reconstruct path. */
	if (depth == 0) {
		snprintf(out, outsiz, "/");
		return;
	}

	off = 0;
	for (i = 0; i < depth && off < outsiz - 1; i++) {
		if (off < outsiz - 1)
			out[off++] = '/';

		const char *c = components[i];
		while (*c != '/' && *c != '\0' && off < outsiz - 1)
			out[off++] = *c++;
	}
	out[off] = '\0';
}

int
vfs_mount_defaults(vfs_t *vfs)
{
	char	tmppath[PATH_MAX];

	if (vfs_mount(vfs, "/proc", &vfs_proc_ops, NULL) != 0)
		return (-1);
	if (vfs_mount(vfs, "/dev", &vfs_dev_ops, NULL) != 0)
		return (-1);

	/* Create /tmp in rootfs if it does not exist. */
	if (snprintf(tmppath, sizeof(tmppath), "%s/tmp", vfs->rootfs) <
	    (int)sizeof(tmppath))
		(void)mkdir(tmppath, 01777);

	return (0);
}
