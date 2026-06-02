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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

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
  #include <signal.h>
  #include <spawn.h>
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
};

// Spawn `binaryPath` and wait up to `timeout` for it to exit.
// Returns the captured exit code or a diagnostic. The child runs
// with no arguments and inherits the parent's stdio handles (its
// stdout/stderr appear in the test runner output).
[[nodiscard]] inline RunResult
runBinary(std::filesystem::path const&     binaryPath,
          std::chrono::milliseconds        timeout =
              std::chrono::milliseconds{5000}) {
    RunResult out;

#if defined(_WIN32)
    auto const pathStr = binaryPath.string();
    if (pathStr.empty()) {
        out.diagnostic = "runBinary: empty path";
        return out;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CreateProcessA's `lpCommandLine` is a writable buffer. Quote
    // the path so spaces in the cwd (e.g. "C:\Users\First Last\..."
    // on dev hosts; ScratchDir resolves under temp_directory_path()
    // which is environment-dependent) don't cause CreateProcess to
    // parse the first space-delimited token as argv[0] for the
    // child. Code-reviewer M1 at Slice C audit fold.
    std::string cmdline = "\"" + pathStr + "\"";

    BOOL const ok = ::CreateProcessA(
        pathStr.c_str(),
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
        out.diagnostic = "CreateProcessA failed for '" + pathStr
                       + "' (GetLastError=" + std::to_string(err) + ")";
        return out;
    }
    out.spawned = true;

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

    char const* const argv[] = {pathStr.c_str(), nullptr};
    pid_t pid = -1;
    int const rc = ::posix_spawn(
        &pid,
        pathStr.c_str(),
        /*file_actions*/ nullptr,
        /*attrp*/        nullptr,
        // posix_spawn signature wants `char* const argv[]` — cast
        // here because the strings we own are read-only by contract;
        // the child won't mutate them across exec.
        const_cast<char* const*>(argv),
        environ);
    if (rc != 0) {
        out.diagnostic = "posix_spawn('" + pathStr + "') failed: rc="
                       + std::to_string(rc);
        return out;
    }
    out.spawned = true;

    auto const start  = std::chrono::steady_clock::now();
    auto const deadline = start + timeout;

    int status = 0;
    while (true) {
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
            return out;
        }
        if (w == -1) {
            out.diagnostic = "waitpid(pid=" + std::to_string(pid)
                           + ") failed: errno=" + std::to_string(errno);
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
            return out;
        }
        auto const remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
        auto const slice = std::min(
            remaining, std::chrono::milliseconds{10});
        std::this_thread::sleep_for(slice);
    }
#endif
}

}  // namespace dss::test_support
