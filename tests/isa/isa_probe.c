/*
 * isa_probe.c -- a source whose generated code moves with the -m flags, used
 * by tests/isa/verify_isa_test.sh to check the ISA gate end to end.
 *
 * Three things about it are deliberate:
 *
 *   - Every aggregate is 32 or 64 bytes wide, so the code generator picks a
 *     different instruction for the x86-64 baseline (SSE2, 128-bit), for
 *     -msse2 (still 128-bit) and for -march=x86-64-v3 (AVX2, 256-bit). That
 *     progression is the whole test: the same source has to come out clean,
 *     clean, and rejected, and a probe that only exercised the first case
 *     would not be testing anything.
 *
 *   - Nothing is returned in a floating-point register and nothing is read
 *     from a floating-point argument. KERNEL_CFLAGS has -mno-sse2 -msoft-float,
 *     under which "SSE register return with SSE disabled" is a hard error, so a
 *     probe with a float-returning function could not be compiled with the
 *     flag list it is meant to test. The results are written through pointers
 *     instead, which is both compilable under every flag list in config.mk and
 *     enough to make the vectoriser reach for a wide register.
 *
 *   - It is ordinary freestanding C with no headers, no intrinsics and no
 *     inline asm. The point is to test what the *flags* permit, not what a
 *     programmer can write by hand.
 */

typedef unsigned long u64;

struct wide {
	u64 a[8];
	double d[4];
	float f[8];
};

void copy64(struct wide *dst, const struct wide *src)
{
	*dst = *src;
}

u64 sum_a(const struct wide *p)
{
	u64 s = 0;
	for (int i = 0; i < 8; i++)
		s += p->a[i];
	return s;
}

/*
 * Floating-point arithmetic is the reliable way to make a compiler emit
 * scalar SSE, so it is what this probe uses. It is guarded because the same
 * file is also compiled with the kernel's own flags, which are -mno-sse: a
 * function returning or taking a double cannot even be compiled without SSE,
 * because the ABI passes and returns it in %xmm0.
 *
 * The guard is the point rather than an inconvenience: the classifier has to
 * distinguish "this object contains no vector instruction" from "this object
 * could not be built", and the probe is compiled both ways to prove it.
 */
#ifdef __SSE__
void sum_d(const struct wide *p, double *out)
{
	double s = 0;
	for (int i = 0; i < 4; i++)
		s += p->d[i];
	*out = s;
}

void sum_f(const struct wide *p, float *out)
{
	float s = 0;
	for (int i = 0; i < 8; i++)
		s += p->f[i];
	*out = s;
}
#endif /* __SSE__ */

u64 mix(u64 x)
{
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdUL;
	x ^= x >> 29;
	return x;
}

#ifdef __SSE__
long convert(u64 a, long b, int c, long *out)
{
	/*
	 * The cast through double is lossy for values above 2^53, but this is an
	 * ISA probe: the result is only used to exercise the SSE path, not to
	 * carry real data. Compute the integer part once rather than twice.
	 */
	long base = b + c;

	*out = base;
	return base + (long)(double)a;
}
#endif /* __SSE__ */
