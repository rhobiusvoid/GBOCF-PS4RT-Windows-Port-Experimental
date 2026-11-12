/* SPDX-License-Identifier: GPL-2.0-or-later
 * PS4RT Windows host helpers (see compat_win.h). */
#ifdef _WIN32
#include "compat_win.h"
#include <windows.h>
#include <bcrypt.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---- setjmp/longjmp without SEH unwinding ----
 * Microsoft x64 ABI: RCX = buffer (RDX = value for longjmp). Callee-saved state is RBX, RBP,
 * RDI, RSI, R12-R15, XMM6-XMM15, MXCSR and the x87 control word. Layout: see compat_win.h.
 * movups: the buffer is only guaranteed 8-byte aligned. */
__asm__(
    ".text\n"
    ".globl ps4rt_setjmp\n"
    "ps4rt_setjmp:\n"
    "    mov %rbx,0(%rcx)\n"
    "    mov %rbp,8(%rcx)\n"
    "    mov %rdi,16(%rcx)\n"
    "    mov %rsi,24(%rcx)\n"
    "    mov %r12,32(%rcx)\n"
    "    mov %r13,40(%rcx)\n"
    "    mov %r14,48(%rcx)\n"
    "    mov %r15,56(%rcx)\n"
    "    lea 8(%rsp),%rax\n"
    "    mov %rax,64(%rcx)\n"
    "    mov (%rsp),%rax\n"
    "    mov %rax,72(%rcx)\n"
    "    stmxcsr 80(%rcx)\n"
    "    fnstcw 84(%rcx)\n"
    "    movups %xmm6,96(%rcx)\n"
    "    movups %xmm7,112(%rcx)\n"
    "    movups %xmm8,128(%rcx)\n"
    "    movups %xmm9,144(%rcx)\n"
    "    movups %xmm10,160(%rcx)\n"
    "    movups %xmm11,176(%rcx)\n"
    "    movups %xmm12,192(%rcx)\n"
    "    movups %xmm13,208(%rcx)\n"
    "    movups %xmm14,224(%rcx)\n"
    "    movups %xmm15,240(%rcx)\n"
    "    xor %eax,%eax\n"
    "    ret\n"
    ".globl ps4rt_longjmp\n"
    "ps4rt_longjmp:\n"
    "    mov %edx,%eax\n"
    "    test %eax,%eax\n"
    "    jnz 1f\n"
    "    mov $1,%eax\n"
    "1:\n"
    "    mov 0(%rcx),%rbx\n"
    "    mov 8(%rcx),%rbp\n"
    "    mov 16(%rcx),%rdi\n"
    "    mov 24(%rcx),%rsi\n"
    "    mov 32(%rcx),%r12\n"
    "    mov 40(%rcx),%r13\n"
    "    mov 48(%rcx),%r14\n"
    "    mov 56(%rcx),%r15\n"
    "    ldmxcsr 80(%rcx)\n"
    "    fldcw 84(%rcx)\n"
    "    movups 96(%rcx),%xmm6\n"
    "    movups 112(%rcx),%xmm7\n"
    "    movups 128(%rcx),%xmm8\n"
    "    movups 144(%rcx),%xmm9\n"
    "    movups 160(%rcx),%xmm10\n"
    "    movups 176(%rcx),%xmm11\n"
    "    movups 192(%rcx),%xmm12\n"
    "    movups 208(%rcx),%xmm13\n"
    "    movups 224(%rcx),%xmm14\n"
    "    movups 240(%rcx),%xmm15\n"
    "    mov 64(%rcx),%rsp\n"
    "    jmp *72(%rcx)\n");

