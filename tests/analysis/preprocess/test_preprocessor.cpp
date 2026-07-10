// FC13 cycle 1 unit tests for the config-selected C preprocessor
// (src/analysis/preprocess/preprocessor.{hpp,cpp}). These exercise the engine
// DIRECTLY (build a SourceBuffer + the shipped c-subset schema, call
// preprocess, inspect the resulting token stream) so each guard is pinned in
// isolation. Every assertion is the STRONGEST provable property and is
// RED-ON-DISABLE (reverting the backing impl line fails the test).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/char_decode.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"   // c105: per-format prologue tests
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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

// c22 (D-PP-LINE-COMMENT-BEFORE-DIRECTIVE): assert a preprocessed buffer (a)
// reported NO error, (b) fully consumed the `#define Z 1` directive — no `#`,
// `define`, or unexpanded `Z` leaked into the parser-visible lexemes — and (c)
// expanded `Z` to `1`. The line comment's own chars (`//`, `c`) are harmless
// trivia the parser skips; the load-bearing facts are directive-removal +
// expansion, asserted robustly rather than over-pinning the comment's spelling.
[[nodiscard]] ::testing::AssertionResult directiveProcessedToOne(std::string text) {
    PreprocessResult r;
    auto lexs = ppLexemes(std::move(text), r);
    if (r.diagnostics->hasErrors())
        return ::testing::AssertionFailure()
               << "preprocess reported an error (directive leaked to the parser)";
    bool hasOne = false;
    for (auto const& l : lexs) {
        if (l == "#" || l == "define")
            return ::testing::AssertionFailure() << "directive leaked: '" << l << "'";
        if (l == "Z")
            return ::testing::AssertionFailure() << "Z was left unexpanded";
        if (l == "1") hasOne = true;
    }
    if (!hasOne)
        return ::testing::AssertionFailure() << "Z did not expand to 1";
    return ::testing::AssertionSuccess();
}

// The bug case: a `//` comment SHARING a line with code, immediately before a
// directive. The line comment must NOT swallow its terminating newline, else the
// directive loses its line boundary (firstOnLine sees the code before the
// comment) and leaks to the parser unrecognized.
TEST(Preprocessor, LineCommentSharingCodeLineThenDirectiveIsRecognized) {
    EXPECT_TRUE(directiveProcessedToOne("int a; // c\n#define Z 1\nint b=Z;\n"));
}

// Control: the forms that already worked must keep working (the fix is newline
// preservation, not a change to comment recognition); plus a multi-line variant
// and a line comment at EOF.
TEST(Preprocessor, LineCommentNewlinePreservedAcrossForms) {
    // (a) comment ALONE on its own line before a directive.
    EXPECT_TRUE(directiveProcessedToOne("int a;\n// c\n#define Z 1\nint b=Z;\n"));
    // (b) trailing comment ON the directive line.
    EXPECT_TRUE(directiveProcessedToOne("#define Z 1 // c\nint b=Z;\n"));
    // (c) TWO code lines each with a trailing comment, then a directive.
    EXPECT_TRUE(directiveProcessedToOne(
        "int a; // one\nint c; // two\n#define Z 1\nint b=Z;\n"));
    // (d) a `code // comment` line followed by ordinary (non-directive) code must
    // still preprocess cleanly — the preserved newline is benign for plain code.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("int a; // c\nint b;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        bool hasB = false;
        for (auto const& l : lexs) if (l == "b") hasB = true;
        EXPECT_TRUE(hasB) << "code after a `code // comment` line must survive";
    }
}

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
    // fmt and __VA_ARGS__ in the replacement remains. GNU comma-elision does NOT
    // apply here: this replacement is `f(fmt, __VA_ARGS__)` with NO `##`, and
    // elision fires only for the `, ## __VA_ARGS__` shape (see FC15GnuComma*).
    ASSERT_EQ(lexs.size(), 9u) << "expected: int v = f ( 7 , ) ;";
    EXPECT_EQ(lexs[3], "f");
    EXPECT_EQ(lexs[4], "(");
    EXPECT_EQ(lexs[5], "7");
    EXPECT_EQ(lexs[6], ",") << "the replacement's literal comma stays (no `##` "
                               "before __VA_ARGS__, so no comma-elision)";
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
// the garbage in the stream. NOTE (c17, D-PP-CONDITIONAL-INCLUDE-ORDERING
// CLOSED): a lexically ILLEGAL character (`$`/`@`) inside this dead branch is
// now ALSO elided (suppressed by the dead-region oracle) -- see
// `DeadBranchIllegalCharDoesNotError` below. The property under test HERE is
// specifically that dead-branch *parse*-garbage (lexically valid tokens) is
// elided.
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

// ─────────────────────────────────────────────────────────────────────────────
// FC15a (`#`/`##` operators -- C 6.10.3.2 stringize, 6.10.3.3 token-paste).
//
// A stringize product (`#x` -> `"..."`) is, by GRAMMAR REALITY, the string
// literal's StringStart + StringLiteral pair (`stringLiteralExpr = StringStart
// StringLiteral`), NOT a single fabricated token -- so `ppLexemes` yields TWO
// entries for it: the opening `"` (StringStart) then the body (StringLiteral,
// whose span excludes the consumed closing `"`). `reconstructStringLiteral`
// joins them back into the full `"..."` for readable assertions. A paste product
// (`a##b` -> `ab`) is exactly ONE token (F1) and yields ONE lexeme.
// Every assertion is RED-ON-DISABLE: without the `#` handling `#x` emits the
// literal `#` token (lexs[0]=="#" not "\""); without the `##` handling `a##b`
// emits three tokens (`a`, `##`, `b`) instead of the single `ab`.
// ─────────────────────────────────────────────────────────────────────────────

// Join a StringStart (`"`) + StringLiteral (body, no closing quote) pair from the
// pp lexemes at index `i` back into the full source-form literal `"...body..."`.
[[nodiscard]] std::string reconstructStringLiteral(
    std::vector<std::string> const& lexs, std::size_t i) {
    if (i + 1 >= lexs.size()) return "<malformed-string-product>";
    return lexs[i] + lexs[i + 1] + "\"";   // StringStart + body + implied close
}

TEST(Preprocessor, FC15aStringizeSimple) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define STR(x) #x\nSTR(hello)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // The product `"hello"` is StringStart `"` + StringLiteral `hello`.
    ASSERT_EQ(lexs.size(), 2u) << "expected the string-literal pair: \" hello";
    EXPECT_EQ(lexs[0], "\"") << "stringize must produce a string-literal opener "
                                "(red-on-disable: a literal `#` here)";
    EXPECT_EQ(lexs[1], "hello");
    EXPECT_EQ(reconstructStringLiteral(lexs, 0), "\"hello\"");
}

TEST(Preprocessor, FC15aStringizeEscapes) {
    // C 6.10.3.2p2: a `\` is inserted before each `"` and `\` of a string/char
    // literal in the argument. STR(a "b\c") -> "a \"b\\c\"".
    PreprocessResult r;
    auto lexs = ppLexemes("#define STR(x) #x\nSTR(a \"b\\c\")\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 2u);
    EXPECT_EQ(lexs[0], "\"");
    // Body (StringStart consumed the opening `"`, the close `"` is consumed):
    //   a <space> \ " b \ \ c \ "   (the escaped inner text).
    EXPECT_EQ(lexs[1], "a \\\"b\\\\c\\\"")
        << "interior `\"` and `\\` of the string arg must be backslash-escaped";
    EXPECT_EQ(reconstructStringLiteral(lexs, 0), "\"a \\\"b\\\\c\\\"\"");
    // The product must round-trip: decoding the body recovers the raw arg text.
    auto decoded = decodeStringLiteralBody(lexs[1]);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, "a \"b\\c\"");
}

TEST(Preprocessor, FC15aStringizeUsesUnexpandedArg) {
    // C 6.10.3.2p2: the `#` operand uses the RAW (un-pre-expanded) argument. With
    // `#define X hello`, STR(X) stringizes to "X", NOT "hello".
    PreprocessResult r;
    auto lexs = ppLexemes("#define X hello\n#define STR(x) #x\nSTR(X)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 2u);
    EXPECT_EQ(lexs[1], "X")
        << "stringize uses the RAW arg `X`, not its expansion `hello`";
    EXPECT_EQ(reconstructStringLiteral(lexs, 0), "\"X\"");
}

TEST(Preprocessor, FC15aPasteIdentifiers) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define PASTE(a,b) a ## b\nPASTE(foo,bar)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 1u) << "the paste product is exactly ONE token";
    EXPECT_EQ(lexs[0], "foobar");
}

TEST(Preprocessor, FC15aPasteResultIsExpanded) {
    // The paste product is RESCANNED: `foobar` becomes a macro use of `foobar`.
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define PASTE(a,b) a ## b\n#define foobar 42\nint v = PASTE(foo,bar);\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int v = 42 ;
    ASSERT_EQ(lexs.size(), 5u) << "expected: int v = 42 ;";
    EXPECT_EQ(lexs[3], "42")
        << "the paste product `foobar` must be rescanned and expand to 42";
}

TEST(Preprocessor, FC15aPasteInvalidFailsLoud) {
    // F1 (C 6.10.3.3p3): a `##` product that is NOT a single token fails loud.
    // BAD(a,!b) pastes `a` ## `!` -> `a!`, which re-tokenizes to TWO tokens.
    PreprocessResult r;
    (void)ppLexemes("#define BAD(a,b) a ## b\nint v = BAD(a,!b);\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste))
        << "a `##` product that is not a single token must fail loud (F1)";
}

TEST(Preprocessor, FC15aPasteAtStartFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define BAD(a) ## a\nint v = BAD(1);\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste))
        << "a `##` at the START of a replacement list must fail loud";
}

TEST(Preprocessor, FC15aPasteAtEndFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define BAD(a) a ##\nint v = BAD(1);\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste))
        << "a `##` at the END of a replacement list must fail loud";
}

TEST(Preprocessor, FC15aStringizeNotFollowedByParamFailsLoud) {
    // C 6.10.3.2p1: in a function-like macro, `#` must be followed by a
    // parameter. `#define BAD(a) # 1` -> `#` precedes a non-parameter.
    PreprocessResult r;
    (void)ppLexemes("#define BAD(a) # 1\nint v = BAD(0);\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorStringize))
        << "a `#` not followed by a parameter must fail loud";
}

TEST(Preprocessor, FC15aStringizeVaArgs) {
    // `#__VA_ARGS__` stringizes the RAW joined trailing args. S(a,b) -> "a,b".
    PreprocessResult r;
    auto lexs = ppLexemes("#define S(...) #__VA_ARGS__\nS(a,b)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 2u);
    EXPECT_EQ(reconstructStringLiteral(lexs, 0), "\"a,b\"")
        << "#__VA_ARGS__ stringizes the raw comma-joined trailing args";
}

TEST(Preprocessor, FC15aPasteUsesRawOperand) {
    // C 6.10.3.3p1: a `##` operand uses the RAW argument. With `#define X foo`,
    // PASTE(X,bar) pastes RAW `X` ## `bar` -> `Xbar`, NOT `foobar`.
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define X foo\n#define PASTE(a,b) a ## b\nPASTE(X,bar)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 1u);
    EXPECT_EQ(lexs[0], "Xbar")
        << "paste uses the RAW operand `X`, not its expansion `foo`";
}

// audit LOW-1: `##` against `__VA_ARGS__` uses the RAW trailing-args run (the
// rawVaArgs paste branch). With `#define Y q`, `p ## __VA_ARGS__` invoked as
// J(x, Y) pastes RAW `x` ## `Y` -> `xY`, NOT `xq` (the raw va-arg, not its
// expansion). RED-ON-DISABLE: routing the `## __VA_ARGS__` operand through the
// EXPANDED va-args yields `xq`; dropping the paste leaves `x` `Y` unpasted.
TEST(Preprocessor, FC15aPasteVaArgsUsesRawRun) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define Y q\n#define J(p, ...) p ## __VA_ARGS__\nJ(x, Y)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 1u)
        << "`## __VA_ARGS__` against a single raw trailing token yields one product";
    EXPECT_EQ(lexs[0], "xY")
        << "## __VA_ARGS__ pastes the RAW first trailing token `Y`, not its "
           "expansion `q`";
}

// F4 (NOT an order claim -- `##` is associative for the product spelling):
// `a##b##c` collapses BOTH `##` operators into ONE final single token, and the
// two paste operators reduce to one product. We pin the FINAL token + that the
// operators all collapsed (no leftover `##`), NOT any evaluation order.
TEST(Preprocessor, FC15aChainedPasteCollapsesToOneToken) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define CAT3(a,b,c) a ## b ## c\nCAT3(foo,bar,baz)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 1u)
        << "two `##` operators collapse to ONE product token";
    EXPECT_EQ(lexs[0], "foobarbaz");
    // No `##` operator survives in the output.
    for (auto const& s : lexs) EXPECT_NE(s, "##");
}

// AGNOSTICISM (opt-OUT): a language with NO preprocess block declares neither the
// stringize nor the paste token, so the config fields are empty and the engine
// produces no products -- zero behavior change for toy/tsql.
TEST(Preprocessor, FC15aStringizePasteAreOptOutPerLanguage) {
    auto toy = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(toy.has_value());
    EXPECT_TRUE((*toy)->preprocess().stringizeToken.empty());
    EXPECT_TRUE((*toy)->preprocess().pasteToken.empty());

    auto tsql = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(tsql.has_value());
    EXPECT_TRUE((*tsql)->preprocess().stringizeToken.empty());
    EXPECT_TRUE((*tsql)->preprocess().pasteToken.empty());
}

// CONFIG-READ: c-subset declares the `#`/`##` operator kinds from config.
TEST(Preprocessor, FC15aStringizePasteTokensAreConfigRead) {
    auto c = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ((*c)->preprocess().stringizeToken, "HashOp");
    EXPECT_EQ((*c)->preprocess().pasteToken, "HashHashOp");
}

// F3 (HashOp non-contamination): a macro USE immediately followed by a
// `#`-introduced DIRECTIVE line. The directive-introducing `#` (peeled at top
// level via firstOnLine, BEFORE expansion) and an in-replacement stringize `#`
// (handled only inside `substitute`) live in structurally separate phases, so
// they must NOT cross-contaminate: the `#define` directive is consumed (NOT
// mis-read as a stringize), and the stringize `#x` in STR's replacement still
// produces a string literal. (Uses a benign `#define` rather than `#undef STR`
// so STR stays defined when STR(a) is expanded -- directives are processed in a
// single pre-pass, so a later `#undef STR` would undefine it before any body
// expansion, a pre-existing architecture property unrelated to FC15a.)
TEST(Preprocessor, FC15aHashOpDirectiveVsStringizeNoContamination) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define STR(x) #x\n"
        "STR(a)\n"
        "#define UNUSED 1\n"
        "int after;\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "the directive `#define` must be consumed, not treated as a stringize";
    // Output: "a" (StringStart+body)  then  int after ;
    ASSERT_EQ(lexs.size(), 5u) << "expected: \" a int after ;";
    EXPECT_EQ(reconstructStringLiteral(lexs, 0), "\"a\"");
    EXPECT_EQ(lexs[2], "int");
    EXPECT_EQ(lexs[3], "after");
    EXPECT_EQ(lexs[4], ";");
    // The directive-introducing `#` never leaked a stringize diagnostic.
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorStringize));
}

