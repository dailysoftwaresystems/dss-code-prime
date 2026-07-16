/* c156 D-LK-ELF-SYMBOL-VERSIONING silent-hole witness: the goal-2 SUPPRESSED
** shipped path carries the required symbol version through a user redeclaration.
**
** A user prototype re-declares realpath WHILE #include <stdlib.h> also ships it
** (a common feature-test-macro pattern). goal-2 suppresses the descriptor's own
** versioned realpath injection, and THIS prototype synthesizes the import
** instead. The required version (GLIBC_2.3 on x86_64) must ride through the
** suppression map, or the synthesized import binds UNVERSIONED and ld.so
** silently misbinds realpath@GLIBC_2.2.5 -- whose pre-2.3 form EINVALs a NULL
** resolved buffer -- so realpath(".", NULL) returns NULL (exit 3), zero
** diagnostic. This path was untested when the trio landed, which is exactly why
** the descriptor-only witness was green over the hole.
**
** x86_64: readelf -V shows realpath@GLIBC_2.3; the NULL/malloc form returns a
** real absolute path. arm64: realpath is single-versioned (GLIBC_2.17), so the
** suppression map records an empty version, the import stays UNVERSIONED, and
** the SAME source runs (the opt-in regression control on the suppressed path).
**
** RED-ON-DISABLE (witnessed): neuter the version at the synthesized
** HirExternRecord (cst_to_hir.cpp) or drop it from the suppression map
** (semantic_analyzer.cpp) -> x86_64 emits no .gnu.version_r -> ld.so misbinds
** realpath@GLIBC_2.2.5 -> realpath returns NULL -> exit 3. gcc -std=c17
** -D_DEFAULT_SOURCE cross-checked on linux (exit 42). */

#include <stdlib.h>
#include <stdio.h>

/* The user redeclaration that triggers goal-2 suppression of the shipped,
** versioned realpath -- the synthesized import must still bind GLIBC_2.3. */
char *realpath(const char *path, char *resolved);

int main(void) {
    char *rp = realpath(".", NULL);
    if (rp == NULL) return 3;      /* the pre-fix misbind (EINVAL on NULL) */
    if (rp[0] != '/') return 4;    /* must be an absolute path */
    free(rp);

    fputs("redecl-versioned-ok\n", stdout);
    return 42;
}
