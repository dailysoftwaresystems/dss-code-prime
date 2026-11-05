# P69 fold 3d (lane xa): a hand-assembled COFF fixture of `tests/link/test_coff_comdat_selection.cpp` -- a
# COMDAT `.data$x` whose COMDAT symbol is the STATIC `xsec`, holding the external `x`: no copy of anything
# another object holds, so `x` is one definition of its name. LARGEST, 12 and 16 bytes; object 2 of its pair
# (a program returning x[2] reads 2).
# REBUILD (clang 19.1.5): clang --target=x86_64-pc-windows-msvc -c coff_comdat_static_largest_2.s -o coff_comdat_static_largest_2.obj
	.section .data$x,"dw",largest,xsec
	.p2align 2
xsec:
	.globl x
x:
	.long 0
	.long 0
	.long 2
	.long 0
