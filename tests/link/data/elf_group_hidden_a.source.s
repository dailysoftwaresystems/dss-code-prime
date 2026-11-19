# SOURCE of the committed fixture `tests/link/data/elf_group_hidden_a_x86_64_elf_gas.o` (P69 fold 3c, lane p69/xa;
# D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE, part (c)): UNIT A of a pair.
#
# A COMDAT group `h` whose signature is a GLOBAL, HIDDEN, STRONG definition — what a C++ compiler writes for an
# inline function of a hidden class, and what no rule of name resolution can settle: two units define one strong
# global name. The format settles it by the GROUP: the first unit's is kept, every later unit's discarded with all
# it holds, so the second definition never meets the first. h() answers 7 here and 9 in unit B
# (`elf_group_hidden_b.source.s`); each unit calls `h` by name (`group_hidden_a`).
#
# REBUILD (GNU as 2.42 through gcc 13.3.0; no path of the builder is recorded in the object):
#   gcc -c elf_group_hidden_a.source.s -o elf_group_hidden_a_x86_64_elf_gas.o
    .text
    .globl  group_hidden_a
    .type   group_hidden_a, @function
group_hidden_a:
    jmp     h
    .size   group_hidden_a, .-group_hidden_a

    .section .text.h,"axG",@progbits,h,comdat
    .globl  h
    .hidden h
    .type   h, @function
h:
    movl    $7, %eax
    ret
    .size   h, .-h

    .section .note.GNU-stack,"",@progbits
