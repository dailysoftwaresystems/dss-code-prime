	# A CALL OUT OF THE TRANSLATION UNIT. `putchar` is defined nowhere in
	# this file, so it is an EXTERN — declared by being referenced, which is
	# the only way gas has (`.extern` is documented as accepted and ignored:
	# "as treats all undefined symbols as external").
	#
	# TWO references, ONE import: both call sites relocate against a single
	# undefined symbol, exactly as `as` emits them.
	#
	# ⚠ NO ARGUMENT MARSHALLING, AND THAT IS DELIBERATE. This unit is a
	# LINK-TIME subject — every arm below emits an object and none of them
	# spawns it (see expected.json for the two named reasons it cannot yet be
	# a running program). Writing an argument setup that no arm executes
	# would be a fixture nobody had measured; the x86_64 sibling
	# `asm_x86_64_branch_call_memory` is the corpus's runnable call example.
	.text

	.globl	main
	.type	main, @function
main:
	subq	$40, %rsp		# 16-byte aligned at the call, per the ABI
	call	putchar
	call	putchar
	movq	$0, %rax
	addq	$40, %rsp
	ret
