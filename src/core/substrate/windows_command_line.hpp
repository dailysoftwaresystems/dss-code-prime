#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// windows_command_line — the MSVCRT / `CommandLineToArgvW` argv quoter, and the
// strict UTF-8 widening that goes with it. THE ONE implementation of that
// algorithm in this repository.
//
// WHY IT IS ITS OWN HEADER, SPLIT OUT OF `process_spawn.hpp`. The quoter is a
// PURE FUNCTION over `std::vector<std::string>` — no OS call, no library state,
// nothing to link. Its second consumer is `tests/test_support/run_binary.hpp`,
// which is included by THREE targets with three different include-path sets,
// and one of them (`integrated_tests`, via `integrated_tests/runner.cpp`) links
// NO DSS library at all and does not carry `src/` on its include path, because
// it drives the compiler as a black-box subprocess. A call from there into
// `dss-code-prime-lib` would break both its compile and its link, and widening
// that target's link graph to borrow a string helper is the wrong trade — the
// black-box boundary is the point of the target.
//
// So this header is HEADER-ONLY and DEPENDENCY-FREE, exactly like
// `src/program/platform_token.hpp` and for exactly the same reason. In
// particular it deliberately does NOT include `core/export.hpp`: everything
// below is `inline`, so `DSS_EXPORT` would be meaningless at best (an inline
// function needs no export) and an outright compile error at worst
// (`__declspec(dllimport)` on a function that is also defined here). KEEP IT
// THAT WAY — one added project include here breaks a consumer that never links
// the lib, and the duplicate quoter comes back.
//
// `run_binary.hpp` includes this by RELATIVE PATH
// (`#include "../../src/core/substrate/windows_command_line.hpp"`) for the same
// reason `arm_verdict_ledger.hpp` includes `platform_token.hpp` that way: a
// quoted include resolves against the includer's own directory first, so the
// one spelling works for every consumer with no build-graph change.
//
// PORTABLE ON PURPOSE — there is no Windows API call in this file, so it
// compiles and is unit-tested on every host in the matrix, not only where its
// output is used. (`tests/core/substrate/test_process_spawn.cpp` runs the
// exact-string pins everywhere and the `CommandLineToArgvW` round-trip ORACLE
// on Windows.)
//
// AGNOSTIC: names no source language, no target processor and no object format.
// "Windows" here is the HOST whose process API is being fed, never a target.

namespace dss::substrate {

namespace detail {

// Quote `argv` into a single command line, as NARROW bytes. Split out from the
// wide entry points because the algorithm is byte-oriented and the UTF-8 →
// `wchar_t` widening is a separate concern with a separate failure mode.
//
// BYTE-ORIENTED IS CORRECT, NOT A SHORTCUT: every character the algorithm
// reacts to (space, tab, newline, vertical tab, `"`, `\`) is ASCII, and no
// ASCII byte can occur inside a UTF-8 multi-byte sequence. So running the
// transform over UTF-8 bytes gives exactly the same answer as running it over
// decoded code points, without needing to decode first.
[[nodiscard]] inline std::string
quoteArgvNarrow(std::vector<std::string> const& argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        std::string const& arg = argv[i];

        // An argument with no delimiter and no quote in it is passed
        // VERBATIM. This is what keeps `a\` (a trailing backslash that the
        // parser never sees next to a quote, hence literal) distinct from
        // `"C:\Program Files\dir\\"` (a trailing backslash that DOES abut the
        // closing quote and therefore must be doubled). Quoting everything
        // unconditionally would still be correct, but it would collapse those
        // two cases into one and hide a whole class of regression.
        bool const needsQuotes =
            arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string::npos;
        if (!needsQuotes) {
            out += arg;
            continue;
        }

        out.push_back('"');
        for (std::size_t j = 0;; ++j) {
            // Consume a maximal run of backslashes; how it is emitted depends
            // entirely on what FOLLOWS it.
            std::size_t backslashes = 0;
            while (j < arg.size() && arg[j] == '\\') {
                ++j;
                ++backslashes;
            }
            if (j == arg.size()) {
                // The run abuts the closing quote we are about to write, so
                // every backslash must be doubled — otherwise the last one
                // escapes that quote and the argument swallows the next one.
                // THIS is the `C:\dir\` defect.
                out.append(backslashes * 2, '\\');
                break;
            }
            if (arg[j] == '"') {
                // 2n+1 backslashes then `"` parses back to n backslashes and
                // a LITERAL quote.
                out.append(backslashes * 2 + 1, '\\');
                out.push_back('"');
            } else {
                // Backslashes not followed by a quote are literal as written.
                out.append(backslashes, '\\');
                out.push_back(arg[j]);
            }
        }
        out.push_back('"');
    }
    return out;
}

} // namespace detail

