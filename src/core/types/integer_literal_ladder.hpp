#pragma once

#include "core/types/data_model.hpp"
#include "core/types/number_decode.hpp"
#include "core/types/number_style.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <cstdint>
#include <span>
#include <string_view>

// ── FC3 c1: the integer-literal typing ladder (C 6.4.4.1) ────────────────
//
// THE single implementation both typing tiers call — the semantic
// analyzer's pass-2 literal arm AND the CST→HIR `lowerLiteral` — so the
// two can never drift (plan-lock C-2: the ladder runs at BOTH sites; one
// algorithm, two call sites).
//
// Inputs: the literal token's RAW text (suffix re-derived by matching
// the tail against `numberStyle.integerSuffixes` — the same longest
// match `decodeInteger`'s strip performs), the language's
// `integerLiteralTyping` rules, the active `DataModel`, and the DECODED
// magnitude. Output: the first candidate type (of the matched rule's
// radix-class list) whose RANGE represents the magnitude.
//
// Engine-closed knowledge: only the core integer kinds' widths +
// signedness (the lattice's own vocabulary). WHICH suffixes exist, WHICH
// candidates each suffix admits, and their ORDER are all per-language
// config. Languages without the block never reach here.

namespace dss {

namespace detail::int_ladder {

// Width in bits of a core integer kind; 0 for non-integer kinds. The
// loader rejects non-integer ladder candidates, so a 0 here at runtime
// is a substrate inconsistency the caller surfaces as NoRule.
[[nodiscard]] constexpr int integerWidth(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::I8:   case TypeKind::U8:   return 8;
        case TypeKind::I16:  case TypeKind::U16:  return 16;
        case TypeKind::I32:  case TypeKind::U32:  return 32;
        case TypeKind::I64:  case TypeKind::U64:  return 64;
        case TypeKind::I128: case TypeKind::U128: return 128;
        default: return 0;
    }
}

[[nodiscard]] constexpr bool isSignedIntKind(TypeKind k) noexcept {
    return k == TypeKind::I8 || k == TypeKind::I16 || k == TypeKind::I32
        || k == TypeKind::I64 || k == TypeKind::I128;
}

// Does `magnitude` (the literal's decoded NON-NEGATIVE value — unary
// minus is a separate operator applied after typing, exactly C's rule)
// fit the kind's value range?
[[nodiscard]] constexpr bool magnitudeFits(TypeKind k,
                                           std::uint64_t magnitude) noexcept {
    int const w = integerWidth(k);
    if (w == 0) return false;
    if (w >= 128) return true;          // any u64 magnitude fits 128-bit
    if (isSignedIntKind(k)) {
        if (w > 64) return true;
        // max = 2^(w-1) - 1
        std::uint64_t const maxv =
            (std::uint64_t{1} << (w - 1)) - std::uint64_t{1};
        return magnitude <= maxv;
    }
    if (w >= 64) return true;           // u64 holds any u64 magnitude
    std::uint64_t const maxv = (std::uint64_t{1} << w) - std::uint64_t{1};
    return magnitude <= maxv;
}

} // namespace detail::int_ladder

// Outcome of a ladder run. `kind` is meaningful only when
// `status == Typed`.
enum class IntegerLadderStatus : std::uint8_t {
    Typed,       // a candidate's range fits — `kind` is the literal's type
    TooLarge,    // every candidate's range exceeded (S_IntegerLiteralTooLarge)
    NoRule,      // no rule covers the matched suffix — loader-prevented for
                 // shipped configs; surfaced fail-loud, never silently I32
};

struct IntegerLadderResult {
    IntegerLadderStatus status = IntegerLadderStatus::NoRule;
    TypeKind            kind   = TypeKind::Void;
};

// Run the ladder. `rawText` is the literal token's verbatim source text;
// `magnitude` its `decodeInteger` value (the caller has already handled
// the decode-overflow nullopt).
[[nodiscard]] inline IntegerLadderResult
typeIntegerLiteral(std::string_view rawText,
                   NumberStyle const* ns,
                   std::span<IntegerLiteralTypingRule const> rules,
                   DataModel dm,
                   std::uint64_t magnitude) {
    // 1. Suffix class: the EXACT declared spelling at the tail (longest
    //    match — the same match decodeInteger strips), empty = unsuffixed.
    std::string_view const suffix = matchIntegerSuffix(rawText, ns);

    // 2. Rule select: the rule whose `suffixes` contains the matched
    //    spelling; the empty-`suffixes` rule for an unsuffixed literal.
    IntegerLiteralTypingRule const* rule = nullptr;
    for (auto const& r : rules) {
        if (suffix.empty()) {
            if (r.suffixes.empty()) { rule = &r; break; }
            continue;
        }
        for (auto const& s : r.suffixes) {
            if (s == suffix) { rule = &r; break; }
        }
        if (rule != nullptr) break;
    }
    if (rule == nullptr) return {IntegerLadderStatus::NoRule, TypeKind::Void};

    // 3. Radix class: prefixed (per the declared numberStyle prefixes)
    //    selects the `nondecimal` candidate list; else `decimal`.
    auto const& candidates = integerLiteralIsPrefixed(rawText, ns)
                                 ? rule->nondecimal
                                 : rule->decimal;

    // 4. First candidate whose range fits wins.
    for (auto const& c : candidates) {
        TypeKind const k = c.resolveCore(dm);
        if (detail::int_ladder::magnitudeFits(k, magnitude)) {
            return {IntegerLadderStatus::Typed, k};
        }
    }
    return {IntegerLadderStatus::TooLarge, TypeKind::Void};
}

} // namespace dss
