/* [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]] — the CONTROL direction,
 * as a runnable program on every leg.
 *
 * THE ROW IS ABOUT A SHARED LIBRARY. A `.so` / `.dylib` that BRANCHES to its
 * own body for a name the LOADER may have coalesced away answers one
 * identifier differently from every other image in the process, silently.
 * MEASURED before the fix: a DSS-built ELF `.so` returned its OWN bodies where
 * the gcc-built `.so` from identical source returned the executable's; a
 * DSS-built darwin dylib did the same against ld64's (cycle P61, Apple
 * Silicon). Such a call is now routed through the format's loader-resolved
 * reference — an ELF PLT stub over a GOT slot — for the bindings the format
 * DECLARES preemptible.
 *
 * THIS EXAMPLE IS THE OTHER HALF, AND IT IS NOT A LESSER ONE. An EXECUTABLE
 * declares nothing preemptible, because the main executable is ALWAYS its own
 * winner under both loaders — MEASURED: gcc 13.3.0 and clang 18.1.3 bind an
 * executable's calls to its own weak AND its own strong definitions DIRECTLY,
 * `-pie` and `-no-pie` alike, while PLT-routing the very same callees in a
 * `.so` built from the same file. So an executable must keep every self-call a
 * direct branch: routing it would cost every ordinary call a load through
 * memory to reach a definition nothing can replace, and — on an ELF or Mach-O
 * EXEC, whose artifacts cannot carry a reference resolved from the loader's
 * global scope — would REFUSE the program outright.
 *
 * A corpus example cannot build the shared library and its rival consumer
 * together, so the divergence itself is witnessed by execution outside this
 * harness (`.temp/p62-wc-scratch/elf-run-witness*.sh`, DSS `.so` rc 6 against
 * the gcc control's rc 6, on x86_64 and on arm64 under qemu). What THIS file
 * pins is that the routing does not leak into the flavour that must not have
 * it: the program builds, links, loads and returns its OWN answer on all four
 * legs, and it does so from a shape that carries every callee flavour the
 * routing decision distinguishes.
 *
 * WHY THE SHAPE. Every callee is reached through a const function-pointer
 * table indexed by a RUNTIME value (`argc - 1`, zero with no arguments), so
 * neither pipeline can fold the index and drop a definition before it reaches
 * the call-lowering decision. The four flavours are exactly the four cells of
 * that decision — weak/default, strong-global/default, `static`, and
 * hidden-visibility — and in a `.so` the first two would be PLT-routed while
 * the last two stayed direct.
 *
 * Exit arithmetic, with no arguments (`argc - 1 == 0`):
 *   table[0] = weak_answer, weak_answer(40) = 40 + strong_bias() = 40 + 2 = 42.
 */

/* (1) WEAK / DEFAULT — coalescible under BOTH loaders. */
__attribute__((weak)) int weak_answer(int v);

/* (2) STRONG GLOBAL / DEFAULT — interposable in an ELF shared object (gcc and
 * clang both emit `call <st@plt>` for it there), NOT in a Mach-O dylib. The
 * two ecosystems disagree, which is why the set is declared per format. */
int strong_bias(void) { return 2; }

/* (3) `static` — module-private, in no image's dynamic export set. Never
 * routed anywhere, under any format. */
static int hidden_double(int v) { return v * 2; }

/* (4) hidden visibility — externally defined but not exported, the source's
 * own opt-out from interposition. Never routed either. */
__attribute__((visibility("hidden"))) int unexported_triple(int v) {
    return v * 3;
}

__attribute__((weak)) int weak_answer(int v) { return v + strong_bias(); }

typedef int (*IntFn)(int);

static IntFn const dispatch[3] = { weak_answer, hidden_double,
                                   unexported_triple };

/* An ordinary inlinable wrapper, so the release arm has real work to do and is
 * not byte-identical to the baseline (the manifest declares
 * `mustDifferFromBaseline`, and an arm that transformed nothing compares
 * x == x and cannot fail). */
static int route(int v, int which) {
    if (which < 0 || which > 2) which = 0;
    return dispatch[which](v);
}

int main(int argc, char **argv) {
    (void)argv;
    return route(40, argc - 1);
}
