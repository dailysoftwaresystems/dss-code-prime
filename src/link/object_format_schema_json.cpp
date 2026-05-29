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
    // is identical-by-construction on both sides. The `nativeId`
    // field is the format's wire tag (e.g. ELF R_X86_64_PC32 = 2).
    substrate::loadRelocationsTable<ObjectFormatRelocationInfo>(
        doc, data.relocations, data.relocationNameIndex,
        data.relocationKindIndex, coll,
        [](nlohmann::json const& r, ObjectFormatRelocationInfo& info,
           Collector& c, std::size_t i) -> bool {
            if (!r.contains("nativeId") || !r.at("nativeId").is_number_integer()) {
                c.emit(DiagnosticCode::C_MissingField,
                       std::format("/relocations/{}/nativeId", i),
                       "missing or non-integer 'nativeId' (format-specific "
                       "wire tag, e.g. ELF R_X86_64_PC32 = 2)");
                return false;
            }
            std::int64_t const v = r.at("nativeId").get<std::int64_t>();
            if (v <= 0 || v > 0xFFFFFFFFLL) {
                c.emit(DiagnosticCode::C_MalformedJson,
                       std::format("/relocations/{}/nativeId", i),
                       std::format("'nativeId' ({}) must be in (0, 2^32)", v));
                return false;
            }
            info.nativeId = static_cast<std::uint32_t>(v);
            return true;
        });

    // sections[] — D-LK4-2 schema row. Each entry maps a universal
    // SectionKind to format-native name + structural fields.
    if (doc.contains("sections")) {
        if (!doc.at("sections").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/sections",
                      "'sections' must be an array");
        } else {
            auto const& secs = doc.at("sections");
            data.sections.reserve(secs.size());
            for (std::size_t i = 0; i < secs.size(); ++i) {
                auto const& s = secs[i];
                if (!s.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}", i),
                              "section entry must be an object");
                    continue;
                }
                ObjectFormatSectionInfo info;
                if (!s.contains("kind") || !s.at("kind").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/sections/{}/kind", i),
                              "missing or non-string 'kind' (one of 'text' "
                              "/ 'rodata' / 'data' / 'bss' / 'symtab' / "
                              "'strtab' / 'reloc' / 'dynamic' / 'note' / "
                              "'debug' / 'custom')");
                    continue;
                }
                auto const kOpt =
                    sectionKindFromName(s.at("kind").get<std::string>());
                if (!kOpt.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/kind", i),
                              "unknown SectionKind name");
                    continue;
                }
                info.kind = *kOpt;
                if (!s.contains("name") || !s.at("name").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/sections/{}/name", i),
                              "missing or non-string 'name'");
                    continue;
                }
                info.name = s.at("name").get<std::string>();
                auto readU64 = [&](char const* field, std::uint64_t& out) {
                    if (!s.contains(field)) return;
                    if (!s.at(field).is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/{}", i, field),
                                  std::format("'{}' must be a non-negative "
                                              "integer",
                                              field));
                        return;
                    }
                    std::int64_t const v = s.at(field).get<std::int64_t>();
                    if (v < 0) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/{}", i, field),
                                  std::format("'{}' ({}) must be >= 0",
                                              field, v));
                        return;
                    }
                    out = static_cast<std::uint64_t>(v);
                };
                std::uint64_t typeRaw = 0;
                readU64("type", typeRaw);
                if (typeRaw > 0xFFFFFFFFu) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/type", i),
                              "'type' must fit in 32 bits");
                    continue;
                }
                info.type = static_cast<std::uint32_t>(typeRaw);
                readU64("flags", info.flags);
                readU64("addrAlign", info.addrAlign);
                readU64("entrySize", info.entrySize);
                std::uint16_t const idx =
                    static_cast<std::uint16_t>(data.sections.size());
                auto [it, fresh] =
                    data.sectionKindIndex.emplace(info.kind, idx);
                if (!fresh) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/kind", i),
                              std::format("duplicate section kind '{}'",
                                          std::string{sectionKindName(info.kind)}));
                    continue;
                }
                data.sections.push_back(std::move(info));
            }
        }
    }

    // ELF identity block — read only when format kind is Elf (other
    // formats' identity sub-blocks land alongside LK2/LK3).
    if (data.kind == ObjectFormatKind::Elf && doc.contains("elf")) {
        auto const& e = doc.at("elf");
        if (!e.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/elf",
                      "'elf' must be an object when format.kind == 'elf'");
        } else {
            auto readU16 = [&](char const* field, std::uint16_t& out,
                               std::int64_t max) {
                if (!e.contains(field) || !e.at(field).is_number_integer())
                    return;
                std::int64_t const v = e.at(field).get<std::int64_t>();
                if (v < 0 || v > max) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/elf/{}", field),
                              std::format("'{}' ({}) out of range [0, {}]",
                                          field, v, max));
                    return;
                }
                out = static_cast<std::uint16_t>(v);
            };
            // class: "elf32" | "elf64" (ELFCLASS values from psABI).
            if (e.contains("class") && e.at("class").is_string()) {
                auto const c = e.at("class").get<std::string>();
                if      (c == "elf32") data.elf.fileClass = 1;
                else if (c == "elf64") data.elf.fileClass = 2;
                else coll.emit(DiagnosticCode::C_MalformedJson, "/elf/class",
                               "'class' must be 'elf32' or 'elf64'");
            }
            // data: "lsb" | "msb".
            if (e.contains("data") && e.at("data").is_string()) {
                auto const d = e.at("data").get<std::string>();
                if      (d == "lsb") data.elf.dataEncoding = 1;
                else if (d == "msb") data.elf.dataEncoding = 2;
                else coll.emit(DiagnosticCode::C_MalformedJson, "/elf/data",
                               "'data' must be 'lsb' or 'msb'");
            }
            // osabi: string name → numeric (ELFOSABI_*). Default 0 = SysV.
            if (e.contains("osabi") && e.at("osabi").is_string()) {
                auto const o = e.at("osabi").get<std::string>();
                if      (o == "sysv"     || o == "none") data.elf.osabi = 0;
                else if (o == "hpux"   ) data.elf.osabi = 1;
                else if (o == "netbsd" ) data.elf.osabi = 2;
                else if (o == "gnu"    || o == "linux") data.elf.osabi = 3;
                else if (o == "freebsd") data.elf.osabi = 9;
                else coll.emit(DiagnosticCode::C_MalformedJson, "/elf/osabi",
                               "'osabi' must be one of 'sysv' / 'gnu' / "
                               "'freebsd' / 'netbsd' / 'hpux' / 'none'");
            }
            std::uint16_t abiVerRaw = 0;
            readU16("abiVersion", abiVerRaw, 255);
            data.elf.abiVersion = static_cast<std::uint8_t>(abiVerRaw);
            readU16("machine", data.elf.machine, 0xFFFF);
        }
    }

    for (auto&& problem : data.validate()) {
        coll.emitRaw(std::move(problem));
    }

    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll).release());
    }

    return std::make_shared<ObjectFormatSchema>(std::move(data));
}

} // namespace dss
