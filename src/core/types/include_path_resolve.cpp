#include "core/types/include_path_resolve.hpp"

namespace dss {

namespace fs = std::filesystem;

std::optional<fs::path> findInDirs(std::string_view                  filename,
                                   std::span<fs::path const>         dirs) {
    fs::path const rel{filename};
    std::error_code ec;
    if (rel.is_absolute()) {
        return fs::exists(rel, ec) ? std::optional<fs::path>{rel} : std::nullopt;
    }
    for (fs::path const& dir : dirs) {
        if (auto candidate = dir / rel; fs::exists(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

std::optional<fs::path> resolveIncludePath(
    std::string_view              filename,
    fs::path const&               includingDir,
    std::span<fs::path const>     includeDirs) {
    fs::path const rel{filename};
    std::error_code ec;
    if (rel.is_absolute()) {
        return fs::exists(rel, ec) ? std::optional<fs::path>{rel} : std::nullopt;
    }
    if (!includingDir.empty()) {
        if (auto candidate = includingDir / rel; fs::exists(candidate, ec))
            return candidate;
    }
    for (fs::path const& dir : includeDirs) {
        if (auto candidate = dir / rel; fs::exists(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

std::optional<fs::path> resolveSystemDescriptor(
    std::string_view              filename,
    std::span<fs::path const>     systemDirs) {
    // `<stem>.json`, PRESERVING any subdirectory so a POSIX `sys/*` header maps
    // to a distinct descriptor and never collides with a top-level header of the
    // same stem: `<sys/types.h>` -> `sys/types.json`, `<sys/time.h>` ->
    // `sys/time.json` (DISTINCT from `<time.h>` -> `time.json`). A flat header
    // keeps its flat name (`<stdio.h>` -> `stdio.json`). Agnostic of the requested
    // extension (`<stdio.h>`, `<stdio>` both -> `stdio.json`). This is the SINGLE
    // FC15c funnel every consumer shares (import_resolver typed-surface +
    // preprocessor macro inject + `__has_include`), so they stay in lock-step.
    fs::path const requested{filename};
    fs::path const relStem = requested.parent_path() / requested.stem();
    std::string const descriptorName = relStem.generic_string() + ".json";
    return findInDirs(descriptorName, systemDirs);
}

} // namespace dss
