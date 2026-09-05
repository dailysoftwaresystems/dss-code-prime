// [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]] — THE INCLUDE PRE-SCAN'S MACRO STATE
// IS THE AUTHORITATIVE MACRO EXPANDER, NOT A WEAKER SHADOW OF IT.
//
// ★★★ WHAT THE DEFECT WAS. The pre-scan (`SynthBuilder`) has to decide, BEFORE
// the macro pass runs, which quote-`#include`s are live — it must splice them so
// that pass can run over one frozen buffer. To decide that it must evaluate
// `#if`. It used to do so with a PRIVATE evaluator that expanded OBJECT-LIKE
// macros only, and every guard that evaluator could not decide became a
// CONSERVATIVE SKIP: the header was never spliced, and the macro pass then
// refused the program with `P_PreprocessorIncludeError` ("… is LIVE here but the
// include pre-scan could not evaluate its conditional guard").
//
// ★★ WHAT THE REFERENCES DO, EACH PROBED SEPARATELY, 2026-09-03. Every construct
// below compiles under WSL gcc 13.3.0, WSL clang 18.1.3, mingw-w64 gcc 13.2.0 and
// MSVC 19.51.36252 (`cl /nologo /c /std:c17`), and the three that link also RUN
// the program to its expected exit code. The union is UNANIMOUS, so
// `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` REQUIRES all of it — there is no vote to
// weigh and no reference to match against another.
//
// ★ WHY A SEPARATE BINARY RATHER THAN MORE CASES IN test_preprocessor.cpp.
// Every arm here needs a REAL HEADER ON DISK reached through a scratch dir that
// the fixture makes the process cwd — the only way a quote-`#include` resolution
// is observable at all — while that file's fixtures preprocess in-memory buffers
// with no include path. It is the same split `test_pragma_once_include_dedup`
// and `test_include_bare_relative_includer_dir` already make, for the same
// reason.
//
// ★★ THE PAIRING RULE THIS FILE FOLLOWS. Every "it now splices" arm is paired
// with a DEAD arm of the same shape whose header DOES NOT EXIST. A fix that made
// the pre-scan splice by becoming eager rather than by becoming correct would
// pass the first half and fail the second — that eagerness is P0016
// (D-PP-CONDITIONAL-INCLUDE-ORDERING) itself, and the conservative direction
// this row narrows is the only thing that ever stood between the two.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"

#include "test_support/repo_root.hpp"
#include "test_support/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// The header every "it spliced" arm looks for, and the macro that proves the
// splice really happened rather than the directive merely being dropped.
constexpr std::string_view kHeaderName  = "oracle_h.h";
constexpr std::string_view kHeaderText  = "#define ORACLE_VALUE 42\n";
constexpr std::string_view kMissingName = "oracle_no_such_header.h";
constexpr std::string_view kMainName    = "oracle_main.c";

// Shared schema fixture. Returns a REFERENCE to a function-local static: a
// `GrammarSchema`'s accessors hand back references INTO the schema, so a
// by-value return makes `helper()->accessor()` a heap-use-after-free
// (D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE).
//
// ⚠ CALL THIS BEFORE `useAsCwd()`. Schema discovery reads `$DSS_CONFIG_ROOT`
// first — which `dss_add_test` sets — but its fallback walks UP FROM THE CWD, so
// forcing the static while the cwd is still the launch directory keeps this file
// honest under a bare `.exe` run too.
[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cSchema() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            // THROW, never `std::abort()`: abort kills the whole test BINARY and
            // every sibling loses its verdict. Machine-checked by
            // check-no-abort-in-tests.
            throw std::runtime_error{"loadShipped(c) failed"};
        }
        return *loaded;
    }();
    return schema;
}

// A scratch dir made the process cwd, so a source named with no directory
// component finds the header this fixture writes beside it.
struct CwdFixture {
    test_support::ScratchDir dir{test_support::Location::InsideRepo,
                                 "prescan-macro-oracle"};

