/* TF-C93 (D-CSUBSET-ATTRIBUTE-IGNORED-FOR-DECL-KIND-SILENT) — the decl-kind gate
 * REPORTS WITHOUT CHANGING BEHAVIOUR, proven END-TO-END through the real release
 * pipeline. Six misplaced attributes fire `warning[S005F]`
 * S_AttributeIgnoredForDeclarationKind, the binary still LINKS, and it still RUNS
 * correctly — exit 42.
 *
 * ★ WHY THIS EXAMPLE EXISTS AND NO UNIT TEST REPLACES IT. Every unit pin on this
 * axis asserts a DIAGNOSTIC COUNT at the semantic tier. NONE of them proves the
 * other half of the gate's contract: that a warning is NOT an error, that the
 * declarations it warns about still lower to correct bytes, and that the whole
 * thing survives the shipped optimizer. This drags the new gate through the real
 * compile → link → SPAWN → exit-code sequence on every CI leg, where a warning
 * that had accidentally become fatal, or a warned declaration whose initializer
 * got dropped along with its discarded attribute, is immediately visible.
 *
 * ★ THE FOUR AXES OF THE CLASS APPEAR IN BOTH POLARITIES, side by side, because
 * the gate's value is that it discriminates. MISPLACED (warns, effect discarded,
 * VALUE must survive): `noinline` / `always_inline` / `no_sanitize_thread` on data
 * objects, `warn_unused_result` on a data object, `noinline` on a TYPEDEF, and
 * `always_inline` on a BLOCK-SCOPE local. CORRECTLY PLACED (silent, honored):
 * all four spellings on functions, plus `warn_unused_result` on a typedef — which
 * is SILENT on purpose, the row split's clang-legal position.
 *
 * ★★ WHAT BREAKS IT — this is a LOAD-BEARING example, not a smoke test:
 *  - the gate becoming an ERROR instead of a warning ⇒ no binary at all, both arms
 *    (the runner asserts rc==0 AND errorCount()==0);
 *  - a warned DECLARATION losing its value along with its discarded attribute —
 *    a dropped initializer, a global emitted into the wrong section, a typedef
 *    that stops naming `int` — ⇒ t1..t7 flip and the exit code is no longer 42
 *    (each warned object is READ and gated against a volatile-seeded twin);
 *  - the block-scope warned local not being allocated/stored ⇒ t6 flips;
 *  - `warn_unused_result` on the TYPEDEF starting to warn ⇒ the row was re-narrowed
 *    to match `nodiscard`'s (which the shipped row forbids); the exit code stays 42,
 *    so THAT half is pinned by the unit suite — stated here so this example is not
 *    read as covering it;
 *  - a correctly-placed function attribute breaking its callee's ARITHMETIC (the
 *    `noinline` refusal or the `always_inline` threshold bypass corrupting the
 *    lowered body) ⇒ t8..t10 flip.
 *
 * ⚠ NOT claimed here: that `noinline` actually refuses inlining or that
 * `always_inline` bypasses the cost model. Those are MIR-level facts with their own
 * pins, and the debug pipeline is `Identity` (no inliner at all), so an exit code
 * cannot see them. What this example pins is that honoring them never changes the
 * COMPUTED RESULT — in the baseline arm and under the full shipped `release`
 * pipeline alike.
 *
 * Every operand is a RUNTIME value (volatile-seeded, the c23_bitint_wide_muldiv
 * device), so no const-fold / mem2reg / CSE pass in either arm can pre-evaluate a
 * term and mask a broken declaration. */

/* ── MISPLACED: each of these WARNS (S005F) and has its effect DISCARDED, while
 * its declared VALUE must still reach the program correctly. ───────────────── */
__attribute__((noinline))           int gNoinline  = 7;
__attribute__((always_inline))      int gAlwaysInl = 9;
__attribute__((no_sanitize_thread)) int gNoTsan    = 11;
int gWur __attribute__((warn_unused_result))       = 13;

/* A TYPEDEF is `DeclarationKind::Type` — out of bounds for `noinline`, so this
 * warns; the alias must nevertheless still name `int`. */
typedef __attribute__((noinline)) int TNoinline;

/* …and the CONTRAST that makes the row split observable in a runnable program:
 * clang's applicability text enumerates typedefs for the GNU `warn_unused_result`
 * spelling, so the shipped row declares `type` and this position is SILENT. */
typedef __attribute__((warn_unused_result)) int TWur;

/* ── CORRECTLY PLACED: silent, honored, and their arithmetic is load-bearing. ── */
__attribute__((noinline))           static int addNI(int a, int b) { return a + b; }
__attribute__((always_inline))      static int mulAI(int a, int b) { return a * b; }
__attribute__((no_sanitize_thread)) static int xorNT(int a, int b) { return a ^ b; }
__attribute__((warn_unused_result)) static int incWUR(int a)       { return a + 1; }

int main(void) {
    /* Runtime seeds — defeat const-fold in both arms. */
    volatile int v1 = 1, v2 = 2, v3 = 3, v7 = 7, v9 = 9, v11 = 11;
    volatile int v13 = 13, v17 = 17, v19 = 19, v21 = 21;

    /* t1..t4 — the four warned DATA OBJECTS must still carry their initializers. */
    int t1 = (gNoinline  == v7)  ? 4 : 0;
    int t2 = (gAlwaysInl == v9)  ? 4 : 0;
    int t3 = (gNoTsan    == v11) ? 4 : 0;
    int t4 = (gWur       == v13) ? 4 : 0;

    /* t5 — the warned TYPEDEF still names `int` and round-trips a runtime value. */
    TNoinline n1 = (TNoinline)v17;
    int t5 = (n1 == v17) ? 4 : 0;

    /* t6 — the warned BLOCK-SCOPE local is still allocated and still stores. */
    __attribute__((always_inline)) int loc = 0;
    loc = (int)v19;
    int t6 = (loc == v19) ? 4 : 0;

    /* t7 — the SILENT typedef position (clang-legal) also has to work. */
    TWur w1 = (TWur)v21;
    int t7 = (w1 == v21) ? 4 : 0;

    /* t8..t10 — the correctly-placed FUNCTION attributes: honored AND correct.
     * 19+2 == 21 ; 7*3 == 21 ; 9^3 == 10 ; 1+1 == 2. */
    int t8  = (addNI((int)v19, (int)v2) == 21) ? 5 : 0;
    int t9  = (mulAI((int)v7,  (int)v3) == 21) ? 5 : 0;
    int t10 = (xorNT((int)v9,  (int)v3) == 10 && incWUR((int)v1) == 2) ? 4 : 0;

    /* 4+4+4+4+4+4+4+5+5+4 == 42, exactly. */
    return t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9 + t10;
}
