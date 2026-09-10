// ═══════════════════════════════════════════════════════════════════════════
//  THE VARIADIC MARKER TERMINATES THE PARAMETER LIST
//  (subject: checkVariadicMarkerTerminatesParamList in
//   src/analysis/semantic/semantic_analyzer.cpp)
// ═══════════════════════════════════════════════════════════════════════════
//
// `int f(int a, ..., int b);` compiled at rc 0 with ZERO bytes on stderr, and so
// did its DEFINITION, its function-pointer spelling and the double-marker form.
// ✔MEASURED through the shipped CLI at the pre-change HEAD.
//
// ★★ THE REFERENCES ARE UNANIMOUS AGAINST IT. ✔MEASURED 2026-09-09, each probed
// SEPARATELY on its own translation unit:
//   gcc 13.3.0   `-std=c2x`      rc 1 — `error: expected ')' before ',' token`
//   clang 18.1.3 `-std=c23`      rc 1 — `error: expected ')'`
//   MSVC 19.51.36257 `/std:clatest` rc 2 —
//                 `error C2760: syntax error: ',' was unexpected here; expected ')'`
// All three point at the SEPARATOR that follows the marker. DSS was ABOVE the
// union, the direction this project treats as the worst — a silent accept of
// malformed source. C 6.7.6.3 (C23 6.7.7.4) puts `...` last, full stop.
// ⓘ DSS points at the MARKER instead of at the separator, deliberately: the
// references are reporting a parse expectation ("I wanted `)` here") while DSS
// is reporting the constraint, and the misplaced thing is the `...`. The
// column difference is stated rather than papered over.
//
// ★★★ WHY THIS IS A SEMANTIC CHECK AND NOT A GRAMMAR ONE — MEASURED, five
// shapes, in a PRIVATE `$DSS_CONFIG_ROOT` copy so the live document never moved:
//   (A) `param (sep param)* {optional: sep ellipsis, speculative:true}`,
//   (C) the same with the bare separator TOKEN instead of the `listSeparator`
//       rule, and
//   (E) the same with no `speculative` flag
//       → all three REFUSED AT LOAD, on every case in the battery:
//         `error[C_AmbiguousAlternatives]: at /shapes/paramList: alt branches
//          share FIRST token 'Comma'` — the repeat's loop entry and the trailing
//         optional share the separator. That is the IDENTICAL refusal
//         `/shapes/listSeparator`'s `$whyNotTheStandardSpellingComment` already
//         records for C23's trailing comma.
//   (B) the loop body wrapped in a one-branch `speculative` alt LOADS and
//       refuses all six malformed forms — and REGRESSES LEGAL C23:
//       `int f(int a, ...);` became `error[P_NoAlternativeMatched]` at 1:18,
//       because this engine does not roll a failed repeat iteration back.
//   (D) `/shapes/enumBody`'s own topology — `(sep {optional param,
//       speculative})*` plus a trailing `{optional ellipsis}` — refuses all five
//       misplaced-marker forms at the right column and accepts every legal one,
//       BUT newly ACCEPTS `int f(int a,)`, an illegal trailing separator all
//       three references refuse. Trading one Direction-B silent accept for
//       another is not a fix.
// The check is nonetheless 100% document-driven: the list is whatever rule the
// config names `declarators.fnSuffixParamsRule`, the marker is whatever token it
// names `declarators.variadicMarker`, and a document declaring neither gets no
// check. No language, target or format name appears in the engine arm.
//
// ★ IT RUNS AT `pass1Node`, ONCE PER LIST, AND THAT PLACEMENT IS THE FIX'S
// COMPLETENESS ARGUMENT. Three different resolvers reach a parameter list (the
// shared declarator-suffix FnSig build, the legacy function-declaration build,
// and the abstract/function-pointer path that reaches neither) — and ✔MEASURED,
// `int (*p)(int a, ..., int b);` was accepted at rc 0 by a path neither named
// resolver sees. A per-resolver check would have been the
// [[a partial fix reads as a complete one]] shape; a rule-keyed pre-order visit
// covers every list in the tree exactly once.
//
// ── WHAT EACH GROUP PINS ───────────────────────────────────────────────────
//   * every LEGAL variadic spelling still compiles — a tightening that refuses
//     `int printf_like(const char *fmt, ...);` would be far worse than the
//     defect;
//   * the misplaced marker is refused in the DECLARATION, the DEFINITION, the
//     function-POINTER and the DOUBLE-marker forms — four spellings, one check;
//   * a nested prototype's own `...` is NOT the outer list's business
//     (`int g(int (*cb)(int, ...), int n);` is legal and stays legal) — the
//     scope arm, and the one a subtree scan gets wrong;
//   * the diagnostic is POSITIONED at the marker, not at the declaration.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ───────────────────────────────────────
// ENGINE SOURCE, so SOURCE and OBJECT md5 both move and return: delete the
// `checkVariadicMarkerTerminatesParamList(s, cfg, tree, node);` call from
// `pass1Node`. Every refusal arm below goes red; every legal-spelling control
// stays green. The transcript is in the lane report.

