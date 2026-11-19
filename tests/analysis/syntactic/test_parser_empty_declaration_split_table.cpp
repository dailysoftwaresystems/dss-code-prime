// ===========================================================================
// P69 L2b2 (lane `lm`) — THE EMPTY TYPEDEF-NAMED DECLARATION, SPLIT IN TWO, READS EVERY ROW AS BEFORE.
//
// The merged rule `[optional localDeclSpecifiers] identDeclHead ;` put Identifier into the FIRST set of
// `declOrAttrStmt`, the statement's first candidate, so every identifier-led statement opened two more
// probes: ✔MEASURED on sqlite3.c, speculation probes 96,316 → 180,057 and the parse +21–36%. The split
// keeps the specifier-led form (`typeNameEmptyDecl`, `typeNameEmptyForDecl`, the specifiers REQUIRED) where
// it was, and gives the bare form (`typeNameBareEmptyDecl`, `typeNameBareEmptyForDecl`) to the statement
// site an identifier already leads (`declOrExprStmt`, the for-init alternative after the specifier-led one).
//
// WHAT THIS FILE PINS
//   (A) THE TABLE: every row — plan §6's nine spellings at file scope, block scope and as a for-init, the
//       design audit's further block rows, the owed rows — through the whole front end and the HIR
//       lowering, and every C unit of tests/corpus/diagnostics/c through the parse, rendered with every
//       diagnostic's code, severity, span, text and SCOPE KINDS (the memory fix's trails), each row with the
//       statement and declaration rules its tree holds. The golden beside this file is that rendering
//       BEFORE the split and the memory fix (captured on the lane's tree, 2026-11-16); the rendering now
//       must equal it byte for byte but for `kSplitDeltas`, each a rule name the split moves, with its
//       reason, and each found exactly once in its row.
//   (B) THE SPLIT'S POINT: the specifier-led forms, and the statement's first candidate, lead with no name.
//   (C) A name after a specifier that the sketch does not know is no empty declaration.
//
// RED-ON-DISABLE (tests/mutations/arms.registry): the split undone (the specifiers optional again) — (A)'s
// rule names and (B); the specifier-led form's guard at the default polarity — (A)'s `const u;` rows and (C).
// ⚠ MUST run through ctest: the shipped documents, the corpus and the golden are found through the repo root.
// A missing or differing golden writes the rendering to build/empty_declaration_split_table.actual.txt.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/substrate/path_identity.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/scope_trail.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {
// ── RENDERING BEGIN — the same bytes before and after the split: never edited ──────────────
struct Row {
    char const* name;
    char const* src;
};

