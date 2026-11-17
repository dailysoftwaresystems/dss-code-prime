// ===========================================================================
// P69 L2b2 (lane `lm`) — THE TYPE-NAME TRIAGE'S BASE ASK, HOISTED BEFORE THE PROBE.
//
// A speculative candidate whose type child is entered after a fixed run of s
// single-token elements has its base at grammar-token offset s. When the run's
// tokens are the ones it admits and the base is a name the binder sketch knows
// as a VALUE, the probe would open, be refuted by the triage's early base ask
// when the type child's frame opens, and be abandoned; the parser asks the same
// question before opening it instead (`Parser::Impl::hoistRefutesTheCandidate_`).
// ✔MEASURED 2026-11-16 (sqlite3.c, windows-x86_64-release): the empty
// typedef-named declaration's bare form and identVarDecl each failed that way
// 40,224 times, once per identifier-led statement.
//
// WHAT THIS FILE PINS, each against `ParserConfig::hoistTriageBaseAsk` off (every
// candidate through its probe) on the same input:
//   (A) THE REACH, read off the data: the shipped rules whose type child leads
//       after a fixed run, with the run's length — c's nine, and none in any
//       other shipped language (`GrammarSchema::typeChildLeadRunLength`).
//   (B) A VALUE BASE COSTS NO PROBE: the same tree and diagnostics, and strictly
//       fewer probes, at each run length the reach holds (0, 1 and 2), each
//       input isolating its length so an arm that reads the wrong offset reds.
//   (C) A TYPE OR UNKNOWN BASE IS LEFT TO ITS PROBE: the same tree and
//       diagnostics, and the SAME probe count. `T + 1;` and `return (T) / 2;`
//       are the inputs whose type child commits nothing and fails after the
//       type (a hoist refuting them would change which failure is remembered).
//   (D) EVERY CONDITION BITES, on a synthetic grammar where it alone fails, the
//       control beside it (the same grammar with the condition met) proving the
//       hoist fires there: the type rule derives nothing; an attribute
//       specifier can start with an identifier; the candidate is a definition
//       body; the run does not fit the probe's token budget; the run's tokens
//       are not the ones it admits.
//   (E) THE GRAMMAR-TOKEN VIEW looks through a non-coalesced body mode's
//       per-codepoint leaves (`Parser::Impl::peekGrammarToken_`): tsql-subset
//       with a speculative `[name]()` call parses `SELECT [a]() FROM t;`.
//
// RED-ON-DISABLE (tests/mutations/arms.registry): one arm per condition in (D),
// the hoist reading the base at the wrong offset (B), the hoist ignoring the
// sketch's answer (C, and the identity witness), the view ignoring body tokens
// (E).
// ⚠ MUST run through ctest: the shipped documents are found through the repo root.
// ===========================================================================

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "tokenizer/token_stream.hpp"
#include "tokenizer/tokenizer.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> shipped(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    EXPECT_TRUE(loaded.has_value()) << "the shipped language document " << name;
    return loaded.has_value() ? *loaded : nullptr;
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> fromText(std::string const& text,
                                                            std::string_view label) {
    auto loaded = GrammarSchema::loadFromText(text, label);
    if (!loaded) {
        std::string why;
        for (auto const& d : loaded.error()) why += "\n  " + d.message;
        ADD_FAILURE() << label << " failed to load:" << why;
        return nullptr;
    }
    return *loaded;
}

// The shipped ceilings, read off the schema; the hoist switch as asked.
[[nodiscard]] ParserConfig configFor(GrammarSchema const& schema, bool hoist) {
    ParserConfig cfg;
    if (auto v = schema.maxExpressionDepth())      cfg.maxExpressionDepth = *v;
    if (auto v = schema.maxSpeculationDepth())     cfg.maxSpeculationDepth = *v;
    if (auto v = schema.speculationBudgetFactor()) cfg.speculationBudgetFactor = *v;
    cfg.hoistTriageBaseAsk = hoist;
    return cfg;
}

