/* c156 D-LK-ELF-SYMBOL-VERSIONING end-to-end witness — the realpath NULL/
** malloc form binds its DEFAULT glibc version instead of misbinding to the
** oldest compat instance.
**
** realpath(path, NULL) mallocs + returns the resolved absolute path. On
** x86_64 glibc the pre-2.3 compat `realpath@GLIBC_2.2.5` EINVALs a NULL
** resolved buffer; the `@@GLIBC_2.3` default accepts it. Before c156 DSS's
** dynamic imports carried NO symbol-version info, so an UNVERSIONED reference
** misbound to `@GLIBC_2.2.5` and this call returned NULL (exit 3). c156 pins
** realpath's x86_64/elf requirement to GLIBC_2.3 through the descriptor's
** per-symbol `version` field, so the ELF writer emits `.gnu.version_r` and
** ld.so binds GLIBC_2.3 — the call now RUNS (readelf -V shows
** `realpath@GLIBC_2.3`; LD_DEBUG=bindings shows `[GLIBC_2.3]`).
**
** W3 (opt-in regression) rides the SAME source on TWO axes:
**   1. arm64 glibc has a SINGLE realpath (GLIBC_2.17) → NO variant matches →
**      the import stays UNVERSIONED (no `.gnu.version_r`), and the identical
**      source binds + runs (a versionless import is untouched by c156);
**   2. the UNVERSIONED sibling `strtoll` in the SAME x86_64 binary (whose
**      `.gnu.version` slot stays *global* = 1) still binds + returns 42.
**
** Every branch feeds a DISTINCT exit code — a wrong result is attributable,
** never a silent pass. RED-ON-DISABLE: drop realpath's descriptor `version`
** and the x86_64 leg misbinds `@GLIBC_2.2.5` → realpath returns NULL → exit
** 3. gcc -std=c17 -D_DEFAULT_SOURCE cross-checked on linux (exit 42). */

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    /* The VERSIONED symbol, NULL/malloc form — the reproduced bug. */
    char *rp = realpath(".", NULL);
    if (rp == NULL) return 3;      /* the pre-c156 misbind (EINVAL) */
    if (rp[0] != '/') return 4;    /* must be an absolute path */
    free(rp);

    /* An UNVERSIONED sibling import in the SAME binary still binds. */
    long long v = strtoll("42", NULL, 10);
    if (v != 42) return 5;

    fputs("realpath-versioned-ok\n", stdout);
    return (int)v;                 /* 42 */
}
