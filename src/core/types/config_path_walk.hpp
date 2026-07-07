#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"   // LoadResult + ConfigDiagnostic
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <string_view>

// Shared loader substrate for the `loadShipped(name)` lookup pattern.
// Both `GrammarSchema` (`src/dss-config/sources/<name>.lang.json`) and
// `TargetSchema` (`src/dss-config/targets/<name>.target.json`) need the
// same discovery: reject path-like names, then (1) honour an explicit
// `DSS_CONFIG_ROOT` env override, else (2) cwd-walk up to 8 levels
// looking for the config file. This helper consolidates the shape so a
// future third config kind (e.g., a passes manifest) drops into the same
// substrate.
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

} // namespace dss
