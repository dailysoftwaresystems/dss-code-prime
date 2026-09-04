// [[D-CSUBSET-COMPILER-FEATURE-QUERY-OPERATORS]] — THE FEATURE-QUERY OPERATOR
// FAMILY, PINNED AS A FAMILY RATHER THAN AS FOUR SEPARATE NAMES.
//
// ═══ WHAT THIS FILE GUARDS, AND WHY IT IS THE ROW'S NON-OPTIONAL HALF ════════
//
// The row covers `__has_attribute` / `__has_builtin` / `__has_feature` /
// `__has_extension` — four EXTENSION operators DSS does not implement. Its own
// closing work says step (D) is the only part that is not optional:
//
//     "a test that a capability DSS does NOT have answers 0 and REDS if someone
//      later makes it answer 1 without implementing it. That is the guard
//      against (B)'s failure mode."
//
// (B)'s failure mode, in the row's words, is that "AN OPERATOR THAT ANSWERS 1
// FOR SOMETHING DSS DOES NOT IMPLEMENT IS STRICTLY WORSE THAN NOT HAVING THE
// OPERATOR — it routes the header onto a path DSS cannot honour, converting a
// working fallback into a silent miscompile." Nothing in the engine, the config
// loader or the anchor guard can see that class of lie. This file can.
//
// ═══ THE REFERENCE MATRIX THESE ARMS ENCODE ══════════════════════════════════
//
// ✔MEASURED 2026-09-03, each reference invoked SEPARATELY on its own fixture
// (`-E -P` for the three gcc/clang builds; `cl /nologo /std:c17 /EP`, and again
// with `/Zc:preprocessor`, for MSVC — a front-end question needs no vcvars):
//
//   NAME VISIBLE TO `#ifdef`     gcc 13.3.0  gcc 13.2.0   clang     cl
//                                  (WSL)      (mingw)    18.1.3   19.51.36252
//     __has_include                  yes        yes        yes        yes
//     __has_c_attribute              yes        yes        yes        yes
//     __has_embed                    no         no         no         no
//     __has_attribute                yes        yes        yes        NO
//     __has_builtin                  yes        yes        yes        NO
//     __has_feature                  no         no         yes        no
//     __has_extension                no         no         yes        no
//     __has_warning                  no         no         yes        no
//     __has_declspec_attribute       no         no         yes        no
//     __is_identifier                no         no         yes        no
//     __has_include_next             yes        yes        yes        no
//     defined                        no         no         no         no
//
// ★★★ THE RULE THAT TABLE STATES, AND IT IS THE ONE THIS FILE PINS: a name is
// `#ifdef`-visible on a reference EXACTLY WHEN THAT REFERENCE IMPLEMENTS THE
// OPERATOR. MSVC's `no` on `__has_attribute` is not an opinion about the name —
// it is MSVC correctly reporting that it has no such operator, and it is what
// makes the universal `#ifndef __has_attribute / #define __has_attribute(x) 0 /
// #endif` shim WORK there. Visibility is a CONSEQUENCE of implementation, never
// a decoration added to a name, and every arm below is written so that a future
// cycle that separates the two goes red.
//
// ⚠⚠ THE ONE THING A FUTURE CYCLE IS MOST LIKELY TO DO WRONG, stated with the
// measurement that settles it. `isConditionalInclusionOperator`
// (core/types/preprocess_config.hpp) bundles TWO properties: a name in that set
// is (1) visible to `#ifdef`/`defined` AND (2) REFUSED as the subject of
// `#define`/`#undef`. Adding these four names there looks like the fix and is
// not, because only property (1) belongs to them:
//   • C23 6.10.10p2 names FOUR identifiers and only four — `defined`,
//     `__has_c_attribute`, `__has_include`, `__has_embed`. NONE of this row's
//     four is reserved by the standard.
//   • ✔MEASURED, ALL FOUR references ACCEPT `#define __has_attribute(x) 0`
//     (rc 0, at most a warning: gcc/mingw `"__has_attribute" redefined`, clang
//     `-Wbuiltin-macro-redefined`, cl silent).
// So refusing it would be a divergence from every reference AND would defeat the
// `#ifndef` shim, after which `#if __has_builtin(x)` is a function-like
// invocation of an undefined macro inside `#if` — a hard failure in a header
// included by essentially every macOS TU. `TheDefineRefusalCoversExactlyTheIso
// ReservedIdentifiers` below is the tripwire, with the ISO trio as its control.
//
// ═══ WHY A SEPARATE BINARY RATHER THAN MORE CASES IN test_preprocessor.cpp ════
//
// Two reasons, and the first is the load-bearing one. (a) These arms are ONE
// ENUMERATION whose joint reading is the guard: "visible iff declared", "the
// shim still installs", "an unimplemented capability answers 0", "the refusal
// covers exactly the reserved names" and "an unrecognised query fails loud" are
// five halves of a single invariant, and splitting them across a 10k-line file
// is what lets a later cycle satisfy one and break another without noticing.
// (b) The pre-scan arm needs a REAL HEADER ON DISK reached through an InsideRepo
// ScratchDir made the process cwd — a quote-include resolution is not observable
// any other way — which that file's in-memory buffer fixtures cannot express.
// Same split, same reason, as `test_prescan_macro_oracle` beside it.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/source_buffer.hpp"

