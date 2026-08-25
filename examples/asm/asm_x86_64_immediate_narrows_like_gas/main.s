	# D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES — the RUNNABLE
	# witness that a narrowed immediate carries the REFERENCE's bits.
	#
	# ★★★ THE FILE IS THE ASSERTION BEFORE A SINGLE INSTRUCTION RUNS. Until
	# cycle P34 this file DID NOT ASSEMBLE: both `movw` spellings below were
	# refused with A_ImmediateOperandOutOfRange, while ✔GNU as 2.42 assembles
	# both at rc=0. That refusal is the defect the row records — DSS turning
	# away a `.s` a working reference takes.
	#
	# ★★ AND WHAT IT RUNS FOR IS THE OTHER HALF. Assembling is not enough:
	# the ruling says DSS must emit the SAME BYTES gas emits, and only a
	# program whose EXIT CODE depends on the narrowed value can say whether it
	# did. Each half compares the narrowed spelling against the in-window
	# spelling of the SAME 16 bits; they must be indistinguishable.
	.text

	.globl	main
	.type	main, @function
main:
	subq	$64, %rsp
	movq	$42, %rax

	# ── HALF 1 — POSITIVE overflow, the case gas DOES mention ──────────
	# 0x1BEEF does not fit 16 bits; its low half is 0xBEEF.
	# ✔MEASURED, GNU as 2.42: `mov $0x1BEEF, %cx` → rc=0, `66 b9 ef be`,
	# "Warning: 0x1beef shortened to 0xbeef". DSS emits the same immediate
	# and warns too.
	movw	$0x1BEEF, %cx
	movw	$0xBEEF, %dx
	cmpw	%dx, %cx
	je	half1_ok
	addq	$10, %rax
half1_ok:

	# ── HALF 2 — NEGATIVE overflow, THE CASE THE RULING EXISTS FOR ─────
	# ✔MEASURED, GNU as 2.42: `mov $-32769, %cx` → rc=0, `66 b9 ff 7f`,
	# and **NO DIAGNOSTIC AT ALL** — 0x8000 of magnitude disappears with
	# nothing on stderr. DSS emits the SAME `ff 7f` and SAYS SO, which is
	# the whole of the operator's third arm in one instruction.
	movw	$-32769, %cx
	movw	$32767, %dx
	cmpw	%dx, %cx
	je	half2_ok
	addq	$100, %rax
half2_ok:

	# 42 correct · 52 the positive half narrowed wrong · 142 the negative
	# half narrowed wrong · 152 both. Four DISTINCT outcomes, so a partial
	# regression cannot read as a partial pass.
	addq	$64, %rsp
	ret
