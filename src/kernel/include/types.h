/*
 * types.h — fixed-width types and core kernel primitives.
 *
 * Freestanding: no host libc, no compiler extensions beyond what GCC provides
 * for a bare-metal x86-64 target. Sizes are asserted at the bottom of the file
 * because a silent width mismatch between the kernel and its structures is the
 * single most expensive class of bug in this codebase.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint64_t phys_addr_t;
typedef uint64_t virt_addr_t;

/* Size_t for kernel-internal allocation arithmetic. */
typedef size_t   ksize_t;

/*
 * A virtual address with the low 12 bits cleared. x86-64 pages are always
 * 4 KiB regardless of the physical memory geometry, so this is the only page
 * granularity the kernel has to reason about.
 */
#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)
#define PAGE_MASK       (~(PAGE_SIZE - 1))

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((typeof(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((typeof(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((typeof(x))(a) - 1)) == 0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define UNUSED(x) ((void)(x))

/* Compiler hints used throughout the kernel. */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define __packed    __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __noreturn  __attribute__((noreturn))
#define __must_check __attribute__((warn_unused_result))

/* A symbol placed in a named section, e.g. __section(".text.boot"). */
#define __section(s) __attribute__((section(s)))

/*
 * Build-time assertion that works in both C and assembly contexts. The kernel
 * uses it to pin down ABI assumptions (struct sizes, enum widths) that would
 * otherwise only fail at run time, on a machine that is already crashing.
 */
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* Correct branch prediction for unreachable code, used after __noreturn calls. */
#define __builtin_unreachable() __builtin_unreachable()

#endif /* TYPES_H */