// ─────────────────────────────────────────────────────────────────────────────
// FC15b (predefined macros -- C 6.10.8): `__LINE__`/`__FILE__`/`__STDC__`/
// `__STDC_VERSION__`/`__STDC_HOSTED__`/`__DATE__`/`__TIME__`. The set is
// CONFIG-driven (`preprocess.predefinedMacros`); the engine dispatches ONLY on
// the entry `kind`, never on the macro NAME. A predefined name that is NOT a
// `#define`d macro materializes its configured value at use; `#define`/`#undef`
// of a predefined name fails loud (C 6.10.8.1p2). The load-bearing subtlety:
// `__LINE__`/`__FILE__` resolve against the INVOCATION offset (C 6.10.8.1) -- a
// `__LINE__` reached through a macro replacement reports the INVOCATION line,
// not the `#define` line. Every assertion is RED-ON-DISABLE.
// ─────────────────────────────────────────────────────────────────────────────

// MAKE-OR-BREAK (C 6.10.8.1): `__LINE__` inside a macro replacement resolves to
// the macro's INVOCATION line, NOT the `#define` line and NOT the replacement
// token's own physical span. With WARN defined on line 1 and invoked on line 4,
// `WARN` must materialize `4`. RED-ON-DISABLE: resolving via the replacement
// token's OWN span (its physical position is the `#define` line 1) yields `1`;
// the invocation-offset inheritance (ExpToken::invOffset threaded through the
// object-like splice) is exactly what makes it `4`.
TEST(Preprocessor, FC15bLineInMacroResolvesToInvocationLine) {
    PreprocessResult r;
    //              line: 1                    2        3        4
    auto lexs = ppLexemes("#define WARN __LINE__\nint a;\nint b;\nint x = WARN;\n",
                          r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int a ; int b ; int x = 4 ;   (the directive line is stripped)
    ASSERT_EQ(lexs.size(), 11u) << "expected: int a ; int b ; int x = 4 ;";
    EXPECT_EQ(lexs[9], "4")
        << "__LINE__ in WARN's replacement must resolve to the INVOCATION line "
           "(4), not the #define line (1) -- red-on-disable: the replacement "
           "token's own span would give 1";
    // Sanity on the surrounding shape (so a stray token can't hide a wrong [9]).
    EXPECT_EQ(lexs[6], "int");
    EXPECT_EQ(lexs[7], "x");
    EXPECT_EQ(lexs[8], "=");
    EXPECT_EQ(lexs[10], ";");
}

// The logging idiom: the SAME macro `L` (object-like -> `__LINE__`) used on two
// DIFFERENT lines yields two DIFFERENT values (the invocation line each time).
TEST(Preprocessor, FC15bLineMacroDiffersPerInvocationLine) {
    PreprocessResult r;
    //              line: 1                 2            3            4
    auto lexs = ppLexemes("#define L __LINE__\nint a = L;\nint b = L;\nint c = L;\n",
                          r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int a = 2 ; int b = 3 ; int c = 4 ;
    ASSERT_EQ(lexs.size(), 15u);
    EXPECT_EQ(lexs[3], "2") << "first L invoked on line 2";
    EXPECT_EQ(lexs[8], "3") << "second L invoked on line 3";
    EXPECT_EQ(lexs[13], "4") << "third L invoked on line 4";
}

// A BARE `__LINE__` (no macro) resolves to its own physical line -- the
// degenerate case where the invocation anchor IS the token's source position.
TEST(Preprocessor, FC15bBareLineResolvesToPhysicalLine) {
    PreprocessResult r;
    //              line: 1        2        3
    auto lexs = ppLexemes("int a;\nint b;\nint x = __LINE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // int a ; int b ; int x = 3 ;
    ASSERT_EQ(lexs.size(), 11u);
    EXPECT_EQ(lexs[9], "3") << "a bare __LINE__ on line 3 resolves to 3";
}

// The `constant` kind (C 6.10.8.1): `__STDC__` -> 1, `__STDC_VERSION__` ->
// 202311L (C23), `__STDC_HOSTED__` -> 1. The value spelling reaches the parser
// VERBATIM as a single Number token. RED-ON-DISABLE: without the predefined hook
// these stay ordinary identifiers (lexs would carry `__STDC__` not `1`).
TEST(Preprocessor, FC15bStdcConstants) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes("int v = __STDC__;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 5u);
        EXPECT_EQ(lexs[3], "1") << "__STDC__ materializes its config value 1";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("long v = __STDC_VERSION__;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 5u);
        EXPECT_EQ(lexs[3], "202311L")
            << "__STDC_VERSION__ materializes its config value 202311L (C23)";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("int v = __STDC_HOSTED__;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 5u);
        EXPECT_EQ(lexs[3], "1") << "__STDC_HOSTED__ materializes its config value 1";
    }
}

// FC17.5 (D-CSUBSET-VLA): `__STDC_NO_VLA__` is DEFINED (= 1). C11 6.10.8.3 /
// C23 6.10.9.3 REQUIRE an implementation without variable-length-array support
// to define it — DSS has no VLA support (a runtime array bound fails loud,
// S_NonConstantArrayLength), so declaring the macro is the conformance-honest
// line: a conforming program's `#ifdef __STDC_NO_VLA__` now selects its
// fixed-size fallback instead of tripping the fail-loud gate. RED-ON-DISABLE:
// drop the predefinedMacros row → `#ifdef` selects the else arm.
TEST(Preprocessor, FC175StdcNoVlaDefined) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#ifdef __STDC_NO_VLA__\nint no_vla;\n#else\nint has_vla;\n#endif\n",
            r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u)
            << "__STDC_NO_VLA__ must be defined -> the no_vla arm";
        EXPECT_EQ(lexs[1], "no_vla");
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("int v = __STDC_NO_VLA__;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 5u);
        EXPECT_EQ(lexs[3], "1")
            << "__STDC_NO_VLA__ materializes its config value 1";
    }
}

// `__STDC_VERSION__` works in a `#if` controlling expression (it expands via the
// SAME engine, then the ICE evaluator folds it): `#if __STDC_VERSION__ >= 201112L`
// is true under C23. RED-ON-DISABLE: without the predefined hook the identifier
// folds to 0 and the branch is wrongly elided.
TEST(Preprocessor, FC15bStdcVersionInIfExpression) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if __STDC_VERSION__ >= 201112L\nint modern;\n#else\nint old;\n#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "C23 version >= 201112L -> the modern branch";
    EXPECT_EQ(lexs[1], "modern");
}

// The `file` kind (C 6.10.8.1): `__FILE__` materializes the current source file
// name as a C string literal. The buffer is named "main.c" by ppLexemes, so the
// product decodes to "main.c". A string-literal product is a StringStart +
// StringLiteral pair (like a stringize product), so we reconstruct it.
TEST(Preprocessor, FC15bFileResolvesToSourceName) {
    PreprocessResult r;
    auto lexs = ppLexemes("const char* f = __FILE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // const char * f = " main.c ;   -> the string product is lexs[5]+lexs[6].
    ASSERT_EQ(lexs.size(), 8u) << "const char * f = <str-start> <str-body> ;";
    EXPECT_EQ(reconstructStringLiteral(lexs, 5), "\"main.c\"")
        << "__FILE__ materializes the source file name as a string literal";
}

// `__FILE__` inside an `#include`'d HEADER reports the HEADER's name, not the
// main file's (C 6.10.8.1: the PRESUMED name of the current source file -- which
// after the include splice is the header). The invocation offset of the
// `__FILE__` token lands in the header's line-map segment, so it resolves to the
// header's origin buffer name. RED-ON-DISABLE: resolving __FILE__ to the main
// buffer name (ignoring the line-map origin) yields "main.c" inside the header.
TEST(Preprocessor, FC15bFileInIncludedHeaderReportsHeaderName) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_pp_file_macro_test";
    fs::create_directories(dir);
    // The header USES __FILE__ -- so the product must carry the HEADER's name.
    { std::ofstream(dir / "hdr.h", std::ios::binary)
          << "const char* h = __FILE__;\n"; }
    auto mainPath = dir / "main.c";
    { std::ofstream(mainPath, std::ios::binary)
          << "#include \"hdr.h\"\nint x;\n"; }

    auto schema = cSubset();
    auto mainBuf = SourceBuffer::fromFile(mainPath);
    ASSERT_NE(mainBuf, nullptr);
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(mainBuf, schema, noDirs);
    EXPECT_FALSE(r.diagnostics->hasErrors());

    std::vector<std::string> lexs;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
    }
    // const char * h = " hdr.h ; int x ;  -- find the string product after `=`.
    // The header's __FILE__ product decodes to a name ENDING in "hdr.h" (the
    // origin buffer name is the full path passed to fromFile).
    bool sawHeaderName = false;
    for (std::size_t i = 0; i + 1 < lexs.size(); ++i) {
        if (lexs[i] == "\"") {
            auto decoded = decodeStringLiteralBody(lexs[i + 1]);
            if (decoded.has_value()) {
                const std::string& s = *decoded;
                // Normalized to '/'; ends with "hdr.h" and NOT "main.c".
                if (s.size() >= 5 && s.compare(s.size() - 5, 5, "hdr.h") == 0) {
                    sawHeaderName = true;
                }
                EXPECT_EQ(s.find("main.c"), std::string::npos)
                    << "__FILE__ in the header must NOT report the main file name";
            }
        }
    }
    EXPECT_TRUE(sawHeaderName)
        << "__FILE__ inside an #include'd header must report the HEADER's name";

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// `__DATE__` SHAPE-ONLY (NEVER the exact value -- the build date is
// nondeterministic). C 6.10.8.1: the product is a string literal of the form
// `"Mmm dd yyyy"` -- a decoded body of EXACTLY 11 chars (3 month + space +
// 2 space-padded day + space + 4 year). We pin the LENGTH + structure (a space
// at indices 3 and 6), never the contents.
TEST(Preprocessor, FC15bDateShapeOnly) {
    PreprocessResult r;
    auto lexs = ppLexemes("const char* d = __DATE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 8u);
    EXPECT_EQ(lexs[5], "\"") << "__DATE__ is a string-literal product";
    auto decoded = decodeStringLiteralBody(lexs[6]);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->size(), 11u)
        << "__DATE__ decodes to \"Mmm dd yyyy\" -- exactly 11 chars";
    if (decoded->size() == 11u) {
        EXPECT_EQ((*decoded)[3], ' ') << "space after the month";
        EXPECT_EQ((*decoded)[6], ' ') << "space after the (space-padded) day";
    }
}

// `__TIME__` SHAPE-ONLY: C 6.10.8.1 `"hh:mm:ss"` -- a decoded body of EXACTLY
// 8 chars with `:` at indices 2 and 5. Never the exact value.
TEST(Preprocessor, FC15bTimeShapeOnly) {
    PreprocessResult r;
    auto lexs = ppLexemes("const char* t = __TIME__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 8u);
    EXPECT_EQ(lexs[5], "\"") << "__TIME__ is a string-literal product";
    auto decoded = decodeStringLiteralBody(lexs[6]);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->size(), 8u)
        << "__TIME__ decodes to \"hh:mm:ss\" -- exactly 8 chars";
    if (decoded->size() == 8u) {
        EXPECT_EQ((*decoded)[2], ':') << "colon after hours";
        EXPECT_EQ((*decoded)[5], ':') << "colon after minutes";
    }
}

// FAIL-LOUD (C 6.10.8.1p2): `#define` of a predefined name is a constraint
// violation -> P_PreprocessorPredefinedMacro, and the directive does NOT alter
// the table (a subsequent `__LINE__` still materializes its line value).
TEST(Preprocessor, FC15bDefineOfPredefinedFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define __LINE__ 5\nint x = __LINE__;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "#define of a predefined macro name must fail loud";
    // The rejected #define did not bind __LINE__ to 5; line-2 __LINE__ is 2.
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "2")
        << "the rejected #define must NOT alter the table -- __LINE__ still "
           "resolves to its invocation line (2), not the rejected value 5";
}

