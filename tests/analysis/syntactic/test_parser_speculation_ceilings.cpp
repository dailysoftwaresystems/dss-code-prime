// ── THE PARSER'S SPECULATION CEILINGS ───────────────────────────────────────
//
// D-PARSE-NINE-NESTED-CASTS-ARE-REFUSED-BY-THE-SPECULATION-CAP-WITH-A-FABRICATED-SYNTAX-ERROR.
//
// A nested `(int)` cast chain is the shape that walks EVERY ceiling the parser
// puts on speculation, because c's `operand` alt speculates once per cast
// (`castExpr` vs `parenExpr`). Before this file existed there were FOUR of them
// stacked on that one construct, three of them hardcoded, and all four reported
// the SAME fabricated syntax error against the user's own `int`:
//
//   1. `ParserConfig::maxSpeculationDepth`   was 8, hardcoded  → refused at 9
//   2. `BuilderConfig::maxSpeculationDepth`  was 64, hardcoded → would refuse at 65
//   3. the probe token budget, `16 x lookahead`, hardcoded     → refused at 342
//   4. `parser.maxExpressionDepth` = 1024 (already config-driven, but its
//      `P_ExpressionTooDeep` was ERASED when it fired inside a probe)
//                                                              → refused at 1024
//
// ✔MEASURED through the real CLI, in that order, each one uncovered only by
// lifting the one before it. Every one of them surfaced as
// `error[P_NoAlternativeMatched]: expected 'Identifier', … — got 'int'`, i.e.
// the compiler telling the author their correct `int` was wrong. gcc 13.3.0,
// mingw-w64 gcc 13.2.0, clang 18.1.3 and MSVC 19.51 all accept 8, 9, 16, 64,
// 256, 512 and 1024 nested casts, so DSS was far below the union — and the
// diagnostic actively misled.
//
// WHAT THIS FILE PINS
//   (A) all three knobs are CONFIG-DRIVEN and reach the schema;
//   (B) the LIFT — 9/64/65/256 casts parse clean at the shipped values;
//   (C) each ceiling RE-IMPOSED still fails loud, by its OWN name, with NO
//       fabricated `P_NoAlternativeMatched` — one arm per ceiling so no arm
//       can cover for another;
//   (D) the shipped ceiling sits BELOW the measured host-stack floor on an
//       ORDINARY ~1 MiB thread, so the ceiling — not a crash — is what a
//       deep-but-legal program meets.
//
// ★★ ORDINARY THREAD, ALWAYS. `src/program/program.cpp` builds every CU inside
// `substrate::callOnLargeStack(64 MiB)`, so a ceiling measured through the CLI
// reports a floor 64x higher than a library embedder or the LSP actually has.
// Nothing here calls `callOnLargeStack`: every parse below runs on the gtest
// thread, which is the ordinary one.
//
// THE PROBE (idle unless asked). A ceiling has to be MEASURED per site rather
// than argued, and a host-stack floor can only be found by walking into it —
// which kills the process, so it is walked from OUTSIDE, one run per depth:
//   DSS_NC_PROBE_CASTS=4000 DSS_NC_PROBE_SPEC=100000 \
//   DSS_NC_PROBE_BUDGET=100000 DSS_NC_PROBE_EXPR=200000 \
//   ctest --test-dir build/nc -R analysis/syntactic/test_parser_speculation_ceilings -V
// With `DSS_NC_PROBE_CASTS` unset the case is a skip, so the ordinary gate is
// unaffected. `PROBE-MARK` is flushed at each stage because a stack overflow
// terminates the process without running atexit — the LAST mark on stdout names
// what survived, so the death belongs to the next thing.

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include "../../core/bounded_stack.hpp"   // runOnBoundedStack — the BOUND (see there)

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace dss;

