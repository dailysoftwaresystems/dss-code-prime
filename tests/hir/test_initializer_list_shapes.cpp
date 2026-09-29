// ===========================================================================
// P68 round 9 (lane `cs`) — THREE INITIALIZER-LIST RULES THE ARRAY COMPLETION AND
// THE BRACE WALKER GOT WRONG, found while fixing a compound literal's omitted bound
// (D-C-INCOMPATIBLE-POINTER-CONVERSION-REFUSED-WHERE-EVERY-REFERENCE-WARNS' continuation).
//
//   (1) C 6.7.9p22 — an array of unknown size is as long as the LARGEST index its
//       list initializes, plus one. The completion COUNTED the elements, so every
//       designated shape (`int a[] = {[3] = 42}`, `{1, [5] = 2, 39}`, `{[4] = 1, [1]
//       = 41}`, `struct P ps[] = {[2].y = 42}`, a file-scope one, an enumeration
//       constant index, the compound-literal twins) came out short and the walker
//       refused it: "init element targets position out of aggregate range".
//   (2) C 6.7.9p14 — a character array may be initialized by a string literal
//       "optionally enclosed in braces". `char s[] = { "abc" }` was taken as a
//       `char` ELEMENT: refused (H_VerifierFailure) at the round's base, then BUILT
//       with the pointer's low byte once row 1 admitted pointer→integer with a
//       warning — a regression of this round, caught by its own measurement.
//   (3) C 6.7.9p2 — EXCESS positional elements are a constraint violation that
//       gcc, mingw and clang (-std=c2x) build with a warning, dropping the element
//       UNEVALUATED; the walker refused them with the same internal message.
//
// ✔REFERENCE VOTES, 2026-09-23, each case its own translation unit, each reference
// probed SEPARATELY, every build RUN (the lane's `.temp/probe/r8`, `r8b`, `r8d`,
// `r8e`, `r8f`, `r8g`): gcc 13.3.0 and clang 18.1.3 at `-std=c17 -pedantic-errors`
// and `-std=c2x`, mingw-w64 13.2.0, MSVC 19.51 at `/std:c17` and `/std:clatest`.
// (1) and (2): all four build and run every accepting case to 42. (3): gcc, mingw
// and clang at c2x build and run 42 (a call in the excess element never runs),
// MSVC refuses (C2078); `char s[] = {"a", "b"}` is built by clang alone.
//
//   (4) C 6.7.9p17-p20 — the CURRENT OBJECT (the placement cursor both tiers ask,
//       `analysis/semantic/initializer_cursor.hpp`): brace elision, the continuation
//       after a deep designator, a union taking one member, designators through
//       union and anonymous members. Its own section below carries its measurements.
//
//   (5) WHERE a value lands after an unnamed bit-field, read off the lowered values.
//   (6) The placement cursor's index space — an index designator is never narrowed.
//   (7) An array past that index space is refused before any slot or zero is built.
//
// RED-ON-DISABLE: the completion counting elements again → the designated sizes;
// the braced-string arm (`openBraceLevel`) removed → `{ "abc" }` warns and holds the
// wrong bytes; the excess arm refusing again → the excess cases fail to lower; the
// cursor's elision off → every (4) elided shape fails to lower or mis-sizes; the
// lowering's unnamed-bit-field answer off → (5); an index narrowed to 32 bits again,
// in either tier or in the cursor → (6).
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

constexpr DiagnosticCode kExcess = DiagnosticCode::S_ExcessInitializerElements;

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
    std::size_t semanticWarnings = 0;
    bool        lowered = false;
    std::size_t hirErrors = 0;
    std::size_t hirWarnings = 0;
    std::size_t hirExcess = 0;
    std::string diagnostics;   // every diagnostic both tiers reported, for the failure message
};

void appendDiagnostics(std::string& out, DiagnosticReporter const& r) {
    for (auto const& d : r.all())
        out += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
}