// FAIL-LOUD (C 6.10.8.1p2): `#undef` of a predefined name is a constraint
// violation -> P_PreprocessorPredefinedMacro, and the name still materializes.
TEST(Preprocessor, FC15bUndefOfPredefinedFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes("#undef __FILE__\nconst char* f = __FILE__;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "#undef of a predefined macro name must fail loud";
    // __FILE__ still materializes (the #undef was rejected, not applied).
    ASSERT_EQ(lexs.size(), 8u);
    EXPECT_EQ(reconstructStringLiteral(lexs, 5), "\"main.c\"")
        << "the rejected #undef must NOT remove the predefined macro";
}

// AGNOSTICISM (RED-ON-DISABLE): the predefined-macro set is CONFIG-driven
// (`preprocess.predefinedMacros`), NOT hard-coded. Rebind the `__LINE__` entry's
// name to `__CURLINE__` and reload: now `__CURLINE__` resolves to its line while
// the OLD spelling `__LINE__` is an ordinary identifier (passes through). RED-
// ON-DISABLE: hard-coding "__LINE__" makes `__CURLINE__` ordinary (fails (1)) and
// keeps `__LINE__` resolving (fails (2)).
TEST(Preprocessor, FC15bPredefinedNameIsConfigDrivenNotHardcoded) {
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    // Rebind ONLY the name token of the `line` entry (a minimal, unambiguous
    // substring -- `"name": "__LINE__"` appears exactly once in the config).
    auto schema = reboundCSubset("\"name\": \"__LINE__\"",
                                 "\"name\": \"__CURLINE__\"",
                                 "<rebound-line-c-subset>");
    ASSERT_NE(schema, nullptr);

    // (1) The REBOUND name resolves to its invocation line.
    {
        auto buf = SourceBuffer::fromString(
            std::string{"int a;\nint x = __CURLINE__;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        std::vector<std::string> lexs;
        for (Token const& t : r.tokens) {
            if (t.coreKind == CoreTokenKind::Eof) continue;
            if (t.coreKind == CoreTokenKind::Whitespace) continue;
            if (t.coreKind == CoreTokenKind::Newline) continue;
            lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
        }
        ASSERT_EQ(lexs.size(), 8u);
        EXPECT_EQ(lexs[6], "2")
            << "the rebound __CURLINE__ resolves to its invocation line (2)";
    }
    // (2) The OLD spelling `__LINE__` is now an ORDINARY identifier (it passes
    // through verbatim, not resolved to a number).
    {
        auto buf = SourceBuffer::fromString(
            std::string{"int x = __LINE__;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        std::vector<std::string> lexs;
        for (Token const& t : r.tokens) {
            if (t.coreKind == CoreTokenKind::Eof) continue;
            if (t.coreKind == CoreTokenKind::Whitespace) continue;
            if (t.coreKind == CoreTokenKind::Newline) continue;
            lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
        }
        ASSERT_EQ(lexs.size(), 5u);
        EXPECT_EQ(lexs[3], "__LINE__")
            << "with __LINE__ rebound away, the literal `__LINE__` is an ordinary "
               "identifier -- proving the predefined name is read from config";
    }
}

// AGNOSTICISM (opt-OUT): a language with NO preprocess block declares NO
// predefined macros, so `__LINE__` &c. stay ordinary identifiers (zero behavior
// change for toy / tsql-subset). c-subset, by contrast, declares the 7 UNGATED
// C 6.10.8 macros PLUS (c95) the pe-gated Windows-selection macros — `_WIN32` /
// `_WIN64` (value 1) and the ABI qualifiers `__stdcall` / `__cdecl` /
// `__fastcall` / `WINAPI` (empty value → erased). The per-format filter lives in
// `availableObjectFormats`: EMPTY ⇒ every format (the 7 core), a non-empty set ⇒
// that format only. This test pins the split so a stray un-gated Win32 macro
// (which would leak `_WIN32` onto elf/macho) fails loud.
TEST(Preprocessor, FC15bPredefinedMacrosAreOptOutPerLanguage) {
    auto toy = GrammarSchema::loadShipped("toy");
    ASSERT_TRUE(toy.has_value());
    EXPECT_TRUE((*toy)->preprocess().predefinedMacros.empty())
        << "toy declares no predefined macros -- __LINE__ stays ordinary";

    auto tsql = GrammarSchema::loadShipped("tsql-subset");
    ASSERT_TRUE(tsql.has_value());
    EXPECT_TRUE((*tsql)->preprocess().predefinedMacros.empty());

    auto c = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(c.has_value());
    auto const& pms = (*c)->preprocess().predefinedMacros;
    // 8 ungated (the 7 C 6.10.8 core + the FC17.5 `__STDC_NO_VLA__`
    // conformance line, D-CSUBSET-VLA) + 10 pe-gated = 18: the c95 Windows
    // selection (_WIN32/_WIN64/__stdcall/__cdecl/__fastcall/WINAPI) + the
    // c105 MSVC-profile flip (_MSC_VER/__int64/__forceinline/__declspec).
    EXPECT_EQ(pms.size(), 18u)
        << "c-subset declares 8 un-gated + 10 pe-gated Windows predefined macros";
    std::size_t ungated = 0;
    std::size_t peGated = 0;
    for (auto const& pm : pms) {
        if (pm.availableObjectFormats.empty()) {
            ++ungated;
        } else {
            ++peGated;
            EXPECT_EQ(pm.availableObjectFormats.size(), 1u)
                << pm.name << " should be gated to exactly one format";
            EXPECT_EQ(pm.availableObjectFormats.front(), "pe")
                << pm.name << " should be pe-gated (Windows selection)";
        }
    }
    EXPECT_EQ(ungated, 8u)
        << "the 7 C 6.10.8 macros + __STDC_NO_VLA__ (FC17.5) are un-gated "
           "(available on every format)";
    EXPECT_EQ(peGated, 10u)
        << "_WIN32/_WIN64/__stdcall/__cdecl/__fastcall/WINAPI (c95) + "
           "_MSC_VER/__int64/__forceinline/__declspec (c105) are pe-gated";
}

// LOADER fail-loud (c95): a `predefinedMacros.availableObjectFormats` naming an
// UNKNOWN object-format is a config typo that would silently never seed the
// macro on any target (an OS-selection macro that never fires) -> it must be a
// LOAD error, never accepted. We corrupt `_WIN32`'s ["pe"] to ["pee"] and assert
// the load FAILS (C_InvalidPreprocess via objectFormatKindFromName). RED-ON-
// DISABLE: without the loader validation this parses and the macro is dead.
TEST(Preprocessor, FC15bPredefinedMacroBadObjectFormatIsLoadError) {
    std::string text = loadShippedCSubsetText();
    ASSERT_FALSE(text.empty());
    const std::string from =
        "{ \"name\": \"_WIN32\",              \"kind\": \"constant\", "
        "\"value\": \"1\", \"availableObjectFormats\": [\"pe\"] }";
    const std::string to   =
        "{ \"name\": \"_WIN32\",              \"kind\": \"constant\", "
        "\"value\": \"1\", \"availableObjectFormats\": [\"pee\"] }";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos)
        << "the _WIN32 predefinedMacros entry must be present verbatim";
    text.replace(pos, from.size(), to);
    auto loaded = GrammarSchema::loadFromText(text, "<bad-objfmt-c-subset>");
    EXPECT_FALSE(loaded.has_value())
        << "an unknown availableObjectFormats name ('pee') must be a load error";
}

// ─────────────────────────────────────────────────────────────────────────────
// FC15c (`#pragma` -- C 6.10.6; `__has_include` + `__has_c_attribute` --
// C23 6.10.1p4). `#pragma` is consumed-and-DROPPED with NO error. The two
// operators are valid only in a `#if`/`#elif` operand; their RESULT (0/1 for
// __has_include, a version int for __has_c_attribute) is folded by the ICE
// evaluator. The angle delimiters of `__has_include(<h>)` are matched by CONFIG
// token KIND, never the `<`/`>` bytes (agnosticism). Every assertion is
// RED-ON-DISABLE.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Run the preprocessor over `text` with an explicit systemDirs (the angle-form
// search path) + includeDirs (the quote-form search path) and return the
// NON-trivia lexemes. Mirrors `ppLexemes` but threads the search paths so the
// Finding-3 `__has_include(<stem.json>)` mapping can be exercised.
[[nodiscard]] std::vector<std::string> ppLexemesWithDirs(
    std::string text, PreprocessResult& out,
    std::vector<std::filesystem::path> includeDirs,
    std::vector<std::filesystem::path> systemDirs) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(std::move(text), "main.c");
    out = preprocess(buf, schema, includeDirs, systemDirs);
    std::vector<std::string> lexs;
    for (Token const& t : out.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{out.synthBuffer->slice(t.span)});
    }
    return lexs;
}
} // namespace

// `#pragma` is consumed-and-DROPPED with NO error (C 6.10.6p2). The line carries
// a GCC-style payload; only `int v = 1 ;` survives. RED-ON-DISABLE: without the
// `#pragma`-consume arm the directive hits the generic unsupported-directive
// fail-loud (P_PreprocessorUnsupported).
TEST(Preprocessor, FC15cPragmaConsumedAndDropped) {
    PreprocessResult r;
    auto lexs = ppLexemes("#pragma GCC optimize(\"O2\")\nint v=1;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a `#pragma` line must be silently consumed (C 6.10.6p2)";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "a `#pragma` must NOT trip the unsupported-directive fail-loud";
    ASSERT_EQ(lexs.size(), 5u) << "only `int v = 1 ;` survives the dropped pragma";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "v");
    EXPECT_EQ(lexs[2], "=");
    EXPECT_EQ(lexs[3], "1");
    EXPECT_EQ(lexs[4], ";");
}

// A `#pragma` inside a DEAD branch is silent too (the arm is past the
// stackActive gate). `#if 0 ... #pragma ... #endif` -> no diagnostic, nothing
// emitted from the dead group.
TEST(Preprocessor, FC15cPragmaInDeadBranchIsSilent) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#if 0\n#pragma whatever here\nint dead;\n#endif\nint x;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "only `int x ;` survives";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "x");
    EXPECT_EQ(lexs[2], ";");
}

// OPT-OUT (RED-ON-DISABLE for the config match): with `pragmaDirective` stripped
// from config, `#pragma` is no longer recognized -> it hits the generic
// unsupported-directive fail-loud. Proves the engine matches the CONFIG word,
// not a hard-coded "pragma".
TEST(Preprocessor, FC15cPragmaIsConfigDrivenFailsLoudWhenStripped) {
    namespace fs = std::filesystem;
    // Remove the pragmaDirective line entirely (so the field defaults to empty).
    auto schema = reboundCSubset("\"pragmaDirective\":          \"pragma\",",
                                 "",
                                 "<no-pragma-c-subset>");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->preprocess().pragmaDirective.empty())
        << "the rebound schema must declare no pragma directive";
    auto buf = SourceBuffer::fromString(
        std::string{"#pragma GCC optimize(\"O2\")\nint v=1;\n"}, "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "with `pragmaDirective` stripped, `#pragma` must fail loud as an "
           "unsupported directive -- proving the directive word is read from "
           "config, not hard-coded";
}

// `__has_include("h")` quote form -> 1 when the local file exists. We write a
// real header into a temp dir, pass it as the includeDir, and probe it. The 42
// branch is taken (lexs == `int yes ;`). RED-ON-DISABLE: without the
// `__has_include` arm the identifier folds to 0 -> the #else branch.
TEST(Preprocessor, FC15cHasIncludeQuoteExistingFileIsOne) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_fc15c_has_include_q";
    fs::create_directories(dir);
    { std::ofstream(dir / "real_header.h", std::ios::binary) << "/* x */\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#if __has_include(\"real_header.h\")\nint yes;\n#else\nint no;\n#endif\n",
        r, {dir}, {});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "the existing header -> the `yes` branch";
    EXPECT_EQ(lexs[1], "yes")
        << "__has_include of an existing quote header must be 1 (branch taken)";
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// `__has_include("h")` quote form -> 0 when the file does NOT exist -> the #else
// branch is taken.
TEST(Preprocessor, FC15cHasIncludeQuoteMissingFileIsZero) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if __has_include(\"definitely_no_such_header_xyz.h\")\n"
        "int yes;\n#else\nint no;\n#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "the missing header -> the `no` branch";
    EXPECT_EQ(lexs[1], "no")
        << "__has_include of a missing quote header must be 0 (else branch)";
}

