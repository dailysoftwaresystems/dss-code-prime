# P69 fold 3c, lane p69/xa: a 32-bit absolute field at each edge of its range
# (D-LK-LINEAR-RELOCATION-FIELD-RANGE-CHECKED-AS-SIGNED-FOR-EVERY-KIND). Four objects, one per `--defsym`:
#
#   as --defsym ZX=1 --defsym HI=1 -o linear_field_edge_32_hi_x86_64_elf_gas.o   linear_field_edge.source.s
#   as --defsym ZX=1 --defsym HI=0 -o linear_field_edge_32_lo_x86_64_elf_gas.o   linear_field_edge.source.s
#   as --defsym ZX=0 --defsym HI=1 -o linear_field_edge_32s_hi_x86_64_elf_gas.o  linear_field_edge.source.s
#   as --defsym ZX=0 --defsym HI=0 -o linear_field_edge_32s_lo_x86_64_elf_gas.o  linear_field_edge.source.s
#
# (GNU as 2.42, WSL Ubuntu 24.04.) `edge` returns `ext + 2^31` (HI) or `ext - 2^31` (LO): with ZX, through `movl`,
# whose 32-bit write the CPU zero-extends -- R_X86_64_32; without, through `movq $imm32`, sign-extended --
# R_X86_64_32S. In a non-PIE image `ext` sits below 2^31, so the field's value V is in [2^31, 2^32) for HI and in
# [-2^31, 0) for LO. GNU ld 2.42 and ld.lld 18.1.3 link 32 HI and 32S LO and refuse 32 LO and 32S HI ("relocation
# truncated to fit"), MEASURED 2026-10-20. The value `edge` returns is what the program checks against
# `(unsigned long)ext +/- 2^31` computed in C -- the field read back the way the instruction reads it.
        .text
        .globl  edge
        .type   edge, @function
edge:
.if ZX
  .if HI
        movl    $ext+0x80000000, %eax
  .else
        movl    $ext-0x80000000, %eax
  .endif
.else
  .if HI
        movq    $ext+0x80000000, %rax
  .else
        movq    $ext-0x80000000, %rax
  .endif
.endif
        ret
        .size   edge, .-edge
        .data
        .globl  ext
        .type   ext, @object
        .p2align 3
ext:    .quad   0
        .size   ext, 8
        .section .note.GNU-stack,"",@progbits
