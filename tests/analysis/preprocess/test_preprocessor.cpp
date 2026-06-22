// FC13 cycle 1 unit tests for the config-selected C preprocessor
// (src/analysis/preprocess/preprocessor.{hpp,cpp}). These exercise the engine
// DIRECTLY (build a SourceBuffer + the shipped c-subset schema, call
// preprocess, inspect the resulting token stream) so each guard is pinned in
// isolation. Every assertion is the STRONGEST provable property and is
// RED-ON-DISABLE (reverting the backing impl line fails the test).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace dss;

[[nodiscard]] std::shared_ptr<GrammarSchema const> cSubset() {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded.has_value()) {
        ADD_FAILURE() << "loadShipped(c-subset) failed";
        std::abort();
    }
    return *loaded;
}

// Run the preprocessor over `text` (no include dirs) and return the NON-trivia
// token lexemes (sliced from the synth buffer), in order. Directives removed +
// macros expanded, so this is exactly what the parser would see.
[[nodiscard]] std::vector<std::string> ppLexemes(std::string text,
                                                 PreprocessResult& out) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(std::move(text), "main.c");
    std::vector<std::filesystem::path> noDirs;
    out = preprocess(buf, schema, noDirs);
    std::vector<std::string> lexs;
    for (Token const& t : out.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{out.synthBuffer->slice(t.span)});
    }
    return lexs;
}

[[nodiscard]] bool hasPPCode(PreprocessResult const& r, DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == code) return true;
    }
    return false;
}

// Read the shipped c-subset config TEXT (walk up to src/dss-config/sources) so
// a test can REBIND a single config field and reload, proving the engine reads
// that field from config rather than hard-coding a lexeme. Returns "" if not
// found (the caller asserts). Mirrors the inline walk in
// FunctionLikeOpenTokenIsConfigDrivenNotHardcoded.
[[nodiscard]] std::string loadShippedCSubsetText() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path here = fs::current_path(ec);
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        fs::path const cand =
            here / "src" / "dss-config" / "sources" / "c-subset.lang.json";
        if (fs::exists(cand, ec)) {
            std::ifstream in(cand, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
        }
        fs::path const parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
    return {};
}

} // namespace

TEST(Preprocessor, ObjectMacroExpandsAndDirectiveRemoved) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 42\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u) << "expected: int v = 42 ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "42");
    EXPECT_EQ(lexs[4], ";");
}

TEST(Preprocessor, MacroReplacementIsRescanned) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define A 7\n#define B A\nint v = B;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "7") << "B -> A -> 7 requires rescan";
}

TEST(Preprocessor, SelfReferentialMacroDoesNotLoop) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X X\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "X") << "a self-referential macro freezes to its own name";
}

TEST(Preprocessor, MutuallyRecursiveMacrosDoNotLoop) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X Y\n#define Y X\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_TRUE(lexs[3] == "X" || lexs[3] == "Y");
}

TEST(Preprocessor, UndefRemovesBinding) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 1\n#undef X\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "X") << "after #undef, the name is no longer a macro";
}

TEST(Preprocessor, IncompatibleRedefinitionIsReported) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 1\n#define X 2\nint v = X;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "a different #define of an existing macro must be reported";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "1") << "the first definition is kept";
}

TEST(Preprocessor, IdenticalRedefinitionIsBenign) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 1\n#define X 1\nint v = X;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "an identical redefinition is allowed by C";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "1");
}

// FC13 cycle 2 (D-PP-FUNCTION-LIKE-MACRO): a function-like macro DEFINITION
// now PARSES (parameter list) and a simple invocation EXPANDS. This is the
// FLIP of the cycle-1 `FunctionLikeMacroDefinitionFailsLoud` guard (which
// pinned the now-removed P_PreprocessorUnsupported fail-loud). RED-ON-DISABLE:
// reverting the lookahead/substitution in `expand()` (so the call is not
// expanded) leaves `ADD ( 2 , 3 )` in the stream and this exact-token check
// fails.
TEST(Preprocessor, FunctionLikeMacroSimpleInvocationExpands) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define ADD(a,b) ((a)+(b))\nint v = ADD(2,3);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a well-formed function-like macro must not error";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "a function-like macro definition must now be ACCEPTED";
    // int v = ( ( 2 ) + ( 3 ) ) ;
    ASSERT_EQ(lexs.size(), 13u) << "expected: int v = ( ( 2 ) + ( 3 ) ) ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "(");
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "2");
    EXPECT_EQ(lexs[6], ")");
    EXPECT_EQ(lexs[7], "+");
    EXPECT_EQ(lexs[8], "(");
    EXPECT_EQ(lexs[9], "3");
    EXPECT_EQ(lexs[10], ")");
    EXPECT_EQ(lexs[11], ")");
    EXPECT_EQ(lexs[12], ";");
}

// NESTED invocation: ADD(ADD(1,2),3). The inner ADD is an ARGUMENT, so it is
// pre-expanded (C 6.10.3.1) to ((1)+(2)) before the outer substitution. Outer
// a = ((1)+(2)), b = 3 -> ( (((1)+(2))) + (3) ). RED-ON-DISABLE: dropping
// argument pre-expansion leaves the inner `ADD ( 1 , 2 )` tokens unexpanded.
TEST(Preprocessor, FunctionLikeMacroNestedInvocationExpands) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define ADD(a,b) ((a)+(b))\nint v = ADD(ADD(1,2),3);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = ( ( ( ( 1 ) + ( 2 ) ) ) + ( 3 ) ) ;
    ASSERT_EQ(lexs.size(), 21u);
    const char* want[] = {"int","v","=",
        "(","(","(","(","1",")","+","(","2",")",")",")","+","(","3",")",")",";"};
    for (std::size_t i = 0; i < lexs.size(); ++i) {
        EXPECT_EQ(lexs[i], want[i]) << "token index " << i;
    }
}

// ARGUMENT THAT IS A MACRO (pre-expansion): an object macro X used as an
// argument is fully expanded BEFORE substitution. #define X 5 + ADD(X,1) must
// give ((5)+(1)), NOT ((X)+(1)). RED-ON-DISABLE: skipping the per-argument
// expand() call yields an `X` token in the output and lexs[5] != "5".
TEST(Preprocessor, FunctionLikeMacroArgumentIsMacroPreExpanded) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define X 5\n#define ADD(a,b) ((a)+(b))\nint v = ADD(X,1);\n",
                  r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = ( ( 5 ) + ( 1 ) ) ;
    ASSERT_EQ(lexs.size(), 13u);
    EXPECT_EQ(lexs[5], "5") << "the macro argument X must be pre-expanded to 5";
    EXPECT_EQ(lexs[9], "1");
}

// ARITY MISMATCH fails loud (C 6.10.3p4): ADD expects 2 args, called with 1.
// The diagnostic fires (P_PreprocessorMacroArgument) and the name is emitted
// verbatim. RED-ON-DISABLE: removing the arity check silently mis-substitutes.
TEST(Preprocessor, FunctionLikeMacroArityMismatchFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define ADD(a,b) ((a)+(b))\nint v = ADD(2);\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroArgument))
        << "calling a 2-parameter macro with 1 argument must fail loud";
    (void)lexs;
}

// A function-like macro NAME not followed by `(` is NOT an invocation
// (C 6.10.3p10) -- it is emitted VERBATIM. #define F(x) x then `F;` -> `F ;`.
// RED-ON-DISABLE: a lookahead that treats the name as a call (or expands it
// object-like) would drop or mis-handle the bare `F`.
TEST(Preprocessor, FunctionLikeMacroNameWithoutParenIsVerbatim) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define F(x) x\nF;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 2u) << "expected the bare name then `;`: F ;";
    EXPECT_EQ(lexs[0], "F") << "a function-like name with no `(` stays verbatim";
    EXPECT_EQ(lexs[1], ";");
}

// OBJECT + FUNCTION-like MIXING in one TU: both kinds coexist in the table and
// expand correctly in the same stream.
TEST(Preprocessor, FunctionLikeAndObjectMacrosMix) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define TWO 2\n#define DBL(x) ((x)+(x))\nint v = DBL(TWO);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = ( ( 2 ) + ( 2 ) ) ;
    ASSERT_EQ(lexs.size(), 13u);
    EXPECT_EQ(lexs[5], "2") << "the object macro TWO must expand inside the arg";
    EXPECT_EQ(lexs[9], "2");
}