namespace {

// `int main(void){ int x=0; return (int)(int)…x; }` — `x` is a RUNTIME
// variable on purpose: a constant operand would let the front end fold the
// chain and the instrument would measure nothing while reporting green.
[[nodiscard]] std::string castChain(std::size_t casts) {
    std::string s = "int main(void){ int x=0; return ";
    s.reserve(casts * 5 + 48);
    for (std::size_t i = 0; i < casts; ++i) s += "(int)";
    s += "x; }";
    return s;
}

// `int main(void){ int x=0; return (int)(x+1+1+…+1); }` — the OTHER axis the
// speculation machinery is bounded on: a construct that is TOKEN-LONG but
// nesting-SHALLOW. A left-associative chain climbs iteratively (it never
// deepens the expression work-stack) and the whole chain sits inside ONE
// speculative probe, so this shape reaches the per-probe token budget without
// going anywhere near the depth cap. `x` is a runtime variable — nothing folds.
[[nodiscard]] std::string longExprInCast(std::size_t terms) {
    std::string s = "int main(void){ int x=0; return (int)(x";
    s.reserve(terms * 2 + 64);
    for (std::size_t i = 0; i < terms; ++i) s += "+1";
    s += "); }";
    return s;
}

// Parse against an ALREADY-LOADED shipped c schema on the CALLING thread — the
// split that lets the bounded-stack pins below load the schema on the main
// thread (a large, input-independent cost) and run only the parse on the
// small one.
[[nodiscard]] Tree parseCWith(std::shared_ptr<GrammarSchema const> schema,
                              std::string source, ParserConfig cfg) {
    auto src = SourceBuffer::fromString(std::move(source), "<spec-ceilings>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             DiagnosticBudget::libraryDefault(), std::move(cfg),
             std::move(lexDiags)};
    return std::move(std::move(p).parse().tree);
}

// Parse against the real shipped c schema ON THE CALLING (ordinary) THREAD.
[[nodiscard]] Tree parseC(std::string source, ParserConfig cfg) {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value());
    return parseCWith(*loaded, std::move(source), std::move(cfg));
}

// The SHIPPED config, read from the language rather than restated here — the
// point of the change is that these numbers live in `c.lang.json`.
[[nodiscard]] ParserConfig shippedCConfig() {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value());
    ParserConfig cfg;
    if (auto v = (*loaded)->maxExpressionDepth())      cfg.maxExpressionDepth = *v;
    if (auto v = (*loaded)->maxSpeculationDepth())     cfg.maxSpeculationDepth = *v;
    if (auto v = (*loaded)->speculationBudgetFactor()) cfg.speculationBudgetFactor = *v;
    return cfg;
}

[[nodiscard]] std::size_t countCode(Tree const& t, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == code) ++n;
    }
    return n;
}

[[nodiscard]] std::string firstMessageFor(Tree const& t, DiagnosticCode code) {
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == code) return d.actual;
    }
    return {};
}

[[nodiscard]] std::string allCodes(Tree const& t) {
    std::string s;
    for (auto const& d : t.diagnostics().all()) {
        if (!s.empty()) s += ' ';
        s += diagnosticCodeName(d.code);
    }
    return s;
}

[[nodiscard]] std::string envOr(char const* name, std::string fallback) {
    char const* v = std::getenv(name);
    return (v == nullptr || *v == '\0') ? std::move(fallback) : std::string{v};
}

void mark(std::string const& what) {
    std::fprintf(stdout, "PROBE-MARK %s\n", what.c_str());
    std::fflush(stdout);
}

} // namespace

