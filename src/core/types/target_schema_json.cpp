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
#include <set>
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
    // FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS) + D-CSUBSET-CHAR-STRING-VALUE-CODEGEN:
    // optional `width` key — the operation-width discriminator. Absent = the
    // variant matches an instruction of ANY width (every pre-FC3 variant).
    // 8 (the byte forms: movsx/movzx r/m8, mov r8, sxtb, ldrb/strb), 16 (the
    // half-word memory forms: 0x66 mov / movzx r16, STURH/LDURH —
    // D-LIR-INT-MEMORY-WIDTH-EXACT), 32, and 64 are encodable; any other value
    // is a load-time reject, never a silent match-nothing variant.
    if (g.contains("width")) {
        auto const& w = g.at("width");
        if (!w.is_number_integer()
            || (w.get<std::int64_t>() != 8 && w.get<std::int64_t>() != 16
                && w.get<std::int64_t>() != 32 && w.get<std::int64_t>() != 64)) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/guard/width", opIdx, vi),
                      "'width' must be the integer 8, 16, 32, or 64 "
                      "(the shipped operation-width vocabulary; "
                      "D-CSUBSET-32BIT-ALU-FORMS / CHAR-STRING-VALUE-CODEGEN / "
                      "D-LIR-INT-MEMORY-WIDTH-EXACT)");
        } else {
            variant.guardWidthBits =
                static_cast<std::uint8_t>(w.get<std::int64_t>());
        }
    }
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: OPTIONAL `immMin`/`immMax`
    // immediate-magnitude bounds (uint32, [0, 0xFFFFFFFF]). Absent ⇒ the
    // variant matches any immediate magnitude (every pre-existing variant).
    // Present ⇒ the matcher additionally requires the inst's immediate /
    // memOffset magnitude in [immMin, immMax] — what lets one opcode carry
    // both a single-word imm12 variant (immMax:4095) and a 2-word shifted
    // form (immMin:4096). A non-integer or out-of-uint32 value is a load-
    // time reject (never a silently dropped bound). `immMin > immMax` is
    // caught at validate() (an empty range matches nothing — a config bug).
    auto const parseImmBound = [&](char const* key, std::optional<std::uint32_t>& out) {
        if (!g.contains(key)) return;
        auto const& jv = g.at(key);
        auto const path = std::format(
            "/opcodes/{}/encoding/variants/{}/guard/{}", opIdx, vi, key);
        if (!jv.is_number_integer()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      std::format("'{}' must be a non-negative integer", key));
            return;
        }
        std::int64_t const n = jv.get<std::int64_t>();
        if (n < 0 || n > static_cast<std::int64_t>(
                             std::numeric_limits<std::uint32_t>::max())) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      std::format("'{}' ({}) must fit in [0, 4294967295]", key, n));
            return;
        }
        out = static_cast<std::uint32_t>(n);
    };
    parseImmBound("immMin", variant.immMin);
    parseImmBound("immMax", variant.immMax);
    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: OPTIONAL `negMemoffset` (bool).
    // Absent ⇒ false (the non-negative magnitude axis — every pre-existing
    // variant). Present-and-true ⇒ the variant matches only a NEGATIVE
    // memoffset, keyed by |disp| against [immMin, immMax]. A non-boolean is a
    // load-time reject (never a silently dropped flag). validate() rejects
    // negMemoffset on a guard with no `memoffset` operand (nothing to sign-
    // route) — the same coherence family as immMin/immMax.
    if (g.contains("negMemoffset")) {
        auto const& nv = g.at("negMemoffset");
        if (!nv.is_boolean()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/guard/negMemoffset",
                                  opIdx, vi),
                      "'negMemoffset' must be a boolean");
        } else {
            variant.negMemoffset = nv.get<bool>();
        }
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
    // FC2 Part B (SSE float backend): mandatory legacy-prefix bytes
    // (F2/F3/66 — the SSE opcode-form selectors). Same shape +
    // validation rigor as `opcode` above; emitted BEFORE the REX
    // prefix by the x86-variable walker. validate() rejects the
    // field on a fixed32 variant.
    if (t.contains("mandatoryPrefix")) {
        auto const& mp = t.at("mandatoryPrefix");
        if (!mp.is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/template/mandatoryPrefix", opIdx, vi),
                      "'mandatoryPrefix' must be an array of byte integers");
        } else {
            for (auto const& bn : mp) {
                if (!bn.is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/encoding/variants/{}/template/mandatoryPrefix", opIdx, vi),
                              "every mandatoryPrefix entry must be an integer in [0, 255]");
                    continue;
                }
                std::int64_t const bv = bn.get<std::int64_t>();
                if (bv < 0 || bv > 255) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/encoding/variants/{}/template/mandatoryPrefix", opIdx, vi),
                              std::format("mandatoryPrefix byte {} out of range [0, 255]", bv));
                    continue;
                }
                tmpl.mandatoryPrefix.push_back(static_cast<std::uint8_t>(bv));
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
    // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1): cond-code-
    // from-payload flag. When true, the encoder reads the inst's
    // payload as `TargetCondCode`, looks up the schema's
    // `condCodeEncoding[idx]` nibble, and OR's it into the LAST
    // opcode byte before emission. Used by x86 setcc + jcc.
    if (t.contains("condCodeFromPayload")) {
        auto const path = std::format("/opcodes/{}/encoding/variants/{}/template/condCodeFromPayload", opIdx, vi);
        if (!t.at("condCodeFromPayload").is_boolean()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'condCodeFromPayload' must be a boolean");
        } else {
            tmpl.condCodeFromPayload = t.at("condCodeFromPayload").get<bool>();
        }
    }
    // D-AS3-COND-CODE-ARM64: `condBitPos` — LSB inside word 0 where the
    // fixed32 walker OR's the cond nibble. DEFAULT 0 (x86 / B.cond low
    // nibble). Range [0, 28] (a 4-bit nibble must fit inside the 32-bit
    // word). 12 for AArch64 CSET. (The x86-variable walker ignores this;
    // it places the nibble in the opcode byte regardless.)
    if (t.contains("condBitPos")) {
        auto const path = std::format("/opcodes/{}/encoding/variants/{}/template/condBitPos", opIdx, vi);
        if (!t.at("condBitPos").is_number_integer()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'condBitPos' must be an integer in [0, 28]");
        } else {
            std::int64_t const cb = t.at("condBitPos").get<std::int64_t>();
            if (cb < 0 || cb > 28) {
                coll.emit(DiagnosticCode::C_MalformedJson, path,
                          std::format("'condBitPos' ({}) must be in [0, 28] "
                                      "(a 4-bit nibble must fit within the "
                                      "32-bit word)", cb));
            } else {
                tmpl.condBitPos = static_cast<std::uint8_t>(cb);
            }
        }
    }
    // D-AS3-COND-CODE-ARM64: `condInvert` — XOR the cond nibble with 1
    // before placing it (the AArch64 inverse-condition trick used by
    // CSET = CSINC with the inverted condition). DEFAULT false (x86 /
    // B.cond place the cond verbatim).
    if (t.contains("condInvert")) {
        auto const path = std::format("/opcodes/{}/encoding/variants/{}/template/condInvert", opIdx, vi);
        if (!t.at("condInvert").is_boolean()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'condInvert' must be a boolean");
        } else {
            tmpl.condInvert = t.at("condInvert").get<bool>();
        }
    }
    // D-LIR-SETCC-WIDTH-CONTRACT (step 13.5 cycle 1 post-fold): force a
    // REX prefix even when no REX bit is set — required by x86 byte-
    // register-bearing opcodes (setcc) to access the spl/bpl/sil/dil
    // low-byte registers instead of the legacy ah/ch/dh/bh aliases.
    if (t.contains("forceRexPrefix")) {
        auto const path = std::format("/opcodes/{}/encoding/variants/{}/template/forceRexPrefix", opIdx, vi);
        if (!t.at("forceRexPrefix").is_boolean()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'forceRexPrefix' must be a boolean");
        } else {
            tmpl.forceRexPrefix = t.at("forceRexPrefix").get<bool>();
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
    // `fixedWords` (D-AS4-3 — multi-word `fixed32` macro) — an array of
    // 32-bit base words, one per emitted instruction (e.g. AArch64
    // `lea` = [ADRP, ADD]). MUTUALLY EXCLUSIVE with `fixedWord`: a
    // template that declares BOTH is rejected here (the single-word
    // default would otherwise be silently shadowed by the multi-word
    // path — a config typo discriminator). Each element is range-
    // checked [0, 0xFFFFFFFF] exactly like `fixedWord`.
    if (t.contains("fixedWords")) {
        auto const path = std::format("/opcodes/{}/encoding/variants/{}/template/fixedWords", opIdx, vi);
        if (t.contains("fixedWord")) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "a template must not declare BOTH 'fixedWord' and "
                      "'fixedWords' — use 'fixedWord' for a single-word "
                      "opcode or 'fixedWords' for a multi-word macro, "
                      "never both");
        } else if (!t.at("fixedWords").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'fixedWords' must be an array of 32-bit unsigned "
                      "integers (one per emitted word)");
        } else if (t.at("fixedWords").empty()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'fixedWords' must be non-empty (a multi-word macro "
                      "emits at least one word — omit the key entirely for "
                      "the single-word `fixedWord` path)");
        } else {
            for (auto const& wn : t.at("fixedWords")) {
                if (!wn.is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "every 'fixedWords' entry must be a 32-bit "
                              "unsigned integer");
                    continue;
                }
                std::int64_t const wv = wn.get<std::int64_t>();
                if (wv < 0 || wv > 0xFFFFFFFFLL) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              std::format("'fixedWords' entry ({}) must fit "
                                          "in 32 bits", wv));
                    continue;
                }
                tmpl.fixedWords.push_back(static_cast<std::uint32_t>(wv));
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

