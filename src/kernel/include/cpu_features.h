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

/* ---------------------------------------------------------------------------
 * Feature IDs.
 *
 * A CPU_FEATURE_* constant is a bit in `cpu_features::feature_mask`, and it is
 * NOT a CPUID bit number. The two used to be the same thing, and that is the
 * bug this block replaces.
 *
 * feature_mask is 64 bits and CPUID features are scattered over five leaf
 * registers of 32 bits each. Reusing the CPUID bit number as the mask bit
 * collapses 160 bits into 64, so features from different registers collide and
 * the mask can no longer answer the only question anyone asks of it: does this
 * CPU have *this* feature? `cpu_has(CPU_FEATURE_APIC)` tested bit 9, which is
 * APIC in leaf 1 EDX, ERMS in leaf 0x80000001 ECX and ERMS in leaf 7 EBX — so
 * one bit, three features, and the answer was "yes" if any of them was present.
 * A wider mask is not the fix; there is no width at which 160 bits fit in 64.
 * Each leaf register gets its own block of IDs instead, so a feature's ID says
 * which leaf it came from and no two features can ever share a bit.
 *
 * The IDs below are sequential within each leaf's block and each carries its
 * real CPUID leaf, register and bit in the trailing comment. That comment is
 * the authority, not the constant: reading the feature out of the raw register
 * is `cpu_features.basic_edx & (1u << 24)`, and it is what
 * cpu_features_init() does. Use cpu_has() when you want the cached answer.
 *
 * Cross-checked against the Linux kernel's arch/x86/include/asm/cpufeatures.h,
 * which is itself generated from the SDM tables. Several of the old constants
 * named a leaf and a bit that do not go together — see the note on
 * CPU_FEATURE_AVX below for the worst of them.
 * ------------------------------------------------------------------------- */

/* CPUID.1:EDX — IDs 0..15 */
#define CPU_FEATURE_FPU         (1ULL << 0)   /* 1:EDX  0  on-board x87        */
#define CPU_FEATURE_TSC         (1ULL << 1)   /* 1:EDX  4                        */
#define CPU_FEATURE_MSR         (1ULL << 2)   /* 1:EDX  5                        */
#define CPU_FEATURE_PAE         (1ULL << 3)   /* 1:EDX  6                        */
#define CPU_FEATURE_MCE         (1ULL << 4)   /* 1:EDX  7  machine check        */
#define CPU_FEATURE_CX8         (1ULL << 5)   /* 1:EDX  8  CMPXCHG8B            */
#define CPU_FEATURE_APIC        (1ULL << 6)   /* 1:EDX  9                        */
#define CPU_FEATURE_SEP         (1ULL << 7)   /* 1:EDX 11  SYSENTER/SYSEXIT     */
#define CPU_FEATURE_MTRR        (1ULL << 8)   /* 1:EDX 12                        */
#define CPU_FEATURE_PGE         (1ULL << 9)   /* 1:EDX 13  page global enable   */
#define CPU_FEATURE_PAT         (1ULL << 10)  /* 1:EDX 16                       */
#define CPU_FEATURE_CLFLUSH     (1ULL << 11)  /* 1:EDX 19                       */
#define CPU_FEATURE_FXSR        (1ULL << 12)  /* 1:EDX 24  FXSAVE/FXRSTOR       */
#define CPU_FEATURE_SSE         (1ULL << 13)  /* 1:EDX 25  XMM                   */
#define CPU_FEATURE_SSE2        (1ULL << 14)  /* 1:EDX 26  XMM2                  */
#define CPU_FEATURE_HT          (1ULL << 15)  /* 1:EDX 28  hyper-threading       */

/* CPUID.1:ECX — IDs 16..23 */
#define CPU_FEATURE_SSE3        (1ULL << 16)  /* 1:ECX  0  XMM3                  */
#define CPU_FEATURE_CX16        (1ULL << 17)  /* 1:ECX 13  CMPXCHG16B            */
#define CPU_FEATURE_POPCNT      (1ULL << 18)  /* 1:ECX 23                        */
#define CPU_FEATURE_TSC_DEADLINE (1ULL << 19) /* 1:ECX 24  local APIC TSC deadline*/
#define CPU_FEATURE_XSAVE       (1ULL << 20)  /* 1:ECX 26                        */
#define CPU_FEATURE_OSXSAVE     (1ULL << 21)  /* 1:ECX 27                        */
/* AVX is leaf 1 ECX bit 28. The old constant said "leaf 7 EBX bit 0", which is
 * FSGSBASE — so cpu_has(CPU_FEATURE_AVX) was reporting FSGSBASE. */
