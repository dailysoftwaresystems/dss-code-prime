// ── A REFUTED SPECULATIVE BRANCH IS NOT PARSED ANY FURTHER ──────────────────
//
// D-PARSE-SPECULATION-REFUSAL-REPLAY-IS-QUADRATIC.
//
// A cast chain ONE level past `parser.maxSpeculationDepth` is refused loudly
// and by name — and the refusal used to cost Θ(D²). ✔MEASURED through the real
// CLI at the shipped `speculationBudgetFactor` 128 with `maxExpressionDepth`
// lifted: caps 256 / 512 / 1024 / 2048 refused in 1.75 / 6.07 / 23.78 /
// 94.81 s, successive ratios 3.48 / 3.91 / 3.99. The consequence was not
// cosmetic: it is the ONLY thing that stopped the ceiling reaching gcc's
// ✔measured working 65536, and a raise to 16384 attempted in cycle P61 had to
// be REVERTED because the gate spent 4083 s in the ceilings suite and failed.
//
// ★★★ THE ROW'S STATED MECHANISM WAS WRONG, AND THE COUNTERS SAY SO. The row
// blamed the alt's non-speculative fallback REPLAY re-parsing the chain at
// every level. ✔MEASURED by counting inside the parser (caps 64/128/256/512):
// the replay fires EXACTLY ONCE at every cap; speculation SITES opened
// 65/129/257/513 and probes 128/256/512/1024 — both LINEAR, so nothing is
// re-parsed and there is nothing for a memo to hit. What was quadratic is
// `P_BacktrackFailed` PANIC RECOVERIES: 2272 / 8640 / 33664 / 132864 —
// C(C+7)/2 exactly, at all four points. (It is NOT C(C+1)/2, which gives
// 2080 / 8256 / 32896 / 131328; the counts were measured, the closed form
// beside them was arithmetic written from memory, and it was WRONG.) Of the
// 134401 PANIC SCANS at cap 512, 133376 — 99.2 % — ran inside a still-live
// probe; the denominator of that percentage is SCANS, not the recovery count
// beside it, which 133376 already exceeds.
//
// The cause is a WINDOW, not a repetition. `driveParse_` abandons a probe that
// has emitted a diagnostic or desynced the walker — but it only ASKS once
// everything the branch's step SPAWNED (an expression walk, a nested site) has
// finished. So a branch already certain to be rolled back went on parsing,
// recovering and panic-scanning to the end of its rule, and every token of
// that was discarded. The fix asks the same question at the TOP of the driver
// loop, which makes the window O(1): `SpeculationProbe::isRefuted`.
//
// ★★★ AND CUTTING THAT WINDOW CHANGES WHAT THE PARSER SAYS ON ONE SHAPE. A
// ceiling latched by a branch the parser had ALREADY rejected does not survive
// the cut — correctly, since it is a fact about a discarded reading — but the
// cascade that latch was accidentally silencing then spoke. ✔MEASURED, real
// CLI, shipped `c.lang.json`, on `return (int)sizeof({x) + (int)…x` (a genuine
// unbalanced brace ahead of a cast chain): one positioned
// `P_MaxSpeculationDepth` became `P_NoAlternativeMatched` + 26
// `P_UnexpectedToken`, two more per cast added. ✔MEASURED that the vanished
// ceiling was itself a misattribution: at cap C the pre-cut rendition reported
// it from a chain of C casts, while a CLEAN chain of C casts parses fine and
// only C+1 trips it — one level was being held by the refuted branch. So the
// remedy is not to put the latch back; it is to stop the cascade, which
// `recoverAt` now does by raising the SAME cascade shield the ceiling raises
// whenever the immediately preceding dispatch step also recovered. Pinned by
// `ARefutedBranchDoesNotCascadeAfterTheOneRealError` below.
//
// WHAT THIS FILE PINS
//   (A) COST: the refusal's TOKEN WORK grows LINEARLY, not quadratically, with
//       the depth ceiling. Measured as a RATIO across two ceilings, never as
//       an absolute bound, and on `ParseResult::tokenAccessCount` — the
//       parser's own deterministic O(work) proxy — rather than on wall time,
//       so the pin cannot be flaky and cannot be read differently on a busy
//       host.
//   (B) LOUD: the refusal is still ONE positioned `P_MaxSpeculationDepth` that
//       names its own limit and its own config key, with no fabricated
//       `P_NoAlternativeMatched`. A faster wrong answer is not the goal, and a
//       refusal that stopped firing would satisfy (A) perfectly.
//   (C) IDENTITY: the same program produces the same TREE. Every case renders
//       the finished tree node-for-node (kind, flags, rule, token kind, span,
//       parent, child list) plus the whole diagnostic stream INCLUDING each
//       diagnostic's rendered text, and prints a hash of that rendering. The
//       corpus is chosen for speculation: every entry drives c's `operand`
//       alt (compoundLiteralExpr / castExpr / parenExpr all lead with `(`) or
//       its statement-level declaration-vs-expression alt, and each entry
//       declares whether it is valid and what the first error must be.
//
// ★ HOW (C) IS READ ACROSS THE RENDITIONS, AND AGAINST WHICH MUTANT. The
// change has no runtime switch — an "off" mode would be a second parser to
// keep honest — so the identity comparison is a RED-ON-DISABLE arm, and it is
// DIRECTIONAL:
//   · against the REFUTATION-CUT mutant (the cost fix removed, the cascade
//     shield left in place) every `IDENTITY <index> <hash>` line must be
//     BYTE-IDENTICAL while the cost cases go red. That is the claim: cutting a
//     refuted branch short changes WHEN the parser stops, never what a program
//     MEANS.
//   · against the CASCADE-SHIELD mutant the lines must DIFFER, on the
//     malformed cases, and that difference IS that arm's red. Asserting
//     identity against the whole base-commit rendition would assert both at
//     once and could only be satisfied by dropping one of the two fixes.
// A hash per case rather than a golden constant in this file, because a golden
// would red on every unrelated `c.lang.json` edit and teach the next reader to
// re-bless it. `DSS_SP_IDENTITY_DUMP` prints the full rendering when one moves
// and the hash alone will not say what changed.

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// `int main(void){ int x=0; return (int)(int)…x; }` — `x` is a RUNTIME
// variable so nothing folds and the chain is actually parsed.
[[nodiscard]] std::string castChain(std::size_t casts) {
    std::string s = "int main(void){ int x=0; return ";
    s.reserve(casts * 5 + 48);
    for (std::size_t i = 0; i < casts; ++i) s += "(int)";
    s += "x; }";
    return s;
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> cSchema() {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value()) << "the shipped c language document";
    return loaded.has_value() ? *loaded : nullptr;
}

