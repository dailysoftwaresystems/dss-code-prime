// C 6.7p2 (D-CSUBSET-DECL-QUALIFIER-BEFORE-STORAGE-CLASS, P65): a declaration's
// specifiers are an UNORDERED SET, so a TYPE QUALIFIER may be written BEFORE,
// BETWEEN or AFTER the storage-class specifiers. Every declaration in this file
// writes the qualifier FIRST — the spelling DSS refused with
// `P_NoAlternativeMatched … got 'static'` before this cycle — and every one of
// them is accepted and RUN by gcc 13.3.0 (-std=c2x), clang 18.1.3 (-std=c23)
// and MSVC 19.51.36252, each probed separately on its own translation unit.
//
// THE EXAMPLE IS NOT A PARSE TEST. Parsing the qualifier is the easy half; the
// half that can go wrong in SILENCE is what the qualifier then MEANS, because a
// leading qualifier lands in the declaration's specifier PREFIX — the subtree
// every positional consumer STRIPS — rather than in the type head. So each arm
// below checks a RUNTIME consequence that only holds if the qualifier survived
// the move:
//
//   *  `const static` at file scope must keep INTERNAL linkage and read back 1.
//   *  `volatile static` must still be a distinct, correctly-typed object: it is
//      written and re-read through a helper so a dropped volatile cannot be
//      constant-folded away by the release pipeline arm.
//   *  `const static` at BLOCK scope must keep STATIC STORAGE DURATION — the
//      counter is incremented on two separate calls and must reach 2, which an
//      automatic would never do. (A dropped `static` here is the exact silent
//      miscompile the block-scope half of the fix exists to prevent: the
//      qualifier lead MOVED from `kwDeclHead` to `localDeclSpecifiers`, so the
//      storage class and the qualifier now travel in the same node.)
//   *  `const typedef` must still declare an ALIAS.
//   *  a qualifier written BETWEEN two specifiers (`static const inline`) must
//      still produce a callable function.
//
// EXIT 42 on success; every arm contributes and a single wrong answer changes
// the exit code. Data-model independent — all values are small ints.
//
// RED-ON-DISABLE (REMOVE direction): delete "headQualifier" from the trailing
// {repeat} of `declSpecifiers` and from both alternatives of
// `localDeclSpecifiers` in src/dss-config/sources/c.lang.json. Every
// declaration below becomes a loud parse error and the example never links.

const static int g_qualFirst = 1;             // qualifier BEFORE storage class
static const int g_classFirst = 1;            // the order that always worked
volatile static int g_vol = 5;                // volatile through the prefix
const static const int g_bothSides = 2;       // qualifiers on BOTH sides
const volatile static int g_twoQuals = 3;     // two qualifiers, then the class

const typedef int CT;                         // `typedef` is a storage class too
CT g_aliased = 4;

// A qualifier written BETWEEN two storage-class/function specifiers, whose
// PARAMETER carries the other order-free position: `register` is the only
// storage-class specifier C 6.7.6.3p2 admits on a parameter, and a qualifier may
// precede it. DSS admitted neither before this cycle.
static const inline int midOrder(const register int x) { return x + 6; }

// Not `static`: the volatile object must be reached through a real load/store
// so a dropped qualifier cannot be folded to a constant by the release arm.
static int bumpVolatile(void) {
    g_vol = g_vol + 1;
    return g_vol;
}

// Block-scope `const static`: STATIC STORAGE DURATION is the property under
// test. `seen` must persist across calls, so two calls return 1 then 2.
static int callCount(void) {
    static int seen = 0;                      // plain static (the control)
    const static int step = 1;                // qualifier BEFORE the class
    seen = seen + step;
    return seen;
}

int main(void) {
    int acc = 0;

    acc += g_qualFirst;                       // 1
    acc += g_classFirst;                      // 1   -> 2
    acc += g_bothSides;                       // 2   -> 4
    acc += g_twoQuals;                        // 3   -> 7
    acc += g_aliased;                         // 4   -> 11

    acc += bumpVolatile();                    // 6   -> 17

    const register int r = 4;                 // qualifier before `register`
    acc += r;                                 // 4   -> 21

    volatile static int localVol = 2;         // block-scope volatile+static
    localVol = localVol + 1;
    acc += localVol;                          // 3   -> 24

    const static char *msg = "*";             // const on the POINTEE only
    msg = msg + 0;                            // the POINTER object is mutable
    acc += (msg[0] == '*') ? 1 : 0;           // 1   -> 25

    acc += midOrder(1);                       // 7   -> 32

    if (callCount() != 1) return 1;
    if (callCount() != 2) return 2;           // static storage duration held
    acc += 2;                                 // 2   -> 34

    const auto inferred = 8;                  // C23 inferred + a qualifier
    acc += inferred;                          // 8   -> 42

    return acc;
}
