	# CALL FRAME INFORMATION — the directive family real compiler output is
	# made of. ✔MEASURED 2026-08-13: `gcc -O1 -fno-ident -fcf-protection=none
	# -S` on a five-line C function emits SIX `.cfi_*` lines against FIVE
	# instructions, because `-fasynchronous-unwind-tables` is on by default on
	# every Linux target. A dialect that refuses `.cfi_startproc` cannot read
	# one function of its own reference compiler's output.
	#
	# Every `.cfi_*` spelling this dialect declares appears below, so deleting
	# ANY ONE ROW from `asm-x86_64-att.lang.json` turns this example red.
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
	.cfi_signal_frame
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
	.cfi_escape 0x10, 0x06, 0x02
	subq	%rcx, %rax
	.cfi_restore 6
	ret
	.cfi_endproc
