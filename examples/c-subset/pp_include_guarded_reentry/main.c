// ★ TF-C87 (D-PP-INCLUDE-REENTRY-GUARD-AWARE) — the guard-aware include
// RE-ENTRY runtime witness.
//
// WHAT THIS PROVES, AND WHY IT HAD TO BE A RUNNABLE EXAMPLE. Before TF-C87 the
// preprocessor REFUSED to re-enter any header already on the include stack and
// emitted `P0016 circular include of X`. That rejects LEGAL, STANDARD-
// CONFORMING C: a real cpp has NO refuse-re-entry rule at all — it has a
// nesting DEPTH limit, and the include GUARD is what terminates the cycle,
// because the second entry finds the guard macro already defined and expands to
// nothing. MEASURED on the macho corpus leg at 5093341, `src/mem1.c`:
//     mach/mach_types.h -> mach/task_policy.h -> mach/mach_types.h
// produced 4 `F001A` + 1 `P0016`, and that single false positive was the leg's
// ENTIRE residual F001A count.
//
// So the runtime-observable behaviour here is the strongest kind there is: THIS
// PROGRAM DID NOT COMPILE BEFORE. There is no "wrong answer" arm to witness,
// because a refused include is a hard error, not a miscompile — the witness is
// that the artifact builds clean and RUNS, and that every macro that had to
// survive a back-edge is present with the right value.
//
// ★ WHY FOUR HEADERS RATHER THAN ONE. Guard DETECTION now GATES re-entry, so an
// unrecognised but LEGAL guard becomes a refused include — which presents to a
// user as a compiler bug. The sqlite corpus exercises exactly ONE spelling
// (`#ifndef`), so a witness built on that alone would leave every other spelling
// untested at runtime. Each header below carries a DIFFERENT guard form, every
// one of them MEASURED in the real macOS SDK and/or the sqlite tree, and each
// contributes one addend — so a detector gap on ANY single form takes the whole
// program down rather than hiding behind the others:
//
//     reentry_a.h  FORM 1  #ifndef X / #define X        (2942 SDK, 35 sqlite)
//     reentry_b.h  FORM 2  #if !defined(X)              (10 SDK, 1 sqlite)
//     reentry_c.h  FORM 3  compound, MIXED polarity     (2 sqlite, verbatim)
//     reentry_d.h  FORM 4  guard not first + #define not adjacent (11+15 SDK)
//
// The include graph is a genuine cycle with THREE distinct back edges
// (b.h -> a.h, d.h -> a.h, d.h -> b.h), all of them into a header that is on
// the include stack at that moment.
//
//     A_VALUE + B_VALUE + C_VALUE + D_VALUE = 10 + 11 + 9 + 12 = 42
//
// CLANG GROUND TRUTH, not DSS's opinion: this tree is clean under
// `/usr/bin/clang -std=c11 -Wall -Wextra -fsyntax-only` and the clang-built
// binary independently exits 42. The `release` arm runs the shipped
// parser->semantic->codegen->native pipeline over the same source.

#include "reentry_a.h"

int main(void) {
    // 1: every guard FORM's macro survived its back-edge. A refused re-entry
    //    never reaches this line — the compile fails first — but a detector
    //    that wrongly SKIPPED a header instead of refusing it would land here
    //    with a missing macro, so the check is not vacuous against that
    //    (worse) failure mode either.
    int const sum = A_VALUE + B_VALUE + C_VALUE + D_VALUE;
    if (sum != 42) return 1;

    // 2: the deepest header's real DEFINITION landed, and landed ONCE. This is
    //    the discriminator for the opposite error — permitting re-entry but
    //    failing to let the guard EMPTY the re-entered copy would define
    //    `reentry_d_value` twice, which is a duplicate-definition failure
    //    rather than a wrong number.
    if (reentry_d_value() != 12) return 2;

    // 3: the individual addends, so a compensating pair of wrong values cannot
    //    reach 42 by accident.
    if (A_VALUE != 10) return 3;
    if (B_VALUE != 11) return 4;
    if (C_VALUE != 9) return 5;
    if (D_VALUE != 12) return 6;

    return sum;
}
