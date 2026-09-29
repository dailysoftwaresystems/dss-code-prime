# RIP-RELATIVE ADDRESSING, THE WAY gcc -S WRITES EVERY GLOBAL ACCESS
# (D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER). Every shape below changes
# the exit code if it is wrong:
#   * `(%rip)` is the address of the NEXT instruction, and `-7(%rip)` from a
#     7-byte lea is that lea itself — compared with each other, and with the
#     label on a lea (a label reached only after a conditional branch, so no
#     layout choice sits between them);
#   * a store with an immediate AFTER its displacement (`movl $5, counter(%rip)`)
#     and a read-modify-write of the same word — a relocation that ignored the 4
#     immediate bytes would write 4 bytes past `counter`;
#   * a constant after a symbol, both signs: `table+16` and `tail-8` name the
#     same word, and `-8` is never read as the number -8;
#   * `leaq 1f(%rip)` + `jmp *%rdi` — an interior label's address, branched to.
# gas 2.42 and clang 18.1.3 run this file to 42 (measured 2026-09-23).
	.text
	.globl	main
	.type	main, @function
main:
	leaq	(%rip), %r8
	leaq	-7(%rip), %r9
	cmpq	%r8, %r9
	jne	9f
3:	leaq	-7(%rip), %r10
	leaq	3b(%rip), %r11
	cmpq	%r10, %r11
	jne	9f
	movl	$5, counter(%rip)
	addl	$10, counter(%rip)
	movl	counter(%rip), %eax
	leaq	table(%rip), %rcx
	addl	8(%rcx), %eax
	movl	table+16(%rip), %edx
	movl	tail-8(%rip), %esi
	cmpl	%edx, %esi
	jne	9f
	addl	%edx, %eax
	leaq	1f(%rip), %rdi
	jmp	*%rdi
9:	movl	$1, %eax
	ret
1:	ret
	.data
counter:	.long	0
	.long	0
table:	.quad	3
	.quad	7
	.quad	20
tail:	.quad	0
