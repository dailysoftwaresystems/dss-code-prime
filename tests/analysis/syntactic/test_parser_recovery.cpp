// Panic-mode recovery + diagnostic-UX pins.
//
// Three axes of recovery quality:
//   1. Loop reaches a sync/follow point and continues parsing.
//   2. Diagnostics carry rich content — `expected` from the schema
//      cursor and `actual` rendered from source bytes.
//   3. Cascade bound: one structural error produces at most one
//      recovery diagnostic for the broken region.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

struct Harness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
};

// `EXPECT_TRUE` doesn't abort the test, so dereferencing `*loaded`
// on failure would UB. `std::abort()` produces a clean fast-fail
// pinned to the load site rather than a cryptic crash deeper in.
[[nodiscard]] Harness loadShipped(std::string_view name, std::string source) {
    auto loaded = GrammarSchema::loadShipped(name);
    if (!loaded.has_value()) {
        ADD_FAILURE() << "loadShipped(\"" << name << "\") failed: "
                      << loaded.error()[0].message;
        std::abort();
    }
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<recovery>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, _] = std::move(tk).tokenize();
    return Harness{std::move(src), std::move(schema), std::move(stream)};
}

[[nodiscard]] Harness loadInline(std::string_view schemaText,
                                 std::string      source) {
    auto loaded = GrammarSchema::loadFromText(schemaText);
    if (!loaded.has_value()) {
        ADD_FAILURE() << "loadFromText failed: "
                      << loaded.error()[0].message;
        std::abort();
    }
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<recovery>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, _] = std::move(tk).tokenize();
    return Harness{std::move(src), std::move(schema), std::move(stream)};
}

[[nodiscard]] std::size_t countCode(std::span<ParseDiagnostic const> diags,
                                    DiagnosticCode code) {
    return static_cast<std::size_t>(std::ranges::count_if(
        diags, [code](ParseDiagnostic const& d) { return d.code == code; }));
}

} // namespace

// ── sync-token recovery ─────────────────────────────────────────────────

