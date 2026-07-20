#pragma once

#include "core/types/data_model.hpp"
#include "core/types/number_decode.hpp"
#include "core/types/number_style.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <cstdint>
#include <optional>
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
    // D-LANG-TYPE-IDENTITY-VOCABULARY: the winning candidate's vocabulary tag
    // ("long", "unsigned long long", …; empty for an anonymous type such as
    // `int`). Both typing tiers mint `primitive(kind, vocabularyName)` with it —
    // WITHOUT it `20L` on LP64 would intern the ANONYMOUS I64, i.e. a type
    // `_Generic`'s `long:` association could not match. A view into the
    // schema's `DataModelTypeRef`, which outlives every ladder call.
    std::string_view    vocabularyName{};
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
            return {IntegerLadderStatus::Typed, k, c.vocabularyName};
        }
    }
    return {IntegerLadderStatus::TooLarge, TypeKind::Void};
}

// ── C23 6.4.4.1 (D-CSUBSET-BITINT-WIDE-LITERAL / Fork-1b): wb/uwb detection ──
//
// If the literal's declared suffix belongs to a `bitPrecise` integerLiteralTyping
// rule (a `wb`/`uwb` suffix), return that rule's signedness (`wb` → true, `uwb` →
// false); else nullopt. Both typing call sites (`cst_to_hir` lowerLiteral +
// `semantic_analyzer` typeLiteralIfAny) consult this FIRST: a bit-precise literal
// is typed `[unsigned] _BitInt(N)` with N derived from its (arbitrary-magnitude)
// value via `decodeBigInteger` + `BitIntValue::fromLiteralMagnitude`, NOT through
// the u64 magnitude ladder (which nullopt-overflows a `>u64` literal). The
// typing thus lives INSIDE the `integerLiteralTyping` engine (a config rule mode),
// so the loader's suffix-coverage cross-check is satisfied natively. A schema
// with no bit-precise rule never returns a signedness here (→ standard ladder).
[[nodiscard]] inline std::optional<bool>
bitPreciseLiteralSignedness(std::string_view rawText,
                            NumberStyle const* ns,
                            std::span<IntegerLiteralTypingRule const> rules) {
    std::string_view const suffix = matchIntegerSuffix(rawText, ns);
    if (suffix.empty()) return std::nullopt;
    for (auto const& r : rules) {
        if (!r.bitPrecise) continue;
        for (auto const& s : r.suffixes) {
            if (s == suffix) return r.bitPreciseSigned;
        }
    }
    return std::nullopt;
}

// ── FC3.5 sweep-c2: the float-literal typing rule (C 6.4.4.2) ────────────
//
// The float sibling of `typeIntegerLiteral`, shared by the SAME two
// tiers (semantic pass-2 + CST→HIR lowerLiteral) so they can never
// drift. Far simpler than the integer ladder: a floating constant's
// type is keyed by its SUFFIX alone — no magnitude ranges, no radix
// classes (C 6.4.4.2: unsuffixed → double, f/F → float). Two distinct
// non-Typed outcomes (FC17.9(e)):
//   * NoRule — no rule covers the matched suffix. Loader-prevented for
//     shipped configs (every numberStyle float suffix must be covered +
//     an unsuffixed rule must exist); a miss is substrate drift the
//     caller surfaces fail-loud, never silently the literalTypes base.
//   * AxisUndeclared — the matched rule's type DEPENDS on the
//     long-double axis (`coreByLongDoubleFormat`) but the active format
//     declared none (D-CSUBSET-LONG-DOUBLE): a `20.0L` whose
//     representation is unknowable. The caller emits the precise
//     S_LongDoubleFormatUndeclared — never the base core (the same
//     no-silent-fallback rule the typeSpecifiers bind applies).
enum class FloatLadderStatus : std::uint8_t {
    Typed,
    AxisUndeclared,
    NoRule,
};
struct FloatLadderResult {
    FloatLadderStatus status = FloatLadderStatus::NoRule;
    TypeKind          kind   = TypeKind::Void;   // meaningful only when Typed
    // D-LANG-TYPE-IDENTITY-VOCABULARY: the resolved rule's vocabulary tag — the
    // integer ladder's sibling. Empty for `double`/`float`; "long double" for
    // the l/L rule, which is what keeps `20.0L` distinct from `20.0` on an
    // f64 axis where BOTH are F64.
    std::string_view  vocabularyName{};
};

[[nodiscard]] inline FloatLadderResult
typeFloatLiteral(std::string_view rawText,
                 NumberStyle const* ns,
                 std::span<FloatLiteralTypingRule const> rules,
                 DataModel dm,
                 LongDoubleFormat ldf) {
    auto const resolve = [&](FloatLiteralTypingRule const& r) -> FloatLadderResult {
        auto const k = r.type.resolveCore(dm, ldf);
        if (!k.has_value()) return {FloatLadderStatus::AxisUndeclared, TypeKind::Void};
        return {FloatLadderStatus::Typed, *k, r.type.vocabularyName};
    };
    std::string_view const suffix = matchFloatSuffix(rawText, ns);
    for (auto const& r : rules) {
        if (suffix.empty()) {
            if (r.suffixes.empty()) return resolve(r);
            continue;
        }
        for (auto const& s : r.suffixes) {
            if (s == suffix) return resolve(r);
        }
    }
    return {FloatLadderStatus::NoRule, TypeKind::Void};
}

} // namespace dss
