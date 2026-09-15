// D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE — per-site pins.
//
// THE DEFECT THESE GUARD. `--max-diagnostics=N` configured the run-wide
// reporter and nothing else: ten front-end construction sites built their own
// reporters from `DiagnosticReporter::Config`'s in-class initializers and
// capped at 1000/50 regardless. ✔MEASURED at the CLI before the fix — a TU
// whose semantic tier stored 1400 diagnostics printed `reporter cap of 1000
// diagnostics reached; 60 further diagnostics were dropped` at the default, at
// `--max-diagnostics=1200`, AND at `--max-diagnostics=100000`, byte-identical
// every time.
//
// ★★ WHY EVERY BEHAVIOURAL PIN BELOW RAISES A CAP AND NEVER LOWERS ONE.
// Threading is observable ONLY in the RAISE direction, and this is measured,
// not stylistic. Lower the budget and the destination reporter enforces the
// same smaller number a moment later, so the drained output is IDENTICAL
// whether or not the tier honoured it — a lowering pin is vacuous by
// construction. Raise it above the library default and the two diverge: a
// threaded tier keeps everything, an un-threaded one still stops at 50 (per
// code) or 1000 (globally). Each pin therefore sets a budget ABOVE the library
// default and asserts the tier kept more than the library default would allow.
//
// ★★ AND WHY `maxPerCode` IS THE AXIS MOST PINS USE. `report()` checks
// `maxPerCode` (50) BEFORE `maxDiagnostics` (1000), so an ordinary tier is
// bounded at (distinct codes x 50) and cannot reach 1000 at all — ✔MEASURED:
// 1400 illegal characters reach stderr as 100 diagnostics, 1400 malformed
// declarations as 53, 1400 `#warning` directives as 50. The global cap is
// reachable at a sub-tier only through `Guaranteed` / unsuppressable traffic,
// which is exactly what the semantic pin uses. Pinning `maxDiagnostics` alone
// would leave the axis that actually bites at every other tier unguarded.
//
// ★ ONE LEVER PER SITE. Reverting any single construction site must red a pin
// that NAMES that site — a single pin over all ten is the multi-site contract
// trap (dss-cycle §A.5). Sites that are SERIALLY COMPOSED (a tokenizer whose
// only observable is the reporter it drains into) cannot be separated
// behaviourally; those are named individually by
// `SourceEnumeration.EveryReporterInAThreadedFileIsBudgetDerived` at the
// bottom of this file, which reads the shipped source and fails with the file
// and line of the offending construction.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/source_buffer.hpp"
#include "repo_root.hpp"  // the ONE repo-root resolver; see the note below
#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;

namespace {

// A budget deliberately ABOVE the library default on every axis, so "the tier
// honoured it" and "the tier fell back" produce different observable output.
// Built from a `Config`, never from typed numbers pulled out of the air: the
// numbers below are TEST INPUTS, not a second spelling of the shipped default.
[[nodiscard]] DiagnosticBudget raisedBudget() {
    DiagnosticReporter::Config cfg{};
    cfg.maxDiagnostics = 9001;   // > the library's 1000
    cfg.maxPerCode     = 400;    // > the library's 50
    cfg.dedupWindow    = 0;      // so repeated identical diagnostics all land
    return DiagnosticBudget{cfg};
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> shipped(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"" << name << "\") failed";
        std::abort();
    }
    return *loaded;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode code) {
    auto all = r.all();
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(),
                      [code](ParseDiagnostic const& d) { return d.code == code; }));
}

// `n` lines each carrying one illegal character — a tokenizer diagnostic
// (`P_IllegalChar`) per line, all sharing ONE code, so the per-code cap is the
// only thing that can bound them.
[[nodiscard]] std::string illegalCharSource(std::size_t n) {
    std::ostringstream os;
    os << "int main(void) { return 0; }\n";
    for (std::size_t i = 0; i < n; ++i) os << "int v" << i << " = 1 ` 2;\n";
    return os.str();
}

constexpr std::size_t kIllegalCharLines = 120;  // > the library's maxPerCode of 50

}  // namespace

