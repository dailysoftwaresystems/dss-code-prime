// ★★ [[D-CSUBSET-ATTRIBUTE-ARG-CONSTANT-EXPRESSION]] — a CONSTANT EXPRESSION as
// the operand of `__attribute__((aligned(...)))`: the keyword-led primaries
// (`sizeof`, `_Alignof`, `__alignof__`) and the operator forms over them.
//
// ★ ALMOST EVERY PIN HERE ASSERTS THE ALIGNMENT OR A FOLDED SIZE, NEVER MERELY
// THE PARSE — "it stopped erroring" and "it works" are different claims and only
// the second is closure. Each such fixture is built so a DROPPED or MIS-FOLDED
// operand changes the NUMBER rather than leaving a clean compile behind: the
// requested alignment is 16 (or 32) while the decorated object's natural
// alignment is 8, so "the attribute vanished" and "the attribute landed" are
// distinguishable.
// ⚠ THE EXCEPTIONS ARE NAMED RATHER THAN GLOSSED, because "every one" was
// claimed once here and was false: `TypedefWitnessWithASatisfiedRequestCompiles-
// Clean`, `CommaSeparatorAndAssignTailSurviveTheExpression` and
// `MultiArgumentIdentifierClauseIsUnchanged` assert only that nothing errored.
// The first is vacuous in the drop direction ON ITS OWN and is paired with
// `TypedefRequestStricterThanTheAliasFailsLoud`, which is not; the other two are
// regression walls whose subject IS "this still compiles clean".
//
// The witness is real and is why the row exists: the shipped Xcode SDK's
// `MacOSX.sdk/usr/include/libkern/OSAtomicDeprecated.h` writes
// `typedef int64_t __attribute__((__aligned__((sizeof(int64_t))))) …`, and the
// `_Alignof` and typedef-name variants of the same shape.
//
// ★ WHY THE FAIL-LOUD PIN IS NOT AN AFTERTHOUGHT HERE. An un-foldable alignment
// must never degrade to "no alignment" — that is
// [[D-TEST-IGNORE-LIST-IS-A-LICENSE-TO-DROP]]'s failure mode applied to a
// decoration that changes LAYOUT, i.e. a silent miscompile. The refusal is also
// the UNION's answer rather than a DSS quirk: ✔MEASURED, gcc 13.3.0, clang 18.1.3
// and mingw-w64 gcc 13.2.0 each REFUSE an un-foldable operand
// (`requested alignment is not an integer constant` / `'aligned' attribute
// requires integer constant`) and none of them drops it.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using namespace dss;
using namespace dss::sem_test;

