// ── A SCALAR'S BRACE INITIALIZER: MORE BRACES, MORE ELEMENTS (cycle P69 round 1, lane `lm`) ──
//
// C 6.7.9p11 gives a scalar "a single expression, optionally enclosed in braces", and C
// 6.7.9p2 makes an element beyond the first a constraint violation. The lowering refused both
// (S_InvalidScalarInitializer): `int v = {{42}};` and `int v = {42, 7};`. gcc 13.3.0 and
// clang 18.1.3 build every such shape with a warning (✔MEASURED 2026-10-16, every program
// RUN): the scalar holds the innermost FIRST value, `{{}}` is zero, an excess element is
// dropped UNEVALUATED whatever its type or constness — in a static object too — and a
// designator at any level is refused, as a structure inside `{{ s }}` is. Both warn once per
// brace level beyond the first, and around a scalar member or element of an aggregate
// already at the first; both warn the excess.
//
// What C makes of it is the LANGUAGE's statement (`hirLowering.scalarBraceInitializer` in
// c.lang.json): each shape's warning code, and a document that names none refuses the shape.
// Three tiers read the same levels to the same value — the lowering
// (`lowerScalarBraceInit`), the semantic tier's element walks (`forEachPlacedBraceValue`: the
// element judge and the static-initializer check) and the constant evaluator
// (`valueStepChild`) — and each has its cases here.
//
// THE MUTATIONS are arms of `dssharness check-mutations` (tests/mutations/arms.registry),
// written, not read: the lowering's step through a level, its excess rule, the semantic
// walk's step through a level, its excess skip, its designator refusal, and the evaluator's
// first element. The runnable half is examples/c/scalar_brace_initializer_takes_its_first_value.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "repo_root.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace dss;

namespace {

constexpr DiagnosticCode kBraces  = DiagnosticCode::S_BracesAroundScalarInitializer;
constexpr DiagnosticCode kExcess  = DiagnosticCode::S_ExcessElementsInScalarInitializer;
constexpr DiagnosticCode kRefused = DiagnosticCode::S_InvalidScalarInitializer;

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

[[nodiscard]] std::string dump(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        out += "\n    ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
    return out.empty() ? std::string{" <no diagnostics>"} : out;
}

// The shipped C document with `hirLowering.scalarBraceInitializer` REPLACED by `value`
// (erased when `value` is null). The key must be there to replace: a fixture that "removed"
// a key the document never stated would hand back the shipped rule under a name that says
// otherwise. Returns the load's result, so a malformed declaration's refusal can be read.
[[nodiscard]] LoadResult<std::shared_ptr<GrammarSchema>>
cDocumentWith(nlohmann::json const& value, std::string_view label) {
    std::filesystem::path const path = dss::test::configRoot() / "sources" / "c.lang.json";
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        throw std::runtime_error("cannot open the shipped C document: " + path.string());
    }
    nlohmann::json doc = nlohmann::json::parse(in);
    nlohmann::json& hl = doc.at("hirLowering");
    if (!hl.contains("scalarBraceInitializer")) {
        throw std::runtime_error("the shipped C document states no scalarBraceInitializer");
    }
    if (value.is_null()) {
        hl.erase("scalarBraceInitializer");
    } else {
        hl["scalarBraceInitializer"] = value;
    }
    return GrammarSchema::loadFromText(doc.dump(), label);
}

// One translation unit taken through the front end and the lowering under `schema`.
struct Lowered {
    explicit Lowered(SemanticModel m) : model(std::move(m)) {}
    SemanticModel                   model;
    DiagnosticReporter              reporter;
    std::unique_ptr<CstToHirResult> result;
};

