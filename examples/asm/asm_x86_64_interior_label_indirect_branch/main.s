	# An interior label's address, materialized and then BRANCHED THROUGH.
	#
	# ★ THIS IS THE HALF OF D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET
	# THAT NEEDS NO DATA SECTION: an interior label's address materialized
	# straight into a register. The data-defined jump table is its sibling
	# `asm_arm64_jump_table_and_data_readback`. Between them the capability is
	# witnessed on both CPUs and on two object formats, and each file is
	# written in the assembly its own CPU actually speaks.
	#
	# ★★ TWO ADDRESSES ARE TAKEN, NOT ONE, AND THAT IS THE WHOLE TEST. With a
	# single address-taken label the indirect branch has exactly one possible
	# destination, so a build that ignored %rax entirely would still exit 42.
	# With two, the successor set has two members and the branch has to follow
	# the one that was actually loaded — `Lfail` is reachable, is a real
	# successor, and returns a DIFFERENT exit code.
	#
	# ⚠ `Lfail` COMES FIRST IN LAYOUT, so an interior symbol wrongly bound to
	# the function's start or to the nearest block lands there and exits 1
	# rather than crashing — a wrong answer that the harness reports, instead
	# of a signal that could be mistaken for an environment problem.
	.text

	.globl	main
	.type	main, @function
main:
	leaq	Lfail, %rdx
	leaq	Lwin, %rax
	jmp	*%rax
Lfail:
	movl	$1, %eax
	ret
Lwin:
	movl	$42, %eax
	ret
