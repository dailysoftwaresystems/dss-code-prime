// D-CSUBSET-VOID-POINTER-ARITHMETIC-REFUSED: GNU C's "Arithmetic on void- and
// Function-Pointers", end to end and at run time.
//
// gcc's manual states the rule ONCE — "the size of a void or of a function" is 1 —
// and everything below follows from it: `sizeof(void)`, `sizeof(*p)` on a `void *`,
// `_Alignof(void)` and `sizeof(f)` all fold to 1, and pointer arithmetic on a
// `void *` therefore steps ONE BYTE at a time. ISO C forbids all of it (6.5.3.4p1
// forbids `sizeof` of a function or an incomplete type — and `void` is permanently
// incomplete, 6.2.5p19; 6.5.6p2 admits `p ± n` only for a complete object type), so
// under `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` this file exists because the UNION
// decides and a unanimous ACCEPTANCE settles it.
//
// ✔MEASURED at 301e2a63 through the shipped CLI (x86_64:pe64-x86_64-windows-exec):
// EVERY arithmetic line below was `error[H0009] array/pointer index element type
// has no computable size`, the `++`/`--` lines were `error[H0009] ++/-- on a
// pointer whose pointee has no size … C 6.5.6 forbids pointer arithmetic on it`,
// and each `sizeof`/`_Alignof` was `error[H0009] sizeof of an incomplete or
// un-sizeable type` — while gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`),
// probed SEPARATELY, both COMPILE AND RUN all of them.
//
// ★ WHY THIS FILE ASSERTS BYTES AND NOT MERE ACCEPTANCE. The failure mode a
// widened stride opens is not a refusal — it is a SILENT WRONG OFFSET. A stride of
// 4 (inheriting a neighbour's size), or 0 (a synthesized empty layout), or an
// unscaled index would all still COMPILE. So every arm reads back a byte whose
// value is unique in the buffer and returns the arm's own id on mismatch: this file
// can only exit 42 if every offset is exactly right.
//
// ⚠ THE BASE IS OPAQUE ON PURPOSE. `gate` is `volatile`, so `buf + (gate - 1)` is
// `buf` at run time but is NOT foldable — the `release` arm therefore witnesses
// real GEPs and a real runtime `void *` difference rather than constants the
// optimizer computed at compile time.
//
// ⚠ AND THE COMPILE-TIME HALF IS DELIBERATELY SPLIT ACROSS TWO TIERS. The three
// `_Static_assert`s and the `dim[]` bound are folded by the SEMANTIC const-evaluator
// (they are constant-expression contexts and never reach lowering); the `sizeof` /
// `_Alignof` expressions inside `main` are folded by HIR→MIR. Both tiers ask the
// same `operandLayout` query, and this file is what proves they agree — a fix
// applied to only one of them cannot reach 42.
//
// exit 42 = every arm correct. Any other exit IS the failing arm's id.

// The semantic const-fold tier: a constant-expression context, refused before P42
// with `error[S0029] static assertion condition is not an integer constant
// expression`. gcc and clang both accept all three.
_Static_assert(sizeof(void) == 1, "GNU C: sizeof(void) is 1");
_Static_assert(_Alignof(void) == 1, "GNU C: _Alignof(void) is 1");

int probe_fn(int x) { return x + 1; }

// A function DESIGNATOR is the same rule, not a second one. (`_Alignof(probe_fn)`
// is deliberately NOT asserted: gcc says 1, clang accepts the form but does NOT say
// 1 — the references disagree on the VALUE while both accept, so the union pins
// acceptance here and nothing more.)
_Static_assert(sizeof(probe_fn) == 1, "GNU C: sizeof(a function) is 1");

static volatile int gate = 1;   // opaque: defeats constant folding of the base

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Distinct, non-repeating byte values, so reading the WRONG offset can never
    // coincidentally match the right one.
    char buf[16];
    for (int i = 0; i < 16; i++) buf[i] = (char)(i * 3 + 1);   // 1,4,7,10,13,...

    void *const base = buf + (gate - 1);        // == buf, not foldable
    void *p;

    // (1) THE ROWED SHAPE: `p = p + 1` on a `void *`. One byte, not four.
    p = base;
    p = p + 1;
    if (*(char *)p != buf[1]) return 1;

    // (2) The compound form. A separate lowering path from (1).
    p = base;
    p += 2;
    if (*(char *)p != buf[2]) return 2;

    // (3) POST-INCREMENT. Before P42 this had its OWN diagnostic, from an early
    // kind guard in the HIR tier that duplicated the stride decision — which is
    // why it is pinned separately from (1) rather than assumed to follow.
    p = base;
    p++;
    if (*(char *)p != buf[1]) return 3;

    // (4) PRE-increment, three times: the step must be 1 EACH time, not once.
    p = base;
    ++p;
    ++p;
    ++p;
    if (*(char *)p != buf[3]) return 4;

    // (5) PRE-decrement, walking back from a known-good position.
    p = base;
    p += 5;
    --p;
    if (*(char *)p != buf[4]) return 5;

    // (6) Compound subtract. Forward 7, back 3 — a NON-symmetric pair, so a
    // stride applied to only one direction is visible.
    p = base;
    p += 7;
    p -= 3;
    if (*(char *)p != buf[4]) return 6;

    // (7) THE DIFFERENCE OF TWO `void *`s, which is the OTHER half of the rule:
    // `q - p` is an element COUNT, and with stride 1 the count IS the byte
    // difference. Both operands are opaque, so this is a runtime subtract.
    {
        void *q = base;
        q = q + 9;
        p = base;
        if (q - p != 9) return 7;
    }

    // (8) A `const void *`. The qualifier is a transparent skin and must not
    // change the stride — a tier that stripped it in one place and not another
    // would read the wrong byte here and nowhere else.
    {
        const void *cp = base;
        cp = cp + 6;
        if (*(const char *)cp != buf[6]) return 8;
    }

    // (9) The HIR→MIR `SizeOf` fold, on `void` and on a dereferenced `void *`.
    // The static asserts above prove the SEMANTIC tier; these prove the LOWERING
    // tier, and the two are different code paths asking the same query.
    if (sizeof(void) != 1) return 9;
    {
        void *vp = base;
        if (sizeof(*vp) != 1) return 10;
    }

    // (10) The HIR→MIR `AlignOf` fold. Same rule, no separate alignment logic.
    if (_Alignof(void) != 1) return 11;

    // (11) The function designator at the lowering tier.
    if (sizeof(probe_fn) != 1) return 12;

    // (12) `sizeof(void)` INSIDE AN ARRAY DIMENSION — the Pass-1.5 const-fold, a
    // third distinct site, and one that fails LOUD (S_NonConstantArrayLength)
    // rather than quietly if the fold declines.
    {
        char dim[sizeof(void) * 5];
        for (int i = 0; i < (int)sizeof(dim); i++) dim[i] = (char)(i + 20);
        if (sizeof(dim) != 5) return 13;
        if (dim[4] != 24) return 14;
    }

    // (13) And the function still WORKS — giving a function type a size must not
    // disturb calling one.
    if (probe_fn(41) != 42) return 15;

    return 42;
}
