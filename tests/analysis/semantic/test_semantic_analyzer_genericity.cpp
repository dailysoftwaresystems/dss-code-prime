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

// ── D-DECL-SPECIFIER-PREFIX-SUBSTRATE genericity (specifier-prefix decl) ────
//
// A miniature language `SpecSynth` whose declaration form `sdecl` carries an
// OPTIONAL leading declaration-specifier prefix (`specs` → the `stat`
// keyword), declared via the `specifierPrefix` facet. Proves the engine
// STRIPS that prefix before resolving the positional `type:0`/`name:1`
// indices — so a `static`-style specifier does not shift them. Rule/token
// names are non-shipped (engine-generic). RED-ON-DISABLE: delete the strip in
// `declRoleChildren` and the first test fails (name:1 hits the type node,
// type:0 hits the specifier).
namespace {

constexpr char kSpecPrefixSchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "SpecSynth", "version": "0.0.1", "fileExtensions": [".ssyn"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "=": [{ "kind": "Eq" }],
    ";": [{ "kind": "Semi" }]
  },
  "keywords": [
    { "word": "stat", "kind": "StatKw" },
    { "word": "wide", "kind": "WideKw" },
    { "word": "aa", "kind": "Word" },
    { "word": "cc", "kind": "Word" }
  ],
  "syncTokens": [ "Semi" ],
  "shapes": {
    "root":     { "sequence": [ { "repeat": "sdecl" } ] },
    "sdecl":    { "sequence": [ { "optional": "specs" }, "typeName", "Word", "Eq", "use", "Semi" ] },
    "specs":    { "sequence": [ "StatKw" ] },
    "typeName": { "alt": [ "WideKw" ] },
    "use":      { "sequence": [ "Word" ] }
  },
  "semantics": {
    "identifierToken": "Word",
    "declarations": [
      { "rule": "sdecl", "specifierPrefix": "specs",
        "type": 0, "name": 1, "init": 3, "kind": "variable" }
    ],
    "references": [ { "rule": "use" } ],
    "builtinTypes": [ { "name": "wide", "core": "I32" } ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<CompilationUnit const> buildSpecPrefixCu(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kSpecPrefixSchemaText, "<specsynth>");
    if (!loaded) {
        ADD_FAILURE() << "spec-prefix schema failed to load";
        std::abort();
    }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(source), "<specsynth-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}

} // namespace

// WITH a leading specifier: `stat wide aa = aa;` — the `stat` prefix is
// stripped, so type:0 → `wide` (I32) and name:1 → `aa`. RED-ON-DISABLE:
// without the strip, name:1 lands on the typeName node (symbol != "aa") and
// type:0 lands on the `stat` specifier (not a builtin type).
TEST(SemanticAnalyzerGenericity, SpecifierPrefixStrippedFromPositionalIndices) {
    auto cu = buildSpecPrefixCu("stat wide aa = aa;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_EQ(model.symbols().size() - 1, 1u);
    EXPECT_EQ(model.symbols()[1].name, "aa")
        << "name:1 resolves to the identifier AFTER the stripped specifier prefix";
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(model.symbols()[1].type), TypeKind::I32)
        << "type:0 resolves to the typeName (wide=I32) AFTER the stripped prefix";
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// WITHOUT a specifier: `wide cc = cc;` resolves identically — proving the
// strip is presence-gated (declRoleChildren == visibleChildren when the
// prefix is absent), so the substrate is inert for no-specifier declarations.
TEST(SemanticAnalyzerGenericity, SpecifierPrefixAbsentResolvesNormally) {
    auto cu = buildSpecPrefixCu("wide cc = cc;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_EQ(model.symbols().size() - 1, 1u);
    EXPECT_EQ(model.symbols()[1].name, "cc");
    auto const& interner = model.lattice().interner();
    EXPECT_EQ(interner.kind(model.symbols()[1].type), TypeKind::I32);
}

// Fail-loud: a `specifierPrefix` naming a rule the grammar does not define is
// rejected at load with C_UnknownShape (mirrors arraySuffix.rule validation).
TEST(SemanticAnalyzerGenericity, SpecifierPrefixUnknownRuleRejectedAtLoad) {
    std::string bad = kSpecPrefixSchemaText;
    constexpr char kFrom[] = "\"specifierPrefix\": \"specs\"";
    auto const pos = bad.find(kFrom);
    ASSERT_NE(pos, std::string::npos);
    bad.replace(pos, std::string(kFrom).size(),
                "\"specifierPrefix\": \"noSuchRule\"");
    auto loaded = GrammarSchema::loadFromText(bad, "<specsynth-bad>");
    ASSERT_FALSE(loaded) << "unknown specifierPrefix rule must fail the load";
    bool sawUnknownShape = false;
    for (auto const& d : loaded.error()) {
        if (d.code == DiagnosticCode::C_UnknownShape) sawUnknownShape = true;
    }
    EXPECT_TRUE(sawUnknownShape) << "unknown specifierPrefix rule → C_UnknownShape";
}

// ── SE4 + SE5 + SE6 genericity (a SECOND synthetic schema) ─────────────────
//
// A different miniature language `Synth2` proving const-correctness (SE4),
// typedef alias RESOLUTION (SE5), function declarations + FnSig + call
// checking + a variadic builtin (SE6) are 100% config-driven under
// NON-shipped rule/token names. None of the rule names (`fnDecl`,
// `letBind`, `aliasDecl`, `callExpr`, `useExpr`, `body`) nor the const
// marker token (`Lock`) nor the function keyword (`Fn`) overlap any
// shipped language. Identifiers are the `Word` token (not "Identifier").
//
// Grammar discipline: the only Word-leading statement ambiguity (call vs
// use vs assign) is resolved with a SHALLOW speculative `prim` alt — two
// tokens of lookahead, no Pratt operators — which the parser handles
// reliably (unlike a whole-function speculation).
namespace {

constexpr char kSynth2SchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "Synth2", "version": "0.0.1", "fileExtensions": [".syn2"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "=": [{ "kind": "Eq" }],
    ";": [{ "kind": "Semi" }],
    ",": [{ "kind": "Comma" }],
    "(": [{ "kind": "LParen", "opensScope": "Paren" }],
    ")": [{ "kind": "RParen", "closesScope": true }],
    "{": [{ "kind": "LBrace", "opensScope": "Block" }],
    "}": [{ "kind": "RBrace", "closesScope": true }]
  },
  "keywords": [
    { "word": "fn",   "kind": "Fn" },
    { "word": "var",  "kind": "VarKw" },
    { "word": "lock", "kind": "Lock" },
    { "word": "type", "kind": "TypeKw" },
    { "word": "i32",  "kind": "I32Kw" },
    { "word": "i8",   "kind": "I8Kw" },
    { "word": "aa", "kind": "Word" },
    { "word": "bb", "kind": "Word" },
    { "word": "cc", "kind": "Word" },
    { "word": "ff", "kind": "Word" },
    { "word": "SUM", "kind": "Word" },
    { "word": "ANY", "kind": "Word" }
  ],
  "syncTokens": [ "Semi" ],
  "shapes": {
    "root":     { "sequence": [ { "repeat": "stmt" } ] },
    "stmt":     { "alt": [ "fnDecl", "aliasDecl", "letBind", "exprStmt" ] },
    "fnDecl":   { "sequence": [ "Fn", "Word", "fnParams", "body" ] },
    "fnParams": { "sequence": [ "LParen", { "optional": "paramList" }, "RParen" ] },
    "paramList":{ "sequence": [ "param", { "repeat": { "sequence": [ "Comma", "param" ] } } ] },
    "param":    { "sequence": [ "typeName", "Word" ] },
    "body":     { "sequence": [ "LBrace", { "repeat": "stmt" }, "RBrace" ] },
    "aliasDecl":{ "sequence": [ "TypeKw", "Word", "Eq", "typeName", "Semi" ] },
    "letBind":  { "sequence": [ "VarKw", "typeName", "Word", "Eq", "expr", "Semi" ] },
    "exprStmt": { "sequence": [ "expr", "Semi" ] },
    "expr":     { "sequence": [ "prim", { "optional": { "sequence": [ "Eq", "prim" ] } } ] },
    "prim":     { "alt": [ "callExpr", "useExpr" ], "speculative": true },
    "callExpr": { "sequence": [ "Word", "LParen", { "optional": "argList" }, "RParen" ] },
    "argList":  { "sequence": [ "expr", { "repeat": { "sequence": [ "Comma", "expr" ] } } ] },
    "useExpr":  { "sequence": [ "Word" ] },
    "typeName": { "sequence": [ { "optional": "Lock" }, "typeCore" ] },
    "typeCore": { "alt": [ "I32Kw", "I8Kw", "Word" ] }
  },
  "semantics": {
    "identifierToken": "Word",
    "declarations": [
      { "rule": "fnDecl",  "name": 1, "params": 2, "body": 3, "kind": "function" },
      { "rule": "param",   "name": 1, "type": 0, "kind": "variable" },
      { "rule": "letBind", "name": 2, "type": 1, "init": 4, "kind": "variable",
        "constMarker": "Lock" },
      { "rule": "aliasDecl", "name": 1, "type": 3, "kind": "type" }
    ],
    "references": [ { "rule": "useExpr" } ],
    "scopes": [ "fnDecl", "body" ],
    "assignments": [ { "rule": "expr", "operatorToken": "Eq", "lhs": 0, "rhs": 2 } ],
    "callRules": [ { "rule": "callExpr", "callee": 0, "args": 2 } ],
    "builtinFunctions": [
      { "name": "SUM", "params": [ "I32", "I32" ], "result": "I32" },
      { "name": "ANY", "result": "I32", "variadic": true }
    ],
    "builtinTypes": [
      { "name": "i32", "core": "I32" },
      { "name": "i8",  "core": "I8"  }
    ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<CompilationUnit const> buildSynth2Cu(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kSynth2SchemaText, "<synth2>");
    if (!loaded) {
        ADD_FAILURE() << "synth2 schema failed to load";
        std::abort();
    }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(source), "<synth2-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}

[[nodiscard]] std::size_t cnt(SemanticModel const& m, DiagnosticCode c) {
    return countCode(m.diagnostics(), c);
}

} // namespace

// SE4 (generic): a `lock`-marked binding reassigned → exactly one
// S_ConstViolation; an unmarked binding reassigned → zero.
TEST(SemanticAnalyzerGenericity, Synth2ConstViolationFromConfig) {
    auto cu = buildSynth2Cu("var lock i32 aa = aa; aa = aa;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_ConstViolation), 1u);
}

TEST(SemanticAnalyzerGenericity, Synth2NonConstReassignIsClean) {
    auto cu = buildSynth2Cu("var i32 aa = aa; aa = aa;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_ConstViolation), 0u);
}

// SE5 (generic): `type aa = i32; i32 bb = bb;` then a use of the alias in
// type position resolves through the alias to I32. We assert the variable
// declared with the alias type carries I32.
TEST(SemanticAnalyzerGenericity, Synth2TypedefAliasResolvesToAliasedType) {
    // alias `aa` → i32; then `aa bb = bb;` declares bb with alias type.
    auto cu = buildSynth2Cu("type aa = i32; var aa bb = bb;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& interner = model.lattice().interner();
    SymbolRecord const* bbRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "bb") bbRec = &model.symbols()[i];
    }
    ASSERT_NE(bbRec, nullptr);
    ASSERT_TRUE(bbRec->type.valid()) << "alias `aa` must resolve to I32";
    EXPECT_EQ(interner.kind(bbRec->type), TypeKind::I32);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_UnknownType), 0u);
}

// SE5 (generic): an unknown type-position name → S_UnknownType.
TEST(SemanticAnalyzerGenericity, Synth2UnknownAliasEmitsUnknownType) {
    auto cu = buildSynth2Cu("var cc bb = bb;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_UnknownType), 1u);
}

// SE6 (generic): a function decl mints a Function symbol carrying a FnSig
// type whose params/result match the config-resolved types.
TEST(SemanticAnalyzerGenericity, Synth2FunctionDeclBuildsFnSig) {
    auto cu = buildSynth2Cu("fn ff(i32 aa, i8 bb) { aa; }");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& interner = model.lattice().interner();
    SymbolRecord const* ffRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "ff") ffRec = &model.symbols()[i];
    }
    ASSERT_NE(ffRec, nullptr);
    EXPECT_EQ(ffRec->kind, DeclarationKind::Function);
    ASSERT_TRUE(ffRec->type.valid());
    ASSERT_EQ(interner.kind(ffRec->type), TypeKind::FnSig);
    auto params = interner.fnParams(ffRec->type);
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(interner.kind(params[0]), TypeKind::I32);
    EXPECT_EQ(interner.kind(params[1]), TypeKind::I8);
}

// SE6 (generic): a correct-arity call to a user function → no diagnostic.
TEST(SemanticAnalyzerGenericity, Synth2CorrectCallIsClean) {
    auto cu = buildSynth2Cu("fn ff(i32 aa) { aa; } var i32 bb = bb; ff(bb);");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_NotCallable), 0u);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_ArgCountMismatch), 0u);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_TypeMismatch), 0u);
}

