/* D-FFI-PE-IMPORT-THUNK (c112): the address-taken-import RUN witness.
 *
 * `g_table` is statically initialized with the ADDRESS of an imported
 * function (`puts` from the shipped C runtime) — the exact shape of
 * sqlite's os_win.c `aSyscall[]` table (`(SYSCALL)Win32Func`). main
 * loads that pointer and CALLS it indirectly.
 *
 * WHAT THE POINTER HOLDS ON PE CHANGED TWICE. Before c112 the address
 * resolved to the raw `.idata` IAT *data* slot, so this `call *ptr`
 * executed data as code -> 0xC0000005. From c112 it was the image's
 * synthesized `jmp *[IAT slot]` import THUNK: callable, but not the
 * function's address. Since P69 (design c2,
 * D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB) the slot is the
 * FirstThunk of an import descriptor of its own, so the LOADER writes
 * `puts`'s own address into it — the value GetProcAddress answers
 * (examples/c/library_function_address_equals_getprocaddress compares
 * the two). A direct call still goes through the thunk
 * (`externCallDispatch: direct-plt`).
 *
 * The `volatile` load defeats any fold of the indirect call into a
 * direct `puts(...)`, so the address-taken path survives to runtime in
 * BOTH the baseline and the release (optimized) arm. A correct run
 * prints the line and returns 42. RED-on-disable: stop emitting the
 * slot's own import descriptor in pe.cpp -> the slot keeps its lookup
 * entry (an RVA, not code) -> the indirect call faults (no exit 42).
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
