/* [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]], the ADDRESS half -- THE
 * CLIENT. `self_consistent` is defined in `dsssubject.c`, which the runner has
 * already built into a DYNAMIC library for this arm's target (the target's
 * `dependsOn`); the driver's `--resolve-library` records the runtime import and
 * this exec calls it.
 *
 * KEEP THIS FILE THIN, for the reason `dynlib_resolve_roundtrip`'s client
 * states: with no locals, no arithmetic and no inlinable callee, the shipped
 * release pipeline has nothing HERE to transform, so the whole
 * baseline-vs-release difference belongs to the PREREQUISITE LIBRARY -- which
 * is why `mustDifferFromBaseline` is declared per dependency rather than on the
 * arm.
 *
 * The check itself runs INSIDE the library on purpose: the identity the row
 * protects is that ONE image gives ONE identifier ONE address across every use
 * form, and that is a property of the library, not of its caller. */
extern int self_consistent(void);

int main(void) {
    return self_consistent();
}
