// ===========================================================================
// P69 L2a (lane `lm`) — WHAT C EVALUATES IN A VARIABLE-LENGTH OPERAND OR A VARIABLY
// MODIFIED TYPE NAME IS EVALUATED, AND NOTHING ELSE IS.
// (D-C-SIZEOF-OF-A-VLA-TYPED-OPERAND-SKIPS-THE-OPERANDS-SIDE-EFFECTS.)
//
// THE PROPERTY THIS FILE OWNS: a `sizeof` whose operand has VARIABLE-LENGTH ARRAY type
// evaluates that operand (C 6.5.3.4p2) — its index with a side effect, a pointer stepped on
// the way — and still answers the size the operand's root FROZE; a cast or a compound
// literal whose type name is variably modified evaluates that type name's size expressions
// (C 6.7.6.2p5; a `typeof` of a variably modified operand, C23 6.7.2.5p4). An operand whose
// type is NOT a variable-length array — a fixed array's row, a variable-length array's fixed
// row or element, a comma that decays, a nested `sizeof`, any `alignof`, a `sizeof` of a
// pointer type — is NOT evaluated.
//
// ✔MEASURED at 937a3b15 (macos-arm64-debug, x86_64:pe64-x86_64-windows-exec, images run
// natively, baseline and release alike): every evaluated form below left its side effect
// UNDONE — `int v[n][m]; sizeof v[k++]` left k == 0 and the program ran 1 where gcc 13.3.0
// and clang 18.1.3 (-std=gnu2x and -std=c2x -pedantic-errors) run 41; the call, assignment,
// three-dimensional, fixed-outer, pointer, `sizeof(typeof(v[k++]))`, cast and compound
// literal forms alike. Every unevaluated form ran as the references do. The run-time half is
// the example `a_variable_length_operand_is_evaluated`.
//
// THE READING: the lowered HIR. An evaluated form is a SeqExpr whose statements run the
// operand (or the type name's size expressions) and whose value is the SizeOf (or the cast's
// value, or the literal's initializer); the variable the side effect writes is referenced
// inside those statements. An unevaluated form references that variable NOWHERE.
//
// ── RED-ON-DISABLE ─────────────────────────────────────────────────────────
//   * the sizeof's operand not handed to the driver → every `sizeof` evaluated case: no
//     SeqExpr around the SizeOf, the variable never referenced;
//   * a variably modified type name's size expressions not collected → the cast, typeof-in-a-
//     cast and compound literal cases.
// The negative cases hold either way (they are the other half of the property).
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
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

// The shipped `x86_64` target, OWNED for the whole process: the fixture analyzes with what
// the CLI gives an ELF x86_64 build, so an element's size is known.
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

// How many `Ref`s to `sym` the subtree of `root` holds (an explicit work stack).
[[nodiscard]] std::size_t refsUnder(Hir const& h, HirNodeId root, std::uint32_t sym) {
    std::size_t n = 0;
    std::vector<HirNodeId> stack{root};
    while (!stack.empty()) {
        HirNodeId const id = stack.back();
        stack.pop_back();
        if (!id.valid()) continue;
        if (h.kind(id) == HirKind::Ref && h.payload(id) == sym) ++n;
        for (HirNodeId const c : h.children(id)) stack.push_back(c);
    }
    return n;
}

// How many `Ref`s to `sym` the whole module holds.
[[nodiscard]] std::size_t refsInModule(Hir const& h, std::uint32_t sym) {
    std::size_t n = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i) {
        HirNodeId const id{i, tag};
        if (h.kind(id) == HirKind::Ref && h.payload(id) == sym) ++n;
    }
    return n;
}

// Every SeqExpr whose value is a node of `valueKind` — the evaluations this file pins.
[[nodiscard]] std::vector<HirNodeId> sequencesYielding(Hir const& h, HirKind valueKind) {
    std::vector<HirNodeId> out;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i) {
        HirNodeId const id{i, tag};
        if (h.kind(id) != HirKind::SeqExpr) continue;
        HirNodeId const value = h.seqExprResult(id);
        if (value.valid() && h.kind(value) == valueKind) out.push_back(id);
    }
    return out;
}