    CwdFixture() {
        (void)cSchema();   // force the schema static BEFORE the cwd moves
        dir.useAsCwd();
    }

    void write(std::string_view relName, std::string_view bytes) const {
        fs::path const p = dir.path() / fs::path{relName};
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // Write the header every positive arm needs.
    void writeHeader() const { write(kHeaderName, kHeaderText); }
};

// Preprocess `text` as `oracle_main.c` with NO `-I` dirs, so the ONLY way a
// quote include can resolve is the includer-directory arm — i.e. the scratch dir
// the fixture just made the cwd.
[[nodiscard]] PreprocessResult pp(std::string text) {
    auto buf = SourceBuffer::fromString(std::move(text), std::string{kMainName});
    std::vector<fs::path> const noDirs;
    return preprocess(buf, cSchema(), noDirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

// The same, with the PE object format active. Needed by exactly one pair of arms:
// the C profile gates `__declspec`/`_declspec` to `pe` (`availableObjectFormats`),
// so the function-like-PREDEFINE arms have no subject at all without a format.
// Stated explicitly rather than defaulted, so the dependence is greppable.
[[nodiscard]] PreprocessResult ppPe(std::string text) {
    auto buf = SourceBuffer::fromString(std::move(text), std::string{kMainName});
    std::vector<fs::path> const noDirs;
    return preprocess(buf, cSchema(), noDirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault(), noDirs,
                      ObjectFormatKind::Pe);
}

[[nodiscard]] bool hasCode(PreprocessResult const& r, DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all())
        if (d.code == code) return true;
    return false;
}

// The non-trivia lexemes a parser would see.
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

// THE positive property, in one place so every arm asserts the same thing: the
// guarded header was spliced AND its macro expanded, so `int v = 42 ;`.
//
// ★ IT IS THE EXPANSION, NOT THE ABSENCE OF AN ERROR, THAT IS THE WITNESS. A
// pre-scan that "resolved" the include by dropping the directive would raise no
// diagnostic and produce `int v = ORACLE_VALUE ;` — which is the SILENT DROP
// that the fail-loud this row narrows exists to prevent. Asserting the VALUE is
// what tells the two apart.
void expectSpliced(PreprocessResult const& r, char const* what) {
    EXPECT_FALSE(r.diagnostics->hasErrors()) << what;
    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorIncludeError)) << what;
    auto const lexs = lexemesOf(r);
    ASSERT_EQ(lexs.size(), 5u) << what << " — expected `int v = 42 ;`";
    EXPECT_EQ(lexs[3], "42")
        << what
        << " — the header's macro must have EXPANDED; a dropped directive would "
           "leave the macro name here and raise nothing at all";
}

// THE negative property: a guard that is FALSE must leave the include alone, and
// "alone" means the missing header is never even looked for.
void expectDeadAndSilent(PreprocessResult const& r, char const* what) {
    EXPECT_FALSE(r.diagnostics->hasErrors()) << what;
    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << what
        << " — a DEAD branch's `#include` is not processed at all (C 6.10p1), so "
           "a missing header there must not be resolved, reported, or spliced";
}

// ── 1. THE ROW'S CHARTER: A FUNCTION-LIKE MACRO GATING A QUOTE INCLUDE ──────
//
// RED-ON-DISABLE: make `SynthBuilder::sbEvalIfOperand` return
// `uncertain = true` (the FIX-3 bail this row deleted) and this fails with
// `P_PreprocessorIncludeError` — the CLI's refusal seen from the inside.
TEST(PreScanMacroOracle, FunctionLikeGuardSplicesTheQuoteInclude) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#define ENABLED(x) (x)\n"
        "#if ENABLED(1)\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    expectSpliced(r, "a function-like macro gating a quote #include");
}

