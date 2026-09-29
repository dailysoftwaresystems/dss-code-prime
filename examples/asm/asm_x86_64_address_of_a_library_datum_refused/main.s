# A DIRECT REFERENCE TO A LIBRARY DATUM (libc's `stdout`), refused by name: it needs a copy relocation, which
# this link does not make (D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL). See expected.json.
	.text
	.globl	main
	.type	main, @function
main:
	movq	stdout(%rip), %rcx
	movl	$42, %eax
	ret
