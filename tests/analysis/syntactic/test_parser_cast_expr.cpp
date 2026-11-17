// FC2 Part A — C-style cast `(type)expr` parser tier.
//
// Pins (1) the castExpr/castOperand grammar shapes + their precedence
// outcomes (the castOperand expr-rule re-enters the Pratt walker at
// PREFIX precedence, so postfix binds INSIDE the cast and binary binds
// OUTSIDE), and (2) the lone-`(Identifier)` type-name commit triage:
//   rule 1 — non-lone-identifier type forms commit unconditionally;
//   rule 2 — a binder-sketch TYPE hit commits;
//   rule 3 — a binder-sketch VALUE hit rolls back (shadowing-aware);
//   rule 4 — an UNKNOWN name: castExpr's guard is `requireKnownType`, so the
//            cast reading is refused whatever follows and the value reading
//            is taken. The refusal is recorded (`UnknownNameRefusal`); when the
//            value reading then fails at a token the refused probe had read,
//            the parser names the identifier FIRST (S_UndeclaredIdentifier at
//            it) and the error second — the references' order.
//
// Every shape assertion pins the EXACT subtree nesting via
// `prettyPrintSubtree` (the house pattern from test_parser_c_smoke)
// rather than "rule appears somewhere".

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

[[nodiscard]] ParseResult parseC(std::string source,
                                       ParserConfig config = {}) {
    auto loaded = GrammarSchema::loadShipped("c");
    EXPECT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src = SourceBuffer::fromString(std::move(source), "<cast-expr-test>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             DiagnosticBudget::libraryDefault(),
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
    auto r = parseC("int main() { return (int)x; }");
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
    auto r = parseC("int main() { return (char*)p; }");
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
              "    rule:pointerLayer\n"
              "      tok:\"*\"\n"
              "  tok:\")\"\n"
              "  rule:castOperand\n"
              "    rule:operand\n"
              "      tok:\"p\"\n");
}

