#include "core/types/target_schema.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/substrate/relocation_table.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <concepts>
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

[[nodiscard]] std::optional<TargetResultRule> parseResultRule(std::string_view s) noexcept {
    if (s == "none")     return TargetResultRule::None;
    if (s == "value")    return TargetResultRule::Value;
    if (s == "optional") return TargetResultRule::Optional;
    return std::nullopt;
}

// Note: `targetTerminatorKindFromName` lives in `target_schema.hpp`
// alongside the enum, so loader + future emit-side serializers share
// one source of truth for the string mapping.

// Single helper for "read a bounded non-negative integer field". Replaces
// the cycle-2b-first-cut `readByte` + `readU16` duplication — the only
// real differences were the upper bound and the output type, both of
// which the template captures. The path argument lets the caller pass
// the JSON path prefix (`/opcodes/N/`, `/registers/N/`, etc.); the field
// name is appended.
template <std::unsigned_integral T>
void readBoundedInt(json const& obj, Collector& coll,
                    std::string_view pathPrefix,
                    char const* field, T& out) {
    if (!obj.contains(field)) return;
    auto const path = std::format("{}/{}", pathPrefix, field);
    if (!obj.at(field).is_number_integer()) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  "must be a non-negative integer");
        return;
    }
    std::int64_t const v = obj.at(field).get<std::int64_t>();
    constexpr std::int64_t kMax =
        static_cast<std::int64_t>(std::numeric_limits<T>::max());
    if (v < 0 || v > kMax) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  std::format("must fit in [0, {}]", kMax));
        return;
    }
    out = static_cast<T>(v);
}

// ── Encoding-variant sub-parsers (plan 13 AS2) ────────────────────
//
// One free function per sub-tree of the `variants[]` JSON shape.
// Mirrors the file's `readStringArray` pattern (used by the
// calling-conventions block above) — each helper takes the parent
// `coll` + the JSON path prefix and populates a typed sub-field.
// The opcode-level driver (`parseEncodingVariants`) just walks the
// array and calls these. Replaces the inline 175-line nested block
// flagged by simplifier review.

void parseVariantGuard(json const& v, std::size_t opIdx, std::size_t vi,
                       TargetEncodingVariant& variant, Collector& coll) {
    if (!v.contains("guard")) return;
    auto const& g = v.at("guard");
    if (!g.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson,
                  std::format("/opcodes/{}/encoding/variants/{}/guard", opIdx, vi),
                  "'guard' must be an object");
        return;
    }
    if (!g.contains("operandKinds")) return;
    auto const& oks = g.at("operandKinds");
    if (!oks.is_array()) {
        coll.emit(DiagnosticCode::C_MalformedJson,
                  std::format("/opcodes/{}/encoding/variants/{}/guard/operandKinds", opIdx, vi),
                  "'operandKinds' must be an array of strings");
        return;
    }
    for (std::size_t ki = 0; ki < oks.size(); ++ki) {
        if (!oks[ki].is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/guard/operandKinds/{}", opIdx, vi, ki),
                      "every operandKinds entry must be a string");
            continue;
        }
        auto const k = operandKindFilterFromName(oks[ki].get<std::string>());
        if (!k.has_value()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/guard/operandKinds/{}", opIdx, vi, ki),
                      "expected 'reg' / 'imm32' / 'symbol'");
            continue;
        }
        variant.operandKinds.push_back(*k);
    }
}

