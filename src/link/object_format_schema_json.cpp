#include "link/object_format_schema.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/substrate/relocation_table.hpp"
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
    // Cross-tier symmetry with `target.name` (D-LK6-8.2 post-fold #2
    // architect Q3): `format.name` is the label every walker
    // diagnostic message uses. An empty or whitespace-only name
    // would produce unintelligible diagnostics silently. The same
    // non-empty-non-whitespace discipline applies on both sides.
    auto const isAsciiSpace = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r'
            || c == '\v' || c == '\f';
    };
    bool const isBadName = [&]() noexcept {
        if (data.name.empty()) return true;
        if (isAsciiSpace(data.name.front())) return true;
        if (isAsciiSpace(data.name.back()))  return true;
        for (char c : data.name) if (!isAsciiSpace(c)) return false;
        return true;  // all whitespace
    }();
    if (isBadName) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/name",
                  "'name' must be a non-empty string with no leading "
                  "or trailing whitespace — appears verbatim in every "
                  "walker diagnostic.");
        return std::unexpected(std::move(coll).release());
    }
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

    // Cross-format identity-block validation (test-analyzer Gap 6
    // fold, LK8 review): a schema's `kind` is the load-bearing
    // dispatcher — every per-format identity block (`elf`, `pe`,
    // `optionalHeader`, `macho`, `image`) is only consumed when
    // its matching kind is set. A schema declaring `kind: wasm`
    // with a stray `elf` / `pe` / `macho` block would silently
    // drop the block. Reject loudly so a copy-paste-then-rename
    // mistake surfaces at schema load.
    struct CrossKindGuard {
        ObjectFormatKind expectedKind;
        char const*      blockName;
    };
    constexpr CrossKindGuard kCrossKindRules[] = {
        { ObjectFormatKind::Elf,    "elf"            },
        { ObjectFormatKind::Pe,     "pe"             },
        { ObjectFormatKind::Pe,     "optionalHeader" },
        { ObjectFormatKind::MachO,  "macho"          },
        { ObjectFormatKind::MachO,  "image"          },
    };
    for (auto const& rule : kCrossKindRules) {
        if (data.kind != rule.expectedKind
         && doc.contains(rule.blockName)) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::string{"/"} + rule.blockName,
                      std::string{"identity block '"} + rule.blockName
                          + "' is only meaningful when format.kind == '"
                          + std::string{
                                objectFormatKindName(rule.expectedKind)}
                          + "' (got kind '"
                          + std::string{objectFormatKindName(data.kind)}
                          + "'). A stray block of the wrong kind would "
                            "be silently dropped — fix the block name or "
                            "the format.kind.");
        }
    }
    // Universal-field positive assertion for Wasm + Spirv (type-
    // design Q3 fold, LK8 post-fold review). WASM has no native
    // relocations, no per-section file-layout knobs, and its
    // entry-point lives inside the Start section's function index
    // (not a top-level symbol name). Spirv's `OpEntryPoint` is
    // similarly emitted inline as a typed module instruction, not
    // declared as a substrate-tier symbol name. Skeleton schemas
    // (`wasm32-v1.format.json`) ship with these fields ABSENT; a
    // future plan-18 schema that declares them would be silently
    // ignored by the walker. Reject loudly so a stray
    // `sections` / `relocations` / `entryPoint` surfaces at load
    // and gets re-anchored against plan 18 / plan 17 vocabulary.
    if (data.kind == ObjectFormatKind::Wasm
     || data.kind == ObjectFormatKind::Spirv) {
        char const* const universalFields[] = {
            "sections", "relocations", "entryPoint",
        };
        for (auto const* field : universalFields) {
            if (doc.contains(field)) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::string{"/"} + field,
                          std::string{"format kind '"}
                              + std::string{objectFormatKindName(data.kind)}
                              + "' must not declare a top-level '"
                              + field
                              + "' field — WASM / SPIR-V emit this "
                                "information through their own format-"
                                "native section vocabulary (plan 18 / "
                                "plan 17). A top-level declaration "
                                "would be silently ignored by the "
                                "walker.");
            }
        }
    }

    // Top-level `entryPoint` — universal entry-symbol name for
    // executable artifacts (e.g. "_start" / "main" / Mach-O's
    // LC_MAIN target). Empty for relocatable artifacts. The walker
    // resolves this against AssembledModule's symbols at emit time
    // to compute the entry virtual address.
    if (doc.contains("entryPoint")) {
        if (!doc.at("entryPoint").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/entryPoint",
                      "'entryPoint' must be a string");
        } else {
            data.entryPoint = doc.at("entryPoint").get<std::string>();
        }
    }

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
                // `segment` is optional for ELF/PE (empty default);
                // Mach-O validate() rejects empty here.
                if (s.contains("segment")) {
                    if (!s.at("segment").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/segment", i),
                                  "'segment' must be a string");
                    } else {
                        info.segment = s.at("segment").get<std::string>();
                    }
                }
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
                readU64("virtualAddress", info.virtualAddress);
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

    // Per-format identity sub-block readers — each runs only when
    // `format.kind` matches its arm. Mach-O (LK3) will add a third
    // arm on the same pattern.

    // ELF identity block — read only when format kind is Elf.
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
            // `type`: closed-enum `ElfObjectType` (rel/exec/dyn)
            // round-tripped through `EnumNameTable`. Default Rel
            // keeps LK1 cycle 1 schemas working unchanged.
            if (e.contains("type") && e.at("type").is_string()) {
                auto const tName = e.at("type").get<std::string>();
                auto const tEnum = elfObjectTypeFromName(tName);
                if (tEnum.has_value()) {
                    data.elf.objectType = *tEnum;
                } else {
                    coll.emit(DiagnosticCode::C_MalformedJson, "/elf/type",
                              "'type' must be 'rel' / 'exec' / 'dyn'");
                }
            }
            // `interpreter`: PT_INTERP path (dynamic linker name).
            // Optional in JSON. An empty-string literal (`""`) is
            // rejected at load: the Linux kernel rejects ELFs with a
            // zero-length PT_INTERP path, so `""` is unambiguously a
            // config error (3-agent convergence: code-reviewer +
            // silent-failure + comment-analyzer on LK6 cycle 2b.1
            // review). Absent field = field stays at its default
            // `""` and the walker treats it as "self-contained
            // executable" (no PT_INTERP emission).
            if (e.contains("interpreter")) {
                if (!e.at("interpreter").is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/elf/interpreter",
                              "'interpreter' must be a string (e.g. "
                              "'/lib64/ld-linux-x86-64.so.2')");
                } else {
                    auto const value =
                        e.at("interpreter").get<std::string>();
                    if (value.empty()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/elf/interpreter",
                                  "'interpreter' must not be empty — "
                                  "the Linux kernel rejects ELFs with "
                                  "a zero-length PT_INTERP path. Omit "
                                  "the field entirely for self-"
                                  "contained executables.");
                    } else {
                        data.elf.interpreter = value;
                    }
                }
            }
            // `pageAlign`: PT_LOAD p_align for Exec images. Required
            // for ET_EXEC at validate() — the kernel rejects ELF
            // exec'd images whose p_align is smaller than the
            // runtime page size. Each (arch × OS) schema declares
            // its own value (D-LK6-3).
            if (e.contains("pageAlign")) {
                if (!e.at("pageAlign").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/elf/pageAlign",
                              "'pageAlign' must be an integer (PT_LOAD "
                              "p_align, e.g. 4096 for x86_64 Linux or "
                              "65536 for ARM64-64K)");
                } else {
                    std::int64_t const pa =
                        e.at("pageAlign").get<std::int64_t>();
                    if (pa <= 0
                     || (static_cast<std::uint64_t>(pa) &
                         (static_cast<std::uint64_t>(pa) - 1u)) != 0u) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/elf/pageAlign",
                                  "'pageAlign' must be a positive "
                                  "power of two (kernel constraint: "
                                  "p_vaddr % p_align == p_offset % "
                                  "p_align)");
                    } else {
                        data.elf.pageAlign =
                            static_cast<std::uint64_t>(pa);
                    }
                }
            }
            // `bindNow`: eager vs lazy dynamic-binding choice.
            // Optional; defaults to `true` (v1 stance, plan 14 §5
            // risk row). `false` is the lazy-binding upgrade path
            // anchored at D-LK6-11 — v1 walker fails loud on
            // `bindNow == false` until D-LK6-11 lands.
            if (e.contains("bindNow")) {
                if (!e.at("bindNow").is_boolean()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/elf/bindNow",
                              "'bindNow' must be a boolean (true = "
                              "eager / DF_1_NOW, false = lazy / "
                              ".rela.plt + JUMP_SLOT — anchored at "
                              "D-LK6-11, not yet implemented)");
                } else {
                    data.elf.bindNow =
                        e.at("bindNow").get<bool>();
                }
            }
        }
    }

    // PE/COFF identity block — read only when format kind is Pe.
    if (data.kind == ObjectFormatKind::Pe && doc.contains("pe")) {
        auto const& p = doc.at("pe");
        if (!p.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/pe",
                      "'pe' must be an object when format.kind == 'pe'");
        } else {
            auto readU16 = [&](char const* field, std::uint16_t& out,
                               std::int64_t max) {
                if (!p.contains(field) || !p.at(field).is_number_integer())
                    return;
                std::int64_t const v = p.at(field).get<std::int64_t>();
                if (v < 0 || v > max) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/pe/{}", field),
                              std::format("'{}' ({}) out of range [0, {}]",
                                          field, v, max));
                    return;
                }
                out = static_cast<std::uint16_t>(v);
            };
            readU16("machine", data.pe.machine, 0xFFFF);
            readU16("characteristics", data.pe.characteristics, 0xFFFF);
            // `type`: closed-enum PeObjectType (obj/exec/dll).
            // Default Obj keeps LK2 cycle 1 schemas unchanged.
            if (p.contains("type") && p.at("type").is_string()) {
                auto const tName = p.at("type").get<std::string>();
                auto const tEnum = peObjectTypeFromName(tName);
                if (tEnum.has_value()) {
                    data.pe.objectType = *tEnum;
                } else {
                    coll.emit(DiagnosticCode::C_MalformedJson, "/pe/type",
                              "'type' must be 'obj' / 'exec' / 'dll'");
                }
            }
        }
    }

    // PE32+ Optional Header — read only when PE objectType != Obj.
    // The walker emits the optional header for Exec/Dll; Obj schemas
    // never carry it, and validate() rejects an `optionalHeader` key
    // on an Obj schema as a load-time config error (symmetric with
    // ELF ET_REL's virtualAddress=0 rejection).
    if (data.kind == ObjectFormatKind::Pe && doc.contains("optionalHeader")) {
        auto const& oh = doc.at("optionalHeader");
        if (!oh.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/optionalHeader",
                      "'optionalHeader' must be an object");
        } else {
            auto readU16 = [&](char const* field, std::uint16_t& out) {
                if (!oh.contains(field)) return;
                if (!oh.at(field).is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' must be an integer", field));
                    return;
                }
                std::int64_t const v = oh.at(field).get<std::int64_t>();
                if (v < 0 || v > 0xFFFF) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' ({}) out of u16 range",
                                          field, v));
                    return;
                }
                out = static_cast<std::uint16_t>(v);
            };
            auto readU32 = [&](char const* field, std::uint32_t& out) {
                if (!oh.contains(field)) return;
                if (!oh.at(field).is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' must be an integer", field));
                    return;
                }
                std::int64_t const v = oh.at(field).get<std::int64_t>();
                if (v < 0 || v > 0xFFFFFFFFLL) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' ({}) out of u32 range",
                                          field, v));
                    return;
                }
                out = static_cast<std::uint32_t>(v);
            };
            auto readU64 = [&](char const* field, std::uint64_t& out) {
                if (!oh.contains(field)) return;
                if (!oh.at(field).is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' must be an integer", field));
                    return;
                }
                std::int64_t const v = oh.at(field).get<std::int64_t>();
                if (v < 0) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' ({}) must be non-negative",
                                          field, v));
                    return;
                }
                out = static_cast<std::uint64_t>(v);
            };
            readU16("magic", data.peOptionalHeader.magic);
            readU64("imageBase", data.peOptionalHeader.imageBase);
            readU32("sectionAlignment",
                    data.peOptionalHeader.sectionAlignment);
            readU32("fileAlignment", data.peOptionalHeader.fileAlignment);
            readU16("majorOperatingSystemVersion",
                    data.peOptionalHeader.majorOperatingSystemVersion);
            readU16("minorOperatingSystemVersion",
                    data.peOptionalHeader.minorOperatingSystemVersion);
            readU16("majorSubsystemVersion",
                    data.peOptionalHeader.majorSubsystemVersion);
            readU16("minorSubsystemVersion",
                    data.peOptionalHeader.minorSubsystemVersion);
            readU16("subsystem", data.peOptionalHeader.subsystem);
            readU16("dllCharacteristics",
                    data.peOptionalHeader.dllCharacteristics);
            readU64("sizeOfStackReserve",
                    data.peOptionalHeader.sizeOfStackReserve);
            readU64("sizeOfStackCommit",
                    data.peOptionalHeader.sizeOfStackCommit);
            readU64("sizeOfHeapReserve",
                    data.peOptionalHeader.sizeOfHeapReserve);
            readU64("sizeOfHeapCommit",
                    data.peOptionalHeader.sizeOfHeapCommit);
            // Plan 14 LK7 — Authenticode codesign placeholder
            // reservation. Optional; defaults to 0 (no reservation,
            // no security directory entry). Multiple-of-8 enforced
            // at validate() (PE COFF §5.9.1 alignment).
            readU32("attributeCertReserveSize",
                    data.peOptionalHeader.attributeCertReserveSize);
        }
    }

    // Mach-O identity block — read only when format kind is MachO.
    if (data.kind == ObjectFormatKind::MachO && doc.contains("macho")) {
        auto const& m = doc.at("macho");
        if (!m.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/macho",
                      "'macho' must be an object when format.kind == 'macho'");
        } else {
            auto readU32 = [&](char const* field, std::uint32_t& out) {
                if (!m.contains(field) || !m.at(field).is_number_integer())
                    return;
                std::int64_t const v = m.at(field).get<std::int64_t>();
                if (v < 0
                 || v > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/macho/{}", field),
                              std::format("'{}' ({}) out of range [0, 2^32)",
                                          field, v));
                    return;
                }
                out = static_cast<std::uint32_t>(v);
            };
            readU32("cputype",    data.macho.cputype);
            readU32("cpusubtype", data.macho.cpusubtype);
            readU32("flags",      data.macho.flags);
            // `filetype`: closed enum MachOObjectType. Accepts the
            // string form ("object"/"execute"/"dylib") OR the
            // integer wire value (1/2/6) for back-compat with
            // pre-enum shipped JSONs. Unknown values fail loud.
            if (m.contains("filetype")) {
                auto const& ft = m.at("filetype");
                if (ft.is_string()) {
                    auto const tEnum = machoObjectTypeFromName(
                        ft.get<std::string>());
                    if (tEnum.has_value()) {
                        data.macho.filetype = *tEnum;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/macho/filetype",
                                  "'filetype' must be 'object' / "
                                  "'execute' / 'dylib'");
                    }
                } else if (ft.is_number_integer()) {
                    std::int64_t const v = ft.get<std::int64_t>();
                    if (v == 1) data.macho.filetype = MachOObjectType::Object;
                    else if (v == 2) data.macho.filetype = MachOObjectType::Execute;
                    else if (v == 6) data.macho.filetype = MachOObjectType::Dylib;
                    else {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/macho/filetype",
                                  std::format("'filetype' integer {} "
                                              "not in {{1,2,6}}", v));
                    }
                } else {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/macho/filetype",
                              "'filetype' must be a string or integer");
                }
            }
        }
    }

    // Mach-O image block — read only when format kind is MachO and
    // an `image` key is present. Validate() will reject the key on
    // a MH_OBJECT schema, and require its full population for
    // MH_EXECUTE (symmetric with PE's optionalHeader gate).
    if (data.kind == ObjectFormatKind::MachO && doc.contains("image")) {
        auto const& im = doc.at("image");
        if (!im.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/image",
                      "'image' must be an object");
        } else {
            if (im.contains("pageZeroSize")) {
                if (!im.at("pageZeroSize").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/image/pageZeroSize",
                              "'pageZeroSize' must be an integer");
                } else {
                    std::int64_t const v =
                        im.at("pageZeroSize").get<std::int64_t>();
                    if (v < 0) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/pageZeroSize",
                                  "'pageZeroSize' must be non-negative");
                    } else {
                        data.machoImage.pageZeroSize =
                            static_cast<std::uint64_t>(v);
                    }
                }
            }
            if (im.contains("dylinkerPath")) {
                if (!im.at("dylinkerPath").is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/image/dylinkerPath",
                              "'dylinkerPath' must be a string");
                } else {
                    data.machoImage.dylinkerPath =
                        im.at("dylinkerPath").get<std::string>();
                }
            }
            if (im.contains("loadDylibs")) {
                if (!im.at("loadDylibs").is_array()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/image/loadDylibs",
                              "'loadDylibs' must be an array — each "
                              "entry is either a bare string (path "
                              "sugar) or an object {path: ...}");
                } else {
                    auto const& arr = im.at("loadDylibs");
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        if (arr[i].is_string()) {
                            data.machoImage.loadDylibs.push_back(
                                MachODylibRef{arr[i].get<std::string>()});
                        } else if (arr[i].is_object()
                                && arr[i].contains("path")
                                && arr[i].at("path").is_string()) {
                            data.machoImage.loadDylibs.push_back(
                                MachODylibRef{arr[i].at("path")
                                                  .get<std::string>()});
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("/image/loadDylibs/{}", i),
                                      "each loadDylibs entry must be a "
                                      "string or an object with 'path'");
                        }
                    }
                }
            }
            // `bindNow`: eager vs lazy dynamic-binding choice on
            // Mach-O — parallel to `elf.bindNow`. Optional; defaults
            // to `true` (v1 stance, plan 14 §5 risk row). `false`
            // is the lazy-binding upgrade path anchored at D-LK6-13.
            if (im.contains("bindNow")) {
                if (!im.at("bindNow").is_boolean()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/image/bindNow",
                              "'bindNow' must be a boolean (true = "
                              "eager / bind_off opcode stream, false "
                              "= lazy / lazy_bind_off — anchored at "
                              "D-LK6-13, not yet implemented)");
                } else {
                    data.machoImage.bindNow =
                        im.at("bindNow").get<bool>();
                }
            }
            // Plan 14 LK7 — Apple codesign placeholder reservation.
            // Optional; defaults to 0 (no LC_CODE_SIGNATURE emitted).
            // Multiple-of-8 enforced at validate() (Apple SuperBlob
            // alignment).
            if (im.contains("codeSignatureSize")) {
                if (!im.at("codeSignatureSize").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/image/codeSignatureSize",
                              "'codeSignatureSize' must be a "
                              "non-negative integer (Apple SuperBlob "
                              "reservation size in bytes; plan 16 "
                              "fills the bytes post-link).");
                } else {
                    std::int64_t const v =
                        im.at("codeSignatureSize").get<std::int64_t>();
                    if (v < 0
                     || v > static_cast<std::int64_t>(
                                std::numeric_limits<std::uint32_t>::max())) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/codeSignatureSize",
                                  std::format("'codeSignatureSize' "
                                              "({}) out of u32 range",
                                              v));
                    } else {
                        data.machoImage.codeSignatureSize =
                            static_cast<std::uint32_t>(v);
                    }
                }
            }
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
