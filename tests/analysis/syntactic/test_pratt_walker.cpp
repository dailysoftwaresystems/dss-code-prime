// Pratt-walker unit pins. The walker drives operator-precedence climbing
// for `expr`-shape rules. Tests here construct inline schemas with
// targeted operator tables so each axis (left/right assoc, prefix,
// postfix, mixed precedence) is covered in isolation. The c-subset
// end-to-end pin lives in test_parser_c_subset_smoke.cpp; this file
// pins the walker's structural output on minimal grammars where the
// tree shape isn't obscured by surrounding syntax.

#include "analysis/syntactic/parser.hpp"
#include "core/substrate/large_stack_call.hpp"
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
#include <cstddef>
#include <cstdint>
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

// Inline schema whose operand can be a PARENTHESIZED expression:
// `operand = Identifier | ( expression )`. This is the HEAVIEST
// expression-recursion path — a nested paren re-enters the Pratt
// walker through the atom (parsePrimary -> parseUntilFrameDepth ->
// stepOnce -> walkExpression -> parseExpressionAt), so it exercises
// the depth guard's chokepoint on the path that consumes the most
// C++ stack per level. Used by the too-deep diagnostic+recovery pins.
constexpr std::string_view kParenSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "PrattParen", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "+":  [{ "kind": "PlusOp"  }],
    "(":  [{ "kind": "ParenOpen",  "opensScope": "Paren" }],
    ")":  [{ "kind": "ParenClose", "closesScope": true   }],
    ";":  [{ "kind": "Semi" }]
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
          "binary":  "binaryExpr",
          "unary":   "unaryExpr",
          "postfix": "postfixExpr"
        }
      }
    },
    "operand": {
      "alt": [
        "Identifier",
        { "sequence": ["ParenOpen", "expression", "ParenClose"] }
      ]
    }
  }
})JSON";

// Inline schema with a RIGHT-assoc ternary `?:` (mirrors c-subset's
// operator-table shape). The ternary else-clause recurses through
// parseExpressionAt at the operator's own precedence — the path a deep
// `a?b:a?b:...:c` chain stresses. Declares the `ternary` wrapper rule so
// the `?` participates (without it the climb drops `?` to the parent).
constexpr std::string_view kTernarySchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "PrattTernary", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "?":  [{ "kind": "QuestionOp" }],
    ":":  [{ "kind": "Colon" }],
    ";":  [{ "kind": "Semi" }]
  },
  "operators": {
    "groups": [
      { "precedence": 16, "associativity": "right", "arity": "ternary", "operators": ["?"], "middle": ":" }
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
          "postfix": "postfixExpr",
          "ternary": "ternaryExpr"
        }
      }
    },
    "operand":    { "alt": ["Identifier"] }
  }
})JSON";

// Inline schema with a single postfix operator `?`. C-subset ships its
// own postfix ops (`++ -- ( [ . ->`), but this minimal grammar pins
// the SIMPLE single-token postfix arm in isolation (c-subset's are
// grouped/follower forms wrapped in surrounding declaration syntax).
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

