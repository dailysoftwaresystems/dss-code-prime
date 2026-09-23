# The LAST LINE of this file has no terminating newline, on purpose: its
# final byte is the `t` of `ret`. GNU as 2.42 assembles it ("end of file
# not at end of a line; newline inserted"), clang 18.1.3 accepts it
# silently, and DSS refused it until the dialect declared
# `endOfInputImplies` (D-ASM-LAST-LINE-WITHOUT-A-NEWLINE-REFUSED).
#
# The `ret` is load-bearing: a lost last line would let `main` fall off
# its end, so a green run proves the unterminated line was READ, not
# merely tolerated.
	.text
	.globl	main
	.type	main, @function
main:
	movq	$100, %rax
	movq	$58, %rcx
	subq	%rcx, %rax
	ret