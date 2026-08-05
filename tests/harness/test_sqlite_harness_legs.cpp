// D-HARNESS-CROSS-HOST-ANY-TARGET item (2) — THE GATE ON THE DE-HOST-LOCKED
// SQLITE HARNESS DRIVERS.
//
// THE REQUIREMENT (user, 2026-07-25): "build ANY target inside ANY host, this
// MUST work." DSS-the-compiler already satisfies it — target selection is
// config-driven, and a Windows host has produced pe64 + elf64 + mach-o sqlite3
// binaries that RAN on their respective machines. The DRIVERS did not:
// `build-and-test.sh` derived its leg list from `uname` and `build-and-test.ps1`
// refused to start unless `$IsWindows`, so each conflated HOST with TARGET.
//
// WHAT THIS FILE ASSERTS, AND WHY IT IS HERE RATHER THAN IN THE DRIVERS.
// The drivers are shell/PowerShell, run by hand, take hours, and are NOT part of
// ctest — nothing in the gate has ever had an opinion about them. Their leg
// decision is now a single host-independent resolver (real-examples/c/sqlite/
// harness_legs.py) over a single declared catalogue (legs.json), and BOTH are
// cheap to interrogate. So the gate can hold the architecture to account on
// every leg, every run, in ~1 second:
//
//   1. VOCABULARY PIN — the resolver's verdict names are EXACTLY
//      `armVerdictName()` over `kAllArmVerdicts`, in order. The sqlite harness
//      does not get its own private words for "did not run".
//   2. HOST-INVARIANCE — for nine simulated hosts (including two the project
//      has never run on), the set of legs planned for BUILD is identical and
//      complete. This is the requirement restated as an executable property.
//   3. EXECUTION CAPABILITY IS THE ONLY THING A HOST CHANGES — the run plan
//      moves between native / launched / a NAMED skip, and never removes a leg.
//   4. NO SILENT SKIP — every non-run carries a verdict from (1) and a reason.
//   5. THE DRIVERS ACTUALLY CONSULT IT — both scripts invoke the resolver, and
//      neither carries a hardcoded target spec or a host-keyed leg gate. This is
//      the anti-regression pin: re-adding `if (-not $IsWindows) { Die ... }` or
//      a second `$Spec = '...'` literal reds here.
//   6. LAUNCHER VOCABULARY AGREEMENT — where the catalogue and the examples
//      corpus both declare a launcher for the same (targetArch, hostOs), they
//      must spell it the same way, so a reader cannot conclude the project has
//      two qemus.
//   7. EVERY LEG COMPILES AGAINST A THIRD-PARTY HEADER CONFIGURED FOR ITS OWN
//      TARGET (D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED). One staged zconf.h
//      used to serve all five legs, carrying the pe leg's `Z_HAVE_UNISTD_H`
//      answer — so the .ps1 refused every other leg and the .sh, which never
//      applied the flip at all, could not build its pe leg. The catalogue now
//      declares each target's answer, the drivers stage one zinc/ per
//      recipeTransform through stage-zinc.py, and these tests hold all three
//      pieces to account: the declaration matches the target, the stage plan is
//      host-free, and the staging tool actually writes what was declared.
//
// PYTHON IS A HARD DEPENDENCY, NOT AN OPTIONAL ONE. Both drivers already
// `ensure_cmd python3` / gate on python3 before they will run, and the manifest
// generators are python. A machine without it cannot run the sqlite harness at
// all, so this test FAILS rather than skipping — a skip here would be the exact
// no-verdict defect the ledger it pins was created to end.

#include "arm_verdict_ledger.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using dss::test_support::ArmVerdict;
using dss::test_support::armVerdictName;
using dss::test_support::findOnPath;
using dss::test_support::kAllArmVerdicts;
using json = nlohmann::json;

namespace {

// The repo root, through the ONE test-side resolver (`repo_root.hpp`:
// `$DSS_CONFIG_ROOT` → the root CMake bakes in → a 12-hop cwd walk, every
// candidate validated as a directory containing `src/dss-config`).
//
// This used to be a private copy that diverged in two ways that matter: it
// took `DSS_CONFIG_ROOT` UNVALIDATED (so a stale export poisoned the whole
// file rather than falling through), and it had no knowledge of the baked
// `DSS_TEST_REPO_ROOT`, so running this binary directly — outside ctest, which
// always exports the variable — resolved nothing out-of-tree and returned an
// EMPTY path that then composed into `""/real-examples/c/sqlite`. The shared
// resolver throws instead, which GoogleTest reports as a failure of the one
// test that asked, naming all three sources it tried.
using dss::test::repoRoot;

[[nodiscard]] fs::path harnessDir() {
    return repoRoot() / "real-examples" / "c" / "sqlite";
}

// The interpreter. Resolved to a FULL PATH because `runBinary`'s Windows arm
// passes it as CreateProcessA's lpApplicationName, which does not search PATH.
[[nodiscard]] std::string pythonPath() {
    for (char const* name : {"python3", "python"}) {
        auto const p = findOnPath(name);
        if (!p.empty()) return p;
    }
    return {};
}

struct PyRun {
    bool        spawned = false;
    std::uint32_t exitCode = 0;
    std::string output;
    std::string diagnostic;
};

// Run the resolver. `trailing` is the LAST argv element and must be a real,
// existing file: `runBinary`'s POSIX arm chmods its `binaryPath` to 0755 before
// spawning, so pointing it at a repo file would mutate a tracked mode bit. Every
// call therefore ends in `--catalogue <scratch copy of legs.json>`, which also
// means the resolver is exercised against a COPY of the shipped catalogue rather
// than a fixture invented here.
[[nodiscard]] PyRun runResolver(std::vector<std::string> const& args,
                                fs::path const&                 catalogueCopy) {
    PyRun out;
    auto const py = pythonPath();
    if (py.empty()) {
        out.diagnostic =
            "python3 (or python) is not on PATH. The sqlite harness drivers"
            " hard-require it (build-and-test.sh does `ensure_cmd python3`;"
            " build-and-test.ps1 dies without it), so this is a real"
            " unmet dependency and not a reason to skip the check.";
        return out;
    }
    std::vector<std::string> prefix{py,
                                    (harnessDir() / "harness_legs.py").string()};
    for (auto const& a : args) prefix.push_back(a);
    prefix.push_back("--catalogue");
    auto const res = dss::test_support::runBinary(
        catalogueCopy, std::chrono::seconds{120}, /*captureStdout=*/true, prefix);
    out.spawned    = res.spawned && !res.timedOut;
    out.exitCode   = res.exitCode;
    out.output     = res.capturedStdout;
    out.diagnostic = res.diagnostic;
    return out;
}

// One scratch copy of the shipped catalogue, made once and shared. A per-test
// copy would be tidier in isolation and would also make every test pay the file
// copy; the catalogue is read-only to the resolver, so one copy is honest.
class HarnessLegs : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        scratch_ = new dss::test_support::ScratchDir(
            dss::test_support::Location::Temp, "sqlite-harness-legs");
        auto const src = harnessDir() / "legs.json";
        ASSERT_TRUE(fs::exists(src))
            << "the sqlite harness leg catalogue is missing: " << src
            << "\nIt is the declared, host-free answer to 'which targets does"
               " this harness build?' — without it the drivers have nothing to"
               " read and would fall back to keying on the host.";
        catalogue_ = scratch_->path() / "legs.json";
        std::error_code ec;
        fs::copy_file(src, catalogue_, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "could not stage the catalogue: " << ec.message();
    }
    static void TearDownTestSuite() {
        delete scratch_;
        scratch_ = nullptr;
    }

    [[nodiscard]] static PyRun run(std::vector<std::string> const& args) {
        return runResolver(args, catalogue_);
    }

    static dss::test_support::ScratchDir* scratch_;
    static fs::path                       catalogue_;
};

dss::test_support::ScratchDir* HarnessLegs::scratch_   = nullptr;
fs::path                       HarnessLegs::catalogue_ = {};

[[nodiscard]] std::vector<std::string> splitLines(std::string const& s) {
    std::vector<std::string> out;
    std::istringstream in{s};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// Every simulated host the property must hold for. The last three are hosts
// this project has NEVER run on: an unrecognised OS, an unrecognised arch, and
// both at once. They are here because "build nothing on a machine I do not
// recognise" is the host-locking defect wearing a different hat.
struct SimHost {
    char const* os;
    char const* arch;
};
constexpr SimHost kSimHosts[] = {
    {"linux", "x86_64"},  {"linux", "arm64"},
    {"windows", "x86_64"}, {"windows", "arm64"},
    {"darwin", "arm64"},  {"darwin", "x86_64"},
    {"unknown", "x86_64"}, {"linux", "riscv64"}, {"unknown", "unknown"},
};

[[nodiscard]] std::string fileText(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The driver's LIVE lines — comments removed.
//
// ★ THIS IS LOAD-BEARING, and it was learned the hard way inside this cycle: the
// first version of `TheDriversAreNotHostKeyed` searched the RAW text for the
// removed host gate, and it went red against a correctly-converted driver —
// because the driver's header now DOCUMENTS the gate it removed ("Until TF-C114
// this driver opened with `if (-not $IsWindows) { Die ... }`"). A pin that
// punishes writing down what you removed teaches the wrong lesson and would
// have been "fixed" by deleting the explanation. Comments are prose ABOUT the
// code; only the code is the code.
//
// `#` line comments cover both shells; `<# … #>` block comments are PowerShell
// only and are passed over as well. Trailing comments after code are left in
// place deliberately — stripping them needs a quote-state parser, and a needle
// that only ever appears in a trailing comment is not a shape these rules look
// for.
[[nodiscard]] std::vector<std::string> liveLines(fs::path const& p,
                                                 bool             powershell) {
    std::vector<std::string> out;
    std::istringstream in{fileText(p)};
    std::string line;
    bool inBlockComment = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto const first = line.find_first_not_of(" \t");
        std::string const trimmed =
            first == std::string::npos ? std::string{} : line.substr(first);
        if (powershell) {
            if (inBlockComment) {
                if (trimmed.find("#>") != std::string::npos) {
                    inBlockComment = false;
                }
                continue;
            }
            if (trimmed.rfind("<#", 0) == 0) {
                if (trimmed.find("#>") == std::string::npos) {
                    inBlockComment = true;
                }
                continue;
            }
        }
        if (trimmed.rfind("#", 0) == 0) continue;
        out.push_back(line);
    }
    return out;
}

// A scratch copy of the shipped catalogue with one edit applied, for the tests
// that must witness a LINT REFUSAL. Mutating the shipped file is not an option
// and inventing a fixture catalogue would let the mutation tests pass against a
// document the drivers never read, so each one starts from the real thing.
class MutatedCatalogue {
public:
    MutatedCatalogue(fs::path const& source, char const* tag)
        : dir_{dss::test_support::Location::Temp, tag} {
        doc_  = json::parse(fileText(source));
        path_ = dir_.path() / "legs.json";
    }
    [[nodiscard]] json& doc() { return doc_; }
    // Write and return the path. Called once the caller has edited `doc()`.
    [[nodiscard]] fs::path const& commit() {
        std::ofstream out(path_, std::ios::binary);
        out << doc_.dump(2) << '\n';
        out.close();
        return path_;
    }
    // The first launcher entry of the named leg — every mutation below targets
    // one, and finding it by label keeps the tests readable when the catalogue
    // grows a leg.
    [[nodiscard]] json& firstLauncherOf(std::string const& label) {
        for (auto& leg : doc_.at("legs")) {
            if (leg.at("label").get<std::string>() != label) continue;
            EXPECT_FALSE(leg.at("launchers").empty())
                << label << " declares no launcher to mutate";
            return leg.at("launchers").at(0);
        }
        ADD_FAILURE() << "no leg labelled '" << label << '\'';
        return doc_;  // unreachable in a passing run
    }

private:
    dss::test_support::ScratchDir dir_;
    json                          doc_;
    fs::path                      path_;
};

// A `<arch>:<format>-exec` target spec written as a literal. The catalogue is
// the only place a leg's spec may be stated.
[[nodiscard]] bool mentionsTargetSpecLiteral(std::string const& line) {
    static constexpr std::string_view kArches[] = {"x86_64:", "arm64:"};
    for (auto const& arch : kArches) {
        std::size_t at = line.find(arch);
        while (at != std::string::npos) {
            auto const rest = line.find_first_of(" \t\"'`)]},;", at + arch.size());
            auto const tok  = line.substr(
                at, rest == std::string::npos ? std::string::npos : rest - at);
            if (tok.size() > 5 && tok.substr(tok.size() - 5) == "-exec") return true;
            at = line.find(arch, at + 1);
        }
    }
    return false;
}

// `x86_64:pe64-x86_64-windows-exec` -> `windows`. The python twin is
// `spec_target_os()`; the format names are `<container><bits>-<arch>-<os>-<kind>`.
[[nodiscard]] std::string specTargetOs(std::string const& spec) {
    auto const colon = spec.find(':');
    std::string const  fmt = colon == std::string::npos ? spec : spec.substr(colon + 1);
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= fmt.size(); ++i) {
        if (i == fmt.size() || fmt[i] == '-') {
            parts.push_back(fmt.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts.size() >= 3 ? parts[parts.size() - 2] : std::string{};
}

// The `./configure` block shape stage-zinc.py rewrites, reproduced as a FIXTURE.
// Not the machine's real zconf.h: this test must give the same answer on a box
// with no zlib installed, and the shipped header's exact line numbers are not
// this test's business. What IS its business is that the tool, driven by the
// SHIPPED catalogue, writes a per-stage header whose guard matches each leg's
// own target.
constexpr char const* kZconfFixture =
    "/* fixture standing in for a ./configure'd zconf.h */\n"
    "#ifndef ZCONF_H\n"
    "#define ZCONF_H\n"
    "#if 1    /* was set to #if 1 by ./configure */\n"
    "#  define Z_HAVE_UNISTD_H\n"
    "#endif\n"
    "\n"
    "#if 1    /* was set to #if 1 by ./configure */\n"
    "#  define Z_HAVE_STDARG_H\n"
    "#endif\n"
    "\n"
    "#ifndef Z_HAVE_UNISTD_H\n"
    "#  ifdef __WATCOMC__\n"
    "#    define Z_HAVE_UNISTD_H\n"
    "#  endif\n"
    "#endif\n"
    "#endif /* ZCONF_H */\n";

// The effective state of one guard in a staged zconf.h: the `#if N` line
// immediately above `#  define <guard>`. Mirrors what a preprocessor would do
// with the ./configure block, and deliberately does NOT reuse stage-zinc.py's
// own parser — a test that asked the tool to check itself would pass on any
// self-consistent mistake.
[[nodiscard]] int guardStateIn(fs::path const& zconf, std::string const& guard) {
    auto const lines = splitLines(fileText(zconf));
    std::string const needle = "#  define " + guard;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i] != needle) continue;
        auto const& prev = lines[i - 1];
        if (prev.rfind("#if 1", 0) == 0) return 1;
        if (prev.rfind("#if 0", 0) == 0) return 0;
        return -1;  // present but not a plain #if 1/#if 0
    }
    return -2;  // no ./configure site at all
}

}  // namespace