#include "analysis/semantic/semantic_model.hpp"
#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] std::size_t misplacedMarkerCount(SemanticModel const& model) {
    return countCode(model.diagnostics(),
                     DiagnosticCode::S_VariadicMarkerMustEndParameterList);
}

} // namespace

// ── THE CONTROLS FIRST: every legal variadic spelling still compiles ───────
//
// Pinned first and pinned broadly. A "fix" that refuses a real varargs
// prototype breaks every C corpus there is, and `hasErrors() == false` is the
// only assertion that can say so.

TEST(VariadicMarkerPosition, LegalVariadicDeclarationIsUntouched) {
    auto model = analyzeShipped("c", {"int dss_f(int a, ...);\n"});
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(misplacedMarkerCount(model), 0u);
}

TEST(VariadicMarkerPosition, LegalVariadicDefinitionIsUntouched) {
    auto model = analyzeShipped("c", {
        "int dss_f(int a, ...) { return a; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(misplacedMarkerCount(model), 0u);
}

TEST(VariadicMarkerPosition, ALongLegalParameterListIsUntouched) {
    auto model = analyzeShipped("c", {
        "int dss_many(int a, int b, int c, int d, int e, int f, int g, int h,\n"
        "             const char *fmt, ...);\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(misplacedMarkerCount(model), 0u);
}

TEST(VariadicMarkerPosition, ANonVariadicListIsUntouched) {
    auto model = analyzeShipped("c", {
        "int dss_g(int a, int b, int c);\n"
        "int dss_h(void);\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(misplacedMarkerCount(model), 0u);
}

TEST(VariadicMarkerPosition, AVariadicFunctionPointerParameterIsUntouched) {
    auto model = analyzeShipped("c", {
        "int dss_cb(int (*p)(void *, int, ...), int n);\n",
    });
    EXPECT_FALSE(model.hasErrors())
        << "the marker belongs to the NESTED prototype, and that list is legal";
    EXPECT_EQ(misplacedMarkerCount(model), 0u);
}

// ── THE REFUSALS: four spellings of one defect ─────────────────────────────

TEST(VariadicMarkerPosition, MarkerBeforeAParameterIsRefusedInADeclaration) {
    auto model = analyzeShipped("c", {"int dss_bad(int a, ..., int b);\n"});
    EXPECT_EQ(misplacedMarkerCount(model), 1u);
}

TEST(VariadicMarkerPosition, MarkerBeforeAParameterIsRefusedInADefinition) {
    auto model = analyzeShipped("c", {
        "int dss_bad(int a, ..., int b) { return a + b; }\n",
    });
    EXPECT_GE(misplacedMarkerCount(model), 1u);
}

TEST(VariadicMarkerPosition, MarkerBeforeAParameterIsRefusedInAFunctionPointer) {
    auto model = analyzeShipped("c", {"int (*dss_bad)(int a, ..., int b);\n"});
    EXPECT_EQ(misplacedMarkerCount(model), 1u)
        << "the abstract/function-pointer path reaches neither named FnSig "
           "resolver — this is the arm a per-resolver check would have missed";
}

TEST(VariadicMarkerPosition, TwoMarkersAreRefused) {
    auto model = analyzeShipped("c", {"int dss_bad(int a, ..., ...);\n"});
    EXPECT_EQ(misplacedMarkerCount(model), 1u);
}

TEST(VariadicMarkerPosition, TheInnerListIsJudgedOnItsOwn) {
    auto model = analyzeShipped("c", {
        "int dss_bad(int (*cb)(int, ..., int), int n);\n",
    });
    EXPECT_EQ(misplacedMarkerCount(model), 1u)
        << "the NESTED list is the one that violates; the outer one does not";
}

// ── THE DIAGNOSTIC IS POSITIONED AT THE MARKER ────────────────────────────

TEST(VariadicMarkerPosition, TheDiagnosticPointsAtTheMarkerItself) {
    auto model = analyzeShipped("c", {"int dss_bad(int a, ..., int b);\n"});
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_VariadicMarkerMustEndParameterList)
            continue;
        found = true;
        EXPECT_EQ(d.actual, "...")
            << "`.actual` must be the offending token's own text";
        EXPECT_FALSE(d.related.empty())
            << "the parameter that follows is named as a related location";
    }
    EXPECT_TRUE(found);
}
