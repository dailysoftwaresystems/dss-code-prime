# SOURCE of the committed fixture `tests/link/data/elf_group_jump_table_a_x86_64_elf_gas.o` (P69 fold 3, lane
# p69/xa; D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE, the review of fold 2, F1): UNIT A of a pair.
#
# A COMDAT group `f` of TWO sections, as gcc writes a function that has a jump table (x86-64, PIC): `.text.f`
# holds the function and `.rodata.f` its table. The table's two entries are relocations against `.text.f` + an
# offset (`R_X86_64_PC32` on the section's own symbol) — a relocation that stands INSIDE the group and names the
# BYTES of another member of it. `.rodata.f` holds no named symbol at all. The unit also calls `f` by name
# (`jump_table_a`), as every unit that carries an inline function does.
#
# f(1) answers 22 here and 44 in unit B (`elf_group_jump_table_b.source.s`), whose body is padded so that its
# table does not fit this function. A link keeps the FIRST unit's group: with this unit first both callers answer
# 22, with unit B first 44 (✔MEASURED 2026-10-12, GNU ld 2.42 and ld.lld 18.1.3 on these two objects: a `main`
# returning `jump_table_a() + jump_table_b()` exits 44 and 88).
#
# REBUILD (GNU as 2.42 through gcc 13.3.0; no path of the builder is recorded in the object):
#   gcc -c elf_group_jump_table_a.source.s -o elf_group_jump_table_a_x86_64_elf_gas.o
    .text
    .globl  jump_table_a
    .type   jump_table_a, @function
jump_table_a:
    movl    $1, %edi
    jmp     f
    .size   jump_table_a, .-jump_table_a

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
.L0:
    movl    $11, %eax
    ret
.L1:
    movl    $22, %eax
    ret
.Ldef:
    movl    $7, %eax
    ret
    .size   f, .-f

    .section .rodata.f,"aG",@progbits,f,comdat
    .align  4
.Ltab:
    .long   .L0-.Ltab
    .long   .L1-.Ltab

    .section .note.GNU-stack,"",@progbits
