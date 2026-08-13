	# The x86_64 AT&T dialect: the same control flow as the arm64 sibling,
	# plus memory operands. Comments here are `#` — which is the arm64
	# dialect's IMMEDIATE marker.
	.text

	.globl	main
	.type	main, @function
main:
	subq	$72, %rsp		# 16-byte aligned at the call, per the ABI
	movq	$100, %rax
	movq	$7, %rcx
	subq	%rcx, %rax		# 93 — two-address: rax = rax - rcx
	cmpq	$93, %rax
	jne	Lfail			# NOT taken; falls through to the unlabeled block
	movq	$2, %rcx
	imulq	%rcx, %rax		# 186
	movq	%rax, 40(%rsp)		# STORE through base + displacement
	movq	40(%rsp), %rdx		# LOAD  through base + displacement
	cmpq	$186, %rdx
	je	Lwin			# TAKEN
	jmp	Lfail			# second unlabeled block; never reached at runtime
Lwin:
	movq	$6, %r8
	movq	%rdx, (%rsp,%r8,8)	# STORE through base + index*scale  -> [rsp+48]
	movq	(%rsp,%r8,8), %r9	# LOAD  through base + index*scale
	movq	%r9, %rax
	movq	$144, %rcx
	call	helper			# rax = helper(rax=186, rcx=144) = 42
	addq	$72, %rsp
	ret
Lfail:
	movq	$1, %rax
	addq	$72, %rsp
	ret

	.globl	helper
	.type	helper, @function
helper:
	subq	%rcx, %rax
	ret
