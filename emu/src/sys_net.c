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

#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "syscall.h"
#include "process.h"
#include "memory.h"
#include "log.h"

/* Linux errno */
#define LINUX_EBADF		9
#define LINUX_ENOMEM		12
#define LINUX_EFAULT		14
#define LINUX_EINVAL		22
#define LINUX_EMFILE		24
#define LINUX_ENOSYS		38
#define LINUX_ENOTSOCK		88
#define LINUX_ENOPROTOOPT	92

/* Linux O_CLOEXEC for socket flags */
#define LINUX_SOCK_CLOEXEC	0x80000
#define LINUX_SOCK_NONBLOCK	0x800

static int64_t
neg_errno_net(int host_errno_val)
{
	switch (host_errno_val) {
#ifdef EBADF
	case EBADF:		return -LINUX_EBADF;
#endif
#ifdef ENOMEM
	case ENOMEM:		return -LINUX_ENOMEM;
#endif
#ifdef EFAULT
	case EFAULT:		return -LINUX_EFAULT;
#endif
#ifdef EINVAL
	case EINVAL:		return -LINUX_EINVAL;
#endif
#ifdef EMFILE
	case EMFILE:		return -LINUX_EMFILE;
#endif
#ifdef ENOTSOCK
	case ENOTSOCK:		return -LINUX_ENOTSOCK;
#endif
#ifdef ENOPROTOOPT
	case ENOPROTOOPT:	return -LINUX_ENOPROTOOPT;
#endif
	default:		return -LINUX_EINVAL;
	}
}

static int64_t
do_socket(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		domain, type, protocol;
	int		hfd, efd, is_cloexec;
	fd_entry_t	*fde;

	domain = (int)a0;
	type = (int)a1;
	protocol = (int)a2;

	is_cloexec = (type & LINUX_SOCK_CLOEXEC) != 0;
	type &= ~(LINUX_SOCK_CLOEXEC | LINUX_SOCK_NONBLOCK);

	hfd = socket(domain, type, protocol);
	if (hfd < 0)
		return neg_errno_net(errno);

	efd = fd_alloc(proc->fds, 0);
	if (efd < 0) {
		close(hfd);
		return -LINUX_EMFILE;
	}

	fde = fd_get(proc->fds, efd);
	fde->type = FD_SOCKET;
	fde->real_fd = hfd;
	fde->flags = 0;
	fde->cloexec = is_cloexec;

	return efd;
}

static int64_t
do_socketpair(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3)
{
	int		domain, type, protocol;
	int		sv[2], efd0, efd1;
	int32_t		fds[2];
	fd_entry_t	*fde;
	int		is_cloexec;

	domain = (int)a0;
	type = (int)a1;
	protocol = (int)a2;

	is_cloexec = (type & LINUX_SOCK_CLOEXEC) != 0;
	type &= ~(LINUX_SOCK_CLOEXEC | LINUX_SOCK_NONBLOCK);

	if (socketpair(domain, type, protocol, sv) < 0)
		return neg_errno_net(errno);

	efd0 = fd_alloc(proc->fds, 0);
	if (efd0 < 0) {
		close(sv[0]);
		close(sv[1]);
		return -LINUX_EMFILE;
	}
	fde = fd_get(proc->fds, efd0);
	fde->type = FD_SOCKET;
	fde->real_fd = sv[0];
	fde->cloexec = is_cloexec;

	efd1 = fd_alloc(proc->fds, 0);
	if (efd1 < 0) {
		fd_close(proc->fds, efd0);
		close(sv[0]);
		close(sv[1]);
		return -LINUX_EMFILE;
	}
	fde = fd_get(proc->fds, efd1);
	fde->type = FD_SOCKET;
	fde->real_fd = sv[1];
	fde->cloexec = is_cloexec;

	fds[0] = efd0;
	fds[1] = efd1;
	if (mem_copy_to(proc->mem, a3, fds, sizeof(fds)) != 0) {
		fd_close(proc->fds, efd0);
		fd_close(proc->fds, efd1);
		close(sv[0]);
		close(sv[1]);
		return -LINUX_EFAULT;
	}

	return 0;
}

