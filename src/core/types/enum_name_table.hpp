#pragma once

// ── Closed-enum name table (substrate) ────────────────────────────
//
// Many closed enums across the config layer carry `XxxName(e)` /
// `XxxFromName(s)` constexpr helpers. This template gives ONE source of truth
// (the `kXxxTable` array) and derives both helpers from it. Adding a new
// enumerator: one row in the table, no helper edits. The `entry == e` lookup is
// linear but the enums are tiny (<= ~12 entries) and the helpers are constexpr —
// the cost is one branch per entry, eliminated by the compiler on constant
// evaluation.
//
// This lives in its own dependency-free header (only <array>/<optional>/
// <string_view>/<utility>) so leaf enum headers (`object_format_kind.hpp`,
// `data_model.hpp`, …) can define their tables WITHOUT pulling in the heavy
// `target_schema.hpp` — which itself depends on `grammar_schema.hpp`
// (`ConfigDiagnostic`) and would otherwise form an include cycle when a leaf enum
// header is reached early in `grammar_schema.hpp`'s own include list.

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace dss {

template <typename E, std::size_t N>
struct EnumNameTable {
    std::array<std::pair<E, std::string_view>, N> rows;

    [[nodiscard]] constexpr std::string_view name(E e) const noexcept {
        for (auto const& r : rows) {
            if (r.first == e) return r.second;
        }
        // Fall-back returns the FIRST row's string — semantically a
        // sentinel "unknown/invalid". Each enum's `name()` historically
        // had its own fall-back string; using row 0 means each enum
        // controls its fall-back by ordering the table.
        return rows[0].second;
    }

    [[nodiscard]] constexpr std::optional<E> fromName(std::string_view s) const noexcept {
        for (auto const& r : rows) {
            if (r.second == s) return r.first;
        }
        return std::nullopt;
    }
};

} // namespace dss