// `(struct S*)q` — struct-tag type base.
//
// TF-C101: the base node is `structSpec`, not the ref-only `structTypeRef` it was
// before. That rename IS the fix, not a side effect — `castTypeBase` now routes its
// composite arms through the c25 UNIFIED specifier so a type-name may carry a struct
// DEFINITION (`sizeof(struct { int dummy; })`, C 6.7.7p1). A body-ABSENT `structSpec`
// is still exactly a tag reference (the `isTagReference` semantics arm), so this
// cast's MEANING is unchanged; only the rule that owns the position moved. Asserting
// the node is present keeps the structural pin: were the composite arm ever dropped
// from the type-name base, this fires instead of the cast silently re-reading as a
// parenthesized value.
TEST(ParserCastExpr, StructPointerCastParses) {
    auto r = parseC("int main() { return (struct S*)q; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    EXPECT_NE(findFirstNodeWithRule(t, "structSpec"), InvalidNode);
}

// TF-C101 companion: the type-name base admits a composite DEFINITION, and it must
// still be the cast's type — the construct that motivated the unify. A struct-typed
// cast is a constraint violation downstream (a cast target must be scalar), so this
// pins PARSE structure only, which is precisely the tier the gap lived at.
TEST(ParserCastExpr, SizeofAnonymousStructDefinitionParses) {
    auto r = parseC("int main() { return (int)sizeof(struct { int dummy; }); }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_NE(findFirstNodeWithRule(t, "sizeofType"), InvalidNode);
    EXPECT_NE(findFirstNodeWithRule(t, "structBody"), InvalidNode)
        << "the inline member list must parse as the struct's BODY inside the "
           "sizeof type-name, not be rejected at the '{'";
}

// `(const long)v` — const-qualified keyword base.
TEST(ParserCastExpr, ConstQualifiedCastParses) {
    auto r = parseC("int main() { return (const long)v; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
}

// `-(int)x` — prefix minus OUTSIDE the cast: unaryExpr wraps the cast.
TEST(ParserCastExpr, PrefixMinusBindsOutsideCast) {
    auto r = parseC("int main() { return -(int)x; }");
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
    auto r = parseC("int main() { return (int)(long)x; }");
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
    auto r = parseC("int main() { return (int)x[2]; }");
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
    auto r = parseC("int main() { return (int)f(a); }");
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
    auto r = parseC("int main() { return (int)a + b; }");
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
    auto r = parseC("int main() { int x = (int){ 1 }; return x; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "compoundLiteralExpr").valid());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
}

// `(x)` with nothing castable after the closer — plain parenthesized
// expression (the cast probe fails structurally: `;` starts no operand).
TEST(ParserCastExpr, PlainParenStaysParenExpr) {
    auto r = parseC("int main() { return (x); }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "parenExpr").valid());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
}

// `(a) + b` — c12 made `+` a PREFIX operator too (unary plus), so `+b` can now
// start a cast operand: `(a)(+b)` is structurally viable. Exactly like the
// `(q)-x` precedent above, the value reading (`(a) + b`) still wins (rolls back
// to parenExpr + add, no committed castExpr because `a` isn't a known type). The
// parse succeeds, so the refusal names nothing here: the semantic tier names `a`.
TEST(ParserCastExpr, ParenPlusStaysValueReading) {
    auto r = parseC("int main() { return (a) + b; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "parenExpr").valid());
    EXPECT_TRUE(findFirstNodeWithRule(t, "binaryExpr").valid());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
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
    auto r = parseC("int main() { return " + expr + "; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
}

// ── A2: lone-(Identifier) triage — binder sketch ───────────────────────

// Rule 2: a file-scope typedef name is KNOWN TYPE → `(T)-x` commits the
// cast (the `-x` parses as the cast operand's unary minus).
TEST(ParserCastExpr, TypedefNameCastCommits) {
    auto r = parseC(
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
}

// Rule 3: a local VARIABLE of the same shape is KNOWN VALUE → `(a)-b`
// stays subtraction (parenExpr LHS, minus, b).
TEST(ParserCastExpr, KnownValueStaysSubtraction) {
    auto r = parseC(
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
    // A KNOWN value is no refusal: the sketch knows it.
}

// Rule 4 (operator follower): an UNKNOWN lone identifier followed by an
// operator-viable token takes the value reading; the parse succeeds, so the
// refusal names nothing (the semantic tier names `q`).
TEST(ParserCastExpr, UnknownNameWithOperatorFollowerKeepsTheValueReading) {
    auto r = parseC("int main() { return (q)-x; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(findFirstNodeWithRule(t, "parenExpr").valid());
}

// Rule 4 (non-operator follower): `(q) z` — the cast reading is refused (the sketch
// knows no `q`: nothing declares it), the value reading `(q)` cannot continue at `z`,
// and the parser names the refused identifier FIRST, then the error at `z` — gcc's
// pair (MEASURED 2026-11-04, probe f01: gcc 13.3.0 "'q' undeclared" at 1:15, then
// "expected ';' before 'z'" at 1:17; clang 18.1.3 names `q` only; cl C2065 'q', C2146
// before 'z', C2065 'z'). Never a committed cast.
TEST(ParserCastExpr, UnknownNameWithNonOperatorFollowerIsNamedBeforeTheFollowersError) {
    auto r = parseC("int main() { (q) z; }");
    auto const& t = r.tree;
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    auto const all = t.diagnostics().all();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].code, DiagnosticCode::S_UndeclaredIdentifier);
    EXPECT_EQ(all[0].actual, "q");
    EXPECT_EQ(t.source().slice(all[0].span), "q") << "named at the identifier itself";
    EXPECT_NE(all[1].code, DiagnosticCode::S_UndeclaredIdentifier);
    EXPECT_EQ(all[1].severity, DiagnosticSeverity::Error);
    EXPECT_EQ(t.source().slice(all[1].span), "z") << "then the follower's error, at `z`";
}

// `(T)(x)` cast-vs-call, type direction: T is a typedef → cast of the
// parenthesized expression.
TEST(ParserCastExpr, TypedefThenParensIsCast) {
    auto r = parseC(
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
    auto r = parseC(
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
    auto r = parseC(
        "typedef int T;\n"
        "int main() { int T; return (T)-1; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_TRUE(findFirstNodeWithRule(t, "binaryExpr").valid());
}

// Param shadowing: a parameter binds into its parameter list's prototype
// scope, and a DEFINITION's own parameters are carried into the scope its
// declaration opens (c's topLevelDecl / externDecl) when its body opens
// (P69 L2a2), so a param named T shadows the file-scope typedef INSIDE the
// body.
TEST(ParserCastExpr, ParamShadowsFileScopeTypedefInsideBody) {
    auto r = parseC(
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
    auto r = parseC(
        "typedef int T;\n"
        "int main() { { int T; } return (T)-1; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "castExpr").valid());
}

// Declare-before-use (C ordering): a typedef AFTER the use site is not
// visible at the use — `(T)-1` before the typedef line reads T as
// unknown, follower `-` is an operator → the value reading.
TEST(ParserCastExpr, TypedefAfterUseSiteIsNotVisible) {
    auto r = parseC(
        "int main() { return (T)-1; }\n"
        "typedef int T;");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
}

// ── what the sketch knows ────────────────────────────────────────────────

// P69 L2 (lane `lm`): a struct/union/enum TAG is no type name — it lives in the tag
// name space (C 6.2.3) and never makes its spelling one, so after the four tag
// declarations only the typedef `MyT` reads `( name )` as a cast; `(S)`, `(U)` and
// `(E)` keep the value reading (an operator follows, so each parses; the analysis
// refuses the undeclared names).
TEST(ParserCastExpr, ATagNeverMakesItsSpellingATypeName) {
    auto r = parseC(
        "typedef int MyT;\n"
        "struct S { int x; };\n"
        "union U { int y; };\n"
        "enum E { EA };\n"
        "int g;\n"
        "int main() { return (MyT)-1 + (S)-1 + (U)-1 + (E)-1; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
    EXPECT_EQ(countNodesWithRule(r.tree, "castExpr"), 1u)
        << "only the typedef MyT is a type name; a tag is none";
    EXPECT_EQ(countNodesWithRule(r.tree, "parenExpr"), 3u);
}

// ── P69 L2 (lane `lm`): WHICH NAME THE TRIAGE SEES ──────────────────────────
// The type-name triage asks the binder sketch whether `(name)` opens a type name.
// Two readings it gave wrongly, both MEASURED at eb6f22a3 (pe64, images run
// natively) against gcc 13.3.0 and clang 18.1.3 (gnu2x and c2x -pedantic-errors,
// built and run): a struct/union/enum TAG answered as an ordinary type (C 6.2.3
// gives tags their own name space), so a tag newer than a function or an object of
// its spelling turned `(name)` into a cast — `int g(int); struct g {…}; (g)(2)` and
// `int T = 44; enum T {A = 2}; (T) - A` were refused, `typedef int T; … int T = 44;
// { struct T {…}; return (T) - 2; }` RAN -2 (references: 42, every one); and a
// declarator's name was bound only at the end of its DECLARATION, while C 6.2.1p7
// starts its scope at the end of its DECLARATOR — `int T = 44, y = (T) - 2;` RAN -2
// and `typedef char T; … int T = sizeof(T);` RAN 1 (references: 42 and sizeof(int)).
// A structure MEMBER was recorded as an ordinary value likewise: `typedef int P;
// struct S { int P; char a[(P) 42]; };` was refused P_UnexpectedToken at `42`
// (MEASURED on this lane's tree; both references build it).
// The sketch now records ordinary identifiers only: neither a tag nor a member.
// RED-ON-DISABLE: a tag row's name recorded → the tag rows; a member row's name
// recorded → the member row; the slot declarator's record skipped → the declarator
// rows of the next case. The controls hold either way: a use BEFORE the object's
// declarator still reads the typedef. The two rows inside a tag's own body are
// REGRESSION pins: HEAD built and ran both (✔MEASURED, exit 42) even while it bound
// the tag there as an ordinary type.
TEST(ParserCastExpr, ATagNeverAnswersTheTypeNameTriage) {
    struct Row { char const* what; char const* src; std::size_t casts; };
    Row const rows[] = {
        {"a function, then a tag of its spelling: `(g)(2)` is a call",
         "int g(int v) { return v + 40; }\nstruct g { int a; };\n"
         "int main(void) { return (g)(2); }\n", 0},
        {"an object hides the typedef; a newer inner TAG changes nothing",
         "typedef int T;\nint main(void) { int T = 44; { struct T { int a; }; "
         "return (T) - 2; } }\n", 0},
        {"an object, then an ENUM TAG of its spelling",
         "int main(void) { int T = 44; enum T { A = 2 }; return (T) - A; }\n", 0},
        {"a tag defined inside the operand: only the operand's own cast",
         "int g(void *p) { return p ? 1 : 42; }\n"
         "int main(void) { return (g)((struct g { int a; } *)0); }\n", 1},
        // Inside its own body the tag is declared (`struct g *next;` works), and still
        // no ordinary identifier: `(g)` there is the function (both references: 42).
        {"inside its own body a tag answers no ordinary lookup: `sizeof (g)(2)` is a call",
         "int g(int v) { return v + 40; }\nstruct g { int a[sizeof (g)(2)]; };\n", 0},
        {"... and where a cast could parse: `sizeof ((g)(2))` is a call",
         "int g(int v) { return v + 40; }\nstruct g { int a[sizeof ((g)(2))]; };\n", 0},
        // A structure MEMBER is its structure's own name space (C 6.2.3) as a tag is
        // its own: `(P)` in the next member's bound casts through the typedef.
        {"a member spelled like the typedef: `(P) 42` is a cast",
         "typedef int P;\nstruct S { int P; char a[(P) 42]; };\n", 1},
        // CONTROL: a typedef with a tag of its spelling, in both orders, is a type.
        {"a tag, then a typedef of its spelling: `(T)(x)` is a cast",
         "struct T { int a; };\ntypedef int T;\n"
         "int main(void) { T x = 40; return (T)(x); }\n", 1},
        {"a typedef, then a tag of its spelling: `(T)(x)` is a cast",
         "typedef int T;\nstruct T { int a; };\n"
         "int main(void) { T x = 40; return (T)(x); }\n", 1},
    };
    for (Row const& row : rows) {
        auto r = parseC(row.src);
        EXPECT_FALSE(r.tree.diagnostics().hasErrors()) << row.what;
        EXPECT_EQ(countNodesWithRule(r.tree, "castExpr"), row.casts) << row.what;
    }
}

TEST(ParserCastExpr, ADeclaratorsNameIsInScopeFromTheEndOfItsDeclarator) {
    struct Row { char const* what; char const* src; std::size_t casts; std::size_t sizeofTypes; };
    Row const rows[] = {
        {"the next declarator of the same declaration sees the object",
         "typedef int T;\nint main(void) { int T = 44, y = (T) - 2; return y; }\n", 0, 0},
        {"its own initializer sees the object",
         "typedef char T;\nint main(void) { int T = sizeof(T); return T; }\n", 0, 0},
        {"a typedef's next declarator sees the typedef",
         "typedef int T, U[sizeof(T) * 10 + 2];\nint main(void) { return 0; }\n", 0, 1},
        {"a parameter is in scope in the body",
         "typedef int T;\nint f(int T) { return (T) - 2; }\n", 0, 0},
        {"a block-scope function declaration hides the typedef",
         "typedef int T;\nint main(void) { int T(int); return (T)(2); }\n", 0, 0},
        // CONTROLS: a use BEFORE the object's declarator reads the typedef.
        {"a use before the object's declaration is the typedef's",
         "typedef int T;\nint main(void) { int y = (T) - 2; int T = 44; return y + T; }\n",
         1, 0},
        {"a use inside the object's own declarator is the typedef's",
         "typedef int T;\nint main(void) { int T[sizeof(T)]; return 0; }\n", 0, 1},
    };
    for (Row const& row : rows) {
        auto r = parseC(row.src);
        EXPECT_FALSE(r.tree.diagnostics().hasErrors()) << row.what;
        EXPECT_EQ(countNodesWithRule(r.tree, "castExpr"), row.casts) << row.what;
        EXPECT_EQ(countNodesWithRule(r.tree, "sizeofType"), row.sizeofTypes) << row.what;
    }
}

// P69 L2a2 (the L2a review's N9): a PARAMETER of a function declarator that is not a
// definition's own has function prototype scope (C 6.2.1p4); past its declarator the typedef
// it shadowed is a type again, so `(T)(x)` is a cast. The sketch bound such a parameter into
// the enclosing block, and `(T)(x)` parsed as a call. A definition's own parameter reaches its
// body (the control rows). ✔MEASURED 2026-10-28 (WSL gcc 13.3.0 / clang 18.1.3, gnu2x and c2x
// -pedantic-errors): every cast row's program exits 42; the own-parameter call is refused.
TEST(ParserCastExpr, APrototypeParametersScopeEndsWithItsDeclarator) {
    struct Row { char const* what; char const* src; std::size_t casts; };
    Row const rows[] = {
        {"a function-pointer object's parameter",
         "typedef int T;\nint main(void) { int (*fp)(int T) = 0; int x = 42; (void)fp; "
         "return (T)(x); }\n", 2},
        {"a block-scope prototype's parameter",
         "typedef int T;\nint main(void) { int f(int T); int x = 42; return (T)(x); }\n", 1},
        {"a block-scope typedef's parameter",
         "typedef int T;\nint main(void) { typedef int (*F)(int T); F f = 0; int x = 42; "
         "(void)f; return (T)(x); }\n", 2},
        {"a cast's type name's parameter",
         "typedef int T;\nint main(void) { (void)(int (*)(int T))0; int x = 42; "
         "return (T)(x); }\n", 3},
        {"a file-scope prototype's parameter, in the next declarator",
         "typedef int T;\nint f(int T), y = sizeof((T)(1));\n", 1},
        {"a definition's RETURNED function type's parameter, in its body",
         "typedef int T;\nstatic int h(int t) { return t; }\n"
         "static int (*g(int a))(int T) { return (a == (T)(a)) ? h : 0; }\n", 1},
        // CONTROLS: a definition's OWN parameter reaches its body, in either spelling.
        {"a definition's own parameter, in its body",
         "typedef int T;\nint f(int T, int x) { return (T)(x); }\n", 0},
        {"a parenthesised definition's own parameter, in its body",
         "typedef int T;\nint (f)(int T, int x) { return (T)(x); }\n", 0},
    };
    for (Row const& row : rows) {
        auto r = parseC(row.src);
        EXPECT_FALSE(r.tree.diagnostics().hasErrors()) << row.what;
        EXPECT_EQ(countNodesWithRule(r.tree, "castExpr"), row.casts) << row.what;
    }
}

// ── P69 L2b1 (lane `lm`, the L2a2 review's A1): A DEFINITION'S OWN PARAMETERS SURVIVE THE
// PARAMETER LISTS OF THE TYPE IT RETURNS. A function returning a function pointer writes its
// own list INSIDE its declarator and the returned type's lists AFTER it; each `param` row of a
// returned list ended its declarator and its row by dropping every list the sketch had noted, the
// definition's own included, so its body saw none of its parameters and `(T)(x)` there was a cast
// through the file-scope typedef. ✔MEASURED 2026-11-03 (probe-reference-cc run
// 20261103-200559-43e50237, gcc 13.3.0 and clang 18.1.3, default and -std=c2x -pedantic-errors,
// built and run): every shape below with `(T) - 1` in the body exits 42 — the parameter, never the
// typedef. THE UNIVERSE: the returned list unnamed, named or `(void)` × one or two levels of
// returned function pointers × a DEFINITION (its own `T` is the body's: `(T)(x)` is a call, no
// cast) and a DECLARATION (its own list ends with its declarator: `(T)(1)` after it is a cast),
// and the `signal` shape (a function-pointer PARAMETER beside a returned function pointer).
TEST(ParserCastExpr, ADefinitionsOwnParametersSurviveTheListsOfTheTypeItReturns) {
    struct Shape { char const* what; char const* head; char const* tail; };
    // `head` + "g(int T, int x)" + `tail` is the declarator; the definition's own list is g's.
    Shape const shapes[] = {
        {"one level, the returned list unnamed", "int (*", ")(int)"},
        {"one level, the returned list named", "int (*", ")(int u)"},
        {"one level, the returned list (void)", "void (*", ")(void)"},
        {"two levels, unnamed", "int (*(*", ")(int))(int)"},
        {"two levels, named", "int (*(*", ")(int u))(int v)"},
        {"two levels, (void)", "void (*(*", ")(void))(void)"},
    };
    for (Shape const& s : shapes) {
        std::string const decl = std::string{s.head} + "g(int T, int x)" + s.tail;
        std::string const def = "typedef int T;\nstatic " + decl
                                + " { int y = (T)(x); (void)y; return 0; }\n";
        auto rd = parseC(def);
        EXPECT_FALSE(rd.tree.diagnostics().hasErrors()) << "definition, " << s.what;
        // (void)y is the one cast; `(T)(x)` is a call of the parameter.
        EXPECT_EQ(countNodesWithRule(rd.tree, "castExpr"), 1u) << "definition, " << s.what << "\n" << def;
        std::string const dcl = "typedef int T;\nstatic " + decl + ";\nint y = sizeof((T)(1));\n";
        auto rc = parseC(dcl);
        EXPECT_FALSE(rc.tree.diagnostics().hasErrors()) << "declaration, " << s.what;
        EXPECT_EQ(countNodesWithRule(rc.tree, "castExpr"), 1u) << "declaration, " << s.what << "\n" << dcl;
    }
    // The `signal` shape: a function-pointer parameter's own list closes inside g's list.
    auto rs = parseC("typedef int T;\nstatic void (*s(int T, void (*fn)(int), int x))(int) "
                     "{ int y = (T)(x); (void)y; return fn; }\n");
    EXPECT_FALSE(rs.tree.diagnostics().hasErrors());
    EXPECT_EQ(countNodesWithRule(rs.tree, "castExpr"), 1u) << "the signal shape";
    // P69 L2b2: the definition's NAME in parentheses — its derivation is read one level out, past
    // the group (`int (g)(…)`, and the same returning a function pointer), and its own list is still
    // the body's: with `(T) - 1 + x` in the body, gcc 13.3.0, clang 18.1.3 (default and -std=c2x
    // -pedantic-errors) and cl 19.51 (default and /std:c17) build both and run them to 42, the
    // parameter read (✔MEASURED 2026-11-04, probe-reference-cc runs 20261104-215531-bc2bac6c and
    // 20261104-215610-41d2ffbb).
    for (char const* src :
         {"typedef int T;\nstatic int (g)(int T, int x) { int y = (T)(x); (void)y; return 0; }\n",
          "typedef int T;\nstatic int (*(g)(int T, int x))(int) { int y = (T)(x); (void)y; return 0; }\n"}) {
        auto rp = parseC(src);
        EXPECT_FALSE(rp.tree.diagnostics().hasErrors()) << src;
        EXPECT_EQ(countNodesWithRule(rp.tree, "castExpr"), 1u) << "a parenthesised name\n" << src;
    }
}

// Seeding (the shipped descriptors' typedef channel, ParserConfig::
// seedGlobalTypeNames): the SAME source whose unknown name is refused as a
// type above commits the CAST when the name is seeded as a global type.
TEST(ParserCastExpr, SeededTypeNameFlipsTheRefusedSiteToCast) {
    ParserConfig cfg;
    cfg.seedGlobalTypeNames.push_back("q");
    auto r = parseC("int main() { return (q)-x; }", std::move(cfg));
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_TRUE(findFirstNodeWithRule(t, "castExpr").valid());
}

// A toy-grammar parse (no `semantics.declarations`) asks the binder sketch
// nothing — the sketch is inert for binder-less languages.
TEST(ParserCastExpr, BinderlessLanguageAsksTheSketchNothing) {
    auto loaded = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(loaded.has_value());
    auto schema = *loaded;
    auto src = SourceBuffer::fromString("print 1 + 2;", "<toy-sidecar>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             DiagnosticBudget::libraryDefault(), {},
             std::move(lexDiags)};
    auto r = std::move(p).parse();
    EXPECT_EQ(r.sketchWorkCount, 0u);
}

// ── the unknown-name refusal, READ (P69 L2b2, rulings 512 / 513 / 516) ───────
//
// A refusal is a lone name the cast's triage refused as a type because the binder sketch
// knows nothing of it. The parser names it — S_UndeclaredIdentifier at the name, once,
// before the error — exactly when the reading taken instead fails at a token the refused
// probe had read. Every pin below varies one thing against `UnknownNameWithNonOperator…`.
namespace {
[[nodiscard]] std::size_t countUndeclared(Tree const& t) {
    std::size_t n = 0;
    for (auto const& d : t.diagnostics().all())
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier) ++n;
    return n;
}
}  // namespace

// The refusal of the inner site, recorded under an outer probe that rolls back and again
// in the reading that wins, is ONE record: named once.
TEST(ParserCastExpr, ANestedRefusalIsNamedOnce) {
    auto r = parseC("int main() { return ((q) z); }");
    auto const& t = r.tree;
    ASSERT_TRUE(t.diagnostics().hasErrors());
    EXPECT_EQ(countUndeclared(t), 1u);
    auto const all = t.diagnostics().all();
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all[0].code, DiagnosticCode::S_UndeclaredIdentifier);
    EXPECT_EQ(all[0].actual, "q");
}

// The window ends at the type name's FOLLOWER. An operator follower continues the value
// reading, so an error after it is the operand's, never the refused type's: `(q)-x z`
// fails at `z`, past the follower `-`, and names nothing — the climb chain `(q)-x-y z`
// alike (the semantic tier, which names `q`, does not run on a tree that failed to parse).
TEST(ParserCastExpr, AnOperatorFollowersRefusalIsNeverNamed) {
    for (char const* src : {"int main() { return (q)-x z; }",
                            "int main() { return (q)-x-y z; }",
                            "int main() { return ((q)-x z); }"}) {
        auto r = parseC(src);
        auto const& t = r.tree;
        ASSERT_TRUE(t.diagnostics().hasErrors()) << src;
        EXPECT_EQ(countUndeclared(t), 0u) << src;
        auto const all = t.diagnostics().all();
        ASSERT_FALSE(all.empty()) << src;
        EXPECT_EQ(t.source().slice(all[0].span), "z") << src;
    }
}

// Two statements, each failing at its own follower: each names its own identifier, before
// its own error.
TEST(ParserCastExpr, TwoFailingSitesNameTwoNames) {
    auto r = parseC("int main() { (q) z; (r) w; }");
    auto const& t = r.tree;
    auto const all = t.diagnostics().all();
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[0].code, DiagnosticCode::S_UndeclaredIdentifier);
    EXPECT_EQ(all[0].actual, "q");
    EXPECT_EQ(t.source().slice(all[1].span), "z");
    EXPECT_EQ(all[2].code, DiagnosticCode::S_UndeclaredIdentifier);
    EXPECT_EQ(all[2].actual, "r");
    EXPECT_EQ(t.source().slice(all[3].span), "w");
}

// A name the sketch KNOWS is no refusal, whatever it is — a value of the source's own, a
// builtin function (`semantics.builtinFunctions`), a predefined identifier, a library
// builtin (`semantics.libraryBuiltins`), a seeded descriptor value: the follower's error
// only, as gcc and clang give it (MEASURED 2026-11-04, probes b08/b09: `(__func__)42;` and
// `(__builtin_expect)42;` "expected ';'" alone on gcc 13.3.0 and clang 18.1.3).
TEST(ParserCastExpr, AKnownNameIsNeverNamedUndeclared) {
    for (char const* src : {
             "int main() { int v; (v) z; }",
             "int main() { (__builtin_expect)42; }",
             "int f(void) { (__func__)42; return 0; }",
             "int main() { (__builtin_memcpy)42; }",
         }) {
        auto r = parseC(src);
        auto const& t = r.tree;
        ASSERT_TRUE(t.diagnostics().hasErrors()) << src;
        EXPECT_EQ(countUndeclared(t), 0u) << src;
    }
    ParserConfig cfg;
    cfg.seedGlobalValueNames.push_back("q");
    auto r = parseC("int main() { (q) z; }", std::move(cfg));
    ASSERT_TRUE(r.tree.diagnostics().hasErrors());
    EXPECT_EQ(countUndeclared(r.tree), 0u) << "a seeded descriptor value is a known name";
}

// ── speculation rollback hygiene ────────────────────────────────────────

// A refusal recorded inside an ENCLOSING probe that itself rolls back
// must not leak twice: `((q)-x)` probes parenExpr around the inner
// ambiguous site; the inner refusal is recorded, the outer parenExpr
// probe COMMITS, and the Pratt walker climbs the surrounding chain
// (wrap-in-place — the primary is never re-parsed) — the net result
// must be EXACTLY ONE refusal for `q`, not a duplicate per probe — read
// where the reader can see it, `ANestedRefusalIsNamedOnce` below.
// (This pins the probe snapshot/restore + record-after-rollback
// interplay; a leak would double-count.)
TEST(ParserCastExpr, NestedProbesParseTheValueReading) {
    auto r = parseC("int main() { return ((q)-x); }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
}

// Same hygiene across a climb chain: `(q)-x-y` builds two binary wraps
// above the `(q)` primary (wrap-in-place — the ambiguous site is
// parsed exactly once) — still exactly one refusal (named nowhere: `AnOperatorFollowersRefusalIsNeverNamed`).
TEST(ParserCastExpr, ClimbChainParsesTheValueReading) {
    auto r = parseC("int main() { return (q)-x-y; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
}

// Two genuinely distinct ambiguous sites (`TwoFailingSitesNameTwoNames`).
TEST(ParserCastExpr, TwoDistinctSitesParseTheValueReading) {
    auto r = parseC("int main() { return (q)-x + (r)-y; }");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors());
}

// Rolled-back probes leak no BINDINGS either: a failed castExpr probe
// over `(a)` (a known value) must leave `a`'s binding intact and the
// sketch consistent — proven by the SECOND lookup behaving identically
// (if the probe leaked or truncated state, the second `(a)-c` would
// flip its reading).
TEST(ParserCastExpr, RolledBackProbeLeaksNoSketchState) {
    auto r = parseC(
        "int main() { int a; int s = (a)-b; return (a)-c; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid());
    EXPECT_EQ(countNodesWithRule(t, "parenExpr"), 2u);
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
// leak a refusal.
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
    auto r = parseC(
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

    // The tokens AFTER the rollback parsed correctly: the trailing `-z` closes
    // a left-assoc subtraction chain over the (deep) value reading. Two `-`
    // operators ⇒ two binaryExpr wraps; the climb resumed cleanly from the
    // restored state.
    EXPECT_EQ(countNodesWithRule(t, "binaryExpr"), 2u);
}

// ── c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME — abstract fn-ptr/array
//    declarator in cast / sizeof / compound-literal / va_arg type-name ───────
//
// The fix reuses `abstractDirectDeclarator` (the abstract twin of
// `directDeclarator`, Identifier base excluded) as the optional tail of
// `castTypeRef` (the leading stars stay in `{repeat StarOp}`). These pins assert
// the EXACT subtree the reuse produces — red-on-disable: drop the optional
// `abstractDirectDeclarator` from `castTypeRef` and every one of these fails to
// parse (P0009) or loses the `abstractDirectDeclarator`/`parenDeclarator`/
// `fnSuffix` nesting.

// `(int(*)(void))p` — the canonical abstract fn-pointer cast (the sqlite
// `osGeteuid` shape). castTypeRef carries the base + an abstractDirectDeclarator
// whose parenDeclarator holds the abstract `(*)` and whose fnSuffix holds
// `(void)`.
TEST(ParserCastExpr, AbstractFnPtrCastExactShape) {
    auto r = parseC("int main() { return (int(*)(void))p; }");
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
              "    rule:abstractDirectDeclarator\n"
              "      rule:parenDeclarator\n"
              "        tok:\"(\"\n"
              "        rule:declarator\n"
              "          rule:pointerLayer\n"
              "            tok:\"*\"\n"
              "        tok:\")\"\n"
              "      rule:fnSuffix\n"
              "        tok:\"(\"\n"
              "        rule:paramList\n"
              "          rule:param\n"
              "            rule:declHeadForParam\n"
              "              rule:typeSpecifierSeq\n"
              "                tok:\"void\"\n"
              "        tok:\")\"\n"
              "  tok:\")\"\n"
              "  rule:castOperand\n"
              "    rule:operand\n"
              "      tok:\"p\"\n");
}

// `(int(*)(int))p` — a non-void param list flows through identically; the
// param's declHeadForParam carries `int`.
TEST(ParserCastExpr, AbstractFnPtrCastWithIntParamParses) {
    auto r = parseC("int main() { return (int(*)(int))p; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    // The abstractDirectDeclarator + parenDeclarator(*) + fnSuffix(int) nesting.
    const std::string pretty = prettyPrintSubtree(t, cast);
    EXPECT_NE(pretty.find("rule:castTypeRef\n"
                          "    rule:castTypeBase\n"
                          "      rule:typeSpecifierSeq\n"
                          "        tok:\"int\"\n"
                          "    rule:abstractDirectDeclarator\n"
                          "      rule:parenDeclarator\n"),
              std::string::npos) << pretty;
    EXPECT_NE(pretty.find("tok:\"int\"\n"
                          "        tok:\")\"\n"),
              std::string::npos) << pretty;
}

// `sizeof(int(*)(void))` — the SAME castTypeRef tail under sizeofType (shared
// rule). The abstractDirectDeclarator nesting must appear below the type child.
TEST(ParserCastExpr, AbstractFnPtrSizeofExactShape) {
    auto r = parseC("int main() { return sizeof(int(*)(void)); }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId so = findFirstNodeWithRule(t, "sizeofType");
    ASSERT_TRUE(so.valid());
    EXPECT_EQ(prettyPrintSubtree(t, so),
              "rule:sizeofType\n"
              "  tok:\"sizeof\"\n"
              "  tok:\"(\"\n"
              "  rule:castTypeRef\n"
              "    rule:castTypeBase\n"
              "      rule:typeSpecifierSeq\n"
              "        tok:\"int\"\n"
              "    rule:abstractDirectDeclarator\n"
              "      rule:parenDeclarator\n"
              "        tok:\"(\"\n"
              "        rule:declarator\n"
              "          rule:pointerLayer\n"
              "            tok:\"*\"\n"
              "        tok:\")\"\n"
              "      rule:fnSuffix\n"
              "        tok:\"(\"\n"
              "        rule:paramList\n"
              "          rule:param\n"
              "            rule:declHeadForParam\n"
              "              rule:typeSpecifierSeq\n"
              "                tok:\"void\"\n"
              "        tok:\")\"\n"
              "  tok:\")\"\n");
}

// Compound-literal type position shares castTypeRef: `(int(*)(void)){0}` parses
// the abstract fn-ptr type then the brace init. The abstractDirectDeclarator
// must nest under the compoundLiteralExpr's castTypeRef child.
TEST(ParserCastExpr, AbstractFnPtrCompoundLiteralParses) {
    auto r = parseC(
        "int main() { return (int(*)(void)){0} != 0; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cl = findFirstNodeWithRule(t, "compoundLiteralExpr");
    ASSERT_TRUE(cl.valid());
    const std::string pretty = prettyPrintSubtree(t, cl);
    EXPECT_NE(pretty.find("rule:castTypeRef\n"
                          "    rule:castTypeBase\n"
                          "      rule:typeSpecifierSeq\n"
                          "        tok:\"int\"\n"
                          "    rule:abstractDirectDeclarator\n"
                          "      rule:parenDeclarator\n"),
              std::string::npos) << pretty;
    EXPECT_NE(pretty.find("rule:braceInitList"), std::string::npos) << pretty;
}

// va_arg type position shares castTypeRef: `va_arg(ap, int(*)(void))` parses the
// abstract fn-ptr type as the second (type) argument. (`ap` is an ordinary
// identifier operand here — parsing the TYPE arg is what this pins.)
TEST(ParserCastExpr, AbstractFnPtrVaArgParses) {
    auto r = parseC(
        "int f(int n, ...) {\n"
        "  return __builtin_va_arg(n, int(*)(void)) != 0;\n"
        "}\n");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId va = findFirstNodeWithRule(t, "vaArgExpr");
    ASSERT_TRUE(va.valid());
    const std::string pretty = prettyPrintSubtree(t, va);
    EXPECT_NE(pretty.find("rule:castTypeRef\n"
                          "    rule:castTypeBase\n"
                          "      rule:typeSpecifierSeq\n"
                          "        tok:\"int\"\n"
                          "    rule:abstractDirectDeclarator\n"
                          "      rule:parenDeclarator\n"),
              std::string::npos) << pretty;
}

// REGRESSION — the base+stars forms must stay byte-identical: the optional
// abstract declarator does NOT fire for `(int*)` / `(char const *)`. The cast's
// OWN castTypeRef subtree must carry the star inside a `rule:pointerLayer` with
// NO abstractDirectDeclarator tail (the `{repeat pointerLayer}` consumes it; the
// optional sees the closing `)` and matches nothing). Pins the cast subtree
// EXACTLY (the tree-wide declarator count is unusable — every decl has one).
TEST(ParserCastExpr, PointerCastTypeRefHasNoDeclaratorTail) {
    auto r = parseC("int main() { return (int*)p; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    // The star nests in a pointerLayer (c29 `{repeat pointerLayer}`), NO
    // abstractDirectDeclarator — a star-only cast leaves the abstract tail empty.
    EXPECT_EQ(prettyPrintSubtree(t, cast),
              "rule:castExpr\n"
              "  tok:\"(\"\n"
              "  rule:castTypeRef\n"
              "    rule:castTypeBase\n"
              "      rule:typeSpecifierSeq\n"
              "        tok:\"int\"\n"
              "    rule:pointerLayer\n"
              "      tok:\"*\"\n"
              "  tok:\")\"\n"
              "  rule:castOperand\n"
              "    rule:operand\n"
              "      tok:\"p\"\n");
}

// REGRESSION (the c26 abstract-base hazard): `(c * c)` is a parenthesized
// MULTIPLICATION, NOT a cast. The abstract declarator's base must EXCLUDE the
// Identifier production — otherwise `(c *<abstract-decl c>)` would speculatively
// commit a bogus cast (the type child gains >1 leaf → commit triage rule-1).
// Red-on-disable: add `Identifier` back to abstractDirectDeclarator's base alt
// and this flips to a (mis-parsed) castExpr. Mirrors the zext_u32_to_u64 corpus
// example's `(c * c) * (c * c)`.
TEST(ParserCastExpr, ParenMultiplicationIsNotAnAbstractCast) {
    auto r = parseC(
        "unsigned long long f(unsigned long long c) {\n"
        "  return (c * c) * (c * c);\n"
        "}\n");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    EXPECT_FALSE(findFirstNodeWithRule(t, "castExpr").valid())
        << "(c * c) must parse as a multiplication, never a cast";
    EXPECT_TRUE(findFirstNodeWithRule(t, "binaryExpr").valid());
}

// REGRESSION (the c26 array-base hazard): `sizeof(b[0])` is the VALUE form (an
// array INDEX), NOT a bare-array abstract type-name `b[0]`. The abstract
// declarator's base must EXCLUDE the array suffix — otherwise `(b[0])` mis-commits
// as a type-name (D-CSUBSET-ABSTRACT-ARRAY-TYPE-NAME). Red-on-disable: add
// `arrayDeclSuffix` to abstractDirectDeclarator's base alt and `sizeof(b[0])`
// flips to a (mis-typed) sizeofType. Mirrors the sizeof_value_array_dim corpus.
TEST(ParserCastExpr, SizeofIndexValueIsNotAnAbstractArrayTypeName) {
    auto r = parseC(
        "int run() {\n"
        "  int b[8];\n"
        "  return (int)(sizeof(b) / sizeof(b[0]));\n"
        "}\n");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    // Both sizeofs are the VALUE form; neither commits a sizeofType type-name.
    EXPECT_FALSE(findFirstNodeWithRule(t, "sizeofType").valid())
        << "sizeof(b) and sizeof(b[0]) are value forms, not type-names";
    EXPECT_TRUE(findFirstNodeWithRule(t, "sizeofValue").valid());
}

// east-const `(char const *)` stays base+const+star with no declarator tail.
TEST(ParserCastExpr, EastConstPointerCastTypeRefHasNoDeclaratorTail) {
    auto r = parseC("int main() { return (char const *)p; }");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors());
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    const std::string pretty = prettyPrintSubtree(t, cast);
    EXPECT_EQ(pretty.find("rule:abstractDirectDeclarator"), std::string::npos)
        << "the east-const pointer cast must not grow an abstract-declarator tail\n"
        << pretty;
    // The east-const star (now inside its pointerLayer) is present, no tail.
    EXPECT_NE(pretty.find("rule:pointerLayer\n"), std::string::npos) << pretty;
    EXPECT_NE(pretty.find("tok:\"*\"\n"), std::string::npos) << pretty;
}

// ─────────────────────────────────────────────────────────────────────────
// c29 D-CSUBSET-POST-STAR-CAST-QUALIFIER — post-star qualifier in a cast
// type-name (`(const u8 * const)p`, `(u32 * volatile)p`). The grammar change
// is `castTypeRef`'s `{repeat StarOp}` → `{repeat pointerLayer}` so a post-star
// const/volatile rides INSIDE the layer (present only WITH a star → no
// zero-star adjacency, the c11 ambiguity). Milestone-1 verifies the schema
// still LOADS clean (no C_AmbiguousAlternatives — a load failure trips the
// EXPECT_TRUE(loaded) inside parseC) and the post-star qualifier PARSES.
// ─────────────────────────────────────────────────────────────────────────

// The frontier form: `(const u8 * const)pKey1` — a west const, a star, a POST-star
// const. `u8` aliases unsigned char (a typedef) so the base resolves and the cast
// commits via triage rule 1 (the star). EXACT shape: headQualifier(const),
// castTypeBase(u8), then a pointerLayer wrapping the star AND its ptrQualifier(const).
TEST(ParserCastExpr, PostStarConstCastExactShape) {
    auto r = parseC(
        "typedef unsigned char u8;\n"
        "int main() { return (const u8 * const)pKey1; }\n");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors())
        << "(const u8 * const) must parse — the post-star const rides in pointerLayer";
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    const std::string pretty = prettyPrintSubtree(t, cast);
    // West const (headQualifier before base), the typedef-name base (the
    // castTypeBase Identifier alt — `u8`), then a pointerLayer that carries BOTH
    // the star and the post-star const ptrQualifier — NO abstractDirectDeclarator.
    EXPECT_NE(pretty.find(
                  "  rule:castTypeRef\n"
                  "    rule:headQualifier\n"
                  "      tok:\"const\"\n"
                  "    rule:castTypeBase\n"
                  "      tok:\"u8\"\n"
                  "    rule:pointerLayer\n"
                  "      tok:\"*\"\n"
                  "      rule:ptrQualifier\n"
                  "        tok:\"const\"\n"),
              std::string::npos)
        << "post-star const must nest inside pointerLayer, not be a flat sibling\n"
        << pretty;
    EXPECT_EQ(pretty.find("rule:abstractDirectDeclarator"), std::string::npos)
        << pretty;
}

// `(unsigned char * const)&x` — the same post-star const with a multi-word
// keyword base (no typedef). Parses; the post-star const is in the layer.
TEST(ParserCastExpr, PostStarConstKeywordBaseCastParses) {
    auto r = parseC(
        "int main() { unsigned char x = 0;\n"
        "             const unsigned char *p = (unsigned char * const)&x;\n"
        "             return *p; }\n");
    ASSERT_FALSE(r.tree.diagnostics().hasErrors())
        << "(unsigned char * const)&x must parse";
    EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
}

// `(u32 * volatile)p` — a post-star VOLATILE (not const). The grammar accepts it
// (ptrQualifier = const|volatile|restrict); the resolver strips it (a no-op at the
// parse layer — pinned semantically in the semantic-analyzer test). Here: it PARSES
// and the volatile lands inside the pointerLayer.
TEST(ParserCastExpr, PostStarVolatileCastParses) {
    auto r = parseC(
        "typedef unsigned int u32;\n"
        "int main() { return (u32 * volatile)p; }\n");
    auto const& t = r.tree;
    ASSERT_FALSE(t.diagnostics().hasErrors())
        << "(u32 * volatile) must parse — post-star volatile rides in pointerLayer";
    const NodeId cast = findFirstNodeWithRule(t, "castExpr");
    ASSERT_TRUE(cast.valid());
    const std::string pretty = prettyPrintSubtree(t, cast);
    EXPECT_NE(pretty.find("    rule:pointerLayer\n"
                          "      tok:\"*\"\n"
                          "      rule:ptrQualifier\n"
                          "        tok:\"volatile\"\n"),
              std::string::npos) << pretty;
}

// Milestone-1 regression battery: the SAME castTypeRef rule must still parse the
// PRE-c29 cast forms after the `{repeat StarOp}` → `{repeat pointerLayer}` swap.
// If the swap re-introduced C_AmbiguousAlternatives the schema would fail to load
// and EVERY parseC here would trip its internal EXPECT_TRUE(loaded). These
// pin the four shapes the prompt names as must-stay-green.
TEST(ParserCastExpr, PostStarSwapKeepsExistingCastsParsing) {
    // (int*)x — bare pointer cast.
    {
        auto r = parseC("int main() { return (int*)x; }");
        ASSERT_FALSE(r.tree.diagnostics().hasErrors()) << "(int*)x";
        EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
        // The star now nests in a pointerLayer (the shape change).
        const NodeId cast = findFirstNodeWithRule(r.tree, "castExpr");
        EXPECT_NE(prettyPrintSubtree(r.tree, cast).find(
                      "rule:pointerLayer\n"),
                  std::string::npos);
    }
    // (char const *)x — east-const cast (c11).
    {
        auto r = parseC("int main() { return (char const *)x; }");
        ASSERT_FALSE(r.tree.diagnostics().hasErrors()) << "(char const *)x";
        EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
    }
    // (struct S)x — struct-tag base, zero stars (no pointerLayer at all).
    {
        auto r = parseC("struct S { int a; };\n"
                              "int main() { struct S s; return (struct S)s.a; }");
        ASSERT_FALSE(r.tree.diagnostics().hasErrors()) << "(struct S)x";
        EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
    }
    // (int(*)(void))p — c26 abstract fn-ptr declarator (the abstract tail still
    // follows the (now-empty) pointerLayer repeat).
    {
        auto r = parseC("int main() { return (int(*)(void))p; }");
        ASSERT_FALSE(r.tree.diagnostics().hasErrors()) << "(int(*)(void))p";
        EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
        EXPECT_TRUE(
            findFirstNodeWithRule(r.tree, "abstractDirectDeclarator").valid());
    }
    // (volatile u32 **)p — c27 pre-stars volatile pointee (two pointerLayers).
    {
        auto r = parseC("typedef unsigned int u32;\n"
                              "int main() { return (volatile u32 **)p; }");
        ASSERT_FALSE(r.tree.diagnostics().hasErrors()) << "(volatile u32 **)p";
        EXPECT_TRUE(findFirstNodeWithRule(r.tree, "castExpr").valid());
    }
}


// ── A COMMITTED TYPE READING THAT FAILS IS THE SITE'S ANSWER (batch B) ──────
//
// ★★★ Once a site's guarded TYPE reading has committed (its type child closed
// as a type: a keyword base, or a name the sketch knows as a type) and then
// failed, the program is that type reading, wrong — never the site's VALUE
// reading (`"valueReading": true` in the grammar: exprStmt, expression,
// parenExpr, sizeofValue, alignofValue, typeofValue, assignmentExpr). So a
// value reading is not tried after it, the final descent takes none, and the
// depth-0 replay is the declared-LAST committed candidate, as the structural
// fallback is. A type reading that did NOT commit leaves every candidate to be
// tried, value readings included. Each row's position is the one gcc, clang
// and cl report (clang's column, gcc's token): before this the replay read
// `(int)` as a parenthesised `int` and refused at `int`, a position no
// reference names, and `sizeof((T))` compiled.
namespace {
struct BatchBRow {
    char const* what;
    char const* source;
    char const* firstErrorAt;             // nullptr: the row parses clean
    char const* thenAt = nullptr;         // the second error's text, where the row states one
};
}  // namespace

TEST(ParserCastExpr, AFailedCommittedTypeReadingIsTheSitesAnswer) {
    BatchBRow const rows[] = {
        // A valid declaration whose FIRST guarded candidate commits and fails:
        // the type readings after it are still tried; only value readings are not.
        {"a qualified typedef declaration",
         "typedef int T;\nint main(void) { const T x = 1; return x; }\n", nullptr},
        // The committed reading's own error. ✔MEASURED 2026-11-13 (probes/l2b2bB; linux
        // host run 20261113-210608-54e345ab, windows run host 20261113-210625-50556da7):
        // gcc 13.3.0 and clang 18.1.3 "expected expression" at `;`, cl C2059 at `;`;
        // `T (x) + 1;` — gcc "expected '=', ',', ';' … before '+'", cl C2143 "missing ';'
        // before '+'" (clang at the end of the declarator just before it); `const T x = 1;`
        // compiles on all three. (`typeNameBareEmptyDecl` reads `T` first and its follower
        // refuses it: a follower's refusal is no committed failure, so only `identVarDecl`
        // commits and fails here.)
        {"a declaration missing its initializer",
         "typedef int T;\nint main(void) { T x = ; return 0; }\n", ";"},
        {"a declaration whose parenthesised declarator is followed by an operator",
         "typedef int T;\nint main(void) { T (x) + 1; return 0; }\n", "+"},
        // q04-q07 (gcc 13.3.0, clang 18.1.3, cl 19.51: "expected expression" at
        // the inner `)`). TWO candidates commit on `(int)` and fail — compoundLiteralExpr
        // (declared first) and castExpr — and the DECLARED-LAST, castExpr, is replayed: its
        // operand's error at the inner `)`, where every reference puts it.
        {"q04 a value-star cast inside a cast",
         "int main(void) { int v = 1; return (int)(v *)0; }\n", ")"},
        {"q05 a value-star sizeof inside a cast",
         "int main(void) { int v = 1; return (int)sizeof(v *); }\n", ")"},
        {"q06 a value-star cast two casts deep",
         "int main(void) { int v = 1; return (int)(long)(v *)0; }\n", ")"},
        // q07: and `U` undeclared — gcc and cl name `U` FIRST, then the inner `)`; clang
        // the `)` first (probes l2b2tq, ruling 523: gcc's order where they split). DSS
        // names U, then the `)` (✔MEASURED, compile-and-keep l2b2g7-dss-1, macos-arm64-debug).
        {"q07 an unknown-star cast inside a cast",
         "int main(void) { return (int)(U *)0; }\n", "U", ")"},
        // n02: every reference refuses at the token after the cast `(T)`.
        {"a parenthesised typedef name as sizeof's operand",
         "typedef int T;\nint main(void) { return (int)sizeof((T)); }\n", ")"},
    };
    for (BatchBRow const& r : rows) {
        SCOPED_TRACE(r.what);
        auto res = parseC(r.source);
        auto const& t = res.tree;
        if (r.firstErrorAt == nullptr) {
            EXPECT_FALSE(t.diagnostics().hasErrors());
            continue;
        }
        auto const all = t.diagnostics().all();
        ASSERT_FALSE(all.empty()) << "the row parsed clean";
        EXPECT_EQ(all[0].severity, DiagnosticSeverity::Error);
        EXPECT_EQ(t.source().slice(all[0].span), r.firstErrorAt);
        if (r.thenAt != nullptr) {
            ASSERT_GE(all.size(), 2u) << "the row's second error is missing";
            EXPECT_EQ(t.source().slice(all[1].span), r.thenAt);
        }
    }
}

// A guarded type reading that did NOT commit (its type child is a known VALUE),
// or one its FOLLOWER refused after it read clean (`notFollowedBy`: `sizeof ( T )`
// and `_Alignof ( T )` before `{` are the operator over a compound literal's VALUE,
// C 6.5.2.5), leaves the value readings to be tried: each of these parses clean.
TEST(ParserCastExpr, AnUncommittedTypeReadingLeavesEveryCandidateToBeTried) {
    for (char const* src : {
             "int main(void) { int v = 2, p = 3; v * p; return 0; }\n",
             "int main(void) { int v = 2; return (v) + 1; }\n",
             "int main(void) { int v = 2; return (int)sizeof(v); }\n",
             "int main(void) { int v = 2; return (int)sizeof((v)); }\n",
             "int main(void) { return (int)sizeof (int[]){ 1, 2 }; }\n",
             "int main(void) { return (int)_Alignof (int[]){ 1, 2 }; }\n",
         }) {
        SCOPED_TRACE(src);
        auto res = parseC(src);
        EXPECT_FALSE(res.tree.diagnostics().hasErrors());
    }
}

// The `valueReading` shape key is a boolean, and never sits beside a type guard
// on one shape: a value reading carries no type guard, and a shape holding both
// would be a type reading the parser skips after another candidate's committed
// type reading failed. Both are refused loud at load (C_UnknownShape, the
// message naming the key); the control loads and answers `isValueReading`.
TEST(ParserCastExpr, AValueReadingIsABooleanShapeKeyWithNoTypeGuard) {
    auto const doc = [](std::string_view vdeclExtra) {
        return std::string{R"JSON({
      "dssSchemaVersion": 4,
      "language": { "name": "X", "version": "0.1.0" },
      "tokens": { ";": [{ "kind": "Semi" }] },
      "shapes": {
        "root":     { "sequence": [ "Semi" ] },
        "typeName": { "sequence": [ "Identifier" ] },
        "vdecl":    { "sequence": [ "typeName", "Identifier", "Semi" ])JSON"}
            + std::string{vdeclExtra} + R"JSON( }
      },
      "semantics": { "identifierToken": "Identifier" }
    })JSON";
    };
    struct Refused {
        char const* what;
        char const* extra;
        char const* says;
    };
    Refused const refused[] = {
        {"a value that is not a boolean", R"(, "valueReading": 5)",
         "'valueReading' must be a boolean"},
        {"a value reading beside a type guard",
         R"(, "valueReading": true, "commitRequiresTypeName": "typeName")",
         "'valueReading' and 'commitRequiresTypeName' are mutually exclusive"},
    };
    for (Refused const& r : refused) {
        SCOPED_TRACE(r.what);
        auto loaded = GrammarSchema::loadFromText(doc(r.extra));
        ASSERT_FALSE(loaded.has_value()) << "the shape loaded";
        bool said = false;
        for (auto const& d : loaded.error()) {
            said = said || (d.code == DiagnosticCode::C_UnknownShape
                            && d.message.find(r.says) != std::string::npos);
        }
        EXPECT_TRUE(said) << "refused, but not as C_UnknownShape naming the key";
    }
    auto ok = GrammarSchema::loadFromText(doc(R"(, "valueReading": true)"));
    ASSERT_TRUE(ok.has_value()) << "a value reading with no type guard is refused";
    auto const& schema = *ok;
    EXPECT_TRUE(schema->isValueReading(schema->rules().find("vdecl")));
    EXPECT_FALSE(schema->isValueReading(schema->rules().find("typeName")));
}

