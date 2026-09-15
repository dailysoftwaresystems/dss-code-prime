// [[D-CSUBSET-COMPILER-FEATURE-QUERY-OPERATORS]] — RUNTIME WITNESS FOR THE
// PORTABLE FEATURE-QUERY SHIM, i.e. the shape every real-world header uses to
// span compilers that implement DIFFERENT SUBSETS of the `__has_*` family.
//
//     #ifndef __has_attribute
//     #define __has_attribute(x) 0
//     #endif
//
// The Apple SDK's `sys/cdefs.h` ships that block for FIVE operators; glibc,
// musl, Boost and zlib ship the same three lines. On a compiler that HAS the
// operator the block is DEAD; on one that does not, it installs a fallback that
// answers 0.
//
// ⚠⚠ THE SENTENCE THAT STOOD HERE WENT FALSE ON 2026-09-04 AND THIS EXAMPLE
// STAYED GREEN THROUGHOUT — which is exactly why it is worth reading. It said
// DSS implements `__has_include` / `__has_embed` / `__has_c_attribute` and NOT
// `__has_attribute` / `__has_builtin` / `__has_feature` / `__has_extension`,
// "so all four shims here are LIVE". P59 shipped all four operators (operator
// ruling 2026-09-03), so ALL FOUR SHIMS HERE ARE NOW DEAD — the `#ifndef` is
// false and the fallback never installs.
//
// ★ THE ASSERTIONS DID NOT MOVE, BECAUSE THE ANSWERS DID NOT: every query below
// takes a deliberately bogus argument, and 0 is the correct answer whether it
// comes from a live shim or from the real operator. That is what makes this a
// FALLBACK witness rather than a capability witness — and it is also why
// nothing reddened when the world underneath it changed. A green example is not
// evidence that its own header is still true.
// ⇒ What it proves today: a build in which the shims are DEAD is still a
// WORKING one, not merely a non-erroring one. The LIVE-shim path it was written
// for is no longer reachable on DSS for these four names.
//
// ═══ WHY EVERY QUERY BELOW TAKES A DELIBERATELY BOGUS ARGUMENT ═══════════════
//
// ★★★ A feature query is the one construct whose CORRECT answer differs between
// conforming implementations — `__has_attribute(cold)` is 1 on gcc and clang and
// unavailable on MSVC — so an example that asserted a REAL capability could not
// have one expected exit code across the union, and "expected" would silently
// mean "whatever this compiler happens to do". A BOGUS argument has ONE right
// answer everywhere: 0. ✔MEASURED 2026-09-03, each reference probed separately:
// gcc 13.3.0 (WSL), gcc 13.2.0 (mingw), clang 18.1.3 and cl 19.51.36252 all
// answer 0 for a nonsense attribute / builtin / feature / extension name, and
// all answer 1 for `__has_include` of a header that is really there.
//
// The five weights sum to 42 and NO TWO ARE EQUAL, so a wrong answer from any
// one operator changes the exit code — including the case where two operators
// are wrong in opposite directions. FOLD-RESISTANT: the result feeds the live
// conditional-compilation decision AND the `#include` of the header that
// supplies the last weight.
//
// RED-ON-DISABLE: making any query answer non-zero for its bogus argument (the
// [[D-CONFIG-COMMENT-CLAIM-ROT]] failure this row exists to prevent) drops that
// weight and the exit code stops being 42. Refusing the shim's `#define` —
// which is what adding these four names to `isConditionalInclusionOperator`
// would do — fails the build outright.

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#ifndef __has_extension
#define __has_extension(x) 0
#endif

#if __has_attribute(dss_no_such_attribute_xyz)
#define FQ_ATTR_WEIGHT 0
#else
#define FQ_ATTR_WEIGHT 8
#endif

#if __has_builtin(__dss_no_such_builtin_xyz)
#define FQ_BUILTIN_WEIGHT 0
#else
#define FQ_BUILTIN_WEIGHT 16
#endif

#if __has_feature(dss_no_such_feature_xyz)
#define FQ_FEATURE_WEIGHT 0
#else
#define FQ_FEATURE_WEIGHT 4
#endif

#if __has_extension(dss_no_such_extension_xyz)
#define FQ_EXTENSION_WEIGHT 0
#else
#define FQ_EXTENSION_WEIGHT 2
#endif

// The POSITIVE control, and the reason this example is not only about absences:
// `__has_include` is an operator DSS really implements, it answers 1 for a
// header that is really there on all four references, and the TAKEN arm then
// includes it — so the last weight arrives from the header rather than from the
// probe folding.
#if __has_include("fq_shim_defs.h")
#include "fq_shim_defs.h"
#else
#define FQ_INCLUDE_WEIGHT 0
#endif

int main(void) {
    return FQ_ATTR_WEIGHT + FQ_BUILTIN_WEIGHT + FQ_FEATURE_WEIGHT
         + FQ_EXTENSION_WEIGHT + FQ_INCLUDE_WEIGHT;   // 8+16+4+2+12 = 42
}
