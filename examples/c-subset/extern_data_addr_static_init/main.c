/* c158 (D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR, the ELF/Mach-O positive WITNESS
   half of roadmap C2): taking the ADDRESS of an extern libc DATA object in
   a file-scope initializer is LEGAL on formats with a symbol-based image
   relocation, and the pointer must satisfy cross-image identity (C11 6.5.9)
   at runtime.

   `stdout` is libc's extern FILE* stream object (stdio.json kind:object):
   ELF ships it as `stdout` [elf]; Mach-O's stdio macro rewrites it to the
   real data export `__stdoutp` [macho]. The static initializer
   `FILE **pp = &stdout;` lowers to a data-item abs64 relocation targeting
   that extern DATA object.

   - ELF exec binds it via COPY RELOCATION: ld-linux reserves a .bss copy
     slot that IS the object, so `&stdout` is a genuine link-time address
     and `pp == &stdout` holds.
   - Mach-O exec emits a SYMBOL-BASED dyld BIND opcode (the c153 fold): the
     slot is zeroed on disk and dyld writes the resolved `&__stdoutp` in, so
     `pp` and `&stdout` (a __got-indirect load of the same symbol) resolve
     to the ONE libSystem address.

   Either way the identity holds -> exit 42. (PE has NO symbol-based image
   reloc, so its arm is the fail-loud sibling extern_data_addr_reject_pe.)

   `*pp != stdout` additionally proves the slot DEREFERENCES to the live
   stream -- the whole pointer chain is real, not just address-equal. The
   optimized arms prove the address materialization survives the release
   pipelines. RED-ON-DISABLE: bake the indirection-slot VA instead of the
   symbol-based reloc -> `pp` is one indirection off -> `pp != &stdout` ->
   exit 1. gcc/clang -std=c17 agree (exit 42). */
#include <stdio.h>

FILE **pp = &stdout;

int main(void) {
    if (pp != &stdout) return 1;   /* cross-image pointer identity (C11 6.5.9) */
    if (*pp != stdout) return 2;   /* the slot dereferences to the live stream */
    return 42;
}
