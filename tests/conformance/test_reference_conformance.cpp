// ════════════════════════════════════════════════════════════════════════════
//  THE DIFFERENTIAL ACCEPT/REJECT ORACLE — D-CONF-REFERENCE-DIFFERENTIAL-ORACLE
// ════════════════════════════════════════════════════════════════════════════
//
// The operator's standing rule: "WE MUST SUPPORT WHAT REFERENCE COMPILERS DO
// SUPPORT FOR C Language, that's it." It is BIDIRECTIONAL and both directions
// are defects:
//
//   Direction A — a reference compiler ACCEPTS and DSS REJECTS. A conformance
//                 gap; real corpora fail to build.
//   Direction B — DSS ACCEPTS and NO reference compiler accepts. An INVENTED
//                 extension; code written for DSS builds nowhere else.
//
// Direction B had never been checked. The operator found an instance BY EYE
// (`extern int _fmode "msvcrt.dll";`) and said: "I DON'T WANT TO HAVE TO BABYSIT
// YOU EVERY TIME, this type of syntax failure is not acceptable." So the
// deliverable is not a fix to that instance — it is this mechanism, which
// compiles a probe corpus with DSS *and* with every reference compiler present on
// the host, DIFFS the accept/reject verdicts, and fails loud in either direction.
//
// ── WHAT IS PRECEDENT AND WHAT IS NEW ──────────────────────────────────────
//
// `tests/core/native_c_probe.hpp` already knows how to find the host C compiler,
// quote a build command for the host shell, capture the compiler's own output,
// and tell "no toolchain installed" apart from "a step broke". This file INCLUDES
// it and reuses `locateMsvcToolchain`, `captureCmd`, `tailOf`, `fileExists` and
// `ExecutedRows` rather than growing a second copy — the whole docblock at the
// top of that header is about what happened the last time this repo had two.
//
// What is genuinely new here, and where the care has to go:
//
//   1. It needs MANY compilers, not one. The precedent asks "is there a host C
//      compiler"; this asks "which reference compilers exist, of which family,
//      and at which measured standard level" — because a verdict is only as
//      honest as the identity attached to it.
//   2. It runs each oracle as ONE BATCH. A per-probe cl.exe invocation would pay
//      the vcvars64 cost 76 times; one generated script per oracle pays it once
//      and prints `@@PROBE <id> RC=<n>` after each compile, with `%ERRORLEVEL%` /
//      `$?` read DIRECTLY on the following line and never after a pipe.
//   3. It drives the REAL DSS CLI as a subprocess (`DSS_CLI_PATH`, baked from
//      `$<TARGET_FILE:dss-code-prime>`), not the library in-process. A pin on
//      "what DSS accepts" that bypasses argv, the config resolver and the
//      pipeline is not a pin on the thing users run.
//
// ── ORACLE HONESTY, WHICH IS THE PART MOST LIKELY TO GO WRONG ──────────────
//
// ★ A STANDARD LEVEL IS MEASURED, NEVER ASSUMED. Every oracle's level comes from
//   compiling a `#pragma message("DSS_STDC=" ...)` probe under the candidate flag
//   and reading back `__STDC_VERSION__`. Nothing labels a compiler "C23" because
//   a flag was accepted.
//
// ✔MEASURED, and it is exactly why: `cl /std:c23` emits `Command line warning
//   D9002: ignoring unknown option '/std:c23'`, EXITS ZERO, and compiles in the
//   default mode where `__STDC_VERSION__` is not defined at all. A discovery step
//   that trusted the exit code would have labelled cl 19.51 a C23 oracle while it
//   was compiling pre-C99. The `#if defined(__STDC_VERSION__)` guard in the
//   version probe is what makes that case fall through: the marker prints `none`,
//   the flag is discarded, and cl is labelled by `/std:clatest` → 202312L.
//
// ✔MEASURED, the same trap on the other side: WSL gcc is 13.3.0, which REJECTS
//   `-std=c23` and reports 202000L under `-std=c2x`. It is a C2x-DRAFT oracle. A
//   probe that declares `@min-stdc 202311` is therefore NOT judged by it — by
//   MEASUREMENT, with no per-compiler special case anywhere in this file.
//
// ✔MEASURED, and the reason discovery probes the FILESYSTEM rather than PATH:
//   this host has `/usr/bin/clang-19` (Ubuntu clang 19.1.1, `-std=c23` →
//   202311L, a REAL C23 oracle) and `/usr/bin/clang-18`, while `command -v clang`
//   finds NOTHING because there is no unversioned symlink. The repo already has
//   this lesson recorded for macOS shims; it applies to versioned Linux packages
//   just as hard. Discovery therefore stats explicit paths and then proves each
//   candidate by RUNNING it.
//
// ── AND AN ABSENT ORACLE MUST NEVER READ LIKE A PASS ───────────────────────
//
// `D-TEST-NATIVE-ORACLE-INERT-ON-POSIX` is on record in this repo: an oracle that
// no-ops where its compiler is absent is indistinguishable from one that agreed.
// So: every candidate that is not used is reported BY NAME WITH A REASON; the
// corpus carries a SANITY PAIR (one trivially-valid and one trivially-invalid
// probe) that every oracle must get right or be declared BROKEN and red; and
// every enumeration dimension carries a floor, so a collapsed corpus scan or a
// silent driver failure cannot come back green.
//
// ── WHY DSS IS RUN FOR TWO FIXED TARGETS ───────────────────────────────────
//
// Not the host-native target. DSS builds any target on any host, and a
// FRONT-END conformance census must not change its answer depending on which
// machine ran it. So every probe is compiled for a fixed pe64 AND a fixed elf64
// RELOCATABLE-OBJECT format, and "DSS accepts" means accepted under at least one.
// Two things fall out of that, both wanted:
//   * target-gated behaviour is separated from conformance. `int WINAPI f(void);`
//     is admitted for pe (WINAPI is a format-gated predefined macro) and refused
//     for elf — "accepted for some target" is what makes it an invented construct
//     rather than a per-target detail.
//   * the relocatable-object formats are the honest counterpart of the `-c` / `/c`
//     the reference compilers are held to: no entry point, no link step, so a
//     declaration-only probe is a complete translation unit on both sides.

#include "native_c_probe.hpp"   // locateMsvcToolchain, captureCmd, tailOf, ExecutedRows
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace native_probe = dss::test_support::native_probe;

#if !defined(DSS_CLI_PATH)
#  error "DSS_CLI_PATH must be baked from $<TARGET_FILE:dss-code-prime> — see tests/conformance/CMakeLists.txt"
#endif