// The PAIR. Same shape, guard FALSE, header absent. A pre-scan that became eager
// rather than correct passes the arm above and fails here.
TEST(PreScanMacroOracle, FunctionLikeGuardThatIsFalseResolvesNothing) {
    CwdFixture fx;   // deliberately writes NO header

    PreprocessResult const r = pp(
        "#define ENABLED(x) (x)\n"
        "#if ENABLED(0)\n"
        "#include \"oracle_no_such_header.h\"\n"
        "#endif\n"
        "int v = 1;\n");

    expectDeadAndSilent(r, "a FALSE function-like guard");
}

// ── 2. AN OBJECT-LIKE MACRO THAT EXPANDS *TO* A CALL ───────────────────────
// The old evaluator had a SECOND bail for exactly this (it scanned the
// post-expansion token run for a function-like name), because folding an
// unexpandable identifier to 0 is only safe at even polarity. Both bails are
// gone: the oracle expands the call.
TEST(PreScanMacroOracle, ObjectLikeMacroExpandingToACallSplices) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#define ENABLED(x) (x)\n"
        "#define GATE ENABLED(1)\n"
        "#if GATE\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    expectSpliced(r, "an object-like macro expanding to a function-like call");
}

// The ODD-POLARITY pair, which is the case the second bail was written for:
// `#if !GATE` where GATE expands to a call. Freezing the identifier would fold
// it to 0 and make `!0` TRUE — eagerly resolving an authoritatively DEAD
// include. The header is absent, so that eagerness is caught here.
TEST(PreScanMacroOracle, NegatedCallGuardDoesNotResolveADeadInclude) {
    CwdFixture fx;   // deliberately writes NO header

    PreprocessResult const r = pp(
        "#define ENABLED(x) (x)\n"
        "#define GATE ENABLED(1)\n"
        "#if !GATE\n"
        "#include \"oracle_no_such_header.h\"\n"
        "#endif\n"
        "int v = 1;\n");

    expectDeadAndSilent(r, "`#if !GATE` where GATE expands to a TRUE call");
}

// ── 3. `##` IN THE GUARD ───────────────────────────────────────────────────
// A paste is not something a rescan-only expander can do at all; it needs the
// argument machinery and the mint path. `CAT(ON,E)` -> `ONE` -> rescanned -> 1.
TEST(PreScanMacroOracle, TokenPasteInTheGuardSplices) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#define CAT(a,b) a##b\n"
        "#define ONE 1\n"
        "#if CAT(ON,E)\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    expectSpliced(r, "a `##` paste in the controlling expression");
}

// ── 4. THE COMPUTED `#include` FORM (C23 6.10.2p4) ─────────────────────────
// The SAME weakness through a different arm, and one the row did not name: the
// operand's expansion ran through the same object-like-only rescan, so
// `#include HDR(x)` came back `Unresolvable` and the macro pass refused it.
TEST(PreScanMacroOracle, ComputedIncludeThroughAFunctionLikeMacroResolves) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#define HDR(x) #x\n"
        "#include HDR(oracle_h.h)\n"
        "int v = ORACLE_VALUE;\n");

    expectSpliced(r, "a computed #include whose operand is a function-like call");
}

// The computed form must still REFUSE what every reference refuses: an operand
// whose expansion completes and spells no header name. This is the arm that
// keeps "resolve more" from becoming "accept anything".
TEST(PreScanMacroOracle, ComputedIncludeThatSpellsNoHeaderNameStillFailsLoud) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#define HDR(x) x\n"
        "#include HDR(1234)\n"
        "int v = 1;\n");

    EXPECT_TRUE(hasCode(r, DiagnosticCode::P_PreprocessorDirective))
        << "gcc, clang and MSVC all hard-error on a computed #include whose "
           "expansion is not a header name; widening what the pre-scan can "
           "expand must not widen what it ACCEPTS";
    EXPECT_TRUE(r.diagnostics->hasErrors());
}

