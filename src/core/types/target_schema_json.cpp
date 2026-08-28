#include "core/types/target_schema.hpp"

#include "core/crypto/sha256.hpp"                 // crypto::sha256Hex — the retained content digest
#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/substrate/relocation_table_json.hpp"
#include "core/types/config_key_vocabulary.hpp"   // TF-C74: the shared closed-key guard
#include "core/types/parse_diagnostic.hpp"
#include "core/types/predefined_macro_json.hpp"   // TF-C74: the shared predefine parser

#include <nlohmann/json.hpp>

#include <array>
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

// The anchor id this loader's stacked-arg-packing refusal cites, spelled ONCE
// and on ONE line. ⚠ A `std::format` message assembled from adjacent string
// literals is exactly where an anchor id gets WRAPPED across two lines by a
// re-indent — which does not fail anything, it makes the id invisible to the
// registry scan and mints a false one. Naming it here makes that unwritable.
inline constexpr std::string_view kApplePackingAnchor =
    "D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED";

using Collector = substrate::DiagnosticCollector;

// Note: EVERY opcode-vocabulary `…FromName` lives in `target_schema.hpp`
// alongside its enum and its `kXxxTable`, so loader + future emit-side
// serializers share one source of truth for the string mapping.
// ⚠ There used to be ONE exception, right here: a private `parseResultRule`
// if-chain respelling `kTargetResultRuleTable`'s three names because that
// table shipped a `…Name` half and no `…FromName` half. The gap was closed in
// the header rather than papered over again here — a loader that has to write
// its own parser has found a hole in the vocabulary, not a reason to keep a
// local copy (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).

// ── D-CONFIG-TARGET-LOADER-CONTAINER-KEYS-UNGATED ─────────────────────────
//
// Reject every key of `obj` that is not in `known`. THE ONE loop; every
// closed-key object in this loader calls it.
//
// ★ WHY A HELPER AND NOT A LOOP PER SITE — this is not tidying, it is the
// defect. Before this existed the file had FIVE hand-rolled key loops and
// THREE of them were wrong in the same way: they hardcoded the literal
// `"$comment"` (or nothing at all) instead of the `$`-PREFIX predicate, so a
// `$templateComment` / `$framePointerComment` — spellings the shipped targets
// actually use — would have been rejected as a typo. A carve-out that must be
// remembered per site is a carve-out that holds only where someone remembered
// it. Here it is UNSKIPPABLE: `isDocumentationKey` is applied by the helper,
// not by the caller.
//
// ★★ AND THE ARCHETYPE THIS CLOSES, because it showed up twice in one day in
// two unrelated files: a CONTAINER object with no key set sitting next to
// NESTED objects that have one. The neighbours' rejection loops are exactly
// what makes the container's absence invisible — you read `variants[j]/guard`
// rejecting typos and conclude the variant row does too, when a misspelled
// `"tempalte"` was silently yielding an all-default encoding template. Same
// shape as `numberStyle`'s direct keys in the LANGUAGE loader (routed to that
// lane). The cure is per-CONTAINER, not per-leaf.
//
// `objectLabel` names the object in the message ("variant", "register row")
// so the diagnostic reads as prose rather than as a path echo.
//
// ★★ THE LOOP ITSELF NOW LIVES IN `core/types/config_key_vocabulary.hpp`,
// beside `isDocumentationKey`, and the argument above is WHY — it just could
// not reach past this translation unit while it was written here. Three
// sibling loaders had independently written the same helper and two of them
// omitted the `$` carve-out this header spends a paragraph on. What remains
// here is an ADAPTER, not a fifth copy: it binds the shared check to THIS
// loader's sink, diagnostic code and `path/key` convention. The allowed-key
// TABLES stay with the objects they describe, which is the half a maintainer
// actually edits.
template <std::size_t N>
void rejectUnknownKeys(json const& obj,
                       std::array<std::string_view, N> const& known,
                       std::string_view path,
                       std::string_view objectLabel,
                       Collector& coll) {
    detail::rejectUnknownKeys(obj, known, objectLabel,
        [&](std::string_view key, std::string message) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("{}/{}", path, key), std::move(message));
        });
}

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
                && w.get<std::int64_t>() != 32 && w.get<std::int64_t>() != 64
                && w.get<std::int64_t>() != 128)) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/guard/width", opIdx, vi),
                      "'width' must be the integer 8, 16, 32, 64, or 128 "
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
    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB (introduced) /
    // D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE (generalized + renamed):
    // OPTIONAL `negValue` (bool). Absent ⇒ false (the non-negative magnitude
    // axis — every pre-existing variant). Present-and-true ⇒ the variant
    // matches only a STRICTLY NEGATIVE value-bearing operand (ImmInt or
    // MemOffset), keyed by |value| against [immMin, immMax]. A non-boolean is
    // a load-time reject (never a silently dropped flag). validate() rejects
    // negValue on a guard with neither an `imm32` nor a `memoffset` operand
    // (nothing to sign-route) — the same coherence family as immMin/immMax.
    // ⚠ The key was spelled `negMemoffset` before 2026-08-13; the old
    // spelling is REJECTED by the unknown-key gate below rather than silently
    // read as `false` (which would have turned arm64's three negative-disp
    // `lea` variants into duplicate positive ones).
    if (g.contains("negValue")) {
        auto const& nv = g.at("negValue");
        if (!nv.is_boolean()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/guard/negValue",
                                  opIdx, vi),
                      "'negValue' must be a boolean");
        } else {
            variant.negValue = nv.get<bool>();
        }
    }
    // D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE: OPTIONAL
    // `memoryDestination` (bool). Absent ⇒ the variant does not discriminate
    // on the memory reference's role (every pre-existing variant). Present ⇒
    // the variant matches only an instruction whose
    // `kLirInstFlagMemoryIsDestination` agrees. This is what separates
    // `cmpq %r14, mem` (39 /r) from `cmpq mem, %r14` (3B /r), whose LIR
    // operand lists are byte-identical. A non-boolean is a load-time reject,
    // never a silently dropped axis — dropping it would collapse the two
    // directions onto one variant and encode one spelling as the other.
    if (g.contains("memoryDestination")) {
        auto const& md = g.at("memoryDestination");
        if (!md.is_boolean()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/guard/memoryDestination",
                                  opIdx, vi),
                      "'memoryDestination' must be a boolean");
        } else {
            variant.memoryDestination = md.get<bool>();
        }
    }
    // `D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD` discipline: every key this
    // guard object may carry is read ABOVE (or just below, for
    // `operandKinds`) via a bare `g.contains(...)`, so a typo — or a
    // spelling this loader USED to accept — would be silently ignored and
    // the variant would quietly take the default routing. That is precisely
    // how a renamed axis turns into a wrong-variant election with no
    // diagnostic. Allowlist the known sub-keys and emit per unknown key.
    // Any `$`-PREFIXED key is allowed on top (config-wide prose convention) —
    // the prefix, not the single spelling `$comment`, which is what
    // `$framePointerComment` and friends depend on. The helper applies it, so
    // the table below states only this object's real vocabulary.
    // ⚠ 'negMemoffset' was RENAMED to 'negValue' when the sign axis
    // generalized from a memory displacement to any value-bearing operand;
    // the old spelling now lands here as an unknown key rather than being
    // read as `false`.
    static constexpr std::array<std::string_view, 6> kGuardKeys{
        "operandKinds", "width", "immMin", "immMax", "negValue",
        "memoryDestination"};
    DSS_CHECK_KEY_VOCABULARY(kGuardKeys);
    rejectUnknownKeys(g, kGuardKeys,
                      std::format("/opcodes/{}/encoding/variants/{}/guard",
                                  opIdx, vi),
                      "a variant guard", coll);
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
            // ⚠ PROJECTED, AND THE RETYPED LIST IT REPLACES WAS ALREADY WRONG:
            // it named 'reg' / 'imm32' / 'symbol' while the table has SEVEN
            // rows, so `membase`, `memoffset`, `blockref` and `imm64` were
            // accepted by the check and denied by the sentence. That is the
            // drift this class predicts, already realized
            // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/encoding/variants/{}/guard/operandKinds/{}", opIdx, vi, ki),
                      std::format("expected {}",
                                  detail::renderAllowedList(
                                      allNames(kOperandKindFilterTable), " / ")));
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
    // A template key is a BIT PATTERN or a prefix byte; a typo here emits a
    // different instruction. `fixedWord`/`fixedWords` are mutually exclusive
    // (checked below) but both belong to the vocabulary.
    static constexpr std::array<std::string_view, 11> kTemplateKeys{
        "rexW", "opcode", "mandatoryPrefix", "modrmRegExt",
        "condCodeFromPayload", "condBitPos", "condInvert",
        "payloadBytePrefix", "forceRexPrefix", "fixedWord", "fixedWords"};
    DSS_CHECK_KEY_VOCABULARY(kTemplateKeys);
    rejectUnknownKeys(t, kTemplateKeys,
                      std::format("/opcodes/{}/encoding/variants/{}/template",
                                  opIdx, vi),
                      "an encoding template", coll);
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
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): `payloadBytePrefix` — emit the
    // instruction's LIR payload low byte as the FIRST byte (x86 prefix
    // group 2 segment override, before mandatoryPrefix + REX). Used by
    // `tlsbase`; the byte VALUE flows from format config through the
    // lowering's payload, keeping the shared target JSON format-blind.
    // The encoder fails loud on payload low-byte 0 (never a valid
    // segment override).
    if (t.contains("payloadBytePrefix")) {
        auto const path = std::format("/opcodes/{}/encoding/variants/{}/template/payloadBytePrefix", opIdx, vi);
        if (!t.at("payloadBytePrefix").is_boolean()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'payloadBytePrefix' must be a boolean");
        } else {
            tmpl.payloadBytePrefix = t.at("payloadBytePrefix").get<bool>();
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
        // ⚠ THE HAND-WRITTEN LIST THIS REPLACES NAMED **8** OF **32** SLOT
        // KINDS, grouped by encoding shape with parenthetical annotations. It
        // was written when eight was the whole vocabulary and was never
        // revisited; every slot added since — `imm12`, `imm19`,
        // `imm32.movzmovk`, `memreloc.disp32`, twenty-four in all — is
        // ACCEPTED by the lookup above and DENIED by the sentence below it.
        // The grouping annotations went with it, and deliberately: which shape
        // a slot belongs to is a fact no table owns, so restating it here
        // would just be the same defect with fewer rows.
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  std::format("expected one of: {}",
                              detail::renderAllowedList(
                                  allNames(kEncodingSlotKindTable), " / ")));
        return;
    }
    variant.resultSlot = *r;
}

// D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD: the shared parse of one
// register-bank spelling. ONE reader for all three sites that carry a bank
// name (`encoding.registerClass`, a variant's `resultRegClass`, a wire's
// `regClass`) so the three cannot disagree about what a bank name is or which
// names are refused. The ALLOWED list is projected off `kTargetRegClassTable`
// — never retyped (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
//
// ⚠ `none` PARSES HERE and is refused by `validate()` rather than by this
// reader, deliberately: `targetRegClassFromName` owns the spelling set and
// `isOperableTargetRegClass` owns the "may a field draw from it" question, and
// splitting one of those two facts across two files is how the two drift. The
// message a `"none"` author sees names the operable set, from validate().
std::optional<TargetRegClass>
parseRegClassField(json const& obj, std::string_view key,
                   std::string const& path, Collector& coll) {
    if (!obj.contains(key)) return std::nullopt;
    auto const& n = obj.at(key);
    if (!n.is_string()) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  std::format("'{}' must be a register-class string (one "
                              "of {})",
                              key,
                              detail::renderAllowedList(
                                  allNames(kTargetRegClassTable), " / ")));
        return std::nullopt;
    }
    auto const c = targetRegClassFromName(n.get<std::string>());
    if (!c.has_value()) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  std::format("unknown register class '{}' — expected one "
                              "of {}",
                              n.get<std::string>(),
                              detail::renderAllowedList(
                                  allNames(kTargetRegClassTable), " / ")));
        return std::nullopt;
    }
    return c;
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
        static constexpr std::array<std::string_view, 2> kExtraSlotKeys{
            "slotKind", "wordIndex"};
        DSS_CHECK_KEY_VOCABULARY(kExtraSlotKeys);
        rejectUnknownKeys(e, kExtraSlotKeys, ePath,
                          "an extra result-slot placement", coll);
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
        // A dropped `relocationKind` is the sharpest hazard here: the wire
        // would encode literal bits where the linker was meant to patch a
        // symbol address.
        static constexpr std::array<std::string_view, 6> kWireKeys{
            "index", "slotKind", "relocationKind", "wordIndex",
            "prefixOpcodeBytes",
            // D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD: this field's
            // register-bank override.
            "regClass"};
        DSS_CHECK_KEY_VOCABULARY(kWireKeys);
        rejectUnknownKeys(o2, kWireKeys, wirePath, "an operand wire", coll);
        TargetEncodingWire wire;
        wire.regClass = parseRegClassField(
            o2, "regClass", std::format("{}/regClass", wirePath), coll);
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
            // Same projected slot vocabulary as `parseVariantResultSlot` —
            // see the note there for what the retyped list was hiding.
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("{}/slotKind", wirePath),
                      std::format("expected one of: {}",
                                  detail::renderAllowedList(
                                      allNames(kEncodingSlotKindTable), " / ")));
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
        // ★ THE HIGHEST-VALUE GATE IN THIS FILE, and the archetype's exact
        // shape: the variant ROW had no key set while its nested `guard`
        // object did. A misspelled `"tempalte"` therefore loaded clean and
        // `parseVariantTemplate`'s `v.contains("template")` simply returned —
        // leaving an ALL-DEFAULT template (fixedWord 0, no opcode bytes) that
        // the encoder would emit as zero words. The neighbouring guard loop
        // is what made the absence invisible.
        static constexpr std::array<std::string_view, 6> kVariantKeys{
            "guard", "template", "resultSlot", "extraResultSlots", "wires",
            // D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD: the result
            // field's register-bank override.
            "resultRegClass"};
        DSS_CHECK_KEY_VOCABULARY(kVariantKeys);
        rejectUnknownKeys(v, kVariantKeys,
                          std::format("/opcodes/{}/encoding/variants/{}",
                                      opIdx, vi),
                          "an encoding variant", coll);
        TargetEncodingVariant variant;
        variant.resultRegClass = parseRegClassField(
            v, "resultRegClass",
            std::format("/opcodes/{}/encoding/variants/{}/resultRegClass",
                        opIdx, vi),
            coll);
        parseVariantGuard      (v, opIdx, vi, variant, coll);
        parseVariantTemplate   (v, opIdx, vi, variant.tmpl, coll);
        parseVariantResultSlot (v, opIdx, vi, variant, coll);
        parseVariantExtraResultSlots(v, opIdx, vi, variant, coll);
        parseVariantWires      (v, opIdx, vi, variant, data, coll);
        out.push_back(std::move(variant));
    }
}

