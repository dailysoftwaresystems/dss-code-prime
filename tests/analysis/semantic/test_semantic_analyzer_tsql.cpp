// SE3 acceptance: tsql-subset language end-to-end. Exercises the
// `lastIdentifier` name matcher (`db.schema.table` → "table") for both
// declarations (CREATE TABLE) and references (qualifiedName).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"

#include <gtest/gtest.h>

using namespace dss;
using namespace dss::sem_test;

// CREATE TABLE → one symbol minted, kind == DeclarationKind::Table.
TEST(SemanticAnalyzerTsql, CreateTableMintsTableSymbol) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE Customers (Id INT NOT NULL, Name VARCHAR);",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_EQ(model.symbols().size() - 1, 1u);
    auto const& rec = model.symbols()[1];
    EXPECT_EQ(rec.name, "Customers");
    EXPECT_EQ(rec.kind, DeclarationKind::Table);
}

// `dbo.Customers` in a CREATE TABLE — the `lastIdentifier` matcher
// extracts `Customers` (the rightmost component of the dotted name).
TEST(SemanticAnalyzerTsql, LastIdentifierMatcherExtractsRightmost) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE dbo.Orders (Id INT);",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    ASSERT_EQ(model.symbols().size() - 1, 1u);
    EXPECT_EQ(model.symbols()[1].name, "Orders");
}

// Cross-statement (SAME tree): a CREATE TABLE and a SELECT referencing
// the same name in the SAME file resolve through that tree's root scope,
// with no S_UndeclaredIdentifier (no import edge needed — intra-tree).
TEST(SemanticAnalyzerTsql, CrossStatementTableReferenceResolves) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE Orders (Id INT);"
        "SELECT * FROM Orders;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// POSITIVE cross-FILE visibility: CREATE TABLE in tree 0, SELECT FROM in
// tree 1. tsql's NameMatching import strategy produces a crossRef edge
// (source = the referencing tree, target = the defining tree), which the
// analyzer's injection step uses to make Orders visible in tree 1.
TEST(SemanticAnalyzerTsql, CrossFileTableReferenceResolvesViaImportEdge) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE Orders (Id INT);",   // tree 0 defines Orders
        "SELECT * FROM Orders;",           // tree 1 references it
    });
    assertNoBuilderErrors(*cu);
    // The NameMatching resolver recorded a cross-tree edge.
    ASSERT_GE(cu->crossRefs().size(), 1u)
        << "tsql name-matching must produce a cross-file edge";
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "the import edge injects Orders into the referencing tree";
}

// NEGATIVE cross-FILE isolation: two SAME-NAMED tables in separate files
// with NO reference between them. Pre-Fix-1 (shared CU-wide root) this
// spuriously fired S_RedeclaredSymbol; with per-tree-root scopes the two
// `Orders` live in distinct scopes and do NOT collide, and no crossRef
// edge connects the files.
TEST(SemanticAnalyzerTsql, CrossFileSameNameTablesDoNotCollide) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE Orders (Id INT);",   // tree 0
        "CREATE TABLE Orders (Id INT);",   // tree 1 — same name, different file
    });
    assertNoBuilderErrors(*cu);
    // Neither file references the other → no cross-tree edge.
    EXPECT_EQ(cu->crossRefs().size(), 0u);
    auto model = analyze(cu);
    // Per-tree-root isolation: NOT a redeclaration.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u);
    // Two independent symbols minted in two distinct tree-root scopes.
    EXPECT_EQ(model.symbols().size() - 1, 2u);
}

// Two CREATE TABLEs with the same name in the same CU → exactly one
// S_RedeclaredSymbol with a RelatedLocation to the original.
TEST(SemanticAnalyzerTsql, DuplicateCreateTableEmitsRedecl) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE Orders (Id INT);"
        "CREATE TABLE Orders (Id INT);",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 1u);
    bool sawRelated = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_RedeclaredSymbol) {
            EXPECT_FALSE(d.related.empty());
            if (!d.related.empty()) sawRelated = true;
        }
    }
    EXPECT_TRUE(sawRelated);
}

// SELECT against a table that doesn't exist → exactly one
// S_UndeclaredIdentifier; the table name (last identifier) is in
// `actual`.
TEST(SemanticAnalyzerTsql, MissingTableEmitsUndeclared) {
    auto cu = buildShippedUnit("tsql-subset", {
        "SELECT * FROM Ghosts;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier) {
            EXPECT_EQ(d.actual, "Ghosts");
        }
    }
}
