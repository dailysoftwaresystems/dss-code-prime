// SE1 GENERICITY guard: the analyzer must drive any schema-described
// language, not just the three shipped ones. We hand it a synthetic
// schema whose rule names (`bind` / `useStmt` / `tdecl`) deliberately do
// NOT overlap with any shipped language, AND whose identifier token is
// named `Word` (NOT "Identifier"). If anything in src/analysis/semantic/
// hardcoded the string "Identifier" or a shipped rule name, these tests
// would fail — they prove the engine is config-name-independent.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_visitor.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

using namespace dss;
using namespace dss::sem_test;

namespace {

// A miniature language declared inline. Identifiers are NOT the built-in
// "Identifier" token — they are a custom `Word` token kind (declared via
// keyword lexemes `aa`/`bb`/`cc`/`dd`), and the semantics block points
// `identifierToken` at "Word". Rule names are `bind`/`useStmt`/`use` —
// none shipped. A `tdecl` form carries an explicit `type` + `init` plus
// `builtinTypes` so the type-mismatch path is exercised purely from
// config. `scope` opens a fresh lexical scope (shadowing test).
constexpr char kSyntheticSchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "Synth", "version": "0.0.1", "fileExtensions": [".syn"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "=": [{ "kind": "Eq" }],
    ";": [{ "kind": "Semi" }],
    "{": [{ "kind": "LBrace", "opensScope": "Block" }],
    "}": [{ "kind": "RBrace", "closesScope": true }]
  },
  "keywords": [
    { "word": "let",  "kind": "LetKw" },
    { "word": "wide", "kind": "WideKw" },
    { "word": "narrow", "kind": "NarrowKw" },
    { "word": "aa", "kind": "Word" },
    { "word": "bb", "kind": "Word" },
    { "word": "cc", "kind": "Word" },
    { "word": "dd", "kind": "Word" }
  ],
  "syncTokens": [ "Semi" ],
  "shapes": {
    "root":    { "sequence": [ { "repeat": "stmt" } ] },
    "stmt":    { "alt": [ "tdecl", "bind", "useStmt", "scope" ] },
    "tdecl":   { "sequence": [ "typeName", "Word", "Eq", "use", "Semi" ] },
    "bind":    { "sequence": [ "LetKw", "Word", "Eq", "use", "Semi" ] },
    "useStmt": { "sequence": [ "use", "Semi" ] },
    "use":     { "sequence": [ "Word" ] },
    "typeName":{ "alt": [ "WideKw", "NarrowKw" ] },
    "scope":   { "sequence": [ "LBrace", { "repeat": "stmt" }, "RBrace" ] }
  },
  "semantics": {
    "identifierToken": "Word",
    "declarations": [
      { "rule": "tdecl", "type": 0, "name": 1, "init": 3, "kind": "variable" },
      { "rule": "bind", "name": 1, "init": 3, "kind": "variable" }
    ],
    "references": [
      { "rule": "use" }
    ],
    "scopes": [ "scope" ],
    "builtinTypes": [
      { "name": "wide",   "core": "I32" },
      { "name": "narrow", "core": "I8"  }
    ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<GrammarSchema const> loadSyntheticSchema() {
    auto loaded = GrammarSchema::loadFromText(kSyntheticSchemaText, "<synth>");
    if (!loaded) {
        ADD_FAILURE() << "synthetic schema failed to load";
        std::abort();
    }
    return *loaded;
}

[[nodiscard]] std::shared_ptr<CompilationUnit const> buildSynthCu(std::string source) {
    auto schema = loadSyntheticSchema();
    UnitBuilder builder{schema};
    builder.addInMemory(std::move(source), "<synth-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}

// Walk the single tree looking for a Word leaf with the given text whose
// parent is a `use` rule (a reference site, not a decl name). `wantLast`
// selects the last such leaf (for the shadowing test); otherwise the
// first. Returns the leaf NodeId so a test can assert the exact symbol/
// type that use resolves to.
[[nodiscard]] NodeId findUseLeaf(CompilationUnit const& cu, std::string_view text,
                                 bool wantLast = false) {
    Tree const& tree = cu.trees()[0];
    NodeId out{};
    walkPreOrder(tree, [&](TreeCursor const& cursor) {
        NodeId const n = cursor.current();
        if (out.valid() && !wantLast) return;
        if (tree.kind(n) != NodeKind::Token || tree.text(n) != text) return;
        NodeId parent = tree.parent(n);
        // A `use` rule wraps a single Word, so the parent's text equals
        // the leaf text — a cheap "is this leaf under a `use`" check.
        if (parent.valid() && tree.kind(parent) == NodeKind::Internal
            && tree.text(parent) == text) {
            out = n;
        }
    });
    return out;
}

} // namespace

// `let aa = bb;` — one symbol `aa` is minted; `bb` is undeclared → one
// S_UndeclaredIdentifier. Proves the engine handles declarations,
// references, and missing-decl reporting purely from the synthetic
// schema's `bind`/`use` rule names AND its `Word` (non-"Identifier")
// identifier token.
TEST(SemanticAnalyzerGenericity, SyntheticSchemaDrivesBindAndUse) {
    auto cu = buildSynthCu("let aa = bb;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_EQ(model.symbols().size() - 1, 1u);
    EXPECT_EQ(model.symbols()[1].name, "aa");
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier) {
            EXPECT_EQ(d.actual, "bb");
        }
    }
}

// Two synthetic decls with the same name → S_RedeclaredSymbol. Same
// engine, same diagnostic — driven by `bind`'s `nameChild=1`.
TEST(SemanticAnalyzerGenericity, SyntheticRedeclaration) {
    auto cu = buildSynthCu("let aa = aa; let aa = aa;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u);
}

// Forward reference across two `bind` statements resolves cleanly —
// Pass 1 mints both before Pass 2 resolves uses. ALSO asserts the use
// site binds to the EXACT declared SymbolId (resolution correctness, not
// just "no undeclared").
TEST(SemanticAnalyzerGenericity, SyntheticForwardReferenceBindsExactSymbol) {
    auto cu = buildSynthCu("let aa = bb; let bb = aa;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
    ASSERT_EQ(model.symbols().size() - 1, 2u);

    // The decl name leaves carry SymbolIds; find them by name.
    SymbolId aaSym{}, bbSym{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "aa") aaSym = SymbolId{static_cast<std::uint32_t>(i)};
        if (model.symbols()[i].name == "bb") bbSym = SymbolId{static_cast<std::uint32_t>(i)};
    }
    ASSERT_TRUE(aaSym.valid());
    ASSERT_TRUE(bbSym.valid());

    // The use of `bb` in `let aa = bb;` must bind to bbSym; the use of
    // `aa` in `let bb = aa;` must bind to aaSym.
    NodeId useBb = findUseLeaf(*cu, "bb");
    NodeId useAa = findUseLeaf(*cu, "aa");
    ASSERT_TRUE(useBb.valid());
    ASSERT_TRUE(useAa.valid());
    EXPECT_EQ(model.symbolAt(useBb).v, bbSym.v) << "use of bb binds to bb's decl";
    EXPECT_EQ(model.symbolAt(useAa).v, aaSym.v) << "use of aa binds to aa's decl";
}

// SHADOWING: an inner-scope decl shadows an outer one; a use inside the
// inner scope must bind to the INNER symbol.
TEST(SemanticAnalyzerGenericity, SyntheticInnerScopeShadows) {
    auto cu = buildSynthCu("let aa = aa; { let aa = aa; aa; }");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);

    // Two `aa` symbols minted: outer (scope=tree root) and inner (scope=block).
    SymbolId outer{}, inner{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& rec = model.symbols()[i];
        if (rec.name != "aa") continue;
        if (!outer.valid()) outer = SymbolId{static_cast<std::uint32_t>(i)};
        else                inner = SymbolId{static_cast<std::uint32_t>(i)};
    }
    ASSERT_TRUE(outer.valid());
    ASSERT_TRUE(inner.valid());
    EXPECT_NE(outer.v, inner.v);

    // The LAST `aa` use leaf (in `bb = aa;` inside the block) must resolve
    // to the inner symbol, not the outer.
    NodeId lastUse = findUseLeaf(*cu, "aa", /*wantLast=*/true);
    ASSERT_TRUE(lastUse.valid());
    EXPECT_EQ(model.symbolAt(lastUse).v, inner.v)
        << "inner-scope use binds to the inner (shadowing) symbol";
}

// TYPE PROPAGATION + S_TypeMismatch: a `tdecl` with an explicit type and
// an initializer whose inferred type does not assign into the declared
// type emits exactly one S_TypeMismatch; a widening-compatible init emits
// zero. Exercises the isAssignable engine path purely from config.
TEST(SemanticAnalyzerGenericity, SyntheticTypeMismatchOnNarrowingInit) {
    // `wide aa = aa;` declares aa : I32. `narrow bb = aa;` declares
    // bb : I8 and initializes it with aa (I32) — I32 does NOT assign into
    // I8 (narrowing), so exactly one S_TypeMismatch.
    auto cu = buildSynthCu("wide aa = aa; narrow bb = aa;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 1u);

    // The declared types propagate to the decl name leaves.
    auto const& interner = model.lattice().interner();
    SymbolId aaSym{}, bbSym{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "aa") aaSym = SymbolId{static_cast<std::uint32_t>(i)};
        if (model.symbols()[i].name == "bb") bbSym = SymbolId{static_cast<std::uint32_t>(i)};
    }
    ASSERT_TRUE(aaSym.valid());
    ASSERT_TRUE(bbSym.valid());
    EXPECT_EQ(interner.kind(model.symbols()[aaSym.v].type), TypeKind::I32);
    EXPECT_EQ(interner.kind(model.symbols()[bbSym.v].type), TypeKind::I8);
}

TEST(SemanticAnalyzerGenericity, SyntheticWideningInitIsClean) {
    // `narrow aa = aa; wide bb = aa;` — bb : I32 initialized from aa : I8.
    // I8 widens into I32, so ZERO S_TypeMismatch.
    auto cu = buildSynthCu("narrow aa = aa; wide bb = aa;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}