/* ---- Windows 10 placeholder memory (see compat_win_mem.h) ---- */
#include "compat_win_mem.h"
#undef VirtualAlloc2
#undef MapViewOfFile3
#undef UnmapViewOfFile2
#undef CreateFileMapping2
typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER *, ULONG);
typedef BOOL (WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
typedef HANDLE (WINAPI *CreateFileMapping2Fn)(HANDLE, SECURITY_ATTRIBUTES *, ULONG, ULONG, ULONG, ULONG64, PCWSTR,
                                              MEM_EXTENDED_PARAMETER *, ULONG);
static struct { VirtualAlloc2Fn alloc; MapViewOfFile3Fn map; UnmapViewOfFile2Fn unmap; CreateFileMapping2Fn create; } win_mem;
static INIT_ONCE win_mem_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK win_mem_resolve(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once; (void)parameter; (void)context;
    HMODULE module = GetModuleHandleA("kernelbase.dll");
    if (!module) module = LoadLibraryA("kernelbase.dll");
    if (module) {
        win_mem.alloc = (VirtualAlloc2Fn)(void *)GetProcAddress(module, "VirtualAlloc2");
        win_mem.map = (MapViewOfFile3Fn)(void *)GetProcAddress(module, "MapViewOfFile3");
        win_mem.unmap = (UnmapViewOfFile2Fn)(void *)GetProcAddress(module, "UnmapViewOfFile2");
        win_mem.create = (CreateFileMapping2Fn)(void *)GetProcAddress(module, "CreateFileMapping2");
    }
    if (!win_mem.alloc || !win_mem.map || !win_mem.unmap || !win_mem.create) {
        fputs("STOP: this program needs Windows 10 version 1803 or newer "
              "(VirtualAlloc2/MapViewOfFile3 placeholder memory).\n", stderr);
        ExitProcess(21);
    }
    return TRUE;
}
static void win_mem_init(void) { InitOnceExecuteOnce(&win_mem_once, win_mem_resolve, NULL, NULL); }
PVOID compat_VirtualAlloc2(HANDLE process, PVOID address, SIZE_T size, ULONG type, ULONG protect,
                           MEM_EXTENDED_PARAMETER *extended, ULONG extended_count) {
    win_mem_init();
    return win_mem.alloc(process, address, size, type, protect, extended, extended_count);
}
PVOID compat_MapViewOfFile3(HANDLE section, HANDLE process, PVOID address, ULONG64 offset, SIZE_T size,
                            ULONG type, ULONG protect, MEM_EXTENDED_PARAMETER *extended, ULONG extended_count) {
    win_mem_init();
    return win_mem.map(section, process, address, offset, size, type, protect, extended, extended_count);
}
BOOL compat_UnmapViewOfFile2(HANDLE process, PVOID address, ULONG flags) {
    win_mem_init();
    return win_mem.unmap(process, address, flags);
}
HANDLE compat_CreateFileMapping2(HANDLE file, SECURITY_ATTRIBUTES *security, ULONG access, ULONG protect,
                                 ULONG attributes, ULONG64 maximum_size, PCWSTR name,
                                 MEM_EXTENDED_PARAMETER *extended, ULONG extended_count) {
    win_mem_init();
    return win_mem.create(file, security, access, protect, attributes, maximum_size, name, extended, extended_count);
}

/* ---- time ---- */
void compat_sleep_ns(uint64_t ns) {
    /* One high-resolution timer per thread (Windows 10 1803+); older systems fall back to
     * Sleep, whose resolution follows the system timer. */
    static __thread HANDLE timer;
    static __thread int tried;
    if (!tried) {
        tried = 1;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        int64_t ticks = (int64_t)(ns / 100);
        due.QuadPart = -(ticks ? ticks : 1); /* negative: relative, 100 ns units */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)((ns + 999999u) / 1000000u));
}

int compat_utc_offset(time_t t) {
    struct tm local;
    if (localtime_s(&local, &t)) return 0;
    /* local is wall-clock time; read as if it were UTC, the difference is the offset. */
    local.tm_isdst = 0;
    time_t as_utc = _mkgmtime(&local);
    if (as_utc == (time_t)-1) return 0;
    return (int)(as_utc - t);
}

