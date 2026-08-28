// D-DRIVER-SHIPPED-SOURCE-RESOLUTION-COMPILES-EVERY-SHIPPED-GRAMMAR — THE
// EXTENSION⇒LANGUAGE RESOLUTION THE SHIPPED RUNTIME UNITS GO THROUGH.
//
// WHAT THE DRIVER DOES, AND WHAT CHANGED
//
// A shipped-lib descriptor may declare that a symbol's body is PROVIDED by a
// source file DSS ships. The driver compiles those units as ordinary extra
// translation units, and it picks their FRONT END the same way it picks one for
// any file: by asking which shipped language document claims the extension.
//
// That question used to be answered by CONSTRUCTING every shipped language's
// whole grammar and reading one field off each. This suite pins the answer AND
// the mechanism, because a test that pinned only the answer would stay green the
// day the resolution went back to building six grammars to read six arrays.
//
// ★★★ THE MECHANISM PIN IS `ADocumentThatIsNotALoadableGrammarStillClaims`, and
// it is a behavioural discriminator rather than a counter, so no amount of
// memoization anywhere in the loaders can make it vacuous. A document with a
// well-formed `language` block and a BROKEN grammar body is:
//   * INVISIBLE to a resolver that constructs grammars — the construction fails
//     and the old code skipped it ("health is the loader's own business"), so
//     the corpus looked like it had ONE claimant and the build succeeded;
//   * a CLAIMANT to a resolver that reads the declared field — two claimants,
//     and the driver refuses rather than guessing.
// Those two outcomes are opposite, so exactly one of them can be green.
//
// ★★ AND THE COUNT IS PINNED TOO, on the property the change is actually about:
// `ResolvingOneFrontEndDoesNotBuildEveryShippedGrammar` measures how many
// `GrammarSchema` objects a one-file compile constructs and refuses a number
// that reaches the size of the language corpus. Construction is counted through
// `GrammarSchema::schemaId()`, which is minted from ONE monotonic counter at the
// single site that builds a schema from JSON — so the delta between two probe
// loads IS the number of constructions in between, with no instrumentation
// inside the product.
//
// ⚠ THE PROBES AND THE STAGED CORPUS CARRY A PER-RUN BYTE NONCE (trailing
// whitespace, which JSON ignores and which changes nothing but the digest).
// Config documents are memoized in-process BY THEIR BYTES, so without the nonce
// a document another test in this binary had already loaded would answer from
// the memo, the measured count would collapse to zero, and the assertion would
// report a beautiful green having measured nothing. The nonce makes every load
// in these tests a real construction, which is what makes the count a
// measurement.
//
// WHAT IT DELIBERATELY DOES NOT DO. It does not restate
// `tests/program/test_shipped_runtime_compiles.cpp`, which asserts that every
// declared realization COMPILES for its format on every machine. This file
// asserts only how the FRONT END for such a unit is chosen, and what happens
// when the corpus cannot name one. Neither implies the other.
//
// ★ NO LANGUAGE NAME, NO FORMAT NAME AND NO EXTENSION IS SPELLED BELOW. The
// realized unit, its extension, the language that claims it, the format kind
// that declares it and the machine it compiles for are all discovered from the
// shipped trees through the production readers — the same discipline
// `test_shipped_runtime_compiles.cpp` states at length. The one string this file
// authors is the extension of the decoy documents it writes itself, and that one
// is asserted to be claimed by NOTHING before it is used.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/program.hpp"
#include "program/runtime_object_cache.hpp"

#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace {

// ── The real trees, through the ONE test-side resolver ───────────────────────
[[nodiscard]] fs::path configRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg;
}

[[nodiscard]] fs::path descriptorDir()    { return configRoot() / "shippedLibs"; }
[[nodiscard]] fs::path objectFormatsDir() { return configRoot() / "object-formats"; }
[[nodiscard]] fs::path targetsDir()       { return configRoot() / "targets"; }
[[nodiscard]] fs::path languagesDir()     { return configRoot() / "sources"; }

