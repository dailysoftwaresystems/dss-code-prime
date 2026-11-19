// P69 (lane `cs`, fold f26e) — D-CSUBSET-ALIGNOF-VALUE-OVERALIGNED-OBJECT: `__alignof__` of a VALUE operand that
// designates a DECLARATION answers that declaration's own alignment, at BOTH tiers that answer it — the semantic
// tier's constant fold (read here through a `_Static_assert` the analysed source states: a wrong answer is
// S_StaticAssertFailed) and the HIR tier's lowering of the value a program computes (read here from the lowered
// `return`). One home decides it (`declaredAlignmentOfDesignation`); the HIR tier reads the record it leaves.
//
// THE UNIVERSE, probe n5w's cells (2026-11-19; gcc 15.2.0 and clang 21.1.8 run 20261119-070455-429b412a, MinGW-w64
// gcc 13.2.0 20261119-070507-778a3209, Apple clang 21 20261119-070526-7f0a2a8d, every reference alike): a file-scope
// object asking 64 in the GNU spelling, the C23 spelling of the GNU attribute and `_Alignas`; a block-scope object; a
// member read through an access; a LOWER request; and the controls that answer the TYPE -- a dereference here, the
// object's type (n5w.p6) in the type form's own case below. RED-ON-DISABLE: the home answering nothing leaves both
// tiers at the type's 4.
#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace dss;

namespace {

struct Lowered {
    explicit Lowered(SemanticModel m) : model(std::move(m)) {}
    SemanticModel                   model;
    DiagnosticReporter              reporter;
    std::unique_ptr<CstToHirResult> result;
};

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

[[nodiscard]] TargetSchema const* fixtureTarget() {
    static std::shared_ptr<TargetSchema const> const kTarget = [] {
        auto t = TargetSchema::loadShipped("x86_64");
        return t.has_value() ? *t : nullptr;
    }();
    return kTarget.get();
}

// The source analysed for ELF x86_64 with the target's layout, then lowered to HIR.
[[nodiscard]] std::unique_ptr<Lowered> lower(std::string src) {
    UnitBuilder builder{dss::test_support::shippedSchemaOrThrow("c"),
                        DiagnosticBudget::libraryDefault()};
    builder.setActiveFormat(ObjectFormatKind::Elf);
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    TargetSchema const* const target = fixtureTarget();
    if (target == nullptr) throw std::runtime_error("the shipped x86_64 target did not load");
    std::optional<AggregateLayoutParams> layout;
    if (target->aggregateLayoutLoaded()) layout = target->aggregateLayout();
    auto out = std::make_unique<Lowered>(
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, layout, std::nullopt,
                SelectableObjectFormatKind::of(ObjectFormatKind::Elf), target->name(),
                LongDoubleFormat::None, target));
    if (!out->model.hasErrors()) out->result = lowerToHir(out->model, out->reporter);
    return out;
}

// The value the one `return` of the module computes: a literal's, or an AlignOf's type's alignment (the answer the
// MIR fold gives it), through any conversion the lowering wraps it in.
[[nodiscard]] std::optional<std::uint64_t> returnedAlignment(Lowered const& l) {
    Hir const& h = l.result->hir;
    std::uint32_t const tag = h.id().v;
    std::optional<HirNodeId> ret;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i) {
        HirNodeId const id{i, tag};
        if (h.kind(id) != HirKind::ReturnStmt) continue;
        EXPECT_FALSE(ret.has_value()) << "the fixture holds ONE return";
        ret = id;
    }
    if (!ret.has_value() || h.children(*ret).empty()) return std::nullopt;
    HirNodeId v = h.children(*ret)[0];
    while (v.valid() && h.kind(v) == HirKind::Cast && !h.children(v).empty()) v = h.children(v)[0];
    if (!v.valid()) return std::nullopt;
    if (h.kind(v) == HirKind::Literal) {
        std::uint32_t const at = h.payload(v);
        if (at >= l.result->literalPool.size()) return std::nullopt;
        if (auto const* u = std::get_if<std::uint64_t>(&l.result->literalPool.at(at).value)) return *u;
        if (auto const* s = std::get_if<std::int64_t>(&l.result->literalPool.at(at).value))
            return static_cast<std::uint64_t>(*s);
        return std::nullopt;
    }
    if (h.kind(v) == HirKind::AlignOf && !h.children(v).empty()) {
        auto const& params = l.model.aggregateLayout();
        if (!params.has_value()) return std::nullopt;
        auto const lay = computeLayout(h.typeId(h.children(v)[0]), l.model.lattice().interner(), *params,
                                       DataModel::Lp64);
        if (!lay.has_value()) return std::nullopt;
        return lay->align.bytes();
    }
    return std::nullopt;
}

struct Cell {
    char const*   probe;      // the n5w cell it reproduces
    char const*   decls;      // file-scope declarations
    char const*   local;      // block-scope declarations of `g` (or "")
    char const*   operand;    // the `__alignof__` operand
    std::uint64_t expected;   // the references' answer
};

}  // namespace

