// `runBuildScripts` — the `.dss-project.json` build-hook runner
// (`src/program/build_scripts.{hpp,cpp}`).
//
// EVERY WAY THIS FEATURE CAN BREAK IS SILENT, which is why the pins below are
// literal to the point of tedium:
//
//   * A SHELL sneaking in under the argv vector. `system()`, `cmd /c` or
//     `sh -c` would all make the happy path look identical — the script still
//     runs, the build still goes green — while quietly expanding `$HOME`,
//     re-splitting `&& echo pwned` into two commands, globbing `*` against the
//     working directory, and turning a path containing `;` into executable
//     text. Nothing observable changes until the day a manifest argument
//     contains a metacharacter, and then the failure is an injection, not a
//     diagnostic. `NoShellArgumentsArriveByteIdentical` is the load-bearing
//     test of this whole file for exactly that reason.
//   * CONTINUING PAST A FAILURE. Build hooks are ordered work; step 2 consumes
//     what step 1 produced. A runner that reports the failure and keeps going
//     still emits the right diagnostic, so a test that only counts diagnostics
//     passes — while the build has actually run steps against inputs that were
//     never generated. Only the ABSENCE of the third entry's marker file
//     distinguishes "stopped" from "merely complained".
//   * `runOn` BEING IGNORED. A test that asserts "the entry naming this host
//     ran" passes just as well when the filter is deleted entirely. Only the
//     opposite polarity — an entry naming a DIFFERENT host that must NOT run
//     and must NOT report — can tell the two apart.
//   * THE TWO FAILURE CODES COLLAPSING INTO ONE. "The program isn't there" and
//     "the program ran and said no" have completely different remediations, and
//     the first is the single most common CI failure there is (a missing
//     interpreter). Each pin below is THREE-SIDED — the code that must fire
//     once, the code that must fire zero times, and the total error count — so
//     a runner that emits both, or the wrong one, cannot pass.
//   * A STATUS THE SCRIPT NEVER CHOSE, REPORTED AS ITS VERDICT. On the spawned
//     arm the exit number is the child's own decision ONLY when
//     `SpawnResult::diagnostic` is empty; a POSIX child killed by a signal
//     reports `128 + signo` with the signal named in that field. A runner that
//     drops the field tells an operator whose codegen hook just SEGV'd that the
//     hook deliberately returned 139 — a loud, confident, wrong diagnostic that
//     sends them looking for a `return 139` that does not exist. Both arms are
//     pinned on FULL message text, because they differ only in a clause in the
//     middle and every weaker assertion passes on either.
//   * THE DIAGNOSTIC BEING SILENCEABLE. `runBuildScripts` returns false whether
//     or not its report survived, and the driver turns that into `return 1`. So
//     a suppressible hook code produces a build that fails with an EMPTY
//     stderr: a failure with no statement of what failed. Both codes are
//     members of `kUnsuppressableCodes` and `SuppressCannotSilenceAFailingHook`
//     asserts the DIAGNOSTIC, never merely the exit code.
//
// ── WHAT IS DELIBERATELY *NOT* IN THE `NoShellArgumentsArriveByteIdentical`
//    PAYLOAD: A NUL ───────────────────────────────────────────────────────────
//
// Every other hostile byte round-trips; a NUL cannot, and pretending otherwise
// would be the one dishonest element in that vector. An argv word reaches the
// OS as a NUL-TERMINATED C string, so the byte is not something the spawner
// could preserve if it tried. It is rejected one tier up instead — at the
// MANIFEST, by `parseProjectConfig`, pinned by
// `ProjectConfigLoader.ScriptEntryRunWordWithEmbeddedNulFailsLoud` in
// `test_project_config.cpp` — which is the tier that can still name the entry
// and the word index while the author is looking at the file. (A separate
// defence-in-depth guard lives in the spawn layer; a `ScriptEntry` reaching
// `runBuildScripts` from CODE never passed the loader.)
//
// ── THE FIXTURE MECHANISM: SELF-SPAWN ───────────────────────────────────────
//
// These tests need a program that RECORDS what it was handed. The repo has no
// CMake-built helper-executable convention (the only `add_executable` calls
// under `tests/` are gtest targets and the example-corpus runner), and it does
// have an established answer to exactly this need:
// `tests/test_support/test_run_binary_capture.cpp` re-execs the TEST BINARY
// ITSELF with a private flag its `main` intercepts before gtest parses argv.
// That precedent is copied here verbatim in shape, because it is the right
// answer three times over:
//
//   1. NO EXTERNAL DEPENDENCY. Windows has no `/bin/echo`; a Mac has no
//      `/proc`. A fixture that is this file is available on every host in the
//      matrix, in every build configuration, with no PATH assumptions — and a
//      test of "argv arrives byte-identically" must not be at the mercy of a
//      third-party program's own argv handling.
//   2. NO NEW BUILD TARGET, hence no new failure surface. A separate
//      `add_executable` fixture would need a `$<TARGET_FILE:…>` compile
//      definition, an `add_dependencies` edge (nothing links it, so a fresh
//      `ctest` could otherwise run against a fixture that was never built), and
//      a row in BOTH `tools/check-orphan-tests.sh` and its `.ps1` twin — whose
//      allowlist is machine-checked and must stay byte-identical between them.
//      That is three cross-cutting files to keep in sync for a program whose
//      entire job is to write down its argv.
//   3. THE PATH IS EXACT. `argv[0]`, made absolute, is the real executable for
//      this build tree — no search heuristic and no chance of picking up a
//      stale binary from another one.
//
// The fixture protocol is POSITIONAL, not scanned: `argv[1]` must be the
// record-file flag and `argv[2]` the exit-code flag. A scan would mean a
// payload argument that happened to start with the flag prefix could be
// swallowed, and payload arguments in this file are deliberately hostile text.
// Everything from `argv[3]` on is opaque payload, recorded verbatim.
//
// The record is LENGTH-PREFIXED (`<len>:<bytes>\n`), not line-oriented, so a
// payload containing a newline, a `:` or a NUL-free binary blob still round
// trips unambiguously — a line-based record would silently turn "one argument
// containing a newline" into "two arguments", which is precisely the class of
// corruption these tests exist to detect.
//
// ── RED-ON-DISABLE (the levers every pin above hangs on) ────────────────────
//
//   * Re-join `entry.run` into a command string and hand it to `system()` /
//     `cmd /c` / `sh -c` → `NoShellArgumentsArriveByteIdentical` reds: `$HOME`
//     empties or `%PATH%` expands, `&& echo pwned` vanishes from the received
//     argv (and becomes a second command), `*` globs into the working
//     directory's file names, and the received-vs-expected vectors differ in
//     both size and content.
//   * Delete the `return false` after either diagnostic (report-and-continue)
//     → `StopsAtTheFirstFailingScript` reds on the THIRD entry's marker file
//     existing. The diagnostic counts in that same test stay green, which is
//     the point of asserting the marker.
//   * Delete the `appliesToHost` call (run everything) → `RunOnSkipsOtherHosts`
//     reds on the other-host marker existing.
//   * Invert the empty-`runOn` default (`if (runOn.empty()) return false`) →
//     `EmptyRunOnRunsOnEveryHost` reds on a missing marker, and
//     `RunOnSkipsOtherHosts` stays green — which is why both polarities are
//     pinned separately.
//   * Collapse the two codes into one, or read `exitCode` when `spawned` is
//     false → `SpawnFailureIsItsOwnCode` and `NonZeroExitIsItsOwnCode` red on
//     the zero-count side of their three-sided assertions.
//   * Substitute the process working directory for the `cwd` parameter →
//     `SpawnsInTheRequestedWorkingDirectory` reds, because the directory it
//     passes is deliberately NOT the process cwd.
//   * Drop the substrate's `diagnostic` text from the spawn-failure message →
//     `SpawnFailureIsItsOwnCode` reds: it recomputes that text by calling the
//     substrate directly with the same argv and requires the reported message
//     to contain it verbatim.
//   * Drop `SpawnResult::diagnostic` on the NON-ZERO-EXIT arm (report the
//     "script's own verdict" prose unconditionally, as the runner did before
//     the fork existed) → `SignalKilledChildIsNotReportedAsTheScriptsOwnVerdict`
//     reds on POSIX. `NonZeroExitIsItsOwnCode` stays green, which is why both
//     arms are pinned separately and both on full message text.
//   * Remove either hook code from `kUnsuppressableCodes` →
//     `SuppressCannotSilenceAFailingHook` reds on a zero occurrence count while
//     `runBuildScripts` still returns false — the exact shape of the defect.

