/* libSceUlt host subset: waiting-queue / queue-data resource pools and queues.
 * The game keeps these objects in its own storage and hands the library
 * work areas; the host keeps its state on the heap and stores a magic plus a
 * pointer in the first 16 bytes of each guest object. Call shapes come from
 * the game's wrappers (REA: eboot wrappers around the Ult thunks). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if PS4RT_DIAGNOSTICS && !defined(_WIN32)
#include <unistd.h>
#include <sys/syscall.h>
#endif

#define ULT_BUSY    ((int32_t)0x80810007)
#define ULT_EMPTY   ((int32_t)0x80810008)
#define ULT_INVALID ((int32_t)0x80810002)
#define MAGIC_WAIT  0x554c545741495431ull
#define MAGIC_DATA  0x554c544441544131ull
#define MAGIC_QUEUE 0x554c545155455531ull

typedef struct { uint64_t magic; void *host; } Handle;
typedef struct { uint32_t threads,sync; } WaitPool;
typedef struct { uint32_t capacity,size; } DataPool;
typedef struct {
    pthread_mutex_t lock; pthread_cond_t ready;
    DataPool *pool; unsigned char *slots; uint32_t head,count,size,capacity,base_capacity;
    uint32_t busy_streak; /* consecutive full pushes; the game retries every 100 us */
} Queue;
static unsigned long pools_created, queues_created, pushes, pops;

#if PS4RT_DIAGNOSTICS && !defined(_WIN32)
/* Ring of recent queue operations, readable from a hung process (symbol
 * ps4rt_ult_trace / ps4rt_ult_trace_pos) to see which request got no reply. */
typedef struct { uint32_t tid; uint32_t op; void *queue; uint32_t count; uint32_t result; } UltTrace;
enum { TR_PUSH=1, TR_POP_WAIT, TR_POP_DONE, TR_TRY_POP };
#define ULT_TRACE_SIZE 8192
UltTrace ps4rt_ult_trace[ULT_TRACE_SIZE];
unsigned long ps4rt_ult_trace_pos;
static void trace(uint32_t op,Queue *q,int32_t result) {
    unsigned long i=__atomic_fetch_add(&ps4rt_ult_trace_pos,1,__ATOMIC_RELAXED)%ULT_TRACE_SIZE;
    ps4rt_ult_trace[i]=(UltTrace){(uint32_t)syscall(SYS_gettid),op,q,q->count,(uint32_t)result};
}
#else
#define trace(op,q,result) ((void)0)
#endif

static int valid(Handle *h,uint64_t magic) { return h && h->magic==magic && h->host; }
static void release(Handle *h) { free(h->host); h->host=NULL; h->magic=0; }

static ABI uint64_t wait_calc(uint32_t threads,uint32_t sync) { return 0x100+(uint64_t)threads*16+(uint64_t)sync*64; }
static ABI int32_t wait_create(Handle *h,const char *name,uint32_t threads,uint32_t sync,void *work,uint64_t flags,uint64_t attr) {
    (void)name; (void)work; (void)flags; (void)attr;
    if (!h) return ULT_INVALID;
    WaitPool *p=calloc(1,sizeof(*p)); if (!p) return ULT_INVALID;
    p->threads=threads; p->sync=sync; h->magic=MAGIC_WAIT; h->host=p; ++pools_created;
    return 0;
}
static ABI int32_t wait_destroy(Handle *h) {
    if (!valid(h,MAGIC_WAIT)) return ULT_INVALID;
    release(h); return 0;
}
static ABI uint64_t data_calc(uint32_t count,uint32_t size,uint32_t queues) {
    return 0x100+(uint64_t)count*size+(uint64_t)queues*64;
}
static ABI int32_t data_create(Handle *h,const char *name,uint32_t count,uint32_t size,uint32_t queues,Handle *wait,void *work,uint64_t flags,uint64_t attr) {
    (void)name; (void)queues; (void)work; (void)flags; (void)attr;
    if (!h || !valid(wait,MAGIC_WAIT) || !count || !size) return ULT_INVALID;
    DataPool *p=calloc(1,sizeof(*p)); if (!p) return ULT_INVALID;
    p->capacity=count; p->size=size; h->magic=MAGIC_DATA; h->host=p; ++pools_created;
    return 0;
}
static ABI int32_t data_destroy(Handle *h) {
    if (!valid(h,MAGIC_DATA)) return ULT_INVALID;
    release(h); return 0;
}
static ABI int32_t queue_create(Handle *h,const char *name,uint32_t size,Handle *wait,Handle *data,uint64_t flags,uint64_t attr) {
    (void)name; (void)flags; (void)attr;
    if (!h || !valid(wait,MAGIC_WAIT) || !valid(data,MAGIC_DATA)) return ULT_INVALID;
    DataPool *pool=data->host;
    if (!size || size>pool->size) return ULT_INVALID;
    Queue *q=calloc(1,sizeof(*q)); if (!q) return ULT_INVALID;
    q->slots=calloc(pool->capacity,size);
    if (!q->slots) { free(q); return ULT_INVALID; }
    pthread_mutex_init(&q->lock,NULL); pthread_cond_init(&q->ready,NULL);
    q->pool=pool; q->size=size; q->capacity=q->base_capacity=pool->capacity;
    h->magic=MAGIC_QUEUE; h->host=q; ++queues_created;
    PS4RT_LOADING_NOTE("queue-create host=%p guest=%p name=%s capacity=%u size=%u",
                       (void*)q, (void*)h, name ? name : "", q->capacity, q->size);
    return 0;
}
static ABI int32_t queue_destroy(Handle *h) {
    if (!valid(h,MAGIC_QUEUE)) return ULT_INVALID;
    Queue *q=h->host; h->host=NULL; h->magic=0;
    PS4RT_LOADING_NOTE("queue-destroy host=%p guest=%p", (void*)q, (void*)h);
    pthread_mutex_destroy(&q->lock); pthread_cond_destroy(&q->ready);
    free(q->slots); free(q); return 0;
}
/* Doubles a full queue's ring, keeping item order. The game's push wrapper (FUN_00a46ee0) only
 * retries on BUSY, so a push that succeeds is what it is waiting for anyway. Code Fairy's RMS
 * worker (RmsThread) needs this: the main thread pushes one wake token per request while the
 * worker reports idle, and on the host it can push all 32 before the worker is scheduled. If the
 * worker then spins on a load only the main thread's per-frame update can finish, a main thread
 * stuck in the BUSY retry deadlocks the game (PS4 priority scheduling runs the worker at once). */
