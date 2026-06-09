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

// Spawn `binaryPath` and wait up to `timeout` for it to exit.
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
runBinary(std::filesystem::path const&     binaryPath,
          std::chrono::milliseconds        timeout =
              std::chrono::milliseconds{5000},
          bool                             captureStdout = false,
          std::vector<std::string> const&  launcherPrefix = {}) {
    RunResult out;

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

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (captureStdout) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = pipeWrite;
        si.hStdError  = pipeWrite;
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
        out.diagnostic = "CreateProcessA failed for '" + pathStr
                       + "' (GetLastError=" + std::to_string(err) + ")";
        return out;
    }
    out.spawned = true;

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
        /*file_actions*/ captureStdout ? &actions : nullptr,
        /*attrp*/        nullptr,
        // posix_spawn signature wants `char* const argv[]` — cast
        // here because the strings we own are read-only by contract;
        // the child won't mutate them across exec.
        const_cast<char* const*>(argvVec.data()),
        environ);
    if (captureStdout && actionsInited) {
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

}  // namespace dss::test_support
