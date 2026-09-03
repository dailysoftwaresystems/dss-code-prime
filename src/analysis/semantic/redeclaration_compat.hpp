#pragma once

#include "core/types/declared_qualification.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>   // P55: the spelling-blind walk's visited-pair set
#include <utility>
#include <vector>

// ── THE ONE FUNCTION-REDECLARATION COMPATIBILITY ORACLE ───────────────────────
//
// C23 asks "are these two declarations of one name declarations of the SAME
// function" in exactly one place — 6.7.6.3p15 for the function type, 6.7.6.1p2
// for each pointer inside it — and DSS asked it in three, with three different
// answers:
//
//   * `claimSuppressedShimSymbol` (CST→HIR) compared interner TypeIds to decide
//     whether a user declaration may inherit a shipped `synthesize` recipe's one
//     fixed body;
//   * the platform-realization pass compared NOTHING AT ALL — a shipped
//     descriptor row was suppressed on a NAME MATCH, so `#include <stdio.h>`
//     plus `int puts(double);` compiled clean and called ucrtbase's real `puts`
//     with a double in xmm0 where it expects a `char *` in rcx
//     ([[D-CSUBSET-SUPPRESSED-SHIPPED-ROW-SIGNATURE-UNCHECKED]]);
//   * the merged-declaration sweep compared interner TypeIds for EQUALITY, which
//     is neither necessary nor sufficient for C23 compatibility — it refuses the
//     legal `int f(volatile int); int f(int);` (a top-level parameter qualifier,
//     which 6.7.6.3p15 says to DROP) and accepts the illegal
//     `int f(const char *); int f(char *);` (a pointee qualifier, which
//     6.7.6.1p2 says is part of the type)
//     ([[D-LANG-TYPE-IDENTITY-QUALIFIER-BLIND-VS-C23-REDECL]]).
//
// One question, one answer. Every caller routes here.
//
// ★★ WHY THIS IS A PREDICATE AND NOT A CHANGE TO INTERNING. `const` is
// deliberately NOT a `QualBit` (type_interner.hpp states it: const never affects
// codegen or layout, so it is not materialized as a qualifier skin at all), and
// interning it would perturb EVERY type comparison in the compiler to serve one
// rule. So DSS type IDENTITY stays qualifier-blind and C23 redeclaration
// COMPATIBILITY — which is qualifier-SENSITIVE — is answered here, against a
// qualification claim carried ALONGSIDE the TypeId. The interner is untouched.
//
// ★★ ABSENT IS NOT UNQUALIFIED. A `DeclaredFunction` may carry NO qualification
// claim (a shipped-library descriptor signature is written in hir-text, which has
// no `const` spelling; a declarator shape the semantic harvester does not model).
// The oracle then does not JUDGE that axis — it neither invents a diagnostic nor
// invents a match. The consequence is a possible missed diagnostic, never a
// refused legal program, which is the only safe direction for a rule whose false
// positives reject correct code.
//
// AGNOSTIC: pure queries over `TypeInterner` + TypeIds. No language, target or
// object-format identity is readable from here, and no caller supplies one.

