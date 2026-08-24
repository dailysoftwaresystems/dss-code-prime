// D-CSUBSET-GNU-DUNDER-QUALIFIER-SPELLINGS: the GNU `__`-wrapped spellings of
// four qualifiers/specifiers plus `_Complex`. Ten keyword-table rows onto FIVE
// EXISTING token kinds — `__volatile__`/`__volatile` → VolatileKeyword,
// `__const__`/`__const` → ConstKeyword, `__signed__`/`__signed` →
// SignedKeyword, `__restrict__`/`__restrict` → RestrictKeyword,
// `__complex__`/`__complex` → ComplexKeyword. No new kind, no new grammar, no
// C++: the same many-words-one-kind facility as `inline`/`__inline`/`__inline__`
// and `__typeof__`/`__alignof__`.
//
// WHY THE ALIASES ARE THE SPELLINGS THAT MATTER: `restrict` is C99-only and
// `_Complex` is C99-only, while the `__`-prefixed forms are available at EVERY
// -std level — so a header that must still compile as C89 writes the GNU form,
// which is why real system headers use them. MEASURED on this host with matched
// positive controls (a plain-ISO file) and a NEGATIVE control
// (`__no_such_keyword__`, refused — so the lexer is NOT blanket-accepting dunder
// words): gcc 13.3.0, clang 18.1.3 and clang 19.1.1 accept all ten, and
// `gcc -pedantic-errors` accepts them too, because a dunder spelling lives in C
// 7.1.3's implementation-reserved namespace and cannot collide with a conforming
// program's identifiers. That is the reason these needed NO dialect flag.
// (NOT claimed: real `cl.exe` refuses the dunder qualifier forms.)
//
// ★★ EVERY ONE OF THE TEN SPELLINGS IS LOAD-BEARING HERE, so deleting ANY ONE
// keyword row from c.lang.json makes this example fail to COMPILE:
//   __volatile__  g_vol1  + the `__asm__ __volatile__ ("")` barrier
//   __volatile    g_vol2
//   __const__     kc (a const LOCAL initialized from a runtime value)
//   __const       add_const's parameter
//   __signed__    sc's declaration AND its cast type-name
//   __signed      sc2's declaration AND its cast type-name
//   __restrict__  sum_restrict's first parameter + arr_restrict's array suffix
//   __restrict    sum_restrict's second parameter
//   __complex__   z (leading) AND z2 (EAST/trailing specifier position)
//   __complex     zf (an F32-element complex)
//
// ★★ IT IS ALSO RED ON A *WRONG* ALIAS FOR THREE OF THE FIVE KINDS — AND THE
// OTHER TWO ARE THE REASON THE SEMANTIC PINS EXIST. This is a MEASUREMENT, not a
// claim: each row's `kind` was swapped to a plausible neighbour in a scratch
// DSS_CONFIG_ROOT copy and this example was rebuilt and RE-RUN.
//   __signed__  -> UnsignedKeyword   compiles, exits 35  ← arm 3 catches it
//   __complex__ -> ImaginaryKeyword  FAILS to compile    ← arm 5/6 catch it
//   __volatile__-> ConstKeyword      FAILS to compile    ← arm 1 catches it, via
//                                    the `__asm__ __volatile__` slot, which
//                                    names VolatileKeyword specifically
//   __const__   -> VolatileKeyword   compiles, STILL EXITS 42  ← INVISIBLE HERE
//   __restrict__-> ConstKeyword      compiles, STILL EXITS 42  ← INVISIBLE HERE
// Arm 3 is the clean value witness: `__signed__ char` is an 8-bit SIGNED type,
// so 130 stored through it WRAPS to -126 and 201 wraps to -55, while an
// UnsignedKeyword mis-key yields 130 and 201 and drops the exit to 35.
// ★ But const-ness and restrict-ness have NO portable value observable, so this
// example CANNOT see those two mis-keys — a silently un-const object computes
// the same numbers. Their semantic effects are pinned instead in
// tests/analysis/semantic/test_semantic_analyzer_c.cpp (the `isConst`
// flag + S_ConstViolation on assignment; and for restrict, the fact that it is
// REFUSED in declaration-head position exactly as ISO `restrict` is, which is
// what separates it from Const/Volatile/Atomic/Signed/Complex — all of which ARE
// accepted there). Those pins were exercised the same way and DID go red on
// these two mis-keys. Neither surface alone is proof of the whole set; that is
// why both exist, and stating which surface covers which kind is the point.
//
// ANTI-FOLD: every value below descends from `argc`, a RUNTIME argument (1 when
// run with no args), so nothing here is a constant the front-end can fold away
// before the program runs — in the `release` pipeline either. Six independent
// arms worth 7 each; a failing arm drops the exit BELOW 42, so a wrong value is
// as red as a crash.
//
// DATA-MODEL INDEPENDENT: only `int` (32-bit), `signed char` (8-bit), `float`
// and `double` appear — no `long`, whose width differs LP64 vs LLP64 — so the
// one exit code holds on all four targets and on the release arm.
//
// exit = 6 arms x 7 = 42.

