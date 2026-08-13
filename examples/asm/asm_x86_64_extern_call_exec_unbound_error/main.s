	# The EXEC arm of D-ASM-EXTERNAL-CALL-UNREPRESENTABLE. Byte-for-byte the
	# same reference its sibling `asm_x86_64_extern_call_object` emits into a
	# `.o` / `.obj` / `.so` — the difference is entirely the OBJECT FORMAT the
	# manifest builds it for, which is the point: one lowering, one import
	# row, three verdicts from the linker's one reference-gate policy.
	.text

	.globl	main
	.type	main, @function
main:
	subq	$40, %rsp
	call	putchar
	movq	$0, %rax
	addq	$40, %rsp
	ret
