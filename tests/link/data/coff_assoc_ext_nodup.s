# P69 fold 3d (lane xa): a hand-assembled COFF fixture of `tests/link/test_coff_comdat_selection.cpp` -- the function
# `f` in a COMDAT `.text$mn` of selection NODUPLICATES, and an ASSOCIATIVE `.data$a` that goes with it and DEFINES THE
# EXTERNAL `y` = 42, which `f` returns. The associate of a section no link discards: an ordinary section of its unit,
# and `y` one definition of its name (a program returning f() + y - 42 reads 42 under link.exe and lld-link).
# REBUILD: built through probe-reference-cc's keep mode (object mode, twice, the two reports' md5s equal):
#   clang --target=x86_64-pc-windows-msvc -mno-incremental-linker-compatible -c  -> coff_assoc_ext_nodup.obj
#   (the leg and the compiler's version are those of the build record in the test that reads it)
	.section .text$mn,"xr",one_only,f
	.globl f
	.p2align 4
f:
	movl y(%rip), %eax
	retq
	.section .data$a,"dw",associative,f
	.globl y
	.p2align 2
y:
	.long 42