// ── THE PROBE ───────────────────────────────────────────────────────────────
// Idle unless `DSS_NC_PROBE_CASTS` is set. See the file header.
TEST(ParserSpeculationCeilings, ProbeWalksTheConfiguredCastDepth) {
    std::string const casts = envOr("DSS_NC_PROBE_CASTS", "");
    if (casts.empty()) {
        GTEST_SKIP() << "DSS_NC_PROBE_CASTS unset — probe idle";
    }
    const auto n = static_cast<std::size_t>(std::atoll(casts.c_str()));

    ParserConfig cfg = shippedCConfig();
    if (auto v = envOr("DSS_NC_PROBE_SPEC", ""); !v.empty()) {
        cfg.maxSpeculationDepth = static_cast<std::size_t>(std::atoll(v.c_str()));
    }
    if (auto v = envOr("DSS_NC_PROBE_BUDGET", ""); !v.empty()) {
        cfg.speculationBudgetFactor =
            static_cast<std::size_t>(std::atoll(v.c_str()));
    }
    if (auto v = envOr("DSS_NC_PROBE_EXPR", ""); !v.empty()) {
        cfg.maxExpressionDepth = static_cast<std::size_t>(std::atoll(v.c_str()));
    }
    // `DSS_NC_PROBE_SHAPE=longexpr` swaps the cast chain for the token-long,
    // nesting-shallow shape, which is the only way to reach the per-probe
    // token budget once the depth ceiling is the tighter of the two.
    const bool longExpr = envOr("DSS_NC_PROBE_SHAPE", "castchain") == "longexpr";
    mark("BEGIN n=" + std::to_string(n)
         + " shape=" + (longExpr ? "longexpr" : "castchain")
         + " spec=" + std::to_string(cfg.maxSpeculationDepth)
         + " budget=" + std::to_string(cfg.speculationBudgetFactor)
         + " expr=" + std::to_string(cfg.maxExpressionDepth));

    Tree t = parseC(longExpr ? longExprInCast(n) : castChain(n),
                    std::move(cfg));
    mark("PARSE errors=" + std::to_string(t.diagnostics().hasErrors() ? 1 : 0)
         + " codes=[" + allCodes(t) + "]");
}

// ── (A) ALL THREE KNOBS ARE CONFIG-DRIVEN ───────────────────────────────────
// If any of these reads `nullopt` the CU falls back to the C++ default and the
// "100% config driven" requirement is defeated silently — which is exactly the
// state two of them were in.
TEST(ParserSpeculationCeilings, EveryCeilingIsConfigDriven) {
    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());

    auto spec = (*loaded)->maxSpeculationDepth();
    ASSERT_TRUE(spec.has_value())
        << "c `parser.maxSpeculationDepth` must reach the schema";
    // 2048 since P60 (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED):
    // the drive is heap-driven, so the value is bounded by the MEMORY a live
    // probe's checkpoint costs (quadratic in depth — ✔MEASURED 332 MiB at 2048,
    // 1.2 GiB at 4096), not by a host stack. The `$parserComment` in
    // `c.lang.json` carries the whole derivation.
    EXPECT_EQ(*spec, 2048u) << "shipped c speculation-depth ceiling";
    EXPECT_GT(*spec, 63u)
        << "ISO C23 5.2.4.1 requires 63 nesting levels of parenthesised "
           "expressions — the FLOOR, not the target";
    EXPECT_GT(*spec, 64u)
        << "must exceed the builder's old hardcoded 64, or that second cap "
           "would still be the binding one";

    auto factor = (*loaded)->speculationBudgetFactor();
    ASSERT_TRUE(factor.has_value())
        << "c `parser.speculationBudgetFactor` must reach the schema";
    // 128 since P60: the outermost probe of a cast chain holds the WHOLE chain
    // (3 tokens per cast), so at the old 64 x 64 = 4096 tokens the budget would
    // have silently taken over from the depth ceiling at ~1365 casts.
    EXPECT_EQ(*factor, 128u) << "shipped c speculative token-budget factor";
    EXPECT_GT(*factor, 16u) << "the lift must raise it above the old hardcoded 16";
    EXPECT_GE(*factor * 64u, 3u * *spec + 1u)
        << "the operand alt's budget (factor x its lookahead of 64) must hold a "
           "cast chain at the depth ceiling, or the budget — not the depth — is "
           "what a deep cast chain meets";

    auto expr = (*loaded)->maxExpressionDepth();
    ASSERT_TRUE(expr.has_value())
        << "c `parser.maxExpressionDepth` must reach the schema";
    EXPECT_EQ(*expr, 16384u)
        << "shipped c expression-depth ceiling (gcc's measured working paren "
           "depth; pinned exactly in test_parser_c_smoke too)";
    EXPECT_GT(*expr, *spec)
        << "a cast costs one expression level AND one probe, so the "
           "speculation ceiling must be the lower of the two or a deep cast "
           "chain would be refused as an expression-depth overflow";
}

