// ══ A CONSTANT CONDITIONAL IS BALANCED, AND ITS TYPE IS NEVER STATED TWICE IN SILENCE ═══
// (cycle P69 round 1, lane `lm`; a review of the bit-field promotion fold, its BLOCKER B.)
//
// C 6.5.15p5: when both arms of a conditional are arithmetic, the conditional has the type
// the usual arithmetic conversions give them (each arm promoted first — a bit-field by its
// width) — `1 ? 5 : 0u` is an `unsigned int`, so `(1 ? 5 : 0u) - 6 < 0` is false. The
// constant evaluator handed on the SELECTED arm at its own type. ✔MEASURED 2026-10-15/16
// (WSL clang 18.1.3 and gcc 13.3.0, -O0/-O2; DSS pe64, both pipelines, images run): every
// reference runs each `if` below false and accepts each assertion and bound; DSS folded the
// `if`s true and died of an illegal instruction (the code after them marked never reached),
// failed the assertions, and refused the file-scope bound as a negative length.
//
// The evaluator converts the selected arm to the TIER's type of the conditional where the
// tier has one (the semantic tier's typer, which types the literals first before its second
// pass; the lowering's record of it, `conditionalTypeFor`) and, where it has none, to the
// arms' balance it computes itself (`bitIntUac`, the verb its binary operators perform
// in), a non-constant arm by the tier's type of that arm. ★ WHERE BOTH EXIST BOTH ARE
// COMPUTED, and a disagreement stops the process (`failOnDivergentConditionalType`,
// src/hir/cst_const_eval.cpp) — so every case here runs IN A CHILD PROCESS, and a stop is the
// case's red with the stop's own words, never the binary's death. A binary of its own: a
// mutation of either statement stops every binary that folds such a conditional.

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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

constexpr char const* kObjects =
    "static const struct S { int pad; unsigned b : 3; unsigned long long q : 3; } g = { 0, 5, 5 };\n";

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// Every error of the analysis of `src`, in words; empty when it has none.
[[nodiscard]] std::string analysisErrors(std::string const& src) {
    std::string out;
    auto const m = analyzeC(src);
    for (auto const& d : m.diagnostics().all())
        if (d.severity == DiagnosticSeverity::Error) out += "\n    error: " + d.actual;
    return out;
}

// How many positions of the unit the lowering marks as never reached — what it concluded
// from folding a condition (`ifNeverCompletes`); SIZE_MAX when the unit does not lower.
[[nodiscard]] std::size_t positionsMarkedNeverReached(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    if (model.hasErrors()) return SIZE_MAX;
    DiagnosticReporter reporter{};
    auto lowered = lowerToHir(model, reporter);
    if (lowered == nullptr || reporter.errorCount() != 0) return SIZE_MAX;
    Hir const& hir = lowered->hir;
    std::size_t marks = 0;
    for (std::uint32_t i = 1; i < hir.nodeCount(); ++i)
        if (hir.kind(HirNodeId{i, hir.id().v}) == HirKind::Unreachable) ++marks;
    return marks;
}

// `body` as the body of `int f(int y)`, with the objects.
[[nodiscard]] std::string inF(std::string const& body) {
    return std::string{kObjects} + "int f(int y) {\n" + body + "\n}\nint main(void) { return f(1); }\n";
}

// THE CHILD: `check` runs in a process of its own and reports what it found on stderr; the
// process exits 0 when it found nothing.
template <class Check>
void inAChildProcess(Check const& check) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            std::string const found = check();
            std::fputs(found.c_str(), stderr);
            std::exit(found.empty() ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

} // namespace

