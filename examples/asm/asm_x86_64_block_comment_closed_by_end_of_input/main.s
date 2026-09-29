# The file ENDS INSIDE a `/*` comment opened on the line of the final `ret`,
# and no newline follows it. GNU as 2.42 assembles such a file, warning
# "end of file in comment"; clang 18.1.3 refuses it. DSS accepts it with
# gas's warning, P_ClosedByEndOfInput, reported at the opener.
#
# The two lines inside the comment are load-bearing: were the comment closed
# anywhere but the end of the input, the second `main:` would be a duplicate
# label. And the `ret` line is terminated only by the end of input, AFTER the
# comment closes there, exactly as gas inserts its newline.
	.text
	.globl	main
	.type	main, @function
main:
	movq	$100, %rax
	movq	$58, %rcx
	subq	%rcx, %rax
	ret	/* never closed, so everything below is COMMENT:
main:
	movq	$1, %rax