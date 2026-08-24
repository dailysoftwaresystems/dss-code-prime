#pragma once

// D-LK10-ENTRY Slice C (plan 14 §2.13) run-harness — spawn an
// emitted binary and capture its OS exit code via the platform's
// process-lifecycle API. This is the first test substrate that
// EXECUTES a compiler output (every prior test asserted bytes
// in-memory); the user has explicitly underlined the discipline:
// "the run-harness genuinely spawns the file and asserts the OS
// exit code — not a mocked/in-memory check — this is the first
// test that must touch the real loader."
//
// Platform arms:
//   * Windows — `CreateProcessW` + `WaitForSingleObject` +
//     `GetExitCodeProcess` (Stage 1 Slice C 2026-06-02; the call
//     moved from the `A` to the `W` form when the argv quoting
//     was shared with the compiler's own spawn substrate —
//     D-TEST-RUN-BINARY-ARGV-QUOTING-UNESCAPED. The shared quoter
//     produces a `std::wstring`, so `W` consumes it directly
//     instead of round-tripping it back through UTF-8, and a path
//     character outside the ANSI code page now survives.)
//   * POSIX — `posix_spawn` + `waitpid` with `WNOHANG` poll loop
//     for timeout (closes D-LK10-ENTRY-POSIX-RUN-HARNESS,
//     2026-06-02). The poll loop sleeps in increments capped at
//     remaining-timeout so the harness exits promptly on short
//     runs (the examples runner spawns dozens per test cycle).
//
// Caller writes the bytes to disk first via
// `dss::linker::writeImage`. Caller-side responsibility for
// permissions on POSIX + .exe extension on Windows (the parent-
// directory contract is documented at `writer.hpp`). The
// POSIX arm also chmod+x the spawned binary so `posix_spawn`
// can exec it (the linker writes 0644 by default; a caller that
// already applied 0755 sees the redundant chmod as a no-op).
//
// stdout capture (Plan 11 FF6 Slice 1, 2026-06-02). When
// `captureStdout=true` the harness redirects the child's
// STDOUT + STDERR to an anonymous pipe, drains it after the
// child exits, and reports the captured bytes in
// `RunResult.capturedStdout`. Defaults to OFF so the existing
// 8 D-LK10-ENTRY examples (exit-code-only assertions) stay
// behaviorally identical (the child keeps inheriting the
// parent's stdio handles when capture is off).
//
// The capture path is the prerequisite for the FF6 hello-world
// example pin — without it, a silent print-failure (e.g. a CRT
// init bug or a wrong `puts` mangling) would leave `return 42`
// untouched and the test would pass with no output. We assert
// captured_stdout == "hello\n" alongside exit==42 so a
// regression in ANY layer (FFI mangling, .idata layout, CRT
// init, msvcrt's puts, file-descriptor wiring) trips the pin.

// D-TEST-RUN-BINARY-ARGV-QUOTING-UNESCAPED — the MSVCRT / CommandLineToArgvW
// argv quoter used by the Windows arm below. THE SAME ONE the shipped compiler
// spawns with (`src/core/substrate/process_spawn.cpp`): this harness used to
// carry a second, half-correct copy that escaped neither embedded quotes nor
// trailing backslashes, and a test harness that silently mangles the argv a test
// believes it sent is worse than no harness.
//
// INCLUDED BY RELATIVE PATH on purpose — the same reason, and the same shape, as
// `arm_verdict_ledger.hpp`'s include of `src/program/platform_token.hpp`: this
// header's consumers are targets with different include-path sets, and
// `integrated_tests` (integrated_tests/CMakeLists.txt) carries ONLY
// `tests/test_support`, no `src/`, and links NO DSS library — it drives the CLI
// as a black-box subprocess. A quoted include resolves against the includer's
// own directory first, so this one spelling works everywhere with no build-graph
// change. The target it protects is why the quoter is header-only and
// dependency-free in the first place (see that header's opening note); linking
// `dss-code-prime-lib` into `integrated_tests` to borrow a string helper would
// break the very boundary that target exists to hold. Included UNCONDITIONALLY,
// not under `#if defined(_WIN32)`, so the pure transform is compile-checked on
// every leg of the matrix rather than only where its output is used.
#include "../../src/core/substrate/windows_command_line.hpp"

// The suite's wall-clock budget vocabulary — `kRunBudget` (this header's default
// timeout) and `kAdmissionBudget` are DEFINED there, with the measurements that
// sized them. Reachable by this bare spelling from every consumer, including
// `integrated_tests`, whose target carries only `tests/test_support` on its
// include path (see the note on the relative include above).
#include "test_wait_budget.hpp"

#include <algorithm>  // std::min in the POSIX poll-loop arm; std::sort of sysroot candidates
#include <chrono>
#include <cstdint>
#include <cstdio>     // AwakeClock's fatal path (the project's fputs+abort pattern)
#include <cstdlib>    // setenv / _putenv_s (QEMU_STACK_SIZE + QEMU_LD_PREFIX), getenv, abort
#include <filesystem>
#include <fstream>    // PT_INTERP read out of the guest artifact
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>     // launcherPrefix (emulator argv prefix)

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    // Prevent <windows.h>'s `max` / `min` macros from clashing with
    // `std::numeric_limits<...>::max()` in including translation
    // units (caught at Slice C audit-fold test addition).
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/resource.h>  // getrlimit/setrlimit RLIMIT_STACK (large-frame corpus)
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <time.h>          // AwakeClock: clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
  #include <unistd.h>

  extern char** environ;
#endif

