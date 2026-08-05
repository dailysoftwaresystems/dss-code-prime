#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"   // LoadResult + ConfigDiagnostic
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

// Shared loader substrate for the `loadShipped(name)` lookup pattern.
// Both `GrammarSchema` (`src/dss-config/sources/<name>.lang.json`) and
// `TargetSchema` (`src/dss-config/targets/<name>.target.json`) need the
// same discovery: reject path-like names, then (1) honour an explicit
// `DSS_CONFIG_ROOT` env override, else (2) cwd-walk up to 8 levels
// looking for the config file. This helper consolidates the shape so a
// future third config kind (e.g., a passes manifest) drops into the same
// substrate. `findShippedConfigDir` below is the DIRECTORY form of the
// same precedence, for the callers that need the containing directory
// rather than one named file.
//
// `DSS_CONFIG_ROOT` (optional): a directory that CONTAINS `src/dss-config/`
// (absolute recommended; a relative value resolves against cwd). When set,
// it is checked before the cwd-walk so config
// resolves independent of the launch cwd — the test harness sets it to the
// repo root (`dss_add_test`) so OUT-OF-TREE builds, whose ctest cwd has no
// `src/dss-config/` in its ancestry, still find shipped config. Unset (the
// production default) is exactly the historical cwd-walk; a set-but-miss
// falls through to the walk.

namespace dss {

struct DSS_EXPORT ShippedConfigLocator {
    std::string_view name;             // user-supplied (e.g. "x86_64")
    std::string_view subdir;           // "sources" / "targets"
    std::string_view suffix;           // ".lang.json" / ".target.json"
    std::string_view kindLabel;        // "language" / "target" — diag prose
    DiagnosticCode   invalidNameCode;  // C_Invalid{Language,Target,Format}Name per kind
};

// Locate a shipped config file by walking up from cwd. Returns the
// resolved absolute path on success, or a single-entry diagnostic
// vector on rejection (bad name) / not-found (no file in ancestry).
[[nodiscard]] DSS_EXPORT LoadResult<std::filesystem::path>
findShippedConfig(ShippedConfigLocator const& loc);

// Locate a shipped config DIRECTORY (`src/dss-config/<subdir>`) — the directory
// analogue of `findShippedConfig`, and the ONE place that precedence lives for
// the directory form.
//
// WHY IT EXISTS. Four sites needed "resolve something under src/dss-config/" and
// three of them hand-rolled the walk: the shipped-lib descriptor scan
// (`ffi/shipped_lib_descriptor.cpp`), the LSP language discovery
// (`lsp/schema_cache.cpp`), and the driver's system-include dirs
// (`program.cpp::applySystemDirs`). The copies DRIFTED on the one thing that
// matters — the driver's forgot to read `DSS_CONFIG_ROOT` at all, so the shipped
// CLI could not resolve `#include <stdio.h>` from any cwd outside its own source
// tree even with the documented override set (MEASURED: cwd in-tree → rc 0; cwd
// `C:\` → `error[F001A]: got stdio.h`, same binary, override set in both arms).
// Route every new directory lookup through here rather than opening a fifth walk.
//
// PRECEDENCE — deliberately identical to `findShippedConfig`'s:
//   1. `$DSS_CONFIG_ROOT/src/dss-config/<subdir>`, validated as a DIRECTORY
//   2. the cwd ancestor walk (up to 8 hops), same validation
//   3. `std::nullopt` — a set-but-miss override falls THROUGH to the walk, so a
//      stale value never worsens discovery.
// Validation is `is_directory` rather than the file form's `exists` because that
// IS the analogue: `exists` would accept a plain file named `shippedLibs` and
// hand a caller a "directory" it cannot iterate.
//
// `subdir` is CONFIG-SUPPLIED, not a user-supplied logical name — a literal
// ("sources", "shippedLibs") or a `.lang.json` value that may itself be nested
// (`semantics.shippedLibDirs`, e.g. "shippedLibs/windows-x86_64"). It therefore
// does NOT get the file form's path-like-name rejection, which exists to stop an
// untrusted `loadShipped` name escaping the config tree via `../`. Do not
// forward untrusted input here.
//
// `startPath`, when engaged, means "discover from exactly here": the env
// override is SKIPPED and the walk starts there instead of cwd. Only the LSP
// uses it, and it is load-bearing — the discovery fixtures point at a scratch
// dir, and honouring the ambient environment over an explicit caller argument
// would make them untestable.
//
// Returns `std::optional` rather than `LoadResult` on purpose: every caller
// already owns a distinct not-found behaviour (fall through to another tier /
// report "not located" / skip the dir so the miss fails loud downstream), and
// none of them would forward a diagnostic manufactured here.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
findShippedConfigDir(
    std::string_view                            subdir,
    std::optional<std::filesystem::path> const& startPath = std::nullopt);

} // namespace dss