namespace dss {

// The axis on which two declarations of one name diverge. `None` == compatible.
enum class RedeclarationDivergence : std::uint8_t {
    None,
    NotAFunction,            // one side is not a function type at all
    ParameterCount,          // 6.7.6.3p15 — "agree in the number of parameters"
    Ellipsis,                // 6.7.6.3p15 — "and in use of the ellipsis terminator"
    ParameterType,           // 6.7.6.3p15 — corresponding parameters, unqualified
    ParameterQualification,  // 6.7.6.1p2  — a POINTED-TO type's qualifiers differ
    ReturnType,              // 6.7.6.3p15 — "compatible return types"
    ReturnQualification,     // 6.7.6.1p2  — inside the return type
    CallingConvention,       // the FnSig's cc scalar
    Unenumerated,            // interned differently on an axis not walked here
};

// ★★ P44 (item (a) of D-C23-REDECL-QUALIFIER-AXIS-HAS-THREE-UNCLAIMED-SOURCES)
// — `QualifierSpine` AND `DeclaredQualification` NOW LIVE IN
// `core/types/declared_qualification.hpp`, AND THE MOVE IS THE LAYERING, NOT
// TIDYING. A claim is produced by THREE layers that all sit BELOW this one: the
// semantic declarator walk, the hir-text type decoder (a shipped-descriptor
// signature's `const<…>` spelling), and the shipped-library descriptor reader
// that stores what the decoder produced. `src/hir` and `src/ffi` must not
// include an `analysis/semantic` header from their own PUBLIC headers, so the
// vocabulary moved down to where every producer and the one consumer can see it.
// THE COMPARISON STAYED HERE — this file is still the one place the question
// "are these two declarations compatible" is answered.

// One side of the comparison: the resolved type, plus an optional qualification
// claim. A null `qualification` is the shipped-descriptor case — the row states a
// signature and says nothing about qualifiers.
struct DeclaredFunction {
    TypeId                       type{};
    DeclaredQualification const* qualification = nullptr;
};

// How the two sides' SCALAR LEAVES are to be compared.
//
// ★★ THIS IS NOT A STRICTNESS DIAL — IT NAMES WHOSE VOCABULARY EACH SIDE IS
// WRITTEN IN, and picking wrong REFUSES LEGAL C. ✔MEASURED by a prior cycle and
// recorded in the platform-realization pass: a shipped descriptor spells integers
// in hir-text (`random: fn() -> i64`) while a C declaration carries the C
// SPELLING's type identity (`long` interns distinctly from a bare `i64` —
// [[D-LANG-TYPE-IDENTITY-VOCABULARY]]), so a TypeId comparison of
// `extern long random(void);` against that row is UNEQUAL and the perfectly legal
// program is rejected. That measurement is why the first attempt at this check
// was abandoned rather than shipped.
//
//   * `SourceVocabulary` — BOTH sides are declarations in the SAME source
//     language, so two spellings that intern differently ARE different types and
//     identity is the right relation (`long` and `long long` must not merge).
//   * `PlatformVocabulary` — ONE side is a platform descriptor, which states a
//     REPRESENTATION and no source spelling at all. Leaves are then compared
//     spelling-blind: same kind, same scalars, recursively the same operands.
//
// ⚠ THE PLATFORM MODE IS DELIBERATELY WEAKER, AND THE COST IS STATED: it cannot
// tell `long` from `long long` where a target makes both i64, so
// `extern long long random(void);` over `#include <stdlib.h>` is accepted where
// gcc refuses it. That is a MISSED diagnostic, never a refused legal program —
// the only safe direction — and it is a limit of what the descriptor SAYS, not of
// this oracle. It closes when a descriptor row can spell a source vocabulary.
enum class LeafComparison : std::uint8_t { SourceVocabulary, PlatformVocabulary };

struct RedeclarationVerdict {
    RedeclarationDivergence axis = RedeclarationDivergence::None;
    // 1-based ordinal of the diverging parameter; 0 when the axis is not
    // parameter-keyed. 1-based because it is rendered to a human.
    std::size_t             parameterOrdinal = 0;

