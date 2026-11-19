# P69 fold 3d (lane xa): a hand-assembled COFF fixture of `tests/link/test_coff_comdat_selection.cpp` -- a
# COMDAT `.data$x` whose COMDAT symbol is the STATIC `xsec`, holding the external `x`: no copy of anything
# another object holds, so `x` is one definition of its name. SAME_SIZE, both 12 bytes; object 1 of its pair
# (a program returning x[2] reads 1).
# REBUILD (clang 19.1.5): clang --target=x86_64-pc-windows-msvc -c coff_comdat_static_samesize_1.s -o coff_comdat_static_samesize_1.obj
	.section .data$x,"dw",same_size,xsec
	.p2align 2
xsec:
	.globl x
x:
	.long 0
	.long 0
	.long 1
