#pragma once

#include "core/types/data_model.hpp"
#include "core/types/number_decode.hpp"
#include "core/types/number_style.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <cstdint>
#include <limits>
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

// The rule that covers the literal's matched suffix — the empty-`suffixes` rule
// for an unsuffixed literal — or nullptr when none does, which the loader's
// coverage cross-check makes substrate drift for a shipped document. ONE select
// for every question asked of a literal (its type, its phase-4 operand), so no
// two of them can pick different rules.
[[nodiscard]] inline IntegerLiteralTypingRule const*
ruleFor(std::string_view suffix,
        std::span<IntegerLiteralTypingRule const> rules) noexcept {
    for (auto const& r : rules) {
        if (suffix.empty()) {
            if (r.suffixes.empty()) return &r;
            continue;
        }
        for (auto const& s : r.suffixes) {
            if (s == suffix) return &r;
        }
    }
    return nullptr;
}

} // namespace detail::int_ladder

// ── THE VALUE A TYPED INTEGER LITERAL CARRIES ─────────────────────────────
//   P68 round 13, D-C-MSVC-SIZED-INTEGER-SUFFIXES-REFUSED
//
// The decoded magnitude REDUCED to the literal's type — modulo 2^width, then read
// at the type's signedness — as that value's 64-bit two's-complement pattern
// (sign-extended from a signed type's width, zero-extended from an unsigned one).
// For a ladder-typed literal this is the identity: the ladder chose the first
// candidate whose range HOLDS the magnitude. For a FIXED-TYPE rule whose
// `outOfRange` is `wrap` it is the reduction MSVC performs on its sized suffixes
// (✔MEASURED 2026-09-25, MSVC 19.51.36260, run 20260925-092743-c2711a1a: `300i8`
// is 44, `0xFFi8` is -1, `256ui8` is 0, `2147483648i32` is INT_MIN). Every tier
// that needs a literal's VALUE — the CST→HIR literal, the CST-side constant
// evaluator and phase 4 — asks here, so no two of them can reduce differently.
//
// Plain `char` is the one core whose signedness the core does not carry: the
// TARGET decides it (`TargetSchema::charIsUnsigned(ObjectFormatKind)`), so the
// caller hands over the pair's answer. nullopt ⇔ the type is `char`, the caller
// has no pair, and the reduced byte is above 0x7F — the one case whose value the
// answer changes; the caller refuses, loud, never a guessed sign. A 128-bit kind
// holds every 64-bit magnitude, so its reduction is the identity. Any other core
// is not a type an integer literal can have (nullopt).
[[nodiscard]] inline std::optional<std::uint64_t>
reducedIntegerLiteralBits(TypeKind kind, std::uint64_t magnitude,
                          std::optional<bool> charIsUnsigned) noexcept {
    int  width    = 0;
    bool isSigned = true;
    if (kind == TypeKind::Char) {
        width = 8;
        if (charIsUnsigned.has_value()) {
            isSigned = !*charIsUnsigned;
        } else if ((magnitude & 0xFFu) > 0x7Fu) {
            return std::nullopt;
        }
    } else {
        width = detail::int_ladder::integerWidth(kind);
        if (width == 0) return std::nullopt;
        isSigned = detail::int_ladder::isSignedIntKind(kind);
    }
    if (width >= 64) return magnitude;
    std::uint64_t bits = magnitude & ((std::uint64_t{1} << width) - 1);
    if (isSigned) {
        std::uint64_t const sign = std::uint64_t{1} << (width - 1);
        bits = (bits ^ sign) - sign;   // sign-extend from the type's width
    }
    return bits;
}

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
    IntegerLiteralTypingRule const* const rule =
        detail::int_ladder::ruleFor(suffix, rules);
    if (rule == nullptr) return {IntegerLadderStatus::NoRule, TypeKind::Void};

    // 2b. A FIXED-TYPE rule (D-C-MSVC-SIZED-INTEGER-SUFFIXES-REFUSED): its one
    //     type, whatever the magnitude — `300i8` is a `char` as surely as `5i8`.
    //     What the magnitude then IS as that type is `reducedIntegerLiteralBits`'
    //     answer, which every tier that needs the value asks.
    if (rule->fixedType.has_value()) {
        return {IntegerLadderStatus::Typed, rule->fixedType->resolveCore(dm),
                rule->fixedType->vocabularyName};
    }

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