#include "test_support/repo_root.hpp"
#include "test_support/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace dss;
namespace fs = std::filesystem;

// Shared schema fixture. Returns a REFERENCE to a function-local static: a
// `GrammarSchema`'s accessors hand back references INTO the schema, so a
// by-value return makes `helper()->accessor()` a heap-use-after-free
// (D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE).
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

// The shipped `c` document's TEXT, for the config-mutant arm below. Reached
// through the ONE test-side resolver (`repo_root.hpp`), never a private cwd
// walk — [[D-TEST-HELPERS-IGNORE-DSS-CONFIG-ROOT-OUT-OF-TREE]] cost 28 ctest
// entries to the seventeen files that each had their own.
[[nodiscard]] std::string shippedCText() {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    fs::path const cand = *root / "sources" / "c.lang.json";
    std::ifstream in(cand, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read the shipped c config: " << cand.string();
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

[[nodiscard]] PreprocessResult pp(std::string text) {
    auto buf = SourceBuffer::fromString(std::move(text), "fq_main.c");
    std::vector<fs::path> const noDirs;
    return preprocess(buf, cSchema(), noDirs, kDefaultHeaderNameMatching,
                      DiagnosticBudget::libraryDefault());
}

[[nodiscard]] bool hasCode(PreprocessResult const& r, DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all())
        if (d.code == code) return true;
    return false;
}

[[nodiscard]] std::optional<DiagnosticSeverity> codeSeverity(
    PreprocessResult const& r, DiagnosticCode code) {
    for (auto const& d : r.diagnostics->all())
        if (d.code == code) return d.severity;
    return std::nullopt;
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

[[nodiscard]] bool sawLexeme(PreprocessResult const& r, std::string_view want) {
    auto const lexs = lexemesOf(r);
    return std::find(lexs.begin(), lexs.end(), want) != lexs.end();
}

// ── THE ROSTER ───────────────────────────────────────────────────────────────
// Every feature-query operator this project has a reason to have an opinion
// about: the three C23 CONDITIONAL-INCLUSION operators DSS implements, this
// row's four, and five further spellings the reference sweep turned up —
// `__has_include_next` (gcc and clang, not MSVC), `__has_warning`,
// `__is_identifier` and `__has_declspec_attribute` (clang alone), and
// `__has_cpp_attribute` (a C++ operator, on the roster precisely BECAUSE a C
// front end must not grow it by accident). Kept as one list on purpose — a
// roster that only holds the names already handled cannot notice a name that
// stopped being handled.
constexpr std::string_view kRoster[] = {
    "__has_include",  "__has_embed",   "__has_c_attribute",
    "__has_attribute", "__has_builtin", "__has_feature", "__has_extension",
    "__has_include_next", "__has_warning", "__is_identifier",
    "__has_declspec_attribute", "__has_cpp_attribute",
};

// This row's four EXTENSION operators — the ones C23 6.10.10p2 does NOT reserve.
constexpr std::string_view kExtensionFour[] = {
    "__has_attribute", "__has_builtin", "__has_feature", "__has_extension",
};

// THE SET THE ENGINE ACTUALLY HONOURS, read from the SCHEMA rather than
// restated here. This is what keeps the file from becoming a second, drifting
// copy of the config ([[D-CONFIG-COMMENT-CLAIM-ROT]]): the day a cycle declares
// `__has_attribute` in `c.lang.json`, the arms below keep asserting the
// INVARIANT instead of going stale on a hardcoded expectation.
//
// ★★★ AND THAT DAY CAME — P59 shipped all four, and the predicate this reads
// MOVED, which is precisely the event the paragraph above was written for.
// `isImplementationProvidedOperator` is the VISIBILITY question alone;
// `isConditionalInclusionOperator` (still used by arm 4 below) is the narrower
// C23 6.10.10p2 RESERVED set. Bundling the two is the mistake this file's
// header warns about at length, and separating them is what let the four ship
// without breaking the portable `#ifndef` shim.
[[nodiscard]] bool languageDeclaresOperator(std::string_view name) {
    return isImplementationProvidedOperator(name, cSchema()->preprocess());
}

// The C23 6.10.10p2 RESERVED subset — the names whose `#define`/`#undef` the
// standard makes implementation business. Distinct from the predicate above on
// purpose; see arm 4.
[[nodiscard]] bool languageReservesOperator(std::string_view name) {
    return isConditionalInclusionOperator(name, cSchema()->preprocess());
}

// The declared feature-query operator for `name`, or null.
[[nodiscard]] FeatureQueryOperatorDef const* declaredFeatureQuery(
    std::string_view name) {
    return findFeatureQueryOperator(name, cSchema()->preprocess());
}

// Does the language declare a BUILTIN FUNCTION by this name? This is the truth
// set `__has_builtin` would have to answer from — the row's step (B) in one
// sentence: "`__has_builtin(__is_target_arch)` [answers 1] only if that builtin
// really exists", DERIVED from a declared capability set, never hard-coded.
[[nodiscard]] bool languageDeclaresBuiltin(std::string_view name) {
    for (auto const& b : cSchema()->semantics().builtinFunctions)
        if (b.name == name) return true;
    return false;
}

// ...and the same question for an ATTRIBUTE — the truth set `__has_attribute`
// would have to answer from.
[[nodiscard]] bool languageDeclaresAttribute(std::string_view name) {
    for (auto const& row : cSchema()->semantics().attributeEffects)
        for (auto const& n : row.names)
            if (n == name) return true;
    return false;
}

// A capability NO plausible future cycle implements, so the "answers 0" arms can
// never become accidentally-true. `__is_target_arch` is a clang target
// introspection builtin; `ptrauth_calls` is the arm64e pointer-authentication
// feature, and `src/dss-config/shippedLibs/malloc/malloc.json` records it as the
// ONE real macOS SDK site in this tree — `MALLOC_ZONE_FN_PTR` is gated on
// `__has_feature(ptrauth_calls)` and 0 is the RIGHT answer there, because DSS
// does not target arm64e.
constexpr std::string_view kAbsentBuiltin   = "__is_target_arch";
constexpr std::string_view kAbsentFeature   = "ptrauth_calls";
constexpr std::string_view kAbsentAttribute = "dss_no_such_attribute_exists";

// The shim every portable header ships, verbatim in SHAPE (the Apple SDK's
// `sys/cdefs.h` compatibility block, and the same three lines in glibc, musl,
// Boost and zlib). `#ifndef` is load-bearing: on a compiler that HAS the
// operator the block is DEAD, which is the whole point of the idiom.
[[nodiscard]] std::string shimFor(std::string_view op) {
    return "#ifndef " + std::string{op} + "\n#define " + std::string{op}
         + "(x) 0\n#endif\n";
}

// ── 1. VISIBILITY TRACKS IMPLEMENTATION, FOR EVERY NAME ON THE ROSTER ────────
//
// The property, not the current answer: `#ifdef NAME` is TRUE exactly when the
// language DECLARES an operator by that name. Both directions are asserted, so
// this reds on a name made visible without being implemented (the lie) AND on an
// operator declared but left invisible to `#ifdef` (which re-admits the F001A
// cascade TF-C86 fixed: the shim goes live and shadows the real operator).
//
// RED-ON-DISABLE (MEASURED — mutant M1 in this row's closing cell): delete the
// `|| isConditionalInclusionOperator(name, cfg())` disjunct from
// `MacroExpander::isDefined` and the three declared names go invisible here,
// with the other five arms staying green as the controls.
TEST(FeatureQueryOperators, NameIsIfdefVisibleExactlyWhenTheOperatorIsDeclared) {
    for (std::string_view op : kRoster) {
        PreprocessResult const r = pp(
            "#ifdef " + std::string{op} + "\nint seen_defined;\n"
            "#else\nint seen_undefined;\n#endif\n");
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << "`#ifdef " << op << "` must never be an error — an unknown name "
               "in `#ifdef` is simply false (C 6.10.1)";
        bool const declared = languageDeclaresOperator(op);
        EXPECT_EQ(sawLexeme(r, "seen_defined"), declared)
            << op
            << ": `#ifdef` must answer TRUE exactly when the language DECLARES "
               "the operator. ✔MEASURED on gcc 13.3.0, gcc 13.2.0 (mingw), "
               "clang 18.1.3 and cl 19.51.36252, each probed separately: every "
               "reference answers TRUE for precisely the operators it "
               "implements and FALSE for the rest. Answering TRUE for a name "
               "this implementation does not implement kills the portable "
               "`#ifndef` shim and leaves a function-like invocation of an "
               "undefined macro inside `#if`.";
        EXPECT_EQ(sawLexeme(r, "seen_undefined"), !declared) << op;
    }
}

// ── 2. THE SHIM IS NOW DEAD CODE, AND THAT IS THE WHOLE IDIOM WORKING ────────
//
// ★★★ THE TRIPWIRE, AND ITS POLARITY IS THE POINT. When this arm was written the
// four operators were absent, so the shim INSTALLED and answered 0. P59 shipped
// them, so `#ifndef X` is now FALSE, the shim's `#define` never executes, and
// the REAL operator answers — which is exactly what the `#ifndef` guard is for
// and exactly what gcc 13.3.0 / gcc 13.2.0 (mingw) / clang 18.1.3 do.
//
// The invariant the arm pins did NOT change: the shim must never SHADOW an
// operator the implementation provides. What changed is which mechanism
// delivers it — the guard, instead of the operator's absence. Both readings end
// at `took_fallback` for an argument nothing declares, so this arm was green
// before AND after; the assertions below are what make it non-vacuous, by
// pinning WHICH of the two produced the answer.
TEST(FeatureQueryOperators, ThePortableShimIsDeadCodeForTheExtensionFour) {
    for (std::string_view op : kExtensionFour) {
        ASSERT_NE(declaredFeatureQuery(op), nullptr)
            << op
            << " is no longer a declared feature-query operator. That is a real "
               "capability change, not a test-maintenance chore: this arm now "
               "asserts the shim is DEAD, which is only true of a compiler that "
               "HAS the operator.";
        // The shim's guard must be FALSE — measured by whether its `#define`
        // executed, not by inspecting config.
        PreprocessResult const guard = pp(
            "#ifndef " + std::string{op} + "\nint shim_installed;\n"
            "#else\nint shim_dead;\n#endif\n");
        EXPECT_TRUE(sawLexeme(guard, "shim_dead"))
            << op
            << ": `#ifndef` must be FALSE. A compiler that implements the "
               "operator but leaves the name invisible to `#ifdef` lets the "
               "world's most common portability shim SHADOW its own operator "
               "with a function-like macro answering 0 forever — the TF-C86 "
               "F001A cascade, one operator over.";

        PreprocessResult const r = pp(
            shimFor(op) + "#if " + std::string{op} + "(anything_at_all)\n"
            "int took_query;\n#else\nint took_fallback;\n#endif\n");
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << op
            << ": the `#ifndef X / #define X(x) 0 / #endif` shim is the idiom "
               "every portable header ships, and it must preprocess cleanly "
               "whether or not this implementation has the operator. "
               "✔MEASURED: gcc 13.3.0, gcc 13.2.0 (mingw), clang 18.1.3 and "
               "cl 19.51.36252 all accept this whole shape, rc 0.";
        EXPECT_FALSE(hasCode(
            r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable))
            << op
            << ": this name is NOT one C23 6.10.10p2 reserves — that clause "
               "names `defined`, `__has_c_attribute`, `__has_include` and "
               "`__has_embed`, and no others. The refusal code belongs to "
               "`defined` alone; see arm 4.";
        EXPECT_TRUE(sawLexeme(r, "took_fallback"))
            << op
            << ": `anything_at_all` is declared by nothing, so the REAL "
               "operator answers 0 and the fallback arm is taken — the same "
               "answer the shim used to give, now for the right reason.";
    }
}

// ── 3. AN UNIMPLEMENTED CAPABILITY ANSWERS 0. THE ROW'S STEP (D). ────────────
//
// ★★★ THIS IS THE ARM THE ROW CALLS "the only part of this row that is not
// optional". It is written against the CONFIG TRUTH SET, not against today's
// answer, so it does not expire when the operators ship: the assertion is
// "non-zero only if the capability is DECLARED", which is false today for a
// different reason (there is no operator) and must stay true afterwards for the
// right one.
//
// ⚠ ON VACUITY, because this arm is exactly the shape that can be vacuous and
// the project has paid for that mistake. TODAY the 0 comes from the SHIM, not
// from an operator — so the arm's first job is to assert, out loud, that the
// capability really is undeclared; if a cycle ever declares `__is_target_arch`
// as a builtin the guard is telling the truth about a different world, and the
// `ASSERT_FALSE` below fires FIRST and stops the arm — naming the change rather
// than letting the query arms quietly re-base on it. It is an ASSERT rather
// than an EXPECT precisely so the rest of the arm cannot report on a premise
// that has already been contradicted.
TEST(FeatureQueryOperators, AnUndeclaredCapabilityAnswersZeroNotOne) {
    // The premise, asserted rather than assumed.
    ASSERT_FALSE(languageDeclaresBuiltin(kAbsentBuiltin))
        << kAbsentBuiltin
        << " is declared in semantics.builtinFunctions now. That is a real "
           "capability change, not a test-maintenance chore: pick a builtin "
           "this implementation still does not have, and re-read this row's "
           "step (B) before making any __has_builtin answer 1.";
    ASSERT_FALSE(languageDeclaresAttribute(kAbsentAttribute))
        << kAbsentAttribute << " unexpectedly appears in semantics.attributeEffects";

    struct Arm {
        std::string_view op;
        std::string_view arg;
        char const*      why;
    };
    const Arm arms[] = {
        {"__has_builtin", kAbsentBuiltin,
         "a clang target-introspection builtin DSS does not have. Answering 1 "
         "routes TargetConditionals.h onto its __is_target_arch detection block "
         "— a path DSS cannot honour — instead of the legacy __GNUC__ ladder "
         "that WORKS. ✔MEASURED: gcc 13.3.0, gcc 13.2.0 (mingw) and cl "
         "19.51.36252 all answer 0 here and all take the legacy ladder."},
        {"__has_feature", kAbsentFeature,
         "arm64e pointer authentication. src/dss-config/shippedLibs/malloc/"
         "malloc.json records this as the one real Apple-SDK site in this tree: "
         "MALLOC_ZONE_FN_PTR expands to the bare member name unless this answers "
         "1, and the arm64e expansion adds a __ptrauth qualifier. DSS does not "
         "target arm64e, so 0 is the RIGHT answer and 1 would be a false ABI."},
        {"__has_attribute", kAbsentAttribute,
         "an attribute no language declares. An operator that answers 1 for an "
         "attribute the engine does not honour makes the header emit it on real "
         "declarations, which is a claim the engine does not back — the "
         "D-CONFIG-COMMENT-CLAIM-ROT shape, one tier down."},
        {"__has_extension", "dss_no_such_extension_exists",
         "a clang extension name nothing in this tree implements."},
    };
    for (Arm const& a : arms) {
        PreprocessResult const r = pp(
            shimFor(a.op) + "#if " + std::string{a.op} + "(" + std::string{a.arg}
            + ")\nint answered_nonzero;\n#else\nint answered_zero;\n#endif\n");
        EXPECT_FALSE(r.diagnostics->hasErrors()) << a.op << "(" << a.arg << ")";
        EXPECT_TRUE(sawLexeme(r, "answered_zero"))
            << a.op << "(" << a.arg << ") ANSWERED NON-ZERO. " << a.why
            << "\n★ An operator that answers 1 for something this "
               "implementation does not implement is STRICTLY WORSE than not "
               "having the operator: it converts a working compatibility "
               "fallback into a silent miscompile. If you are implementing "
               "this operator, its answer must be DERIVED from the declared "
               "capability set (semantics.builtinFunctions / "
               "semantics.attributeEffects), never from a second hand-kept "
               "list — and this arm is what proves it was.";
        EXPECT_FALSE(sawLexeme(r, "answered_nonzero")) << a.op;
    }
}

// ── 4. NO NAME ON THE ROSTER IS REFUSED, AND A DIAGNOSED ONE IS APPLIED ──────
//
// ★★★ REVERSED BY OPERATOR RULING 2026-09-03, and the reversal is the row. This
// arm used to assert that the C23 6.10.10p2 trio was REFUSED at Error. The
// ruling: *"we must accept too. best long term solution, no workaround, first
// class implementation, 100% config driven. leave nothing to be done."*
//
// ✔MEASURED 2026-09-04, each reference invoked SEPARATELY, WITH A CONTROL that
// proves the untouched operator answers 1 for the same local header (without it
// a 0 is equally consistent with the header simply not being found — which is
// exactly what MSVC's answer looked like until the control was run):
//
//   shape                          gcc 13.3.0  gcc 13.2.0  clang 18.1.3  cl 19.51
//   `#define __has_include(x) 0`   warn, rc 0  warn, rc 0  warn, rc 0   silent, rc 0
//     ...and APPLIED                  yes         yes         yes          yes
//   `#undef __has_include`         warn, rc 0  warn, rc 0  warn, rc 0   silent, rc 0
//     ...and APPLIED                  yes         yes         yes          NO
//
// So ACCEPTANCE is unanimous and the disjunction forbids refusing; on the
// `#undef`'s MEANING the split is 3-1 and DSS follows the three, because MSVC's
// alternative silently discards code the author wrote.
//
// ★★ TWO PROPERTIES, TWO PREDICATES, AND KEEPING THEM APART IS THE FIX. The
// C23-RESERVED set (`languageReservesOperator`) still decides whether a
// DIAGNOSTIC is owed; the PROVIDED set (`languageDeclaresOperator`) decides
// `#ifdef` visibility. Neither decides refusal any more — that is config data,
// and the only name it still refuses is `defined`, which is not on this roster.
TEST(FeatureQueryOperators, NoRosterNameIsRefusedAndADiagnosedOneIsApplied) {
    for (std::string_view op : kRoster) {
        bool const reserved = languageReservesOperator(op);
        bool const provided = languageDeclaresOperator(op);
        {
            // An OBJECT-like definition, so the definition's EFFECT is
            // observable as a lexeme. A function-like `(x) 0` would be applied
            // just as truly but would leave nothing to read.
            PreprocessResult const r =
                pp("#define " + std::string{op} + " 5\nint v = "
                   + std::string{op} + ";\n");
            EXPECT_FALSE(hasCode(
                r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable))
                << "#define " << op
                << ": NOTHING on this roster is refused any more. All four "
                   "references accept it, and the refusal code now has exactly "
                   "one client — `defined` — which is the one its name is true "
                   "about (C23 6.10.2 makes `defined` an operator inside `#if`, "
                   "so admitting it as a macro name makes conditional inclusion "
                   "unparseable).";
            EXPECT_FALSE(r.diagnostics->hasErrors()) << "#define " << op;
            EXPECT_EQ(codeSeverity(r,
                                   DiagnosticCode::P_PreprocessorPredefinedMacro)
                              .has_value(),
                      reserved || provided)
                << "#define " << op
                << ": accepted, but NEVER in silence for a name this "
                   "implementation OWNS. A name it does not own is an ordinary "
                   "macro and draws nothing, which is what every reference does.";
            auto const lexs = lexemesOf(r);
            ASSERT_EQ(lexs.size(), 5u) << op << " — expected: int v = 5 ;";
            EXPECT_EQ(lexs[3], "5")
                << op
                << ": the definition must be APPLIED. Accepting and IGNORING is "
                   "a silent wrong answer — it is what cl does to the `#undef` "
                   "half, and it is the one failure class this project refuses "
                   "outright.";
        }
        {
            PreprocessResult const r =
                pp("#undef " + std::string{op} + "\n"
                   "#ifdef " + std::string{op} + "\nint still_here;\n"
                   "#else\nint gone;\n#endif\n");
            EXPECT_FALSE(hasCode(
                r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable))
                << "#undef " << op << ": same ruling, same set.";
            EXPECT_FALSE(r.diagnostics->hasErrors()) << "#undef " << op;
            EXPECT_TRUE(sawLexeme(r, "gone"))
                << op
                << ": the `#undef` must TAKE EFFECT. An operator lives in the "
                   "CONFIG rather than the macro table, so nothing the ordinary "
                   "`#undef` path does can reach it — this assertion is what "
                   "proves the engine records the revocation instead of warning "
                   "and changing nothing.";
        }
    }
}

// ── 4b. THE ONE NAME STILL REFUSED, AND ITS SEVERITY IS CONFIG DATA ──────────
//
// The CONTROL half of arm 4: if no name were refused at all, "nothing on the
// roster is refused" would pass for a reason that has nothing to do with this
// row. `defined` is deliberately NOT on the roster (it is an operator SPELLING,
// not a macro name — ✔MEASURED, `#ifdef defined` is not taken on any of the
// four references), so it is the honest control.
//
// ⚠ AND THIS REFUSAL IS NEW, NOT PRESERVED. ✔MEASURED at this row's base commit
// b1f31420 through the shipped CLI: `#define defined 1` compiled rc 0 in
// SILENCE, while gcc 13.3.0, gcc 13.2.0 (mingw) and clang 18.1.3 all make it a
// hard ERROR (rc 1). `isConditionalInclusionOperator` deliberately excluded
// `definedOperator` and no other arm covered it — while `preprocess_config.hpp`
// asserted in a comment that "that refusal lives in the conditional-inclusion-
// operator guard". A comment claiming a guard that did not exist.
TEST(FeatureQueryOperators, TheDefinedOperatorIsTheOnlyNameStillRefused) {
    std::string const& definedKw = cSchema()->preprocess().definedOperator;
    ASSERT_FALSE(definedKw.empty());
    ASSERT_FALSE(languageDeclaresOperator(definedKw))
        << "`defined` must NOT be `#ifdef`-visible — it is an operator "
           "spelling, not a macro name, and all four references agree";
    for (std::string const& src : {"#define " + definedKw + " 1\nint v;\n",
                                   "#undef " + definedKw + "\nint v;\n"}) {
        PreprocessResult const r = pp(src);
        EXPECT_EQ(codeSeverity(
                      r, DiagnosticCode::P_PreprocessorOperatorNameNotDefinable),
                  std::optional<DiagnosticSeverity>{DiagnosticSeverity::Error})
            << "3 of 4 references make this a hard error, and the fourth "
               "accepts-then-ignores it (cl's C4117), which is the silent-drop "
               "failure class that does not get to be the model:\n"
            << src;
    }
}

// ── 5. AN UNRECOGNISED QUERY FAILS LOUD, IT DOES NOT ANSWER 0 IN SILENCE ─────
//
// ⚠ THIS ARM CORRECTS THE ROW'S OWN STEP (C), which says "the honest default is
// 0" and that implementing the operator while answering 0 would be
// "INDISTINGUISHABLE in behaviour from today's shadow". ✔MEASURED, it is not:
// today's 0 comes from the SHIM. UNSHIMMED, DSS refuses the directive loudly and
// the two are very distinguishable — the loud one is the fail-loud posture the
// bar requires, because an unrecognised query silently answering 0 is a CLAIM
// ("I checked, and no") where an error is an admission ("I do not know this
// operator"). The row's (C) and this arm cannot both be shipped; this is the
// measurement, and it wins.
//
// ⓘ WHAT THE REFERENCES DO HERE, stated because it is NOT unanimous and this
// arm is deliberately not trying to settle it: gcc 13.3.0, gcc 13.2.0 (mingw)
// and clang 18.1.3 all REFUSE an unknown function-like query in `#if` (and
// refuse it even in an operand `||` short-circuits away); cl 19.51.36252 accepts
// it under BOTH its preprocessors, folding the name to 0 and DROPPING the
// trailing tokens with C4067. That 3-1 accept-vs-refuse split is the subject of
// [[D-PP-IF-OPERAND-PARSE-NO-SHORTCIRCUIT]], not of this row, and this arm pins
// only that DSS does not go SILENT.
// ★★★ REVERSED BY THE 2026-09-03 RULING: the four ARE implemented now, so an
// UNSHIMMED query must ANSWER rather than refuse. What the arm still forbids is
// SILENCE OF THE OTHER KIND — an operator this implementation does NOT have,
// invoked unshimmed, must still fail loud. Both halves are asserted here,
// because "it answers" and "an unknown operator still refuses" are two claims
// and a cycle can ship one while breaking the other.
TEST(FeatureQueryOperators, AnUnshimmedQueryAnswersAndAnUnknownOperatorStillFailsLoud) {
    // (a) THE FOUR ANSWER, UNSHIMMED, FROM THE DECLARED TRUTH SETS.
    struct Arm { std::string_view op, arg; bool want; char const* why; };
    const Arm arms[] = {
        {"__has_attribute", "deprecated", true,
         "`deprecated` is declared in semantics.attributeSemantics.effects AND "
         "in preprocess.knownCAttributes. ✔MEASURED: gcc 13.3.0, gcc 13.2.0 "
         "(mingw) and clang 18.1.3 all answer 1."},
        {"__has_attribute", kAbsentAttribute, false,
         "the CONTROL: an attribute nothing declares. All three implementing "
         "references answer 0, silently."},
        {"__has_builtin", "__builtin_popcount", true,
         "declared in semantics.builtinFunctions — the truth set the answer is "
         "READ FROM rather than a second copy of."},
        {"__has_builtin", "__builtin_offsetof", true,
         "declared as a grammar KEYWORD rather than a builtinFunctions row "
         "(it is an operator wearing a call's punctuation), and reachable "
         "through preprocess.builtinQueryKeywordTokens. ✔MEASURED: all three "
         "implementing references answer 1, so a 0 here would be a WRONG answer "
         "about a builtin DSS demonstrably has, not a conservative one."},
        {"__has_builtin", kAbsentBuiltin, false,
         "the CONTROL: a clang target-introspection builtin DSS does not have."},
        {"__has_feature", "c_static_assert", true,
         "declared in preprocess.languageFeatures, and the declaration is "
         "backed by a compiled-and-run witness. ✔MEASURED: clang 18.1.3 answers "
         "1 in c17/c2x; gcc has no such operator at all."},
        {"__has_feature", kAbsentFeature, false,
         "the CONTROL: arm64e pointer authentication. "
         "src/dss-config/shippedLibs/malloc/malloc.json is the one real "
         "Apple-SDK site in this tree and 0 is the RIGHT answer there — 1 would "
         "be a false ABI."},
        {"__has_extension", "c_static_assert", true,
         "__has_extension is a strict SUPERSET of __has_feature. ✔MEASURED on "
         "clang 18.1.3, the only reference that implements the pair: in c89 "
         "__has_feature answers 0 for all six C features while __has_extension "
         "still answers 1."},
        {"__has_extension", "dss_no_such_extension_exists", false,
         "the CONTROL: a name nothing declares."},
    };
    for (Arm const& a : arms) {
        PreprocessResult const r = pp(
            "#if " + std::string{a.op} + "(" + std::string{a.arg} + ")\n"
            "int answered_nonzero;\n#else\nint answered_zero;\n#endif\n");
        EXPECT_FALSE(r.diagnostics->hasErrors())
            << a.op << "(" << a.arg
            << ") UNSHIMMED must be answerable — the operator is implemented, "
               "and an unknown ARGUMENT is a 0, never an error, on every "
               "reference that has the operator.";
        EXPECT_EQ(sawLexeme(r, "answered_nonzero"), a.want)
            << a.op << "(" << a.arg << "): " << a.why;
        EXPECT_EQ(sawLexeme(r, "answered_zero"), !a.want) << a.op;
    }

    // (b) AN OPERATOR THIS IMPLEMENTATION DOES NOT HAVE STILL FAILS LOUD.
    // ⚠ 0 is an ANSWER. Answering it for an operator DSS does not recognise is
    // the same lie as answering 1 for a capability it does not have: it is a
    // CLAIM ("I checked, and no") where an error is an admission ("I do not
    // know this operator"). ✔MEASURED: gcc 13.3.0, gcc 13.2.0 (mingw) and clang
    // 18.1.3 all REFUSE an unknown function-like query in `#if`; cl 19.51
    // accepts it, folding to 0 and DROPPING the trailing tokens with C4067.
    // That 3-1 accept-vs-refuse split is
    // [[D-PP-IF-OPERAND-PARSE-NO-SHORTCIRCUIT]]'s subject, not this row's, and
    // this half pins only that DSS does not go SILENT.
    for (std::string_view op :
         {"__has_warning", "__is_identifier", "__has_declspec_attribute"}) {
        ASSERT_EQ(declaredFeatureQuery(op), nullptr)
            << op << " became a declared operator; pick one that is still absent";
        PreprocessResult const r = pp(
            "#if " + std::string{op} + "(cold)\nint took_query;\n"
            "#else\nint took_fallback;\n#endif\n");
        EXPECT_TRUE(r.diagnostics->hasErrors())
            << op
            << " UNSHIMMED went quiet. Whatever a later cycle does here, it "
               "must not be an unannounced 0.";
        EXPECT_EQ(codeSeverity(r, DiagnosticCode::P_PreprocessorDirective),
                  std::optional<DiagnosticSeverity>{DiagnosticSeverity::Error})
            << op << ": the refusal is the `#if` operand diagnostic, at Error.";
    }
}

// ── 5b. THE ANSWER SET IS THE CONFIG'S, NOT THE ENGINE'S ────────────────────
//
// ★★★ THE ARM THE ROW'S "100% CONFIG DRIVEN" CLAUSE RESTS ON, and it is
// REMOVE-direction on purpose ([[feedback-a-fixture-must-synthesize-the-negative]]):
// the mutant takes a capability AWAY from a config that has it. An
// ADD-direction fixture — declaring something the shipped config lacks — stays
// green precisely when the real config loses the feature.
//
// RED-ON-DISABLE by construction: if `__has_builtin`'s answer came from a list
// in `src/` rather than from `semantics.builtinFunctions`, deleting the row
// would not move the answer and this reds.
TEST(FeatureQueryOperators, TheBuiltinAnswerFollowsTheDeclaredTruthSet) {
    // The premise, asserted rather than assumed.
    ASSERT_TRUE(languageDeclaresBuiltin("__builtin_popcount"))
        << "the shipped c must declare this builtin; without it the mutant "
           "below removes nothing and this arm is vacuous";

    std::string text = shippedCText();
    ASSERT_FALSE(text.empty());
    constexpr std::string_view kRow =
        "{ \"name\": \"__builtin_popcount\",";
    auto const pos = text.find(kRow);
    ASSERT_NE(pos, std::string::npos)
        << "the shipped c config no longer carries a `__builtin_popcount` row "
           "in that spelling; re-point this mutant rather than deleting it";
    // Rename the ROW's name so the builtin is genuinely absent from the truth
    // set while the document stays well formed.
    text.replace(pos, kRow.size(),
                 "{ \"name\": \"__builtin_popcount_removed_by_test\",");

    auto loaded = GrammarSchema::loadFromText(text, "<no-popcount-c>");
    ASSERT_TRUE(loaded.has_value()) << "the mutated config must still LOAD — a "
                                       "load failure would make this arm pass "
                                       "for the wrong reason";
    std::shared_ptr<GrammarSchema const> mutant = *loaded;

    std::vector<fs::path> const noDirs;
    auto runOn = [&](std::shared_ptr<GrammarSchema const> const& s) {
        auto buf = SourceBuffer::fromString(
            std::string{"#if __has_builtin(__builtin_popcount)\n"
                        "int yes;\n#else\nint no;\n#endif\n"},
            "fq_cfg.c");
        return preprocess(buf, s, noDirs, kDefaultHeaderNameMatching,
                          DiagnosticBudget::libraryDefault());
    };

    PreprocessResult const control = runOn(cSchema());
    EXPECT_TRUE(sawLexeme(control, "yes"))
        << "CONTROL: the shipped config declares the builtin, so the operator "
           "must answer 1. A control that does not stay green makes the mutant "
           "arm below prove nothing.";

    PreprocessResult const r = runOn(mutant);
    EXPECT_FALSE(r.diagnostics->hasErrors());
    EXPECT_TRUE(sawLexeme(r, "no"))
        << "with the builtin REMOVED from semantics.builtinFunctions the answer "
           "must follow it to 0. If this says `yes`, the operator is answering "
           "from a second list — which is the duplicated-truth-set defect the "
           "ruling names explicitly, and it goes out of sync the first time a "
           "builtin is added.";
}

// ── 6. THE SHIM SURVIVES THE INCLUDE PRE-SCAN ORACLE ────────────────────────
//
// [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]] made the include pre-scan host the
// authoritative `MacroExpander` as an ORACLE, and one residual is known there:
// `__COUNTER__` in a guard is refused loudly because the two expanders advance
// SEPARATE counters. The question this arm answers is whether a feature-query
// shim has any comparable per-read state. ✔MEASURED: it does not — a shim is an
// ordinary function-like object with no state at all — and the pre-scan resolves
// the gated include correctly. Pinned rather than reasoned, because "no state"
// is a claim about an implementation that can change.
struct CwdFixture {
    test_support::ScratchDir dir{test_support::Location::InsideRepo,
                                 "feature-query-operator-family"};
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
};

TEST(FeatureQueryOperators, TheShimSurvivesTheIncludePreScanOracle) {
    CwdFixture fx;
    fx.write("fq_present.h", "#define FQ_PRESENT_ANSWER 42\n");

    // The shape `sys/cdefs.h` + a guarded `#include` really form: the shim
    // installs, the query answers 0, and the `#else` arm's header must SPLICE.
    PreprocessResult const r = pp(
        shimFor("__has_attribute")
        + "#if __has_attribute(" + std::string{kAbsentAttribute} + ")\n"
          "#include \"fq_definitely_absent_header.h\"\n"
          "#else\n"
          "#include \"fq_present.h\"\n"
          "#endif\n"
          "int v = FQ_PRESENT_ANSWER;\n");

    EXPECT_FALSE(r.diagnostics->hasErrors())
        << "a shimmed feature query gating an #include must resolve in the "
           "pre-scan exactly as it does in the authoritative pass";
    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "the pre-scan must not go conservative-uncertain on a shim macro";

    // ★ IT IS THE EXPANSION, NOT THE ABSENCE OF AN ERROR, THAT IS THE WITNESS.
    // A pre-scan that "resolved" the include by DROPPING the directive raises
    // nothing and leaves `int v = FQ_PRESENT_ANSWER ;` — the silent drop. The
    // VALUE is what tells the two apart.
    auto const lexs = lexemesOf(r);
    ASSERT_EQ(lexs.size(), 5u) << "expected `int v = 42 ;`";
    EXPECT_EQ(lexs[3], "42")
        << "the #else arm's header must have been spliced AND its macro "
           "expanded; a dropped directive would leave the macro NAME here";

    // The DEAD arm's missing header must never be looked for (C 6.10p1).
    EXPECT_FALSE(hasCode(r, DiagnosticCode::P_PreprocessorIncludeError))
        << "the TRUE arm names a header that does not exist; a pre-scan that "
           "resolved it anyway would be eager rather than correct";
}

} // namespace