#include "program/build_scripts.hpp"

#include "core/substrate/process_spawn.hpp"  // the spawn layer, called directly to recompute its own diagnostic text
#include "core/types/diagnostic_reporter.hpp"    // DiagnosticPolicy::suppress + the report() shim
#include "core/types/parse_diagnostic.hpp"
#include "core/types/unsuppressable_codes.hpp"   // isUnsuppressable — the hook codes' membership
#include "program/platform_token.hpp"
#include "core/types/project_config.hpp"

#include "diagnostic_count.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::DiagnosticSeverity;
using dss::ScriptEntry;
using dss::runBuildScripts;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace fs = std::filesystem;

// ── MEASURED, and it would have made the `*` pin lie ────────────────────────
//
// MinGW-w64's C runtime EXPANDS WILDCARDS IN ITS OWN COMMAND LINE before `main`
// is entered — it is the only mainstream CRT that does so by default (MSVC
// globs only if you deliberately link `setargv.obj`; POSIX never globs, that
// being the shell's job). So on the MinGW leg the fixture's `argv` is rewritten
// by the CHILD's startup code, in the child's process, long after the spawner
// has done everything correctly.
//
// MEASURED 2026-08-12 with a standalone probe (g++ 13.2, x86_64-w64-mingw32),
// spawning via a bare `CreateProcessW` with NO shell anywhere: the argument `*`
// arrived at the child's `main` split into `child-cwd` and
// `globbable-file.txt`, i.e. the names of the two entries in the spawn working
// directory, and every later element shifted by one.
//
// That is a FALSE RED with the worst possible reading: `NoShellArgumentsArrive-
// ByteIdentical` would fail on the MinGW CI leg while passing on the MSVC gate,
// and the failure text — an argument that turned into filenames — says "a shell
// globbed your argument", which is exactly the accusation the test exists to
// make and exactly the wrong diagnosis.
//
// Turning globbing off is the honest fix, not a workaround: the subject of that
// test is what `runBuildScripts` HANDS THE OPERATING SYSTEM, and a receiving
// program's own runtime conventions are downstream of that boundary and none of
// the driver's business. With this set, `*` measures the spawner alone.
//
// Guarded to MinGW because `_CRT_glob` exists in no other C runtime; an
// unguarded definition risks a duplicate-symbol link error the day some other
// CRT adopts the name.
#if defined(__MINGW32__) || defined(__MINGW64__)
extern "C" int _CRT_glob = 0;
#endif

