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

   - ELF exec emits a SYMBOL-BASED ABSOLUTE image relocation (R_X86_64_64 /
     R_AARCH64_ABS64 against the extern's dynsym row, in `.rela.dyn`): the
     slot is ZEROED on disk and ld.so writes the resolved `&stdout` in, so
     `pp` and `&stdout` (a got-indirect load of the same symbol) resolve to
     the ONE libc address.
     ⓘ IT USED TO BIND BY COPY RELOCATION, and that is why this witness is
     worth reading: ld-linux reserved a `.bss` copy slot that WAS the object,
     so `&stdout` was a genuine LINK-TIME address and the apply could bake it
     in with no `.rela.dyn` row at all. Copy relocation is DELETED
     (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET: it claimed ONE
     name of an aliased libc object and split it), and its removal took that
     link-time constant with it — ✔MEASURED, the first cut of the deletion
     baked `symbolVa[extern]`, which is now the GOT SLOT, and THIS EXAMPLE
     went red at exit 1 with the pointer one indirection off, exactly as the
     red-on-disable note below predicts. The exec arm therefore adopted the
     rows the ET_DYN arm already emitted for the same reason.
   - Mach-O exec emits a SYMBOL-BASED dyld BIND opcode (the c153 fold): the
     slot is zeroed on disk and dyld writes the resolved `&__stdoutp` in, so
     `pp` and `&stdout` (a __got-indirect load of the same symbol) resolve
     to the ONE libSystem address.
   ⇒ All three image families now use the SAME shape — a symbol-based
     load-time write into a zeroed slot — which is one fewer per-format story
     to keep straight, not merely a smaller diff.

   Either way the identity holds -> exit 42. (PE has NO symbol-based image
   reloc, so its arm is the fail-loud sibling extern_data_addr_reject_pe.)

   `*pp != stdout` additionally proves the slot DEREFERENCES to the live
   stream -- the whole pointer chain is real, not just address-equal. The
   optimized arms prove the address materialization survives the release
   pipelines. RED-ON-DISABLE, and it has now FIRED FOR REAL on the ELF exec
   arm rather than being merely predicted: bake the indirection-slot VA
   instead of the symbol-based reloc -> `pp` is one indirection off ->
   `pp != &stdout` -> exit 1, with NO diagnostic anywhere. ★ Note where that
   was caught: only a RUN leg sees it. The Windows ctest COMPILES every elf
   arm and runs none, so this example was green on Windows while broken.
   gcc/clang -std=c17 agree (exit 42). */
#include <stdio.h>

FILE **pp = &stdout;

int main(void) {
    if (pp != &stdout) return 1;   /* cross-image pointer identity (C11 6.5.9) */
    if (*pp != stdout) return 2;   /* the slot dereferences to the live stream */
    return 42;
}
