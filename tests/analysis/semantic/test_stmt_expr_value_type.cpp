// P32 lane A — [[D-C-SUBTREE-TYPE-WALKS-INTO-A-STATEMENT-EXPRESSION-BODY]]
//
// The semantic tier's expression typer (`subtreeType`) used to descend into a GNU
// statement expression's BODY and take a type from a DECLARATION inside it, so a
// use of the construct's VALUE was judged against the wrong operand.
//
// ✔MEASURED through the shipped CLI at the pre-change HEAD, bisected so the
// trigger is exact rather than guessed:
//     `*({ *q; })`                        ✅   no declaration in the body
//     `*({ int *r = p; r; })`             ✅   declarator type == yielded type
//     `int x = *({ int **q = &p; *q; });` ✗    false `S0003`
// ⇒ the trigger is a declaration whose type DIFFERS from what the body yields.
//
// ★★ WHY THIS FILE EXISTS BESIDE A RUNNABLE EXAMPLE. The accepting half is a
// program that must BUILD, and `examples/c/stmt_expr_value_type_from_last_item/`
// owns it. What an example structurally cannot carry is the REFUSING half — and
// the refusing half is where this fix could go wrong in the other direction. A
// statement expression whose last item is NOT an expression statement has type
// `void` (GNU C 6.1), and using it as a VALUE is refused by gcc and by clang;
// the old descent would happily have answered with some declarator's type
// instead, which is a silent answer to a question the language says has none.
// So the `void` cases are pinned here, by CODE, not by "the build failed".
//
// ★ AND THE FIX IS DERIVED FROM THE LOWERING, not written twice: the value is
// the last item's expression, recognised by HIR kind `ExprStmt` exactly as
// `stmtExprItems` recognises it in `cst_to_hir.cpp`. The two tiers therefore
// cannot disagree about what a body yields, which is the property the pins below
// are really protecting.
//
// ⓘ THE VLA C4a-LOCAL NOTE IN THE SAME FILE WAS CHECKED AND IS NOT THIS BUG —
// the registry row asked, and the answer is measured rather than assumed. That
// note is about a node the earlier pass already STAMPED with a decayed pointer
// type, which `subtreeType` short-circuits on BEFORE any structural arm runs; it
// is a stamped-node question, not a descent question, and no boundary in the walk
// can reach it. It stayed its own deferral
// ([[D-CSUBSET-VLA-PTR-INIT-FORM-TYPING]]) — and CLOSED in P34 from a THIRD place
// again: not this walk and not that stamp, but the DECLARATOR resolver, which had
// been building `Ptr<incompleteArray<int>>` for `(*p)[n]` whenever an initializer
// was present. The conclusion recorded here — that it is not this bug — held.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/hir.hpp"
#include "hir/hir_node.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] std::size_t parseErrorsFor(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    std::size_t n = 0;
    for (auto const& t : cu->trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

// The `void`-statement-expression refusal fires at the HIR LOWERING tier, not in
// `analyze()` — whether a VALUE is required is a property of the lowering
// position, and the lowering is the tier that knows which position it is in. A
// pin that read `model.diagnostics()` alone would report ZERO for a source the
// compiler in fact refuses: green, asserting nothing. (This exact mistake is
// recorded in `test_gnu_extension_and_statement_expr.cpp`; it is not repeated.)
[[nodiscard]] std::size_t loweringCodeCount(std::string const& src,
                                            DiagnosticCode code) {
    auto cu = buildShippedUnit("c", {src});
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    DiagnosticReporter reporter{};
    auto lowered = lowerToHir(model, reporter);
    (void)lowered;
    return countCode(reporter, code) + countCode(model.diagnostics(), code);
}

} // namespace


