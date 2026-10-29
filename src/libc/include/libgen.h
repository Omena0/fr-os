/*
 * libgen.h — path component helpers.
 *
 * Both functions may modify the string they are given (POSIX permits it).
 * basename never allocates; dirname returns a pointer into `path`, or a static
 * "." for a path with no directory part.
 */
#ifndef LIBGEN_H
#define LIBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

char *basename(char *path);
char *dirname(char *path);

#ifdef __cplusplus
}
#endif

#endif /* LIBGEN_H */
