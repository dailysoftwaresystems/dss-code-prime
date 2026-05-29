#include "core/types/config_path_walk.hpp"

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

    // Walk up to 8 ancestor dirs from cwd looking for
    // `src/dss-config/<subdir>/<name><suffix>`. Works whether the
    // binary is invoked from the repo root, build/, or a nested
    // tests/<area>/ build subdirectory (ctest's cwd varies).
    std::error_code ec;
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
