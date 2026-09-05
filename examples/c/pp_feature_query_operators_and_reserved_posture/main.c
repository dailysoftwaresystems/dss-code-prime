// [[D-PP-HAS-EXTENSION-BUILTIN-ABSENT]] — RUN WITNESS FOR BOTH OPERATOR
// RULINGS OF 2026-09-03, in one program, on one exit code.
//
//   RULING 1 — ship `__has_attribute`, `__has_builtin`, `__has_feature` and
//   `__has_extension` as real feature-query operators, 100% config driven,
//   answering from the capability sets `c.lang.json` already declares.
//
//   RULING 2, verbatim — "we must accept too. best long term solution, no
//   workaround, first class implementation, 100% config driven. leave nothing
//   to be done." — DSS must ACCEPT `#define __has_include(x) 0` and
//   `#undef __has_include`, diagnose at most a warning, and APPLY them.
//
// ═══ WHY A RUN WITNESS AND NOT A COMPILE-ONLY ONE ════════════════════════════
//
// Everything here is decided in the preprocessor, so a compile-only entry would
// witness that it BUILDS. The property worth pinning is stronger and is one
// no unit assertion can reach: the answers survive the whole pipeline into an
// artifact that EXECUTES and returns them. Each of the eight claims below
// carries a DISTINCT positive weight and contributes 0 when it is wrong, so any
// wrong answer strictly lowers the sum — two errors cannot cancel, and exit 42
// is reachable only if all eight hold at once.
//
//   1 `__has_attribute(deprecated)` answers 1
//   2 `__has_attribute(<a name nothing declares>)` answers 0
//   3 `__has_builtin(__builtin_offsetof)` answers 1
//   4 `__has_builtin(<a name nothing declares>)` answers 0
//   5 `__has_feature(c_static_assert)` answers 1
//   6 `__has_extension(c_static_assert)` answers 1
//   7 `#define __has_include(x) 0` is ACCEPTED and APPLIED
//  14 `#undef  __has_include`      is ACCEPTED and APPLIED
//   = 42
//
// The two `#error` blocks are deliberately NOT weights: a shim that goes live,
// or a query that is refused outright, must fail the BUILD rather than quietly
// return a different number.
//
// ═══ THE REFERENCE MATRIX ════════════════════════════════════════════════════
//
// ✔MEASURED 2026-09-04, each reference invoked SEPARATELY on its own fixture
// (`-E -P` for the three gcc/clang builds; `cl /nologo /std:c17 /EP`, and again
// with `/Zc:preprocessor`, for MSVC — a front-end question needs no vcvars):
//
//   claim                                gcc 13.3.0  gcc 13.2.0  clang    cl
//                                          (WSL)      (mingw)   18.1.3   19.51
//   `#ifdef __has_attribute`/`__has_builtin`  yes       yes        yes     NO
//   `#ifdef __has_feature`/`__has_extension`  no        no         yes     no
//   `__has_attribute(deprecated)`              1         1          1     n/a
//   `__has_builtin(__builtin_offsetof)`        1         1          1     n/a
//   `__has_feature(c_static_assert)`         n/a       n/a          1     n/a
//   `__has_extension(c_static_assert)`       n/a       n/a          1     n/a
//   CONTROL — any name nothing declares        0         0          0     n/a
//   `#define __has_include(x) 0`            warn      warn       warn   silent
//     ...and APPLIED                          yes       yes        yes     yes
//   `#undef __has_include`                  warn      warn       warn   silent
//     ...and APPLIED                          yes       yes        yes      NO
//
// ★ MSVC's `no` on `__has_attribute` is not an opinion about the NAME — it is
// MSVC correctly reporting that it has no such operator, which is exactly what
// makes the universal `#ifndef` shim work there. An abstention is not a vote
// against, and `__has_attribute`/`__has_builtin` still have THREE working
// references while `__has_feature`/`__has_extension` have one. One working
// reference makes a construct REQUIRED.
//
// ⚠⚠ NO SINGLE REFERENCE PRODUCES 42 ON THIS FILE, and that is a property of
// the union (a per-CONSTRUCT disjunction) rather than a weakness of the entry:
// gcc and mingw gcc lack `__has_feature`/`__has_extension` entirely, MSVC lacks
// four of the six queries and ignores the `#undef`. Every claim here is
// delivered by at least one reference; no one reference delivers all eight.
//
// ⚠ AND ONE HALF OF THE MEASUREMENT THIS ROW'S RULING WAS ASKED ON IS REFUTED.
// The row records that all four references "APPLY" the `#define`/`#undef`.
// ✔MEASURED: cl 19.51 ACCEPTS `#undef __has_include` and then IGNORES it —
// `#ifdef __has_include` is still true afterwards and the operator still answers
// 1. Acceptance is unanimous; MEANING is 3-1. DSS follows the three, on the
// tie-break [[D-PP-VA-SPECIAL-IDENTIFIER-NAME-POSITIONS-REFUSED-ABOVE-THE-UNION]]
// already ruled: a reference that SILENTLY DISCARDS code the author wrote does
// not get to be the model. Claim 14 is therefore a deliberate, recorded
// divergence from cl.
//
// ★ 100% CONFIG DRIVEN. Every operator word below is read from the active
// language document (`preprocess.featureQueryOperators[].name`), every answer
// from a capability set that document already declares
// (`semantics.attributeSemantics.effects`, `semantics.builtinFunctions`,
// `preprocess.languageFeatures`, `preprocess.builtinQueryKeywordTokens`), and
// every severity from `preprocess.reservedIdentifiers`. Nothing here names a
// literal in `src/`.