// The SHIPPED ceilings, read off the schema rather than restated here.
[[nodiscard]] ParserConfig shippedCConfig(GrammarSchema const& schema) {
    ParserConfig cfg;
    if (auto v = schema.maxExpressionDepth())      cfg.maxExpressionDepth = *v;
    if (auto v = schema.maxSpeculationDepth())     cfg.maxSpeculationDepth = *v;
    if (auto v = schema.speculationBudgetFactor()) cfg.speculationBudgetFactor = *v;
    return cfg;
}

// Parse on the CALLING thread against an already-loaded schema, keeping the
// whole `ParseResult` — `tokenAccessCount` is what case (A) measures.
[[nodiscard]] ParseResult parseCFull(std::shared_ptr<GrammarSchema const> schema,
                                     std::string source, ParserConfig cfg) {
    auto src = SourceBuffer::fromString(std::move(source), "<refusal-cost>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             DiagnosticBudget::libraryDefault(), std::move(cfg),
             std::move(lexDiags)};
    return std::move(p).parse();
}

[[nodiscard]] std::size_t countCode(Tree const& t, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == code) ++n;
    }
    return n;
}

[[nodiscard]] std::string allCodes(Tree const& t) {
    std::string s;
    for (auto const& d : t.diagnostics().all()) {
        if (!s.empty()) s += ' ';
        s += diagnosticCodeName(d.code);
    }
    return s;
}

[[nodiscard]] std::string firstMessageFor(Tree const& t, DiagnosticCode code) {
    for (auto const& d : t.diagnostics().all()) {
        if (d.code == code) return d.actual;
    }
    return {};
}

// A flat, tree-id-independent rendering of one finished tree plus its whole
// diagnostic stream. Same shape as `tests/core/test_checkpoint_identity.cpp`'s
// — including `d.actual`, because a diagnostic whose code, position and place
// in the stream are all right and whose SENTENCE is wrong is exactly the leak
// a code-only rendering cannot see.
//
// ⚠ NodeIds carry the producing tree's arena tag, so only the id's INDEX is
// compared; the tagged id is never printed.
[[nodiscard]] std::string render(Tree const& t) {
    std::string out;
    out += "root=" + std::to_string(t.root().v)
         + " nodes=" + std::to_string(t.nodeCount()) + "\n";
    for (std::uint32_t i = 1;
         t.root().valid() && i < static_cast<std::uint32_t>(t.nodeCount()); ++i) {
        const NodeId id{i, t.root().arenaTag};
        out += "#" + std::to_string(i);
        out += " kind=" + std::to_string(static_cast<int>(t.kind(id)));
        out += " flags=" + std::to_string(
                   static_cast<std::uint32_t>(t.flags(id)));
        out += " span=" + std::to_string(
                   static_cast<std::uint64_t>(t.span(id).start()))
             + ":" + std::to_string(
                   static_cast<std::uint64_t>(t.span(id).end()));
        out += " parent=" + std::to_string(t.parent(id).v);
        if (t.kind(id) == NodeKind::Internal) {
            out += " rule=" + std::to_string(t.rule(id).v);
        } else if (t.kind(id) == NodeKind::Token) {
            out += " tok=" + std::to_string(t.tokenKind(id).v);
        }
        out += " kids=[";
        for (auto c : t.children(id)) out += std::to_string(c.v) + ",";
        out += "]\n";
    }
    out += "diags:\n";
    for (auto const& d : t.diagnostics().all()) {
        out += "  " + std::to_string(static_cast<int>(d.code))
             + "@" + std::to_string(static_cast<std::uint64_t>(d.span.start()))
             + " actual=" + d.actual + "\n";
    }
    return out;
}

