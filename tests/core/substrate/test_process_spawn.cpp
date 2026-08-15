// Direct unit tests for the process-spawn substrate
// (`src/core/substrate/process_spawn.{hpp,cpp}`) — the compiler's FIRST
// process creation. Two consumers land on it: running a user build script
// (`{"run": ["bash","gen.sh"]}`) and invoking `git` for dependency
// acquisition. Both interpolate USER-SUPPLIED text into `argv`, so the
// properties pinned here are security properties, not conveniences.
//
// ★ SELF-SPAWN FIXTURE, so the suite has no external dependency. `main` at the
// bottom intercepts `--dss-spawn-fixture` before gtest parses argv; in that
// mode the executable records its OWN working directory and its OWN argv into
// a marker file and exits with a caller-chosen code. That makes the fixture a
// property of this file rather than of the machine (Windows has no `/bin/sh`;
// a Mac has no `/proc`), and it is the same technique — and the same reason
// for owning `main` — as `tests/test_support/test_run_binary_capture.cpp`.
// Reading the child's OWN view back out of the marker is what turns "the call
// accepted a cwd argument" into "the child actually started there", and
// "we passed a hostile string" into "the child received those exact bytes".

#include "core/substrate/process_spawn.hpp"

// D-TEST-RUN-BINARY-ARGV-QUOTING-UNESCAPED. The TEST-TIER spawner, which now
// composes its Windows command line with the very quoter pinned in this file
// (`src/core/substrate/windows_command_line.hpp`) instead of its own
// half-correct copy. It is included HERE, next to the substrate's own pins,
// because this file already owns the only self-spawn fixture in the suite that
// reports the child's received argv back to the parent — so it is the one place
// the harness's quoting can be judged by what the CHILD actually got, rather
// than by re-reading the string the parent produced.
#include "run_binary.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <shellapi.h>  // CommandLineToArgvW — the round-trip ORACLE
#else
    #include <fcntl.h>   // open — the stdio-inheritance pin redirects fd 1 and 2
    #include <unistd.h>  // access(X_OK) — the permission-probe pin's own control
#endif

namespace fs = std::filesystem;

using dss::substrate::buildWindowsCommandLine;
using dss::substrate::resolveExecutableOnPath;
using dss::substrate::spawnAndWaitInherit;
using dss::substrate::spawnAndWaitRedirectStdout;
using dss::substrate::SpawnResult;
using dss::substrate::tryBuildWindowsCommandLine;

#if !defined(_WIN32)
using dss::substrate::detail::ChildFailure;
using dss::substrate::detail::HandshakeContext;
using dss::substrate::detail::interpretExecHandshake;
#endif

namespace {

// ── Fixture protocol ───────────────────────────────────────────────────────
//
// argv = { self, kFixtureFlag, <marker path>, <exit code>, payload... }
// The payload elements are recorded verbatim; the marker path is deliberately
// an ordinary argv element too, so the scratch directory's own spaces travel
// through the quoting on every single spawn test rather than only the ones
// that opt in.
constexpr std::string_view kFixtureFlag = "--dss-spawn-fixture";

#if !defined(_WIN32)
// The one payload element the fixture ACTS on instead of merely recording: it
// finishes writing its marker (so the parent can prove the child really reached
// user code) and then raises the signal named by the element after it. Only the
// signal-termination test sends it, and it is spelled distinctly enough that no
// other payload could collide with it.
constexpr std::string_view kRaiseSignalDirective = "--dss-fixture-raise-signal";

// The second acting directive: write a fixed token to stdout and a DIFFERENT
// one to stderr, so a parent that has redirected its own descriptors 1 and 2 can
// prove the child wrote through THOSE. Two tokens rather than one because a
// spawn that crossed the streams over would satisfy a single-token assertion.
constexpr std::string_view kEchoStdioDirective = "--dss-fixture-echo-stdio";
#endif

// UNCONDITIONAL, unlike the POSIX-only directive above that first used them:
// `spawnAndWaitRedirectStdout` moves stdout on BOTH hosts and must leave stderr
// where it was on both, so the same two tokens are now read by a pin that runs
// everywhere. Two distinct tokens because a spawn that crossed the streams over
// would satisfy a single-token assertion.
constexpr std::string_view kInheritedStdoutToken = "dss-fixture-wrote-stdout";
constexpr std::string_view kInheritedStderrToken = "dss-fixture-wrote-stderr";

// The stdio PROBE directive. Same two tokens as the echo directive, plus one
// extra fact the redirect arm needs and inheritance never did: whether the child
// was handed a usable STDIN. `STARTF_USESTDHANDLES` is all-or-nothing (see
// `duplicateStdHandleInheritable` in the substrate), so the natural way to get
// the Windows arm wrong is to supply hStdOutput and leave the other two zeroed —
// which closes stdin and stderr for the child while every stdout assertion stays
// green. The answer travels in the MARKER, not on a stream, because the streams
// are the thing under test.
constexpr std::string_view kProbeStdioDirective = "--dss-fixture-probe-stdio";

// The flood directive: write exactly N bytes of `floodPayload` to stdout and
// exit with the caller's code. N comes from the argv element after it.
constexpr std::string_view kFloodStdoutDirective = "--dss-fixture-flood-stdout";

// ★ MORE THAN ANY PIPE BUFFER, WHICH IS THE ENTIRE POINT OF THE HEADLINE PIN.
// Linux gives an anonymous pipe 64 KiB; a Windows anonymous pipe defaults to
// 4 KiB. A redirect built on a pipe that is drained only AFTER the wait wedges
// the moment the child writes past that — the child blocks in write, the parent
// blocks in the wait, and neither moves (measured, and documented in
// `run_binary.hpp` as D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-DEADLOCKS). 256 KiB
// clears the larger of the two by 4x, so the pin is not sitting on the boundary
// of some host's buffer size, and it stays small enough to cost milliseconds.
constexpr std::size_t kFloodBytes = 256u * 1024u;

// The flood bytes, generated by ONE function that BOTH the child and the parent
// call. "Byte-for-byte" then means "against the bytes that were actually asked
// for" rather than against a second description of them that could drift.
//
// ★ NO '\n' AND NO '\r' ANYWHERE IN IT. The child writes through the CRT's
// stdout, which on Windows is a TEXT-mode stream that would translate them —
// and this pin is about whether the redirection carried the child's bytes, not
// about newline translation. A payload that let those two questions mix would
// answer neither.
//
// The pattern SHIFTS every 64 bytes rather than repeating, so a capture that
// lost, duplicated or reordered a block is detectable at the byte where it
// happened instead of only by its length.
std::string floodPayload(std::size_t bytes) {
    constexpr std::string_view kAlphabet =
        "0123456789abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // exactly 64, none of them a newline
    static_assert(kAlphabet.size() == 64);
    std::string out;
    out.reserve(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        out.push_back(kAlphabet[(i + i / kAlphabet.size()) % kAlphabet.size()]);
    }
    return out;
}

// Is descriptor 0 / STD_INPUT_HANDLE something this process could actually read
// from? Called by the CHILD (recording its answer in the marker) and by the
// PARENT (computing what the answer has to be), so the two verdicts are produced
// by identical code and a mismatch means the spawn changed something.
bool stdinIsLive() {
#if defined(_WIN32)
    HANDLE const handle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    // `GetFileType` reports FILE_TYPE_UNKNOWN both for a dead handle and for a
    // live one of a type it cannot name, and the documented way to separate them
    // is the last-error channel — which must be cleared first, because the call
    // does not set it on success.
    ::SetLastError(NO_ERROR);
    return ::GetFileType(handle) != FILE_TYPE_UNKNOWN
        || ::GetLastError() == NO_ERROR;
#else
    return ::fcntl(0, F_GETFD) != -1;
#endif
}

// Marker records are LENGTH-PREFIXED, not line-delimited: an argument the
// no-shell test sends could legitimately contain a newline, and a format that
// could not represent it would quietly limit what the pin is allowed to prove.
void appendRecord(std::string& blob, std::string_view tag,
                  std::string const& value) {
    blob.append(tag);
    blob.push_back(' ');
    blob.append(std::to_string(value.size()));
    blob.push_back('\n');
    blob.append(value);
    blob.push_back('\n');
}

struct Marker {
    std::string              cwd;
    std::vector<std::string> args;  // the payload, argv[4..]
    // "live" / "dead" — written ONLY by the stdio-probe directive, so an empty
    // string here means "the child was never asked", which is a third state and
    // not a synonym for "dead". The pin that reads it asserts the value it
    // expects, so a marker that silently stopped carrying the record fails
    // rather than passing as a not-asked.
    std::string              stdinProbe;
};

// Parse a marker blob. Returns nullopt with `error` filled on ANY malformed
// input — a half-parsed marker would silently weaken every assertion made
// against it.
std::optional<Marker> parseMarker(std::string const& blob, std::string& error) {
    Marker      marker;
    std::size_t pos      = 0;
    bool        sawCwd   = false;
    while (pos < blob.size()) {
        std::size_t const space = blob.find(' ', pos);
        if (space == std::string::npos) {
            error = "no tag/length separator at offset " + std::to_string(pos);
            return std::nullopt;
        }
        std::string const tag = blob.substr(pos, space - pos);
        std::size_t const nl  = blob.find('\n', space + 1);
        if (nl == std::string::npos) {
            error = "no length terminator at offset " + std::to_string(space);
            return std::nullopt;
        }
        std::size_t const length = static_cast<std::size_t>(
            std::strtoull(blob.substr(space + 1, nl - space - 1).c_str(),
                          nullptr, 10));
        if (nl + 1 + length + 1 > blob.size()) {
            error = "record at offset " + std::to_string(pos)
                  + " claims " + std::to_string(length)
                  + " bytes but the blob is short";
            return std::nullopt;
        }
        std::string const value = blob.substr(nl + 1, length);
        if (tag == "CWD") {
            marker.cwd = value;
            sawCwd     = true;
        } else if (tag == "ARG") {
            marker.args.push_back(value);
        } else if (tag == "STDIN") {
            marker.stdinProbe = value;
        } else if (tag != "ARGV0") {
            error = "unknown record tag '" + tag + "'";
            return std::nullopt;
        }
        pos = nl + 1 + length + 1;
    }
    if (!sawCwd) {
        error = "marker has no CWD record";
        return std::nullopt;
    }
    return marker;
}

// The test executable's own path, captured in `main` BEFORE any test can
// change the working directory (argv[0] may be relative).
fs::path& selfPathStorage() {
    static fs::path self;
    return self;
}
fs::path const& selfPath() { return selfPathStorage(); }

// Read a marker file whole, in BINARY mode — on Windows a text-mode read
// would translate CRLF and the byte-identity assertion would be testing the
// iostream layer instead of the spawn.
std::optional<std::string> readFileBinary(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

// Spawn the fixture and return both the spawn result and the parsed marker.
struct FixtureRun {
    SpawnResult           result;
    std::optional<Marker> marker;
    std::string           markerError;
};

FixtureRun runFixture(fs::path const&                 markerPath,
                      int                             exitCode,
                      std::vector<std::string> const& payload,
                      fs::path const&                 cwd = {}) {
    std::vector<std::string> argv{selfPath().string(), std::string{kFixtureFlag},
                                  markerPath.string(), std::to_string(exitCode)};
    argv.insert(argv.end(), payload.begin(), payload.end());

    FixtureRun run;
    run.result = spawnAndWaitInherit(argv, cwd);
    if (auto const blob = readFileBinary(markerPath)) {
        run.marker = parseMarker(*blob, run.markerError);
    } else {
        run.markerError = "marker file '" + markerPath.string()
                        + "' was not written";
    }
    return run;
}

// ── The redirecting spawn's fixture runner ─────────────────────────────────

// Everything one redirected call produced: the verdict, the child's own view of
// itself (the marker), and the CAPTURE. The capture is read here, immediately
// after the call returns, rather than by each test — so every pin observes the
// file at the same moment in the protocol the contract describes ("read it after
// the wait returns"), and a test cannot accidentally read it while a later spawn
// is rewriting it.
struct RedirectRun {
    SpawnResult                result;
    std::optional<Marker>      marker;
    std::string                markerError;
    std::optional<std::string> captured;
    std::string                captureError;
};

RedirectRun runFixtureRedirected(fs::path const&                 markerPath,
                                 int                             exitCode,
                                 std::vector<std::string> const& payload,
                                 fs::path const&                 stdoutFile,
                                 fs::path const&                 cwd = {}) {
    std::vector<std::string> argv{selfPath().string(), std::string{kFixtureFlag},
                                  markerPath.string(), std::to_string(exitCode)};
    argv.insert(argv.end(), payload.begin(), payload.end());

    RedirectRun run;
    run.result = spawnAndWaitRedirectStdout(argv, cwd, stdoutFile);
    if (auto const blob = readFileBinary(markerPath)) {
        run.marker = parseMarker(*blob, run.markerError);
    } else {
        run.markerError = "marker file '" + markerPath.string()
                        + "' was not written";
    }
    run.captured = readFileBinary(stdoutFile);
    if (!run.captured.has_value()) {
        run.captureError = "redirect file '" + stdoutFile.string()
                         + "' does not exist or could not be opened";
    }
    return run;
}

// ── A DEADLINE, BECAUSE THE FAILURE UNDER TEST IS A HANG ───────────────────
//
// ★★ A DEADLOCK TEST WITHOUT A DEADLINE IS NOT A TEST. The property the flood
// pin establishes is that a child which outgrows every pipe buffer still
// completes — and the way that property fails is that `spawnAndWaitRedirectStdout`
// NEVER RETURNS. With no deadline the observable outcome is a CI job that hangs
// until some outer timeout kills the whole suite: no test name, no assertion, no
// red, and 25 minutes of a machine (ctest's default per-test limit is 1500 s).
//
// So a watchdog thread turns the hang into a hard, named failure. It cannot use
// gtest — an `ADD_FAILURE` from a thread that is not the one blocked inside the
// call would be recorded and then never reported, because the test body cannot
// reach its own end to have a verdict printed. `std::_Exit` after a loud message
// on stderr is what actually produces a red: ctest sees a non-zero exit and the
// message names which call did not come back. Killing the binary loses the tests
// that would have run after it, which is the correct trade for a deadlock — a
// substrate that can hang the compiler is not a partial result.
//
// The wait is a condition variable, not a sleep-poll: the common case is that
// the call finishes in milliseconds and the destructor must not add latency to
// every run of the pin.
class CallDeadline {
public:
    CallDeadline(std::chrono::milliseconds limit, std::string what)
        : what_(std::move(what)), limit_(limit), thread_([this] { watch(); }) {}

    ~CallDeadline() {
        {
            std::lock_guard<std::mutex> const lock(mutex_);
            finished_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    CallDeadline(CallDeadline const&)            = delete;
    CallDeadline& operator=(CallDeadline const&) = delete;

private:
    void watch() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, limit_, [this] { return finished_; })) {
            return;
        }
        std::fprintf(
            stderr,
            "\n★★ DEADLINE EXCEEDED after %lld ms: %s\n"
            "The call has not returned. This is the parent/child deadlock the "
            "file-handle redirect exists to make impossible: a capture with a "
            "bounded buffer and no concurrent drain wedges once the child "
            "writes past it. Exiting non-zero so this reports as a FAILURE "
            "rather than as a hung job.\n",
            static_cast<long long>(limit_.count()), what_.c_str());
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }

    std::string               what_;
    std::chrono::milliseconds limit_;
    std::mutex                mutex_;
    std::condition_variable   cv_;
    bool                      finished_ = false;
    std::thread               thread_;  // LAST: it runs `watch`, which reads
                                        // every member above it
};

// 60 s. ⚠ MEASURED on the MSVC-Debug leg (Windows 11, this repo's local gate):
// the whole flood pin — one `CreateProcessW` of the multi-megabyte gtest binary,
// 256 KiB written, the wait, and reading the file back — takes 49 ms. So the
// budget is ~1200x the observed cost and cannot fire on a merely loaded or
// emulated machine; it is a HANG detector, not a performance assertion.
// It is also 25x shorter than ctest's default per-test timeout, which is the
// number it exists to beat: a deadlock then costs one minute and names itself,
// instead of costing 25 minutes and naming nothing.
constexpr std::chrono::milliseconds kRedirectDeadline{60000};

// Compare two large blobs WITHOUT dumping them. `EXPECT_EQ` on a 256 KiB string
// prints both operands in full, burying the only fact that matters — where they
// diverge — under half a megabyte of log. Returns npos when they are identical.
std::size_t firstDifference(std::string const& got, std::string const& want) {
    std::size_t const shared = std::min(got.size(), want.size());
    for (std::size_t i = 0; i < shared; ++i) {
        if (got[i] != want[i]) {
            return i;
        }
    }
    return got.size() == want.size() ? std::string::npos : shared;
}

std::string differenceReport(std::string const& got, std::string const& want) {
    std::size_t const at = firstDifference(got, want);
    if (at == std::string::npos) {
        return "identical";
    }
    auto const window = [at](std::string const& s) {
        if (at >= s.size()) {
            return std::string{"<end of data>"};
        }
        return "[" + s.substr(at, std::min<std::size_t>(24, s.size() - at))
             + "]";
    };
    return "first difference at byte " + std::to_string(at) + " of "
         + std::to_string(want.size()) + " — captured " + window(got)
         + " expected " + window(want) + " (captured " + std::to_string(got.size())
         + " bytes in total)";
}

// ── Moving THIS process's own stderr and stdin, so the child's can be seen ──
//
// ★ THE HANDLES/DESCRIPTORS IT INSTALLS ARE DELIBERATELY NOT INHERITABLE ON
// WINDOWS, and that is what gives the pin its teeth. `CreateFileW` with a null
// `SECURITY_ATTRIBUTES` produces a NON-inheritable handle, so a substrate that
// merely copied `GetStdHandle`'s results into `STARTUPINFOW` would hand the
// child two handles that cannot cross — the child's stderr would be dead and the
// stderr file empty. Only the `DuplicateHandle(..., bInheritHandle=TRUE)` step
// in `duplicateStdHandleInheritable` makes them travel, so this test fails if
// that step is removed. On POSIX inheritance is the default for a descriptor and
// the same swap simply proves nothing was redirected that should not have been.
//
// STDIN is pointed at the null device rather than left alone so the "the child
// still has a stdin" assertion cannot be vacuous: whatever this process was
// launched with, it definitely has a readable stdin for the duration.
class ParentStdioSwap {
public:
    explicit ParentStdioSwap(fs::path const& errPath) { install(errPath); }
    ~ParentStdioSwap() { restore(); }

