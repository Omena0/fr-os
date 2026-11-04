#include <stdbool.h>
#include <types.h>
#include <list.h>
#include <spinlock.h>
#include <task.h>
#include <sched.h>
#include <process.h>
#include <futex.h>
#include <uapi/syscall.h>
#include <uapi/errno.h>

static spinlock_t futex_lock = SPINLOCK_INIT;
static struct list_head futex_waiters = LIST_HEAD_INIT(futex_waiters);

static bool futex_address_valid(struct address_space *mm, u64 address)
{
    return !(address & (sizeof(u32) - 1)) &&
           user_range_ok(mm, address, sizeof(u32));
}

static long futex_wake_key(struct address_space *mm, u64 address, u32 count)
{
    long woken = 0;

    if (!count)
        return 0;

    u64 flags = spinlock_irqsave(&futex_lock);
    struct list_head *pos, *next;

    list_for_each_safe(pos, next, &futex_waiters) {
        struct task *task = list_entry(pos, struct task, futex_node);

        if (task->futex_mm != mm || task->futex_address != address)
            continue;
        list_del(&task->futex_node);
        list_init(&task->futex_node);
        task->futex_waiting = false;
        task->futex_mm = NULL;
        task->futex_address = 0;
        sched_wake(task);
        if (++woken >= count)
            break;
    }

    spinlock_unlock_irqrestore(&futex_lock, flags);
    return woken;
}

long futex_wait_user(u64 address, u32 expected)
{
    struct task *task = current_task();
    u32 value;
    u64 flags;

    if (!task || !task->mm)
        return -ESRCH;
    if (address & (sizeof(u32) - 1))
        return -EINVAL;
    if (!user_range_ok(task->mm, address, sizeof(value)))
        return -EFAULT;
    if (copy_from_user(&value, address, sizeof(value)) < 0)
        return -EFAULT;
    if (value != expected)
        return -EAGAIN;

    flags = spinlock_irqsave(&futex_lock);
    /* Recheck under the queue lock so a wake cannot slip between compare/add. */
    if (copy_from_user(&value, address, sizeof(value)) < 0) {
        spinlock_unlock_irqrestore(&futex_lock, flags);
        return -EFAULT;
    }
    if (value != expected) {
        spinlock_unlock_irqrestore(&futex_lock, flags);
        return -EAGAIN;
    }

    task->futex_mm = task->mm;
    task->futex_address = address;
    task->futex_waiting = true;
    list_add_tail(&task->futex_node, &futex_waiters);
    task->state = TASK_BLOCKED;
    sched_remove(task);
    spinlock_unlock_irqrestore(&futex_lock, flags);

    schedule();
    return 0;
}

long futex_wake_user(u64 address, u32 count)
{
    struct task *task = current_task();

    if (!task || !task->mm)
        return -ESRCH;
    if (!futex_address_valid(task->mm, address))
        return -EFAULT;
    return futex_wake_key(task->mm, address, count);
}

void futex_cancel_task(struct task *task)
{
    u64 flags;

    if (!task || !task->futex_waiting)
        return;

    flags = spinlock_irqsave(&futex_lock);
    if (task->futex_waiting) {
        list_del(&task->futex_node);
        list_init(&task->futex_node);
        task->futex_waiting = false;
        task->futex_mm = NULL;
        task->futex_address = 0;
    }
    spinlock_unlock_irqrestore(&futex_lock, flags);
}

void futex_clear_child_tid(struct task *task)
{
    u32 zero = 0;
    u64 address;

    if (!task || !task->clear_child_tid || !task->mm)
        return;

    address = task->clear_child_tid;
    task->clear_child_tid = 0;
    if (user_memory_write(task->mm, address, &zero, sizeof(zero),
                          VM_READ | VM_WRITE | VM_USER) < 0)
        return;

    (void)futex_wake_key(task->mm, address, 1);
}