// ── C 6.10.1p4: THE SAME LADDER, RUN AT PHASE-4 WIDTHS ───────────────────
//   D-PP-IF-UNSIGNED-INTMAX
//
// A `#if`/`#elif` controlling expression is evaluated with every integer type
// acting as `intmax_t` or `uintmax_t`. So the ONLY thing the preprocessor needs
// from a literal is its SIGNEDNESS, and the ladder that decides it is this same
// ordered candidate list -- with every candidate's WIDTH replaced by 64, because
// in phase 4 there are only two integer types and both are 64 bits wide.
//
// Returns true = signed, false = unsigned; nullopt = no rule covers the matched
// suffix (the loader cross-checks that cannot happen for a shipped config, so it
// is substrate drift the caller surfaces fail-loud -- never a guessed
// signedness, because a guess here selects a wrong BRANCH in silence).
//
// ★★ WHY THIS IS NOT `typeIntegerLiteral` WITH THE WIDTH IGNORED, WHICH IS THE
// DESIGN THIS REPLACED AND WHICH IS MEASURABLY WRONG. Asking the ordinary ladder
// for `0xFFFFFFFF` returns `unsigned int` -- correct C 6.4.4.1, since the value
// does not fit a 32-bit `int`. Carry that signedness into phase 4 and
// `#if 0xFFFFFFFF > -1` converts `-1` to `UINTMAX_MAX` and takes the FALSE arm.
// ✔MEASURED 2026-08-27, gcc 13.3.0 `-std=c2x` and clang 18.1.3 `-std=c23` probed
// SEPARATELY with the taken arm read out of the emitted object via `nm`: BOTH
// take the TRUE arm. They do not use the literal's own C type here; they
// re-decide against intmax_t. The boundary is exactly INTMAX_MAX, pinned by the
// adjacent pair `#if 0x7FFFFFFFFFFFFFFF > -1` (both: TRUE => signed) and
// `#if 0x8000000000000000 > -1` (both: FALSE => unsigned).
//
// ⓘ AND THE CONSEQUENCE IS THAT THIS QUESTION TAKES NO `DataModel`. A data model
// fixes type WIDTHS; at phase-4 widths every candidate is 64 bits, so the model
// cannot reach the answer. `#if -1 < 0x80000000L` is therefore the SAME branch on
// LP64 and LLP64 -- which is what both references do, and which removes the
// per-target threading this rule would otherwise have needed.
//
// ── P68 round 13 (D-C-MSVC-SIZED-INTEGER-SUFFIXES-REFUSED): THE WHOLE OPERAND ──
// A FIXED-TYPE literal is the one kind whose phase-4 VALUE is not its magnitude:
// it is first reduced to its type, and only then taken at intmax width with that
// type's signedness. ✔MEASURED 2026-09-25, MSVC 19.51.36260 (run
// 20260925-092749-73db8424), identically under `/std:c17` (the conforming
// preprocessor), `/std:c17 /Zc:preprocessor-` (the traditional one), the default
// mode and `/std:clatest`: `#if 300i8 == 44`, `#if 128i8 < 0`, `#if 0xFFi8 == -1`,
// `#if 256ui8 == 0` and `#if 4294967296i32 == 0` all take the `#if` arm, and
// `#if 5ui8 - 6 < 0` / `#if -1 < 0ui8` the `#else` arm (the `ui` forms are
// unsigned). So the question is the operand, `bits` + `isSigned`, and the
// signedness-only one below is derived from it. Still no `DataModel`: the loader
// admits only a fixed type whose core is the same under every model.
enum class PhaseFourLiteralStatus : std::uint8_t {
    Operand,             // `bits` / `isSigned` are the literal's #if operand
    NoRule,              // no rule covers the suffix, or a candidate's signedness
                         // varies by data model -- substrate drift, refuse
    CharSignednessUnknown,  // a `char`-typed literal with no target pair: its
                            // phase-4 signedness is the target's to decide
};
struct PhaseFourLiteral {
    PhaseFourLiteralStatus status   = PhaseFourLiteralStatus::NoRule;
    std::uint64_t          bits     = 0;      // the value at intmax width
    bool                   isSigned = true;
    // The literal's type is SIGNED but its magnitude exceeds INTMAX_MAX, so phase 4
    // reads it as uintmax_t — the case both references warn about (the unsuffixed
    // decimal, and clang's `wb`). The ladder that decided the reading reports it,
    // so the evaluator's warning cannot drift from the answer it used.
    bool                   reinterpretedUnsigned = false;
};