namespace {

// ── the fixture protocol ────────────────────────────────────────────────────

constexpr std::string_view kFixtureFlag = "--dss-build-script-record=";
constexpr std::string_view kExitFlag    = "--dss-build-script-exit=";

// Fixture-side protocol violations return codes in the 90s so that a broken
// FIXTURE can never be mistaken for the SUBJECT misbehaving: every one of them
// surfaces as a `D_ScriptExitedNonZero` naming the status, and no test expects
// a status in that range.
constexpr int kFixtureBadRecordPath = 90;
constexpr int kFixtureMissingExitArg = 91;
constexpr int kFixtureMalformedExitArg = 92;
constexpr int kFixtureCannotOpenRecord = 93;
constexpr int kFixtureNoCurrentPath = 94;
constexpr int kFixtureWriteFailed = 95;
constexpr int kFixtureSignalUnsupported = 96;
constexpr int kFixtureRaiseReturned = 97;

// One record item: its byte length, a `:`, the raw bytes, a `\n`. The trailing
// newline is a readability affordance only — the parser is driven entirely by
// the length, so it never has to guess where an item ends.
void writeRecordItem(std::ostream& out, std::string const& item) {
    out << item.size() << ':' << item << '\n';
}

// FIXTURE MODE. Records the working directory it was spawned in, then EVERY
// element of its own argv (index 0 included — argv[0] is part of what must
// arrive intact), then exits with the requested status.
//
// A NEGATIVE requested status means DIE BY SIGNAL `-status` rather than return
// it. That is the only way this suite can produce a child whose exit NUMBER was
// not chosen by the child, which is the whole subject of the runner's
// diagnostic-emptiness fork.
int runFixtureMode(int argc, char** argv) {
    std::string const recordPath{
        std::string_view{argv[1]}.substr(kFixtureFlag.size())};
    if (recordPath.empty()) return kFixtureBadRecordPath;
    if (argc < 3) return kFixtureMissingExitArg;

    std::string_view const exitArg{argv[2]};
    if (exitArg.substr(0, kExitFlag.size()) != kExitFlag) {
        return kFixtureMalformedExitArg;
    }
    int const requestedExit = std::atoi(
        std::string{exitArg.substr(kExitFlag.size())}.c_str());

    std::error_code ec;
    fs::path const cwd = fs::current_path(ec);
    if (ec) return kFixtureNoCurrentPath;

    // BINARY mode: Windows' default text mode would rewrite every '\n' in a
    // payload argument to CRLF, and the length prefix — computed from the
    // in-memory bytes — would then no longer describe what is on disk.
    std::ofstream out(recordPath, std::ios::binary | std::ios::trunc);
    if (!out) return kFixtureCannotOpenRecord;

    writeRecordItem(out, cwd.string());
    for (int i = 0; i < argc; ++i) writeRecordItem(out, std::string{argv[i]});
    out.flush();
    if (!out) return kFixtureWriteFailed;

    if (requestedExit < 0) {
        // The record is already FLUSHED to the OS above, and the kernel closes
        // the descriptor as the process dies, so it survives the kill —
        // nothing below this point may depend on a destructor running.
#if defined(_WIN32)
        // Windows has no signal with this shape and the only caller is
        // POSIX-gated. Report a FIXTURE code rather than exiting some
        // plausible number: a fixture that quietly did something else is worse
        // than one that says it cannot do this here.
        return kFixtureSignalUnsupported;
#else
        std::raise(-requestedExit);
        // Unreachable for a fatal signal. If it was somehow handled or
        // blocked, say so with a fixture code — returning 0 would let the pin
        // read a clean exit as the signal it asked for.
        return kFixtureRaiseReturned;
#endif
    }
    return requestedExit;
}

// ── test-side helpers ───────────────────────────────────────────────────────

// A parsed record: the working directory the fixture ran in, and the argv it
// actually received.
struct FixtureRecord {
    std::string              cwd;
    std::vector<std::string> argv;
};

// Parse the length-prefixed record. Returns nullopt on ANY malformation rather
// than a partial result — a half-parsed record would let a corrupted argv
// masquerade as a short one, and "the child received fewer arguments" is
// exactly the symptom a shell would produce.
std::optional<FixtureRecord> readRecord(fs::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string const blob = buf.str();

    std::vector<std::string> items;
    std::size_t pos = 0;
    while (pos < blob.size()) {
        std::size_t const colon = blob.find(':', pos);
        if (colon == std::string::npos) return std::nullopt;
        std::size_t len = 0;
        for (std::size_t i = pos; i < colon; ++i) {
            char const c = blob[i];
            if (c < '0' || c > '9') return std::nullopt;
            len = len * 10u + static_cast<std::size_t>(c - '0');
        }
        std::size_t const start = colon + 1;
        if (start + len + 1 > blob.size()) return std::nullopt;
        if (blob[start + len] != '\n') return std::nullopt;
        items.emplace_back(blob, start, len);
        pos = start + len + 1;
    }
    if (items.empty()) return std::nullopt;  // cwd is always item 0

    FixtureRecord rec;
    rec.cwd  = items.front();
    rec.argv.assign(items.begin() + 1, items.end());
    return rec;
}

// This test executable, as an ABSOLUTE path. Absolute matters: entries below
// are spawned with a working directory that is deliberately NOT the process
// cwd, and a relative `argv[0]` would then resolve against the wrong directory
// (or not at all) — a spawn failure that has nothing to do with the subject.
fs::path selfExecutable() {
    auto const& argvs = ::testing::internal::GetArgvs();
    if (argvs.empty()) return {};
    std::error_code ec;
    fs::path const abs = fs::absolute(fs::path{argvs[0]}, ec);
    if (ec) return fs::path{argvs[0]};
    return abs;
}

std::string fixtureFlagFor(fs::path const& recordPath) {
    return std::string{kFixtureFlag} + recordPath.string();
}

std::string exitFlagFor(int code) {
    return std::string{kExitFlag} + std::to_string(code);
}

// An entry that spawns this binary in fixture mode: writes `recordPath`, exits
// `exitCode`, and carries `payload` as its trailing arguments.
ScriptEntry fixtureEntry(fs::path const& recordPath,
                         int             exitCode,
                         std::vector<std::string> payload = {},
                         std::vector<std::string> runOn   = {}) {
    ScriptEntry e;
    e.run.push_back(selfExecutable().string());
    e.run.push_back(fixtureFlagFor(recordPath));
    e.run.push_back(exitFlagFor(exitCode));
    for (auto& p : payload) e.run.push_back(std::move(p));
    e.runOn = std::move(runOn);
    return e;
}

// A `runOn` token naming a host that is NOT this one. DERIVED from the
// vocabulary and the host probe rather than hard-coded, so the same source
// exercises the "does not apply" polarity on all three legs of the matrix.
std::string aDifferentHost() {
    for (std::string_view const token : dss::kRunOnPlatformTokens) {
        if (token != dss::currentHostOs()) return std::string{token};
    }
    return {};  // impossible while the vocabulary has >1 token; asserted below
}

// The FIRST diagnostic carrying `code`, or nullptr. Used only where a pin has
// already established that exactly one such diagnostic exists.
dss::ParseDiagnostic const* firstWith(DiagnosticReporter const& rep,
                                      DiagnosticCode            code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return &d;
    }
    return nullptr;
}