    ParentStdioSwap(ParentStdioSwap const&)            = delete;
    ParentStdioSwap& operator=(ParentStdioSwap const&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::string const& error() const { return error_; }

private:
#if defined(_WIN32)
    void install(fs::path const& errPath) {
        savedErr_ = ::GetStdHandle(STD_ERROR_HANDLE);
        savedIn_  = ::GetStdHandle(STD_INPUT_HANDLE);
        errHandle_ =
            ::CreateFileW(errPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                          /*lpSecurityAttributes=*/nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (errHandle_ == INVALID_HANDLE_VALUE) {
            error_ = "CreateFileW('" + errPath.string()
                   + "') failed, GetLastError="
                   + std::to_string(::GetLastError());
            return;
        }
        nulHandle_ = ::CreateFileW(L"NUL", GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
        if (nulHandle_ == INVALID_HANDLE_VALUE) {
            error_ = "CreateFileW('NUL') failed, GetLastError="
                   + std::to_string(::GetLastError());
            return;
        }
        std::fflush(stderr);
        ok_ = ::SetStdHandle(STD_ERROR_HANDLE, errHandle_) != 0
           && ::SetStdHandle(STD_INPUT_HANDLE, nulHandle_) != 0;
        if (!ok_) {
            error_ = "SetStdHandle failed, GetLastError="
                   + std::to_string(::GetLastError());
        }
    }

    void restore() {
        std::fflush(stderr);
        ::SetStdHandle(STD_ERROR_HANDLE, savedErr_);
        ::SetStdHandle(STD_INPUT_HANDLE, savedIn_);
        if (errHandle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(errHandle_);
        }
        if (nulHandle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(nulHandle_);
        }
    }

    HANDLE savedErr_  = nullptr;
    HANDLE savedIn_   = nullptr;
    HANDLE errHandle_ = INVALID_HANDLE_VALUE;
    HANDLE nulHandle_ = INVALID_HANDLE_VALUE;
#else
    void install(fs::path const& errPath) {
        std::fflush(stderr);
        savedErr_ = ::dup(2);
        savedIn_  = ::dup(0);
        if (savedErr_ < 0 || savedIn_ < 0) {
            error_ = "dup() of this process's own stderr/stdin failed (errno="
                   + std::to_string(errno)
                   + ") — the pin cannot restore what it cannot save";
            return;
        }
        errFd_ = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        nulFd_ = ::open("/dev/null", O_RDONLY);
        if (errFd_ < 0 || nulFd_ < 0) {
            error_ = "open() of the stderr file or /dev/null failed (errno="
                   + std::to_string(errno) + ")";
            return;
        }
        ok_ = ::dup2(errFd_, 2) >= 0 && ::dup2(nulFd_, 0) >= 0;
        if (!ok_) {
            error_ = "dup2() onto 0/2 failed (errno=" + std::to_string(errno)
                   + ")";
        }
    }

    void restore() {
        std::fflush(stderr);
        if (savedErr_ >= 0) {
            ::dup2(savedErr_, 2);
            ::close(savedErr_);
        }
        if (savedIn_ >= 0) {
            ::dup2(savedIn_, 0);
            ::close(savedIn_);
        }
        if (errFd_ >= 0) {
            ::close(errFd_);
        }
        if (nulFd_ >= 0) {
            ::close(nulFd_);
        }
    }

    int savedErr_ = -1;
    int savedIn_  = -1;
    int errFd_    = -1;
    int nulFd_    = -1;
#endif

    bool        ok_ = false;
    std::string error_;
};

// ── PATH manipulation (hermetic resolver tests) ────────────────────────────

std::string currentPathVar() {
    char const* const raw = std::getenv("PATH");
    return raw != nullptr ? std::string{raw} : std::string{};
}

void setPathVar(std::string const& value) {
#if defined(_WIN32)
    ::_putenv_s("PATH", value.c_str());
#else
    ::setenv("PATH", value.c_str(), /*overwrite=*/1);
#endif
}

// Restores PATH on scope exit so one test cannot leak its search path into
// the next (they share a process).
class PathVarGuard {
public:
    PathVarGuard() : saved_(currentPathVar()) {}
    ~PathVarGuard() { setPathVar(saved_); }
    PathVarGuard(PathVarGuard const&)            = delete;
    PathVarGuard& operator=(PathVarGuard const&) = delete;

private:
    std::string saved_;
};

#if defined(_WIN32)
constexpr char kPathListSeparator = ';';
#else
constexpr char kPathListSeparator = ':';
#endif

// The hostile payload, in ONE place, because TWO spawners are held to it: the
// shipped `spawnAndWaitInherit` and the test tier's `runBinary`. They share the
// argv-quoting algorithm and nothing else, so a divergence between them is
// exactly the failure this list is shaped to catch — every element is something
// a shell would MANGLE (`$HOME` / `%PATH%` substituted, `&&` / `;` / `|`
// splitting the command, `>` redirecting) or something a naive `"` + arg + `"`
// quoter would corrupt (an embedded quote, a trailing backslash, the empty
// argument).
std::vector<std::string> const& hostileArgvPayload() {
    static std::vector<std::string> const payload{
        "$HOME",
        "%PATH%",
        "&& echo pwned",
        ";",
        "a | b > c",
        "~",
        "$(echo substituted)",
        "`echo backticked`",
        "two  spaces  and\ttab",
        "he said \"hi\"",
        "C:\\Program Files\\dir\\",
        "",
        "trailing\\",
    };
    return payload;
}

// Narrow an ASCII wide string for readable failure messages ONLY. Assertions
// compare `std::wstring` directly — comparing narrowed forms would hide a
// widening bug, which is exactly one of the things under test.
std::string narrowForMessage(std::wstring const& wide) {
    std::string out;
    for (wchar_t c : wide) {
        auto const v = static_cast<unsigned long>(c);
        if (v >= 0x20u && v < 0x7Fu) {
            out.push_back(static_cast<char>(v));
        } else {
            out += "\\u" + std::to_string(v);
        }
    }
    return out;
}

// Run `argv` through the reporting quoter and hand back the rejection message,
// having first insisted that it WAS a rejection. Returning the string (rather
// than asserting inside) is what lets each reject branch be pinned with one
// `EXPECT_EQ` against its own literal, so a decoder that collapsed two branches
// onto one message would fail on the branch it stole from.
#if !defined(_WIN32)
// The substrate composes its errno prose as `errno=<n>: <strerror text>`. The
// TAIL is localized, so rebuilding it here from `std::strerror` — rather than
// hardcoding an English literal — is what keeps these pins from failing on a
// non-English CI machine. The NUMERIC prefix is the part that must not drift,
// and a message that dropped it, or that reported a different errno, fails the
// comparison exactly as it should.
std::string expectedErrnoText(int code) {
    char const* const message = std::strerror(code);
    return "errno=" + std::to_string(code) + ": "
         + (message != nullptr ? message : "<no message text for this errno>");
}

// What "the OS refused to execute this image" reads like on the POSIX arm this
// host actually takes. The two arms create the process with DIFFERENT system
// calls, so the failure is reported by whichever one made the attempt — `execv`
// from inside the forked child, `posix_spawn` from the parent — and each host is
// pinned against its OWN exact sentence rather than a shared substring. The test
// that uses this RUNS on every POSIX leg; only the literal differs, so there is
// no platform where it quietly proves nothing.
//
// The `posix_spawn` form carries one extra clause, and it is the honest half of
// that arm's trade: the chdir file action and the exec happen inside one call
// which hands back ONE errno for the pair, so when a working directory was
// requested the platform genuinely does not say which step failed. The fork arm
// learns the stage from the child's own record and never has to say it.
std::string expectedImageFailureDiagnostic(std::string const& exePath,
                                           std::string const& cwdPath,
                                           int                code) {
#if defined(__APPLE__)
    std::string message = "spawnAndWaitInherit: posix_spawn('" + exePath
                        + "') failed (" + expectedErrnoText(code) + ")";
    if (!cwdPath.empty()) {
        message += " — the child was also asked to enter working directory '"
                 + cwdPath
                 + "' first, and posix_spawn reports ONE errno for the whole "
                   "operation, so the platform does not say which of the two "
                   "failed";
    }
    return message;
#else
    (void)cwdPath;  // the fork arm's child chdir'd first and reports the stage
    return "spawnAndWaitInherit: execv('" + exePath + "') failed ("
         + expectedErrnoText(code) + ")";
#endif
}
#endif

std::string decodeErrorFor(std::vector<std::string> const& argv) {
    std::wstring line;
    std::string  error;
    EXPECT_FALSE(tryBuildWindowsCommandLine(argv, line, error))
        << "expected a REJECT; the decoder accepted the input and produced ["
        << narrowForMessage(line) << "]";
    return error;
}

} // namespace

// ══ buildWindowsCommandLine — exact-string pins ═══════════════════════════
//
// NOT `#ifdef _WIN32`-gated: the function is a pure string transform with no
// Windows API in it, so every host in the matrix checks the algorithm. The
// ROUND-TRIP oracle below is the part that genuinely needs Windows.

TEST(BuildWindowsCommandLine, EmptyArgvIsAnEmptyCommandLine) {
    EXPECT_EQ(buildWindowsCommandLine({}), std::wstring{});
}

TEST(BuildWindowsCommandLine, PlainArgumentsAreNotQuotedAtAll) {
    // Gratuitous quoting is not wrong, but it IS a different algorithm; pin
    // the one we implement so a rewrite has to be deliberate.
    auto const got = buildWindowsCommandLine({"git", "clone", "https://h/r.git"});
    EXPECT_EQ(got, std::wstring{L"git clone https://h/r.git"})
        << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, ArgumentWithASpaceIsQuoted) {
    auto const got = buildWindowsCommandLine({"a b"});
    EXPECT_EQ(got, std::wstring{L"\"a b\""}) << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, ArgumentWithATabIsQuoted) {
    // Tab is a delimiter to the CRT parser just like space; an implementation
    // that only checked for ' ' would split this argument in two.
    auto const got = buildWindowsCommandLine({"a\tb"});
    EXPECT_EQ(got, std::wstring{L"\"a\tb\""}) << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, EmbeddedQuoteIsBackslashEscaped) {
    // The naive `"` + arg + `"` produces `"a"b"`, whose embedded quote CLOSES
    // the argument — everything after it re-splits into new arguments. This is
    // half of the live defect documented at run_binary.hpp:343-349.
    auto const got = buildWindowsCommandLine({"a\"b"});
    EXPECT_EQ(got, std::wstring{L"\"a\\\"b\""}) << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, TrailingBackslashInAnUnquotedArgStaysSingle) {
    // No delimiter and no quote ⇒ no quoting ⇒ the backslash never abuts a
    // `"` and must NOT be doubled. This and the next test are two different
    // cases precisely because the quoting is conditional; an implementation
    // that doubled unconditionally would corrupt this one.
    auto const got = buildWindowsCommandLine({"a\\"});
    EXPECT_EQ(got, std::wstring{L"a\\"}) << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, TrailingBackslashBeforeTheClosingQuoteIsDoubled) {
    // ★ THE `git` CONSUMER'S NORMAL CASE. A backslash-terminated Windows
    // directory argument that also needs quoting: the naive form emits
    // `"C:\Program Files\dir\"`, where the trailing backslash ESCAPES the
    // closing quote — the child then sees one argument
    // `C:\Program Files\dir" next` and `next` disappears entirely.
    auto const got =
        buildWindowsCommandLine({"C:\\Program Files\\dir\\", "next"});
    EXPECT_EQ(got, std::wstring{L"\"C:\\Program Files\\dir\\\\\" next"})
        << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, EmptyArgumentBecomesAnEmptyQuotedPair) {
    // `""` is the ONLY way to pass an empty argument; emitting nothing would
    // silently drop it and shift every later argument down one index.
    auto const got = buildWindowsCommandLine({"a", "", "b"});
    EXPECT_EQ(got, std::wstring{L"a \"\" b"}) << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, BackslashRunBeforeAQuoteIsDoubledPlusOne) {
    // Input bytes:  a \ \ " b   →   2n+1 = 5 backslashes, then a literal `"`.
    // This is the case that separates "doubles backslashes" (wrong) from
    // "doubles backslashes that PRECEDE a quote" (right).
    auto const got = buildWindowsCommandLine({"a\\\\\"b"});
    EXPECT_EQ(got, std::wstring{L"\"a\\\\\\\\\\\"b\""}) << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, InteriorBackslashesAreNotTouched) {
    // A path with no space and no quote passes through byte-for-byte —
    // doubling here would turn C:\a\b into a path that does not exist.
    auto const got = buildWindowsCommandLine({"C:\\a\\b"});
    EXPECT_EQ(got, std::wstring{L"C:\\a\\b"}) << narrowForMessage(got);
}

TEST(BuildWindowsCommandLine, Utf8IsDecodedToTheHostWideEncoding) {
    // The widening step is a strict UTF-8 decode, not a byte-widening cast:
    // "é" is 0xC3 0xA9 on the way in and must be ONE wide unit U+00E9, not
    // two. A cast-based implementation passes every ASCII pin above and fails
    // only here — which is why this pin exists.
    auto const got = buildWindowsCommandLine({"caf\xC3\xA9"});
    ASSERT_EQ(got.size(), std::size_t{4}) << narrowForMessage(got);
    EXPECT_EQ(static_cast<unsigned long>(got[3]), 0xE9ul);

    // U+1F600 (F0 9F 98 80): a surrogate PAIR where wchar_t is 16 bits, one
    // unit where it is 32. Pinning both shapes keeps the encoder honest on
    // every leg instead of only the one it was written on.
    auto const astral = buildWindowsCommandLine({"\xF0\x9F\x98\x80"});
    if constexpr (sizeof(wchar_t) >= 4) {
        ASSERT_EQ(astral.size(), std::size_t{1});
        EXPECT_EQ(static_cast<unsigned long>(astral[0]), 0x1F600ul);
    } else {
        ASSERT_EQ(astral.size(), std::size_t{2});
        EXPECT_EQ(static_cast<unsigned long>(astral[0]), 0xD83Dul);
        EXPECT_EQ(static_cast<unsigned long>(astral[1]), 0xDE00ul);
    }
}

// ══ decodeUtf8ToWide — every REJECT branch, by its exact message ══════════
//
// The decoder refuses malformed UTF-8 rather than substituting U+FFFD, because
// a transliterated command line is a WRONG command line that would then be
// EXECUTED — and `spawnAndWaitInherit` reports the message it produces straight
// out through `SpawnResult::diagnostic`. Until now not one of its five reject
// branches had an assertion anywhere in `tests/`: grepping for
// `decodeUtf8ToWide` or `tryBuildWindowsCommandLine` returned zero. A decoder
// that accepted an overlong form, or that reported every failure with one
// generic sentence, was free to ship.
//
// NOT `_WIN32`-gated, for the same reason as the pins above: the function is a
// pure string transform with no Windows API in it, so the branch coverage is
// worth having on every leg. The inputs are hex escapes, never source
// characters, so no toolchain's source-charset handling can alter them.
TEST(TryBuildWindowsCommandLine, AStrayContinuationByteIsRejectedAsABadLead) {
    // 0x80 is a continuation byte with no lead in front of it. The byte is
    // rendered as REAL hex: `0x80`, not the decimal-wearing-a-hex-prefix
    // `0x128` that `"0x" + std::to_string(lead)` would produce and that is not
    // even a possible byte value.
    EXPECT_EQ(decodeErrorFor({"\x80"}),
              "invalid UTF-8 lead byte 0x80 at offset 0");
}

TEST(TryBuildWindowsCommandLine, AnOutOfRangeLeadByteIsRejected) {
    // 0xF5 would start a code point above U+10FFFF; the encoding has no such
    // lead, so it is refused before any continuation byte is even looked at.
    EXPECT_EQ(decodeErrorFor({"\xF5\x80\x80\x80"}),
              "invalid UTF-8 lead byte 0xF5 at offset 0");
}

TEST(TryBuildWindowsCommandLine, ATruncatedSequenceIsRejected) {
    // 0xC3 promises one continuation byte and the string ends instead.
    EXPECT_EQ(decodeErrorFor({"\xC3"}),
              "truncated UTF-8 sequence at offset 0");
}

TEST(TryBuildWindowsCommandLine, ABadContinuationByteIsRejected) {
    // The tail is present but is not a continuation byte — a distinct fact
    // from "the tail is missing", and it must not report as the same one.
    EXPECT_EQ(decodeErrorFor({"\xC3\x28"}),
              "invalid UTF-8 continuation byte at offset 1");
}

TEST(TryBuildWindowsCommandLine, AnOverlongEncodingIsRejected) {
    // C0 80 / E0 80 80 encode U+0000 in more bytes than the shortest form.
    // Accepting them is the classic filter bypass: a NUL, or a '/', that the
    // layer above scanned for and did not see.
    EXPECT_EQ(decodeErrorFor({"\xE0\x80\x80"}),
              "overlong UTF-8 encoding at offset 0");
}

TEST(TryBuildWindowsCommandLine, ASurrogateCodePointIsRejected) {
    // ED A0 80 decodes to U+D800, half of a surrogate pair. It is a valid
    // UTF-16 code UNIT and not a Unicode scalar, so it may not be encoded.
    EXPECT_EQ(decodeErrorFor({"\xED\xA0\x80"}),
              "UTF-8 sequence at offset 0 decodes to a value that is not a "
              "Unicode scalar");
}

TEST(TryBuildWindowsCommandLine, ACodePointAboveTheUnicodeMaximumIsRejected) {
    // F4 90 80 80 = U+110000, one past the last scalar. The lead byte itself
    // is legal, so this branch is reachable only after the full decode — the
    // one the out-of-range LEAD test above cannot stand in for.
    EXPECT_EQ(decodeErrorFor({"\xF4\x90\x80\x80"}),
              "UTF-8 sequence at offset 0 decodes to a value that is not a "
              "Unicode scalar");
}

TEST(TryBuildWindowsCommandLine, TheReportedOffsetIsIntoTheQUOTEDCommandLine) {
    // The decode runs over the JOINED, already-quoted line, not over the
    // element — so the offset a caller is handed counts from the start of the
    // command line. Pinning it stops the two coordinate systems being swapped
    // (which no single-element test above could notice: there, both are 0).
    EXPECT_EQ(decodeErrorFor({"ok", "\x80"}),
              "invalid UTF-8 lead byte 0x80 at offset 3");
}

// ★ THE `std::abort` IS API CONTRACT, so it is pinned like one (SKILL §7.1.6).
// `buildWindowsCommandLine` has no error channel and deliberately dies rather
// than return a U+FFFD-substituted command line that would then be executed;
// the message names the decode failure so the operator learns WHICH argument
// was malformed and not merely that one was.
TEST(BuildWindowsCommandLineDeath, InvalidUtf8AbortsAndNamesTheDecodeFailure) {
    EXPECT_DEATH(
        (void)buildWindowsCommandLine({"\x80"}),
        "dss::substrate::buildWindowsCommandLine fatal: argv is not valid "
        "UTF-8 .invalid UTF-8 lead byte 0x80 at offset 0.");
}

#if defined(_WIN32)

// ★ THE ORACLE. Everything above is a hand-computed expectation, and a
// hand-computed expectation can be wrong in exactly the same way the code is.
// This test instead feeds the output to `CommandLineToArgvW` — the very
// parser the algorithm targets, and the one behind every Windows program's
// `argv` — and demands the original vector back, element for element. A
// quoting bug cannot survive this unless it is also a Windows bug.
//
// argv[0] is a plain token on purpose: the Windows parser applies a DIFFERENT
// rule to the first element (quotes toggle, backslashes are never escapes), so
// a program path is the only thing that legitimately goes there. Every hostile
// case is placed at argv[1..], which is where user-supplied text actually goes.
TEST(BuildWindowsCommandLine, RoundTripsThroughCommandLineToArgvW) {
    std::vector<std::string> const argv{
        "program",
        "a b",
        "a\"b",
        "a\\",
        "C:\\Program Files\\dir\\",
        "",
        "a\\\\\"b",
        "\\\\server\\share\\",
        "--message=he said \"hi\" and left",
        "$HOME %PATH% && echo pwned ; x | y > z",
        "trailing space ",
        " leading space",
        "a\tb",
    };

    std::wstring const line = buildWindowsCommandLine(argv);
    ASSERT_FALSE(line.empty());

    int     count = 0;
    LPWSTR* parsed = ::CommandLineToArgvW(line.c_str(), &count);
    ASSERT_NE(parsed, nullptr)
        << "CommandLineToArgvW rejected our command line outright ("
        << narrowForMessage(line) << ")";

    EXPECT_EQ(static_cast<std::size_t>(count), argv.size())
        << "the parser recovered a DIFFERENT NUMBER of arguments — the "
           "command line re-split. Line: " << narrowForMessage(line);
    if (static_cast<std::size_t>(count) == argv.size()) {
        for (int i = 0; i < count; ++i) {
            std::wstring const back{parsed[i]};
            // Compare against the expected value widened the same way the
            // producer widens (these cases are all ASCII, so this is a direct
            // element-wise widening and introduces no second algorithm).
            std::wstring want;
            for (char c : argv[static_cast<std::size_t>(i)]) {
                want.push_back(static_cast<wchar_t>(
                    static_cast<unsigned char>(c)));
            }
            EXPECT_EQ(back, want)
                << "argv[" << i << "] did not survive the round trip: got ["
                << narrowForMessage(back) << "] want ["
                << narrowForMessage(want) << "]";
        }
    }
    ::LocalFree(parsed);
}

#endif  // _WIN32

// ══ resolveExecutableOnPath ═══════════════════════════════════════════════

TEST(ResolveExecutableOnPath, EmptyCommandIsNotFound) {
    EXPECT_FALSE(resolveExecutableOnPath("").has_value());
}

TEST(ResolveExecutableOnPath, AnAbsentNameIsNotFound) {
    // A name with no plausible collision on any host in the matrix.
    auto const hit =
        resolveExecutableOnPath("dss-no-such-program-3f1c9a2e-please-fail");
    ASSERT_FALSE(hit.has_value())
        << "resolved to " << (hit ? hit->string() : std::string{});
}

// HERMETIC, and therefore an EXACT assertion rather than a host survey: the
// test puts a directory it controls (its own) on PATH and resolves a name it
// knows is in it (its own). RED-ON-DISABLE is built in — the same lookup is
// performed BEFORE the directory is added and must fail, so a resolver that
// ignored PATH entirely (e.g. one that fell back to searching the current
// directory) would trip the first half.
//
// On Windows this also exercises PATHEXT: the test executable is
// `dss_core_substrate_test_process_spawn.exe` and the name looked up is the
// STEM, with no extension at all — only PATHEXT probing can bridge that.
TEST(ResolveExecutableOnPath, FindsANameOnPathAndNotOtherwise) {
    ASSERT_TRUE(fs::exists(selfPath())) << selfPath().string();
    fs::path const  ownDir = selfPath().parent_path();
    std::string const stem = selfPath().stem().string();

    PathVarGuard guard;

    // (1) A PATH consisting of ONE directory we just created and know is
    // empty. Deliberately not the empty string: `_putenv_s("PATH","")` DELETES
    // the variable on Windows, and "the resolver found nothing because there
    // was no PATH at all" is a much weaker statement than "the resolver
    // searched a real PATH and correctly found nothing".
    dss::test_support::ScratchDir emptyDir{dss::test_support::Location::Temp,
                                           "process-spawn-empty-path"};
    setPathVar(emptyDir.path().string());
    EXPECT_FALSE(resolveExecutableOnPath(stem).has_value())
        << "found '" << stem << "' with PATH=" << currentPathVar()
        << " — the resolver is searching somewhere it should not (the current "
           "directory is deliberately never searched)";

    // (2) The same lookup with our directory on PATH. A trailing entry that
    // does not exist is included so a resolver that stopped at the first
    // unusable entry (rather than continuing the scan) would still be caught
    // by ordering the real one first — and so the scan provably tolerates a
    // dead entry, which real PATHs are full of.
    setPathVar(ownDir.string() + std::string(1, kPathListSeparator)
               + "/dss-nonexistent-tail");
    auto const hit = resolveExecutableOnPath(stem);
    {
        char const* const pathext = std::getenv("PATHEXT");
        ASSERT_TRUE(hit.has_value())
            << "'" << stem << "' not found with PATH=" << currentPathVar()
            << " PATHEXT=" << (pathext != nullptr ? pathext : "<unset>")
            << " — on Windows the stem carries no extension, so this lookup "
               "can only succeed through PATHEXT probing";
    }
    EXPECT_TRUE(fs::exists(*hit)) << hit->string();
    EXPECT_TRUE(hit->is_absolute())
        << "a resolved path must be absolute so it stays valid after the "
           "child changes directory: " << hit->string();

    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(*hit, selfPath(), ec))
        << "resolved " << hit->string() << " but expected "
        << selfPath().string()
        << (ec ? " (equivalent() failed: " + ec.message() + ")"
               : std::string{});
}

// A command that already carries a directory component is NOT a PATH lookup —
// it is taken as given. Pinned with a PATH that contains only a known-empty
// directory, so a resolver that (wrongly) routed this through the search would
// find nothing and fail here.
TEST(ResolveExecutableOnPath, APathIsTakenAsGivenWithoutSearching) {
    PathVarGuard guard;
    dss::test_support::ScratchDir emptyDir{dss::test_support::Location::Temp,
                                           "process-spawn-empty-path"};
    setPathVar(emptyDir.path().string());

    auto const hit = resolveExecutableOnPath(selfPath().string());
    ASSERT_TRUE(hit.has_value()) << selfPath().string();
    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(*hit, selfPath(), ec)) << hit->string();
}

// ★ THE CURRENT DIRECTORY IS NEVER SEARCHED — including through the back door
// POSIX leaves open, where an EMPTY PATH entry means "the current directory"
// (`execvp` honours that; `PATH=/usr/bin:` really does search `.`). This test
// stands in exactly that spot: the process's cwd IS the directory holding the
// executable, and PATH is one empty directory plus two EMPTY entries. Finding
// the program here would mean a `git` named in a project config could be
// satisfied by a `git` sitting in the project being compiled.
//
// Added because the mutation battery proved the sibling test above did not
// cover it: re-introducing execvp's empty-entry rule left the whole suite
// green. It goes red here.
TEST(ResolveExecutableOnPath, AnEmptyPathEntryDoesNotMeanTheCurrentDirectory) {
    ASSERT_TRUE(fs::exists(selfPath())) << selfPath().string();
    std::string const stem = selfPath().stem().string();

    PathVarGuard guard;
    dss::test_support::ScratchDir emptyDir{dss::test_support::Location::Temp,
                                           "process-spawn-empty-path"};
    // Entries: [<empty dir>, "", ""] — an interior and a trailing empty, the
    // two spellings a real PATH picks up from careless concatenation.
    setPathVar(emptyDir.path().string() + std::string(2, kPathListSeparator));

    fs::path const restore = fs::current_path();
    fs::current_path(selfPath().parent_path());
    auto const hit = resolveExecutableOnPath(stem);
    std::string const pathUsed = currentPathVar();
    fs::current_path(restore);  // restore BEFORE asserting, so a red test
                                // does not corrupt every test after it

    EXPECT_FALSE(hit.has_value())
        << "resolved '" << stem << "' to " << (hit ? hit->string() : "")
        << " while standing in its own directory with PATH=" << pathUsed
        << " — an empty PATH entry was treated as the current directory";
}

// A directory is not an executable, however resolvable its name is.
TEST(ResolveExecutableOnPath, ADirectoryIsNotAnExecutable) {
    EXPECT_FALSE(
        resolveExecutableOnPath(selfPath().parent_path().string()).has_value());
}

#if defined(_WIN32)

// ★ A NON-ASCII argv[0] MUST RESOLVE, AND MUST RESOLVE THE SAME WAY ON BOTH
// WINDOWS TOOLCHAINS. `argv` is UTF-8 by contract, but `fs::path{std::string}`
// converts through the host's "native narrow encoding" — MS-STL uses the ACTIVE
// CODE PAGE, libstdc++/MinGW uses UTF-8 — and this project ships both. So a
// manifest naming `tools/générateur.exe` resolved against a mojibake path on the
// MSVC leg and the operator was told to fix a PATH that was never wrong.
//
// The file is CREATED from a wide literal (the name Windows actually stores) and
// LOOKED UP with the UTF-8 bytes a manifest carries, so the two halves cannot
// agree by accident. RED-ON-REVERT: restore `fs::path{entry} / command` and the
// MSVC leg searches for `gÃ©nÃ©rateur.*` and finds nothing.
TEST(ResolveExecutableOnPath, ANonAsciiUtf8NameResolvesRegardlessOfCodePage) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-utf8"};
    // Spelled with universal-character-names, never as source characters: MSVC
    // reads a BOM-less file in the ACTIVE CODE PAGE, so a literal `é` in this
    // file would itself become mojibake and the test would pin the bug.
    fs::path const tool =
        scratch.path() / std::wstring{L"g\u00e9n\u00e9rateur.exe"};
    {
        std::ofstream file(tool, std::ios::binary);
        ASSERT_TRUE(file.good()) << "could not create the non-ASCII fixture";
        file << "not a real executable image";
    }
    ASSERT_TRUE(fs::exists(tool));

