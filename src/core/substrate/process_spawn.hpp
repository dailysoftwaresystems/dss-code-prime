#pragma once

#include "core/export.hpp"

// `dss::substrate::buildWindowsCommandLine` USED TO BE DECLARED HERE and is
// still part of this facility's surface — including this header still gets it,
// so every existing caller is untouched. It lives in its own HEADER-ONLY,
// DEPENDENCY-FREE file because a test-tier consumer that links no DSS library
// needs the same algorithm; see that header's opening note, and the
// "two spawn implementations" section below.
#include "core/substrate/windows_command_line.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// process_spawn — the compiler's FIRST process-spawn substrate.
//
// WHY this exists (host/OS utility — NOT a language/CPU/format concept).
// Until now `src/` contained ZERO process-creation call sites: the compiler
// read bytes, computed, and wrote bytes. Two consumers change that:
//   * user build scripts — a project config's `{"run": ["bash","gen.sh"]}`
//     step, which must execute BEFORE the sources it generates are compiled;
//   * dependency acquisition — invoking `git` to fetch a declared dependency.
// Both hand us an argv VECTOR that already came from user data (a config
// file, a URL, a Windows path), and both need exactly one thing from the OS:
// "run this program, let me see its output, tell me how it exited."
//
// THE SURFACE IS DELIBERATELY THREE FUNCTIONS. An earlier draft proposed a
// `SpawnHandle` lifecycle (`spawnStart` / `spawnWait` / `spawnKill` /
// `spawnDrainStdout` / `spawnClose`) plus a `SpawnStdio` enum with
// `CapturePipe` and `Discard` arms. An audit ruled that a SPECULATIVE BUILD:
// no consumer this cycle needs a non-blocking handle, a kill, a timeout, or a
// captured pipe, and an unused async API is an unused async API whether or
// not it is written carefully. Add an arm the day a caller needs it, with its
// own pin. What ships here is the intersection of what both real consumers
// need and nothing else. `detail::interpretExecHandshake` further down is NOT
// a fourth entry point walking back that decision — it is one DECISION lifted
// out of `spawnAndWaitInherit`'s POSIX arm so that it can be pinned; see the
// note above it for why that extraction had to happen.
//
// ★ NO SHELL, EVER. `argv[0]` is executed DIRECTLY — `CreateProcessW` on
// Windows, `posix_spawn` on macOS, `fork` + `execv` elsewhere on POSIX (the
// .cpp's opening note says which host takes which and why). There is no
// `std::system`, no `popen`,
// no `cmd /c`, no `/bin/sh -c`, and no code path that can grow one. The
// remaining elements of `argv` reach the child BYTE-IDENTICALLY: no glob
// expansion, no `$VAR` / `%VAR%` substitution, no `&&` / `;` splitting, no
// word splitting on whitespace. This is not an incidental property of the
// implementation — it is the security contract of the whole facility, because
// every consumer interpolates user-supplied text (a dependency URL, a script
// path) into `argv`. `tests/core/substrate/test_process_spawn.cpp` asserts it
// directly by round-tripping an argument containing `$HOME`, `%PATH%`,
// `&& echo pwned` and `;` through a real spawn and comparing bytes.
//
// AGNOSTIC: this facility names no source language, no target processor and
// no object format. The `_WIN32` / POSIX `#ifdef` inside the .cpp is pure
// host-portability (which process API the host offers), exactly like the
// sibling `large_stack_call.cpp`'s thread-API split.
//
// FAIL LOUD, NEVER SWALLOW. Every OS failure carries the platform's own error
// text (`GetLastError` / `errno` + `strerror`) into `SpawnResult::diagnostic`.
// Nothing here returns a plausible-looking success it did not achieve.
//
// ── WHY TWO SPAWN IMPLEMENTATIONS LEGITIMATELY COEXIST ────────────────────
// `tests/test_support/run_binary.hpp` also spawns processes, and a future
// reader will be tempted to "clean that up" by deleting one of them. DO NOT.
// They are not two implementations of one thing; they are two DIFFERENT
// things that share a system call:
//
//   THIS FILE (shipped library, `dss-code-prime-lib`) has NO timeout, NO
//   kill, NO emulator/launcher prefix, NO `RLIMIT_STACK` / `QEMU_STACK_SIZE`
//   manipulation, NO pipe drain, and NO warm-up exec. It blocks until the
//   child exits, because a build step that hangs is the user's build step
//   hanging and the compiler has no business inventing a deadline for it, and
//   because "wait for the tool, inherit its stdio" is the entire requirement.
//
//   `run_binary.hpp` (test tier only) is ~850 lines of TEST POLICY: a run
//   budget vs. a separate OS-admission budget (macOS Gatekeeper's per-new-
//   signature network scan), a concurrent pipe drain that exists to avoid a
//   measured parent/child deadlock, an emulator argv prefix so an AArch64 ELF
//   runs on an x86_64 host, and a stack-limit bump for the large-frame corpus.
//   Every one of those is a decision about how the TEST SUITE judges an
//   emitted binary. None of it belongs in a compiler shipped to users, and
//   folding it in would put a 5-second kill timer on the user's build script.
//
// ✅ THE QUOTING IS NOW SHARED; THE SPAWN MACHINERY IS NOT — and that split is
// the whole point. The ONE thing the two implementations SHOULD share is argv
// quoting, which has exactly one correct answer per platform, and they now do:
// `buildWindowsCommandLine` lives in `core/substrate/windows_command_line.hpp`
// and BOTH `spawnAndWaitInherit` (below, via the reporting
// `tryBuildWindowsCommandLine`) and `run_binary.hpp`'s Windows arm call it.
// There is exactly ONE implementation of the algorithm in the repository.
// `run_binary.hpp` used to carry a second, half-correct copy that escaped
// neither embedded quotes nor trailing backslashes and said so in a comment
// (D-TEST-RUN-BINARY-ARGV-QUOTING-UNESCAPED); that copy is gone.
//
// HOW IT IS SHARED WITHOUT A LINK — and why it is NOT shared by linking. The
// obvious wiring fails: `integrated_tests` (`integrated_tests/runner.cpp`)
// includes `run_binary.hpp`, but its target (`integrated_tests/CMakeLists.txt`)
// links ONLY `nlohmann_json` and puts only `tests/test_support` on its include
// path. That is DELIBERATE — it drives the CLI as a black box, and linking
// `dss-code-prime-lib` into it to borrow a string helper would break exactly the
// boundary the target exists to enforce. So the quoter was made header-only and
// dependency-free instead (a pure `vector<string>` → `wstring` transform has
// nothing to link), and `run_binary.hpp` includes it by relative path. The
// `integrated_tests` link graph is untouched. The same trick, for the same
// reason, as `src/program/platform_token.hpp` ↔ `arm_verdict_ledger.hpp`.
//
// What is NOT shared, and must not become shared, is everything in the two
// paragraphs above: the timeout, the kill, the emulator prefix, the stack
// policy, the concurrent drain, the warm-up exec. Those are test-tier judgment
// about how the suite grades an emitted binary; the quoter is arithmetic.

