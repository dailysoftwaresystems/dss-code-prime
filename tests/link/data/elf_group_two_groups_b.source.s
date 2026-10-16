# SOURCE of the committed fixture `tests/link/data/elf_group_two_groups_b_x86_64_elf_gas.o` (P69 fold 3b, lane
# p69/xa; the review of fold 3a, F2): UNIT B of the pair `elf_group_two_groups_a.source.s` describes — the same
# COMDAT groups `f` and `g`, with different answers: f() = 10, and `g` jumps to a NON-EXTERNAL label of the `f`
# group (`.Lf_inner`, 20), so `.text.g` holds a relocation against `.text.f` — a relocation standing in one group
# that names the bytes of ANOTHER group of the same unit. Behind unit A both groups are discarded together, and
# no linker processes a relocation of a discarded section. The unit calls both by name (`two_groups_b`).
#
# REBUILD (GNU as 2.42 through gcc 13.3.0; no path of the builder is recorded in the object):
#   gcc -c elf_group_two_groups_b.source.s -o elf_group_two_groups_b_x86_64_elf_gas.o
    .text
    .globl  two_groups_b
    .type   two_groups_b, @function
two_groups_b:
    pushq   %rbx
    call    f
    movl    %eax, %ebx
    call    g
    addl    %ebx, %eax
    popq    %rbx
    ret
    .size   two_groups_b, .-two_groups_b

    .section .text.f,"axG",@progbits,f,comdat
    .weak   f
    .type   f, @function
f:
    movl    $10, %eax
    ret
.Lf_inner:
    movl    $20, %eax
    ret
    .size   f, .-f

    .section .text.g,"axG",@progbits,g,comdat
    .weak   g
    .type   g, @function
g:
    jmp     .Lf_inner
    .size   g, .-g
    .section .note.GNU-stack,"",@progbits