// SE6 (generic): wrong arg count → exactly one S_ArgCountMismatch.
TEST(SemanticAnalyzerGenericity, Synth2WrongArgCount) {
    auto cu = buildSynth2Cu("fn ff(i32 aa) { aa; } var i32 bb = bb; ff(bb, bb);");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_ArgCountMismatch), 1u);
}

// SE6 (generic): calling a non-function symbol → S_NotCallable.
TEST(SemanticAnalyzerGenericity, Synth2CallNonFunction) {
    auto cu = buildSynth2Cu("var i32 aa = aa; aa(aa);");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_NotCallable), 1u);
}

// SE5 (generic): MULTI-LEVEL alias chain. `type aa = i32; type bb = aa;`
// then `var bb cc = cc;` resolves cc's declared type through bb → aa →
// I32. This pins the recursive resolveTypeNode path through the SE5
// alias lookup branch — a single-step alias is not enough to catch a
// regression where the engine forgets to recurse on the aliased TypeId.
TEST(SemanticAnalyzerGenericity, Synth2MultiLevelAliasChainResolves) {
    auto cu = buildSynth2Cu("type aa = i32; type bb = aa; var bb cc = cc;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& interner = model.lattice().interner();
    SymbolRecord const* ccRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "cc") ccRec = &model.symbols()[i];
    }
    ASSERT_NE(ccRec, nullptr);
    ASSERT_TRUE(ccRec->type.valid());
    EXPECT_EQ(interner.kind(ccRec->type), TypeKind::I32)
        << "alias chain bb → aa → i32 must resolve to I32";
    EXPECT_EQ(cnt(model, DiagnosticCode::S_UnknownType), 0u);
}

