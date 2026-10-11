// ===========================================================================
// A BRACE ELEMENT IS THE EXPRESSION THAT WAS WRITTEN (cycle P69 round 1, lane `lm`)
//
// Two defects of the base, both LOUD refusals of valid C, both in what the lowering
// took a brace element's VALUE to be. Found while fixing the constant-aggregate
// member read ([[D-C-CONSTANT-AGGREGATE-MEMBER-READ-WITHOUT-ITS-INITIALIZERS-OPERATOR]]),
// whose cause they share: a node with one child that is not a token was taken for
// grouping, and a unary operator's node has exactly one.
//
//   (1) [[D-HIR-BRACE-PEEL-TAKES-AN-OPERATORS-OPERAND-FOR-THE-ELEMENT]] — the HIR
//       tier's own peel (`peelToBraceInitOrCore`) walked through every such node, so
//       the STRING under an operator answered "is this the braced string that
//       initializes the array" (C 6.7.9p14): `char s[] = { *"abc" };`, `{ !"abc" }`,
//       `{ sizeof "abc" }`, `{ *"abc", 'z' }` were refused by the MIR verifier (a
//       `store` of a Char into an Array), and at file scope the element after one
//       was warned as in excess of a string.
//   (2) [[D-HIR-WHOLE-STRUCTURE-BRACE-ELEMENT-IS-PLACED-BY-TYPE-STAMPS]] — whether an
//       element initializes a structure or union subobject WHOLE (6.7.9p13) was
//       answered twice: by the analyzer from the value's TYPE, and by the lowering
//       from the type STAMPS under the value's node. An operator's node carries no
//       stamp, so `struct S arr[] = { src[1], src[0] };` — and `{ *p }`, `{ (0 ? s :
//       s) }`, `{ (0, s) }`, `{ *(&s + i) }`, `{ mk() }` — went into the structure's
//       FIRST MEMBER while the analyzer had sized the list for whole structures: a
//       verifier failure ("ConstructAggregate … child[0] type … doesn't match field
//       type").
//
// ✔REFERENCES, 2026-10-10, every case its own translation unit, each reference
// probed separately and every build RUN to 42 (the lane's reference runs are in the
// two rows): cl 19.51.36260, MinGW-w64 gcc 13.2.0, gcc 13.3.0 and clang 18.1.3 on
// x86_64 Linux, gcc 13.3.0 on arm64 Linux, AppleClang 21 on arm64 macOS.
//
// THE FIXES. (1) The peel takes its steps by the ONE statement of what a wrapper is
// (`nodeSaysSomethingItself`, `core/types/decl_prefix_strip.hpp`), shared with the
// semantic tier. (2) The analyzer's answer is handed over as a fact
// (`SemanticModel::braceElementValueTypeFor`) and the lowering reads it before any
// stamp — it cannot type the value by lowering it first, because the placement is
// decided before anything is lowered, so that an element in excess stays
// UNEVALUATED (pinned below).
//
// RED-ON-DISABLE (arms of `tests/mutations/arms.registry`): the peel stopping only
// at a brace list again → (1); the fact not read, or not written → (2), while the
// forms whose node IS stamped stay green — the control that nothing else moved.
//
// ⚠ MUST run through ctest: the shipped documents are found through
// `DSS_CONFIG_ROOT`, which only `dss_add_test` sets.
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
#include "repo_root.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

using namespace dss;

namespace {

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

[[nodiscard]] std::size_t countSeverity(DiagnosticReporter const& r, DiagnosticSeverity sv) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.severity == sv) ++n;
    return n;
}

struct Lowered {
    std::size_t semanticErrors = 0;
    bool        lowered = false;
    std::size_t hirErrors = 0;
    std::size_t hirExcess = 0;
    std::string diagnostics;   // every diagnostic both tiers reported, for the failure message
};

void appendDiagnostics(std::string& out, DiagnosticReporter const& r) {
    for (auto const& d : r.all())
        out += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
}

// The shipped `x86_64` target, OWNED for the whole process (`analyze` takes it
// non-owning and the model republishes it for the lowering): the fixture analyzes
// with what the CLI gives an ELF x86_64 build, because without the layout a `sizeof`
// does not fold.
[[nodiscard]] TargetSchema const* fixtureTarget() {
    static std::shared_ptr<TargetSchema const> const kTarget = [] {
        auto t = TargetSchema::loadShipped("x86_64");
        return t.has_value() ? *t : nullptr;
    }();
    return kTarget.get();
}

// Handed the lowered module while its model — whose lattice its types live in — is alive.
using Inspect = std::function<void(CstToHirResult const&)>;

