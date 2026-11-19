/* SOURCE of the committed fixture `owner_weak_strong_arm64_macho.o` -- OWNERSHIP SHAPE B on
   Mach-O (`owner_weak_strong.source.c`, the ELF fixtures' source, says what the
   shape is). Apple clang refuses `alias`, so the two names of one datum are written here: `_dss_owned_weak`
   (.weak_definition, visible) and `_dss_owned_strong` (.private_extern), at one address; the weak name is read
   through the GOT, as Apple clang reads a weak definition.
   REBUILD: cc -arch arm64 -c owner_weak_strong.arm64.source.s -o owner_weak_strong_arm64_macho.o
   PROVENANCE, MEASURED at the build (2026-10-15): Apple clang 21.0.0 (clang-2100.1.1.101), ld-1267. */
	.section	__TEXT,__text,regular,pure_instructions
	.globl	_dss_read_owned_strong
	.p2align	2
_dss_read_owned_strong:
	adrp	x8, _dss_owned_strong@PAGE
	ldr	w0, [x8, _dss_owned_strong@PAGEOFF]
	ret
	.globl	_dss_read_owned_weak
	.p2align	2
_dss_read_owned_weak:
	adrp	x8, _dss_owned_weak@GOTPAGE
	ldr	x8, [x8, _dss_owned_weak@GOTPAGEOFF]
	ldr	w0, [x8]
	ret
	.section	__DATA,__data
	.globl	_dss_owned_weak
	.weak_definition	_dss_owned_weak
	.globl	_dss_owned_strong
	.private_extern	_dss_owned_strong
	.p2align	2
_dss_owned_weak:
_dss_owned_strong:
	.long	7
.subsections_via_symbols
