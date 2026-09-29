/* A FOREIGN ARCHIVE MEMBER THAT READS A LIBRARY DATUM *DIRECTLY*, refused by name at the link.

   The prebuilt archive holds ONE member, gcc 13.3.0 `-O2 -c` (DEFAULT codegen: PIE on Ubuntu) of:

       #include <stdio.h>
       FILE *member_stdout(void) { return stdout; }
       FILE **member_addr_of_stdout(void) { return &stdout; }

   gcc's x86_64 PIE code reaches libc's `stdout` DIRECTLY — `movq stdout(%rip)` and `leaq stdout(%rip)`, two
   R_X86_64_PC32 — because it assumes the final link makes a COPY relocation, and gcc's does: this main.c with that
   archive, `gcc -no-pie` and `gcc -pie`, exits 42 (✔MEASURED 2026-09-24). DSS makes no copy relocation (library
   data binds through a loader-filled slot), so the reference cannot be honoured, and the link refuses it by name:
   `K_ImportReferenceUnbindable`. Before the refusal existed the link bound the member's references to that SLOT —
   the member returned the slot's address and the slot's contents, and this program exited 2.

   The same member built `-fPIC` reaches `stdout` through the GOT and links:
   `examples/c/foreign_got_member_reads_a_library_datum`. */
#include <stdio.h>

FILE *member_stdout(void);
FILE **member_addr_of_stdout(void);

int main(void) {
    if (member_addr_of_stdout() != &stdout) return 2;   /* the member's view of the object's address */
    if (member_stdout() != stdout) return 1;            /* ... and of its value */
    return 42;
}
