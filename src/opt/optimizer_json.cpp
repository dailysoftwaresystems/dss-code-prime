#include "opt/optimizer.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/types/config_path_walk.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// `*.pipeline.json` loader. Mirrors the 7-step shape shared by every
// shipped config (`TargetSchema::loadFromText` / `GrammarSchema` / ...):
// parse → version → required → optional → enum-resolve → validate → return.
//
// Schema (D-OPT1-PIPELINE-FROM-CONFIG):
//   { "dssPipelineVersion": 1,
//     "pipeline": { "name": "<str>", "passes": ["<PassId-name>", ...] } }
//
// Unknown sub-keys under `pipeline` are rejected (D-CONFIG-LOADER-
// UNKNOWN-KEYS-FAIL-LOUD). Unknown top-level keys are also rejected so
// a typo in `dssPipelineVersion` doesn't silently load with the default.

namespace dss::opt {

namespace {
using json = nlohmann::json;

void emitMalformed(substrate::DiagnosticCollector& coll,
                   std::string path, std::string what) {
    coll.emit(DiagnosticCode::X_PipelineMalformed,
              std::move(path), std::move(what));
}

// D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD enforcement for any JSON
// object whose key set is fully closed. Emits X_PipelineMalformed
// (with object-path context) for every key not in the allow-list.
void rejectUnknownKeys(substrate::DiagnosticCollector& coll,
                       nlohmann::json const& obj,
                       std::string const& objPath,
                       std::initializer_list<std::string_view> allowed) {
    for (auto const& kv : obj.items()) {
        bool known = false;
        for (auto k : allowed) { if (kv.key() == k) { known = true; break; } }
        if (!known) {
            emitMalformed(coll, objPath,
                          std::format("unknown key '{}' "
                                      "(D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD)",
                                      kv.key()));
        }
    }
}

[[nodiscard]] LoadResult<OptPipeline>
parsePipelineDoc(json const& doc, std::string_view sourceLabel) {
    substrate::DiagnosticCollector coll;

    if (!doc.is_object()) {
        emitMalformed(coll, std::string{sourceLabel},
                      "top-level value must be a JSON object");
        return std::unexpected(std::move(coll).release());
    }

    // Step 2: version gate.
    if (!doc.contains("dssPipelineVersion")
     || !doc.at("dssPipelineVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::X_PipelineVersionMismatch,
                  std::string{sourceLabel},
                  "missing or non-integer 'dssPipelineVersion'");
        return std::unexpected(std::move(coll).release());
    }
    int const ver = doc.at("dssPipelineVersion").get<int>();
    if (ver != 1) {
        coll.emit(DiagnosticCode::X_PipelineVersionMismatch,
                  "/dssPipelineVersion",
                  std::format("only version 1 supported (got {})", ver));
        return std::unexpected(std::move(coll).release());
    }

    // Step 3: required `pipeline` object.
    if (!doc.contains("pipeline") || !doc.at("pipeline").is_object()) {
        emitMalformed(coll, std::string{sourceLabel},
                      "missing 'pipeline' object");
        return std::unexpected(std::move(coll).release());
    }
    json const& pipe = doc.at("pipeline");

    rejectUnknownKeys(coll, doc, std::string{sourceLabel},
                      {"dssPipelineVersion", "pipeline"});

    // `pipeline.name`.
    if (!pipe.contains("name") || !pipe.at("name").is_string()) {
        emitMalformed(coll, std::string{sourceLabel} + "/pipeline",
                      "missing or non-string 'name'");
        return std::unexpected(std::move(coll).release());
    }
    std::string const name = pipe.at("name").get<std::string>();

    // `pipeline.passes` — array of pass-name strings.
    if (!pipe.contains("passes") || !pipe.at("passes").is_array()) {
        emitMalformed(coll, std::string{sourceLabel} + "/pipeline",
                      "missing or non-array 'passes'");
        return std::unexpected(std::move(coll).release());
    }
    std::vector<PassId> passes;
    passes.reserve(pipe.at("passes").size());
    std::size_t idx = 0;
    for (auto const& el : pipe.at("passes")) {
        if (!el.is_string()) {
            emitMalformed(coll,
                          std::format("{}/pipeline/passes[{}]", sourceLabel, idx),
                          "pass entry must be a string");
            ++idx;
            continue;
        }
        std::string const s = el.get<std::string>();
        auto const resolved = optPassIdFromName(s);
        if (!resolved.has_value()) {
            coll.emit(DiagnosticCode::X_UnknownPassName,
                      std::format("{}/pipeline/passes[{}]", sourceLabel, idx),
                      std::format("unknown PassId name '{}' "
                                  "(not in optPassIdFromName)", s));
            ++idx;
            continue;
        }
        passes.push_back(*resolved);
        ++idx;
    }

    // Pipeline-level fixed-point loop (D-OPT-FIXED-POINT-LOOP +
    // D-OPT1-PASS-RUN-MAX-ITER). Optional. Default = 1 (single
    // iteration — historical behavior). Rejected outside
    // [1, kMaxPipelineIterations]: 0 is a silent-no-op trap, and
    // values > 32 indicate non-convergence or pathological input
    // (every realistic mutually-enabling cluster converges in
    // < log(blockCount) iterations).
    std::uint8_t maxIterations = 1;
    if (pipe.contains("maxIterations")) {
        if (!pipe.at("maxIterations").is_number_integer()) {
            emitMalformed(coll, std::string{sourceLabel} + "/pipeline/maxIterations",
                          "must be an integer");
        } else {
            auto const v = pipe.at("maxIterations").get<std::int64_t>();
            if (v < 1 || v > kMaxPipelineIterations) {
                emitMalformed(coll,
                    std::string{sourceLabel} + "/pipeline/maxIterations",
                    std::format("must be in [1, {}] (got {})",
                                static_cast<int>(kMaxPipelineIterations), v));
            } else {
                maxIterations = static_cast<std::uint8_t>(v);
            }
        }
    }

    rejectUnknownKeys(coll, pipe, std::string{sourceLabel} + "/pipeline",
                      {"name", "passes", "maxIterations"});

    // Empty pipeline = silent no-op at the optimizer engine. Reject
    // at load-time so a stray `"passes": []` doesn't ship a build
    // that thinks optimization happened but ran zero passes.
    if (passes.empty()) {
        emitMalformed(coll, std::string{sourceLabel} + "/pipeline/passes",
                      "'passes' array must contain at least one PassId; "
                      "use [\"Identity\"] for an explicit no-op pipeline");
    }
    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll).release());
    }
    return OptPipeline{std::move(name), std::move(passes), maxIterations};
}

} // namespace

LoadResult<OptPipeline>
loadPipelineFromText(std::string_view jsonText, std::string_view sourceLabel) {
    substrate::DiagnosticCollector coll;
    json doc;
    try {
        doc = json::parse(jsonText);
    } catch (json::parse_error const& e) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  std::format("JSON parse error: {}", e.what()));
        return std::unexpected(std::move(coll).release());
    }
    return parsePipelineDoc(doc, sourceLabel);
}

LoadResult<OptPipeline>
loadShippedPipeline(std::string_view name) {
    auto pathR = findShippedConfig({
        name, "pipelines", ".pipeline.json", "pipeline",
        DiagnosticCode::X_PipelineNameResolutionFailed});
    if (!pathR.has_value()) {
        return std::unexpected(std::move(pathR).error());
    }
    auto const path = pathR.value();
    std::ifstream in{path};
    if (!in) {
        substrate::DiagnosticCollector coll;
        coll.emit(DiagnosticCode::X_PipelineNameResolutionFailed,
                  path.string(),
                  "failed to open pipeline file for reading");
        return std::unexpected(std::move(coll).release());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return loadPipelineFromText(ss.str(), path.string());
}

} // namespace dss::opt
