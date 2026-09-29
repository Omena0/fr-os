/*
 * hello.c — the smallest possible userspace smoke test.
 *
 * Its job is to prove that a process can reach both stdout and stderr through
 * separate file descriptors, that buffered and unbuffered output interleave in
 * the right order, and that the exit status observed by the parent is the one
 * the process chose. That last part is why it does not just always succeed:
 * a PID-supervisor test can only check status propagation by watching a
 * failure happen, and a program that never fails never proves the path works.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	/*
	 * Pass any argument to take the failure path. Documented, greppable
	 * condition rather than a hidden one: the test harness passes a token
	 * and then asserts on both the message and the status code.
	 */
	int fail = (argc > 1 && strcmp(argv[1], "--fail") == 0);

	printf("hello from userspace\n");
	printf("pid %ld\n", (long)getpid());
	printf("stdout is a real fd: write() returned %d\n",
	       write(STDOUT_FILENO, "direct-write-stdout\n", 19));
	printf("args:");
	for (int i = 1; i < argc; i++)
		printf(" %s", argv[i]);
	printf("\n");

	/*
	 * stderr is unbuffered in this libc, so this line is guaranteed to
	 * appear even if stdout is still sitting in its line buffer. If the
	 * two ever interleave wrongly in a console capture, the unbuffered
	 * path is broken.
	 */
	fprintf(stderr, "hello-stderr: this line went to fd 2\n");
	fflush(stdout);

	if (fail) {
		fprintf(stderr, "hello-stderr: failure path taken (--fail), "
				"exiting %d\n", EXIT_FAILURE);
		return EXIT_FAILURE;
	}

	printf("hello-stderr absent: success path, exiting %d\n", EXIT_SUCCESS);
	return EXIT_SUCCESS;
}
