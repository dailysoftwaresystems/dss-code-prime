// ═══════════════════════════════════════════════════════════════════════════
//  THE BARE-ELLIPSIS PARAMETER LIST  —  `int f(...);`
//  (subject: /shapes/paramList in src/dss-config/sources/c.lang.json)
// ═══════════════════════════════════════════════════════════════════════════
//
// `int f(...);` was `error[P_NoAlternativeMatched]: expected 'StringStart',
// 'EndStatement', … or 'BlockOpen' — got '('` at 1:6 — the parameter list never
// opened at all, because `/shapes/paramList` required a `param` before the
// marker could be reached. ✔MEASURED through the shipped CLI at the pre-change
// HEAD.
//
// ★★★ C23 6.7.7.1 GIVES `parameter-type-list` THREE PRODUCTIONS, AND DSS
// CARRIED TWO:
//     parameter-type-list:
//         parameter-list
//         parameter-list , ...
//         ...
// The third is the bare marker: a function that is variadic with ZERO named
// parameters. It is new in C23 and pairs with the one-argument `va_start(ap)`
// the same edition introduced — there is no `last` parameter to name — which
// DSS already accepts.
//
// ★★ THE REFERENCES, EACH PROBED SEPARATELY ON ITS OWN TRANSLATION UNIT,
// ✔MEASURED 2026-09-09:
//   gcc 13.3.0   `-std=c2x`         rc 0, SILENT at `-Wall -Wextra` AND at
//                                   `-pedantic-errors`
//   clang 18.1.3 `-std=c23`         rc 0, likewise silent in both arms
//   MSVC 19.51.36257 `/std:clatest` rc 2 — `C2143: syntax error: missing ')'
//                                   before '...'`
// TWO accepting references ⇒ REQUIRED (`DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C`,
// the union over what WORKS). Both accepting references cover the declaration,
// the definition, a zero-argument call, a call with arguments, the
// function-POINTER spelling, the typedef spelling, an abstract parameter, and
// `va_start(ap)` + `va_arg` reading the arguments back. Neither accepts it
// before C23: at `-std=c17 -pedantic-errors` gcc says `ISO C requires a named
// argument before '...' before C2X` and clang `ISO C requires a named parameter
// before '...'`.
//
// ★ THE SHAPE: `paramList` became a TOP-LEVEL `{alt}` whose first branch is
// `ellipsisParam` and whose second is the previous body as an INLINE
// `{sequence}`. FIRST(ellipsisParam) = {EllipsisOp} is disjoint from
// FIRST(param), so the choice is 1-token predictive and nothing became
// speculative; and an inline sequence materialises no CST node, so every
// already-parsing list keeps a byte-identical child sequence.
//
// ⚠⚠ P66's SIBLING CONSTRAINT IS NOT WEAKENED — the two shapes are different
// and both stay refused. A bare `...` IS the whole list. A `...` FOLLOWED by a
// parameter is an error in BOTH spellings: `int f(..., int b);` is refused at
// PARSE (branch 1 ends the list, so the `,` has nowhere to go — `P_UnexpectedToken:
// expected 'ParenClose' — got ','`, which is what gcc, clang and MSVC each
// report in their own words), and `int f(int a, ..., int b);` still reds
// `S_VariadicMarkerMustEndParameterList` from
// `checkVariadicMarkerTerminatesParamList`. Both are pinned below, and
// `tests/analysis/semantic/test_variadic_marker_position.cpp` pins the second
// in nine more spellings.
//
// ⚠ WHY THE REFUSAL ARMS READ **BOTH** TIERS AND NOT `model.hasErrors()` ALONE
// — this is a measurement, and an earlier draft of this file got it wrong. A
// construct the PARSER rejects never reaches the analyzer, so the semantic
// model comes back EMPTY AND ERROR-FREE for a translation unit the compiler
// refused outright: ✔MEASURED, `int f(..., int b);` is rc 1 through the shipped
// CLI while `analyzeShipped({…}).hasErrors()` is FALSE. An arm that read only
// the model would have passed while asserting nothing.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ───────────────────────────────────────
// CONFIG DOCUMENT, so there is no object md5 to move — the CONFIG md5 is the
// moved artefact (`c.lang.json` is copied into `dss-config-snapshot` at ctest
// RUN time, so nothing recompiles). Remove the `"ellipsisParam"` branch from
// `/shapes/paramList`'s alt, leaving the inline sequence as the sole branch:
// every acceptance arm below goes red and every refusal arm and every control
// stays green. The transcript is in the lane report.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] std::size_t misplacedMarkerCount(SemanticModel const& model) {
    return countCode(model.diagnostics(),
                     DiagnosticCode::S_VariadicMarkerMustEndParameterList);
}

[[nodiscard]] std::size_t argCountMismatches(SemanticModel const& model) {
    return countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch);
}

// "Does DSS refuse this program?" is a question about the WHOLE front end, and
// it has to be asked of both tiers — see the ⚠ paragraph in the header.
[[nodiscard]] bool frontEndRefuses(std::string source) {
    auto cu = buildShippedUnit("c", {std::move(source)});
    for (auto const& t : cu->trees())
        if (t.diagnostics().hasErrors()) return true;
    return analyze(cu, DiagnosticBudget::libraryDefault()).hasErrors();
}

} // namespace

// ── THE CONTROLS FIRST ─────────────────────────────────────────────────────
//
// A widening that broke an ordinary prototype would be far worse than the gap
// it closes, so the shapes every C corpus depends on are pinned before the new
// one.

