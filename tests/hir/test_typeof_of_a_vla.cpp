// ===========================================================================
// P69 L2 (lane `lm`) — A `typeof` OF A VARIABLE-LENGTH ARRAY TYPE IS LOWERED.
//
// THE PROPERTY THIS FILE OWNS: a declaration whose head is `typeof ( type-name )`
// with a variable-length bound evaluates that bound ONCE, when the declaration is
// reached (C23 6.7.2.5, 6.8p3), and every name it declares has that size. The
// lowering captures the head's bound under the FIRST name that carries it (inner to
// that declarator's own suffixes), and every later name reaches the size that first
// one froze — an object through the VLA-typedef copy-down (`typedefVlaOriginBySymbol`),
// a later typedef name as an alias of the first (`sizeof` and its objects read the
// first's slots).
//
// ✔MEASURED at eb6f22a3 (pe64, image run natively): `int k = 0; typeof(int[n + k++])
// a;` was REFUSED by the lowering (H_UnsupportedLoweringForKind, "variable-length array
// declarator carries no array suffix to lower its runtime size from"), one layer deep
// and three. gcc 13.3.0 and clang 18.1.3 (-std=gnu2x and -std=c2x -pedantic-errors)
// build and run every shape below, the bound evaluated once (`k == 1` after
// `typeof(int[n + k++]) a, b;` and after `typedef typeof(int[n + k++]) R, S;`).
// The run-time half is the example `typeof_of_a_variable_length_array`.
//
// ── RED-ON-DISABLE ─────────────────────────────────────────────────────────
//   * the head's type name not handed to the capture → every case: the first name
//     captures no bound and the lowering refuses it;
//   * a later name capturing the head again instead of copying → the two-name
//     cases: the later name holds a bound of its own and no origin (the bound would
//     be evaluated twice);
//   * the typedef's later name not an alias → the typedef case;
//   * a typedef's own dimension over a variable-length head recording no origin →
//     the three own-dimension cases (S composes on nothing);
//   * a `typeof` of a variable-length TYPEDEF taken for a head bound → the alias-head
//     case (S captures, and finds no suffix to capture);
//   * a `typeof` of a variable-length OBJECT read whole naming no origin → the object
//     case (every name finds no suffix to capture, and the lowering refuses);
//   * the object taken for the origin of an operand that does not read it whole → the
//     row control (`typeof(v[k++])` would be sized and its `k++` never run).
// The control (no `typeof` head) holds either way.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"   // the fixture analyzes WITH a target in scope
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

// The shipped `x86_64` target, OWNED for the whole process (`analyze` takes it non-owning
// and the model republishes it for the lowering): the fixture analyzes with what the CLI
// gives an ELF x86_64 build, so an element's size is known.
[[nodiscard]] TargetSchema const* fixtureTarget() {
    static std::shared_ptr<TargetSchema const> const kTarget = [] {
        auto t = TargetSchema::loadShipped("x86_64");
        return t.has_value() ? *t : nullptr;
    }();
    return kTarget.get();
}

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
    std::optional<VaListStrategy> vaList;
    if (auto const* cc = target->callingConventionByName("sysv_amd64");
        cc != nullptr && cc->vaListLayout.has_value())
        vaList = cc->vaListLayout->strategy;
    auto out = std::make_unique<Lowered>(
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, layout, vaList,
                SelectableObjectFormatKind::of(ObjectFormatKind::Elf), target->name(),
                LongDoubleFormat::None, target));
    EXPECT_FALSE(out->model.hasErrors())
        << "the front end must be clean, or the lowering verdict is about something else:"
        << dump(out->model.diagnostics());
    out->result = lowerToHir(out->model, out->reporter);
    return out;
}

// The one symbol of `name` (the probes give every declared name its own spelling).
[[nodiscard]] std::uint32_t symbolNamed(SemanticModel const& model, std::string_view name) {
    std::uint32_t found = 0;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name != name) continue;
        EXPECT_EQ(found, 0u) << "two symbols named " << name;
        found = static_cast<std::uint32_t>(i);
    }
    EXPECT_NE(found, 0u) << "no symbol named " << name;
    return found;
}

// How many bounds the lowering captured under `name`, and the origin it gave it (0: none).
struct Capture {
    std::size_t   bounds = 0;
    std::uint32_t origin = 0;
};
[[nodiscard]] Capture captureOf(Lowered const& l, std::string_view name) {
    std::uint32_t const sym = symbolNamed(l.model, name);
    Capture c;
    if (auto it = l.result->vlaSizeExprBySymbol.find(sym);
        it != l.result->vlaSizeExprBySymbol.end())
        c.bounds = it->second.size();
    if (auto it = l.result->typedefVlaOriginBySymbol.find(sym);
        it != l.result->typedefVlaOriginBySymbol.end())
        c.origin = it->second;
    return c;
}

}  // namespace

