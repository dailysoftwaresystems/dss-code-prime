# P69 fold 3d (lane xa): a hand-assembled COFF fixture of `tests/link/test_coff_comdat_selection.cpp` -- copy a of a
# COMDAT `.data$x` of selection ANY holding `x` = 1, and an ASSOCIATIVE `.data$m` that goes with it and DEFINES THE
# EXTERNAL `y` = 10. Beside copy b (`x` = 2, `y` = 20) a program returning x * 10 + y / 10 reads the kept copy's pair:
# 11 for a, 22 for b (lld-link keeps the first; link.exe refuses the two `y`, LNK2005).
# REBUILD: built through probe-reference-cc's keep mode (object mode, twice, the two reports' md5s equal):
#   clang --target=x86_64-pc-windows-msvc -mno-incremental-linker-compatible -c  -> coff_assoc_ext_any_a.obj
#   (the leg and the compiler's version are those of the build record in the test that reads it)
	.section .data$x,"dw",discard,x
	.globl x
	.p2align 2
x:
	.long 1
	.section .data$m,"dw",associative,x
	.globl y
	.p2align 2
y:
	.long 10
