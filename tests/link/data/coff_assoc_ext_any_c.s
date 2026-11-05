# P69 fold 3d (lane xa): a hand-assembled COFF fixture of `tests/link/test_coff_comdat_selection.cpp` -- copy c of a
# COMDAT `.data$x` of selection ANY holding `x` = 3, with NO associate, so it defines no `y`. Kept first beside copy b,
# the only `y` is in b's lost associate: lld-link refuses (undefined `y`), and link.exe links an image that reads its
# own DOS header as `y` (exit 946060) -- a discarded section's name, which does not work.
# REBUILD: built through probe-reference-cc's keep mode (object mode, twice, the two reports' md5s equal):
#   clang --target=x86_64-pc-windows-msvc -mno-incremental-linker-compatible -c  -> coff_assoc_ext_any_c.obj
#   (the leg and the compiler's version are those of the build record in the test that reads it)
	.section .data$x,"dw",discard,x
	.globl x
	.p2align 2
x:
	.long 3
