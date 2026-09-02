#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// ── THE QUALIFICATION CLAIM A DECLARATION MAKES, AS DATA ──────────────────────
//
// C23 redeclaration compatibility is qualifier-SENSITIVE (6.7.6.1p2 compares
// pointed-to types INCLUDING their qualifiers) while DSS type IDENTITY is
// deliberately qualifier-BLIND for `const` and `restrict`: neither affects
// codegen or layout, so neither is interned, and interning either would perturb
// every type comparison in the compiler to serve one rule. The claim therefore
// travels ALONGSIDE the TypeId, and these two structs are that side channel.
//
// ★★ WHY THIS HEADER EXISTS AT ALL, RATHER THAN THE STRUCTS LIVING WITH THE
// ORACLE THAT CONSUMES THEM (P44, item (a) of
// D-C23-REDECL-QUALIFIER-AXIS-HAS-THREE-UNCLAIMED-SOURCES). They started in
// `analysis/semantic/redeclaration_compat.hpp`, which is the right home for the
// COMPARISON and the wrong one for the DATA: a claim is now produced by THREE
// layers that all sit BELOW the semantic analyzer —
//   * the semantic declarator walk (`declaratorConstSpine`),
//   * the hir-text type decoder (`parseTypeFromText`, for a shipped-descriptor
//     signature's `const<…>` / `restrict<…>` spelling),
//   * the shipped-library descriptor reader, which stores what that decoder
//     produced on `ShippedSymbol`.
// `src/hir` and `src/ffi` must not include `src/analysis/semantic` headers in
// their own PUBLIC headers, so the vocabulary moved down to where every producer
// and the one consumer can all see it. The oracle stayed put.
//
// ★★ ABSENT IS NOT UNQUALIFIED, AND THAT RULE LIVES WITH THE DATA BECAUSE EVERY
// PRODUCER HAS TO OBEY IT. A `nullopt` spine (or a null claim) means the producer
// COULD NOT READ this axis — a descriptor row that spells no qualifier, a
// declarator shape the walk does not model — and the oracle then does not JUDGE
// that axis. It neither invents a diagnostic nor invents a match. Reading an
// absent claim as "unqualified" would refuse the ubiquitous and legal
// `int printf(const char *, ...);` against a corpus row that simply says nothing.
// The consequence of silence is a possible MISSED diagnostic, never a refused
// legal program, which is the only safe direction for a rule whose false
// positives reject correct code.

namespace dss {

struct DeclaredQualification;   // recursive: a FUNCTION level carries one

// The C qualifier spine of ONE declared type: the qualifiers at each level of
// its derivation chain, OUTERMOST FIRST. Level 0 is the declared entity itself
// (`char *const p` sets bit 0); level i+1 is what level i points to or contains
// (`const char *p` sets bit 1 on a 2-level spine).
//
// ★ `const` AND `restrict` RIDE HERE; `volatile` AND `_Atomic` DELIBERATELY DO
// NOT, AND THE SPLIT IS BY WHAT THE INTERNER ALREADY CARRIES. `volatile` and
// `_Atomic` ARE interned (`QualBit`), so they already ride the TypeId and the
// oracle's structural comparison sees them — ✔MEASURED, DSS refuses
// `int v(volatile char *); int v(char *);` today, agreeing with gcc and clang.
//
// ⚠ THE BASE LEVEL CAN NEVER CARRY A `restrict` BIT, and that is a language fact
// rather than a walk limitation: C 6.7.3.2p1 restricts `restrict` to POINTER
// types, so only a pointer LAYER can spell it and the head never legally does.
struct QualifierSpine {
    std::uint64_t constBits    = 0;   // bit i == level i is const-qualified
    std::uint64_t restrictBits = 0;   // bit i == level i is restrict-qualified
    std::uint8_t  levels       = 0;   // derivation levels, including the base
    // ★★ THE RECURSIVE HALF. A level that is a FUNCTION type has no pointee, so
    // the flat bitset says nothing about it — and `int (*)(const char *)` beside
    // `int (*)(char *)` differs ONLY there. ✔MEASURED, gcc 13.3.0 and clang
    // 18.1.3 probed separately: both REFUSE that pair and both ACCEPT the
    // identical pair, so the axis is real and it is not reachable from a flat
    // spine.
    //
    // ★ ONLY THE PARAMETERS NEED A NESTED CLAIM, AND THAT IS THE DECLARATOR
    // ORDERING RULE PAYING OFF RATHER THAN A SIMPLIFICATION. `ctors(D') ++
    // suffixes(D) ++ reverse(L(D))`, base underneath, already places an inner
    // function's RESULT as the DEEPER LEVELS OF THIS SAME SPINE — ✔checked on
    // `const char *(*)(void)`, which folds to [Ptr, Fn, Ptr] + base(const), the
    // const landing on the returned pointee exactly where 6.7.6.1p2 wants it. So
    // the result is already judged; the parameters are what a flat chain cannot
    // reach, and they are all that rides here.
    //
    // Keyed by LEVEL rather than assumed to be one, because a spine may contain
    // more than one function level (`int (*(*)(char))(const char *)`). A level
    // with no entry makes NO claim.
    std::vector<std::pair<std::uint8_t,
                          std::shared_ptr<DeclaredQualification const>>> fnParams;

    // NOT `= default`: the defaulted comparison would compare `shared_ptr`
    // IDENTITY, so two structurally identical claims built by two producers
    // would read as different. `detail::redecl::spinesDiverge` is the one
    // comparison and it recurses.
    [[nodiscard]] friend bool operator==(QualifierSpine const& a,
                                         QualifierSpine const& b) noexcept {
        return a.constBits == b.constBits && a.restrictBits == b.restrictBits
               && a.levels == b.levels;
    }
};

// What ONE declaration claims about the qualification of its result and of each
// parameter, positionally. `nullopt` at either granularity means NO CLAIM — see
// the "ABSENT IS NOT UNQUALIFIED" note above.
struct DeclaredQualification {
    std::optional<QualifierSpine>              result;
    std::vector<std::optional<QualifierSpine>> params;

    // True when this claim says nothing at all. A producer that would return an
    // empty claim returns "no claim" instead, so the two spellings of nothing
    // cannot drift apart at a consumer.
    [[nodiscard]] bool empty() const noexcept {
        if (result.has_value()) return false;
        for (auto const& p : params)
            if (p.has_value()) return false;
        return true;
    }
};

}  // namespace dss