// FNV-1a 64. A hash and not the rendering itself so the arm comparison is one
// short line per case; `DSS_SP_IDENTITY_DUMP` prints the rendering when a
// hash moves.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

struct Case {
    char const*    name;
    std::string    source;
    bool           valid;        // must parse with no errors at all
    DiagnosticCode firstError;   // meaningful only when !valid
    // ── PER-CASE CEILINGS, 0 = "leave the shipped value alone" ──
    //
    // ⚠⚠ WITHOUT THESE THE IDENTITY CORPUS IS BLIND TO ITS OWN SUBJECT. Every
    // case used to run at the SHIPPED ceilings and none nested past 256, so
    // not one of the 25 programs raised any of the three speculation ceilings
    // — and the ceiling LATCH is the single piece of parser state a probe's
    // rollback deliberately does NOT restore, i.e. exactly the state the
    // refutation cut can disturb. "All 25 renderings byte-identical" was a
    // true measurement of a region where the change does nothing, offered as
    // proof of the property most at risk
    // (D-PARSE-SPECULATION-REFUSAL-REPLAY-IS-QUADRATIC).
    // A SMALL ceiling rather than a 2049-cast chain at the shipped one: the
    // identity claim is that the two renditions agree under the SAME config,
    // and a case that trips in milliseconds can be run 25 times over.
    std::size_t    spec   = 0;   // maxSpeculationDepth
    std::size_t    expr   = 0;   // maxExpressionDepth
    std::size_t    factor = 0;   // speculationBudgetFactor
};