// SE5 (generic): MUTUAL alias CYCLE. `type aa = bb; type bb = aa;` —
// the engine must NOT hang and must end with both `aa` and `bb`
// unresolvable. Using either alias in type position then emits
// S_UnknownType. Cycle resolution is deterministic in our SE5 model
// because both aliases' Pass 1.5 type resolution runs in post-order
// (the alias's own typeChild is just an identifier token, not an
// Internal type-shape, so each alias's RHS resolves via the typeBase
// path — and a Word in type position whose only resolution path is the
// scope-chain alias lookup returns InvalidType when the looked-up
// symbol itself has no valid type yet). With both unresolved, any
// USE-site of the alias in a `letBind` triggers S_UnknownType.
TEST(SemanticAnalyzerGenericity, Synth2MutualAliasCycleDoesNotHang) {
    auto cu = buildSynth2Cu("type aa = bb; type bb = aa; var aa cc = cc;");
    assertNoBuilderErrors(*cu);
    // The act of analyzing must terminate (no infinite loop) — death by
    // hang would manifest as a test-suite timeout, but the assertion
    // here is simply that we reach this line.
    auto model = analyze(cu);
    // The alias-in-use-site emits S_UnknownType because the cycle leaves
    // both aliases with no valid type.
    // Source `type aa = bb; type bb = aa; var aa cc = cc;` contains THREE
    // distinct type-position uses of unresolved aliases:
    //   - `bb` on the RHS of `type aa = bb;`
    //   - `aa` on the RHS of `type bb = aa;`
    //   - `aa` in the type position of `var aa cc = cc;`
    // Both `aa` and `bb` are stuck in the mutual cycle, so each of the
    // three sites lacks a resolvable type and emits S_UnknownType.
    // Strict equality so a future regression that changes the cycle's
    // resolution shape is loud.
    EXPECT_EQ(cnt(model, DiagnosticCode::S_UnknownType), 3u)
        << "every type-position use of an unresolved alias emits one S_UnknownType";
}