namespace {

// The SAME layout params the alignas pins use (Natural scalar alignment, stack
// alignment 16), so member layout and `_Alignof` folds are exact.
constexpr AggregateLayoutParams kLayout{ScalarAlignmentRule::Natural, 16};

// A 16-byte aggregate whose ALIGNMENT is 8. Every fixture below asks for
// `sizeof(struct pair)` (16) on an object whose natural alignment is 8, so the
// two hypotheses "the operand folded" and "the attribute was dropped" produce
// DIFFERENT numbers. `_Alignof(struct pair)` (8) is the second operand shape and
// is deliberately NOT 16 — it discriminates a genuine alignof fold from a
// copy-paste of the sizeof one.
constexpr char const* kPair = "struct pair { long long a; long long b; };\n";

[[nodiscard]] SymbolRecord const* findSym(SemanticModel const& m,
                                          std::string_view name) {
    for (std::size_t i = 1; i < m.symbols().size(); ++i)
        if (m.symbols()[i].name == name) return &m.symbols()[i];
    return nullptr;
}

// ⚠⚠ THROWS ON A PARSE ERROR, AND THAT IS THE WHOLE REASON THIS HELPER EXISTS
// RATHER THAN A BARE `analyze` CALL. Every fixture below is meant to PARSE; a
// mutant that takes the widening away turns them all into parse errors, and
// `analyze` on the resulting RECOVERY tree does not fail — it RAISES
// (`Tree::tokenKind on non-Token node`) and kills the whole test BINARY with
// 0xc0000409, taking every sibling verdict in this file — the GREEN CONTROLS
// included — with it. ✔MEASURED: the first red-on-disable run of this suite died
// at its FIRST test and reported nothing else, which is a transcript with no
// verdict in it. That is the exact failure P58 recorded against 88 sibling pins
// in test_parser_c_smoke.cpp.
// Throwing is the fixture's own established answer to this (`loadShippedSchema`
// throws for the same reason, and says so): GoogleTest reports a throw as a
// failure of that ONE test, so a mutant produces thirteen NAMED failures and the
// controls still speak. The diagnostic codes ride IN the message — a red that
// says only "failed" sends the reader off to guess which fixture broke.
// ⓘ ✔THE ENGINE DEFECT BEHIND THAT ABORT IS NOW FIXED, and this helper is kept
// anyway. `scanNoreturnSpelling` and five sibling specifier scans were reading a
// `NodeKind::Error` node as a Token; `forEachTokenLeafUnder` in
// `src/analysis/semantic/semantic_analyzer.cpp` is the one answer they now share,
// and `tests/corpus/diagnostics/c/attr_arg_malformed_operator_expr.c` pins that a
// recovery tree survives `analyze()`. The helper stays because a fixture that
// does not PARSE cannot support an alignment claim either way, and saying so by
// NAME beats an EXPECT_FALSE(hasErrors) that reports "failed" and sends the
// reader off to guess which fixture broke.
[[nodiscard]] SemanticModel analyzeC(std::string src) {
    auto cu = buildShippedUnit("c", {std::move(src)});
    std::string parseErrors;
    for (auto const& t : cu->trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) {
                parseErrors += "\n  ";
                parseErrors += diagnosticCodeName(d.code);
                parseErrors += " actual='";
                parseErrors += d.actual;
                parseErrors += "'";
            }
    if (!parseErrors.empty())
        throw std::runtime_error(
            "fixture did not PARSE, so its alignment claim cannot be measured:"
            + parseErrors);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64, kLayout);
}

}  // namespace

// ── THE THREE WITNESS SHAPES, each asserting the stored alignment ────────────

// WITNESS SHAPE `aligned(sizeof(T))` — the bare single-paren form.
TEST(AttributeArgConstExpr, SizeofOperandReachesTheStoredAlignment) {
    auto model = analyzeC(std::string{kPair}
                          + "long long v __attribute__((__aligned__(sizeof(struct pair))));\n");
    EXPECT_FALSE(model.diagnostics().hasErrors())
        << "gcc, clang and mingw-w64 gcc all BUILD AND RUN this shape";
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->explicitAlignment.has_value())
        << "parsed-but-unstored is exactly the silent drop this row exists to kill";
    EXPECT_EQ(*v->explicitAlignment, 16u)
        << "16 is sizeof(struct pair); 8 would mean the operand never folded";
}

// WITNESS SHAPE `aligned((sizeof(T)))` — the DOUBLE-paren form the SDK header
// actually writes. The extra parens are a nested attribute-argument GROUP, not a
// parenthesized expression, so this exercises a second descent level.
TEST(AttributeArgConstExpr, DoubleParenSizeofOperandReachesTheStoredAlignment) {
    auto model = analyzeC(std::string{kPair}
                          + "long long v __attribute__((__aligned__((sizeof(struct pair)))));\n");
    EXPECT_FALSE(model.diagnostics().hasErrors());
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->explicitAlignment.has_value());
    EXPECT_EQ(*v->explicitAlignment, 16u);
}