constexpr std::string_view kLangSuffix = ".lang.json";

[[nodiscard]] std::string lowered(std::string_view s) {
    std::string out{s};
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Every shipped schema NAME in `dir` — the `loadShipped` key of each
// `<name><suffix>` document, sorted so a failure message never depends on the
// host filesystem's iteration order.
[[nodiscard]] std::vector<std::string> shippedSchemaNames(fs::path const& dir,
                                                          std::string_view suffix) {
    std::vector<std::string> names;
    std::error_code          ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        std::string const leaf = it->path().filename().generic_string();
        if (!leaf.ends_with(suffix)) continue;
        names.push_back(leaf.substr(0, leaf.size() - suffix.size()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] std::string readWhole(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) return {};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] bool writeWhole(fs::path const& p, std::string_view text) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

// Every ERROR the reporter carries, rendered — a refusal assertion has to be
// able to say WHAT was refused when it fails.
[[nodiscard]] std::string renderErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "\n      [";
        out += diagnosticCodeName(d.code);
        out += "] ";
        out += d.actual;
    }
    return out.empty() ? std::string{"\n      (the reporter carries no Error)"} : out;
}

[[nodiscard]] bool anyErrorContains(DiagnosticReporter const& rep,
                                    std::string_view          needle) {
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ── THE SUBJECT, DISCOVERED FROM THE CORPUS ──────────────────────────────────

// One shipped runtime unit the descriptor corpus realizes: which object-format
// KEY declares it and which config-root-relative source it names.
struct RealizedUnit {
    std::string formatKey;
    std::string source;
};

// The first realized unit in the corpus, offering EVERY name in the closed
// `ObjectFormatKind` vocabulary to the production corpus reader — the same
// discovery `test_shipped_runtime_compiles.cpp` uses for its denominator. If
// this answers nothing, every test below is vacuous, so every test asserts it.
[[nodiscard]] std::optional<RealizedUnit> firstRealizedUnit() {
    for (auto const& row : kObjectFormatKindTable.rows) {
        if (!isSelectableObjectFormatKind(row.first)) continue;
        std::string const key{row.second};
        for (auto& source : dss::ffi::allShippedSourcesForFormat(descriptorDir(), key))
            return RealizedUnit{key, std::move(source)};
    }
    return std::nullopt;
}

// One (target, archive-writing format) pair the given format KIND reaches. The
// ARCHIVE sibling for the same reason the sibling gate uses it: it exercises the
// whole front end and the shipped-source seam without demanding a linkable
// program image, so the test measures resolution rather than linking.
struct Machine {
    std::string target;
    std::string archiveFormat;
};

[[nodiscard]] std::optional<Machine> firstMachineFor(std::string const& formatKey) {
    auto const targetNames = shippedSchemaNames(targetsDir(), ".target.json");
    for (auto const& formatName :
         shippedSchemaNames(objectFormatsDir(), ".format.json")) {
        auto const format = ObjectFormatSchema::loadShipped(formatName);
        if (!format.has_value()) continue;
        if (objectFormatKindName((*format)->kind()) != formatKey) continue;
        for (auto const& targetName : targetNames) {
            auto const target = TargetSchema::loadShipped(targetName);
            if (!target.has_value()) continue;
            // The reporter is local and discarded: a non-matching pair is the
            // ORDINARY case in a cross product, not an event.
            DiagnosticReporter pairing;
            if (!crossValidateTargetFormat(**target, **format, pairing)) continue;
            auto const sibling = dss::runtime::resolveArchiveSiblingFormat(
                **format, **target, objectFormatsDir(),
                dss::runtime::kRuntimeCacheSiblingRequester);
            if (!sibling.has_value()) continue;
            return Machine{targetName, *sibling};
        }
    }
    return std::nullopt;
}

// Which shipped language claims `ext`, re-derived from the config exactly as
// `test_shipped_runtime_compiles.cpp` does — never restated as a literal.
// Answers nullopt on zero claimants AND on two, because both are refusals.
[[nodiscard]] std::optional<std::string> languageClaiming(std::string const& ext) {
    std::vector<std::string> claimants;
    for (auto const& name : shippedSchemaNames(languagesDir(), kLangSuffix)) {
        auto const grammar = GrammarSchema::loadShipped(name);
        if (!grammar.has_value()) continue;   // health is the loader's own business
        for (auto const& declared : (*grammar)->fileExtensions())
            if (lowered(declared) == ext) { claimants.push_back(name); break; }
    }
    if (claimants.size() == 1) return claimants.front();
    return std::nullopt;
}

// ── A PRIVATE, WRITABLE COPY OF THE WHOLE SHIPPED CONFIG TREE ────────────────
//
// `$DSS_CONFIG_ROOT` names a REPO-SHAPED root, i.e. a directory that CONTAINS
// `src/dss-config/`, so that is the shape staged here. No `VERSION` file is
// written: the skew check only judges a tree that declares a version, and this
// tree deliberately declares none — it is the SAME documents, byte for byte,
// plus whatever the individual test perturbs.
struct StagedTree {
    fs::path root;        // hand this to DSS_CONFIG_ROOT
    fs::path configDir;   // root/src/dss-config
};

[[nodiscard]] std::optional<StagedTree> stageConfigTree(fs::path const& under) {
    StagedTree staged;
    staged.root      = under;
    staged.configDir = under / "src" / "dss-config";
    std::error_code ec;
    fs::create_directories(staged.configDir.parent_path(), ec);
    if (ec) {
        ADD_FAILURE() << "cannot create the staged config root at "
                      << staged.configDir.parent_path().generic_string() << ": "
                      << ec.message();
        return std::nullopt;
    }
    fs::copy(configRoot(), staged.configDir, fs::copy_options::recursive, ec);
    if (ec) {
        ADD_FAILURE() << "cannot stage the shipped config tree into "
                      << staged.configDir.generic_string() << ": " << ec.message();
        return std::nullopt;
    }
    return staged;
}

// A run-unique count of trailing spaces. Trailing whitespace is ignored by every
// JSON reader on both paths (the SAX field read and the full loader), so this
// changes the document's DIGEST and nothing else — which is exactly what defeats
// a bytes-keyed in-process memo without changing what any document MEANS.
[[nodiscard]] std::size_t nextNonce() {
    static std::atomic<std::size_t> counter{0};
    return counter.fetch_add(1) + 1;
}

[[nodiscard]] std::string withNonce(std::string text, std::size_t nonce) {
    text.append(nonce, ' ');
    return text;
}

// Give every language document in the staged tree bytes nothing else in this
// process has seen.
[[nodiscard]] bool nonceEveryLanguageDocument(fs::path const& stagedSources,
                                              std::size_t     nonce) {
    bool ok = true;
    for (auto const& name : shippedSchemaNames(stagedSources, kLangSuffix)) {
        fs::path const doc = stagedSources / (name + std::string{kLangSuffix});
        std::string const text = readWhole(doc);
        if (text.empty() || !writeWhole(doc, withNonce(text, nonce))) {
            ADD_FAILURE() << "cannot re-write the staged language document "
                          << doc.generic_string();
            ok = false;
        }
    }
    return ok;
}

// ── THE CONSTRUCTION COUNTER ─────────────────────────────────────────────────
//
// `SchemaId` is minted from ONE monotonic counter, at the ONE site that builds a
// GrammarSchema out of JSON, so the ids of two schemas constructed either side of
// some work bracket exactly the constructions that happened in between. The probe
// text is a real shipped document plus a unique nonce, so the probe itself is
// always a construction and never a memo hit.
[[nodiscard]] std::optional<std::uint32_t> constructionProbe(
    std::string const& baseDocumentText) {
    auto const probe = GrammarSchema::loadFromText(
        withNonce(baseDocumentText, nextNonce()),
        "<shipped-source language resolution construction probe>");
    if (!probe.has_value()) return std::nullopt;
    return (*probe)->schemaId().v;
}

// The smallest shipped language document's TEXT — the cheapest thing that is
// guaranteed to load, chosen by size from the corpus rather than by name.
[[nodiscard]] std::string smallestLanguageDocumentText() {
    fs::path    best;
    std::size_t bestSize = 0;
    for (auto const& name : shippedSchemaNames(languagesDir(), kLangSuffix)) {
        fs::path const  doc = languagesDir() / (name + std::string{kLangSuffix});
        std::error_code ec;
        auto const      size = fs::file_size(doc, ec);
        if (ec) continue;
        if (best.empty() || size < bestSize) { best = doc; bestSize = size; }
    }
    return best.empty() ? std::string{} : readWhole(best);
}

// ── ONE COMPILE, THROUGH THE DRIVER ──────────────────────────────────────────
//
// The user file is trivial and its content is beside the point: what matters is
// that ANY compile for a format the corpus realizes drags the shipped runtime
// units in (they are compiled per (target, config), not per `#include`), which
// is what puts the resolver under test on the path.
struct CompileOutcome {
    int                rc = 1;
    DiagnosticReporter rep;
};

void runOneCompile(fs::path const& sandbox, std::string const& languageName,
                   std::string const& spec, CompileOutcome& out) {
    fs::path const  outDir = sandbox / "out";
    std::error_code ec;
    fs::create_directories(outDir, ec);
    ASSERT_FALSE(ec) << "cannot create " << outDir.generic_string() << ": "
                     << ec.message();
    fs::path const src = sandbox / "one.c";
    ASSERT_TRUE(writeWhole(src, "int main(void){return 0;}\n"))
        << "cannot write the trivial user source at " << src.generic_string();

    Program program;
    program.setOutputDir(outDir);
    program.setCompileConfig(CompileConfig::Debug);
    out.rc = program.compileFiles(std::vector<std::string>{src.string()},
                                  languageName, std::vector<std::string>{spec},
                                  out.rep);
}

}  // namespace

