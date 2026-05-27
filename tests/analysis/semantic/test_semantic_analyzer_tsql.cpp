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

namespace {
// Count symbols that originate from a tree (user-declared), excluding the
// CU-wide builtin functions (SE6) which carry an invalid `tree`.
[[nodiscard]] std::size_t userSymbolCount(SemanticModel const& model) {
    std::size_t n = 0;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].tree.valid()) ++n;
    }
    return n;
}

// Count user-declared symbols of a specific kind (D3: `columnDecl` now mints
// Variable-kind column symbols per table, so "how many tables?" must filter
// by Table-kind rather than counting every tree symbol).
[[nodiscard]] std::size_t tableSymbolCount(SemanticModel const& model) {
    std::size_t n = 0;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& rec = model.symbols()[i];
        if (rec.tree.valid() && rec.kind == DeclarationKind::Table) ++n;
    }
    return n;
}

// The single Table-kind symbol (asserts exactly one exists).
[[nodiscard]] SymbolRecord const* singleTable(SemanticModel const& model) {
    SymbolRecord const* found = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& rec = model.symbols()[i];
        if (rec.tree.valid() && rec.kind == DeclarationKind::Table) {
            EXPECT_EQ(found, nullptr) << "expected exactly one Table symbol";
            found = &rec;
        }
    }
    return found;
}
} // namespace

// CREATE TABLE → one Table symbol minted + one Variable symbol per column
// (D3: `columnDecl` is now a declaration). The two columns (Id, Name) are
// table-local (createTableStmt opens a scope), so the CU has 3 user symbols.
TEST(SemanticAnalyzerTsql, CreateTableMintsTableSymbol) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE Customers (Id INT NOT NULL, Name VARCHAR);",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(tableSymbolCount(model), 1u);
    EXPECT_EQ(userSymbolCount(model), 3u) << "1 table + 2 columns (Id, Name)";
    SymbolRecord const* rec = singleTable(model);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->name, "Customers");
    EXPECT_EQ(rec->kind, DeclarationKind::Table);
}

// D3: column types resolve via the tsql `builtinTypes` block. `INT`→I32,
// `BIT`→Bool (both core primitives). `VARCHAR`→the `TSQL::Varchar`
// extension (no core string kind exists, so VARCHAR maps to the registered
// extension type rather than a core primitive). Critically, NO column emits
// S_UnknownType — the no-regression property the D3 mapping must preserve.
TEST(SemanticAnalyzerTsql, ColumnTypesResolveViaBuiltinTypes) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE t (a INT, b BIT, c VARCHAR);",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    auto const& interner = model.lattice().interner();

    SymbolRecord const* aRec = nullptr;
    SymbolRecord const* bRec = nullptr;
    SymbolRecord const* cRec = nullptr;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        auto const& rec = model.symbols()[i];
        if (!rec.tree.valid() || rec.kind != DeclarationKind::Variable) continue;
        if (rec.name == "a") aRec = &rec;
        if (rec.name == "b") bRec = &rec;
        if (rec.name == "c") cRec = &rec;
    }
    ASSERT_NE(aRec, nullptr) << "column a must mint a symbol";
    ASSERT_NE(bRec, nullptr) << "column b must mint a symbol";
    ASSERT_NE(cRec, nullptr) << "column c must mint a symbol";

    ASSERT_TRUE(aRec->type.valid());
    EXPECT_EQ(interner.kind(aRec->type), TypeKind::I32) << "INT → I32";
    ASSERT_TRUE(bRec->type.valid());
    EXPECT_EQ(interner.kind(bRec->type), TypeKind::Bool) << "BIT → Bool";
    // VARCHAR resolves to the TSQL::Varchar extension type (kind Extension),
    // NOT a core primitive and NOT S_UnknownType.
    ASSERT_TRUE(cRec->type.valid()) << "VARCHAR must resolve, not stay Invalid";
    EXPECT_EQ(interner.kind(cRec->type), TypeKind::Extension) << "VARCHAR → TSQL::Varchar";
    EXPECT_EQ(interner.name(cRec->type), "TSQL::Varchar");

    // No-regression: not one column type position emits S_UnknownType.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UnknownType), 0u);
}

// `dbo.Customers` in a CREATE TABLE — the `lastIdentifier` matcher
// extracts `Customers` (the rightmost component of the dotted name).
TEST(SemanticAnalyzerTsql, LastIdentifierMatcherExtractsRightmost) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE dbo.Orders (Id INT);",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(tableSymbolCount(model), 1u);
    SymbolRecord const* rec = singleTable(model);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->name, "Orders");
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
    // The NameMatching resolver recorded exactly one cross-tree edge
    // (the SELECT's `Orders` reference → the CREATE's `Orders` definition).
    ASSERT_EQ(cu->crossRefs().size(), 1u)
        << "tsql name-matching must produce exactly one cross-file edge";
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
    // Two independent table symbols minted in two distinct tree-root scopes
    // (the CU-wide COALESCE builtin is excluded by userSymbolCount). Each
    // table also mints its own table-local `Id` column → 4 user symbols.
    EXPECT_EQ(tableSymbolCount(model), 2u);
    EXPECT_EQ(userSymbolCount(model), 4u) << "2 tables + 2 (table-local) Id columns";
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