__volatile__ int g_vol1;   // GNU volatile, long spelling
__volatile int g_vol2;     // GNU volatile, short spelling

// A `__const` PARAMETER: a different grammar position than a declaration head,
// and the returned value feeds the exit code.
static int add_const(__const int v) { return v + 1; }

// `__restrict__` and `__restrict` in ONE signature (both spellings must be
// admitted by the same `ptrQualifier` slot). The stores through `a` are real, so
// the caller observes them — restrict promises the two never alias, which holds:
// the caller passes two distinct objects.
static int sum_restrict(int * __restrict__ a, int * __restrict b) {
    *a += 1;
    return *a + *b;
}

// `__restrict__` in the ARRAY-SUFFIX modifier slot (C99 6.7.6.3 array-parameter
// qualifier) — a third grammar position for the same kind.
static int arr_restrict(int a[__restrict__ 4]) { return a[0] + a[3]; }

int main(int argc, char **argv) {
    (void)argv;
    int acc = 0;

    // ── ARM 1: __volatile__ / __volatile ─────────────────────────────────────
    // Volatile globals: every read is a real load. The barrier between the
    // stores and the reads is `__asm__ __volatile__ ("")` — the GNU qualifier
    // spelling in `asmStmt`'s optional-qualifier slot, which is a DIFFERENT slot
    // than a declaration qualifier and closes the `__volatile__` half of
    // D-CSUBSET-INLINE-ASM-SPELLING. (Bare `asm` stays absent, and the `:`
    // operand list is still an open gap — see that anchor.)
    g_vol1 = 20 + argc;                     // 21
    g_vol2 = g_vol1 * 2;                    // 42
    __asm__ __volatile__ ("");              // empty-template optimizer barrier
    if (g_vol1 + g_vol2 == 63) acc += 7;    // 21 + 42

    // ── ARM 2: __const__ / __const ───────────────────────────────────────────
    // A const LOCAL initialized from a RUNTIME value (so it is not a folded
    // literal), read back, and passed through a `__const` parameter.
    __const__ int kc = 6 + argc;            // 7
    if (kc + add_const(kc) == 15) acc += 7; // 7 + 8

    // ── ARM 3: __signed__ / __signed ─── THE WRONG-ALIAS DETECTOR ────────────
    // 8-bit SIGNED wrap. An UnsignedKeyword mis-key gives 130 and 201 here and
    // this arm goes red; a dropped specifier gives 130 and 201 as ints and it
    // goes red too. Both the DECLARATION and the CAST type-name use the alias.
    __signed__ char sc = (__signed__ char)(120 + argc * 10);   // 130 -> -126
    __signed char sc2 = (__signed char)(200 + argc);           // 201 -> -55
    if ((int)sc == -126 && (int)sc2 == -55) acc += 7;

    // ── ARM 4: __restrict__ / __restrict ─────────────────────────────────────
    // Two distinct objects (so the no-alias promise holds), a store the caller
    // observes, and the array-parameter qualifier slot.
    int ra = 10 + argc;                     // 11
    int rb = 20 + argc;                     // 21
    int rsum = sum_restrict(&ra, &rb);      // ra -> 12, returns 12 + 21 = 33
    int buf[4];
    buf[0] = argc;                          // 1
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 8 + argc;                      // 9
    int rarr = arr_restrict(buf);           // 1 + 9 = 10
    if (rsum == 33 && rarr == 10 && ra == 12) acc += 7;

    // ── ARM 5: __complex__ (leading AND east) ────────────────────────────────
    // Runtime-constructed complex, then a complex MULTIPLY through an
    // east-position `double __complex__` declaration. A wrong multiset row
    // changes the element type and these accessors stop agreeing.
    __complex__ double z = __builtin_complex((double)(2 + argc),
                                             (double)(3 + argc));   // (3, 4)
    double __complex__ z2 = z * z;                                  // (-7, 24)
    if ((int)__builtin_creal(z) == 3 && (int)__builtin_cimag(z) == 4 &&
        (int)__builtin_creal(z2) == -7 && (int)__builtin_cimag(z2) == 24)
        acc += 7;

    // ── ARM 6: __complex (F32 element) ───────────────────────────────────────
    // The short spelling over `float`: the F64-complex builtin result NARROWS
    // into the F32-element object and the accessors WIDEN back, so the element
    // type is genuinely exercised rather than assumed.
    __complex float zf = __builtin_complex((double)(1 + argc),
                                           (double)(1 + argc * 2));  // (2, 3)
    if ((int)__builtin_creal(zf) == 2 && (int)__builtin_cimag(zf) == 3)
        acc += 7;

    return acc;   // 42 iff all six arms pass
}