// ── SITE 1: UnitBuilder(schema, budget) → driverDiagnostics_ ────────────────
TEST(DiagnosticBudgetThreading, UnitBuilderSingleSchemaCtorGivesDriverReporterTheBudget) {
    UnitBuilder b{shipped("toy"), raisedBudget()};
    auto cu = std::move(b).finish();
    auto const& cfg = cu.driverDiagnostics().config();
    EXPECT_EQ(cfg.maxDiagnostics, 9001u);
    EXPECT_EQ(cfg.maxPerCode, 400u);
    // The driver reporter states its OWN dedup choice on top of the budget:
    // D_FileNotFound for two different files hashes alike, so a window would
    // eat the second one. `withoutDedup()` must survive the threading.
    EXPECT_EQ(cfg.dedupWindow, 0u);
}

// ── SITE 2: UnitBuilder(vector<schema>, budget) → driverDiagnostics_ ────────
TEST(DiagnosticBudgetThreading, UnitBuilderMultiSchemaCtorGivesDriverReporterTheBudget) {
    std::vector<std::shared_ptr<GrammarSchema const>> schemas{shipped("toy"),
                                                              shipped("c")};
    UnitBuilder b{schemas, raisedBudget()};
    auto cu = std::move(b).finish();
    auto const& cfg = cu.driverDiagnostics().config();
    EXPECT_EQ(cfg.maxDiagnostics, 9001u);
    EXPECT_EQ(cfg.maxPerCode, 400u);
    EXPECT_EQ(cfg.dedupWindow, 0u);
}

// ── SITE 3: parser.cpp `make_unique<TreeBuilder>(I.src, I.schema, I.budget)` ─
// Driven through `Parser`'s REAL construction path, not by building a
// TreeBuilder directly: reverting `I.budget` at the TreeBuilder site is what
// this must catch, and only a Parser-driven build exercises that line.
TEST(DiagnosticBudgetThreading, ParserHandsTheBudgetToTheTreeBuilderItConstructs) {
    auto schema = shipped("toy");
    auto src    = SourceBuffer::fromString("let x = 1;\n", "<inline>");
    Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream), raisedBudget()};
    Tree t = std::move(p).parse().tree;
    auto const& cfg = t.diagnostics().config();
    EXPECT_EQ(cfg.maxDiagnostics, 9001u);
    EXPECT_EQ(cfg.maxPerCode, 400u);
    EXPECT_EQ(cfg.dedupWindow, 0u);
}

// ── SITE 4: compilation_unit.cpp `Parser p{..., budget_, ...}` ──────────────
// Distinct lever from SITE 3: this one reds when the CU stops FORWARDING its
// budget to the parser, even though the parser→TreeBuilder hop is intact.
TEST(DiagnosticBudgetThreading, UnitBuilderForwardsTheBudgetToTheParserItDrives) {
    UnitBuilder b{shipped("toy"), raisedBudget()};
    b.addInMemory("let x = 1;\n", "<mem>");
    auto cu = std::move(b).finish();
    ASSERT_EQ(cu.trees().size(), 1u);
    auto const& cfg = cu.trees()[0].diagnostics().config();
    EXPECT_EQ(cfg.maxDiagnostics, 9001u);
    EXPECT_EQ(cfg.maxPerCode, 400u);
}

// ── SITE 5: compilation_unit.cpp `Tokenizer tk{src, schema, budget_}` ───────
// The non-preprocess parse path (a language whose schema declares no
// preprocess block). BEHAVIOURAL, not a config read: the tokenizer's own
// reporter is drained into the tree and destroyed, so the only witness is how
// many diagnostics survived. 120 identical-code lexer diagnostics — an
// un-threaded tokenizer stops at the library's 50.
TEST(DiagnosticBudgetThreading, UnitBuilderNonPreprocessTokenizerHonoursTheRaisedPerCodeCap) {
    UnitBuilder b{shipped("toy"), raisedBudget()};
    b.addInMemory(illegalCharSource(kIllegalCharLines), "<mem>");
    auto cu = std::move(b).finish();
    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_EQ(countCode(cu.trees()[0].diagnostics(), DiagnosticCode::P_IllegalChar),
              kIllegalCharLines)
        << "the tokenizer stopped short of the configured per-code budget — it "
           "is building its reporter from the library defaults, not from the "
           "budget UnitBuilder handed it";
}

// ── SITE 6: preprocessor.cpp `preprocessRun` → result.diagnostics ──────────
TEST(DiagnosticBudgetThreading, PreprocessResultDiagnosticsCarriesTheBudget) {
    auto schema = shipped("c");
    auto src    = SourceBuffer::fromString("int main(void){return 0;}\n", "<inline>");
    PreprocessResult pp = preprocess(src, schema, {}, kDefaultHeaderNameMatching,
                                     raisedBudget());
    ASSERT_NE(pp.diagnostics, nullptr);
    auto const& cfg = pp.diagnostics->config();
    EXPECT_EQ(cfg.maxDiagnostics, 9001u);
    EXPECT_EQ(cfg.maxPerCode, 400u);
}

