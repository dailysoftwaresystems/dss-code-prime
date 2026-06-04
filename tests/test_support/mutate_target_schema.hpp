#pragma once

#include "core/types/config_path_walk.hpp"
#include "core/types/target_schema.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

// D-CSUBSET-LOCAL-INT-CODEGEN-NEGATIVE-PIN substrate (cycle 10k,
// 2026-06-04): test-tier helper that loads a shipped target schema,
// strips named opcode rows, and returns a freshly-constructed
// TargetSchema built from the mutated JSON.
//
// Why this is test-tier: shipped configs are always-correct by
// construction (a misconfiguration would have been caught by CI long
// ago); negative-pin tests that exercise schema misconfiguration code
// paths need a way to *synthesize* a misconfiguration. The cleanest
// long-term shape is: don't author a parallel "broken" JSON file
// (would rot vs. shipped + couples tests to one specific shape);
// instead mutate the shipped file in-memory so the test pins the
// SUBSTRATE'S handling of an absent opcode, not a particular JSON
// shape.
//
// **Long-term design**: returns a `LoadResult<std::shared_ptr<...>>`
// — same envelope as `TargetSchema::loadFromText` so callers consume
// it with `ASSERT_TRUE(result.has_value())`. The helper does NOT
// silently degrade on a malformed shipped JSON (caller can inspect
// the error vector); the only "soft" path is when a `removeMnemonics`
// entry isn't found in the JSON — that's silently accepted (caller's
// declared intent is "ensure this opcode is absent"; the absent-from-
// start case is a vacuous success).
//
// **Agnosticism**: target-name-driven via `findShippedConfig`; works
// for any target (x86_64, arm64, etc.) by JSON-only addition. No
// hardcoded target string in this helper.

namespace dss::test_support {

// Load the shipped target schema for `targetName`, parse its JSON,
// remove every opcode row whose `mnemonic` field matches any entry
// in `removeMnemonics`, and re-construct a `TargetSchema` from the
// resulting JSON text. Returns the schema on success or a
// `ConfigDiagnostic` vector on failure (mirrors
// `TargetSchema::loadFromText`'s envelope).
//
// Use cases:
//   - Negative-pin tests for `materializeCallingConvention`'s
//     "schema declares X but not Y" fail-loud arms.
//   - Future cycle's regression guards against silent-failure
//     classes triggered by absent opcodes.
[[nodiscard]] inline LoadResult<std::shared_ptr<TargetSchema>>
mutateShippedTargetSchemaJson(
    std::string_view targetName,
    std::initializer_list<std::string_view> removeMnemonics) {

    auto pathR = findShippedConfig(
        ShippedConfigLocator{targetName, "targets", ".target.json",
                             "target", DiagnosticCode::C_InvalidLanguageName});
    if (!pathR.has_value()) {
        return std::unexpected(std::move(pathR).error());
    }

    std::ifstream in{*pathR};
    if (!in.is_open()) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             pathR->string(), "mutateShippedTargetSchemaJson: cannot "
                              "open shipped target JSON"}});
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(buf.str());
    } catch (nlohmann::json::parse_error const& e) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_InvalidSemantics, DiagnosticSeverity::Error,
             pathR->string(),
             std::string{"mutateShippedTargetSchemaJson: JSON parse "
                         "error in shipped schema: "} + e.what()}});
    }

    // Build the removal set (string_view → string copies for stable
    // storage during the erase walk).
    std::unordered_set<std::string> drop;
    drop.reserve(removeMnemonics.size());
    for (std::string_view const m : removeMnemonics) {
        drop.emplace(m);
    }

    // Strip matching opcode rows. Silently tolerant: an entry not
    // present in the JSON is a vacuous removal — caller's intent
    // ("ensure this mnemonic is absent") is satisfied.
    if (doc.contains("opcodes") && doc["opcodes"].is_array()) {
        auto& opcodes = doc["opcodes"];
        opcodes.erase(
            std::remove_if(
                opcodes.begin(), opcodes.end(),
                [&drop](nlohmann::json const& entry) {
                    auto it = entry.find("mnemonic");
                    if (it == entry.end() || !it->is_string()) return false;
                    return drop.contains(it->get<std::string>());
                }),
            opcodes.end());
    }

    std::string const mutatedText = doc.dump();
    return TargetSchema::loadFromText(mutatedText,
        std::string{"<mutated "} + std::string{targetName} + ">");
}

} // namespace dss::test_support
