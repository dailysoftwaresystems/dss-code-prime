#include "link/object_format_schema.hpp"

#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
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

    // Same discipline as `TargetSchemaData::validate()` for the
    // assembler-side relocations table — substrate-tier so the
    // format-side row always satisfies the cross-reference contract
    // with plan 13 §2.6.
    {
        std::unordered_map<RelocationKind, std::size_t> seenKind;
        for (std::size_t i = 0; i < relocations.size(); ++i) {
            auto const& r = relocations[i];
            if (r.name.empty()) {
                fail(std::format("/relocations/{}/name", i),
                     "relocation row: 'name' must be a non-empty string");
            }
            if (!r.kind.valid()) {
                fail(std::format("/relocations/{}/kind", i),
                     std::format("relocation '{}': 'kind' must be != 0 "
                                 "(slot 0 is reserved as the invalid sentinel)",
                                 r.name));
                continue;
            }
            auto [it, fresh] = seenKind.emplace(r.kind, i);
            if (!fresh) {
                fail(std::format("/relocations/{}/kind", i),
                     std::format("relocation '{}': duplicate 'kind' value {} "
                                 "(already declared by relocation '{}' at /relocations/{})",
                                 r.name, r.kind.v,
                                 relocations[it->second].name, it->second));
            }
        }
    }

    return problems;
}

} // namespace detail

} // namespace dss
