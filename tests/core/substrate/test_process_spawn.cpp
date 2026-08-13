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

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
using dss::substrate::SpawnResult;
using dss::substrate::tryBuildWindowsCommandLine;

#if !defined(_WIN32)
using dss::substrate::detail::ChildFailure;
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
constexpr std::string_view kInheritedStdoutToken = "dss-fixture-wrote-stdout";
constexpr std::string_view kInheritedStderrToken = "dss-fixture-wrote-stderr";
#endif

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

#if !defined(_WIN32)

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
                               "/opt/tools/gen", "/work");
    EXPECT_TRUE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic, "")
        << "a successful spawn must carry no diagnostic — `build_scripts.cpp` "
           "uses that emptiness to tell a clean exit from a stand-in status";
}

TEST(InterpretExecHandshake, AWholeRecordFromExecvIsNotSpawnedAndNamesTheImage) {
    ChildFailure const failure{'X', ENOENT};
    auto const         verdict =
        interpretExecHandshake(kStandInRecordBytes, kStandInRecordBytes, 0,
                               &failure, "/opt/tools/gen", "/work");
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
                               &failure, "/opt/tools/gen", "/work");
    EXPECT_FALSE(verdict.spawned);
    EXPECT_EQ(verdict.diagnostic,
              "spawnAndWaitInherit: the child could not enter working "
              "directory '/work' (" + expectedErrnoText(ENOTDIR) + ")");
}

TEST(InterpretExecHandshake, APartialRecordIsNotSpawnedAndSaysTheHandshakeBroke) {
    // ★ THE DEFECT, DIRECTLY. Three bytes of eight: not EOF, not a record.
    // The old code called this "the child ran".
    ChildFailure const poison = poisonedRecord();
    auto const         verdict =
        interpretExecHandshake(3, kStandInRecordBytes, 0, &poison,
                               "/opt/tools/gen", "/work");
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
                                                nullptr, "/opt/tools/gen",
                                                "/work");
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
                               &poison, "/opt/tools/gen", "/work");
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
                               nullptr, "/opt/tools/gen", "/work");
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