    [[nodiscard]] bool compatible() const noexcept {
        return axis == RedeclarationDivergence::None;
    }
};

namespace detail::redecl {

// Are `a` and `b` the same type MODULO the VOCABULARY SPELLING of their scalar
// leaves? The recursive form of `TypeInterner::sameRepresentation` (which is
// deliberately ONE level deep — it exists to license a retag, not to answer
// compatibility), and the "spelling-blind compatibility query on the interner"
// the platform-realization pass named as this check's prerequisite.
//
// ⚠ A NOMINAL type (struct / union / enum / extension) is answered by IDENTITY
// ALONE and never structurally. Two distinct structs may have identical field
// TypeIds — `sameRepresentation` would call them equal, which is right for a
// retag and WRONG here, where merging them would bind two different layouts to
// one symbol. Reaching this point means the ids already differed, so the answer
// is no.
//
// ★★★ P55 — AN EXPLICIT HEAP WORK STACK AND A VISITED SET, WITH **NO DEPTH CAP
// AT ALL**, AND THE CAP THIS REPLACES WAS A WRONG ANSWER RATHER THAN A LIMIT
// ([[D-SEMANTIC-DEPTH-CAPS-TRUNCATE-INTO-TWO-WRONG-ANSWERS]]).
//
// The old body recursed per derivation level and answered `depth > 16 ⇒ NOT
// COMPATIBLE`. That is not a refusal a user can see and work around — it is a
// FABRICATED verdict: this predicate's `false` becomes
// `S_IncompatibleRedeclaration` at the caller, so a perfectly legal declaration
// past the cap was REJECTED. ✔MEASURED at this cycle's HEAD, through the real
// CLI on `x86_64:elf64-x86_64-linux-exec`, against a shipped-library descriptor
// whose parameter is an N-level pointer chain and a source declaration of the
// SAME function spelled `long` where the row spells `i64`: N=16 compiles clean,
// **N=17 is refused** with "parameter 1 has a different type". ✔All four
// references, probed SEPARATELY, ACCEPT the equivalent 63-level program (gcc
// 13.3.0 `-std=c2x`, clang 18.1.3 `-std=c23`, mingw-w64 gcc 13.2.0, MSVC
// 19.51.36252 `/std:clatest`, each `-pedantic-errors` where it has one), so the
// cap put DSS BELOW THE UNION with no workaround available to the user at all.
//
// ⚠ RAISING 16 TO A BIGGER NUMBER WOULD HAVE MOVED THE WRONG ANSWER, NOT REMOVED
// IT. The walk is now iterative over a heap `std::vector`, so host stack depth is
// no longer a function of the input (the operator's 2026-09-02 no-recursion
// ruling), and it TERMINATES ON ANY GRAPH — including a cyclic one — because a
// pair is walked at most once.
//
// ★ WHY THE VISITED SET IS THE RIGHT TERMINATION ARGUMENT AND A DEPTH COUNTER IS
// NOT. Re-entering a pair means the two types are related to themselves the same
// way one level down, which is precisely the co-inductive "assume equal, refute
// on a difference" answer that structural equivalence of recursive types is
// DEFINED by — so `continue` here is the CORRECT verdict, not a truncation. It
// doubles as memoization: a type graph that SHARES a deep subtree (`fn(T, T, T)`)
// is walked once per distinct pair rather than once per occurrence.
//
// ⚠ A NOMINAL type still short-circuits above, so the set is normally never
// touched: `std::unordered_set` allocates nothing until its first insert, and the
// two early-outs answer the overwhelmingly common case before that.
[[nodiscard]] inline bool spellingBlindCompatible(TypeInterner const& in, TypeId a,
                                                  TypeId b) {
    if (!a.valid() || !b.valid()) return false;
    if (a.v == b.v) return true;

    // The pairs still to compare. LIFO, pushed in reverse so operands come back
    // out in source order and the FIRST difference found is the shallowest,
    // leftmost one — the same one the recursion used to report.
    std::vector<std::pair<TypeId, TypeId>> work;
    work.emplace_back(a, b);
    // The pairs already ASSUMED equal. Ordered — (x,y) and (y,x) are different
    // keys — which costs one extra entry on a symmetric walk and keeps the key
    // exactly the pair that was pushed.
    std::unordered_set<std::uint64_t> assumedEqual;

    while (!work.empty()) {
        auto const [x, y] = work.back();
        work.pop_back();
        if (!x.valid() || !y.valid()) return false;
        if (x.v == y.v) continue;
        if (!assumedEqual
                 .insert((static_cast<std::uint64_t>(x.v) << 32) | y.v)
                 .second)
            continue;
        TypeKind const kx = in.kind(x);
        if (kx != in.kind(y)) return false;
        switch (kx) {
            case TypeKind::Struct:
            case TypeKind::Union:
            case TypeKind::Enum:
            case TypeKind::Extension:
                return false;   // nominal — identity already answered
            default:
                break;
        }
        // The qualifier skin IS part of the type here (a `volatile` pointee is a
        // different type from a plain one), unlike in `sameRepresentation` where
        // it is transparent because it changes no bits.
        if (in.isVolatileQualified(x) != in.isVolatileQualified(y)) return false;
        if (in.isAtomicQualified(x) != in.isAtomicQualified(y)) return false;
        {
            auto const scX = in.scalars(x);
            auto const scY = in.scalars(y);
            if (scX.size() != scY.size()) return false;
            for (std::size_t i = 0; i < scX.size(); ++i)
                if (scX[i] != scY[i]) return false;
        }
        auto const opsX = in.operands(x);
        auto const opsY = in.operands(y);
        if (opsX.size() != opsY.size()) return false;
        for (std::size_t i = opsX.size(); i-- > 0;)
            work.emplace_back(opsX[i], opsY[i]);
    }
    return true;
}

// The leaf relation for one comparison mode. Everything above the leaves — arity,
// ellipsis, the pointer spine — is walked identically in both modes; only the
// question "are these two types the same" changes.
[[nodiscard]] inline bool leavesCompatible(TypeInterner const& in, TypeId a,
                                           TypeId b, LeafComparison mode) {
    return mode == LeafComparison::SourceVocabulary
               ? a.v == b.v
               : spellingBlindCompatible(in, a, b);
}

// C23 6.7.6.3p15, second sentence: "each parameter declared with qualified type
// is taken as having the UNQUALIFIED version of its declared type". So a
// top-level qualifier on a parameter is dropped before the comparison — and
// dropping it is not cosmetic, it is what makes `int f(volatile int);` and
// `int f(int);` the same declaration, which ✔BOTH gcc and clang accept and DSS
// used to refuse.
//
// ⚠ `_Atomic` IS NOT DROPPED, and that asymmetry is the standard's, not a
// shortcut: C23 makes `_Atomic T` a DISTINCT TYPE (its representation, size and
// alignment may all differ), not a qualified spelling of `T`. ✔MEASURED — gcc and
// clang BOTH refuse `int f(_Atomic int); int f(int);` while BOTH accept the
// `volatile` pair above. `stripVolatile` removes the WHOLE qualifier skin, so the
// atomic bit is re-asked separately rather than being smuggled back in.
//
// Reads only const accessors — nothing is interned, so no caller's GuardedSpan is
// invalidated by asking.
[[nodiscard]] inline bool parameterTypesCompatible(TypeInterner const& in,
                                                   TypeId a, TypeId b,
                                                   LeafComparison mode) {
    if (a.v == b.v) return true;
    if (in.isAtomicQualified(a) != in.isAtomicQualified(b)) return false;
    return leavesCompatible(in, in.stripVolatile(a), in.stripVolatile(b), mode);
}

// The FnSig's calling convention (scalar slot 0 — see `TypeInterner::fnSig`).
// Compared EXPLICITLY rather than being left to the `Unenumerated` catch-all, so
// that the catch-all means what it says: an axis this file does not know about.
[[nodiscard]] inline std::int64_t callConvOf(TypeInterner const& in, TypeId t) {
    auto const sc = in.scalars(t);
    return sc.empty() ? -1 : sc[0];
}

// Forward declaration: `spinesDiverge` recurses through a function level's
// parameter claim, and that claim's type is only complete further down.
[[nodiscard]] inline bool nestedParamsDiverge(DeclaredQualification const& a,
                                              DeclaredQualification const& b);

// Two spines are comparable only when they describe the same number of
// derivation levels. A disagreement means one of the two harvests modelled a
// shape the other did not — the honest answer is then "no claim", never a
// diagnostic built on two things that are not the same measurement.
[[nodiscard]] inline bool spinesDiverge(QualifierSpine const& a,
                                        QualifierSpine const& b,
                                        std::uint64_t ignoredLevels) {
    if (a.levels != b.levels) return false;
    // P44: both qualifier axes are masked by the SAME `ignoredLevels`. C23
    // 6.7.6.3p15 says "the unqualified version of its declared type" — it names
    // no particular qualifier, so a parameter's top-level `restrict` is dropped
    // exactly as its top-level `const` is (✔MEASURED: gcc and clang both accept
    // `int f(char *restrict); int f(char *);`).
    if ((((a.constBits ^ b.constBits)
          | (a.restrictBits ^ b.restrictBits)) & ~ignoredLevels) != 0)
        return true;
    // ★★ P44 part (c) — RECURSE INTO A FUNCTION LEVEL'S PARAMETERS.
    //
    // Judged ONLY where BOTH sides carry a claim AT THE SAME LEVEL and the two
    // claims describe the same number of parameters. Every other combination is
    // NO CLAIM and returns "no divergence": one side spelling `(void)` and the
    // other `()` is the same function type written two ways, and a count-keyed
    // refusal there would reject legal C — the one outcome this oracle may never
    // produce. `ignoredLevels = 1` inside, because an inner function's parameters
    // get 6.7.6.3p15's top-level unqualification exactly as an outer one's do.
    for (auto const& [level, aClaim] : a.fnParams) {
        if (aClaim == nullptr) continue;
        auto const bIt = std::ranges::find_if(
            b.fnParams, [lv = level](auto const& e) { return e.first == lv; });
        if (bIt == b.fnParams.end() || bIt->second == nullptr) continue;
        if (nestedParamsDiverge(*aClaim, *bIt->second)) return true;
    }
    return false;
}

// The recursive step for `spinesDiverge`'s function-level arm. Positional, and
// SILENT on any disagreement about how many positions there are — see that
// arm's comment for why a count mismatch must not be a diagnosis.
[[nodiscard]] inline bool nestedParamsDiverge(DeclaredQualification const& a,
                                              DeclaredQualification const& b) {
    if (a.params.size() != b.params.size()) return false;
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        if (!a.params[i].has_value() || !b.params[i].has_value()) continue;
        if (spinesDiverge(*a.params[i], *b.params[i], /*ignoredLevels=*/1u))
            return true;
    }
    return false;
}

}  // namespace detail::redecl