// c source → semantic model → HIR. A `_Static_assert` in the source pins a SIZE at the
// semantic tier; the lowering pins what the walker makes of the list, and `inspect`
// (when given, and only for a module that lowered) reads WHERE it put each value.
[[nodiscard]] Lowered lowerC(std::string src, Inspect const& inspect = {}) {
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.setActiveFormat(ObjectFormatKind::Elf);
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    Lowered out;
    // A malformed fixture (a parse error) is counted as an error and named, never left
    // to surface as a phantom semantic miss.
    for (auto const& t : cu->trees()) {
        for (auto const& d : t.diagnostics().all()) {
            if (d.severity != DiagnosticSeverity::Error) continue;
            ++out.semanticErrors;
            out.diagnostics += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
        }
    }
    if (out.semanticErrors != 0) return out;
    TargetSchema const* const target = fixtureTarget();
    if (target == nullptr) throw std::runtime_error("the shipped x86_64 target did not load");
    std::optional<AggregateLayoutParams> layout;
    if (target->aggregateLayoutLoaded()) layout = target->aggregateLayout();
    std::optional<VaListStrategy> vaList;
    if (auto const* cc = target->callingConventionByName("sysv_amd64");
        cc != nullptr && cc->vaListLayout.has_value())
        vaList = cc->vaListLayout->strategy;
    SemanticModel model = analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, layout,
                                  vaList, SelectableObjectFormatKind::of(ObjectFormatKind::Elf),
                                  target->name(), LongDoubleFormat::None, target);
    out.semanticErrors = countSeverity(model.diagnostics(), DiagnosticSeverity::Error);
    appendDiagnostics(out.diagnostics, model.diagnostics());
    if (out.semanticErrors != 0) return out;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    out.lowered = res->ok;
    out.hirErrors = countSeverity(r, DiagnosticSeverity::Error);
    out.hirExcess = countCode(r, DiagnosticCode::S_ExcessInitializerElements);
    appendDiagnostics(out.diagnostics, r);
    if (inspect && res != nullptr && res->ok) inspect(*res);
    return out;
}

// The value of a lowered integer LITERAL (its literal-pool entry), or nullopt for any
// other node.
[[nodiscard]] std::optional<std::int64_t> literalValue(CstToHirResult const& res, HirNodeId n) {
    if (!n.valid() || res.hir.kind(n) != HirKind::Literal) return std::nullopt;
    std::uint32_t const at = res.hir.payload(n);
    if (at >= res.literalPool.size()) return std::nullopt;
    if (auto const* v = std::get_if<std::int64_t>(&res.literalPool.at(at).value)) return *v;
    return std::nullopt;
}

// The initializer of the first local declared in the module's first function body.
[[nodiscard]] HirNodeId firstLocalInit(Hir const& hir) {
    for (HirNodeId d : hir.moduleDecls(hir.root())) {
        if (hir.kind(d) != HirKind::Function) continue;
        for (HirNodeId s : hir.children(hir.functionBody(d))) {
            if (hir.kind(s) != HirKind::VarDecl) continue;
            if (auto const init = hir.varDeclInit(s)) return *init;
        }
    }
    return HirNodeId{};
}

// Every node of kind `k` in the module.
[[nodiscard]] std::size_t countKind(Hir const& h, HirKind k) {
    std::size_t n = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i)
        if (h.kind(HirNodeId{i, tag}) == k) ++n;
    return n;
}

struct Case {
    char const* what;
    std::string src;
    std::size_t excessWarnings;   // S_ExcessInitializerElements expected from the walker
};

void expectLowersCleanly(std::initializer_list<Case> cases) {
    for (Case const& c : cases) {
        Lowered const l = lowerC(c.src);
        EXPECT_EQ(l.semanticErrors, 0u) << c.what << "\n" << c.src << l.diagnostics;
        EXPECT_TRUE(l.lowered) << c.what << "\n" << c.src << l.diagnostics;
        EXPECT_EQ(l.hirErrors, 0u) << c.what << "\n" << c.src << l.diagnostics;
        EXPECT_EQ(l.hirExcess, c.excessWarnings) << c.what << "\n" << c.src << l.diagnostics;
    }
}

// `char s[] = { <elements> };` as the first local of `f`: the list must lower to an
// AGGREGATE of `slots` elements, with no element reported as in excess of a string.
void expectCharacterElements(char const* what, char const* elements, std::size_t slots) {
    bool inspected = false;
    std::string const src = std::string{"void f(void) { char s[] = { "} + elements + " };\n"
        "  _Static_assert(sizeof(s) == " + std::to_string(slots) + ", \"the analyzer's size\"); }\n";
    Lowered const l = lowerC(src, [&](CstToHirResult const& res) {
        HirNodeId const init = firstLocalInit(res.hir);
        ASSERT_TRUE(init.valid()) << what;
        EXPECT_EQ(res.hir.kind(init), HirKind::ConstructAggregate)
            << what << ": an operator's result is an ELEMENT of the list, not the string under it";
        EXPECT_EQ(res.hir.children(init).size(), slots) << what;
        inspected = true;
    });
    EXPECT_EQ(l.semanticErrors, 0u) << what << "\n" << src << l.diagnostics;
    EXPECT_TRUE(l.lowered) << what << "\n" << src << l.diagnostics;
    EXPECT_EQ(l.hirErrors, 0u) << what << "\n" << src << l.diagnostics;
    EXPECT_EQ(l.hirExcess, 0u)
        << what << ": no element here follows a string that initializes the array\n" << l.diagnostics;
    EXPECT_TRUE(inspected) << what;
}

