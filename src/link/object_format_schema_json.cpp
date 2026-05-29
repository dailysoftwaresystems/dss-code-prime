#include "link/object_format_schema.hpp"

#include "core/substrate/mint_monotonic_id.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dss {

namespace {

using json = nlohmann::json;

struct Collector {
    std::vector<ConfigDiagnostic> diagnostics;
    void emit(DiagnosticCode code, std::string path, std::string message,
              DiagnosticSeverity sev = DiagnosticSeverity::Error) {
        diagnostics.push_back({code, sev, std::move(path), std::move(message)});
    }
};

} // namespace

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadFromText(std::string_view jsonText,
                                  std::string_view sourceLabel) {
    Collector coll;
    json doc;
    try {
        doc = json::parse(jsonText);
    } catch (json::parse_error const& e) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  std::format("JSON parse error: {}", e.what()));
        return std::unexpected(std::move(coll.diagnostics));
    }
    if (!doc.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  "top-level value must be a JSON object");
        return std::unexpected(std::move(coll.diagnostics));
    }

    // dssObjectFormatVersion — same per-schema-file version contract
    // as TargetSchema's. v1 is the only accepted version today;
    // future LK* cycles bump as schema shape grows.
    if (!doc.contains("dssObjectFormatVersion")
     || !doc.at("dssObjectFormatVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::C_VersionMismatch, std::string{sourceLabel},
                  "missing or non-integer 'dssObjectFormatVersion'");
        return std::unexpected(std::move(coll.diagnostics));
    }
    int const ver = doc.at("dssObjectFormatVersion").get<int>();
    if (ver != 1) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssObjectFormatVersion",
                  std::format("only version 1 supported (got {})", ver));
        return std::unexpected(std::move(coll.diagnostics));
    }

    detail::ObjectFormatData data;
    data.id = substrate::mintMonotonicId<ObjectFormatSchemaId>();

    if (!doc.contains("format") || !doc.at("format").is_object()) {
        coll.emit(DiagnosticCode::C_MissingField, std::string{sourceLabel},
                  "missing 'format' object");
        return std::unexpected(std::move(coll.diagnostics));
    }
    auto const& format = doc.at("format");
    if (!format.contains("name") || !format.at("name").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/name",
                  "missing or non-string 'name'");
        return std::unexpected(std::move(coll.diagnostics));
    }
    data.name = format.at("name").get<std::string>();
    if (format.contains("version") && format.at("version").is_string()) {
        data.version = format.at("version").get<std::string>();
    }
    if (!format.contains("kind") || !format.at("kind").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/kind",
                  "missing or non-string 'kind' (one of 'elf' / 'pe' / "
                  "'macho' / 'wasm' / 'spirv')");
        return std::unexpected(std::move(coll.diagnostics));
    }
    auto const kindOpt = objectFormatKindFromName(
        format.at("kind").get<std::string>());
    if (!kindOpt.has_value()) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/format/kind",
                  "expected 'elf' / 'pe' / 'macho' / 'wasm' / 'spirv'");
        return std::unexpected(std::move(coll.diagnostics));
    }
    data.kind = *kindOpt;

    // relocations[] — substrate-tier; identical contract to
    // TargetSchema's relocations[] loader so the cross-reference
    // unifier (plan 13 §2.6) has symmetric validation on both sides.
    if (doc.contains("relocations")) {
        if (!doc.at("relocations").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/relocations",
                      "'relocations' must be an array");
        } else {
            auto const& rels = doc.at("relocations");
            data.relocations.reserve(rels.size());
            for (std::size_t i = 0; i < rels.size(); ++i) {
                auto const& r = rels[i];
                if (!r.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/relocations/{}", i),
                              "relocation entry must be an object");
                    continue;
                }
                ObjectFormatRelocationInfo info;
                if (!r.contains("name") || !r.at("name").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/relocations/{}/name", i),
                              "missing or non-string 'name'");
                    continue;
                }
                info.name = r.at("name").get<std::string>();
                if (!r.contains("kind") || !r.at("kind").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/relocations/{}/kind", i),
                              "missing or non-integer 'kind' (must be the "
                              "non-zero uint32 tag that matches the assembler-"
                              "side TargetSchema relocations[] table)");
                    continue;
                }
                std::int64_t const v = r.at("kind").get<std::int64_t>();
                if (v < 0 || v > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/relocations/{}/kind", i),
                              std::format("'kind' ({}) must fit in [0, {}]",
                                          v, std::numeric_limits<std::uint32_t>::max()));
                    continue;
                }
                info.kind = RelocationKind{static_cast<std::uint32_t>(v)};
                std::uint16_t const idx =
                    static_cast<std::uint16_t>(data.relocations.size());
                bool const freshName =
                    data.relocationNameIndex.emplace(info.name, idx).second;
                if (!freshName) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/relocations/{}/name", i),
                              std::format("duplicate relocation name '{}'",
                                          info.name));
                    continue;
                }
                (void)data.relocationKindIndex.emplace(info.kind, idx);
                data.relocations.push_back(std::move(info));
            }
        }
    }

    for (auto&& problem : data.validate()) {
        coll.diagnostics.push_back(std::move(problem));
    }

    if (!coll.diagnostics.empty()) {
        bool fatal = false;
        for (auto const& d : coll.diagnostics) {
            if (d.severity == DiagnosticSeverity::Error) { fatal = true; break; }
        }
        if (fatal) return std::unexpected(std::move(coll.diagnostics));
    }

    return std::make_shared<ObjectFormatSchema>(std::move(data));
}

} // namespace dss