static int64_t
do_bind(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd;
	fd_entry_t	*fde;
	void		*addr;
	socklen_t	addrlen;

	fd = (int)a0;
	addrlen = (socklen_t)a2;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	addr = mem_translate(proc->mem, a1, addrlen, MEM_PROT_READ);
	if (addr == NULL)
		return -LINUX_EFAULT;

	if (bind(fde->real_fd, (struct sockaddr *)addr, addrlen) < 0)
		return neg_errno_net(errno);
	return 0;
}

static int64_t
do_listen(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	if (listen(fde->real_fd, (int)a1) < 0)
		return neg_errno_net(errno);
	return 0;
}

static int64_t
do_accept(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    int is_cloexec)
{
	int		fd, hfd, efd;
	fd_entry_t	*fde, *nfde;
	struct sockaddr_storage	ss;
	socklen_t	sslen;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	sslen = sizeof(ss);
	hfd = accept(fde->real_fd, (struct sockaddr *)&ss, &sslen);
	if (hfd < 0)
		return neg_errno_net(errno);

	efd = fd_alloc(proc->fds, 0);
	if (efd < 0) {
		close(hfd);
		return -LINUX_EMFILE;
	}

	nfde = fd_get(proc->fds, efd);
	nfde->type = FD_SOCKET;
	nfde->real_fd = hfd;
	nfde->cloexec = is_cloexec;

	/* Write address back if requested. */
	if (a1 != 0 && a2 != 0) {
		uint32_t	len;

		if (sslen > sizeof(ss))
			sslen = sizeof(ss);
		mem_copy_to(proc->mem, a1, &ss, sslen);
		len = (uint32_t)sslen;
		mem_copy_to(proc->mem, a2, &len, sizeof(len));
	}

	return efd;
}

static int64_t
do_connect(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd;
	fd_entry_t	*fde;
	void		*addr;
	socklen_t	addrlen;

	fd = (int)a0;
	addrlen = (socklen_t)a2;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	addr = mem_translate(proc->mem, a1, addrlen, MEM_PROT_READ);
	if (addr == NULL)
		return -LINUX_EFAULT;

	if (connect(fde->real_fd, (struct sockaddr *)addr, addrlen) < 0)
		return neg_errno_net(errno);
	return 0;
}

static int64_t
do_getsockname(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int			fd;
	fd_entry_t		*fde;
	struct sockaddr_storage	ss;
	socklen_t		sslen;
	uint32_t		len;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	sslen = sizeof(ss);
	if (getsockname(fde->real_fd, (struct sockaddr *)&ss, &sslen) < 0)
		return neg_errno_net(errno);

	if (a1 != 0) {
		mem_copy_to(proc->mem, a1, &ss, sslen);
		len = (uint32_t)sslen;
		mem_copy_to(proc->mem, a2, &len, sizeof(len));
	}
	return 0;
}

static int64_t
do_getpeername(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int			fd;
	fd_entry_t		*fde;
	struct sockaddr_storage	ss;
	socklen_t		sslen;
	uint32_t		len;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	sslen = sizeof(ss);
	if (getpeername(fde->real_fd, (struct sockaddr *)&ss, &sslen) < 0)
		return neg_errno_net(errno);

	if (a1 != 0) {
		mem_copy_to(proc->mem, a1, &ss, sslen);
		len = (uint32_t)sslen;
		mem_copy_to(proc->mem, a2, &len, sizeof(len));
	}
	return 0;
}

static int64_t
do_sendto(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5)
{
	int		fd;
	fd_entry_t	*fde;
	void		*buf;
	void		*dest_addr;
	socklen_t	addrlen;
	ssize_t		n;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	buf = mem_translate(proc->mem, a1, a2, MEM_PROT_READ);
	if (buf == NULL)
		return -LINUX_EFAULT;

	dest_addr = NULL;
	addrlen = (socklen_t)a5;
	if (a4 != 0 && addrlen > 0) {
		dest_addr = mem_translate(proc->mem, a4, addrlen,
		    MEM_PROT_READ);
		if (dest_addr == NULL)
			return -LINUX_EFAULT;
	}

	n = sendto(fde->real_fd, buf, (size_t)a2, (int)a3,
	    (struct sockaddr *)dest_addr, addrlen);
	if (n < 0)
		return neg_errno_net(errno);
	return n;
}