// Each assertion holds in C and on both references; each `if` is false in C (no position
// after it marked) unless it says otherwise. A NON-constant arm balances by its type, and an
// arm that does not fold is never read.
TEST(ConstantConditionalIsBalanced, TheSelectedArmHasTheConditionalsType) {
    inAChildProcess([] {
        std::string found = analysisErrors(inF(
            "    unsigned xu = 3;\n"
            "    _Static_assert(!((1 ? g.b : 0u) - 6 < 0), \"int and unsigned int: unsigned int\");\n"
            "    _Static_assert(!((1 ? 5 : 0u) - 6 < 0), \"the plain root\");\n"
            "    _Static_assert(!((0 ? 0u : 5) - 6 < 0), \"the other arm selected\");\n"
            "    _Static_assert(!((1 ? 5 : 0ull) - 6 < 0), \"unsigned long long\");\n"
            "    _Static_assert((1 ? -1 : 0u) > 0, \"-1 converted to unsigned int\");\n"
            "    _Static_assert((1 ? g.b : 0) - 6 < 0, \"int and int: int\");\n"
            "    _Static_assert((1 ? g.b : g.b) - 6 < 0, \"both arms promoted\");\n"
            "    _Static_assert((1 ? 5 : xu) - 6 > 0, \"a non-constant arm, by its type\");\n"
            "    _Static_assert((1 ? 5 : 1 / 0) == 5, \"an arm that does not fold\");\n"
            "    (void)xu;\n    return y;"));
        // What was answered before: each is FALSE of C and must fail, once.
        for (char const* line : {"    _Static_assert((1 ? g.b : 0u) - 6 < 0, \"m\");\n    return y;",
                                 "    _Static_assert((1 ? 5 : 0u) - 6 < 0, \"m\");\n    return y;",
                                 "    unsigned xu = 3;\n    _Static_assert((1 ? 5 : xu) - 6 < 0, \"m\");\n"
                                 "    (void)xu;\n    return y;"}) {
            if (analysisErrors(inF(line)).empty())
                found += std::string{"\n    accepted, false of C:\n"} + line;
        }
        struct Fold { char const* body; std::size_t marks; };
        for (Fold const& f : {
                 Fold{"    if ((1 ? 5 : 0u) - 6 < 0) return 7;\n    return 42;", 0},
                 Fold{"    if ((1 ? g.b : 0u) - 6 < 0) return 7;\n    return 42;", 0},
                 Fold{"    unsigned xu = (unsigned)y;\n    if ((1 ? 5 : xu) - 6 < 0) return 7;\n"
                      "    return 42;", 0},
                 // true in C: the position after it is marked — through the lowering's record
                 // of the typer's type when the other arm neither folds nor carries a stamp
                 Fold{"    if ((1 ? 5 : 0u) - 6 >= 0) return 7;\n    return 42;", 1},
                 Fold{"    unsigned xu = (unsigned)y;\n    if ((1 ? 5 : xu + 1u) - 6 > 0) return 7;\n"
                      "    return 42;", 1},
             }) {
            std::size_t const marks = positionsMarkedNeverReached(inF(f.body));
            if (marks != f.marks)
                found += "\n    " + std::to_string(marks) + " position(s) marked never reached, not " +
                         std::to_string(f.marks) + ", in:\n" + f.body;
        }
        return found;
    });
}

