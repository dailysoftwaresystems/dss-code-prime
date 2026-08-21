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

    // The LOOKUP, and the only loop over `rows` in this struct. `name` and
    // `nameOrEmpty` differ ONLY in what they do about a miss, and writing the
    // scan twice is how the two would eventually start disagreeing about what
    // a miss even is.
    [[nodiscard]] constexpr std::optional<std::string_view>
    findName(E e) const noexcept {
        for (auto const& r : rows) {
            if (r.first == e) return r.second;
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::string_view name(E e) const noexcept {
        // Fall-back returns the FIRST row's string — semantically a
        // sentinel "unknown/invalid". Each enum's `name()` historically
        // had its own fall-back string; using row 0 means each enum
        // controls its fall-back by ordering the table.
        auto const found = findName(e);
        return found ? *found : rows[0].second;
    }

    // ── THE STRICT PROJECTION: "this value has no spelling" ────────────────
    //
    // `name()`'s row-0 fall-back is the RIGHT answer for a table whose row 0 is
    // the invalid sentinel, and the WRONG answer for a table that deliberately
    // does not list its sentinel at all: the unlisted value comes back wearing
    // the FIRST REAL ENTRY'S NAME, which reads as a legitimate declaration.
    //
    // ★ THIS GAP IS WHY THREE ENUMS IN THIS TREE WERE HAND-ROLLED. `object_format_kind.hpp`'s
    // `cSymbolDecorationSchemeName` writes the reason out in full: neither
    // shape `EnumNameTable` offered could express "this value has no
    // spelling" — listing the sentinel with an empty name makes
    // `fromName("")` RESOLVE TO IT, re-opening the hole from the other side —
    // so `dataModelName`, `headerNameMatchingName` and
    // `cSymbolDecorationSchemeName` were each written as a switch, and each
    // therefore became a SECOND OWNER of its enum's spellings alongside its
    // `…FromName` sibling.
    //
    // `nameOrEmpty` closes that gap in the SHARED template rather than at each
    // site: leave the sentinel OUT of the table (so `fromName` cannot resolve
    // it, from any spelling including `""`), and ask for the name here (so an
    // unlisted value renders EMPTY). That is exactly the contract the
    // `…Name(x).empty()` idiom in `ObjectFormatData::validate()` depends on.
    // All three of those enums are converted; none is hand-rolled any more, and
    // the `-Werror=switch` guarantee each one cited as its second reason is kept
    // by a switch that owns no spelling (see `dataModelName`).
    //
    // ⚠ Use this, NOT `name()`, at any site whose enum has an unlisted
    // sentinel value. `name()` stays for the tables that DO list theirs.
    [[nodiscard]] constexpr std::string_view nameOrEmpty(E e) const noexcept {
        auto const found = findName(e);
        return found ? *found : std::string_view{};
    }

    [[nodiscard]] constexpr std::optional<E> fromName(std::string_view s) const noexcept {
        for (auto const& r : rows) {
            if (r.second == s) return r.first;
        }
        return std::nullopt;
    }
};

// ── projecting the SELECTABLE spellings of a table ────────────────────────
//
// Several closed vocabularies are declared with rows a config author may NOT
// write: an invalid sentinel (`ObjectFormatKind::Unknown`,
// `EntryParamShape::None`), or a superset enum of which only a subrange is
// legal at one site (`SectionKind` where a DATA section is required). Their
// diagnostics have to state the ACCEPTED set, which is the table minus those
// rows — and every one of them stated it by RETYPING the surviving names as a
// string literal beside the check (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
// A second owner of one fact: the rows keep working while the sentence quietly
// becomes a lie, and no test can see it because the sentence is what the test
// reads.
//
// `M` — the number of rows `pred` is expected to ACCEPT — is written at the
// call site and CHECKED, not trusted. A mismatch (a new enumerator, a second
// sentinel) makes the call a non-constant expression, so the `inline constexpr`
// initializer that uses it FAILS THE BUILD. It cannot silently truncate the
// list, and it cannot leave a zero-filled `""` slot that the renderer would
// print as `''`.
template <std::size_t M, typename E, std::size_t N, typename Pred>
[[nodiscard]] constexpr std::array<std::string_view, M>
namesWhere(EnumNameTable<E, N> const& table, Pred pred) {
    std::array<std::string_view, M> out{};
    std::size_t accepted = 0;
    for (auto const& r : table.rows) {
        if (!pred(r.first)) continue;
        if (accepted < M) out[accepted] = r.second;
        ++accepted;
    }
    if (accepted != M) {
        // Reached only in a constant-evaluated context, where a throw is not a
        // constant expression — i.e. this is a COMPILE ERROR at the caller's
        // initializer, naming this line. Deliberately not an assert: the whole
        // point is that the count is checked where the array is BUILT.
        throw "namesWhere<M>: M does not match the number of accepted rows";
    }
    return out;
}

// Every spelling in the table, in declaration order — the unfiltered case of
// `namesWhere`, and written in terms of it so there is still exactly one loop.
//
// ⓘ `dss::detail::keysOf` (config_key_vocabulary.hpp) answers the same question
// for the OTHER row shape — a `pair<name, value>` config vocabulary, name
// first. This one is for `EnumNameTable`, whose rows are value-first. They are
// not interchangeable and neither is a copy of the other; the difference is
// which half of the pair is the key.
template <typename E, std::size_t N>
[[nodiscard]] constexpr std::array<std::string_view, N>
allNames(EnumNameTable<E, N> const& table) {
    return namesWhere<N>(table, [](E) { return true; });
}

// ---------------------------------------------------------------------------
// WELL-FORMEDNESS -- the guard the `N` in `EnumNameTable<E, N>` does not give you
// ---------------------------------------------------------------------------
//
// ⚠ ✔MEASURED 2026-08-20 (cycle P23,
// `D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE`):
// `EnumNameTable<E, 30>` written with 29 initializers is LEGAL C++. The missing
// row value-initializes to `{E(0), ""}` -- so `fromName("")` resolves to
// whatever enumerator happens to be 0, and for a vocabulary whose 0 is a
// sentinel (`None`, `Default`, `Unspecified`) that means an EMPTY STRING
// silently selects "this knob does nothing". A duplicate spelling is the same
// class one step over: the second row is unreachable through `fromName` and
// nothing says so.
//
// ★ `namesWhere<M>` already refuses a count mismatch, but only at a call site
// that USES it -- a table nobody projects is unchecked, and most are declared
// long before their first projection. This predicate is the check that belongs
// to the TABLE rather than to its readers.
//
// ⓘ Deliberately a free `constexpr` predicate plus a macro rather than a
// constructor precondition: `EnumNameTable` is an aggregate, and making it
// non-aggregate would cost every existing brace-init in the tree.
template <typename E, std::size_t N>
[[nodiscard]] constexpr bool
isWellFormedEnumNameTable(EnumNameTable<E, N> const& table) {
    for (std::size_t i = 0; i < N; ++i) {
        if (table.rows[i].second.empty()) return false;
        for (std::size_t j = i + 1; j < N; ++j) {
            if (table.rows[i].second == table.rows[j].second) return false;
            if (table.rows[i].first == table.rows[j].first) return false;
        }
    }
    return true;
}

} // namespace dss

// Assert a table is well formed, at the point of declaration. Use it on EVERY
// `EnumNameTable`: an under-filled or duplicated table is a silent miscompile
// of the vocabulary, not a compile error, until something asks for the row that
// is not there.
//
// ⓘ Sibling of `DSS_CHECK_KEY_VOCABULARY` in `config_key_vocabulary.hpp`, which
// answers the same question for the OTHER row shape (name-first config
// vocabularies). This one lives here because `enum_name_table.hpp` is
// dependency-free by design and public type headers may include it -- which
// `config_key_vocabulary.hpp` forbids for itself.
#define DSS_CHECK_ENUM_NAME_TABLE(table)                                           static_assert(::dss::isWellFormedEnumNameTable(table),                                       #table " is malformed: a row carries an EMPTY spelling "                              "(an under-filled table value-initializes the "                                "missing rows), or two rows share a spelling or an "                           "enumerator. See D-CORE-ENUM-NAME-TABLE-HAS-NO-"                               "WELL-FORMEDNESS-PREDICATE.")