namespace dss::test_support {

struct RunResult {
    bool          spawned    = false;  // CreateProcess succeeded
    bool          timedOut   = false;  // child exceeded the timeout
    std::uint32_t exitCode   = 0;      // valid when spawned && !timedOut
    std::string   diagnostic;          // populated on any failure
    // FF6 Slice 1: child's stdout+stderr bytes. Populated only
    // when `captureStdout=true` was passed to runBinary. Empty
    // string is a VALID outcome (child printed nothing); callers
    // that care about silent-print regressions must compare
    // against an explicit expected payload, not "nonempty".
    std::string   capturedStdout;
};

// D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB (harness): give EVERY spawned
// binary a generous stack ONCE per process, in the parent, before the
// first spawn. The `large_frame_beyond_16mib` example reserves a ~20 MB
// stack frame that SIGSEGVs under the default 8 MB ulimit (exit 139). Two
// mechanisms, both inherited by the child (posix_spawn / CreateProcess
// inherit the environment + POSIX rlimits):
//   * QEMU_STACK_SIZE=256MB — the qemu-aarch64 child reads it to size the
//     emulated guest stack (the cross-arch run-under-emulator path).
//   * RLIMIT_STACK=256MB (POSIX) — for the NATIVE run (e.g. the
//     ubuntu-24.04-arm leg spawning the arm64 ELF directly): raise the
//     soft limit toward the hard cap; the spawned child inherits it.
// Lives HERE, at the single spawn chokepoint, so BOTH harnesses that spawn
// through runBinary — the in-process `tests/examples/examples_runner` AND
// the `integrated_tests` CLI-subprocess runner — get it by construction.
// (The integrated_tests runner previously lacked the bump, so the native
// arm64-Linux leg SIGSEGV'd on this one example while every other leg —
// which skips the arm64 target as cross-arch/cross-format — stayed green.)
// Harmless to every small-frame example: an idempotent env set + a one-shot
// best-effort rlimit raise (a failure is non-fatal). Function-local static
// ⇒ the cost is paid exactly once per test process.
inline void ensureGenerousSpawnStack() noexcept {
    static bool const done = [] {
        constexpr char const* kStackBytes = "268435456";  // 256 MiB
#if defined(_WIN32)
        ::_putenv_s("QEMU_STACK_SIZE", kStackBytes);
#else
        ::setenv("QEMU_STACK_SIZE", kStackBytes, /*overwrite=*/1);
        struct rlimit rl{};
        if (::getrlimit(RLIMIT_STACK, &rl) == 0) {
            rlim_t const want = static_cast<rlim_t>(268435456);  // 256 MiB
            rlim_t const target =
                (rl.rlim_max == RLIM_INFINITY)
                    ? want
                    : std::min<rlim_t>(want, rl.rlim_max);
            if (rl.rlim_cur < target) {
                rl.rlim_cur = target;
                (void)::setrlimit(RLIMIT_STACK, &rl);  // best-effort
            }
        }
#endif
        return true;
    }();
    (void)done;
}

// ─── D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY, closing-work item (1) ─────────────
//
// The SECOND ambient-process-setup dependency of an emulated spawn, and the
// sibling of `ensureGenerousSpawnStack` directly above: that one owns the
// guest's STACK, this one owns the guest's LOADER SEARCH ROOT. Both are
// properties of the child process that the harness — not the operator's shell
// — is in a position to establish, and both are invisible in the failure text
// when they are missing.
//
// WHAT GOES WRONG WITHOUT IT. `qemu-aarch64` being ON PATH is not the same
// fact as `qemu-aarch64` being ABLE TO RUN a guest binary. A dynamically
// linked guest ELF names its interpreter in PT_INTERP — DSS's arm64 output
// asks for `/lib/ld-linux-aarch64.so.1` — and qemu-user resolves that path
// against the HOST filesystem unless `QEMU_LD_PREFIX` points at a guest
// sysroot. On an x86_64 box that path is not there, so EVERY emulated arm
// dies at exit 255 before the guest's first instruction.
//
// ✔MEASURED 2026-08-12, this project's WSL leg: a raw `ctest` with
// QEMU_LD_PREFIX unset reported 468 of 826 tests failing; with
// `QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` exported and NOTHING else changed,
// 826 of 826 passed. Every one of those 468 was the same single cause —
// `qemu-aarch64: Could not open '/lib/ld-linux-aarch64.so.1': No such file or
// directory` on raw stderr — surfacing to the reader only as `baseline
// exit-code mismatch (expected=42; OS=255)`. That is the SECOND recorded
// occurrence of the identical signature (~450 failures on 2026-08-03), which
// is what moved this from "operator error" to "harness defect": a 468-test
// red whose one environmental cause appears nowhere in the failure text is a
// DIAGNOSTIC failure, and documenting the variable in one more place had
// already been tried.
//
// ★ THE DERIVATION IS FROM VOCABULARY, NOT FROM HOST IDENTITY, and that is
// the constraint that shaped the whole mechanism — there is no
// `if (arch == "aarch64")` ladder anywhere below, because the two inputs
// already carry everything needed:
//
//   * the LAUNCHER's own name states the guest arch — `qemu-<arch>` is
//     qemu-user's naming convention, so `qemu-aarch64` → `aarch64`;
//   * the ARTIFACT's own PT_INTERP states the exact file the guest loader
//     needs — read out of the bytes, never guessed from the arch (the
//     loader's spelling is not derivable from the arch name:
//     `ld-linux-aarch64.so.1` vs `ld-linux-x86-64.so.2` vs
//     `ld-linux-armhf.so.3` is a table, and a table is the thing to avoid).
//
// The candidate sysroots are then the directories under `/usr` whose names
// start with `<arch>-` (the Debian/Ubuntu multiarch cross layout puts them at
// `/usr/<gnu-triple>`, e.g. `/usr/aarch64-linux-gnu`) — enumerated from the
// FILESYSTEM rather than from a list in this file, and each one accepted only
// if it actually contains the interpreter the artifact asked for. So the
// answer to "is this a sysroot for this binary?" is always a probe, never an
// assumption, and a host with its cross runtime somewhere else falls through
// to the loud path rather than being silently mis-served.
//
// ★ NOTHING HERE CAN MASK A REAL CROSS-ARCH FAILURE. Every branch either
// makes a correct binary run that would otherwise have died in the loader, or
// refuses to spawn and says why. It never skips an arm, never suppresses an
// exit code, and never changes what a spawned child is asked to do.

namespace detail {

// The guest architecture a qemu-user launcher emulates, taken from its own
// file name; empty when the launcher is not qemu-user.
//
// TWO exclusions, both load-bearing:
//   * `qemu-system-<arch>` is the FULL-SYSTEM emulator. It boots a kernel and
//     never reads QEMU_LD_PREFIX, so deriving a sysroot for it would attach a
//     remedy to a failure the variable has nothing to do with.
//   * any launcher that is not `qemu-*` at all (`wine`, `box64`, an
//     ssh-into-a-real-board wrapper) is somebody else's mechanism.
//
// `-static` is stripped because Debian's `qemu-user-static` package installs
// the very same emulator as `qemu-<arch>-static` (that is the binfmt_misc
// spelling, and the one a container image usually has); the suffix is part of
// the PACKAGING, not of the guest arch.
[[nodiscard]] inline std::string
qemuUserGuestArch(std::string const& launcherPath) {
    // `stem()` rather than `filename()` so a `.exe` on a Windows-hosted
    // launcher does not become part of the derived architecture.
    std::string const name =
        std::filesystem::path{launcherPath}.filename().stem().string();
    constexpr std::string_view kUserPrefix   = "qemu-";
    constexpr std::string_view kSystemPrefix = "qemu-system-";
    if (!name.starts_with(kUserPrefix))  return {};
    if (name.starts_with(kSystemPrefix)) return {};
    std::string arch = name.substr(kUserPrefix.size());
    constexpr std::string_view kStaticSuffix = "-static";
    if (arch.ends_with(kStaticSuffix)) {
        arch.resize(arch.size() - kStaticSuffix.size());
    }
    return arch;
}

// The PT_INTERP string of an ELF64 little-endian image — i.e. the absolute
// path the guest's loader will be looked up at — or empty.
//
// EMPTY IS A FULL ANSWER, not a failure to answer, and every caller treats it
// as "this harness has no claim to make": a statically linked guest has no
// interpreter to resolve and runs under qemu-user with no sysroot at all, so
// the same empty return correctly means "nothing to arrange" for it as it
// does for a PE, a Mach-O, an unreadable file, or a shape this reader does
// not decode. Refusing to spawn on a no-claim answer would break the static
// case, which works today.
//
// ELF64/LSB only — the one shape DSS emits for the emulated legs. A 32-bit or
// big-endian image returns empty and is spawned unchanged, which is the
// conservative direction: the emulator then reports its own error rather than
// this harness inventing one from a header it did not parse.
[[nodiscard]] inline std::string
elfInterpreterPath(std::filesystem::path const& binaryPath) {
    std::ifstream in{binaryPath, std::ios::binary};
    if (!in) return {};

    unsigned char header[64]{};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) return {};
    if (!(header[0] == 0x7Fu && header[1] == 'E' && header[2] == 'L'
          && header[3] == 'F')) {
        return {};
    }
    if (header[4] != 2u) return {};  // EI_CLASS  != ELFCLASS64
    if (header[5] != 1u) return {};  // EI_DATA   != ELFDATA2LSB

