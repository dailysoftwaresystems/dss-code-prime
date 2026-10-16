// P69 round 4 (lane `lm`; the re-review of lane `cs`, finding 1): THE SWEEP — for EVERY library
// builtin the C language declares, on EVERY real target pair, `#if __has_builtin(__builtin_<name>)`
// answers 1 exactly where the semantic tier BINDS a use of the builtin, and 0 exactly where it
// refuses the use (S_LibraryBuiltinUnavailable).
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════════════════════
//
// The two answers have two owners. The preprocessor asks the corpus INDEX
// (`ffi::shippedLibraryFunctionProvidedOnFormat`: names and availability gates, no signature is
// decoded); the binder asks the realization ORACLE (`ffi::realizeShippedExternSymbols`: the
// descriptor read in full, for the pair). Wherever they part, a portable
// `#if __has_builtin(__builtin_x)` guard walks straight into a call the next tier refuses — or
// hides one that would have bound. Six names were asserted by hand
// (`FeatureQueryOperators.ALibraryBuiltinAnswersWhereThePlatformProvidesTheFunction`); the review
// found a row class neither list covered (an alias row), so the agreement is asked here of the
// WHOLE list: `semantics.libraryBuiltins.functions`, read off the shipped `c` document, never
// retyped.
//
// ═══ HOW EACH ANSWER IS READ ════════════════════════════════════════════════════════════════
//
// ONE translation unit per pair, compiled through the driver's own two halves
// (`applyTargetFormatPair` + `analyzeForTargetFormat`), so both tiers see the pair a build of
// that target sees:
//
//     #if __has_builtin(__builtin_<name>)       ← the PREPROCESSOR's answer: the object below is
//     int sweep_has_<name>;                        declared iff it answered non-zero
//     #endif
//     void sweep_uses(void) { (void)__builtin_<name>; … }   ← the BINDER's answer: accepted, or
//                                                              one S_LibraryBuiltinUnavailable
//                                                              naming the builtin
//
// The use is unconditional — never under the `#if` — so a name that answers 0 is still put to the
// binder, and "answers 0 where the call would bind" is seen as well as its converse.
//
// A FAILURE NAMES THE BUILTIN AND THE PAIR. Any other error of either tier fails the pair too: an
// accepted use that is refused for another reason is a disagreement by another name.
//
// RED-ON-DISABLE (read on the built mutant): make `shippedLibraryFunctionProvidedOnFormat` look
// past a row's symbol gate → every pe pair fails naming the builtins math.json gates to elf and
// macho (`__builtin_fabsf` among them: ucrtbase.dll exports no fabsf).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/target_format_analysis.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

