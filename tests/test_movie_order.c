#include "runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* The runtime reports delivered movie frames to the GPU library (frame limit); not linked here. */
void ps4rtgpu_movie_frame(void) {}
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void *finish_later(void *movie) {
    struct timespec d = {0, 8 * 1000000};
    nanosleep(&d, NULL);
    runtime_movie_order_done(movie);
    return NULL;
}
static void put_rel(unsigned char *image, uint32_t site, uint32_t bytes, uint32_t disp_at, uint32_t target) {
    int32_t rel = (int32_t)((int64_t)target - ((int64_t)site + bytes));
    memcpy(image + site + disp_at, &rel, 4);
}
int main(void) {
    int a = 0, b = 0;
    /* Nothing pending: no wait. */
    assert(runtime_movie_order_wait(&a, 50000000ull) == 1);
    /* A queued upload holds the next request until it is done. */
    runtime_movie_order_queued(&a);
    pthread_t t; pthread_create(&t, NULL, finish_later, &a);
    uint64_t start = now_ms();
    assert(runtime_movie_order_wait(&a, 500000000ull) == 1);
    uint64_t waited = now_ms() - start;
    assert(waited >= 6 && waited < 400);
    pthread_join(t, NULL);
    /* Movies are independent. */
    runtime_movie_order_queued(&a);
    assert(runtime_movie_order_wait(&b, 50000000ull) == 1);
    runtime_movie_order_done(&a);
    /* A callback that never runs does not hold the movie up for good. */
    runtime_movie_order_queued(&a);
    start = now_ms();
    assert(runtime_movie_order_wait(&a, 20000000ull) == 0);
    assert(now_ms() - start >= 18);
    assert(runtime_movie_order_wait(&a, 20000000ull) == 1);
    /* An unmatched done does not make later waits misbehave. */
    runtime_movie_order_done(&a);
    runtime_movie_order_queued(&a);
    assert(runtime_movie_order_wait(&a, 5000000ull) == 0);
    /* More movies than slots: untracked ones are simply not held. */
    static int many[64];
    for (int i = 0; i < 64; ++i) { runtime_movie_order_queued(&many[i]); assert(runtime_movie_order_wait(&many[i], 1000000ull) <= 1); }

    /* The hook sites are patched only in this title's code. */
    const uint32_t update = 0xaddd70, refer = 0xcf9230, ready = 0xcf93f0, upload = 0xadeeb0;
    const size_t size = 0xd00000;
    unsigned char *image = calloc(1, size + 4096), *stubs = image + size;
    assert(image);
    assert(runtime_movie_order_apply(image, size, stubs) == 0);
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb};
    memcpy(image + update, prologue, sizeof(prologue));
    image[0xaddddd] = 0xe8; put_rel(image, 0xaddddd, 5, 1, refer);
    image[0xadddf1] = 0xe8; put_rel(image, 0xadddf1, 5, 1, ready + 0x10); /* wrong target */
    memcpy(image + 0xadddfb, "\x48\x8d\x3d", 3); put_rel(image, 0xadddfb, 7, 3, upload);
    unsigned char before[0x100];
    memcpy(before, image + 0xadddd0, sizeof(before));
    assert(runtime_movie_order_apply(image, size, stubs) == 0);
    assert(!memcmp(before, image + 0xadddd0, sizeof(before)) && stubs[0] == 0);
    put_rel(image, 0xadddf1, 5, 1, ready);
    assert(runtime_movie_order_apply(image, size, stubs) == 1);
    for (int i = 0; i < 3; ++i) {
        const uint32_t site[3] = {0xaddddd, 0xadddf1, 0xadddfb}, bytes[3] = {5, 5, 7}, disp[3] = {1, 1, 3};
        int32_t rel; memcpy(&rel, image + site[i] + disp[i], 4);
        assert(image + site[i] + bytes[i] + rel == stubs + 16 * i);
        assert(stubs[16 * i] == 0x48 && stubs[16 * i + 1] == 0xb8 && stubs[16 * i + 10] == 0xff && stubs[16 * i + 11] == 0xe0);
    }
    free(image);
    puts("movie order ok");
    return 0;
}
