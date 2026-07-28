#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

// INTERNAL header — the CLOSED-KEY-VOCABULARY substrate shared by every DSS
// config loader (`grammar_schema_json.cpp`, `target_schema_json.cpp`, …).
// Never included by a public type header; it declares no types, only the two
// compile-time predicates + the `$`-documentation carve-out that make a
// "typo discriminator" honest.
//
// EXTRACTED (TF-C74) from `grammar_schema_json.cpp`, unchanged. The extraction
// is the point: the language loader closed its root-key vocabulary in TF-C72
// and the TARGET loader did not, so a misspelled target root key loaded clean
// and the feature it named silently no-op'd. A guard that lives inside ONE
// loader's .cpp is a guard the sibling loader cannot use — so it lives here.

namespace dss::detail {

// ── the `$`-prefixed documentation-key carve-out ──────────────────────────
//
// A key whose name starts with `$` (`$comment`, `$…Comment`) is PROSE, never
// config: the codebase-wide way to explain a block inside the JSON that
// declares it. Every closed-key vocabulary must skip such keys, and so must
// every place that reads an object's keys AS IDENTIFIERS — most sharply the
// `shapes` map, where a `$`-prefixed sibling of the rule names was read as a
// SHAPE DEFINITION and its prose value as a rule REFERENCE, failing the whole
// load with the paragraph echoed back as if it were a rule name.
//
// ★ ONE predicate, not another copy of the same expression. The convention had
// been open-coded identically at seven sites; a convention spelled out N times
// is a convention that holds only where someone remembered it, and the shapes
// map is the site where it was forgotten. Every site calls this, so
// "documentation key" has exactly one definition to read and to change.
[[nodiscard]] constexpr bool isDocumentationKey(std::string_view key) {
    return !key.empty() && key.front() == '$';
}

// ── the closed-key-vocabulary well-formedness check ───────────────────────
//
// Every `kSomethingKeys` table is a `std::array<string_view, N>` whose N is
// written BY HAND next to the initializer list — and the compiler does NOT
// check that the two agree in the dangerous direction.
// `std::array<std::string_view, 57>` with 56 initializers is perfectly legal:
// it value-initializes the tail, so element 56 becomes the EMPTY string_view
// and the typo discriminator silently starts whitelisting a key of `""`.
// (MEASURED on `kSemanticsKeys`: bumping 56→57 alone compiles clean, and a
// `semantics` key of `""` then loads without a diagnostic.) A DUPLICATE entry
// is silent in a mirror-image way — it inflates the count, so a key that is
// actually missing looks accounted for by the number.
//
// ★ ONE helper, not a copy of the loop per table. The check had been written
// once, for `kDeclarationRowKeys`, and the three sibling tables — including the
// 56-entry `kSemanticsKeys`, the largest and most edit-prone of them — went
// unguarded; a guard that must be remembered per table is a guard that holds
// only where someone remembered it. Every closed-key table calls this, at
// compile time, so the next person to add a key cannot get the number wrong
// without the build saying so.
template <std::size_t N>
[[nodiscard]] constexpr bool isWellFormedKeyVocabulary(
    std::array<std::string_view, N> const& keys) {
    for (std::size_t a = 0; a < N; ++a) {
        if (keys[a].empty()) return false;             // under-filled ⇒ tail is ""
        for (std::size_t b = a + 1; b < N; ++b)
            if (keys[a] == keys[b]) return false;      // duplicate ⇒ count lies
    }
    return true;
}
// The same check for a name→value vocabulary (a verb table). The under-fill
// hazard is WORSE here: the value half zero-initializes too, so the phantom
// `""` verb silently maps to whatever enumerator happens to be 0.
template <typename T, std::size_t N>
[[nodiscard]] constexpr bool isWellFormedKeyVocabulary(
    std::array<std::pair<std::string_view, T>, N> const& rows) {
    for (std::size_t a = 0; a < N; ++a) {
        if (rows[a].first.empty()) return false;
        for (std::size_t b = a + 1; b < N; ++b)
            if (rows[a].first == rows[b].first) return false;
    }
    return true;
}

} // namespace dss::detail

// The one message every closed-key table shares, so the diagnosis reads the
// same wherever the build breaks. FULLY QUALIFIED on purpose: the two loaders
// that use it sit at different scopes (`dss::detail` vs `dss`), and a macro
// that silently requires a `using`-declaration at each call site is exactly
// the "guard you must remember" this file exists to abolish.
#define DSS_CHECK_KEY_VOCABULARY(table)                                        \
    static_assert(::dss::detail::isWellFormedKeyVocabulary(table),             \
                  #table ": the declared std::array size must equal the "      \
                  "initializer count (an under-filled array zero-fills and "   \
                  "would whitelist the empty key) and every entry must be "    \
                  "unique")