// SE6: COALESCE is a config-declared VARIADIC builtin function bound into
// a CU-wide builtins scope. A call `COALESCE(...)` with any arity resolves
// as callable (no S_NotCallable, no S_ArgCountMismatch).
TEST(SemanticAnalyzerTsql, CoalesceBuiltinIsVariadicCallable) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE T (a INT);"
        "SELECT COALESCE(a, a, a) FROM T;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 0u)
        << "COALESCE must resolve to a callable builtin FnSig";
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 0u)
        << "a variadic builtin accepts any arity";
}

// SE6: calling a NON-function symbol (a table) → S_NotCallable.
TEST(SemanticAnalyzerTsql, CallingTableSymbolIsNotCallable) {
    // `T(a)` in expression position: T is a Table-kind symbol (not a
    // FnSig), so calling it emits S_NotCallable.
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE T (a INT);"
        "SELECT T(a) FROM T;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NotCallable), 1u);
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

// HR10 `ReferenceRule.hardParents`: a relational COLUMN reference that doesn't
// resolve lexically is NOT an error — columns bind against the FROM relation,
// which this frontend doesn't model. The `qualifiedName` reference rule is hard
// only under the statement-target parents (tableRef / insert/update/delete), so
// a column in a WHERE/projection position stays soft (sym 0).
TEST(SemanticAnalyzerTsql, UnresolvedColumnInExpressionStaysSoft) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE T (a INT);"
        "SELECT * FROM T WHERE Ghost = 1;",   // Ghost is a column, not in scope
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "an unresolved relational column must not emit S_UndeclaredIdentifier";
}

