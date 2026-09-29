# Kernel Libraries

## Overview

The kernel maintains an internal utility library (`lib/`) providing common data structures, synchronization primitives, string utilities, and memory helpers used throughout the kernel. This library is not exposed to userspace.

## Memory Utilities

### `kmalloc` / `kfree`

The primary kernel allocation API, backed by the SLAB allocator for small objects and `vmalloc` for large:

```c
void *kmalloc(size_t size, gfp_t flags);
void  kfree(void *ptr);
void *kzalloc(size_t size, gfp_t flags);  // zero-initialized
void *krealloc(void *ptr, size_t size, gfp_t flags);
```

`gfp_t` flags control allocation behavior:

- `GFP_KERNEL`: May sleep; used in process context.
- `GFP_ATOMIC`: May not sleep; used in interrupt context.
- `GFP_ZERO`: Zero the allocation.
- `GFP_DMA`: Allocate from DMA-capable memory zone.

### `copy_from_user` / `copy_to_user`

Safe memory access to userspace buffers:

```c
int copy_from_user(void *dst, const void __user *src, size_t len);
int copy_to_user(void __user *dst, const void *src, size_t len);
```

These functions validate the userspace pointer range, temporarily enable SMAP access (`STAC`/`CLAC`), and handle page faults during the copy. Return 0 on success, number of bytes not copied on partial failure.

## String Utilities

```c
size_t kstrlen(const char *s);
char  *kstrdup(const char *s, gfp_t flags);
int    kstrcmp(const char *a, const char *b);
int    kstrncmp(const char *a, const char *b, size_t n);
char  *kstrcpy(char *dst, const char *src);
char  *kstrncpy(char *dst, const char *src, size_t n);
int    ksnprintf(char *buf, size_t size, const char *fmt, ...);
```

`ksnprintf` is a minimal printf-like formatter supporting `%d`, `%u`, `%x`, `%s`, `%p`, `%zu`. It does not support floating-point.

## Synchronization Primitives

### Spinlock

```c
typedef struct { atomic_t val; } spinlock_t;
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
void spin_lock_irqsave(spinlock_t *l, unsigned long *flags);
void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags);
```

Spinlocks disable preemption. The `irqsave` variant also disables interrupts and saves the RFLAGS.IF state.

### Mutex

```c
typedef struct { ... } mutex_t;
void mutex_lock(mutex_t *m);     // may sleep
void mutex_unlock(mutex_t *m);
int  mutex_trylock(mutex_t *m);  // non-blocking
```

Mutexes allow the current thread to sleep while waiting. Only usable in process context (not in interrupt handlers).

### RW Lock

```c
typedef struct { ... } rwlock_t;
void read_lock(rwlock_t *l);
void read_unlock(rwlock_t *l);
void write_lock(rwlock_t *l);
void write_unlock(rwlock_t *l);
```

Multiple concurrent readers; exclusive writer.

### Completion

```c
typedef struct { ... } completion_t;
void init_completion(completion_t *c);
void complete(completion_t *c);        // signal
void wait_for_completion(completion_t *c); // block until signaled
```

Used to synchronize initialization between producer and consumer threads.

### Atomic Operations

```c
typedef struct { int32_t val; } atomic_t;
int  atomic_read(const atomic_t *a);
void atomic_set(atomic_t *a, int v);
int  atomic_add_return(atomic_t *a, int v);
int  atomic_sub_return(atomic_t *a, int v);
bool atomic_cmpxchg(atomic_t *a, int old, int new);
int  atomic_fetch_and(atomic_t *a, int mask);
int  atomic_fetch_or(atomic_t *a, int mask);
```

Implemented using x86 `LOCK` prefix instructions or `CMPXCHG`.

## Linked Lists

Intrusive doubly-linked list (embed `struct list_head` in your struct):

```c
struct list_head { struct list_head *next, *prev; };
void list_add(struct list_head *new, struct list_head *head);
void list_add_tail(struct list_head *new, struct list_head *head);
void list_del(struct list_head *entry);
bool list_empty(const struct list_head *head);
#define list_for_each(pos, head) ...
#define list_entry(ptr, type, member) container_of(ptr, type, member)
```

## Hash Tables

```c
struct hlist_head { struct hlist_node *first; };
struct hlist_node { struct hlist_node *next, **pprev; };
```

Used throughout the kernel for per-bucket chaining (PID table, inode table, socket table).

## Related Documents

- [memory/slab-allocator.md](../memory/slab-allocator.md)
- [memory/virtual-memory.md](../memory/virtual-memory.md)
- [kernel/overview.md](overview.md)
