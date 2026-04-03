/*
 * Copyright (c) 2026 Alpine on iOS contributors
 * ISC License - see emu.h
 *
 * AArch64 Linux syscall numbers and emulation.
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* Forward declarations */
typedef struct emu_process emu_process_t;

/* AArch64 Linux syscall numbers (from asm-generic/unistd.h) */
#define SYS_GETCWD		17
#define SYS_DUP			23
#define SYS_DUP3		24
#define SYS_FCNTL		25
#define SYS_IOCTL		29
#define SYS_FLOCK		32
#define SYS_MKNODAT		33
#define SYS_MKDIRAT		34
#define SYS_UNLINKAT		35
#define SYS_SYMLINKAT		36
#define SYS_LINKAT		37
#define SYS_RENAMEAT		38
#define SYS_UMOUNT2		39
#define SYS_MOUNT		40
#define SYS_STATFS		43
#define SYS_FSTATFS		44
#define SYS_TRUNCATE		45
#define SYS_FTRUNCATE		46
#define SYS_FACCESSAT		48
#define SYS_CHDIR		49
#define SYS_FCHDIR		50
#define SYS_CHROOT		51
#define SYS_FCHMOD		52
#define SYS_FCHMODAT		53
#define SYS_FCHOWNAT		54
#define SYS_FCHOWN		55
#define SYS_OPENAT		56
#define SYS_CLOSE		57
#define SYS_PIPE2		59
#define SYS_GETDENTS64		61
#define SYS_LSEEK		62
#define SYS_READ		63
#define SYS_WRITE		64
#define SYS_READV		65
#define SYS_WRITEV		66
#define SYS_PREAD64		67
#define SYS_PWRITE64		68
#define SYS_SENDFILE		71
#define SYS_PSELECT6		72
#define SYS_PPOLL		73
#define SYS_READLINKAT		78
#define SYS_NEWFSTATAT		79
#define SYS_FSTAT		80
#define SYS_SYNC		81
#define SYS_FSYNC		82
#define SYS_FDATASYNC		83
#define SYS_UTIMENSAT		88
#define SYS_EXIT		93
#define SYS_EXIT_GROUP		94
#define SYS_SET_TID_ADDRESS	96
#define SYS_FUTEX		98
#define SYS_SET_ROBUST_LIST	99
#define SYS_GET_ROBUST_LIST	100
#define SYS_NANOSLEEP		101
#define SYS_GETITIMER		102
#define SYS_SETITIMER		103
#define SYS_CLOCK_GETTIME	113
#define SYS_CLOCK_GETRES	114
#define SYS_CLOCK_NANOSLEEP	115
#define SYS_SYSLOG		116
#define SYS_SCHED_YIELD		124
#define SYS_KILL		129
#define SYS_TKILL		130
#define SYS_TGKILL		131
#define SYS_SIGALTSTACK		132
#define SYS_RT_SIGSUSPEND	133
#define SYS_RT_SIGACTION	134
#define SYS_RT_SIGPROCMASK	135
#define SYS_RT_SIGPENDING	136
#define SYS_RT_SIGTIMEDWAIT	137
#define SYS_RT_SIGRETURN	139
#define SYS_SETPRIORITY		140
#define SYS_GETPRIORITY		141
#define SYS_SETREGID		143
#define SYS_SETGID		144
#define SYS_SETREUID		145
#define SYS_SETUID		146
#define SYS_SETRESUID		147
#define SYS_GETRESUID		148
#define SYS_SETRESGID		149
#define SYS_GETRESGID		150
#define SYS_TIMES		153
#define SYS_SETPGID		154
#define SYS_GETPGID		155
#define SYS_SETSID		156
#define SYS_GETGROUPS		157
#define SYS_SETGROUPS		158
#define SYS_UNAME		160
#define SYS_GETRLIMIT		163
#define SYS_SETRLIMIT		164
#define SYS_GETRUSAGE		165
#define SYS_UMASK		166
#define SYS_PRCTL		167
#define SYS_GETTIMEOFDAY	169
#define SYS_GETPID		172
#define SYS_GETPPID		173
#define SYS_GETUID		174
#define SYS_GETEUID		175
#define SYS_GETGID		176
#define SYS_GETEGID		177
#define SYS_GETTID		178
#define SYS_SYSINFO		179
#define SYS_SOCKET		198
#define SYS_SOCKETPAIR		199
#define SYS_BIND		200
#define SYS_LISTEN		201
#define SYS_ACCEPT		202
#define SYS_CONNECT		203
#define SYS_GETSOCKNAME		204
#define SYS_GETPEERNAME		205
#define SYS_SENDTO		206
#define SYS_RECVFROM		207
#define SYS_SETSOCKOPT		208
#define SYS_GETSOCKOPT		209
#define SYS_SHUTDOWN		210
#define SYS_SENDMSG		211
#define SYS_RECVMSG		212
#define SYS_BRK			214
#define SYS_MUNMAP		215
#define SYS_MREMAP		216
#define SYS_CLONE		220
#define SYS_EXECVE		221
#define SYS_MMAP		222
#define SYS_MPROTECT		226
#define SYS_MSYNC		227
#define SYS_MADVISE		233
#define SYS_ACCEPT4		242
#define SYS_WAIT4		260
#define SYS_PRLIMIT64		261
#define SYS_RENAMEAT2		276
#define SYS_GETRANDOM		278
#define SYS_MEMFD_CREATE	279
#define SYS_STATX		291
#define SYS_RSEQ		293
#define SYS_CLONE3		435
#define SYS_FACCESSAT2		439

/*
 * Handle a syscall. Called when SVC #0 is executed.
 * Syscall number in X8, args in X0-X5, result in X0.
 */
void	sys_handle(emu_process_t *proc);

/* Per-subsystem syscall handlers */
int64_t	sys_file(emu_process_t *, int nr, uint64_t, uint64_t,
	    uint64_t, uint64_t, uint64_t, uint64_t);
int64_t	sys_process(emu_process_t *, int nr, uint64_t, uint64_t,
	    uint64_t, uint64_t, uint64_t, uint64_t);
int64_t	sys_memory(emu_process_t *, int nr, uint64_t, uint64_t,
	    uint64_t, uint64_t, uint64_t, uint64_t);
int64_t	sys_signal(emu_process_t *, int nr, uint64_t, uint64_t,
	    uint64_t, uint64_t, uint64_t, uint64_t);
int64_t	sys_net(emu_process_t *, int nr, uint64_t, uint64_t,
	    uint64_t, uint64_t, uint64_t, uint64_t);
int64_t	sys_misc(emu_process_t *, int nr, uint64_t, uint64_t,
	    uint64_t, uint64_t, uint64_t, uint64_t);

#endif /* SYSCALL_H */
