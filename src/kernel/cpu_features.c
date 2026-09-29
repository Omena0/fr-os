/*
 * cpu_features.c — CPUID feature discovery.
 *
 * Runs first thing in kernel_main, before any subsystem is initialised, so
 * every later decision (whether to enable NX, SMEP, SMAP, TSC deadline
 * interrupts, 1 GiB pages) is based on what the machine actually reports
 * rather than what the kernel was compiled to expect.
 */

#include <cpu_features.h>
#include <io.h>
#include <klog.h>

KLOG_SUBSYSTEM("cpuid");

struct cpu_features cpu_features;

/*
 * A thin wrapper so the leaf selectors are written in a readable order.
 *
 * Named cpuid_raw rather than cpuid because GCC already owns `cpuid` as a
 * builtin taking and returning a __cpuid_result; a static function of the same
 * name is a redefinition, not an overload, and the build stops.
 *
 * On x86-64, EBX is an ordinary register and CPUID can write it directly; the
 * "CPUID clobbers EBX" workaround is a 32-bit PIC issue that does not apply
 * here. GCC is told the outputs so it never assumes a register survives the
 * call.
 */
static inline void cpuid_raw(uint32_t leaf, uint32_t sub,
			     uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid"
			 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			 : "a"(leaf), "c"(sub));
}

/*
 * Discover what the processor supports. Runs first in kernel_main, before any
 * subsystem is initialised, so every later decision (NX, SMEP, SMAP, TSC
 * deadline interrupts, 1 GiB pages) is based on what the machine reports
 * rather than what the kernel was compiled to expect.
 */
