/*
 * hello.c — Fr Userland's smallest smoke test.
 *
 * Its job is to prove that a process can reach both stdout and stderr through
 * separate file descriptors, that buffered and unbuffered output interleave in
 * the right order, and that the exit status observed by the parent is the one
 * the process chose. That last part is why it does not just always succeed:
 * a PID-supervisor test can only check status propagation by watching a
 * failure happen, and a program that never fails never proves the path works.
 *
 * It is a Fr Userland program: it runs in user mode on top of Fr Init, on Fr
 * Core, and it is built with Fr Libc. Those names come from <version.h> so
 * that renaming the project cannot leave userspace claiming an older identity.
 */
#include <stdio.h>

int main()
{
    printf("Hello, World!\n");

    return 0;
}