// The shipped `x86_64` target, OWNED for the whole process: `analyze` takes it
// non-owning and the model republishes it for the lowering. The fixture analyzes
// with what the CLI gives an ELF x86_64 build — the target, its aggregate layout
// and its va_list strategy — because without the layout a `sizeof` does not fold,
// and a fixture must not be able to fail on a construct the product compiles.
[[nodiscard]] TargetSchema const* fixtureTarget() {
    static std::shared_ptr<TargetSchema const> const kTarget = [] {
        auto t = TargetSchema::loadShipped("x86_64");
        return t.has_value() ? *t : nullptr;
    }();
    return kTarget.get();
}

// Handed the lowered module while its model — whose lattice its types live in — is alive.
using Inspect = std::function<void(SemanticModel const&, CstToHirResult const&)>;

// c source → semantic model → HIR. A `_Static_assert` in the source pins a SIZE at
// the semantic tier; the lowering pins what the walker makes of the list, and
// `inspect` (when given) reads WHERE it put each value.
[[nodiscard]] Lowered lowerC(std::string src, Inspect const& inspect = {}) {
    auto const loaded = dss::test_support::shippedSchemaOrThrow("c");
    UnitBuilder builder{loaded, DiagnosticBudget::libraryDefault()};
    builder.addSystemDir(dss::test::configRoot() / "shippedLibs");   // `#include <stddef.h>`
    builder.setActiveFormat(ObjectFormatKind::Elf);
    builder.addInMemory(std::move(src), "<mem>");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    Lowered out;
    // A malformed fixture (an unresolved `#include`, a parse error) is counted as an
    // error and named, never left to surface as a phantom semantic miss.
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
    out.semanticWarnings = countSeverity(model.diagnostics(), DiagnosticSeverity::Warning);
    appendDiagnostics(out.diagnostics, model.diagnostics());
    if (out.semanticErrors != 0) return out;
    DiagnosticReporter r;
    auto res = lowerToHir(model, r);
    out.lowered = res->ok;
    out.hirErrors = countSeverity(r, DiagnosticSeverity::Error);
    out.hirWarnings = countSeverity(r, DiagnosticSeverity::Warning);
    out.hirExcess = countCode(r, kExcess);
    appendDiagnostics(out.diagnostics, r);
    if (inspect && res != nullptr && res->ok) inspect(model, *res);
    return out;
}

// The value of a lowered integer LITERAL (its literal-pool entry), or nullopt for any
// other node — a zero-filled slot the lowering synthesized reads as whatever it is.
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

struct Case {
    char const* what;
    char const* src;
    std::size_t excessWarnings;   // S_ExcessInitializerElements expected from the walker
};

void expectLowersCleanly(std::initializer_list<Case> cases) {
    for (Case const& c : cases) {
        Lowered const l = lowerC(c.src);
        EXPECT_EQ(l.semanticErrors, 0u) << c.what << "\n" << c.src << l.diagnostics;
        EXPECT_EQ(l.semanticWarnings, 0u) << c.what << "\n" << c.src << l.diagnostics;
        EXPECT_TRUE(l.lowered) << c.what << "\n" << c.src << l.diagnostics;
        EXPECT_EQ(l.hirErrors, 0u) << c.what << "\n" << c.src << l.diagnostics;
        EXPECT_EQ(l.hirExcess, c.excessWarnings) << c.what << "\n" << c.src << l.diagnostics;
        EXPECT_EQ(l.hirWarnings, c.excessWarnings)
            << c.what << " — no warning but the excess one\n" << c.src << l.diagnostics;
    }
}

}  // namespace