[[nodiscard]] SemanticModel analyzeUnder(std::shared_ptr<GrammarSchema const> schema,
                                         std::string src) {
    UnitBuilder builder{std::move(schema), DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    return analyze(cu, DiagnosticBudget::libraryDefault());
}

[[nodiscard]] std::unique_ptr<Lowered> lowerUnder(std::shared_ptr<GrammarSchema const> schema,
                                                  std::string src) {
    auto out = std::make_unique<Lowered>(analyzeUnder(std::move(schema), std::move(src)));
    EXPECT_FALSE(out->model.hasErrors())
        << "the front end must be clean, or the lowering verdict below is about something "
           "else:" << dump(out->model.diagnostics());
    out->result = lowerToHir(out->model, out->reporter);
    return out;
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> shippedC() {
    return dss::test_support::shippedSchemaOrThrow("c");
}

// Every integer value in the unit's literal pool.
[[nodiscard]] std::vector<std::int64_t> integerLiterals(CstToHirResult const& res) {
    std::vector<std::int64_t> out;
    for (std::size_t i = 0; i < res.literalPool.size(); ++i) {
        auto const& v = res.literalPool.at(i);
        if (std::holds_alternative<std::int64_t>(v.value)) out.push_back(std::get<std::int64_t>(v.value));
    }
    return out;
}

[[nodiscard]] bool holds(std::vector<std::int64_t> const& v, std::int64_t x) {
    for (std::int64_t y : v) if (y == x) return true;
    return false;
}

[[nodiscard]] std::size_t countKind(Hir const& hir, HirKind k) {
    std::size_t n = 0;
    for (std::uint32_t i = 1; i < hir.nodeCount(); ++i)
        if (hir.kind(HirNodeId{i, hir.id().v}) == k) ++n;
    return n;
}

struct Row {
    char const*               body;      // the declarations inside `int f(void) { … return 0; }`
    std::size_t               braces;    // S_BracesAroundScalarInitializer expected
    std::size_t               excess;    // S_ExcessElementsInScalarInitializer expected
    std::vector<std::int64_t> present;   // integer literals the lowered unit holds
    std::vector<std::int64_t> absent;    // integer literals it must not hold (never lowered)
};

void expectRow(Row const& row) {
    std::string src = "struct S { int a; int b; };\nstatic int bump(void) { return 1000; }\n"
                      "int f(void) {\n    ";
    src += row.body;
    src += "\n    return 0;\n}\n";
    auto const l = lowerUnder(shippedC(), src);
    ASSERT_NE(l->result, nullptr) << row.body;
    EXPECT_TRUE(l->result->ok) << row.body << dump(l->reporter);
    EXPECT_EQ(countCode(l->reporter, kRefused), 0u) << row.body << dump(l->reporter);
    EXPECT_EQ(countCode(l->reporter, kBraces), row.braces) << row.body << dump(l->reporter);
    EXPECT_EQ(countCode(l->reporter, kExcess), row.excess) << row.body << dump(l->reporter);
    auto const lits = integerLiterals(*l->result);
    for (std::int64_t x : row.present) EXPECT_TRUE(holds(lits, x)) << row.body << ": " << x;
    for (std::int64_t x : row.absent) EXPECT_FALSE(holds(lits, x)) << row.body << ": " << x;
}

}  // namespace

// MORE BRACES: the innermost value initializes, one warning per brace level beyond the first
// — and around a scalar SUBOBJECT already at the first (gcc and clang warn `struct S s =
// {{42}};` and `int a[2] = {{40}, {2}};`). One brace pair around a whole scalar and a value
// elided into a member say nothing.
TEST(ScalarBraceInitializer, MoreBracesTakeTheInnermostValueWithAWarningPerLevel) {
    Row const rows[] = {
        {"int v = {42}; (void)v;", 0, 0, {42}, {}},
        {"int v = {{42}}; (void)v;", 1, 0, {42}, {}},
        {"int v = {{{42}}}; (void)v;", 2, 0, {42}, {}},
        {"static int v = {{42}}; (void)v;", 1, 0, {42}, {}},
        {"int v = {{}}; (void)v;", 1, 0, {}, {}},
        {"int v = {{{}}}; (void)v;", 2, 0, {}, {}},
        {"int v = (int){{42}}; (void)v;", 1, 0, {42}, {}},
        {"struct S s = {40, 2}; (void)s;", 0, 0, {40, 2}, {}},
        {"struct S s = {{40}, 2}; (void)s;", 1, 0, {40, 2}, {}},
        {"struct S s = {{{40}}, 2}; (void)s;", 2, 0, {40, 2}, {}},
        {"int a[2] = {{40}, {2}}; (void)a;", 2, 0, {40, 2}, {}},
    };
    for (Row const& row : rows) expectRow(row);
}

// MORE ELEMENTS: the first initializes, the rest are never lowered — a call in one is never
// made — and the list warns once, at its first excess element.
TEST(ScalarBraceInitializer, MoreElementsTakeTheFirstAndLowerNoneOfTheRest) {
    Row const rows[] = {
        {"int v = {42, 7}; (void)v;", 0, 1, {42}, {7}},
        {"int v = {42, 7, 8}; (void)v;", 0, 1, {42}, {7, 8}},
        {"static int v = {42, 7}; (void)v;", 0, 1, {42}, {7}},
        {"int v = {{42}, 7}; (void)v;", 1, 1, {42}, {7}},
        {"int v = {{42, 7}}; (void)v;", 1, 1, {42}, {7}},
        {"int v = {42, {7}}; (void)v;", 0, 1, {42}, {7}},
        {"struct S s = {{40, 7}, 2}; (void)s;", 1, 1, {40, 2}, {7}},
        {"int v = (int){42, 7}; (void)v;", 0, 1, {42}, {7}},
    };
    for (Row const& row : rows) expectRow(row);

    auto const l = lowerUnder(shippedC(),
                              "static int bump(void) { return 1000; }\n"
                              "int f(void) { int v = {42, bump()}; return v; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_TRUE(l->result->ok) << dump(l->reporter);
    EXPECT_EQ(countCode(l->reporter, kExcess), 1u) << dump(l->reporter);
    EXPECT_EQ(countKind(l->result->hir, HirKind::Call), 0u)
        << "an excess element is dropped unevaluated: `bump()` must not be called";
}

// THE CONSTANT IS THE FIRST VALUE TOO: the evaluator reads a scalar's braces as their first
// undesignated element, level by level, so the semantic tier's `_Static_assert` sees the
// value the lowering stores. ✔MEASURED 2026-10-16: clang 18.1.3 folds `static const int c`
// initialized by each form below to 5 in `_Static_assert` (and in an array bound, probes
// sb2/sb5); gcc 13.3.0 folds `constexpr int c = {5, 7};` and `{{5}}` to 5.
TEST(ScalarBraceInitializer, TheConstantIsTheFirstValueInEveryTier) {
    char const* const inits[] = {"{5}", "{{5}}", "{{{5}}}", "{5, 7}", "{{5}, 7}", "{{5, 7}}"};
    for (char const* init : inits) {
        std::string const decl = std::string{"static const int c = "} + init + ";\n";
        SemanticModel const ok = analyzeUnder(
            shippedC(), decl + "_Static_assert(c == 5, \"the first value\");\n"
                               "int main(void) { return 0; }\n");
        EXPECT_FALSE(ok.hasErrors()) << init << dump(ok.diagnostics());
        // A `constexpr` object's initializer is a constant by the same reading.
        SemanticModel const cx = analyzeUnder(
            shippedC(), std::string{"int main(void) {\n    constexpr int k = "} + init +
                            ";\n    _Static_assert(k == 5, \"the first value\");\n    return k - 5;\n}\n");
        EXPECT_FALSE(cx.hasErrors()) << "constexpr " << init << dump(cx.diagnostics());
        SemanticModel const no = analyzeUnder(
            shippedC(), decl + "_Static_assert(c != 5, \"not the first value\");\n"
                               "int main(void) { return 0; }\n");
        std::size_t failed = 0;
        for (auto const& d : no.diagnostics().all())
            if (d.code == DiagnosticCode::S_StaticAssertFailed) ++failed;
        EXPECT_EQ(failed, 1u) << init << ": the assertion must be folded, and fail"
                              << dump(no.diagnostics());
    }
}

// THE SEMANTIC TIER JUDGES THE VALUE THE LOWERING STORES, at any level, and refuses a
// designator in a scalar's braces at any level (C 6.7.9p6-p7): both references refuse each.
// An excess element is judged by nothing, as gcc and clang judge none.
TEST(ScalarBraceInitializer, ADesignatorOrAValueOfAnotherTypeIsRefusedAtAnyLevel) {
    struct Refusal { char const* body; DiagnosticCode code; };
    Refusal const refusals[] = {
        {"int v = {.x = 42};", kRefused},
        {"int v = {[0] = 42};", kRefused},
        {"int v = {{[0] = 42}};", kRefused},
        {"int v = {{{.x = 42}}};", kRefused},
        {"struct T { int a; } t = {{.a = 1}};", kRefused},
        {"struct T { int a; } t0 = {1}; int v = {{t0}};", DiagnosticCode::S_TypeMismatch},
        {"struct T { int a; } t0 = {1}; int v = {{{t0}}, 7};", DiagnosticCode::S_TypeMismatch},
    };
    for (Refusal const& r : refusals) {
        std::string const src = std::string{"int f(void) {\n    "} + r.body + "\n    return 0;\n}\n";
        SemanticModel const m = analyzeUnder(shippedC(), src);
        std::size_t n = 0;
        for (auto const& d : m.diagnostics().all())
            if (d.code == r.code && d.severity == DiagnosticSeverity::Error) ++n;
        EXPECT_EQ(n, 1u) << r.body << dump(m.diagnostics());
    }
    char const* const accepted[] = {
        "struct T { int a; } t0 = {1}; int v = {42, t0}; (void)v;",
        "int v = {42, \"x\"}; (void)v;",
    };
    for (char const* body : accepted) {
        std::string const src = std::string{"int f(void) {\n    "} + body + "\n    return 0;\n}\n";
        SemanticModel const m = analyzeUnder(shippedC(), src);
        EXPECT_FALSE(m.hasErrors()) << body << dump(m.diagnostics());
    }
}

// AN ELEMENT THAT INITIALIZES NOTHING IS NO CONSTANTNESS QUESTION: a static object's excess
// element — of a scalar or of an aggregate, at any depth — is dropped unevaluated, and gcc
// and clang build each with the excess warning alone (✔MEASURED, probes sb3/sb4: `g()` never
// called); a call that INITIALIZES something is refused by both, and stays refused.
TEST(ScalarBraceInitializer, AnExcessElementOfAStaticObjectIsNoConstantQuestion) {
    std::string const g = "struct S { int a; int b; };\nunion U { int a; char c; };\n"
                          "static int g(void) { return 7; }\n";
    char const* const accepted[] = {
        "static int v = {42, g()};",
        "static int v = {{42, g()}};",
        "static int v = {{42}, g()};",
        "static int a[1] = {42, g()};",
        "static int a[1][1] = {{42, g()}};",
        "static struct S s = {40, 2, g()};",
        "static struct S s = {{40, g()}, 2};",
        "static union U u = {42, g()};",
        "static char s[] = {\"*\", g()};",
    };
    for (char const* decl : accepted) {
        SemanticModel const m = analyzeUnder(shippedC(), g + decl + "\nint main(void) { return 0; }\n");
        EXPECT_FALSE(m.hasErrors()) << decl << dump(m.diagnostics());
    }
    char const* const refused[] = {
        "static int v = {g(), 42};",
        "static int a[1] = {g(), 42};",
        "static struct S s = {{g()}, 2};",
    };
    for (char const* decl : refused) {
        SemanticModel const m = analyzeUnder(shippedC(), g + decl + "\nint main(void) { return 0; }\n");
        std::size_t n = 0;
        for (auto const& d : m.diagnostics().all())
            if (d.code == DiagnosticCode::S_StaticInitializerNotConstant) ++n;
        EXPECT_EQ(n, 1u) << decl << dump(m.diagnostics());
    }
}

// A DOCUMENT THAT DECLARES NO WARNING REFUSES THE SHAPE: under the shipped C document with
// the declaration erased, more braces and more elements are refused as they were before it —
// and a scalar subobject's ONE brace pair, which C allows, stands, unwarned.
TEST(ScalarBraceInitializer, ADocumentThatDeclaresNoWarningRefusesTheShape) {
    auto const loaded = cDocumentWith(nullptr, "<c-no-scalar-brace-declaration>");
    ASSERT_TRUE(loaded.has_value()) << "the C document without the declaration must load";
    std::shared_ptr<GrammarSchema const> const schema = *loaded;
    for (char const* body : {"int v = {{42}};", "int v = {42, 7};", "int v = {{}};"}) {
        auto const l = lowerUnder(schema, std::string{"int f(void) { "} + body + " return v; }\n");
        ASSERT_NE(l->result, nullptr) << body;
        EXPECT_FALSE(l->result->ok) << body;
        EXPECT_EQ(countCode(l->reporter, kRefused), 1u) << body << dump(l->reporter);
        EXPECT_EQ(countCode(l->reporter, kBraces) + countCode(l->reporter, kExcess), 0u) << body;
    }
    auto const l = lowerUnder(schema, "struct S { int a; int b; };\n"
                                      "int f(void) { struct S s = {{40}, 2}; return s.a; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_TRUE(l->result->ok) << dump(l->reporter);
    EXPECT_EQ(countCode(l->reporter, kRefused) + countCode(l->reporter, kBraces), 0u)
        << dump(l->reporter);
}

// THE DECLARATION IS READ LOUD: not an object, a key the block does not know, a code that is
// not a name, a name that is no code, a sentinel name — each refused at load, never a
// warning that never fires.
TEST(ScalarBraceInitializer, AMalformedDeclarationIsRefusedAtLoad) {
    nlohmann::json const bad[] = {
        5,
        nlohmann::json::parse(R"({ "nestedBrace": "S_BracesAroundScalarInitializer" })"),
        nlohmann::json::parse(R"({ "nestedBraces": 5 })"),
        nlohmann::json::parse(R"({ "excessElements": "S_NoSuchCode" })"),
        nlohmann::json::parse(R"({ "nestedBraces": "None" })"),
    };
    for (nlohmann::json const& v : bad) {
        auto const loaded = cDocumentWith(v, "<c-malformed-scalar-brace-declaration>");
        ASSERT_FALSE(loaded.has_value()) << v.dump() << " must be refused";
        bool named = false;
        for (auto const& d : loaded.error())
            if (d.code == DiagnosticCode::C_InvalidHirLowering
                && d.path.find("scalarBraceInitializer") != std::string::npos)
                named = true;
        EXPECT_TRUE(named) << v.dump() << ": refused by C_InvalidHirLowering at the key";
    }
}