[[nodiscard]] ParseResult parse(std::shared_ptr<GrammarSchema const> schema, std::string source,
                                ParserConfig cfg) {
    auto src = SourceBuffer::fromString(std::move(source), "<triage-hoist>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{std::move(src), std::move(schema), std::move(stream),
             DiagnosticBudget::libraryDefault(), std::move(cfg), std::move(lexDiags)};
    return std::move(p).parse();
}

// The finished tree node for node and its whole diagnostic stream (the
// refusal-cost test's rendering, plus each diagnostic's scope kinds).
[[nodiscard]] std::string render(Tree const& t) {
    std::string out = "root=" + std::to_string(t.root().v) + " nodes=" + std::to_string(t.nodeCount()) + "\n";
    for (std::uint32_t i = 1; t.root().valid() && i < static_cast<std::uint32_t>(t.nodeCount()); ++i) {
        NodeId const id{i, t.root().arenaTag};
        out += "#" + std::to_string(i) + " kind=" + std::to_string(static_cast<int>(t.kind(id)))
             + " flags=" + std::to_string(static_cast<std::uint32_t>(t.flags(id)))
             + " span=" + std::to_string(static_cast<std::uint64_t>(t.span(id).start())) + ":"
             + std::to_string(static_cast<std::uint64_t>(t.span(id).end()))
             + " parent=" + std::to_string(t.parent(id).v);
        if (t.kind(id) == NodeKind::Internal) out += " rule=" + std::to_string(t.rule(id).v);
        else if (t.kind(id) == NodeKind::Token) out += " tok=" + std::to_string(t.tokenKind(id).v);
        out += " kids=[";
        for (auto c : t.children(id)) out += std::to_string(c.v) + ",";
        out += "]\n";
    }
    out += "diags:\n";
    for (auto const& d : t.diagnostics().all()) {
        out += "  " + std::to_string(static_cast<int>(d.code)) + "@"
             + std::to_string(static_cast<std::uint64_t>(d.span.start())) + " actual=" + d.actual + " scopes=[";
        for (ScopeKind const k : d.scopeStack.kinds()) out += std::to_string(static_cast<int>(k)) + ",";
        out += "]\n";
    }
    return out;
}

[[nodiscard]] bool hasRule(Tree const& t, std::string_view name) {
    RuleId const r = t.schema().rules().find(name);
    if (!r.valid()) return false;
    for (std::uint32_t i = 1; t.root().valid() && i < static_cast<std::uint32_t>(t.nodeCount()); ++i) {
        NodeId const id{i, t.root().arenaTag};
        if (t.kind(id) == NodeKind::Internal && t.rule(id).v == r.v) return true;
    }
    return false;
}

struct OnOff {
    std::string   on, off;
    std::uint64_t probesOn = 0, probesOff = 0;
    bool          errorsOn = false;
};

[[nodiscard]] OnOff onOff(std::shared_ptr<GrammarSchema const> const& schema, std::string const& source,
                          std::optional<std::size_t> budgetFactor = std::nullopt) {
    ParserConfig on = configFor(*schema, true), off = configFor(*schema, false);
    if (budgetFactor) on.speculationBudgetFactor = off.speculationBudgetFactor = *budgetFactor;
    ParseResult const a = parse(schema, source, std::move(on));
    ParseResult const b = parse(schema, source, std::move(off));
    return OnOff{render(a.tree), render(b.tree), a.speculationProbeCount, b.speculationProbeCount,
                 a.tree.diagnostics().hasErrors()};
}

// ── shipped c ─────────────────────────────────────────────────────────────
// Every name the inputs read: a typedef T, an enumerator N, a function f, and two
// variables. The wrapper's own statements and file-scope declarations are all
// keyword-led, so none of them offers the hoist a candidate: every refutation a
// case counts is the case's own statement's.
[[nodiscard]] std::string inC(std::string_view stmt) {
    return std::format("typedef int T;\nenum {{ N = 4 }};\nvoid f(int);\n"
                       "int main(void) {{\n  int v = 0;\n  int x = 0;\n  {}\n  return 0;\n}}\n", stmt);
}

