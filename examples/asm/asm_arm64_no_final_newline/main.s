// The LAST LINE of this file has no terminating newline, on purpose: its
// final byte is the `t` of `ret`. aarch64-linux-gnu-as 2.42 assembles it
// ("end of file not at end of a line; newline inserted"), clang 18.1.3
// accepts it silently, and DSS refused it until the dialect declared
// `endOfInputImplies` (D-ASM-LAST-LINE-WITHOUT-A-NEWLINE-REFUSED).
//
// The `ret` is load-bearing: a lost last line would let `main` fall off
// its end, so a green run proves the unterminated line was READ.
	.text
	.globl	main
	.type	main, %function
main:
	mov	x0, #100
	mov	x1, #58
	sub	x0, x0, x1
	ret