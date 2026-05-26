// Pratt-walker unit pins. The walker drives operator-precedence climbing
// for `expr`-shape rules. Tests here construct inline schemas with
// targeted operator tables so each axis (left/right assoc, prefix,
// postfix, mixed precedence) is covered in isolation. The c-subset
// end-to-end pin lives in test_parser_c_subset_smoke.cpp; this file
// pins the walker's structural output on minimal grammars where the
// tree shape isn't obscured by surrounding syntax.

#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

// Inline schema with left-assoc `+` and `*` (`*` binds tighter), plus
// a right-assoc `=` (lowest precedence) and a prefix `-` (highest).
// Mirrors the c-subset table for the operators it includes; tests pin
// the walker's output against this minimal grammar to avoid the
// surrounding c-subset machinery (function decls, blocks, etc.).
constexpr std::string_view kInfixSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "PrattInfix", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "+":  [{ "kind": "PlusOp"  }],
    "-":  [{ "kind": "MinusOp" }],
    "*":  [{ "kind": "StarOp"  }],
    "=":  [{ "kind": "AssignOp" }],
    ";":  [{ "kind": "Semi" }]
  },
  "operators": {
    "groups": [
      { "precedence": 15, "associativity": "right",                    "operators": ["="]  },
      { "precedence": 65, "associativity": "left",                     "operators": ["+", "-"] },
      { "precedence": 70, "associativity": "left",                     "operators": ["*"]  },
      { "precedence": 90, "associativity": "right", "arity": "prefix", "operators": ["-"]  }
    ]
  },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "stmt" }] },
    "stmt":       { "sequence": ["expression", "Semi"] },
    "expression": {
      "expr": {
        "atom": "operand",
        "wrapperRules": {
          "binary":  "binaryExpr",
          "unary":   "unaryExpr",
          "postfix": "postfixExpr"
        }
      }
    },
    "operand":    { "alt": ["Identifier"] }
  }
})JSON";

// Inline schema with a single postfix operator `?`. C-subset doesn't
// ship any postfix ops yet (PA4 will add `++`/`--`/`()`/`[]`), so we
// need a dedicated schema to pin postfix behavior here.
constexpr std::string_view kPostfixSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "PrattPostfix", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "+":  [{ "kind": "PlusOp"  }],
    "?":  [{ "kind": "QueryOp" }],
    ";":  [{ "kind": "Semi" }]
  },
  "operators": {
    "groups": [
      { "precedence": 65, "associativity": "left",                       "operators": ["+"] },
      { "precedence": 95,                            "arity": "postfix", "operators": ["?"] }
    ]
  },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "stmt" }] },
    "stmt":       { "sequence": ["expression", "Semi"] },
    "expression": {
      "expr": {
        "atom": "operand",
        "wrapperRules": {
          "binary":  "binaryExpr",
          "unary":   "unaryExpr",
          "postfix": "postfixExpr"
        }
      }
    },
    "operand":    { "alt": ["Identifier"] }
  }
})JSON";

struct PrattHarness {
    std::shared_ptr<SourceBuffer>        src;
    std::shared_ptr<GrammarSchema const> schema;
    TokenStream                          stream;
};

[[nodiscard]] PrattHarness loadAndTokenize(std::string_view schemaText,
                                           std::string source) {
    auto loaded = GrammarSchema::loadFromText(schemaText);
    EXPECT_TRUE(loaded.has_value())
        << (loaded.has_value() ? "" : loaded.error()[0].message);
    auto schema = *loaded;
    auto src    = SourceBuffer::fromString(std::move(source), "<pratt>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();
    return PrattHarness{
        .src    = std::move(src),
        .schema = std::move(schema),
        .stream = std::move(stream),
    };
}

[[nodiscard]] Tree parse(std::string_view schemaText, std::string source) {
    auto h = loadAndTokenize(schemaText, std::move(source));
    Parser p{h.src, h.schema, std::move(h.stream)};
    auto result = std::move(p).parse();
    return std::move(result.tree);
}

// Skip whitespace tokens when comparing structural output. The walker
// pushes trivia tokens through unchanged, but the AST-mode cursor's
// EmptySpace skip would normally hide them; keep that visible for
// clarity by emitting only non-trivia leaves.
inline std::string prettyPrint(Tree const& t) {
    std::string out;
    if (!t.root().valid()) return out;
    walkPreOrder(TreeCursor{t, t.root(), CursorMode::Ast},
                 [&](TreeCursor const& c) {
        const int d = c.depth();
        for (int i = 0; i < d; ++i) out += "  ";
        const auto id = c.current();
        if (t.kind(id) == NodeKind::Internal) {
            out += "rule:";
            out += t.rules().name(t.rule(id));
        } else {
            out += "tok:\"";
            out += t.text(id);
            out += '"';
        }
        out += '\n';
    });
    return out;
}

[[nodiscard]] std::size_t countCode(
    std::span<ParseDiagnostic const> diags, DiagnosticCode code) {
    return static_cast<std::size_t>(std::ranges::count_if(
        diags, [code](ParseDiagnostic const& d) { return d.code == code; }));
}

} // namespace

