/* ═══ DSS PLATFORM RUNTIME — `access` for the `pe` object format ════════════
 *
 * The IMPLEMENTATION half of <io.h>'s `access`; the DECLARATION half is its row
 * in `src/dss-config/shippedLibs/io.json`, whose `realization` names this unit.
 *
 * ★ WHY A BODY AND NOT THE PLATFORM'S `_access`. No Windows image exports
 * `access`. The two reference toolchains reach it by different roads, and only
 * one of them answers every mode POSIX defines (MEASURED, P69 fold 3f,
 * probe-reference-cc on windows-x86_64-release and windows-x86_64-debug):
 *   - cl + link.exe: OLDNAMES aliases `access` to UCRT's `_access`, which
 *     refuses mode X_OK (1) through the invalid-parameter handler -- the image
 *     dies 0xC0000409 with no handler installed, and answers -1 / EINVAL with one;
 *   - mingw-w64 gcc: binds its own `access` body (libmsvcrt.a's ucrt-access.o),
 *     which answers X_OK -- 0 for an existing file and an existing directory.
 * One working reference makes the meaning required, so DSS's `access` answers
 * X_OK the way the working one does: the X_OK bit is DROPPED (on Windows every
 * existing file and directory is "executable" in the sense POSIX asks for), and
 * the rest of the mode is asked of `_access`, so F_OK, W_OK and R_OK keep UCRT's
 * answers and its errno.
 * COST, recorded on the row: where mingw-w64's body would answer another mode
 * differently from `_access` (a mode bit outside the four, a directory under
 * W_OK), DSS answers as UCRT does. INFERRED, not measured.
 *
 * ★ THE SIGNATURE IS CHECKED BY THE COMPILER: this unit includes <io.h>, so the
 * row's declaration is in scope and the definition must match it.
 */

#include <io.h>

int access(char *path, int mode) {
    return _access(path, mode & ~X_OK);
}
