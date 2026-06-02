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
// Stage 1 — Windows host only via `CreateProcessA` +
// `WaitForSingleObject` + `GetExitCodeProcess`. POSIX
// (`posix_spawn` + `waitpid`) anchored D-LK10-ENTRY-POSIX-RUN-
// HARNESS — Stage 1 ships the Windows-host smoke; cross-host
// running is the LK10-full hermetic acceptance gate.
//
// Caller writes the bytes to disk first via
// `dss::linker::writeImage`. Caller-side responsibility for
// permissions on POSIX + .exe extension on Windows
// (writer.hpp:27-30 documents this contract).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
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

    // CreateProcessA's `lpCommandLine` is a writable buffer.
    std::string cmdline = pathStr;

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
    // POSIX path anchored D-LK10-ENTRY-POSIX-RUN-HARNESS. When
    // LK10-full hermetic acceptance lands, this becomes:
    //   posix_spawn + waitpid with clock-based timeout loop.
    // Today: stub returns "not implemented" so non-Windows hosts
    // compile this header but tests skip via runtime check on
    // `result.spawned`.
    (void)binaryPath;
    (void)timeout;
    out.diagnostic = "runBinary: POSIX run-harness anchored "
                     "D-LK10-ENTRY-POSIX-RUN-HARNESS — Stage 1 "
                     "ships Windows host only.";
    return out;
#endif
}

}  // namespace dss::test_support
