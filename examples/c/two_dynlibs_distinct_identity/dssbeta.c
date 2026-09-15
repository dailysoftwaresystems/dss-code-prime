/* [[D-LK-MACHO-DYLIB-INSTALL-NAME-IS-ONE-CONSTANT-FOR-EVERY-ARTIFACT]] — the
 * SECOND library, and its only job is to be a second one.
 *
 * Nothing anywhere in the shipped corpus built TWO dynamic libraries into one
 * program before this entry: `dynlib_resolve_roundtrip` carries exactly one
 * dependency per arm. One library cannot notice a defect whose whole shape is
 * "two artifacts of a format claim ONE identity" — which is exactly what the
 * two darwin dylib documents used to guarantee, by declaring the literal
 * install name `@rpath/libdss.dylib` for every dylib DSS would ever emit.
 * ✔MEASURED before the fix: both libraries built rc 0 with ZERO diagnostics and
 * the executable recorded ONE `LC_LOAD_DYLIB`, so the second library's symbols
 * were simply absent at load, while the ELF control recorded two correct
 * `DT_NEEDED` entries.
 *
 * The body carries an inlinable helper and a loop-invariant addend for the same
 * reason as its sibling: the manifest declares `mustDifferFromBaseline` on each
 * dependency, and a bare `return 22;` would emit identical bytes at both
 * configurations and assert nothing. */

static int beta_step(int acc, int addend) {
    return acc + addend;
}

int beta_answer(void) {
    int acc    = 0;
    int base   = 9;
    int addend = 0;
    int k      = 2;
    while (k) {
        addend = base + 2;            /* loop-invariant: 9 + 2 == 11 */
        acc    = beta_step(acc, addend);
        k      = k - 1;
    }
    return acc;                       /* 2 iterations x 11 == 22 */
}