// Compare two directories as the OS sees them. `temp_directory_path()` is a
// SYMLINK on macOS (`/var/folders/…` → `/private/var/folders/…`), so the child
// reporting its own `current_path()` legitimately spells the same directory
// differently from the string we handed the spawner. Canonicalising both sides
// compares the directory, not the spelling — and it still reds for a genuinely
// different directory, which is the property under test.
fs::path canonicalDir(fs::path const& p) {
    std::error_code ec;
    fs::path const c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

}  // namespace

// ── 0. The harness itself, so a harness failure names itself ────────────────

TEST(BuildScripts, TheFixtureProgramIsThisExecutable) {
    // Every pin below spawns THIS binary. If `argv[0]` cannot be recovered or
    // does not resolve to a file, all of them fail as `D_ScriptSpawnFailed` —
    // which reads as "the subject cannot spawn things" when the truth is "the
    // test could not find its own fixture". This entry makes that distinction
    // visible in the ctest output instead of leaving it to be inferred.
    auto const self = selfExecutable();
    ASSERT_FALSE(self.empty()) << "could not recover argv[0] from gtest";
    ASSERT_TRUE(self.is_absolute())
        << "argv[0] was not made absolute: " << self.string();
    ASSERT_TRUE(fs::exists(self))
        << "argv[0] does not resolve to a file: " << self.string();
}

// ── 1. THE LOAD-BEARING PIN: no shell, ever ─────────────────────────────────

TEST(BuildScripts, NoShellArgumentsArriveByteIdentical) {
    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const record = scratch.path() / "argv.rec";

    // ★ THE `*` ELEMENT IS VACUOUS UNLESS THE SPAWN DIRECTORY HAS SOMETHING TO
    // MATCH. An unmatched glob is left LITERAL by POSIX `sh` and by every
    // wildcard-expanding runtime, so in an empty directory `*` arrives intact
    // even from a shell — the pin would pass no matter what. These decoys are
    // created BEFORE the spawn (the record file is written by the child, i.e.
    // far too late to count) so that any expansion has a visible, deterministic
    // result: `*` would arrive as these two names instead of one `*`.
    for (char const* decoy : {"globbable-decoy-a.txt", "globbable-decoy-b.txt"}) {
        std::ofstream f(scratch.path() / decoy, std::ios::binary);
        f << "decoy";
        ASSERT_TRUE(f.good()) << "could not create the glob decoy " << decoy;
    }

    // Every element here is text a shell would MANGLE, and each mangling is a
    // different mechanism, so no single "it happens to work" explanation covers
    // all of them:
    //   `$HOME`          — POSIX variable expansion (becomes the home dir, or
    //                      empty under `sh -c` in a stripped environment).
    //   `%PATH%`         — cmd.exe variable expansion.
    //   `&& echo pwned`  — command SEPARATION. Under a shell this element stops
    //                      being an argument at all and becomes a second
    //                      command; the received argv simply loses it. It also
    //                      contains spaces, so a naive re-join + re-split turns
    //                      one argument into three.
    //   `;`              — the other command separator, and the character most
    //                      likely to appear innocently inside a real path.
    //   `*`              — glob expansion against the working directory.
    //   the combined line — all of the above at once, plus interior spaces, so
    //                      a partial escaper that handles one case cannot pass.
    //   a bare space and a lone quote-free `"a b"`-shaped argument would need
    //     the substrate's own quoting to survive; the two space-bearing entries
    //     below cover that axis.
    std::vector<std::string> const payload{
        "$HOME",
        "%PATH%",
        "&& echo pwned",
        ";",
        "*",
        "a;b && echo pwned $HOME %PATH% * | tee /dev/null",
        "trailing space and tab\targ",
    };

    ScriptEntry const entry = fixtureEntry(record, /*exitCode=*/0, payload);
    // The EXACT vector the child must report back. Built from `entry.run`
    // itself, not re-typed: the contract is "what the runner was handed is what
    // the child receives", and re-typing the expectation would let a typo in
    // the test paper over a real mismatch.
    std::vector<std::string> const expected = entry.run;

    DiagnosticReporter rep;
    ASSERT_TRUE(runBuildScripts({entry}, scratch.path(), rep))
        << (rep.all().empty() ? std::string{"(no diagnostic)"}
                              : rep.all().front().actual);
    EXPECT_EQ(rep.errorCount(), 0u);

    ASSERT_TRUE(fs::exists(record))
        << "the fixture never wrote its record — it did not run";
    auto const rec = readRecord(record);
    ASSERT_TRUE(rec.has_value()) << "record file is malformed: " << record;

    // EXACT EQUALITY of the whole vector. Not `find`, not a substring, not a
    // count: a shell that expanded `$HOME` would still leave every other
    // element intact, and a substring check on the survivors would pass.
    ASSERT_EQ(rec->argv.size(), expected.size())
        << "the child received a DIFFERENT NUMBER of arguments than it was "
           "given — an argument was split, dropped, or glob-expanded, which is "
           "what a shell does and a direct spawn cannot";
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(rec->argv[i], expected[i])
            << "argv[" << i << "] did not survive the spawn byte-identically";
    }
    EXPECT_EQ(rec->argv, expected);

    // The decoys must still have been there when the child started, or the `*`
    // element above proved nothing. Re-asserted AFTER the run rather than only
    // before it, so a future edit that moves the decoy creation (or points the
    // spawn at a different directory) cannot quietly hollow out the pin.
    EXPECT_TRUE(fs::exists(scratch.path() / "globbable-decoy-a.txt"));
    EXPECT_TRUE(fs::exists(scratch.path() / "globbable-decoy-b.txt"));
    for (auto const& element : rec->argv) {
        EXPECT_EQ(element.find("globbable-decoy"), std::string::npos)
            << "a decoy filename appeared IN THE CHILD'S ARGV — the `*` "
               "argument was glob-expanded against the spawn working "
               "directory: "
            << element;
    }

    // And nothing named `pwned` was created anywhere in the scratch tree: the
    // `&& echo pwned` element must never have been INTERPRETED.
    for (auto const& de : fs::recursive_directory_iterator(scratch.path())) {
        EXPECT_EQ(de.path().filename().string().find("pwned"),
                  std::string::npos)
            << "a shell interpreted `&& echo pwned`: " << de.path();
    }
}

