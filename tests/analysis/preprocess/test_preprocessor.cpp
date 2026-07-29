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
#include "core/types/target_schema.hpp"   // TF-C74: per-arch target predefines
#include "tokenizer/tokenizer.hpp"
#include "test_support/golden_file.hpp"   // TF-C85: findCorpusRoot / readFile

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
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

// ── TF-C82 (D-PP-PRAGMA-REGISTRY): the LOADER's guards on `pragmaEffects` ──
//
// The registry mirrors `attributeEffects`, so it inherits that table's hard-won
// rules: a CLOSED verb set (a typo can never silently disarm a row), no
// DUPLICATE key (which row wins must not be decided by iteration order), no
// EMPTY key (an empty prefix matches EVERY pragma and would silently disarm
// `unknownPragmaIsError` wholesale), and no row without a SURFACE to fire on.
// Each sub-case rebinds the shipped config and asserts the load REJECTS.
namespace {
[[nodiscard]] std::vector<ConfigDiagnostic>
loadCSubsetExpectingFailure(std::string const& from, std::string const& to,
                            std::string const& label) {
    std::string text = loadShippedCSubsetText();
    if (text.empty()) {
        ADD_FAILURE() << "could not locate shipped c-subset config";
        return {};
    }
    auto const pos = text.find(from);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "shipped c-subset config no longer carries: " << from;
        return {};
    }
    text.replace(pos, from.size(), to);
    auto loaded = GrammarSchema::loadFromText(text, label);
    if (loaded.has_value()) {
        ADD_FAILURE() << "the load should have FAILED for: " << label;
        return {};
    }
    return loaded.error();
}
[[nodiscard]] bool hasCode(std::vector<ConfigDiagnostic> const& ds,
                           DiagnosticCode code) {
    for (auto const& d : ds) if (d.code == code) return true;
    return false;
}
} // namespace

TEST(Preprocessor, TfC82PragmaEffectsLoaderRejectsUnknownVerb) {
    // The CLOSED verb set, and — like `kEffectVerbs` — the rejection message is
    // DERIVED from the vocabulary rather than restated beside it, so it cannot
    // drift into advertising a verb that no longer loads. This mirror going RED
    // on a vocabulary change is the test working as designed.
    constexpr std::string_view kVerbs[] = {"diagnosticsOnly", "annotationOnly",
                                           "structPacking", "unsupported"};
    auto const ds = loadCSubsetExpectingFailure(
        "\"effect\": \"structPacking\" }", "\"effect\": \"strcutPacking\" }",
        "<bad-pragma-verb>");
    EXPECT_TRUE(hasCode(ds, DiagnosticCode::C_InvalidPreprocess))
        << "a misspelled pragma effect must fail the LOAD — silently defaulting "
           "it would disarm the row, and for `structPacking` that is a wrong "
           "struct layout rather than a lost warning";
    std::string closed;
    for (auto const& d : ds) {
        if (d.message.find("unknown pragma effect") != std::string::npos) {
            closed = d.message;
        }
    }
    ASSERT_FALSE(closed.empty()) << "the rejection must name the closed set";
    for (std::string_view v : kVerbs) {
        EXPECT_NE(closed.find(v), std::string::npos)
            << "the closed-set message omits the ACCEPTED verb '" << v
            << "' — a config author reads exactly this sentence to learn the "
               "vocabulary";
    }
}

TEST(Preprocessor, TfC82PragmaEffectsLoaderRejectsDuplicateAndEmptyPrefix) {
    {
        // DUPLICATE: two rows claiming `pack`. Which wins would be decided by
        // consumer iteration order, and here that is `structPacking` (a real
        // layout) versus `unsupported` (a refusal) — never a cosmetic ambiguity.
        auto const ds = loadCSubsetExpectingFailure(
            "\"prefix\": [\"once\"],                  \"effect\": \"unsupported\" },",
            "\"prefix\": [\"pack\"],                  \"effect\": \"unsupported\" },",
            "<dup-pragma-prefix>");
        EXPECT_TRUE(hasCode(ds, DiagnosticCode::C_InvalidPreprocess))
            << "a prefix bound twice must fail the load";
    }
    {
        // EMPTY: `[]` is a prefix of EVERY pragma, so one such row silently
        // turns the whole registry into a catch-all and disarms the loudness.
        auto const ds = loadCSubsetExpectingFailure(
            "\"prefix\": [\"once\"],                  \"effect\": \"unsupported\" },",
            "\"prefix\": [],                        \"effect\": \"unsupported\" },",
            "<empty-pragma-prefix>");
        EXPECT_TRUE(hasCode(ds, DiagnosticCode::C_InvalidPreprocess))
            << "an empty prefix matches everything and must be rejected";
    }
}

TEST(Preprocessor, TfC82PragmaEffectsLoaderRequiresASurface) {
    // A registry with no `pragmaDirective` AND no `pragmaOperator` is an
    // incomplete contract: every row would read as configured and never fire —
    // the knob-that-lies this loader rejects everywhere else.
    std::string text = loadShippedCSubsetText();
    ASSERT_FALSE(text.empty());
    for (char const* key : {"\"pragmaDirective\":          \"pragma\",",
                            "\"pragmaOperator\":           \"_Pragma\","}) {
        auto const pos = text.find(key);
        ASSERT_NE(pos, std::string::npos) << key;
        text.erase(pos, std::strlen(key));
    }
    auto loaded = GrammarSchema::loadFromText(text, "<registry-no-surface>");
    ASSERT_FALSE(loaded.has_value())
        << "a pragma registry with NO pragma surface must fail the load — the "
           "rows could never fire, so declaring them would be a lie";
    EXPECT_TRUE(hasCode(loaded.error(), DiagnosticCode::C_InvalidPreprocess));
}

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
// literal's token TRIPLE (`stringLiteralExpr = StringStart StringLiteral
// StringEnd`), NOT a single fabricated token -- so `ppLexemes` yields THREE
// entries for it: the opening `"` (StringStart), the body (StringLiteral, whose
// span covers only the bytes BETWEEN the delimiters), and the closing `"`
// (StringEnd). `reconstructStringLiteral` joins them back into the full `"..."`
// for readable assertions. A paste product (`a##b` -> `ab`) is exactly ONE token
// (F1) and yields ONE lexeme.
// Every assertion is RED-ON-DISABLE: without the `#` handling `#x` emits the
// literal `#` token (lexs[0]=="#" not "\""); without the `##` handling `a##b`
// emits three tokens (`a`, `##`, `b`) instead of the single `ab`.
//
// ★ RENEGOTIATED -- D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN. This block used to say
// the pair was TWO entries and that the body's span "excludes the CONSUMED
// closing `\"`". The closer is no longer consumed token-lessly; it is a
// `StringEnd` token of its own, so every stringize/predefined-macro lexeme count
// in this file went UP BY ONE PER STRING PRODUCT. The BODY lexeme is byte-for-
// byte unchanged, which is why no `decodeStringLiteralBody` expectation moved.
// ─────────────────────────────────────────────────────────────────────────────

// Join the StringStart (`"`) + StringLiteral (body) + StringEnd (`"`) TRIPLE
// starting at pp-lexeme index `i` back into the full source-form literal.
//
// ★ D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN: this used to be
//     `lexs[i] + lexs[i + 1] + "\""`
// -- the trailing quote was a HAND-WRITTEN CONSTANT, because the closer had no
// token to read it from. That made this helper one more instance of the
// compensate-at-the-consumer pattern the anchor is about, and it also made the
// assertions WEAKER than they looked: a corrupted or missing closer could never
// fail them, since the closing quote came from this line rather than from the
// token stream. It now READS the closer, so `reconstructStringLiteral` is a real
// round-trip check on all three tokens.
[[nodiscard]] std::string reconstructStringLiteral(
    std::vector<std::string> const& lexs, std::size_t i) {
    if (i + 2 >= lexs.size()) return "<malformed-string-product>";
    return lexs[i] + lexs[i + 1] + lexs[i + 2];   // opener + body + closer
}

TEST(Preprocessor, FC15aStringizeSimple) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define STR(x) #x\nSTR(hello)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // The product `"hello"` is StringStart `"` + StringLiteral `hello` +
    // StringEnd `"`.
    // ★ 2 -> 3 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): the closing `"` of the
    // stringize product is now its own token. The stringize BODY is unchanged --
    // what the `#` operator produces did not change, only how many tokens the
    // literal is spelled with.
    ASSERT_EQ(lexs.size(), 3u)
        << "expected the string-literal triple: \" hello \"";
    EXPECT_EQ(lexs[0], "\"") << "stringize must produce a string-literal opener "
                                "(red-on-disable: a literal `#` here)";
    EXPECT_EQ(lexs[1], "hello");
    EXPECT_EQ(lexs[2], "\"") << "the closing `\"` is a token of its own";
    EXPECT_EQ(reconstructStringLiteral(lexs, 0), "\"hello\"");
}