static int64_t filetime_us(FILETIME f) {
    return (int64_t)((((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10u);
}
void compat_cpu_times(int who, int64_t *user_us, int64_t *kernel_us) {
    FILETIME creation, exit_time, kernel, user;
    BOOL ok = who == 0 ? GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)
                       : GetThreadTimes(GetCurrentThread(), &creation, &exit_time, &kernel, &user);
    if (!ok) return;
    if (user_us) *user_us = filetime_us(user);
    if (kernel_us) *kernel_us = filetime_us(kernel);
}

int sched_getcpu(void) { return (int)GetCurrentProcessorNumber(); }

/* ---- positioned I/O ---- */
#define FD_LOCKS 64
static SRWLOCK fd_locks[FD_LOCKS];
static SRWLOCK *fd_lock(int fd) { return &fd_locks[(unsigned)fd % FD_LOCKS]; }
#define MAX_TRANSFER 0x7ffff000u

static ssize_t positioned(int fd, void *buffer, size_t count, int64_t offset, int writing) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    if (count > MAX_TRANSFER) count = MAX_TRANSFER;
    SRWLOCK *lock = fd_lock(fd);
    AcquireSRWLockExclusive(lock);
    /* A synchronous handle moves its file pointer even when ReadFile/WriteFile is given an
     * explicit offset, so put it back. Pipes and consoles have no position: skip both steps. */
    LARGE_INTEGER saved = {0}, zero = {0};
    BOOL has_position = SetFilePointerEx(h, zero, &saved, FILE_CURRENT);
    OVERLAPPED overlapped = {0};
    overlapped.Offset = (DWORD)((uint64_t)offset & 0xffffffffu);
    overlapped.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD done = 0;
    BOOL ok = writing ? WriteFile(h, buffer, (DWORD)count, &done, &overlapped)
                      : ReadFile(h, buffer, (DWORD)count, &done, &overlapped);
    DWORD error = ok ? 0 : GetLastError();
    if (has_position) SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    ReleaseSRWLockExclusive(lock);
    if (!ok) {
        if (error == ERROR_HANDLE_EOF) return 0; /* read at or past the end of file */
        errno = compat_errno_from_win32(error);
        return -1;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buffer, size_t count, int64_t offset) { return positioned(fd, buffer, count, offset, 0); }
ssize_t pwrite(int fd, const void *buffer, size_t count, int64_t offset) {
    return positioned(fd, (void *)buffer, count, offset, 1);
}
ssize_t compat_read_locked(int fd, void *buffer, size_t count) {
    if (count > MAX_TRANSFER) count = MAX_TRANSFER;
    SRWLOCK *lock = fd_lock(fd);
    AcquireSRWLockShared(lock); /* concurrent plain reads are the CRT's business; only pread/pwrite exclude them */
    ssize_t n = read(fd, buffer, (unsigned)count);
    ReleaseSRWLockShared(lock);
    return n;
}
ssize_t compat_write_locked(int fd, const void *buffer, size_t count) {
    if (count > MAX_TRANSFER) count = MAX_TRANSFER;
    SRWLOCK *lock = fd_lock(fd);
    AcquireSRWLockShared(lock);
    ssize_t n = write(fd, buffer, (unsigned)count);
    ReleaseSRWLockShared(lock);
    return n;
}
int fsync(int fd) { return _commit(fd); }

/* ---- environment ---- */
int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=') || !value) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    return _putenv_s(name, "") ? -1 : 0;
}

/* ---- random ---- */
int compat_random(void *buffer, size_t size) {
    unsigned char *p = buffer;
    while (size) {
        ULONG chunk = size > 0x10000000u ? 0x10000000u : (ULONG)size;
        if (BCryptGenRandom(NULL, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -1;
        p += chunk;
        size -= chunk;
    }
    return 0;
}

/* ---- file system ---- */
int compat_errno_from_win32(unsigned long error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
    case ERROR_BAD_PATHNAME: case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: case ERROR_NETWORK_ACCESS_DENIED: return EACCES;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_BUSY: return EBUSY;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return ENOMEM;
    case ERROR_INVALID_PARAMETER: case ERROR_INVALID_HANDLE: return EINVAL;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    default: return EIO;
    }
}

void compat_remove_tree(const char *path) {
    char pattern[MAX_PATH * 2];
    WIN32_FIND_DATAA entry;
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        /* A file, or a link: remove the entry itself and never follow it. */
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileA(path)) RemoveDirectoryA(path);
        return;
    }
    if (snprintf(pattern, sizeof(pattern), "%s\\*", path) >= (int)sizeof(pattern)) return;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            char child[MAX_PATH * 2];
            if (snprintf(child, sizeof(child), "%s\\%s", path, entry.cFileName) >= (int)sizeof(child)) continue;
            compat_remove_tree(child);
        } while (FindNextFileA(find, &entry));
        FindClose(find);
    }
    RemoveDirectoryA(path);
}
#endif /* _WIN32 */

/* Windows wakes waiting threads on the 15.6 ms scheduler tick unless the timer resolution is
 * raised. The game's thread hand-offs (queue pops, mutex and condition waits) assume the quick
 * wake-ups of the console, so run the whole process at 1 ms. */
#include <mmsystem.h>
__attribute__((constructor)) static void compat_timer_resolution(void) { timeBeginPeriod(1); }