// FINDING 3 (the silent-miscompile the plan-lock caught): the ANGLE form maps
// `<stem>.json` on the systemDirs path (DSS ships JSON descriptors, e.g.
// `stdio.json`, NOT `stdio.h`). `__has_include(<stdio.h>)` must be 1 when
// `stdio.json` is on the system path -- a naive `findInDirs("stdio.h", ...)`
// returns 0 while `#include <stdio.h>` succeeds (the wrong answer). We put a
// `stdio.json` in a temp systemDir and probe `<stdio.h>`.
// RED-ON-DISABLE for the stem mapping: resolving the literal `stdio.h` on the
// systemDirs (which holds only `stdio.json`) yields 0 -> the wrong branch.
TEST(Preprocessor, FC15cHasIncludeAngleMapsStemDotJson) {
    namespace fs = std::filesystem;
    auto sysdir = fs::temp_directory_path() / "dss_fc15c_has_include_sys";
    fs::create_directories(sysdir);
    // Ship a JSON descriptor (the shape DSS ships), NOT a `.h` file.
    { std::ofstream(sysdir / "stdio.json", std::ios::binary) << "{}\n"; }
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#if __has_include(<stdio.h>)\nint yes;\n#else\nint no;\n#endif\n",
            r, {}, {sysdir});
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "yes")
            << "__has_include(<stdio.h>) must map to stdio.json on the system "
               "path (Finding 3) -- a literal `stdio.h` search yields 0";
    }
    // A header with no shipped descriptor -> 0 (the else branch).
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#if __has_include(<nope.h>)\nint yes;\n#else\nint no;\n#endif\n",
            r, {}, {sysdir});
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "no")
            << "__has_include(<nope.h>) with no nope.json on the path must be 0";
    }
    std::error_code ec;
    fs::remove_all(sysdir, ec);
}

// FAIL-LOUD (C23 6.10.1p4 well-formedness): every malformed `__has_include`
// shape -> P_PreprocessorHasInclude (a DISTINCT, positioned diagnostic, never a
// generic ICE fallthrough). Missing `(`, missing `>`, missing `)`, empty name.
TEST(Preprocessor, FC15cHasIncludeMalformedFailsLoud) {
    // Missing `(`.
    {
        PreprocessResult r;
        (void)ppLexemes("#if __has_include\nint a;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorHasInclude))
            << "__has_include with no `(` must fail loud";
    }
    // Missing closing `>`.
    {
        PreprocessResult r;
        (void)ppLexemes("#if __has_include(<stdio.h)\nint a;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorHasInclude))
            << "__has_include(<...  with no `>` must fail loud";
    }
    // Missing closing `)`.
    {
        PreprocessResult r;
        (void)ppLexemes("#if __has_include(\"h.h\"\nint a;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorHasInclude))
            << "__has_include(\"...\"  with no `)` must fail loud";
    }
    // Empty angle name `<>`.
    {
        PreprocessResult r;
        (void)ppLexemes("#if __has_include(<>)\nint a;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorHasInclude))
            << "__has_include(<>) (empty name) must fail loud";
    }
}

// `__has_c_attribute(deprecated)` -> the configured version (202311, truthy) ->
// the branch is taken. RED-ON-DISABLE: without the operator the identifier folds
// to 0 -> the #else branch.
TEST(Preprocessor, FC15cHasCAttributeKnownIsVersion) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if __has_c_attribute(deprecated)\nint yes;\n#else\nint no;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "a known attribute -> the `yes` branch";
    EXPECT_EQ(lexs[1], "yes")
        << "__has_c_attribute(deprecated) must fold to its version (202311 != 0)";
}

// The exact version reaches the ICE comparator: `__has_c_attribute(nodiscard)
// == 202311` is true; `>= 202312` is false -- proves the minted value is the
// configured int, not just a truthy 1.
TEST(Preprocessor, FC15cHasCAttributeExactVersion) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#if __has_c_attribute(nodiscard) == 202311\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a") << "the minted value is exactly 202311";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#if __has_c_attribute(nodiscard) >= 202312\nint a;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        EXPECT_TRUE(lexs.empty()) << "202311 >= 202312 is false -> nothing emitted";
    }
}

// The dunder form is accepted (C 6.10.1: the lookup ignores leading/trailing
// `__`): `__has_c_attribute(__deprecated__)` resolves the same as `deprecated`.
TEST(Preprocessor, FC15cHasCAttributeDunderForm) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if __has_c_attribute(__deprecated__)\nint a;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a")
        << "__deprecated__ must match the known `deprecated` (dunder stripped)";
}

// An UNKNOWN attribute -> 0 -> the #else branch (never an error -- C23 says
// unknown attributes yield 0).
TEST(Preprocessor, FC15cHasCAttributeUnknownIsZero) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if __has_c_attribute(not_a_real_attr)\nint yes;\n#else\nint no;\n#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "an unknown attribute folds to 0, never an error";
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "no")
        << "__has_c_attribute(not_a_real_attr) must be 0 (else branch)";
}

// AGNOSTICISM (opt-OUT): with `hasIncludeOperator` stripped from config,
// `__has_include` is an ORDINARY identifier in a `#if` operand -> it folds to 0
// (C 6.10.1p4), so `#if __has_include("x")` is `#if 0` -> the else branch. The
// `(...)` trails as a malformed expression? No -- a folded-0 identifier followed
// by `(` would be a call shape the ICE parser rejects; we instead pin the
// CONFIG-READ contract directly (the operator/angle-token strings) which is the
// load-bearing agnosticism property, plus the bare-identifier fold via a name
// the parser accepts standalone.
TEST(Preprocessor, FC15cHasIncludeIsConfigDrivenOptOut) {
    // Strip the operator declaration; the angle tokens go too (the loader
    // requires them only WHEN the operator is declared, so removing all three
    // keeps the schema self-consistent).
    std::string text = loadShippedCSubsetText();
    ASSERT_FALSE(text.empty());
    for (std::string const& line :
         {std::string{"\"hasIncludeOperator\":       \"__has_include\",\n"},
          std::string{"    \"hasIncludeAngleOpenToken\":  \"LtOp\",\n"},
          std::string{"    \"hasIncludeAngleCloseToken\": \"GtOp\",\n"}}) {
        auto const pos = text.find(line);
        ASSERT_NE(pos, std::string::npos) << "config no longer carries: " << line;
        text.erase(pos, line.size());
    }
    auto loaded = GrammarSchema::loadFromText(text, "<no-has-include-c-subset>");
    ASSERT_TRUE(loaded.has_value())
        << "stripping the operator + its angle tokens must still load: "
        << (loaded.error().empty() ? "<none>" : loaded.error()[0].message);
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    EXPECT_TRUE(schema->preprocess().hasIncludeOperator.empty());

    // A BARE `__has_include` (no parens) now folds as an ordinary identifier ->
    // 0, so `#if __has_include` is `#if 0` -> the else branch is taken. No
    // P_PreprocessorHasInclude (the operator is gone).
    namespace fs = std::filesystem;
    auto buf = SourceBuffer::fromString(
        std::string{"#if __has_include\nint yes;\n#else\nint no;\n#endif\n"},
        "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorHasInclude))
        << "with the operator stripped, `__has_include` is ordinary -- no "
           "has-include diagnostic";
    std::vector<std::string> lexs;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
    }
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "no")
        << "a stripped __has_include folds to 0 -> the #else branch";
}

// CONFIG-READ pins: the shipped c-subset declares the operator names + the angle
// token KINDS; toy / tsql declare none. The angle delimiters being CONFIG token
// names (not the `<`/`>` bytes) is the make-or-break agnosticism property.
TEST(Preprocessor, FC15cOperatorNamesAndAngleTokensAreConfigDeclared) {
    auto c = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ((*c)->preprocess().pragmaDirective, "pragma");
    EXPECT_EQ((*c)->preprocess().hasIncludeOperator, "__has_include");
    EXPECT_EQ((*c)->preprocess().hasCAttributeOperator, "__has_c_attribute");
    EXPECT_EQ((*c)->preprocess().hasIncludeAngleOpenToken, "LtOp")
        << "the angle delimiters are matched by token KIND, declared in config";
    EXPECT_EQ((*c)->preprocess().hasIncludeAngleCloseToken, "GtOp");
    EXPECT_EQ((*c)->preprocess().knownCAttributes.size(), 7u);

    for (char const* lang : {"toy", "tsql-subset"}) {
        auto s = GrammarSchema::loadShipped(lang);
        ASSERT_TRUE(s.has_value()) << lang;
        EXPECT_TRUE((*s)->preprocess().pragmaDirective.empty()) << lang;
        EXPECT_TRUE((*s)->preprocess().hasIncludeOperator.empty()) << lang;
        EXPECT_TRUE((*s)->preprocess().hasCAttributeOperator.empty()) << lang;
        EXPECT_TRUE((*s)->preprocess().knownCAttributes.empty()) << lang;
    }
}

// FINDING 1 (RED-ON-DISABLE): the angle delimiters of `__has_include(<h>)` are
// matched by CONFIG token KIND, not the `<`/`>` bytes. Rebind
// `hasIncludeAngleOpenToken` from `LtOp` to a DIFFERENT real declared token
// (`TildeOp` = `~`) and the `<h>` form must NO LONGER be recognized as the
// angle opener -> the operand `<stdio.h>` is now a malformed shape -> fail loud.
// RED-ON-DISABLE: matching `<` by the literal byte would ignore the rebind and
// still parse the angle form, so no diagnostic fires.
TEST(Preprocessor, FC15cAngleDelimiterIsConfigKindNotByte) {
    namespace fs = std::filesystem;
    // Rebind the angle OPEN token to a real token that is NOT `<` (`TildeOp`=`~`).
    auto schema = reboundCSubset("\"hasIncludeAngleOpenToken\":  \"LtOp\"",
                                 "\"hasIncludeAngleOpenToken\":  \"TildeOp\"",
                                 "<rebound-angle-open-c-subset>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().hasIncludeAngleOpenToken, "TildeOp");
    // `__has_include(<stdio.h>)`: the `<` is no longer the configured angle
    // opener, so the operand is neither the angle nor the quote form -> the
    // engine must fail loud (it is NOT silently parsed via the `<` byte).
    auto buf = SourceBuffer::fromString(
        std::string{"#if __has_include(<stdio.h>)\nint a;\n#endif\n"}, "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorHasInclude))
        << "with the angle-open token rebound off `<`, a `<h>` operand must fail "
           "loud -- proving the delimiter is matched by config KIND, not the `<` "
           "byte";
}

// LOADER (make-or-break self-consistency): a language declaring
// `hasIncludeOperator` WITHOUT both angle tokens is a self-inconsistent contract
// -> C_InvalidPreprocess at load. We strip ONLY the angle-open token, leaving the
// operator declared, and assert the load FAILS.
TEST(Preprocessor, FC15cHasIncludeWithoutAngleTokensIsLoadError) {
    std::string text = loadShippedCSubsetText();
    ASSERT_FALSE(text.empty());
    const std::string line = "\"hasIncludeAngleOpenToken\":  \"LtOp\",\n";
    auto const pos = text.find(line);
    ASSERT_NE(pos, std::string::npos);
    text.erase(pos, line.size());
    auto loaded = GrammarSchema::loadFromText(text, "<bad-has-include-c-subset>");
    EXPECT_FALSE(loaded.has_value())
        << "declaring hasIncludeOperator without both angle tokens must be a "
           "load error (C_InvalidPreprocess)";
}

// LOADER: a malformed `knownCAttributes` entry (a non-positive version) ->
// C_InvalidPreprocess at load.
TEST(Preprocessor, FC15cKnownCAttributeBadVersionIsLoadError) {
    std::string text = loadShippedCSubsetText();
    ASSERT_FALSE(text.empty());
    const std::string from = "{ \"name\": \"deprecated\",   \"version\": 202311 }";
    const std::string to   = "{ \"name\": \"deprecated\",   \"version\": 0 }";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos);
    text.replace(pos, from.size(), to);
    auto loaded = GrammarSchema::loadFromText(text, "<bad-attr-c-subset>");
    EXPECT_FALSE(loaded.has_value())
        << "a knownCAttributes entry with version <= 0 must be a load error";
}

// ─────────────────────────────────────────────────────────────────────────────
// FC15 paste residuals — object-like `##` (D-PP-PASTE-OBJECT-LIKE), placemarkers
// for empty `##` operands (D-PP-PASTE-PLACEMARKER, C 6.10.3.3p2), and the GNU
// `,##__VA_ARGS__` comma-elision (D-PP-VARIADIC-GNU-COMMA-ELISION). These COMPLETE
// FC15: `##` now works in object-like macros and with empty operands, and the GNU
// elision is config-gated (`variadicCommaElision`). A GENUINE dangling `##` (no
// operand token in the replacement list) still fails loud (FC15aPasteAt{Start,End}
// + the object-like pin below).
// ─────────────────────────────────────────────────────────────────────────────

// (1) Object-like `##`: `#define HW a ## b` -> `HW` pastes to the single token
// `ab`. RED-ON-DISABLE: without the `collapsePastes` call in the object-like
// expand arm, `a`, `##`, `b` pass through verbatim (3 tokens; `##` then trips the
// parser). lexs.size() != 1.
TEST(Preprocessor, FC15ObjectLikePasteYieldsOneToken) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define HW a ## b\nHW\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 1u) << "object-like ## must yield exactly ONE token";
    EXPECT_EQ(lexs[0], "ab");
}

// (2) Object-like `##` chains left-to-right exactly like the function-like path.
TEST(Preprocessor, FC15ObjectLikePasteChain) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define T x ## y ## z\nT\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 1u) << "two object-like ## collapse to ONE token";
    EXPECT_EQ(lexs[0], "xyz");
}

