/*
 * cpu_features.h — runtime CPU feature detection.
 *
 * Features are detected once, before any subsystem is initialised, and cached
 * in a global the rest of the kernel reads. Nothing checks CPUID at run time in
 * a hot path: a cpuid is a serialising instruction costing hundreds of
 * cycles, and the answer cannot change while the kernel is running.
 *
 * The target is a single specific CPU (QEMU with -cpu host), so the kernel is
 * free to *use* the whole feature set once confirmed present. Every feature is
 * still gated behind a cached check rather than assumed, so the same image runs
 * on a subset machine instead of faulting.
 */
#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include <types.h>

/* Bits in cpu_features::basic[] (CPUID leaf 1, EDX). */
#define CPU_FEATURE_FPU        (1u << 0)
#define CPU_FEATURE_TSC        (1u << 4)
#define CPU_FEATURE_MSR        (1u << 5)
#define CPU_FEATURE_PAE        (1u << 6)
#define CPU_FEATURE_CX8        (1u << 8)   /* CMPXCHG8B: atomic 64-bit CAS */
#define CPU_FEATURE_APIC       (1u << 9)
#define CPU_FEATURE_SEP        (1u << 11)
#define CPU_FEATURE_MTRR       (1u << 12)
#define CPU_FEATURE_PGE        (1u << 13)
#define CPU_FEATURE_MCE        (1u << 14)
#define CPU_FEATURE_PAT        (1u << 16)
#define CPU_FEATURE_CLFLUSH    (1u << 19)
#define CPU_FEATURE_NX         (1u << 20)   /* XD: page no-execute */
#define CPU_FEATURE_TSD_DEADLINE (1u << 24) /* local APIC TSC deadline timer */

/* Bits in cpu_features::basic_ecx[] (CPUID leaf 1, ECX). */
#define CPU_FEATURE_SSE        (1u << 0)
#define CPU_FEATURE_SSE2       (1u << 26)
#define CPU_FEATURE_HYPERVISOR (1u << 31)

/* Bits in cpu_features::extended_ecx[] (CPUID 0x80000001, ECX). */
#define CPU_FEATURE_LAHF_LAM   (1u << 0)
#define CPU_FEATURE_LZCNT      (1u << 5)
#define CPU_FEATURE_POPCNT     (1u << 23)
#define CPU_FEATURE_ERMS       (1u << 9)    /* enhanced REP MOVSB/STOSB */
#define CPU_FEATURE_FSRM       (1u << 31)   /* fast short REP MOVSB */

/* Bits in cpu_features::structured[] (CPUID leaf 7 subleaf 0, EBX/ECX/EDX). */
#define CPU_FEATURE_AVX        (1u << 0)
#define CPU_FEATURE_BMI1       (1u << 3)
#define CPU_FEATURE_HLE        (1u << 4)
#define CPU_FEATURE_AVX2       (1u << 5)
#define CPU_FEATURE_BMI2       (1u << 8)
#define CPU_FEATURE_ERMS2      (1u << 9)
#define CPU_FEATURE_RTM        (1u << 11)
#define CPU_FEATURE_AVX512F    (1u << 16)
#define CPU_FEATURE_AVX512DQ   (1u << 17)
#define CPU_FEATURE_AVX512BW   (1u << 30)
#define CPU_FEATURE_AVX512VL   (1u << 31)
#define CPU_FEATURE_SHA        (1u << 29)
#define CPU_FEATURE_CLFLUSHOPT (1u << 23)   /* leaf 7 EBX */
#define CPU_FEATURE_CLWB       (1u << 24)   /* leaf 7 EBX */
#define CPU_FEATURE_RDPID      (1u << 22)   /* leaf 7 ECX */
#define CPU_FEATURE_AVX_VNNI   (1u << 4)    /* leaf 7 ECX */

/* CR4 bits the kernel cares about. */
#define CR4_PAE      (1u << 5)
#define CR4_MCE      (1u << 6)
#define CR4_PGE      (1u << 7)
#define CR4_OSFXSR   (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)
#define CR4_SMEP     (1u << 20)
#define CR4_SMAP     (1u << 21)
#define CR4_PCIDE    (1u << 17)

/* Control register 0 bits. */
#define CR0_PE       (1u << 0)
#define CR0_MP       (1u << 1)   /* monitor coprocessor */
#define CR0_EM       (1u << 2)
#define CR0_TS       (1u << 3)   /* task switched: save FPU on x87/SSE */
#define CR0_NE       (1u << 5)   /* native FPU error reporting */
#define CR0_WP       (1u << 16)  /* write protect */
#define CR0_PG       (1u << 31)

struct cpu_features {
	char     vendor[13];
	uint32_t max_leaf;           /* highest basic CPUID leaf */
	uint32_t max_ext_leaf;       /* highest extended CPUID leaf */
	uint32_t family, model, stepping;
	uint32_t brand;              /* brand index from CPUID 0x80000002-4 */

	uint32_t basic_edx;          /* CPUID leaf 1 EDX */
	uint32_t basic_ecx;          /* CPUID leaf 1 ECX */
	uint32_t extended_ecx;       /* CPUID 0x80000001 ECX */
	uint32_t extended_edx;       /* CPUID 0x80000001 EDX */
	uint32_t leaf7_ebx;          /* CPUID leaf 7.0 EBX */
	uint32_t leaf7_ecx;          /* CPUID leaf 7.0 ECX */
	uint32_t leaf7_edx;          /* CPUID leaf 7.0 EDX */

	/* Cached boolean views, computed once in cpu_features_init(). */
	uint64_t feature_mask;
	uint32_t numa_nodes;         /* from CPUID leaf 0xB, logical processors */
	uint32_t logical_cpus;       /* from CPUID leaf 1 EBX[23:16] */
	uint64_t tsc_hz;             /* measured, not assumed */
	uint64_t tsc_khz;            /* from CPUID 0x15 where available */
	bool     invariant_tsc;      /* CPUID 0x80000007 EDX bit 8 */
	bool     tsc_deadline_timer; /* TSD_DEADLINE usable on the local APIC */
	bool     hypervisor;
	bool     apic;
	bool     has_1gb_pages;      /* PDPT entry with PageSize set */
	bool     has_5level_paging;  /* CPUID 0x80000008 ECX bit 16 */
};

extern struct cpu_features cpu_features;

void cpu_features_init(void);

/* Test one CPU_FEATURE_* bit. */
static inline bool cpu_has(uint64_t feature)
{
	return (cpu_features.feature_mask & feature) != 0;
}

/* Read the vendor string. */
const char *cpu_vendor(void);

#endif /* CPU_FEATURES_H */