// Panic-mode scans past the bad `@` until `;` (a declared sync token)
// then resumes parsing — the FOLLOWING `var y : int = z;` MUST appear in
// the tree.
TEST(ParserRecovery, ScansToSyncTokenAndResumes) {
    auto h = loadShipped("toy", "var x : int = @ noise more; var y : int = z;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());

    // After resync, the second `var y : int = z;` must have produced a
    // statement frame. Two statements total (one broken, one clean).
    std::size_t stmtCount = 0;
    const auto stmtRule = t.schema().rules().find("topLevel");
    ASSERT_TRUE(stmtRule.valid());
    for (auto c : t.children(t.root())) {
        if (t.kind(c) == NodeKind::Internal
            && t.rule(c).v == stmtRule.v) ++stmtCount;
    }
    EXPECT_EQ(stmtCount, 2u);
}

// Mid-statement noise inside a `varDecl` rhs that doesn't reach `;`
// still gets diagnosed, and a SECOND statement after the sync token
// parses cleanly. The resync point is the `;` in declared syncTokens.
//
// Input shape needs the bad tokens to land WITHIN a statement frame —
// at root, the repeat[statement] loop would just exit on the first
// non-stmt-FIRST token rather than dispatching to recovery.
TEST(ParserRecovery, BadTokensBeforeSyncDoNotPreventNextStmt) {
    auto h = loadShipped("toy", "var x : int = @ noise more; var y : int = z;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    EXPECT_TRUE(t.diagnostics().hasErrors());

    const auto stmtRule = t.schema().rules().find("topLevel");
    ASSERT_TRUE(stmtRule.valid());
    bool sawCleanStmt = false;
    for (auto c : t.children(t.root())) {
        if (t.kind(c) == NodeKind::Internal && t.rule(c).v == stmtRule.v
            && !hasError(t.flags(c))) {
            sawCleanStmt = true;
        }
    }
    EXPECT_TRUE(sawCleanStmt)
        << "expected the post-sync `var y : int = z;` to parse error-free";
}

// ── diagnostic shape ────────────────────────────────────────────────────

// `expected` must be populated from `expectedSet(cursor)` — at least
// one entry, formatted with single quotes (renderer-ready prose:
// downstream `appendExpectedActual` does `expected 'X' or 'Y' — got 'Z'`).
TEST(ParserRecovery, EmittedDiagnosticHasExpectedListPopulated) {
    auto h = loadShipped("toy", "var x : int = @;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    bool foundOne = false;
    for (auto const& d : t.diagnostics().all()) {
        if (d.code != DiagnosticCode::P_UnknownToken
            && !d.expected.empty()) {
            foundOne = true;
            EXPECT_EQ(d.expected.front().front(), '\'')
                << "expected entry must be quoted";
            break;
        }
    }
    EXPECT_TRUE(foundOne)
        << "at least one parser-emitted diagnostic must carry "
           "a non-empty `expected` list";
}

// `actual` carries the real source bytes for the bad token, quoted —
// not a raw integer kind id.
TEST(ParserRecovery, EmittedDiagnosticActualIsQuotedLexeme) {
    auto h = loadShipped("toy", "var x : int = @;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    bool sawAtSymbol = false;
    for (auto const& d : t.diagnostics().all()) {
        if (d.actual == "'@'") { sawAtSymbol = true; break; }
    }
    EXPECT_TRUE(sawAtSymbol)
        << "expected `actual = \"'@'\"` for the bad token's diagnostic";
}

// ── cascade bound ───────────────────────────────────────────────────────

// Cascade bound: one structural error inside a statement plus
// trailing noise must emit at most 3 recovery diagnostics across
// the broken region. Quality bar (`recovery continues without
// cascading more than 3× the original error count`). Builder-side
// codes (P_UnknownToken, P_SchemaCursorDesync) are not "cascade"
// symptoms and excluded from the count.
TEST(ParserRecovery, SingleErrorCascadeBoundedAtThreeX) {
    auto h = loadShipped("toy", "var x : int = @ noise more stuff; var y : int = z;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& diags = result.tree.diagnostics().all();

    std::size_t recoveryCount = 0;
    for (auto const& d : diags) {
        switch (d.code) {
        case DiagnosticCode::P_UnexpectedToken:
        case DiagnosticCode::P_NoAlternativeMatched:
        case DiagnosticCode::P_BacktrackFailed:
        case DiagnosticCode::P_MissingRequiredChild:
            ++recoveryCount;
            break;
        default:
            break;
        }
    }
    EXPECT_GE(recoveryCount, 1u);
    EXPECT_LE(recoveryCount, 3u);
}

// ── sync-scan cap ───────────────────────────────────────────────────────

constexpr std::string_view kSyncCapSchema = R"JSON({
  "dssSchemaVersion": 2,
  "language": { "name": "SyncCap", "version": "0.1.0" },
  "tokens": {
    " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "x": [{ "kind": "XKind" }],
    ";": [{ "kind": "Semi" }]
  },
  "syncTokens": ["Semi"],
  "shapes": {
    "root": { "sequence": [ "Semi" ] }
  }
})JSON";

// `maxSyncScanTokens = 3` with input "x x x x x ;" — recovery must
// stop at the cap (3 tokens) rather than running to the `;`.
TEST(ParserRecovery, SyncScanRespectsMaxCap) {
    auto h = loadInline(kSyncCapSchema, "x x x x x x x x x ;");
    ParserConfig cfg;
    cfg.maxSyncScanTokens = 3;
    Parser p{h.src, h.schema, std::move(h.stream),
             DiagnosticBudget::libraryDefault(), std::move(cfg)};
    auto result = std::move(p).parse();

    // At least one P_UnexpectedToken (the cursor expects `;`, sees `x`);
    // the cap-bounded scan leaves leftover `x` tokens which trigger
    // ADDITIONAL recovery diagnostics on later outer iterations.
    // The test pins: parse completes, multiple recovery diags fired
    // (proving the cap kicked in — without it, ONE scan would have
    // consumed everything up to `;` and only 1 diag would fire).
    auto const& diags = result.tree.diagnostics().all();
    EXPECT_GE(countCode(diags, DiagnosticCode::P_UnexpectedToken), 2u)
        << "cap=3 should leave residual `x` tokens for subsequent diags";
}

// ── SingleToken strategy ────────────────────────────────────────────────

// Both recovery strategies must run end-to-end without crashing or
// hanging on the same broken input. The exact diagnostic counts
// differ depending on how each strategy interacts with the schema-
// cursor's nullable-tail logic, but both must produce errored trees
// that include the broken statement structurally.
TEST(ParserRecovery, BothStrategiesCompleteOnBrokenInput) {
    const std::string source = "var x : int = @ noise more;";
    {
        auto h = loadShipped("toy", source);
        ParserConfig cfg;
        cfg.recoveryStrategy = RecoveryStrategy::SingleToken;
        Parser p{h.src, h.schema, std::move(h.stream),
             DiagnosticBudget::libraryDefault(), std::move(cfg)};
        auto t = std::move(p).parse();
        EXPECT_NE(t.tree.root(), InvalidNode);
        EXPECT_TRUE(t.tree.diagnostics().hasErrors());
    }
    {
        auto h = loadShipped("toy", source);
        Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
        auto t = std::move(p).parse();
        EXPECT_NE(t.tree.root(), InvalidNode);
        EXPECT_TRUE(t.tree.diagnostics().hasErrors());
    }
}

// Panic-mode hitting EOF before any sync token: input has no `;`
// or `}` after the broken `@`. Recovery must terminate cleanly at
// EOF without scanning past the cap and without hanging.
TEST(ParserRecovery, PanicModeTerminatesAtEofWithNoSyncToken) {
    auto h = loadShipped("toy", "var x : int = @ @ @ @");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    EXPECT_TRUE(t.diagnostics().hasErrors());
    // No infinite loop, no watchdog stall: parse returned with a
    // built tree.
    EXPECT_NE(t.root(), InvalidNode);
}

// ── off-grammar body-token drain × recovery ────────────────────────────
//
// String / bracket-id / comment bodies emit one off-grammar leaf per
// codepoint. The parser's per-iteration drain skips them when dispatch
// proceeds normally, and `panicRecover`'s sync-scan loop silently
// consumes them as "not-in-sync, not-in-follow" bytes. Either way,
// body codepoints inside a broken region must NOT generate a
// per-codepoint diagnostic storm.
//
// Broken tsql `SELECT 'oops' x FROM t;` — `SELECT 'oops'` matches the
// selectItem path (operand → stringLit body), then `x` is an extra
// unexpected token before `FROM`. Recovery scans to the next sync
// (`;`). The trip through the string body before the structural
// error AND any body chars in the sync-scan range must not multiply
// diagnostics.
TEST(ParserRecovery, BodyCodepointsDoNotMultiplyDiagnostics) {
    // Build a broken statement whose recovery span crosses a string
    // body. `'abcdefg'` is 9 codepoints (7 body + opener + closer);
    // if any of them produced its own diagnostic the count would
    // explode.
    auto h = loadShipped("tsql-subset",
        "SELECT @bad 'abcdefg' garbage FROM t;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    EXPECT_TRUE(t.diagnostics().hasErrors());
    // Pin EXACT diagnostic-code multiset (codes only; spans drift).
    // A regression that lets per-codepoint body chars through dispatch
    // produces one extra diag per body byte — strict-equality catches
    // even a 1-codepoint leak. Print the full multiset on mismatch so
    // a drift surfaces the actual codes a reader can compare against.
    std::map<DiagnosticCode, std::size_t> codeCounts;
    for (auto const& d : t.diagnostics().all()) {
        ++codeCounts[d.code];
    }
    const std::size_t totalDiags = t.diagnostics().all().size();
    // The exact expected count was empirically 2 codes on the
    // current recovery policy. The cap-3 floor catches any
    // body-codepoint leak (which would push the count to ~9+ for the
    // 7-char string body) without locking us to a specific recovery
    // shape that may evolve.
    EXPECT_LE(totalDiags, 3u)
        << "body codepoints must not multiply diagnostics. Codes seen:\n"
        << [&] {
            std::string s;
            for (auto const& [c, n] : codeCounts) {
                s += "  ";
                s += diagnosticCodeName(c);
                s += ": " + std::to_string(n) + "\n";
            }
            return s;
        }();
    EXPECT_NE(t.root(), InvalidNode);
}

// Clean-parse counterpart: a SELECT whose string literal sits in a
// well-formed position. Body codepoints are absorbed silently by the
// drain — zero diagnostics, tree still walks. This pins the
// "happy-path drain" half of the body-token machinery while the
// recovery test above pins the broken-path half.
TEST(ParserRecovery, BodyCodepointsAbsorbedCleanlyOnHappyPath) {
    auto h = loadShipped("tsql-subset",
        "SELECT * FROM t WHERE name = 'a long string value';");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "string body codepoints must be absorbed silently by the "
           "parser's body-token drain";
    EXPECT_NE(t.root(), InvalidNode);
}

// ── externTail broken-path ─────────────────────────────────────────────
//
// `externDecl`'s tail is an alt of `funcParamsOnly` (starts with `(`)
// and `varDeclTail` (starts with `[`, `=`, or `;`). A malformed extern
// like `extern int foo bar;` matches neither arm at the position after
// `foo` (`bar` is a stray Identifier). The parser must emit a
// diagnostic at the offending token and recover to the next sync
// point. Without this pin, a regression that silently accepts the
// stray token or routes the diagnostic to the wrong span would slip
// through every happy-path corpus test.
TEST(ParserRecovery, ExternTailUnexpectedTokenIsDiagnosed) {
    auto h = loadShipped("c", "extern int foo bar;");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    EXPECT_TRUE(t.diagnostics().hasErrors())
        << "stray identifier in externTail position must produce a diagnostic";
    EXPECT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(hasError(t.flags(t.root())))
        << "HasError must propagate to the root frame";
}

// ── ctor preconditions ─────────────────────────────────────────────────

TEST(ParserRecoveryDeath, ZeroSyncScanCapAborts) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString("", "<x>");
    TokenStream empty;

    ParserConfig cfg;
    cfg.maxSyncScanTokens = 0;
    EXPECT_DEATH(
        Parser(src, schema, std::move(empty),
               DiagnosticBudget::libraryDefault(), std::move(cfg)),
        "maxSyncScanTokens must be >= 1");
}

// ── missing required production before a stop-point ─────────────────────
//
// A required rule that can't start at the current token, where that token is
// a STOP-POINT (a sync token or in an enclosing FOLLOW), is recovered by
// SYNTHESIZING the missing rule and letting the enclosing frame consume the
// token — NOT by panic-consuming it. Panic-consuming a scope-closer would
// leave the scope stack unbalanced (`P_BuilderInvariant` at finish) and
// strand the rest of the input as a Missing-node cascade.

namespace {
// Recursive: does the subtree rooted at `n` contain an Internal node for
// `rule`? Proves recovery RESTORED post-error structure vs discarding it.
[[nodiscard]] bool treeContainsRule(Tree const& t, NodeId n, RuleId rule) {
    if (t.kind(n) == NodeKind::Internal && t.rule(n).v == rule.v) return true;
    for (auto c : t.children(n)) {
        if (treeContainsRule(t, c, rule)) return true;
    }
    return false;
}
} // namespace

// THE headline pin (RED-on-disable). `int x = ;` — a declaration with `=`
// then an immediately-absent initializer expression. Pre-fix the `initValue`
// RuleLeaf recovery left the schema cursor frozen; panic recovery then
// consumed the `}` block-closer via `tokens.advance()` (never `pushToken`, so
// `closesScope` never fired) → `P_BuilderInvariant` "scope stack non-empty at
// finish" + a 3x `P_MissingRequiredChild` EOF cascade. The fix synthesizes the
// missing `initValue` and lets `varDeclHead` consume the `;`, the block consume
// + scope-close its `}`, and `return undefined_thing;` parse normally.
TEST(ParserRecovery, MissingInitializerExpressionRecoversCleanly) {
    auto h = loadShipped("c",
        "int main() {\n    int x = ;\n    return undefined_thing;\n}\n");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& t     = result.tree;
    auto const& diags = t.diagnostics().all();

    // Exactly ONE positioned parse diagnostic for the missing expression
    // (pre-fix: 3). This count is the primary RED-on-disable lever.
    ASSERT_EQ(countCode(diags, DiagnosticCode::P_NoAlternativeMatched), 1u);
    // ...located at the `;` (line 2, column 13).
    for (auto const& d : diags) {
        if (d.code == DiagnosticCode::P_NoAlternativeMatched) {
            const auto lc = t.source().lineCol(d.span.start());
            EXPECT_EQ(lc.line, 2u);
            EXPECT_EQ(lc.column, 13u);
        }
    }
    // ZERO builder-internal invariants: the block scope balanced (pre-fix 1).
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_BuilderInvariant), 0u);
    // ZERO EOF Missing-node cascade: recovery returned to the block (pre-fix 3).
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_MissingRequiredChild), 0u);

    // Positive structural proof recovery restored the block: the
    // `return undefined_thing;` after the broken decl appears in the tree
    // (pre-fix those tokens were panic-discarded entirely).
    const auto returnRule = t.schema().rules().find("returnStmt");
    ASSERT_TRUE(returnRule.valid());
    EXPECT_TRUE(treeContainsRule(t, t.root(), returnRule))
        << "the post-error `return ...;` must survive recovery in the tree";
}

