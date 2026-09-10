/* [[D-FFI-SHIPPED-DESCRIPTORS-DECLARE-STRUCTS-THEIR-OWN-SIGNATURES-DO-NOT-USE]]
 * — the RUNNABLE witness that the defect `dirent.json` had was SYSTEMIC, and
 * that the mechanism which fixed it generalises: <sys/stat.h> and <time.h> now
 * type their signatures over the structs those same files already declare.
 *
 * ★★ THE TWO HALVES OF THIS ROW ARE NOT EQUALLY OBSERVABLE, AND SAYING SO IS
 *   PART OF THE PIN. A RETURN type is visible in an expression's type, so it
 *   can be interrogated from C. A PARAMETER type is not: `struct stat *`
 *   converts to `void *` silently in both directions, so a call written the
 *   ordinary way compiles identically against either spelling and NO runnable
 *   program can tell them apart. The parameter half is therefore pinned where
 *   it IS observable — as a refusal, at the semantic tier, by
 *   `ShippedStatTypedSurface.*` in `tests/ffi/test_shipped_stat_typed_surface.cpp`.
 *   This file pins the return half plus the field-offset consequence, and
 *   claims nothing about the rest.
 *
 * ★★ ARM (1) — THE TYPING WITNESS, AN EXPRESSION-TYPE QUESTION. `sizeof` does
 *   not evaluate its operand, so `sizeof(*localtime(&t))` asks exactly one
 *   thing: what does `localtime`'s OWN declared return type point at? Under the
 *   shipped-before signature `fn(ptr<void>) -> ptr<void>` the operand is `void`
 *   — `sizeof(void)` is 1 where the GNU extension is honoured and a hard error
 *   where it is not, and BOTH are a red here. Under the shipped signature
 *   `fn(ptr<void>) -> ptr<tm>` it is the platform's own broken-down time.
 *   Writing `struct tm *p = localtime(&t); sizeof(*p)` would have measured the
 *   DECLARATION on the left instead — the mistake that makes a pin vacuous.
 *
 * ★★ ARM (2) — THE RUNTIME WITNESS, AND WHY IT IS `st_mode` AND NOT `sizeof`.
 *   A size check cannot discriminate a right layout from a wrong one of the
 *   same size (the lesson `shipped_stat_macho` records: macho and x86-64-elf are
 *   both 144 bytes). `st_mode` sits at a DIFFERENT OFFSET and a DIFFERENT WIDTH
 *   on every leg — ✔MEASURED by execution on three of them: byte 24 as u32 on
 *   x86-64 glibc (gcc 13.3.0 native), byte 16 as u32 on aarch64 glibc (cross
 *   gcc, run under qemu-aarch64), byte 6 as u16 on pe/UCRT (mingw-w64 gcc
 *   13.2.0 native) and byte 4 as u16 on Darwin (Apple clang 21.0.0, macOS
 *   26.6.2 arm64, measured on the operator's own hardware) — so reading a real
 *   directory's mode back through it is a value
 *   check that a wrong offset cannot pass. `.` is a directory on all four legs
 *   by construction, and the NEGATIVE (`S_ISREG` must be false) is asserted too,
 *   so a mode that degenerated to 0 fails instead of passing arm 2 by accident.
 *
 * ★ TARGET-AGNOSTIC BY CONSTRUCTION — not one size or offset literal appears.
 *   `struct tm` is 56 bytes on glibc AND on Darwin (✔MEASURED on both — nine
 *   ints, then tm_gmtoff and tm_zone) and 36 on the UCRT (✔MEASURED — nine
 *   ints, no BSD tail); `struct stat` is 144 (x86-64 elf) / 128 (aarch64 elf) /
 *   48 (pe) / 144 (macho, with a COMPLETELY different field order), all four
 *   ✔MEASURED BY EXECUTION. Both sides of every comparison derive from the same
 *   declared type, so the four legs need one expression between them.
 *
 * ⚠ WHAT IS DELIBERATELY ABSENT. `lstat` is elf/macho-only, so a call to it
 *   would make this example unbuildable on pe rather than agnostic; it is pinned
 *   in the unit fixture instead. And `fstat` is CALLED but its result is not
 *   asserted: under a test runner stdin may be a pipe, a console or closed, and
 *   an assertion over that would be a claim about the harness, not about DSS.
 *   The call is here because it must still COMPILE against the typed parameter.
 *
 * RED-ON-DISABLE (REMOVE direction, over the shipped descriptors; config only,
 * no rebuild — the config snapshot is taken at ctest RUN time): restore
 * `time.json`'s `localtime` rows to `"fn(ptr<void>) -> ptr<void>"` and arm (1)
 * reds on every leg (or refuses to compile, where `sizeof(void)` is not an
 * extension). Restore `sys/stat.json`'s `stat` rows to `ptr<void>` and THIS
 * FILE STAYS GREEN — which is the honest attribution, and why the parameter half
 * lives in a fixture that can assert a refusal.
 */
#include <sys/stat.h>
#include <time.h>

int main(void) {
    struct stat st;
    time_t      t = 0;

    /* (1) THE TYPING WITNESS — unevaluated, so no clock is read and no static
     *     buffer is touched; only the declared return type is consulted. */
    if (sizeof(*localtime(&t)) != sizeof(struct tm)) return 4;
    /* The same claim from the other side, so a leg on which `sizeof(struct tm)`
     * somehow degenerated to 1 cannot pass the line above by accident. */
    if (sizeof(*localtime(&t)) == 1u)               return 5;

    /* (2) THE RUNTIME WITNESS — a real libc/UCRT fill through the newly typed
     *     `struct stat *` parameter, read back at the per-target offset. */
    if (stat(".", &st) != 0)  return 1;
    if (!S_ISDIR(st.st_mode)) return 2;
    if (S_ISREG(st.st_mode))  return 3;

    /* (3) the fd-taking sibling must still COMPILE against the typed parameter;
     *     its RESULT is a property of the harness's stdin, so it is not asserted. */
    (void)fstat(0, &st);

    return 42;
}