// ★ THE ROW'S EXACT REPRO. The dereference is checked against the body's YIELDED
// type (`int *`), not against the `int **` a declaration inside the body happens
// to introduce. Asserting on `S_TypeMismatch` and not merely on "zero errors" is
// deliberate: this is a false-POSITIVE bug, so the pin names the diagnostic that
// must not appear.
TEST(StmtExprValueType, DeclarationInsideTheBodyDoesNotTypeTheConstruct) {
    std::string const src =
        "int main(void){ int v = 7; int *p = &v;\n"
        "                int x = *({ int **q = &p; *q; });\n"
        "                return x - 7; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u)
        << "the body yields `int *`; the `int **` declared inside it is not the "
           "operand of the dereference";
    EXPECT_FALSE(hasDiagnosedPointerConversion(model.diagnostics()))
        << "a compatible pointer pair must not be DIAGNOSED (the vacuity sweep)";
}

// The two bisect CONTROLS, which passed before the fix and must still pass: a
// body with no declaration, and a body whose declarator type happens to equal
// what it yields. Without these, a fix that simply stopped typing statement
// expressions at all would look identical to the real one.
TEST(StmtExprValueType, TheTwoShapesThatAlreadyWorkedStillWork) {
    std::string const src =
        "int main(void){ int v = 7; int *p = &v;\n"
        "                int a = *({ p; });\n"
        "                int b = *({ int *r = p; r; });\n"
        "                return a + b - 14; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_FALSE(hasDiagnosedPointerConversion(model.diagnostics()))
        << "a compatible pointer pair must not be DIAGNOSED (the vacuity sweep)";
}

// A body whose declarations share NOTHING with what it yields — the walk that
// took the first declarator would come back with `double`.
TEST(StmtExprValueType, ADeclarationOfAnUnrelatedTypeDoesNotLeakOut) {
    std::string const src =
        "int main(void){ int c = ({ double d = 0.5; int t = 10; (void)d; t; });\n"
        "                return c - 10; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
}

// ★★ THE REFUSING HALF — the half a corpus example cannot carry. A body whose
// LAST item is a declaration yields nothing at all, and using that as a value is
// refused by gcc ("void value not ignored as it ought to be") and by clang. The
// typer must answer "no type" rather than the declaration's type, which is what
// lets the lowering emit its refusal instead of silently accepting.
TEST(StmtExprValueType, ABodyEndingInADeclarationHasNoValueAndIsRefused) {
    std::string const src =
        "int main(void){ int y = ({ int t = 1; });\n"
        "                return y; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u)
        << "the construct PARSES; the refusal below is semantic, which is what "
           "makes it possible to state a reason";
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 1u)
        << "a `void` statement expression in VALUE position must be refused, not "
           "silently typed from whatever the body declared";
}

// The same `void` body in a DISCARD position is legal — it is a block, and its
// value is nobody's business. The pair is what keeps the refusal above from
// being over-broad.
TEST(StmtExprValueType, TheSameVoidBodyIsFineWhereNoValueIsRequired) {
    std::string const src =
        "int main(void){ ({ int t = 1; (void)t; });\n"
        "                return 0; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 0u);
}

// `sizeof` reads the semantic tier's type DIRECTLY, with no lowering behind it —
// so it is the sharpest available probe of what `subtreeType` actually answered.
// A body yielding `char` must fold to 1, not to the width of the `double` its
// body declares.
TEST(StmtExprValueType, SizeofReadsTheYieldedTypeAndNotADeclaredOne) {
    std::string const src =
        "int main(void){ char arr[sizeof(({ double d = 0.5; char ch = 'a'; "
        "(void)d; ch; }))];\n"
        "                return (int)sizeof(arr) - 1; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_TypeMismatch), 0u);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_NonConstantArrayLength),
              0u)
        << "the fold must SUCCEED — a declined fold is the silent failure mode "
           "this probe exists to catch";
}

