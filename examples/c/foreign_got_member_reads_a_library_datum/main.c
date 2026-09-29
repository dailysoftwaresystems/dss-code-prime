/* A FOREIGN ARCHIVE MEMBER THAT READS A LIBRARY DATUM THROUGH THE GOT.

   Each prebuilt archive holds ONE member built from:

       #include <stdio.h>
       FILE *member_stdout(void) { return stdout; }
       FILE **member_addr_of_stdout(void) { return &stdout; }

   x86_64: gcc 13.3.0 `-O2 -fPIC -c` — `stdout` through two R_X86_64_REX_GOTPCRELX (`movq stdout@GOTPCREL(%rip)`).
   aarch64: aarch64-linux-gnu-gcc 13.3.0 `-O2 -c` (default codegen) — two R_AARCH64_ADR_GOT_PAGE +
   R_AARCH64_LD64_GOT_LO12_NC pairs.

   The link lowers each reference to a direct reference to ONE slot it mints for `stdout`, and because `stdout` is
   a library DATUM the slot is filled by the loader through a row against the symbol — the member loads the
   datum's address from it, exactly as under GNU ld. ✔MEASURED 2026-09-24: gcc (-no-pie and -pie) and this link
   both run it to 42. What each exit code proves: 2 — the member's `&stdout` is not the object the program's own
   `stdout` names; 1 — its value is not; 42 — both are. The same source built with gcc's DEFAULT x86_64 code names
   `stdout` directly and is refused: `examples/c/foreign_member_reads_a_library_datum_directly_refused`. */
#include <stdio.h>

FILE *member_stdout(void);
FILE **member_addr_of_stdout(void);

int main(void) {
    if (member_addr_of_stdout() != &stdout) return 2;
    if (member_stdout() != stdout) return 1;
    return 42;
}
