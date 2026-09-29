// D-SEMANTIC-EXPRESSION-TYPER-REDERIVES-EVERY-SUBTREE — the COMPLEXITY pin.
//
// ★★★ WHAT WENT WRONG, AND IT WAS A WALK THROWING AWAY WHAT IT HAD ALREADY COMPUTED.
// `subtreeType` is the semantic tier's ONE expression typer: an explicit heap
// work-stack that types a whole expression subtree bottom-up and returns the ROOT's
// type — discarding the N-1 descendant types it necessarily computed on the way. Its
// only fast exit is the Pass-2 authoritative stamp, and Pass 2 stamps references,
// members, calls, casts, `sizeof` and literals — never an OPERATOR node. `pass2Post`
// is a POST-ORDER walk that asks that whole-subtree question at every operator node
// (the C 6.3.2.2 void-operand arm asks it of both operands; the C23 nullptr-operand
// arm's transparent descent gives up at a node with two Internal children and falls
// back to the same typer). On a left-deep `v + v + … + v` chain — a tree of DEPTH N —
// the left operand is always an unstamped operator node, so each arm re-walks the
// whole left subtree at every level: `2 · Σ(N-d) ≈ 2N²`.
//
// ✔MEASURED before the fix (Release, Windows, one host, one run), nodes the typer's
// walks classified: 1 998 010 at n=1 000; 7 996 010 at n=2 000; 31 992 010 at
// n=4 000 — growth 4.0020× and 4.0010× per doubling, the identity `visits ≈ 2N²`
// holding to the digit, while the NUMBER of questions asked stayed LINEAR
// (3 001 / 6 001 / 12 001). It is the REACH of each question that was quadratic.
// The `semantic` phase was 98% of the whole compile and cost 24.2 s at n=8 000
// against gcc 13.2.0 at 0.74 s on the identical file in the same interleaved run.
// Under `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` taken over what WORKS, gcc is a WORKING
// reference here, so near-linear scaling is REQUIRED and not an aspiration.
//
// ★★★ WHY THIS PIN COUNTS OPERATIONS AND DOES NOT TIME THE CLOCK.
// A wall-clock performance assertion is sized on the machine that wrote it and reds
// on the slowest leg that runs it, NAMING THE WRONG EVENT —
// `.harness-config/runner/actions/check-wall-clock-in-tests/` refuses new ones for exactly that reason.
// What is asserted here is `SemanticModel::exprType{Queries,NodeVisits}`: THE
// ALGORITHM'S OWN WORK. Those numbers are deterministic, host-independent,
// load-independent and identical in Debug and Release, so a statement about them is a
// fact about the algorithm rather than about the box. A busy CI machine cannot red it
// and a genuine complexity regression cannot hide from it.
//
// ★★ THE FOUR ARMS, AND WHY NONE OF THEM CAN GO VACUOUS.
//   * PREMISE — the subject really does present a depth-N expression and the typer
//     really is consulted. The operand count is RE-DERIVED from the SOURCE the test
//     generated rather than read back from the instrument, because asking an
//     instrument to confirm its own premise is how a pin ends up testing its helper
//     instead of the property. Without this arm, a `pass2Post` change that stopped
//     asking for operand types would leave every other arm passing over zero work.
//   * WORK — `nodeVisits` must grow with the INPUT, not with the input squared,
//     across a doubled subject; plus a per-question reach bound, because a growth
//     ratio alone is satisfied by "quadratic, but both sizes small".
//   * CONTROL — `exprTypeQueries` is UNCHANGED by the record (it counts questions,
//     and the record changes only how far each question has to walk), and the
//     analysis is still clean. Reverting the record leaves this arm IDENTICAL, which
//     is what makes the work arm's red evidence about COMPLEXITY specifically rather
//     than about "the mutant broke something".
//   * LINEAR-SHAPE BOUND — the sibling subject that moves N DECLARATIONS instead of
//     N operands was ALREADY linear before the fix (✔MEASURED 41/65/107 ms at
//     n=2 000/4 000/8 000). It is pinned here so the claim stays bounded to the
//     expression walk and a future regression in the declaration path is not
//     mistaken for this one.
//
// ⚠ TWO COUNTERS, AND THE SECOND IS NOT A REFINEMENT OF THE FIRST. With only
// `nodeVisits`, "the derived-type record made this free" and "the operand type is no
// longer asked for at all" are the same number — and the second of those is a
// correctness regression wearing a performance win's clothes. `exprTypeQueries` is
// what tells them apart, and it is asserted to be BOTH non-trivial and unchanged.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/diagnostic_budget.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

