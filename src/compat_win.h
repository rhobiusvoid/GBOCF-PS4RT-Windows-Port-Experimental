/* SPDX-License-Identifier: GPL-2.0-or-later
 * PS4RT Windows host helpers: the few POSIX behaviours the runtime relies on that the
 * MinGW-w64 CRT does not supply. Implemented in compat_win.c.
 *
 * Windows only. Nothing here is included on Linux builds. */
#ifndef PS4RT_COMPAT_WIN_H
#define PS4RT_COMPAT_WIN_H
#ifdef _WIN32
#include <stdint.h>
#include <stddef.h>
#include <time.h>
/* These system headers declare mkdir() and the O_* flags; they come first so the macros
 * below never rewrite a declaration. */
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

/* mmap-style protection bits (the runtime's memory code shares them with Linux). */
#ifndef PROT_NONE
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#endif
/* Open flags with no Windows meaning; the runtime ORs them into its host flag word. */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
/* The CRT's mkdir takes no mode. */
#define mkdir(path, mode) mkdir(path)
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Index of the logical processor the calling thread runs on. */
int sched_getcpu(void);

/* POSIX positioned I/O. Offsets are 64-bit (the CRT's off_t is 32-bit). Like POSIX they leave
 * the descriptor's own offset unchanged: the position is saved and restored under a per-fd
 * lock, which compat_read_locked/compat_write_locked share, so a plain read racing a pread
 * on the same descriptor never observes the temporary position. Counts above 0x7ffff000 are
 * clamped (a short transfer, as on Linux). Return -1 and set errno on failure. */
ssize_t pread(int fd, void *buffer, size_t count, int64_t offset);
ssize_t pwrite(int fd, const void *buffer, size_t count, int64_t offset);
ssize_t compat_read_locked(int fd, void *buffer, size_t count);
ssize_t compat_write_locked(int fd, const void *buffer, size_t count);
int fsync(int fd);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);

/* Register snapshot for ps4rt_setjmp/ps4rt_longjmp. 256 bytes; same size as the buffer type
 * gpu/shim/ps4rt_toggles.h declares for the renderer (unsigned long long[32]). Layout (bytes):
 * 0 rbx, 8 rbp, 16 rdi, 24 rsi, 32 r12, 40 r13, 48 r14, 56 r15, 64 rsp, 72 rip,
 * 80 mxcsr, 84 x87 control word, 96..255 xmm6..xmm15. */
typedef unsigned long long ps4rt_jmp_buf[32];

/* The CRT's longjmp unwinds with SEH and fails across guest frames, which carry no unwind
 * data. These two only restore registers. ps4rt_longjmp is also entered by the loader's
 * exception handler with RCX=buffer and RDX=value (Microsoft x64 ABI). */
__attribute__((returns_twice)) int ps4rt_setjmp(ps4rt_jmp_buf buffer);
__attribute__((noreturn)) void ps4rt_longjmp(ps4rt_jmp_buf buffer, int value);

/* Sleeps at least ns nanoseconds (high-resolution waitable timer where available). */
void compat_sleep_ns(uint64_t ns);
/* Seconds east of UTC at time t, daylight saving included (like tm_gmtoff). */
int compat_utc_offset(time_t t);
/* who: 0 = this process, 1 = calling thread. Times in microseconds. */
void compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us);
/* Cryptographically secure random bytes. 0 on success. */
int compat_random(void *buffer, size_t size);
/* Deletes a directory and everything below it; a missing path is not an error. */
void compat_remove_tree(const char *path);
/* A GetLastError() value as a positive POSIX errno. */
int compat_errno_from_win32(unsigned long error);

#ifdef __cplusplus
}
#endif
#endif /* _WIN32 */
#endif