// ── D-TARGET-ENCODING-TABLE-EXPRESSES-ONLY-THE-DEGENERATE-SEQUENCE ────────
// The `lowering` block: opcode → the SEQUENCE of machine instructions that
// realizes it on this target. Shape-only here (names stay unresolved); the
// step→opcode resolution needs the COMPLETE mnemonic index, so it runs in the
// post-pass beside the implicit-register resolution — the same reason that one
// waits for the register table.
void parseLoweringOperand(json const& o, std::string const& path,
                          TargetLoweringOperand& out, Collector& coll) {
    static constexpr std::array<std::string_view, 4> kOperandKeys{
        "source", "temp", "imm", "const"};
    DSS_CHECK_KEY_VOCABULARY(kOperandKeys);
    rejectUnknownKeys(o, kOperandKeys, path, "a lowering-step operand", coll);
    // EXACTLY ONE kind key. Zero would be an all-default operand silently
    // reading MIR source 0 (the `"tempalte"` failure class this file's
    // highest-value gate exists for); two would make the read order the
    // meaning.
    int present = 0;
    for (auto const& k : kOperandKeys) if (o.contains(k)) ++present;
    if (present != 1) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  std::format("a lowering-step operand must carry EXACTLY ONE "
                              "of {} — found {}",
                              detail::renderAllowedList(
                                  allNames(kTargetLoweringOperandKindTable), " / "),
                              present));
        return;
    }
    if (o.contains("source")) {
        auto const& v = o.at("source");
        if (!v.is_number_unsigned() || v.get<std::uint64_t>() > 255u) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'source' must be a non-negative integer operand index");
            return;
        }
        out.kind        = TargetLoweringOperandKind::Source;
        out.sourceIndex = static_cast<std::uint8_t>(v.get<std::uint64_t>());
        return;
    }
    if (o.contains("temp")) {
        auto const& v = o.at("temp");
        if (!v.is_string() || v.get<std::string>().empty()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'temp' must be a non-empty string naming a temporary "
                      "an EARLIER step of this sequence defines");
            return;
        }
        out.kind     = TargetLoweringOperandKind::Temp;
        out.tempName = v.get<std::string>();
        return;
    }
    if (o.contains("imm")) {
        auto const& v = o.at("imm");
        if (!v.is_number_integer()
            || v.get<std::int64_t>() < std::numeric_limits<std::int32_t>::min()
            || v.get<std::int64_t>() > std::numeric_limits<std::int32_t>::max()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      "'imm' must be an integer that fits int32 (the LIR "
                      "inline-immediate operand's width)");
            return;
        }
        out.kind      = TargetLoweringOperandKind::Immediate;
        out.immediate = static_cast<std::int32_t>(v.get<std::int64_t>());
        return;
    }
    // `const`: a 64-bit BIT PATTERN, spelled as a hex STRING. A JSON number
    // cannot carry one — 0x43E0000000000000 exceeds the exactly-representable
    // double range every JSON reader parses unsuffixed numbers into, so a
    // numeric spelling would round and the author would never see it.
    auto const& v = o.at("const");
    if (!v.is_string()) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  "'const' must be a STRING holding a 0x-prefixed 64-bit hex "
                  "bit pattern (a JSON number cannot carry 64 bits exactly)");
        return;
    }
    std::string const s = v.get<std::string>();
    if (s.size() < 3 || s.size() > 18 || s[0] != '0'
        || (s[1] != 'x' && s[1] != 'X')) {
        coll.emit(DiagnosticCode::C_MalformedJson, path,
                  std::format("'const' must be a 0x-prefixed hex bit pattern of "
                              "1..16 digits — got '{}'", s));
        return;
    }
    std::uint64_t pattern = 0;
    for (std::size_t k = 2; k < s.size(); ++k) {
        char const c = s[k];
        int digit = -1;
        if (c >= '0' && c <= '9')      digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0) {
            coll.emit(DiagnosticCode::C_MalformedJson, path,
                      std::format("'const' has a non-hex character '{}' in "
                                  "'{}'", c, s));
            return;
        }
        pattern = (pattern << 4) | static_cast<std::uint64_t>(digit);
    }
    out.kind     = TargetLoweringOperandKind::Constant;
    out.constant = pattern;
}

void parseLoweringStep(json const& st, std::string const& path,
                       TargetLoweringStep& out, Collector& coll) {
    static constexpr std::array<std::string_view, 5> kStepKeys{
        "op", "result", "resultClass", "width", "operands"};
    DSS_CHECK_KEY_VOCABULARY(kStepKeys);
    rejectUnknownKeys(st, kStepKeys, path, "a lowering step", coll);
    if (!st.contains("op") || !st.at("op").is_string()
        || st.at("op").get<std::string>().empty()) {
        coll.emit(DiagnosticCode::C_MissingField, path + "/op",
                  "missing or empty 'op' — every lowering step must name a "
                  "mnemonic in THIS target's opcode table");
        return;
    }
    out.opcodeMnemonic = st.at("op").get<std::string>();
    if (st.contains("result")) {
        auto const& r = st.at("result");
        if (!r.is_string() || r.get<std::string>().empty()) {
            coll.emit(DiagnosticCode::C_MalformedJson, path + "/result",
                      "'result' must be a non-empty string — a temporary's "
                      "name, or the reserved name 'result' for the value the "
                      "lowered instruction itself produces");
        } else {
            out.hasResult  = true;
            out.resultName = r.get<std::string>();
            out.definesResult = (out.resultName == "result");
        }
    }
    if (out.hasResult) {
        if (!st.contains("resultClass") || !st.at("resultClass").is_string()) {
            coll.emit(DiagnosticCode::C_MissingField, path + "/resultClass",
                      std::format("a step that declares a 'result' must also "
                                  "declare its register class (one of {}) — the "
                                  "class is a fact about the instruction, and "
                                  "guessing it is how a GPR mov assembles onto "
                                  "an XMM ordinal",
                                  detail::renderAllowedList(
                                      allNames(kTargetRegClassTable), " / ")));
        } else if (auto const cls = targetRegClassFromName(
                       st.at("resultClass").get<std::string>());
                   cls.has_value() && *cls != TargetRegClass::None) {
            out.resultClass = *cls;
        } else {
            coll.emit(DiagnosticCode::C_MalformedJson, path + "/resultClass",
                      std::format("expected {} (not 'none' — a step with a "
                                  "result has a class)",
                                  detail::renderAllowedList(
                                      allNames(kTargetRegClassTable), " / ")));
        }
    } else if (st.contains("resultClass")) {
        coll.emit(DiagnosticCode::C_MalformedJson, path + "/resultClass",
                  "'resultClass' without 'result' — the step declares a class "
                  "for a register it never defines");
    }
    if (st.contains("width")) {
        auto const& w = st.at("width");
        if (!w.is_number_integer()
            || (w.get<std::int64_t>() != 8  && w.get<std::int64_t>() != 16
                && w.get<std::int64_t>() != 32 && w.get<std::int64_t>() != 64
                && w.get<std::int64_t>() != 128)) {
            coll.emit(DiagnosticCode::C_MalformedJson, path + "/width",
                      "'width' must be the integer 8, 16, 32, 64, or 128 (the same "
                      "operation-width vocabulary the encoding-variant guards "
                      "key on); omit it for the 64-bit LIR default");
        } else {
            out.widthBits = static_cast<std::uint8_t>(w.get<std::int64_t>());
        }
    }
    if (!st.contains("operands")) return;
    auto const& ops = st.at("operands");
    if (!ops.is_array()) {
        coll.emit(DiagnosticCode::C_MalformedJson, path + "/operands",
                  "'operands' must be an array");
        return;
    }
    out.operands.reserve(ops.size());
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (!ops[k].is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("{}/operands/{}", path, k),
                      "a lowering-step operand must be an object");
            continue;
        }
        TargetLoweringOperand op;
        parseLoweringOperand(ops[k], std::format("{}/operands/{}", path, k),
                             op, coll);
        out.operands.push_back(std::move(op));
    }
}

void parseLoweringBlock(json const& low, std::size_t opIdx,
                        TargetLoweringInfo& out, Collector& coll) {
    auto const blockPath = std::format("/opcodes/{}/lowering", opIdx);
    if (!low.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, blockPath,
                  "'lowering' must be an object");
        return;
    }
    static constexpr std::array<std::string_view, 1> kLoweringKeys{"sequences"};
    DSS_CHECK_KEY_VOCABULARY(kLoweringKeys);
    rejectUnknownKeys(low, kLoweringKeys, blockPath, "a lowering block", coll);
    if (!low.contains("sequences")) {
        coll.emit(DiagnosticCode::C_MissingField, blockPath + "/sequences",
                  "missing 'sequences' (required when a 'lowering' block is "
                  "present — an empty block would silently mean 'no expansion' "
                  "while the author's intent was to declare one)");
        return;
    }
    auto const& seqs = low.at("sequences");
    if (!seqs.is_array() || seqs.empty()) {
        coll.emit(DiagnosticCode::C_MalformedJson, blockPath + "/sequences",
                  "'sequences' must be a NON-EMPTY array");
        return;
    }
    out.sequences.reserve(seqs.size());
    for (std::size_t si = 0; si < seqs.size(); ++si) {
        auto const seqPath = std::format("{}/sequences/{}", blockPath, si);
        auto const& s = seqs[si];
        if (!s.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, seqPath,
                      "sequence entry must be an object");
            continue;
        }
        static constexpr std::array<std::string_view, 2> kSequenceKeys{
            "guard", "steps"};
        DSS_CHECK_KEY_VOCABULARY(kSequenceKeys);
        rejectUnknownKeys(s, kSequenceKeys, seqPath, "a lowering sequence", coll);
        TargetLoweringSequence seq;
        if (s.contains("guard")) {
            auto const& g = s.at("guard");
            if (!g.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson, seqPath + "/guard",
                          "'guard' must be an object");
            } else {
                static constexpr std::array<std::string_view, 1> kGuardKeys{"width"};
                DSS_CHECK_KEY_VOCABULARY(kGuardKeys);
                rejectUnknownKeys(g, kGuardKeys, seqPath + "/guard",
                                  "a lowering-sequence guard", coll);
                if (g.contains("width")) {
                    auto const& w = g.at("width");
                    if (!w.is_number_integer()
                        || (w.get<std::int64_t>() != 8 && w.get<std::int64_t>() != 16
                            && w.get<std::int64_t>() != 32
                            && w.get<std::int64_t>() != 64
                            && w.get<std::int64_t>() != 128)) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  seqPath + "/guard/width",
                                  "'width' must be the integer 8, 16, 32, 64, or 128");
                    } else {
                        seq.guardWidthBits =
                            static_cast<std::uint8_t>(w.get<std::int64_t>());
                    }
                }
            }
        }
        if (!s.contains("steps") || !s.at("steps").is_array()
            || s.at("steps").empty()) {
            coll.emit(DiagnosticCode::C_MalformedJson, seqPath + "/steps",
                      "missing or empty 'steps' — a sequence with no steps "
                      "would lower the operation to NOTHING and leave its "
                      "result undefined");
            continue;
        }
        auto const& steps = s.at("steps");
        seq.steps.reserve(steps.size());
        for (std::size_t ti = 0; ti < steps.size(); ++ti) {
            auto const stepPath = std::format("{}/steps/{}", seqPath, ti);
            if (!steps[ti].is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson, stepPath,
                          "step entry must be an object");
                continue;
            }
            TargetLoweringStep step;
            parseLoweringStep(steps[ti], stepPath, step, coll);
            seq.steps.push_back(std::move(step));
        }
        out.sequences.push_back(std::move(seq));
    }
}

} // namespace

