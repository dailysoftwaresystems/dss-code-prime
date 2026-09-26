// AN ARM64 `.s` NAMES SYMBOLS IT DOES NOT DEFINE, BY ADDRESS ONLY
// (D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL, the twin of
// `asm_x86_64_address_of_an_undefined_symbol`). `sib_data` (a datum) and
// `sib_fn` (a function) live in a SIBLING unit: the reference-built archive
// the manifest names. Nothing here says which is code and which is data, and
// nothing has to: the link takes the kind from the definition.
// `sib_fn` is never CALLED, so no reference states its kind. Its address is
// taken twice, by `adr` and by a `.quad` slot, and the two must agree; then
// its first instruction byte is read through it (`mov w0, #2` begins 0x40).
//   30 + 10 + (0x40 - 62) = 42, and 1 if the two addresses disagree.
	.text
	.globl	main
	.type	main, %function
main:
	adr	x1, sib_data
	ldr	w2, [x1]
	ldr	w3, [x1, #4]
	add	w0, w2, w3
	adr	x5, slot
	ldr	x5, [x5]
	adr	x6, sib_fn
	cmp	x5, x6
	b.ne	Lbad
	ldr	w7, [x6]
	mov	w8, #255
	and	w7, w7, w8
	sub	w7, w7, #62
	add	w0, w0, w7
	ret
Lbad:
	mov	w0, #1
	ret
	.data
slot:	.quad	sib_fn