// The statements of `seq` (what runs before its value), as one count of `Ref`s to `sym`.
[[nodiscard]] std::size_t refsInStatements(Hir const& h, HirNodeId seq, std::uint32_t sym) {
    std::size_t n = 0;
    for (HirNodeId const st : h.seqExprStmts(seq)) n += refsUnder(h, st, sym);
    return n;
}

// THE EVALUATED `sizeof`: lowered clean, exactly one SizeOf has its operand run before it,
// and that run references each of `written`.
void expectTheSizeofEvaluates(std::string const& src,
                              std::initializer_list<std::string_view> written) {
    auto l = lower(src);
    ASSERT_NE(l->result, nullptr) << src;
    EXPECT_EQ(l->reporter.errorCount(), 0u) << src << dump(l->reporter);
    Hir const& h = l->result->hir;
    std::vector<HirNodeId> const seqs = sequencesYielding(h, HirKind::SizeOf);
    ASSERT_EQ(seqs.size(), 1u)
        << "the operand of variable-length array type must run before its size is read: "
        << src;
    for (std::string_view const name : written)
        EXPECT_GE(refsInStatements(h, seqs[0], symbolNamed(l->model, name)), 1u)
            << "the operand's run must reach '" << name << "': " << src;
}

// THE UNEVALUATED operand: lowered clean, no SizeOf has anything run before it, and the
// variable the operand would write is referenced nowhere in the module.
void expectNothingIsEvaluated(std::string const& src, std::string_view written) {
    auto l = lower(src);
    ASSERT_NE(l->result, nullptr) << src;
    EXPECT_EQ(l->reporter.errorCount(), 0u) << src << dump(l->reporter);
    Hir const& h = l->result->hir;
    EXPECT_TRUE(sequencesYielding(h, HirKind::SizeOf).empty())
        << "an operand C does not evaluate must not run: " << src;
    EXPECT_EQ(refsInModule(h, symbolNamed(l->model, written)), 0u)
        << "'" << written << "' must be written by nothing: " << src;
}

}  // namespace

// ── sizeof's operand of variable-length array type: EVALUATED ──────────────────────────────

TEST(VariableLengthEvaluation, ARowWhoseIndexStepsIsEvaluated) {
    expectTheSizeofEvaluates("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)sizeof v[k++] + k; }\n",
                             {"k"});
}

// The call runs EXACTLY once: one `Ref` to `at` in the module, inside the operand's run.
TEST(VariableLengthEvaluation, ARowWhoseIndexCallsIsEvaluatedOnce) {
    std::string const src = "static int calls;\n"
                            "static int at(void) { calls++; return 0; }\n"
                            "int f(int n, int m) { int v[n][m]; return (int)sizeof v[at()]; }\n";
    expectTheSizeofEvaluates(src, {"at"});
    auto l = lower(src);
    ASSERT_NE(l->result, nullptr);
    std::uint32_t const at = symbolNamed(l->model, "at");
    EXPECT_EQ(refsInModule(l->result->hir, at), 1u) << "at() is called once, by the operand";
}

TEST(VariableLengthEvaluation, AParenthesisedRowIsEvaluated) {
    expectTheSizeofEvaluates("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)sizeof (v[k++]) + k; }\n",
                             {"k"});
}

TEST(VariableLengthEvaluation, ARowWhoseIndexAssignsIsEvaluated) {
    expectTheSizeofEvaluates("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)sizeof v[k = 1] + k; }\n",
                             {"k"});
}

TEST(VariableLengthEvaluation, ARowOfARowRunsBothIndices) {
    expectTheSizeofEvaluates("int f(int n, int m, int o) { int k = 0, j = 0; int w[n][m][o]; "
                             "return (int)sizeof w[k++][j++] + k + j; }\n",
                             {"k", "j"});
}

TEST(VariableLengthEvaluation, ARowOfAFixedOuterVlaIsEvaluated) {
    expectTheSizeofEvaluates("int f(int m) { int k = 0; int a[5][m]; "
                             "return (int)sizeof a[k++] + k; }\n",
                             {"k"});
}

