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
 * Set one feature ID if the named CPUID register has the named bit set.
 *
 * The register and the bit are written out at every call site rather than being
 * implied by the constant. That is the whole point: a constant that also carries
 * its CPUID position invites exactly the bug this file had, where the constant
 * said one register and the code read another, and nothing complained because
 * both happened to be a plausible-looking `1u << n`.
 */
static inline void feat_set(uint64_t *f, uint32_t reg, uint32_t bit, uint64_t id)
{
	if (reg & (1u << bit))
		*f |= id;
}

#define FEAT(reg, bit, id) feat_set(&f, (reg), (bit), (id))

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
		/*
		 * EBX[23:16] is only defined when the hyper-threading bit
		 * (EDX 28) is set; on a machine without it the field is reserved
		 * and reads 0. Normalising here rather than at the print site is
		 * deliberate: there are two printers of this field — this file and
		 * main.c — and a `?: 1` fallback in only one of them is how two
		 * prints of one field came to disagree within a single boot.
		 */
		cpu_features.logical_cpus = (b >> 16) & 0xFF;

		if (!cpu_features.logical_cpus)
			cpu_features.logical_cpus = 1;
	}

	cpuid_raw(CPUID_MAX_EXT_LEAF, 0, &max_ext, &b, &c, &d);
	cpu_features.max_ext_leaf = max_ext;

	if (max_ext >= CPUID_EXT_FEATURES) {
		cpuid_raw(CPUID_EXT_FEATURES, 0, &a, &b, &c, &d);
		cpu_features.extended_ecx = c;
		cpu_features.extended_edx = d;
	}

	/*
	 * Invariant TSC is CPUID.0x80000007 EDX bit 8.
	 *
	 * It is not CPUID.0x80000001 EDX bit 8, which is reserved and reads zero
	 * — that is where this used to be read from, so `invariant_tsc` was
	 * permanently false. It was invisible because the only print of it is
	 * inside the `if (tsc_khz)` branch below, and tsc_khz was itself always
	 * zero, so no boot had ever reached it.
	 */
	if (max_ext >= 0x80000007u) {
		cpuid_raw(0x80000007u, 0, &a, &b, &c, &d);
		cpu_features.invariant_tsc = (d >> 8) & 1;
	}

	/* Structured extended features. Leaf 7 subleaf 0 reports the maximum
	 * input value for the leaf in EAX, so the subleaf walk has to be bounded
	 * rather than trusting a fixed count. Only subleaf 0 is kept; the walk
	 * exists to learn the bound, and the features below are all subleaf 0. */
	if (max_leaf >= 7) {
		uint32_t sub = 0;
		uint32_t max_sub;

		cpuid_raw(7, 0, &max_sub, &b, &c, &d);

		if (max_sub > 0) {
			uint32_t max_clamped = max_sub > 8 ? 8 : max_sub;

			while (sub <= max_clamped) {
				cpuid_raw(7, sub, &a, &b, &c, &d);
				if (sub == 0) {
					cpu_features.leaf7_ebx = b;
					cpu_features.leaf7_ecx = c;
					cpu_features.leaf7_edx = d;
				}
				sub++;
			}
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

	/* TSC frequency. CPUID 0x15 reports a crystal ratio where available. The comment
	 * used to promise a fallback "to CPUID 0x16 and finally to calibration
	 * against the PIT" that did not exist in the code; 0x16 reports a base
	 * frequency, not the TSC, and a PIT calibration needs a delay loop that
	 * has no place in code that runs before the scheduler exists. So there is
	 * one source, and tsc_khz is 0 when it does not answer — which klog
	 * already handles by printing no timestamp rather than dividing by it. */
	cpu_features.tsc_khz = 0;
	if (max_leaf >= CPUID_TSC) {
		uint32_t denom = 0, numer = 0, ecx = 0;

		cpuid_raw(CPUID_TSC, 0, &a, &denom, &numer, &ecx);
		/*
		 * Leaf 0x15: EAX is the crystal clock in Hz, EBX the denominator
		 * and ECX the numerator of the TSC ratio.
		 *
		 * The old code read the multiplier from EDX, which this leaf does
		 * not define and which comes back 0, so tsc_khz was 0 on every
		 * machine and every klog timestamp read [    0.000 ...].
		 */
		if (a != 0 && denom != 0 && numer != 0)
			cpu_features.tsc_khz = (uint64_t)a * numer / denom;
	}

	cpu_features.hypervisor = (cpu_features.basic_ecx >> 31) & 1;
	cpu_features.apic = (cpu_features.basic_edx >> 9) & 1;
	cpu_features.tsc_deadline_timer = (cpu_features.basic_ecx >> 24) & 1;
	/*
	 * 1 GiB pages live at 0x80000001 EDX bit 26. vmm.c reads
	 * has_1gb_pages before mapping a PDPT entry with PS=1, and nothing ever
	 * set the field, so the 1 GiB path was dead on every machine and main.c
	 * printed has_1gb_pages=0 as though the CPU had been asked.
	 */
	cpu_features.has_1gb_pages = (cpu_features.extended_edx >> 26) & 1;

	/*
	 * The feature mask: one bit per feature ID, grouped by the CPUID leaf
	 * register the feature comes from. Each line below names the register and
	 * the CPUID bit explicitly rather than letting a constant imply them —
	 * the old code folded four leaves into one bit namespace by reusing the
	 * CPUID bit number as the mask bit, so nine bit positions carried two or
	 * three different meanings and a feature from one leaf answered for a
	 * feature from another. Seven of the old constants also named a register
	 * the code never read (AVX, SSE, SSE2, ERMS, ERMS2, FSRM, SHA), so those
	 * bits were reporting whatever unrelated feature happened to occupy the
	 * same position. See the block comment in cpu_features.h.
	 */
	uint64_t f = 0;

	/* CPUID leaf 1, EDX */
	FEAT(cpu_features.basic_edx, 0, CPU_FEATURE_FPU);
	FEAT(cpu_features.basic_edx, 4, CPU_FEATURE_TSC);
	FEAT(cpu_features.basic_edx, 5, CPU_FEATURE_MSR);
	FEAT(cpu_features.basic_edx, 6, CPU_FEATURE_PAE);
	FEAT(cpu_features.basic_edx, 7, CPU_FEATURE_MCE);
	FEAT(cpu_features.basic_edx, 8, CPU_FEATURE_CX8);
	FEAT(cpu_features.basic_edx, 9, CPU_FEATURE_APIC);
	FEAT(cpu_features.basic_edx, 11, CPU_FEATURE_SEP);
	FEAT(cpu_features.basic_edx, 12, CPU_FEATURE_MTRR);
	FEAT(cpu_features.basic_edx, 13, CPU_FEATURE_PGE);
	FEAT(cpu_features.basic_edx, 16, CPU_FEATURE_PAT);
	FEAT(cpu_features.basic_edx, 19, CPU_FEATURE_CLFLUSH);
	FEAT(cpu_features.basic_edx, 24, CPU_FEATURE_FXSR);
	FEAT(cpu_features.basic_edx, 25, CPU_FEATURE_SSE);
	FEAT(cpu_features.basic_edx, 26, CPU_FEATURE_SSE2);
	FEAT(cpu_features.basic_edx, 28, CPU_FEATURE_HT);

	/* CPUID leaf 1, ECX */
	FEAT(cpu_features.basic_ecx, 0, CPU_FEATURE_SSE3);
	FEAT(cpu_features.basic_ecx, 13, CPU_FEATURE_CX16);
	FEAT(cpu_features.basic_ecx, 23, CPU_FEATURE_POPCNT);
	FEAT(cpu_features.basic_ecx, 24, CPU_FEATURE_TSC_DEADLINE);
	FEAT(cpu_features.basic_ecx, 26, CPU_FEATURE_XSAVE);
	/*
	 * ECX bit 27 is AVX; bit 28 is OSXSAVE. These were the other way round,
	 * which made every "can this CPU do X" question in the tree answer the
	 * wrong thing, and one of the answers was load-bearing.
	 *
	 * stage2_long.S gates its XSETBV on ECX.27 && ECX.28. With the bits
	 * swapped, cpu_features reported OSXSAVE=0 and AVX=1, so the loader's gate
	 * -- correctly written -- was skipped and XCR0 was never written on any
	 * machine. Measured three ways on this host: the boot log's feature mask has
	 * the OSXSAVE bit clear and the AVX bit set, a `-d int` register dump shows
	 * CR4.OSXSAVE clear, and an exec trace contains no xsetbv at all.
	 *
	 * The error direction matters. Swapped this way, `cpu_has(CPU_FEATURE_AVX)`
	 * answers "can the OS use XCR0", so a CPU with XSAVE and OSXSAVE but no AVX
	 * -- a KNL, or a hypervisor masking the bit -- passes a check meant to stop
	 * it. The other way round merely under-reports and disables features, which
	 * is safe. The mask keeps its bit *IDs*; only the mapping changes, so no
	 * stored mask is invalidated.
	 */
	FEAT(cpu_features.basic_ecx, 27, CPU_FEATURE_AVX);
	FEAT(cpu_features.basic_ecx, 28, CPU_FEATURE_OSXSAVE);
	FEAT(cpu_features.basic_ecx, 31, CPU_FEATURE_HYPERVISOR);

	/* CPUID 0x80000001, ECX */
	FEAT(cpu_features.extended_ecx, 0, CPU_FEATURE_LAHF_LAM);
	FEAT(cpu_features.extended_ecx, 5, CPU_FEATURE_ABM);
	FEAT(cpu_features.extended_ecx, 6, CPU_FEATURE_SSE4A);
	FEAT(cpu_features.extended_ecx, 9, CPU_FEATURE_OSVW);

	/*
	 * CPUID 0x80000001, EDX. NX is bit 20 of THIS register. Testing bit 20 of
	 * leaf 1 EDX instead — which is reserved and reads 0 on every part ever
	 * made — is what made the kernel print "no NX support" on machines plainly
	 * running with EFER.NXE set.
	 */
	FEAT(cpu_features.extended_edx, 11, CPU_FEATURE_SYSCALL);
	FEAT(cpu_features.extended_edx, 19, CPU_FEATURE_MP);
	FEAT(cpu_features.extended_edx, 20, CPU_FEATURE_NX);
	FEAT(cpu_features.extended_edx, 22, CPU_FEATURE_MMXEXT);
	FEAT(cpu_features.extended_edx, 25, CPU_FEATURE_FXSR_OPT);
	FEAT(cpu_features.extended_edx, 26, CPU_FEATURE_GBPAGES);
	FEAT(cpu_features.extended_edx, 27, CPU_FEATURE_RDTSCP);
	FEAT(cpu_features.extended_edx, 29, CPU_FEATURE_LM);

	/* CPUID leaf 7 subleaf 0, EBX */
	FEAT(cpu_features.leaf7_ebx, 0, CPU_FEATURE_FSGSBASE);
	FEAT(cpu_features.leaf7_ebx, 3, CPU_FEATURE_BMI1);
	FEAT(cpu_features.leaf7_ebx, 4, CPU_FEATURE_HLE);
	FEAT(cpu_features.leaf7_ebx, 5, CPU_FEATURE_AVX2);
	FEAT(cpu_features.leaf7_ebx, 7, CPU_FEATURE_SMEP);
	FEAT(cpu_features.leaf7_ebx, 8, CPU_FEATURE_BMI2);
	FEAT(cpu_features.leaf7_ebx, 9, CPU_FEATURE_ERMS);
	FEAT(cpu_features.leaf7_ebx, 10, CPU_FEATURE_INVPCID);
	FEAT(cpu_features.leaf7_ebx, 11, CPU_FEATURE_RTM);
	FEAT(cpu_features.leaf7_ebx, 16, CPU_FEATURE_AVX512F);
	FEAT(cpu_features.leaf7_ebx, 17, CPU_FEATURE_AVX512DQ);
	FEAT(cpu_features.leaf7_ebx, 23, CPU_FEATURE_CLFLUSHOPT);
	FEAT(cpu_features.leaf7_ebx, 24, CPU_FEATURE_CLWB);
	FEAT(cpu_features.leaf7_ebx, 29, CPU_FEATURE_SHA);
	FEAT(cpu_features.leaf7_ebx, 30, CPU_FEATURE_AVX512BW);
	FEAT(cpu_features.leaf7_ebx, 31, CPU_FEATURE_AVX512VL);

	/* CPUID leaf 7 subleaf 0, ECX */
	FEAT(cpu_features.leaf7_ecx, 16, CPU_FEATURE_LA57);
	FEAT(cpu_features.leaf7_ecx, 22, CPU_FEATURE_RDPID);

	/* CPUID leaf 7 subleaf 0, EDX */
	FEAT(cpu_features.leaf7_edx, 4, CPU_FEATURE_FSRM);
	FEAT(cpu_features.leaf7_edx, 14, CPU_FEATURE_SERIALIZE);

	cpu_features.feature_mask = f;

	klog(KLOG_INFO, "cpu: family %u model %u stepping %u, %u logical processors, %u numa nodes\n",
	     cpu_features.family, cpu_features.model, cpu_features.stepping,
	     cpu_features.logical_cpus, cpu_features.numa_nodes);

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
