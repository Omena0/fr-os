# Userspace Driver Model

## Overview

Userspace drivers are ordinary ring-3 processes that implement device-specific logic. They communicate with the kernel driver core via shared memory ring buffers and receive device events via a file descriptor.

## Driver Process Lifecycle

1. **Spawned by kernel**: The kernel `probe` function launches the driver process via `kernel_exec("/usr/lib/drivers/<name>", argv, envp)`.
2. **Initialization**: The driver process opens its device control fd (`/dev/driver_ctl_<id>`), calls `ioctl(ctl_fd, DRIVER_INIT, &params)` to receive the shared memory fd.
3. **Maps shared region**: `mmap(NULL, size, PROT_RW, MAP_SHARED, shm_fd, 0)`.
4. **Event loop**: Processes commands from `cmd_queue`, writes completions to `rsp_queue`.
5. **Shutdown**: On `SIGTERM`, drains in-progress requests, writes completion entries, calls `ioctl(ctl_fd, DRIVER_STOP, 0)`, exits cleanly.

## Device Control File Descriptor

The kernel exposes a per-driver control fd at `/dev/driver_ctl_<device_id>`:

```c
// Available ioctl commands:
#define DRIVER_INIT   0x4400   // Initialize driver; returns shm_fd
#define DRIVER_STOP   0x4401   // Driver is shutting down
#define DRIVER_PANIC  0x4402   // Driver is in error state; kernel activates stub
```

The `DRIVER_INIT` ioctl returns:

```c
struct driver_init_params {
    int      shm_fd;           // fd for the shared memory region
    size_t   shm_size;         // size of the shared memory region
    uint64_t mmio_phys_base;   // physical base of device MMIO (if applicable)
    size_t   mmio_size;        // MMIO region size
    uint8_t  irq_vector;       // IRQ vector assigned to this device
};
```

## Shared Memory Layout

```
+──────────────────────────────────────────────────────+
│  struct driver_shared_region (header)                │
│    cmd_queue (ring buffer): kernel → driver          │
│    rsp_queue (ring buffer): driver → kernel          │
│    doorbell, irq_count, flags                        │
+──────────────────────────────────────────────────────+
│  Device-specific data area (DMA buffers, etc.)       │
+──────────────────────────────────────────────────────+
```

## Capability Set

At launch, the kernel configures the driver process's capabilities to a minimal set:

- `CAP_IPC_LOCK`: lock shared memory pages in RAM (prevent swap).
- Device-specific (e.g., `CAP_NET_ADMIN` for network drivers).
- All others: cleared.

## Example Driver: Block Device

```c
int main(int argc, char *argv[]) {
    int ctl_fd = open("/dev/driver_ctl_0", O_RDWR);
    
    struct driver_init_params params;
    ioctl(ctl_fd, DRIVER_INIT, &params);
    
    struct driver_shared_region *shm =
        mmap(NULL, params.shm_size, PROT_READ | PROT_WRITE,
             MAP_SHARED, params.shm_fd, 0);
    close(params.shm_fd);
    
    uint64_t last_irq = shm->irq_count;
    while (1) {
        // Wait for kernel to notify us of a new command or IRQ
        sys_futex(&shm->irq_count, FUTEX_WAIT, last_irq, NULL, NULL, 0);
        last_irq = shm->irq_count;
        
        // Process commands
        struct driver_cmd cmd;
        while (cmd_queue_dequeue(&shm->cmd_queue, &cmd)) {
            process_block_cmd(&cmd, shm);
        }
    }
}
```

## Driver Restart

If the userspace driver process crashes:

1. The kernel detects process exit (via the process monitoring mechanism).
2. The kernel activates a stub driver that returns `-EIO` to all pending requests.
3. The kernel re-spawns the driver process.
4. The new process re-initializes, reinitializes the device hardware, and resumes processing.
5. Pending requests from before the crash are failed (not retried).

## Related Documents

- [hybrid-architecture.md](hybrid-architecture.md)
- [kernel-drivers.md](kernel-drivers.md)
- [driver-communication.md](driver-communication.md)
- [driver-isolation.md](driver-isolation.md)
- [ipc/shared-memory.md](../ipc/shared-memory.md)