// ── primary cases ───────────────────────────────────────────────────────

TEST(PrattWalker, BareIdentifierIsOperandOnly) {
    Tree t = parse(kInfixSchema, "a;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:operand\n"
        "        tok:\"a\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// ── infix precedence ────────────────────────────────────────────────────

// `a + b * c;` — `*` binds tighter than `+`, so the inner `b * c` becomes
// the right operand of the outer `+`. Verifies the mixed-precedence
// climb produces the correct nesting.
TEST(PrattWalker, MixedPrecedenceMakesInnerBinaryExpr) {
    Tree t = parse(kInfixSchema, "a + b * c;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"+\"\n"
        "        rule:binaryExpr\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "          tok:\"*\"\n"
        "          rule:operand\n"
        "            tok:\"c\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// `a * b + c;` — `*` (tighter) on the left; `+` is at the outer level.
// The left side wraps the `a * b` group; the right is just `c`.
TEST(PrattWalker, TightLhsThenLooseOperatorWrapsLhsInBinaryExpr) {
    Tree t = parse(kInfixSchema, "a * b + c;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:binaryExpr\n"
        "          rule:operand\n"
        "            tok:\"a\"\n"
        "          tok:\"*\"\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "        tok:\"+\"\n"
        "        rule:operand\n"
        "          tok:\"c\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Same-precedence left-assoc operator chain `a + b + c;` — the tree
// shape is right-recursive (each `+` recurses for the right side at
// equal precedence), which is the documented PA2 design. Left-vs-right
// associativity is encoded in the operator table, not the tree's
// structural nesting; a downstream semantic pass reads
// `operatorTable().lookup(+, Infix)->associativity` to know how to
// interpret the chain.
TEST(PrattWalker, SamePrecLeftAssocChainIsRightRecursive) {
    Tree t = parse(kInfixSchema, "a + b + c;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"+\"\n"
        "        rule:binaryExpr\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "          tok:\"+\"\n"
        "          rule:operand\n"
        "            tok:\"c\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Right-assoc `=` chain `a = b = c;` — tree shape matches the
// semantic right-associativity directly (`a = (b = c)`).
TEST(PrattWalker, RightAssocChainNestsRightward) {
    Tree t = parse(kInfixSchema, "a = b = c;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"=\"\n"
        "        rule:binaryExpr\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "          tok:\"=\"\n"
        "          rule:operand\n"
        "            tok:\"c\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// ── prefix ──────────────────────────────────────────────────────────────

TEST(PrattWalker, BarePrefixWrapsOperandInUnaryExpr) {
    Tree t = parse(kInfixSchema, "-a;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:unaryExpr\n"
        "        tok:\"-\"\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// `-a + b;` — prefix `-` (prec 90) binds tighter than `+` (prec 65),
// so the unary wraps just `a`, and the binaryExpr wraps the unary on
// the left of `+`.
TEST(PrattWalker, PrefixBindsTighterThanInfix) {
    Tree t = parse(kInfixSchema, "-a + b;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:unaryExpr\n"
        "          tok:\"-\"\n"
        "          rule:operand\n"
        "            tok:\"a\"\n"
        "        tok:\"+\"\n"
        "        rule:operand\n"
        "          tok:\"b\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// `a + -b;` — prefix `-` on the RHS wraps just `b`.
TEST(PrattWalker, PrefixOnRhsWrapsRhsOperandOnly) {
    Tree t = parse(kInfixSchema, "a + -b;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"+\"\n"
        "        rule:unaryExpr\n"
        "          tok:\"-\"\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// ── postfix ─────────────────────────────────────────────────────────────

TEST(PrattWalker, BarePostfixWrapsOperandInPostfixExpr) {
    Tree t = parse(kPostfixSchema, "a?;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:postfixExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"?\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// `a + b?;` — postfix `?` (prec 95) binds tighter than `+` (prec 65),
// so the postfix wraps just `b`, and the binaryExpr wraps `[a, +, b?]`.
TEST(PrattWalker, PostfixBindsTighterThanInfix) {
    Tree t = parse(kPostfixSchema, "a + b?;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"+\"\n"
        "        rule:postfixExpr\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "          tok:\"?\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// ── diagnostics ─────────────────────────────────────────────────────────

// Happy paths must emit no diagnostics — neither parser-side recovery
// codes nor builder-side info/desync. Catches regressions where the
// walker's frame-stack discipline drifts and the schema cursor desyncs.
TEST(PrattWalker, HappyPathEmitsNoDiagnostics) {
    Tree t = parse(kInfixSchema, "a + b * -c;");
    ASSERT_NE(t.root(), InvalidNode);

    auto const& diags = t.diagnostics().all();
    for (auto const code : {
            DiagnosticCode::P_UnexpectedToken,
            DiagnosticCode::P_MissingRequiredChild,
            DiagnosticCode::P_NoAlternativeMatched,
            DiagnosticCode::P_BacktrackFailed,
            DiagnosticCode::P_UnknownToken,
            DiagnosticCode::P_PrematureEndOfInput,
            DiagnosticCode::P_AmbiguousToken,
            DiagnosticCode::P_SchemaCursorDesync,
            DiagnosticCode::P_BuilderInvariant,
            DiagnosticCode::P_MaxSpeculationDepth,
            DiagnosticCode::P_UncommittedCheckpoint,
            DiagnosticCode::P_RecoveryStalled}) {
        EXPECT_EQ(countCode(diags, code), 0u)
            << "code " << diagnosticCodeName(code);
    }
    EXPECT_FALSE(t.diagnostics().hasErrors());
}

// ── minPrecedence floor ─────────────────────────────────────────────────

// `expr.minPrecedence` lets a schema declare an outer climb floor —
// operators below it are left for an outer context (e.g. ternary
// arms, function-call argument list). Pin that the loader plumbs the
// value AND the walker honors it: with minPrecedence=20, `=` (prec 15)
// is below the floor and is not consumed, so the assignment sits
// outside the expression as a trailing token.
constexpr std::string_view kMinPrecSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "PrattMinPrec", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "+":  [{ "kind": "PlusOp" }],
    "=":  [{ "kind": "AssignOp" }],
    ";":  [{ "kind": "Semi" }]
  },
  "operators": {
    "groups": [
      { "precedence": 15, "associativity": "right", "operators": ["="] },
      { "precedence": 65, "associativity": "left",  "operators": ["+"] }
    ]
  },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "stmt" }] },
    "stmt":       { "sequence": ["expression", { "optional": { "sequence": ["AssignOp", "Identifier"] } }, "Semi"] },
    "expression": {
      "expr": {
        "atom": "operand",
        "minPrecedence": 20,
        "wrapperRules": {
          "binary":  "binaryExpr",
          "unary":   "unaryExpr",
          "postfix": "postfixExpr"
        }
      }
    },
    "operand":    { "alt": ["Identifier"] }
  }
})JSON";

TEST(PrattWalker, MinPrecedenceFloorLeavesLowerOpsUnconsumed) {
    Tree t = parse(kMinPrecSchema, "a + b = c;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // Expression consumes `a + b` (since `+` at prec 65 >= 20). `=` at
    // prec 15 < 20 is below the floor — the stmt's optional
    // `AssignOp Identifier` tail picks it up.
    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:binaryExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"+\"\n"
        "        rule:operand\n"
        "          tok:\"b\"\n"
        "    tok:\"=\"\n"
        "    tok:\"c\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// ── AltChoice → RuleLeaf expr-rule routing ──────────────────────────────
//
// PA4 fixed a regression where the parser's AltChoice scan path
// (which picks one branch from an `alt` shape) would call
// `openExprFrame(expression)` directly instead of routing through
// `prattWalker->walkExpression`. The direct call skipped operator
// climbing — `return f(x);` parsed as a flat operand sequence with
// the `(` and `x)` as siblings of `f` instead of `f(x)` as a
// postfixExpr. This test isolates the AltChoice→RuleLeaf scan path
// (rather than going through `returnStmt` which carries surrounding
// machinery) so a regression reverting the `isExprRule` check at
// `parser.cpp:992` surfaces with a single failing pin.
TEST(PrattWalker, AltChoiceScanRoutesExprRuleThroughWalker) {
    // Tiny synthetic grammar with an `alt` whose ONLY branch is
    // RuleLeaf(expression). The dispatch's AltChoice path scans
    // that branch's FIRST set, finds Identifier matches, and routes
    // to RuleLeaf(expression). If the routing dispatches through
    // `prattWalker->walkExpression`, `a + b` produces a `binaryExpr`.
    // If it falls back to `openExprFrame(expression)` directly, the
    // expression rule's `expr:` content is opened without operator
    // climbing — `binaryExpr` is absent and the tree has the
    // operator + operand as flat siblings.
    constexpr std::string_view kAltSchema = R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "PrattAlt", "version": "0.1.0" },
      "tokens": {
        " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
        "+":  [{ "kind": "PlusOp"  }],
        ";":  [{ "kind": "Semi" }]
      },
      "operators": {
        "groups": [
          { "precedence": 65, "associativity": "left", "operators": ["+"] }
        ]
      },
      "shapes": {
        "root":       { "sequence": [{ "repeat": "stmt" }] },
        "stmt":       { "sequence": ["body", "Semi"] },
        "body":       { "alt": ["expression"] },
        "expression": {
          "expr": {
            "atom": "operand",
            "wrapperRules": {
              "binary":  "binaryExpr",
              "unary":   "unaryExpr",
              "postfix": "postfixExpr"
            }
          }
        },
        "operand":    { "alt": ["Identifier"] }
      }
    })JSON";

    Tree t = parse(kAltSchema, "a + b;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // Full structural pin: `body` must contain `expression` as its
    // sole rule child, and `expression` must contain `binaryExpr`
    // (i.e. the walker climbed `+` rather than building `a + b` as a
    // flat operand sequence). A pure "sawBinaryExpr" check could pass
    // accidentally if the routing picked the wrong rule that happened
    // to also produce a binaryExpr; pinning the body→expression edge
    // closes that loophole.
    constexpr std::string_view kExpected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:body\n"
        "      rule:expression\n"
        "        rule:binaryExpr\n"
        "          rule:operand\n"
        "            tok:\"a\"\n"
        "          tok:\"+\"\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), kExpected);
}

// ── recursion-depth death ───────────────────────────────────────────────

// Adversarial right-associative input — chain of `=` ops longer than
// the configured `maxExpressionDepth`. The walker fatal-aborts rather
// than risking a C++ stack overflow.
TEST(PrattWalkerDeath, ExceedingMaxExpressionDepthAborts) {
    auto loaded = GrammarSchema::loadFromText(kInfixSchema);
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;

    // 8 right-assoc `=` ops will recurse to depth 8+ — well above
    // maxExpressionDepth=4.
    auto src = SourceBuffer::fromString(
        "a = b = c = d = e = f = g = h = i;", "<deep>");
    Tokenizer tk{src, schema};
    auto [stream, _] = std::move(tk).tokenize();

    ParserConfig cfg;
    cfg.maxExpressionDepth = 4;

    // Lambda wraps the multi-statement body so the C preprocessor sees
    // a single argument to `EXPECT_DEATH` — without it, commas in
    // declarations would be interpreted as macro argument separators.
    auto run = [&] {
        Parser p{src, schema, std::move(stream), std::move(cfg)};
        (void)std::move(p).parse();
    };
    EXPECT_DEATH(run(),
        "expression recursion depth exceeded ParserConfig::maxExpressionDepth");
}

// ── 08.55: wrapperRules genericity pin ─────────────────────────────────────
//
// Drive the Pratt walker against a schema whose wrapperRules name the
// frames `bExpr`/`uExpr`/`pExpr` — distinct from the c-subset names. The
// walker reads the names from the schema's `expr.wrapperRules` block;
// any hardcoded `binaryExpr`/`unaryExpr`/`postfixExpr` somewhere would
// make this test fail because the wrapper frames in the resulting tree
// would have the wrong rule.
namespace {
constexpr std::string_view kGenericWrapSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "GenWrap", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "+":  [{ "kind": "PlusOp" }],
    ";":  [{ "kind": "Semi"   }]
  },
  "operators": {
    "groups": [
      { "precedence": 65, "associativity": "left", "operators": ["+"] }
    ]
  },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "stmt" }] },
    "stmt":       { "sequence": ["expression", "Semi"] },
    "expression": {
      "expr": {
        "atom": "operand",
        "wrapperRules": {
          "binary":  "bExpr",
          "unary":   "uExpr",
          "postfix": "pExpr"
        }
      }
    },
    "operand":    { "alt": ["Identifier"] }
  }
})JSON";
} // namespace

TEST(PrattWalker, CustomWrapperRuleNamesDriveOperatorClimb) {
    Tree t = parse(kGenericWrapSchema, "a + b;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // The wrapper frame in the resulting tree carries the SCHEMA-DECLARED
    // wrapper rule name `bExpr` — proving the walker read it from the
    // language's `expr.wrapperRules` (no hardcoded engine names).
    const std::string_view expected =
        "rule:root\n"
        "  rule:stmt\n"
        "    rule:expression\n"
        "      rule:bExpr\n"
        "        rule:operand\n"
        "          tok:\"a\"\n"
        "        tok:\"+\"\n"
        "        rule:operand\n"
        "          tok:\"b\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// F16: end-to-end genericity test combining custom wrapperRules
// (bExpr/uExpr/pExpr) AND custom numberStyle ($-prefixed hex with
// `^` exponent and a `q` float suffix). Pin that both the wrapper
// rule names AND the numeric literal kinds reach the tree, proving
// the engine has no hardcoded grammar.
namespace {
constexpr std::string_view kGenericE2eSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "GenE2E", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "+":  [{ "kind": "PlusOp" }],
    ";":  [{ "kind": "Semi"   }]
  },
  "operators": {
    "groups": [
      { "precedence": 65, "associativity": "left", "operators": ["+"] }
    ]
  },
  "numberStyle": {
    "decimal": true,
    "integerPrefixes": [
      { "prefix": "$", "radix": 16, "digits": "0-9a-fA-F" }
    ],
    "exponent":      { "letters": ["^"], "signOptional": true },
    "fractionPoint": ".",
    "floatSuffixes": ["q"],
    "emitKind":      { "integer": "IntLiteral", "float": "FloatLiteral" }
  },
  "shapes": {
    "root":       { "sequence": [{ "repeat": "stmt" }] },
    "stmt":       { "sequence": ["expression", "Semi"] },
    "expression": {
      "expr": {
        "atom": "operand",
        "wrapperRules": {
          "binary":  "bExpr",
          "unary":   "uExpr",
          "postfix": "pExpr"
        }
      }
    },
    "operand":    { "alt": ["IntLiteral", "FloatLiteral"] }
  }
})JSON";
} // namespace

TEST(PrattWalker, CustomWrapperRulesAndNumberStyleE2E) {
    // `$10 + 2.5q;` — first literal exercises the `$` hex prefix
    // (IntLiteral); second exercises the bare-decimal path with the
    // custom `.` fraction point AND the `q` float suffix (FloatLiteral).
    // The wrapper frame must be `bExpr` (NOT a hardcoded binaryExpr).
    // Schema deliberately keeps decimal=false on prefix branches —
    // bare-decimal path is enabled by the fractional/exponent presence.
    Tree t = parse(kGenericE2eSchema, "$10 + 2.5q;");
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors())
        << "expected clean parse; first diag: "
        << (t.diagnostics().all().empty()
                ? ""
                : t.diagnostics().all()[0].actual);
    const auto pp = prettyPrint(t);
    EXPECT_NE(pp.find("rule:bExpr"), std::string::npos)
        << "expected the custom wrapper rule `bExpr` in the tree";
    EXPECT_NE(pp.find("tok:\"$10\""), std::string::npos)
        << "expected `$10` to tokenize as a single IntLiteral";
    EXPECT_NE(pp.find("tok:\"2.5q\""), std::string::npos)
        << "expected `2.5q` to tokenize as a single FloatLiteral";
}