Row const kRows[] = {
    Row{"l2b2split_n01_file_T", R"ROW(typedef int T;
int v = 0;
T;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n01_block_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  T;
  return 0;
}
)ROW"},
    Row{"l2b2split_n01_for_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (T; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_n02_file_T_const", R"ROW(typedef int T;
int v = 0;
T const;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n02_block_T_const", R"ROW(typedef int T;
int v = 0;
int f(void) {
  T const;
  return 0;
}
)ROW"},
    Row{"l2b2split_n02_for_T_const", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (T const; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_n03_file_const_T", R"ROW(typedef int T;
int v = 0;
const T;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n03_block_const_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  const T;
  return 0;
}
)ROW"},
    Row{"l2b2split_n03_for_const_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (const T; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_n04_file_static_T", R"ROW(typedef int T;
int v = 0;
static T;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n04_block_static_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  static T;
  return 0;
}
)ROW"},
    Row{"l2b2split_n04_for_static_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (static T; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_n05_file_auto_T", R"ROW(typedef int T;
int v = 0;
auto T;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n05_block_auto_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  auto T;
  return 0;
}
)ROW"},
    Row{"l2b2split_n05_for_auto_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (auto T; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_n06_file_register_T", R"ROW(typedef int T;
int v = 0;
register T;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n06_block_register_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  register T;
  return 0;
}
)ROW"},
    Row{"l2b2split_n06_for_register_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (register T; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_n07_file_extension___T", R"ROW(typedef int T;
int v = 0;
__extension__ T;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n07_block_extension___T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  __extension__ T;
  return 0;
}
)ROW"},
    Row{"l2b2split_n07_for_extension___T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (__extension__ T; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_n08_file_maybe_unused___T", R"ROW(typedef int T;
int v = 0;
[[maybe_unused]] T;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n08_block_maybe_unused___T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  [[maybe_unused]] T;
  return 0;
}
)ROW"},
    Row{"l2b2split_n08_for_maybe_unused___T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for ([[maybe_unused]] T; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_n09_file_int", R"ROW(typedef int T;
int v = 0;
int;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_n09_block_int", R"ROW(typedef int T;
int v = 0;
int f(void) {
  int;
  return 0;
}
)ROW"},
    Row{"l2b2split_n09_for_int", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (int; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_f01_block_attribute____unused___T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  __attribute__((unused)) T;
  return 0;
}
)ROW"},
    Row{"l2b2split_f02_block_Alignas_8__T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  _Alignas(8) T;
  return 0;
}
)ROW"},
    Row{"l2b2split_f03_block_T_volatile", R"ROW(typedef int T;
int v = 0;
int f(void) {
  T volatile;
  return 0;
}
)ROW"},
    Row{"l2b2split_f04_block_T_const_volatile", R"ROW(typedef int T;
int v = 0;
int f(void) {
  T const volatile;
  return 0;
}
)ROW"},
    Row{"l2b2split_f05_block_const_T_const", R"ROW(typedef int T;
int v = 0;
int f(void) {
  const T const;
  return 0;
}
)ROW"},
    Row{"l2b2split_f06_block_extension___T_const", R"ROW(typedef int T;
int v = 0;
int f(void) {
  __extension__ T const;
  return 0;
}
)ROW"},
    Row{"l2b2split_f07_block_extension___const_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  __extension__ const T;
  return 0;
}
)ROW"},
    Row{"l2b2split_f08_block_extension_____extension___T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  __extension__ __extension__ T;
  return 0;
}
)ROW"},
    Row{"l2b2split_f09_block_L__T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  L: T;
  return 0;
}
)ROW"},
    Row{"l2b2split_f10_block_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  T *;
  return 0;
}
)ROW"},
    Row{"l2b2split_f11_block_v", R"ROW(typedef int T;
int v = 0;
int f(void) {
  v;
  return 0;
}
)ROW"},
    Row{"l2b2split_f12_block_u", R"ROW(typedef int T;
int v = 0;
int f(void) {
  u;
  return 0;
}
)ROW"},
    Row{"l2b2split_f13_file_T_ptr", R"ROW(typedef int T;
int v = 0;
T *;
int f(void) { return 0; }
)ROW"},
    Row{"l2b2split_g01_for_T_const", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (T const; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_g02_for_static_T", R"ROW(typedef int T;
int v = 0;
int f(void) {
  for (static T; 0;) ;
  return 0;
}
)ROW"},
    Row{"l2b2split_z12_labelled_statement_in_a_cast_stmt_expr", R"ROW(typedef int T;
int v = 0;
int f(void) {
  return (int)({ L: v = v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v + v; 0; });
}
)ROW"},
    Row{"l2b2split_a01_block_alignas_v", R"ROW(typedef int T;
int v = 0;
int f(void) {
  _Alignas(v) int a;
  return 0;
}
)ROW"},
    Row{"l2b2split_a02_block_enum_E_N", R"ROW(typedef int T;
int v = 0;
int f(void) {
  enum { N = 4 }; enum E : N;
  return 0;
}
)ROW"},
    Row{"l2b2split_a03_block_sizeof_v", R"ROW(typedef int T;
int v = 0;
int f(void) {
  int s = (int)sizeof(v); (void)s;
  return 0;
}
)ROW"},
    Row{"l2b2split_u01_block_const_u", R"ROW(typedef int T;
int v = 0;
int f(void) {
  const u;
  return 0;
}
)ROW"},
    Row{"l2b2split_u02_block_static_u", R"ROW(typedef int T;
int v = 0;
int f(void) {
  static u;
  return 0;
}
)ROW"}
};

char const* const kRules[] = {"declOrAttrStmt", "declOrExprStmt", "extensionDeclStmt", "labelStmt",
                              "typeNameEmptyDecl", "typeNameBareEmptyDecl", "typeNameEmptyForDecl",
                              "typeNameBareEmptyForDecl", "identVarDecl", "forIdentDecl", "varDecl",
                              "autoInferredVarDecl", "attributeDeclaration", "exprStmt", "castExpr",
                              "compoundLiteralExpr", "parenExpr", "sizeofType", "sizeofValue",
                              "alignasTypeName", "enumTypeSpecifier", "stmtExpr"};

void renderDiagnostic(std::string& out, char const* where, ParseDiagnostic const& d) {
    out += "  ";
    out += where;
    out += " ";
    out += diagnosticCodeName(d.code);
    out += d.severity == DiagnosticSeverity::Error ? " E" : d.severity == DiagnosticSeverity::Warning ? " W" : " I";
    out += " @" + std::to_string(static_cast<std::uint64_t>(d.span.start())) + ":"
         + std::to_string(static_cast<std::uint64_t>(d.span.end()));
    out += " actual=" + d.actual + " scopes=[";
    for (ScopeKind const k : d.scopeStack.kinds()) out += std::to_string(static_cast<int>(k)) + ",";
    out += "]\n";
}

std::string rulesHeld(CompilationUnit const& cu) {
    std::string out;
    for (char const* name : kRules) {
        std::size_t n = 0;
        for (Tree const& t : cu.trees()) {
            RuleId const r = t.schema().rules().find(name);
            if (!r.valid()) continue;
            for (std::uint32_t i = 1; t.root().valid() && i < static_cast<std::uint32_t>(t.nodeCount()); ++i) {
                NodeId const id{i, t.root().arenaTag};
                if (t.kind(id) == NodeKind::Internal && t.rule(id).v == r.v) ++n;
            }
        }
        if (n != 0) out += std::string{name} + "x" + std::to_string(n) + " ";
    }
    return out;
}

// The whole table: every row, then every diagnostics-corpus C unit. Empty on a failure it reports itself.
std::string renderTable() {
    auto loaded = GrammarSchema::loadShipped("c");
    if (!loaded.has_value()) {
        ADD_FAILURE() << "the shipped c language document";
        return {};
    }
    std::shared_ptr<GrammarSchema const> c = *loaded;
    std::string out;
    for (Row const& row : kRows) {
        UnitBuilder builder{c, DiagnosticBudget::libraryDefault()};
        builder.addInMemory(row.src, "<mem>");
        auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
        out += "ROW " + std::string{row.name} + "\n  rules: " + rulesHeld(*cu) + "\n";
        for (Tree const& t : cu->trees())
            for (auto const& d : t.diagnostics().all()) renderDiagnostic(out, "parse", d);
        for (auto const& d : cu->driverDiagnostics().all()) renderDiagnostic(out, "driver", d);
        SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                                      AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
        for (auto const& d : model.diagnostics().all()) renderDiagnostic(out, "model", d);
        if (model.hasErrors()) {
            out += "  lowering: not run (model errors)\n";
            continue;
        }
        DiagnosticReporter reporter;
        auto const result = lowerToHir(model, reporter);
        for (auto const& d : reporter.all()) renderDiagnostic(out, "lower", d);
        out += std::string{"  lowering: "} + (result && result->ok ? "ok" : "failed") + "\n";
    }
    fs::path const root = dss::test::repoRoot();
    std::vector<fs::path> units;
    auto const walk = dss::core::walkTree(root / "tests/corpus/diagnostics/c", [&](dss::core::TreeEntry const& e) {
        if (e.kind == dss::core::EntryKind::File) {
            fs::path const p{e.at.spelling().generic()};
            if (p.extension() == ".c") units.push_back(p);
        }
        return dss::core::WalkStep::Continue;
    });
    if (walk.error) {
        ADD_FAILURE() << "the walk of tests/corpus/diagnostics/c stopped: " << walk.error.message();
        return {};
    }
    std::sort(units.begin(), units.end());
    for (fs::path const& f : units) {
        UnitBuilder builder{c, DiagnosticBudget::libraryDefault()};
        builder.addFile(f);
        CompilationUnit cu = std::move(builder).finish();
        out += "UNIT " + f.filename().string() + "\n";
        for (Tree const& t : cu.trees())
            for (auto const& d : t.diagnostics().all()) renderDiagnostic(out, "parse", d);
        for (auto const& d : cu.driverDiagnostics().all()) renderDiagnostic(out, "driver", d);
    }
    out += "END rows=" + std::to_string(std::size(kRows)) + " units=" + std::to_string(units.size()) + "\n";
    return out;
}
// ── RENDERING END ──────────────────────────────────────────────────────────────────────

// The rule names the split moves, each in its row's block, with its reason: the golden is the rendering
// BEFORE the split, and these are its only deltas.
struct SplitDelta {
    char const* row;
    char const* before;
    char const* after;
    char const* reason;
};

std::vector<SplitDelta> const kSplitDeltas{
    SplitDelta{"l2b2split_n01_block_T", "  rules: declOrAttrStmtx1 typeNameEmptyDeclx1 \n", "  rules: declOrExprStmtx1 typeNameBareEmptyDeclx1 \n",
               "the bare form: `T;` is typeNameBareEmptyDecl, under declOrExprStmt, where an identifier already leads"},
    SplitDelta{"l2b2split_n01_for_T", "  rules: typeNameEmptyForDeclx1 \n", "  rules: typeNameBareEmptyForDeclx1 \n",
               "the bare form: the for-init `T;` is typeNameBareEmptyForDecl"},
    SplitDelta{"l2b2split_n02_block_T_const", "  rules: declOrAttrStmtx1 typeNameEmptyDeclx1 \n", "  rules: declOrExprStmtx1 typeNameBareEmptyDeclx1 \n",
               "the bare form: `T const;` leads with the name, so it is typeNameBareEmptyDecl under declOrExprStmt"},
    SplitDelta{"l2b2split_n02_for_T_const", "  rules: typeNameEmptyForDeclx1 \n", "  rules: typeNameBareEmptyForDeclx1 \n",
               "the bare form: the for-init `T const;` is typeNameBareEmptyForDecl"},
    SplitDelta{"l2b2split_n07_block_extension___T", "  rules: declOrExprStmtx1 extensionDeclStmtx1 typeNameEmptyDeclx1 \n", "  rules: declOrExprStmtx1 extensionDeclStmtx1 typeNameBareEmptyDeclx1 \n",
               "the bare form: after `__extension__` (extensionDeclStmt) the name leads, so it is typeNameBareEmptyDecl"},
    SplitDelta{"l2b2split_f03_block_T_volatile", "  rules: declOrAttrStmtx1 typeNameEmptyDeclx1 \n", "  rules: declOrExprStmtx1 typeNameBareEmptyDeclx1 \n",
               "the bare form: `T volatile;` leads with the name, so it is typeNameBareEmptyDecl under declOrExprStmt"},
    SplitDelta{"l2b2split_f04_block_T_const_volatile", "  rules: declOrAttrStmtx1 typeNameEmptyDeclx1 \n", "  rules: declOrExprStmtx1 typeNameBareEmptyDeclx1 \n",
               "the bare form: `T const volatile;` leads with the name, so it is typeNameBareEmptyDecl under declOrExprStmt"},
    SplitDelta{"l2b2split_f06_block_extension___T_const", "  rules: declOrExprStmtx1 extensionDeclStmtx1 typeNameEmptyDeclx1 \n", "  rules: declOrExprStmtx1 extensionDeclStmtx1 typeNameBareEmptyDeclx1 \n",
               "the bare form: after `__extension__` the name leads, so it is typeNameBareEmptyDecl"},
    SplitDelta{"l2b2split_f08_block_extension_____extension___T", "  rules: declOrExprStmtx1 extensionDeclStmtx2 typeNameEmptyDeclx1 \n", "  rules: declOrExprStmtx1 extensionDeclStmtx2 typeNameBareEmptyDeclx1 \n",
               "the bare form: after two `__extension__` the name leads, so it is typeNameBareEmptyDecl"},
    SplitDelta{"l2b2split_f09_block_L__T", "  rules: declOrAttrStmtx1 declOrExprStmtx1 labelStmtx1 typeNameEmptyDeclx1 \n", "  rules: declOrExprStmtx2 labelStmtx1 typeNameBareEmptyDeclx1 \n",
               "the bare form: the labelled `T;` is typeNameBareEmptyDecl under declOrExprStmt, as the label's own statement is"},
    SplitDelta{"l2b2split_g01_for_T_const", "  rules: typeNameEmptyForDeclx1 \n", "  rules: typeNameBareEmptyForDeclx1 \n",
               "the bare form: the for-init `T const;` is typeNameBareEmptyForDecl"}
};

fs::path goldenPath() { return dss::test::repoRoot() / "tests/analysis/syntactic/empty_declaration_split_table.golden"; }
fs::path actualPath() { return dss::test::repoRoot() / "build" / "empty_declaration_split_table.actual.txt"; }

void writeActual(std::string const& actual) {
    std::error_code ec;
    fs::create_directories(actualPath().parent_path(), ec);
    std::ofstream out{actualPath(), std::ios::binary};
    out << actual;
}

// `text` with each delta applied inside its row's block (from `ROW <row>` to the next ROW or UNIT line).
std::string applyDeltas(std::string text) {
    for (SplitDelta const& d : kSplitDeltas) {
        std::string const head = std::string{"ROW "} + d.row + "\n";
        std::size_t const from = text.find(head);
        if (from == std::string::npos) {
            ADD_FAILURE() << "a delta names a row the golden does not hold: " << d.row;
            continue;
        }
        std::size_t to = text.find("\nROW ", from + head.size());
        to = std::min(to, text.find("\nUNIT ", from + head.size()));
        std::string block = text.substr(from, to == std::string::npos ? std::string::npos : to - from);
        std::size_t const at = block.find(d.before);
        if (at == std::string::npos || block.find(d.before, at + 1) != std::string::npos) {
            ADD_FAILURE() << "a delta's text does not stand exactly once in row " << d.row << ": " << d.before;
            continue;
        }
        block.replace(at, std::string_view{d.before}.size(), d.after);
        text.replace(from, to == std::string::npos ? std::string::npos : to - from, block);
    }
    return text;
}

std::string firstDifference(std::string const& want, std::string const& got) {
    std::size_t i = 0, line = 1;
    while (i < want.size() && i < got.size() && want[i] == got[i]) {
        if (want[i] == '\n') ++line;
        ++i;
    }
    auto lineAt = [](std::string const& s, std::size_t at) {
        std::size_t const b = at == 0 ? std::string::npos : s.rfind('\n', at - 1);
        std::size_t const from = b == std::string::npos ? 0 : b + 1;
        std::size_t const e = s.find('\n', from);
        return s.substr(from, (e == std::string::npos ? s.size() : e) - from);
    };
    return "line " + std::to_string(line) + "\n  golden: " + lineAt(want, i) + "\n  now   : " + lineAt(got, i);
}

[[nodiscard]] bool firstSetHoldsAName(GrammarSchema const& s, std::string_view rule) {
    RuleId const r = s.rules().find(rule);
    EXPECT_TRUE(r.valid()) << rule;
    SchemaTokenId const id = s.schemaTokens().find("Identifier");
    return r.valid() && s.firstSetContains(r, id);
}

}  // namespace

