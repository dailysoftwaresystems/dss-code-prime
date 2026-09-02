// ★ D-PP-PRAGMA-RECOGNIZED-SEMANTICS — `#pragma once` runtime witness.
//
// WHAT THIS PROVES, AND WHY IT HAD TO BE A RUNNABLE EXAMPLE. Until 2026-08-29
// DSS did not merely ignore `#pragma once` — `applyPragma` REFUSED THE
// TRANSLATION UNIT (`P_PreprocessorPragma`, exit 1), so every real-world header
// using the commonest guard idiom failed to compile, against an idiom gcc,
// clang and MSVC all accept. So the runtime-observable behaviour is the
// strongest kind there is: THIS PROGRAM DID NOT COMPILE BEFORE.
//
// ★ EVERY HEADER DEFINES A FUNCTION, WHICH IS WHAT MAKES THIS A WITNESS RATHER
// THAN A RESTATEMENT. If any dedup below fails, that header's `int
// once_X_value(void) { ... }` is DEFINED TWICE and the build fails loudly. A
// header carrying only `#define`s could be re-spliced with no consequence at
// all, and the example would prove nothing.
//
// ★ THE SPELLINGS ARE THE POINT. The operator ruled 2026-08-28 that the dedup
// key is IDENTITY, NOT CONTENT, so every spelling of one file must reduce to one
// key. This TU reaches `once_a.h` FIVE ways — directly, transitively through
// `once_b.h`, as `./once_a.h`, as `sub/../once_a.h`, and as `../once_a.h` from
// inside `sub/once_d.h` — and `once_c.h`/`sub/once_d.h` twice each. A
// path-STRING key splices `once_a.h` five times and the link dies on the first
// duplicate.
//
//     A_VALUE + B_VALUE + C_VALUE + D_VALUE = 10 + 11 + 9 + 12 = 42
//
// ★ REFERENCE GROUND TRUTH, not DSS's opinion: this tree compiles clean and the
// built binary independently exits 42 under WSL gcc 13.3.0, WSL clang 18.1.3 and
// mingw-w64 gcc 13.2.0. The `release` arm runs the shipped
// parser->semantic->codegen->native pipeline over the same source, so it also
// witnesses that the optimizer does not disturb the deduped splice.

#include "once_a.h"
#include "once_b.h"          // includes once_a.h AGAIN — a sibling, not a cycle
#include "./once_a.h"        // dot spelling
#include "sub/../once_a.h"   // dot-dot spelling
#include "once_c.h"
#include "sub/../once_c.h"   // the same file through a walk that leaves and returns
#include "sub/once_d.h"      // itself includes ../once_a.h
#include "./sub/once_d.h"    // and again, under another spelling

int main(void) {
    // 1: every header's macro is present exactly once and correct. A refused
    //    include never reaches this line — the compile fails first — but a dedup
    //    that wrongly SKIPPED a header entirely would land here with a missing
    //    macro, so the check is not vacuous against that failure mode either.
    int const sum = A_VALUE + B_VALUE + C_VALUE + D_VALUE;
    if (sum != 42) return 1;

    // 2: every header's real DEFINITION landed, and landed ONCE. This is the
    //    discriminator for the opposite error — splicing the text twice — which
    //    is a duplicate-definition failure rather than a wrong number.
    if (once_a_value() != 10) return 2;
    if (once_b_value() != 11) return 3;
    if (once_c_value() != 9) return 4;
    if (once_d_value() != 12) return 5;

    // 3: the individual addends, so a compensating pair of wrong values cannot
    //    reach 42 by accident.
    if (A_VALUE != 10) return 6;
    if (B_VALUE != 11) return 7;
    if (C_VALUE != 9) return 8;
    if (D_VALUE != 12) return 9;

    return sum;
}
