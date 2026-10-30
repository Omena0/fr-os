
# TODO

Stuff that needs to be implemented in order to run real programs.

## P0: Blockers for most non-trivial programs

These are missing entirely or barely stubbed and will cause crashes/compile errors for almost anything beyond the existing Fr Userland programs.

| Feature | What exists now | What to add | Why it's #1 | Notes |
| :-- | :-- | :-- | :-- | :-- |
| **Signals** | Dispositions, masks, `kill` to a process or group, `rt_sigreturn`, delivery on syscall and interrupt exit, fault signals, and signal status in `wait4` all exist. Status and remaining gaps: `docs/src/ipc/signals.md`, "Implementation Status". | `EINTR` for blocking calls, `sigaltstack`, `siginfo_t` and `SA_SIGINFO`, job control (stop/continue), queued real-time signals, `tkill`, `rt_sigqueueinfo`. |
| **Termios/TTY control** | `TCGETS`, `TCSETS`, `TCSETSW` and `TCSETSF` store and report the console's termios, and libc has `tcgetattr`/`tcsetattr`. Canonical mode (`ICANON`) buffers lines with erase, kill and EOF; `ECHO`, `ECHOE` and `ECHOK` echo. `ICRNL` applies in canonical mode only. | `ISIG` (signal characters, now that signals exist), `ICRNL` for raw reads, `VMIN`/`VTIME` for non-canonical reads, and a runtime test of canonical reads (needs input injection in the boot harness). |
| **Core filesystem operations** | `open`/`close`/`read`/`write`/`lseek`/`fstat`/`getdents`/`dup`/`fcntl` exist. | `unlink`, `rmdir`, `mkdir`, `rename`, `link`, `symlink`, `readlink`, `access`, `chmod`, `chown`, `stat`/`lstat` (separate from `fstat`), `chdir`, `getcwd`, `umask`. | Coreutils, build systems (make), and most programs rely on these. `fstat` exists but `stat()` on paths is what almost everything calls. | This is purely VFS + syscall table work. |
| **Process waiting semantics** | `wait4` supports `WNOHANG`, the pid rules (`-1`, `0`, `>0`, `<-1`), signal status, and rejects unknown option bits with `EINVAL`. `WUNTRACED` and `WCONTINUED` are accepted and report nothing, because no child can stop or continue yet. | Report stopped and continued children once job control (stop/continue signals) exists; `waitid`. | Shells and any program that waits on children depend on these. | Stop and continue reporting depends on job control, which is not implemented. |

## P1: High impact for portability

These will unblock a lot of real programs once P0 is done.

| Feature | What exists now | What to add | Why | Notes |
| :-- | :-- | :-- | :-- | :-- |
| **I/O multiplexing** | `poll` exists for the console and pipes, with a timeout; the kernel asks each file's `ops->poll`. | `select` (many programs still use it), `ppoll` with a signal mask, and `epoll`. Readiness currently causes a slice-by-slice poll rather than waking the poller on the event. |
| **Environment & exec details** | `environ`/`__environ` are set by crt1 from the kernel-built stack, and an `execve` with `envp = { NULL }` is covered by a runtime test. | `prctl` is missing. Consider `PR_SET_NAME`/`PR_GET_NAME` and `PR_SET_PDEATHSIG`, which libraries and babysitting processes use. |

## P2: For more complex programs

These will be needed for bigger software, but you can get a lot done without them.

| Feature | What exists now | What to add | Why | Notes |
| :-- | :-- | :-- | :-- | :-- |
| **Full threading support** | `clone` with flags, `futex` exist. `arch_prctl` exists. | `set_tid_address`, robust futex handling, proper TLS setup, thread exit coordination. Make pthreads actually work end-to-end. | Needed for any multi-threaded program (some runtimes, servers). | The pieces are there, they just need to be wired together correctly. |
| **Networking** | `socket` syscall number exists but no implementation details visible. | Implement a minimal socket layer (at least `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`). Even if just loopback/virtio-net, it's needed. | Only needed if you want network clients/servers. | This is the biggest dependency if you go this route (drivers + protocol stack). |
| **Advanced memory** | `mmap`/`munmap`/`mprotect`/`brk`/`mremap`/`mlock` exist. | `madvise` is partially stubbed (only accepts MADV_NORMAL/DONTNEED), expand it. `munlock`, `msync`. | Mostly for performance/correctness of some allocators and programs. | Less critical than P0/P1. |
| **Process/session features** | `getpgid`, `setpgid`, `setsid` and `getpgrp` exist in the kernel and in libc. | Job control: stop/continue signals, foreground/background process groups, the terminal's foreground group (`tcsetpgrp`). |

**Recommendation:** Tackle in order: **Signals → Termios → Core filesystem ops → waitpid semantics**.
