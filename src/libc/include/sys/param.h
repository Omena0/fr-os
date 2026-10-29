/*
 * sys/param.h — small value helpers that almost every program wants.
 *
 * The type-generic MIN/MAX evaluate their arguments twice, so do not pass an
 * expression with side effects. ARRAY_SIZE is the safe count form.
 */
#ifndef SYS_PARAM_H
#define SYS_PARAM_H

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#endif /* SYS_PARAM_H */