// Strict UTF-8 → `wchar_t` decode. Returns false and fills `error` on any
// malformed input; NEVER substitutes a replacement character, because a
// transliterated command line is a WRONG command line that would then be
// EXECUTED. Encodes to UTF-16 (surrogate pairs) where `wchar_t` is 16 bits and
// to UTF-32 where it is 32 — i.e. the host's native wide encoding.
//
// PUBLIC because a `CreateProcessW` call site needs TWO wide strings from the
// same UTF-8 vocabulary — `lpCommandLine` and `lpApplicationName` — and they
// must be produced by the SAME decoder. Exposing this is what keeps the second
// one from growing a second (and differently wrong) widening.
[[nodiscard]] inline bool decodeUtf8ToWide(std::string const& in,
                                           std::wstring& out,
                                           std::string&  error) {
    constexpr bool kWideIsUtf32 = sizeof(wchar_t) >= 4;

    out.clear();
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        auto const    lead   = static_cast<unsigned char>(in[i]);
        std::size_t   extra  = 0;
        std::uint32_t cp     = 0;
        std::uint32_t lowest = 0;  // smallest value legally encodable
        if (lead < 0x80u) {
            cp    = lead;
            extra = 0;
        } else if (lead >= 0xC2u && lead <= 0xDFu) {
            cp     = lead & 0x1Fu;
            extra  = 1;
            lowest = 0x80u;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            cp     = lead & 0x0Fu;
            extra  = 2;
            lowest = 0x800u;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            cp     = lead & 0x07u;
            extra  = 3;
            lowest = 0x10000u;
        } else {
            // 0x80..0xBF (a stray continuation) and 0xC0/0xC1/0xF5..0xFF
            // (overlong or out-of-range leads) are never valid leads.
            // ★ FORMATTED AS ACTUAL HEX. The obvious spelling —
            // `"0x" + std::to_string(lead)` — renders byte 0x80 as "0x128":
            // a DECIMAL value wearing a hex prefix, and not even a possible
            // byte value, which sends the reader hunting for a corruption
            // that is not there. Done by hand rather than through
            // <iomanip>/<sstream> because this header is deliberately
            // dependency-free — `run_binary.hpp` includes it to share the
            // quoter without taking a link edge, and that is the property
            // that let the quoting fix reach both consumers at once.
            constexpr char kHexDigits[] = "0123456789ABCDEF";
            char const     hex[3]       = {kHexDigits[(lead >> 4) & 0xFu],
                                           kHexDigits[lead & 0xFu],
                                           '\0'};
            error = "invalid UTF-8 lead byte 0x" + std::string(hex)
                  + " at offset " + std::to_string(i);
            return false;
        }
        // The sequence occupies bytes [i, i + extra]; anything shorter is a
        // truncated tail, not a decodable code point.
        if (i + extra >= in.size()) {
            error = "truncated UTF-8 sequence at offset " + std::to_string(i);
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            auto const cont = static_cast<unsigned char>(in[i + k]);
            if ((cont & 0xC0u) != 0x80u) {
                error = "invalid UTF-8 continuation byte at offset "
                      + std::to_string(i + k);
                return false;
            }
            cp = (cp << 6) | (cont & 0x3Fu);
        }
        if (extra != 0 && cp < lowest) {
            error = "overlong UTF-8 encoding at offset " + std::to_string(i);
            return false;
        }
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            error = "UTF-8 sequence at offset " + std::to_string(i)
                  + " decodes to a value that is not a Unicode scalar";
            return false;
        }

        if (kWideIsUtf32 || cp <= 0xFFFFu) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            std::uint32_t const v = cp - 0x10000u;
            out.push_back(static_cast<wchar_t>(0xD800u + (v >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00u + (v & 0x3FFu)));
        }
        i += extra + 1;
    }
    return true;
}

