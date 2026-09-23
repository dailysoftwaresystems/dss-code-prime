/* D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH -- the LIBRARY half.
 * The runner builds THIS into a DYNAMIC library for the arm's own target (a
 * `.so` on both elf64 targets, a `.dylib` on both macho64 targets, a `.dll` on
 * pe64) and puts it in the SAME directory as the executable; `main.c` then
 * resolves `dss_runpath_answer` against it and records a runtime import.
 *
 * What finds this library at RUN time is the point of the example: the
 * executable records `runpaths: ["${ORIGIN}"]` -- DT_RUNPATH `$ORIGIN` on ELF,
 * LC_RPATH `@loader_path` on Mach-O -- so the loader looks in the executable's
 * own directory with NO loader variable set. The control beside this example
 * (`dynlib_runpath_absent_exits_127`) is the same program without the request.
 *
 * WHY THIS BODY IS NOT `return 42;`: `expected.json` declares a `release` arm
 * and each `dependsOn` entry carries `mustDifferFromBaseline: true`, which reds
 * unless the optimized LIBRARY differs byte-wise from the baseline one. The
 * baseline pipeline is a bare `Identity`, so a single `return 42;` would emit
 * the same bytes at both configurations and the arm would assert nothing. So
 * `dss_runpath_answer` carries an inlinable file-local helper, a
 * loop-invariant addend and locals for Mem2Reg -- while still returning
 * exactly 42, because the example's exit-code contract must not move. */
static int dss_runpath_step(int acc, int addend) {
    return acc + addend;
}

int dss_runpath_answer(void) {
    int acc    = 0;
    int base   = 4;
    int addend = 0;
    int k      = 7;
    while (k) {
        addend = base + 2;                      /* loop-invariant: 4 + 2 == 6 */
        acc    = dss_runpath_step(acc, addend); /* inlinable under `release`  */
        k      = k - 1;
    }
    return acc;                                 /* 7 iterations x 6 == 42 */
}
