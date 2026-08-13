#pragma once

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"

#include <concepts>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Shared validator substrate for `relocations[]` tables — the TYPED
// half. The JSON loader half lives in `relocation_table_json.hpp`.
//
// ── WHY THE SPLIT ────────────────────────────────────────────────────
// This header used to carry `loadRelocationsTable` too, and with it
// `<nlohmann/json.hpp>`, which it then put on every consumer — against
// the standing convention that a public header does not. ✔MEASURED
// 2026-08-13 (D-CORE-JSON-LEAKS-INTO-TWO-PUBLIC-HEADERS): the consumers
// divide exactly along file naming with NO overlap — the two `_json.cpp`
// readers use only the loaders, `object_format_schema.cpp` and
// `target_schema.cpp` use only the validator below. Splitting therefore
// costs no caller anything and stops those two TUs pulling nlohmann
// transitively for a function that never touches it.
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