// The stems of `<configRoot>/<dir>/*<suffix>`, sorted — the shipped documents, never a hand list.
[[nodiscard]] std::vector<std::string> shippedNames(std::string_view dir, std::string_view suffix) {
    std::vector<std::string> out;
    for (auto const& e : std::filesystem::directory_iterator{dss::test::configRoot() / dir}) {
        std::string const fn = e.path().filename().string();
        if (fn.size() <= suffix.size() || !fn.ends_with(suffix)) continue;
        out.push_back(fn.substr(0, fn.size() - suffix.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

constexpr std::string_view kHasPrefix = "sweep_has_";

// What ONE pair answered for the whole list.
struct PairAnswers {
    std::set<std::string>              has;       // `__has_builtin` answered non-zero
    std::map<std::string, std::string> refused;   // the binder refused the use: name → its words
    std::string                        strayErrors;   // any other error, either tier
};

// A volume budget above the list, and no recent-duplicate window: one refusal per name must reach
// the reader, and a cap would turn "refused" into "not reported".
[[nodiscard]] DiagnosticBudget sweepBudget(std::size_t names) {
    DiagnosticReporter::Config cfg{};
    cfg.maxDiagnostics = names * 8 + 1000;
    cfg.maxPerCode     = names * 4 + 100;
    cfg.dedupWindow    = 0;
    return DiagnosticBudget{cfg};
}

void appendErrors(DiagnosticReporter const& rep, std::string_view tier, std::string& out) {
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "\n  [";
        out += tier;
        out += "] ";
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
    }
}

[[nodiscard]] PairAnswers answersFor(TargetSchema const& target, ObjectFormatSchema const& format,
                                     LibraryBuiltins const& builtins) {
    std::string source;
    for (std::string const& fn : builtins.functions) {
        source += "#if __has_builtin(" + builtins.prefix + fn + ")\nint " + std::string{kHasPrefix} + fn
                + ";\n#endif\n";
    }
    source += "void sweep_uses(void) {\n";
    for (std::string const& fn : builtins.functions) source += "    (void)" + builtins.prefix + fn + ";\n";
    source += "}\n";

    DiagnosticBudget const budget = sweepBudget(builtins.functions.size());
    UnitBuilder builder{cLanguage(), budget};
    applySystemDirs(builder, *cLanguage());
    applyTargetFormatPair(builder, target, format);
    builder.addInMemory(std::move(source), "sweep.c");
    auto const cu = std::make_shared<CompilationUnit const>(std::move(builder).finish());

    PairAnswers r;
    appendErrors(cu->driverDiagnostics(), "driver", r.strayErrors);
    for (auto const& tree : cu->trees()) appendErrors(tree.diagnostics(), "parse", r.strayErrors);

    auto const analysis = analyzeForTargetFormat(cu, budget, target, format, nullptr);
    for (SymbolRecord const& sym : analysis.model.symbols()) {
        if (sym.name.size() > kHasPrefix.size() && std::string_view{sym.name}.starts_with(kHasPrefix))
            r.has.insert(sym.name.substr(kHasPrefix.size()));
    }
    for (auto const& d : analysis.model.diagnostics().all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        if (d.code == DiagnosticCode::S_LibraryBuiltinUnavailable) {
            // The refusal opens with the spelling it refuses, in quotes.
            auto const open = d.actual.find('\'');
            auto const close = open == std::string::npos ? open : d.actual.find('\'', open + 1);
            std::string_view const spelled =
                close == std::string::npos
                    ? std::string_view{}
                    : std::string_view{d.actual}.substr(open + 1, close - open - 1);
            std::string_view const fn = builtins.libraryFunctionOf(spelled);
            if (!fn.empty()) {
                r.refused.emplace(std::string{fn}, d.actual);
                continue;
            }
            // A refusal this reader cannot attribute is reported as it stands, below.
        }
        r.strayErrors += "\n  [semantic] ";
        r.strayErrors += diagnosticCodeName(d.code);
        r.strayErrors += ": ";
        r.strayErrors += d.actual;
    }
    return r;
}

}  // namespace

TEST(LibraryBuiltinAnswerSweep, HasBuiltinAnswersWhereTheBinderBindsOnEveryRealPair) {
    ASSERT_NE(cLanguage(), nullptr);
    LibraryBuiltins const& builtins = cLanguage()->semantics().libraryBuiltins;
    ASSERT_FALSE(builtins.prefix.empty()) << "the shipped c document declares no library builtins";
    // FLOOR, ✔MEASURED 2026-10-08: 360 names. A sweep over a list that collapsed passes in silence.
    ASSERT_GE(builtins.functions.size(), 300u) << "the library-builtin list collapsed";

    std::size_t pairs = 0;
    std::size_t refusedSomewhere = 0;
    for (std::string const& targetName : shippedNames("targets", ".target.json")) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (std::string const& formatName : shippedNames("object-formats", ".format.json")) {
            auto formatR = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(formatR.has_value()) << formatName;
            if ((*formatR)->targetArch() != (*targetR)->name()) continue;   // not a real pair
            std::string const pair = targetName + ":" + formatName;
            SCOPED_TRACE(pair);
            ++pairs;
            PairAnswers const a = answersFor(**targetR, **formatR, builtins);
            EXPECT_EQ(a.strayErrors, "") << "errors that are not a library-builtin refusal";

            std::size_t provided = 0;
            for (std::string const& fn : builtins.functions) {
                bool const has = a.has.contains(fn);
                auto const refusal = a.refused.find(fn);
                bool const binds = refusal == a.refused.end();
                if (binds) ++provided;
                if (has == binds) continue;
                if (has) {
                    ADD_FAILURE() << "`__has_builtin(" << builtins.prefix << fn << ")` answers 1 on " << pair
                                  << ", and a use of the builtin there is REFUSED: " << refusal->second;
                } else {
                    ADD_FAILURE() << "`__has_builtin(" << builtins.prefix << fn << ")` answers 0 on " << pair
                                  << ", and a use of the builtin there BINDS (the semantic tier accepts it)";
                }
            }
            refusedSomewhere += a.refused.size();
            // THE POSITIVE CONTROL: `strlen` is provided on every real pair. With it, an "every
            // name answers 0 and every use is refused" pair — a corpus this process did not find
            // — cannot pass as agreement.
            EXPECT_TRUE(a.has.contains("strlen")) << "`__has_builtin(__builtin_strlen)` must answer 1";
            EXPECT_FALSE(a.refused.contains("strlen")) << "a use of `__builtin_strlen` must bind";
            EXPECT_GE(provided, 20u) << "almost nothing binds on this pair: was the corpus read?";
        }
    }
    // FLOOR: the real-pair enumeration of the limits.h pin (P68 round 9: 22 real pairs).
    EXPECT_GE(pairs, 22u) << "the real-pair enumeration collapsed";
    // THE NEGATIVE HALF MUST HAVE SOMETHING TO CHECK. ✔MEASURED 2026-10-08: the corpus provides
    // only part of the list on every pair. If it ever provides all of it everywhere, this floor
    // is what says the 0-answer half of the sweep has stopped being exercised — remove it then,
    // deliberately.
    EXPECT_GE(refusedSomewhere, 1u)
        << "no listed builtin is refused on any pair: the sweep's 0-answer half checked nothing";
}
