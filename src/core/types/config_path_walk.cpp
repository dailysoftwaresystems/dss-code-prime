#include "core/types/config_path_walk.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace dss {

LoadResult<std::filesystem::path>
findShippedConfig(ShippedConfigLocator const& loc) {
    // Reject path-like names up front. `loadShipped` is the LOGICAL-
    // name resolver — only `csharp` / `x86_64` / `toy` / ... — never
    // arbitrary paths. Defending against `../` traversal here also
    // covers callers that forward an untrusted name (LSP requests,
    // future driver flags).
    if (loc.name.empty()
        || loc.name.find('/')  != std::string_view::npos
        || loc.name.find('\\') != std::string_view::npos
        || loc.name.front() == '.') {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {loc.invalidNameCode, DiagnosticSeverity::Error,
             std::string{loc.name},
             std::string{"invalid shipped-"} + std::string{loc.kindLabel} + " name"}});
    }

    namespace fs = std::filesystem;
    const std::string leaf = std::string{loc.name} + std::string{loc.suffix};
    std::error_code ec;

    // Explicit override: `DSS_CONFIG_ROOT` names a directory that CONTAINS
    // `src/dss-config/` (typically the repo root). It is consulted BEFORE the
    // cwd-walk so shipped config resolves regardless of where the process is
    // launched. The test harness sets it per-test (`dss_add_test` ENVIRONMENT
    // = repo root) so an OUT-OF-TREE build's ctest — whose cwd is a build
    // subdirectory with no `src/dss-config/` anywhere in its ancestry — still
    // finds config instead of nulling the loader, WHEN the lookup comes through
    // THIS function.
    // ⚠ THAT LAST CLAUSE IS NOT PEDANTRY — the comment used to omit it and was
    // therefore false in the way that mattered. Setting the variable never made
    // an out-of-tree ctest pass, because seventeen helpers under `tests/` had
    // their own private cwd-walks that never read it, and `lsp/schema_cache.cpp`
    // had one too. MEASURED at a3af1320: out-of-tree ctest failed 29/787 with
    // `DSS_CONFIG_ROOT` exported process-wide — an IDENTICAL failure set to not
    // exporting it. The test side now funnels through ONE resolver
    // (`tests/test_support/repo_root.hpp`, same env-first precedence as here);
    // if you add a new config lookup, route it through that or through this
    // function rather than opening a fresh walk. Unset (the production
    // default) → behaviour is EXACTLY the cwd-walk below, unchanged. A
    // set-but-miss falls THROUGH to the walk (a stale override never worsens
    // discovery). The path-like-name rejection above still gates `loc.name`,
    // so the override is not a `../` traversal vector. A relative value is
    // resolved against cwd (absolute recommended). This is a `std::getenv`
    // READ only — the compiler never writes the environment, so the lookup is
    // race-free; preserve that no-env-writes-during-compilation invariant if
    // CU-parallel compilation ([[D-PERF-4-CU-PARALLELISM]]) ever lands.
    if (const char* envRoot = std::getenv("DSS_CONFIG_ROOT");
        envRoot != nullptr && envRoot[0] != '\0') {
        const fs::path candidate = fs::path{envRoot} / "src" / "dss-config"
                                 / std::string{loc.subdir} / leaf;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }

    // Walk up to 8 ancestor dirs from cwd looking for
    // `src/dss-config/<subdir>/<name><suffix>`. Works whether the
    // binary is invoked from the repo root, build/, or a nested
    // tests/<area>/ build subdirectory (ctest's cwd varies).
    fs::path here = fs::current_path(ec);
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        const fs::path candidate =
            here / "src" / "dss-config" / std::string{loc.subdir} / leaf;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
        const fs::path parent = here.parent_path();
        if (parent == here) break;  // hit the filesystem root
        here = parent;
    }

    return std::unexpected(std::vector<ConfigDiagnostic>{
        {loc.invalidNameCode, DiagnosticSeverity::Error,
         std::string{loc.name},
         std::string{"no shipped "} + std::string{loc.kindLabel}
             + " config found in src/dss-config/" + std::string{loc.subdir} + "/"}});
}

std::optional<std::filesystem::path>
findShippedConfigDir(std::string_view                            subdir,
                     std::optional<std::filesystem::path> const& startPath) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Same precedence as `findShippedConfig` above: the explicit override FIRST,
    // so a shipped binary resolves config regardless of where it was launched.
    // (The driver's private copy of this walk omitted exactly this branch, which
    // is why `#include <stdio.h>` failed from any cwd outside the source tree —
    // see the header's WHY IT EXISTS note.)
    //
    // Same `std::getenv` READ-only discipline too: the compiler never WRITES the
    // environment, so the lookup is race-free; preserve that
    // no-env-writes-during-compilation invariant if CU-parallel compilation
    // ([[D-PERF-4-CU-PARALLELISM]]) ever lands.
    //
    // An engaged `startPath` outranks the environment — it is the caller saying
    // "discover from exactly here" (see the header), not a search hint.
    if (!startPath.has_value()) {
        if (const char* envRoot = std::getenv("DSS_CONFIG_ROOT");
            envRoot != nullptr && envRoot[0] != '\0') {
            const fs::path candidate =
                fs::path{envRoot} / "src" / "dss-config" / std::string{subdir};
            if (fs::is_directory(candidate, ec)) {
                return candidate;
            }
        }
    }

    // A set-but-miss override falls THROUGH to this walk, exactly as the file
    // form does — a stale override never worsens discovery. Bound and
    // termination match the file form too (8 hops, stop at the filesystem root)
    // so the two cannot drift on reach.
    fs::path here = startPath.value_or(fs::current_path(ec));
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        const fs::path candidate =
            here / "src" / "dss-config" / std::string{subdir};
        if (fs::is_directory(candidate, ec)) {
            return candidate;
        }
        const fs::path parent = here.parent_path();
        if (parent == here) break;  // hit the filesystem root
        here = parent;
    }

    // NOT merged with `findShippedConfig` into one "find the root, then probe"
    // helper, and that is a semantic decision rather than duplication left
    // standing: the file form must CONTINUE walking past an ancestor that has a
    // `src/dss-config/<subdir>` but not the requested leaf, while this form is
    // done the moment the directory itself resolves. A shared root-finder would
    // silently shorten the file form's reach.
    return std::nullopt;
}

} // namespace dss
