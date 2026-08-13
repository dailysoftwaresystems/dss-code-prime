#include "core/substrate/process_spawn.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>

    // ── Which POSIX process-creation mechanism this host gets, and why ───────
    //
    // The `fork` arm below learns whether `execv` happened through a self-pipe,
    // and that pipe MUST be created with its close-on-exec flag already set: a
    // sibling thread that forks between a bare `pipe` and a follow-up
    // `fcntl(F_SETFD)` leaks the write end into an unrelated child, which then
    // holds the pipe open so the parent's blocking `read` never reaches EOF —
    // and this facility has NO TIMEOUT by design (`process_spawn.hpp`), so the
    // observable failure is the compiler HANGING. The threading premise is this
    // file's own: the async-signal-safety discipline further down exists because
    // of the per-CU compile pool.
    //
    //   * `pipe2(O_CLOEXEC)` sets the flag IN THE KERNEL, atomically with the
    //     descriptors' creation, so there is no window. Linux (since 2.6.27) and
    //     FreeBSD have it. The `_GNU_SOURCE` half of the predicate is not
    //     decoration: glibc hides the declaration behind `__USE_GNU`, and a host
    //     that somehow compiles without it must take a different arm or it will
    //     not compile at all.
    //   * macOS has no `pipe2` in any spelling, so it takes `posix_spawn`
    //     instead — which reports exec failure through its RETURN VALUE and
    //     therefore needs no self-pipe at all. That is strictly stronger than an
    //     atomic pipe: a descriptor that is never created cannot leak.
    //   * Any other POSIX host falls through to the two-step `pipe` + `fcntl`
    //     pair, which is the one arm where the window is genuinely open. No host
    //     in this project's matrix reaches it (Linux and FreeBSD take `pipe2`,
    //     macOS takes `posix_spawn`, Windows is a different function entirely);
    //     it is the last-resort arm for a port to a host offering neither
    //     mechanism, and such a port should wire up whichever one it does have
    //     rather than settle for it.
    #if (defined(__linux__) && defined(_GNU_SOURCE)) || defined(__FreeBSD__)
        #define DSS_SPAWN_HAVE_PIPE2 1
    #elif defined(__APPLE__)
        #define DSS_SPAWN_USE_POSIX_SPAWN 1
        #include <Availability.h>
        #include <crt_externs.h>  // _NSGetEnviron
        #include <spawn.h>

        // ★ THE chdir FILE ACTION HAS TWO SPELLINGS AND EACH IS A WARNING ON
        // THE OTHER'S HOST — this predicate picks the one that is neither
        // deprecated nor unavailable, rather than silencing whichever fires.
        // The action arrived as Apple's non-portable
        // `posix_spawn_file_actions_addchdir_np` in macOS 10.15 and was
        // DEPRECATED in macOS 26.0 in favour of the standard-spelled
        // `posix_spawn_file_actions_addchdir`, which is `__API_AVAILABLE(macos(
        // 26.0))` and therefore is not declared at all by an older SDK. Neither
        // name is right everywhere, so neither is hardcoded:
        //   * `__MAC_26_0` is only defined by an SDK that knows macOS 26 exists,
        //     which is exactly the condition under which the standard spelling
        //     is declared;
        //   * `__MAC_OS_X_VERSION_MIN_REQUIRED` is the DEPLOYMENT TARGET, and
        //     comparing against it is what keeps a new-SDK build aimed at an
        //     older macOS on the `_np` name — where that name is not yet
        //     deprecated, so nothing warns there either.
        // MEASURED on Darwin 25.5 / Apple clang 21: `__MAC_26_0` = 260000 and
        // `__MAC_OS_X_VERSION_MIN_REQUIRED` = 260000, i.e. this host takes the
        // standard spelling and compiles clean.
        #if defined(__MAC_26_0) && defined(__MAC_OS_X_VERSION_MIN_REQUIRED) \
            && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
            #define DSS_SPAWN_ADDCHDIR ::posix_spawn_file_actions_addchdir
        #else
            #define DSS_SPAWN_ADDCHDIR ::posix_spawn_file_actions_addchdir_np
        #endif
    #endif
#endif

// process_spawn — see process_spawn.hpp for the contract, the rationale, and
// the note on why a SECOND spawn implementation legitimately lives in
// tests/test_support/run_binary.hpp.
//
// The three entry points share this shape: validate the caller's request
// against reality FIRST (argv non-empty, argv[0] resolvable, cwd a real
// directory), so that by the time an OS call is made the only remaining
// failures are genuine OS failures — and every one of those carries the
// platform's own error text out through `SpawnResult::diagnostic`.

