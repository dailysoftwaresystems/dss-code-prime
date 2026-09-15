/* [[D-FFI-DIRENT-API-DECLARED-OVER-VOID-NOT-ITS-OWN-STRUCTS]] — the RUNNABLE
 * witness that <dirent.h>'s three functions are typed over the API's OWN
 * structs (`DIR`, `struct dirent`) instead of over `void`.
 *
 * ★ WHY A SECOND dirent EXAMPLE EXISTS BESIDE `shipped_dirent_readdir`. That one
 *   proves the WALK — that opendir/readdir/closedir run and that `d_name` sits
 *   where the per-format `struct dirent` variant says it does. It passed
 *   throughout the defect, and it had to: `void *` converts to `struct dirent *`
 *   silently in C, so a walk written the ordinary way cannot tell a typed
 *   descriptor from an untyped one. THE PROPERTY UNDER TEST HERE IS THE TYPE OF
 *   THE CALL EXPRESSION ITSELF, which no amount of walking reaches.
 *
 * ★★ THE WITNESS, AND WHY IT IS AN EXPRESSION-TYPE QUESTION RATHER THAN A
 *   VARIABLE'S. `sizeof` does not evaluate its operand, so `sizeof(*readdir(d))`
 *   asks exactly one thing: what does readdir's OWN declared return type point
 *   at? Under the shipped-before signature `fn(ptr<void>) -> ptr<void>` the
 *   operand is `void` — `sizeof(void)` is 1 where the GNU extension is honoured
 *   and a hard error where it is not, and BOTH are a red here. Under the shipped
 *   signature `fn(ptr<DIR>) -> ptr<dirent>` it is the platform's own record.
 *   Writing `struct dirent *e = readdir(d); sizeof(*e)` would have measured the
 *   DECLARATION on the left instead, which is the mistake that makes a pin
 *   vacuous.
 *
 * ★ TARGET-AGNOSTIC BY CONSTRUCTION — no size literal appears. `struct dirent`
 *   is 280 bytes on both glibc legs, 268 on pe (mingw-w64's LLP64 layout, which
 *   DSS's own `runtime/platform/src/dirent.c` realizes) and 1048 on Darwin's
 *   64-bit-inode ABI; both sides of the comparison derive from the same declared
 *   type, so the four legs need one expression between them.
 *
 * ★ PLACED AFTER THE WALK ON PURPOSE. `sizeof` is unevaluated in ISO C, but this
 *   example must not DEPEND on that to stay hermetic: by the time it runs the
 *   stream is exhausted, so an implementation that did evaluate the operand
 *   would call `readdir` once more on an exhausted handle, which every one of
 *   the three implementations answers with a null pointer and no side effect.
 *
 * ⚠ WHAT THIS FILE DOES NOT CLAIM. `opendir`'s return type changing from
 *   `ptr<void>` to `ptr<DIR>` has NO runtime signature — both are one address —
 *   and inventing an arithmetic that pretended otherwise would be an instrument
 *   answering an adjacent question. `DIR`'s typing is pinned where it is
 *   observable, at the semantic tier, by
 *   `ShippedDirentTypedSurface.*` in `tests/ffi/test_shipped_dirent_typed_surface.cpp`.
 *
 * RED-ON-DISABLE (REMOVE direction, over the shipped descriptor): restore
 * `dirent.json`'s `readdir` row to `"fn(ptr<void>) -> ptr<void>"` and this
 * example fails on every leg — the `sizeof` comparison reads 1 instead of the
 * platform record's size where `sizeof(void)` is accepted, and the compile
 * refuses outright where it is not.
 */
#include <dirent.h>
#include <string.h>

int main(void) {
    DIR           *d;
    struct dirent *e;
    int            found_dot = 0;

    /* No cast and no `void *` staging post anywhere in this file: opendir's
     * declared return type IS `DIR *`, and `DIR *` IS what readdir and closedir
     * declare they take. */
    d = opendir(".");
    if (d == 0) return 1;

    /* Every directory contains "." (POSIX), so the walk must find it — and
     * finding it BY NAME re-proves the `d_name` offset of whichever per-format
     * variant this leg selected. */
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0) found_dot = 1;
    }

    /* THE TYPING WITNESS. */
    if (sizeof(*readdir(d)) != sizeof(struct dirent)) { closedir(d); return 4; }
    /* The same claim from the other side, so a leg on which `sizeof(struct
     * dirent)` somehow degenerated to 1 cannot pass the line above by accident. */
    if (sizeof(*readdir(d)) == 1u)                    { closedir(d); return 5; }

    if (closedir(d) != 0) return 2;
    return found_dot ? 42 : 3;
}