TEST(VariableLengthEvaluation, APointeeReachedThroughAPostIncrementStepsThePointer) {
    for (char const* operand : {"*(p++)", "*p++"})
        expectTheSizeofEvaluates(std::string{"int f(int n, int m) { int v[n][m]; "
                                             "int (*p)[m] = v; return (int)sizeof "} +
                                     operand + " + (p == &v[1]); }\n",
                                 {"p"});
}

TEST(VariableLengthEvaluation, APointeeReachedThroughAPreIncrementStepsThePointer) {
    expectTheSizeofEvaluates("int f(int n, int m) { int v[n][m]; int (*p)[m] = v; "
                             "return (int)sizeof *++p + (p == &v[1]); }\n",
                             {"p"});
}

TEST(VariableLengthEvaluation, APointerSteppedAsASubscriptsBaseIsEvaluated) {
    expectTheSizeofEvaluates("int f(int n, int m) { int v[n][m]; int (*p)[m] = v; "
                             "return (int)sizeof p++[0] + (p == &v[1]); }\n",
                             {"p"});
}

TEST(VariableLengthEvaluation, APointersRowWhoseIndexStepsIsEvaluated) {
    expectTheSizeofEvaluates("int f(int n, int m) { int k = 0; int v[n][m]; int (*p)[m] = v; "
                             "return (int)sizeof p[k++] + k; }\n",
                             {"k"});
}

TEST(VariableLengthEvaluation, ATypeofOfARowInATypeNameIsEvaluated) {
    expectTheSizeofEvaluates("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)sizeof(typeof(v[k++])) + k; }\n",
                             {"k"});
}

