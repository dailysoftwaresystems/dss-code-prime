#pragma once

// Canonical SymbolBinding / SymbolVisibility vocabulary — lifted from
// link/object_format_schema.hpp (which originally declared these) so
// MIR-tier producers can use them without a layer inversion. The
// link-tier header still includes this one + re-exports the names,
// keeping every existing consumer source-compatible.
//
// **Why MIR needs this** (D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD,
// step 13.6 OPT1 gate): plan 22 §2.9 requires DCE to NOT delete an
// externally-visible symbol. The link tier knows the binding/visibility
// per AssembledFunction, but MIR — where DCE actually runs — didn't.
// Threading these onto MirFunc + MirGlobal at HIR→MIR lowering time
// lets the optimizer's DCE pass consult the attributes directly,
// preserving every Global symbol (and every Default-visibility symbol
// that isn't Local) from elimination.
//
// **Closed enum, semantic only.** No bit-flag combinations; no
// numeric arithmetic. The 1-byte storage keeps MirFunc + MirGlobal
// under their 32-byte trivially-copyable POD budgets.

#include "core/types/target_schema.hpp"   // EnumNameTable<E,N>

#include <cstdint>
#include <optional>
#include <string_view>

namespace dss {

// Symbol binding — visibility within the linker's symbol-resolution
// algorithm. Local symbols never resolve across translation units;
// Weak symbols defer to Global symbols of the same name.
enum class SymbolBinding : std::uint8_t {
    Local  = 0,
    Global = 1,
    Weak   = 2,
};

inline constexpr EnumNameTable<SymbolBinding, 3> kSymbolBindingTable{{{
    { SymbolBinding::Local,  "local"  },
    { SymbolBinding::Global, "global" },
    { SymbolBinding::Weak,   "weak"   },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kSymbolBindingTable);

[[nodiscard]] constexpr std::string_view
symbolBindingName(SymbolBinding b) noexcept {
    return kSymbolBindingTable.name(b);
}
[[nodiscard]] constexpr std::optional<SymbolBinding>
symbolBindingFromName(std::string_view s) noexcept {
    return kSymbolBindingTable.fromName(s);
}

// Symbol visibility — affects whether a symbol is exported to other
// images at runtime. Default = exported (subject to binding).
enum class SymbolVisibility : std::uint8_t {
    Default   = 0,
    Hidden    = 1,
    Protected = 2,
    Internal  = 3,
};

inline constexpr EnumNameTable<SymbolVisibility, 4> kSymbolVisibilityTable{{{
    { SymbolVisibility::Default,   "default"   },
    { SymbolVisibility::Hidden,    "hidden"    },
    { SymbolVisibility::Protected, "protected" },
    { SymbolVisibility::Internal,  "internal"  },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kSymbolVisibilityTable);

[[nodiscard]] constexpr std::string_view
symbolVisibilityName(SymbolVisibility v) noexcept {
    return kSymbolVisibilityTable.name(v);
}
[[nodiscard]] constexpr std::optional<SymbolVisibility>
symbolVisibilityFromName(std::string_view s) noexcept {
    return kSymbolVisibilityTable.fromName(s);
}

// ── WHAT A WEAK DEFINITION PROMISES ABOUT ITS DUPLICATES ──────────────────
//    D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED
//
// `SymbolBinding::Weak` says "several translation units may define this; the
// linker keeps one". It does NOT say how much the copies are required to have
// in common, and for two of COFF's COMDAT selections that difference is the
// entire contract: their selection byte is a PROMISE the linker is specified to
// VERIFY, and issue a multiply-defined-symbol error when it is broken.
//
// ★★ THIS IS A SEPARATE AXIS FROM BINDING, NOT A THIRD BINDING. Making it
// `SymbolBinding::WeakSameSize` would have forced every `== SymbolBinding::Weak`
// test in the tree (the writers' dialect gates, the DCE preserve rule, the
// merge's own strong-shadows-weak arm) to enumerate a widening set of
// enumerators, and each site that forgot one would silently reclassify a weak
// definition as strong. The duty rides ALONGSIDE the binding and is read only
// by the code that folds duplicates.
//
// ★ FORMAT-BLIND ON PURPOSE. Nothing here is COFF: the COFF reader is simply
// the first producer that has a duty to declare, because COMDAT is where the
// spelling exists today. ELF's section groups (`GRP_COMDAT`) and Mach-O's
// coalesced sections express the same idea and would map onto the same three
// answers, so a second producer needs no new vocabulary — which is exactly what
// stops a `Coff*` enum from appearing beside this one.
//
// `Any` is the DEFAULT and it is the honest default: it is what a weak
// definition minted by DSS's own front end promises (nothing beyond "keep one"),
// and it is what IMAGE_COMDAT_SELECT_ANY promises. A producer that knows more
// says more.
enum class DuplicateMatch : std::uint8_t {
    Any          = 0,  // keep any one copy; the copies need not agree at all
    SameSize     = 1,  // every copy must have the SAME BYTE LENGTH
    ExactContent = 2,  // every copy must have identical BYTES
};

inline constexpr EnumNameTable<DuplicateMatch, 3> kDuplicateMatchTable{{{
    { DuplicateMatch::Any,          "any"           },
    { DuplicateMatch::SameSize,     "same-size"     },
    { DuplicateMatch::ExactContent, "exact-content" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kDuplicateMatchTable);

[[nodiscard]] constexpr std::string_view
duplicateMatchName(DuplicateMatch m) noexcept {
    return kDuplicateMatchTable.name(m);
}
[[nodiscard]] constexpr std::optional<DuplicateMatch>
duplicateMatchFromName(std::string_view s) noexcept {
    return kDuplicateMatchTable.fromName(s);
}

// When two definitions of one name are folded, the duty that governs is the
// STRICTER of the two. ★ THAT IS A DELIBERATE CHOICE AND IT IS THE SAFE ONE:
// it can only ever refuse input the format also refuses, never accept input the
// format rejects. It also means a producer that declares nothing (`Any`) cannot
// dilute a sibling's promise, which is the failure mode a "first one wins" or a
// "they must agree" rule would each have in one direction.
//
// ⚠ NOT the same question as "do the two definitions declare the SAME
// selection". COFF's linker treats a selection-byte disagreement between
// duplicates as its own error; DSS does not model that today and this function
// is not it — widening it into that would start refusing inputs on a rule this
// row never measured.
[[nodiscard]] constexpr DuplicateMatch
stricterDuplicateMatch(DuplicateMatch a, DuplicateMatch b) noexcept {
    // The enumerators are ordered by strictness, and the ONE static_assert
    // below is what keeps that true: reordering them silently inverts this
    // function, and nothing else in the tree would notice.
    return (static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b)) ? a : b;
}

static_assert(static_cast<std::uint8_t>(DuplicateMatch::Any)
              < static_cast<std::uint8_t>(DuplicateMatch::SameSize)
           && static_cast<std::uint8_t>(DuplicateMatch::SameSize)
              < static_cast<std::uint8_t>(DuplicateMatch::ExactContent),
              "DuplicateMatch enumerators must stay ordered by increasing "
              "strictness -- stricterDuplicateMatch() compares them numerically");
static_assert(stricterDuplicateMatch(DuplicateMatch::Any,
                                     DuplicateMatch::SameSize)
              == DuplicateMatch::SameSize);
static_assert(stricterDuplicateMatch(DuplicateMatch::ExactContent,
                                     DuplicateMatch::SameSize)
              == DuplicateMatch::ExactContent);
static_assert(stricterDuplicateMatch(DuplicateMatch::Any, DuplicateMatch::Any)
              == DuplicateMatch::Any);

// ── WHEN TWO TRANSLATION UNITS REFERENCE ONE NAME AND DISAGREE ────────────
//    D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE
//
// The REFERENCE-side twin of `stricterDuplicateMatch` above, and it exists for
// the same reason: two merge tiers fold import rows (`mir_merge.cpp`'s
// `ffiImportKey` group and `linker.cpp`'s `dedupKey` group) and a rule spelled
// twice is a rule that drifts.
//
// A STRONG (`Global`) reference says the symbol MUST be resolved. A `Weak` one
// says it MAY resolve to nothing, in which case its address is 0. Where one CU
// requires the symbol and another can do without it, the PROGRAM requires it —
// which is what C says and what gcc and clang do — so Global wins. The opposite
// rule produces an image that links with the symbol absent and then reads
// through a null address, which is a silent wrong answer rather than a link
// error.
//
// ⚠ IT DOES NOT COMPARE THE ENUMERATORS NUMERICALLY, and that is not fussiness.
// `SymbolBinding` is ordered Local(0) < Global(1) < Weak(2) — an order that
// suits neither strength nor visibility, because it was never chosen for either.
// A `std::max` here would return WEAK and invert the rule silently, which is the
// exact mistake `stricterDuplicateMatch`'s static_assert exists to prevent one
// axis over. The value order is therefore not relied on at all.
//
// ⚠ `Local` IS NOT A REPRESENTABLE REFERENCE BINDING — no format spells an
// undefined LOCAL symbol — and it never reaches here: HIR→MIR's `collectExterns`
// refuses it at the declaration's own source span. It is nevertheless TOTAL
// rather than assumed away, and the total answer is stated as a single
// disjunction so there is no arm that can RETURN `Local`: a Local operand
// contributes nothing, exactly as a Weak one does, and two of them still yield
// the representable `Weak`. A function that could hand `Local` back to a writer
// would put an unspellable binding on an undefined symbol — the very thing the
// refusal upstream exists to prevent — from an arm no test would think to cover.
[[nodiscard]] constexpr SymbolBinding
strongerReferenceBinding(SymbolBinding a, SymbolBinding b) noexcept {
    return (a == SymbolBinding::Global || b == SymbolBinding::Global)
               ? SymbolBinding::Global
               : SymbolBinding::Weak;
}

static_assert(strongerReferenceBinding(SymbolBinding::Weak, SymbolBinding::Global)
              == SymbolBinding::Global);
static_assert(strongerReferenceBinding(SymbolBinding::Global, SymbolBinding::Weak)
              == SymbolBinding::Global);
static_assert(strongerReferenceBinding(SymbolBinding::Weak, SymbolBinding::Weak)
              == SymbolBinding::Weak);
static_assert(strongerReferenceBinding(SymbolBinding::Global, SymbolBinding::Global)
              == SymbolBinding::Global);
static_assert(strongerReferenceBinding(SymbolBinding::Local, SymbolBinding::Weak)
              == SymbolBinding::Weak);
static_assert(strongerReferenceBinding(SymbolBinding::Local, SymbolBinding::Global)
              == SymbolBinding::Global);
static_assert(strongerReferenceBinding(SymbolBinding::Local, SymbolBinding::Local)
              == SymbolBinding::Weak,
              "two non-representable operands still yield a representable "
              "reference binding -- the fold has no arm that can return Local");

// D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD invariant: a symbol whose
// `binding == Global` AND `visibility != Hidden` AND `visibility !=
// Internal` is externally observable — every later image (the linker,
// the dynamic loader, profile-guided optimization tools) may reference
// it by name. DCE / unused-symbol elimination MUST preserve such
// symbols even when no INTRA-MODULE use of them exists.
//
// `Weak` binding is treated as "preserve unless a Global definition
// of the same name supersedes it" — same DCE-protect treatment as
// Global at MIR tier; the linker resolves the supersede later.
//
// `Local` binding + any visibility = DCE-eligible (intra-module-only).
//
// The free function is THE source of truth for the optimizer; the
// MIR verifier's invariant rule + the link-tier emitter share it.
[[nodiscard]] constexpr bool
isExternallyVisible(SymbolBinding binding, SymbolVisibility visibility) noexcept {
    if (binding == SymbolBinding::Local) return false;
    if (visibility == SymbolVisibility::Hidden) return false;
    if (visibility == SymbolVisibility::Internal) return false;
    return true;  // Global+Default, Global+Protected, Weak+*-non-Hidden/Internal
}

} // namespace dss
