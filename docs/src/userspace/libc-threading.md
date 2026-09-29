# POSIX Threads (libpthread)

## Overview

The thread library implements POSIX threads (pthreads) on top of the kernel's `sys_clone` + `sys_futex` primitives. Applications link against `libpthread.so.1` (or it may be integrated into `libc.so.1`).

## `pthread_create`

```c
int pthread_create(pthread_t *tid, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
```

Implementation:
1. Allocate a thread stack (via `mmap(MAP_ANONYMOUS | MAP_GROWSDOWN)`, default 8 MB).
2. Allocate a TLS block for the new thread.
3. Call `sys_clone(CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SETTLS, stack_top, NULL, &child_tid, tls_base)`.
4. The child starts at the trampoline:
   ```c
   void thread_trampoline(void *arg) {
       void *result = start_routine(arg);
       pthread_exit(result);
   }
   ```
5. Store the TID in `*tid`. Return 0.

## Mutex

```c
typedef struct {
    uint32_t  state;    // 0=unlocked, 1=locked-no-waiters, 2=locked-with-waiters
    int       type;     // PTHREAD_MUTEX_NORMAL, RECURSIVE, ERRORCHECK
    uint32_t  owner;    // TID of owner (for RECURSIVE type)
    uint32_t  count;    // recursion count (for RECURSIVE type)
} pthread_mutex_t;
```

`pthread_mutex_lock`:
```
CAS(state, 0, 1) → success: return (fast path, no syscall)
CAS(state, 1, 2) → exchange: futex_wait(state, 2)
On wake: check state again (spurious wakeup guard)
```

`pthread_mutex_unlock`:
```
if ATOMIC_XCHG(state, 0) == 2:
    futex_wake(state, 1)  // wake one waiter
```

## Condition Variable

```c
typedef struct {
    uint32_t seq;    // sequence number (incremented on signal/broadcast)
    uint32_t mutex;  // address of associated mutex (used by condvar broadcast requeue)
} pthread_cond_t;
```

`pthread_cond_wait`:
1. Record current `seq`.
2. Release the mutex.
3. `futex_wait(seq, old_seq)` — sleep until seq changes.
4. Reacquire the mutex.

`pthread_cond_signal`: increment `seq`, `futex_wake(seq, 1)`.
`pthread_cond_broadcast`: increment `seq`, `futex_requeue(seq, INT_MAX, mutex)`.

## `pthread_join`

Wait for a thread to terminate:
1. Spin/sleep on the thread's `tid` word (the kernel zeroes this word via CLONE_CHILD_CLEARTID on thread exit).
2. `futex_wait(tid, tid_value)` → kernel wakes us when the TID word becomes 0.
3. Retrieve the thread's `result` (stored in a shared per-thread control block).

## Thread-Local Storage (TLS)

Each thread has a TLS block allocated when the thread is created. The `FS` segment register base (`FSBASE`) points to the TLS block. The kernel sets `FSBASE` on each context switch.

Access pattern:
```c
// Accessing a thread-local variable (compiler generates):
mov rax, QWORD PTR fs:[thread_var@TPOFF]
```

`errno` is a thread-local variable in `libc`:
```c
// In libc:
__thread int __errno_location_val;
int *__errno_location(void) { return &__errno_location_val; }
#define errno (*__errno_location())
```

## Thread Cancellation

`pthread_cancel(tid)`: sends a cancellation request to the thread. The thread must reach a **cancellation point** (e.g., `read`, `sleep`, `pthread_cond_wait`) to be cancelled.

Implementation: the cancellation is delivered by setting a flag in the thread's control block. At each cancellation point, the flag is checked; if set, the thread calls `pthread_exit(PTHREAD_CANCELED)`.

## Related Documents

- [libc.md](libc.md)
- [ipc/synchronization-primitives.md](../ipc/synchronization-primitives.md)
- [syscalls/ipc-syscalls.md](../syscalls/ipc-syscalls.md)
- [scheduling/multicore-overview.md](../scheduling/multicore-overview.md)