// ── the synthetic grammar (D) ───────────────────────────────────────────
// A cast-like candidate `castish` whose type child `typePos` carries the
// requireKnownType guard, beside a parenthesized value reading. `let x;` binds a
// variable the sketch then knows as a VALUE, `alias A;` a type. Each knob varies
// exactly one condition.
struct Synth {
    std::string castish   = R"("sequence": [ "ParenOpen", "typePos", "ParenClose", "castOperand" ])";
    std::string typePos   = R"({ "alt": [ "BaseKw", "Identifier" ] })";
    int         lookahead = 16;
    std::string vardeclRowExtra;     // a definition-body discriminator on the variable row
    std::string attributeSemantics;  // `"attributeSemantics": {...},` or empty
    std::string extraShapes;         // `"name": {...},` rows
};

[[nodiscard]] std::string synthText(Synth const& s) {
    return std::format(R"JSON({{
  "dssSchemaVersion": 4,
  "language": {{ "name": "HoistSynth", "version": "0.0.1", "fileExtensions": [".hsyn"] }},
  "tokens": {{
    " ":  [{{ "kind": "Whitespace", "flags": ["EmptySpace"] }}],
    "\n": [{{ "kind": "Newline",    "flags": ["EmptySpace"] }}],
    "-":  [{{ "kind": "Minus" }}],
    ";":  [{{ "kind": "Semi" }}],
    "(":  [{{ "kind": "ParenOpen" }}],
    ")":  [{{ "kind": "ParenClose" }}]
  }},
  "numberStyle": {{ "decimal": true, "emitKind": {{ "integer": "IntLiteral" }} }},
  "keywords": [
    {{ "word": "let",   "kind": "LetKw" }},
    {{ "word": "alias", "kind": "AliasKw" }},
    {{ "word": "base",  "kind": "BaseKw" }}
  ],
  "operators": {{
    "groups": [
      {{ "precedence": 65, "associativity": "left", "operators": ["-"] }},
      {{ "precedence": 90, "associativity": "right", "arity": "prefix", "operators": ["-"] }}
    ]
  }},
  "syncTokens": [ "Semi" ],
  "shapes": {{
    {0}
    "root":      {{ "sequence": [ {{ "repeat": "stmt" }} ] }},
    "stmt":      {{ "alt": [ "vardecl", "aliasdecl", "exprStmt" ] }},
    "vardecl":   {{ "sequence": [ "LetKw", "Identifier", "Semi" ] }},
    "aliasdecl": {{ "sequence": [ "AliasKw", "Identifier", "Semi" ] }},
    "exprStmt":  {{ "sequence": [ "expression", "Semi" ] }},
    "expression": {{ "expr": {{ "atom": "operand",
        "wrapperRules": {{ "binary": "binaryExpr", "unary": "unaryExpr", "postfix": "postfixExpr" }} }} }},
    "operand": {{
      "alt": [ "castish", "parenish", "Identifier", "IntLiteral" ],
      "speculative": true,
      "lookahead": {1}
    }},
    "castish": {{ {2},
      "commitRequiresTypeName": {{ "rule": "typePos", "polarity": "requireKnownType" }} }},
    "typePos":  {3},
    "parenish": {{ "sequence": [ "ParenOpen", "expression", "ParenClose" ] }},
    "castOperand": {{ "expr": {{ "atom": "operand", "minPrecedence": 90,
        "wrapperRules": {{ "binary": "binaryExpr", "unary": "unaryExpr", "postfix": "postfixExpr" }} }} }}
  }},
  "semantics": {{
    {4}
    "identifierToken": "Identifier",
    "declarations": [
      {{ "rule": "vardecl",   "name": 1, "kind": "variable"{5} }},
      {{ "rule": "aliasdecl", "name": 1, "kind": "type" }}
    ]
  }}
}})JSON",
                       s.extraShapes, s.lookahead, s.castish, s.typePos, s.attributeSemantics,
                       s.vardeclRowExtra);
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> synth(Synth const& s) {
    return fromText(synthText(s), "<hoist-synth>");
}

// `x` is a variable (a VALUE to the sketch), `A` a type.
constexpr char const* kSynthValueCast = "let x;\nalias A;\n(x) - 1;\n";