// ═══ THE DENOMINATOR ═════════════════════════════════════════════════════════

// Every test below is a statement about a build that REALIZES a shipped source
// unit. With no realized unit the resolver is never reached and all of them pass
// having exercised nothing, so the corpus is asserted first and separately —
// this is the assertion that makes the rest measurements.
TEST(ShippedSourceLanguageResolution, TheCorpusRealizesAtLeastOneShippedSourceUnit) {
    ASSERT_FALSE(configRoot().empty());
    auto const unit = firstRealizedUnit();
    ASSERT_TRUE(unit.has_value())
        << "no shipped-lib descriptor under " << descriptorDir().generic_string()
        << " declares a 'realization' with a source for ANY name in the "
           "ObjectFormatKind vocabulary. The extension⇒language resolution these "
           "tests pin is then unreachable, and every one of them would be a "
           "vacuous green.";
    if (!unit) return;
    EXPECT_FALSE(unit->source.empty());
    auto const machine = firstMachineFor(unit->formatKey);
    ASSERT_TRUE(machine.has_value())
        << "format kind '" << unit->formatKey
        << "' realizes a shipped source but reaches no (target, archive format) "
           "pair over the shipped trees, so no compile below can exercise it";
}

// ═══ THE ANSWER ══════════════════════════════════════════════════════════════