#define CPU_FEATURE_AVX         (1ULL << 22)  /* 1:ECX 28                        */
#define CPU_FEATURE_HYPERVISOR  (1ULL << 23)  /* 1:ECX 31                        */

/* CPUID.0x80000001:ECX — IDs 24..27 */
#define CPU_FEATURE_LAHF_LAM    (1ULL << 24)  /* :ECX  0  LAHF/SAHF in long mode */
#define CPU_FEATURE_ABM         (1ULL << 25)  /* :ECX  5  LZCNT/LMUL/POPCNT/SHA  */
/* There is no LZCNT bit. LZCNT is part of ABM, so this reads the ABM bit under
 * the name callers already use. The old constant said ":ECX bit 5" and was
 * documented as being in leaf 7 — bit 5 of leaf 7 EBX is AVX2. */
#define CPU_FEATURE_LZCNT       CPU_FEATURE_ABM
#define CPU_FEATURE_SSE4A       (1ULL << 26)  /* :ECX  6                        */
#define CPU_FEATURE_OSVW        (1ULL << 27)  /* :ECX  9  OS-visible workaround  */

/* CPUID.0x80000001:EDX — IDs 28..35 */
#define CPU_FEATURE_SYSCALL     (1ULL << 28)  /* :EDX 11                        */
#define CPU_FEATURE_MP          (1ULL << 29)  /* :EDX 19  MP capable            */
/* NX is bit 20 of THIS register. Bit 20 of leaf 1 EDX is reserved and reads zero
 * on every part, which is why NX used to report as absent on every CPU. */
#define CPU_FEATURE_NX          (1ULL << 30)  /* :EDX 20  XD: page no-execute   */
#define CPU_FEATURE_MMXEXT      (1ULL << 31)  /* :EDX 22                        */
#define CPU_FEATURE_FXSR_OPT    (1ULL << 32)  /* :EDX 25                        */
/* 1 GiB pages. The kernel reads this: vmm.c asks has_1gb_pages before mapping a
 * PDPT entry with PS=1, and nothing in cpu_features.c ever set the field, so the
 * 1 GiB path was dead on every machine. */
#define CPU_FEATURE_GBPAGES     (1ULL << 33)  /* :EDX 26  PDPE1GB               */
#define CPU_FEATURE_RDTSCP      (1ULL << 34)  /* :EDX 27                        */
#define CPU_FEATURE_LM          (1ULL << 35)  /* :EDX 29  long mode             */

/* CPUID.7.0:EBX — IDs 36..51 */
#define CPU_FEATURE_FSGSBASE    (1ULL << 36)  /* 7:EBX  0                       */
#define CPU_FEATURE_BMI1        (1ULL << 37)  /* 7:EBX  3                       */
#define CPU_FEATURE_HLE         (1ULL << 38)  /* 7:EBX  4  hardware lock elision*/
#define CPU_FEATURE_AVX2        (1ULL << 39)  /* 7:EBX  5                       */
#define CPU_FEATURE_SMEP        (1ULL << 40)  /* 7:EBX  7                       */
#define CPU_FEATURE_BMI2        (1ULL << 41)  /* 7:EBX  8                       */
/* ERMS is leaf 7 EBX bit 9. The old CPU_FEATURE_ERMS read ":ECX bit 9", which is
 * OSVW, and the old CPU_FEATURE_ERMS2 read 7:EBX bit 9, which is ERMS — the two
 * constants were each other's error. There is no ERMS2 CPUID bit; AMD's second
 * fast-string feature is FSRM, below. */
#define CPU_FEATURE_ERMS        (1ULL << 42)  /* 7:EBX  9  enhanced REP MOVSB   */
#define CPU_FEATURE_INVPCID     (1ULL << 43)  /* 7:EBX 10                       */
#define CPU_FEATURE_RTM         (1ULL << 44)  /* 7:EBX 11                       */
#define CPU_FEATURE_AVX512F     (1ULL << 45)  /* 7:EBX 16                       */
#define CPU_FEATURE_AVX512DQ    (1ULL << 46)  /* 7:EBX 17                       */
#define CPU_FEATURE_CLFLUSHOPT  (1ULL << 47)  /* 7:EBX 23                       */
#define CPU_FEATURE_CLWB        (1ULL << 48)  /* 7:EBX 24                       */
/* SHA_NI is leaf 7 EBX bit 29. The old constant read leaf 7 *EDX* bit 29, which
 * is ARCH_CAPABILITIES. */
