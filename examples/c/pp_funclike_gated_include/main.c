// ★ [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]] — the runtime witness for an
// include whose LIVENESS only a real macro expander can decide.
//
// WHAT THIS PROVES, AND WHY IT HAD TO BE A RUNNABLE EXAMPLE. DSS's preprocessor
// resolves quote-`#include`s in a PRE-SCAN, before the authoritative macro pass,
// because that pass must run over one already-spliced buffer. Deciding which
// includes are live means evaluating `#if` — and the pre-scan used to do that
// with a private evaluator that expanded OBJECT-LIKE macros only. Any guard it
// could not decide became a conservative SKIP, the header was never spliced, and
// the macro pass then REFUSED the program:
//
//     error[P0016]: quote #include "gate_a.h" is LIVE here but the include
//     pre-scan could not evaluate its conditional guard, so the header was
//     never spliced; refusing to silently drop it
//
// So the runtime-observable behaviour here is the strongest kind there is: THIS
// PROGRAM DID NOT COMPILE BEFORE. There is no "wrong answer" arm to witness,
// because a refused include is a hard error rather than a miscompile — the
// witness is that the artifact builds clean and RUNS, and that every value that
// had to arrive through a gated header is present and right.
//
// ★★ REFERENCE GROUND TRUTH, NOT DSS'S OPINION — each probed SEPARATELY,
// 2026-09-03. Every construct below is accepted by WSL gcc 13.3.0, WSL clang
// 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51.36252 (`cl /nologo /c /std:c17`),
// and the three that link also run the program to 42. The union is UNANIMOUS, so
// `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` REQUIRES all of it.
//
// ★ FOUR GATES, FOUR SHAPES, ONE ADDEND EACH — so a gap in ANY ONE takes the
// whole program down rather than hiding behind the others:
//
//     gate_a.h  #if ENABLED(1)        a FUNCTION-LIKE macro in the guard
//     gate_b.h  #if GATE              an OBJECT-like macro expanding TO a call
//     gate_c.h  #if CAT(ON,E)         a `##` PASTE in the guard
//     gate_d.h  #include HDR(x)       a COMPUTED include (C23 6.10.2p4)
//
//     GATE_A + GATE_B + GATE_C + GATE_D = 10 + 11 + 9 + 12 = 42
//
// ★★★ AND THE FIFTH GATE IS THE ONE THAT MUST *NOT* FIRE. `#if ENABLED(0)`
// guards an `#include` of a header that DOES NOT EXIST. C 6.10p1 says a dead
// branch's directives are not processed at all, so a preprocessor that became
// EAGER rather than CORRECT — resolving the include because it could not decide
// the guard — fails to compile this file at all. That is P0016
// (D-PP-CONDITIONAL-INCLUDE-ORDERING) itself, the defect the conservative
// direction existed to prevent, and it is why every positive gate here is
// carried alongside a negative one.

#define ENABLED(x) (x)

#if ENABLED(1)
#include "gate_a.h"
#endif

#define GATE ENABLED(1)

#if GATE
#include "gate_b.h"
#endif

#define CAT(a, b) a##b
#define ONE 1

#if CAT(ON, E)
#include "gate_c.h"
#endif

#define HDR(x) #x
#include HDR(gate_d.h)

// THE NEGATIVE GATE. This header does not exist and must never be looked for.
#if ENABLED(0)
#include "pp_funclike_gated_include_no_such_header.h"
#endif

// A dead branch of the SAME group as a live one, so the negative property is
// also asserted where a group's liveness actually moves.
#if ENABLED(0)
#include "pp_funclike_gated_include_no_such_header.h"
#else
#define ELSE_ARM_TAKEN 1
#endif

int main(void) {
    // 1: every gate's header arrived and its macro survived into the body.
    int const sum = GATE_A_VALUE + GATE_B_VALUE + GATE_C_VALUE + GATE_D_VALUE;
    if (sum != 42) return 1;

    // 2: the headers' real DEFINITIONS landed too, and landed once. This is the
    //    discriminator for the opposite error — a directive dropped instead of
    //    spliced would leave the macro undefined and fail at compile time, but a
    //    header spliced TWICE would define these functions twice and fail to
    //    link, which the sum alone would not catch.
    if (gate_a_value() != 10) return 2;
    if (gate_b_value() != 11) return 3;
    if (gate_c_value() != 9) return 4;
    if (gate_d_value() != 12) return 5;

    // 3: the `#else` of a function-like-gated group was taken, so the guard was
    //    evaluated as FALSE rather than merely left undecided.
    if (ELSE_ARM_TAKEN != 1) return 6;

    // 4: the individual addends, so a compensating pair of wrong values cannot
    //    reach 42 by accident.
    if (GATE_A_VALUE != 10) return 7;
    if (GATE_B_VALUE != 11) return 8;
    if (GATE_C_VALUE != 9) return 9;
    if (GATE_D_VALUE != 12) return 10;

    return sum;
}
