#include "core/types/target_schema.hpp"

#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
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
    auto path = findShippedConfig({name, "targets", ".target.json", "target",
                                   DiagnosticCode::C_InvalidLanguageName});
    if (!path) return std::unexpected(std::move(path).error());
    return loadFromFile(*path);
}

// `loadFromText` is implemented in target_schema_json.cpp (mirrors the
// GrammarSchema boundary — JSON dep stays off the public header).

} // namespace dss