// ── THE SHIM MUST BE DEAD, FOR ALL FOUR ─────────────────────────────────────
// This is the block the Apple SDK's `sys/cdefs.h` ships for five operators, and
// glibc / musl / Boost / zlib ship for three. On a compiler that HAS the
// operator it is DEAD; taking it would shadow the real operator with a
// function-like macro answering 0 forever — the TF-C86 `F001A` cascade.
// A build failure, not a weight: a shadowed operator must never be able to
// produce a plausible-looking exit code.
#ifndef __has_attribute
#error "__has_attribute is not #ifdef-visible: the portable shim would SHADOW it"
#endif
#ifndef __has_builtin
#error "__has_builtin is not #ifdef-visible: the portable shim would SHADOW it"
#endif
#ifndef __has_feature
#error "__has_feature is not #ifdef-visible: the portable shim would SHADOW it"
#endif
#ifndef __has_extension
#error "__has_extension is not #ifdef-visible: the portable shim would SHADOW it"
#endif

// ── (1) A DECLARED ATTRIBUTE ANSWERS 1 ──────────────────────────────────────
// `deprecated` is declared in `semantics.attributeSemantics.effects` (as
// `warnOnUse`) AND in `preprocess.knownCAttributes`. The answer is READ from
// those tables; a second hand-kept list is the defect the ruling names.
#if __has_attribute(deprecated)
#define W_ATTR_YES 1
#else
#define W_ATTR_YES 0
#endif

// ── (2) THE CONTROL: AN UNDECLARED ATTRIBUTE ANSWERS 0, AND IS NOT AN ERROR ──
// ★ An operator that answers 1 for something this implementation does not
// implement is STRICTLY WORSE than not having the operator: it routes a
// portable header onto a path DSS cannot honour, turning a working fallback
// into a silent miscompile.
#if __has_attribute(dss_no_such_attribute_xyz)
#define W_ATTR_NO 0
#else
#define W_ATTR_NO 2
#endif

// ── (3) A BUILTIN DECLARED AS A GRAMMAR KEYWORD ANSWERS 1 ───────────────────
// `__builtin_offsetof` is not a `semantics.builtinFunctions` row — it is an
// OPERATOR wearing a call's punctuation (its operands are a type-name and a
// member designator, not values), so it is declared as a keyword. It is still a
// builtin, and `preprocess.builtinQueryKeywordTokens` names its KIND so the
// answer is read back out of the keyword table rather than restated. All three
// implementing references answer 1, so a 0 here would be a WRONG answer about a
// builtin DSS demonstrably has, not a conservative one.
#if __has_builtin(__builtin_offsetof)
#define W_BUILTIN_YES 3
#else
#define W_BUILTIN_YES 0
#endif

