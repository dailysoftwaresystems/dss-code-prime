// SE2 acceptance: c-subset language end-to-end via the same
// SchemaDrivenSemantics engine — proves zero per-language C++ is needed
// to add a new language with built-in types, lexical block scopes, and
// typed literals.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_visitor.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"

#include <gtest/gtest.h>

using namespace dss;
using namespace dss::sem_test;

// `int x;` inside a function body parses through varDecl → varDeclHead,
// which the c-subset `semantics` block declares as a Variable decl
// (name=1, type=0). Should mint one symbol typed I32. (Top-level
// decls in c-subset's grammar are an inline `sequence` under
// `topLevel.alt`, NOT a named rule — they fall through to SE5/SE6
// when we introduce named topLevelDecl/functionDecl wrapper rules.)
TEST(SemanticAnalyzerCSubset, FunctionLocalIntDeclTypedAsI32) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_EQ(model.symbols().size() - 1, 1u);
    auto const& rec = model.symbols()[1];
    EXPECT_EQ(rec.name, "x");
    ASSERT_TRUE(rec.type.valid()) << "the int builtin must resolve to a TypeId";
    EXPECT_EQ(model.lattice().interner().kind(rec.type), TypeKind::I32);
}

// `int x;` in two DIFFERENT blocks is NOT a redecl — c-subset's
// `block` is declared as a scope opener in the language semantics, so
// each nested block produces its own ScopeId and same-name decls are
// independent symbols.
TEST(SemanticAnalyzerCSubset, NestedBlocksShadowWithoutRedecl) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() {\n"
        "    int x;\n"
        "    { int x; }\n"
        "}\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "different blocks → different scopes → no shadow redecl";
    // Two distinct symbols minted (one per scope).
    EXPECT_EQ(model.symbols().size() - 1, 2u);
}

// Use-before-decl inside the same scope resolves through Pass 1's
// pre-minting (G-209 forward refs). Also asserts the use of `x` binds to
// the EXACT declared symbol AND inherits its I32 type — not just "no
// undeclared diagnostic".
TEST(SemanticAnalyzerCSubset, ForwardReferenceWithinBlock) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { x; int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);

    ASSERT_EQ(model.symbols().size() - 1, 1u);
    SymbolId const xSym{1};
    EXPECT_EQ(model.symbols()[xSym.v].name, "x");

    // Find the `x` USE leaf (the `x;` statement, which precedes the decl).
    // The decl's own name leaf also carries xSym; we want a leaf whose
    // node differs from the decl name node. Both should bind to xSym.
    Tree const& tree = cu->trees()[0];
    NodeId declName = model.symbols()[xSym.v].declNode;
    int boundUses = 0;
    walkPreOrder(tree, [&](TreeCursor const& cursor) {
        NodeId const n = cursor.current();
        if (tree.kind(n) != NodeKind::Token || tree.text(n) != "x") return;
        if (n.v == declName.v) return;  // skip the decl's own name leaf
        EXPECT_EQ(model.symbolAt(n).v, xSym.v) << "use of x binds to x's decl";
        EXPECT_EQ(model.lattice().interner().kind(model.typeAt(n)), TypeKind::I32)
            << "use inherits the declared I32 type";
        ++boundUses;
    });
    EXPECT_GE(boundUses, 1) << "the `x;` use site must be present and bound";
}

// IntLiteral and FloatLiteral leaves get the configured TypeId.
TEST(SemanticAnalyzerCSubset, LiteralsAreTyped) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { 42; 3.14; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);

    bool sawI32Lit = false;
    bool sawF64Lit = false;
    model.nodeToType().forEach([&](TreeId, NodeId, TypeId tid) {
        if (!tid.valid()) return;
        auto k = model.lattice().interner().kind(tid);
        if (k == TypeKind::I32) sawI32Lit = true;
        if (k == TypeKind::F64) sawF64Lit = true;
    });
    EXPECT_TRUE(sawI32Lit) << "IntLiteral must be typed I32 per the language semantics";
    EXPECT_TRUE(sawF64Lit) << "FloatLiteral must be typed F64 per the language semantics";
}

// Same-block redeclaration of `int x; int x;` IS a redecl error.
TEST(SemanticAnalyzerCSubset, SameBlockRedeclEmitsError) {
    auto cu = buildShippedUnit("c-subset", {
        "int main() { int x; int x; }\n",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u);
}
