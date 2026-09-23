// GNU as's NUMERIC LOCAL LABELS (D-ASM-LABELS-INSIDE-A-TEMPLATE-AND-NUMERIC-LOCAL-LABELS-REFUSED)
// on the second dialect — the evidence that the resolver is shared, not an
// x86 quirk. `N:` may be defined any number of times; `Nb` names the nearest
// `N:` BEFORE the reference and `Nf` the nearest AFTER it. The shapes, each
// load-bearing: a loop on `1:` / `1b`; `b 9f` written after one `9:`, which
// must reach the NEXT `9:`; `b.lt 9b` looping on the nearest `9:`; label 0
// through `0f` / `0b` beside the binary literal `#0b10101` (21).
// aarch64-linux-gnu-as 2.42 and clang 18.1.3 both run this to 42 under qemu
// (measured 2026-09-23).
	.text
	.globl	main
	.type	main, %function
main:
	mov	w0, #0
	mov	w1, #3
1:	add	w0, w0, #2
	sub	w1, w1, #1
	cbnz	w1, 1b
9:	add	w0, w0, #1
	cmp	w0, #7
	b.ne	8f
	b	9f
9:	add	w0, w0, #7
	cmp	w0, #21
	b.lt	9b
	b	0f
0:	add	w0, w0, #0b10101
	cmp	w0, #42
	b.gt	8f
	b.lt	0b
	ret
8:	mov	w0, #1
	ret
