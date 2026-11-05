# P69 fold 3d (lane xa): a hand-assembled COFF fixture of `tests/link/test_coff_comdat_selection.cpp` -- one copy of
# a COMDAT `.data$x` holding `x`, whose first quad points (through a LOCAL label, so no external name is defined
# twice) at its own ASSOCIATIVE `.data$m`, which holds the copy's mark. A program returning
# `((int *)x)[2] * 10 + **(int **)x` (or, for the relocation pair, `**(int **)(x + 2) * 10 + **(int **)x`) names the
# copy the link kept and shows the associate went with it.
# EXACT_MATCH, `_b`'s bytes with its aux CheckSum altered (the REBUILD's second step; exit 32).
# REBUILD (clang 19.1.5): clang --target=x86_64-pc-windows-msvc -mno-incremental-linker-compatible -c coff_comdat_exact_checksum.s -o coff_comdat_exact_checksum.obj
#   then flip byte 8 of the aux record that follows `.data$x`'s section symbol (its CheckSum)
	.section .data$x,"dw",same_contents,x
	.globl x
	.p2align 3
x:
	.quad .Lm
	.long 3
	.section .data$m,"dw",associative,x
	.p2align 2
.Lm:
	.long 2
