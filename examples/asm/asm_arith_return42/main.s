	.text
	.globl	main
	.type	main, @function
main:
	movq	$100, %rax
	movq	$7, %rcx
	subq	%rcx, %rax
	movq	$2, %rdx
	imulq	%rdx, %rax
	movq	$144, %rcx
	xorq	%rcx, %rax
	ret