    auto readLe = [](unsigned char const* at, std::size_t width) {
        std::uint64_t value = 0;
        for (std::size_t i = width; i-- > 0;) {
            value = (value << 8) | static_cast<std::uint64_t>(at[i]);
        }
        return value;
    };
    std::uint64_t const phoff     = readLe(header + 0x20, 8);  // e_phoff
    std::uint64_t const phentsize = readLe(header + 0x36, 2);  // e_phentsize
    std::uint64_t const phnum     = readLe(header + 0x38, 2);  // e_phnum
    constexpr std::uint64_t kPhdr64Size = 56;
    if (phoff == 0 || phnum == 0 || phentsize < kPhdr64Size) return {};

    constexpr std::uint32_t kPtInterp = 3;
    for (std::uint64_t index = 0; index < phnum; ++index) {
        unsigned char phdr[kPhdr64Size]{};
        in.seekg(static_cast<std::streamoff>(phoff + index * phentsize),
                 std::ios::beg);
        if (!in) return {};
        in.read(reinterpret_cast<char*>(phdr), kPhdr64Size);
        if (in.gcount() != static_cast<std::streamsize>(kPhdr64Size)) return {};
        if (readLe(phdr + 0, 4) != kPtInterp) continue;

        std::uint64_t const offset = readLe(phdr + 8, 8);   // p_offset
        std::uint64_t const size   = readLe(phdr + 32, 8);  // p_filesz
        // A path longer than this is a corrupt header, not a loader; bound the
        // read rather than trust a length field out of the file under test.
        constexpr std::uint64_t kMaxInterpBytes = 4096;
        if (size == 0 || size > kMaxInterpBytes) return {};
        std::string interpreter(static_cast<std::size_t>(size), '\0');
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in) return {};
        in.read(interpreter.data(), static_cast<std::streamsize>(size));
        if (in.gcount() != static_cast<std::streamsize>(size)) return {};
        // p_filesz counts the NUL terminator; anything at or past it is not
        // part of the path.
        if (auto const nul = interpreter.find('\0'); nul != std::string::npos) {
            interpreter.resize(nul);
        }
        return interpreter;
    }
    return {};
}

// What this spawn needs QEMU_LD_PREFIX to be, decided WITHOUT touching the
// environment or the clock, so every branch below is reachable from a test on
// any host: the operator's value, the host root and the sysroot search root
// are all parameters rather than ambient facts.
struct QemuSysrootDecision {
    std::string interpreter;  // the guest's PT_INTERP; empty ⇒ nothing to resolve
    std::string sysroot;      // the prefix to export; empty ⇒ export nothing
    std::string diagnostic;   // non-empty ⇒ refuse to spawn, and this says why
};

[[nodiscard]] inline QemuSysrootDecision
decideQemuGuestSysroot(std::vector<std::string> const& launcherPrefix,
                       std::filesystem::path const&    binaryPath,
                       std::string const&              operatorSuppliedPrefix,
                       std::filesystem::path const&    hostRoot,
                       std::filesystem::path const&    sysrootSearchRoot) {
    QemuSysrootDecision out;
    if (launcherPrefix.empty()) return out;  // native run — no loader to place
    std::string const guestArch = qemuUserGuestArch(launcherPrefix.front());
    if (guestArch.empty()) return out;       // not qemu-user — not our variable
    // An operator who set it OWNS it. Overwriting a deliberate value with a
    // derived one would make this harness the reason a hand-built sysroot,
    // a container mount or a bisect stopped being used.
    if (!operatorSuppliedPrefix.empty()) return out;

    out.interpreter = elfInterpreterPath(binaryPath);
    if (out.interpreter.empty()) return out;  // static guest / not an ELF64

    std::string interpRelative = out.interpreter;
    while (!interpRelative.empty()
           && (interpRelative.front() == '/' || interpRelative.front() == '\\')) {
        interpRelative.erase(interpRelative.begin());
    }
    if (interpRelative.empty()) return out;

    std::error_code ec;
    // The host root may already carry the guest loader — a native run under
    // qemu (same arch), or a machine with the foreign libc installed
    // multiarch-style at `/`. Then the spawn works with no prefix at all and
    // setting one would only narrow where the child may look.
    if (std::filesystem::exists(hostRoot / interpRelative, ec)) return out;

    std::vector<std::string> candidates;
    if (std::filesystem::is_directory(sysrootSearchRoot, ec)) {
        for (auto const& entry :
             std::filesystem::directory_iterator(
                 sysrootSearchRoot, std::filesystem::directory_options::
                                        skip_permission_denied, ec)) {
            auto const name = entry.path().filename().string();
            if (!name.starts_with(guestArch + "-")) continue;
            candidates.push_back(entry.path().generic_string());
        }
    }
    // `directory_iterator` order is unspecified; a harness whose choice of
    // sysroot depends on inode order is one whose failures cannot be
    // reproduced from the message it printed.
    std::sort(candidates.begin(), candidates.end());
    for (auto const& candidate : candidates) {
        if (std::filesystem::exists(
                std::filesystem::path{candidate} / interpRelative, ec)) {
            out.sysroot = candidate;
            return out;
        }
    }

    std::string searched;
    for (auto const& candidate : candidates) {
        if (!searched.empty()) searched += ", ";
        searched += candidate;
    }
    if (searched.empty()) {
        searched = "no '" + guestArch + "-*' directory under '"
                 + sysrootSearchRoot.generic_string() + "'";
    }
    out.diagnostic =
        "runBinary: refusing to spawn '" + binaryPath.generic_string()
      + "' under '" + launcherPrefix.front() + "' — QEMU_LD_PREFIX is UNSET "
        "and no guest sysroot on this host provides '" + out.interpreter
      + "', the ELF interpreter this binary declares. qemu-user resolves that "
        "path against the host filesystem, so it would exit 255 before the "
        "guest's first instruction and the run would be reported as an "
        "exit-code mismatch against the manifest rather than as the missing "
        "package it is (D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY). Not found at '"
      + (hostRoot / interpRelative).generic_string() + "'; searched: "
      + searched
      + ". Fix: install the guest's cross RUNTIME — on Debian/Ubuntu the "
        "libc6-*-cross or gcc-" + guestArch + "-linux-gnu packages, which "
        "populate /usr/" + guestArch + "-linux-gnu — or export "
        "QEMU_LD_PREFIX=<guest sysroot> yourself.";
    return out;
}

// The operator's QEMU_LD_PREFIX as it stood BEFORE this process wrote one.
//
// Snapshotted once because the alternative — re-reading the live variable —
// cannot tell an operator's deliberate value from THIS harness's own earlier
// write, and a process that spawns two different guest architectures would
// then serve the second one the first one's sysroot. The snapshot is taken on
// the first emulated spawn, which is strictly before any write below.
[[nodiscard]] inline std::string const& operatorSuppliedQemuLdPrefix() {
    static std::string const value = [] {
        char const* const raw = std::getenv("QEMU_LD_PREFIX");
        return (raw != nullptr) ? std::string{raw} : std::string{};
    }();
    return value;
}

}  // namespace detail

