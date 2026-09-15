/* D-FFI-RESOLVE-LIBRARY-DOES-NOT-CHECK-THE-LIBRARY-ARCH -- the LIBRARY half.
 * The runner builds THIS into a DYNAMIC library for the arm's own target (a
 * `.dll` on pe64, a `.so` on both elf64 targets, a `.dylib` on macho64), then
 * builds `main.c` resolving `dss_dyn_answer` against it via
 * `--resolve-library` -> the exec records a runtime import -> exit 42.
 *
 * ★ WHY A DYNAMIC LIBRARY, WHEN `staticlib_resolve_roundtrip` ALREADY COVERS
 * `dependsOn` + `--resolve-library`. The two flags take DIFFERENT PATHS
 * through the driver, and the difference is the whole point of this entry: an
 * `ar` archive is partitioned OUT of `resolveLibraries` and merged at link,
 * while a dynamic library stays on the list `compile_pipeline`'s step 2.5-pre
 * walks -- the EAGER, unconditional probe where the format+architecture
 * boundary check runs. Before this example the corpus exercised that probe's
 * happy path for NO target at all: a `.dll` / `.so` / `.dylib` never appeared
 * as a `dependsOn` artifact anywhere in `examples/`.
 *
 * ⚠ AND WHY THE POSITIVE HALF IS THE HALF THAT NEEDS COVERING. A guard is not
 * only wrong when it fails to fire; it is wrong when it fires on a MATCHED
 * pairing, or reads its number out of the wrong offset. A refusal-shaped
 * example would need `expectDiagnostics`, which the runners cannot combine
 * with `dependsOn` (see the manifest's $comment) -- but a false refusal, or a
 * mis-located field, reds RIGHT HERE, on every target spec, with nothing
 * synthetic about the input. The PE arm in particular is the only end-to-end
 * exercise anywhere of the `e_lfanew` SEEK: PE keeps its COFF header at a file
 * offset the DOS stub names, so locating `Machine` needs a read the other two
 * formats do not.
 *
 * WHY THIS BODY IS NOT `return 42;`, the same reason as its staticlib sibling:
 * `expected.json` declares a `release` arm, and each `dependsOn` entry carries
 * `mustDifferFromBaseline: true`, which reds unless the optimized LIBRARY
 * differs byte-wise from the baseline one. The baseline pipeline is a bare
 * `Identity` (a no-op), so a single `return 42;` would emit the same bytes at
 * both configurations and the arm would assert nothing. `dss_dyn_answer`
 * therefore carries an inlinable file-local helper, a loop-invariant addend
 * and locals for Mem2Reg -- while still returning exactly 42, because the
 * example's exit-code contract must not move.
 * ✔MEASURED (shipped CLI, Windows host, this source): baseline vs release
 * DIFFER byte-wise for all four dynamic libraries, and are byte-IDENTICAL for
 * the four executables -- which is why `mustDifferFromBaseline` is declared on
 * the DEPENDENCIES and deliberately NOT at the arm level. */
static int dss_dyn_step(int acc, int addend) {
    return acc + addend;
}

int dss_dyn_answer(void) {
    int acc    = 0;
    int base   = 5;
    int addend = 0;
    int k      = 6;
    while (k) {
        addend = base + 2;                  /* loop-invariant: 5 + 2 == 7 */
        acc    = dss_dyn_step(acc, addend); /* inlinable under `release`   */
        k      = k - 1;
    }
    return acc;                             /* 6 iterations x 7 == 42 */
}