void cpu_features_init(void)
{
	uint32_t a, b, c, d;
	uint32_t max_leaf, max_ext;
	uint32_t cpuid_leaf, cpuid_sub;

	cpuid_raw(CPUID_MAX_LEAF, 0, &max_leaf, &b, &c, &d);
	cpu_features.max_leaf = max_leaf;

	/* Vendor string, little-endian order per the SDM. */
	cpuid_raw(CPUID_VENDOR, 0, &a, &b, &c, &d);
	cpu_features.vendor[0] = (char)(b & 0xFF);
	cpu_features.vendor[1] = (char)((b >> 8) & 0xFF);
	cpu_features.vendor[2] = (char)((b >> 16) & 0xFF);
	cpu_features.vendor[3] = (char)((b >> 24) & 0xFF);
	cpu_features.vendor[4] = (char)(d & 0xFF);
	cpu_features.vendor[5] = (char)((d >> 8) & 0xFF);
	cpu_features.vendor[6] = (char)((d >> 16) & 0xFF);
	cpu_features.vendor[7] = (char)((d >> 24) & 0xFF);
	cpu_features.vendor[8] = (char)(c & 0xFF);
	cpu_features.vendor[9] = (char)((c >> 8) & 0xFF);
	cpu_features.vendor[10] = (char)((c >> 16) & 0xFF);
	cpu_features.vendor[11] = (char)((c >> 24) & 0xFF);
	cpu_features.vendor[12] = '\0';

	klog(KLOG_INFO, "cpu: %s, max leaf 0x%x\n", cpu_features.vendor, max_leaf);

	if (max_leaf >= CPUID_FEATURES_1) {
		cpuid_raw(CPUID_FEATURES_1, 0, &a, &b, &c, &d);
		cpu_features.basic_edx = d;
		cpu_features.basic_ecx = c;
		cpu_features.family = ((a >> 8) & 0xF) + ((a >> 20) & 0xF);
		cpu_features.model = ((a >> 4) & 0xF) | ((a >> 16) & 0xF);
		cpu_features.stepping = a & 0xF;
		cpu_features.logical_cpus = (b >> 16) & 0xFF;
	}

	cpuid_raw(CPUID_MAX_EXT_LEAF, 0, &max_ext, &b, &c, &d);
	cpu_features.max_ext_leaf = max_ext;

	if (max_ext >= CPUID_EXT_FEATURES) {
		cpuid_raw(CPUID_EXT_FEATURES, 0, &a, &b, &c, &d);
		cpu_features.extended_ecx = c;
		cpu_features.extended_edx = d;
		cpu_features.invariant_tsc = (d >> 8) & 1;
	}

	/* Structured extended features. Leaf 7 subleaf 0 reports the maximum
	 * input value for the leaf in EAX, so the subleaf walk has to be bounded
	 * rather than trusting a fixed count. */
	if (max_leaf >= 7) {
		uint32_t sub = 0;
		uint32_t max_sub;

		cpuid_raw(7, 0, &max_sub, &b, &c, &d);
		while (sub <= max_sub) {
			cpuid_raw(7, sub, &a, &b, &c, &d);
			if (sub == 0) {
				cpu_features.leaf7_ebx = b;
				cpu_features.leaf7_ecx = c;
				cpu_features.leaf7_edx = d;
			}
			sub++;
		}
	}

	/* 5-level paging and CLZERO capability live in 0x80000008. */
	if (max_ext >= 0x80000008u) {
		cpuid_raw(0x80000008u, 0, &a, &b, &c, &d);
		cpu_features.has_5level_paging = (c >> 16) & 1;
	}

	/* Topology: leaf 0xB reports the logical processor count and, on
	 * subleaf 0, whether a level is the x2APIC enumeration. */
	{
		uint32_t sub = 0;
		uint32_t max_sub = 0;

		if (max_leaf >= 0xB) {
			cpuid_raw(0xB, 0, &max_sub, &b, &c, &d);
			uint32_t nodes = 0;
			for (sub = 0; sub <= max_sub && sub < 16; sub++) {
				uint32_t type = 0;
				cpuid_raw(0xB, sub, &a, &b, &type, &d);
				if (type == 0)
					continue;
				if (type == 1)
					nodes++;
			}
			cpu_features.numa_nodes = nodes ? nodes : 1;
		} else {
			cpu_features.numa_nodes = 1;
		}
	}

	/* TSC frequency. CPUID 0x15 reports a crystal ratio where available; fall
	 * back to CPUID 0x16 (processor frequency in MHz) and finally to
	 * calibration against the PIT. */
	cpu_features.tsc_khz = 0;
	if (max_leaf >= CPUID_TSC) {
		uint32_t denom = 0, numer = 0, crystal = 0;

		cpuid_raw(CPUID_TSC, 0, &a, &denom, &numer, &crystal);
		if (denom && numer) {
			/* Leaf 0x15 EAX is the crystal clock, EBX the denominator,
			 * ECX the numerator. The ratio only describes the TSC when
			 * EAX is non-zero. */
			if (a != 0)
				cpu_features.tsc_khz = (uint64_t)crystal * numer / denom;
		}
	}

	cpu_features.hypervisor = (cpu_features.basic_ecx >> 31) & 1;
	cpu_features.apic = (cpu_features.basic_edx >> 9) & 1;
	cpu_features.tsc_deadline_timer = (cpu_features.basic_edx >> 24) & 1;

	/* Assemble the feature mask the rest of the kernel queries. */
	uint64_t f = 0;

	if (cpu_features.basic_edx & CPU_FEATURE_FPU) f |= CPU_FEATURE_FPU;
	if (cpu_features.basic_edx & CPU_FEATURE_TSC) f |= CPU_FEATURE_TSC;
	if (cpu_features.basic_edx & CPU_FEATURE_MSR) f |= CPU_FEATURE_MSR;
	if (cpu_features.basic_edx & CPU_FEATURE_PAE) f |= CPU_FEATURE_PAE;
	if (cpu_features.basic_edx & CPU_FEATURE_CX8) f |= CPU_FEATURE_CX8;
	if (cpu_features.basic_edx & CPU_FEATURE_APIC) f |= CPU_FEATURE_APIC;
	if (cpu_features.basic_edx & CPU_FEATURE_PGE) f |= CPU_FEATURE_PGE;
	if (cpu_features.basic_edx & CPU_FEATURE_PAT) f |= CPU_FEATURE_PAT;
	if (cpu_features.basic_edx & CPU_FEATURE_NX) f |= CPU_FEATURE_NX;
	if (cpu_features.basic_edx & CPU_FEATURE_TSD_DEADLINE)
		f |= CPU_FEATURE_TSD_DEADLINE;
	if (cpu_features.basic_ecx & CPU_FEATURE_SSE) f |= CPU_FEATURE_SSE;
	if (cpu_features.basic_ecx & CPU_FEATURE_SSE2) f |= CPU_FEATURE_SSE2;
	if (cpu_features.extended_ecx & CPU_FEATURE_ERMS) f |= CPU_FEATURE_ERMS;
	if (cpu_features.extended_ecx & CPU_FEATURE_FSRM) f |= CPU_FEATURE_FSRM;
	if (cpu_features.extended_ecx & CPU_FEATURE_POPCNT) f |= CPU_FEATURE_POPCNT;
	if (cpu_features.extended_ecx & CPU_FEATURE_LZCNT) f |= CPU_FEATURE_LZCNT;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_AVX) f |= CPU_FEATURE_AVX;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_BMI1) f |= CPU_FEATURE_BMI1;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_AVX2) f |= CPU_FEATURE_AVX2;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_BMI2) f |= CPU_FEATURE_BMI2;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_ERMS2) f |= CPU_FEATURE_ERMS2;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_AVX512F) f |= CPU_FEATURE_AVX512F;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_AVX512DQ) f |= CPU_FEATURE_AVX512DQ;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_AVX512BW) f |= CPU_FEATURE_AVX512BW;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_AVX512VL) f |= CPU_FEATURE_AVX512VL;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_CLFLUSHOPT)
		f |= CPU_FEATURE_CLFLUSHOPT;
	if (cpu_features.leaf7_ebx & CPU_FEATURE_CLWB) f |= CPU_FEATURE_CLWB;
	if (cpu_features.leaf7_ecx & CPU_FEATURE_RDPID) f |= CPU_FEATURE_RDPID;
	if (cpu_features.leaf7_ecx & CPU_FEATURE_AVX_VNNI) f |= CPU_FEATURE_AVX_VNNI;
	if (cpu_features.leaf7_edx & CPU_FEATURE_SHA) f |= CPU_FEATURE_SHA;

	cpu_features.feature_mask = f;

	klog(KLOG_INFO, "cpu: family %u model %u stepping %u, %u logical processors, %u numa nodes\n",
	     cpu_features.family, cpu_features.model, cpu_features.stepping,
	     cpu_features.logical_cpus ? cpu_features.logical_cpus : 1,
	     cpu_features.numa_nodes);

	if (cpu_features.hypervisor)
		klog(KLOG_DEBUG, "cpu: running under a hypervisor\n");

	if (cpu_features.tsc_khz)
		klog(KLOG_INFO, "cpu: tsc %llu kHz (crystal ratio), invariant=%d\n",
		     (unsigned long long)cpu_features.tsc_khz,
		     cpu_features.invariant_tsc);

	klog(KLOG_INFO, "cpu: avx2=%d bmi2=%d erms=%d fsrm=%d clwb=%d rdpid=%d nx=%d\n",
	     cpu_has(CPU_FEATURE_AVX2), cpu_has(CPU_FEATURE_BMI2),
	     cpu_has(CPU_FEATURE_ERMS), cpu_has(CPU_FEATURE_FSRM),
	     cpu_has(CPU_FEATURE_CLWB), cpu_has(CPU_FEATURE_RDPID),
	     cpu_has(CPU_FEATURE_NX));

	/* Fail loudly rather than triple-fault later. Every subsystem in this
	 * kernel assumes long mode with PAE, and there is no useful degraded
	 * path that does not involve rewriting the memory manager. */
	if (!cpu_has(CPU_FEATURE_PAE))
		klog(KLOG_FATAL, "cpu: PAE is required and not present\n");
	if (!cpu_has(CPU_FEATURE_NX))
		klog(KLOG_WARN, "cpu: no NX support; user/kernel separation is weaker than intended\n");
}

const char *cpu_vendor(void)
{
	return cpu_features.vendor;
}
