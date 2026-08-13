#pragma once

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/relocation_table.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// JSON loader half of the `relocations[]` substrate. The typed half —
// the `relocation_row` concept and `validateRelocationsTable` — lives
// in `relocation_table.hpp` and pulls no JSON.
//
// ── WHY THIS FILE EXISTS (the `_json` split convention) ──────────────
// `core` links nlohmann PRIVATE, and the standing convention is that a
// public header does not put `<nlohmann/json.hpp>` on its consumers.
// This file is the sanctioned EXCEPTION shape, the same one
// `grammar_schema_json.hpp` / `target_schema_json.cpp` /
// `predefined_macro_json.hpp` already use: a header whose declared job
// IS to accept a `nlohmann::json const&` names itself `_json` and is
// included only by TUs that already hold a parsed document.
//
// ⚠ The exception is NOT a loophole, and the distinction is mechanical
// rather than a matter of taste: a parser interface cannot hide the
// type it parses. These loaders are TEMPLATES, so their definitions
// must be visible to instantiate — there is no `.cpp` to hide the
// include in, and an opaque wrapper would be handed straight back to
// nlohmann one line later by a caller who already parsed the file.
// Hiding JSON here is not expensive; it is not expressible.
//
// ✔MEASURED 2026-08-13 (D-CORE-JSON-LEAKS-INTO-TWO-PUBLIC-HEADERS) —
// the split is not cosmetic. Consumers divide exactly along the file
// naming, with no overlap: `object_format_schema_json.cpp` and
// `target_schema_json.cpp` use ONLY the loaders; `object_format_schema.cpp`
// and `target_schema.cpp` use ONLY the validator. Those last two now
// stop pulling nlohmann transitively, which is the whole point.

namespace dss::substrate {

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

} // namespace dss::substrate