// SE6 (generic): the `kindByChild` discriminator facet under
// NON-shipped rule names. Synth3 declares a `topDecl` rule whose
// disambiguating child is `topTail = alt[fnTail, varTail]`. The schema
// uses `childPath: [2, 0]` to descend from topDecl → topTail → child to
// reach the discriminator-deciding rule (fnTail) — the same shape c-
// subset uses for its `topLevelDecl`. Proves the facet is purely
// config-driven, not tied to c-subset's specific names.
namespace {
constexpr char kSynth3SchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "Synth3", "version": "0.0.1", "fileExtensions": [".syn3"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "=": [{ "kind": "Eq" }],
    ";": [{ "kind": "Semi" }],
    ",": [{ "kind": "Comma" }],
    "(": [{ "kind": "LParen", "opensScope": "Paren" }],
    ")": [{ "kind": "RParen", "closesScope": true }],
    "{": [{ "kind": "LBrace", "opensScope": "Block" }],
    "}": [{ "kind": "RBrace", "closesScope": true }]
  },
  "keywords": [
    { "word": "wide", "kind": "Wide" },
    { "word": "aa", "kind": "Word" },
    { "word": "bb", "kind": "Word" },
    { "word": "ff", "kind": "Word" }
  ],
  "syncTokens": [ "Semi" ],
  "shapes": {
    "root":    { "sequence": [ { "repeat": "topDecl" } ] },
    "topDecl": { "sequence": [ "typeRef", "Word", "topTail" ] },
    "topTail": { "alt": [ "fnTail", "varTail" ] },
    "fnTail":  { "sequence": [ "fnParams", "body" ] },
    "varTail": { "sequence": [ "Semi" ] },
    "fnParams":{ "sequence": [ "LParen", { "optional": "paramList" }, "RParen" ] },
    "paramList":{ "sequence": [ "param", { "repeat": { "sequence": [ "Comma", "param" ] } } ] },
    "param":   { "sequence": [ "typeRef", "Word" ] },
    "body":    { "sequence": [ "LBrace", "RBrace" ] },
    "typeRef": { "alt": [ "Wide" ] }
  },
  "semantics": {
    "identifierToken": "Word",
    "declarations": [
      { "rule": "topDecl", "name": 1, "type": 0, "kind": "variable",
        "kindByChild": {
          "childPath": [2, 0],
          "whenRule": "fnTail",
          "whenKind": "function",
          "paramsPath": [0],
          "bodyPath":   [1]
        } },
      { "rule": "param", "name": 1, "type": 0, "kind": "variable" }
    ],
    "scopes": [ "fnTail", "body" ],
    "builtinTypes": [
      { "name": "wide", "core": "I32" }
    ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<CompilationUnit const> buildSynth3Cu(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kSynth3SchemaText, "<synth3>");
    if (!loaded) {
        ADD_FAILURE() << "synth3 schema failed to load: "
                      << (loaded.error().empty() ? "<no diagnostics>"
                                                : loaded.error()[0].message);
        std::abort();
    }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(source), "<synth3-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}
} // namespace

TEST(SemanticAnalyzerGenericity, Synth3KindByChildDiscriminatesVariableVsFunction) {
    // Two top-decls: a variable `aa` (varTail → Semi) and a function `ff`
    // (fnTail → fnParams + body). The kindByChild facet must mint `aa`
    // Variable and `ff` Function with a FnSig.
    auto cu = buildSynth3Cu("wide aa; wide ff(wide bb) { }");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& interner = model.lattice().interner();
    SymbolRecord const* aaRec = nullptr;
    SymbolRecord const* ffRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "aa") aaRec = &model.symbols()[i];
        if (model.symbols()[i].name == "ff") ffRec = &model.symbols()[i];
    }
    ASSERT_NE(aaRec, nullptr);
    ASSERT_NE(ffRec, nullptr);
    EXPECT_EQ(aaRec->kind, DeclarationKind::Variable)
        << "topDecl with varTail child → Variable";
    EXPECT_EQ(ffRec->kind, DeclarationKind::Function)
        << "topDecl with fnTail child → Function (via kindByChild)";
    ASSERT_TRUE(ffRec->type.valid());
    ASSERT_EQ(interner.kind(ffRec->type), TypeKind::FnSig);
    auto params = interner.fnParams(ffRec->type);
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(interner.kind(params[0]), TypeKind::I32);
    EXPECT_EQ(interner.kind(interner.fnResult(ffRec->type)), TypeKind::I32);
}

