
# TODO

Stuff that needs to be implemented in order to run real programs.

## P0: Blockers for most non-trivial programs

These are missing entirely or barely stubbed and will cause crashes/compile errors for almost anything beyond the existing Fr Userland programs.

| Feature | What exists now | What to add | Why it's #1 | Notes |
| :-- | :-- | :-- | :-- | :-- |
| **Signals** | `kill` exists but only accepts valid signals and `SIGKILL` exits the target. `process_deliver_signal` is a weak hook, `sigaction`/`rt_sig*` don't exist. | `rt_sigaction`, `rt_sigprocmask`, `rt_sigpending`, `rt_sigreturn`, `sigaltstack`. Implement delivery in the kernel (especially on syscalls, returns to userland, and faults). | Essential for shells (^C/^Z), job control, crashing programs, timeouts, and most POSIX programs. Without this, everything is broken on interruption. | The hook is already there in the IDT, so the plumbing exists, it's just not implemented. |
| **Termios/TTY control** | Custom console/tty exists, but no visible `termios`/`tcgetattr`/`tcsetattr`. | Implement `ioctl` handlers for `TCGETS`/`TCSETS` (and variants), canonical vs raw mode, echo, ISIG, ICANON, etc. | Interactive programs (editors, shells, REPLs) will not work correctly without raw/canonical mode. This is used constantly by CLI programs. | `ioctl` syscall exists, it's just missing the termios request handlers. |
| **Core filesystem operations** | `open`/`close`/`read`/`write`/`lseek`/`fstat`/`getdents`/`dup`/`fcntl` exist. | `unlink`, `rmdir`, `mkdir`, `rename`, `link`, `symlink`, `readlink`, `access`, `chmod`, `chown`, `stat`/`lstat` (separate from `fstat`), `chdir`, `getcwd`, `umask`. | Coreutils, build systems (make), and most programs rely on these. `fstat` exists but `stat()` on paths is what almost everything calls. | This is purely VFS + syscall table work. |
| **Process waiting semantics** | `wait4` exists. | Support `WNOHANG`, `WUNTRACED`, `WCONTINUED`. Properly handle zombie reaping, process groups, and return values. Integrate with signals/job control. | Shells and any program that spawns children will hang or behave incorrectly without `WNOHANG`. | The process code is already there, just needs the flag handling. |

## P1: High impact for portability

These will unblock a lot of real programs once P0 is done.

| Feature | What exists now | What to add | Why | Notes |
| :-- | :-- | :-- | :-- | :-- |
| **I/O multiplexing** | Nothing obvious. | `poll` and/or `select`. `epoll` would be nice but `poll`/`select` is far more common and easier to implement on top of the existing fd/pipe/tty infrastructure. | Needed for shells, network clients (once networking exists), and many interactive tools. | `pipe`/`pipe2` exist which pairs well with this. |
| **Time functions** | `clock_gettime`, `nanosleep` exist. | `gettimeofday`, `time()`. `setitimer` is less critical but useful. | Tons of programs call `time()`/`gettimeofday()` even if they don't need high precision. `clock_gettime` is there but libc portability will expect these. | Should be trivial wrappers if `clock_gettime` is solid. |
| **Resource limits & system info** | `getuid`/`getgid` exist. | `getrlimit`/`setrlimit`, `uname`, `sysinfo`. | Build systems, shells, and many programs check these for compatibility or to configure themselves. | Usually just return sane defaults, doesn't need full Linux semantics. |
| **FD operations** | `dup`, `fcntl` exist. | `dup2`, `dup3`. Expand `fcntl` to cover `F_DUPFD_CLOEXEC`, `F_GETFD`/`F_SETFD`, `F_GETFL`/`F_SETFL` fully. | Used constantly in shell redirection and by libraries. | Very common and easy to implement on top of existing fd code. |
| **Environment & exec details** | `execve` exists. | Ensure `environ`/`envp` handling is correct (the STATE.md notes had issues with this), handle `execve` with empty env properly, and consider `prctl` for basic flags. | Portability and fixing edge cases. The crash noted in memory about `environ` shows this matters. | Focus on correctness here based on the lessons learned in the repo. |

## P2: For more complex programs

These will be needed for bigger software, but you can get a lot done without them.

| Feature | What exists now | What to add | Why | Notes |
| :-- | :-- | :-- | :-- | :-- |
| **Full threading support** | `clone` with flags, `futex` exist. `arch_prctl` exists. | `set_tid_address`, robust futex handling, proper TLS setup, thread exit coordination. Make pthreads actually work end-to-end. | Needed for any multi-threaded program (some runtimes, servers). | The pieces are there, they just need to be wired together correctly. |
| **Networking** | `socket` syscall number exists but no implementation details visible. | Implement a minimal socket layer (at least `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`). Even if just loopback/virtio-net, it's needed. | Only needed if you want network clients/servers. | This is the biggest dependency if you go this route (drivers + protocol stack). |
| **Advanced memory** | `mmap`/`munmap`/`mprotect`/`brk`/`mremap`/`mlock` exist. | `madvise` is partially stubbed (only accepts MADV_NORMAL/DONTNEED), expand it. `munlock`, `msync`. | Mostly for performance/correctness of some allocators and programs. | Less critical than P0/P1. |
| **Process/session features** | `getpgid`/`setpgid`/`setsid` exist. | Better integration with signals/job control, `getpgrp`. | Needed for proper shell job control. | Pairs with signals. |

**Recommendation:** Tackle in order: **Signals → Termios → Core filesystem ops → waitpid semantics**.