// ── SITES 7+8: `provisionalTokDiags` and `tokenizeToPP`'s Tokenizer ────────
// These two are serially composed — the tokenizer drains into
// `provisionalTokDiags`, which forwards into `result.diagnostics` — so no
// observation can tell which of them capped. This pin proves the CHAIN honours
// the budget; `SourceEnumeration` below names each of the two sites
// individually.
TEST(DiagnosticBudgetThreading, PreprocessTokenizeChainHonoursTheRaisedPerCodeCap) {
    auto schema = shipped("c");
    auto src    = SourceBuffer::fromString(illegalCharSource(kIllegalCharLines),
                                           "<inline>");
    PreprocessResult pp = preprocess(src, schema, {}, kDefaultHeaderNameMatching,
                                     raisedBudget());
    ASSERT_NE(pp.diagnostics, nullptr);
    EXPECT_EQ(countCode(*pp.diagnostics, DiagnosticCode::P_IllegalChar),
              kIllegalCharLines)
        << "the preprocessor's tokenize chain stopped short of the configured "
           "per-code budget — the synth-buffer Tokenizer or provisionalTokDiags "
           "is still built from the library defaults";
}

// ── SITE 9: semantic_analyzer.cpp `EngineState(cu, budget)` ────────────────
// The tier MEASURED producing `reporter cap of 1000` under
// `--max-diagnostics=100000`.
TEST(DiagnosticBudgetThreading, AnalyzeGivesTheSemanticModelTheBudget) {
    UnitBuilder b{shipped("c"), DiagnosticBudget::libraryDefault()};
    b.addInMemory("int main(void){return 0;}\n", "<mem>");
    auto cu = std::make_shared<CompilationUnit const>(std::move(b).finish());
    SemanticModel model = analyze(cu, raisedBudget());
    auto const& cfg = model.diagnostics().config();
    EXPECT_EQ(cfg.maxDiagnostics, 9001u);
    EXPECT_EQ(cfg.maxPerCode, 400u);
}

// ── The default keeps exactly ONE spelling ─────────────────────────────────
// `libraryDefault()` must BE `DiagnosticReporter::Config{}`, never a second
// transcription of 1000/50/4. If someone moves the shipped default and types
// the old numbers here, this reds.
TEST(DiagnosticBudgetThreading, LibraryDefaultIsExactlyTheShippedConfigDefault) {
    DiagnosticReporter::Config const shippedDefault{};
    DiagnosticBudget const b = DiagnosticBudget::libraryDefault();
    EXPECT_EQ(b.maxDiagnostics(), shippedDefault.maxDiagnostics);
    EXPECT_EQ(b.maxPerCode(), shippedDefault.maxPerCode);
    EXPECT_EQ(b.dedupWindow(), shippedDefault.dedupWindow);
    EXPECT_EQ(DiagnosticBudget{shippedDefault}, b);
}

// A budget must not smuggle policy into a tier: policy is applied at the DRAIN,
// and applying `warningsAsErrors` a second time at a tier would flip that
// tier's own `hasErrors()` and change control flow, not merely volume.
TEST(DiagnosticBudgetThreading, BudgetDoesNotCarryPolicyIntoATier) {
    DiagnosticReporter::Config cfg{};
    cfg.policy.warningsAsErrors = true;
    cfg.policy.suppress.insert(DiagnosticCode::P_IllegalChar);
    cfg.policy.overrides[DiagnosticCode::P_IllegalChar] = DiagnosticSeverity::Info;
    DiagnosticReporter::Config const tier = DiagnosticBudget{cfg}.asConfig();
    EXPECT_FALSE(tier.policy.warningsAsErrors);
    EXPECT_TRUE(tier.policy.suppress.empty());
    EXPECT_TRUE(tier.policy.overrides.empty());
}