namespace {

// ─────────────────────────────────────────────────────────────────────────────
//  THE CORPUS
// ─────────────────────────────────────────────────────────────────────────────

enum class Direction : std::uint8_t { A, B };
enum class Mode : std::uint8_t { Strict, Extended };
enum class ExpectRef : std::uint8_t { Accept, Reject, Varies };
enum class Family : std::uint8_t { Gnu, Msvc };

[[nodiscard]] char const* familyName(Family f) {
    return f == Family::Gnu ? "gnu" : "msvc";
}

struct Probe {
    std::string        id;
    Direction          direction = Direction::A;
    Mode               mode      = Mode::Strict;
    long               minStdc   = 0;
    std::set<Family>   families;
    ExpectRef          expectRef = ExpectRef::Accept;
    std::string        variesNote;
    std::string        acknowledgedGap;   // empty = NOT acknowledged
    std::string        why;
    std::string        code;
    [[nodiscard]] bool acknowledged() const { return !acknowledgedGap.empty(); }
};

// A corpus parse either yields probes or says exactly which line broke. There is
// no third outcome: a silently-empty parse is the collapsed-scan failure this
// whole file is guarding against, so "no probes" is reported as an error string
// and the floors below turn it into a red.
struct CorpusParse {
    std::vector<Probe> probes;
    std::string        error;
};

[[nodiscard]] std::string trim(std::string s) {
    auto const notSpace = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r'; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

[[nodiscard]] CorpusParse parseCorpus(fs::path const& file) {
    CorpusParse out;
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        out.error = "the probe corpus `" + file.string() + "` could not be opened";
        return out;
    }

    // `seen` is what makes a duplicated id an ERROR rather than a silent
    // last-one-wins: two rows under one name would make the required-id floor
    // satisfiable by a probe that is not the one it names.
    std::set<std::string>       seen;
    std::optional<Probe>        cur;
    std::vector<std::string>    codeLines;
    bool                        inCode = false;
    std::size_t                 lineNo = 0;

    auto const fail = [&](std::string const& msg) {
        out.error = file.filename().string() + ":" + std::to_string(lineNo) + ": " + msg;
    };

    for (std::string raw; std::getline(in, raw);) {
        ++lineNo;
        std::string const line = trim(raw);
        if (inCode) {
            if (line == "@end") {
                if (codeLines.empty()) { fail("probe `" + cur->id + "` has an EMPTY @code block"); return out; }
                std::string body;
                for (auto const& l : codeLines) body += l + "\n";
                cur->code = body;
                out.probes.push_back(*cur);
                cur.reset();
                codeLines.clear();
                inCode = false;
            } else {
                codeLines.push_back(raw);
            }
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        if (line == "@code") {
            if (!cur) { fail("@code with no open @probe"); return out; }
            inCode = true;
            continue;
        }
        if (line[0] != '@') { fail("expected a `@key value` line, got `" + line + "`"); return out; }

        auto const sp  = line.find(' ');
        std::string const key = line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
        std::string const val = sp == std::string::npos ? std::string{} : trim(line.substr(sp + 1));

        if (key == "probe") {
            if (cur) { fail("@probe `" + val + "` opened while `" + cur->id + "` is still open"); return out; }
            if (val.empty()) { fail("@probe with no id"); return out; }
            if (!seen.insert(val).second) { fail("DUPLICATE probe id `" + val + "`"); return out; }
            Probe p;
            p.id = val;
            cur  = p;
            continue;
        }
        if (!cur) { fail("key `@" + key + "` before any @probe"); return out; }
        if (val.empty()) { fail("key `@" + key + "` on probe `" + cur->id + "` has an EMPTY value"); return out; }

        if (key == "direction") {
            if (val == "A") cur->direction = Direction::A;
            else if (val == "B") cur->direction = Direction::B;
            else { fail("@direction must be A or B, got `" + val + "`"); return out; }
        } else if (key == "mode") {
            if (val == "strict") cur->mode = Mode::Strict;
            else if (val == "extended") cur->mode = Mode::Extended;
            else { fail("@mode must be strict or extended, got `" + val + "`"); return out; }
        } else if (key == "min-stdc") {
            try { cur->minStdc = std::stol(val); }
            catch (...) { fail("@min-stdc is not a number: `" + val + "`"); return out; }
        } else if (key == "families") {
            std::istringstream fs2{val};
            for (std::string f; std::getline(fs2, f, ',');) {
                f = trim(f);
                if (f == "gnu") cur->families.insert(Family::Gnu);
                else if (f == "msvc") cur->families.insert(Family::Msvc);
                else { fail("@families entry must be gnu or msvc, got `" + f + "`"); return out; }
            }
            if (cur->families.empty()) { fail("@families is empty"); return out; }
        } else if (key == "expect-ref") {
            if (val == "accept") cur->expectRef = ExpectRef::Accept;
            else if (val == "reject") cur->expectRef = ExpectRef::Reject;
            else if (val == "varies") cur->expectRef = ExpectRef::Varies;
            else { fail("@expect-ref must be accept|reject|varies, got `" + val + "`"); return out; }
        } else if (key == "varies-note") {
            cur->variesNote = val;
        } else if (key == "acknowledged-gap") {
            cur->acknowledgedGap = val;
        } else if (key == "why") {
            cur->why = val;
        } else {
            // The descriptor rule, applied here: an unknown key FAILS LOUD rather
            // than being ignored. A silently-dropped key is a declaration the
            // author believes is in force and that nothing reads.
            fail("UNKNOWN key `@" + key + "` on probe `" + cur->id
                 + "` — the allowed keys are probe/direction/mode/min-stdc/families/"
                   "expect-ref/varies-note/acknowledged-gap/why/code");
            return out;
        }
    }
    if (inCode || cur) {
        out.error = file.filename().string() + ": probe `" + (cur ? cur->id : std::string{"?"})
                  + "` is unterminated (missing @end)";
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SHELLS — how a generated script is written and launched
// ─────────────────────────────────────────────────────────────────────────────
//
// Three, and they are not interchangeable. `native_c_probe.hpp` documents the
// Windows/POSIX quoting split at length; the third one here is the WSL bridge,
// which adds two failure modes of its own and both are SILENT if got wrong:
//   * a script written in text mode on Windows gets CRLF line endings, and bash
//     then fails on `\r` in ways that look like the compiler rejecting the probe.
//     Every script is therefore written in BINARY mode with explicit newlines.
//   * paths must be re-spelled `C:\x` → `/mnt/c/x` for the distro. That
//     translation is never trusted: the discovery batch compiles a trivial file
//     through the same translation, so a wrong mapping makes the oracle report
//     ABSENT-with-reason instead of mis-verdicting every probe.
enum class ShellKind : std::uint8_t { WinCmd, PosixSh, WslSh };

[[nodiscard]] char const* shellSuffix(ShellKind k) {
    return k == ShellKind::WinCmd ? ".bat" : ".sh";
}

[[nodiscard]] std::string shellPath(ShellKind kind, fs::path const& p) {
    std::string s = p.string();
    if (kind != ShellKind::WslSh) return s;
    for (auto& c : s) if (c == '\\') c = '/';
    if (s.size() > 1 && s[1] == ':') {
        std::string drive(1, static_cast<char>(std::tolower(static_cast<unsigned char>(s[0]))));
        s = "/mnt/" + drive + s.substr(2);
    }
    return s;
}

[[nodiscard]] std::string quoted(std::string const& s) { return "\"" + s + "\""; }

// One step's verdict: was its marker published, what was the exit code, and what
// did the command itself print. The output comes from that step's OWN file, never
// from a position within a shared stream -- see the Step docblock.
struct StepOutcome {
    bool        present  = false;   // the marker line was found at all
    bool        accepted = false;   // RC=0
    int         rc       = -1;
    std::string output;
};

// Scan the marker stream. A marker is matched ANYWHERE in a line, not only at its
// start: a shell that puts a stray byte in front of it must not cost the step its
// verdict, and a lost verdict is precisely what this file must never read as
// agreement.
[[nodiscard]] std::map<std::string, StepOutcome> parseScriptLog(std::string const& log) {
    std::map<std::string, StepOutcome> out;
    std::istringstream in{log};
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto const at = line.find("@@PROBE ");
        if (at == std::string::npos) continue;
        std::istringstream ls{line.substr(at + 8)};
        std::string marker, rcTok;
        ls >> marker >> rcTok;
        auto const eq = rcTok.find("RC=");
        if (marker.empty() || eq == std::string::npos) continue;
        StepOutcome o;
        o.present = true;
        try { o.rc = std::stoi(rcTok.substr(eq + 3)); } catch (...) { o.rc = -1; }
        o.accepted = (o.rc == 0);
        out[marker] = o;
    }
    return out;
}

// One command, the marker that publishes its exit code, and the file its OWN
// output goes to. `marker` is empty for setup lines (`call vcvars`) that have no
// verdict of their own.
//
// * WHY EVERY STEP GETS ITS OWN OUTPUT FILE, and why "everything printed before a
// marker belongs to that marker" is NOT good enough.
//
// MEASURED, and it silently cost this harness both of its C23-capable gnu
// oracles: with one shared capture file, the shell's marker lines (stdout) and the
// compiler's diagnostics (stderr) interleave in an order NOBODY controls once they
// cross the WSL boundary. The captured log contained
//
//     ...warning: DSS_STDC=202400L [-W#pragm@@PROBE STD_c2y RC=0
//
// -- a marker line spliced into the middle of a diagnostic -- with the five `STD_*`
// markers hoisted above output produced before them and the `IDENT` marker
// destroyed outright. The harness then reported `produced no @@PROBE IDENT
// marker`, dropped clang 19.1.1 and clang 18.1.3 from the roster, and let the C23
// probes fall to MSVC alone -- which manufactured TWO FALSE direction-B rows.
//
// So ordering is no longer relied on at all. The marker line carries ONLY the exit
// code; each command's own output is redirected to its own file and read back by
// name. `cmd > file 2>&1` followed by `$?` still reports CMD's status -- a
// redirection is not a pipe -- so the "rc DIRECTLY, never after a pipe" rule holds.
struct Step {
    std::string command;
    std::string marker;
    fs::path    outFile;   // this step's own stdout+stderr; empty for setup lines
};

// Render the steps into a script, run it with BOTH streams captured, and return
// the captured text. `ok` is false only when the script itself could not be
// written or launched — an individual probe's nonzero exit is DATA, not an error.
struct ScriptRun {
    bool        launched = false;
    std::string log;              // the marker stream ONLY
    std::string diagnostic;
    std::string scriptPath;
    std::map<std::string, StepOutcome> outcomes;   // marker -> verdict + its output
    // ★ THE LOG PATH TRAVELS WITH THE RESULT. [D-TEST-NATIVE-PROBE-COMPILE-FAILURE-
    // DISCARDS-ITS-OWN-OUTPUT, same shape.] A driver that launched and then
    // produced no marker has an explanation sitting in its capture file, and the
    // first version of this file threw it away: the failure read `produced no
    // @@PROBE IDENT marker ... the shell could not run it`, which named a guess
    // instead of the reason. Every caller that reports a failure must fold in
    // `tailOf(logPath, N)`.
    fs::path    logPath;
};

[[nodiscard]] ScriptRun runScript(ShellKind kind, fs::path const& scriptFile,
                                  fs::path const& logFile,
                                  std::vector<Step> const& steps) {
    ScriptRun r;
    r.scriptPath = scriptFile.string();
    r.logPath    = logFile;
    {
        std::ofstream o{scriptFile, std::ios::binary | std::ios::trunc};
        if (!o) {
            r.diagnostic = "could not write the driver script `" + scriptFile.string() + "`";
            return r;
        }
        char const* nl = (kind == ShellKind::WinCmd) ? "\r\n" : "\n";
        if (kind == ShellKind::WinCmd) o << "@echo off" << nl;
        else                           o << "#!/bin/sh" << nl;
        for (auto const& s : steps) {
            o << s.command;
            // The redirect belongs to the SAME simple command, so `$?` /
            // `%ERRORLEVEL%` on the next line is still THIS command's status.
            if (!s.outFile.empty())
                o << " > " << quoted(shellPath(kind, s.outFile)) << " 2>&1";
            o << nl;
            if (s.marker.empty()) continue;
            // ★ THE EXIT CODE IS READ ON THE VERY NEXT LINE, never after a pipe.
            // On cmd.exe `%ERRORLEVEL%` expands per-line at execution time, which
            // is correct here precisely because these lines are at the top level
            // of the batch and not inside a parenthesised block.
            if (kind == ShellKind::WinCmd)
                o << "echo @@PROBE " << s.marker << " RC=%ERRORLEVEL%" << nl;
            else
                o << "echo \"@@PROBE " << s.marker << " RC=$?\"" << nl;
        }
        // A trailing success so the SCRIPT's own status never masquerades as the
        // last probe's verdict (which is already published in its marker line).
        if (kind == ShellKind::WinCmd) o << "exit /b 0" << nl;
        else                           o << "exit 0" << nl;
    }

    // ── HOW THE SCRIPT IS LAUNCHED, AND WHY THE WSL ARM GETS A .bat WRAPPER ──
    //
    // ✔MEASURED, and this cost the WSL oracles their first run: `captureCmd`'s
    // Windows form wraps the whole command in one more pair of quotes, which is
    // CORRECT for a command that already BEGINS with a quoted program path (the
    // shape native_c_probe.hpp built it for) and WRONG for one that begins with a
    // bare program name. `"wsl.exe -e bash "/mnt/..." > "log" 2>&1"` makes cmd.exe
    // read the leading quoted run as the program:
    //     '"wsl.exe -e bash "' is not recognized as an internal or external command
    // The symptom in the harness was NOT a diagnostic about quoting — it was
    // `wsl-clang-19: its discovery driver produced no @@PROBE IDENT marker`, i.e.
    // both C23-capable gnu oracles silently dropped off the roster and the C23
    // probes fell to MSVC alone, which manufactured two FALSE direction-B rows.
    // (It reddened rather than passing, because the corpus declares
    // `@expect-ref accept` on those probes — but the message named the wrong thing.)
    //
    // So the WSL launch goes through a one-line .bat, which is the ONE shape this
    // repo has already proven against cmd.exe's quoting (see `findCompiler`'s MSVC
    // arm writing `build_probe.bat`). No second quoting theory to maintain.
    std::string launch;
    switch (kind) {
        case ShellKind::WinCmd:  launch = quoted(scriptFile.string()); break;
        case ShellKind::PosixSh: launch = "sh " + quoted(scriptFile.string()); break;
        case ShellKind::WslSh: {
            fs::path const wrapper = scriptFile.parent_path()
                                   / (scriptFile.stem().string() + "_wsl.bat");
            std::ofstream w{wrapper, std::ios::binary | std::ios::trunc};
            if (!w) {
                r.diagnostic = "could not write the WSL launcher `" + wrapper.string() + "`";
                return r;
            }
            w << "@echo off\r\n"
              << "wsl.exe -e bash \"" << shellPath(ShellKind::WslSh, scriptFile) << "\"\r\n";
            w.close();
            launch = quoted(wrapper.string());
            break;
        }
    }
    // rc is captured DIRECTLY from std::system; it is only used to explain a
    // launch failure, because every per-probe verdict travels in a marker line.
    int const status = std::system(native_probe::captureCmd(launch, logFile).c_str());
    std::ifstream in{logFile, std::ios::binary};
    if (!in) {
        r.diagnostic = "the driver ran (status " + std::to_string(status)
                     + ") but its capture log `" + logFile.string() + "` could not be opened";
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    r.log      = ss.str();
    r.logPath  = logFile;
    r.launched = true;
    r.outcomes = parseScriptLog(r.log);
    // Fold each step's OWN output in, BY NAME. A step whose capture file cannot be
    // read keeps its verdict and says so: "the command printed nothing" and "we
    // could not read what it printed" are different facts, and merging them is the
    // defect `tailOf` in native_c_probe.hpp exists to avoid.
    for (auto const& st : steps) {
        if (st.marker.empty() || st.outFile.empty()) continue;
        auto const it = r.outcomes.find(st.marker);
        if (it == r.outcomes.end()) continue;
        std::ifstream so{st.outFile, std::ios::binary};
        if (!so) {
            it->second.output = "(this step's capture file `" + st.outFile.string()
                              + "` could not be opened, so WHY is not known here)";
            continue;
        }
        std::ostringstream sos;
        sos << so.rdbuf();
        it->second.output = sos.str();
    }
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ORACLES
// ─────────────────────────────────────────────────────────────────────────────

// The probe whose ONLY job is to make a compiler state its own standard level.
// The `#if defined` guard is load-bearing: see the cl `/std:c23` measurement in
// the file docblock.
constexpr char const* kVersionProbe =
    "#define DSS_STDC_S2(x) #x\n"
    "#define DSS_STDC_S(x) DSS_STDC_S2(x)\n"
    "#if defined(__STDC_VERSION__)\n"
    "#pragma message(\"DSS_STDC=\" DSS_STDC_S(__STDC_VERSION__))\n"
    "#else\n"
    "#pragma message(\"DSS_STDC=none\")\n"
    "#endif\n"
    "int dss_version_probe_object;\n";

struct Oracle {
    std::string id;
    Family      family      = Family::Gnu;
    ShellKind   shell       = ShellKind::PosixSh;
    std::string exe;           // as spelled INSIDE the script
    std::string setupCommand;  // e.g. `call "vcvars64.bat" >nul 2>&1`; may be empty
    std::string ident;         // the compiler's own identification line
    std::string stdFlag;       // the flag that produced `stdcVersion`
    long        stdcVersion = 0;

    // The command that compiles `src` to `obj` under `mode` with `flag`.
    [[nodiscard]] std::string compileCommand(fs::path const& src, fs::path const& obj,
                                             Mode mode, std::string const& flag) const {
        std::string const s = quoted(shellPath(shell, src));
        std::string const o = shellPath(shell, obj);
        if (family == Family::Msvc) {
            // MSVC HAS NO ISO-STRICT MODE. `/Za` is not usable with /std:c11 or
            // later, so `strict` and `extended` are the SAME command line for this
            // family. Stated in the corpus header and again here rather than left
            // for a reader to infer from a missing branch.
            return quoted(exe) + " /nologo /std:" + flag + " /c " + s + " /Fo" + quoted(o);
        }
        std::string std_ = flag;
        std::string pedantic;
        if (mode == Mode::Strict) pedantic = " -pedantic-errors";
        else if (!flag.empty() && flag[0] == 'c') std_ = "gnu" + flag.substr(1);
        return quoted(exe) + " -std=" + std_ + pedantic + " -c " + s + " -o " + quoted(o);
    }

    [[nodiscard]] std::string commandShape() const {
        return family == Family::Msvc
                   ? "cl /nologo /std:" + stdFlag + " /c   (no ISO-strict arm exists)"
                   : "-std=" + stdFlag + " -pedantic-errors -c   |   -std=gnu"
                         + (stdFlag.size() > 1 ? stdFlag.substr(1) : stdFlag) + " -c";
    }
};

struct AbsentOracle {
    std::string name;
    std::string reason;
};

struct OracleSet {
    std::vector<Oracle>       found;
    std::vector<AbsentOracle> absent;
    // A HARNESS defect, as opposed to an absent tool. Kept apart for the exact
    // reason native_c_probe.hpp keeps ProbeStatus apart from ToolAbsent: "absent"
    // is an answer, a step that broke is not.
    std::vector<std::string>  broken;
};

// A candidate before it has proven itself. Nothing reaches `OracleSet::found`
// without having compiled the version probe.
struct Candidate {
    std::string              id;
    Family                   family = Family::Gnu;
    ShellKind                shell  = ShellKind::PosixSh;
    std::string              exe;
    std::string              setupCommand;
    std::string              identCommand;   // prints the compiler's identity
    std::vector<std::string> stdFlags;       // best-first
};

[[nodiscard]] std::vector<std::string> gnuStdFlags() {
    // Best-first. Every one of these is TRIED and only the ones that produce a
    // `__STDC_VERSION__` survive, so listing a flag a compiler does not have
    // costs one compile and never produces a false label.
    return {"c2y", "c23", "c2x", "c17", "c11"};
}

// Prove a candidate and fill in its measured identity + level. Returns the reason
// it cannot be used, or nullopt on success.
[[nodiscard]] std::optional<std::string>
proveCandidate(Candidate const& c, fs::path const& work, Oracle& out) {
    fs::path const src = work / ("verprobe_" + c.id + ".c");
    {
        std::ofstream o{src, std::ios::binary | std::ios::trunc};
        if (!o) return "could not write the version probe next to it";
        o << kVersionProbe;
    }

    Oracle probeOracle;
    probeOracle.id     = c.id;
    probeOracle.family = c.family;
    probeOracle.shell  = c.shell;
    probeOracle.exe    = c.exe;

    std::vector<Step> steps;
    if (!c.setupCommand.empty()) steps.push_back({c.setupCommand, "", {}});
    steps.push_back({c.identCommand, "IDENT", work / ("ident_" + c.id + ".txt")});
    for (auto const& flag : c.stdFlags) {
        fs::path const obj = work / ("verprobe_" + c.id + "_" + flag + ".o");
        steps.push_back({probeOracle.compileCommand(src, obj, Mode::Strict, flag),
                         "STD_" + flag,
                         work / ("verout_" + c.id + "_" + flag + ".txt")});
    }

    auto const run = runScript(c.shell, work / ("discover_" + c.id + shellSuffix(c.shell)),
                               work / ("discover_" + c.id + ".log"), steps);
    if (!run.launched) return run.diagnostic;

    auto const& outcomes = run.outcomes;
    auto const  identIt  = outcomes.find("IDENT");
    if (identIt == outcomes.end())
        return "its discovery driver produced no `@@PROBE IDENT` marker (script `"
               + run.scriptPath + "`), so the shell did not reach the first command. "
               "What it DID say:" + native_probe::tailOf(run.logPath, 25);
    // ⚠ The IDENT step's EXIT CODE is deliberately NOT the liveness test. A bare
    // `cl.exe` with no input file prints its banner and exits NONZERO, so gating on
    // it would discard the MSVC oracle on every host. Liveness is instead "at
    // least one candidate std flag made it define __STDC_VERSION__", asserted at
    // the end of this function — a compiler that cannot compile the version probe
    // is not usable regardless of what its `--version` returned.

    // The identity is the compiler's OWN first output line. Never a name this
    // file made up from the path it was found at.
    {
        std::istringstream is{identIt->second.output};
        for (std::string l; std::getline(is, l);) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            l = trim(l);
            if (!l.empty()) { probeOracle.ident = l; break; }
        }
    }
    if (probeOracle.ident.empty()) probeOracle.ident = "(identified itself with no output)";

    for (auto const& flag : c.stdFlags) {
        auto const it = outcomes.find("STD_" + flag);
        if (it == outcomes.end() || !it->second.accepted) continue;
        auto const at = it->second.output.find("DSS_STDC=");
        if (at == std::string::npos) continue;
        std::string digits;
        for (std::size_t i = at + 9; i < it->second.output.size(); ++i) {
            char const ch = it->second.output[i];
            if (ch >= '0' && ch <= '9') digits += ch;
            else break;
        }
        if (digits.empty()) continue;   // `DSS_STDC=none` — the flag was IGNORED
        probeOracle.stdFlag     = flag;
        probeOracle.stdcVersion = std::stol(digits);
        break;
    }
    if (probeOracle.stdcVersion == 0)
        return "it runs (" + probeOracle.ident + ") but NO candidate std flag made it "
               "define __STDC_VERSION__, so no standard level can be attributed to it. "
               "An unlabelled oracle is not used: a verdict without a level is a "
               "verdict this harness cannot report honestly.";

    probeOracle.setupCommand = c.setupCommand;
    out = probeOracle;
    return std::nullopt;
}

// ── CANDIDATE ENUMERATION ─────────────────────────────────────────────────
//
// ★ THE FILESYSTEM IS THE INSTRUMENT, NOT PATH. `command -v` answers a
// different question than "is there a compiler here": on macOS it resolves an
// xcrun shim that may have no toolchain behind it (recorded in
// native_c_probe.hpp), and on this very Ubuntu it MISSES clang-18 and clang-19
// because Debian-style packages ship no unversioned symlink. ✔MEASURED:
// `command -v clang` finds nothing while `/usr/bin/clang-19` is Ubuntu clang
// 19.1.1 with real `-std=c23`. So candidates are stat'd at explicit paths — and
// then every one of them still has to RUN.
//
// Versioned names are listed NEWEST-FIRST and generously: a name that is not
// installed costs one `stat` and is reported absent by name.
[[nodiscard]] std::vector<std::string> gnuVersionedNames() {
    return {"clang-21", "clang-20", "clang-19", "clang-18", "clang",
            "gcc-16", "gcc-15", "gcc-14", "gcc-13", "gcc", "cc"};
}

#if defined(_WIN32)
// Does a path exist INSIDE the WSL distro, and is it executable there? Answered
// by the distro, never guessed from the Windows side.
[[nodiscard]] bool wslHasExecutable(std::string const& p, fs::path const& work) {
    fs::path const log = work / "wslprobe.log";
    std::string const cmd = "wsl.exe -e test -x " + quoted(p);
    return std::system(native_probe::captureCmd(cmd, log).c_str()) == 0;
}
#endif

[[nodiscard]] OracleSet discoverOracles(fs::path const& work) {
    OracleSet set;
    std::vector<Candidate> candidates;

#if defined(_WIN32)
    // ── MSVC, through the ONE shared locator ──
    auto const msvc = native_probe::locateMsvcToolchain(work);
    if (msvc.ok()) {
        Candidate c;
        c.id           = "msvc-cl";
        c.family       = Family::Msvc;
        c.shell        = ShellKind::WinCmd;
        c.exe          = "cl.exe";           // vcvars64 puts it on PATH
        c.setupCommand = "call " + quoted(msvc.vcvars.string()) + " >nul 2>&1";
        // ⚠ NO PIPE. `%ERRORLEVEL%` after a pipe is the LAST command's, so
        // `cl | findstr` would publish findstr's status as cl's. The banner
        // goes into the captured log and the identity is parsed from there.
        c.identCommand = "cl.exe";
        c.stdFlags     = {"clatest", "c17", "c11"};
        candidates.push_back(c);
    } else if (msvc.toolAbsent()) {
        set.absent.push_back({"msvc-cl (cl.exe)", msvc.detail});
    } else {
        // NOT absent — a step in the locator broke. That is a defect, not a
        // machine without a compiler, and it goes red.
        set.broken.push_back(msvc.describe());
    }

    // ── Windows-native gnu-family compilers, at their usual absolute homes ──
    std::vector<std::string> const winGnu{
        "C:/Strawberry/c/bin/gcc.exe",          // ships with Strawberry Perl
        "C:/msys64/ucrt64/bin/gcc.exe",
        "C:/msys64/mingw64/bin/gcc.exe",
        "C:/msys64/clang64/bin/clang.exe",
        "C:/mingw64/bin/gcc.exe",
        "C:/ProgramData/chocolatey/bin/gcc.exe",
        "C:/Program Files/LLVM/bin/clang.exe",
    };
    for (auto const& p : winGnu) {
        if (!native_probe::fileExists(p)) {
            set.absent.push_back({p, "not on this filesystem"});
            continue;
        }
        // The id names the DISTRIBUTION, not just the binary: a host can hold a
        // Strawberry gcc and an msys64 gcc at once, and a census row that says only
        // "win-gcc" cannot tell a reader which one produced the verdict.
        std::string dist = fs::path{p}.relative_path().begin()->string();
        if (dist.empty() || dist.size() < 2) dist = "root";
        for (auto& ch : dist) if (ch == ' ') ch = '_';
        Candidate c;
        c.id           = "win-" + fs::path{p}.stem().string() + "@" + dist;
        c.family       = Family::Gnu;
        c.shell        = ShellKind::WinCmd;
        c.exe          = p;
        c.identCommand = quoted(p) + " --version";
        c.stdFlags     = gnuStdFlags();
        candidates.push_back(c);
    }

    // ── The WSL bridge. It is what turns "this host has no C23 gnu oracle" into
    // "this host has clang 19.1.1 at 202311L". It is also the arm most able to
    // fail spuriously, so it is admitted ONLY after the distro confirms the
    // binary is executable AND the candidate compiles the version probe through
    // the same /mnt path translation the real probes will use. A WSL that is
    // installed but not running therefore reports ABSENT-with-reason and is
    // listed — it never silently reduces the oracle set to nothing, because the
    // sanity pair and the floors below are what decide whether the run is
    // meaningful.
    if (native_probe::fileExists("C:/Windows/System32/wsl.exe")) {
        for (auto const& name : gnuVersionedNames()) {
            std::string const p = "/usr/bin/" + name;
            if (!wslHasExecutable(p, work)) {
                set.absent.push_back({"wsl:" + p, "not an executable inside the distro"});
                continue;
            }
            Candidate c;
            c.id           = "wsl-" + name;
            c.family       = Family::Gnu;
            c.shell        = ShellKind::WslSh;
            c.exe          = p;
            c.identCommand = quoted(p) + " --version";
            c.stdFlags     = gnuStdFlags();
            candidates.push_back(c);
        }
    } else {
        set.absent.push_back({"wsl.exe", "no WSL on this machine (C:/Windows/System32/wsl.exe absent)"});
    }
#else
    // ── POSIX hosts: Linux and macOS. Explicit directories, explicit names. ──
    std::vector<std::string> const dirs{
        "/usr/bin", "/usr/local/bin", "/opt/homebrew/bin",
        "/Library/Developer/CommandLineTools/usr/bin",
    };
    for (auto const& d : dirs) {
        for (auto const& name : gnuVersionedNames()) {
            std::string const p = std::string{d} + "/" + name;
            if (!native_probe::fileExists(p)) continue;   // absent by omission is
                                                          // reported in bulk below
            Candidate c;
            c.id           = name + "@" + fs::path{d}.filename().string();
            c.family       = Family::Gnu;
            c.shell        = ShellKind::PosixSh;
            c.exe          = p;
            c.identCommand = quoted(p) + " --version";
            c.stdFlags     = gnuStdFlags();
            candidates.push_back(c);
        }
    }
    // Report every probed NAME that was found nowhere. "REPORT what does not
    // exist" is half the honesty requirement: a roster that lists only what it
    // found does not let a reader see that clang-21 was looked for at all.
    {
        std::string tried;
        for (auto const& d : dirs) tried += (tried.empty() ? "" : ", ") + std::string{d};
        for (auto const& name : gnuVersionedNames()) {
            bool anywhere = false;
            for (auto const& d : dirs)
                if (native_probe::fileExists(std::string{d} + "/" + name)) anywhere = true;
            if (!anywhere)
                set.absent.push_back({name, "not present in any of " + tried});
        }
    }
    set.absent.push_back({"msvc-cl (cl.exe)",
                          "not a POSIX host — the msvc family cannot be oracled here, so "
                          "every probe that names ONLY that family is reported UNORACLED "
                          "rather than treated as agreement"});
#endif

    // ── Prove every candidate, and DEDUPE BY MEASURED IDENTITY ──
    //
    // `cc`, `gcc` and `gcc-13` are very often ONE binary under three names.
    // Deduping on the path cannot see that. Deduping on the raw `--version` line
    // could not either — ✔MEASURED, the first version of this dedupe admitted all
    // three of `cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`, `gcc (Ubuntu
    // 13.3.0-...) 13.3.0` and `gcc-13 (Ubuntu 13.3.0-...) 13.3.0` as separate
    // oracles, because the strings differ in exactly their FIRST token: the
    // program name. So the key drops that token.
    //
    // It is a heuristic and it is stated as one. A false MERGE costs redundancy
    // only — duplicates agree by construction — and every merge is reported by
    // name in the absent list with the identity it merged into, so a reader can
    // see what happened rather than wonder why a compiler is missing. A false SPLIT
    // is the more dangerous direction (it inflates the apparent number of
    // independent verdicts) and that is the one this key exists to close.
    auto const dedupeKey = [](Oracle const& o) {
        std::string rest = o.ident;
        auto const sp = rest.find(' ');
        rest = (sp == std::string::npos) ? std::string{} : rest.substr(sp + 1);
        return std::string{familyName(o.family)} + "|" + rest + "|" + o.stdFlag + "|"
             + std::to_string(o.stdcVersion);
    };
    std::set<std::string> identities;
    std::map<std::string, std::string> firstWithKey;   // key -> the oracle id that won
    for (auto const& c : candidates) {
        Oracle o;
        if (auto const why = proveCandidate(c, work, o)) {
            set.absent.push_back({c.id, *why});
            continue;
        }
        std::string const key = dedupeKey(o);
        if (!identities.insert(key).second) {
            set.absent.push_back({c.id, "DUPLICATE of oracle `" + firstWithKey[key]
                                        + "` — it identifies itself as `" + o.ident
                                        + "` at the same measured level, i.e. the same "
                                          "compiler under another name. Counting it twice "
                                          "would inflate the number of INDEPENDENT "
                                          "verdicts without adding one."});
            continue;
        }
        firstWithKey[key] = c.id;
        set.found.push_back(o);
    }
    return set;
}

// ─────────────────────────────────────────────────────────────────────────────
//  THE SUBJECT — the real DSS CLI, as a subprocess
// ─────────────────────────────────────────────────────────────────────────────
//
// TWO FIXED TARGETS, both RELOCATABLE-OBJECT formats: see the file docblock for
// why the host-native target would be the wrong choice here.
[[nodiscard]] std::vector<std::string> dssTargets() {
    return {"x86_64:pe64-x86_64-windows", "x86_64:elf64-x86_64-linux"};
}

struct DssVerdict {
    bool                     ran = false;      // the CLI was launched at all
    std::map<std::string, bool> acceptedFor;   // target -> accepted
    std::string              diagnostic;       // the CLI's own output on refusal
    std::string              launchFailure;    // non-empty = a HARNESS failure

    [[nodiscard]] bool acceptedAnywhere() const {
        for (auto const& [t, a] : acceptedFor) if (a) return true;
        return false;
    }
    [[nodiscard]] std::string perTarget() const {
        std::string s;
        for (auto const& [t, a] : acceptedFor)
            s += (s.empty() ? "" : " ") + t + "=" + (a ? "accept" : "reject");
        return s;
    }
};

[[nodiscard]] DssVerdict runDss(fs::path const& src, fs::path const& work,
                                std::string const& probeId) {
    DssVerdict v;
    for (auto const& target : dssTargets()) {
        std::string safe = target;
        for (auto& ch : safe) if (ch == ':' || ch == '/' || ch == '\\') ch = '_';
        fs::path const outDir = work / ("dssout_" + probeId + "_" + safe);
        fs::path const log    = work / ("dss_" + probeId + "_" + safe + ".log");
        std::string const cmd =
            quoted(std::string{DSS_CLI_PATH}) + " --compile " + quoted(src.string())
            + " --language c-subset --target " + target + " --output " + quoted(outDir.string());
        // ✔ rc captured DIRECTLY from std::system, never after a pipe.
        int const status = std::system(native_probe::captureCmd(cmd, log).c_str());
        if (status == -1) {
            v.launchFailure = "std::system could not start a shell for `" + cmd + "`";
            return v;
        }
        v.ran                  = true;
        v.acceptedFor[target]  = (status == 0);
        if (status != 0) {
            std::ifstream in{log, std::ios::binary};
            if (in) {
                std::ostringstream ss;
                ss << in.rdbuf();
                v.diagnostic += "[" + target + "]\n" + ss.str();
            } else {
                v.launchFailure = "the DSS CLI exited " + std::to_string(status)
                                + " for " + target + " but its capture log `"
                                + log.string() + "` could not be opened, so WHY is "
                                  "not known here";
            }
        }
    }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
//  THE DECISION FUNCTION — the whole point, kept pure so it can be exercised
// ─────────────────────────────────────────────────────────────────────────────
enum class Outcome : std::uint8_t {
    Agreement,          // DSS and the references say the same thing
    DirectionADefect,   // >=1 judging oracle ACCEPTS, DSS REJECTS everywhere
    DirectionBDefect,   // NO judging oracle accepts, DSS ACCEPTS somewhere
    Unoracled,          // no available oracle was entitled to judge this probe
    NotPinned,          // `@expect-ref varies` — censused, deliberately not pinned
};

[[nodiscard]] char const* outcomeName(Outcome o) {
    switch (o) {
        case Outcome::Agreement:        return "AGREE";
        case Outcome::DirectionADefect: return "DIR-A";
        case Outcome::DirectionBDefect: return "DIR-B";
        case Outcome::Unoracled:        return "UNORACLED";
        case Outcome::NotPinned:        return "NOT-PINNED";
    }
    return "?";
}

// PURE. Given how many oracles judged the probe, whether any of them accepted,
// whether DSS accepted for any target, and whether the corpus declared the
// reference verdict host-dependent — what is this row?
//
// `varies` is NOT PINNED IN EITHER DIRECTION, and that is a measured necessity
// rather than a convenience. ✔MEASURED: cl 19.51 ACCEPTS `enum E { };` while
// gcc 13.x and clang 18/19 refuse it. On a Windows host a reference compiler
// accepts it, so DSS accepting it is agreement; on a Linux host the same probe
// would look invented purely because no MSVC exists there. A pin whose verdict
// depends on which machine ran it is not a pin, so those rows are censused,
// printed with their note, and counted — never turned into a red.
[[nodiscard]] Outcome classify(std::size_t judgingOracles, bool anyOracleAccepted,
                               bool dssAcceptedSomewhere, ExpectRef expectRef) {
    if (judgingOracles == 0)              return Outcome::Unoracled;
    if (expectRef == ExpectRef::Varies)   return Outcome::NotPinned;
    if (anyOracleAccepted && !dssAcceptedSomewhere) return Outcome::DirectionADefect;
    if (!anyOracleAccepted && dssAcceptedSomewhere) return Outcome::DirectionBDefect;
    return Outcome::Agreement;
}

[[nodiscard]] bool isDefect(Outcome o) {
    return o == Outcome::DirectionADefect || o == Outcome::DirectionBDefect;
}

// ─────────────────────────────────────────────────────────────────────────────
//  FLOORS — fail-closed enumeration
// ─────────────────────────────────────────────────────────────────────────────
//
// A count is the WEAK half of every floor here, and it is present only to catch
// the one failure a content check cannot: a corpus scan that collapses to
// nothing. The STRONG half is `kRequiredProbeIds` — named rows that must exist,
// because "76 probes were found" is equally true of 76 corrupted ones.
constexpr std::size_t kMinProbes           = 60;
constexpr std::size_t kMinDirectionAProbes = 40;
constexpr std::size_t kMinDirectionBProbes = 15;

// Rows this corpus is not allowed to lose. The direction-B block is listed in
// FULL: it is the half that had never been checked, every entry is a MEASURED
// divergence, and a "cleanup" that drops one would otherwise pass silently.
[[nodiscard]] std::vector<std::string> requiredProbeIds() {
    return {
        // the harness's own accept/reject discrimination
        "sanity_accept", "sanity_reject",
        // ── direction B, complete ──
        "b_extern_library_realization_string",
        "b_extern_library_realization_string_function",
        "b_extern_library_realization_string_decl_list",
        "b_extern_library_realization_wide_string",
        "b_va_start_keywords_without_stdarg",
        "b_asm_label_after_attribute_and_duplicated",
        "b_comma_operator_in_array_size",
        "b_comma_operator_in_block_array_size",
        "b_comma_operator_in_case_label",
        "b_comma_operator_in_designated_index",
        "b_empty_enumerator_list",
        "b_elided_enumerator",
        "b_elided_initializer_element",
        "b_elided_struct_member_declarator",
        "b_ellipsis_not_last_parameter",
        "b_duplicate_array_param_qualifiers",
        "b_assignment_inside_attribute_argument",
        "b_msvc_calling_convention_macro_without_windows_h",
        // ── direction A: one representative per standard level + per extension
        // family, so a trim cannot quietly remove a whole dimension ──
        "a_generic_c11", "a_compound_literal_c99", "a_nullptr_c23",
        "a_typeof_c23", "a_bitint_c23", "a_va_opt_c23",
        "a_attribute_packed_gnu", "a_asm_label_gnu", "a_int128_gnu",
        "a_statement_expression_gnu", "a_inline_asm_gnu",
        "a_declspec_msvc", "a_seh_try_except_msvc", "a_counter_macro",
        // The eight gaps found by EXERCISING the red-on-disable arms rather than by
        // reading them: an arm whose premise was `DSS rejects __builtin_clz` turned
        // out to be false (DSS accepts it), and measuring nine alternatives to
        // replace it surfaced these. Listed by name for the same reason as the rest.
        "a_local_label_gnu", "a_builtin_types_compatible_p_gnu",
        "a_builtin_choose_expr_gnu", "a_builtin_offsetof_gnu", "a_real_imag_gnu",
        "a_alignof_gnu_spelling", "a_attribute_constructor_gnu",
        "a_attribute_alias_gnu",
        // ★ A row is listed here whether it is an OPEN gap or a CLOSED one. Once
        // `a_typeof_gnu_spelling` and `a_alignof_gnu_spelling` were fixed
        // (D-CSUBSET-TYPEOF-GNU-SPELLING / D-CSUBSET-ALIGNOF-GNU-SPELLING,
        // 2026-08-11) they stopped being gap records and became FEATURE guards —
        // deleting either now would retire the only cross-compiler evidence that
        // the alias still resolves, which is exactly what this list exists to
        // prevent. `a_alignof_expression_operand_gnu` is the gap that closing
        // them EXPOSED: the sibling probe only ever exercised the TYPE-NAME
        // operand, so the operand axis had never been oracled at all, on any
        // spelling. Named the day it was found so it cannot be trimmed before it
        // is fixed.
        "a_alignof_expression_operand_gnu",
    };
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  ONE discovery per test PROCESS. Discovery costs a compile per candidate per
//  candidate std flag, and three of the four tests below need it; re-running it
//  per TEST would triple that for no extra information. The scratch dir is
//  ScratchDir's per-run one, so two concurrent ctest processes never share a
//  path (D-TEST-FIXED-SCRATCH-PATH-POPULATION).
// ═══════════════════════════════════════════════════════════════════════════
namespace {

struct Harness {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "reference-conformance"};
    OracleSet   oracles;
    CorpusParse corpus;
    std::string repoRootDiag;

    Harness() {
        // The SPAWNED CLI resolves shipped config through its own
        // config_path_walk, which reads $DSS_CONFIG_ROOT and otherwise walks up
        // from ITS cwd. ctest sets the variable for this test's own process and
        // the child inherits it — but a developer running this binary directly
        // has no such environment, and the child's cwd would then decide. So set
        // it here from the shared resolver: baked DSS_TEST_REPO_ROOT is one of
        // its candidates, which is what makes an out-of-tree build work with no
        // environment at all.
        try {
            auto const root = dss::test::repoRoot().string();
#if defined(_WIN32)
            ::_putenv_s("DSS_CONFIG_ROOT", root.c_str());
#else
            ::setenv("DSS_CONFIG_ROOT", root.c_str(), /*overwrite=*/1);
#endif
            corpus = parseCorpus(fs::path{root} / "tests" / "conformance" / "corpus"
                                 / "reference_conformance.probes");
        } catch (std::exception const& e) {
            repoRootDiag = e.what();
        }
        oracles = discoverOracles(scratch.path());
    }
};

[[nodiscard]] Harness& harness() {
    static Harness h;
    return h;
}

// The oracle roster, printed by every test that depends on it. An absent oracle
// is NAMED with its reason here and nowhere else — a run whose log does not say
// which compilers were used has not reported its own scope.
[[nodiscard]] std::string describeOracles(OracleSet const& set) {
    std::ostringstream s;
    s << "\n  ORACLES USED (" << set.found.size() << "):";
    if (set.found.empty()) s << "  NONE";
    for (auto const& o : set.found) {
        s << "\n    " << o.id << "  family=" << familyName(o.family)
          << "  flag=" << o.stdFlag << "  __STDC_VERSION__=" << o.stdcVersion
          << (o.stdcVersion >= 202311 ? "  [C23-LEVEL]"
              : o.stdcVersion >= 202000 ? "  [C2x-DRAFT, NOT C23]" : "")
          << "\n      identity: " << o.ident
          << "\n      command : " << o.commandShape();
    }
    s << "\n  ORACLES NOT USED (" << set.absent.size() << ", each with a named reason):";
    for (auto const& a : set.absent) s << "\n    " << a.name << " — " << a.reason;
    if (!set.broken.empty()) {
        s << "\n  HARNESS/HOST DEFECTS (" << set.broken.size() << "):";
        for (auto const& b : set.broken) s << "\n    " << b;
    }
    return s.str();
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  1. THE CORPUS ITSELF — no oracle needed, so this arm is LIVE on every host.
// ═══════════════════════════════════════════════════════════════════════════
TEST(ReferenceConformance, CorpusIsWellFormedAndComplete) {
    auto const& h = harness();
    ASSERT_TRUE(h.repoRootDiag.empty()) << h.repoRootDiag;
    ASSERT_TRUE(h.corpus.error.empty())
        << "the probe corpus did not parse: " << h.corpus.error;

    auto const& probes = h.corpus.probes;
    ASSERT_GE(probes.size(), kMinProbes)
        << "COLLAPSED CORPUS SCAN: only " << probes.size() << " probes parsed, floor is "
        << kMinProbes << ". A differential oracle that scans nothing agrees with "
           "everything.";

    std::map<std::string, Probe const*> byId;
    for (auto const& p : probes) byId[p.id] = &p;
    ASSERT_EQ(byId.size(), probes.size()) << "duplicate probe ids survived the parse";

    // ★ CONTENT, NOT COUNT. Named rows, checked by name.
    std::vector<std::string> missing;
    for (auto const& id : requiredProbeIds())
        if (byId.find(id) == byId.end()) missing.push_back(id);
    {
        std::string joined;
        for (auto const& m : missing) joined += (joined.empty() ? "" : ", ") + m;
        EXPECT_TRUE(missing.empty())
            << "REQUIRED PROBES ARE MISSING: " << joined
            << ". These are named rather than counted because a corpus of the right "
               "SIZE and the wrong CONTENT is the failure a count cannot see. If a probe "
               "was deliberately retired, remove it from requiredProbeIds() in the same "
               "change and say why.";
    }

    std::size_t nA = 0, nB = 0, nVaries = 0, nAcked = 0;
    for (auto const& p : probes) {
        EXPECT_FALSE(p.why.empty()) << p.id << ": @why is required";
        EXPECT_FALSE(p.code.empty()) << p.id << ": @code is required";
        EXPECT_FALSE(p.families.empty()) << p.id << ": @families is required";

        // No `#include` anywhere: system headers are the biggest confound
        // available to this corpus and are excluded by construction.
        EXPECT_EQ(p.code.find("#include"), std::string::npos)
            << p.id << ": a probe must not #include anything — a divergence caused by "
                       "two different <stdio.h> is not a divergence about C.";

        if (p.expectRef == ExpectRef::Varies) {
            ++nVaries;
            EXPECT_FALSE(p.variesNote.empty())
                << p.id << ": `@expect-ref varies` waives the pin in BOTH directions, so "
                           "it MUST carry a non-empty @varies-note saying which compiler "
                           "splits from which. A blank waiver is a guard turned off with "
                           "no record.";
        } else {
            EXPECT_TRUE(p.variesNote.empty())
                << p.id << ": @varies-note is only meaningful with `@expect-ref varies`";
        }
        if (p.acknowledged()) ++nAcked;

        if (p.direction == Direction::B) {
            ++nB;
            // A direction-B claim is "NO reference compiler accepts this". Judging
            // it strictly, or against only one family, would manufacture that
            // conclusion instead of measuring it.
            EXPECT_EQ(p.mode, Mode::Extended)
                << p.id << ": a direction-B probe must be judged in the MOST PERMISSIVE "
                           "reference mode (`@mode extended`). Under -pedantic-errors a "
                           "legitimate GNU extension also fails, so a strict B probe "
                           "cannot tell `invented` from `non-ISO`.";
            EXPECT_EQ(p.families.size(), 2u)
                << p.id << ": a direction-B probe must name EVERY family (`gnu,msvc`) — "
                           "\"no reference compiler accepts it\" is a union over all of "
                           "them, and narrowing turns an unmeasured family into evidence.";
        } else {
            ++nA;
        }
    }
    EXPECT_GE(nA, kMinDirectionAProbes) << "direction-A coverage floor";
    EXPECT_GE(nB, kMinDirectionBProbes)
        << "direction-B coverage floor. Direction B is the half that had never been "
           "checked; letting it shrink is letting this file stop doing the thing it "
           "was built for.";

    std::cout << "[corpus] probes=" << probes.size() << " directionA=" << nA
              << " directionB=" << nB << " expect-ref-varies=" << nVaries
              << " acknowledged-gaps=" << nAcked << "\n";
    for (auto const& p : probes)
        if (p.expectRef == ExpectRef::Varies)
            std::cout << "[corpus] NOT PINNED (expect-ref varies): " << p.id << " — "
                      << p.variesNote << "\n";
}

// ═══════════════════════════════════════════════════════════════════════════
//  2. THE ORACLE ROSTER — every verdict must carry a MEASURED level.
// ═══════════════════════════════════════════════════════════════════════════
TEST(ReferenceConformance, OracleStandardLevelsAreMeasuredNotAssumed) {
    auto const& set = harness().oracles;
    std::cout << "[oracles]" << describeOracles(set) << "\n";

    // A locator that BROKE is a defect, distinct from a machine with no compiler.
    for (auto const& b : set.broken) ADD_FAILURE() << b;

    if (set.found.empty()) {
        // ★ A SKIP IS ITSELF FAIL-CLOSED. ctest scores a gtest SKIP as a pass, which
        // is exactly the `D-TEST-NATIVE-ORACLE-INERT-ON-POSIX` shape — so the one
        // legitimate skip is only legitimate if discovery actually LOOKED. An empty
        // absent list means the candidate enumeration produced nothing at all, i.e.
        // a harness defect wearing a skip's clothes.
        EXPECT_FALSE(set.absent.empty())
            << "DECORATIVE SKIP: no oracle was found AND no candidate was even "
               "enumerated, so nothing was looked for. That is a harness defect, not a "
               "toolchain-less host.";
        GTEST_SKIP() << "NO reference C compiler could be used on this host. This is the "
                        "ONE legitimate skip, and it is a skip rather than a pass on "
                        "purpose (D-TEST-NATIVE-ORACLE-INERT-ON-POSIX). Every candidate "
                        "and its reason:" << describeOracles(set);
    }

    for (auto const& o : set.found) {
        EXPECT_FALSE(o.ident.empty()) << o.id << ": no identity was read back from it";
        EXPECT_FALSE(o.stdFlag.empty()) << o.id << ": no std flag was attributed";
        EXPECT_GT(o.stdcVersion, 0L)
            << o.id << ": an oracle with no measured __STDC_VERSION__ must never be used "
                       "— its verdicts could not be reported honestly.";
    }

    // ★ A LEVEL CLAIM IS RE-VERIFIED BY A SECOND, INDEPENDENT MECHANISM, and the
    // negative control is what makes that verification mean something.
    //
    // Discovery reads the level out of a `#pragma message` — a PARSE, which can be
    // fooled by anything that puts a similar string in the log. So each oracle is
    // asked again, this time through `#error`, where the answer is the compiler's
    // own exit code and no parsing is involved:
    //
    //   AT   — `#if __STDC_VERSION__ < <claimed>  → #error` MUST COMPILE.
    //   OVER — the same file with the threshold raised ABOVE the claimed level MUST
    //          FAIL. Without this arm the AT arm passes on any compiler that
    //          silently ignores `#error`, and the whole check would be vacuous —
    //          the exact "exercise the failure arm, don't read it" lesson.
    fs::path const work = harness().scratch.path();
    for (auto const& o : set.found) {
        auto const write = [&](fs::path const& p, long threshold) {
            std::ofstream f{p, std::ios::binary | std::ios::trunc};
            f << "#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < " << threshold << "L\n"
              << "#error DSS_ORACLE_LEVEL_NOT_REACHED\n"
              << "#endif\n"
              << "int dss_level_probe_object;\n";
        };
        fs::path const atSrc   = work / ("level_at_" + o.id + ".c");
        fs::path const overSrc = work / ("level_over_" + o.id + ".c");
        write(atSrc, o.stdcVersion);
        write(overSrc, o.stdcVersion + 1);

        std::vector<Step> steps;
        if (!o.setupCommand.empty()) steps.push_back({o.setupCommand, "", {}});
        steps.push_back({o.compileCommand(atSrc, work / ("level_at_" + o.id + ".o"),
                                          Mode::Strict, o.stdFlag), "AT",
                         work / ("level_at_" + o.id + ".txt")});
        steps.push_back({o.compileCommand(overSrc, work / ("level_over_" + o.id + ".o"),
                                          Mode::Strict, o.stdFlag), "OVER",
                         work / ("level_over_" + o.id + ".txt")});
        auto const run = runScript(o.shell, work / ("level_" + o.id + shellSuffix(o.shell)),
                                   work / ("level_" + o.id + ".log"), steps);
        ASSERT_TRUE(run.launched) << o.id << ": level re-verification driver failed to "
                                             "launch: " << run.diagnostic
                                  << native_probe::tailOf(run.logPath, 25);
        auto const& outcomes = run.outcomes;
        auto const  at  = outcomes.find("AT");
        auto const over = outcomes.find("OVER");
        ASSERT_NE(at, outcomes.end())   << o.id << ": no AT verdict";
        ASSERT_NE(over, outcomes.end()) << o.id << ": no OVER verdict";
        EXPECT_TRUE(at->second.accepted)
            << o.id << " (" << o.ident << ") was labelled __STDC_VERSION__="
            << o.stdcVersion << " from its `#pragma message`, but it REFUSES a file that "
               "only requires >= that value. The label is wrong.\n" << at->second.output;
        EXPECT_FALSE(over->second.accepted)
            << o.id << " (" << o.ident << ") accepted a file requiring __STDC_VERSION__ >= "
            << (o.stdcVersion + 1) << "L, one above its measured level. Either the level is "
               "understated or this compiler ignores `#error` — and in the second case the "
               "AT arm above proves nothing.";
    }

    // The gating property, asserted rather than trusted: no probe that demands
    // C23 may be judged by a sub-C23 oracle. Verified against the corpus so the
    // rule is checked where it is USED, not only where it is written.
    ASSERT_TRUE(harness().corpus.error.empty()) << harness().corpus.error;
    std::size_t checked = 0;
    for (auto const& p : harness().corpus.probes) {
        if (p.minStdc < 202311L) continue;
        for (auto const& o : set.found) {
            bool const relevant = p.families.count(o.family) != 0;
            bool const judges   = relevant && o.stdcVersion >= p.minStdc;
            if (judges) EXPECT_GE(o.stdcVersion, 202311L)
                << p.id << " demands C23 but " << o.id << " (" << o.stdcVersion
                << "L) was admitted as a judge";
            ++checked;
        }
    }
    EXPECT_GT(checked, 0u)
        << "the C23 gating property was never exercised: either the corpus has no "
           "`@min-stdc 202311` probe left, or no oracle was found. Both make this "
           "assertion decorative.";
}

// ═══════════════════════════════════════════════════════════════════════════
//  3. THE DECISION FUNCTION, EXHAUSTIVELY. The census in test 4 can only be as
//  trustworthy as this table, and this table needs no compiler — so it is the
//  one arm that is live even on a host with no toolchain at all.
// ═══════════════════════════════════════════════════════════════════════════
TEST(ReferenceConformance, ClassifierTruthTableIsExhaustive) {
    using E = ExpectRef;

    // No judging oracle → UNORACLED, whatever DSS said. A row with no reference
    // verdict must never be scored as agreement: that is the inert-oracle defect
    // in its purest form.
    for (bool dss : {false, true})
        for (E e : {E::Accept, E::Reject, E::Varies})
            EXPECT_EQ(classify(0, /*anyOracleAccepted=*/false, dss, e), Outcome::Unoracled)
                << "judgingOracles=0 must be UNORACLED";

    // `varies` → NOT PINNED in both directions, for every verdict combination.
    for (bool refAcc : {false, true})
        for (bool dss : {false, true})
            EXPECT_EQ(classify(3, refAcc, dss, E::Varies), Outcome::NotPinned);

    // The four pinned combinations, at every oracle count that means "someone
    // judged it".
    for (std::size_t n : {std::size_t{1}, std::size_t{5}}) {
        for (E e : {E::Accept, E::Reject}) {
            EXPECT_EQ(classify(n, /*refAcc=*/true,  /*dss=*/false, e),
                      Outcome::DirectionADefect)
                << "reference accepts + DSS rejects is direction A";
            EXPECT_EQ(classify(n, /*refAcc=*/false, /*dss=*/true,  e),
                      Outcome::DirectionBDefect)
                << "no reference accepts + DSS accepts is direction B — the invented-"
                   "extension case this whole file exists to detect";
            EXPECT_EQ(classify(n, /*refAcc=*/true,  /*dss=*/true,  e), Outcome::Agreement);
            EXPECT_EQ(classify(n, /*refAcc=*/false, /*dss=*/false, e), Outcome::Agreement);
        }
    }

    EXPECT_TRUE(isDefect(Outcome::DirectionADefect));
    EXPECT_TRUE(isDefect(Outcome::DirectionBDefect));
    EXPECT_FALSE(isDefect(Outcome::Agreement));
    EXPECT_FALSE(isDefect(Outcome::Unoracled));
    EXPECT_FALSE(isDefect(Outcome::NotPinned));
}

// ═══════════════════════════════════════════════════════════════════════════
//  4. THE CENSUS — compile every probe with DSS and with every oracle, diff.
// ═══════════════════════════════════════════════════════════════════════════
TEST(ReferenceConformance, DifferentialAcceptRejectCensus) {
    auto& h = harness();
    ASSERT_TRUE(h.repoRootDiag.empty()) << h.repoRootDiag;
    ASSERT_TRUE(h.corpus.error.empty()) << h.corpus.error;
    auto const& probes = h.corpus.probes;
    auto const& set    = h.oracles;

    for (auto const& b : set.broken) ADD_FAILURE() << b;
    if (set.found.empty()) {
        // Same fail-closed condition as the roster test: see the note there.
        EXPECT_FALSE(set.absent.empty())
            << "DECORATIVE SKIP: nothing was found and nothing was looked for.";
        GTEST_SKIP() << "NO usable reference C compiler on this host — the census cannot "
                        "be taken. Reported as a SKIP with every candidate named, never "
                        "as a pass:" << describeOracles(set);
    }
    std::cout << "[census]" << describeOracles(set) << "\n";

    fs::path const work = h.scratch.path();

    // ── Write every probe to disk once, then run each oracle as ONE batch ──
    std::map<std::string, fs::path> srcOf;
    for (auto const& p : probes) {
        fs::path const src = work / (p.id + ".c");
        std::ofstream o{src, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(o.good()) << "could not write probe source " << src;
        o << p.code;
        o.close();
        srcOf[p.id] = src;
    }

    // oracleId -> probeId -> outcome
    std::map<std::string, std::map<std::string, StepOutcome>> refVerdicts;
    for (auto const& o : set.found) {
        std::vector<Step> steps;
        if (!o.setupCommand.empty()) steps.push_back({o.setupCommand, "", {}});
        for (auto const& p : probes) {
            if (p.families.count(o.family) == 0) continue;
            if (o.stdcVersion < p.minStdc) continue;
            fs::path const obj = work / (p.id + "." + o.id + ".o");
            steps.push_back({o.compileCommand(srcOf[p.id], obj, p.mode, o.stdFlag), p.id,
                             work / (p.id + "." + o.id + ".txt")});
        }
        auto const run = runScript(o.shell, work / ("drive_" + o.id + shellSuffix(o.shell)),
                                   work / ("drive_" + o.id + ".log"), steps);
        ASSERT_TRUE(run.launched)
            << "ORACLE DRIVER FAILED TO LAUNCH for " << o.id << ": " << run.diagnostic
            << native_probe::tailOf(run.logPath, 25)
            << " — this is a harness defect, not an absent compiler, and it must never "
               "read as agreement.";
        refVerdicts[o.id] = run.outcomes;
    }

    // ── The SANITY PAIR. Before a single conformance claim is made, prove that
    // each driver can distinguish accept from reject AT ALL. A quoting slip that
    // made every compile fail would otherwise turn this whole census into "DSS
    // accepts things nobody accepts" — 76 false direction-B rows.
    for (auto const& o : set.found) {
        auto const& v  = refVerdicts[o.id];
        auto const acc = v.find("sanity_accept");
        auto const rej = v.find("sanity_reject");
        ASSERT_NE(acc, v.end()) << "ORACLE BROKEN (" << o.id << "): its driver produced no "
                                   "verdict for `sanity_accept`.";
        ASSERT_NE(rej, v.end()) << "ORACLE BROKEN (" << o.id << "): its driver produced no "
                                   "verdict for `sanity_reject`.";
        EXPECT_TRUE(acc->second.accepted)
            << "ORACLE BROKEN (" << o.id << " — " << o.ident << "): it REJECTED `int x;`, "
               "so its verdicts mean nothing. rc=" << acc->second.rc << "\n"
            << acc->second.output;
        EXPECT_FALSE(rej->second.accepted)
            << "ORACLE BROKEN (" << o.id << " — " << o.ident << "): it ACCEPTED a probe "
               "full of illegal characters, so a 'reject' from it is not evidence.\n"
            << rej->second.output;
    }

    // ── DSS, through its real CLI, for both fixed targets ──
    native_probe::ExecutedRows dssRows{"DSS verdicts produced", probes.size()};
    std::map<std::string, DssVerdict> dss;
    for (auto const& p : probes) {
        auto v = runDss(srcOf[p.id], work, p.id);
        ASSERT_TRUE(v.launchFailure.empty())
            << "DSS CLI HARNESS FAILURE on probe " << p.id << ": " << v.launchFailure;
        ASSERT_TRUE(v.ran) << "the DSS CLI produced no verdict for " << p.id;
        ASSERT_EQ(v.acceptedFor.size(), dssTargets().size())
            << p.id << ": DSS was not asked about every fixed target";
        // CONTENT, not just a code: a refusal must carry a real diagnostic. DSS
        // exiting nonzero with nothing to say would be a fail-quiet, and this
        // corpus is exactly where that would be invisible.
        if (!v.acceptedAnywhere()) {
            bool const hasDiag = v.diagnostic.find("error") != std::string::npos
                              || v.diagnostic.find("Error") != std::string::npos;
            EXPECT_TRUE(hasDiag)
                << p.id << ": DSS refused the probe for EVERY target but printed no "
                           "diagnostic containing `error`. A silent refusal is a "
                           "fail-quiet.\n" << v.diagnostic;
        }
        dssRows.record();
        dss[p.id] = std::move(v);
    }

    // ── DIFF ──
    native_probe::ExecutedRows judgedRows{"probes compared against >=1 oracle", 1};
    // ⚠ DELIBERATELY PLAIN COUNTERS, NOT `ExecutedRows`. A ledger with a floor on
    // the DEFECT counts would go red precisely when every gap has been closed —
    // i.e. it would punish the outcome this file exists to produce. The floors
    // belong on the ENUMERATION (probes found, probes compared, DSS verdicts
    // produced), never on the findings.
    std::size_t dirACount = 0, dirBCount = 0;

    std::ostringstream table;
    table << "\n[census] probe                                            | outcome    | DSS"
             "                | reference verdicts";
    std::vector<std::string> newDefects, staleAcks, expectRefWrong, unoracled, censusA, censusB;

    for (auto const& p : probes) {
        std::vector<std::string> judged, accepting;
        for (auto const& o : set.found) {
            if (p.families.count(o.family) == 0) continue;
            if (o.stdcVersion < p.minStdc) continue;
            auto const& v  = refVerdicts[o.id];
            auto const it = v.find(p.id);
            if (it == v.end()) {
                // The driver ran (the sanity pair proved it) but this row has no
                // marker: a step vanished. That is a harness defect, not a verdict.
                ADD_FAILURE() << "ORACLE " << o.id << " produced NO verdict line for probe "
                              << p.id << " even though its driver launched and passed the "
                                         "sanity pair. A missing row must never be read as "
                                         "agreement.";
                continue;
            }
            judged.push_back(o.id);
            if (it->second.accepted) accepting.push_back(o.id);
        }

        bool const dssAccepts = dss[p.id].acceptedAnywhere();
        auto const outcome = classify(judged.size(), !accepting.empty(), dssAccepts,
                                      p.expectRef);

        std::string refCell;
        for (auto const& o : set.found) {
            auto const& v = refVerdicts[o.id];
            auto const it = v.find(p.id);
            char const* cell = (it == v.end()) ? "-"
                             : (it->second.accepted ? "ACCEPT" : "reject");
            refCell += " " + o.id + "=" + cell;
        }
        table << "\n[census] " << p.id << std::string(p.id.size() < 48 ? 48 - p.id.size() : 1, ' ')
              << "| " << outcomeName(outcome)
              << std::string(std::max<std::size_t>(
                     1, 11 - std::min<std::size_t>(
                            10, std::string_view{outcomeName(outcome)}.size())), ' ')
              << "| " << dss[p.id].perTarget() << " |" << refCell;

        if (!judged.empty()) judgedRows.record();
        if (outcome == Outcome::Unoracled) {
            unoracled.push_back(p.id);
            continue;
        }
        if (outcome == Outcome::NotPinned) continue;

        // The corpus's own understanding, cross-checked against the measurement.
        if (p.expectRef == ExpectRef::Accept && accepting.empty()) {
            std::string j;
            for (auto const& x : judged) j += (j.empty() ? "" : ",") + x;
            expectRefWrong.push_back(p.id + ": declares `@expect-ref accept` but NONE of {"
                                     + j + "} accepted it");
        }
        if (p.expectRef == ExpectRef::Reject && !accepting.empty()) {
            std::string a;
            for (auto const& x : accepting) a += (a.empty() ? "" : ",") + x;
            expectRefWrong.push_back(p.id + ": declares `@expect-ref reject` but it was "
                                     "ACCEPTED by {" + a + "} — the corpus is wrong about "
                                     "reference compilers, which is worse than a gap");
        }

        if (outcome == Outcome::DirectionADefect) ++dirACount;
        if (outcome == Outcome::DirectionBDefect) ++dirBCount;

        std::string const row =
            p.id + "  [DSS " + dss[p.id].perTarget() + "]  [refs:" + refCell + "]";
        if (outcome == Outcome::DirectionADefect) censusA.push_back(row);
        if (outcome == Outcome::DirectionBDefect) censusB.push_back(row);

        if (isDefect(outcome) && !p.acknowledged()) {
            newDefects.push_back(
                std::string{outcomeName(outcome)} + " on `" + p.id + "`: " + row
                + "\n      why: " + p.why);
        }
        // ★ THE RATCHET, AND IT ONLY TURNS ONE WAY. An acknowledgement that no
        // longer describes a divergence is a guard that has been weakened until
        // it asserts nothing — this repo has that on record twice. Closing a gap
        // therefore reds HERE, with the exact edit named, rather than leaving a
        // row that will still read as "known" a year from now.
        if (!isDefect(outcome) && p.acknowledged() && p.id != "sanity_reject") {
            staleAcks.push_back(p.id);
        }
    }
    std::cout << table.str() << "\n";

    // ── The censuses, always printed: this is the deliverable, not the failure. ──
    std::cout << "\n[census] DIRECTION-B (DSS accepts, NO reference compiler does) — "
              << censusB.size() << " row(s):\n";
    for (auto const& r : censusB) std::cout << "  " << r << "\n";
    std::cout << "[census] DIRECTION-A (a reference compiler accepts, DSS rejects) — "
              << censusA.size() << " row(s):\n";
    for (auto const& r : censusA) std::cout << "  " << r << "\n";
    std::cout << "[census] UNORACLED on this host (" << unoracled.size()
              << ") — no available compiler was entitled to judge these:\n";
    for (auto const& u : unoracled) std::cout << "  " << u << "\n";

    // ── Verdicts ──
    for (auto const& e : expectRefWrong)
        ADD_FAILURE() << "CORPUS DISAGREES WITH MEASUREMENT — " << e;

    for (auto const& d : newDefects)
        ADD_FAILURE()
            << "UNACKNOWLEDGED CONFORMANCE DIVERGENCE\n    " << d
            << "\n  Direction A means real C code fails to build with DSS. Direction B "
               "means code written for DSS builds NOWHERE ELSE. Either fix it, or — if it "
               "is a real gap someone else owns — add an `@acknowledged-gap <reason>` line "
               "to that probe in tests/conformance/corpus/reference_conformance.probes so "
               "the gap is RECORDED rather than tolerated silently.";

    for (auto const& s : staleAcks)
        ADD_FAILURE()
            << "STALE ACKNOWLEDGEMENT on probe `" << s << "`: it carries an "
               "`@acknowledged-gap` line but DSS and the reference compilers now AGREE. "
               "That is good news and it requires one edit: DELETE the `@acknowledged-gap` "
               "line from that probe in "
               "tests/conformance/corpus/reference_conformance.probes. This reds on "
               "purpose — an acknowledgement list that keeps rows after they are fixed "
               "stops meaning anything, and then a genuinely new gap hides among them.";

    // ── FAIL-CLOSED enumeration, per dimension. Floors DERIVED from the corpus
    // and the roster, never typed in: a hand-written floor drifts away from what
    // it guards and quietly stops being one.
    std::size_t universalProbes = 0;
    for (auto const& p : probes)
        if (p.minStdc == 0 && p.families.size() == 2) ++universalProbes;
    EXPECT_GE(judgedRows.count(), universalProbes)
        << "INERT ORACLE: " << judgedRows.count() << " probes were compared, but "
        << universalProbes << " of them name every family and demand no standard level, so "
           "ANY located oracle is entitled to judge them. A shortfall means rows were "
           "dropped between the driver and the diff.";
    EXPECT_LT(unoracled.size(), probes.size())
        << "EVERY probe was unoracled — the roster is non-empty yet judged nothing.";

    std::cout << "[census] compared=" << judgedRows.count() << "/" << probes.size()
              << " dss-verdicts=" << dssRows.count()
              << " direction-A=" << dirACount
              << " direction-B=" << dirBCount
              << " unoracled=" << unoracled.size() << "\n";
}
