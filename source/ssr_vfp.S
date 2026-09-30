@ ssr_vfp.S -- int -> float conversions in VFP, for the engine's libgcc
@ __aeabi_i2f / __aeabi_ui2f (ssr_patch.c hooks them: ui2f branches into the
@ middle of i2f, so neither can be rewritten in place). softfp: the argument
@ and the result in r0. MIT.
	.syntax unified
	.arm
	.fpu	neon-fp-armv8
	.text

	.global	ssr_vfp_i2f
	.type	ssr_vfp_i2f, %function
ssr_vfp_i2f:
	vmov	s0, r0
	vcvt.f32.s32	s0, s0
	vmov	r0, s0
	bx	lr
	.size	ssr_vfp_i2f, . - ssr_vfp_i2f

	.global	ssr_vfp_ui2f
	.type	ssr_vfp_ui2f, %function
ssr_vfp_ui2f:
	vmov	s0, r0
	vcvt.f32.u32	s0, s0
	vmov	r0, s0
	bx	lr
	.size	ssr_vfp_ui2f, . - ssr_vfp_ui2f

	.section .note.GNU-stack,"",%progbits
