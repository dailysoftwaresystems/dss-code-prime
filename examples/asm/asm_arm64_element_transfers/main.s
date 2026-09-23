// ONE ELEMENT OF A VECTOR REGISTER, TO AND FROM A GENERAL REGISTER — `umov`,
// `ins` and their `mov` aliases at every element size, in a standalone aarch64
// `.s` (D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS).
//
// `v0.d[1]` names the high 64 bits of v0: a size suffix and an index. The
// P68 round-7 base refused every such line at the parser. Each check below
// writes a lane from a general register, reads it back into another, and
// compares — and the halfword and word writes are each followed by a re-read
// of a lane written BEFORE them, because `ins` writes one element and must keep
// the rest. Exit 11-16
// names the check that failed; 42 = 40 + 2, assembled from two 64-bit lanes.
	.text

	.globl	main
	.type	main, %function
main:
	mov	w5, #0x7f
	ins	v1.b[1], w5		// byte lane 1
	umov	w6, v1.b[1]
	cmp	w6, w5
	b.ne	.Lbad11
	mov	w5, #0xabc
	mov	v1.h[2], w5		// halfword lane 2 (bytes 4-5) — the `mov` alias
	umov	w6, v1.h[2]
	cmp	w6, w5
	b.ne	.Lbad12
	umov	w6, v1.b[1]		// the byte lane survived the halfword write
	cmp	w6, #0x7f
	b.ne	.Lbad13
	mov	w5, #0x5678
	ins	v1.s[3], w5		// word lane 3 (bytes 12-15)
	mov	w6, v1.s[3]		// the `mov` alias of umov, 32-bit element
	cmp	w6, w5
	b.ne	.Lbad14
	umov	w6, v1.h[2]		// the halfword lane survived the word write
	cmp	w6, #0xabc
	b.ne	.Lbad15
	mov	x1, #40
	mov	x2, #2
	ins	v0.d[0], x1
	mov	v0.d[1], x2		// v0 = {40, 2}
	umov	x3, v0.d[0]
	mov	x4, v0.d[1]		// the `mov` alias, 64-bit element
	cmp	x3, #40
	b.ne	.Lbad16
	add	x0, x3, x4		// 42
	ret
.Lbad11:
	mov	w0, #11
	ret
.Lbad12:
	mov	w0, #12
	ret
.Lbad13:
	mov	w0, #13
	ret
.Lbad14:
	mov	w0, #14
	ret
.Lbad15:
	mov	w0, #15
	ret
.Lbad16:
	mov	w0, #16
	ret
