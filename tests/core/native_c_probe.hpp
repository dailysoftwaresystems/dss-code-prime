#pragma once

// The host-native-C-compiler probe seam shared by the cross-compile-COMPARE ABI
// conformance witnesses (`test_bitfield_abi_conformance.cpp`,
// `test_packed_abi_conformance.cpp`).
//
// WHY THIS IS ONE HEADER AND NOT TWO COPIES. It WAS two copies, and the copies
// diverged in the worst possible direction. TF-C97 fixed the run-step shell quoting
// in the packed witness; the bit-field witness kept the broken spelling and stayed
// inert on every Unix host for another cycle — while printing green. Hoisting is
// what makes "fixed in one place" actually mean "fixed everywhere", and it is the
// reason the fix below cannot silently apply to only one of the two batteries.
//
// A native oracle that skips on error is a broken oracle that reports success.
//
// WHAT THIS SEAM GUARANTEES. `runNativeCProbe` reports WHY it produced no rows, and
// that distinction is the entire point of the file:
//
//   * `ToolAbsent` — this host has no C compiler that can build a trivial program.
//     The ONLY legitimate skip; a toolchain-less dev box must not be blocked.
//   * EVERY other status — the probe FAILED at a step instead of answering. That is
//     a defect (in this harness, or in the toolchain), the oracle compared nothing,
//     and the caller must go RED.
//
// Those two are not "found vs not found": `ToolQueryFailed` can fire while the search
// for a toolchain is still in progress (see `locateMsvcToolchain`). What separates
// them is whether the probe ANSWERED — "absent" is an answer, a step that broke is not.
//
// Collapsing those two into one `nullopt` is precisely how an oracle that executes
// NOTHING reports success: the pre-fix code returned a bare `nullopt` for both, so
// the skip message read "no native C compiler available" while `/usr/bin/cc` sat in
// PATH and the probe binary sat built on disk. MEASURED on macOS before the fix:
//
//   sh: /var/.../probe > /var/.../probe_out.txt: No such file or directory
//   [  SKIPPED ] BitFieldAbiConformance.DssLayoutMatchesNativeCompiler
//   [  PASSED  ] 0 tests.
//
// Exit code 0. Eleven struct comparisons, zero of them executed, ctest green.
//
// ★★★ THE SECOND THING THIS SEAM OWNS: THE MSVC DEVELOPER ENVIRONMENT, ENTERED ONCE PER
// PROCESS (`msvcEnvironment` below). Every native MSVC witness needs `cl`/`lib`/`link` with the
// INCLUDE, LIB and PATH that vcvars64.bat computes — the ABI probes here, and the COFF reader's
// and PE import-slot's witnesses in `tests/link/` through `msvcToolsIn`. They used to enter
// vcvars64.bat in a FRESH cmd.exe for EVERY tool line, from three private copies of the same
// batch, and that entry — not the tools — was what the suites spent their time on.

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
  // The same guard `run_binary.hpp` uses, so a translation unit that includes both sees one
  // configuration of <windows.h>: `CreateProcessW` for `internal::spawnInterpreter`.
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace dss::test_support::native_probe {

namespace fs = std::filesystem;

// The host toolchain family the probe is built with. Callers branch their emitted C
// source on this wherever the spelling differs (`#pragma pack(1)` vs
// `__attribute__((packed))`) and pick the dss strategy that matches the host ABI.
enum class Toolchain : std::uint8_t {
    Unix,  // cc / clang / gcc — GNU ABI
    Msvc,  // cl.exe, entered through vcvars64
};

// WHY a probe produced no rows. Exactly ONE of these is a legitimate skip; keeping
// them as distinct values (rather than one `nullopt`) is what makes the skip-vs-fail
// decision at the call site possible at all.
enum class ProbeStatus : std::uint8_t {
    Ok = 0,
    ToolAbsent,         // no host C compiler that can build a trivial program — the
                        // ONLY legitimate skip
    // ── locating the toolchain ──
    ToolQueryFailed,    // the LOCATOR itself could not be run (it exists, but failed)
    ToolIncomplete,     // a toolchain was located but a required piece is not where
                        // it must be — a stale path assumption in this harness
    // ── entering the toolchain's environment ──
    EnvironmentFailed,  // the toolchain was located, but entering its developer
                        // environment (vcvars64.bat in cmd.exe, then `set`) did not
                        // yield a usable one
    // ── using the toolchain ──
    CompileFailed,      // a compiler IS present and failed on the probe source
    ExecutableMissing,  // the build reported success but left no binary behind
    RunFailed,          // the built probe could not be invoked
    OutputUnreadable,   // it ran, but its captured stdout could not be opened
    EmptyOutput,        // it ran and printed NOTHING — there is nothing to compare
};

// A stable, greppable code per status. A CI log that says `NATIVE-PROBE-RUN-FAILED`
// names the failing STEP; "no native C compiler available" named the wrong one for
// three cycles, which is why these are spelled out instead of stringified inline.
[[nodiscard]] inline char const* statusCode(ProbeStatus s) noexcept {
    switch (s) {
        case ProbeStatus::Ok:                return "NATIVE-PROBE-OK";
        case ProbeStatus::ToolAbsent:        return "NATIVE-PROBE-TOOL-ABSENT";
        case ProbeStatus::ToolQueryFailed:   return "NATIVE-PROBE-TOOL-QUERY-FAILED";
        case ProbeStatus::ToolIncomplete:    return "NATIVE-PROBE-TOOL-INCOMPLETE";
        case ProbeStatus::EnvironmentFailed: return "NATIVE-PROBE-ENVIRONMENT-FAILED";
        case ProbeStatus::CompileFailed:     return "NATIVE-PROBE-COMPILE-FAILED";
        case ProbeStatus::ExecutableMissing: return "NATIVE-PROBE-EXE-MISSING";
        case ProbeStatus::RunFailed:         return "NATIVE-PROBE-RUN-FAILED";
        case ProbeStatus::OutputUnreadable:  return "NATIVE-PROBE-OUTPUT-UNREADABLE";
        case ProbeStatus::EmptyOutput:       return "NATIVE-PROBE-EMPTY-OUTPUT";
    }
    return "NATIVE-PROBE-UNKNOWN";
}

// The fail-loud diagnostic, shared by every consumer of `ProbeStatus`. `tool` names
// what was being LOOKED FOR ("a host C compiler", "an MSVC toolchain") — NOT what was
// found, because some statuses fire while the search is still in progress.
//
// It used to read "<tool> WAS found, but the step after that failed", and that sentence
// is not always true. `-products *` and `-requires` are vswhere 2.x flags: a box whose
// VS Installer ships vswhere 1.x errors on the ARGUMENTS, exits non-zero, and lands on
// `ToolQueryFailed` — with only the INSTALLER located and no toolchain found at all. A
// diagnostic that asserts more than the code knows is the same species of defect as a
// comment that overstates what the code does, so this one states only what IS known:
// a step broke instead of answering.
[[nodiscard]] inline std::string describeFailure(ProbeStatus status,
                                                 std::string_view tool,
                                                 std::string_view detail) {
    return std::string{statusCode(status)} + ": the probe for " + std::string{tool}
         + " FAILED at a step rather than reporting it absent, so the oracle verified "
           "NOTHING. How much of the toolchain is installed is NOT known here — only "
           "that this step broke instead of answering. Treat it as a defect in this "
           "harness or on this host; a machine that genuinely has none reports "
           "NATIVE-PROBE-TOOL-ABSENT and skips. Failing step: "
         + std::string{detail};
}

// The outcome of one compile-and-run probe: a status, the command or path that
// produced it, and the captured stdout split into lines.
struct ProbeResult {
    ProbeStatus              status = ProbeStatus::ToolAbsent;
    Toolchain                toolchain = Toolchain::Unix;
    std::string              detail;  // the command / path that produced `status`
    std::vector<std::string> lines;   // captured stdout, one entry per line

    [[nodiscard]] bool ok() const noexcept { return status == ProbeStatus::Ok; }

    // TRUE only for the one status that a test may legitimately skip on. Written as
    // its own predicate so a call site reads as the policy it is implementing.
    [[nodiscard]] bool toolAbsent() const noexcept {
        return status == ProbeStatus::ToolAbsent;
    }

    // The fail-loud diagnostic. Names the step that broke and says the oracle
    // compared nothing — the two facts the old skip message got wrong.
    [[nodiscard]] std::string describe() const {
        return describeFailure(status, "a host C compiler", detail);
    }
};

[[nodiscard]] inline bool fileExists(fs::path const& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// Run `exe` with stdout redirected into `out`, spelled for the HOST's shell.
//
// These two spellings are NOT interchangeable, and getting it wrong is SILENT: the
// cmd.exe form wraps the whole command in an extra pair of quotes (`""prog" > "file""`)
// so a spaced path survives, but a POSIX shell reads that same string as ONE command
// NAME and reports `prog > file: No such file or directory`. `std::system` then
// returns non-zero — and before this seam existed, the caller turned that into a
// GTEST_SKIP claiming no compiler was installed.
//
// MEASURED: the cmd.exe form was used unconditionally in BOTH conformance witnesses,
// so the cross-compile-compare leg had never once executed on a Unix host.
[[nodiscard]] inline std::string redirectCmd(fs::path const& exe, fs::path const& out) {
#if defined(_WIN32)
    return "\"\"" + exe.string() + "\" > \"" + out.string() + "\"\"";
#else
    return "\"" + exe.string() + "\" > \"" + out.string() + "\"";
#endif
}

// Run an ALREADY-ASSEMBLED command string with BOTH streams captured into `out`.
// A probe whose compile fails must not discard its own output.
//
// Distinct from `redirectCmd` above, which takes an executable PATH and quotes it.
// Here the caller already holds a full command line (on Windows a
// `"…vcvars64.bat" && cl …` chain), so quoting it again would corrupt it — the
// redirection is appended to it instead. cmd.exe still needs the outer pair for the
// same reason redirectCmd needs it: a command line that BEGINS with a quote has its
// first and last quote stripped, and the build command does begin with one.
//
// ⚠ AND `2>&1` IS THE POINT, not a detail. A compiler says why it failed on stderr.
[[nodiscard]] inline std::string captureCmd(std::string const& cmd, fs::path const& out) {
#if defined(_WIN32)
    return "\"" + cmd + " > \"" + out.string() + "\" 2>&1\"";
#else
    return cmd + " > \"" + out.string() + "\" 2>&1";
#endif
}

// The tail of a captured log, folded into a failure detail. `producer` names what wrote
// the log — a build, unless the caller says otherwise (the developer-environment entry
// names its vcvars64.bat). The bytes are carried as they were written, in the console's
// code page, like any tool output this file captures.
//
// ★ THE THREE OUTCOMES ARE KEPT APART — unreadable, empty, and present are three
//   different facts about a failed build, and collapsing them into "" would put the
//   reader back where this whole file started.
[[nodiscard]] inline std::string tailOf(fs::path const& p, std::size_t maxLines,
                                        std::string_view producer = "build") {
    std::string const who{producer};
    std::ifstream in{p};
    if (!in) {
        return "\n  (no " + who + " output: the capture log `" + p.string()
             + "` could not be opened, so WHY it failed is not known here)";
    }
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    if (lines.empty()) return "\n  (the " + who + " command produced NO output at all)";
    std::size_t const from = lines.size() > maxLines ? lines.size() - maxLines : 0;
    std::string out = "\n  --- " + who + " output, last " + std::to_string(lines.size() - from)
                    + " of " + std::to_string(lines.size()) + " line(s) ---";
    for (std::size_t i = from; i < lines.size(); ++i) out += "\n  " + lines[i];
    return out;
}

// Where an MSVC toolchain was found, or WHY it was not.
struct MsvcLocation {
    ProbeStatus status = ProbeStatus::ToolAbsent;
    std::string detail;   // human-readable, in BOTH directions
    fs::path    vcvars;   // the vcvars64.bat that puts cl/lib on PATH; set iff `ok()`

    [[nodiscard]] bool ok() const noexcept { return status == ProbeStatus::Ok; }
    // The ONE status a native witness may legitimately skip on.
    [[nodiscard]] bool toolAbsent() const noexcept {
        return status == ProbeStatus::ToolAbsent;
    }
    [[nodiscard]] std::string describe() const {
        return describeFailure(status, "an MSVC toolchain", detail);
    }
};

// The per-process tallies the COST PINS read (`msvcToolchainQueries`,
// `msvcEnvironmentEntries`, both below). An inline function's static is ONE object per
// program, whichever test file includes this header — that is what makes "per process"
// literally true. Nothing but those two readers touches them.
namespace internal {
[[nodiscard]] inline std::size_t& msvcToolchainQueryTally() noexcept {
    static std::size_t n = 0;
    return n;
}
[[nodiscard]] inline std::size_t& msvcEnvironmentEntryTally() noexcept {
    static std::size_t n = 0;
    return n;
}
}  // namespace internal

// Locate a cl.exe/lib.exe toolchain via vswhere -> vcvars64, and say WHY when that
// fails. THE single implementation — `findCompiler` below, the native COFF and PE
// witnesses in tests/link/ and the reference-conformance oracle all call THIS.
//
// ★★ ASKED ONCE PER PROCESS. The installation does not move while a test binary runs, so
// the first call's answer — found, absent, or the step that broke — IS every later call's
// answer, and vswhere starts once (`msvcToolchainQueries`), not once per native case as
// it used to. `work` is where that first call writes vswhere's output.
//
// WHY IT LIVES HERE. It was written twice, and the two copies DISAGREED about the same
// machine: the coff copy reddened on a non-zero vswhere exit while the copy inside
// `findCompiler` returned a bare `nullopt` that the caller read as "no toolchain" and
// skipped GREEN. So the ABI witnesses kept, on Windows, the exact defect this seam
// exists to close, in the very file that defines the vocabulary for closing it. One
// implementation is the only arrangement under which "fixed in one place" means what
// the docblock at the top of this file says it means.
//
// COMPILED ON EVERY HOST, deliberately — no `#if defined(_WIN32)` around the body. It
// is only ever CALLED on Windows (the paths and the cmd.exe quoting are Windows-only),
// but a Windows-only seam that a Unix developer's compiler never even parses is a seam
// that rots unseen between CI runs. The cmd.exe quoting below is correct FOR cmd.exe;
// nothing here is handed to a POSIX shell.
//
// This lookup used to collapse FOUR distinct outcomes into one skip: vswhere missing,
// vswhere unrunnable, vswhere reporting no matching install, and vcvars64.bat absent
// from an installation that vswhere said HAS the VC tools. Only the first and third
// mean "this machine has no toolchain".
namespace internal {
[[nodiscard]] inline MsvcLocation queryMsvcToolchain(fs::path const& work) {
    ++msvcToolchainQueryTally();
    fs::path const vswhere =
        fs::path{"C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"};
    // No VS Installer at all — this machine genuinely has no toolchain.
    if (!fileExists(vswhere))
        return {ProbeStatus::ToolAbsent,
                "no Visual Studio Installer (vswhere.exe) on this machine", {}};

    fs::path const outTxt = work / "vsinstall.txt";
    std::string const q =
        "\"\"" + vswhere.string() + "\" -latest -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
        "-property installationPath > \"" + outTxt.string() + "\"\"";
    // vswhere.exe EXISTS (just checked) and is documented to exit 0 and print nothing
    // when no instance matches — that "no match" case is the empty-output branch below.
    // A NON-ZERO exit therefore means the invocation itself failed, which is a
    // harness/host defect rather than an absent toolchain.
    //
    // THE KNOWN WAY THAT ASSUMPTION BREAKS: `-products *` and `-requires` are vswhere
    // 2.x flags. A VS Installer old enough to ship vswhere 1.x rejects the ARGUMENTS
    // and exits non-zero without ever evaluating the match — so this branch reds on a
    // box that may have no VC tools at all. That is why `describeFailure` no longer
    // claims a toolchain WAS found: the status says a step broke, not what is installed.
    if (std::system(q.c_str()) != 0)
        return {ProbeStatus::ToolQueryFailed,
                "vswhere.exe is present but the query failed: `" + q
                    + "` (a vswhere 1.x that rejects the 2.x `-products`/`-requires` "
                      "flags lands here too)", {}};

    std::ifstream vin{outTxt};
    std::string vsPath;
    std::getline(vin, vsPath);
    while (!vsPath.empty() && (vsPath.back() == '\r' || vsPath.back() == '\n'))
        vsPath.pop_back();
    // vswhere ran and matched nothing: VS may be installed, but not with the x64 VC
    // tools these witnesses need. A legitimate skip.
    if (vsPath.empty())
        return {ProbeStatus::ToolAbsent,
                "no Visual Studio installation carrying "
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", {}};

    fs::path const vcvars =
        fs::path{vsPath} / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    // vswhere matched an installation that REQUIRES the VC x64 tools, so vcvars64.bat
    // must exist inside it. Missing means this harness is looking in the wrong place
    // (a VS layout change), not that the machine lacks a compiler — skipping green
    // would hide a stale path assumption behind "no toolchain" forever.
    if (!fileExists(vcvars))
        return {ProbeStatus::ToolIncomplete,
                "vswhere matched `" + vsPath + "` as having the x64 VC tools, but `"
                    + vcvars.string() + "` does not exist", {}};

    return {ProbeStatus::Ok, "", vcvars};
}
}  // namespace internal

[[nodiscard]] inline MsvcLocation locateMsvcToolchain(fs::path const& work) {
    static MsvcLocation const located = internal::queryMsvcToolchain(work);
    return located;
}

// How many times THIS PROCESS has asked vswhere — the number `locateMsvcToolchain` keeps
// at one. Read by the cost pins; nothing may depend on it for behaviour.
[[nodiscard]] inline std::size_t msvcToolchainQueries() noexcept {
    return internal::msvcToolchainQueryTally();
}

// ══ THE MSVC DEVELOPER ENVIRONMENT — ENTERED ONCE PER PROCESS ═════════════════════════
//
// ★★★ THE ENTRY WAS THE COST, NOT THE TOOLS. `cl`, `lib` and `link` need the INCLUDE, LIB
// and PATH that vcvars64.bat computes, and every native witness used to get them by
// CALLing vcvars64.bat in a FRESH cmd.exe for EACH tool line: the COFF reader's witnesses
// 11 times per run, the PE import-slot ones 10 times, each ABI probe once. ✔MEASURED by
// lane mig on 2026-09-19: one entry costs 1.2–2.4 s alone and 3.9–16.2 s beside a
// concurrent build, an entry PLUS `cl /c` of a one-line file costs no more than the entry
// alone, and both link suites timed out at 315 s in a gate run beside another lane's
// build — while neither had grown.
//
// ★★ SO IT IS ENTERED ONCE PER PROCESS AND ITS RESULT IS DATA. The first caller runs
// vcvars64.bat in cmd.exe and has THAT cmd.exe print the environment it left (`set`);
// every command line the process runs afterwards goes to a fresh cmd.exe HANDED that
// environment. Each command line and its working directory are what they were; only the
// re-entry is gone (and AutoRun — see `/d` below). A second, different vcvars64.bat in one
// process is REFUSED, never entered.
//
// ★ WHY `set` IS READ AS UTF-16. Under `cmd /u` an internal command writes UTF-16LE to a
// file; without it `set` writes 8-bit text in a code page, and a value holding a character
// that page lacks would reach every tool altered. ✔MEASURED: a variable holding `café-中`
// round-trips through `cmd /d /u /c` + `set > file` exactly, with no BOM.
//
// ⚠ `/d` ON EVERY cmd.exe STARTED HERE: an AutoRun command (the `Command Processor`
// registry key) would otherwise run before the capture and again before every tool line —
// a host setting deciding what environment a witness measured.
//
// ★★★ THE ENTRY STARTS FROM WHAT THE PARENT WAS BEFORE ITS OWN ENTRY. A process started
// from a developer environment — a gate leg imports vcvars64 into the shell that runs
// ctest — hands that environment to this entry, and vcvars64 does not replace what an
// earlier entry added: it PREPENDS again. ✔MEASURED 2026-09-24 on the Windows gate host:
// PATH 2,655 → 4,328 → 6,001 → 7,674 characters over three nested entries, INCLUDE,
// LIB, LIBPATH and EXTERNAL_INCLUDE growing alike, and the entry from 7,674 dies inside
// VsDevCmd: cmd.exe prints its line-too-long message (a line expanded past its 8,191
// characters), then a syntax error, and exits 255 before `set` runs. A gate chain that
// imported vcvars64 three times into one PowerShell reddened six native witnesses
// NATIVE-PROBE-ENVIRONMENT-FAILED that way, and this file could not say why: the entry's
// `>nul 2>&1` had thrown cmd's two lines away. PAST the limit it is worse, because it is
// silent: cmd.exe expands a variable longer than 8,191 characters to NOTHING, so an entry from
// such a parent SUCCEEDS with every inherited PATH directory dropped (✔MEASURED from Git Bash,
// whose longer PATH put three nested entries at 8,387 characters: the entry left 1,486). The
// undo below cures both for a parent that entered before; for one whose OWN PATH is past the
// limit nothing can, and the entry refuses the environment rather than hand it on (the check
// at the end of `enterMsvcEnvironment`).
//
// So the batch first undoes a prior entry the way VsDevCmd's own `-clean_env` does
// (vsdevcmd_end.bat), keyed on `__VSCMD_PREINIT_PATH` — the variable vsdevcmd_start.bat
// keys its capture on, set by every entry and never overwritten by a nested one:
//   * it clears the four variables that name an installation — vsdevcmd_start.bat TRUSTS
//     an inherited VSINSTALLDIR, so a parent that entered another installation would
//     otherwise choose the tools this one reports;
//   * it sets PATH, INCLUDE, LIB, LIBPATH and EXTERNAL_INCLUDE back to their pre-entry
//     values and CLEARS the ones that had none, as clean_env does. ✔MEASURED: after an
//     entry from the gate host's shell only `__VSCMD_PREINIT_PATH` is defined, so a
//     restore of each list "when its PREINIT variable is defined" would have left
//     INCLUDE, LIB, LIBPATH and EXTERNAL_INCLUDE growing;
//   * it clears those PREINIT variables, so the entry captures them afresh.
// Like `-clean_env`, the undo also drops what the parent put on PATH AFTER its own entry —
// the CI workflow's Ninja and CMake steps run after its `msvc-dev-cmd` step: the entered
// PATH is the installation's around the pre-entry one. Every tool a native witness runs
// under it is the installation's own (`cl`, `lib`, `link`) or a program it built.
// ✔MEASURED: an entry from a three-times-nested parent then leaves every variable a tool
// reads identical to an entry from the shell; only VsDevCmd's restore bookkeeping (the
// `__VSCMD_PREINIT_` variables its components add) differs. NOT undone, deliberately: a
// component's own state — vcvars.bat still honours an inherited VCToolsVersion, as it
// does for any entry, and in a nest of one installation that IS its default toolset.
//
// ★ vcvars64's own output goes to `dss_msvc_environment.log` in `work`, and a failed
// entry's detail carries its last lines. The ENTERING cmd.exe runs WITHOUT `/u` so that
// log is in ONE encoding: ✔MEASURED, under `/u` cmd's own lines were UTF-16 while a child
// process's (the `for /F` cmd.exe inside VsDevCmd) were 8-bit, and an 8-bit run of odd
// length shifts every later line. `set` still prints UTF-16, from a child
// `cmd /d /u /c set` — ✔MEASURED to print the same variables as the entering cmd.exe's own.
//
// ★ `VSCMD_SKIP_SENDTELEMETRY=1` is VsDevCmd.bat's own switch. Without it every entry
// STARTs a detached powershell.exe that inherits the batch's handles and outlives it —
// ✔MEASURED, it still held the vcvars log open after cmd.exe had exited.

// What entering vcvars64.bat produced: the environment every MSVC tool this process starts
// runs in, or WHY there is none.
struct MsvcEnvironment {
    ProbeStatus               status = ProbeStatus::ToolAbsent;
    std::string               detail;     // human-readable, in BOTH directions
    fs::path                  vcvars;     // the vcvars64.bat that was entered
    std::vector<std::wstring> variables;  // `NAME=VALUE`, in the order cmd.exe printed them
    std::wstring              comspec;    // the interpreter every command line runs under

    [[nodiscard]] bool ok() const noexcept { return status == ProbeStatus::Ok; }
    [[nodiscard]] std::string describe() const {
        return describeFailure(status, "an MSVC developer environment", detail);
    }
};

namespace internal {

// A wide string for a diagnostic, one character per code unit, anything outside ASCII
// shown as `?`. Only ever used to NAME something in a message — never to compare or to
// hand to the OS, where the wide string itself is used.
[[nodiscard]] inline std::string asciiOf(std::wstring_view w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t const c : w) s.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

[[nodiscard]] inline wchar_t asciiUpper(wchar_t c) noexcept {
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

// This process's own ComSpec — the cmd.exe `std::system` would start. Empty off Windows.
[[nodiscard]] inline std::wstring thisProcessComspec() {
#if defined(_WIN32)
    wchar_t const* const v = _wgetenv(L"ComSpec");
    return v != nullptr ? std::wstring{v} : std::wstring{};
#else
    return {};
#endif
}

// One of this process's own environment variables, or nullopt when it has none (always off
// Windows, where no entry is ever made). Only NAMES a length in a diagnostic.
[[nodiscard]] inline std::optional<std::wstring> thisProcessVariable(wchar_t const* name) {
#if defined(_WIN32)
    wchar_t const* const v = _wgetenv(name);
    if (v != nullptr) return std::wstring{v};
#else
    (void)name;
#endif
    return std::nullopt;
}

// The NAME of a `NAME=VALUE` entry, for ordering an environment block.
[[nodiscard]] inline std::wstring_view variableName(std::wstring const& entry) {
    return std::wstring_view{entry}.substr(0, entry.find(L'='));
}

// Start the interpreter `comspec` with `tail` — its switches and the command — as the rest
// of its command line, and WAIT, as `std::system` does: `CreateProcessW` with handle
// inheritance (the child gets this process's console and standard handles, as
// `std::system`'s child does), `environment` as the child's WHOLE environment (null = this
// process's own), then `WaitForSingleObject` + `GetExitCodeProcess`. Returns the
// interpreter's exit code (the last command's), or -1 when it could not be started.
//
// ★★ NOT THE C RUNTIME'S `_wspawnve`, AND THAT IS MEASURED. It was the first cut here, and
// with an explicit environment it CRASHED the calling process — an access violation inside
// ucrtbase, depending on who had started the caller (✔MEASURED 2026-09-19, same binary: from
// cmd.exe 0 of 3, from PowerShell 2 of 2, from Git Bash 5 of 5 filtered runs, and 2 of 4
// unfiltered ones). The faulting loop steps through environment strings testing each first
// character for `=`; 🧠INFERRED, it looks in the CALLER's own block for the per-drive `=C:`
// entries that only cmd.exe's children carry and does not stop at the block's end, so
// whether it faults depends on whatever memory follows. `CreateProcessW` is the documented
// contract and scans nothing of ours.
//
// COMPILED ON EVERY HOST like `locateMsvcToolchain`; only the spawn is Windows-only, and
// off Windows there is no cmd.exe to start, which is what -1 already says.
[[nodiscard]] inline std::intptr_t spawnInterpreter(std::wstring const& comspec,
                                                    std::wstring const& tail,
                                                    std::vector<std::wstring> const* environment) {
#if defined(_WIN32)
    // `lpCommandLine` is written through by CreateProcessW: a buffer we own.
    std::wstring const  line = L"\"" + comspec + L"\" " + tail;
    std::vector<wchar_t> commandLine(line.begin(), line.end());
    commandLine.push_back(L'\0');

    // CreateProcessW's documented environment contract: every `NAME=VALUE` NUL-terminated,
    // the block closed by one more NUL, SORTED by name — case-insensitively, in Unicode
    // order, without regard to locale, which is what `CompareStringOrdinal` compares.
    std::vector<wchar_t> block;
    if (environment != nullptr) {
        std::vector<std::wstring const*> order;
        order.reserve(environment->size());
        for (std::wstring const& v : *environment) order.push_back(&v);
        std::stable_sort(order.begin(), order.end(),
                         [](std::wstring const* a, std::wstring const* b) {
                             std::wstring_view const na = variableName(*a);
                             std::wstring_view const nb = variableName(*b);
                             return ::CompareStringOrdinal(
                                        na.data(), static_cast<int>(na.size()), nb.data(),
                                        static_cast<int>(nb.size()), TRUE) == CSTR_LESS_THAN;
                         });
        for (std::wstring const* v : order) {
            block.insert(block.end(), v->begin(), v->end());
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
    }

    STARTUPINFOW        si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL const started = ::CreateProcessW(
        comspec.c_str(), commandLine.data(),
        /*lpProcessAttributes*/ nullptr, /*lpThreadAttributes*/ nullptr,
        /*bInheritHandles*/ TRUE,
        /*dwCreationFlags*/ environment != nullptr ? CREATE_UNICODE_ENVIRONMENT : 0,
        /*lpEnvironment*/ environment != nullptr ? block.data() : nullptr,
        /*lpCurrentDirectory*/ nullptr, &si, &pi);
    if (!started) return -1;
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD      code = 0;
    BOOL const read = ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return read ? static_cast<std::intptr_t>(code) : -1;
#else
    (void)comspec;
    (void)tail;
    (void)environment;
    return -1;
#endif
}

// cmd.exe's `set` output as `cmd /u` writes it (UTF-16LE), split into `NAME=VALUE`
// entries. Anything else — an odd byte count, an empty line, a line with no `=` or one that
// BEGINS with it — returns empty and says why in `why`, rather than being skipped: a value
// that spanned two lines would otherwise hand every tool one truncated variable and one
// garbage one, silently.
[[nodiscard]] inline std::vector<std::wstring> parseSetOutput(std::string const& bytes,
                                                              std::string&       why) {
    if (bytes.size() % 2 != 0) {
        why = "an odd byte count (" + std::to_string(bytes.size()) + "), so not UTF-16";
        return {};
    }
    std::wstring text;
    text.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        unsigned const lo = static_cast<unsigned char>(bytes[i]);
        unsigned const hi = static_cast<unsigned char>(bytes[i + 1]);
        text.push_back(static_cast<wchar_t>(lo | (hi << 8)));
    }
    std::vector<std::wstring> vars;
    std::size_t               start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::size_t const eq = line.find(L'=');
        if (eq == 0 || eq == std::wstring::npos) {
            why = "line " + std::to_string(vars.size() + 1) + " (`" + asciiOf(line)
                + "`) is not NAME=VALUE";
            return {};
        }
        vars.push_back(std::move(line));
    }
    if (vars.empty()) why = "it printed no variable at all";
    return vars;
}

// The value of `name` in `vars`, matched the way Windows matches environment names
// (case-insensitively), or nullopt when it is absent.
[[nodiscard]] inline std::optional<std::wstring>
variableIn(std::vector<std::wstring> const& vars, std::wstring_view name) {
    for (std::wstring const& v : vars) {
        std::size_t const eq = v.find(L'=');
        if (eq != name.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < eq && same; ++i)
            same = asciiUpper(v[i]) == asciiUpper(name[i]);
        if (same) return v.substr(eq + 1);
    }
    return std::nullopt;
}

// What an entry does with a parent that already entered a developer environment.
// `Undone` is the only mode `msvcEnvironment` uses. `Kept` is the batch without the undo —
// what every entry was before it — and exists for one caller: the control arm that proves
// a nested parent still breaks an entry that does not undo it, so the pin beside it
// cannot pass on a parent too short to break anything.
enum class PriorEntry : std::uint8_t { Undone, Kept };

// The lists VsDevCmd prepends to and its `-clean_env` restores (vsdevcmd_end.bat), each
// saved before an entry as `__VSCMD_PREINIT_<name>`.
inline constexpr char const* kVsDevCmdRestoredLists[] = {"PATH", "INCLUDE", "LIB", "LIBPATH",
                                                         "EXTERNAL_INCLUDE"};

// The entry batch: undo a prior entry (`prior == Undone`; the block above says what and
// why), enter `vcvars` with its output kept in `log`, then print the environment it left
// into `out` as UTF-16.
[[nodiscard]] inline std::string entryBatch(fs::path const& vcvars, fs::path const& log,
                                            fs::path const& out, PriorEntry prior) {
    std::string b = "@echo off\r\n"
                    "set VSCMD_SKIP_SENDTELEMETRY=1\r\n";
    if (prior == PriorEntry::Undone) {
        b += "if not defined __VSCMD_PREINIT_PATH goto :enter\r\n"
             "set DevEnvDir=\r\n"
             "set VSINSTALLDIR=\r\n"
             "set VSCMD_VER=\r\n"
             "set VisualStudioVersion=\r\n";
        for (char const* list : kVsDevCmdRestoredLists)
            b += std::string{"set \""} + list + "=%__VSCMD_PREINIT_" + list + "%\"\r\n";
        for (char const* list : kVsDevCmdRestoredLists)
            b += std::string{"set __VSCMD_PREINIT_"} + list + "=\r\n";
        b += ":enter\r\n";
    }
    b += "call \"" + vcvars.string() + "\" > \"" + log.string() + "\" 2>&1\r\n";
    b += "\"%ComSpec%\" /d /u /c set > \"" + out.string() + "\"\r\n";
    return b;
}

// Enter `loc`'s vcvars64.bat and read back the environment it left — the step
// `msvcEnvironment` runs ONCE per process. `work` receives its three files: the batch,
// vcvars64's own output, and what `set` printed. `parent` is the environment cmd.exe is
// handed (null: this process's own, which is all `msvcEnvironment` ever passes). Counted
// by `msvcEnvironmentEntries` before cmd.exe starts, so an entry that fails still counts
// as the cost it was.
[[nodiscard]] inline MsvcEnvironment
enterMsvcEnvironment(MsvcLocation const& loc, fs::path const& work,
                     std::vector<std::wstring> const* parent = nullptr,
                     PriorEntry                       prior  = PriorEntry::Undone) {
    MsvcEnvironment e;
    e.vcvars = loc.vcvars;
    e.status = ProbeStatus::EnvironmentFailed;
    fs::path const bat = work / "dss_msvc_environment.bat";
    fs::path const log = work / "dss_msvc_environment.log";
    fs::path const out = work / "dss_msvc_environment.txt";
    // A file left by an earlier entry in the same `work` would be read as THIS entry's
    // answer — a failed entry reported with a previous one's environment or output.
    for (fs::path const& stale : {log, out}) {
        std::error_code ec;
        fs::remove(stale, ec);
        if (ec) {
            e.detail = "could not remove `" + stale.string() + "`, left by an earlier entry: "
                     + ec.message();
            return e;
        }
    }
    {
        std::ofstream b{bat, std::ios::binary};
        b << entryBatch(loc.vcvars, log, out, prior);
        b.close();
        if (!b) {
            e.detail = "could not write the environment-capture batch `" + bat.string() + "`";
            return e;
        }
    }
    std::wstring const comspec = thisProcessComspec();
    if (comspec.empty()) {
        e.detail = "this process has no ComSpec, so there is no cmd.exe to enter `"
                 + loc.vcvars.string() + "` in";
        return e;
    }
    ++msvcEnvironmentEntryTally();
    std::intptr_t const rc =
        spawnInterpreter(comspec, L"/d /c \"\"" + bat.wstring() + L"\"\"", parent);
    if (rc == -1) {
        e.detail = "`" + asciiOf(comspec) + "` could not be started to enter `"
                 + loc.vcvars.string() + "`";
        return e;
    }
    // What vcvars64 SAID, for every failure below: the reason an entry failed is in its
    // own output, and a detail without it names the step and not the cause.
    auto const said = [&] { return tailOf(log, 12, loc.vcvars.filename().string()); };
    // The batch's exit code is NOT the verdict, and deliberately: no copy of this batch
    // ever read vcvars64's own exit code — the tools' exit codes decided, and still do.
    // What `set` printed decides here; the exit code is only reported.
    std::ifstream in{out, std::ios::binary};
    if (!in) {
        e.detail = "cmd.exe entered `" + loc.vcvars.string() + "` (exit "
                 + std::to_string(rc) + ") but `set` left nothing at `" + out.string() + "`"
                 + said();
        return e;
    }
    std::string const bytes{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
    std::string       why;
    e.variables = parseSetOutput(bytes, why);
    if (e.variables.empty()) {
        e.detail = "`set` after `" + loc.vcvars.string() + "` (exit " + std::to_string(rc)
                 + ") did not print an environment into `" + out.string() + "`: " + why
                 + said();
        return e;
    }
    std::optional<std::wstring> const shell = variableIn(e.variables, L"ComSpec");
    std::optional<std::wstring> const path  = variableIn(e.variables, L"PATH");
    if (!shell || shell->empty() || !path) {
        e.variables.clear();
        e.detail = "the environment `" + loc.vcvars.string() + "` left (exit "
                 + std::to_string(rc) + ", `" + out.string()
                 + "`) has no ComSpec or no PATH, so no tool could be started in it"
                 + said();
        return e;
    }
    // ★★ THE ENTRY MUST KEEP THE PATH IT WAS ENTERED FROM, and a successful exit does not say
    // it did. cmd.exe expands a variable longer than its 8,191-character line limit to NOTHING,
    // silently, so from a parent whose PATH is past the limit — with no shorter saved one for
    // the undo to restore — vcvars64 builds its PATH from an empty one, saves no pre-entry PATH
    // and exits 0. ✔MEASURED 2026-09-24 from a parent that never entered, PATH 8,607
    // characters: exit 0, the entered PATH 1,486 characters, the installation's directories
    // only, System32 gone. vsdevcmd_start.bat saves the PATH it was entered from as
    // `__VSCMD_PREINIT_PATH`, and an entry that kept it holds that value whole — the one trailing
    // `;` VsDevCmd's normalization may take off it aside.
    std::optional<std::wstring> const saved = variableIn(e.variables, L"__VSCMD_PREINIT_PATH");
    bool kept = saved.has_value() && !saved->empty() && path->find(*saved) != std::wstring::npos;
    if (!kept && saved && saved->size() > 1 && saved->back() == L';')
        kept = path->find(saved->substr(0, saved->size() - 1)) != std::wstring::npos;
    if (!kept) {
        std::size_t const from = [&]() -> std::size_t {
            std::optional<std::wstring> const p =
                parent != nullptr ? variableIn(*parent, L"PATH") : thisProcessVariable(L"PATH");
            return p ? p->size() : 0;
        }();
        e.variables.clear();
        e.detail = "the environment `" + loc.vcvars.string() + "` left (exit " + std::to_string(rc)
                 + ", `" + out.string() + "`) did not keep the PATH it was entered from: "
                 + (saved ? "its PATH (" + std::to_string(path->size())
                                + " characters) does not hold the pre-entry PATH vcvars64 saved ("
                                + std::to_string(saved->size()) + " characters)"
                          : std::string{"vcvars64 saved no pre-entry PATH"})
                 + ". The PATH it was entered from was " + std::to_string(from)
                 + " characters; cmd.exe expands a variable longer than its 8,191-character line "
                   "limit to nothing, so a tool run in this environment would find none of it"
                 + said();
        return e;
    }
    e.comspec = *shell;
    e.status  = ProbeStatus::Ok;
    return e;
}

}  // namespace internal

// THE ONE ENTRY. The first call in a process enters `loc`'s vcvars64.bat (writing its three
// files into `work`); every later call returns that same environment without starting
// anything, and one naming a DIFFERENT vcvars64.bat is refused rather than entered — one
// process, one developer environment. A location that is not `ok()` is answered with its
// own status and enters nothing, so a caller's skip-vs-fail decision is unchanged.
[[nodiscard]] inline MsvcEnvironment msvcEnvironment(MsvcLocation const& loc,
                                                     fs::path const&     work) {
    if (!loc.ok()) {
        MsvcEnvironment e;
        e.status = loc.status;
        e.detail = loc.detail;
        return e;
    }
    static MsvcEnvironment const entered = internal::enterMsvcEnvironment(loc, work);
    if (entered.vcvars != loc.vcvars) {
        MsvcEnvironment e;
        e.status = ProbeStatus::EnvironmentFailed;
        e.detail = "this process already entered `" + entered.vcvars.string()
                 + "` and was then asked for `" + loc.vcvars.string()
                 + "`: one process, one developer environment";
        return e;
    }
    return entered;
}

// How many times THIS PROCESS has entered vcvars64.bat — the number `msvcEnvironment`
// exists to keep at one. Read by the cost pins; nothing may depend on it for behaviour.
[[nodiscard]] inline std::size_t msvcEnvironmentEntries() noexcept {
    return internal::msvcEnvironmentEntryTally();
}

// `std::system(command)`, run under `env` instead of this process's own environment: the
// same interpreter (cmd.exe, `env`'s ComSpec), handed `/c <command>` as `std::system` hands
// it — so a command quoted FOR `std::system` (`captureCmd`'s outer pair) means the same
// here — and the same answer back: the command's exit code, -1 when cmd.exe did not start.
//
// The narrow `command` is widened by `fs::path`, the same conversion its `string()` used to
// narrow the paths inside it: the inverse of what built the command, not a guessed code page.
[[nodiscard]] inline int systemUnder(MsvcEnvironment const& env, std::string const& command) {
    if (!env.ok()) return -1;
    return static_cast<int>(internal::spawnInterpreter(
        env.comspec, L"/d /c " + fs::path{command}.wstring(), &env.variables));
}

// The `cl` / `lib` / `link` a native witness drives: every line runs in `work`, under the
// process's ONE developer environment, its output discarded — a tool answers through its
// exit code, and every call site asserts it. This replaced the file-local `MsvcEnv` the
// COFF and PE witnesses each carried, which re-entered vcvars64.bat for every line.
struct MsvcTools {
    MsvcEnvironment env;
    fs::path        work;

    [[nodiscard]] bool ready() const noexcept { return env.ok(); }
    [[nodiscard]] std::string describe() const { return env.describe(); }

    // One tool line: `cd /d` then the tool — the old batch's two lines, joined by `&&` so a
    // `cd` that fails runs nothing rather than running the tool somewhere else.
    [[nodiscard]] bool run(std::string const& cmdline) const {
        return ready()
            && systemUnder(env, "\"cd /d \"" + work.string() + "\" && " + cmdline
                                    + " >nul 2>&1\"") == 0;
    }
};

// The tools of the process's developer environment, run in `work`. Check `ready()` before
// the first `run`: an environment that could not be entered says why there.
[[nodiscard]] inline MsvcTools msvcToolsIn(MsvcLocation const& loc, fs::path const& work) {
    return MsvcTools{msvcEnvironment(loc, work), work};
}

// A located host C compiler: its family, plus a shell-ready command that builds
// `<src>` into `<exe>`.
struct Compiler {
    Toolchain kind = Toolchain::Unix;
    std::function<std::string(fs::path const&, fs::path const&)> buildCmd;
    // Set iff `kind == Msvc`: the toolchain whose developer environment `buildCmd`'s command
    // must run in. `runNativeCProbe` enters it (once per process) at the first BUILD, not
    // `findCompiler` — a caller that only asks which toolchain this is enters nothing.
    std::optional<MsvcLocation> msvc;
};

// Where a host C compiler was found, or WHY it was not. The status is the whole point:
// `findCompiler` returning a bare `std::optional` is what let a BROKEN lookup read as
// an ABSENT one at the call site.
struct CompilerLocation {
    ProbeStatus             status = ProbeStatus::ToolAbsent;
    std::string             detail;
    std::optional<Compiler> compiler;  // set iff `status == Ok`
};

// Locate a host C compiler. On Windows this wraps cl.exe in a generated batch that runs
// under the process's ONE developer environment (cl needs INCLUDE/LIB — see
// `msvcEnvironment`); on Unix it uses cc/clang/gcc directly, no environment setup.
//
// EVERY failure carries its own status, and `runNativeCProbe` propagates it. Before
// that, all four Windows failure modes returned the same `nullopt` the Unix
// no-compiler case did, so `ToolQueryFailed` and `ToolIncomplete` were unreachable
// from `runNativeCProbe` — dead values in the file that defines them, and a Windows
// ABI witness that still skipped green on a harness defect.
//
// The Windows arm's cmd.exe quoting is CORRECT — it is only ever parsed by cmd.exe. It
// is the run step above, which was NOT guarded, that carried the original defect.
//
// ASYMMETRY, on purpose: only the Unix arm builds a trivial program before declaring a
// compiler present. `locateMsvcToolchain` already proves an INSTALLATION (vswhere
// matched an instance carrying the x64 VC tools, and its vcvars64.bat is on disk), so a
// `cl` that then fails on the battery is a real defect and `CompileFailed` is the right
// answer. Unix has no such proof available — `command -v` resolves a NAME, and on macOS
// that name is a shim — which is why that arm needs the extra step and this one does not.
[[nodiscard]] inline CompilerLocation findCompiler(fs::path const& work) {
#if defined(_WIN32)
    auto const msvc = locateMsvcToolchain(work);
    if (!msvc.ok()) return {msvc.status, msvc.detail, std::nullopt};
    Compiler c;
    c.kind = Toolchain::Msvc;
    c.msvc = msvc;
    c.buildCmd = [work](fs::path const& src, fs::path const& exe) -> std::string {
        // Generate a build batch: cd, then cl. Quote everything. It runs under the
        // process's developer environment (`runNativeCProbe` hands it over), so it no
        // longer CALLs vcvars64 itself — that line was one entry per probe.
        fs::path const bat = work / "build_probe.bat";
        std::ofstream b{bat};
        // ★ `cl` IS NO LONGER SILENCED — a failing compile must not discard its
        // own output.
        // `>nul 2>&1` here threw the compiler's diagnostics away INSIDE the batch,
        // so the caller's capture — added in the same pass — collected an empty log
        // and could only report "the build command produced NO output at all". ✔That
        // is exactly what it reported when the arm was first exercised, which is how
        // this line was found: the capture was right and was pointed at a stream
        // already emptied one level down.
        // ★★★ `cd /d <work>` IS THE FIX FOR THE PROBE COMPILE THAT FAILED UNDER
        // CONCURRENT LOAD, AND IT IS A ROOT CAUSE, NOT A RETRY.
        // ✔MEASURED (ctest -j 6, dss-wt-lane2): `core/test_bitfield_abi_conformance`
        // failed `NATIVE-PROBE-COMPILE-FAILED` with the compiler's own words —
        //   probe.c : fatal error C1083: Cannot open compiler generated file:
        //   '…/build-dbg/tests/core/probe.obj': Permission denied
        // — and PASSED when re-run alone. `/Fe:` names the EXE in this test's
        // private scratch, but nothing named the OBJECT, so `cl` wrote
        // `<stem>.obj` into the PROCESS CWD. Under ctest that cwd is the SHARED
        // build directory, and every native-probe test names its source
        // `probe.c`, so all of them write ONE path: `…/tests/core/probe.obj`.
        // Two probes in flight at once therefore fight over a single file, and
        // the loser reports a toolchain failure that has nothing to do with any
        // toolchain. Serializing the suite would have "fixed" it by hiding it —
        // a workaround this project has refused by name.
        // ⚠ `cd /d` rather than `/Fo`, deliberately: `.obj` is not the only
        // artifact `cl` drops in the cwd (`vc*.pdb` is another), so moving the
        // WORKING DIRECTORY closes the whole class instead of the one member
        // that happened to be observed. `work` is already per-test-unique.
        b << "@echo off\r\n"
          << "cd /d \"" << work.string() << "\"\r\n"
          << "cl /nologo /W3 /Fe:\"" << exe.string() << "\" \""
          << src.string() << "\"\r\n";
        b.close();
        return "\"\"" + bat.string() + "\"\"";
    };
    return {ProbeStatus::Ok, "", c};
#else
    // A compiler is PRESENT only if it can build a trivial program. `command -v cc`
    // is not that test, and the difference is not hypothetical: `/usr/bin/cc` is the
    // xcrun shim and exists on EVERY macOS install, including ones with no Command
    // Line Tools, where invoking it fails with `xcrun: error: invalid active developer
    // path`. Gating on mere presence sends such a machine into the battery build,
    // which fails, which is CompileFailed — a RED on a host that has no toolchain at
    // all. That is this file's own defect pointed the other way: the first version
    // called a broken invocation "absent", and gating on `command -v` alone calls an
    // absent toolchain "broken".
    //
    // So: build `int main(void){return 0;}` first. If THAT cannot be built by any
    // candidate, the machine has no usable compiler and `ToolAbsent` (skip) is the
    // honest answer. `CompileFailed` is then reserved for the case that actually means
    // something — the trivial program builds and the battery does not.
    //
    // stderr is NOT suppressed here: on a shim-only box the `xcrun:` line is the
    // explanation for the skip, and it costs nothing on a box where cc works.
    fs::path const helloSrc = work / "toolchain_check.c";
    {
        std::ofstream h{helloSrc};
        h << "int main(void){return 0;}\n";
    }
    fs::path const helloExe = work / "toolchain_check";
    std::string tried;
    for (char const* cc : {"cc", "clang", "gcc"}) {
        std::string const onPath = std::string{"command -v "} + cc + " >/dev/null 2>&1";
        if (std::system(onPath.c_str()) != 0) continue;
        if (!tried.empty()) tried += ", ";
        tried += cc;

        std::string const ccName = cc;
        auto const buildCmd = [ccName](fs::path const& src,
                                       fs::path const& exe) -> std::string {
            return ccName + " -std=c11 -O0 -o \"" + exe.string() + "\" \""
                 + src.string() + "\"";
        };
        std::error_code ec;
        fs::remove(helloExe, ec);  // a stale binary must not vouch for a broken cc
        if (std::system(buildCmd(helloSrc, helloExe).c_str()) != 0) continue;
        if (!fileExists(helloExe)) continue;

        Compiler c;
        c.kind = Toolchain::Unix;
        c.buildCmd = buildCmd;
        return {ProbeStatus::Ok, "", c};
    }
    return {ProbeStatus::ToolAbsent,
            tried.empty()
                ? "no cc/clang/gcc on PATH"
                : "PATH has " + tried + ", but none of them could build "
                  "`int main(void){return 0;}` — a shim without a toolchain behind it "
                  "(e.g. macOS with no Command Line Tools) counts as ABSENT, not broken",
            std::nullopt};
#endif
}

// Compile and run one native C probe, capturing its stdout line by line.
//
// `tag` names the ScratchDir group. `writeSource` emits the probe's C source at the
// path it is handed, and receives the toolchain family so it can spell the
// host-specific bits. Every failure after `findCompiler` succeeds is reported with
// its own status — see the header docblock for why that matters.
[[nodiscard]] inline ProbeResult
runNativeCProbe(std::string_view tag,
                std::function<void(fs::path const&, Toolchain)> const& writeSource) {
    ScratchDir scratch{Location::Temp, tag};
    fs::path const work = scratch.path();

    // PROPAGATE the locator's status verbatim. Overwriting it with a blanket
    // `ToolAbsent` here is exactly the conflation this seam exists to remove: a
    // vswhere that failed to RUN is not a machine without a compiler.
    auto const loc = findCompiler(work);
    if (!loc.compiler) {
        ProbeResult r;
        r.status = loc.status;
        r.detail = loc.detail;
        return r;
    }

    ProbeResult r;
    r.toolchain = loc.compiler->kind;

    fs::path const src = work / "probe.c";
    fs::path const exe = work / (
#if defined(_WIN32)
        "probe.exe"
#else
        "probe"
#endif
    );
    writeSource(src, loc.compiler->kind);

    std::string const build = loc.compiler->buildCmd(src, exe);
    // ★ CAPTURE THE BUILD'S OWN OUTPUT — a failing compile must not discard it.
    // ⚠ AND THIS ARM HAS AN UNEXPLAINED OCCURRENCE BEHIND IT: one
    // non-reproducing probe-compile failure under a concurrently-running second
    // gate, scratch-dir collision ruled
    // out by construction, mechanism NOT established. Do not "fix" it with a retry
    // and do not serialize the gates around it (the operator refused that class of
    // remedy). The capture below IS the experiment the next occurrence needs.
    // ✔MEASURED 2026-08-10: a one-shot NATIVE-PROBE-COMPILE-FAILED in a parallel
    // ctest run (`core/test_packed_abi_conformance`, which then passed alone in two
    // shells) left NOTHING to diagnose — the detail named which command failed and
    // the compiler's own explanation had gone to a discarded stream. Every other
    // status in this file distinguishes what broke; this one knew and threw it away.
    // The `build.empty()` test stays FIRST so `captureCmd` is never handed an empty
    // command.
    //
    // ★★ AN MSVC BUILD RUNS UNDER THE PROCESS'S ONE DEVELOPER ENVIRONMENT, entered here
    // by the first probe that builds and handed as-is to every later one — the same
    // command, captured the same way, through `systemUnder` instead of `std::system`.
    // An environment that could not be entered answers with ITS status, not
    // `CompileFailed`: no compiler ran, so none failed.
    fs::path const buildLog = work / "build_output.txt";
    int buildRc = -1;
    if (!build.empty()) {
        if (loc.compiler->msvc) {
            MsvcEnvironment const vs = msvcEnvironment(*loc.compiler->msvc, work);
            if (!vs.ok()) {
                r.status = vs.status;
                r.detail = vs.detail;
                return r;
            }
            buildRc = systemUnder(vs, captureCmd(build, buildLog));
        } else {
            buildRc = std::system(captureCmd(build, buildLog).c_str());
        }
    }
    if (build.empty() || buildRc != 0) {
        r.status = ProbeStatus::CompileFailed;
        r.detail = "build command `" + build + "`"
                 + (build.empty() ? std::string{} : tailOf(buildLog, 30));
        return r;
    }
    if (!fileExists(exe)) {
        r.status = ProbeStatus::ExecutableMissing;
        r.detail = "build reported success but `" + exe.string() + "` does not exist";
        return r;
    }

    fs::path const out = work / "probe_out.txt";
    std::string const run = redirectCmd(exe, out);
    if (std::system(run.c_str()) != 0) {
        r.status = ProbeStatus::RunFailed;
        r.detail = "run command `" + run + "` (shell-quoting mismatch lands here)";
        return r;
    }

    std::ifstream in{out};
    if (!in) {
        r.status = ProbeStatus::OutputUnreadable;
        r.detail = "captured stdout `" + out.string() + "` could not be opened";
        return r;
    }
    for (std::string line; std::getline(in, line);) r.lines.push_back(line);
    if (r.lines.empty()) {
        r.status = ProbeStatus::EmptyOutput;
        r.detail = "the probe ran and exited 0 but printed nothing";
        return r;
    }

    r.status = ProbeStatus::Ok;
    return r;
}

// The anti-recurrence guard for the whole class.
//
// The defect above was never that a comparison DISAGREED — it was that no comparison
// ever RAN, and on exit code alone a battery of zero rows is indistinguishable from a
// battery that passed every row. This ledger makes that difference observable: it
// tallies the comparisons a test body actually reached and FAILS at scope exit if the
// tally came in under the floor. A battery that goes inert again shows up as
// `executed 0 of at least 11`, not as a pass.
//
// DERIVE THE FLOOR FROM THE BATTERY (`bs.size()`, a count of the bit-field entries),
// never from a typed-in constant. A hand-typed floor drifts away from the battery it
// is supposed to be guarding and quietly stops being a floor at all.
//
// CONSTRUCT IT AFTER EVERY EARLY EXIT — after the tool-absent skip and after the
// probe-failed assert. A ledger that outlives a legitimate skip would report zero
// rows and convert that skip into a spurious red.
class ExecutedRows {
public:
    ExecutedRows(std::string_view what, std::size_t floorN)
        : what_(what), floor_(floorN) {
        // A ZERO floor is a guard that guards nothing: `n_ >= 0` always holds, so the
        // ledger below can never fire and the battery it certifies is free to go
        // inert again — silently, which is the one failure mode this class exists to
        // make loud. Floors are derived from the battery (`bs.size()` and friends), so
        // a zero arrives exactly when someone empties a battery, and THAT is the case
        // that must not pass quietly. Fail here rather than trust the caller.
        if (floor_ != 0) return;
        ADD_FAILURE()
            << "DECORATIVE GUARD (" << what_ << "): the inert-oracle floor is 0, so "
               "this ledger can never fail and certifies nothing. A floor is derived "
               "from the battery it guards; a zero means the battery is EMPTY — which "
               "is the inert oracle itself, not a configuration.";
    }

    ~ExecutedRows() {
        // Never add a failure while an exception is in flight. A destructor is
        // implicitly `noexcept`, and under `--gtest_throw_on_failure` (or any throw
        // out of `computeLayout`/the interner inside the loop) this one runs DURING
        // unwinding with the ledger still short of its floor — so `ADD_FAILURE()`
        // would throw a second exception from a noexcept dtor and `std::terminate`.
        //
        // MEASURED, macOS/libc++, one EXPECT_EQ forced to fail mid-battery under
        // `--gtest_throw_on_failure`: WITHOUT this line the process aborts reporting
        // `native_c_probe.hpp` / "INERT ORACLE … executed 0 of at least 21" as the
        // terminating exception — the corroboration DISPLACES the cause. WITH it, the
        // real `EXPECT_EQ` failure is what gets reported. (Both runs still exit 134
        // under that flag, because gtest lets an uncaught test exception escape
        // `main`; what this line buys is which failure the log names, and not
        // terminating from a destructor. The repo's ctest does not pass the flag.)
        if (std::uncaught_exceptions() > 0) return;
        if (n_ >= floor_) return;
        ADD_FAILURE()
            << "INERT ORACLE (" << what_ << "): executed " << n_
            << " of at least " << floor_ << " expected comparisons. This battery "
               "compares dss against a freshly-measured host compiler; a shortfall "
               "means the oracle stopped short or never ran at all, and a run that "
               "compares nothing must never read as a pass. If another failure is "
               "reported above, that one is the cause and this line is its "
               "corroboration.";
    }

    ExecutedRows(ExecutedRows const&)            = delete;
    ExecutedRows& operator=(ExecutedRows const&) = delete;
    ExecutedRows(ExecutedRows&&)                 = delete;
    ExecutedRows& operator=(ExecutedRows&&)      = delete;

    // Tally one comparison that ACTUALLY reached a native-vs-dss assertion. Call it
    // next to the assertion, never at the top of the loop body — a counter bumped
    // before the work it certifies counts intentions, not comparisons.
    void record() noexcept { ++n_; }

    [[nodiscard]] std::size_t count() const noexcept { return n_; }

private:
    std::string what_;
    std::size_t floor_ = 0;
    std::size_t n_     = 0;
};

}  // namespace dss::test_support::native_probe