// ─────────────────────────────────────────────────────────────────────────
// SOURCE ENUMERATION — the guard that names an individual site.
// ─────────────────────────────────────────────────────────────────────────
//
// The behavioural pins above prove the threading WORKS. This one proves no
// site was MISSED, which is the failure this whole row exists to record: six
// sites were named in the original report and the real count is ten, and every
// one of the four that were missed was missed by reading rather than by
// enumerating.
//
// The rule is INVERTED on purpose, exactly like `check-anchor-balance.py`:
// a construction is a VIOLATION unless it is visibly budget-derived, so a
// shape nobody has thought of yet counts as a violation, which is the safe
// direction. Deliberately-unbudgeted throwaways are allowlisted BY THEIR EXACT
// SOURCE LINE and BY COUNT, so a new one either has new text (fails at once)
// or bumps an allowlisted count (fails too).
namespace {

// ⚠ THERE WAS A PRIVATE `repoRoot()` HERE AND IT WAS THE LAST ONE IN THE TREE.
// D-TEST-BUDGET-THREADING-PRIVATE-REPO-ROOT-WALK-FAILS-OUT-OF-SOURCE. It walked
// up from `fs::current_path()` ONLY — never consulting `DSS_CONFIG_ROOT` nor the
// CMake-baked `DSS_TEST_REPO_ROOT` — so the SAME binary passed with a cwd inside
// the repo and FAILED from an out-of-repo build directory, where the walk runs
// out of parents and returns `{}`. ✔MEASURED both ways on one binary.
// ★ It was the SOLE SURVIVOR of the `test_support/repo_root.hpp` consolidation:
// the only file in `tests/` that still defined its own `repoRoot()` and included
// none of the shared one — and it failed in bit-for-bit the way that header's own
// docblock exists to prevent. ⇒ A consolidation is not finished when the shared
// thing exists; it is finished when nothing private survives, and only a scan can
// tell you which of those you have.
// The shared resolver tries $DSS_CONFIG_ROOT, then the baked root, then the walk —
// so it is correct from ANY working directory — and THROWS rather than returning
// empty, which GoogleTest reports as a failure of this one test instead of
// letting an empty path flow onward.

[[nodiscard]] std::string trimmed(std::string s) {
    auto const b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    auto const e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

struct AllowedThrowaway {
    std::string_view line;    // exact trimmed source line
    std::size_t      count;   // exactly this many occurrences, no more
    std::string_view why;
};

}  // namespace

TEST(DiagnosticBudgetThreadingSourceEnumeration,
     EveryReporterInAThreadedFileIsBudgetDerived) {
    fs::path const root = dss::test::repoRoot();
    ASSERT_FALSE(root.empty());

    // Every file that the operator's budget flows through. A `DiagnosticReporter`
    // born in one of these must come from a `DiagnosticBudget`.
    struct Subject {
        std::string_view              path;
        std::vector<AllowedThrowaway> allowed;
    };
    std::vector<Subject> const subjects = {
        {"src/analysis/compilation_unit/compilation_unit.cpp", {}},
        {"src/analysis/syntactic/parser.cpp", {}},
        {"src/analysis/semantic/semantic_analyzer.cpp", {}},
        {"src/tokenizer/tokenizer.cpp", {}},
        {"src/core/types/tree_builder.cpp", {}},
        {"src/analysis/preprocess/preprocessor.cpp",
         {
             {"DiagnosticReporter scratch;", 4,
              "pre-scan / re-reported throwaways: their contents are either "
              "discarded or re-reported into a budgeted reporter, so they are "
              "never an operator-visible budget. RAISED 5 -> 6 (cycle P36) for "
              "the sixth, in `preScanIncludeFile`: the per-file pre-scan memo "
              "tokenizes each header ONCE and discards that tokenize's "
              "diagnostics exactly as the per-occurrence call site it replaced "
              "already did -- the authoritative pass re-tokenizes the same "
              "bytes and owns every diagnostic about them, so memoizing moved "
              "WHERE the throwaway is built and dropped nothing an operator "
              "could see. ⚠ The count is the memo's ONE call site, not a "
              "per-header multiplier: if this number ever has to rise because "
              "a reporter was added per HEADER rather than per call site, that "
              "is a budget escaping into a loop and the fix is the budget, not "
              "this number. LOWERED 6 -> 4 (cycle P57, "
              "D-PP-SINGLE-PASS-INCLUDE-RESOLUTION): the include pre-scan's "
              "private object-like evaluator is GONE — its macro state is now "
              "the authoritative `MacroExpander`, hosted as an oracle — so "
              "`sbMintProduct`'s per-mint tokenize scratch and "
              "`sbEvalIfOperand`'s per-evaluation ICE scratch no longer exist. "
              "The oracle's OWN reporter is budget-derived "
              "(`preScanScratch{budget.asConfig()}`) rather than allowlisted, "
              "so this count falls without a new entry. ⚠ A DROP is as much a "
              "deliberate change as a rise: the two that went were the shadow "
              "evaluator's, and if this number ever falls again without a "
              "deletion in `preprocessor.cpp` to point at, a reporter was "
              "silently re-pointed at a budgeted one and the question is "
              "whether its contents can now reach the operator"},
             {"DiagnosticReporter macroRep;   // throwaway - malformed surfaced downstream",
              1, "throwaway; the malformed macro is surfaced downstream"},
         }},
    };

    for (auto const& subj : subjects) {
        fs::path const file = root / fs::path{std::string{subj.path}};
        std::ifstream in{file};
        ASSERT_TRUE(in.good()) << "cannot read subject " << file.string();

        // Read the whole file once: classifying a BARE declaration needs the
        // constructor init-list, which is elsewhere in the file.
        std::string whole;
        {
            std::ostringstream buf;
            buf << in.rdbuf();
            whole = buf.str();
        }
        std::istringstream lines{whole};

        std::vector<std::pair<std::size_t, std::string>> violations;
        std::vector<std::string>                          seenAllowed;
        std::string line;
        for (std::size_t lineNo = 1; std::getline(lines, line); ++lineNo) {
            std::string const t = trimmed(line);
            if (t.rfind("//", 0) == 0) continue;          // a comment, not code
            // A PARAMETER declaration ends the line with `,` or `)` — it
            // declares nothing and constructs nothing.
            if (!t.empty() && (t.back() == ',' || t.back() == ')')) continue;

            bool const viaMakeUnique =
                t.find("make_unique<DiagnosticReporter>") != std::string::npos;
            bool const isDecl = (t.rfind("DiagnosticReporter ", 0) == 0);
            if (!viaMakeUnique && !isDecl) continue;
            if (isDecl && (t.find('&') != std::string::npos)) continue;  // a reference

            // A BARE `DiagnosticReporter x;` is a DECLARATION, not necessarily a
            // default construction: a class MEMBER is initialized in the ctor's
            // init list, which is where its budget actually arrives. Look the
            // member up rather than guessing from this line alone — guessing is
            // what produced this guard's first three false positives.
            std::string effective = t;
            bool const bare = isDecl && !viaMakeUnique &&
                              t.find('{') == std::string::npos &&
                              t.find('(') == std::string::npos;
            if (bare) {
                std::string ident = t.substr(std::string{"DiagnosticReporter "}.size());
                ident = trimmed(ident);
                if (!ident.empty() && ident.back() == ';') ident.pop_back();
                ident = trimmed(ident);
                // `: ident(` or `, ident(` — a ctor member-initializer.
                for (std::string_view marker : {": ", ", "}) {
                    std::string const needle = std::string{marker} + ident + "(";
                    auto pos = whole.find(needle);
                    while (pos != std::string::npos) {
                        auto const eol = whole.find('\n', pos);
                        std::string const initLine =
                            whole.substr(pos, (eol == std::string::npos ? whole.size() : eol) - pos);
                        // A ctor-init that MOVES an already-built reporter in is a
                        // transfer, not a construction — the budget was applied
                        // where it was built.
                        if (initLine.find("budget") != std::string::npos ||
                            initLine.find("Budget") != std::string::npos ||
                            initLine.find("asConfig()") != std::string::npos ||
                            initLine.find("std::move(") != std::string::npos) {
                            effective = initLine;
                        }
                        pos = whole.find(needle, pos + 1);
                    }
                }
            }

            // ★★ `libraryDefault()` IS NOT BUDGET-DERIVED, AND SAYING SO IS THE
            // WHOLE POINT OF THIS CHECK. ✔MEASURED 2026-08-13: the first
            // version of this guard tested for the substring "Budget", which
            // `DiagnosticBudget::libraryDefault()` contains — so a mutation
            // that reverted a shipped construction site to the library defaults
            // left the guard GREEN. That is the exact regression this file
            // exists to catch, wearing the name of the type that fixes it. A
            // threaded file may reach the library defaults only through the
            // allowlist, where it has to carry a reason.
            bool const libraryDefaulted =
                effective.find("libraryDefault") != std::string::npos;
            bool const budgetDerived =
                !libraryDefaulted &&
                (effective.find("budget") != std::string::npos ||
                 effective.find("Budget") != std::string::npos ||
                 effective.find("asConfig()") != std::string::npos ||
                 effective.find("std::move(") != std::string::npos);
            if (budgetDerived) continue;
            seenAllowed.push_back(t);
            bool listed = false;
            for (auto const& a : subj.allowed) {
                std::string const code = t.substr(0, t.find("//"));
                std::string const want = std::string{a.line}.substr(
                    0, std::string{a.line}.find("//"));
                if (trimmed(code) == trimmed(want)) { listed = true; break; }
            }
            if (!listed) violations.emplace_back(lineNo, t);
        }

        for (auto const& v : violations) {
            ADD_FAILURE()
                << subj.path << ":" << v.first
                << " constructs a DiagnosticReporter that is NOT derived from a "
                   "DiagnosticBudget: `" << v.second << "`. Every reporter in a "
                   "threaded file must carry the operator's budget "
                   "(D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE). If this "
                   "one is a genuine throwaway whose contents never reach the "
                   "operator, add it to this test's allowlist WITH A REASON.";
        }

        // Counts: an allowlisted shape must not silently gain a new instance.
        for (auto const& a : subj.allowed) {
            std::string const want =
                trimmed(std::string{a.line}.substr(0, std::string{a.line}.find("//")));
            std::size_t n = 0;
            for (auto const& s : seenAllowed) {
                if (trimmed(s.substr(0, s.find("//"))) == want) ++n;
            }
            EXPECT_EQ(n, a.count)
                << subj.path << ": allowlisted throwaway `" << a.line
                << "` occurs " << n << " times, expected " << a.count
                << ". A NEW unbudgeted reporter was added — give it the budget, "
                   "or raise this count deliberately and say why. Reason on "
                   "record for the existing ones: " << a.why;
        }
    }
}

// ★★ THE FORWARDING SITES, WHICH THE REPORTER SCAN ABOVE CANNOT SEE.
// ✔MEASURED 2026-08-13: reverting `Tokenizer tk{sc.source, sc.schema, budget_}`
// (the oracle-reparse path) to `libraryDefault()` left EVERY pin in this file
// green — the scan above only looks at `DiagnosticReporter` constructions, and
// that line constructs a `Tokenizer`. The tier reporters are reached through
// `Tokenizer` / `Parser` / `TreeBuilder` / `UnitBuilder` / `preprocess`, so a
// budget can be dropped at a FORWARDING site without any reporter construction
// changing at all.
//
// The rule is deliberately blunt and therefore hard to drift: on the threaded
// path the library defaults are never the right answer, so `libraryDefault()`
// must not appear in these files AT ALL. (It is exactly right in `src/lsp/` and
// `src/ffi/`, which have no operator budget to thread — they are not subjects.)
TEST(DiagnosticBudgetThreadingSourceEnumeration,
     NoThreadedFileReachesTheLibraryDefaultBudget) {
    fs::path const root = dss::test::repoRoot();
    ASSERT_FALSE(root.empty());
    for (std::string_view rel : {
             "src/analysis/compilation_unit/compilation_unit.cpp",
             "src/analysis/preprocess/preprocessor.cpp",
             "src/analysis/syntactic/parser.cpp",
             "src/analysis/semantic/semantic_analyzer.cpp",
             "src/tokenizer/tokenizer.cpp",
             "src/core/types/tree_builder.cpp",
             "src/program/program.cpp",
             "src/program/compile_pipeline.cpp",
         }) {
        std::ifstream in{root / fs::path{std::string{rel}}};
        ASSERT_TRUE(in.good()) << "cannot read subject " << rel;
        std::ostringstream buf;
        buf << in.rdbuf();
        std::string const whole = buf.str();
        std::size_t pos = whole.find("libraryDefault");
        while (pos != std::string::npos) {
            auto const lineNo = static_cast<std::size_t>(
                std::count(whole.begin(),
                           whole.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) + 1;
            ADD_FAILURE()
                << rel << ":" << lineNo
                << " reaches the LIBRARY DEFAULT budget inside a threaded file. "
                   "Every construction on this path must forward the operator's "
                   "budget; `libraryDefault()` here is the silent revert to "
                   "1000/50 that D-DIAG-VOLUME-CAP-ENFORCED-AT-SIX-STAGES-NOT-ONCE "
                   "records.";
            pos = whole.find("libraryDefault", pos + 1);
        }
    }
}
