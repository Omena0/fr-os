/*
 * termios.h - terminal attributes.
 *
 * The structure and the flag and index constants are the kernel's
 * (uapi/syscall.h). The console stores the flags and reports them; canonical
 * mode, echo and signal characters are not acted on yet.
 */
#ifndef TERMIOS_H
#define TERMIOS_H

#include <uapi/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int optional_actions, const struct termios *t);

#ifdef __cplusplus
}
#endif

#endif /* TERMIOS_H */