// A sole claimant resolves, and the build that pulls the realized unit in
// succeeds. This is the arm every other test is the negative of.
TEST(ShippedSourceLanguageResolution, TheSoleClaimantOfTheUnitsExtensionResolves) {
    ASSERT_FALSE(configRoot().empty());
    auto const unit = firstRealizedUnit();
    ASSERT_TRUE(unit.has_value());
    auto const machine = firstMachineFor(unit->formatKey);
    ASSERT_TRUE(machine.has_value());

    std::string const unitExt = lowered(fs::path{unit->source}.extension().generic_string());
    ASSERT_FALSE(unitExt.empty())
        << "the realized unit '" << unit->source << "' has no extension at all, so "
           "the extension⇒language rule cannot name a front end for it";

    auto const claiming = languageClaiming(unitExt);
    ASSERT_TRUE(claiming.has_value())
        << "the extension '" << unitExt << "' of realized unit '" << unit->source
        << "' is claimed by zero or by two shipped languages, so the driver "
           "refuses it — which the corpus must not be in the habit of doing";

    // The USER file's language is the same one, resolved the same way; the point
    // is that the compile reaches the shipped-source seam and comes back clean.
    ScratchDir     scratch{Location::Temp, "shipped-source-language-resolution"};
    CompileOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(runOneCompile(scratch.path(), *claiming,
                                          machine->target + ":" + machine->archiveFormat,
                                          outcome));
    EXPECT_EQ(outcome.rc, 0) << renderErrors(outcome.rep);
    EXPECT_FALSE(outcome.rep.hasErrors())
        << "a build that realizes shipped source unit '" << unit->source
        << "' under language '" << *claiming << "' must resolve its front end "
           "cleanly" << renderErrors(outcome.rep);
}

