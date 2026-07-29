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
//   * Windows — `CreateProcessA` + `WaitForSingleObject` +
//     `GetExitCodeProcess` (Stage 1 Slice C 2026-06-02).
//   * POSIX — `posix_spawn` + `waitpid` with `WNOHANG` poll loop
//     for timeout (closes D-LK10-ENTRY-POSIX-RUN-HARNESS,
//     2026-06-02). The poll loop sleeps in increments capped at
//     remaining-timeout so the harness exits promptly on short
//     runs (the examples runner spawns dozens per test cycle).
//
// Caller writes the bytes to disk first via
// `dss::linker::writeImage`. Caller-side responsibility for
// permissions on POSIX + .exe extension on Windows (the parent-
// directory contract is documented at `writer.hpp:30-34`). The
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

#include <algorithm>  // std::min in the POSIX poll-loop arm
#include <chrono>
#include <cstdint>
#include <cstdlib>    // setenv / _putenv_s (QEMU_STACK_SIZE bump)
#include <filesystem>
#include <optional>
#include <string>
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

// ─── Timeout budgets (TF-C84) ──────────────────────────────────────────
// TWO different things need bounding here, and conflating them into one
// bare `5000` literal is what made the examples suite flaky:
//
//   kRunBudget       — how long the COMPILED PROGRAM may take to
//                      terminate. This is the hang detector: an infinite
//                      loop in emitted code must trip it.
//   kAdmissionBudget — how long the OPERATING SYSTEM may take to ADMIT a
//                      freshly written binary for execution, before a
//                      single instruction of the program has run. This is
//                      not the program's cost and must not be charged to
//                      the program's budget.
//
// Why the split exists — MEASURED 2026-07-29 on macOS 26.5.2 / arm64
// T8132 (10 cpu, 4 P-cores), replicating a real emitted example binary:
//
//   per-exec latency (ms)      1st exec of a NEW file      re-exec, same file
//   serial                            265 – 303                   ~15
//   8-way concurrent            2607 med / 4899 max         394 med / 776 max
//   16-way concurrent           5095 med / 6841 max        1168 med / 2263 max
//
// The program itself runs in ~15 ms. Everything above that is the OS
// admitting the binary, and macOS's unified log names the mechanism: the
// first exec of a freshly written, ad-hoc-signed Mach-O makes syspolicyd
// run a Gatekeeper scan that issues a NETWORK REQUEST to Apple's
// notarization service before the child's main() is entered —
//   syspolicyd [syspolicy.exec] GK performScan: PST: (path: …)
//   syspolicyd [CFNetwork:Summary] transaction_duration_ms=1527, status=200
//   syspolicyd [syspolicy.exec] GatekeeperPolicyScanError Code=-67018
//                               "Code did not match any currently allowed policy"
// The scan always FAILS (our output is ad-hoc signed, never notarized) and
// the exec is then allowed anyway (no com.apple.quarantine xattr) — but the
// round trip is paid in full, every time, per NEW code-signature. Measured
// 103 such scans in 3 minutes across only 3 TLS connections: they multiplex
// over one HTTP/3 connection inside the single syspolicyd process, so they
// SERIALIZE. That is why the cost explodes with ctest -j parallelism while
// a serial rerun of the very same tests always passes.
//
// Hence the fix is NOT a bigger number: the cost being absorbed is somebody
// else's network and is therefore unbounded, whereas the thing the timeout
// exists to catch — a program that never terminates — is bounded and small.
// `runBinary` instead performs an UNTIMED warm-up exec (below) so the timed
// window measures program runtime only, and kRunBudget can stay tight.
//
// kRunBudget = 5000 ms: >2x the worst WARM latency ever measured here
// (2263 ms at 16-way concurrency, which is pure CPU contention on 4
// P-cores), and ~300x the ~15 ms a real example actually needs.
inline constexpr std::chrono::milliseconds kRunBudget{5000};