#define ULT_GROW_LIMIT 64u
static int grow(Queue *q) {
    if (q->capacity >= q->base_capacity*ULT_GROW_LIMIT) return 0;
    uint32_t cap=q->capacity*2;
    unsigned char *slots=calloc(cap,q->size);
    if (!slots) return 0;
    for (uint32_t i=0;i<q->count;++i)
        memcpy(slots+(size_t)i*q->size,q->slots+(size_t)((q->head+i)%q->capacity)*q->size,q->size);
    free(q->slots); q->slots=slots; q->head=0; q->capacity=cap;
    PS4RT_LOADING_NOTE("queue-grow host=%p capacity=%u", (void*)q, cap);
    return 1;
}
static ABI int32_t queue_push(Handle *h,const void *data) {
    if (!valid(h,MAGIC_QUEUE) || !data) return ULT_INVALID;
    Queue *q=h->host; int32_t result=0;
    pthread_mutex_lock(&q->lock);
    if (q->count==q->capacity && !grow(q)) { result=ULT_BUSY; ++q->busy_streak; }
    else {
        q->busy_streak=0;
        memcpy(q->slots+(size_t)((q->head+q->count)%q->capacity)*q->size,data,q->size);
        ++q->count; ++pushes; pthread_cond_signal(&q->ready);
    }
    trace(TR_PUSH,q,result);
    PS4RT_LOADING_EVENT(PS4RT_LOAD_ULT_PUSH, q, result);
    /* The game's push wrapper (FUN_00a46ee0) sleeps 100 us and retries on BUSY; note only the
     * 1st, 2nd, 4th, 8th... failure of a streak so a long wait cannot fill the loading log. */
    if (result && !(q->busy_streak & (q->busy_streak-1)))
        PS4RT_LOADING_NOTE("queue-push-failed host=%p count=%u capacity=%u result=%x streak=%u",
                           (void*)q, q->count, q->capacity, (unsigned)result, q->busy_streak);
    pthread_mutex_unlock(&q->lock);
    return result;
}
static int32_t take(Handle *h,void *out,int wait) {
    if (!valid(h,MAGIC_QUEUE) || !out) return ULT_INVALID;
    Queue *q=h->host; int32_t result=0;
    pthread_mutex_lock(&q->lock);
    if (wait && !q->count) {
        trace(TR_POP_WAIT,q,0);
        PS4RT_LOADING_EVENT(PS4RT_LOAD_ULT_WAIT, q, 0);
    }
    while (!q->count && wait) pthread_cond_wait(&q->ready,&q->lock);
    if (!q->count) result=ULT_EMPTY;
    else {
        memcpy(out,q->slots+(size_t)q->head*q->size,q->size);
        q->head=(q->head+1)%q->capacity; --q->count; ++pops;
    }
    trace(wait?TR_POP_DONE:TR_TRY_POP,q,result);
    if (!result) PS4RT_LOADING_EVENT(PS4RT_LOAD_ULT_POP, q, 0);
    pthread_mutex_unlock(&q->lock);
    return result;
}
static ABI int32_t queue_pop(Handle *h,void *out) { return take(h,out,1); }
static ABI int32_t queue_try_pop(Handle *h,void *out) { return take(h,out,0); }

uintptr_t runtime_ult_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"9Y5keOvb6ok#libSceUlt",queue_create}, {"TFHm6-N6vks#libSceUlt",data_create},
        {"YiHujOG9vXY#libSceUlt",wait_create}, {"dh11uAUWNyM#libSceUlt",data_destroy},
        {"evj9YPkS8s4#libSceUlt",data_calc}, {"PP9nZxpSKLY#libSceUlt",queue_destroy},
        {"RVSq2tsm2yw#libSceUlt",queue_pop}, {"dUwpX3e5NDE#libSceUlt",queue_push},
        {"uZz3ci7XYqc#libSceUlt",queue_try_pop}, {"or55417wcDk#libSceUlt",wait_destroy},
        {"WIWV1Qd7PFU#libSceUlt",wait_calc},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_ult_report(void) {
    printf("Runtime: Ult pools=%lu, queues=%lu, pushes=%lu, pops=%lu\n",pools_created,queues_created,pushes,pops);
}