// D-AS4-3 (multi-instruction-macro encoder): parse `extraResultSlots`
// — additional placements of the SAME result register beyond the
// primary `resultSlot` (word 0). Each entry is { "slotKind": <name>,
// "wordIndex": <int> }. Optional; absent leaves the vector empty
// (every single-placement opcode). validate() requires a `resultSlot`
// to exist when this is non-empty and bounds each wordIndex.
void parseVariantExtraResultSlots(json const& v, std::size_t opIdx,
                                  std::size_t vi,
                                  TargetEncodingVariant& variant,
                                  Collector& coll) {
    if (!v.contains("extraResultSlots")) return;
    auto const path = std::format("/opcodes/{}/encoding/variants/{}/extraResultSlots", opIdx, vi);
    auto const& arr = v.at("extraResultSlots");
    if (!arr.is_array()) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  "'extraResultSlots' must be an array of "
                  "{ slotKind, wordIndex } objects");
        return;
    }
    for (std::size_t ei = 0; ei < arr.size(); ++ei) {
        auto const& e = arr[ei];
        auto const ePath = std::format("{}/{}", path, ei);
        if (!e.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, ePath,
                      "each 'extraResultSlots' entry must be an object");
            continue;
        }
        if (!e.contains("slotKind") || !e.at("slotKind").is_string()) {
            coll.emit(DiagnosticCode::C_MissingField,
                      std::format("{}/slotKind", ePath),
                      "missing or non-string 'slotKind'");
            continue;
        }
        auto const sk = encodingSlotKindFromName(e.at("slotKind").get<std::string>());
        if (!sk.has_value()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("{}/slotKind", ePath),
                      "unknown 'slotKind' for an extra result placement");
            continue;
        }
        ResultSlotExtra extra;
        extra.slotKind = *sk;
        // wordIndex optional, default 0.
        if (e.contains("wordIndex")) {
            auto const wiPath = std::format("{}/wordIndex", ePath);
            if (!e.at("wordIndex").is_number_integer()) {
                coll.emit(DiagnosticCode::C_MalformedJson, wiPath,
                          "'wordIndex' must be an integer in [0, 255]");
                continue;
            }
            std::int64_t const wiv = e.at("wordIndex").get<std::int64_t>();
            if (wiv < 0 || wiv > 255) {
                coll.emit(DiagnosticCode::C_MalformedJson, wiPath,
                          std::format("'wordIndex' ({}) must fit in [0, 255]", wiv));
                continue;
            }
            extra.wordIndex = static_cast<std::uint8_t>(wiv);
        }
        variant.extraResultSlots.push_back(extra);
    }
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
        // `wordIndex` (D-AS4-3 — multi-word `fixed32` macro): which
        // 32-bit word of the template this wire's slot lives in.
        // Optional, default 0 (every single-word wire). Range
        // [0, 255]; validate() further bounds it < template.wordCount().
        if (o2.contains("wordIndex")) {
            auto const wiPath = std::format("{}/wordIndex", wirePath);
            if (!o2.at("wordIndex").is_number_integer()) {
                coll.emit(DiagnosticCode::C_MalformedJson, wiPath,
                          "'wordIndex' must be an integer in [0, 255]");
            } else {
                std::int64_t const wiv = o2.at("wordIndex").get<std::int64_t>();
                if (wiv < 0 || wiv > 255) {
                    coll.emit(DiagnosticCode::C_MalformedJson, wiPath,
                              std::format("'wordIndex' ({}) must fit in "
                                          "[0, 255]", wiv));
                } else {
                    wire.wordIndex = static_cast<std::uint8_t>(wiv);
                }
            }
        }
        // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1):
        // optional `prefixOpcodeBytes` — bytes emitted IMMEDIATELY
        // BEFORE this wire's slot bytes (between the previous
        // wire's emission and this one). Used by jcc's compound
        // `0F 8x rel32; E9 rel32` encoding: wire 0 is the cond
        // branch, wire 1 declares `[0xE9]` to bridge to the
        // trailing uncond jmp's rel32 placeholder.
        if (o2.contains("prefixOpcodeBytes")) {
            auto const& pb = o2.at("prefixOpcodeBytes");
            auto const pbPath = std::format("{}/prefixOpcodeBytes", wirePath);
            if (!pb.is_array()) {
                coll.emit(DiagnosticCode::C_MalformedJson, pbPath,
                          "'prefixOpcodeBytes' must be an array of "
                          "byte integers");
            } else {
                for (auto const& bn : pb) {
                    if (!bn.is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson, pbPath,
                                  "every prefixOpcodeBytes entry must be "
                                  "an integer in [0, 255]");
                        continue;
                    }
                    auto const bv = bn.get<std::int64_t>();
                    if (bv < 0 || bv > 255) {
                        coll.emit(DiagnosticCode::C_MalformedJson, pbPath,
                                  std::format("prefixOpcodeBytes entry {} "
                                              "out of range [0, 255]", bv));
                        continue;
                    }
                    wire.prefixOpcodeBytes.push_back(
                        static_cast<std::uint8_t>(bv));
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
        parseVariantExtraResultSlots(v, opIdx, vi, variant, coll);
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

    // ── condCodeEncoding (D-CSUBSET-WHILE-LOOP-SUBSTRATE step 13.5
    // cycle 1, 2026-06-03 — optional) ───────────────────────────────
    // Per-target mapping from abstract `TargetCondCode` (substrate-tier
    // 10-arm enum: eq/ne/slt/sle/sgt/sge/ult/ule/ugt/uge) to a numeric
    // encoding used by the ISA's conditional opcodes. x86_64 writes
    // the value into the low 4 bits of the setcc/jcc opcode byte;
    // ARM64 writes it into bits 0..3 of the 32-bit B.cc instruction
    // word (same low-nibble position; different numeric mapping per
    // the AArch64 condition table). JSON shape: object with all 10
    // string keys present, each mapping to an integer in [0, 15].
    // Missing keys / extra keys / out-of-range values fail-loud.
    //
    // A target with no cond-code-bearing opcodes (a declarative-only
    // target, future SPIR-V variant) omits the section entirely —
    // `condCodeEncoding()` then returns nullopt and the per-opcode
    // encoder fails loud `A_NoCondCodeEncoding` on any wire that
    // references `CondCodeNibble`. The wire never silently OR's a
    // zero nibble (which would map every condition to `eq`).
    if (doc.contains("condCodeEncoding")) {
        auto const& cc = doc.at("condCodeEncoding");
        if (!cc.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/condCodeEncoding",
                      "must be an object mapping cond-code name → "
                      "integer encoding");
        } else {
            // The full TargetCondCode name set: the 10 INTEGER codes
            // (REQUIRED when the table is declared — pre-FC3.5 rule
            // unchanged) + the 7 FLOAT codes (OPTIONAL per entry —
            // FC3.5 sweep-c2: an undeclared float code is the
            // capability signal that the target realizes that FCmp
            // predicate via the two-setcc composition instead of a
            // single native condition; see mir_to_lir floatCmpPlan).
            constexpr std::array<std::string_view, 17> kCondNames{
                "eq", "ne", "slt", "sle", "sgt", "sge",
                "ult", "ule", "ugt", "uge",
                "fogt", "foge", "foeq", "fone", "fune", "fuo", "ford"};
            constexpr std::size_t kRequiredCount = 10;
            std::array<bool, 17> seen{};
            for (auto it = cc.begin(); it != cc.end(); ++it) {
                auto const& key = it.key();
                std::size_t idx = kCondNames.size();
                for (std::size_t i = 0; i < kCondNames.size(); ++i) {
                    if (kCondNames[i] == key) { idx = i; break; }
                }
                if (idx >= kCondNames.size()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/condCodeEncoding/{}", key),
                              std::format("unknown cond-code key '{}' "
                                          "(expected one of eq/ne/slt/sle/"
                                          "sgt/sge/ult/ule/ugt/uge or the "
                                          "float codes fogt/foge/foeq/"
                                          "fone/fune/fuo/ford)", key));
                    continue;
                }
                if (!it.value().is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/condCodeEncoding/{}", key),
                              "value must be a non-negative integer");
                    continue;
                }
                auto const v = it.value().get<std::int64_t>();
                if (v < 0 || v > 15) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/condCodeEncoding/{}", key),
                              std::format("value {} out of range — must "
                                          "fit in 4 bits [0..15] to OR "
                                          "into the opcode byte's low "
                                          "nibble", v));
                    continue;
                }
                data.condCodeEncoding[idx] = static_cast<std::uint8_t>(v);
                data.condCodeDeclared[idx] = true;
                seen[idx] = true;
            }
            std::vector<std::string_view> missing;
            for (std::size_t i = 0; i < kRequiredCount; ++i) {
                if (!seen[i]) missing.push_back(kCondNames[i]);
            }
            if (!missing.empty()) {
                std::string list;
                for (std::size_t i = 0; i < missing.size(); ++i) {
                    if (i) list += ", ";
                    list += missing[i];
                }
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/condCodeEncoding",
                          std::format("missing cond-code(s) {} — when "
                                      "the table is declared, ALL 10 "
                                      "integer entries must be present "
                                      "so the encoder cannot silently "
                                      "default to 0 for an absent code "
                                      "(the float codes are optional — "
                                      "absence selects the composed "
                                      "FCmp realization)", list));
            } else {
                data.condCodeEncodingLoaded = true;
            }
        }
    }

    // ── aggregateLayout (FC6, D-FF3-1 layout half): the per-ABI struct/union/
    //    array layout params the generic `type_layout` engine reads. REQUIRED on
    //    a register-machine target — a silent default would bake a wrong alignment
    //    rule into every aggregate (mirrors the format's required `dataModel`). ──
    if (doc.contains("aggregateLayout")) {
        auto const& al = doc.at("aggregateLayout");
        if (!al.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/aggregateLayout",
                      "must be an object { scalarAlignment, maxAlignment }");
        } else {
            bool ok = true;
            if (!al.contains("scalarAlignment")
                || !al.at("scalarAlignment").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/aggregateLayout/scalarAlignment",
                          "missing required 'scalarAlignment' string (e.g. \"natural\")");
                ok = false;
            } else {
                auto const name = al.at("scalarAlignment").get<std::string>();
                auto const rule = scalarAlignmentRuleFromName(name);
                if (!rule) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/aggregateLayout/scalarAlignment",
                              std::format("unknown scalarAlignment '{}' "
                                          "(expected \"natural\")", name));
                    ok = false;
                } else {
                    data.aggregateLayout.scalarAlignment = *rule;
                }
            }
            if (!al.contains("maxAlignment")
                || !al.at("maxAlignment").is_number_integer()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/aggregateLayout/maxAlignment",
                          "missing required 'maxAlignment' integer (the ISA's "
                          "largest fundamental alignment, a power of two)");
                ok = false;
            } else {
                auto const v = al.at("maxAlignment").get<std::int64_t>();
                if (v < 1 || v > 256 || (v & (v - 1)) != 0) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/aggregateLayout/maxAlignment",
                              std::format("maxAlignment {} must be a power of two "
                                          "in [1, 256]", v));
                    ok = false;
                } else {
                    data.aggregateLayout.maxAlignment =
                        static_cast<std::uint32_t>(v);
                }
            }
            // FC8 bitfields (D-CSUBSET-BITFIELD): OPTIONAL bit-field packing
            // strategy. Absent → BitFieldStrategy::None (the layout engine then
            // FAILS LOUD only if a struct with a bit-field is laid out — a
            // bitfield-free target is unaffected). A wrong spelling is a hard
            // error (a typo can't silently fall back to a wrong rule).
            if (al.contains("bitFieldStrategy")) {
                if (!al.at("bitFieldStrategy").is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/aggregateLayout/bitFieldStrategy",
                              "must be a string (e.g. \"gnu_packed\")");
                    ok = false;
                } else {
                    auto const name = al.at("bitFieldStrategy").get<std::string>();
                    auto const strat = bitFieldStrategyFromName(name);
                    if (!strat) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/aggregateLayout/bitFieldStrategy",
                                  std::format("unknown bitFieldStrategy '{}' "
                                              "(expected \"gnu_packed\")", name));
                        ok = false;
                    } else {
                        data.aggregateLayout.bitFieldStrategy = *strat;
                    }
                }
            }
            if (ok) data.aggregateLayoutLoaded = true;
        }
    }
    // NOTE: `aggregateLayout` is OPTIONAL at load — consistent with the
    // callingConventions/registers relaxation for minimal schema-substrate targets
    // (validate() allows a target with neither). A target that never compiles a
    // C aggregate needs no layout params. The fail-loud is at the CONSUMER: the
    // layout/sizeof site asserts `aggregateLayoutLoaded()` and emits a loud
    // diagnostic if a target is asked to lay out an aggregate without declaring
    // its params — a silent wrong-layout is thereby still impossible.

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
        // Implicit-register-constraint (cycle 10p substrate,
        // 2026-06-04). Optional per-opcode block. Field-shape rejects
        // here; name→ordinal resolution + cross-field rejects happen
        // post-register-load (see "Implicit-register-constraint
        // resolution + validation" block lower in this function).
        // See `ImplicitRegisterConstraint` docblock in
        // target_schema.hpp for the full contract.
        if (o.contains("implicitRegisters")) {
            auto const& ir = o.at("implicitRegisters");
            if (!ir.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/implicitRegisters", i),
                          "'implicitRegisters' must be an object with "
                          "optional 'inputs', 'outputs', 'clobbered' "
                          "string-array fields");
                // continue past this opcode arm: a malformed block must
                // not leave a partial-state opcode pushed downstream.
                // Mirror the duplicate-mnemonic pattern below.
            } else {
                // Per `D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD`
                // discipline (closed 2026-06-04 elsewhere): a typo
                // like `"inpts": [...]` would silently leave inputs
                // empty + slip through to a misleading "empty block"
                // reject. Allowlist the known sub-keys + emit per
                // unknown key.
                static constexpr std::array<std::string_view, 5>
                    kKnownKeys{"inputs", "outputs", "clobbered",
                               "inputRoles", "outputRoles"};
                for (auto it = ir.begin(); it != ir.end(); ++it) {
                    bool known = false;
                    for (auto const& k : kKnownKeys) {
                        if (it.key() == k) { known = true; break; }
                    }
                    if (!known) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/opcodes/{}/implicitRegisters/{}",
                                              i, it.key()),
                                  std::format("unknown key '{}' — allowed "
                                              "keys are 'inputs', 'outputs', "
                                              "'clobbered', 'inputRoles', "
                                              "'outputRoles' (typo "
                                              "discriminator)",
                                              it.key()));
                    }
                }
                ImplicitRegisterConstraint irc;
                auto readRegArray = [&](char const* field,
                                        std::vector<std::string>& out) {
                    if (!ir.contains(field)) return;
                    auto const& arr = ir.at(field);
                    if (!arr.is_array()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/opcodes/{}/implicitRegisters/{}",
                                              i, field),
                                  "must be an array of register-name strings");
                        return;
                    }
                    out.reserve(arr.size());
                    for (std::size_t j = 0; j < arr.size(); ++j) {
                        auto const& s = arr.at(j);
                        if (!s.is_string()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("/opcodes/{}/implicitRegisters/{}/{}",
                                                  i, field, j),
                                      "every entry must be a string");
                            continue;
                        }
                        out.push_back(s.get<std::string>());
                    }
                };
                readRegArray("inputs",    irc.inputNames);
                readRegArray("outputs",   irc.outputNames);
                readRegArray("clobbered", irc.clobberedNames);
                // Role maps (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-
                // CONTRACT): each is an OBJECT of role → register
                // name. Shape-only here; role-vocabulary, membership
                // (role's register ∈ the positional array), and
                // name→ordinal resolution happen in the post-register
                // resolution block with the other cross-field checks.
                auto readRoleMap =
                    [&](char const* field,
                        std::vector<std::pair<std::string, std::string>>& out) {
                    if (!ir.contains(field)) return;
                    auto const& obj = ir.at(field);
                    if (!obj.is_object()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/opcodes/{}/implicitRegisters/{}",
                                              i, field),
                                  "must be an object mapping role names to "
                                  "register-name strings");
                        return;
                    }
                    for (auto it = obj.begin(); it != obj.end(); ++it) {
                        if (!it.value().is_string()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("/opcodes/{}/implicitRegisters/{}/{}",
                                                  i, field, it.key()),
                                      "role value must be a register-name "
                                      "string");
                            continue;
                        }
                        out.emplace_back(it.key(),
                                         it.value().get<std::string>());
                    }
                };
                readRoleMap("inputRoles",  irc.inputRoleNames);
                readRoleMap("outputRoles", irc.outputRoleNames);
                info.implicitRegisters = std::move(irc);
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

    // ── registerClassOps (FC2 Part B — optional) ───────────────────
    // Per-register-class move/load/store mnemonic table. A class with
    // no row resolves to the universal defaults iff it is GPR (see
    // TargetSchema::regClassOpOpcode); a declared row may omit slots
    // (consumers fail loud on an omitted slot — trigger discipline).
    if (doc.contains("registerClassOps")) {
        if (!doc.at("registerClassOps").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/registerClassOps",
                      "'registerClassOps' must be an array");
        } else {
            auto const& rows = doc.at("registerClassOps");
            for (std::size_t i = 0; i < rows.size(); ++i) {
                auto const& r = rows[i];
                if (!r.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/registerClassOps/{}", i),
                              "registerClassOps entry must be an object");
                    continue;
                }
                if (!r.contains("class") || !r.at("class").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/registerClassOps/{}/class", i),
                              "missing or non-string 'class'");
                    continue;
                }
                auto const cls =
                    targetRegClassFromName(r.at("class").get<std::string>());
                if (!cls.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/registerClassOps/{}/class", i),
                              "expected 'gpr' / 'fpr' / 'vr' / 'flags'");
                    continue;
                }
                auto& row = data.registerClassOps[static_cast<std::size_t>(*cls)];
                if (row.declared) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/registerClassOps/{}/class", i),
                              std::format("duplicate registerClassOps row for "
                                          "class '{}'",
                                          targetRegClassName(*cls)));
                    continue;
                }
                row.declared = true;
                auto readOp = [&](char const* field, std::string& out) {
                    if (!r.contains(field)) return;
                    if (!r.at(field).is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/registerClassOps/{}/{}", i, field),
                                  "must be a mnemonic string");
                        return;
                    }
                    out = r.at(field).get<std::string>();
                };
                readOp("move",  row.move);
                readOp("load",  row.load);
                readOp("store", row.store);
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
                // D-WIN64-LARGE-FRAME-STACK-PROBE: OS stack guard-page
                // size + probe step. Optional (default 0 = no probing —
                // Linux/macOS/arm64). ms_x64 declares 4096. Validated
                // below: when nonzero MUST be a power of two.
                readBoundedInt(c, coll, ccPath, "stackProbePageBytes",
                               cc.stackProbePageBytes);
                // FC7 by-value aggregate ABI (D-FC7-STRUCT-BY-VALUE-ARG-RETURN):
                // the max aggregate size passed/returned in registers (SysV 16,
                // Win64 8, AAPCS64 16); larger ⇒ by-reference / sret.
                readBoundedInt(c, coll, ccPath, "aggregateMaxRegBytes",
                               cc.aggregateMaxRegBytes);
                // FC7: the by-value aggregate CLASSIFICATION strategy (a closed
                // verb set — the realization tier switches on this enum, never
                // identity). Unknown strategy ⇒ FAIL-LOUD (no silent fallback).
                if (c.contains("aggregateClassification")) {
                    if (!c.at("aggregateClassification").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/aggregateClassification", ccPath),
                                  "'aggregateClassification' must be a strategy-name string");
                    } else {
                        auto const s = c.at("aggregateClassification").get<std::string>();
                        if (auto const k = aggregateClassKindFromName(s); k.has_value()) {
                            cc.aggregateClassification = *k;
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("{}/aggregateClassification", ccPath),
                                      std::format("unknown aggregate-classification "
                                                  "strategy '{}'", s));
                        }
                    }
                }
                // D-ML7-2.6: slot-aligned arg passing (Win64 ms_x64).
                // Defaults to false (independent counters — SysV/AAPCS64
                // semantics). A cc declaring `slotAligned: true` means
                // each arg consumes one shared slot index regardless of
                // class.
                if (c.contains("slotAligned")) {
                    if (!c.at("slotAligned").is_boolean()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/slotAligned", ccPath),
                                  "'slotAligned' must be a boolean");
                    } else {
                        cc.slotAligned = c.at("slotAligned").get<bool>();
                    }
                }
                // FC12c (D-FC12C-APPLE-ARM64-VARIADIC-CALLEE): optional — when true,
                // every variadic arg of a variadic call is forced onto the stack
                // (Apple arm64). Default false (AAPCS64 + x86 CCs unaffected).
                if (c.contains("variadicArgsAlwaysStack")) {
                    if (!c.at("variadicArgsAlwaysStack").is_boolean()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/variadicArgsAlwaysStack", ccPath),
                                  "'variadicArgsAlwaysStack' must be a boolean");
                    } else {
                        cc.variadicArgsAlwaysStack =
                            c.at("variadicArgsAlwaysStack").get<bool>();
                    }
                }
                // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: optional —
                // when true, a by-value aggregate placed wholly on the stack
                // (it straddled the reg/stack boundary) EXHAUSTS the overflowed
                // arg-register class (AAPCS64). Default false = BACKFILL (SysV:
                // the leftover registers stay available for later args). Win64 is
                // slot-aligned so it never straddles (the flag is inert).
                if (c.contains("aggregateStackExhaustsRegisters")) {
                    if (!c.at("aggregateStackExhaustsRegisters").is_boolean()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/aggregateStackExhaustsRegisters",
                                              ccPath),
                                  "'aggregateStackExhaustsRegisters' must be a "
                                  "boolean");
                    } else {
                        cc.aggregateStackExhaustsRegisters =
                            c.at("aggregateStackExhaustsRegisters").get<bool>();
                    }
                }
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
                // D-LANG-VARIADIC (step 13.4, 2026-06-02): optional
                // caller-side vector-count register for variadic calls.
                // SysV AMD64 sets it to "al"; Win64 / AAPCS64 omit it.
                if (c.contains("variadicVectorCountReg")) {
                    if (!c.at("variadicVectorCountReg").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/variadicVectorCountReg", ccPath),
                                  "must be a register-name string");
                    } else {
                        auto const name = c.at("variadicVectorCountReg").get<std::string>();
                        auto it = data.registerIndex.find(name);
                        if (it != data.registerIndex.end()) {
                            cc.variadicVectorCountReg = TargetCallingConvention::NamedRegisterRef{
                                name, it->second
                            };
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("{}/variadicVectorCountReg", ccPath),
                                      std::format("variadic vector-count register "
                                                  "'{}' is not in the register table", name));
                        }
                    }
                }
                // FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): AAPCS64/Apple's x8
                // indirect-result-location register for sret returns. Omitted
                // by SysV / Win64 (their sret pointer is a hidden first arg).
                if (c.contains("indirectResultRegister")) {
                    if (!c.at("indirectResultRegister").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/indirectResultRegister", ccPath),
                                  "must be a register-name string");
                    } else {
                        auto const name = c.at("indirectResultRegister").get<std::string>();
                        auto it = data.registerIndex.find(name);
                        if (it != data.registerIndex.end()) {
                            cc.indirectResultRegister = TargetCallingConvention::NamedRegisterRef{
                                name, it->second
                            };
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("{}/indirectResultRegister", ccPath),
                                      std::format("indirect-result register '{}' is not "
                                                  "in the register table", name));
                        }
                    }
                }
                // FC12a-core (D-FC12A-VARIADIC-CALLEE): the optional `__va_list_tag`
                // layout + register-save-area geometry for variadic-callee support.
                // Present on SysV AMD64; omitted by Win64 / AAPCS64 (their variadic
                // ABI differs — those CCs fail loud at the va_start site). Mirrors
                // the optional `variadicVectorCountReg` / `indirectResultRegister`
                // shape above: a present-but-malformed block emits and skips
                // (leaving the optional unengaged), so the consumer's
                // has_value() guard fails loud rather than mis-walking a half-set layout.
                if (c.contains("vaListLayout")) {
                    if (!c.at("vaListLayout").is_object()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/vaListLayout", ccPath),
                                  "must be an object");
                    } else {
                        auto const& vl = c.at("vaListLayout");
                        VaListLayout layout;
                        bool vlOk = true;
                        // Read a required non-negative u32 scalar; emit + clear vlOk on miss.
                        auto readU32 = [&](char const* key, std::uint32_t& out) {
                            if (!vl.contains(key) || !vl.at(key).is_number_unsigned()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          std::format("{}/vaListLayout/{}", ccPath, key),
                                          std::format("'{}' is required and must be a "
                                                      "non-negative integer", key));
                                vlOk = false;
                                return;
                            }
                            out = vl.at(key).get<std::uint32_t>();
                        };
                        // Read a {byteOffset,widthBytes} field object.
                        auto readField = [&](char const* key, VaListLayout::Field& out) {
                            if (!vl.contains(key) || !vl.at(key).is_object()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          std::format("{}/vaListLayout/{}", ccPath, key),
                                          std::format("'{}' is required and must be an "
                                                      "object {{byteOffset,widthBytes}}", key));
                                vlOk = false;
                                return;
                            }
                            auto const& f = vl.at(key);
                            if (!f.contains("byteOffset") || !f.at("byteOffset").is_number_unsigned()
                             || !f.contains("widthBytes") || !f.at("widthBytes").is_number_unsigned()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          std::format("{}/vaListLayout/{}", ccPath, key),
                                          "field must declare non-negative 'byteOffset' "
                                          "and 'widthBytes'");
                                vlOk = false;
                                return;
                            }
                            out.byteOffset = f.at("byteOffset").get<std::uint32_t>();
                            out.widthBytes = f.at("widthBytes").get<std::uint32_t>();
                        };
                        // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the lowering
                        // strategy gates WHICH fields are required. ABSENT defaults
                        // to SysVRegisterSave (the pre-FC12b shape — back-compat).
                        if (vl.contains("strategy")) {
                            if (!vl.at("strategy").is_string()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          std::format("{}/vaListLayout/strategy", ccPath),
                                          "'strategy' must be a string");
                                vlOk = false;
                            } else {
                                auto const sname =
                                    vl.at("strategy").get<std::string>();
                                auto const s = vaListStrategyFromName(sname);
                                if (!s.has_value()) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              std::format("{}/vaListLayout/strategy", ccPath),
                                              std::format("unknown va_list strategy '{}' "
                                                          "(sysv_register_save / "
                                                          "homogeneous_pointer / "
                                                          "aapcs64_dual_cursor)", sname));
                                    vlOk = false;
                                } else {
                                    layout.strategy = *s;
                                }
                            }
                        }
                        // FC12c: an optional bool on the va_list block — default false.
                        auto readBoolDefaultFalse =
                            [&](char const* key, bool& out) {
                                if (!vl.contains(key)) { out = false; return; }
                                if (!vl.at(key).is_boolean()) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              std::format("{}/vaListLayout/{}", ccPath, key),
                                              std::format("'{}' must be a boolean", key));
                                    vlOk = false;
                                    return;
                                }
                                out = vl.at(key).get<bool>();
                            };
                        // Branch the remaining parse on the strategy. SysVRegisterSave
                        // requires the full register-save geometry; HomogeneousPointer
                        // requires only namedArgSlotBytes (+ optional variadicUsesOverflow
                        // Base for Apple arm64); Aapcs64DualCursor (FC12c) requires the
                        // 5 `__va_list` fields + the GR/VR save geometry. namedArgSlot
                        // Bytes is required on every realized arm.
                        switch (layout.strategy) {
                            case VaListStrategy::SysVRegisterSave:
                                readField("gpOffsetField",        layout.gpOffsetField);
                                readField("fpOffsetField",        layout.fpOffsetField);
                                readField("overflowArgAreaField", layout.overflowArgAreaField);
                                readField("regSaveAreaField",     layout.regSaveAreaField);
                                readU32("gpSaveCount",   layout.gpSaveCount);
                                readU32("gpSlotBytes",   layout.gpSlotBytes);
                                readU32("fpSaveCount",   layout.fpSaveCount);
                                readU32("fpSlotBytes",   layout.fpSlotBytes);
                                readU32("gpOffsetLimit", layout.gpOffsetLimit);
                                readU32("fpOffsetLimit", layout.fpOffsetLimit);
                                readU32("namedArgSlotBytes", layout.namedArgSlotBytes);
                                break;
                            case VaListStrategy::HomogeneousPointer:
                                readU32("namedArgSlotBytes", layout.namedArgSlotBytes);
                                // FC12c: Apple arm64 anchors `ap` at the overflow base.
                                readBoolDefaultFalse("variadicUsesOverflowBase",
                                                     layout.variadicUsesOverflowBase);
                                break;
                            case VaListStrategy::Aapcs64DualCursor:
                                // FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE): the 5-field
                                // `__va_list` locator + the GR/VR save-area geometry.
                                readField("stackField",   layout.stackField);
                                readField("grTopField",   layout.grTopField);
                                readField("vrTopField",   layout.vrTopField);
                                readField("grOffsField",  layout.grOffsField);
                                readField("vrOffsField",  layout.vrOffsField);
                                readU32("gpSaveCount",       layout.gpSaveCount);
                                readU32("gpSlotBytes",       layout.gpSlotBytes);
                                readU32("fpSaveCount",       layout.fpSaveCount);
                                readU32("fpSlotBytes",       layout.fpSlotBytes);
                                readU32("namedArgSlotBytes", layout.namedArgSlotBytes);
                                break;
                        }
                        if (vlOk) cc.vaListLayout = layout;
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

    // ── Implicit-register-constraint resolution + validation ──────
    // Cycle 10p: resolves each opcode's `implicitRegisters` names to
    // register ordinals via `data.registerIndex` (which is fully
    // populated by this point — registers were loaded above). Also
    // enforces the per-opcode invariants that depend on a populated
    // register table: unknown-name reject, within-array duplicate
    // reject, empty-block reject (typo discriminator). Cross-array
    // overlap is allowed (idiv's RAX is both an input dividend AND
    // an output quotient). This LOAD-time placement keeps `validate()`
    // pure-const + cross-OPCODE-only; per-opcode field resolution
    // happens once at construction. Closes the 7-agent fold's silent-
    // failure F1 (const_cast smell removed) + the code-reviewer's
    // ordinal-resolution-should-live-in-loader recommendation.
    for (std::size_t opIdx = 0; opIdx < data.opcodes.size(); ++opIdx) {
        auto& info = data.opcodes[opIdx];
        if (!info.implicitRegisters.has_value()) continue;
        auto& ir = *info.implicitRegisters;
        if (ir.inputNames.empty()
         && ir.outputNames.empty()
         && ir.clobberedNames.empty()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/implicitRegisters", opIdx),
                      std::format("opcode '{}': `implicitRegisters` is an "
                                  "empty block — either declare at least "
                                  "one entry in 'inputs'/'outputs'/"
                                  "'clobbered' or omit the block entirely. "
                                  "An empty block is a typo discriminator "
                                  "(author meant to constrain something).",
                                  info.mnemonic));
            continue;
        }
        auto resolveArr = [&](std::vector<std::string> const& names,
                              std::vector<std::uint16_t>& ordinals,
                              char const* field) {
            ordinals.clear();
            ordinals.reserve(names.size());
            // Per-array duplicate detection via a forward scan over
            // already-emitted names — preserves ordinal-index
            // parity with names (`ordinals[k]` always corresponds to
            // `names[k]` that was admitted; a name failing resolution
            // OR duplication still occupies its index in `names` but
            // gets a sentinel ordinal, so consumers iterating both
            // arrays in lockstep see the failure).
            for (std::size_t j = 0; j < names.size(); ++j) {
                auto const& n = names[j];
                bool duplicate = false;
                for (std::size_t k = 0; k < j; ++k) {
                    if (names[k] == n) { duplicate = true; break; }
                }
                if (duplicate) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format(
                                  "/opcodes/{}/implicitRegisters/{}/{}",
                                  opIdx, field, j),
                              std::format("opcode '{}': duplicate register "
                                          "'{}' in implicitRegisters.{}",
                                          info.mnemonic, n, field));
                    continue;
                }
                auto it = data.registerIndex.find(n);
                if (it == data.registerIndex.end()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format(
                                  "/opcodes/{}/implicitRegisters/{}/{}",
                                  opIdx, field, j),
                              std::format("opcode '{}': implicitRegisters.{} "
                                          "names unknown register '{}' "
                                          "(must resolve through this "
                                          "target's register table)",
                                          info.mnemonic, field, n));
                    continue;
                }
                ordinals.push_back(static_cast<std::uint16_t>(it->second));
            }
        };
        resolveArr(ir.inputNames,     ir.inputOrdinals,     "inputs");
        resolveArr(ir.outputNames,    ir.outputOrdinals,    "outputs");
        resolveArr(ir.clobberedNames, ir.clobberedOrdinals, "clobbered");

        // D-TARGET-IMPLICIT-REGISTER-CONSTRAINT cross-array invariant
        // (cycle 10r 7-agent review fold F1 CRITICAL 9/10, 2026-06-04):
        // every `outputs` register MUST also appear in `clobbered`. An
        // implicit-output is a register the instruction WRITES — by
        // definition any vreg whose live range covers this op gets
        // destroyed if it lives in that register, which is exactly
        // what `clobbered` declares to regalloc's
        // `collectImplicitClobberPositions`. The regalloc consumes
        // only `inputs` + `clobbered` to build the forbidden set;
        // `outputs` informs the lowering's ordinal lookup but does
        // not by itself constrain allocation. A JSON edit that drops
        // `clobbered: ["rdx"]` from `xor_rdx_zero` while keeping
        // `outputs: ["rdx"]` is internally inconsistent + would
        // silently allow divisor vregs into RDX → zeroed by xor →
        // divide-by-zero trap. Fail loud at load time.
        std::set<std::uint16_t> const clobberedSet(
            ir.clobberedOrdinals.begin(),
            ir.clobberedOrdinals.end());
        for (std::size_t k = 0; k < ir.outputOrdinals.size(); ++k) {
            if (clobberedSet.find(ir.outputOrdinals[k]) == clobberedSet.end()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format(
                              "/opcodes/{}/implicitRegisters/outputs/{}",
                              opIdx, k),
                          std::format("opcode '{}': implicit-output "
                                      "register '{}' must also be "
                                      "declared in implicitRegisters."
                                      "clobbered (every register the "
                                      "instruction WRITES is by "
                                      "definition clobbered for any "
                                      "prior live value — the regalloc "
                                      "consumes clobbered to build its "
                                      "forbidden set; missing this "
                                      "declaration would silently admit "
                                      "vregs into a register the op is "
                                      "about to overwrite)",
                                      info.mnemonic,
                                      ir.outputNames[k]));
            }
        }

        // Role-map resolution + validation (D-CSUBSET-MOD-OP-CODEGEN-
        // OUTPUT-INDEX-CONTRACT, 2026-06-10). Three rejects per role:
        //   1. unknown role name (typo discriminator — the lowering
        //      queries a registered vocabulary; "remaindr" must fail
        //      at LOAD, not surface as a missing-role at lowering);
        //   2. the role's register must appear in the corresponding
        //      POSITIONAL array (a role naming a register the op does
        //      not declare as an implicit input/output is internally
        //      inconsistent);
        //   3. the register name must resolve through the target's
        //      register table (same rule as the positional arrays).
        // The registered vocabulary grows as new projection shapes
        // arrive (a fail-loud reject here forces the deliberate
        // extension rather than a silent free-form string).
        // FC3.5 sweep-c1 added "count" — the shift-count input of the
        // implicit-count shift realization (x86 SHL/SHR/SAR read the
        // count from CL; the MIR→LIR shift lowering pins the count
        // vreg into the role-declared register exactly like the div
        // lowering's "dividend" pin).
        static constexpr std::array<std::string_view, 4>
            kKnownImplicitRegisterRoles{"dividend", "quotient", "remainder",
                                        "count"};
        auto resolveRoles =
            [&](std::vector<std::pair<std::string, std::string>> const& roles,
                std::vector<std::pair<std::string, std::uint16_t>>& resolved,
                std::vector<std::string> const& memberNames,
                char const* field) {
            resolved.clear();
            for (auto const& [role, regName] : roles) {
                bool knownRole = false;
                for (auto const& k : kKnownImplicitRegisterRoles) {
                    if (role == k) { knownRole = true; break; }
                }
                if (!knownRole) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format(
                                  "/opcodes/{}/implicitRegisters/{}/{}",
                                  opIdx, field, role),
                              std::format("opcode '{}': unknown implicit-"
                                          "register role '{}' — registered "
                                          "roles are 'dividend', 'quotient', "
                                          "'remainder', 'count' (typo "
                                          "discriminator; extend the "
                                          "registered vocabulary for a new "
                                          "projection shape)",
                                          info.mnemonic, role));
                    continue;
                }
                bool member = false;
                for (auto const& n : memberNames) {
                    if (n == regName) { member = true; break; }
                }
                if (!member) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format(
                                  "/opcodes/{}/implicitRegisters/{}/{}",
                                  opIdx, field, role),
                              std::format("opcode '{}': role '{}' names "
                                          "register '{}' which is not "
                                          "declared in the corresponding "
                                          "positional array (a projection "
                                          "role must tag a register the op "
                                          "actually declares)",
                                          info.mnemonic, role, regName));
                    continue;
                }
                auto it = data.registerIndex.find(regName);
                if (it == data.registerIndex.end()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format(
                                  "/opcodes/{}/implicitRegisters/{}/{}",
                                  opIdx, field, role),
                              std::format("opcode '{}': role '{}' names "
                                          "unknown register '{}'",
                                          info.mnemonic, role, regName));
                    continue;
                }
                resolved.emplace_back(
                    role, static_cast<std::uint16_t>(it->second));
            }
        };
        resolveRoles(ir.inputRoleNames,  ir.inputRoleOrdinals,
                     ir.inputNames,  "inputRoles");
        resolveRoles(ir.outputRoleNames, ir.outputRoleOrdinals,
                     ir.outputNames, "outputRoles");
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
