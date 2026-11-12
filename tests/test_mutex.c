#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef int32_t (ABI *Init)(void *,void *,const char *);
typedef int32_t (ABI *Op)(void *);
#define GET(t,n) ((t)runtime_mutex_resolve(n))
#define EINVAL_ORBIS ((int32_t)0x80020016)
#define EBUSY_ORBIS ((int32_t)0x80020010)
int main(void) {
    Init init=GET(Init,"cmo1RIYva9o#libkernel");
    Op lock=GET(Op,"9UK1vLZQft4#libkernel"), unlock=GET(Op,"tn3VlD0hG60#libkernel");
    Op destroy=GET(Op,"2Of0f+3mhhE#libkernel");
    assert(init && lock && unlock && destroy);
    void *handle=NULL;
    assert(init(&handle,NULL,"t")==0 && handle>(void *)2);
    assert(lock(&handle)==0 && unlock(&handle)==0);
    /* A held mutex cannot be destroyed. */
    assert(lock(&handle)==0);
    assert(destroy(&handle)==EBUSY_ORBIS && handle>(void *)2);
    assert(unlock(&handle)==0);
    /* Another thread's copy of the handle, taken before the destroy (the crash scenario:
     * glibc aborted in __pthread_tpp_change_priority when this lock reached a destroyed mutex). */
    void *stale=handle;
    assert(destroy(&handle)==0 && handle==(void *)2);
    assert(lock(&stale)==EINVAL_ORBIS && unlock(&stale)==EINVAL_ORBIS && destroy(&stale)==EINVAL_ORBIS);
    assert(lock(&handle)==EINVAL_ORBIS);
    /* Statically initialized handles still work, even after many destroys recycle the quarantine. */
    for (int i=0;i<40000;++i) { void *h=NULL; assert(init(&h,NULL,"q")==0 && destroy(&h)==0); }
    void *fixed=NULL;
    assert(lock(&fixed)==0 && unlock(&fixed)==0 && destroy(&fixed)==0);
    puts("mutex ok");
    return 0;
}