// (3) The object-like paste PRODUCT is rescanned: `MK` -> `foo` (paste) ->
// rescans as a macro use of `foo` -> 7. RED-ON-DISABLE: an un-collapsed
// `fo ## o` never forms `foo`, so the `foo`->7 expansion cannot fire.
TEST(Preprocessor, FC15ObjectLikePasteProductRescanned) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#define MK fo ## o\n#define foo 7\nint v = MK;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u) << "expected: int v = 7 ;";
    EXPECT_EQ(lexs[3], "7")
        << "object-like ## product `foo` must rescan and expand to 7";
}

// (4) Placemarker, RIGHT operand empty (C 6.10.3.3p2): `J(x,)` -> `x ## <pm>` ->
// `x`. RED-ON-DISABLE: without the placemarker, the empty `b` arg pushes nothing,
// `items` ends `[x, ##]`, and `collapsePastes` fires P_PreprocessorPaste (dangling).
TEST(Preprocessor, FC15PlacemarkerRightEmpty) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define J(a,b) a ## b\nJ(x,)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste))
        << "x ## <empty> is a placemarker paste, NOT a dangling ##";
    ASSERT_EQ(lexs.size(), 1u) << "x ## placemarker -> x";
    EXPECT_EQ(lexs[0], "x");
}

// (5) Placemarker, LEFT operand empty: `J(,y)` -> `<pm> ## y` -> `y`.
TEST(Preprocessor, FC15PlacemarkerLeftEmpty) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define J(a,b) a ## b\nJ(,y)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 1u) << "placemarker ## y -> y";
    EXPECT_EQ(lexs[0], "y");
}

// (6) Placemarker, BOTH operands empty: `J(,)` -> `<pm> ## <pm>` -> a placemarker
// -> dropped -> NO output tokens. RED-ON-DISABLE: a surviving placemarker would
// emit a garbage token (size 1, not 0); a missing placemarker would fail loud.
TEST(Preprocessor, FC15PlacemarkerBothEmpty) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define J(a,b) a ## b\nJ(,)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 0u) << "placemarker ## placemarker -> empty";
}

// (7) Placemarker MID-chain: `J3(x,,z)` = `x ## <pm> ## z` collapses left-to-right
// (`x ## <pm>` -> `x`, then `x ## z` -> `xz`). RED-ON-DISABLE: the first `##` would
// try to paste `x` with the bare `##` marker (>1 token) or fail dangling.
TEST(Preprocessor, FC15PlacemarkerMidChain) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define J3(a,b,c) a ## b ## c\nJ3(x,,z)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 1u) << "x ## pm ## z -> xz";
    EXPECT_EQ(lexs[0], "xz");
}

// (8) GNU comma-elision, EMPTY __VA_ARGS__ (the primary pin): `LOG(42)` ->
// `f(42)` — the separator before `## __VA_ARGS__` is DROPPED. RED-ON-DISABLE
// (flag off / elision block removed): the comma survives via the standard
// placemarker rule (`, ## <pm>` -> `,`) -> 5 tokens `f ( 42 , )`.
TEST(Preprocessor, FC15GnuCommaElisionEmptyVaArgs) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define LOG(fmt, ...) f(fmt, ## __VA_ARGS__)\nLOG(42)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 4u) << "expected: f ( 42 ) -- the comma elided";
    EXPECT_EQ(lexs[0], "f");
    EXPECT_EQ(lexs[1], "(");
    EXPECT_EQ(lexs[2], "42");
    EXPECT_EQ(lexs[3], ")");
}

// (9) GNU comma-elision, NON-empty __VA_ARGS__: `LOG(7, 1, 2)` -> `f(7, 1, 2)` —
// the comma is KEPT and the `##` does NOT paste (`,1` would be two tokens / a
// malformed paste). RED-ON-DISABLE: pasting `,` with `1` trips P_PreprocessorPaste
// or mangles the stream.
TEST(Preprocessor, FC15GnuCommaElisionNonEmptyVaArgs) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define LOG(fmt, ...) f(fmt, ## __VA_ARGS__)\nLOG(7, 1, 2)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    ASSERT_EQ(lexs.size(), 8u) << "expected: f ( 7 , 1 , 2 )";
    EXPECT_EQ(lexs[2], "7");
    EXPECT_EQ(lexs[3], ",") << "the separator is KEPT when __VA_ARGS__ is non-empty";
    EXPECT_EQ(lexs[4], "1") << "no paste between the comma and the first arg";
    EXPECT_EQ(lexs[5], ",");
    EXPECT_EQ(lexs[6], "2");
}

// (10) MUST-FIX-1 pin: an empty `__VA_ARGS__` in a `## __VA_ARGS__` position whose
// left neighbor is NOT a separator (so comma-elision does NOT apply) still becomes
// a PLACEMARKER -> `K(x)` = `x ## <empty __VA_ARGS__>` -> `x`. RED-ON-DISABLE:
// reverting the vaArgs fall-through to `stampArg` (not `stampArgOrPM`) drops the
// empty operand -> dangling `##` -> P_PreprocessorPaste.
TEST(Preprocessor, FC15PasteEmptyVaArgsIsPlacemarkerNotDangling) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define K(p, ...) p ## __VA_ARGS__\nK(x)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste))
        << "x ## <empty __VA_ARGS__> is a placemarker, NOT a dangling ##";
    ASSERT_EQ(lexs.size(), 1u) << "x ## placemarker -> x";
    EXPECT_EQ(lexs[0], "x");
}

// (11) AGNOSTICISM pin: comma-elision is CONFIG-driven. Rebind the shipped
// c-subset's `variadicCommaElision` to false and re-preprocess: the comma now
// SURVIVES (standard placemarker) -> `f ( 42 , )`. RED-ON-DISABLE: if the engine
// hardcoded the elision (ignoring the flag), the comma would vanish even at false.
TEST(Preprocessor, FC15GnuCommaElisionIsConfigDriven) {
    std::string text = loadShippedCSubsetText();
    ASSERT_FALSE(text.empty());
    const std::string from = "\"variadicCommaElision\": true";
    const std::string to   = "\"variadicCommaElision\": false";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos) << "config no longer carries the flag";
    text.replace(pos, from.size(), to);
    auto loaded = GrammarSchema::loadFromText(text, "<no-elision-c-subset>");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<none>" : loaded.error()[0].message);
    ASSERT_FALSE((*loaded)->preprocess().variadicCommaElision);

    namespace fs = std::filesystem;
    auto buf = SourceBuffer::fromString(
        std::string{"#define LOG(fmt, ...) f(fmt, ## __VA_ARGS__)\nLOG(42)\n"},
        "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, *loaded, noDirs);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    std::vector<std::string> lexs;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
    }
    ASSERT_EQ(lexs.size(), 5u) << "without elision: f ( 42 , )";
    EXPECT_EQ(lexs[3], ",") << "the separator survives without GNU comma-elision";
}

// (12) Fail-loud preserved for OBJECT-like macros: `#define OBJ a ##` (a genuine
// dangling `##` -- no operand token at the END of the replacement list) must STILL
// fail loud, now that the object-like arm routes through `collapsePastes`.
TEST(Preprocessor, FC15ObjectLikeDanglingPasteFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define OBJ a ##\nOBJ\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste))
        << "a `##` at the end of an OBJECT-like replacement must fail loud";
}

// ============================================================================
// c17 (D-PP-CONDITIONAL-INCLUDE-ORDERING): the SynthBuilder pre-scan makes the
// conditional pass skip DEAD `#if` branches BEFORE the include splice + the
// global tokenize. Two symptoms, both closed:
//   P0016 -- a quote-`#include` inside `#if 0`/`#if SQLITE_OS_WIN` is no longer
//            resolved (a missing dead-branch header no longer errors);
//   P000E -- a `P_IllegalChar` (`$ @ ``) inside a DEAD branch is suppressed,
//            while an ACTIVE one (a live body, a `#define`/`#if` line, a
//            `#`-stringized arg, an uninvoked live macro body) STILL reports
//            (the FIX-1 dead-region oracle keys on the source BYTE's liveness).
// Every assertion is RED-ON-DISABLE. The completeness pins (tests 2/4/4b/6)
// prove the fix did not over-suppress.
// ============================================================================

// (1) P0016 core: a quote-`#include` of a NONEXISTENT header inside `#if 0` is
// elided -- NO P_PreprocessorIncludeError -- and the rest of the file parses.
// RED-ON-DISABLE: dropping the SynthBuilder `includeResolvable()` gate on the
// quote-include resolution re-resolves the dead-branch include -> the missing
// "nope.h" errors.
TEST(Preprocessor, DeadBranchQuoteIncludeDoesNotError) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 0\n#include \"nope.h\"\n#endif\nint x;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a quote-#include inside #if 0 must NOT be resolved (no missing-file "
           "error)";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "the dead-branch include must not emit an include error";
    ASSERT_EQ(lexs.size(), 3u) << "expected only: int x ;";
    EXPECT_EQ(lexs[1], "x");
}

// (2) COMPLETENESS / FAIL-LOUD: a quote-`#include` of a NONEXISTENT header in a
// LIVE `#if 1` branch STILL errors loud. RED-ON-DISABLE: gating the include on
// the WRONG predicate (always-skip) would silence this real missing-include.
TEST(Preprocessor, LiveBranchQuoteIncludeStillErrorsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#if 1\n#include \"nope.h\"\n#endif\nint x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "a LIVE-branch missing quote-#include must STILL fail loud";
}