TEST(Preprocessor, FC15aStringizeEscapes) {
    // C 6.10.3.2p2: a `\` is inserted before each `"` and `\` of a string/char
    // literal in the argument. STR(a "b\c") -> "a \"b\\c\"".
    PreprocessResult r;
    auto lexs = ppLexemes("#define STR(x) #x\nSTR(a \"b\\c\")\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // ★ 2 -> 3 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): the product's closing `"`
    // is its own token. The ESCAPING under test is untouched -- note the body
    // expectation below is byte-identical to before, which is the proof that
    // giving the closer a token did not disturb the escape logic.
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[0], "\"");
    // Body (StringStart is the opening `"`, StringEnd the closing one; the body
    // token covers only what is BETWEEN them):
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
    // ★ 2 -> 3 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): opener + body + closer.
    ASSERT_EQ(lexs.size(), 3u);
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
    // ★ 2 -> 3 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): opener + body + closer.
    ASSERT_EQ(lexs.size(), 3u);
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
    // Output: "a" (StringStart + body + StringEnd)  then  int after ;
    // ★ 5 -> 6 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): ONE extra lexeme, the
    // stringize product's closing `"`. The trailing `int after ;` shifts by one
    // index for the same reason -- the non-contamination property under test is
    // unaffected, only the literal's spelling widened.
    ASSERT_EQ(lexs.size(), 6u) << "expected: \" a \" int after ;";
    EXPECT_EQ(reconstructStringLiteral(lexs, 0), "\"a\"");
    EXPECT_EQ(lexs[3], "int");
    EXPECT_EQ(lexs[4], "after");
    EXPECT_EQ(lexs[5], ";");
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
// ─────────────────────────────────────────────────────────────────────────────
// TF-C59 `#line` (C23 6.10.4 -- D-CPP-LINE-DIRECTIVE). Sets the PRESUMED line,
// and optionally the presumed file, for the lines that FOLLOW. Config-driven
// (`lineDirective`), so an empty field leaves `#line` to the generic
// unsupported-directive fail-loud. Every assertion below is RED-ON-DISABLE.
// ─────────────────────────────────────────────────────────────────────────────

// THE off-by-one that a naive implementation gets wrong: `#line N` numbers the
// line FOLLOWING the directive N -- not the directive's own line.
// RED-ON-DISABLE: dropping the `-1` in `N + physLine - dirLine - 1` yields 101.
TEST(Preprocessor, Tf59LineDirectiveRenumbersFollowingLine) {
    PreprocessResult r;
    //                    line: 1          2
    auto lexs = ppLexemes("#line 100\nint x = __LINE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u) << "expected: int x = 100 ;";
    EXPECT_EQ(lexs[3], "100")
        << "#line 100 must number the FOLLOWING line 100 (not 101, and not the "
           "directive's own physical line)";
}

// Numbering ADVANCES from the directive: two lines later is N+1.
// RED-ON-DISABLE: a fix that pins every following line to N gives 100 twice.
TEST(Preprocessor, Tf59LineDirectiveNumberingAdvances) {
    PreprocessResult r;
    //                    line: 1          2               3
    auto lexs = ppLexemes("#line 100\nint a = __LINE__;\nint b = __LINE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 10u) << "expected: int a = 100 ; int b = 101 ;";
    EXPECT_EQ(lexs[3], "100");
    EXPECT_EQ(lexs[8], "101") << "numbering must ADVANCE from the directive";
}

// C23 6.10.4p3: the file operand is OPTIONAL, and when OMITTED the presumed NAME
// is left UNCHANGED. So a bare `#line N` AFTER a `#line M "f"` must keep "f".
// RED-ON-DISABLE: resetting the name on a bare directive reverts __FILE__ to the
// real buffer name -- the single subtlest rule in the directive.
TEST(Preprocessor, Tf59LineDirectiveOmittedFileLeavesPresumedNameUnchanged) {
    PreprocessResult r;
    auto lexs = ppLexemes("#line 10 \"virtual.c\"\n#line 900\n"
                          "const char* f = __FILE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    bool sawVirtual = false;
    for (auto const& s : lexs) {
        if (s.find("virtual.c") != std::string::npos) sawVirtual = true;
    }
    EXPECT_TRUE(sawVirtual)
        << "a BARE `#line 900` must NOT revert the presumed file name set by the "
           "earlier `#line 10 \"virtual.c\"` (C23 6.10.4p3)";
}

// A `#line` inside an ELIDED conditional branch is skipped with NO diagnostic and
// NO renumbering -- the #define/#include/#pragma/#embed dead-branch parity.
// RED-ON-DISABLE: dispatching before the `stackActive()` gate renumbers to 500.
TEST(Preprocessor, Tf59LineDirectiveInDeadBranchIsInert) {
    PreprocessResult r;
    //                    line: 1      2           3       4
    auto lexs = ppLexemes("#if 0\n#line 500\n#endif\nint x = __LINE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a `#line` in a dead branch must not diagnose";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "4")
        << "a dead-branch `#line` must NOT renumber -- the real line 4 stands";
}

// Fail loud, never silently mis-number: a non-digit operand is rejected. This is
// also the current behaviour for the macro-expanded form (6.10.4p4), pinned by
// D-CPP-LINE-DIRECTIVE-MACRO-OPERAND -- a wrong line number would be exactly the
// silent-wrongness the bar forbids.
TEST(Preprocessor, Tf59LineDirectiveNonDigitOperandFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#line abc\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "a non-digit `#line` operand must FAIL LOUD, never silently renumber";
}

// A missing operand is a constraint violation, not a no-op.
TEST(Preprocessor, Tf59LineDirectiveMissingOperandFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#line\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "`#line` with no operand must FAIL LOUD";
}

// C23 6.10.4p2: the digit sequence is constrained to 1..2147483647 — the range is
// TWO-sided. `#line 0` was silently accepted (making __LINE__ 0) until the
// code-audit caught the one-sided check. RED-ON-DISABLE: drop the `n == 0` arm.
TEST(Preprocessor, Tf59LineDirectiveZeroFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#line 0\nint x = __LINE__;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "`#line 0` is out of the 1..2147483647 range (C23 6.10.4p2)";
}

// Trailing junk after the file operand must be rejected, matching handleEmbed.
// Silently ignoring it would half-honour an unsupported form.
// RED-ON-DISABLE: drop the trailing-token arm.
TEST(Preprocessor, Tf59LineDirectiveTrailingJunkFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#line 5 \"f.c\" garbage\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "tokens after the #line file operand must FAIL LOUD";
}

// A directive may span several PHYSICAL lines via a `\` continuation. `#line N`
// renumbers the line after the directive ENDS, so keying the record on the
// directive's FIRST token made this silently off-by-one (audit finding 1:
// DSS gave 101, gcc gives 100).
// RED-ON-DISABLE: resolve from `dirTok` instead of the line's last token.
TEST(Preprocessor, Tf59LineDirectiveSpanningContinuationIsNotOffByOne) {
    PreprocessResult r;
    //                    line: 1        2      3
    auto lexs = ppLexemes("#line \\\n100\nint x = __LINE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u) << "expected: int x = 100 ;";
    EXPECT_EQ(lexs[3], "100")
        << "a `\\`-continued #line renumbers the line after the directive ENDS "
           "(gcc gives 100); keying on the directive's first token gives 101";
}

// The design's HEADLINE property, which nothing pinned until the code-audit said
// so: records are keyed PER ORIGIN BUFFER, so a `#line` inside an #include'd
// header renumbers only THAT header — the includer's own numbering is untouched.
// RED-ON-DISABLE: replace the per-origin map with one global vector and the
// includer's __LINE__ after the #include wrongly follows the header's directive.
// (moved below — it needs the `fs` alias + `ppText`, declared later in this file)

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

// D-CSUBSET-VLA C1b (2026-07-13): VLA support LANDED (`int a[n]` now RUNS —
// dynamic-stack `sub sp,<size>` + a conditional frame pointer), so `__STDC_NO_VLA__`
// is REMOVED from the predefinedMacros — a VLA-SUPPORTING implementation must NOT
// define it (C11 6.10.8.3 / C23 6.10.9.3). This is the FLIP of the fail-loud-era
// FC175StdcNoVlaDefined pin. RED-ON-DISABLE: re-add the predefinedMacros row → the
// `#ifdef` selects the no_vla arm again (a conformance lie for a VLA-capable impl).
TEST(Preprocessor, VlaSupportedStdcNoVlaUndefined) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#ifdef __STDC_NO_VLA__\nint no_vla;\n#else\nint has_vla;\n#endif\n",
            r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u)
            << "__STDC_NO_VLA__ must be UNDEFINED (VLA supported) -> the has_vla arm";
        EXPECT_EQ(lexs[1], "has_vla");
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes("int v = __STDC_NO_VLA__;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 5u);
        // Undefined object-like macro in a non-#if context stays a bare identifier.
        EXPECT_EQ(lexs[3], "__STDC_NO_VLA__")
            << "__STDC_NO_VLA__ is no longer a predefined macro -> not replaced";
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
// StringLiteral + StringEnd triple (like a stringize product), so we
// reconstruct it.
TEST(Preprocessor, FC15bFileResolvesToSourceName) {
    PreprocessResult r;
    auto lexs = ppLexemes("const char* f = __FILE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // const char * f = " main.c " ;  -> the string product is lexs[5..7].
    // ★ 8 -> 9 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): the materialized literal
    // now carries its closing `"` as a token. `__FILE__`'s VALUE is unchanged --
    // the reconstruction below still reads "main.c", which is what this test is
    // actually about.
    ASSERT_EQ(lexs.size(), 9u)
        << "const char * f = <str-start> <str-body> <str-end> ;";
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
    // ★ 8 -> 9 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): the closing `"` is a
    // token. The BODY index (6) and the 11-char shape below are UNCHANGED --
    // the closer was split off the body, not folded into it, so the decoded
    // `"Mmm dd yyyy"` is still exactly 11 chars. If it ever reads 12, the
    // delimiter leaked into the body.
    ASSERT_EQ(lexs.size(), 9u);
    EXPECT_EQ(lexs[5], "\"") << "__DATE__ is a string-literal product";
    EXPECT_EQ(lexs[7], "\"") << "...terminated by its own closer token";
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
    // ★ 8 -> 9 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN), as for __DATE__. The
    // 8-char decoded shape is unchanged; a 9 would mean the closer leaked in.
    ASSERT_EQ(lexs.size(), 9u);
    EXPECT_EQ(lexs[5], "\"") << "__TIME__ is a string-literal product";
    EXPECT_EQ(lexs[7], "\"") << "...terminated by its own closer token";
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
    // ★ 8 -> 9 (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN): the materialized literal
    // carries its closer token. The fail-loud property under test is unaffected.
    ASSERT_EQ(lexs.size(), 9u);
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
    // 11 ungated (the 7 C 6.10.8 core + the `_BitInt` C1 `__BITINT_MAXWIDTH__` line,
    // D-CSUBSET-BITINT — C23 6.2.5, the mandatory bit-precise max width 8388608 + the
    // FC17.9(h) C23 `#embed` trichotomy macros `__STDC_EMBED_NOT_FOUND__`/`_FOUND__`/
    // `_EMPTY__` = 0/1/2, D-PP-EMBED) + 11 pe-gated = 22: the pe-gated set is the c95
    // Windows selection (_WIN32/_WIN64/__stdcall/__cdecl/__fastcall/WINAPI) + the c105
    // MSVC-profile flip (_MSC_VER/__int64/__forceinline/__declspec) + the legacy
    // single-underscore `_declspec` alias (B1, D-SQLITE-PE64-TESTFIXTURE-FRONTEND) + 2
    // macho-gated (the Darwin platform-selection pair pinned as an EXACT SET below)
    // = 24. `__STDC_NO_THREADS__` is REMOVED ENTIRELY (FC17.9(a) macho trampolines —
    // <threads.h> is COMPLETE on ALL legs), and D-CSUBSET-VLA C1b removed
    // `__STDC_NO_VLA__` (a VLA-supporting impl must not define it).
    //
    // TF-C74 SCOPE NOTE: the per-ARCHITECTURE identity macros (`__aarch64__`,
    // `__x86_64__`, …) are deliberately ABSENT from this list — they live on the
    // TARGET config, not the language, and are merged in at preprocess time. That
    // is why this count did not move in TF-C74. The effective language ⊕ target
    // sets are pinned by `TFC74EffectiveArchPredefinesForShippedTargets`.
    //
    // TF-C83: +6 rows — the identities DSS
    // presents. 5 un-gated (`__DSSCP__` the honest one, plus the dialect claim
    // `__GNUC__`/`__GNUC_MINOR__`/`__GNUC_PATCHLEVEL__` and `__clang__`) and 1
    // macho-gated (`__APPLE_CC__`). 11+5=16 un-gated, 11 pe-gated, 2+1=3
    // macho-gated = 30.
    EXPECT_EQ(pms.size(), 30u)
        << "c-subset declares 16 un-gated + 11 pe-gated + 3 macho-gated predefined macros";
    std::size_t ungated = 0;
    std::size_t peGated = 0;
    std::vector<std::string> machoGatedNames;
    for (auto const& pm : pms) {
        if (pm.availableObjectFormats.empty()) {
            ++ungated;
        } else {
            EXPECT_EQ(pm.availableObjectFormats.size(), 1u)
                << pm.name << " should be gated to exactly one format";
            EXPECT_NE(pm.name, "__STDC_NO_THREADS__")
                << "__STDC_NO_THREADS__ must be REMOVED (threads.h complete on all legs)";
            auto const& fmt = pm.availableObjectFormats.front();
            if (fmt == "macho") {
                machoGatedNames.push_back(pm.name);
            } else {
                ++peGated;
                EXPECT_EQ(fmt, "pe")
                    << pm.name << " should be pe-gated (Windows selection) or macho-gated "
                                  "(Darwin selection) — no other format gate is declared";
            }
        }
    }
    // Darwin platform-selection macros (first macOS sqlite-corpus run, 2026-07-27,
    // D-CSUBSET-DARWIN-PLATFORM-MACROS). EXACT SET, not a count: every portable C
    // program branches on `__APPLE__`, and without it DSS silently compiled the
    // `#else` (Linux) arm of sqlite's `src/test1.c` CPU-count code on a Darwin
    // target — reaching `sysconf(_SC_NPROCESSORS_ONLN)` instead of the `sysctl`
    // arm real Apple toolchains take. Mirrors the c95 `_WIN32`-for-pe precedent.
    std::sort(machoGatedNames.begin(), machoGatedNames.end());
    EXPECT_EQ(machoGatedNames,
              (std::vector<std::string>{"__APPLE_CC__", "__APPLE__", "__MACH__"}))
        << "macho targets must predefine exactly __APPLE__, __MACH__ and (TF-C83) "
           "__APPLE_CC__ — the platform-selection macros clang/gcc define on Darwin; "
           "dropping either of the first two makes every `#ifdef __APPLE__` in portable C "
           "take the wrong branch, and dropping __APPLE_CC__ re-closes the "
           "TargetConditionals.h:342 conjunction that gates the whole Darwin ladder";
    EXPECT_EQ(ungated, 16u)
        << "the 7 C 6.10.8 macros + __BITINT_MAXWIDTH__ (_BitInt C1) + the 3 C23 "
           "__STDC_EMBED_* trichotomy macros (FC17.9(h), D-PP-EMBED) + the 5 TF-C83 "
           "un-gated identity rows (__DSSCP__, __GNUC__, __GNUC_MINOR__, "
           "__GNUC_PATCHLEVEL__, __clang__) are un-gated (every format); "
           "__STDC_NO_VLA__ (D-CSUBSET-VLA C1b) + __STDC_NO_THREADS__ (threads.h "
           "complete on all legs) are both REMOVED";
    EXPECT_EQ(peGated, 11u)
        << "_WIN32/_WIN64/__stdcall/__cdecl/__fastcall/WINAPI (c95) + "
           "_MSC_VER/__int64/__forceinline/__declspec (c105) + _declspec (the legacy "
           "single-underscore MSVC alias; tcl.h's TCL_NORETURN under the pe profile, "
           "D-SQLITE-PE64-TESTFIXTURE-FRONTEND B1) are pe-gated";
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
// TF-C83 — the six rows that make DSS present a
// coherent identity to Apple SDK headers, and the `version` predefine kind that
// derives `__DSSCP__` from the repo-root VERSION file.
// ─────────────────────────────────────────────────────────────────────────────

// The VALUES, as an exact SET, every one MEASURED against
// `/usr/bin/clang -dM -E -x c /dev/null` (2026-07-29):
//   __GNUC__ 4 / __GNUC_MINOR__ 2 / __GNUC_PATCHLEVEL__ 1 / __APPLE_CC__ 6000 /
//   __clang__ 1.
// `__DSSCP__` is the one row with no clang counterpart — it is DSS's own
// identity, packed from VERSION (0.0.2 -> 0*1000000 + 0*1000 + 2 == 2).
//
// WHY VALUES AND NOT JUST PRESENCE: `__GNUC__` alone would satisfy a
// presence-only test while yielding GCC_VERSION 4000000 instead of the truthful
// 4002001 that sqliteInt.h:112 computes.
TEST(Preprocessor, TFC83IdentityPredefineValuesMatchClang) {
    auto c = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(c.has_value());
    std::map<std::string, std::string> got;
    for (auto const& pm : (*c)->preprocess().predefinedMacros) {
        if (pm.name == "__GNUC__" || pm.name == "__GNUC_MINOR__"
            || pm.name == "__GNUC_PATCHLEVEL__" || pm.name == "__APPLE_CC__"
            || pm.name == "__clang__" || pm.name == "__DSSCP__") {
            got[pm.name] = pm.value;
        }
    }
    const std::map<std::string, std::string> want{
        {"__APPLE_CC__", "6000"},     {"__DSSCP__", "2"},
        {"__GNUC_MINOR__", "2"},      {"__GNUC_PATCHLEVEL__", "1"},
        {"__GNUC__", "4"},            {"__clang__", "1"},
    };
    EXPECT_EQ(got, want)
        << "the TF-C83 identity predefines must carry their clang-MEASURED "
           "values; __DSSCP__ must be VERSION (0.0.2) packed to 2";
    // The GCC_VERSION arithmetic sqliteInt.h:112 actually performs.
    EXPECT_EQ(std::stoll(got.at("__GNUC__")) * 1000000
                  + std::stoll(got.at("__GNUC_MINOR__")) * 1000
                  + std::stoll(got.at("__GNUC_PATCHLEVEL__")),
              4002001)
        << "GCC_VERSION must evaluate to 4002001 — shipping __GNUC__ without "
           "its two components would yield 4000000 and misreport the dialect";
}

// `__DSSCP__` FOLLOWS the VERSION file rather than restating a literal. The
// packing is exercised directly with arbitrary version strings — the loader
// calls this same function with the build's own version, so a green run here
// plus the value pin above is the full chain.
//
// ORDERING is the property the encoding exists for: 0.0.2 < 0.1.0 < 1.0.0 must
// hold as INTEGER comparison so `#if __DSSCP__ >= ...` behaves as anyone reading
// it expects.
TEST(Preprocessor, TFC83VersionPackingIsOrderPreserving) {
    const std::vector<long long> w{1000000, 1000, 1};
    auto pack = [&](std::string_view v) {
        auto r = dss::packVersionComponents(v, w);
        EXPECT_TRUE(r.has_value()) << v << ": " << (r ? "" : r.error());
        return r.value_or(-1);
    };
    EXPECT_EQ(pack("0.0.2"), 2);
    EXPECT_EQ(pack("0.1.0"), 1000);
    EXPECT_EQ(pack("1.0.0"), 1000000);
    EXPECT_EQ(pack("4.2.1"), 4002001);   // the GCC_VERSION shape, same encoding
    EXPECT_LT(pack("0.0.2"), pack("0.1.0"));
    EXPECT_LT(pack("0.1.0"), pack("1.0.0"));
    EXPECT_LT(pack("0.0.999"), pack("0.1.0"));

    // RED-ON-DISABLE for the value pin above: a DIFFERENT VERSION produces a
    // DIFFERENT macro body. If `__DSSCP__` were a hard-coded "2" this would be
    // the assertion that could not hold.
    EXPECT_NE(pack("0.0.3"), pack("0.0.2"));
}

// ★ THE BOUND IS DERIVED FROM THE WEIGHTS, NOT HARD-CODED — and it FAILS LOUD.
// Under [1000000,1000,1] a component of 1000 carries into the next field:
// 0.0.1000 would pack to 1000, byte-identical to 0.1.0, and every `#if
// __DSSCP__ >= ...` downstream would silently compare wrongly. That is the
// wrong-value-with-no-diagnostic class this arc keeps closing, so it is an
// ERROR naming the offending component — never a wraparound.
TEST(Preprocessor, TFC83VersionPackingBoundFailsLoud) {
    const std::vector<long long> w{1000000, 1000, 1};
    auto bad = dss::packVersionComponents("0.0.1000", w);
    ASSERT_FALSE(bad.has_value())
        << "0.0.1000 must be REFUSED — it packs to 1000, identical to 0.1.0";
    EXPECT_NE(bad.error().find("component #2"), std::string::npos)
        << "the diagnostic must name the offending component: " << bad.error();
    EXPECT_NE(bad.error().find("1000"), std::string::npos) << bad.error();
    // The collision the bound prevents, stated as the fact it is.
    auto collide = dss::packVersionComponents("0.1.0", w);
    ASSERT_TRUE(collide.has_value());
    EXPECT_EQ(*collide, 1000);

    EXPECT_FALSE(dss::packVersionComponents("1.2000.0", w).has_value())
        << "a middle component at its bound must be refused too";
    // The MOST-significant component has no field above it -> unbounded.
    EXPECT_TRUE(dss::packVersionComponents("9999.0.0", w).has_value())
        << "the leading component has no more-significant neighbour to collide "
           "with and must NOT be bounded";

    // A DIFFERENT declared encoding gets its own correct bound for free —
    // proving 1000 is nowhere in the engine.
    const std::vector<long long> w2{10000, 100, 1};
    EXPECT_FALSE(dss::packVersionComponents("0.0.100", w2).has_value())
        << "under [10000,100,1] the bound is 100, derived from the weights";
    EXPECT_TRUE(dss::packVersionComponents("0.0.99", w2).has_value());
}

// Shape mismatches and malformed version text are load errors, not guesses.
TEST(Preprocessor, TFC83VersionPackingRejectsMalformed) {
    const std::vector<long long> w{1000000, 1000, 1};
    for (auto const* v : {"0.0", "0.0.2.1", "", "0..2", "0.0.x", "1.0.2a"}) {
        EXPECT_FALSE(dss::packVersionComponents(v, w).has_value())
            << "'" << v << "' must be refused, never silently coerced";
    }
    auto countErr = dss::packVersionComponents("0.0", w);
    ASSERT_FALSE(countErr.has_value());
    EXPECT_NE(countErr.error().find("component"), std::string::npos)
        << countErr.error();
}

// LOADER fail-loud for the `version` kind. RED-ON-DISABLE: each corruption must
// make the load FAIL — a tolerated one would ship a macro whose declared
// encoding never ran.
TEST(Preprocessor, TFC83VersionKindLoadFailures) {
    // Match only the FIELDS of the version row (the row also carries a
    // `$comment`, so the whole-object spelling is not stable to match on).
    const std::string good =
        "\"name\": \"__DSSCP__\",           \"kind\": \"version\",  "
        "\"componentWeights\": [1000000, 1000, 1]";
    struct Case { const char* what; std::string to; };
    const std::vector<Case> cases{
        {"componentWeights missing",
         "\"name\": \"__DSSCP__\", \"kind\": \"version\""},
        {"weights ascending (non-injective packing)",
         "\"name\": \"__DSSCP__\", \"kind\": \"version\", "
         "\"componentWeights\": [1, 1000, 1000000]"},
        {"last weight not 1",
         "\"name\": \"__DSSCP__\", \"kind\": \"version\", "
         "\"componentWeights\": [1000000, 1000, 10]"},
        {"weight count != version component count",
         "\"name\": \"__DSSCP__\", \"kind\": \"version\", "
         "\"componentWeights\": [1000, 1]"},
        {"componentWeights on a non-version kind",
         "\"name\": \"__DSSCP__\", \"kind\": \"constant\", \"value\": \"2\", "
         "\"componentWeights\": [1000000, 1000, 1]"},
        {"unknown kind stays loud",
         "\"name\": \"__DSSCP__\", \"kind\": \"vershion\", "
         "\"componentWeights\": [1000000, 1000, 1]"},
    };
    for (auto const& c : cases) {
        std::string text = loadShippedCSubsetText();
        ASSERT_FALSE(text.empty());
        auto const pos = text.find(good);
        ASSERT_NE(pos, std::string::npos)
            << "the __DSSCP__ version entry must be present verbatim";
        text.replace(pos, good.size(), c.to);
        auto loaded = GrammarSchema::loadFromText(text, "<tfc83-version>");
        EXPECT_FALSE(loaded.has_value()) << "must be a load error: " << c.what;
    }
    // Control: the UNMODIFIED text still loads, so the cases above fail for the
    // reason claimed and not because the fixture text is stale.
    auto ok = GrammarSchema::loadFromText(loadShippedCSubsetText(),
                                          "<tfc83-version-control>");
    EXPECT_TRUE(ok.has_value()) << "the shipped c-subset text must still load";
}

// AGNOSTICISM: not one of these macro spellings may appear in engine C++. The
// engine knows the KIND verbs (`version`, `constant`, …) and the config key
// (`componentWeights`); it must never know that `__DSSCP__` or `__GNUC__`
// exist. Guarded here rather than by review because this is exactly the rule a
// well-meaning "just special-case it" patch breaks.
TEST(Preprocessor, TFC83IdentityMacroNamesAreNotInEngineCpp) {
    namespace fs = std::filesystem;
    auto root = fs::path{__FILE__}.parent_path().parent_path().parent_path()
                    .parent_path() / "src";
    ASSERT_TRUE(fs::is_directory(root)) << root.string();
    const std::vector<std::string> banned{"__DSSCP__", "__GNUC__", "__clang__",
                                          "__APPLE_CC__"};
    std::vector<std::string> hits;
    for (auto const& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        auto const p = e.path();
        // Only ENGINE sources. `src/dss-config/` is config — the macro names
        // are supposed to live there, that is the entire point.
        if (p.extension() != ".cpp" && p.extension() != ".hpp") continue;
        if (p.string().find("dss-config") != std::string::npos) continue;
        std::ifstream in(p);
        std::string   line;
        int           n = 0;
        while (std::getline(in, line)) {
            ++n;
            for (auto const& b : banned) {
                if (line.find(b) != std::string::npos) {
                    hits.push_back(p.filename().string() + ":"
                                   + std::to_string(n) + " " + b);
                }
            }
        }
    }
    EXPECT_TRUE(hits.empty())
        << "identity macro spellings must live ONLY in config, never in engine "
           "C++; found: "
        << [&] {
               std::string s;
               for (auto const& h : hits) s += "\n  " + h;
               return s;
           }();
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

// ★★ TF-C82 (D-PP-PRAGMA-REGISTRY) — REWRITTEN, not deleted. THIS TEST USED TO
// PIN THE BUG. As `FC15cPragmaConsumedAndDropped` it asserted
// `EXPECT_FALSE(r.diagnostics->hasErrors())` for `#pragma GCC optimize("O2")`
// and called that correct, citing C 6.10.6p2. The citation was right and the
// conclusion was wrong: 6.10.6p2 licenses IGNORING a pragma, and DSS was not
// ignoring pragmas, it was ignoring the QUESTION — the same silence covered
// `GCC optimize` (harmless here) and `pack(4)` (MEASURED: it makes
// `sys/fcntl.h`'s `struct log2phys` 20 bytes where DSS computed 24, on a live
// `fcntl(F_LOG2PHYS)` path). What is pinned now is the DISTINCTION: a pragma the
// registry CLAIMS is ignored silently, and one it does not is LOUD.
//
// The token assertions are unchanged and still load-bearing in both arms: a
// pragma line is never program text, whatever the verdict on its meaning.
TEST(Preprocessor, TfC82RegisteredInertPragmaIsSilentUnknownIsLoud) {
    // (A) REGISTERED as `diagnosticsOnly` -> ignored, and ignored because a ROW
    // SAYS SO. `clang diagnostic` is the MEASURED most-reached pragma in the
    // sqlite corpus (48 occurrences across 12 TUs).
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#pragma clang diagnostic push\nint v=1;\n", r);
        EXPECT_TRUE(r.diagnostics->all().empty())
            << "a pragma whose `pragmaEffects` row is `diagnosticsOnly` is "
               "ignored with NO diagnostic — the row is the justification";
        ASSERT_EQ(lexs.size(), 5u) << "only `int v = 1 ;` survives";
        EXPECT_EQ(lexs[0], "int");
        EXPECT_EQ(lexs[1], "v");
        EXPECT_EQ(lexs[2], "=");
        EXPECT_EQ(lexs[3], "1");
        EXPECT_EQ(lexs[4], ";");
    }
    // (B) UNREGISTERED -> LOUD. This is the exact input the old test asserted
    // was silent. `GCC optimize` matches no row, and DSS cannot know whether
    // ignoring it is safe, so it says so instead of assuming.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#pragma GCC optimize(\"O2\")\nint v=1;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "an unregistered pragma must be LOUD under "
               "`unknownPragmaIsError` — the whole thesis of this cycle";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "it is a RECOGNIZED directive with an unrecognized payload, not "
               "an unsupported directive — the code must distinguish them";
        ASSERT_EQ(lexs.size(), 5u)
            << "the line is still not program text: a REFUSED pragma emits no "
               "tokens either (a leaked payload would cascade into a parse error "
               "that hides this accurate one)";
        EXPECT_EQ(lexs[0], "int");
        EXPECT_EQ(lexs[4], ";");
    }
    // (C) `unsupported` -> LOUD, and for a DIFFERENT reason than (B): the row
    // exists and states that the pragma has real translation semantics DSS has
    // not built. Both are `P_PreprocessorPragma`; what differs is that this one
    // is a considered answer.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#pragma STDC FP_CONTRACT OFF\nint v=1;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "an `unsupported` row must fail loud, not be silently ignored";
        ASSERT_EQ(lexs.size(), 5u);
    }
}

// ★ TF-C82: the OPT-OUT, and the red-on-disable for the loudness itself.
// `unknownPragmaIsError: false` restores the pre-TF-C82 silent drop EXACTLY —
// so the loud posture is a declared choice a language makes, not a behavior
// baked into the engine. Without this pin, "loud" and "hard-coded" would be
// indistinguishable from the outside.
TEST(Preprocessor, TfC82UnknownPragmaIsErrorFalseRestoresSilence) {
    auto schema = reboundCSubset("\"unknownPragmaIsError\":     true,",
                                 "\"unknownPragmaIsError\":     false,",
                                 "<silent-pragma-c-subset>");
    ASSERT_NE(schema, nullptr);
    ASSERT_FALSE(schema->preprocess().unknownPragmaIsError)
        << "the rebound schema must declare the silent posture";
    auto buf = SourceBuffer::fromString(
        std::string{"#pragma GCC optimize(\"O2\")\nint v=1;\n"}, "main.c");
    std::vector<std::filesystem::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "with `unknownPragmaIsError` false an unregistered pragma is ignored "
           "in silence — C 6.10.6p2's licence, taken deliberately rather than by "
           "omission";
}

// ★★ TF-C82 — `#pragma pack` REALLY APPLIES, and the assertion is the NUMBER.
// `pack` is the one registry row with a layout sink, so the pin is not "it
// compiled": it reads the cap the preprocessor recorded for the tokens inside
// the region, which is the value the semantic tier feeds to the interner.
// MEASURED against clang for the same source: cap 4 inside, none outside.
TEST(Preprocessor, TfC82PragmaPackStampsTheTokensInsideTheRegion) {
    PreprocessResult r;
    auto lexs = ppLexemes("#pragma pack(4)\nint inside;\n"
                          "#pragma pack()\nint outside;\n", r);
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "a well-formed `#pragma pack` is honored, not diagnosed";
    ASSERT_EQ(lexs.size(), 6u) << "`int inside ; int outside ;` survives";

    // Find the two identifier tokens and read their recorded caps.
    std::optional<std::uint32_t> insideCap, outsideCap;
    for (Token const& t : r.tokens) {
        std::string_view const tx = r.synthBuffer->slice(t.span);
        auto const  key = static_cast<std::uint32_t>(t.span.start());
        auto const  it  = r.pragmaPackByOffset.find(key);
        std::uint32_t const cap =
            it == r.pragmaPackByOffset.end() ? 0u : it->second;
        if (tx == "inside")  insideCap  = cap;
        if (tx == "outside") outsideCap = cap;
    }
    ASSERT_TRUE(insideCap.has_value());
    ASSERT_TRUE(outsideCap.has_value());
    EXPECT_EQ(*insideCap, 4u)
        << "a token inside the `pack(4)` region carries the cap 4 — the value "
           "that makes `struct log2phys` 20 bytes instead of 24";
    EXPECT_EQ(*outsideCap, 0u)
        << "`pack()` RESETS to no cap (the depth-less idiom the SDK uses 14 "
           "times); a token after it must carry no cap at all, or every struct "
           "in the rest of the file is silently relaid out";
}

// ★★ TF-C82 — THE HALFWAY-STATE DISCRIMINATOR. `_Pragma` inside a macro
// REPLACEMENT LIST must resolve at EXPANSION time, not at the directive scan.
// Route `_Pragma` at the directive scan only and the file-scope case below stays
// GREEN while this one silently does nothing — a green-looking half-feature. It
// is the `sys/queue.h` shape (`__NULLABILITY_COMPLETENESS_PUSH`, expanded at 40
// use sites) and MEASURED it is how 24 of the corpus's reached pragmas arrive.
TEST(Preprocessor, TfC82PragmaOperatorResolvesAtExpansionNotAtDirectiveScan) {
    // (A) FILE SCOPE — the easy half. Both routings pass this one.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("_Pragma(\"pack(4)\")\nint a;\n", r);
        EXPECT_TRUE(r.diagnostics->all().empty());
        ASSERT_EQ(lexs.size(), 3u) << "`int a ;` — the operator emits nothing";
        bool sawCap = false;
        for (Token const& t : r.tokens) {
            if (r.synthBuffer->slice(t.span) != "a") continue;
            auto const it = r.pragmaPackByOffset.find(
                static_cast<std::uint32_t>(t.span.start()));
            sawCap = it != r.pragmaPackByOffset.end() && it->second == 4u;
        }
        EXPECT_TRUE(sawCap) << "a file-scope `_Pragma(\"pack(4)\")` applies";
    }
    // (B) FROM A MACRO REPLACEMENT LIST — the half that discriminates.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#define PACK4 _Pragma(\"pack(4)\")\n"
                              "PACK4\nint b;\n", r);
        EXPECT_TRUE(r.diagnostics->all().empty());
        ASSERT_EQ(lexs.size(), 3u)
            << "`int b ;` — the expanded `_Pragma` must leave no tokens behind";
        bool sawCap = false;
        for (Token const& t : r.tokens) {
            if (r.synthBuffer->slice(t.span) != "b") continue;
            auto const it = r.pragmaPackByOffset.find(
                static_cast<std::uint32_t>(t.span.start()));
            sawCap = it != r.pragmaPackByOffset.end() && it->second == 4u;
        }
        EXPECT_TRUE(sawCap)
            << "a `_Pragma` reached through a macro must take effect at its "
               "EXPANSION site — this is the assertion a directive-scan-only "
               "routing fails while (A) above still passes";
    }
}

// ★ TF-C82 — ONE REGISTRY, TWO SPELLINGS. `_Pragma("pack(4)")` must produce the
// IDENTICAL state as `#pragma pack(4)`. Give the operator its own table and the
// two drift; the pin compares them directly rather than checking each in
// isolation.
TEST(Preprocessor, TfC82PragmaOperatorAndDirectiveAgree) {
    auto capOf = [](char const* src, char const* name) -> std::uint32_t {
        PreprocessResult r;
        (void)ppLexemes(src, r);
        EXPECT_TRUE(r.diagnostics->all().empty()) << src;
        for (Token const& t : r.tokens) {
            if (r.synthBuffer->slice(t.span) != name) continue;
            auto const it = r.pragmaPackByOffset.find(
                static_cast<std::uint32_t>(t.span.start()));
            return it == r.pragmaPackByOffset.end() ? 0u : it->second;
        }
        return 0xFFFFFFFFu;   // name not found — a broken fixture, never a pass
    };
    std::uint32_t const viaDirective = capOf("#pragma pack(4)\nint z;\n", "z");
    std::uint32_t const viaOperator  = capOf("_Pragma(\"pack(4)\")\nint z;\n", "z");
    EXPECT_EQ(viaDirective, 4u);
    EXPECT_EQ(viaOperator, viaDirective)
        << "the two spellings are one feature routed through one registry; a "
           "divergence here means `_Pragma` grew a table of its own";
}

// ★ TF-C82 — C 6.10.9p1 DE-STRINGIZE: `\"` -> `"` and `\\` -> `\`. The escape
// pass is exactly those two replacements, NOT the general string decoder (which
// would turn a `\n` a pragma legitimately contains into a newline byte).
// `sys/queue.h:225` needs this.
TEST(Preprocessor, TfC82PragmaOperatorDestringizesPerC6109) {
    PreprocessResult r;
    // The operand de-stringizes to: clang diagnostic ignored "-Wfoo"
    auto lexs = ppLexemes(
        "_Pragma(\"clang diagnostic ignored \\\"-Wfoo\\\"\")\nint q;\n", r);
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "after de-stringizing, the pragma matches the `clang diagnostic` row "
           "and is inert; a failure here means the `\\\"` escapes were not "
           "collapsed and the leading words did not match";
    ASSERT_EQ(lexs.size(), 3u) << "`int q ;`";
}

// ★ TF-C82 — AGNOSTICISM. `toy.lang.json` declares no `pragmaOperator`, so
// `_Pragma` there is an ORDINARY IDENTIFIER and survives into the token stream.
// The rebind proves the engine reads the CONFIG word rather than knowing the
// spelling `_Pragma`.
TEST(Preprocessor, TfC82PragmaOperatorIsConfigDrivenOrdinaryIdentifierWhenAbsent) {
    auto schema = reboundCSubset("\"pragmaOperator\":           \"_Pragma\",",
                                 "",
                                 "<no-pragma-operator-c-subset>");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->preprocess().pragmaOperator.empty())
        << "the rebound schema must declare no pragma operator";
    auto buf = SourceBuffer::fromString(
        std::string{"int _Pragma;\n"}, "main.c");
    std::vector<std::filesystem::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "with no `pragmaOperator` declared, `_Pragma` is just an identifier";
    std::size_t seen = 0;
    for (Token const& t : r.tokens) {
        if (r.synthBuffer->slice(t.span) == "_Pragma") ++seen;
    }
    EXPECT_EQ(seen, 1u)
        << "the identifier must reach the parser untouched — the identity-pass "
           "property for a language that declares no pragma operator";
}

// ★ TF-C82 — the `pack` operand forms that were MEASURED reachable, and the ones
// that were not. Building a form nothing uses is how a wrong guess ships; the
// unbuilt forms fail LOUD rather than silently doing nothing.
TEST(Preprocessor, TfC82PragmaPackFormsBuiltAndRefused) {
    struct Row { char const* src; bool loud; char const* why; };
    Row const rows[] = {
        {"#pragma pack(4)\nint x;\n",        false, "pack(N) — MEASURED 14x"},
        {"#pragma pack()\nint x;\n",         false, "pack() reset — MEASURED 14x"},
        {"#pragma pack(push, 4)\n#pragma pack(pop)\nint x;\n",
                                              false, "push/pop — MEASURED 6x"},
        // NOT built, and deliberately: bare `pack(push)` occurs 0 times in the
        // whole macOS SDK, and the paren-less `pragma pack 8` occurs twice (both
        // in an unreached `ffi/ffi.h`). A guess would silently relayout structs.
        {"#pragma pack(push)\nint x;\n",     true,  "bare pack(push) — 0 in SDK"},
        {"#pragma pack 8\nint x;\n",         true,  "paren-less — unreached"},
        // Envelope: the cap lands in the same layout channel `alignas` uses.
        {"#pragma pack(3)\nint x;\n",        true,  "non-power-of-two"},
        {"#pragma pack(512)\nint x;\n",      true,  "over the 256 cap"},
        // An unbalanced pop makes the alignment below it underivable from source.
        {"#pragma pack(pop)\nint x;\n",      true,  "pop with an empty stack"},
        // A push never popped: the `#endif`-balance argument.
        {"#pragma pack(push, 4)\nint x;\n",  true,  "push unbalanced at EOF"},
    };
    for (Row const& row : rows) {
        PreprocessResult r;
        (void)ppLexemes(row.src, r);
        EXPECT_EQ(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma), row.loud)
            << row.why << "  [" << row.src << "]";
    }
}

// ★ TF-C82 — the STACK-form vocabulary is CONFIG. Strip `pragmaPackPushWord` and
// `pack(push, 4)` becomes an unbuilt form (LOUD), while `pack(4)` keeps working:
// the red-on-disable for that pair, and the proof the engine never compares a
// token to a literal `"push"`.
TEST(Preprocessor, TfC82PragmaPackPushWordIsConfigDriven) {
    auto schema = reboundCSubset("\"pragmaPackPushWord\":       \"push\",",
                                 "",
                                 "<no-pack-push-c-subset>");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->preprocess().pragmaPackPushWord.empty());
    std::vector<std::filesystem::path> noDirs;
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#pragma pack(push, 4)\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "with no `pragmaPackPushWord` declared the push form is an "
               "unbuilt form and must be REFUSED, never silently ignored";
    }
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#pragma pack(4)\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "the set/reset forms are independent of the stack vocabulary";
    }
}

