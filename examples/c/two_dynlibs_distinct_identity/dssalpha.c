/* [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]] — the LIBRARY that makes
 * the call, and the shape a DSS-built darwin dylib could not carry at all
 * until this cycle.
 *
 * `alpha_answer` calls `alpha_weak_part`, a WEAK definition IN THIS SAME
 * LIBRARY. Under both loaders that call may not be a direct branch: dyld and
 * ld.so coalesce weak definitions across images, so another image in the
 * process may win the name, and a library that branched to its own body would
 * answer one identifier differently from every other image — silently.
 * ✔MEASURED on Apple Silicon (macOS 25.6.0, Apple clang 21.0.0 / ld-1267): the
 * ld64-built dylib routes it `bl … symbol stub for: _w` over a `__DATA_CONST
 * __got` slot bound `<weak-def-coalesce>`, and a DSS-built one returned its own
 * body (rc 1) where ld64's returned the consumer's (rc 2) until the walker
 * learned the LC_DYLD_INFO_ONLY weak-bind stream.
 *
 * ⚠ WHAT THIS ARM OF THE CORPUS CAN AND CANNOT WITNESS, stated so the entry is
 * not read as more than it is. A corpus example builds ONE process, so it
 * cannot stage a RIVAL definition in a second image — the divergence itself is
 * witnessed by execution outside this harness (a DSS dylib under an
 * Apple-clang executable defining a rival weak body: rc 2, against the
 * ld64-built control's rc 2, on the operator's Apple Silicon host). ⚠ That
 * hardware run covers ONE configuration: its "release" arm shipped a file
 * byte-identical to the debug one, because the tiny witness source gave the
 * release pipeline nothing to transform. THIS file is the subject that fixes
 * that — its release artifact genuinely differs (`alpha_extra` inlines away),
 * and the weak-bind stream plus the `__stubs`-routed call survive unchanged
 * across both configurations. What this file pins is that the routed shape BUILDS, LINKS
 * and still answers correctly on every format — including the two that declare
 * nothing preemptible and must keep the direct branch. Before the Mach-O
 * realization landed, this library was REFUSED outright on both darwin dylib
 * targets, by name.
 *
 * `alpha_extra` is an ordinary inlinable helper with a loop-invariant addend,
 * so the `release` arm has real work here and the library's bytes differ from
 * the baseline's — the manifest declares `mustDifferFromBaseline` per
 * dependency, and an arm that transformed nothing would compare x == x. */

__attribute__((weak)) int alpha_weak_part(void);

static int alpha_extra(int acc, int addend) {
    return acc + addend;
}

__attribute__((weak)) int alpha_weak_part(void) {
    int acc    = 0;
    int base   = 3;
    int addend = 0;
    int k      = 4;
    while (k) {
        addend = base + 1;            /* loop-invariant: 3 + 1 == 4 */
        acc    = alpha_extra(acc, addend);
        k      = k - 1;
    }
    return acc;                       /* 4 iterations x 4 == 16 */
}

int alpha_answer(void) {
    /* The self-call the row is about: a WEAK, externally-visible definition
     * reached from inside the same shared library. */
    return alpha_weak_part() + 4;     /* 16 + 4 == 20 */
}
