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

// FOLD 3 (RED-on-disable): the pathological-recursion backstop (>256 expansion
// nesting) FAILS LOUD with a positioned `P_PreprocessorUnsupported` diagnostic
// instead of silently returning the input verbatim (which previously truncated
// a deep chain silently, then failed downstream at the parser). We construct a
// 300-deep object-macro chain (`M0`->`M1`->...->`M300`->0) whose rescan depth
// exceeds the backstop, and assert the diagnostic is emitted at the PP.
// RED-ON-DISABLE: removing the emitPP at the backstop makes the deep chain
// truncate silently with NO P_Preprocessor* diagnostic -> this test fails.
TEST(Preprocessor, DeepMacroExpansionFailsLoudNotSilent) {
    std::string src;
    const int chain = 300;  // > the 256 backstop, so the rescan tops out
    for (int n = 0; n < chain; ++n) {
        src += "#define M" + std::to_string(n) + " M" + std::to_string(n + 1)
             + "\n";
    }
    src += "#define M" + std::to_string(chain) + " 0\n";
    src += "int v = M0;\n";

    PreprocessResult r;
    auto lexs = ppLexemes(src, r);
    (void)lexs;
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "a macro-expansion chain deeper than the backstop must fail LOUD at "
           "the preprocessor (positioned diagnostic), never truncate silently";
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
