// `.L` LABELS AS BRANCH TARGETS, `cbz` / `cbnz`, AND THE 32-BIT IMMEDIATE
// ADD/SUB — three aarch64 gas spellings GNU as and clang assemble, and the
// P68 round-7 base refused (D-ASM-DOTTED-LABEL-AS-A-BRANCH-TARGET-REFUSED,
// D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS).
//
// Every `gcc -S` output branches to `.L` labels; before this, a dotted name in
// an instruction was "operand shape is not one this dialect binds to an operand
// role". `cbz`/`cbnz` were unknown mnemonics, and `add w0, w1, #2` had no
// encoding. The exit code is built from values that only the right branch
// and the right immediate produce: 42 = 40 (+2 by a W-form add) reached through
// three taken conditional branches, each skipping a line that would spoil it.
	.text

	.globl	main
	.type	main, %function
main:
	mov	w1, #40
	add	w0, w1, #2		// w0 = 42 — the W-form immediate add
	mov	x5, #0
	cbz	x5, .Lzero		// taken: x5 == 0
	mov	w0, #11			// skipped
.Lzero:
	mov	w7, #3
	cbnz	w7, .Lnonzero		// taken: w7 != 0
	mov	w0, #12			// skipped
.Lnonzero:
	sub	w2, w0, #4095		// w2 = 42 - 4095, wrapped at 32 bits
	add	w2, w2, #4095		// back to 42 — the W-form immediate subtract
	cmp	w2, w0
	b.ne	.Lbad
	b	.Ldone			// an unconditional branch to a dotted label
.Lbad:
	mov	w0, #13
.Ldone:
	ret