// First INTERNAL node whose rule matches `ruleName` (pre-order by id).
// Returns an invalid NodeId when absent.
[[nodiscard]] NodeId findFirstNodeWithRule(Tree const& t,
                                           std::string_view ruleName) {
    const auto ruleId = t.rules().find(ruleName);
    if (!ruleId.valid()) return NodeId{};
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) {
            return id;
        }
    }
    return NodeId{};
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
// nests LEFT, structurally: `(a + b) + c`. The walker consumes the
// operator table's declared associativity when picking the RHS climb
// floor (`prec + 1` for left), so the chain wraps iteratively and the
// nesting IS the evaluation order — no downstream pass re-derives it.
TEST(PrattWalker, SamePrecLeftAssocChainNestsLeftward) {
    Tree t = parse(kInfixSchema, "a + b + c;");
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
        "          tok:\"+\"\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "        tok:\"+\"\n"
        "        rule:operand\n"
        "          tok:\"c\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Mixed SAME-precedence left-assoc ops `a - b + c;` (kInfixSchema puts
// `-` and `+` in one prec-65 left group, like C) — `(a - b) + c`. The
// shape that miscompiled under the right-recursive design (10-3+1 = 6
// instead of 8).
TEST(PrattWalker, MixedSamePrecLeftAssocOpsNestLeftward) {
    Tree t = parse(kInfixSchema, "a - b + c;");
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
        "          tok:\"-\"\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "        tok:\"+\"\n"
        "        rule:operand\n"
        "          tok:\"c\"\n"
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

// Schema that OMITS `associativity` on an infix group. The loader
// defaults the field to `OperatorAssoc::None`; the walker's contract
// is that None behaves structurally LEFT (rhsMin = prec + 1) — an
// omitted declaration must never silently produce right-recursive
// nesting. (The postfix schemas above also omit it, but associativity
// is moot for postfix — this is the INFIX omission pin.)
namespace {
constexpr std::string_view kOmittedAssocSchema = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "PrattNoAssoc", "version": "0.1.0" },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "+":  [{ "kind": "PlusOp" }],
    ";":  [{ "kind": "Semi"   }]
  },
  "operators": {
    "groups": [
      { "precedence": 65, "operators": ["+"] }
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
} // namespace

TEST(PrattWalker, OmittedAssociativityDefaultsToStructurallyLeft) {
    Tree t = parse(kOmittedAssocSchema, "a + b + c;");
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
        "          tok:\"+\"\n"
        "          rule:operand\n"
        "            tok:\"b\"\n"
        "        tok:\"+\"\n"
        "        rule:operand\n"
        "          tok:\"c\"\n"
        "    tok:\";\"\n";
    EXPECT_EQ(prettyPrint(t), expected);
}

// Long LEFT-assoc chain far beyond maxExpressionDepth (default 256):
// 2000 `+` continuations. The wrap-in-place climb is ITERATIVE for
// left chains — depth stays O(1) per op and the parse is clean. (The
// right-assoc counterpart deliberately still aborts — see the
// PrattWalkerDeath test below.)
TEST(PrattWalker, LongLeftAssocChainParsesIteratively) {
    std::string src = "a";
    for (int i = 0; i < 2000; ++i) src += " + b";
    src += ";";
    Tree t = parse(kInfixSchema, std::move(src));
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_FALSE(t.diagnostics().hasErrors());

    // 2000 ops → exactly 2000 binaryExpr wrappers (left-nested).
    const auto binRule = t.rules().find("binaryExpr");
    ASSERT_TRUE(binRule.valid());
    std::size_t wrappers = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == binRule.v) {
            ++wrappers;
        }
    }
    EXPECT_EQ(wrappers, 2000u);
}

