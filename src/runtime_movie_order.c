/* Code Fairy (CUSA28763) movie frame / upload ordering.
 *
 * The title's movie update (FUN_00addd70) asks the CRI Mana player whether a frame is ready
 * (FUN_00cf9230 refer, FUN_00cf93f0 ready) and, when it is, queues FUN_00adeeb0 on the render
 * thread. That callback copies the frame out of the decoder's recycled buffers and releases it.
 * The title relies on the callback having finished before the next update asks again: until the
 * release the player keeps reporting the same frame as ready. The render thread starts the
 * callback right after the frame's GPU work retires, which here is the same instant the main
 * thread resumes (both wake on the previous frame's completion), so the next refer can land
 * mid-copy: the frame is queued twice and the second copy reads buffers the decoder already
 * reused. On a fresh save that happened for about 40% of the frames; the picture then steps
 * back and forth by up to the decoder's three buffers.
 *
 * Fix: the refer call of a movie waits (bounded) for that movie's queued callback to finish,
 * restoring the order the title expects. Only these three call sites are redirected, and only
 * when the code there matches this build exactly. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/ps4rtgpu.h"
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SLOTS 16
#define WAIT_LIMIT_NS 4000000ull
#define OFF_UPDATE 0xaddd70u
#define OFF_REFER_CALL 0xadddddu
#define OFF_READY_CALL 0xadddf1u
#define OFF_UPLOAD_LEA 0xadddfbu
#define OFF_REFER 0xcf9230u
#define OFF_READY 0xcf93f0u
#define OFF_UPLOAD 0xadeeb0u
#define INFO_IN_MOVIE 0xa0u

typedef struct { _Atomic uintptr_t movie; _Atomic int pending; } Slot;
static Slot slots[SLOTS];
static _Atomic unsigned long timeouts;

static Slot *find_slot(uintptr_t movie, int create) {
    for (unsigned i = 0; i < SLOTS; ++i) {
        uintptr_t seen = atomic_load_explicit(&slots[i].movie, memory_order_acquire);
        if (seen == movie) return &slots[i];
        if (!seen && create) {
            if (atomic_compare_exchange_strong(&slots[i].movie, &seen, movie) || seen == movie) return &slots[i];
        }
    }
    return NULL;
}
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* The player reported a frame ready and the title queued its upload callback. */
void runtime_movie_order_queued(const void *movie) {
    Slot *s = find_slot((uintptr_t)movie, 1);
    if (s) atomic_fetch_add_explicit(&s->pending, 1, memory_order_acq_rel);
}
/* The upload callback returned: the frame is copied and released. */
void runtime_movie_order_done(const void *movie) {
    Slot *s = find_slot((uintptr_t)movie, 0);
    if (s && atomic_fetch_sub_explicit(&s->pending, 1, memory_order_acq_rel) <= 0)
        atomic_store_explicit(&s->pending, 0, memory_order_release);
}
/* Waits until the movie has no upload pending. Returns 1 when it has none, 0 when the limit
 * passed (a callback that never ran, e.g. its queue was full): the count is then reset so the
 * movie is not held up again. */
int runtime_movie_order_wait(const void *movie, uint64_t limit_ns) {
    Slot *s = find_slot((uintptr_t)movie, 0);
    if (!s || atomic_load_explicit(&s->pending, memory_order_acquire) <= 0) return 1;
    const uint64_t deadline = now_ns() + limit_ns;
    for (unsigned spin = 0;; ++spin) {
        if (atomic_load_explicit(&s->pending, memory_order_acquire) <= 0) return 1;
        if (now_ns() >= deadline) break;
        if (spin < 2000) __builtin_ia32_pause(); else sched_yield();
    }
    atomic_store_explicit(&s->pending, 0, memory_order_release);
    if (atomic_fetch_add(&timeouts, 1) < 5)
        fprintf(stderr, "Runtime: movie %p upload still pending after %llu us; continuing\n", movie,
                (unsigned long long)(limit_ns / 1000));
    return 0;
}

typedef int32_t (ABI *FrameCall)(void *player, void *info);
typedef void (ABI *UploadCall)(void *ctx, void *movie);
static FrameCall orig_refer, orig_ready;
static UploadCall orig_upload;

static ABI int32_t refer_hook(void *player, void *info) {
    runtime_movie_order_wait((unsigned char *)info - INFO_IN_MOVIE, WAIT_LIMIT_NS);
    return orig_refer(player, info);
}
static ABI int32_t ready_hook(void *player, void *info) {
    const int32_t ready = orig_ready(player, info);
    if (ready == 1) {
        runtime_movie_order_queued((unsigned char *)info - INFO_IN_MOVIE);
        ps4rtgpu_movie_frame();
    }
    return ready;
}
static ABI void upload_hook(void *ctx, void *movie) {
    orig_upload(ctx, movie);
    runtime_movie_order_done(movie);
}

/* movabs rax, target; jmp rax */
static void write_stub(unsigned char *stub, const void *target) {
    stub[0] = 0x48; stub[1] = 0xb8;
    memcpy(stub + 2, &target, 8);
    stub[10] = 0xff; stub[11] = 0xe0;
}
static int rel32_target_is(const unsigned char *image, uint32_t site, uint32_t instruction_bytes,
                           uint32_t disp_at, uint32_t target) {
    int32_t rel;
    memcpy(&rel, image + site + disp_at, 4);
    return (int64_t)site + instruction_bytes + rel == (int64_t)target;
}
static void patch_rel32(unsigned char *image, uint32_t site, uint32_t instruction_bytes, uint32_t disp_at,
                        const unsigned char *stub) {
    const int64_t rel = (int64_t)(intptr_t)stub - ((int64_t)(intptr_t)image + site + instruction_bytes);
    const int32_t rel32 = (int32_t)rel;
    memcpy(image + site + disp_at, &rel32, 4);
}

/* Installs the hooks when image is this title's executable; stubs: 4096 bytes within rel32 reach
 * of the image, made executable by the caller. Returns 1 when installed. */
int runtime_movie_order_apply(unsigned char *image, uint64_t image_size, unsigned char *stubs) {
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb};
    if (image_size < OFF_REFER + 16 || memcmp(image + OFF_UPDATE, prologue, sizeof(prologue))) return 0;
    if (image[OFF_REFER_CALL] != 0xe8 || image[OFF_READY_CALL] != 0xe8 ||
        memcmp(image + OFF_UPLOAD_LEA, "\x48\x8d\x3d", 3) ||
        !rel32_target_is(image, OFF_REFER_CALL, 5, 1, OFF_REFER) ||
        !rel32_target_is(image, OFF_READY_CALL, 5, 1, OFF_READY) ||
        !rel32_target_is(image, OFF_UPLOAD_LEA, 7, 3, OFF_UPLOAD)) {
        fputs("Runtime: movie upload ordering not applied (code differs from the reference build)\n", stderr);
        return 0;
    }
    orig_refer = (FrameCall)(image + OFF_REFER);
    orig_ready = (FrameCall)(image + OFF_READY);
    orig_upload = (UploadCall)(image + OFF_UPLOAD);
    write_stub(stubs, (const void *)refer_hook);
    write_stub(stubs + 16, (const void *)ready_hook);
    write_stub(stubs + 32, (const void *)upload_hook);
    patch_rel32(image, OFF_REFER_CALL, 5, 1, stubs);
    patch_rel32(image, OFF_READY_CALL, 5, 1, stubs + 16);
    patch_rel32(image, OFF_UPLOAD_LEA, 7, 3, stubs + 32);
    puts("Runtime: movie upload ordering applied (3 call sites)");
    return 1;
}