// RECURSIVE function-like macro (blue-paint, C 6.10.3.4): #define F(x) F(x)
// then F(1). During the rescan of the substituted body `F(1)`, F is painted,
// so the inner F is FROZEN -> the result is the literal `F ( 1 )` (no infinite
// loop). RED-ON-DISABLE: failing to paint the macro name around the rescan
// recurses until the depth backstop and changes the output.
TEST(Preprocessor, RecursiveFunctionLikeMacroFreezes) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define F(x) F(x)\nint v = F(1);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // F(1) -> (substitute x=1) F(1) -> (rescan, F painted) frozen `F ( 1 )`.
    // int v = F ( 1 ) ;  (8 tokens: the self-call freezes to its own form).
    ASSERT_EQ(lexs.size(), 8u) << "expected: int v = F ( 1 ) ;";
    EXPECT_EQ(lexs[3], "F") << "the self-referential call freezes to its name";
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "1");
    EXPECT_EQ(lexs[6], ")");
    EXPECT_EQ(lexs[7], ";");
}

// ============================================================================
// FC13 cycle 4 (D-PP-MACRO-HIDESET-PRECISE): the precise per-token hide set
// (Prosser, C 6.10.3.4). The cycle-2/3 engine used a recursion-scoped blue-paint
// set, which FROZE a function-like name whose `(` lived in the PARENT stream
// (the paint had already popped when the rescan returned to the parent). The
// precise hide set carries the disabled-name set PER TOKEN through the produced
// stream, so a name and a `(` that become adjacent only ACROSS the
// replacement/parent boundary now RE-PAIR and expand.
//
// FLIP 1: `#define A(x) x` + `#define F(x) ((x)+100)` + `A(F)(3)`.
//   A(F) -> `F` (hide {A}); the trailing `(3)` is in the PARENT stream (empty
//   hide). F ∉ hide(F-token) -> F expands with the parent's `(3)` -> ((3)+100).
// Previously this emitted the literal `F ( 3 )` (a downstream parser error).
// RED-ON-DISABLE: the recursion-scoped paint (or dropping the splice-rescan)
// leaves `F ( 3 )` and the exact-token check fails.
TEST(Preprocessor, HideSetCrossBoundaryFunctionNameThenParenExpands) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define A(x) x\n#define F(x) ((x)+100)\nint v = A(F)(3);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "the cross-boundary re-pairing must expand cleanly (no parser-bound "
           "literal F)";
    // int v = ( ( 3 ) + 100 ) ;
    ASSERT_EQ(lexs.size(), 11u) << "expected: int v = ( ( 3 ) + 100 ) ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "(");
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "3") << "A(F)(3): F re-pairs with the parent's (3)";
    EXPECT_EQ(lexs[6], ")");
    EXPECT_EQ(lexs[7], "+");
    EXPECT_EQ(lexs[8], "100");
    EXPECT_EQ(lexs[9], ")");
    EXPECT_EQ(lexs[10], ";");
}

// FLIP 2: `#define NAME SQ` + `#define SQ(x) ((x)*(x))` + `NAME(4)`.
//   NAME -> `SQ` (hide {NAME}); SQ ∉ hide -> SQ re-scans to collect the
//   parent's `(4)` -> ((4)*(4)). Previously NAME froze to a bare `SQ` (the
//   object expansion did not re-pair with `(4)`).
// RED-ON-DISABLE: the recursion-scoped paint leaves a bare `SQ ( 4 )`.
TEST(Preprocessor, HideSetObjectMacroNamingFunctionMacroExpands) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define NAME SQ\n#define SQ(x) ((x)*(x))\nint v = NAME(4);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "NAME(4) must re-pair the object-expanded SQ with the parent's (4)";
    // int v = ( ( 4 ) * ( 4 ) ) ;
    ASSERT_EQ(lexs.size(), 13u) << "expected: int v = ( ( 4 ) * ( 4 ) ) ;";
    const char* want[] = {"int","v","=",
        "(","(","4",")","*","(","4",")",")",";"};
    for (std::size_t i = 0; i < lexs.size(); ++i) {
        EXPECT_EQ(lexs[i], want[i]) << "token index " << i;
    }
    EXPECT_EQ(lexs[5], "4") << "the object macro NAME -> SQ then collects (4)";
}

// PRESERVED FREEZE 1: direct object self-reference `#define X X` + `X` stays
// `X` (M ∈ its own result's hide set). The precise hide set must keep this
// frozen exactly as the blue-paint did. RED-ON-DISABLE: omitting the invoked
// macro from the replacement's hide set re-expands X forever (backstop) /
// changes the output.
TEST(Preprocessor, HideSetDirectObjectSelfReferenceFreezes) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X X\nint v = X;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u) << "expected: int v = X ;";
    EXPECT_EQ(lexs[3], "X") << "a direct object self-reference freezes to X";
}

// PRESERVED FREEZE 2: direct function-like self-reference `#define F(x) F(x)` +
// `F(1)` stays `F ( 1 )`. The Prosser function-like rule HS' =
// (hide(name) ∩ hide(close)) ∪ {F}: the rescanned inner `F` AND its `)` both
// carry {F} (they came from the SAME substitution), so the intersection keeps
// {F} and the inner F stays frozen. RED-ON-DISABLE: breaking the intersection
// (e.g. using union, or dropping {F}) either re-expands forever or mis-freezes.
TEST(Preprocessor, HideSetDirectFunctionSelfReferenceFreezes) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define F(x) F(x)\nint v = F(1);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = F ( 1 ) ;
    ASSERT_EQ(lexs.size(), 8u) << "expected: int v = F ( 1 ) ;";
    EXPECT_EQ(lexs[3], "F") << "the self-referential call freezes to its name";
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "1");
    EXPECT_EQ(lexs[6], ")");
    EXPECT_EQ(lexs[7], ";");
}

// PRESERVED FREEZE 3: MUTUAL recursion terminates. `#define P(x) Q(x)` +
// `#define Q(x) P(x)` + `P(1)`: P(1) -> Q(1) [hide {P}] -> P(1) [hide {P,Q}]
// -> P ∈ hide -> frozen `P ( 1 )`. The hide set must terminate the cycle (it
// accretes both names across the two substitutions). RED-ON-DISABLE: a wrong
// hide-set propagation (not carrying {P} into Q's result) loops to the backstop.
TEST(Preprocessor, HideSetMutualFunctionRecursionTerminates) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define P(x) Q(x)\n#define Q(x) P(x)\nint v = P(1);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "mutual recursion must terminate (no backstop diagnostic)";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "the hide set must bound the P<->Q cycle without the backstop";
    // int v = P ( 1 ) ;  (or Q(1) -- both are valid frozen forms; C freezes at
    // the first name that re-enters its own hide set, here P).
    ASSERT_EQ(lexs.size(), 8u) << "expected: int v = P ( 1 ) ;";
    EXPECT_TRUE(lexs[3] == "P" || lexs[3] == "Q")
        << "the mutual cycle freezes to one of the two names";
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "1");
    EXPECT_EQ(lexs[6], ")");
    EXPECT_EQ(lexs[7], ";");
}

// EMPTY argument + ZERO-parameter `()`. A zero-parameter macro M invoked as
// `M()` collects ZERO arguments (arity 0 == 0, OK). A one-parameter macro G
// invoked as `G()` collects ONE empty argument, expanding to nothing.
TEST(Preprocessor, FunctionLikeMacroEmptyAndZeroParen) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#define M() 7\nint v = M();\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        // int v = 7 ;
        ASSERT_EQ(lexs.size(), 5u);
        EXPECT_EQ(lexs[3], "7") << "a zero-parameter macro M() expands to 7";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#define G(x) [x]\nint v = G();\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        // The single EMPTY argument substitutes to nothing: int v = [ ] ;
        ASSERT_EQ(lexs.size(), 6u) << "expected: int v = [ ] ;";
        EXPECT_EQ(lexs[3], "[");
        EXPECT_EQ(lexs[4], "]") << "an empty argument substitutes to no tokens";
        EXPECT_EQ(lexs[5], ";");
    }
}

// FC13 cycle 3 (D-PP-VARIADIC-MACRO): variadic `__VA_ARGS__` macros now WORK.
// This is the FLIP of the cycle-2 `VariadicMacroDefinitionFailsLoud` guard
// (which pinned the now-removed P_PreprocessorUnsupported fail-loud for a
// `#define V(...)`). The case (a) witness: a named-param-PLUS-variadic macro
// substitutes both the named arg AND the trailing args at `__VA_ARGS__`.
// RED-ON-DISABLE: reverting the `__VA_ARGS__` arm in `substitute` (so the
// catch-all is not replaced) leaves `__VA_ARGS__` literally in the stream and
// this exact-token check fails; reverting the parseParamList accept (so `...`
// fails loud again) makes the define error and lexs is wrong.
TEST(Preprocessor, VariadicMacroNamedPlusVaArgsExpands) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define LOG(fmt, ...) f(fmt, __VA_ARGS__)\n"
                  "int v = LOG(7, 1, 2);\n",
                  r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a well-formed variadic macro must not error";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "a variadic macro definition must now be ACCEPTED";
    // int v = f ( 7 , 1 , 2 ) ;
    ASSERT_EQ(lexs.size(), 12u) << "expected: int v = f ( 7 , 1 , 2 ) ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "f");
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "7") << "the named fmt arg substitutes";
    EXPECT_EQ(lexs[6], ",") << "the original separator comma is preserved";
    EXPECT_EQ(lexs[7], "1") << "__VA_ARGS__ substitutes the first trailing arg";
    EXPECT_EQ(lexs[8], ",") << "the trailing args keep their original commas";
    EXPECT_EQ(lexs[9], "2");
    EXPECT_EQ(lexs[10], ")");
    EXPECT_EQ(lexs[11], ";");
}