// SE6 (generic): paramsPath out-of-range MUST degrade safely. A schema
// whose `kindByChild.paramsPath` references a visible-child index that
// doesn't exist on the discriminator's matched subtree (here, [99] on
// fnTail, which has only 2 visible children) is the kind of config typo
// the engine pins as "no crash + 0 params" rather than crash or hang.
// descendVisible returns InvalidNode for any out-of-range step; the
// pass-1.5 resolver guards on `paramsNode.valid()` before collecting
// param types, so the resulting FnSig is correctly built with 0 params.
// This test pins THAT defensive fallback at the engine level — a future
// refactor that drops the guard would be caught here.
namespace {
constexpr char kSynth3BadParamsPathSchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "Synth3Bad", "version": "0.0.1", "fileExtensions": [".syn3b"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    "=": [{ "kind": "Eq" }],
    ";": [{ "kind": "Semi" }],
    ",": [{ "kind": "Comma" }],
    "(": [{ "kind": "LParen", "opensScope": "Paren" }],
    ")": [{ "kind": "RParen", "closesScope": true }],
    "{": [{ "kind": "LBrace", "opensScope": "Block" }],
    "}": [{ "kind": "RBrace", "closesScope": true }]
  },
  "keywords": [
    { "word": "wide", "kind": "Wide" },
    { "word": "aa", "kind": "Word" },
    { "word": "bb", "kind": "Word" },
    { "word": "ff", "kind": "Word" }
  ],
  "syncTokens": [ "Semi" ],
  "shapes": {
    "root":    { "sequence": [ { "repeat": "topDecl" } ] },
    "topDecl": { "sequence": [ "typeRef", "Word", "topTail" ] },
    "topTail": { "alt": [ "fnTail", "varTail" ] },
    "fnTail":  { "sequence": [ "fnParams", "body" ] },
    "varTail": { "sequence": [ "Semi" ] },
    "fnParams":{ "sequence": [ "LParen", { "optional": "paramList" }, "RParen" ] },
    "paramList":{ "sequence": [ "param", { "repeat": { "sequence": [ "Comma", "param" ] } } ] },
    "param":   { "sequence": [ "typeRef", "Word" ] },
    "body":    { "sequence": [ "LBrace", "RBrace" ] },
    "typeRef": { "alt": [ "Wide" ] }
  },
  "semantics": {
    "identifierToken": "Word",
    "declarations": [
      { "rule": "topDecl", "name": 1, "type": 0, "kind": "variable",
        "kindByChild": {
          "childPath": [2, 0],
          "whenRule": "fnTail",
          "whenKind": "function",
          "paramsPath": [99],
          "bodyPath":   [1]
        } },
      { "rule": "param", "name": 1, "type": 0, "kind": "variable" }
    ],
    "scopes": [ "fnTail", "body" ],
    "builtinTypes": [
      { "name": "wide", "core": "I32" }
    ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<CompilationUnit const> buildSynth3BadCu(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kSynth3BadParamsPathSchemaText,
                                              "<synth3-bad>");
    if (!loaded) {
        ADD_FAILURE() << "synth3-bad schema failed to load: "
                      << (loaded.error().empty() ? "<no diagnostics>"
                                                : loaded.error()[0].message);
        std::abort();
    }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(source), "<synth3b-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}
} // namespace

TEST(SemanticAnalyzerGenericity, Synth3KindByChildOutOfRangePathsNoOpsCleanly) {
    // Source: one function (`ff(wide bb)`) whose discriminator (fnTail)
    // matches and triggers the `paramsPath: [99]` descent. fnTail has
    // exactly 2 visible children (fnParams + body), so index 99 is the
    // canonical out-of-range case.
    auto cu = buildSynth3BadCu("wide ff(wide bb) { }");
    assertNoBuilderErrors(*cu);
    // Must terminate (no crash, no hang) — reaching the next line proves it.
    auto model = analyze(cu);
    auto const& interner = model.lattice().interner();
    SymbolRecord const* ffRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "ff") ffRec = &model.symbols()[i];
    }
    ASSERT_NE(ffRec, nullptr);
    // The kindByChild discriminator still MATCHED (fnTail rule lives at
    // childPath [2,0]), so the symbol is Function-kind with a FnSig —
    // but the FnSig carries ZERO params because paramsNode was
    // InvalidNode (descendVisible bailed on the bogus [99] step).
    EXPECT_EQ(ffRec->kind, DeclarationKind::Function)
        << "discriminator still matched; only the params descent failed";
    ASSERT_TRUE(ffRec->type.valid());
    ASSERT_EQ(interner.kind(ffRec->type), TypeKind::FnSig);
    EXPECT_EQ(interner.fnParams(ffRec->type).size(), 0u)
        << "an out-of-range paramsPath must yield a 0-param FnSig, not crash";
    EXPECT_EQ(interner.kind(interner.fnResult(ffRec->type)), TypeKind::I32);
}

