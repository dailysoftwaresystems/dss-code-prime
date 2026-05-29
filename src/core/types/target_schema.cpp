#include "core/types/target_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace dss {

LoadResult<std::shared_ptr<TargetSchema>> TargetSchema::loadFromFile(
    std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             path.string(), "cannot open file"}});
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return loadFromText(std::move(buf).str(), path.string());
}

LoadResult<std::shared_ptr<TargetSchema>> TargetSchema::loadShipped(
    std::string_view name) {
    // Reject path-like names; this loader resolves logical target
    // names (`x86_64` / `arm64` / ...), not arbitrary paths.
    if (name.empty() || name.find('/') != std::string_view::npos
        || name.find('\\') != std::string_view::npos
        || name.front() == '.') {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_InvalidLanguageName, DiagnosticSeverity::Error,
             std::string{name}, "invalid shipped-target name"}});
    }

    namespace fs = std::filesystem;
    const std::string leaf = std::string{name} + ".target.json";

    // cwd-walk: ctest's cwd varies (build/, build/tests/lir/, repo root).
    // Same 8-level limit + same `src/source-config/...` discipline as
    // GrammarSchema::loadShipped.
    std::error_code ec;
    fs::path here = fs::current_path(ec);
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        const fs::path candidate = here / "src" / "dss-config" / "targets" / leaf;
        if (fs::exists(candidate, ec)) {
            return loadFromFile(candidate);
        }
        const fs::path parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
    return std::unexpected(std::vector<ConfigDiagnostic>{
        {DiagnosticCode::C_InvalidLanguageName, DiagnosticSeverity::Error,
         std::string{name},
         "no shipped target config found in src/dss-config/targets/"}});
}

// `loadFromText` is implemented in target_schema_json.cpp (it pulls
// in nlohmann::json which we deliberately keep out of this header
// so the public API surface stays free of the JSON dependency —
// same boundary GrammarSchema observes).

} // namespace dss