constexpr char const* kTypes =
    "struct S { int a; int b; };\n"
    "struct W { struct S in; int n; };\n"
    "static struct S src[2] = { { 1, 2 }, { 3, 4 } };\n"
    "static struct S one = { 1, 2 };\n"
    "struct S mk0(void);\n"
    "struct S mk(int);\n";

}  // namespace

// ── (1) an operator over a string is an element ──────────────────────────────
TEST(BraceElementAsWritten, AnOperatorOverAStringIsAnElementNotTheString) {
    expectCharacterElements("a dereference", "*\"abc\"", 1);
    expectCharacterElements("a dereference, then another element", "*\"abc\", 'z'", 2);
    expectCharacterElements("a logical negation", "!\"abc\"", 1);
    expectCharacterElements("a logical negation, then another element", "!\"abc\", 9", 2);
    expectCharacterElements("the size of a string", "sizeof \"abc\"", 1);
    expectCharacterElements("under grouping, still the operator's result", "(*\"abc\"), (sizeof \"abc\")", 2);
    expectLowersCleanly({
        {"file scope: the size of a string is one element",
         "static char t[] = { sizeof \"abc\" };\n_Static_assert(sizeof(t) == 1, \"1\");\n"
         "int main(void) { return t[0] == 4 ? 42 : 1; }\n", 0},
    });
}

// THE CONTROL OF (1): a string under GROUPING is still the string that initializes
// the array (C 6.7.9p14) — parentheses and `__extension__` say nothing themselves.
TEST(BraceElementAsWritten, AStringUnderGroupingStillInitializesTheArray) {
    expectLowersCleanly({
        {"`{ (\"abc\") }`, file and block scope",
         "static char s[] = { (\"abc\") };\n"
         "int main(void) { char t[] = { (\"abcd\") };\n"
         "  _Static_assert(sizeof(s) == 4 && sizeof(t) == 5, \"\"); return s[2] == 'c' && t[3] == 'd' ? 42 : 1; }\n", 0},
        {"`(\"abc\")`, file and block scope",
         "static char s[] = (\"abc\");\n"
         "int main(void) { char t[] = (\"abcd\");\n"
         "  _Static_assert(sizeof(s) == 4 && sizeof(t) == 5, \"\"); return s[2] == 'c' && t[3] == 'd' ? 42 : 1; }\n", 0},
        {"`__extension__ \"abc\"`",
         "static char s[] = __extension__ \"abc\";\n_Static_assert(sizeof(s) == 4, \"4\");\n"
         "int main(void) { return s[2] == 'c' ? 42 : 1; }\n", 0},
        {"a member and an element met by a string",
         "struct C { char c[4]; int n; };\n"
         "static struct C g = { \"ab\", 5 };\nstatic struct C h = { { \"cd\" }, 6 };\n"
         "static char a[2][4] = { \"ab\", \"cd\" };\n"
         "int main(void) { return g.c[1] == 'b' && h.n == 6 && a[1][1] == 'd' ? 42 : 1; }\n", 0},
    });
}

// ── (2) a whole structure, whatever expression writes it ─────────────────────
TEST(BraceElementAsWritten, AWholeStructureElementIsPlacedWholeWhateverExpressionWritesIt) {
    std::string const t = kTypes;
    expectLowersCleanly({
        {"an index",
         t + "int main(void) { struct S arr[] = { src[1], src[0], src[1] };\n"
             "  _Static_assert(sizeof(arr) == 3 * sizeof(struct S), \"3\"); return arr[0].b; }\n", 0},
        {"an index that is no constant",
         t + "int main(void) { int i = 1, j = 0; struct S arr[] = { src[i], src[j] };\n"
             "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[0].b; }\n", 0},
        {"a dereference",
         t + "int main(void) { struct S *p = &one; struct S arr[] = { *p, one, *p };\n"
             "  _Static_assert(sizeof(arr) == 3 * sizeof(struct S), \"3\"); return arr[2].b; }\n", 0},
        {"pointer arithmetic under a dereference",
         t + "int main(void) { int z = 0; struct S arr[] = { *(src + 1), *(&one + z) };\n"
             "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[0].b; }\n", 0},
        {"a conditional expression",
         t + "int main(void) { int j = 0; struct S arr[] = { (0 ? one : src[1]), (j ? one : one) };\n"
             "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[0].b; }\n", 0},
        {"a comma expression",
         t + "int main(void) { int j = 0; struct S arr[] = { (j, one), (j, src[1]) };\n"
             "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[1].b; }\n", 0},
        {"a call with no argument",
         t + "int main(void) { struct S arr[] = { mk0(), mk0() };\n"
             "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[1].b; }\n", 0},
        {"as a member, positional and designated, and in a nested list",
         t + "int main(void) { struct W w = { src[1], 5 }; struct W d = { .in = src[0], .n = 6 };\n"
             "  struct W v[] = { { src[0], 7 }, { *src, 8 } };\n"
             "  _Static_assert(sizeof(v) == 2 * sizeof(struct W), \"2\"); return w.n + d.n + v[1].n; }\n", 0},
        {"in a compound literal",
         t + "int main(void) { struct W w = (struct W){ src[1], 5 }; return w.n; }\n", 0},
    });
}