// ── GAP A / C / D genericity (a FOURTH synthetic schema) ───────────────────
//
// Synth4 proves returnRules (GAP A), loopRules/loopControls (GAP C), and
// bracketIdentifierToken (GAP D) are 100% config-driven under NON-shipped
// rule/token names. None of `fun`/`retStmt`/`spin`/`halt`/`tag`/`nameRef`
// nor the `Word` identifier token nor the `Brk` bracket-id opener overlap
// any shipped language. The function result type threads to the body walk
// purely via the config (no hardcoded `returnStmt`/`while`/`BracketIdStart`).
namespace {
constexpr char kSynth4SchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "Synth4", "version": "0.0.1", "fileExtensions": [".syn4"] },
  "lexerModes": {
    "main":     { "tokens": "default" },
    "tag-body": { "defaultToken": { "kind": "TagChar" } }
  },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    ";": [{ "kind": "Semi" }],
    "{": [{ "kind": "LBrace", "opensScope": "Block" }],
    "}": [{ "kind": "RBrace", "closesScope": true }],
    "(": [{ "kind": "LParen", "opensScope": "Paren" }],
    ")": [{ "kind": "RParen", "closesScope": true }],
    "[": [{
      "kind": "Brk",
      "modeOp": "pushMode", "modeArg": "tag-body",
      "stringStyle": { "escapeKind": "none", "endsAt": "]" }
    }]
  },
  "keywords": [
    { "word": "fun",  "kind": "FunKw" },
    { "word": "ret",  "kind": "RetKw" },
    { "word": "spin", "kind": "SpinKw" },
    { "word": "halt", "kind": "HaltKw" },
    { "word": "next", "kind": "NextKw" },
    { "word": "wide", "kind": "WideKw" },
    { "word": "small","kind": "SmallKw" },
    { "word": "vd",   "kind": "VoidKw" },
    { "word": "aa", "kind": "Word" },
    { "word": "bb", "kind": "Word" },
    { "word": "ff", "kind": "Word" }
  ],
  "syncTokens": [ "Semi" ],
  "shapes": {
    "root":     { "sequence": [ { "repeat": "fun" } ] },
    "fun":      { "sequence": [ "typeName", "Word", "LParen", "RParen", "fbody" ] },
    "fbody":    { "sequence": [ "LBrace", { "repeat": "stmt" }, "RBrace" ] },
    "stmt":     { "alt": [ "retStmt", "spinStmt", "haltStmt", "nextStmt", "useStmt" ] },
    "retStmt":  { "sequence": [ "RetKw", { "optional": "nameRef" }, "Semi" ] },
    "spinStmt": { "sequence": [ "SpinKw", "LBrace", { "repeat": "stmt" }, "RBrace" ] },
    "haltStmt": { "sequence": [ "HaltKw", "Semi" ] },
    "nextStmt": { "sequence": [ "NextKw", "Semi" ] },
    "useStmt":  { "sequence": [ "nameRef", "Semi" ] },
    "nameRef":  { "sequence": [ "tag" ] },
    "tag":      { "alt": [ "Word", "Brk" ] },
    "typeName": { "alt": [ "WideKw", "SmallKw", "VoidKw" ] }
  },
  "semantics": {
    "identifierToken": "Word",
    "bracketIdentifierToken": "Brk",
    "declarations": [
      { "rule": "fun", "name": 1, "type": 0, "body": 4, "kind": "function" }
    ],
    "references": [ { "rule": "nameRef", "nameMatch": "lastIdentifier" } ],
    "scopes": [ "fbody" ],
    "returnRules": [ { "rule": "retStmt", "value": 1 } ],
    "loopRules": [ "spinStmt" ],
    "loopControls": [ { "rule": "haltStmt" }, { "rule": "nextStmt" } ],
    "builtinTypes": [
      { "name": "wide",  "core": "I32" },
      { "name": "small", "core": "I8"  },
      { "name": "vd",    "core": "Void" }
    ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<CompilationUnit const> buildSynth4Cu(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kSynth4SchemaText, "<synth4>");
    if (!loaded) {
        ADD_FAILURE() << "synth4 schema failed to load: "
                      << (loaded.error().empty() ? "<no diagnostics>"
                                                : loaded.error()[0].message);
        std::abort();
    }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(source), "<synth4-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}
} // namespace

// GAP A (generic): a bare `ret;` in a non-Void (`wide`) function →
// S_ReturnTypeMismatch; a `ret;` in a Void (`vd`) function → clean.
TEST(SemanticAnalyzerGenericity, Synth4ReturnRulesFromConfig) {
    {   // non-void function, bare return → mismatch
        auto cu = buildSynth4Cu("wide ff() { ret; }");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ReturnTypeMismatch), 1u);
    }
    {   // void function, bare return → clean
        auto cu = buildSynth4Cu("vd ff() { ret; }");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ReturnTypeMismatch), 0u);
    }
}

// GAP A (generic): returning a value from a Void function → mismatch. The
// returned `aa` is a (forward-declared-as-undeclared) name use; we declare
// it as a param-free use that resolves to nothing, so its type is Invalid —
// to force a typed value we instead return the function itself is awkward,
// so use a non-void function returning a value of the WRONG width: a
// `small` (I8) function returning a `wide`-typed... but Synth4 has no
// locals. Simplest: a Void function returning ANY value node → mismatch
// regardless of the value's resolved type (the Void rule fires first).
TEST(SemanticAnalyzerGenericity, Synth4ValueReturnInVoidIsMismatch) {
    auto cu = buildSynth4Cu("vd ff() { ret aa; }");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_ReturnTypeMismatch), 1u);
}