// ── (B) THE LIFT ────────────────────────────────────────────────────────────
// The exact depths the row and the reference matrix name. 9 is the one the row
// is titled after; 65 is the builder's old cap plus one; 256 is well past both.
TEST(ParserSpeculationCeilings, DeepCastChainsParseCleanAtTheShippedCeilings) {
    for (std::size_t n : {9u, 16u, 64u, 65u, 128u, 256u}) {
        Tree t = parseC(castChain(n), shippedCConfig());
        EXPECT_FALSE(t.diagnostics().hasErrors())
            << n << " nested `(int)` casts is legal C that gcc, mingw-w64 gcc, "
                    "clang and MSVC all compile; codes=[" << allCodes(t) << "]";
        EXPECT_EQ(countCode(t, DiagnosticCode::P_NoAlternativeMatched), 0u)
            << "no fabricated syntax error at " << n << " casts";
    }
}

// ── (C1) RED-ON-DISABLE: the SPECULATION-DEPTH ceiling ──────────────────────
// The same input, only `maxSpeculationDepth` re-imposed at the old 8. The
// ceiling MUST still fire — and must name ITSELF, not the user's `int`.
TEST(ParserSpeculationCeilings, DepthCeilingReimposedAtEightStillFailsLoud) {
    ParserConfig cfg = shippedCConfig();
    cfg.maxSpeculationDepth = 8;
    Tree t = parseC(castChain(9), std::move(cfg));

    ASSERT_NE(t.root(), InvalidNode) << "must RECOVER, never abort";
    EXPECT_TRUE(t.diagnostics().hasErrors());
    ASSERT_GE(countCode(t, DiagnosticCode::P_MaxSpeculationDepth), 1u)
        << "with the cap back at 8 the ceiling must fail loud BY NAME; "
           "codes=[" << allCodes(t) << "]";
    EXPECT_EQ(countCode(t, DiagnosticCode::P_NoAlternativeMatched), 0u)
        << "the refusal must NOT be dressed up as a syntax error against the "
           "user's own token — that misattribution is the defect; codes=["
        << allCodes(t) << "]";

    // The message names the limit, its value and the config key that sets it.
    const std::string msg =
        firstMessageFor(t, DiagnosticCode::P_MaxSpeculationDepth);
    EXPECT_NE(msg.find("speculation-depth limit"), std::string::npos) << msg;
    EXPECT_NE(msg.find(" 8"), std::string::npos) << msg;
    EXPECT_NE(msg.find("parser.maxSpeculationDepth"), std::string::npos) << msg;
}

// ── (C2) RED-ON-DISABLE: the BUILDER's checkpoint cap is DERIVED ────────────
// `Parser::parse` sets `BuilderConfig::maxSpeculationDepth` from the parser's
// own value. If that derivation is dropped the builder falls back to its 64 and
// becomes the binding ceiling at 65 casts — a second cap invisible until
// someone writes one more level. 65 casts parsing clean is the proof it is
// gone; the ARM that keeps it honest is (C1) above, which shows the parser's
// counter still fires when it should. Deliberately a SEPARATE case from (B) so
// a regression here reads as "the builder cap came back", not as "deep chains
// broke".
TEST(ParserSpeculationCeilings, BuilderCheckpointCapNoLongerBindsAtSixtyFive) {
    Tree t = parseC(castChain(65), shippedCConfig());
    EXPECT_EQ(countCode(t, DiagnosticCode::P_MaxSpeculationDepth), 0u)
        << "65 casts opens 65 builder checkpoints; the builder's own cap must "
           "be derived from the (larger) parser cap, not left at 64; codes=["
        << allCodes(t) << "]";
    EXPECT_FALSE(t.diagnostics().hasErrors()) << allCodes(t);
}

