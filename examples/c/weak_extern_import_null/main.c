/* D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE: the RUN witness for a
 * WEAK extern IMPORT that nothing in the program defines.
 *
 * `ea` is declared `extern` with `__attribute__((weak))` and NO translation
 * unit in this program defines it. That is not an error and never was — it is
 * the whole point of the construct. A weak reference MAY legally resolve to
 * nothing, in which case its address is NULL, and `if (&ea)` is the idiom that
 * observes it: an optional symbol is tested for presence and the program takes
 * the other path when it is absent.
 *
 * ✔MEASURED that the reference toolchains do exactly this, and the property was
 * named before the results were read (the object marks the symbol weak AND a
 * program that tests it for null links with no definition present and RUNS,
 * taking the null branch):
 *   * gcc 13.3.0 and clang 18.1.3 on ELF x86_64 — object symbol reads
 *     `NOTYPE WEAK DEFAULT UND ea`; links with no definition; exits 42.
 *   * clang 18.1.3 for `x86_64-w64-windows-gnu` AND `x86_64-pc-windows-msvc` —
 *     COFF symbol reads `StorageClass: WeakExternal (0x69)` with an Auxiliary
 *     Format 3 record naming an ABSOLUTE value-0 default; the mingw linker
 *     links it with no definition and the PE binary exits 42.
 *   * mingw-w64 gcc 13.2.0 ACCEPTS the attribute and then emits a PLAIN strong
 *     UNDEF, and its own link FAILS — so it is not a working reference for this
 *     construct and casts no vote for its own output.
 *
 * WHAT WENT WRONG BEFORE, and it was silent at every tier that could have
 * caught it: `weak` reached the HIR linkage map and STOPPED. HIR→MIR consumed
 * that map for function DEFINITIONS and GLOBALS only, so the attribute was
 * parsed, understood, recorded, and dropped one layer below where it was
 * recorded. The emitted object marked the undefined symbol STRONG on ELF,
 * Mach-O and COFF alike, and a DSS-linked image refused the program outright.
 *
 * THE PROGRAM. Two questions, one exit code, so a wrong answer to either is
 * visible and they cannot mask each other:
 *   * `&ea` must be NULL          -> the else arm contributes 7.
 *   * `&present` must NOT be NULL -> the then arm contributes 35.
 * 7 + 35 = 42. Reversing EITHER branch changes the exit code, and taking the
 * `ea` THEN arm would additionally read through the null slot.
 *
 * ANTI-FOLD. `ea` is an extern the compiler cannot see the storage of, so
 * nothing constant-folds it; `present` is a MUTABLE global, so its value is a
 * load rather than a literal. The `release` arm is what proves the null
 * resolution survives the optimizer: a pass that assumed "the address of a
 * declared object is never null" would fold `if (&ea)` to TRUE, take the read
 * arm, and load through the null slot — a crash or a wrong exit, never 42.
 *
 * RED-on-disable: delete the `__attribute__((weak))` from `ea`'s declaration.
 * The reference becomes STRONG, nothing defines it, and the link refuses with
 * `K_SymbolUndefined` on every leg — no artifact, no exit 42.
 */

extern int ea __attribute__((weak));

int present = 35;

int main(void)
{
    int r = 0;

    /* The null-resolution branch. `&ea` is NULL because nothing defines `ea`. */
    if (&ea) {
        r += ea;
    } else {
        r += 7;
    }

    /* The control, in the SAME shape: an ordinary global's address is never
     * null, so this arm must go the other way. Without it a build that
     * resolved EVERY symbol address to null would still exit 42. */
    if (&present) {
        r += present;
    } else {
        r += 1;
    }

    return r;
}
