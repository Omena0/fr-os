#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

#include <types.h>
#include <task.h>

long futex_wait_user(u64 address, u32 expected);
long futex_wake_user(u64 address, u32 count);
void futex_cancel_task(struct task *task);
void futex_clear_child_tid(struct task *task);

#endif