// ── (C3) RED-ON-DISABLE: the per-probe TOKEN BUDGET ─────────────────────────
// The third ceiling, and the one that was completely silent. With the factor
// back at the old hardcoded 16, c `operand`'s lookahead of 64 gives a 1024-token
// budget: `(int)` is 3 tokens, so 341 casts (1023) is the last that fits and 342
// is refused. It must be refused BY NAME.
// ISOLATION: the depth ceiling is lifted to 500 for this case ONLY, so the
// budget is provably the ceiling under test and not a second name for the
// depth cap. 500 stays well under the MEASURED 641-cast ordinary-thread stack
// floor, so lifting it here cannot turn a red into a crash.
TEST(ParserSpeculationCeilings, TokenBudgetReimposedAtSixteenStillFailsLoud) {
    ParserConfig cfg = shippedCConfig();
    cfg.speculationBudgetFactor = 16;
    cfg.maxSpeculationDepth     = 500;
    Tree t = parseC(castChain(342), std::move(cfg));

    ASSERT_NE(t.root(), InvalidNode) << "must RECOVER, never abort";
    EXPECT_TRUE(t.diagnostics().hasErrors());
    ASSERT_GE(countCode(t, DiagnosticCode::P_SpeculationBudgetExhausted), 1u)
        << "342 casts = 1026 tokens exceeds 64 x 16; the budget must fail loud "
           "BY NAME rather than silently failing the probe; codes=["
        << allCodes(t) << "]";
    EXPECT_EQ(countCode(t, DiagnosticCode::P_NoAlternativeMatched), 0u)
        << "codes=[" << allCodes(t) << "]";

    const std::string msg =
        firstMessageFor(t, DiagnosticCode::P_SpeculationBudgetExhausted);
    EXPECT_NE(msg.find("per-alternative token budget"), std::string::npos) << msg;
    EXPECT_NE(msg.find("parser.speculationBudgetFactor"), std::string::npos)
        << msg;
}

// ── (C4) RED-ON-DISABLE: the EXPRESSION-depth ceiling, hit INSIDE a probe ───
// The fourth ceiling was already config-driven and already had the right
// diagnostic — but firing inside a speculative probe meant the probe's rollback
// ERASED it and the alt's fallback replay fabricated a syntax error instead. So
// the honest ceiling was swapped for a dishonest one purely by WHERE it fired.
TEST(ParserSpeculationCeilings, ExpressionCeilingSurvivesSpeculationRollback) {
    ParserConfig cfg = shippedCConfig();
    cfg.maxExpressionDepth = 40;
    Tree t = parseC(castChain(60), std::move(cfg));

    ASSERT_NE(t.root(), InvalidNode) << "must RECOVER, never abort";
    EXPECT_TRUE(t.diagnostics().hasErrors());
    EXPECT_GE(countCode(t, DiagnosticCode::P_ExpressionTooDeep), 1u)
        << "the expression ceiling must reach the operator even when it was "
           "reached inside a speculative probe; codes=[" << allCodes(t) << "]";
    EXPECT_EQ(countCode(t, DiagnosticCode::P_NoAlternativeMatched), 0u)
        << "codes=[" << allCodes(t) << "]";
}

