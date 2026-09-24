// THE PAGE AND THE PAGE OFFSET OF A SYMBOL, SPELLED AS GAS AND CLANG WRITE
// THEM FOR ELF (P68 round 9, the aarch64 twins of
// D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER, and the ELF half of
// D-LK-MACHO-ARM64-PAGEOFF12-LOAD-PATCHED-AS-AN-ADD). The Mach-O spellings of
// the same program are `asm_arm64_page_and_page_offset_macho`.
//
// A global is reached as `adrp` (its 4 KiB page) + an `add` or a load / store
// whose 12-bit field holds its offset in the page. An ADD's offset is bytes; a
// load's or store's counts ACCESS-SIZED units, so the link divides it by the
// size. `pad` keeps every object off page offset 0, where an unscaled field
// would happen to be right. Each value below is read through a different form,
// and 2 + 3 + 4 + 5 + 6 + 7 + 9 + 6 = 42 only if every one reads its own object;
// each address check has its own exit code.
	.text
	.globl	main
	.type	main, %function
main:
	mov	x9, #0
	// A bare `adrp` operand is the PAGE on ELF; `:lo12:` is the ADD's offset.
	adrp	x0, byte1
	add	x0, x0, :lo12:byte1
	ldr	b0, [x0]
	umov	w1, v0.b[0]		// 2 (a byte: the offset is not scaled)
	add	x9, x9, x1
	// `:pg_hi21:` spells the page explicitly (gas); each load scales its field.
	adrp	x2, :pg_hi21:half2
	ldr	h1, [x2, :lo12:half2]
	umov	w3, v1.h[0]		// 3 (a halfword: offset / 2)
	add	x9, x9, x3
	adrp	x2, word4
	ldr	w3, [x2, #:lo12:word4]	// 4 (a word: offset / 4)
	add	x9, x9, x3
	adrp	x2, dword8
	ldr	x3, [x2, :lo12:dword8]	// 5 (a doubleword: offset / 8)
	add	x9, x9, x3
	mov	x4, #6
	str	x4, [x2, :lo12:dword8]	// a store scales the same way
	ldr	x3, [x2, :lo12:dword8]	// 6
	add	x9, x9, x3
	adrp	x2, qword16
	ldr	q2, [x2, :lo12:qword16]
	umov	x3, v2.d[1]		// 7 (a quadword: offset / 16)
	add	x9, x9, x3
	// A constant after the name rides both halves: the page and the offset of tail+8.
	adrp	x2, tail+8
	ldr	x3, [x2, :lo12:tail+8]	// 9
	add	x9, x9, x3
	// The one-word `adr` of a datum: R_AARCH64_ADR_PREL_LO21, +-1 MiB.
	adr	x2, tail
	ldr	x3, [x2]		// 6
	add	x9, x9, x3
	// `.quad .` holds its own address.
	adr	x2, self
	ldr	x3, [x2]
	cmp	x2, x3
	b.ne	.Lbad21
	// The location counter names the line it is on — at a label and mid-block.
3:	adr	x7, .
	adr	x8, 3b
	cmp	x7, x8
	b.ne	.Lbad22
	nop
	adr	x10, .
	adr	x11, .-4
	cmp	x10, x11
	b.ne	.Lbad23
	// `adr` of a label of this function is resolved at assembly: a computed jump.
	adr	x12, 4f
	br	x12
	mov	w0, #24
	ret
4:	mov	x0, x9			// 42
	ret
.Lbad21:
	mov	w0, #21
	ret
.Lbad22:
	mov	w0, #22
	ret
.Lbad23:
	mov	w0, #23
	ret

	.data
	.p2align	4
pad:	.quad	1, 1, 1
byte1:	.byte	2			// page offset 24 in the section
	.byte	0
half2:	.short	3			// 26
word4:	.long	4			// 28
dword8:	.quad	5			// 32
	.quad	0
qword16:	.quad	0, 7			// 48
tail:	.quad	6, 9			// 64
self:	.quad	.			// 80