// One condition's verdict: the CONTROL (the condition met) must let the hoist
// fire, the VARIANT (it alone failing) must not, and both parse alike on and off.
void expectTheConditionBites(std::shared_ptr<GrammarSchema const> const& control,
                             std::shared_ptr<GrammarSchema const> const& variant, std::string const& input,
                             std::optional<std::size_t> variantBudgetFactor = std::nullopt) {
    ASSERT_NE(control, nullptr);
    ASSERT_NE(variant, nullptr);
    OnOff const c = onOff(control, input);
    EXPECT_EQ(c.on, c.off) << "the control parses differently with the hoist";
    EXPECT_LT(c.probesOn, c.probesOff) << "the CONTROL does not let the hoist fire, so the variant proves nothing";
    OnOff const v = onOff(variant, input, variantBudgetFactor);
    EXPECT_EQ(v.on, v.off) << "the variant parses differently with the hoist";
    EXPECT_EQ(v.probesOn, v.probesOff) << "the hoist fired where its condition fails";
}

[[nodiscard]] std::string readShippedText(std::string_view name) {
    auto const root = dss::test::findConfigRoot();
    EXPECT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    if (!root) return {};
    std::ifstream in{*root / "sources" / (std::string{name} + ".lang.json"), std::ios::binary};
    std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::erase(text, '\r');  // a CRLF checkout: JSON does not care, the edits below do
    return text;
}

// `text` with `before` replaced by `after`, `before` occurring exactly once.
[[nodiscard]] std::string replacedOnce(std::string text, std::string_view before, std::string_view after) {
    std::size_t const at = text.find(before);
    EXPECT_NE(at, std::string::npos) << "the edit's text is not in the document: " << before;
    if (at == std::string::npos) return text;
    EXPECT_EQ(text.find(before, at + 1), std::string::npos) << "the edit's text occurs twice: " << before;
    text.replace(at, before.size(), after);
    return text;
}

} // namespace

// ── (A) ─────────────────────────────────────────────────────────────────
TEST(ParserTriageHoist, TheReachIsReadOffTheShippedData) {
    std::map<std::string, std::size_t> const expectedC = {
        {"alignasTypeName", 0},       {"castExpr", 1},          {"compoundLiteralExpr", 1},
        {"enumTypeSpecifier", 1},     {"forIdentDecl", 0},      {"genericTypedAssoc", 0}, {"identVarDecl", 0},
        {"sizeofType", 2},            {"typeNameBareEmptyDecl", 0}, {"typeNameBareEmptyForDecl", 0},
    };
    std::map<std::string, std::size_t> const none;
    for (std::string const lang : {"c", "toy", "tsql-subset", "asm-x86_64-att", "asm-arm64-gas"}) {
        SCOPED_TRACE(lang);
        auto const schema = shipped(lang);
        ASSERT_NE(schema, nullptr);
        std::map<std::string, std::size_t> reach;
        for (auto const& name : schema->rules()) {
            RuleId const r = schema->rules().find(name);
            auto const s = schema->typeChildLeadRunLength(r);
            if (s.has_value() && schema->typeBaseAskable(r) && !schema->isExprRule(r)) {
                reach.emplace(std::string{name}, *s);
                std::printf("REACH %s %s s=%zu\n", lang.c_str(), std::string{name}.c_str(), *s);
            }
        }
        EXPECT_EQ(reach, lang == "c" ? expectedC : none);
    }
}