// (3) P000E core: illegal characters (`$ @ ``) inside `#if 0` are suppressed.
// RED-ON-DISABLE: dropping the dead-region promotion (forwarding every
// provisional P_IllegalChar unconditionally) re-errors the dead `$`/`@`.
TEST(Preprocessor, DeadBranchIllegalCharDoesNotError) {
    PreprocessResult r;
    auto lexs = ppLexemes("#if 0\n$ @ `\n#endif\nint x;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "illegal chars inside #if 0 must be elided (no P_IllegalChar)";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "no illegal-char diagnostic for a dead branch";
    ASSERT_EQ(lexs.size(), 3u) << "expected only: int x ;";
    EXPECT_EQ(lexs[1], "x");
}

// (4) COMPLETENESS / FAIL-LOUD (§A.4 pin): a BARE illegal char in a LIVE `#if 1`
// body STILL errors. RED-ON-DISABLE: a too-broad dead-region (suppressing live
// bytes) would silence this.
TEST(Preprocessor, ActiveIllegalCharStillErrorsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#if 1\n$\n#endif\nint x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "a LIVE-branch illegal char must STILL fail loud";
}

// (4b) ★ FIX-1 PROOF (the dead-region oracle, NOT the survival oracle): an
// illegal char on an ACTIVE `#define` LINE still errors. The `$` is consumed by
// the directive line (it never survives into the final token stream), so the
// REJECTED "Error token survived" oracle would WRONGLY drop it. The dead-region
// oracle reports it because its source byte is in a LIVE region.
// RED-ON-DISABLE: switching the promotion to the survival oracle drops this.
TEST(Preprocessor, ActiveIllegalCharOnDefineLineStillErrors) {
    PreprocessResult r;
    (void)ppLexemes("#define A 1 $\nint x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "an illegal char on an ACTIVE #define line must STILL error (it is "
           "consumed by the directive, so only the BYTE-liveness oracle catches "
           "it -- the survival oracle would wrongly drop it)";
}

// (4c) FIX-1 (the `#`-stringize variant): an illegal char in a STRINGIZED macro
// argument still errors. c-subset declares `#` (HashOp), so `#define S(x) #x` +
// `S($)` consumes the `$` into a `#`-product string -- the original `$` token
// does NOT survive, so again only the dead-region (byte-liveness) oracle catches
// it. RED-ON-DISABLE: the survival oracle drops it. (If `#` were out of c-subset
// scope this case would be covered generically by the same byte-liveness
// predicate and could be skipped.)
TEST(Preprocessor, ActiveIllegalCharInStringizedArgStillErrors) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define S(x) #x\nint y = S($);\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "an illegal char in a (live) #-stringized argument must STILL error";
    // DISCRIMINATOR vs the survival oracle: the `$` was CONSUMED into the
    // `#`-stringize product (a `"$"` string literal), so it does NOT survive as a
    // standalone Error token -- yet P_IllegalChar still fired. That co-occurrence
    // is what only the byte-liveness oracle (not the survival oracle) achieves.
    bool stringizedDollarPresent = false;
    for (auto const& s : lexs) {
        if (s.find('$') != std::string::npos) stringizedDollarPresent = true;
    }
    EXPECT_TRUE(stringizedDollarPresent)
        << "the `$` must appear inside the #-stringized product (proving it was "
           "consumed, not surviving as a token) -- so the survival oracle would "
           "have seen nothing while the byte-liveness oracle still reports it";
}

// (4d) FIX-1 (the uninvoked-live-macro-body variant; an EXPLICIT pinned choice):
// an illegal char in the replacement of a LIVE-region `#define` that is NEVER
// invoked STILL errors. The `$` byte is in a live region (the `#define` line),
// so the byte-liveness oracle reports it -- matching today's behavior (the
// tokenizer sees every byte of the synth buffer). RED-ON-DISABLE: the survival
// oracle would drop it (an uninvoked macro body never reaches finalTokens).
TEST(Preprocessor, ActiveUninvokedMacroBodyIllegalCharStillErrors) {
    PreprocessResult r;
    (void)ppLexemes("#define M $\nint x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "an illegal char in an uninvoked LIVE macro body still errors (its "
           "byte is in a live region) -- an explicit, asserted choice";
}

// (5) P0016 via `#ifdef`: a quote-`#include` guarded by `#ifdef SQLITE_OS_WIN`
// (UNDEFINED) is skipped (the SQLite cross-compile pattern). RED-ON-DISABLE: the
// include gate off -> the missing header errors.
TEST(Preprocessor, DeadBranchViaDefinedMacroSkipsInclude) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#ifdef SQLITE_OS_WIN\n#include \"os_win.h\"\n#endif\nint x;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "#ifdef of an UNDEFINED macro must skip its quote-#include";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError));
    ASSERT_EQ(lexs.size(), 3u) << "expected only: int x ;";
    EXPECT_EQ(lexs[1], "x");
}

// (6) COMPLETENESS / localMacros tracking: a LIVE-branch `#define MYOS 1` makes
// a following `#if MYOS` guard LIVE, so its quote-`#include` of a MISSING header
// DOES error -- proving the pre-scan tracks `#define`s in `localMacros` and the
// include gate is then ON. RED-ON-DISABLE: not tracking the `#define` (MYOS->0)
// would WRONGLY skip the include and SILENCE this missing-header error.
TEST(Preprocessor, DefineMakesIfBranchLiveSoIncludeErrors) {
    PreprocessResult r;
    (void)ppLexemes(
        "#define MYOS 1\n#if MYOS\n#include \"still_missing.h\"\n#endif\n"
        "int x;\n",
        r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "a #define-driven LIVE #if must resolve (and error on) its missing "
           "quote-#include -- proves localMacros tracking + the live include gate";
}

// (FIX-3) the CONSERVATIVE fallback: a guard that INVOKES a FUNCTION-LIKE macro
// (the pre-scan's weaker eval cannot fold it) takes the P0016-safe direction --
// the quote-`#include` is SKIPPED, so a missing header does NOT error (a
// wrongly-skipped LIVE include would instead fail loud downstream as a missing
// symbol, never a silent wrong include). RED-ON-DISABLE: removing the
// function-like-invocation detection lets the include resolve -> the missing
// header errors (P0016 returns in the uncertain direction).
TEST(Preprocessor, FunctionLikeMacroGuardSkipsIncludeConservatively) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define ENABLED(x) x\n#if ENABLED(1)\n#include \"nope_fn.h\"\n#endif\n"
        "int x;\n",
        r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "a function-like-macro guard must take the conservative skip (no "
           "missing-include error) -- the FIX-3 P0016-safe direction";
    (void)lexs;
}

// AGNOSTICISM pin (RED-ON-DISABLE): the dead-branch include skip is driven by
// the CONFIG conditional words, not a hard-coded "if". Rebind `ifDirective` to
// "whenever" and reload: a quote-`#include` inside `#whenever 0` must STILL be
// skipped (no missing-file error), proving the pre-scan reads the directive word
// from config. RED-ON-DISABLE: hard-coding "if" makes `#whenever 0` an unknown
// directive that does NOT conditionalize -> the include resolves -> the missing
// header errors.
TEST(Preprocessor, DeadBranchIncludeSkipIsConfigDrivenNotHardcoded) {
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    auto schema = reboundCSubset("\"ifDirective\":         \"if\"",
                                 "\"ifDirective\":         \"whenever\"",
                                 "<rebound-if-c17>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().ifDirective, "whenever");

    auto buf = SourceBuffer::fromString(
        std::string{"#whenever 0\n#include \"nope.h\"\n#endif\nint x;\n"},
        "main.c");
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "the dead-branch include skip must use the CONFIG conditional word "
           "(#whenever), not a hard-coded #if";
}

// A `#if 0` block combining ALL c17 symptoms (the corpus pattern in unit form):
// illegal chars `$ @ ``, a quote-`#include` of a missing header, AND a nested
// `#ifdef SQLITE_OS_WIN #include` -- the whole group elides cleanly.
TEST(Preprocessor, DeadBranchCombinedGarbageAndIncludeElides) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 0\n"
        "$ @ `\n"
        "#include \"does_not_exist.h\"\n"
        "#ifdef SQLITE_OS_WIN\n"
        "#include \"os_win.h\"\n"
        "#endif\n"
        "#endif\n"
        "int x;\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a dead branch with illegal chars + missing includes must elide "
           "with NO diagnostics";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_IllegalChar));
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError));
    ASSERT_EQ(lexs.size(), 3u) << "expected only: int x ;";
    EXPECT_EQ(lexs[1], "x");
}

// Nested-dead suppression: an illegal char inside a TAKEN-looking inner branch
// that is ENCLOSED by a dead `#if 0` must still be suppressed (the inner branch
// is dead because its enclosing context is dead). RED-ON-DISABLE: a per-frame
// (rather than whole-stack) dead test would wrongly treat the inner #else as
// live and re-error the `$`.
TEST(Preprocessor, NestedDeadBranchIllegalCharSuppressed) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 0\n#if 1\n$\n#else\n@\n#endif\n#endif\nint x;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "illegal chars in a dead-enclosed nested conditional must be elided";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_IllegalChar));
    ASSERT_EQ(lexs.size(), 3u) << "expected only: int x ;";
    EXPECT_EQ(lexs[1], "x");
}

// The LIVE arm of a conditional keeps its illegal char an ERROR while the DEAD
// arm's is suppressed -- the two arms are treated independently by byte. `#if 1`
// -> `$` in the then-arm errors; the `#else` `@` is dead + suppressed.
TEST(Preprocessor, LiveArmErrorsDeadArmSuppressedInSameGroup) {
    PreprocessResult r;
    (void)ppLexemes("#if 1\n$\n#else\n@\n#endif\nint x;\n", r);
    // Exactly the live `$` reports; the dead `@` does not. We assert at least
    // the live one fires AND that suppression did not silence it.
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "the LIVE arm's illegal char must report";
    // Count: there must be exactly ONE illegal-char diagnostic (the live `$`),
    // proving the dead `@` was suppressed (not 2).
    int illegalCount = 0;
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == DiagnosticCode::P_IllegalChar) ++illegalCount;
    }
    EXPECT_EQ(illegalCount, 1)
        << "exactly the LIVE `$` reports; the DEAD `@` is suppressed";
}

// ============================================================================
// c17 Option 1 (authoritative dead-regions): the dead-branch `P_IllegalChar`
// suppression is keyed on the AUTHORITATIVE `MacroExpander` pass's liveness
// (full `table_`+`predefined_`), NOT a pre-scan that cannot see predefined or
// header-supplied macros. These pin the silent-miscompile the pre-scan oracle
// shipped (a predefined-macro-guarded LIVE branch wrongly recorded dead).
// ============================================================================

// ★ THE PROVEN c17 SILENT MISCOMPILE, now fixed. `#if __STDC__` is a PREDEFINED-
// macro guard: the SynthBuilder pre-scan never sees predefined macros, so it
// folds `__STDC__` -> 0 and calls the branch DEAD -- but the real macro pass
// materializes `__STDC__` = 1, so the branch is LIVE. A `$` on the live `#define`
// line is CONSUMED by the directive (it reaches no token stream), so ONLY a
// byte-liveness oracle keyed on the AUTHORITATIVE pass can catch it. Before
// Option 1 (the pre-scan dead-region oracle) this compiled SILENTLY. RED-ON-
// DISABLE: revert the oracle to the pre-scan's `deadRegions` and this `$` is
// silently dropped again (verified: the pre-scan records the whole `#if __STDC__`
// body as dead).
TEST(Preprocessor, PredefinedMacroGuardedLiveIllegalCharStillErrors) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if __STDC__\n#define UNUSED_MACRO $\nint live_in_stdc_branch;\n"
        "#endif\nint x;\n",
        r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "an illegal char in a PREDEFINED-macro-guarded LIVE branch (consumed "
           "by a #define line) must STILL fail loud -- the AUTHORITATIVE oracle "
           "catches it where the pre-scan oracle silently dropped it";
    // GUARD AGAINST FALSE GREEN: prove `#if __STDC__` is genuinely LIVE here, so
    // the assertion above can't pass for the WRONG reason (the branch going dead).
    // The live-branch declaration must survive into the token stream.
    bool sawLiveDecl = false;
    for (auto const& s : lexs) {
        if (s == "live_in_stdc_branch") sawLiveDecl = true;
    }
    EXPECT_TRUE(sawLiveDecl)
        << "`#if __STDC__` must be LIVE (its body reaches the parser); otherwise "
           "the P_IllegalChar above would fire for the wrong reason -- a dead "
           "branch, not a real live illegal char";
}

// An UNTERMINATED dead `#if 0` (no `#endif`): the dead illegal chars up to EOF
// are suppressed (no double-report), but the missing-`#endif` STILL fails loud.
// RED-ON-DISABLE: dropping the EOF dead-span close re-errors the dead `$`/`@`/`` ` ``;
// dropping the unterminated-conditional check silences the structural error.
TEST(Preprocessor, UnterminatedDeadBranchSuppressesCharsButErrorsUnterminated) {
    PreprocessResult r;
    (void)ppLexemes("#if 0\n$ @ `\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "illegal chars in an unterminated dead `#if 0` must be suppressed (the "
           "EOF dead-span close covers them)";
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "the unterminated conditional (missing #endif) must STILL fail loud";
}

// A LIVE-outer / DEAD-inner nest: `#if 1 { $ } #if 0 { @ }`. The authoritative
// recorder must open a dead range ONLY for the inner dead group -- the live-outer
// `$` is in NO dead range and must report. RED-ON-DISABLE: a per-frame (not
// whole-stack) or sloppy boundary recorder swallows the live `$`.
TEST(Preprocessor, LiveOuterDeadInnerNestReportsLiveSuppressesInner) {
    PreprocessResult r;
    (void)ppLexemes("#if 1\n$\n#if 0\n@\n#endif\n#endif\nint x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "the LIVE-outer `$` must report";
    int illegalCount = 0;
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == DiagnosticCode::P_IllegalChar) ++illegalCount;
    }
    EXPECT_EQ(illegalCount, 1)
        << "exactly the live-outer `$` reports; the dead-inner `@` is suppressed";
}

// (FIX-3, the nullopt arm) a guard the pre-scan cannot evaluate as an ICE (an
// unbalanced/malformed expr, NOT a function-like macro) -> nullopt -> uncertain
// -> the quote-`#include` is conservatively SKIPPED (no missing-file error; the
// malformed `#if` itself errors separately). RED-ON-DISABLE: dropping the
// nullopt->uncertain handling lets the include resolve -> P0016 returns.
TEST(Preprocessor, UnevaluableGuardSkipsIncludeConservatively) {
    PreprocessResult r;
    (void)ppLexemes("#if (\n#include \"nope_unp.h\"\n#endif\nint x;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "an unevaluable (nullopt) #if guard must conservatively SKIP its "
           "quote-#include -- the FIX-3 nullopt arm (P0016-safe direction)";
}

// AGNOSTICISM (RED-ON-DISABLE), the `#endif` word: the dead-region CLOSE boundary
// reads `endifDirective` from config, not a hard-coded "endif". Rebind it to
// "endwhile": after `#endwhile` the `#if 0` reactivates, so a following `$` is
// LIVE and must report. RED-ON-DISABLE: hard-coding "endif" leaves `#endwhile`
// unrecognized -> the `#if 0` stays open -> the live `$` is wrongly suppressed.
TEST(Preprocessor, DeadRegionCloseUsesConfigEndifWordNotHardcoded) {
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    auto schema = reboundCSubset("\"endifDirective\":      \"endif\"",
                                 "\"endifDirective\":      \"endwhile\"",
                                 "<rebound-endif-c17>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().endifDirective, "endwhile");

    auto buf = SourceBuffer::fromString(
        std::string{"#if 0\n#endwhile\n$\nint x;\n"}, "main.c");
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_IllegalChar))
        << "the dead-region close must use the CONFIG `#endwhile`, so the `$` "
           "AFTER it is LIVE and reports -- not a hard-coded `#endif`";
}

// ============================================================================
// c18 (positional macro expansion, C 6.10.3): a `#define`/`#undef` affects only
// text AFTER it. run() now FLUSHES the pending body through expand() at each
// table-mutating directive, so a use BEFORE a later same-name `#define` is NOT
// retroactively replaced. (Pre-c18 the whole body was expanded once at EOF with
// the FINAL table -- the bug SQLite's declare-then-`#define name 0` omit pattern
// exposed.) Every test is RED-ON-DISABLE: making isMutatingDirective() always
// return false (reverting to the single end-flush) fails each one.
// ============================================================================

// ★ THE MINIMAL REPRO (confirmed via CLI on the real compiler): a `#define g 0`
// must NOT clobber the EARLIER `int g;`. RED-ON-DISABLE: the single end-flush
// expands `g`->`0` in the declaration -> `int 0 ;` (a parse error downstream).
TEST(Preprocessor, MacroDefineIsNotRetroactive) {
    PreprocessResult r;
    auto lexs = ppLexemes("int g;\n#define g 0\nint x;\n", r);
    ASSERT_EQ(lexs.size(), 6u) << "expected: int g ; int x ;";
    EXPECT_EQ(lexs[1], "g")
        << "the `g` BEFORE `#define g 0` must stay an identifier, not expand to 0";
    EXPECT_EQ(lexs[4], "x");
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_IllegalChar));
}

