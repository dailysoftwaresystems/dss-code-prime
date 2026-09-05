/* D-LK3-DYLIB-WEAK-EXPORT runtime witness.
 *
 * A Mach-O IMAGE that carries WEAK definitions of both kinds — a weak
 * FUNCTION and a weak DATA global — and still loads, passes its ad-hoc
 * signature and exits with the asserted code.
 *
 * WHAT USED TO HAPPEN. Both image arms wrote a hard-coded `n_desc = 0` into
 * every nlist record, built no export trie on the executable arm, and copied
 * the schema's header flags verbatim — so a weak definition reaching a Mach-O
 * image went out SILENTLY STRONG: `nm -m` read `external`, the coalescing
 * surface dyld reads (the export trie) was empty, and the mach header did not
 * advertise MH_WEAK_DEFINES. A dylib refused a weak export outright rather
 * than publish half the machinery.
 *
 * WHAT HAPPENS NOW, and it is MEASURED on the operator's Apple Silicon host
 * (macOS 26.6.2, Apple clang 21.0.0 / ld-1267, 2026-09-05) against the
 * IDENTICAL source built by Apple's toolchain: header flags `0x00218085`
 * against a weak-free control's `0x00200085` (the delta is exactly
 * MH_WEAK_DEFINES | MH_BINDS_TO_WEAK), `weak external` in `nm -m`, and
 * `_weak_helper [weak-def]` in `dyld_info -exports`. DSS emits the same three.
 * The structural pins live in tests/link/test_macho_dylib_writer.cpp; THIS
 * example is the RUN witness — and the runtime semantics themselves (two
 * images defining one weak symbol collapsing to ONE) were witnessed
 * separately on that host, because they need a second image the corpus
 * harness cannot build.
 *
 * WHY THE SHAPE. The two weak functions are reached through a const
 * function-pointer table indexed by a RUNTIME value (`argc - 1`, zero with no
 * arguments), and the weak global is READ rather than folded, so neither the
 * baseline nor the release pipeline can see a constant index or a constant
 * initializer and drop the definitions before they reach the image writer.
 * That is the INTENT of the shape, not a measurement; what this example
 * asserts is the exit code on both arms.
 *
 * Exit arithmetic: table[0](40) = weak_helper(40) = 40 + weak_bias(2) = 42.
 */

/* A weak DATA definition. It reaches the export trie through the writer's
 * data-symbol classification arm, which is a different branch from the
 * function arm below — both take the weak-definition flag. */
__attribute__((weak)) int weak_bias = 2;

/* A weak FUNCTION definition, the shape `examples/c/attributes_syntax`
 * already carried; kept here so one example covers both kinds at once. */
__attribute__((weak)) int weak_helper(int v) { return v + weak_bias; }

/* A SECOND weak function, so the table has a real choice and the index
 * cannot be reasoned away. */
__attribute__((weak)) int weak_double(int v) { return v * 2; }

typedef int (*IntFn)(int);

static IntFn const dispatch[2] = { weak_helper, weak_double };

/* An ORDINARY (non-weak) inlinable wrapper, so the release pipeline has real
 * work to do and its arm is not byte-identical to the baseline — the manifest
 * declares `mustDifferFromBaseline`, and an arm that transformed nothing
 * compares x == x and cannot fail.
 *
 * The image also carries `main`, an ordinary EXTERNAL definition, beside the
 * three weak ones — so a writer that marked everything weak would be as wrong
 * as one that marked nothing, and the darwin witness reads that directly:
 * `nm -m` shows `weak external _weak_helper` and `weak external _weak_double`
 * while `_main` stays plain `external`. */
static int route(int v, int which) { return dispatch[which & 1](v); }

int main(int argc, char **argv) {
    (void)argv;
    return route(40, argc - 1);
}
