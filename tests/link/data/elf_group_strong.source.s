# SOURCE of the committed fixture `tests/link/data/elf_group_strong_x86_64_elf_gas.o` (P69 fold 2, lane p69/xa;
# D-LK-WEAK-NAME-REFERENCE-BOUND-TO-THE-BODY-NOT-THE-NAME): a STRONG definition of `shared_g`, holding 9, in NO
# group. Beside the group of `elf_group_label.source.s` it wins the NAME and discards nothing: that object's group
# stays, and what reads its bytes through the label still reads 7.
#
# REBUILD (no path of the builder is recorded in the object):
#   gcc -c elf_group_strong.source.s -o elf_group_strong_x86_64_elf_gas.o
    .data
    .globl shared_g
    .type shared_g,@object
    .size shared_g,4
    .p2align 2
shared_g:
    .long 9
    .section .note.GNU-stack,"",@progbits
