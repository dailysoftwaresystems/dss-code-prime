// ★★★ CALL FRAME INFORMATION ON aarch64 — THE COUNTERPART THE ANCHOR SAID COULD
// NOT EXIST YET. D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED recorded, in its own
// words, that "the existing witness examples/asm/asm_x86_64_cfi_unwind_annotations
// has NO arm64 counterpart and cannot acquire one until the arm64 rows exist, so a
// green corpus says nothing about arm64 CFI today". The rows exist now, so this is
// that counterpart, and it is not a copy of the x86_64 file: the frame shape, the
// register numbers and the RETURN-ADDRESS COLUMN are all different.
//
// ★★★ THE ASYMMETRY THIS FILE EXISTS TO COVER. On x86_64 the `.cfi_*` spellings
// were declared and DROPPED (the defect). On aarch64 they were declared NOWHERE, so
// `.cfi_startproc` FAILED LOUD as an unknown directive — bar-compliant, and unable
// to read one function of `aarch64-linux-gnu-gcc -S`. The two ports sat on opposite
// sides of one row, so a fix that only repaired x86_64 would have left half the
// claim unwitnessed.
//
// ✔MEASURED 2026-08-17, `aarch64-linux-gnu-gcc 13.3.0 -O1 -S`: the compiler emits
// `.cfi_startproc`, `.cfi_def_cfa_offset 64`, `.cfi_offset 29, -16`,
// `.cfi_offset 30, -8`, `.cfi_remember_state`, `.cfi_restore 29`, `.cfi_restore 30`,
// `.cfi_def_cfa_offset 0`, `.cfi_restore_state`, `.cfi_endproc` — numeric operands,
// no `#` sigil. Every one of those appears below.
//
// ✔MEASURED the same day, `aarch64-linux-gnu-as` 2.42 + `readelf`: the aarch64 CIE
// prints `Return address column: 30` and `DW_CFA_def_cfa: r31 (sp) ofs 0`, where
// x86_64 prints column 16 and `rsp+8`. x30 IS an ordinary register that ALSO serves
// as the RA column; x86_64's column 16 is synthetic and belongs to no register. So
// `.cfi_offset 30, -8` below names the same column the CIE does — which is why the
// producer resolves a number against the target's RA column BEFORE its register
// table, and why doing it the other way round is invisible on x86_64 and wrong here.
//
// ★★ THE FRAME IS REAL, NOT DECORATIVE. `x30` (the link register) is genuinely
// spilled to the stack across the `bl`, so `.cfi_offset 30, -8` describes a save
// that ACTUALLY HAPPENED — an unwinder following it finds the true return address.
// A file that stated saves it never performed would still assemble and would still
// exit 42; it would simply describe a frame that does not exist, which is the class
// of defect this whole row is about.
	.text

	.globl	main
	.type	main, %function
main:
	.cfi_startproc
	.cfi_return_column 30		// 30 IS the aarch64 default; restating it is the no-op gcc emits
	sub	sp, sp, #32
	.cfi_def_cfa_offset 32
	str	x30, [sp, #24]		// spill the link register: bl overwrites it
	.cfi_offset 30, -8		// ...and say so: x30's entry value is at CFA-8
	.cfi_remember_state
	mov	x0, #100
	mov	x1, #7
	sub	x0, x0, x1		// 93
	mov	x1, #2
	mul	x0, x0, x1		// 186
	.cfi_restore_state
	mov	x1, #144
	bl	helper			// x0 = helper(186, 144) = 42
	ldr	x30, [sp, #24]
	.cfi_restore 30
	add	sp, sp, #32
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc

// ★ `helper` CARRIES THE SPELLINGS A COMPILER NEVER EMITS, so the per-ROW
// red-on-disable covers the whole honoured set rather than gcc's nine. They are
// exercised on a leaf whose real frame is empty, so nothing here claims a save that
// did not happen; the rules are about registers the caller does not rely on.
// ⚠ `.cfi_def_cfa 31, 0` names DWARF register 31 (`sp`) — ✔MEASURED, that is what
// `aarch64-linux-gnu-as` prints for `sp` — and 31 is NOT the RA column here, which
// on this port is 30.
	.globl	helper
	.type	helper, %function
helper:
	.cfi_startproc
	.cfi_def_cfa 31, 0
	.cfi_def_cfa_register 31
	.cfi_adjust_cfa_offset 0
	.cfi_offset 19, -16
	.cfi_rel_offset 19, 0
	.cfi_val_offset 19, -16
	.cfi_register 20, 21
	.cfi_same_value 20
	.cfi_undefined 21
	sub	x0, x0, x1
	.cfi_restore 19
	ret
	.cfi_endproc
