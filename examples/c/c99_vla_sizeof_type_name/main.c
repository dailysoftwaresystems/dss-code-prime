// [[D-CSUBSET-VLA-SIZEOF-TYPEFORM]] part (1) — `sizeof ( int[n] )`, the VLA
// TYPE-NAME form, and the ONE `sizeof` whose operand C evaluates.
//
// C 6.5.3.4p2: *"If the type of the operand is a variable length array type, the
// operand is evaluated."* So this `sizeof` is a RUNTIME expression: its bound is
// RE-READ at every occurrence, its side effects HAPPEN, and its value is not a
// constant expression (C 6.6).
//
// ✔MEASURED 2026-09-04, each reference invoked SEPARATELY. gcc 13.3.0 and clang
// 18.1.3 both compile and RUN every construct below, at `-std=c17` AND
// `-std=c2x`, and both agree on every number. MSVC 19.44.35228 ABSTAINS —
// `error C2057: expected constant expression` on the bound plus
// `warning C4034: sizeof returns 0` — which is an abstention, not a vote
// against. Two references accept AND run, so the disjunction requires it.
// At this cycle's base (b1f31420) DSS refused the whole family:
// `error[H_UnsupportedLoweringForKind]: sizeof of an incomplete or un-sizeable
// type`.
//
// ★★ THE PROPERTY THIS WITNESS EXISTS TO PIN IS **FRESH EVALUATION**, NOT THE
// NUMBER. A constant-folded implementation passes any single-value check. So `n`
// CHANGES between the two `sizeof(int[n])` below and the two answers must
// DIFFER — that is the witness gcc and clang both produce (12 then 20) and the
// one an implementation that sized the type once cannot.
//
// ★ AND THE OBJECT FORM IS THE OTHER HALF OF THE SAME DISCRIMINATION, IN THE
// OPPOSITE DIRECTION. `sizeof a` for a VLA OBJECT is the size FROZEN at a's
// declaration (C 6.7.6.2p2) and must NOT re-read `n`; `sizeof(int[n])` is the
// TYPE question and must. Both live in this one program, with `n` mutated
// between them, so an implementation that conflated the two questions fails
// whichever way it conflated them.
//
// ⚠ MAIN IS DELIBERATELY A LEAF — no calls, results carried out on the exit code.
// That is not style: [[D-CSUBSET-VLA-NONLEAF-CALL-FRAME]] is OPEN at the time of
// writing, so a function that holds a VLA object AND makes a call is refused
// (`L_VlaNonLeafFrameUnsupported`). The VLA OBJECT arm below is what needs the
// leaf; the type-name arms allocate nothing and would not. When that row closes,
// this example may be rewritten to print.
//
// EXIT CODE 42 == every arm held. Each early return names the arm that broke.

typedef int Row[3];
enum E { A = 3, B = 5 };

int main(void) {
    volatile int seed = 3;      // volatile => genuinely runtime, never folded
    int n = seed;               // n == 3

    // ── ARM 1: FRESH EVALUATION. The same source expression, twice, with `n`
    // changed in between: two DIFFERENT answers. This is the arm a folded
    // implementation fails and the reason the example exists.
    unsigned long first = sizeof(int[n]);       // 3 * 4 == 12
    n = 5;
    unsigned long second = sizeof(int[n]);      // 5 * 4 == 20 — re-evaluated
    if (first != 12) return 1;                  // wrong first size
    if (second != 20) return 2;                 // wrong second size
    if (first == second) return 3;              // FOLDED — the whole point

    // ── ARM 2: FIXED-ARRAY CONTROL. A non-VLA type-name must keep taking the
    // ordinary static fold; if this broke, arm 1 would be "working" by having
    // made every sizeof runtime.
    if (sizeof(int[4]) != 16) return 4;
    if (sizeof(int) != 4) return 5;

    // ── ARM 3: THE OPERAND'S SIDE EFFECTS HAPPEN. C 6.5.3.4p2 evaluates the
    // operand, so `n++` inside the bound really increments `n` — the sharpest
    // discriminator between an evaluated implementation and a folded one, and
    // the one place in C where a `sizeof` operand is not inert.
    unsigned long bumped = sizeof(int[n++]);    // n was 5 → 20 bytes, n becomes 6
    if (bumped != 20) return 6;
    if (n != 6) return 7;                       // the side effect did NOT happen

    // ── ARM 4: MULTIDIMENSIONAL. Every dimension is evaluated and all of them
    // multiply. A fix that handled one bound and silently mis-answered two would
    // read as a complete one, so both are here.
    int m = 2;
    if (sizeof(int[n][m]) != 48) return 8;      // 6 * 2 * 4
    if (sizeof(int[n][3]) != 72) return 9;      // a MIXED runtime/fixed pair

    // ── ARM 5: SUFFIX↔LEVEL PAIRING. `Row[n]` writes ONE array suffix over a
    // type that has TWO array levels, so its element is the whole `int[3]`, not
    // `int`. An implementation that descended to the leaf element would multiply
    // by 4 instead of 12 and under-report by a factor of 3 — a plausible wrong
    // number rather than a refusal, which is the failure direction that costs
    // most. ✔gcc and clang both give 72 here.
    if (sizeof(Row[n]) != 72) return 10;        // 6 * sizeof(int[3]) == 6*12

    // ── ARM 6: THE OBJECT FORM STAYS FROZEN. `sizeof a` is the size captured at
    // a's declaration (C 6.7.6.2p2) and must be immune to a later change of `n`,
    // even though the TYPE form two lines later is not. This arm is what makes
    // arm 1 a statement about the type-name form specifically.
    int a[n];                                   // n == 6 here → 24 bytes, frozen
    a[0] = 0;
    unsigned long frozen = sizeof a;
    n = 100;                                    // mutate n AFTER the declaration
    if (frozen != 24) return 11;
    if (sizeof a != frozen) return 12;          // the object size must not move
    if (sizeof(int[n]) != 400) return 13;       // …while the TYPE form does

    // ── ARM 7: AN ENUM-TYPED BOUND IS AN INTEGER BOUND. C 6.7.6.2p1 requires
    // the size expression to have INTEGER type, and the lowering enforces that
    // (`S_VlaSizeNotInteger` — a `double` or pointer bound is refused, as gcc and
    // clang both refuse it). An enum-typed VARIABLE is an integer, so this arm is
    // the POSITIVE control for that guard: without it the refusal could be a
    // blanket one and nobody would notice. ✔gcc and clang both give 20.
    enum E e = seed ? B : A;                    // runtime enum value == B == 5
    if (sizeof(int[e]) != 20) return 14;

    return 42;
}
