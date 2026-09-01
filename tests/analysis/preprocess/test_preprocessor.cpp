// FC13 cycle 1 unit tests for the config-selected C preprocessor
// (src/analysis/preprocess/preprocessor.{hpp,cpp}). These exercise the engine
// DIRECTLY (build a SourceBuffer + the shipped c schema, call
// preprocess, inspect the resulting token stream) so each guard is pinned in
// isolation. Every assertion is the STRONGEST provable property and is
// RED-ON-DISABLE (reverting the backing impl line fails the test).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/char_decode.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"   // c105: per-format prologue tests
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/target_schema.hpp"   // TF-C74: per-arch target predefines
#include "core/types/unsuppressable_codes.hpp"  // TF-C86: the refusal's closed-table pin
#include "link/object_format_schema.hpp"  // TF-C97: per-format data-model predefines
#include "tokenizer/tokenizer.hpp"
#include "test_support/golden_file.hpp"   // TF-C85: findCorpusRoot / readFile
#include "test_support/repo_root.hpp"     // the ONE repo/config-root resolver
#include "test_support/scratch_dir.hpp"   // the per-run scratch root (see ppScratchRoot)

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>   // TF-C91: the cSubset() return-shape static_assert
#include <vector>

namespace {

using namespace dss;

// ── THE scratch-path chokepoint ────────────────────────────────────────────
// Every filesystem fixture in this file hangs its directory off THIS root.
// Nothing below may name `temp_directory_path()` directly — that is the whole
// point of routing 39 sites through one function.
//
// WHY THE ROOT MUST BE PER-RUN (D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES).
// `tests/CMakeLists.txt` registers a SECOND ctest entry that runs THIS
// binary again under `--gtest_shuffle --gtest_repeat=20`, deliberately without
// serialization. So on any ordinary `ctest -j` two live processes of this
// binary overlap — and while the fixtures derived their directories from
// CONSTANT `/tmp/dss_pp_*` names they addressed the same bytes: one process's
// `remove_all` deleted a header the other had just written and was about to
// read. MEASURED pre-fix on this host: `ctest -j 8` over the two entries went
// red 1 run in 3 with `SourceBuffer::fromFile: cannot open
// .../dss_tf87_depthcap/rec.h`, and two concurrent bare processes BOTH went red
// with DIFFERENT counts (20 vs 18 distinct tests) — the tell that this is
// contention and not a real regression.
//
// The paths must be UNIQUE, not contended-for. Serializing the two entries
// (`RUN_SERIAL` / `RESOURCE_LOCK`) would bury the defect and slow every gate
// run, and dropping the shuffle arm would give up the order-dependence
// coverage it exists for. Neither is a fix; do not "simplify" back to either.
//
// `ScratchDir` (tests/test_support/scratch_dir.hpp) already carries the
// correct scheme, so this REUSES it rather than growing a second one: a PID
// SEED plus an atomic claim via `create_directory` (SINGULAR) in a loop. The
// singular form returns true only for the caller that actually created the
// directory — check-and-create is atomic in the OS — so the claim is race-free
// against a concurrent sibling AND steps over a stale directory left by a
// killed run instead of silently sharing it. A PID alone would not do: PIDs
// recycle.
//
// ONE root per process, not one per fixture, and that is load-bearing twice
// over: repeated calls with the SAME tag must resolve to the SAME directory
// (`writeEmbedResource` is called twice with "dss_embed_t10" to sit two
// resources side by side), and the static's destructor sweeps the whole tree at
// exit, so a fixture whose trailing `remove_all` is skipped by an early
// `ASSERT_*` return no longer leaks a directory under the temp base.
[[nodiscard]] std::filesystem::path const& ppScratchRoot() {
    static test_support::ScratchDir const root{test_support::Location::Temp,
                                               "preprocess"};
    return root.path();
}

// Shared schema fixture: load once per test binary process, and hand back a
// REFERENCE to the cached owner. Same shape as `x86Schema()` in
// tests/lir/test_lir.cpp.
//
// D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE — WHY THIS RETURNS A REFERENCE.
// While this returned `std::shared_ptr<GrammarSchema const>` BY VALUE, every
// call built a fresh schema owned solely by the returned temporary, so the
// one-liner `auto const& x = cSubset()->accessor();` bound a reference into an
// object destroyed at the end of that full-expression. `GrammarSchema`'s
// accessors (`preprocess()`, src/core/types/grammar_schema.hpp) return
// references INTO the schema, so that is a heap-use-after-free — MEASURED with
// ASan as a 40/40 deterministic `heap-use-after-free`, and reported from
// Windows/g++/libstdc++ as a non-deterministic 0xC0000005 that a full-suite run
// could not see. Returning a reference to a function-local static makes the
// pointee outlive every expression, so NO accessor on this helper can dangle at
// any call site, present or future: the defect class is unrepresentable here
// rather than merely absent.
//
// Safe to cache (MEASURED over this file, not assumed): the pointee is
// `GrammarSchema const` so mutation is ill-formed; no test asserts on pointer
// identity, `use_count()`, or freshness; nothing here writes to
// `src/dss-config/`, so the shipped config cannot change mid-process; and
// `reboundC()` — the one helper that DOES need a per-call variant — builds
// its own schema via `loadFromText` and never calls this function.
//
// The 34 `auto schema = cSubset();` call sites are unaffected: `auto` deduces
// `shared_ptr` BY VALUE from a `const&`, i.e. a refcount bump.
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSubset() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            ADD_FAILURE() << "loadShipped(c) failed";
            std::abort();
        }
        return *loaded;
    }();
    return schema;
}

// D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE — the DURABLE guard, and the only
// kind available here. Once `cSubset()` hands back a reference to a static the
// dangling read becomes IMPOSSIBLE, which retires the crash test that proved it:
// a runtime red-on-disable cannot survive its own fix. So the property is pinned
// at COMPILE time instead. Restoring the by-value return re-admits the entire
// defect class at all 36 call sites, so that regression must not be silent.
// RED-ON-DISABLE (MEASURED): change the return type back to
// `std::shared_ptr<GrammarSchema const>` and this fails to compile.
static_assert(std::is_reference_v<decltype(cSubset())>,
              "cSubset() must return a REFERENCE to a cached owner. A by-value "
              "return re-admits D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE: "
              "`helper()->accessor()` would again bind a reference into a schema "
              "owned only by the temporary, which dies at the end of the "
              "full-expression (heap-use-after-free).");

// Run the preprocessor over `text` (no include dirs) and return the NON-trivia
// token lexemes (sliced from the synth buffer), in order. Directives removed +
// macros expanded, so this is exactly what the parser would see.
[[nodiscard]] std::vector<std::string> ppLexemes(std::string text,
                                                 PreprocessResult& out) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(std::move(text), "main.c");
    std::vector<std::filesystem::path> noDirs;
    out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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

// The SEVERITY a code was reported at, or nullopt when it was not reported at
// all. `hasPPCode` alone cannot tell an Error from a Warning, so a pin written
// with it stays green across exactly the change that matters when a diagnostic's
// severity is the SUBJECT of the fix (D-PP-INCOMPATIBLE-REDEFINITION-IS-FATAL:
// the old pins here asserted the code's presence and would have survived the
// severity flip untouched, proving nothing).
[[nodiscard]] std::optional<DiagnosticSeverity> ppCodeSeverity(
    PreprocessResult const& r, DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == code) return d.severity;
    }
    return std::nullopt;
}

// Read the shipped c config TEXT so a test can REBIND a single config
// field and reload, proving the engine reads that field from config rather than
// hard-coding a lexeme. Returns "" if not found — the ~14 callers each already
// assert non-empty, so that contract is UNCHANGED; what is new is that the
// REASON is reported here instead of being guessed at fourteen call sites.
//
// The directory comes from the ONE test-side resolver (`repo_root.hpp`:
// $DSS_CONFIG_ROOT → the repo root CMake bakes in → a cwd ancestor walk). It
// used to be a private 8-hop cwd walk, duplicated verbatim inside
// FunctionLikeOpenTokenIsConfigDrivenNotHardcoded (which now calls this one):
// out of tree the cwd has no `src/dss-config` in its ancestry, so BOTH copies
// missed and every rebind test in this file went red at once, for a reason none
// of them named.
[[nodiscard]] std::string loadShippedCText() {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    std::filesystem::path const cand =
        *root / "sources" / "c.lang.json";
    std::ifstream in(cand, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read the shipped c config: "
                      << cand.string();
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
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

// D-PP-INCOMPATIBLE-REDEFINITION-IS-FATAL. C 6.10.3p2 makes this a constraint
// violation, so a diagnostic is REQUIRED — but ✔MEASURED on the compilers this
// language declares itself to be, one TU per shape: every divergence shape warns
// at rc=0 and the NEW definition takes effect. This pin asserts all three facts
// (reported / not fatal / new value), because any one of them alone is satisfied
// by a wrong implementation: presence alone survived the very defect this closes.
TEST(Preprocessor, IncompatibleRedefinitionWarnsAndTakesTheNewDefinition) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define X 1\n#define X 2\nint v = X;\n", r);
    EXPECT_EQ(ppCodeSeverity(r, DiagnosticCode::P_PreprocessorMacroRedefinition),
              DiagnosticSeverity::Warning)
        << "reported, and as a WARNING — the references do not make this fatal";
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "translation CONTINUES: a warning must never bump the error count";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "2")
        << "the SECOND definition wins, as on every reference compiler; keeping "
           "the first would agree about the diagnostic and disagree about the "
           "PROGRAM, which is strictly worse than failing loud";
}

// The four other shapes `sameDefinition` can report. Measured INDIVIDUALLY: the
// parameter-spelling case is the one that bit a real consumer (sqlite shell.c
// defines S_ISLNK(mode) before including <sys/stat.h>), but generalizing from it
// would have been a guess wearing a measurement's clothes.
TEST(Preprocessor, EveryIncompatibleRedefinitionShapeWarnsAndTakesTheNew) {
    struct Shape {
        char const* name;
        char const* src;
        char const* expect;  // lexeme the expansion must produce
    };
    // Each program ends `int v = <use>;` so lexs[3] is the expanded value.
    Shape const shapes[] = {
        {"parameter spelling",
         "#define M(mode) 1\n#define M(m) 2\nint v = M(0);\n", "2"},
        {"arity",
         "#define M(a) 1\n#define M(a,b) 2\nint v = M(0,0);\n", "2"},
        {"object vs function-like",
         "#define M 1\n#define M(a) 2\nint v = M(0);\n", "2"},
        {"variadic vs not",
         "#define M(a,...) 1\n#define M(a) 2\nint v = M(0);\n", "2"},
    };
    for (Shape const& s : shapes) {
        PreprocessResult r;
        auto lexs = ppLexemes(s.src, r);
        EXPECT_EQ(ppCodeSeverity(r, DiagnosticCode::P_PreprocessorMacroRedefinition),
                  DiagnosticSeverity::Warning)
            << s.name << ": must be reported, and as a warning";
        EXPECT_FALSE(r.diagnostics->hasErrors()) << s.name << ": must not be fatal";
        ASSERT_EQ(lexs.size(), 5u) << s.name;
        EXPECT_EQ(lexs[3], s.expect) << s.name << ": the new definition must win";
    }
}

// D-PP-PREDEFINE-REDEFINITION-PARTITION — THIS TEST USED TO ASSERT THE
// OPPOSITE, AND THE REVERSAL IS THE POINT.
//
// It previously pinned "an ordinary redefinition is a Warning, while redefining
// a CONFIG PREDEFINED macro stays a hard Error", on the reading that C
// 6.10.10.1p2 is a CONSTRAINT. ✔RE-MEASURED against N3220: 6.10.10 carries NO
// `Constraints` heading (6.10.5 Macro replacement DOES), so C23 4p2 —
// "If a 'shall' or 'shall not' requirement that appears outside of a constraint
// or runtime-constraint is violated, the behavior is undefined" — makes it
// UNDEFINED BEHAVIOUR, which 5.1.1.3 requires NO diagnostic for. J.2 lists it
// as UB explicitly.
// ★ So the standard is STRICTER about an ordinary redefinition (a real
// constraint) than about `#define __STDC__ 9` (UB). The old test encoded the
// intuitive inversion of that, and reading `shall not` as `constraint` without
// checking the enclosing subclause is what produced it.
// ✔MEASURED 2026-08-26, gcc 13.3.0 and clang 18.1.3 probed SEPARATELY: both
// WARN, both continue at rc=0, and both APPLY — `#undef __STDC__` then
// `#define __STDC__ 77` prints 77 on both.
//
// The PAIR is still the invariant, and it is still asserted in one place — but
// the invariant is now that BOTH are warnings that let translation continue,
// and that the predefined one TAKES EFFECT. A future "tidy-up" that restored
// the refusal would fail here.
TEST(Preprocessor, PredefinedMacroDefineWarnsAndAppliesLikeOrdinaryRedefinition) {
    PreprocessResult ordinary;
    (void)ppLexemes("#define X 1\n#define X 2\nint v = X;\n", ordinary);
    EXPECT_EQ(ppCodeSeverity(ordinary, DiagnosticCode::P_PreprocessorMacroRedefinition),
              DiagnosticSeverity::Warning);
    EXPECT_FALSE(ordinary.diagnostics->hasErrors())
        << "an ordinary redefinition must not stop translation";

    PreprocessResult predefined;
    auto lexs = ppLexemes("#define __STDC__ 9\nint v = __STDC__;\n", predefined);
    EXPECT_EQ(ppCodeSeverity(predefined, DiagnosticCode::P_PreprocessorPredefinedMacro),
              DiagnosticSeverity::Warning)
        << "6.10.10.1p2 sits OUTSIDE any Constraints subclause, so 4p2 makes it "
           "UB and no diagnostic is required at all — a warning is already more "
           "than the standard asks, and it is what both references emit";
    EXPECT_FALSE(predefined.diagnostics->hasErrors())
        << "refusing this was the divergence the row closed: gcc and clang both "
           "continue at rc=0";
    // AND IT TOOK EFFECT. Without this clause the test would still pass over an
    // engine that warned and then ignored the directive — which is the worst of
    // both readings, and exactly what the old code did.
    ASSERT_EQ(lexs.size(), 5u) << "int v = 9 ;";
    EXPECT_EQ(lexs[3], "9")
        << "the program's definition must WIN — gcc and clang both print the "
           "program's value";
}

// D-PP-REDEFINITION-IGNORES-WHITESPACE-PRESENCE — the OPPOSITE direction of the
// same rule, and the reason the shapes above were swept rather than assumed.
// C 6.10.3p2: white-space separation must agree in PRESENCE; the AMOUNT is
// immaterial. `MacroDef::text` joins tokens with one space unconditionally, so
// before `MacroDef::spacing` existed DSS reported these two lists as IDENTICAL
// and accepted SILENTLY what both references diagnose.
TEST(Preprocessor, RedefinitionWhitespacePresenceIsPartOfTheIdentity) {
    PreprocessResult differs;
    auto a = ppLexemes("#define M 40+2\n#define M 40 + 2\nint v = M;\n", differs);
    EXPECT_EQ(ppCodeSeverity(differs, DiagnosticCode::P_PreprocessorMacroRedefinition),
              DiagnosticSeverity::Warning)
        << "`40+2` vs `40 + 2` differ in white-space PRESENCE (C 6.10.3p2)";
    EXPECT_FALSE(differs.diagnostics->hasErrors());
    ASSERT_EQ(a.size(), 7u) << "int v = 40 + 2 ;";

    PreprocessResult amount;
    (void)ppLexemes("#define M 4 + 38\n#define M 4  +  38\nint v = M;\n", amount);
    EXPECT_FALSE(hasPPCode(amount, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "the AMOUNT of white space is immaterial — this pair is IDENTICAL, and "
           "a fix that reported it would have overshot into the other direction "
           "of wrong";

    PreprocessResult comment;
    (void)ppLexemes("#define M 4 + 38\n#define M 4 /*c*/ + 38\nint v = M;\n", comment);
    EXPECT_FALSE(hasPPCode(comment, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "a comment IS white space for this purpose (C 6.10.3p2), so it neither "
           "creates nor removes a separation";
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
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(r.diagnostics->hasErrors());

    // ⚠ The ORIGINAL buffer, not `r.synthBuffer`. The synth buffer now carries
    // the "<built-in>" prologue ahead of the source, so tokenizing IT would
    // count the prologue's own `#define` tokens as "raw input" and compare 83
    // tokens against 20 — a mismatch that says nothing about the identity
    // property. `buf` is what the caller actually handed the preprocessor, and
    // it is what "in == out" is a claim about.
    Tokenizer tk{buf, schema, DiagnosticBudget::libraryDefault()};
    auto rawResult = std::move(tk).tokenize();
    std::vector<Token> raw;
    while (!rawResult.stream.isAtEnd()) {
        raw.push_back(rawResult.stream.advance());
    }

    // Compare the non-Eof content tokens (the PP appends its own single Eof;
    // the raw drain above stops before Eof). Identity means: same count, same
    // core kinds, same lexemes, in order — and the spans related by ONE
    // CONSTANT TRANSLATION.
    //
    // ★ D-PP-PREDEFINE-REDEFINITION-PARTITION — WHY THE SPAN CLAIM IS STATED
    // THIS WAY AND NOT WEAKENED. It used to be raw span EQUALITY, which was a
    // proxy that silently assumed the synthetic "<built-in>" prologue is EMPTY.
    // That held only because the sole prologue occupants were the two pe-gated
    // function-like predefines, absent on the format this test loads. The
    // partition lowers every ORDINARY predefine into that prologue (the model
    // the references themselves implement), so the prologue is now non-empty on
    // every format and every span shifts by its length.
    // ⚠ The property under test is UNCHANGED and is still fully asserted: the
    // preprocessor is an identity pass on directive-free, macro-free input.
    // Same count, same kinds, same TEXT — and requiring the span delta to be
    // the SAME for every token is strictly STRONGER than the old equality was,
    // because it also catches a prologue that translated tokens unevenly, which
    // raw equality could never have detected. `raw` is re-tokenized from the
    // synth buffer, so a prologue whose own tokens leaked into the output would
    // change the COUNT and fail the assert above.
    std::vector<Token> ppNoEof;
    for (Token const& t : r.tokens) {
        if (t.coreKind != CoreTokenKind::Eof) ppNoEof.push_back(t);
    }
    ASSERT_EQ(ppNoEof.size(), raw.size())
        << "non-directive input must be identity (content token count)";
    ASSERT_FALSE(raw.empty());
    const std::int64_t delta = static_cast<std::int64_t>(ppNoEof[0].span.start())
                             - static_cast<std::int64_t>(raw[0].span.start());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        EXPECT_EQ(ppNoEof[i].coreKind, raw[i].coreKind) << "at index " << i;
        EXPECT_EQ(ppNoEof[i].span.length(), raw[i].span.length())
            << "at index " << i;
        EXPECT_EQ(static_cast<std::int64_t>(ppNoEof[i].span.start())
                      - static_cast<std::int64_t>(raw[i].span.start()),
                  delta)
            << "at index " << i
            << ": identity means ONE constant translation for the whole stream, "
               "not a per-token adjustment";
    }
}

// MULTI-LANGUAGE NO-OP at the config level: a language WITHOUT a preprocess
// block (toy, tsql-subset) reports preprocess().enabled == false, so the
// pipeline gate skips the pass; c (which declares the block) reports
// true. RED-ON-DISABLE: removing the c block flips its expectation.
TEST(Preprocessor, EnabledIsConfigDrivenPerLanguage) {
    auto c = GrammarSchema::loadShipped("c");
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
// it by loading the shipped c config TEXT with `functionLikeOpenToken`
// rebound to a DIFFERENT real token (`BlockOpen` = `{`). Now `#define F(x)`
// must be treated as an OBJECT-like macro (the `(` is no longer the configured
// function-like opener), so it must NOT emit P_PreprocessorUnsupported.
// RED-ON-DISABLE: reverting the ctor to the literal `find("ParenOpen")` makes
// the engine ignore the rebound config and STILL detect `(` as function-like
// -> P_PreprocessorUnsupported fires -> this test fails. (Agnosticism: the
// opener is read from config, so a language whose paren token is named
// differently is handled correctly.)
TEST(Preprocessor, FunctionLikeOpenTokenIsConfigDrivenNotHardcoded) {
    auto loadedText = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(loadedText.has_value());
    // Read the shipped c config text. This WAS a byte-for-byte copy of
    // the cwd walk `loadShippedCText()` used to carry; the copy is gone,
    // because a second implementation is a second place to forget when the
    // resolution rules change — which is precisely what an out-of-tree build
    // exposed (neither copy read $DSS_CONFIG_ROOT, so both missed together).
    namespace fs = std::filesystem;
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty()) << "could not locate shipped c config";
    // Rebind ONLY the function-like opener to `BlockOpen` (a real, declared
    // c token). The token name must resolve (validated at load), so this
    // is a well-formed schema -- just one where `(` is no longer the opener.
    const std::string from = "\"functionLikeOpenToken\": \"ParenOpen\"";
    const std::string to   = "\"functionLikeOpenToken\": \"BlockOpen\"";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos)
        << "shipped c config no longer carries functionLikeOpenToken=ParenOpen";
    text.replace(pos, from.size(), to);

    auto loaded = GrammarSchema::loadFromText(text, "<rebound-paren-c>");
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
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty()) << "could not locate shipped c config";
    // Rebind ONLY the variadic marker to `TildeOp` (a real, declared c
    // token that is NOT `...`). Still a well-formed schema -- just one where the
    // ellipsis is no longer the variadic marker.
    const std::string from = "\"variadicMarkerToken\": \"EllipsisOp\"";
    const std::string to   = "\"variadicMarkerToken\": \"TildeOp\"";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos)
        << "shipped c config no longer carries variadicMarkerToken=EllipsisOp";
    text.replace(pos, from.size(), to);

    auto loaded =
        GrammarSchema::loadFromText(text, "<rebound-variadic-c>");
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
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
// Helper: load the shipped c text with ONE `from`->`to` field rebind.
namespace {
[[nodiscard]] std::shared_ptr<GrammarSchema const>
reboundC(std::string const& from, std::string const& to,
               std::string const& label) {
    std::string text = loadShippedCText();
    if (text.empty()) {
        ADD_FAILURE() << "could not locate shipped c config";
        return nullptr;
    }
    auto const pos = text.find(from);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "shipped c config no longer carries: " << from;
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
loadCExpectingFailure(std::string const& from, std::string const& to,
                            std::string const& label) {
    std::string text = loadShippedCText();
    if (text.empty()) {
        ADD_FAILURE() << "could not locate shipped c config";
        return {};
    }
    auto const pos = text.find(from);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "shipped c config no longer carries: " << from;
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
    auto const ds = loadCExpectingFailure(
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
        auto const ds = loadCExpectingFailure(
            "\"prefix\": [\"once\"],                  \"effect\": \"includeOnce\" },",
            "\"prefix\": [\"pack\"],                  \"effect\": \"unsupported\" },",
            "<dup-pragma-prefix>");
        EXPECT_TRUE(hasCode(ds, DiagnosticCode::C_InvalidPreprocess))
            << "a prefix bound twice must fail the load";
    }
    {
        // EMPTY: `[]` is a prefix of EVERY pragma, so one such row silently
        // turns the whole registry into a catch-all and disarms the loudness.
        auto const ds = loadCExpectingFailure(
            "\"prefix\": [\"once\"],                  \"effect\": \"includeOnce\" },",
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
    std::string text = loadShippedCText();
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
        auto schema = reboundC(
            "\"functionLikeCloseToken\": \"ParenClose\"",
            "\"functionLikeCloseToken\": \"BracketClose\"",
            "<rebound-close-c>");
        ASSERT_NE(schema, nullptr);
        ASSERT_EQ(schema->preprocess().functionLikeCloseToken, "BracketClose");
        auto buf = SourceBuffer::fromString(
            std::string{"#define F(a,b) ((a)+(b))\nint v = F(1,2);\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
        auto schema = reboundC(
            "\"functionLikeArgSeparatorToken\": \"Comma\"",
            "\"functionLikeArgSeparatorToken\": \"Colon\"",
            "<rebound-separator-c>");
        ASSERT_NE(schema, nullptr);
        ASSERT_EQ(schema->preprocess().functionLikeArgSeparatorToken, "Colon");
        auto buf = SourceBuffer::fromString(
            std::string{"#define CNT(x) 1\nint v = CNT(a,b);\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
        reboundC("\"variadicArgsName\": \"__VA_ARGS__\"",
                       "\"variadicArgsName\": \"__REST__\"",
                       "<rebound-vaargs-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().variadicArgsName, "__REST__");

    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;

    // (1) The REBOUND catch-all `__REST__` substitutes the trailing args.
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#define V(...) g(__REST__)\nint v = V(1,2);\n"},
            "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    auto dir = ppScratchRoot() / "dss_pp_linemap_test";
    fs::create_directories(dir);
    // The header contains a malformed construct (a stray `@` is an illegal
    // char in c) so the parser/lexer emits a diagnostic whose span
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
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
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

// FIX 4 (RED-on-disable): the headline cycle-1 lexer change (c
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
        << "c must declare HeaderStart/HeaderPath for this pin to mean "
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
    auto dir = ppScratchRoot() / "dss_pp_render_attribution_test";
    fs::create_directories(dir);
    // Header error: a stray `@` (illegal char) on the header's line 1.
    // ★ THE EXPECTED LINES ARE NAMED, NEVER SPELLED INTO THE EXPECTATION. A bare
    // `"<file>:<line>:"` literal is indistinguishable from a positional CITATION — to
    // a reader and to `check-plan-citations` alike — and it restates a magic
    // number instead of saying where the number comes from. These two constants
    // ARE the fixture's shape: the header's only line, and the main file's line
    // AFTER the leading `#include`, which is the whole point of this pin.
    constexpr int kHeaderErrorLine = 1;
    constexpr int kMainErrorLine   = 2;
    { std::ofstream(dir / "bad.h", std::ios::binary)
          << "int hdr(void) { return @; }\n"; }
    // Main: a LEADING include (line 1), then a main-file error (`@`) on line 2.
    auto mainPath = dir / "main.c";
    { std::ofstream(mainPath, std::ios::binary)
          << "#include \"bad.h\"\nint main(void) { return @; }\n"; }

    auto schema = cSubset();
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
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

    // The header error attributes to bad.h's own line.
    std::string const headerAt =
        "bad.h:" + std::to_string(kHeaderErrorLine) + ":";
    EXPECT_NE(rendered.find(headerAt), std::string::npos)
        << "header-origin diagnostic must render " << headerAt << "\n"
        << rendered;
    // The main error attributes to the ORIGINAL main.c line (after the
    // leading #include) -- proving the splice did not drift the main line.
    std::string const mainAt =
        "main.c:" + std::to_string(kMainErrorLine) + ":";
    EXPECT_NE(rendered.find(mainAt), std::string::npos)
        << "main-origin diagnostic must render the ORIGINAL " << mainAt << "\n"
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
    UnitBuilder builder{*tsql, DiagnosticBudget::libraryDefault()};
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

    auto schema = reboundC("\"ifDirective\":         \"if\"",
                                 "\"ifDirective\":         \"whenever\"",
                                 "<rebound-if-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().ifDirective, "whenever");

    // (1) `#whenever 0` now conditionalizes -> the body is elided.
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#whenever 0\nint dead;\n#endif\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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

// CONFIG-READ: c declares the `#`/`##` operator kinds from config.
TEST(Preprocessor, FC15aStringizePasteTokensAreConfigRead) {
    auto c = GrammarSchema::loadShipped("c");
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
    auto dir = ppScratchRoot() / "dss_pp_file_macro_test";
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
    PreprocessResult r = preprocess(mainBuf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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

// D-PP-PREDEFINE-REDEFINITION-PARTITION (C23 6.10.10.1p2 + 4p2): a `#define` of
// an ENGINE-DERIVED predefine is DIAGNOSED and then APPLIED. Both halves are
// asserted, and the second is the one that changed: this test previously pinned
// that the directive did NOT alter the table.
// ✔MEASURED on gcc 13.3.0 and clang 18.1.3 separately: `#undef __LINE__` then
// `#define __LINE__ 4242` then `printf("%d", __LINE__)` prints 4242 on BOTH.
TEST(Preprocessor, FC15bDefineOfPredefinedWarnsAndTakesEffect) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define __LINE__ 5\nint x = __LINE__;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "a warn-class predefine must still be DIAGNOSED";
    EXPECT_EQ(ppCodeSeverity(r, DiagnosticCode::P_PreprocessorPredefinedMacro),
              DiagnosticSeverity::Warning);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // The program's definition WINS: line-2 `__LINE__` is now 5, not 2.
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "5")
        << "the #define must REPLACE the derived line value — 2 here would mean "
           "DSS warned and then ignored the directive, which is the one "
           "behaviour neither the old reading nor the new one licenses";
}

// The `#undef` half: DIAGNOSED, and the name is really GONE afterwards.
// ✔MEASURED on both references: after a bare `#undef`, `#ifdef __LINE__` /
// `__STDC__` / `__COUNTER__` are all FALSE, and `int __LINE__ = 9;` compiles.
TEST(Preprocessor, FC15bUndefOfPredefinedWarnsAndRemovesTheName) {
    PreprocessResult r;
    auto lexs = ppLexemes("#undef __FILE__\nconst char* f = __FILE__;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "a warn-class predefine must still be DIAGNOSED on #undef";
    EXPECT_EQ(ppCodeSeverity(r, DiagnosticCode::P_PreprocessorPredefinedMacro),
              DiagnosticSeverity::Warning);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    // `__FILE__` is now an ORDINARY IDENTIFIER — it no longer materializes a
    // string literal, so the token count collapses from 9 to 7.
    ASSERT_EQ(lexs.size(), 7u)
        << "const char * f = __FILE__ ;  — the name passes through unexpanded";
    EXPECT_EQ(lexs[5], "__FILE__")
        << "an applied #undef leaves a bare identifier; a materialized "
           "\"main.c\" here would mean the directive was diagnosed and dropped";
}

// The OTHER half of the partition, and the half with no diagnostic at all: an
// IMPLEMENTATION-SUPPLIED predefine is an ordinary macro. ✔MEASURED over a
// 36-name sweep on gcc 13.3.0 and clang 18.1.3 separately — `#undef __GNUC__`,
// `#undef __BYTE_ORDER__`, `#undef __SIZE_TYPE__`, `#undef __CHAR_BIT__` and
// 12 more are all SILENT on both, while every ISO-6.10.10 name and every
// engine-derived one warns on both.
// ⚠ THE SILENCE IS THE ASSERTION. A partition that emitted the warning for
// every config row would still pass every test above; only this one fails.
TEST(Preprocessor, OrdinaryPredefineUndefIsSilentAndTakesEffect) {
    PreprocessResult r;
    auto lexs = ppLexemes("#undef __GNUC__\nint x = __GNUC__;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "neither reference diagnoses `#undef __GNUC__`; DSS must not either";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorMacroRedefinition));
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "__GNUC__")
        << "the #undef must take effect — the name is a bare identifier now";
}

// And the redefinition arm of the ordinary half: NO predefined diagnostic, but
// the ORDINARY 6.10.5p2 policy still applies over the built-in definition, so a
// DIFFERING body warns and the NEW body wins. ✔MEASURED: gcc is silent for
// `#define __GNUC__ 13` (its own value is 13 ⇒ identical replacement list ⇒
// benign per 6.10.5p2) and clang warns for the same line (its `__GNUC__` is 4).
// Same rule, different inventories — which is why this asserts the RULE.
TEST(Preprocessor, OrdinaryPredefineRedefinitionUsesTheOrdinary61052Policy) {
    PreprocessResult differing;
    auto lexs = ppLexemes("#define __GNUC__ 99\nint x = __GNUC__;\n", differing);
    EXPECT_FALSE(hasPPCode(differing, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "an ordinary predefine is not in the diagnosed set";
    EXPECT_TRUE(hasPPCode(differing, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "but it IS a live macro, so a differing body is a 6.10.5p2 "
           "redefinition — the built-in prologue is what makes it visible";
    EXPECT_FALSE(differing.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "99") << "the NEW definition wins";

    // An `#undef` FIRST suppresses that warning, because there is then no live
    // definition to conflict with — the shape both references implement.
    PreprocessResult undefFirst;
    (void)ppLexemes("#undef __GNUC__\n#define __GNUC__ 99\nint x = __GNUC__;\n",
                    undefFirst);
    EXPECT_FALSE(hasPPCode(undefFirst, DiagnosticCode::P_PreprocessorMacroRedefinition))
        << "an explicit #undef reads as a deliberate replacement and silences "
           "the redefinition warning — MEASURED on gcc and clang";
    EXPECT_FALSE(hasPPCode(undefFirst, DiagnosticCode::P_PreprocessorPredefinedMacro));
}

// ══ D-PP-PREDEFINE-REDEFINITION-PARTITION — AGNOSTICISM / RED-ON-DISABLE ═════
//
// The partition is CONFIG DATA, not a name list in `src/`. This is the arm that
// proves it, and it runs the mutant in BOTH DIRECTIONS over the SHIPPED
// document so neither can pass vacuously:
//   (1) flip `__GNUC__` from `ordinary` to a warn verb  ⇒ the diagnostic APPEARS
//       on a name that is silent today;
//   (2) flip `__STDC__` from a warn verb to `ordinary`  ⇒ the diagnostic
//       DISAPPEARS from a name that warns today.
// ⚠ THE THIRD ASSERTION IS THE ONE THAT MATTERS: each case also asserts the
// CLEAN (unmutated) verdict is the OPPOSITE. Without it a mutant that silently
// failed to apply — or an engine that hard-coded a name and ignored the config
// entirely — would still be green in one direction, which is exactly how two of
// four arms asserted nothing in an earlier cycle.
// ⓘ `reboundC` ADD_FAILUREs when its `from` text is absent, so a mutation that
// stops matching the shipped document is LOUD rather than silently a no-op.
namespace {
[[nodiscard]] bool
predefinedDiagnosedUnder(std::shared_ptr<GrammarSchema const> const& schema,
                         std::string const& text) {
    if (schema == nullptr) return false;
    auto buf = SourceBuffer::fromString(text, "main.c");
    std::vector<std::filesystem::path> noDirs;
    PreprocessResult r =
        preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching,
                   DiagnosticBudget::libraryDefault());
    return hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro);
}
} // namespace

TEST(Preprocessor, PredefineRedefinitionVerbIsConfigDrivenNotHardcoded) {
    const std::string undefGnuc = "#undef __GNUC__\nint a;\n";
    const std::string undefStdc = "#undef __STDC__\nint a;\n";

    // The CLEAN baseline, from the shipped document. Both halves are asserted so
    // the mutants below have something to differ FROM.
    EXPECT_FALSE(predefinedDiagnosedUnder(cSubset(), undefGnuc))
        << "shipped `__GNUC__` is `ordinary` ⇒ silent";
    EXPECT_TRUE(predefinedDiagnosedUnder(cSubset(), undefStdc))
        << "shipped `__STDC__` is a warn verb ⇒ diagnosed";

    // (1) ordinary -> warn: the diagnostic must APPEAR.
    {
        auto mutated = reboundC(
            "\"value\": \"4\", \"programRedefinition\": \"ordinary\"",
            "\"value\": \"4\", \"programRedefinition\": \"warn-iso-macro\"",
            "<gnuc-to-warn>");
        ASSERT_NE(mutated, nullptr);
        EXPECT_TRUE(predefinedDiagnosedUnder(mutated, undefGnuc))
            << "the verb is read from config: flipping `__GNUC__` to a warn "
               "verb must start diagnosing it. Still silent here means the "
               "engine is deciding by NAME, not by the declared verb";
        // The mutant must not have moved the OTHER name — a mutation that
        // changed everything would make (1) green for the wrong reason.
        EXPECT_TRUE(predefinedDiagnosedUnder(mutated, undefStdc));
    }

    // (2) warn -> ordinary: the diagnostic must DISAPPEAR.
    {
        auto mutated = reboundC(
            "\"name\": \"__STDC__\",            \"kind\": \"constant\", "
            "\"value\": \"1\", \"programRedefinition\": \"warn-iso-macro\"",
            "\"name\": \"__STDC__\",            \"kind\": \"constant\", "
            "\"value\": \"1\", \"programRedefinition\": \"ordinary\"",
            "<stdc-to-ordinary>");
        ASSERT_NE(mutated, nullptr);
        EXPECT_FALSE(predefinedDiagnosedUnder(mutated, undefStdc))
            << "and the reverse: declaring `__STDC__` ordinary must silence it. "
               "Still diagnosed here means a hard-coded ISO name list survived";
        EXPECT_FALSE(predefinedDiagnosedUnder(mutated, undefGnuc));
    }
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
    auto schema = reboundC("\"name\": \"__LINE__\"",
                                 "\"name\": \"__CURLINE__\"",
                                 "<rebound-line-c>");
    ASSERT_NE(schema, nullptr);

    // (1) The REBOUND name resolves to its invocation line.
    {
        auto buf = SourceBuffer::fromString(
            std::string{"int a;\nint x = __CURLINE__;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
// change for toy / tsql-subset). c, by contrast, declares the 7 UNGATED
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

    auto c = GrammarSchema::loadShipped("c");
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
    //
    // ★★★ D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC: −1 pe-gated row. `_MSC_VER`
    // is DELETED. DSS predefined it for `pe` while `__GNUC__`/`__GNUC_MINOR__`/
    // `__GNUC_PATCHLEVEL__`/`__clang__` sit UN-GATED above, so a pe64 TU saw
    // `_MSC_VER` AND `__GNUC__` AND `_WIN32` at once — ✔MEASURED (clang 19.1.1,
    // `-dM -E --target=<triple>`) to be an identity NO reference compiler
    // presents: `x86_64-pc-windows-msvc` defines `_MSC_VER` and SUPPRESSES
    // `__GNUC__`; `x86_64-w64-mingw32` defines `__GNUC__` and not `_MSC_VER`.
    // Presenting the union is a BIDIRECTIONAL conformance defect, and it is what
    // sent pe64 alone into sqlite `src/hwtime.h`'s `#if defined(_MSC_VER) &&
    // defined(_WIN32)` arm -> `#include <profileapi.h>` -> error[F001A], while
    // the other four legs reached the `__GNUC__` rdtsc / `mrs cntvct_el0` arms.
    // The identity kept is GNU-on-Windows (the half DSS actually implements —
    // GNU extended inline asm, `__attribute__`, `__declspec` AS A MACRO, which
    // is the MinGW shape and not the MSVC one). 11 pe-gated - 1 = 10, so
    // 19 un-gated + 10 pe-gated + 3 macho-gated = 32.
    //
    // TF-C115 (D-PP-ENDIANNESS-PREDEFINES): +3 UN-GATED rows — the byte-order
    // NAMING VOCABULARY `__ORDER_LITTLE_ENDIAN__`/`__ORDER_BIG_ENDIAN__`/
    // `__ORDER_PDP_ENDIAN__` (1234/4321/3412). They are on the LANGUAGE, not the
    // target, because MEASURED 2026-08-04 (`clang-19 -dM -E -x c /dev/null
    // -target <triple>`) they are IDENTICAL on every triple DSS targets AND on
    // the big-endian control `aarch64_be-linux-gnu` — they are names, not the
    // machine's answer, so they vary by nothing. The per-CPU ANSWER
    // (`__LITTLE_ENDIAN__`, `__BYTE_ORDER__`) is on the TARGET and is therefore
    // absent from this list, exactly like the TF-C74 identity spellings.
    // 16+3=19 un-gated, 11 pe-gated, 3 macho-gated = 33 (was 32 after _MSC_VER
    // was deleted; now 35 — the GNU-on-Windows identity was COMPLETED with
    // __MINGW32__/__MINGW64__/__MSVCRT__, all pe-gated, in the same change that
    // gave pe the <unistd.h>/<dirent.h> surface those three promise;
    // D-LANG-PE64-HAS-NO-POSIX-DIRECTORY-API).
    // D-CSUBSET-COUNTER-MACRO-NOT-EXPANDED: +1 UN-GATED row, `__COUNTER__`. It is
    // on the LANGUAGE and un-gated for the same reason the __ORDER_* rows are:
    // its value is a property of the TRANSLATION, not of the CPU or the object
    // format, so it varies by nothing a target or format could decide. It is also
    // the FIRST row of the `counter` kind — the only STATEFUL one — which is why
    // it could not be expressed as a `constant` row.
    // 20 un-gated, 13 pe-gated, 3 macho-gated = 36.
    EXPECT_EQ(pms.size(), 36u)
        << "c declares 20 un-gated + 13 pe-gated + 3 macho-gated predefined macros";
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
           "TargetConditionals.h conjunction that gates the whole Darwin ladder";
    EXPECT_EQ(ungated, 20u)
        << "__COUNTER__ (D-CSUBSET-COUNTER-MACRO-NOT-EXPANDED, the one `counter` "
           "kind) + the 7 C 6.10.8 macros + __BITINT_MAXWIDTH__ (_BitInt C1) + the 3 C23 "
           "__STDC_EMBED_* trichotomy macros (FC17.9(h), D-PP-EMBED) + the 5 TF-C83 "
           "un-gated identity rows (__DSSCP__, __GNUC__, __GNUC_MINOR__, "
           "__GNUC_PATCHLEVEL__, __clang__) + the 3 TF-C115 __ORDER_* byte-order "
           "vocabulary rows (D-PP-ENDIANNESS-PREDEFINES — measured identical on "
           "every triple INCLUDING the big-endian control, so they are names "
           "rather than a per-CPU fact and belong here rather than on a target) "
           "are un-gated (every format); "
           "__STDC_NO_VLA__ (D-CSUBSET-VLA C1b) + __STDC_NO_THREADS__ (threads.h "
           "complete on all legs) are both REMOVED";
    EXPECT_EQ(peGated, 13u)
        << "_WIN32/_WIN64/__stdcall/__cdecl/__fastcall/WINAPI (c95) + "
           "__int64/__forceinline/__declspec (c105; _MSC_VER DELETED, "
           "D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC) + _declspec (the legacy "
           "single-underscore MSVC alias; tcl.h's TCL_NORETURN under the pe profile, "
           "D-SQLITE-PE64-TESTFIXTURE-FRONTEND B1) + __MINGW32__/__MINGW64__/__MSVCRT__ "
           "(D-LANG-PE64-HAS-NO-POSIX-DIRECTORY-API: the GNU-on-Windows identity, "
           "COMPLETED in the same change that made it backable -- pe gained <unistd.h> "
           "as a re-export and <dirent.h> via a shipped-source realization, so all three "
           "carry an `impliedSurface` claim naming the symbols they promise) are pe-gated. "
           "MEASURED, x86_64-w64-mingw32-gcc -dM -E: the reference defines __MINGW32__, "
           "__MINGW64__, __MSVCRT__, _WIN32, _WIN64, __GNUC__ and NOT _MSC_VER, so this "
           "count going back to 10 means DSS is presenting an identity no toolchain has";
}

// LOADER fail-loud (c95): a `predefinedMacros.availableObjectFormats` naming an
// UNKNOWN object-format is a config typo that would silently never seed the
// macro on any target (an OS-selection macro that never fires) -> it must be a
// LOAD error, never accepted. We corrupt `_WIN32`'s ["pe"] to ["pee"] and assert
// the load FAILS (C_InvalidPreprocess via objectFormatKindFromName). RED-ON-
// DISABLE: without the loader validation this parses and the macro is dead.
TEST(Preprocessor, FC15bPredefinedMacroBadObjectFormatIsLoadError) {
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty());
    // ANCHORED ON THE ROW, MUTATING ONLY THE FIELD UNDER TEST. The first cut
    // matched the WHOLE `_WIN32` entry verbatim, which made this pin fail every
    // time an UNRELATED field was added to that row -- it broke when
    // `impliedSurface` landed (D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE),
    // and it would break again on the next one. That is not a weakening: the
    // verbatim match never asserted anything about the row's other fields, it
    // only guaranteed the substitution actually happened, and locating the row by
    // NAME and then its OWN `availableObjectFormats` guarantees exactly that while
    // naming the field the test is about. It still fails loud if `_WIN32` stops
    // being ["pe"]-gated, which is the property worth pinning here.
    const std::string row = "\"name\": \"_WIN32\"";
    auto const rowPos = text.find(row);
    ASSERT_NE(rowPos, std::string::npos)
        << "the _WIN32 predefinedMacros entry must be present";
    const std::string from = "\"availableObjectFormats\": [\"pe\"]";
    const std::string to   = "\"availableObjectFormats\": [\"pee\"]";
    auto const pos = text.find(from, rowPos);
    ASSERT_NE(pos, std::string::npos)
        << "_WIN32 must still carry availableObjectFormats [\"pe\"] -- if it does "
           "not, this test is mutating some LATER row and proves nothing about _WIN32";
    text.replace(pos, from.size(), to);
    auto loaded = GrammarSchema::loadFromText(text, "<bad-objfmt-c>");
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
// 4002001 that sqliteInt.h computes.
TEST(Preprocessor, TFC83IdentityPredefineValuesMatchClang) {
    auto c = GrammarSchema::loadShipped("c");
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
    // The GCC_VERSION arithmetic sqliteInt.h actually performs.
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
        std::string text = loadShippedCText();
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
    auto ok = GrammarSchema::loadFromText(loadShippedCText(),
                                          "<tfc83-version-control>");
    EXPECT_TRUE(ok.has_value()) << "the shipped c text must still load";
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
    std::vector<std::filesystem::path> systemDirs,
    // D-PP-HEADER-CASE-INSENSITIVE-PE: the active format's header-name case
    // rule. Defaults to the production default so every existing caller is
    // byte-identical; the case pins pass it explicitly.
    dss::HeaderNameMatching matching = dss::kDefaultHeaderNameMatching) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(std::move(text), "main.c");
    out = preprocess(buf, schema, includeDirs, matching, DiagnosticBudget::libraryDefault(), systemDirs);
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
        // ⚠ THE FIXTURE MOVED 2026-08-29 AND THE REASON IS THE POINT, NOT
        // BOOKKEEPING. This arm used `#pragma STDC FP_CONTRACT OFF`, which was
        // an `unsupported` row when the arm was written.
        // D-PP-PRAGMA-RECOGNIZED-SEMANTICS BUILT the `STDC` family, and
        // `FP_CONTRACT OFF` is now `standardFloatState` — a state DSS SATISFIES
        // (C23 7.12.2p2 disallows contraction, and MEASURED, DSS contracts
        // nothing anywhere), so it is accepted and silent. Keeping it here would
        // have pinned the OLD behaviour of a pragma that deliberately changed.
        // `GCC poison` is used instead because it is STILL genuinely
        // `unsupported`: it makes an identifier an error if used, DSS has not
        // built that, and ignoring it silently accepts code the header author
        // forbade. The arm therefore tests exactly what it always meant to —
        // a row that CLAIMS real unbuilt semantics is LOUD — with an exemplar
        // that still has them.
        auto lexs = ppLexemes("#pragma GCC poison evil\nint v=1;\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "an `unsupported` row must fail loud, not be silently ignored";
        ASSERT_EQ(lexs.size(), 5u);
    }
    // (D) ★ THE FOURTH VERDICT, ADDED 2026-08-29: ACCEPTED-WITH-NOTICE. A
    // `standardFloatStateDiverges` row means the pragma is RECOGNIZED and the TU
    // COMPILES (all four references accept it, so refusing is below the union)
    // while the unhonoured request is NAMED at WARNING severity. It is the only
    // verdict in this table that is neither silent nor fatal, and it exists
    // because the other two are both wrong for a numerics request DSS cannot
    // meet: silence ships wrong results, refusal rejects a valid program.
    {
        PreprocessResult r;
        auto lexs =
            ppLexemes("#pragma STDC CX_LIMITED_RANGE OFF\nint v=1;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "the translation unit must still compile — every reference "
               "accepts this pragma";
        bool sawWarning = false;
        for (auto const& d : r.diagnostics->all()) {
            if (d.code == DiagnosticCode::P_PreprocessorPragma
                && d.severity == DiagnosticSeverity::Warning) {
                sawWarning = true;
            }
        }
        EXPECT_TRUE(sawWarning)
            << "…and the divergence must be NAMED: DSS evaluates complex "
               "multiply and divide with the usual algebraic formulas, which is "
               "the state this pragma is asking it NOT to use";
        ASSERT_EQ(lexs.size(), 5u)
            << "an accepted-with-notice pragma emits no tokens either";
    }
}

// ★ TF-C82: the OPT-OUT, and the red-on-disable for the loudness itself.
// `unknownPragmaIsError: false` restores the pre-TF-C82 silent drop EXACTLY —
// so the loud posture is a declared choice a language makes, not a behavior
// baked into the engine. Without this pin, "loud" and "hard-coded" would be
// indistinguishable from the outside.
TEST(Preprocessor, TfC82UnknownPragmaIsErrorFalseRestoresSilence) {
    auto schema = reboundC("\"unknownPragmaIsError\":     true,",
                                 "\"unknownPragmaIsError\":     false,",
                                 "<silent-pragma-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_FALSE(schema->preprocess().unknownPragmaIsError)
        << "the rebound schema must declare the silent posture";
    auto buf = SourceBuffer::fromString(
        std::string{"#pragma GCC optimize(\"O2\")\nint v=1;\n"}, "main.c");
    std::vector<std::filesystem::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
// `sys/queue.h` needs this.
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
    auto schema = reboundC("\"pragmaOperator\":           \"_Pragma\",",
                                 "",
                                 "<no-pragma-operator-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->preprocess().pragmaOperator.empty())
        << "the rebound schema must declare no pragma operator";
    auto buf = SourceBuffer::fromString(
        std::string{"int _Pragma;\n"}, "main.c");
    std::vector<std::filesystem::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    auto schema = reboundC("\"pragmaPackPushWord\":       \"push\",",
                                 "",
                                 "<no-pack-push-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->preprocess().pragmaPackPushWord.empty());
    std::vector<std::filesystem::path> noDirs;
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#pragma pack(push, 4)\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
            << "with no `pragmaPackPushWord` declared the push form is an "
               "unbuilt form and must be REFUSED, never silently ignored";
    }
    {
        auto buf = SourceBuffer::fromString(
            std::string{"#pragma pack(4)\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, fmt, noDefines);
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
// (mutex_w32.c) and `default : N` (totype.c). The row claims nothing
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
    auto schema = reboundC("\"prefix\": [\"warning\"]",
                                 "\"prefix\": [\"warningXX\"]",
                                 "<no-warning-row-c>");
    ASSERT_NE(schema, nullptr);
    std::vector<std::filesystem::path> noDirs;
    auto buf = SourceBuffer::fromString(
        std::string{"#pragma warning(disable: 4127)\nint x;\n"}, "main.c");
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    auto schema = reboundC("\"prefix\": [\"intrinsic\"]",
                                 "\"prefix\": [\"intrinsicXX\"]",
                                 "<no-intrinsic-row-c>");
    ASSERT_NE(schema, nullptr);
    std::vector<std::filesystem::path> noDirs;
    auto buf = SourceBuffer::fromString(
        std::string{"#pragma intrinsic(_byteswap_ulong)\nint x;\n"}, "main.c");
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
        auto schema = reboundC("\"prefix\": [\"optimize\"]",
                                     "\"prefix\": [\"optimizeXX\"]",
                                     "<no-optimize-row-c>");
        ASSERT_NE(schema, nullptr);
        std::vector<std::filesystem::path> noDirs;
        auto buf = SourceBuffer::fromString(
            std::string{"#pragma optimize(\"\", off)\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma));
        EXPECT_TRUE(r.pragmaNoOptimizeByOffset.empty())
            << "and nothing is stamped — the sink is inert without its row";
    }
    {   // row present, but the OFF word undeclared -> the form is unbuilt -> loud
        auto schema = reboundC("\"pragmaOptimizeOffWord\":    \"off\",",
                                     "",
                                     "<no-optimize-off-word-c>");
        ASSERT_NE(schema, nullptr);
        ASSERT_TRUE(schema->preprocess().pragmaOptimizeOffWord.empty());
        std::vector<std::filesystem::path> noDirs;
        auto buf = SourceBuffer::fromString(
            std::string{"#pragma optimize(\"\", off)\nint x;\n"}, "main.c");
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
// (_WIN32/_WIN64 + more; `_MSC_VER` was here until
// D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC deleted it), `macho`
// (__APPLE__/__MACH__), and the
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
        test_support::findCorpusRoot() / "c" / "pragma_profile_census.c";
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
// trivially, and the pe-only pragmas live behind a pe-ONLY PREDEFINE — so the
// fixture MUST actually reach them on the pe leg and MUST NOT on the others.
// Without this, deleting the fixture's whole body would leave the guard green.
//
// ★★ THE FIXTURE'S GUARD IS `_WIN32`, NOT `_MSC_VER`, AND THAT IS NOT A
// WEAKENING (D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC). `_MSC_VER` is gone
// from every leg — DSS may not present an identity no reference compiler has —
// so a guard spelled on it would now be dead EVERYWHERE and this non-vacuity
// twin would be asserting over an arm nothing can reach. What the pin needs is
// a predefine LIVE on pe and DEAD elsewhere; `_WIN32` is that, and is the
// honest one (it names the OS, not a toolchain). Both halves below are
// unchanged in strength: same two legs, same stamping witness.
TEST(Preprocessor, TfC85ProfileCensusFixtureActuallyReachesTheProfileGatedRows) {
    auto const text = test_support::readFile(
        test_support::findCorpusRoot() / "c" / "pragma_profile_census.c");
    ASSERT_FALSE(text.empty());
    {   // pe: `_WIN32` is defined, so the MSVC arm is LIVE and its
        // `#pragma optimize("", off)` region actually stamps a token.
        PreprocessResult r;
        ppUnderFormat(text, ObjectFormatKind::Pe, r);
        EXPECT_FALSE(r.pragmaNoOptimizeByOffset.empty())
            << "on the pe leg the fixture's `#pragma optimize` region must be "
               "REACHED — otherwise this whole census is vacuous";
        EXPECT_TRUE(tokenIsNoOptimize(r, "msvc_no_optimize_marker"));
    }
    {   // macho: `_WIN32` undefined -> the MSVC arm is a DEAD branch -> C
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
    // ⚠ ONE FIXTURE MOVED 2026-08-29 (D-PP-PRAGMA-RECOGNIZED-SEMANTICS) BECAUSE
    // THE BEHAVIOUR IT STOOD FOR CHANGED, NOT BECAUSE THE TEST WAS WRONG. This
    // list needs pragmas that are MEASURABLY LOUD WHEN REACHED — that is what
    // makes the dead-branch arms non-vacuous — and `#pragma STDC FP_CONTRACT
    // OFF` stopped being one: it is now `standardFloatState`, a state DSS
    // satisfies, so it is accepted and silent. `FP_CONTRACT MAYBE` replaces it
    // and keeps the arm's ORIGINAL meaning intact: `MAYBE` is not an
    // on-off-switch (C23 6.10.8 spells it `one of ON OFF DEFAULT`), so it
    // matches no three-word row, falls through to the one-word `STDC` row, and
    // is loud for exactly the reason the comment claims — an `unsupported` row.
    char const* const deadPragmas[] = {
        "#pragma whatever here",          // unregistered -> loud if reached
        "#pragma STDC FP_CONTRACT MAYBE", // `unsupported` row -> loud if reached
        // ★ THE FOURTH VERDICT, and it belongs in this list precisely because it
        // is NOT fatal: an accepted-with-notice pragma still emits a
        // `P_PreprocessorPragma` (at Warning severity) when REACHED, and must
        // still be entirely silent when elided. A verdict that fires a warning
        // is exactly the kind a hoisted recognizer would leak out of a dead
        // branch, which is the failure this test exists to catch.
        "#pragma STDC CX_LIMITED_RANGE OFF",
        "#pragma pack(3)",                // malformed operand -> loud if reached
        "#pragma pack(pop)",              // unbalanced pop    -> loud if reached
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
    auto schema = reboundC("\"pragmaDirective\":          \"pragma\",",
                                 "",
                                 "<no-pragma-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->preprocess().pragmaDirective.empty())
        << "the rebound schema must declare no pragma directive";
    auto buf = SourceBuffer::fromString(
        std::string{"#pragma GCC optimize(\"O2\")\nint v=1;\n"}, "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    auto dir = ppScratchRoot() / "dss_fc15c_has_include_q";
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
    auto sysdir = ppScratchRoot() / "dss_fc15c_has_include_sys";
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
    auto inc = ppScratchRoot() / "dss_angle_src_fallback_p1";
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
    auto inc = ppScratchRoot() / "dss_angle_src_fallback_p5";
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
    auto sysdir = ppScratchRoot() / "dss_dperf2_resolved_desc";
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
    auto sysdir = ppScratchRoot() / "dss_dperf2_deadbranch";
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
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty());
    for (std::string const& line :
         {std::string{"\"hasIncludeOperator\":       \"__has_include\",\n"},
          std::string{"    \"hasIncludeAngleOpenToken\":  \"LtOp\",\n"},
          std::string{"    \"hasIncludeAngleCloseToken\": \"GtOp\",\n"}}) {
        auto const pos = text.find(line);
        ASSERT_NE(pos, std::string::npos) << "config no longer carries: " << line;
        text.erase(pos, line.size());
    }
    auto loaded = GrammarSchema::loadFromText(text, "<no-has-include-c>");
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
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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

// CONFIG-READ pins: the shipped c declares the operator names + the angle
// token KINDS; toy / tsql declare none. The angle delimiters being CONFIG token
// names (not the `<`/`>` bytes) is the make-or-break agnosticism property.
TEST(Preprocessor, FC15cOperatorNamesAndAngleTokensAreConfigDeclared) {
    auto c = GrammarSchema::loadShipped("c");
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
    auto schema = reboundC("\"hasIncludeAngleOpenToken\":  \"LtOp\"",
                                 "\"hasIncludeAngleOpenToken\":  \"TildeOp\"",
                                 "<rebound-angle-open-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().hasIncludeAngleOpenToken, "TildeOp");
    // `__has_include(<stdio.h>)`: the `<` is no longer the configured angle
    // opener, so the operand is neither the angle nor the quote form -> the
    // engine must fail loud (it is NOT silently parsed via the `<` byte).
    auto buf = SourceBuffer::fromString(
        std::string{"#if __has_include(<stdio.h>)\nint a;\n#endif\n"}, "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty());
    const std::string line = "\"hasIncludeAngleOpenToken\":  \"LtOp\",\n";
    auto const pos = text.find(line);
    ASSERT_NE(pos, std::string::npos);
    text.erase(pos, line.size());
    auto loaded = GrammarSchema::loadFromText(text, "<bad-has-include-c>");
    EXPECT_FALSE(loaded.has_value())
        << "declaring hasIncludeOperator without both angle tokens must be a "
           "load error (C_InvalidPreprocess)";
}

// LOADER: a malformed `knownCAttributes` entry (a non-positive version) ->
// C_InvalidPreprocess at load.
TEST(Preprocessor, FC15cKnownCAttributeBadVersionIsLoadError) {
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty());
    const std::string from = "{ \"name\": \"deprecated\",   \"version\": 202311 }";
    const std::string to   = "{ \"name\": \"deprecated\",   \"version\": 0 }";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos);
    text.replace(pos, from.size(), to);
    auto loaded = GrammarSchema::loadFromText(text, "<bad-attr-c>");
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
// c's `variadicCommaElision` to false and re-preprocess: the comma now
// SURVIVES (standard placemarker) -> `f ( 42 , )`. RED-ON-DISABLE: if the engine
// hardcoded the elision (ignoring the flag), the comma would vanish even at false.
TEST(Preprocessor, FC15GnuCommaElisionIsConfigDriven) {
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty());
    const std::string from = "\"variadicCommaElision\": true";
    const std::string to   = "\"variadicCommaElision\": false";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos) << "config no longer carries the flag";
    text.replace(pos, from.size(), to);
    auto loaded = GrammarSchema::loadFromText(text, "<no-elision-c>");
    ASSERT_TRUE(loaded.has_value())
        << (loaded.error().empty() ? "<none>" : loaded.error()[0].message);
    ASSERT_FALSE((*loaded)->preprocess().variadicCommaElision);

    namespace fs = std::filesystem;
    auto buf = SourceBuffer::fromString(
        std::string{"#define LOG(fmt, ...) f(fmt, ## __VA_ARGS__)\nLOG(42)\n"},
        "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, *loaded, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
// argument still errors. c declares `#` (HashOp), so `#define S(x) #x` +
// `S($)` consumes the `$` into a `#`-product string -- the original `$` token
// does NOT survive, so again only the dead-region (byte-liveness) oracle catches
// it. RED-ON-DISABLE: the survival oracle drops it. (If `#` were out of c
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
    auto dir = ppScratchRoot() / "dss_tf60_c2p";
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
    auto out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, {});
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
    auto dir = ppScratchRoot() / "dss_tf60_p2c";
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
    auto out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, {});
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
    auto dir = ppScratchRoot() / "dss_tf60_charlit";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "inner.h", std::ios::binary) << "int lit_ok_zzz;\n"; }
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#define NL '\\n'\n#if NL == 10\n#include \"inner.h\"\n#endif\nint y;\n",
        "main.c");
    std::vector<fs::path> includeDirs{dir};
    auto out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, {});
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
    auto dir = ppScratchRoot() / "dss_tf60_mint";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "inner.h", std::ios::binary) << "int mint_ok_zzz;\n"; }
    auto schema = cSubset();
    std::vector<fs::path> includeDirs{dir};
    {   // positive: the chain must fold to EXACTLY 424242
        auto buf = SourceBuffer::fromString(
            "#define A B\n#define B 424242\n#if A == 424242\n"
            "#include \"inner.h\"\n#endif\nint y;\n", "main.c");
        auto out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, {});
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
    auto dir = ppScratchRoot() / "dss_tf60_morelive";
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
    auto out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, {});
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
    auto dir = ppScratchRoot() / "dss_c19_child_seed";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "outer.h", std::ios::binary)
          << "#ifdef GATE\n#include \"still_missing_inner.h\"\n#endif\n"; }
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#ifdef GATE\n#include \"outer.h\"\n#endif\nint x;\n", "main.c");
    std::vector<fs::path> includeDirs{dir};
    std::vector<std::string> defines{"GATE"};
    auto out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
    auto dir = ppScratchRoot() / "dss_tf59_line_hdr";
    std::error_code ec;
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "h.h", std::ios::binary)
          << "#line 700\nint from_header = __LINE__;\n"; }
    auto schema = cSubset();
    //                                        line: 1              2
    auto buf = SourceBuffer::fromString(
        "#include \"h.h\"\nint after = __LINE__;\n", "main.c");
    std::vector<fs::path> includeDirs{dir};
    auto out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, {});
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
        auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
        EXPECT_TRUE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
            << "#if defined(M) must gate the include LIVE for --define M=1";
    }
    {
        auto buf = SourceBuffer::fromString(
            "#if M\n#include \"still_missing.h\"\n#endif\nint x;\n", "main.c");
        auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
// predefined subset of the prefix (c __STDC_VERSION__ = 202311L).
TEST(Preprocessor, PredefinedValueGuardMakesIncludeLive) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString(
        "#if __STDC_VERSION__ >= 201112L\n#include \"still_missing.h\"\n#endif\n"
        "int x;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string> noDefs;
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, noDefs);
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, noDefs);
    EXPECT_FALSE(hasPPCode(out, DiagnosticCode::P_PreprocessorIncludeError))
        << "#if __STDC_VERSION__ < 0 must stay dead (the seeded predefined value is "
           "real, not a blanket predefined->live)";
}

// C21 Pin F (FINDING-A, function-like predefine must NOT be value-seeded): a bare
// `#if NAME` (no call) on a FUNCTION-like predefine must fold to 0 in the pre-scan
// EXACTLY as in the authoritative pass; value-seeding it would make the pre-scan
// MORE-live -> a silent P0016 re-open. The prefix builder therefore SKIPS
// `isFunctionLike` predefines (mirroring the MacroExpander ctor + the <built-in>
// prologue). NOTE: the c schema's function-like predefines are `_declspec` (B1) +
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, ObjectFormatKind::Pe, noDefs);
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
    auto out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, std::nullopt, defines);
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
    auto sysdir = ppScratchRoot() / "dss_ppangle_pos_sys";
    auto incdir = ppScratchRoot() / "dss_ppangle_pos_inc";
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
    auto sysdir = ppScratchRoot() / "dss_ppangle_dead_sys";
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
    auto sysdir = ppScratchRoot() / "dss_ppangle_p0016_sys";
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
    auto sysdir = ppScratchRoot() / "dss_ppangle_malformed_sys";
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
    auto sysdir = ppScratchRoot() / "dss_ppangle_transitive_sys";
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
    auto sysdir = ppScratchRoot() / "dss_ppangle_cyclic_sys";
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
    auto schema = reboundC("\"ifDirective\":         \"if\"",
                                 "\"ifDirective\":         \"whenever\"",
                                 "<rebound-if-c17>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().ifDirective, "whenever");

    auto buf = SourceBuffer::fromString(
        std::string{"#whenever 0\n#include \"nope.h\"\n#endif\nint x;\n"},
        "main.c");
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    auto schema = reboundC("\"endifDirective\":      \"endif\"",
                                 "\"endifDirective\":      \"endwhile\"",
                                 "<rebound-endif-c17>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().endifDirective, "endwhile");

    auto buf = SourceBuffer::fromString(
        std::string{"#if 0\n#endwhile\n$\nint x;\n"}, "main.c");
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    // This test is count-free by construction, so D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN
    // left it GREEN; only the wording above needed the correction (the
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
    out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, fmt, defines);
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

// D-PP-PREDEFINE-REDEFINITION-PARTITION: a `--define` naming a WARN-CLASS
// config predefine (here `__STDC__`) is DIAGNOSED and then APPLIED — the same
// partition as the in-source directive, because the CLI entry lowers to an
// ordinary `<command-line>` `#define` that runs through the very same handler.
// ✔MEASURED 2026-08-26, gcc 13.3.0 and clang 18.1.3 separately: `-D__STDC__=0`
// warns, rc=0, on both; `-U__GNUC__` and `-U__BYTE_ORDER__` are SILENT on both;
// only `-Ddefined=1`/`-Udefined` are hard errors.
// ⚠ THIS TEST USED TO SAY THE OPPOSITE ("must fail loud, not override") and to
// justify it as the `_MSC_VER`/`_WIN32` SILENT-MISCOMPILE CHANNEL. That framing
// does not survive the measurement: `_WIN32` is an implementation-supplied
// name, not an ISO-6.10.10 one, so both references let a program flip it
// silently — and in this architecture the OBJECT FORMAT, never the macro,
// decides which descriptors and which target a build actually uses, so flipping
// the macro changes what USER code sees in an `#ifdef` and nothing else.
TEST(Preprocessor, UserDefineCollidingWithConfigPredefineWarnsAndApplies) {
    PreprocessResult r;
    auto lexs = ppLexemesWithDefines("int a = __STDC__;\n", {"__STDC__=0"}, r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "a warn-class predefine is diagnosed however the definition arrives";
    EXPECT_EQ(ppCodeSeverity(r, DiagnosticCode::P_PreprocessorPredefinedMacro),
              DiagnosticSeverity::Warning);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "0") << "--define overrides, as it does on gcc and clang";
}

// The ORDINARY half of the same axis, and the arm that would survive a
// blanket relaxation OR a blanket refusal only by accident: `--define` of an
// implementation-supplied predefine is SILENT about the predefined rule and
// governed purely by 6.10.5p2 over the built-in definition.
TEST(Preprocessor, UserDefineCollidingWithOrdinaryPredefineIsNotAPredefinedDiagnostic) {
    PreprocessResult r;
    auto lexs = ppLexemesWithDefines("int a = __GNUC__;\n", {"__GNUC__=9"}, r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "gcc reports `-D__GNUC__=9` as a plain <command-line> redefinition "
           "of a <built-in> macro, not as a protected-name violation";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "9") << "and it takes effect";
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
// C23 6.10.9.3) — the macro is REMOVED from c.lang.json entirely. RED-on-disable:
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
// `ppLexemes` (which loads the shipped c).
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
    auto schema = reboundC("\"elifdefDirective\":    \"elifdef\",",
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
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
        PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    auto schema = reboundC("\"elifdefDirective\":    \"elifdef\",", "",
                                 "<no-elifdef-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_TRUE(schema->preprocess().elifdefDirective.empty())
        << "the rebound schema must declare no elifdef directive";
    auto buf = SourceBuffer::fromString(
        std::string{"#define A\n#ifdef A\nint a;\n#elifdef B\nint b;\n#endif\n"},
        "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    auto dir = ppScratchRoot() / "dss_elifdef_live_inc";
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
    auto dir = ppScratchRoot() / sub;
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
    out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), noSys);
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
    auto root = ppScratchRoot() / "dss_embed_t4";
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
    auto r = preprocess(buf, schema, dirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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

// T13 (C23 6.10.10.1p2): a `#define` of a predefined `__STDC_EMBED_*` macro is
// DIAGNOSED. These three are ISO 6.10.10.2 MANDATORY macros, so they carry the
// warn verb — and this loop is what keeps them there if somebody re-classifies
// the `__STDC_*` family. ⚠ Post D-PP-PREDEFINE-REDEFINITION-PARTITION the
// directive is no longer REFUSED: 6.10.10 sits outside any Constraints
// subclause, so 4p2 makes it UB and both references warn-and-apply.
TEST(Preprocessor, FC179EmbedStdcMacrosAreDiagnosedPredefines) {
    for (std::string const& name : {"__STDC_EMBED_NOT_FOUND__",
                                    "__STDC_EMBED_FOUND__",
                                    "__STDC_EMBED_EMPTY__"}) {
        PreprocessResult r;
        (void)ppEmbedTokens("#define " + name + " 9\nint a;\n", r, {});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
            << "#define of a predefined embed macro must be diagnosed: " << name;
        EXPECT_EQ(ppCodeSeverity(r, DiagnosticCode::P_PreprocessorPredefinedMacro),
                  DiagnosticSeverity::Warning)
            << "diagnosed, not refused: " << name;
        EXPECT_FALSE(r.diagnostics->hasErrors()) << name;
    }
}

// T14 (agnosticism): the directive WORD is config-driven, never a hard-coded
// "embed". Rebind `embedDirective` off "embed" -> "embad": now `#embed` is an
// unknown directive (P0015) and `#embad "..."` drives the embed handler
// (P_PreprocessorEmbed). RED-ON-DISABLE: a hard-coded "embed" ignores the rebind.
TEST(Preprocessor, FC179EmbedDirectiveIsConfigDrivenNotHardcoded) {
    auto schema = reboundC("\"embed\"", "\"embad\"", "<rebound-embed>");
    ASSERT_TRUE(schema != nullptr);
    ASSERT_EQ(schema->preprocess().embedDirective, "embad");
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    {
        auto buf = SourceBuffer::fromString(
            "static const unsigned char x[] = {\n#embed \"missing.bin\"\n};\n",
            "main.c");
        auto r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "with the word rebound, `#embed` is an unknown directive (P0015)";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorEmbed));
    }
    {
        auto buf = SourceBuffer::fromString(
            "static const unsigned char x[] = {\n#embad \"missing.bin\"\n};\n",
            "main.c");
        auto r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    auto r = preprocess(buf, schema, dirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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

// Rebind ONE `"<key>": "<word>"` string field of the shipped c config to
// `newWord` and reload. Unlike handing `reboundC` a literal key-value
// spelling, this LOCATES the value (key -> `:` -> the quoted value), so the
// rebind survives any re-alignment of the config's columns. A missing key is an
// ADD_FAILURE, never a silent no-op: a rebind whose `from` stopped matching
// would otherwise re-run the BASELINE schema and pass VACUOUSLY -- the exact
// failure mode that makes a config-driven test worthless.
[[nodiscard]] std::shared_ptr<GrammarSchema const>
reboundPreprocessWord(std::string const& key, std::string const& newWord,
                      std::string const& label) {
    std::string const text = loadShippedCText();
    if (text.empty()) {
        ADD_FAILURE() << "could not locate shipped c config";
        return nullptr;
    }
    std::string const quotedKey = "\"" + key + "\"";
    auto const keyPos = text.find(quotedKey);
    if (keyPos == std::string::npos) {
        ADD_FAILURE() << "shipped c config declares no " << quotedKey;
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
    return reboundC(text.substr(keyPos, closeQ + 1 - keyPos),
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
// in preprocessor.cpp AND `sbStackActive` walking every frame
// there too. Undoing either ALONE leaves the composite
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
// that subset while `#warning "Unsupported compiler detected"` — sys/cdefs.h,
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
    // (A) THE sys/cdefs.h SHAPE — operand is a lone string literal.
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
        auto r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "with the word rebound, `#error` is an unknown directive (P0015)";
        EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorErrorDirective))
            << "a hard-coded \"error\" literal would still fire P001E here";
    }
    {
        auto buf = SourceBuffer::fromString("#errr x\nint v=1;\n", "main.c");
        auto r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
        auto r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorUnsupported))
            << "with the word rebound, `#warning` is an unknown directive";
        EXPECT_FALSE(
            hasPPCode(r, DiagnosticCode::P_PreprocessorWarningDirective))
            << "a hard-coded \"warning\" literal would still fire P001F here";
    }
    {
        auto buf = SourceBuffer::fromString("#warnn x\nint v=1;\n", "main.c");
        auto r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
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
    out = preprocess(buf, schema, includeDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, fmt, noDefines, targetMacros);
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
    auto dir = ppScratchRoot() / "dss_tfc74_prescan_ifdef";
    std::error_code ec;
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
    // D-PP-PREDEFINE-REDEFINITION-PARTITION: STATED, not defaulted. A
    // function-like predefine has always been an ORDINARY macro (c105 lowers it
    // to a `<built-in>` `#define`, so it is `#undef`-able and carries no
    // predefined-name diagnostic), and the JSON loader now REFUSES any other
    // verb on such a row. A `PredefinedMacroDef` built here never passes
    // through that loader, so it would otherwise inherit the struct's
    // conservative default and misdescribe itself.
    fn.programRedefinition = PredefinedMacroRedefinition::Ordinary;
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
    // `ec` overload, like the other 50 cleanup calls in this file: a teardown
    // hiccup must not throw out of a test body that has already made every
    // assertion it exists to make. (This site and its twin below were the two
    // that still threw — MEASURED as `filesystem error: in remove_all: No such
    // file or directory` when a colliding sibling process removed the directory
    // first, back when the path was a constant.)
    fs::remove_all(dir, ec);
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
    auto dir = ppScratchRoot() / "dss_tfc74_prescan_value";
    std::error_code ec;
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
    fs::remove_all(dir, ec);   // `ec` overload — see the twin site above
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
    // D-PP-PREDEFINE-REDEFINITION-PARTITION: STATED, not defaulted. A
    // function-like predefine has always been an ORDINARY macro (c105 lowers it
    // to a `<built-in>` `#define`, so it is `#undef`-able and carries no
    // predefined-name diagnostic), and the JSON loader now REFUSES any other
    // verb on such a row. A `PredefinedMacroDef` built here never passes
    // through that loader, so it would otherwise inherit the struct's
    // conservative default and misdescribe itself.
    fn.programRedefinition = PredefinedMacroRedefinition::Ordinary;
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

// ── D-CONFIG-PREDEFINED-MACRO-ROW-KEYS-UNGATED ────────────────────────────
//
// A `predefinedMacros` ENTRY had no closed key vocabulary, so every optional
// key was a knob that could be misspelled into silence: `"paramss"` ships an
// OBJECT-like macro where a function-like one was declared,
// `"availabelObjectFormats"` predefines a pe-only spelling on EVERY format,
// and `"componentWeigths"` leaves the `version` kind's missing-field
// diagnostic as the only thing between a typo and a wrong encoding.
//
// The gate lives in the SHARED entry parser (`predefined_macro_json.cpp`), so
// all THREE declaring families inherit it. This suite pins the LANGUAGE family
// (`/preprocess/predefinedMacros`); `tests/core/test_target_schema.cpp` and
// `tests/link/test_object_format_schema.cpp` pin the other two, because "the
// shared parser gained a rule" is a claim about all of its callers.
//
// ★ DRIVEN THROUGH THE REAL INPUT PATH — the SHIPPED c document,
// spliced, never a hand-typed minimal stub. That matters twice over. The
// pristine arm is the INVERSE-FAILURE guard: a vocabulary enumerated from what
// the code READS rather than from what the shipped documents CONTAIN is
// exactly how the sibling target-loader gate turned 21 tests red on its first
// cut. And this is the one document that uses `$comment` INSIDE macro entries
// — seven times — so the `$`-prefix carve-out is witnessed on real data.
namespace {

// The shipped c text with ONE extra entry spliced into the FRONT of its
// `predefinedMacros` array. A whole extra entry (rather than a corrupted
// existing one) keeps the splice independent of the file's hand-aligned
// formatting, and keeps the probe's own name out of every other assertion.
[[nodiscard]] std::string cSubsetWithExtraMacroKey(std::string_view extraKey) {
    std::string       text  = loadShippedCText();
    const std::string anchor = "\"predefinedMacros\": [";
    const auto        pos    = text.find(anchor);
    if (pos == std::string::npos) return {};
    // `impliedSurface` is MANDATORY on every entry, in one of its three tagged
    // states (D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE, ruling B'),
    // so the probe entry must answer it. `claims-nothing`/`standard-defined` is
    // the honest answer for a synthetic probe macro that implies no platform
    // surface. Omitting it — or writing the older bare `null` — would make BOTH
    // tests that use this fixture fail for the wrong reason: the unknown-key
    // test would go green on an impliedSurface diagnostic instead of the key one
    // it is pinning, and the `$`-prefix carve-out test could not load at all.
    // D-PP-PREDEFINE-REDEFINITION-PARTITION added a SECOND mandatory key with
    // the same "no default, answer it out loud" contract, and it fails the same
    // two tests the same way when omitted. `ordinary` is the honest answer for a
    // synthetic probe: it is not a name ISO C 6.10.10 lists, and its value is a
    // static constant rather than one the engine derives.
    std::string entry = "{\"name\":\"__DSS_KEY_GATE_PROBE__\","
                        "\"kind\":\"constant\",\"value\":\"1\","
                        "\"programRedefinition\":\"ordinary\","
                        "\"impliedSurface\":{\"kind\":\"claims-nothing\","
                        "\"reason\":\"standard-defined\"},";
    entry += '"';
    entry += extraKey;
    entry += "\":\"1\"},";
    text.insert(pos + anchor.size(), entry);
    return text;
}

} // namespace

TEST(Preprocessor, PredefinedMacroEntryPristineShippedDocumentStillLoads) {
    const std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty()) << "could not locate shipped c config";
    auto loaded = GrammarSchema::loadFromText(text, "<pristine-c>");
    ASSERT_TRUE(loaded.has_value())
        << "the closed entry vocabulary must admit every key the SHIPPED "
           "document declares — including the seven `$comment`s inside macro "
           "entries. First diagnostic: "
        << (loaded.error().empty() ? "<none>" : loaded.error()[0].message);
}

// RED-ON-DISABLE: delete the rejection loop in `parsePredefinedMacroArray` and
// this load succeeds — which is the silent no-op the gate exists to stop.
TEST(Preprocessor, PredefinedMacroEntryUnknownKeyRejectedAndNamed) {
    const std::string text = cSubsetWithExtraMacroKey("vaule");
    ASSERT_FALSE(text.empty()) << "shipped c config no longer carries a "
                                  "`predefinedMacros` array to splice";
    auto loaded = GrammarSchema::loadFromText(text, "<typo-c>");
    ASSERT_FALSE(loaded.has_value())
        << "a misspelled entry key must be REFUSED — silently dropping it "
           "ships the macro with the very default the key was overriding";
    bool named = false;
    for (auto const& d : loaded.error()) {
        named = d.message.find("vaule") != std::string::npos
             && d.message.find("predefinedMacros") != std::string::npos;
        if (named) break;
    }
    EXPECT_TRUE(named)
        << "the diagnostic must name BOTH the offending key and the container "
           "it was found in — 'unknown key' alone sends the author hunting";
}

// The carve-out, on its own. Without it the gate would reject every shipped
// language document on first load, which is the inverse failure and the more
// expensive one.
TEST(Preprocessor, PredefinedMacroEntryDollarPrefixedKeyStillAccepted) {
    const std::string text = cSubsetWithExtraMacroKey("$valueComment");
    ASSERT_FALSE(text.empty());
    auto loaded = GrammarSchema::loadFromText(text, "<documented-c>");
    ASSERT_TRUE(loaded.has_value())
        << "`$`-prefixed keys are the codebase-wide documentation convention "
           "and must survive the typo discriminator — and the carve-out must "
           "be the PREFIX predicate, not a literal `$comment` compare, or "
           "`$valueComment` would be rejected as a typo. First diagnostic: "
        << (loaded.error().empty() ? "<none>" : loaded.error()[0].message);
}

// ══ D-PP-PREDEFINE-REDEFINITION-PARTITION — the LOADER's three refusals ═════
//
// `programRedefinition` is MANDATORY and its two structural rules are enforced
// at load. Each sub-case mutates the SHIPPED document by REMOVAL or by an
// impossible pairing and asserts the load REFUSES, naming the key.
// ⚠ REMOVAL, not addition, for the missing-key case: `loadCExpectingFailure`
// ADD_FAILUREs when its `from` text is absent, so a removal that finds nothing
// is LOUD — whereas an added key always "succeeds" and would keep reporting
// green over a document that had lost the field entirely.
TEST(Preprocessor, PredefineRedefinitionVerbIsMandatoryAndStructurallyChecked) {
    // (1) MISSING — remove the key from `__STDC__`'s row.
    {
        auto diags = loadCExpectingFailure(
            "\"value\": \"1\", \"programRedefinition\": \"warn-iso-macro\", "
            "\"impliedSurface\": { \"kind\": \"claims-nothing\", "
            "\"reason\": \"standard-defined\" } }",
            "\"value\": \"1\", "
            "\"impliedSurface\": { \"kind\": \"claims-nothing\", "
            "\"reason\": \"standard-defined\" } }",
            "<no-program-redefinition>");
        bool named = false;
        for (auto const& d : diags) {
            // The KEY may be named in the message text OR carried by the JSON
            // POINTER — a structural refusal points at the field and explains
            // the rule in prose, so searching only the message would miss it.
            if (d.message.find("programRedefinition") != std::string::npos
                || d.path.find("programRedefinition") != std::string::npos) {
                named = true;
                break;
            }
        }
        EXPECT_TRUE(named)
            << "an omitted `programRedefinition` must be REFUSED and NAMED: "
               "neither default is safe, so the question cannot be answered by "
               "omission";
    }
    // (2) A DERIVED kind declared `ordinary`. It would lower to `#define
    // __LINE__` — an EMPTY object-like macro — so the name would stay defined
    // and expand to nothing. That is a silently-evaporating predefine.
    {
        auto diags = loadCExpectingFailure(
            "\"name\": \"__LINE__\",            \"kind\": \"line\", "
            "\"programRedefinition\": \"warn-iso-macro\"",
            "\"name\": \"__LINE__\",            \"kind\": \"line\", "
            "\"programRedefinition\": \"ordinary\"",
            "<derived-kind-ordinary>");
        bool named = false;
        for (auto const& d : diags) {
            // The KEY may be named in the message text OR carried by the JSON
            // POINTER — a structural refusal points at the field and explains
            // the rule in prose, so searching only the message would miss it.
            if (d.message.find("programRedefinition") != std::string::npos
                || d.path.find("programRedefinition") != std::string::npos) {
                named = true;
                break;
            }
        }
        EXPECT_TRUE(named)
            << "a `line`/`file`/`date`/`time`/`counter` row has no static "
               "`value` to lower into the built-in prologue, so `ordinary` is "
               "unimplementable and must fail at LOAD, not at first use";
    }
    // (3) A FUNCTION-LIKE row declared with a warn verb. c105 lowers it to an
    // ordinary `<built-in>` `#define`, so it has ALWAYS been `#undef`-able with
    // no diagnostic — a warn verb there promises behaviour the engine has no
    // place to produce.
    {
        auto diags = loadCExpectingFailure(
            "\"params\": [\"x\"], \"availableObjectFormats\": [\"pe\"], "
            "\"programRedefinition\": \"ordinary\"",
            "\"params\": [\"x\"], \"availableObjectFormats\": [\"pe\"], "
            "\"programRedefinition\": \"warn-derived-macro\"",
            "<function-like-warn>");
        bool named = false;
        for (auto const& d : diags) {
            // The KEY may be named in the message text OR carried by the JSON
            // POINTER — a structural refusal points at the field and explains
            // the rule in prose, so searching only the message would miss it.
            if (d.message.find("programRedefinition") != std::string::npos
                || d.path.find("programRedefinition") != std::string::npos) {
                named = true;
                break;
            }
        }
        EXPECT_TRUE(named)
            << "a function-like predefine is an ordinary macro by construction; "
               "config must not be able to claim otherwise";
    }
}

// ── the COLLISION policy ──────────────────────────────────────────────────
// A name owned by BOTH config families is FATAL. Neither may silently win:
// picking either quietly is a wrong-value miscompile with no diagnostic.
TEST(Preprocessor, TFC74CollidingPredefineFailsLoudNamingBothPaths) {
    auto schema = cSubset();
    // `__LINE__` is declared by the shipped c language config.
    std::vector<PredefinedMacroDef> tms{targetMacro("__LINE__", "1")};
    auto buf = SourceBuffer::fromString("int x = 1;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string>           noDefines;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {},
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
// pe-GATED by the shipped c language config, so on an ELF target the
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
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {},
                                    ObjectFormatKind::Elf, noDefines, tms);
    EXPECT_TRUE(r.fatal);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::C_ConflictingPredefinedMacro))
        << "a pe-GATED language `_WIN32` must still collide with an UNGATED "
           "target `_WIN32` on an ELF build — gating decides which formats SEE "
           "a macro, not who OWNS the name";
}

// ── TF-C115: the ABORT PATH MUST STILL HONOUR THE RESULT CONTRACT ─────────
//    ([[D-PP-RESULT-CONTRACT-SINGLE-EXIT]])
//
// The two tests above prove the collision is DETECTED. They say nothing about
// whether the `PreprocessResult` handed back is USABLE — and it was not. The
// abort returned with `tokens` EMPTY and `synthBuffer` NULL, while
// `PreprocessResult` documents `tokens` as "Eof-terminated" and every consumer
// dereferences `synthBuffer`. `compilation_unit.cpp` did `pp.tokens.back()` on
// that empty vector, so the DOCUMENTED FAIL-LOUD DIAGNOSTIC WAS ACTUALLY A
// SEGFAULT: MEASURED before the fix, the CLI exited 139 (Linux) / 0xC0000005
// (Windows) printing NOTHING at all. The whole suite stayed green because
// nobody asserted the SHAPE of an aborting result.
//
// RED-ON-DISABLE: delete the `establishResultContract(...)` call from
// `preprocess()`'s single exit and every EXPECT below fails.
TEST(Preprocessor, TFC115CollisionAbortReturnsUsableResult) {
    auto schema = cSubset();
    std::vector<PredefinedMacroDef> tms{targetMacro("__LINE__", "1")};
    auto buf = SourceBuffer::fromString("int x = 1;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string>           noDefines;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {},
                                    ObjectFormatKind::Elf, noDefines, tms);

    // The abort is still an abort — the contract must not have quietly turned
    // the refusal into a successful (and silently one-sided) preprocess.
    ASSERT_TRUE(r.fatal);
    ASSERT_TRUE(hasPPCode(r, DiagnosticCode::C_ConflictingPredefinedMacro));

    // (1) Eof-termination. This is the exact byte the crash read past.
    ASSERT_FALSE(r.tokens.empty())
        << "an aborting preprocess must still return the Eof-terminated token "
           "vector `PreprocessResult` documents — an empty vector is what the "
           "consumer's `tokens.back()` dereferenced past the end of";
    EXPECT_EQ(r.tokens.back().coreKind, CoreTokenKind::Eof);
    EXPECT_EQ(r.tokens.size(), 1u)
        << "a refused pass produces the EMPTY translation unit: the Eof alone, "
           "never a partially-merged token stream a caller could mistake for a "
           "successful preprocess";
    // The checked read the consumer now uses must agree and must not abort.
    EXPECT_EQ(r.eofToken().coreKind, CoreTokenKind::Eof);

    // (2) every other field a consumer dereferences without asking first.
    ASSERT_NE(r.synthBuffer, nullptr)
        << "the Parser takes `synthBuffer` as its source and the CU sidecar "
           "stores it; a null here is the NEXT crash after the token one";
    EXPECT_TRUE(r.synthBuffer->text().empty());
    EXPECT_NE(r.mainSourceId, BufferId{});
    ASSERT_NE(r.diagnostics, nullptr);

    // (3) the diagnostic must be RENDERABLE, not just present. Its span is
    // stamped with the MAIN buffer's id, so that buffer has to be among the
    // origins the caller registers — otherwise the error prints
    // `--> <unknown-buffer:1>` and cannot say which file to fix (MEASURED:
    // exactly what it did before the origin invariant moved to the exit).
    bool mainIsAnOrigin = false;
    for (auto const& ob : r.originBuffers) {
        if (ob && ob->id() == buf->id()) mainIsAnOrigin = true;
    }
    EXPECT_TRUE(mainIsAnOrigin)
        << "the main source is an origin buffer of EVERY preprocess result — "
           "it is the input — so a diagnostic stamped with its id resolves";
}

// The SAME contract on a HAPPY-path result, as the matched control: the single
// exit must not have changed the shape of a successful preprocess (in
// particular it must not append a SECOND Eof, and must not duplicate the main
// buffer in `originBuffers`).
// RED-ON-DISABLE: make `establishResultContract` append its Eof
// unconditionally, or push the main buffer without the dedup scan.
TEST(Preprocessor, TFC115ContractExitLeavesHappyPathShapeUnchanged) {
    auto schema = cSubset();
    auto buf = SourceBuffer::fromString("int x = 1;\n", "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string>           noDefines;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {},
                                    ObjectFormatKind::Elf, noDefines, {});
    EXPECT_FALSE(r.fatal);
    ASSERT_FALSE(r.tokens.empty());
    EXPECT_EQ(r.tokens.back().coreKind, CoreTokenKind::Eof);
    // EXACTLY ONE Eof — a second one would be the exit double-terminating.
    std::size_t eofs = 0;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) ++eofs;
    }
    EXPECT_EQ(eofs, 1u);
    // The main buffer appears exactly once among the origins.
    std::size_t mainHits = 0;
    for (auto const& ob : r.originBuffers) {
        if (ob && ob->id() == buf->id()) ++mainHits;
    }
    EXPECT_EQ(mainHits, 1u)
        << "the exit's origin invariant must DEDUPE against what the run "
           "already collected from the line map, not append a duplicate";
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
            preprocess(legacyBuf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, fmt, noDefines);

        // NEW shape: explicitly empty target span.
        auto newBuf = SourceBuffer::fromString(kSrc, "main.c");
        std::vector<PredefinedMacroDef> none;
        PreprocessResult withEmpty =
            preprocess(newBuf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, fmt, noDefines, none);

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
    auto merged = mergePredefinedMacros(lang, tgt, {}, ObjectFormatKind::Elf);
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
    auto merged = mergePredefinedMacros(lang, tgt, {}, ObjectFormatKind::MachO);
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
    auto merged = mergePredefinedMacros(lang, tgt, {}, ObjectFormatKind::Elf);
    EXPECT_EQ(merged.conflicts.size(), 1u);
    EXPECT_TRUE(merged.effective.empty());
}

// ══ D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC — THE CO-DEFINITION PIN ══════
//
// DSS predefined `_MSC_VER=1943` for `pe` while `__GNUC__`/`__clang__` sat
// un-gated on every format, so a pe64 TU saw `_MSC_VER` AND `__GNUC__` AND
// `_WIN32` at once. ✔MEASURED (Ubuntu clang 19.1.1, `clang-19 -dM -E -x c
// /dev/null --target=<triple>`): `x86_64-pc-windows-msvc` defines `_MSC_VER`/
// `_MSC_FULL_VER`/`_MSC_EXTENSIONS`/`_WIN32`/`_WIN64`/`__clang__` and NOT
// `__GNUC__` — clang SUPPRESSES `__GNUC__` in MS-compatibility mode on purpose;
// `x86_64-w64-mingw32` defines `__GNUC__`/`_WIN32`/`_WIN64`/`__MINGW32__`/
// `__MINGW64__` and NOT `_MSC_VER`. No reference compiler presents the union.
//
// ★ WHAT THE FALSE IDENTITY COST: sqlite `src/hwtime.h` opens `#if
// defined(_MSC_VER) && defined(_WIN32)` -> `#include <profileapi.h>`, so pe64
// ALONE took that arm and died `error[F001A]` while the four other legs reached
// the `__GNUC__` rdtsc / `mrs cntvct_el0` arms. The identity kept is
// GNU-on-Windows, which is the half DSS actually implements.

namespace {
// The shipped `<arch>.target.json` / `<name>.format.json` STEMS, discovered from
// the config tree rather than listed here — a future target or format file is
// then covered the day it lands, which is the entire point of a pin whose job is
// to stop a REGRESSION nobody is looking for. A hard-coded list would pass
// forever while a new arm quietly re-co-defined the pair.
[[nodiscard]] std::vector<std::string> shippedStems(char const* subdir,
                                                    char const* suffix) {
    std::vector<std::string> out;
    std::error_code ec;
    auto const dir = test::configRoot() / subdir;
    for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
        auto const name = e.path().filename().string();
        if (name.size() > std::strlen(suffix)
            && name.ends_with(suffix)) {
            out.push_back(name.substr(0, name.size() - std::strlen(suffix)));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

// ★★★ THE PIN ITSELF, over the WHOLE shipped matrix: no target x format may
// make two members of a mutual-exclusion group effective at once.
//
// TARGET-AGNOSTIC BY CONSTRUCTION — the loop enumerates the config tree, so this
// is a statement about EVERY target DSS ships, present and future, not about
// pe64. The pe64 instance is merely the one that was caught in the field.
TEST(Preprocessor, PeIdentityNoShippedTargetFormatCoDefinesAnExclusiveGroup) {
    auto c = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(c.has_value());
    auto const& groups = (*c)->preprocess().mutuallyExclusivePredefinedMacros;
    ASSERT_FALSE(groups.empty())
        << "c must declare at least one mutual-exclusion group — with "
           "none, every assertion below is vacuous and the pin is disarmed";

    auto const targets = shippedStems("targets", ".target.json");
    auto const formats = shippedStems("object-formats", ".format.json");
    ASSERT_FALSE(targets.empty()) << "no shipped <arch>.target.json discovered";
    ASSERT_FALSE(formats.empty()) << "no shipped <name>.format.json discovered";

    // ★ EVERY arm runs. The body is a `void` callable precisely so a failing
    // ASSERT_* returns from the BODY and not from the test: one bad
    // target x format must not cancel the remaining ~48 and leave the operator
    // believing a single arm is the whole story.
    std::size_t checked = 0;
    auto checkOne = [&](std::string const& tname,
                        std::string const& fname) -> void {
        auto t = TargetSchema::loadShipped(tname);
        ASSERT_TRUE(t.has_value()) << "target " << tname << " must load";
        auto f = ObjectFormatSchema::loadShipped(fname);
        ASSERT_TRUE(f.has_value()) << "format " << fname << " must load";
        auto const merged = mergePredefinedMacros(
            (*c)->preprocess().predefinedMacros, (*t)->predefinedMacros(),
            (*f)->predefinedMacros(), (*f)->kind(), groups);
        std::string msgs;
        for (auto const& m : merged.conflicts) msgs += "\n    " + m;
        EXPECT_TRUE(merged.conflicts.empty())
            << "target=" << tname << " format=" << fname
            << " presents an identity no reference compiler can have:" << msgs;
        ++checked;
    };
    for (auto const& tname : targets) {
        for (auto const& fname : formats) checkOne(tname, fname);
    }
    EXPECT_EQ(checked, targets.size() * formats.size())
        << "every target x format pair must have been visited";
}

// ★★ NON-VACUITY: every group must have at least ONE member that some config
// actually declares, so the group is anchored to a real macro rather than being
// dead config that reads as protection.
//
// ★★★ WHY THIS IS "AT LEAST ONE" AND NOT "ALL", WHICH IS WHAT I WROTE FIRST AND
// WHICH THIS TEST ITSELF REFUTED. The obvious-looking invariant — every member
// must be declared somewhere — is WRONG, and wrong in the direction that would
// have forced the fix back out. A mutual-exclusion group is a PROHIBITION: its
// whole purpose here is to name a spelling DSS deliberately does NOT declare and
// must never start declaring again. Requiring every member to exist would make
// the rule unstatable exactly when it matters, and the only way to satisfy it
// would be to re-add the row this cycle removed. So the checkable property is
// that the group is ANCHORED (some member is real), not that it is fully
// populated.
//
// ⚠ WHAT THIS THEREFORE DOES NOT COVER, said plainly rather than left implied: a
// typo in the ABSENT member's spelling. Nothing that reads only the configs can
// catch it — an absent name and a misspelled absent name are the same bytes to
// every check. It is covered instead by
// `PeIdentityRedOnDisableRemovingTheGroupReadmitsTheCoDefinition`, which re-adds
// that exact spelling to the shipped text and requires the group to FIRE. Misspell
// it in the group and that test goes red, because arm 1 demands a conflict.
TEST(Preprocessor, PeIdentityEveryExclusionGroupIsAnchoredToADeclaredMacro) {
    auto c = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(c.has_value());
    auto const& groups = (*c)->preprocess().mutuallyExclusivePredefinedMacros;
    ASSERT_FALSE(groups.empty());

    // The UNION of every name any family declares, gates ignored — availability
    // is what the sweep above tests; existence is what this one tests.
    std::vector<std::string> declared;
    for (auto const& pm : (*c)->preprocess().predefinedMacros) {
        declared.push_back(pm.name);
    }
    for (auto const& tname : shippedStems("targets", ".target.json")) {
        auto t = TargetSchema::loadShipped(tname);
        ASSERT_TRUE(t.has_value());
        for (auto const& pm : (*t)->predefinedMacros()) declared.push_back(pm.name);
    }
    for (auto const& fname : shippedStems("object-formats", ".format.json")) {
        auto f = ObjectFormatSchema::loadShipped(fname);
        ASSERT_TRUE(f.has_value());
        for (auto const& pm : (*f)->predefinedMacros()) declared.push_back(pm.name);
    }

    for (auto const& g : groups) {
        std::size_t anchored = 0;
        std::string listed;
        for (auto const& want : g.macros) {
            listed += (listed.empty() ? "" : ", ") + want;
            if (std::ranges::find(declared, want) != declared.end()) ++anchored;
        }
        EXPECT_GT(anchored, 0u)
            << "mutual-exclusion group {" << listed << "} names NO macro that "
               "any language/target/format config declares. A group in which "
               "every member is absent can never fire under any target, so it "
               "is dead config that only looks like a rule — most likely every "
               "spelling is stale, or the group outlived the macros it governed";
    }
}

// ★ THE PIN FIRES, AND THE MESSAGE NAMES THE OFFENDING FORMAT. Synthetic
// config, because the shipped one must (and now does) be clean: a pin whose
// firing path is never exercised is a guess, not a guard.
TEST(Preprocessor, PeIdentityCoDefinitionIsRefusedAndNamesTheFormat) {
    std::vector<PredefinedMacroDef> lang{
        targetMacro("__GNUC__", "4"),               // un-gated, like the real one
        targetMacro("_MSC_VER", "1943", {"pe"})};   // pe-gated, like the real one
    std::vector<PredefinedMacroExclusionGroup> groups{
        {{"_MSC_VER", "__GNUC__"}, "clang suppresses __GNUC__ under -fms-compatibility"}};

    // On pe BOTH are effective -> refused, `effective` unusable.
    auto pe = mergePredefinedMacros(lang, {}, {}, ObjectFormatKind::Pe, groups);
    ASSERT_EQ(pe.conflicts.size(), 1u);
    EXPECT_TRUE(pe.effective.empty())
        << "a refused identity must leave NO partially-merged list a caller "
           "could mistake for usable";
    EXPECT_NE(pe.conflicts[0].find("_MSC_VER"), std::string::npos);
    EXPECT_NE(pe.conflicts[0].find("__GNUC__"), std::string::npos);
    EXPECT_NE(pe.conflicts[0].find(objectFormatKindName(ObjectFormatKind::Pe)),
              std::string::npos)
        << "the message must NAME the offending format — the whole defect class "
           "is a pair that is legitimate everywhere else, so a message that "
           "does not say WHICH leg leaves the reader to re-derive it: "
        << pe.conflicts[0];
    EXPECT_NE(pe.conflicts[0].find("-fms-compatibility"), std::string::npos)
        << "the group's `reason` must be quoted verbatim: " << pe.conflicts[0];

    // ★ THE OTHER HALF, which is what makes the check a pin and not a ban: on
    // elf the pe-gated member is filtered out, ONE member remains, and the very
    // same group is silent. Without this, gating the pair to disjoint formats
    // (the actual fix a maintainer would reach for) would be refused too.
    auto elf = mergePredefinedMacros(lang, {}, {}, ObjectFormatKind::Elf, groups);
    EXPECT_TRUE(elf.conflicts.empty())
        << "one effective member is not a co-definition — a group must forbid "
           "the OVERLAP, never the names";
    EXPECT_EQ(elf.effective.size(), 1u);
}

// ★★ RED-ON-DISABLE, at the CONFIG level and against the SHIPPED text. Re-add
// the deleted `_MSC_VER` row to the real c config: WITH the group the
// pe leg is refused, and with the group ALSO removed the impossible identity is
// silently accepted again — which is exactly the state this cycle found.
TEST(Preprocessor, PeIdentityRedOnDisableRemovingTheGroupReadmitsTheCoDefinition) {
    // The `_MSC_VER` row as it stood before this cycle, re-inserted ahead of the
    // `_WIN32` row it used to follow.
    //
    // ANCHORED ON THE ROW'S OPENING, NOT ON THE WHOLE ROW. The first cut spelled
    // `_WIN32`'s entry out in full and used that as the rebind anchor, so it
    // broke the moment an UNRELATED field was added to the row — which
    // `impliedSurface` did (D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE).
    // The anchor's only job is to name a UNIQUE insertion point; reproducing the
    // rest of the row pinned content this test says nothing about. It still
    // fails loud if the `_WIN32` row disappears, which is the real precondition.
    std::string const winRowOpen = "{ \"name\": \"_WIN32\",";
    // The re-inserted row must satisfy the SAME mandatory-`impliedSurface` rule
    // every other row does, or arm 1 would fail at the load for that reason
    // instead of exercising the exclusion group. `not-expressible` is the
    // honest tag for a historical spelling re-created by a pin: it is recorded,
    // never evaluated, so it cannot itself pass or fail a surface check.
    std::string const withMsc =
        "{ \"name\": \"_MSC_VER\",            \"kind\": \"constant\", "
        "\"value\": \"1943\", \"availableObjectFormats\": [\"pe\"], "
        // D-PP-PREDEFINE-REDEFINITION-PARTITION: a second mandatory key with no
        // default. `ordinary` is what the row would carry if it still shipped —
        // a compiler-identity spelling is not an ISO 6.10.10 name.
        "\"programRedefinition\": \"ordinary\", "
        "\"impliedSurface\": { \"kind\": \"not-expressible\", \"note\": "
        "\"the historical row, re-created by this red-on-disable pin only\" } },\n      "
        + winRowOpen;

    {   // ARM 1 — group PRESENT: the co-definition is REFUSED on pe.
        auto schema = reboundC(winRowOpen, withMsc, "<msc-ver-readded>");
        ASSERT_NE(schema, nullptr);
        auto const& groups = schema->preprocess().mutuallyExclusivePredefinedMacros;
        ASSERT_FALSE(groups.empty()) << "arm 1 must still carry the group";
        auto merged = mergePredefinedMacros(schema->preprocess().predefinedMacros,
                                            {}, {}, ObjectFormatKind::Pe, groups);
        EXPECT_EQ(merged.conflicts.size(), 1u)
            << "with _MSC_VER back alongside the un-gated __GNUC__, pe must be "
               "refused";
        // ...and the OTHER legs stay green, proving the refusal is per-format.
        EXPECT_TRUE(mergePredefinedMacros(schema->preprocess().predefinedMacros,
                                          {}, {}, ObjectFormatKind::Elf, groups)
                        .conflicts.empty());
    }
    {   // ARM 2 — group REMOVED: the SAME config is silently accepted. This is
        // the disable arm; if it ever starts failing, the check has grown a
        // second, un-configured source of truth and is no longer config-driven.
        std::string text = loadShippedCText();
        ASSERT_FALSE(text.empty());
        auto const wpos = text.find(winRowOpen);
        ASSERT_NE(wpos, std::string::npos);
        text.replace(wpos, winRowOpen.size(), withMsc);
        auto const gpos = text.find("\"mutuallyExclusivePredefinedMacros\"");
        ASSERT_NE(gpos, std::string::npos)
            << "the shipped config must carry the group for this arm to mean "
               "anything";
        auto const gend = text.find("],", gpos);
        ASSERT_NE(gend, std::string::npos);
        text.replace(gpos, gend + 2 - gpos, "\"$disabledGroups\": [],");
        auto loaded = GrammarSchema::loadFromText(text, "<no-exclusion-groups>");
        ASSERT_TRUE(loaded.has_value())
            << "removing an OPTIONAL key must still load — that is what makes "
               "this a real disable arm rather than a load error";
        auto const& groups = (*loaded)->preprocess().mutuallyExclusivePredefinedMacros;
        ASSERT_TRUE(groups.empty());
        auto merged = mergePredefinedMacros((*loaded)->preprocess().predefinedMacros,
                                            {}, {}, ObjectFormatKind::Pe, groups);
        EXPECT_TRUE(merged.conflicts.empty())
            << "WITHOUT the group the impossible identity must be accepted "
               "again — this arm is the proof that the shipped group, and not "
               "some engine-side hard-coding of the two names, is what refuses "
               "it";
        // And it really is co-defined in this arm — otherwise arm 1 proves
        // nothing either.
        auto has = [&](char const* n) {
            return std::ranges::find(merged.effective, std::string{n},
                                     &PredefinedMacroDef::name)
                   != merged.effective.end();
        };
        EXPECT_TRUE(has("_MSC_VER"));
        EXPECT_TRUE(has("__GNUC__"));
    }
}

// ★ THE IDENTITY ITSELF, on the SHIPPED config: `_MSC_VER` effective NOWHERE,
// `__GNUC__` effective EVERYWHERE. The sweep above proves the pair never
// overlaps; this proves WHICH of the two survived, which is the decision the
// cycle actually made and the thing a well-meaning "restore MSVC compatibility"
// edit would undo.
TEST(Preprocessor, PeIdentityShippedConfigIsGnuOnWindowsNotMsvc) {
    auto c = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(c.has_value());
    for (ObjectFormatKind k : {ObjectFormatKind::Pe, ObjectFormatKind::Elf,
                               ObjectFormatKind::MachO}) {
        auto const merged = mergePredefinedMacros(
            (*c)->preprocess().predefinedMacros, {}, {}, k,
            (*c)->preprocess().mutuallyExclusivePredefinedMacros);
        ASSERT_TRUE(merged.conflicts.empty());
        auto has = [&](char const* n) {
            return std::ranges::find(merged.effective, std::string{n},
                                     &PredefinedMacroDef::name)
                   != merged.effective.end();
        };
        EXPECT_FALSE(has("_MSC_VER"))
            << objectFormatKindName(k)
            << ": `_MSC_VER` must be defined on NO leg — DSS implements GNU "
               "extended inline asm, which MSVC x64 does not have at all, and "
               "spells __declspec/__cdecl/__stdcall as MACROS, which is the "
               "MinGW shape and not the MSVC one";
        EXPECT_TRUE(has("__GNUC__"))
            << objectFormatKindName(k)
            << ": `__GNUC__` is the language's un-gated base identity and must "
               "survive on every leg";
    }
    // `_WIN32` is the HONEST half and stays: it names the OS, not the toolchain,
    // and it is what selects sqlite's os_win.c.
    auto const pe = mergePredefinedMacros((*c)->preprocess().predefinedMacros,
                                          {}, {}, ObjectFormatKind::Pe, {});
    EXPECT_NE(std::ranges::find(pe.effective, std::string{"_WIN32"},
                                &PredefinedMacroDef::name),
              pe.effective.end())
        << "removing `_MSC_VER` must not have taken `_WIN32` with it";
}

// ── LOADER shape guards. Each rejected form is one that would sit in config
//    looking like a rule while being incapable of ever firing. ──────────────
TEST(Preprocessor, PeIdentityExclusionGroupShapeIsValidatedAtLoad) {
    std::string const good = "\"mutuallyExclusivePredefinedMacros\": [";
    struct Case {
        char const* label;
        char const* replacement;
    };
    // A one-name group can never fire; a missing `reason` makes the refusal
    // unexplainable; a repeated name is a name excluding itself.
    for (Case const& c : {Case{"one-name group",
                               "\"mutuallyExclusivePredefinedMacros\": ["
                               "{\"reason\":\"r\",\"macros\":[\"ONLY\"]}],\"$x\": ["},
                          Case{"missing reason",
                               "\"mutuallyExclusivePredefinedMacros\": ["
                               "{\"macros\":[\"A\",\"B\"]}],\"$x\": ["},
                          Case{"duplicate name",
                               "\"mutuallyExclusivePredefinedMacros\": ["
                               "{\"reason\":\"r\",\"macros\":[\"A\",\"A\"]}],\"$x\": ["}}) {
        std::string text = loadShippedCText();
        ASSERT_FALSE(text.empty());
        auto const pos = text.find(good);
        ASSERT_NE(pos, std::string::npos);
        text.replace(pos, good.size(), c.replacement);
        auto loaded = GrammarSchema::loadFromText(text, "<bad-exclusion-group>");
        EXPECT_FALSE(loaded.has_value())
            << c.label << " must be a LOAD error — a group that cannot fire is "
                          "config that reads as protection and asserts nothing";
    }
}

// ── shipped-config sibling pins: language ⊕ arm64 and language ⊕ x86_64 ───
// The EFFECTIVE arch-identity set a real macho/elf build sees. EXACT SETS, not
// counts — the whole point of the cycle is which SPELLINGS reach the source.
TEST(Preprocessor, TFC74EffectiveArchPredefinesForShippedTargets) {
    auto c = GrammarSchema::loadShipped("c");
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
        return mergePredefinedMacros((*c)->preprocess().predefinedMacros, {}, {}, fmt)
            .effective.size();
    };

    // arm64 on MACHO: all four spellings, including the Apple-only pair.
    {
        auto m = mergePredefinedMacros((*c)->preprocess().predefinedMacros,
                                       (*arm)->predefinedMacros(), {},
                                       ObjectFormatKind::MachO);
        ASSERT_TRUE(m.conflicts.empty())
            << "the shipped language and arm64 configs must not collide";
        EXPECT_EQ(namesOfTargetHalf(m, langSurviving(ObjectFormatKind::MachO)),
                  (std::vector<std::string>{"__ARM_ARCH_ISA_A64", "__BYTE_ORDER__",
                                            "__LITTLE_ENDIAN__", "__aarch64__",
                                            "__arm64", "__arm64__"}))
            << "TF-C115 (D-PP-ENDIANNESS-PREDEFINES): the two endianness rows "
               "are UNGATED, so they appear on macho alongside the Apple-only "
               "identity pair; `__BIG_ENDIAN__` must appear on NO leg";
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
                                       (*arm)->predefinedMacros(), {},
                                       ObjectFormatKind::Elf);
        ASSERT_TRUE(m.conflicts.empty());
        EXPECT_EQ(namesOfTargetHalf(m, langSurviving(ObjectFormatKind::Elf)),
                  (std::vector<std::string>{"__ARM_ARCH_ISA_A64",
                                            "__BYTE_ORDER__",
                                            "__CHAR_UNSIGNED__",
                                            "__LITTLE_ENDIAN__",
                                            "__aarch64__"}))
            << "`__arm64__`/`__arm64` are Apple-only and must NOT leak onto "
               "ELF, while `__CHAR_UNSIGNED__` is ELF-only and MUST appear "
               "there — it is the preprocessor face of the target's "
               "`charIsUnsigned` default, which macho/pe override to signed. "
               "TF-C115: the two endianness rows are UNGATED and therefore "
               "appear on BOTH legs — the negative that matters is "
               "`__BIG_ENDIAN__`, which appears on none";
    }
    // x86_64: the same four spellings on every format.
    for (ObjectFormatKind fmt : {ObjectFormatKind::Elf, ObjectFormatKind::MachO,
                                 ObjectFormatKind::Pe}) {
        auto m = mergePredefinedMacros((*c)->preprocess().predefinedMacros,
                                       (*x86)->predefinedMacros(), {}, fmt);
        ASSERT_TRUE(m.conflicts.empty())
            << "the shipped language and x86_64 configs must not collide";
        EXPECT_EQ(namesOfTargetHalf(m, langSurviving(fmt)),
                  (std::vector<std::string>{"__BYTE_ORDER__", "__LITTLE_ENDIAN__",
                                            "__amd64", "__amd64__", "__x86_64",
                                            "__x86_64__"}))
            << "TF-C115: x86_64 is little-endian under elf64, macho64 AND pe64, "
               "so the two endianness rows are UNGATED and identical on all "
               "three — the property `__LP64__` did NOT have (LP64 on elf/macho, "
               "LLP64 on pe), which is why that one lives on the object format "
               "and these live here";
    }
}

// TF-C115 (D-PP-ENDIANNESS-PREDEFINES) — THE CROSS-LAYER PIN.
//
// `__BYTE_ORDER__` is declared on the TARGET but its value NAMES
// `__ORDER_LITTLE_ENDIAN__`, which is declared on the LANGUAGE. Two things must
// hold and neither is implied by the per-layer exact sets above:
//   (a) the reference RESOLVES — the value text must be exactly the vocabulary
//       name the language declares, so `#if __BYTE_ORDER__ ==
//       __ORDER_LITTLE_ENDIAN__` is a comparison of two DEFINED integers rather
//       than C 6.10.1p4's `0 == 0`;
//   (b) `__BIG_ENDIAN__` is declared by NO layer, on NO format. That is not a
//       missing feature, it is the declaration that DSS ships no big-endian
//       target (MEASURED 2026-08-04: clang defines it only for
//       aarch64_be-linux-gnu), and Apple's libkern/OSByteOrder.h tests it
//       BEFORE the little-endian arm, so a stray row silently selects
//       byte-swapping macros on a little-endian machine.
TEST(Preprocessor, TFC115EndiannessPredefinesCrossLayerCoherence) {
    auto c = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(c.has_value());
    for (char const* arch : {"arm64", "x86_64"}) {
        auto t = TargetSchema::loadShipped(arch);
        ASSERT_TRUE(t.has_value()) << arch;
        for (ObjectFormatKind fmt : {ObjectFormatKind::Elf, ObjectFormatKind::MachO,
                                     ObjectFormatKind::Pe}) {
            auto m = mergePredefinedMacros((*c)->preprocess().predefinedMacros,
                                           (*t)->predefinedMacros(), {}, fmt);
            ASSERT_TRUE(m.conflicts.empty()) << arch;
            std::optional<std::string> byteOrderValue;
            std::optional<std::string> orderLittleValue;
            bool sawBigEndianSpelling = false;
            for (auto const& pm : m.effective) {
                if (pm.name == "__BYTE_ORDER__")           byteOrderValue   = pm.value;
                if (pm.name == "__ORDER_LITTLE_ENDIAN__")  orderLittleValue = pm.value;
                if (pm.name == "__BIG_ENDIAN__")           sawBigEndianSpelling = true;
            }
            ASSERT_TRUE(byteOrderValue.has_value())
                << arch << ": __BYTE_ORDER__ must be in the effective set on every format";
            ASSERT_TRUE(orderLittleValue.has_value())
                << arch << ": __ORDER_LITTLE_ENDIAN__ must be in the effective set";
            EXPECT_EQ(*byteOrderValue, "__ORDER_LITTLE_ENDIAN__")
                << arch << ": __BYTE_ORDER__'s body must NAME the language-declared "
                           "vocabulary constant, not restate its literal — the "
                           "reference is what keeps the two layers from drifting";
            EXPECT_EQ(*orderLittleValue, "1234")
                << arch << ": the vocabulary constant must carry its measured value";
            EXPECT_FALSE(sawBigEndianSpelling)
                << arch << ": __BIG_ENDIAN__ must be declared by NO layer on ANY "
                           "format — DSS ships no big-endian target, and Apple's "
                           "libkern/OSByteOrder.h tests this spelling FIRST";
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// TF-C86 (D-CSUBSET-STDARG-F001A) — the conditional-inclusion OPERATORS are
// DEFINED NAMES, and are not names a program may take over.
//
// WHAT WAS BROKEN. `sbNameDefined` (pre-scan) and `MacroExpander::isDefined`
// (authoritative) both answered FALSE for `__has_include`. The universal
// portability shim
//     #ifndef __has_include
//     #define __has_include(x) 0
//     #endif
// — Apple SDK `sys/cdefs.h`, and the same three lines in glibc, musl,
// Boost — therefore went LIVE, shadowing an operator DSS actually implements
// with a function-like macro that answers 0 forever. The damage was not the 0:
// it was that the pre-scan's FIX-3 arm (preprocessor.cpp, "a function-like-macro
// invocation in the guard is NOT evaluated by this weaker pre-scan") then went
// conservative-UNCERTAIN on every `#if __has_include(<h>)`, which skipped the
// angle SOURCE splice, which left the directive verbatim, which made the
// post-parse import resolver hard-fail it as `F001A` — a MISSING-HEADER error
// for headers sitting readable on the include path. MEASURED on the sqlite
// corpus at bb75fb8: 5 of the 7 macho `F001A` were this, all of them reached
// through `malloc/_platform.h`.
//
// THE CONTRACT HAS FORMS, AND EVERY FORM IS TESTED BELOW: three declared
// operators x {#ifdef, #ifndef, defined(), !defined(), #elifdef, #elifndef},
// plus the `defined` NEGATIVE pin, plus the end-to-end splice witness, plus
// both `#define` and `#undef` refusals, plus the config opt-out.
// ════════════════════════════════════════════════════════════════════════════

// The three spellings the shipped c declares. Read from CONFIG, so a
// grammar that renames one is covered and this test cannot drift from the
// engine's own notion of the set.
namespace {
[[nodiscard]] std::vector<std::string> tfc86DeclaredOperators() {
    // D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE: NAME the owning handle. The
    // one-liner `auto const& pp = cSubset()->preprocess();` binds a reference
    // INTO a schema owned only by a temporary `shared_ptr`, which dies at the
    // end of that full-expression -> `pp` dangles for every read below.
    auto schema = cSubset();
    auto const& pp = schema->preprocess();
    std::vector<std::string> names;
    for (std::string const* s : {&pp.hasIncludeOperator, &pp.hasEmbedOperator,
                                 &pp.hasCAttributeOperator}) {
        if (!s->empty()) names.push_back(*s);
    }
    return names;
}
} // namespace

// FORM 1-4: `#ifdef` / `#ifndef` / `#if defined()` / `#if !defined()`.
// MEASURED against the host toolchain (`clang -std=c2x -E`): `#ifdef
// __has_include` is TAKEN, and likewise `__has_embed` and `__has_c_attribute`.
// RED-ON-DISABLE: drop the `isConditionalInclusionOperator` arm from
// `MacroExpander::isDefined` -> every `yes` below becomes `no`.
TEST(Preprocessor, TFC86ConditionalInclusionOperatorsAreDefinedEveryForm) {
    auto const ops = tfc86DeclaredOperators();
    ASSERT_EQ(ops.size(), 3u)
        << "the shipped c must declare all three operators; if one was "
           "removed this test is measuring less than it claims";
    for (std::string const& op : ops) {
        {   // FORM 1 — #ifdef NAME -> taken
            PreprocessResult r;
            auto lexs = ppLexemes(
                "#ifdef " + op + "\nint yes;\n#else\nint no;\n#endif\n", r);
            EXPECT_FALSE(r.diagnostics->hasErrors()) << op;
            ASSERT_EQ(lexs.size(), 3u) << op;
            EXPECT_EQ(lexs[1], "yes")
                << "#ifdef " << op << " must be TAKEN — the implementation "
                   "provides the operator";
        }
        {   // FORM 2 — #ifndef NAME -> NOT taken. This is the exact shape of
            //          the shim that caused the F001A cascade.
            PreprocessResult r;
            auto lexs = ppLexemes(
                "#ifndef " + op + "\nint yes;\n#else\nint no;\n#endif\n", r);
            EXPECT_FALSE(r.diagnostics->hasErrors()) << op;
            ASSERT_EQ(lexs.size(), 3u) << op;
            EXPECT_EQ(lexs[1], "no")
                << "#ifndef " << op << " must NOT be taken — this is the arm "
                   "the sys/cdefs.h shadowing shim lives in";
        }
        {   // FORM 3 — #if defined(NAME) -> true
            PreprocessResult r;
            auto lexs = ppLexemes(
                "#if defined(" + op + ")\nint yes;\n#else\nint no;\n#endif\n", r);
            EXPECT_FALSE(r.diagnostics->hasErrors()) << op;
            ASSERT_EQ(lexs.size(), 3u) << op;
            EXPECT_EQ(lexs[1], "yes")
                << "defined(" << op << ") must agree with #ifdef " << op;
        }
        {   // FORM 4 — #if !defined(NAME) -> false (the inverted polarity: a
            //          predicate that only fixed the positive direction reds here)
            PreprocessResult r;
            auto lexs = ppLexemes(
                "#if !defined(" + op + ")\nint yes;\n#else\nint no;\n#endif\n", r);
            EXPECT_FALSE(r.diagnostics->hasErrors()) << op;
            ASSERT_EQ(lexs.size(), 3u) << op;
            EXPECT_EQ(lexs[1], "no") << "!defined(" << op << ") must be FALSE";
        }
    }
}

// FORMS 5-6: the C23 `#elifdef` / `#elifndef` spellings route through the SAME
// definedness callbacks (`handleElif`, "the definedness callbacks are the SAME
// ones handleIf binds"). Tested separately anyway: "routes through the same
// code" is a claim, and an untested claim is how a multi-form contract ends up
// half-implemented.
TEST(Preprocessor, TFC86ConditionalInclusionOperatorsAreDefinedInElifdefForms) {
    for (std::string const& op : tfc86DeclaredOperators()) {
        {   // #elifdef NAME -> taken (the leading #if is false)
            PreprocessResult r;
            auto lexs = ppLexemes(
                "#if 0\nint first;\n#elifdef " + op
                + "\nint yes;\n#else\nint no;\n#endif\n", r);
            EXPECT_FALSE(r.diagnostics->hasErrors()) << op;
            ASSERT_EQ(lexs.size(), 3u) << op;
            EXPECT_EQ(lexs[1], "yes") << "#elifdef " << op << " must be TAKEN";
        }
        {   // #elifndef NAME -> NOT taken
            PreprocessResult r;
            auto lexs = ppLexemes(
                "#if 0\nint first;\n#elifndef " + op
                + "\nint yes;\n#else\nint no;\n#endif\n", r);
            EXPECT_FALSE(r.diagnostics->hasErrors()) << op;
            ASSERT_EQ(lexs.size(), 3u) << op;
            EXPECT_EQ(lexs[1], "no")
                << "#elifndef " << op << " must NOT be taken";
        }
    }
}

// ★ THE NEGATIVE PIN. `defined` is an OPERATOR SPELLING, not a macro name, and
// `definedOperator` is deliberately NOT a member of
// `isConditionalInclusionOperator`. MEASURED on the host clang: `#ifdef defined`
// is NOT taken. RED-ON-DISABLE: add `definedOperator` to the predicate -> this
// reds. Without this test an over-broad predicate would pass every assertion
// above while quietly changing what `#ifdef defined` means.
TEST(Preprocessor, TFC86DefinedOperatorItselfIsNotADefinedName) {
    // D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE: see the note in
    // `tfc86DeclaredOperators` — the owning handle must outlive `pp`.
    auto schema = cSubset();
    auto const& pp = schema->preprocess();
    ASSERT_FALSE(pp.definedOperator.empty());
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#ifdef " + pp.definedOperator + "\nint yes;\n#else\nint no;\n#endif\n", r);
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "no")
        << "`" << pp.definedOperator << "` is an operator spelling, NOT a macro "
           "name — #ifdef of it must be FALSE (measured: clang agrees)";
}

// ★★ THE END-TO-END WITNESS — the property the whole cycle is about, and the
// one no definedness assertion above can see. Reproduces `sys/cdefs.h`'s shim
// VERBATIM, then guards a real angle SOURCE include with `__has_include`, and
// requires the header to be TEXTUALLY SPLICED (its macro must EXPAND — a
// symbol-only cross-ref would carry no macros).
//
// RED-ON-DISABLE, MEASURED: with the `isDefined` arm reverted, the shim goes
// live, `__has_include` becomes a function-like macro, the pre-scan's FIX-3 arm
// bails uncertain, the splice is skipped, `MARKER_OK` never expands and the
// directive's `#` survives verbatim — which is exactly the state that produced
// `F001A: got mach/boolean.h` on a header that was present the whole time.
TEST(Preprocessor, TFC86PortabilityShimDoesNotShadowTheOperatorEndToEnd) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_tfc86_shim_endtoend";
    std::error_code ec;
    fs::remove_all(inc, ec);
    fs::create_directories(inc);
    { std::ofstream(inc / "guarded.h", std::ios::binary)
        << "#define MARKER_OK 4242\nint guarded_sym;\n"; }

    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        // The shim, byte-for-byte as the SDK writes it ...
        "#ifndef __has_include\n"
        "#define __has_include(x) 0\n"
        "#endif\n"
        // ... then the guarded include it is supposed to leave alone.
        "#if __has_include(<guarded.h>)\n"
        "#include <guarded.h>\n"
        "#endif\n"
        "int u = MARKER_OK;\n",
        r, {inc}, {});

    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "the shim is DEAD code here (its #ifndef is false), so nothing in "
           "this TU may error";
    EXPECT_FALSE(hasPPCode(
        r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable))
        << "the GUARDED shim must never trip the redefinition refusal — the "
           "belt must not break the world's most common portability idiom";
    auto has = [&](std::string_view s) {
        for (auto const& l : lexs) if (l == s) return true;
        return false;
    };
    EXPECT_TRUE(has("guarded_sym"))
        << "the guarded header must be textually spliced despite the shim";
    EXPECT_TRUE(has("4242"))
        << "MARKER_OK must EXPAND — proving a real textual splice, which is the "
           "property F001A's cascade destroyed";
    EXPECT_FALSE(has("#"))
        << "the include directive must be consumed, not left verbatim for the "
           "import resolver to hard-fail as a missing system header";
    fs::remove_all(inc, ec);
}

// The `#define` refusal, for EVERY declared operator. An UNGUARDED shadow is a
// silent miscompile (the guard would answer 0 while `#include` still splices),
// so it is refused loudly and the operator KEEPS working.
TEST(Preprocessor, TFC86UnguardedDefineOfOperatorFailsLoudEveryOperator) {
    for (std::string const& op : tfc86DeclaredOperators()) {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define " + op + "(x) 0\n"
            "#ifdef " + op + "\nint yes;\n#else\nint no;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(
            r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable))
            << "#define " << op << " must fail LOUD";
        ASSERT_EQ(lexs.size(), 3u) << op;
        EXPECT_EQ(lexs[1], "yes")
            << op << " must STILL be the operator after the refusal — a "
                     "refusal that also dropped the name would leave the "
                     "program in the shadowed state it was refused for";
    }
}

// The `#undef` half. Symmetric, and equally load-bearing: an `#undef` that
// SUCCEEDED would turn the operator back into an ordinary identifier folding
// to 0 — the same include-vs-guard disagreement by a different route.
TEST(Preprocessor, TFC86UndefOfOperatorFailsLoudEveryOperator) {
    for (std::string const& op : tfc86DeclaredOperators()) {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#undef " + op + "\n"
            "#ifdef " + op + "\nint yes;\n#else\nint no;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(
            r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable))
            << "#undef " << op << " must fail LOUD";
        ASSERT_EQ(lexs.size(), 3u) << op;
        EXPECT_EQ(lexs[1], "yes")
            << op << " must survive the refused #undef";
    }
}

// The refusal is UNSUPPRESSABLE: `--suppress` of it would restore exactly the
// silent include-vs-guard disagreement it exists to prevent.
TEST(Preprocessor, TFC86OperatorNameRefusalIsUnsuppressable) {
    EXPECT_TRUE(isUnsuppressable(
        DiagnosticCode::P_PreprocessorOperatorNameNotDefinable))
        << "suppressing this code re-opens the silent miscompile channel";
}

// AGNOSTICISM (opt-OUT). With `hasIncludeOperator` stripped from config, DSS no
// longer provides the operator — so `#ifndef __has_include` becomes TRUE and the
// shim is correct to fire, exactly as it is on a compiler that lacks it. Pins
// that the definedness comes from the CONFIG-DECLARED set and not from a
// hard-coded `__has_include` lexeme. RED-ON-DISABLE: hard-code the name in
// `isConditionalInclusionOperator` -> the stripped grammar still reports it
// defined -> `no`.
TEST(Preprocessor, TFC86OperatorDefinednessIsConfigDrivenNotHardcoded) {
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty());
    for (std::string const& line :
         {std::string{"\"hasIncludeOperator\":       \"__has_include\",\n"},
          std::string{"    \"hasIncludeAngleOpenToken\":  \"LtOp\",\n"},
          std::string{"    \"hasIncludeAngleCloseToken\": \"GtOp\",\n"}}) {
        auto const pos = text.find(line);
        ASSERT_NE(pos, std::string::npos) << "config no longer carries: " << line;
        text.erase(pos, line.size());
    }
    auto loaded = GrammarSchema::loadFromText(text, "<no-has-include-c>");
    ASSERT_TRUE(loaded.has_value());
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    ASSERT_TRUE(schema->preprocess().hasIncludeOperator.empty());

    namespace fs = std::filesystem;
    auto buf = SourceBuffer::fromString(
        std::string{"#ifndef __has_include\nint yes;\n#else\nint no;\n#endif\n"},
        "main.c");
    std::vector<fs::path> noDirs;
    PreprocessResult r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(r.diagnostics->hasErrors());
    std::vector<std::string> lexs;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
    }
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "yes")
        << "with the operator stripped from CONFIG, DSS does not provide it, so "
           "#ifndef __has_include is TRUE and the portability shim correctly "
           "fires — the definedness must follow config, never a baked-in name";
}

// ★ THE PRE-SCAN'S OWN PIN — added because the first red-on-disable pass
// MEASURED that it was MISSING. Reverting `SynthBuilder::sbNameDefined`'s
// operator arm left all eight tests above GREEN, because the pre-scan carries
// TWO independent guards (this one, and the define-TRACKING refusal that keeps
// a shadowing `#define` out of `localMacros`) and every test above is satisfied
// by the second one alone. A guard whose assertion is already met by a
// DIFFERENT mechanism on the tested path is not a guard.
//
// This input isolates `sbNameDefined` exactly: the include's liveness is
// decided BY the operator's definedness, and no `#define` of the operator
// appears anywhere, so the define-tracking arm is inert here.
//
// With `sbNameDefined` correct, the pre-scan reads `#ifdef __has_include` LIVE,
// `includeResolvable()` is true, and the angle SOURCE arm splices `probe.h`
// textually — so `PRESCAN_MARKER` EXPANDS. Reverted, the pre-scan reads the
// group DEAD, skips the splice, leaves the directive verbatim, and the marker
// never expands (while the authoritative pass, which has its own correct
// `isDefined`, considers the branch live — the pre-scan/authoritative
// DIVERGENCE that the one-directional-lockstep invariant forbids).
//
// RED-ON-DISABLE: MEASURED — replacing `sbNameDefined`'s operator arm with
// `return false;` reds this test and only this test.
TEST(Preprocessor, TFC86PreScanDefinednessGatesTheAngleSourceSplice) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_tfc86_prescan_gate";
    std::error_code ec;
    fs::remove_all(inc, ec);
    fs::create_directories(inc);
    { std::ofstream(inc / "probe.h", std::ios::binary)
        << "#define PRESCAN_MARKER 9191\nint prescan_sym;\n"; }

    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        // No `#define` of the operator ANYWHERE — the define-tracking arm
        // cannot be what makes this pass.
        "#ifdef __has_include\n"
        "#include <probe.h>\n"
        "#endif\n"
        "int u = PRESCAN_MARKER;\n",
        r, {inc}, {});

    EXPECT_FALSE(r.diagnostics->hasErrors());
    auto has = [&](std::string_view s) {
        for (auto const& l : lexs) if (l == s) return true;
        return false;
    };
    EXPECT_TRUE(has("prescan_sym"))
        << "the PRE-SCAN must read `#ifdef __has_include` as LIVE, or it never "
           "splices the header the authoritative pass then believes is there";
    EXPECT_TRUE(has("9191"))
        << "PRESCAN_MARKER must EXPAND — only a real textual splice carries the "
           "header's macros, and only a live-reading pre-scan performs one";
    EXPECT_FALSE(has("#"))
        << "the directive must be consumed by the pre-scan, not survive to be "
           "hard-failed downstream as a missing system header";
    fs::remove_all(inc, ec);
}

// The pre-scan's SECOND guard, pinned on its own for the same reason. Here the
// shadowing `#define` IS present but UNGUARDED, so `sbNameDefined` cannot be
// what saves it (the operator is defined either way): what must hold is that
// the pre-scan REFUSES to record the operator into `localMacros`. If it
// recorded it, FIX-3 would see a function-like macro in the following
// `#if __has_include(<...>)` guard, go conservative-uncertain, and skip the
// splice — reproducing the F001A cascade with the definedness fix in place.
//
// RED-ON-DISABLE: MEASURED — dropping the `isConditionalInclusionOperator`
// filter from the pre-scan's `#define` arm reds this test.
TEST(Preprocessor, TFC86PreScanRefusesToRecordAShadowingOperatorDefine) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_tfc86_prescan_norecord";
    std::error_code ec;
    fs::remove_all(inc, ec);
    fs::create_directories(inc);
    { std::ofstream(inc / "norec.h", std::ios::binary)
        << "#define NOREC_MARKER 7373\nint norec_sym;\n"; }

    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#define __has_include(x) 0\n"      // UNGUARDED — refused, and refused
        "#if __has_include(<norec.h>)\n"    // loudly (asserted below)
        "#include <norec.h>\n"
        "#endif\n"
        "int u = NOREC_MARKER;\n",
        r, {inc}, {});

    EXPECT_TRUE(hasPPCode(
        r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable))
        << "the unguarded shadow must be refused LOUDLY";
    auto has = [&](std::string_view s) {
        for (auto const& l : lexs) if (l == s) return true;
        return false;
    };
    EXPECT_TRUE(has("norec_sym"))
        << "the refused #define must leave the operator INTACT in the pre-scan "
           "too — a pre-scan that recorded it would go FIX-3-uncertain on the "
           "next guard and skip this splice";
    EXPECT_TRUE(has("7373"))
        << "NOREC_MARKER must EXPAND — the splice really happened";
    fs::remove_all(inc, ec);
}

// ══════════════════════════════════════════════════════════════════════════════
// TF-C87 (D-PP-INCLUDE-REENTRY-GUARD-AWARE) — GUARD-AWARE INCLUDE RE-ENTRY.
//
// ★ WHAT THIS BLOCK PINS. `includeStack` used to REFUSE re-entry into any header
// already on the stack, emitting `P_PreprocessorIncludeError`. That rejects
// LEGAL, STANDARD-CONFORMING C. MEASURED on the macho corpus leg at 5093341,
// `src/mem1.c`:
//     mach/mach_types.h -> mach/task_policy.h -> mach/mach_types.h
// terminates perfectly under a real cpp — the second entry hits
// `#ifndef _MACH_MACH_TYPES_H_` and expands to nothing. cpp has NO
// refuse-re-entry rule; it has a NESTING DEPTH limit, and the include guard is
// what terminates the cycle. That one false positive was the sole cause of the
// leg's 4 residual `F_ShippedHeaderNotFound`.
//
// ★ WHY THERE IS ONE TEST PER GUARD SPELLING RATHER THAN ONE FOR "GUARDS".
// Guard DETECTION GATES re-entry, so an unrecognised but LEGAL guard becomes a
// REFUSED include — which presents to a user as a compiler bug. The corpus
// exercises exactly ONE spelling (`#ifndef`); every other spelling below was
// MEASURED in the macOS SDK and/or the sqlite tree and would otherwise ship
// untested. Counts are from `scratchpad/guard-shape-census.py` over the 3100 SDK
// headers + 52 sqlite headers.
//
// Shared shape of every POSITIVE test: `outer.h` includes `inner.h`, and
// `inner.h` includes `outer.h` BACK. Under the old rule the back-edge was
// refused and errored. Under the new one it is permitted, the guard empties the
// second expansion, and `OUTER_MARK` — defined in outer.h AFTER the back-edge
// resolves — must still reach the program text exactly once.

namespace {

// Build the outer/inner pair in a fresh dir and preprocess a main.c that
// includes `outer.h`. `outerGuardOpen`/`outerGuardClose` are the SPELLING under
// test; `outerBody` is spliced between them.
struct ReentryFixture {
    std::filesystem::path dir;
    PreprocessResult      result;
    std::vector<std::string> lexemes;

    [[nodiscard]] bool has(std::string_view s) const {
        for (auto const& l : lexemes) if (l == s) return true;
        return false;
    }
    [[nodiscard]] std::size_t count(std::string_view s) const {
        std::size_t n = 0;
        for (auto const& l : lexemes) if (l == s) ++n;
        return n;
    }
    [[nodiscard]] std::vector<std::string> messages() const {
        std::vector<std::string> out;
        for (auto const& d : result.diagnostics->all()) out.push_back(d.actual);
        return out;
    }
    [[nodiscard]] bool anyMessageContains(std::string_view needle) const {
        for (auto const& m : messages()) {
            if (m.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

// `guardedOuter` is outer.h's FULL text and must (a) include "inner.h" and
// (b) define OUTER_MARK to 8787 after it.
[[nodiscard]] ReentryFixture runReentry(std::string const& tag,
                                        std::string const& guardedOuter,
                                        std::string const& innerText) {
    namespace fs = std::filesystem;
    ReentryFixture f;
    f.dir = ppScratchRoot() / ("dss_tf87_" + tag);
    std::error_code ec;
    fs::remove_all(f.dir, ec);
    fs::create_directories(f.dir, ec);
    { std::ofstream(f.dir / "outer.h", std::ios::binary) << guardedOuter; }
    { std::ofstream(f.dir / "inner.h", std::ios::binary) << innerText; }
    f.lexemes = ppLexemesWithDirs("#include \"outer.h\"\nint m = OUTER_MARK;\n",
                                  f.result, {f.dir}, {});
    fs::remove_all(f.dir, ec);   // the splice is complete; nothing re-reads it
    return f;
}

// inner.h ALWAYS includes outer.h back — that back-edge is the whole point.
constexpr char const* kInnerIncludesOuterBack =
    "#include \"outer.h\"\nint inner_sym_8787;\n";

// ★ ONE constant, read by BOTH the test that requires this phrase and the two
// that require its ABSENCE. Two hand-typed copies is how a message-assertion
// goes vacuous: the `EXPECT_FALSE` half passes for free the moment the phrasing
// drifts, and nothing says so. (TF-C86 shipped exactly that gap once.)
constexpr char const* kNoMechanismPhrase = "include-once mechanism was found";
constexpr char const* kDepthCapPhrase    = "include nesting deeper than";
constexpr char const* kRefusalPhrase     = "refusing to re-enter";

// Assert the POSITIVE contract: no diagnostic at all, the back-edge produced no
// duplicate text, and the marker really expanded.
void expectReentryPermitted(ReentryFixture const& f, char const* what) {
    EXPECT_FALSE(f.result.diagnostics->hasErrors())
        << what << ": re-entry into a GUARDED header must be PERMITTED — the "
                   "guard is what makes the second expansion empty, exactly as "
                   "in cpp. First diagnostic: "
        << (f.messages().empty() ? std::string{"<none>"} : f.messages().front());
    EXPECT_EQ(f.count("8787"), 1u)
        << what << ": OUTER_MARK must expand EXACTLY ONCE — 0 means the header "
                   "was dropped, >1 means the guard failed to empty the "
                   "re-entered copy and the text was spliced twice";
    EXPECT_EQ(f.count("inner_sym_8787"), 1u)
        << what << ": inner.h's own text must appear exactly once";
}

}  // namespace

// ── FORM 1: canonical `#ifndef X` / `#define X`. ────────────────────────────
// MEASURED 2942 of 3100 SDK headers and 35 of 52 sqlite headers. THE corpus
// shape (`mach/mach_types.h`).
TEST(Preprocessor, Tf87ReentryPermittedForCanonicalIfndefGuard) {
    auto f = runReentry("ifndef",
                        "#ifndef OUTER_H\n#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "#ifndef X / #define X");
}

// ── FORM 2: `#if !defined(X)` / `#define X`. ────────────────────────────────
// MEASURED 10 SDK headers (`_inttypes.h`, `odmodule/*`) + 1 sqlite
// (`ext/expert/sqlite3expert.h`).
TEST(Preprocessor, Tf87ReentryPermittedForIfNotDefinedParenGuard) {
    auto f = runReentry("ifnotdef_paren",
                        "#if !defined(OUTER_H)\n#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "#if !defined(X) / #define X");
}

// ── FORM 3: `#if !defined X` — NO PARENTHESES. ──────────────────────────────
// MEASURED as real SDK vocabulary (`math.h`, `libxslt/xsltconfig.h`,
// `Spatial/SPPose3D.h` …) though never as a first-line guard in this tree.
// Supported because a spelling that is legal C must not become a refused
// include the day someone uses it as a guard.
TEST(Preprocessor, Tf87ReentryPermittedForIfNotDefinedNoParenGuard) {
    auto f = runReentry("ifnotdef_noparen",
                        "#if !defined OUTER_H\n#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "#if !defined X (no parens) / #define X");
}

// ── FORM 4: COMPOUND, all-negative. ─────────────────────────────────────────
// MEASURED verbatim in `pcap/bpf.h`:
//   #if !defined(_NET_BPF_H_) && !defined(_BPF_H_) && … && !defined(lib_pcap_bpf_h)
// The guard name is the LAST of five, so a detector that only reads the first
// operand refuses this header.
TEST(Preprocessor, Tf87ReentryPermittedForCompoundAllNegativeGuard) {
    auto f = runReentry("compound_neg",
                        "#if !defined(_ALT_A) && !defined(_ALT_B) "
                        "&& !defined(OUTER_H)\n"
                        "#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "#if !defined(A) && !defined(B) && !defined(X)");
}

// ── FORM 5: COMPOUND with MIXED polarity. ───────────────────────────────────
// MEASURED in the sqlite corpus ITSELF — `ext/misc/windirent.h`:
//   #if defined(_WIN32) && defined(_MSC_VER) && !defined(SQLITE_WINDIRENT_H)
// and `ext/session/sqlite3session.h`:
//   #if !defined(__SQLITESESSION_H_) && defined(SQLITE_ENABLE_SESSION)
// A detector that pattern-matches "the condition is a negation" fails both.
TEST(Preprocessor, Tf87ReentryPermittedForCompoundMixedPolarityGuard) {
    auto f = runReentry("compound_mixed",
                        "#define GATE_ON 1\n"
                        "#if defined(GATE_ON) && !defined(OUTER_H)\n"
                        "#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "#if defined(ON) && !defined(X)");
}

// ── FORM 6: the `#define` is NOT the next line. ─────────────────────────────
// MEASURED 15 headers (libc++ `inttypes.h` / `stdint.h` put it 5 logical lines
// down; `sys/_types/_os_inline.h` 2). Comments, blank lines AND a nested `#if`
// all sit between the guard open and its `#define` here.
TEST(Preprocessor, Tf87ReentryPermittedWhenGuardDefineIsNotAdjacent) {
    auto f = runReentry("define_far",
                        "#ifndef OUTER_H\n"
                        "\n"
                        "/* a banner comment\n   spanning lines */\n"
                        "#if 1\n"
                        "#endif\n"
                        "\n"
                        "#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "guard #define 6 lines below the #ifndef");
}

// ── FORM 7: the guard is NOT the FIRST conditional in the file. ─────────────
// MEASURED 9 SDK + 2 sqlite headers. `netinet6/in6.h` opens with
// `#ifndef __KAME_NETINET_IN_H_INCLUDED_` (an umbrella check with no matching
// `#define`) and only THEN opens the real guard. A "the first conditional is
// the guard" rule refuses every one of these.
TEST(Preprocessor, Tf87ReentryPermittedWhenGuardIsNotFirstConditional) {
    auto f = runReentry("guard_not_first",
                        "#ifndef _UMBRELLA_CHECK_\n"
                        "#endif\n"
                        "#ifdef SOMETHING_ELSE\n"
                        "#endif\n"
                        "#ifndef OUTER_H\n#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "guard preceded by two unrelated conditionals");
}

// ── FORM 8: the controlling name arrives on an `#elif`, not the `#if`. ──────
// The `#elif`/`#elifdef`/`#elifndef` operands are further controlling
// expressions of the SAME group, so they must extend that group's name set.
// Pins the elif arm of the detector, which no other test in this block reaches.
TEST(Preprocessor, Tf87ReentryPermittedWhenGuardNameComesFromElif) {
    auto f = runReentry("guard_via_elif",
                        "#if defined(NEVER_DEFINED_ZZZ)\n"
                        "#elif !defined(OUTER_H)\n"
                        "#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "#elif !defined(X) / #define X");
}

// ── THE NEGATIVE: an UNGUARDED self-including header is REFUSED, LOUDLY, ────
// and with a message a reader can act on. Refusing immediately beats churning
// to the depth cap: this names the header on the first back-edge.
//
// ★ THE MESSAGE ASSERTIONS ARE THE POINT, not decoration. Because guard
// detection GATES re-entry, a DETECTOR GAP and a REAL CYCLE produce the same
// refusal — so the message must say which one the compiler believes it saw and
// must not be confusable with the depth-cap backstop.
TEST(Preprocessor, Tf87UnguardedSelfIncludeIsRefusedLoudlyAndDiagnosably) {
    namespace fs = std::filesystem;
    auto dir = ppScratchRoot() / "dss_tf87_unguarded";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // No conditional names a macro this header then #defines, and no
    // include-once #pragma: nothing terminates the cycle.
    { std::ofstream(dir / "loop.h", std::ios::binary)
          << "int loop_sym;\n#include \"loop.h\"\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs("#include \"loop.h\"\nint m = 1;\n", r,
                                  {dir}, {});
    (void)lexs;
    // ★ THE CODE IS THE MACHINE-READABLE HALF OF THE DISTINGUISHABILITY
    // REQUIREMENT. `P_PreprocessorIncludeError` (0x0016) is four-way overloaded
    // — not found, unreadable, this refusal, and the depth cap — so a shared
    // code leaves every census / log filter / tool unable to tell a GUARD-
    // DETECTOR GAP from "your includes nest too deeply". The refusal therefore
    // has its OWN code and the depth cap keeps 0x0016.
    EXPECT_TRUE(hasPPCode(r,
                          DiagnosticCode::P_PreprocessorIncludeReentryRefused))
        << "an unguarded self-including header IS an infinite cycle and must be "
           "refused LOUDLY, under the refusal's OWN diagnostic code";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "and NOT under the overloaded include-error code — sharing it with "
           "the depth cap is exactly what makes a detector gap undiagnosable";
    bool sawRefusal = false, sawDepthCap = false, sawDetectorGapHint = false;
    for (auto const& d : r.diagnostics->all()) {
        if (d.actual.find(kRefusalPhrase) != std::string::npos) {
            sawRefusal = true;
            if (d.actual.find(kNoMechanismPhrase) != std::string::npos) {
                sawDetectorGapHint =
                    d.actual.find("GAP IN THE GUARD DETECTOR")
                    != std::string::npos;
            }
        }
        if (d.actual.find(kDepthCapPhrase) != std::string::npos) {
            sawDepthCap = true;
        }
    }
    EXPECT_TRUE(sawRefusal)
        << "the refusal must NAME the header and say it is refusing to re-enter";
    EXPECT_TRUE(sawDetectorGapHint)
        << "the refusal must say WHY (no include-once mechanism found) AND tell "
           "the reader that a guarded header reaching this message is a DETECTOR "
           "GAP, not a cycle in their code — without that a detector gap is "
           "indistinguishable from a real cycle";
    EXPECT_FALSE(sawDepthCap)
        << "an immediate refusal must NOT be reported as the depth-cap backstop "
           "— the two are different failures and must read differently";
}

// ── `#pragma once` — HONOURED, so a self-include BELOW it is SKIPPED. ───────
//
// ⚠⚠ THIS TEST ASSERTED THE OPPOSITE UNTIL 2026-08-29, AND WHAT IT ASSERTED WAS
// BELOW THE REFERENCE UNION. Its previous body required BOTH
// `P_PreprocessorIncludeReentryRefused` AND `P_PreprocessorPragma` on this exact
// fixture, with the rationale "`includeOnce` is a REFINEMENT of `unsupported`,
// not an escape from it: this cycle must not let `#pragma once` quietly start
// working". That was a correct reading of TF-C87's scope and a correct guard
// against an ACCIDENTAL behaviour change — but it pinned a REFUSAL of a program
// every reference compiles. ✔MEASURED 2026-08-29 on this fixture verbatim, each
// reference invoked separately: WSL gcc 13.3.0 rc=0, WSL clang 18.1.3 rc=0,
// mingw-w64 gcc 13.2.0 rc=0, MSVC 19.51.36252 rc=0.
// D-PP-PRAGMA-RECOGNIZED-SEMANTICS built the dedup deliberately, so the pin is
// inverted rather than deleted, and the old rationale is kept above so nobody
// re-derives it.
//
// The fixture is a header that declares itself include-once and THEN includes
// itself. The pragma is REACHED before the self-include, so it has already
// fired: the inner `#include` names a file identity already in the registry and
// is skipped. No refusal, no pragma diagnostic, and `once_sym` is defined once.
TEST(Preprocessor, PragmaOnceSelfIncludeBelowTheDirectiveIsSkippedNotRefused) {
    namespace fs = std::filesystem;
    auto dir = ppScratchRoot() / "dss_tf87_pragma_once";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "once.h", std::ios::binary)
          << "#pragma once\nint once_sym;\n#include \"once.h\"\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs("#include \"once.h\"\nint m = 1;\n", r,
                                  {dir}, {});

    EXPECT_FALSE(hasPPCode(r,
                           DiagnosticCode::P_PreprocessorIncludeReentryRefused))
        << "the pragma had already FIRED when the self-include was reached, so "
           "the repeat is skipped and there is no re-entry to adjudicate";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPragma))
        << "`#pragma once` is IMPLEMENTED now — refusing it here refused a "
           "program all four references compile";
    EXPECT_FALSE(r.diagnostics->hasErrors());

    // The substantive property, not just the absence of diagnostics: the
    // header's text was spliced exactly ONCE.
    std::size_t onceSymCount = 0;
    for (auto const& lx : lexs)
        if (lx == "once_sym") ++onceSymCount;
    EXPECT_EQ(onceSymCount, 1u)
        << "a header that declares itself include-once must contribute its text "
           "once; two would be a duplicate definition downstream";
}

// ── …AND THE `OncePragma` RE-ENTRY ARM STILL HAS A REACHABLE CASE. ──────────
// A header whose self-include sits ABOVE its own `#pragma once` cannot be
// terminated by that pragma, because the declaration comes after the recursion.
// ✔MEASURED 2026-08-29: WSL gcc 13.3.0 and WSL clang 18.1.3 BOTH fail this
// (rc=1) with a runaway `In file included from` chain naming the header over and
// over into their nesting-depth limits — so refusing is matching them, not
// out-stricting them.
// The message must say WHY without repeating the now-false claim that DSS has
// not built include-once dedup.
TEST(Preprocessor, PragmaOnceSelfIncludeAboveTheDirectiveIsStillRefused) {
    namespace fs = std::filesystem;
    auto dir = ppScratchRoot() / "dss_pragma_once_above";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "once.h", std::ios::binary)
          << "#include \"once.h\"\n#pragma once\nint once_sym;\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs("#include \"once.h\"\nint m = 1;\n", r,
                                  {dir}, {});
    (void)lexs;

    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "a header that includes itself above its own include-once line is a "
           "genuine unbounded recursion; both gcc and clang fail it";

    bool sawOnceRefusal = false, sawNoMechanismClaim = false, sawStaleClaim = false;
    for (auto const& d : r.diagnostics->all()) {
        if (d.actual.find(kRefusalPhrase) == std::string::npos) continue;
        if (d.actual.find("'includeOnce'") != std::string::npos)
            sawOnceRefusal = true;
        if (d.actual.find(kNoMechanismPhrase) != std::string::npos)
            sawNoMechanismClaim = true;
        if (d.actual.find("has not built include-once dedup")
            != std::string::npos)
            sawStaleClaim = true;
    }
    EXPECT_TRUE(sawOnceRefusal)
        << "the refusal must NAME the mechanism and the registry verb that "
           "declares it";
    EXPECT_FALSE(sawNoMechanismClaim)
        << "it must NOT claim no include-once mechanism was found — the header "
           "carries one";
    EXPECT_FALSE(sawStaleClaim)
        << "it must NOT say DSS has not built include-once dedup: that stopped "
           "being true when this row landed, and a message that misdescribes the "
           "engine sends the reader hunting the wrong thing";
}

// ── THE DEPTH CAP REALLY FIRES on a GUARDED-BUT-STILL-RECURSIVE header. ─────
// The detector is deliberately GENEROUS (over-recognition is the safe direction
// under PERMIT), so a value-default `#ifndef MIN` / `#define MIN` — MEASURED as
// a real shape in `sqlite/src/btreeInt.h` — reads as a guard. Here the
// self-include sits OUTSIDE that region, so the guard cannot empty anything and
// the recursion is real. The BACKSTOP must catch it, LOUDLY, and say something
// different from the refusal.
TEST(Preprocessor, Tf87DepthCapFiresLoudlyOnGuardedButStillRecursiveHeader) {
    namespace fs = std::filesystem;
    auto dir = ppScratchRoot() / "dss_tf87_depthcap";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "rec.h", std::ios::binary)
          << "#ifndef REC_MIN\n#define REC_MIN 1\n#endif\n"
             "#include \"rec.h\"\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs("#include \"rec.h\"\nint m = 1;\n", r,
                                  {dir}, {});
    (void)lexs;
    EXPECT_TRUE(r.fatal)
        << "an include nesting that reaches the backstop TRUNCATES the splice, "
           "so the preprocess result must be fatal — a truncated splice reported "
           "as merely 'an error' would let a partial TU flow downstream";
    // ★ THE CODE SPLIT, FROM THE OTHER SIDE. The depth cap is a genuine
    // resource/structure limit and makes NO claim about guards, so it keeps
    // `P_PreprocessorIncludeError`; the guard-detection refusal must NOT appear
    // here. Together with the negative test's mirror assertions this pins the
    // separation in BOTH directions — either code leaking into the other case
    // reds exactly one of the two.
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "the depth-cap backstop keeps the include-error code";
    EXPECT_FALSE(hasPPCode(r,
                           DiagnosticCode::P_PreprocessorIncludeReentryRefused))
        << "a depth-cap failure must NOT be reported as a refused re-entry: a "
           "guard WAS detected here, and telling the reader otherwise sends them "
           "hunting a detector gap that does not exist";
    bool sawDepthCap = false, sawNoMechanismClaim = false;
    for (auto const& d : r.diagnostics->all()) {
        if (d.actual.find(kDepthCapPhrase) != std::string::npos) {
            sawDepthCap = true;
        }
        if (d.actual.find(kNoMechanismPhrase) != std::string::npos) {
            sawNoMechanismClaim = true;
        }
    }
    EXPECT_TRUE(sawDepthCap)
        << "the guarded-but-recursive case must reach the DEPTH CAP and say so";
    EXPECT_FALSE(sawNoMechanismClaim)
        << "the depth cap must not be reported as a missing guard — a guard WAS "
           "detected here; it simply did not neutralize the repeat include";
}

// ── THE BACKSTOP MUST TERMINATE THE WHOLE SPLICE, NOT JUST ONE ARM. ────────
// `fatal` is shared by reference across the builder tree, but before TF-C87 it
// only truncated the recursion that hit the cap. A header that includes a
// recursive sibling TWICE then re-enters the cap path down BOTH arms at every
// level — 2^64 builds. `build()` now returns immediately once `fatal` is set.
//
// ★ RED-ON-DISABLE IS A HANG, NOT A FAILURE: delete `if (fatal) return;` from
// `SynthBuilder::build` and this test does not finish (ctest reports a timeout).
// That is the honest red for an exponential-blowup guard; there is no cheaper
// witness, because the blowup is only reachable AT the cap depth.
TEST(Preprocessor, Tf87DepthCapShortCircuitsTheWholeSpliceNotJustOneArm) {
    namespace fs = std::filesystem;
    auto dir = ppScratchRoot() / "dss_tf87_depthcap_branch";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "rec2.h", std::ios::binary)
          << "#ifndef REC2_MIN\n#define REC2_MIN 1\n#endif\n"
             "#include \"rec2.h\"\n#include \"rec2.h\"\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs("#include \"rec2.h\"\nint m = 1;\n", r,
                                  {dir}, {});
    (void)lexs;
    EXPECT_TRUE(r.fatal)
        << "the backstop must still fire on the branching shape";
    fs::remove_all(dir, ec);
}

// ── THE ANGLE ARM HAS THE SAME CONTRACT. ───────────────────────────────────
// ★ THIS IS THE ARM THE CORPUS DEFECT WAS ON. `mach/mach_types.h` is reached as
// an ANGLE include resolved to a real source header on the -I path
// (`AngleIncludeKind::Source`), and that arm's refusal `continue`d WITHOUT
// dropping the directive — so the surviving `#include <…>` line reached the
// post-parse import resolver and became `F_ShippedHeaderNotFound`. A quote-only
// fix would have left all 4 F001A in place.
TEST(Preprocessor, Tf87ReentryPermittedThroughTheAngleSourceIncludeArm) {
    namespace fs = std::filesystem;
    auto dir = ppScratchRoot() / "dss_tf87_angle";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sub", ec);
    { std::ofstream(dir / "sub" / "outer_ang.h", std::ios::binary)
          << "#ifndef OUTER_ANG_H\n#define OUTER_ANG_H\n"
             "#include <sub/inner_ang.h>\n#define ANG_MARK 9191\n#endif\n"; }
    { std::ofstream(dir / "sub" / "inner_ang.h", std::ios::binary)
          << "#include <sub/outer_ang.h>\nint ang_inner_sym;\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#include <sub/outer_ang.h>\nint m = ANG_MARK;\n", r, {dir}, {});
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "the ANGLE-source include arm must make the same re-entry decision as "
           "the quote arm — this is the arm the corpus F001A came from";
    std::size_t marks = 0, inners = 0;
    for (auto const& l : lexs) {
        if (l == "9191") ++marks;
        if (l == "ang_inner_sym") ++inners;
    }
    EXPECT_EQ(marks, 1u) << "ANG_MARK must expand exactly once";
    EXPECT_EQ(inners, 1u) << "the angle-spliced inner header must appear once";
    fs::remove_all(dir, ec);
}

// ── AGNOSTICISM: the detector reads its directive vocabulary from CONFIG. ───
// Rebind `ifndefDirective` to a non-C spelling and re-run the FORM-1 fixture
// with that spelling. If any part of the detector hard-coded "ifndef", the
// rebound guard goes unrecognised and the back-edge is refused.
TEST(Preprocessor, Tf87GuardDetectionReadsIfndefSpellingFromConfigNotHardcoded) {
    namespace fs = std::filesystem;
    std::string cfgText = loadShippedCText();
    ASSERT_FALSE(cfgText.empty()) << "could not locate the shipped c JSON";
    const std::string from = "\"ifndefDirective\":     \"ifndef\"";
    const std::string to   = "\"ifndefDirective\":     \"unlesseth\"";
    auto const at = cfgText.find(from);
    ASSERT_NE(at, std::string::npos)
        << "shipped c config no longer carries ifndefDirective=ifndef";
    cfgText.replace(at, from.size(), to);

    auto loaded =
        GrammarSchema::loadFromText(cfgText, "<rebound-ifndef-c>");
    ASSERT_TRUE(loaded.has_value())
        << "the rebound config must still load — otherwise this test proves "
           "nothing about the detector";
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    ASSERT_EQ(schema->preprocess().ifndefDirective, "unlesseth");

    auto dir = ppScratchRoot() / "dss_tf87_agnostic";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "outer.h", std::ios::binary)
          << "#unlesseth OUTER_H\n#define OUTER_H\n"
             "#include \"inner.h\"\n#define OUTER_MARK 8787\n#endif\n"; }
    { std::ofstream(dir / "inner.h", std::ios::binary)
          << kInnerIncludesOuterBack; }
    auto buf = SourceBuffer::fromString(
        "#include \"outer.h\"\nint m = OUTER_MARK;\n", "main.c");
    std::vector<fs::path> dirs{dir};
    auto out = preprocess(buf, schema, dirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(out.diagnostics->hasErrors())
        << "the guard detector must recognise the CONFIG-declared `#ifndef` "
           "spelling — a hard-coded \"ifndef\" refuses this legal header";
    std::size_t marks = 0;
    for (Token const& t : out.tokens) {
        if (std::string{out.synthBuffer->slice(t.span)} == "8787") ++marks;
    }
    EXPECT_EQ(marks, 1u)
        << "the rebound-spelling guard must empty the re-entered copy exactly "
           "as the C spelling does";
    fs::remove_all(dir, ec);
}

// ── THE `#define` MUST BE INSIDE THE REGION THE CONDITIONAL CONTROLS. ──────
// A `#ifndef X` group that CLOSES before `#define X` is not a guard: the define
// runs unconditionally, so a repeat include is not emptied by anything. Pins the
// `#endif` POP — without it the closed frame's names stay in scope, this header
// reads as guarded, re-entry is permitted, and the failure degrades from an
// immediate named refusal into a 64-level churn to the depth cap. That
// degradation is precisely what the "refuse an unguarded header immediately"
// half of this design exists to avoid.
TEST(Preprocessor, Tf87DefineOutsideTheClosedConditionalIsNotAGuard) {
    namespace fs = std::filesystem;
    auto dir = ppScratchRoot() / "dss_tf87_halfguard";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    { std::ofstream(dir / "half.h", std::ios::binary)
          << "#ifndef HALF_H\n#endif\n"       // group opens AND closes
             "#define HALF_H\n"               // …the define is OUTSIDE it
             "int half_sym;\n#include \"half.h\"\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs("#include \"half.h\"\nint m = 1;\n", r,
                                  {dir}, {});
    (void)lexs;
    bool sawRefusal = false, sawDepthCap = false;
    for (auto const& d : r.diagnostics->all()) {
        if (d.actual.find(kRefusalPhrase) != std::string::npos
            && d.actual.find(kNoMechanismPhrase) != std::string::npos) {
            sawRefusal = true;
        }
        if (d.actual.find(kDepthCapPhrase) != std::string::npos) {
            sawDepthCap = true;
        }
    }
    EXPECT_TRUE(hasPPCode(r,
                          DiagnosticCode::P_PreprocessorIncludeReentryRefused))
        << "the refusal must carry the refusal code";
    EXPECT_TRUE(sawRefusal)
        << "a `#define X` OUTSIDE the closed `#ifndef X` group is not a guard — "
           "this header must be refused by name on the first back-edge";
    EXPECT_FALSE(sawDepthCap)
        << "and refused IMMEDIATELY, not churned to the depth cap: naming the "
           "header on the first back-edge is the whole point of refusing an "
           "unguarded one";
    fs::remove_all(dir, ec);
}

// ── A GUARD SPLIT ACROSS A LINE CONTINUATION. ──────────────────────────────
// C 5.1.1.2 phase 2 splices `\`-newline BEFORE directives are recognised, so a
// multi-line `#if` is ONE logical controlling line. The detector must splice
// too: harvesting names per PHYSICAL line loses every operand past the first
// backslash — i.e. UNDER-recognises, the direction that turns a legal header
// into a refused include. Multi-line `#if !defined(A) && \` guards are ordinary
// in C.
TEST(Preprocessor, Tf87GuardSplitAcrossLineContinuationIsStillDetected) {
    auto f = runReentry("continuation",
                        "#if !defined(_ALT_ZZZ) && \\\n"
                        "    !defined(OUTER_H)\n"
                        "#define OUTER_H\n"
                        "#include \"inner.h\"\n#define OUTER_MARK 8787\n"
                        "#endif\n",
                        kInnerIncludesOuterBack);
    expectReentryPermitted(f, "guard operand on a continuation line");
}

// ═════════════════════════════════════════════════════════════════════════════
// TF-C97 (D-PP-FORMAT-DATA-MODEL-PREDEFINES) — the FORMAT's predefines, the
// THIRD config family.
//
// `<name>.format.json` may now carry `predefinedMacros`, merged with the
// language's and the target's by the same `mergePredefinedMacros`. What the
// format owns is the C-visible face of ITS OWN axes — `__LP64__`/`_LP64`, the
// face of `dataModel` — which is neither a language fact nor a CPU fact: the
// SAME x86_64 target is LP64 under elf64/macho64 and LLP64 under pe64, so a
// target-side row would be wrong on one of its own formats.
//
// ★ THE DEFECT THIS CLOSED, MEASURED on `arm64:macho64-arm64-darwin-staticlib`
// before the change: `__arm64__` and `__APPLE__` defined, `__LP64__` NOT
// defined, `sizeof(long)==8`, `sizeof(void*)==8` — LP64 widths with ILP32
// headers. The macOS SDK gates 234 occurrences across 91 headers on `__LP64__`;
// `mach/port.h` then asserted the user32 struct size 12 instead of
// user64's 16, which is sqlite `mem1.c`'s three `error[S0029]`.
//
// ★ WHAT THESE TESTS PIN THAT THE FORMAT-SCHEMA TESTS CANNOT
// (tests/link/test_object_format_schema.cpp owns the loader half): that the
// parsed rows actually REACH the preprocessor, that the NEGATIVE holds, and
// that the collision policy covers every PAIR of the three families — the last
// being the specific thing a two-family implementation grown by one loop
// silently misses.
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// `ppLexemesForTarget`, plus a FORMAT predefine list. Kept separate rather than
// widening the TF-C74 helper so its ~20 existing callers stay byte-identical.
[[nodiscard]] std::vector<std::string> ppLexemesForTargetAndFormat(
    std::string text, std::optional<ObjectFormatKind> fmt,
    std::span<PredefinedMacroDef const> targetMacros,
    std::span<PredefinedMacroDef const> formatMacros, PreprocessResult& out) {
    auto schema = cSubset();
    auto buf    = SourceBuffer::fromString(std::move(text), "main.c");
    std::vector<std::filesystem::path> noDirs;
    std::vector<std::string>           noDefines;
    out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, fmt, noDefines, targetMacros,
                     formatMacros);
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

// The channel is READ end-to-end: a format-declared predefine EXPANDS in
// ordinary code, exactly as a target-declared one does.
// RED-ON-DISABLE: drop `formatPredefinedMacros` from the `mergePredefinedMacros`
// call in `preprocess()` and the token comes back as the bare identifier.
TEST(Preprocessor, TFC97FormatPredefineExpands) {
    std::vector<PredefinedMacroDef> fms{targetMacro("__FMTPROBE__", "9")};
    PreprocessResult r;
    auto lexs = ppLexemesForTargetAndFormat("int x = __FMTPROBE__;\n",
                                            ObjectFormatKind::MachO, {}, fms, r);
    EXPECT_EQ(lexs, (std::vector<std::string>{"int", "x", "=", "9", ";"}))
        << "a FORMAT-declared predefine must reach the MacroExpander, not just "
           "the merge result";
}

// ...and it is visible to `#if defined()`, i.e. to the include-gating pre-scan's
// definedness oracle — the seed site a naive wiring misses. The SDK arms this
// unblocks are `#if defined(__LP64__)` shapes, so this is the site that matters.
TEST(Preprocessor, TFC97FormatPredefineIsVisibleToConditionalInclusion) {
    std::vector<PredefinedMacroDef> fms{targetMacro("__FMTPROBE__", "1")};
    PreprocessResult r;
    auto lexs = ppLexemesForTargetAndFormat(
        "#if defined(__FMTPROBE__)\nint yes;\n#else\nint no;\n#endif\n",
        ObjectFormatKind::MachO, {}, fms, r);
    EXPECT_EQ(lexs, (std::vector<std::string>{"int", "yes", ";"}));
}

// ── the COLLISION policy, over ALL THREE PAIRS ────────────────────────────
//
// ★ THE TARGET×FORMAT PAIR IS THE ONE THAT WOULD BE MISSED. With two families
// the scan was one loop; the natural way to add a third is to write one more
// loop against the LANGUAGE, which leaves target×format silently unscanned and
// therefore silently last-writer-wins. That is why the scan enumerates pairs
// over a table, and why this test asserts the pair, not just the code.
TEST(Preprocessor, TFC97CollisionCoversEveryPairOfTheThreeFamilies) {
    auto run = [](std::span<PredefinedMacroDef const> tgt,
                  std::span<PredefinedMacroDef const> fmt) {
        auto schema = cSubset();
        auto buf = SourceBuffer::fromString("int x = 1;\n", "main.c");
        std::vector<std::filesystem::path> noDirs;
        std::vector<std::string>           noDefines;
        return preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, ObjectFormatKind::Elf,
                          noDefines, tgt, fmt);
    };

    // (1) LANGUAGE × FORMAT. `__LINE__` is declared by the shipped c
    // language config.
    {
        std::vector<PredefinedMacroDef> fms{targetMacro("__LINE__", "1")};
        PreprocessResult r = run({}, fms);
        EXPECT_TRUE(r.fatal);
        ASSERT_TRUE(hasPPCode(r, DiagnosticCode::C_ConflictingPredefinedMacro));
        bool named = false;
        for (auto const& d : r.diagnostics->all()) {
            if (d.code != DiagnosticCode::C_ConflictingPredefinedMacro) continue;
            named = d.actual.find(".lang.json") != std::string::npos
                 && d.actual.find(".format.json") != std::string::npos
                 && d.actual.find("__LINE__") != std::string::npos;
            if (named) break;
        }
        EXPECT_TRUE(named)
            << "the message must name the macro AND both declaring config "
               "FILES — target and format share the JSON pointer "
               "`/predefinedMacros`, so a bare pointer would name two "
               "different files identically";
    }

    // (2) TARGET × FORMAT — neither side is the language, so a scan written
    // only against the language misses it entirely.
    {
        std::vector<PredefinedMacroDef> tms{targetMacro("__COLLIDE__", "1")};
        std::vector<PredefinedMacroDef> fms{targetMacro("__COLLIDE__", "2")};
        PreprocessResult r = run(tms, fms);
        EXPECT_TRUE(r.fatal)
            << "a TARGET/FORMAT collision must abort the pass — the language "
               "is not involved, so a language-anchored scan would silently "
               "resolve it to whichever side merged last";
        ASSERT_TRUE(hasPPCode(r, DiagnosticCode::C_ConflictingPredefinedMacro));
        bool named = false;
        for (auto const& d : r.diagnostics->all()) {
            if (d.code != DiagnosticCode::C_ConflictingPredefinedMacro) continue;
            named = d.actual.find(".target.json") != std::string::npos
                 && d.actual.find(".format.json") != std::string::npos;
            if (named) break;
        }
        EXPECT_TRUE(named) << "the message must name BOTH config files";
    }

    // (3) LANGUAGE × TARGET still fires (the TF-C74 pair, unregressed).
    {
        std::vector<PredefinedMacroDef> tms{targetMacro("__LINE__", "1")};
        PreprocessResult r = run(tms, {});
        EXPECT_TRUE(r.fatal);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::C_ConflictingPredefinedMacro));
    }
}

// The collision scan still runs BEFORE the format filter for the new family:
// a `["pe"]`-gated FORMAT row collides with an ungated target row even on ELF,
// where the gated entry would not survive. Gating decides which formats SEE a
// macro, never who OWNS the name.
TEST(Preprocessor, TFC97FormatCollisionDetectedBeforeFormatFilter) {
    std::vector<PredefinedMacroDef> tms{targetMacro("__COLLIDE__", "1")};
    std::vector<PredefinedMacroDef> fms{targetMacro("__COLLIDE__", "2", {"pe"})};
    auto merged = mergePredefinedMacros({}, tms, fms, ObjectFormatKind::Elf);
    EXPECT_FALSE(merged.conflicts.empty())
        << "a pe-GATED format row must still collide with an UNGATED target "
           "row on an ELF build";
    EXPECT_TRUE(merged.effective.empty())
        << "a failed merge must leave NO partially-merged list a caller could "
           "mistake for usable";
}

// Order is language, then target, then format — stable within each family. The
// seed sites lower the effective list to `#define` STREAMS, so order is
// observable behaviour, not an accident.
TEST(Preprocessor, TFC97MergeOrderIsLanguageThenTargetThenFormat) {
    std::vector<PredefinedMacroDef> lang{targetMacro("L1", "1")};
    std::vector<PredefinedMacroDef> tgt{targetMacro("T1", "2"),
                                        targetMacro("T2", "3")};
    std::vector<PredefinedMacroDef> fmt{targetMacro("F1", "4")};
    auto merged = mergePredefinedMacros(lang, tgt, fmt, ObjectFormatKind::Elf);
    ASSERT_TRUE(merged.conflicts.empty());
    std::vector<std::string> names;
    for (auto const& pm : merged.effective) names.push_back(pm.name);
    EXPECT_EQ(names, (std::vector<std::string>{"L1", "T1", "T2", "F1"}));
}

// The per-entry `availableObjectFormats` filter applies to FORMAT rows too, and
// is still applied exactly ONCE. (Shipped format rows are ungated — the file is
// already the gate — but the mechanism must not quietly skip this family.)
TEST(Preprocessor, TFC97FormatRowsAreFormatFilteredLikeEveryOther) {
    std::vector<PredefinedMacroDef> fmt{targetMacro("PE_ONLY", "1", {"pe"}),
                                        targetMacro("UNIVERSAL", "2")};
    auto onElf = mergePredefinedMacros({}, {}, fmt, ObjectFormatKind::Elf);
    ASSERT_TRUE(onElf.conflicts.empty());
    ASSERT_EQ(onElf.effective.size(), 1u);
    EXPECT_EQ(onElf.effective[0].name, "UNIVERSAL");

    auto onPe = mergePredefinedMacros({}, {}, fmt, ObjectFormatKind::Pe);
    ASSERT_EQ(onPe.effective.size(), 2u);
}

// The NO-REGRESSION invariant: an EMPTY format span leaves the synthesized text
// byte-identical to the pre-TF-C97 call shape, on every format. This is what
// lets the LSP and the FFI header parser keep their (deliberate) empty lists.
TEST(Preprocessor, TFC97EmptyFormatSpanIsByteIdenticalToLegacy) {
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
        std::vector<PredefinedMacroDef>    none;

        auto legacyBuf = SourceBuffer::fromString(kSrc, "main.c");
        PreprocessResult legacy = preprocess(legacyBuf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, fmt,
                                             noDefines, none);
        auto newBuf = SourceBuffer::fromString(kSrc, "main.c");
        PreprocessResult withEmpty = preprocess(newBuf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault(), {}, fmt,
                                                noDefines, none, none);
        ASSERT_FALSE(legacy.fatal);
        ASSERT_FALSE(withEmpty.fatal);
        EXPECT_EQ(legacy.synthBuffer->text(), withEmpty.synthBuffer->text());
    }
}

// ★ THE SHIPPED-CONFIG END-TO-END PIN, BOTH DIRECTIONS.
//
// Everything above uses hand-built rows so it pins the ENGINE. This one drives
// the REAL shipped language + target + format configs through the real merge and
// asserts what a C program on each leg actually sees. The negative half (pe64
// defines NEITHER spelling) is the reason the channel is on the format, so it is
// asserted as strictly as the positive.
//
// The expectation is keyed on the loaded schema's `dataModel()`, never on a
// format name — the same discipline the config author applied.
TEST(Preprocessor, TFC97ShippedFormatsGiveACoherentDataModelWorld) {
    auto c = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(c.has_value());
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm.has_value());
    auto x86 = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value());

    struct Leg {
        std::string_view formatName;
        std::shared_ptr<TargetSchema const> target;
        ObjectFormatKind kind;
    };
    std::vector<Leg> const legs{
        {"macho64-arm64-darwin-exec",  *arm, ObjectFormatKind::MachO},
        {"macho64-arm64-darwin-staticlib", *arm, ObjectFormatKind::MachO},
        {"elf64-aarch64-linux-exec",   *arm, ObjectFormatKind::Elf},
        {"elf64-x86_64-linux-exec",    *x86, ObjectFormatKind::Elf},
        {"macho64-x86_64-darwin-exec", *x86, ObjectFormatKind::MachO},
        {"pe64-x86_64-windows-exec",   *x86, ObjectFormatKind::Pe},
        {"pe64-x86_64-windows-dll",    *x86, ObjectFormatKind::Pe},
    };

    std::size_t lp64Legs = 0, llp64Legs = 0;
    for (Leg const& leg : legs) {
        auto f = ObjectFormatSchema::loadShipped(std::string{leg.formatName});
        ASSERT_TRUE(f.has_value()) << leg.formatName;
        auto merged = mergePredefinedMacros((*c)->preprocess().predefinedMacros,
                                            leg.target->predefinedMacros(),
                                            (*f)->predefinedMacros(), leg.kind);
        ASSERT_TRUE(merged.conflicts.empty())
            << leg.formatName
            << ": the shipped language, target and format configs must not "
               "collide on any name";
        auto defines = [&](std::string_view n) {
            for (auto const& pm : merged.effective) {
                if (pm.name == n) return true;
            }
            return false;
        };
        bool const wantLp64 = (*f)->dataModel() == DataModel::Lp64;
        if (wantLp64) ++lp64Legs; else ++llp64Legs;
        EXPECT_EQ(defines("__LP64__"), wantLp64) << leg.formatName;
        EXPECT_EQ(defines("_LP64"), wantLp64) << leg.formatName;
        // ★ The two spellings must move TOGETHER. A leg that defined one and
        // not the other would satisfy a naive "is __LP64__ defined?" check
        // while leaving every `#ifdef _LP64` SDK arm on the wrong branch.
        EXPECT_EQ(defines("__LP64__"), defines("_LP64")) << leg.formatName;
    }
    EXPECT_EQ(lp64Legs, 5u);
    EXPECT_EQ(llp64Legs, 2u);
}

// ── D-PP-HEADER-CASE-INSENSITIVE-PE ──────────────────────────────────────────
//
// `__has_include` and `#include` must give the SAME answer, and that answer is
// the TARGET FORMAT's case convention — never the build host's filesystem.
//
// Each pin asserts BOTH policies against ONE on-disk file, because each
// direction of the defect is only observable on one kind of host: the
// case-INSENSITIVE arm goes red on ext4 if DSS stops folding for itself, and
// the case-SENSITIVE arm goes red on NTFS/APFS if DSS lets the host fold.
TEST(Preprocessor, HeaderCaseHasIncludeAngleFollowsFormatPolicyNotHost) {
    namespace fs = std::filesystem;
    auto sysdir = ppScratchRoot() / "dss_header_case_sys";
    fs::create_directories(sysdir);
    { std::ofstream(sysdir / "windows.json", std::ios::binary) << "{}\n"; }

    char const* src = "#if __has_include(<Windows.h>)\nint yes;\n#else\nint no;\n#endif\n";
    {   // pe / macho convention: `<Windows.h>` IS available.
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(src, r, {}, {sysdir},
                                      dss::HeaderNameMatching::CaseInsensitive);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "yes")
            << "a case-insensitive format must see <Windows.h> -> windows.json "
               "on ANY build host (sqlite3.c spells it with a capital W)";
    }
    {   // elf convention: it is NOT — byte-exact or nothing.
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(src, r, {}, {sysdir},
                                      dss::HeaderNameMatching::CaseSensitive);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "no")
            << "a POSIX/elf target must NOT fold <Windows.h> onto windows.json "
               "even when the build host's filesystem would";
    }
    {   // CONTROL: the byte-exact spelling is available under both policies, so
        // the divergence above is attributable to CASE and nothing else.
        char const* exact =
            "#if __has_include(<windows.h>)\nint yes;\n#else\nint no;\n#endif\n";
        for (auto m : {dss::HeaderNameMatching::CaseSensitive,
                       dss::HeaderNameMatching::CaseInsensitive}) {
            PreprocessResult r;
            auto lexs = ppLexemesWithDirs(exact, r, {}, {sysdir}, m);
            ASSERT_EQ(lexs.size(), 3u);
            EXPECT_EQ(lexs[1], "yes");
        }
    }
    std::error_code ec;
    fs::remove_all(sysdir, ec);
}

// The QUOTE form takes the same policy — `#include "Windows.h"` was measured
// succeeding on a Windows host for an elf target, the same silent accept.
TEST(Preprocessor, HeaderCaseHasIncludeQuoteFollowsFormatPolicyNotHost) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_header_case_quote";
    fs::create_directories(inc);
    { std::ofstream(inc / "myheader.h", std::ios::binary) << "int q;\n"; }

    char const* src = "#if __has_include(\"MyHeader.h\")\nint yes;\n#else\nint no;\n#endif\n";
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(src, r, {inc}, {},
                                      dss::HeaderNameMatching::CaseInsensitive);
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "yes");
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(src, r, {inc}, {},
                                      dss::HeaderNameMatching::CaseSensitive);
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "no")
            << "a case-mismatched QUOTE include must be rejected for a "
               "case-sensitive target on a case-insensitive host";
    }
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// AGREEMENT: whatever `__has_include` says, the `#include` must do. Here the
// header is a REAL source header on the -I path (the angle source-fallback
// arm), so a live include SPLICES its text — observable in the token stream.
// Without the policy in BOTH the `__has_include` callback and the splice arm,
// this test shows `yes` with no spliced `MARKER`, or vice versa: the exact
// drift the FC15c single-funnel design exists to prevent.
TEST(Preprocessor, HeaderCaseIncludeAndHasIncludeAgreeUnderBothPolicies) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_header_case_agree";
    fs::create_directories(inc);
    { std::ofstream(inc / "marker.h", std::ios::binary) << "int spliced_marker;\n"; }

    char const* src =
        "#if __has_include(<Marker.h>)\n"
        "#include <Marker.h>\n"
        "int probe_yes;\n"
        "#else\n"
        "int probe_no;\n"
        "#endif\n";
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(src, r, {inc}, {},
                                      dss::HeaderNameMatching::CaseInsensitive);
        auto has = [&](char const* n) {
            return std::find(lexs.begin(), lexs.end(), std::string{n}) != lexs.end();
        };
        EXPECT_TRUE(has("probe_yes")) << "__has_include said 0 under the "
                                         "case-insensitive policy";
        EXPECT_TRUE(has("spliced_marker"))
            << "__has_include answered 1 but the #include did not resolve — the "
               "two disagreed on the SAME name";
        EXPECT_FALSE(has("probe_no"));
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(src, r, {inc}, {},
                                      dss::HeaderNameMatching::CaseSensitive);
        auto has = [&](char const* n) {
            return std::find(lexs.begin(), lexs.end(), std::string{n}) != lexs.end();
        };
        EXPECT_TRUE(has("probe_no"));
        EXPECT_FALSE(has("probe_yes"));
        EXPECT_FALSE(has("spliced_marker"))
            << "__has_include answered 0 but the header was spliced anyway";
    }
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// ── D-PP-HEADER-CASE-INSENSITIVE-PE, H1/H2/H3: the fold-collision EMIT SITES ──
//
// These reach the real emit paths through the real filesystem, so they need a
// directory holding two names that differ only by ASCII case. That directory
// cannot exist on NTFS or a default APFS volume, so each test first ASKS the
// platform to make its scratch dir case-sensitive (the Windows 10+
// per-directory flag) and, if that is unavailable, records a NAMED skip rather
// than a silent pass. The host-independent half of this coverage — the code,
// the severity and the names-every-candidate contract — lives in
// `tests/core/test_header_name_matching.cpp` and runs everywhere.
namespace {

void ppTryMakeDirCaseSensitive(std::filesystem::path const& dir) {
#ifdef _WIN32
    std::string cmd = "fsutil.exe file setCaseSensitiveInfo \"";
    cmd += dir.string();
    cmd += "\" enable >nul 2>&1";
    (void)std::system(cmd.c_str());
#else
    (void)dir;   // the POSIX filesystems this project builds on already qualify
#endif
}

// Build `<dir>/<lower>` and `<dir>/<upper>` and report whether BOTH survived.
[[nodiscard]] bool ppMakeCollidingPair(std::filesystem::path const& dir,
                                       char const* lower, char const* upper,
                                       char const* body) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    ppTryMakeDirCaseSensitive(dir);
    { std::ofstream(dir / lower, std::ios::binary) << body; }
    { std::ofstream(dir / upper, std::ios::binary) << body; }
    std::size_t seen = 0;
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        ++seen;
    }
    return seen == 2;
}

[[nodiscard]] std::size_t ppCountCode(PreprocessResult const& r,
                                      dss::DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == code) ++n;
    }
    return n;
}

[[nodiscard]] std::string ppFirstMessage(PreprocessResult const& r,
                                         dss::DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all()) {
        if (d.code == code) return d.actual;
    }
    return {};
}

} // namespace

// H1 — THE BLOCKER. A quote-`#include` fold collision must fail loud with the
// UNSUPPRESSABLE `F_HeaderNameCaseAmbiguous`. It used to flatten to nullopt,
// fall through the quote-to-angle fallback, and exit as a SUPPRESSABLE
// `P_PreprocessorIncludeError` with the directive dropped, so suppressing that
// code turned a case collision into a silently missing header. Nothing
// downstream re-resolves a quote include once the preprocessor is enabled (the
// import resolver returns early on every quote directive), so this tier is the
// only possible reporter.
TEST(Preprocessor, HeaderCaseQuoteIncludeCollisionFailsLoudNotSuppressably) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_hdrcase_quote_collide";
    if (!ppMakeCollidingPair(inc, "foo.h", "Foo.h", "int q;\n")) {
        GTEST_SKIP() << "this filesystem folds case, so a foo.h/Foo.h pair "
                        "cannot be built here; the emit is pinned "
                        "host-independently in core/test_header_name_matching "
                        "and end-to-end on the case-sensitive leg";
    }
    PreprocessResult r;
    (void)ppLexemesWithDirs("#include \"Foo.h\"\nint after;\n", r, {inc}, {},
                            dss::HeaderNameMatching::CaseInsensitive);

    EXPECT_EQ(ppCountCode(r, dss::DiagnosticCode::F_HeaderNameCaseAmbiguous), 1u)
        << "a quote-include collision must be reported HERE — no other tier "
           "ever sees a quote directive when the preprocessor is enabled";
    EXPECT_EQ(ppCountCode(r, dss::DiagnosticCode::P_PreprocessorIncludeError), 0u)
        << "and NOT as the suppressable not-found error, which is what let a "
           "suppression hide the collision entirely";
    std::string const msg =
        ppFirstMessage(r, dss::DiagnosticCode::F_HeaderNameCaseAmbiguous);
    EXPECT_NE(msg.find("foo.h"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Foo.h"), std::string::npos) << msg;

    std::error_code ec;
    fs::remove_all(inc, ec);
}

// H2 — the ANGLE form's SOURCE half. The import resolver's angle arm calls
// `resolveSystemDescriptor` alone, so it never sees a collision among real
// `-I` headers; leaving it verbatim surfaced it as `F_ShippedHeaderNotFound`,
// naming the wrong defect while the diagnostic that lists the colliding paths
// never fired. The preprocessor is the only tier that can report this one.
TEST(Preprocessor, HeaderCaseAngleSourceCollisionIsReportedNotMisdiagnosed) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_hdrcase_angle_collide";
    if (!ppMakeCollidingPair(inc, "bar.h", "Bar.h", "int b;\n")) {
        GTEST_SKIP() << "filesystem folds case — pinned on the case-sensitive leg";
    }
    PreprocessResult r;
    // NO systemDirs at all, so the descriptor half misses and the search
    // reaches the `-I` source half where the collision lives.
    (void)ppLexemesWithDirs("#include <Bar.h>\nint after;\n", r, {inc}, {},
                            dss::HeaderNameMatching::CaseInsensitive);

    EXPECT_EQ(ppCountCode(r, dss::DiagnosticCode::F_HeaderNameCaseAmbiguous), 1u)
        << "the -I source half must be reported by the PREPROCESSOR";
    std::string const msg =
        ppFirstMessage(r, dss::DiagnosticCode::F_HeaderNameCaseAmbiguous);
    EXPECT_NE(msg.find("bar.h"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Bar.h"), std::string::npos) << msg;

    std::error_code ec;
    fs::remove_all(inc, ec);
}

// The AUTHORITATIVE `__has_include` arms (angle + quote). A collision behind a
// `__has_include` guard is the one shape NO later tier can see: the operator
// answers 0, the guarded `#include` never materializes, and nothing else ever
// resolves that name. Answering 0 silently was a silent drop.
TEST(Preprocessor, HeaderCaseHasIncludeCollisionIsReportedInBothForms) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_hdrcase_hasinc_collide";
    if (!ppMakeCollidingPair(inc, "baz.h", "Baz.h", "int z;\n")) {
        GTEST_SKIP() << "filesystem folds case — pinned on the case-sensitive leg";
    }
    for (char const* src :
         {"#if __has_include(<Baz.h>)\nint y;\n#else\nint n;\n#endif\n",
          "#if __has_include(\"Baz.h\")\nint y;\n#else\nint n;\n#endif\n"}) {
        PreprocessResult r;
        auto const lexs =
            ppLexemesWithDirs(src, r, {inc}, {},
                              dss::HeaderNameMatching::CaseInsensitive);
        EXPECT_GE(ppCountCode(r, dss::DiagnosticCode::F_HeaderNameCaseAmbiguous),
                  1u)
            << "a __has_include-guarded collision must fail loud: " << src;
        std::string const msg =
            ppFirstMessage(r, dss::DiagnosticCode::F_HeaderNameCaseAmbiguous);
        EXPECT_NE(msg.find("baz.h"), std::string::npos) << msg;
        EXPECT_NE(msg.find("Baz.h"), std::string::npos) << msg;
        // The operator still answers something (0), but the build cannot
        // proceed silently because the error is unsuppressable.
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "n");
    }
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// The `#embed` directive's own resolution (FC17.9(h)) goes through the same
// policy-aware quote search, so it carries the same collision surface.
TEST(Preprocessor, HeaderCaseEmbedResourceCollisionIsReported) {
    namespace fs = std::filesystem;
    auto inc = ppScratchRoot() / "dss_hdrcase_embed_collide";
    if (!ppMakeCollidingPair(inc, "res.bin", "Res.bin", "AB")) {
        GTEST_SKIP() << "filesystem folds case — pinned on the case-sensitive leg";
    }
    PreprocessResult r;
    (void)ppLexemesWithDirs("int a[] = {\n#embed \"Res.bin\"\n};\n", r, {inc}, {},
                            dss::HeaderNameMatching::CaseInsensitive);
    EXPECT_EQ(ppCountCode(r, dss::DiagnosticCode::F_HeaderNameCaseAmbiguous), 1u)
        << "#embed must not silently resolve a host-dependent resource name";
    EXPECT_EQ(ppCountCode(r, dss::DiagnosticCode::P_PreprocessorEmbed), 0u)
        << "and must not degrade to the generic not-found embed error";
    std::string const msg =
        ppFirstMessage(r, dss::DiagnosticCode::F_HeaderNameCaseAmbiguous);
    EXPECT_NE(msg.find("res.bin"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Res.bin"), std::string::npos) << msg;

    std::error_code ec;
    fs::remove_all(inc, ec);
}

// ═══════════════════════════════════════════════════════════════════════════
// FC18a (D-PP-VA-OPT) -- C23 6.10.5.1 `__VA_OPT__`.
//
// EVERY expectation below is ✔MEASURED, not derived: the same construct was run
// through clang-18 `-std=c23`, clang-19 `-std=c23` and gcc-13 `-std=c2x`, which
// agreed on all of them, and through cl 19.51.36252 `/std:clatest
// /Zc:preprocessor` as a fourth oracle for the expansion cases. Where the
// standard states an answer itself (its EXAMPLE 2: F/G/SDEF/H2..H5) the measured
// answer matched the standard, so the two are not independent votes -- they are
// a cross-check that the probe was written correctly.
//
// The assertions run over the TOKEN stream (`ppLexemes`) rather than over
// stringized text, deliberately: `#` applied to an already-expanded argument has
// its own pre-existing defect (D-PP-STRINGIZE-EXPANDED-ARG-SLICES-WRONG-BYTES),
// and an instrument that shares a defect with its subject cannot detect it.
// ═══════════════════════════════════════════════════════════════════════════
namespace {

// The token sequence, one space between lexemes -- a canonical form directly
// comparable with a reference preprocessor's `-E` output after whitespace
// normalization.
[[nodiscard]] std::string ppJoin(std::vector<std::string> const& lexs) {
    std::string out;
    for (std::size_t i = 0; i < lexs.size(); ++i) {
        if (i != 0) out.push_back(' ');
        out += lexs[i];
    }
    return out;
}
// Lexemes with NO separator. Used for the stringize assertions so they do not
// depend on how many tokens a string literal is split into (opener/body/closer).
[[nodiscard]] std::string ppConcat(std::vector<std::string> const& lexs) {
    std::string out;
    for (auto const& s : lexs) out += s;
    return out;
}

} // namespace

// ── The standard's own EXAMPLE 2, clause by clause. ──────────────────────────

TEST(PreprocessorVaOpt, PresenceAndAbsenceOfTheOptionalTokens) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)\n"
                          "F(a, b, c)\n",
                          r);
    EXPECT_FALSE(r.diagnostics->hasErrors()) << "a well-formed __VA_OPT__ must not error";
    EXPECT_EQ(ppJoin(lexs), "f ( 0 , a , b , c )");

    PreprocessResult r2;
    auto lexs2 = ppLexemes("#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)\n"
                           "F()\n",
                           r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs2), "f ( 0 )")
        << "with no variable arguments the va-opt contributes NOTHING";
}

// ★★ THE LOAD-BEARING TEST OF THIS WHOLE FEATURE.
//
// C23 6.10.5.1p7 keys the emptiness choice on "a (hypothetical) substitution of
// __VA_ARGS__ as neither an operand of # nor ##" -- the MACRO-EXPANDED variable
// arguments. `F(EMP)` passes one RAW argument token, so an implementation that
// tested the raw run would keep the comma and emit `f(0 , )`. The standard says
// `F(EMP)` is "replaced by f(0)", and clang-18/clang-19/gcc-13/cl all agree.
//
// RED-ON-DISABLE: change the predicate in `substituteRange`'s va-opt arm from
// `vaArgs.empty()` to `rawVaArgs.empty()` -- the one-token edit that "looks
// equivalent" and is the same predicate the GNU comma-elision arm four lines
// away legitimately uses -- and all three cases below flip to `f ( 0 , )`.
TEST(PreprocessorVaOpt, EmptinessIsTestedOnTheExpandedArgumentsNotTheRawOnes) {
    // One raw token that expands to nothing.
    PreprocessResult r;
    auto lexs = ppLexemes("#define EMP\n"
                          "#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)\n"
                          "F(EMP)\n",
                          r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "f ( 0 )")
        << "an argument that is PRESENT but expands to NOTHING is empty";

    // A CHAIN of empty macros -- still empty after full expansion.
    PreprocessResult r2;
    auto lexs2 = ppLexemes("#define EMP\n"
                           "#define EMP2 EMP\n"
                           "#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)\n"
                           "F(EMP2)\n",
                           r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs2), "f ( 0 )");

    // TWO raw tokens, both expanding to nothing.
    PreprocessResult r3;
    auto lexs3 = ppLexemes("#define EMP\n"
                           "#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)\n"
                           "F(EMP EMP)\n",
                           r3);
    EXPECT_FALSE(r3.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs3), "f ( 0 )");
}

// ★ WHITE SPACE IS NOT A PREPROCESSING TOKEN — the sibling of the test above,
// and the case that actually caught the bug. `collectArgs` trims only an
// argument's LEADING and TRAILING trivia, so `EMP EMP` leaves one INTERIOR
// whitespace token after both macros expand away. A `vaArgs.empty()` test then
// reports "non-empty" and the va-opt fires. ✔MEASURED on clang-18/clang-19/
// gcc-13: all three treat it as empty in all three positions below.
//
// RED-ON-DISABLE: revert `hasSignificantToken(vaArgs)` to `!vaArgs.empty()` and
// all three assertions flip (`z1`/`v ( 0 , )`/`" "`).
TEST(PreprocessorVaOpt, WhitespaceLeftByAnExpandedAwayArgumentIsStillEmpty) {
    PreprocessResult r;
    auto paste = ppLexemes("#define EMP\n"
                           "#define PQ(X, ...) z ## __VA_OPT__(X)\n"
                           "PQ(EMP EMP, 1)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(paste), "z")
        << "the content left only white space, so the va-opt is a placemarker";

    PreprocessResult r2;
    auto comma = ppLexemes("#define EMP\n"
                           "#define VV(...) v(0 __VA_OPT__(,) __VA_ARGS__)\n"
                           "VV(EMP EMP)\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(comma), "v ( 0 )");

    PreprocessResult r3;
    auto str = ppLexemes("#define EMP\n"
                         "#define SZW(X, ...) #__VA_OPT__(X)\n"
                         "SZW(EMP EMP, 1)\n", r3);
    EXPECT_FALSE(r3.diagnostics->hasErrors());
    EXPECT_EQ(ppConcat(str), "\"\"")
        << "and the stringized form is empty, not a lone space";
}

// ★★ THE GNU IDIOM AND `__VA_OPT__` DISAGREE ON EXACTLY THIS INPUT, AND BOTH
// ANSWERS ARE CORRECT. `,##__VA_ARGS__` asks "was a trailing argument WRITTEN"
// (raw); `__VA_OPT__` asks "does it SUBSTITUTE to anything" (expanded).
// ✔MEASURED on clang-18/clang-19/gcc-13: `g(1 , )` vs `g(1 )`.
//
// This test is the guard against "simplifying" the two onto one predicate. If a
// later cycle routes `__VA_OPT__` through the comma-elision path (or vice
// versa), one of the two lines below goes red.
TEST(PreprocessorVaOpt, GnuCommaElisionAndVaOptDisagreeOnAnEmptyExpandingArg) {
    PreprocessResult rGnu;
    auto gnu = ppLexemes("#define EMP\n"
                         "#define GNU(f, ...) g(f , ## __VA_ARGS__)\n"
                         "GNU(1, EMP)\n",
                         rGnu);
    EXPECT_FALSE(rGnu.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(gnu), "g ( 1 , )")
        << "GNU comma-elision keys off the RAW argument, which is PRESENT";

    PreprocessResult rStd;
    auto std_ = ppLexemes("#define EMP\n"
                          "#define STD(f, ...) g(f __VA_OPT__(,) __VA_ARGS__)\n"
                          "STD(1, EMP)\n",
                          rStd);
    EXPECT_FALSE(rStd.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(std_), "g ( 1 )")
        << "__VA_OPT__ keys off the EXPANDED argument, which is EMPTY";
}

TEST(PreprocessorVaOpt, NamedParameterThenOptionalComma) {
    PreprocessResult r;
    auto a = ppLexemes("#define G(X, ...) f(0, X __VA_OPT__(,) __VA_ARGS__)\n"
                       "G(a, b, c)\n", r);
    EXPECT_EQ(ppJoin(a), "f ( 0 , a , b , c )");
    PreprocessResult r2;
    auto b = ppLexemes("#define G(X, ...) f(0, X __VA_OPT__(,) __VA_ARGS__)\n"
                       "G(a, )\n", r2);
    EXPECT_EQ(ppJoin(b), "f ( 0 , a )")
        << "an explicitly EMPTY trailing argument is still empty";
    PreprocessResult r3;
    auto c = ppLexemes("#define G(X, ...) f(0, X __VA_OPT__(,) __VA_ARGS__)\n"
                       "G(a)\n", r3);
    EXPECT_EQ(ppJoin(c), "f ( 0 , a )");
}

// The standard's SDEF -- the motivating real-world shape (an optional
// initializer). Also pins that `{`/`}` inside the content are ordinary tokens:
// only PARENTHESES participate in finding the closing `)` (6.10.5.1p3).
TEST(PreprocessorVaOpt, OptionalInitializerShape) {
    PreprocessResult r;
    auto a = ppLexemes("#define SDEF(sname, ...) S sname __VA_OPT__(= { __VA_ARGS__ })\n"
                       "SDEF(foo);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(a), "S foo ;");
    PreprocessResult r2;
    auto b = ppLexemes("#define SDEF(sname, ...) S sname __VA_OPT__(= { __VA_ARGS__ })\n"
                       "SDEF(bar, 1, 2);\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(b), "S bar = { 1 , 2 } ;");
}

// H2: `##` INSIDE the content pastes normally.
TEST(PreprocessorVaOpt, PasteInsideTheContent) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define H2(X, Y, ...) __VA_OPT__(X ## Y,) __VA_ARGS__\n"
                          "H2(a, b, c, d)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "ab , c , d");
}

// ★ H4: the va-opt RESULT is an operand of an OUTSIDE `##`, and an interior
// `##` produced a placemarker. Both must be handled, and in the right order.
// `H4(, 1)` = `a b`: inside, `X ## X` with X empty is placemarker##placemarker
// = placemarker; outside, `<pm> ## b` = `b`.
TEST(PreprocessorVaOpt, PlacemarkerFromInteriorPasteThenExteriorPaste) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define H4(X, ...) __VA_OPT__(a X ## X) ## b\n"
                          "H4(, 1)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "a b");
}

// H5: an EMPTY va-opt content is a placemarker, and a placemarker survives long
// enough to satisfy the `##` operands around it, then disappears.
TEST(PreprocessorVaOpt, EmptyContentIsAPlacemarkerNotADanglingPaste) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define H5A(...) __VA_OPT__()/**/__VA_OPT__()\n"
                          "#define H5B(X) a ## X ## b\n"
                          "#define H5C(X) H5B(X)\n"
                          "H5C(H5A())\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "ab");
}

// EXAMPLE 1: the closing `)` is found by paren MATCHING, so a `(` that arrives
// from a nested macro invocation does not confuse the scan.
TEST(PreprocessorVaOpt, StandardExampleOneWithLparenMacro) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define LPAREN() (\n"
                          "#define G(Q) 42\n"
                          "#define F(R, X, ...) __VA_OPT__(G R X) )\n"
                          "int x = F(LPAREN(), 0, ~);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "int x = 42 ;");
}

// ── Structural cases beyond the standard's examples. ─────────────────────────

// ✔MEASURED: white space between `__VA_OPT__` and its `(` is allowed. This is
// the OPPOSITE of the `#define F (x)` rule, where a space makes the macro
// object-like -- there the adjacency is what distinguishes two forms, here the
// grammar is just `__VA_OPT__ ( pp-tokens )` over preprocessing tokens.
TEST(PreprocessorVaOpt, SpaceBetweenIntroducerAndParenIsAllowed) {
    PreprocessResult r;
    auto a = ppLexemes("#define SP(...) s(0 __VA_OPT__ (,) __VA_ARGS__)\nSP()\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(a), "s ( 0 )");
    PreprocessResult r2;
    auto b = ppLexemes("#define SP(...) s(0 __VA_OPT__ (,) __VA_ARGS__)\nSP(9)\n", r2);
    EXPECT_EQ(ppJoin(b), "s ( 0 , 9 )");
}

TEST(PreprocessorVaOpt, EmptyContentWithNonEmptyArgumentsYieldsNothing) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define EC(...) e( __VA_OPT__() __VA_ARGS__ )\nEC(7)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "e ( 7 )");
}

TEST(PreprocessorVaOpt, TwoVaOptsInOneReplacementList) {
    PreprocessResult r;
    auto a = ppLexemes("#define TW(...) t( __VA_OPT__([) __VA_ARGS__ __VA_OPT__(]) )\nTW()\n", r);
    EXPECT_EQ(ppJoin(a), "t ( )");
    PreprocessResult r2;
    auto b = ppLexemes("#define TW(...) t( __VA_OPT__([) __VA_ARGS__ __VA_OPT__(]) )\nTW(3)\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(b), "t ( [ 3 ] )");
}

// Nested parentheses inside the content must not terminate the scan early
// (6.10.5.1p3: "skipping intervening pairs of matching left and right
// parentheses"). The content also carries a COMMA at depth > 0.
TEST(PreprocessorVaOpt, NestedParensInsideTheContent) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define NP(...) n( __VA_OPT__( (a,(b)) ) __VA_ARGS__ )\nNP(4)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "n ( ( a , ( b ) ) 4 )");
}

TEST(PreprocessorVaOpt, NamedParameterInsideTheContentSubstitutes) {
    PreprocessResult r;
    auto a = ppLexemes("#define PN(X, ...) p( X __VA_OPT__(X X) )\nPN(q)\n", r);
    EXPECT_EQ(ppJoin(a), "p ( q )");
    PreprocessResult r2;
    auto b = ppLexemes("#define PN(X, ...) p( X __VA_OPT__(X X) )\nPN(q, r)\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(b), "p ( q q q )");
}

// ★ The va-opt as a `##` operand. Empty -> placemarker -> the paste yields the
// other operand (NOT a dangling-`##` constraint error). ✔MEASURED `z` / `zw`.
TEST(PreprocessorVaOpt, VaOptAsAPasteOperand) {
    PreprocessResult r;
    auto a = ppLexemes("#define PL(...) z ## __VA_OPT__(w)\nPL()\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "an empty va-opt is a PLACEMARKER, so the `##` has an operand";
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    EXPECT_EQ(ppJoin(a), "z");

    PreprocessResult r2;
    auto b = ppLexemes("#define PL(...) z ## __VA_OPT__(w)\nPL(1)\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(b), "zw");
}

// The same, but the content itself substitutes to nothing (an empty parameter),
// and separately an entirely EMPTY content -- both must become placemarkers.
TEST(PreprocessorVaOpt, PasteOperandWhoseContentSubstitutesToNothing) {
    PreprocessResult r;
    auto a = ppLexemes("#define PZ(...) z ## __VA_OPT__()\nPZ(1)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    EXPECT_EQ(ppJoin(a), "z");

    PreprocessResult r2;
    auto b = ppLexemes("#define PQ(X, ...) z ## __VA_OPT__(X)\nPQ(,1)\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(b), "z");

    PreprocessResult r3;
    auto c = ppLexemes("#define PQ(X, ...) z ## __VA_OPT__(X)\nPQ(w,1)\n", r3);
    EXPECT_EQ(ppJoin(c), "zw");
}

TEST(PreprocessorVaOpt, VaOptBetweenTwoPastes) {
    PreprocessResult r;
    auto a = ppLexemes("#define BB(...) a ## __VA_OPT__(m) ## b\nBB()\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(a), "ab");
    PreprocessResult r2;
    auto b = ppLexemes("#define BB(...) a ## __VA_OPT__(m) ## b\nBB(1)\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(b), "amb");
}

TEST(PreprocessorVaOpt, VaArgsAsAPasteOperandInsideTheContent) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define VP(...) v( __VA_OPT__(q ## __VA_ARGS__) )\nVP(1)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "v ( q1 )");
}

// The content is NOT rescanned in place; the OUTER rescan expands what it
// produced (6.10.5.1p7: the argument is the expansion "before ... rescanning").
// Observationally: `INNER(__VA_ARGS__)` inside the content still ends up
// expanded, but by the ordinary post-substitution rescan.
TEST(PreprocessorVaOpt, ContentIsRescannedByTheOuterPass) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define INNER(x) [x]\n"
                          "#define RS(...) r( __VA_OPT__(INNER(__VA_ARGS__)) )\n"
                          "RS(5)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppJoin(lexs), "r ( [ 5 ] )");
}

// ── Stringizing a va-opt (6.10.5.1p4 + 6.10.5.2). ───────────────────────────
//
// ★ THE SPELLING IS NOT A BLIND SPACE-JOIN. 6.10.5.2p3: white space BETWEEN the
// stringizing argument's tokens becomes one space, and where there was none,
// none is inserted. ✔MEASURED `"a+b"` vs `"a + b"`, and `"p+p"` for a
// SUBSTITUTED parameter -- adjacency comes from the replacement list.
TEST(PreprocessorVaOpt, StringizePreservesOriginalAdjacency) {
    PreprocessResult r;
    auto tight = ppLexemes("#define SZ3(...) #__VA_OPT__(a+b)\nSZ3(1)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppConcat(tight), "\"a+b\"");

    PreprocessResult r2;
    auto spaced = ppLexemes("#define SZ4(...) #__VA_OPT__(a + b)\nSZ4(1)\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppConcat(spaced), "\"a + b\"");

    PreprocessResult r3;
    auto param = ppLexemes("#define SZ5(X, ...) #__VA_OPT__(X+X)\nSZ5(p, 1)\n", r3);
    EXPECT_FALSE(r3.diagnostics->hasErrors());
    EXPECT_EQ(ppConcat(param), "\"p+p\"")
        << "a substituted parameter keeps the replacement list's adjacency";

    PreprocessResult r4;
    auto runs = ppLexemes("#define SZ(...) #__VA_OPT__(a b   c)\nSZ(1)\n", r4);
    EXPECT_FALSE(r4.diagnostics->hasErrors());
    EXPECT_EQ(ppConcat(runs), "\"a b c\"")
        << "a RUN of white space collapses to exactly one space";
}

// An empty variable-argument substitution makes the whole va-opt a placemarker,
// which stringization removes -- leaving `""` (6.10.5.2p4). And H3: interior
// pastes must actually RUN, because spelling the content literally would give
// "####" instead of "".
TEST(PreprocessorVaOpt, StringizeOfAnEmptyOrPlacemarkerOnlyVaOptIsEmptyString) {
    PreprocessResult r;
    auto empty = ppLexemes("#define SZ(...) #__VA_OPT__(a b c)\nSZ()\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppConcat(empty), "\"\"");

    PreprocessResult r2;
    auto h3 = ppLexemes("#define H3(X, ...) #__VA_OPT__(X##X X##X)\nH3(, 0)\n", r2);
    EXPECT_FALSE(r2.diagnostics->hasErrors());
    EXPECT_EQ(ppConcat(h3), "\"\"")
        << "the interior pastes yield placemarkers, which stringization removes";
}

TEST(PreprocessorVaOpt, StringizeOfVaArgsInsideTheContent) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define SZ7(...) #__VA_OPT__(__VA_ARGS__)\nSZ7(a , b)\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_EQ(ppConcat(lexs), "\"a , b\"");
}

// ── Constraint violations: every one must FAIL LOUD AND BY NAME. ─────────────
//
// ★ THE POINT OF THESE IS THE MESSAGE, NOT MERELY THE REJECTION. Before FC18a a
// `__VA_OPT__` reached the PARSER as an unknown identifier and the user was told
// `expected 'ParenClose'` -- true about the parser's stack, silent about the
// construct. Each case below asserts the diagnostic NAMES `__VA_OPT__`.
namespace {
// Every diagnostic's text, joined. Code-agnostic on purpose: these tests assert
// on WHAT THE USER IS TOLD, and pinning the code as well would make the test
// re-cut itself every time a message legitimately moves between codes.
[[nodiscard]] std::string ppAllMessages(PreprocessResult const& r) {
    std::string out;
    for (auto const& d : r.diagnostics->all()) {
        out += d.actual;
        out.push_back('\n');
    }
    return out;
}
} // namespace

TEST(PreprocessorVaOpt, InANonVariadicFunctionLikeMacroFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define NV(X) v( X __VA_OPT__(,) )\nNV(1)\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
    std::string const m = ppAllMessages(r);
    EXPECT_NE(m.find("__VA_OPT__"), std::string::npos)
        << "the diagnostic must NAME the construct, not say \"expected ')'\": " << m;
    EXPECT_NE(m.find("variadic"), std::string::npos) << m;
}

TEST(PreprocessorVaOpt, InAnObjectLikeMacroFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define OL __VA_OPT__(x)\nOL\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
    EXPECT_NE(ppAllMessages(r).find("__VA_OPT__"), std::string::npos);
}

TEST(PreprocessorVaOpt, WithoutAnOpeningParenFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define NP(...) q( __VA_OPT__ )\nNP(1)\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
    std::string const m = ppAllMessages(r);
    EXPECT_NE(m.find("__VA_OPT__"), std::string::npos) << m;
    EXPECT_NE(m.find("("), std::string::npos)
        << "must say a '(' is required: " << m;
}

TEST(PreprocessorVaOpt, UnterminatedContentFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define UT(...) q __VA_OPT__( a b\nUT(1)\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
    std::string const m = ppAllMessages(r);
    EXPECT_NE(m.find("__VA_OPT__"), std::string::npos) << m;
    EXPECT_NE(m.find(")"), std::string::npos)
        << "must say the closing ')' is missing: " << m;
}

TEST(PreprocessorVaOpt, NestedVaOptFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define N(...) n( __VA_OPT__( a __VA_OPT__(b) ) )\nN(1)\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
    std::string const m = ppAllMessages(r);
    EXPECT_NE(m.find("__VA_OPT__"), std::string::npos) << m;
    EXPECT_NE(m.find("nested"), std::string::npos) << m;
}

// The standard's H1: `##` may not sit at either end of the content, because the
// content "shall form a valid replacement list" (6.10.5.1p3 -> 6.10.5.3p1).
TEST(PreprocessorVaOpt, PasteAtEitherEndOfTheContentFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define H1(X, ...) X __VA_OPT__(##) __VA_ARGS__\nH1(1,2)\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPaste));
    EXPECT_NE(ppAllMessages(r).find("__VA_OPT__"), std::string::npos);

    PreprocessResult r2;
    (void)ppLexemes("#define PE(X, ...) __VA_OPT__(X ##) __VA_ARGS__\nPE(1,2)\n", r2);
    EXPECT_TRUE(r2.diagnostics->hasErrors());
    EXPECT_TRUE(hasPPCode(r2, DiagnosticCode::P_PreprocessorPaste));
}

TEST(PreprocessorVaOpt, DefineOrUndefOfTheReservedNameFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#define __VA_OPT__ 1\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable));

    PreprocessResult r2;
    (void)ppLexemes("#undef __VA_OPT__\n", r2);
    EXPECT_TRUE(r2.diagnostics->hasErrors());
    EXPECT_TRUE(hasPPCode(r2, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable));
}

// ── Agnosticism: the spelling comes from CONFIG, never from the C source. ────
//
// RED-ON-DISABLE: replace `cfg().vaOptName` with a literal "__VA_OPT__" anywhere
// in the engine and this test fails -- under the rebound config the engine would
// still recognise the C spelling (so `DSS_OPT` would pass through verbatim and
// the joined output would not contain `w`), while `__VA_OPT__` would be treated
// as a construct in a config that never declared one.
TEST(PreprocessorVaOpt, VaOptNameIsConfigDrivenNotHardcoded) {
    std::string text = loadShippedCText();
    ASSERT_FALSE(text.empty()) << "could not locate shipped c config";
    const std::string from = "\"vaOptName\": \"__VA_OPT__\"";
    const std::string to   = "\"vaOptName\": \"DSS_OPT\"";
    auto const pos = text.find(from);
    ASSERT_NE(pos, std::string::npos)
        << "shipped c config no longer carries vaOptName=__VA_OPT__";
    text.replace(pos, from.size(), to);

    auto loaded = GrammarSchema::loadFromText(text, "<rebound-vaopt-c>");
    ASSERT_TRUE(loaded.has_value())
        << "rebound schema should still load: "
        << (loaded.error().empty() ? "<no diagnostics>" : loaded.error()[0].message);
    std::shared_ptr<GrammarSchema const> schema = *loaded;
    ASSERT_EQ(schema->preprocess().vaOptName, "DSS_OPT");

    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;

    // The REBOUND spelling now behaves as the construct.
    auto buf = SourceBuffer::fromString(
        std::string{"#define PL(...) z ## DSS_OPT(w)\nPL(1)\n"}, "main.c");
    PreprocessResult r =
        preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching, DiagnosticBudget::libraryDefault());
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "the CONFIG-declared spelling must be the one the engine acts on";
    std::string joined;
    for (Token const& t : r.tokens) {
        if (t.coreKind == CoreTokenKind::Eof) continue;
        if (t.coreKind == CoreTokenKind::Whitespace) continue;
        if (t.coreKind == CoreTokenKind::Newline) continue;
        joined += std::string{r.synthBuffer->slice(t.span)};
    }
    EXPECT_EQ(joined, "zw")
        << "under the rebound config `DSS_OPT(w)` must expand, proving the "
           "spelling is read from config rather than hard-coded";
}

// ═══════════════════════════════════════════════════════════════════════════
// D-PP-STRINGIZE-EXPANDED-ARG-SLICES-WRONG-BYTES — `#` spells a TOKEN SEQUENCE
// ═══════════════════════════════════════════════════════════════════════════
//
// `#` used to build its product by slicing ONE contiguous byte range,
// `[raw.front().span.start(), raw.back().span.end())`. That is only exact while
// the stringizing argument really is unbroken call-site text. The `XSTR(...)`
// idiom breaks it: the inner macro receives an argument that has ALREADY been
// expanded, so its tokens come from the `#define` line, the call site and
// `productText_` interleaved, and `front()..back()` spans whatever lies between
// two unrelated buffer positions. No diagnostic — the wrong string just shipped.
//
// EVERY expectation below is ✔MEASURED on four reference preprocessors that
// agreed exactly: clang-18 `-std=c23`, clang-19 `-std=c23`, gcc-13 `-std=c2x`,
// and cl 19.51.36252 `/std:clatest /Zc:preprocessor`.
//
// RED-ON-DISABLE: restore the slice and `NestedStringize*` reports the `#define`
// line's own text (`"g(a, b)"`), a comment (`"a/*x*/b"`), or the remainder of the
// file; drop the per-token spelling and `*InsideATokenSpelling` collapses the two
// spaces it must keep; restore `applySpacing`'s span loop and
// `*ArgumentThatArrivedThroughAnExpansion` says `"z +b"`.

// ★ THE CORE DIFFERENTIAL. The argument is the EXPANSION of an inner macro, so
// its tokens are not contiguous anywhere; the product must spell those tokens.
TEST(PreprocessorStringizeTokenSequence,
     NestedStringizeSpellsTheExpandedArgumentNotTheDefineLine) {
    // The two-level idiom shared by every real "stringify after expansion" macro.
    const std::string prelude =
        "#define STR(...) #__VA_ARGS__\n"
        "#define XSTR(...) STR(__VA_ARGS__)\n"
        "#define PLAIN(a,b) g(a, b)\n"
        "#define VA(f,...) k(f, __VA_ARGS__)\n"
        "#define MIX(a) q(a, w)\n";

    // CONTROL (single level): `#` does NOT pre-expand its own operand, so the raw
    // spelling survives. This passed before the fix too — it is here so a failure
    // in the cases below cannot be blamed on stringize being broken generally.
    PreprocessResult rc0;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "STR(PLAIN(1,2))\n", rc0)),
              "\"PLAIN(1,2)\"")
        << "a `#` operand is never pre-expanded (C 6.10.3.2p2)";
    EXPECT_FALSE(rc0.diagnostics->hasErrors());

    PreprocessResult r1;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(PLAIN(1,2))\n", r1)),
              "\"g(1, 2)\"")
        << "the product must spell the ARGUMENT's tokens; a contiguous slice "
           "reads PLAIN's `#define` line instead and yields \"g(a, b)\"";
    EXPECT_FALSE(r1.diagnostics->hasErrors());

    PreprocessResult r2;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(VA(1,2,3))\n", r2)),
              "\"k(1, 2,3)\"")
        << "note the ASYMMETRIC spacing is correct: the `, ` comes from VA's "
           "replacement list and the `2,3` from the call site, where there is "
           "no space — a blind space-join would say \"k(1, 2, 3)\"";
    EXPECT_FALSE(r2.diagnostics->hasErrors());

    // The argument mixes a CALL-SITE token (`z`) with REPLACEMENT-ORIGIN ones.
    PreprocessResult r3;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(MIX(z))\n", r3)), "\"q(z, w)\"");
    EXPECT_FALSE(r3.diagnostics->hasErrors());

    // Two levels of the inner macro, then two levels of stringize.
    PreprocessResult r4;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(PLAIN(PLAIN(1,2),3))\n", r4)),
              "\"g(g(1, 2), 3)\"");
    EXPECT_FALSE(r4.diagnostics->hasErrors());

    // Three levels of stringize indirection still lands on the same answer.
    PreprocessResult r5;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define YSTR(...) XSTR(__VA_ARGS__)\n"
                                          "YSTR(PLAIN(1,2))\n", r5)),
              "\"g(1, 2)\"");
    EXPECT_FALSE(r5.diagnostics->hasErrors());

    // CONTROL (negative): an operand that is not a macro is unchanged by the
    // extra expansion level, so both idioms agree. Guards against a "fix" that
    // makes every product go through some transform.
    PreprocessResult r6;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(notamacro)\n", r6)),
              "\"notamacro\"");
    EXPECT_FALSE(r6.diagnostics->hasErrors());
}

// ★ WHITE SPACE INSIDE A TOKEN'S OWN SPELLING SURVIVES VERBATIM. C23 6.10.5.2p2
// collapses white space BETWEEN the argument's preprocessing tokens; two spaces
// inside a string or character literal are part of THAT TOKEN and are not
// between anything. The old whole-slice collapse could not tell the difference.
TEST(PreprocessorStringizeTokenSequence,
     StringizePreservesWhiteSpaceInsideATokenSpelling) {
    const std::string def = "#define S(x) #x\n";

    // CONTROL: white space BETWEEN tokens still collapses to exactly one space.
    PreprocessResult rc0;
    EXPECT_EQ(ppConcat(ppLexemes(def + "S(a     b)\n", rc0)), "\"a b\"")
        << "a run of white space BETWEEN tokens is one space (6.10.5.2p2)";
    EXPECT_FALSE(rc0.diagnostics->hasErrors());

    PreprocessResult r1;
    EXPECT_EQ(ppConcat(ppLexemes(def + "S(\"a  b\")\n", r1)),
              "\"\\\"a  b\\\"\"")
        << "both interior spaces belong to the literal's spelling and must "
           "survive; collapsing them ships a string the source never wrote";
    EXPECT_FALSE(r1.diagnostics->hasErrors());

    PreprocessResult r2;
    EXPECT_EQ(ppConcat(ppLexemes(def + "S(\"a\tb\")\n", r2)),
              "\"\\\"a\tb\\\"\"")
        << "an interior TAB is preserved as a TAB, not turned into a space";
    EXPECT_FALSE(r2.diagnostics->hasErrors());

    PreprocessResult r3;
    EXPECT_EQ(ppConcat(ppLexemes(def + "S('a  b')\n", r3)), "\"'a  b'\"")
        << "the rule is about the TOKEN, so a character constant behaves alike";
    EXPECT_FALSE(r3.diagnostics->hasErrors());

    // Escaping and interior spacing are independent and must both hold.
    PreprocessResult r4;
    EXPECT_EQ(ppConcat(ppLexemes(def + "S(\"a  \\\"  b\")\n", r4)),
              "\"\\\"a  \\\\\\\"  b\\\"\"")
        << "an escaped quote inside the literal gains its `\\` AND keeps its "
           "surrounding interior spaces";
    EXPECT_FALSE(r4.diagnostics->hasErrors());

    PreprocessResult r5;
    EXPECT_EQ(ppConcat(ppLexemes(def + "S(\"a  \\\\  b\")\n", r5)),
              "\"\\\"a  \\\\\\\\  b\\\"\"");
    EXPECT_FALSE(r5.diagnostics->hasErrors());

    // ★★ THE CASE THAT PINS BOTH RULES AT ONCE, and the reason the spelling has
    // to be built per-token: the same product collapses between tokens while
    // preserving inside them. No byte-range walk can express this.
    PreprocessResult r6;
    EXPECT_EQ(ppConcat(ppLexemes(def + "S(f(\"a  b\" ,   \"c  d\"))\n", r6)),
              "\"f(\\\"a  b\\\" , \\\"c  d\\\")\"")
        << "interior white space kept, between-token white space collapsed, in "
           "ONE product";
    EXPECT_FALSE(r6.diagnostics->hasErrors());

    // ... and it still holds when the literal reaches `#` through an expansion.
    PreprocessResult r7;
    EXPECT_EQ(ppConcat(ppLexemes("#define STR(...) #__VA_ARGS__\n"
                                 "#define XSTR(...) STR(__VA_ARGS__)\n"
                                 "#define PLAIN(a,b) g(a, b)\n"
                                 "XSTR(PLAIN(\"a  b\",2))\n", r7)),
              "\"g(\\\"a  b\\\", 2)\"");
    EXPECT_FALSE(r7.diagnostics->hasErrors());

    // CONTROL (negative): a literal with NO interior white space is untouched,
    // so the fix cannot be "stop normalizing anything at all".
    PreprocessResult r8;
    EXPECT_EQ(ppConcat(ppLexemes(def + "S(\"ab\")\n", r8)), "\"\\\"ab\\\"\"");
    EXPECT_FALSE(r8.diagnostics->hasErrors());
}

// ★ THE THIRD SITE. `applySpacing` reconstructed tokens 2..N of a substituted run
// with `span.start() != prev.span.end()`, on the same false contiguity premise.
// This was already REACHABLE through `#__VA_OPT__(X)` — i.e. it was a live
// wrong-bytes defect, not a latent one.
TEST(PreprocessorStringizeTokenSequence,
     StringizeSpacingOfAnArgumentThatArrivedThroughAnExpansion) {
    const std::string prelude =
        "#define SP(X, ...) #__VA_OPT__(X)\n"
        "#define CAT2(a) a+b\n"
        "#define CAT3(a) a + b\n";

    // CONTROL: a one-token argument has no interior adjacency to get wrong.
    PreprocessResult rc0;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "SP(q, 1)\n", rc0)), "\"q\"");
    EXPECT_FALSE(rc0.diagnostics->hasErrors());

    // CONTROL: an argument whose tokens ARE one contiguous `#define` line. This
    // is the case the span reconstruction got right by luck, and it must stay
    // right — so a fix cannot simply hardcode "never spaced".
    PreprocessResult rc1;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define TIGHT x+y\nSP(TIGHT, 1)\n",
                                 rc1)),
              "\"x+y\"");
    EXPECT_FALSE(rc1.diagnostics->hasErrors());
    PreprocessResult rc2;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define SPACED x + y\nSP(SPACED, 1)\n",
                                 rc2)),
              "\"x + y\"");
    EXPECT_FALSE(rc2.diagnostics->hasErrors());

    // ★ THE DEFECT: `z` is a CALL-SITE token, `+` and `b` come from CAT2's
    // `#define` line. Nothing separates them in the construct, but they are far
    // apart in the buffers, so span arithmetic invents a space -> "z +b".
    PreprocessResult r1;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "SP(CAT2(z), 1)\n", r1)), "\"z+b\"")
        << "adjacency belongs to the construct; the tokens' byte positions are "
           "unrelated once the argument came through an expansion";
    EXPECT_FALSE(r1.diagnostics->hasErrors());

    // The same shape spelled WITH spaces must still report them — so the fix is
    // carrying the real answer, not suppressing spaces.
    PreprocessResult r2;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "SP(CAT3(z), 1)\n", r2)), "\"z + b\"");
    EXPECT_FALSE(r2.diagnostics->hasErrors());
}

// ★ ADJACENCY MUST SURVIVE EVERY BOUNDARY A TOKEN CROSSES: a comment, an
// object-like replacement, a substitution taking a macro name's place, and a
// `##` product. Each of these is a place the bit is PROPAGATED rather than
// recomputed, and each was measured against the four oracles.
TEST(PreprocessorStringizeTokenSequence,
     StringizeSpacingAcrossSubstitutionAndPasteBoundaries) {
    const std::string prelude =
        "#define STR(...) #__VA_ARGS__\n"
        "#define XSTR(...) STR(__VA_ARGS__)\n"
        "#define PLAIN(a,b) g(a, b)\n";

    // A COMMENT is white space (translation phase 3), so it becomes one space —
    // and must not appear in the product. The slice used to copy it verbatim.
    PreprocessResult r1;
    EXPECT_EQ(ppConcat(ppLexemes("#define S(x) #x\nS(a/*x*/b)\n", r1)), "\"a b\"")
        << "a comment is white space, not spelling: it must never reach the "
           "string literal";
    EXPECT_FALSE(r1.diagnostics->hasErrors());
    PreprocessResult r2;
    EXPECT_EQ(ppConcat(ppLexemes("#define S(x) #x\nS(a/*x*/  b)\n", r2)),
              "\"a b\"")
        << "a comment plus real spaces is still exactly one space";
    EXPECT_FALSE(r2.diagnostics->hasErrors());

    // An OBJECT-LIKE replacement list carries its own adjacency, read from the
    // `#define` line's spans (trivia is dropped at definition time, but the list
    // is one contiguous run of that line, so the gap is exact).
    PreprocessResult r3;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define OBJ p+q\nXSTR(OBJ)\n", r3)),
              "\"p+q\"");
    EXPECT_FALSE(r3.diagnostics->hasErrors());
    PreprocessResult r4;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define OBJS p + q\nXSTR(OBJS)\n", r4)),
              "\"p + q\"");
    EXPECT_FALSE(r4.diagnostics->hasErrors());

    // A SUBSTITUTION takes the invoking NAME's place, so its first token keeps
    // the spacing that preceded the invocation — not the define line's.
    PreprocessResult r5;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(a PLAIN(1,2))\n", r5)),
              "\"a g(1, 2)\"")
        << "the `g` inherits the space that preceded `PLAIN`";
    EXPECT_FALSE(r5.diagnostics->hasErrors());
    PreprocessResult r6;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(a+PLAIN(1,2))\n", r6)),
              "\"a+g(1, 2)\"")
        << "...and inherits NO space when the invocation had none";
    EXPECT_FALSE(r6.diagnostics->hasErrors());

    // A `##` PRODUCT stands where its LEFT operand stood: the paste consumed the
    // boundary BETWEEN the operands, not the one before them.
    PreprocessResult r7;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define P(a,b) a##b\nXSTR(x P(1,2))\n",
                                 r7)),
              "\"x 12\"");
    EXPECT_FALSE(r7.diagnostics->hasErrors());
    PreprocessResult r8;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define P(a,b) a##b\nXSTR(x+P(1,2))\n",
                                 r8)),
              "\"x+12\"");
    EXPECT_FALSE(r8.diagnostics->hasErrors());

    // A `##` product built from an ARGUMENT and a replacement-list token.
    PreprocessResult r9;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define SUF(a) a##_z\nXSTR(w SUF(v))\n",
                                 r9)),
              "\"w v_z\"");
    EXPECT_FALSE(r9.diagnostics->hasErrors());

    // LEADING/TRAILING white space of the stringizing argument is DELETED
    // (6.10.5.2p2), even when the argument arrived through an expansion.
    PreprocessResult r10;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(   PLAIN(1,2)   )\n", r10)),
              "\"g(1, 2)\"");
    EXPECT_FALSE(r10.diagnostics->hasErrors());

    // A replacement list whose own tokens are SPACED, with arguments substituted
    // into it: the space is the define line's, the values are the call site's.
    PreprocessResult r11;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "#define TWO(a,b) a b\nXSTR(TWO(1,2))\n",
                                 r11)),
              "\"1 2\"")
        << "the slice used to answer \"1,2\" — the call site's comma, which is "
           "not in the construct at all";
    EXPECT_FALSE(r11.diagnostics->hasErrors());

    // An ARGUMENT with interior white space, carried through the nesting.
    PreprocessResult r12;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR(PLAIN(x  y,2))\n", r12)),
              "\"g(x y, 2)\"");
    EXPECT_FALSE(r12.diagnostics->hasErrors());

    // CONTROL: an empty stringizing argument is still `""` (6.10.5.2p4).
    PreprocessResult r13;
    EXPECT_EQ(ppConcat(ppLexemes(prelude + "XSTR()\n", r13)), "\"\"");
    EXPECT_FALSE(r13.diagnostics->hasErrors());
}

// ─────────────────────────────────────────────────────────────────────────────
// D-C-PREPROCESSED-INPUT-REFUSES-GCC-LINEMARKERS — the GNU LINEMARKER form
// `# N "file" [flags]`, which is what `gcc -E` / `clang -E` write where a
// `#line` would go. Same presumed-position facility, different surface: both
// spellings land in ONE `recordPresumedPosition`, so a drift between them is
// structurally impossible rather than merely watched for.
//
// Config-driven (`preprocess.lineMarker`), so a language WITHOUT the block
// leaves a digit-led directive on the generic unsupported-directive fail-loud —
// which is exactly what every one of these TUs did before this cycle.
// Every assertion below is RED-ON-DISABLE.
// ─────────────────────────────────────────────────────────────────────────────

// THE headline: the marker renumbers the FOLLOWING line, same off-by-one rule
// `#line` obeys. RED-ON-DISABLE: remove the `lineMarker` dispatch arm and this
// TU goes red with P_PreprocessorUnsupported instead.
TEST(PreprocessorLineMarker, RenumbersFollowingLineLikeHashLine) {
    PreprocessResult r;
    //                    line: 1              2
    auto lexs = ppLexemes("# 100 \"virtual.h\"\nint x = __LINE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u) << "expected: int x = 100 ;";
    EXPECT_EQ(lexs[3], "100")
        << "a linemarker numbers the FOLLOWING line N, exactly as `#line N` does";
}

// The FLAG TAIL must not disturb the numbering. An implementation that read the
// number, then choked on (or mis-consumed) the flags would fail HERE while
// passing the bare form above — which is the shape 39 of gcc's 154 markers take.
TEST(PreprocessorLineMarker, FullGccFlagTailDoesNotDisturbTheNumbering) {
    PreprocessResult r;
    auto lexs = ppLexemes("# 100 \"virtual.h\" 1 3 4\nint x = __LINE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "100");
}

// The presumed FILE drives `__FILE__`. Separate from the line pin on purpose:
// an implementation that set one and not the other passes the other test.
TEST(PreprocessorLineMarker, SetsThePresumedFileName) {
    PreprocessResult r;
    auto lexs = ppLexemes("# 7 \"Alpha.h\" 2 3 4\n"
                          "const char* f = __FILE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    bool sawAlpha = false;
    for (auto const& s : lexs) {
        if (s.find("Alpha.h") != std::string::npos) sawAlpha = true;
    }
    EXPECT_TRUE(sawAlpha) << "a linemarker's quoted operand must become __FILE__";
}

// ★ LINE ZERO IS LEGAL HERE AND ILLEGAL IN `#line`, and that divergence is
// MEASURED, not chosen: gcc 13.3.0's own `-E` output opens with `# 0 "tu.c"` and
// gcc recompiles that output rc=0. Importing C23 6.10.4p2's 1..2147483647 floor
// would make DSS refuse the very bytes this row exists to read.
// RED-ON-DISABLE: reuse `handleLine`'s `n == 0` arm and this goes red.
TEST(PreprocessorLineMarker, LineZeroIsAcceptedUnlikeHashLine) {
    PreprocessResult r;
    auto lexs = ppLexemes("# 0 \"builtin.h\"\nint x = __LINE__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "gcc emits `# 0 \"...\"` in its own -E output; refusing it defeats "
           "the whole point of accepting linemarkers";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "0");
}

// ...and the CONTROL that keeps the divergence honest: `#line 0` is still a
// constraint violation. A shared implementation that relaxed BOTH would pass the
// test above while quietly breaking C23 conformance for the ISO spelling.
TEST(PreprocessorLineMarker, HashLineZeroStillFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("#line 0 \"f.c\"\nint x = __LINE__;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "`#line 0` is out of the C23 6.10.4p2 range even though a LINEMARKER "
           "0 is legal — the two surfaces diverge here deliberately";
}

// An UNDECLARED flag digit fails loud. MEASURED: gcc 13.3.0 and clang 19.1.1
// both refuse `# 1 "f.c" 9`, so this is matching the references rather than
// out-stricting them. RED-ON-DISABLE: accept-and-ignore an unknown flag.
TEST(PreprocessorLineMarker, UnknownFlagFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("# 1 \"f.c\" 9\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "an undeclared linemarker flag must FAIL LOUD (both references do)";
}

// Two members of the same `exclusiveGroup` may not co-occur. MEASURED: both
// references refuse `# 1 "f.c" 1 2`. The rule is CONFIG-driven — it reads the
// group id from `preprocess.lineMarker.flags`, never a hard-coded 1-vs-2.
TEST(PreprocessorLineMarker, MutuallyExclusiveFlagsFailLoud) {
    PreprocessResult r;
    (void)ppLexemes("# 1 \"f.c\" 1 2\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "`1` (enter-file) and `2` (return-to-file) share an exclusiveGroup";
}

// A repeated flag is refused: it is either a typo or a shape no reference emits,
// and accepting it would mean the tail is not really being read.
TEST(PreprocessorLineMarker, RepeatedFlagFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("# 1 \"f.c\" 3 3\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
}

// The file operand is REQUIRED in this form (unlike `#line`'s, which C23
// 6.10.4p3 makes optional). MEASURED: all 331 linemarkers in one real gcc and
// clang `-E` census carry a quoted name, so a bare `# 5` is refused rather than
// guessed at.
TEST(PreprocessorLineMarker, MissingFileOperandFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("# 5\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
}

// An UNQUOTED name is refused rather than read as a flag or as junk.
TEST(PreprocessorLineMarker, UnquotedFileOperandFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("# 5 f.c\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
}

// Out of range, the one numeric failure the recognizer leaves to the handler.
TEST(PreprocessorLineMarker, OutOfRangeLineNumberFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes("# 99999999999 \"f.c\"\nint x;\n", r);
    EXPECT_TRUE(r.diagnostics->hasErrors());
}

// A linemarker inside an ELIDED branch is skipped with NO diagnostic and NO
// renumbering — the #define/#include/#pragma/#embed/#line dead-branch parity
// (C 6.10p1). The malformed flag makes this strictly stronger than a well-formed
// fixture: it proves the arm is never REACHED, not merely that it succeeded.
TEST(PreprocessorLineMarker, InDeadBranchIsInert) {
    PreprocessResult r;
    //                    line: 1      2             3       4
    auto lexs = ppLexemes("#if 0\n# 500 \"f.c\" 9\n#endif\nint x = __LINE__;\n",
                          r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a linemarker in a dead branch must not diagnose — not even a "
           "malformed one";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "4")
        << "a dead-branch linemarker must NOT renumber";
}

// ★ THE `system-header` DECISION, PINNED SO IT CANNOT DRIFT SILENTLY.
// gcc/clang use flag `3` to SUPPRESS warnings inside the marked region
// (MEASURED: clang 19.1.1 warns -Wunused-variable under `1` and not under
// `1 3`). DSS recognises the flag and suppresses NOTHING — a UNIFORM policy, not
// a hole in this directive: DSS has no system-header posture anywhere, so it
// already declines to suppress inside an ordinary `#include <...>` header, and a
// TU fed through `gcc -E` therefore reports exactly what DSS reports compiling
// the same headers directly. That is what makes a conformance census meaningful.
// If DSS ever gains such a posture, THIS pin is what goes red and forces the
// decision to be re-made deliberately.
TEST(PreprocessorLineMarker, SystemHeaderFlagIsRecognisedAndSuppressesNothing) {
    PreprocessResult r;
    // `#warning` inside a region marked as a system header still fires.
    (void)ppLexemes("# 10 \"sys.h\" 1 3 4\n#warning still audible\nint x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorWarningDirective))
        << "flag 3 marks a system header in gcc/clang and suppresses their "
           "warnings there; DSS suppresses nothing ANYWHERE, so this must still "
           "fire — change this pin only together with a real system-header "
           "posture";
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a #warning is a Warning, not an Error";
}

// ─────────────────────────────────────────────────────────────────────────────
// D-CSUBSET-COUNTER-MACRO-NOT-EXPANDED — `__COUNTER__`, the one STATEFUL
// predefined-macro kind. Config-declared by NAME (`predefinedMacros` with
// `"kind": "counter"`); the engine owns the count. Every assertion below is
// RED-ON-DISABLE.
// ─────────────────────────────────────────────────────────────────────────────

// The core property, and the one a single-use fixture cannot see: two expansions
// in ONE translation unit yield DIFFERENT values. At the pre-change HEAD the name
// was not a macro at all and pasted as literal text, so a one-use pin passed.
TEST(PreprocessorCounter, AdvancesOncePerExpansion) {
    PreprocessResult r;
    auto lexs = ppLexemes("int a = __COUNTER__; int b = __COUNTER__; "
                          "int c = __COUNTER__;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 15u) << "expected: int a = 0 ; int b = 1 ; int c = 2 ;";
    EXPECT_EQ(lexs[3], "0") << "the FIRST expansion is 0, matching gcc and clang";
    EXPECT_EQ(lexs[8], "1");
    EXPECT_EQ(lexs[13], "2");
}

// The idiom the row was opened for: `##`-pasting the counter into an identifier
// must mint DISTINCT names. RED-ON-DISABLE: drop the `counter` kind and both
// names become the literal `v___COUNTER__`, which is what HEAD produced.
TEST(PreprocessorCounter, PastesIntoDistinctIdentifiers) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define DSS_CAT2(a,b) a##b\n"
                          "#define DSS_CAT(a,b) DSS_CAT2(a,b)\n"
                          "int DSS_CAT(v_, __COUNTER__);\n"
                          "int DSS_CAT(v_, __COUNTER__);\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 6u) << "expected: int v_0 ; int v_1 ;";
    EXPECT_EQ(lexs[1], "v_0");
    EXPECT_EQ(lexs[4], "v_1");
    EXPECT_NE(lexs[1], lexs[4])
        << "the whole point of the construct is that the two names DIFFER";
    for (auto const& s : lexs) {
        EXPECT_EQ(s.find("__COUNTER__"), std::string::npos)
            << "the token must never survive into the output as literal text — "
               "that is the silent wrongness this row was opened for: `"
            << s << "`";
    }
}

// It is a genuine PREDEFINED macro, so `#ifdef` sees it. A value-context-only
// implementation would leave `#ifdef __COUNTER__` false, and the universal
// `#ifndef X / #define X` shim pattern would then shadow it.
TEST(PreprocessorCounter, IsVisibleToIfdefWithoutConsumingACount) {
    PreprocessResult r;
    auto lexs = ppLexemes("#ifdef __COUNTER__\nint x = __COUNTER__;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "0")
        << "a definedness TEST is not an expansion, so it must not burn a count";
}

// D-PP-PREDEFINE-REDEFINITION-PARTITION: `#undef` of the counter is DIAGNOSED
// and then APPLIED — it inherits that from the shared predefined table rather
// than needing its own rule, exactly as before; what changed is the rule.
//
// ★ THIS NAME IS THE INDEPENDENT CONFIRMATION THAT THE PARTITION'S SECOND ARM
// IS REAL AND NOT A DSS INVENTION. The counter is a pure compiler extension —
// ISO C never mentions it, so no clause of 6.10.10 reaches it — and yet
// ✔MEASURED 2026-08-26, gcc 13.3.0 AND clang 18.1.3 BOTH diagnose
// `#undef __COUNTER__`, exactly as they diagnose `#undef __LINE__`, while
// staying silent for 16 other extension-supplied names in the same sweep. The
// property that separates it from those 16 is that its value is DERIVED per
// use, which is precisely what the `warn-derived-macro` verb declares. The
// references drew that line first; the verb records it.
// ⚠ It was previously pinned as a hard ERROR. Both references warn and
// continue, and 6.10.10 sits outside any Constraints subclause (C23 4p2 ⇒ UB,
// no diagnostic required at all), so refusing was the divergence.
TEST(PreprocessorCounter, UndefIsDiagnosedThenApplied) {
    PreprocessResult r;
    auto lexs = ppLexemes("#undef __COUNTER__\nint x = __COUNTER__;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorPredefinedMacro))
        << "a derived-value predefine must be diagnosed on #undef — gcc and "
           "clang both are, for this name specifically";
    EXPECT_EQ(ppCodeSeverity(r, DiagnosticCode::P_PreprocessorPredefinedMacro),
              DiagnosticSeverity::Warning);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "diagnosed, not refused";
    ASSERT_EQ(lexs.size(), 5u);
    EXPECT_EQ(lexs[3], "__COUNTER__")
        << "and APPLIED: the name is a bare identifier now, not a minted count. "
           "A `0` here would mean the #undef was diagnosed and then ignored";
}

// PER-TRANSLATION-UNIT RESET. Two independent preprocess runs must both start at
// 0 — otherwise two TUs in one invocation would disagree about a name they both
// mint, which is the failure the row names explicitly. RED-ON-DISABLE: make the
// counter `static` and the second run starts at 1.
TEST(PreprocessorCounter, ResetsPerTranslationUnit) {
    PreprocessResult r1;
    auto a = ppLexemes("int x = __COUNTER__; int y = __COUNTER__;\n", r1);
    PreprocessResult r2;
    auto b = ppLexemes("int x = __COUNTER__; int y = __COUNTER__;\n", r2);
    ASSERT_EQ(a.size(), 10u);
    ASSERT_EQ(b.size(), 10u);
    EXPECT_EQ(a[3], "0");
    EXPECT_EQ(a[8], "1");
    EXPECT_EQ(b[3], "0")
        << "a second translation unit must start its own count at 0";
    EXPECT_EQ(b[8], "1");
}

// ═════════════════════════════════════════════════════════════════════════════
// D-PP-DEFINED-VIA-MACRO-EXPANSION — the `defined` operator ARRIVING VIA MACRO
// EXPANSION, and the OPERAND BARRIER that makes it answerable.
//
// C 6.10.1 leaves the case UNDEFINED, so "the standard requires it" is NOT the
// argument and these pins do not make it. The argument is the union of the
// references, ✔MEASURED 2026-08-26 on eighteen shapes across three toolchains:
//   gcc 13.3.0    `-std=c2x  -pedantic`  ACCEPTS + EVALUATES, warns
//                                        `-Wexpansion-to-defined`
//   clang 18.1.3  `-std=c23  -pedantic`  ACCEPTS + EVALUATES, warns
//                                        `-Wexpansion-to-defined`
//   MSVC 19.51    `/std:c17 /W4`, BOTH traditional AND `/Zc:preprocessor`
//                                        ACCEPTS + EVALUATES, warns C5105
// Unanimous, with IDENTICAL answers on every shape. Apple's own SDK depends on
// it (`secure/_string.h`'s `__is_modern_darwin`, whose last operand is literally
// `defined(__DRIVERKIT_VERSION_MIN_REQUIRED)`).
//
// ★★ THE PIN THAT MATTERS MOST IS THE OPERAND-PROTECTION ONE
// (`DefinedViaMacroExpansionDoesNotExpandItsOperand`). Every other shape here
// would also pass an implementation that simply re-ran the `defined` rewrite
// after expansion — and that implementation would be WRONG, because by then the
// operand has already been rescanned and replaced. `#define BAR 0` +
// `#define HAS_BAR defined(BAR)` is the shape that separates the two: protected,
// it is `defined(BAR)` == 1 (all three references agree); unprotected, it is
// `defined(0)`, a DIFFERENT QUESTION rather than a syntax error to report.
// ═════════════════════════════════════════════════════════════════════════════

// The row's headline shape: an OBJECT-like macro whose replacement list IS
// `defined(X)`. Both polarities, so a fix that hard-wires "true" is red.
// ✔REFERENCES: gcc/clang/MSVC all exit 11 (defined) / 22 (undefined).
TEST(Preprocessor, DefinedViaMacroExpansionObjectLikeBothPolarities) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define FOO 1\n#define HAS_FOO defined(FOO)\n#if HAS_FOO\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "a macro-produced `defined` must EVALUATE, not fail loud";
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a") << "HAS_FOO -> defined(FOO) -> 1 -> #if taken";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define HAS_FOO defined(FOO)\n#if HAS_FOO\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "b")
            << "FOO is NOT defined -> defined(FOO) -> 0 -> #else taken. A fix "
               "that answers 1 unconditionally passes the sibling case and "
               "fails here";
    }
}

// ★★ THE OPERAND BARRIER. `BAR` is defined TO ZERO, so the two readings differ
// in ANSWER, not merely in well-formedness:
//   operand PROTECTED (correct)   -> defined(BAR) -> 1 -> `int a;`
//   operand EXPANDED  (the bug)   -> defined(0)   -> malformed / 0 -> `int b;`
// ✔REFERENCES: gcc, clang and MSVC all exit 77 here, i.e. all three protect it.
TEST(Preprocessor, DefinedViaMacroExpansionDoesNotExpandItsOperand) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define BAR 0\n#define HAS_BAR defined(BAR)\n#if HAS_BAR\n"
        "int a;\n#else\nint b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "`defined(BAR)` reached through a macro must not become `defined(0)`";
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a")
        << "the operand of `defined` is NEVER macro-expanded (C 6.10.1p1), "
           "whichever construct produced the operator — BAR is DEFINED, and its "
           "VALUE (0) is not the question being asked";
}

// The barrier holds one level deeper: the operand is itself a macro whose own
// replacement is a macro. ✔REFERENCES: gcc/clang exit 55 (defined(NAME) == 1).
TEST(Preprocessor, DefinedViaMacroExpansionOperandProtectedThroughAChain) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define FOO 1\n#define NAME FOO\n#define A defined(NAME)\n#if A\n"
        "int a;\n#else\nint b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a")
        << "NAME is a defined macro, so defined(NAME) is 1 — it must not be "
           "expanded to FOO (also 1 here) NOR to 1 (which would be malformed)";
}

// The NO-PAREN form produced by expansion (`#define HAS_FOO defined FOO`), and
// the operator KEYWORD ITSELF arriving from a macro with the operand written at
// the call site (`#define D defined` + `#if D(FOO)`). The second is the shape
// that forces the barrier to be driven over the EXPANDER'S OUTPUT rather than
// its input — keyword and operand come from two different constructs.
// ✔REFERENCES: gcc/clang/MSVC exit 99 and 101 respectively.
TEST(Preprocessor, DefinedViaMacroExpansionNoParenAndKeywordFromAMacro) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define FOO 1\n#define HAS_FOO defined FOO\n#if HAS_FOO\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a") << "`defined FOO` (no parens) via expansion";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define FOO 1\n#define D defined\n#if D(FOO)\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a")
            << "the KEYWORD comes from `D`'s replacement list and the OPERAND "
               "from the directive line; a barrier driven over the expander's "
               "INPUT would never join them and FOO would expand to 1";
    }
}

// THE REAL-HEADER SHAPE, transcribed from Apple's `secure/_string.h`
// (`__is_modern_darwin`): a FUNCTION-like macro whose last operand is a
// `defined(...)` and whose earlier operands are ordinary comparisons using the
// parameters. ✔REFERENCES: gcc/clang/MSVC all exit 55.
TEST(Preprocessor, DefinedViaMacroExpansionRealHeaderFunctionLikeShape) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define A 101000\n#define B 130000\n"
        "#define IS_MODERN(ios, macos) "
        "(A >= (macos) || B >= (ios) || defined(NOT_SET))\n"
        "#if IS_MODERN(120000, 999999)\nint a;\n#else\nint b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "the shipped-SDK shape must compile: this is the population the row "
           "was opened for";
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a")
        << "A(101000) >= 999999 is false, B(130000) >= 120000 is TRUE -> taken";
}

// The barrier RESETS: two `defined`s produced by expansion in ONE controlling
// expression must BOTH fold, and an identifier that is NOT a `defined` operand
// must still expand. ✔REFERENCES: gcc/clang exit 99 and 109 respectively.
TEST(Preprocessor, DefinedViaMacroExpansionBarrierResetsBetweenOperands) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define P 1\n#define A defined(P)\n#if A && A\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a") << "both occurrences fold to 1";
    }
    {
        // `FOO` appears TWICE in one replacement list: once as the protected
        // operand of `defined`, once as an ordinary value. Only the first is
        // protected — the second MUST expand to 3, or `FOO > 2` folds an
        // identifier to 0 and the branch flips.
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define FOO 3\n#define A (defined(FOO) && FOO > 2)\n#if A\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a")
            << "the barrier is exactly ONE operand wide: the second FOO is a "
               "value and must still expand to 3";
    }
}

// `#elif` takes the same path as `#if` (C 6.10.1p5 makes them one evaluator).
// ✔REFERENCES: gcc/clang exit 105.
TEST(Preprocessor, DefinedViaMacroExpansionInElif) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define FOO 1\n#define A defined(FOO)\n#if 0\nint dead;\n#elif A\n"
        "int a;\n#else\nint b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a");
}

// ★★ THE BAR'S OTHER DIRECTION (§A.3b): accepting what NOT ONE reference accepts
// is also a defect. A `defined` whose OPERAND is a macro PARAMETER, or a literal
// `defined(...)` handed to a function-like macro as an ARGUMENT, is pre-expanded
// before substitution — `defined(1)` — and every reference REFUSES it:
//   gcc 13.3.0   `error: operator "defined" requires an identifier`
//   clang 18.1.3 `error: macro name must be an identifier`
//   MSVC 19.51   `error C2004: expected 'defined(id)'`  (both preprocessors)
// DSS used to ACCEPT the second one (folding the literal `defined` off the raw
// directive line before `ID` was ever invoked) and answer 1. Moving the fold to
// AFTER expansion closes it by construction.
TEST(Preprocessor, DefinedWithAPreExpandedOperandFailsLoudLikeEveryReference) {
    {
        PreprocessResult r;
        (void)ppLexemes(
            "#define FOO 1\n#define M(x) defined(x)\n#if M(FOO)\n"
            "int a;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "the ARGUMENT is pre-expanded (FOO -> 1) before substitution, so "
               "the operator gets `defined(1)` — all three references refuse it";
    }
    {
        PreprocessResult r;
        (void)ppLexemes(
            "#define FOO 1\n#define ID(a) a\n#if ID(defined(FOO))\n"
            "int a;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "a LITERAL `defined` inside a function-like macro's ARGUMENT list "
               "is not the top-level operator: the argument is pre-expanded "
               "first. Answering 1 here is accepting what no reference accepts";
    }
}

// NO REGRESSION on the literal form — the path the whole shipped header corpus
// walks. The `defined(__has_include)` half is the shape
// `examples/c/has_include_operator_not_shadowable` exists for: an operand that
// SPELLS an operator must be read as a NAME, never invoked.
TEST(Preprocessor, LiteralDefinedStillFoldsAfterTheOrderingChange) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define FOO 1\n#if defined(FOO) && !defined(NOPE)\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a");
    }
    {
        // The operand is a defined macro whose VALUE is 0 — the literal-form
        // twin of the barrier pin above.
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define ZERO 0\n#if defined(ZERO)\nint a;\n#else\nint b;\n#endif\n",
            r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a")
            << "literal `defined` must not expand its operand either";
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#if defined(__has_include)\nint a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "`__has_include` as the OPERAND of `defined` is a NAME, not an "
               "invocation — reading it as the operator would fail loud with "
               "P_PreprocessorHasInclude";
        ASSERT_EQ(lexs.size(), 3u);
    }
}

// AGNOSTICISM pin (RED-ON-DISABLE, and the mutation is in the REMOVE direction:
// the shipped spelling LOSES its operator status). Rebind
// `preprocess.definedOperator` from "defined" to "isdefined" and reload. The
// barrier + the fold must BOTH follow the config:
//   (1) the NEW spelling now protects its operand through expansion;
//   (2) the OLD spelling `defined` is now an ORDINARY IDENTIFIER — so the shape
//       that used to answer the definedness question must NOT still answer it.
// A hard-coded "defined" anywhere in the barrier or the fold fails (2).
TEST(Preprocessor, DefinedOperatorSpellingIsConfigDrivenThroughExpansion) {
    namespace fs = std::filesystem;
    std::vector<fs::path> noDirs;
    auto schema = reboundC("\"definedOperator\":     \"defined\"",
                           "\"definedOperator\":     \"isdefined\"",
                           "<rebound-defined-c>");
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->preprocess().definedOperator, "isdefined");

    auto lexemesOf = [&](std::string text, PreprocessResult& out) {
        auto buf = SourceBuffer::fromString(std::move(text), "main.c");
        out = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching,
                         DiagnosticBudget::libraryDefault());
        std::vector<std::string> lexs;
        for (Token const& t : out.tokens) {
            if (t.coreKind == CoreTokenKind::Eof) continue;
            if (t.coreKind == CoreTokenKind::Whitespace) continue;
            if (t.coreKind == CoreTokenKind::Newline) continue;
            lexs.push_back(std::string{out.synthBuffer->slice(t.span)});
        }
        return lexs;
    };

    // (1) the NEW spelling is the operator, barrier and all: ZERO is defined to
    // 0, so an unprotected operand would give `isdefined(0)` and the #else.
    {
        PreprocessResult r;
        auto lexs = lexemesOf(
            "#define ZERO 0\n#define A isdefined(ZERO)\n#if A\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a")
            << "the rebound spelling must carry BOTH the operator and its "
               "operand barrier";
    }
    // (2) THE REMOVE-DIRECTION HALF: the shipped spelling has LOST its operator
    // status. `defined` is now a plain identifier, so `#if defined(NOPE)` no
    // longer answers "is NOPE defined" — it is `<identifier> ( <identifier> )`,
    // which the ICE parser refuses. A silent 1/0 here would mean the spelling is
    // hard-coded somewhere.
    {
        PreprocessResult r;
        (void)lexemesOf(
            "#if defined(NOPE)\nint a;\n#else\nint b;\n#endif\n", r);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "with the operator rebound, a literal `defined` is an ordinary "
               "identifier — if this still evaluated as the operator, the "
               "spelling would be hard-coded somewhere";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// D-PP-DEFINED-VIA-MACRO-EXPANSION, second half: the `__has_*` OPERATOR FAMILY
// reached VIA MACRO EXPANSION, and the WARNING the references all emit.
//
// ✔MEASURED 2026-08-27, each toolchain SEPARATELY, on eleven `__has_*` shapes:
//   gcc 13.3.0   `-std=c2x -pedantic -I.`
//   clang 18.1.3 `-std=c23 -pedantic -I.`
//   MSVC 19.51   `/std:c17 /W4 /I.`, traditional AND `/Zc:preprocessor`
// All three give IDENTICAL answers on every `__has_include` shape below. The one
// place MSVC differs is `__has_c_attribute`, which `/std:c17` does not implement
// at all (it folds the name to 0) — that is the operator's ABSENCE, not a third
// opinion, so gcc ∪ clang decides that arm.
//
// ★★ THE TWO ARMS PULL IN OPPOSITE DIRECTIONS, WHICH IS WHY BOTH ARE PINNED.
// An operand already spelled `<...>` must NOT be expanded; an operand spelled
// anything else MUST be. Get either backwards and a shipped header answers the
// wrong question about which headers exist — silently, since both readings
// produce a perfectly well-formed 1 or 0.
// ═════════════════════════════════════════════════════════════════════════════

// The headline shape, both polarities: an object-like macro whose replacement
// list IS the whole operator call. ✔REFERENCES: 11 / 44 (exists / does not).
// `__has_include` here uses a name DSS resolves through the shipped descriptor
// path, so the positive arm is a real resolution, not a stub.
TEST(Preprocessor, HasIncludeViaMacroExpansionBothPolarities) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define HAS_H __has_include(<no_such_header_xyz.h>)\n#if HAS_H\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "a macro-produced `__has_include` must EVALUATE, not fail loud — "
               "this exact shape was `error[P0013]: trailing tokens after #if "
               "controlling expression` before the operator fold moved past "
               "expansion";
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "b") << "the header does not exist -> 0 -> #else";
    }
    {
        // POSITIVE arm through a real on-disk header, so the 1 is a resolution
        // rather than a constant. Quote form, which is the other spelling.
        namespace fs = std::filesystem;
        auto dir = ppScratchRoot() / "dss_pp_hasinc_macro";
        fs::create_directories(dir);
        { std::ofstream(dir / "real_hdr.h", std::ios::binary) << "int q;\n"; }
        auto mainPath = dir / "main.c";
        { std::ofstream(mainPath, std::ios::binary)
              << "#define HAS_Q __has_include(\"real_hdr.h\")\n#if HAS_Q\n"
                 "int a;\n#else\nint b;\n#endif\n"; }
        auto schema = cSubset();
        auto buf = SourceBuffer::fromFile(mainPath);
        ASSERT_NE(buf, nullptr);
        std::vector<fs::path> noDirs;
        PreprocessResult r = preprocess(buf, schema, noDirs,
                                        dss::kDefaultHeaderNameMatching,
                                        DiagnosticBudget::libraryDefault());
        EXPECT_FALSE(r.diagnostics->hasErrors());
        std::vector<std::string> lexs;
        for (Token const& t : r.tokens) {
            if (t.coreKind == CoreTokenKind::Eof) continue;
            if (t.coreKind == CoreTokenKind::Whitespace) continue;
            if (t.coreKind == CoreTokenKind::Newline) continue;
            lexs.push_back(std::string{r.synthBuffer->slice(t.span)});
        }
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a")
            << "the QUOTE form reached through a macro must resolve the real "
               "header — its NAME is a coalesced literal body whose raw bytes "
               "are read, which is the read the post-expansion move had to make "
               "buffer-aware";
    }
}

// ★★ THE PROTECTION ARM. `HDR` is a macro, but it sits INSIDE the angle
// delimiters, so it is part of the header NAME and must not be expanded: this
// asks for a header literally called `HDR.h`, which does not exist.
// ✔REFERENCES: gcc, clang and MSVC all answer 0 here.
// RED-ON-DISABLE shape: expand the interior and it asks for `stdio.h` instead —
// a perfectly well-formed question, and the wrong one.
TEST(Preprocessor, HasIncludeAngleInteriorIsNotMacroExpanded) {
    // ⚠⚠ THE OPERAND MACRO MUST EXPAND TO A NAME THIS RUN ACTUALLY RESOLVES, or
    // the pin is VACUOUS — and the first draft of it WAS, twice over. First it
    // used a nonexistent name on both sides, so protected and expanded BOTH
    // answered 0 and it passed over a completely broken barrier. The second
    // draft named a real shipped descriptor but went through `ppLexemes`, which
    // passes NO systemDirs — so the angle path resolved nothing and the arm was
    // vacuous again, in a way only the control below could see. Both drafts were
    // green. The control is the whole reason this test means anything.
    //
    // FC15c: the angle path maps `<stem.h>` onto `stem.json` on the SYSTEM path,
    // so the fixture ships a descriptor into a scratch systemDir:
    //   protected (correct) -> asks for `HDR.h`   -> no `HDR.json`  -> 0 -> #else
    //   expanded  (the bug) -> asks for `ctype.h` -> `ctype.json`   -> 1 -> #if
    namespace fs = std::filesystem;
    auto sysdir = ppScratchRoot() / "dss_pp_angle_interior_sys";
    fs::create_directories(sysdir);
    { std::ofstream(sysdir / "ctype.json", std::ios::binary) << "{}\n"; }

    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#define HDR ctype\n#if __has_include(<HDR.h>)\n"
        "int a;\n#else\nint b;\n#endif\n", r, {}, {sysdir});
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "b")
        << "the `<...>` interior is the header NAME, not an expression: a macro "
           "spelled inside it stays spelled. `a` here means the interior WAS "
           "expanded and the build asked about a DIFFERENT header";

    // ★ THE CONTROL, and it is load-bearing: the very name `HDR` expands to IS
    // resolvable in this run, so the arm above is a real refusal to expand and
    // not a missing header. Without this the test cannot tell the two apart.
    PreprocessResult ctl;
    auto ctlLexs = ppLexemesWithDirs(
        "#if __has_include(<ctype.h>)\nint a;\n#else\nint b;\n#endif\n",
        ctl, {}, {sysdir});
    EXPECT_FALSE(ctl.diagnostics->hasErrors());
    ASSERT_EQ(ctlLexs.size(), 3u);
    EXPECT_EQ(ctlLexs[1], "a")
        << "control: `<ctype.h>` DOES resolve here, so the sibling arm's `b` "
           "cannot be explained by the header simply being absent";

    // ★★★ AND THIS IS THE ARM THAT ACTUALLY EXERCISES THE BARRIER — the two
    // above pin the ANSWER but not the MECHANISM, which a red-on-disable run
    // proved rather than a review: stripping the angle-interior protection left
    // BOTH of them green, with the mutated object md5 correctly moved.
    //
    // ★ WHY THEY CANNOT SEE IT. A header name is read as a RAW BYTE RANGE from
    // the angle-open token's end to the LAST interior token's end. In `<HDR.h>`
    // the last interior token is `h`, which sits on the directive line whether or
    // not `HDR` expanded — so the range, and therefore the answer, is identical
    // either way. The protection only becomes observable when expanding the
    // interior MOVES THE END OF THE RANGE, i.e. when the macro IS the last
    // interior token:
    //   protected (correct) -> the range is `HDR`, one contiguous run on the
    //                          directive line -> no `HDR.json` -> 0 -> #else
    //   expanded  (the bug) -> the last interior token is now on the `#define`
    //                          line, BEFORE the range's start -> `ppRawRun`
    //                          refuses -> P_PreprocessorHasInclude
    // ✔REFERENCES: gcc 13.3.0 and clang 18.1.3 both answer 0 with NO diagnostic
    // for exactly this shape (`#define HDR realname.h` + `__has_include(<HDR>)`
    // takes the #else), i.e. both protect the interior here too.
    PreprocessResult whole;
    auto wholeLexs = ppLexemesWithDirs(
        "#define HDR ctype.h\n#if __has_include(<HDR>)\n"
        "int a;\n#else\nint b;\n#endif\n", whole, {}, {sysdir});
    EXPECT_FALSE(whole.diagnostics->hasErrors())
        << "with the interior protected the header name is one contiguous run "
           "on the directive line; an expanded interior leaves the range ends in "
           "two different constructs and the operator refuses";
    ASSERT_EQ(wholeLexs.size(), 3u);
    EXPECT_EQ(wholeLexs[1], "b")
        << "the whole interior is the header NAME `HDR`, which does not resolve";
}

// ★★ THE OPPOSITE ARM, and it is the one a naive "protect the operand like
// `defined`" fix gets wrong. An operand matching NEITHER `<...>` nor `"..."` IS
// macro-expanded and re-examined — C's own `#include MACRO` rule (C23 6.10.1p4
// deferring to 6.10.2). ✔REFERENCES: gcc, clang and MSVC all answer the
// operator here rather than refusing.
TEST(Preprocessor, HasIncludeOperandMatchingNeitherFormIsExpandedAndReExamined) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define H <no_such_header_xyz.h>\n#if __has_include(H)\n"
        "int a;\n#else\nint b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "`H` matches neither delimited form, so it MUST expand — refusing "
           "here (P001C, \"requires <header> or \\\"header\\\"\") is what DSS "
           "did before, and no reference does it";
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "b")
        << "expanded to <no_such_header_xyz.h>, re-examined, resolved to 0";
}

// The operator KEYWORD alone from a macro, operand written at the call site —
// the shape that forces the barrier to be driven over the expander's OUTPUT.
// And the family composing with `defined` inside ONE replacement list.
// ✔REFERENCES: both accepted by all three.
TEST(Preprocessor, HasIncludeKeywordFromAMacroAndComposedWithDefined) {
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define HI __has_include\n#if HI(<no_such_header_xyz.h>)\n"
            "int a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "b");
    }
    {
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#define FOO 1\n"
            "#define BOTH (defined(FOO) && !__has_include(<no_such_hdr_xyz.h>))\n"
            "#if BOTH\nint a;\n#else\nint b;\n#endif\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors());
        ASSERT_EQ(lexs.size(), 3u);
        EXPECT_EQ(lexs[1], "a")
            << "two DIFFERENT operators produced by one replacement list, with "
               "opposite operand rules, must both fold in the same pass";
    }
}

// `__has_c_attribute`'s operand IS macro-expanded — the third rule in the
// family, and the reason the barrier has no state for this operator at all.
// ✔REFERENCES: gcc AND clang both answer the ATTRIBUTE's version here, i.e. `A`
// expanded to `deprecated`. (MSVC `/std:c17` does not implement the operator, so
// it contributes nothing to this arm.)
TEST(Preprocessor, HasCAttributeOperandIsMacroExpanded) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define A deprecated\n#if __has_c_attribute(A)\nint a;\n#else\n"
        "int b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a")
        << "`A` expands to `deprecated`, a known attribute -> non-zero. Reading "
           "the raw operand instead asks about an attribute named `A` -> 0, "
           "which is what DSS answered before the fold moved past expansion";
}

// ★★★ THE REFUSAL THAT REPLACED THE OLD REFUSAL, and the reason moving these
// operators past expansion is safe at all. A header NAME is read as RAW BYTES
// spanning several tokens, so once expansion can splice two constructs into one
// operand, a range read across the splice is not malformed — it is a PLAUSIBLE
// header name made of unrelated bytes, which the resolver would then answer
// confidently. `ppRawRun` refuses any range that crosses a line, and the
// operator fails LOUD instead of guessing.
// ⓘ This is the one shape in the family DSS does not answer. It is not a
// conformance gap: gcc and clang reject it too (`#include` nested in the middle
// of a header name is not a form either accepts), and the point of the pin is
// that DSS's refusal is LOUD and specific rather than a wrong 1 or 0.
TEST(Preprocessor, HasIncludeHeaderNameSplicedAcrossConstructsFailsLoud) {
    PreprocessResult r;
    (void)ppLexemes(
        "#define OPENA <no_such\n#if __has_include(OPENA.h>)\nint a;\n#endif\n",
        r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorHasInclude))
        << "half the header name comes from the #define line and half from the "
           "#if line; the byte range between them is the rest of the file. It "
           "must fail loud, never resolve whatever those bytes spell";
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "and it is an ERROR — guessing a header name is a silent wrong "
           "answer, which is the one outcome worse than refusing";
}

// NO REGRESSION on the literal forms — the path the whole shipped header corpus
// walks, and the one that had no reason to move.
TEST(Preprocessor, LiteralHasIncludeStillFoldsAfterTheOrderingChange) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#if !__has_include(<no_such_header_xyz.h>)\nint a;\n#else\n"
        "int b;\n#endif\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a");
}

// ═════════════════════════════════════════════════════════════════════════════
// P_PreprocessorDefinedFromExpansion — the warning all three references emit.
// ✔MEASURED: gcc `-Wexpansion-to-defined` ("this use of \"defined\" may not be
// portable"), clang `-Wexpansion-to-defined` ("macro expansion producing
// 'defined' has undefined behavior"), MSVC `C5105` (clang's wording), the last
// in BOTH preprocessors. Evaluating in silence is the one behaviour none of the
// three has.
// ═════════════════════════════════════════════════════════════════════════════

// Diagnosed, at WARNING severity, and translation CONTINUES with the operator
// evaluated — all three properties asserted, because any one of them alone
// would be satisfied by a wrong implementation (an error would also be
// "diagnosed"; a silent evaluation would also "continue").
TEST(Preprocessor, DefinedFromExpansionIsDiagnosedAsAWarningAndStillEvaluated) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define FOO 1\n#define HAS_FOO defined(FOO)\n#if HAS_FOO\n"
        "int a;\n#else\nint b;\n#endif\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDefinedFromExpansion))
        << "all three references say so while evaluating; evaluating in silence "
           "is the one behaviour none of them has";
    EXPECT_EQ(ppCodeSeverity(r, DiagnosticCode::P_PreprocessorDefinedFromExpansion),
              DiagnosticSeverity::Warning);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a warning must never bump the error count — the construct is legal "
           "by consensus and real SDK headers use it deliberately";
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a") << "and the operator was still EVALUATED";
}

// The NEGATIVE that makes the pin above non-vacuous: a `defined` written in the
// directive is NOT diagnosed. Without this, an implementation that warned on
// every `defined` would pass the sibling test.
TEST(Preprocessor, LiteralDefinedIsNotDiagnosedAsFromExpansion) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define FOO 1\n#if defined(FOO) && !defined(NOPE)\nint a;\n#else\n"
        "int b;\n#endif\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorDefinedFromExpansion))
        << "the literal form is fully defined by C 6.10.1 and must be silent";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a");
}

// REACHABILITY (C 6.10p1), the `#error`/`#warning`/`#pragma` invariant: a
// macro-produced `defined` inside a NOT-TAKEN branch is never evaluated, so it
// is never diagnosed. Load-bearing rather than decorative — SDK headers park
// these inside unsupported-configuration branches by the hundred, and a
// per-lexed-token warning would fire on every one of them.
TEST(Preprocessor, DefinedFromExpansionInADeadBranchIsSilent) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define FOO 1\n#define HAS_FOO defined(FOO)\n#if 0\n#if HAS_FOO\n"
        "int dead;\n#endif\n#endif\nint a;\n", r);
    EXPECT_FALSE(hasPPCode(r, DiagnosticCode::P_PreprocessorDefinedFromExpansion))
        << "a dead `#if` group is parsed only far enough to track nesting; its "
           "controlling expression is never evaluated and must be silent";
    EXPECT_FALSE(r.diagnostics->hasErrors());
    ASSERT_EQ(lexs.size(), 3u);
    EXPECT_EQ(lexs[1], "a");
}

// SUPPRESSABILITY, pinned as a NEGATIVE against the closed table itself. The
// membership rule in `unsuppressable_codes.cpp` has two prongs — ship-a-wrong-
// artifact-green, and fail-with-nothing-said — and this code satisfies NEITHER:
// suppressing it changes no answer (the operator is still evaluated), ships no
// bytes and hides no failure. It is also advice about code the author usually
// cannot edit (Apple's `secure/_string.h` uses the construct on purpose), which
// is the A_ImmediateNarrowedToOperandField distinction drawn in that file.
// RED-ON-DISABLE: add the code to the table and this goes red immediately.
TEST(Preprocessor, DefinedFromExpansionWarningIsSuppressable) {
    EXPECT_FALSE(isUnsuppressable(
        DiagnosticCode::P_PreprocessorDefinedFromExpansion))
        << "`--suppress` must be able to silence exactly this advisory class — "
           "the P_PreprocessorWarningDirective / S_DeprecatedSymbolUsed posture. "
           "`--warnings-as-errors` remains the lever for a project that wants "
           "the construct out of its own sources.";
    // The sibling that IS in the table, so the assertion above is not vacuously
    // true against a broken `isUnsuppressable`.
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::P_PreprocessorErrorDirective))
        << "control: `#error` IS unsuppressable, so a query that answered false "
           "for everything would fail here";
}

// ── D-PP-IF-UNSIGNED-INTMAX ─────────────────────────────────────────────────
// C 6.10.1p4: a `#if`/`#elif` controlling expression is evaluated with every
// integer type acting as `intmax_t` / `uintmax_t`.
//
// ★★ EVERY PIN BELOW IS A TWO-ARM DISCRIMINATOR, AND THAT SHAPE IS MANDATORY
// RATHER THAN STYLISTIC. This defect selects the WRONG BRANCH and then compiles
// it perfectly: no diagnostic, exit 0, a different program. A pin that asserted
// "no errors" or counted diagnostics would have stayed green over it forever --
// which is exactly what happened. The row spent four months labelled
// "NOT silently wrong -- fail-loud" while DSS was emitting the opposite branch
// from a unanimous gcc + clang. So each case writes `taken_if` in one arm and
// `taken_else` in the other and asserts WHICH SURVIVED; the surviving lexeme is
// the observation, never the absence of a diagnostic.
//
// ⓘ The unit tier reads the surviving TOKEN; the corpus example
// `examples/c/c_pp_if_intmax` reads the same question out of a running
// program's EXIT CODE; the lane's probe harness read it out of the emitted
// object with `nm`. Three independent instruments, one answer.
//
// Every expectation is ✔MEASURED against gcc 13.3.0 `-std=c2x` and clang 18.1.3
// `-std=c23`, probed SEPARATELY, with the taken arm read from the emitted object
// via `nm`. Where they are cited as unanimous, they agreed.
namespace {

// The arm a controlling expression selects: "taken_if" / "taken_else", or a
// description of why neither was observed. Deliberately returns the ARM rather
// than a bool, so a test that stops discriminating fails loudly instead of
// degrading into "something compiled".
[[nodiscard]] std::string ppTakenArm(std::string const& cond,
                                     PreprocessResult& out) {
    auto lexs = ppLexemes("#if " + cond + "\nint taken_if;\n#else\n"
                          "int taken_else;\n#endif\n", out);
    bool sawIf = false, sawElse = false;
    for (auto const& l : lexs) {
        if (l == "taken_if")   sawIf = true;
        if (l == "taken_else") sawElse = true;
    }
    if (sawIf && sawElse) return "BOTH";
    if (sawIf)   return "taken_if";
    if (sawElse) return "taken_else";
    return "NEITHER";
}

}  // namespace

TEST(PreprocessorIfIntmax, UnsignedLiteralComparesUnsigned) {
    PreprocessResult r;
    // gcc: taken_if. clang: taken_if. DSS before the fix: taken_else.
    EXPECT_EQ(ppTakenArm("18446744073709551615u > 0", r), "taken_if")
        << "a `u`-suffixed literal above INT64_MAX is unsigned and positive; "
           "reading it as a signed -1 takes the wrong arm in silence";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

TEST(PreprocessorIfIntmax, SignedOperandConvertsToUnsignedInAMixedComparison) {
    PreprocessResult r;
    // C 6.3.1.8: `-1` converts to UINTMAX_MAX, so the comparison is FALSE.
    // gcc: taken_else. clang: taken_else. DSS before the fix: taken_if.
    EXPECT_EQ(ppTakenArm("-1 < 0u", r), "taken_else")
        << "the usual arithmetic conversions make this comparison UNSIGNED; a "
           "signed comparison answers the opposite";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// ★ THE WIDTH HALF, WHICH THE ROW DID NOT KNOW ABOUT AND WHICH NEEDS NO
// UNSIGNED LITERAL AT ALL. The leaf used to stamp every literal `TypeKind::I32`,
// so `intOpDomain` ran the whole evaluator in 32-BIT signed -- not the "signed
// int64" the row claimed -- and `wrapToIntTarget` truncated both operands before
// every comparison. "Is INT64_MAX positive" answered NO.
TEST(PreprocessorIfIntmax, EvaluationIsSixtyFourBitNotThirtyTwo) {
    PreprocessResult r;
    // All three: taken_if. DSS before the fix: taken_else, on all four.
    EXPECT_EQ(ppTakenArm("9223372036854775807 > 0", r), "taken_if")
        << "INT64_MAX is positive; a 32-bit domain truncates it to -1";
    EXPECT_EQ(ppTakenArm("3000000000 > 0", r), "taken_if")
        << "an ordinary decimal literal above INT32_MAX, no suffix in sight";
    EXPECT_EQ(ppTakenArm("2147483647 + 1 > 0", r), "taken_if")
        << "INT32_MAX+1 stays positive at intmax width";
    EXPECT_EQ(ppTakenArm("(1 << 40) > 0", r), "taken_if")
        << "a shift past bit 31 must not fall off a 32-bit domain";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// ★★ THE SIGNEDNESS BOUNDARY IS INTMAX_MAX, NOT THE LITERAL'S OWN C TYPE, AND
// THIS PAIR IS THE WHOLE REASON `preprocessorLiteralSignedness` EXISTS INSTEAD
// OF A CALL TO `typeIntegerLiteral`. Asking the ordinary C 6.4.4.1 ladder types
// `0xFFFFFFFF` as `unsigned int` -- correct 6.4.4.1 -- and carrying that
// signedness into phase 4 converts `-1` to UINTMAX_MAX and takes the FALSE arm.
// Both references take the TRUE arm: in phase 4 the only integer types are
// intmax_t and uintmax_t, so signedness is re-decided at 64 bits.
TEST(PreprocessorIfIntmax, LiteralSignednessIsDecidedAtIntmaxWidth) {
    PreprocessResult r;
    // Straddling INTMAX_MAX exactly. gcc and clang agree on both, opposite ways.
    EXPECT_EQ(ppTakenArm("0x7FFFFFFFFFFFFFFF > -1", r), "taken_if")
        << "INTMAX_MAX fits a signed intmax_t, so this is a SIGNED comparison";
    EXPECT_EQ(ppTakenArm("0x8000000000000000 > -1", r), "taken_else")
        << "one greater does not fit, so it is unsigned and -1 becomes "
           "UINTMAX_MAX -- the adjacent cell that proves the boundary is real";
    // A value that fits intmax_t but NOT a 32-bit int: 6.4.4.1 says `unsigned
    // int`, phase 4 says signed. This is the cell the ladder-as-written got
    // wrong, and it is the reason a second helper was needed.
    EXPECT_EQ(ppTakenArm("0xFFFFFFFF > -1", r), "taken_if")
        << "6.4.4.1 would type this `unsigned int`; phase 4 re-decides at "
           "intmax width and both references take the TRUE arm";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// A SUFFIX still forces unsignedness at any magnitude -- so the rule is not
// merely "magnitude decides". Without this the previous test would be satisfied
// by a bogus implementation that ignored suffixes entirely.
TEST(PreprocessorIfIntmax, AnUnsignedSuffixForcesUnsignedAtAnyMagnitude) {
    PreprocessResult r;
    // gcc: taken_else. clang: taken_else.
    EXPECT_EQ(ppTakenArm("0xFFFFFFFFu > -1", r), "taken_else")
        << "the `u` suffix makes it unsigned even though the value fits an "
           "intmax_t -- the control against a magnitude-only implementation";
    EXPECT_EQ(ppTakenArm("4294967295u > -1", r), "taken_else")
        << "the same in decimal, so the answer is not a radix artifact";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// A decimal literal above INTMAX_MAX whose ladder is entirely SIGNED
// (int/long/long long). ✔MEASURED: gcc warns "integer constant is so large that
// it is unsigned", clang warns "interpreting as unsigned", and BOTH then take
// the TRUE arm. Unanimous, so the union rule makes it required.
TEST(PreprocessorIfIntmax, DecimalAboveIntmaxMaxIsUnsignedNotRefused) {
    PreprocessResult r;
    EXPECT_EQ(ppTakenArm("18446744073709551615 > 0", r), "taken_if")
        << "both references accept this and evaluate it as unsigned with its "
           "true value; refusing it would diverge from a unanimous pair";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

TEST(PreprocessorIfIntmax, UnsignedArithmeticUsesUnsignedFormsThroughout) {
    PreprocessResult r;
    // Each of these folded to the wrong value under a signed 32-bit domain.
    EXPECT_EQ(ppTakenArm("0u - 1u > 0", r), "taken_if")
        << "unsigned wraparound is a large positive, not -1";
    EXPECT_EQ(ppTakenArm("(18446744073709551615u / 2u) == 9223372036854775807", r),
              "taken_if") << "unsigned division, not signed";
    EXPECT_EQ(ppTakenArm("(18446744073709551615u % 10u) == 5", r), "taken_if")
        << "unsigned remainder, not signed";
    EXPECT_EQ(ppTakenArm("(18446744073709551615u >> 60) == 15", r), "taken_if")
        << "a LOGICAL right shift on an unsigned operand, not arithmetic";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// ★ THE REGRESSION DIRECTION. Widening the domain must not make SIGNED
// arithmetic behave unsigned. Without these, an implementation that simply
// declared everything unsigned would pass every test above.
TEST(PreprocessorIfIntmax, SignedArithmeticStaysSigned) {
    PreprocessResult r;
    EXPECT_EQ(ppTakenArm("-1 < 0", r), "taken_if")
        << "a plain signed comparison; unsigned would answer the opposite";
    EXPECT_EQ(ppTakenArm("(-1 >> 1) == -1", r), "taken_if")
        << "an ARITHMETIC right shift on a signed operand";
    EXPECT_EQ(ppTakenArm("(-1 / 2) == 0", r), "taken_if")
        << "signed division truncating toward zero";
    EXPECT_EQ(ppTakenArm("2 + 2 == 4", r), "taken_if")
        << "THE CONTROL: all three implementations agree, so this harness can "
           "demonstrate AGREEMENT and is not merely stuck reporting divergence";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// The cell the ROW ITSELF cited as its witness for four months. It is the only
// shape that came out right before the fix, and by coincidence -- both sides
// landed on int64 -1. Kept as a pin precisely so the coincidence is recorded:
// the row's own witness was the cell LEAST able to show the defect.
TEST(PreprocessorIfIntmax, TheRowsOriginalWitnessCellStillHolds) {
    PreprocessResult r;
    EXPECT_EQ(ppTakenArm("0xFFFFFFFFFFFFFFFFu == -1", r), "taken_if")
        << "right before the fix and right after it, for different reasons";
    EXPECT_EQ(ppTakenArm("-1 < 0xFFFFFFFFFFFFFFFF", r), "taken_else")
        << "the sibling that was ALSO right by coincidence";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// A `#if` whose result is a legitimate unsigned value ABOVE INT64_MAX, used
// bare as the controlling expression.
//
// ⚠ THIS CASE IS *NOT* RED-ON-DISABLE FOR `evaluate()`, AND SAYING SO IS THE
// POINT. It was written believing it pinned the `asInt64` -> `asBool` change,
// on the reasoning that the old bridge nullopts for a value above INT64_MAX.
// ✔MEASURED (red-on-disable arm M3, 2026-08-27): reverting that line alone is
// GREEN here and the corpus example still exits 42 — indistinguishable from the
// line-inserting CONTROL arm. The reason is the shipped representation:
// `intmaxOperand` stores the raw bit pattern in the INT64 arm (it must, or
// `applyUnaryInt`'s `asInt64` bridge would refuse unary operators on large
// unsigned values), and `asInt64`'s int64 arm always succeeds. The prediction
// held only for the `uint64_t`-arm representation that was planned and not used.
//
// It is kept because the FACT is worth pinning — `#if UINT64_MAX` is truthy and
// must not be refused — and because it is the case that would go red first if
// any future producer here starts minting a `uint64_t` or `BitIntValue` arm.
// Do not describe it as pinning `evaluate()`; it pins the behaviour, not the line.
TEST(PreprocessorIfIntmax, AnUnsignedResultAboveIntmaxMaxIsTruthyNotRefused) {
    PreprocessResult r;
    EXPECT_EQ(ppTakenArm("18446744073709551615u", r), "taken_if")
        << "UINT64_MAX is non-zero, so the branch is taken; asking whether it "
           "fits an int64 asks a question C 6.10.1p2 never asks";
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "and it must not be reported as `not an integer constant`";
}

// ── THE SETTLED REFERENCE SPLIT, PINNED SO IT IS NOT RE-OPENED AS A GAP ──────
// A literal too large for `uintmax_t` has no cast-free route into a `#if` other
// than being written out. ✔MEASURED: clang REFUSES ("integer literal is too
// large to be represented"); gcc accepts with a warning and evaluates a
// TRUNCATED value. The references disagree, and the one that "accepts" does so
// only by destroying the value -- the same silent wrongness this row exists to
// remove. DSS refuses, matching clang and the fail-loud side. RULED 2026-08-27;
// this pin records the ruling, it is not a defect report.
TEST(PreprocessorIfIntmax, LiteralAboveUintmaxMaxIsRefusedMatchingClang) {
    PreprocessResult r;
    EXPECT_EQ(ppTakenArm("340282366920938463463374607431768211455 > 0", r),
              "taken_else")
        << "a refused controlling expression is treated as false by the caller";
    EXPECT_TRUE(r.diagnostics->hasErrors())
        << "and it must FAIL LOUD rather than silently pick an arm -- the "
           "distinction from every other case in this block";
}

// ─────────────────────────────────────────────────────────────────────────────
// D-PP-IF-LARGE-DECIMAL-LITERAL-HAS-NO-WARNING — the ADVICE half of the block
// above. The branch is already right; what was missing is that both references
// SAY the literal was reinterpreted, and DSS said nothing.
//
// ★★ THE PIN IS A PAIR AND THE SECOND HALF IS WHAT MAKES IT NON-VACUOUS. A pin
// that only asserted PRESENCE would stay green over a diagnostic that fired on
// every literal in the language. Each negative below is MEASURED on both
// references, not reasoned: a `u` suffix, a hexadecimal spelling and a value at
// INTMAX_MAX exactly all draw NO warning from gcc 13.3.0 or clang 18.1.3, and
// `9223372036854775808` — one more than INTMAX_MAX — draws one from both. The
// boundary is therefore pinned from both sides.
// ─────────────────────────────────────────────────────────────────────────────
TEST(PreprocessorIfIntmax, DecimalAboveIntmaxMaxWarnsThatItIsUnsigned) {
    PreprocessResult r;
    EXPECT_EQ(ppTakenArm("18446744073709551615 > 0", r), "taken_if")
        << "the ARM is unchanged by the warning -- both references warn and "
           "then take this arm, and so must DSS";
    EXPECT_EQ(ppCodeSeverity(r,
                  DiagnosticCode::P_PreprocessorIfLiteralImplicitlyUnsigned),
              DiagnosticSeverity::Warning)
        << "gcc: `integer constant is so large that it is unsigned`; clang: "
           "`interpreting as unsigned`. Both on by default at -std=c2x with no "
           "-W flag, so the union rule makes the warning REQUIRED";
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "and it must stay a WARNING: making it an error would REFUSE a "
           "construct both references compile, breaking the union rule in the "
           "other direction";
}

TEST(PreprocessorIfIntmax, TheImplicitlyUnsignedWarningIsAbsentWhereBothReferencesAreSilent) {
    // (1) SUFFIXED. The `u` rule's candidates are unsigned outright, so nothing
    // was reinterpreted. gcc: silent. clang: silent.
    {
        PreprocessResult r;
        EXPECT_EQ(ppTakenArm("18446744073709551615u > 0", r), "taken_if");
        EXPECT_FALSE(hasPPCode(
            r, DiagnosticCode::P_PreprocessorIfLiteralImplicitlyUnsigned))
            << "an explicit `u` is not a surprise -- warning here would be an "
               "invented diagnostic neither reference emits";
    }
    // (2) NON-DECIMAL. C 6.4.4.1 gives a hex literal unsigned candidates, so
    // again nothing was reinterpreted. Both spellings measured silent on both.
    for (char const* hex : {"0xFFFFFFFFFFFFFFFF > 0", "0x8000000000000000 > 0"}) {
        PreprocessResult r;
        EXPECT_EQ(ppTakenArm(hex, r), "taken_if");
        EXPECT_FALSE(hasPPCode(
            r, DiagnosticCode::P_PreprocessorIfLiteralImplicitlyUnsigned))
            << "hexadecimal: " << hex << " -- neither reference warns";
    }
    // (3) THE BOUNDARY, from below. INTMAX_MAX itself fits a signed candidate.
    {
        PreprocessResult r;
        EXPECT_EQ(ppTakenArm("9223372036854775807 > 0", r), "taken_if");
        EXPECT_FALSE(hasPPCode(
            r, DiagnosticCode::P_PreprocessorIfLiteralImplicitlyUnsigned))
            << "INTMAX_MAX exactly -- silent on both references";
    }
    // (4) THE BOUNDARY, from above, one greater. This one DOES warn on both, so
    // the boundary is exact rather than approximately right.
    {
        PreprocessResult r;
        EXPECT_EQ(ppTakenArm("9223372036854775808 > 0", r), "taken_if");
        EXPECT_TRUE(hasPPCode(
            r, DiagnosticCode::P_PreprocessorIfLiteralImplicitlyUnsigned))
            << "INTMAX_MAX + 1 -- gcc and clang both warn here";
    }
    // (5) AN ORDINARY LITERAL. The control that the warning is not simply on.
    {
        PreprocessResult r;
        EXPECT_EQ(ppTakenArm("42 > 0", r), "taken_if");
        EXPECT_FALSE(hasPPCode(
            r, DiagnosticCode::P_PreprocessorIfLiteralImplicitlyUnsigned));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// D-FFI-DESCRIPTOR-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR
//
// A shipped descriptor's `constants` surface reached the SEMANTIC seam and
// nothing else, so after `#include <limits.h>` the preprocessor read `UINT_MAX`
// as C 6.10.1p4's "identifier that survived expansion" = 0 and took the
// OPPOSITE branch from gcc AND clang, at rc=0 with no diagnostic.
//
// ★★ EVERY CELL BELOW IS ARM-DISCRIMINATING, never "it compiled". A wrong
// branch IS a successful compile of the wrong program, so a pin that asserts
// `!hasErrors()` is structurally blind to the entire defect class.
//
// ★ THE HARNESS SYNTHESIZES ITS OWN DESCRIPTOR rather than reading the shipped
// `limits.json`, so it pins the MECHANISM (a `constants` row becomes a
// preprocessor macro with the right value AND the right signedness) and not one
// header's current contents.
// ─────────────────────────────────────────────────────────────────────────────
namespace {
// Write a descriptor exercising all four shapes the splice must get right:
// an unsigned constant, a signed one, the MOST NEGATIVE value of its width, and
// a `preprocessorVisible: false` row that must NOT become a macro.
[[nodiscard]] std::filesystem::path ppConstantsSysDir() {
    namespace fs = std::filesystem;
    auto dir = ppScratchRoot() / "dss_pp_shipped_constants_sys";
    fs::create_directories(dir);
    std::ofstream(dir / "limits.json", std::ios::binary) << R"({
  "header": "limits.h",
  "constants": [
    { "name": "T_UINT_MAX", "value": 4294967295, "type": "u32" },
    { "name": "T_INT_MAX",  "value": 2147483647, "type": "i32" },
    { "name": "T_INT_MIN",  "value": -2147483648, "type": "i32" },
    { "name": "T_CHAR_BIT", "value": 8, "type": "i32" },
    { "name": "T_ENUMERATOR", "value": 3, "type": "i32",
      "preprocessorVisible": false }
  ]
}
)";
    return dir;
}

// The taken arm of `#if <cond>` after `#include <limits.h>`, with the synthetic
// descriptor on the system path.
[[nodiscard]] std::string ppShippedConstantArm(std::string const& cond,
                                               PreprocessResult& out) {
    auto const dir = ppConstantsSysDir();
    auto lexs = ppLexemesWithDirs(
        "#include <limits.h>\n#if " + cond
            + "\nint taken_if;\n#else\nint taken_else;\n#endif\n",
        out, {}, {dir});
    bool sawIf = false, sawElse = false;
    for (auto const& l : lexs) {
        if (l == "taken_if")   sawIf = true;
        if (l == "taken_else") sawElse = true;
    }
    if (sawIf && sawElse) return "BOTH";
    if (sawIf)   return "taken_if";
    if (sawElse) return "taken_else";
    return "NEITHER";
}
}  // namespace

TEST(PreprocessorShippedConstants, AConstantsRowIsVisibleToDefinedAndToIf) {
    PreprocessResult r;
    EXPECT_EQ(ppShippedConstantArm("defined(T_UINT_MAX)", r), "taken_if")
        << "THE DISCRIMINATING CELL. `defined()` on a `constants` row was FALSE "
           "while `defined()` on a `macros` row was TRUE -- which is how this "
           "defect was localised to the surface rather than to the bridge";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

TEST(PreprocessorShippedConstants, TheThreeMeasuredWrongArmsNowMatchBothReferences) {
    // gcc 13.3.0 -std=c2x and clang 18.1.3 -std=c2x, probed SEPARATELY with the
    // arm read from the emitted object via `nm`: taken_if on all three, against
    // DSS's taken_else on all three before this fix.
    for (auto const* cond : {"T_UINT_MAX > T_INT_MAX",
                             "T_UINT_MAX > 0",
                             "T_INT_MIN < 0",
                             "T_CHAR_BIT == 8"}) {
        PreprocessResult r;
        EXPECT_EQ(ppShippedConstantArm(cond, r), "taken_if")
            << "cell: " << cond;
        EXPECT_FALSE(r.diagnostics->hasErrors()) << "cell: " << cond;
    }
}

// ★★ THE SIGNEDNESS CELL, and it is the one a decimal-only implementation
// FAILS. `-1 < UINT_MAX` is FALSE in gcc and clang because UINT_MAX is unsigned
// and the -1 converts to uintmax_t. Spell the constant `4294967295` instead of
// `4294967295u` and this arm flips -- so this cell, alone among them, proves the
// splice carries the declared SIGNEDNESS and not merely the value.
TEST(PreprocessorShippedConstants, AnUnsignedConstantConvertsTheOtherOperand) {
    PreprocessResult r;
    EXPECT_EQ(ppShippedConstantArm("-1 < T_UINT_MAX", r), "taken_else")
        << "a signed spelling of the same value takes the OPPOSITE arm; both "
           "references take this one";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// ★ THE NEGATIVE THE SURFACE OWES, in the direction that matters. A name that
// is an ENUMERATION CONSTANT in C (`memory_order_seq_cst`, `thrd_success`) is
// NOT a macro in either reference; making it one would be an invented extension
// -- above the union, which the bar forbids exactly as firmly as falling below
// it. `preprocessorVisible: false` is the ONE field both seams read, and this
// pin is what stops a future "splice every constant" simplification.
TEST(PreprocessorShippedConstants, ASemanticOnlyConstantDoesNotBecomeAMacro) {
    PreprocessResult r;
    EXPECT_EQ(ppShippedConstantArm("defined(T_ENUMERATOR)", r), "taken_else")
        << "a `preprocessorVisible: false` row must stay invisible to the "
           "preprocessor while remaining a semantic constant";
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// THE CONTROL. An expression with no shipped constant in it at all, so this
// harness can demonstrate AGREEMENT and is not merely stuck reporting
// divergence -- and a descriptor that failed to load would fail this too.
TEST(PreprocessorShippedConstants, ControlAnOrdinaryConditionIsUnaffected) {
    PreprocessResult r;
    EXPECT_EQ(ppShippedConstantArm("2 + 2 == 4", r), "taken_if");
    EXPECT_FALSE(r.diagnostics->hasErrors());
}

// ─────────────────────────────────────────────────────────────────────────────
// [[D-PP-COMPUTED-INCLUDE-SILENT-DROP]] — C23 6.10.2p4, THE COMPUTED `#include`
//
// `#include pp-tokens` whose operand is neither `"q-char-seq"` nor
// `<h-char-seq>`: the operand is MACRO-EXPANDED and the result must spell one of
// those forms. Before this cycle DSS neither honoured it NOR reported it. The
// mechanism of the silence is the part worth writing down, because "the
// pre-scan skipped it" is only half: the macro pass then FORWARDED the line as
// inert tokens, the operand expanded in the ORDINARY BODY STREAM, and the
// parser's `includeDirective` rule -- which carries `hirKind: Skip` -- matched
// the resulting well-formed `#include "h"` and lowered it to NOTHING. ✔MEASURED
// at dac121cc: a computed include of a MISSING header compiled rc=0, emitted an
// artifact and said nothing at all.
//
// ★ THE REFERENCE SET IS UNANIMOUS, probed SEPARATELY (2026-09-01): gcc 13.3.0
// and clang 18.1.3 COMPILE AND RUN the quote / angle / multi-token-angle /
// chained / stringize shapes; MSVC 19.51.36252 resolves every one. All three
// hard-error on an operand that expands to nothing. So this is REQUIRED by
// `DSS = (gcc union clang union MSVC) union ISO C`, and a loud refusal would
// still be a refusal of correct C.
//
// Every pin below is REMOVE-direction red-on-disable, and the two directions are
// both covered on purpose: reverting the RESOLUTION turns a working splice into
// a hard error (loud, still not silent), and reverting the BACKSTOP turns a hard
// error into the original silence.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// A scratch include dir carrying `computed_inner.h` (a macro + a declaration)
// and `sub/computed_deep.h`. Each caller owns its own directory so the shuffled
// second ctest entry of this binary cannot contend for it (see ppScratchRoot).
[[nodiscard]] std::filesystem::path computedIncludeDir(std::string const& tag) {
    namespace fs = std::filesystem;
    auto const inc = ppScratchRoot() / ("dss_computed_include_" + tag);
    fs::create_directories(inc / "sub");
    { std::ofstream(inc / "computed_inner.h", std::ios::binary)
        << "#define COMPUTED_OK 7\nint computed_sym;\n"; }
    { std::ofstream(inc / "sub" / "computed_deep.h", std::ios::binary)
        << "int computed_deep_sym;\n"; }
    return inc;
}
[[nodiscard]] bool hasLexeme(std::vector<std::string> const& lexs,
                             std::string_view want) {
    for (auto const& l : lexs) if (l == want) return true;
    return false;
}
} // namespace

// (1) THE HEADLINE, in the direction the row is written in. A computed
// `#include` of a header that EXISTS must splice it -- the macro must expand and
// the declaration must arrive -- and the directive must be fully consumed.
// RED-ON-DISABLE: revert the pre-scan's Quote arm and the operand is left
// verbatim, so `COMPUTED_OK` never expands to `7`, `computed_sym` never appears,
// AND the macro pass's backstop fires -- three independent failures.
TEST(Preprocessor, ComputedIncludeQuoteFormSplicesTheHeader) {
    namespace fs = std::filesystem;
    auto const inc = computedIncludeDir("quote");
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#define HDR \"computed_inner.h\"\n#include HDR\nint u = COMPUTED_OK;\n",
        r, {inc}, {});
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a computed #include of an existing header must resolve cleanly "
           "(gcc/clang/MSVC all compile and run this)";
    EXPECT_TRUE(hasLexeme(lexs, "computed_sym"))
        << "the computed include's header text must be spliced into the TU";
    EXPECT_TRUE(hasLexeme(lexs, "7"))
        << "the spliced header's macro must EXPAND -- proving a real textual "
           "splice, not a directive that merely stopped erroring";
    EXPECT_FALSE(hasLexeme(lexs, "HDR"))
        << "the operand macro must not survive into the parser-visible stream";
    EXPECT_FALSE(hasLexeme(lexs, "#"))
        << "the computed directive must be consumed, not forwarded verbatim";
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// (2) THE SILENT DROP ITSELF. A computed `#include` of a MISSING header must
// fail loud. ✔MEASURED at dac121cc: this exact program compiled rc=0 with ZERO
// diagnostics and emitted an artifact. RED-ON-DISABLE in the strongest sense --
// reverting either half of the fix restores that silence and this goes red,
// because the assertion is on the DIAGNOSTIC, not on a token count.
TEST(Preprocessor, ComputedIncludeOfAMissingHeaderFailsLoudNeverSilently) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define HDR \"no_such_computed_header.h\"\n#include HDR\nint x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "a LIVE computed #include whose header does not exist must be "
           "REPORTED; vanishing is the miscompile class this row closes";
    (void)lexs;
}

// (3) THE ANGLE FORM. `#define H <foo.h>` cannot reach the resolver as its own
// bytes: the post-parse import resolver reads the ANGLE form only, and
// `#include H` is not it (✔MEASURED at dac121cc: `D_UnresolvedImport
// <malformed include>` plus `P_NoAlternativeMatched: expected 'StringStart' or
// 'HeaderStart'`). The pre-scan therefore NORMALIZES a computed angle include to
// its canonical literal spelling. Here the header is a real source file on the
// -I path, so the angle SOURCE fallback splices it textually -- the same outcome
// the literal `#include <sub/computed_deep.h>` control produces.
// RED-ON-DISABLE: revert the Angle arm -> the operand is left verbatim ->
// `computed_deep_sym` never arrives and the backstop fires.
TEST(Preprocessor, ComputedIncludeAngleFormNormalizesToTheLiteralSpelling) {
    namespace fs = std::filesystem;
    auto const inc = computedIncludeDir("angle");
    {
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#define H <sub/computed_deep.h>\n#include H\nint x;\n", r, {inc}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "a computed ANGLE include of a resolvable header must not error";
        EXPECT_TRUE(hasLexeme(lexs, "computed_deep_sym"))
            << "the computed angle include must resolve through the SAME funnel "
               "the literal form uses (multi-token name re-assembly included)";
    }
    {   // THE CONTROL. The literal spelling of the same include, same dirs.
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#include <sub/computed_deep.h>\nint x;\n", r, {inc}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors());
        EXPECT_TRUE(hasLexeme(lexs, "computed_deep_sym"))
            << "the LITERAL angle form is the control: the computed form must "
               "reach exactly this outcome, not merely some outcome";
    }
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// (4) THE IMPLEMENTATION-DEFINED JOIN, pinned against the reference set rather
// than against taste. C 6.10.2p4 leaves "the method by which a sequence of
// preprocessing tokens between a < and a > is combined into a single header
// name" implementation-defined. ✔MEASURED 2026-09-01: gcc 13.3.0, clang 18.1.3
// AND MSVC 19.51 all PRESERVE the operand's internal whitespace -- each quotes
// the name back as `sys / types.h` in its not-found message -- so a
// whitespace-NORMALIZING join would disagree with all three at once. The
// un-resolvable name is left in the stream as the canonical angle spelling,
// which is what makes the assembled name directly observable here.
// RED-ON-DISABLE: drop the span-adjacency test in the join and the lexeme
// becomes `nosuchdir/x.h` (spaces eaten) or `no such dir / x . h` (spaces
// everywhere); either way this EXPECT fails.
TEST(Preprocessor, ComputedIncludeAngleNameKeepsTheOperandsInternalSpacing) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define H <no such dir/x.h>\n#include H\nint x;\n", r);
    EXPECT_TRUE(hasLexeme(lexs, "no such dir/x.h"))
        << "the assembled h-char-sequence must preserve the operand's internal "
           "whitespace exactly once per gap -- what gcc, clang AND MSVC do";
}

// (5) THE CONSTRAINT VIOLATION. An operand that expands to NOTHING is not a
// header name, and all three references say so (`#include expects "FILENAME" or
// <FILENAME>` / `expected "FILENAME" or <FILENAME>` / `error C2006`). The
// pre-scan is authoritative here -- it expanded every name it was asked to -- so
// it reports rather than deferring. RED-ON-DISABLE: without the Malformed arm
// this produces no preprocessor diagnostic at all (the fault surfaced only as a
// downstream PARSER error, which `preprocess()` never sees).
TEST(Preprocessor, ComputedIncludeExpandingToNothingIsAConstraintViolation) {
    PreprocessResult r;
    auto lexs = ppLexemes("#define H\n#include H\nint x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "an operand that expands to nothing must be reported as the C 6.10.2 "
           "constraint violation it is";
    (void)lexs;
}

// (6) THE BACKSTOP, and the honest boundary of this cycle. The include pre-scan
// expands OBJECT-like macros only -- deliberately, because a second
// function-like expander here is the duplication
// [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]] already charges this pass for.
// gcc/clang/MSVC all resolve the stringize form, so DSS REFUSING it is a real
// residual; what must never happen is that it vanishes. This pins the refusal.
// RED-ON-DISABLE: remove the macro pass's computed-include arm and the operand is
// forwarded, expands in the body stream, and the parser's `hirKind: Skip` include
// rule swallows it -- no diagnostic, no header, exactly the original defect.
TEST(Preprocessor, ComputedIncludeWithAFunctionLikeOperandFailsLoudNeverSilent) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define STR(x) #x\n#define HDR(x) STR(x)\n#include HDR(inner.h)\n"
        "int x;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "an operand this pre-scan cannot expand must be REFUSED loudly; the "
           "one outcome forbidden is silence";
    (void)lexs;
}

// (7) THE NEGATIVE THE RESOLUTION OWES. A computed include in a DEAD branch must
// NOT resolve and must NOT error -- ✔MEASURED as required reference behaviour
// (gcc compiles a dead-branch include of a missing header rc=0). This is the
// P0016 direction: an eager resolve in a branch the pre-scan is not confident
// about is the hazard the `includeResolvable()` gate exists for, and without
// that gate this test goes red with a spurious not-found error.
TEST(Preprocessor, ComputedIncludeInADeadBranchNeitherResolvesNorErrors) {
    PreprocessResult r;
    auto lexs = ppLexemes(
        "#define HDR \"no_such_computed_header.h\"\n#if 0\n#include HDR\n#endif\n"
        "int x;\n", r);
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a dead-branch computed include must be inert -- resolving it would "
           "re-open the P0016 class the pre-scan's live-gate closes";
    EXPECT_FALSE(hasLexeme(lexs, "#"))
        << "the dead branch (directive included) must be elided by the macro pass";
}

// (8) THE CROSS-BUFFER CASE. The operand macro is defined in a QUOTE-INCLUDED
// HEADER, so resolving it needs the pre-scan's macro state to be shared across
// the whole builder tree (TF-C60's `localMacros`-by-reference). A per-builder
// map would leave `HDR` unknown here -> Unresolvable -> the backstop's hard
// error, so this pin is red the moment that sharing regresses.
TEST(Preprocessor, ComputedIncludeResolvesAMacroDefinedInAnIncludedHeader) {
    namespace fs = std::filesystem;
    auto const inc = computedIncludeDir("crossbuffer");
    { std::ofstream(inc / "computed_names.h", std::ios::binary)
        << "#define HDR \"computed_inner.h\"\n"; }
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#include \"computed_names.h\"\n#include HDR\nint u = COMPUTED_OK;\n",
        r, {inc}, {});
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "the operand macro arrives via a nested include; the pre-scan's "
           "shared macro state must see it";
    EXPECT_TRUE(hasLexeme(lexs, "computed_sym"))
        << "the header named by a header-supplied macro must be spliced";
    EXPECT_TRUE(hasLexeme(lexs, "7"));
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// (9) THE CONTROL ARM ON BOTH SIDES. The literal forms must be untouched by the
// computed path -- a quote include still splices, and an `#include` line the
// pre-scan already handled must not trip the new backstop. Without a control a
// green above could mean "the computed arm works" or "every include now errors".
TEST(Preprocessor, ComputedIncludeArmLeavesTheLiteralFormsUnchanged) {
    namespace fs = std::filesystem;
    auto const inc = computedIncludeDir("control");
    PreprocessResult r;
    auto lexs = ppLexemesWithDirs(
        "#include \"computed_inner.h\"\nint u = COMPUTED_OK;\n", r, {inc}, {});
    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "the LITERAL quote include is the control and must stay clean";
    EXPECT_TRUE(hasLexeme(lexs, "computed_sym"));
    EXPECT_TRUE(hasLexeme(lexs, "7"));
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// (10) THE ANGLE DELIMITERS ARE CONFIG, NOT THE `<`/`>` BYTES. A computed
// operand's `<`/`>` are the ORDINARY comparison operators -- the tokenizer's
// `header-context` mode (which mints `HeaderStart`) is entered only after a
// literal `#include`, never inside a macro REPLACEMENT LIST -- so this arm
// matches `hasIncludeAngleOpenToken`/`CloseToken`, the very kinds
// `__has_include` and `__has_embed` already use for the same job. This is the
// JSON-KEY-REBINDING mutant that proves it: rebind the OPEN delimiter off `LtOp`
// and the operand can no longer spell an angle form, so it becomes the C 6.10.2
// constraint violation instead of resolving.
// RED-ON-DISABLE: matching the `<` BYTE (or hard-coding `find("LtOp")`) ignores
// the rebind, the include still resolves, no diagnostic fires, and this fails.
TEST(Preprocessor, ComputedIncludeAngleDelimitersAreConfigDrivenNotHardcoded) {
    namespace fs = std::filesystem;
    auto const inc = computedIncludeDir("rebind");
    std::vector<std::filesystem::path> dirs{inc};
    std::vector<std::filesystem::path> noSys;
    {   // BASELINE: the shipped config resolves it.
        auto buf = SourceBuffer::fromString(
            std::string{"#define H <sub/computed_deep.h>\n#include H\nint x;\n"},
            "main.c");
        PreprocessResult r = preprocess(buf, cSubset(), dirs,
                                        dss::kDefaultHeaderNameMatching,
                                        DiagnosticBudget::libraryDefault(), noSys);
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "baseline control: the shipped angle-delimiter kinds resolve this";
    }
    {   // REBOUND: `hasIncludeAngleOpenToken` moved off `LtOp`.
        auto schema = reboundC("\"hasIncludeAngleOpenToken\":  \"LtOp\"",
                               "\"hasIncludeAngleOpenToken\":  \"PlusOp\"",
                               "<rebound-angle-open-c>");
        ASSERT_NE(schema, nullptr);
        ASSERT_EQ(schema->preprocess().hasIncludeAngleOpenToken, "PlusOp");
        auto buf = SourceBuffer::fromString(
            std::string{"#define H <sub/computed_deep.h>\n#include H\nint x;\n"},
            "main.c");
        PreprocessResult r = preprocess(buf, schema, dirs,
                                        dss::kDefaultHeaderNameMatching,
                                        DiagnosticBudget::libraryDefault(), noSys);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "with the angle OPEN delimiter rebound away, the operand no "
               "longer spells a header name -- the arm must read the KIND from "
               "config, never the `<` byte";
    }
    std::error_code ec;
    fs::remove_all(inc, ec);
}

// (11) THE SHAPE THAT IS ACTUALLY IN THE CORPUS, and the reason it stays quiet.
// ✔MEASURED over the staged sqlite tree at dac121cc: SEVEN real computed
// includes, and every single one is the FUNCTION-LIKE stringize operand under an
// `#ifdef` -- `src/sqliteInt.h` and `bld/sqlite3.c` (`# include
// INC_STRINGIFY(SQLITE_CUSTOM_INCLUDE)`), `bld/shell.c` twice
// (`SHELL_STRINGIFY` of `SQLITE_CUSTOM_INCLUDE` and of `SQLITE_SHELL_EXTSRC`),
// `bld/tclsqlite3.c`, and the jni/wasm pair. That is EXACTLY the operand shape
// pin (6) proves this pre-scan refuses -- so if a dead `#ifdef` did not keep
// them inert, this change would hard-error the project's headline corpus on
// unmodified upstream source.
//
// It does keep them inert, in BOTH tiers and for two independent reasons: the
// pre-scan never reaches the operand (`includeResolvable()` is false on a dead
// stack) and the macro pass returns at its dead-branch gate before the include
// arm. This pin holds that open. The shell.c variant is included because it
// differs in the way that matters: its stringify macros are defined OUTSIDE the
// guard, so they ARE in the pre-scan's table when the dead include line is
// reached -- the one arrangement in which a liveness slip would actually
// produce a name.
TEST(Preprocessor, ComputedIncludeSqliteStringifyShapeUnderADeadGuardIsInert) {
    {   // src/sqliteInt.h / bld/sqlite3.c: macros defined INSIDE the guard.
        PreprocessResult r;
        auto lexs = ppLexemes(
            "#ifdef SQLITE_CUSTOM_INCLUDE\n"
            "# define INC_STRINGIFY_(f) #f\n"
            "# define INC_STRINGIFY(f) INC_STRINGIFY_(f)\n"
            "# include INC_STRINGIFY(SQLITE_CUSTOM_INCLUDE)\n"
            "#endif\n"
            "int x;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "the sqliteInt.h computed-include shape under an undefined "
               "#ifdef must be INERT -- reporting it would red the corpus on "
               "unmodified upstream source";
        EXPECT_FALSE(hasLexeme(lexs, "#"))
            << "the whole dead group must be elided";
    }
    {   // bld/shell.c: stringify macros defined OUTSIDE the guard.
        PreprocessResult r;
        auto lexs = ppLexemes(
            "# define SHELL_STRINGIFY_(f) #f\n"
            "# define SHELL_STRINGIFY(f) SHELL_STRINGIFY_(f)\n"
            "#ifdef SQLITE_CUSTOM_INCLUDE\n"
            "# include SHELL_STRINGIFY(SQLITE_CUSTOM_INCLUDE)\n"
            "#endif\n"
            "int x;\n", r);
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "the shell.c variant -- stringify macros VISIBLE to the pre-scan "
               "at the dead include line -- must be inert for the same reason";
        (void)lexs;
    }
    {   // THE POSITIVE CONTROL, so the two greens above cannot mean "this arm
        // never fires". Same source, guard made LIVE by a command-line define:
        // the operand is still unexpandable here, so it must be REFUSED LOUDLY.
        auto schema = cSubset();
        auto buf = SourceBuffer::fromString(
            std::string{"# define SHELL_STRINGIFY_(f) #f\n"
                        "# define SHELL_STRINGIFY(f) SHELL_STRINGIFY_(f)\n"
                        "#ifdef SQLITE_CUSTOM_INCLUDE\n"
                        "# include SHELL_STRINGIFY(SQLITE_CUSTOM_INCLUDE)\n"
                        "#endif\n"
                        "int x;\n"},
            "main.c");
        std::vector<std::filesystem::path> noDirs;
        std::vector<std::string> defines{"SQLITE_CUSTOM_INCLUDE=x.h"};
        auto r = preprocess(buf, schema, noDirs, dss::kDefaultHeaderNameMatching,
                            DiagnosticBudget::libraryDefault(), {}, std::nullopt,
                            defines);
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorIncludeError))
            << "with the guard LIVE the same directive must fail loud -- the "
               "inertness above is the DEAD-branch rule, not a disarmed arm";
    }
}

// (12) THE EMPTY OPERAND, and the DUPLICATION it nearly caused. A bare
// `#include` with nothing after it is the degenerate computed form: the operand
// run is empty, so the expansion is empty, so it is the same C 6.10.2 constraint
// violation pin (5) covers -- all three references reject it. It gets its own
// pin because it is the ONE shape with no operand token to read a cut point off,
// and the first draft of this arm seeded that cut point at 0. That would have
// REWOUND `copiedUpTo` behind everything already emitted, and the closing
// `copyVerbatim` would have re-emitted the WHOLE buffer prefix -- a silent
// DUPLICATION, strictly worse than the silent drop this row closes. The cut is
// seeded at the directive WORD's end instead, and the duplication half is what
// the lexeme counts below assert; the diagnostic half is the easy part.
TEST(Preprocessor, ABareIncludeIsReportedAndNeverDuplicatesTheBuffer) {
    PreprocessResult r;
    auto lexs = ppLexemes("int before;\n#include\nint after;\n", r);
    EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "an `#include` with no operand is the C 6.10.2 constraint violation "
           "gcc, clang and MSVC all report";
    auto count = [&](std::string_view want) {
        std::size_t n = 0;
        for (auto const& l : lexs) if (l == want) ++n;
        return n;
    };
    EXPECT_EQ(count("before"), 1u)
        << "the text BEFORE the malformed directive must appear exactly once -- "
           "a rewound cut point re-emits the whole prefix";
    EXPECT_EQ(count("after"), 1u)
        << "the text AFTER it must appear exactly once";
    EXPECT_FALSE(hasLexeme(lexs, "include"))
        << "the malformed directive is the sole reporter and drops its own line";
}

// (13) THE h-char-SEQUENCE ENDS AT THE FIRST `>`, AND TRAILING TOKENS ARE
// TOLERATED. C 6.4.7 defines an h-char as any character except `>` and a
// newline, so the sequence cannot contain one -- reading the LAST closer instead
// of the first would refuse a shape the references accept. ✔MEASURED 2026-09-01:
// `#define H <inner.h> trailing` + `#include H` COMPILES AND RUNS under gcc
// 13.3.0 and clang 18.1.3, each emitting only `warning: extra tokens at end of
// #include directive`; the quote form with a trailing token does the same. An
// UNTERMINATED operand (`#define H <inner.h`) is a hard error in BOTH (`missing
// terminating > character` / `expected '>'`), so that half must stay refused.
// This is a bar pin in both directions at once: too strict is BELOW the union,
// too loose is ABOVE it.
// RED-ON-DISABLE: reading `ex.back()` as the closer makes the first case
// Malformed (an error where two references warn); dropping the unterminated
// check makes the second resolve a header name with no closer at all.
TEST(Preprocessor, ComputedIncludeTakesTheFirstCloserAndToleratesTrailingTokens) {
    namespace fs = std::filesystem;
    auto const inc = computedIncludeDir("trailing");
    {   // TRAILING TOKENS: accepted, and the header really is spliced.
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#define H <sub/computed_deep.h> trailing\n#include H\nint x;\n",
            r, {inc}, {});
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "gcc and clang both COMPILE AND RUN a computed include with "
               "trailing tokens -- refusing it would put DSS below the union";
        EXPECT_TRUE(hasLexeme(lexs, "computed_deep_sym"))
            << "the header name ends at the FIRST `>`; the trailing token is "
               "ignored exactly as the literal arms ignore theirs";
    }
    {   // UNTERMINATED: refused, as both references refuse it.
        PreprocessResult r;
        auto lexs = ppLexemesWithDirs(
            "#define H <sub/computed_deep.h\n#include H\nint x;\n",
            r, {inc}, {});
        EXPECT_TRUE(hasPPCode(r, DiagnosticCode::P_PreprocessorDirective))
            << "an operand with no terminating `>` is a hard error in gcc AND "
               "clang; accepting it would put DSS above the union";
        (void)lexs;
    }
    std::error_code ec;
    fs::remove_all(inc, ec);
}