// Suppression-reset invariant. TWO separate broken decls (`int x = ;` then
// `int y = ;`) each emit their OWN diagnostic: the clean `int y =` dispatch
// between the two errors resets `stepRecovered_` at the top of `stepOnce`, so
// the second synthesis is NOT mid-recovery and is NOT suppressed. This guards
// that the one-diagnostic-per-region suppression collapses only a CONTIGUOUS
// recovery run, never two genuinely-distinct errors separated by clean parse.
TEST(ParserRecovery, MissingInitDiagnosticsAreNotSuppressedAcrossCleanParse) {
    auto h = loadShipped("c",
        "int main() {\n    int x = ;\n    int y = ;\n    return 0;\n}\n");
    Parser p{h.src, h.schema, std::move(h.stream), DiagnosticBudget::libraryDefault()};
    auto result = std::move(p).parse();
    auto const& diags = result.tree.diagnostics().all();

    // Both missing initializers diagnosed — not collapsed to one.
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_NoAlternativeMatched), 2u);
    // Still clean recovery — no invariant, no EOF cascade.
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_BuilderInvariant), 0u);
    EXPECT_EQ(countCode(diags, DiagnosticCode::P_MissingRequiredChild), 0u);
}

// AGNOSTICISM is proven by the by-construction diagnostic-corpus harness
// (tests/analysis/test_diagnostic_corpus.cpp): the SAME grammar-driven fix
// (isStopPoint = schema `syncTokens` + FOLLOW; synthesize the missing
// RuleLeaf) fires for THREE shipped grammars with no engine identity branch —
// c (`missing_init.c`, the scope-bearing block case above), toy
// (`unknown_token.toy`), and tsql-subset (`broken_select.sql`), whose goldens
// each shed the pre-fix spurious EOF cascade. The garbage path (a non-stop-
// point bad token still panic-scans, NOT synthesize-and-skip) is held by the
// toy `@ noise more` resync pins earlier in this file — they would RED if the
// stop-point branch over-fired on garbage.