// ── 2. Ordering, and STOPPING (not merely reporting) ────────────────────────

TEST(BuildScripts, StopsAtTheFirstFailingScript) {
    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const markerA = scratch.path() / "first.rec";
    fs::path const markerB = scratch.path() / "second.rec";
    fs::path const markerC = scratch.path() / "third.rec";

    constexpr int kFailStatus = 7;  // distinctive: not 1, not a fixture 9x code

    std::vector<ScriptEntry> const scripts{
        fixtureEntry(markerA, /*exitCode=*/0),
        fixtureEntry(markerB, /*exitCode=*/kFailStatus),
        fixtureEntry(markerC, /*exitCode=*/0),
    };

    DiagnosticReporter rep;
    EXPECT_FALSE(runBuildScripts(scripts, scratch.path(), rep));

    // THREE-SIDED on the diagnostics: the code that must fire, the sibling code
    // that must NOT, and the total — so a runner that emits both, or emits the
    // failure twice, cannot pass.
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);

    // ORDER: the first entry ran (so the runner did not start in the middle or
    // reorder), and the second ran (so the failure is a real one, not a
    // skipped entry mis-reported).
    EXPECT_TRUE(fs::exists(markerA))
        << "the FIRST entry never ran — the runner is not walking the list in "
           "manifest order";
    EXPECT_TRUE(fs::exists(markerB))
        << "the SECOND entry never ran, so the reported failure did not come "
           "from where the test thinks it did";

    // ★ THE ASSERTION THAT SEPARATES "STOPPED" FROM "COMPLAINED". Every
    // diagnostic count above stays green for a runner that reports the failure
    // and then keeps going. Only this one reds.
    EXPECT_FALSE(fs::exists(markerC))
        << "the THIRD entry RAN after the second one failed. The runner "
           "reported the failure but did not stop — a build hook that failed "
           "must not be followed by more hooks running against inputs that "
           "were never produced.";
}

TEST(BuildScripts, NonZeroExitIsItsOwnCode) {
    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const record = scratch.path() / "failing.rec";
    constexpr int kFailStatus = 7;

    ScriptEntry const entry = fixtureEntry(record, kFailStatus);
    DiagnosticReporter rep;
    EXPECT_FALSE(runBuildScripts({entry}, scratch.path(), rep));

    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    ASSERT_TRUE(fs::exists(record)) << "the fixture did not actually run";

    auto const* d = firstWith(rep, DiagnosticCode::D_ScriptExitedNonZero);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->severity, DiagnosticSeverity::Error);
    // The message must name the program AND the status — the two facts an
    // operator needs before deciding whether to read the script or the log.
    EXPECT_NE(d->actual.find(entry.run.front()), std::string::npos)
        << "the message does not name argv[0]: " << d->actual;
    EXPECT_NE(d->actual.find("status " + std::to_string(kFailStatus)),
              std::string::npos)
        << "the message does not carry the exit status: " << d->actual;

    // ★ THE CLEAN-VERDICT ARM OF THE FORK, PINNED WHOLE. The `exitCode != 0`
    // path chooses its prose on `SpawnResult::diagnostic.empty()`, and this is
    // the EMPTY side: a child that called `exit(7)` really did choose 7, so
    // "read what it printed" is the correct — and the only — remediation.
    // FULL equality rather than substrings, because the two arms differ only
    // in a clause in the MIDDLE: a runner that took the wrong branch would
    // still name the program and still carry the status, and every assertion
    // above it would stay green.
    EXPECT_EQ(d->actual,
              "build script '" + entry.run.front()
                  + "' ran and exited with status " + std::to_string(kFailStatus)
                  + ". The program was found and executed, so this is the "
                    "script's own verdict, not an environment problem — fix "
                    "what it reported on this build's inherited stdout/stderr "
                    "and re-run. Non-zero stops the build: a hook that failed "
                    "must not be followed by a compile of stale inputs.");
    // The OTHER arm's discriminator must be ABSENT. Implied by the equality,
    // stated because it is the contract: this status WAS the script's choice,
    // and nothing may hedge about that.
    EXPECT_EQ(d->actual.find("NOT a status the script chose"),
              std::string::npos)
        << "a clean exit(7) was reported as a status the script did not "
           "choose: "
        << d->actual;
}