    PathVarGuard guard;
    setPathVar(scratch.path().string());

    // The UTF-8 encoding of "générateur", with no extension — so the lookup
    // also has to survive PATHEXT probing on the decoded name.
    std::string const utf8Stem = "g\xC3\xA9n\xC3\xA9rateur";
    auto const        hit      = resolveExecutableOnPath(utf8Stem);
    ASSERT_TRUE(hit.has_value())
        << "the UTF-8 name did not resolve with PATH=" << currentPathVar()
        << " — argv[0] was decoded with something other than the UTF-8 decoder";

    // PATHEXT supplies the extension in ITS OWN case (".EXE"), so compare the
    // stem exactly and the file by identity rather than the whole string.
    EXPECT_EQ(hit->stem().wstring(), std::wstring{L"g\u00e9n\u00e9rateur"});
    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(*hit, tool, ec))
        << "resolved " << hit->string() << " but expected " << tool.string()
        << (ec ? " (equivalent() failed: " + ec.message() + ")"
               : std::string{});
}

#endif  // _WIN32

// ══ spawnAndWaitInherit ═══════════════════════════════════════════════════

TEST(SpawnAndWaitInherit, EmptyArgvIsReportedNotAborted) {
    auto const r = spawnAndWaitInherit({});
    EXPECT_FALSE(r.spawned);
    EXPECT_FALSE(r.diagnostic.empty());
}