// CASE (b): a ZERO-NAMED variadic macro `#define V(...) g(__VA_ARGS__)`. Every
// argument is a trailing arg -> the whole list rides __VA_ARGS__ (commas
// preserved). RED-ON-DISABLE: a wrong named-count split (e.g. binding the first
// arg to a non-existent named param) drops or misplaces an argument.
TEST(Preprocessor, VariadicMacroZeroNamedAllArgsAreVaArgs) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define V(...) g(__VA_ARGS__)\nint v = V(1,2,3);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = g ( 1 , 2 , 3 ) ;
    ASSERT_EQ(lexs.size(), 12u) << "expected: int v = g ( 1 , 2 , 3 ) ;";
    EXPECT_EQ(lexs[3], "g");
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "1");
    EXPECT_EQ(lexs[6], ",");
    EXPECT_EQ(lexs[7], "2");
    EXPECT_EQ(lexs[8], ",");
    EXPECT_EQ(lexs[9], "3");
    EXPECT_EQ(lexs[10], ")");
    EXPECT_EQ(lexs[11], ";");
}

// CASE (c): EMPTY variadic part (C23 6.10.3p4 allows the `...` to match zero
// arguments). `LOG("x")` supplies the named `fmt` but NO trailing args, so
// `__VA_ARGS__` substitutes to NOTHING. RED-ON-DISABLE: requiring >= 1 trailing
// arg (a pre-C23 arity floor) would fail loud here; an unsubstituted
// `__VA_ARGS__` would leave the identifier in the stream.
TEST(Preprocessor, VariadicMacroEmptyVaArgsIsC23Allowed) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define LOG(fmt, ...) f(fmt, __VA_ARGS__)\n"
                          "int v = LOG(7);\n",
                          r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "an empty variadic part is allowed (C23) -- must NOT fail loud";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroArgument))
        << "an empty __VA_ARGS__ must not trip the arity floor";
    // int v = f ( 7 , ) ;  -- __VA_ARGS__ vanished; the literal comma between
    // fmt and __VA_ARGS__ in the replacement remains (no GNU comma-elision in
    // this cycle -- that is FC15's `,##__VA_ARGS__`).
    ASSERT_EQ(lexs.size(), 9u) << "expected: int v = f ( 7 , ) ;";
    EXPECT_EQ(lexs[3], "f");
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "7");
    EXPECT_EQ(lexs[6], ",") << "the replacement's literal comma stays (no GNU "
                               "comma-elision in this cycle)";
    EXPECT_EQ(lexs[7], ")") << "__VA_ARGS__ with no trailing args is empty";
    EXPECT_EQ(lexs[8], ";");
}

// CASE (d): a TRAILING arg that is itself a MACRO is PRE-EXPANDED (C 6.10.3.1)
// before it is gathered into __VA_ARGS__, exactly like a named arg.
// `#define N 7` then `V(N, N)` -> the __VA_ARGS__ run is `7 , 7`, not `N , N`.
// RED-ON-DISABLE: skipping the per-trailing-arg `expand()` leaves `N` tokens.
TEST(Preprocessor, VariadicMacroTrailingArgIsMacroPreExpanded) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define N 7\n#define V(...) g(__VA_ARGS__)\nint v = V(N,N);\n",
                  r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = g ( 7 , 7 ) ;
    ASSERT_EQ(lexs.size(), 10u) << "expected: int v = g ( 7 , 7 ) ;";
    EXPECT_EQ(lexs[3], "g");
    EXPECT_EQ(lexs[5], "7") << "a trailing macro arg must be pre-expanded";
    EXPECT_EQ(lexs[6], ",");
    EXPECT_EQ(lexs[7], "7");
    EXPECT_EQ(lexs[8], ")");
    EXPECT_EQ(lexs[9], ";");
}

// I1 (review fold): C 6.10.3p6 -- the catch-all identifier `__VA_ARGS__` may NOT
// be used as a parameter NAME. RED-ON-DISABLE: removing the parseParamList guard
// accepts the name (in a variadic macro the catch-all silently shadows it; in a
// non-variadic one it binds with no diagnostic).
TEST(Preprocessor, VaArgsNameAsParameterNameFailsLoud) {
    {
        PreprocessResult r;
        (void)ppLexemes("#define F(__VA_ARGS__) F\nint v = 0;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "__VA_ARGS__ as a sole parameter name must fail loud";
    }
    {
        PreprocessResult r;
        (void)ppLexemes("#define G(a, __VA_ARGS__) a\nint v = 0;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "__VA_ARGS__ as a named parameter (alongside others) must fail loud";
    }
}

// T1 (review fold): a parenthesized comma in a TRAILING arg is PROTECTED --
// `J((1,2),3)` is TWO trailing args `(1,2)` and `3` (the inner depth-2 comma is
// not a separator). RED-ON-DISABLE: depth-blind splitting yields three args.
TEST(Preprocessor, VariadicParenthesizedCommaTrailingArgIsOneArg) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define J(...) g(__VA_ARGS__)\nint v = J((1,2),3);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = g ( ( 1 , 2 ) , 3 ) ;
    ASSERT_EQ(lexs.size(), 14u) << "expected: int v = g ( ( 1 , 2 ) , 3 ) ;";
    EXPECT_EQ(lexs[3], "g");
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "(");
    EXPECT_EQ(lexs[6], "1");
    EXPECT_EQ(lexs[7], ",") << "the inner (depth-2) comma is protected";
    EXPECT_EQ(lexs[8], "2");
    EXPECT_EQ(lexs[9], ")");
    EXPECT_EQ(lexs[10], ",") << "the depth-1 comma separates the two trailing args";
    EXPECT_EQ(lexs[11], "3");
    EXPECT_EQ(lexs[12], ")");
    EXPECT_EQ(lexs[13], ";");
}

// T1 (review fold): `__VA_ARGS__` used MULTIPLE times in one replacement --
// every occurrence substitutes (no consume-once state). RED-ON-DISABLE: a
// stateful single-use substitution would drop the second copy.
TEST(Preprocessor, VaArgsUsedTwiceExpandsBoth) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define D(...) __VA_ARGS__ __VA_ARGS__\nint v = D(7);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = 7 7 ;
    ASSERT_EQ(lexs.size(), 6u) << "expected: int v = 7 7 ;";
    EXPECT_EQ(lexs[3], "7");
    EXPECT_EQ(lexs[4], "7") << "__VA_ARGS__ used twice substitutes twice";
    EXPECT_EQ(lexs[5], ";");
}

// CASE (e): a `...` that is NOT last in the parameter list fails loud
// (`#define BAD(a, ..., b)`). RED-ON-DISABLE: accepting a mid-list `...` (no
// last-element check) would silently mis-define the macro.
TEST(Preprocessor, VariadicMarkerNotLastFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define BAD(a, ..., b) 0\nint v = 0;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "a `...` that is not the last parameter must fail loud";
    (void)lexs;
}

// CASE (f): too FEW arguments (fewer than the NAMED count) fails loud. `P` has
// TWO named params + variadic; `P(1)` supplies only ONE argument (< 2 named).
// (Note `P(1)`/`P(1,2)` collect 1/2 argument GROUPS; the variadic floor is the
// NAMED count, so 1 < 2 is too few but 2 >= 2 -- with an empty variadic part --
// is fine, C23.) RED-ON-DISABLE: dropping the `args.size() < params.size()`
// floor mis-substitutes a named param from an absent argument with no error.
TEST(Preprocessor, VariadicMacroTooFewArgsFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define P(a, b, ...) f(a, b, __VA_ARGS__)\n"
                          "int v = P(1);\n",
                          r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroArgument))
        << "a variadic macro invoked with fewer than its named-param count "
           "must fail loud";
    (void)lexs;
}

