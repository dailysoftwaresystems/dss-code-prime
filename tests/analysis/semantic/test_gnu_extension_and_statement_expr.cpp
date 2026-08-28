// P31 (2026-08-24) — the two GNU constructs this lane landed, pinned where the
// two runnable corpus examples structurally CANNOT pin them:
//
//   * [[D-C-GNU-EXTENSION-KEYWORD]]     `__extension__` as a declaration prefix
//                                       and as a unary expression prefix
//   * [[D-C-GNU-STATEMENT-EXPRESSION]]  `({ stmt… expr; })`
//
// ★★ WHY A UNIT SUITE BESIDE TWO RUNNABLE EXAMPLES, RATHER THAN INSTEAD OF THEM.
// A corpus example must BUILD, so it can only exercise the ACCEPTING half. Both
// constructs have a refusing half that carries the whole design:
//
//   * `__extension__` is a PREFIX, not a declaration specifier. The ONLY thing
//     that distinguishes those two readings is what they REFUSE — every
//     accepting shape passes under both. ✔MEASURED gcc 13.3.0 AND clang 19.1.7,
//     one translation unit per case so no failure masks the next:
//     `static __extension__ int g;`, `int __extension__ g;`,
//     `_Alignas(8) __extension__ int x;` and
//     `__attribute__((aligned(4))) __extension__ int x;` are refused by BOTH,
//     while the same word LEADING each of those declarations is accepted by
//     both. Without the negatives below, the specifier reading — the shape this
//     repository reaches for first, and the one every other GNU dunder alias
//     took — passes the whole example corpus.
//   * a statement expression whose last statement is not an expression statement
//     has type `void`; using it as a VALUE is refused by both references
//     (`void value not ignored as it ought to be` / `initializing 'int' with an
//     expression of incompatible type 'void'`), and at FILE scope the construct
//     is refused by both outright (`braced-group within expression allowed only
//     inside a function` / `statement expression not allowed at file scope`).
//     Both refusals are DSS's too, and "the build failed" is not the property
//     worth pinning — "it failed with THIS code, for THIS reason" is.
//
// ★★ THE THIRD THING THIS FILE OWNS IS THE DATA MODEL. `__extension__` is
// transparent, which is exactly why a per-target pin is worth having: a constant
// folded THROUGH the prefix must equal the constant folded without it, under
// BOTH data models. `sizeof(long)` is the one scalar that disagrees (LP64 8 /
// LLP64 4), so the same source is folded under both here — an answer that
// silently changed with the target and said nothing is the worst case, and the
// front end runs PER TARGET, so this is measurable rather than assumed.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

// The layout params every const-expr pin in this directory uses. `analyze()`'s
// direct-API default supplies NO `aggregateLayout`, and without one a fold
// silently declines — so a pin expecting a refusal would pass for the wrong
// reason.
[[nodiscard]] SemanticModel analyzeAt(std::string const& src, DataModel dm) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), dm,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

[[nodiscard]] SemanticModel analyzeLp64(std::string const& src) {
    return analyzeAt(src, DataModel::Lp64);
}

// Every Error-severity diagnostic the whole pipeline produced for this source —
// the tree builder's PARSE errors included. A `__extension__` written in a
// position the references refuse dies in the PARSER, so a pin that only counted
// semantic codes would report "no error" for a source that never parsed.
[[nodiscard]] std::size_t parseErrorCount(CompilationUnit const& cu) {
    std::size_t n = 0;
    for (auto const& t : cu.trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

[[nodiscard]] std::size_t parseErrorsFor(std::string const& src) {
    return parseErrorCount(*buildShippedUnit("c", {src}));
}

// The dimension the analyzer folded for the array named `name`; nullopt when the
// fold DECLINED (so these assertions detect a silent decline, not only a wrong
// value). ⚠ `model.symbols()` is ONE FLAT LIST across every scope, so this scans
// for the unique ARRAY-typed match rather than taking the first name hit.
[[nodiscard]] std::optional<std::int64_t>
foldedArrayDim(SemanticModel const& model, std::string_view name) {
    auto const& ti = model.lattice().interner();
    std::optional<std::int64_t> found;
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name != name) continue;
        TypeId const t = model.symbols()[i].type;
        if (!t.valid() || ti.kind(t) != TypeKind::Array) continue;
        auto const sc = ti.scalars(t);
        if (sc.empty()) continue;
        if (found.has_value() && *found != sc[0]) return std::nullopt;  // ambiguous
        found = sc[0];
    }
    return found;
}

