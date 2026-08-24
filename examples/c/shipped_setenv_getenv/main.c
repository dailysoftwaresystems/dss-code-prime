/* c85 (shell.c POSIX environment surface): setenv/unsetenv ship from
   stdlib.json gated [elf,macho] (POSIX, not C89 — the realpath precedent;
   msvcrt has only _putenv) and chdir ships from unistd.json (flat — rides
   the header's [elf,macho] gate, the getcwd sibling). glibc prototypes
   verified /usr/include/stdlib.h + unistd.h; exports verified nm -D
   libc.so.6 (W setenv / W unsetenv / W chdir, x86_64 AND aarch64, one
   version each — the D-LK-ELF-SYMBOL-VERSIONING multi-version caveat does
   not apply).

   Assertions (every branch feeds a DISTINCT exit code — a wrong result is
   attributable, never a silent pass):
   - the ROUND-TRIP: setenv(K,V,1) then getenv(K) returns byte-equal V
     (the store went through the real libc environ, not a stub);
   - overwrite=0 does NOT clobber an existing binding (the third parameter
     is genuinely wired — a mis-ABI'd int arg would flip this behavior);
   - overwrite=1 DOES replace (the same parameter, opposite value);
   - unsetenv(K) removes the binding: getenv(K) then returns NULL;
   - chdir("/") succeeds and getcwd() reads back exactly "/" (the process
     cwd really moved — a no-op chdir stub would leave the test cwd).

   RED-ON-DISABLE: un-ship setenv/unsetenv (or chdir) from the descriptor
   -> honest S0001 at the call site — never a silent no-op. gcc -std=c17
   cross-checked in WSL (exit 42, same stdout) on both arches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* setenv/getenv round-trip */
    if (setenv("DSS_C85_KEY", "round-trip-42", 1) != 0) return 10;
    char *v = getenv("DSS_C85_KEY");
    if (v == 0) return 11;
    if (strcmp(v, "round-trip-42") != 0) return 12;

    /* overwrite=0 must NOT replace the existing value */
    if (setenv("DSS_C85_KEY", "clobbered", 0) != 0) return 13;
    v = getenv("DSS_C85_KEY");
    if (v == 0 || strcmp(v, "round-trip-42") != 0) return 14;

    /* overwrite=1 must replace it */
    if (setenv("DSS_C85_KEY", "second", 1) != 0) return 15;
    v = getenv("DSS_C85_KEY");
    if (v == 0 || strcmp(v, "second") != 0) return 16;

    /* unsetenv removes the binding entirely */
    if (unsetenv("DSS_C85_KEY") != 0) return 17;
    if (getenv("DSS_C85_KEY") != 0) return 18;

    /* chdir really moves the process cwd (getcwd reads it back) */
    if (chdir("/") != 0) return 19;
    char buf[64];
    if (getcwd(buf, sizeof(buf)) == 0) return 20;
    if (buf[0] != '/' || buf[1] != '\0') return 21;

    puts("setenv:round-trip-ok");
    return 42;
}
