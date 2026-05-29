#include "link/object_format_schema.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/substrate/relocation_table.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dss {

namespace {

using json = nlohmann::json;
using Collector = substrate::DiagnosticCollector;

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
        return std::unexpected(std::move(coll).release());
    }
    if (!doc.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  "top-level value must be a JSON object");
        return std::unexpected(std::move(coll).release());
    }

    // dssObjectFormatVersion — same per-schema-file version contract
    // as TargetSchema's. v1 is the only accepted version today;
    // future LK* cycles bump as schema shape grows.
    if (!doc.contains("dssObjectFormatVersion")
     || !doc.at("dssObjectFormatVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::C_VersionMismatch, std::string{sourceLabel},
                  "missing or non-integer 'dssObjectFormatVersion'");
        return std::unexpected(std::move(coll).release());
    }
    int const ver = doc.at("dssObjectFormatVersion").get<int>();
    if (ver != 1) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssObjectFormatVersion",
                  std::format("only version 1 supported (got {})", ver));
        return std::unexpected(std::move(coll).release());
    }

    detail::ObjectFormatData data;
    data.id = substrate::mintMonotonicId<ObjectFormatSchemaId>();

    if (!doc.contains("format") || !doc.at("format").is_object()) {
        coll.emit(DiagnosticCode::C_MissingField, std::string{sourceLabel},
                  "missing 'format' object");
        return std::unexpected(std::move(coll).release());
    }
    auto const& format = doc.at("format");
    if (!format.contains("name") || !format.at("name").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/name",
                  "missing or non-string 'name'");
        return std::unexpected(std::move(coll).release());
    }
    data.name = format.at("name").get<std::string>();
    if (format.contains("version") && format.at("version").is_string()) {
        data.version = format.at("version").get<std::string>();
    }
    if (!format.contains("kind") || !format.at("kind").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/kind",
                  "missing or non-string 'kind' (one of 'elf' / 'pe' / "
                  "'macho' / 'wasm' / 'spirv')");
        return std::unexpected(std::move(coll).release());
    }
    auto const kindOpt = objectFormatKindFromName(
        format.at("kind").get<std::string>());
    if (!kindOpt.has_value()) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/format/kind",
                  "expected 'elf' / 'pe' / 'macho' / 'wasm' / 'spirv'");
        return std::unexpected(std::move(coll).release());
    }
    data.kind = *kindOpt;

    // relocations[] — substrate-tier; shares the cross-side
    // `relocation_table.hpp` substrate with TargetSchema so the
    // `{name, kind}` shape of plan 13 §2.6's reloc-taxonomy unifier
    // is identical-by-construction on both sides. No row-specific
    // extension fields on the format side (target side carries
    // `formula`).
    substrate::loadRelocationsTable<ObjectFormatRelocationInfo>(
        doc, data.relocations, data.relocationNameIndex,
        data.relocationKindIndex, coll);

    for (auto&& problem : data.validate()) {
        coll.emitRaw(std::move(problem));
    }

    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll).release());
    }

    return std::make_shared<ObjectFormatSchema>(std::move(data));
}

} // namespace dss
