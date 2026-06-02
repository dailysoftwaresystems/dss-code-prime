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

LoadResult<std::filesystem::path>
findShippedFfiHeader(std::string_view headerRelPath) {
    namespace fs = std::filesystem;

    // Reject empty + leading-`.` (would hide a `.config` file) first.
    if (headerRelPath.empty() || headerRelPath.front() == '.') {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_InvalidShippedFfiHeaderPath,
             DiagnosticSeverity::Error,
             std::string{headerRelPath},
             "shipped-ffi-header path must be non-empty and not start with '.'"}});
    }

    // Use the platform-correct filesystem check rather than a leading-
    // char check. Catches POSIX `/etc/passwd`, Windows `C:\...` (drive
    // letter), `\\server\share` (UNC), forward-slash variants on
    // Windows. The pre-fold check missed Windows absolute paths
    // (silent-failure CRITICAL-1 post-FF2-#2 fold).
    fs::path const relPath{headerRelPath};
    if (relPath.is_absolute() || relPath.has_root_name()
        || relPath.has_root_directory()) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_InvalidShippedFfiHeaderPath,
             DiagnosticSeverity::Error,
             std::string{headerRelPath},
             "shipped-ffi-header path must be relative (no drive letter, "
             "no leading '/' or '\\')"}});
    }

    // Per-component `..` traversal check. The previous `find("..")`
    // substring search false-rejected names like `foo..bar` (no
    // traversal intent) while also matching the real cases. Iterate
    // components — each `..` is a parent-dir reference regardless of
    // whether it appears as `../foo` or `foo/../bar`.
    for (auto const& comp : relPath) {
        if (comp == "..") {
            return std::unexpected(std::vector<ConfigDiagnostic>{
                {DiagnosticCode::C_InvalidShippedFfiHeaderPath,
                 DiagnosticSeverity::Error,
                 std::string{headerRelPath},
                 "shipped-ffi-header path must not contain a '..' component"}});
        }
    }

    std::error_code ec;
    fs::path here = fs::current_path(ec);
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        const fs::path candidate =
            here / "src" / "dss-config" / "ffi-headers" / relPath;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
        const fs::path parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }

    // Genuinely-not-found is a deploy/install bug, not a caller-API
    // bug. Use a DIFFERENT C_* code so consumers can route
    // `--suppress` independently. (Reuses C_MissingField which is
    // the existing "config not where it should be" code; specific
    // not-found code can split later if needed.)
    return std::unexpected(std::vector<ConfigDiagnostic>{
        {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
         std::string{headerRelPath},
         "no shipped FFI header found in src/dss-config/ffi-headers/"}});
}

} // namespace dss
