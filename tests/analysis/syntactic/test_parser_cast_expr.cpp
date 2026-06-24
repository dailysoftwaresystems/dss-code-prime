// FC2 Part A — C-style cast `(type)expr` parser tier.
//
// Pins (1) the castExpr/castOperand grammar shapes + their precedence
// outcomes (the castOperand expr-rule re-enters the Pratt walker at
// PREFIX precedence, so postfix binds INSIDE the cast and binary binds
// OUTSIDE), and (2) the lone-`(Identifier)` type-name commit triage:
//   rule 1 — non-lone-identifier type forms commit unconditionally;
//   rule 2 — a binder-sketch TYPE hit commits;
//   rule 3 — a binder-sketch VALUE hit rolls back (shadowing-aware);
//   rule 4 — an UNKNOWN name commits iff the follower token is not an
//            infix/postfix/ternary operator, else rolls back AND
//            records an AmbiguousTypeNameCandidate on the ParseResult.
//
// Every shape assertion pins the EXACT subtree nesting via
// `prettyPrintSubtree` (the house pattern from test_parser_c_subset_smoke)
// rather than "rule appears somewhere".

#include "analysis/syntactic/parser.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

[[nodiscard]] ParseResult parseCSubset(std::string source,
                                       ParserConfig config = {}) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    EXPECT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src = SourceBuffer::fromString(std::move(source), "<cast-expr-test>");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             std::move(config), std::move(lexDiags)};
    return std::move(p).parse();
}

[[nodiscard]] NodeId findFirstNodeWithRule(Tree const& t,
                                           std::string_view ruleName) {
    if (!t.hasSchema()) return NodeId{};
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return NodeId{};
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) {
            return id;
        }
    }
    return NodeId{};
}

[[nodiscard]] std::size_t countNodesWithRule(Tree const& t,
                                             std::string_view ruleName) {
    if (!t.hasSchema()) return 0;
    const auto ruleId = t.schema().rules().find(ruleName);
    if (!ruleId.valid()) return 0;
    std::size_t n = 0;
    for (std::uint32_t i = 1; i < t.nodeCount(); ++i) {
        const NodeId id{i};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == ruleId.v) ++n;
    }
    return n;
}