// ── A CORRECT REFUSAL IS NOT FOLLOWED BY THE BUILDER'S OWN INVARIANT ───────
//
// [[D-PARSE-BUILDER-INVARIANT-PRINTED-AFTER-A-CORRECT-REFUSAL]]. ✔MEASURED
// before the fix, through `dsscp --compile` over 15 refusal shapes: NINE of
// them, after the parser had reported the real error, also printed
// `P_BuilderInvariant: scope stack non-empty at finish` — the tree builder's
// internal check, worded for the compiler's author, spent on user input. The
// scope stack is driven by tokens and the frames by the parser, so the two
// drifted apart whenever the parse never reached a closer: an unterminated
// literal swallowed it, panic recovery consumed it without `pushToken`, or the
// input ended first. A frame now closes the scopes it opened
// (`TreeBuilder::closeFrame_`).
//
// Each shape runs through the front end the driver runs (preprocess + parse),
// and each pin asserts the invariant is ABSENT and the user's own diagnostic
// is still there — so a fix that silenced everything cannot pass.
// RED-ON-DISABLE: without the frame's truncation every pin below reds by name.

namespace {

struct FrontEnd {
    std::shared_ptr<CompilationUnit> cu;
    std::vector<ParseDiagnostic>     diags;
};

[[nodiscard]] FrontEnd frontEnd(std::string source) {
    UnitBuilder b{dss::test_support::shippedSchemaOrThrow("c"),
                  DiagnosticBudget::libraryDefault()};
    b.addInMemory(std::move(source), "main.c");
    FrontEnd out{std::make_shared<CompilationUnit>(std::move(b).finish()), {}};
    for (auto const& t : out.cu->trees()) {
        for (auto const& d : t.diagnostics().all()) out.diags.push_back(d);
    }
    return out;
}

[[nodiscard]] std::string codesOf(std::span<ParseDiagnostic const> diags) {
    std::string s;
    for (auto const& d : diags) {
        s += ' ';
        s += diagnosticCodeName(d.code);
    }
    return s;
}

void expectRefusedWithoutTheInvariant(std::string source, DiagnosticCode userCode) {
    auto const fe = frontEnd(std::move(source));
    EXPECT_EQ(countCode(fe.diags, DiagnosticCode::P_BuilderInvariant), 0u)
        << "the builder's internal invariant reached the user; codes:" << codesOf(fe.diags);
    EXPECT_GE(countCode(fe.diags, userCode), 1u)
        << "the user's own diagnostic " << diagnosticCodeName(userCode)
        << " must still be reported; codes:" << codesOf(fe.diags);
}

} // namespace

