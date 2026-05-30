#pragma once

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"

#include <nlohmann/json.hpp>

#include <concepts>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
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
// `{name, kind}` contract — symmetry by construction.
//
// The substrate carries no target / format / linker knowledge. A
// third consumer (e.g. a debug-info schema) plugs in by declaring a
// `RowT` satisfying the `relocation_row` concept and a row-specific
// `extendRow` callback.

namespace dss::substrate {

// Universal `{name, kind}` join key the substrate enforces. Any
// `relocations[]` row type must expose at least these two fields.
template <typename T>
concept relocation_row = requires(T& t) {
    { t.name } -> std::same_as<std::string&>;
    { t.kind } -> std::same_as<RelocationKind&>;
};

// Load a `relocations[]` array from `doc` into `out`, populating
// the dual O(1) lookup indices in parallel. `extendRow` runs after
// the universal `{name, kind}` extraction; **return false from
// `extendRow` to SKIP the row** (no push to `out`, no index
// insertion) when the row failed a row-specific shape check.
// Return value is independent of whether `extendRow` emitted a
// diagnostic — the caller decides.
//
// Duplicate `kind` is detected at the loader level (the
// downstream `validateRelocationsTable` is the belt-and-suspenders
// catch, but the loader fails loud here so a row with a duplicate
// `kind` can never reach the dual indices).
template <relocation_row RowT, typename ExtendRowFn>
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
                      "uint32 tag that joins this row to its peer in the "
                      "cross-side relocations[] table)");
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
        // Loader-side duplicate-kind detection (defense-in-depth
        // with `validateRelocationsTable`). `validate()` would
        // catch the duplicate later, but a caller that bypasses
        // the full loader (e.g. unit tests constructing schema
        // data directly) would otherwise leave the dual indices
        // pointing at the FIRST occurrence silently.
        if (auto it = kindIndex.find(info.kind); it != kindIndex.end()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/relocations/{}/kind", i),
                      std::format("duplicate 'kind' value {} (already "
                                  "declared by relocation '{}' at "
                                  "/relocations/{})",
                                  info.kind.v,
                                  out[it->second].name, it->second));
            continue;
        }
        // Row-specific extension fields (e.g. target's `formula`).
        // **Return false to SKIP the row** — neither push nor index.
        if (!extendRow(r, info, coll, i)) continue;

        std::uint16_t const idx = static_cast<std::uint16_t>(out.size());
        bool const freshName = nameIndex.emplace(info.name, idx).second;
        if (!freshName) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::format("/relocations/{}/name", i),
                      std::format("duplicate relocation name '{}'", info.name));
            continue;
        }
        kindIndex.emplace(info.kind, idx);
        out.push_back(std::move(info));
    }
}

// Convenience overload — for rows without extension fields. The
// format-side `ObjectFormatRelocationInfo` is the canonical no-
// extension consumer; eliminates the 4-line empty lambda at the
// callsite.
template <relocation_row RowT>
void loadRelocationsTable(
    nlohmann::json const& doc,
    std::vector<RowT>& out,
    TransparentStringMap<std::uint16_t>& nameIndex,
    std::unordered_map<RelocationKind, std::uint16_t>& kindIndex,
    DiagnosticCollector& coll) {
    loadRelocationsTable(doc, out, nameIndex, kindIndex, coll,
        [](nlohmann::json const&, RowT&,
           DiagnosticCollector&, std::size_t) -> bool { return true; });
}

// Cross-row uniqueness + sentinel + non-empty validation. Shared
// across both sides of the reloc-taxonomy unifier. Runs DOWNSTREAM
// of the loader as a belt-and-suspenders catch for rows that
// reached `out` through a bypass path (e.g. unit tests constructing
// schema data directly). `fail` is the caller's diagnostic-append
// callback (matches the per-callsite `fail` lambda shape every
// `validate()` method already uses).
template <relocation_row RowT, typename FailFn>
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