[[nodiscard]] inline PhaseFourLiteral
preprocessorLiteral(std::string_view rawText,
                    NumberStyle const* ns,
                    std::span<IntegerLiteralTypingRule const> rules,
                    std::uint64_t magnitude,
                    std::optional<bool> charIsUnsigned) {
    std::string_view const suffix = matchIntegerSuffix(rawText, ns);

    // Rule select + radix class: IDENTICAL to `typeIntegerLiteral`'s steps 2-3.
    IntegerLiteralTypingRule const* const rule =
        detail::int_ladder::ruleFor(suffix, rules);
    if (rule == nullptr) return {};

    // A fixed-type literal: reduced to its type, then read at intmax width with
    // the type's own signedness -- which, for plain `char`, only the target knows.
    if (rule->fixedType.has_value()) {
        TypeKind const k = rule->fixedType->core;   // model-invariant (loader)
        bool isSigned = true;
        if (k == TypeKind::Char) {
            if (!charIsUnsigned.has_value()) {
                return {PhaseFourLiteralStatus::CharSignednessUnknown};
            }
            isSigned = !*charIsUnsigned;
        } else {
            isSigned = detail::int_ladder::isSignedIntKind(k);
        }
        auto const bits = reducedIntegerLiteralBits(k, magnitude, charIsUnsigned);
        if (!bits.has_value()) return {};   // a kind the loader never admits
        return {PhaseFourLiteralStatus::Operand, *bits, isSigned};
    }

    // A BIT-PRECISE literal (`wb`, `uwb`): its type's signedness is its rule's own,
    // because C23 counts the bit-precise types among the signed and unsigned integer
    // types phase 4 widens to intmax_t / uintmax_t. ✔MEASURED 2026-09-25, clang
    // 18.1.3 `-std=c23` and `-std=c2x -pedantic-errors` (runs
    // 20260925-095900-8cb95906, 20260925-102338-6976cc3f): `#if 5wb - 6 < 0` takes
    // the `#if` arm and `#if 5uwb - 6 < 0` the `#else`; a `wb` literal past INTMAX_MAX
    // is read UNSIGNED with a warning, exactly like an unsuffixed decimal; gcc 13.3.0
    // refuses both spellings. ⚠ This rule used to reach the candidate loop below with
    // NO candidates and fall to its closing `unsigned`, so `wb` read unsigned and the
    // first condition took the wrong arm in silence (D-PP-IF-BIT-PRECISE-LITERAL-READS-UNSIGNED).
    if (rule->bitPrecise) {
        bool const fitsSigned = magnitude <= static_cast<std::uint64_t>(
                                                 std::numeric_limits<std::int64_t>::max());
        bool const isSigned = rule->bitPreciseSigned && fitsSigned;
        return {PhaseFourLiteralStatus::Operand, magnitude, isSigned,
                rule->bitPreciseSigned && !fitsSigned};
    }

    auto const& candidates = integerLiteralIsPrefixed(rawText, ns)
                                 ? rule->nondecimal
                                 : rule->decimal;

    // A candidate's SIGNEDNESS, which a data model must not change. LP64 / LLP64
    // / ILP32 are WIDTH models -- `long` is 64-bit or 32-bit but signed in every
    // one of them. Verified rather than assumed: if a config ever declares a name
    // whose signedness varies by model, that is substrate drift and this refuses
    // (nullopt) instead of silently picking one.
    auto signednessOf =
        [](DataModelTypeRef const& t) -> std::optional<bool> {
        bool const base = detail::int_ladder::isSignedIntKind(t.core);
        if (detail::int_ladder::integerWidth(t.core) == 0) return std::nullopt;
        for (auto const& [dm, k] : t.coreByDataModel) {
            (void)dm;
            if (detail::int_ladder::integerWidth(k) == 0) return std::nullopt;
            if (detail::int_ladder::isSignedIntKind(k) != base) return std::nullopt;
        }
        return base;
    };

    // First candidate whose PHASE-4 range holds the magnitude. The candidate's
    // own width is discarded and 64 substituted, so the test collapses to
    // "signed candidate ⇒ fits iff magnitude <= INTMAX_MAX; unsigned ⇒ always".
    for (auto const& c : candidates) {
        auto const sgn = signednessOf(c);
        if (!sgn.has_value()) return {};
        TypeKind const at64 = *sgn ? TypeKind::I64 : TypeKind::U64;
        if (detail::int_ladder::magnitudeFits(at64, magnitude)) {
            return {PhaseFourLiteralStatus::Operand, magnitude, *sgn};
        }
    }

    // Every candidate is signed and the magnitude exceeds INTMAX_MAX -- C's
    // decimal-unsuffixed ladder (int/long/long long) meeting
    // `18446744073709551615`. ✔MEASURED: gcc warns "integer constant is so large
    // that it is unsigned", clang warns "interpreting as unsigned", and BOTH then
    // evaluate it as UNSIGNED with its true value. Unanimous, so the union rule
    // makes it required.
    // ⚠ Distinct from the settled >64-bit-literal split, where gcc warns and
    // substitutes a TRUNCATED value while clang refuses and DSS refuses with it.
    // A magnitude too large for `uintmax_t` never reaches here: `decodeInteger`
    // has already nullopt'd and the caller has failed loud.
    // REINTERPRETED only for the UNSUFFIXED rule — the case both references are
    // measured warning about, and the one the evaluator's warning has always
    // covered; an `l`/`ll` decimal past INTMAX_MAX reads unsigned here too and
    // keeps the silence the evaluator has always given it.
    return {PhaseFourLiteralStatus::Operand, magnitude, false, suffix.empty()};
}

