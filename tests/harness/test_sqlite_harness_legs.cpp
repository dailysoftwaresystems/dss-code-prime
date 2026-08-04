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
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
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

// The repo root. `dss_add_test` sets DSS_CONFIG_ROOT to CMAKE_SOURCE_DIR for
// every registered test, which is the same anchor the shipped-config loader
// uses — so this resolves identically for in-tree and out-of-tree builds.
[[nodiscard]] fs::path repoRoot() {
    char const* const root = std::getenv("DSS_CONFIG_ROOT");
    if (root != nullptr && *root != '\0') return fs::path{root};
    // Fall back to walking up for src/dss-config, mirroring findShippedConfig.
    fs::path cur = fs::current_path();
    for (int i = 0; i < 12; ++i) {
        if (fs::exists(cur / "src" / "dss-config")) return cur;
        if (!cur.has_parent_path() || cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    return {};
}

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