// ══ TF-C85: the three rows the pe64 sqlite leg needed, and the `optimize` sink ══
//
// MEASURED at TF-C85 on the real pe64 corpus leg: 2135 `error[P0020]` across 113
// of 189 TUs, partitioned `warning` 1685 / `intrinsic` 448 / `optimize` 2. Every
// one of those is invisible to a macOS `clang -E` census, because `_MSC_VER` is
// never defined there and DSS's `pe` profile DOES define it.
namespace {
// Preprocess `text` under an explicit predefine class. Returns the result by
// out-param so a test can inspect BOTH the diagnostics and the pragma products.
void ppUnderFormat(std::string text, std::optional<ObjectFormatKind> fmt,
                   PreprocessResult& out) {
    auto schema = cSubset();
    auto buf    = SourceBuffer::fromString(std::move(text), "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string>           noDefines;
    out = preprocess(buf, schema, noDirs, {}, fmt, noDefines);
}
// TRUE iff the token spelled `name` was emitted inside a `#pragma optimize("",
// off)` region — the exact lookup the semantic tier performs on a function
// declaration's leftmost token.
[[nodiscard]] bool tokenIsNoOptimize(PreprocessResult const& r,
                                     std::string_view name) {
    for (Token const& t : r.tokens) {
        if (r.synthBuffer->slice(t.span) != name) continue;
        return r.pragmaNoOptimizeByOffset.contains(
            static_cast<std::uint32_t>(t.span.start()));
    }
    ADD_FAILURE() << "token '" << name << "' not found in the output";
    return false;
}
} // namespace

// ★ ONE `["warning"]` ROW, ALL FOUR REACHED PAYLOAD SHAPES. MEASURED in the
// corpus: `disable : N` (msvc.h x15, mutex_w32.c, totype.c), `push`/`pop`
// (mutex_w32.c:40,64) and `default : N` (totype.c:504). The row claims nothing
// about the argument list, which is exactly why one row can cover four shapes —
// and the test asserts all four rather than the one that happens to dominate.
TEST(Preprocessor, TfC85WarningPragmaIsInertInEveryReachedShape) {
    char const* const shapes[] = {
        "#pragma warning(disable: 4127)\n",
        "#pragma warning(push)\n",
        "#pragma warning(pop)\n",
        "#pragma warning(default: 4748)\n",
    };
    for (char const* s : shapes) {
        PreprocessResult r;
        auto lexs = ppLexemes(std::string{s} + "int x;\n", r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "a `diagnosticsOnly` row claims this pragma; it must be silent: "
            << s;
        EXPECT_EQ(lexs.size(), 3u) << "`int x ;` survives, the directive does not";
    }
}

// ★ RED-ON-DISABLE for the `["warning"]` row: rename the PREFIX (the structural
// key, never the prose) and the same pragmas go loud again.
TEST(Preprocessor, TfC85WarningRowIsWhatMakesTheWarningPragmaSilent) {
    auto schema = reboundCSubset("\"prefix\": [\"warning\"]",
                                 "\"prefix\": [\"warningXX\"]",
                                 "<no-warning-row-c-subset>");
    ASSERT_NE(schema, nullptr);
    std::vector<std::filesystem::path> noDirs;
    auto buf = SourceBuffer::fromString(
        std::string{"#pragma warning(disable: 4127)\nint x;\n"}, "main.c");
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
        << "with no row claiming `warning` the pragma is UNREGISTERED and loud — "
           "which is precisely the state that broke pe64 for 1685 of 2135 lines";
}

// ★★ THE `realizationRequestOnly` VERB. Its claim is about the ENGINE, not about
// the pragma's arguments: `#pragma intrinsic` asks HOW a listed name is realized,
// never WHETHER it exists, and a name DSS does not provide fails loud at the CALL
// SITE. That is why one PREFIX row can speak for names this implementation has
// never heard of — asserted here with a deliberately invented name beside the
// four MEASURED-reached ones.
TEST(Preprocessor, TfC85IntrinsicPragmaIsInertForAnyNameList) {
    char const* const lists[] = {
        "#pragma intrinsic(_byteswap_ushort)\n",
        "#pragma intrinsic(_byteswap_ulong)\n",
        "#pragma intrinsic(_byteswap_uint64)\n",
        "#pragma intrinsic(_ReadWriteBarrier)\n",
        // NOT a name DSS knows, and deliberately so: the row's claim spans every
        // name the pragma can list. A per-name claim would be false the moment a
        // new one appeared.
        "#pragma intrinsic(_no_such_intrinsic_anywhere)\n",
        "#pragma intrinsic(_byteswap_ulong, _ReadWriteBarrier)\n",
    };
    for (char const* s : lists) {
        PreprocessResult r;
        auto lexs = ppLexemes(std::string{s} + "int x;\n", r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "the row claims the pragma requests a REALIZATION, so it is "
               "inert regardless of the names listed: " << s;
        EXPECT_EQ(lexs.size(), 3u);
    }
}

TEST(Preprocessor, TfC85IntrinsicRowIsWhatMakesTheIntrinsicPragmaSilent) {
    auto schema = reboundCSubset("\"prefix\": [\"intrinsic\"]",
                                 "\"prefix\": [\"intrinsicXX\"]",
                                 "<no-intrinsic-row-c-subset>");
    ASSERT_NE(schema, nullptr);
    std::vector<std::filesystem::path> noDirs;
    auto buf = SourceBuffer::fromString(
        std::string{"#pragma intrinsic(_byteswap_ulong)\nint x;\n"}, "main.c");
    PreprocessResult r = preprocess(buf, schema, noDirs);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
        << "with no row claiming `intrinsic` the pragma is UNREGISTERED and loud "
           "— 448 of the 2135 pe64 failures";
}

// ★★ THE `optimizerControl` SINK, PREPROCESSOR HALF. A REGION, stamped per
// EMITTED TOKEN — the `#pragma pack` mechanism, for the same reason: a function
// definition arriving from a macro replacement list carries the `#define` line's
// span, so a byte-RANGE lookup would answer "optimize" for exactly the case the
// author was controlling.
TEST(Preprocessor, TfC85OptimizePragmaStampsTheTokensInsideTheRegion) {
    PreprocessResult r;
    auto lexs = ppLexemes("#pragma optimize(\"\", off)\nint inside;\n"
                          "#pragma optimize(\"\", on)\nint outside;\n", r);
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "a well-formed `#pragma optimize` is honored, not diagnosed";
    ASSERT_EQ(lexs.size(), 6u) << "`int inside ; int outside ;` survives";
    EXPECT_TRUE(tokenIsNoOptimize(r, "inside"))
        << "a token inside the region must be stamped — this is the state the "
           "semantic tier folds onto SymbolRecord.isNoOptimize";
    EXPECT_FALSE(tokenIsNoOptimize(r, "outside"))
        << "`optimize(\"\", on)` CLOSES the region; a token after it must carry "
           "nothing, or the whole rest of the file stops being optimized";
}

// ★★ THE SINGLE-DISPATCH PIN. Every failing corpus site is `#pragma`-spelled, so
// a directive-arm-only implementation would go GREEN on the corpus while silently
// destroying the property `D-PP-PRAGMA-OPERATOR-FORM` closed. This asserts the
// `_Pragma` spelling of the NEW row reaches the SAME sink — including from inside
// a macro replacement list, which resolves at EXPANSION time.
TEST(Preprocessor, TfC85OptimizeReachesTheSameSinkThroughThePragmaOperator) {
    {   // file-scope operator form
        PreprocessResult r;
        auto lexs = ppLexemes("_Pragma(\"optimize(\\\"\\\", off)\")\nint a;\n", r);
        EXPECT_TRUE(r.diagnostics->all().empty());
        ASSERT_EQ(lexs.size(), 3u) << "`int a ;` — the operator emits nothing";
        EXPECT_TRUE(tokenIsNoOptimize(r, "a"))
            << "the `_Pragma` spelling must drive the SAME region state as the "
               "`#pragma` spelling — one registry, two spellings";
    }
    {   // ★ THE DISCRIMINATOR: from a macro REPLACEMENT LIST.
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define NOOPT _Pragma(\"optimize(\\\"\\\", off)\")\n"
            "NOOPT\nint b;\n", r);
        EXPECT_TRUE(r.diagnostics->all().empty());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_TRUE(tokenIsNoOptimize(r, "b"))
            << "a `_Pragma` reached through a macro takes effect at its "
               "EXPANSION site; a directive-scan-only routing fails HERE while "
               "the corpus stays green";
    }
}

// ★ THE UNBUILT FORMS FAIL LOUD — the `#pragma pack` posture. A selective option
// list names MSVC optimizations DSS does not have; widening it to "all off" or
// dropping it are both silent wrong answers.
TEST(Preprocessor, TfC85OptimizeUnbuiltFormsAreRefusedNotGuessed) {
    char const* const refused[] = {
        "#pragma optimize(\"gt\", off)\n",   // selective option list
        "#pragma optimize(\"\", sideways)\n",// not a declared state word
        "#pragma optimize(\"\")\n",          // no state word at all
        "#pragma optimize off\n",            // paren-less
    };
    for (char const* s : refused) {
        PreprocessResult r;
        (void)ppLexemes(std::string{s} + "int x;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "an unbuilt `#pragma optimize` form must be REFUSED: " << s;
    }
}

// ★ RED-ON-DISABLE for the `optimizerControl` sink AND for its config words.
TEST(Preprocessor, TfC85OptimizeRowAndItsStateWordsAreConfigDriven) {
    {   // no row -> the pragma is unregistered -> loud
        auto schema = reboundCSubset("\"prefix\": [\"optimize\"]",
                                     "\"prefix\": [\"optimizeXX\"]",
                                     "<no-optimize-row-c-subset>");
        ASSERT_NE(schema, nullptr);
        std::vector<std::filesystem::path> noDirs;
        auto buf = SourceBuffer::fromString(
            std::string{"#pragma optimize(\"\", off)\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma));
        EXPECT_TRUE(r.pragmaNoOptimizeByOffset.empty())
            << "and nothing is stamped — the sink is inert without its row";
    }
    {   // row present, but the OFF word undeclared -> the form is unbuilt -> loud
        auto schema = reboundCSubset("\"pragmaOptimizeOffWord\":    \"off\",",
                                     "",
                                     "<no-optimize-off-word-c-subset>");
        ASSERT_NE(schema, nullptr);
        ASSERT_TRUE(schema->preprocess().pragmaOptimizeOffWord.empty());
        std::vector<std::filesystem::path> noDirs;
        auto buf = SourceBuffer::fromString(
            std::string{"#pragma optimize(\"\", off)\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "with no `pragmaOptimizeOffWord` declared the off form is an "
               "unbuilt form and must be REFUSED, never silently ignored";
        EXPECT_TRUE(r.pragmaNoOptimizeByOffset.empty());
    }
}

// ★★★ THE PROFILE-CENSUS GUARD (tier a) — THE CLASS FIX.
//
// TF-C82's row set came from a census taken under ONE predefine class, written up
// as if it were universal. This preprocesses an in-repo fixture under ALL THREE
// classes and asserts NO pragma goes unclaimed in any of them.
//
// MEASURED: `availableObjectFormats` keys on format KIND, so the 24 shipped
// format files collapse to exactly three predefine classes — `pe`
// (_WIN32/_WIN64/_MSC_VER + more), `macho` (__APPLE__/__MACH__), and the
// NEITHER class (elf/spirv/wasm declare no identity predefines at all). Three
// passes, not 24. `nullopt` is a FOURTH state (only universal entries survive)
// and is included because the LSP / direct-API callers use it.
//
// ★ WHAT A GREEN RUN HERE DOES AND DOES NOT PROVE. It proves every pragma THIS
// FIXTURE reaches has a row under every class. It does NOT prove the row set is
// complete — a reached-set is a function of the defines in play and of how far
// each TU gets (MEASURED: sqlite's `ext/rtree/rtree.c` carries two more
// `#pragma intrinsic` lines that contribute nothing because its whole body sits
// in a not-taken `#if`). Completeness is what tier (b), the corpus census
// script, exists to keep reviewable.
TEST(Preprocessor, TfC85NoUnclaimedPragmaUnderAnyPredefineClass) {
    namespace fs = std::filesystem;
    fs::path const fixture =
        test_support::findCorpusRoot() / "c-subset" / "pragma_profile_census.c";
    ASSERT_TRUE(fs::exists(fixture))
        << "the profile-census fixture must exist: " << fixture.string();
    auto const text = test_support::readFile(fixture);
    ASSERT_FALSE(text.empty());

    for (std::optional<ObjectFormatKind> fmt :
         {std::optional<ObjectFormatKind>{ObjectFormatKind::Pe},
          std::optional<ObjectFormatKind>{ObjectFormatKind::MachO},
          std::optional<ObjectFormatKind>{ObjectFormatKind::Elf},
          std::optional<ObjectFormatKind>{}}) {
        PreprocessResult r;
        ppUnderFormat(text, fmt, r);
        std::string const legName =
            fmt.has_value() ? std::string{objectFormatKindName(*fmt)}
                            : std::string{"<no active format>"};
        // The ONE assertion that matters. `P_PreprocessorPragma` covers BOTH
        // "no row claimed it" and "a row claimed it `unsupported`", which is
        // exactly the pair a census is meant to surface.
        std::string offending;
        for (auto const& d : r.diagnostics->all()) {
            if (d.code != DiagnosticCode::P_PreprocessorPragma) continue;
            offending += "\n    " + d.actual;
        }
        EXPECT_TRUE(offending.empty())
            << "predefine class " << legName
            << " reaches a pragma no `preprocess.pragmaEffects` row claims."
               " Add a row (or fix the fixture) — do NOT relax this test:"
            << offending;
    }
}

// ★ NON-VACUITY FOR THE CENSUS GUARD. A census that preprocesses nothing passes
// trivially, and the pe-only pragmas live behind `#if defined(_MSC_VER)` — so the
// fixture MUST actually reach them on the pe leg and MUST NOT on the others.
// Without this, deleting the fixture's whole body would leave the guard green.
TEST(Preprocessor, TfC85ProfileCensusFixtureActuallyReachesTheProfileGatedRows) {
    namespace fs = std::filesystem;
    auto const text = test_support::readFile(
        test_support::findCorpusRoot() / "c-subset" / "pragma_profile_census.c");
    ASSERT_FALSE(text.empty());
    {   // pe: `_MSC_VER` is defined, so the MSVC arm is LIVE and its
        // `#pragma optimize("", off)` region actually stamps a token.
        PreprocessResult r;
        ppUnderFormat(text, ObjectFormatKind::Pe, r);
        EXPECT_FALSE(r.pragmaNoOptimizeByOffset.empty())
            << "on the pe leg the fixture's `#pragma optimize` region must be "
               "REACHED — otherwise this whole census is vacuous";
        EXPECT_TRUE(tokenIsNoOptimize(r, "msvc_no_optimize_marker"));
    }
    {   // macho: `_MSC_VER` undefined -> the MSVC arm is a DEAD branch -> C
        // 6.10p1 silence, and nothing is stamped.
        PreprocessResult r;
        ppUnderFormat(text, ObjectFormatKind::MachO, r);
        EXPECT_TRUE(r.pragmaNoOptimizeByOffset.empty())
            << "the MSVC arm must be entirely elided off the pe leg — the "
               "dead-branch gate is what keeps the Apple SDK compiling";
    }
}

// ★★ REACHABILITY, NOT RECOGNITION (C 6.10p1) — the `#error`/`#embed`/`#line`
// parity. A `#pragma` inside a DEAD branch is entirely silent because the arm
// sits past the `stackActive()` gate.
//
// ★ TF-C82 STRENGTHENED THIS TEST RATHER THAN LEAVING IT. It used to elide a
// pragma (`whatever here`) that was silent EVERYWHERE, so it could not tell a
// working reachability gate from a broken one. Now each dead-branch arm carries
// a pragma that is MEASURABLY LOUD when reached — an UNREGISTERED one, an
// `unsupported` one, and a MALFORMED `pack` — so the test fails the moment
// recognition is hoisted above the gate. That hoist is not hypothetical: the
// Apple SDK headers park hundreds of pragmas inside unsupported-configuration
// branches, and erroring on them would break every macOS compile.
TEST(Preprocessor, FC15cPragmaInDeadBranchIsSilent) {
    char const* const deadPragmas[] = {
        "#pragma whatever here",        // unregistered -> loud if reached
        "#pragma STDC FP_CONTRACT OFF", // `unsupported` row -> loud if reached
        "#pragma pack(3)",              // malformed operand -> loud if reached
        "#pragma pack(pop)",            // unbalanced pop    -> loud if reached
    };
    for (char const* const dead : deadPragmas) {
        PreprocessResult r;
        auto lexs = ppLexemes(std::string{"#if 0\n"} + dead
                                  + "\nint dead;\n#endif\nint x;\n",
                              r);
        EXPECT_TRUE(r.diagnostics->all().empty())
            << "a pragma in a NOT-TAKEN branch is never recognized at all: "
            << dead;
        ASSERT_EQ(lexs.size(), 3u) << "only `int x ;` survives: " << dead;
        EXPECT_EQ(lexs[0], "int");
        EXPECT_EQ(lexs[1], "x");
        EXPECT_EQ(lexs[2], ";");
    }
    // The CONTROL that makes the four arms above non-vacuous: the same pragmas
    // in a LIVE branch DO fire. Without this, a change that silenced pragmas
    // everywhere would leave the loop above green.
    for (char const* const live : deadPragmas) {
        PreprocessResult r;
        (void)ppLexemes(std::string{"#if 1\n"} + live + "\n#endif\nint x;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "reached, this pragma must be loud — else the dead-branch arms "
               "above assert nothing: "
            << live;
    }
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

// D-INCLUDE-ANGLE-SOURCE-FALLBACK (P1 — the PP surface): an angle `#include <h>`
// with NO `h.json` descriptor but a REAL `h` source header on the -I includeDirs
// falls back to TEXTUALLY splicing that header (byte-for-byte like a quote
// include), so its `#define`s AND declarations inline into THIS TU — the crux
// (a post-parse cross-ref would carry the symbol but NOT the macro). We ship a
// `foo.h` (a macro + a symbol) on an includeDir, NO systemDir descriptor, and
// angle-include it. RED-ON-DISABLE: revert the source-fallback arm -> the angle
// include is left verbatim -> `FOO_OK` never expands (no `7`), `foo_sym` never
// appears (the header text is absent), and the `#` of the directive survives.
TEST(Preprocessor, FC15cAngleSourceFallbackSplicesHeaderTextually) {
    namespace fs = std::filesystem;
    auto inc = fs::temp_directory_path() / "dss_angle_src_fallback_p1";
    fs::create_directories(inc);
    { std::ofstream(inc / "foo.h", std::ios::binary)
        << "#define FOO_OK 7\nint foo_sym;\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#include <foo.h>\nint u = FOO_OK;\n", r, {inc}, {});
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a resolvable angle source fallback must not error";
    auto has = [&](std::string_view s) {
        for (auto const& l : lexs) if (l == s) return true;
        return false;
    };
    EXPECT_TRUE(has("foo_sym"))
        << "the fallback header's declaration must be textually inlined";
    EXPECT_TRUE(has("7"))
        << "the fallback header's macro FOO_OK must EXPAND -- proving a textual "
           "splice, not a symbol-only cross-ref (which carries no macros)";
    EXPECT_FALSE(has("FOO_OK"))
        << "FOO_OK must be expanded, not survive as a bare identifier";
    EXPECT_FALSE(has("#"))
        << "the angle directive must be consumed (dropped), not left verbatim";
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// D-INCLUDE-ANGLE-SOURCE-FALLBACK (P5 — the FC15c parity pin): `__has_include(<h>)`
// MUST give the SAME answer `#include <h>` would. With `foo.h` on the -I
// includeDirs and NO descriptor, the source fallback makes `#include <foo.h>`
// resolve -> `__has_include(<foo.h>)` MUST be 1 (the `yes` branch). A header
// absent on BOTH paths is 0 (the `no` branch). RED-ON-DISABLE: revert the
// funnel's Source arm in the `__has_include` callback -> `<foo.h>` answers 0 ->
// the wrong branch, AND it would then DISAGREE with the now-resolving `#include`
// -- exactly the FC15c silent-miscompile this parity forbids.
TEST(Preprocessor, FC15cAngleSourceFallbackHasIncludeParity) {
    namespace fs = std::filesystem;
    auto inc = fs::temp_directory_path() / "dss_angle_src_fallback_p5";
    fs::create_directories(inc);
    { std::ofstream(inc / "foo.h", std::ios::binary) << "/* x */\n"; }
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#if __has_include(<foo.h>)\nint yes;\n#else\nint no;\n#endif\n",
            r, {inc}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "yes")
            << "__has_include(<foo.h>) must be 1 via the source fallback "
               "(parity with the now-resolving #include <foo.h>)";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#if __has_include(<none_xyz.h>)\nint yes;\n#else\nint no;\n#endif\n",
            r, {inc}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "no")
            << "__has_include of a header absent on BOTH paths must be 0";
    }
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: `preprocess()` surfaces the
// weakly-canonical paths of every RESOLVED system descriptor an angle
// `#include <h>` splices, EXPANDED to the transitive `includes` closure and
// deduped. This is the exact set `parseAndAdd_` harvests typedef NAMES from to
// seed the first parse (so `(size_t)(x)` commits as a cast without a full-file
// oracle reparse). EMIT-ONLY: it changes no token output. RED-ON-DISABLE: drop
// the 581/807/1945 accumulation (or the closure expansion) → the vector is empty
// / missing the child → the seed never covers the descriptor's typedefs.
TEST(Preprocessor, DPerf2ResolvedShippedDescriptorsIncludeTransitiveClosure) {
    namespace fs = std::filesystem;
    auto sysdir = fs::temp_directory_path() / "dss_dperf2_resolved_desc";
    fs::create_directories(sysdir);
    { std::ofstream(sysdir / "parent.json", std::ios::binary)
        << R"({ "header": "parent.h", "includes": ["child.h"],
                "typedefs": [ { "name": "ParentT", "type": "i32" } ] })"; }
    { std::ofstream(sysdir / "child.json", std::ios::binary)
        << R"({ "header": "child.h",
                "typedefs": [ { "name": "ChildT", "type": "i32" } ] })"; }

    PreprocessResult r;
    (void)ppLexemesWithDirs("#include <parent.h>\nint x;\n", r, {}, {sysdir});
    EXPECT_FALSE(r.diagnostics->hasErrors());

    std::error_code ec;
    auto const wantParent = fs::weakly_canonical(sysdir / "parent.json", ec);
    auto const wantChild  = fs::weakly_canonical(sysdir / "child.json", ec);
    bool sawParent = false;
    bool sawChild  = false;
    for (auto const& p : r.resolvedShippedDescriptors) {
        auto const c = fs::weakly_canonical(p, ec);
        if (c == wantParent) sawParent = true;
        if (c == wantChild) sawChild = true;
    }
    EXPECT_EQ(r.resolvedShippedDescriptors.size(), 2u)
        << "the angle include must surface parent.json + the transitive "
           "child.json, deduped by weakly-canonical path";
    EXPECT_TRUE(sawParent) << "the parent descriptor must be surfaced";
    EXPECT_TRUE(sawChild)
        << "the transitively-included child descriptor must be surfaced";

    fs::remove_all(sysdir, ec);
}

// D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION (dead-range filter — the effectiveness
// proof): the recorded seed set is the AUTHORITATIVELY-LIVE one, EQUAL to the
// finish() oracle's `shippedLibDescriptors`, never a superset. A LIVE angle
// `#include <h>` IS recorded (its un-gated splice offset lands in a live region);
// the SAME include inside a `#if 0 … #endif` dead branch is DROPPED (its splice
// offset lands in an AUTHORITATIVE dead range), so the first-parse seed can never
// resolve a name the finish() reparse would not. RED-ON-DISABLE: revert the
// `byteInDeadRegion(off)` skip in `preprocess()`'s closure block -> the dead
// branch's un-gated splice survives -> the descriptor is surfaced -> the two
// dead-branch assertions below fail.
TEST(Preprocessor, DPerf2SeedSetMatchesLiveOracleNotDeadBranch) {
    namespace fs = std::filesystem;
    auto sysdir = fs::temp_directory_path() / "dss_dperf2_deadbranch";
    fs::create_directories(sysdir);
    // A typedef-only descriptor for `<stddef.h>` (the `size_t` shape). Its typedef
    // surface is injected SEMANTICALLY, so an angle include does not splice text —
    // the ONLY trace is the recorded seed entry this test pins.
    { std::ofstream(sysdir / "stddef.json", std::ios::binary)
        << R"({ "header": "stddef.h",
                "typedefs": [ { "name": "size_t", "type": "u64" } ] })"; }
    std::error_code ec;
    auto const wantDesc = fs::weakly_canonical(sysdir / "stddef.json", ec);

    auto containsStddef = [&](PreprocessResult const& r) {
        for (auto const& p : r.resolvedShippedDescriptors) {
            if (fs::weakly_canonical(p, ec) == wantDesc) return true;
        }
        return false;
    };

    // (1) LIVE include -> the descriptor IS recorded (the seed covers `size_t`, so
    // the includer's `(size_t)(0)` cast commits on parse 1 without a reparse).
    PreprocessResult live;
    (void)ppLexemesWithDirs(
        "#include <stddef.h>\nint main(){ return (size_t)(0); }\n",
        live, {}, {sysdir});
    EXPECT_FALSE(live.diagnostics->hasErrors());
    EXPECT_TRUE(containsStddef(live))
        << "a LIVE angle include must record its descriptor for the seed";

    // (2) DEAD include (`#if 0 … #endif`) -> NOT recorded: the un-gated splice
    // still fires, but its offset is in an authoritative dead range, so the filter
    // drops it. The finish() oracle would not resolve it either -> seed == oracle.
    PreprocessResult dead;
    (void)ppLexemesWithDirs(
        "#if 0\n#include <stddef.h>\n#endif\nint main(){ return 0; }\n",
        dead, {}, {sysdir});
    EXPECT_FALSE(dead.diagnostics->hasErrors());
    EXPECT_FALSE(containsStddef(dead))
        << "a dead `#if 0` include must NOT seed -- the finish() oracle would "
           "not resolve it either";
    EXPECT_TRUE(dead.resolvedShippedDescriptors.empty())
        << "the dead-branch TU resolves no LIVE system descriptor";

    // (3) MACRO-GATED dead branch (`#define GATE 0` then `#if GATE` -> the
    // condition is dead only AFTER macro expansion): the authoritative
    // MacroExpander records the dead byte range branch-agnostically, so the
    // un-gated splice's offset is filtered EXACTLY as the `#if 0` case --
    // locking the seed==oracle guarantee for EVERY dead-branch form, not only
    // the literal constant one.
    PreprocessResult macroDead;
    (void)ppLexemesWithDirs(
        "#define DSS_GATE 0\n#if DSS_GATE\n#include <stddef.h>\n#endif\n"
        "int main(){ return 0; }\n",
        macroDead, {}, {sysdir});
    EXPECT_FALSE(macroDead.diagnostics->hasErrors());
    EXPECT_FALSE(containsStddef(macroDead))
        << "a macro-gated dead include must NOT seed (the dead range is "
           "branch-agnostic)";

    // (4) LIVE include immediately AFTER a dead `#endif`: the filter is half-open
    // `[deadStart, deadEnd)`, so a live include at/after the reactivating
    // directive is KEPT -- never over-filtered into the adjacent live region.
    PreprocessResult liveAfterDead;
    (void)ppLexemesWithDirs(
        "#if 0\n#endif\n#include <stddef.h>\n"
        "int main(){ return (size_t)(0); }\n",
        liveAfterDead, {}, {sysdir});
    EXPECT_FALSE(liveAfterDead.diagnostics->hasErrors());
    EXPECT_TRUE(containsStddef(liveAfterDead))
        << "a live include after a dead #endif must still seed";

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

// ── TF-C60 (D-PP-PRESCAN-CROSS-BUFFER-MACRO-STATE) ───────────────────────────
// The pre-scan's macro state must span the WHOLE include tree in DOCUMENT ORDER.
// Before the fix, `localMacros` was PER-BUILDER: a `#define` arriving via a
// NESTED include was invisible to the parent's later `#if` (and a parent's
// source `#define` invisible to a child's), so the guard folded 0 → FALSE-DEAD →
// the gated quote-include was left verbatim → the macro pass forwarded it as
// INERT tokens → the header was SILENTLY DROPPED (green-with-missing-code).
// This is the sqlite os_unix.c shape: sqliteInt.h→os_setup.h defines
// SQLITE_OS_UNIX=1; `#if SQLITE_OS_UNIX` gates `#include "os_common.h"`.

// CHILD→PARENT: a gate macro defined in an INCLUDED header must make the
// parent's later `#if GATE`-gated include LIVE. The included inner.h defines a
// macro the parent USES — if inner.h is dropped, EMPTY stays undefined and the
// use site parse-errors (the exact os_unix.c failure).
// RED-ON-DISABLE: fresh per-child localMacros (the pre-fix state) drops inner.h.
TEST(Preprocessor, Tf60GateDefinedInChildHeaderMakesParentIncludeLive) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_tf60_c2p";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "gate.h", std::ios::binary) << "#define GATE 1\n"; }
    { std::ofstream(dir / "inner.h", std::ios::binary)
          << "#define EMPTY(A)\n"; }
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#include \"gate.h\"\n#if GATE\n#include \"inner.h\"\n#endif\n"
        "int f(void) { EMPTY( return 7; ); return 42; }\n",
        "main.c");
    std::vector<fs::path> includeDirs{dir};
    auto out = preprocess(buf, schema, includeDirs, {}, std::nullopt, {});
    EXPECT_FALSE(out.diagnostics->hasErrors())
        << "a gate macro defined in an INCLUDED header must make the parent's "
           "#if-gated include LIVE — dropping inner.h leaves EMPTY undefined "
           "and the use site errors (the sqlite os_unix.c silent-drop)";
    bool sawEmptyWord = false;
    for (Token const& t : out.tokens) {
        if (std::string{out.synthBuffer->slice(t.span)} == "EMPTY")
            sawEmptyWord = true;
    }
    EXPECT_FALSE(sawEmptyWord)
        << "EMPTY( ... ) must have been macro-EXPANDED away — its survival "
           "means inner.h was silently dropped";
    fs::remove_all(dir, ec);
}

// PARENT→CHILD: a `#define` in the parent's OWN text before the include must be
// visible to the CHILD's pre-scan (only command-line defines threaded before).
// RED-ON-DISABLE: the child's fresh localMacros drops inner.h the same way.
TEST(Preprocessor, Tf60GateDefinedInParentSourceMakesChildIncludeLive) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_tf60_p2c";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "outer.h", std::ios::binary)
          << "#if GATE\n#include \"inner.h\"\n#endif\n"; }
    { std::ofstream(dir / "inner.h", std::ios::binary)
          << "#define EMPTY(A)\n"; }
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#define GATE 1\n#include \"outer.h\"\n"
        "int f(void) { EMPTY( return 7; ); return 42; }\n",
        "main.c");
    std::vector<fs::path> includeDirs{dir};
    auto out = preprocess(buf, schema, includeDirs, {}, std::nullopt, {});
    EXPECT_FALSE(out.diagnostics->hasErrors())
        << "a parent-source #define before the include must be visible to the "
           "CHILD's pre-scan so its #if GATE-gated include is LIVE";
    bool sawEmptyWord = false;
    for (Token const& t : out.tokens) {
        if (std::string{out.synthBuffer->slice(t.span)} == "EMPTY")
            sawEmptyWord = true;
    }
    EXPECT_FALSE(sawEmptyWord)
        << "EMPTY( ... ) must have been macro-EXPANDED away — its survival "
           "means the child dropped inner.h";
    fs::remove_all(dir, ec);
}

// TF-C60 code-audit BLOCKER 1: a macro whose replacement is a CHARACTER LITERAL
// must survive into the pre-scan's guard evaluation. The tokenizer USED TO emit
// a coalesced literal as OPENER + BODY and consume the closing delimiter with NO
// token, so building the stored replacement by joining token TEXTS lost the
// closer (`'A'` → `' A`), which re-lexed to an unterminated literal → the guard
// became unevaluable → the (d) arm turned it into a HARD ERROR on valid C.
// (D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN has since given the closer its own
// `CharEnd` token. This test stayed GREEN through that change and is kept: it
// pins the OBSERVABLE contract — a char-literal macro replacement survives
// guard evaluation — which must hold regardless of how the replacement is
// captured.)
// `#define NEWLINE '\n'` guarding a conditional include is ordinary C.
//
// RED-ON-DISABLE, RE-AIMED AND MEASURED: replace `sbTrackDefine`'s source
// SLICE with a WHITESPACE-SEPARATED join of `toks[q].text` and this errors
// while every `#define GATE 1`-shaped test stays green — the coverage hole the
// audit found. Verified: the stored replacement becomes `' \n '` (the
// separators land INSIDE the literal), a multi-char char constant, so the guard
// is unevaluable and the (d) arm hard-errors on valid C — `hasErrors()` true
// and `inner.h` never splices.
//
// ★ The BARE join (`joined += toks[q].text`, the pre-TF-C60 shape the earlier
// version of this note named) no longer reddens it — MEASURED GREEN. Now that
// the closer carries its own `CharEnd` token, a bare join happens to
// reassemble `'\n'` byte-for-byte. That is exactly why the instruction is
// aimed at the SEPARATOR instead: it targets the property the slice actually
// buys — every replacement byte in its ORIGINAL spacing — which no token-text
// join can promise, rather than a token-count accident that the closer-token
// work already changed once.
TEST(Preprocessor, Tf60CharLiteralMacroReplacementSurvivesGuardEval) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_tf60_charlit";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "inner.h", std::ios::binary) << "int lit_ok_zzz;\n"; }
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#define NL '\\n'\n#if NL == 10\n#include \"inner.h\"\n#endif\nint y;\n",
        "main.c");
    std::vector<fs::path> includeDirs{dir};
    auto out = preprocess(buf, schema, includeDirs, {}, std::nullopt, {});
    EXPECT_FALSE(out.diagnostics->hasErrors())
        << "a char-literal macro replacement must round-trip into the guard "
           "evaluation — losing the literal's closing delimiter makes the guard "
           "unevaluable and hard-errors valid C";
    bool sawMarker = false;
    for (Token const& t : out.tokens) {
        if (std::string{out.synthBuffer->slice(t.span)} == "lit_ok_zzz")
            sawMarker = true;
    }
    EXPECT_TRUE(sawMarker) << "`#if NL == 10` is TRUE, so inner.h must splice";
    fs::remove_all(dir, ec);
}

