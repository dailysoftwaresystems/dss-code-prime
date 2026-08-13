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
// D-TEST-NATIVE-ORACLE-INERT-ON-POSIX — a native oracle that skips on error is a broken oracle that reports success.
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

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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
// [D-TEST-NATIVE-PROBE-COMPILE-FAILURE-DISCARDS-ITS-OWN-OUTPUT.]
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

// The tail of a captured log, folded into a failure detail.
//
// ★ THE THREE OUTCOMES ARE KEPT APART — unreadable, empty, and present are three
//   different facts about a failed build, and collapsing them into "" would put the
//   reader back where this whole file started.
[[nodiscard]] inline std::string tailOf(fs::path const& p, std::size_t maxLines) {
    std::ifstream in{p};
    if (!in) {
        return "\n  (no build output: the capture log `" + p.string()
             + "` could not be opened, so WHY it failed is not known here)";
    }
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    if (lines.empty()) return "\n  (the build command produced NO output at all)";
    std::size_t const from = lines.size() > maxLines ? lines.size() - maxLines : 0;
    std::string out = "\n  --- build output, last " + std::to_string(lines.size() - from)
                    + " of " + std::to_string(lines.size()) + " line(s) ---";
    for (std::size_t i = from; i < lines.size(); ++i) out += "\n  " + lines[i];
    return out;
}

// A located host C compiler: its family, plus a shell-ready command that builds
// `<src>` into `<exe>`.
struct Compiler {
    Toolchain kind = Toolchain::Unix;
    std::function<std::string(fs::path const&, fs::path const&)> buildCmd;
};

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

// Locate a cl.exe/lib.exe toolchain via vswhere -> vcvars64, and say WHY when that
// fails. THE single implementation — `findCompiler` below and the four native COFF
// witnesses in tests/link/test_coff_object_reader.cpp all call THIS.
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
[[nodiscard]] inline MsvcLocation locateMsvcToolchain(fs::path const& work) {
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

// Where a host C compiler was found, or WHY it was not. The status is the whole point:
// `findCompiler` returning a bare `std::optional` is what let a BROKEN lookup read as
// an ABSENT one at the call site.
struct CompilerLocation {
    ProbeStatus             status = ProbeStatus::ToolAbsent;
    std::string             detail;
    std::optional<Compiler> compiler;  // set iff `status == Ok`
};

// Locate a host C compiler. On Windows this wraps cl.exe in a generated vcvars64 batch
// (cl needs INCLUDE/LIB); on Unix it uses cc/clang/gcc directly, no environment setup.
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
    fs::path const vcvars = msvc.vcvars;
    Compiler c;
    c.kind = Toolchain::Msvc;
    c.buildCmd = [vcvars, work](fs::path const& src, fs::path const& exe) -> std::string {
        // Generate a build batch: call vcvars64, then cl. Quote everything.
        fs::path const bat = work / "build_probe.bat";
        std::ofstream b{bat};
        // ★ `cl` IS NO LONGER SILENCED. [D-TEST-NATIVE-PROBE-COMPILE-FAILURE-DISCARDS-ITS-OWN-OUTPUT.]
        // `>nul 2>&1` here threw the compiler's diagnostics away INSIDE the batch,
        // so the caller's capture — added in the same pass — collected an empty log
        // and could only report "the build command produced NO output at all". ✔That
        // is exactly what it reported when the arm was first exercised, which is how
        // this line was found: the capture was right and was pointed at a stream
        // already emptied one level down.
        // `call vcvars` KEEPS its silencer: on success it prints a banner that is
        // pure noise, and its failure mode is already covered upstream — this batch
        // is only written after `locateMsvcToolchain` has validated the path.
        // ★★★ `cd /d <work>` IS THE FIX FOR [D-TEST-NATIVE-PROBE-COMPILE-FAILS-
        // UNDER-CONCURRENT-LOAD], AND IT IS A ROOT CAUSE, NOT A RETRY.
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
          << "call \"" << vcvars.string() << "\" >nul 2>&1\r\n"
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
    // ★ CAPTURE THE BUILD'S OWN OUTPUT. [D-TEST-NATIVE-PROBE-COMPILE-FAILURE-DISCARDS-ITS-OWN-OUTPUT.]
    // ⚠ AND THIS ARM HAS AN OPEN, UNEXPLAINED OCCURRENCE BEHIND IT:
    // [D-TEST-NATIVE-PROBE-COMPILE-FAILS-UNDER-CONCURRENT-LOAD] — one non-reproducing
    // failure under a concurrently-running second gate, scratch-dir collision ruled
    // out by construction, mechanism NOT established. Do not "fix" it with a retry
    // and do not serialize the gates around it (the operator refused that class of
    // remedy). The capture below IS the experiment the next occurrence needs.
    // ✔MEASURED 2026-08-10: a one-shot NATIVE-PROBE-COMPILE-FAILED in a parallel
    // ctest run (`core/test_packed_abi_conformance`, which then passed alone in two
    // shells) left NOTHING to diagnose — the detail named which command failed and
    // the compiler's own explanation had gone to a discarded stream. Every other
    // status in this file distinguishes what broke; this one knew and threw it away.
    // The `build.empty()` test stays FIRST so `captureCmd` is never handed an empty
    // command — `||` short-circuits, exactly as before.
    fs::path const buildLog = work / "build_output.txt";
    if (build.empty()
        || std::system(captureCmd(build, buildLog).c_str()) != 0) {
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