// WITNESS SHAPE `aligned(_Alignof(T))`. The expected number is 8, not 16 —
// `_Alignof(struct pair)` is 8 — so this pin cannot pass by reading the sizeof
// path by mistake. It is discriminating because `char` aligns to 1.
TEST(AttributeArgConstExpr, AlignofOperandReachesTheStoredAlignment) {
    auto model = analyzeC(std::string{kPair}
                          + "char v __attribute__((__aligned__(_Alignof(struct pair))));\n");
    EXPECT_FALSE(model.diagnostics().hasErrors());
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->explicitAlignment.has_value());
    EXPECT_EQ(*v->explicitAlignment, 8u)
        << "_Alignof(struct pair) is 8; 16 would mean this read sizeof";
}

// The GNU `__alignof__` spelling — the one a header that must still compile as
// C89 writes, and therefore the one an SDK actually contains. It is an ALIAS of
// the same keyword kind, so it must arrive at the same fold with no second rule.
TEST(AttributeArgConstExpr, GnuAlignofSpellingReachesTheStoredAlignment) {
    auto model = analyzeC(std::string{kPair}
                          + "char v __attribute__((__aligned__(__alignof__(struct pair))));\n");
    EXPECT_FALSE(model.diagnostics().hasErrors());
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->explicitAlignment.has_value());
    EXPECT_EQ(*v->explicitAlignment, 8u);
}

// A `sizeof` whose operand is a VALUE rather than a type-name. The value form is
// a different branch of the sizeof rule, so admitting only the type form would
// leave this a parse error while looking closed.
TEST(AttributeArgConstExpr, SizeofValueOperandReachesTheStoredAlignment) {
    auto model = analyzeC(std::string{kPair}
                          + "struct pair g;\n"
                            "long long v __attribute__((__aligned__(sizeof g)));\n");
    EXPECT_FALSE(model.diagnostics().hasErrors());
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->explicitAlignment.has_value());
    EXPECT_EQ(*v->explicitAlignment, 16u);
}

// ── THE COMPOSITE SINK, which is a different consumer of the same operand ────

// `struct … { } __attribute__((aligned(sizeof(T))))` goes through the composite
// scan, not the declaration-level one. Both call the same operand locator, so a
// descent that stopped in the wrong place would break BOTH — and only one of them
// would be noticed without this pin. Asserted through `_Static_assert`, so the
// claim is the composite's ACTUAL interned alignment.
TEST(AttributeArgConstExpr, CompositeAlignedTakesAKeywordLedOperand) {
    auto model = analyzeC(
        std::string{kPair}
        + "struct box { char c; } __attribute__((__aligned__(sizeof(struct pair))));\n"
          "_Static_assert(_Alignof(struct box) == 16, \"composite alignment\");\n"
          "_Static_assert(sizeof(struct box) == 16, \"composite size\");\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_StaticAssertFailed), 0u)
        << "an undecorated `struct box` would be _Alignof 1 / sizeof 1";
    EXPECT_FALSE(model.diagnostics().hasErrors());
}

// ── THE TYPEDEF WITNESS, and why its PAIR is the proof ───────────────────────

// The row's literal witness decorates a TYPEDEF. DSS's graded typedef arm accepts
// a request the alias already satisfies (a proven no-op) and REFUSES one it
// cannot deliver, because a typedef interns to the same TypeId as its aliasee.
// The two pins below are one measurement: the second is what proves the operand
// FOLDED TO 16 rather than being dropped, because a dropped operand produces the
// silent acceptance of the first.
TEST(AttributeArgConstExpr, TypedefWitnessWithASatisfiedRequestCompilesClean) {
    auto model = analyzeC(
        "typedef long long __attribute__((__aligned__((sizeof(long long))))) osint64_t;\n"
        "osint64_t v;\n");
    EXPECT_FALSE(model.diagnostics().hasErrors())
        << "sizeof(long long) is 8 and the alias already aligns to 8 — a no-op, "
           "and gcc/clang/mingw all compile the SDK line clean";
}

TEST(AttributeArgConstExpr, TypedefRequestStricterThanTheAliasFailsLoud) {
    auto model = analyzeC(
        std::string{kPair}
        + "typedef long long __attribute__((__aligned__((sizeof(struct pair))))) t16;\n"
          "t16 v;\n");
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasInvalidContext), 1u)
        << "16 > the alias's 8, and DSS cannot represent an over-aligned alias — "
           "this red is what proves the operand folded to 16 instead of vanishing";
}