// ★ THE STATEMENT-EXPRESSION REFUSALS FIRE AT THE **HIR LOWERING** TIER, NOT IN
// `analyze()`, AND THAT IS DELIBERATE: whether a value is REQUIRED at a given
// position is a property of the lowering position (a discard position is legal
// and an initializer position is not), and the lowering is the tier that knows
// which one it is. So a pin on those codes must run `lowerToHir` too — reading
// `model.diagnostics()` alone reports ZERO for a source that is in fact refused,
// which is a GREEN test asserting nothing. ⚠ This bit once already: both
// statement-expression refusal pins passed `analyze()` only, came back green-
// looking as "count == 0", and had to be corrected against the CLI, which had
// been refusing the same two sources with S006F/S0070 all along.
//
// Run the FULL front end (analyze → CST→HIR) and hand back the lowering
// reporter, which is where an H_/S_ code emitted by the lowering lands.
[[nodiscard]] std::size_t
loweringCodeCount(std::string const& src, DiagnosticCode code) {
    auto cu = buildShippedUnit("c", {src});
    SemanticModel model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
    DiagnosticReporter reporter{};
    auto lowered = lowerToHir(model, reporter);
    (void)lowered;
    return countCode(reporter, code) + countCode(model.diagnostics(), code);
}

// Does ANY symbol carry this name? The discriminator for "did the declaration
// survive the wrapper" — a prefix rule that parsed and then dropped its
// declaration is silent everywhere else.
[[nodiscard]] bool bindsName(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == name) return true;
    return false;
}

} // namespace


// ── [[D-C-GNU-EXTENSION-KEYWORD]] — the ACCEPTED positions ───────────────────

// The four declaration positions, in one unit. The assertion is not "it parsed"
// but "the declaration is STILL THERE": a transparent wrapper that swallowed its
// child would parse perfectly and bind nothing.
TEST(GnuExtensionKeyword, PrefixesEveryDeclarationPositionAndKeepsTheDeclaration) {
    std::string const src =
        "__extension__ typedef long long ext_ll;\n"
        "__extension__ static int ext_file;\n"
        "struct S { int a; __extension__ ext_ll ext_member; };\n"
        "int main(void){ __extension__ int ext_local = 0; return ext_local; }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u) << "every position is accepted by gcc and clang";
    auto model = analyzeLp64(src);
    EXPECT_TRUE(bindsName(model, "ext_file"))
        << "the file-scope declaration must survive the prefix wrapper";
    EXPECT_TRUE(bindsName(model, "ext_member"))
        << "the struct MEMBER must survive — a lost member is silent everywhere else";
    EXPECT_TRUE(bindsName(model, "ext_local"))
        << "the block-scope declaration must survive the prefix wrapper";
}

// ★ THE TYPEDEF-NAME-LED BLOCK DECLARATION, and it is its own pin because it was
// a REAL BUG this lane shipped and the corpus example caught: a block-scope
// declaration whose type is a typedef NAME is `identVarDecl`, which lives under
// `declOrExprStmt` and NOT under `declOrAttrStmt` — so an inner alt copied from
// the latter silently omits exactly the shape glibc writes most
// (`__extension__ __quad_t x;`). It failed `P0001 expected 'EndStatement' — got
// 'lu'` and no other pin here noticed.
TEST(GnuExtensionKeyword, PrefixesABlockDeclarationWhoseTypeIsATypedefName) {
    std::string const src =
        "typedef unsigned ext_u;\n"
        "int main(void){ __extension__ ext_u v = 4; return (int)v - 4; }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
    EXPECT_TRUE(bindsName(analyzeLp64(src), "v"));
}