// The SIGNEDNESS alone — true = signed, false = unsigned — of the phase-4 operand
// above, for a caller that only verifies a spelling (the preprocessor's shipped-
// constant splice). nullopt ⇔ no operand: no rule covers the suffix (substrate
// drift, refuse) or it is a `char`-typed literal and `charIsUnsigned` is absent,
// in which case a spelling search simply does not use it.
[[nodiscard]] inline std::optional<bool>
preprocessorLiteralSignedness(std::string_view rawText,
                              NumberStyle const* ns,
                              std::span<IntegerLiteralTypingRule const> rules,
                              std::uint64_t magnitude,
                              std::optional<bool> charIsUnsigned) {
    PhaseFourLiteral const lit =
        preprocessorLiteral(rawText, ns, rules, magnitude, charIsUnsigned);
    if (lit.status != PhaseFourLiteralStatus::Operand) return std::nullopt;
    return lit.isSigned;
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
    // D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY (P54 lane
    // `fw`): the matched rule's SOURCE SPELLING — "double" / "float" /
    // "long double" for c — carried out for the range diagnostic to name the
    // type the way the programmer wrote it. `vocabularyName` cannot serve: it is
    // deliberately EMPTY for `double` and `float` (they are the anonymous
    // representatives of their cores), so a message built from it would say
    // `too large for ''`. `DataModelTypeRef::name`'s own comment already calls
    // itself "source spelling, for diagnostics"; this is the first diagnostic to
    // need it. A `string_view` into the rule, whose lifetime is the
    // SemanticConfig's — the same lifetime `vocabularyName` already assumes.
    std::string_view  typeName{};
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
        return {FloatLadderStatus::Typed, *k, r.type.vocabularyName, r.type.name};
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