// ── one name: the head's bound is captured under it, through every typeof layer ──
TEST(TypeofOfAVla, TheHeadsBoundIsCapturedUnderTheNameItDeclares) {
    for (char const* src : {
             "int f(int n) { int k = 0; typeof(int[n + k++]) a; return (int)sizeof a + k; }\n",
             "int f(int n) { int k = 0; typeof(typeof(typeof(int[n + k++]))) a; "
             "return (int)sizeof a + k; }\n",
         }) {
        auto l = lower(src);
        ASSERT_NE(l->result, nullptr) << src;
        EXPECT_EQ(l->reporter.errorCount(), 0u) << src << dump(l->reporter);
        Capture const a = captureOf(*l, "a");
        EXPECT_EQ(a.bounds, 1u) << "the head's bound is captured under the first name: " << src;
        EXPECT_EQ(a.origin, 0u) << src;
    }
}

// ── CONTROL: a variable-length array with no `typeof` head captures its own suffix ──
TEST(TypeofOfAVla, AVlaWithNoTypeofHeadCapturesItsOwnSuffix) {
    auto l = lower("int f(int n) { int a[n], b[n]; return (int)(sizeof a + sizeof b); }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    for (char const* name : {"a", "b"}) {
        Capture const c = captureOf(*l, name);
        EXPECT_EQ(c.bounds, 1u) << name;
        EXPECT_EQ(c.origin, 0u) << name;
    }
}

