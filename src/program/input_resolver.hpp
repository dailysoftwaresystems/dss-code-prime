#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Input resolution — plan 00 §4.1.3 + plan 14 D-LK10-1 closure (LK10
// cycle 3 — landed 2026-06-01).
//
// Hoisted out of `program.cpp::compileDirectory` once the CLI gained
// `--recursive` / `--no-recursive` flags — that's the second policy
// axis D-LK10-1 anchored on. The substrate is target-blind,
// source-blind, linker-blind: it walks a filesystem path against an
// extension allow-list and produces a sorted, deduplicated list of
// matching files. No language / target / format identity references.

namespace dss {

class DSS_EXPORT InputResolver {
public:
    enum class Mode : std::uint8_t {
        Recursive = 0,  // walks all subdirectories (the cycle 2 default)
        Flat      = 1,  // only the top-level directory (no subdir walk)
    };

    // Resolve a directory to a sorted, deduplicated list of source
    // files whose extension matches `fileExtensions`. `recursive`
    // controls subdirectory traversal. Fails loud on:
    //   * `D_FileNotFound`        — directory doesn't exist / not a dir
    //   * `D_DirectoryScanFailed` — mid-scan filesystem error
    //   * `D_EmptyInput`          — no files match the extensions
    //
    // The recursive vs flat split is the policy axis D-LK10-1 anchored
    // on; both modes use the same extension-filter + sort discipline.
    //
    // Returns the resolved paths via `out` (caller-owned vector;
    // the helper appends, doesn't clear). The bool return mirrors
    // the `<expected>`-shape contract: true = success, false = a
    // diagnostic was emitted into `reporter`.
    [[nodiscard]] static bool resolveDirectory(
        std::filesystem::path const&        directoryPath,
        std::span<std::string const>        fileExtensions,
        Mode                                mode,
        std::vector<std::string>&           out,
        DiagnosticReporter&                 reporter);

    // Validate that every path in `inputs` exists and is a regular
    // file. Appends each valid path to `out` (caller-owned;
    // append-semantics matching `resolveDirectory`); emits
    // `D_FileNotFound` (path absent / not a regular file) or
    // `D_DirectoryScanFailed` (filesystem-level I/O error,
    // permission-denied) per failing input; returns true iff every
    // input passed. (Today every caller already validates upstream
    // — this helper exists for the future case where the CLI's
    // `--compile <files>` flag wants pre-pipeline validation
    // diagnostics.)
    [[nodiscard]] static bool validateFiles(
        std::span<std::string const>        inputs,
        std::vector<std::string>&           out,
        DiagnosticReporter&                 reporter);

    // ── D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED, the ACCEPTANCE half ──
    //
    // ★★★ SAY IT WHERE THE DIRECTORY IS NAMED, NOT WHERE A HEADER GOES
    // MISSING. A search directory that cannot be enumerated contributes
    // nothing, and the only symptom the user ever saw was a missing-header
    // error naming the HEADER — pointing at the `#include` line, which is
    // correct, while the actual fault is in the `-I` argument several
    // hundred characters away on the command line. This names the DIRECTORY,
    // once, at the point the driver accepts it, before any source is read.
    //
    // ★★★ IT WARNS. IT MUST NOT REFUSE, AND THAT IS A MEASUREMENT, NOT A
    // PREFERENCE. The requirement as originally written said "fail LOUD",
    // and an ERROR would have put DSS ABOVE `(gcc ∪ clang ∪ MSVC) ∪ ISO C` —
    // the union binds in BOTH directions, so refusing a command line every
    // reference accepts is as much a defect as refusing a valid construct.
    // ✔MEASURED 2026-08-28 on this host:
    //   * gcc 13.2.0 (mingw-w64), `-I <absent>`: SILENT, rc=0 — and `-v`
    //     shows the directory is not even listed in the search list, so it
    //     is dropped, not searched. Still silent under `-Wall -Wextra
    //     -Werror`.
    //   * gcc 13.2.0, `-I <a regular file>`: rc=0 with
    //     `cc1.exe: warning: <path>: not a directory` — a WARNING naming the
    //     path, which is exactly this function's shape.
    //   * gcc 13.2.0, `-I //nosuchhost/share` (UNC, unreachable): SILENT,
    //     rc=0.
    //   * MSVC 14.44.35207, `/I` on all three of the above: SILENT, rc=0,
    //     including at `/W4 /WX`.
    // ⇒ NO reference refuses any of them, so DSS must not either. A warning
    // is strictly more informative than both references and refuses nothing;
    // a user who wants it fatal already has `--warnings-as-errors`, and one
    // drowning in it (a build system that passes `-I` speculatively is
    // ordinary) has `--suppress=<code>`.
    //
    // ⚠ THE ABSOLUTENESS OF THE PATH IS NEVER ASKED, AND DELIBERATELY SO.
    // ✔MEASURED by the lane root-causing this row's resolver half: on the
    // MinGW toolchain that builds DSS a UNC path answers `is_absolute()`
    // FALSE while `has_root_directory()` is TRUE, so an `is_absolute()`
    // test is wrong on exactly the paths this row is about. This function
    // asks only what it actually needs — can the directory be ENUMERATED —
    // which is a question the filesystem answers directly and identically
    // on every host.
    //
    // `optionSpelling` is the CLI/manifest spelling being validated (`-I`,
    // `includes`), echoed so the message points at the right input surface.
    // Returns true iff every directory is usable; the caller does NOT abort
    // on false — the return exists so a test can assert the classification
    // without reading message text.
    [[nodiscard]] static bool checkSearchDirectoriesUsable(
        std::span<std::string const>        directories,
        std::string_view                    optionSpelling,
        DiagnosticReporter&                 reporter);
};

} // namespace dss