// Every entry drives speculation. c's `operand` alt has compoundLiteralExpr,
// castExpr and parenExpr all leading with `(`, and the predictive prune cannot
// separate them (a type-name is variable-width, so the fixed prefix stops at
// the `(`) — so each `(` here is a real probe, and the compound-literal
// candidate is tried FIRST and fails, which is precisely the "a branch emits
// and a later candidate wins" shape the fix cuts short.
[[nodiscard]] std::vector<Case> corpus() {
    std::vector<Case> cs;
    auto add = [&cs](char const* n, std::string s, bool ok,
                     DiagnosticCode e = DiagnosticCode::P_NoAlternativeMatched) {
        cs.push_back(Case{n, std::move(s), ok, e});
    };

    // ── valid: the alt must pick the right candidate every time ──
    add("cast-1",        castChain(1), true);
    add("cast-9",        castChain(9), true);
    add("cast-64",       castChain(64), true);
    add("cast-256",      castChain(256), true);
    add("paren-nest",    "int main(void){ int x=0; return ((((x)))); }", true);
    add("paren-vs-cast", "int main(void){ int x=0; return (x)+(x); }", true);
    add("compound-lit",  "struct P { int a; int b; };\n"
                         "int main(void){ struct P p = (struct P){1,2};"
                         " return p.a; }", true);
    add("compound-arr",  "int main(void){ int *q = (int[]){1,2,3};"
                         " return q[0]; }", true);
    add("cast-of-lit",   "int main(void){ return (int)(char)(long)7; }", true);
    add("cast-ptr",      "int main(void){ int x=0; int *p=&x;"
                         " return *(int *)(void *)p; }", true);
    add("sizeof-type",   "int main(void){ return (int)sizeof(int); }", true);
    add("sizeof-expr",   "int main(void){ int x=0; return (int)sizeof(x); }", true);
    add("call-vs-cast",  "int f(int a){ return a; }\n"
                         "int main(void){ int x=0; return f((int)(x)); }", true);
    add("decl-vs-expr",  "int main(void){ int a=1; int b=2; a * b; return a; }",
                         true);
    add("decl-ptr",      "typedef int T;\nint main(void){ T * p = 0;"
                         " return p == 0; }", true);
    add("stmt-expr",     "int main(void){ int x = ({ int y = 3; y + 1; });"
                         " return x; }", true);
    add("ternary",       "int main(void){ int x=0; return (x) ? (int)(x) : (x); }",
                         true);
    add("comma-in-paren","int main(void){ int x=0; return (x, (int)x); }", true);
    add("nested-call",   "int g(int a, int b){ return a+b; }\n"
                         "int main(void){ int x=0; return g((int)x, (x)); }", true);
    add("for-init-ambig","int main(void){ int s=0; for (int i=0;i<3;i++) s+=i;"
                         " return s; }", true);

    // ── invalid: the SAME shapes, broken, so recovery is compared too ──
    add("bad-in-paren",  "int main(void){ int x=0; return (x @ x); }",
                         false, DiagnosticCode::P_UnexpectedToken);
    // ⚠ `P_MissingRequiredChild`, not `P_UnexpectedToken`: the ternary's `:`
    // is a REQUIRED child the walker never reaches, and the code was READ off
    // the parser rather than assumed. Declaring the wrong one here would have
    // pinned a behaviour the parser does not have.
    add("missing-colon", "int main(void){ int x=0; return (x) ? x ; }",
                         false, DiagnosticCode::P_MissingRequiredChild);
    add("empty-init",    "int main(void){ int z = ; return z; }",
                         false, DiagnosticCode::P_NoAlternativeMatched);
    // ⚠ `P_NoAlternativeMatched`, and it USED to be `P_UnexpectedToken`. The
    // `P_UnexpectedToken` that followed was the second voice of the same
    // structural error, and `recoverAt`'s cascade shield now holds it. The
    // code below was READ off the parser after that change, not carried over.
    add("unclosed-cast", "int main(void){ int x=0; return (int x; }",
                         false, DiagnosticCode::P_NoAlternativeMatched);
    add("garbage-tail",  "int main(void){ int x=0; return (int)x @@@; }",
                         false, DiagnosticCode::P_UnexpectedToken);

    // ── THE THREE SPECULATION CEILINGS, EACH ACTUALLY RAISED ────────────────
    //
    // The row's own subject. Each of these trips a ceiling whose LATCH
    // survives every probe rollback, so the rendering below is the one that
    // can see a refutation cut change what a ceiling reports. Small ceilings,
    // named per case, so the whole corpus still runs in well under a second.
    auto addAt = [&cs](char const* n, std::string s, DiagnosticCode e,
                       std::size_t spec, std::size_t expr,
                       std::size_t factor = 0) {
        cs.push_back(Case{n, std::move(s), false, e, spec, expr, factor});
    };
    // DEPTH: a clean chain one past the ceiling — the shape the whole row is
    // about, and the one whose refusal must stay a single named report.
    addAt("ceiling-depth", castChain(9), DiagnosticCode::P_MaxSpeculationDepth,
          8, 1u << 20);
    // DEPTH, reached from deeper in the file, with valid code before and
    // after it: the latch must not leak across the statement boundary.
    addAt("ceiling-depth-mid-file",
          [] {
              std::string s = "int main(void){ int x=0; int a=(int)(int)x;\n"
                              " int b = ";
              for (int i = 0; i < 9; ++i) s += "(int)";
              s += "x;\n int c=(int)a; return a+b+c; }";
              return s;
          }(),
          DiagnosticCode::P_MaxSpeculationDepth, 8, 1u << 20);
    // EXPRESSION NESTING: the third ceiling, latched from INSIDE a probe
    // (`recoverExpressionTooDeep_`) — the one whose diagnostic does not
    // survive its own emission and must be re-reported from the latch.
    addAt("ceiling-expression",
          "int main(void){ int x=0; return " + std::string(24, '(') + "x"
          + std::string(24, ')') + "; }",
          DiagnosticCode::P_ExpressionTooDeep, 2048, 16);
    // TOKEN BUDGET: the ceiling that latches WITHOUT emitting at all.
    // ⚠ P65 (D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS)
    // — THIS CASE USED TO BE `return (x+1+1+…);` AND THAT PROGRAM IS NO LONGER
    // REFUSED, WHICH IS THE FIX RATHER THAN A HOLE. A bare parenthesised
    // expression makes `parenExpr` both the last SURVIVING candidate of c's
    // `operand` alt and its declared-last STRUCTURAL one, so
    // `finalCandidateDirectDescent_` descends into it with no budget — and it
    // always could have, because the all-fail path was going to REPLAY that
    // same rule without a budget one step later. The budget was refusing a
    // program gcc 13.3.0 and clang 18.1.3 both compile, and then parsing it
    // correctly on the replay while still exiting 1.
    // ★ THE CEILING IS STILL REACHABLE AND IS STILL PINNED HERE — a CAST
    // operand reaches it, because the prune drops `parenExpr` on `(int)…` so
    // the last survivor is `castExpr` while the fallback reading stays
    // `parenExpr`, and a candidate that is not the fallback reading keeps its
    // budget. ✔MEASURED through the shipped CLI at the SHIPPED
    // `speculationBudgetFactor` of 128, after the change:
    // `return (int)(x+1 x4100);` is error[P_SpeculationBudgetExhausted] rc=1
    // while `return (x+1 x4100);` is rc=0 — so the config key is still
    // falsifiable and the two shapes say which mechanism each one exercises.
    addAt("ceiling-budget",
          [] {
              std::string s = "int main(void){ int x=0; return (int)(x";
              for (int i = 0; i < 400; ++i) s += "+1";
              s += "); }";
              return s;
          }(),
          DiagnosticCode::P_SpeculationBudgetExhausted, 2048, 1u << 20, 1);
    return cs;
}

} // namespace