// A use BEFORE the define stays; a use AFTER expands. Pins both directions in one.
TEST(Preprocessor, MacroDefineAfterUseDoesNotExpandEarlierUse) {
    PreprocessResult r;
    auto lexs = ppLexemes("int a = g;\n#define g 0\nint b = g;\n", r);
    ASSERT_EQ(lexs.size(), 10u) << "int a = g ; int b = 0 ;";
    EXPECT_EQ(lexs[3], "g") << "the use BEFORE the define stays an identifier";
    EXPECT_EQ(lexs[8], "0") << "the use AFTER the define expands";
}

// `#undef` is also positional: a use between `#define X 1` and `#undef X` sees 1;
// a use after `#undef X` sees X again; a use before `#define X 1` stays X.
TEST(Preprocessor, UndefBetweenTwoUsesIsPositional) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "int a = X;\n#define X 1\nint b = X;\n#undef X\nint c = X;\n", r);
    ASSERT_EQ(lexs.size(), 15u);
    EXPECT_EQ(lexs[3], "X")  << "before #define X 1 -> identifier";
    EXPECT_EQ(lexs[8], "1")  << "between #define and #undef -> 1";
    EXPECT_EQ(lexs[13], "X") << "after #undef X -> identifier again";
}

// Redefinition is positional: use after the first define -> 1, use after the
// undef+redefine -> 2.
TEST(Preprocessor, MacroRedefineGivesPositionalValues) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "int a = X;\n#define X 1\nint b = X;\n#undef X\n#define X 2\n"
        "int c = X;\n",
        r);
    ASSERT_EQ(lexs.size(), 15u);
    EXPECT_EQ(lexs[3], "X");
    EXPECT_EQ(lexs[8], "1");
    EXPECT_EQ(lexs[13], "2") << "the redefined value applies to the later use";
}

// ★ THE SQLITE OMIT PATTERN (the c18 driver): declare an API function, then
// `#define name 0` to nullify it in a feature-omit build. The declaration must
// survive as an identifier (a valid function decl), NOT become `void 0(void);`.
TEST(Preprocessor, SqliteOmitPatternDeclareThenNullify) {
    PreprocessResult r;
    auto lexs = ppLexemes("void f(void);\n#define f 0\nint x = 1;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "declare-then-#define-name-0 must leave the declaration intact";
    ASSERT_GE(lexs.size(), 2u);
    EXPECT_EQ(lexs[1], "f")
        << "the declared function name must stay an identifier, not expand to 0";
}

// Multiple defines: uses before any define stay; uses after both expand.
TEST(Preprocessor, MultipleDefinesGivePositionalExpansion) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "int a = X;\n#define X 1\n#define Y 2\nint b = X + Y;\n", r);
    ASSERT_EQ(lexs.size(), 12u);
    EXPECT_EQ(lexs[3], "X")  << "use before the defines stays an identifier";
    EXPECT_EQ(lexs[8], "1");
    EXPECT_EQ(lexs[10], "2");
}

// ★ THE CRUX (plan-lock fix 1): `#`/`##` PRODUCTS minted in DIFFERENT flushes must
// all slice correctly from the final buffer. `productText_` is append-only with
// absolute spans, so a product from flush 1 (`"aa"`) stays valid after later
// mutations + a product from flush 3 (`foobar`). NOTE: unlike the positional tests
// above, this one is NOT red-on-disable w.r.t. reverting the flush (a single
// end-flush makes all products trivially valid); it is a regression guard for the
// MULTI-flush product accounting itself -- it goes red if a refactor made
// `productText_` reset/per-flush-local (a stale span -> empty/garbage lexeme).
TEST(Preprocessor, ProductSpansSurviveAcrossFlush) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define STR(x) #x\n"
        "const char *a = STR(aa);\n"   // product "aa" minted in flush 1
        "#define BB 1\n"                // #define mutation -> flush
        "#define CAT(x,y) x##y\n"       // #define mutation -> flush
        "int b = CAT(foo,bar);\n"       // product foobar minted in a later flush
        "int c = BB;\n"                 // BB -> 1
        "#undef BB\n"                   // #undef mutation -> flush (the erase path)
        "const char *d = STR(zz);\n",   // product "zz" minted AFTER the undef flush
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // A stringize product is tokenized as a string-literal opener `"` + a BODY
    // token (+ implied close) -- see reconstructStringLiteral above -- so we check
    // the distinctive product BODIES (which slice from productText_; an invalid
    // multi-flush span would yield an empty/garbage lexeme, not the exact body).
    auto has = [&](std::string_view s) {
        for (auto const& l : lexs) if (l == s) return true;
        return false;
    };
    EXPECT_TRUE(has("aa"))
        << "the stringize product BODY from the FIRST flush must keep a valid span "
           "after later flushes (and an #if-operand expansion) grow productText_";
    EXPECT_TRUE(has("foobar"))
        << "the paste product minted in a LATER flush must slice correctly";
    EXPECT_TRUE(has("1")) << "BB expands to 1 in the final flush";
    EXPECT_TRUE(has("zz"))
        << "a stringize product minted AFTER a #undef-triggered flush (the erase "
           "path) must also slice correctly -- #define and #undef share the flush "
           "path";
}

// ★ THE SPANNING-CALL EDGE (plan-lock fix 2): a function-like macro CALL whose
// argument list spans a `#define` boundary is split by the flush -> collectArgs
// hits end-of-flush -> FAIL LOUD (unterminated argument list), never a silent
// mis-expansion. Pins the documented edge as red-on-disable.
TEST(Preprocessor, FunctionLikeCallSpanningDefineFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes(
        "#define FOO(a,b) a b\nint q = FOO(1,\n#define X 9\n2);\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroArgument))
        << "a function-like call whose args span a #define must fail loud "
           "(unterminated argument list), never silently mis-expand";
}

// The OTHER spanning sub-case (audit follow-up): only the function-like macro NAME
// precedes the `#define` (its `(` is after). The first flush sees the bare name
// with no `(`, so it is emitted VERBATIM (not silently expanded); the call is then
// rejected downstream at the parser. Pin: the name survives unexpanded at the
// preprocess stage (no silent mis-expansion), and no unterminated-arg error fires
// here (collectArgs is never reached -- distinguishing this from the case above).
TEST(Preprocessor, FunctionLikeMacroNameOnlyAtDefineBoundaryNotMisexpanded) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define F(a) a\nint q = F\n#define X 1\n(5);\n", r);
    bool sawF = false;
    for (auto const& l : lexs) if (l == "F") sawF = true;
    EXPECT_TRUE(sawF)
        << "the bare macro name at a #define boundary must be emitted VERBATIM "
           "(not silently expanded) -- its `(` is on the far side of the directive";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroArgument))
        << "collectArgs is never reached (no `(` in the name's flush), so no "
           "unterminated-argument error -- unlike the name+`(`-in-flush case";
}

// Positional expansion inside a LIVE `#if` branch: a use before the `#define` (but
// inside the same live conditional) stays an identifier; a use after expands. Pins
// that the positional flush composes with a non-empty conditional stack.
TEST(Preprocessor, PositionalDefineInsideLiveIfBranch) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 1\nint a = X;\n#define X 5\nint b = X;\n#endif\n", r);
    ASSERT_EQ(lexs.size(), 10u) << "int a = X ; int b = 5 ;";
    EXPECT_EQ(lexs[3], "X") << "use before the #define (in a live #if) stays";
    EXPECT_EQ(lexs[8], "5") << "use after the #define (in a live #if) expands";
}

// ── c105: --define user macros (D-PP-USER-DEFINE) + function-like predefined
//    macros (D-PP-FUNCTION-LIKE-PREDEFINE) — the "<command-line>"/"<built-in>"
//    prologue mechanism ─────────────────────────────────────────────────────

// Run preprocess with user --define entries (+ optional active format).
[[nodiscard]] static std::vector<std::string> ppLexemesWithDefines(
        std::string text, std::vector<std::string> const& defines,
        PreprocessResult& out,
        std::optional<ObjectFormatKind> fmt = std::nullopt) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(std::move(text), "main.c");
    std::vector<std::filesystem::path> noDirs;
    out = preprocess(buf, schema, noDirs, {}, fmt, defines);
    std::vector<std::string> lexs;
    for (Token const& t : out.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{out.synthBuffer->slice(t.span)});
    }
    return lexs;
}

// `--define FOO=2` is an ORDINARY macro seeded before the first source line
// (the gcc -D model): it expands in the source, and `#undef FOO` WORKS (a
// predefined_-seeded macro would fail loud on the #undef — this pin locks the
// ordinary-table contract). RED-ON-DISABLE: dropping the prologue emission
// leaves FOO an identifier.
TEST(Preprocessor, UserDefineSeedsOrdinaryUndefableMacro) {
    PreprocessResult r;
    auto lexs = ppLexemesWithDefines(
        "int a = FOO;\n#undef FOO\nint b = FOO;\n", {"FOO=2"}, r);
    ASSERT_EQ(lexs.size(), 10u) << "int a = 2 ; int b = FOO ;";
    EXPECT_EQ(lexs[3], "2")   << "--define FOO=2 expands before the #undef";
    EXPECT_EQ(lexs[8], "FOO") << "after #undef the name is a bare identifier";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "a --define macro is ORDINARY: #undef must not trip the 6.10.8.1 guard";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroRedefinition));
}

// A value-less `--define BAR` defaults to 1 (the -D convention).
TEST(Preprocessor, UserDefineWithoutValueDefaultsToOne) {
    PreprocessResult r;
    auto lexs = ppLexemesWithDefines("int a = BAR;\n", {"BAR"}, r);
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "1");
}

// C 6.10.3p2 duplicate policy rides the ordinary #define handler: an IDENTICAL
// duplicate --define is tolerated silently; a CONFLICTING one is loud.
TEST(Preprocessor, UserDefineDuplicatePolicyIsC61032) {
    PreprocessResult ok;
    (void)ppLexemesWithDefines("int a = X;\n", {"X=3", "X=3"}, ok);
    EXPECT_FALSE(hasPPCode(ok, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "identical duplicate --define is idempotent (C 6.10.3p2)";
    PreprocessResult bad;
    (void)ppLexemesWithDefines("int a = X;\n", {"X=3", "X=4"}, bad);
    EXPECT_TRUE(hasPPCode(bad, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "conflicting duplicate --define must fail loud";
}

// A --define naming a CONFIG PREDEFINED macro (here `__STDC__`) trips the
// C 6.10.8.1 guard — a user may not silently flip a profile macro (the
// _MSC_VER/_WIN32 silent-miscompile channel).
TEST(Preprocessor, UserDefineCollidingWithConfigPredefineIsLoud) {
    PreprocessResult r;
    (void)ppLexemesWithDefines("int a = 0;\n", {"__STDC__=0"}, r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "--define of a predefined macro must fail loud, not override";
}

// c105 (D-PP-FUNCTION-LIKE-PREDEFINE): the pe-gated `__declspec(x)` → empty
// erase — a params-bearing config predefine lowered through the "<built-in>"
// prologue. The NESTED-paren argument (`align(128)`) is the hard case: the
// arg-eater must balance parens, leaving `int x ;` exactly. Also pins the
// declaration-position cleanliness of `__declspec(dllexport)`.
TEST(Preprocessor, FunctionLikePredefineErasesArgsOnPe) {
    PreprocessResult r;
    auto lexs = ppLexemesWithDefines(
        "__declspec(align(128)) int x;\n__declspec(dllexport) int f(void);\n",
        {}, r, ObjectFormatKind::Pe);
    std::vector<std::string> const expect{
        "int", "x", ";", "int", "f", "(", "void", ")", ";"};
    EXPECT_EQ(lexs, expect)
        << "__declspec(...) must erase to nothing on pe, args fully eaten";
}

// The SAME source WITHOUT the pe format: `__declspec` is format-gated
// (availableObjectFormats:["pe"]), so off-pe it stays an ordinary identifier —
// the c9-class per-format filter exercised on the NEW params axis.
TEST(Preprocessor, FunctionLikePredefineOffFormatStaysIdentifier) {
    PreprocessResult r;
    auto lexs = ppLexemesWithDefines(
        "__declspec(align(128)) int x;\n", {}, r, ObjectFormatKind::Elf);
    ASSERT_FALSE(lexs.empty());
    EXPECT_EQ(lexs[0], "__declspec")
        << "off-pe the name must survive verbatim (no erase, no expansion)";
}

// c105 (the MSVC-profile flip): `__int64` is a pe predefine expanding to the
// TWO-token `long long` — `typedef unsigned __int64 T;` must land the exact
// specifier run `unsigned long long` (the multiset row), proving a multi-token
// predefine value re-tokenizes correctly.
TEST(Preprocessor, Int64PredefineExpandsToLongLongOnPe) {
    PreprocessResult r;
    auto lexs = ppLexemesWithDefines(
        "typedef unsigned __int64 dss_u64_t;\n", {}, r, ObjectFormatKind::Pe);
    std::vector<std::string> const expect{
        "typedef", "unsigned", "long", "long", "dss_u64_t", ";"};
    EXPECT_EQ(lexs, expect);
}

// ============================================================================
// C23 (D-PP-ELIFDEF-ELIFNDEF; C 6.10.1): `#elifdef`/`#elifndef`. `#elifdef X`
// == `#elif defined(X)`, `#elifndef X` == `#elif !defined(X)` -- routed through
// the SAME #elif conditional-group state machine with the DIRECT #ifdef-style
// definedness path (never the #if expression evaluator). Before the fix, an
// unrecognized `#elifdef` was silently consumed inside a dead group -> a true
// #elifdef branch was skipped and control fell to #else (a SILENT MISCOMPILE).
// Every assertion is RED-ON-DISABLE (reverting the handleDirective dispatch arms
// makes the elifdef branch fall through wrongly / the directive fail loud).
// ============================================================================
namespace {
// Extract non-trivia lexemes from a PreprocessResult produced with a CUSTOM
// (rebound/stripped) schema -- the config-driven tests below can't use
// `ppLexemes` (which loads the shipped c-subset).
[[nodiscard]] std::vector<std::string> lexemesOf(PreprocessResult const& r) {
    std::vector<std::string> lexs;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
    }
    return lexs;
}
// The message of the FIRST diagnostic carrying `code` ("" if none) -- lets a
// test pin that a malformed `#elifdef` names "elifdef", not "ifdef".
[[nodiscard]] std::string firstMessageWithCode(PreprocessResult const& r,
                                               DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == code) return d.actual;
    }
    return {};
}
} // namespace

// `#elifdef X` takes its branch when X IS defined and an earlier arm did not.
// THE SILENT-MISCOMPILE CORE: the enclosing `#ifdef A` is false (dead), so a
// pre-fix `#elifdef` was silently consumed and #else was wrongly taken (int c).
TEST(Preprocessor, ElifdefTakesBranchWhenDefined) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define B\n#ifdef A\nint a;\n#elifdef B\nint b;\n#else\nint c;\n#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "#elifdef B (B defined) is taken: int b ;";
    EXPECT_EQ(lexs[1], "b")
        << "the #elifdef branch must win, NOT fall through to #else (int c)";
}

// `#elifndef X` takes its branch when X is NOT defined and an earlier arm did
// not: `#ifdef A` false, C undefined -> !defined(C) true -> int b.
TEST(Preprocessor, ElifndefTakesBranchWhenNotDefined) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#ifdef A\nint a;\n#elifndef C\nint b;\n#else\nint c;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "#elifndef C (C undefined) is taken: int b ;";
    EXPECT_EQ(lexs[1], "b");
}