void parseVariantTemplate(json const& v, std::size_t opIdx, std::size_t vi,
                          TargetEncodingTemplate& tmpl, Collector& coll) {
    if (!v.contains("template")) return;
    auto const& t = v.at("template");
    if (!t.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson,
                  std::format("/opcodes/{}/encoding/variants/{}/template", opIdx, vi),
                  "'template' must be an object");
        return;
    }
    if (t.contains("rexW") && t.at("rexW").is_boolean()) {
        tmpl.rexW = t.at("rexW").get<bool>();
    }
    if (t.contains("opcode")) {
        auto const& ob = t.at("opcode");
        if (!ob.is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/template/opcode", opIdx, vi),
                      "'opcode' must be an array of byte integers");
        } else {
            for (auto const& bn : ob) {
                if (!bn.is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/encoding/variants/{}/template/opcode", opIdx, vi),
                              "every opcode entry must be an integer in [0, 255]");
                    continue;
                }
                std::int64_t const bv = bn.get<std::int64_t>();
                if (bv < 0 || bv > 255) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/encoding/variants/{}/template/opcode", opIdx, vi),
                              std::format("opcode byte {} out of range [0, 255]", bv));
                    continue;
                }
                tmpl.opcodeBytes.push_back(static_cast<std::uint8_t>(bv));
            }
        }
    }
    if (t.contains("modrmRegExt")) {
        auto const path = std::format("/opcodes/{}/encoding/variants/{}/template/modrmRegExt", opIdx, vi);
        if (!t.at("modrmRegExt").is_number_integer()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'modrmRegExt' must be an integer in [0, 7]");
        } else {
            std::int64_t const mv = t.at("modrmRegExt").get<std::int64_t>();
            if (mv < 0 || mv > 7) {
                coll.emit(DiagnosticCode::C_MalformedJson, path,
                          std::format("'modrmRegExt' ({}) must be in [0, 7]", mv));
            } else {
                tmpl.modrmRegExt = static_cast<std::uint8_t>(mv);
            }
        }
    }
    // `fixedWord` (plan 13 AS3 — `fixed32` shape) — 32-bit base bit
    // pattern. JSON accepts unsigned 32-bit integer values.
    if (t.contains("fixedWord")) {
        auto const path = std::format("/opcodes/{}/encoding/variants/{}/template/fixedWord", opIdx, vi);
        if (!t.at("fixedWord").is_number_integer()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'fixedWord' must be a 32-bit unsigned integer");
        } else {
            std::int64_t const wv = t.at("fixedWord").get<std::int64_t>();
            if (wv < 0 || wv > 0xFFFFFFFFLL) {
                coll.emit(DiagnosticCode::C_MalformedJson, path,
                          std::format("'fixedWord' ({}) must fit in 32 bits", wv));
            } else {
                tmpl.fixedWord = static_cast<std::uint32_t>(wv);
            }
        }
    }
}

void parseVariantResultSlot(json const& v, std::size_t opIdx, std::size_t vi,
                            TargetEncodingVariant& variant, Collector& coll) {
    if (!v.contains("resultSlot")) return;
    auto const path = std::format("/opcodes/{}/encoding/variants/{}/resultSlot", opIdx, vi);
    if (!v.at("resultSlot").is_string()) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  "'resultSlot' must be a slot-kind string");
        return;
    }
    auto const r = encodingSlotKindFromName(v.at("resultSlot").get<std::string>());
    if (!r.has_value()) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  "expected one of: 'modrm.reg' / 'modrm.rm' / "
                  "'imm32' (x86-variable) or 'rd' / 'rn' / 'rm' "
                  "(fixed32) or 'disp32' (x86) / 'imm26' (fixed32, "
                  "symbol-bearing)");
        return;
    }
    variant.resultSlot = *r;
}

void parseVariantWires(json const& v, std::size_t opIdx, std::size_t vi,
                       TargetEncodingVariant& variant,
                       detail::TargetSchemaData const& data,
                       Collector& coll) {
    if (!v.contains("wires")) return;
    auto const& ops = v.at("wires");
    if (!ops.is_array()) {
        coll.emit(DiagnosticCode::C_MalformedJson,
                  std::format("/opcodes/{}/encoding/variants/{}/wires", opIdx, vi),
                  "'wires' must be an array");
        return;
    }
    for (std::size_t oi = 0; oi < ops.size(); ++oi) {
        auto const& o2 = ops[oi];
        auto const wirePath = std::format("/opcodes/{}/encoding/variants/{}/wires/{}", opIdx, vi, oi);
        if (!o2.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, wirePath,
                      "wire entry must be an object");
            continue;
        }
        TargetEncodingWire wire;
        if (!o2.contains("index") || !o2.at("index").is_number_integer()) {
            coll.emit(DiagnosticCode::C_MissingField,
                      std::format("{}/index", wirePath),
                      "missing or non-integer 'index'");
            continue;
        }
        std::int64_t const iv = o2.at("index").get<std::int64_t>();
        if (iv < 0 || iv > 255) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("{}/index", wirePath),
                      std::format("'index' ({}) must fit in [0, 255]", iv));
            continue;
        }
        wire.index = static_cast<std::uint8_t>(iv);
        if (!o2.contains("slotKind") || !o2.at("slotKind").is_string()) {
            coll.emit(DiagnosticCode::C_MissingField,
                      std::format("{}/slotKind", wirePath),
                      "missing or non-string 'slotKind'");
            continue;
        }
        auto const sk = encodingSlotKindFromName(o2.at("slotKind").get<std::string>());
        if (!sk.has_value()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("{}/slotKind", wirePath),
                      "expected one of: 'modrm.reg' / 'modrm.rm' / "
                  "'imm32' (x86-variable) or 'rd' / 'rn' / 'rm' "
                  "(fixed32) or 'disp32' (x86) / 'imm26' (fixed32, "
                  "symbol-bearing)");
            continue;
        }
        wire.slotKind = *sk;
        // `relocationKind` (plan 13 AS4) — name string resolved
        // against the schema's `relocations[]` rows. The loader
        // processes `relocations[]` BEFORE the opcode block, so the
        // resolution is inline. `relocationNameIndex` is keyed by
        // name; the resolved value is the row's opaque kind tag.
        if (o2.contains("relocationKind")) {
            if (!o2.at("relocationKind").is_string()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("{}/relocationKind", wirePath),
                          "'relocationKind' must be a string naming a "
                          "row in the schema's `relocations[]`");
            } else {
                auto const name = o2.at("relocationKind").get<std::string>();
                auto const it = data.relocationNameIndex.find(name);
                if (it == data.relocationNameIndex.end()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/relocationKind", wirePath),
                              std::format("'relocationKind' = '{}' does not "
                                          "resolve to any row in the "
                                          "schema's `relocations[]` "
                                          "(declare it there first)",
                                          name));
                } else {
                    wire.relocationKind = data.relocations[it->second].kind;
                }
            }
        }
        variant.wires.push_back(wire);
    }
}

