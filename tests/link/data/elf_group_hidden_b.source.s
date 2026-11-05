# SOURCE of the committed fixture `tests/link/data/elf_group_hidden_b_x86_64_elf_gas.o` (P69 fold 3c, lane p69/xa;
# D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE, part (c)): UNIT B of the pair whose unit A is
# `elf_group_hidden_a.source.s` (read there). h() answers 9 here, after a padding instruction, so the two bodies
# differ in length as well as in their answer.
#
# REBUILD (GNU as 2.42 through gcc 13.3.0; no path of the builder is recorded in the object):
#   gcc -c elf_group_hidden_b.source.s -o elf_group_hidden_b_x86_64_elf_gas.o
    .text
    .globl  group_hidden_b
    .type   group_hidden_b, @function
group_hidden_b:
    jmp     h
    .size   group_hidden_b, .-group_hidden_b

    .section .text.h,"axG",@progbits,h,comdat
    .globl  h
    .hidden h
    .type   h, @function
h:
    nop
    movl    $9, %eax
    ret
    .size   h, .-h

    .section .note.GNU-stack,"",@progbits