// ══ THE CONSTRUCT YIELDS A VALUE: ITS TYPE IS THE LAST EXPRESSION'S, CONVERTED ═══════
// (cycle P69 round 1, lane `lm`.)
//
// The value of the last expression is no lvalue. Its type is the operand's with the
// three conversions C 6.3.2.1p2-p4 names and no other (`valueConversion`,
// `analysis/semantic/type_rules.hpp`): an array is the pointer to its first element,
// a function designator the pointer to the function, and no qualifier stays. The
// comma operator yields a value the same way, and the two constructs read ONE law.
//
// ✔MEASURED 2026-10-12 (gcc 13, mingw-w64 gcc 13.2, clang 18, AppleClang 21 — 31
// questions each through `({ e; })`, through `(0, e)` and bare): the four agree on
// every cell pinned here. DSS answered 4 for `sizeof(({ "abc"; }))` and 12 for
// `sizeof(({ a; }))` over an `int a[3]` — the ARRAY's size, a silent wrong constant —
// 1 for `sizeof(({ f; }))` and for `sizeof((0, f))`, and kept `volatile` through
// both constructs. And because the lowering built the sequence at the array's type
// with the conversion laid over it, `static const char *p = ({ "abc"; });` — which
// runs on all four — was refused as "not folded", and `char k[4] = ({ "abc"; });`
// passed this tier for the lowering to refuse by an internal sentence.

namespace {

[[nodiscard]] std::string firstActual(SemanticModel const& m, DiagnosticCode code) {
    for (auto const& d : m.diagnostics().all())
        if (d.code == code) return d.actual;
    return {};
}

// The codes of the ERRORS the SEMANTIC tier reports for `src`, in the order it
// reported them. (A source this tier refuses is never lowered by the pipeline.)
[[nodiscard]] std::vector<DiagnosticCode> semanticErrorCodesOf(std::string const& src) {
    auto model = analyzeC(src);
    std::vector<DiagnosticCode> out;
    for (auto const& d : model.diagnostics().all())
        if (d.severity == DiagnosticSeverity::Error) out.push_back(d.code);
    return out;
}

constexpr char const* kValuePrelude =
    "static int f(void){ return 0; }\n"
    "static int a[3];\n"
    "static volatile int v;\n";

} // namespace

