/* D-LK-WEAK-UNDEFINED-SYMBOL-NAMED-DIRECTLY-IS-NOT-ADDRESS-ZERO (P69): the RUN
 * witness for the ADDRESS of a weak symbol nothing defines, taken in STATIC DATA.
 *
 * `wd` and `wf` are declared weak and no translation unit defines them, so each
 * resolves to nothing and its address is NULL — in code (`if (&wd)`, the
 * weak_extern_import_null example) and in a static initializer alike. A pointer in
 * static data names the symbol DIRECTLY: it is a relocation against the symbol
 * itself, not a read through a slot, so it holds the symbol's VALUE, which is 0.
 *
 * ✔MEASURED that the references run THIS program to 42 (2026-10-07, dssharness
 * probe-reference-cc, this file's code): gcc 13.3.0 + GNU ld 2.42 (-O0 -no-pie,
 * -O2 -pie), clang 18 + ld.lld 18 (-O0 -no-pie, -O2 -pie) and clang 18 + lld-link
 * on PE, run 20261007-081305-8067e92f; aarch64 gcc 13 + GNU ld (-O0 -no-pie, -O2
 * -pie), run 20261007-081354-4f1e0abe.
 *
 * WHAT WENT WRONG BEFORE: a datum resolved to nothing was given a NULL SLOT — a
 * pointer-sized constant 0 — and the slot WAS the symbol, which is right for a
 * read through it and wrong for a reference that names the symbol: `pwd` held
 * the slot's own non-null address, and on pe64 this program's first test went
 * the wrong way (✔MEASURED at the merged P69 tree: exit 1 for `p == 0 ? 42 : 1`).
 * A weak FUNCTION was refused outright on every image, so `pwf` did not link.
 *
 * THE PROGRAM. Three questions, one exit code: `pwd` must be NULL (+3), `pwf`
 * must be NULL (+4), and the CONTROL `ppresent` — an ordinary global's address
 * in the same shape — must not be, and must read 35 (+35). 3 + 4 + 35 = 42;
 * answering any of the three the other way adds 100 or skips 35.
 *
 * ANTI-FOLD. The three pointers are MUTABLE globals, so each test is a load, not
 * a literal; the `release` arm is what proves no pass folds "a declared object's
 * address is never null" into the first two tests.
 *
 * RED-on-disable: classify a data item's absolute pointer as a read through a
 * slot (`classifyWeakNullReference`, link/weak_resolved_to_nothing.hpp — the
 * pre-P69 shape, where the null slot was the symbol) -> `pwd` and `pwf` hold
 * the slot's address -> exit 235 (100 + 100 + 35).
 */

extern int wd __attribute__((weak));
extern void wf(void) __attribute__((weak));

int present = 35;

int *pwd = &wd;
void (*pwf)(void) = wf;
int *ppresent = &present;

int main(void)
{
    int r = 0;
    if (pwd == 0) {
        r += 3;
    } else {
        r += 100;
    }
    if (pwf == 0) {
        r += 4;
    } else {
        r += 100;
    }
    if (ppresent != 0 && *ppresent == 35) {
        r += *ppresent;
    }
    return r;
}