// Completeness (no OVER-take): `#elifdef B` with neither A nor B defined falls
// through to #else (int c).
TEST(Preprocessor, ElifdefFallsThroughToElseWhenNeitherDefined) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#ifdef A\nint a;\n#elifdef B\nint b;\n#else\nint c;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "neither A nor B defined -> #else: int c ;";
    EXPECT_EQ(lexs[1], "c");
}

// Completeness (no OVER-take): `#elifndef C` with C DEFINED is false, so control
// falls through to #else (int c).
TEST(Preprocessor, ElifndefFallsThroughToElseWhenDefined) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define C\n#ifdef A\nint a;\n#elifndef C\nint b;\n#else\nint c;\n"
        "#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u) << "C defined -> #elifndef C false -> #else: int c ;";
    EXPECT_EQ(lexs[1], "c");
}

// THE TAKEN-ONCE KEYSTONE: even though B IS defined, `#elifdef B` must NOT be
// taken because the earlier `#ifdef A` (A defined) already won. Proves the
// elifdef path respects `anyBranchTaken` (the preserved update order).
TEST(Preprocessor, ElifdefSkippedWhenEarlierArmAlreadyTaken) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define A\n#define B\n#ifdef A\nint a;\n#elifdef B\nint b;\n#else\n"
        "int c;\n#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "#elifdef must be recognized (not the unknown-directive fail-loud)";
    ASSERT_EQ(lexs.size(), 3u) << "the FIRST true arm wins: int a ;";
    EXPECT_EQ(lexs[1], "a")
        << "a taken-once group must NOT re-take the true #elifdef (no int b)";
}

// C 6.10.1p6: a DEAD `#elifdef` (an earlier arm already took) is NOT evaluated,
// so a malformed (name-less) `#elifdef` in a dead position emits NO diagnostic.
TEST(Preprocessor, DeadElifdefMissingNameIsNotEvaluated) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define B\n#ifdef B\nint a;\n#elifdef\nint b;\n#else\nint c;\n#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a dead (name-less) #elifdef must not be evaluated -> no error";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "the operand of a dead #elifdef is not evaluated (C 6.10.1p6)";
    ASSERT_EQ(lexs.size(), 3u) << "the taken #ifdef B arm survives: int a ;";
    EXPECT_EQ(lexs[1], "a");
}

// A LIVE malformed `#elifdef` (no macro name) fails LOUD -- and the message
// names "#elifdef", NOT "#ifdef" (Finding 1: the directive spelling is threaded
// through, not hard-coded to the #ifdef family).
TEST(Preprocessor, ElifdefMissingNameFailsLoudWhenLive) {
    PreprocessResult r;
    (void)ppLexemes(
        "#ifdef A\nint a;\n#elifdef\nint b;\n#else\nint c;\n#endif\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "a LIVE #elifdef with no macro name must fail loud";
    std::string const msg =
        firstMessageWithCode(r, DiagnosticCode::P_PreprocessorDirective);
    EXPECT_NE(msg.find("#elifdef"), std::string::npos)
        << "the malformed message must name '#elifdef', not '#ifdef' (got: "
        << msg << ")";
}

// Sibling parity for the ndef spelling (Finding 1): a LIVE malformed `#elifndef`
// names "#elifndef", not "#ifndef" or "#elifdef" (same word-selection path, pinned
// so a future regression that special-cases one spelling can't slip through).
TEST(Preprocessor, ElifndefMissingNameFailsLoudWhenLive) {
    PreprocessResult r;
    (void)ppLexemes(
        "#ifdef A\nint a;\n#elifndef\nint b;\n#else\nint c;\n#endif\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "a LIVE #elifndef with no macro name must fail loud";
    std::string const msg2 =
        firstMessageWithCode(r, DiagnosticCode::P_PreprocessorDirective);
    EXPECT_NE(msg2.find("#elifndef"), std::string::npos)
        << "the malformed message must name '#elifndef' (got: " << msg2 << ")";
}

// `#elifdef` AFTER `#else` fails loud (C 6.10.1p4) -- naming "#elifdef".
TEST(Preprocessor, ElifdefAfterElseFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes(
        "#ifdef A\nint a;\n#else\nint b;\n#elifdef B\nint c;\n#endif\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "a #elifdef after a #else must fail loud";
    EXPECT_NE(firstMessageWithCode(r, DiagnosticCode::P_PreprocessorDirective)
                  .find("#elifdef"),
              std::string::npos)
        << "the after-#else message must name '#elifdef'";
}

// A bare `#elifdef` with NO matching `#if` fails loud AS AN ORPHAN (the
// conditional-directive diagnostic), NOT as an unknown directive -- proving the
// dispatch recognizes it before the unsupported-directive fall-through.
TEST(Preprocessor, BareElifdefWithNoMatchingIfFailsLoudAsOrphan) {
    PreprocessResult r;
    (void)ppLexemes("int x;\n#elifdef B\nint y;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "a #elifdef with no matching #if must fail loud as an orphan";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "an orphan #elifdef is a recognized conditional, NOT an unknown "
           "directive";
    EXPECT_NE(firstMessageWithCode(r, DiagnosticCode::P_PreprocessorDirective)
                  .find("#elifdef"),
              std::string::npos)
        << "the orphan message must name '#elifdef'";
}

// C 6.10.1p1: the `#elifdef` operand is NOT macro-expanded. `#define A B` then
// `#elifdef A` tests whether "A" is defined (yes -> taken, int q), NOT whether
// its expansion "B" is defined (no -> would fall to #else, int r).
TEST(Preprocessor, ElifdefOperandIsNotMacroExpanded) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define A B\n#ifdef X\nint p;\n#elifdef A\nint q;\n#else\nint r;\n"
        "#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u)
        << "defined(A) is true (A is a macro) -> int q ; (NOT the expansion B)";
    EXPECT_EQ(lexs[1], "q")
        << "the operand names A directly; it must not expand A->B and test "
           "defined(B)";
}

// The `#elifdef` word is CONFIG-DRIVEN: rebind it to `elifwhendef`. (1) the new
// spelling conditionalizes; (2) the OLD `#elifdef` (in a live context) is now an
// UNKNOWN directive -- proving the word is read from config, not hard-coded.
TEST(Preprocessor, ElifdefWordIsConfigDrivenNotHardcoded) {
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    auto schema = reboundCSubset("\"elifdefDirective\":    \"elifdef\",",
                                 "\"elifdefDirective\":    \"elifwhendef\",",
                                 "<rebound-elifdef>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().elifdefDirective, "elifwhendef");
    // (1) `#elifwhendef B` now takes its branch (B defined, #ifdef A false).
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#define B\n#ifdef A\nint a;\n#elifwhendef B\nint b;\n"
                        "#else\nint c;\n#endif\n"},
            "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        auto lexs = lexemesOf(r);
        ASSERT_EQ(lexs.size(), 3u) << "#elifwhendef B is taken: int b ;";
        EXPECT_EQ(lexs[1], "b");
    }
    // (2) The OLD `#elifdef` (in a LIVE #ifdef A branch) is now unknown -> fails
    // loud as unsupported (it no longer conditionalizes).
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#define A\n#ifdef A\nint a;\n#elifdef B\nint b;\n"
                        "#endif\n"},
            "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "with elifdef rebound, a literal #elifdef in a live branch is an "
               "unknown directive -- proving the word is read from config";
    }
}

// When the `elifdefDirective` field is STRIPPED (absent), the config declares no
// `#elifdef` -> a `#elifdef` in a LIVE branch falls through to the generic
// unsupported-directive fail-loud (never a silent branch skip). Mirrors the
// pragma opt-out pin (Finding 3: absent config is provably inert).
TEST(Preprocessor, ElifdefIsConfigDrivenFailsLoudWhenStripped) {
    namespace fs = std::filesystem;
    auto schema = reboundCSubset("\"elifdefDirective\":    \"elifdef\",", "",
                                 "<no-elifdef-c-subset>");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->preprocess().elifdefDirective.empty())
        << "the rebound schema must declare no elifdef directive";
    auto buf = SourceBuffer::fromString(
        std::string{"#define A\n#ifdef A\nint a;\n#elifdef B\nint b;\n#endif\n"},
        "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "with `elifdefDirective` stripped, `#elifdef` must fail loud as an "
           "unsupported directive -- proving the directive word is read from "
           "config, not hard-coded";
}

// SynthBuilder PARITY (live arm): a quote-`#include` inside a LIVE `#elifdef`
// arm is resolved + spliced by the pre-scan (so the include gate agrees with the
// authoritative macro pass). RED-ON-DISABLE: without the SynthBuilder elifdef
// arm the pre-scan leaves the frame in its stale (dead #ifdef) state -> the
// live include is never spliced -> `included_by_elifdef` is missing.
TEST(Preprocessor, ElifdefLiveArmResolvesNestedQuoteInclude) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_elifdef_live_inc";
    fs::create_directories(dir);
    { std::ofstream(dir / "elifdef_live.h", std::ios::binary)
          << "int included_by_elifdef;\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#define FEATURE_B\n"
        "#ifdef FEATURE_A\n"
        "int from_a;\n"
        "#elifdef FEATURE_B\n"
        "#include \"elifdef_live.h\"\n"
        "#else\n"
        "int from_else;\n"
        "#endif\n",
        r, {dir}, {});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    bool hasIncluded = false, hasElse = false, hasFromA = false;
    for (auto const& l : lexs) {
        if (l == "included_by_elifdef") hasIncluded = true;
        if (l == "from_else") hasElse = true;
        if (l == "from_a") hasFromA = true;
    }
    EXPECT_TRUE(hasIncluded)
        << "the LIVE #elifdef arm's quote-#include must be spliced by the "
           "SynthBuilder pre-scan (elifdef parity)";
    EXPECT_FALSE(hasElse) << "the #else arm must be elided";
    EXPECT_FALSE(hasFromA) << "the dead #ifdef arm must be elided";
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// SynthBuilder PARITY (dead arm): a quote-`#include` of a MISSING header inside a
// DEAD `#elifdef` arm (an earlier arm already took) must NOT be resolved -> no
// include error. RED-ON-DISABLE: without the SynthBuilder elifdef arm the
// pre-scan keeps the frame ACTIVE (stale) -> it wrongly resolves the dead-arm
// include -> the missing header errors.
TEST(Preprocessor, DeadElifdefArmSkipsNestedQuoteInclude) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define FEATURE_A\n"
        "#ifdef FEATURE_A\n"
        "int from_a;\n"
        "#elifdef FEATURE_B\n"
        "#include \"no_such_elifdef_header.h\"\n"
        "#else\n"
        "int from_else;\n"
        "#endif\n",
        r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a quote-#include in a DEAD #elifdef arm must NOT be resolved";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "the dead-#elifdef-branch include must not emit an include error";
    bool hasFromA = false;
    for (auto const& l : lexs)
        if (l == "from_a") hasFromA = true;
    EXPECT_TRUE(hasFromA) << "the taken #ifdef arm survives: int from_a ;";
}