// TF-C60 code-audit BLOCKER 2: an object-like macro that expands TO a
// function-like macro's NAME leaves the guard undecidable for this weaker
// pre-scan. Freezing the expansion (folding the identifier to 0) is conservative
// ONLY at even polarity — under `!` it inverts to TRUE and eagerly resolves an
// authoritatively-DEAD include, re-opening P0016. It must take the UNCERTAIN
// (skip) path in both polarities.
// RED-ON-DISABLE: make the post-expansion arm `return in;` without setting
// `sbPostExpandUncertain` and this resolves the dead include → P0016 fires.
TEST(Preprocessor, Tf60ObjectMacroExpandingToFunclikeIsUncertainUnderNegation) {
    PreprocessResult r;
    (void)ppLexemes(
        "#define ENABLED(x) x\n#define Z ENABLED(1)\n"
        "#if !Z\n#include \"nope_missing_z.h\"\n#endif\nint g;\n",
        r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "`#if !Z` is authoritatively DEAD; the pre-scan must NOT eagerly "
           "resolve its include (P0016: never resolve a dead include)";
}

// TF-C60 code-audit SHOULD-FIX 7(b): the mint/slice arithmetic pinned on a
// DISTINCTIVE value through a two-step chain, plus a negative control — a wrong
// product-region slice that happens to yield some other nonzero number would
// pass a mere "is live" assertion.
TEST(Preprocessor, Tf60MintedProductSlicesExactValueThroughChain) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_tf60_mint";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "inner.h", std::ios::binary) << "int mint_ok_zzz;\n"; }
    auto schema = cSubset();
    std::vector<fs::path> includeDirs{dir};
    {   // positive: the chain must fold to EXACTLY 424242
        auto buf = SourceBuffer::fromString(
            "#define A B\n#define B 424242\n#if A == 424242\n"
            "#include \"inner.h\"\n#endif\nint y;\n", "main.c");
        auto out = preprocess(buf, schema, includeDirs, {}, std::nullopt, {});
        EXPECT_FALSE(out.diagnostics->hasErrors());
        bool saw = false;
        for (Token const& t : out.tokens)
            if (std::string{out.synthBuffer->slice(t.span)} == "mint_ok_zzz")
                saw = true;
        EXPECT_TRUE(saw) << "the minted chain must slice to exactly 424242";
    }
    {   // negative control: a MISSING header under a FALSE guard must stay unresolved
        PreprocessResult r;
        (void)ppLexemes("#define A B\n#define B 424242\n#if A != 424242\n"
                        "#include \"nope_missing_mint.h\"\n#endif\nint y;\n", r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a FALSE guard must not resolve its include — a mis-sliced "
               "product that folded differently would wrongly make this live";
    }
    fs::remove_all(dir, ec);
}

// TF-C60 (design-audit Finding 6): the ONE residual direction where the pre-scan
// can read MORE-live than the authoritative pass — a function-like-guarded
// branch containing an `#undef` the pre-scan never tracks (the FIX-3 skip means
// the branch is not stack-active for tracking). The later `#if X` then reads X
// still defined → the pre-scan eagerly SPLICES "a.h" — but the authoritative
// pass (which DID run the #undef) reads that region DEAD and ELIDES the spliced
// content. The edge is LOUD-OR-BENIGN by construction: an existing header is
// spliced-then-elided (benign, pinned here); a missing one errors loudly.
// RED-ON-DISABLE: if the authoritative dead-gate ever stopped eliding the
// pre-scan's over-eager splice, the marker below would LEAK into the output.
TEST(Preprocessor, Tf60FunclikeGuardedUndefMoreLiveEdgeStaysBenign) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_tf60_morelive";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "a.h", std::ios::binary)
          << "int morelive_marker_zzz;\n"; }
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#define X 1\n#define FUNC(a) a\n"
        "#if FUNC(1)\n#undef X\n#endif\n"
        "#if X\n#include \"a.h\"\n#endif\n"
        "int y;\n",
        "main.c");
    std::vector<fs::path> includeDirs{dir};
    auto out = preprocess(buf, schema, includeDirs, {}, std::nullopt, {});
    bool sawMarker = false;
    for (Token const& t : out.tokens) {
        if (std::string{out.synthBuffer->slice(t.span)} == "morelive_marker_zzz")
            sawMarker = true;
    }
    EXPECT_FALSE(sawMarker)
        << "the authoritative pass #undef'd X, so the pre-scan's eager splice of "
           "a.h must be ELIDED by the authoritative dead-gate — content leaking "
           "past it would be a silent wrong-include";
    fs::remove_all(dir, ec);
}

// (FIX-3, re-pinned by TF-C60) a guard that INVOKES a FUNCTION-LIKE macro is
// still not evaluated by the weaker pre-scan (the conservative skip stands),
// but the OUTCOME is no longer silence: this guard is authoritatively LIVE
// (`ENABLED(1)` -> 1), so the unresolved quote-`#include` reaches the macro
// pass, and the TF-C60 fail-loud arm ERRORS rather than forwarding the line as
// inert tokens. The pre-fix behaviour — no diagnostic at all — was the
// silent-drop bug class (D-PP-PRESCAN-CROSS-BUFFER-MACRO-STATE): the old
// comment claimed a wrongly-skipped live include "fails loud downstream", and
// nothing downstream ever failed.
// RED-ON-DISABLE: removing the macro-pass unresolved-live-quote-include arm
// returns this to the silent drop (no error at all).
TEST(Preprocessor, FunctionLikeMacroGuardUnresolvedLiveIncludeFailsLoud) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define ENABLED(x) x\n#if ENABLED(1)\n#include \"nope_fn.h\"\n#endif\n"
        "int x;\n",
        r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "a LIVE quote-#include the pre-scan could not resolve (function-like "
           "guard) must FAIL LOUD in the macro pass — never a silent drop";
    (void)lexs;
}

// ── C19 (D-PP-PRESCAN-DEFINEDNESS-PARITY): the include-gating pre-scan must know
// COMMAND-LINE `--define`s. A `#ifdef <cmdline-define>`-gated quote-`#include` was
// FALSELY read dead -- the pre-scan saw only in-source `#define`s (localMacros) +
// predefined, never the `<command-line>` prologue (spliced straight into synthText)
// -- so the header was left un-inlined and its `#define`s dropped, surfacing
// downstream as a spurious P0009 (the real SQLITE_TEST-gated `tclsqlite.h` ->
// `SQLITE_TCLAPI` drop across 7 `src/test*.c` TUs). These pins reuse the
// "missing-header-must-error-when-LIVE" oracle of DefineMakesIfBranchLiveSoInclude-
// Errors, so the gate state is directly observable. ──────────────────────────────

// (Self-contained: `ppLexemesWithDefines` lives later in this file, so these call
// `preprocess()` directly -- they only need the include-error diagnostic / a short
// lexeme check.)

// Pin 1 (CORE, RED-ON-DISABLE): a command-line `--define GATE` makes `#ifdef GATE`
// LIVE in the include-gating pre-scan, so its quote-`#include` RESOLVES (and errors
// on the missing header). Disable the seed -> GATE unknown -> branch read dead ->
// include silently skipped -> NO error (the exact silent drop this cycle fixes).
TEST(Preprocessor, CommandLineDefineMakesIfdefIncludeLive) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#ifdef GATE\n#include \"still_missing.h\"\n#endif\nint x;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> defines{"GATE"};
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
    EXPECT_TRUE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "a command-line --define GATE must make #ifdef GATE live in the include-"
           "gating pre-scan so its quote-#include resolves "
           "(D-PP-PRESCAN-DEFINEDNESS-PARITY)";
}

// Pin 2 (SYMMETRY, RED-ON-DISABLE): the `#if defined(GATE)` form agrees with
// `#ifdef GATE` for a command-line define -- both route through the unified
// `sbNameDefined`. Before, `#ifdef` saw ONLY localMacros while `#if defined()` also
// saw predefined, and NEITHER saw a command-line define; now they agree.
TEST(Preprocessor, CommandLineDefineViaDefinedOperatorIncludeLive) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#if defined(GATE)\n#include \"still_missing.h\"\n#endif\nint x;\n",
        "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> defines{"GATE"};
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
    EXPECT_TRUE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "#if defined(GATE) must agree with #ifdef GATE for a command-line define "
           "(the unified sbNameDefined oracle)";
}

// Pin 3 (CHILD THREADING, depth>=1, RED-ON-DISABLE): a header included LIVE from an
// outer command-line-gated conditional must ITSELF see the command-line define for
// its OWN gated includes -- proving the C21 `preScanDefinePrefix` (which supersedes
// the C19 `seededDefines` NAME set) threads into child builders. `outer.h`
// (resolved + inlined from main) has its own `#ifdef GATE #include <missing_inner>`;
// with child-threading that inner include resolves + errors, without it the child
// never learns GATE -> inner skipped -> no error.
TEST(Preprocessor, CommandLineDefineSeedThreadsIntoChildBuilders) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_c19_child_seed";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "outer.h", std::ios::binary)
          << "#ifdef GATE\n#include \"still_missing_inner.h\"\n#endif\n"; }
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#ifdef GATE\n#include \"outer.h\"\n#endif\nint x;\n", "main.c");
    std::vector<fs::path> includeDirs{dir};
    std::vector<std::string> defines{"GATE"};
    auto out = preprocess(buf, schema, includeDirs, {}, std::nullopt, defines);
    EXPECT_TRUE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "the command-line define seed must thread into the child builder so "
           "outer.h's own #ifdef GATE-gated include is LIVE (D-PP-PRESCAN-"
           "DEFINEDNESS-PARITY child-threading)";
    fs::remove_all(dir, ec);
}

// TF-C59 (D-CPP-LINE-DIRECTIVE) — the design's HEADLINE property, which nothing
// pinned until the independent code-audit said so: `#line` records are keyed PER
// ORIGIN BUFFER, so a `#line` inside an #include'd header renumbers only THAT
// header; the includer's own numbering is untouched.
// RED-ON-DISABLE: replace the per-origin `lineDirs_` map with a single global
// vector and the includer's `__LINE__` after the #include wrongly follows the
// header's directive (every other Tf59 test stays green — they are all
// single-buffer, which is exactly the coverage hole the audit found).
TEST(Preprocessor, Tf59LineDirectiveInHeaderDoesNotRenumberIncluder) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_tf59_line_hdr";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "h.h", std::ios::binary)
          << "#line 700\nint from_header = __LINE__;\n"; }
    auto schema = cSubset();
    //                                        line: 1              2
    auto buf = SourceBuffer::fromString(
        "#include \"h.h\"\nint after = __LINE__;\n", "main.c");
    std::vector<fs::path> includeDirs{dir};
    auto out = preprocess(buf, schema, includeDirs, {}, std::nullopt, {});
    EXPECT_FALSE(out.diagnostics->hasErrors());
    bool saw700 = false, saw2 = false;
    for (Token const& t : out.tokens) {
        std::string const s{out.synthBuffer->slice(t.span)};
        if (s == "700") saw700 = true;
        if (s == "2")   saw2   = true;
    }
    EXPECT_TRUE(saw700) << "the header's own line must follow its #line 700";
    EXPECT_TRUE(saw2)
        << "the INCLUDER's line 2 must be UNAFFECTED by the header's #line — "
           "exactly what per-origin keying buys";
    fs::remove_all(dir, ec);
}

// Pin 4 (NO OUTPUT CONTAMINATION): the definedness seed is pre-scan knowledge ONLY
// -- read for branch decisions, never written to the output. `--define GATE=7`
// still expands GATE to its prologue value `7` in a live branch (not shadowed to
// empty by a stray seed `#define`), and no extra tokens leak.
TEST(Preprocessor, CommandLineDefineSeedDoesNotContaminateOutput) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString("#ifdef GATE\nint v = GATE;\n#endif\n",
                                        "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> defines{"GATE=7"};
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
    EXPECT_FALSE(out.diagnostics->hasErrors());
    std::vector<std::string> lexs;
    for (Token const& t : out.tokens) {
        if (t.coreKind == CoreTokenKind::Eof
            || t.coreKind == CoreTokenKind::Whitespace
            || t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{out.synthBuffer->slice(t.span)});
    }
    ASSERT_EQ(lexs.size(), 5u) << "expected exactly: int v = 7 ;";
    EXPECT_EQ(lexs[3], "7") << "GATE expands to its prologue value, seed adds no "
                               "shadowing #define";
}

// ── C21 (D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE): the include-gating pre-scan
// now seeds command-line/predefined macro VALUES (not just definedness) via a
// NON-EMITTED span-safe `#define NAME VALUE` prefix on each build()'s scan buffer,
// so a `#if <macro>` VALUE guard gating a quote-`#include` evaluates correctly.
// These pins use the same "missing-header-errors-when-LIVE" oracle as the C19 pins
// above and are RED-ON-DISABLE (revert the value prefix -> the value guard folds to
// 0 -> the include is silently skipped -> NO error). ────────────────────────────

// C21 Pin A (VALUE CORE, RED-ON-DISABLE): a command-line `--define M=1` makes the
// VALUE guard `#if M` LIVE (not merely `#ifdef M`), so its quote-`#include`
// RESOLVES (and errors on the missing header). This is the value capability C19's
// definedness-only seed lacked: without the value prefix `#if M` folds M->0->dead
// -> the include is silently skipped -> NO error (the exact drop this cycle fixes).
TEST(Preprocessor, CommandLineDefineValueMakesIfIncludeLive) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#if M\n#include \"still_missing.h\"\n#endif\nint x;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> defines{"M=1"};
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
    EXPECT_TRUE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "a command-line --define M=1 must make the VALUE guard #if M live in the "
           "include-gating pre-scan so its quote-#include resolves "
           "(D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE)";
}

// C21 Pin B (VALUE-vs-DEFINEDNESS PARITY): with `--define M=1`, BOTH `#if defined(M)`
// (definedness, via sbNameDefined) AND `#if M` (value, via the prefix seeding
// localMacros) gate the include LIVE -> both error. The two contexts agree.
TEST(Preprocessor, CommandLineDefineValueAndDefinednessAgree) {
    auto schema = cSubset();
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> defines{"M=1"};
    {
        auto buf = SourceBuffer::fromString(
            "#if defined(M)\n#include \"still_missing.h\"\n#endif\nint x;\n",
            "main.c");
        auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
        EXPECT_TRUE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
            << "#if defined(M) must gate the include LIVE for --define M=1";
    }
    {
        auto buf = SourceBuffer::fromString(
            "#if M\n#include \"still_missing.h\"\n#endif\nint x;\n", "main.c");
        auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
        EXPECT_TRUE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
            << "#if M (value) must agree with #if defined(M) for --define M=1";
    }
}

// C21 Pin C (NEGATIVE / P0016 one-directional divergence): `--define M=0` makes the
// VALUE guard `#if M` DEAD, so the quote-`#include` is NOT resolved -> NO error.
// Proves the value seed does not OVER-resolve -- a 0-valued define stays dead, so
// the pre-scan is more-live only IN LOCKSTEP with the authoritative pass.
TEST(Preprocessor, CommandLineDefineValueZeroKeepsIncludeDead) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#if M\n#include \"still_missing.h\"\n#endif\nint x;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> defines{"M=0"};
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
    EXPECT_FALSE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "--define M=0 must leave #if M dead so its quote-#include is not resolved "
           "(no P0016 over-resolution)";
}

// C21 Pin D (FINDING-C, NOT-EMIT, RED-ON-DISABLE): the value prefix is pre-scan
// knowledge ONLY -- it must NEVER be emitted into the synth buffer. Token-count is
// BLIND to a leak (a duplicate `#define GATE 7` is an idempotent redefinition -> 0
// extra output tokens), so this inspects the synth buffer TEXT and asserts the
// `#define GATE` string occurs EXACTLY ONCE (the authoritative <command-line>
// prologue). A leaked prefix -> TWO occurrences.
TEST(Preprocessor, CommandLineDefineValuePrefixNotEmittedIntoSynthText) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString("#ifdef GATE\nint v = GATE;\n#endif\n",
                                        "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> defines{"GATE=7"};
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
    std::string_view syn = out.synthBuffer->text();
    std::size_t count = 0;
    for (std::size_t pos = syn.find("#define GATE");
         pos != std::string_view::npos; pos = syn.find("#define GATE", pos + 1)) {
        ++count;
    }
    EXPECT_EQ(count, 1u)
        << "the non-emitted value prefix must NOT leak into the synth buffer -- "
           "`#define GATE` must appear exactly once (the <command-line> prologue)";
}

// C21 Pin E (predefined-VALUE, RED-ON-DISABLE -- CLOSES the sibling anchor
// D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE): a predefined macro used in VALUE
// position (`#if __STDC_VERSION__ >= 201112L`) now gates a quote-`#include` LIVE ->
// the missing header errors. Before C21 the pre-scan folded __STDC_VERSION__ to 0
// -> 0 >= 201112L is false -> dead -> the include was conservatively skipped (the
// exact residual this anchor tracked). The value comes from the OBJECT-like
// predefined subset of the prefix (c-subset __STDC_VERSION__ = 202311L).
TEST(Preprocessor, PredefinedValueGuardMakesIncludeLive) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#if __STDC_VERSION__ >= 201112L\n#include \"still_missing.h\"\n#endif\n"
        "int x;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> noDefs;
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, noDefs);
    EXPECT_TRUE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "a predefined VALUE guard (#if __STDC_VERSION__ >= 201112L) must gate the "
           "quote-#include LIVE (closes D-PP-PRESCAN-PREDEFINED-VALUE-INCLUDE-GATE)";
}

