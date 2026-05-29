#pragma once

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Shared loader + validator substrate for `relocations[]` tables.
//
// Both `TargetSchema` (assembler-side formula owner) and
// `ObjectFormatSchema` (linker-side platform-native name owner)
// declare a `relocations[]` JSON array whose row shape ALWAYS
// includes `{name, kind}` (the universal join key per plan 13 §2.6's
// cross-side reloc-taxonomy unifier) and MAY include row-specific
// extension fields (e.g. target's `formula`). This substrate hoists
// the common loader + validator so the two sides cannot drift on the
// `{name, kind}` contract — symmetry by construction, not by review.
//
// Refactor target: code-simplifier review of AS6 + LK4 (Findings 2 + 3).
// The TargetSchema + ObjectFormatSchema callers populate row-specific
// fields via the `extendRow` callback; everything else (uniqueness,
// non-zero kind, non-empty name, dual indexing) lives here.

namespace dss::substrate {

// Load a `relocations[]` array from `doc` into `out`, populating
// the dual O(1) lookup indices in parallel. `extendRow` runs after
// the universal `{name, kind}` extraction; pass an empty lambda when
// the row has no extension fields. `kindIndex.emplace` is intentionally
// last-wins on the duplicate-kind path; cross-row uniqueness is
// enforced upstream by `validateRelocationsTable()` so a duplicate kind
// never escapes the loader.
template <typename RowT, typename ExtendRowFn>
void loadRelocationsTable(
    nlohmann::json const& doc,
    std::vector<RowT>& out,
    TransparentStringMap<std::uint16_t>& nameIndex,
    std::unordered_map<RelocationKind, std::uint16_t>& kindIndex,
    DiagnosticCollector& coll,
    ExtendRowFn extendRow) {
    if (!doc.contains("relocations")) return;
    if (!doc.at("relocations").is_array()) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/relocations",
                  "'relocations' must be an array");
        return;
    }
    auto const& rels = doc.at("relocations");
    out.reserve(rels.size());
    for (std::size_t i = 0; i < rels.size(); ++i) {
        auto const& r = rels[i];
        if (!r.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/relocations/{}", i),
                      "relocation entry must be an object");
            continue;
        }
        RowT info;
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
                      "missing or non-integer 'kind' (must be the non-zero "
                      "uint32 tag that matches the cross-side "
                      "TargetSchema / ObjectFormatSchema relocations[] table)");
            continue;
        }
        {
            std::int64_t const v = r.at("kind").get<std::int64_t>();
            if (v < 0
             || v > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::format("/relocations/{}/kind", i),
                          std::format("'kind' ({}) must fit in [0, {}]",
                                      v, std::numeric_limits<std::uint32_t>::max()));
                continue;
            }
            info.kind = RelocationKind{static_cast<std::uint32_t>(v)};
        }
        // Row-specific extension fields (e.g. target's `formula`).
        // Skips the row if the callback emits a fatal-flagged diagnostic.
        if (!extendRow(r, info, coll, i)) continue;

        std::uint16_t const idx = static_cast<std::uint16_t>(out.size());
        bool const freshName = nameIndex.emplace(info.name, idx).second;
        if (!freshName) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/relocations/{}/name", i),
                      std::format("duplicate relocation name '{}'", info.name));
            continue;
        }
        (void)kindIndex.emplace(info.kind, idx);
        out.push_back(std::move(info));
    }
}

// Cross-row uniqueness + sentinel + non-empty validation. Shared
// across both sides of the reloc-taxonomy unifier so a row that
// passed the loader still gets the same cross-field discipline
// regardless of which schema owns it. `fail` is the caller's
// diagnostic-append callback (matches the per-callsite `fail` lambda
// shape every `validate()` method already uses).
template <typename RowT, typename FailFn>
void validateRelocationsTable(std::span<RowT const> rels, FailFn fail) {
    std::unordered_map<RelocationKind, std::size_t> seenKind;
    for (std::size_t i = 0; i < rels.size(); ++i) {
        auto const& r = rels[i];
        if (r.name.empty()) {
            fail(std::format("/relocations/{}/name", i),
                 "relocation row: 'name' must be a non-empty string");
        }
        if (!r.kind.valid()) {
            fail(std::format("/relocations/{}/kind", i),
                 std::format("relocation '{}': 'kind' must be != 0 "
                             "(slot 0 is reserved as the invalid sentinel)",
                             r.name));
            continue;
        }
        auto [it, fresh] = seenKind.emplace(r.kind, i);
        if (!fresh) {
            fail(std::format("/relocations/{}/kind", i),
                 std::format("relocation '{}': duplicate 'kind' value {} "
                             "(already declared by relocation '{}' at "
                             "/relocations/{})",
                             r.name, r.kind.v,
                             rels[it->second].name, it->second));
        }
    }
}

} // namespace dss::substrate
