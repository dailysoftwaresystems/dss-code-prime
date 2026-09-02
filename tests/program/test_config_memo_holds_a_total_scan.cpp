// D-CONFIG-MEMO-CAPACITY-IS-HALF-THE-SHIPPED-CORPUS-SO-A-TOTAL-SCAN-THRASHES-IT
//
// ═══ THE DEFECT, AND WHY IT HAD NO SYMPTOM ═══════════════════════════════════
//
// `ConfigDocumentMemoStore` exists so a shipped config document is BUILT ONCE
// per process. Its bound was 16 entries, FIFO, set from a stated premise: *"the
// real working set is the handful of documents one invocation touches (the
// shipped-language scan is six)"*.
//
// ✔MEASURED 2026-08-31 (cycle P46, lane `dg`), Windows Debug, at `f865897c`:
// two PRODUCTION paths scan the object-format class in FULL, in one
// uninterruptible loop, because each is proving UNIQUENESS and a uniqueness
// proof cannot early-exit —
//   * `runtime::resolveArchiveSiblingFormat`, once per BUILD (the root's and
//     every dependency sub-build's);
//   * `Resolver::shippedFormats_`, once per dependency resolve.
// The corpus is 24 `.format.json` documents. A 24-document loop through a
// 16-slot store evicts EIGHT OF ITS OWN ENTRIES before it finishes, so its hit
// rate is ZERO however often it runs — and it also evicts everything loaded
// BEFORE it, including the 506 KB language document, the most expensive build in
// the process.
//
// ✔THE NUMBER, `--time`'s `build-config` runs (the phase whose `runs` IS the
// memo's miss count), on a project-mode compile with `staticlib` dependencies:
//
//     dependsOn      build-config runs
//     (empty)                     29
//     K=1, T=1                   105
//     K=2, T=1                   131
//     K=4, T=1                   183
//     K=8, T=1                   287       ⇒ +26 per dependency
//     K=1, T=4                   181       ⇒ +25 per consumer target
//
// ★★★ SO THE DEFECT WAS NEVER "THE NUMBER IS TOO SMALL". It is that a COUNT
// bound has a SILENT CLIFF: the store degrades from a 100% hit rate to a 0% one
// the moment one document class outgrows it, with no diagnostic and no symptom
// other than the compiler getting slower. Nothing watched the cliff, which is
// why it was crossed and stayed crossed.
//
// ═══ WHAT THIS FILE IS: THE THING THAT MAKES THE CLIFF LOUD ═════════════════
//
// The fix has two halves and only the second is durable:
//   1. LRU instead of FIFO, and a bound stated as a multiple of the WHOLE
//      shipped corpus rather than as a guess about one path's working set;
//   2. **this file**, which COUNTS the corpus on disk and goes RED when it no
//      longer fits with headroom. The 129th shipped document now reddens a test
//      in the commit that adds it, instead of silently halving the compiler.
//
// ⚠ NOTHING HERE SPELLS THE CAPACITY. `capacity()` is asked of the store and
// compared against a corpus this file has just counted — a re-spelled constant
// would go green the day someone changed one of the two and not the other,
// which is the same class of drift the defect itself is.
//
// ═══ THE INSTRUMENT: `PhaseTimers::read(BuildConfig).runs`, NOT A CLOCK ═════
//
// `build-config` is DEFINED as the work a memo hit skips (see
// `core/substrate/phase_timers.hpp`), so its `runs` IS the miss count — exact,
// deterministic and host-independent, where a wall-clock threshold on a drifting
// host is how a test starts passing for the wrong reason.

#include "core/substrate/phase_timers.hpp"
#include "core/types/config_document_memo.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using dss::DiagnosticReporter;
using dss::ObjectFormatSchema;
using dss::Program;
using dss::detail::ConfigDocumentMemoStore;
using dss::substrate::CompilePhase;
using dss::substrate::PhaseTimers;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace {

// Every shipped document, counted from the tree rather than from a table. The
// three classes are the three the memo has families for.
struct ShippedCorpus {
    std::size_t formats   = 0;
    std::size_t languages = 0;
    std::size_t targets   = 0;
    [[nodiscard]] std::size_t total() const noexcept {
        return formats + languages + targets;
    }
};

[[nodiscard]] std::size_t countSuffix(fs::path const&  dir,
                                      std::string_view suffix) {
    std::size_t     n = 0;
    std::error_code ec;
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().filename().string().ends_with(suffix)) ++n;
    }
    return n;
}