// ── (1) an unknown size is the largest index + 1 ─────────────────────────────
TEST(InitializerListShapes, AnUnknownSizeIsTheLargestInitializedIndexPlusOne) {
    expectLowersCleanly({
        {"`[3] = 42`",
         "int a[] = { [3] = 42 };\n_Static_assert(sizeof(a) == 4 * sizeof(int), \"4\");\n"
         "int main(void) { return a[3]; }\n", 0},
        {"`1, [5] = 2, 39` (positional after a designator counts on)",
         "int main(void) { int a[] = { 1, [5] = 2, 39 };\n"
         "  _Static_assert(sizeof(a) == 7 * sizeof(int), \"7\"); return a[6]; }\n", 0},
        {"`[4] = 1, [1] = 41` (a later, smaller index does not shrink it)",
         "int main(void) { int a[] = { [4] = 1, [1] = 41 };\n"
         "  _Static_assert(sizeof(a) == 5 * sizeof(int), \"5\"); return a[1]; }\n", 0},
        {"`[2].y = 42` (the first designator step picks the element)",
         "struct P { int x, y; };\n"
         "int main(void) { struct P ps[] = { [2].y = 42 };\n"
         "  _Static_assert(sizeof(ps) == 3 * sizeof(struct P), \"3\"); return ps[2].y; }\n", 0},
        {"an enumeration-constant index",
         "enum { K = 2 };\n"
         "int main(void) { int a[] = { [K + 1] = 42 };\n"
         "  _Static_assert(sizeof(a) == 4 * sizeof(int), \"4\"); return a[3]; }\n", 0},
        {"a character array by designators",
         "int main(void) { char s[] = { [2] = 'x' };\n"
         "  _Static_assert(sizeof(s) == 3, \"3\"); return s[2]; }\n", 0},
        {"a compound literal `(int[]){ [3] = 42 }`",
         "int main(void) { int *p = (int[]){ [3] = 42 };\n"
         "  _Static_assert(sizeof((int[]){ [3] = 1 }) == 4 * sizeof(int), \"4\"); return p[3]; }\n", 0},
        {"a compound literal `(int[]){ 40, 2 }`",
         "int main(void) { int *p = (int[]){ 40, 2 };\n"
         "  _Static_assert(sizeof((int[]){ 40, 2 }) == 2 * sizeof(int), \"2\"); return p[0] + p[1]; }\n", 0},
        {"a compound literal `(int[][2]){ {1,2}, {3,4} }`",
         "int main(void) { int (*r)[2] = (int[][2]){ { 1, 2 }, { 3, 36 } };\n"
         "  _Static_assert(sizeof((int[][2]){ { 1, 2 }, { 3, 4 } }) == 4 * sizeof(int), \"4\");\n"
         "  return r[1][0] + r[1][1] + 3; }\n", 0},
    });
}

// ── (2) a string literal optionally enclosed in braces ───────────────────────
TEST(InitializerListShapes, ABracedStringInitializesACharacterArray) {
    expectLowersCleanly({
        {"`char s[] = { \"abc\" }`",
         "int main(void) { char s[] = { \"abc\" };\n"
         "  _Static_assert(sizeof(s) == 4, \"4\"); return s[2] == 'c' && s[3] == 0 ? 42 : 1; }\n", 0},
        {"a sized one",
         "int main(void) { char s[6] = { \"abc\" }; return s[2] == 'c' && s[5] == 0 ? 42 : 1; }\n", 0},
        {"a compound literal `(char[]){ \"abc\" }`",
         "int main(void) { _Static_assert(sizeof((char[]){ \"abc\" }) == 4, \"4\"); return 42; }\n", 0},
        // THE CONTROLS: characters are elements; a pointer array takes the string as
        // an element.
        {"`char s[] = { 'a', 'b' }` (elements)",
         "int main(void) { char s[] = { 'a', 'b' };\n"
         "  _Static_assert(sizeof(s) == 2, \"2\"); return s[1] == 'b' ? 42 : 1; }\n", 0},
        {"`char *p[] = { \"abc\" }` (a pointer element)",
         "int main(void) { char *p[] = { \"abc\" };\n"
         "  _Static_assert(sizeof(p) == sizeof(char *), \"1\"); return p[0][2] == 'c' ? 42 : 1; }\n", 0},
        {"`wchar_t w[] = { L\"ab\" }` (the wide twin)",
         "#include <stddef.h>\n"
         "int main(void) { wchar_t w[] = { L\"ab\" };\n"
         "  _Static_assert(sizeof(w) == 3 * sizeof(wchar_t), \"3\"); return w[1] == L'b' ? 42 : 1; }\n", 0},
    });
}