static int64_t
do_recvfrom(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4, uint64_t a5)
{
	int			fd;
	fd_entry_t		*fde;
	void			*buf;
	struct sockaddr_storage	ss;
	socklen_t		sslen;
	ssize_t			n;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	buf = mem_translate(proc->mem, a1, a2, MEM_PROT_WRITE);
	if (buf == NULL)
		return -LINUX_EFAULT;

	sslen = sizeof(ss);
	n = recvfrom(fde->real_fd, buf, (size_t)a2, (int)a3,
	    (a4 != 0) ? (struct sockaddr *)&ss : NULL,
	    (a4 != 0) ? &sslen : NULL);
	if (n < 0)
		return neg_errno_net(errno);

	/* Write source address back if requested. */
	if (a4 != 0 && a5 != 0) {
		uint32_t	len;

		mem_copy_to(proc->mem, a4, &ss, sslen);
		len = (uint32_t)sslen;
		mem_copy_to(proc->mem, a5, &len, sizeof(len));
	}

	return n;
}

static int64_t
do_setsockopt(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4)
{
	int		fd;
	fd_entry_t	*fde;
	void		*optval;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	optval = mem_translate(proc->mem, a3, a4, MEM_PROT_READ);
	if (optval == NULL && a4 > 0)
		return -LINUX_EFAULT;

	if (setsockopt(fde->real_fd, (int)a1, (int)a2, optval,
	    (socklen_t)a4) < 0)
		return neg_errno_net(errno);
	return 0;
}

static int64_t
do_getsockopt(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2,
    uint64_t a3, uint64_t a4)
{
	int		fd;
	fd_entry_t	*fde;
	uint32_t	optlen;
	socklen_t	hoptlen;
	uint8_t		optbuf[256];

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	if (mem_read32(proc->mem, a4, &optlen) != 0)
		return -LINUX_EFAULT;

	hoptlen = (socklen_t)optlen;
	if (hoptlen > sizeof(optbuf))
		hoptlen = sizeof(optbuf);

	if (getsockopt(fde->real_fd, (int)a1, (int)a2, optbuf,
	    &hoptlen) < 0)
		return neg_errno_net(errno);

	if (mem_copy_to(proc->mem, a3, optbuf, hoptlen) != 0)
		return -LINUX_EFAULT;

	optlen = (uint32_t)hoptlen;
	if (mem_copy_to(proc->mem, a4, &optlen, sizeof(optlen)) != 0)
		return -LINUX_EFAULT;

	return 0;
}

static int64_t
do_shutdown(emu_process_t *proc, uint64_t a0, uint64_t a1)
{
	int		fd;
	fd_entry_t	*fde;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	if (shutdown(fde->real_fd, (int)a1) < 0)
		return neg_errno_net(errno);
	return 0;
}

static int64_t
do_sendmsg(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	/*
	 * Linux msghdr layout:
	 * msg_name(8), msg_namelen(4), pad(4),
	 * msg_iov(8), msg_iovlen(8),
	 * msg_control(8), msg_controllen(8),
	 * msg_flags(4)
	 */
	int		fd, i;
	fd_entry_t	*fde;
	uint64_t	iov_ptr, iov_count;
	ssize_t		total;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	if (mem_read64(proc->mem, a1 + 16, &iov_ptr) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a1 + 24, &iov_count) != 0)
		return -LINUX_EFAULT;

	total = 0;
	for (i = 0; i < (int)iov_count; i++) {
		uint64_t	base, len;
		void		*buf;
		ssize_t		n;

		if (mem_read64(proc->mem, iov_ptr + (uint64_t)i * 16,
		    &base) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, iov_ptr + (uint64_t)i * 16 + 8,
		    &len) != 0)
			return -LINUX_EFAULT;

		if (len == 0)
			continue;

		buf = mem_translate(proc->mem, base, len, MEM_PROT_READ);
		if (buf == NULL)
			return -LINUX_EFAULT;

		n = send(fde->real_fd, buf, (size_t)len, (int)a2);
		if (n < 0)
			return total > 0 ? total : neg_errno_net(errno);
		total += n;
		if ((size_t)n < len)
			break;
	}

	return total;
}