TEST(StmtExprValueType, AnArrayOrAFunctionYieldedIsThePointerItConvertsTo) {
    std::string const src = std::string{kValuePrelude} +
        "int main(void){\n"
        "    char c = 1;\n"
        "    _Static_assert(sizeof(({ \"abc\"; })) == sizeof(char *), \"a string through the braces\");\n"
        "    _Static_assert(sizeof(({ a; })) == sizeof(int *), \"an array through the braces\");\n"
        "    _Static_assert(sizeof(({ f; })) == sizeof(int (*)(void)), \"a function through the braces\");\n"
        "    _Static_assert(sizeof((0, a)) == sizeof(int *), \"an array through the comma\");\n"
        "    _Static_assert(sizeof((0, f)) == sizeof(int (*)(void)), \"a function through the comma\");\n"
        "    _Static_assert(sizeof(({ c; })) == 1 && sizeof((0, c)) == 1, \"a char is not promoted\");\n"
        "    return c + f();\n"
        "}\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u)
        << firstActual(model, DiagnosticCode::S_StaticAssertFailed);
    EXPECT_FALSE(model.hasErrors());
    // What this tier answered before. Each assertion is FALSE and must fail, by this
    // code — the array's own size, and the size GNU C gives a function type.
    char const* const wasSilentlyTrue[] = {
        "_Static_assert(sizeof(({ \"abc\"; })) == 4, \"m\");",
        "_Static_assert(sizeof(({ a; })) == 3 * sizeof(int), \"m\");",
        "_Static_assert(sizeof(({ f; })) == 1, \"m\");",
        "_Static_assert(sizeof((0, f)) == 1, \"m\");",
    };
    for (char const* line : wasSilentlyTrue) {
        auto m = analyzeC(std::string{kValuePrelude} + "int main(void){ " + line + " return 0; }\n");
        EXPECT_EQ(countCode(m.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u) << line;
    }
}

// THE QUALIFIER THE TypeId CARRIES GOES TOO. `typeof` is the reader that shows it:
// an object declared with the construct's type is assignable through a plain
// `int *` only when the type lost its `volatile` — `_Generic` over its address
// selects by the pointee's qualification. (`const` is not interned; its half of
// the sentence, and every other value form's, is the case after the next.)
TEST(StmtExprValueType, AQualifierOfTheLastExpressionsTypeIsNotTheValues) {
    std::string const src = std::string{kValuePrelude} +
        "int main(void){\n"
        "    __typeof__(v) y0 = 0;\n"
        "    __typeof__(({ v; })) y1 = 0;\n"
        "    __typeof__((0, v)) y2 = 0;\n"
        "    _Static_assert(_Generic(&y0, int *: 41, volatile int *: 42, default: 43) == 42,"
        " \"the control: the object's own type is volatile\");\n"
        "    _Static_assert(_Generic(&y1, int *: 41, volatile int *: 42, default: 43) == 41,"
        " \"through the braces the value is not volatile\");\n"
        "    _Static_assert(_Generic(&y2, int *: 41, volatile int *: 42, default: 43) == 41,"
        " \"through the comma the value is not volatile\");\n"
        "    return y0 + y1 + y2;\n"
        "}\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u)
        << firstActual(model, DiagnosticCode::S_StaticAssertFailed);
    EXPECT_FALSE(model.hasErrors());
}

// THE LOWERING BUILDS WHAT THIS TIER TYPED — the two halves move together or the
// defect stays whole (`lowerSizeof` reads this tier's stamp, the static-initializer
// fold reads the lowering's node). Every sequence the lowering makes for a
// statement expression or a comma whose last expression is an array or a function
// is typed as the POINTER, with the decay inside it. (Automatic objects, which were
// built before too — the conversion was laid OVER the sequence; the static objects
// that form refused are the example's, in both runners.)
TEST(StmtExprValueType, TheLoweredSequenceIsTypedAsTheValue) {
    std::string const src = std::string{kValuePrelude} +
        "int main(void){\n"
        "    const char *p = ({ \"abc\"; });\n"
        "    int *q = ({ a; });\n"
        "    int (*g)(void) = ({ f; });\n"
        "    int (*h)(void) = (0, f);\n"
        "    return p[0] + q[0] + g() + h();\n"
        "}\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto cu = buildShippedUnit("c", {src});
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    ASSERT_FALSE(model.hasErrors());
    DiagnosticReporter reporter{};
    auto lowered = lowerToHir(model, reporter);
    ASSERT_TRUE(lowered != nullptr);
    ASSERT_EQ(reporter.errorCount(), 0u);
    Hir const& hir = lowered->hir;
    auto const& in = model.lattice().interner();
    std::size_t sequences = 0;
    std::vector<HirNodeId> stack{hir.root()};
    while (!stack.empty()) {
        HirNodeId const id = stack.back();
        stack.pop_back();
        for (HirNodeId c : hir.children(id)) stack.push_back(c);
        if (hir.kind(id) != HirKind::SeqExpr) continue;
        ++sequences;
        EXPECT_EQ(in.kind(hir.typeId(id)), TypeKind::Ptr)
            << "a sequence yielding an array or a function is typed as the pointer "
               "the value converts to, not as what its last expression designates";
    }
    EXPECT_EQ(sequences, 4u) << "the premise: one sequence per construct of the source";
}

// ══ AN EXPRESSION THAT IS NO LVALUE HAS NO QUALIFIER AT ITS OWN LEVEL ════════════════
//
// The comma and the statement expression are two of the forms; the sentence is one,
// and it is said once per tier of the qualifier: `volatile` / `_Atomic` live in the
// TypeId (`subtreeType`, where a frame finishes on a value form, and the two Pass-2
// stamps that build no frame — a cast node's and a call node's), `const` / `restrict`
// in the spine beside it (`expressionQualifierSpine`'s `finish`).
//
// ✔MEASURED 2026-10-12 (43 questions — 20 `const`, 23 `volatile` — each `__typeof__(E)
// y;` read by `_Generic(&y, …)`; gcc 13, mingw-w64 gcc 13.2, clang 18 and AppleClang 21
// agree in every one): an lvalue keeps its qualifier (a name, parentheses, `*&`, a
// member, `__builtin_choose_expr` whose arm is the name, a compound literal); a value
// has none (a conditional, a cast to the qualified type, `+` / `-`, a call's result,
// an assignment, a compound assignment, `++` / `--` either side, the comma, a
// statement expression) — and a MEMBER of a value keeps what its own declaration
// says: it is the OBJECT's qualifier a value has lost. DSS kept `volatile` through
// the cast, `+`, `-`, the call, `=`, `+=` and the four `++` / `--`, kept the `const` of
// a `const` pointer through the comma and the braces, and dropped the `const` of the
// arm `__builtin_choose_expr` chooses.
// ⓘ Two lvalue cells of the same set are NOT pinned here, because DSS still answers
// them wrong and they are `typeof`'s own: `typeof(_Generic(0, int: cx))` drops `const`
// (the spine reads the selection Pass 2 records, and a declaration's `typeof` head is
// resolved before it), and `typeof(vs.m)` over a `volatile struct` drops `volatile`
// (this tier's member type lacks the object's).
namespace {

struct QualifierCell {
    char const* expr;     // `Q` stands for the qualifier
    bool        keeps;    // the expression's type keeps the qualifier at its own level
};

// One program: every cell declares `y` by `typeof` of its expression and asserts what
// a pointer to `y` selects. The assertion's message is the expression, so a failure
// names its cell.
[[nodiscard]] std::string qualifierProgram(std::string const& q,
                                           std::vector<QualifierCell> const& cells) {
    auto const spelled = [&](std::string text) {
        for (std::size_t at = text.find('Q'); at != std::string::npos; at = text.find('Q', at))
            text.replace(at, 1, q), at += q.size();
        return text;
    };
    std::string src = spelled(
        "struct S { Q int qm; int m; };\n"
        "static Q int fq(void) { return 1; }\n"
        "static struct S fs(void) { struct S r = {1, 2}; return r; }\n"
        "int main(void) {\n"
        "    Q int qx = 1;\n"
        "    int m = 1;\n"
        "    struct S s = {1, 2};\n"
        "    Q struct S qs = {1, 2};\n"
        "    (void)qx; (void)m; (void)s; (void)qs;\n");
    for (QualifierCell const& c : cells) {
        std::string const e = spelled(c.expr);
        src += "    { __typeof__(" + e + ") y = 0; _Static_assert(_Generic(&y, int *: 41, "
             + q + " int *: 42, default: 43) == " + (c.keeps ? "42" : "41") + ", \"" + e
             + "\"); (void)y; }\n";
    }
    src += "    return fq() + fs().m;\n}\n";
    return src;
}

[[nodiscard]] std::string allActuals(SemanticModel const& m, DiagnosticCode code) {
    std::string out;
    for (auto const& d : m.diagnostics().all())
        if (d.code == code) out += "\n    " + d.actual;
    return out;
}

} // namespace

TEST(StmtExprValueType, AValueHasNoQualifierOfTheObjectItWasReadFrom) {
    // The forms both qualifiers can be asked of.
    std::vector<QualifierCell> const shared = {
        {"qx", true}, {"(qx)", true}, {"*&qx", true}, {"s.qm", true},
        {"__builtin_choose_expr(1, qx, 0)", true}, {"(Q int){1}", true},
        {"fs().qm", true},                       // a member of a VALUE: its own declaration's
        {"(1 ? qx : qx)", false}, {"(Q int)m", false}, {"+qx", false}, {"-qx", false},
        {"fq()", false}, {"fs().m", false}, {"(0, qs).m", false}, {"({ qs; }).m", false},
        {"(0, qx)", false}, {"({ qx; })", false},
    };
    // A `const` object cannot be assigned, so these are `volatile`'s alone.
    std::vector<QualifierCell> assigning = shared;
    for (char const* e : {"(qx = 1)", "(qx += 1)", "qx++", "++qx", "qx--", "--qx"})
        assigning.push_back({e, false});

    for (auto const& [q, cells] : {std::pair{std::string{"const"}, shared},
                                   std::pair{std::string{"volatile"}, assigning}}) {
        SCOPED_TRACE(q);
        std::string const src = qualifierProgram(q, cells);
        ASSERT_EQ(parseErrorsFor(src), 0u);
        auto model = analyzeC(src);
        EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u)
            << "the cells whose type is wrong:"
            << allActuals(model, DiagnosticCode::S_StaticAssertFailed);
        EXPECT_FALSE(model.hasErrors());
    }

    // The level BELOW is what the value points to, and stays: a `const` pointer to
    // `const int` read as a value is a plain pointer to `const int`.
    std::string const pointers =
        "int main(void) {\n"
        "    int m = 1;\n"
        "    const int *const cp = &m;\n"
        "    { __typeof__(cp) y = 0; _Static_assert(_Generic(&y, const int **: 41,"
        " const int *const *: 42, default: 43) == 42, \"cp\"); (void)y; }\n"
        "    { __typeof__((0, cp)) y = 0; _Static_assert(_Generic(&y, const int **: 41,"
        " const int *const *: 42, default: 43) == 41, \"(0, cp)\"); (void)y; }\n"
        "    { __typeof__(({ cp; })) y = 0; _Static_assert(_Generic(&y, const int **: 41,"
        " const int *const *: 42, default: 43) == 41, \"({ cp; })\"); (void)y; }\n"
        "    return *cp - 1;\n"
        "}\n";
    ASSERT_EQ(parseErrorsFor(pointers), 0u);
    auto model = analyzeC(pointers);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_StaticAssertFailed), 0u)
        << "the cells whose type is wrong:"
        << allActuals(model, DiagnosticCode::S_StaticAssertFailed);
    EXPECT_FALSE(model.hasErrors());
}