// ── (B2) THE LIFT, ON THE OTHER AXIS: TOKEN-LONG, NESTING-SHALLOW ───────────
// The depth ceiling is the tighter of the two for a CAST CHAIN, so the shipped
// `speculationBudgetFactor` can only be observed on a construct that is long in
// TOKENS without being deep in NESTING — which is the shape a real corpus
// actually has (the sqlite amalgamation nests 18 deep but carries a single
// 1765-element initializer in fts5.c).
//
// ✔MEASURED with `DSS_NC_PROBE_SHAPE=longexpr`, `(int)(x+1+1+…)`:
//   factor 16 (the old hardcoded value, 64 x 16 = 1024 tokens) — 505 terms
//     parse, 510 are refused;
//   factor 64 (shipped, 4096 tokens)                            — 2040 terms
//     parse, 2050 are refused.
// 1000 sits between the two, so this case is GREEN only because the factor is
// read from config: re-hardcode 16 and it goes red.
TEST(ParserSpeculationCeilings, LongExpressionInAProbeFitsTheShippedBudget) {
    Tree t = parseC(longExprInCast(1000), shippedCConfig());
    EXPECT_EQ(countCode(t, DiagnosticCode::P_SpeculationBudgetExhausted), 0u)
        << "a 1000-term chain inside one speculative probe is ~2000 tokens — "
           "under the shipped budget and over the old hardcoded one; codes=["
        << allCodes(t) << "]";
    EXPECT_FALSE(t.diagnostics().hasErrors()) << allCodes(t);
}

// ── (C5) THE CASCADE IS ONE DIAGNOSTIC, NOT N ───────────────────────────────
// Reporting the ceiling honestly is only half of it: the alt's ordinary
// recovery then walks back out through every level it speculated into, and
// every step of that walk lands on a token the author wrote correctly.
// ✔MEASURED against the pre-change binary on `(int)` x9 at cap 8: NINETEEN
// diagnostics — 1 `P_NoAlternativeMatched`, 16 `P_UnexpectedToken`,
// 1 `P_MissingRequiredChild`, 1 `P_BuilderInvariant` — none of which named a
// limit. The count must not grow with the depth either: a cascade proportional
// to the input is how one refusal becomes a screenful.
TEST(ParserSpeculationCeilings, CeilingReportsOnceAndDoesNotCascade) {
    std::size_t previousParserDiagnostics = 0;
    for (std::size_t n : {9u, 40u, 321u}) {
        ParserConfig cfg = shippedCConfig();
        cfg.maxSpeculationDepth = 8;
        Tree t = parseC(castChain(n), std::move(cfg));

        EXPECT_EQ(countCode(t, DiagnosticCode::P_MaxSpeculationDepth), 1u)
            << "exactly one ceiling report at " << n << " casts; codes=["
            << allCodes(t) << "]";
        EXPECT_EQ(countCode(t, DiagnosticCode::P_UnexpectedToken), 0u)
            << "codes=[" << allCodes(t) << "]";
        EXPECT_EQ(countCode(t, DiagnosticCode::P_BacktrackFailed), 0u)
            << "codes=[" << allCodes(t) << "]";

        // Count only the PARSER's own recovery chatter. `P_BuilderInvariant`
        // ("scope stack non-empty at finish") is deliberately EXCLUDED and
        // deliberately not silenced: it is the builder's internal-invariant
        // signal, it is PRE-EXISTING on this input (present in the pre-change
        // baseline above), and shielding an instrument because it is
        // inconvenient is how a gate stops meaning anything.
        std::size_t parserChatter = 0;
        for (auto const& d : t.diagnostics().all()) {
            if (d.code != DiagnosticCode::P_BuilderInvariant) ++parserChatter;
        }
        EXPECT_LE(parserChatter, 2u)
            << "one honest ceiling report, not a cascade; codes=["
            << allCodes(t) << "]";
        if (previousParserDiagnostics != 0) {
            EXPECT_EQ(parserChatter, previousParserDiagnostics)
                << "the diagnostic count must NOT grow with the nesting depth "
                   "— a cascade proportional to the input is the defect; n="
                << n << " codes=[" << allCodes(t) << "]";
        }
        previousParserDiagnostics = parserChatter;
    }
}

