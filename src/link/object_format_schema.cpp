#include "link/object_format_schema.hpp"

#include "core/substrate/relocation_table.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

namespace dss {

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadFromFile(std::filesystem::path const& path) {
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

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadShipped(std::string_view name) {
    auto path = findShippedConfig({name, "object-formats", ".format.json",
                                   "object format",
                                   DiagnosticCode::C_InvalidLanguageName});
    if (!path) return std::unexpected(std::move(path).error());
    return loadFromFile(*path);
}

namespace detail {

namespace {

ConfigDiagnostic makeProblem(std::string path, std::string message) {
    return ConfigDiagnostic{
        DiagnosticCode::C_MalformedJson,
        DiagnosticSeverity::Error,
        std::move(path),
        std::move(message),
    };
}

} // namespace

std::vector<ConfigDiagnostic> ObjectFormatData::validate() const {
    std::vector<ConfigDiagnostic> problems;
    auto fail = [&](std::string path, std::string msg) {
        problems.push_back(makeProblem(std::move(path), std::move(msg)));
    };

    // Format kind must be a real shipped format. `Unknown` is the
    // invalid sentinel — a JSON file that names "unknown" (or omits
    // the field, which the loader rejects upstream) is not a real
    // format declaration.
    if (kind == ObjectFormatKind::Unknown) {
        fail("/format/kind",
             "format kind 'unknown' is reserved as the invalid sentinel; "
             "declare one of 'elf' / 'pe' / 'macho' / 'wasm' / 'spirv'");
    }

    // Cross-row reloc uniqueness + non-empty-name + non-zero-kind:
    // shared substrate with TargetSchema so the two sides of plan
    // 13 §2.6's reloc-taxonomy unifier are validated identically.
    substrate::validateRelocationsTable<ObjectFormatRelocationInfo>(
        relocations, fail);

    return problems;
}

} // namespace detail

} // namespace dss