// ── 1. The vocabulary pin ──────────────────────────────────────────────────
//
// RED-ON-DISABLE: rename, reorder or drop any entry of `VERDICTS` in
// harness_legs.py (or add an ArmVerdict enumerator without mirroring it) and
// this fails naming both lists.
TEST_F(HarnessLegs, TheResolverSpeaksExactlyTheLedgersVerdictVocabulary) {
    auto const r = run({"--verdict-vocabulary"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;

    std::vector<std::string> want;
    for (ArmVerdict const v : kAllArmVerdicts) {
        want.emplace_back(armVerdictName(v));
    }
    auto const got = splitLines(r.output);
    EXPECT_EQ(got, want)
        << "the sqlite harness resolver's verdict names must be EXACTLY"
           " armVerdictName() over kAllArmVerdicts, in order — one vocabulary"
           " across all three corpus harnesses, or a reader has to learn two"
           " sets of words for the same fact.\n  ledger:   "
        << [&] {
               std::string s;
               for (auto const& w : want) s += w + " ";
               return s;
           }()
        << "\n  resolver: " << r.output;
}

// ── The resolver's own self-test + lint, run by the gate ───────────────────
//
// The drivers run these at their Step 0 so a broken plan refuses the run. That
// only helps someone who starts a driver. Running them here means a defect is
// caught by the ordinary gate instead of by a person three hours into a corpus.
TEST_F(HarnessLegs, TheResolverSelfTestPasses) {
    auto const r = run({"--self-test"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 0u) << r.output;
    EXPECT_NE(r.output.find("failed=0"), std::string::npos) << r.output;
}

TEST_F(HarnessLegs, TheLegCatalogueLintsClean) {
    auto const r = run({"--lint"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 0u) << r.output;
    EXPECT_NE(r.output.find("findings=0"), std::string::npos) << r.output;
}

// ── The pipe-buffer pin (D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS) ────
//
// This test is the reason that anchor exists. `runBinary` used to drain the
// capture pipe only AFTER the child exited, so a child that outgrew the pipe's
// kernel buffer (4 KiB on Windows) blocked in write() while the parent blocked
// in WaitForSingleObject — a mutual stall that ended with the parent KILLING a
// perfectly healthy child at the timeout. This resolver's JSON plan is the
// first caller big enough to trip it, and it did, immediately.
//
// RED-ON-DISABLE (measured, both numbers in the cycle report): restore the
// drain-after-exit ordering and the two plan tests time out at 120 s each.
TEST_F(HarnessLegs, ALargePlanIsCapturedWholeRatherThanDeadlockingTheHarness) {
    auto const r = run({"--plan", "--host-os", "linux", "--host-arch", "x86_64",
                        "--launchers-none", "--format", "json"});
    ASSERT_TRUE(r.spawned)
        << r.diagnostic
        << "\n(a TIMEOUT here reads as 'the child hung' and means the exact"
           " opposite: the child is blocked writing into a pipe nobody is"
           " reading)";
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    // 4096 = the Windows anonymous-pipe default. Anything at or below it would
    // pass with the broken ordering too, which would make this pin theatre.
    EXPECT_GT(r.output.size(), 4096u)
        << "the plan no longer exceeds one pipe buffer, so this test no longer"
           " witnesses the deadlock it was written for. Do not delete it —"
           " give it a payload that does.";
    json parsed;
    EXPECT_NO_THROW(parsed = json::parse(r.output))
        << "captured output is not whole JSON — a truncated capture is the"
           " other half of the same defect";
    EXPECT_TRUE(parsed.contains("legs"));
}

// ── 2 + 3 + 4. The requirement, as an executable property ──────────────────
//
// RED-ON-DISABLE: make the leg list depend on the host in ANY way — filter
// `legs` by `runOn` containing the host, gate a leg on the host arch, restore
// the old `if [[ $HOST_OS == linux ]]` shape inside the resolver — and the
// build-set comparison fails on at least six of the nine hosts.
TEST_F(HarnessLegs, EveryDeclaredLegIsBuiltOnEveryHost) {
    // The declared truth, read straight from the catalogue.
    auto const doc = json::parse(fileText(catalogue_));
    std::vector<std::string> declared;
    for (auto const& leg : doc.at("legs")) {
        declared.push_back(leg.at("label").get<std::string>());
    }
    ASSERT_GE(declared.size(), 2u)
        << "a catalogue with fewer than two legs cannot witness"
           " host-invariance of the leg set";

    std::set<std::string> vocabulary;
    for (ArmVerdict const v : kAllArmVerdicts) {
        vocabulary.emplace(armVerdictName(v));
    }

    for (auto const& host : kSimHosts) {
        // `--launchers-none` pins the plan: no PATH lookup, so the result is
        // identical on every machine that runs this test.
        auto const r = run({"--plan", "--host-os", host.os, "--host-arch",
                            host.arch, "--launchers-none", "--format", "json"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const plan = json::parse(r.output);

        std::vector<std::string> got;
        for (auto const& leg : plan.at("legs")) {
            got.push_back(leg.at("label").get<std::string>());
        }
        EXPECT_EQ(got, declared)
            << "host " << host.os << '/' << host.arch
            << " plans a DIFFERENT set of legs than the catalogue declares."
               " The leg set is a property of the harness, not of the machine"
               " (D-HARNESS-CROSS-HOST-ANY-TARGET item 2).";

        for (auto const& leg : plan.at("legs")) {
            auto const label = leg.at("label").get<std::string>();
            EXPECT_TRUE(leg.at("build").at("attempt").get<bool>())
                << host.os << '/' << host.arch << ' ' << label
                << ": the BUILD of a declared leg is unconditional. Whether a"
                   " host can EXECUTE the artifact is a separate question and"
                   " must not suppress the compile.";

            auto const& runPlan = leg.at("run");
            auto const  mode    = runPlan.at("mode").get<std::string>();
            EXPECT_TRUE(mode == "native" || mode == "launched" || mode == "skip")
                << host.os << '/' << host.arch << ' ' << label
                << ": unknown run mode '" << mode << '\'';
            EXPECT_FALSE(runPlan.at("detail").get<std::string>().empty())
                << host.os << '/' << host.arch << ' ' << label
                << ": every outcome must carry a REASON. A skip with no reason"
                   " is the no-verdict defect this whole ledger exists to end.";

            if (mode == "skip") {
                ASSERT_FALSE(runPlan.at("verdict").is_null())
                    << host.os << '/' << host.arch << ' ' << label
                    << ": a skip must be NAMED";
                auto const verdict = runPlan.at("verdict").get<std::string>();
                EXPECT_TRUE(vocabulary.count(verdict) == 1)
                    << host.os << '/' << host.arch << ' ' << label
                    << ": verdict '" << verdict
                    << "' is not in the ledger's closed vocabulary";
            } else {
                EXPECT_TRUE(runPlan.at("verdict").is_null())
                    << host.os << '/' << host.arch << ' ' << label
                    << ": a leg that WILL run carries no verdict yet — `ran` /"
                       " `poisoned` is the driver's to record once it has.";
            }
        }
    }
}

// A host that can execute nothing must still build everything, and must say
// exactly why each leg will not run. This is the cell the old drivers got
// wrong in both directions (the .ps1 refused to start at all; the .sh silently
// produced a shorter leg list).
TEST_F(HarnessLegs, AHostThatCanRunNothingStillBuildsEverythingAndSaysWhy) {
    auto const r = run({"--plan", "--host-os", "unknown", "--host-arch",
                        "unknown", "--launchers-none", "--format", "json"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    auto const plan = json::parse(r.output);
    ASSERT_FALSE(plan.at("legs").empty());
    for (auto const& leg : plan.at("legs")) {
        EXPECT_TRUE(leg.at("build").at("attempt").get<bool>());
        EXPECT_EQ(leg.at("run").at("mode").get<std::string>(), "skip");
        EXPECT_EQ(leg.at("run").at("verdict").get<std::string>(),
                  armVerdictName(ArmVerdict::SkippedByRunOn));
    }
}

// ── 5. The drivers actually consult it ─────────────────────────────────────
//
// A resolver nothing reads would be theatre. These are structural pins on the
// two shipped drivers: they must invoke the resolver, and they must not carry
// the shapes that host-locked them.
//
// RED-ON-DISABLE (measured, see the cycle report): restore
// `if (-not $IsWindows ...) { Die ... }` to build-and-test.ps1 and
// `TheDriversAreNotHostKeyed` fails; delete the resolver call from either
// driver and `TheDriversConsultTheResolver` fails.
TEST_F(HarnessLegs, TheDriversConsultTheResolver) {
    for (char const* driver : {"build-and-test.sh", "build-and-test.ps1"}) {
        auto const path = harnessDir() / driver;
        ASSERT_TRUE(fs::exists(path)) << path;
        auto const text = fileText(path);
        EXPECT_NE(text.find("harness_legs.py"), std::string::npos)
            << driver
            << " does not invoke harness_legs.py. Its leg set is therefore"
               " decided somewhere else — which is how both drivers came to"
               " key on the host in the first place.";
        EXPECT_NE(text.find("legs.json"), std::string::npos)
            << driver << " never mentions the leg catalogue.";
    }
}

TEST_F(HarnessLegs, TheDriversAreNotHostKeyed) {
    // Three rules, each naming a shape that was REMOVED this cycle, each
    // evaluated over LIVE lines only (see `liveLines` — the first version of
    // this test searched raw text and went red on the driver's own explanation
    // of the gate it had deleted).
    //
    // What these rules deliberately do NOT forbid: a host automatic variable in
    // the ONE canonical host-identification function (`$IsWindows` → the string
    // "windows"), a fatal on failing to IDENTIFY the host at all, and a fatal on
    // a missing host TOOLCHAIN (`bash` not on PATH). All three are the
    // legitimate host question — "what machine am I, and what can it do" — and
    // a pin that outlawed them would be pushing the driver toward guessing.

    struct Driver {
        char const* name;
        bool        powershell;
    };
    constexpr Driver kDrivers[] = {{"build-and-test.sh", false},
                                   {"build-and-test.ps1", true}};

    for (auto const& d : kDrivers) {
        auto const lines = liveLines(harnessDir() / d.name, d.powershell);
        ASSERT_FALSE(lines.empty()) << d.name << " has no live lines";

        for (std::size_t i = 0; i < lines.size(); ++i) {
            auto const& line = lines[i];

            // RULE 1 — a leg's target spec comes from the catalogue, never from
            // a literal in a driver. This is what `$Spec =
            // 'x86_64:pe64-x86_64-windows-exec'` was: a sixth leg, invisible to
            // the catalogue's host-invariance property and to the verdict
            // accounting.
            EXPECT_FALSE(mentionsTargetSpecLiteral(line))
                << d.name << " writes a target spec as a literal:\n  " << line
                << "\nEvery leg this harness builds must come from legs.json"
                   " through harness_legs.py, or it escapes both the"
                   " host-invariance property and the ledger.";

            // RULE 2 — the .sh's host-keyed leg CONSTRUCTOR is gone. `add_leg`
            // was called once unconditionally and once under
            // `if [[ $HOST_OS == linux && $HOST_ARCH == x86_64 ]]`, which is
            // precisely how pe64 and mach-o became unreachable from that
            // driver.
            if (!d.powershell) {
                EXPECT_EQ(line.find("add_leg"), std::string::npos)
                    << d.name << " still constructs legs itself:\n  " << line
                    << "\nThe leg set is read, not built.";
            }

            // RULE 3 — no host automatic variable may guard a REFUSAL. This is
            // the .ps1's `if (-not $IsWindows) { Die ... }` exactly: the driver
            // declining to run because of the machine it is on.
            if (d.powershell) {
                bool const hostVar = line.find("$IsWindows") != std::string::npos
                                  || line.find("$IsLinux") != std::string::npos
                                  || line.find("$IsMacOS") != std::string::npos;
                EXPECT_FALSE(hostVar && line.find("Die") != std::string::npos)
                    << d.name << " refuses to run based on the host:\n  " << line
                    << "\nThe host decides what can be EXECUTED, never what can"
                       " be BUILT.";
            }
        }
    }
}

// ── 6. Launcher vocabulary agreement with the examples corpus ──────────────
//
// `lintDeclaredEmulators` already enforces "one (arch, runOn-OS) pair, one
// emulator spelling" WITHIN the examples corpus. The sqlite catalogue is a
// second corpus of declarations about the same machines, so a divergence
// ("qemu-aarch64" here, "qemu-aarch64-static" there) would mean the project
// believes two different things about the same host. Only the pairs BOTH
// corpora declare are compared: the catalogue deliberately declares launchers
// the corpus does not (qemu-x86_64 for an arm64 Linux host, Wine for pe on
// Linux), and requiring the corpus to match those would be a different — and
// much larger — change than this one.
TEST_F(HarnessLegs, LauncherSpellingsAgreeWithTheExamplesCorpus) {
    auto const examples = repoRoot() / "examples" / "c-subset";
    ASSERT_TRUE(fs::exists(examples)) << examples;

    // (targetArch, runOn-OS) -> emulator spelling, from the corpus.
    std::map<std::pair<std::string, std::string>, std::set<std::string>> corpus;
    for (auto const& entry : fs::directory_iterator(examples)) {
        auto const manifest = entry.path() / "expected.json";
        if (!fs::exists(manifest)) continue;
        json doc;
        try {
            doc = json::parse(fileText(manifest));
        } catch (std::exception const&) {
            continue;  // the corpus runners own manifest validity
        }
        if (!doc.contains("targets")) continue;
        for (auto const& t : doc.at("targets")) {
            if (!t.contains("spec") || !t.contains("runOn")) continue;
            auto const emulator = t.value("emulator", std::string{});
            if (emulator.empty()) continue;
            auto const arch = dss::test_support::specTargetArch(
                t.at("spec").get<std::string>());
            for (auto const& osName : t.at("runOn")) {
                corpus[{arch, osName.get<std::string>()}].insert(emulator);
            }
        }
    }
    ASSERT_FALSE(corpus.empty())
        << "no example manifest declares an emulator — this comparison would"
           " be vacuously green";

    auto const doc = json::parse(fileText(catalogue_));
    std::size_t compared = 0;
    for (auto const& leg : doc.at("legs")) {
        auto const arch = dss::test_support::specTargetArch(
            leg.at("spec").get<std::string>());
        for (auto const& entry : leg.at("launchers")) {
            auto const hostOs = entry.at("hostOs").get<std::string>();
            auto const cmd    = entry.at("command");
            ASSERT_FALSE(cmd.empty());
            auto const head = cmd.at(0).get<std::string>();
            auto const it   = corpus.find({arch, hostOs});
            if (it == corpus.end()) continue;
            ++compared;
            EXPECT_EQ(it->second.count(head), 1u)
                << "the sqlite catalogue launches (arch=" << arch
                << ", host OS=" << hostOs << ") with '" << head
                << "' while the examples corpus declares '"
                << *it->second.begin()
                << "' for the same pair — one pair, one vocabulary";
        }
    }
    EXPECT_GT(compared, 0u)
        << "no (arch, host OS) pair is declared by BOTH the sqlite catalogue and"
           " the examples corpus, so this test compared nothing. That is a"
           " silent-vacuity failure, not a pass.";
}

// ── 7. Every leg's staged zlib header is configured for ITS OWN target ─────
//
// D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED. Three tests, one per link in the
// chain: the DECLARATION (does the catalogue say the right thing?), the PLAN (is
// the recipeTransform -> zinc/ mapping host-free and one-stage-per-transform?),
// and the ARTEFACT (does stage-zinc.py actually write it?).

// THE DECLARATION. `Z_HAVE_UNISTD_H` governs `#include <unistd.h>`, which exists
// on POSIX and not on Windows — so its correct value is DERIVABLE from the leg's
// own target and can be checked rather than trusted.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): give the pe64 leg the
// POSIX answer — or give the four POSIX legs the pe answer, which is exactly the
// shared pe-shaped stage this anchor is about — and this fails naming the leg,
// its target OS and both values.
TEST_F(HarnessLegs, EveryLegsZlibHeaderIsConfiguredForItsOwnTarget) {
    auto const doc = json::parse(fileText(catalogue_));
    std::size_t checked = 0;
    std::set<std::string> osSeen;
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        auto const spec  = leg.at("spec").get<std::string>();
        auto const os    = specTargetOs(spec);
        ASSERT_FALSE(os.empty())
            << label << ": cannot derive a target OS from spec '" << spec << '\'';
        osSeen.insert(os);
        ASSERT_TRUE(leg.at("build").contains("zconfGuards"))
            << label
            << ": declares no build.zconfGuards. Without it the drivers have"
               " nothing to stage this leg's zlib header FROM, and the only"
               " remaining option is the copy some other leg's target wanted —"
               " which is the defect.";
        auto const& guards = leg.at("build").at("zconfGuards");
        ASSERT_TRUE(guards.contains("Z_HAVE_UNISTD_H")) << label;
        ASSERT_TRUE(guards.at("Z_HAVE_UNISTD_H").is_boolean())
            << label << ": Z_HAVE_UNISTD_H must be a JSON boolean — a string is"
                        " truthy in bash, PowerShell and python alike";
        bool const posix = (os == "linux" || os == "darwin");
        EXPECT_EQ(guards.at("Z_HAVE_UNISTD_H").get<bool>(), posix)
            << label << " targets OS '" << os
            << "' but declares Z_HAVE_UNISTD_H="
            << guards.at("Z_HAVE_UNISTD_H").get<bool>()
            << ". That guard decides whether the staged zconf.h does"
               " `#include <unistd.h>` and therefore whether z_off_t is off_t or"
               " long. MEASURED TF-C115: on darwin those are `long long` vs"
               " `long` — same width, DIFFERENT type — and on pe the POSIX answer"
               " does not compile at all (error[F001D] got unistd.h).";
        ++checked;
    }
    EXPECT_GE(checked, 2u);
    EXPECT_GE(osSeen.size(), 2u)
        << "every leg targets the same OS, so this test cannot witness a"
           " per-target difference — it would pass on a single shared header";
}

// THE PLAN. One stage per recipeTransform, the same on every host, and each
// leg's key is its own transform.
//
// RED-ON-DISABLE: make `headerStageKey` depend on anything but the leg's
// declared transform (a host, a label) and the cross-host comparison fails; give
// two legs the same transform but different guards and `--header-stages` exits
// non-zero with the conflict named.
TEST_F(HarnessLegs, TheHeaderStagePlanIsPerTransformAndHostFree) {
    auto const stagesRun = run({"--header-stages"});
    ASSERT_TRUE(stagesRun.spawned) << stagesRun.diagnostic;
    ASSERT_EQ(stagesRun.exitCode, 0u) << stagesRun.output;
    std::map<std::string, std::string> stages;  // key -> guard string
    for (auto const& line : splitLines(stagesRun.output)) {
        auto const tab = line.find('\t');
        ASSERT_NE(tab, std::string::npos) << "malformed stage line: " << line;
        stages[line.substr(0, tab)] = line.substr(tab + 1);
    }
    ASSERT_GE(stages.size(), 2u)
        << "the catalogue declares fewer than two header stages, so per-target"
           " staging is untested by the catalogue it ships with:\n"
        << stagesRun.output;

    auto const doc = json::parse(fileText(catalogue_));
    std::set<std::string> transforms;
    for (auto const& leg : doc.at("legs")) {
        transforms.insert(
            leg.at("build").at("recipeTransform").get<std::string>());
    }
    std::set<std::string> stageKeys;
    for (auto const& [k, _] : stages) stageKeys.insert(k);
    EXPECT_EQ(stageKeys, transforms)
        << "the stage set must be exactly the distinct recipeTransforms — one"
           " zinc/ per transform, no more (a stage nothing uses) and no fewer"
           " (two targets sharing one header).";

    // Same answer on every host, including three this project has never run on.
    std::map<std::string, std::string> firstHostKeys;
    for (auto const& host : kSimHosts) {
        auto const r = run({"--plan", "--host-os", host.os, "--host-arch",
                            host.arch, "--launchers-none", "--format", "json"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const plan = json::parse(r.output);
        std::map<std::string, std::string> keys;
        for (auto const& leg : plan.at("legs")) {
            auto const label = leg.at("label").get<std::string>();
            auto const& build = leg.at("build");
            ASSERT_TRUE(build.contains("headerStageKey")) << label;
            auto const key = build.at("headerStageKey").get<std::string>();
            EXPECT_EQ(key, build.at("recipeTransform").get<std::string>())
                << host.os << '/' << host.arch << ' ' << label
                << ": the staged-header key must BE the declared"
                   " recipeTransform. Anything else is a second mapping a"
                   " reader would have to find.";
            EXPECT_EQ(stages.count(key), 1u)
                << host.os << '/' << host.arch << ' ' << label
                << ": names stage '" << key << "', which is not in the plan";
            keys[label] = key;
        }
        if (firstHostKeys.empty()) {
            firstHostKeys = keys;
        } else {
            EXPECT_EQ(keys, firstHostKeys)
                << "host " << host.os << '/' << host.arch
                << " assigns DIFFERENT staged headers than the first host."
                   " Which zlib header a leg compiles against is a fact about"
                   " its TARGET; a host that changes it has re-locked the"
                   " harness one level down from the leg set.";
        }
    }
}

// THE ARTEFACT. Drive the REAL staging tool with the SHIPPED catalogue over a
// zconf.h fixture, then read every leg's include dir back off disk and confirm
// the guard it got is the one its own target wanted.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): revert to one shared
// zinc/ — i.e. point every leg at a single staged directory, however that
// directory was configured — and the legs on the other side of the guard fail
// here with the value they actually got.
TEST_F(HarnessLegs, StageZincWritesOneHeaderPerTargetAndEveryLegGetsItsOwn) {
    dss::test_support::ScratchDir work{dss::test_support::Location::Temp,
                                       "sqlite-harness-zinc"};
    auto const zconfSrc = work.path() / "zconf.h";
    auto const zlibSrc  = work.path() / "zlib.h";
    {
        std::ofstream out(zconfSrc, std::ios::binary);
        out << kZconfFixture;
        std::ofstream z(zlibSrc, std::ios::binary);
        z << "#include \"zconf.h\"\n";
    }
    auto const dest = work.path() / "zinc";

    auto const py = pythonPath();
    ASSERT_FALSE(py.empty())
        << "python3 is a hard dependency of both drivers; a skip here would be"
           " the no-verdict defect this file exists to end.";
    std::vector<std::string> argv{
        py,
        (harnessDir() / "stage-zinc.py").string(),
        "--zlib-h",  zlibSrc.string(),
        "--zconf-h", zconfSrc.string(),
        "--dest",    dest.string(),
        // `runBinary` APPENDS its `binaryPath` as the final argv element (see
        // `runResolver`), so the catalogue's VALUE is supplied by that
        // append — and it is the scratch copy, which is also what keeps this
        // from chmod'ing a tracked repo file on POSIX.
        "--catalogue"};
    auto const res = dss::test_support::runBinary(
        catalogue_, std::chrono::seconds{120}, /*captureStdout=*/true, argv);
    ASSERT_TRUE(res.spawned && !res.timedOut) << res.diagnostic;
    ASSERT_EQ(res.exitCode, 0u)
        << "stage-zinc.py could not produce every declared stage:\n"
        << res.capturedStdout;
    EXPECT_NE(res.capturedStdout.find("ZINC-STAGES="), std::string::npos)
        << res.capturedStdout;

    auto const doc = json::parse(fileText(catalogue_));
    std::map<std::string, int> observed;  // stage key -> Z_HAVE_UNISTD_H state
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        auto const key =
            leg.at("build").at("recipeTransform").get<std::string>();
        auto const zconf = dest / key / "zconf.h";
        ASSERT_TRUE(fs::exists(zconf))
            << label << ": no staged zlib header at " << zconf
            << ". A leg without its own zinc/ has nowhere to go but another"
               " target's copy, and stage-zinc.py refuses that on purpose —"
               " so the driver poisons the leg instead.";
        bool const want =
            leg.at("build").at("zconfGuards").at("Z_HAVE_UNISTD_H").get<bool>();
        int const got = guardStateIn(zconf, "Z_HAVE_UNISTD_H");
        EXPECT_EQ(got, want ? 1 : 0)
            << label << " (target OS "
            << specTargetOs(leg.at("spec").get<std::string>())
            << ") compiles against " << zconf
            << ", whose Z_HAVE_UNISTD_H reads " << got << " — it declared "
            << want
            << ". A leg parsing a zlib header configured for a different target"
               " is D-HARNESS-SQLITE-STAGE-ZCONF-IS-PE-SHAPED exactly.";
        // Z_HAVE_STDARG_H is the CONTROL: it is declared true on every leg, so
        // if the tool were rewriting whole blocks rather than the one declared
        // guard, this would move too.
        EXPECT_EQ(guardStateIn(zconf, "Z_HAVE_STDARG_H"), 1)
            << label << ": the sibling guard must be untouched at #if 1";
        observed[key] = got;
    }
    ASSERT_GE(observed.size(), 2u);
    std::set<int> distinct;
    for (auto const& [k, v] : observed) distinct.insert(v);
    EXPECT_EQ(distinct.size(), 2u)
        << "every staged header came out with the SAME Z_HAVE_UNISTD_H, so this"
           " test would pass against the single shared zinc/ it was written to"
           " outlaw. That is a silent-vacuity failure, not a pass.";
}

// THE DRIVERS. Both must use the shared tool, and neither may carry the shapes
// that made one zinc/ serve every leg.
//
// RED-ON-DISABLE: put the `perl -0777 … Z_HAVE_UNISTD_H` flip back into either
// driver, or hand every leg one `includes.txt`, and this fails naming the line.
TEST_F(HarnessLegs, BothDriversStageOneZincPerTransform) {
    struct Driver {
        char const* name;
        bool        powershell;
        char const* sharedIncludeList;  // the pre-TF-C115 single-file spelling
    };
    constexpr Driver kDrivers[] = {
        {"build-and-test.sh", false, "recipe-includes.txt"},
        {"build-and-test.ps1", true, "includes.txt'"}};

    for (auto const& d : kDrivers) {
        auto const path = harnessDir() / d.name;
        ASSERT_TRUE(fs::exists(path)) << path;
        EXPECT_NE(fileText(path).find("stage-zinc.py"), std::string::npos)
            << d.name
            << " does not call stage-zinc.py. Per-target header staging then"
               " lives somewhere else in that driver — and a capability in one"
               " driver and not the other is a silent harness bug, which is how"
               " the .sh came to build its pe64 leg against a POSIX zconf.h.";

        for (auto const& line : liveLines(path, d.powershell)) {
            // The macro names belong to the CATALOGUE and to stage-zinc.py. A
            // driver that spells one is deciding a target's header itself.
            EXPECT_EQ(line.find("Z_HAVE_UNISTD_H"), std::string::npos)
                << d.name << " names a zconf guard in live code:\n  " << line
                << "\nGuards are declared per leg in legs.json and applied by"
                   " stage-zinc.py; a driver that edits one has re-created the"
                   " single pe-shaped stage.";
            EXPECT_EQ(line.find("perl -0777"), std::string::npos)
                << d.name << " patches a staged header in place:\n  " << line;
            EXPECT_EQ(line.find(d.sharedIncludeList), std::string::npos)
                << d.name << " still names ONE shared include list:\n  " << line
                << "\nThe list's last entry is the staged zlib dir, which is"
                   " per target — so there is one list per header stage.";
        }
    }
}

// ── 8. A launcher's PATH NAMESPACE is declared, closed, and enforced ───────
//
// D-HARNESS-NO-WSL-LAUNCHER-FOR-ELF-ON-WINDOWS. The catalogue declared Wine for
// pe-on-Linux and NOTHING for elf-on-Windows, so a Windows host recorded
// `skipped-by-runOn` for a leg whose testfixture it had just built — and that
// same artefact, run under WSL BY HAND, passed 330,436 tests.
//
// WHY IT WAS NOT A ONE-LINE CONFIG ADDITION, which is what these tests are
// really about: a launcher does not always share a filesystem NAMESPACE with the
// driver that spawns it. `wine /home/me/x.exe` takes the driver's own path;
// `wsl.exe` needs `/mnt/c/...` where the driver holds `C:\...`. An untranslated
// path is NOT reported as a bad path — the callee opens a RELATIVE file of that
// name, misses, and the run reads as a broken binary. So every launcher declares
// `pathTranslation`, the resolver owns the closed vocabulary AND performs the
// translation (`--translate-path`), and `--assert-translated` is the net under
// the drivers.
//
// ★ WHAT IS NOT TESTED HERE AND WHY: none of these tests invokes `wslpath`.
// This file runs on Windows, on WSL and on an arm64 Linux VPS, and a test whose
// green depended on `wsl.exe` would be a skip-or-red on two of the three. The
// translator CONTRACT (the path handed over VERBATIM, rc, empty output, an
// output still in the source namespace, a source path in the wrong namespace)
// is exercised by `harness_legs.py --self-test` with an INJECTED translator —
// which this file runs, as `TheResolverSelfTestPasses`. The live `wslpath` path
// is measured by the driver run recorded in the cycle report.
// ⚠ "VERBATIM" replaced "separator normalisation before the call" on
// 2026-08-04: that normalisation was a workaround for `wslpath: C:ab`, a symptom
// whose cause was misattributed to wslpath eating backslashes when it was
// actually the local shell `wsl.exe` runs without `-e` (section 9 below).

// The vocabulary is CLOSED and the catalogue may not step outside it. Asserted
// from both directions: the resolver prints the set, and every verb any launcher
// declares is in it.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): give a launcher a verb
// the resolver does not implement and this fails naming the leg and the verb.
TEST_F(HarnessLegs, EveryLauncherDeclaresAVerbFromTheClosedNamespaceVocabulary) {
    auto const r = run({"--path-translations"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;

    std::map<std::string, std::string> known;  // verb -> translator argv
    for (auto const& line : splitLines(r.output)) {
        auto const tab = line.find('\t');
        ASSERT_NE(tab, std::string::npos) << "malformed line: " << line;
        known[line.substr(0, tab)] = line.substr(tab + 1);
    }
    ASSERT_EQ(known.count("none"), 1u)
        << "`none` must always exist: it is how a launcher that takes this"
           " driver's paths verbatim SAYS SO, and without it the absence of a"
           " key would have to mean it.";
    EXPECT_TRUE(known.at("none").empty())
        << "`none` names a translator ('" << known.at("none")
        << "') — it is the identity by definition.";
    ASSERT_GE(known.size(), 2u)
        << "only one verb is declared, so 'closed vocabulary' is untested by the"
           " resolver it ships with";

    auto const  doc      = json::parse(fileText(catalogue_));
    std::size_t declared = 0;
    std::set<std::string> verbsSeen;
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        for (auto const& entry : leg.at("launchers")) {
            ASSERT_TRUE(entry.contains("pathTranslation"))
                << label << ": a launcher for (" << entry.at("hostOs")
                << ", " << entry.at("hostArch")
                << ") declares no pathTranslation. Every launcher states the"
                   " PATH NAMESPACE its argv lives in — a default would make"
                   " 'nobody thought about it' indistinguishable from 'it takes"
                   " our paths verbatim'.";
            auto const verb = entry.at("pathTranslation").get<std::string>();
            EXPECT_EQ(known.count(verb), 1u)
                << label << ": launcher for (" << entry.at("hostOs") << ", "
                << entry.at("hostArch") << ") declares pathTranslation '" << verb
                << "', which the resolver does not implement";
            verbsSeen.insert(verb);
            ++declared;
        }
    }
    EXPECT_GE(declared, 2u) << "no launcher declared a namespace — vacuous";
    EXPECT_GE(verbsSeen.size(), 2u)
        << "every launcher in the catalogue declares the SAME namespace ("
        << *verbsSeen.begin()
        << "), so this catalogue cannot witness the distinction the key exists"
           " to make. That is a silent-vacuity failure, not a pass.";

    // ── the ENVIRONMENT namespace, the same three ways ────────────────────
    // A launcher in another OS namespace does not inherit the driver's
    // environment either, and that failure is quieter than the path one: the
    // child runs with an EMPTY run environment and simply does something else.
    auto const e = run({"--env-transfers"});
    ASSERT_TRUE(e.spawned) << e.diagnostic;
    ASSERT_EQ(e.exitCode, 0u) << e.output;
    std::map<std::string, std::string> envVerbs;  // verb -> carrier variable
    for (auto const& line : splitLines(e.output)) {
        auto const tab = line.find('\t');
        ASSERT_NE(tab, std::string::npos) << "malformed line: " << line;
        envVerbs[line.substr(0, tab)] = line.substr(tab + 1);
    }
    ASSERT_EQ(envVerbs.count("inherit"), 1u);
    EXPECT_TRUE(envVerbs.at("inherit").empty())
        << "`inherit` names a carrier variable ('" << envVerbs.at("inherit")
        << "') — inheriting is precisely needing none.";
    ASSERT_GE(envVerbs.size(), 2u)
        << "only one environment-transfer verb is declared, so the vocabulary is"
           " untested by the resolver it ships with";
    std::set<std::string> envSeen;
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        for (auto const& entry : leg.at("launchers")) {
            ASSERT_TRUE(entry.contains("envTransfer"))
                << label << ": a launcher for (" << entry.at("hostOs") << ", "
                << entry.at("hostArch")
                << ") declares no envTransfer. MEASURED 2026-08-04: a"
                   " wsl.exe-launched fixture saw SQLITE_TEST_PATTERN_LIST as"
                   " EMPTY, and the corpus resume engine — which selects its"
                   " files through that variable — re-ran the whole corpus"
                   " instead of the tail after the abort.";
            auto const verb = entry.at("envTransfer").get<std::string>();
            EXPECT_EQ(envVerbs.count(verb), 1u)
                << label << ": envTransfer '" << verb
                << "' is not implemented by the resolver";
            envSeen.insert(verb);
        }
    }
    EXPECT_GE(envSeen.size(), 2u)
        << "every launcher declares the SAME environment transfer ("
        << *envSeen.begin() << ") — vacuous, as above.";
}

// The lint REFUSES the three ways a declaration can be wrong. Each mutation is
// applied to a scratch copy of the SHIPPED catalogue, so what is refused is a
// realistic edit and not a straw fixture.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): delete any of the
// three lint rules in harness_legs.py and the matching case here goes green with
// `findings=0`.
TEST_F(HarnessLegs, AMalformedPathTranslationDeclarationFailsLint) {
    struct Case {
        char const* tag;
        char const* leg;
        char const* needle;  // must appear in the lint output
    };
    // (1) an unknown verb, (2) the key omitted entirely, (3) a verb declared on
    // a host whose namespace it does not describe.
    {
        MutatedCatalogue m{catalogue_, "legs-unknown-verb"};
        m.firstLauncherOf("pe64-x86_64")["pathTranslation"] = "windows-to-posix";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown pathTranslation verb LINTED CLEAN:\n"
            << r.output
            << "\nA verb nothing implements is a declaration that reads as"
               " configuration and is not — the launcher would be handed"
               " untranslated paths and the failure would look like a broken"
               " binary.";
        EXPECT_NE(r.output.find("windows-to-posix"), std::string::npos)
            << "the refusal must NAME the verb:\n" << r.output;
        EXPECT_NE(r.output.find("pe64-x86_64"), std::string::npos)
            << "the refusal must NAME the leg:\n" << r.output;
    }
    {
        MutatedCatalogue m{catalogue_, "legs-missing-verb"};
        m.firstLauncherOf("pe64-x86_64").erase("pathTranslation");
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a launcher with NO pathTranslation linted clean:\n"
            << r.output
            << "\nThe key is required on every entry for the same reason every"
               " zconf guard is: a reader must not have to know a default.";
        EXPECT_NE(r.output.find("pathTranslation"), std::string::npos)
            << r.output;
    }
    {
        // `windows-to-wsl` translates FROM a drive-letter path using a Windows
        // tool. On a Linux host there is neither. The lint can DERIVE that, so
        // it checks the declaration instead of trusting it.
        MutatedCatalogue m{catalogue_, "legs-verb-wrong-host"};
        m.firstLauncherOf("pe64-x86_64")["pathTranslation"] = "windows-to-wsl";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "pathTranslation 'windows-to-wsl' declared on a LINUX host"
               " launcher linted clean:\n"
            << r.output;
        EXPECT_NE(r.output.find("windows-to-wsl"), std::string::npos) << r.output;
    }
    // ── the same three, for the ENVIRONMENT namespace ──────────────────────
    {
        MutatedCatalogue m{catalogue_, "legs-unknown-envverb"};
        m.firstLauncherOf("pe64-x86_64")["envTransfer"] = "copy-the-block";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown envTransfer verb LINTED CLEAN:\n" << r.output;
        EXPECT_NE(r.output.find("copy-the-block"), std::string::npos) << r.output;
    }
    {
        MutatedCatalogue m{catalogue_, "legs-missing-envverb"};
        m.firstLauncherOf("pe64-x86_64").erase("envTransfer");
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a launcher with NO envTransfer linted clean:\n" << r.output;
        EXPECT_NE(r.output.find("envTransfer"), std::string::npos) << r.output;
    }
    {
        MutatedCatalogue m{catalogue_, "legs-envverb-wrong-host"};
        m.firstLauncherOf("pe64-x86_64")["envTransfer"] = "wslenv";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "envTransfer 'wslenv' declared on a LINUX host launcher linted"
               " clean:\n"
            << r.output;
    }
}

// THE RESUME ENGINE'S OWN HOOK MUST CROSS. `--env-transfer` turns a verb plus a
// list of variable NAMES into the assignments a driver must make, and this is
// the part that decides whether a cross-OS launcher's fixture can be steered at
// all. Needs no WSL: the merge and the carrier are pure string work.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): make
// `env_carrier_assignments` return [] for every verb and the wslenv cases fail.
TEST_F(HarnessLegs, TheRunEnvironmentIsForwardedIntoTheLaunchersNamespace) {
    auto forward = [&](char const* verb, std::vector<std::string> const& names,
                       char const* current) {
        std::vector<std::string> args{"--env-transfer", verb, "--carrier-current",
                                      current};
        for (auto const& n : names) { args.push_back("--forward"); args.push_back(n); }
        return run(args);
    };
    {  // `inherit` must add NOTHING — a native run stays byte-for-byte itself.
        auto const r = forward("inherit", {"SQLITE_TEST_PATTERN_LIST"}, "");
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 0u) << r.output;
        EXPECT_TRUE(splitLines(r.output).empty())
            << "an inheriting launcher was given environment assignments:\n"
            << r.output;
    }
    {
        auto const r = forward("wslenv",
                               {"SQLITE_TEST_PATTERN_LIST", "QUICKTEST_OMIT"}, "");
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const lines = splitLines(r.output);
        ASSERT_EQ(lines.size(), 1u) << r.output;
        EXPECT_EQ(lines[0], "WSLENV=SQLITE_TEST_PATTERN_LIST:QUICKTEST_OMIT")
            << "the resume engine steers the corpus through"
               " SQLITE_TEST_PATTERN_LIST; a launcher that cannot see it re-runs"
               " the whole corpus and looks like it is working";
    }
    {  // an operator's own carrier value survives.
        auto const r = forward("wslenv", {"QUICKTEST_OMIT"}, "MYVAR/u");
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const lines = splitLines(r.output);
        ASSERT_EQ(lines.size(), 1u) << r.output;
        EXPECT_EQ(lines[0], "WSLENV=MYVAR/u:QUICKTEST_OMIT")
            << "an operator's existing carrier setting was clobbered";
    }
    {  // an unknown verb is FATAL, never a silent "inherit".
        auto const r = forward("copy-the-block", {"A"}, "");
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown envTransfer verb resolved silently:\n" << r.output;
    }
}

// THE FAILURE THE BAR NAMES: a translation that covers argv[0] and nothing else.
// `--assert-translated` is the guard both drivers call at the ONE point the
// child is spawned, and it must see the fixture AND every file argument.
//
// This test needs no translator, which is why it can run on every gate leg.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): make
// `assert_translated` inspect only args[0] and the second case goes green.
TEST_F(HarnessLegs, TheLauncherArgvGuardCoversTheFixtureAndEveryFileArgument) {
    // The real shapes: a fixture, a `.test` script, and a tester.tcl flag.
    constexpr char const* kFixtureOk = "/mnt/c/out/elf64-x86_64/testfixture";
    constexpr char const* kScriptOk  = "/mnt/c/stage/test/veryquick.test";
    constexpr char const* kFixtureBad =
        "C:\\build\\out\\elf64-x86_64\\testfixture";
    constexpr char const* kScriptBad = "C:\\build\\stage\\test\\veryquick.test";

    auto assertTranslated = [&](std::vector<std::string> const& argv) {
        std::vector<std::string> args{"--path-translation", "windows-to-wsl"};
        for (auto const& a : argv) args.push_back("--assert-translated=" + a);
        return run(args);
    };

    {  // everything translated — the only shape that may spawn.
        auto const r = assertTranslated({kFixtureOk, kScriptOk, "--start=full:"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 0u)
            << "a fully-translated argv was REFUSED:\n" << r.output;
    }
    {  // argv[0] untranslated.
        auto const r = assertTranslated({kFixtureBad, kScriptOk});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an untranslated FIXTURE path was accepted:\n" << r.output;
    }
    {  // ★ THE ONE THE BAR NAMES: argv[0] fine, the file argument not.
        auto const r = assertTranslated({kFixtureOk, kScriptBad});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "the FIXTURE was translated and a FILE ARGUMENT was not, and the"
               " guard accepted it:\n"
            << r.output
            << "\nThat is exactly the half-done translation this guard exists"
               " for: the fixture would load, the first test would fail to open"
               " its script, and the failure would read as a test bug.";
        EXPECT_NE(r.output.find("veryquick.test"), std::string::npos)
            << "the refusal must NAME the offending argument:\n" << r.output;
    }
    {  // a path hiding inside a flag is still a path.
        auto const r = assertTranslated({kFixtureOk, "--testdir=D:\\scratch"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an untranslated path EMBEDDED IN A FLAG was accepted:\n"
            << r.output;
    }
    {  // and a launcher that declared `none` translates nothing, by definition.
        auto const r = run({"--path-translation", "none",
                            std::string{"--assert-translated="} + kFixtureBad});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 0u)
            << "a `none` launcher refused this driver's own path spelling:\n"
            << r.output;
    }
}

// THE CELL THE ANCHOR IS ABOUT. On a Windows host the elf64 leg must be
// LAUNCHED, with a translating launcher — and when the launcher is absent the
// verdict must be the ENVIRONMENTAL skip, not the structural one, because
// "nobody can ever run this here" and "this machine lacks a tool" are different
// facts and only the second is actionable.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): delete the wsl.exe
// launcher entry from legs.json and both halves fail with `skipped-by-runOn`.
TEST_F(HarnessLegs, AWindowsHostLaunchesTheLinuxLegRatherThanSkippingIt) {
    struct Cell {
        char const* hostArch;
        char const* leg;
    };
    // WSL runs the HOST's architecture: an x86_64 Windows box reaches the
    // x86_64 Linux leg, an arm64 one the arm64 leg.
    constexpr Cell kCells[] = {{"x86_64", "elf64-x86_64"},
                               {"arm64", "elf64-arm64"}};
    for (auto const& cell : kCells) {
        auto const r = run({"--plan", "--host-os", "windows", "--host-arch",
                            cell.hostArch, "--launchers-available", "wsl.exe",
                            "--format", "json"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const plan  = json::parse(r.output);
        bool       found = false;
        for (auto const& leg : plan.at("legs")) {
            if (leg.at("label").get<std::string>() != cell.leg) continue;
            found = true;
            auto const& runPlan = leg.at("run");
            EXPECT_EQ(runPlan.at("mode").get<std::string>(), "launched")
                << "windows/" << cell.hostArch << ' ' << cell.leg
                << " is not launched: " << runPlan.at("detail")
                << "\nD-HARNESS-NO-WSL-LAUNCHER-FOR-ELF-ON-WINDOWS: this driver"
                   " BUILDS this leg on this host and the artefact has been"
                   " MEASURED to pass 330,436 tests under WSL. A skip beside a"
                   " working binary is a declaration gap, not a capability one.";
            ASSERT_FALSE(runPlan.at("launcher").empty());
            EXPECT_EQ(runPlan.at("launcher").at(0).get<std::string>(), "wsl.exe");
            EXPECT_EQ(runPlan.at("pathTranslation").get<std::string>(),
                      "windows-to-wsl")
                << "the launcher lives in another path namespace and must say"
                   " so; a plain command entry would spawn and then fail in a"
                   " way that looks like a broken binary";
            EXPECT_FALSE(runPlan.at("pathTranslator").empty())
                << "a translating verb must resolve to the argv that performs"
                   " it, so neither driver has to name the tool";
            EXPECT_EQ(runPlan.at("envTransfer").get<std::string>(), "wslenv")
                << "the launcher does not inherit this driver's environment"
                   " either — MEASURED: SQLITE_TEST_PATTERN_LIST arrives EMPTY,"
                   " and the resume engine then re-runs the whole corpus";
        }
        EXPECT_TRUE(found) << cell.leg << " is not in the plan at all";

        // Launcher absent -> ENVIRONMENTAL, and it must name what is missing.
        auto const none = run({"--plan", "--host-os", "windows", "--host-arch",
                               cell.hostArch, "--launchers-none", "--format",
                               "json"});
        ASSERT_TRUE(none.spawned) << none.diagnostic;
        ASSERT_EQ(none.exitCode, 0u) << none.output;
        // ★ The parsed document is NAMED, not a temporary iterated in place:
        // this file is C++23 but the gate compiles with GCC 13.2, which predates
        // P2718R0's lifetime extension for range-for temporaries — so
        // `for (x : json::parse(s).at("legs"))` iterates a DESTROYED object and
        // silently yields nothing, which an EXPECT-only loop reads as a pass.
        // MEASURED in this cycle: the first draft of the Wine test did exactly
        // that and failed with "pe64-x86_64 is not in the plan".
        auto const  nonePlan  = json::parse(none.output);
        bool        sawNoneCell = false;
        for (auto const& leg : nonePlan.at("legs")) {
            if (leg.at("label").get<std::string>() != cell.leg) continue;
            sawNoneCell = true;
            EXPECT_EQ(leg.at("run").at("verdict").get<std::string>(),
                      armVerdictName(ArmVerdict::SkippedEmulatorMissing))
                << "windows/" << cell.hostArch << ' ' << cell.leg
                << " without wsl.exe must be the ENVIRONMENTAL skip — a"
                   " structural one would say no Windows host can ever run it,"
                   " which is false and which DSS_STRICT_ARM_VERDICTS could not"
                   " act on.";
        }
        EXPECT_TRUE(sawNoneCell)
            << cell.leg << " is absent from the launcher-less plan, so the"
                           " environmental-skip half of this test compared"
                           " nothing";
    }
}

// WINE IS UNCHANGED. It is the one launcher that was already working, it takes
// the driver's own paths, and the whole mechanism above must not have moved it.
//
// RED-ON-DISABLE: give the Wine launchers a translating verb and this fails on
// both the declaration and the resolved plan.
TEST_F(HarnessLegs, WineStillTakesTheDriversOwnPathsUnchanged) {
    auto const doc = json::parse(fileText(catalogue_));
    std::size_t wineEntries = 0;
    for (auto const& leg : doc.at("legs")) {
        for (auto const& entry : leg.at("launchers")) {
            if (entry.at("command").at(0).get<std::string>() != "wine") continue;
            ++wineEntries;
            EXPECT_EQ(entry.at("pathTranslation").get<std::string>(), "none")
                << "Wine takes a unix path on a unix host — it shares this"
                   " driver's namespace, and translating for it would break the"
                   " one launcher that already worked.";
            EXPECT_EQ(entry.at("envTransfer").get<std::string>(), "inherit")
                << "a Wine child is an ordinary process of this host and gets"
                   " this driver's environment block; carrying it would be"
                   " machinery for nothing.";
        }
    }
    ASSERT_GE(wineEntries, 1u) << "no Wine launcher to check — vacuous";

    auto const r = run({"--plan", "--host-os", "linux", "--host-arch", "x86_64",
                        "--launchers-available", "wine,qemu-aarch64", "--format",
                        "json"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    bool       checked = false;
    auto const plan    = json::parse(r.output);  // NAMED — see the note above
    for (auto const& leg : plan.at("legs")) {
        if (leg.at("label").get<std::string>() != "pe64-x86_64") continue;
        checked = true;
        auto const& runPlan = leg.at("run");
        EXPECT_EQ(runPlan.at("mode").get<std::string>(), "launched");
        ASSERT_FALSE(runPlan.at("launcher").empty());
        EXPECT_EQ(runPlan.at("launcher").at(0).get<std::string>(), "wine");
        EXPECT_EQ(runPlan.at("pathTranslation").get<std::string>(), "none");
        EXPECT_TRUE(runPlan.at("pathTranslator").empty())
            << "a `none` launcher resolved a translator argv — the drivers"
               " would then spawn one per path for no reason";
        EXPECT_EQ(runPlan.at("envTransfer").get<std::string>(), "inherit");
    }
    EXPECT_TRUE(checked) << "pe64-x86_64 is not in the plan";
}

// BOTH DRIVERS, OR NEITHER. A translation landing in one driver only is the
// recurring capability-pair defect in this harness — it is what let the .sh
// build its pe64 leg against a POSIX zconf.h for a whole cycle.
//
// The pins are structural because the alternative is running a driver, and each
// one names a shape that would silently half-work:
//   · the FIXTURE must be translated — otherwise the launcher cannot even be
//     handed the binary;
//   · EVERY segment record's script argument must be translated — the "argv[0]
//     only" failure, caught at the construction site rather than at run time;
//   · the intermediary each driver routes segments through must be BOUND to the
//     real translation call, so it cannot quietly become a passthrough;
//   · the spawn-point guard must be called, because translating at construction
//     is only safe with a net;
//   · and the translation must be ASKED OF THE RESOLVER (`--translate-path`),
//     not hand-rolled, so there is one implementation and not two.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): drop the translation
// from either driver, or add a fourth segment record that does not call the
// helper, and this fails naming the file and the line.
TEST_F(HarnessLegs, BothDriversTranslateTheFixtureAndEverySegmentScript) {
    struct Driver {
        char const* name;
        bool        powershell;
        char const* fixtureHelper;  // translates the fixture, once per leg
        char const* fixtureSite;    // the line that must call it
        char const* segmentHelper;  // translates a segment's script argument
        char const* segmentNeedle;  // marks a segment-queue RECORD
        char const* forwardListSite; // names the variables that must cross
    };
    // Each driver reaches the translator through ONE named intermediary, and it
    // is the intermediary the segment records must name:
    //   · the .sh stores each translated script in a `LAUNCH_*` variable —
    //     deliberately NOT `$(launch_path …)` inline, because `die` inside a
    //     command substitution exits only the SUBSHELL and `set -e` would not
    //     see the empty field it leaves behind;
    //   · the .ps1 wraps its per-segment call in `Get-SegmentArgs`, which is
    //     where the "arg1 is the only path" invariant is spelled.
    // `segmentHelper` is that intermediary and `fixtureHelper` is the real
    // translation call; the pair must be BOUND on some live line, so an
    // intermediary that stopped translating cannot satisfy this test.
    // `forwardListSite` is the ONE construct that names the variables carried
    // into the launcher's environment. It is checked ON ITS OWN LINE rather than
    // "somewhere in the file": the first version of this pin looked for a line
    // naming both variables anywhere and was satisfied by the RESTORE line
    // (`$env:QUICKTEST_OMIT = $oldOmit; $env:SQLITE_TEST_PATTERN_LIST = …`), so
    // emptying the real list stayed GREEN. Measured, and fixed here.
    constexpr Driver kDrivers[] = {
        {"build-and-test.sh", false, "launch_path", "launch_bin=", "LAUNCH_",
         "${US}", "LEG_ENV_FORWARD"},
        {"build-and-test.ps1", true, "Convert-LaunchPath", "$legLaunchFixture =",
         "Get-SegmentArgs", "Kind = '", "$legForward"}};

    for (auto const& d : kDrivers) {
        auto const path = harnessDir() / d.name;
        ASSERT_TRUE(fs::exists(path)) << path;
        auto const lines = liveLines(path, d.powershell);
        ASSERT_FALSE(lines.empty()) << d.name;

        bool        translatesFixture = false;
        bool        guardsTheArgv     = false;
        bool        boundToTranslator = false;
        std::size_t segmentRecords    = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            auto const& line = lines[i];
            if (line.find(d.fixtureSite) != std::string::npos &&
                line.find(d.fixtureHelper) != std::string::npos) {
                translatesFixture = true;
            }
            // A SMALL WINDOW, not the same line: the .sh binds them in one
            // assignment but the .ps1's intermediary is a function whose body is
            // the next line. Four lines is enough for either shape and still far
            // too narrow to pair two unrelated mentions.
            if (line.find(d.segmentHelper) != std::string::npos) {
                for (std::size_t j = i; j < std::min(i + 4, lines.size()); ++j) {
                    if (lines[j].find(d.fixtureHelper) != std::string::npos) {
                        boundToTranslator = true;
                    }
                }
            }
            if (line.find("assert") != std::string::npos &&
                (line.find("Translated") != std::string::npos ||
                 line.find("translated") != std::string::npos)) {
                guardsTheArgv = true;
            }
            if (line.find(d.segmentNeedle) == std::string::npos) continue;
            ++segmentRecords;
            EXPECT_NE(line.find(d.segmentHelper), std::string::npos)
                << d.name
                << " builds a fixture segment whose script argument is NOT"
                   " translated into the launcher's namespace:\n  "
                << line
                << "\nThe first argument of every segment is the .test script"
                   " the fixture sources. Under a translating launcher an"
                   " untranslated one is opened as a RELATIVE file, missed, and"
                   " reported as a test failure rather than a harness bug.";
        }
        EXPECT_TRUE(translatesFixture)
            << d.name << " never translates the FIXTURE path (looked for a line"
                         " containing both '"
            << d.fixtureSite << "' and '" << d.fixtureHelper
            << "'). A launcher in another namespace cannot even be handed the"
               " binary.";
        EXPECT_TRUE(boundToTranslator)
            << d.name << " names '" << d.segmentHelper
            << "' at its segment records but never BINDS it to '"
            << d.fixtureHelper
            << "' on any live line. An intermediary that has stopped calling the"
               " translator satisfies the per-record check above while"
               " translating nothing.";
        EXPECT_TRUE(guardsTheArgv)
            << d.name
            << " never calls the assert-translated guard. Translating at"
               " construction is only safe with a net at the spawn point: a"
               " future segment kind that adds a path argument must be refused"
               " by name, not discovered three hours in.";
        EXPECT_GE(segmentRecords, 3u)
            << d.name << " has only " << segmentRecords
            << " segment record(s) matching '" << d.segmentNeedle
            << "' — the corpus engine builds three (tier, permutation resume,"
               " tier resume), so this pin has stopped seeing them and is"
               " vacuous. Fix the needle, do not delete the test.";

        // THE LAUNCHER TRANSLATION GOES THROUGH THE RESOLVER. This is the "one
        // implementation, two drivers" property stated as a needle: whatever a
        // driver's helper is called, it must ASK harness_legs.py.
        //
        // ⚠ Deliberately NOT a blanket ban on the word `wslpath`: both drivers
        // legitimately name it in their recipe-DERIVATION step, which spells a
        // WSL path the Windows way for the manifest — the opposite direction and
        // a different mechanism, tracked separately as
        // D-HARNESS-STAGING-PATH-TRANSLATION-IS-HAND-ROLLED-AND-HOST-KEYED. A
        // pin that outlawed the word would be "fixed" by renaming a variable.
        // ★ OVER LIVE LINES, not raw text. Both drivers DOCUMENT these flags in
        // their headers, so a raw-text search is satisfied by a comment — the
        // same lesson `liveLines` already exists for. And each capability is
        // pinned by the flag that ONLY its call site carries: `--env-transfers`
        // (the vocabulary read) would be matched as a prefix of nothing else,
        // but the RESOLUTION is `--carrier-current`, and that is what a driver
        // losing the capability actually deletes.
        bool asksForTranslation = false, asksForAssertion = false;
        bool asksForEnvVocabulary = false, asksForEnvResolution = false;
        for (auto const& line : lines) {
            if (line.find("--translate-path") != std::string::npos) asksForTranslation = true;
            if (line.find("--assert-translated") != std::string::npos) asksForAssertion = true;
            if (line.find("--env-transfers") != std::string::npos) asksForEnvVocabulary = true;
            if (line.find("--carrier-current") != std::string::npos) asksForEnvResolution = true;
        }
        EXPECT_TRUE(asksForTranslation && asksForAssertion)
            << d.name
            << " does not ask harness_legs.py to translate (--translate-path: "
            << asksForTranslation << ") and guard (--assert-translated: "
            << asksForAssertion
            << ") its launcher paths. Its launcher translation is therefore"
               " implemented somewhere inside this driver — and a capability"
               " that exists in one driver and not the other is the silent"
               " harness bug this file exists to end.";
        EXPECT_TRUE(asksForEnvVocabulary && asksForEnvResolution)
            << d.name
            << " does not ask harness_legs.py how its run environment reaches a"
               " launched process (--env-transfers: " << asksForEnvVocabulary
            << ", --carrier-current: " << asksForEnvResolution
            << "). On this driver's own host that may be a no-op today; a"
               " capability present in one driver and absent from the other is"
               " the defect regardless.";

        // The forward list itself, ON ITS OWN CONSTRUCT.
        bool forwardsThePatternList = false;
        for (auto const& line : lines) {
            if (line.find(d.forwardListSite) == std::string::npos) continue;
            if (line.find("SQLITE_TEST_PATTERN_LIST") != std::string::npos &&
                line.find("QUICKTEST_OMIT") != std::string::npos) {
                forwardsThePatternList = true;
            }
            // NAMESPACE-NEUTRAL VALUES ONLY. Forwarding a HOST path into a
            // foreign namespace is worse than not forwarding it: the child gets
            // a path it cannot resolve and uses it anyway.
            EXPECT_EQ(line.find("TCL_LIBRARY"), std::string::npos)
                << d.name << " forwards a HOST PATH into the launcher's"
                             " environment:\n  "
                << line;
            EXPECT_EQ(line.find("PATH'"), std::string::npos)
                << d.name << " forwards this host's PATH into the launcher's"
                             " environment:\n  "
                << line;
        }
        EXPECT_TRUE(forwardsThePatternList)
            << d.name << ": its forward list (" << d.forwardListSite
            << ") does not name both SQLITE_TEST_PATTERN_LIST and"
               " QUICKTEST_OMIT. The first is how the RESUME ENGINE selects its"
               " files: MEASURED 2026-08-04, a launched fixture that could not"
               " see it re-ran the corpus from the beginning after an abort and"
               " reported it as progress.";
    }
}

// A SUMMARY LINE IS NOT PROOF THAT A SUITE RAN. Both drivers must refuse a
// segment that completed ZERO test files, whatever tester.tcl printed.
//
// MEASURED 2026-08-04 (TF-C116) and it is why this pin exists: an environment
// variable that arrived EMPTY-BUT-SET through a cross-OS launcher made the tier
// select no files at all; tester.tcl finalised and printed `0 errors out of 1
// tests`; the driver reported "corpus GREEN — 0 errors out of 1 tests" beside
// "0 test file(s) completed". A false pass is the worst outcome this harness can
// produce, and the floor belongs in the verdict ladder rather than beside the
// one cause that happened to expose it.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): delete the
// zero-files-completed branch from either driver and this fails naming it.
TEST_F(HarnessLegs, NeitherDriverCallsAZeroFileRunGreen) {
    struct Driver {
        char const* name;
        bool        powershell;
        char const* guard;   // the counter the branch must test
    };
    constexpr Driver kDrivers[] = {{"build-and-test.sh", false, "files_done"},
                                   {"build-and-test.ps1", true, "$filesDone"}};
    for (auto const& d : kDrivers) {
        auto const lines = liveLines(harnessDir() / d.name, d.powershell);
        ASSERT_FALSE(lines.empty()) << d.name;
        bool hasFloor = false;
        for (auto const& line : lines) {
            if (line.find(d.guard) == std::string::npos) continue;
            // `elif [[ "$files_done" -eq 0 ]]` / `} elseif ($filesDone -eq 0) {`
            if (line.find("-eq 0") == std::string::npos) continue;
            if (line.find("els") == std::string::npos &&
                line.find("if") == std::string::npos) {
                continue;
            }
            hasFloor = true;
        }
        EXPECT_TRUE(hasFloor)
            << d.name << " has no verdict branch testing '" << d.guard
            << " -eq 0'. Without it a run that executed NOTHING is reported"
               " GREEN on the strength of tester.tcl's summary line — measured"
               " 2026-08-04, `corpus GREEN — 0 errors out of 1 tests` beside `0"
               " test file(s) completed`.";
    }
}

// ── 8. The declared library-acquisition route ──────────────────────────────
//
// D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER. Operator
// principle, 2026-08-04: "we should be able to build macho on linux. ANY LEG
// MUST BE ABLE TO BUILD TO ANY LEG."
//
// The catalogue already says every leg is BUILT on every host and only the RUN
// is gated (§2/§3 above), so a leg whose libraries exist only on one kind of
// machine made that promise false in practice: before this, both macho legs
// were `host-system`, which off a Mac means "hope this box has a Darwin
// libtcl". The mechanism was never missing — `build-and-test.sh` had downloaded
// Ubuntu ports `.deb`s for the arm64 leg since TF-C68 — what was missing was
// GENERALITY (one hand-written provider serving one leg) and a SECOND
// IMPLEMENTATION (`build-and-test.ps1` could not acquire at all).
//
// These pins hold the general form to account: the route is DECLARED, it is
// CHECKSUM-PINNED, it REFUSES rather than improvising, the identity an acquired
// stand-in is recorded under is DECLARED rather than inherited from whoever
// packaged it, and — the anti-regression pin that matters most — BOTH drivers
// implement every provider the catalogue declares.

namespace {

// Every provider name the catalogue actually uses.
[[nodiscard]] std::set<std::string> declaredProviders(fs::path const& catalogue) {
    std::set<std::string> out;
    auto const doc = json::parse(fileText(catalogue));
    for (auto const& leg : doc.at("legs")) {
        out.insert(leg.at("build").at("libraries").at("provider")
                       .get<std::string>());
    }
    return out;
}

// The label of the first leg declaring the acquisition route, or "".
[[nodiscard]] std::string firstAcquiringLeg(fs::path const& catalogue) {
    auto const doc = json::parse(fileText(catalogue));
    for (auto const& leg : doc.at("legs")) {
        if (leg.at("build").at("libraries").at("provider").get<std::string>()
            == "pinned-archive") {
            return leg.at("label").get<std::string>();
        }
    }
    return {};
}

}  // namespace

// AN ACQUIRED LIBRARY IS A STAND-IN, AND ITS EMBEDDED IDENTITY IS THE
// PACKAGER'S, NOT THE TARGET'S.
//
// MEASURED 2026-08-04 on this host, and it is the whole reason the key exists:
// the MacPorts Tcl/zlib dylibs carry `LC_ID_DYLIB = /opt/local/lib/...`, DSS
// records a resolved library's embedded identity as the LC_LOAD_DYLIB, and a
// Mach-O cross-built against them came out demanding
// `/opt/local/lib/libtcl8.6.dylib`. That is a dyld LOAD failure on the target
// Mac — not a build error, and not something this host can observe. So the
// identity to record is DECLARED per acquired member, and the plan carries it
// to the drivers so neither has to decide it.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): remove an
// `importName` from any acquired member and the lint refuses the catalogue —
// asserted here directly rather than described.
TEST_F(HarnessLegs, AnAcquiredLibraryDeclaresTheIdentityItIsRecordedUnder) {
    auto const r = run({"--plan", "--host-os", "linux", "--host-arch", "x86_64"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    auto const plan = json::parse(r.output);
    unsigned acquiring = 0;
    for (auto const& leg : plan.at("legs")) {
        auto const  label    = leg.at("label").get<std::string>();
        auto const& libs     = leg.at("build").at("libraries");
        auto const  provider = libs.at("provider").get<std::string>();
        auto const  tcl      = libs.value("tclImportName", std::string{});
        auto const  z        = libs.value("zImportName", std::string{});
        if (provider == "pinned-archive") {
            ++acquiring;
            EXPECT_FALSE(tcl.empty())
                << label << " acquires its Tcl but declares no identity to"
                            " record it under. The downloaded file's own"
                            " LC_ID_DYLIB/DT_SONAME belongs to whoever packaged"
                            " it, so inheriting it bakes that packager's prefix"
                            " into the artefact and fails at LOAD time on the"
                            " target machine.";
            EXPECT_FALSE(z.empty()) << label << " (zlib): same.";
        } else {
            EXPECT_TRUE(tcl.empty() && z.empty())
                << label << " uses provider '" << provider << "', which hands"
                            " over a library already carrying the right embedded"
                            " identity — overriding it would replace a true name"
                            " with a declared one.";
        }
    }
    EXPECT_GT(acquiring, 0u)
        << "no leg declares the acquisition route, so this test asserts nothing."
           " If the last acquiring leg was deliberately removed, remove this pin"
           " in the same commit rather than leaving it vacuous.";

    // The refusal, witnessed rather than asserted about.
    MutatedCatalogue m{catalogue_, "legs-no-import-name"};
    bool stripped = false;
    for (auto& leg : m.doc().at("legs")) {
        auto& libs = leg.at("build").at("libraries");
        if (libs.at("provider").get<std::string>() != "pinned-archive") continue;
        for (auto& a : libs.at("acquire").at("archives")) {
            for (auto& mem : a.at("members")) {
                mem.erase("importName");
                stripped = true;
            }
        }
        break;
    }
    ASSERT_TRUE(stripped) << "nothing to strip — the mutation is vacuous";
    auto const bad = runResolver({"--lint"}, m.commit());
    ASSERT_TRUE(bad.spawned) << bad.diagnostic;
    EXPECT_NE(bad.exitCode, 0u)
        << "the lint accepted an acquired member with no declared identity:\n"
        << bad.output;
    EXPECT_NE(bad.output.find("importName"), std::string::npos) << bad.output;
}

// A BUILD THAT FETCHES THIRD-PARTY BINARIES IS A SUPPLY-CHAIN SURFACE.
// Every archive pins a sha256, the download is filed UNDER that digest (so "is
// the cached copy the thing we pinned?" is answered by re-hashing rather than by
// trusting a file name), and the source is https.
//
// RED-ON-DISABLE: replace any `sha256` with a non-digest and the lint refuses —
// asserted here.
TEST_F(HarnessLegs, TheAcquisitionRouteIsChecksumPinnedAndContentAddressed) {
    auto const providers = declaredProviders(catalogue_);
    ASSERT_TRUE(providers.count("pinned-archive"))
        << "no leg declares 'pinned-archive'; this pin would be vacuous";
    auto const doc      = json::parse(fileText(catalogue_));
    unsigned   archives = 0;
    for (auto const& leg : doc.at("legs")) {
        auto const  label = leg.at("label").get<std::string>();
        auto const& libs  = leg.at("build").at("libraries");
        if (libs.at("provider").get<std::string>() != "pinned-archive") continue;
        auto const r = run({"--acquire-plan", label});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const  ap   = json::parse(r.output);
        auto const  spec = leg.at("spec").get<std::string>();
        std::string const specArch = spec.substr(0, spec.find(':'));
        EXPECT_EQ(ap.at("targetArch").get<std::string>(), specArch)
            << label << ": the slice taken must be the LEG's target arch, never"
                        " anything about the host doing the acquiring.";
        for (auto const& a : ap.at("archives")) {
            ++archives;
            auto const url = a.at("url").get<std::string>();
            auto const sha = a.at("sha256").get<std::string>();
            EXPECT_EQ(sha.size(), 64u) << label << " " << url;
            EXPECT_EQ(sha.find_first_not_of("0123456789abcdef"),
                      std::string::npos)
                << label << " " << url << ": sha256 is not lowercase hex";
            EXPECT_EQ(url.rfind("https://", 0), 0u)
                << label << ": " << url << " is not https. The digest"
                                           " authenticates the CONTENT; TLS"
                                           " authenticates the SOURCE.";
            EXPECT_NE(a.at("download").get<std::string>().find(sha),
                      std::string::npos)
                << label << ": the download is not filed under its digest, so a"
                            " corrupt cache entry cannot be detected by name.";
        }
    }
    EXPECT_GT(archives, 0u) << "no archives inspected — vacuous";

    MutatedCatalogue m{catalogue_, "legs-unpinned"};
    bool             unpinned = false;
    for (auto& leg : m.doc().at("legs")) {
        auto& libs = leg.at("build").at("libraries");
        if (libs.at("provider").get<std::string>() != "pinned-archive") continue;
        libs.at("acquire").at("archives").at(0)["sha256"] = "not-a-digest";
        unpinned = true;
        break;
    }
    ASSERT_TRUE(unpinned) << "nothing to unpin — the mutation is vacuous";
    auto const bad = runResolver({"--lint"}, m.commit());
    ASSERT_TRUE(bad.spawned) << bad.diagnostic;
    EXPECT_NE(bad.exitCode, 0u)
        << "the lint accepted an archive with no pinned digest:\n" << bad.output;
}

// THE CAPABILITY-PAIR PIN — the half of this anchor that is about the DRIVERS.
//
// `build-and-test.ps1` used to end its provider switch with "library provider
// '$provider' is NOT IMPLEMENTED by build-and-test.ps1", which made the one
// working provider Linux-driver-only. This project's rule is that a capability
// in one driver and not the other is a SILENT harness bug, and this is the third
// time that shape has cost a cycle.
//
// The check is deliberately STRUCTURAL — a provider must appear as a real ARM of
// each driver's provider dispatch, not merely as a word somewhere in the file.
// Naming it inside a "not implemented" message would otherwise satisfy a naive
// substring search, which is exactly the state being repaired.
//
// ★ ONE PROVIDER IS EXEMPT, AND THE EXEMPTION RETIRES ITSELF.
// `ubuntu-ports-arm64` is the ORIGINAL bespoke provider — `build-and-test.sh`
// implements it with `curl` + `dpkg-deb`, and neither tool is a thing the .ps1
// can call. Absorbing it into the general route needs a `.deb` reader in the
// resolver, and MEASURED 2026-08-04 that is not safely doable yet: a modern
// `.deb`'s inner archive can be zstd, whose stdlib module arrived in Python
// 3.14 — this Windows host has 3.14, the WSL leg has 3.12 — so a zstd-based
// route would be a capability that exists on one host and not another, which is
// the same defect one level down. It is therefore listed HERE, by name, with
// the anchor, rather than quietly passing.
//
// The exemption cannot rot: the loop below asserts a listed provider is STILL
// missing from exactly one driver. Implement it and this test REDS, demanding
// the entry be deleted. Add a second entry and the size assertion reds. An
// allow-list that can only shrink is a ratchet; one that can grow is a licence.
[[nodiscard]] std::set<std::string> knownDriverLocalProviders() {
    // D-HARNESS-UBUNTU-PORTS-PROVIDER-NOT-GENERALISED-TO-PINNED-ARCHIVE
    return {"ubuntu-ports-arm64"};
}

// RED-ON-DISABLE: delete either driver's arm for any declared provider and this
// fails naming the driver and the provider.
TEST_F(HarnessLegs, BothDriversImplementEveryProviderTheCatalogueDeclares) {
    auto const providers = declaredProviders(catalogue_);
    ASSERT_GE(providers.size(), 2u) << "one provider — the pin proves nothing";
    auto const exempt = knownDriverLocalProviders();
    EXPECT_EQ(exempt.size(), 1u)
        << "the driver-local exemption list may only SHRINK. A second entry"
           " means a new capability was added to one driver and not the other,"
           " which is the defect D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-"
           "LEG-IN-ONE-DRIVER exists to end.";
    struct Driver {
        char const* name;
        bool        powershell;
    };
    constexpr Driver kDrivers[] = {{"build-and-test.sh", false},
                                   {"build-and-test.ps1", true}};
    std::map<std::string, unsigned> armsFor;
    for (auto const& d : kDrivers) {
        auto const lines = liveLines(harnessDir() / d.name, d.powershell);
        ASSERT_FALSE(lines.empty()) << d.name;
        for (auto const& p : providers) {
            // `<provider>)` opens a bash case arm; `'<provider>' {` opens a
            // PowerShell switch arm. Both ARE the dispatch.
            std::string const needle =
                d.powershell ? ("'" + p + "'") : (p + ")");
            bool arm = false;
            for (auto const& line : lines) {
                auto const at = line.find(needle);
                if (at == std::string::npos) continue;
                if (d.powershell
                    && line.find('{', at + needle.size()) == std::string::npos) {
                    continue;   // a mention, not a switch arm
                }
                arm = true;
                break;
            }
            if (arm) ++armsFor[p];
            if (exempt.count(p)) continue;
            EXPECT_TRUE(arm)
                << d.name << " has no dispatch arm for library provider '" << p
                << "', which the catalogue DECLARES. A leg is then buildable"
                   " from one driver and not the other — the silent harness-bug"
                   " shape this anchor exists to end.";
        }
    }
    // The exemption retires itself: a listed provider must STILL be missing
    // from exactly one driver. The day it is implemented, this reds and the
    // entry has to go.
    for (auto const& p : exempt) {
        if (!providers.count(p)) {
            ADD_FAILURE()
                << "provider '" << p << "' is exempted but no longer declared by"
                   " the catalogue — delete the exemption.";
            continue;
        }
        EXPECT_EQ(armsFor[p], 1u)
            << "provider '" << p << "' is on the driver-local exemption list but"
               " now has " << armsFor[p] << " driver arm(s) instead of 1. If it"
               " was implemented in both drivers, DELETE it from"
               " knownDriverLocalProviders() — a stale exemption is how a closed"
               " gap comes to look open.";
    }
}

// IT REFUSES RATHER THAN IMPROVISING.
// Offline with a cold cache is the case that matters: the tempting behaviour is
// to fall back to "whatever tcl is on this machine", which would build the leg
// against a foreign library and report it green.
TEST_F(HarnessLegs, AcquisitionRefusesRatherThanImprovisingWithAColdCache) {
    auto const label = firstAcquiringLeg(catalogue_);
    ASSERT_FALSE(label.empty()) << "no acquiring leg — vacuous";
    auto const cold = scratch_->path() / "cold-cache";
    ASSERT_FALSE(fs::exists(cold)) << "the cache root must be absent: " << cold;

    auto const r =
        run({"--acquire", label, "--offline", "--cache-root", cold.string()});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_NE(r.exitCode, 0u)
        << "--offline with a cold cache SUCCEEDED. Somewhere it improvised:\n"
        << r.output;
    EXPECT_NE(r.output.find("https://"), std::string::npos)
        << "the refusal must name the source it could not reach:\n" << r.output;

    // And it refused BEFORE creating anything: a run that cannot succeed must
    // not leave a half-built cache tree behind for the next run to trust.
    EXPECT_FALSE(fs::exists(cold))
        << "a failed offline acquisition created " << cold;
}

// THE COMPILER FLAG IS NAMED IN ONE FILE, NOT IN TWO DRIVERS — the same argument
// `--translate-path` makes for `wslpath`. A driver that spelled it itself is a
// capability that can exist in one driver and not the other.
TEST_F(HarnessLegs, TheRecordedIdentityFlagIsNamedInExactlyOneFile) {
    // Without an override, the argv is what every leg has always passed.
    auto const plain = run({"--resolve-library-argv", "/tmp/libz.so.1"});
    ASSERT_TRUE(plain.spawned) << plain.diagnostic;
    ASSERT_EQ(plain.exitCode, 0u) << plain.output;
    auto const plainTokens = splitLines(plain.output);
    ASSERT_EQ(plainTokens.size(), 2u) << plain.output;
    EXPECT_EQ(plainTokens[0], "--resolve-library");
    EXPECT_EQ(plainTokens[1], "/tmp/libz.so.1");

    // With one, the flag appears — and it is the RESOLVER that chose it.
    auto const over = run({"--resolve-library-argv", "/tmp/libz.1.dylib",
                           "--import-name", "@loader_path/libz.1.dylib"});
    ASSERT_TRUE(over.spawned) << over.diagnostic;
    ASSERT_EQ(over.exitCode, 0u) << over.output;
    // ★ THE OVERRIDE IS A VALUE SUFFIX, NOT A SECOND FLAG:
    // `--resolve-library <path>[=<import-name>]`, which the compiler splits on
    // its LAST `=`. An earlier draft of this test asserted the opposite — that
    // the first token must NOT be `--resolve-library` — which was a guess made
    // before the compiler side landed, and it went red against a correct
    // resolver. The shape below is the one that is actually true, and it is
    // asserted on the TOKENS rather than on the joined string so a change in
    // either half reds here, in the one place that has to be updated.
    auto const overTokens = splitLines(over.output);
    ASSERT_EQ(overTokens.size(), 2u) << over.output;
    EXPECT_EQ(overTokens[0], "--resolve-library") << over.output;
    EXPECT_EQ(overTokens[1], "/tmp/libz.1.dylib=@loader_path/libz.1.dylib")
        << "the identity must ride as a `=<import-name>` suffix on the path:\n"
        << over.output;

    // A compiler whose --help does not carry the flag is a LOUD refusal. The
    // stand-in is the python interpreter: a real executable whose help text
    // certainly lacks it, so the probe is exercised rather than mocked.
    auto const py = pythonPath();
    ASSERT_FALSE(py.empty());
    auto const refused = run({"--resolve-library-argv", "/tmp/libz.1.dylib",
                              "--import-name", "@loader_path/libz.1.dylib",
                              "--dss", py});
    ASSERT_TRUE(refused.spawned) << refused.diagnostic;
    EXPECT_NE(refused.exitCode, 0u)
        << "a compiler that cannot record the declared identity was accepted."
           " Dropping the override links clean here and fails at LOAD time on a"
           " machine this host cannot observe:\n"
        << refused.output;

    // Neither driver may spell the compiler's flag itself — it must come from
    // the resolver. Matched as a WHOLE TOKEN: `--resolve-library-argv` is the
    // RESOLVER's own subcommand and a driver has to spell that to call it, so a
    // naive substring scan would forbid the very thing being required. (That is
    // not hypothetical — the first draft of this check did exactly that.)
    struct Driver {
        char const* name;
        bool        powershell;
    };
    constexpr Driver kDrivers[] = {{"build-and-test.sh", false},
                                   {"build-and-test.ps1", true}};
    std::string const compilerFlag = "--resolve-library";
    for (auto const& d : kDrivers) {
        for (auto const& line : liveLines(harnessDir() / d.name, d.powershell)) {
            for (std::size_t at = line.find(compilerFlag);
                 at != std::string::npos;
                 at = line.find(compilerFlag, at + 1)) {
                char const next = at + compilerFlag.size() < line.size()
                                      ? line[at + compilerFlag.size()]
                                      : '\0';
                if (next == '-' || std::isalnum(static_cast<unsigned char>(next))) {
                    continue;   // `--resolve-library-argv`, the resolver's verb
                }
                ADD_FAILURE()
                    << d.name << " spells the COMPILER's --resolve-library flag"
                                 " itself:\n  " << line
                    << "\nIt belongs in harness_legs.py alone (--resolve-library"
                       "-argv), so the two drivers cannot drift and so the"
                       " import-name suffix can never be silently dropped by"
                       " one of them.";
            }
        }
    }
}

// ── 9. NOTHING INVOKES `wsl.exe` WITHOUT `-e` ──────────────────────────────
//
// D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL.
//
// THE FORBIDDEN SHAPE, in one line: `wsl.exe <anything-but--e> …`.
//
// WHY. `wsl.exe <cmd>` does not run <cmd>. WSL reconstructs a command LINE from
// the remaining argv and feeds it to the distro's DEFAULT SHELL, which strips
// quoting and performs expansions BEFORE the named binary is ever reached — so
// the payload is parsed twice and the first pass happens where nobody is
// looking. ✔MEASURED 2026-08-04 on this host, one variable changed, same input:
//     wsl.exe    bash -lc "printf '[%s]\n' 'echo A=$(uname -m)'"  ->  [echo A=x86_64]
//     wsl.exe -e bash -lc "printf '[%s]\n' 'echo A=$(uname -m)'"  ->  [echo A=$(uname -m)]
//
// ★ QUOTING IS NOT THE FIX, and that is why this is a test and not a review
// note. ✔MEASURED at the real call site (Invoke-PosixCommand's payload): with no
// `-e`, a SINGLE-QUOTED `$HOME` inside the payload still expanded, arriving as
// `[lit /home/rafael and * and a\b]` instead of `[lit $HOME and * and a\b]`,
// because the outer shell removed the quotes first. Every escaping fix
// therefore looks correct and still leaks.
//
// ★ `--` IS NOT `-e` EITHER — it is documented as "pass the remaining command
// line as is", and "as is" means "to the shell". ✔MEASURED the same day:
// `wsl.exe -- /nope` answers `/bin/bash: line 1: /nope: No such file` while
// `wsl.exe -e /nope` answers `execvpe(/nope) failed`; and one argument
// `…/g/*.test` reached the callee as TWO arguments under `--` and as ONE under
// `-e`. That is why `--` is called out by name below instead of being lumped in
// with "some other token" — it reads like the safe spelling and is not.
//
// WHAT IT COST, so nobody re-litigates the severity: tools/ssh-arm64-vps.ps1 ran
// `wsl.exe bash -lc "ssh … $Command"`, so `-Command 'hostname; uname -m'`
// printed the VPS hostname and then the LOCAL WSL architecture — x86_64 for an
// aarch64 box — while exiting 0. A cross-host verification instrument answering
// with the wrong host's data, silently. The same defect sat under
// `wslpath: C:ab`, where it was misattributed to wslpath eating backslashes and
// papered over with a separator rewrite (section 8's corrected comment).
//
// THE RULE, and why it is shaped this way rather than "the file must not
// contain `wsl.exe` without `-e`":
//   · over LIVE lines (`liveLines`), because every driver DOCUMENTS the shape it
//     removed — a raw-text rule would be satisfied by deleting the explanation,
//     which is the lesson `liveLines` already exists for;
//   · only in COMMAND POSITION (first token, or after `&` / `;` / `(` / `{` /
//     backtick), because `Get-Command wsl.exe -ErrorAction …` RESOLVES the
//     launcher without running it, and a diagnostic string may legitimately name
//     it in prose. This needs no suppression list: neither shape is command
//     position, by construction;
//   · `|` is deliberately NOT a command-position marker. build-and-test.sh
//     carries `grep -qiE 'microsoft|wsl' /proc/version`, and a rule that read
//     that as a pipeline would red on a regex. A pipeline INTO wsl.exe is not a
//     shape this harness uses, and buying it would cost a false positive today;
//   · a bare `wsl` counts only when followed by WHITESPACE — an invocation is
//     always followed by its argv. That one condition is what keeps the same
//     regex, `build-wsl/` and `$wslKey` out of it without naming any of them;
//   · a PowerShell SPLAT (`& wsl.exe @a`) is the correct fix's own shape — there
//     is no string left to escape — so it is accepted only when the array it
//     splats is bound to `-e` nearby. That is how tools/ssh-arm64-vps.ps1 passes.
//
// COVERAGE IS BY DIRECTORY, NOT BY LIST: both sqlite drivers plus every
// `tools/*.ps1` and `tools/*.sh`, so a NEW tools script is governed the day it
// lands. The catalogue's launcher argv and the resolver's translator argv are
// held to the same rule from their DATA, in the second test — and those are the
// two places that actually carried `--`.
//
// ⚠ ONE THING THIS RULE DOES NOT COVER, labelled rather than left implicit:
// `.claude/skills/dss-state/driver.mjs` spawns WSL from a JS argv ARRAY
// (`spawnSync('wsl', ['-e', …])`), not from a command line, so none of the
// shapes above apply to it. It was VERIFIED BY READING on 2026-08-04 — lines
// 171, 244, 248-249 and 250 all put `-e` first — and it is left unscanned
// because a line-oriented rule would either miss it or invent false positives.

namespace {

struct Script {
    fs::path path;
    bool     powershell;
};

// Every script this rule governs. `tools/` is enumerated rather than listed so
// a new script there cannot land outside the rule.
[[nodiscard]] std::vector<Script> shellScriptsUnderTest() {
    std::vector<Script> out;
    out.push_back({harnessDir() / "build-and-test.ps1", true});
    out.push_back({harnessDir() / "build-and-test.sh", false});
    std::error_code       ec;
    std::vector<fs::path> tools;
    for (auto const& e : fs::directory_iterator(repoRoot() / "tools", ec)) {
        if (!e.is_regular_file()) continue;
        auto const ext = e.path().extension().string();
        if (ext == ".ps1" || ext == ".sh") tools.push_back(e.path());
    }
    std::sort(tools.begin(), tools.end());   // a stable failure order
    for (auto const& p : tools) {
        out.push_back({p, p.extension() == ".ps1"});
    }
    return out;
}

[[nodiscard]] bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The character before `at`, ignoring spaces/tabs; '\0' at the start of a line.
[[nodiscard]] char precedingSymbol(std::string const& line, std::size_t at) {
    while (at > 0 && (line[at - 1] == ' ' || line[at - 1] == '\t')) --at;
    return at == 0 ? '\0' : line[at - 1];
}

// The whitespace-delimited token FOLLOWING the one that starts at `at`.
[[nodiscard]] std::string tokenAfter(std::string const& line, std::size_t at) {
    std::size_t i = at;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    std::size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    return line.substr(i, j - i);
}

[[nodiscard]] std::vector<std::string> splitWords(std::string const& s) {
    std::vector<std::string> out;
    std::istringstream       in{s};
    std::string              w;
    while (in >> w) out.push_back(w);
    return out;
}

// The 1-based line number of `content` in the RAW file, or 0.
//
// ★ NOT the index into `liveLines`, and this is not a nicety: the first version
// of this test reported the live index, which pointed at build-and-test.ps1:45
// for a defect on line 373 — a diagnostic that sends the reader to an unrelated
// line is worse than one that gives no line at all. `liveLines` hands back its
// lines verbatim, so the raw number is recoverable by looking the text back up.
[[nodiscard]] std::size_t rawLineNumberOf(fs::path const&    p,
                                          std::string const& content) {
    std::istringstream in{fileText(p)};
    std::string        line;
    for (std::size_t n = 1; std::getline(in, line); ++n) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == content) return n;
    }
    return 0;
}

// `@a` -> `a`; "" when `tok` is not a PowerShell splat.
[[nodiscard]] std::string splattedVariable(std::string const& tok) {
    if (tok.size() < 2 || tok[0] != '@') return {};
    std::string name;
    for (std::size_t i = 1; i < tok.size() && isWordChar(tok[i]); ++i) {
        name.push_back(tok[i]);
    }
    return name;
}

// Is `$<name>` bound to an argv naming `-e` within the window ABOVE `at`? The
// window mirrors `boundToTranslator`'s reasoning: wide enough for a construct
// split over a few lines, far too narrow to pair two unrelated mentions.
[[nodiscard]] bool splatIsBoundToExec(std::vector<std::string> const& lines,
                                      std::size_t                     at,
                                      std::string const&              name) {
    std::string const needle = "$" + name;
    std::size_t const first  = at >= 12 ? at - 12 : 0;
    for (std::size_t i = first; i <= at; ++i) {
        auto const pos = lines[i].find(needle);
        if (pos == std::string::npos) continue;
        auto const after = pos + needle.size();
        if (after < lines[i].size() && isWordChar(lines[i][after])) continue;
        if (lines[i].find('=') == std::string::npos) continue;
        if (lines[i].find("'-e'") != std::string::npos ||
            lines[i].find("\"-e\"") != std::string::npos ||
            lines[i].find("'--exec'") != std::string::npos ||
            lines[i].find("\"--exec\"") != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct Mention {
    std::size_t at;
    std::string spelling;
};

// Every offset at which this line names the WSL launcher AS A COMMAND WORD.
[[nodiscard]] std::vector<Mention> wslMentions(std::string const& line) {
    std::vector<Mention> out;
    for (std::size_t at = line.find("wsl"); at != std::string::npos;
         at = line.find("wsl", at + 1)) {
        // LEFT boundary: `build-wsl/`, `$wslKey`, `--no-wsl` and
        // `$script:HostNeedsWsl` are names, not invocations.
        if (at > 0) {
            char const p = line[at - 1];
            if (isWordChar(p) || p == '-' || p == '.' || p == '/' ||
                p == '\\' || p == '$') {
                continue;
            }
        }
        std::string spelling = "wsl";
        if (line.compare(at, 7, "wsl.exe") == 0) spelling = "wsl.exe";
        // RIGHT boundary, and it must be WHITESPACE: an invocation is always
        // followed by its argv, while `'microsoft|wsl'` is followed by a quote.
        auto const after = at + spelling.size();
        if (after >= line.size()) continue;
        if (line[after] != ' ' && line[after] != '\t') continue;
        out.push_back({at, spelling});
    }
    return out;
}

}  // namespace

TEST_F(HarnessLegs, NoScriptInvokesWslWithoutExec) {
    std::size_t invocationsSeen = 0;
    for (auto const& script : shellScriptsUnderTest()) {
        ASSERT_TRUE(fs::exists(script.path)) << script.path;
        auto const name  = script.path.filename().string();
        auto const lines = liveLines(script.path, script.powershell);
        ASSERT_FALSE(lines.empty()) << name << " has no live lines";

        for (std::size_t i = 0; i < lines.size(); ++i) {
            auto const& line = lines[i];
            auto const  mentions = wslMentions(line);
            auto const  at = mentions.empty() ? 0 : rawLineNumberOf(script.path, line);
            for (auto const& m : mentions) {
                auto const next = tokenAfter(line, m.at);

                // `--` first and UNCONDITIONALLY, command position or not: it is
                // wrong even as advice an operator pastes, so it is refused
                // wherever it is written.
                EXPECT_NE(next, "--")
                    << name << ':' << at << " spells `" << m.spelling
                    << " --`:\n  " << line
                    << "\n`--` is NOT `--exec`. MEASURED: `wsl.exe -- /nope`"
                       " answers `/bin/bash: line 1: /nope: No such file` where"
                       " `wsl.exe -e /nope` answers `execvpe(/nope) failed` — so"
                       " `--` hands the whole argv to the distro's default shell,"
                       " which re-expands it. Use `-e`.";

                char const prev = precedingSymbol(line, m.at);
                bool const commandPosition =
                    prev == '\0' || prev == '&' || prev == ';' || prev == '(' ||
                    prev == '{' || prev == '`';
                if (!commandPosition) continue;   // a mention, not an invocation
                ++invocationsSeen;

                if (next == "-e" || next == "--exec") continue;
                std::string const var = splattedVariable(next);
                if (!var.empty()) {
                    EXPECT_TRUE(splatIsBoundToExec(lines, i, var))
                        << name << ':' << at << " splats `" << next
                        << "` into " << m.spelling
                        << " but nothing within 12 live lines above binds `$"
                        << var << "` to an argv naming '-e':\n  " << line
                        << "\nA real argv is the RIGHT fix for this defect — it"
                           " is what tools/ssh-arm64-vps.ps1 does — but only if"
                           " `-e` is actually in it.";
                    continue;
                }

                ADD_FAILURE()
                    << name << ':' << at << " invokes `" << m.spelling
                    << "` without `-e`:\n  " << line
                    << "\nThe next token is '" << next
                    << "'. D-TOOLS-WSL-EXE-WITHOUT-DASH-E-RUNS-A-LOCAL-SHELL:"
                       " `wsl.exe <cmd>` does not run <cmd>, it hands the"
                       " reconstructed command line to the distro's DEFAULT"
                       " SHELL, which strips quoting and expands ON THIS MACHINE"
                       " first. MEASURED: the same input string gives"
                       " [echo A=x86_64] without `-e` and [echo A=$(uname -m)]"
                       " with it, and a SINGLE-QUOTED $HOME in a payload still"
                       " expanded. Quoting cannot fix it — pass `-e`, or build a"
                       " real argv and splat it as tools/ssh-arm64-vps.ps1 does.";
            }
        }
    }
    // NON-VACUITY. Every invocation above could stop being RECOGNISED — a
    // renamed helper, a driver that stops shelling out, a boundary rule that
    // grew too strict — and this test would go green by seeing nothing. The
    // floor is what the two sqlite drivers and ssh-arm64-vps.ps1 supply today.
    EXPECT_GE(invocationsSeen, 4u)
        << "only " << invocationsSeen
        << " wsl invocation(s) were RECOGNISED across the scanned scripts, so"
           " this rule has stopped seeing the shape it governs and is vacuous."
           " Fix the recogniser, do not delete the test.";
}

// The same rule over the two places the spelling is DATA rather than script
// text — and they are the places that actually carried `--`: the catalogue's
// launcher argv, and the resolver's declared translator argv. Neither is a
// command line, so the line rule above cannot see them; both end up as argv[0]
// of a real process, so the defect is identical.
TEST_F(HarnessLegs, NoDeclaredWslArgvOmitsExec) {
    auto const isWsl = [](std::string const& s) {
        auto const  slash = s.find_last_of("/\\");
        std::string tail  = slash == std::string::npos ? s : s.substr(slash + 1);
        for (auto& c : tail) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return tail == "wsl" || tail == "wsl.exe";
    };

    std::size_t launchers = 0;
    auto const  doc       = json::parse(fileText(catalogue_));
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        for (auto const& entry : leg.at("launchers")) {
            auto const cmd = entry.at("command").get<std::vector<std::string>>();
            if (cmd.empty() || !isWsl(cmd.front())) continue;
            ++launchers;
            ASSERT_GE(cmd.size(), 2u)
                << "leg '" << label << "': launcher for ("
                << entry.at("hostOs") << ", " << entry.at("hostArch")
                << ") is a bare `" << cmd.front() << "` with no `-e`.";
            EXPECT_TRUE(cmd[1] == "-e" || cmd[1] == "--exec")
                << "leg '" << label << "': launcher for ("
                << entry.at("hostOs") << ", " << entry.at("hostArch")
                << ") is declared as `" << cmd.front() << ' ' << cmd[1]
                << "`. Only `-e`/`--exec` EXECUTES the fixture; anything else"
                   " (including `--`) hands the whole argv to the distro's"
                   " default shell, which re-expands it. MEASURED: one argument"
                   " `…/g/*.test` arrived at the callee as TWO under `--` and as"
                   " ONE under `-e` — a launcher that cannot be trusted to have"
                   " run the file the driver named.";
        }
    }
    EXPECT_GE(launchers, 2u)
        << "no wsl launcher was found in the catalogue, so this pin is vacuous"
           " (both elf64 legs declare one for a Windows host).";

    // The resolver's own translator vocabulary, read the way a driver reads it.
    auto const r = run({"--path-translations"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    std::size_t translators = 0;
    for (auto const& line : splitLines(r.output)) {
        auto const tab = line.find('\t');
        ASSERT_NE(tab, std::string::npos) << "malformed line: " << line;
        auto const verb = line.substr(0, tab);
        auto const argv = splitWords(line.substr(tab + 1));
        if (argv.empty() || !isWsl(argv.front())) continue;
        ++translators;
        EXPECT_TRUE(argv.size() >= 2 && (argv[1] == "-e" || argv[1] == "--exec"))
            << "pathTranslation '" << verb << "' declares translator argv `"
            << line.substr(tab + 1)
            << "`. Without `-e` the path is parsed by WSL's default shell before"
               " wslpath sees it, and a backslash is that shell's ESCAPE"
               " character — which is exactly how `wslpath: C:ab` came to be"
               " blamed on wslpath. MEASURED through the real call path (python"
               " subprocess.run): 'C:\\a\\b' is rc=1 `wslpath: C:ab` without"
               " `-e` and rc=0 /mnt/c/a/b with it.";
    }
    EXPECT_GE(translators, 1u)
        << "no wsl-based translator was found, so this half of the pin is"
           " vacuous (windows-to-wsl declares one).";
}

// ── 10. NEITHER DRIVER NAMES THE ARTEFACT — THE COMPILER DOES ──────────────
//
// ★ ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING
//
// ✔MEASURED 2026-08-04, WSL x86_64, HEAD a3af1320: the .sh driver CROSS-BUILT the
// Windows testfixture — 189 TUs compiled, the link ran, ZERO `error[` and zero
// `error:`, and `…/pe64-x86_64-windows-exec/testfixture.exe` landed on disk
// (`file(1)`: PE32+ executable (console) x86-64, 8 sections, 5,387,264 bytes). The
// driver reported `build FAILED — 0 error[ but no executable at …/testfixture` and
// marked the leg POISONED: it was looking for a suffix-less name, because nothing
// in the build had ever told it what the artefact was CALLED.
//
// ★ WHY THIS IS WORSE THAN AN ORDINARY BUG, and why it earns a gate test. It is a
// false negative on the project's headline capability
// (ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-CROSS-HOST-ANY-TARGET)
// manufactured by the instrument that measures it — and it hid ITSELF:
// only a POSIX host cross-building for Windows can reach it, which is exactly the
// case this harness exists to observe. On the arm64 VPS the leg never got that far;
// on Windows the .ps1 sibling had its own, different copy of the suffix table.
//
// THE THREE RULES BELOW, and each names a shape that was REMOVED this cycle:
//
//   1. NO SUFFIX TABLE IN A DRIVER. `TargetSpec::outputExtension`
//      (src/program/target_spec.cpp) derives the artefact extension from the
//      CLOSED object-format enum. The .ps1 carried a second copy —
//      `$sfx = if ($fmt -like 'pe*') { '.exe' } else { '' }` — matched on a format
//      NAME PREFIX, wrong for every non-exec format, and its own comment claimed
//      it was "DERIVED FROM THE OBJECT FORMAT, never hardcoded". The .sh carried a
//      third copy by having none at all. Three copies of one table is how they
//      came to disagree.
//   2. NO DRIVER ASSEMBLES THE ARTEFACT PATH. `bin="$outd/$fmt/testfixture"` and
//      `Join-Path (Join-Path $legOut $fmt) "testfixture$sfx"` are the two shapes
//      that did; both put the per-format subdir variable and the artefact's base
//      name on ONE line, which is the shape this rule forbids. (Scoped to `$fmt`
//      on purpose: it is only ever the DSS output routing. The gcc REFERENCE
//      fixture, which make names and this driver legitimately copies, never
//      mentions it.)
//   3. BOTH DRIVERS READ THE BUILD'S OWN REPORT — and so does the compiler emit
//      it. The marker is a WIRE FORMAT across three files in two languages, so it
//      is asserted in all three at once: change it in one and this reds.
//
// RED-ON-DISABLE (measured, see the cycle report for the verbatim messages):
// restore either removed shape and rule 1 or 2 fails naming the line; delete the
// marker from a driver or from program.cpp and rule 3 fails naming the file.
TEST_F(HarnessLegs, NeitherDriverNamesTheArtefactTheCompilerDoes) {
    // The report line the compiler emits per artefact it commits:
    //   dss-code-prime: artifact <targetSpec> <absolute path>
    constexpr char const* kMarker = "dss-code-prime: artifact ";

    // DSS's artifact-extension table, as a driver would be tempted to spell one
    // entry of it: a bare, quoted extension. `wsl.exe` / `dss-code-prime.exe` are
    // whole file names and never match; a `$sfx`-style row always does.
    constexpr std::string_view kSuffixes[] = {".exe", ".dll", ".so", ".dylib",
                                              ".lib"};

    struct Driver {
        char const* name;
        bool        powershell;
    };
    constexpr Driver kDrivers[] = {{"build-and-test.sh", false},
                                   {"build-and-test.ps1", true}};

    std::size_t markersSeen = 0;
    for (auto const& d : kDrivers) {
        auto const path  = harnessDir() / d.name;
        auto const lines = liveLines(path, d.powershell);
        ASSERT_FALSE(lines.empty()) << d.name << " has no live lines";

        std::size_t markerHere = 0;
        for (auto const& line : lines) {
            // RULE 1 — a bare quoted artifact extension is a copy of DSS's table.
            for (auto const& sfx : kSuffixes) {
                for (char const q : {'\'', '"'}) {
                    std::string const needle =
                        std::string{q} + std::string{sfx} + std::string{q};
                    EXPECT_EQ(line.find(needle), std::string::npos)
                        << d.name << " spells an artifact extension as a"
                           " literal:\n  " << line
                        << "\nThe suffix belongs to the object format"
                           " (TargetSpec::outputExtension, keyed on the closed"
                           " format enum). A copy here is a second table to keep"
                           " in step — and the last time there were two, they"
                           " disagreed and threw away a real cross-host build.";
                }
            }

            // RULE 2 — the per-format output subdir and the artefact's base name
            // on one line is a path being ASSEMBLED.
            bool const assembles = line.find("$fmt") != std::string::npos
                                && line.find("testfixture") != std::string::npos;
            EXPECT_FALSE(assembles)
                << d.name << " assembles the artefact path itself:\n  " << line
                << "\nThe file name is the compiler's to decide and to REPORT"
                   " (`" << kMarker << "<spec> <path>`); a driver that rebuilds"
                   " it needs the extension table it must not have.";

            if (line.find(kMarker) != std::string::npos) ++markerHere;
        }

        // RULE 3 (driver half) — it actually reads the report.
        EXPECT_GE(markerHere, 1u)
            << d.name << " never mentions `" << kMarker
            << "`, so it is not reading the build's own statement of what it"
               " wrote. Something else is deciding the artefact's name, which is"
               " the defect this section exists to prevent.";
        markersSeen += markerHere;
    }

    // RULE 3 (producer half) — and the compiler actually emits it. Without this
    // the two drivers could agree perfectly on a string nothing ever prints.
    //
    // ★ OVER LIVE LINES, and this was learned the hard way INSIDE this cycle,
    // exactly as `liveLines` itself was: the first version searched program.cpp's
    // raw text, and renaming the emitted marker left the test GREEN — because the
    // emitter DOCUMENTS the wire format it prints, and the docblock still spelled
    // it. `liveLines`' comment rules are shell/PowerShell (`#`, `<# #>`), so C++
    // needs its own one-line strip here.
    auto const emitter = repoRoot() / "src" / "program" / "program.cpp";
    ASSERT_TRUE(fs::exists(emitter)) << emitter;
    std::string emitterCode;
    for (auto const& line : splitLines(fileText(emitter))) {
        auto const first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 2, "//") == 0) continue;
        emitterCode += line;
        emitterCode += '\n';
    }
    EXPECT_NE(emitterCode.find(kMarker), std::string::npos)
        << emitter.generic_string() << " does not emit `" << kMarker
        << "`. Both drivers parse that exact prefix, so this is a wire format:"
           " renaming it here without renaming it there leaves every leg"
           " reporting 'the build produced no artefact' on a build that"
           " succeeded — the original defect, restored.";

    // NON-VACUITY. Rules 1 and 2 are NEGATIVE and go green by seeing nothing, so
    // the positive half carries the floor: two drivers, at least one reader each.
    EXPECT_GE(markersSeen, 2u)
        << "only " << markersSeen
        << " artefact-report reader(s) were found across the two drivers. Fix the"
           " recogniser, do not delete the test.";
}