// BEFORE THE PASS THAT TYPES A LITERAL. A file-scope array bound, an enumerator, a bit-field
// width and a file-scope static assertion are folded before the semantic tier's second pass
// stamps a literal's type: the typer's hook types the conditional's literals first (the one
// walk every typer query before that pass takes), so the conditional has its type there too,
// and an UNSELECTED arm that does not fold and holds only literals (`1 / 0`, `1u << 40`) is
// typed rather than read as no constant. ✔MEASURED 2026-10-16, this unit word for word:
// clang 18.1.3 builds it and runs it (folding `a2`, `a3` and `a5` as an extension,
// -Wgnu-folding-constant); gcc 13.3.0 refuses `a2` and `a5` (a member of a constant is no
// integer constant expression to it, and a variably modified array at file scope an error),
// and builds and runs the unit without those two, warning `a3` variably modified at file
// scope; DSS had refused `a6` S_NonConstantArrayLength. One working reference makes it
// required.
TEST(ConstantConditionalIsBalanced, BeforeItsLiteralsAreTyped) {
    inAChildProcess([] {
        return analysisErrors(std::string{kObjects} +
            "static int x;\n"
            "int a1[(1 ? 5 : 0u) - 6 > 0 ? 1 : -1];\n"
            "int a2[(1 ? g.b : 0u) - 6 > 0 ? 1 : -1];\n"
            "int a3[(1 ? 5 : x) == 5 ? 1 : -1];\n"
            "int a4[(1 ? -1 : 0u) > 0 ? 1 : -1];\n"
            "int a5[(1 ? g.b : 0) - 6 < 0 ? 1 : -1];\n"
            "int a6[(1 ? 5 : 1 / 0) == 5 ? 1 : -1];\n"
            "int a7[(1 ? 5 : 1 / 0u) - 6 > 0 ? 1 : -1];\n"
            "int a8[(1 ? -1 : 1u / 0) > 0 ? 1 : -1];\n"
            "int a9[(1 ? 5 : 1u << 40) - 6 > 0 ? 1 : -1];\n"
            "enum { E1 = (1 ? 5 : 1 / 0) == 5 ? 1 : -1, E2 = (1 ? 5 : 1 / 0u) - 6 > 0 };\n"
            "int a10[E1];\n"
            "int a11[E2];\n"
            "struct W { unsigned w : ((1 ? 5 : 1 / 0u) - 6 > 0) + 2; };\n"
            "_Static_assert((1 ? 5 : 1 / 0) == 5, \"a file-scope assertion\");\n"
            "_Static_assert((1 ? 5 : 1 / 0u) - 6 > 0, \"its arm unsigned\");\n"
            "int main(void) { return 0; }\n");
    });
}

// THE TWO STATEMENTS AGREE. Every conditional below is folded where both the tier's type and
// the arms' balance exist — the semantic tier's second pass and the lowering — over arms
// that balance in each way the conversions do: to a type of neither arm, the unsigned of
// equal rank, the wider, a floating type, past a promoted bit-field. A disagreement is the
// process stopping in the child, in its own words.
TEST(ConstantConditionalIsBalanced, TheTypersTypeAndTheArmsBalanceAgree) {
    inAChildProcess([] {
        std::string const body =
            "    _Static_assert((1 ? 5 : 0u) - 6 > 0, \"int and unsigned int\");\n"
            "    _Static_assert((0 ? 0u : 5) - 6 > 0, \"the other arm selected\");\n"
            "    _Static_assert((1 ? 5 : 0ull) - 6 > 0, \"int and unsigned long long\");\n"
            "    _Static_assert((1 ? (char)1 : 2L) == 1, \"char and long\");\n"
            "    _Static_assert((1 ? 1u : 2L) == 1, \"unsigned int and long\");\n"
            "    _Static_assert((int)((1 ? 5 : 2.0) * 2) == 10, \"int and double\");\n"
            "    _Static_assert((1 ? g.b : 0u) - 6 > 0, \"a promoted bit-field and unsigned int\");\n"
            "    if ((1 ? 5 : 0u) - 6 < 0) return y;\n"
            "    if ((1 ? g.b : 0ull) - 6 > 0) return y + 1;\n"
            "    return 0;";
        std::string found = analysisErrors(inF(body));
        std::size_t const marks = positionsMarkedNeverReached(inF(body));
        if (marks != 1)
            found += "\n    " + std::to_string(marks) + " position(s) marked never reached, not 1";
        return found;
    });
}