// ── THE FAIL-LOUD ARM, with its control ─────────────────────────────────────

// A KEYWORD-LED operand the const-evaluator genuinely cannot fold: `sizeof` of a
// VLA whose extent is a runtime value. It must REFUSE, positioned, and it must
// NOT store an alignment — degrading to "no alignment" here is a silent
// miscompile of the layout.
TEST(AttributeArgConstExpr, UnfoldableKeywordLedOperandFailsLoudAndStoresNothing) {
    auto model = analyzeC(
        "int n;\n"
        "void f(void) { int vla[n]; long long v __attribute__((__aligned__(sizeof(vla)))); (void)v; }\n");
    EXPECT_GE(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNonConstant), 1u)
        << "gcc/clang/mingw all refuse this; accept-and-drop would be the "
           "D-TEST-IGNORE-LIST-IS-A-LICENSE-TO-DROP failure mode";
    EXPECT_TRUE(model.diagnostics().hasErrors())
        << "the refusal must be an ERROR — a warning would let the wrong layout ship";
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    EXPECT_FALSE(v->explicitAlignment.has_value())
        << "an un-foldable request must leave NO alignment behind, not a guess";
}

// CONTROL for the pin above: the SAME construct in the SAME position with a
// FOLDABLE keyword-led operand. Without it, "the un-foldable case is red" is
// equally consistent with "this whole construct is refused".
TEST(AttributeArgConstExpr, FoldableOperandInTheSameBlockPositionIsGreen) {
    auto model = analyzeC(
        std::string{kPair}
        + "void f(void) { long long v __attribute__((__aligned__(sizeof(struct pair)))); (void)v; }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AlignasNonConstant), 0u);
    EXPECT_FALSE(model.diagnostics().hasErrors());
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->explicitAlignment.has_value());
    EXPECT_EQ(*v->explicitAlignment, 16u);
}

// ── THE PRE-P62 SURFACE, unchanged ──────────────────────────────────────────

// The integer-literal operand is the shape that already worked. It is pinned here
// as the widening's regression wall: the new alt must not reshape the tree an
// existing attribute argument produces.
TEST(AttributeArgConstExpr, IntegerLiteralOperandIsUnchanged) {
    auto model = analyzeC("long long v __attribute__((__aligned__(16)));\n");
    EXPECT_FALSE(model.diagnostics().hasErrors());
    SymbolRecord const* v = findSym(model, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->explicitAlignment.has_value());
    EXPECT_EQ(*v->explicitAlignment, 16u);
}

// A MULTI-ARGUMENT clause whose arguments are identifiers and integers — the
// shape whose tree the widening must not disturb, and the one that proves the
// operand locator still declines a list rather than folding its first element.
TEST(AttributeArgConstExpr, MultiArgumentIdentifierClauseIsUnchanged) {
    auto model = analyzeC(
        "__attribute__((__format__(__printf__, 1, 2))) int h(char const* f, ...);\n");
    EXPECT_FALSE(model.diagnostics().hasErrors())
        << "the widened alt must not make an identifier argument speculative";
}

// ── THE OPERATOR SURFACE, in BOTH directions ────────────────────────────────

