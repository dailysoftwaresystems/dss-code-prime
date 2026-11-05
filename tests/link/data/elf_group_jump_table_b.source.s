# SOURCE of the committed fixture `tests/link/data/elf_group_jump_table_b_x86_64_elf_gas.o` (P69 fold 3, lane
# p69/xa; D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE, the review of fold 2, F1): UNIT B of the
# pair `elf_group_jump_table_a.source.s` describes — the same COMDAT group `f` of two sections, with a DIFFERENT
# body (seven bytes of padding before its cases, so its table's offsets do not fit unit A's function) and
# different answers, so that what a link kept can be read: f(1) answers 44 here. The unit calls `f` by name
# (`jump_table_b`).
#
# REBUILD (GNU as 2.42 through gcc 13.3.0; no path of the builder is recorded in the object):
#   gcc -c elf_group_jump_table_b.source.s -o elf_group_jump_table_b_x86_64_elf_gas.o
    .text
    .globl  jump_table_b
    .type   jump_table_b, @function
jump_table_b:
    movl    $1, %edi
    jmp     f
    .size   jump_table_b, .-jump_table_b

    .section .text.f,"axG",@progbits,f,comdat
    .weak   f
    .type   f, @function
f:
    cmpl    $1, %edi
    ja      .Ldef
    movl    %edi, %edi
    leaq    .Ltab(%rip), %rdx
    movslq  (%rdx,%rdi,4), %rax
    addq    %rdx, %rax
    jmp     *%rax
    nop
    nop
    nop
    nop
    nop
    nop
    nop
.L0:
    movl    $33, %eax
    ret
.L1:
    movl    $44, %eax
    ret
.Ldef:
    movl    $9, %eax
    ret
    .size   f, .-f

    .section .rodata.f,"aG",@progbits,f,comdat
    .align  4
.Ltab:
    .long   .L0-.Ltab
    .long   .L1-.Ltab

    .section .note.GNU-stack,"",@progbits
