# Mutex / deadlock review for the Windows port

Scope: src/runtime_mutex.c, runtime_rwlock.c, runtime_sema.c (static read), runtime_ult.c, runtime_model_wait.c.
Status: static review only. NOT run on real Windows.

## What the runtime does
Guest (PS4) mutex/cond/rwlock handles are heap objects wrapping pthread primitives. On Windows
these resolve to MinGW-w64 **winpthreads**, not glibc, so semantics that were implicit on Linux
were checked.

## Findings
1. **Timer resolution (fixed).** Windows wakes blocked threads on a 15.6 ms tick by default.
   The known game deadlock (runtime_model_wait.c: main thread takes the model-manager lock
   before the manager thread, both wait forever) is a *wake-up race*; the file's own comment says
   slow wake-ups make it more likely. Windows is slower than Linux here, so the race would be
   hit more often. Fix: `timeBeginPeriod(1)` constructor in compat_win.c. The lock-order fix
   in runtime_model_wait.c (reply wait moved before lock) is pure code patching on guest memory
   and is OS independent, so it still applies.
2. **Recursive/errorcheck mutex types.** winpthreads implements PTHREAD_MUTEX_RECURSIVE and
   ERRORCHECK (EDEADLK on relock, EPERM on foreign unlock), matching glibc. Static-init guest
   handles 0/1 map to PTHREAD_RECURSIVE_MUTEX_INITIALIZER via a macro in compat_win.h.
3. **Timed waits.** pthread_mutex_timedlock / pthread_cond_timedwait take absolute
   CLOCK_REALTIME deadlines on winpthreads too. OK. Resolution is bounded by item 1.
4. **Destroy of a locked mutex.** glibc aborts on kind -1 after destroy (the quarantine of
   16384 destroyed mutexes exists for that). winpthreads returns EBUSY/EINVAL instead, which
   the wrapper maps to guest errors. Not a deadlock source; may differ in diagnostics.
5. **rwlocks.** winpthreads rwlocks differ from glibc (writer preference, no recursion guarantees),
   but runtime_rwlock.c tracks per-thread ownership itself and returns EDEADLK/EBUSY *before*
   touching the native lock, so same-thread read->write upgrades cannot hang. Residual risk:
   a writer waiting while readers recurse (glibc default prefers readers, winpthreads may
   not). If a hang shows up in a rwlock, that is the first suspect.
6. **ULT queues (runtime_ult.c).** Plain mutex+cond with spurious-wakeup-safe loops; queue
   growth patch avoids the BUSY-retry deadlock. No Windows-specific issue found.
7. **Thread priorities.** The runtime never sets host thread priorities (grep: none), so no
   priority-inversion risk was introduced; the PS4 priority scheduling the comments mention is
   not emulated on either OS.

## If it hangs on Windows
Run with `Play-Game.bat --game ... ` from a console, let it hang, and note: last console line,
whether CPU is idle (lock deadlock) or spinning (BUSY retry loop), and the loading note
lines starting `queue-`. Dumping all thread stacks with Process Explorer / procdump helps.