// ★ THE `spawned` / `exitCode` DISCRIMINATION, both halves in one test.
// An absent argv[0] must report `spawned == false`; a program that RAN and
// returned 127 must report `spawned == true, exitCode == 127`. If spawn
// failure were ever signalled through a magic 127, these two would be
// indistinguishable and this test would go red.
TEST(SpawnAndWaitInherit, AnAbsentProgramIsNotSpawnedAndSaysWhy) {
    auto const missing =
        spawnAndWaitInherit({"dss-no-such-program-3f1c9a2e-please-fail",
                             "--whatever"});
    EXPECT_FALSE(missing.spawned);
    EXPECT_FALSE(missing.diagnostic.empty())
        << "a spawn failure with no diagnostic is a silent failure";
    EXPECT_NE(missing.diagnostic.find("dss-no-such-program-3f1c9a2e-please-fail"),
              std::string::npos)
        << "the diagnostic must name the program that was not found: "
        << missing.diagnostic;

    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn"};
    auto const ran = runFixture(scratch.path() / "exit127.marker", 127, {});
    EXPECT_TRUE(ran.result.spawned)
        << "a program that RUNS and returns 127 must not be confused with a "
           "program that never started: "
        << ran.result.diagnostic;
    EXPECT_EQ(ran.result.exitCode, 127);
}

// The not-found diagnostic in FULL, character for character. Its counterpart
// below (the present-but-not-executable arm) differs by one inserted clause, so
// pinning only one of the two would let the other be emitted everywhere: an
// unconditional "and it is not executable" is a plausible refactor and would
// leave every substring assertion in this file green.
TEST(SpawnAndWaitInherit, TheNotFoundDiagnosticSaysNothingAboutPermissions) {
    auto const r =
        spawnAndWaitInherit({"dss-no-such-program-3f1c9a2e-please-fail"});
    EXPECT_FALSE(r.spawned);
    EXPECT_EQ(r.diagnostic,
              "spawnAndWaitInherit: 'dss-no-such-program-3f1c9a2e-please-fail' "
              "was not found as an executable. It was looked up in PATH (the "
              "current directory is deliberately never searched); pass a path "
              "containing a directory separator to run a local tool.");
}

#if !defined(_WIN32)

// ★ PRESENT BUT NOT EXECUTABLE IS NOT "NOT FOUND". `access(X_OK)` failure used
// to be folded into "not a candidate", so an interpreter sitting at mode 644 —
// right where the operator put it, spelled correctly — produced "'x' was not
// found as an executable. It was looked up in PATH...", sending them to audit a
// PATH that was never the problem. The message must name the file it found and
// say what is wrong with it.
//
// POSIX-only because the fault is POSIX-only: `classifyCandidate`'s Windows arm
// has no permission probe to fold, so there is no near-miss to report there.
TEST(SpawnAndWaitInherit, APresentButNonExecutableFileIsNotReportedAsMissing) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-perm"};
    fs::path const tool = scratch.path() / "dss-non-executable-hook";
    {
        std::ofstream file(tool, std::ios::binary);
        ASSERT_TRUE(file.good());
        file << "#!/bin/sh\nexit 0\n";
    }
    std::error_code ec;
    fs::permissions(tool, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    ASSERT_FALSE(ec) << ec.message();
    // The control: with no execute bit set anywhere, X_OK is refused even for
    // uid 0 (root's exemption requires at least one x bit), so this pin is not
    // silently vacuous in a container that runs the suite as root.
    ASSERT_NE(::access(tool.c_str(), X_OK), 0)
        << "the fixture is executable after chmod 600 — the pin would prove "
           "nothing";

    PathVarGuard guard;
    setPathVar(scratch.path().string());

    auto const r = spawnAndWaitInherit({"dss-non-executable-hook", "--x"});
    EXPECT_FALSE(r.spawned);
    EXPECT_EQ(r.diagnostic,
              "spawnAndWaitInherit: 'dss-non-executable-hook' was not found as "
              "an executable: '" + tool.string()
                  + "' exists and is a regular file but is not executable by "
                    "this user (check its permission bits). It was looked up "
                    "in PATH (the current directory is deliberately never "
                    "searched); pass a path containing a directory separator "
                    "to run a local tool.");
}

// ★★ THE `WIFSIGNALED` ARM WAS UNEXECUTED ON EVERY LEG. A reviewer replaced the
// whole block with `out.exitCode = 0;` and the suite stayed green — which means
// a pre-build hook that SEGV'd would have reported a clean exit and the compile
// would have proceeded against stale generated sources. Both halves are
// asserted here, because either alone can be faked: the CODE (`128 + signo`,
// the shell convention) and the DIAGNOSTIC that names the signal, without which
// 137 is indistinguishable from a program that deliberately returned 137.
//
// SIGKILL rather than SIGSEGV: it cannot be caught, blocked or redefined, so
// the outcome does not depend on whether a sanitizer or a JIT installed a
// handler first. The marker assertion is what keeps "killed" from being
// satisfied by a child that died before reaching user code at all.
TEST(SpawnAndWaitInherit, ASignalKilledChildReportsTheSignalAndNamesIt) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn"};
    auto const resolved = resolveExecutableOnPath(selfPath().string());
    ASSERT_TRUE(resolved.has_value()) << selfPath().string();

    std::vector<std::string> const directive{
        std::string{kRaiseSignalDirective}, std::to_string(SIGKILL)};
    auto const run =
        runFixture(scratch.path() / "sigkill.marker", 0, directive);

    ASSERT_TRUE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.exitCode, 128 + SIGKILL);
    EXPECT_EQ(run.result.diagnostic,
              "spawnAndWaitInherit: '" + resolved->string()
                  + "' was terminated by signal " + std::to_string(SIGKILL)
                  + " (reported as exit code " + std::to_string(128 + SIGKILL)
                  + ")");

    ASSERT_TRUE(run.marker.has_value()) << run.markerError;
    EXPECT_EQ(run.marker->args, directive)
        << "the child must have reached user code before dying, or 'killed by "
           "a signal' says nothing about the WIFSIGNALED arm";
}

// ★★ A FILE THAT RESOLVES AND STILL CANNOT BE EXECUTED — the last place
// `spawned` can be got wrong, and the ONLY pin that reaches the macOS arm's
// failure branch. Everything before this point succeeds: the path is taken as
// given, the file is a regular file, the execute bit is set. The image is still
// not runnable, and the OS says so at the moment of creation.
//
// The two POSIX arms learn that fact by different routes and the difference is
// the whole reason this pin matters. The fork arm hears it from the child, over
// the exec-status pipe. The `posix_spawn` arm hears it as the call's RETURN
// VALUE — which is that arm's entire error channel, so an implementation that
// dropped the check would set `spawned` for a process that was never created and
// then hand `waitpid` a pid of -1, reaping some unrelated child of this process
// and reporting ITS exit status as the build step's verdict.
//
// The errno is pinned exactly (ENOEXEC — "the file is not in an executable
// format", which is what a text file with no `#!` line is on both hosts) rather
// than accepted as "some failure": a diagnostic that named the wrong errno would
// send the operator after the wrong problem, and `posix_spawn` returning its
// code through the RETURN VALUE while leaving `errno` alone is the single most
// common way to get this call wrong.
//
// POSIX-only because the two POSIX arms are what is under test. The Windows
// counterpart already exists and makes the same statement about
// `CreateProcessW`: `ANonAsciiArgv0ReachesCreateProcessRatherThanNotFound`.
TEST(SpawnAndWaitInherit, AResolvableFileThatIsNotAnExecutableImageIsNotSpawned) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-not-an-image"};
    fs::path const tool = scratch.path() / "dss-not-an-executable-image";
    {
        std::ofstream file(tool, std::ios::binary);
        ASSERT_TRUE(file.good());
        // No `#!` and no magic number either host recognises. A shebang would
        // make this a perfectly runnable script and the pin would prove nothing.
        file << "this is not an executable image\n";
    }
    std::error_code ec;
    fs::permissions(tool,
                    fs::perms::owner_read | fs::perms::owner_write
                        | fs::perms::owner_exec,
                    fs::perm_options::replace, ec);
    ASSERT_FALSE(ec) << ec.message();
    // The control: the resolver's own `access(X_OK)` probe must PASS here, or
    // this would silently become the not-executable pin above wearing a
    // different name and the spawn arm would never be reached at all.
    ASSERT_EQ(::access(tool.c_str(), X_OK), 0)
        << "the fixture is not executable, so resolution would fail before any "
           "process creation was attempted";

    auto const resolved = resolveExecutableOnPath(tool.string());
    ASSERT_TRUE(resolved.has_value()) << tool.string();

    auto const plain = spawnAndWaitInherit({tool.string(), "--x"});
    EXPECT_FALSE(plain.spawned)
        << "the OS refused the image, so there is no process whose exit code "
           "could be reported: "
        << plain.diagnostic;
    EXPECT_EQ(plain.exitCode, 0);
    EXPECT_EQ(plain.diagnostic,
              expectedImageFailureDiagnostic(resolved->string(), "", ENOEXEC));

    // The SAME failure with a working directory, which the two arms report
    // differently on purpose: the fork arm's child chdir'd successfully and then
    // failed at `execv`, so it can name the stage; `posix_spawn` performed both
    // inside one call and cannot, so it says which two things the one errno
    // covers instead of picking one.
    fs::path const childDir = scratch.path() / "child cwd";
    ASSERT_TRUE(fs::create_directory(childDir));
    auto const withCwd = spawnAndWaitInherit({tool.string(), "--x"}, childDir);
    EXPECT_FALSE(withCwd.spawned) << withCwd.diagnostic;
    EXPECT_EQ(withCwd.diagnostic,
              expectedImageFailureDiagnostic(resolved->string(),
                                             childDir.string(), ENOEXEC));
}

// ★★ "INHERIT" IS IN THE FUNCTION'S NAME AND NOTHING CHECKED IT. The child is
// supposed to write to the COMPILER'S OWN stdout and stderr — that is why a
// build script's output appears live on the user's terminal instead of being
// buffered and replayed — and both POSIX arms achieve it by saying NOTHING about
// descriptors 0, 1 and 2. A property held by omission is precisely the kind a
// refactor deletes without noticing: `POSIX_SPAWN_CLOEXEC_DEFAULT` on the macOS
// arm would close all three unless each was re-inherited by an explicit `adddup2`
// action, and a stray `dup2` file action on either arm would redirect them. In
// both cases the child's output vanishes exactly when it is being captured, and
// every other pin in this file — all of which read a marker FILE — stays green.
//
// So the parent redirects its own 1 and 2 to files, spawns, restores, and reads
// what landed. The restore happens before the first assertion on purpose: a
// failure message streamed into the redirect would be swallowed by the very
// descriptors under test.
//
// POSIX-only because the descriptor is the unit of inheritance here. A Windows
// child receives the process std HANDLES, which `_dup2` on a CRT descriptor does
// not move, so the same test there would be exercising `SetStdHandle` rather
// than the spawn; that arm inherits through `bInheritHandles=TRUE` with no
// `STARTF_USESTDHANDLES`, which is a different mechanism and a different pin.
TEST(SpawnAndWaitInherit, TheChildWritesToTheParentsOwnStdoutAndStderr) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-stdio"};
    fs::path const outPath = scratch.path() / "inherited-stdout";
    fs::path const errPath = scratch.path() / "inherited-stderr";

    std::fflush(stdout);
    std::fflush(stderr);
    int const savedOut = ::dup(1);
    int const savedErr = ::dup(2);
    ASSERT_GE(savedOut, 0);
    ASSERT_GE(savedErr, 0);

    int const outFd =
        ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int const errFd =
        ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool const redirected = outFd >= 0 && errFd >= 0 && ::dup2(outFd, 1) >= 0
                         && ::dup2(errFd, 2) >= 0;

    FixtureRun run;
    if (redirected) {
        run = runFixture(scratch.path() / "stdio.marker", 0,
                         {std::string{kEchoStdioDirective}});
    }

    std::fflush(stdout);
    std::fflush(stderr);
    if (outFd >= 0) {
        ::close(outFd);
    }
    if (errFd >= 0) {
        ::close(errFd);
    }
    ::dup2(savedOut, 1);
    ::dup2(savedErr, 2);
    ::close(savedOut);
    ::close(savedErr);

    ASSERT_TRUE(redirected)
        << "could not redirect this process's own stdio, so the pin could not "
           "have observed anything";
    ASSERT_TRUE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.exitCode, 0);

    auto const outBlob = readFileBinary(outPath);
    auto const errBlob = readFileBinary(errPath);
    ASSERT_TRUE(outBlob.has_value()) << outPath.string();
    ASSERT_TRUE(errBlob.has_value()) << errPath.string();
    // Exact equality, not a substring: the child writes these bytes and nothing
    // else, so anything extra means something other than the fixture reached
    // that descriptor.
    EXPECT_EQ(*outBlob, std::string{kInheritedStdoutToken})
        << "the child's stdout did not land in the descriptor this process had "
           "on fd 1 — stdio was not inherited";
    EXPECT_EQ(*errBlob, std::string{kInheritedStderrToken})
        << "the child's stderr did not land in the descriptor this process had "
           "on fd 2";
}