TEST(AlignofOfADeclaration, BothTiersAnswerTheDeclarationsOwnAlignment) {
    static constexpr Cell kCells[] = {
        {"n5w.p1", "int x __attribute__((aligned(64)));\n", "", "x", 64},
        {"n5w.p2", "int x [[gnu::aligned(64)]];\n", "", "x", 64},
        {"n5w.p3", "_Alignas(64) int x;\n", "", "x", 64},
        {"n5w.p4", "", "    int y __attribute__((aligned(64))) = 0;\n    (void)y;\n", "y", 64},
        {"n5w.p5", "struct S { char c; int m __attribute__((aligned(64))); } s;\n", "", "s.m", 64},
        {"n5w.p7", "int x __attribute__((aligned(1)));\n", "", "x", 1},
        {"n5w.p8", "int x __attribute__((aligned(64)));\nint *p = &x;\n", "", "*p", 4},
        // n5w2: a lower request on a block-scope object and a static local answers 1 like a file-scope
        // one; a member's lower request answers its type's 4 (a member's request only raises).
        {"n5w2.p3", "", "    int y __attribute__((aligned(1))) = 0;\n    (void)y;\n", "y", 1},
        {"n5w2.p7", "", "    static int y __attribute__((aligned(1))) = 0;\n    (void)y;\n", "y", 1},
        {"n5w2.p4", "struct S { char c; int m __attribute__((aligned(1))); } s;\n", "", "s.m", 4},
    };
    for (Cell const& c : kCells) {
        std::string const n = std::to_string(c.expected);
        std::string const src = std::string{c.decls} + "unsigned long g(void) {\n" + c.local
            + "    _Static_assert(__alignof__(" + c.operand + ") == " + n + ", \"the constant fold\");\n"
            + "    return __alignof__(" + c.operand + ");\n}\n";
        SCOPED_TRACE(std::string{c.probe} + "\n" + src);
        auto const l = lower(src);
        EXPECT_FALSE(l->model.hasErrors())
            << "the semantic tier's constant fold must answer " << n << ":" << dump(l->model.diagnostics());
        ASSERT_NE(l->result, nullptr);
        EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
        EXPECT_EQ(returnedAlignment(*l), std::optional<std::uint64_t>{c.expected})
            << "the HIR tier must answer what the constant fold answered";
    }
}

// P69 (lane `cs`, fold f26e) — A VARIABLY MODIFIED TYPE's alignment, at both tiers: `typeAlignment`, the one
// home (an array's is its element's, a level's own `aligned` answering). Probe vm2's cells (gcc 15.2.0, clang
// 21.1.8, Apple clang 21, MinGW-w64 gcc 13.2.0): every typedef, declarator and pointee cell alike on every
// reference; the two TYPE-NAME cells (vm2.p1, p7) gcc's 16 -- clang answers 4 and says the attribute is "ignored
// when parsing type", so it does not implement the request there and gcc's documented meaning is taken.
TEST(AlignofOfAVariablyModifiedType, BothTiersAnswerItsAlignment) {
    static constexpr Cell kCells[] = {
        {"vm2.p1", "", "", "__attribute__((aligned(16))) int[n]", 16},
        {"vm2.p2", "", "    typedef int T[n] __attribute__((aligned(16)));\n", "T", 16},
        {"vm2.p3", "", "    typedef __attribute__((aligned(16))) int T[n];\n", "T", 16},
        {"vm2.p7", "", "", "__attribute__((aligned(16))) int[n][2]", 16},
        {"vm2.p8", "", "    int (__attribute__((aligned(64))) a)[n];\n    a[0] = 0;\n", "a", 64},
        {"vm2.p10", "", "    typedef int T[n] __attribute__((aligned(1)));\n", "T", 1},
        {"vm2.p11", "", "    typedef int T[n] __attribute__((aligned(64)));\n    T a;\n    a[0] = 0;\n", "a", 64},
        {"vm2.p14", "", "    typedef int T[n] __attribute__((aligned(16)));\n    T *p = 0;\n    (void)p;\n", "*p", 16},
        {"vm2.p12", "", "", "int[n]", 4},
        {"vt15", "", "    typedef double W[n][3];\n", "W", 8},
    };
    for (Cell const& c : kCells) {
        std::string const v = std::to_string(c.expected);
        std::string const src = std::string{c.decls} + "unsigned long g(int n) {\n" + c.local
            + "    _Static_assert(__alignof__(" + c.operand + ") == " + v + ", \"the constant fold\");\n"
            + "    return __alignof__(" + c.operand + ");\n}\n";
        SCOPED_TRACE(std::string{c.probe} + "\n" + src);
        auto const l = lower(src);
        EXPECT_FALSE(l->model.hasErrors())
            << "the semantic tier's constant fold must answer " << v << ":" << dump(l->model.diagnostics());
        ASSERT_NE(l->result, nullptr);
        EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
        EXPECT_EQ(returnedAlignment(*l), std::optional<std::uint64_t>{c.expected})
            << "the HIR tier must answer what the constant fold answered";
    }
}

// The TYPE form keeps the type's answer whatever an object of it asks (n5w.p6's own reading).
TEST(AlignofOfADeclaration, TheTypeFormAnswersTheType) {
    auto const l = lower("int x __attribute__((aligned(64)));\n"
                         "unsigned long g(void) {\n"
                         "    _Static_assert(__alignof__(__typeof__(x)) == 4, \"the type\");\n"
                         "    return __alignof__(__typeof__(x));\n}\n");
    EXPECT_FALSE(l->model.hasErrors()) << dump(l->model.diagnostics());
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(returnedAlignment(*l), std::optional<std::uint64_t>{4});
}
