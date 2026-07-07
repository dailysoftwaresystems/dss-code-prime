/* c85 (shell.c `atexit(abnormalExit)`): atexit ships as the per-format
   MACRO/SYMBOL SPLIT — the errno.json pattern, pure config:

   - elf: glibc exports NO dynamic `atexit` (a libc.so.6-bound import fails
     at LOAD — witnessed c82). `atexit(f)` is therefore a FUNCTION-LIKE
     MACRO (C 7.1.4p1 allows any library function to additionally be one)
     expanding to `__cxa_atexit((void (*)(void *))(f), 0, 0)` — byte-equal
     to glibc's own libc_nonshared.a shim (Itanium ABI: dso_handle=NULL
     registers against the main program). `__cxa_atexit` is the real
     import (exported T, single-version, both arches).
   - pe: no macro variant (0-match => not injected); `atexit` is the
     DIRECT msvcrt.dll export (verified [1068] atexit). The trampoline's
     kernel32 ExitProcess runs msvcrt's onexit table at CRT DLL-detach.

   The witness proves the handler RUNS AT EXIT, not merely that the call
   compiles:
   - registration must return 0 (else exit 10);
   - main writes its marker via puts, then fflush(0) — the C-standard
     flush-ALL form (fflush(NULL), 7.21.5.2p2) — so main's bytes land
     FIRST deterministically. (No `stdout` OBJECT reference: the stream
     objects are elf-only data imports (c84) and this example also runs
     the pe leg, so it stays stream-object-free.)
   - main returns 42 — return-from-main ≡ exit(42) (C 5.1.2.2.3), and the
     trampoline terminates via the libc-level exit primitive on every
     format (the format schema's processExit contract);
   - exit() runs handlers BEFORE stream flush/close (C 7.22.4.4), so the
     handler's puts lands on the still-open stdout AFTER main's marker;
   - the handler asserts it observed main's LAST write (mainRan==1) — a
     handler running early/spuriously prints nothing and the strict
     stdout assertion fails;
   - the exit code stays main's 42 (a handler cannot change it).

   RED-ON-DISABLE: drop the stdlib.json atexit macro (elf) or symbol (pe)
   -> honest S0001 `got atexit` — never a silently-dropped registration.
   gcc -std=c17 cross-checked in WSL (exit 42, identical two-line stdout).

   RESIDUAL (pinned in stdlib.json's $comment): `&atexit` / `(atexit)(f)`
   / `#undef atexit` on elf need the REAL function (glibc's is a STATIC
   shim a dynamic-only linker cannot bind) — honest S0001, never silent.
   macho: deferred unshipped (Darwin exports atexit directly; the direct
   symbol lands with a macho consumer). */
#include <stdio.h>
#include <stdlib.h>

static int mainRan = 0;

static void byeHandler(void) {
    /* Runs inside exit(): after main returned, before streams close. */
    if (mainRan == 1) {
        puts("atexit:handler-ran");
    }
}

int main(void) {
    if (atexit(byeHandler) != 0) return 10;  /* registration must succeed */
    mainRan = 1;
    puts("atexit:main-done");
    if (fflush(0) != 0) return 11;           /* flush ALL: main's bytes land FIRST */
    return 42;                               /* == exit(42) -> handler runs */
}
