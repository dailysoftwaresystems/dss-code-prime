/* SOURCE of the committed fixture `owner_static_hidden_arm64_macho.o` -- OWNERSHIP SHAPE A on
   Mach-O (`owner_static_hidden.source.c`, the ELF fixtures' source, says what the
   shape is). Apple clang refuses `alias`, so the two names of one datum are written here: `_dss_owned_local`
   (module-private) and `_dss_owned_hidden` (.private_extern), at one address.
   REBUILD: cc -arch arm64 -c owner_static_hidden.arm64.source.s -o owner_static_hidden_arm64_macho.o
   PROVENANCE, MEASURED at the build (2026-10-15): Apple clang 21.0.0 (clang-2100.1.1.101), ld-1267. */
	.section	__TEXT,__text,regular,pure_instructions
	.globl	_dss_read_owned_local
	.p2align	2
_dss_read_owned_local:
	adrp	x8, _dss_owned_local@PAGE
	ldr	w0, [x8, _dss_owned_local@PAGEOFF]
	ret
	.section	__DATA,__data
	.globl	_dss_owned_hidden
	.private_extern	_dss_owned_hidden
	.p2align	2
_dss_owned_local:
_dss_owned_hidden:
	.long	7
.subsections_via_symbols