namespace dss::substrate {

// Resolve `command` against PATH the way OS process creation would.
// `nullopt` = not found (the caller must NOT then try to spawn it anyway).
//
// SEARCH ORDER
//   * If `command` contains a directory separator ('/', or '\' and a drive
//     prefix on Windows) it is NOT a PATH lookup at all — the path is taken
//     as given (relative paths resolve against the current directory), and
//     only the extension probing below applies. This mirrors `execvp` and
//     `CreateProcess`, both of which skip the search once a path is present.
//   * Otherwise each `PATH` entry is tried in order, left to right. Empty
//     entries are SKIPPED — POSIX `execvp` treats an empty entry as the
//     current directory, and we deliberately do not: see below.
//
// ★ THE CURRENT DIRECTORY IS NEVER SEARCHED. Windows' `CreateProcess` search
// order includes the application directory and the current directory ahead of
// PATH; we deliberately omit both. A consumer that resolves `git` must get
// the `git` the user installed, not a `git.exe` that happened to be dropped
// into the project directory being compiled — and the project directory is
// precisely where untrusted content lives for a compiler. Callers wanting a
// local tool pass a path (which contains a separator, and skips the search).
//
// EXTENSION PROBING (Windows). A bare `git` is not a filename on Windows;
// `git.exe` is. Candidates are, in order: the name AS GIVEN if it already has
// an extension, then the name with each `PATHEXT` extension appended (falling
// back to the documented default `.COM;.EXE;.BAT;.CMD` when `PATHEXT` is
// unset). NOTE the honest edge: `PATHEXT` names `.BAT` and `.CMD`, which are
// resolvable FILES but are NOT executable images — `CreateProcess` cannot run
// them without `cmd.exe`, and this facility never spawns a shell. Such a hit
// is therefore reported by the resolver and then fails LOUD at spawn time
// with the OS's own error text, rather than being silently dropped here (a
// resolver that hid them would report "git not found" for a `git.cmd`
// shim, which is a strictly worse diagnostic than the real one).
// On POSIX there is no extension probing and a candidate must additionally
// pass `access(X_OK)`.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
resolveExecutableOnPath(std::string const& command);

// `buildWindowsCommandLine` / `tryBuildWindowsCommandLine` / `decodeUtf8ToWide`
// are declared in `core/substrate/windows_command_line.hpp`, included above —
// they are part of this facility and this namespace, they are just kept
// link-free so a test-tier consumer can use them. `spawnAndWaitInherit` below
// uses the reporting form so a malformed-UTF-8 `argv` surfaces through
// `SpawnResult::diagnostic` instead of aborting.

// The outcome of one spawn attempt.
//
// ★ `spawned` AND `exitCode` ANSWER TWO DIFFERENT QUESTIONS, and conflating
// them is the mistake this struct exists to prevent. `spawned == false` means
// the OPERATING SYSTEM NEVER CREATED THE PROCESS — argv[0] was not found, was
// not an executable image, the working directory did not exist, or the OS
// refused for a reason in `diagnostic`. `spawned == true` with a non-zero
// `exitCode` means the program RAN and FAILED. The caller emits two
// remediation-distinct diagnostics off this boolean ("install git / check
// PATH" vs. "the build script reported an error"), which is why spawn failure
// is NOT signalled through a magic exit code like 127: 127 is a value a real
// program can genuinely return, so a caller could never tell the two apart.
struct SpawnResult {
    bool        spawned  = false;  // the OS created the process
    int         exitCode = 0;      // meaningful only when `spawned`
    std::string diagnostic;        // non-empty on every failure; OS text
};

// Spawn `argv[0]` DIRECTLY (see the NO SHELL note above), passing `argv` as
// the child's argument vector, and BLOCK until it exits.
//
//   * stdio is INHERITED — the child writes to the compiler's own stdout and
//     stderr, and reads the compiler's stdin. A build script's output is the
//     user's output and belongs on the user's terminal, interleaved live
//     rather than buffered and replayed.
//   * `cwd` empty (the default) = inherit the caller's current directory.
//     Otherwise the child starts in `cwd`; the PARENT's current directory is
//     never changed (which is what makes this safe to call from the
//     per-compilation-unit thread pool — no process-global `chdir`).
//   * NO TIMEOUT, by design. See the two-implementations note above.
//
// EXIT CODE. On Windows this is the process exit code as returned by
// `GetExitCodeProcess`. On POSIX a normal exit reports `WEXITSTATUS`; a child
// KILLED BY A SIGNAL reports `128 + signo` (the shell convention) AND sets
// `diagnostic` to name the signal, so a segfaulting build step is never
// mistaken for a clean exit.
//
// ★ AND THE POSIX RANGE IS EIGHT BITS, WHICH MAKES THIS FIELD HOST-DEPENDENT
// ABOVE 255. `WEXITSTATUS` is defined as the LOW 8 BITS of the wait status, so
// a hook that exits 256 reports 0 on Linux and macOS while Windows reports the
// full DWORD 256 — the SAME manifest then passes the build on one host and
// aborts it on another, and the passing host is the one that lost information.
// This is inherent to POSIX `wait`, not something this layer can repair: the
// upper bits never leave the kernel, so there is nothing here left to widen,
// and inventing a "probably truncated" heuristic would be guessing. It is
// documented rather than fixed so the asymmetry is a known property instead of
// a field report. A build step that wants a portable verdict must stay inside
// 0..255 — which is also all a POSIX shell can observe of it.
//
// `argv` must be non-empty; an empty vector is a caller bug and is reported
// as a spawn failure with a diagnostic rather than aborting, because every
// other failure on this path is reportable and one abort in the middle of
// them would be a surprise.
[[nodiscard]] DSS_EXPORT SpawnResult
spawnAndWaitInherit(std::vector<std::string> const& argv,
                    std::filesystem::path const&    cwd = {});

// ── The exec-status handshake, decided as a value (POSIX only) ─────────────
//
// ★ WHY THIS IS LIFTED OUT AT ALL, WHICH IS THE WHOLE POINT. On POSIX,
// `spawnAndWaitInherit` learns whether `execv` happened from a CLOEXEC
// self-pipe: EOF means the exec took, a whole record means it did not. Reading
// that pipe and INTERPRETING what came back used to be one fused block — and
// the arm no test could reach was precisely the arm that was wrong. A SHORT
// read, or a `read` that failed for anything other than EINTR, fell through the
// "did we get a whole record" check and set `spawned = true`; `waitpid` then
// reaped the child's own `_exit(127)` and the caller told the operator the
// script "ran and exited with status 127 — the script's own verdict", about a
// program that may never have executed an instruction. That is the exact
// confusion `SpawnResult::spawned` exists to prevent, reached from the other
// side. Nothing a caller can pass through the public surface forces a partial
// read, so the bug was structurally invisible to the suite and a fix left fused
// with the I/O would have been just as invisible — deletable by any future
// refactor with the suite still green.
//
// So the decision is a PURE function of what the parent observed: no
// descriptor, no syscall, no global state, no allocation beyond the message it
// returns. It is `detail` and not surface — the "three functions" note at the
// top of this file still describes what a consumer calls.
//
// ★ AND IT IS DECLARED ON EVERY POSIX HOST, INCLUDING ONES WITH NO CALLER.
// macOS spawns through `posix_spawn`, which reports exec failure through its
// return value and reads no pipe, so nothing in `src/` calls this there. The
// declaration is NOT narrowed to the hosts that use it, because narrowing it
// would force its pins to be narrowed identically — and a pin that quietly does
// not run on one platform is the masked coverage this project treats as a
// defect. The function is pure, so compiling it everywhere costs one symbol.
#if !defined(_WIN32)

namespace detail {

// The record the forked child writes to the exec-status pipe immediately before
// `_exit`, and the only thing the parent ever learns about a child that never
// became the program. Deliberately small so it fits one sub-PIPE_BUF write: the
// kernel then delivers it whole or not at all, rather than in pieces that could
// tear across two reads.
struct ChildFailure {
    char stage;  // 'C' = chdir, 'X' = execv
    int  error;  // errno as the CHILD saw it
};

struct HandshakeVerdict {
    bool        spawned;
    std::string diagnostic;  // empty ONLY on the clean-exec arm
};

// What `bytesRead` bytes off the exec-status pipe (and a `readErrno`, zero when
// `read` never failed) mean:
//
//   bytesRead == 0, no error       SPAWNED. CLOEXEC closed the write end, so
//                                  the exec took and there was nothing to say.
//   bytesRead == expectedBytes     NOT spawned. `failure->stage` says whether
//                                  the child died at `chdir` or at `execv`.
//   anything else, and any
//   non-EINTR read error           NOT spawned — and the diagnostic says the
//                                  HANDSHAKE broke, not that exec failed. That
//                                  is a different fact: what was lost is the
//                                  ability to know which happened, and saying
//                                  "execv failed" would be inventing one.
//
// `failure` is dereferenced ONLY on the complete-record arm, because a torn
// record is not a record. A null pointer there is a caller bug and resolves to
// the handshake verdict — never to `spawned`. The safe direction is always the
// one where the caller is told it does not know.
[[nodiscard]] DSS_EXPORT HandshakeVerdict
interpretExecHandshake(std::size_t bytesRead, std::size_t expectedBytes,
                       int readErrno, ChildFailure const* failure,
                       std::string const& exePath, std::string const& cwdPath);

} // namespace detail

#endif  // !_WIN32

} // namespace dss::substrate