// CONTROL: the TYPE-NAME form evaluates its bound by multiplying it (no SeqExpr): `k` is
// referenced, once, by the bound the size multiplies.
TEST(VariableLengthEvaluation, ATypeNamesBoundIsStillEvaluatedOnce) {
    auto l = lower("int f(void) { int k = 0; return (int)sizeof(int[k++ + 1]) + k; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    Hir const& h = l->result->hir;
    std::uint32_t const k = symbolNamed(l->model, "k");
    // `k++` desugars to a read and a write of k, and the return adds one read.
    EXPECT_GE(refsInModule(h, k), 2u) << "the bound `k++ + 1` is lowered and run";
    EXPECT_TRUE(sequencesYielding(h, HirKind::SizeOf).empty())
        << "a type name's bound is the size's own operand, not an effect before it";
}

// ── a variably modified type name in a cast or a compound literal: EVALUATED ──────────────

TEST(VariableLengthEvaluation, ACastToAVariablyModifiedTypeRunsItsBound) {
    auto l = lower("int f(void) { int k = 0; int x[4] = {0}; "
                   "(void)(int (*)[k++ + 1])x; return k; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    Hir const& h = l->result->hir;
    std::uint32_t const k = symbolNamed(l->model, "k");
    std::uint32_t const x = symbolNamed(l->model, "x");
    std::size_t evaluated = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i) {
        HirNodeId const id{i, tag};
        if (h.kind(id) != HirKind::SeqExpr) continue;
        if (refsInStatements(h, id, k) >= 1u && refsUnder(h, h.seqExprResult(id), x) >= 1u)
            ++evaluated;
    }
    EXPECT_EQ(evaluated, 1u) << "the bound runs before the cast's operand, once";
}

TEST(VariableLengthEvaluation, ACastsTypeofOfAVariablyModifiedOperandRuns) {
    auto l = lower("int f(int n, int m) { int k = 0; int v[n][m]; int x[4] = {0}; "
                   "(void)(typeof(v[k++]) *)x; return k; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    Hir const& h = l->result->hir;
    std::uint32_t const k = symbolNamed(l->model, "k");
    std::size_t evaluated = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i) {
        HirNodeId const id{i, tag};
        if (h.kind(id) == HirKind::SeqExpr && refsInStatements(h, id, k) >= 1u) ++evaluated;
    }
    EXPECT_GE(evaluated, 1u) << "the typeof's operand `v[k++]` runs before the cast";
}

TEST(VariableLengthEvaluation, ACompoundLiteralsVariablyModifiedTypeNameRuns) {
    auto l = lower("int f(void) { int k = 0; int x[4] = {0}; "
                   "(void)(int (*)[k++ + 1]){ (void *)x }; return k; }\n");
    ASSERT_NE(l->result, nullptr);
    EXPECT_EQ(l->reporter.errorCount(), 0u) << dump(l->reporter);
    Hir const& h = l->result->hir;
    std::uint32_t const k = symbolNamed(l->model, "k");
    std::size_t evaluated = 0;
    std::uint32_t const tag = h.id().v;
    for (std::uint32_t i = 1; i < h.nodeCount(); ++i) {
        HirNodeId const id{i, tag};
        if (h.kind(id) != HirKind::UnnamedObject) continue;
        auto const kids = h.children(id);
        if (kids.size() == 1u && h.kind(kids[0]) == HirKind::SeqExpr
            && refsInStatements(h, kids[0], k) >= 1u)
            ++evaluated;
    }
    EXPECT_EQ(evaluated, 1u)
        << "the literal's object is initialized by the bound's run, then its value";
}

// ── NOT a variable-length array type: NOT evaluated ───────────────────────────────────────

TEST(VariableLengthEvaluation, AFixedArraysRowIsNotEvaluated) {
    expectNothingIsEvaluated("int f(void) { int k = 0; int a[3][1]; "
                             "return (int)sizeof a[k++]; }\n",
                             "k");
}

TEST(VariableLengthEvaluation, AVlasFixedRowIsNotEvaluated) {
    expectNothingIsEvaluated("int f(int n) { int k = 0; int a[n][1]; "
                             "return (int)sizeof a[k++]; }\n",
                             "k");
}

TEST(VariableLengthEvaluation, AnElementOfAVlaIsNotEvaluated) {
    expectNothingIsEvaluated("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)sizeof v[k++][0]; }\n",
                             "k");
}

TEST(VariableLengthEvaluation, ACommaThatDecaysIsNotEvaluated) {
    expectNothingIsEvaluated("int f(int n) { int k = 0; int v[n]; "
                             "return (int)sizeof (k++, v); }\n",
                             "k");
}

TEST(VariableLengthEvaluation, ANestedSizeofIsNotEvaluated) {
    expectNothingIsEvaluated("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)sizeof(sizeof v[k++]); }\n",
                             "k");
}

TEST(VariableLengthEvaluation, AlignofOfAVariableLengthTypeNameIsNotEvaluated) {
    expectNothingIsEvaluated("int f(void) { int k = 0; return (int)_Alignof(int[k++ + 1]); }\n",
                             "k");
}

TEST(VariableLengthEvaluation, TheC23AlignofIsNotEvaluated) {
    expectNothingIsEvaluated("int f(void) { int k = 0; return (int)alignof(int[k++ + 1]); }\n",
                             "k");
}

TEST(VariableLengthEvaluation, TheGnuAlignofOfAValueIsNotEvaluated) {
    expectNothingIsEvaluated("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)__alignof__(v[k++]); }\n",
                             "k");
}

TEST(VariableLengthEvaluation, AlignofOfATypeofOfARowIsNotEvaluated) {
    expectNothingIsEvaluated("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)_Alignof(typeof(v[k++])); }\n",
                             "k");
}

TEST(VariableLengthEvaluation, AStatementExpressionThatDecaysIsNotEvaluated) {
    expectNothingIsEvaluated("int f(int n) { int k = 0; int v[n]; "
                             "return (int)sizeof ({ k++; v; }); }\n",
                             "k");
}

TEST(VariableLengthEvaluation, ASizeofOfAPointerTypeDoesNotRunItsTypeof) {
    expectNothingIsEvaluated("int f(int n, int m) { int k = 0; int v[n][m]; "
                             "return (int)sizeof(typeof(v[k++]) *); }\n",
                             "k");
}

// ── P69 L2a: THE RULE IS TOTAL — an operand that reaches its array through anything else is
//    REFUSED BY NAME, at that expression, never sized without running it ─────────────────────
//
// ✔MEASURED (references: gcc 13.3.0 and clang 18.1.3, gnu2x and c2x -pedantic-errors, each built
// and run): every form below runs its side effect and answers the row's size (41). The lowering
// does not know which declaration froze the size through these expressions (owed by cycle P69
// L2b); until then each is refused with the sentence below, positioned at the expression.
// RED-ON-DISABLE: the refusal removed → each falls to the MIR's unnamed "un-sizeable" (or, were
// the MIR ever to size it, to a silent skip of its side effect).
namespace {
void expectRefusedByName(std::string const& src) {
    auto l = lower(src);
    ASSERT_NE(l->result, nullptr) << src;
    bool named = false;
    for (auto const& d : l->reporter.all())
        if (d.code == DiagnosticCode::H_UnsupportedLoweringForKind
            && d.actual.find("a variable-length array reached through this expression")
                   != std::string::npos)
            named = true;
    EXPECT_TRUE(named) << "refused BY NAME, never sized without running the operand: " << src
                       << dump(l->reporter);
    EXPECT_TRUE(sequencesYielding(l->result->hir, HirKind::SizeOf).empty()) << src;
}
}  // namespace

TEST(VariableLengthEvaluation, AnArrayReachedThroughACommaIsRefusedByName) {
    expectRefusedByName("int f(int n, int m) { int k = 0; int v[n][m]; int (*p)[m] = v; "
                        "return (int)sizeof *(k++, p) + k; }\n");
}

TEST(VariableLengthEvaluation, AnArrayReachedThroughAConditionalIsRefusedByName) {
    expectRefusedByName("int f(int n, int m) { int c = 0; int v[n][m], w[n][m]; "
                        "int (*p)[m] = v, (*q)[m] = w; return (int)sizeof *(c++ ? p : q) + c; }\n");
}

TEST(VariableLengthEvaluation, AnArrayReachedThroughAnAssignmentIsRefusedByName) {
    expectRefusedByName("int f(int n, int m) { int v[n][m], w[n][m]; int (*p)[m] = v, (*q)[m] = w; "
                        "return (int)sizeof *(p = q) + (p == q); }\n");
}

TEST(VariableLengthEvaluation, AnArrayReachedThroughACallIsRefusedByName) {
    expectRefusedByName("static int calls;\nstatic int store[8];\n"
                        "static int (*g(void))[] { calls++; return (int (*)[])store; }\n"
                        "int f(int m) { int (*(*fp)(void))[m] = g; "
                        "return (int)sizeof *fp() + calls; }\n");
}

TEST(VariableLengthEvaluation, AnArrayReachedThroughACompoundAssignmentIsRefusedByName) {
    expectRefusedByName("int f(int n, int m) { int v[n][m]; int (*p)[m] = v; "
                        "return (int)sizeof *(p += 1) + (p == &v[1]); }\n");
}

TEST(VariableLengthEvaluation, AnArrayReachedThroughAPointerToAPointerIsRefusedByName) {
    expectRefusedByName("int f(int n, int m) { int k = 0; int v[n][m]; int (*p)[m] = v; "
                        "int (**pp)[m] = &p; return (int)sizeof (*pp)[k++] + k; }\n");
}

TEST(VariableLengthEvaluation, AnArrayReachedThroughACastIsRefusedByName) {
    expectRefusedByName("int f(int n) { int x[8] = {0}; return (int)sizeof *(int (*)[n])x; }\n");
}

// A VOLATILE pointer's read is a side effect (C 5.1.2.3p2): evaluating `*p` reads it, so it runs
// before the size is read — the one read, and none when a `++` of it already reads it.
TEST(VariableLengthEvaluation, AVolatilePointersReadRuns) {
    expectTheSizeofEvaluates("int f(int n, int m) { int v[n][m]; int (*volatile p)[m] = v; "
                             "return (int)sizeof *p; }\n",
                             {"p"});
}

// The bare object computes nothing: its size is read, nothing runs before it.
TEST(VariableLengthEvaluation, TheBareObjectRunsNothing) {
    for (char const* operand : {"v", "(v)"}) {
        auto l = lower(std::string{"int f(int n) { int v[n]; return (int)sizeof "} + operand +
                       "; }\n");
        ASSERT_NE(l->result, nullptr) << operand;
        EXPECT_EQ(l->reporter.errorCount(), 0u) << operand << dump(l->reporter);
        EXPECT_TRUE(sequencesYielding(l->result->hir, HirKind::SizeOf).empty()) << operand;
    }
}
