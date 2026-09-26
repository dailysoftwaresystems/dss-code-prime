// THE PAGE AND THE PAGE OFFSET OF A SYMBOL, SPELLED AS CLANG WRITES THEM FOR
// DARWIN (P68 round 9, the aarch64 twins of
// D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER, and the Mach-O half of
// D-LK-MACHO-ARM64-PAGEOFF12-LOAD-PATCHED-AS-AN-ADD). The ELF spellings of the
// same program are `asm_arm64_page_and_page_offset_elf`; Darwin's references
// read `sym@PAGE` / `sym@PAGEOFF` and refuse `:lo12:` and a bare `adrp` operand,
// and Mach-O has no relocation for an `adr` to another section.
//
// A global is reached as `adrp` (its 4 KiB page) + an `add` or a load / store
// whose 12-bit field holds its offset in the page. Mach-O writes ONE relocation
// type for all of them (ARM64_RELOC_PAGEOFF12), and the link takes the scale
// from the instruction: an ADD's offset is bytes, a load's or store's counts
// ACCESS-SIZED units. `pad` keeps every object off page offset 0, where an
// unscaled field would happen to be right. 2 + 3 + 4 + 5 + 6 + 7 + 9 + 6 = 42
// only if every load reads its own object; each address check has its own exit
// code.
	.text
	.globl	main
	.type	main, %function
main:
	mov	x9, #0
	adrp	x0, byte1@PAGE
	add	x0, x0, byte1@PAGEOFF
	ldr	b0, [x0]
	umov	w1, v0.b[0]		// 2 (a byte: the offset is not scaled)
	add	x9, x9, x1
	adrp	x2, half2@PAGE
	ldr	h1, [x2, half2@PAGEOFF]
	umov	w3, v1.h[0]		// 3 (a halfword: offset / 2)
	add	x9, x9, x3
	adrp	x2, word4@PAGE
	ldr	w3, [x2, word4@PAGEOFF]	// 4 (a word: offset / 4)
	add	x9, x9, x3
	adrp	x2, dword8@PAGE
	ldr	x3, [x2, dword8@PAGEOFF]	// 5 (a doubleword: offset / 8)
	add	x9, x9, x3
	mov	x4, #6
	str	x4, [x2, dword8@PAGEOFF]	// a store scales the same way
	ldr	x3, [x2, dword8@PAGEOFF]	// 6
	add	x9, x9, x3
	adrp	x2, qword16@PAGE
	ldr	q2, [x2, qword16@PAGEOFF]
	umov	x3, v2.d[1]		// 7 (a quadword: offset / 16)
	add	x9, x9, x3
	// A constant after the name rides both halves, on either side of the operator.
	adrp	x2, tail+8@PAGE
	ldr	x3, [x2, tail@PAGEOFF+8]	// 9
	add	x9, x9, x3
	adrp	x2, tail@PAGE
	add	x2, x2, tail@PAGEOFF
	ldr	x3, [x2]		// 6
	add	x9, x9, x3
	// `.quad .` holds its own address.
	adrp	x2, self@PAGE
	add	x2, x2, self@PAGEOFF
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