// ── (C6) THE SHIELD IS ONE REGION WIDE ──────────────────────────────────────
// The cascade shield silences the parser until the next schema sync token. If
// it ever outlived the statement it was raised in, it would swallow real
// errors in the rest of the file — a mechanism whose job is to say less
// failing toward saying nothing. A genuine syntax error in a LATER statement
// must still be reported in full.
TEST(ParserSpeculationCeilings, ShieldDoesNotSwallowALaterStatementsError) {
    ParserConfig cfg = shippedCConfig();
    cfg.maxSpeculationDepth = 8;
    // Statement 1 trips the ceiling (the SAME `return`-expression shape every
    // other case here uses, so the ceiling provably fires); statement 2 is
    // genuinely malformed and belongs to the author.
    std::string src = "int main(void){ int x=0; x = ";
    for (int i = 0; i < 9; ++i) src += "(int)";
    src += "x; int z = ; return z; }";
    Tree t = parseC(std::move(src), std::move(cfg));

    EXPECT_GE(countCode(t, DiagnosticCode::P_MaxSpeculationDepth), 1u)
        << "codes=[" << allCodes(t) << "]";
    // `int z = ;` is a real error and must survive the shield raised by the
    // PREVIOUS statement. Any parser-tier error beyond the ceiling report is
    // the proof the shield came down at the `;`.
    std::size_t beyondTheCeiling = 0;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code != DiagnosticCode::P_MaxSpeculationDepth
            && d.code != DiagnosticCode::P_BuilderInvariant) {
            ++beyondTheCeiling;
        }
    }
    EXPECT_GE(beyondTheCeiling, 1u)
        << "the malformed LATER statement must still be diagnosed — the "
           "shield is one region wide, not one file wide; codes=["
        << allCodes(t) << "]";
}

// ── (D) THE CEILING SITS BELOW THE CRASH FLOOR — ON A BOUNDED STACK ─────────
//
// The whole point of a fail-loud counter is that it trips BEFORE the host stack
// does. This case runs a chain one level past the shipped ceiling on a thread
// whose stack is DELIBERATELY SMALL (`tests/core/bounded_stack.hpp`, 256 KiB —
// a quarter of the ordinary thread and a fraction of what any leg's host hands
// out): reaching the assertions at all is the proof that the ceiling — not a
// stack overflow — is what a too-deep program meets, and that the proof holds
// on EVERY toolchain, not on the one the gate happens to build with.
//
// ★★★ WHY THE BOUND, AND NOT THE ORDINARY THREAD THIS CASE USED TO RUN ON.
// ✔MEASURED 2026-09-04 (P60, lane `rc`): with the speculation drive still host
// recursion, this exact case was GREEN on the Ninja + mingw-w64 g++ gate and
// SEGFAULTED under MSVC 19.51 Debug — gdb-attributed to an 8-frame cycle per
// cast (`trySpeculativeBranch → stepOnce → walkExpression → … → stepOnce`) at
// 1072 bytes a frame, 8.5 KiB per level, so the 1 MiB thread died at ~120
// casts where `c.lang.json` promised a loud refusal at 320. On the ordinary
// thread a pin that COMPLETES measures the margin of one toolchain's frames;
// on 256 KiB, a recursion of even 64 bytes per level is dead by 4096 levels on
// every toolchain, while the converted drive (one heap `SpeculationSite` per
// probe, one heap `ExprFrame` per expression level) costs O(1) host stack per
// level and completes. Restore the recursion and this case does not fail, the
// process dies — on the Ninja gate too.
//
// The schema is loaded on the MAIN thread on purpose: the loader's cost is
// large and input-INDEPENDENT (see `bounded_stack.hpp`), and it is not what
// this case bounds.
TEST(ParserSpeculationCeilings, ShippedCeilingIsReachableOnABoundedStack) {
    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    const std::size_t ceiling = schema->maxSpeculationDepth().value_or(8);
    ParserConfig cfg = shippedCConfig();

    std::optional<Tree> t;
    test::runOnBoundedStack([&] {
        t.emplace(parseCWith(schema, castChain(ceiling + 1), std::move(cfg)));
    });
    ASSERT_TRUE(t.has_value());
    ASSERT_NE(t->root(), InvalidNode);
    EXPECT_TRUE(t->diagnostics().hasErrors())
        << "one past the ceiling must be refused, not silently accepted";
    EXPECT_GE(countCode(*t, DiagnosticCode::P_MaxSpeculationDepth), 1u)
        << "and the refusal is the SPECULATION ceiling, by name — no other "
           "ceiling may be the one that binds first; codes=[" << allCodes(*t)
        << "]";
    EXPECT_EQ(countCode(*t, DiagnosticCode::P_NoAlternativeMatched), 0u)
        << "codes=[" << allCodes(*t) << "]";
}