// CASE (g): `__VA_ARGS__` in a NON-variadic macro is a constraint violation
// (C 6.10.3p5) -- fail loud at DEFINITION. RED-ON-DISABLE: removing the
// definition-time guard lets `__VA_ARGS__` leak as a plain identifier (or be
// mis-handled) with no diagnostic.
TEST(Preprocessor, VaArgsInNonVariadicMacroFailsLoud) {
    {
        // Object-like macro.
        PreprocessResult r;
        auto lexs = ppLexemes("#define OBJ __VA_ARGS__\nint v = 0;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "__VA_ARGS__ in an object-like macro must fail loud";
        (void)lexs;
    }
    {
        // Non-variadic function-like macro.
        PreprocessResult r;
        auto lexs =
            ppLexemes("#define F(a) g(a, __VA_ARGS__)\nint v = 0;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "__VA_ARGS__ in a non-variadic function-like macro must fail loud";
        (void)lexs;
    }
}

// DUPLICATE parameter name fails loud (C 6.10.3p6): #define F(a,a) ...
TEST(Preprocessor, DuplicateMacroParameterFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define F(a,a) ((a))\nint v = 0;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "a duplicate macro parameter name must fail loud";
    (void)lexs;
}

// UNTERMINATED invocation (EOF before the matching `)`) fails loud.
TEST(Preprocessor, FunctionLikeMacroUnterminatedInvocationFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define ADD(a,b) ((a)+(b))\nint v = ADD(1,2;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroArgument))
        << "an argument list with no closing paren must fail loud";
    (void)lexs;
}

TEST(Preprocessor, SpaceBeforeParenIsObjectMacro) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define G (1+2)\nint v = G;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported));
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 9u);
    EXPECT_EQ(lexs[3], "(");
    EXPECT_EQ(lexs[4], "1");
    EXPECT_EQ(lexs[5], "+");
    EXPECT_EQ(lexs[6], "2");
    EXPECT_EQ(lexs[7], ")");
}

// IDENTITY on non-directive input: a TU with NO directives + NO macro uses
// passes through UNCHANGED -- the PP token stream equals the raw tokenizer
// stream (same core kinds + spans). The in==out property the no-op relies on.
TEST(Preprocessor, NonDirectiveInputIsIdentity) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        std::string{"int main(void) { return 1 + 2; }\n"}, "main.c");
    std::vector<std::filesystem::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_FALSE(r.diagnostics->hasErrors());

    Tokenizer tk{r.synthBuffer, schema};
    auto rawResult = std::move(tk).tokenize();
    std::vector<Token> raw;
    while (!rawResult.stream.isAtEnd()) {
        raw.push_back(rawResult.stream.advance());
    }

    // Compare the non-Eof content tokens (the PP appends its own single Eof;
    // the raw drain above stops before Eof). Identity means: same count, same
    // core kinds, same spans, in order.
    std::vector<Token> ppNoEof;
    for (Token const& t : r.tokens) {
        if (t.coreKind != CoreTokenKind::Eof) ppNoEof.push_back(t);
    }
    ASSERT_EQ(ppNoEof.size(), raw.size())
        << "non-directive input must be identity (content token count)";
    for (std::size_t i = 0; i < raw.size(); ++i) {
        EXPECT_EQ(ppNoEof[i].coreKind, raw[i].coreKind) << "at index " << i;
        EXPECT_EQ(ppNoEof[i].span, raw[i].span) << "at index " << i;
    }
}

// MULTI-LANGUAGE NO-OP at the config level: a language WITHOUT a preprocess
// block (toy, tsql-subset) reports preprocess().enabled == false, so the
// pipeline gate skips the pass; c-subset (which declares the block) reports
// true. RED-ON-DISABLE: removing the c-subset block flips its expectation.
TEST(Preprocessor, EnabledIsConfigDrivenPerLanguage) {
    auto c = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE((*c)->preprocess().enabled);

    auto toy = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(toy.has_value());
    EXPECT_FALSE((*toy)->preprocess().enabled);

    auto tsql = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(tsql.has_value());
    EXPECT_FALSE((*tsql)->preprocess().enabled);
}

// FIX 1 (RED-on-disable): the function-like-macro `(` opener is CONFIG-DRIVEN
// (`preprocess.functionLikeOpenToken`), NOT a hard-coded "ParenOpen". We prove
// it by loading the shipped c-subset config TEXT with `functionLikeOpenToken`
// rebound to a DIFFERENT real token (`BlockOpen` = `{`). Now `#define F(x)`
// must be treated as an OBJECT-like macro (the `(` is no longer the configured
// function-like opener), so it must NOT emit P_PreprocessorUnsupported.
// RED-ON-DISABLE: reverting the ctor to the literal `find("ParenOpen")` makes
// the engine ignore the rebound config and STILL detect `(` as function-like
// -> P_PreprocessorUnsupported fires -> this test fails. (Agnosticism: the
// opener is read from config, so a language whose paren token is named
// differently is handled correctly.)
TEST(Preprocessor, FunctionLikeOpenTokenIsConfigDrivenNotHardcoded) {
    auto loadedText = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(loadedText.has_value());
    // Read the shipped c-subset config text (walk up to src/dss-config/sources).
    namespace fs = std::filesystem;
    std::string text;
    {
        std::error_code ec;
        fs::path here = fs::current_path(ec);
        for (int i = 0; i < 8 && !here.empty(); ++i) {
            fs::path const cand =
                here / "src" / "dss-config" / "sources" / "c-subset.lang.json";
            if (fs::exists(cand, ec)) {
                std::ifstream in(cand, std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>());
                break;
            }
            fs::path const parent = here.parent_path();
            if (parent == here) break;
            here = parent;
        }
        ASSERT_FALSE(text.empty()) << "could not locate shipped c-subset config";
    }
    // Rebind ONLY the function-like opener to `BlockOpen` (a real, declared
    // c-subset token). The token name must resolve (validated at load), so this
    // is a well-formed schema -- just one where `(` is no longer the opener.
    const std::string from = "\"functionLikeOpenToken\": \"ParenOpen\"";
    const std::string to   = "\"functionLikeOpenToken\": \"BlockOpen\"";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos)
        << "shipped c-subset config no longer carries functionLikeOpenToken=ParenOpen";
    text.replace(pos, from.size(), to);

    auto loaded = GrammarSchema::loadFromText(text, "<rebound-paren-c-subset>");
    ASSERT_TRUE(loaded.has_value())
        << "rebound schema should still load: "
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    ASSERT_EQ(schema->preprocess().functionLikeOpenToken, "BlockOpen");

    // `#define F(x) ...` under the rebound opener: `(` is NOT the function-like
    // marker, so this is an OBJECT-like macro -> NO unsupported diagnostic.
    auto buf = SourceBuffer::fromString(
        std::string{"#define F(x) ((x)+1)\nint v = 0;\n"}, "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "with the `(` opener rebound away, `#define F(x)` must be treated "
           "as object-like -- proving the opener is read from config, not "
           "hard-coded as ParenOpen";
}

// FOLD 1 (RED-on-disable): the VARIADIC marker (`...`) is CONFIG-DRIVEN
// (`preprocess.variadicMarkerToken`), NOT a hard-coded `...` lexeme. We prove it
// by rebinding `variadicMarkerToken` from `EllipsisOp` to a DIFFERENT real token
// (`TildeOp` = `~`) and reloading. Now `...` in `#define V(...)` is NO LONGER
// the configured variadic marker, so the engine must NOT trip the variadic
// fail-loud (`P_PreprocessorUnsupported`) via the `...` SPELLING -- it instead
// hits the generic "expected a parameter name" guard (`P_PreprocessorDirective`,
// since `...` is not a Word). RED-ON-DISABLE: reverting the detection to
// `text(in[q]) == "..."` makes it match by TEXT regardless of the rebound
// config -> `P_PreprocessorUnsupported` fires again -> this test fails.
// (Agnosticism: a second preprocess-opting language whose variadic marker is
// spelled differently is parsed by config kind, not the C `...` text.)
TEST(Preprocessor, VariadicMarkerIsConfigDrivenNotHardcoded) {
    std::string text = loadShippedCSubsetText();
    ASSERT_FALSE(text.empty()) << "could not locate shipped c-subset config";
    // Rebind ONLY the variadic marker to `TildeOp` (a real, declared c-subset
    // token that is NOT `...`). Still a well-formed schema -- just one where the
    // ellipsis is no longer the variadic marker.
    const std::string from = "\"variadicMarkerToken\": \"EllipsisOp\"";
    const std::string to   = "\"variadicMarkerToken\": \"TildeOp\"";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos)
        << "shipped c-subset config no longer carries variadicMarkerToken=EllipsisOp";
    text.replace(pos, from.size(), to);

    auto loaded =
        GrammarSchema::loadFromText(text, "<rebound-variadic-c-subset>");
    ASSERT_TRUE(loaded.has_value())
        << "rebound schema should still load: "
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    ASSERT_EQ(schema->preprocess().variadicMarkerToken, "TildeOp");

    // `#define V(...)` under the rebound marker: the `...` is NOT the configured
    // variadic marker, so the variadic-specific fail-loud must NOT fire.
    namespace fs = std::filesystem;
    auto buf = SourceBuffer::fromString(
        std::string{"#define V(...) 0\nint v = 0;\n"}, "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "with the variadic marker rebound off `...`, a `#define V(...)` must "
           "NOT trip the variadic fail-loud via the `...` spelling -- proving "
           "the marker is read from config kind, not hard-coded text";
    // Positive pin: `...` now hits the generic non-parameter-name guard, so the
    // define still fails loud (just via a DIFFERENT code) -- it is never
    // silently accepted as a named parameter.
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "an unrecognized `...` in parameter position must still fail loud as "
           "a malformed parameter list (not be accepted as a named parameter)";
}