// ── (4) THE CONTROL FOR BUILTINS ────────────────────────────────────────────
// `__is_target_arch` is a clang target-introspection builtin DSS does not have.
// Answering 1 would route `TargetConditionals.h` onto its `__is_target_arch`
// detection block — a path DSS cannot honour — instead of the legacy `__GNUC__`
// ladder that WORKS.
#if __has_builtin(__is_target_arch)
#define W_BUILTIN_NO 0
#else
#define W_BUILTIN_NO 4
#endif

// ── (5)(6) A DECLARED LANGUAGE FEATURE, UNDER BOTH OPERATORS ────────────────
// `preprocess.languageFeatures` is the truth set, and `__has_extension` is a
// strict SUPERSET of `__has_feature` — clang's own two-tier model, ✔MEASURED
// rather than recalled (in `-std=c89` clang answers 0 under `__has_feature` for
// all six C features and 1 under `__has_extension`). `_Static_assert` is used
// for real below, so the claim is not only declared but exercised.
#if __has_feature(c_static_assert)
#define W_FEATURE 5
#else
#define W_FEATURE 0
#endif
#if __has_extension(c_static_assert)
#define W_EXTENSION 6
#else
#define W_EXTENSION 0
#endif

// ── THE POSITIVE CONTROL FOR `__has_include`, AND IT IS LOAD-BEARING ────────
// Without it, the 0 that claim (7) reads is equally consistent with "the header
// was never found" — which is precisely what MSVC's answer looked like while
// this row was being measured, until the control was run. Here the UNTOUCHED
// operator must find the sibling header AND the header must really SPLICE.
#if __has_include("fq_ops_local.h")
#include "fq_ops_local.h"
#else
#error "CONTROL FAILED: the untouched __has_include must find fq_ops_local.h"
#endif
#ifndef FQ_OPS_HEADER_REALLY_SPLICED
#error "CONTROL FAILED: the guarded header was not textually spliced"
#endif

// ── (7) `#define __has_include(x) 0` IS ACCEPTED **AND APPLIED** ────────────
// Until 2026-09-03 this was an unsuppressable Error. C23 6.10.10p2 really does
// reserve the identifier — which is exactly why this needed a RULING rather
// than a fix: the union's ISO C vertex and its three implementation vertices
// disagreed, and DSS had picked the standard over every implementation.
// ⚠ ACCEPTING AND IGNORING WOULD NOT SATISFY THE RULING. It is a silent wrong
// answer, and this arm is what tells the two apart: the control above proves
// the operator answers 1 for this same header.
#define __has_include(x) 0
#if __has_include("fq_ops_local.h")
#define W_DEFINE 0
#else
#define W_DEFINE 7
#endif

// ── (14) `#undef __has_include` IS ACCEPTED **AND APPLIED** ─────────────────
// The heaviest weight, because it is the claim with the weakest reference
// support: 3 of 4 apply it and cl ignores it. An operator lives in the CONFIG,
// not in the macro table, so nothing the ordinary `#undef` path does can reach
// it — this arm is what proves the engine records the revocation rather than
// emitting a warning that changes nothing.
#undef __has_include
#ifdef __has_include
#define W_UNDEF 0
#else
#define W_UNDEF 14
#endif

_Static_assert(W_ATTR_YES + W_ATTR_NO + W_BUILTIN_YES + W_BUILTIN_NO
                   + W_FEATURE + W_EXTENSION + W_DEFINE + W_UNDEF
               == 42,
               "one of the eight claims answered wrong; the addends are "
               "distinct and every wrong answer contributes 0, so the sum can "
               "only be 42 when all eight hold");

int main(void) {
    return W_ATTR_YES + W_ATTR_NO + W_BUILTIN_YES + W_BUILTIN_NO
         + W_FEATURE + W_EXTENSION + W_DEFINE + W_UNDEF;   /* 42 */
}
