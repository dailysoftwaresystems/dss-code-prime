// ===========================================================================
// P69 L2b2 (lane `lm`) — A DECLARATION THAT DECLARES NOTHING IS A WARNING, AND LOWERS TO
// NOTHING.
//
// THE PROPERTY THIS FILE OWNS: C 6.7p2 — a declaration shall declare a declarator, a tag
// or an enumeration's constants. One that declares none (`int;`, `T;`, `const T;`,
// `static T;`, `struct { int a; };`, the for-init `for (int; 0; )`) is diagnosed, as the
// language says (`semantics.declarators.emptyDeclarationWarns`): C's is a WARNING, once, at
// the declaration, and the program compiles. Every typedef-named form PARSES as the
// declaration it is (the TYPE reading, committed only for a name the binder sketch knows as
// a type), so an unknown name (`u;`) stays an expression.
//
// ✔MEASURED 2026-11-05 (the lane's probes l2b2e and l2b2i):
//   * gcc 13.3.0 ("useless type name in empty declaration", "unnamed struct/union that
//     defines no instances"), clang 18.1.3 and Apple clang ("declaration does not declare
//     anything") warn and cl accepts in silence, every form at file and block scope;
//   * `auto T;`, `register T;`, `__extension__ T;` alike; `[[maybe_unused]] T;` gcc alone;
//   * the for-init: gcc and clang warn, cl refuses (C2059) — the union accepts.
//   DSS refused each typedef-named form by a misreading (`T;` an expression, `T const;` a
//   parse error, `const T;` / `static T;` a head-less declaration of a variable named T),
//   refused `int;` with an error, refused the for-init outright, and accepted the unnamed
//   struct in silence.
// An ABSTRACT declarator is no empty declaration: every reference refuses `int *;` and
// `T *;` at both scopes (probe l2b2a, 56 cells), and so does DSS, by name.
//
// ── RED-ON-DISABLE ─────────────────────────────────────────────────────────
//   * the report dropped → every warning cell counts 0;
//   * the language's severity ignored (an error always) → every warning cell's severity, and
//     its lowering's success;
//   * the type-first alternatives gone → the typedef-named cells misread again;
//   * the unnamed composite read as declaring its members → its cells count 0;
//   * the absent list returning silently → the for-init cells count 0;
//   * the bare for-init form kept before a declarator, or refused before its `;` → the
//     for-init cells meet a PARSE error, which each cell reads off the unit's trees.
// The controls (a tag, an enumeration, an object, an unknown name) pin that only an empty
// declaration is reported; the key rewritten off pins that the severity is the language's.
// ★ P69 L2b2 (lane `lm`, the fold's third sweep): "the front end accepts it" is read off BOTH
// tiers. The semantic model carries only the analysis's own diagnostics, and the parse's stay
// on the unit's trees (the semantic tests read them through `assertNoBuilderErrors`).
// ✔MEASURED (l2b2-post-it4): with the bare for-init form kept only before a declarator,
// `for (T; 0; )` is a P_NoAlternativeMatched at its `;`, and a cell that read the model alone
// still read it as accepted (check-mutations runs 20261116-213402-d2ea60b0 and
// 20261116-223042-65df8e82).
//
// ⚠ MUST run through ctest: the shipped documents are found through `DSS_CONFIG_ROOT`.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "repo_root.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

constexpr DiagnosticCode kNothing = DiagnosticCode::S_DeclarationDeclaresNothing;

[[nodiscard]] std::string dump(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        out += "\n    ";
        out += diagnosticCodeName(d.code);
        out += d.severity == DiagnosticSeverity::Warning ? " (warning): " : ": ";
        out += d.actual;
    }
    return out.empty() ? std::string{" <no diagnostics>"} : out;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

// The shipped C document with `semantics.declarators.emptyDeclarationWarns` set to `value`
// (a JSON value: the key must be there to be rewritten).
[[nodiscard]] std::string cDocumentWithTheKey(nlohmann::json const& value) {
    std::filesystem::path const path = dss::test::configRoot() / "sources" / "c.lang.json";
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) throw std::runtime_error("cannot open the shipped C document");
    nlohmann::json doc = nlohmann::json::parse(in);
    auto& dc = doc.at("semantics").at("declarators");
    if (!dc.contains("emptyDeclarationWarns"))
        throw std::runtime_error("the shipped C document states no emptyDeclarationWarns");
    dc["emptyDeclarationWarns"] = value;
    return doc.dump();
}

struct Lowered {
    SemanticModel                   model;
    DiagnosticReporter              reporter;
    std::unique_ptr<CstToHirResult> result;
    // Every error the unit's TREES carry (the parse's), dumped; empty when the parse is clean.
    // The model holds the analysis's diagnostics only.
    std::string                     parseErrors;
};