static int64_t
do_recvmsg(emu_process_t *proc, uint64_t a0, uint64_t a1, uint64_t a2)
{
	int		fd, i;
	fd_entry_t	*fde;
	uint64_t	iov_ptr, iov_count;
	ssize_t		total;

	fd = (int)a0;
	fde = fd_get(proc->fds, fd);
	if (fde == NULL || fde->type != FD_SOCKET)
		return -LINUX_ENOTSOCK;

	if (mem_read64(proc->mem, a1 + 16, &iov_ptr) != 0)
		return -LINUX_EFAULT;
	if (mem_read64(proc->mem, a1 + 24, &iov_count) != 0)
		return -LINUX_EFAULT;

	total = 0;
	for (i = 0; i < (int)iov_count; i++) {
		uint64_t	base, len;
		void		*buf;
		ssize_t		n;

		if (mem_read64(proc->mem, iov_ptr + (uint64_t)i * 16,
		    &base) != 0)
			return -LINUX_EFAULT;
		if (mem_read64(proc->mem, iov_ptr + (uint64_t)i * 16 + 8,
		    &len) != 0)
			return -LINUX_EFAULT;

		if (len == 0)
			continue;

		buf = mem_translate(proc->mem, base, len, MEM_PROT_WRITE);
		if (buf == NULL)
			return -LINUX_EFAULT;

		n = recv(fde->real_fd, buf, (size_t)len, (int)a2);
		if (n < 0)
			return total > 0 ? total : neg_errno_net(errno);
		total += n;
		if (n == 0 || (size_t)n < len)
			break;
	}

	return total;
}

int64_t
sys_net(emu_process_t *proc, int nr, uint64_t a0, uint64_t a1,
    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	switch (nr) {
	case SYS_SOCKET:
		return do_socket(proc, a0, a1, a2);
	case SYS_SOCKETPAIR:
		return do_socketpair(proc, a0, a1, a2, a3);
	case SYS_BIND:
		return do_bind(proc, a0, a1, a2);
	case SYS_LISTEN:
		return do_listen(proc, a0, a1);
	case SYS_ACCEPT:
		return do_accept(proc, a0, a1, a2, 0);
	case SYS_ACCEPT4: {
		int	cloexec;

		cloexec = ((int)a3 & LINUX_SOCK_CLOEXEC) != 0;
		return do_accept(proc, a0, a1, a2, cloexec);
	}
	case SYS_CONNECT:
		return do_connect(proc, a0, a1, a2);
	case SYS_GETSOCKNAME:
		return do_getsockname(proc, a0, a1, a2);
	case SYS_GETPEERNAME:
		return do_getpeername(proc, a0, a1, a2);
	case SYS_SENDTO:
		return do_sendto(proc, a0, a1, a2, a3, a4, a5);
	case SYS_RECVFROM:
		return do_recvfrom(proc, a0, a1, a2, a3, a4, a5);
	case SYS_SETSOCKOPT:
		return do_setsockopt(proc, a0, a1, a2, a3, a4);
	case SYS_GETSOCKOPT:
		return do_getsockopt(proc, a0, a1, a2, a3, a4);
	case SYS_SHUTDOWN:
		return do_shutdown(proc, a0, a1);
	case SYS_SENDMSG:
		return do_sendmsg(proc, a0, a1, a2);
	case SYS_RECVMSG:
		return do_recvmsg(proc, a0, a1, a2);
	default:
		LOG_WARN("sys_net: unhandled nr=%d", nr);
		return -LINUX_ENOSYS;
	}
}
