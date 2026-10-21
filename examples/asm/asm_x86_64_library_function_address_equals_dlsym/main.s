	# asm_x86_64_library_function_address_equals_dlsym — see expected.json.
	#
	# A hand-written unit takes the address of the C library's `puts` the two ways assembly can: computed from
	# the place (`leaq puts(%rip)`) and stored in data (`.quad puts`). In an ELF executable or PIE both must be
	# the ONE address the dynamic linker answers for `puts` (dlsym): the image's stub, made canonical. Exit 42;
	# 1 when the computed address differs, 2 when the stored one does, 3 when dlopen(NULL) failed.
	#
	# "puts\0" is the quadword 0x73747570 = 1937012080 (little-endian bytes 'p' 'u' 't' 's' 0 0 0 0).
	.text

	.globl	main
	.type	main, @function
main:
	subq	$40, %rsp
	movq	%rbx, 16(%rsp)
	movq	%r12, 24(%rsp)
	xorl	%edi, %edi
	movl	$2, %esi		# RTLD_NOW
	call	dlopen
	testq	%rax, %rax
	je	2f
	movq	%rax, %rdi
	leaq	name_puts(%rip), %rsi
	call	dlsym
	movq	%rax, %rbx		# what the dynamic linker binds `puts` to
	leaq	puts(%rip), %r12	# computed from the place
	movl	$1, %eax
	cmpq	%rbx, %r12
	jne	1f
	movq	puts_slot(%rip), %rcx	# stored in data
	movl	$2, %eax
	cmpq	%rbx, %rcx
	jne	1f
	movl	$42, %eax
1:	movq	16(%rsp), %rbx
	movq	24(%rsp), %r12
	addq	$40, %rsp
	ret
2:	movl	$3, %eax
	jmp	1b

	.data
puts_slot:	.quad	puts
name_puts:	.quad	1937012080