// Repetition. The rule is SELF-RECURSIVE rather than a `{repeat}` of the token,
// because the prefix of a declaration is itself a declaration. ✔MEASURED
// accepted by gcc 13.3.0 and clang 19.1.7 in all three positions.
TEST(GnuExtensionKeyword, RepeatsInDeclarationAndExpressionPosition) {
    std::string const src =
        "__extension__ __extension__ typedef int ext_i;\n"
        "int main(void){ __extension__ __extension__ int a = 0;\n"
        "                return a + (__extension__ __extension__ 0); }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
}

// ★ THE OPERAND IS AN ASSIGNMENT-EXPRESSION, NOT AN `expression`, AND THIS PIN IS
// THE ONE THAT CAN TELL. Had the rule bound `expression`, the comma below would
// be read as the COMMA OPERATOR and `two_args` would be called with ONE argument
// — a program that compiles, runs, and returns the wrong number. The call binds
// two parameters, so the wrong reading shows up as an arity diagnostic here.
TEST(GnuExtensionKeyword, ExpressionPrefixDoesNotSwallowAnArgumentComma) {
    std::string const src =
        "static int two_args(int a, int b){ return a + b; }\n"
        "int main(void){ return two_args(__extension__ 1, 2) - 3; }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeLp64(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_ArgCountMismatch), 0u)
        << "an `expression` operand would make this ONE argument, silently";
}

// `__extension__ x = 0;` is an EXPRESSION statement, not a declaration — the
// case that forces the declaration reading and the expression reading to share
// one speculative site. The discriminator is that NO NEW SYMBOL is bound: a
// grammar that read it as a declaration would mint a second `x`.
TEST(GnuExtensionKeyword, PrefixedAssignmentIsAnExpressionStatementNotADeclaration) {
    std::string const src =
        "int main(void){ int x = 1; __extension__ x = 0; return x; }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u)
        << "gcc and clang both accept this as `(__extension__ x) = 0`";
    auto model = analyzeLp64(src);
    std::size_t xs = 0;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "x") ++xs;
    EXPECT_EQ(xs, 1u) << "reading it as a declaration would mint a SECOND `x`";
}


// ── [[D-C-GNU-EXTENSION-KEYWORD]] — the REFUSED positions ────────────────────
//
// Each of these is refused by gcc 13.3.0 AND clang 19.1.7 (✔MEASURED, one TU
// each). They are the whole reason the construct is a prefix rule and not an
// entry in `singleDeclSpecifier`'s alt — every ACCEPTING shape above passes
// under the specifier reading too.

TEST(GnuExtensionKeyword, RefusedAfterAStorageClass) {
    EXPECT_GT(parseErrorsFor("static __extension__ int g;\n"
                             "int main(void){ return g; }\n"), 0u)
        << "gcc: `expected identifier or '(' before '__extension__'`";
}

TEST(GnuExtensionKeyword, RefusedAfterATypeSpecifier) {
    EXPECT_GT(parseErrorsFor("int __extension__ g;\n"
                             "int main(void){ return g; }\n"), 0u);
}

TEST(GnuExtensionKeyword, RefusedAfterAMemberAlignmentSpecifier) {
    EXPECT_GT(parseErrorsFor("struct S { _Alignas(8) __extension__ int x; };\n"
                             "int main(void){ return 0; }\n"), 0u)
        << "the member LEAD position is the only one either reference admits";
}

TEST(GnuExtensionKeyword, RefusedAfterAMemberAttribute) {
    EXPECT_GT(parseErrorsFor("struct S { __attribute__((aligned(4))) __extension__ int x; };\n"
                             "int main(void){ return 0; }\n"), 0u)
        << "the reverse order (`__extension__` first) is accepted — see the corpus example";
}

// It prefixes a DECLARATION, never a statement. Both references refuse these.
TEST(GnuExtensionKeyword, RefusedBeforeANonDeclarationStatement) {
    EXPECT_GT(parseErrorsFor("int main(void){ int x=0; __extension__ if(x){x=1;} return x; }\n"), 0u);
    EXPECT_GT(parseErrorsFor("int main(void){ int x=0; __extension__ return x; }\n"), 0u);
}


// ── [[D-C-GNU-EXTENSION-KEYWORD]] — the DATA MODEL ───────────────────────────

