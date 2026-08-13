	# D-ASM-NEGATIVE-SCALAR-LOSES-ITS-SIGN — the RUNNABLE witness.
	#
	# Every value below is chosen so a DROPPED MINUS SIGN changes the exit
	# code, and changes it to a DIFFERENT number per half, so one broken half
	# cannot hide behind the other.
	.text

	.globl	main
	.type	main, @function
main:
	subq	$64, %rsp

	# HALF 1 — a negative IMMEDIATE. 100 + (-8) = 92; drop the sign and it
	# is 100 + 8 = 108.
	movq	$100, %rax
	movq	$-8, %rcx
	addq	%rcx, %rax

	# HALF 2 — a negative memory DISPLACEMENT, which is strictly worse than a
	# wrong constant because it is a wrong ADDRESS. rsp+24 is poisoned with 1;
	# `-8(%rdx)` (rdx = rsp+32) must overwrite it with 0. Drop the sign and
	# the store lands on rsp+40 instead, leaving the poison in place.
	movq	$1, %r9
	movq	%r9, 24(%rsp)
	leaq	32(%rsp), %rdx
	movq	$0, %r9
	movq	%r9, -8(%rdx)
	movq	24(%rsp), %r8
	addq	%r8, %rax

	# 92 correct · 108 immediate sign dropped · 93 displacement sign dropped
	# · 109 both.
	addq	$64, %rsp
	ret