#endif  // !_WIN32

// ★ AN EMBEDDED NUL IN AN ARGV ELEMENT IS REJECTED, AND NO CHILD IS CREATED.
// JSON permits U+0000 and `std::string` carries it, but a process argument
// cannot: on POSIX `execv` reads `element.data()` up to the NUL and the child
// gets a TRUNCATED argument; on Windows the NUL terminates `lpCommandLine` and
// every argument after it VANISHES. Both spellings let the child run, possibly
// exit 0, and the build report success having executed something other than
// what the manifest said — a silent breach of the header's "byte-identically"
// guarantee.
//
// The load-bearing half is the LAST assertion: the marker file the fixture
// always writes must not exist, i.e. the rejection happened before any process
// did. A guard that ran the child and then complained would pass everything
// above it.
TEST(SpawnAndWaitInherit, AnArgumentWithAnEmbeddedNulIsRejectedBeforeAnyChild) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn"};
    fs::path const marker = scratch.path() / "embedded-nul.marker";

    std::string payload = "before";
    payload.push_back('\0');
    payload += "after";
    ASSERT_EQ(payload.size(), std::size_t{12});

    auto const r = spawnAndWaitInherit(
        {selfPath().string(), std::string{kFixtureFlag}, marker.string(), "0",
         payload});

    EXPECT_FALSE(r.spawned);
    EXPECT_EQ(r.exitCode, 0);
    EXPECT_EQ(r.diagnostic,
              "spawnAndWaitInherit: argv[4] contains an embedded NUL byte, "
              "which no process argument can carry — the child would silently "
              "receive something other than what was asked for");
    EXPECT_FALSE(fs::exists(marker))
        << "the fixture wrote its marker, so a process WAS created with an "
           "argument that cannot survive the crossing";

    // The INDEX is part of the message, so it has to be the offending one and
    // not a constant. argv[0] is checked by the same loop, before the PATH
    // lookup that would otherwise fail first on this unspawnable name.
    std::string argv0 = "dss";
    argv0.push_back('\0');
    argv0 += "tool";
    auto const first = spawnAndWaitInherit({argv0, "--x"});
    EXPECT_FALSE(first.spawned);
    EXPECT_EQ(first.diagnostic,
              "spawnAndWaitInherit: argv[0] contains an embedded NUL byte, "
              "which no process argument can carry — the child would silently "
              "receive something other than what was asked for");
}

#if defined(_WIN32)

// The Windows half of the encoding fix, from the caller's side. A resolvable
// non-ASCII argv[0] must get all the way to `CreateProcessW` — which is only
// possible if `lpApplicationName` (built from the resolved path) and
// `lpCommandLine` (built by `tryBuildWindowsCommandLine`) came from the same
// UTF-8 decoder, the rule `windows_command_line.hpp` states over
// `decodeUtf8ToWide` and that this call site used to break.
//
// The fixture file is deliberately NOT a valid image, so the OS refuses it —
// and the refusal is the evidence: reaching a CreateProcessW error at all means
// resolution SUCCEEDED. Before the fix this reported the "not found" prose
// instead, on the MSVC leg only.
TEST(SpawnAndWaitInherit, ANonAsciiArgv0ReachesCreateProcessRatherThanNotFound) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-utf8"};
    fs::path const tool =
        scratch.path() / std::wstring{L"g\u00e9n\u00e9rateur.exe"};
    {
        std::ofstream file(tool, std::ios::binary);
        ASSERT_TRUE(file.good());
        file << "not a real executable image";
    }

    PathVarGuard guard;
    setPathVar(scratch.path().string());

    auto const r = spawnAndWaitInherit({"g\xC3\xA9n\xC3\xA9rateur", "--x"});
    EXPECT_FALSE(r.spawned);
    EXPECT_EQ(r.diagnostic.rfind(
                  "spawnAndWaitInherit: CreateProcessW failed for '", 0),
              std::size_t{0})
        << "expected the OS to refuse a non-image, i.e. resolution to have "
           "SUCCEEDED; got: "
        << r.diagnostic;
    EXPECT_EQ(r.diagnostic.find("was not found as an executable"),
              std::string::npos)
        << r.diagnostic;
}

// Malformed UTF-8 in argv[0] says SO, rather than borrowing the not-found
// prose. It cannot resolve — every candidate path is built by decoding those
// bytes — but "your PATH is wrong" is the one thing that is definitely not the
// problem, and it is the sentence an operator would act on.
TEST(SpawnAndWaitInherit, AnArgv0ThatIsNotUtf8IsReportedAsAnEncodingFault) {
    auto const r = spawnAndWaitInherit({"tool-\x80-name", "--x"});
    EXPECT_FALSE(r.spawned);
    EXPECT_EQ(r.diagnostic,
              "spawnAndWaitInherit: argv[0] is not valid UTF-8 (invalid UTF-8 "
              "lead byte 0x80 at offset 5), so it cannot name a program on "
              "this host");
}

#endif  // _WIN32

TEST(SpawnAndWaitInherit, AMissingWorkingDirectoryIsReportedBeforeSpawning) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn"};
    fs::path const absent = scratch.path() / "no-such-subdir";
    ASSERT_FALSE(fs::exists(absent));

    auto const r = spawnAndWaitInherit(
        {selfPath().string(), std::string{kFixtureFlag},
         (scratch.path() / "never.marker").string(), "0"},
        absent);
    EXPECT_FALSE(r.spawned);
    EXPECT_FALSE(r.diagnostic.empty());
    EXPECT_FALSE(fs::exists(scratch.path() / "never.marker"))
        << "the child must not have run at all";
}

TEST(SpawnAndWaitInherit, ReportsTheChildsExitCode) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn"};
    // 0 and 1 are the load-bearing pair (success vs. the ordinary failure a
    // build script reports); 42 and 200 prove the code is CARRIED, not
    // collapsed to a boolean.
    for (int code : {0, 1, 42, 200}) {
        auto const run = runFixture(
            scratch.path() / ("exit" + std::to_string(code) + ".marker"), code,
            {});
        ASSERT_TRUE(run.result.spawned)
            << "exit code " << code << ": " << run.result.diagnostic;
        EXPECT_EQ(run.result.exitCode, code) << run.result.diagnostic;
        EXPECT_TRUE(run.result.diagnostic.empty())
            << "a successful spawn must not carry a diagnostic: "
            << run.result.diagnostic;
        ASSERT_TRUE(run.marker.has_value()) << run.markerError;
    }
}

// ★ THE cwd PARAMETER IS HONORED — read out of the CHILD's own
// `current_path()`, not inferred from the call having been accepted.
// RED-ON-DISABLE is inside the test: the same fixture is run with NO cwd and
// must report the parent's directory instead, so an implementation that
// ignored the argument would make the two runs agree and fail the first
// assertion.
TEST(SpawnAndWaitInherit, TheChildStartsInTheRequestedWorkingDirectory) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn"};
    fs::path const childDir = scratch.path() / "child cwd with spaces";
    ASSERT_TRUE(fs::create_directory(childDir));

    fs::path const parentCwd = fs::current_path();
    ASSERT_FALSE(fs::equivalent(parentCwd, childDir))
        << "the test would be vacuous if the parent already sat in childDir";

    auto const withCwd =
        runFixture(scratch.path() / "cwd.marker", 0, {}, childDir);
    ASSERT_TRUE(withCwd.result.spawned) << withCwd.result.diagnostic;
    ASSERT_TRUE(withCwd.marker.has_value()) << withCwd.markerError;
    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(fs::path{withCwd.marker->cwd}, childDir, ec))
        << "child reported cwd '" << withCwd.marker->cwd << "', expected '"
        << childDir.string() << "'"
        << (ec ? " (equivalent() failed: " + ec.message() + ")"
               : std::string{});

    // The parent's own directory must be UNCHANGED — this facility never
    // chdir()s the compiler process (it runs under a per-CU thread pool).
    EXPECT_TRUE(fs::equivalent(fs::current_path(), parentCwd));

    auto const withoutCwd = runFixture(scratch.path() / "nocwd.marker", 0, {});
    ASSERT_TRUE(withoutCwd.result.spawned) << withoutCwd.result.diagnostic;
    ASSERT_TRUE(withoutCwd.marker.has_value()) << withoutCwd.markerError;
    EXPECT_TRUE(
        fs::equivalent(fs::path{withoutCwd.marker->cwd}, parentCwd, ec))
        << "with no cwd argument the child must inherit ours; it reported '"
        << withoutCwd.marker->cwd << "'";
}

// ★★ THE LOAD-BEARING TEST OF THE WHOLE DESIGN: NO SHELL, EVER.
//
// Every element below is something a shell would MANGLE — `$HOME` and
// `%PATH%` would be substituted, `&&`, `;` and `|` would split the command
// into several, `>` would redirect to a file, `~` and quotes would be
// re-interpreted, and unquoted whitespace would word-split. The child reports
// the bytes it actually received; they must be identical, element for
// element, count included.
//
// This is also the pin that a future "just shell out, it's simpler" change
// must break in order to land.
TEST(SpawnAndWaitInherit, ArgumentsReachTheChildByteIdenticallyWithNoShell) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn"};

    auto const& payload = hostileArgvPayload();

    auto const run = runFixture(scratch.path() / "noshell.marker", 0, payload);
    ASSERT_TRUE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.exitCode, 0);
    ASSERT_TRUE(run.marker.has_value()) << run.markerError;

    ASSERT_EQ(run.marker->args.size(), payload.size())
        << "the child received a DIFFERENT NUMBER of arguments — something "
           "split or joined them, which is what a shell does";
    for (std::size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(run.marker->args[i], payload[i])
            << "argv[" << i << "] was altered in transit: got ["
            << run.marker->args[i] << "] sent [" << payload[i] << "]";
    }
}

// ══ spawnAndWaitRedirectStdout ════════════════════════════════════════════
//
// The capture arm. It exists because dependency acquisition must READ what
// `git rev-parse` printed, and it captures into a FILE rather than an anonymous
// pipe because a pipe has a bounded kernel buffer and this facility has no
// timeout to escape a full one with. Both halves of that sentence are pinned
// below: the bytes must arrive, and the call must come back.

// ★★ THE HEADLINE. A child that writes 256 KiB — four times the largest pipe
// buffer on any host in this matrix — must be captured BYTE FOR BYTE, and the
// call must RETURN.
//
// ★ THE DEADLINE IS NOT DECORATION. This pin's failure mode is not a wrong
// value, it is no value at all: a capture with a bounded buffer and no
// concurrent drain wedges permanently once the child outruns it (measured, on
// only ~7 KB, as `run_binary.hpp`'s D-TEST-RUN-HARNESS-DRAIN-AFTER-EXIT-
// DEADLOCKS). Without `CallDeadline` the observable result of that regression
// is a CI job hanging until an outer timeout kills the suite — no test name and
// no red. With it, the hang becomes a named non-zero exit in 60 s.
//
// The marker assertion is what keeps a passing capture honest: a child that
// never reached user code cannot have written the bytes, so "the file matched"
// has to be accompanied by "and the fixture ran".
TEST(SpawnAndWaitRedirectStdout, ALargeStdoutIsCapturedByteForByteAndReturns) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect"};
    fs::path const    capturePath = scratch.path() / "flood.out";
    std::string const expected    = floodPayload(kFloodBytes);

    // The payload's own preconditions, asserted rather than assumed: a newline
    // would put the CRT's text-mode translation between the child and the file
    // and this would stop being a test of the redirection.
    ASSERT_EQ(expected.size(), kFloodBytes);
    ASSERT_EQ(expected.find('\n'), std::string::npos);
    ASSERT_EQ(expected.find('\r'), std::string::npos);

    RedirectRun run;
    {
        CallDeadline const deadline{
            kRedirectDeadline,
            "spawnAndWaitRedirectStdout with a child writing "
                + std::to_string(kFloodBytes) + " bytes to stdout"};
        run = runFixtureRedirected(
            scratch.path() / "flood.marker", 0,
            {std::string{kFloodStdoutDirective}, std::to_string(kFloodBytes)},
            capturePath);
    }

    ASSERT_TRUE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.exitCode, 0)
        << "exit 123 is the fixture reporting a SHORT WRITE to its own stdout, "
           "which is a different fault from a lossy capture: "
        << run.result.diagnostic;
    EXPECT_EQ(run.result.diagnostic, "")
        << "a successful capture must not carry a diagnostic";
    ASSERT_TRUE(run.marker.has_value())
        << "the child never reached user code, so the capture proves nothing: "
        << run.markerError;

    ASSERT_TRUE(run.captured.has_value()) << run.captureError;
    EXPECT_EQ(run.captured->size(), expected.size())
        << "the capture is a DIFFERENT SIZE: " << differenceReport(*run.captured,
                                                                   expected);
    EXPECT_EQ(firstDifference(*run.captured, expected), std::string::npos)
        << differenceReport(*run.captured, expected);
}

// The exit code is carried across the redirect arm exactly as it is across the
// inheriting one, and — the half that is easy to get wrong — a NON-ZERO exit
// still delivers everything the child printed. An implementation that only
// finished the capture on the success path would pass every assertion in the
// headline pin above and lose the output of exactly the runs an operator most
// needs to read.
//
// 0 and 1 are the load-bearing pair (a tool's success vs. its ordinary
// failure); 42 and 200 prove the code is CARRIED rather than collapsed to a
// boolean. Each run writes its own payload SIZE, so a capture left over from
// the previous iteration cannot satisfy the next.
TEST(SpawnAndWaitRedirectStdout, TheExitCodeIsCarriedAndOutputSurvivesFailure) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect-exit"};
    for (int code : {0, 1, 42, 200}) {
        std::string const suffix = std::to_string(code);
        std::size_t const bytes  = 4096u + static_cast<std::size_t>(code);
        std::string const expected = floodPayload(bytes);
        fs::path const capturePath = scratch.path() / ("exit" + suffix + ".out");

        auto const run = runFixtureRedirected(
            scratch.path() / ("exit" + suffix + ".marker"), code,
            {std::string{kFloodStdoutDirective}, std::to_string(bytes)},
            capturePath);

        ASSERT_TRUE(run.result.spawned)
            << "exit code " << code << ": " << run.result.diagnostic;
        EXPECT_EQ(run.result.exitCode, code) << run.result.diagnostic;
        EXPECT_TRUE(run.result.diagnostic.empty())
            << "a successful spawn must not carry a diagnostic: "
            << run.result.diagnostic;
        ASSERT_TRUE(run.captured.has_value()) << run.captureError;
        EXPECT_EQ(firstDifference(*run.captured, expected), std::string::npos)
            << "exit code " << code << ": "
            << differenceReport(*run.captured, expected);
    }
}