// ── (3) excess positional elements: a warning, and dropped ──────────────────
TEST(InitializerListShapes, ExcessPositionalElementsWarnOnceAndAreDropped) {
    expectLowersCleanly({
        {"`int a[2] = {1, 2, 3}`", "int main(void) { int a[2] = { 1, 2, 3 }; return a[0] + a[1] + 39; }\n", 1},
        {"`struct P p = {40, 2, 7}`",
         "struct P { int x, y; };\nint main(void) { struct P p = { 40, 2, 7 }; return p.x + p.y; }\n", 1},
        {"a nested row", "int main(void) { int a[1][2] = { { 1, 41, 5 } }; return a[0][0] + a[0][1]; }\n", 1},
        {"a file-scope array", "static int g[2] = { 40, 2, 9 };\nint main(void) { return g[0] + g[1]; }\n", 1},
        {"two excess elements: ONE warning",
         "int main(void) { int a[1] = { 42, 1, 2 }; return a[0]; }\n", 1},
        {"`char s[] = { \"a\", \"b\" }` (clang's reading: the string, then excess)",
         "int main(void) { char s[] = { \"a\", \"b\" };\n"
         "  _Static_assert(sizeof(s) == 2, \"2\"); return s[0] == 'a' && s[1] == 0 ? 42 : 1; }\n", 1},
    });
}

// A DESIGNATED index past the end is not an excess element: every reference that
// accepts excess elements refuses it, and so does DSS — at any depth. ★ The nested
// form `{[0][5] = 1}` was BUILT SILENTLY before the placement cursor (the slot
// writer's "defense in depth" no-op dropped the write); gcc, clang and mingw-w64
// refuse it, and MSVC 19.51's silent acceptance drops or misplaces the value
// (✔MEASURED `.temp/probe/r9c` c33, `r9d`), which is no semantics to follow.
TEST(InitializerListShapes, ADesignatedIndexPastTheEndStaysRefused) {
    Lowered const l = lowerC("int main(void) { int a[2] = { [5] = 1 }; return a[0]; }\n");
    EXPECT_EQ(l.semanticErrors, 0u);
    EXPECT_GT(l.hirErrors, 0u) << "`[5]` in a two-element array must stay refused";
    EXPECT_EQ(l.hirExcess, 0u);
    Lowered const deep =
        lowerC("int main(void) { int a[2][2] = { [0][5] = 1 }; return a[0][0]; }\n");
    EXPECT_EQ(deep.semanticErrors, 0u);
    EXPECT_GT(deep.hirErrors, 0u) << "`[0][5]` in a two-element row must be refused\n"
                                  << deep.diagnostics;
    EXPECT_EQ(deep.hirExcess, 0u);
}

