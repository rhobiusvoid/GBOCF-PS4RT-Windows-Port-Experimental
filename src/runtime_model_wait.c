/* Code Fairy (CUSA28763) model-manager reply wait.
 *
 * The main thread's end-of-frame call FUN_00ba8cd0 takes the model manager's lock (object+0xe8),
 * and while holding it waits for the manager thread's reply to a request it sent earlier
 * (FUN_00a46840 on the queue at object+0xd8, flag byte at object+0xe0). The manager thread
 * (FUN_00ba8d40) needs that same lock to handle the request and send the reply. When the main
 * thread reaches the lock first both wait on each other forever: the manager thread sits in the
 * mutex, the main thread in the reply pop. The console's thread wake-up is quick enough that the
 * manager thread nearly always wins; here the slower wake-up (worse during heavy loads) loses the
 * race now and then and the game stops at a load screen.
 *
 * Fix: the reply wait runs before the lock is taken. The reply is what the lock was guarding
 * against, so nothing the lock protected is exposed earlier. The function is replaced only when
 * the code there matches this build exactly. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define OFF_WAIT_ALL 0xba8cd0u
#define OFF_REPLY_POP 0xa46840u
#define OBJ_QUEUE 0xd8u
#define OBJ_PENDING 0xe0u

typedef void (ABI *ReplyPop)(void *queue);
static ReplyPop reply_pop;

static ABI void wait_hook(unsigned char *obj) {
    void *queue = *(void **)(obj + OBJ_QUEUE);
    if (queue && __atomic_exchange_n(obj + OBJ_PENDING, 0, __ATOMIC_ACQ_REL)) reply_pop(queue);
}

int runtime_model_wait_apply(unsigned char *image, uint64_t image_size, unsigned char *stub) {
    /* push rbp; mov rbp,rsp; push r14; push rbx; sub rsp,16 ... lea rsi,[rdi+0xe8] ... call lock */
    static const unsigned char entry[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x10};
    static const unsigned char lock_arg[] = {0x48, 0x8d, 0xb7, 0xe8, 0x00, 0x00, 0x00};
    static const unsigned char pop_entry[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x18};
    static const unsigned char reads[] = {0x48, 0x8b, 0xbb, 0xd8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xff, 0x74, 0x15,
                                          0x80, 0xbb, 0xe0, 0x00, 0x00, 0x00, 0x00};
    if (image_size < OFF_WAIT_ALL + 109 || image_size < OFF_REPLY_POP + 16) return 0;
    if (memcmp(image + OFF_WAIT_ALL, entry, sizeof(entry)) ||
        memcmp(image + OFF_WAIT_ALL + 21, lock_arg, sizeof(lock_arg)) ||
        memcmp(image + OFF_WAIT_ALL + 44, reads, sizeof(reads)) ||
        memcmp(image + OFF_REPLY_POP, pop_entry, sizeof(pop_entry))) {
        fputs("Runtime: model manager wait fix not applied (code differs from the reference build)\n", stderr);
        return 0;
    }
    reply_pop = (ReplyPop)(image + OFF_REPLY_POP);
    stub[0] = 0x48; stub[1] = 0xb8;
    const void *target = (const void *)wait_hook;
    memcpy(stub + 2, &target, 8);
    stub[10] = 0xff; stub[11] = 0xe0;
    /* jmp rel32 over the old entry, via the stub */
    const int64_t rel = (int64_t)(intptr_t)stub - ((int64_t)(intptr_t)image + OFF_WAIT_ALL + 5);
    const int32_t rel32 = (int32_t)rel;
    image[OFF_WAIT_ALL] = 0xe9;
    memcpy(image + OFF_WAIT_ALL + 1, &rel32, 4);
    puts("Runtime: model manager reply wait moved before its lock");
    return 1;
}
