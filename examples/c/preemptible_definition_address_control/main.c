/* [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]], the ADDRESS half — the
 * CONTROL direction, as a runnable program on every leg.
 *
 * THE ROW IS ABOUT A SHARED LIBRARY. A `.so` / `.dylib` that materializes its
 * OWN body's address for a name the LOADER may replace hands out a pointer no
 * other image in the process agrees with: `&w` inside the library and `&w` in
 * the executable become two different pointers for one identifier, where
 * C 6.2.2p2 gives one identifier one function/object across the whole program.
 * MEASURED 2026-09-05: gcc 13.3.0 and clang 18.1.3 both materialize `&wk`,
 * `&st`, `&wd` and `&sd` inside a `.so` by LOADING a `R_X86_64_GLOB_DAT` GOT
 * slot, while leaving the `static` and `visibility("hidden")` siblings in the
 * SAME object a bare `lea`. The address of such a definition is now read from
 * the format's loader-filled slot for the bindings the format DECLARES
 * preemptible.
 *
 * THIS EXAMPLE IS THE OTHER HALF, AND IT IS NOT A LESSER ONE. An EXECUTABLE
 * declares nothing preemptible, because the main executable is ALWAYS its own
 * winner under both loaders — MEASURED in the same probe: an executable built
 * from the same source materializes ALL EIGHT subjects with a bare `lea`,
 * `-pie` and `-no-pie` alike. So an executable must keep every address direct:
 * routing it would cost a load through memory to reach a definition nothing can
 * replace, and — on an ELF or Mach-O EXEC, which cannot carry a reference
 * resolved from the loader's global scope — would REFUSE the program outright.
 *
 * A corpus example cannot build the shared library and its rival consumer
 * together, so the divergence itself is witnessed by execution outside this
 * harness (`.temp/p62-pa-scratch/addr-run-witness*.sh`: the DSS `.so`'s `&w`,
 * `&st`, `&wd`, `&sd` all compare EQUAL to the consumer executable's, against a
 * gcc-built control answering identically in the same run, on x86_64 and on
 * arm64 under qemu, debug and `--config=release`). What THIS file pins is that
 * the address routing does not leak into the flavour that must not have it, and
 * that the identity it protects HOLDS here: the program builds, links, loads and
 * returns its own answer on all four legs.
 *
 * WHY THE SHAPE. Every address is taken through a helper selected by a RUNTIME
 * value (`argc - 1`, zero with no arguments), so no pass sees a constant index
 * and folds an address-take away before the lowering decision. All four cells of
 * that decision appear TWICE — once as a FUNCTION and once as a DATA object —
 * because the decision is about a DEFINITION the loader can see, not about
 * functions: weak/default, strong-global/default, `static`, hidden-visibility.
 * In a `.so` the first two of each pair would be slot-read while the last two
 * stayed direct.
 *
 * Exit arithmetic, with no arguments (`argc - 1 == 0`):
 *   pick_fn(0)    = &weak_answer, weak_answer(40) = 40 + strong_bias() = 42
 *   pick_datum(0) = &weak_datum  = 3
 *   identity      = 1  (both addresses equal the ones in the static tables)
 *   42 + 3 + 1 = 46
 */

/* (1) WEAK / DEFAULT — replaceable under BOTH loaders. */
__attribute__((weak)) int weak_answer(int v);

/* (2) STRONG GLOBAL / DEFAULT — replaceable in an ELF shared object (both
 * references GOT-load its address there), NOT in a Mach-O dylib. The two
 * ecosystems disagree, which is why the set is declared per format. */
int strong_bias(void) { return 2; }

/* (3) `static` — module-private, in no image's dynamic export set. Its address
 * is never routed anywhere, under any format. */
static int private_double(int v) { return v * 2; }

/* (4) hidden visibility — externally defined but not exported, the source's own
 * opt-out from interposition. Never routed either. */
__attribute__((visibility("hidden"))) int unexported_triple(int v) {
    return v * 3;
}

__attribute__((weak)) int weak_answer(int v) { return v + strong_bias(); }

/* The same four cells as DATA definitions. */
__attribute__((weak)) int weak_datum = 3;
int strong_datum = 5;
static int private_datum = 7;
__attribute__((visibility("hidden"))) int unexported_datum = 11;

typedef int (*IntFn)(int);

/* The static tables are the IDENTITY the runtime address-takes are compared
 * against: a program whose two ways of naming one definition disagree is
 * exactly the defect, one artifact flavour over. */
static IntFn const fnTable[4] = { weak_answer, private_double,
                                  unexported_triple, weak_answer };
static int *const dataTable[4] = { &weak_datum, &strong_datum,
                                   &private_datum, &unexported_datum };

/* Ordinary inlinable helpers, so the release arm has real work to do and is not
 * byte-identical to the baseline (the manifest declares
 * `mustDifferFromBaseline`, and an arm that transformed nothing compares x == x
 * and cannot fail). */
static IntFn pick_fn(int which) {
    switch (which) {
        case 0:  return &weak_answer;
        case 1:  return &private_double;
        case 2:  return &unexported_triple;
        default: return &weak_answer;
    }
}

static int *pick_datum(int which) {
    switch (which) {
        case 0:  return &weak_datum;
        case 1:  return &strong_datum;
        case 2:  return &private_datum;
        default: return &unexported_datum;
    }
}

int main(int argc, char **argv) {
    (void)argv;
    int which = argc - 1;
    if (which < 0 || which > 3) which = 0;

    IntFn const f = pick_fn(which);
    int *const  d = pick_datum(which);

    int const identity = (f == fnTable[which]) && (d == dataTable[which]);
    return f(40) + *d + identity;
}