LoadResult<std::shared_ptr<TargetSchema>> TargetSchema::loadFromText(
    std::string_view jsonText, std::string_view sourceLabel) {
    // ── Content digest ────────────────────────────────────────────────
    // Digest the bytes AS RECEIVED, before the parser is allowed an opinion
    // about them. This is the one chokepoint where the document bytes are
    // already in memory (`loadFromFile` reads them, hands them here, and drops
    // them), so the digest costs zero extra I/O — versus ~165 ms per
    // invocation to re-walk and re-read `src/dss-config/` (MEASURED
    // 2026-08-17, I/O-dominated). Computed BEFORE the parse so it is the
    // digest of what was actually LOADED, independent of what the parse made
    // of it. See `contentDigest()` for the full rationale and for why a
    // non-`loadFromText` construction leaves it EMPTY.
    std::string digest = crypto::sha256Hex(jsonText);

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

    // ── closed root-key vocabulary (TF-C74) ───────────────────────────────
    //
    // The TARGET loader had NO closed root-key vocabulary: every root key was
    // read through a bare `doc.contains(…)` and an unknown key was silently
    // ignored. MEASURED consequence for THIS cycle: a misspelled
    // `"predefindMacros"` would have loaded perfectly clean and the whole
    // per-architecture-identity feature would have silently no-op'd — the new
    // key would have been a knob that LIES. The language family closed this in
    // TF-C72 (`kDocumentKeys`); the target family closes it here, with the same
    // helper (`DSS_CHECK_KEY_VOCABULARY`) and the same `C_MalformedJson` code.
    //
    // The `$`-prefix carve-out is MANDATORY, not decorative: both shipped
    // target files use `$comment` / `$…Comment` heavily (MEASURED: 12 such
    // keys in arm64.target.json, 10 in x86_64.target.json), so without it the
    // guard would reject every shipped target on its first load.
    //
    // Every name here is a key the loader genuinely reads.
    static constexpr std::array<std::string_view, 17> kTargetDocumentKeys{
        // identity + loader gates
        "dssTargetVersion", "target",
        // per-target LANGUAGE-affecting semantics
        "charIsUnsigned", "predefinedMacros", "aggregateLayout", "tls",
        // which SOURCE LANGUAGE document spells this processor's assembly
        // (a NAME — D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET)
        "defaultAssemblyLanguage",
        // Which GNU inline-asm constraint LETTER means which register /
        // register class / operand form on this processor.
        //
        // ⚠⚠ THE FACET'S SINGLE HIGHEST-RISK LINE, AND ITS FAILURE MODE IS
        // ✔MEASURED RATHER THAN ASSUMED. Deleting this one entry (with the
        // array size dropped to 15 so it still compiles) was built and run:
        // both shipped targets FAIL TO LOAD with `/asmConstraints: unknown
        // top-level key 'asmConstraints' (typo discriminator)`, and the
        // blast radius is **683 of 855 tests red**, including SEGFAULTs in
        // the lir/asm suites whose consumers deref a schema that never
        // loaded. ⇒ omitting it is LOUD, not silent — which is exactly what
        // TF-C74's closed root-key vocabulary buys. Before that vocabulary
        // existed the same omission would have silently no-op'd the whole
        // facet, leaving a knob that lies. `ShippedTargetsDeclareAsm-
        // Constraints` pins that the key is genuinely consumed.
        "asmConstraints",
        // Which register VIEW a BARE `%0` names, per register class — the
        // half `asmConstraints` cannot answer, because it is about the
        // width a reference states rather than about which register a
        // letter binds. Undeclared is a legitimate state and every
        // consumer refuses by name; see `AsmBareOperandWidth`.
        "asmBareOperandWidths",
        // machine description
        "opcodes", "registers", "registerClassOps", "relocations",
        "condCodeEncoding",
        // ABI / softcall surface
        "wideFloatSoftcalls", "wideFloatSoftcallLibraryByFormat",
        "callingConventions"};
    DSS_CHECK_KEY_VOCABULARY(kTargetDocumentKeys);
    // The ROOT runs the same check as every nested object — it had its own
    // hand-written loop, which is how the container/leaf asymmetry this
    // helper's header describes gets in. Empty `path` yields `/key`.
    rejectUnknownKeys(doc, kTargetDocumentKeys, "", "the target document", coll);

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
    // ⚠ `description` is DECLARED BY BOTH SHIPPED TARGETS AND READ BY
    // NOTHING — it is human metadata that predates the `$`-prefix prose
    // convention. It belongs in the vocabulary because the vocabulary is
    // "keys this object may legally carry", not "keys the loader stores";
    // omitting it made BOTH shipped targets fail to load (✔MEASURED: 21 red
    // tests), which is the INVERSE failure a closed key set can cause and the
    // reason every gate here is pinned against the real shipped configs
    // rather than against a hand-written sample.
    static constexpr std::array<std::string_view, 8> kTargetKeys{
        "name", "version", "abiModel", "isa", "frameLoadMnemonic",
        "frameStoreMnemonic", "description", "dwarfReturnAddressColumn"};
    DSS_CHECK_KEY_VOCABULARY(kTargetKeys);
    rejectUnknownKeys(target, kTargetKeys, "/target",
                      "the target identity block", coll);
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
                      std::format("expected {}",
                                  detail::renderAllowedList(
                                      allNames(kTargetAbiModelTable), " / ")));
            return std::unexpected(std::move(coll).release());
        }
    }
    // ── target.isa — THE ARCHITECTURE THIS TARGET EXECUTES ────────────────
    // (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE.) The target half of the ISA
    // axis; `GrammarSchema::isa()` is the language half, and the gate
    // compares the two DECLARED strings. See the `TargetSchemaData::isa`
    // docblock for why this is neither `name` nor a `machine` code.
    //
    // OPTIONAL, and deliberately so: a REQUIRED key breaks the load of every
    // target document that predates it (✔MEASURED: 90 synthetic target
    // documents in `test_target_schema.cpp` alone), which is the mechanical
    // cost §A.1b warns a new REQUIRE-ALL role imposes. The absence is made
    // safe at the GATE instead, where it fails CLOSED.
    //
    // ⚠ THE VALUE IS *NOT* CROSS-CHECKED AGAINST THE SHIPPED TARGET
    // INVENTORY, AND MUST NOT BE. That inventory records what is
    // IMPLEMENTED, never what is POSSIBLE (plan 06 §5.1 B.12-CORRECTED),
    // so validating against it would refuse a language that legitimately
    // declares an architecture for which no target has shipped yet. Only
    // the SHAPE is checked here; the vocabulary is open by construction,
    // which is exactly what lets a new target satisfy an existing language
    // binding with no C++ and no language-document edit.
    if (target.contains("isa")) {
        json const& isaNode = target.at("isa");
        if (!isaNode.is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/target/isa",
                      "'isa' must be a string naming the instruction-set "
                      "architecture this target executes (e.g. \"x86_64\", "
                      "\"aarch64\").");
            return std::unexpected(std::move(coll).release());
        }
        auto isaText = isaNode.get<std::string>();
        // Same shape rule `name` above gets, for the same reason and with a
        // sharper consequence: the gate compares this string for EQUALITY,
        // so `" aarch64"` would match nothing, and an ISA-bound language
        // would be refused on the very target that implements it — with a
        // message showing two values that look identical.
        if (isaText.empty() || allWhitespace(isaText)
            || hasLeadingTrailingWS(isaText)) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/target/isa",
                      "'isa' must be a non-empty string with no leading or "
                      "trailing whitespace — the language↔target gate "
                      "compares it for exact equality, so a padded value "
                      "matches nothing while LOOKING correct. OMIT the key "
                      "entirely to declare no architecture.");
            return std::unexpected(std::move(coll).release());
        }
        data.isa = std::move(isaText);
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
    // The CIE's `return_address_register` — the other half of the psABI
    // DWARF numbering whose per-register half is `/registers/N/dwarfNumber`.
    // Declared rather than derived because on x86_64 SysV it is column 16,
    // a synthetic column no register row can carry (see the field docblock
    // in `TargetSchemaData`). The two halves are cross-checked for presence
    // in `validate()`; here we only read.
    if (target.contains("dwarfReturnAddressColumn")) {
        std::uint16_t ra = 0;
        auto const before = coll.size();
        readBoundedInt(target, coll, "/target", "dwarfReturnAddressColumn", ra);
        if (coll.size() == before) data.dwarfReturnAddressColumn = ra;
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
            // ⚠ THIS ROW IS PARSED BY TWO FILES: `name`/`kind` by the shared
            // `substrate::loadRelocationsTable`, the rest by this extension
            // lambda. The closed set must therefore be the UNION, and it
            // belongs HERE rather than in the substrate — `ObjectFormatSchema`
            // drives the same substrate loader with a DIFFERENT extension set,
            // so a set placed there would reject the other family's keys.
            static constexpr std::array<std::string_view, 8> kRelocationKeys{
                "name", "kind", "formula", "widthBytes", "pcRelative",
                "addendBias", "tls", "imageRelative"};
            DSS_CHECK_KEY_VOCABULARY(kRelocationKeys);
            rejectUnknownKeys(r, kRelocationKeys,
                              std::format("/relocations/{}", i),
                              "a relocation row", c);
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
            // TLS C1 (D-CSUBSET-THREAD-LOCAL): `tls` — marks the kind
            // as thread-local-offset-bearing (its patched value is a
            // tpoff, not a VA). Consumed by the walker's TLS symbol ⟺
            // tls-kind cross-check (both directions).
            if (r.contains("tls")) {
                if (!r.at("tls").is_boolean()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/tls", i),
                           "'tls' must be a boolean");
                    return false;
                }
                info.tls = r.at("tls").get<bool>();
            }
            // D-LK-PE-OBJ-ARM-CARRIES-NO-UNWIND-INFO: `imageRelative`
            // — the patched value is the target's address MINUS the
            // image base (an RVA), not an absolute VA. The one
            // property that separates PE's `IMAGE_REL_AMD64_ADDR32NB`
            // from `IMAGE_REL_AMD64_ADDR32`; both are 32-bit Linear
            // absolute rows with a zero bias, so without it a walker
            // asking for "the RVA relocation" is choosing by table
            // order. The coherence rules (imageRelative excludes
            // pcRelative and tls) live in `TargetSchema::validate()`,
            // which a programmatically-built schema also reaches.
            if (r.contains("imageRelative")) {
                if (!r.at("imageRelative").is_boolean()) {
                    c.emit(DiagnosticCode::C_MalformedJson,
                           std::format("/relocations/{}/imageRelative", i),
                           "'imageRelative' must be a boolean");
                    return false;
                }
                info.imageRelative = r.at("imageRelative").get<bool>();
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
            // ⚠ THIS USED TO BE A SECOND COPY OF THE VOCABULARY — seventeen
            // spellings hand-listed here, in the same order as
            // `kTargetCondCodeTable`, with a THIRD copy inside the refusal
            // message below. `data.condCodeEncoding[idx]` indexes by ENUM
            // ORDINAL, so the two copies also had to agree on ORDER, and
            // nothing checked that either
            // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
            static constexpr auto kCondNames = allNames(kTargetCondCodeTable);
            DSS_CHECK_KEY_VOCABULARY(kCondNames);
            // ★ THE ORDER IS LOAD-BEARING, so it is ASSERTED rather than
            // assumed: `idx` is used as an enum ordinal two statements down.
            // The check names ENUMERATORS, never spellings, so it is not a
            // fourth copy of the vocabulary.
            static_assert([] {
                for (std::size_t i = 0; i < kTargetCondCodeTable.rows.size(); ++i) {
                    if (static_cast<std::size_t>(kTargetCondCodeTable.rows[i].first) != i) {
                        return false;
                    }
                }
                return true;
            }(), "condCodeEncoding indexes by ENUM ORDINAL, so kTargetCondCodeTable "
                 "row i must declare ordinal i — reordering the table would "
                 "silently write every encoding into the wrong slot");
            // The INTEGER codes are the REQUIRED prefix; the float codes are
            // optional. Derived from the first float enumerator's own ordinal,
            // never a literal 10 — a new integer code inserted before `Fogt`
            // moves this boundary automatically.
            constexpr std::size_t kRequiredCount =
                static_cast<std::size_t>(TargetCondCode::Fogt);
            std::array<bool, kTargetCondCodeCount> seen{};
            for (auto it = cc.begin(); it != cc.end(); ++it) {
                auto const& key = it.key();
                // A `$`-prefixed key here is PROSE, not a condition. This
                // loop matches names against a closed table, so it needs the
                // same carve-out every other closed vocabulary gets —
                // without it a `$comment` documenting the table would be
                // rejected as an unknown cond code.
                //
                // ⓘ NOT ROUTED THROUGH `rejectUnknownKeys`, and that is a
                // judgement rather than an oversight: this is a name→INDEX
                // lookup whose "not found" arm IS the unknown-key report, and
                // the index it produces is consumed two statements down. A
                // shared pre-pass would walk the object twice to learn what
                // this loop already knows. The carve-out is the shared
                // predicate, which is the half that was ever gotten wrong.
                if (detail::isDocumentationKey(key)) continue;
                std::size_t idx = kCondNames.size();
                for (std::size_t i = 0; i < kCondNames.size(); ++i) {
                    if (kCondNames[i] == key) { idx = i; break; }
                }
                if (idx >= kCondNames.size()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/condCodeEncoding/{}", key),
                              // ⚠ THE SET WAS UNQUOTED HERE, which is why no
                              // quoted-token census could see it. The renderer
                              // quotes each spelling; a bare slash-joined run
                              // is invisible to every instrument this project
                              // has, and to every test that reads a message
                              // back.
                              std::format("unknown cond-code key '{}' — "
                                          "accepted: {}", key,
                                          detail::renderAllowedList(kCondNames,
                                                                    " / ")));
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
                          // The COUNT is derived too: "ALL 10" was a fourth
                          // copy of the same fact, and a new integer code
                          // would have left it counting to the old number.
                          std::format("missing cond-code(s) {} — when "
                                      "the table is declared, ALL {} "
                                      "integer entries must be present "
                                      "so the encoder cannot silently "
                                      "default to 0 for an absent code "
                                      "(the float codes are optional — "
                                      "absence selects the composed "
                                      "FCmp realization)", list,
                                      kRequiredCount));
            } else {
                data.condCodeEncodingLoaded = true;
            }
        }
    }

    // ── aggregateLayout (FC6, D-FF3-1 layout half): the per-ABI struct/union/
    //    array layout params the generic `type_layout` engine reads. REQUIRED on
    //    a register-machine target — a silent default would bake a wrong alignment
    //    rule into every aggregate (mirrors the format's required `dataModel`). ──
    // D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET: the key
    // table is declared BEFORE the shape sentence, because the shape sentence
    // renders it. It used to sit inside the `else` arm below and the sentence
    // above it named `{ scalarAlignment, maxAlignment }` — TWO of the THREE keys
    // this block accepts, `bitFieldStrategy` silently absent. A message narrower
    // than its own check tells an author, by name, that a key the loader takes
    // is not allowed.
    static constexpr std::array<std::string_view, 3> kAggregateLayoutKeys{
        "scalarAlignment", "maxAlignment", "bitFieldStrategy"};
    DSS_CHECK_KEY_VOCABULARY(kAggregateLayoutKeys);
    if (doc.contains("aggregateLayout")) {
        auto const& al = doc.at("aggregateLayout");
        if (!al.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/aggregateLayout",
                      std::format("must be an object {{ {} }}",
                                  detail::renderAllowedList(
                                      kAggregateLayoutKeys, ", ")));
        } else {
            rejectUnknownKeys(al, kAggregateLayoutKeys, "/aggregateLayout",
                              "the aggregate-layout block", coll);
            bool ok = true;
            if (!al.contains("scalarAlignment")
                || !al.at("scalarAlignment").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/aggregateLayout/scalarAlignment",
                          std::format("missing required 'scalarAlignment' "
                                      "string — accepted: {}",
                                      detail::renderAllowedList(
                                          allNames(kScalarAlignmentRuleTable),
                                          " / ")));
                ok = false;
            } else {
                auto const name = al.at("scalarAlignment").get<std::string>();
                auto const rule = scalarAlignmentRuleFromName(name);
                if (!rule) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/aggregateLayout/scalarAlignment",
                              std::format("unknown scalarAlignment '{}' — "
                                          "accepted: {}", name,
                                          detail::renderAllowedList(
                                              allNames(kScalarAlignmentRuleTable),
                                              " / ")));
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
            //
            // ★ THE WHOLE TABLE, sentinel included, and that is not the same
            // answer the FORMAT loader gives. A TARGET may write `none`: it
            // means what omitting the key means, and this check accepts it. A
            // FORMAT may not, because the format's value falls back to THIS one
            // when absent, so `none` there would be ambiguous. Each message
            // states its OWN site's accepted set — which is the property the
            // projection exists to keep true.
            // ⚠ ✔MEASURED 2026-08-20: this refusal used to read
            // `(expected "gnu_packed")` — ONE of the THREE spellings the very
            // next line accepts. A config author declaring the msvc_straddle
            // rule was told by name that the spelling the loader takes is not
            // allowed (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
            if (al.contains("bitFieldStrategy")) {
                if (!al.at("bitFieldStrategy").is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/aggregateLayout/bitFieldStrategy",
                              std::format("must be a string — accepted: {}",
                                          detail::renderAllowedList(
                                              allNames(kBitFieldStrategyTable),
                                              " / ")));
                    ok = false;
                } else {
                    auto const name = al.at("bitFieldStrategy").get<std::string>();
                    auto const strat = bitFieldStrategyFromName(name);
                    if (!strat) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/aggregateLayout/bitFieldStrategy",
                                  std::format("unknown bitFieldStrategy '{}' — "
                                              "accepted: {}", name,
                                              detail::renderAllowedList(
                                                  allNames(kBitFieldStrategyTable),
                                                  " / ")));
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

    // ── charIsUnsigned (TF-C56 D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET
    //     + TF-C75 D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM) ────────────────
    //
    // THE SINGLE SOURCE OF TRUTH for bare-`char` signedness — the whole
    // (processor × platform) fact in ONE key on the ONE file that owns it:
    //
    //     "charIsUnsigned": {
    //       "default": true,
    //       "byObjectFormat": { "macho": false, "pe": false }
    //     }
    //
    // `default` is the PROCESSOR's answer; `byObjectFormat` overrides it for
    // the platforms that fix the answer for every CPU they serve (Darwin and
    // Windows both chose SIGNED). OPTIONAL as a whole; ABSENT ⇒ default false
    // = signed everywhere, which is the C-common answer and is CORRECT on
    // every format x86_64 serves (x86_64.target.json therefore omits the key,
    // exactly as before this cycle).
    //
    // ★ WHEN PRESENT THE KEY MUST BE THE OBJECT FORM — a bare boolean is
    // REJECTED, not accepted as a shorthand. This is deliberate and is the
    // point of the reshape: `"charIsUnsigned": true` READS as "char is
    // unsigned on this target, full stop", and that sentence is exactly the
    // falsehood TF-C75 exists to delete (it was true for aarch64-linux and
    // silently wrong for arm64-darwin). `{"default": true}` reads as "unsigned
    // unless a format says otherwise", which is what the value actually means.
    // Accepting both shapes would also leave the bare-bool form with ZERO
    // shipped users — accepted config surface nothing exercises.
    //
    // ★ `default` is REQUIRED inside the object. An object carrying only
    // `byObjectFormat` would leave every unlisted format resolving to an
    // implicit `false` that no file states — a silent fallback on the one axis
    // whose two answers are opposite high-bit extensions.
    //
    // ★ EVERY `byObjectFormat` KEY IS VALIDATED through
    // `objectFormatKindFromName`. The nearby `wideFloatSoftcallLibraryByFormat`
    // precedent does NOT do this (it stores arbitrary strings), and inheriting
    // that hole here would be fatal: `"machO"` would silently mean NO ENTRY →
    // silent fallback to the default → the exact miscompile this cycle closes.
    // The sentinel spelling `"unknown"` is likewise rejected — it SPELLS
    // correctly, so the name lookup succeeds, but it selects no real format
    // (the shared `kObjectFormatKindSentinelRejection` discipline, identical
    // to `wideFloatSoftcallLibraryByFormat` below).
    if (doc.contains("charIsUnsigned")) {
        auto const& cu = doc.at("charIsUnsigned");
        if (!cu.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/charIsUnsigned",
                      "'charIsUnsigned' must be an OBJECT — "
                      R"({"default": <bool>, "byObjectFormat": {"macho": <bool>, …}}. )"
                      "A bare boolean is rejected on purpose: it asserts one "
                      "signedness for every platform this processor serves, "
                      "which is the claim that made bare `char` sign-extend "
                      "wrongly on Apple arm64. Write {\"default\": <bool>} if "
                      "the answer really is uniform.");
        } else {
            // Closed inner-key vocabulary: a misspelled `"defualt"` would
            // otherwise leave the required-key check firing on a file that
            // plainly meant to declare one, or (worse, once `default` is
            // present) silently drop a `"byObjectFormt"` override map.
            static constexpr std::array<std::string_view, 2>
                kCharIsUnsignedKeys{"default", "byObjectFormat"};
            DSS_CHECK_KEY_VOCABULARY(kCharIsUnsignedKeys);
            rejectUnknownKeys(cu, kCharIsUnsignedKeys, "/charIsUnsigned",
                              "the 'charIsUnsigned' block", coll);

            if (!cu.contains("default")) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/charIsUnsigned/default",
                          "'charIsUnsigned' must state its 'default' — the "
                          "processor's answer for every object format that "
                          "declares no override. Omitting it would leave those "
                          "formats resolving to a signedness no file states.");
            } else if (!cu.at("default").is_boolean()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/charIsUnsigned/default",
                          "'default' must be a boolean (true = bare `char` is "
                          "UNSIGNED)");
            } else {
                data.charIsUnsignedDefault = cu.at("default").get<bool>();
            }

            if (cu.contains("byObjectFormat")) {
                auto const& byFmt = cu.at("byObjectFormat");
                if (!byFmt.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/charIsUnsigned/byObjectFormat",
                              "'byObjectFormat' must be an object mapping "
                              "object-format kind names to booleans");
                } else {
                    for (auto it = byFmt.begin(); it != byFmt.end(); ++it) {
                        if (detail::isDocumentationKey(it.key())) continue;
                        auto const path = std::format(
                            "/charIsUnsigned/byObjectFormat/{}", it.key());
                        auto const kind = objectFormatKindFromName(it.key());
                        if (!kind.has_value()) {
                            coll.emit(DiagnosticCode::C_MalformedJson, path,
                                      std::format(
                                          "'{}' is not a recognized "
                                          "object-format kind (expected one of "
                                          "{}). An unrecognized name would "
                                          "declare an override that never "
                                          "fires, silently leaving the default "
                                          "in place.", it.key(),
                                          detail::renderAllowedList(
                                              kSelectableObjectFormatKindNames,
                                              " / ")));
                            continue;
                        }
                        if (!isSelectableObjectFormatKind(*kind)) {
                            coll.emit(DiagnosticCode::C_MalformedJson, path,
                                      std::string{
                                          kObjectFormatKindSentinelRejection});
                            continue;
                        }
                        if (!it.value().is_boolean()) {
                            coll.emit(DiagnosticCode::C_MalformedJson, path,
                                      "override must be a boolean (true = bare "
                                      "`char` is UNSIGNED on this format)");
                            continue;
                        }
                        auto const idx = static_cast<std::size_t>(*kind);
                        data.charIsUnsignedByFormat[idx] =
                            it.value().get<bool>();
                        data.charIsUnsignedByFormatDeclared[idx] = true;
                    }
                }
            }
        }
    }

    // ── predefinedMacros (TF-C74 — per-architecture identity macros) ──
    // The macros that identify this CPU ARCHITECTURE to the preprocessor
    // (`__aarch64__`, `__x86_64__`, …). Declared HERE, next to the other
    // per-target language-affecting semantics (`charIsUnsigned` above,
    // `aggregateLayout`, `tls`, `callingConventions`) — never on the
    // language, which must not enumerate CPU architectures.
    //
    // The per-entry grammar is the SHARED parser the language loader uses
    // (`parsePredefinedMacroArray`), so the closed `kind` verb set, the
    // Constant⇒`value` rule, the function-like `params` checks and the
    // `availableObjectFormats` validation are inherited rather than
    // re-implemented. OPTIONAL; absent ⇒ no target predefines ⇒ the
    // preprocessor's effective list is byte-identical to today's.
    // Malformed entries emit `C_MalformedJson` (this family's code for a
    // structurally-wrong value, as `charIsUnsigned`/`tls` above do);
    // MISSING required fields emit the universal `C_MissingField`.
    if (doc.contains("predefinedMacros")) {
        json const& pms = doc.at("predefinedMacros");
        if (!pms.is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/predefinedMacros",
                      "'predefinedMacros' must be an array");
        } else {
            detail::parsePredefinedMacroArray(
                pms, "/predefinedMacros", DiagnosticCode::C_MalformedJson,
                coll, data.predefinedMacros);
        }
    }

    // ── defaultAssemblyLanguage (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET) ──
    //
    // The NAME of the shipped source-language document that spells this
    // processor's assembly — a `<stem>` for `GrammarSchema::loadShipped`,
    // exactly what `--language` takes. OPTIONAL; absent ⇒ the target declares
    // none and any build that would have needed it fails loud naming the
    // target (the driver's job, not the loader's — a target with no assembly
    // dialect is a legitimate state, not a malformed document).
    //
    // A PRESENT key is strict on both axes a lying knob could hide behind:
    // non-string, and the empty string. `""` is rejected rather than treated
    // as absent because the two states are operator-distinguishable — absent
    // says "this target has no dialect", `""` says "I meant to name one" — and
    // silently folding the second into the first is how a config key stops
    // meaning anything. The loader deliberately does NOT check that the named
    // language EXISTS or that it claims `.s`: resolving a language name is the
    // grammar loader's job, and duplicating it here would put a second,
    // drifting copy of language discovery in the target family. An unloadable
    // name fails loud at the driver with the language loader's own diagnostic.
    if (doc.contains("defaultAssemblyLanguage")) {
        json const& dal = doc.at("defaultAssemblyLanguage");
        if (!dal.is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/defaultAssemblyLanguage",
                      "'defaultAssemblyLanguage' must be a string naming a "
                      "shipped source language (the `<stem>` of "
                      "src/dss-config/sources/<stem>.lang.json, e.g. "
                      "\"asm-x86_64-att\")");
        } else if (dal.get<std::string>().empty()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/defaultAssemblyLanguage",
                      "'defaultAssemblyLanguage' must not be empty — OMIT the "
                      "key to declare that this target has no assembly "
                      "dialect; an empty string reads as a name that was meant "
                      "to be filled in");
        } else {
            data.defaultAssemblyLanguage = dal.get<std::string>();
        }
    }

    // ── tls identity (TLS C1, D-CSUBSET-THREAD-LOCAL — optional) ──
    // The target's static-TLS layout convention: `"tls": { "variant":
    // "variant1"|"variant2", "tcbHeaderBytes": N }`. OPTIONAL like
    // `aggregateLayout` — absence IS the capability signal (the walker's
    // tpoff helper fails loud on a TLS symbol under a tls-less target).
    // A PRESENT block is strict: `variant` is REQUIRED closed-enum
    // (an unknown spelling must never silently pick a variant — the
    // two variants produce opposite-signed tpoffs, a silent-miscompile
    // axis); `tcbHeaderBytes` is optional (default 0, the Variant-II
    // value) but must be a non-negative integer when present.
    // D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET: declared
    // ahead of the shape sentence, which renders it rather than retyping it.
    static constexpr std::array<std::string_view, 2> kTlsKeys{
        "variant", "tcbHeaderBytes"};
    DSS_CHECK_KEY_VOCABULARY(kTlsKeys);
    if (doc.contains("tls")) {
        auto const& tb = doc.at("tls");
        if (!tb.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/tls",
                      std::format("'tls' must be an object with the keys {} "
                                  "('variant' is one of {})",
                                  detail::renderAllowedList(kTlsKeys, ", "),
                                  detail::renderAllowedList(
                                      allNames(kTlsVariantTable), " / ")));
        } else {
            rejectUnknownKeys(tb, kTlsKeys, "/tls", "the tls block", coll);
            TlsIdentity tlsId{};
            bool ok = true;
            if (!tb.contains("variant") || !tb.at("variant").is_string()) {
                // ★ THE PER-VALUE PROSE SURVIVES THE PROJECTION. The two
                // spellings are still named individually — through
                // `tlsVariantName`, not as literals — because WHICH variant a
                // target is cannot be guessed from the name alone, and the
                // sign of the tpoff is the whole reason no default is safe.
                coll.emit(DiagnosticCode::C_MissingField, "/tls/variant",
                          std::format("'tls.variant' is required and must be a "
                                      "string — accepted: {} ('{}' = "
                                      "tp-at-TCB-head positive tpoff [arm64]; "
                                      "'{}' = tp-past-block-end negative tpoff "
                                      "[x86_64]) — the two produce "
                                      "opposite-signed offsets, so no default "
                                      "is safe",
                                      detail::renderAllowedList(
                                          allNames(kTlsVariantTable), " / "),
                                      tlsVariantName(TlsVariant::Variant1),
                                      tlsVariantName(TlsVariant::Variant2)));
                ok = false;
            } else {
                auto const name = tb.at("variant").get<std::string>();
                auto const v = tlsVariantFromName(name);
                if (!v.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, "/tls/variant",
                              std::format("unknown tls variant '{}' — "
                                          "accepted: {}", name,
                                          detail::renderAllowedList(
                                              allNames(kTlsVariantTable),
                                              " / ")));
                    ok = false;
                } else {
                    tlsId.variant = *v;
                }
            }
            if (tb.contains("tcbHeaderBytes")) {
                if (!tb.at("tcbHeaderBytes").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tls/tcbHeaderBytes",
                              "'tcbHeaderBytes' must be a non-negative "
                              "integer");
                    ok = false;
                } else {
                    std::int64_t const n =
                        tb.at("tcbHeaderBytes").get<std::int64_t>();
                    if (n < 0 || n > 0xFFFFFFFFLL) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/tls/tcbHeaderBytes",
                                  std::format("'tcbHeaderBytes' ({}) must "
                                              "fit a non-negative u32", n));
                        ok = false;
                    } else {
                        tlsId.tcbHeaderBytes = static_cast<std::uint32_t>(n);
                    }
                }
            }
            if (ok) data.tls = tlsId;
        }
    }

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
        static constexpr std::array<std::string_view, 14> kOpcodeKeys{
            "mnemonic", "result", "hasSideEffects", "requires2Address",
            "twoAddressSourceOperand",
            "isCall", "terminatorKind", "minOperands", "maxOperands",
            "minSuccessors", "maxSuccessors", "encoding", "lowering",
            "implicitRegisters"};
        DSS_CHECK_KEY_VOCABULARY(kOpcodeKeys);
        rejectUnknownKeys(o, kOpcodeKeys, std::format("/opcodes/{}", i),
                          "an opcode row", coll);
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
        auto const rr = targetResultRuleFromName(o.at("result").get<std::string>());
        if (!rr.has_value()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/result", i),
                      std::format("expected {}",
                                  detail::renderAllowedList(
                                      allNames(kTargetResultRuleTable), " / ")));
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
        // ── 2-address tie: the FLAG plus the OPTIONAL operand INDEX ──
        // `requires2Address: true` alone means the historical shape,
        // *result == operand[0]*, and every shipped opcode spells it that
        // way. `twoAddressSourceOperand: <j>` refines WHICH operand the
        // result is tied to (D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE).
        //
        // ★ THE INDEX IS A SEPARATE KEY RATHER THAN AN INTEGER-VALUED
        // `requires2Address`, AND THAT IS THE WHOLE SAFETY ARGUMENT. A
        // polymorphic `requires2Address: 1` would be read by a human as
        // "true" and by the loader as "tie to operand 1" — a divergence
        // between what the author meant and what the assembler encodes,
        // with no diagnostic. Two keys with an ENFORCED dependency cannot
        // drift (they collapse into the one in-memory field below), and
        // neither spelling can be misread as the other.
        //
        // ⚠ AND THE FLAG IS REJECTED WHEN IT IS NOT A BOOLEAN, which the
        // pre-index loader merely IGNORED. `"requires2Address": 1` used to
        // read as "not two-address" and silently skip legalization — an
        // encoding bug with no diagnostic — and now it is also the most
        // likely way an author would try to spell the index. Both readings
        // are wrong; say so instead of picking one.
        if (o.contains("requires2Address")
            && !o.at("requires2Address").is_boolean()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/requires2Address", i),
                      std::format("opcode '{}': 'requires2Address' must be a "
                                  "boolean. To tie the result to an operand "
                                  "other than 0, keep "
                                  "'requires2Address': true and add "
                                  "'twoAddressSourceOperand': <index>",
                                  info.mnemonic));
        }
        bool const twoAddressDeclared =
            o.contains("requires2Address")
            && o.at("requires2Address").is_boolean()
            && o.at("requires2Address").get<bool>();
        if (twoAddressDeclared) info.requires2Address = std::uint8_t{0};
        if (o.contains("twoAddressSourceOperand")) {
            auto const& tsoNode = o.at("twoAddressSourceOperand");
            if (!tsoNode.is_number_unsigned()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/twoAddressSourceOperand", i),
                          std::format("opcode '{}': 'twoAddressSourceOperand' "
                                      "must be a non-negative integer — it is "
                                      "the INDEX of the operand the result is "
                                      "tied to, not a flag",
                                      info.mnemonic));
            } else if (!twoAddressDeclared) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/twoAddressSourceOperand", i),
                          std::format("opcode '{}': 'twoAddressSourceOperand' "
                                      "requires 'requires2Address: true' — an "
                                      "index with no tie declared would be read "
                                      "by nothing, so the operand the author "
                                      "meant to tie would silently stay free",
                                      info.mnemonic));
            } else {
                auto const idx = tsoNode.get<std::uint64_t>();
                if (idx > 0xFFu) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/twoAddressSourceOperand", i),
                              std::format("opcode '{}': 'twoAddressSourceOperand' "
                                          "{} does not fit an operand index",
                                          info.mnemonic, idx));
                } else {
                    info.requires2Address = static_cast<std::uint8_t>(idx);
                }
            }
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
                // ⚠ THE RETYPED LIST THIS REPLACES OMITTED `indirect-br`,
                // which the table has carried since the computed-goto work —
                // accepted by the parse, denied by both sentences.
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/terminatorKind", i),
                          std::format("'terminatorKind' must be a string (one "
                                      "of {})",
                                      detail::renderAllowedList(
                                          allNames(kTargetTerminatorKindTable),
                                          " / ")));
            } else {
                auto const tk = targetTerminatorKindFromName(tkNode.get<std::string>());
                if (tk.has_value()) {
                    info.terminatorKind = *tk;
                } else {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/terminatorKind", i),
                              std::format("expected {}",
                                          detail::renderAllowedList(
                                              allNames(kTargetTerminatorKindTable),
                                              " / ")));
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
            static constexpr std::array<std::string_view, 3> kEncodingKeys{
                "format", "variants",
                // D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD: the register
                // bank this opcode's fields draw from unless a field overrides
                // it. validate() proves every register-bearing field resolves.
                "registerClass"};
            DSS_CHECK_KEY_VOCABULARY(kEncodingKeys);
            rejectUnknownKeys(enc, kEncodingKeys,
                              std::format("/opcodes/{}/encoding", i),
                              "an encoding block", coll);
            if (!enc.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/encoding", i),
                          "'encoding' must be an object");
            } else if (!enc.contains("format")) {
                coll.emit(DiagnosticCode::C_MissingField,
                          std::format("/opcodes/{}/encoding/format", i),
                          std::format("missing 'format' (required when an "
                                      "'encoding' block is present; one of {})",
                                      detail::renderAllowedList(
                                          allNames(kTargetEncodingShapeTable),
                                          " / ")));
            } else {
                auto const& fmt = enc.at("format");
                if (!fmt.is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/opcodes/{}/encoding/format", i),
                              std::format("'format' must be a string (one "
                                          "of {})",
                                          detail::renderAllowedList(
                                              allNames(kTargetEncodingShapeTable),
                                              " / ")));
                } else {
                    auto const shape =
                        targetEncodingShapeFromName(fmt.get<std::string>());
                    if (shape.has_value()) {
                        info.encoding.shape = *shape;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/opcodes/{}/encoding/format", i),
                                  std::format("expected {}",
                                              detail::renderAllowedList(
                                                  allNames(kTargetEncodingShapeTable),
                                                  " / ")));
                    }
                }
                // D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD: the
                // opcode-wide register bank. Read BEFORE the variants so a
                // malformed spelling is reported once, at the opcode, rather
                // than once per field that would have inherited it.
                info.encoding.registerClass = parseRegClassField(
                    enc, "registerClass",
                    std::format("/opcodes/{}/encoding/registerClass", i), coll);
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
        // Instruction-SEQUENCE facet
        // (D-TARGET-ENCODING-TABLE-EXPRESSES-ONLY-THE-DEGENERATE-SEQUENCE).
        // Optional per-opcode block declaring the machine instructions this
        // operation expands into. Shape-only here; step mnemonics resolve to
        // opcode indexes in the post-pass below (the mnemonic index is not
        // complete until every opcode row has been read).
        if (o.contains("lowering")) {
            parseLoweringBlock(o.at("lowering"), i, info.lowering, coll);
        }
        // Implicit-register-constraint (cycle 10p substrate,
        // 2026-06-04). Optional per-opcode block. Field-shape rejects
        // here; name→ordinal resolution + cross-field rejects happen
        // post-register-load (see "Implicit-register-constraint
        // resolution + validation" block lower in this function).
        // See `ImplicitRegisterConstraint` docblock in
        // target_schema.hpp for the full contract.
        // ⚠ ✔MEASURED 2026-08-20: the shape sentence below named THREE of the
        // FIVE keys `kImplicitRegisterKeys` accepts — `inputRoles` and
        // `outputRoles` were read by the parse arm and advertised by nothing,
        // so an author declaring either was told BY NAME that a key the loader
        // takes is not a field of this block. The table is declared ahead of
        // the sentence and the sentence RENDERS it, which is the only shape in
        // which the two cannot disagree
        // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
        // ⓘ NOT the same set as the "empty block" refusal further down: that
        // one names `inputs`/`outputs`/`clobbered` because those are the three
        // its own emptiness check reads — `inputRoles`/`outputRoles` are role
        // MAPS over names declared in those three and cannot make a block
        // non-empty on their own. A message states ITS OWN check's set.
        static constexpr std::array<std::string_view, 5>
            kImplicitRegisterKeys{"inputs", "outputs", "clobbered",
                                  "inputRoles", "outputRoles"};
        DSS_CHECK_KEY_VOCABULARY(kImplicitRegisterKeys);
        if (o.contains("implicitRegisters")) {
            auto const& ir = o.at("implicitRegisters");
            if (!ir.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/implicitRegisters", i),
                          std::format("'implicitRegisters' must be an object "
                                      "with optional {} fields",
                                      detail::renderAllowedList(
                                          kImplicitRegisterKeys, ", ")));
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
                // ⚠ THIS LOOP USED TO BE HAND-ROLLED AND REJECTED `$comment`.
                // It predates the shared helper and never grew the `$`-prefix
                // carve-out, so a prose key here was reported as a typo — the
                // INVERSE failure, and the reason `rejectUnknownKeys` applies
                // the carve-out itself rather than trusting each call site.
                rejectUnknownKeys(ir, kImplicitRegisterKeys,
                                  std::format("/opcodes/{}/implicitRegisters", i),
                                  "an implicitRegisters block", coll);
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
                // Role maps (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT):
                // each is an OBJECT of role → register
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
                        // The keys here are ROLE NAMES (identifiers), so this
                        // map must stay open — its vocabulary is checked later
                        // against `kKnownImplicitRegisterRoles`. But a
                        // `$`-prefixed PROSE key would be captured as a role
                        // and then rejected downstream as an unknown one,
                        // which reads as a config error instead of a comment.
                        if (detail::isDocumentationKey(it.key())) continue;
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
                static constexpr std::array<std::string_view, 6> kRegisterKeys{
                    "name", "class", "subOf", "widthBytes", "hwEncoding",
                    "dwarfNumber"};
                DSS_CHECK_KEY_VOCABULARY(kRegisterKeys);
                rejectUnknownKeys(r, kRegisterKeys,
                                  std::format("/registers/{}", i),
                                  "a register row", coll);
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
                                  std::format("expected {}",
                                              detail::renderAllowedList(
                                                  allNames(kTargetRegClassTable),
                                                  " / ")));
                        continue;
                    }
                }
                if (r.contains("subOf") && r.at("subOf").is_string()) {
                    info.subOf = r.at("subOf").get<std::string>();
                }
                std::string const regPath = std::format("/registers/{}", i);
                readBoundedInt(r, coll, regPath, "widthBytes", info.widthBytes);
                readBoundedInt(r, coll, regPath, "hwEncoding", info.hwEncoding);
                // The psABI DWARF number. ABSENT is a real state (a narrow
                // view, or AArch64's `xzr` which has no DWARF number) — see
                // `TargetRegisterInfo::dwarfNumber`. `readBoundedInt` writes
                // through a reference and cannot express "absent", so the
                // presence test is explicit and the scratch value is only
                // committed once the read succeeded.
                if (r.contains("dwarfNumber")) {
                    std::uint16_t dw = 0;
                    auto const before = coll.size();
                    readBoundedInt(r, coll, regPath, "dwarfNumber", dw);
                    if (coll.size() == before) info.dwarfNumber = dw;
                }

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

    // ── asmConstraints (GNU inline-asm constraint letters — optional) ──
    //
    // Which LETTER means which register / register class / operand form on
    // THIS processor. See `TargetAsmConstraint` in the header for why this is
    // target vocabulary rather than a C++ table keyed on architecture, and
    // for the line that separates it from the reverted `asmSyntax` block
    // ([[D-CONFIG-ASM-DIALECT-DECLARED-AS-TARGET-VOCABULARY]]).
    //
    // ★ PARSED AFTER `registers` ON PURPOSE — a `binds: "register"` row
    // resolves its NAME to an ORDINAL right here, so a dangling name is a
    // load error rather than a lookup that fails much later at a site with no
    // target in hand. Same precedent as the `implicitRegisters` roles and the
    // wide-float softcall registers.
    //
    // OPTIONAL. A target declaring none refuses every constraint letter by
    // name, which is the correct answer for a processor whose inline-asm
    // binding has not been described — not a malformed document.
    if (doc.contains("asmConstraints")) {
        // Rendered FROM THE TABLE at each use, never re-typed. The comment on
        // `kKnownImplicitRegisterRoles` records what re-typing costs: a
        // diagnostic that told the reader their valid role was invalid.
        auto const renderNames = [](auto const& table) {
            std::string out;
            for (auto const& row : table.rows) {
                if (!out.empty()) out += ", ";
                out += '\'';
                out += row.second;
                out += '\'';
            }
            return out;
        };
        if (!doc.at("asmConstraints").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/asmConstraints",
                      "'asmConstraints' must be an array of constraint-letter "
                      "rows");
        } else {
            auto const& rows = doc.at("asmConstraints");
            data.asmConstraints.reserve(rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i) {
                auto const& r = rows[i];
                auto const path = std::format("/asmConstraints/{}", i);
                if (!r.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "asmConstraints entry must be an object");
                    continue;
                }
                // ★ THE PAYLOAD KEYS ARE THE `binds` SPELLINGS. Adding an
                // axis to `kAsmConstraintBindingTable` therefore adds its key
                // here too — the allowlist cannot fall behind the vocabulary.
                static constexpr std::array<std::string_view, 5>
                    kAsmConstraintKeys{"letter", "binds", "registerClass",
                                       "register", "operandKind"};
                DSS_CHECK_KEY_VOCABULARY(kAsmConstraintKeys);
                static_assert(kAsmConstraintKeys.size()
                                  == kAsmConstraintBindingTable.rows.size() + 2,
                              "every `binds` spelling must also be an accepted "
                              "payload key (plus 'letter' and 'binds' "
                              "themselves) — a new axis whose key is missing "
                              "here would be rejected as unknown at the exact "
                              "moment it was first used");
                rejectUnknownKeys(r, kAsmConstraintKeys, path,
                                  "an asmConstraints row", coll);

                TargetAsmConstraint entry;

                // ── letter ────────────────────────────────────────────
                if (!r.contains("letter") || !r.at("letter").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("{}/letter", path),
                              "missing or non-string 'letter' — a constraint "
                              "row must name the spelling it binds");
                    continue;
                }
                entry.letter = r.at("letter").get<std::string>();
                if (entry.letter.empty()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/letter", path),
                              "'letter' must not be empty — omit the ROW to "
                              "declare that this target binds nothing");
                    continue;
                }
                // ⚠⚠ THE ONE MISTAKE A HUMAN ACTUALLY MAKES HERE, AND THE
                // ONE THIS FACET EXISTS TO KEEP OUT. `=`/`+`/`&`/`%` are GNU
                // *asm grammar* — output, in-out, earlyclobber, commutative —
                // and they mean the same thing on every processor, so the C
                // front end owns them. Storing `"=a"` as a letter would put a
                // grammar fact in the target file, which is precisely how the
                // reverted `asmSyntax` block got in. It would ALSO silently
                // never match, because a front end that strips modifiers
                // before looking up (the only sane order) would search for
                // `a`. A silent never-match is the worst of the two, so this
                // is a hard reject with the split spelled out.
                {
                    constexpr std::string_view kModifiers = "=+&%";
                    auto const bad = entry.letter.find_first_of(kModifiers);
                    if (bad != std::string::npos) {
                        coll.emit(
                            DiagnosticCode::C_MalformedJson,
                            std::format("{}/letter", path),
                            std::format(
                                "constraint letter '{}' contains the modifier "
                                "'{}' — modifiers ({}) are GNU-asm GRAMMAR "
                                "owned by the source language and are the "
                                "same on every processor; declare the LETTER "
                                "alone (e.g. \"a\", not \"=a\") and let the "
                                "front end strip modifiers before it asks "
                                "this target",
                                entry.letter, entry.letter[bad], kModifiers));
                        continue;
                    }
                }
                // A sigil is dialect grammar too, and whitespace is always a
                // typo. ⚠ `%` is DELIBERATELY NOT LISTED HERE even though it
                // is a sigil in AT&T: it is also the commutative-operand
                // modifier, so the check above already consumed it and a
                // second listing would describe a branch this line can never
                // reach — the "comment records a fact the code does not use"
                // failure, one layer down.
                if (entry.letter.find_first_of(" \t$#") != std::string::npos) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/letter", path),
                              std::format("constraint letter '{}' contains "
                                          "whitespace or a sigil — sigils are "
                                          "DIALECT grammar (AT&T writes `$7` "
                                          "where Intel writes `7`, ONE CPU) "
                                          "and never appear in target "
                                          "vocabulary",
                                          entry.letter));
                    continue;
                }
                // ★ Duplicate letters are rejected rather than
                // last-writer-wins: two rows for `a` is an ambiguity the
                // consumer cannot see, and silently picking one is how a
                // config key stops meaning anything. CASE-SENSITIVE —
                // ✔MEASURED, x86_64 `d` is %rdx and `D` is %rdi.
                {
                    bool dup = false;
                    for (auto const& prior : data.asmConstraints) {
                        if (prior.letter == entry.letter) { dup = true; break; }
                    }
                    if (dup) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/letter", path),
                                  std::format("duplicate constraint letter "
                                              "'{}' — one letter binds one "
                                              "thing; the second row would be "
                                              "silently unreachable",
                                              entry.letter));
                        continue;
                    }
                }

                // ── binds (closed discriminator) ──────────────────────
                if (!r.contains("binds") || !r.at("binds").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("{}/binds", path),
                              std::format("constraint '{}': missing or "
                                          "non-string 'binds' — a letter must "
                                          "say WHICH AXIS it binds ({})",
                                          entry.letter,
                                          renderNames(
                                              kAsmConstraintBindingTable)));
                    continue;
                }
                auto const bindsStr = r.at("binds").get<std::string>();
                auto const binds = asmConstraintBindingFromName(bindsStr);
                if (!binds.has_value()) {
                    coll.emit(
                        DiagnosticCode::C_MalformedJson,
                        std::format("{}/binds", path),
                        std::format("constraint '{}': unknown binding axis "
                                    "'{}' — declared axes are {} (the "
                                    "three axes constraint letters actually "
                                    "split across; a new one needs core "
                                    "vocabulary, not a free-form string)",
                                    entry.letter, bindsStr,
                                    renderNames(kAsmConstraintBindingTable)));
                    continue;
                }
                entry.binds = *binds;

                // ── exactly the payload `binds` names, and no other ───
                // ⚠ The OTHER arms must be ABSENT, not merely ignored. A row
                // carrying both `register` and `registerClass` has two
                // answers and the loader would silently use one; the operator
                // who wrote the second one would never learn it was dead.
                auto const wantKey = asmConstraintBindingName(entry.binds);
                if (!r.contains(wantKey)) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("{}/{}", path, wantKey),
                              std::format("constraint '{}': binds '{}' but "
                                          "carries no '{}' key — the payload "
                                          "key IS the axis name",
                                          entry.letter, bindsStr, wantKey));
                    continue;
                }
                {
                    bool extra = false;
                    for (auto const& row : kAsmConstraintBindingTable.rows) {
                        if (row.second == wantKey) continue;
                        if (!r.contains(row.second)) continue;
                        coll.emit(
                            DiagnosticCode::C_MalformedJson,
                            std::format("{}/{}", path, row.second),
                            std::format("constraint '{}': binds '{}', so the "
                                        "'{}' key is a second answer that "
                                        "would be silently discarded — a row "
                                        "carries exactly the payload its "
                                        "'binds' names",
                                        entry.letter, bindsStr, row.second));
                        extra = true;
                    }
                    if (extra) continue;
                }

                json const& payload = r.at(wantKey);
                if (!payload.is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/{}", path, wantKey),
                              std::format("constraint '{}': '{}' must be a "
                                          "string naming the bound {}",
                                          entry.letter, wantKey, wantKey));
                    continue;
                }
                auto const payloadStr = payload.get<std::string>();

                switch (entry.binds) {
                case AsmConstraintBinding::RegisterClass: {
                    auto const cls = targetRegClassFromName(payloadStr);
                    if (!cls.has_value()) {
                        coll.emit(
                            DiagnosticCode::C_MalformedJson,
                            std::format("{}/registerClass", path),
                            std::format("constraint '{}': '{}' is not a "
                                        "register class — declared classes "
                                        "are {}",
                                        entry.letter, payloadStr,
                                        renderNames(kTargetRegClassTable)));
                        continue;
                    }
                    // `none` is a spelling in the table but not a class a
                    // letter can usefully bind: it means "no register file",
                    // so the constraint would match nothing while LOOKING
                    // declared. Reject rather than ship a dead letter.
                    if (*cls == TargetRegClass::None) {
                        coll.emit(
                            DiagnosticCode::C_MalformedJson,
                            std::format("{}/registerClass", path),
                            std::format("constraint '{}': binding to class "
                                        "'{}' declares a letter that can "
                                        "never match a register — omit the "
                                        "row instead",
                                        entry.letter,
                                        targetRegClassName(*cls)));
                        continue;
                    }
                    // ★ THE CLASS MUST ACTUALLY BE POPULATED ON THIS TARGET.
                    // ✔MEASURED: x86_64 declares 0 `vr` registers and arm64
                    // declares 0 `flags` registers, so a letter bound to an
                    // empty class is a knob that resolves to the empty set —
                    // it loads clean, reads as support, and allocates
                    // nothing. Checked against the register table that was
                    // parsed immediately above, never against a hardcoded
                    // per-architecture expectation.
                    {
                        bool populated = false;
                        for (auto const& reg : data.registers) {
                            if (reg.regClass == *cls) { populated = true; break; }
                        }
                        if (!populated) {
                            coll.emit(
                                DiagnosticCode::C_MalformedJson,
                                std::format("{}/registerClass", path),
                                std::format("constraint '{}': class '{}' is "
                                            "declared by no register in this "
                                            "target's 'registers' table, so "
                                            "the letter would resolve to the "
                                            "empty set while reading as "
                                            "supported",
                                            entry.letter, payloadStr));
                            continue;
                        }
                    }
                    entry.registerClass = *cls;
                    break;
                }
                case AsmConstraintBinding::Register: {
                    auto it = data.registerIndex.find(payloadStr);
                    if (it == data.registerIndex.end()) {
                        // ⚠ The valid list is NOT rendered here, and that is
                        // a deliberate departure from the small-vocabulary
                        // diagnostics above: ✔MEASURED, this target's table
                        // holds 65 (x86_64) / 129 (arm64) names, and pasting
                        // them turns the message into noise. Nothing is
                        // hand-typed either way — what IS rendered is the
                        // count and, when it explains the failure, the
                        // sigil-stripped name, which catches the exact
                        // grammar-into-vocabulary mistake this facet guards.
                        std::string hint;
                        if (!payloadStr.empty()
                            && std::string_view{"%$#"}.find(payloadStr.front())
                                   != std::string_view::npos
                            && data.registerIndex.find(payloadStr.substr(1))
                                   != data.registerIndex.end()) {
                            hint = std::format(
                                " — did you mean '{}'? a sigil is DIALECT "
                                "grammar and never appears in target "
                                "vocabulary", payloadStr.substr(1));
                        }
                        coll.emit(
                            DiagnosticCode::C_MalformedJson,
                            std::format("{}/register", path),
                            std::format("constraint '{}': names register "
                                        "'{}', which is not one of the {} "
                                        "registers this target declares{}",
                                        entry.letter, payloadStr,
                                        data.registers.size(), hint));
                        continue;
                    }
                    entry.registerOrdinal =
                        static_cast<std::uint16_t>(it->second);
                    break;
                }
                case AsmConstraintBinding::OperandKind: {
                    auto const kind = operandKindFilterFromName(payloadStr);
                    if (!kind.has_value()) {
                        coll.emit(
                            DiagnosticCode::C_MalformedJson,
                            std::format("{}/operandKind", path),
                            std::format("constraint '{}': '{}' is not an "
                                        "operand kind — declared kinds are "
                                        "{} (the same closed vocabulary the "
                                        "encoding variant guards use; the "
                                        "JSON names carry historical width "
                                        "labels)",
                                        entry.letter, payloadStr,
                                        renderNames(kOperandKindFilterTable)));
                        continue;
                    }
                    entry.operandKind = *kind;
                    break;
                }
                }

                data.asmConstraints.push_back(std::move(entry));
            }
        }
    }

    // ── asmBareOperandWidths (which view a BARE `%0` names — optional) ──
    //
    // See `AsmBareOperandWidth` in the header for the measurement: gcc and
    // clang agree with each other and DISAGREE ACROSS PORTS about how a
    // modifier-less template operand states its width, and no width arithmetic
    // reproduces both — so the derivation is declared, per register class.
    //
    // ★ PARSED AFTER `registers` ON PURPOSE, the same precedent as
    // `asmConstraints` and the `implicitRegisters` roles: the `registerNatural`
    // derivation reads its width off the class's own full registers, so a class
    // that declares none must be visible as a load-time fact rather than as a
    // lookup that fails much later at a site with no target in hand.
    //
    // OPTIONAL, AND UNDECLARED IS NOT A DEFAULT. A target that omits this
    // refuses every bare operand reference BY NAME rather than substituting at
    // a plausible width — see the field's docblock for why a default here would
    // be a silent wrong width rather than a missing feature.
    if (doc.contains("asmBareOperandWidths")) {
        // Rendered FROM THE TABLES at each use, never re-typed — the same rule
        // the `asmConstraints` block states, and for the same reason.
        auto const renderRows = [](auto const& table) {
            std::string out;
            for (auto const& row : table.rows) {
                if (!out.empty()) out += ", ";
                out += '\'';
                out += row.second;
                out += '\'';
            }
            return out;
        };
        if (!doc.at("asmBareOperandWidths").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/asmBareOperandWidths",
                      "'asmBareOperandWidths' must be an array of "
                      "per-register-class derivation rows");
        } else {
            auto const& rows = doc.at("asmBareOperandWidths");
            for (std::size_t i = 0; i < rows.size(); ++i) {
                auto const& r = rows[i];
                auto const path =
                    std::format("/asmBareOperandWidths/{}", i);
                if (!r.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "asmBareOperandWidths entry must be an object");
                    continue;
                }
                static constexpr std::array<std::string_view, 2>
                    kBareWidthKeys{"class", "derivation"};
                DSS_CHECK_KEY_VOCABULARY(kBareWidthKeys);
                rejectUnknownKeys(r, kBareWidthKeys, path,
                                  "an asmBareOperandWidths row", coll);

                if (!r.contains("class") || !r.at("class").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("{}/class", path),
                              "missing or non-string 'class' — a bare-operand "
                              "width row must name the register class whose "
                              "references it describes");
                    continue;
                }
                auto const clsName = r.at("class").get<std::string>();
                auto const cls = targetRegClassFromName(clsName);
                // ★ THE SAME `isOperableTargetRegClass` GATE `registerClassOps`
                // USES, and for the same reason: `none` resolves as a NAME but
                // is not a legitimate SUBJECT — the no-class sentinel has no
                // registers, so no reference can name a view of one.
                if (!cls.has_value() || !isOperableTargetRegClass(*cls)) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/class", path),
                              std::format("unknown register class '{}' "
                                          "(expected one of: {})",
                                          clsName,
                                          renderRows(kTargetRegClassTable)));
                    continue;
                }
                if (!r.contains("derivation")
                    || !r.at("derivation").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("{}/derivation", path),
                              std::format("missing or non-string 'derivation' "
                                          "— a row must say WHICH view a bare "
                                          "operand reference names (one of: "
                                          "{})",
                                          renderRows(
                                              kAsmBareOperandWidthTable)));
                    continue;
                }
                auto const derivName = r.at("derivation").get<std::string>();
                auto const deriv = asmBareOperandWidthFromName(derivName);
                if (!deriv.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/derivation", path),
                              std::format("unknown derivation '{}' (expected "
                                          "one of: {})",
                                          derivName,
                                          renderRows(
                                              kAsmBareOperandWidthTable)));
                    continue;
                }
                auto const idx = static_cast<std::size_t>(*cls);
                // ⚠ A DUPLICATE ROW IS REFUSED RATHER THAN LAST-WINS. Two rows
                // for one class is a document whose author believed two
                // different things about the same reference, and silently
                // keeping either one ships one of those beliefs unexamined.
                if (data.asmBareOperandWidths[idx].has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/class", path),
                              std::format("register class '{}' already has a "
                                          "bare-operand width derivation — one "
                                          "row per class",
                                          clsName));
                    continue;
                }
                data.asmBareOperandWidths[idx] = *deriv;
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
                static constexpr std::array<std::string_view, 4> kRegClassOpKeys{
                    "class", "move", "load", "store"};
                DSS_CHECK_KEY_VOCABULARY(kRegClassOpKeys);
                rejectUnknownKeys(r, kRegClassOpKeys,
                                  std::format("/registerClassOps/{}", i),
                                  "a registerClassOps row", coll);
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
                              std::format("expected {}",
                                          detail::renderAllowedList(
                                              kOperableTargetRegClassNames,
                                              " / ")));
                    continue;
                }
                // The sentinel SPELLS correctly, so the lookup above accepts
                // it — the `isSelectableObjectFormatKind` hazard, on this
                // vocabulary. A row for the no-class sentinel would occupy
                // slot 0 of `registerClassOps` and declare move/load/store
                // mnemonics for registers that by definition do not exist.
                // See `isOperableTargetRegClass`.
                if (!isOperableTargetRegClass(*cls)) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/registerClassOps/{}/class", i),
                              std::format("'{}' is the no-class sentinel, not a "
                                          "class that owns registers — a "
                                          "registerClassOps row under it could "
                                          "never fire. Expected {}",
                                          targetRegClassName(*cls),
                                          detail::renderAllowedList(
                                              kOperableTargetRegClassNames,
                                              " / ")));
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

    // ── wideFloatSoftcalls (LD-2, D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH —
    // optional) ────────────────────────────────────────────────────
    // Per-`WideFloatOp` softfloat-libcall rows: {op, helperSymbol,
    // argRegisters[], resultRegister}. `op` resolves via wideFloatOpFromName;
    // an unknown op / a duplicate op row / a missing helperSymbol fail loud
    // (mirroring the registerClassOps duplicate rejection above). The arg /
    // result register NAMES are resolved to ordinals in validate() (the
    // register table is fully parsed by then) — an unresolvable name fails
    // there. A target that omits the section keeps every row `!declared`, so
    // the F128 engine verb falls through to the encoded-width wall.
    if (doc.contains("wideFloatSoftcalls")) {
        if (!doc.at("wideFloatSoftcalls").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/wideFloatSoftcalls",
                      "'wideFloatSoftcalls' must be an array");
        } else {
            auto const& rows = doc.at("wideFloatSoftcalls");
            for (std::size_t i = 0; i < rows.size(); ++i) {
                auto const& r = rows[i];
                if (!r.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/wideFloatSoftcalls/{}", i),
                              "wideFloatSoftcalls entry must be an object");
                    continue;
                }
                static constexpr std::array<std::string_view, 4> kSoftcallKeys{
                    "op", "helperSymbol", "argRegisters", "resultRegister"};
                DSS_CHECK_KEY_VOCABULARY(kSoftcallKeys);
                rejectUnknownKeys(r, kSoftcallKeys,
                                  std::format("/wideFloatSoftcalls/{}", i),
                                  "a wideFloatSoftcalls row", coll);
                if (!r.contains("op") || !r.at("op").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/wideFloatSoftcalls/{}/op", i),
                              "missing or non-string 'op'");
                    continue;
                }
                auto const op = wideFloatOpFromName(r.at("op").get<std::string>());
                if (!op.has_value()) {
                    // ⚠ ALSO AN UNQUOTED SET before this cycle — invisible to
                    // every quoted-token census, and to any test that reads a
                    // message back. Projected from the table that owns it
                    // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/wideFloatSoftcalls/{}/op", i),
                              std::format("unknown wideFloatSoftcalls op — "
                                          "accepted: {}",
                                          detail::renderAllowedList(
                                              allNames(kWideFloatOpTable),
                                              " / ")));
                    continue;
                }
                auto& row = data.wideFloatSoftcalls[static_cast<std::size_t>(*op)];
                if (row.declared) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/wideFloatSoftcalls/{}/op", i),
                              std::format("duplicate wideFloatSoftcalls row for "
                                          "op '{}'", wideFloatOpName(*op)));
                    continue;
                }
                if (!r.contains("helperSymbol")
                    || !r.at("helperSymbol").is_string()
                    || r.at("helperSymbol").get<std::string>().empty()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/wideFloatSoftcalls/{}/helperSymbol", i),
                              "missing or empty 'helperSymbol' — the runtime "
                              "library symbol the op lowers to a CALL of");
                    continue;
                }
                row.declared     = true;
                row.helperSymbol = r.at("helperSymbol").get<std::string>();
                if (r.contains("argRegisters")) {
                    if (!r.at("argRegisters").is_array()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/wideFloatSoftcalls/{}/argRegisters", i),
                                  "'argRegisters' must be an array of register names");
                    } else {
                        for (auto const& s : r.at("argRegisters")) {
                            if (!s.is_string()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          std::format("/wideFloatSoftcalls/{}/argRegisters", i),
                                          "each argRegisters entry must be a register-name string");
                                continue;
                            }
                            row.argRegisterNames.push_back(s.get<std::string>());
                        }
                    }
                }
                if (!r.contains("resultRegister")
                    || !r.at("resultRegister").is_string()
                    || r.at("resultRegister").get<std::string>().empty()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/wideFloatSoftcalls/{}/resultRegister", i),
                              "missing or empty 'resultRegister' register name");
                    continue;
                }
                row.resultRegisterName = r.at("resultRegister").get<std::string>();
            }
        }
    }

    // ── wideFloatSoftcallLibraryByFormat (LD-2 — optional) ─────────
    // Object-format-kind key → DT_NEEDED library string (e.g.
    // {"elf":"libgcc_s.so.1"}). The LIR lowerer resolves the ACTIVE format's
    // entry and binds each minted softcall extern to it.
    //
    // ★ EVERY KEY IS VALIDATED through `objectFormatKindFromName`, and the
    // parsed value is stored under the resolved KIND (an
    // `ObjectFormatKind`-indexed array), not under the raw string. This key
    // used to accept ARBITRARY strings — it was the precedent the
    // `charIsUnsigned.byObjectFormat` reshape deliberately did NOT follow, and
    // it left exactly the hole that reshape exists to delete: a misspelled
    // `"elff"` / a mis-cased `"ELF"` loaded perfectly clean, the accessor's
    // lookup missed, and the F128 softcall path reported that the format
    // declares no softcall library. Long-double arithmetic degraded or failed
    // on a PURE TYPO, with no diagnostic naming the config.
    //
    // Three rejections, each closing one way the old shape could lie:
    //   * an unrecognized name — the typo case, and the diagnostic NAMES it;
    //   * the `unknown` sentinel — it SPELLS correctly so the name lookup
    //     succeeds, but it selects no real format (the shared
    //     `kObjectFormatKindSentinelRejection` discipline);
    //   * an EMPTY library string — indistinguishable at the accessor from an
    //     absent key, i.e. the same silent fallback one layer down.
    if (doc.contains("wideFloatSoftcallLibraryByFormat")) {
        auto const& lib = doc.at("wideFloatSoftcallLibraryByFormat");
        if (!lib.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/wideFloatSoftcallLibraryByFormat",
                      std::format("must be an object mapping object-format "
                                  "kind name ({}) → library string",
                                  detail::renderAllowedList(
                                      kSelectableObjectFormatKindNames, " / ")));
        } else {
            for (auto it = lib.begin(); it != lib.end(); ++it) {
                if (detail::isDocumentationKey(it.key())) continue;
                auto const path = std::format(
                    "/wideFloatSoftcallLibraryByFormat/{}", it.key());
                auto const kind = objectFormatKindFromName(it.key());
                if (!kind.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              std::format(
                                  "'{}' is not a recognized object-format kind "
                                  "(expected one of {}). An unrecognized name "
                                  "would declare a softcall library that never "
                                  "resolves, silently leaving the F128 softcall "
                                  "path with no runtime library.", it.key(),
                                  detail::renderAllowedList(
                                      kSelectableObjectFormatKindNames, " / ")));
                    continue;
                }
                if (!isSelectableObjectFormatKind(*kind)) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              std::string{kObjectFormatKindSentinelRejection});
                    continue;
                }
                if (!it.value().is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "library value must be a string (the DT_NEEDED "
                              "library the minted softcall externs bind to)");
                    continue;
                }
                auto value = it.value().get<std::string>();
                if (value.empty()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "library value must be non-empty — an empty "
                              "string is indistinguishable from declaring no "
                              "library at all, so it would read as a silent "
                              "'this format has none' instead of the "
                              "declaration it looks like. Omit the key "
                              "instead.");
                    continue;
                }
                data.wideFloatSoftcallLibraryByFormat[
                    static_cast<std::size_t>(*kind)] = std::move(value);
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
                // The widest row in the file, and the one whose shipped
                // instances carry the most `$...Comment` prose keys - which is
                // exactly why the carve-out lives in `rejectUnknownKeys` as a
                // PREFIX test rather than a literal `"$comment"` entry.
                static constexpr std::array<std::string_view, 28> kCallConvKeys{
                    "name",
                    "argGprs", "argFprs", "returnGprs", "returnFprs",
                    "argVrs", "returnVrs", "callerSaved", "calleeSaved",
                    "stackAlignment", "shadowSpaceBytes", "redZoneBytes",
                    "entryStackPointerBias", "callPushBytes",
                    "stackProbePageBytes", "aggregateMaxRegBytes",
                    "aggregateClassification", "slotAligned",
                    "variadicArgsAlwaysStack",
                    "aggregateStackExhaustsRegisters",
                    "stackArgPacking",
                    "linkRegister", "stackPointer", "framePointer",
                    // D-CODEGEN-APPLE-ARM64-X29-USED-AS-GENERAL-SCRATCH-AGAINST-ITS-RESERVED-ROLE:
                    // WHEN the frame pointer leaves the allocatable pool. A key
                    // absent from THIS array is REFUSED AT LOAD, so the
                    // vocabulary row lands before the descriptors that declare
                    // it, never after.
                    "framePointerReservation",
                    "variadicVectorCountReg", "indirectResultRegister",
                    "vaListLayout"};
                DSS_CHECK_KEY_VOCABULARY(kCallConvKeys);
                rejectUnknownKeys(c, kCallConvKeys,
                                  std::format("/callingConventions/{}", i),
                                  "a calling-convention row", coll);
                TargetCallingConvention cc;
                if (!c.contains("name") || !c.at("name").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/callingConventions/{}/name", i),
                              "missing or non-string 'name'");
                    continue;
                }
                cc.name = c.at("name").get<std::string>();
                // D-FFI-ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY:
                // a `.format.json` selects its platform C ABI by NAMING one of
                // these rows, and reserves one spelling to mean "this format
                // has no register-level C calling convention at all". That is
                // an in-band sentinel inside a name space, which is the exact
                // hazard `objectFormatKindName`'s "★ THE SENTINEL SPELLS
                // CORRECTLY" note describes — a sentinel that RESOLVES is worse
                // than a typo, because it passes every lookup and dies quietly
                // downstream. Closing it needs BOTH sides: the format loader
                // refuses an empty convention, and this refuses a row that
                // would make the reserved spelling ambiguous. Refused at LOAD,
                // once, rather than defended against at every use.
                if (cc.name == kCCallingConventionNone) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/callingConventions/{}/name", i),
                              std::format("'{}' is a RESERVED calling-convention "
                                          "name — a .format.json spells it to "
                                          "declare that the format has NO "
                                          "register-level C calling convention, "
                                          "so a row carrying it would make that "
                                          "declaration ambiguous. Rename the row.",
                                          kCCallingConventionNone));
                    continue;
                }
                readStringArray(c, i, "argGprs",     cc.argGprs);
                readStringArray(c, i, "argFprs",     cc.argFprs);
                readStringArray(c, i, "returnGprs",  cc.returnGprs);
                readStringArray(c, i, "returnFprs",  cc.returnFprs);
                // D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): the binary128 VR
                // arg/return register lists (empty on non-ieee128 targets).
                readStringArray(c, i, "argVrs",      cc.argVrs);
                readStringArray(c, i, "returnVrs",   cc.returnVrs);
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
                // ⚠ BOTH refusals used to name NO accepted set at all — the
                // degenerate case of the retyped-closed-set class: a config
                // author was told the spelling was wrong and given nothing to
                // write instead. Rendered from the table, so a new strategy
                // reaches the message with its row
                // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
                if (c.contains("aggregateClassification")) {
                    if (!c.at("aggregateClassification").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/aggregateClassification", ccPath),
                                  std::format("'aggregateClassification' must be "
                                              "a strategy-name string — "
                                              "accepted: {}",
                                              detail::renderAllowedList(
                                                  allNames(kAggregateClassKindTable),
                                                  " / ")));
                    } else {
                        auto const s = c.at("aggregateClassification").get<std::string>();
                        if (auto const k = aggregateClassKindFromName(s); k.has_value()) {
                            cc.aggregateClassification = *k;
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("{}/aggregateClassification", ccPath),
                                      std::format("unknown aggregate-classification "
                                                  "strategy '{}' — accepted: {}", s,
                                                  detail::renderAllowedList(
                                                      allNames(kAggregateClassKindTable),
                                                      " / ")));
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
                // D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: the
                // three-axis stacked-argument packing declaration. OPTIONAL, and
                // an omitted object (or an omitted key inside it) means "slot" —
                // the classic one-pointer-width-slot rule — so every CC that says
                // nothing is byte-unchanged. Each value is a name from the closed
                // `StackArgPacking` vocabulary; an unknown key OR an unknown value
                // FAILS LOUD naming the accepted set, because the failure mode of
                // silently accepting either is an ABI divergence that compiles
                // clean and miscompiles at the boundary
                // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
                if (c.contains("stackArgPacking")) {
                    std::string const sapPath =
                        std::format("{}/stackArgPacking", ccPath);
                    if (!c.at("stackArgPacking").is_object()) {
                        coll.emit(DiagnosticCode::C_MalformedJson, sapPath,
                                  "'stackArgPacking' must be an object with the "
                                  "optional keys 'namedScalars' / "
                                  "'namedAggregates' / 'variadic'");
                    } else {
                        auto const& sap = c.at("stackArgPacking");
                        static constexpr std::array<std::string_view, 3>
                            kStackArgPackingKeys{"namedScalars",
                                                 "namedAggregates", "variadic"};
                        DSS_CHECK_KEY_VOCABULARY(kStackArgPackingKeys);
                        rejectUnknownKeys(sap, kStackArgPackingKeys, sapPath,
                                          "a stack-arg-packing row", coll);
                        auto readAxis =
                            [&](std::string_view key, StackArgPacking& out) {
                                if (!sap.contains(std::string{key})) return;
                                auto const& v = sap.at(std::string{key});
                                if (!v.is_string()) {
                                    coll.emit(
                                        DiagnosticCode::C_MalformedJson,
                                        std::format("{}/{}", sapPath, key),
                                        std::format(
                                            "'{}' must be a packing-rule name "
                                            "string — accepted: {}", key,
                                            detail::renderAllowedList(
                                                allNames(kStackArgPackingTable),
                                                " / ")));
                                    return;
                                }
                                auto const s = v.get<std::string>();
                                if (auto const p = stackArgPackingFromName(s);
                                    p.has_value()) {
                                    out = *p;
                                } else {
                                    coll.emit(
                                        DiagnosticCode::C_MalformedJson,
                                        std::format("{}/{}", sapPath, key),
                                        std::format(
                                            "unknown stack-arg packing rule '{}' "
                                            "— accepted: {}", s,
                                            detail::renderAllowedList(
                                                allNames(kStackArgPackingTable),
                                                " / ")));
                                }
                            };
                        readAxis("namedScalars",    cc.stackArgPacking.namedScalars);
                        readAxis("namedAggregates", cc.stackArgPacking.namedAggregates);
                        readAxis("variadic",        cc.stackArgPacking.variadic);
                        // ⚠ ONE BUILDABLE VALUE ON THE AGGREGATE AXIS, AND THE
                        // OTHER IS REFUSED RATHER THAN APPROXIMATED. Natural
                        // packing needs the datum's own ALIGNMENT; a stacked
                        // aggregate reaches the placement tier through the
                        // `ByValueStackAgg` carrier, which states a byte SIZE and
                        // nothing else. Accepting "natural" here would make the
                        // cursor align aggregates to the slot while advancing by
                        // their exact size — a THIRD rule nobody measured, shipped
                        // under the name of one that was. ✔MEASURED: both shipped
                        // ABIs slot-round aggregates, so nothing is lost today.
                        if (cc.stackArgPacking.namedAggregates
                                != StackArgPacking::Slot) {
                            coll.emit(
                                DiagnosticCode::C_MalformedJson,
                                std::format("{}/namedAggregates", sapPath),
                                std::format(
                                    "'namedAggregates' may only be '{}' today: "
                                    "natural aggregate packing needs the "
                                    "aggregate's own alignment, which the "
                                    "by-value stack-aggregate carrier does not "
                                    "state (it carries a byte size only), so the "
                                    "placement tier cannot honour it — refusing "
                                    "rather than approximating it as "
                                    "slot-aligned-but-exactly-sized ({})",
                                    stackArgPackingName(StackArgPacking::Slot),
                                    kApplePackingAnchor));
                            cc.stackArgPacking.namedAggregates =
                                StackArgPacking::Slot;
                        }
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
                // D-CSUBSET-VLA (C1b): optional frame-pointer register (rbp / x29).
                // Present only on a target that supports dynamic-stack VLA codegen;
                // omitted ⇒ a VLA fails loud rather than miscompiling. Mirrors the
                // stackPointer decode (name → register-table ordinal).
                if (c.contains("framePointer")) {
                    if (!c.at("framePointer").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/framePointer", ccPath),
                                  "must be a register-name string");
                    } else {
                        auto const name = c.at("framePointer").get<std::string>();
                        auto it = data.registerIndex.find(name);
                        if (it != data.registerIndex.end()) {
                            cc.framePointer = TargetCallingConvention::NamedRegisterRef{
                                name, it->second
                            };
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("{}/framePointer", ccPath),
                                      std::format("frame pointer '{}' is not "
                                                  "in the register table", name));
                        }
                    }
                }
                // D-CODEGEN-APPLE-ARM64-X29-USED-AS-GENERAL-SCRATCH-AGAINST-ITS-RESERVED-ROLE:
                // WHEN that register is withheld from the allocator's pool.
                // OPTIONAL, defaulting to `dynamic-frame-only` — the behavior
                // every shipped convention had before the key existed — so
                // adding the vocabulary changes no frame by itself; only a
                // convention that DECLARES `always` loses the register. An
                // unknown spelling is a HARD error, the `dataModel` discipline:
                // a typo silently falling back to `dynamic-frame-only` would
                // re-open the exact defect the key closes, on the one
                // convention whose author was trying to close it.
                if (c.contains("framePointerReservation")) {
                    if (!c.at("framePointerReservation").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("{}/framePointerReservation", ccPath),
                                  std::format("must be a string ({})",
                                              detail::renderAllowedList(
                                                  allNames(kFramePointerReservationTable),
                                                  " or ")));
                    } else {
                        auto const s =
                            c.at("framePointerReservation").get<std::string>();
                        auto const r = framePointerReservationFromName(s);
                        if (!r.has_value()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      std::format("{}/framePointerReservation", ccPath),
                                      std::format("unknown framePointerReservation "
                                                  "'{}' — expected one of {}", s,
                                                  detail::renderAllowedList(
                                                      allNames(kFramePointerReservationTable),
                                                      ", ")));
                        } else {
                            cc.framePointerReservation = *r;
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
                        // ── D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY ──
                        //
                        // The key set is PER-STRATEGY, not a union over all
                        // three. A union accepts `gpOffsetLimit` on an
                        // `aapcs64_dual_cursor` layout: the key is spelled
                        // correctly, it loads clean, and the strategy's own
                        // parse arm never reads it — so the producer declared
                        // a limit, got no limit, and got no diagnostic. That
                        // is the same silently-switched-off-feature failure
                        // `rejectUnknownKeys` exists to prevent, just one
                        // level in: the gate was checking that the key is a
                        // key SOMEWHERE, not that it is a key HERE.
                        //
                        // ★ The union below is DERIVED from the per-strategy
                        //   arrays and never written a second time — a
                        //   hand-maintained union is precisely how the two
                        //   would drift back apart.
                        static constexpr std::array<std::string_view, 2>
                            kVaListCommonKeys{"strategy", "namedArgSlotBytes"};
                        static constexpr std::array<std::string_view, 10>
                            kVaListSysVKeys{
                                "gpOffsetField", "fpOffsetField",
                                "overflowArgAreaField", "regSaveAreaField",
                                "gpSaveCount", "gpSlotBytes",
                                "fpSaveCount", "fpSlotBytes",
                                "gpOffsetLimit", "fpOffsetLimit"};
                        static constexpr std::array<std::string_view, 1>
                            kVaListHomogeneousKeys{"variadicUsesOverflowBase"};
                        static constexpr std::array<std::string_view, 9>
                            kVaListAapcs64Keys{
                                "stackField", "grTopField", "vrTopField",
                                "grOffsField", "vrOffsField",
                                "gpSaveCount", "gpSlotBytes",
                                "fpSaveCount", "fpSlotBytes"};
                        DSS_CHECK_KEY_VOCABULARY(kVaListCommonKeys);
                        DSS_CHECK_KEY_VOCABULARY(kVaListSysVKeys);
                        DSS_CHECK_KEY_VOCABULARY(kVaListHomogeneousKeys);
                        DSS_CHECK_KEY_VOCABULARY(kVaListAapcs64Keys);
                        auto const keysOf =
                            [&](VaListStrategy s)
                                -> std::span<std::string_view const> {
                            switch (s) {
                                case VaListStrategy::SysVRegisterSave:
                                    return kVaListSysVKeys;
                                case VaListStrategy::HomogeneousPointer:
                                    return kVaListHomogeneousKeys;
                                case VaListStrategy::Aapcs64DualCursor:
                                    return kVaListAapcs64Keys;
                            }
                            return {};
                        };
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
                            static constexpr std::array<std::string_view, 2>
                                kVaListFieldKeys{"byteOffset", "widthBytes"};
                            DSS_CHECK_KEY_VOCABULARY(kVaListFieldKeys);
                            rejectUnknownKeys(
                                f, kVaListFieldKeys,
                                std::format("{}/vaListLayout/{}", ccPath, key),
                                "a vaListLayout field row", coll);
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
                        bool strategyResolved = true;
                        if (vl.contains("strategy")) {
                            if (!vl.at("strategy").is_string()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          std::format("{}/vaListLayout/strategy", ccPath),
                                          "'strategy' must be a string");
                                vlOk = false;
                                strategyResolved = false;
                            } else {
                                auto const sname =
                                    vl.at("strategy").get<std::string>();
                                auto const s = vaListStrategyFromName(sname);
                                if (!s.has_value()) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              std::format("{}/vaListLayout/strategy", ccPath),
                                              std::format("unknown va_list strategy '{}' "
                                                          "— accepted: {}", sname,
                                                          detail::renderAllowedList(
                                                              allNames(kVaListStrategyTable),
                                                              " / ")));
                                    vlOk = false;
                                    strategyResolved = false;
                                } else {
                                    layout.strategy = *s;
                                }
                            }
                        }
                        // ── The per-strategy key gate ────────────────────
                        //
                        // Runs HERE, after `strategy` is known, because the
                        // legal key set is a function of it. Skipped when the
                        // strategy did not resolve: reporting every field of
                        // an `aapcs46_duel_cursor` typo as a cross-strategy
                        // key would bury the one diagnostic that matters
                        // under nine that do not.
                        if (strategyResolved) {
                            auto const own = keysOf(layout.strategy);
                            auto const inSet =
                                [](std::span<std::string_view const> set,
                                   std::string const& k) {
                                    for (auto const& e : set) {
                                        if (k == e) return true;
                                    }
                                    return false;
                                };
                            for (auto it = vl.begin(); it != vl.end(); ++it) {
                                auto const& key = it.key();
                                // The `$`-prefix prose convention, applied by
                                // the SAME predicate `rejectUnknownKeys` uses
                                // — a second spelling of it here is how one
                                // site ends up rejecting `$fooComment`.
                                //
                                // ⓘ NOT ROUTED THROUGH `rejectUnknownKeys`,
                                // deliberately: the vocabulary here is TWO
                                // tables (common + the declared strategy's
                                // own), and the whole value of this site is a
                                // message the shared sentence cannot carry —
                                // it names the OTHER strategy that would have
                                // read the key, which is what distinguishes a
                                // typo from a copy-paste off the wrong ABI
                                // (D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY).
                                // Routing it would trade a diagnostic
                                // for a shape.
                                if (detail::isDocumentationKey(key)) continue;
                                if (inSet(kVaListCommonKeys, key)) continue;
                                if (inSet(own, key)) continue;
                                // Name the strategy (or strategies) that WOULD
                                // have taken it — that is what turns "unknown
                                // key" into a message the author can act on,
                                // and it is the difference between a typo and
                                // a copy-paste from the wrong ABI.
                                // ⚠ THE SCAN WALKS THE TABLE, not a hand-listed
                                // triple. It used to name the three
                                // enumerators inline, which made a fourth
                                // strategy's keys invisible to this message —
                                // the author would get the bare "no va_list
                                // strategy declares it" arm for a key that
                                // plainly belongs to one, which is the exact
                                // opposite of what the arm means.
                                std::string accepted;
                                for (auto const& row : kVaListStrategyTable.rows) {
                                    if (row.first == layout.strategy) continue;
                                    if (!inSet(keysOf(row.first), key)) continue;
                                    if (!accepted.empty()) accepted += "' / '";
                                    accepted += row.second;
                                }
                                // The shared renderer, not a third copy of the
                                // quote-and-join loop (`renderAllowedList` is
                                // the ONE renderer — see its header note).
                                std::string allowed =
                                    detail::renderAllowedList(kVaListCommonKeys,
                                                              ", ");
                                if (!own.empty()) {
                                    if (!allowed.empty()) allowed += ", ";
                                    allowed += detail::renderAllowedList(own, ", ");
                                }
                                coll.emit(
                                    DiagnosticCode::C_MalformedJson,
                                    std::format("{}/vaListLayout/{}", ccPath, key),
                                    accepted.empty()
                                        ? std::format(
                                              "unknown key '{}' in a vaListLayout "
                                              "block — no va_list strategy declares "
                                              "it. Allowed for the declared strategy "
                                              "'{}' are {} (plus any '$'-prefixed "
                                              "documentation key)",
                                              key,
                                              vaListStrategyName(layout.strategy),
                                              allowed)
                                        : std::format(
                                              "key '{}' belongs to va_list strategy "
                                              "'{}', but this block declares "
                                              "strategy '{}' — the '{}' parse arm "
                                              "never reads it, so it would load "
                                              "clean and do NOTHING "
                                              "(D-CONFIG-VALISTLAYOUT-INERT-CROSS-"
                                              "STRATEGY-KEY). Allowed here are {} "
                                              "(plus any '$'-prefixed documentation "
                                              "key)",
                                              key, accepted,
                                              vaListStrategyName(layout.strategy),
                                              vaListStrategyName(layout.strategy),
                                              allowed));
                                vlOk = false;
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

    // ── Lowering-sequence resolution + validation ─────────────────
    // D-TARGET-ENCODING-TABLE-EXPRESSES-ONLY-THE-DEGENERATE-SEQUENCE.
    // Runs here, not in the per-opcode loop, because a step may name ANY
    // opcode of this target — including one declared LATER in the array — so
    // the mnemonic index must be complete first (the same reason the
    // implicit-register resolution below waits for the register table).
    //
    // ★ THE LOAD-BEARING RULE IS "a step names a REAL MACHINE INSTRUCTION":
    // its opcode must declare a non-empty `encoding`. That single check is
    // what makes the expansion ONE LEVEL and non-recursive — arm64's
    // self-naming one-step sequence is well-founded because the opcode it
    // names carries FCVTZU's bytes, and a step naming an opcode that only has
    // a `lowering` of its own is refused at LOAD, not discovered as a hang.
    for (std::size_t opIdx = 0; opIdx < data.opcodes.size(); ++opIdx) {
        auto& info = data.opcodes[opIdx];
        if (info.lowering.sequences.empty()) continue;
        auto const opPath = std::format("/opcodes/{}/lowering", opIdx);
        // Width-guard coherence: the encoding variants' rule, verbatim.
        // Two sequences on one width, or the width-keyed/width-absent mix,
        // make first-match dispatch silently shadow one of them.
        for (std::size_t a = 0; a < info.lowering.sequences.size(); ++a) {
            for (std::size_t b = a + 1; b < info.lowering.sequences.size(); ++b) {
                auto const wa = info.lowering.sequences[a].guardWidthBits;
                auto const wb = info.lowering.sequences[b].guardWidthBits;
                if (wa == wb) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/sequences/{}", opPath, b),
                              std::format(
                                  "opcode '{}': two lowering sequences declare "
                                  "the same guard width ({}) — first-match "
                                  "dispatch would silently shadow one",
                                  info.mnemonic, wa));
                } else if (wa == 0 || wb == 0) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("{}/sequences/{}", opPath, b),
                              std::format(
                                  "opcode '{}': a width-keyed lowering sequence "
                                  "({}) sits beside a width-ABSENT sibling — the "
                                  "absent one matches every width and would "
                                  "shadow or be shadowed depending on order; "
                                  "key both or neither",
                                  info.mnemonic, wa != 0 ? wa : wb));
                }
            }
        }
        for (std::size_t si = 0; si < info.lowering.sequences.size(); ++si) {
            auto& seq     = info.lowering.sequences[si];
            auto const sp = std::format("{}/sequences/{}", opPath, si);
            // Temp-slot table: built as the steps are walked, so a `temp`
            // operand can only reference a name an EARLIER step defined.
            // A forward reference would read an undefined register.
            std::size_t resultDefiners = 0;
            for (std::size_t ti = 0; ti < seq.steps.size(); ++ti) {
                auto& step  = seq.steps[ti];
                auto const tp = std::format("{}/steps/{}", sp, ti);
                // (a) the step's opcode exists and is a real instruction.
                auto const it = data.mnemonicIndex.find(step.opcodeMnemonic);
                if (it == data.mnemonicIndex.end()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, tp + "/op",
                              std::format("opcode '{}': lowering step names "
                                          "mnemonic '{}', which this target's "
                                          "opcode table does not declare",
                                          info.mnemonic, step.opcodeMnemonic));
                    continue;
                }
                step.opcodeIndex = it->second;
                auto const& stepInfo = data.opcodes[step.opcodeIndex];
                if (stepInfo.encoding.variants.empty()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, tp + "/op",
                              std::format(
                                  "opcode '{}': lowering step names '{}', which "
                                  "declares NO encoding variants — a step must "
                                  "name a real MACHINE instruction (this is the "
                                  "rule that keeps the expansion one level deep "
                                  "and non-recursive)",
                                  info.mnemonic, step.opcodeMnemonic));
                }
                // (b) arity against the named opcode's declared bounds.
                if (step.operands.size() < stepInfo.minOperands
                    || step.operands.size() > stepInfo.maxOperands) {
                    coll.emit(DiagnosticCode::C_MalformedJson, tp + "/operands",
                              std::format(
                                  "opcode '{}': lowering step '{}' supplies {} "
                                  "operand(s), outside that opcode's declared "
                                  "[{}, {}]", info.mnemonic,
                                  step.opcodeMnemonic, step.operands.size(),
                                  stepInfo.minOperands, stepInfo.maxOperands));
                }
                // (c) operand references.
                for (std::size_t k = 0; k < step.operands.size(); ++k) {
                    auto& op = step.operands[k];
                    auto const opp = std::format("{}/operands/{}", tp, k);
                    if (op.kind == TargetLoweringOperandKind::Source) {
                        if (op.sourceIndex >= info.maxOperands) {
                            coll.emit(DiagnosticCode::C_MalformedJson, opp,
                                      std::format(
                                          "opcode '{}': lowering step reads "
                                          "source operand {}, but the opcode "
                                          "declares at most {} operand(s)",
                                          info.mnemonic, op.sourceIndex,
                                          info.maxOperands));
                        }
                    } else if (op.kind == TargetLoweringOperandKind::Temp) {
                        bool found = false;
                        for (std::size_t s = 0; s < seq.tempNames.size(); ++s) {
                            if (seq.tempNames[s] == op.tempName) {
                                op.tempSlot = static_cast<std::uint16_t>(s);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            coll.emit(DiagnosticCode::C_MalformedJson, opp,
                                      std::format(
                                          "opcode '{}': lowering step reads "
                                          "temporary '{}', which no EARLIER "
                                          "step of this sequence defines",
                                          info.mnemonic, op.tempName));
                        }
                    }
                }
                // (d) result naming. `result` is the reserved spelling for
                // the value the lowered instruction itself yields; every
                // other name mints a temp slot for later steps to read.
                if (!step.hasResult) continue;
                if (step.definesResult) {
                    ++resultDefiners;
                    continue;
                }
                for (auto const& existing : seq.tempNames) {
                    if (existing == step.resultName) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  tp + "/result",
                                  std::format(
                                      "opcode '{}': lowering sequence defines "
                                      "temporary '{}' twice — a redefinition "
                                      "makes which one a later step reads "
                                      "depend on scan order",
                                      info.mnemonic, step.resultName));
                        break;
                    }
                }
                step.resultTempSlot =
                    static_cast<std::uint16_t>(seq.tempNames.size());
                seq.tempNames.push_back(step.resultName);
            }
            // (e) exactly one step yields the operation's value, and it is
            // the LAST one. A sequence whose value-producing step is not
            // last has instructions running AFTER the result is bound.
            if (resultDefiners != 1) {
                coll.emit(DiagnosticCode::C_MalformedJson, sp,
                          std::format("opcode '{}': lowering sequence declares "
                                      "{} step(s) naming the reserved result — "
                                      "exactly one is required",
                                      info.mnemonic, resultDefiners));
            } else if (!seq.steps.empty() && !seq.steps.back().definesResult) {
                coll.emit(DiagnosticCode::C_MalformedJson, sp,
                          std::format("opcode '{}': the step naming the "
                                      "reserved result is not the LAST step of "
                                      "the sequence", info.mnemonic));
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
        //
        // ★★★ ASKED WITH THE TYPE'S OWN PREDICATE (P42, 2026-08-27). This site
        // used to hand-roll the comparison over a local `std::set`, which made
        // it the SECOND implementation of a rule three other callers were
        // already asking `ImplicitRegisterConstraint` for — the per-instruction
        // builder, the `.dsslir` reader and the inline-asm lowering. Two
        // implementations of one invariant do not stay equal; the friendlier
        // one wins by accident, and the strict one's abort then contradicts a
        // document this loader accepted.
        //
        // ★ WHY THE PER-INDEX PREDICATE AND NOT `firstOutputNotClobbered`. This
        // caller must emit ONE diagnostic per offending output, each with that
        // output's own JSON pointer, so a single re-run surfaces every mistake
        // in the document. A first-failure query structurally cannot do that.
        // One rule, two shapes — see the predicate's docblock.
        //
        // ⚠ A LENGTH MISMATCH BAILS RATHER THAN REPORTING. `resolveArr` SKIPS
        // an unresolvable name (having already reported it), so `outputOrdinals`
        // can be SHORTER than `outputNames` — and the message below subscripts
        // the NAMES with an index taken from the ORDINALS. Reporting through a
        // mismatch cannot make the test pass wrongly, but it CAN name the wrong
        // register, which is the half-applied shape this repo keeps
        // rediscovering. Nothing is silently dropped: every skipped name was
        // already diagnosed by `resolveArr`, and the document cannot load.
        if (ir.outputOrdinals.size() == ir.outputNames.size()) {
            for (std::size_t k = 0; k < ir.outputOrdinals.size(); ++k) {
                if (ir.outputIsClobbered(k)) continue;
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

        // Role-map resolution + validation (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT,
        // 2026-06-10). Three rejects per role:
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
        // c103 (D-CSUBSET-INTRINSIC-UMULH) added the MUL-HIGH projection:
        // "multiplicand" (the RAX implicit input of x86 `mul r/m64`) plus
        // "high"/"low" (the RDX/RAX halves of the 128-bit product; the
        // `__umulh` lowering captures the "high" output-role). arm64's
        // native `umulh` needs no roles (a 3-address result-bearing op).
        // c104 (D-CSUBSET-INTRINSIC-ATOMIC-CAS) added the CAS projection:
        // "comparand" (the RAX implicit input of x86 `lock cmpxchg`) plus
        // "old" (RAX out — the observed-original value on BOTH outcomes; the
        // AtomicCas lowering captures it). arm64's ldaxr/stlxr need no roles
        // (result-bearing ops; the status rides the stlxr result slot).
        static constexpr std::array<std::string_view, 9>
            kKnownImplicitRegisterRoles{"dividend", "quotient", "remainder",
                                        "count", "multiplicand", "high", "low",
                                        "comparand", "old"};
        DSS_CHECK_KEY_VOCABULARY(kKnownImplicitRegisterRoles);
        // ⚠ THE LIST IS RENDERED FROM THE TABLE, never re-typed. The message
        // below used to spell out four roles by hand while the table held
        // NINE — a diagnostic that told the reader their valid role was
        // invalid. Every role added since (multiplicand/high/low/comparand/
        // old) grew the table and left the prose behind, which is the exact
        // "the comment records half of what the code does" failure this file
        // is being swept for, one layer up.
        auto const knownImplicitRoleList = [&] {
            std::string out;
            for (auto const& k : kKnownImplicitRegisterRoles) {
                if (!out.empty()) out += ", ";
                out += '\'';
                out += k;
                out += '\'';
            }
            return out;
        };
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
                                          "roles are {} (typo discriminator; "
                                          "extend the registered vocabulary "
                                          "for a new projection shape)",
                                          info.mnemonic, role,
                                          knownImplicitRoleList()));
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

    // ── wideFloatSoftcall register resolution (LD-2, mirrors the
    // implicit-register role resolution above) ────────────────────
    // Now that the register table is fully parsed, resolve each declared
    // softcall's arg/result register NAMES → ordinals into the mutable
    // `data`. An unresolvable name fails loud (C_MalformedJson) exactly as a
    // role naming an unknown register does — the F128 softcall lowering reads
    // the ordinals directly and must never index a name that has no register.
    for (std::size_t oi = 0; oi < data.wideFloatSoftcalls.size(); ++oi) {
        auto& row = data.wideFloatSoftcalls[oi];
        if (!row.declared) continue;
        auto const opName = wideFloatOpName(static_cast<WideFloatOp>(oi));
        row.argRegisterOrdinals.clear();
        row.argRegisterOrdinals.reserve(row.argRegisterNames.size());
        for (auto const& argName : row.argRegisterNames) {
            auto it = data.registerIndex.find(argName);
            if (it == data.registerIndex.end()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/wideFloatSoftcalls/{}/argRegisters", opName),
                          std::format("wideFloatSoftcalls op '{}': argRegister "
                                      "'{}' does not resolve to any declared "
                                      "register", opName, argName));
                continue;
            }
            row.argRegisterOrdinals.push_back(
                static_cast<std::uint16_t>(it->second));
        }
        auto rit = data.registerIndex.find(row.resultRegisterName);
        if (rit == data.registerIndex.end()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/wideFloatSoftcalls/{}/resultRegister", opName),
                      std::format("wideFloatSoftcalls op '{}': resultRegister "
                                  "'{}' does not resolve to any declared "
                                  "register", opName, row.resultRegisterName));
        } else {
            row.resultRegisterOrdinal = static_cast<std::uint16_t>(rit->second);
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

    auto schema = std::make_shared<TargetSchema>(std::move(data));
    schema->contentDigest_ = std::move(digest);
    return schema;
}

} // namespace dss
