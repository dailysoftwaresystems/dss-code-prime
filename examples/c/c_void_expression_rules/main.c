// D-CSUBSET-VOID-TERNARY-LOWERS-A-PHI-OF-VOID (+ the two refusals that bound it).
//
// C 6.5.15p3: "if both the second and third operands have void type, the result
// has void type." A conditional whose arms are both `void` is ordinary C — the
// idiomatic branch-between-two-side-effects — and gcc 13.3.0 (`-std=c2x`) and
// clang 18.1.3 (`-std=c23`), probed SEPARATELY, compile and RUN every shape here.
//
// ✔MEASURED at 301e2a63 through the shipped CLI (x86_64:pe64-x86_64-windows-exec):
// ALL FIVE shapes below produced the SAME `error[L_UnsupportedLoweringForOpcode]:
// MIR value %N used …` — an INTERNAL message two tiers below the source. One
// error text for five unrelated-looking syntaxes is what identified the cause as a
// single lowering step rather than five gaps: the ternary's value path ends in a
// join `Phi`, and `Phi` is a value-producing opcode whose result type must be
// valid, so a VOID result asked for a phi of a thing that has no value.
//
// ★ WHY THE ARMS MUST BE SIDE-EFFECTING AND COUNTED, not just compiled. The
// failure a naive fix opens is not a refusal — it is running the WRONG arm, or
// running BOTH, or running NEITHER while still compiling. `t` is `volatile`, each
// arm adds a distinct amount, and the expected total is exact, so a diamond that
// branches wrongly cannot reach 42. The `release` arm re-checks that the optimizer
// does not collapse a value-less diamond into nothing.
//
// exit 42 = every arm correct. Any other exit IS the failing arm's id.

static volatile int t = 0;

static void add1(void) { t += 1; }
static void add2(void) { t += 2; }
static void add4(void) { t += 4; }

int main(int argc, char **argv) {
    (void)argv;
    // argc is set by the OS, so `sel` is never foldable and every conditional
    // below is a genuine runtime branch.
    int const sel = (argc > 0) ? 1 : 0;   // 1 in every real invocation

    // (1) THE BARE STATEMENT — `c ? f() : g();`, the shape the row is named for.
    t = 0;
    sel ? add1() : add2();
    if (t != 1) return 1;

    // (2) THE OTHER BRANCH. Without this, arm (1) is satisfied by a lowering that
    // always takes the `then` side — which is exactly what a broken diamond does.
    t = 0;
    (!sel) ? add1() : add2();
    if (t != 2) return 2;

    // (3) CAST TO VOID around the whole conditional. A distinct SOURCE shape that
    // reaches the same lowering: `(void)X` mints no Cast node at all
    // (D-CSUBSET-CAST-VOID-DISCARD), so this must not regress independently.
    t = 0;
    (void)(sel ? add4() : add2());
    if (t != 4) return 3;

    // (4) AS THE LEFT OPERAND OF A COMMA — the conditional is a side-effect
    // statement of a SeqExpr, and the comma's VALUE comes from the right operand.
    // Both halves are asserted: a lowering that dropped the left arm would still
    // produce 9 here.
    t = 0;
    int const r = ((sel ? add1() : add2()), 9);
    if (r != 9) return 4;
    if (t != 1) return 5;

    // (5) INSIDE A LOOP, alternating arms — three iterations, both arms taken, so
    // a diamond that mis-wires its join or leaves the loop body unreachable
    // produces a different total. 2 + 1 + 2 = 5.
    t = 0;
    for (int i = 0; i < 3; i++) {
        (i & 1) ? add1() : add2();
    }
    if (t != 5) return 6;

    // (6) ONE ARM A CALL, THE OTHER A `(void)0` — the arms need not be symmetric,
    // and the void-typed constant arm must contribute no value either.
    t = 0;
    sel ? add1() : (void)0;
    if (t != 1) return 7;
    t = 0;
    (!sel) ? add1() : (void)0;
    if (t != 0) return 8;

    // (7) NESTED: a void conditional inside the arm of another. The lowering
    // recurses through the same discard funnel, so this is the shape that proves
    // the fix is not depth-1 only.
    t = 0;
    sel ? (((argc & 1) ? add1() : add2())) : add4();
    if (t != 1) return 9;

    // (8) And the ordinary VALUE-producing ternary is untouched — the change must
    // not have moved the scalar diamond, which still joins with a real phi.
    int const v = sel ? 40 : 7;
    if (v != 40) return 10;
    int const w = (!sel) ? 40 : 2;
    if (w != 2) return 11;

    return 42;
}
