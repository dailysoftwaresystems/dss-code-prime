/* D-FFI-PE-IMPORT-THUNK (c112): the address-taken-import RUN witness.
 *
 * `g_table` is statically initialized with the ADDRESS of an imported
 * function (`puts` from the shipped C runtime) — the exact shape of
 * sqlite's os_win.c `aSyscall[]` table (`(SYSCALL)Win32Func`). main
 * loads that pointer and CALLS it indirectly.
 *
 * On PE/Windows a format that resolves an extern's ADDRESS to the raw
 * `.idata` IAT *data* slot (the retired `indirect-slot` model) makes
 * this `call *ptr` jump INTO the import table and execute data as code
 * -> 0xC0000005. The fix: the PE linker synthesizes one `jmp *[IAT
 * slot]` import THUNK per extern (the ELF-PLT / Mach-O-__stubs analog)
 * and `externCallDispatch: direct-plt` points the extern's VA at its
 * THUNK -> a callable code address -> the indirect call reaches `puts`.
 * ELF/Mach-O already ran this green via their PLT / __stubs.
 *
 * The `volatile` load defeats any fold of the indirect call into a
 * direct `puts(...)`, so the address-taken path survives to runtime in
 * BOTH the baseline and the release (optimized) arm. A correct run
 * prints the line and returns 42; a wrong thunk access-violates (no
 * exit 42). RED-on-disable: flip the pe64 exec format's
 * `externCallDispatch` back to `indirect-slot` (or drop the pe.cpp
 * thunk emission) -> PE crashes 0xC0000005 instead of exiting 42.
 */
#include <stdio.h>

typedef int (*putfn)(const char *);

static putfn g_table[] = { puts };

int main(void) {
    volatile putfn f = g_table[0];
    if (((putfn)f)("addr_import: indirect import-ptr call OK") < 0) {
        return 91;
    }
    return 42;
}
