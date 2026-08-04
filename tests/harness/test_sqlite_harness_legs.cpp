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
