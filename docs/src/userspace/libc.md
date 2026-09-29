# C Library (libc)

## Overview

The C library (`libc.so.1`) provides the POSIX C runtime. It is the primary interface between application code and the kernel — most applications never call syscalls directly; they call libc wrappers.

## Component Map

```
libc.so.1
  ├── syscall wrappers     (open, read, write, fork, ...)
  ├── stdio                (FILE *, printf, fgets, fopen, fclose, ...)
  ├── stdlib               (malloc, free, atoi, strtol, qsort, ...)
  ├── string               (memcpy, memset, strlen, strcpy, strcmp, ...)
  ├── time                 (time, clock_gettime, strftime, ...)
  ├── signal               (signal, sigaction, sigemptyset, ...)
  ├── setjmp               (setjmp, longjmp — non-local exit)
  ├── ctype                (isalpha, isdigit, toupper, ...)
  ├── locale               (setlocale, nl_langinfo, ...)
  ├── environment          (getenv, setenv, putenv, environ)
  ├── errno                (thread-local errno variable)
  └── startup              (_start, __libc_init, __cxa_atexit, ...)
```

## Syscall Wrappers

Each syscall has a thin wrapper that:
1. Calls the kernel via the `SYSCALL` instruction.
2. On negative return: negates the value, stores in `errno`, returns -1.
3. On success: returns the value directly.

```c
int open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);
    long ret = __syscall3(SYS_open, (long)path, (long)flags, (long)mode);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (int)ret;
}
```

`errno` is a per-thread variable (stored in TLS).

## stdio (Buffered I/O)

`FILE *` wraps a file descriptor with an in-memory buffer:
- **Read buffer**: `fread`/`fgets`/`fgetc` read a large chunk at once from the fd and serve subsequent calls from the buffer.
- **Write buffer**: `fwrite`/`fputs`/`fputc` accumulate writes in the buffer. The buffer is flushed on `fflush`, `\n` (line-buffered streams), or when the buffer is full.
- **Line buffering**: `stdout` to a terminal is line-buffered. `stdout` to a file or pipe is fully buffered.
- **Unbuffered**: `stderr` is unbuffered (writes go directly to the fd).

## malloc Implementation

The libc `malloc` is the hardened allocator (see [security/hardened-allocator.md](../security/hardened-allocator.md)):
- Size classes: 8, 16, 32, ..., 1024, 2048 bytes (per-thread cache, then per-CPU magazine).
- Large allocations (> 32 KB): direct `mmap(MAP_ANONYMOUS)`.
- All allocations: canary words, ASLR-influenced placement.

`free(ptr)` with `ptr == NULL` is a no-op (required by C standard).

## `_start` and Program Startup

The program's entry point (set in the ELF header, or `_start` for static executables):

```asm
_start:
    xor rbp, rbp          ; clear frame pointer (ABI requirement)
    mov rdi, [rsp]        ; argc
    lea rsi, [rsp+8]      ; argv
    lea rdx, [rsp+8 + rdi*8 + 8]  ; envp
    call __libc_init       ; set up libc internal state, TLS, etc.
    call main              ; call application's main()
    mov rdi, rax           ; main return value
    call exit              ; call exit handlers and _exit
```

`__libc_init`:
1. Sets up TLS for the main thread.
2. Initializes `environ` pointer.
3. Calls `__cxa_atexit` registrations from `.init_array`.
4. Returns.

## `exit` and Shutdown

`exit(status)`:
1. Calls all `atexit` handlers in reverse registration order.
2. Flushes and closes all open `FILE *` streams.
3. Calls `sys_exit_group(status)`.

## Related Documents

- [dynamic-linker.md](dynamic-linker.md)
- [libc-threading.md](libc-threading.md)
- [elf-loading.md](elf-loading.md)
- [security/hardened-allocator.md](../security/hardened-allocator.md)
- [memory/userspace-malloc.md](../memory/userspace-malloc.md)