// ═══ THE MECHANISM ═══════════════════════════════════════════════════════════

// ★★★ RED-ON-DISABLE FOR THE WHOLE CHANGE. A document whose `language` block is
// well-formed and whose grammar body is NOT is a claimant to a resolver that
// READS the declared field, and invisible to one that CONSTRUCTS a grammar to
// find out. The two answers are opposite — refuse vs. succeed — so restoring the
// construct-every-grammar resolution turns this test red immediately, and no
// loader memoization anywhere can blur it.
TEST(ShippedSourceLanguageResolution, ADocumentThatIsNotALoadableGrammarStillClaims) {
    ASSERT_FALSE(configRoot().empty());
    auto const unit = firstRealizedUnit();
    ASSERT_TRUE(unit.has_value());
    auto const machine = firstMachineFor(unit->formatKey);
    ASSERT_TRUE(machine.has_value());
    std::string const unitExt =
        lowered(fs::path{unit->source}.extension().generic_string());
    auto const claiming = languageClaiming(unitExt);
    ASSERT_TRUE(claiming.has_value());

    ScratchDir scratch{Location::Temp, "shipped-source-language-resolution"};
    auto const staged = stageConfigTree(scratch.path() / "tree");
    ASSERT_TRUE(staged.has_value());

    // The decoy: a valid `language` block claiming the realized unit's extension,
    // and NOTHING a grammar can be built from. The name is this file's own — it
    // is not read from the corpus, it is written into it.
    std::string const decoyName = "lane-b-decoy";
    fs::path const    decoyDoc =
        staged->configDir / "sources" / (decoyName + std::string{kLangSuffix});
    ASSERT_TRUE(writeWhole(decoyDoc,
                           std::string{"{\"language\":{\"name\":\""} + decoyName
                           + "\",\"version\":\"1\",\"fileExtensions\":[\"" + unitExt
                           + "\"]}}\n"))
        << "cannot write the decoy document at " << decoyDoc.generic_string();

    ScopedEnv env{"DSS_CONFIG_ROOT", staged->root.string()};

    // ⚠ THE DISCRIMINATOR IS ASSERTED, NOT ASSUMED. If the decoy ever became a
    // LOADABLE grammar, the test below would still refuse — but it would be
    // measuring "two loadable languages claim one extension", which is a
    // different and much weaker statement than the one this test exists to make.
    auto const asGrammar = GrammarSchema::loadShipped(decoyName);
    ASSERT_FALSE(asGrammar.has_value())
        << "the decoy must NOT be constructible as a grammar — that is precisely "
           "what makes this a test of HOW the claim is read. A resolver that "
           "builds a grammar to answer 'do you claim this extension?' cannot see "
           "this document at all.";

    CompileOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(runOneCompile(scratch.path() / "work", *claiming,
                                          machine->target + ":" + machine->archiveFormat,
                                          outcome));
    EXPECT_NE(outcome.rc, 0)
        << "with TWO documents claiming '" << unitExt
        << "' the driver must refuse rather than pick one" << renderErrors(outcome.rep);
    EXPECT_TRUE(anyErrorContains(outcome.rep,
                                 "shipped languages claim the extension '" + unitExt))
        << "the refusal must be the TWO-CLAIMANT diagnostic, naming the extension "
           "and saying it refuses rather than guesses" << renderErrors(outcome.rep);
}

