	// asm_arm64_library_function_address_equals_dlsym — see expected.json. The aarch64 twin of
	// `asm_x86_64_library_function_address_equals_dlsym`: `adr x20, puts` (computed from the place) and
	// `.quad puts` (stored in data) must both equal dlsym(dlopen(NULL), "puts"). Exit 42; 1 the computed
	// address differs, 2 the stored one does, 3 dlopen(NULL) failed.
	//
	// "puts\0" is the quadword 0x73747570 = 1937012080 (little-endian bytes 'p' 'u' 't' 's' 0 0 0 0).
	.text

	.globl	main
	.type	main, %function
main:
	sub	sp, sp, #32
	str	x30, [sp, #0]
	str	x19, [sp, #8]
	str	x20, [sp, #16]
	mov	x0, #0
	mov	x1, #2			// RTLD_NOW
	bl	dlopen
	cmp	x0, #0
	b.eq	Lnoself
	adr	x1, name_puts
	bl	dlsym
	mov	x19, x0			// what the dynamic linker binds `puts` to
	adr	x20, puts		// computed from the place
	mov	x0, #1
	cmp	x19, x20
	b.ne	Lout
	adr	x2, puts_slot		// stored in data
	ldr	x2, [x2]
	mov	x0, #2
	cmp	x19, x2
	b.ne	Lout
	mov	x0, #42
Lout:
	ldr	x30, [sp, #0]
	ldr	x19, [sp, #8]
	ldr	x20, [sp, #16]
	add	sp, sp, #32
	ret
Lnoself:
	mov	x0, #3
	b	Lout

	.data
puts_slot:	.quad	puts
name_puts:	.quad	1937012080
