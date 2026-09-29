# Synchronization Primitives

## POSIX Semaphores

POSIX semaphores are counting synchronization objects. A semaphore holds a non-negative integer value. Two operations are defined:

- `sem_wait`: Decrement. If the value is 0: block until it becomes positive.
- `sem_post`: Increment. Wake one blocked waiter (if any).

### Named Semaphores

```c
sem_t *sem = sem_open("/sem_name", O_CREAT, 0600, initial_value);
sem_wait(sem);   // acquire
sem_post(sem);   // release
sem_close(sem);
sem_unlink("/sem_name");
```

Named semaphores are backed by a kernel object in the semaphore namespace (process-shared). They are accessible by name from any process in the same IPC namespace.

### Unnamed (Process-Shared) Semaphores

```c
sem_t *sem = mmap(NULL, sizeof(sem_t), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
sem_init(sem, /*pshared=*/1, initial_value);
// share sem_t pointer via shared memory or fork
sem_wait(sem);
sem_post(sem);
sem_destroy(sem);
```

Placed in a shared memory region (or inherited via `fork`). The `sem_t` struct contains the count and a futex word (see below) — no separate kernel object.

### Kernel Implementation

`sem_wait` slow path:

1. Decrement `sem->count` atomically (CAS).
2. If the result ≥ 0: return immediately (fast path — no syscall).
3. If result < 0: call `sys_futex(FUTEX_WAIT, &sem->futex_word, ...)` to block.

`sem_post` slow path:

1. Increment `sem->count` atomically.
2. If there were waiters (`count` was negative before increment): call `sys_futex(FUTEX_WAKE, ...)` to wake one waiter.

## Futex (Fast Userspace Mutex)

The futex syscall is the primitive underlying all higher-level synchronization in libc (pthread_mutex, sem_t, pthread_cond_t).

### Futex Operations

```c
int sys_futex(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3);
```

| `op` | Description |
|---|---|
| `FUTEX_WAIT` | If `*uaddr == val`: sleep. The atomic check prevents lost wakeup. |
| `FUTEX_WAKE` | Wake up to `val` threads sleeping on `uaddr`. |
| `FUTEX_PRIVATE_FLAG` | Optimization for process-private futexes (no hash sharing). |

### Kernel Futex Table

The kernel maintains a global hash table of futex wait queues, indexed by the physical page and offset of the futex word:

- This allows `FUTEX_WAKE` to find sleeping threads from a different process mapping the same page (for process-shared futexes in shared memory).
- For private futexes (`FUTEX_PRIVATE_FLAG`), the key is the virtual address within the calling process — faster hash lookup.

### pthread_mutex Implementation

A `pthread_mutex_t` contains one `uint32_t` futex word (the lock state):

- `0`: unlocked.
- `1`: locked, no waiters.
- `2`: locked, waiters present.

```c
void pthread_mutex_lock(pthread_mutex_t *m) {
    if (atomic_cas(&m->state, 0, 1) == 0) return;  // fast path: no syscall
    while (atomic_xchg(&m->state, 2) != 0)
        sys_futex(&m->state, FUTEX_WAIT, 2, NULL, NULL, 0);
}
void pthread_mutex_unlock(pthread_mutex_t *m) {
    if (atomic_fetch_sub(&m->state, 1) != 1) {     // had waiters
        m->state = 0;
        sys_futex(&m->state, FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}
```

Uncontended acquire/release: ~2 ns (CAS only, no syscall).

## Related Documents

- [shared-memory.md](shared-memory.md)
- [ipc/overview.md](overview.md)
- [userspace/libc-threading.md](../userspace/libc-threading.md)
- [syscalls/ipc-syscalls.md](../syscalls/ipc-syscalls.md)