[[nodiscard]] ShippedCorpus measureShippedCorpus() {
    ShippedCorpus corpus;
    auto const    formats   = dss::findShippedConfigDir("object-formats");
    auto const    languages = dss::findShippedConfigDir("sources");
    auto const    targets   = dss::findShippedConfigDir("targets");
    if (!formats || !languages || !targets) {
        ADD_FAILURE() << "could not locate the shipped config directories; set "
                         "DSS_CONFIG_ROOT or run from inside the source tree";
        return corpus;
    }
    corpus.formats   = countSuffix(*formats, ".format.json");
    corpus.languages = countSuffix(*languages, ".lang.json");
    corpus.targets   = countSuffix(*targets, ".target.json");
    return corpus;
}

void write(fs::path const& p, std::string_view text) {
    std::ofstream out{p, std::ios::binary};
    out << text;
}

} // namespace

// ═══ THE DURABLE HALF: the cliff is now LOUD ════════════════════════════════
//
// If this goes red, the shipped config corpus has outgrown the memo and EVERY
// build in the tree just got slower for reasons no other test can see. The fix
// is to raise `kCapacity` in `core/types/config_document_memo.cpp` — and to read
// the capacity note there first, because the number is derived, not chosen.
TEST(ConfigMemoHoldsATotalScan, TheCapacityExceedsTheWholeShippedCorpus) {
    ShippedCorpus const corpus = measureShippedCorpus();
    ASSERT_GT(corpus.formats, 0u) << "no shipped object formats were found";
    ASSERT_GT(corpus.languages, 0u) << "no shipped languages were found";

    // ★ THE RELATION, NOT THE VALUE. A total scan over the LARGEST class must
    // fit with every other class still resident, or a scan displaces documents
    // the build is still using — which is the whole defect. The 2x is headroom
    // against the corpus growing between one commit and the next.
    EXPECT_GE(ConfigDocumentMemoStore::capacity(), corpus.total() * 2)
        << "the shipped corpus is " << corpus.total() << " documents ("
        << corpus.formats << " formats, " << corpus.languages << " languages, "
        << corpus.targets << " targets) and the memo holds "
        << ConfigDocumentMemoStore::capacity()
        << ". A total object-format scan will now thrash it to a 0% hit rate "
           "and evict the language document with it.";
}