// ── 5. A FUNCTION-LIKE *PREDEFINE*, CALLED IN A GUARD ──────────────────────
// This is FINDING-A retired. The pre-scan's define prefix used to EXCLUDE every
// function-like predefined macro, because the weaker evaluator could not expand
// a call and value-seeding one would have made the pre-scan read more-live than
// the real pass. The prefix is now the authoritative `<built-in>` prologue
// verbatim, so `__declspec(x)` (which erases to nothing) is an ordinary macro in
// both passes and `#if __declspec(dllimport) 1` reduces to `#if 1`.
TEST(PreScanMacroOracle, FunctionLikePredefineCalledInAGuardSplices) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = ppPe(
        "#if __declspec(dllimport) 1\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    expectSpliced(r, "a function-like PREDEFINE called in the guard");
}

// The CONTROL FINDING-A's own note pinned: a BARE function-like predefine name
// (no call) must still fold to 0, so `#if !__declspec` is TRUE and splices.
// ✔MEASURED at the time as DSS 42 / mingw-w64 gcc 13.2.0 42. A "fix" that
// value-seeded the call macros would break exactly this.
TEST(PreScanMacroOracle, BareFunctionLikePredefineNameStillFoldsToZero) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = ppPe(
        "#if !__declspec\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    expectSpliced(r, "`#if !__declspec` — a bare call-macro name folds to 0");
}

// ── 6. `#undef` OF A PREDEFINED NAME COMPOSES ──────────────────────────────
// The deleted `undefinedNames` subtraction set existed for exactly this: the old
// definedness oracle answered out of a CONFIG LIST that no directive could
// subtract from. The oracle holds predefines where the authoritative pass holds
// them, so its own `handleUndef` is the whole story.
TEST(PreScanMacroOracle, UndefOfAPredefinedNameComposesWithTheIncludeGate) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#undef __STDC_HOSTED__\n"
        "#ifndef __STDC_HOSTED__\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    // The `#undef` of an implementation predefine WARNS (C23 6.10.10.1p2 is
    // undefined behaviour, not a constraint — gcc and clang both warn and both
    // apply), so assert on errors and on the include code, never on silence.
    EXPECT_FALSE(r.diagnostics->hasErrors());
    expectSpliced(r, "`#undef` of a predefined name gating an include");
}

// The PAIR: without the `#undef`, the same `#ifndef` is DEAD and the missing
// header is never resolved.
TEST(PreScanMacroOracle, WithoutTheUndefTheSameGuardIsDead) {
    CwdFixture fx;   // deliberately writes NO header

    PreprocessResult const r = pp(
        "#ifndef __STDC_HOSTED__\n"
        "#include \"oracle_no_such_header.h\"\n"
        "#endif\n"
        "int v = 1;\n");

    expectDeadAndSilent(r, "`#ifndef` of a still-defined predefine");
}

// ── 7. `__LINE__` IN A GUARD, AND THE DIRECTIONALITY OF THE ONE ESCAPE ─────
//
// The oracle mints into a product tail that belongs to no file, so it has no
// line-map. Rather than answer 1 and diverge, the pre-scan STATES the origin
// line and file it is asking from — it is the pass that builds the map, so this
// is a lookup, not a guess.
//
// ⚠ THIS IS THE ARM THAT KEEPS THE ESCAPE HONEST. `positionDerivedMints()` is a
// bail, and [[feedback-an-escape-every-row-triggers-disarms-the-guard]] is about
// a bail that every row trips and that therefore refuses nothing. If the
// presumed position were dropped, THIS test goes red while every other arm in
// this file stays green — which is what proves the escape is narrow rather than
// universal.
TEST(PreScanMacroOracle, LineMacroInTheGuardIsDecidedNotRefused) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#if __LINE__ > 0\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    expectSpliced(r, "`#if __LINE__ > 0` gating a quote #include");
}

// The FALSE half — and it is the one that proves the line number is REAL rather
// than a constant that happens to satisfy `> 0`. The `#if` is on line 1, so
// `__LINE__ > 1000` is false and the missing header is never resolved.
TEST(PreScanMacroOracle, LineMacroGuardThatIsFalseResolvesNothing) {
    CwdFixture fx;   // deliberately writes NO header

    PreprocessResult const r = pp(
        "#if __LINE__ > 1000\n"
        "#include \"oracle_no_such_header.h\"\n"
        "#endif\n"
        "int v = 1;\n");

    expectDeadAndSilent(r, "`#if __LINE__ > 1000` — a FALSE position guard");
}

