// D-ASM-ARM64-DIALECT-INVERTS-LDR-LDUR-MAPPING — the RUNNABLE witness.
//
// gas has FOUR load/store spellings for arm64 and DSS has four opcodes, and
// the dialect used to pair them backwards. This file exercises BOTH ends of
// the pairing, with an offset that only ONE of the two encodings can carry:
//
//   * `stur`/`ldur` = UNSCALED, signed imm9, reach -256..255  → DSS load/store
//   * `str`/`ldr`   = SCALED,   unsigned imm12, reach 0..32760 → DSS load_u/store_u
//
// so `#-8` is encodable ONLY by the unscaled form and `#512` is beyond the
// unscaled form's reach. Swap the four rows back and neither half assembles.
	.text

	.globl	main
	.type	main, %function
main:
	sub	sp, sp, #1024
	add	x1, sp, #256		// mid-frame anchor, so x1-8 is still inside the frame

	// Both target slots are POISONED FIRST, through addressing that does not
	// use a displacement at all (`[x4]` / `[x6]`, offset 0). That is what makes
	// this a test of the ADDRESS rather than of the round trip: if a store
	// lands somewhere other than where the matching read looks, the poison
	// survives and shows up in the exit code.
	sub	x4, x1, #8
	mov	x2, #200
	stur	x2, [x4]
	add	x6, x1, #512
	mov	x2, #50
	stur	x2, [x6]

	mov	x0, #100

	// HALF A — UNSCALED, NEGATIVE. `#-8` has no representation in the scaled
	// form's UNSIGNED imm12, so the inverted mapping refused this line outright
	// (A_ImmediateOperandOutOfRange). Read back through x4, computed
	// independently by `sub`, so the check is not self-consistent-by-accident.
	mov	x2, #7
	stur	x2, [x1, #-8]
	ldr	x3, [x4]
	sub	x0, x0, x3		// 100 - 7 = 93

	// HALF B — SCALED, LARGE. 512 is beyond the unscaled form's 255-byte
	// reach, so this is the half that catches the inversion in the OTHER
	// direction — the dangerous one, where a `ldr`/`str` wrongly routed to the
	// unscaled encoder silently reads the offset as a raw byte count.
	mov	x2, #23
	str	x2, [x1, #512]
	ldur	x7, [x6]
	sub	x0, x0, x7		// 93 - 23 = 70

	// 70 correct · 156 half A stored to the wrong address (100-200 truncated)
	// · 43 half B stored to the wrong address · 106 both.
	add	sp, sp, #1024
	ret