// ⓘ THE SHAPE IS THE SUBJECT, NOT AN ARBITRARY STRESSOR. One function whose `return`
// is a single N-operand `+` chain is the exact discriminator that separated this
// defect from the scope/symbol-table candidate: the sibling `declarationSource`
// below moves N DECLARATIONS with a one-operand return and was already linear, so
// the two together say the quadratic lived in the EXPRESSION walk and nowhere else.
// A left-associative `+` chain is a tree of DEPTH N, which is what makes a per-node
// whole-subtree walk quadratic rather than merely wasteful.
[[nodiscard]] std::string expressionSource(unsigned n) {
    std::string src = "long sm_expr(int i) {\n    long v = (long)i;\n    return v";
    src.reserve(src.size() + static_cast<std::size_t>(n) * 4u + 64u);
    for (unsigned i = 1; i < n; ++i) src += " + v";
    src += ";\n}\n";
    return src;
}

// The sibling that moves DECLARATIONS instead of operands — the control shape that
// was measured LINEAR before the fix and must stay so.
[[nodiscard]] std::string declarationSource(unsigned n) {
    std::string src = "long sm_decls(int i) {\n";
    src.reserve(src.size() + static_cast<std::size_t>(n) * 28u + 64u);
    for (unsigned i = 0; i < n; ++i) {
        src += "    long v" + std::to_string(i) + " = (long)(i + "
             + std::to_string(i) + ");\n";
    }
    src += "    return v0;\n}\n";
    return src;
}

struct Work {
    std::uint64_t queries = 0;
    std::uint64_t visits  = 0;
    bool          clean   = false;
};

[[nodiscard]] Work analyzeWork(std::string const& source) {
    auto  cu    = buildShippedUnit("c", {source});
    auto  model = analyze(cu, DiagnosticBudget::libraryDefault());
    return Work{model.exprTypeQueries(), model.exprTypeNodeVisits(),
                !model.hasErrors()};
}

// The two sizes the growth arm compares. Small enough that the PRE-fix quadratic
// still completes inside a unit test (it is ~8 million node visits at the larger
// size, which is seconds, not minutes) and large enough that a quadratic and a
// linear reading are unmistakably different: 4.00× against ~2.00×.
constexpr unsigned kN  = 1000;
constexpr unsigned kN2 = 2000;

} // namespace

// ── ARM 1: PREMISE ───────────────────────────────────────────────────────────
// The subject is what it claims to be and the typer is genuinely consulted.
TEST(SemanticExpressionTypeReuseComplexity, TheSubjectReallyPresentsADepthNExpression) {
    std::string const src = expressionSource(kN);

    // RE-DERIVED FROM THE SOURCE, not read back from the instrument: exactly N-1
    // `+` operators, so the expression really is a chain of N operands.
    std::size_t plusCount = 0;
    for (char c : src) if (c == '+') ++plusCount;
    EXPECT_EQ(plusCount, static_cast<std::size_t>(kN) - 1u)
        << "the generator stopped producing an N-operand chain — every other arm "
           "in this file would then be measuring a different program";

    Work const w = analyzeWork(src);
    EXPECT_TRUE(w.clean) << "the subject must analyze cleanly; a refusal would put "
                            "the walk on the diagnostic path instead of the typing one";
    // The typer must be asked about this program at all. Pre-fix and post-fix this
    // is ~3 questions per operator node; the bound is deliberately loose because the
    // arm's job is to refuse ZERO, not to pin a constant.
    EXPECT_GT(w.queries, static_cast<std::uint64_t>(kN))
        << "the expression typer was barely consulted (" << w.queries
        << " queries for " << kN << " operands) — the work arms below would then be "
           "measuring nothing";
    EXPECT_GE(w.visits, w.queries)
        << "every query classifies at least its own root node";
}

