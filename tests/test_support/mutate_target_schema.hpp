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
#include <utility>

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

namespace detail {

// Shared load+parse half of the two mutation entry points below.
// Returns the parsed shipped JSON document, or the failure envelope.
[[nodiscard]] inline LoadResult<nlohmann::json>
parseShippedTargetJson(std::string_view targetName) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{targetName, "targets", ".target.json",
                             "target", DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        return std::unexpected(std::move(pathR).error());
    }

    std::ifstream in{*pathR};
    if (!in.is_open()) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             pathR->string(), "parseShippedTargetJson: cannot open "
                              "shipped target JSON"}});
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    try {
        return nlohmann::json::parse(buf.str());
    } catch (nlohmann::json::parse_error const& e) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_InvalidSemantics, DiagnosticSeverity::Error,
             pathR->string(),
             std::string{"parseShippedTargetJson: JSON parse error in "
                         "shipped schema: "} + e.what()}});
    }
}

}  // namespace detail

// Generalized in-memory schema mutation (FC1 V2-4.X, 2026-06-10):
// load the shipped target JSON, hand the parsed document to `mutate`
// (any in-place transform), re-construct a `TargetSchema` from the
// mutated text. The remove-mnemonics helper below is the common
// special case; tests needing finer-grained surgery (e.g. stripping
// ONE sub-key off one opcode's `implicitRegisters` to exercise a
// lowering fail-loud arm) pass a lambda instead of authoring a
// parallel broken JSON file (which would rot against the shipped
// one — the cycle-10k rationale above applies unchanged).
template <typename MutateFn>
[[nodiscard]] inline LoadResult<std::shared_ptr<TargetSchema>>
mutateShippedTargetSchemaDoc(std::string_view targetName,
                             MutateFn&& mutate) {
    auto docR = detail::parseShippedTargetJson(targetName);
    if (!docR.has_value()) {
        return std::unexpected(std::move(docR).error());
    }
    nlohmann::json doc = *std::move(docR);
    std::forward<MutateFn>(mutate)(doc);
    return TargetSchema::loadFromText(doc.dump(),
        std::string{"<mutated "} + std::string{targetName} + ">");
}

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
    return mutateShippedTargetSchemaDoc(targetName,
        [&drop](nlohmann::json& doc) {
            if (!doc.contains("opcodes") || !doc["opcodes"].is_array()) {
                return;
            }
            auto& opcodes = doc["opcodes"];
            opcodes.erase(
                std::remove_if(
                    opcodes.begin(), opcodes.end(),
                    [&drop](nlohmann::json const& entry) {
                        auto it = entry.find("mnemonic");
                        if (it == entry.end() || !it->is_string()) {
                            return false;
                        }
                        return drop.contains(it->get<std::string>());
                    }),
                opcodes.end());
        });
}

} // namespace dss::test_support
