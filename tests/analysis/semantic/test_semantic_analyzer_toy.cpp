// SE1 acceptance: toy language end-to-end through the
// SchemaDrivenSemantics engine.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <type_traits>

using namespace dss;
using namespace dss::sem_test;

// SemanticModel is move-only (the side-tables hold raw Tree*; copying
// would silently alias them).
static_assert(!std::is_copy_constructible_v<SemanticModel>);
static_assert(!std::is_copy_assignable_v<SemanticModel>);
static_assert( std::is_move_constructible_v<SemanticModel>);

// Single variable declaration: `var x = y;` — yields one variable
// symbol, no diagnostics (toy has no notion of undeclared identifiers
// in expressions because every right-hand-side ident becomes a use
// against the same scope).
TEST(SemanticAnalyzerToy, SingleVarDeclMintsOneSymbol) {
    auto cu = buildShippedUnit("toy", {"var x : int = x;"});
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // One variable symbol minted; no S_RedeclaredSymbol.
    EXPECT_EQ(model.symbols().size() - 1, 1u);   // -1 for the slot-0 sentinel
    EXPECT_FALSE(hasCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol));
    // `var x = x;` uses x BEFORE it's bound (toy's pass-1 binds at
    // varDecl entry, but the init is visited DURING that subtree —
    // so we don't actually use this test for forward refs; we just
    // assert the symbol got minted.
    auto const& syms = model.symbols();
    EXPECT_EQ(syms[1].name, "x");
}

// Two distinct decls in the same scope, same name → S_RedeclaredSymbol
// with a RelatedLocation pointing at the original.
TEST(SemanticAnalyzerToy, RedeclarationEmitsS_RedeclaredSymbol) {
    auto cu = buildShippedUnit("toy", {
        "var x : int = y; var x : int = z;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u);
    // Find the diagnostic and verify it carries a RelatedLocation.
    bool sawRelated = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_RedeclaredSymbol) {
            EXPECT_FALSE(d.related.empty())
                << "S_RedeclaredSymbol must carry a related-location to the prior decl";
            if (!d.related.empty()) sawRelated = true;
        }
    }
    EXPECT_TRUE(sawRelated);
}

// Forward reference across decls: var declarations in toy are minted in
// Pass 1 BEFORE Pass 2 resolves uses, so a use of `y` declared later
// resolves cleanly.
TEST(SemanticAnalyzerToy, ForwardReferenceResolves) {
    auto cu = buildShippedUnit("toy", {
        "var x : int = y; var y : int = x;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
    EXPECT_EQ(model.symbols().size() - 1, 2u);
}

// Use without decl → exactly one S_UndeclaredIdentifier per ungrounded
// identifier (`ghost` is unbound; the var-decl init `z` is also unbound
// here — toy's grammar mints `x` as a decl, but it doesn't pre-bind any
// other names). We use a corpus with exactly one undeclared use.
TEST(SemanticAnalyzerToy, UndeclaredUseEmitsExactlyOne) {
    auto cu = buildShippedUnit("toy", {
        "var x : int = x; var g : int = ghost;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // The init `x` resolves to the just-minted symbol; `ghost` is the
    // sole unresolved reference.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier) {
            EXPECT_EQ(d.actual, "ghost");
        }
    }
}

// Multi-tree CU isolation: toy has NO import mechanism, so two trees
// produce ZERO crossRef edges — each tree's top-level decls live in its
// own root scope and are invisible to the other. A use of `a` in tree 1
// (declared only in tree 0) is therefore UNDECLARED. This is the
// negative cross-file-visibility guard for a language with no imports.
TEST(SemanticAnalyzerToy, MultiTreeSymbolsAreIsolatedWithoutImports) {
    auto cu = buildShippedUnit("toy", {
        "var a : int = a;",        // tree 0 declares `a`
        "var b : int = a;",        // tree 1 uses `a` — must NOT resolve cross-tree
    });
    assertNoBuilderErrors(*cu);
    // No import edges between unrelated toy files.
    EXPECT_EQ(cu->crossRefs().size(), 0u);
    auto model = analyze(cu);
    // tree 1's `a` is unbound (the init of `var b = a;`); `b` itself and
    // tree 0's `a` self-init resolve. Exactly one undeclared use.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier) {
            EXPECT_EQ(d.actual, "a");
        }
    }
}

// Empty CU (no in-memory sources) — no diagnostics, model is well-formed.
TEST(SemanticAnalyzerToy, EmptyCuIsClean) {
    auto schema = loadShippedSchema("toy");
    UnitBuilder builder{schema};
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto model = analyze(cu);
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(model.symbols().size() - 1, 0u);
}

// The bound NodeId-keyed UnitAttribute aborts when handed a NodeId from
// a different CompilationUnit — this is the SE1 cross-CU guard.
TEST(SemanticAnalyzerToyDeathTest, ForeignNodeIdAborts) {
    auto cu = buildShippedUnit("toy", {"var x : int = x;"});
    auto model = analyze(cu);
    // A NodeId tagged with a TreeId that doesn't belong to this CU's
    // trees triggers the UnitAttribute crossUnitFatal path.
    NodeId foreign{1, /*tag=*/0xDEAD'BEEF};
    EXPECT_DEATH({ (void)model.symbolAt(foreign); },
                 "UnitAttribute.*not in this unit");
}
