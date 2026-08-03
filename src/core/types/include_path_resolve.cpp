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

AngleIncludeResolution resolveAngleInclude(
    std::string_view          filename,
    std::span<fs::path const> systemDirs,
    std::span<fs::path const> includeDirs) {
    // 1. Descriptor FIRST — the DSS neutral `<stem>.json` model. Existence of the
    //    descriptor FILE is the gate here; per-format availability is the caller's
    //    verdict (so an existing-but-unavailable descriptor still returns
    //    Descriptor and does NOT fall through to a source header).
    if (auto desc = resolveSystemDescriptor(filename, systemDirs)) {
        return {AngleIncludeKind::Descriptor, std::move(*desc)};
    }
    // 2. Source fallback — a REAL header on the -I includeDirs. The angle form does
    //    NOT search the including file's own directory (C 6.10.2p2), so this is
    //    `includeDirs` ONLY, never a self-dir prepend (that distinction is what the
    //    quote form's `resolveIncludePath` adds; angle omits it by construction).
    if (auto src = findInDirs(filename, includeDirs)) {
        return {AngleIncludeKind::Source, std::move(*src)};
    }
    // 3. Total miss — the caller fails loud (F_ShippedHeaderNotFound).
    return {AngleIncludeKind::NotFound, {}};
}

} // namespace dss