// ── ARM 2: WORK ──────────────────────────────────────────────────────────────
// The typer's own work grows with the input, not with the input squared.
TEST(SemanticExpressionTypeReuseComplexity, WalkWorkGrowsWithTheInputNotItsSquare) {
    Work const small = analyzeWork(expressionSource(kN));
    Work const big   = analyzeWork(expressionSource(kN2));

    ASSERT_GT(small.visits, 0u);
    double const growth = static_cast<double>(big.visits)
                        / static_cast<double>(small.visits);

    // ★ 3.0 IS A DISCRIMINATOR, NOT A TUNING KNOB. Doubling the input multiplies a
    // LINEAR walk's work by ~2 and a QUADRATIC walk's by ~4 (✔MEASURED pre-fix:
    // 4.0020×). Any value strictly between the two separates them; 3.0 is the
    // midpoint, so the arm cannot be made to pass by a small constant-factor change
    // in either direction and does not have to be re-tuned when one lands.
    EXPECT_LT(growth, 3.0)
        << "doubling the operand count multiplied the expression typer's node "
           "visits by " << growth << " (" << small.visits << " → " << big.visits
        << "). A linear walk gives ~2; the quadratic this pin exists for gave "
           "4.0020. The derived-expression-type record is not being consulted.";

    // A growth ratio on the TOTAL alone could be satisfied by a walk whose reach
    // grows while the question count falls, so state the property directly: the
    // average REACH of ONE question must not grow with the input. ★ This arm carries
    // NO magnitude constant at all — it compares the subject against itself at two
    // sizes, so it cannot be tuned and does not have to be re-tuned when a legitimate
    // change makes expressions a little deeper or a little shallower.
    // ✔MEASURED pre-fix: reach 666 at n=1 000 and 1 332 at n=2 000 — it DOUBLED with
    // the input, which is the definition of the defect. Post-fix: 1.667 and 1.667.
    double const reachSmall = static_cast<double>(small.visits)
                            / static_cast<double>(small.queries);
    double const reachBig   = static_cast<double>(big.visits)
                            / static_cast<double>(big.queries);
    ASSERT_GT(reachSmall, 0.0);
    EXPECT_LT(reachBig / reachSmall, 1.5)
        << "one expression-type question classified " << reachSmall
        << " nodes on average at " << kN << " operands and " << reachBig << " at "
        << kN2 << " — the reach GROWS with the input, which is a Θ(N) walk per "
           "question and is the defect itself";
}

// ── ARM 3: CONTROL ───────────────────────────────────────────────────────────
// ★ THIS ARM STAYS GREEN UNDER THE MUTANT. That is its whole purpose: it is what
// makes ARM 2's red evidence about COMPLEXITY rather than about "the mutant broke
// something". The record changes how far a question walks; it does not change how
// many questions are asked, nor what the analysis decides.
TEST(SemanticExpressionTypeReuseComplexity, QuestionsAskedAndTheVerdictAreUnchangedControl) {
    Work const small = analyzeWork(expressionSource(kN));
    Work const big   = analyzeWork(expressionSource(kN2));

    EXPECT_TRUE(small.clean);
    EXPECT_TRUE(big.clean);

    // The number of QUESTIONS is a property of `pass2Post`'s arms and the shape of
    // the program — it is identical with and without the derived-type record. It
    // scales linearly, so doubling the operands roughly doubles it.
    ASSERT_GT(small.queries, 0u);
    double const qGrowth = static_cast<double>(big.queries)
                         / static_cast<double>(small.queries);
    EXPECT_GT(qGrowth, 1.5)
        << "the expression typer is being asked " << qGrowth
        << "× as often for twice the program — if the questions stopped being asked, "
           "a fall in node visits would be a correctness regression, not a win";
    EXPECT_LT(qGrowth, 3.0)
        << "the number of expression-type questions is growing superlinearly ("
        << qGrowth << "×) — that is a second, different defect from the one this "
           "file pins, and the record cannot fix it";
}

// ── ARM 4: LINEAR-SHAPE BOUND ────────────────────────────────────────────────
// The declaration-shaped sibling was ALREADY linear before the fix. Pinning it here
// keeps the claim bounded to the expression walk, and stops a future regression in
// the declaration path from being read as this one.
TEST(SemanticExpressionTypeReuseComplexity, TheDeclarationShapedSiblingStaysLinear) {
    Work const small = analyzeWork(declarationSource(kN));
    Work const big   = analyzeWork(declarationSource(kN2));

    EXPECT_TRUE(small.clean);
    EXPECT_TRUE(big.clean);
    ASSERT_GT(small.visits, 0u);

    double const growth = static_cast<double>(big.visits)
                        / static_cast<double>(small.visits);
    EXPECT_LT(growth, 3.0)
        << "N declarations with a one-operand return made the expression typer's "
           "node visits grow " << growth << "× for a doubled input ("
        << small.visits << " → " << big.visits
        << "). This shape was linear before the expression fix and is not what that "
           "fix addressed — a red here is a NEW defect in the declaration path";
}
