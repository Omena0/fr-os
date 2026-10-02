	.text
# --- AVX / AVX2 (VEX) -----------------------------------------------------
	vaddps	%ymm0, %ymm1, %ymm2
	vaddss	%xmm0, %xmm1, %xmm2
	vandps	%ymm0, %ymm1, %ymm2
	vandnps	%ymm0, %ymm1, %ymm2
	vorps	%ymm0, %ymm1, %ymm2
	vxorps	%ymm0, %ymm1, %ymm2
	vmovaps	%ymm0, %ymm1
	vmovups	%ymm0, %ymm1
	vmovdqu	%ymm0, %ymm1
	vmovdqa	%xmm0, %xmm1
	vmovd	%xmm0, %xmm1
	vmovq	%xmm0, %xmm1
	vmovmskps	%ymm0, %eax
	vpmovmskb	%ymm0, %eax
	vpand	%ymm0, %ymm1, %ymm2
	vpandn	%ymm0, %ymm1, %ymm2
	vpor	%ymm0, %ymm1, %ymm2
	vpxor	%ymm0, %ymm1, %ymm2
	vpaddb	%ymm0, %ymm1, %ymm2
	vpaddq	%ymm0, %ymm1, %ymm2
	vpsubq	%ymm0, %ymm1, %ymm2
	vpcmpeqb	%ymm0, %ymm1, %ymm2
	vpcmpgtq	%ymm0, %ymm1, %ymm2
	vpshufb	%ymm0, %ymm1, %ymm2
	vpshufd	$0x1b, %ymm0, %ymm1
	vunpcklps	%ymm0, %ymm1, %ymm2
	vperm2i128	$0x31, %ymm0, %ymm1, %ymm2
	vextracti128	$1, %ymm0, %xmm1
	vinserti128	$1, %xmm1, %ymm0, %ymm2
	vbroadcastsd	(%rdi), %ymm0
	vpbroadcastb	%xmm0, %ymm1
	vpmulld	%ymm0, %ymm1, %ymm2
	vpmuldq	%ymm0, %ymm1, %ymm2
	vpmuludq	%ymm0, %ymm1, %ymm2
	vsqrtps	%ymm0, %ymm1
	vdivpd	%ymm0, %ymm1, %ymm2
	vcvtdq2ps	%ymm0, %ymm1
	vcvtsi2sd	%rax, %xmm0, %xmm1
	vfmadd132ps	%ymm0, %ymm1, %ymm2
	vfmsub213sd	%xmm0, %xmm1, %xmm2
	vfnmadd231ps	%ymm0, %ymm1, %ymm2
	vcmpps	$0x1, %ymm0, %ymm1, %ymm2
	vcmppd	$0x4, %ymm0, %ymm1, %ymm2
	vptest	%ymm0, %ymm1
	vblendvps	%ymm0, %ymm1, %ymm2, %ymm3
	vroundps	$0x3, %ymm0, %ymm1
	vaesenc	%ymm0, %ymm1, %ymm2
	vaesenclast	%xmm0, %xmm1, %xmm2
	vpclmulqdq	$0x1, %ymm0, %ymm1, %ymm2
	vzeroupper
	vzeroall
	vlddqu	(%rdi), %ymm0
	vmaskmovps	(%rdi), %ymm0, %ymm1
	vmovaps	16(%rdi), %ymm0
	vaddpd	%ymm0, %ymm1, %ymm2

# --- AVX-512 (EVEX) --------------------------------------------------------
	kmovw	%k1, %k2
	kmovb	%k3, (%rdi)
	kortestw	%k1, %k2
	kandw	%k1, %k2, %k3
	knotw	%k1, %k2
	korw	%k1, %k2, %k3
	kunpckbw	%k1, %k2, %k3
	kaddb	%k1, %k2, %k3
	vmovdqu32	%zmm0, %zmm1
	vmovdqu64	(%rdi), %zmm2
	vmovaps	%zmm0, %zmm1
	kmovw	%k1, %eax
	vpaddq	%zmm0, %zmm1, %zmm2
	vpcmpeqd	%zmm0, %zmm1, %k2
	vfmadd231ps	%zmm0, %zmm1, %zmm2
	vrcp14ps	%zmm0, %zmm1
	vpternlogd	$0x96, %zmm0, %zmm1, %zmm2
	vpcompressd	%zmm0, %zmm1
	vpexpandd	%zmm0, %zmm1
	vpermt2d	%zmm0, %zmm1, %zmm2
	vmovdqu32	%zmm0, %zmm1{%k1}
	vpdpbusd	%zmm0, %zmm1, %zmm2
	ret