// ── (B) ─────────────────────────────────────────────────────────────────
TEST(ParserTriageHoist, AValueBaseCostsNoProbeAtEveryRunLength) {
    auto const c = shipped("c");
    ASSERT_NE(c, nullptr);
    struct Case {
        char const* stmt;
        char const* why;
    };
    Case const cases[] = {
        {"x = 1;", "s=0: identVarDecl and the bare empty declaration, base x"},
        {"f(x);", "s=0: the same two, base f"},
        {"v;", "s=0: the same two, base v"},
        {"for (x = 0; x < 1; ++x) {}", "s=0: forIdentDecl and the bare empty for-init, base x"},
        {"_Alignas(N) int a;", "s=0: alignasTypeName, base N (an enumerator)"},
        {"return (v) + 1;", "s=1: castExpr and compoundLiteralExpr, base v"},
        {"struct S { enum E : N; } s;", "s=1: enumTypeSpecifier, base N"},
        {"return (int)sizeof - v;", "s=1 and s=2 absent: the control for the next row (no hoist)"},
        {"return (int)sizeof(v);", "s=2: sizeofType, base v (and s=1 inside sizeof's value form)"},
    };
    for (Case const& k : cases) {
        SCOPED_TRACE(k.stmt);
        OnOff const r = onOff(c, inC(k.stmt));
        EXPECT_EQ(r.on, r.off) << "the hoist changed the parse";
        std::printf("VALUE %-32s probes on %llu off %llu  (%s)\n", k.stmt,
                    static_cast<unsigned long long>(r.probesOn), static_cast<unsigned long long>(r.probesOff),
                    k.why);
        if (std::string_view{k.stmt} == "return (int)sizeof - v;") {
            EXPECT_EQ(r.probesOn, r.probesOff) << k.why;
        } else {
            EXPECT_LT(r.probesOn, r.probesOff) << "no probe saved: " << k.why;
        }
    }
}

// ── (C) ─────────────────────────────────────────────────────────────────
TEST(ParserTriageHoist, ATypeOrUnknownBaseIsLeftToItsProbe) {
    auto const c = shipped("c");
    ASSERT_NE(c, nullptr);
    for (char const* stmt : {"T + 1;", "return (T) / 2;", "T * = 5;", "T y = 0;", "T;", "u;", "u = 1;", "return (u)42;",
                             "return (T)1;", "return (int)sizeof(T);", "return (int)sizeof(u);"}) {
        SCOPED_TRACE(stmt);
        OnOff const r = onOff(c, inC(stmt));
        EXPECT_EQ(r.on, r.off) << "the hoist changed the parse";
        EXPECT_EQ(r.probesOn, r.probesOff) << "the hoist refuted a candidate whose base is no value";
    }
}

// ── (D) ─────────────────────────────────────────────────────────────────
TEST(ParserTriageHoist, TheStaticHalfIsOneLoadTimeAnswer) {
    auto const base = synth(Synth{});
    ASSERT_NE(base, nullptr);
    RuleId const castish = base->rules().find("castish");
    EXPECT_TRUE(base->typeBaseAskable(castish));
    EXPECT_EQ(base->typeChildLeadRunLength(castish), std::optional<std::size_t>{1});
    SchemaTokenId const open  = base->schemaTokens().find("ParenOpen");
    SchemaTokenId const close = base->schemaTokens().find("ParenClose");
    EXPECT_TRUE(base->typeChildLeadRunAdmits(castish, 0, open));
    EXPECT_FALSE(base->typeChildLeadRunAdmits(castish, 0, close));
    EXPECT_FALSE(base->typeChildLeadRunAdmits(castish, 1, open)) << "past the run";
    // No guard, no reach: the rule the type child names, and a rule without a guard.
    EXPECT_FALSE(base->typeBaseAskable(base->rules().find("parenish")));
    EXPECT_FALSE(base->typeChildLeadRunLength(base->rules().find("parenish")).has_value());
}

TEST(ParserTriageHoist, ATypeRuleThatDerivesNothingIsNotAsked) {
    Synth nullable;
    nullable.typePos = R"({ "sequence": [ { "optional": "Identifier" } ] })";
    auto const variant = synth(nullable);
    ASSERT_NE(variant, nullptr);
    EXPECT_FALSE(variant->typeBaseAskable(variant->rules().find("castish")));
    expectTheConditionBites(synth(Synth{}), variant, kSynthValueCast);
}