// AN ARRAY IS NOT INITIALIZED BY THE POINTER A STATEMENT EXPRESSION YIELDS — and DSS
// says so as it says it of any pointer. Every reference refuses both forms
// (✔MEASURED 2026-10-12, the four). They were refused before too, but by the
// LOWERING's sentence about its own state ("an aggregate-typed comma/SeqExpr must be
// lowered by ADDRESS"): this tier had taken the construct for an array and passed
// it. The pin is the PAIR, in this tier, for an array of a STATED size: the same
// error codes, in the same order, as the plain pointer draws — and at least one.
//
// ⚠ AN ARRAY OF NO STATED SIZE IS REFUSED BY THE SAME SENTENCE ABOUT ITS VALUE, AND
// NOT YET BY THE ONE ITS POINTER TWIN DRAWS. ✔MEASURED 2026-10-12 (this binary's first
// run, the Mac leg): `char k[] = ({ "abc"; });` draws S_TypeMismatch — an array is not
// initialized by a pointer, what the sized form draws — where `char k[] = p;` draws
// S_ArrayLengthOutOfRange: the search that sizes an array from its initializer
// (`completeIncompleteArrayFromInit`) still walks INTO the construct and sizes this
// one from the literal inside. It is one of the six descents that do not read the
// statement of what a wrapper is (`core/types/decl_prefix_strip.hpp` names them);
// when it does, this half becomes the pair too, and the exact code below is what
// tells whoever changes it.
TEST(StmtExprValueType, AnArrayInitializedByTheConstructIsRefusedAsByAnyPointer) {
    std::string const head = "int main(void){ const char *p = \"abc\"; (void)p; ";
    {
        SCOPED_TRACE("char k[4]: the pair");
        auto const construct =
            semanticErrorCodesOf(head + "char k[4] = ({ \"abc\"; }); return k[0]; }\n");
        auto const pointer = semanticErrorCodesOf(head + "char k[4] = p; return k[0]; }\n");
        ASSERT_FALSE(pointer.empty()) << "the premise: the plain pointer form is refused";
        EXPECT_EQ(construct, pointer)
            << "the construct's value is a pointer, and this tier refuses an array "
               "initialized by it as it refuses one initialized by any pointer";
    }
    {
        SCOPED_TRACE("char k[]: refused for its value");
        auto const construct =
            semanticErrorCodesOf(head + "char k[] = ({ \"abc\"; }); return k[0]; }\n");
        auto const pointer = semanticErrorCodesOf(head + "char k[] = p; return k[0]; }\n");
        EXPECT_EQ(construct, (std::vector<DiagnosticCode>{DiagnosticCode::S_TypeMismatch}))
            << "an array of no stated size is refused in this tier for the pointer its "
               "initializer yields";
        EXPECT_EQ(pointer,
                  (std::vector<DiagnosticCode>{DiagnosticCode::S_ArrayLengthOutOfRange}))
            << "the premise of the comment above: the plain pointer cannot size the array";
    }
}