// The other refusal: an extension NO shipped document claims. Reached by giving
// the realized unit an extension the corpus is asserted not to know, which also
// proves the resolver is reading the DECLARED extension set and not some other
// property of the document.
TEST(ShippedSourceLanguageResolution, AnUnclaimedExtensionIsRefused) {
    ASSERT_FALSE(configRoot().empty());
    auto const unit = firstRealizedUnit();
    ASSERT_TRUE(unit.has_value());
    auto const machine = firstMachineFor(unit->formatKey);
    ASSERT_TRUE(machine.has_value());
    std::string const unitExt =
        lowered(fs::path{unit->source}.extension().generic_string());
    auto const claiming = languageClaiming(unitExt);
    ASSERT_TRUE(claiming.has_value());

    // This file's own string, and asserted UNKNOWN to the corpus before it is
    // used — a hard-coded extension that some language quietly started claiming
    // would turn this into a two-claimant test wearing a zero-claimant name.
    std::string const strangerExt = ".lanebnolang";
    ASSERT_FALSE(languageClaiming(strangerExt).has_value())
        << "'" << strangerExt << "' must be claimed by NO shipped language for "
           "this to be the zero-claimant arm";

    ScratchDir scratch{Location::Temp, "shipped-source-language-resolution"};
    auto const staged = stageConfigTree(scratch.path() / "tree");
    ASSERT_TRUE(staged.has_value());

    // Re-extension the realized unit in the staged tree, and re-point every
    // descriptor that names it. R1 (a realization names an existing file) still
    // holds, so the refusal under test is the LANGUAGE one and not a missing
    // body.
    fs::path const oldSource = staged->configDir / unit->source;
    fs::path       newSource = oldSource;
    newSource.replace_extension(strangerExt);
    std::error_code ec;
    fs::rename(oldSource, newSource, ec);
    ASSERT_FALSE(ec) << "cannot re-extension the staged runtime unit "
                     << oldSource.generic_string() << ": " << ec.message();

    std::string const declaredOld = "\"" + unit->source + "\"";
    std::string       declaredNew = unit->source;
    declaredNew = declaredNew.substr(0, declaredNew.rfind('.')) + strangerExt;
    declaredNew = "\"" + declaredNew + "\"";
    std::size_t rewritten = 0;
    for (fs::recursive_directory_iterator it{staged->configDir / "shippedLibs", ec},
         end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        if (it->path().extension() != ".json") continue;
        std::string text = readWhole(it->path());
        auto const  at   = text.find(declaredOld);
        if (at == std::string::npos) continue;
        for (std::size_t p = at; p != std::string::npos;
             p = text.find(declaredOld, p + declaredNew.size()))
            text.replace(p, declaredOld.size(), declaredNew);
        ASSERT_TRUE(writeWhole(it->path(), text))
            << "cannot re-write staged descriptor " << it->path().generic_string();
        ++rewritten;
    }
    ASSERT_GT(rewritten, 0u)
        << "no staged descriptor names " << declaredOld
        << ", so the re-extensioned unit would never be realized and this test "
           "would refuse nothing";

    ScopedEnv      env{"DSS_CONFIG_ROOT", staged->root.string()};
    CompileOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(runOneCompile(scratch.path() / "work", *claiming,
                                          machine->target + ":" + machine->archiveFormat,
                                          outcome));
    EXPECT_NE(outcome.rc, 0)
        << "a realized unit whose extension no shipped language claims has no "
           "front end, and the driver must say so rather than skip it"
        << renderErrors(outcome.rep);
    EXPECT_TRUE(anyErrorContains(
        outcome.rep, "no shipped language claims the extension '" + strangerExt))
        << "the refusal must be the ZERO-CLAIMANT diagnostic, naming the extension"
        << renderErrors(outcome.rep);
}

