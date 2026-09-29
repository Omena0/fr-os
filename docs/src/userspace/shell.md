# Shell

## Overview

The shell is a command-line interpreter that reads commands from a terminal (or a script file), parses them, and executes them. It is a normal userspace application — it does not require any special kernel privileges.

## Features

- Interactive REPL: display prompt, read a line, execute, repeat.
- Command execution: `fork` + `exec` (simple commands); `fork` + pipe + two `exec` calls (pipelines).
- Built-in commands: `cd`, `exit`, `export`, `unset`, `source`, `jobs`, `fg`, `bg`, `wait`.
- Variable expansion: `$VAR`, `${VAR:-default}`, `$(command)`.
- Glob expansion: `*`, `?`, `[...]` (matched against filesystem via `opendir`/`readdir`).
- Pipelines: `cmd1 | cmd2 | cmd3` — creates a chain of pipes between processes.
- Redirections: `< infile`, `> outfile`, `>> append`, `2>&1`.
- Job control: `Ctrl+Z` → `SIGTSTP`; `bg`/`fg` to manage background jobs.
- Script execution: `sh script.sh` — execute commands from a file.

## Execution Model

### Simple Command

```c
// parse: ["ls", "-la"]
pid = fork();
if (pid == 0) {
    // child
    execvp("ls", (char *[]){"ls", "-la", NULL});
    perror("execvp"); exit(127);
}
// parent: waitpid(pid, &status, 0)
```

### Pipeline

```c
// parse: ["ls", "|", "grep", "foo"]
int pipefd[2]; pipe(pipefd);

pid1 = fork();
if (pid1 == 0) {
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[0]); close(pipefd[1]);
    execvp("ls", ...);
}

pid2 = fork();
if (pid2 == 0) {
    dup2(pipefd[0], STDIN_FILENO);
    close(pipefd[0]); close(pipefd[1]);
    execvp("grep", ...);
}

close(pipefd[0]); close(pipefd[1]);
waitpid(pid1, ...); waitpid(pid2, ...);
```

## Job Control

When a command is run in the background (`cmd &`):
- The shell puts the child into a new process group: `setpgid(child_pid, child_pid)`.
- The shell does not wait for it; records it in the jobs list.

Terminal foreground group: only the foreground process group receives `SIGINT` (Ctrl+C), `SIGTSTP` (Ctrl+Z) from the terminal driver.

`fg %1`: `tcsetpgrp(tty_fd, job->pgid)` → gives terminal control to the job's process group; then `SIGCONT` to the group; then `waitpid`.

## Prompt

The shell prints a prompt to `stderr` (to avoid contaminating piped output):
```
/home/user $ 
```

The prompt string can include the current directory, git branch, exit code of the previous command, etc., substituted via variable expansion.

## Signal Handling

The shell sets up the following signal handlers for itself:
| Signal | Handler |
|---|---|
| `SIGINT` | Ignore (the Ctrl+C is sent to the foreground job's process group, not the shell) |
| `SIGTSTP` | Ignore (the shell should not be stopped by Ctrl+Z) |
| `SIGCHLD` | Reap background jobs and update the jobs list |
| `SIGTTOU` / `SIGTTIN` | Ignore (when background job tries to access terminal) |

## Related Documents

- [init-system.md](init-system.md)
- [process-model.md](process-model.md)
- [ipc/signals.md](../ipc/signals.md)
- [ipc/pipes.md](../ipc/pipes.md)
- [syscalls/process-syscalls.md](../syscalls/process-syscalls.md)
