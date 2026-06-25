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
    // `<stem>.json` -- agnostic of the requested extension (`<stdio.h>`,
    // `<stdio>` both map to `stdio.json`). IDENTICAL to the import resolver's
    // angle mapping (import_resolver.cpp): the FC15c funnel that keeps
    // `__has_include(<h>)` in lock-step with `#include <h>`.
    std::string const descriptorName = fs::path(filename).stem().string() + ".json";
    return findInDirs(descriptorName, systemDirs);
}

} // namespace dss
