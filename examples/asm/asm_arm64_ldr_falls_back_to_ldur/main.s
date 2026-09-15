// [[D-ASM-ARM64-LDR-TO-LDUR-CONVENIENCE-ALIAS-REFUSED]] — the RUNNABLE witness.
//
// gas spells ONE load with two encodings. `ldr` is the SCALED unsigned-offset
// form (imm12, scaled by the access size, non-negative only); `ldur` is the
// UNSCALED signed one (imm9, raw bytes, -256..255). A displacement the scaled
// form cannot represent does NOT make `ldr` an error: both references quietly
// assemble it as the unscaled encoding, with no warning.
//
// ✔MEASURED 2026-09-03, gas 2.42 and clang 18.1.3 probed SEPARATELY and
// agreeing on every cell, each word read back with `aarch64-linux-gnu-objdump`:
//     ldr  x0,[x1,#1]   -> 0xF8401020  = ldur x0,[x1,#1]
//     str  x0,[x1,#1]   -> 0xF8001020  = stur x0,[x1,#1]
//     ldr  d0,[x1,#-8]  -> 0xFC5F8020  = ldur d0,[x1,#-8]
//     ldr  q0,[x1,#-16] -> 0x3CDF0020  = ldur q0,[x1,#-16]
// while DSS refused every one of them with A_ImmediateOperandOutOfRange.
//
// ★★★ WHY THIS FILE EXISTS RATHER THAN A BYTE PIN. A unit test can assert the
// emitted WORD, and the file `tests/asm/test_asm_arm64_memory_dialect_rows.cpp`
// does exactly that for all 35 measured cells. What a word cannot prove is that
// the instruction addresses the memory the programmer NAMED — and that is the
// whole risk of this change, because the two encodings read their displacement
// field differently: `ldr`'s is SCALED by the access size and `ldur`'s is a RAW
// byte count. An offset routed to the wrong one is not a refusal; it is a
// perfectly valid instruction at the WRONG ADDRESS. So every half below writes
// through one route and reads through a SECOND, INDEPENDENTLY COMPUTED address,
// and every target slot is POISONED first with a distinctive value.
//
// ★★ THE POISONS ARE CHOSEN SO A PARTIAL FAILURE CANNOT READ AS A PASS:
// 100 / 200 / 300 against the correct 7 / 11 / 52. Correct is 70; any single
// half failing gives 163, 3 or 62, and no combination lands back on 70.
//
// ★ THE FRAME IS HAND-WRITTEN because this is the `encode` tier: no MIR module
// exists, so `materializeCallingConvention` does not run and the prologue and
// epilogue below are the programmer's own. 512 keeps sp 16-byte aligned and
// leaves x1±32 comfortably inside the frame. Only caller-saved registers
// (x0-x9, q0) are touched, so no AAPCS64 callee-saved register is clobbered.
	.text

	.globl	main
	.type	main, %function
main:
	sub	sp, sp, #512
	add	x1, sp, #256		// mid-frame anchor: x1±32 is inside the frame

	// ── HALF A — the LOAD rewrite, at a NON-MULTIPLE offset ───────────────
	// `ldr x3, [x1, #1]` is an 8-byte load at a 1-byte displacement: 1 is not
	// a multiple of 8, so the scaled field cannot express it and the assembler
	// must reach for the unscaled form. DSS refused this line before P55.
	// The value is placed through x4, computed independently by `add`, so the
	// read is a test of the ADDRESS and not of a round trip with itself.
	add	x4, x1, #1
	mov	x2, #100		// poison
	stur	x2, [x4]
	mov	x2, #7
	stur	x2, [x4]		// the real value, same independent route
	ldr	x3, [x1, #1]		// ← THE REWRITE (load direction)

	// ── HALF B — the STORE rewrite, at a NEGATIVE offset that IS a multiple ─
	// -8 is a multiple of 8, and still has no scaled encoding: the imm12 field
	// is UNSIGNED. This is the case the anchor row's own premise missed — it
	// framed the trigger as "not a multiple of the access size", and this line
	// is a multiple. Written through the rewrite, read back through x5.
	sub	x5, x1, #8
	mov	x2, #200		// poison
	stur	x2, [x5]
	mov	x2, #11
	str	x2, [x1, #-8]		// ← THE REWRITE (store direction)
	ldur	x6, [x5]

	// ── HALF C — the SIMD&FP q-form, negative offset ──────────────────────
	// The same question on the other register file and at the widest access:
	// 16 bytes at -32. Build a known q value in the frame, store it through
	// the rewrite, and read its low half back through an independent address.
	add	x7, x1, #64		// scratch, 16-byte aligned relative to x1
	mov	x2, #52
	stur	x2, [x7]
	mov	x2, #0
	stur	x2, [x7, #8]
	ldr	q0, [x7]		// q0 = { 52, 0 }

	sub	x9, x1, #32
	mov	x2, #300		// poison the low half of the destination
	stur	x2, [x9]
	str	q0, [x1, #-32]		// ← THE REWRITE (q form, negative)
	ldur	x8, [x9]

	// ── THE ANSWER ────────────────────────────────────────────────────────
	// 7 + 11 + 52 = 70. Every failure mode carries its own poison into the
	// sum, so which half broke is readable from the exit code alone.
	add	x0, x3, x6
	add	x0, x0, x8

	add	sp, sp, #512
	ret