// ★ TRUNCATE, NEVER APPEND. The file is created with `CREATE_ALWAYS` /
// `O_TRUNC` precisely so a stale capture cannot survive underneath a shorter
// one and be read back as this run's answer — `git rev-parse` printing a
// 40-byte id over a previous 200-byte error message would otherwise hand the
// caller a commit id with somebody else's text glued to the end of it.
TEST(SpawnAndWaitRedirectStdout, AnExistingCaptureFileIsTruncatedNotAppendedTo) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect-trunc"};
    fs::path const capturePath = scratch.path() / "reused.out";

    // A stale capture that is LONGER than what the next run will write, so an
    // append (or a create-without-truncate) leaves a recognisable tail.
    std::string const stale = floodPayload(8192);
    {
        std::ofstream pre(capturePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(pre.good());
        pre.write(stale.data(), static_cast<std::streamsize>(stale.size()));
    }
    ASSERT_EQ(fs::file_size(capturePath), stale.size());

    std::string const expected = floodPayload(64);
    auto const        run      = runFixtureRedirected(
        scratch.path() / "trunc.marker", 0,
        {std::string{kFloodStdoutDirective}, "64"}, capturePath);

    ASSERT_TRUE(run.result.spawned) << run.result.diagnostic;
    ASSERT_TRUE(run.captured.has_value()) << run.captureError;
    EXPECT_EQ(run.captured->size(), expected.size())
        << "the previous run's bytes are still in the file — it was opened for "
           "append, or created without truncation";
    EXPECT_EQ(*run.captured, expected);
}

// ★★ THE FILE CANNOT BE CREATED, AND THAT IS REPORTED AS ITSELF. The one
// outcome this arm must never produce is the silent fallback: run the program
// with the compiler's own stdout, return its exit code, and leave the caller
// reading a file that does not exist or is empty. That reads as "the tool
// printed nothing", which is a WRONG ANSWER rather than an error.
//
// The load-bearing assertion is the last one: no marker, i.e. no child was
// created at all. A guard that spawned first and complained afterwards would
// pass everything above it.
TEST(SpawnAndWaitRedirectStdout, AnUncreatableCaptureFileIsReportedAndNoChildRuns) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect-badfile"};
    fs::path const marker      = scratch.path() / "never.marker";
    fs::path const capturePath = scratch.path() / "no-such-subdir" / "cap.out";
    ASSERT_FALSE(fs::exists(capturePath.parent_path()))
        << "the fixture directory must NOT exist, or the open would succeed";

    auto const run = runFixtureRedirected(marker, 0, {}, capturePath);

    EXPECT_FALSE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.exitCode, 0);
    EXPECT_EQ(run.result.diagnostic.rfind(
                  "spawnAndWaitRedirectStdout: could not create the stdout "
                  "redirect file '" + capturePath.string() + "' (",
                  0),
              std::size_t{0})
        << "the diagnostic must open by naming the file it could not create: "
        << run.result.diagnostic;
    EXPECT_NE(run.result.diagnostic.find("was not started"), std::string::npos)
        << "the message must say the program did NOT run, or a reader could "
           "take it for a warning about a capture that merely failed: "
        << run.result.diagnostic;
    EXPECT_FALSE(fs::exists(marker))
        << "a child was created despite the capture being impossible — it ran "
           "with the compiler's own stdout and the caller would have been "
           "handed a clean exit code with nothing to read";
}

// An EMPTY path is the internal sentinel for "inherit", so letting it through
// this entry point would turn the one function whose purpose is to capture
// output into one that silently produces none. Refused, by exact message.
TEST(SpawnAndWaitRedirectStdout, AnEmptyCapturePathIsRefusedNotTreatedAsInherit) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect-empty"};
    fs::path const marker = scratch.path() / "never.marker";

    auto const r = spawnAndWaitRedirectStdout(
        {selfPath().string(), std::string{kFixtureFlag}, marker.string(), "0"},
        /*cwd=*/{}, /*stdoutFile=*/{});

    EXPECT_FALSE(r.spawned);
    EXPECT_EQ(r.diagnostic,
              "spawnAndWaitRedirectStdout: no stdout file was named. This "
              "entry point exists to CAPTURE the child's output, so an empty "
              "path cannot mean 'inherit' — call spawnAndWaitInherit when "
              "inheriting is what is wanted.");
    EXPECT_FALSE(fs::exists(marker))
        << "the call spawned the child anyway, with its output going wherever "
           "ours goes";
}

// ★ A SPAWN FAILURE ON THIS ARM NAMES THIS ARM. Every diagnostic in the shared
// implementation opens with the entry point the CALLER invoked, not the one the
// code was first written for — a message reading `spawnAndWaitInherit` here
// would send a reader to a function that was never called. Pinned as an EXACT
// string, because the rest of the sentence is the inheriting spawn's, letter
// for letter, and only the prefix distinguishes them.
//
// The second half is the file: a spawn that never happened must not leave an
// empty capture behind, because a caller that read one could not tell "the tool
// was not found" from "the tool printed nothing".
TEST(SpawnAndWaitRedirectStdout, AnAbsentProgramNamesThisEntryPointAndLeavesNoFile) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect-missing"};
    fs::path const capturePath = scratch.path() / "never-created.out";

    auto const r = spawnAndWaitRedirectStdout(
        {"dss-no-such-program-3f1c9a2e-please-fail", "--whatever"},
        /*cwd=*/{}, capturePath);

    EXPECT_FALSE(r.spawned);
    EXPECT_EQ(r.diagnostic,
              "spawnAndWaitRedirectStdout: "
              "'dss-no-such-program-3f1c9a2e-please-fail' was not found as an "
              "executable. It was looked up in PATH (the current directory is "
              "deliberately never searched); pass a path containing a "
              "directory separator to run a local tool.");
    EXPECT_FALSE(fs::exists(capturePath))
        << "an empty capture file was left behind by a spawn that never "
           "happened — a caller reading it cannot tell 'not found' from "
           "'printed nothing'";
}

// ★★ ONLY STDOUT MOVES. stderr and stdin must still be the compiler's own, and
// on Windows that is not free: `STARTF_USESTDHANDLES` is all-or-nothing, so the
// moment stdout is supplied explicitly the other two must be supplied too — and
// supplied as INHERITABLE duplicates, which the parent's own handles need not
// be. The natural way to get it wrong is to set `hStdOutput` and leave the
// other two fields zeroed, which closes the child's stdin and stderr while
// every stdout pin above stays green.
//
// `ParentStdioSwap` installs a deliberately NON-inheritable stderr handle on
// Windows, so this test fails if the substrate stops duplicating. It also
// points this process's stdin at the null device, which is what stops the
// stdin assertion being vacuous on a runner whose own stdin is closed.
//
// The swap is restored BEFORE the first assertion: a failure message streamed
// into the redirected stderr would be swallowed by the very stream under test.
TEST(SpawnAndWaitRedirectStdout, OnlyStdoutMovesStderrAndStdinKeepInheriting) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect-stdio"};
    fs::path const capturePath = scratch.path() / "child-stdout";
    fs::path const errPath     = scratch.path() / "parent-stderr";
    fs::path const marker      = scratch.path() / "probe.marker";

    RedirectRun run;
    bool        swapped = false;
    std::string swapError;
    bool        parentStdinLive = false;
    {
        ParentStdioSwap swap{errPath};
        swapped   = swap.ok();
        swapError = swap.error();
        if (swapped) {
            parentStdinLive = stdinIsLive();
            run             = runFixtureRedirected(
                marker, 0, {std::string{kProbeStdioDirective}}, capturePath);
        }
    }

    ASSERT_TRUE(swapped)
        << "could not move this process's own stderr/stdin, so the pin could "
           "not have observed anything: "
        << swapError;
    ASSERT_TRUE(parentStdinLive)
        << "the null device did not give this process a readable stdin, so the "
           "child's stdin assertion below would prove nothing";
    ASSERT_TRUE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.exitCode, 0) << run.result.diagnostic;

    ASSERT_TRUE(run.captured.has_value()) << run.captureError;
    // Exact equality, not a substring: the child writes this token and nothing
    // else to stdout, so anything extra means something other than the fixture
    // reached the capture.
    EXPECT_EQ(*run.captured, std::string{kInheritedStdoutToken})
        << "the child's stdout did not land in the capture file";

    auto const errBlob = readFileBinary(errPath);
    ASSERT_TRUE(errBlob.has_value()) << errPath.string();
    EXPECT_EQ(*errBlob, std::string{kInheritedStderrToken})
        << "the child's stderr did not reach the stream THIS process had on "
           "stderr — redirecting stdout took stderr with it, so a build tool's "
           "error text would vanish exactly when its output was being captured";

    ASSERT_TRUE(run.marker.has_value()) << run.markerError;
    EXPECT_EQ(run.marker->stdinProbe, "live")
        << "the child was handed a dead stdin while this process had a live "
           "one — STARTF_USESTDHANDLES supplies all three descriptors or none, "
           "and stdin was dropped on the floor";
}

// ★ THE CHILD'S cwd AND THE CAPTURE FILE ANSWER TO DIFFERENT DIRECTORIES, ON
// PURPOSE. `cwd` moves the CHILD; the capture file is created by the PARENT and
// therefore resolves against the CALLER's directory — the same rule argv[0]
// resolution follows, and the reason a caller can compute a relative path and
// still find the file afterwards.
//
// A RELATIVE capture path is what makes this pin real: with an absolute one,
// both readings agree and the test would be vacuous. The parent's directory is
// moved to the scratch root for the duration and restored BEFORE any assertion,
// so a red test cannot corrupt every test after it.
TEST(SpawnAndWaitRedirectStdout, TheCaptureIsRelativeToTheCallerNotToTheChildsCwd) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect-cwd"};
    fs::path const childDir = scratch.path() / "child cwd with spaces";
    ASSERT_TRUE(fs::create_directory(childDir));

    fs::path const restore = fs::current_path();
    ASSERT_FALSE(fs::equivalent(restore, childDir));

    fs::current_path(scratch.path());
    auto const run = runFixtureRedirected(scratch.path() / "cwd.marker", 0,
                                          {std::string{kFloodStdoutDirective},
                                           "64"},
                                          fs::path{"relative-capture.out"},
                                          childDir);
    fs::current_path(restore);

    ASSERT_TRUE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.exitCode, 0) << run.result.diagnostic;

    ASSERT_TRUE(run.marker.has_value()) << run.markerError;
    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(fs::path{run.marker->cwd}, childDir, ec))
        << "child reported cwd '" << run.marker->cwd << "', expected '"
        << childDir.string() << "'";

    EXPECT_TRUE(fs::exists(scratch.path() / "relative-capture.out"))
        << "the capture did not land in the CALLER's directory";
    EXPECT_FALSE(fs::exists(childDir / "relative-capture.out"))
        << "the capture landed in the CHILD's working directory — the file was "
           "opened after the chdir, so a caller could not find what it asked "
           "for";
    auto const captured =
        readFileBinary(scratch.path() / "relative-capture.out");
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(*captured, floodPayload(64));
}

#if !defined(_WIN32)

// ★★ THE CAPTURE FILE LANDS ON DESCRIPTOR 1, AND THE CHILD STILL GETS IT.
// `open` hands back the LOWEST free descriptor, so a process that has already
// closed its own stdout gets descriptor 1 for the capture — and `dup2(1, 1)` is
// specified as a NO-OP that returns immediately WITHOUT clearing FD_CLOEXEC,
// the exact opposite of the normal case the fork arm relies on. The exec would
// then close the descriptor the program was supposed to write through and the
// tool would run with no stdout at all: an empty capture and a clean exit code,
// which is the silent degradation this entry point exists to refuse — reached
// through an allocator detail rather than a failed syscall.
//
// The substrate re-homes any such descriptor above 2 with `F_DUPFD_CLOEXEC`.
// This pin produces the collision for real: it closes descriptor 1 for the
// duration of one call, which is the only way to make `open` return it.
//
// POSIX-only, because the hazard is: Windows handles are not small integers and
// `CreateFileW` cannot return "the one the child's stdout needs".
//
// Descriptor 1 is restored BEFORE any assertion — gtest reports through it, and
// a failure printed into the capture file would be both invisible and a
// corruption of the thing being measured.
TEST(SpawnAndWaitRedirectStdout, TheCaptureSurvivesLandingOnDescriptorOne) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-redirect-fd1"};
    fs::path const    capturePath = scratch.path() / "fd1.out";
    std::string const expected    = floodPayload(4096);

    std::fflush(stdout);
    int const savedOut = ::dup(1);
    ASSERT_GE(savedOut, 0);

    RedirectRun run;
    int         observedFd = -1;
    bool const  closed     = ::close(1) == 0;
    if (closed) {
        // The control, taken inside the window: prove descriptor 1 really is
        // what an `open` would now return, so the pin cannot pass by never
        // having produced the collision at all. The probe is closed again
        // immediately, leaving 1 free for the call under test.
        observedFd = ::open(capturePath.c_str(), O_WRONLY | O_CREAT, 0644);
        if (observedFd >= 0) {
            ::close(observedFd);
        }
        run = runFixtureRedirected(
            scratch.path() / "fd1.marker", 0,
            {std::string{kFloodStdoutDirective}, "4096"}, capturePath);
    }
    ::dup2(savedOut, 1);
    ::close(savedOut);
    std::fflush(stdout);

    ASSERT_TRUE(closed)
        << "could not close this process's own stdout, so the collision was "
           "never produced and the pin would prove nothing";
    ASSERT_EQ(observedFd, 1)
        << "an open() with descriptor 1 free returned " << observedFd
        << ", not 1 — the collision this pin exists for did not happen";

    ASSERT_TRUE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.exitCode, 0) << run.result.diagnostic;
    ASSERT_TRUE(run.captured.has_value()) << run.captureError;
    EXPECT_EQ(firstDifference(*run.captured, expected), std::string::npos)
        << "the child ran with no usable stdout: "
        << differenceReport(*run.captured, expected);
}

#endif  // !_WIN32

// ══ CONCURRENT SPAWNS — the substrate's re-entrancy, executed ═════════════
//
// ★★ EVERY RE-ENTRANCY PROPERTY OF THIS SUBSTRATE USED TO BE DERIVED FROM
// READING. There was no concurrent-spawn test anywhere in the repository: that
// `waitpid` is pid-specific and cannot collect a sibling's child, that
// `WaitForSingleObject` is handle-specific, that no non-const static holds a
// spawn's state, that `getenv`'s result is copied before anything can invalidate
// it — all of it was an argument about the source, and an argument about the
// source is exactly what a refactor is free to invalidate without noticing.
//
// It matters NOW because the substrate is growing its second consumer (`git`
// dependency acquisition alongside the build hooks), and because the honest
// reading of today's call graph is that NOTHING is concurrent yet — see "THE
// THREADING PREMISE" in `process_spawn.cpp`. A property nothing exercises and
// nothing asserts is a property the next change silently removes.
//
// ★ WHAT MAKES THIS MORE THAN "NOTHING CRASHED". Each call is given its OWN
// exit code, its OWN marker path and its OWN two-element payload, all derived
// from a unique index. So:
//   * a CROSSED EXIT STATUS — thread A reaping thread B's child — shows up as a
//     mismatched exit code, not as a hang;
//   * a STOLEN or OVERWRITTEN MARKER shows up as a payload that belongs to a
//     different index;
//   * a DROPPED spawn shows up in the count.
// Assertions are exact counts and exact contents, and every one of them is made
// on the MAIN thread after the join: gtest's `ASSERT_*` only returns from the
// function it appears in, so an assertion inside a worker would abandon that
// worker's remaining loop and report nothing useful about it.
//
// ⚠ NO `setenv`/`putenv` FROM THE THREADS, deliberately. `src/` contains no
// environment writer, and the whole safety argument for the resolver's
// `std::getenv` rests on that; a test that introduced one would be exercising a
// shape the real path never has, and would make its own green meaningless.
// `argv[0]` here carries a directory separator, so the PATH search is skipped
// entirely on the POSIX arm; on Windows only the read-only `PATHEXT` lookup
// runs, concurrently, which is the real shape.
//
// K and M are deliberately small. This runs on every leg including the
// operator's laptop, and each iteration is a real process creation of a
// multi-megabyte gtest binary.
namespace {
constexpr int kConcurrentThreads   = 4;
constexpr int kSpawnsPerThread     = 3;
constexpr int kConcurrentTotal     = kConcurrentThreads * kSpawnsPerThread;

// One call's complete observation, filled in by a worker and judged on the main
// thread. Copying the marker out here (rather than re-reading the file later)
// is what makes a stolen marker detectable: the file is read at the moment the
// call completed, so a later overwrite cannot repair the evidence.
struct ConcurrentOutcome {
    bool                     spawned      = false;
    int                      exitCode     = -1;
    std::string              diagnostic;
    bool                     markerParsed = false;
    std::string              markerError;
    std::vector<std::string> markerArgs;
};

// Release every worker at once. A plain "start the threads and hope" gives the
// first thread a head start big enough to serialise the whole test on a busy
// machine — and a serialised concurrency test is a green that means nothing.
void rendezvous(std::atomic<int>& arrived, int participants) {
    arrived.fetch_add(1, std::memory_order_acq_rel);
    while (arrived.load(std::memory_order_acquire) < participants) {
        std::this_thread::yield();
    }
}

// The unique payload for call `index`. TWO elements, not one: a single element
// would still compare equal if the substrate delivered a truncated argv.
std::vector<std::string> concurrentPayload(int index) {
    return {"dss-concurrent-token-" + std::to_string(index),
            "slot=" + std::to_string(index)};
}
} // namespace