[[nodiscard]] std::unique_ptr<Lowered>
lowerUnder(std::shared_ptr<GrammarSchema const> schema, std::string src) {
    UnitBuilder builder{std::move(schema), DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    std::string parseErrors;
    for (auto const& tree : cu->trees())
        for (auto const& d : tree.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error)
                parseErrors += "\n    " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    auto out = std::unique_ptr<Lowered>(new Lowered{
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16}),
        DiagnosticReporter{}, nullptr, std::move(parseErrors)});
    if (!out->model.hasErrors()) out->result = lowerToHir(out->model, out->reporter);
    return out;
}

[[nodiscard]] std::unique_ptr<Lowered> lowerC(std::string src) {
    return lowerUnder(dss::test_support::shippedSchemaOrThrow("c"), std::move(src));
}

// Where `fragment` begins in the text the unit's tree was parsed from — the buffer a span
// indexes (an in-memory unit's tree text is the source behind a composed prefix).
[[nodiscard]] std::size_t startOf(Lowered const& l, std::string_view fragment) {
    for (auto const& tree : l.model.unit().trees()) {
        std::string_view const text = tree.source().text();
        std::size_t const at = text.find(fragment);
        if (at == std::string_view::npos) continue;
        EXPECT_EQ(text.find(fragment, at + 1), std::string_view::npos)
            << "`" << fragment << "` must occur once";
        return at;
    }
    ADD_FAILURE() << "no tree of the unit holds `" << fragment << "`";
    return std::string_view::npos;
}

constexpr char const* kHead = "typedef int T;\n";

[[nodiscard]] std::string atFileScope(std::string const& decl) {
    return std::string{kHead} + decl + "\nint main(void) { return 42; }\n";
}
[[nodiscard]] std::string atBlockScope(std::string const& stmt) {
    return std::string{kHead} + "int main(void) {\n  " + stmt + "\n  return 42;\n}\n";
}

// One cell: `src` lowers with exactly one S_DeclarationDeclaresNothing, a WARNING, starting
// at `decl`, and nothing else of error severity. `lead + decl` is a fragment that occurs once
// in the text; the span starts past its lead, at the declaration itself.
void expectWarnsOnce(char const* what, std::string const& src, std::string const& lead,
                     std::string const& decl) {
    auto const l = lowerC(src);
    ASSERT_TRUE(l->parseErrors.empty())
        << what << ": the parser accepts it, as every reference does:" << l->parseErrors << "\n"
        << src;
    ASSERT_FALSE(l->model.hasErrors())
        << what << ": the front end accepts it, as every reference does:"
        << dump(l->model.diagnostics()) << "\n" << src;
    ASSERT_NE(l->result, nullptr) << what;
    EXPECT_TRUE(l->result->ok) << what << ": it compiles, as every reference compiles it:"
                               << dump(l->reporter) << "\n" << src;
    EXPECT_EQ(countCode(l->reporter, kNothing), 1u)
        << what << ": declared nothing, reported once:" << dump(l->reporter) << "\n" << src;
    std::size_t const start = startOf(*l, lead + decl) + lead.size();
    for (auto const& d : l->reporter.all()) {
        if (d.code != kNothing) {
            EXPECT_NE(d.severity, DiagnosticSeverity::Error) << what << ":" << dump(l->reporter);
            continue;
        }
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning)
            << what << ": a warning, as gcc and clang give it\n" << src;
        EXPECT_EQ(d.span.start(), start) << what << ": at the declaration\n" << src;
    }
}

}  // namespace

TEST(EmptyDeclaration, ADeclarationThatDeclaresNothingWarnsOnceAndCompiles) {
    struct Form { char const* what; char const* decl; };
    for (Form const& f : {Form{"a typedef name", "T;"}, Form{"a keyword type", "int;"},
                          Form{"a qualified typedef name", "const T;"},
                          Form{"a qualified keyword type", "const int;"},
                          Form{"a typedef name, qualified after", "T const;"},
                          Form{"a storage class and a keyword type", "static int;"},
                          Form{"a storage class and a typedef name", "static T;"},
                          Form{"an unnamed struct", "struct { int a; };"}}) {
        expectWarnsOnce(f.what, atFileScope(f.decl), "\n", f.decl);
        expectWarnsOnce(f.what, atBlockScope(f.decl), "  ", f.decl);
    }
}