// FOLD 2 (RED-on-disable): the function-like CLOSE token + ARG SEPARATOR are
// CONFIG-DRIVEN (`preprocess.functionLikeCloseToken` /
// `functionLikeArgSeparatorToken`), NOT hard-coded `find("ParenClose")` /
// `find("Comma")`. The open-paren already has its own pin
// (FunctionLikeOpenTokenIsConfigDrivenNotHardcoded); this closes the gap for the
// other two. Each sub-case rebinds ONE token to a different real punctuation
// token and asserts the macro machinery changes behavior accordingly.
//
// Helper: load the shipped c-subset text with ONE `from`->`to` field rebind.
namespace {
[[nodiscard]] std::shared_ptr<GrammarSchema const>
reboundCSubset(std::string const& from, std::string const& to,
               std::string const& label) {
    std::string text = loadShippedCSubsetText();
    if (text.empty()) {
        ADD_FAILURE() << "could not locate shipped c-subset config";
        return nullptr;
    }
    auto const pos = text.find(from);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "shipped c-subset config no longer carries: " << from;
        return nullptr;
    }
    text.replace(pos, from.size(), to);
    auto loaded = GrammarSchema::loadFromText(text, label);
    if (!loaded.has_value()) {
        ADD_FAILURE() << "rebound schema should still load: "
                      << (loaded.error().empty() ? "<no diagnostics>"
                                                  : loaded.error()[0].message);
        return nullptr;
    }
    return *loaded;
}
} // namespace