// ── 8. THE STATED RESIDUAL: `__COUNTER__` ──────────────────────────────────
//
// The one predefined kind no plumbing reconciles — its value is a function of
// how many times it has been read, and the oracle and the authoritative pass
// advance separate counters at different points by construction. So a guard
// reading it stays UNDECIDABLE for the pre-scan and the include is refused
// LOUDLY rather than silently dropped.
//
// ⚠ THIS TEST ASSERTS A DIVERGENCE FROM THE REFERENCES, DELIBERATELY. ✔MEASURED
// 2026-09-03: gcc 13.3.0 and clang 18.1.3 both compile AND RUN this program.
// DSS refuses it. The test exists so that the residual is a RECORDED position
// with a failing arm attached rather than an unexamined gap — when
// `__COUNTER__` is reconciled, this test goes red and says so.
TEST(PreScanMacroOracle, CounterMacroInTheGuardIsRefusedLoudlyNotDropped) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#if __COUNTER__ >= 0\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    EXPECT_TRUE(hasCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "gcc 13.3.0 and clang 18.1.3 both compile this; DSS refuses it. The "
           "refusal is the STATED residual — what must never happen is the "
           "header being dropped in silence, so this arm pins the LOUDNESS";
    EXPECT_TRUE(r.diagnostics->hasErrors());
}

// ── 9. THE ORDINARY GUARD MUST NOT TRIP THE ESCAPE ─────────────────────────
// The complement of arm 8, and the reason the escape is DIRECTIONAL: a guard
// built from ordinary macros — object-like, function-like, pasted, nested —
// touches no position-derived value at all, so nothing bails.
TEST(PreScanMacroOracle, ADenseOrdinaryGuardNeverTripsTheEscape) {
    CwdFixture fx;
    fx.writeHeader();

    PreprocessResult const r = pp(
        "#define CAT(a,b) a##b\n"
        "#define TWO 2\n"
        "#define PICK(a,b,...) b\n"
        "#define REST(a,...) __VA_ARGS__\n"
        "#define BIG(x) ((x) * TWO)\n"
        "#if BIG(CAT(T,WO)) == 4 && PICK(0,1,9) && REST(0,1) \\\n"
        "    && defined(CAT) && !defined(NOPE)\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    // ⓘ Every operator here is one the ICE actually supports in `#if`
    // (C 6.10.1p4): arithmetic, `&&`, `!`, and `defined`. `sizeof` and a string
    // literal are NOT, so neither appears — an arm that errored for a reason
    // unrelated to this row would say nothing about the escape. The line
    // CONTINUATION is deliberate: a guard split across a `\` is where a
    // text-slicing pre-scan would lose half its operand.
    expectSpliced(r, "a dense ordinary guard — paste, varargs, nesting, defined");
}

// ── 10. THE MACRO STATE STILL SPANS THE INCLUDE TREE IN DOCUMENT ORDER ─────
// TF-C60's property, re-asserted against the replacement: a `#define` arriving
// through a NESTED include must be visible to the parent's later `#if`. The
// oracle is shared by reference across every child builder exactly as the
// `SbMacro` map was; this is the arm that would catch a per-builder oracle.
TEST(PreScanMacroOracle, ADefineFromAnIncludedHeaderGatesALaterIncludeInTheParent) {
    CwdFixture fx;
    fx.writeHeader();
    fx.write("oracle_setup.h", "#define TURN_IT_ON(x) (x)\n");

    PreprocessResult const r = pp(
        "#include \"oracle_setup.h\"\n"
        "#if TURN_IT_ON(1)\n"
        "#include \"oracle_h.h\"\n"
        "#endif\n"
        "int v = ORACLE_VALUE;\n");

    expectSpliced(r,
                  "a function-like #define from a nested header gating a later "
                  "include in the parent");
}

}   // namespace