// Establish QEMU_LD_PREFIX for an emulated spawn, or say — in one sentence
// naming the variable — why the spawn must not happen.
//
// Returns EMPTY when the spawn may proceed, which covers four different
// "nothing to do" answers as well as the one that did work: no launcher, a
// launcher that is not qemu-user, an operator-supplied prefix, a guest with
// no interpreter to resolve, and the derive-and-export path.
//
// Non-empty is a REFUSAL and the caller must not spawn. Refusing is the point:
// the alternative is not a working run, it is the same failure 468 times over
// with its one cause named nowhere in the text.
//
// `hostRoot` / `sysrootSearchRoot` are parameters so the derivation is
// pinnable on a host that has no cross sysroot (and on one where creating
// `/usr/<triple>` would need root). Production callers take the defaults.
[[nodiscard]] inline std::string
ensureQemuGuestSysroot(std::vector<std::string> const& launcherPrefix,
                       std::filesystem::path const&    binaryPath,
                       std::filesystem::path const&    hostRoot          = "/",
                       std::filesystem::path const&    sysrootSearchRoot = "/usr") {
    // Both checks run BEFORE the snapshot, so a spawn with nothing to do with
    // qemu-user neither reads the environment nor freezes a value it will
    // never use. `decideQemuGuestSysroot` repeats them because it is also
    // called directly; here they exist to keep the snapshot's timing a
    // property of the FIRST EMULATED spawn rather than of the first spawn.
    if (launcherPrefix.empty()) return {};
    if (detail::qemuUserGuestArch(launcherPrefix.front()).empty()) return {};

    auto const decision = detail::decideQemuGuestSysroot(
        launcherPrefix, binaryPath, detail::operatorSuppliedQemuLdPrefix(),
        hostRoot, sysrootSearchRoot);
    if (!decision.diagnostic.empty()) return decision.diagnostic;
    if (decision.sysroot.empty())     return {};

#if defined(_WIN32)
    ::_putenv_s("QEMU_LD_PREFIX", decision.sysroot.c_str());
#else
    ::setenv("QEMU_LD_PREFIX", decision.sysroot.c_str(), /*overwrite=*/1);
#endif
    return {};
}

// ─── Timeout budgets (TF-C84) ──────────────────────────────────────────
// `kRunBudget` (how long the COMPILED PROGRAM may take to terminate — the
// hang detector) and `kAdmissionBudget` (how long the OPERATING SYSTEM may
// take to ADMIT a freshly written binary, before a single instruction of the
// program has run) are DEFINED in `test_wait_budget.hpp`, included at the top
// of this file, together with the macOS admission-latency table that sized
// them and the syspolicyd/Gatekeeper mechanism that explains the split. They
// live there because that header is the suite's ONE budget vocabulary; the
// SHAPE they produce — the two-phase warm-up-then-time spawn — is documented
// at `runBinary` below, which is the code that implements it.

// ─── D-TEST-RUN-HARNESS-DEADLINE-COUNTS-HOST-SUSPEND ───────────────────────
//
// EVERY BUDGET ABOVE IS A QUESTION ABOUT AWAKE TIME. "Has this child made
// progress" cannot be answered on a clock that ran while the machine was
// asleep: a suspended laptop is not the child's fault, and billing the child
// for it SIGKILLs a program that was doing exactly the work it was asked to.
//
// ⚠ `std::chrono::steady_clock` IS NOT THAT CLOCK EVERYWHERE, and the two
// platforms this project runs are OPPOSITE, so the name cannot be reasoned
// from — only measured:
//
//   ✔MEASURED 2026-08-13, macOS 26.5.2 / Darwin 25.5.0 arm64, one process
//     reading five clocks across a live nap: `wall=100.3 steady=100.3
//     mach_absolute=92.4 CLOCK_MONOTONIC=100.3 CLOCK_UPTIME_RAW=92.4`. Darwin
//     has TWO continuous monotonic clocks and one awake one, and a nap moves
//     both continuous ones by the whole nap — six live naps caught by a 1 Hz
//     watcher on this host, of which the largest: `wall=911.413
//     CLOCK_MONOTONIC=911.413 CLOCK_MONOTONIC_RAW=911.445` against
//     `CLOCK_UPTIME_RAW=1.010` and `time.monotonic=1.010`, i.e. one tick.
//     Absolute readings, same host: `steady 174003.989923` vs
//     `CLOCK_MONOTONIC_RAW 174003.989948` (25 ns apart) vs `CLOCK_MONOTONIC
//     173995.163449` vs `CLOCK_UPTIME_RAW 27855.511944`.
//     ⚠ SO libc++'s `steady_clock` HERE IS `CLOCK_MONOTONIC_RAW`, NOT
//     `CLOCK_MONOTONIC` — the two are 8.8 s apart by NTP adjustment, and an
//     earlier reading of this same defect named the wrong one because a
//     nap-DELTA cannot tell them apart (both advance by the nap). It does not
//     change the defect by one line: the clock `steady_clock` resolves to
//     COUNTS SUSPEND, and `CLOCK_UPTIME_RAW` is 40 hours behind it on this
//     laptop. The pin therefore asserts the PROPERTY against the kernel —
//     steady_clock is one of the continuous ids and is not the awake one —
//     rather than pinning a mapping libc++ is free to change.
//   ✔CONSEQUENCE MEASURED, same host, same child: a run killed at `child timed
//     out after 120000 ms` having taken 421710 ms of wall, because the child —
//     CPython, bounded on `time.monotonic()`, which on Darwin is
//     `mach_absolute_time` and does NOT count sleep — and this parent disagreed
//     about what a second is by the length of the nap.
//   ⓘ LINUX IS ALREADY CORRECT and needs no arm: its `CLOCK_MONOTONIC` EXCLUDES
//     suspend (`CLOCK_BOOTTIME` is the one that includes it), and libstdc++'s
//     `steady_clock` is `CLOCK_MONOTONIC`. ✔MEASURED the same day on the WSL2
//     leg: both ids exist and are distinct (1 and 7), reading 91644.442731 and
//     91644.442742 s — that VM has recorded zero suspend, so the difference is
//     documented rather than witnessed there.
//   ⚠ WINDOWS IS UNMEASURED, DELIBERATELY SAID RATHER THAN ASSUMED. The Windows
//     arm does not use this clock at all — `WaitForSingleObject` does its own
//     timing inside the kernel — and whether ITS timeout is charged for modern
//     standby is not known, because inducing modern standby from a test run is
//     not practical on the operator's own box. An honest "unmeasured" beats an
//     invented table row; this project has paid for closed-vocabulary-by-
//     assumption before. `AwakeClock` is still DEFINED there (it is
//     `QueryPerformanceCounter`, whose own standby behaviour is equally
//     unmeasured) so the pin compiles and states exactly that on every leg.
//
// ★ IT IS ITS OWN CLOCK TYPE ON PURPOSE. `time_point<AwakeClock>` will not
// compare against a `time_point<steady_clock>`, so a future edit that computes
// the deadline from `steady_clock::now()` again does not silently regress — it
// fails to compile. That is the only binding available: a suspend cannot be
// induced from inside a test, so the pin can assert WHICH CLOCK is selected
// (tests/test_support/test_run_binary_deadline_clock.cpp) but never the killing
// behaviour across a real nap.
struct AwakeClock {
    using duration   = std::chrono::nanoseconds;
    using rep        = duration::rep;
    using period     = duration::period;
    using time_point = std::chrono::time_point<AwakeClock>;
    static constexpr bool is_steady = true;