// ── (4) the CURRENT OBJECT: brace elision and the continuation after a designator ──
// C 6.7.9p17-p20. ✔MEASURED 2026-09-23 on gcc 13.3.0 and clang 18.1.3 (c17-pedantic
// and c2x), mingw-w64 13.2.0 and MSVC 19.51, every program RUN to its C placement
// (`.temp/probe/r9`, `r9b`, `r9c`, `r9f`). Before the placement cursor DSS refused
// every elided shape (H_VerifierFailure: the value met the aggregate slot as a
// scalar), resumed after a deep designator at the next TOP-LEVEL slot (`{.i.a = 30,
// 12}` stored 12 in `o.c`), refused a union list of more than one element or with a
// chained designator, and DROPPED a designated write through a union member (`{.u.i =
// 40, 2}` ran 2). The sizes are pinned here; the values run in
// `examples/c/brace_elision_current_object`.
TEST(InitializerListShapes, BraceElisionFillsEachAggregateElementInOrder) {
    expectLowersCleanly({
        {"`int a[2][2] = {1, 2, 3, 40}`",
         "int main(void) { int a[2][2] = { 1, 2, 3, 40 }; return a[1][0] + a[1][1] - 1; }\n", 0},
        {"`int a[][2] = {1, 2, 3}` is TWO rows",
         "int main(void) { int a[][2] = { 1, 2, 3 };\n"
         "  _Static_assert(sizeof(a) == 4 * sizeof(int), \"4\"); return a[1][0]; }\n", 0},
        {"`struct P ps[] = {1, 2, 3, 39}` is two structures",
         "struct P { int x, y; };\n"
         "int main(void) { struct P ps[] = { 1, 2, 3, 39 };\n"
         "  _Static_assert(sizeof(ps) == 2 * sizeof(struct P), \"2\"); return ps[1].y; }\n", 0},
        {"three levels", "int main(void) { int a[2][2][2] = { 1, 2, 3, 4, 5, 6, 7, 8 }; return a[1][1][1]; }\n", 0},
        {"a structure VALUE is not elided",
         "struct P { int x, y; };\n"
         "int main(void) { struct P q = { 40, 2 }; struct P ps[] = { q, 1, 1 };\n"
         "  _Static_assert(sizeof(ps) == 2 * sizeof(struct P), \"2\"); return ps[0].x; }\n", 0},
        {"a string initializes its character array whole",
         "struct V { char s[4]; int x; };\n"
         "int main(void) { struct V v[] = { \"ab\", 40, \"cd\", 1 };\n"
         "  _Static_assert(sizeof(v) == 2 * sizeof(struct V), \"2\"); return v[1].x; }\n", 0},
        {"character rows: strings and elided characters",
         "int main(void) { char s[][3] = { \"ab\", 'c', 'd', 0 };\n"
         "  _Static_assert(sizeof(s) == 6, \"6\"); return s[1][1]; }\n", 0},
        {"an unnamed bit-field is skipped",
         "struct B { int a; unsigned : 3; int b; };\n"
         "int main(void) { struct B bs[] = { 40, 2, 7, 8 };\n"
         "  _Static_assert(sizeof(bs) == 2 * sizeof(struct B), \"2\"); return bs[1].b; }\n", 0},
        {"a braced row after elided ones",
         "int main(void) { int e[3][2] = { 1, 2, { 3, 4 }, 32 }; return e[2][0]; }\n", 0},
        {"braces around an elided scalar",
         "int main(void) { int a[2][2] = { 1, { 2 }, 3, 36 }; return a[0][1]; }\n", 0},
        {"file scope", "static int g[][2] = { 1, 2, 3, 36 };\n"
                       "_Static_assert(sizeof(g) == 4 * sizeof(int), \"4\");\n"
                       "int main(void) { return g[1][1]; }\n", 0},
        {"a compound literal",
         "int main(void) { int (*p)[2] = (int[][2]){ 1, 2, 3, 36 };\n"
         "  _Static_assert(sizeof((int[][2]){ 1, 2, 3 }) == 4 * sizeof(int), \"4\"); return p[1][1]; }\n", 0},
        {"excess after elision", "int main(void) { int a[1][2] = { 1, 2, 3 }; return a[0][1]; }\n", 1},
    });
}

TEST(InitializerListShapes, ADesignatorsContinuationStaysInsideItsAggregate) {
    expectLowersCleanly({
        {"`int a[][2] = {[1][0] = 1, 41}` is two rows",
         "int main(void) { int a[][2] = { [1][0] = 1, 41 };\n"
         "  _Static_assert(sizeof(a) == 4 * sizeof(int), \"4\"); return a[1][1]; }\n", 0},
        {"`{.i.a = 30, 12}` continues in `i`",
         "struct I { int a, b; }; struct O { struct I i; int c; };\n"
         "int main(void) { struct O o = { .i.a = 30, 12 }; return o.i.b; }\n", 0},
        {"`{.i.b = 30, 12}` leaves `i`",
         "struct I { int a, b; }; struct O { struct I i; int c; };\n"
         "int main(void) { struct O o = { .i.b = 30, 12 }; return o.c; }\n", 0},
        {"`{.a[1] = 40, 2}` leaves the array",
         "struct T { int a[2]; int b; };\n"
         "int main(void) { struct T t = { .a[1] = 40, 2 }; return t.b; }\n", 0},
        {"a designator at the top after elision",
         "struct P { int x, y; };\n"
         "int main(void) { struct P ps[2] = { 1, [1] = { 40, 2 } }; return ps[1].y; }\n", 0},
    });
}