// ⚠⚠ THIS PIN REPLACED ONE THAT ASSERTED THE OPPOSITE, AND THE REPLACEMENT IS
// THE POINT. The first version of this file shipped
// `OperatorExpressionResidueIsSymmetricAndLoud`, which asserted that DSS REFUSES
// `aligned(sizeof(long long) * 2)` and `aligned(2 * 8)` — a GREEN test asserting
// a union violation is correct, and insensitive in the only direction that
// mattered (removing the fix can only refuse MORE, so it stayed green under both
// of that lane's mutants). ✔MEASURED, each reference probed SEPARATELY with a
// foldable-literal CONTROL, BUILD **and** RUN with the alignment asserted:
// gcc 13.3.0 (`-std=gnu17` and `-std=c2x`), clang 18.1.3 (`-std=gnu17` and
// `-std=c23`) and mingw-w64 gcc 13.2.0 all build AND run both constructs to 42
// with zero warnings at `-Wall -Wextra`; only MSVC refuses, and it refuses its
// own `__declspec(align(...))` control-free of `__attribute__` entirely. Three
// working references to one refusal is the same vote count that justified
// admitting `sizeof` in the first place.
//
// The SYMMETRY argument the deleted pin made was right and is kept — a construct
// must not mean two things depending on where it is written — but it is satisfied
// by ACCEPTING both, not by refusing both.
TEST(AttributeArgConstExpr, OperatorExpressionOperandsAreSymmetricAndFold) {
    auto keywordLed = analyzeC(
        std::string{kPair}
        + "long long v __attribute__((__aligned__(sizeof(struct pair) * 2)));\n");
    EXPECT_FALSE(keywordLed.diagnostics().hasErrors());
    SymbolRecord const* v = findSym(keywordLed, "v");
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->explicitAlignment.has_value())
        << "gcc, clang and mingw-w64 gcc all BUILD AND RUN this shape";
    EXPECT_EQ(*v->explicitAlignment, 32u)
        << "sizeof(struct pair) is 16, so the request is 32";

    auto literalLed = analyzeC(
        "long long w __attribute__((__aligned__(2 * 8)));\n");
    EXPECT_FALSE(literalLed.diagnostics().hasErrors());
    SymbolRecord const* w = findSym(literalLed, "w");
    ASSERT_NE(w, nullptr);
    ASSERT_TRUE(w->explicitAlignment.has_value())
        << "the literal-led form must be admitted the SAME way as the "
           "keyword-led one, or one construct means two things depending on "
           "where it is written";
    EXPECT_EQ(*w->explicitAlignment, 16u);

    // The mirror image of the keyword-led form — the operator on the OTHER side.
    auto mirrored = analyzeC(
        std::string{kPair}
        + "long long x __attribute__((__aligned__(2 * sizeof(struct pair))));\n");
    EXPECT_FALSE(mirrored.diagnostics().hasErrors());
    SymbolRecord const* x = findSym(mirrored, "x");
    ASSERT_NE(x, nullptr);
    ASSERT_TRUE(x->explicitAlignment.has_value());
    EXPECT_EQ(*x->explicitAlignment, 32u);
}

// ★★ PRECEDENCE IS THE SILENT-MISCOMPILE AXIS OF THIS WHOLE CHANGE, and it is
// why the operand is a real expression rule rather than an operator TAIL on the
// argument item. A flat left-to-right tail folds `2 + 2 * 3` to 12 and `1 << 2 <<
// 1` to 8 by luck; the answers are 8 and 8, and a wrong ALIGNMENT is a wrong
// LAYOUT that compiles clean. Reusing the language's one Pratt walker makes
// precedence and associativity a property of the shared operator table.
// ✔MEASURED: gcc 13.3.0 and clang 18.1.3 both build and RUN every row below to
// the same numbers.
TEST(AttributeArgConstExpr, OperatorPrecedenceMatchesTheSharedOperatorTable) {
    struct Row { char const* expr; std::uint32_t want; char const* why; };
    constexpr Row rows[] = {
        {"2 + 2 * 3",  8u,  "8, not 12 — `*` binds tighter than `+`"},
        {"1 << 2 << 1", 8u, "left-associative shift: (1<<2)<<1"},
        {"64 / 2 / 2", 16u, "left-associative division: (64/2)/2"},
        {"(2 + 2) * 4", 16u, "explicit parens must still group"},
        {"1 ? 8 : 32",  8u,
         "the conditional operator IS in a constant-expression (C 6.6p1)"},
    };
    for (Row const& r : rows) {
        SCOPED_TRACE(r.expr);
        auto model = analyzeC(std::string{"long long v __attribute__((__aligned__("}
                              + r.expr + ")));\n");
        EXPECT_FALSE(model.diagnostics().hasErrors());
        SymbolRecord const* v = findSym(model, "v");
        ASSERT_NE(v, nullptr);
        ASSERT_TRUE(v->explicitAlignment.has_value());
        EXPECT_EQ(*v->explicitAlignment, r.want) << r.why;
    }
}