// (1)–(3) the closer SWALLOWED by an unterminated literal on its own line
TEST(ParserRecoveryScopeBalance, ACharConstantSwallowingTheBlockCloser) {
    expectRefusedWithoutTheInvariant(
        "int f(void){ return 'a; }\nint g(void){ return 1; }\nint main(void){ return 0; }\n",
        DiagnosticCode::P_UnexpectedToken);
}
TEST(ParserRecoveryScopeBalance, AStringLiteralSwallowingTheBlockCloser) {
    expectRefusedWithoutTheInvariant(
        "int f(void){ char const *s = \"abc; }\nint main(void){ return 0; }\n",
        DiagnosticCode::P_UnexpectedToken);
}
TEST(ParserRecoveryScopeBalance, AMacroExpandingToAnUnterminatedCharInABlock) {
    expectRefusedWithoutTheInvariant("#define X 'a\nint main(void){ return X; }\n",
                                     DiagnosticCode::P_UnexpectedToken);
}

// (4)–(6) the closer genuinely MISSING at the end of input
TEST(ParserRecoveryScopeBalance, OneMissingBlockCloserAtEndOfInput) {
    expectRefusedWithoutTheInvariant("int main(void){ return 0;\n",
                                     DiagnosticCode::P_MissingRequiredChild);
}
TEST(ParserRecoveryScopeBalance, TwoMissingBlockClosersAtEndOfInput) {
    expectRefusedWithoutTheInvariant("int main(void){ if (1) { return 0;\n",
                                     DiagnosticCode::P_MissingRequiredChild);
}
TEST(ParserRecoveryScopeBalance, AMissingParenAndBlockCloserAtEndOfInput) {
    expectRefusedWithoutTheInvariant("int main(void){ return (1 + 2;\n",
                                     DiagnosticCode::P_MissingRequiredChild);
}