// ── (A) ─────────────────────────────────────────────────────────────────
TEST(EmptyDeclarationSplitTable, EveryRowAndEveryCorpusUnitReadsAsBeforeTheSplit) {
    std::string const actual = renderTable();
    ASSERT_FALSE(actual.empty());
    std::ifstream in{goldenPath(), std::ios::binary};
    if (!in.good()) {
        writeActual(actual);
        FAIL() << "no golden at " << goldenPath().generic_string() << "; the rendering is at "
               << actualPath().generic_string();
    }
    std::string golden{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::erase(golden, '\r');   // a CRLF checkout
    std::string const want = applyDeltas(golden);
    if (want != actual) {
        writeActual(actual);
        ADD_FAILURE() << "the table moved from its golden (with the split's deltas) at " << firstDifference(want, actual)
                      << "\nthe rendering is at " << actualPath().generic_string();
    }
}

// ── (B) ─────────────────────────────────────────────────────────────────
TEST(EmptyDeclarationSplitTable, TheSpecifierLedFormsAndTheFirstCandidateLeadWithNoName) {
    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());
    GrammarSchema const& c = **loaded;
    for (char const* rule : {"typeNameEmptyDecl", "typeNameEmptyForDecl", "declOrAttrStmt"}) {
        EXPECT_FALSE(firstSetHoldsAName(c, rule))
            << rule << " can start with a name: every identifier-led statement probes it again";
    }
    for (char const* rule : {"typeNameBareEmptyDecl", "typeNameBareEmptyForDecl", "declOrExprStmt"}) {
        EXPECT_TRUE(firstSetHoldsAName(c, rule)) << rule << " must take the bare typedef name";
    }
}

// ── (C) ─────────────────────────────────────────────────────────────────
TEST(EmptyDeclarationSplitTable, AnUnknownNameAfterASpecifierIsNoEmptyDeclaration) {
    auto loaded = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loaded.has_value());
    std::shared_ptr<GrammarSchema const> c = *loaded;
    for (char const* stmt : {"const u;", "static u;"}) {
        UnitBuilder builder{c, DiagnosticBudget::libraryDefault()};
        builder.addInMemory(std::string{"int f(void) {\n  "} + stmt + "\n  return 0;\n}\n", "<mem>");
        CompilationUnit const cu = std::move(builder).finish();
        EXPECT_EQ(rulesHeld(cu).find("typeNameEmptyDecl"), std::string::npos)
            << "`" << stmt << "`, with no `u` declared, was read as an empty declaration of an unknown type: "
            << rulesHeld(cu);
    }
}
