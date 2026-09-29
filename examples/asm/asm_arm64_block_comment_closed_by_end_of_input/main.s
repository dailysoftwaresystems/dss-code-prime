// The file ENDS INSIDE a `/*` comment opened on the line of the final `ret`,
// and no newline follows it. aarch64-linux-gnu-as 2.42 assembles it,
// warning "end of file in multiline comment"; clang 18.1.3 refuses it. DSS
// accepts it with gas's warning, P_ClosedByEndOfInput, at the opener.
//
// The lines inside the comment are load-bearing: were the comment closed
// anywhere but the end of the input, the second `main:` would be a duplicate
// label; and the `ret` line is terminated only by the end of input, after
// the comment closes there.
	.text
	.globl	main
	.type	main, %function
main:
	mov	x0, #100
	mov	x1, #58
	sub	x0, x0, x1
	ret	/* never closed, so everything below is COMMENT:
main:
	mov	x0, #1