TEST(BareEllipsisParameterList, OrdinaryPrototypesAreUntouched) {
    auto model = analyzeShipped("c", {
        "int dss_named(int a, ...);\n"
        "int dss_void(void);\n"
        "int dss_two(int a, int b);\n"
        "int dss_abstract(int, int);\n"
        "int dss_nested(int (*cb)(int, ...), int n);\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(misplacedMarkerCount(model), 0u);
    EXPECT_FALSE(frontEndRefuses("int dss_named(int a, ...);\n"));
}

// ── THE GAP: a BARE `...` is the whole parameter list ──────────────────────

TEST(BareEllipsisParameterList, ADeclarationIsAccepted) {
    EXPECT_FALSE(frontEndRefuses("int dss_bare(...);\n"))
        << "gcc 13.3.0 -std=c2x and clang 18.1.3 -std=c23 both accept this "
           "silently, at -Wall -Wextra and at -pedantic-errors";

    auto model = analyzeShipped("c", {"int dss_bare(...);\n"});
    EXPECT_EQ(misplacedMarkerCount(model), 0u)
        << "a BARE marker IS the whole list — nothing follows it, so the "
           "P66 terminator constraint has nothing to report";
}

TEST(BareEllipsisParameterList, ADefinitionIsAccepted) {
    EXPECT_FALSE(frontEndRefuses("int dss_bare(...) { return 7; }\n"));

    auto model = analyzeShipped("c", {"int dss_bare(...) { return 7; }\n"});
    EXPECT_EQ(misplacedMarkerCount(model), 0u);
}

TEST(BareEllipsisParameterList, TheDerivedTypeSpellingsAreAccepted) {
    EXPECT_FALSE(frontEndRefuses(
        "int (*dss_p)(...);\n"
        "typedef int dss_F(...);\n"
        "dss_F *dss_g;\n"
        "int dss_takes(int (*cb)(...));\n"))
        << "gcc and clang accept the function-pointer, typedef and abstract-"
           "parameter spellings too — one shape, one rule, every position";
}

// ★★ THE ACCEPTANCE IS NOT THE CLAIM — THE FUNCTION MUST BE **VARIADIC** WITH
// ZERO FIXED PARAMETERS, AND THAT HAS A MEASURABLE CONSEQUENCE. If the list
// were read as an ordinary EMPTY parameter list, every one of these calls would
// draw `S_ArgCountMismatch` (✔MEASURED at this base: `int f(void);` called as
// `f(1)` reports exactly that). Silence across three different argument counts
// is what says the FnSig carries `isVariadic` rather than arity 0.
TEST(BareEllipsisParameterList, ItIsVariadicAndAcceptsAnyArgumentCount) {
    auto model = analyzeShipped("c", {
        "int dss_bare(...);\n"
        "int dss_call(void) {\n"
        "    return dss_bare() + dss_bare(1) + dss_bare(1, 2, 3);\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(argCountMismatches(model), 0u)
        << "a bare `...` list is VARIADIC with zero fixed parameters; read as "
           "an empty fixed list, all three calls would be arity errors";
}

// The DISCRIMINATING negative control for the arm above, printed by name: the
// same shape against a genuinely empty parameter list DOES report. Without it
// the assertion above is satisfied by a compiler that never checks arity.
TEST(BareEllipsisParameterList, TheEmptyFixedListStillRefusesExtraArguments) {
    auto model = analyzeShipped("c", {
        "int dss_fixed(void);\n"
        "int dss_call(void) { return dss_fixed(1); }\n",
    });
    EXPECT_GE(argCountMismatches(model), 1u)
        << "CONTROL: `(void)` is not `(...)`, and the arity check that makes "
           "the previous arm meaningful must still be live";
}

// ── THE REFUSALS: `...` STILL TERMINATES THE LIST ──────────────────────────
//
// Both spellings are refused by gcc, clang AND MSVC — ✔MEASURED 2026-09-09,
// each on its own TU. This widening must not have bought either of them.

TEST(BareEllipsisParameterList, AParameterAfterTheBareMarkerIsRefused) {
    EXPECT_TRUE(frontEndRefuses("int dss_bad(..., int b);\n"))
        << "gcc: `expected ')' before ',' token`; clang: `expected ')'`; "
           "MSVC: C2143 — all three refuse it, and DSS reports "
           "P_UnexpectedToken: expected 'ParenClose' — got ','";
}

TEST(BareEllipsisParameterList, AParameterAfterANonBareMarkerIsStillRefused) {
    auto model = analyzeShipped("c", {"int dss_bad(int a, ..., int b);\n"});
    EXPECT_EQ(misplacedMarkerCount(model), 1u)
        << "P66's sibling constraint (checkVariadicMarkerTerminatesParamList) "
           "must not be weakened by the bare-list branch";
}

TEST(BareEllipsisParameterList, TheFunctionPointerSpellingOfBothRefusalsHolds) {
    EXPECT_TRUE(frontEndRefuses("int (*dss_bad)(..., int b);\n"));

    auto named = analyzeShipped("c", {"int (*dss_bad)(int a, ..., int b);\n"});
    EXPECT_EQ(misplacedMarkerCount(named), 1u)
        << "the abstract/function-pointer path reaches neither named FnSig "
           "resolver — the arm a per-resolver check would have missed";
}

TEST(BareEllipsisParameterList, TwoBareMarkersAreRefused) {
    EXPECT_TRUE(frontEndRefuses("int dss_bad(..., ...);\n"))
        << "gcc and clang both refuse a second marker; the bare branch ends "
           "the list, so the separator has nowhere to go";
}

// The DISCRIMINATING control for the three refusal arms above: `frontEndRefuses`
// must be capable of answering FALSE, or they pass by measuring nothing.
TEST(BareEllipsisParameterList, TheRefusalInstrumentIsDiscriminating) {
    EXPECT_FALSE(frontEndRefuses("int dss_ok(int a, ...);\n"));
    EXPECT_FALSE(frontEndRefuses("int dss_ok2(...);\n"));
}