// C21 Pin E' (predefined-VALUE converse): the SAME predefined used in a FALSE guard
// (`#if __STDC_VERSION__ < 0`) stays DEAD -> NO error. Proves the seeded value is
// REAL (not a blanket "predefined -> live").
TEST(Preprocessor, PredefinedValueGuardFalseKeepsIncludeDead) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#if __STDC_VERSION__ < 0\n#include \"still_missing.h\"\n#endif\nint x;\n",
        "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> noDefs;
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, noDefs);
    EXPECT_FALSE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "#if __STDC_VERSION__ < 0 must stay dead (the seeded predefined value is "
           "real, not a blanket predefined->live)";
}

// C21 Pin F (FINDING-A, function-like predefine must NOT be value-seeded): a bare
// `#if NAME` (no call) on a FUNCTION-like predefine must fold to 0 in the pre-scan
// EXACTLY as in the authoritative pass; value-seeding it would make the pre-scan
// MORE-live -> a silent P0016 re-open. The prefix builder therefore SKIPS
// `isFunctionLike` predefines (mirroring the MacroExpander ctor + the <built-in>
// prologue). NOTE: the c-subset schema's function-like predefines are `_declspec` (B1) +
// `__declspec` (pe-only, value ""), a WEAK red-on-disable witness -- wrongly
// value-seeding it yields an object-like EMPTY macro, so `#if __declspec` -> empty
// operand -> uncertain -> conservative skip -> NO error, the SAME outcome as the
// correct guard (`#if __declspec` -> undefined identifier -> 0 -> skip). So this pin
// POSITIVELY confirms the guarded outcome (the function-like predefine is available
// on pe yet its VALUE guard stays dead -> no include resolved); the CODE guard is
// what enforces FINDING-A. Run on the pe format so __declspec passes the
// availability filter and actually reaches the isFunctionLike guard.
TEST(Preprocessor, FunctionLikePredefinedNotValueSeededIntoPrescan) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#if __declspec\n#include \"still_missing.h\"\n#endif\nint x;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> noDefs;
    auto out = preprocess(buf, schema, noDirs, {}, ObjectFormatKind::Pe, noDefs);
    EXPECT_FALSE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "a function-like predefine (__declspec) must NOT be value-seeded into the "
           "pre-scan: #if __declspec stays dead so its quote-#include is not resolved";
}

// C21 Pin G (#undef COMPOSE, Option 2): `--define M=1` followed by an in-source
// `#undef M` BEFORE the `#if M`-gated include leaves the include DEAD -> NO error.
// The Option-2 improvement over C19's separate NAME set (which ignored #undef): the
// value seeds `localMacros`, so a source `#undef` erases it and the guard composes.
TEST(Preprocessor, CommandLineDefineThenUndefComposesDead) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#undef M\n#if M\n#include \"still_missing.h\"\n#endif\nint x;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> defines{"M=1"};
    auto out = preprocess(buf, schema, noDirs, {}, std::nullopt, defines);
    EXPECT_FALSE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "an in-source #undef M must compose with --define M=1 (Option 2 seeds the "
           "value into localMacros, which #undef erases) -> #if M dead -> no include";
}

// ── D-PP-PRESCAN-ANGLE-MACRO-SPLICE-AUTHORITATIVE-LIVENESS: the ANGLE shipped-
// macro splice is NO LONGER gated on the pre-scan's (weaker, sometimes-blind)
// conditional verdict -- the injected `#define`s are emitted inside the include's
// conditional region and the AUTHORITATIVE MacroExpander (which elides dead-branch
// defines) arbitrates their liveness. Two pins: the POSITIVE case the fix unblocks
// (a shipped macro under a `#if` gated by a QUOTE-include define the pre-scan cannot
// see) and the NEGATIVE final-output invariant (a `#if 0`-gated shipped include must
// not leak a usable macro -- the authoritative pass elides it). ──────────────────

// POSITIVE (RED-ON-DISABLE): a shipped OBJECT-macro from an ANGLE `#include` must
// inject+expand even when that include is gated by `#if <flag>` whose flag is
// `#define`d in a QUOTE-included header -- a flag the include-gating pre-scan is BLIND
// to (a child SynthBuilder's localMacros is discarded), so the pre-scan CONFIDENTLY
// folds the guard to 0 (an undefined identifier -> 0, C 6.10.1p4) and mis-marks the
// branch dead, while the authoritative pass (seeing platform.h's spliced text) reads
// it LIVE. This is the reduced sqlite test_syscall.c shape (`#if SQLITE_OS_UNIX` ->
// `#include <errno.h>`, SQLITE_OS_UNIX from the quote-included os_setup.h). Uses a
// FLAT object-macro (no per-format variant) so it injects under the harness's nullopt
// activeFormat. RED-ON-DISABLE: restore the `includeResolvable()` gate on the angle
// arm -> the pre-scan skips the splice -> SHIPPED_MAC survives unexpanded.
TEST(Preprocessor, AngleShippedMacroSplicesUnderQuoteIncludeGatedIf) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto sysdir = fs::temp_directory_path() / "dss_ppangle_pos_sys";
    auto incdir = fs::temp_directory_path() / "dss_ppangle_pos_inc";
    fs::create_directories(sysdir, ec);
    fs::create_directories(incdir, ec);
    { std::ofstream(sysdir / "shippedmac.json", std::ios::binary)
          << "{ \"header\": \"shippedmac.h\", \"macros\": ["
             "{ \"name\": \"SHIPPED_MAC\", \"replacement\": \"777\" } ] }\n"; }
    { std::ofstream(incdir / "platform.h", std::ios::binary)
          << "#define GATE_FLAG 1\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#include \"platform.h\"\n"
        "#if GATE_FLAG\n"
        "#include <shippedmac.h>\n"
        "int v = SHIPPED_MAC;\n"
        "#endif\n",
        r, {incdir}, {sysdir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    bool has777 = false, hasBareMac = false;
    for (auto const& l : lexs) {
        if (l == "777") has777 = true;
        if (l == "SHIPPED_MAC") hasBareMac = true;
    }
    EXPECT_TRUE(has777)
        << "the shipped object-macro must inject+expand under a quote-include-gated "
           "#if the pre-scan is blind to (D-PP-PRESCAN-ANGLE-MACRO-SPLICE-"
           "AUTHORITATIVE-LIVENESS)";
    EXPECT_FALSE(hasBareMac) << "SHIPPED_MAC must not survive the parser boundary "
                               "unexpanded";
    fs::remove_all(sysdir, ec);
    fs::remove_all(incdir, ec);
}

// NEGATIVE (final-output layer, RED-ON-DISABLE): splice-always emits the shipped
// `#define` into scanBuf even for a confidently-DEAD `#if 0` angle include, but the
// AUTHORITATIVE pass elides dead-branch `#define`s -- so the macro must NOT be usable
// in the final token stream (the P0016 one-directional-divergence invariant, measured
// where it matters: the tokens the parser sees). This is the layer the SynthBuilder
// emit-only property protects: the spliced define never enters the pre-scan's
// localMacros, and the authoritative pass drops it in the dead branch. RED-ON-DISABLE:
// if the authoritative pass ever stops eliding a dead-branch `#define`, SHIPPED_MAC
// would expand to 777 here -> the leak this pin forbids.
TEST(Preprocessor, DeadBranchAngleShippedMacroDoesNotLeakToFinalOutput) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto sysdir = fs::temp_directory_path() / "dss_ppangle_dead_sys";
    fs::create_directories(sysdir, ec);
    { std::ofstream(sysdir / "shippedmac.json", std::ios::binary)
          << "{ \"header\": \"shippedmac.h\", \"macros\": ["
             "{ \"name\": \"SHIPPED_MAC\", \"replacement\": \"777\" } ] }\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#if 0\n"
        "#include <shippedmac.h>\n"
        "#endif\n"
        "int v = SHIPPED_MAC;\n",
        r, {}, {sysdir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    bool has777 = false, hasBareMac = false;
    for (auto const& l : lexs) {
        if (l == "777") has777 = true;
        if (l == "SHIPPED_MAC") hasBareMac = true;
    }
    EXPECT_FALSE(has777)
        << "a #if 0-gated shipped include must NOT leak a usable macro into the final "
           "token stream (the authoritative pass elides the dead-branch #define)";
    EXPECT_TRUE(hasBareMac)
        << "SHIPPED_MAC stays an undefined bare identifier after the dead-branch "
           "include (final-output P0016 invariant)";
    fs::remove_all(sysdir, ec);
}

// DIRECT P0016 PIN (RED-ON-DISABLE against a future refactor): a shipped `#define`
// spliced inside a confidently-DEAD `#if 0` angle include must be INVISIBLE to a
// LATER pre-scan `#if defined(thatMacro)`. The splice is EMIT-ONLY (`out.append`
// into the authoritative buffer), never tracked into the pre-scan's localMacros, so
// the later guard folds dead in the pre-scan exactly as in the authoritative pass --
// the pre-scan is never MORE-live. Observed via the include gate: the later guard
// gates a MISSING quote-`#include`, which must NOT resolve (no missing-file error).
// RED-ON-DISABLE: route the splice through localMacros and the pre-scan would see
// the macro defined -> resolve that guard live -> the missing include errors (a P0016
// re-open). A DIRECT `#define` positive-control proves the probe can observe a leak.
TEST(Preprocessor, DeadBranchAngleShippedMacroInvisibleToLaterPrescanGuard) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto sysdir = fs::temp_directory_path() / "dss_ppangle_p0016_sys";
    fs::create_directories(sysdir, ec);
    { std::ofstream(sysdir / "shippedmac.json", std::ios::binary)
          << "{ \"header\": \"shippedmac.h\", \"macros\": ["
             "{ \"name\": \"SHIPPED_MAC\", \"replacement\": \"777\" } ] }\n"; }
    // MAIN: dead-branch splice must not leak into the later pre-scan guard.
    {
        PreprocessResult r;
        (void)ppLexemesWithDirs(
            "#if 0\n#include <shippedmac.h>\n#endif\n"
            "#if defined(SHIPPED_MAC)\n#include \"missing_p0016_probe.h\"\n#endif\n"
            "int v = 0;\n",
            r, {}, {sysdir});
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a shipped #define spliced in a DEAD #if 0 angle include must be "
               "INVISIBLE to a later pre-scan #if defined() -- emit-only, never "
               "tracked into localMacros (the P0016 one-directional invariant)";
    }
    // POSITIVE CONTROL: a DIRECT #define makes the SAME later guard resolve LIVE in
    // the pre-scan -> the missing include DOES error, so the MAIN case's silence
    // genuinely means "no leak" (not "the probe can't observe a live guard").
    {
        PreprocessResult r;
        (void)ppLexemesWithDirs(
            "#define SHIPPED_MAC 777\n"
            "#if defined(SHIPPED_MAC)\n#include \"missing_p0016_probe.h\"\n#endif\n"
            "int v = 0;\n",
            r, {}, {sysdir});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a DIRECT #define must make #if defined() live in the pre-scan (the "
               "leak-observability control)";
    }
    fs::remove_all(sysdir, ec);
}

// ITEM-2 PIN (RED-ON-DISABLE for the reportMalformed gate): removing the angle-arm
// `includeResolvable()` gate also un-gated the malformed-descriptor DIAGNOSTIC
// (`spliceSystemDescriptorMacros` emits P_PreprocessorIncludeError on an exists-but-
// fails-macro-decode descriptor). Threading `reportMalformed = includeResolvable()`
// restores its dead-branch inertness (C 6.10p1): the SPLICE stays ungated but the
// diagnostic fires only on a confidently-LIVE include. RED-ON-DISABLE: pass
// reportMalformed=true unconditionally and the DEAD case below emits the error.
TEST(Preprocessor, DeadBranchMalformedShippedDescriptorStaysSilent) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto sysdir = fs::temp_directory_path() / "dss_ppangle_malformed_sys";
    fs::create_directories(sysdir, ec);
    // Malformed: `macros` is not an array -> readShippedLibMacros fails to decode ->
    // the "descriptor malformed (macros)" P_PreprocessorIncludeError would fire.
    { std::ofstream(sysdir / "badmac.json", std::ios::binary)
          << "{ \"macros\": \"not-an-array\" }\n"; }
    // LIVE (top-level, confidently-live): the malformed descriptor errors loud.
    {
        PreprocessResult r;
        (void)ppLexemesWithDirs("#include <badmac.h>\nint v = 0;\n", r, {}, {sysdir});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a malformed shipped descriptor on a CONFIDENTLY-LIVE include must "
               "error loud (reportMalformed = includeResolvable() = true)";
    }
    // DEAD (#if 0): the SAME malformed descriptor stays SILENT (dead-branch inertness).
    {
        PreprocessResult r;
        (void)ppLexemesWithDirs(
            "#if 0\n#include <badmac.h>\n#endif\nint v = 0;\n", r, {}, {sysdir});
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a malformed shipped descriptor on a DEAD #if 0 include must stay "
               "SILENT -- the reportMalformed gate restores dead-branch inertness "
               "(code-audit Item-2)";
    }
    fs::remove_all(sysdir, ec);
}

// D-FFI-DESCRIPTOR-INCLUDES: the preprocessor macro-splice walks the TRANSITIVE
// descriptor closure. A parent descriptor declaring `includes:["child.h"]` where
// the CHILD ships a macro -> `#include <parent.h>` splices the CHILD's macro too,
// so a use of the child macro expands. RED-ON-DISABLE: revert the closure walk in
// spliceSystemDescriptorMacros to a single-descriptor read -> only parent.json's
// macros (none here) splice -> CHILD_MAC stays a bare undefined identifier.
TEST(Preprocessor, AngleIncludeSplicesTransitiveSiblingMacros) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto sysdir = fs::temp_directory_path() / "dss_ppangle_transitive_sys";
    fs::create_directories(sysdir, ec);
    // parent.json declares NO macros of its own — only the `includes` edge.
    { std::ofstream(sysdir / "parent.json", std::ios::binary)
          << "{ \"header\": \"parent.h\", \"includes\": [\"child.h\"], "
             "\"symbols\": [ { \"name\": \"pfn\", \"signature\": \"fn() -> i32\" } ] }\n"; }
    // child.json ships the macro reached transitively.
    { std::ofstream(sysdir / "child.json", std::ios::binary)
          << "{ \"header\": \"child.h\", \"macros\": ["
             "{ \"name\": \"CHILD_MAC\", \"replacement\": \"42\" } ] }\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#include <parent.h>\nint v = CHILD_MAC;\n", r, {}, {sysdir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    bool has42 = false, hasBareMac = false;
    for (auto const& l : lexs) {
        if (l == "42") has42 = true;
        if (l == "CHILD_MAC") hasBareMac = true;
    }
    EXPECT_TRUE(has42)
        << "a transitively-included sibling descriptor's macro must be spliced by "
           "the closure walk (CHILD_MAC -> 42 via parent.h's includes:[child.h])";
    EXPECT_FALSE(hasBareMac)
        << "CHILD_MAC must NOT survive unexpanded (the transitive #define was spliced)";
    fs::remove_all(sysdir, ec);
}

// D-FFI-DESCRIPTOR-INCLUDES: an `includes` CYCLE (parent<->child) must TERMINATE in
// the preprocessor splice (the shared visited-set), not infinite-loop. RED-ON-DISABLE:
// drop the visited-set guard in forEachDescriptorInClosure -> this hangs / OOMs.
TEST(Preprocessor, AngleIncludeCyclicIncludesTerminate) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto sysdir = fs::temp_directory_path() / "dss_ppangle_cyclic_sys";
    fs::create_directories(sysdir, ec);
    { std::ofstream(sysdir / "pa.json", std::ios::binary)
          << "{ \"header\": \"pa.h\", \"includes\": [\"pb.h\"], \"macros\": ["
             "{ \"name\": \"PA_MAC\", \"replacement\": \"40\" } ] }\n"; }
    { std::ofstream(sysdir / "pb.json", std::ios::binary)
          << "{ \"header\": \"pb.h\", \"includes\": [\"pa.h\"], \"macros\": ["
             "{ \"name\": \"PB_MAC\", \"replacement\": \"2\" } ] }\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#include <pa.h>\nint v = PA_MAC + PB_MAC;\n", r, {}, {sysdir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    bool has40 = false, has2 = false;
    for (auto const& l : lexs) {
        if (l == "40") has40 = true;
        if (l == "2") has2 = true;
    }
    EXPECT_TRUE(has40 && has2)
        << "a cyclic includes graph must terminate AND splice both descriptors' "
           "macros exactly once (the closure visited-set)";
    fs::remove_all(sysdir, ec);
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
    // token + a closing `"` -- see reconstructStringLiteral above -- so we check
    // the distinctive product BODIES (which slice from productText_; an invalid
    // multi-flush span would yield an empty/garbage lexeme, not the exact body).
    // This test is count-free by construction, so D-TOK-CLOSING-DELIMITER-HAS-NO-
    // TOKEN left it GREEN; only the wording above needed the correction (the
    // close is no longer "implied", it is a real token).
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

// FC17.9(a) (D-CSUBSET-C11-THREADS-TRAMPOLINES / -MACHO): <threads.h> is now COMPLETE on
// ALL legs — thrd_create/call_once/thrd_join land on elf (libc FFI), pe64 (kernel32 synth)
// AND macho (libSystem pthread synth: pthread_create/pthread_once/pthread_join). So a
// conforming impl must NOT define `__STDC_NO_THREADS__` on ANY target (C11 6.10.8.3 /
// C23 6.10.9.3) — the macro is REMOVED from c-subset.lang.json entirely. RED-on-disable:
// re-add the macro (any gating) → the corresponding arm flips to the no_threads
// (conformance-lie) branch.
TEST(Preprocessor, ThreadsCompleteStdcNoThreadsRemovedAllLegs) {
    char const* const src =
        "#ifdef __STDC_NO_THREADS__\nint no_threads;\n#else\nint has_threads;\n#endif\n";
    for (ObjectFormatKind fmt :
         {ObjectFormatKind::MachO, ObjectFormatKind::Elf, ObjectFormatKind::Pe}) {
        // every leg: <threads.h> complete -> the macro is UNDEFINED -> has_threads arm.
        PreprocessResult r;
        auto lexs = ppLexemesWithDefines(src, {}, r, fmt);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u)
            << "__STDC_NO_THREADS__ UNDEFINED on every leg (threads.h complete) -> has_threads";
        EXPECT_EQ(lexs[1], "has_threads");
    }
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

// ── FC17.9(h) C23 `#embed` (6.10.4 / N3096 6.10.3) + `__has_embed` (6.10.1) ──
// D-PP-EMBED. The quote-form scalar `#embed` splices a binary resource's bytes as
// a comma-separated list of decimal `int` constants; `__has_embed` reports the
// C23 trichotomy 0/1/2. Every fixture writes its resource at RUNTIME in binary
// mode (no git involvement); resources reach the resolver via `includeDirs`
// (main.c has no directory, so its self-dir is empty). Every assertion is the
// strongest provable property and red-on-disable.
namespace {
namespace fsemb = std::filesystem;

// Write `bytes` (exact, embedded NULs allowed) to `dir/name` in BINARY mode so a
// CR/LF/SUB byte survives verbatim; returns the containing directory.
[[nodiscard]] fsemb::path
writeEmbedResource(std::string const& sub, std::string const& name,
                   std::string const& bytes) {
    auto dir = fsemb::temp_directory_path() / sub;
    fsemb::create_directories(dir);
    std::ofstream out(dir / name, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return dir;
}

// The significant (non-trivia, non-Eof) tokens of preprocessing `text` with
// `includeDirs` as the resource search path.
[[nodiscard]] std::vector<Token>
ppEmbedTokens(std::string text, PreprocessResult& out,
              std::vector<fsemb::path> includeDirs) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(std::move(text), "main.c");
    std::vector<fsemb::path> noSys;
    out = preprocess(buf, schema, includeDirs, noSys);
    std::vector<Token> sig;
    for (Token const& t : out.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        sig.push_back(t);
    }
    return sig;
}

// The lexemes strictly between the FIRST `{` and the next `}` (the initializer
// list of a FILE-SCOPE `... x[] = { ... };`, so no function-body brace precedes).
[[nodiscard]] std::vector<std::string>
braceInner(std::vector<Token> const& toks, PreprocessResult const& out) {
    std::vector<std::string> inner;
    bool inBrace = false;
    for (Token const& t : toks) {
        std::string s{out.synthBuffer->slice(t.span)};
        if (!inBrace) { if (s == "{") inBrace = true; continue; }
        if (s == "}") break;
        inner.push_back(std::move(s));
    }
    return inner;
}

[[nodiscard]] std::string firstEmbedMsg(PreprocessResult const& r) {
    for (auto const& d : r.diagnostics->all())
        if (d.code == DiagnosticCode::P_PreprocessorEmbed) return d.actual;
    return {};
}
} // namespace

// T1 (RUN FIRST — the P2 residual closure): a multi-byte resource splices to
// EXACTLY the byte values as a comma-separated `int` list, product tokens
// (IntLiteral + Comma) accepted in brace-init position. Bytes {42, 0, 7}.
TEST(Preprocessor, FC179EmbedByteExactProductTokenStream) {
    auto dir = writeEmbedResource("dss_embed_t1", "three.bin",
                                  std::string("\x2a\x00\x07", 3));
    PreprocessResult r;
    auto toks = ppEmbedTokens(
        "static const unsigned char x[] = {\n#embed \"three.bin\"\n};\n", r, {dir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto inner = braceInner(toks, r);
    ASSERT_EQ(inner.size(), 5u) << "spliced as exactly `42, 0, 7`";
    EXPECT_EQ(inner[0], "42");
    EXPECT_EQ(inner[1], ",");
    EXPECT_EQ(inner[2], "0");
    EXPECT_EQ(inner[3], ",");
    EXPECT_EQ(inner[4], "7");
    // Each byte value is an IntLiteral token (a value the parser accepts in a
    // brace initializer -- the make-or-break P2 premise, empirically closed).
    int intLiterals = 0;
    bool inBrace = false;
    for (Token const& t : toks) {
        std::string s{r.synthBuffer->slice(t.span)};
        if (!inBrace) { if (s == "{") inBrace = true; continue; }
        if (s == "}") break;
        if (t.coreKind == CoreTokenKind::IntLiteral) ++intLiterals;
    }
    EXPECT_EQ(intLiterals, 3) << "each embedded byte materializes as an IntLiteral";
    std::error_code ec; fsemb::remove_all(dir, ec);
}

// T2: binary-mode read preserves hostile bytes. {13, 10, 26, 0, 255} -> the
// decimal texts. RED if anyone ever "simplifies" to a text-mode read (CR/LF/SUB
// would mangle on Windows).
TEST(Preprocessor, FC179EmbedBinaryModeReadPreservesHostileBytes) {
    auto dir = writeEmbedResource("dss_embed_t2", "hostile.bin",
                                  std::string("\x0d\x0a\x1a\x00\xff", 5));
    PreprocessResult r;
    auto toks = ppEmbedTokens(
        "static const unsigned char x[] = {\n#embed \"hostile.bin\"\n};\n", r, {dir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto inner = braceInner(toks, r);
    ASSERT_EQ(inner.size(), 9u) << "5 values + 4 commas";
    EXPECT_EQ(inner[0], "13");
    EXPECT_EQ(inner[2], "10");
    EXPECT_EQ(inner[4], "26");
    EXPECT_EQ(inner[6], "0");
    EXPECT_EQ(inner[8], "255");
    std::error_code ec; fsemb::remove_all(dir, ec);
}

// T3: the dss-state `c23_embed` probe shape -- a single-byte '*' (0x2A) resource
// -> the value 42.
TEST(Preprocessor, FC179EmbedProbeShapeSingleByteIsFortyTwo) {
    auto dir = writeEmbedResource("dss_embed_t3", "answer.bin", std::string("*"));
    PreprocessResult r;
    auto toks = ppEmbedTokens(
        "static const unsigned char answer[] = {\n#embed \"answer.bin\"\n};\n",
        r, {dir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto inner = braceInner(toks, r);
    ASSERT_EQ(inner.size(), 1u);
    EXPECT_EQ(inner[0], "42") << "'*' = 0x2A = 42";
    std::error_code ec; fsemb::remove_all(dir, ec);
}

// T4: the resource resolves relative to the FILE THAT CONTAINS the directive (the
// per-origin dir via the line-map), NOT the main file's dir. `inc/h.h` carries
// the `#embed "res.bin"`; `inc/res.bin` exists but NO copy sits next to main.c.
// RED-ON-DISABLE: resolving against the main dir (not the header's) misses res.bin.
TEST(Preprocessor, FC179EmbedResolvesRelativeToIncludingHeader) {
    auto root = fsemb::temp_directory_path() / "dss_embed_t4";
    auto inc  = root / "inc";
    fsemb::create_directories(inc);
    { std::ofstream(inc / "h.h", std::ios::binary)
          << "static const unsigned char x[] = {\n#embed \"res.bin\"\n};\n"; }
    { std::ofstream out(inc / "res.bin", std::ios::binary); out.put('*'); }
    // main.c lives in `root`, includes `inc/h.h`; res.bin is NOT in `root`.
    auto buf = SourceBuffer::fromString("#include \"inc/h.h\"\n",
                                        (root / "main.c").string());
    auto schema = cSubset();
    std::vector<fsemb::path> dirs{root};
    auto r = preprocess(buf, schema, dirs);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "#embed inside a spliced header resolves relative to the HEADER's dir";
    bool has42 = false;
    for (Token const& t : r.tokens)
        if (std::string{r.synthBuffer->slice(t.span)} == "42") has42 = true;
    EXPECT_TRUE(has42) << "the header-relative resource spliced its byte (42)";
    std::error_code ec; fsemb::remove_all(root, ec);
}

// T5: a missing resource fails loud (a C23 constraint violation).
TEST(Preprocessor, FC179EmbedMissingResourceFailsLoud) {
    PreprocessResult r;
    (void)ppEmbedTokens(
        "static const unsigned char x[] = {\n#embed \"no_such_embed_xyz.bin\"\n};\n",
        r, {});
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed))
        << "a missing #embed resource must fail loud";
    EXPECT_NE(firstEmbedMsg(r).find("not found"), std::string::npos);
}

// T6: `#embed ""` (empty filename) fails loud.
TEST(Preprocessor, FC179EmbedEmptyFilenameFailsLoud) {
    PreprocessResult r;
    (void)ppEmbedTokens("static const unsigned char x[] = {\n#embed \"\"\n};\n",
                        r, {});
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed));
}

// T7 (D-PP-EMBED-PARAMS): ANY standard/vendor parameter after the filename fails
// loud -- even for an EXISTING resource (silently honoring `limit` would embed a
// different byte set = a silent miscompile). Resource present, so ONLY the param
// can red.
TEST(Preprocessor, FC179EmbedParametersFailLoud) {
    auto dir = writeEmbedResource("dss_embed_t7", "r.bin", std::string("*"));
    for (std::string const& param :
         {"limit(1)", "if_empty(0)", "prefix(1)", "suffix(2)", "gnu"}) {
        PreprocessResult r;
        (void)ppEmbedTokens(
            "static const unsigned char x[] = {\n#embed \"r.bin\" " + param
                + "\n};\n",
            r, {dir});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed))
            << "#embed parameter must fail loud (even for an existing file): "
            << param;
    }
    std::error_code ec; fsemb::remove_all(dir, ec);
}

// T8 (D-PP-EMBED-ANGLE / D-PP-EMBED-MACRO-ARG): the angle form and a
// macro-expanded argument are deferred loud (never silent).
TEST(Preprocessor, FC179EmbedAngleAndMacroArgFailLoud) {
    {
        PreprocessResult r;
        (void)ppEmbedTokens(
            "static const unsigned char x[] = {\n#embed <r.bin>\n};\n", r, {});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed))
            << "the angle form is a loud deferral";
    }
    {
        PreprocessResult r;
        (void)ppEmbedTokens(
            "static const unsigned char x[] = {\n#embed RES\n};\n", r, {});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed))
            << "a macro-expanded argument is a loud deferral";
    }
}

