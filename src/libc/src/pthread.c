#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <uapi/syscall.h>
#include <unistd.h>
#define DEFAULT_STACK_SIZE (1024u * 1024u)
#define TLS_ALIGNMENT      64u

struct thread_control {
    volatile uint32_t tid;
    pid_t kernel_tid;
    void *(*start_routine)(void *);
    void *argument;
    void *result;
    void *stack;
    size_t stack_size;
    void *tls_region;
    size_t tls_region_size;
};

static __thread struct thread_control *current_thread;

extern long __syscall0(long number);
extern long __syscall2(long number, long a1, long a2);
extern long __syscall6(long number, long a1, long a2, long a3, long a4,
                       long a5, long a6);
extern long sys_arch_prctl(unsigned long code, unsigned long address);

static int futex_wait_word(volatile uint32_t *word, uint32_t expected)
{
    long ret = __syscall6(SYS_futex, (long)word, FUTEX_WAIT_PRIVATE,
                          expected, 0, 0, 0);

    return ret < 0 ? (int)-ret : 0;
}

static int futex_wake_word(volatile uint32_t *word, uint32_t count)
{
    long ret = __syscall6(SYS_futex, (long)word, FUTEX_WAKE_PRIVATE,
                          count, 0, 0, 0);

    return ret < 0 ? (int)-ret : 0;
}

static __attribute__((noreturn)) void *thread_trampoline(void *argument)
{
    struct thread_control *control = argument;

    current_thread = control;
    control->result = control->start_routine(control->argument);
    _exit(0);
}