// ── (E) THE WHOLE SHIPPED RANGE PARSES ON THE BOUNDED STACK ─────────────────
//
// (D) proves the counter trips before the stack does; these two prove the
// range BELOW the counter is real — a cast chain AT the shipped speculation
// ceiling and a paren nest one below the shipped expression ceiling both parse
// clean on 256 KiB. The discriminating arithmetic, per the ceilings measured
// with the recursion in the tree: a cast cost ~1.63 KiB per level on the
// THINNEST supported frames (mingw-w64 g++ Debug, 640 casts ≈ 1 MiB), so a
// regression needs ceiling × 1.63 KiB — over 6 MiB at 4096 — against a 256 KiB
// reserve; a paren cost ~800 bytes per level there (1318 levels ≈ 1 MiB), over
// 12 MiB at 16383. Either would die dozens of times over.
//
// ⚠ These depths are the SHIPPED numbers read off the schema, deliberately —
// the same values `c.lang.json`'s `$parserComment` justifies — so raising a
// ceiling past what the parser can carry on 256 KiB is caught HERE, by the
// process dying, rather than by an embedder.
TEST(ParserSpeculationCeilings, DeepCastChainAtTheShippedCeilingParsesOnABoundedStack) {
    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    const std::size_t ceiling = schema->maxSpeculationDepth().value_or(8);
    ParserConfig cfg = shippedCConfig();

    std::optional<Tree> t;
    test::runOnBoundedStack([&] {
        t.emplace(parseCWith(schema, castChain(ceiling), std::move(cfg)));
    });
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->diagnostics().hasErrors())
        << ceiling << " nested `(int)` casts is exactly the shipped ceiling and "
           "must parse clean; codes=[" << allCodes(*t) << "]";
}

TEST(ParserSpeculationCeilings, DeepParenNestBelowTheShippedCeilingParsesOnABoundedStack) {
    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    const std::size_t exprCeiling = schema->maxExpressionDepth().value_or(256);
    ASSERT_GT(exprCeiling, 1u);
    ParserConfig cfg = shippedCConfig();

    // `int main(void){ int x=0; return (((…x…))); }` — the paren / postfix-body
    // re-entry, one atom frame per `(`; `x` is a runtime variable so nothing
    // folds. One BELOW the expression ceiling: the outermost expression is a
    // level of its own.
    const std::size_t parens = exprCeiling - 1;
    std::string src = "int main(void){ int x=0; return ";
    src.append(parens, '(');
    src += "x";
    src.append(parens, ')');
    src += "; }";

    std::optional<Tree> t;
    test::runOnBoundedStack([&] {
        t.emplace(parseCWith(schema, std::move(src), std::move(cfg)));
    });
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->diagnostics().hasErrors())
        << parens << " nested parentheses sit one below the shipped expression "
           "ceiling and must parse clean; codes=[" << allCodes(*t) << "]";
}
