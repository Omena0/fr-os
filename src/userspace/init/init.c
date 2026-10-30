/*
 * init.c - Fr Init, PID 1, the system bring-up and a small shell.
 *
 * Fr Init is the first process. It announces the system, then reads command
 * lines from stdin and runs them. There are no builtins beyond "cd" and
 * "exit": echo, ls, cat and the rest are ordinary programs in /bin, so
 * installing a file there is all it takes to add a command. The line editor
 * lives in lineedit.c.
 *
 * The parser is deliberately small: whitespace separates words, single and
 * double quotes group them, and a backslash quotes the next character. That is
 * enough for the commands this system has; expansion, globbing and pipelines
 * are not implemented yet.
 */
#include <errno.h>
#include <fcntl.h>
#include <kbd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <version.h>
#include "lineedit.h"

/* Internal libc syscall wrapper */
extern int sys_shutdown(int fn);

extern char **environ;

#define TAG FR_INIT_NAME ": "
#define PROMPT FR_INIT_NAME "> "
#define MAX_WORDS 32

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    va_list ap;

    fputs(TAG, stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/*
 * Split a line into words in place. Quotes are removed; a backslash quotes the
 * next character. A quote that is never closed ends the word at the end of the
 * line rather than being an error, which is what a shell prompt does when the
 * user is still typing.
 */
static int split_words(char *line, char **argv, int max)
{
    int argc = 0;
    char *out = line;
    char *p = line;

    while (*p && argc < max - 1) {
        char quote = 0;
        char *word;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        word = out;
        while (*p) {
            if (!quote && (*p == ' ' || *p == '\t'))
                break;
            if (!quote && (*p == '\'' || *p == '"')) {
                quote = *p++;
                continue;
            }
            if (quote && *p == quote) {
                quote = 0;
                p++;
                continue;
            }
            if (*p == '\\' && p[1]) {
                p++;
                *out++ = *p++;
                continue;
            }
            *out++ = *p++;
        }
        /* Consume the separator before writing the terminator: p points at
         * the space, and the terminator is written at out, which is at or
         * before p. Terminating first would end the line at the first space
         * and every later word would disappear. */
        if (*p)
            p++;
        *out++ = '\0';
        argv[argc++] = word;
    }
    argv[argc] = NULL;
    return argc;
}

/*
 * Run an external command. The child executes; the parent waits and then
 * restores the console state a program may have changed (non-blocking stdin,
 * a faster keyboard repeat rate).
 */
static void run_command(int argc, char **argv)
{
    pid_t pid;
    int status;

    (void)argc;
    pid = fork();
    if (pid < 0) {
        say("%s: %s", argv[0], strerror(errno));
        return;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "%s%s: %s\n", TAG, argv[0], strerror(errno));
        _exit(127);
    }

    waitpid(pid, &status, 0);

    {
        int fl = fcntl(0, F_GETFL, 0);

        if (fl >= 0)
            fcntl(0, F_SETFL, fl & ~O_NONBLOCK);
    }
    kbd_typematic(KBD_TYPEMATIC_DEFAULT);
}

/* Returns nonzero when the shell should exit. */
static int dispatch(int argc, char **argv)
{
    if (argc == 0)
        return 0;

    if (strcmp(argv[0], "exit") == 0) {
        say("exit: shutting down.");
        sys_shutdown(0);
        return 1;
    }

    /*
     * cd has to be a builtin: an external process can change only its own
     * working directory, not the shell's. Everything else is a program.
     */
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : "/";

        if (chdir(dir) < 0)
            say("cd: %s: %s", dir, strerror(errno));
        return 0;
    }

    run_command(argc, argv);
    return 0;
}

int main(void)
{
    char line[512];
    char *argv[MAX_WORDS];

    printf("%s%s %s (build %s, rev %s)\n", TAG, FR_INIT_NAME,
           KERNEL_VERSION, KERNEL_BUILD_STAMP, KERNEL_GIT_REV);
    printf("%s%s %s -- pid %ld, page size %d bytes\n", TAG,
           FR_PROJECT_NAME, "system bring-up", (long)getpid(),
           getpagesize());
    printf("%srunning on %s\n", TAG, KERNEL_VERSION_STRING);
    printf("%stype 'help' for the command list, EOF to stop\n", TAG);
    fflush(stdout);

    /*
     * PATH is what the commands are found on. The kernel passes no
     * environment, so this is the default the shell installs. A caller that
     * did provide PATH keeps it.
     */
    setenv("PATH", "/bin:/usr/bin", 0);

    for (;;) {
        int argc;

        if (line_edit(PROMPT, line, sizeof(line)) < 0)
            break;

        argc = split_words(line, argv, MAX_WORDS);
        if (dispatch(argc, argv))
            break;
    }

    fflush(stdout);
    printf("%sstdin closed, init exiting\n", TAG);
    return 0;
}