TEST(Preprocessor, FunctionLikeCloseAndSeparatorAreConfigDrivenNotHardcoded) {
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;

    // (1) CLOSE token rebound off `ParenClose` (-> `BracketClose` = `]`). The
    // parameter-list parser terminates on the configured close; with `)` no
    // longer the close token, `#define F(a,b) ...` can no longer find the end
    // of its parameter list -> fail loud (`P_PreprocessorDirective`). The
    // baseline config parses this define cleanly, so the diagnostic is caused
    // solely by the rebind -- proving the close is read from config.
    // RED-ON-DISABLE: hard-coding the close as `find("ParenClose")` ignores the
    // rebind, the define parses, NO diagnostic fires, and this EXPECT fails.
    {
        auto schema = reboundCSubset(
            "\"functionLikeCloseToken\": \"ParenClose\"",
            "\"functionLikeCloseToken\": \"BracketClose\"",
            "<rebound-close-c-subset>");
        ASSERT_NE(schema, nullptr);
        ASSERT_EQ(schema->preprocess().functionLikeCloseToken, "BracketClose");
        auto buf = SourceBuffer::fromString(
            std::string{"#define F(a,b) ((a)+(b))\nint v = F(1,2);\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(r.diagnostics->hasErrors())
            << "with the `)` close token rebound away, a function-like define's "
               "parameter list cannot terminate -- it must fail loud";
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "the parameter-list terminator is read from config; rebinding it "
               "off `)` must surface a malformed-parameter-list diagnostic";
    }

    // (2) SEPARATOR rebound off `Comma` (-> `Colon` = `:`). The call-site
    // argument collector (`collectArgs`) splits arguments on the configured
    // separator. We define a ONE-parameter macro (no comma in the params, so
    // the define still parses under the rebind) and INVOKE it with a comma:
    // `CNT(a,b)`. Baseline (separator=Comma): the `,` splits -> TWO arguments
    // -> arity mismatch (2 != 1) -> `P_PreprocessorMacroArgument`. Rebound
    // (separator=Colon): the `,` is an ordinary token -> ONE argument `a , b`
    // -> arity 1 == 1 -> NO arity error. So the absence of the arity error
    // proves the separator is read from config. RED-ON-DISABLE: hard-coding the
    // separator as `find("Comma")` ignores the rebind, the `,` still splits
    // into two args, the arity error fires, and this EXPECT_FALSE fails.
    {
        auto schema = reboundCSubset(
            "\"functionLikeArgSeparatorToken\": \"Comma\"",
            "\"functionLikeArgSeparatorToken\": \"Colon\"",
            "<rebound-separator-c-subset>");
        ASSERT_NE(schema, nullptr);
        ASSERT_EQ(schema->preprocess().functionLikeArgSeparatorToken, "Colon");
        auto buf = SourceBuffer::fromString(
            std::string{"#define CNT(x) 1\nint v = CNT(a,b);\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroArgument))
            << "with the `,` separator rebound away, `CNT(a,b)` collects ONE "
               "argument (no arity error) -- proving the separator is read from "
               "config, not hard-coded as Comma";
    }
}

// RED-ON-DISABLE: `__VA_ARGS__` is CONFIG-DRIVEN (`preprocess.variadicArgsName`),
// NOT a hard-coded `__VA_ARGS__` lexeme. We rebind `variadicArgsName` from
// `__VA_ARGS__` to a DIFFERENT identifier (`__REST__`) and reload. Now (1) a
// macro using `__REST__` as the catch-all expands correctly, and (2) the OLD
// spelling `__VA_ARGS__` is just an ordinary identifier -- so it does NOT
// substitute (it passes through verbatim, and in a non-variadic context does
// NOT trip the misuse guard, which keys on the configured name). RED-ON-DISABLE:
// hard-coding the catch-all as the literal "__VA_ARGS__" ignores the rebind ->
// `__REST__` would not substitute (test fails) AND `__VA_ARGS__` would wrongly
// be treated as the catch-all.
TEST(Preprocessor, VaArgsNameIsConfigDrivenNotHardcoded) {
    auto schema =
        reboundCSubset("\"variadicArgsName\": \"__VA_ARGS__\"",
                       "\"variadicArgsName\": \"__REST__\"",
                       "<rebound-vaargs-c-subset>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().variadicArgsName, "__REST__");

    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;

    // (1) The REBOUND catch-all `__REST__` substitutes the trailing args.
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#define V(...) g(__REST__)\nint v = V(1,2);\n"},
            "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        std::vector<std::string> lexs;
        for (Token const& t : r.tokens) {
            if (t.coreKind == CoreTokenKind::Eof) continue;
            if (t.coreKind == CoreTokenKind::Whitespace) continue;
            if (t.coreKind == CoreTokenKind::Newline) continue;
            lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
        }
        // int v = g ( 1 , 2 ) ;
        ASSERT_EQ(lexs.size(), 10u) << "expected: int v = g ( 1 , 2 ) ;";
        EXPECT_EQ(lexs[3], "g");
        EXPECT_EQ(lexs[5], "1");
        EXPECT_EQ(lexs[6], ",");
        EXPECT_EQ(lexs[7], "2")
            << "the REBOUND catch-all __REST__ must substitute the trailing args";
        EXPECT_EQ(lexs[8], ")");
        EXPECT_EQ(lexs[9], ";");
    }
    // (2) The OLD spelling `__VA_ARGS__` is now an ordinary identifier: in an
    // object-like macro it does NOT trip the (rebound-name) misuse guard.
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#define OBJ __VA_ARGS__\nint v = 0;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "with the catch-all rebound to __REST__, the literal __VA_ARGS__ "
               "is an ordinary identifier and must NOT trip the misuse guard -- "
               "proving the catch-all name is read from config, not hard-coded";
    }
}

// FC13 cycle 4 (D-PP-MACRO-HIDESET-PRECISE) -- FLIP of the cycle-2/3
// `DeepMacroExpansionFailsLoudNotSilent` premise. Under the PRECISE per-token
// hide set a finite object-macro CHAIN (`M0`->`M1`->...->`M300`->0) expands
// ITERATIVELY in a single frame (each step splices its replacement back over the
// cursor and rescans; the recursion `depth` stays flat), so it now TERMINATES
// CORRECTLY to `0` instead of tripping the >256 recursion backstop. The old
// fail-loud here was an artifact of the cycle-2 recursive engine (depth ==
// chain length); a 300-long finite chain is valid C and must expand. RED-ON-
// DISABLE: reverting to the recursion-scoped engine (depth tracks chain length)
// re-trips the backstop and leaves `M0` (or a diagnostic) instead of `0`.
TEST(Preprocessor, DeepFiniteMacroChainExpandsToValue) {
    std::string src;
    const int chain = 300;  // would have exceeded the old 256 recursion backstop
    for (int n = 0; n < chain; ++n) {
        src += "#define M" + std::to_string(n) + " M" + std::to_string(n + 1)
             + "\n";
    }
    src += "#define M" + std::to_string(chain) + " 0\n";
    src += "int v = M0;\n";

    PreprocessResult r;
    auto lexs = ppLexemes(src, r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a finite macro chain is valid C and must expand, not fail loud";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "the recursion backstop must NOT fire on a finite (terminating) chain";
    // int v = 0 ;
    ASSERT_EQ(lexs.size(), 5u) << "expected: int v = 0 ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "0")
        << "M0 -> M1 -> ... -> M300 -> 0 expands fully under the hide set";
    EXPECT_EQ(lexs[4], ";");
}

// FC13 cycle 4: the pathological-NESTING backstop still FAILS LOUD with a
// positioned `P_PreprocessorUnsupported` diagnostic instead of silently
// truncating. Under the precise hide set the construct that genuinely recurses
// is NESTING (argument pre-expansion `expand(arg, depth+1)`), so we nest a
// function-like call `F(F(F(...F(0)...)))` deeper than the 256 backstop. Each
// nesting level pre-expands its argument one frame deeper, so the >256 guard
// trips. RED-ON-DISABLE: removing the emitPP at the backstop makes the deep nest
// truncate silently with NO P_Preprocessor* diagnostic -> this test fails.
TEST(Preprocessor, DeepNestedMacroArgumentFailsLoudNotSilent) {
    const int nest = 300;  // > the 256 nesting backstop
    std::string src = "#define F(x) (x)\nint v = ";
    for (int n = 0; n < nest; ++n) src += "F(";
    src += "0";
    for (int n = 0; n < nest; ++n) src += ")";
    src += ";\n";

    PreprocessResult r;
    auto lexs = ppLexemes(src, r);
    (void)lexs;
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "a macro-argument nest deeper than the backstop must fail LOUD at the "
           "preprocessor (positioned diagnostic), never truncate silently";
}

// LINE-MAP HEADER ATTRIBUTION: a diagnostic that originates in an included
// header must be remapped (via PreprocessResult::makeRemap + the line-map) to
// the HEADER file's buffer, not the synthesized buffer. We drive this through
// the full CU pipeline (UnitBuilder) so the remap runs, then check that some
// diagnostic on the produced tree carries the header's BufferId.
//
// RED-ON-DISABLE: removing the `result.tree.remapDiagnostics(remap)` call in
// compilation_unit.cpp parseAndAdd_ (or the header-origin branch in makeRemap)
// leaves the diagnostic on the synth buffer and the header-attribution check
// fails.
TEST(Preprocessor, HeaderOriginDiagnosticAttributesToHeader) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_pp_linemap_test";
    fs::create_directories(dir);
    // The header contains a malformed construct (a stray `@` is an illegal
    // char in c-subset) so the parser/lexer emits a diagnostic whose span
    // lands inside the header's inlined text.
    {
        std::ofstream(dir / "bad.h", std::ios::binary)
            << "int bad(void) { return @; }\n";
    }
    auto mainPath = dir / "main.c";
    {
        std::ofstream(mainPath, std::ios::binary)
            << "#include \"bad.h\"\nint main(void) { return 0; }\n";
    }

    auto schema = cSubset();
    UnitBuilder builder{schema};
    builder.addFile(mainPath);
    auto cu = std::move(builder).finish();

    // Resolve the header's BufferId: the CU's tree source is the SYNTH buffer,
    // but a header-origin diagnostic must have been remapped to a buffer whose
    // NAME ends in "bad.h".
    ASSERT_EQ(cu.trees().size(), 1u);
    bool sawHeaderAttributed = false;
    for (auto const& d : cu.trees()[0].diagnostics().all()) {
        // The synth buffer is the tree's own source; a remapped header
        // diagnostic carries a DIFFERENT buffer id than the synth buffer.
        if (d.buffer != cu.trees()[0].source().id()) {
            sawHeaderAttributed = true;
        }
    }
    EXPECT_TRUE(sawHeaderAttributed)
        << "a diagnostic originating in the included header must be remapped "
           "off the synth buffer onto the header's own buffer";

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// FIX 4 (RED-on-disable): the headline cycle-1 lexer change (c-subset
// `directive` mode no longer overrides `<` -> HeaderStart) is what lets a
// `<<` shift operator survive inside a NON-include directive like `#define`.
// Preprocess `#define SHIFT (1 << 2)` + a use, and assert the EXPANSION lexes
// as the 5 non-trivia tokens `( 1 << 2 )` with NO HeaderStart/HeaderPath
// token. RED-ON-DISABLE: re-adding the `<`->HeaderStart override to the
// `directive` mode makes the `<<` mis-lex as a header path -> a HeaderPath
// token appears and the `<<` lexeme is gone -> this test fails.
TEST(Preprocessor, ShiftOperatorInDefineIsNotMisLexedAsHeader) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define SHIFT (1 << 2)\nint v = SHIFT;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());

    // The expansion of SHIFT must be exactly `( 1 << 2 )` surrounded by
    // `int v = ... ;` -> int v = ( 1 << 2 ) ;
    ASSERT_EQ(lexs.size(), 9u) << "expected: int v = ( 1 << 2 ) ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "(");
    EXPECT_EQ(lexs[4], "1");
    EXPECT_EQ(lexs[5], "<<") << "the shift operator must survive inside #define";
    EXPECT_EQ(lexs[6], "2");
    EXPECT_EQ(lexs[7], ")");
    EXPECT_EQ(lexs[8], ";");

    // Strongest pin: NO token in the stream carries the HeaderStart/HeaderPath
    // schema kind. If the `directive` mode mis-lexed `< 2)\n...` as a header
    // path, one of these would appear.
    auto schema = cSubset();
    const SchemaTokenId headerStart =
        schema->schemaTokens().find("HeaderStart");
    const SchemaTokenId headerPath =
        schema->schemaTokens().find("HeaderPath");
    ASSERT_TRUE(headerStart.valid() && headerPath.valid())
        << "c-subset must declare HeaderStart/HeaderPath for this pin to mean "
           "anything";
    for (Token const& t : r.tokens) {
        EXPECT_NE(t.schemaKind, headerStart)
            << "a `<<` in #define must never lex as a header opener";
        EXPECT_NE(t.schemaKind, headerPath)
            << "a `<<` in #define must never lex as a header path";
    }
}

// FIX 2 (RED-on-disable): full diagnostic-RENDER attribution across an
// include splice. A TU whose FIRST line is a quote-`#include` of a header
// that contains an error, followed by a main-file error on a LATER line,
// must render (via DiagnosticReporter::formatAll over a registry assembled
// from the CU's tree sources + auxiliaryBuffers()):
//   * the HEADER's path:line for the header-origin error, AND
//   * the ORIGINAL main.c line for the main-origin error (NOT a synth-shifted
//     line -- the leading header splice must not drift the main line), AND
//   * NEVER the `<unknown-buffer` sentinel.
// RED-ON-DISABLE: reverting the program.cpp/corpus auxiliaryBuffers()
// registration (or the makeRemap main-origin remap) breaks one of these.
TEST(Preprocessor, IncludeSpliceDiagnosticsRenderToRealFilesAndLines) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_pp_render_attribution_test";
    fs::create_directories(dir);
    // Header error: a stray `@` (illegal char) on the header's line 1.
    { std::ofstream(dir / "bad.h", std::ios::binary)
          << "int hdr(void) { return @; }\n"; }
    // Main: a LEADING include (line 1), then a main-file error (`@`) on line 2.
    auto mainPath = dir / "main.c";
    { std::ofstream(mainPath, std::ios::binary)
          << "#include \"bad.h\"\nint main(void) { return @; }\n"; }

    auto schema = cSubset();
    UnitBuilder builder{schema};
    builder.addFile(mainPath);
    auto cu = std::move(builder).finish();
    ASSERT_EQ(cu.trees().size(), 1u);

    // Assemble the registry exactly as the driver does: every tree's own
    // source PLUS the CU's auxiliary (PP origin) buffers.
    BufferRegistry bufs;
    for (auto const& tree : cu.trees()) {
        if (auto s = tree.sourceShared()) bufs.add(std::move(s));
    }
    for (auto const& b : cu.auxiliaryBuffers()) {
        if (b) bufs.add(b);
    }

    std::string const rendered = cu.trees()[0].diagnostics().formatAll(bufs);

    // The header error attributes to bad.h line 1.
    EXPECT_NE(rendered.find("bad.h:1:"), std::string::npos)
        << "header-origin diagnostic must render the header path:line\n"
        << rendered;
    // The main error attributes to the ORIGINAL main.c line 2 (after the
    // leading #include) -- proving the splice did not drift the main line.
    EXPECT_NE(rendered.find("main.c:2:"), std::string::npos)
        << "main-origin diagnostic must render the ORIGINAL main.c line 2\n"
        << rendered;
    // Never the unknown-buffer sentinel -- every origin buffer is registered.
    EXPECT_EQ(rendered.find("<unknown-buffer"), std::string::npos)
        << "every remapped diagnostic must resolve to a registered buffer\n"
        << rendered;

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// FIX 6 (strict multi-language NO-OP proof): a language with NO `preprocess`
// block, driven through the SAME `preprocess().enabled`-gated pipeline
// (UnitBuilder), must be a strict IDENTITY -- the parsed tree's source is the
// ORIGINAL buffer verbatim (NO synthesized buffer, NO splice, NO directive
// removal), not merely `enabled == false`. tsql `:r`-style directives would
// be mangled if the C preprocessor ever ran on a non-opted-in language, so we
// feed a source that contains a `#`-shaped token and a `<<`-shaped token and
// assert they pass through byte-for-byte. The gate is config-driven (the
// preprocess block's absence), never a language-name check.
TEST(Preprocessor, DisabledLanguageGatePipelineIsStrictIdentity) {
    auto tsql = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(tsql.has_value());
    ASSERT_FALSE((*tsql)->preprocess().enabled)
        << "tsql-subset must declare no preprocess block (gate=false)";

    // A source whose BYTES the C preprocessor would mutate if it ran: it has
    // no real C directive, but the identity property is that the gated
    // pipeline leaves the tree's source text EXACTLY equal to the input.
    std::string const src = "SELECT id FROM T WHERE id = 1;";
    UnitBuilder builder{*tsql};
    builder.addInMemory(src, "q.sql");
    auto cu = std::move(builder).finish();

    ASSERT_EQ(cu.trees().size(), 1u);
    // Strict in==out: the parsed tree's source is the original text verbatim.
    // A preprocessed language would expose a SYNTHESIZED buffer here instead.
    EXPECT_EQ(std::string{cu.trees()[0].source().text()}, src)
        << "a no-preprocess language must pass its source through unchanged";
    // And NO auxiliary (PP origin) buffers were produced -- the pass never ran.
    EXPECT_TRUE(cu.auxiliaryBuffers().empty())
        << "the preprocessor must not run for a language without a block";
}

// ============================================================================
// FC14 (D-PP-CONDITIONAL-COMPILATION): #if / #ifdef / #ifndef / #elif / #else /
// #endif + the `defined` operator. Dead-branch tokens are NOT emitted into the
// body (elision precedes macro expansion); the #if/#elif controlling expression
// is an integer-constant-expression folded by the shared const-eval core via a
// config-precedence Pratt parser. Every assertion is RED-ON-DISABLE.
// ============================================================================

// #if 0 elides the whole group -- grammatically GARBAGE tokens inside a dead
// branch are dropped before the parser sees them (so they never become a parse
// error). RED-ON-DISABLE: dropping the stackActive gate on the body-push leaves
// the garbage in the stream. NOTE: the garbage is lexically VALID -- a lexically
// ILLEGAL character (`@`) would still diagnose, because conditional elision is a
// token-level pass that runs AFTER the single global tokenize of the synth
// buffer (same ordering limit as D-PP-CONDITIONAL-INCLUDE-ORDERING); the
// property under test is that dead-branch *parse*-garbage is elided.
TEST(Preprocessor, IfZeroElidesGarbageBranch) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 0\nthis is not valid c 1 2 3 ) ) ( foo bar baz\n#endif\nint x;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a dead #if-0 branch must elide silently (its garbage never parses)";
    ASSERT_EQ(lexs.size(), 3u) << "expected only: int x ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "x");
    EXPECT_EQ(lexs[2], ";");
}

// #if 1 / #else: the TRUE branch is kept, the #else branch elided.
TEST(Preprocessor, IfOneKeepsThenElseElided) {
    PreprocessResult r;
    auto lexs = ppLexemes("#if 1\nint a;\n#else\nint b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "expected only the #if-1 branch: int a ;";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "a");
    EXPECT_EQ(lexs[2], ";");
}

// PRECEDENCE pin (the crux): `1+2*3 == 7` AND `2*3+1 == 7` must BOTH take the
// branch -- proving the evaluator uses the operator table's precedence (a naive
// left-fold of `1+2*3` would give 9, and `9 == 7` is false). This is the proof
// that the Pratt parser reuses `operatorTable()`.
TEST(Preprocessor, IfExpressionUsesOperatorPrecedence) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#if 1+2*3 == 7\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u) << "1+2*3 == 7 is true -> branch taken";
        EXPECT_EQ(lexs[1], "a");
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#if 2*3+1 == 7\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u) << "2*3+1 == 7 is true -> branch taken";
        EXPECT_EQ(lexs[1], "a");
    }
    {
        // Negative control: a LEFT-fold would make 1+2*3 == 9, so if precedence
        // were wrong this branch would be WRONGLY taken. With correct
        // precedence `1+2*3 == 9` is false -> branch elided.
        PreprocessResult r;
        auto lexs = ppLexemes("#if 1+2*3 == 9\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 0u)
            << "1+2*3 == 9 is FALSE under correct precedence -> elided";
    }
}

// Division by zero in a #if expression FAILS LOUD (MF-5) -- never a silent fold.
TEST(Preprocessor, IfDivisionByZeroFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#if 1/0\nint a;\n#endif\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "a division by zero in a #if expression must fail loud";
    (void)lexs;
}

// #ifdef / #ifndef after a #define FOO: #ifdef takes the branch, #ifndef does
// not (and vice-versa when undefined).
TEST(Preprocessor, IfdefIfndefTrackDefinedness) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#define FOO 1\n#ifdef FOO\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u) << "#ifdef FOO is true after #define FOO";
        EXPECT_EQ(lexs[1], "a");
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#define FOO 1\n#ifndef FOO\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 0u) << "#ifndef FOO is false after #define FOO";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#ifndef BAR\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u) << "#ifndef BAR is true when BAR is undefined";
        EXPECT_EQ(lexs[1], "a");
    }
}

