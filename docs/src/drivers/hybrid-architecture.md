# Hybrid Driver Architecture

## Design Rationale

Traditional monolithic kernels run all device drivers in ring-0. A bug in any driver can corrupt kernel state and crash the system. Microkernels run drivers in ring-3, but the per-operation context switching cost is prohibitive for high-throughput devices (e.g., NVMe SSDs at 1M IOPS).

The hybrid model splits the driver at a carefully chosen boundary:

- **Ring-0**: Only the code that must run in ring-0 (interrupt acknowledgment, DMA configuration, MMIO access) stays in the kernel.
- **Ring-3**: Everything else (protocol state machine, request parsing, completion processing) runs in a userspace process.

Communication between the two is via **shared memory ring buffers** — after initial setup, the hot path requires no syscalls on the kernel side.

## Shared Memory Protocol

The kernel allocates two ring buffers in a shared memory region:

```c
struct driver_shared_region {
    struct ring_buf cmd_queue;    // kernel → userspace: commands/events
    struct ring_buf rsp_queue;    // userspace → kernel: completions/results
    uint64_t        doorbell;     // userspace writes non-zero to kick kernel
    uint64_t        irq_count;    // kernel increments on interrupt; userspace reads
    uint32_t        flags;        // DRIVER_FLAG_ALIVE | DRIVER_FLAG_ERROR
    uint8_t         data[0];      // device-specific data follows
};
```

The shared region is mmap'd into the userspace driver's address space at initialization.

## Hot Path (Storage Example)

1. Userspace process calls `submit_bio(bio)`.
2. Bio parameters are written into `cmd_queue` (ring buffer append — no syscall).
3. Userspace writes to `doorbell` word (shared memory write — visible to kernel via poll).
4. Kernel reads `cmd_queue`, programs DMA, submits to device.
5. Device completes, IRQ fires → kernel ISR runs.
6. Kernel writes completion to `rsp_queue`. Increments `irq_count`.
7. Userspace detects new `irq_count` (polling or futex wait) and processes completion.

Steps 2–3 and 5–7 involve no kernel-level locks. Steps 4–5 are kernel-side only.

## Event-Driven Userspace Driver

Userspace drivers run an event loop:

```c
while (1) {
    // Wait for events (interrupt, timeout, or doorbell from kernel)
    sys_futex(&shared->irq_count, FUTEX_WAIT, last_irq_count, NULL, NULL, 0);
    
    // Drain completion queue
    while (rsp_queue_has_entry(&shared->rsp_queue)) {
        struct driver_completion comp;
        rsp_queue_dequeue(&shared->rsp_queue, &comp);
        handle_completion(&comp);
    }
    
    // Submit pending commands if space available
    while (!cmd_queue_full(&shared->cmd_queue) && has_pending_work()) {
        struct driver_cmd cmd = build_next_cmd();
        cmd_queue_enqueue(&shared->cmd_queue, &cmd);
    }
    write_doorbell(&shared->doorbell);
}
```

## Isolation Boundary

The userspace driver runs as a dedicated process with a minimal capability set:

- `CAP_IPC_LOCK` (to lock shared memory pages).
- Specific device file access (e.g., `/dev/driver_ctl`).
- No other capabilities.

A crash of the userspace driver process does not affect the kernel or other processes. The kernel detects driver process death (via `EPIPE` on the shared region fd or process exit notification) and activates a stub that returns errors to callers.

## Related Documents

- [kernel-drivers.md](kernel-drivers.md)
- [userspace-drivers.md](userspace-drivers.md)
- [driver-communication.md](driver-communication.md)
- [driver-isolation.md](driver-isolation.md)
- [ipc/shared-memory.md](../ipc/shared-memory.md)
