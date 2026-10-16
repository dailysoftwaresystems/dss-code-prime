/* SOURCE of the committed fixture `owner_weak_strong_x86_64_macho.o` -- OWNERSHIP SHAPE B on
   Mach-O (`owner_weak_strong.source.c`, the ELF fixtures' source, says what the
   shape is). Apple clang refuses `alias`, so the two names of one datum are written here: `_dss_owned_weak`
   (.weak_definition, visible) and `_dss_owned_strong` (.private_extern), at one address; the weak name is read
   through the GOT, as Apple clang reads a weak definition.
   REBUILD: cc -arch x86_64 -c owner_weak_strong.x86_64.source.s -o owner_weak_strong_x86_64_macho.o
   PROVENANCE, MEASURED at the build (2026-10-15): Apple clang 21.0.0 (clang-2100.1.1.101), ld-1267. */
	.section	__TEXT,__text,regular,pure_instructions
	.globl	_dss_read_owned_strong
	.p2align	4, 0x90
_dss_read_owned_strong:
	movl	_dss_owned_strong(%rip), %eax
	retq
	.globl	_dss_read_owned_weak
	.p2align	4, 0x90
_dss_read_owned_weak:
	movq	_dss_owned_weak@GOTPCREL(%rip), %rax
	movl	(%rax), %eax
	retq
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