TEST(ParserTriageHoist, AnAttributeThatStartsWithANameStopsTheAsk) {
    auto const withAttr = [](char const* lead) {
        Synth s;
        s.extraShapes = std::format(R"("attrish": {{ "sequence": [ "{}", "Semi" ] }},
    "stdish":  {{ "sequence": [ "BaseKw", "Semi" ] }},
    "bareish": {{ "sequence": [ "BaseKw", "Semi" ] }},)",
                                    lead);
        s.attributeSemantics =
            R"("attributeSemantics": { "attrSpecRule": "attrish", "stdAttrRule": "stdish", "bareStatementRule": "bareish", "effects": [] },)";
        return synth(s);
    };
    auto const keywordLed = withAttr("AliasKw");
    auto const nameLed    = withAttr("Identifier");
    ASSERT_NE(nameLed, nullptr);
    EXPECT_FALSE(nameLed->typeBaseAskable(nameLed->rules().find("castish")));
    expectTheConditionBites(keywordLed, nameLed, kSynthValueCast);
}

TEST(ParserTriageHoist, ADefinitionBodyCandidateTakesItsProbe) {
    Synth body;
    body.vardeclRowExtra = R"(, "kindByChild": { "child": 1, "whenRule": "castish", "whenKind": "function" })";
    expectTheConditionBites(synth(Synth{}), synth(body), kSynthValueCast);
}

TEST(ParserTriageHoist, ARunPastTheProbeBudgetTakesItsProbe) {
    // Budget = the site's lookahead times the factor: 16 x 0 = 0 < the run's 1.
    expectTheConditionBites(synth(Synth{}), synth(Synth{}), kSynthValueCast, /*variantBudgetFactor=*/0);
}

TEST(ParserTriageHoist, ARunWhoseTokensDoNotMatchTakesItsProbe) {
    // A two-token run, at a site whose lookahead (1) prunes nothing past FIRST, so
    // the candidate reaches the hoist on `(-x)`: the run fails at `-`, the base
    // offset holds the value `x`.
    Synth two;
    two.castish   = R"("sequence": [ "ParenOpen", "ParenOpen", "typePos", "ParenClose", "ParenClose", "castOperand" ])";
    two.lookahead = 1;
    auto const s = synth(two);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->typeChildLeadRunLength(s->rules().find("castish")), std::optional<std::size_t>{2});
    OnOff const matched = onOff(s, "let x;\n((x)) - 1;\n");
    EXPECT_EQ(matched.on, matched.off);
    EXPECT_LT(matched.probesOn, matched.probesOff) << "the control: a matching run with a value base";
    OnOff const mismatched = onOff(s, "let x;\n(-x) - 1;\n");
    EXPECT_EQ(mismatched.on, mismatched.off);
    EXPECT_EQ(mismatched.probesOn, mismatched.probesOff) << "the hoist refuted a run its tokens do not match";
}

// ── (E) ─────────────────────────────────────────────────────────────────
TEST(ParserTriageHoist, TheGrammarTokenViewLooksThroughABodyModesLeaves) {
    std::string const doc = replacedOnce(readShippedText("tsql-subset"),
                                         R"("nameOrCall": {
      "alt": [ "callExpr", "qualifiedName" ],
      "speculative": true
    },)",
                                         R"("nameOrCall": {
      "alt": [ "bracketCall", "callExpr", "qualifiedName" ],
      "speculative": true,
      "lookahead": 4
    },

    "bracketCall": { "sequence": [ "BracketIdStart", "ParenOpen", "ParenClose" ] },)");
    auto const schema = fromText(doc, "<tsql-bracket-call>");
    ASSERT_NE(schema, nullptr);
    for (bool const hoist : {true, false}) {
        SCOPED_TRACE(hoist ? "hoist on" : "hoist off");
        ParseResult const call = parse(schema, "SELECT [a]() FROM t;\n", configFor(*schema, hoist));
        EXPECT_FALSE(call.tree.diagnostics().hasErrors()) << render(call.tree);
        EXPECT_TRUE(hasRule(call.tree, "bracketCall")) << "the call was pruned on the bracket name's body";
        ParseResult const name = parse(schema, "SELECT [a] FROM t;\n", configFor(*schema, hoist));
        EXPECT_FALSE(name.tree.diagnostics().hasErrors()) << render(name.tree);
        EXPECT_FALSE(hasRule(name.tree, "bracketCall")) << "the control: a bracket name is no call";
    }
}
