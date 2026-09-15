/* [[D-LK-MACHO-DYLIB-INSTALL-NAME-IS-ONE-CONSTANT-FOR-EVERY-ARTIFACT]] +
 * [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]] — the CLIENT of two
 * DSS-built dynamic libraries.
 *
 * The runner builds `dssalpha.c` and `dssbeta.c` into TWO dynamic libraries
 * for this arm's target, then builds this file with BOTH on
 * `--resolve-library`. Each import's recorded runtime identity comes from the
 * library's OWN embedded identity (ELF `DT_SONAME` / Mach-O `LC_ID_DYLIB` / PE
 * export `DllName`) — nothing here states one — so two libraries that claimed
 * the same identity would collapse into one recorded dependency.
 *
 * KEPT THIN on purpose, the same discipline `dynlib_resolve_roundtrip` states:
 * no locals, no arithmetic, no inlinable callee here, so the shipped `release`
 * pipeline has nothing in THIS file to transform and the whole
 * baseline-vs-release difference belongs to the two prerequisite LIBRARIES —
 * which is why `mustDifferFromBaseline` is declared per dependency rather than
 * on the arm. */

extern int alpha_answer(void);
extern int beta_answer(void);

int main(void) {
    return alpha_answer() + beta_answer();   /* 20 + 22 == 42 */
}