// The discriminating case: a missing TABLE (hard, under tableRef) errors while
// the column references in the SAME statement (soft, under expression) do not —
// so the count is exactly one, on the table name.
TEST(SemanticAnalyzerTsql, HardTableErrorsWhileColumnsStaySoft) {
    auto cu = buildShippedUnit("tsql-subset", {
        "SELECT Ghost FROM Missing WHERE Other = 1;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "exactly the table (hard) errors; the two columns (soft) do not";
    for (auto const& d : model.diagnostics().all())
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier)
            EXPECT_EQ(d.actual, "Missing");
}

// The hard parents beyond tableRef (insert/update/delete statement targets) must
// also still error on a missing table — they're otherwise unreachable via SELECT.
TEST(SemanticAnalyzerTsql, MissingTableInUpdateAndDeleteErrors) {
    auto upd = buildShippedUnit("tsql-subset", { "UPDATE Missing SET a = 1;" });
    assertNoBuilderErrors(*upd);
    EXPECT_EQ(countCode(analyze(upd).diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "UPDATE on a missing table (hard target) must error";

    auto del = buildShippedUnit("tsql-subset", { "DELETE FROM Missing WHERE a = 1;" });
    assertNoBuilderErrors(*del);
    EXPECT_EQ(countCode(analyze(del).diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "DELETE on a missing table (hard target) must error";
}

// SE6 (tsql): a call to an unknown function → an S_UndeclaredIdentifier
// on the callee. tsql's `callExpr` callee is a bare `Identifier` token
// NOT covered by the `qualifiedName` reference rule, so the engine's
// checkCall path is the one that has to be loud about the unresolved
// callee — otherwise `BOGUS(...)` would silently produce zero call-
// related diagnostics. The arg `a` (a table column) is independently
// undeclared in this SE-era model — so the engine emits one for `BOGUS`
// and one for `a`. We assert exactly the BOGUS undeclared is present.
TEST(SemanticAnalyzerTsql, UnknownFunctionCallEmitsUndeclared) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE T (a INT);"
        "SELECT BOGUS(a) FROM T;",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    std::size_t bogusCount = 0;
    ParseDiagnostic const* bogusDiag = nullptr;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier
            && d.actual == "BOGUS") {
            ++bogusCount;
            bogusDiag = &d;
        }
    }
    EXPECT_EQ(bogusCount, 1u)
        << "the unknown callee must produce an S_UndeclaredIdentifier "
           "with actual == 'BOGUS'";
    // Pin the BYTE offsets of the BOGUS token. The two source string
    // literals concat to:
    //     "CREATE TABLE T (a INT);SELECT BOGUS(a) FROM T;"
    // `CREATE TABLE T (a INT);` is 23 bytes (offsets 0..22), then
    // `SELECT ` adds 7 (23..29), so `BOGUS` spans offsets 30..34 — i.e.
    // a half-open span [30, 35). The engine emits the diagnostic on the
    // callee token's span; pinning these here catches any future
    // regression that drifts the emit point off the callee leaf.
    ASSERT_NE(bogusDiag, nullptr);
    EXPECT_EQ(bogusDiag->span.start(), 30u);
    EXPECT_EQ(bogusDiag->span.end(),   35u);
}

// ── GAP D: bracket-quoted identifier `[Name]` matching ─────────────────────

// `CREATE TABLE [Orders] (...); SELECT * FROM [Orders];` — the bracket-id
// `[Orders]` both MINTS the table symbol and RESOLVES the reference. No
// S_UndeclaredIdentifier. Pins the bracketIdentifierToken facet (GAP D).
TEST(SemanticAnalyzerTsql, BracketIdentifierMintsAndResolves) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE [Orders] (id INT); SELECT * FROM [Orders];",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    // The table symbol minted with the bracketed name (brackets stripped).
    EXPECT_EQ(tableSymbolCount(model), 1u);
    SymbolRecord const* rec = singleTable(model);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->name, "Orders");
    EXPECT_EQ(rec->kind, DeclarationKind::Table);
    // The bracket-id reference resolves — no undeclared.
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// A bracket-id reference to a table that does not exist → loud (exactly one
// S_UndeclaredIdentifier), not a silent miss.
TEST(SemanticAnalyzerTsql, BracketIdentifierUnknownTableEmitsUndeclared) {
    auto cu = buildShippedUnit("tsql-subset", {
        "SELECT * FROM [Unknown];",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u);
    for (auto const& d : model.diagnostics().all()) {
        if (d.code == DiagnosticCode::S_UndeclaredIdentifier) {
            EXPECT_EQ(d.actual, "Unknown");
        }
    }
}

// FIX 1: a `]]` doubled-delimiter inside a bracket-id is an ESCAPED literal
// `]`, so `[Ord]]ers]` is the SINGLE identifier `Ord]ers` (the tokenizer
// declares `escapeKind: doubled-delimiter, endsAt: "]"` for `[`; confirmed by
// test_tsql_subset BracketIdentifierHasDoubledDelimiterStyle). The bracket-id
// decoder must MATCH the tokenizer: stopping at the first `]` (pre-fix) would
// mint `Ord` and look up `Ord` for the reference, diverging from the
// tokenizer's `Ord]ers`. Same-file: the table mints under `Ord]ers` AND the
// SELECT reference resolves (zero S_UndeclaredIdentifier).
TEST(SemanticAnalyzerTsql, BracketIdDoubledDelimiterMintsAndResolves) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE [Ord]]ers] (id INT); SELECT * FROM [Ord]]ers];",
    });
    assertNoBuilderErrors(*cu);
    auto model = analyze(cu);
    EXPECT_EQ(tableSymbolCount(model), 1u);
    SymbolRecord const* rec = singleTable(model);
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec->name, "Ord]ers")
        << "the `]]` must un-double to a single literal `]` — name is Ord]ers";
    EXPECT_EQ(rec->kind, DeclarationKind::Table);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u)
        << "the escaped-bracket reference must resolve to the escaped-bracket decl";
}

// FIX 1 cross-FILE: the SAME `]]`-escaped name spanning two files resolves via
// exactly one name-matching crossRef edge — the import resolver's bracket-id
// decoder shares the same `]]` un-escaping, so both files agree on `Ord]ers`.
TEST(SemanticAnalyzerTsql, BracketIdDoubledDelimiterCrossFileResolves) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE [Ord]]ers] (Id INT);",   // tree 0 defines Ord]ers
        "SELECT * FROM [Ord]]ers];",            // tree 1 references it
    });
    assertNoBuilderErrors(*cu);
    ASSERT_EQ(cu->crossRefs().size(), 1u)
        << "both files decode the escaped bracket-id to the same name → one edge";
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}

// GAP D cross-FILE: a CREATE TABLE [Orders] in one file and a SELECT FROM
// [Orders] in another resolve via a name-matching crossRef edge — the
// import resolver's lastIdentifierText must also read the bracket-id text.
TEST(SemanticAnalyzerTsql, BracketIdentifierCrossFileResolvesViaImportEdge) {
    auto cu = buildShippedUnit("tsql-subset", {
        "CREATE TABLE [Orders] (Id INT);",   // tree 0 defines [Orders]
        "SELECT * FROM [Orders];",           // tree 1 references it
    });
    assertNoBuilderErrors(*cu);
    // FIX 9: a deterministic two-file input produces EXACTLY one cross-tree
    // edge — assert equality, not a lower bound.
    ASSERT_EQ(cu->crossRefs().size(), 1u)
        << "bracket-id name-matching must produce exactly one cross-file edge";
    auto model = analyze(cu);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 0u);
}