TEST(InitializerListShapes, AUnionTakesOneMemberAndDesignatorsReachThroughIt) {
    expectLowersCleanly({
        {"a union's first member, elided",
         "union U { struct { int a, b; } s; int i; };\n"
         "int main(void) { union U u = { 40, 2 }; return u.s.b; }\n", 0},
        {"`{.u.i = 40, 2}`: through a union, then the next member",
         "struct S { union { int i; char c; } u; int k; };\n"
         "int main(void) { struct S s = { .u.i = 40, 2 }; return s.k; }\n", 0},
        {"elided into a union member, then the next member",
         "union U { int i; char c; }; struct S { union U u; int k; };\n"
         "int main(void) { struct S s = { 2, 40 }; return s.k; }\n", 0},
        {"a chained designator into a union member",
         "struct Inner { int v; }; union U { struct Inner a; int i; };\n"
         "int main(void) { union U u = { .a.v = 42 }; return u.a.v; }\n", 0},
        {"the last designator of a union list wins",
         "union V { int i; float f; };\n"
         "int main(void) { union V v = { .f = 1.0f, .i = 42 }; return v.i; }\n", 0},
        {"`{.s.a = 40, 2}` continues inside the union's member",
         "union U { struct { int a, b; } s; int i; };\n"
         "int main(void) { union U u = { .s.a = 40, 2 }; return u.s.b; }\n", 0},
        {"a union list's excess element",
         "union U { int i; char c; };\n"
         "int main(void) { union U u = { 42, 7 }; return u.i; }\n", 1},
    });
    Lowered const index =
        lowerC("union U { int i; char c; };\nint main(void) { union U u = { [0] = 1 }; return u.i; }\n");
    EXPECT_GT(index.hirErrors, 0u) << "an index designator on a union is refused by all four";
}

// C 6.7.2.1p13: a member of an ANONYMOUS structure or union member is a member of the
// containing one — a designator names it, and a positional element enters it.
TEST(InitializerListShapes, AnonymousMembersAreMembers) {
    expectLowersCleanly({
        {"a designator naming a member of an anonymous union",
         "struct T { int x; union { int a; float b; }; int y; };\n"
         "int main(void) { struct T t = { .a = 40, 2 }; return t.y; }\n", 0},
        {"a positional element enters an anonymous structure",
         "struct T { int x; struct { int a, b; }; int y; };\n"
         "int main(void) { struct T t = { 1, 2, 3, 36 }; return t.y; }\n", 0},
        {"a designator into an anonymous structure continues inside it",
         "struct T { int x; struct { int a, b; }; int y; };\n"
         "int main(void) { struct T t = { .a = 30, 10, 2 }; return t.y; }\n", 0},
    });
}