// AN ARM THAT HOLDS FLOATING ARITHMETIC, OR A FOLDED BUILTIN (P69 L2, lane `lm`, the review of
// checkpoint 29, BLOCKER 1). The balance reads the arms' folded values, so each value must
// state its C type: the float fold handed `2.0f * 3.0f` on as a binary64 (core F64) and the
// folded-constant rules every value as a 64-bit unsigned, so the balance said `double` / an
// unsigned 64-bit integer where the typer said `float` / `int`, and the process STOPPED.
// ✔MEASURED 2026-10-20 on checkpoint 29's bytes (pe64 and ELF, both pipelines): every
// conditional below stopped the compiler (exit -6, "the typer's F32 and the arms' balance
// F64"; "I32 … U64" for the builtin), where clang 18.1.3 and gcc 13.3.0 build and run each
// (clang has no C23 `constexpr`). Each assertion holds; each `if` is true in C, so the
// position after it is marked never reached.
TEST(ConstantConditionalIsBalanced, AnArmOfFloatingArithmeticOrAFoldedBuiltinHasItsType) {
    inAChildProcess([] {
        std::string found = analysisErrors(inF(
            "    constexpr float cf = 1 ? 2.0f * 3.0f : 1.0f;\n"
            "    _Static_assert((1 ? 2.0f * 3.0f : 1.0f) > 0, \"float times float: float\");\n"
            "    _Static_assert((1 ? 0.5f * 2 : 0.0f) == 1.0f, \"float times int: float\");\n"
            "    _Static_assert((1 ? 5 : (0 ? 2.0f * 3 : 1.0f)) == 5, \"the unselected arm\");\n"
            "    _Static_assert((1 ? 2.0f * 3.0f : 1.0) == 6.0, \"float and double: double\");\n"
            "    _Static_assert((1 ? __builtin_types_compatible_p(int, int) : 0) == 1, \"an int\");\n"
            "    _Static_assert((1 ? __builtin_types_compatible_p(int, int) : 0u) - 2 > 0,\n"
            "                   \"an int and an unsigned int: unsigned int\");\n"
            "    (void)cf;\n    return y;"));
        found += analysisErrors(std::string{kObjects} +
            "int a1[(1 ? 2.0f * 3.0f : 1.0f) > 0 ? 1 : -1];\n"
            "int a2[(1 ? __builtin_types_compatible_p(int, int) : 0) == 1 ? 1 : -1];\n"
            "static int s = (1 ? 2.0f * 3.0f : 1.0f) > 0 ? 42 : 7;\n"
            "_Static_assert((1 ? 2.0f * 3.0f : 1.0f) > 0, \"at file scope\");\n"
            "int main(void) { return s; }\n");
        struct Fold { char const* body; std::size_t marks; };
        for (Fold const& f : {
                 Fold{"    if ((1 ? 2.0f * 3.0f : 1.0f) > 0) return 7;\n    return 42;", 1},
                 Fold{"    if ((1 ? 2.0f * 3.0f : 1.0f) < 0) return 7;\n    return 42;", 0},
                 Fold{"    if ((1 ? 5 : (0 ? 2.0f * 3 : 1.0f)) == 5) return 7;\n    return 42;", 1},
                 Fold{"    static const float fc = 3.0f;\n    if ((1 ? fc * 2 : 0.0f) > 0) return 7;\n"
                      "    return 42;", 1},
                 Fold{"    if ((1 ? __builtin_types_compatible_p(int, int) : 0) == 1) return 7;\n"
                      "    return 42;", 1},
             }) {
            std::size_t const marks = positionsMarkedNeverReached(inF(f.body));
            if (marks != f.marks)
                found += "\n    " + std::to_string(marks) + " position(s) marked never reached, not " +
                         std::to_string(f.marks) + ", in:\n" + f.body;
        }
        return found;
    });
}

// THE CONTROL: arms of ONE type keep it — the conversion of the selected arm is the identity,
// so this case stands whatever either statement of the conditional's type says, and it is the
// neighbour that stays green while a case above reds.
TEST(ConstantConditionalIsBalanced, ArmsOfOneTypeKeepIt) {
    inAChildProcess([] {
        std::string const body =
            "    _Static_assert((1 ? 5 : 7) == 5, \"int and int\");\n"
            "    _Static_assert((1 ? -1 : 0) < 0, \"int and int, signed\");\n"
            "    _Static_assert((0 ? 1u : 2u) - 3 > 0, \"unsigned int and unsigned int\");\n"
            "    _Static_assert((1 ? g.b : g.b) - 6 < 0, \"two bit-fields promoted alike\");\n"
            "    if ((1 ? 5 : 7) - 6 > 0) return y;\n"
            "    if ((1 ? 5 : 7) - 6 < 0) return 7;\n"
            "    return 42;";
        std::string found = analysisErrors(inF(body));
        std::size_t const marks = positionsMarkedNeverReached(inF(body));
        if (marks != 1)
            found += "\n    " + std::to_string(marks) + " position(s) marked never reached, not 1";
        return found;
    });
}
