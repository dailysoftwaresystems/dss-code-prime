# SOURCE of the committed fixture `tests/link/data/elf_group_two_groups_a_x86_64_elf_gas.o` (P69 fold 3b, lane
# p69/xa; the review of fold 3a, F2): UNIT A of a pair that both carry the COMDAT groups `f` and `g`. Here each
# group is one function, f() = 1 and g() = 2; the unit calls both by name (`two_groups_a`, f() + g()).
# `elf_group_two_groups_b.source.s` is the other unit, whose `g` reaches into its own `f` group.
#
# REBUILD (GNU as 2.42 through gcc 13.3.0; no path of the builder is recorded in the object):
#   gcc -c elf_group_two_groups_a.source.s -o elf_group_two_groups_a_x86_64_elf_gas.o
    .text
    .globl  two_groups_a
    .type   two_groups_a, @function
two_groups_a:
    pushq   %rbx
    call    f
    movl    %eax, %ebx
    call    g
    addl    %ebx, %eax
    popq    %rbx
    ret
    .size   two_groups_a, .-two_groups_a

    .section .text.f,"axG",@progbits,f,comdat
    .weak   f
    .type   f, @function
f:
    movl    $1, %eax
    ret
    .size   f, .-f

    .section .text.g,"axG",@progbits,g,comdat
    .weak   g
    .type   g, @function
g:
    movl    $2, %eax
    ret
    .size   g, .-g
    .section .note.GNU-stack,"",@progbits