// ★ TRANSPARENCY IS A NUMBER, AND IT IS PINNED UNDER BOTH DATA MODELS. The
// prefix must not change the constant it precedes; `sizeof(long)` is the one
// scalar the two models disagree about (LP64 8 / LLP64 4), so a fold that
// silently normalised to one model — or that lost the operand and answered a
// default — shows up here as a wrong dimension rather than as silence.
TEST(GnuExtensionKeyword, PrefixedConstantFoldsIdenticallyUnderBothDataModels) {
    std::string const src =
        "int with_prefix[__extension__ sizeof(long)];\n"
        "int no_prefix[sizeof(long)];\n"
        "int main(void){ return 0; }\n";
    auto lp64 = analyzeAt(src, DataModel::Lp64);
    EXPECT_EQ(foldedArrayDim(lp64, "with_prefix"), 8);
    EXPECT_EQ(foldedArrayDim(lp64, "with_prefix"), foldedArrayDim(lp64, "no_prefix"))
        << "the prefix is transparent: it must fold to what the bare form folds to";
    auto llp64 = analyzeAt(src, DataModel::Llp64);
    EXPECT_EQ(foldedArrayDim(llp64, "with_prefix"), 4);
    EXPECT_EQ(foldedArrayDim(llp64, "with_prefix"), foldedArrayDim(llp64, "no_prefix"));
}


// ── [[D-C-GNU-STATEMENT-EXPRESSION]] — the REFUSALS ──────────────────────────

// ★ REFUSAL 1 — a `void` statement expression used as a VALUE. Its last
// statement is a DECLARATION, so it has no value at all; gcc says `void value
// not ignored as it ought to be` and clang says `initializing 'int' with an
// expression of incompatible type 'void'`. Refusing is not a DSS-only narrowing,
// and a fabricated 0 here would build and run and be wrong.
TEST(GnuStatementExpression, VoidValuedFormIsRefusedWhereAValueIsRequired) {
    EXPECT_EQ(loweringCodeCount("int main(void){ int x = ({ int a = 1; }); return x; }\n",
                                DiagnosticCode::S_StatementExprHasNoValue), 1u)
        << "the last statement is a declaration, so the construct has no value";
}

// The SAME construct in a DISCARD position is accepted by both references and is
// accepted here — the refusal is about the VALUE, never about the construct.
// This is the pin that stops the refusal above from being over-broad.
TEST(GnuStatementExpression, VoidValuedFormIsAcceptedInADiscardPosition) {
    std::string const src =
        "int main(void){ int k = 1; ({ int a = 2; k += a; }); return k - 3; }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 0u)
        << "gcc and clang both accept `({ int a = 1; });` as a statement";
}

// An EMPTY body is the same shape with nothing in it, and both references accept
// it as a statement. It is its own pin because an implementation that reached
// for "the last child" without checking for zero children crashes here rather
// than refusing.
TEST(GnuStatementExpression, EmptyBodyIsAcceptedInADiscardPosition) {
    std::string const src = "int main(void){ ({ }); return 0; }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 0u);
}

// ★ REFUSAL 2 — at FILE scope. Both references refuse it, and the reason DSS
// must too is not conformance but the shape of the alternative: folding
// `int g = ({ f(); 1; });` to its last literal would silently DISCARD the call.
TEST(GnuStatementExpression, RefusedAtFileScope) {
    std::string const src = "int g = ({ 1; });\nint main(void){ return g - 1; }\n";
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprAtFileScope), 1u)
        << "there is no frame at file scope to run the statements in";
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 0u)
        << "the file-scope refusal must fire BEFORE the value question is asked, "
           "so one construct produces one diagnostic and not two";
}


// ── [[D-C-GNU-STATEMENT-EXPRESSION]] — the ACCEPTED shapes ───────────────────