// ── (A) THE COST — LINEAR IN THE DEPTH CEILING ──────────────────────────────
//
// The witness the row names: the refusal-cost curve one past the cap must stop
// showing a ~4x step per doubling. Measured on the parser's own
// `tokenAccessCount` (deterministic, host-independent) across an 8x change of
// ceiling: LINEAR predicts ~8x, QUADRATIC ~64x. ✔MEASURED through the CLI on
// the same shape, before / after: cap 256 → 362270 / 28958 accesses and cap
// 512 → 1379614 / 57630, i.e. 3.81x per doubling before and 1.99x after.
//
// `maxExpressionDepth` is lifted for both arms because a cast chain is also
// that many expression levels deep, and this case is about the SPECULATION
// ceiling; the budget factor stays SHIPPED, since P61 made the budget a
// per-probe charge and it no longer binds on this shape.
TEST(ParserSpeculationRefusalCost, RefusalWorkIsLinearInTheDepthCeiling) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    constexpr std::size_t kShallow = 256;
    constexpr std::size_t kDeep    = 2048;   // 8x
    static_assert(kDeep == kShallow * 8, "the ratio bound below assumes 8x");

    auto refuseAt = [&](std::size_t ceiling) -> ParseResult {
        ParserConfig cfg = shippedCConfig(*schema);
        cfg.maxSpeculationDepth = ceiling;
        cfg.maxExpressionDepth  = 1u << 20;  // not the binding ceiling here
        return parseCFull(schema, castChain(ceiling + 1), std::move(cfg));
    };

    ParseResult shallow = refuseAt(kShallow);
    ParseResult deep    = refuseAt(kDeep);

    // SELF-CHECK. A measurement of zero would make the ratio meaningless while
    // everything below passed — the shape that made an earlier cost instrument
    // in this repository report clean forever.
    ASSERT_GT(shallow.tokenAccessCount, 0u);
    ASSERT_GT(deep.tokenAccessCount, 0u);

    // Both arms must actually have REFUSED. A cost pin whose arms stopped
    // refusing would be measuring the accepting path and would pass.
    ASSERT_EQ(countCode(shallow.tree, DiagnosticCode::P_MaxSpeculationDepth), 1u)
        << allCodes(shallow.tree);
    ASSERT_EQ(countCode(deep.tree, DiagnosticCode::P_MaxSpeculationDepth), 1u)
        << allCodes(deep.tree);

    const double ratio = static_cast<double>(deep.tokenAccessCount)
                       / static_cast<double>(shallow.tokenAccessCount);
    EXPECT_LT(ratio, 24.0)
        << "the refusal's token work must grow LINEARLY with the depth "
           "ceiling: an 8x ceiling costs ~8x linearly and ~64x quadratically, "
           "and 24 is three times the linear expectation. Measured "
        << deep.tokenAccessCount << " accesses at ceiling " << kDeep
        << " against " << shallow.tokenAccessCount << " at " << kShallow
        << " (ratio " << ratio << ")";
}

// ── REFUSING IS NOT ASYMPTOTICALLY WORSE THAN ACCEPTING ─────────────────────
//
// ⚠⚠ THIS CASE REPLACES ONE THAT WAS THE FIRST CASE WRITTEN TWICE. Its
// predecessor asserted `acc2048/2048 < 3 x acc256/256`, which is
// `acc2048/acc256 < 24` — the SAME inequality on the SAME two measurements as
// `RefusalWorkIsLinearInTheDepthCeiling`, since 3 x 2048/256 = 24. Its comment
// claimed to be "the one a single ratio cannot make"; it was the same ratio,
// rearranged, and the red-on-disable transcript that presented two independent
// reds was presenting ONE, counted twice
// (D-PARSE-SPECULATION-REFUSAL-REPLAY-IS-QUADRATIC).
//
// What a single across-ceilings ratio genuinely cannot say: that the refusal
// is cheap AT ALL. A rendition whose refusal cost 1000x the accepting parse
// would satisfy the linearity ratio perfectly, as long as it scaled. So this
// case measures the refusal against the ACCEPTING parse of the very same
// chain at the very same ceiling — a comparison the other case never forms,
// on an arm (the accepting parse) it never runs.
//
// ✔MEASURED, this test's own numbers, printed by the case itself so the bound
// is never a number nobody re-derives. The bound is deliberately generous:
// what it must separate is "a constant factor" from "a factor that grows with
// the ceiling", and the pre-fix rendition is two orders of magnitude out.
TEST(ParserSpeculationRefusalCost, RefusingCostsNoMoreThanAConstantTimesAccepting) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    constexpr std::size_t kCeiling = 2048;
    auto at = [&](std::size_t casts) -> ParseResult {
        ParserConfig cfg = shippedCConfig(*schema);
        cfg.maxSpeculationDepth = kCeiling;
        cfg.maxExpressionDepth  = 1u << 20;
        return parseCFull(schema, castChain(casts), std::move(cfg));
    };

    ParseResult accepted = at(kCeiling);        // AT the ceiling: parses clean
    ParseResult refused  = at(kCeiling + 1);    // one past it: refused by name

    // Both arms must be the thing they are named for, or the ratio is
    // measuring two copies of the same parse.
    ASSERT_FALSE(accepted.tree.diagnostics().hasErrors())
        << "the accepting arm must ACCEPT; codes=[" << allCodes(accepted.tree)
        << "]";
    ASSERT_EQ(countCode(refused.tree, DiagnosticCode::P_MaxSpeculationDepth),
              1u)
        << "the refusing arm must REFUSE by name; codes=["
        << allCodes(refused.tree) << "]";
    ASSERT_GT(accepted.tokenAccessCount, 0u);

    const double ratio = static_cast<double>(refused.tokenAccessCount)
                       / static_cast<double>(accepted.tokenAccessCount);
    std::fprintf(stdout,
                 "REFUSE/ACCEPT at ceiling %zu: refuse=%zu accept=%zu "
                 "ratio=%.3f\n",
                 kCeiling, refused.tokenAccessCount,
                 accepted.tokenAccessCount, ratio);
    std::fflush(stdout);
    EXPECT_LT(ratio, 12.0)
        << "refusing a chain one past the ceiling must cost a CONSTANT "
           "multiple of accepting the chain AT the ceiling. When the refusal "
           "was quadratic this factor grew with the ceiling, because every "
           "refuted branch recovered its way through the rest of the chain "
           "while the accepting parse stayed linear. Measured "
        << refused.tokenAccessCount << " accesses to refuse " << kCeiling + 1
        << " casts against " << accepted.tokenAccessCount << " to accept "
        << kCeiling << " (ratio " << ratio << ")";
}