TEST(SpawnAndWaitInherit, ConcurrentSpawnsEachGetTheirOwnChildAndTheirOwnVerdict) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn-concurrent"};

    std::vector<ConcurrentOutcome> outcomes(
        static_cast<std::size_t>(kConcurrentTotal));
    std::atomic<int>         arrived{0};
    std::vector<std::thread> workers;
    workers.reserve(kConcurrentThreads);

    for (int t = 0; t < kConcurrentThreads; ++t) {
        workers.emplace_back([&, t] {
            rendezvous(arrived, kConcurrentThreads);
            for (int m = 0; m < kSpawnsPerThread; ++m) {
                int const index = t * kSpawnsPerThread + m;
                // Exit codes 1..kConcurrentTotal — never 0, so a call whose
                // status was collected from the wrong child (or never collected
                // and left at the struct's default) cannot coincide with a
                // legitimate "clean exit" value.
                int const  exitCode = index + 1;
                auto const marker =
                    scratch.path()
                    / ("concurrent-" + std::to_string(index) + ".marker");
                auto const run = runFixture(marker, exitCode,
                                            concurrentPayload(index));

                auto& out        = outcomes[static_cast<std::size_t>(index)];
                out.spawned      = run.result.spawned;
                out.exitCode     = run.result.exitCode;
                out.diagnostic   = run.result.diagnostic;
                out.markerParsed = run.marker.has_value();
                out.markerError  = run.markerError;
                if (run.marker.has_value()) {
                    out.markerArgs = run.marker->args;
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    // Exact counts, not "no exceptions". Every one of these is a different way
    // for the substrate to be wrong under concurrency, so each is counted and
    // named separately rather than folded into one boolean.
    int notSpawned = 0;
    int wrongExit  = 0;
    int noMarker   = 0;
    int wrongArgs  = 0;
    int noisy      = 0;
    std::string firstFailure;
    auto const note = [&firstFailure](std::string what) {
        if (firstFailure.empty()) { firstFailure = std::move(what); }
    };

    for (int index = 0; index < kConcurrentTotal; ++index) {
        auto const& out = outcomes[static_cast<std::size_t>(index)];
        if (!out.spawned) {
            ++notSpawned;
            note("call " + std::to_string(index) + " reported NOT spawned: "
                 + out.diagnostic);
            continue;
        }
        if (!out.diagnostic.empty()) {
            ++noisy;
            note("call " + std::to_string(index)
                 + " succeeded but carried a diagnostic: " + out.diagnostic);
        }
        if (out.exitCode != index + 1) {
            ++wrongExit;
            note("call " + std::to_string(index) + " expected exit "
                 + std::to_string(index + 1) + " but got "
                 + std::to_string(out.exitCode)
                 + " — a wait collected the wrong child's status");
        }
        if (!out.markerParsed) {
            ++noMarker;
            note("call " + std::to_string(index) + " has no marker: "
                 + out.markerError);
            continue;
        }
        if (out.markerArgs != concurrentPayload(index)) {
            ++wrongArgs;
            std::string got;
            for (auto const& arg : out.markerArgs) { got += "[" + arg + "]"; }
            note("call " + std::to_string(index)
                 + " read a marker belonging to someone else: " + got);
        }
    }

    EXPECT_EQ(notSpawned, 0)
        << kConcurrentTotal
        << " concurrent spawns, " << notSpawned
        << " never started. First: " << firstFailure;
    EXPECT_EQ(wrongExit, 0)
        << "a concurrent wait collected another call's exit status ("
        << wrongExit << " of " << kConcurrentTotal << "). First: "
        << firstFailure;
    EXPECT_EQ(noMarker, 0)
        << noMarker << " of " << kConcurrentTotal
        << " children never wrote their marker. First: " << firstFailure;
    EXPECT_EQ(wrongArgs, 0)
        << wrongArgs << " of " << kConcurrentTotal
        << " markers held another call's payload — argv crossed between "
           "concurrent spawns. First: "
        << firstFailure;
    EXPECT_EQ(noisy, 0)
        << "a successful concurrent spawn carried a diagnostic. First: "
        << firstFailure;

    // Every marker distinct, asserted independently of the per-call loop: two
    // calls that BOTH read the same (correct-looking) payload would satisfy
    // every check above if the payload happened to match one of them.
    std::set<std::vector<std::string>> distinct;
    for (auto const& out : outcomes) {
        distinct.insert(out.markerArgs);
    }
    EXPECT_EQ(distinct.size(), static_cast<std::size_t>(kConcurrentTotal))
        << "only " << distinct.size() << " distinct payloads came back from "
        << kConcurrentTotal
        << " calls — two calls observed the same child, so at least one "
           "marker was stolen or overwritten";
}

#if !defined(_WIN32)

// ★★ THE `errnoText` / `std::strerror` DECISION, EXECUTED.
//
// `errnoText` composes eleven of this facility's failure messages and used to
// call `std::strerror`, under a comment arguing the thread-safety exposure was
// nil. ⚠ MEASURED FALSE on Darwin 25.5 — but the true statement is narrower
// than "strerror is racy", and the narrowness is what this pin has to be built
// around. 12 threads, each on its OWN code, 20,000 calls apiece:
//
//   codes the libc KNOWS     0 wrong strings. `strerror` returns an immutable
//                            table entry — no buffer, nothing to race.
//   codes it does NOT know   33,692 wrong out of 240,000, rising to 100,265
//                            once the callers do the string work `errnoText`
//                            really does. "Unknown error: N" is formatted into
//                            ONE shared static buffer.
//
// glibc 2.39 is 0 on both populations. The substrate now uses `strerror_r`,
// chosen by overload resolution rather than a feature-macro guess.
//
// ★★ THE FIRST VERSION OF THIS PIN WAS STRUCTURALLY INCAPABLE OF FAILING, and
// that is the lesson worth more than the fix. It used eleven known codes and a
// single unknown one — so exactly ONE thread ever touched the shared buffer,
// nothing could overwrite anything, and the pin passed 3 runs for 3 with the
// defect deliberately restored. MEASURED, then diagnosed, then rebuilt: it now
// puts EIGHT threads on unknown codes, and the same lever fails it every run.
// A "modest" test that touches the racy path once is not a weak pin; it is a
// green light wired to nothing.
//
// ★ WHY IT DRIVES `interpretExecHandshake` AND NOT A SPAWN. A shared buffer
// that every thread fills with the SAME text corrupts nothing observable, and
// concurrent spawns of one fixture all fail with ONE errno — so a spawn-based
// version of this test would report green against the racy implementation for
// the same reason the first draft did. `interpretExecHandshake` takes the errno
// as a PARAMETER, the only surface in this facility that lets a caller vary it,
// and composes its message through `errnoText`.
//
// Feeding it codes no kernel currently returns is deliberate and in keeping
// with this file's existing practice (`kStandInRecordBytes` pins the decision,
// not the layout). The contract under test is "the text for MY errno is never
// another thread's text, for any errno" — and the unknown region is simply
// where a broken implementation is detectable. It is not a hypothetical region
// either: `Unknown error: N` exists because the set of numbers a kernel may
// return is not closed.
//
// Expectations are computed SERIALLY, before any thread exists, and by a
// DIFFERENT route than the code under test (`std::strerror`, safe here exactly
// because nothing else is running). MEASURED on both POSIX legs: the two
// spellings agree byte-for-byte across all 145 codes probed, so the comparison
// is a real cross-check rather than a tautology.
//
// RED ON REVERT: restore `std::strerror` in `errnoText` and this fails on the
// macOS leg every run. It stays GREEN on Linux either way, and the pin says so
// rather than pretending to cover a leg it cannot.
namespace {
// ONE THREAD PER ENTRY. Four codes the substrate genuinely sees, so the pin is
// anchored in real inputs — and eight the libc cannot name, because those are
// the only ones that can expose a shared buffer. MEASURED at this shape and
// this iteration count: thousands of mismatches with the revert applied, zero
// without it, on both POSIX legs.
std::vector<int> const& concurrentErrnoCodes() {
    static std::vector<int> const codes{
        ENOENT, EACCES, ENOEXEC, ENOTDIR,
        4242,   31337,  65535,   999,
        8888,   7777,   6666,    5555};
    return codes;
}

// The four strings a verdict may name, built here rather than spelled at each
// call site. They travel as a STRUCT because positionally they are four
// same-typed strings in a row: a swap between `exePath` and `cwdPath` compiles
// perfectly and reports every message backwards, and a factory is the cheapest
// way for every pin to be wrong or right together rather than one at a time.
HandshakeContext inheritContext() {
    // `stdoutPath` empty — the inheriting spawn redirects nothing, so its child
    // has no 'D' stage to reach and nothing to name if it did.
    return HandshakeContext{"spawnAndWaitInherit", "/opt/tools/gen", "/work",
                            std::string{}};
}

HandshakeContext redirectContext() {
    return HandshakeContext{"spawnAndWaitRedirectStdout", "/opt/tools/gen",
                            "/work", "/var/tmp/rev-parse.out"};
}
} // namespace

TEST(InterpretExecHandshake, ConcurrentVerdictsNeverBorrowAnotherThreadsErrnoText) {
    auto const& codes = concurrentErrnoCodes();

    // Serial references. Nothing is running yet, so `std::strerror` is safe
    // here even on the host where it is not safe under threads.
    std::vector<std::string> expected;
    expected.reserve(codes.size());
    for (int code : codes) {
        expected.push_back("spawnAndWaitInherit: execv('/opt/tools/gen') failed ("
                           + expectedErrnoText(code) + ")");
    }

    constexpr int kIterations = 2000;
    std::atomic<int>         arrived{0};
    std::atomic<int>         mismatches{0};
    std::atomic<int>         wrongVerdicts{0};
    std::vector<std::thread> workers;
    workers.reserve(codes.size());

    for (std::size_t slot = 0; slot < codes.size(); ++slot) {
        workers.emplace_back([&, slot] {
            rendezvous(arrived, static_cast<int>(codes.size()));
            ChildFailure const failure{
                'X', codes[slot]};
            for (int i = 0; i < kIterations; ++i) {
                auto const verdict = interpretExecHandshake(
                    sizeof(ChildFailure), sizeof(ChildFailure), 0, &failure,
                    inheritContext());
                if (verdict.spawned) {
                    wrongVerdicts.fetch_add(1, std::memory_order_relaxed);
                }
                if (verdict.diagnostic != expected[slot]) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(wrongVerdicts.load(), 0)
        << "a concurrent call reported SPAWNED for a whole failure record — "
           "the verdict itself is not re-entrant, which is far worse than "
           "garbled text";
    EXPECT_EQ(mismatches.load(), 0)
        << mismatches.load() << " of "
        << (static_cast<int>(codes.size()) * kIterations)
        << " concurrent diagnostics carried the WRONG errno text — a thread "
           "read another thread's message out of a shared libc buffer. The "
           "substrate must compose this through `strerror_r`, never "
           "`std::strerror`.";
}

// ══ detail::interpretExecHandshake — every arm of the exec-status verdict ══
//
// ★★ THIS IS THE ARM THAT WAS WRONG AND COULD NOT BE REACHED. On POSIX the
// parent learns whether `execv` happened by reading a CLOEXEC self-pipe: EOF
// means it took, a whole record means it did not. Reading and INTERPRETING used
// to be one fused block, and a SHORT read — or a `read` that failed for
// anything but EINTR — fell straight through the "whole record?" test into
// `spawned = true`. `waitpid` then collected the child's own `_exit(127)` and
// the caller reported "ran and exited with status 127 — the script's own
// verdict" about a program that may never have executed an instruction.
//
// Nothing a caller can pass through `spawnAndWaitInherit` forces a partial read
// or a failing `read`, so that arm was structurally unreachable from a test and
// a fix left fused with the I/O would have been just as unreachable — green
// whether it was there or not. The decision is therefore its own PURE function,
// and these are its arms, one call each, exact strings only.
//
// POSIX-only because the handshake is: the Windows arm uses `CreateProcessW`
// and has no pipe to interpret.
//
// ★ AND UNGATED ACROSS ALL OF POSIX, INCLUDING THE HOST WITH NO CALLER. macOS
// spawns through `posix_spawn`, which reports exec failure through its return
// value, so nothing in `src/` reads an exec-status pipe there. These pins still
// run on that leg, deliberately: the function is pure and compiled everywhere
// (see the header's note on why), and narrowing the pins to the hosts that call
// it would be a pin that silently does not run on a platform — the masked
// coverage this project treats as a defect. The rule is one-way and absolute:
// the function and its pins are gated identically or not at all.
//
// `expectedBytes` is a PARAMETER, so these pins pass a stand-in record size and
// do not depend on the real `sizeof(ChildFailure)` — they pin the decision, not
// the layout.
namespace {
constexpr std::size_t kStandInRecordBytes = 8;

// A record whose contents would be WRONG to report. Every arm that must not
// read the record is handed this one, so an implementation that read it anyway
// fails with recognisably poisoned prose rather than passing by luck.
ChildFailure poisonedRecord() { return ChildFailure{'C', EACCES}; }
} // namespace

TEST(InterpretExecHandshake, EofWithNoErrorIsACleanExecAndReadsNoRecord) {
    ChildFailure const poison = poisonedRecord();
    auto const         verdict =
        interpretExecHandshake(0, kStandInRecordBytes, 0, &poison,
                               inheritContext());
    EXPECT_TRUE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic, "")
        << "a successful spawn must carry no diagnostic — `build_scripts.cpp` "
           "uses that emptiness to tell a clean exit from a stand-in status";
}

TEST(InterpretExecHandshake, AWholeRecordFromExecvIsNotSpawnedAndNamesTheImage) {
    ChildFailure const failure{'X', ENOENT};
    auto const         verdict =
        interpretExecHandshake(kStandInRecordBytes, kStandInRecordBytes, 0,
                               &failure, inheritContext());
    EXPECT_FALSE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitInherit: execv('/opt/tools/gen') failed ("
                  + expectedErrnoText(ENOENT) + ")");
}

TEST(InterpretExecHandshake, AWholeRecordFromChdirNamesTheDirectoryNotTheImage) {
    // The two stages are different remediations — a missing tool versus a
    // missing working directory — so they must not share one sentence.
    ChildFailure const failure{'C', ENOTDIR};
    auto const         verdict =
        interpretExecHandshake(kStandInRecordBytes, kStandInRecordBytes, 0,
                               &failure, inheritContext());
    EXPECT_FALSE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitInherit: the child could not enter working "
              "directory '/work' (" + expectedErrnoText(ENOTDIR) + ")");
}

// ★★ THE 'D' STAGE: THE CHILD COULD NOT INSTALL THE STDOUT REDIRECT AND
// REFUSED TO RUN THE PROGRAM. This is the arm that keeps the capture honest
// from the far side of a fork. A child that failed the `dup2` and exec'd anyway
// would run the tool with the COMPILER's stdout and hand the caller its exit
// code — a clean verdict and an empty file, which is a wrong ANSWER rather than
// an error. So the message has to say two things and both are asserted: which
// FILE could not be attached, and that the program was NOT executed.
//
// It also carries the OTHER entry point's name, which is why the context is a
// parameter: this stage is unreachable for `spawnAndWaitInherit`, whose child
// redirects nothing.
TEST(InterpretExecHandshake, AStdoutRedirectFailureNamesTheFileAndSaysNothingRan) {
    ChildFailure const failure{'D', EBADF};
    auto const         verdict =
        interpretExecHandshake(kStandInRecordBytes, kStandInRecordBytes, 0,
                               &failure, redirectContext());
    EXPECT_FALSE(verdict.spawned)
        << "the program was never executed, so there is no exit code and no "
           "process — reporting it as spawned would let the caller read an "
           "empty capture as the tool's own output";
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitRedirectStdout: the child could not redirect its "
              "stdout to '/var/tmp/rev-parse.out' ("
                  + expectedErrnoText(EBADF)
                  + "), so it did NOT execute '/opt/tools/gen' — running the "
                    "program with the compiler's own stdout attached would "
                    "have produced an exit code that looked clean and a file "
                    "with nothing in it");
}

// A WHOLE record carrying a stage byte nobody writes. "The handshake broke" is
// false — every byte arrived — and folding it onto the nearest known stage
// would print a guess in the child's voice. The old two-arm form did exactly
// that: anything that was not 'C' was reported as an `execv` failure, which
// with three stages would have made a redirect fault read as a missing program.
//
// The byte is rendered as REAL hex. `"0x" + std::to_string(b)` yields decimal
// behind a hex prefix and can name a value no byte can hold.
TEST(InterpretExecHandshake, AnUnrecognisedStageIsReportedAsItselfNotAsExecFailure) {
    ChildFailure const failure{'\x7f', EACCES};
    auto const         verdict =
        interpretExecHandshake(kStandInRecordBytes, kStandInRecordBytes, 0,
                               &failure, inheritContext());
    EXPECT_FALSE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitInherit: the child of '/opt/tools/gen' reported "
              "failure stage 0x7f, which this version does not recognise ("
                  + expectedErrnoText(EACCES)
                  + "); reporting it as not spawned, because a record whose "
                    "stage cannot be read is not a record saying the program "
                    "ran.");
}

TEST(InterpretExecHandshake, APartialRecordIsNotSpawnedAndSaysTheHandshakeBroke) {
    // ★ THE DEFECT, DIRECTLY. Three bytes of eight: not EOF, not a record.
    // The old code called this "the child ran".
    ChildFailure const poison = poisonedRecord();
    auto const         verdict =
        interpretExecHandshake(3, kStandInRecordBytes, 0, &poison,
                               inheritContext());
    EXPECT_FALSE(verdict.spawned)
        << "a torn handshake means we do not KNOW whether exec happened; "
           "reporting it as spawned is the 127 confusion the flag exists to "
           "prevent";
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitInherit: the exec-status handshake with the child "
              "of '/opt/tools/gen' broke — 3 of 8 bytes were read, so whether "
              "the program ever started is UNKNOWN; reporting it as not "
              "spawned rather than guessing that it ran.");
}

TEST(InterpretExecHandshake, AReadErrorWithNoBytesIsNotMistakenForCleanEof) {
    // Zero bytes AND a failed read look identical if only the count is
    // consulted — and "zero bytes" is the arm that means SUCCESS. This is the
    // ordering pin: the error must disqualify the clean arm before the count
    // is ever compared.
    auto const verdict = interpretExecHandshake(0, kStandInRecordBytes, EIO,
                                                nullptr, inheritContext());
    EXPECT_FALSE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitInherit: the exec-status handshake with the child "
              "of '/opt/tools/gen' broke — 0 of 8 bytes were read ("
                  + expectedErrnoText(EIO)
                  + "), so whether the program ever started is UNKNOWN; "
                    "reporting it as not spawned rather than guessing that it "
                    "ran.");
}

TEST(InterpretExecHandshake, AReadErrorBeatsAnApparentlyCompleteRecord) {
    // A count collected from a failed read describes nothing, so even a
    // full-looking one may not be interpreted as the child's own report.
    ChildFailure const poison = poisonedRecord();
    auto const         verdict =
        interpretExecHandshake(kStandInRecordBytes, kStandInRecordBytes, EIO,
                               &poison, inheritContext());
    EXPECT_FALSE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitInherit: the exec-status handshake with the child "
              "of '/opt/tools/gen' broke — 8 of 8 bytes were read ("
                  + expectedErrnoText(EIO)
                  + "), so whether the program ever started is UNKNOWN; "
                    "reporting it as not spawned rather than guessing that it "
                    "ran.");
}

TEST(InterpretExecHandshake, AMissingRecordNeverResolvesToSpawned) {
    // A null record with a complete byte count is a caller bug. The function is
    // total, and the safe direction is the one where the caller is told it does
    // not know — never the one where it is told the program ran.
    auto const verdict =
        interpretExecHandshake(kStandInRecordBytes, kStandInRecordBytes, 0,
                               nullptr, inheritContext());
    EXPECT_FALSE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitInherit: the exec-status handshake with the child "
              "of '/opt/tools/gen' broke — 8 of 8 bytes were read, so whether "
              "the program ever started is UNKNOWN; reporting it as not "
              "spawned rather than guessing that it ran.");
}