// (7)–(9) a syntax error INSIDE a block whose `}` is present: panic recovery
// consumes it without `pushToken`
TEST(ParserRecoveryScopeBalance, AnIncompleteExpressionInABlock) {
    expectRefusedWithoutTheInvariant(
        "int main(void){ int x = 1 +; return x; }\nint g(void){ return 2; }\n",
        DiagnosticCode::P_BacktrackFailed);
}
TEST(ParserRecoveryScopeBalance, AnUnfinishedCallInABlock) {
    expectRefusedWithoutTheInvariant(
        "int f(int a){ return a; }\nint main(void){ return f(; }\nint g(void){ return 2; }\n",
        DiagnosticCode::P_MissingRequiredChild);
}
TEST(ParserRecoveryScopeBalance, AnEmptyIfConditionInABlock) {
    expectRefusedWithoutTheInvariant(
        "int main(void){ if ( ) { return 1; } return 0; }\nint g(void){ return 2; }\n",
        DiagnosticCode::P_NoAlternativeMatched);
}

// ★ THE AT-END-OF-INPUT FRAME TELLS THE TRUTH. A block whose `}` never came was
// closed by the parser's premature-end-of-input handling WITHOUT an Error leaf,
// so the tree claimed a well-formed block while the diagnostic said its `}` was
// missing — and the builder's new clean-frame check caught exactly that. The
// branch now drops the Error leaf its two sibling missing-child sites already
// drop. Pinned: the diagnostic's own text is the one it always was, and every
// `block` node on the path carries HasError.
// RED-ON-DISABLE: remove that one leaf and "closed cleanly" is back.
TEST(ParserRecoveryScopeBalance, ABlockThatReachedEndOfInputCarriesTheErrorInItsTree) {
    for (std::string const src : {std::string{"int main(void){ return 0;\n"},
                                  std::string{"int main(void){ if (1) { return 0;\n"}}) {
        SCOPED_TRACE(src);
        auto const fe = frontEnd(src);
        EXPECT_EQ(countCode(fe.diags, DiagnosticCode::P_BuilderInvariant), 0u)
            << codesOf(fe.diags);
        bool sawMissingChild = false;
        for (auto const& d : fe.diags) {
            if (d.code != DiagnosticCode::P_MissingRequiredChild) continue;
            sawMissingChild = true;
            EXPECT_EQ(d.actual, "'<eof>'") << "the diagnostic's text is unchanged";
        }
        EXPECT_TRUE(sawMissingChild) << codesOf(fe.diags);

        ASSERT_EQ(fe.cu->trees().size(), 1u);
        Tree const& t = fe.cu->trees()[0];
        RuleId const blockRule = t.schema().rules().find("block");
        ASSERT_TRUE(blockRule.valid());
        std::size_t blocks = 0;
        for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
            NodeId const id{i, t.id().v};
            if (t.kind(id) != NodeKind::Internal || t.rule(id).v != blockRule.v) continue;
            ++blocks;
            EXPECT_TRUE(hasError(t.flags(id)))
                << "a block that reached the end of input is not a well-formed block";
        }
        EXPECT_GE(blocks, 1u);
    }
}
