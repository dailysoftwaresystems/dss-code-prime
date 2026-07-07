/* c84 (D-LK-EXTERN-DATA-IMPORT): libc's stdout/stderr/stdin are extern DATA
   objects (stdio.json `kind: object`, type FILE*, elf-gated) imported via ELF
   COPY RELOCATIONS — the exec reserves one .bss slot per stream, exports each
   as a DEFINED OBJECT (st_size = the layout-derived pointer width), and
   ld-linux memcpy's libc's FILE* values in before entry (R_X86_64_COPY /
   R_AARCH64_COPY in .rela.dyn).

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
   - all three copied slots hold non-null, pairwise-DISTINCT pointers (three
     independent slots each correctly filled — a mis-sized/mis-offset copy
     would alias or zero one of them).

   RED-ON-DISABLE: un-ship the three objects from stdio.json -> honest S0001
   on `stdout`; drop the format's `dataImportBinding` -> the linker's loud
   data-import reject (K_FormatLacksImportSupport) — never a silent
   stub-read. gcc -std=c17 -no-pie cross-checked (exit 42, same streams). */
#include <stdio.h>

int main(void) {
    int nOut = fprintf(stdout, "copyreloc:stdout-live\n"); /* 22 chars */
    if (fflush(stdout) != 0) return 5;           /* stdout bytes land FIRST */
    int nErr = fprintf(stderr, "copyreloc:stderr-live\n"); /* 22 chars, unbuffered */
    if (stdin == 0) return 1;                    /* stdin slot bound */
    if (stdout == stderr) return 2;              /* distinct objects */
    if (stdin == stdout || stdin == stderr) return 3;
    if (nOut != 22 || nErr != 22) return 4;      /* both streams accepted the bytes */
    return 42;
}