    [[nodiscard]] static time_point now() noexcept {
#if defined(__APPLE__)
        // Returns 0 only on failure, and CLOCK_UPTIME_RAW has been served since
        // 10.12. Aborting rather than falling back to steady_clock is the
        // project's fatal pattern and the right direction here: a silent
        // fallback would reinstate exactly the defect this type exists to
        // remove, on the one platform that has it.
        std::uint64_t const nanos = ::clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (nanos == 0u) {
            std::fputs("dss::test_support::AwakeClock fatal: "
                       "clock_gettime_nsec_np(CLOCK_UPTIME_RAW) failed\n",
                       stderr);
            std::abort();
        }
        return time_point{duration{static_cast<duration::rep>(nanos)}};
#else
        return time_point{std::chrono::duration_cast<duration>(
            std::chrono::steady_clock::now().time_since_epoch())};
#endif
    }
};

// How the child's stdout/stderr are wired.
enum class ChildStdio {
    Inherit,      // child writes to the test runner's own stdio
    CapturePipe,  // merged stdout+stderr drained into RunResult::capturedStdout
    Discard,      // routed to the null device — used by the admission warm-up
};

namespace detail {

// Spawn `binaryPath` ONCE and wait up to `timeout` for it to exit.
// Returns the captured exit code or a diagnostic. The child runs
// with no arguments.
//
// `captureStdout=false` (default): the child inherits the
// parent's stdio handles; its stdout/stderr appear in the test
// runner's output. Existing exit-code-only pins use this mode.
//
// `captureStdout=true`: STDOUT + STDERR are redirected to an
// anonymous pipe (merged — both streams land in the same
// buffer). After the child exits, the harness drains the read
// end and stores the bytes in `RunResult.capturedStdout`. STDIN
// is left attached to the parent's handle (no current pin reads
// stdin; we anchor the split-pipe variant as
// `D-RUN-HARNESS-STDIO-SPLIT` if a future case needs separate
// stdout/stderr streams).
//
// `programArgs` (UCRT-P4): arguments appended AFTER `binaryPath`, i.e. the
// program's own `argv[1..]`. EMPTY by default, which is byte-identical to every
// pre-UCRT-P4 spawn (the child saw argc == 1). It exists because the pe
// program-entry argv spine's REAL INPUT PATH is the OS command line: a test that
// hands the program a re-typed vector is not testing the mechanism at all, and the
// examples runner has no argv knob, so `argc == 1` was the only observable any
// harness could produce. DISTINCT FROM `launcherPrefix`, which goes BEFORE the
// binary and belongs to the EMULATOR, not the program; the two compose (an
// emulated run still passes the program its own arguments).
//
// `launcherPrefix` (D-LK10-ENTRY-ARM64, v0.0.2 V2-1): an optional
// argv prefix prepended ahead of `binaryPath`. EMPTY by default —
// the binary is exec'd directly (byte-identical to the pre-V2-1
// behavior; every existing exit-code/stdout pin is unaffected).
// NON-EMPTY runs the binary under an emulator, e.g. {"<full path
// to>/qemu-aarch64"} so an AArch64 ELF executes on an x86_64 host.
// `launcherPrefix[0]` is the program actually exec'd (a full path —
// the caller resolves it on PATH first); the binary becomes its
// argument. AGNOSTIC: the caller supplies the launcher; this harness
// has no per-arch knowledge.
[[nodiscard]] inline RunResult
spawnAndWait(std::filesystem::path const&     binaryPath,
             std::chrono::milliseconds        timeout,
             ChildStdio                       stdio,
             std::vector<std::string> const&  launcherPrefix,
             std::vector<std::string> const&  programArgs) {
    RunResult out;
    // The pipe plumbing below is written in terms of this predicate; the
    // Inherit and Discard modes differ only in how the child's handles are
    // pre-wired, and neither produces bytes for RunResult::capturedStdout.
    bool const captureStdout = (stdio == ChildStdio::CapturePipe);

    // Large-frame corpus needs a generous child stack (once per process,
    // before the first spawn). See ensureGenerousSpawnStack above.
    ensureGenerousSpawnStack();

    // …and an emulated child needs a guest loader it can actually find. Both
    // arms of the platform fork below are past the point where a missing
    // sysroot could still be reported as anything but the child's own exit
    // code, so the refusal has to happen HERE — before a handle exists, and
    // before anything has been asked of the OS. See the
    // D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY block above ensureQemuGuestSysroot.
    if (std::string prerequisite =
            ensureQemuGuestSysroot(launcherPrefix, binaryPath);
        !prerequisite.empty()) {
        out.diagnostic = std::move(prerequisite);
        return out;  // spawned == false — the caller poisons the arm and prints this
    }

#if defined(_WIN32)
    auto const pathStr = binaryPath.string();
    if (pathStr.empty()) {
        out.diagnostic = "runBinary: empty path";
        return out;
    }

    // ── D-TEST-RUN-BINARY-ARGV-QUOTING-UNESCAPED ──────────────────────────────
    // ONE argv vector → ONE command line, through the SHARED quoter (see the
    // include note at the top of this file). The vector is exactly the POSIX
    // arm's `argStrings`: [launcherPrefix..., binary, programArgs...], so both
    // arms now describe the child's argv the same way and differ only in how the
    // OS is handed it.
    //
    // The path element is `u8string()`, NOT `string()`. The shared quoter takes
    // UTF-8 by contract and decodes STRICTLY (it reports rather than
    // transliterating), while `path::string()` is the host's NARROW encoding —
    // UTF-8 under libstdc++, but the ANSI code page under MSVC, where a scratch
    // directory under a non-ASCII user name (`C:\Users\José\AppData\...`, which
    // `temp_directory_path()` really does produce) would arrive as bytes that are
    // not valid UTF-8. `u8string()` is UTF-8 on every toolchain.
    //
    // Built HERE, before any handle exists, so a malformed-UTF-8 report is a
    // plain early return with nothing to clean up.
    auto const        pathU8 = binaryPath.u8string();
    std::string const pathUtf8(reinterpret_cast<char const*>(pathU8.c_str()),
                               pathU8.size());

    std::vector<std::string> spawnArgv;
    spawnArgv.reserve(launcherPrefix.size() + 1u + programArgs.size());
    for (auto const& p : launcherPrefix) spawnArgv.push_back(p);
    spawnArgv.push_back(pathUtf8);
    for (auto const& a : programArgs) spawnArgv.push_back(a);

    std::wstring cmdline;
    std::string  encodeError;
    if (!::dss::substrate::tryBuildWindowsCommandLine(spawnArgv, cmdline,
                                                      encodeError)) {
        // The reporting form, not the aborting one: this harness HAS a
        // diagnostic channel, and a test that fed the child undecodable bytes
        // should see that sentence rather than a dead process.
        out.diagnostic = "runBinary: argv is not valid UTF-8 and cannot be "
                         "encoded as a Windows command line ("
                       + encodeError + ")";
        return out;
    }

    // `lpApplicationName` — the emulator when there is a launcher prefix, else
    // the binary itself (byte-identical routing to the pre-share code, which
    // picked `launcherPrefix.front()` or `pathStr`). The binary's wide form
    // comes straight off the `path`, so it needs no decode at all; the launcher
    // is a caller-supplied `std::string` and goes through the SAME strict
    // decoder as the command line, because two wide strings handed to one
    // `CreateProcessW` call must not be produced by two different widenings.
    std::wstring appName;
    if (launcherPrefix.empty()) {
        appName = binaryPath.wstring();
    } else if (!::dss::substrate::decodeUtf8ToWide(launcherPrefix.front(),
                                                   appName, encodeError)) {
        out.diagnostic = "runBinary: launcher path '" + launcherPrefix.front()
                       + "' is not valid UTF-8 (" + encodeError + ")";
        return out;
    }

    // Pipe for capturing child stdout (+ stderr merged). The
    // WRITE end is marked inheritable via SECURITY_ATTRIBUTES so
    // CreateProcess's `bInheritHandles=TRUE` propagates it to
    // the child; the READ end is then explicitly turned non-
    // inheritable via SetHandleInformation so the child can't
    // accidentally inherit it (which would keep the pipe alive
    // past the child's exit and stall our ReadFile-until-EOF
    // loop in the parent).
    HANDLE pipeRead  = nullptr;
    HANDLE pipeWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    if (captureStdout) {
        if (!::CreatePipe(&pipeRead, &pipeWrite, &sa, 0u)) {
            out.diagnostic = "CreatePipe failed (GetLastError="
                           + std::to_string(::GetLastError()) + ")";
            return out;
        }
        if (!::SetHandleInformation(pipeRead, HANDLE_FLAG_INHERIT, 0)) {
            DWORD const err = ::GetLastError();
            ::CloseHandle(pipeRead);
            ::CloseHandle(pipeWrite);
            out.diagnostic = "SetHandleInformation(pipeRead, !INHERIT) "
                             "failed (GetLastError="
                           + std::to_string(err) + ")";
            return out;
        }
    }

    // Discard mode routes the child's stdout+stderr at the null device.
    // Opened INHERITABLE (shared `sa`) so CreateProcess's bInheritHandles
    // propagates it. If the open fails we simply fall back to Inherit —
    // the warm-up's only job is to exec, and noisy output is preferable to
    // failing a spawn we deliberately ignore the result of.
    HANDLE nulHandle = INVALID_HANDLE_VALUE;

    // STARTUPINFO*W* — the wide twin of the CreateProcessW below; same fields,
    // and `CreateFileA("NUL", ...)` above is untouched because a HANDLE has no
    // A/W flavour.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (captureStdout) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = pipeWrite;
        si.hStdError  = pipeWrite;
    } else if (stdio == ChildStdio::Discard) {
        nulHandle = ::CreateFileA("NUL", GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, 0, nullptr);
        if (nulHandle != INVALID_HANDLE_VALUE) {
            si.dwFlags    = STARTF_USESTDHANDLES;
            si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = nulHandle;
            si.hStdError  = nulHandle;
        }
    }
    PROCESS_INFORMATION pi{};

