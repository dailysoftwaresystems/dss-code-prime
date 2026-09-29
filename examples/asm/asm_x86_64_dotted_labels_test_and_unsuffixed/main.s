# `.L` LABELS AS BRANCH TARGETS, THE `test` FAMILY, `sete`, AND UNSUFFIXED
# MNEMONICS — AT&T spellings GNU as and clang assemble, and the P68 round-7 base
# refused (D-ASM-DOTTED-LABEL-AS-A-BRANCH-TARGET-REFUSED,
# D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS).
#
# 42 is assembled from a `sete` (1), a byte `test` that must see bit 1 set, and
# unsuffixed moves whose width only the register names state. Each wrong
# answer takes its own exit code (11-14). Only registers volatile under BOTH
# ABIs are touched (rax, rcx, rdx), so the same file is correct on System V
# and on Win64, and no stack frame is needed.
	.text
	.globl	main
	.type	main, @function
main:
	mov	$40, %ecx		# unsuffixed: %ecx states 32 bits
	xor	%eax, %eax
	testl	%eax, %eax		# eax == 0 ...
	sete	%al			# ... so al = 1
	cmp	$1, %eax		# unsuffixed cmp against a register: 32 bits
	jne	.Lbad11
	add	%ecx, %eax		# 1 + 40 = 41
	add	$1, %eax		# 42
	mov	%eax, %edx
	testb	$2, %dl			# 42 = 0b101010: bit 1 is set
	je	.Lbad12
	test	%rdx, %rdx		# unsuffixed test of a 64-bit register
	je	.Lbad13
	lea	0(%rdx), %eax		# unsuffixed lea: the destination states 32
	jmp	.Ldone
.Lbad11:
	mov	$11, %eax
	jmp	.Ldone
.Lbad12:
	mov	$12, %eax
	jmp	.Ldone
.Lbad13:
	mov	$13, %eax
.Ldone:
	ret