int pthread_attr_init(pthread_attr_t *attr)
{
    if (!attr)
        return EINVAL;
    attr->stack_size = DEFAULT_STACK_SIZE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    return attr ? 0 : EINVAL;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stack_size)
{
    if (!attr || stack_size < PTHREAD_STACK_MIN)
        return EINVAL;
    attr->stack_size = stack_size;
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *argument)
{
    struct thread_control *control;
    size_t stack_size = attr ? attr->stack_size : DEFAULT_STACK_SIZE;
    size_t page_size = (size_t)getpagesize();
    size_t tls_size;
    uintptr_t parent_tp = 0;
    uintptr_t child_tp = 0;
    long ret;

    if (!thread || !start_routine || stack_size < PTHREAD_STACK_MIN || !page_size)
        return EINVAL;
    if (stack_size > SIZE_MAX - (page_size - 1))
        return EINVAL;
    stack_size = (stack_size + page_size - 1) & ~(page_size - 1);

    control = calloc(1, sizeof(*control));
    if (!control)
        return ENOMEM;
    control->start_routine = start_routine;
    control->argument = argument;
    control->stack_size = stack_size;

    control->stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (control->stack == MAP_FAILED) {
        free(control);
        return errno ? errno : ENOMEM;
    }

    ret = __syscall0(SYS_get_tls_size);
    if (ret < 0) {
        munmap(control->stack, control->stack_size);
        free(control);
        return (int)-ret;
    }
    tls_size = (size_t)ret;
    if (tls_size) {
        long tp_ret = sys_arch_prctl(ARCH_GET_FS, (unsigned long)&parent_tp);

        if (tp_ret < 0 || !parent_tp || tls_size > SIZE_MAX - TLS_ALIGNMENT) {
            munmap(control->stack, control->stack_size);
            free(control);
            return tp_ret < 0 ? (int)-tp_ret : EINVAL;
        }
        control->tls_region_size = tls_size + TLS_ALIGNMENT;
        control->tls_region = mmap(NULL, control->tls_region_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (control->tls_region == MAP_FAILED) {
            munmap(control->stack, control->stack_size);
            free(control);
            return errno ? errno : ENOMEM;
        }
        child_tp = ((uintptr_t)control->tls_region + tls_size + TLS_ALIGNMENT - 1)
                   & ~(uintptr_t)(TLS_ALIGNMENT - 1);
        memcpy((void *)(child_tp - tls_size),
               (const void *)(parent_tp - tls_size), tls_size);
    }

    uintptr_t stack_top = (uintptr_t)control->stack + stack_size;
    ret = __syscall6(SYS_thread_create, (long)thread_trampoline,
                     (long)control, (long)stack_top, (long)child_tp,
                     (long)&control->tid, (long)&control->tid);
    if (ret < 0) {
        if (control->tls_region)
            munmap(control->tls_region, control->tls_region_size);
        munmap(control->stack, control->stack_size);
        free(control);
        return (int)-ret;
    }

    control->kernel_tid = (pid_t)ret;
    *thread = (pthread_t)(uintptr_t)control;
    return 0;
}

int pthread_join(pthread_t thread, void **result)
{
    struct thread_control *control =
        (struct thread_control *)(uintptr_t)thread;
    uint32_t tid;
    int status;

    if (!control || thread < 65536u)
        return EINVAL;

    while ((tid = __atomic_load_n(&control->tid, __ATOMIC_ACQUIRE)) != 0) {
        int err = futex_wait_word(&control->tid, tid);

        if (err && err != EAGAIN && err != EINTR)
            return err;
    }

    if (waitpid(control->kernel_tid, &status, 0) < 0)
        return errno ? errno : ESRCH;
    if (result)
        *result = control->result;
    if (control->tls_region)
        munmap(control->tls_region, control->tls_region_size);
    munmap(control->stack, control->stack_size);
    free(control);
    return 0;
}

pthread_t pthread_self(void)
{
    if (current_thread)
        return (pthread_t)(uintptr_t)current_thread;
    return (pthread_t)(uintptr_t)gettid();
}

int pthread_equal(pthread_t left, pthread_t right)
{
    return left == right;
}

void pthread_exit(void *result)
{
    if (current_thread)
        current_thread->result = result;
    _exit(0);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr)
{
    (void)attr;
    if (!mutex)
        return EINVAL;
    __atomic_store_n(&mutex->state, 0, __ATOMIC_RELAXED);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if (!mutex)
        return EINVAL;
    return __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (!mutex)
        return EINVAL;

    for (;;) {
        uint32_t state = __atomic_load_n(&mutex->state, __ATOMIC_RELAXED);

        if (state == 0) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&mutex->state, &expected, 1, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return 0;
            continue;
        }

        if (state == 1) {
            uint32_t expected = 1;
            if (!__atomic_compare_exchange_n(&mutex->state, &expected, 2, false,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                continue;
        }
        if (state > 2)
            return EINVAL;

        {
            int err = futex_wait_word(&mutex->state, 2);
            if (err && err != EAGAIN && err != EINTR)
                return err;
        }
    }
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    uint32_t old;

    if (!mutex)
        return EINVAL;
    old = __atomic_exchange_n(&mutex->state, 0, __ATOMIC_RELEASE);
    if (!old)
        return EPERM;
    if (old == 2)
        return futex_wake_word(&mutex->state, 1);
    return 0;
}

int pthread_cond_init(pthread_cond_t *condition, const void *attr)
{
    (void)attr;
    if (!condition)
        return EINVAL;
    __atomic_store_n(&condition->sequence, 0, __ATOMIC_RELAXED);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
    return condition ? 0 : EINVAL;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    uint32_t sequence;
    int err;

    if (!condition || !mutex)
        return EINVAL;
    sequence = __atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE);
    err = pthread_mutex_unlock(mutex);
    if (err)
        return err;
    err = futex_wait_word(&condition->sequence, sequence);
    if (err == EAGAIN || err == EINTR)
        err = 0;
    int lock_err = pthread_mutex_lock(mutex);
    return err ? err : lock_err;
}

int pthread_cond_signal(pthread_cond_t *condition)
{
    if (!condition)
        return EINVAL;
    __atomic_add_fetch(&condition->sequence, 1, __ATOMIC_RELEASE);
    return futex_wake_word(&condition->sequence, 1);
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
    if (!condition)
        return EINVAL;
    __atomic_add_fetch(&condition->sequence, 1, __ATOMIC_RELEASE);
    return futex_wake_word(&condition->sequence, UINT32_MAX);
}