// The scope. `stmtExpr` is a `semantics.scopes` rule for the same reason `block`
// is: two uses of one macro in one function must not collide. The discriminator
// is that the SECOND use's `_a` is a different symbol — with no scope of its own
// this source is a redeclaration.
TEST(GnuStatementExpression, EachBodyGetsItsOwnScope) {
    std::string const src =
        "int main(void){\n"
        "  int p = ({ int _a = 1; _a; });\n"
        "  int q = ({ int _a = 2; _a; });\n"
        "  return p + q - 3;\n"
        "}\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeLp64(src);
    EXPECT_EQ(countCode(model.diagnostics(), DiagnosticCode::S_RedeclaredSymbol), 0u)
        << "without a scope per body the second `_a` redeclares the first";
    std::size_t as = 0;
    for (std::size_t i = 1; i < model.symbols().size(); ++i)
        if (model.symbols()[i].name == "_a") ++as;
    EXPECT_EQ(as, 2u) << "two bodies, two distinct `_a` symbols";
}

// The glibc spelling. The two constructs are ONE cycle's work because seven
// sites in /usr/include write them together (`math.h` ×6, `assert.h` ×1) —
// landing either alone leaves those headers still refused.
TEST(GnuStatementExpression, CarriesTheExtensionPrefixTheWayGlibcWritesIt) {
    std::string const src =
        "int main(void){ return __extension__ ({ int a = 0; a; }); }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 0u);
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprAtFileScope), 0u);
}

// ★ THE `commitAfterPrefix` CUT, AND THE PROPERTY IT BUYS IS A LENGTH. `operand`
// is a SPECULATIVE alt with lookahead 64 — a 1024-token probe budget — so a
// statement-expression body longer than that cannot be parsed under a probe at
// all. The cut fires after the two-token prefix `( {`, a sequence no other
// operand alt can begin, and the body then parses NON-speculatively with no
// budget. This pin builds a body of roughly 4000 tokens, and it exists because
// the red-on-disable exercise found the config comment asserting a DIFFERENT
// property (the alt ORDER) that no mutant could red: a claim nothing measures is
// a claim that has already drifted. The body is generated rather than written
// out so the length is a NUMBER in the test, not a property of how much text
// somebody was willing to paste.
TEST(GnuStatementExpression, ParsesABodyLongerThanTheSpeculationBudget) {
    constexpr int kStatements = 400;      // ~10 tokens each ⇒ ~4000, vs a 1024 budget
    std::string body = "int main(void){ int acc = ({ int s = 0;";
    for (int i = 0; i < kStatements; ++i) {
        body += " s = s + " + std::to_string(i) + "; s = s - " + std::to_string(i) + ";";
    }
    body += " s + 42; }); return acc - 42; }\n";
    EXPECT_EQ(parseErrorsFor(body), 0u)
        << "a " << kStatements << "-statement body is far past the 1024-token probe "
           "budget; without the commitAfterPrefix cut it cannot parse";
    EXPECT_EQ(loweringCodeCount(body, DiagnosticCode::S_StatementExprHasNoValue), 0u);

    // The same body behind the `__extension__` prefix — the spelling glibc's
    // math.h and assert.h use, and the one that needs BOTH cuts to fire.
    std::string prefixed = body;
    auto const at = prefixed.find("({ int s = 0;");
    ASSERT_NE(at, std::string::npos);
    prefixed.insert(at, "__extension__ ");
    EXPECT_EQ(parseErrorsFor(prefixed), 0u)
        << "`__extension__ ({ ... })` over a long body needs the cut on BOTH rules";
}

// A statement expression does NOT steal an ordinary parenthesised expression, a
// cast, or a compound literal — the three other operand alts that also lead with
// `(`. The `commitAfterPrefix` cut is sound only because `( {` begins none of
// them, and this is the pin that says so.
TEST(GnuStatementExpression, DoesNotStealTheOtherParenLedOperandForms) {
    std::string const src =
        "struct P { int a; int b; };\n"
        "int main(void){\n"
        "  int paren = (1 + 2);\n"
        "  int cast = (int)(3.5);\n"
        "  struct P cl = (struct P){ 4, 5 };\n"
        "  return paren + cast + cl.a + cl.b - 15;\n"
        "}\n";
    EXPECT_EQ(parseErrorsFor(src), 0u)
        << "a cut on `(` alone would swallow every parenthesised operand";
    EXPECT_EQ(loweringCodeCount(src, DiagnosticCode::S_StatementExprHasNoValue), 0u);
}