// ── THE THIRD ATTRIBUTE SURFACE THE SAME OPERAND LOCATOR FEEDS ──────────────

// `constructor(N)` / `destructor(N)` read their PRIORITY through the SAME
// `attrClauseArgOperand` locator, and unlike the two alignment callers they hand
// the located node straight to `constIntExpr` with no one-level unwrap of their
// own. So the descent stop has to be right for a caller that does NOT unwrap, and
// nothing pinned that. ✔MEASURED against both references: gcc 13.3.0 and
// clang 18.1.3 build and RUN `constructor(sizeof(struct big))` beside
// `constructor(sizeof(struct big) + 100)` with the two initializers running in
// priority order.
TEST(AttributeArgConstExpr, ConstructorPriorityTakesAConstantExpression) {
    auto model = analyzeC(
        "struct big { char pad[200]; };\n"
        "__attribute__((constructor(sizeof(struct big)))) void early(void) {}\n"
        "__attribute__((constructor(sizeof(struct big) + 100))) void late(void) {}\n");
    EXPECT_FALSE(model.diagnostics().hasErrors());
    SymbolRecord const* early = findSym(model, "early");
    SymbolRecord const* late  = findSym(model, "late");
    ASSERT_NE(early, nullptr);
    ASSERT_NE(late, nullptr);
    ASSERT_TRUE(early->staticInit.beforeEntry().has_value())
        << "a priority that failed to fold would leave the function out of the "
           "schedule or at the UNPRIORITIZED sentinel — both silently reorder it";
    ASSERT_TRUE(late->staticInit.beforeEntry().has_value());
    EXPECT_EQ(*early->staticInit.beforeEntry(), 200u);
    EXPECT_EQ(*late->staticInit.beforeEntry(), 300u);
}

// ── THE TWO OPERATOR LEVELS THE ARGUMENT RULE MUST NOT SWALLOW ──────────────

// The operand climbs from precedence 16 (the ternary's), i.e. exactly C 6.6p1's
// `constant-expression: conditional-expression`. Below that floor sit the two
// operators this grammar needs to keep for itself: `=` (15) is the clang
// keyword-argument tail and `,` (10) is the argument list's separator. If the
// expression ever swallowed either, THESE two shapes would change meaning —
// silently, because both still parse.
TEST(AttributeArgConstExpr, CommaSeparatorAndAssignTailSurviveTheExpression) {
    // The comma is a SEPARATOR: three arguments, not one comma-expression.
    auto multi = analyzeC(
        "__attribute__((__format__(__printf__, 1, 2))) int h(char const* f, ...);\n");
    EXPECT_FALSE(multi.diagnostics().hasErrors())
        << "a comma folded into the expression would turn three arguments into "
           "one and the format check would read the wrong indices";

    // The `=` is the TF-C72 keyword-argument tail, not an assignment operator.
    auto avail = analyzeC(
        "__attribute__((__availability__(swift, unavailable, message=\"gone\")))\n"
        "int gone_fn(void);\n");
    EXPECT_FALSE(avail.diagnostics().hasErrors())
        << "the `message=` tail must still be the item's own tail";

    // And an operator argument inside a MULTI-argument clause must still leave the
    // alignment locator declining a list rather than folding its first element.
    auto listed = analyzeC(
        "__attribute__((__alloc_size__(1 * 1, 2))) void* a(unsigned, unsigned);\n");
    EXPECT_FALSE(listed.diagnostics().hasErrors());
}