// Trivia hold-then-place: a same-prec chain with whitespace AND with
// the operator flush against the operands must produce the SAME
// AST-mode shape (trivia placement can't perturb wrap targets), and
// the CST leaf stream must cover the source byte-for-byte.
TEST(PrattWalker, SpacedChainMatchesUnspacedShapeAndKeepsLeafOrder) {
    Tree spaced   = parse(kInfixSchema, "a  +  b\t+ c;");
    Tree unspaced = parse(kInfixSchema, "a+b+c;");
    ASSERT_NE(spaced.root(), InvalidNode);
    ASSERT_NE(unspaced.root(), InvalidNode);
    EXPECT_FALSE(spaced.diagnostics().hasErrors());
    EXPECT_FALSE(unspaced.diagnostics().hasErrors());
    // AST-mode prettyPrint skips EmptySpace leaves → identical shapes.
    EXPECT_EQ(prettyPrint(spaced), prettyPrint(unspaced));

    // Full-fidelity pin: concatenating EVERY leaf in CST order must
    // reproduce the source text exactly (trivia held at the climb top
    // is re-placed without loss or reordering).
    std::string rebuilt;
    walkPreOrder(TreeCursor{spaced, spaced.root(), CursorMode::Cst},
                 [&](TreeCursor const& c) {
        const auto id = c.current();
        if (spaced.kind(id) != NodeKind::Internal) rebuilt += spaced.text(id);
    });
    EXPECT_EQ(rebuilt, "a  +  b\t+ c;");
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

// ── recursion-depth guard: positioned diagnostic + recovery ────────

// Helper: parse `source` under `schema` with an EXPLICIT expression-depth
// cap, returning the produced tree. Drives the REAL parse path (no death
// expected) so the depth guard's diagnostic + recovery are observable on
// the finished tree.
[[nodiscard]] Tree parseWithCap(std::string_view schemaText,
                                std::string source, std::size_t cap) {
    auto h = loadAndTokenize(schemaText, std::move(source));
    ParserConfig cfg;
    cfg.maxExpressionDepth = cap;
    Parser p{h.src, h.schema, std::move(h.stream), std::move(cfg)};
    auto result = std::move(p).parse();
    return std::move(result.tree);
}

// Structural pin (host-INDEPENDENT): an adversarial right-associative
// chain of `=` ops recurses past a LOWERED cap. The guard FAILS LOUD with
// a positioned `P_ExpressionTooDeep` at the offending token and RECOVERS
// (no fatal-abort, no stack overflow) — the produced tree carries exactly
// ONE such diagnostic and flags HasError. RED-on-disable: deleting the
// guard makes this recurse to a C++ stack overflow (crash) OR emit zero
// P_ExpressionTooDeep — either way the assertions below fail.
TEST(PrattWalker, ExceedingMaxExpressionDepthDiagnosesAndRecovers) {
    // 8 right-assoc `=` ops recurse to depth 8+ — well above the cap=4.
    Tree t = parseWithCap(kInfixSchema,
                          "a = b = c = d = e = f = g = h = i;", 4);

    // The parse RETURNED (did not abort) and built a tree.
    ASSERT_NE(t.root(), InvalidNode);
    // Exactly one too-deep diagnostic — the deepest frame trips the guard
    // once; parents resume their climb on a non-operator peek and do not
    // re-trip. (The primary RED-on-disable lever.)
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_ExpressionTooDeep), 1u);
    // HasError propagated up through the wrapper frames via the Error leaf.
    EXPECT_TRUE(t.diagnostics().hasErrors());
}

// The HEADLINE pin: a pathologically deep PAREN nest — 1000 levels, far
// past the DEFAULT cap (256) and far past the C++ stack-overflow depth —
// must yield the POSITIONED diagnostic, NOT a raw stack overflow / rc-127 /
// hang. Paren is the heaviest recursion path (atom re-entry through the
// frame machinery). Runs at the DEFAULT cap (no lowering) to prove the
// shipped configuration is crash-safe.
//
// D-PARSE-DEEP-FRONTEND-STACK: at the raised default cap (256) the guard
// deliberately lets the parser build a ~256-deep tree before firing — which
// needs the large worker stack the production driver runs the parse on
// (Program::compileFiles wraps the CU build via callOnLargeStack). So this
// pin runs the parse on that SAME 64 MiB stack: it proves the guard fires
// within the production stack budget, exactly as it does end-to-end. (The
// shallower cap=4 variants below build tiny trees and need no wrap.)
TEST(PrattWalker, DeeplyNestedParensDiagnoseAtDefaultCapWithoutCrashing) {
    std::string src = "x";
    constexpr int kDepth = 1000;
    for (int i = 0; i < kDepth; ++i) src = "(" + src + ")";
    src += ";";
    // DEFAULT cap (ParserConfig default) — not lowered. Built on the large
    // stack, mirroring the production deep-parse path.
    Tree t = dss::substrate::callOnLargeStack(
        dss::substrate::kDeepRecursionStackBytes,
        [&] { return parse(kParenSchema, std::move(src)); });

    ASSERT_NE(t.root(), InvalidNode);
    // At least one positioned too-deep diagnostic surfaced (the guard
    // fired before the stack overflowed).
    auto const& diags = t.diagnostics().all();
    ASSERT_GE(countCode(diags, DiagnosticCode::P_ExpressionTooDeep), 1u);
    EXPECT_TRUE(t.diagnostics().hasErrors());

    // The diagnostic is POSITIONED at a real `(` token — line 1, and a
    // column at or before the innermost paren (depth `kDepth`). The exact
    // column is the cap-th `(` (1-based), which is well within the run.
    for (auto const& d : diags) {
        if (d.code != DiagnosticCode::P_ExpressionTooDeep) continue;
        const auto lc = t.source().lineCol(d.span.start());
        EXPECT_EQ(lc.line, 1u);
        EXPECT_GE(lc.column, 1u);
        EXPECT_LE(lc.column, static_cast<std::uint32_t>(kDepth));
        // The offending lexeme is a `(`.
        EXPECT_EQ(t.source().slice(d.span), "(");
    }
}