// ── THE ORACLE ────────────────────────────────────────────────────────────────
//
// Are `a` and `b` compatible declarations of one function (C23 6.7.6.3p15)?
//
// The axes are walked in the order a reader would ask them — is it a function at
// all, how many parameters, ellipsis, each parameter, the return type — so the
// FIRST divergence reported is the one an author would fix first. Reaching the
// end with the two TypeIds still unequal means they were interned differently on
// an axis this walk does not enumerate; that is reported as `Unenumerated` rather
// than claimed to be a match, because a compatibility oracle that guesses in the
// permissive direction is exactly how a wrong-ABI call ships.
[[nodiscard]] inline RedeclarationVerdict
functionRedeclarationCompatibility(
    TypeInterner const& in, DeclaredFunction const& a, DeclaredFunction const& b,
    LeafComparison mode = LeafComparison::SourceVocabulary) {
    using D = RedeclarationDivergence;
    if (!a.type.valid() || !b.type.valid()) return {};   // unresolved ⇒ no verdict
    if (in.kind(a.type) != TypeKind::FnSig
        || in.kind(b.type) != TypeKind::FnSig) {
        // Not a function pair. The leaf relation is then the whole answer — this
        // oracle makes no claim about object compatibility (C 6.2.7 composite
        // types, the incomplete-array relaxation) and its callers keep owning that.
        return detail::redecl::leavesCompatible(in, a.type, b.type, mode)
                   ? RedeclarationVerdict{}
                   : RedeclarationVerdict{D::NotAFunction, 0};
    }
    auto const aParams = in.fnParams(a.type);
    auto const bParams = in.fnParams(b.type);
    if (aParams.size() != bParams.size()) return {D::ParameterCount, 0};
    if (in.fnIsVariadic(a.type) != in.fnIsVariadic(b.type)) return {D::Ellipsis, 0};
    for (std::size_t i = 0; i < aParams.size(); ++i) {
        if (!detail::redecl::parameterTypesCompatible(in, aParams[i], bParams[i],
                                                      mode))
            return {D::ParameterType, i + 1};
    }
    if (!detail::redecl::leavesCompatible(in, in.fnResult(a.type),
                                          in.fnResult(b.type), mode))
        return {D::ReturnType, 0};
    if (detail::redecl::callConvOf(in, a.type)
        != detail::redecl::callConvOf(in, b.type)) {
        return {D::CallingConvention, 0};
    }

    // ── the qualifier axis (C23 6.7.6.1p2) ──
    // Runs LAST because it is the only axis that can be UNANSWERABLE, and an
    // answerable divergence is always the better report. Judged only where BOTH
    // sides made a claim.
    if (a.qualification != nullptr && b.qualification != nullptr) {
        auto const& aq = *a.qualification;
        auto const& bq = *b.qualification;
        if (aq.params.size() == aParams.size()
            && bq.params.size() == bParams.size()) {
            for (std::size_t i = 0; i < aParams.size(); ++i) {
                if (!aq.params[i].has_value() || !bq.params[i].has_value())
                    continue;    // no claim on this parameter — do not judge it
                // Level 0 is the parameter's OWN top-level qualifier, which
                // 6.7.6.3p15 drops (`char *const` and `char *` are the same
                // parameter). Every deeper level is part of the pointed-to type
                // and 6.7.6.1p2 makes it load-bearing.
                if (detail::redecl::spinesDiverge(*aq.params[i], *bq.params[i],
                                                  /*ignoredLevels=*/1u))
                    return {D::ParameterQualification, i + 1};
            }
        }
        if (aq.result.has_value() && bq.result.has_value()) {
            // ★★ THE RESULT'S OWN TOP LEVEL IS IGNORED TOO, AND THAT IS A
            // MEASUREMENT, NOT A READING OF THE TEXT. 6.7.6.3p15's unqualifying
            // sentence names PARAMETERS only, so the standard alone would say
            // `const int f(void); int f(void);` is a conflict — and ✔clang 18.1.3
            // agrees, REFUSING it and `char *const g(void); char *g(void);` too.
            // ✔gcc 13.3.0 ACCEPTS both. The bar is the DISJUNCTION — if ANY
            // reference accepts a correct construct DSS must — so the top level is
            // dropped here as well.
            //
            // ⚠ THIS IS NOT A BLANKET RELAXATION OF THE RETURN AXIS, and the pair
            // is what proves it: `const char *k(void);` beside `char *k(void);` is
            // a POINTED-TO qualifier, ✔REFUSED by gcc AND clang AND mingw-w64 gcc,
            // and it still fires here because only bit 0 is masked.
            if (detail::redecl::spinesDiverge(*aq.result, *bq.result,
                                              /*ignoredLevels=*/1u))
                return {D::ReturnQualification, 0};
        }
    }

    // ⛔ NO RAW-TypeId FALLBACK HERE, AND ITS ABSENCE IS LOAD-BEARING RATHER THAN
    // AN OVERSIGHT. A FnSig's CONTENT is exactly `operands = [result, params...]`
    // plus `scalars = [cc, isVariadic]` — it carries no nominal name and no
    // extension kind — so the axes above are EXHAUSTIVE, and a trailing
    // `a.type.v != b.type.v ⇒ incompatible` would UNDO every relaxation this
    // oracle deliberately makes: 6.7.6.3p15's parameter unqualification and the
    // platform mode's spelling-blindness both END with two different ids, by
    // construction. ✔MEASURED — an earlier draft carried that fallback and it kept
    // `int f(volatile int); int f(int);` refused, exactly the legal shape the
    // unqualification exists to admit.
    //
    // What a fallback must still catch is the FnSig ENCODING GROWING a scalar this
    // file does not enumerate. That is what this is: the two known scalars are
    // compared above (cc, and `fnIsVariadic`), so any FURTHER scalar — or a count
    // disagreement — means the encoding moved and this oracle is out of date. It
    // says so rather than answering from a walk it knows is incomplete.
    {
        auto const scA = in.scalars(a.type);
        auto const scB = in.scalars(b.type);
        if (scA.size() != scB.size()) return {D::Unenumerated, 0};
        for (std::size_t i = 2; i < scA.size(); ++i)
            if (scA[i] != scB[i]) return {D::Unenumerated, 0};
    }
    return {};
}

