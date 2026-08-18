	# CALL FRAME INFORMATION — the directive family real compiler output is
	# made of. ✔MEASURED 2026-08-13: `gcc -O1 -fno-ident -fcf-protection=none
	# -S` on a five-line C function emits SIX `.cfi_*` lines against FIVE
	# instructions, because `-fasynchronous-unwind-tables` is on by default on
	# every Linux target. A dialect that refuses `.cfi_startproc` cannot read
	# one function of its own reference compiler's output.
	#
	# Every `.cfi_*` spelling this dialect declares AS A HONOURED RULE appears
	# below, so deleting ANY ONE ROW from `asm-x86_64-att.lang.json` turns this
	# example red. The two it declares as UNREPRESENTABLE — `.cfi_signal_frame`
	# and `.cfi_escape` — are deliberately ABSENT here and are pinned by the
	# REFUSAL test instead (tests/asm/test_asm_cfi_producer.cpp); they used to
	# sit in this file, accepted and dropped, which is the very defect
	# D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED names.
	#
	# ★★★ `main`'s FRAME DESCRIPTION IS HONEST, AND THAT IS WHAT MAKES THIS AN
	# UNWIND TEST RATHER THAN A PARSE TEST: CFA = rsp+48 from the end of the
	# `subq` until the `addq` releases it, which is exactly the truth about this
	# hand-written frame. ✔MEASURED 2026-08-17 on the ELF this file produces:
	# gdb 15.1 walks helper → main → the entry trampoline and STOPS, with
	# frame 1's CFA exactly 48 bytes above frame 0's. With every `.cfi_*` line
	# deleted (matched control, same compiler) the same binary still exits 42
	# and gdb's walk runs off into `0x7fffffffdf60 in ?? ()` — a STACK address
	# presented as a return address. "Runs correctly, cannot be unwound" is not
	# an abstraction; that garbage frame is what it looks like.
	.text

	.globl	main
	.type	main, @function
main:
	.cfi_startproc
	subq	$40, %rsp		# 16-byte aligned at the call, per the ABI
	.cfi_def_cfa_offset 48
	movq	$100, %rax
	movq	$7, %rcx
	subq	%rcx, %rax		# 93 — two-address: rax = rax - rcx
	movq	%rax, 32(%rsp)		# spill, so the CFA offset above is real
	.cfi_remember_state
	movq	32(%rsp), %rdx		# reload
	movq	$2, %rcx
	imulq	%rcx, %rdx		# 186
	.cfi_restore_state
	movq	%rdx, %rax
	movq	$144, %rcx
	call	helper			# rax = helper(rax=186, rcx=144) = 42
	addq	$40, %rsp
	.cfi_def_cfa_offset 8
	ret
	.cfi_endproc

	.globl	helper
	.type	helper, @function
helper:
	.cfi_startproc
	.cfi_return_column 16		# 16 IS the x86_64 default (DWARF RA column)
	.cfi_def_cfa 7, 8
	.cfi_def_cfa_register 7
	.cfi_adjust_cfa_offset 0
	.cfi_offset 6, -16
	.cfi_rel_offset 6, 8
	.cfi_val_offset 6, -16
	.cfi_register 3, 12
	.cfi_same_value 3
	.cfi_undefined 12
	subq	%rcx, %rax
	.cfi_restore 6
	ret
	.cfi_endproc