// T9: an empty resource expands to NOTHING (C23 6.10.3/6.10.4). `{ <empty> 42 }`
// -> array {42}.
TEST(Preprocessor, FC179EmbedEmptyResourceExpandsToNothing) {
    auto dir = writeEmbedResource("dss_embed_t9", "empty.bin", std::string());
    PreprocessResult r;
    auto toks = ppEmbedTokens(
        "static const unsigned char x[] = {\n#embed \"empty.bin\"\n42\n};\n",
        r, {dir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto inner = braceInner(toks, r);
    ASSERT_EQ(inner.size(), 1u) << "empty resource -> zero tokens spliced";
    EXPECT_EQ(inner[0], "42");
    std::error_code ec; fsemb::remove_all(dir, ec);
}

// T10: `__has_embed` trichotomy -- found(1) / empty(2, == __STDC_EMBED_EMPTY__) /
// missing(0) / an unsupported parameter clause -> NOT_FOUND(0) NOT an error (the
// C23 feature-probe contract) / malformed -> loud.
TEST(Preprocessor, FC179HasEmbedTrichotomy) {
    auto dir = writeEmbedResource("dss_embed_t10", "full.bin", std::string("*"));
    (void)writeEmbedResource("dss_embed_t10", "empty.bin", std::string());
    { // found (non-empty) -> 1 -> yes
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#if __has_embed(\"full.bin\")\nint yes;\n#else\nint no;\n#endif\n",
            r, {dir}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "yes") << "a non-empty resource -> __has_embed == 1";
    }
    { // empty -> 2, and 2 == __STDC_EMBED_EMPTY__ (the config<->engine coupling)
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#if __has_embed(\"empty.bin\") == __STDC_EMBED_EMPTY__\n"
            "int yes;\n#else\nint no;\n#endif\n",
            r, {dir}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "yes")
            << "an empty resource -> __has_embed == 2 == __STDC_EMBED_EMPTY__";
    }
    { // missing -> 0 -> no
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#if __has_embed(\"nope.bin\")\nint yes;\n#else\nint no;\n#endif\n",
            r, {dir}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "no") << "a missing resource -> __has_embed == 0";
    }
    { // an unsupported parameter clause -> NOT_FOUND(0), NOT an error
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#if __has_embed(\"full.bin\" limit(1))\n"
            "int yes;\n#else\nint no;\n#endif\n",
            r, {dir}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "an unsupported parameter is the standard NOT_FOUND signal, "
               "not an error";
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "no")
            << "any parameter clause -> __has_embed == 0 (C23 feature-probe)";
    }
    { // malformed (no `(`) -> loud
        PreprocessResult r;
        (void)ppLexemesWithDirs("#if __has_embed\nint a;\n#endif\n", r, {dir}, {});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed))
            << "__has_embed with no `(` must fail loud";
    }
    { // malformed (empty name) -> loud
        PreprocessResult r;
        (void)ppLexemesWithDirs("#if __has_embed(\"\")\nint a;\n#endif\n",
                                r, {dir}, {});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed))
            << "__has_embed with an empty name must fail loud";
    }
    std::error_code ec; fsemb::remove_all(dir, ec);
}

// T11: dead-branch parity -- a `#if 0` `#embed` of a MISSING file is silent (no
// resolution, no diagnostic; the :stackActive gate), while the live twin fails
// loud. RED-ON-DISABLE: moving the arm above the gate reds the first half.
TEST(Preprocessor, FC179EmbedInDeadBranchIsSilent) {
    {
        PreprocessResult r;
        (void)ppEmbedTokens(
            "int a;\n#if 0\n#embed \"missing_dead.bin\"\n#endif\nint b;\n", r, {});
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "a dead-branch #embed of a missing file must be silent";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed));
    }
    {
        PreprocessResult r;
        (void)ppEmbedTokens(
            "#if 1\nstatic const unsigned char x[] = {\n"
            "#embed \"missing_live.bin\"\n};\n#endif\n",
            r, {});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed))
            << "a LIVE #embed of a missing file must fail loud";
    }
}

// T13 (C 6.10.8.1): a `#define` of a predefined `__STDC_EMBED_*` macro fails loud.
TEST(Preprocessor, FC179EmbedStdcMacrosAreProtectedPredefines) {
    for (std::string const& name : {"__STDC_EMBED_NOT_FOUND__",
                                    "__STDC_EMBED_FOUND__",
                                    "__STDC_EMBED_EMPTY__"}) {
        PreprocessResult r;
        (void)ppEmbedTokens("#define " + name + " 9\nint a;\n", r, {});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
            << "#define of a predefined embed macro must fail loud: " << name;
    }
}

// T14 (agnosticism): the directive WORD is config-driven, never a hard-coded
// "embed". Rebind `embedDirective` off "embed" -> "embad": now `#embed` is an
// unknown directive (P0015) and `#embad "..."` drives the embed handler
// (P_PreprocessorEmbed). RED-ON-DISABLE: a hard-coded "embed" ignores the rebind.
TEST(Preprocessor, FC179EmbedDirectiveIsConfigDrivenNotHardcoded) {
    auto schema = reboundCSubset("\"embed\"", "\"embad\"", "<rebound-embed>");
    ASSERT_TRUE(schema != nullptr);
    ASSERT_EQ(schema->preprocess().embedDirective, "embad");
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    {
        auto buf = SourceBuffer::fromString(
            "static const unsigned char x[] = {\n#embed \"missing.bin\"\n};\n",
            "main.c");
        auto r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "with the word rebound, `#embed` is an unknown directive (P0015)";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed));
    }
    {
        auto buf = SourceBuffer::fromString(
            "static const unsigned char x[] = {\n#embad \"missing.bin\"\n};\n",
            "main.c");
        auto r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed))
            << "the rebound `#embad` word now drives the embed handler";
    }
}

// FIX-1 (D-PP-EMBED, the streaming boundary): the PURE size-budget helper is a
// catchable LOUD wall, not an OOM. Called directly with a COUNT (no giant
// fixture). RED-ON-DISABLE: removing the gate makes the helper always return
// nullopt -> the over-budget assertion reds.
TEST(Preprocessor, FC179EmbedResourceSizeBudgetIsLoudNotOom) {
    EXPECT_FALSE(embedResourceSizeError(0).has_value());
    EXPECT_FALSE(embedResourceSizeError(kEmbedMaxResourceBytes).has_value())
        << "exactly at the budget is allowed";
    auto over = embedResourceSizeError(kEmbedMaxResourceBytes + 1);
    ASSERT_TRUE(over.has_value())
        << "one byte over the budget must yield a loud diagnostic, never an OOM";
    EXPECT_NE(over->find("D-PP-EMBED-STREAMING"), std::string::npos)
        << "the message names the streaming-deferral boundary";
}

// T12 (the pre-scan-parity witness, §9 FIX-4): a `#if __has_embed("res.bin")`
// gates a quote-`#include "defs.h"` whose header supplies a typedef `main` needs.
// GREEN requires the SynthBuilder PRE-SCAN to evaluate `__has_embed` exactly like
// the authoritative pass (live -> the header is spliced, so the `typedef` token
// appears). RED-ON-DISABLE (demonstrated by stubbing the pre-scan `embedCb` to
// {}): the guard turns UNCERTAIN -> the quote-include is conservatively SKIPPED
// -> the `typedef` is ABSENT (and downstream the type is unresolved -- the loud
// divergence direction, never silent).
TEST(Preprocessor, FC179HasEmbedPreScanParityGatesQuoteInclude) {
    auto dir = writeEmbedResource("dss_embed_t12", "res.bin", std::string("*"));
    { std::ofstream(dir / "defs.h", std::ios::binary)
          << "typedef int EmbedGatedType;\n"; }
    auto buf = SourceBuffer::fromString(
        "#if __has_embed(\"res.bin\")\n#include \"defs.h\"\n#endif\n"
        "EmbedGatedType v;\n",
        (dir / "main.c").string());
    auto schema = cSubset();
    std::vector<fsemb::path> dirs{dir};
    auto r = preprocess(buf, schema, dirs);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // The header's `typedef` keyword appears ONLY if the pre-scan took the
    // __has_embed branch and spliced defs.h (main.c has no `typedef` of its own).
    bool sawTypedef = false;
    for (Token const& t : r.tokens)
        if (std::string{r.synthBuffer->slice(t.span)} == "typedef")
            sawTypedef = true;
    EXPECT_TRUE(sawTypedef)
        << "the pre-scan must evaluate __has_embed like the authoritative pass "
           "and splice the gated header (else the include is conservatively "
           "skipped and the type is unresolved)";
    std::error_code ec; fsemb::remove_all(dir, ec);
}

// D-PERF-1 (macro-pass O(n^2) -> O(n)) EFFECTIVENESS PIN. The macro expander
// consumes its stream from a FRONT-CONSUMED deque and splices only at the front,
// so the TOTAL splice-work (`PreprocessResult::macroTokenMoves`, summing
// `consumed + produced` over every `spliceOver`) is LINEAR in the invocation
// count N. On a pathological macro-dense source -- one trivial object-like macro
// invoked N times -- each `A` pops 1 (`A`) + pushes 1 (`1`) = 2 moves, so the
// total is ~2*N. We pin it <= 8*N (generous linear headroom; the MEASURED value
// is 2*N). RED-ON-DISABLE intent: this pins that the pass does LINEAR total
// splice-work -- a regression that re-splices an ever-GROWING region (the
// superlinear pattern the front-consumed deque eliminates) blows the bound. The
// wall-clock O(n^2)->O(n) win itself (the old mid-vector erase+insert paid an
// O(n) PHYSICAL tail-shift per call on top of the same logical count) is
// confirmed separately by the sqlite `preprocess-expand` phase re-measure.
TEST(Preprocessor, DPerf1MacroPassTokenMovesStayLinear) {
    constexpr std::size_t N = 4000;
    std::string src = "#define A 1\n";
    src.reserve(src.size() + N * 2 + 1);
    for (std::size_t k = 0; k < N; ++k) src += "A ";
    src += "\n";

    PreprocessResult r;
    auto lexs = ppLexemes(std::move(src), r);
    EXPECT_FALSE(r.diagnostics->hasErrors());

    // Non-vacuous: every one of the N `A`s actually expanded to `1` (otherwise a
    // zero-work pass would trivially satisfy the bound).
    std::size_t ones = 0;
    for (auto const& l : lexs)
        if (l == "1") ++ones;
    EXPECT_EQ(ones, N) << "each object-like `A` must expand to `1`";

    // The load-bearing assertion: EXACTLY 2 front-splice token-moves per object-
    // like expansion (pop the name `A`, push its replacement `1`) -> 2*N total,
    // LINEAR in N. The exact count is the strongest provable property here.
    // RED-ON-DISABLE: `tokenMoves_` is intrinsic to the D-PERF-1 deque splice;
    // reverting `spliceOver` to the pre-D-PERF-1 `std::vector` erase+insert removes
    // it (the counter lives inside the deque splice), so macroTokenMoves -> 0 and
    // this EQ fails (0 != 2*N). A logical op-counter
    // cannot by itself distinguish the deque's O(n) front-splice from a
    // same-formula vector mid-splice; the PHYSICAL O(n^2)->O(n) tail-shift win is
    // proven separately by the sqlite `preprocess-expand` phase re-measure
    // (~4.3s -> ~3.3s this cycle) + the independently-audited front-consumed design.
    EXPECT_EQ(r.macroTokenMoves, 2 * N)
        << "the macro pass must do exactly 2 front-splice moves per expansion "
           "(2*N total, linear); got " << r.macroTokenMoves << " for N=" << N;
}

// AUDIT FIX #1 correctness pin (the one silent-miscompile seam of the deque
// rewrite). A function-like macro invoked with the WRONG arity fails loud, emits
// the NAME verbatim, and DROPS the whole malformed `(...)` call -- while any
// TRAILING tokens after the call survive. In the front-consumed-deque model the
// arity-bad arm must pop `past` tokens TOTAL off the front (the name + the
// malformed call); a no-op (or popping only the name) would re-scan/re-expand
// the dropped args -- a SILENT divergence (and, for a pure no-op, an infinite
// loop). RED-ON-DISABLE: revert the arm to leave the malformed args on the
// stream and `(`, `1`, `)` leak into the output (or the pass hangs) -- either
// way `lexs` is no longer exactly {M, tail_ok}.
TEST(Preprocessor, ArityMismatchDropsMalformedCallKeepsTrailingTokens) {
    PreprocessResult r;
    // M expects 2 args; invoked with 1 -> arity mismatch. `tail_ok` trails the
    // malformed call and must survive.
    auto lexs = ppLexemes("#define M(a,b) a b\nM(1) tail_ok\n", r);

    // (1) the arity diagnostic fires.
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroArgument))
        << "a 2-parameter macro called with 1 argument must fail loud";

    // (2)+(3)+(4): exactly `M tail_ok` reaches the parser -- the name survives,
    // the `(1)` is dropped, `tail_ok` survives AFTER it.
    ASSERT_EQ(lexs.size(), 2u)
        << "expected exactly {M, tail_ok}: the malformed (1) must be dropped and "
           "tail_ok must survive";
    EXPECT_EQ(lexs[0], "M") << "the macro name must be emitted verbatim";
    EXPECT_EQ(lexs[1], "tail_ok") << "the trailing token must survive the drop";
    // The dropped call must not leak a single token.
    for (auto const& l : lexs) {
        EXPECT_NE(l, "(") << "the malformed call's `(` leaked";
        EXPECT_NE(l, "1") << "the malformed call's arg leaked";
        EXPECT_NE(l, ")") << "the malformed call's `)` leaked";
    }
}

// ============================================================================
// TF-C70 (D-CPP-ERROR-WARNING) -- C23 6.10.5 `#error` / 6.10.6 `#warning`.
//
// The load-bearing property is TWO-DIRECTIONAL, and the SILENT half is the one
// that carries the weight. C 6.10p1 EXECUTES a directive only when its
// enclosing conditional group is live, so an `#error` in a NOT-TAKEN branch
// must produce NO diagnostic at all. Every macOS SDK header depends on exactly
// that: `sys/cdefs.h` and friends park `#error`s inside unsupported-
// configuration branches that a supported target skips. A suite that asserted
// only "a reached `#error` diagnoses" would be fully satisfied by an
// implementation that fires on every LEXED `#error` -- i.e. one that cannot
// preprocess a single system header. So the silence tests below are paired
// with positive twins (an implementation that NEVER fires cannot pass either),
// and each test names the concrete engine edit that turns THAT test red.
//
// The engine seam, for the red-on-disable notes: `MacroExpander::
// handleDirective` (src/analysis/preprocess/preprocessor.cpp) dispatches the
// `#if`-family UNCONDITIONALLY (they must keep `condStack_` balanced inside a
// dead group), then hits `if (!stackActive()) return end;` -- and only BELOW
// that gate live the `#error`/`#warning` arms. Reachability, not recognition.
// ============================================================================
namespace {

// The FIRST diagnostic carrying `code` (nullptr if none). `firstMessageWithCode`
// above yields only the text; `#warning`'s entire contract is its SEVERITY, so
// these tests need the diagnostic itself.
[[nodiscard]] ParseDiagnostic const* firstDiagWithCode(PreprocessResult const& r,
                                                       DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == code) return &d;
    }
    return nullptr;
}

// Rebind ONE `"<key>": "<word>"` string field of the shipped c-subset config to
// `newWord` and reload. Unlike handing `reboundCSubset` a literal key-value
// spelling, this LOCATES the value (key -> `:` -> the quoted value), so the
// rebind survives any re-alignment of the config's columns. A missing key is an
// ADD_FAILURE, never a silent no-op: a rebind whose `from` stopped matching
// would otherwise re-run the BASELINE schema and pass VACUOUSLY -- the exact
// failure mode that makes a config-driven test worthless.
[[nodiscard]] std::shared_ptr<GrammarSchema const>
reboundPreprocessWord(std::string const& key, std::string const& newWord,
                      std::string const& label) {
    std::string const text = loadShippedCSubsetText();
    if (text.empty()) {
        ADD_FAILURE() << "could not locate shipped c-subset config";
        return nullptr;
    }
    std::string const quotedKey = "\"" + key + "\"";
    auto const keyPos = text.find(quotedKey);
    if (keyPos == std::string::npos) {
        ADD_FAILURE() << "shipped c-subset config declares no " << quotedKey;
        return nullptr;
    }
    auto const colon = text.find(':', keyPos + quotedKey.size());
    auto const openQ = (colon == std::string::npos)
                           ? std::string::npos
                           : text.find('"', colon + 1);
    auto const closeQ = (openQ == std::string::npos)
                            ? std::string::npos
                            : text.find('"', openQ + 1);
    if (closeQ == std::string::npos) {
        ADD_FAILURE() << quotedKey << " is not a `\"key\": \"value\"` pair";
        return nullptr;
    }
    return reboundCSubset(text.substr(keyPos, closeQ + 1 - keyPos),
                          quotedKey + ": \"" + newWord + "\"", label);
}

// `lexs` is EXACTLY the five lexemes of `int x=1;` -- the whole program once the
// directive lines are gone. Pins consumption as well as (non-)diagnosis: a
// directive whose `#`/word leaked into the parser stream fails here even when
// the diagnostic assertions pass.
[[nodiscard]] ::testing::AssertionResult isIntXAssignOne(
    std::vector<std::string> const& lexs) {
    static char const* const kWant[] = {"int", "x", "=", "1", ";"};
    if (lexs.size() != 5) {
        return ::testing::AssertionFailure()
               << "expected exactly 5 lexemes (`int x = 1 ;`), got "
               << lexs.size();
    }
    for (std::size_t i = 0; i < 5; ++i) {
        if (lexs[i] != kWant[i]) {
            return ::testing::AssertionFailure()
                   << "lexeme " << i << ": expected '" << kWant[i] << "', got '"
                   << lexs[i] << "'";
        }
    }
    return ::testing::AssertionSuccess();
}

} // namespace

// ── SILENCE (the load-bearing half) ─────────────────────────────────────────

// (1) `#if 0` elides its group, so the `#error` inside is never EXECUTED
// (C 6.10p1) -- no diagnostic of any kind, and the surviving program is exactly
// `int x = 1 ;`.
// RED-ON-DISABLE: move the `#error` arm out of the `else if` chain in
// `handleDirective` and up into the UNCONDITIONAL `#if`-family dispatch above
// `if (!stackActive()) return end;` -- the directive is then diagnosed on
// RECOGNITION rather than reachability and this test reds on every assertion.
TEST(Preprocessor, TfC70ErrorDirectiveInIfZeroIsSilent) {
    PreprocessResult r;
    auto lexs = ppLexemes("#if 0\n#error must not fire\n#endif\nint x=1;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
        << "an `#error` in a NOT-TAKEN `#if 0` group must never fire -- every "
           "macOS SDK header parks `#error`s in branches a supported target "
           "skips";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorWarningDirective))
        << "nor may it be downgraded into a warning: a skipped group produces "
           "NO diagnostic";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "a dead-branch `#error` must be completely silent (got "
        << r.diagnostics->all().size() << " diagnostics)";
    EXPECT_TRUE(isIntXAssignOne(lexs));
}

// (2) The `#ifdef` path is a SEPARATE evaluator arm from `#if` (`SbIfKind::
// Ifdef` reads definedness DIRECTLY; `#if 0` runs the ICE expression
// evaluator), so the silence contract is pinned once per arm. An undefined
// macro name leaves the group dead.
// RED-ON-DISABLE: same edit as (1) (hoisting the arm above the `stackActive()`
// gate); additionally red if `sbHandleIf`'s `Ifdef` branch ever pushed an
// ACTIVE frame for an undefined name.
TEST(Preprocessor, TfC70ErrorDirectiveInIfdefUndefinedMacroIsSilent) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#ifdef DSS_NEVER_DEFINED\n#error must not fire\n#endif\nint x=1;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
        << "`#ifdef <undefined>` is a dead group -- its `#error` is not "
           "executed (the `#ifdef` evaluator arm, distinct from `#if 0`)";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "got " << r.diagnostics->all().size() << " diagnostics";
    EXPECT_TRUE(isIntXAssignOne(lexs));
}

// (3) The NOT-TAKEN `#else` arm: the group's `#if 1` already took, so the
// `#else` body never executes. Distinct frame state from (1)/(2) -- here the
// frame was pushed ACTIVE and is later turned off by `sbHandleElse`, so this
// pins the `#else` transition rather than the open.
// RED-ON-DISABLE: same hoist as (1); also red if `sbHandleElse` stopped
// clearing `thisBranchActive` once `anyBranchTaken`.
TEST(Preprocessor, TfC70ErrorDirectiveInNotTakenElseIsSilent) {
    PreprocessResult r;
    auto lexs = ppLexemes("#if 1\nint x;\n#else\n#error dead else\n#endif\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
        << "the `#else` arm of an already-taken group is dead -- its `#error` "
           "must not fire";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "got " << r.diagnostics->all().size() << " diagnostics";
    ASSERT_EQ(lexs.size(), 3u) << "only the taken arm `int x ;` survives";
    EXPECT_EQ(lexs[0], "int");
    EXPECT_EQ(lexs[1], "x");
    EXPECT_EQ(lexs[2], ";");
}

// (4) ★ NESTED: an `#if 1` whose own controlling expression is TRUE, sitting
// inside a dead `#if 0`. Liveness is a property of the WHOLE frame chain, not
// of the innermost directive -- the shape that appears in real headers as a
// feature test nested under a platform guard.
// RED-ON-DISABLE: the (1) hoist reds this too. The nesting-specific pin is the
// CONJUNCTION of two engine facts, and the test reds if BOTH are undone:
// `sbHandleIf` pushing `thisBranchActive = enclosing && cond`
// (preprocessor.cpp:287) AND `sbStackActive` walking every frame
// (preprocessor.cpp:204-209). Undoing either ALONE leaves the composite
// predicate correct -- they are observationally equivalent by construction,
// which is why no test can separate them; what this test pins is that at least
// one of the two survives, i.e. that a live-looking inner group inside a dead
// outer group stays dead. No other test in this file exercises the nested
// shape.
TEST(Preprocessor, TfC70ErrorDirectiveNestedInDeadOuterBranchStaysSilent) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if 0\n#if 1\n#error inner live but outer dead\n#endif\n#endif\n"
        "int x=1;\n",
        r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
        << "the inner `#if 1` is nested inside a dead `#if 0`: liveness is the "
           "conjunction over EVERY open frame, so the `#error` is not executed";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "got " << r.diagnostics->all().size() << " diagnostics";
    EXPECT_TRUE(isIntXAssignOne(lexs));
}

// ── POSITIVE TWINS (so a never-fires implementation cannot pass) ────────────

