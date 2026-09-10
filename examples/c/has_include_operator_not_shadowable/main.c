/* TF-C86 (D-CSUBSET-STDARG-F001A) — RUN witness that a program's portability
   shim cannot shadow the `__has_include` operator DSS provides.
 *
 * THE BUG THIS PINS. `/usr/include/sys/cdefs.h:91-93` — and glibc, musl, Boost,
 * zlib — all ship exactly the three lines below. On a compiler that HAS
 * `__has_include`, `#ifndef __has_include` is false and the shim is dead. DSS
 * used to read the name as undefined, take the arm, and replace an operator it
 * implements with a function-like macro answering 0 forever. The visible damage
 * was not the 0: the preprocessor's include pre-scan refuses to evaluate a
 * function-like macro in a guard (the conservative P0016-safe direction), so
 * every `#if __has_include(<h>)`-guarded include went UNCERTAIN, its textual
 * splice was skipped, the directive survived to the post-parse import resolver,
 * and that reported `F001A: <h> not found` — for headers sitting readable on
 * the include path. MEASURED on the sqlite corpus: 5 of the 7 macho F001A.
 *
 * WHY THIS IS A RUN WITNESS AND NOT A COMPILE-ONLY ONE. Everything above is
 * decided in the preprocessor, so a compile-only example would witness it. But
 * the property worth pinning is stronger: the guarded header is really SPLICED,
 * its macro really EXPANDS, and its function really gets CODEGENNED, LINKED and
 * CALLED. `HAS_INCLUDE_LIVE` (40) comes from the spliced header's `#define`;
 * `has_include_probe_marker()` (2) is its function, resolved at link time. Only
 * a real splice can produce both. 40 + 2 = exit 42.
 *
 * `__has_include` is format-neutral (it is C23 6.10.1, not a platform feature),
 * so all three object formats run it.
 */

/* The shim, byte-for-byte as the SDK writes it. It MUST be dead here. */
#ifndef __has_include
#define __has_include(x) 0
#endif

/* ⚠ THIS COMMENT WAS FALSE FROM 2026-09-04 UNTIL IT WAS CORRECTED, AND THE
   CORRECTION IS THE POINT: it used to say an unguarded
   `#define __has_include(x) 0` "would be REFUSED loudly
   (P_PreprocessorOperatorNameNotDefinable, unsuppressable)". It is now ACCEPTED
   with a warning and APPLIED — an operator ruling of 2026-09-03, on the
   measurement that all four references accept and apply it and that being
   stricter than the entire union is not rigor. `defined` is the only name that
   code still refuses.
   ⇒ WHAT THIS EXAMPLE ASSERTS IS UNCHANGED AND IS THE GUARDED FORM: the shim
   above must stay DEAD (`#ifndef __has_include` is false because the operator
   IS provided) and therefore SILENT. That property does not depend on what an
   UNGUARDED define would do, which is why this example stayed green across the
   ruling while its comment went false. */

#if __has_include("local_probe.h")
#include "local_probe.h"
#else
/* If this arm is ever taken the build FAILS LOUD rather than quietly returning
   a different exit code -- a wrong-but-running witness is worse than none. */
#error "__has_include of local_probe.h read 0: the operator was shadowed"
#endif

/* The operator must also be visible to `defined()` and `#ifdef`, which is the
   half that makes the shim dead in the first place. */
#ifndef __has_include
#error "#ifndef __has_include was TAKEN: the operator is not a defined name"
#endif
#if !defined(__has_include)
#error "!defined(__has_include) was TRUE: defined() disagrees with #ifdef"
#endif

int main(void) {
    /* 40 from the spliced header's macro + 2 from its linked function. */
    return HAS_INCLUDE_LIVE + has_include_probe_marker();
}