    // `lpCommandLine` is WRITTEN THROUGH by CreateProcessW, so it must be a
    // mutable, NUL-terminated buffer we own — never the `wstring`'s own storage
    // on a const path. (`appName` / `cmdline` were built at the top of this arm.)
    std::vector<wchar_t> commandBuffer(cmdline.begin(), cmdline.end());
    commandBuffer.push_back(L'\0');

    BOOL const ok = ::CreateProcessW(
        appName.c_str(),
        commandBuffer.data(),
        /*lpProcessAttributes*/ nullptr,
        /*lpThreadAttributes*/  nullptr,
        /*bInheritHandles*/     TRUE,
        /*dwCreationFlags*/     0,
        /*lpEnvironment*/       nullptr,
        /*lpCurrentDirectory*/  nullptr,
        &si,
        &pi);

    if (!ok) {
        DWORD const err = ::GetLastError();
        if (captureStdout) {
            ::CloseHandle(pipeRead);
            ::CloseHandle(pipeWrite);
        }
        if (nulHandle != INVALID_HANDLE_VALUE) ::CloseHandle(nulHandle);
        out.diagnostic = "CreateProcessW failed for '" + pathStr
                       + "' (GetLastError=" + std::to_string(err) + ")";
        return out;
    }
    out.spawned = true;
    // The child holds its own duplicated handle; the parent's copy is done.
    if (nulHandle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(nulHandle);
        nulHandle = INVALID_HANDLE_VALUE;
    }

    // Close the parent's copy of the write end. The child holds
    // its own duplicated handle; the pipe stays open until the
    // child exits (or closes its stdout explicitly). If we leave
    // the parent's copy open the ReadFile-until-EOF drain loop
    // below would hang forever waiting for ITSELF to close the
    // write end.
    // ── D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS ──────────────────────
    // The drain runs CONCURRENTLY with the wait, on its own thread. It used to
    // run AFTER the child exited, which deadlocks any child that outgrows the
    // pipe's kernel buffer: the child blocks in write(), the parent blocks in
    // WaitForSingleObject, and neither moves until the timeout fires and the
    // parent KILLS a child that was working correctly. MEASURED 2026-08-04 on
    // the first caller to produce more than a few bytes (the sqlite harness leg
    // resolver, ~7 KB of JSON): two spawns, 120 s timeout each, 240 s of a
    // 240.7 s test run — and the symptom is a TIMEOUT, i.e. it reads as "the
    // child hung", which is precisely backwards.
    //
    // The old code knew the buffer was bounded ("typically 4-64 KiB") and drew
    // the wrong conclusion from it: it looped to EOF so a large capture would
    // not be silently TRUNCATED, which fixes the symptom that cannot happen
    // while leaving the one that can. Truncation was never the risk — the child
    // cannot get far enough ahead to be truncated, because it is blocked.
    //
    // The drain thread owns `drainBuf`/`drainDiag`; the main thread must not
    // touch either until join(). EOF arrives when the LAST write handle closes:
    // ours above, and the child's when it exits — including when we terminate
    // it — so the thread always finishes and the join cannot hang.
    std::thread      drainThread;
    std::string      drainBuf;
    std::string      drainDiag;
    if (captureStdout) {
        ::CloseHandle(pipeWrite);
        pipeWrite = nullptr;
        drainThread = std::thread([pipeRead, &drainBuf, &drainDiag] {
            char readBuf[4096];
            for (;;) {
                DWORD bytesRead = 0;
                BOOL const rok = ::ReadFile(pipeRead, readBuf, sizeof(readBuf),
                                            &bytesRead, nullptr);
                if (rok && bytesRead > 0) {
                    drainBuf.append(readBuf, bytesRead);
                    continue;
                }
                if (!rok) {
                    DWORD const err = ::GetLastError();
                    // EOF — the child closed its write end and we have drained
                    // everything.
                    if (err != ERROR_BROKEN_PIPE) {
                        drainDiag = "ReadFile(pipe) failed (GetLastError="
                                  + std::to_string(err) + ")";
                    }
                    break;
                }
                // rok && bytesRead == 0: anonymous pipes return this only after
                // EOF in some configurations; treat as EOF for safety.
                break;
            }
        });
    }