TEST(EmptyDeclaration, EveryBlockScopeSpellingOfATypedefNamedEmptyDeclarationWarns) {
    struct Spelling { char const* stmt; char const* lead; char const* decl; };
    // `__extension__` is a prefix the declaration does not begin with: the warning is at the
    // declaration it wraps, `T;`, where clang reports it (✔MEASURED, probe l2b2i: clang 18.1.3
    // at 3:17; gcc 13.3.0 at the prefix, 3:3; cl silent).
    for (Spelling const& s : {Spelling{"auto T;", "  ", "auto T;"},
                              Spelling{"register T;", "  ", "register T;"},
                              Spelling{"__extension__ T;", "  __extension__ ", "T;"},
                              Spelling{"[[maybe_unused]] T;", "  ", "[[maybe_unused]] T;"}}) {
        expectWarnsOnce(s.stmt, atBlockScope(s.stmt), s.lead, s.decl);
    }
}

TEST(EmptyDeclaration, AnEmptyForInitDeclarationWarns) {
    expectWarnsOnce("a keyword type as the for-init", atBlockScope("for (int; 0; ) { }"), "",
                    "int; 0");
    expectWarnsOnce("a typedef name as the for-init", atBlockScope("for (T; 0; ) { }"), "",
                    "T; 0");
}

TEST(EmptyDeclaration, OnlyADeclarationThatDeclaresNothingIsReported) {
    struct Control { char const* what; std::string src; };
    for (Control const& c : {
             Control{"a forward-declared tag", atFileScope("struct S;")},
             Control{"a tag's definition", atFileScope("struct S { int a; };")},
             Control{"an enumeration's constants", atFileScope("enum E { A };")},
             Control{"an unnamed enumeration's constants", atBlockScope("enum { B };")},
             Control{"an object of a typedef-named type", atBlockScope("static T x = 42; (void)x;")},
             Control{"an object's value as a statement", atBlockScope("int x = 42; x;")},
             Control{"a typedef-named for-init that declares an object",
                     atBlockScope("for (T i = 0; i < 1; ++i) { }")},
             Control{"a qualified typedef-named for-init that declares an object",
                     atBlockScope("for (const T i = 0; i < 1; ) { break; }")},
         }) {
        auto const l = lowerC(c.src);
        ASSERT_TRUE(l->parseErrors.empty()) << c.what << ": the parser accepts it:" << l->parseErrors;
        ASSERT_FALSE(l->model.hasErrors()) << c.what << ":" << dump(l->model.diagnostics());
        ASSERT_NE(l->result, nullptr) << c.what;
        EXPECT_TRUE(l->result->ok) << c.what << ":" << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kNothing), 0u) << c.what << ":" << dump(l->reporter);
    }
    // An unknown name followed by nothing is the expression it reads as: undeclared.
    auto const u = lowerC(atBlockScope("u;"));
    EXPECT_TRUE(u->parseErrors.empty()) << "`u;` parses as an expression statement:" << u->parseErrors;
    EXPECT_EQ(countCode(u->model.diagnostics(), DiagnosticCode::S_UndeclaredIdentifier), 1u)
        << "`u;` with no `u` declared is an expression naming nothing, as every reference "
           "reads it:" << dump(u->model.diagnostics());
    EXPECT_EQ(countCode(u->model.diagnostics(), kNothing), 0u) << dump(u->model.diagnostics());
}

TEST(EmptyDeclaration, AnAbstractDeclaratorIsNoEmptyDeclarationAndStaysAnError) {
    for (std::string const& src : {atFileScope("int *;"), atBlockScope("int *;"),
                                   atFileScope("T *;"), atBlockScope("T *;")}) {
        auto const l = lowerC(src);
        EXPECT_TRUE(l->model.hasErrors()) << "every reference refuses it:\n" << src;
        EXPECT_EQ(countCode(l->model.diagnostics(), kNothing), 1u)
            << dump(l->model.diagnostics()) << "\n" << src;
        for (auto const& d : l->model.diagnostics().all())
            if (d.code == kNothing) EXPECT_EQ(d.severity, DiagnosticSeverity::Error) << src;
    }
}

