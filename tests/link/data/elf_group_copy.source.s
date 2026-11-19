# SOURCE of the committed fixture `tests/link/data/elf_group_copy_x86_64_elf_gas.o` (P69 fold 2, lane p69/xa;
# D-LK-WEAK-NAME-REFERENCE-BOUND-TO-THE-BODY-NOT-THE-NAME): a SECOND COPY of the COMDAT group `shared_g` of
# `elf_group_label.source.s`, holding 9. Linked BEFORE that object it is the group a link keeps, and the other
# object's group is discarded with its bytes.
#
# REBUILD (no path of the builder is recorded in the object):
#   gcc -c elf_group_copy.source.s -o elf_group_copy_x86_64_elf_gas.o
    .section .data.shared_g,"awG",@progbits,shared_g,comdat
    .weak shared_g
    .type shared_g,@object
    .size shared_g,4
    .p2align 2
shared_g:
    .long 9
    .section .note.GNU-stack,"",@progbits