void parseEncodingVariants(json const& vs,
                           std::vector<TargetEncodingVariant>& out,
                           std::size_t opIdx,
                           detail::TargetSchemaData const& data,
                           Collector& coll) {
    if (!vs.is_array()) {
        coll.emit(DiagnosticCode::C_MalformedJson,
                  std::format("/opcodes/{}/encoding/variants", opIdx),
                  "'variants' must be an array");
        return;
    }
    out.reserve(vs.size());
    for (std::size_t vi = 0; vi < vs.size(); ++vi) {
        auto const& v = vs[vi];
        if (!v.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}", opIdx, vi),
                      "variant entry must be an object");
            continue;
        }
        TargetEncodingVariant variant;
        parseVariantGuard      (v, opIdx, vi, variant, coll);
        parseVariantTemplate   (v, opIdx, vi, variant.tmpl, coll);
        parseVariantResultSlot (v, opIdx, vi, variant, coll);
        parseVariantWires      (v, opIdx, vi, variant, data, coll);
        out.push_back(std::move(variant));
    }
}

} // namespace

LoadResult<std::shared_ptr<TargetSchema>> TargetSchema::loadFromText(
    std::string_view jsonText, std::string_view sourceLabel) {
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

    // ── dssTargetVersion ──
    if (!doc.contains("dssTargetVersion")
     || !doc.at("dssTargetVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::C_VersionMismatch,
                  std::string{sourceLabel},
                  "missing or non-integer 'dssTargetVersion'");
        return std::unexpected(std::move(coll).release());
    }
    int const ver = doc.at("dssTargetVersion").get<int>();
    if (ver != 1) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssTargetVersion",
                  std::format("only version 1 supported (got {})", ver));
        return std::unexpected(std::move(coll).release());
    }

    detail::TargetSchemaData data;
    data.id = substrate::mintMonotonicId<TargetSchemaId>();

    // ── target.name + target.version ──
    if (!doc.contains("target") || !doc.at("target").is_object()) {
        coll.emit(DiagnosticCode::C_MissingField, std::string{sourceLabel},
                  "missing 'target' object");
        return std::unexpected(std::move(coll).release());
    }
    auto const& target = doc.at("target");
    if (!target.contains("name") || !target.at("name").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/target/name",
                  "missing or non-string 'name'");
        return std::unexpected(std::move(coll).release());
    }
    data.name = target.at("name").get<std::string>();
    // Empty OR whitespace-only `name` would be silently accepted by
    // the closed-enum cross-validation at the driver tier
    // (`lookupTargetArch` does exact comparison → no match → skip),
    // reopening the SIGILL surface D-LK6-8.2 was anchored to close.
    // Also reject leading/trailing whitespace ("  arm64 " ≠ "arm64").
    // (silent-failure CRITICAL-2 + HIGH-1 post-fold — D-LK6-8.2 audit
    // rounds 1 and 2 — empty was caught in round 1, whitespace in
    // round 2.)
    auto const isNonAsciiWhitespace = [](char c) noexcept {
        // ASCII whitespace per POSIX [[:space:]]:
        // space, tab, newline, CR, vertical tab, form feed.
        return c == ' ' || c == '\t' || c == '\n' || c == '\r'
            || c == '\v' || c == '\f';
    };
    auto const allWhitespace = [&](std::string_view s) noexcept {
        for (char c : s) {
            if (!isNonAsciiWhitespace(c)) return false;
        }
        return true;
    };
    auto const hasLeadingTrailingWS = [&](std::string_view s) noexcept {
        return !s.empty()
            && (isNonAsciiWhitespace(s.front())
             || isNonAsciiWhitespace(s.back()));
    };
    if (data.name.empty() || allWhitespace(data.name)
        || hasLeadingTrailingWS(data.name)) {
        coll.emit(DiagnosticCode::C_MissingField, "/target/name",
                  "'name' must be a non-empty string with no leading "
                  "or trailing whitespace — would silently bypass the "
                  "(target, format) machine cross-check (plan 14 §3.1 "
                  "D-LK6-8.2).");
        return std::unexpected(std::move(coll).release());
    }
    if (target.contains("version") && target.at("version").is_string()) {
        data.version = target.at("version").get<std::string>();
    }
    if (target.contains("abiModel") && target.at("abiModel").is_string()) {
        auto const m = targetAbiModelFromName(target.at("abiModel").get<std::string>());
        if (m.has_value()) {
            data.abiModel = *m;
        } else {
            coll.emit(DiagnosticCode::C_MalformedJson, "/target/abiModel",
                      "expected 'register-machine' / 'operand-stack' / 'result-id'");
            return std::unexpected(std::move(coll).release());
        }
    }
    // Optional frame-op mnemonic overrides (default "frame_load" /
    // "frame_store" on TargetSchemaData). A target may rename the
    // pseudo-ops without breaking the rewrite/verifier substrate.
    if (target.contains("frameLoadMnemonic")
        && target.at("frameLoadMnemonic").is_string()) {
        data.frameLoadMnemonic = target.at("frameLoadMnemonic").get<std::string>();
    }
    if (target.contains("frameStoreMnemonic")
        && target.at("frameStoreMnemonic").is_string()) {
        data.frameStoreMnemonic = target.at("frameStoreMnemonic").get<std::string>();
    }

    // ── relocations (AS1 §2.6 — optional) ─────────────────────────
    // Loaded BEFORE opcodes so the per-wire `relocationKind` name
    // lookup at opcode-parse time can resolve against the populated
    // `relocations[]` table. Empty/absent section is legal; non-
    // empty rows must satisfy the validate() contract: unique non-
    // zero `kind`, non-empty `name`.
    //
    // Target-side extension fields:
    //   * `formula` (string, REQUIRED for non-trivial kinds) — closed-
    //     enum discriminator of the relocation-formula class. Accepted
    //     values: "linear" (default if absent — x86/ARM abs64/rel32
    //     style), "aarch64_call26", "aarch64_adr_prel_pg_hi21",
    //     "aarch64_add_abs_lo12". Load-bearing: dispatches the kernel
    //     at `applyExecRelocations`. (D-LK6-1 closure — was previously
    //     accepted-and-discarded as human documentation.)
    //   * `pcRelative` (bool), `addendBias` (i32), `widthBytes`
    //     (u8 = 4 or 8) — Linear-only fields; ignored for non-Linear
    //     formula kinds (the variant fully encodes the formula).
    //
    // Coherence rules enforced here:
    //   - non-Linear ⇒ widthBytes must be 4 OR absent (defaulted to 4)
    //   - non-Linear ⇒ pcRelative MUST be absent or false
    //   - non-Linear ⇒ addendBias MUST be absent or zero
    substrate::loadRelocationsTable<TargetRelocationInfo>(
        doc, data.relocations, data.relocationNameIndex,
        data.relocationKindIndex, coll,
        [](nlohmann::json const& r, TargetRelocationInfo& info,
           Collector& c, std::size_t i) -> bool {
            if (r.contains("formula")) {
                if (!r.at("formula").is_string()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/formula", i),
                           std::format("'formula' must be a string "
                                       "discriminator (accepted: {})",
                                       acceptedRelocFormulaList()));
                    return false;
                }
                auto const formulaStr = r.at("formula").get<std::string>();
                auto const parsed = parseRelocFormulaKind(formulaStr);
                if (!parsed.has_value()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/formula", i),
                           std::format("'{}' is not a recognized "
                                       "relocation-formula discriminator "
                                       "(accepted: {}) — see plan 14 "
                                       "§3.1 D-LK6-1",
                                       formulaStr,
                                       acceptedRelocFormulaList()));
                    return false;
                }
                info.formulaKind = *parsed;
            }
            // `widthBytes` absent + Linear ⇒ walker fails loud at apply
            // time (anchored D-LK6-1, retained for legacy declarations).
            // Non-Linear formulas implicitly use 4-byte ARM64 instruction
            // words; widthBytes is auto-set to 4 below if not declared.
            if (r.contains("widthBytes")) {
                if (!r.at("widthBytes").is_number_integer()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/widthBytes", i),
                           "'widthBytes' must be an integer (4 or 8)");
                    return false;
                }
                std::int64_t const wb = r.at("widthBytes").get<std::int64_t>();
                if (wb != 4 && wb != 8) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/widthBytes", i),
                           std::format("'widthBytes' must be 4 or 8; "
                                       "got {}", wb));
                    return false;
                }
                info.widthBytes = static_cast<std::uint8_t>(wb);
            }
            if (r.contains("pcRelative")) {
                if (!r.at("pcRelative").is_boolean()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/pcRelative", i),
                           "'pcRelative' must be a boolean");
                    return false;
                }
                info.pcRelative = r.at("pcRelative").get<bool>();
            }
            if (r.contains("addendBias")) {
                if (!r.at("addendBias").is_number_integer()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/addendBias", i),
                           "'addendBias' must be an integer");
                    return false;
                }
                std::int64_t const ab = r.at("addendBias").get<std::int64_t>();
                if (ab < std::numeric_limits<std::int32_t>::min()
                 || ab > std::numeric_limits<std::int32_t>::max()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/addendBias", i),
                           std::format("'addendBias' ({}) out of "
                                       "i32 range", ab));
                    return false;
                }
                info.addendBias = static_cast<std::int32_t>(ab);
            }
            // Non-Linear coherence + default widthBytes=4 (ARM64
            // instruction word).
            if (info.formulaKind != RelocFormulaKind::Linear) {
                if (info.widthBytes == 0) info.widthBytes = 4;
                if (info.widthBytes != 4) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/widthBytes", i),
                           std::format("non-Linear formula '{}' must use "
                                       "widthBytes=4 (ARM64 instruction "
                                       "word); got {}",
                                       relocFormulaName(info.formulaKind),
                                       info.widthBytes));
                    return false;
                }
                if (info.pcRelative) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/pcRelative", i),
                           std::format("non-Linear formula '{}' encodes "
                                       "PC-relativity intrinsically; "
                                       "'pcRelative' must be absent or "
                                       "false",
                                       relocFormulaName(info.formulaKind)));
                    return false;
                }
                if (info.addendBias != 0) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/addendBias", i),
                           std::format("non-Linear formula '{}' encodes "
                                       "any addend bias intrinsically; "
                                       "'addendBias' must be absent or 0",
                                       relocFormulaName(info.formulaKind)));
                    return false;
                }
            }
            return true;
        });

    // ── opcodes ──
    if (!doc.contains("opcodes") || !doc.at("opcodes").is_array()) {
        coll.emit(DiagnosticCode::C_MissingField, "/opcodes",
                  "missing 'opcodes' array");
        return std::unexpected(std::move(coll).release());
    }
    auto const& ops = doc.at("opcodes");
    if (ops.empty()) {
        coll.emit(DiagnosticCode::C_MissingField, "/opcodes",
                  "opcodes array must be non-empty (first entry is the "
                  "Invalid sentinel)");
        return std::unexpected(std::move(coll).release());
    }
    data.opcodes.reserve(ops.size());
    for (std::size_t i = 0; i < ops.size(); ++i) {
        auto const& o = ops[i];
        if (!o.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}", i),
                      "opcode entry must be an object");
            continue;
        }
        TargetOpcodeInfo info;
        // mnemonic (required)
        if (!o.contains("mnemonic") || !o.at("mnemonic").is_string()) {
            coll.emit(DiagnosticCode::C_MissingField,
                      std::format("/opcodes/{}/mnemonic", i),
                      "missing or non-string 'mnemonic'");
            continue;
        }
        info.mnemonic = o.at("mnemonic").get<std::string>();
        // result (required)
        if (!o.contains("result") || !o.at("result").is_string()) {
            coll.emit(DiagnosticCode::C_MissingField,
                      std::format("/opcodes/{}/result", i),
                      "missing or non-string 'result'");
            continue;
        }
        auto const rr = parseResultRule(o.at("result").get<std::string>());
        if (!rr.has_value()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/result", i),
                      "expected 'none' / 'value' / 'optional'");
            continue;
        }
        info.result = *rr;
        // Booleans (optional, default false). Note: `isTerminator` is
        // NOT a JSON field — it derives from `terminatorKind != none`.
        // Earlier draft accepted both fields; the redundancy invited
        // silent disagreement bugs. Single source of truth wins.
        if (o.contains("hasSideEffects") && o.at("hasSideEffects").is_boolean()) {
            info.hasSideEffects = o.at("hasSideEffects").get<bool>();
        }
        if (o.contains("requires2Address") && o.at("requires2Address").is_boolean()) {
            info.requires2Address = o.at("requires2Address").get<bool>();
        }
        if (o.contains("isCall") && o.at("isCall").is_boolean()) {
            info.isCall = o.at("isCall").get<bool>();
        }
        // terminatorKind (optional — default is `None`, i.e. not a
        // terminator). MUST be a string when present; silent type
        // mismatches would let a `terminatorKind: 4` (integer) slip
        // through as the default and silently mis-classify the opcode.
        if (o.contains("terminatorKind")) {
            auto const& tkNode = o.at("terminatorKind");
            if (!tkNode.is_string()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/terminatorKind", i),
                          "'terminatorKind' must be a string (one of "
                          "'none' / 'br' / 'cond-br' / 'switch' / "
                          "'return' / 'unreachable')");
            } else {
                auto const tk = targetTerminatorKindFromName(tkNode.get<std::string>());
                if (tk.has_value()) {
                    info.terminatorKind = *tk;
                } else {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/terminatorKind", i),
                              "expected 'none' / 'br' / 'cond-br' / "
                              "'switch' / 'return' / 'unreachable'");
                }
            }
        }
        // Arity bounds (optional, default 0). Out-of-range values are
        // diagnosed (the schema is rejected by the final fatal-scan);
        // absent fields stay at the zero default. `validate()` enforces
        // cross-field invariants (min<=max, terminator-implies-successors).
        std::string const opcPath = std::format("/opcodes/{}", i);
        readBoundedInt(o, coll, opcPath, "minOperands",   info.minOperands);
        readBoundedInt(o, coll, opcPath, "maxOperands",   info.maxOperands);
        readBoundedInt(o, coll, opcPath, "minSuccessors", info.minSuccessors);
        readBoundedInt(o, coll, opcPath, "maxSuccessors", info.maxSuccessors);
        // Encoding facet (plan 13 AS1 §2.5). Cycle 1 substrate carries
        // only the shape discriminator — the variants/template
        // sub-structure lands in AS2 (`x86-variable`) and AS3
        // (`fixed32`), preserving the "no fields without consumers"
        // discipline. An absent `encoding` block leaves the opcode at
        // `TargetEncodingShape::None` and AS1's `assemble()` flags
        // each affected instruction with `A_NoEncodingDeclared`.
        //
        // When the `encoding` block IS present, `format` is REQUIRED.
        // Without this gate a typo like `"encoding": { "format2":
        // "x86-variable" }` (or an empty `encoding: {}`) would
        // silently leave the opcode at `None`, and the schema author's
        // intent to declare an encoding would be silently dropped.
        if (o.contains("encoding")) {
            auto const& enc = o.at("encoding");
            if (!enc.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/encoding", i),
                          "'encoding' must be an object");
            } else if (!enc.contains("format")) {
                coll.emit(DiagnosticCode::C_MissingField,
                          std::format("/opcodes/{}/encoding/format", i),
                          "missing 'format' (required when an 'encoding' "
                          "block is present; one of 'none' / 'x86-variable' "
                          "/ 'fixed32')");
            } else {
                auto const& fmt = enc.at("format");
                if (!fmt.is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/encoding/format", i),
                              "'format' must be a string (one of "
                              "'none' / 'x86-variable' / 'fixed32')");
                } else {
                    auto const shape =
                        targetEncodingShapeFromName(fmt.get<std::string>());
                    if (shape.has_value()) {
                        info.encoding.shape = *shape;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/opcodes/{}/encoding/format", i),
                                  "expected 'none' / 'x86-variable' / 'fixed32'");
                    }
                }
                // AS2: parse the per-variant rows when present. Walker
                // consumes via the schema accessor; validate() pins
                // cross-field invariants (opcode bytes non-empty,
                // modrmRegExt in [0,7], operand-wire index in range).
                if (enc.contains("variants")) {
                    parseEncodingVariants(enc.at("variants"), info.encoding.variants,
                                          i, data, coll);
                }
            }
        }
        // Slot-0 Invalid-sentinel sanity check.
        if (i == 0 && info.mnemonic != "invalid") {
            coll.emit(DiagnosticCode::C_MalformedJson, "/opcodes/0",
                      "first opcode must be the 'invalid' sentinel "
                      "(its index is the default-constructed inst's "
                      "opcode field; substrate `addInst` rejects opcode 0)");
        }
        std::uint16_t const idx = static_cast<std::uint16_t>(data.opcodes.size());
        bool const fresh = data.mnemonicIndex.emplace(info.mnemonic, idx).second;
        if (!fresh) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/mnemonic", i),
                      std::format("duplicate mnemonic '{}'", info.mnemonic));
            continue;  // skip push_back so vector & index stay in sync
        }
        data.opcodes.push_back(std::move(info));
    }

    // ── registers (cycle 2b — optional) ───────────────────────────
    // Targets that haven't been promoted to a cycle-2b shape can omit
    // `registers` entirely; ML6 regalloc rejects them at consumer time.
    if (doc.contains("registers")) {
        if (!doc.at("registers").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/registers",
                      "'registers' must be an array");
        } else {
            auto const& regs = doc.at("registers");
            data.registers.reserve(regs.size());
            for (std::size_t i = 0; i < regs.size(); ++i) {
                auto const& r = regs[i];
                if (!r.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/registers/{}", i),
                              "register entry must be an object");
                    continue;
                }
                TargetRegisterInfo info;
                if (!r.contains("name") || !r.at("name").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/registers/{}/name", i),
                              "missing or non-string 'name'");
                    continue;
                }
                info.name = r.at("name").get<std::string>();
                if (r.contains("class") && r.at("class").is_string()) {
                    auto const cls = targetRegClassFromName(r.at("class").get<std::string>());
                    if (cls.has_value()) {
                        info.regClass = *cls;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/registers/{}/class", i),
                                  "expected 'gpr' / 'fpr' / 'vr' / 'flags' / 'none'");
                        continue;
                    }
                }
                if (r.contains("subOf") && r.at("subOf").is_string()) {
                    info.subOf = r.at("subOf").get<std::string>();
                }
                std::string const regPath = std::format("/registers/{}", i);
                readBoundedInt(r, coll, regPath, "widthBytes", info.widthBytes);
                readBoundedInt(r, coll, regPath, "hwEncoding", info.hwEncoding);

                std::uint16_t const ordinal =
                    static_cast<std::uint16_t>(data.registers.size());
                bool const fresh = data.registerIndex.emplace(info.name, ordinal).second;
                if (!fresh) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/registers/{}/name", i),
                              std::format("duplicate register name '{}'", info.name));
                    continue;  // skip push_back so vector & index stay in sync
                }
                data.registers.push_back(std::move(info));
            }
        }
    }

    // ── callingConventions (cycle 2b — optional) ───────────────────
    if (doc.contains("callingConventions")) {
        if (!doc.at("callingConventions").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/callingConventions",
                      "'callingConventions' must be an array");
        } else {
            auto const& ccs = doc.at("callingConventions");
            data.callingConventions.reserve(ccs.size());
            auto readStringArray = [&](json const& root,
                                       std::size_t  ci,
                                       char const*  field,
                                       std::vector<std::string>& out) {
                if (!root.contains(field)) return;
                if (!root.at(field).is_array()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/callingConventions/{}/{}", ci, field),
                              "must be an array of strings");
                    return;
                }
                for (auto const& s : root.at(field)) {
                    if (!s.is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/callingConventions/{}/{}", ci, field),
                                  "every entry must be a string");
                        continue;
                    }
                    out.push_back(s.get<std::string>());
                }
            };
            for (std::size_t i = 0; i < ccs.size(); ++i) {
                auto const& c = ccs[i];
                if (!c.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/callingConventions/{}", i),
                              "calling-convention entry must be an object");
                    continue;
                }
                TargetCallingConvention cc;
                if (!c.contains("name") || !c.at("name").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/callingConventions/{}/name", i),
                              "missing or non-string 'name'");
                    continue;
                }
                cc.name = c.at("name").get<std::string>();
                readStringArray(c, i, "argGprs",     cc.argGprs);
                readStringArray(c, i, "argFprs",     cc.argFprs);
                readStringArray(c, i, "returnGprs",  cc.returnGprs);
                readStringArray(c, i, "returnFprs",  cc.returnFprs);
                readStringArray(c, i, "callerSaved", cc.callerSaved);
                readStringArray(c, i, "calleeSaved", cc.calleeSaved);
                std::string const ccPath = std::format("/callingConventions/{}", i);
                readBoundedInt(c, coll, ccPath, "stackAlignment",   cc.stackAlignment);
                readBoundedInt(c, coll, ccPath, "shadowSpaceBytes", cc.shadowSpaceBytes);
                readBoundedInt(c, coll, ccPath, "redZoneBytes",     cc.redZoneBytes);
                // D-LK10-ENTRY-TRAMP-PROLOGUE: process-entry RSP bias
                // when this cc is the entry cc. See target_schema.hpp
                // for the per-cc concrete values. Validated below
                // (must be < stackAlignment when set).
                readBoundedInt(c, coll, ccPath, "entryStackPointerBias",
                               cc.entryStackPointerBias);
                // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY: ISA-level
                // call-instruction RSP-push width. x86_64 = 8 (CALL
                // pushes 8-byte return address); ARM64 = 0 (BL writes
                // LR, no push). Validated below: must be strictly <
                // `stackAlignment` (the bias is an OFFSET into the
                // alignment quantum, parallel to `entryStackPointerBias`'s
                // contract).
                readBoundedInt(c, coll, ccPath, "callPushBytes",
                               cc.callPushBytes);
                if (c.contains("linkRegister")) {
                    if (!c.at("linkRegister").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/linkRegister", ccPath),
                                  "must be a register-name string");
                    } else {
                        auto const name = c.at("linkRegister").get<std::string>();
                        // Atomic population: only engage the optional when
                        // the name resolves. Validate() handles the
                        // unresolved case fail-loud. The struct shape
                        // prevents a "name set, ordinal unset" state.
                        auto it = data.registerIndex.find(name);
                        if (it != data.registerIndex.end()) {
                            cc.linkRegister = TargetCallingConvention::NamedRegisterRef{
                                name, it->second
                            };
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("{}/linkRegister", ccPath),
                                      std::format("link register '{}' is not "
                                                  "in the register table", name));
                        }
                    }
                }
                if (c.contains("stackPointer")) {
                    if (!c.at("stackPointer").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/stackPointer", ccPath),
                                  "must be a register-name string");
                    } else {
                        auto const name = c.at("stackPointer").get<std::string>();
                        auto it = data.registerIndex.find(name);
                        if (it != data.registerIndex.end()) {
                            cc.stackPointer = TargetCallingConvention::NamedRegisterRef{
                                name, it->second
                            };
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("{}/stackPointer", ccPath),
                                      std::format("stack pointer '{}' is not "
                                                  "in the register table", name));
                        }
                    }
                }

                std::uint16_t const idx =
                    static_cast<std::uint16_t>(data.callingConventions.size());
                bool const fresh = data.callingConventionIndex.emplace(cc.name, idx).second;
                if (!fresh) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/callingConventions/{}/name", i),
                              std::format("duplicate calling-convention name '{}'", cc.name));
                    continue;  // skip push_back so vector & index stay in sync
                }
                data.callingConventions.push_back(std::move(cc));
            }
        }
    }

    // ── Cross-field invariants (validate after per-field parse) ───
    // `validate()` returns pre-shaped `ConfigDiagnostic`s carrying their
    // specific JSON paths (`/opcodes/3/maxSuccessors`, etc.). Append-as-
    // is instead of reshaping under a single sourceLabel path — the path
    // is the load-bearing locator for the user fixing the config.
    for (auto&& problem : data.validate()) {
        coll.emitRaw(std::move(problem));
    }

    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll).release());
    }

    return std::make_shared<TargetSchema>(std::move(data));
}

} // namespace dss
