# SOURCE of the committed fixtures `tests/link/data/elf_group_label_x86_64_elf_gas.o` and
# `tests/link/data/elf_group_label_x86_64_elf_clang.o` (P69 fold 2, lane p69/xa;
# D-LK-WEAK-NAME-REFERENCE-BOUND-TO-THE-BODY-NOT-THE-NAME, the review of fold 1, MAJOR 1).
#
# WHAT IT IS. An ELF COMDAT GROUP `shared_g` holding a weak datum (7) that has a NON-EXTERNAL label `impl_g` at the
# same address; OUTSIDE the group, `direct_g` reads through the label and `through_g` through the weak name. No
# compiler writes the first of those two reads: the gABI forbids a reference from outside a group to a local symbol
# of it. It is hand-written because it is the one shape in which "the loser of a group keeps its bytes" and "the
# loser of a group is discarded" differ in what a link DOES — see `elf_group_copy.source.s`, the second copy of the
# group, and `elf_group_strong.source.s`, a definition of the name that is in no group.
#
# REBUILD (no path of the builder is recorded in either object):
#   gcc   -c elf_group_label.source.s -o elf_group_label_x86_64_elf_gas.o      (GNU as: the relocation names `impl_g`)
#   clang -c elf_group_label.source.s -o elf_group_label_x86_64_elf_clang.o    (clang's assembler: it names the
#                                                                                group's section symbol)
# PROVENANCE and what each reference linker makes of each object are recorded where the fixtures are read
# (`tests/link/test_common_symbols.cpp`, the suite `SectionGroupLosers`). An md5 written there records WHICH BYTES
# were measured; no guard enforces it.
    .section .data.shared_g,"awG",@progbits,shared_g,comdat
    .weak shared_g
    .type shared_g,@object
    .size shared_g,4
    .p2align 2
shared_g:
impl_g:
    .long 7

    .text
    .globl direct_g
    .type direct_g,@function
direct_g:
    movl impl_g(%rip), %eax
    ret
    .size direct_g, .-direct_g
    .globl through_g
    .type through_g,@function
through_g:
    movl shared_g(%rip), %eax
    ret
    .size through_g, .-through_g
    .section .note.GNU-stack,"",@progbits