// Join `argv` into a single Windows command line using the MSVCRT /
// `CommandLineToArgvW` quoting algorithm, i.e. the exact inverse of the
// parser every Windows program's `argv` comes out of.
//
// WHY THIS IS NOT "wrap each argument in quotes". The naive form —
// `"` + arg + `"` — mis-parses two cases that are NORMAL, not exotic:
//
//   * an argument containing a `"` (a URL or message with a quote in it):
//     the embedded quote CLOSES the argument and everything after it
//     re-splits into new arguments;
//   * an argument ENDING in a backslash, which on Windows is what a
//     directory argument looks like: `"C:\dir\"` — the trailing backslash
//     escapes the closing quote, so the child sees `C:\dir" nextarg` as ONE
//     argument and the next argument vanishes. `git`'s consumer passes
//     Windows directory paths, so this is the common case, not the corner.
//
// THE ALGORITHM (as parsed by `CommandLineToArgvW` and by the CRT that
// produces `main`'s `argv`): backslashes are LITERAL except when they precede
// a `"`, where each pair collapses to one backslash and an odd one escapes
// the quote. So: `2n` backslashes + `"` = n backslashes and an argument
// delimiter; `2n+1` backslashes + `"` = n backslashes and a LITERAL quote.
// Emitting therefore means doubling a backslash run only when it is followed
// by a `"` — including the closing `"` this function adds.
//
// An argument is left UNQUOTED when it is non-empty and contains none of
// space, tab, newline, vertical tab or `"`. That is what makes `a\` and
// `"C:\Program Files\dir\\"` two genuinely different cases rather than one.
// An EMPTY argument becomes `""`, which is the only way to pass one.
//
// ★ argv[0] IS SPECIAL AND THIS FUNCTION DOES NOT PRETEND OTHERWISE. The
// Windows parser applies a DIFFERENT rule to the first token: quotes toggle,
// but backslashes are never escapes. A program path never contains a `"` and
// never ends in `\`, so the general algorithm's output for argv[0] is parsed
// identically by both rules; an argv[0] that did contain those characters
// could not be round-tripped by ANY quoting and is not a real input.
//
// `argv` elements are UTF-8 BY CONTRACT. THIS is the reporting form: it
// returns false and fills `error` when they are not, so a caller with a
// diagnostic channel (`spawnAndWaitInherit`'s `SpawnResult::diagnostic`,
// `run_binary.hpp`'s `RunResult::diagnostic`) can report instead of dying.
[[nodiscard]] inline bool
tryBuildWindowsCommandLine(std::vector<std::string> const& argv,
                           std::wstring& commandLine, std::string& error) {
    return decodeUtf8ToWide(detail::quoteArgvNarrow(argv), commandLine, error);
}

// The same transform with NO error channel — see `tryBuildWindowsCommandLine`
// above for the algorithm; this is the spelling the shipped call sites and the
// unit tests use.
[[nodiscard]] inline std::wstring
buildWindowsCommandLine(std::vector<std::string> const& argv) {
    std::wstring wide;
    std::string  error;
    if (!tryBuildWindowsCommandLine(argv, wide, error)) {
        // FAIL LOUD. This entry point has no error channel, and the two
        // alternatives are both worse than aborting: returning a
        // U+FFFD-substituted command line silently executes something other
        // than what the caller asked for, and returning an empty string turns
        // a bad argument into a mysterious spawn failure elsewhere. Callers
        // that need a reportable error use the `try...` form above.
        std::fprintf(stderr,
                     "dss::substrate::buildWindowsCommandLine fatal: argv is "
                     "not valid UTF-8 (%s)\n",
                     error.c_str());
        std::abort();
    }
    return wide;
}

} // namespace dss::substrate
