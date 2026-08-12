/* c85 (shell.c `atexit(abnormalExit)`): atexit ships as the per-format
   MACRO/SYMBOL SPLIT — the errno.json pattern, pure config:

   - elf: glibc exports NO dynamic `atexit` (a libc.so.6-bound import fails
     at LOAD — witnessed c82). `atexit(f)` is therefore a FUNCTION-LIKE
     MACRO (C 7.1.4p1 allows any library function to additionally be one)
     expanding to `__cxa_atexit((void (*)(void *))(f), 0, 0)` — byte-equal
     to glibc's own libc_nonshared.a shim (Itanium ABI: dso_handle=NULL
     registers against the main program). `__cxa_atexit` is the real
     import (exported T, single-version, both arches).
   - pe: a MACRO TOO since TF-C111 — a PLAIN RENAME `atexit` -> `_crt_atexit`
     (object-like, no parameter list, and the two signatures are BYTE-IDENTICAL
     `fn(ptr<fn() -> void>) -> i32`, so no adapter — unlike the elf arm's
     3-argument reshape above). ✔MEASURED on two instruments (`objdump -p` +
     a direct PE export-directory parse): ucrtbase.dll exports NO `atexit` AT
     ALL; UCRT's `atexit` is a static-lib shim over the exported `_crt_atexit`
     (ordinal 203) — the same shape as glibc's libc_nonshared.a shim over
     `__cxa_atexit`. THE OLD TEXT HERE READ "no macro variant (0-match => not
     injected); the DIRECT msvcrt.dll export [1068]", and it is kept only to
     say that it is now FALSE: msvcrt does export `atexit` (ordinal 1069, and
     no `_crt_atexit`), but `library.pe` is ucrtbase.dll now, so BOTH halves
     died with the flip.
   - pe DELIVERY moved in the SAME commit, which is why this example is
     load-bearing rather than decorative: `_crt_atexit` registers fine but its
     callbacks NEVER FIRE on return-from-main under the old spine, because DSS
     terminated via kernel32 `ExitProcess` and ucrtbase's DLL-detach flushes
     streams WITHOUT running the app's onexit table (msvcrt's detach DID run
     it — that is the regression the migration would otherwise have introduced
     SILENTLY). pe64-x86_64-windows-exec.format.json's `processExit` is
     therefore the ucrtbase `exit` import, uniform with elf's libc `exit` and
     macho's libSystem `_exit`, and C-conformant (5.1.2.2.3).
   - macho: the DIRECT `atexit` symbol, and the SYMBOL row narrowed to
     ["macho"] ALONE when pe became a macro. Darwin's libSystem exports
     `_atexit` (c117), so `atexit(f)` binds it directly and NEITHER macro
     variant matches (0-match => un-injected) — no __cxa_atexit split needed,
     that is the glibc-only static-shim workaround.

   The witness proves the handler RUNS AT EXIT, not merely that the call
   compiles:
   - registration must return 0 (else exit 10);
   - main writes its marker via puts, then fflush(0) — the C-standard
     flush-ALL form (fflush(NULL), 7.21.5.2p2) — so main's bytes land
     FIRST deterministically. (No `stdout` OBJECT reference: the stream
     objects are declared per-format — elf `stdout` data imports (c84),
     macho `__stdoutp` data imports (c117), and on pe an `__acrt_iob_func`
     ACCESSOR macro rather than a data symbol at all — so touching one would
     drag a second, format-divergent mechanism into a test about atexit. It
     stays stream-object-free.)
   - main returns 42 — return-from-main ≡ exit(42) (C 5.1.2.2.3), and the
     trampoline terminates via the libc-level exit primitive on every
     format (the format schema's processExit contract);
   - exit() runs handlers BEFORE stream flush/close (C 7.22.4.4), so the
     handler's puts lands on the still-open stdout AFTER main's marker;
   - the handler asserts it observed main's LAST write (mainRan==1) — a
     handler running early/spuriously prints nothing and the strict
     stdout assertion fails;
   - the exit code stays main's 42 (a handler cannot change it).

   RED-ON-DISABLE: drop this format's stdlib.json atexit arm (the MACRO
   variant on elf OR on pe, or the ["macho"] SYMBOL row) -> honest S0001
   `got atexit`, never a silently-dropped registration. On pe, restoring
   `processExit` to kernel32 `ExitProcess` while KEEPING the macro is the
   other half of the pin: it compiles and links clean, and the handler line
   simply never appears, so the stdout assertion below is what catches it.
   gcc -std=c17 cross-checked in WSL (exit 42, identical two-line stdout).

   RESIDUAL (pinned in stdlib.json's $comment): `&atexit` / `(atexit)(f)`
   / `#undef atexit` need the REAL function on BOTH macro formats now (elf
   AND pe) — glibc's and UCRT's are both STATIC shims a dynamic-only linker
   cannot bind — honest S0001, never silent. sqlite's shell.c/sqlite3.c are
   grep-verified to use only the plain call form. (The old note here said
   "macho: deferred unshipped"; that ended at c117 — the macho arm SHIPS,
   and expected.json carries its run arm.) */
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
