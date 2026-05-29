#include "core/types/target_schema.hpp"

#include "core/substrate/mint_monotonic_id.hpp"
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

struct Collector {
    std::vector<ConfigDiagnostic> diagnostics;
    void emit(DiagnosticCode code, std::string path, std::string message,
              DiagnosticSeverity sev = DiagnosticSeverity::Error) {
        diagnostics.push_back({code, sev, std::move(path), std::move(message)});
    }
};

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
        return std::unexpected(std::move(coll.diagnostics));
    }
    if (!doc.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  "top-level value must be a JSON object");
        return std::unexpected(std::move(coll.diagnostics));
    }

    // ── dssTargetVersion ──
    if (!doc.contains("dssTargetVersion")
     || !doc.at("dssTargetVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::C_VersionMismatch,
                  std::string{sourceLabel},
                  "missing or non-integer 'dssTargetVersion'");
        return std::unexpected(std::move(coll.diagnostics));
    }
    int const ver = doc.at("dssTargetVersion").get<int>();
    if (ver != 1) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssTargetVersion",
                  std::format("only version 1 supported (got {})", ver));
        return std::unexpected(std::move(coll.diagnostics));
    }

    detail::TargetSchemaData data;
    data.id = substrate::mintMonotonicId<TargetSchemaId>();

    // ── target.name + target.version ──
    if (!doc.contains("target") || !doc.at("target").is_object()) {
        coll.emit(DiagnosticCode::C_MissingField, std::string{sourceLabel},
                  "missing 'target' object");
        return std::unexpected(std::move(coll.diagnostics));
    }
    auto const& target = doc.at("target");
    if (!target.contains("name") || !target.at("name").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/target/name",
                  "missing or non-string 'name'");
        return std::unexpected(std::move(coll.diagnostics));
    }
    data.name = target.at("name").get<std::string>();
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
            return std::unexpected(std::move(coll.diagnostics));
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

    // ── opcodes ──
    if (!doc.contains("opcodes") || !doc.at("opcodes").is_array()) {
        coll.emit(DiagnosticCode::C_MissingField, "/opcodes",
                  "missing 'opcodes' array");
        return std::unexpected(std::move(coll.diagnostics));
    }
    auto const& ops = doc.at("opcodes");
    if (ops.empty()) {
        coll.emit(DiagnosticCode::C_MissingField, "/opcodes",
                  "opcodes array must be non-empty (first entry is the "
                  "Invalid sentinel)");
        return std::unexpected(std::move(coll.diagnostics));
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
                        info.encodingShape = *shape;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/opcodes/{}/encoding/format", i),
                                  "expected 'none' / 'x86-variable' / 'fixed32'");
                    }
                }
            }
            // `variants` / `template` / `operands` sub-fields are
            // tolerated as forward-compat shape: AS2/AS3 add their
            // schema validation when those sub-trees gain consumers.
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

    // ── relocations (AS1 §2.6 — optional) ─────────────────────────
    // Empty/absent section is legal (cycle 2a targets emit no
    // relocations yet); non-empty rows must satisfy the validate()
    // contract: unique non-zero `kind`, non-empty `name`. The
    // `formula` is opaque to the substrate — stored verbatim for
    // diagnostic display and for the linker's `*.format.json`
    // human-readable cross-reference.
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
                TargetRelocationInfo info;
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
                              "missing or non-integer 'kind' (must be a "
                              "non-zero uint32 — the opaque tag the "
                              "assembler stamps onto Relocation::kind)");
                    continue;
                }
                {
                    std::int64_t const v = r.at("kind").get<std::int64_t>();
                    if (v < 0 || v > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/relocations/{}/kind", i),
                                  std::format("'kind' ({}) must fit in [0, {}]",
                                              v, std::numeric_limits<std::uint32_t>::max()));
                        continue;
                    }
                    info.kind = static_cast<std::uint32_t>(v);
                }
                // `formula` (optional) is opaque to the substrate but
                // type-strict: a present-but-wrong-type field is
                // diagnosed, NOT silently dropped (mirrors the
                // `terminatorKind` discipline above).
                if (r.contains("formula")) {
                    if (!r.at("formula").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/relocations/{}/formula", i),
                                  "'formula' must be a string");
                    } else {
                        info.formula = r.at("formula").get<std::string>();
                    }
                }
                std::uint16_t const idx =
                    static_cast<std::uint16_t>(data.relocations.size());
                bool const freshName =
                    data.relocationNameIndex.emplace(info.name, idx).second;
                if (!freshName) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/relocations/{}/name", i),
                              std::format("duplicate relocation name '{}'", info.name));
                    continue;  // skip push_back so vector & index stay in sync
                }
                // Parallel kind-index for the linker hot path. Uniqueness
                // is also checked by validate() (it produces the user-
                // facing diagnostic with cross-row blame); here we just
                // need the index entry to exist so `relocationInfo(kind)`
                // is O(1) for the rows that DID load. If a later row
                // collides on `kind`, validate() will emit the error and
                // load is rejected, so the index's "first writer wins"
                // shape never reaches consumers.
                (void)data.relocationKindIndex.emplace(info.kind, idx);
                data.relocations.push_back(std::move(info));
            }
        }
    }

    // ── Cross-field invariants (validate after per-field parse) ───
    // `validate()` returns pre-shaped `ConfigDiagnostic`s carrying their
    // specific JSON paths (`/opcodes/3/maxSuccessors`, etc.). Append-as-
    // is instead of reshaping under a single sourceLabel path — the path
    // is the load-bearing locator for the user fixing the config.
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

    return std::make_shared<TargetSchema>(std::move(data));
}

} // namespace dss