    DWORD const waitMs = static_cast<DWORD>(timeout.count());
    DWORD const wr     = ::WaitForSingleObject(pi.hProcess, waitMs);
    if (wr == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1u);
        ::WaitForSingleObject(pi.hProcess, 1000u);
        out.timedOut   = true;
        out.diagnostic = "child timed out after "
                       + std::to_string(timeout.count()) + " ms";
    } else if (wr == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (::GetExitCodeProcess(pi.hProcess, &code)) {
            out.exitCode = static_cast<std::uint32_t>(code);
        } else {
            out.diagnostic = "GetExitCodeProcess failed (GetLastError="
                           + std::to_string(::GetLastError()) + ")";
        }
    } else {
        out.diagnostic = "WaitForSingleObject returned "
                       + std::to_string(wr);
    }

    if (captureStdout) {
        // The child is gone (exited or terminated), so every write handle is
        // closed and the drain thread has already seen EOF or is about to.
        drainThread.join();
        out.capturedStdout = std::move(drainBuf);
        // A drain error never overwrites a spawn/wait diagnostic: those explain
        // WHY there is nothing to read, and are the more useful message.
        if (out.diagnostic.empty()) out.diagnostic = drainDiag;
        ::CloseHandle(pipeRead);
    }

    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return out;
#else
    // POSIX arm — closes D-LK10-ENTRY-POSIX-RUN-HARNESS (2026-06-02).
    // Uses `posix_spawn` (vs fork+exec) so the harness works
    // identically on Linux + macOS without ifdef'ing process-image
    // semantics; on macOS this also avoids the `vfork` deprecation.
    //
    // Timeout policy: poll `waitpid(..., WNOHANG)` in increments
    // bounded by the remaining-timeout. Sleeping the FULL remaining
    // timeout in one shot would make a short run (< few ms) wait
    // for the whole budget; capping each sleep at 10 ms keeps short-
    // run latency bounded while still adapting to long timeouts.
    auto const pathStr = binaryPath.string();
    if (pathStr.empty()) {
        out.diagnostic = "runBinary: empty path";
        return out;
    }

    // The linker writes the emitted exec with 0644 mode (the
    // writer.cpp open() default); POSIX `posix_spawn(execve)` needs
    // executable bits or it returns EACCES. Apply 0755 idempotently
    // here so callers don't have to remember the chmod. If chmod
    // fails (e.g. permission denied on a network volume), report
    // loud — spawning would fail with a less specific error.
    if (::chmod(pathStr.c_str(), 0755) != 0) {
        out.diagnostic = "chmod('" + pathStr + "', 0755) failed: errno="
                       + std::to_string(errno);
        return out;
    }

    // Pipe for stdout+stderr capture. Read end stays in parent
    // (close-on-exec so children spawned by parent later don't
    // inherit it); write end is dup'd to the child's STDOUT_FILENO
    // + STDERR_FILENO via posix_spawn_file_actions, then closed in
    // both parent and child.
    int pipeFds[2] = {-1, -1};
    posix_spawn_file_actions_t actions{};
    bool actionsInited = false;
    if (captureStdout) {
        if (::pipe(pipeFds) != 0) {
            out.diagnostic = "pipe() failed: errno="
                           + std::to_string(errno);
            return out;
        }
        // Mark parent's read end FD_CLOEXEC so later spawns from
        // this process don't accidentally inherit it.
        int const rdFlags = ::fcntl(pipeFds[0], F_GETFD);
        if (rdFlags != -1) {
            ::fcntl(pipeFds[0], F_SETFD, rdFlags | FD_CLOEXEC);
        }
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
            out.diagnostic = "posix_spawn_file_actions_init failed: "
                             "errno=" + std::to_string(errno);
            return out;
        }
        actionsInited = true;
        // Dup write end onto child's stdout (fd 1) + stderr (fd 2).
        ::posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 1);
        ::posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 2);
        // Close the (now-redundant) original write FD in the child
        // after the dup2 so the pipe gets a clean EOF after exit.
        ::posix_spawn_file_actions_addclose(&actions, pipeFds[1]);
        // Also close the read end in the child — leaking it
        // there would keep our drain loop blocked.
        ::posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    } else if (stdio == ChildStdio::Discard) {
        // Route the child's stdout+stderr at /dev/null. Used by the
        // admission warm-up, whose output is by definition uninteresting:
        // sending it to the null device (rather than a pipe) keeps the
        // runner's own stdout clean AND cannot stall the child on a full
        // pipe, since nothing has to be drained.
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            out.diagnostic = "posix_spawn_file_actions_init failed: "
                             "errno=" + std::to_string(errno);
            return out;
        }
        actionsInited = true;
        ::posix_spawn_file_actions_addopen(&actions, 1, "/dev/null",
                                           O_WRONLY, 0);
        ::posix_spawn_file_actions_adddup2(&actions, 1, 2);
    }

    // Build argv: [launcherPrefix..., binary, nullptr]. With no prefix
    // this is exactly [binary, nullptr] (byte-identical to pre-V2-1).
    // The program exec'd is argv[0] — the emulator's full path when a
    // prefix is present (caller-resolved on PATH), else the binary.
    std::vector<std::string> argStrings;
    argStrings.reserve(launcherPrefix.size() + 1 + programArgs.size());
    for (auto const& p : launcherPrefix) argStrings.push_back(p);
    argStrings.push_back(pathStr);
    // The program's own arguments. No quoting on this arm: `posix_spawn` takes an
    // argv ARRAY, so each element crosses the exec boundary intact by construction
    // -- there is no command-line string for a space to be re-split inside.
    for (auto const& a : programArgs) argStrings.push_back(a);
    std::vector<char const*> argvVec;
    argvVec.reserve(argStrings.size() + 1);
    for (auto const& a : argStrings) argvVec.push_back(a.c_str());
    argvVec.push_back(nullptr);
    char const* const execPath = argStrings.front().c_str();

    pid_t pid = -1;
    int const rc = ::posix_spawn(
        &pid,
        execPath,
        /*file_actions*/ actionsInited ? &actions : nullptr,
        /*attrp*/        nullptr,
        // posix_spawn signature wants `char* const argv[]` — cast
        // here because the strings we own are read-only by contract;
        // the child won't mutate them across exec.
        const_cast<char* const*>(argvVec.data()),
        environ);
    if (actionsInited) {
        ::posix_spawn_file_actions_destroy(&actions);
    }
    if (rc != 0) {
        if (captureStdout) {
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
        }
        out.diagnostic = "posix_spawn('" + argStrings.front()
                       + "') failed: rc=" + std::to_string(rc);
        return out;
    }
    out.spawned = true;
    // Parent closes its write-end copy so EOF reaches the read
    // end after the child exits.
    //
    // D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS — the drain runs
    // CONCURRENTLY with the waitpid poll, for the reason spelled out on the
    // Windows arm above. A Linux pipe buffers 64 KiB by default (vs Windows'
    // 4 KiB), so this arm needs a chattier child to hit it — the bug is the
    // same bug, it just waits longer to be found, and BOTH arms are the shared
    // spawn chokepoint for the two corpus harnesses.
    std::thread drainThread;
    std::string drainBuf;
    std::string drainDiag;
    if (captureStdout) {
        ::close(pipeFds[1]);
        pipeFds[1] = -1;
        int const readFd = pipeFds[0];
        drainThread = std::thread([readFd, &drainBuf, &drainDiag] {
            char readBuf[4096];
            for (;;) {
                ssize_t const n = ::read(readFd, readBuf, sizeof(readBuf));
                if (n > 0) {
                    drainBuf.append(readBuf, static_cast<std::size_t>(n));
                    continue;
                }
                if (n == 0) break;              // EOF
                if (errno == EINTR) continue;
                drainDiag = "read(pipe) failed: errno="
                          + std::to_string(errno);
                break;
            }
        });
    }
    // Join, adopt the bytes, and close the read end. Called on EVERY exit path
    // below — a `return` that skipped it would terminate() on a joinable
    // thread's destructor, turning a reporting bug into a crash.
    auto const finishDrain = [&] {
        if (!captureStdout) return;
        drainThread.join();
        out.capturedStdout = std::move(drainBuf);
        if (out.diagnostic.empty()) out.diagnostic = drainDiag;
        ::close(pipeFds[0]);
        pipeFds[0] = -1;
    };

    // D-TEST-RUN-HARNESS-DEADLINE-COUNTS-HOST-SUSPEND — the deadline is spent on
    // AWAKE time, not on elapsed time. See the AwakeClock block above for the
    // per-OS measurements; the type is distinct from steady_clock so this pair
    // cannot be recomputed from the wrong clock without a compile error.
    auto const start    = AwakeClock::now();
    auto const deadline = start + timeout;

    int status = 0;
    bool finished = false;
    while (!finished) {
        pid_t const w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            // Child exited.
            if (WIFEXITED(status)) {
                out.exitCode =
                    static_cast<std::uint32_t>(WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                // Surface the terminating signal as a high exit
                // code (128 + signal — POSIX shell convention) so
                // the caller can distinguish "exited cleanly with
                // N" from "killed by signal N". The diagnostic
                // string carries the precise reason for the
                // strict-asserts in the examples harness.
                out.exitCode = 128u +
                    static_cast<std::uint32_t>(WTERMSIG(status));
                out.diagnostic = "child terminated by signal "
                               + std::to_string(WTERMSIG(status));
            } else {
                out.diagnostic = "waitpid returned with unknown "
                                 "status word " + std::to_string(status);
            }
            finished = true;
            break;
        }
        if (w == -1) {
            out.diagnostic = "waitpid(pid=" + std::to_string(pid)
                           + ") failed: errno=" + std::to_string(errno);
            // The child may still be alive here, so the drain thread may still
            // be blocked in read(). Kill it first so the pipe reaches EOF and
            // the join below is bounded — a waitpid failure must not become a
            // hang.
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            auto const waitDiag = out.diagnostic;
            finishDrain();
            out.diagnostic = waitDiag;
            return out;
        }
        // w == 0 → child still running.
        auto const now = AwakeClock::now();
        if (now >= deadline) {
            // Timeout — terminate the child with SIGKILL, reap it
            // so the parent doesn't leave a zombie, and report.
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            out.timedOut   = true;
            out.diagnostic = "child timed out after "
                           + std::to_string(timeout.count()) + " ms";
            // Whatever the child managed to print before the kill is EVIDENCE
            // about why it hung; the old code threw it away by closing the read
            // end without draining.
            auto const timeoutDiag = out.diagnostic;
            finishDrain();
            out.diagnostic = timeoutDiag;
            return out;
        }
        auto const remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
        // ★ THE 10 ms IS A SLEEP, NOT A DEADLINE, AND IS SPELLED INSIDE THE
        // SLEEP SO THAT STAYS TRUE BY CONSTRUCTION. It is the POLL GRANULARITY
        // of the loop above: it bounds how long this parent naps between two
        // `waitpid(WNOHANG)` probes, and `remaining` — derived from `timeout` —
        // is what bounds the wait. Nothing can red because this number is too
        // small on a slow host; a smaller value only burns more CPU polling and
        // a larger one only delays noticing an exit by that much. Hoisting it
        // to a named local would move the number OUT of the sleep and make it
        // indistinguishable, to a reader and to `check-wall-clock-in-tests`,
        // from a budget sized on the machine that wrote it.
        std::this_thread::sleep_for(
            std::min(remaining, std::chrono::milliseconds{10}));
    }

    // The child has exited, so every write end is closed and the drain thread
    // has seen (or is about to see) EOF.
    finishDrain();
    return out;
