// A hand-written AArch64 jump table, taken at runtime, plus a data read-back.
//
// This file is the closing witness for D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET.
// Every claim it makes is an EXIT CODE, not a byte pattern:
// three interior labels get three DIFFERENT runtime addresses, the table slot
// that is read decides which one runs, and a separately-defined data object is
// read back and used in the arithmetic that produces the answer.
//
// ★ THE LABELS ARE PLAIN IDENTIFIERS (`Lzero`/`Lone`/`Ltwo`) RATHER THAN gas's
// `.L0`/`.L1`/`.L2`, and NOTHING about this fixture depends on the choice: a
// label's SPELLING is the dialect's business, while interiority is a property
// of where the label sits (inside a function, at a byte offset). Both are
// equally valid gas. Keeping the undotted form makes this example independent
// of the dot-prefixed-label work, which is a separate row.
	.data

// The jump table. Each slot is an ABSOLUTE 8-byte address of an INTERIOR
// label — a block inside `main`, not a function. Nothing in this file emits an
// instruction that mentions Lzero/Lone/Ltwo, which is exactly what makes this
// the hard case: the assembler's usual block-symbol binding rides on a
// block-address instruction, and there is none here.
	.globl	tbl
tbl:
	.quad	Lzero
	.quad	Lone
	.quad	Ltwo

// An ordinary data object, defined here and READ BACK below. Its value is
// load-bearing: 37 + 5 = 42.
	.globl	bias
bias:
	.quad	37

	.text

	.globl	main
	.type	main, %function
main:
	adr	x1, tbl			// the DATA symbol's address
	ldr	x0, [x1, #8]		// slot 1 -> the runtime address of Lone
	br	x0			// the indirect branch, through the table

// ⚠ EACH ARM HAS A DISTINCT WRONG ANSWER, so a mis-bound slot cannot hide:
//   * slot 0 read instead of slot 1  -> exit 10
//   * slot 2 read instead of slot 1  -> exit 20
//   * all three slots bound to one address -> whichever arm it is, and the
//     three exit codes are pairwise different
//   * a slot bound to the FUNCTION's start rather than the interior offset
//     -> `main` re-enters itself and loops or faults, never 42
//   * `bias` not read back (or read as its address) -> not 42
Lzero:
	mov	x0, #10
	ret
Lone:
	adr	x2, bias		// the second data symbol
	ldr	x2, [x2, #0]		// 37
	mov	x3, #5
	add	x0, x2, x3		// 42
	ret
Ltwo:
	mov	x0, #20
	ret