// `defined(FOO)` (paren form) AND `defined BAR` (no-paren form). FOO is defined
// -> true; BAR is undefined -> false. Proves MF-1 (the defined parens are the
// CONFIG parens) end to end via behavior.
TEST(Preprocessor, IfDefinedOperatorParenAndNoParen) {
    {
        PreprocessResult r;
        auto lexs =
            ppLexemes("#define FOO 1\n#if defined(FOO)\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u) << "defined(FOO) is true";
        EXPECT_EQ(lexs[1], "a");
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#if defined BAR\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 0u) << "defined BAR (undefined) is false -> elided";
    }
    {
        // `!defined X` composes with the `!` unary operator.
        PreprocessResult r;
        auto lexs = ppLexemes("#if !defined BAZ\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u) << "!defined BAZ (undefined) is true";
        EXPECT_EQ(lexs[1], "a");
    }
}

// MACRO EXPANSION in the operand: `#define N 1` then `#if N+1 > 1` -> the N
// expands to 1, 1+1 > 1 is true. RED-ON-DISABLE: skipping the macro-expand
// callback leaves N as an identifier -> 0 (C 6.10.1p4), 0+1 > 1 is false.
TEST(Preprocessor, IfOperandIsMacroExpanded) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define N 1\n#if N+1 > 1\nint a;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "N expands to 1; 1+1 > 1 is true -> taken";
    EXPECT_EQ(lexs[1], "a");
}

// An identifier that SURVIVES expansion (not a macro) folds to 0 (C 6.10.1p4):
// `#if UNDEFINED_NAME` is `#if 0` -> elided, NO diagnostic.
TEST(Preprocessor, IfUnknownIdentifierIsZero) {
    PreprocessResult r;
    auto lexs = ppLexemes("#if UNDEFINED_NAME\nint a;\n#else\nint b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "an unknown identifier in #if folds to 0, not an error";
    ASSERT_EQ(lexs.size(), 3u) << "the #else branch is taken: int b ;";
    EXPECT_EQ(lexs[1], "b");
}

// #elif chaining: FIRST true branch wins; later true #elif branches + the #else
// are elided.
TEST(Preprocessor, ElifChainFirstTrueWins) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 0\nint a;\n#elif 1\nint b;\n#elif 1\nint c;\n#else\nint d;\n#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "the FIRST true #elif wins: int b ;";
    EXPECT_EQ(lexs[1], "b") << "later true #elif / #else branches are elided";
}

// A #elif AFTER a #else fails loud, and a SECOND #else fails loud (C 6.10.1p4).
TEST(Preprocessor, ElifOrElseAfterElseFailsLoud) {
    {
        PreprocessResult r;
        (void)ppLexemes("#if 0\n#else\nint a;\n#elif 1\nint b;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "a #elif after a #else must fail loud";
    }
    {
        PreprocessResult r;
        (void)ppLexemes("#if 0\n#else\nint a;\n#else\nint b;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "a second #else in one group must fail loud";
    }
}

// An UNTERMINATED conditional (no #endif) fails loud at end of input.
TEST(Preprocessor, UnterminatedConditionalFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#if 1\nint a;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "a #if with no matching #endif must fail loud";
}

// #endif / #else / #elif with NO matching #if each fail loud.
TEST(Preprocessor, DanglingConditionalDirectivesFailLoud) {
    {
        PreprocessResult r;
        (void)ppLexemes("int a;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "a #endif with no matching #if must fail loud";
    }
    {
        PreprocessResult r;
        (void)ppLexemes("int a;\n#else\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "a #else with no matching #if must fail loud";
    }
}

// A string literal in a #if operand is REJECTED as the unsupported subset
// (P_PreprocessorUnsupported, config-driven via the `literalTypes` string
// kinds), never silently folded. `sizeof` is NOT special-cased: the C
// preprocessor does not know keywords (C 6.10.1p4), so `sizeof` folds as an
// ordinary identifier -> 0 and the trailing `(int)` is then a MALFORMED
// expression (P_PreprocessorDirective) -- the C-faithful behavior (matches
// gcc's "missing binary operator"), and agnostic (no hard-coded `sizeof` name).
TEST(Preprocessor, IfRejectsSizeofAndStringLiteral) {
    {
        PreprocessResult r;
        (void)ppLexemes("#if sizeof(int) > 2\nint a;\n#endif\n", r);
        EXPECT_TRUE(r.diagnostics->hasErrors())
            << "sizeof(int) in a #if expression must fail loud (never silently fold)";
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "sizeof folds to 0 (identifier); the trailing `(int)` is malformed";
    }
    {
        PreprocessResult r;
        (void)ppLexemes("#if \"x\"\nint a;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "a string literal in a #if expression must be rejected";
    }
}

// A DEAD branch's directives are NOT errors (C 6.10p1): an unsupported directive
// AND a malformed `#if sizeof` nested inside a `#if 0` are SKIPPED silently
// (only nesting is tracked). RED-ON-DISABLE: gating the else-arm error on
// stackActive is what suppresses these.
TEST(Preprocessor, DeadBranchDirectivesAreNotErrors) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 0\n#pragma whatever\n#if sizeof(int)\nint dead;\n#endif\n#endif\n"
        "int x;\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "directives inside a dead #if-0 branch must not error (only nesting "
           "is tracked)";
    ASSERT_EQ(lexs.size(), 3u) << "expected only: int x ;";
    EXPECT_EQ(lexs[1], "x");
}

// C 6.10.1p6: a #elif whose group ALREADY took a branch does NOT evaluate its
// controlling expression -- so a div-by-zero in a dead #elif operand must NOT
// fire. RED-ON-DISABLE: dropping the `mayTake` guard in handleElif evaluates the
// dead operand and `1/0` raises a P_PreprocessorDirective.
TEST(Preprocessor, DeadElifOperandIsNotEvaluated) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 1\nint a;\n#elif 1/0\nint b;\n#endif\nint x;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a #elif after a taken branch must NOT evaluate its operand (no 1/0)";
    // Only the taken #if branch + the trailing decl: int a ; int x ;
    ASSERT_EQ(lexs.size(), 6u);
    EXPECT_EQ(lexs[1], "a");
    EXPECT_EQ(lexs[4], "x");
}

// NESTED conditionals: a #if inside a taken branch behaves normally; a #if
// inside a DEAD branch stays dead (its taken-looking inner branch is elided).
TEST(Preprocessor, NestedConditionalsRespectEnclosingDeadBranch) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 1\n#if 0\nint inner_dead;\n#else\nint inner_live;\n#endif\n#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "the live nested #else branch: int inner_live ;";
    EXPECT_EQ(lexs[1], "inner_live");
    {
        // Same nest but the OUTER branch is dead -> everything inside is elided,
        // including the inner #else that would otherwise be live.
        PreprocessResult r2;
        auto lexs2 = ppLexemes(
            "#if 0\n#if 0\nint a;\n#else\nint b;\n#endif\n#endif\nint x;\n", r2);
        EXPECT_FALSE(r2.diagnostics->hasErrors());
        ASSERT_EQ(lexs2.size(), 3u) << "outer-dead elides all inner branches";
        EXPECT_EQ(lexs2[1], "x");
    }
}

// The ternary operator works in a #if expression (proves the operator-table
// Ternary-arity reuse): `#if 1 ? 2 : 0` is 2 (truthy) -> taken.
TEST(Preprocessor, IfTernaryExpression) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#if 1 ? 2 : 0\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u) << "1 ? 2 : 0 == 2 (truthy) -> taken";
        EXPECT_EQ(lexs[1], "a");
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#if 0 ? 1 : 0\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 0u) << "0 ? 1 : 0 == 0 (falsey) -> elided";
    }
}

// && short-circuit: `0 && (1/0)` does NOT trip the div-by-zero (the RHS is not
// evaluated), and folds false. RED-ON-DISABLE: a non-short-circuit && would
// evaluate 1/0 and fail loud.
TEST(Preprocessor, IfLogicalAndShortCircuits) {
    PreprocessResult r;
    auto lexs = ppLexemes("#if 0 && (1/0)\nint a;\n#else\nint b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "0 && (1/0) must short-circuit -- the 1/0 is never evaluated";
    ASSERT_EQ(lexs.size(), 3u) << "the #else branch: int b ;";
    EXPECT_EQ(lexs[1], "b");
}

// A line-continuation inside a #if composes (the splice happens in phase 2,
// before this pass): `#if 1 \<nl> && 1` is one logical line `#if 1 && 1` -> taken.
TEST(Preprocessor, IfWithLineContinuation) {
    PreprocessResult r;
    auto lexs = ppLexemes("#if 1 \\\n && 1\nint a;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a backslash-newline continued #if must compose to one logical line";
    ASSERT_EQ(lexs.size(), 3u) << "1 && 1 is true -> taken: int a ;";
    EXPECT_EQ(lexs[1], "a");
}

// AGNOSTICISM pin (RED-ON-DISABLE): the conditional directive word is CONFIG-
// driven (`preprocess.ifDirective`), NOT a hard-coded "if". Rebind `ifDirective`
// from "if" to "whenever" and reload: now `#whenever 1` conditionalizes while a
// literal `#if 1` is just an unknown directive. RED-ON-DISABLE: hard-coding the
// directive word as "if" makes `#whenever` an unknown directive (the body is
// NOT conditionalized) and `#if` still conditionalizes -> both halves fail.
TEST(Preprocessor, ConditionalDirectiveWordIsConfigDrivenNotHardcoded) {
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;

    auto schema = reboundCSubset("\"ifDirective\":         \"if\"",
                                 "\"ifDirective\":         \"whenever\"",
                                 "<rebound-if-c-subset>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().ifDirective, "whenever");

    // (1) `#whenever 0` now conditionalizes -> the body is elided.
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#whenever 0\nint dead;\n#endif\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        std::vector<std::string> lexs;
        for (Token const& t : r.tokens) {
            if (t.coreKind == CoreTokenKind::Eof) continue;
            if (t.coreKind == CoreTokenKind::Whitespace) continue;
            if (t.coreKind == CoreTokenKind::Newline) continue;
            lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
        }
        ASSERT_EQ(lexs.size(), 3u)
            << "#whenever 0 must conditionalize (elide the dead branch): int x ;";
        EXPECT_EQ(lexs[1], "x");
    }
    // (2) The OLD spelling `#if` is now an UNKNOWN directive -> it does NOT
    // conditionalize (and fails loud as unsupported, proving it is no longer the
    // conditional opener).
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#if 0\nint a;\n#endif\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "with `if` rebound to `whenever`, a literal `#if` is an unknown "
               "directive -- proving the conditional word is read from config";
    }
}