#endif
}

}  // namespace detail

// Spawn `binaryPath` and wait up to `timeout` for it to exit, after first
// letting the OS finish admitting it. See the kRunBudget / kAdmissionBudget
// commentary above for the measurements that motivate the two-phase shape.
//
// PHASE 1 — untimed admission warm-up. The binary is exec'd once with its
// output discarded and its result thrown away, bounded by kAdmissionBudget.
// This is where the one-time, per-binary cost the OS charges for a freshly
// written executable gets paid: on macOS, syspolicyd's Gatekeeper scan and
// its network round trip to Apple's notarization service; on Windows, the
// equivalent Defender first-write scan. Nothing is asserted about this run.
//
// PHASE 2 — the timed run, bounded by `timeout`. Because admission is
// already done, this window contains the program's own execution and
// essentially nothing else, so `timeout` is an honest hang detector.
//
// A program that never terminates still FAILS, and still bounded: it burns
// kAdmissionBudget in phase 1, then `timeout` in phase 2, is SIGKILLed both
// times, and phase 2 reports timedOut=true to the caller's assertion.
//
// Cost when there is nothing to warm up (Linux has no Gatekeeper, and a
// re-run binary is already admitted): one extra exec of a program that has
// just been shown to run, ~1-2 ms. That is the price of not having to
// encode per-platform policy knowledge in a test harness.
//
// `captureStdout=false` (default): the child inherits the parent's stdio
// handles; its stdout/stderr appear in the test runner's output. Existing
// exit-code-only pins use this mode.
//
// `captureStdout=true`: STDOUT + STDERR are redirected to an anonymous pipe
// (merged — both streams land in the same buffer). After the child exits,
// the harness drains the read end and stores the bytes in
// `RunResult.capturedStdout`. STDIN is left attached to the parent's handle
// (no current pin reads stdin; we anchor the split-pipe variant as
// `D-RUN-HARNESS-STDIO-SPLIT` if a future case needs separate streams).
[[nodiscard]] inline RunResult
runBinary(std::filesystem::path const&     binaryPath,
          std::chrono::milliseconds        timeout        = kRunBudget,
          bool                             captureStdout  = false,
          std::vector<std::string> const&  launcherPrefix = {},
          // UCRT-P4: the program's own `argv[1..]`. Empty = the pre-UCRT-P4
          // behaviour (argc == 1) for every existing caller.
          std::vector<std::string> const&  programArgs    = {}) {
    // Skipped under an emulator: there the program the kernel actually
    // exec's is `launcherPrefix[0]` — a long-lived system binary that was
    // admitted once, long ago — while the freshly written image is merely
    // read as DATA by the emulator and never exec'd. So there is no
    // admission cost to absorb, and warming up would only double the
    // runtime of the slowest arm in the matrix.
    if (launcherPrefix.empty()) {
        // The warm-up passes the SAME arguments as the timed run. It must: a
        // program given different input can take a different path, and the point of
        // phase 1 is to pay the OS's one-time admission cost for the code the timed
        // run will execute -- not for a different execution of it.
        (void)detail::spawnAndWait(binaryPath, kAdmissionBudget,
                                   ChildStdio::Discard, launcherPrefix,
                                   programArgs);
    }
    return detail::spawnAndWait(
        binaryPath, timeout,
        captureStdout ? ChildStdio::CapturePipe : ChildStdio::Inherit,
        launcherPrefix, programArgs);
}

}  // namespace dss::test_support