TEST(EmptyDeclaration, TheSeverityIsTheLanguagesAndTheKeyIsABoolean) {
    auto const shipped = dss::test_support::shippedSchemaOrThrow("c");
    ASSERT_TRUE(shipped->semantics().declarators.has_value());
    EXPECT_TRUE(shipped->semantics().declarators->emptyDeclarationWarns)
        << "C declares an empty declaration a warning";

    auto off = GrammarSchema::loadFromText(cDocumentWithTheKey(false), "<c-empty-off>");
    ASSERT_TRUE(off.has_value()) << "the document with the key off must load";
    auto const l = lowerUnder(*off, atBlockScope("int;"));
    ASSERT_TRUE(l->parseErrors.empty()) << "the parser accepts it:" << l->parseErrors;
    ASSERT_FALSE(l->model.hasErrors()) << dump(l->model.diagnostics());
    ASSERT_NE(l->result, nullptr);
    EXPECT_FALSE(l->result->ok) << "a language that does not say so keeps it an error";
    EXPECT_EQ(countCode(l->reporter, kNothing), 1u) << dump(l->reporter);
    for (auto const& d : l->reporter.all())
        if (d.code == kNothing) EXPECT_EQ(d.severity, DiagnosticSeverity::Error);

    auto bad = GrammarSchema::loadFromText(cDocumentWithTheKey("yes"), "<c-empty-bad>");
    ASSERT_FALSE(bad.has_value()) << "a key that is no boolean must be refused";
    bool named = false;
    for (auto const& d : bad.error())
        if (d.message.find("'emptyDeclarationWarns' must be a BOOLEAN") != std::string::npos)
            named = true;
    EXPECT_TRUE(named) << "refused for its reason";
}

// ── AN ATTRIBUTE ON AN EMPTY DECLARATION IS SAID OF NOTHING (P69 fold f26d, lane `cs`) ──────
// The declaration declares no entity, so an attribute written on it is said of nothing: the one
// warning is the empty declaration's, and no attribute, conflict or linkage is judged.
// ✔MEASURED 2026-11-17 (lane `cs` probes tn5 and tn5b; gcc 13.3.0 and clang 18.1.3, each cell an
// object at -Wall): every cell below is accepted with the one empty-declaration warning and no word
// on the attribute — at block scope, at file scope and as a for-init, the attribute before or AFTER
// the typedef name, and `static` beside `weak` (no conflict: nothing has linkage). `[[gnu::weak]] T;`
// by gcc alone (clang: "an attribute list cannot appear here"). DSS refused the six forms with the
// attribute after the typedef name at block scope (an initializer-inferred variable named T, or a
// parse error at the `;`) and the four file-scope forms with `static` beside `weak` ('weak'
// conflicting with 'local', H_UnknownLinkageSpecifier).
// RED-ON-DISABLE: the bare form's trailing attribute run gone → its cells meet a parse error; the
// prefix's linkage folded before the empty declaration is asked → the `static`-beside-`weak`
// file-scope cells are refused.
TEST(EmptyDeclaration, AnAttributeOnAnEmptyDeclarationIsSaidOfNothing) {
    struct Cell {
        std::string what, src, lead, decl;
    };
    std::vector<Cell> cells;
    for (char const* stmt : {"__attribute__((weak)) T;", "const __attribute__((weak)) T;",
                             "static T __attribute__((weak));", "const T __attribute__((weak));",
                             "T __attribute__((weak));", "T const __attribute__((weak));",
                             "[[gnu::weak]] T;", "__attribute__((unused)) T;",
                             "static int __attribute__((weak));", "static __attribute__((weak)) int;",
                             "__attribute__((weak)) int;"}) {
        cells.push_back({std::string{"block: "} + stmt, atBlockScope(stmt), "  ", stmt});
    }
    for (char const* init : {"__attribute__((weak)) T", "const T __attribute__((weak))",
                             "T __attribute__((weak))", "const __attribute__((weak)) T"}) {
        cells.push_back({std::string{"for-init: "} + init,
                         atBlockScope(std::string{"for ("} + init + "; 0; ) { }"), "",
                         std::string{init} + "; 0"});
    }
    for (char const* decl : {"T __attribute__((weak));", "static T __attribute__((weak));",
                             "__attribute__((weak)) T;", "static __attribute__((weak)) T;",
                             "static int __attribute__((weak));", "__attribute__((weak)) static int;",
                             "extern T __attribute__((weak));", "T const __attribute__((weak));",
                             "__attribute__((weak)) int;"}) {
        cells.push_back({std::string{"file: "} + decl, atFileScope(decl), "\n", decl});
    }
    for (Cell const& c : cells) {
        expectWarnsOnce(c.what.c_str(), c.src, c.lead, c.decl);
        auto const l = lowerC(c.src);
        for (DiagnosticCode const code :
             {DiagnosticCode::S_AttributeIgnoredForDeclarationKind, DiagnosticCode::S_AttributeNotHonoured,
              DiagnosticCode::S_UnknownAttribute, DiagnosticCode::H_UnknownLinkageSpecifier}) {
            EXPECT_EQ(countCode(l->model.diagnostics(), code) + countCode(l->reporter, code), 0u)
                << c.what << ": the attribute is said of nothing, yet " << diagnosticCodeName(code)
                << dump(l->model.diagnostics()) << dump(l->reporter);
        }
    }
}
