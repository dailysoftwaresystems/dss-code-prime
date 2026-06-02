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
};

} // namespace dss