// ═══ THE SCAN ACTUALLY FITS — RESIDENCY, NOT A HIT COUNT ════════════════════
//
// ⚠ Asserted on RESIDENCY (`size()`) rather than on hits, and the difference is
// the point: a hit count cannot distinguish "the scan fit" from "the scan ran
// twice and the second pass hit what the first left". Residency after ONE scan
// answers the question directly.
TEST(ConfigMemoHoldsATotalScan, ATotalObjectFormatScanStaysResident) {
    auto const dir = dss::findShippedConfigDir("object-formats");
    ASSERT_TRUE(dir.has_value());

    ConfigDocumentMemoStore::clear();
    std::size_t     loaded = 0;
    std::error_code ec;
    for (fs::directory_iterator it{*dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (!it->path().filename().string().ends_with(".format.json")) continue;
        // The REAL load path, exactly as `resolveArchiveSiblingFormat` drives
        // it — `loadFromFile`, by path, one document at a time, no early exit.
        if (ObjectFormatSchema::loadFromFile(it->path()).has_value()) ++loaded;
    }
    ASSERT_GT(loaded, 0u);
    EXPECT_GE(ConfigDocumentMemoStore::size(), loaded)
        << "a total scan of " << loaded
        << " object-format documents left only "
        << ConfigDocumentMemoStore::size()
        << " resident — the scan evicted its own entries, so running it again "
           "rebuilds every one of them";

    // ★ AND THE SECOND SCAN COSTS NOTHING. This is the property the whole memo
    // exists for, measured on the path that was defeating it.
    PhaseTimers::reset();
    for (fs::directory_iterator it{*dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (!it->path().filename().string().ends_with(".format.json")) continue;
        (void)ObjectFormatSchema::loadFromFile(it->path());
    }
    EXPECT_EQ(PhaseTimers::read(CompilePhase::BuildConfig).runs, 0u)
        << "a repeated total scan rebuilt "
        << PhaseTimers::read(CompilePhase::BuildConfig).runs
        << " documents; every one of them was already in the memo when the "
           "scan started";
}

// ═══ ★★ AND THE EXPENSIVE DOCUMENT SURVIVES A SCAN OF A DIFFERENT CLASS ═════
//
// This is the half LRU buys that a bigger FIFO would not. Under FIFO the
// language document — loaded FIRST, used throughout, and by far the most
// expensive to rebuild — was the FIRST thing an object-format scan evicted.
TEST(ConfigMemoHoldsATotalScan, AFormatScanDoesNotEvictTheLanguageDocument) {
    auto const formatsDir = dss::findShippedConfigDir("object-formats");
    auto const langDir    = dss::findShippedConfigDir("sources");
    ASSERT_TRUE(formatsDir.has_value());
    ASSERT_TRUE(langDir.has_value());

    // The language is DISCOVERED, not spelled: the first shipped `.lang.json`
    // in the tree. Naming one here would make this case assert on the corpus
    // this host happens to ship rather than on the policy.
    std::string     languageName;
    std::error_code ec;
    for (fs::directory_iterator it{*langDir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        std::string const file = it->path().filename().string();
        constexpr std::string_view kSuffix = ".lang.json";
        if (!std::string_view{file}.ends_with(kSuffix)) continue;
        languageName = file.substr(0, file.size() - kSuffix.size());
        break;
    }
    ASSERT_FALSE(languageName.empty());

    ConfigDocumentMemoStore::clear();
    ASSERT_TRUE(dss::GrammarSchema::loadShipped(languageName).has_value());

    // A TOTAL scan of a DIFFERENT class, between the two language loads.
    for (fs::directory_iterator it{*formatsDir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (!it->path().filename().string().ends_with(".format.json")) continue;
        (void)ObjectFormatSchema::loadFromFile(it->path());
    }

    PhaseTimers::reset();
    ASSERT_TRUE(dss::GrammarSchema::loadShipped(languageName).has_value());
    EXPECT_EQ(PhaseTimers::read(CompilePhase::BuildConfig).runs, 0u)
        << "the language document was rebuilt after an object-format scan — "
           "the scan displaced the single most expensive entry in the store, "
           "which is exactly what FIFO did and LRU must not";
}

// ═══ ★★ THE LRU POLICY ITSELF, AND WHY IT NEEDS ITS OWN CASE ════════════════
//
// ⚠ AN HONEST LIMIT ON THE CASE ABOVE, STATED RATHER THAN LEFT TO BE
// DISCOVERED: `AFormatScanDoesNotEvictTheLanguageDocument` does NOT pin LRU at
// the SHIPPED capacity. With the bound at 4x the corpus nothing is evicted at
// all, so that case would stay green over a store that had been reverted to
// FIFO — it pins the OUTCOME (the language document survives), which the raised
// capacity alone is enough to deliver. A policy that is only load-bearing once
// the bound binds is a policy nothing watches, and this project has a standing
// lesson about exactly that shape.
//
// ⇒ this case makes the bound BIND, by flooding the store past its own
// capacity with synthetic entries, and then asks the one question that
// separates the two policies: after a flood, is the entry that was USED
// THROUGHOUT still there?
//
//   FIFO — evicts by INSERTION order, so the oldest entry goes first. The entry
//          used throughout is the oldest. It is evicted. RED.
//   LRU  — evicts by USE order. The entry used throughout is the newest by use.
//          It survives. GREEN.
//
// ★ It drives the store DIRECTLY rather than through a loader, and that is the
// right call here and the wrong call for every other case in this file: the
// subject is the EVICTION POLICY, which is a property of the store and not of
// any document. Manufacturing 100+ distinct real config documents to reach the
// same question would test the loader's tolerance of synthetic input instead.
TEST(ConfigMemoHoldsATotalScan, TheLeastRecentlyUSEDIsEvicted) {
    using dss::detail::ConfigDocumentDependency;
    using dss::detail::ConfigDocumentMemo;

    // A real schema so the stored `shared_ptr` is a live object; WHICH schema is
    // irrelevant — the store is type-erased and this case never dereferences it.
    auto const dir = dss::findShippedConfigDir("object-formats");
    ASSERT_TRUE(dir.has_value());
    std::shared_ptr<ObjectFormatSchema const> anySchema;
    std::error_code                           ec;
    for (fs::directory_iterator it{*dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        if (!it->path().filename().string().ends_with(".format.json")) continue;
        auto loaded = ObjectFormatSchema::loadFromFile(it->path());
        if (loaded.has_value()) { anySchema = *loaded; break; }
    }
    ASSERT_NE(anySchema, nullptr);

    std::size_t const capacity = ConfigDocumentMemoStore::capacity();
    ASSERT_GT(capacity, 2u);

    ConfigDocumentMemoStore::clear();
    // ⓘ A synthetic digest, and an EMPTY dependency ledger so a lookup cannot
    // miss for a reason that has nothing to do with eviction: a recorded
    // dependency is re-read from disk on every lookup, and a synthetic path
    // would be unreadable and reject the entry before the policy was consulted.
    auto const synthetic = [](std::size_t n) {
        std::string d = std::to_string(n);
        d.insert(d.begin(), 64 - d.size(), '0');
        return d;
    };
    constexpr std::string_view kKeptLabel = "kept";
    ConfigDocumentMemo<ObjectFormatSchema>::store(
        std::string{kKeptLabel}, synthetic(0),
        std::vector<ConfigDocumentDependency>{},
        std::const_pointer_cast<ObjectFormatSchema>(anySchema));

    // Flood the store, TOUCHING the kept entry between every insertion. Under
    // LRU those touches keep it newest-by-use; under FIFO they change nothing,
    // because FIFO cannot see a use at all.
    for (std::size_t i = 1; i <= capacity; ++i) {
        ConfigDocumentMemo<ObjectFormatSchema>::store(
            "flood-" + std::to_string(i), synthetic(i),
            std::vector<ConfigDocumentDependency>{},
            std::const_pointer_cast<ObjectFormatSchema>(anySchema));
        (void)ConfigDocumentMemo<ObjectFormatSchema>::lookup(kKeptLabel,
                                                            synthetic(0));
    }

    EXPECT_NE(ConfigDocumentMemo<ObjectFormatSchema>::lookup(kKeptLabel,
                                                            synthetic(0)),
              nullptr)
        << "the entry used on every iteration was evicted by " << capacity
        << " insertions — the store is evicting by INSERTION order (FIFO), so "
           "the document a build uses throughout is the first one it loses, "
           "which is exactly what a total scan over another class did to the "
           "language document";

    // ★ THE CONTROL, and without it the assertion above is satisfied by a store
    // that never evicts anything: the FIRST flood entry, never touched again,
    // MUST be gone. That is what proves the bound is real and that the case
    // above measured a policy rather than an absence of pressure.
    EXPECT_EQ(ConfigDocumentMemo<ObjectFormatSchema>::lookup("flood-1",
                                                            synthetic(1)),
              nullptr)
        << "nothing was evicted at all, so this case cannot distinguish LRU "
           "from FIFO and is not testing the policy";
    ConfigDocumentMemoStore::clear();
}

// ═══ THE PRODUCTION PATH, END TO END ════════════════════════════════════════
//
// ★ DRIVEN THROUGH `Program::compileProject`, WHICH IS THE RESOLVER'S REAL
// INPUT PATH. The per-build format scans this defect is about happen inside the
// driver and inside every dependency sub-build; a hand-built resolve request
// would exercise none of them.
//
// ⚠ THE ASSERTION IS A RELATION AGAINST A CONTROL, NEVER A SPELLED COUNT. The
// absolute number moves with the corpus and with the schedule; what must hold is
// that adding a DEPENDENCY does not multiply the config rebuilds — which is the
// property, and the one that was false.
TEST(ConfigMemoHoldsATotalScan, ADependencyDoesNotMultiplyConfigRebuilds) {
    // ⚠ `Location::InsideRepo`, and `ScratchDir` REFUSES the alternative with a
    // message rather than letting it fail obscurely: a `Location::Temp` cwd is
    // outside the repo tree, so the schema loader's cwd-walk cannot find
    // `src/dss-config/`. Every other case in this file stays on `Temp` because
    // none of them drives a compile.
    ScratchDir scratch{Location::InsideRepo, "config-memo-scan"};
    fs::path const root = scratch.path();
    // ⚠ REQUIRED, not tidiness: `compileProject` resolves the ROOT manifest's
    // relative `sources[]` against the PROCESS working directory (its own
    // docblock says so), while a DEPENDENCY's sources resolve against its
    // manifest's directory. Without this the dependency built and the root did
    // not — which is a shape that still produces plausible phase counts, so the
    // `subjectRc` assertion below is what turned it into a failure instead of a
    // quiet mis-measurement.
    scratch.useAsCwd();

    // The dependency: a `staticlib` project whose source carries a private
    // header, i.e. lane `dc`'s fixture shape.
    fs::path const dep = root / "foldlib";
    fs::create_directories(dep);
    write(dep / "fold_impl.h", "#define DSS_FOLD_BIAS 2\n");
    write(dep / "fold.c",
          "#include \"fold_impl.h\"\n"
          "int dss_fold_twice(int v) { return v + v + DSS_FOLD_BIAS; }\n");
    write(dep / ".dss-project.json",
          R"({"language":"c","artifactProfile":"staticlib",)"
          R"("artifactName":"fold",)"
          R"("targets":["x86_64:elf64-x86_64-linux-staticlib"],)"
          R"("sources":["fold.c"]})");

    write(root / "main.c",
          "extern int dss_fold_twice(int);\n"
          "int main(int argc, char** argv) { (void)argv; "
          "return dss_fold_twice(argc) & 0x7f; }\n");

    auto const manifest = [&](bool withDependency) {
        std::string m =
            R"({"language":"c","artifactProfile":"cli","artifactName":"consumer",)"
            R"("targets":["x86_64:elf64-x86_64-linux-exec"],)"
            R"("sources":["main.c"])";
        if (withDependency) m += R"(,"dependsOn":[{"path":"foldlib"}])";
        m += "}";
        return m;
    };

    // A PRIVATE runtime-object-cache root, so neither arm reads or writes the
    // developer's real cache — a leftover entry there would change how much
    // config work each arm does and make the comparison measure the cache.
    ScopedEnv const cacheRoot{"DSS_RUNTIME_CACHE_DIR",
                              (root / "rtcache").string()};

    // ── CONTROL: no `dependsOn`. Its `main.c` is undefined-symbol-free only
    // WITH the dependency, so this arm is expected to FAIL TO LINK — and that is
    // fine, because the subject is the CONFIG work done on the way there, which
    // is complete before the link is attempted. Asserting on the return code
    // would be asserting on something this test is not about.
    write(root / ".dss-project.json", manifest(false));
    PhaseTimers::reset();
    {
        Program prog;
        prog.setOutputDir(root / "out-control");
        DiagnosticReporter rep{};
        (void)prog.compileProject((root / ".dss-project.json").string(), rep);
    }
    std::uint64_t const controlBuilds =
        PhaseTimers::read(CompilePhase::BuildConfig).runs;

    // ── SUBJECT: the same project WITH the dependency.
    write(root / ".dss-project.json", manifest(true));
    PhaseTimers::reset();
    int subjectRc = 0;
    {
        Program prog;
        prog.setOutputDir(root / "out-subject");
        DiagnosticReporter rep{};
        subjectRc = prog.compileProject((root / ".dss-project.json").string(), rep);
    }
    std::uint64_t const subjectBuilds =
        PhaseTimers::read(CompilePhase::BuildConfig).runs;
    ASSERT_EQ(subjectRc, 0) << "the dependency arm must actually build, or the "
                               "config work being counted is not the config "
                               "work a real dependency resolve does";

    // ⚠ Both arms run in ONE process, so the memo is WARM for the subject —
    // which is the point. The claim is that a dependency's sub-builds re-derive
    // (almost) nothing, not that the process derives nothing at all.
    //
    // The bound is stated against the CORPUS rather than as a ratio: a
    // dependency legitimately introduces documents the control never touched
    // (the dependency's own derived staticlib format), so a strict `<=` would be
    // wrong. What must NOT happen is the whole corpus being rebuilt again.
    ShippedCorpus const corpus = measureShippedCorpus();
    EXPECT_LT(subjectBuilds, controlBuilds + corpus.formats)
        << "adding one dependency cost " << (subjectBuilds - controlBuilds)
        << " extra config-schema BUILDS against a shipped format corpus of "
        << corpus.formats
        << ". A dependency's sub-builds re-scan the object-format directory; "
           "every one of those loads must be served by the memo, so the extra "
           "builds must be the dependency's own new documents and not the "
           "corpus over again.";
}