// Multi-form coverage: deep PREFIX (`- - - ... x`) trips the SAME guard.
// Prefix recurses through `parsePrimary`'s prefix arm into
// parseExpressionAt at the operator's own precedence — a distinct path
// from parens/infix, proving the chokepoint covers it too. Asserts >=1
// (not exactly 1): a long prefix run whose tail exceeds the recovery
// scan window (`maxSyncScanTokens`) can re-trip once more as the stack
// unwinds onto a still-pending prefix operand — still fail-loud + no
// crash, just the same honest diagnostic again on pathological input.
TEST(PrattWalker, DeepPrefixChainDiagnosesAndRecovers) {
    std::string src;
    for (int i = 0; i < 40; ++i) src += "- ";   // 40 spaced unary minuses
    src += "a;";
    Tree t = parseWithCap(kInfixSchema, std::move(src), 4);
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_GE(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_ExpressionTooDeep), 1u);
    EXPECT_TRUE(t.diagnostics().hasErrors());
}

// Multi-form coverage: a deep RIGHT-assoc TERNARY chain
// (`a?a:a?a:...:a`) trips the SAME guard. The ternary else-clause
// recurses through parseExpressionAt at the operator's precedence — the
// right-nesting path. Pinned at a lowered cap so the guard fires well
// before any stack pressure.
TEST(PrattWalker, DeepTernaryChainDiagnosesAndRecovers) {
    std::string src = "a";
    for (int i = 0; i < 20; ++i) src += "?a:a";   // 20 nested ternaries
    src += ";";
    Tree t = parseWithCap(kTernarySchema, std::move(src), 4);
    ASSERT_NE(t.root(), InvalidNode);
    EXPECT_GE(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_ExpressionTooDeep), 1u);
    EXPECT_TRUE(t.diagnostics().hasErrors());
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

