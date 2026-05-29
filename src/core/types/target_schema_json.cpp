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
        // Booleans (optional, default false).
        if (o.contains("isTerminator") && o.at("isTerminator").is_boolean()) {
            info.isTerminator = o.at("isTerminator").get<bool>();
        }
        if (o.contains("hasSideEffects") && o.at("hasSideEffects").is_boolean()) {
            info.hasSideEffects = o.at("hasSideEffects").get<bool>();
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
                    auto const s = r.at("class").get<std::string>();
                    if      (s == "gpr")   info.regClass = TargetRegClass::GPR;
                    else if (s == "fpr")   info.regClass = TargetRegClass::FPR;
                    else if (s == "vr")    info.regClass = TargetRegClass::VR;
                    else if (s == "flags") info.regClass = TargetRegClass::Flags;
                    else if (s == "none")  info.regClass = TargetRegClass::None;
                    else {
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
