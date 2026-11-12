/* A GPU tracker can protect a destination after preparation but before read(2).
 * Reproduce that ordering deterministically at the syscall boundary. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { PAGE_BYTES = 4096, DATA_BYTES = 16384 };
static unsigned char* destination;
static size_t destination_size=DATA_BYTES;
static volatile sig_atomic_t protect_at_read, write_faults;
ssize_t __real_read(int fd, void* buffer, size_t size);
ssize_t __real_pread(int fd, void* buffer, size_t size, off_t offset);
static void protect_destination(void) {
    if (protect_at_read) assert(!mprotect(destination, destination_size, PROT_READ));
}
ssize_t __wrap_read(int fd, void* buffer, size_t size) {
    protect_destination();
    return __real_read(fd, buffer, size);
}
ssize_t __wrap_pread(int fd, void* buffer, size_t size, off_t offset) {
    protect_destination();
    return __real_pread(fd, buffer, size, offset);
}
static void fault(int sig, siginfo_t* info, void* context) {
    (void)context;
    uintptr_t address = (uintptr_t)info->si_addr;
    if (sig != SIGSEGV || address < (uintptr_t)destination ||
        address >= (uintptr_t)destination + destination_size) _exit(90);
    ++write_faults;
    void* page = (void*)(address & ~(uintptr_t)(PAGE_BYTES - 1));
    if (mprotect(page, PAGE_BYTES, PROT_READ | PROT_WRITE)) _exit(91);
}
int main(void) {
    char directory[] = "/tmp/ps4rt-file-read-XXXXXX";
    assert(mkdtemp(directory));
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/data.bin", directory);
    int host = open(filename, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(host >= 0);
    unsigned char expected[DATA_BYTES];
    for (int i = 0; i < DATA_BYTES; ++i) expected[i] = (unsigned char)(i * 13 + i / 257);
    assert(write(host, expected, sizeof(expected)) == sizeof(expected));
    assert(!close(host));
    assert(!runtime_file_mount("/app0", directory));
    int fd = (int)runtime_file_open("/app0/data.bin", 0, 0);
    assert(fd >= 3);
    destination = mmap(NULL, DATA_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(destination != MAP_FAILED);
    struct sigaction sa = {0};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = fault;
    sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGSEGV, &sa, NULL));

    protect_at_read = 1;
    int64_t n = runtime_file_read(fd, destination, DATA_BYTES);
    if (n != DATA_BYTES) {
        fprintf(stderr, "FAIL: protected destination read returned %lld, expected %u\n",
                (long long)n, DATA_BYTES);
        return 1;
    }
    assert(!memcmp(destination, expected, DATA_BYTES));
    assert(write_faults == DATA_BYTES / PAGE_BYTES);
    assert(runtime_file_lseek(fd, 0, SEEK_CUR) == DATA_BYTES);
    assert(runtime_file_read(fd, destination, DATA_BYTES) == 0); // EOF

    assert(runtime_file_lseek(fd, 97, SEEK_SET) == 97);
    write_faults = 0;
    assert(runtime_file_pread(fd, destination + 3, 7000, 511) == 7000);
    assert(!memcmp(destination + 3, expected + 511, 7000));
    assert(write_faults == 2);
    assert(runtime_file_lseek(fd, 0, SEEK_CUR) == 97); // pread preserves offset
    assert(runtime_file_pread(fd, destination, DATA_BYTES, DATA_BYTES - 7) == 7);
    assert(!memcmp(destination, expected + DATA_BYTES - 7, 7));
    assert(runtime_file_read(fd, NULL, 0) == 0);
    assert(runtime_file_pread(fd, NULL, 0, 0) == 0);
    assert(runtime_file_pread(fd, destination, 1, -1) == -EINVAL);
    assert(runtime_file_read(-1, destination, 1) == -EBADF);
    assert(!runtime_file_close(fd));
    protect_at_read = 0;
    assert(!munmap(destination, DATA_BYTES));

    /* Exercise requests larger than the reusable scratch cap without a short transfer. */
    destination_size=2u<<20;
    host=open(filename,O_RDWR);
    assert(host>=0 && !ftruncate(host,(off_t)destination_size) && !close(host));
    fd=(int)runtime_file_open("/app0/data.bin",0,0);
    assert(fd>=3);
    destination=mmap(NULL,destination_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(destination!=MAP_FAILED);
    protect_at_read=1; write_faults=0;
    assert(runtime_file_read(fd,destination,destination_size)==(int64_t)destination_size);
    assert(write_faults==(sig_atomic_t)(destination_size/PAGE_BYTES));
    assert(!memcmp(destination,expected,DATA_BYTES));
    for (size_t i=DATA_BYTES;i<destination_size;++i) assert(destination[i]==0);
    assert(runtime_file_lseek(fd,0,SEEK_CUR)==(int64_t)destination_size);
    assert(!runtime_file_close(fd));
    protect_at_read=0;
    assert(!munmap(destination,destination_size));
    assert(!unlink(filename));
    assert(!rmdir(directory));
    puts("PASS: tracked destination read/pread, bytes, offsets, EOF and errors");
    return 0;
}
