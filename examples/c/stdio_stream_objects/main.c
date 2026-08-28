/* D-LK-EXTERN-DATA-IMPORT: libc's stdout/stderr/stdin are extern DATA objects
   (stdio.json `kind: object`, type FILE*, elf-gated) imported GOT-INDIRECT —
   the exec reserves one GOT slot per stream and emits one R_*_GLOB_DAT against
   it, ld.so writes the LIBRARY object's address into the slot at load, and the
   shared GotIndirect lowering derefs it. The exec DEFINES NOTHING.

   ⓘ These three used to bind by ELF COPY RELOCATION (an exec-local `.bss` slot
   + a DEFINED OBJECT dynsym + R_*_COPY, with ld-linux memcpy'ing libc's FILE*
   values in). That mechanism is DELETED: its convergence-on-one-storage is
   NAME-SCOPED, so it SPLIT any imported object that has ALIASES
   (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET — glibc exports ONE
   environment object as `environ`/`_environ`/`__environ`). ✔MEASURED, and it is
   why THIS example never caught that bug: each of `stdout`/`stderr`/`stdin` is a
   SINGLE exported name (`_IO_2_1_stdout_` is a DIFFERENT object, not an alias),
   so these three were never exposed to the defect at all. The object-identity
   property is witnessed by examples/c/environ_alias_object_identity,
   which needs a SECOND image to see it.

   Assertions (the strongest the runner supports — its capture MERGES the
   child's stderr into the captured stdout stream):
   - the asserted text proves BOTH streams bind the REAL libc streams (a
     garbage FILE* from a mis-copied slot would crash or lose the bytes);
   - the merged ORDER is made deterministic by flushing stdout BEFORE the
     stderr write: stdout's bytes hit the pipe at the explicit fflush, and
     C 7.21.3p7 guarantees stderr is never fully buffered, so its bytes
     follow (the manual WSL validation additionally asserts the 1>/2>
     SEPARATION, which a merged capture cannot express);
   - both fprintf RETURN VALUES (counted chars) feed the exit code;
   - all three GOT slots hold non-null, pairwise-DISTINCT FILE* values (three
     independent slots each correctly bound — a mis-indexed slot would alias or
     zero one of them, and a MISSING deref would hand the code the slot's own
     ADDRESS instead of the FILE* it holds).

   RED-ON-DISABLE: un-ship the three objects from stdio.json -> honest S0001
   on `stdout`; drop the format's `dataImportBinding` -> the linker's loud
   data-import reject (K_FormatLacksImportSupport) — never a silent stub-read;
   neuter the got-indirect deref (a bare lea) -> the code reads the SLOT's own
   address as the FILE* and crashes. gcc -std=c17 -no-pie cross-checked
   (exit 42, same streams). */
#include <stdio.h>

int main(void) {
    int nOut = fprintf(stdout, "streamobj:stdout-live\n"); /* 22 chars */
    if (fflush(stdout) != 0) return 5;           /* stdout bytes land FIRST */
    int nErr = fprintf(stderr, "streamobj:stderr-live\n"); /* 22 chars, unbuffered */
    if (stdin == 0) return 1;                    /* stdin slot bound */
    if (stdout == stderr) return 2;              /* distinct objects */
    if (stdin == stdout || stdin == stderr) return 3;
    if (nOut != 22 || nErr != 22) return 4;      /* both streams accepted the bytes */
    return 42;
}