// The live path still routes through the same function ON THE HOSTS THAT HAVE A
// HANDSHAKE: a real spawn that succeeds reads EOF, which is the clean arm above,
// and without this the pure pins could all pass while `spawnAndWaitInherit` had
// stopped calling them. On macOS there is no pipe and no call, so there this
// asserts the weaker — but still true, and still worth holding — statement that
// a clean spawn reports `spawned` with an EMPTY diagnostic. Left ungated for the
// same reason as the pins above it: which host runs which arm is stated here in
// prose, not hidden in a preprocessor condition a reader has to reconstruct.
TEST(InterpretExecHandshake, TheLiveSpawnPathAgreesWithTheCleanArm) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "process-spawn"};
    auto const run = runFixture(scratch.path() / "handshake.marker", 0, {});
    EXPECT_TRUE(run.result.spawned) << run.result.diagnostic;
    EXPECT_EQ(run.result.diagnostic, "");
    ASSERT_TRUE(run.marker.has_value()) << run.markerError;
}

#endif  // !_WIN32

// ══ tests/test_support/run_binary.hpp — the SAME quoter, the OTHER spawner ══
//
// ★★ D-TEST-RUN-BINARY-ARGV-QUOTING-UNESCAPED, CLOSED AND PINNED.
//
// The test harness used to build its Windows command line with `"` + arg + `"`
// and a comment admitting it escaped neither embedded quotes nor trailing
// backslashes. That is not a cosmetic duplication: it means a harness that
// SILENTLY hands the child different bytes than the test asked for, so a test
// of the argv mechanism could pass while proving nothing. It now composes one
// `std::vector<std::string>` — `[launcherPrefix..., binary, programArgs...]` —
// and runs it through `buildWindowsCommandLine`, the same function every pin
// above exercises.
//
// THIS TEST IS THE END-TO-END HALF, and it is deliberately NOT `_WIN32`-gated.
// The oracle test above proves the STRING parses back correctly; this one
// proves the harness actually delivers those bytes to a live child, by spawning
// the self-fixture THROUGH `runBinary` and reading the child's own view of its
// argv back out of the marker. On Windows that is quoter → `CreateProcessW` →
// the CRT's parser; on POSIX it is the `posix_spawn` argv ARRAY, which has no
// command line to quote at all. Both arms owe the caller byte identity, and the
// assertion is the same sentence on both — which is the only way a harness bug
// on one platform cannot hide behind the other being fine.
//
// RED-ON-REVERT is exact: restore the old `" + a + "` concatenation and
// `he said "hi"`, `C:\Program Files\dir\` and `trailing\` all re-split or
// swallow their neighbour, so the argument COUNT changes and this goes red on
// its first assertion.
TEST(RunBinaryHarness, HostileArgumentsReachTheChildByteIdentically) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "run-binary-argv"};
    fs::path const markerPath = scratch.path() / "run-binary-argv.marker";

    // `runBinary` appends `programArgs` after the binary, i.e. they become the
    // child's argv[1..] — exactly the fixture protocol's shape.
    std::vector<std::string> programArgs{std::string{kFixtureFlag},
                                         markerPath.string(), "0"};
    auto const&              payload = hostileArgvPayload();
    programArgs.insert(programArgs.end(), payload.begin(), payload.end());

    auto const run = dss::test_support::runBinary(
        selfPath(), dss::test_support::kRunBudget, /*captureStdout=*/false,
        /*launcherPrefix=*/{}, programArgs);
    ASSERT_TRUE(run.spawned) << run.diagnostic;
    ASSERT_FALSE(run.timedOut) << run.diagnostic;
    EXPECT_EQ(run.exitCode, 0u) << run.diagnostic;

    auto const blob = readFileBinary(markerPath);
    ASSERT_TRUE(blob.has_value())
        << "marker '" << markerPath.string() << "' was not written";
    std::string markerError;
    auto const  marker = parseMarker(*blob, markerError);
    ASSERT_TRUE(marker.has_value()) << markerError;

    ASSERT_EQ(marker->args.size(), payload.size())
        << "the child received a DIFFERENT NUMBER of arguments — the harness's "
           "command line re-split, which is precisely what the unescaped quoter "
           "it used to carry did to an embedded quote or a trailing backslash";
    for (std::size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(marker->args[i], payload[i])
            << "argv[" << i << "] was altered in transit by runBinary: got ["
            << marker->args[i] << "] sent [" << payload[i] << "]";
    }
}

// ── main: spawn fixture OR test runner ─────────────────────────────────────
//
// Owning `main` (rather than linking gtest_main) is what lets the flag be
// consumed BEFORE InitGoogleTest, which would otherwise reject it. It is also
// why this file must stay its own ctest target.
int main(int argc, char** argv) {
    if (argc >= 4 && std::string_view{argv[1]} == kFixtureFlag) {
        std::string blob;
        std::error_code ec;
        fs::path const  cwd = fs::current_path(ec);
        if (ec) {
            std::fprintf(stderr, "fixture: current_path failed: %s\n",
                         ec.message().c_str());
            return 125;
        }
        appendRecord(blob, "ARGV0", std::string{argv[0]});
        appendRecord(blob, "CWD", cwd.string());
        for (int i = 4; i < argc; ++i) {
            appendRecord(blob, "ARG", std::string{argv[i]});
        }
        // Probed BEFORE anything is written to a stream, and reported through
        // the marker rather than through stdout or stderr — those two are what
        // the pin reading this record is judging, so an answer carried on them
        // would depend on the thing it is meant to measure.
        if (argc >= 5 && std::string_view{argv[4]} == kProbeStdioDirective) {
            appendRecord(blob, "STDIN", stdinIsLive() ? "live" : "dead");
        }
        // BINARY mode: a text-mode write would translate '\n' to CRLF on
        // Windows and corrupt both the length prefixes and any payload that
        // contains a newline.
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "fixture: cannot open marker '%s'\n", argv[2]);
            return 126;
        }
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        out.close();
        if (!out) {
            std::fprintf(stderr, "fixture: marker write failed\n");
            return 126;
        }

        // ── Directives that write to the STREAMS, after the marker is on disk
        // so the two facts stay independent (a marker still sitting in a stdio
        // buffer would make "the child reached user code" unprovable).
        // Unconditional on the platform, unlike the POSIX-only pair below:
        // `spawnAndWaitRedirectStdout` moves stdout on every host.
        if (argc >= 5 && std::string_view{argv[4]} == kProbeStdioDirective) {
            std::fwrite(kInheritedStdoutToken.data(), 1,
                        kInheritedStdoutToken.size(), stdout);
            std::fflush(stdout);
            std::fwrite(kInheritedStderrToken.data(), 1,
                        kInheritedStderrToken.size(), stderr);
            std::fflush(stderr);
        }

        // The flood. A SHORT write is reported with its own distinctive exit
        // code rather than being allowed to look like a successful run that
        // captured less — that failure would otherwise be indistinguishable
        // from the redirect losing bytes, which is precisely what the parent is
        // trying to measure.
        if (argc >= 6 && std::string_view{argv[4]} == kFloodStdoutDirective) {
            std::string const payload = floodPayload(static_cast<std::size_t>(
                std::strtoull(argv[5], nullptr, 10)));
            std::size_t const written =
                std::fwrite(payload.data(), 1, payload.size(), stdout);
            if (written != payload.size() || std::fflush(stdout) != 0) {
                std::fprintf(stderr,
                             "fixture: short write to stdout (%llu of %llu)\n",
                             static_cast<unsigned long long>(written),
                             static_cast<unsigned long long>(payload.size()));
                return 123;
            }
        }
#if !defined(_WIN32)
        // Write through the descriptors the parent handed us, only AFTER the
        // marker is on disk so the two facts stay independent. `fwrite` with an
        // explicit length rather than `fputs`: a `string_view` is not obliged to
        // be NUL-terminated, and the parent compares these bytes exactly.
        if (argc >= 5 && std::string_view{argv[4]} == kEchoStdioDirective) {
            std::fwrite(kInheritedStdoutToken.data(), 1,
                        kInheritedStdoutToken.size(), stdout);
            std::fflush(stdout);
            std::fwrite(kInheritedStderrToken.data(), 1,
                        kInheritedStderrToken.size(), stderr);
            std::fflush(stderr);
        }

        // Die by SIGNAL rather than by `return`, and do it only AFTER the
        // marker is closed on disk — the parent asserts both that the child
        // reached user code and how it ended, and a marker still sitting in a
        // stdio buffer would make the first half unprovable.
        if (argc >= 6 && std::string_view{argv[4]} == kRaiseSignalDirective) {
            std::raise(static_cast<int>(std::strtol(argv[5], nullptr, 10)));
            // Unreachable for an uncatchable signal. A distinctive code rather
            // than falling through to argv[3], so a signal that somehow did
            // NOT kill this process cannot be mistaken for the clean exit the
            // caller asked for.
            return 124;
        }
#endif
        return static_cast<int>(std::strtol(argv[3], nullptr, 10));
    }

    // Capture argv[0] before any test can change the working directory.
    std::error_code ec;
    selfPathStorage() = fs::absolute(fs::path{argv[0]}, ec);
    if (ec) {
        selfPathStorage() = fs::path{argv[0]};
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
