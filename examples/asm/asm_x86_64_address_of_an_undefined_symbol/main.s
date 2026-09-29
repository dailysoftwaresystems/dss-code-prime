# A `.s` NAMES SYMBOLS IT DOES NOT DEFINE, BY ADDRESS
# (D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL). `sib_data` (a datum) and
# `sib_fn` (a function) live in a SIBLING unit: the reference-built archive the
# manifest names. Nothing in this file says which is code and which is data, and
# nothing has to. gas records the name alone, and the link takes the kind from
# the definition it finds. Every shape changes the exit code if it is wrong:
#   * `leaq sib_data(%rip)` then a load, and `sib_data+4(%rip)` -- 30 + 10;
#   * `.quad sib_fn` compared with `leaq sib_fn(%rip)` -- one function, one address;
#   * a call through that address -- + 2.
	.text
	.globl	main
	.type	main, @function
main:
	subq	$24, %rsp
	leaq	sib_data(%rip), %rax
	movl	(%rax), %ecx
	addl	sib_data+4(%rip), %ecx
	movl	%ecx, 16(%rsp)
	movq	sib_fn_slot(%rip), %rdx
	leaq	sib_fn(%rip), %r8
	cmpq	%rdx, %r8
	jne	1f
	call	*%rdx
	addl	16(%rsp), %eax
	addq	$24, %rsp
	ret
1:	movl	$1, %eax
	addq	$24, %rsp
	ret
	.data
sib_fn_slot:	.quad	sib_fn