// ── the declarator's own dimension sits above the head's: both are captured ──
TEST(TypeofOfAVla, ADeclaratorsOwnDimensionIsCapturedAboveTheHeads) {
    auto l = lower("int f(int n, int m) { int k = 0; typeof(int[n + k++]) a[m]; "
                   "return (int)sizeof a + k; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    EXPECT_EQ(captureOf(*l, "a").bounds, 2u);
}

// ── two names: the bound is captured ONCE, the later name copies the first's size ──
TEST(TypeofOfAVla, ALaterNameCopiesTheSizeTheFirstFroze) {
    auto l = lower("int f(int n, int m) { int k = 0; typeof(int[n + k++]) a, b, *p, c[m]; "
                   "return (int)(sizeof a + sizeof b + sizeof c) + k + (p == 0); }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    std::uint32_t const aSym = symbolNamed(l->model, "a");
    Capture const a = captureOf(*l, "a");
    Capture const b = captureOf(*l, "b");
    Capture const p = captureOf(*l, "p");
    Capture const c = captureOf(*l, "c");
    EXPECT_EQ(a.bounds, 1u);
    EXPECT_EQ(a.origin, 0u);
    EXPECT_EQ(b.bounds, 0u) << "a later name must not evaluate the head's bound again";
    EXPECT_EQ(b.origin, aSym);
    EXPECT_EQ(p.bounds, 0u);
    EXPECT_EQ(p.origin, aSym);
    EXPECT_EQ(c.bounds, 1u) << "only its own [m]";
    EXPECT_EQ(c.origin, aSym);
}

// ── a typedef's later name is an alias of the first: its objects reach the first ──
TEST(TypeofOfAVla, ATypedefsLaterNameIsAnAliasOfTheFirst) {
    auto l = lower("int f(int n) { int k = 0; typedef typeof(int[n + k++]) R, S; "
                   "R a; S b; return (int)(sizeof a + sizeof b + sizeof(S)) + k; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    std::uint32_t const rSym = symbolNamed(l->model, "R");
    EXPECT_EQ(captureOf(*l, "R").bounds, 1u);
    EXPECT_EQ(captureOf(*l, "S").bounds, 0u) << "S must not evaluate the head's bound again";
    EXPECT_EQ(captureOf(*l, "a").origin, rSym);
    EXPECT_EQ(captureOf(*l, "b").origin, rSym) << "an object of S reaches R's frozen size";
}

// ── a typedef with dimensions of its own over a variable-length head composes on the
//    typedef that head names: the typeof head's first name, or a VLA typedef ──
TEST(TypeofOfAVla, ATypedefsOwnDimensionComposesOnTheTypedefItsHeadNames) {
    for (char const* src : {
             "int f(int n, int m) { int k = 0; typedef typeof(int[n + k++]) R, S[m]; S b; "
             "return (int)(sizeof b + sizeof(R)) + k; }\n",
             "int f(int n, int m) { typedef int R[n]; typedef R S[m]; S b; "
             "return (int)(sizeof b + sizeof(R)); }\n",
             "int f(int n, int m) { typedef int R[n]; typedef typeof(R) S[m]; S b; "
             "return (int)(sizeof b + sizeof(R)); }\n",
         }) {
        auto l = lower(src);
        ASSERT_NE(l->result, nullptr) << src;
        EXPECT_EQ(l->reporter.errorCount(), 0u) << src << dump(l->reporter);
        std::uint32_t const rSym = symbolNamed(l->model, "R");
        std::uint32_t const sSym = symbolNamed(l->model, "S");
        Capture const s = captureOf(*l, "S");
        EXPECT_EQ(s.bounds, 1u) << "S captures its own dimension only: " << src;
        EXPECT_EQ(s.origin, rSym) << "S composes on the typedef its head names: " << src;
        EXPECT_EQ(captureOf(*l, "b").origin, sSym)
            << "an object of S copies the slots S froze: " << src;
    }
}

// ── a typeof of a variable-length TYPEDEF is an alias head: it writes no bound of its own ──
TEST(TypeofOfAVla, ATypeofOfAVlaTypedefIsAnAliasHead) {
    auto l = lower("int f(int n, int m) { typedef int R[n]; typedef typeof(R) S; n = 99; S b; "
                   "typeof(R) a; typeof(R) c[m]; "
                   "return (int)(sizeof a + sizeof b + sizeof c); }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    std::uint32_t const rSym = symbolNamed(l->model, "R");
    EXPECT_EQ(captureOf(*l, "S").bounds, 0u) << "S evaluates nothing: R's bound was frozen at R";
    EXPECT_EQ(captureOf(*l, "b").origin, rSym);
    EXPECT_EQ(captureOf(*l, "a").origin, rSym);
    EXPECT_EQ(captureOf(*l, "a").bounds, 0u);
    EXPECT_EQ(captureOf(*l, "c").origin, rSym);
    EXPECT_EQ(captureOf(*l, "c").bounds, 1u) << "only its own [m]";
}

// ── a typeof of a variable-length array OBJECT read whole: every name the head declares
//    takes the size the object froze at its own declaration — an object, two names, its
//    own dimension above it, a pointer, a typedef (an alias of the object) and a typedef
//    with a dimension of its own; v's bound is evaluated once, at v ──
TEST(TypeofOfAVla, ATypeofOfAVlaObjectTakesTheSizeTheObjectFroze) {
    auto l = lower("int f(int n, int m) { int v[n]; n = 99; typeof(v) a, b; typeof((v)) c[m]; "
                   "typeof(v) *p = &v; typedef typeof(v) T; T d; typedef typeof(v) S[m]; S e; "
                   "return (int)(sizeof a + sizeof b + sizeof c + sizeof *p + sizeof d + sizeof(T) "
                   "+ sizeof e + sizeof(S)); }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    std::uint32_t const vSym = symbolNamed(l->model, "v");
    std::uint32_t const sSym = symbolNamed(l->model, "S");
    EXPECT_EQ(captureOf(*l, "v").bounds, 1u) << "v's own [n], the one evaluation";
    for (char const* name : {"a", "b", "p", "d"}) {
        Capture const c = captureOf(*l, name);
        EXPECT_EQ(c.bounds, 0u) << name << " must not evaluate a bound of its own";
        EXPECT_EQ(c.origin, vSym) << name << " takes the size v froze";
    }
    EXPECT_EQ(captureOf(*l, "c").bounds, 1u) << "only its own [m]";
    EXPECT_EQ(captureOf(*l, "c").origin, vSym) << "c's levels below its own are v's";
    EXPECT_EQ(captureOf(*l, "T").bounds, 0u) << "T evaluates nothing: an alias of v";
    EXPECT_EQ(captureOf(*l, "S").bounds, 1u) << "only its own [m]";
    EXPECT_EQ(captureOf(*l, "S").origin, vSym) << "S composes on v";
    EXPECT_EQ(captureOf(*l, "e").origin, sSym) << "an object of S copies the slots S froze";
}

// THE CONTROL: an operand that does not read an object whole is EVALUATED (C23 6.7.2.5p4:
// its type is variably modified) — `typeof(v[k++]) r;` leaves k == 1 on gcc 13.3.0 and
// clang 18.1.3 — so it names no origin: the slot v froze for its row would size r right
// and skip the `k++`. Its declaration keeps the loud refusal until the composite form is
// evaluated where it stands (owed).
TEST(TypeofOfAVla, ATypeofOfARowOfAVlaObjectNamesNoOrigin) {
    auto l = lower("int f(int n, int m) { int k = 0; int v[n][m]; typeof(v[k++]) r; "
                   "return (int)sizeof r + k; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(captureOf(*l, "r").origin, 0u) << "a row is not the whole object";
    EXPECT_GT(l->reporter.errorCount(), 0u) << "refused loudly, never sized without its k++";
}