// ── 2b. THE STATUS THE SCRIPT DID NOT CHOOSE ────────────────────────────────
//
// `SpawnResult::diagnostic` is EMPTY for a clean `exit(n)` and NON-EMPTY for
// exactly the states where the number is a stand-in rather than a decision —
// on POSIX, a child killed by a SIGNAL, reported as `128 + signo` with the
// signal named in that field. The runner forks on that emptiness, and the arm
// it picks is the entire message: an operator told that a codegen hook which
// just SEGV'd "deliberately returned 139" goes hunting for a `return 139` that
// does not exist, and the environment problem that actually happened is never
// looked at. Loud but wrong is still wrong.
//
// SIGKILL, not SIGSEGV: it cannot be caught, blocked or handled by anything
// gtest or the C runtime installs, it writes no core file and pops no crash
// reporter, so the child's death is deterministic and leaves nothing behind.
//
// RED-ON-DISABLE: drop `result.diagnostic` from the non-zero-exit arm (emit the
// clean-verdict prose unconditionally, as the runner did before the fork
// existed) and this reds on the full-message equality — while every count in
// this file, and `NonZeroExitIsItsOwnCode` itself, stays green.
#if !defined(_WIN32)
TEST(BuildScripts, SignalKilledChildIsNotReportedAsTheScriptsOwnVerdict) {
    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const record = scratch.path() / "signalled.rec";
    constexpr int kSigKill = 9;
    constexpr int kSignalStatus = 128 + kSigKill;

    ScriptEntry const entry = fixtureEntry(record, /*exitCode=*/-kSigKill);

    DiagnosticReporter rep;
    EXPECT_FALSE(runBuildScripts({entry}, scratch.path(), rep));

    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    ASSERT_TRUE(fs::exists(record))
        << "the fixture died before writing its record, so it never reached "
           "the raise — this pin would be measuring the wrong failure";

    // THE SUBSTRATE'S OWN STATUS PROSE, RECOMPUTED rather than re-typed — the
    // same reason as `SpawnFailureIsItsOwnCode`: hard-coding one platform's
    // signal wording would pin this test to that platform, and dropping the
    // interpolation would then still pass on the others.
    dss::substrate::SpawnResult const direct =
        dss::substrate::spawnAndWaitInherit(entry.run, scratch.path());
    ASSERT_TRUE(direct.spawned);
    ASSERT_EQ(direct.exitCode, kSignalStatus)
        << "the fixture did not die by signal " << kSigKill;
    ASSERT_FALSE(direct.diagnostic.empty())
        << "the spawn layer reported a SIGNALLED child with an EMPTY "
           "diagnostic — the runner's fork would then be undecidable and this "
           "pin could not tell the two arms apart";

    auto const* d = firstWith(rep, DiagnosticCode::D_ScriptExitedNonZero);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->severity, DiagnosticSeverity::Error);
    EXPECT_EQ(d->actual,
              "build script '" + entry.run.front()
                  + "' ran and exited with status "
                  + std::to_string(kSignalStatus)
                  + ", which is NOT a status the script chose: "
                  + direct.diagnostic
                  + ". The program was found and executed, so this is not a "
                    "missing-interpreter problem either — the run itself did "
                    "not complete normally. Non-zero stops the build: a hook "
                    "that failed must not be followed by a compile of stale "
                    "inputs.");
    // The CLEAN arm's claim must be ABSENT. Stated separately from the
    // equality because it is the contract: this number was not a verdict, and
    // sending the operator to read the script's output is a wild goose chase.
    EXPECT_EQ(d->actual.find("the script's own verdict"), std::string::npos)
        << "a signal-killed child was reported as having chosen its status: "
        << d->actual;
}
#else
TEST(BuildScripts, SignalKilledChildIsNotReportedAsTheScriptsOwnVerdict) {
    GTEST_SKIP()
        << "POSIX-only, and honestly so rather than faked. On Windows the "
           "non-empty-`diagnostic` arm is reachable only when "
           "WaitForSingleObject or GetExitCodeProcess FAILS on a handle the "
           "spawn layer has just created, which a test cannot provoke. A "
           "Windows child that dies on an access violation still reports "
           "through GetExitCodeProcess with an EMPTY diagnostic, so it takes "
           "the clean-verdict arm that NonZeroExitIsItsOwnCode already pins. "
           "This arm is covered on the Linux and macOS legs.";
}
#endif

// ── 2c. `--suppress` CANNOT SILENCE A FAILING HOOK ──────────────────────────
//
// `runBuildScripts` returns false whether or not its report survived, and the
// driver turns that into `return 1`. So a SUPPRESSIBLE hook code hands the
// operator a build that fails with an EMPTY stderr — a failure with no
// statement of what failed, which is strictly worse than the silent success it
// replaced. Both codes are therefore members of `kUnsuppressableCodes`
// (`src/core/types/unsuppressable_codes.cpp`), which bypasses the suppress set
// AND the reporter's cap / dedup / per-code gates.
//
// ★ This pin asserts the DIAGNOSTIC, never merely the non-zero return: "the
// build still fails" was true throughout the defect, and is exactly what made
// it invisible.
//
// RED-ON-DISABLE: remove either code from `kUnsuppressableCodes` and this reds
// on a zero occurrence count while `runBuildScripts` still returns false.
TEST(BuildScripts, SuppressCannotSilenceAFailingHook) {
    ASSERT_TRUE(dss::isUnsuppressable(DiagnosticCode::D_ScriptExitedNonZero));
    ASSERT_TRUE(dss::isUnsuppressable(DiagnosticCode::D_ScriptSpawnFailed));

    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const record = scratch.path() / "suppressed.rec";
    constexpr int kFailStatus = 7;

    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::D_ScriptExitedNonZero);
    cfg.policy.suppress.insert(DiagnosticCode::D_ScriptSpawnFailed);
    cfg.policy.suppress.insert(DiagnosticCode::S_DeprecatedSymbolUsed);
    DiagnosticReporter rep{cfg};

    // ★ THE CONTROL, and without it this test proves nothing: a reporter whose
    // suppress set was never consulted at all would satisfy every assertion
    // below. A deliberately SUPPRESSIBLE code, reported into this same
    // reporter, must vanish — that is what establishes the set is live here.
    dss::report(rep, DiagnosticCode::S_DeprecatedSymbolUsed,
                DiagnosticSeverity::Warning,
                "control: this code is suppressible on purpose");
    ASSERT_TRUE(rep.all().empty())
        << "a suppressible code survived the suppress set, so the set is not "
           "being applied and the assertions below would prove nothing";

    ScriptEntry const entry = fixtureEntry(record, kFailStatus);
    EXPECT_FALSE(runBuildScripts({entry}, scratch.path(), rep));
    ASSERT_TRUE(fs::exists(record)) << "the fixture did not actually run";

    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);

    auto const* d = firstWith(rep, DiagnosticCode::D_ScriptExitedNonZero);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->severity, DiagnosticSeverity::Error);
    // The WHOLE message survives, not a stub: suppression is bypassed, not
    // partially applied, so the operator loses nothing by having asked.
    EXPECT_EQ(d->actual,
              "build script '" + entry.run.front()
                  + "' ran and exited with status " + std::to_string(kFailStatus)
                  + ". The program was found and executed, so this is the "
                    "script's own verdict, not an environment problem — fix "
                    "what it reported on this build's inherited stdout/stderr "
                    "and re-run. Non-zero stops the build: a hook that failed "
                    "must not be followed by a compile of stale inputs.");
}

