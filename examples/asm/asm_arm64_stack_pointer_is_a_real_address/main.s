// [[D-ASM-ARM64-SP-AND-XZR-SHARE-ENCODING-31-SO-MOV-SP-SILENTLY-BECOMES-ZERO]]
// — the RUNNABLE witness.
//
// ★★★ WHAT WAS WRONG. AArch64 register field 31 names the STACK POINTER in
// add/subtract-immediate, in the extended-register forms and as a load/store
// BASE, and the ZERO REGISTER everywhere else. `sp` and `xzr` are two rows of
// the target's register table at that one number, and nothing said which one a
// field meant — so whichever row the operand resolved to reached the encoder,
// and the encoder wrote 31 either way.
//
// ✔MEASURED at the P55 base through the REAL CLI, every word read back with
// `aarch64-linux-gnu-objdump`:
//     mov x0, sp      -> 0xAA1F03E0 = `mov x0, xzr`   — x0 receives ZERO
//     mov sp, x0      -> 0xAA0003FF = `mov xzr, x0`   — a NO-OP
//     add x0, sp, x1  -> 0x8B0103E0 = `add x0, xzr, x1`
//     add sp, sp, x1  -> 0x8B0103FF — the stack pointer is never adjusted
//     cmp sp, x0      -> 0xEB0003FF = `cmp xzr, x0`
// rc=0, no diagnostic, wrong code. gas 2.42 and clang 18.1.3 — probed
// SEPARATELY and agreeing on all 128 probes of that census — emit 0x910003E0,
// 0x9100001F, 0x8B2163E0, 0x8B2163FF and 0xEB2063FF.
//
// ★★★ WHY THIS FILE EXISTS RATHER THAN A BYTE PIN, and the argument is
// STRONGER here than for most rows. `tests/asm/test_asm_arm64_shared_encoding_31.cpp`
// already asserts every emitted word. What a word cannot prove is that the
// value the program HOLDS is a real address: the whole defect was that a copy
// of the stack pointer silently became ZERO, and zero is a perfectly ordinary
// integer that arithmetic, a comparison and even a byte pin all accept. So
// every half below takes the stack pointer through a general register and then
// USES it — writing through one route and reading back through a SECOND,
// INDEPENDENTLY COMPUTED one. A copy that came back zero cannot survive that.
//
// ⚠ NOTHING SHIPPED CAUGHT THIS, which is why the corpus arm is mandatory: the
// existing arm64 assembly examples exercise `mov` only between general
// registers, where encoding 31 never appears at all.
//
// ★★ THE POISONS ARE CHOSEN SO A PARTIAL FAILURE CANNOT READ AS A PASS:
// 100 / 200 / 300 / 400 against the correct 6 / 9 / 13 / 14. Correct is 42;
// any single half failing gives 136, 233, 329 or 428, and no combination of
// the four lands back on 42. A stack pointer that moved when it should not
// have returns 99, which is none of those.
//
// ★ THE FRAME IS HAND-WRITTEN because this is the `encode` tier: no MIR module
// exists, so `materializeCallingConvention` does not run and the prologue and
// epilogue below are the programmer's own. 512 keeps sp 16-byte aligned and
// leaves the whole working area comfortably inside the frame; every address
// this file touches lies INSIDE it, so nothing is ever read below the stack
// pointer (AAPCS64 has no red zone). Only caller-saved registers (x0-x15) are
// touched, so no callee-saved register is clobbered.
	.text

	.globl	main
	.type	main, %function
main:
	sub	sp, sp, #512
	mov	x11, sp			// ← THE DEFECT'S OWN LINE: `mov Xd, SP`.
					//   At the P55 base this was `mov x11, xzr`
					//   and x11 became 0.

	// ── HALF A — THE COPY IS A REAL ADDRESS ───────────────────────────────
	// Write a value through the ORDINARY `[sp, #N]` route, then read it back
	// through an address computed FROM THE COPY. If `mov x11, sp` produced
	// zero, x12 is 0x100 and the load either faults or reads something that
	// is not 6.
	mov	x2, #100		// poison
	str	x2, [sp, #256]
	mov	x2, #6
	str	x2, [sp, #256]		// the real value, through sp itself
	add	x12, x11, #256		// ← the address, computed from the COPY
	ldr	x3, [x12]		// ← read back through the SECOND route

	// ── HALF B — THE EXTENDED-REGISTER ADD READS SP, NOT ZERO ─────────────
	// `add Xd, SP, Xm` is the EXTENDED-register encoding; the shifted one
	// cannot name SP at all. At the P55 base this emitted `add x13, xzr, x14`
	// — x13 became the OFFSET instead of the address, and the store below
	// would have written to 264. Written through the computed address and read
	// back through the plain `[sp, #N]` route: the mirror image of half A, so
	// neither half can cover for the other.
	mov	x2, #200		// poison
	str	x2, [sp, #264]
	mov	x14, #264
	add	x13, sp, x14		// ← THE EXTENDED-REGISTER FORM
	mov	x2, #9
	str	x2, [x13]		// write through the computed address
	ldr	x4, [sp, #264]		// read back through sp

	// ── HALF C — `mov SP, Xn` ACTUALLY WRITES THE STACK POINTER ───────────
	// At the P55 base `mov sp, x11` was a NO-OP, so a program that saved and
	// restored the stack pointer silently kept whatever sp already held. Both
	// slots are poisoned FIRST — the one the move should reach, and the one a
	// NO-OP would hit instead — so the two failure directions are separable.
	mov	x2, #300
	str	x2, [sp, #128]		// the slot the moved sp should write
	mov	x2, #301
	str	x2, [sp, #0]		// the slot a NO-OP `mov sp, x15` would hit
	add	x15, x11, #128
	mov	sp, x15			// ← `mov SP, Xn`: sp must MOVE
	mov	x2, #13
	str	x2, [sp, #0]		// lands at x11+128 iff the move happened
	mov	sp, x11			// ← restore: sp must move BACK
	ldr	x5, [sp, #128]		// 13 iff sp moved; 300 if it never did
	ldr	x6, [sp, #0]		// must still be 301 — a no-op would have
					//   written 13 here instead
	cmp	x6, #301
	b.ne	Lbroken

	// ── HALF D — `cmp SP, Xm` COMPARES THE STACK POINTER ──────────────────
	// The compare form is extended-register too. After the restore `sp` and
	// `x11` hold the same address, so this must be EQUAL; at the P55 base it
	// emitted `cmp xzr, x11`, comparing zero against an address, which never
	// is. ⚠ THIS ALSO RE-PROVES THE RESTORE: if `mov sp, x11` had been a
	// no-op, sp would be x11+128 here and the compare would differ.
	mov	x7, #400		// poison
	cmp	sp, x11			// ← THE EXTENDED-REGISTER COMPARE
	b.ne	Lsum
	mov	x7, #14
Lsum:
	// ── THE ANSWER ────────────────────────────────────────────────────────
	// 6 + 9 + 13 + 14 = 42. Every failure mode carries its own poison into
	// the sum, so which half broke is readable from the exit code alone.
	add	x0, x3, x4
	add	x0, x0, x5
	add	x0, x0, x7
	add	sp, x11, #512		// ⚠ THE EPILOGUE RESTORES FROM THE **COPY**,
					//   not from sp: a broken `mov SP, Xn`
					//   must not also corrupt the return path
					//   and turn a wrong answer into a crash.
	ret

Lbroken:
	// Half C wrote through the WRONG stack pointer. Return a value that is
	// neither 42 nor any poison sum, so the mode is unmistakable.
	mov	x0, #99
	add	sp, x11, #512
	ret
