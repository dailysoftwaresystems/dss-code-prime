// Panic-mode recovery + diagnostic-UX pins.
//
// Three axes of recovery quality:
//   1. Loop reaches a sync/follow point and continues parsing.
//   2. Diagnostics carry rich content — `expected` from the schema
//      cursor and `actual` rendered from source bytes.
//   3. Cascade bound: one structural error produces at most one
//      recovery diagnostic for the broken region.

#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
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
    Tokenizer tk{src, schema};
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
    Tokenizer tk{src, schema};
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
// then resumes parsing — the FOLLOWING `var y = z;` MUST appear in
// the tree.
TEST(ParserRecovery, ScansToSyncTokenAndResumes) {
    auto h = loadShipped("toy", "var x = @ noise more; var y = z;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_TRUE(t.diagnostics().hasErrors());

    // After resync, the second `var y = z;` must have produced a
    // statement frame. Two statements total (one broken, one clean).
    std::size_t stmtCount = 0;
    const auto stmtRule = t.schema().rules().find("statement");
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
    auto h = loadShipped("toy", "var x = @ noise more; var y = z;");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    EXPECT_TRUE(t.diagnostics().hasErrors());

    const auto stmtRule = t.schema().rules().find("statement");
    ASSERT_TRUE(stmtRule.valid());
    bool sawCleanStmt = false;
    for (auto c : t.children(t.root())) {
        if (t.kind(c) == NodeKind::Internal && t.rule(c).v == stmtRule.v
            && !hasError(t.flags(c))) {
            sawCleanStmt = true;
        }
    }
    EXPECT_TRUE(sawCleanStmt)
        << "expected the post-sync `var y = z;` to parse error-free";
}

// ── diagnostic shape ────────────────────────────────────────────────────

// `expected` must be populated from `expectedSet(cursor)` — at least
// one entry, formatted with single quotes (renderer-ready prose:
// downstream `appendExpectedActual` does `expected 'X' or 'Y' — got 'Z'`).
TEST(ParserRecovery, EmittedDiagnosticHasExpectedListPopulated) {
    auto h = loadShipped("toy", "var x = @;");
    Parser p{h.src, h.schema, std::move(h.stream)};
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
    auto h = loadShipped("toy", "var x = @;");
    Parser p{h.src, h.schema, std::move(h.stream)};
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
    auto h = loadShipped("toy", "var x = @ noise more stuff; var y = z;");
    Parser p{h.src, h.schema, std::move(h.stream)};
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
    Parser p{h.src, h.schema, std::move(h.stream), std::move(cfg)};
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
    const std::string source = "var x = @ noise more;";
    {
        auto h = loadShipped("toy", source);
        ParserConfig cfg;
        cfg.recoveryStrategy = RecoveryStrategy::SingleToken;
        Parser p{h.src, h.schema, std::move(h.stream), std::move(cfg)};
        auto t = std::move(p).parse();
        EXPECT_NE(t.tree.root(), InvalidNode);
        EXPECT_TRUE(t.tree.diagnostics().hasErrors());
    }
    {
        auto h = loadShipped("toy", source);
        Parser p{h.src, h.schema, std::move(h.stream)};
        auto t = std::move(p).parse();
        EXPECT_NE(t.tree.root(), InvalidNode);
        EXPECT_TRUE(t.tree.diagnostics().hasErrors());
    }
}

// Panic-mode hitting EOF before any sync token: input has no `;`
// or `}` after the broken `@`. Recovery must terminate cleanly at
// EOF without scanning past the cap and without hanging.
TEST(ParserRecovery, PanicModeTerminatesAtEofWithNoSyncToken) {
    auto h = loadShipped("toy", "var x = @ @ @ @");
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    auto const& t = result.tree;

    EXPECT_TRUE(t.diagnostics().hasErrors());
    // No infinite loop, no watchdog stall: parse returned with a
    // built tree.
    EXPECT_NE(t.root(), InvalidNode);
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
        Parser(src, schema, std::move(empty), std::move(cfg)),
        "maxSyncScanTokens must be >= 1");
}