// (5) A REACHED `#error` fails loud, at Error severity, and its message carries
// the author's `pp-tokens` verbatim (C 6.10.5p1 makes including them a
// CONSTRAINT, not a nicety). The directive line itself is consumed -- a
// directive is not program text.
// RED-ON-DISABLE: delete the `else if (... word == cfg().errorDirective)` arm
// in `handleDirective` -- the line falls through to the generic
// `P_PreprocessorUnsupported` (P0015) fail-loud, whose message is "unsupported
// preprocessor directive ...: error" and carries none of the user's text.
TEST(Preprocessor, TfC70ErrorDirectiveReachedFailsLoudWithUserText) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#error unsupported configuration for this target\n"
                  "int x=1;\n",
                  r);
    ASSERT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
        << "a REACHED `#error` must fail loud";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
        << "it must route to the `#error` arm, not the generic "
           "unsupported-directive fail-loud";
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "`#error` is fatal: translation does not continue (C 6.10.5)";
    auto const* d =
        firstDiagWithCode(r, DiagnosticCode::P_PreprocessorErrorDirective);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->severity, DiagnosticSeverity::Error)
        << "`#error` is an ERROR, not a warning";
    EXPECT_NE(d->actual.find("unsupported configuration"), std::string::npos)
        << "the message must INCLUDE the author's pp-tokens (C 6.10.5p1); got: "
        << d->actual;
    EXPECT_TRUE(isIntXAssignOne(lexs))
        << "the `#error` line is consumed, never forwarded to the parser";
}

// (6) The positive twin of (4): two LIVE frames. Proves the liveness walk does
// not spuriously report a nested group dead -- the direction that would make
// `#error` unreachable-by-construction and silently satisfy every silence test
// above.
// RED-ON-DISABLE: make `sbStackActive` return false for any non-empty stack
// (the over-conservative mirror of the (1) hoist) -- (1)-(4) stay green and
// ONLY this test reds.
TEST(Preprocessor, TfC70ErrorDirectiveLiveInNestedTakenBranchStillFires) {
    PreprocessResult r;
    (void)ppLexemes(
        "#if 1\n#if 1\n#error live through two frames\n#endif\n#endif\n"
        "int x=1;\n",
        r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
        << "both enclosing frames are TAKEN, so the `#error` IS executed";
    EXPECT_TRUE(r.diagnostics->hasErrors());
}

// ── SEMANTICS ───────────────────────────────────────────────────────────────

// (7) C23 6.10.5 spells the directive `# error pp-tokens_opt` -- the operand is
// OPTIONAL, so a BARE `#error` is well-formed and still fires, with empty user
// text. It must NOT be reported as a malformed directive
// (`P_PreprocessorDirective`, P0013).
// RED-ON-DISABLE: add an "operand required" check to the `#error` arm (the
// obvious-looking `if (operand.empty()) emit(P_PreprocessorDirective)`), which
// is what the `pp-tokens_opt` grammar forbids -- the code assertion reds.
TEST(Preprocessor, TfC70ErrorDirectiveWithNoOperandStillFires) {
    PreprocessResult r;
    (void)ppLexemes("#error\nint x=1;\n", r);
    ASSERT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
        << "`# error pp-tokens_opt`: a bare `#error` is well-formed and fires";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "an absent operand is NOT a malformed directive (C23 6.10.5's "
           "`pp-tokens_opt`)";
    EXPECT_EQ(firstMessageWithCode(
                  r, DiagnosticCode::P_PreprocessorErrorDirective),
              "#error: ")
        << "the message is the fixed label plus EMPTY user text";
}

// (8) The operand is NOT macro-expanded. C 6.10.5 requires the message to
// include "the specified pp-tokens" -- the tokens as WRITTEN. A directive line
// is never run through the expander (C 6.10.3p11), so `X` stays `X`.
// RED-ON-DISABLE: expand the operand before building the message (route it
// through the macro pass instead of `directiveOperandText`'s verbatim slice) --
// the message becomes "#error: 1 is bad" and both assertions red.
TEST(Preprocessor, TfC70ErrorDirectiveOperandIsNotMacroExpanded) {
    PreprocessResult r;
    (void)ppLexemes("#define X 1\n#error X is bad\n", r);
    ASSERT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective));
    std::string const msg = firstMessageWithCode(
        r, DiagnosticCode::P_PreprocessorErrorDirective);
    EXPECT_NE(msg.find("X is bad"), std::string::npos)
        << "the pp-tokens are reported AS WRITTEN; got: " << msg;
    EXPECT_EQ(msg.find("1 is bad"), std::string::npos)
        << "the operand must NOT be macro-expanded; got: " << msg;
}

// OPERAND FORMS the bare-prose tests above do NOT reach. C23 6.10.5p1/6.10.6
// require the pp-tokens VERBATIM, and a trailing LITERAL used to be a special
// case: the tokenizer split a literal into a StringStart token holding only the
// opening quote plus a coalesced BODY token covering the bytes BETWEEN the
// quotes, so the CLOSING quote belonged to no token's span and a naive
// first..last span join stopped one byte short.
//
// ★ WHY THIS TEST EXISTS AT ALL: every other TfC70 operand test uses bare prose,
// and bare prose is exactly the form that does NOT expose the bug — as does
// `"abc" tail`, because there the join ends on `tail`. The suite was green over
// that subset while `#warning "Unsupported compiler detected"` — sys/cdefs.h:81,
// the literal shape that motivated this whole feature — silently reported a
// truncated `"Unsupported compiler detected` with no closing quote. A
// multi-FORM contract needs a test per form, not per site.
//
// ★ RENEGOTIATED — D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN. This test stayed GREEN
// across the anchor's fix, but its RED-ON-DISABLE instruction went stale and had
// to be rewritten rather than left to rot. It used to read:
//
//     "delete the closing-delimiter re-consume in `directiveOperandText`
//      (the `tail[0] == '\"'` guard) -> (A) and (B) red."
//
// That guard NO LONGER EXISTS. It was one of the four hand-compensations the
// anchor catalogued, and closing the anchor at the ROOT deleted it: the closing
// delimiter is now a token of its own, so `last - 1` (the line's last
// non-trivia token) IS the closer and the span join reaches it BY CONSTRUCTION.
// An instruction to disable deleted code cannot be followed, and a
// RED-ON-DISABLE note that cannot be followed is worse than none — it reads as
// verified when nothing is verifying it.
//
// WHAT THIS TEST GUARDS NOW: that the operand text still reaches the closing
// delimiter, whatever mechanism supplies it. It is no longer a pin on a guard;
// it is a pin on the OBSERVABLE CONTRACT (C23 6.10.5p1/6.10.6), which is the
// more durable thing to assert and is why the test survived the mechanism
// change unedited. RED-ON-DISABLE today: revert the tokenizer's closer emit —
// the closer stops being a token, `last - 1` falls back to the body, and (A),
// (B) and (C) all go red together. Note that is strictly BROADER coverage than
// before: one revert now fails all three delimiter forms, where the old guard
// had to be disabled per-byte.
//
// The negative cases (D)/(E) guard the OTHER direction — that nothing appends a
// phantom byte past the operand — and (F) guards the trailing-trivia policy.
TEST(Preprocessor, TfC70OperandKeepsClosingStringDelimiter) {
    auto msgOf = [](char const* src) {
        PreprocessResult r;
        (void)ppLexemes(src, r);
        return firstMessageWithCode(
            r, DiagnosticCode::P_PreprocessorWarningDirective);
    };
    // (A) THE sys/cdefs.h:81 SHAPE — operand is a lone string literal.
    EXPECT_NE(msgOf("#warning \"Unsupported compiler detected\"\n")
                  .find("\"Unsupported compiler detected\""),
              std::string::npos)
        << "the closing quote is part of the pp-tokens and must be reported";
    // (B) char-literal twin: the same span shortfall applies to `'`.
    EXPECT_NE(msgOf("#warning 'q'\n").find("'q'"), std::string::npos);
    // (C) ANGLE delimiter — the THIRD form, and not hypothetical: the word
    // `include` earlier on the line arms the tokenizer's header-context, so the
    // path lexes as HeaderStart + coalesced HeaderPath, which had the same
    // token-less close. Reported `<stdio.h` before the fix; the `>` is now a
    // `HeaderEnd` token and the span join reaches it like any other token.
    EXPECT_NE(msgOf("#warning please include <stdio.h>\n").find("<stdio.h>"),
              std::string::npos)
        << "the header-path close `>` is part of the pp-tokens too";
    // (D) NEGATIVE: the span join must stop exactly at the last non-trivia
    // token. `a > b` ends on `b`, and this `>` is an ordinary operator, not a
    // header-path closer — nothing may append a phantom `>` past the operand.
    // (This used to read "the guard has nothing to extend", naming the
    // `tail[0] == '"'` re-consume in `directiveOperandText`. That guard is
    // DELETED — see the RENEGOTIATED note above. The assertion is unchanged
    // because it always pinned the OBSERVABLE, not the mechanism.)
    {
        std::string const m = msgOf("#warning use a > b\n");
        EXPECT_NE(m.find("use a > b"), std::string::npos) << m;
        EXPECT_EQ(m.find("b>"), std::string::npos)
            << "nothing may append a phantom byte past the operand; got: " << m;
    }
    // (E) NEGATIVE: an UNTERMINATED literal. The stray quote must be reported
    // exactly once, never doubled. NOTE this program is rejected anyway — the
    // tokenizer fails loud with P0010 `EOF inside lexer mode 'string'` — so this
    // pins message shape only, not acceptance. (TWO earlier claims here have now
    // been corrected by measurement: that the stray quote "ends on a StringStart
    // whose own span covers it", and that a "delimiter re-consume" is what must
    // not double it — that re-consume no longer exists. What survives both
    // corrections is the observable: no doubled delimiter byte.)
    {
        std::string const m = msgOf("#warning abc\"\n");
        EXPECT_EQ(m.find("abc\"\""), std::string::npos)
            << "the closing delimiter must appear exactly once; got: " << m;
    }
    // (F) NEGATIVE: the trailing-trivia policy still wins — a trailing comment
    // is one space in phase 3, long before phase-4 directive execution, so it
    // is never part of the pp-tokens.
    {
        std::string const m = msgOf("#warning nope // TODO\n");
        EXPECT_NE(m.find("nope"), std::string::npos) << m;
        EXPECT_EQ(m.find("TODO"), std::string::npos)
            << "a trailing comment is not part of the pp-tokens; got: " << m;
    }
}

// ── ★ CONFIG-DRIVEN: REBIND, not merely strip ───────────────────────────────

// (9) The directive WORD comes from `preprocess.errorDirective`, never a
// hard-coded "error". Rebound off "error" -> "errr": `#error` becomes an
// UNKNOWN directive (P0015) and `#errr` drives the `#error` handler. Follows
// FC179EmbedDirectiveIsConfigDrivenNotHardcoded.
// ★ A strip-only test (asserting `#error` fails loud once the field is empty)
// would be INADEQUATE: an implementation that consults the config solely for
// the `.empty()` guard while MATCHING a hard-coded "error" literal passes it.
// Only the rebind's second half -- `#errr` DRIVING the handler -- excludes that.
// RED-ON-DISABLE: replace `word == cfg().errorDirective` with `word == "error"`
// in `handleDirective`. The rebind is then ignored: `#error` still fires
// P001E (first sub-case reds) and `#errr` reaches the generic fail-loud
// (second sub-case reds).
TEST(Preprocessor, TfC70ErrorDirectiveIsConfigDrivenNotHardcoded) {
    auto schema =
        reboundPreprocessWord("errorDirective", "errr", "<rebound-error>");
    ASSERT_TRUE(schema != nullptr);
    ASSERT_EQ(schema->preprocess().errorDirective, "errr")
        << "the rebound schema must carry the NEW directive word";
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    {
        auto buf = SourceBuffer::fromString("#error x\nint v=1;\n", "main.c");
        auto r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "with the word rebound, `#error` is an unknown directive (P0015)";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
            << "a hard-coded \"error\" literal would still fire P001E here";
    }
    {
        auto buf = SourceBuffer::fromString("#errr x\nint v=1;\n", "main.c");
        auto r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
            << "the rebound `#errr` word now drives the `#error` handler";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported));
    }
}

// (10) The `#warning` twin of (9), with the same reasoning: rebound off
// "warning" -> "warnn", `#warning` is unknown (P0015) and `#warnn` drives the
// warning handler.
// RED-ON-DISABLE: replace `word == cfg().warningDirective` with
// `word == "warning"`.
TEST(Preprocessor, TfC70WarningDirectiveIsConfigDrivenNotHardcoded) {
    auto schema =
        reboundPreprocessWord("warningDirective", "warnn", "<rebound-warning>");
    ASSERT_TRUE(schema != nullptr);
    ASSERT_EQ(schema->preprocess().warningDirective, "warnn");
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    {
        auto buf = SourceBuffer::fromString("#warning x\nint v=1;\n", "main.c");
        auto r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "with the word rebound, `#warning` is an unknown directive";
        EXPECT_FALSE(
            hasPPCode(r, DiagnosticCode::P_PreprocessorWarningDirective))
            << "a hard-coded \"warning\" literal would still fire P001F here";
    }
    {
        auto buf = SourceBuffer::fromString("#warnn x\nint v=1;\n", "main.c");
        auto r = preprocess(buf, schema, noDirs);
        EXPECT_TRUE(
            hasPPCode(r, DiagnosticCode::P_PreprocessorWarningDirective))
            << "the rebound `#warnn` word now drives the `#warning` handler";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported));
    }
}

// ── `#warning` (the half that unblocks sys/cdefs.h) ─────────────────────────

// (11) ★ A reached `#warning` is NON-FATAL: it is reported, carries the user's
// text, and translation CONTINUES (C23 6.10.6) -- `hasErrors()` stays FALSE and
// the program still reaches the parser intact. This is the whole point of the
// directive: `sys/cdefs.h` emits `#warning`s on paths a build must survive.
// RED-ON-DISABLE: drop the explicit `DiagnosticSeverity::Warning` argument from
// the `#warning` `emitPP` call (every other `emitPP` in the file DEFAULTS to
// Error) -- `hasErrors()` flips true and the severity assertion reds.
TEST(Preprocessor, TfC70WarningDirectiveReachedIsNonFatal) {
    PreprocessResult r;
    auto lexs = ppLexemes("#warning deprecated path\nint x=1;\n", r);
    ASSERT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorWarningDirective))
        << "a REACHED `#warning` must be reported";
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "`#warning` must NOT bump errorCount -- translation continues "
           "(C23 6.10.6); a fatal `#warning` breaks every build that uses one";
    auto const* d =
        firstDiagWithCode(r, DiagnosticCode::P_PreprocessorWarningDirective);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->severity, DiagnosticSeverity::Warning);
    EXPECT_NE(d->actual.find("deprecated path"), std::string::npos)
        << "the message must include the author's pp-tokens; got: " << d->actual;
    EXPECT_TRUE(isIntXAssignOne(lexs))
        << "translation continues: the whole program still reaches the parser";
}

// (12) The `#warning` silence twin. Note `hasErrors()` is false either way for
// a warning, so the load-bearing assertions here are the CODE's absence and the
// empty diagnostic list -- not the error flag.
// RED-ON-DISABLE: hoist the `#warning` arm above `if (!stackActive()) return
// end;` (or handle it in the unconditional `#if`-family block) -- the dead
// group's `#warning` then fires and both assertions red.
TEST(Preprocessor, TfC70WarningDirectiveInDeadBranchIsSilent) {
    PreprocessResult r;
    auto lexs =
        ppLexemes("#if 0\n#warning must not fire\n#endif\nint x=1;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorWarningDirective))
        << "a `#warning` in a NOT-TAKEN group is not executed (C 6.10p1)";
    EXPECT_TRUE(r.diagnostics->all().empty())
        << "a dead-branch `#warning` must be completely silent (got "
        << r.diagnostics->all().size() << " diagnostics)";
    EXPECT_TRUE(isIntXAssignOne(lexs));
}

// ── PRE-SCAN PROSE GUARD ────────────────────────────────────────────────────

// (13) The `#error`/`#warning` MESSAGE BODY is prose, never directives. This is
// a real seam, not a hypothetical: `SynthBuilder`'s include pre-scan
// (preprocessor.cpp ~:1136) scans for the intro token with NO `firstOnLine`
// guard -- unlike the authoritative `MacroExpander` loop -- so any `#` EMBEDDED
// in the prose is read as a directive of its own unless the whole line is
// skipped. The consequences are one-sided and LOUD-but-WRONG: a phantom macro
// harvested into `localMacros` flips a later guard, and the include arm then
// eagerly resolves a header the program never asked for (P0016 on a file the
// authoritative pass never looks at).
//
// ★ RED-ON-DISABLE — HONEST STATEMENT, corrected after measurement. The
// pre-scan now carries TWO guards that each independently prevent this shape:
// (1) `sbFirstOnLine` on the hash test (a `#` is a directive intro only when
// FIRST on its line, C 6.10p1), and (2) the unhandled-directive line skip.
// MEASURED: disabling EITHER one alone leaves this test GREEN — they overlap
// here, so this test does NOT pin either individually and it is honest to say
// so rather than claim a red-on-disable it does not have. It reds only when
// BOTH are removed. The guard that IS uniquely pinned by a single test is
// `sbFirstOnLine`, by `TfC70StrayHashInOrdinaryTextIsNotADirective` below —
// a stray `#` in ORDINARY TEXT, which the line skip structurally cannot cover
// because there is no directive word to match. NOTE the skip is deliberately
// stated as the GENERAL invariant
// (any directive this pre-scan does not itself process gets its line skipped,
// D-PP-PRESCAN-UNHANDLED-DIRECTIVE-LINE-SKIP), NOT as an
// `errorDirective`/`warningDirective` special case: an enumerated list left
// `#pragma`/`#line`/`#embed` carrying the identical exposure. So this test
// guards the whole class, and the sub-cases below are merely the two shapes
// that were empirically reproduced. Each sub-case reds:
//   (A) the prose `#define POISON` is harvested (the define arm is live here),
//       `#if defined(POISON)` folds TRUE in the pre-scan only, and
//       `#include "missing.h"` is resolved -> P0016;
//   (B) the prose `#endif` POPS the dead frame in the pre-scan (the conditional
//       arm runs UNCONDITIONALLY, by design, to keep nesting balanced), so the
//       following dead `#include "missing.h"` becomes "confident-live" and is
//       resolved -> P0016;
//   (C) the `#warning` twin of (A) -- pins that the skip covers BOTH words, not
//       just `#error`.
// The skip must NOT be gated on `sbStackActive`: sub-case (B) is a DEAD group.
TEST(Preprocessor, TfC70ErrorMessageBodyIsNotScannedAsDirectives) {
    // (A) LIVE `#error`, prose containing a `#define`.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#error do not #define POISON\n"
                              "#if defined(POISON)\n"
                              "#include \"missing.h\"\n"
                              "#endif\n"
                              "int x=1;\n",
                              r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
            << "non-vacuous: the `#error` itself IS reached and fires";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "the prose `#define POISON` must not be harvested by the "
               "pre-scan -- doing so flips `#if defined(POISON)` and resolves "
               "an include the authoritative pass never executes";
        EXPECT_TRUE(isIntXAssignOne(lexs));
    }
    // (B) DEAD `#error`, prose containing an `#endif` (the conditional arms run
    // unconditionally, so this shape corrupts the frame stack without the skip).
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#if 0\n"
                              "#error remove this #endif marker\n"
                              "#include \"missing.h\"\n"
                              "#endif\n"
                              "int x=1;\n",
                              r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "the prose `#endif` must not pop the pre-scan's conditional "
               "frame -- doing so makes the DEAD include look live and "
               "resolves a header that does not exist";
        EXPECT_FALSE(r.diagnostics->hasErrors());
        EXPECT_TRUE(r.diagnostics->all().empty())
            << "the whole group is dead: nothing at all is reported (got "
            << r.diagnostics->all().size() << ")";
        EXPECT_TRUE(isIntXAssignOne(lexs));
    }
    // (C) LIVE `#warning`, prose containing a `#define` -- the skip covers both
    // directive words. Here the fixed engine reports NOTHING fatal, so the
    // error flag itself is a red-on-disable witness.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#warning do not #define POISON\n"
                              "#if defined(POISON)\n"
                              "#include \"missing.h\"\n"
                              "#endif\n"
                              "int x=1;\n",
                              r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorWarningDirective))
            << "non-vacuous: the `#warning` itself IS reached and reported";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "the `#warning` prose must be skipped by the pre-scan too";
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "a `#warning` alone is non-fatal; a spurious P0016 from the "
               "prose would make this compile fail";
        EXPECT_TRUE(isIntXAssignOne(lexs));
    }
}

// ★ THE `sbFirstOnLine` PIN — the one guard no other test isolates.
// C 6.10p1: a `#` introduces a directive only when it is FIRST on its line.
// The authoritative `MacroExpander` loop always enforced that; the include
// PRE-SCAN did not, so a `#` ANYWHERE — in ordinary program text, not merely in
// a directive payload — was read as a directive of its own. The
// unhandled-directive line skip cannot reach this shape: there is no directive
// word on the line to match, so the skip never fires. That makes this the ONLY
// discriminating witness between the two overlapping guards.
//
// RED-ON-DISABLE (DEMONSTRATED): drop `|| !sbFirstOnLine(i)` from the pre-scan's
// hash test and this test fails — with the line skip still fully in place.
// Before the guard, the prose `#endif` POPPED the pre-scan's conditional frame
// (the conditional arms run UNCONDITIONALLY, by design, to keep nesting
// balanced), so the DEAD `#include` below it looked live and was eagerly
// resolved: a hard `P0016` on a header the authoritative pass never looks at.
TEST(Preprocessor, TfC70StrayHashInOrdinaryTextIsNotADirective) {
    // A bare `#` mid-line in ORDINARY TEXT, inside a dead group.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#if 0\n"
                              "x # endif y\n"
                              "#include \"missing.h\"\n"
                              "#endif\n"
                              "int x=1;\n",
                              r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a `#` that is not first on its line is NOT a directive: the "
               "prose `#endif` must not pop the pre-scan's frame and make the "
               "dead include look live";
        EXPECT_FALSE(r.diagnostics->hasErrors());
        EXPECT_TRUE(isIntXAssignOne(lexs));
    }
    // The same stray `#` in a LIVE region must also stay inert for the
    // pre-scan (it is ordinary text; the parser may reject it later, but the
    // pre-scan must not resolve an include off it).
    {
        PreprocessResult r;
        (void)ppLexemes("#define GUARD 1\n"
                        "a # include \"missing.h\" b\n"
                        "int x=1;\n",
                        r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a mid-line `# include` is ordinary text, not a directive -- "
               "the pre-scan must not resolve it";
    }
}

// The SAME class, for the two OTHER directive words whose payload the pre-scan
// does not interpret: `#pragma` and `#line`
// (D-PP-PRESCAN-UNHANDLED-DIRECTIVE-LINE-SKIP).
//
// WHY THIS TEST EXISTS SEPARATELY FROM THE `#error`/`#warning` ONE ABOVE. The
// first cut of the fix ENUMERATED `errorDirective`/`warningDirective`, which
// was correct-but-incomplete: `#pragma`/`#line`/`#embed` kept the identical
// exposure, and the suite was green over that SUBSET. The fix is now the
// general invariant — "any directive this pre-scan does not itself process
// gets its line skipped" — but an invariant stated only in a comment decays.
// These cases pin the OTHER forms so a future narrowing back to an enumerated
// list cannot pass. `#pragma` is the realistic one: C 6.10.6 makes its payload
// implementation-defined token soup, and the authoritative pass consumes-and-
// drops it, so a bare `#` in a payload is entirely plausible.
//
// ★ RED-ON-DISABLE — CORRECTED AFTER MEASUREMENT, and the correction matters.
// An earlier version of this comment claimed each sub-case reds when the
// unhandled-directive line skip is collapsed to a bare `continue`. That WAS
// true when the skip was the only guard; it is FALSE now. `sbFirstOnLine` was
// added to the pre-scan's hash test afterwards, and every payload `#` here is
// BY DEFINITION not first on its line — so `sbFirstOnLine` alone keeps (A)-(D)
// green and the line skip has NO single-test pin at all. These sub-cases pin
// the PAIR, exactly like their `#error`/`#warning` sibling above; they red only
// when BOTH guards are removed.
//
// The skip is deliberately KEPT despite being behaviourally subsumed: it states
// the intent locally ("this pass does not interpret that payload") and it
// advances `i` past the whole line instead of stepping token-by-token. But it
// is defence-in-depth plus a small win, NOT the load-bearing guard — do not
// claim a red-on-disable for it, and do not let its presence excuse weakening
// `sbFirstOnLine`, which IS uniquely pinned (by
// `TfC70StrayHashInOrdinaryTextIsNotADirective`).
// ★ A first attempt at (C) used `#line 7 "x #define POISON"` and did NOT red —
// the `#define` sat inside a STRING LITERAL and lexes as one token, so the
// pre-scan never saw a `#`. The bare-`#` form below is the one that bites;
// keep it that way or this sub-case silently stops testing anything.
TEST(Preprocessor, TfC70PragmaAndLinePayloadsAreNotScannedAsDirectives) {
    // (A) `#pragma` payload carrying a prose `#define`.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#pragma GCC poison do not #define POISON\n"
                              "#if defined(POISON)\n"
                              "#include \"missing.h\"\n"
                              "#endif\n"
                              "int x=1;\n",
                              r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a `#` inside a `#pragma` payload must not be read as a "
               "directive: harvesting the prose `#define POISON` flips "
               "`#if defined(POISON)` in the PRE-SCAN ONLY and resolves an "
               "include the authoritative pass never executes";
        // ★ TF-C82: assert the diagnostic SET, not `hasErrors()`. `GCC poison`
        // now matches an `unsupported` registry row and is LOUD — a REACHED
        // pragma with real semantics DSS has not built. That is this cycle
        // working, and it is orthogonal to what THIS test is about, so the
        // expectation names both codes instead of collapsing them into a
        // hasErrors() that would silently absorb a future regression.
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "`GCC poison` is an `unsupported` row — reached, it fails loud";
        for (auto const& d : r.diagnostics->all()) {
            EXPECT_EQ(d.code, DiagnosticCode::P_PreprocessorPragma)
                << "the ONLY diagnostic this input may produce is the pragma "
                   "refusal; anything else means the payload was scanned";
        }
        EXPECT_TRUE(isIntXAssignOne(lexs));
    }
    // (B) `#pragma` payload carrying a prose `#endif`, inside a DEAD group that
    // guards an include. Worse than (A): the conditional arms run
    // UNCONDITIONALLY (by design, to keep nesting balanced), so a prose
    // `#endif` POPS the frame and makes the dead include look live.
    {
        PreprocessResult r;
        // ★ BARE prose, NOT `/* ... */`. An earlier version wrapped the stray
        // `#endif` in a block comment and was INERT: comment bodies are
        // trivia-kind tokens, so the pre-scan never sees the `#` at all and the
        // sub-case passed with or without the guard. Exactly the trap this
        // file documents for the string-literal attempt a few tests below —
        // keep the payload bare or this stops testing anything.
        auto lexs = ppLexemes("#if 0\n"
                              "#pragma once stray #endif marker\n"
                              "#include \"missing.h\"\n"
                              "#endif\n"
                              "int x=1;\n",
                              r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "the prose `#endif` must not pop the pre-scan's conditional "
               "frame";
        EXPECT_TRUE(r.diagnostics->all().empty())
            << "the whole group is dead: nothing at all is reported (got "
            << r.diagnostics->all().size() << ")";
        EXPECT_TRUE(isIntXAssignOne(lexs));
    }
    // (C) `#line` payload carrying a prose `#define`. Malformed as C, but the
    // pre-scan must be robust to it — it runs BEFORE any directive is
    // validated, so it cannot assume well-formedness.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#line 7 #define POISON\n"
                              "#if defined(POISON)\n"
                              "#include \"missing.h\"\n"
                              "#endif\n"
                              "int x=1;\n",
                              r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "a `#` inside a `#line` payload must not be read as a directive";
        EXPECT_TRUE(isIntXAssignOne(lexs));
    }
    // (D) `#line` payload carrying a prose `#endif`, in a dead group.
    {
        PreprocessResult r;
        auto lexs = ppLexemes("#if 0\n"
                              "#line 7 #endif\n"
                              "#include \"missing.h\"\n"
                              "#endif\n"
                              "int x=1;\n",
                              r);
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "the prose `#endif` in a `#line` payload must not pop the "
               "pre-scan's conditional frame";
        EXPECT_TRUE(isIntXAssignOne(lexs));
    }
}