namespace dss::substrate {

namespace {

namespace fs = std::filesystem;

// ── Host error text ────────────────────────────────────────────────────────
// Both arms report the NUMERIC code as well as the message: the message is
// localized (and, on Windows, may be absent for some codes), so the number is
// what a bug report can be searched on.

#if defined(_WIN32)

std::string windowsErrorText(DWORD code) {
    std::string text;
    LPSTR       buffer = nullptr;
    DWORD const len    = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    if (len != 0 && buffer != nullptr) {
        text.assign(buffer, len);
        // FormatMessage terminates its text with CRLF; strip it so the
        // message composes into a single-line diagnostic.
        while (!text.empty()
               && (text.back() == '\r' || text.back() == '\n'
                   || text.back() == ' ')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    if (text.empty()) {
        text = "<no message text for this code>";
    }
    return "GetLastError=" + std::to_string(code) + ": " + text;
}

#else

std::string errnoText(int code) {
    // std::strerror is not thread-safe in the strictest reading, and the
    // thread-safe spellings are a portability minefield (GNU strerror_r
    // returns char*, XSI returns int, macOS differs again). This is a
    // failure-path-only call in a function that is already about to return an
    // error, so the practical exposure is nil and the portability win is
    // real. Numeric errno is included so the message is never the only clue.
    char const* const msg = std::strerror(code);
    return "errno=" + std::to_string(code) + ": "
         + (msg != nullptr ? msg : "<no message text for this errno>");
}

#endif

// ── PATH resolution ────────────────────────────────────────────────────────

#if defined(_WIN32)
constexpr char kPathListSeparator = ';';
#else
constexpr char kPathListSeparator = ':';
#endif

bool hasDirectoryComponent(std::string const& command) {
#if defined(_WIN32)
    // A drive-qualified name (`C:tool`) is also a path, not a PATH lookup.
    return command.find_first_of("/\\") != std::string::npos
        || (command.size() >= 2 && command[1] == ':');
#else
    return command.find('/') != std::string::npos;
#endif
}

// A candidate must be a real regular file — and on POSIX, one this user may
// actually exec.
//
// ★ "NOT EXECUTABLE" IS ITS OWN ANSWER, NOT A FLAVOUR OF "NOT FOUND". This
// predicate used to be a `bool` that folded the `access(X_OK)` refusal into
// "not a candidate", so an interpreter sitting at mode 644 — present, named
// correctly, exactly where the operator put it — produced "'x' was not found as
// an executable. It was looked up in PATH...". Loud, and pointing at the wrong
// thing: the operator goes and audits a PATH that was never the problem. The
// resolver still DECLINES such a file (a non-executable file is not a program,
// and continuing the scan is right — a later PATH entry may hold a real one),
// but the verdict now survives the walk so the caller can say it out loud.
// Directory / device / absent cases stay indistinguishable from each other:
// there is nothing useful to add about them.
enum class CandidateVerdict : std::uint8_t {
    NotAFile,
    NotExecutable,  // a regular file this user may not exec — POSIX only
    Executable,
};

CandidateVerdict classifyCandidate(fs::path const& candidate) {
    std::error_code ec;
    if (!fs::is_regular_file(candidate, ec) || ec) {
        return CandidateVerdict::NotAFile;
    }
#if !defined(_WIN32)
    if (::access(candidate.c_str(), X_OK) != 0) {
        return CandidateVerdict::NotExecutable;
    }
#endif
    return CandidateVerdict::Executable;
}

// Build an `fs::path` out of UTF-8 bytes. `nullopt` = the bytes are not UTF-8.
//
// ★ `fs::path{std::string}` IS NOT THIS FUNCTION ON WINDOWS, and the difference
// is a wrong answer rather than a stylistic one. The narrow `path` constructor
// converts through the host's "native narrow encoding", and the two toolchains
// this project ships DISAGREE about what that is: MS-STL runs the bytes through
// `MultiByteToWideChar` with the ACTIVE CODE PAGE, while libstdc++/MinGW treats
// them as UTF-8. `argv` elements are UTF-8 BY CONTRACT (see
// `windows_command_line.hpp`), so a manifest naming `tools/générateur.exe` was
// resolved against a mojibake path on the MSVC leg, missed, and was reported to
// the operator as "not found on PATH" — an encoding bug wearing a PATH bug's
// diagnostic, with the verdict depending on which compiler built the compiler.
//
// It is also what makes `spawnAndWaitInherit` obey the rule
// `windows_command_line.hpp` states over `decodeUtf8ToWide`: a `CreateProcessW`
// call site needs `lpApplicationName` and `lpCommandLine` produced by the SAME
// decoder. The resolved path returned from here becomes `lpApplicationName` via
// `path::wstring()` with no re-encoding in between, and the command line comes
// from `tryBuildWindowsCommandLine`, which is that same decode over those same
// bytes. Before this, the one call site the note exists for had two decoders.
//
// POSIX keeps the bytes untouched: a path there IS a byte string, `execv` takes
// bytes, and there is no re-encoding step available to get wrong.
std::optional<fs::path> pathFromUtf8(std::string const& utf8) {
#if defined(_WIN32)
    std::wstring wide;
    std::string  error;
    if (!decodeUtf8ToWide(utf8, wide, error)) {
        return std::nullopt;
    }
    return fs::path{wide};
#else
    return fs::path{utf8};
#endif
}

// A PATH entry, which is deliberately NOT held to the rule above.
//
// The UTF-8 contract covers `argv` — text the compiler received from a manifest
// it parsed. It does NOT cover the environment block: `std::getenv` hands back
// the CRT's NARROW view, which is ACP-encoded unless the process manifests
// UTF-8, so a directory with a non-ASCII name legitimately arrives here as
// bytes the strict decoder must reject. Decoding first keeps every ASCII and
// UTF-8 entry on the one decoder; falling back to the host conversion keeps a
// genuinely ACP-encoded entry SEARCHABLE, which is what every prior release did
// with it. Dropping it instead would trade a fixed `argv[0]` bug for a new PATH
// bug, and this function has no diagnostic channel to report the trade through.
fs::path pathFromEnvironmentEntry(std::string const& entry) {
#if defined(_WIN32)
    std::wstring wide;
    std::string  error;
    if (decodeUtf8ToWide(entry, wide, error)) {
        return fs::path{wide};
    }
    return fs::path{entry};
#else
    return fs::path{entry};
#endif
}

#if defined(_WIN32)
// PATHEXT, split. The default is the documented Windows fallback for a
// process whose environment lacks the variable (a stripped service
// environment, or a test that clears it).
std::vector<std::string> pathExtensions() {
    char const* const raw  = std::getenv("PATHEXT");
    std::string const spec = (raw != nullptr && *raw != '\0')
                                 ? std::string{raw}
                                 : std::string{".COM;.EXE;.BAT;.CMD"};
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (start <= spec.size()) {
        std::size_t const end = spec.find(';', start);
        std::string const piece =
            spec.substr(start, end == std::string::npos ? std::string::npos
                                                        : end - start);
        if (!piece.empty()) {
            out.push_back(piece);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}
#endif

// Probe one base candidate, applying Windows extension rules. Returns the
// path that actually exists, or nullopt.
//
// `firstNotExecutable`, when non-null and still empty, records the FIRST
// candidate that existed as a regular file but failed the executable probe.
// FIRST rather than last because a left-to-right search reports the entry it
// would have used had the bits been right; a later shadowing copy is strictly
// less interesting to the operator.
std::optional<fs::path> probeCandidate(fs::path const& candidate,
                                       std::string*    firstNotExecutable) {
    auto const consider = [&](fs::path const& probe) {
        switch (classifyCandidate(probe)) {
            case CandidateVerdict::Executable:
                return true;
            case CandidateVerdict::NotExecutable:
                if (firstNotExecutable != nullptr
                    && firstNotExecutable->empty()) {
                    *firstNotExecutable = probe.string();
                }
                return false;
            case CandidateVerdict::NotAFile:
                break;
        }
        return false;
    };
#if defined(_WIN32)
    // A name that already carries an extension is tried AS GIVEN first —
    // `git.exe` must not become `git.exe.COM`. (`has_extension` is false for
    // a bare `git`, which is the case PATHEXT exists for.)
    if (candidate.has_extension() && consider(candidate)) {
        return candidate;
    }
    for (auto const& ext : pathExtensions()) {
        fs::path withExt = candidate;
        // `+=` CONCATENATES; `replace_extension` would eat a legitimate dot
        // in the name (`python3.11` → `python3.EXE`).
        withExt += ext;
        if (consider(withExt)) {
            return withExt;
        }
    }
    return std::nullopt;
#else
    if (consider(candidate)) {
        return candidate;
    }
    return std::nullopt;
#endif
}

// Make a hit absolute so it stays valid after the child changes directory
// (`spawnAndWaitInherit(argv, cwd)` resolves argv[0] against the CALLER's cwd
// and must not silently re-root it against the child's).
fs::path makeAbsolute(fs::path const& hit) {
    std::error_code ec;
    fs::path const  abs = fs::absolute(hit, ec);
    // `absolute` is a pure lexical/cwd operation; a failure here means the
    // cwd itself is gone. The un-absolute hit is still a real file relative
    // to that same cwd, so returning it is strictly better than losing the
    // resolution — and the spawn below will report any residual failure with
    // the OS's own text.
    return ec ? hit : abs;
}

// The whole resolver, plus the one fact the public signature has no room for:
// `firstNotExecutable` (optional) comes back naming a candidate that WAS there
// and was not executable, so a caller that owns a diagnostic channel can say so.
// The public entry point below discards it. `optional<path>` is the right shape
// for the question "is this program here", and only `spawnAndWaitInherit` has
// anywhere to put the extra sentence — putting it in the public signature would
// make every caller carry a parameter for one caller's need.
std::optional<fs::path> resolveDetailed(std::string const& command,
                                        std::string*       firstNotExecutable) {
    if (command.empty()) {
        return std::nullopt;
    }

    // UTF-8 by contract on Windows; bytes on POSIX. A `nullopt` here means the
    // bytes are not UTF-8 at all, which `spawnAndWaitInherit` reports as itself
    // rather than as "not found" (this function has no channel to say it).
    auto const commandPath = pathFromUtf8(command);
    if (!commandPath.has_value()) {
        return std::nullopt;
    }

    // A path was given: no search happens, exactly as with execvp /
    // CreateProcess. Extension probing still applies on Windows so
    // `.\tools\git` finds `.\tools\git.exe`. The separator scan stays on the
    // RAW BYTES on purpose: '/', '\' and ':' are ASCII, and no ASCII byte can
    // occur inside a UTF-8 multi-byte sequence, so scanning the bytes gives the
    // same answer as scanning the decoded form and costs no decode.
    if (hasDirectoryComponent(command)) {
        auto const hit = probeCandidate(*commandPath, firstNotExecutable);
        return hit.has_value() ? std::optional<fs::path>{makeAbsolute(*hit)}
                               : std::nullopt;
    }

    char const* const rawPath = std::getenv("PATH");
    if (rawPath == nullptr) {
        return std::nullopt;
    }
    std::string const pathSpec{rawPath};

    std::size_t start = 0;
    while (start <= pathSpec.size()) {
        std::size_t const end = pathSpec.find(kPathListSeparator, start);
        std::string       entry =
            pathSpec.substr(start, end == std::string::npos
                                       ? std::string::npos
                                       : end - start);
#if defined(_WIN32)
        // Windows PATH entries may be quoted to protect a `;`-free path with
        // spaces; the quotes are not part of the directory name.
        if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
            entry = entry.substr(1, entry.size() - 2);
        }
#endif
        // An EMPTY entry is skipped on purpose. POSIX execvp reads it as "the
        // current directory"; searching the cwd for a tool named in a config
        // file is the injection hazard called out in the header, so this
        // facility never does it.
        if (!entry.empty()) {
            auto const hit = probeCandidate(
                pathFromEnvironmentEntry(entry) / *commandPath,
                firstNotExecutable);
            if (hit.has_value()) {
                return makeAbsolute(*hit);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

} // namespace

// ── Public surface ─────────────────────────────────────────────────────────

std::optional<fs::path> resolveExecutableOnPath(std::string const& command) {
    return resolveDetailed(command, /*firstNotExecutable=*/nullptr);
}

// `buildWindowsCommandLine` and its `try` / decode companions are defined
// inline in `core/substrate/windows_command_line.hpp` — header-only so the test
// tier can share the ONE quoting implementation without linking this library.

#if !defined(_WIN32)

namespace detail {

// See the header for WHY this is a separate function rather than four branches
// inside the read loop below. Order matters and is not arbitrary: a read ERROR
// disqualifies both clean outcomes before their byte counts are consulted,
// because a count collected from a failed read describes nothing.
//
// ★ COMPILED ON EVERY POSIX HOST, INCLUDING THE ONES WITH NO CALLER. macOS now
// spawns through `posix_spawn` and never reads an exec-status pipe, so on that
// host nothing in `src/` calls this. It stays compiled — and its pins stay
// ungated, which is the load-bearing half — because the alternative is a pin
// that silently does not run on a platform, and this project treats masked
// coverage as a defect in its own right. The function is pure: no descriptor, no
// syscall, no global state, so keeping it costs one symbol and buys the same
// branch coverage on the Apple leg that every other leg gets. Gating it would
// also have to gate seven pins identically, and the day a `posix_spawn` host
// needs the handshake back the tested decision would have to be rewritten from
// nothing.
HandshakeVerdict interpretExecHandshake(std::size_t         bytesRead,
                                        std::size_t         expectedBytes,
                                        int                 readErrno,
                                        ChildFailure const* failure,
                                        std::string const&  exePath,
                                        std::string const&  cwdPath) {
    if (readErrno == 0 && bytesRead == 0) {
        return HandshakeVerdict{true, std::string{}};
    }
    if (readErrno == 0 && bytesRead == expectedBytes && failure != nullptr) {
        return HandshakeVerdict{
            false,
            failure->stage == 'C'
                ? "spawnAndWaitInherit: the child could not enter working "
                  "directory '" + cwdPath + "' (" + errnoText(failure->error)
                      + ")"
                : "spawnAndWaitInherit: execv('" + exePath + "') failed ("
                      + errnoText(failure->error) + ")"};
    }
    return HandshakeVerdict{
        false,
        "spawnAndWaitInherit: the exec-status handshake with the child of '"
            + exePath + "' broke — " + std::to_string(bytesRead) + " of "
            + std::to_string(expectedBytes) + " bytes were read"
            + (readErrno != 0 ? " (" + errnoText(readErrno) + ")"
                              : std::string{})
            + ", so whether the program ever started is UNKNOWN; reporting it "
              "as not spawned rather than guessing that it ran."};
}

} // namespace detail

#endif  // !_WIN32

SpawnResult spawnAndWaitInherit(std::vector<std::string> const& argv,
                                fs::path const&                 cwd) {
    SpawnResult out;

    if (argv.empty()) {
        out.diagnostic =
            "spawnAndWaitInherit: empty argv — there is no program to run";
        return out;
    }

    // ★ AN EMBEDDED NUL IS A TOTAL-FUNCTION GUARD, NOT A PEDANTRY. `std::string`
    // carries a '\0' happily and a manifest can produce one (JSON permits an
    // escape for U+0000 and nlohmann stores it verbatim), but a process
    // argument is a C string on BOTH hosts and cannot: on POSIX the pointer
    // array below hands `execv` `element.data()`, a string that ENDS at the
    // NUL, so the child gets a TRUNCATED argument; on Windows the NUL survives
    // into the command buffer and TERMINATES `lpCommandLine`, so every
    // argument after it silently VANISHES. Either way
    // the child runs, may exit 0, and the build reports success having executed
    // something other than what the manifest said — which is precisely the
    // "byte-identically" guarantee this facility's header makes in writing.
    //
    // Checked HERE, at the substrate entry point, and not only in whatever
    // loader happens to feed it: this function is public, the manifest loader
    // is one of two consumers (dependency acquisition via `git` is the other,
    // and it interpolates a URL), and a guard that lives in one caller is not a
    // guard the function has. The offending element is named by INDEX and never
    // printed — a string containing a NUL is by definition unprintable, and
    // echoing it would truncate the diagnostic at the same byte.
    for (std::size_t index = 0; index < argv.size(); ++index) {
        if (argv[index].find('\0') != std::string::npos) {
            out.diagnostic = "spawnAndWaitInherit: argv[" + std::to_string(index)
                           + "] contains an embedded NUL byte, which no process "
                             "argument can carry — the child would silently "
                             "receive something other than what was asked for";
            return out;
        }
    }

#if defined(_WIN32)
    // argv[0] must decode BEFORE the PATH search, because the search is where a
    // bad decode turns into the wrong story. `resolveDetailed` builds its
    // candidate paths with `pathFromUtf8`, so non-UTF-8 bytes make it return
    // "no such program" — and the operator would then be told to fix a PATH
    // that is fine. Say what is actually wrong instead. (POSIX has no such
    // case: argv there is bytes, and a non-UTF-8 program name is legal.)
    {
        std::wstring probe;
        std::string  decodeError;
        if (!decodeUtf8ToWide(argv[0], probe, decodeError)) {
            out.diagnostic = "spawnAndWaitInherit: argv[0] is not valid UTF-8 ("
                           + decodeError
                           + "), so it cannot name a program on this host";
            return out;
        }
    }
#endif

    // Validate the working directory BEFORE spawning, so "the directory does
    // not exist" reports as itself rather than as an opaque CreateProcess
    // failure (Windows) or as a child that dies after fork (POSIX).
    if (!cwd.empty()) {
        std::error_code ec;
        if (!fs::is_directory(cwd, ec) || ec) {
            out.diagnostic = "spawnAndWaitInherit: working directory '"
                           + cwd.string() + "' is not a directory"
                           + (ec ? ": " + ec.message() : std::string{});
            return out;
        }
    }

    // Resolving here (rather than letting the OS search) is what makes
    // `spawned == false` mean what the header says it means: a missing tool
    // is detected before any process exists, and it is detected against the
    // CALLER's cwd even when the child will start somewhere else.
    //
    // ONE diagnostic, two causes. `notExecutable` comes back naming a candidate
    // that WAS there and could not be exec'd (POSIX permission bits); saying so
    // is the difference between "audit your PATH" and "chmod +x the file you
    // already have". It is a sentence inside the same message rather than a
    // second failure code, because the caller's remediation is still "fix
    // argv[0]" and `build_scripts.cpp` keys its two diagnostic codes off
    // `spawned`, not off why the resolution missed.
    std::string notExecutable;
    auto const  exe = resolveDetailed(argv[0], &notExecutable);
    if (!exe.has_value()) {
        out.diagnostic =
            "spawnAndWaitInherit: '" + argv[0]
            + "' was not found as an executable"
            + (notExecutable.empty()
                   ? std::string{". "}
                   : ": '" + notExecutable
                         + "' exists and is a regular file but is not "
                           "executable by this user (check its permission "
                           "bits). ")
            + "It was looked up in PATH"
              " (the current directory is deliberately never searched); pass"
              " a path containing a directory separator to run a local tool.";
        return out;
    }

#if defined(_WIN32)
    std::wstring cmdLine;
    std::string  decodeError;
    if (!tryBuildWindowsCommandLine(argv, cmdLine, decodeError)) {
        out.diagnostic = "spawnAndWaitInherit: argv is not valid UTF-8 and "
                         "cannot be encoded as a Windows command line ("
                       + decodeError + ")";
        return out;
    }

    // CreateProcessW WRITES THROUGH lpCommandLine, so it must be a mutable,
    // NUL-terminated buffer we own — never `cmdLine.data()` on a const path.
    std::vector<wchar_t> commandBuffer(cmdLine.begin(), cmdLine.end());
    commandBuffer.push_back(L'\0');

    std::wstring const exePath = exe->wstring();
    std::wstring const cwdPath = cwd.empty() ? std::wstring{} : cwd.wstring();

    // NO STARTF_USESTDHANDLES: the child inherits the parent's stdio exactly
    // as-is. `bInheritHandles=TRUE` is required for that to hold when the
    // parent's own stdout is a pipe or a file (which it is under ctest and
    // under any `dss-code-prime ... > log` invocation) — with FALSE, only a
    // console-attached parent would pass its output on, so the child's
    // diagnostics would vanish precisely when they are being captured.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // lpApplicationName is the RESOLVED image; lpCommandLine carries argv[0]
    // as the CALLER wrote it, matching execv's semantics on the POSIX arm
    // (the child sees the argv[0] it was given, not our resolution of it).
    BOOL const created = ::CreateProcessW(
        exePath.c_str(), commandBuffer.data(),
        /*lpProcessAttributes*/ nullptr,
        /*lpThreadAttributes*/  nullptr,
        /*bInheritHandles*/     TRUE,
        /*dwCreationFlags*/     0,
        /*lpEnvironment*/       nullptr,
        /*lpCurrentDirectory*/  cwd.empty() ? nullptr : cwdPath.c_str(),
        &si, &pi);
    if (created == 0) {
        // Includes the honest PATHEXT edge documented in the header: a
        // `.BAT`/`.CMD` hit resolves as a file but is not an executable
        // image, and lands here as ERROR_BAD_EXE_FORMAT with real text.
        out.diagnostic = "spawnAndWaitInherit: CreateProcessW failed for '"
                       + exe->string() + "' ("
                       + windowsErrorText(::GetLastError()) + ")";
        return out;
    }
    out.spawned = true;

    DWORD const waitResult = ::WaitForSingleObject(pi.hProcess, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        // The process WAS created (so `spawned` stays true — that is a fact
        // about the OS, not about our bookkeeping), but we cannot report an
        // exit code we never obtained. -1 plus a loud diagnostic beats 0,
        // which would read as a clean success.
        out.exitCode   = -1;
        out.diagnostic = "spawnAndWaitInherit: WaitForSingleObject did not "
                         "signal completion for '"
                       + exe->string() + "' ("
                       + windowsErrorText(::GetLastError()) + ")";
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return out;
    }

    DWORD exitCode = 0;
    if (::GetExitCodeProcess(pi.hProcess, &exitCode) == 0) {
        out.exitCode   = -1;
        out.diagnostic = "spawnAndWaitInherit: GetExitCodeProcess failed for '"
                       + exe->string() + "' ("
                       + windowsErrorText(::GetLastError()) + ")";
    } else {
        // Windows exit codes are DWORD; the struct exposes `int` so that the
        // POSIX 0..255 range and the Windows range share one field. The cast
        // is value-preserving for every code a program returns via `main`.
        out.exitCode = static_cast<int>(exitCode);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return out;
#else
    // Everything the child needs is built BEFORE the process is created. On the
    // `fork` arm that is a hard requirement rather than tidiness: between fork
    // and exec only async-signal-safe calls are legal in a process that may have
    // threads (and this one does — the per-CU compile pool), so no allocation,
    // no std::string construction and no locking may happen after the fork. The
    // `posix_spawn` arm has no such window — none of our code ever runs in the
    // child — but it consumes exactly these three values, so they are prepared
    // once for both rather than duplicated into each.
    std::string const        exePath = exe->string();
    std::string const        cwdPath = cwd.empty() ? std::string{} : cwd.string();
    std::vector<std::string> storage = argv;
    std::vector<char*>       argvPtrs;
    argvPtrs.reserve(storage.size() + 1);
    for (auto& element : storage) {
        argvPtrs.push_back(element.data());
    }
    argvPtrs.push_back(nullptr);

    // The process to wait on, whichever arm below created it. HOW a child comes
    // into being is a host-API question; how it ENDED is not — so `waitpid` and
    // the exit-status interpretation at the bottom of this function are SHARED
    // by both arms. Forking that logic would give one platform its own idea of
    // what "terminated by a signal" reports.
    pid_t childPid = -1;

#if defined(DSS_SPAWN_USE_POSIX_SPAWN)
    // ── macOS: posix_spawn, and therefore NO SELF-PIPE AT ALL ────────────────
    //
    // ★ THE EXEC STATUS COMES BACK AS A RETURN VALUE. `posix_spawn` returns
    // non-zero when the image could not be executed and zero once the child is
    // running the program — which is exactly the distinction
    // `SpawnResult::spawned` exists to make, obtained without creating a
    // descriptor. So the entire handshake apparatus the `fork` arm needs (a
    // pipe, a close-on-exec flag on it, a record written from the child, a read
    // loop, an interpretation of a possibly torn record) is simply absent here.
    //
    // THAT ABSENCE IS THE FIX, not an incidental simplification. The window this
    // arm exists to close is the one described at the top of this file: macOS
    // has no `pipe2`, so the pipe would have to be made close-on-exec in a
    // second step, and a sibling thread forking in between leaks the write end
    // into an unrelated child that then holds the pipe open forever. A pipe that
    // is never created cannot leak, so the race is ELIMINATED rather than
    // narrowed — a stronger outcome than the atomic `pipe2` the other hosts get.
    //
    // `posix_spawn`, not `posix_spawnp`: `resolveDetailed` above already turned
    // argv[0] into an absolute path against the CALLER's directory, and letting
    // the spawn search again would re-open the "which git did we run" question
    // this facility answers deliberately (and would resolve it against the
    // CHILD's directory once the chdir action below has run).
    //
    // No `POSIX_SPAWN_CLOEXEC_DEFAULT`: that attribute closes every inherited
    // descriptor INCLUDING 0/1/2, which would have to be re-inherited with three
    // explicit `adddup2` file actions to keep the promise in this function's
    // name. More moving parts to arrive back where the plain form already is —
    // there is no pipe here whose leak it could prevent.
    posix_spawn_file_actions_t fileActions;
    int spawnStatus = ::posix_spawn_file_actions_init(&fileActions);
    if (spawnStatus != 0) {
        out.diagnostic = "spawnAndWaitInherit: posix_spawn_file_actions_init() "
                         "failed (" + errnoText(spawnStatus) + ")";
        return out;
    }

    // An EMPTY cwd means "inherit the caller's directory", the same sentinel the
    // `fork` arm honours by skipping its `chdir` — so no action is installed at
    // all rather than one naming the current directory, which would turn a
    // relative-path caller into a subtly different question.
    if (!cwdPath.empty()) {
        // `DSS_SPAWN_ADDCHDIR` is the spelling this SDK and deployment target
        // want; see the predicate at the top of this file. The diagnostic names
        // the OPERATION rather than the symbol so it does not vary with that
        // choice — and this branch is an allocation failure inside the file-
        // actions object, so the symbol would tell an operator nothing anyway.
        spawnStatus = DSS_SPAWN_ADDCHDIR(&fileActions, cwdPath.c_str());
        if (spawnStatus != 0) {
            ::posix_spawn_file_actions_destroy(&fileActions);
            out.diagnostic = "spawnAndWaitInherit: could not add a chdir action "
                             "for working directory '"
                           + cwdPath + "' to the posix_spawn file actions ("
                           + errnoText(spawnStatus) + ")";
            return out;
        }
    }

    // `*_NSGetEnviron()` rather than a bare `extern char** environ`, and the
    // reason is a documented CONTRACT rather than an observed failure — which is
    // worth writing down, because the observation points the other way. Darwin's
    // `environ(7)` states that shared libraries and bundles have no direct
    // access to `environ` (it is the linker's, when a whole program is being
    // linked) and names `_NSGetEnviron()` from `<crt_externs.h>` as the way to
    // reach it at runtime. This translation unit IS compiled into a shared
    // library (`libdss-code-prime.dylib`). ⚠ MEASURED 2026-08-12 on Darwin 25.5
    // / Apple clang 21: a trivial `-dynamiclib` referencing `extern char**
    // environ` LINKS today, so the restriction is not enforced by this linker
    // and a future reader who tests the direct spelling will find it "works".
    // It is still not what the platform guarantees, and the documented API costs
    // one include. A null `envp` is not an option either: it would hand the
    // child an EMPTY environment, which is not what the `fork` arm's `execv`
    // does (it keeps ours).
    //
    // stdio is inherited because NOTHING is said about it: with no `adddup2`
    // action and no CLOEXEC-default attribute, descriptors 0/1/2 cross into the
    // child untouched, which is the same "the child writes to our stdout"
    // behaviour the `fork` arm gets by not touching them either.
    pid_t spawnedPid = -1;
    spawnStatus      = ::posix_spawn(&spawnedPid, exePath.c_str(), &fileActions,
                                     /*attrp=*/nullptr, argvPtrs.data(),
                                     *::_NSGetEnviron());
    ::posix_spawn_file_actions_destroy(&fileActions);

    if (spawnStatus != 0) {
        // ★ `posix_spawn` RETURNS the error number and does NOT set `errno`;
        // reporting `errno` here would print whatever unrelated call last
        // failed. This is the single most common way to get this call wrong.
        //
        // ⚠ AND IT CANNOT SAY WHICH STEP FAILED. The `fork` arm distinguishes
        // "could not enter the working directory" from "could not exec" because
        // the child reports its own stage byte; `posix_spawn` performs the chdir
        // action and the exec inside one call and surfaces ONE errno for the
        // pair, with overlapping values (ENOENT, EACCES and ENOTDIR are all
        // reachable from either). The honest report is therefore to name both
        // candidates and say the platform does not separate them — inventing a
        // stage by re-probing the directory from the parent would be reporting a
        // guess as if the child had told us. Where no directory was requested
        // there is no ambiguity to declare, and the message says exec plainly.
        out.diagnostic = "spawnAndWaitInherit: posix_spawn('" + exePath
                       + "') failed (" + errnoText(spawnStatus) + ")";
        if (!cwdPath.empty()) {
            out.diagnostic += " — the child was also asked to enter working "
                              "directory '" + cwdPath
                            + "' first, and posix_spawn reports ONE errno for "
                              "the whole operation, so the platform does not "
                              "say which of the two failed";
        }
        return out;
    }

    childPid    = spawnedPid;
    out.spawned = true;
#else
    // The classic exec-status self-pipe: CLOEXEC on both ends, so a
    // SUCCESSFUL exec closes the write end and the parent reads EOF (zero
    // bytes). Any byte that DOES arrive is the child telling us why it never
    // became the program. Without this, a failed exec is indistinguishable
    // from a program that ran and exited 127 — the exact confusion the
    // `spawned` flag exists to prevent.
    //
    // ★ THE CLOEXEC MUST BE ATOMIC WITH THE CREATION WHERE THE HOST ALLOWS IT.
    // `pipe` followed by a separate `fcntl(F_SETFD, FD_CLOEXEC)` leaves a window
    // in which BOTH descriptors are inheritable, and the process is threaded —
    // that is the same premise the async-signal-safety note directly above is
    // written for (the per-CU compile pool). A sibling thread that forks and
    // execs inside that window leaks the WRITE end into an unrelated child; that
    // child then holds the pipe open, so the parent's blocking `read` below
    // never sees EOF, and this facility has NO TIMEOUT by design
    // (`process_spawn.hpp`). The observable failure is the compiler hanging
    // until some unrelated process happens to exit. `pipe2` closes the window
    // inside the kernel; there is nothing to race.
    //
    // ✅ D-SPAWN-CLOEXEC-RACE-OPEN-ON-MACOS — CLOSED. macOS used to reach the
    // two-step `#else` below because it offers no `pipe2`, and the window was
    // genuinely open there. It no longer reaches this code at all: it takes the
    // `posix_spawn` arm above, which has no pipe to make close-on-exec, so the
    // race is eliminated on every host this project builds for rather than
    // merely closed on the two that have the atomic call. What remains under
    // this `#else` is the last-resort arm for a POSIX port with neither
    // mechanism; the note at the top of this file says so and says what such a
    // port should do instead.
    int errorPipe[2] = {-1, -1};
#if defined(DSS_SPAWN_HAVE_PIPE2)
    if (::pipe2(errorPipe, O_CLOEXEC) != 0) {
        out.diagnostic = "spawnAndWaitInherit: pipe2(O_CLOEXEC) failed ("
                       + errnoText(errno) + ")";
        return out;
    }
#else
    // The only arm where the window above is genuinely open, and no host in the
    // matrix takes it. It is kept so a port to an unforeseen POSIX host compiles
    // and runs rather than failing to build on a line it cannot see, but such a
    // host should be given whichever atomic mechanism it does have instead.
    if (::pipe(errorPipe) != 0) {
        out.diagnostic = "spawnAndWaitInherit: pipe() failed ("
                       + errnoText(errno) + ")";
        return out;
    }
    for (int fd : {errorPipe[0], errorPipe[1]}) {
        int const flags = ::fcntl(fd, F_GETFD);
        if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
            int const saved = errno;
            ::close(errorPipe[0]);
            ::close(errorPipe[1]);
            out.diagnostic = "spawnAndWaitInherit: fcntl(FD_CLOEXEC) failed ("
                           + errnoText(saved) + ")";
            return out;
        }
    }
#endif

    // Declared in the header alongside `interpretExecHandshake`, which is the
    // only thing that reads one.
    using detail::ChildFailure;

    pid_t const pid = ::fork();
    if (pid < 0) {
        int const saved = errno;
        ::close(errorPipe[0]);
        ::close(errorPipe[1]);
        out.diagnostic =
            "spawnAndWaitInherit: fork() failed (" + errnoText(saved) + ")";
        return out;
    }

    if (pid == 0) {
        // ── CHILD. Async-signal-safe calls ONLY from here to _exit. ──
        ::close(errorPipe[0]);
        ChildFailure failure{'X', 0};
        if (!cwdPath.empty() && ::chdir(cwdPath.c_str()) != 0) {
            failure.stage = 'C';
            failure.error = errno;
        } else {
            // No file actions at all: stdin/stdout/stderr are simply the
            // parent's, which IS the inheritance the header promises. On
            // success this never returns.
            ::execv(exePath.c_str(), argvPtrs.data());
            failure.stage = 'X';
            failure.error = errno;
        }
        // Best-effort report, and the two ways it can fall short are NOT
        // equally recoverable — which is why the write is a single sub-PIPE_BUF
        // record (atomic) rather than a loop over fields that could tear. A
        // SHORT write is now caught: the parent sees a partial record and
        // reports a broken handshake rather than "the program ran". A write
        // that delivers NOTHING at all is indistinguishable from a successful
        // exec (both give the parent EOF), so the `_exit(127)` below is the
        // only remaining signal and the caller will read it as an exit status.
        // That residue is inherent to a self-pipe and is why the record is kept
        // small enough for the kernel to deliver in one piece.
        auto const* raw       = reinterpret_cast<char const*>(&failure);
        std::size_t remaining = sizeof(failure);
        while (remaining > 0) {
            ssize_t const written = ::write(errorPipe[1], raw, remaining);
            if (written <= 0) {
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            raw += written;
            remaining -= static_cast<std::size_t>(written);
        }
        ::_exit(127);
    }

    // ── PARENT ──
    ::close(errorPipe[1]);
    ChildFailure failure{'\0', 0};
    std::size_t  filled         = 0;
    int          handshakeErrno = 0;  // non-zero once `read` failed for real
    while (filled < sizeof(failure)) {
        ssize_t const got = ::read(errorPipe[0],
                                   reinterpret_cast<char*>(&failure) + filled,
                                   sizeof(failure) - filled);
        if (got == 0) {
            break;  // EOF: exec succeeded (CLOEXEC closed the write end)
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            handshakeErrno = errno;
            break;
        }
        filled += static_cast<std::size_t>(got);
    }
    ::close(errorPipe[0]);

    // The ENTIRE interpretation happens in one call, so that the entire
    // interpretation is one thing a test can drive. `&failure` is passed
    // unconditionally on purpose: gating it here would move a piece of the
    // decision back out into unpinnable code, and the function's contract is
    // that it reads the record only when the record is whole.
    detail::HandshakeVerdict verdict = detail::interpretExecHandshake(
        filled, sizeof(failure), handshakeErrno, &failure, exePath, cwdPath);

    if (!verdict.spawned) {
        // Reap the corpse so it is not left a zombie. Shared by both
        // not-spawned arms: whether the child reported its own exec failure or
        // told us nothing intelligible, it is a process that must be collected.
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        out.diagnostic = std::move(verdict.diagnostic);
        return out;
    }

    childPid    = pid;
    out.spawned = true;
#endif  // DSS_SPAWN_USE_POSIX_SPAWN

    int status = 0;
    while (::waitpid(childPid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        int const saved = errno;
        out.exitCode    = -1;
        out.diagnostic  = "spawnAndWaitInherit: waitpid() failed for '"
                       + exePath + "' (" + errnoText(saved) + ")";
        return out;
    }

    if (WIFEXITED(status)) {
        out.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        // 128 + signo is the shell convention, and reporting it alone would
        // be indistinguishable from a program that returned that number — so
        // the diagnostic names the signal. A build step that SEGVs must never
        // read as "exited 139 on purpose".
        int const signo = WTERMSIG(status);
        out.exitCode    = 128 + signo;
        out.diagnostic  = "spawnAndWaitInherit: '" + exePath
                       + "' was terminated by signal " + std::to_string(signo)
                       + " (reported as exit code " + std::to_string(128 + signo)
                       + ")";
    } else {
        // Neither exited nor signalled with WUNTRACED/WCONTINUED unset should
        // be unreachable; say so rather than silently reporting 0.
        out.exitCode   = -1;
        out.diagnostic = "spawnAndWaitInherit: '" + exePath
                       + "' produced an unrecognised wait status "
                       + std::to_string(status);
    }
    return out;
#endif
}

} // namespace dss::substrate
