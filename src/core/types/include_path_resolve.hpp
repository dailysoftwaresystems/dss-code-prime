#pragma once

// Shared include-path resolution primitives (FC15c). These are the ONE
// authoritative implementations of the two filesystem searches the include
// machinery performs, so the import resolver (post-parse `#include`) and the
// preprocessor's `__has_include` operator (C 6.10.1 / C23 6.10.1p4) can never
// disagree on "does this header exist". The crux (the silent-miscompile the
// FC15c plan-lock caught): `__has_include` MUST give the SAME answer `#include`
// would, and the ANGLE form's answer is NOT a naive `findInDirs(filename)` --
// DSS ships LANGUAGE-NEUTRAL JSON descriptors (`stdio.json`, not `stdio.h`) on
// the system path, so an angle include maps `<stem>.json` before searching.
// That mapping lives HERE (one chokepoint), called by BOTH sites.

#include "core/export.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace dss {

// Search `dirs` for `filename` (a relative header name). First existing match
// wins. An absolute name resolves against the filesystem directly (the dir
// list is ignored). This is the QUOTE form's includeDirs search and the ANGLE
// form's systemDirs search -- the only difference between the two is WHICH dir
// list is passed and the self-dir prepend (quote-only), handled by the caller
// (see `resolveIncludePath`). Returns the resolved path, or nullopt on a miss.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
findInDirs(std::string_view filename, std::span<std::filesystem::path const> dirs);

// QUOTE-form (`#include "h"` / `__has_include("h")`) resolution: try the
// including file's own directory FIRST, then each of `includeDirs`. Mirrors C's
// quote-include search order (C 6.10.2p3). An absolute name resolves directly.
// `includingDir` may be empty (no self-dir prepend then). Returns the resolved
// path, or nullopt on a miss. This is the SHARED quote search used by both the
// import resolver and `__has_include`.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
resolveIncludePath(std::string_view filename,
                   std::filesystem::path const&                includingDir,
                   std::span<std::filesystem::path const>      includeDirs);

// ANGLE-form (`#include <h>` / `__has_include(<h>)`) resolution -- the
// FUNNEL the FC15c plan-lock mandates (one chokepoint, no drift). DSS ships a
// LANGUAGE-NEUTRAL JSON descriptor per system header (`<stdio.h>` ->
// `stdio.json`), NOT a `.h` source file, so the search is NOT `filename` on the
// path: it is `stem(filename) + ".json"` on `systemDirs` -- agnostic of the
// requested extension spelling (`<stdio.h>`, `<stdio>` both map to
// `stdio.json`). `#include <stdio.h>` and `__has_include(<stdio.h>)` BOTH call
// this so their existence answers always agree. Returns the resolved descriptor
// path, or nullopt on a miss.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
resolveSystemDescriptor(std::string_view                       filename,
                        std::span<std::filesystem::path const> systemDirs);

} // namespace dss