// ── 3. `runOn`, BOTH polarities, on this host ───────────────────────────────

TEST(BuildScripts, RunOnSkipsOtherHosts) {
    std::string const other = aDifferentHost();
    ASSERT_FALSE(other.empty())
        << "could not derive a host token different from currentHostOs() ('"
        << dss::currentHostOs() << "') — the vocabulary should carry three";
    ASSERT_NE(other, std::string{dss::currentHostOs()});

    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const mine    = scratch.path() / "this-host.rec";
    fs::path const foreign = scratch.path() / "other-host.rec";

    std::vector<ScriptEntry> const scripts{
        fixtureEntry(mine, 0, {}, {std::string{dss::currentHostOs()}}),
        fixtureEntry(foreign, 0, {}, {other}),
    };

    DiagnosticReporter rep;
    EXPECT_TRUE(runBuildScripts(scripts, scratch.path(), rep));

    // POLARITY 1 — the entry naming this host RAN. On its own this proves
    // nothing: it also passes when `runOn` is ignored entirely.
    EXPECT_TRUE(fs::exists(mine))
        << "an entry whose runOn names this host ('" << dss::currentHostOs()
        << "') did not run";

    // POLARITY 2 — the entry naming a different host did NOT run. THIS is the
    // half that proves the filter exists.
    EXPECT_FALSE(fs::exists(foreign))
        << "an entry whose runOn names ONLY '" << other
        << "' ran on a '" << dss::currentHostOs()
        << "' host — the runOn filter is not being applied";

    // POLARITY 2b — and it was SKIPPED, not FAILED. A skip is the manifest
    // working as designed; reporting it would make every cross-platform
    // project noisy on two hosts out of three.
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a skipped entry produced a diagnostic; a runOn skip is not a "
           "failure";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 0u);
    EXPECT_TRUE(rep.all().empty()) << "a skip must be completely silent";
}