// GAP C (generic): `halt;` inside a `spin { ... }` loop → clean; a bare
// `halt;` outside any loop → exactly one S_ControlOutsideLoop.
TEST(SemanticAnalyzerGenericity, Synth4LoopControlsFromConfig) {
    {   // inside the loop → clean
        auto cu = buildSynth4Cu("vd ff() { spin { halt; } }");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ControlOutsideLoop), 0u);
    }
    {   // outside any loop → one diagnostic
        auto cu = buildSynth4Cu("vd ff() { halt; }");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ControlOutsideLoop), 1u);
    }
}

// FIX 4 (multi-entry loopControls): Synth4 declares TWO loopControls —
// `halt` (break-style) and `next` (continue-style). BOTH must emit
// S_ControlOutsideLoop outside a loop AND be clean inside the `spin` loop.
// Single-entry coverage above can't catch a regression that only honored the
// first loopControls entry; this exercises the second entry too.
TEST(SemanticAnalyzerGenericity, Synth4MultipleLoopControlsFromConfig) {
    {   // both controls inside the loop → clean
        auto cu = buildSynth4Cu("vd ff() { spin { halt; next; } }");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ControlOutsideLoop), 0u)
            << "both halt and next are valid inside the loop context";
    }
    {   // both controls outside any loop → one diagnostic EACH (two total)
        auto cu = buildSynth4Cu("vd ff() { halt; next; }");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ControlOutsideLoop), 2u)
            << "each of the two loopControls fires outside a loop — proving "
               "multi-entry loopControls works, not just the first entry";
    }
}

// GAP D (generic): a bracket-id `[aa]` reference resolves against a
// bracket-id declaration name — proving bracketIdentifierToken is not
// tsql-coupled. We declare a function named via a plain Word and reference
// it via a bracket-id `[ff]`; the lookup must find it (no undeclared).
TEST(SemanticAnalyzerGenericity, Synth4BracketIdentifierResolves) {
    auto cu = buildSynth4Cu("wide ff() { [ff]; ret; }");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // `[ff]` resolves to the function symbol `ff` (bracket text stripped).
    EXPECT_EQ(cnt(model, DiagnosticCode::S_UndeclaredIdentifier), 0u);
    // A bracket-id reference to an unknown name IS loud.
    auto cu2 = buildSynth4Cu("wide ff() { [bb]; ret; }");
    assertNoBuilderErrors(*cu2);
    auto model2 = analyze(cu2);
    EXPECT_EQ(cnt(model2, DiagnosticCode::S_UndeclaredIdentifier), 1u);
}

// SE6 (generic): a fixed-arity builtin (SUM: (I32,I32)->I32) accepts the
// right count and rejects the wrong one; a variadic builtin (ANY) accepts
// any arity. Builtins are bound CU-wide and shadowable.
TEST(SemanticAnalyzerGenericity, Synth2BuiltinFunctionArity) {
    {
        auto cu = buildSynth2Cu("var i32 aa = aa; SUM(aa, aa);");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_NotCallable), 0u);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ArgCountMismatch), 0u);
    }
    {
        auto cu = buildSynth2Cu("var i32 aa = aa; SUM(aa);");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ArgCountMismatch), 1u);
    }
    {   // variadic ANY accepts 0..N args.
        auto cu = buildSynth2Cu("var i32 aa = aa; ANY(aa, aa, aa);");
        assertNoBuilderErrors(*cu);
        auto model = analyze(cu);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_ArgCountMismatch), 0u);
        EXPECT_EQ(cnt(model, DiagnosticCode::S_NotCallable), 0u);
    }
}