// ═════════════════════════════════════════════════════════════════════════════
// TF-C74 — PER-ARCHITECTURE identity predefined macros from the TARGET config.
//
// Predefines now come from TWO config families: the LANGUAGE
// (`preprocess.predefinedMacros`) and the TARGET (`predefinedMacros` in
// `<arch>.target.json`). They are merged ONCE, at `preprocess()` entry, by
// `mergePredefinedMacros` — which is also the ONE place the per-format
// availability filter now runs. All FOUR predefine seed sites then iterate that
// single effective list, so the include-gating pre-scan and the authoritative
// MacroExpander can no longer disagree (a divergence there is a silent P0016
// seam: the pre-scan resolving a gated `#include` the real pass reads dead).
//
// ★ The four seed sites are pinned INDIVIDUALLY below. A naive implementation
// wires only the MacroExpander (#2), and a test suite that only checked
// `#if defined(X)` would pass on it.
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// Build a target-side predefine row without going through a .target.json — so
// these tests pin the ENGINE contract, not the shipped config's current
// contents (which `test_target_schema.cpp` pins separately).
[[nodiscard]] PredefinedMacroDef targetMacro(
    std::string name, std::string value,
    std::vector<std::string> formats = {}) {
    PredefinedMacroDef pm;
    pm.name                   = std::move(name);
    pm.kind                   = PredefinedMacroKind::Constant;
    pm.value                  = std::move(value);
    pm.availableObjectFormats = std::move(formats);
    return pm;
}

// `ppLexemes`, but with an active object format + a TARGET predefine list.
[[nodiscard]] std::vector<std::string> ppLexemesForTarget(
    std::string text, std::optional<ObjectFormatKind> fmt,
    std::span<PredefinedMacroDef const> targetMacros, PreprocessResult& out,
    std::span<std::filesystem::path const> includeDirs = {}) {
    auto schema = cSubset();
    auto buf    = SourceBuffer::fromString(std::move(text), "main.c");
    std::vector<std::string> noDefines;
    out = preprocess(buf, schema, includeDirs, {}, fmt, noDefines, targetMacros);
    std::vector<std::string> lexs;
    for (Token const& t : out.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{out.synthBuffer->slice(t.span)});
    }
    return lexs;
}

}  // namespace

// ── SEED SITE #2: the authoritative MacroExpander `predefined_` map ───────
// Ordinary expansion of a target predefine in normal code. This is the site a
// naive implementation wires FIRST (and often only).
// RED-ON-DISABLE: revert the `merged.effective` loop in the MacroExpander ctor
// to `cfg().predefinedMacros` and the token comes back as the identifier.
TEST(Preprocessor, TFC74TargetPredefineExpandsSeedSiteExpander) {
    std::vector<PredefinedMacroDef> tms{targetMacro("__ARCHPROBE__", "7")};
    PreprocessResult r;
    auto lexs = ppLexemesForTarget("int x = __ARCHPROBE__;\n",
                                   ObjectFormatKind::Elf, tms, r);
    // EXACT surviving token stream, not a count: `7` must have REPLACED the
    // identifier, not been appended alongside it.
    EXPECT_EQ(lexs, (std::vector<std::string>{"int", "x", "=", "7", ";"}));
}

// ── SEED SITE #1: the include-gating pre-scan's definedness oracle ────────
// A `#ifdef`-gated QUOTE-include whose gate is a FUNCTION-LIKE target
// predefine. The pre-scan decides whether to SPLICE the header long before the
// MacroExpander runs, so it needs its own view of the predefines
// (`SynthBuilder::sbNameDefined`).
//
// ★ The gate MUST be function-like to ISOLATE this seed site. MEASURED while
// verifying red-on-disable: with an OBJECT-like gate, reverting `sbNameDefined`
// alone leaves the test GREEN — the pre-scan VALUE prefix (seed site #4) also
// materializes object-like predefines into `localMacros`, which `sbNameDefined`
// consults first, so the two sites mask each other. Function-like predefines
// are EXCLUDED from the value prefix (FINDING-A: value-seeding a call macro
// would make a bare `#if NAME` fold more-live in the pre-scan than in the
// authoritative pass — a P0016 re-open), so ONLY the predefined arm of
// `sbNameDefined` can report this one DEFINED.
//
// RED-ON-DISABLE (VERIFIED): revert `sbNameDefined`'s loop to
// `schema->preprocess().predefinedMacros` — the header is not spliced and
// `MARKER_FROM_HEADER` never appears.
TEST(Preprocessor, TFC74TargetPredefineGatesQuoteIncludeSeedSitePreScan) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_tfc74_prescan_ifdef";
    fs::create_directories(dir);
    {
        std::ofstream h(dir / "arch_gated.h");
        h << "int MARKER_FROM_HEADER = 1;\n";
    }
    std::vector<fs::path> const dirs{dir};

    PredefinedMacroDef fn;
    fn.name           = "__ARCHFNPROBE__";
    fn.kind           = PredefinedMacroKind::Constant;
    fn.value          = "";
    fn.params         = {"x"};
    fn.isFunctionLike = true;
    std::vector<PredefinedMacroDef> tms{fn};

    static constexpr char const* kSrc = "#ifdef __ARCHFNPROBE__\n"
                                        "#include \"arch_gated.h\"\n"
                                        "#endif\n";
    PreprocessResult r;
    auto lexs = ppLexemesForTarget(kSrc, ObjectFormatKind::Elf, tms, r, dirs);
    EXPECT_NE(std::find(lexs.begin(), lexs.end(), "MARKER_FROM_HEADER"),
              lexs.end())
        << "an `#ifdef`-gated quote-#include must SPLICE when the gate is a "
           "FUNCTION-LIKE TARGET predefine — the pre-scan's definedness oracle "
           "is a seed site the value prefix cannot cover";

    // Mirror: with NO target list the same source must NOT splice — proving the
    // splice above came from the target predefine and not from something else.
    PreprocessResult r2;
    auto lexs2 = ppLexemesForTarget(kSrc, ObjectFormatKind::Elf, {}, r2, dirs);
    EXPECT_EQ(std::find(lexs2.begin(), lexs2.end(), "MARKER_FROM_HEADER"),
              lexs2.end());
    fs::remove_all(dir);
}

// ── SEED SITE #4: the pre-scan's VALUE prefix ─────────────────────────────
// A `#if <macro> == N`-gated QUOTE-include. Definedness is not enough here: the
// pre-scan must know the macro's VALUE. A `#ifdef`-only test would pass with
// site #4 unwired (the value would fold to 0 and the include would be skipped).
// RED-ON-DISABLE: revert the `preScanDefinePrefix` loop to
// `schema->preprocess().predefinedMacros` — the gate folds 0 and the header is
// silently dropped.
TEST(Preprocessor, TFC74TargetPredefineValueGatesIncludeSeedSitePreScanValue) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "dss_tfc74_prescan_value";
    fs::create_directories(dir);
    {
        std::ofstream h(dir / "value_gated.h");
        h << "int MARKER_VALUE_GATED = 1;\n";
    }
    std::vector<fs::path> const dirs{dir};
    std::vector<PredefinedMacroDef> tms{targetMacro("__ARCHPROBE__", "7")};

    // TRUE arm: value 7 == 7 ⇒ splice.
    PreprocessResult r;
    auto lexs = ppLexemesForTarget("#if __ARCHPROBE__ == 7\n"
                                   "#include \"value_gated.h\"\n"
                                   "#endif\n",
                                   ObjectFormatKind::Elf, tms, r, dirs);
    EXPECT_NE(std::find(lexs.begin(), lexs.end(), "MARKER_VALUE_GATED"),
              lexs.end())
        << "a VALUE-gated quote-#include must see the target predefine's VALUE, "
           "not merely its definedness";

    // FALSE arm: same macro, wrong value ⇒ no splice. This is what proves the
    // value actually arrived (a definedness-only seed would make BOTH arms
    // behave the same way).
    PreprocessResult r2;
    auto lexs2 = ppLexemesForTarget("#if __ARCHPROBE__ == 8\n"
                                    "#include \"value_gated.h\"\n"
                                    "#endif\n",
                                    ObjectFormatKind::Elf, tms, r2, dirs);
    EXPECT_EQ(std::find(lexs2.begin(), lexs2.end(), "MARKER_VALUE_GATED"),
              lexs2.end());
    fs::remove_all(dir);
}

// ── SEED SITE #3: the "<built-in>" prologue (FUNCTION-LIKE predefines) ────
// A function-like target predefine is NOT seeded into `predefined_`; it lowers
// to a `#define name(params) value` line in the synthetic "<built-in>"
// prologue. RED-ON-DISABLE: revert the builtin-prologue loop to
// `schema->preprocess().predefinedMacros` and `__ARCHATTR__(x)` survives
// unexpanded, breaking the exact token comparison.
TEST(Preprocessor, TFC74FunctionLikeTargetPredefineSeedSiteBuiltinPrologue) {
    PredefinedMacroDef fn;
    fn.name           = "__ARCHATTR__";
    fn.kind           = PredefinedMacroKind::Constant;
    fn.value          = "";              // erase the call entirely
    fn.params         = {"x"};
    fn.isFunctionLike = true;
    std::vector<PredefinedMacroDef> tms{fn};

    PreprocessResult r;
    auto lexs = ppLexemesForTarget("__ARCHATTR__(unused) int x = 1;\n",
                                   ObjectFormatKind::Elf, tms, r);
    EXPECT_EQ(lexs, (std::vector<std::string>{"int", "x", "=", "1", ";"}))
        << "a FUNCTION-LIKE target predefine must reach the \"<built-in>\" "
           "prologue and erase its call";
}

// ── the per-format filter, on TARGET entries, in BOTH directions ──────────
// This is what keeps the Apple-only `__arm64__` spelling off the ELF leg.
TEST(Preprocessor, TFC74TargetPredefineHonoursAvailableObjectFormats) {
    std::vector<PredefinedMacroDef> tms{
        targetMacro("__APPLEONLYPROBE__", "1", {"macho"})};

    // macho ⇒ DEFINED and expanded.
    {
        PreprocessResult r;
        auto lexs = ppLexemesForTarget("int x = __APPLEONLYPROBE__;\n",
                                       ObjectFormatKind::MachO, tms, r);
        EXPECT_EQ(lexs, (std::vector<std::string>{"int", "x", "=", "1", ";"}));
    }
    // elf ⇒ UNDEFINED: the identifier survives verbatim (leaking the Apple
    // spelling onto ELF is the exact defect the gate exists to prevent).
    {
        PreprocessResult r;
        auto lexs = ppLexemesForTarget("int x = __APPLEONLYPROBE__;\n",
                                       ObjectFormatKind::Elf, tms, r);
        EXPECT_EQ(lexs, (std::vector<std::string>{"int", "x", "=",
                                                  "__APPLEONLYPROBE__", ";"}));
    }
    // nullopt (no target selected) ⇒ UNDEFINED: a format-restricted macro is
    // meaningless without a format.
    {
        PreprocessResult r;
        auto lexs = ppLexemesForTarget("int x = __APPLEONLYPROBE__;\n",
                                       std::nullopt, tms, r);
        EXPECT_EQ(lexs, (std::vector<std::string>{"int", "x", "=",
                                                  "__APPLEONLYPROBE__", ";"}));
    }
}

// ── the COLLISION policy ──────────────────────────────────────────────────
// A name owned by BOTH config families is FATAL. Neither may silently win:
// picking either quietly is a wrong-value miscompile with no diagnostic.
TEST(Preprocessor, TFC74CollidingPredefineFailsLoudNamingBothPaths) {
    auto schema = cSubset();
    // `__LINE__` is declared by the shipped c-subset language config.
    std::vector<PredefinedMacroDef> tms{targetMacro("__LINE__", "1")};
    auto buf = SourceBuffer::fromString("int x = 1;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string>           noDefines;
    PreprocessResult r = preprocess(buf, schema, noDirs, {},
                                    ObjectFormatKind::Elf, noDefines, tms);

    EXPECT_TRUE(r.fatal)
        << "a language/target predefine collision must abort the pass, not "
           "silently resolve to one side";
    ASSERT_TRUE(hasPPCode(r, DiagnosticCode::C_ConflictingPredefinedMacro));

    // The message must name BOTH declaring config paths — a diagnostic that
    // says only "conflict" leaves the maintainer to guess which file to edit.
    bool named = false;
    for (auto const& d : r.diagnostics->all()) {
        if (d.code != DiagnosticCode::C_ConflictingPredefinedMacro) continue;
        named = d.actual.find("/preprocess/predefinedMacros") != std::string::npos
             && d.actual.find("/predefinedMacros") != std::string::npos
             && d.actual.find("__LINE__") != std::string::npos;
        if (named) break;
    }
    EXPECT_TRUE(named)
        << "the collision message must name the macro AND both declaring "
           "config paths";
}

// ★ The collision scan runs BEFORE the format filter. `_WIN32` is declared
// pe-GATED by the shipped c-subset language config, so on an ELF target the
// language entry is filtered OUT — yet an ungated TARGET `_WIN32` must STILL
// collide. Otherwise a maintainer could ship the conflict and only ever see it
// on the one leg where both entries survive the filter.
// RED-ON-DISABLE: move the collision scan in `mergePredefinedMacros` below the
// filter loops and this test goes green-but-wrong.
TEST(Preprocessor, TFC74CollisionDetectedBeforeFormatFilter) {
    auto schema = cSubset();
    std::vector<PredefinedMacroDef> tms{targetMacro("_WIN32", "1")};
    auto buf = SourceBuffer::fromString("int x = 1;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string>           noDefines;
    // ELF: the language's pe-gated `_WIN32` would NOT survive the filter.
    PreprocessResult r = preprocess(buf, schema, noDirs, {},
                                    ObjectFormatKind::Elf, noDefines, tms);
    EXPECT_TRUE(r.fatal);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::C_ConflictingPredefinedMacro))
        << "a pe-GATED language `_WIN32` must still collide with an UNGATED "
           "target `_WIN32` on an ELF build — gating decides which formats SEE "
           "a macro, not who OWNS the name";
}

// ── the NO-REGRESSION invariant: empty target span == legacy ──────────────
// An empty target list must produce a token stream BYTE-IDENTICAL to the
// pre-TF-C74 engine, on every format. This is the guarantee that lets the
// feature ship without re-verifying every existing preprocessor test.
TEST(Preprocessor, TFC74EmptyTargetSpanIsByteIdenticalToLegacy) {
    // Source that touches predefines the shipped language config gates
    // differently per format (`_WIN32` is pe-only, `__APPLE__` macho-only) plus
    // an ungated one, so a filter regression on ANY leg would show up.
    static constexpr char const* kSrc =
        "#ifdef _WIN32\nint w = 1;\n#endif\n"
        "#ifdef __APPLE__\nint a = 1;\n#endif\n"
        "long v = __STDC_VERSION__;\n";

    for (std::optional<ObjectFormatKind> fmt :
         {std::optional<ObjectFormatKind>{ObjectFormatKind::Elf},
          std::optional<ObjectFormatKind>{ObjectFormatKind::MachO},
          std::optional<ObjectFormatKind>{ObjectFormatKind::Pe},
          std::optional<ObjectFormatKind>{}}) {
        auto schema = cSubset();
        std::vector<std::filesystem::path> noDirs;
        std::vector<std::string>           noDefines;

        // LEGACY shape: the 6-arg overload, exactly as every pre-TF-C74 caller
        // spells it (the target-predefine parameter defaulted away).
        auto legacyBuf = SourceBuffer::fromString(kSrc, "main.c");
        PreprocessResult legacy =
            preprocess(legacyBuf, schema, noDirs, {}, fmt, noDefines);

        // NEW shape: explicitly empty target span.
        auto newBuf = SourceBuffer::fromString(kSrc, "main.c");
        std::vector<PredefinedMacroDef> none;
        PreprocessResult withEmpty =
            preprocess(newBuf, schema, noDirs, {}, fmt, noDefines, none);

        ASSERT_FALSE(legacy.fatal);
        ASSERT_FALSE(withEmpty.fatal);
        // Compare the SYNTH TEXT byte-for-byte, not just the token count — the
        // prologues and the pre-scan value prefix are text, and a regression
        // there would be invisible to a token-count comparison.
        EXPECT_EQ(legacy.synthBuffer->text(), withEmpty.synthBuffer->text())
            << "an empty target span must leave the synthesized text "
               "byte-identical to the legacy call shape";
        ASSERT_EQ(legacy.tokens.size(), withEmpty.tokens.size());
        for (std::size_t i = 0; i < legacy.tokens.size(); ++i) {
            EXPECT_EQ(legacy.tokens[i].span, withEmpty.tokens[i].span);
            EXPECT_EQ(legacy.tokens[i].coreKind, withEmpty.tokens[i].coreKind);
        }
    }
}

// ── `mergePredefinedMacros` unit contract ────────────────────────────────
// Order is language-first-then-target, and stable within each side. The seed
// sites depend on this: the "<built-in>" prologue and the pre-scan value prefix
// are `#define` STREAMS, so order is observable behaviour, not an accident.
TEST(Preprocessor, TFC74MergeOrderIsLanguageThenTargetStable) {
    std::vector<PredefinedMacroDef> lang{targetMacro("L1", "1"),
                                         targetMacro("L2", "2")};
    std::vector<PredefinedMacroDef> tgt{targetMacro("T1", "3"),
                                        targetMacro("T2", "4")};
    auto merged = mergePredefinedMacros(lang, tgt, ObjectFormatKind::Elf);
    ASSERT_TRUE(merged.conflicts.empty());
    std::vector<std::string> names;
    for (auto const& pm : merged.effective) names.push_back(pm.name);
    EXPECT_EQ(names, (std::vector<std::string>{"L1", "L2", "T1", "T2"}));
}

// The filter is applied ONCE, here — so `effective` contains ONLY entries
// available on the active format, from BOTH sides.
TEST(Preprocessor, TFC74MergeAppliesFormatFilterOnceToBothSides) {
    std::vector<PredefinedMacroDef> lang{targetMacro("LPE", "1", {"pe"}),
                                         targetMacro("LANY", "2")};
    std::vector<PredefinedMacroDef> tgt{targetMacro("TMACHO", "3", {"macho"}),
                                        targetMacro("TANY", "4")};
    auto merged = mergePredefinedMacros(lang, tgt, ObjectFormatKind::MachO);
    ASSERT_TRUE(merged.conflicts.empty());
    std::vector<std::string> names;
    for (auto const& pm : merged.effective) names.push_back(pm.name);
    EXPECT_EQ(names, (std::vector<std::string>{"LANY", "TMACHO", "TANY"}))
        << "the pe-gated language entry must be filtered out and the "
           "macho-gated target entry kept — one filter, both sides";
}

// A conflict leaves `effective` EMPTY — there is no partially-merged state a
// caller could mistake for usable.
TEST(Preprocessor, TFC74MergeConflictYieldsNoUsableList) {
    std::vector<PredefinedMacroDef> lang{targetMacro("DUP", "1")};
    std::vector<PredefinedMacroDef> tgt{targetMacro("DUP", "2")};
    auto merged = mergePredefinedMacros(lang, tgt, ObjectFormatKind::Elf);
    EXPECT_EQ(merged.conflicts.size(), 1u);
    EXPECT_TRUE(merged.effective.empty());
}

// ── shipped-config sibling pins: language ⊕ arm64 and language ⊕ x86_64 ───
// The EFFECTIVE arch-identity set a real macho/elf build sees. EXACT SETS, not
// counts — the whole point of the cycle is which SPELLINGS reach the source.
TEST(Preprocessor, TFC74EffectiveArchPredefinesForShippedTargets) {
    auto c = GrammarSchema::loadShipped("c-subset");
    ASSERT_TRUE(c.has_value());
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm.has_value());
    auto x86 = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value());

    auto namesOfTargetHalf = [](MergedPredefinedMacros const& m,
                                std::size_t langCount) {
        std::vector<std::string> out;
        for (std::size_t i = langCount; i < m.effective.size(); ++i) {
            out.push_back(m.effective[i].name);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto langSurviving = [&](std::optional<ObjectFormatKind> fmt) {
        return mergePredefinedMacros((*c)->preprocess().predefinedMacros, {}, fmt)
            .effective.size();
    };

    // arm64 on MACHO: all four spellings, including the Apple-only pair.
    {
        auto m = mergePredefinedMacros((*c)->preprocess().predefinedMacros,
                                       (*arm)->predefinedMacros(),
                                       ObjectFormatKind::MachO);
        ASSERT_TRUE(m.conflicts.empty())
            << "the shipped language and arm64 configs must not collide";
        EXPECT_EQ(namesOfTargetHalf(m, langSurviving(ObjectFormatKind::MachO)),
                  (std::vector<std::string>{"__ARM_ARCH_ISA_A64", "__aarch64__",
                                            "__arm64", "__arm64__"}));
    }
    // arm64 on ELF: the Apple-only pair is GONE, and `__CHAR_UNSIGNED__`
    // APPEARS — the two gates point in OPPOSITE directions on the same target,
    // which is the whole reason the gate is per-entry.
    //
    // ★ `__CHAR_UNSIGNED__` is not an identity spelling: it is the
    // PREPROCESSOR-VISIBLE face of the target's `charIsUnsigned` key
    // (D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM), whose `default` is `true` and
    // whose macho/pe overrides are `false`. So it must be defined on exactly
    // the leg where the default is the effective answer. MEASURED 2026-07-28
    // with `/usr/bin/clang -dM -E -x c /dev/null -target <triple>` (Apple clang
    // 21.0.0): DEFINED for aarch64-linux-gnu; NOT defined for
    // arm64-apple-darwin, x86_64-unknown-linux-gnu or x86_64-pc-windows-msvc.
    {
        auto m = mergePredefinedMacros((*c)->preprocess().predefinedMacros,
                                       (*arm)->predefinedMacros(),
                                       ObjectFormatKind::Elf);
        ASSERT_TRUE(m.conflicts.empty());
        EXPECT_EQ(namesOfTargetHalf(m, langSurviving(ObjectFormatKind::Elf)),
                  (std::vector<std::string>{"__ARM_ARCH_ISA_A64",
                                            "__CHAR_UNSIGNED__",
                                            "__aarch64__"}))
            << "`__arm64__`/`__arm64` are Apple-only and must NOT leak onto "
               "ELF, while `__CHAR_UNSIGNED__` is ELF-only and MUST appear "
               "there — it is the preprocessor face of the target's "
               "`charIsUnsigned` default, which macho/pe override to signed";
    }
    // x86_64: the same four spellings on every format.
    for (ObjectFormatKind fmt : {ObjectFormatKind::Elf, ObjectFormatKind::MachO,
                                 ObjectFormatKind::Pe}) {
        auto m = mergePredefinedMacros((*c)->preprocess().predefinedMacros,
                                       (*x86)->predefinedMacros(), fmt);
        ASSERT_TRUE(m.conflicts.empty())
            << "the shipped language and x86_64 configs must not collide";
        EXPECT_EQ(namesOfTargetHalf(m, langSurviving(fmt)),
                  (std::vector<std::string>{"__amd64", "__amd64__", "__x86_64",
                                            "__x86_64__"}));
    }
}