TEST(BuildScripts, RunOnMatchesByMembershipNotByBeingTheOnlyToken) {
    // A `runOn` listing SEVERAL hosts, one of which is this one, must run. A
    // filter written as `runOn.front() == host` or `runOn.size() == 1 && ...`
    // passes the single-token test above and reds here.
    std::string const other = aDifferentHost();
    ASSERT_FALSE(other.empty());

    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const record = scratch.path() / "multi-token.rec";

    ScriptEntry const entry =
        fixtureEntry(record, 0, {},
                     {other, std::string{dss::currentHostOs()}});

    DiagnosticReporter rep;
    EXPECT_TRUE(runBuildScripts({entry}, scratch.path(), rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_TRUE(fs::exists(record))
        << "an entry whose runOn CONTAINS this host (but does not lead with "
           "it) was skipped — the filter is testing position, not membership";
}

// ── 4. Absent `runOn` runs everywhere ───────────────────────────────────────

TEST(BuildScripts, EmptyRunOnRunsOnEveryHost) {
    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const record = scratch.path() / "unfiltered.rec";

    // Empty `runOn` is EXACTLY the "member absent" case — the loader rejects a
    // present-but-empty `[]`, so this spelling has one meaning and it is
    // "unfiltered". Reading it as "matches nothing" would silently disable
    // every unfiltered hook in the corpus, with no diagnostic anywhere.
    ScriptEntry const entry = fixtureEntry(record, 0, {}, /*runOn=*/{});
    ASSERT_TRUE(entry.runOn.empty());

    DiagnosticReporter rep;
    EXPECT_TRUE(runBuildScripts({entry}, scratch.path(), rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_TRUE(fs::exists(record))
        << "an entry with no runOn filter did not run on a '"
        << dss::currentHostOs() << "' host — the unfiltered default is "
        << "inverted";
}

// ── 5. Spawn failure is a DIFFERENT code ────────────────────────────────────

TEST(BuildScripts, SpawnFailureIsItsOwnCode) {
    ScratchDir scratch(Location::Temp, "build-scripts");

    // A bare name with no path separator, so it is looked up on PATH and found
    // nowhere. The suffix makes an accidental collision with a real program
    // essentially impossible.
    std::string const absent =
        "dss-build-script-no-such-program-4f19c7ab2e";

    ScriptEntry entry;
    entry.run = {absent, "--irrelevant", "argument"};

    DiagnosticReporter rep;
    EXPECT_FALSE(runBuildScripts({entry}, scratch.path(), rep));

    // THREE-SIDED, mirrored against the non-zero-exit pin: this code once, the
    // sibling code never, one error total. A runner that read `exitCode` when
    // `spawned` was false would report the wrong one of the two.
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);

    auto const* d = firstWith(rep, DiagnosticCode::D_ScriptSpawnFailed);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->severity, DiagnosticSeverity::Error);
    EXPECT_NE(d->actual.find(absent), std::string::npos)
        << "the message does not name the program that could not be spawned: "
        << d->actual;

    // THE SUBSTRATE'S OWN TEXT MUST SURVIVE INTO THE MESSAGE. Recomputed by
    // calling the spawn layer directly with the same argv rather than re-typed
    // here — a hard-coded expectation would pin this test to one platform's
    // errno prose, and dropping the interpolation would then still pass on the
    // others.
    dss::substrate::SpawnResult const direct =
        dss::substrate::spawnAndWaitInherit(entry.run, scratch.path());
    ASSERT_FALSE(direct.spawned)
        << "'" << absent << "' unexpectedly spawned; pick a different name";
    ASSERT_FALSE(direct.diagnostic.empty())
        << "the spawn layer reported a failure with an EMPTY diagnostic — "
           "there is nothing for the driver to relay to the operator";
    EXPECT_NE(d->actual.find(direct.diagnostic), std::string::npos)
        << "the spawn layer's own diagnostic ('" << direct.diagnostic
        << "') is missing from the reported message: " << d->actual;

    // REMEDIATION-DISTINCT, asserted rather than assumed: the two codes must
    // not produce interchangeable prose. The spawn message must say the process
    // never started; it must NOT talk about an exit status the child never had.
    EXPECT_NE(d->actual.find("never created"), std::string::npos)
        << "the spawn-failure message does not state that no process was "
           "created, so it reads the same as an exit-status failure: "
        << d->actual;
}

// ── 6. `cwd` is honoured ────────────────────────────────────────────────────

TEST(BuildScripts, SpawnsInTheRequestedWorkingDirectory) {
    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const childCwd = scratch.path() / "child-cwd";
    ASSERT_TRUE(fs::create_directory(childCwd));

    // THE TEST IS ONLY MEANINGFUL IF THE TARGET DIRECTORY IS NOT THE PROCESS
    // CWD. A `cwd` parameter that is silently ignored would pass otherwise.
    std::error_code ec;
    fs::path const processCwd = fs::current_path(ec);
    ASSERT_FALSE(ec);
    ASSERT_NE(canonicalDir(childCwd), canonicalDir(processCwd))
        << "the scratch directory coincides with the process working "
           "directory; this test would prove nothing";

    // The record goes in the SCRATCH root, not in `childCwd` — so the child's
    // report about where it is cannot be confused with where its output landed.
    fs::path const record = scratch.path() / "cwd.rec";
    ScriptEntry const entry = fixtureEntry(record, 0);

    DiagnosticReporter rep;
    ASSERT_TRUE(runBuildScripts({entry}, childCwd, rep))
        << (rep.all().empty() ? std::string{"(no diagnostic)"}
                              : rep.all().front().actual);
    EXPECT_EQ(rep.errorCount(), 0u);

    auto const rec = readRecord(record);
    ASSERT_TRUE(rec.has_value()) << "no record at " << record;
    EXPECT_EQ(canonicalDir(fs::path{rec->cwd}), canonicalDir(childCwd))
        << "the child ran in '" << rec->cwd << "' but was asked for '"
        << childCwd.string() << "'";
    EXPECT_NE(canonicalDir(fs::path{rec->cwd}), canonicalDir(processCwd))
        << "the child ran in the PROCESS working directory — the cwd "
           "parameter was ignored. A dependency manifest's hooks would then "
           "generate their files into the depender's tree.";
}

// ── 7. Degenerate inputs ────────────────────────────────────────────────────

TEST(BuildScripts, EmptyScriptListIsASilentNoOp) {
    ScratchDir scratch(Location::Temp, "build-scripts");
    DiagnosticReporter rep;
    EXPECT_TRUE(runBuildScripts({}, scratch.path(), rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(rep.warningCount(), 0u);
    EXPECT_TRUE(rep.all().empty())
        << "a manifest with no build scripts must produce no diagnostics at "
           "all — the overwhelmingly common case must stay silent";
}

TEST(BuildScripts, ListOfOnlySkippedEntriesSucceedsSilently) {
    std::string const other = aDifferentHost();
    ASSERT_FALSE(other.empty());

    ScratchDir scratch(Location::Temp, "build-scripts");
    fs::path const record = scratch.path() / "never.rec";

    DiagnosticReporter rep;
    EXPECT_TRUE(runBuildScripts({fixtureEntry(record, 0, {}, {other})},
                                scratch.path(), rep));
    EXPECT_TRUE(rep.all().empty());
    EXPECT_FALSE(fs::exists(record));
}

TEST(BuildScripts, EmptyArgvIsRejectedRatherThanUndefined) {
    // The loader rejects `"run": []`, so no manifest reaches this — but the
    // function takes a plain vector from any caller, and `run.front()` on an
    // empty vector is undefined behaviour, i.e. the one failure mode that
    // produces no message at all. Total function, fail-loud.
    ScratchDir scratch(Location::Temp, "build-scripts");
    ScriptEntry entry;  // run is empty
    ASSERT_TRUE(entry.run.empty());

    DiagnosticReporter rep;
    EXPECT_FALSE(runBuildScripts({entry}, scratch.path(), rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptSpawnFailed), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ScriptExitedNonZero), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
}

// ── main: fixture program OR test runner ────────────────────────────────────
//
// Defining `main` here means the linker never pulls `gtest_main`'s object out
// of the static library, so there is no duplicate symbol (the same arrangement
// `tests/test_support/test_run_binary_capture.cpp` uses). The fixture flags are
// consumed BEFORE `InitGoogleTest`, which would otherwise reject them as
// unrecognised — and the check is POSITIONAL (`argv[1]` only), so a payload
// argument that happens to begin with the flag prefix can never be mistaken for
// the flag itself.
int main(int argc, char** argv) {
    if (argc >= 2 &&
        std::string_view{argv[1]}.substr(0, kFixtureFlag.size()) ==
            kFixtureFlag) {
        return runFixtureMode(argc, argv);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
