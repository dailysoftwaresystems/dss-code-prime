#include "core/types/config_path_walk.hpp"

#include <cstdlib>
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
    // finds config instead of nulling the loader. Unset (the production
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

} // namespace dss
