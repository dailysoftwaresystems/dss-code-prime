/* [[D-PP-COMPUTED-INCLUDE-SILENT-DROP]] -- the C23 6.10.2p4 COMPUTED `#include`
 * runtime witness, exercising all four shapes end to end through every target leg.
 *
 * THE DEFECT THIS WITNESSES. `#include MACRO` was SILENTLY DROPPED. The include
 * pre-scan could not resolve an operand that was not a literal header name, so
 * it left the line verbatim; the macro pass's fail-loud arm keyed on the QUOTE
 * OPENER token, so a `Word`-led `#include` fell through to the inert forward;
 * the forwarded tokens then expanded in the ORDINARY BODY STREAM into a
 * perfectly well-formed `#include "h"`, which the parser's `includeDirective`
 * rule (`hirKind: Skip`) lowered to NOTHING. ✔MEASURED at dac121cc: a computed
 * include of a MISSING header compiled rc=0, emitted an artifact, and reported
 * nothing whatsoever -- a header deleted from the translation unit in silence.
 *
 * THE REFERENCE SET IS UNANIMOUS, probed SEPARATELY (2026-09-01): gcc 13.3.0 and
 * clang 18.1.3 compile AND run every shape below; MSVC 19.51.36252 resolves
 * every one. `DSS = (gcc union clang union MSVC) union ISO C` therefore REQUIRES
 * the resolution, not merely a loud refusal.
 *
 * FOUR SHAPES, FOUR ADDENDS, EACH ALSO WRONG ALONE:
 *   (1) QUOTE via an object-like macro          -> PART_A   = 30
 *   (2) QUOTE via a CHAIN of object-like macros -> PART_B   =  4
 *   (3) ANGLE via an object-like macro, into the shipped `<limits.h>`
 *       descriptor (the form that must be NORMALIZED to its literal spelling,
 *       because the post-parse import resolver reads the angle form only)
 *                                               -> CHAR_BIT =  8
 *   (4) a DEAD-BRANCH computed include of a header that DOES NOT EXIST. This is
 *       the NEGATIVE the resolution owes: required reference behaviour is that
 *       it is never resolved and never reported (✔MEASURED, gcc compiles a
 *       dead-branch include of a missing header rc=0). An over-eager pre-scan
 *       would hard-error here and this example would never build.
 *   30 + 4 + 8 = 42.
 *
 * RED-ON-DISABLE. Every addend arrives as a MACRO from a header the computed
 * form had to resolve, so a regression in any one of (1)-(3) leaves an
 * undeclared identifier and the program does not compile -- it cannot degrade
 * into a wrong exit code. A regression in (4) fails the build loudly instead.
 * The operands reach `combine` as FUNCTION ARGUMENTS so the baseline arm keeps a
 * live runtime add rather than const-folding the whole thing to one immediate,
 * and the `release` arm re-runs the same source through the shipped optimizer. */

#define HDR "computed_parts.h"
#include HDR

#define HDR_NAME HDR2
#define HDR2 "computed_parts_two.h"
#include HDR_NAME

#define SYS_HDR <limits.h>
#include SYS_HDR

#define MISSING_HDR "no_such_header_at_all.h"
#if 0
#include MISSING_HDR
#endif

int combine(int a, int b, int c) {
    return a + b + c;
}

int main(void) {
    return combine(PART_A, PART_B, CHAR_BIT);
}
