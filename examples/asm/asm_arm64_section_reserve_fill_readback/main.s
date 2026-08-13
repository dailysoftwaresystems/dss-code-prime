// `.section NAME` and the reserve-with-fill family, READ BACK AT RUNTIME —
// the AArch64 half.
//
// ★★★ THE POINT OF THIS FILE IS THAT EVERY BYTE IT RESERVES IS LOADED AGAIN
// BY THE PROGRAM THAT RESERVED IT. A directive that merely EMITS is proven by
// a unit test; a directive whose bytes have to survive the lowering, the
// section assignment, the linker's layout and the loader's mapping is proven
// only by an exit code. This repo has the scar: a lowering computed
// `dataItems` and the driver dropped them on the floor — the mutant compiled
// clean, produced an artifact, and only an image-level witness caught it.
//
// ★★ THE SECOND DIALECT IS THE WHOLE REASON THIS FILE EXISTS SEPARATELY FROM
// ITS x86_64 SIBLING. `.section` / `.space` / `.zero` / `.skip` are declared
// in BOTH `.lang.json` documents and handled by ONE engine, so a mechanism
// witnessed by one dialect is the weaker claim this project keeps having to
// walk back. The DIRECTIVE text below is byte-identical to the sibling's; the
// INSTRUCTIONS are this CPU's, which is exactly the seam the verb/spelling
// split is supposed to sit on.
//
// ⚠ COMMENTS HERE ARE `//` AND `#` IS THE IMMEDIATE MARKER — the sibling
// spells it the other way round. One byte, two dialects, no shared token
// table.

	// ── read-only data, reached by NAME ──────────────────────────────
	// `.rodata` is not a gas directive: a bare `.rodata` is an unknown
	// pseudo-op and only `.section .rodata` assembles. So this line is the
	// ONLY door to read-only data, and the bytes below prove it opened.
	.section .rodata

	.globl	ro
ro:
	.space	1, 20		// [0]     one byte of 0x14 — a fill that is NOT 0
	.skip	7		// [1..7]  seven bytes of 0x00 — no fill named
				//   => quadword at ro+0 == 20
	.quad	7		// [8..15] and this lands at +8 ONLY IF the two
				//   reserves above advanced the cursor by exactly
				//   8 bytes — the reserve's SIZE is asserted here,
				//   independently of its contents
				//   => quadword at ro+8 == 7

	// ── writable data, reached by the SAME directive ─────────────────
	// `.section .data` resolves to the very row a bare `.data` reaches.
	// Two spellings, one row, so they cannot drift apart.
	.section .data

	.globl	rw
rw:
	.zero	1, 15		// [0]     `.zero` WITH a fill byte
	.space	7		// [1..7]  `.space` WITHOUT one
				//   => quadword at rw+0 == 15

	// ── zero-fill, reached by the SAME directive ─────────────────────
	// ★★ THE FILL BYTE HERE IS NAMED AND MUST BE IGNORED, AND THAT IS WHAT
	// MAKES THE SECTION'S IDENTITY OBSERVABLE AT RUNTIME. A `.bss` reserve
	// stores NO file bytes at all — there is nowhere for a pattern to live
	// — so `0x03` is dropped and the read-back is 0. Route this section to
	// a file-backed one by mistake and the very same directive writes eight
	// 0x03 bytes, which this program then reads and rejects.
	// ⓘ THE WARNING IS DELIBERATE AND IS THE REFERENCE ASSEMBLER'S: gas
	// exits 0 with `Warning: ignoring fill value in section '.bss'`.
	// ✔MEASURED that the corpus runner gates on ERROR-severity diagnostics
	// only (`rep.errorCount()`, tests/examples/examples_runner.cpp), so this
	// arm is tolerated by design rather than by luck.
	.section .bss

	.globl	bz
bz:
	.space	8, 3		//   => quadword at bz+0 == 0 (the fill is dropped)

	.text

	.globl	main
	.type	main, %function
main:
	// ⚠ EACH CHECK HAS ITS OWN EXIT CODE so a failure says WHICH quadword
	// was wrong. A bare sum would let two errors cancel, and a large wrong
	// value could alias 42 in the low byte of the exit status.
	adr	x1, ro
	ldr	x0, [x1, #0]		// the `.space 1, 20` + `.skip 7` word
	cmp	x0, #20
	b.ne	Lbad_ro_fill
	ldr	x2, [x1, #8]		// the `.quad` that follows the reserves
	cmp	x2, #7
	b.ne	Lbad_ro_advance
	add	x0, x0, x2		// 27

	adr	x1, rw
	ldr	x2, [x1, #0]		// the `.zero 1, 15` + `.space 7` word
	cmp	x2, #15
	b.ne	Lbad_rw_fill
	add	x0, x0, x2		// 42

	adr	x1, bz
	ldr	x2, [x1, #0]		// the zero-fill reserve
	cmp	x2, #0
	b.ne	Lbad_bss_nonzero
	add	x0, x0, x2		// 42 + 0 — reading it is the assertion
	ret

Lbad_ro_fill:
	mov	x0, #11
	ret
Lbad_ro_advance:
	mov	x0, #12
	ret
Lbad_rw_fill:
	mov	x0, #13
	ret
Lbad_bss_nonzero:
	mov	x0, #14
	ret