// kAdmissionBudget = 30000 ms: >4x the worst ADMISSION latency measured
// (6841 ms at 16-way concurrency). Generous on purpose — it is charged only
// when a program genuinely never terminates, and it must dominate an
// external, network-dependent cost that a slow or offline link can inflate
// (CFNetwork's own per-request timeout is 3 s, and requests queue).
inline constexpr std::chrono::milliseconds kAdmissionBudget{30000};

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
             std::vector<std::string> const&  launcherPrefix) {
    RunResult out;
    // The pipe plumbing below is written in terms of this predicate; the
    // Inherit and Discard modes differ only in how the child's handles are
    // pre-wired, and neither produces bytes for RunResult::capturedStdout.
    bool const captureStdout = (stdio == ChildStdio::CapturePipe);

    // Large-frame corpus needs a generous child stack (once per process,
    // before the first spawn). See ensureGenerousSpawnStack above.
    ensureGenerousSpawnStack();

#if defined(_WIN32)
    auto const pathStr = binaryPath.string();
    if (pathStr.empty()) {
        out.diagnostic = "runBinary: empty path";
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

    STARTUPINFOA si{};
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

    // CreateProcessA's `lpCommandLine` is a writable buffer. Quote
    // the path so spaces in the cwd (e.g. "C:\Users\First Last\..."
    // on dev hosts; ScratchDir resolves under temp_directory_path()
    // which is environment-dependent) don't cause CreateProcess to
    // parse the first space-delimited token as argv[0] for the
    // child. Code-reviewer M1 at Slice C audit fold.
    // With a launcher prefix the emulator is argv[0] (lpApplicationName)
    // and the binary becomes its trailing argument; without it the
    // binary is launched directly (byte-identical to pre-V2-1).
    std::string appName = pathStr;
    std::string cmdline;
    if (!launcherPrefix.empty()) {
        appName = launcherPrefix.front();
        for (auto const& a : launcherPrefix) {
            cmdline += "\"" + a + "\" ";
        }
        cmdline += "\"" + pathStr + "\"";
    } else {
        cmdline = "\"" + pathStr + "\"";
    }

    BOOL const ok = ::CreateProcessA(
        appName.c_str(),
        cmdline.data(),
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
        out.diagnostic = "CreateProcessA failed for '" + pathStr
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
    if (captureStdout) {
        ::CloseHandle(pipeWrite);
        pipeWrite = nullptr;
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
        // Drain pipe AFTER child exit. Buffer is bounded by the
        // pipe's default kernel allocation (typically 4-64 KiB);
        // FF6 hello-world prints ~6 bytes so a one-shot ReadFile
        // typically returns everything, but we loop until EOF
        // (ReadFile returns FALSE with GetLastError() ==
        // ERROR_BROKEN_PIPE when the child's write end is closed
        // AND the read buffer is drained — that's the EOF
        // signal) so a future large-output test doesn't get a
        // silently-truncated capture.
        char readBuf[4096];
        for (;;) {
            DWORD bytesRead = 0;
            BOOL const rok = ::ReadFile(pipeRead, readBuf,
                                        sizeof(readBuf),
                                        &bytesRead, nullptr);
            if (rok && bytesRead > 0) {
                out.capturedStdout.append(readBuf, bytesRead);
                continue;
            }
            if (!rok) {
                DWORD const err = ::GetLastError();
                if (err == ERROR_BROKEN_PIPE) {
                    // EOF — child closed its write end and we've
                    // drained everything.
                    break;
                }
                // Any other ReadFile failure: report it but keep
                // whatever we managed to capture so far.
                if (out.diagnostic.empty()) {
                    out.diagnostic = "ReadFile(pipe) failed "
                                     "(GetLastError="
                                   + std::to_string(err) + ")";
                }
                break;
            }
            // rok && bytesRead == 0: anonymous pipes return this
            // only after EOF in some configurations; treat as
            // EOF for safety.
            break;
        }
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
    argStrings.reserve(launcherPrefix.size() + 1);
    for (auto const& p : launcherPrefix) argStrings.push_back(p);
    argStrings.push_back(pathStr);
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
    if (captureStdout) {
        ::close(pipeFds[1]);
        pipeFds[1] = -1;
    }

    auto const start  = std::chrono::steady_clock::now();
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
            if (captureStdout) {
                ::close(pipeFds[0]);
            }
            return out;
        }
        // w == 0 → child still running.
        auto const now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            // Timeout — terminate the child with SIGKILL, reap it
            // so the parent doesn't leave a zombie, and report.
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            out.timedOut   = true;
            out.diagnostic = "child timed out after "
                           + std::to_string(timeout.count()) + " ms";
            if (captureStdout) {
                ::close(pipeFds[0]);
            }
            return out;
        }
        auto const remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
        auto const slice = std::min(
            remaining, std::chrono::milliseconds{10});
        std::this_thread::sleep_for(slice);
    }

    if (captureStdout) {
        // Drain pipe after child exit. read() returns 0 on EOF
        // (parent's write end + all child write ends closed).
        char readBuf[4096];
        for (;;) {
            ssize_t const n = ::read(pipeFds[0], readBuf,
                                     sizeof(readBuf));
            if (n > 0) {
                out.capturedStdout.append(readBuf,
                                          static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                // EOF.
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (out.diagnostic.empty()) {
                out.diagnostic = "read(pipe) failed: errno="
                               + std::to_string(errno);
            }
            break;
        }
        ::close(pipeFds[0]);
    }
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
          std::vector<std::string> const&  launcherPrefix = {}) {
    // Skipped under an emulator: there the program the kernel actually
    // exec's is `launcherPrefix[0]` — a long-lived system binary that was
    // admitted once, long ago — while the freshly written image is merely
    // read as DATA by the emulator and never exec'd. So there is no
    // admission cost to absorb, and warming up would only double the
    // runtime of the slowest arm in the matrix.
    if (launcherPrefix.empty()) {
        (void)detail::spawnAndWait(binaryPath, kAdmissionBudget,
                                   ChildStdio::Discard, launcherPrefix);
    }
    return detail::spawnAndWait(
        binaryPath, timeout,
        captureStdout ? ChildStdio::CapturePipe : ChildStdio::Inherit,
        launcherPrefix);
}

}  // namespace dss::test_support