// rule:NAME / tok:"TEXT" subtree rendering — exact-nesting pins.
[[nodiscard]] std::string prettyPrintSubtree(Tree const& t, NodeId root) {
    std::string out;
    if (!root.valid()) return out;
    int baseDepth = -1;
    walkPreOrder(TreeCursor{t, root, CursorMode::Ast},
                 [&](TreeCursor const& c) {
        if (baseDepth < 0) baseDepth = c.depth();
        const int d = c.depth() - baseDepth;
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

} // namespace

// ── A1: shape + precedence battery ─────────────────────────────────────

// `(int)x` — the canonical keyword cast: castExpr with the type-ref and
// the castOperand wrapping the identifier. Exact nesting pinned.
TEST(ParserCastExpr, KeywordCastExactShape) {
    auto r = parseCSubset("int main() { return (int)x; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    EXPECT_EQ(prettyPrintSubtree(t, cast),
              "rule:castExpr\n"
              "  tok:\"(\"\n"
              "  rule:castTypeRef\n"
              "    rule:castTypeBase\n"
              "      rule:typeSpecifierSeq\n"
              "        tok:\"int\"\n"
              "  tok:\")\"\n"
              "  rule:castOperand\n"
              "    rule:operand\n"
              "      tok:\"x\"\n");
}

// `(char*)p` — pointer star in the type → triage rule 1 (not a lone
// identifier) commits without consulting the sketch.
TEST(ParserCastExpr, PointerCastParses) {
    auto r = parseCSubset("int main() { return (char*)p; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    EXPECT_EQ(prettyPrintSubtree(t, cast),
              "rule:castExpr\n"
              "  tok:\"(\"\n"
              "  rule:castTypeRef\n"
              "    rule:castTypeBase\n"
              "      rule:typeSpecifierSeq\n"
              "        tok:\"char\"\n"
              "    tok:\"*\"\n"
              "  tok:\")\"\n"
              "  rule:castOperand\n"
              "    rule:operand\n"
              "      tok:\"p\"\n");
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// `(struct S*)q` — struct-tag type base.
TEST(ParserCastExpr, StructPointerCastParses) {
    auto r = parseCSubset("int main() { return (struct S*)q; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    EXPECT_NE(findFirstNodeWithRule(t, "structTypeRef"), InvalidNode);
}

// `(const long)v` — const-qualified keyword base.
TEST(ParserCastExpr, ConstQualifiedCastParses) {
    auto r = parseCSubset("int main() { return (const long)v; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
}

// `-(int)x` — prefix minus OUTSIDE the cast: unaryExpr wraps the cast.
TEST(ParserCastExpr, PrefixMinusBindsOutsideCast) {
    auto r = parseCSubset("int main() { return -(int)x; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId unary = findFirstNodeWithRule(t, "unaryExpr");
    ASSERT_TRUE(unary.valid());
    EXPECT_EQ(prettyPrintSubtree(t, unary),
              "rule:unaryExpr\n"
              "  tok:\"-\"\n"
              "  rule:operand\n"
              "    rule:castExpr\n"
              "      tok:\"(\"\n"
              "      rule:castTypeRef\n"
              "        rule:castTypeBase\n"
              "          rule:typeSpecifierSeq\n"
              "            tok:\"int\"\n"
              "      tok:\")\"\n"
              "      rule:castOperand\n"
              "        rule:operand\n"
              "          tok:\"x\"\n");
}

// `(int)(long)x` — casts chain right-to-left: the outer castOperand
// contains the inner castExpr.
TEST(ParserCastExpr, NestedCastChains) {
    auto r = parseCSubset("int main() { return (int)(long)x; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_EQ(countNodesWithRule(t, "castExpr"), 2u);
    const NodeId outer = findFirstNodeWithRule(t, "castExpr");
    EXPECT_EQ(prettyPrintSubtree(t, outer),
              "rule:castExpr\n"
              "  tok:\"(\"\n"
              "  rule:castTypeRef\n"
              "    rule:castTypeBase\n"
              "      rule:typeSpecifierSeq\n"
              "        tok:\"int\"\n"
              "  tok:\")\"\n"
              "  rule:castOperand\n"
              "    rule:operand\n"
              "      rule:castExpr\n"
              "        tok:\"(\"\n"
              "        rule:castTypeRef\n"
              "          rule:castTypeBase\n"
              "            rule:typeSpecifierSeq\n"
              "              tok:\"long\"\n"
              "        tok:\")\"\n"
              "        rule:castOperand\n"
              "          rule:operand\n"
              "            tok:\"x\"\n");
}

// `(int)x[2]` == (int)(x[2]) — postfix indexing binds INSIDE the cast
// operand (precedence 100 ≥ the castOperand's minPrecedence 90).
TEST(ParserCastExpr, PostfixIndexBindsInsideCast) {
    auto r = parseCSubset("int main() { return (int)x[2]; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    EXPECT_EQ(prettyPrintSubtree(t, cast),
              "rule:castExpr\n"
              "  tok:\"(\"\n"
              "  rule:castTypeRef\n"
              "    rule:castTypeBase\n"
              "      rule:typeSpecifierSeq\n"
              "        tok:\"int\"\n"
              "  tok:\")\"\n"
              "  rule:castOperand\n"
              "    rule:postfixExpr\n"
              "      rule:operand\n"
              "        tok:\"x\"\n"
              "      tok:\"[\"\n"
              "      rule:expression\n"
              "        rule:operand\n"
              "          tok:\"2\"\n"
              "      tok:\"]\"\n");
}

// `(int)f(a)` — a call binds inside the cast operand too.
TEST(ParserCastExpr, PostfixCallBindsInsideCast) {
    auto r = parseCSubset("int main() { return (int)f(a); }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    // The call's postfixExpr must sit BELOW the castOperand.
    const std::string pretty = prettyPrintSubtree(t, cast);
    EXPECT_NE(pretty.find("rule:castOperand\n"
                          "    rule:postfixExpr\n"),
              std::string::npos)
        << pretty;
}

// `(int)a + b` == ((int)a) + b — the binary `+` (precedence 65 < 90)
// stops the cast operand: the binaryExpr's LHS operand holds the cast,
// its RHS holds `b`.
TEST(ParserCastExpr, BinaryPlusBindsOutsideCast) {
    auto r = parseCSubset("int main() { return (int)a + b; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId bin = findFirstNodeWithRule(t, "binaryExpr");
    ASSERT_TRUE(bin.valid());
    EXPECT_EQ(prettyPrintSubtree(t, bin),
              "rule:binaryExpr\n"
              "  rule:operand\n"
              "    rule:castExpr\n"
              "      tok:\"(\"\n"
              "      rule:castTypeRef\n"
              "        rule:castTypeBase\n"
              "          rule:typeSpecifierSeq\n"
              "            tok:\"int\"\n"
              "      tok:\")\"\n"
              "      rule:castOperand\n"
              "        rule:operand\n"
              "          tok:\"a\"\n"
              "  tok:\"+\"\n"
              "  rule:operand\n"
              "    tok:\"b\"\n");
}

// `(int){ 1 }` — the compound literal still wins its probe (the cast
// probe fails structurally on `{`, which no expression operand starts).
TEST(ParserCastExpr, CompoundLiteralStillParses) {
    auto r = parseCSubset("int main() { int x = (int){ 1 }; return x; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "compoundLiteralExpr").valid());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
}

// `(x)` with nothing castable after the closer — plain parenthesized
// expression (the cast probe fails structurally: `;` starts no operand).
TEST(ParserCastExpr, PlainParenStaysParenExpr) {
    auto r = parseCSubset("int main() { return (x); }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "parenExpr").valid());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// `(a) + b` — `+` is binary-only (no prefix entry), so `+b` can never
// start a cast operand: the probe fails STRUCTURALLY, no candidate is
// recorded, and the value reading parses as parenExpr + add.
TEST(ParserCastExpr, ParenPlusStaysValueReadingWithNoCandidate) {
    auto r = parseCSubset("int main() { return (a) + b; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "parenExpr").valid());
    EXPECT_TRUE(findFirstNodeWithRule(t, "binaryExpr").valid());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// A cast operand long enough to dwarf the OLD 8-lookahead probe budget
// (8×16 = 128 tokens): the raised alt lookahead (64 → 1024-token budget)
// must cover real-world cast operands. ~200 tokens of chained adds.
TEST(ParserCastExpr, LongCastOperandSurvivesProbeBudget) {
    std::string expr = "(int)(a0";
    for (int i = 1; i < 50; ++i) {
        expr += " + a" + std::to_string(i);
    }
    expr += ")";
    auto r = parseCSubset("int main() { return " + expr + "; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
}

// ── A2: lone-(Identifier) triage — binder sketch ───────────────────────

// Rule 2: a file-scope typedef name is KNOWN TYPE → `(T)-x` commits the
// cast (the `-x` parses as the cast operand's unary minus).
TEST(ParserCastExpr, TypedefNameCastCommits) {
    auto r = parseCSubset(
        "typedef int T;\n"
        "int main() { return (T)-x; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    EXPECT_EQ(prettyPrintSubtree(t, cast),
              "rule:castExpr\n"
              "  tok:\"(\"\n"
              "  rule:castTypeRef\n"
              "    rule:castTypeBase\n"
              "      tok:\"T\"\n"
              "  tok:\")\"\n"
              "  rule:castOperand\n"
              "    rule:unaryExpr\n"
              "      tok:\"-\"\n"
              "      rule:operand\n"
              "        tok:\"x\"\n");
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// Rule 3: a local VARIABLE of the same shape is KNOWN VALUE → `(a)-b`
// stays subtraction (parenExpr LHS, minus, b).
TEST(ParserCastExpr, KnownValueStaysSubtraction) {
    auto r = parseCSubset(
        "int main() { int a; return (a)-b; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    const NodeId bin = findFirstNodeWithRule(t, "binaryExpr");
    ASSERT_TRUE(bin.valid());
    EXPECT_EQ(prettyPrintSubtree(t, bin),
              "rule:binaryExpr\n"
              "  rule:operand\n"
              "    rule:parenExpr\n"
              "      tok:\"(\"\n"
              "      rule:expression\n"
              "        rule:operand\n"
              "          tok:\"a\"\n"
              "      tok:\")\"\n"
              "  tok:\"-\"\n"
              "  rule:operand\n"
              "    tok:\"b\"\n");
    // A KNOWN value is not ambiguous — no candidate.
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// Rule 4 (operator follower): an UNKNOWN lone identifier followed by an
// operator-viable token rolls back to the value reading AND records the
// candidate for the cross-file oracle.
TEST(ParserCastExpr, UnknownNameWithOperatorFollowerRecordsCandidate) {
    auto r = parseCSubset("int main() { return (q)-x; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(findFirstNodeWithRule(t, "parenExpr").valid());
    ASSERT_EQ(r.typeNameCandidates.size(), 1u);
    EXPECT_EQ(r.typeNameCandidates[0].name, "q");
    // The span must point at the identifier `q` itself.
    EXPECT_EQ(t.source().slice(r.typeNameCandidates[0].span), "q");
}

// Rule 4 (non-operator follower): `(q) z` — the value reading could not
// continue (`z` after `(q)` is no operator), so the CAST is the only
// viable parse and commits; semantic later diagnoses if `q` isn't a type.
TEST(ParserCastExpr, UnknownNameWithNonOperatorFollowerCommitsCast) {
    auto r = parseCSubset("int main() { (q) z; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// `(T)(x)` cast-vs-call, type direction: T is a typedef → cast of the
// parenthesized expression.
TEST(ParserCastExpr, TypedefThenParensIsCast) {
    auto r = parseCSubset(
        "typedef int T;\n"
        "int main() { return (T)(x); }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    const std::string pretty = prettyPrintSubtree(t, cast);
    EXPECT_NE(pretty.find("rule:castOperand\n"
                          "    rule:operand\n"
                          "      rule:parenExpr\n"),
              std::string::npos)
        << pretty;
}

// `(f)(x)` cast-vs-call, value direction: f is a known variable → the
// value reading (a call shape: postfixExpr with `(` ... `)`).
TEST(ParserCastExpr, KnownValueThenParensIsCall) {
    auto r = parseCSubset(
        "int main() { int f; return (f)(x); }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    const NodeId call = findFirstNodeWithRule(t, "postfixExpr");
    ASSERT_TRUE(call.valid());
    const std::string pretty = prettyPrintSubtree(t, call);
    EXPECT_NE(pretty.find("rule:parenExpr"), std::string::npos) << pretty;
    EXPECT_NE(pretty.find("rule:argList"), std::string::npos) << pretty;
}

// Shadowing: file-scope typedef T shadowed by a local `int T;` — after
// the shadow, `(T)-1` is the VALUE reading (subtraction), exactly like
// the analyzer's scope chain would resolve it.
TEST(ParserCastExpr, LocalValueShadowsFileScopeTypedef) {
    auto r = parseCSubset(
        "typedef int T;\n"
        "int main() { int T; return (T)-1; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(findFirstNodeWithRule(t, "binaryExpr").valid());
}

// Param shadowing: a parameter binds into the FUNCTION's scope
// (funcDefTail opens one scope over params + body), so a param named T
// shadows the file-scope typedef INSIDE the body.
TEST(ParserCastExpr, ParamShadowsFileScopeTypedefInsideBody) {
    auto r = parseCSubset(
        "typedef int T;\n"
        "int f(int T) { return (T)-1; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(findFirstNodeWithRule(t, "binaryExpr").valid());
}

// Scope exit: a shadow inside a CLOSED inner block does not leak — after
// the block, T is the typedef again and the cast commits.
TEST(ParserCastExpr, ShadowDiesWithItsScope) {
    auto r = parseCSubset(
        "typedef int T;\n"
        "int main() { { int T; } return (T)-1; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "castExpr").valid());
}

// Declare-before-use (C ordering): a typedef AFTER the use site is not
// visible at the use — `(T)-1` before the typedef line reads T as
// unknown, follower `-` is an operator → value reading + candidate.
TEST(ParserCastExpr, TypedefAfterUseSiteIsNotVisible) {
    auto r = parseCSubset(
        "int main() { return (T)-1; }\n"
        "typedef int T;");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    ASSERT_EQ(r.typeNameCandidates.size(), 1u);
    EXPECT_EQ(r.typeNameCandidates[0].name, "T");
}

// ── ParseResult sidecars ────────────────────────────────────────────────

// The global-scope TYPE export surface: typedefs + struct/union/enum
// tags at file scope, in binding order. (The CU oracle harvests this.)
TEST(ParserCastExpr, GlobalTypeNamesExported) {
    auto r = parseCSubset(
        "typedef int MyT;\n"
        "struct S { int x; };\n"
        "int g;\n"
        "int main() { return 0; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    ASSERT_EQ(r.globalTypeNames.size(), 2u);
    EXPECT_EQ(r.globalTypeNames[0], "MyT");
    EXPECT_EQ(r.globalTypeNames[1], "S");
}

// Seeding (the oracle's reparse channel): the SAME ambiguous source that
// records a candidate above commits the CAST when the name is seeded as
// a global type — this is the parser half of the A3 cross-file path.
TEST(ParserCastExpr, SeededTypeNameFlipsCandidateSiteToCast) {
    ParserConfig cfg;
    cfg.seedGlobalTypeNames.push_back("q");
    auto r = parseCSubset("int main() { return (q)-x; }", std::move(cfg));
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// A toy-grammar parse (no `semantics.declarations`) exports nothing and
// records nothing — the binder sketch is inert for binder-less languages.
TEST(ParserCastExpr, BinderlessLanguageHasEmptySidecars) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src = SourceBuffer::fromString("print 1 + 2;", "<toy-sidecar>");
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream), {},
             std::move(lexDiags)};
    auto r = std::move(p).parse();
    EXPECT_TRUE(r.typeNameCandidates.empty());
    EXPECT_TRUE(r.globalTypeNames.empty());
}

// ── speculation rollback hygiene ────────────────────────────────────────

// A candidate recorded inside an ENCLOSING probe that itself rolls back
// must not leak twice: `((q)-x)` probes parenExpr around the inner
// ambiguous site; the inner candidate is recorded, the outer parenExpr
// probe COMMITS, and the Pratt walker climbs the surrounding chain
// (wrap-in-place — the primary is never re-parsed) — the net result
// must be EXACTLY ONE candidate for `q`, not a duplicate per probe.
// (This pins the probe snapshot/restore + record-after-rollback
// interplay; a leak would double-count.)
TEST(ParserCastExpr, NestedProbesYieldExactlyOneCandidate) {
    auto r = parseCSubset("int main() { return ((q)-x); }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    ASSERT_EQ(r.typeNameCandidates.size(), 1u);
    EXPECT_EQ(r.typeNameCandidates[0].name, "q");
}

// Same hygiene across a climb chain: `(q)-x-y` builds two binary wraps
// above the `(q)` primary (wrap-in-place — the ambiguous site is
// parsed exactly once) — still exactly one candidate.
TEST(ParserCastExpr, ClimbChainDoesNotDuplicateCandidates) {
    auto r = parseCSubset("int main() { return (q)-x-y; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    ASSERT_EQ(r.typeNameCandidates.size(), 1u);
    EXPECT_EQ(r.typeNameCandidates[0].name, "q");
}

// Two genuinely distinct ambiguous sites stay two candidates.
TEST(ParserCastExpr, TwoDistinctSitesRecordTwoCandidates) {
    auto r = parseCSubset("int main() { return (q)-x + (r)-y; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    ASSERT_EQ(r.typeNameCandidates.size(), 2u);
    EXPECT_EQ(r.typeNameCandidates[0].name, "q");
    EXPECT_EQ(r.typeNameCandidates[1].name, "r");
}

// Rolled-back probes leak no BINDINGS either: a failed castExpr probe
// over `(a)` (a known value) must leave `a`'s binding intact and the
// sketch consistent — proven by the SECOND lookup behaving identically
// (if the probe leaked or truncated state, the second `(a)-c` would
// flip its reading).
TEST(ParserCastExpr, RolledBackProbeLeaksNoSketchState) {
    auto r = parseCSubset(
        "int main() { int a; int s = (a)-b; return (a)-c; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_EQ(countNodesWithRule(t, "parenExpr"), 2u);
    EXPECT_TRUE(r.typeNameCandidates.empty());
}

// ── D-PARSE-DEEP-NEST-RECURSION-MEMORY: SF-2 speculation-rollback-at-depth ──
//
// THE SF-2 pin. A speculative `castExpr` probe over `(q)` (an UNKNOWN
// lone-identifier type-name site) DESCENDS the expression work-stack to a
// non-trivial depth — its `castOperand` re-enters the Pratt walker and parses
// the prefix `-` plus a deeply-nested parenthesized operand — and THEN ROLLS
// BACK (rule 4: the follower `-` is an operator, so the value reading wins).
// The tokens after the rollback (the subtraction chain, including a trailing
// `-z`) must then parse CORRECTLY.
//
// This exercises the SF-2 interaction directly: the probe is constructed WHILE
// an expression descent is in flight, descends MANY work-stack levels building
// the doomed cast, and on rollback the parser must resume EXACTLY where it was
// — the `SpeculationProbe` restores the builder/walker/tokens (and truncates
// the expression work-stack + depth counter back to the pre-probe state). A
// broken restore would mis-parse the value reading, drop the deep nesting, or
// leak a candidate.
//
// SCOPE NOTE: this proves the FLAT-descent-under-speculation interaction is
// correct end-to-end. It is NOT red-on-disable for the work-stack `.resize()`
// specifically — by construction each `walkExpression` entry drives its sub-
// stack back to baseline before any probe inspects the outcome, so the work-
// stack is already balanced at rollback and the resize is a structural no-op
// (verified: disabling it leaves this pin + the whole speculation suite green;
// see the `SpeculationProbe` dtor comment). The pin guards the BEHAVIOR (deep
// speculate → rollback → resume) so a future change that breaks that balance
// invariant is caught here.
//
// The deep nesting is bounded well under the default 256 cap so the cap never
// fires here (this pin is about the ROLLBACK, not the guard); the parse runs on
// the default stack (the flat descent makes the doomed deep probe cheap).
TEST(ParserCastExpr, DeepSpeculativeCastRollsBackAndResumesCorrectly) {
    constexpr int kDepth = 60;   // 60-deep paren operand inside the doomed cast

    // `(q) - (((…x…))) - z;` — `q` is unknown; the castExpr probe descends the
    // prefix `-` + kDepth parens, then rolls back to the value reading.
    std::string deep;
    deep.append(kDepth, '(');
    deep += "x";
    deep.append(kDepth, ')');
    auto r = parseCSubset(
        "int main() { return (q)-" + deep + "-z; }");
    auto const& t = r.tree;

    // Post-rollback the parse is CLEAN and the value reading won.
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());

    // The deep nesting SURVIVED the rollback intact: kDepth parens for the
    // operand + 1 for `(q)` itself = kDepth + 1 parenExpr nodes. A dropped
    // level (stale/leaked work-stack frame) would change this count.
    EXPECT_EQ(countNodesWithRule(t, "parenExpr"),
              static_cast<std::size_t>(kDepth + 1));

    // The rolled-back ambiguous site recorded EXACTLY ONE candidate for `q`
    // (no leak, no double-count) — the work-stack restore left the sketch
    // delta clean.
    ASSERT_EQ(r.typeNameCandidates.size(), 1u);
    EXPECT_EQ(r.typeNameCandidates[0].name, "q");

    // The tokens AFTER the rollback parsed correctly: the trailing `-z` closes
    // a left-assoc subtraction chain over the (deep) value reading. Two `-`
    // operators ⇒ two binaryExpr wraps; the climb resumed cleanly from the
    // restored state.
    EXPECT_EQ(countNodesWithRule(t, "binaryExpr"), 2u);
}

