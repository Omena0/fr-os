# Testing

## Overview

The test suite is divided into two tiers:
- **Unit tests**: test individual kernel components in host userspace (no QEMU needed, fast iteration).
- **Integration tests**: boot the OS under QEMU and run test programs in the guest.

## Unit Tests

Unit tests compile kernel subsystems as host binaries (replacing kernel-specific primitives with userspace equivalents). This allows fast iteration without booting QEMU.

### Directory Structure

```
tests/
  unit/
    buddy/
      test_alloc.c          — allocate/free pages, check buddy coalescing
      test_numa.c           — NUMA zone allocation
    slab/
      test_slab.c           — SLAB allocation, per-CPU caches
    scheduler/
      test_mlfq.c           — MLFQ queue operations and priority aging
      test_rt.c             — RT scheduler ordering
    vfs/
      test_mount.c          — mount/unmount operations
      test_dentry.c         — dentry cache lookups
    ipc/
      test_pipe.c           — pipe read/write, blocking behavior
      test_futex.c          — futex wait/wake ordering
    security/
      test_capabilities.c   — cap checks, drop, inherit
      test_seccomp.c        — filter installation and enforcement
```

### Running Unit Tests

```sh
make test
```

Output (using a minimal test framework):
```
[PASS] buddy: alloc order 0
[PASS] buddy: alloc order 10
[PASS] buddy: coalesce after free
[FAIL] slab: per-cpu magazine drain — expected 0 allocs, got 1
```

Exit code: 0 if all pass, 1 if any fail.

### Test Framework

The unit test framework is a small single-header C library (`tests/framework.h`):
```c
#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        fprintf(stderr, "[FAIL] %s:%d: %s == %s: %lld != %lld\n", \
                __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); \
        exit(1); \
    }

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    fprintf(stdout, "[RUN ] " #name "\n"); \
    test_##name(); \
    fprintf(stdout, "[PASS] " #name "\n"); \
} while(0)
```

## Integration Tests

Integration tests boot the OS in QEMU and execute a test binary in the guest:

```sh
make check
```

1. Build the OS (`make all`).
2. Launch QEMU (headless: `-display none -serial pipe:/tmp/serial`).
3. Wait for the OS to boot (detect boot-complete marker on serial: `[BOOT] init: ready`).
4. Mount a host-shared virtio-fs or inject test binaries via initrd.
5. Run test programs in the guest; they write results to serial.
6. Parse serial output for `[PASS]` / `[FAIL]` markers.
7. Kill QEMU. Report results.

### Integration Test Cases

| Test | Validates |
|---|---|
| `test_fork_exec` | fork + exec + wait; exit code propagation |
| `test_mmap` | anonymous mmap, write, munmap |
| `test_pipe` | pipe + fork; read/write across parent/child |
| `test_signal` | SIGTERM delivery and handler invocation |
| `test_thread` | pthread_create, mutex, pthread_join |
| `test_tcp` | loopback TCP connect + send + recv |
| `test_filesystem` | open, write, close, open, read on ext4 partition |
| `test_scheduler` | SCHED_FIFO + sched_yield ordering |

## Related Documents

- [overview.md](overview.md)
- [makefile-structure.md](makefile-structure.md)
- [qemu-setup.md](qemu-setup.md)
