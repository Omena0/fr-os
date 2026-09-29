/*
 * stdarg.h — forward to the compiler's implementation.
 *
 * va_list, va_start, va_arg, va_end, va_copy are all compiler builtins and
 * there is no point in libc redefining them. This header exists so that
 * libc's own headers can be self-contained and so that a program compiled with
 * -I src/libc/include gets the same macros regardless of host toolchain.
 */
#ifndef STDARG_H
#define STDARG_H

#include_next <stdarg.h>

#endif /* STDARG_H */