// Render a verdict as a NOUN PHRASE naming the diverging axis, for embedding in a
// caller's own sentence. `aLabel` / `bLabel` name the two sides in the caller's
// vocabulary ("it" / "the platform declaration", "this declaration" / "the
// previous declaration") so one describer serves every caller without any of them
// re-deriving which axis moved.
//
// ★ THIS NAMES AN AXIS RATHER THAN PRINTING THE TWO TYPES, and that is deliberate
// rather than lazy: there is no shared type PRINTER in the tree — `hir_text` owns
// the only type-spelling grammar and it is the DESCRIPTOR's spelling, not the
// source language's — so printing here would mint a second spelling authority that
// can drift from it. The axis is what an author acts on.
[[nodiscard]] inline std::string
describeRedeclarationDivergence(TypeInterner const& in, RedeclarationVerdict v,
                                DeclaredFunction const& a,
                                DeclaredFunction const& b,
                                std::string_view aLabel,
                                std::string_view bLabel) {
    using D = RedeclarationDivergence;
    switch (v.axis) {
        case D::None:
            return "the two declarations are compatible";
        case D::NotAFunction:
            return "one of the two declarations is not a function type";
        case D::ParameterCount:
            return std::format("{} takes {} parameter(s) where {} takes {}",
                               aLabel, in.fnParams(a.type).size(), bLabel,
                               in.fnParams(b.type).size());
        case D::Ellipsis:
            return in.fnIsVariadic(a.type)
                       ? std::format("{} is variadic and {} is not", aLabel, bLabel)
                       : std::format("{} is variadic and {} is not", bLabel, aLabel);
        case D::ParameterType:
            return std::format("parameter {} has a different type",
                               v.parameterOrdinal);
        case D::ParameterQualification:
            return std::format(
                "parameter {} points to a DIFFERENTLY QUALIFIED type — C23 "
                "6.7.6.1p2 makes two pointer types compatible only when they are "
                "identically qualified, so a `const` difference below the "
                "parameter's own top level is a different type, not a different "
                "spelling", v.parameterOrdinal);
        case D::ReturnType:
            return "the return type differs";
        case D::ReturnQualification:
            return "the return type points to a DIFFERENTLY QUALIFIED type "
                   "(C23 6.7.6.1p2)";
        case D::CallingConvention:
            return "the two declarations carry different calling conventions";
        case D::Unenumerated:
            break;
    }
    return "the two signatures differ in an attribute the type interner keys on "
           "but this report does not enumerate";
}

}  // namespace dss
