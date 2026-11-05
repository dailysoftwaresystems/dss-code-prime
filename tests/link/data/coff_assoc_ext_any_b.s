# P69 fold 3d (lane xa): a hand-assembled COFF fixture of `tests/link/test_coff_comdat_selection.cpp` -- copy b of a
# COMDAT `.data$x` of selection ANY holding `x` = 2, and an ASSOCIATIVE `.data$m` that goes with it and DEFINES THE
# EXTERNAL `y` = 20. Beside copy a (`x` = 1, `y` = 10) a program returning x * 10 + y / 10 reads the kept copy's pair;
# beside copy c (`x` = 3, no associate) it reads 22 when b is first, and nothing defines `y` when c is.
# REBUILD: built through probe-reference-cc's keep mode (object mode, twice, the two reports' md5s equal):
#   clang --target=x86_64-pc-windows-msvc -mno-incremental-linker-compatible -c  -> coff_assoc_ext_any_b.obj
#   (the leg and the compiler's version are those of the build record in the test that reads it)
	.section .data$x,"dw",discard,x
	.globl x
	.p2align 2
x:
	.long 2
	.section .data$m,"dw",associative,x
	.globl y
	.p2align 2
y:
	.long 20