// WHERE the element lands, read off the lowered values: the structure written as an
// index takes the WHOLE first member, and the value after it belongs to the next one.
// (Placed by its stamps it was elided into `in.a`, and the 5 into `in.b`.)
TEST(BraceElementAsWritten, TheAnalyzersAnswerPlacesAnIndexedStructureWhole) {
    bool inspected = false;
    Lowered const l = lowerC(
        std::string{kTypes} + "void f(void) { struct W w = { src[1], 5 }; }\n",
        [&](CstToHirResult const& res) {
            HirNodeId const init = firstLocalInit(res.hir);
            ASSERT_TRUE(init.valid());
            ASSERT_EQ(res.hir.kind(init), HirKind::ConstructAggregate);
            auto const members = res.hir.children(init);
            ASSERT_EQ(members.size(), 2u) << "`in` and `n`";
            EXPECT_NE(res.hir.kind(members[0]), HirKind::ConstructAggregate)
                << "`in` is the indexed structure itself, not a list elided into its members";
            EXPECT_EQ(literalValue(res, members[1]), std::optional<std::int64_t>{5})
                << "the value after the structure belongs to `n`";
            inspected = true;
        });
    EXPECT_TRUE(l.lowered) << l.diagnostics;
    EXPECT_EQ(l.hirErrors, 0u) << l.diagnostics;
    EXPECT_TRUE(inspected);
}

// THE CONTROL OF (2): the forms whose node the analyzer STAMPS were always placed
// whole, and are placed whole with no fact to read — a name, a member access, a call
// with an argument, a compound literal, `*&` of a name.
TEST(BraceElementAsWritten, ANameAMemberACallAndACompoundLiteralAreStillPlacedWhole) {
    std::string const t = kTypes;
    expectLowersCleanly({
        {"a name", t + "int main(void) { struct S arr[] = { one, one };\n"
                       "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[1].b; }\n", 0},
        {"a member access, `.` and `->`",
         t + "int main(void) { struct W w = { { 1, 2 }, 5 }; struct W *q = &w; struct S arr[] = { w.in, q->in };\n"
             "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[1].b; }\n", 0},
        {"a call with an argument",
         t + "int main(void) { struct S arr[] = { mk(1), mk(3) };\n"
             "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[1].b; }\n", 0},
        {"a compound literal",
         t + "int main(void) { struct S arr[] = { (struct S){ 1, 2 }, (struct S){ 3, 4 } };\n"
             "  _Static_assert(sizeof(arr) == 2 * sizeof(struct S), \"2\"); return arr[1].b; }\n", 0},
        {"`*&` of a name",
         t + "int main(void) { struct W w = { *&one, 2 }; return w.n; }\n", 0},
    });
}

// WHY THE PLACEMENT IS DECIDED BEFORE ANYTHING IS LOWERED: an element in excess is
// dropped UNEVALUATED (C 6.7.9p2; gcc, MinGW gcc and clang build it with a warning and
// the call never runs). The lowering therefore cannot learn a value's type by lowering
// it first — the answer has to come from the analyzer.
TEST(BraceElementAsWritten, AnExcessElementAfterAWholeStructureIsDroppedUnevaluated) {
    bool inspected = false;
    Lowered const l = lowerC(
        std::string{kTypes} + "void f(void) { struct S only[1] = { src[1], mk(7) }; }\n",
        [&](CstToHirResult const& res) {
            EXPECT_EQ(countKind(res.hir, HirKind::Call), 0u)
                << "the call in the excess element must not be lowered";
            inspected = true;
        });
    EXPECT_TRUE(l.lowered) << l.diagnostics;
    EXPECT_EQ(l.hirErrors, 0u) << l.diagnostics;
    EXPECT_EQ(l.hirExcess, 1u) << "reported once\n" << l.diagnostics;
    EXPECT_TRUE(inspected);
}