// ── THE REFUTATION CUT MUST NOT TURN ONE ERROR INTO A CASCADE ───────────────
//
// The regression this case exists for was found by REVIEW, not by the lane
// that wrote the fix, and it is the one shape in 468 differential-fuzz runs
// where the fix made the output WORSE
// (D-PARSE-SPECULATION-REFUSAL-REPLAY-IS-QUADRATIC).
//
// A genuine syntax error (`sizeof({x)` — an unbalanced brace) sitting ahead of
// a cast chain. Before the cut, the refuted branch went on parsing, reached
// the depth ceiling somewhere inside that chain and latched it; the ceiling
// report then raised the cascade shield and the region yielded ONE diagnostic.
// After the cut the branch is abandoned at the brace, no ceiling is reached,
// and the shield was never raised — so the statement-level recovery spoke once
// per token, TWO more diagnostics for every cast added.
//
// ★ AND THE LOST CEILING WAS NOT WORTH PUTTING BACK. ✔MEASURED at cap 8: the
// pre-cut rendition reported `P_MaxSpeculationDepth` on this shape from a
// chain of EIGHT casts, while a CLEAN chain of eight casts parses rc 0 and it
// takes NINE to trip the ceiling — the extra level was being held by the
// branch the parser had already rejected, which is the misattribution the
// latch exists to prevent. So the fix is on the cascade, not on the latch:
// `recoverAt` raises the same shield whenever the immediately preceding
// dispatch step also recovered.
//
// THE PIN IS THE SLOPE, NOT A COUNT. The diagnostic count must not grow with
// the chain — a count pinned as an absolute would need re-blessing whenever
// anything upstream changed a message, and would say nothing about cascading.
TEST(ParserSpeculationRefusalCost, ARefutedBranchDoesNotCascadeAfterTheOneRealError) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    auto shape = [](std::size_t casts) {
        std::string s = "int main(void){ int x=0; return (int)sizeof({x) + ";
        for (std::size_t i = 0; i < casts; ++i) s += "(int)";
        s += "x; }";
        return s;
    };
    auto run = [&](std::size_t casts) -> ParseResult {
        ParserConfig cfg = shippedCConfig(*schema);
        cfg.maxSpeculationDepth = 8;
        cfg.maxExpressionDepth  = 1u << 20;
        return parseCFull(schema, shape(casts), std::move(cfg));
    };

    // Three chain lengths spanning the ceiling: below it, at it, well past it.
    ParseResult below = run(4);
    ParseResult at    = run(8);
    ParseResult past  = run(24);

    for (auto const* p : {&below, &at, &past}) {
        ASSERT_NE(p->tree.root(), InvalidNode) << "must RECOVER, never abort";
        EXPECT_TRUE(p->tree.diagnostics().hasErrors())
            << "the unbalanced brace is a REAL error and must be reported";
    }

    const std::size_t nBelow = below.tree.diagnostics().all().size();
    const std::size_t nAt    = at.tree.diagnostics().all().size();
    const std::size_t nPast  = past.tree.diagnostics().all().size();
    EXPECT_EQ(nBelow, nAt)
        << "the diagnostic count must not grow with the cast chain — 4 casts "
           "gave " << nBelow << " and 8 gave " << nAt << "; codes=["
        << allCodes(at.tree) << "]";
    EXPECT_EQ(nAt, nPast)
        << "the diagnostic count must not grow with the cast chain — 8 casts "
           "gave " << nAt << " and 24 gave " << nPast << "; codes=["
        << allCodes(past.tree) << "]";

    // And the region must not be a cascade at all: one structural error, one
    // voice. `P_UnexpectedToken` was the cascade's carrier — 26 of them at 10
    // casts, 2 more per cast — so its count is the direct witness.
    EXPECT_LE(countCode(past.tree, DiagnosticCode::P_UnexpectedToken), 1u)
        << "one structural error yields one diagnostic; codes=["
        << allCodes(past.tree) << "]";

    // The chain BELOW the ceiling and the chain PAST it must be diagnosed the
    // same way: the cut changed WHEN a refuted branch stops, and nothing the
    // author wrote. A rendition that still reported a ceiling here would fail
    // this — and would be reporting a limit it reached only under a reading it
    // had already rejected.
    EXPECT_EQ(allCodes(below.tree), allCodes(past.tree))
        << "below=[" << allCodes(below.tree) << "] past=["
        << allCodes(past.tree) << "]";
}