// ═══ THE COUNT ═══════════════════════════════════════════════════════════════

// ★★ THE PROPERTY THIS CHANGE IS ABOUT. Resolving the front end for a realized
// unit must not construct the whole language corpus. Measured, not inferred: the
// number of `GrammarSchema` objects built during one compile is bracketed by two
// probe loads, and the staged corpus is nonced so every load in this test is a
// real construction rather than a memo hit.
TEST(ShippedSourceLanguageResolution, ResolvingOneFrontEndDoesNotBuildEveryShippedGrammar) {
    ASSERT_FALSE(configRoot().empty());
    auto const unit = firstRealizedUnit();
    ASSERT_TRUE(unit.has_value());
    auto const machine = firstMachineFor(unit->formatKey);
    ASSERT_TRUE(machine.has_value());
    std::string const unitExt =
        lowered(fs::path{unit->source}.extension().generic_string());
    auto const claiming = languageClaiming(unitExt);
    ASSERT_TRUE(claiming.has_value());

    std::string const probeText = smallestLanguageDocumentText();
    ASSERT_FALSE(probeText.empty())
        << "no shipped language document could be read to build the construction "
           "probe from, so nothing below is measurable";

    ScratchDir scratch{Location::Temp, "shipped-source-language-resolution"};
    auto const staged = stageConfigTree(scratch.path() / "tree");
    ASSERT_TRUE(staged.has_value());
    ASSERT_TRUE(nonceEveryLanguageDocument(staged->configDir / "sources", nextNonce()));

    auto const documents =
        shippedSchemaNames(staged->configDir / "sources", kLangSuffix);
    ASSERT_GE(documents.size(), 2u)
        << "with fewer than two shipped language documents, 'did not build them "
           "all' is not a distinguishable claim";

    ScopedEnv env{"DSS_CONFIG_ROOT", staged->root.string()};

    auto const before = constructionProbe(probeText);
    ASSERT_TRUE(before.has_value()) << "the construction probe failed to load";

    CompileOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(runOneCompile(scratch.path() / "work", *claiming,
                                          machine->target + ":" + machine->archiveFormat,
                                          outcome));
    EXPECT_EQ(outcome.rc, 0) << renderErrors(outcome.rep);

    auto const after = constructionProbe(probeText);
    ASSERT_TRUE(after.has_value()) << "the construction probe failed to load";
    ASSERT_GT(*after, *before)
        << "the two probes minted the same schema id, so the counter this "
           "measurement rests on is not monotonic per construction — the number "
           "below would mean nothing";
    // Minus the second probe itself, which is one of the constructions counted.
    std::uint32_t const constructions = *after - *before - 1;

    // Non-vacuity: the front end MUST have been built. A zero here means every
    // load answered from a memo — the nonce failed — and the upper bound below
    // would then be green for the wrong reason.
    EXPECT_GE(constructions, 1u)
        << "the compile constructed NO grammar at all, so the count is not a "
           "measurement of this compile";
    EXPECT_LT(constructions, documents.size())
        << "compiling ONE file constructed " << constructions
        << " grammars against a corpus of " << documents.size()
        << " shipped language documents. The extension⇒language resolution is "
           "reading a single declared array per candidate; building a grammar to "
           "read it is the defect "
           "D-DRIVER-SHIPPED-SOURCE-RESOLUTION-COMPILES-EVERY-SHIPPED-GRAMMAR "
           "closed. (✔MEASURED with the fix in place: 1.)";
}