// ── D8 genericity (a FIFTH synthetic schema) ───────────────────────────────
//
// Synth5 proves the `warnIfUnused` unused-variable facet (D8) is 100%
// config-driven under NON-shipped rule/token names. None of `localDecl`,
// `slotDecl`, `useRef` nor the `Word` identifier token overlap any shipped
// language. TWO declaration forms exercise the per-declaration opt-in:
//   - `localDecl` (`loc <Word> ;`) sets `warnIfUnused: true`  → warns.
//   - `slotDecl`  (`slot <Word> ;`) omits it (default false)  → never warns.
// If the engine hardcoded c-subset's `varDeclHead` name (or "Identifier"),
// these would fail — proving the facet is language-agnostic.
namespace {
constexpr char kSynth5SchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "Synth5", "version": "0.0.1", "fileExtensions": [".syn5"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    ";": [{ "kind": "Semi" }]
  },
  "keywords": [
    { "word": "loc",  "kind": "LocKw" },
    { "word": "slot", "kind": "SlotKw" },
    { "word": "aa", "kind": "Word" },
    { "word": "bb", "kind": "Word" },
    { "word": "cc", "kind": "Word" }
  ],
  "syncTokens": [ "Semi" ],
  "shapes": {
    "root":      { "sequence": [ { "repeat": "stmt" } ] },
    "stmt":      { "alt": [ "localDecl", "slotDecl", "useStmt" ] },
    "localDecl": { "sequence": [ "LocKw", "Word", "Semi" ] },
    "slotDecl":  { "sequence": [ "SlotKw", "Word", "Semi" ] },
    "useStmt":   { "sequence": [ "useRef", "Semi" ] },
    "useRef":    { "sequence": [ "Word" ] }
  },
  "semantics": {
    "identifierToken": "Word",
    "declarations": [
      { "rule": "localDecl", "name": 1, "kind": "variable", "warnIfUnused": true },
      { "rule": "slotDecl",  "name": 1, "kind": "variable" }
    ],
    "references": [ { "rule": "useRef" } ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<CompilationUnit const> buildSynth5Cu(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kSynth5SchemaText, "<synth5>");
    if (!loaded) {
        ADD_FAILURE() << "synth5 schema failed to load: "
                      << (loaded.error().empty() ? "<no diagnostics>"
                                                : loaded.error()[0].message);
        std::abort();
    }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(source), "<synth5-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}
} // namespace

// D8 (generic): a `loc aa;` whose `aa` is never referenced → exactly one
// S_UnusedVariable naming `aa`; a referenced `loc bb; bb;` → no warning for
// bb. Proves the facet fires purely from the synthetic schema's opt-in.
TEST(SemanticAnalyzerGenericity, Synth5UnusedVariableFromConfig) {
    auto cu = buildSynth5Cu("loc aa; loc bb; bb;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // `aa` unused → one warning; `bb` used → none.
    EXPECT_EQ(cnt(model, DiagnosticCode::S_UnusedVariable), 1u);
    ParseDiagnostic const* d = nullptr;
    for (auto const& diag : model.diagnostics().all()) {
        if (diag.code == DiagnosticCode::S_UnusedVariable) d = &diag;
    }
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->actual, "aa");
    EXPECT_EQ(d->severity, DiagnosticSeverity::Warning);
}

// D8 (generic): the per-declaration opt-in. `slot cc;` (a NON-opted-in
// declaration form) whose `cc` is unused → ZERO warnings, even though the
// use-set is just as empty as an opted-in local's. Proves the warning is
// gated by the declaration's `warnIfUnused`, not by emptiness alone.
TEST(SemanticAnalyzerGenericity, Synth5NonOptedInDeclDoesNotWarn) {
    auto cu = buildSynth5Cu("slot cc;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(cnt(model, DiagnosticCode::S_UnusedVariable), 0u)
        << "a declaration form that does not opt in never warns";
}

// ── builtinTypes.extension genericity (a SIXTH synthetic schema) ────────────
//
// Synth6 proves the `builtinTypes.extension` facet (a type name resolving to
// a registered `typeExtensions[]` entry rather than a core-lattice primitive)
// is 100% config-driven under NON-shipped vocabulary. The extension name
// (`Demo::Money`), the type keyword (`cash`), the declaration rule (`hold`),
// and the identifier token (`Word`) overlap nothing in tsql/c-subset. If the
// facet were shaped around tsql's VARCHAR / TSQL::Varchar, the resolved type
// would not carry the arbitrary `Demo::Money` name — this pins that it does.
namespace {
constexpr char kSynth6SchemaText[] = R"JSON({
  "dssSchemaVersion": 4,
  "language": { "name": "Synth6", "version": "0.0.1", "fileExtensions": [".syn6"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\t": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "\n": [{ "kind": "Newline",    "flags": ["EmptySpace"] }],
    ";": [{ "kind": "Semi" }]
  },
  "keywords": [
    { "word": "cash", "kind": "CashKw" },
    { "word": "aa", "kind": "Word" }
  ],
  "syncTokens": [ "Semi" ],
  "typeExtensions": [
    { "name": "Demo::Money" }
  ],
  "shapes": {
    "root":     { "sequence": [ { "repeat": "hold" } ] },
    "hold":     { "sequence": [ "typeName", "Word", "Semi" ] },
    "typeName": { "alt": [ "CashKw" ] }
  },
  "semantics": {
    "identifierToken": "Word",
    "declarations": [
      { "rule": "hold", "type": 0, "name": 1, "kind": "variable" }
    ],
    "builtinTypes": [
      { "name": "cash", "extension": "Demo::Money" }
    ]
  }
})JSON";

[[nodiscard]] std::shared_ptr<CompilationUnit const> buildSynth6Cu(std::string source) {
    auto loaded = GrammarSchema::loadFromText(kSynth6SchemaText, "<synth6>");
    if (!loaded) {
        ADD_FAILURE() << "synth6 schema failed to load: "
                      << (loaded.error().empty() ? "<no diagnostics>"
                                                : loaded.error()[0].message);
        std::abort();
    }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(source), "<synth6-mem>");
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}
} // namespace

// `cash aa;` declares `aa` of the extension type `Demo::Money`. The symbol's
// resolved type must be `TypeKind::Extension` with the exact extension name,
// and the type position must NOT emit S_UnknownType — proving the facet maps
// an arbitrary type keyword to an arbitrary registered extension, with no
// dependency on tsql's VARCHAR shape.
TEST(SemanticAnalyzerGenericity, Synth6ExtensionBuiltinTypeResolves) {
    auto cu = buildSynth6Cu("cash aa;");
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& interner = model.lattice().interner();

    SymbolId aaSym{};
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == "aa")
            aaSym = SymbolId{static_cast<std::uint32_t>(i)};
    }
    ASSERT_TRUE(aaSym.valid()) << "declaration `cash aa;` must mint a symbol";

    auto const aaType = model.symbols()[aaSym.v].type;
    ASSERT_TRUE(aaType.valid()) << "extension type must resolve, not stay Invalid";
    EXPECT_EQ(interner.kind(aaType), TypeKind::Extension);
    EXPECT_EQ(interner.name(aaType), "Demo::Money");

    EXPECT_EQ(cnt(model, DiagnosticCode::S_UnknownType), 0u)
        << "an extension-mapped type keyword must not be reported unknown";
}
