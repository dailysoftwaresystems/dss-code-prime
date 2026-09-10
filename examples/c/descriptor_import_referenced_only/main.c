/* D-FFI-DESCRIPTOR-EAGER-IMPORT — the RUN witness for REFERENCED-ONLY import of
 * shipped-descriptor symbols, and specifically for the direction that can break a
 * working program: OVER-pruning.
 *
 * WHAT CHANGED. A `#include`d descriptor's symbols used to be EAGER — bound into
 * the image whether or not the translation unit referenced them. ✔MEASURED at
 * fcb3a9d7 on this very source's header set, by walking each format's real import
 * pointer chain: THIS source imports 86 symbols on elf64-x86_64, 86 on
 * elf64-aarch64, 85 on pe64, 97 on macho64-x86_64 and 97 on macho64-arm64, while
 * referencing four. With referenced-only import: 5 / 5 / 8 / 5 / 5.
 * gcc 13.3.0, clang 18.1.3, MSVC 19.51 and mingw-w64 gcc 13.2.0 each import ONLY
 * what the TU references — measured at both the object tier (`nm -u` /
 * `dumpbin /symbols`) and the linked-image tier (`nm -D` / `dumpbin /imports`),
 * each arm with a control that fired. So referenced-only is reference BEHAVIOUR,
 * not an optimisation.
 *
 * ★ AND THE DIVERGENCE WAS INTERNAL. C23 7.1.4p2 entitles a program to declare a
 * library function ITSELF instead of including its header, and calls the two
 * equivalent. ✔MEASURED in one eager tree: the same names hand-declared import 3
 * symbols on elf and 3 on pe; `#include`d, 86 and 85. Two spellings the standard
 * calls equivalent produced two different programs at the LOADER.
 *
 * WHY THIS EXAMPLE EXISTS AT ALL, stated honestly. The dangerous direction of this
 * change is not "one import too many" — it is one import too FEW, which is a
 * program that no longer loads or that calls through a slot nothing filled. Until
 * this change the linker's reference gate never even RAN for an ordinary
 * `#include`-only program: `rejectOrDropUnreferencedExterns` short-circuits on
 * "every named import is eager" and returns before its reference scan. Every
 * descriptor row was eager, so the scan was dead code for this shape of program.
 * It is now live for every such program, and this example is the corpus witness
 * that it keeps each reference shape that reaches it:
 *
 *   SHAPE 1  a DATA-ITEM relocation — `g_table` is statically initialized with an
 *            imported function's address (the sqlite `aSyscall[]` shape). The gate
 *            scans data-item relocations as well as function ones; if it did not,
 *            this import is dropped and the indirect call reaches a null slot.
 *   SHAPE 2  an ordinary direct CALL relocation (`strlen`).
 *   SHAPE 3  an imported DATA OBJECT (`stdout`), which is lowered GOT-INDIRECT on
 *            elf/macho and reached through `__acrt_iob_func` on pe — two different
 *            lowerings of one source construct, and the one whose reference is
 *            easiest for a reloc-based scan to miss.
 *
 * And every other name these three headers declare — 80-odd of them — is
 * UNREFERENCED and must NOT be imported. That negative half cannot be asserted
 * from inside a running program (nothing portable lets a process read its own
 * import table), so it is pinned where artifact CONTENT can be read, by
 * `tests/ffi/test_descriptor_import_referenced_only.cpp`, which walks the emitted
 * elf/pe/macho import tables directly. Same split, same reason, as
 * `project_module_standalone_build`'s archive-magic pin.
 *
 * RED-ON-DISABLE: restore `ShippedExternSymbol::eagerImport`'s default to `true`
 * and this example still exits 42 — it is the OVER-pruning guard, and it is
 * deliberately green in both states. The DROP half reds in the unit pin above.
 * The arm that reds HERE is an under-keeping one: delete the data-item half of the
 * gate's reference scan (`for (auto const& di : m.dataItems)`) and SHAPE 1's
 * import is dropped, so the indirect call through `g_table[0]` reaches nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*putfn)(char const *);

/* SHAPE 1 — the address of an imported function, in a DATA item's initializer. */
static putfn const g_table[] = { puts };

int main(void) {
    /* SHAPE 2 — a direct call to an imported function. */
    if (strlen("referenced-only") != 15u) {
        return 91;
    }

    /* SHAPE 3 — an imported DATA OBJECT reached through a call that takes it. */
    if (fputs("descriptor_import_referenced_only: ok\n", stdout) < 0) {
        return 92;
    }

    /* SHAPE 1, consumed. `volatile` defeats any fold of the indirect call back
     * into a direct one, so the address-taken path survives to run time in the
     * release arm as well as the baseline. */
    {
        volatile putfn f = g_table[0];
        if (((putfn)f)("descriptor_import_referenced_only: indirect ok") < 0) {
            return 93;
        }
    }

    return 42;
}
