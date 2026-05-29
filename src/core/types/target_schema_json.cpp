#include "core/types/target_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
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

[[nodiscard]] std::uint32_t mintTargetSchemaId() noexcept {
    static std::atomic<std::uint32_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
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

    TargetSchemaData data;
    data.id = TargetSchemaId{mintTargetSchemaId()};

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
        // Arity bounds (optional, default 0). Stored as uint8 so >255 is
        // clamped + diagnosed.
        auto readByte = [&](std::string_view field,
                            std::uint8_t& out) {
            if (!o.contains(field)) return;
            if (!o.at(std::string{field}).is_number_integer()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/{}", i, field),
                          "must be a non-negative integer");
                return;
            }
            std::int64_t const v = o.at(std::string{field}).get<std::int64_t>();
            if (v < 0 || v > 255) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/opcodes/{}/{}", i, field),
                          "must fit in [0, 255]");
                return;
            }
            out = static_cast<std::uint8_t>(v);
        };
        readByte("minOperands",   info.minOperands);
        readByte("maxOperands",   info.maxOperands);
        readByte("minSuccessors", info.minSuccessors);
        readByte("maxSuccessors", info.maxSuccessors);
        // Slot-0 Invalid-sentinel sanity check.
        if (i == 0 && info.mnemonic != "invalid") {
            coll.emit(DiagnosticCode::C_MalformedJson, "/opcodes/0",
                      "first opcode must be the 'invalid' sentinel "
                      "(its index is the default-constructed inst's "
                      "opcode field; substrate `addInst` rejects opcode 0)");
        }
        std::uint16_t const idx = static_cast<std::uint16_t>(data.opcodes.size());
        if (data.mnemonicIndex.emplace(info.mnemonic, idx).second == false) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/opcodes/{}/mnemonic", i),
                      std::format("duplicate mnemonic '{}'", info.mnemonic));
        }
        data.opcodes.push_back(std::move(info));
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
