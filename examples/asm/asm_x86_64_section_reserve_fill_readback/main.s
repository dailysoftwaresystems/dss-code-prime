	# `.section NAME` and the reserve-with-fill family, READ BACK AT RUNTIME.
	#
	# ★★★ THE POINT OF THIS FILE IS THAT EVERY BYTE IT RESERVES IS LOADED
	# AGAIN BY THE PROGRAM THAT RESERVED IT. A directive that merely EMITS is
	# proven by a unit test; a directive whose bytes have to survive the
	# lowering, the section assignment, the linker's layout and the loader's
	# mapping is proven only by an exit code. This repo has the scar: a
	# lowering computed `dataItems` and the driver dropped them on the floor —
	# the mutant compiled clean, produced an artifact, and only an image-level
	# witness caught it.
	#
	# ★★ FIVE DIRECTIVE FORMS, EACH LOAD-BEARING:
	#   `.section .rodata`  a section reached BY NAME (the `sectionByName`
	#                       verb delegating to this dialect's own `.rodata`
	#                       row, which is `operandOnly` and unwritable bare)
	#   `.section .data`    the SAME verb resolving to a row that IS writable
	#                       bare — one directive, two rows, no second table
	#   `.section .bss`     a ZERO-FILL section, whose read-back must be 0
	#   `.space N, F`       a reserve WITH a fill byte
	#   `.zero  N, F`       the same verb under gas's other spelling — `.zero`
	#                       is NOT a fill-less special case (measured against
	#                       the reference assembler; see expected.json)
	#   `.skip  N`          / `.space N` with NO fill: the default is zero
	#
	# ★★ AND THE ANSWER IS A SUM OF WHAT WAS READ, NOT A CONSTANT: the four
	# loaded quadwords are 20 + 7 + 15 + 0 = 42. Each is ALSO compared against
	# what it must be, so a wrong byte lands on its own numbered arm instead
	# of colliding with 42 by arithmetic accident.

	# ── read-only data, reached by NAME ──────────────────────────────────
	# `.rodata` is not a gas directive: a bare `.rodata` is an unknown
	# pseudo-op and only `.section .rodata` assembles. So this line is the
	# ONLY door to read-only data, and the bytes below prove it opened.
	.section .rodata

	.globl	ro
ro:
	.space	1, 20		# [0]     one byte of 0x14  — a fill that is NOT 0
	.skip	7		# [1..7]  seven bytes of 0x00 — no fill named
				#   => quadword at ro+0 == 20
	.quad	7		# [8..15] and this lands at +8 ONLY IF the two
				#   reserves above advanced the cursor by exactly
				#   8 bytes — the reserve's SIZE is asserted here,
				#   independently of its contents
				#   => quadword at ro+8 == 7

	# ── writable data, reached by the SAME directive ─────────────────────
	# `.section .data` resolves to the very row a bare `.data` reaches. Two
	# spellings, one row, so they cannot drift apart.
	.section .data

	.globl	rw
rw:
	.zero	1, 15		# [0]     `.zero` WITH a fill byte
	.space	7		# [1..7]  `.space` WITHOUT one
				#   => quadword at rw+0 == 15

	# ── zero-fill, reached by the SAME directive ─────────────────────────
	# ★★ THE FILL BYTE HERE IS NAMED AND MUST BE IGNORED, AND THAT IS WHAT
	# MAKES THE SECTION'S IDENTITY OBSERVABLE AT RUNTIME. A `.bss` reserve
	# stores NO file bytes at all — there is nowhere for a pattern to live —
	# so `0x03` is dropped and the read-back is 0. Route this section to a
	# file-backed one by mistake and the very same directive writes eight
	# 0x03 bytes, which this program then reads and rejects.
	# ⓘ THE WARNING IS DELIBERATE AND IS THE REFERENCE ASSEMBLER'S: gas exits
	# 0 with `Warning: ignoring fill value in section '.bss'`. ✔MEASURED that
	# the corpus runner gates on ERROR-severity diagnostics only
	# (`rep.errorCount()`, tests/examples/examples_runner.cpp), so this arm is
	# tolerated by design rather than by luck.
	.section .bss

	.globl	bz
bz:
	.space	8, 3		#   => quadword at bz+0 == 0 (the fill is dropped)

	.text

	.globl	main
	.type	main, @function
main:
	# ⚠ EACH CHECK HAS ITS OWN EXIT CODE so a failure says WHICH quadword
	# was wrong. A bare sum would let two errors cancel, and a large wrong
	# value could alias 42 in the low byte of the exit status.
	leaq	ro, %rcx
	movq	0(%rcx), %rax		# the `.space 1, 20` + `.skip 7` word
	cmpq	$20, %rax
	jne	Lbad_ro_fill
	movq	8(%rcx), %rdx		# the `.quad` that follows the reserves
	cmpq	$7, %rdx
	jne	Lbad_ro_advance
	addq	%rdx, %rax		# 27

	leaq	rw, %rcx
	movq	0(%rcx), %rdx		# the `.zero 1, 15` + `.space 7` word
	cmpq	$15, %rdx
	jne	Lbad_rw_fill
	addq	%rdx, %rax		# 42

	leaq	bz, %rcx
	movq	0(%rcx), %rdx		# the zero-fill reserve
	cmpq	$0, %rdx
	jne	Lbad_bss_nonzero
	addq	%rdx, %rax		# 42 + 0 — reading it is the assertion
	ret

Lbad_ro_fill:
	movq	$11, %rax
	ret
Lbad_ro_advance:
	movq	$12, %rax
	ret
Lbad_rw_fill:
	movq	$13, %rax
	ret
Lbad_bss_nonzero:
	movq	$14, %rax
	ret