// ── (5) WHERE a value lands after an UNNAMED bit-field (C 6.7.9p9) ────────────
// This tier answers the cursor's "is member i an unnamed bit-field" from its own
// reading of the composite's scope (`unnamedBitFieldMask`). The cursor's unit test
// proves it skips what it is told to skip, and "an unnamed bit-field is skipped" above
// proves the list LOWERS; neither reads where the values went. This one does: with
// the lowering's answer off, `{40, 2, 7, 8}` puts the 2 on the bit-field and every
// later value one member early — which the fold-3 red-on-disable saw only through the
// runnable example (P68 round 9, lane `cs`).
TEST(InitializerListShapes, AValueAfterAnUnnamedBitFieldLandsInTheNextNamedMember) {
    // `members[i]` of a lowered structure aggregate, as the literal value it holds.
    auto memberValue = [](CstToHirResult const& res, HirNodeId agg, std::size_t i) {
        auto const kids = res.hir.children(agg);
        return i < kids.size() ? literalValue(res, kids[i]) : std::nullopt;
    };
    bool inspectedOne = false;
    Lowered const one = lowerC(
        "struct B { int a; unsigned : 3; int b; };\n"
        "void f(void) { struct B s = { 40, 2 }; }\n",
        [&](SemanticModel const&, CstToHirResult const& res) {
            HirNodeId const init = firstLocalInit(res.hir);
            ASSERT_TRUE(init.valid());
            ASSERT_EQ(res.hir.kind(init), HirKind::ConstructAggregate);
            ASSERT_EQ(res.hir.children(init).size(), 3u) << "a, the unnamed bit-field, b";
            EXPECT_EQ(memberValue(res, init, 0), std::optional<std::int64_t>{40});
            EXPECT_EQ(memberValue(res, init, 2), std::optional<std::int64_t>{2})
                << "the value after `a` belongs to `b`, the next NAMED member";
            EXPECT_NE(memberValue(res, init, 1), std::optional<std::int64_t>{2})
                << "the unnamed bit-field takes no initializer";
            inspectedOne = true;
        });
    EXPECT_TRUE(one.lowered) << one.diagnostics;
    EXPECT_TRUE(inspectedOne);

    bool inspectedArray = false;
    Lowered const rows = lowerC(
        "struct B { int a; unsigned : 3; int b; };\n"
        "void f(void) { struct B bs[] = { 40, 2, 7, 8 }; }\n",
        [&](SemanticModel const&, CstToHirResult const& res) {
            HirNodeId const init = firstLocalInit(res.hir);
            ASSERT_TRUE(init.valid());
            ASSERT_EQ(res.hir.kind(init), HirKind::ConstructAggregate);
            auto const elems = res.hir.children(init);
            ASSERT_EQ(elems.size(), 2u) << "four values fill TWO structures";
            EXPECT_EQ(memberValue(res, elems[0], 0), std::optional<std::int64_t>{40});
            EXPECT_EQ(memberValue(res, elems[0], 2), std::optional<std::int64_t>{2});
            EXPECT_EQ(memberValue(res, elems[1], 0), std::optional<std::int64_t>{7});
            EXPECT_EQ(memberValue(res, elems[1], 2), std::optional<std::int64_t>{8});
            inspectedArray = true;
        });
    EXPECT_TRUE(rows.lowered) << rows.diagnostics;
    EXPECT_TRUE(inspectedArray);
}

// ── (6) the placement cursor's index space: an index is never narrowed ────────
// ★ P68 round 9 (lane `cs`). Both tiers narrowed an index designator to 32 bits after
// checking only its sign, so an index of 2^32 or more WRAPPED to a small one that was
// in range — `int a[2] = {[4294967296] = 7}` put the 7 in a[0] and built SILENTLY, and
// `int a[] = {[4294967296] = 7}` was one element long. ✔MEASURED 2026-09-23 (the lane's
// `.temp/probe/dx`, each reference separately, every build RUN): gcc 13.3.0 and clang
// 18.1.3 at `-std=c17 -pedantic-errors` and `-std=c2x`, and mingw-w64 13.2.0 at both,
// REFUSE every bounded shape below ("array index in initializer exceeds array bounds");
// MSVC 19.51 refuses the top-level ones (C2078) and builds the nested one while
// DROPPING the value (it ran 0) — no semantics to follow. The unknown size: gcc builds
// it and the run faults on its 16 GiB stack array, clang builds and runs it. DSS
// refuses it as the implementation limit it is — the lowering builds one slot per
// element (about 195 bytes each, ✔MEASURED `.temp/probe/la`), so four billion of them
// cannot be built — rather than sizing it wrong.
TEST(InitializerListShapes, AnIndexDesignatorIsNeverNarrowed) {
    struct Refused {
        char const* what;
        char const* src;
        bool        semantic;   // refused by the semantic tier's sizing, else by the lowering
    };
    for (Refused const& c : std::initializer_list<Refused>{
             {"`[4294967296]` into `int a[2]` (wrapped to 0)",
              "int main(void) { int a[2] = { [4294967296] = 7 }; return a[0]; }\n", false},
             {"`[4294967297]` into `int a[2]` (wrapped to 1)",
              "int main(void) { int a[2] = { [4294967297] = 7 }; return a[1]; }\n", false},
             {"file scope", "static int g[2] = { [4294967296] = 7 };\nint main(void) { return g[0]; }\n",
              false},
             {"a NESTED index `[0][4294967296]`",
              "int main(void) { int a[2][2] = { [0][4294967296] = 7 }; return a[0][0]; }\n", false},
             {"an index written as `(long long)1 << 32`",
              "int main(void) { int a[2] = { [(long long)1 << 32] = 7 }; return a[0]; }\n", false},
             {"an unknown size an index designator sizes past the limit",
              "int main(void) { int a[] = { [4294967296] = 7 }; return (int)sizeof a; }\n", true},
         }) {
        Lowered const l = lowerC(c.src);
        if (c.semantic) {
            EXPECT_GT(l.semanticErrors, 0u) << c.what << "\n" << c.src << l.diagnostics;
            EXPECT_NE(l.diagnostics.find("S_ArrayLengthOutOfRange"), std::string::npos)
                << c.what << ": the limit is named as an array-length refusal\n" << l.diagnostics;
        } else {
            EXPECT_EQ(l.semanticErrors, 0u) << c.what << "\n" << c.src << l.diagnostics;
            EXPECT_GT(l.hirErrors, 0u) << c.what << ": must be REFUSED, never placed at the "
                                          "wrapped index\n" << c.src << l.diagnostics;
        }
        EXPECT_EQ(l.hirExcess, 0u) << c.what << "\n" << l.diagnostics;
    }
    // THE CONTROLS: the same shapes in range still lower, and the value lands where the
    // index says (not where a wrap would put it).
    expectLowersCleanly({
        {"`[1]` into `int a[2]`", "int main(void) { int a[2] = { [1] = 7 }; return a[1] + 35; }\n", 0},
        {"`[0][1]` into `int a[2][2]`",
         "int main(void) { int a[2][2] = { [0][1] = 7 }; return a[0][1] + 35; }\n", 0},
        {"an unknown size sized by `[3]`",
         "int main(void) { int a[] = { [3] = 7 };\n"
         "  _Static_assert(sizeof(a) == 4 * sizeof(int), \"4\"); return a[3] + 35; }\n", 0},
    });
}