// ── D-PARSE-DEEP-NEST-RECURSION-MEMORY: flat-descent depth pin (SF-1) ──────
//
// A ~3000-deep RIGHT-nested expression (`a=a=…=a`, right-assoc `=`) parses to
// completion on the DEFAULT/normal test stack (NO large-stack wrapper). The
// right-assoc RHS arm is the deepest direct `parseExpressionAt` re-entry — the
// SF-1 flattening turns it into a heap work-stack push, so the descent carries
// FLAT O(1) host-stack cost and clears thousands of levels. The RECURSIVE form
// overflowed the normal stack at a few hundred levels (exactly why the
// 1000-deep PAREN headline pin runs on a 64 MiB worker stack); this pin
// deliberately runs OFF that worker stack so reverting the flattening
// re-introduces the host recursion and overflows here.
//
// RED-on-disable: restore the recursive `parseExpressionAt` re-entries → this
// 3000-deep parse recurses 3000 host frames on the default stack → stack
// overflow (crash). It also asserts the exact RIGHT-nesting tree shape (depth +
// leaf order), so a flattening that parsed without crashing but produced the
// WRONG structure (e.g. left-nested, or a dropped level) fails too — not merely
// "didn't crash".
//
// The cap is raised ABOVE the nesting depth so the SEMANTIC cap does not fire
// (this pin proves the FLAT descent clears the depth, not the guard); the
// separate too-deep pins above prove the cap still fires at its configured
// point.
TEST(PrattWalker, DeepRightAssocChainParsesFlatOnNormalStack) {
    constexpr int kOps = 3000;   // 3000 `=` ops → 3000 right-nested binaryExpr

    // `a=a=...=a;` — kInfixSchema's `=` is right-assoc, lowest precedence; its
    // operand is a bare Identifier (no paren re-entry), so the chain recurses
    // PURELY through the infix-RHS arm that SF-1 flattens.
    std::string source = "a";
    for (int i = 0; i < kOps; ++i) source += "=a";
    source += ";";

    // Raise the cap above the depth so the parse runs to completion; run on the
    // DEFAULT stack (no callOnLargeStack) — the flatness is the whole point.
    Tree t = parseWithCap(kInfixSchema, std::move(source), kOps + 1000);

    ASSERT_NE(t.root(), InvalidNode);
    // CLEAN: no overflow, no diagnostics, no spurious P_ExpressionTooDeep.
    EXPECT_FALSE(t.diagnostics().hasErrors());
    EXPECT_EQ(countCode(t.diagnostics().all(),
                        DiagnosticCode::P_ExpressionTooDeep), 0u);

    // Exactly kOps binaryExpr wrappers (one per `=`).
    const auto binRule = t.rules().find("binaryExpr");
    const auto operandRule = t.rules().find("operand");
    ASSERT_TRUE(binRule.valid());
    ASSERT_TRUE(operandRule.valid());
    std::size_t wrappers = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == binRule.v) {
            ++wrappers;
        }
    }
    EXPECT_EQ(wrappers, static_cast<std::size_t>(kOps));

    // Tree-SHAPE pin (not just "didn't crash"): walk the RIGHT spine. The
    // `expression` rule's child is the outermost binaryExpr; descend its LAST
    // non-trivia child exactly kOps times — each step must be a binaryExpr
    // until the innermost, whose tail is the bare `operand`. A left-nested or
    // level-dropping mis-parse breaks the count/shape here.
    const NodeId exprNode = findFirstNodeWithRule(t, "expression");
    ASSERT_TRUE(exprNode.valid());
    auto lastInternalChild = [&](NodeId n) -> NodeId {
        NodeId out{};
        for (NodeId c : t.children(n)) {
            if (t.kind(c) == NodeKind::Internal) out = c;   // last internal wins
        }
        return out;
    };
    NodeId cur = lastInternalChild(exprNode);   // outermost binaryExpr
    int spine = 0;
    while (cur.valid() && t.kind(cur) == NodeKind::Internal
           && t.rule(cur).v == binRule.v) {
        ++spine;
        cur = lastInternalChild(cur);
    }
    // kOps binaryExpr nodes on the right spine, terminating in the operand.
    EXPECT_EQ(spine, kOps);
    ASSERT_TRUE(cur.valid());
    EXPECT_EQ(t.rule(cur).v, operandRule.v);

    // Leaf-order fidelity: concatenating EVERY CST leaf reproduces the source
    // byte-for-byte (the flat descent preserves the trivia/operator/operand
    // emission order — no reordering across the work-stack push/pop).
    std::string rebuilt;
    walkPreOrder(TreeCursor{t, t.root(), CursorMode::Cst},
                 [&](TreeCursor const& c) {
        const auto id = c.current();
        if (t.kind(id) != NodeKind::Internal) rebuilt += t.text(id);
    });
    std::string expectedSrc = "a";
    for (int i = 0; i < kOps; ++i) expectedSrc += "=a";
    expectedSrc += ";";
    EXPECT_EQ(rebuilt, expectedSrc);
}
