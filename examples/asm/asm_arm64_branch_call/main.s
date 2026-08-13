// The arm64 gas dialect: real control flow and a real call.
//
// Comments here are `//` — on the x86_64 AT&T sibling they are `#`, which is
// this file's IMMEDIATE marker. That one byte is why the two dialects cannot
// share a token table.
	.text

	.globl	main
	.type	main, %function
main:
	mov	x9, x30			// preserve the link register: bl overwrites it
	mov	x0, #100
	mov	x1, #7
	sub	x0, x0, x1		// 93 — three-address, unlike AT&T's two-address subq
	cmp	x0, #93
	b.ne	Lfail			// NOT taken; falls through to the unlabeled block
	mov	x1, #2
	mul	x0, x0, x1		// 186
	cmp	x0, #186
	b.eq	Lwin			// TAKEN
	b	Lfail			// second unlabeled block; never reached at runtime
Lwin:
	mov	x1, #144
	bl	helper			// x0 = helper(x0=186, x1=144) = 42
	mov	x30, x9
	ret
Lfail:
	mov	x0, #1
	mov	x30, x9
	ret

	.globl	helper
	.type	helper, %function
helper:
	sub	x0, x0, x1
	ret