// ── (7) an array past the index space is refused BEFORE anything is built ────
// The lowering builds one slot per element (and one zero per unwritten element), so
// an object or a member longer than `initializer_cursor::kMaxLength` must be refused
// before either is built: the level's open asks the cursor about its object, the
// cursor refuses to enter a longer member, and the zero-filler refuses to zero one.
// ★ P68 round 9 (lane `cs`): a declared length was narrowed to 32 bits at the level's
// open, so exactly 2^32 elements was refused as "zero slots" and 2^32 + 1 went on to
// build four billion slots. ⚠ Every case here would exhaust memory if its guard were
// missing; the red-on-disable runs them under an address-space cap.
TEST(InitializerListShapes, AnArrayPastTheIndexSpaceIsRefusedBeforeAnythingIsBuilt) {
    struct Refused {
        char const* src;
        char const* says;   // the refusal that names the length
    };
    for (Refused const& c : std::initializer_list<Refused>{
             {"static char big[4294967297] = { 1 };\nint main(void) { return big[0] + 41; }\n",
              "initializes an array of 4294967297 elements"},
             {"static char big[4294967296] = { 1 };\nint main(void) { return big[0] + 41; }\n",
              "initializes an array of 4294967296 elements"},
             {"static char big[1099511627776] = { 1 };\nint main(void) { return big[0] + 41; }\n",
              "initializes an array of 1099511627776 elements"},
             {"struct H { char big[4294967297]; int k; };\n"
              "static struct H h = { 1 };\nint main(void) { return h.k + 42; }\n",
              "placed inside an array of 4294967297 elements"},
             {"struct H { char big[4294967297]; int k; };\n"
              "static struct H h = { 1 };\nint main(void) { return h.k + 42; }\n",
              "zero-filling an array of 4294967297 elements"},
         }) {
        Lowered const l = lowerC(c.src);
        EXPECT_EQ(l.semanticErrors, 0u) << c.src << l.diagnostics;
        EXPECT_GT(l.hirErrors, 0u) << c.src << l.diagnostics;
        EXPECT_NE(l.diagnostics.find(c.says), std::string::npos)
            << "expected the refusal naming the length: `" << c.says << "`\n" << c.src << l.diagnostics;
        EXPECT_EQ(l.diagnostics.find("internal invariant"), std::string::npos)
            << "refused by the stated limit, not by a broken invariant\n" << l.diagnostics;
    }
}