#define CPU_FEATURE_SHA         (1ULL << 49)  /* 7:EBX 29                       */
#define CPU_FEATURE_AVX512BW    (1ULL << 50)  /* 7:EBX 30                       */
#define CPU_FEATURE_AVX512VL    (1ULL << 51)  /* 7:EBX 31                       */

/* CPUID.7.0:ECX — IDs 52..53 */
#define CPU_FEATURE_LA57        (1ULL << 52)  /* 7:ECX 16  5-level paging       */
#define CPU_FEATURE_RDPID       (1ULL << 53)  /* 7:ECX 22                       */

/* CPUID.7.0:EDX — IDs 54..55 */
#define CPU_FEATURE_FSRM        (1ULL << 54)  /* 7:EDX  4  fast short REP MOVSB */
/* The old CPU_FEATURE_FSRM read ":ECX bit 31", which is reserved, so it could
 * never be set on any CPU — which is exactly what the boot log reported. */
#define CPU_FEATURE_SERIALIZE   (1ULL << 55)  /* 7:EDX 14  SERIALIZE            */

/* CPUID.7.1:EAX — ID 56 */
#define CPU_FEATURE_AVX_VNNI    (1ULL << 56)  /* 7:1:EAX 4                      */

/* Next free feature ID: 57. cpu_features.h owns the allocation; 7 slots remain
 * of the 64. If the kernel ever needs more than that, this stops being a mask
 * and becomes a per-leaf array of the raw registers — which the struct below
 * already is, and which is where a reader should go for anything not listed. */

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

	/*
	 * The raw leaf registers, kept verbatim. Every CPU_FEATURE_* constant
	 * above is defined as a specific bit of one of these, so anything not
	 * given a feature ID is still reachable here rather than invisible.
	 */
	uint32_t basic_edx;          /* CPUID leaf 1 EDX */
	uint32_t basic_ecx;          /* CPUID leaf 1 ECX */
	uint32_t extended_ecx;       /* CPUID 0x80000001 ECX */
	uint32_t extended_edx;       /* CPUID 0x80000001 EDX */
	uint32_t leaf7_ebx;          /* CPUID leaf 7.0 EBX */
	uint32_t leaf7_ecx;          /* CPUID leaf 7.0 ECX */
	uint32_t leaf7_edx;          /* CPUID leaf 7.0 EDX */

	/* Cached answers, computed once in cpu_features_init(). */
	/*
	 * One bit per feature ID from the block above — NOT one bit per CPUID
	 * bit. `features 0x%016llx` in the boot log is therefore a statement
	 * that can be read back against the ID table, which it could not be
	 * while the IDs were CPUID bit numbers: nine bit positions carried two or
	 * three different meanings each.
	 */
	uint64_t feature_mask;
	uint32_t numa_nodes;         /* from CPUID leaf 0xB */
	/*
	 * From CPUID leaf 1 EBX[23:16], which is only defined when the
	 * hyper-threading bit (1:EDX 28) is set. Normalised to at least 1 in
	 * cpu_features_init(), so this field is never the "0 logical CPUs" that
	 * main.c used to print against cpu_features.c's own `?: 1`.
	 */
	uint32_t logical_cpus;
	uint64_t tsc_khz;            /* from CPUID 0x15 where available, else 0 */
	/* CPUID 0x80000007 EDX bit 8. Not 0x80000001 EDX bit 8, which is
	 * reserved — the old read could never report a constant TSC. */
	bool     invariant_tsc;
	bool     tsc_deadline_timer; /* CPUID 1:ECX 24 */
	bool     hypervisor;         /* CPUID 1:ECX 31 */
	bool     apic;               /* CPUID 1:EDX 9 */
	bool     has_1gb_pages;      /* CPUID 0x80000001 EDX 26 (PDPE1GB) */
	bool     has_5level_paging;  /* CPUID 0x80000008 ECX 16 */
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