// ── (B) THE REFUSAL IS STILL LOUD, AND STILL NAMES ITSELF ───────────────────
//
// A faster wrong answer is not the goal. This is the same contract
// `test_parser_speculation_ceilings.cpp` pins at a RE-IMPOSED small ceiling,
// asserted here at a LARGE one — the regime the cost fix exists to make
// reachable, and the one where a refusal that quietly stopped firing would
// look exactly like a very fast refusal.
TEST(ParserSpeculationRefusalCost, ALargeCeilingStillRefusesOnePastItByName) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    ParserConfig cfg = shippedCConfig(*schema);
    cfg.maxSpeculationDepth = 2048;
    cfg.maxExpressionDepth  = 1u << 20;

    ParseResult r = parseCFull(schema, castChain(2049), std::move(cfg));
    ASSERT_NE(r.tree.root(), InvalidNode) << "must RECOVER, never abort";
    EXPECT_TRUE(r.tree.diagnostics().hasErrors());
    ASSERT_EQ(countCode(r.tree, DiagnosticCode::P_MaxSpeculationDepth), 1u)
        << "exactly one honest ceiling report; codes=[" << allCodes(r.tree) << "]";
    EXPECT_EQ(countCode(r.tree, DiagnosticCode::P_NoAlternativeMatched), 0u)
        << "no fabricated syntax error against the author's own `int`; codes=["
        << allCodes(r.tree) << "]";

    const std::string msg =
        firstMessageFor(r.tree, DiagnosticCode::P_MaxSpeculationDepth);
    EXPECT_NE(msg.find("speculation-depth limit"), std::string::npos) << msg;
    EXPECT_NE(msg.find("2048"), std::string::npos) << msg;
    EXPECT_NE(msg.find("parser.maxSpeculationDepth"), std::string::npos) << msg;

    // And the chain one BELOW the ceiling is still accepted — the refusal
    // moved in time, not in position.
    ParserConfig ok = shippedCConfig(*schema);
    ok.maxSpeculationDepth = 2048;
    ok.maxExpressionDepth  = 1u << 20;
    ParseResult good = parseCFull(schema, castChain(2048), std::move(ok));
    EXPECT_FALSE(good.tree.diagnostics().hasErrors())
        << "the ceiling itself must still parse clean; codes=["
        << allCodes(good.tree) << "]";
}

// ── A ROLLED-BACK PROBE MUST NOT DISARM THE CASCADE SHIELD ──────────────────
//
// `suppressCascadeUntilSync_` is DELIBERATELY not snapshotted by
// `SpeculationProbe` — it is a fact about the depth-0 parse, not about a
// branch, the same reasoning that keeps `speculationCapHit_` out of the
// snapshot. But `stepOnce` LOWERED it with no `speculationDepth == 0` guard,
// so a SPECULATIVE walk that merely PEEKED a sync token permanently disarmed a
// shield the depth-0 parse had raised, and the probe's rollback could not put
// it back. Raised at depth 0, consulted at depth 0 (`emitParserError`), and
// now lowered at depth 0 too
// (D-PARSE-SPECULATION-REFUSAL-REPLAY-IS-QUADRATIC).
//
// ⚠ THE WITNESS COST 1150 DIFFERENTIAL PROGRAMS TO FIND, and that is why it is
// pinned here rather than described in a comment. The hole is obvious from the
// code and nearly unreachable in practice: `recoverAt` panic-scans to a
// STOP-POINT, and stop-points include every sync token, so after almost every
// raise the depth-0 parse is already sitting on the token that lowers the
// shield legitimately. Reaching it needs the scan to stop on a FOLLOW-set
// token that is NOT a sync token (here a `)`), and then a probe to peek past a
// `;` and fail. This is machine-found garbage C, kept VERBATIM: a hand-written
// paraphrase would be a program nobody measured.
//
// ✔MEASURED against the mutant that removes ONLY that depth guard: 6
// diagnostics become 9 — a `P_NoAlternativeMatched`, a
// `P_MissingRequiredChild` and a second `P_UnexpectedToken` leak out of the
// region the ceiling report had shielded.
TEST(ParserSpeculationRefusalCost, ARolledBackProbeDoesNotDisarmTheCascadeShield) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);

    ParserConfig cfg = shippedCConfig(*schema);
    // ⚠ P65 (D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS)
    // — THE CEILING MOVED BY ONE LEVEL AND THE CAP IS LOWERED TO MATCH, WHICH IS
    // A RECALIBRATION OF THE INSTRUMENT AND NOT A WEAKENING OF WHAT IT ASSERTS.
    // `finalCandidateDirectDescent_` removed ONE enclosing probe from this
    // shape: the depth-0 statement alt's final candidate is now DESCENDED into
    // rather than probed, so the cast chain below starts one speculation level
    // shallower and this program stopped reaching a cap of 8 at all —
    // ✔MEASURED, `countCode(P_MaxSpeculationDepth)` went 1 -> 0 and the shielded
    // cascade leaked exactly as the mutant's transcript below predicts, which is
    // what turned the arm red. The VERBATIM machine-found program is kept
    // (paraphrasing it would discard the 1150-program search that found it) and
    // the cap moves instead, so the arm still asserts the same three facts: the
    // ceiling reports ONCE, by name, and the region it shields stays shielded.
    cfg.maxSpeculationDepth = 7;
    cfg.maxExpressionDepth  = 64;

    ParseResult r = parseCFull(
        schema,
        "int main ( void ) { int x = 0 ; x = ( int ) ( int ) ( int ) ( int )"
        " ( int ) ( int ) ( int ) ( int ; ( int ) ( int ) x ) struct { int q ;"
        " } s = ( x ; return x ; }",
        std::move(cfg));

    ASSERT_NE(r.tree.root(), InvalidNode) << "must RECOVER, never abort";
    // The ceiling is reported once and by name — the shield's precondition.
    ASSERT_EQ(countCode(r.tree, DiagnosticCode::P_MaxSpeculationDepth), 1u)
        << allCodes(r.tree);
    // …and the region it shielded stays shielded across the speculative walks
    // that run inside it. Each of these is a diagnostic the unguarded lowering
    // let through.
    EXPECT_EQ(countCode(r.tree, DiagnosticCode::P_MissingRequiredChild), 0u)
        << "a rolled-back probe disarmed the cascade shield; codes=["
        << allCodes(r.tree) << "]";
    EXPECT_EQ(countCode(r.tree, DiagnosticCode::P_UnexpectedToken), 1u)
        << "a rolled-back probe disarmed the cascade shield; codes=["
        << allCodes(r.tree) << "]";
    EXPECT_EQ(countCode(r.tree, DiagnosticCode::P_NoAlternativeMatched), 3u)
        << "a rolled-back probe disarmed the cascade shield; codes=["
        << allCodes(r.tree) << "]";
}

// ── (C) IDENTITY — THE SAME PROGRAM MAKES THE SAME TREE ─────────────────────
//
// The fix cuts a speculative branch short the moment it is refuted. If that
// window ever mattered — if a branch could still change the parse after
// emitting — the result would be a SILENT MISCOMPILE: a tree that is wrong
// while every diagnostic looks right. So each case renders the finished tree
// node-for-node plus the whole diagnostic stream and prints a hash of it, and
// the red-on-disable arm compares those lines between the two renditions
// byte-for-byte (see the file header). The per-case expectations below give
// the file teeth on its own, without an arm to diff against.
TEST(ParserSpeculationRefusalCost, SpeculativeParsesAreIdenticalAndPrintTheirRendering) {
    auto schema = cSchema();
    ASSERT_NE(schema, nullptr);
    const bool dump = std::getenv("DSS_SP_IDENTITY_DUMP") != nullptr;

    const auto cases = corpus();
    ASSERT_GE(cases.size(), 20u) << "the corpus must actually cover the alt";

    // ⚠ THE SELF-CHECK THAT KEEPS THIS CORPUS FROM GOING BLIND AGAIN. Counted
    // from the RESULTS, not from the case list: a ceiling case whose config
    // stopped tripping its ceiling would otherwise sit here forever, rendering
    // a perfectly identical parse of a region the change cannot reach.
    std::size_t ceilingsRaised = 0;

    for (std::size_t i = 0; i < cases.size(); ++i) {
        Case const& c = cases[i];
        ParserConfig cfg = shippedCConfig(*schema);
        if (c.spec   != 0) cfg.maxSpeculationDepth     = c.spec;
        if (c.expr   != 0) cfg.maxExpressionDepth      = c.expr;
        if (c.factor != 0) cfg.speculationBudgetFactor = c.factor;
        ParseResult r = parseCFull(schema, c.source, std::move(cfg));

        ceilingsRaised +=
            countCode(r.tree, DiagnosticCode::P_MaxSpeculationDepth)
            + countCode(r.tree, DiagnosticCode::P_ExpressionTooDeep)
            + countCode(r.tree, DiagnosticCode::P_SpeculationBudgetExhausted);

        ASSERT_NE(r.tree.root(), InvalidNode) << c.name;
        const std::string rendering = render(r.tree);

        // The identity line. Byte-identical across renditions or the fix
        // changed what a program MEANS.
        std::fprintf(stdout, "IDENTITY %02zu %s %016llx nodes=%zu diags=%zu\n",
                     i, c.name,
                     static_cast<unsigned long long>(fnv1a64(rendering)),
                     static_cast<std::size_t>(r.tree.nodeCount()),
                     r.tree.diagnostics().all().size());
        if (dump) {
            std::fprintf(stdout, "---- %s ----\n%s", c.name, rendering.c_str());
        }

        if (c.valid) {
            EXPECT_FALSE(r.tree.diagnostics().hasErrors())
                << c.name << " must parse clean; codes=[" << allCodes(r.tree)
                << "]";
            EXPECT_EQ(countCode(r.tree, DiagnosticCode::P_NoAlternativeMatched),
                      0u)
                << c.name << " codes=[" << allCodes(r.tree) << "]";
            EXPECT_EQ(countCode(r.tree, DiagnosticCode::P_MaxSpeculationDepth),
                      0u)
                << c.name << " codes=[" << allCodes(r.tree) << "]";
        } else {
            EXPECT_TRUE(r.tree.diagnostics().hasErrors())
                << c.name << " must be REFUSED, not silently accepted";
            EXPECT_GE(countCode(r.tree, c.firstError), 1u)
                << c.name << " must be refused with its own diagnostic; "
                << "codes=[" << allCodes(r.tree) << "]";
        }
    }
    std::fflush(stdout);

    EXPECT_GE(ceilingsRaised, 4u)
        << "the identity corpus must actually RAISE the speculation ceilings "
           "— the latch is the one piece of parser state a probe rollback "
           "does not restore, so a corpus that never reaches a ceiling is "
           "structurally blind to the change it is offered as proof of";
}
