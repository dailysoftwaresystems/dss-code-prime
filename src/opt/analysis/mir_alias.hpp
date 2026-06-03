#pragma once

// MIR-tier alias-analysis substrate (opens `D-OPT-LOAD-ALIAS-ANALYSIS`).
//
// One predicate — `mirMayAlias` — answering "could these two pointer SSA
// values name the same memory?" over MIR vocabulary + the core type
// lattice only. No source/target/format identity branches. Consumers
// (CSE + LICM Load admission) thread the per-language strict-aliasing
// flag from `SemanticConfig.PointerAliasingRules`; that wiring lives in
// the consumer cycle and is anchored at `D-OPT-LOAD-ALIAS-ANALYSIS`.
//
// Long-term substrate: MemorySSA (one walk per function, then ~O(1)
// clobber queries). Anchored at `D-OPT-MEMORYSSA-CLOBBER-WALK`; built
// when the region-walk step-cap fires on real input OR a third memory-
// clobber consumer (DSE) lands.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_id.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace dss::opt::analysis {

// Three-state alias relationship between two pointer-typed SSA values.
// `Yes` is separated from `Maybe` so future CSE/LICM can short-circuit
// the same-pointer fast path; `No` is the precision win that admits
// reordering across the pair.
enum class MirAliasResult : std::uint8_t { No, Maybe, Yes };

// Strong-typed flag for the per-language strict-TBAA opt-in. A bool
// would silently swap with an adjacent argument; the enum forces an
// explicit `StrictTbaa::Yes`/`No` at every call site. Threaded from
// `SemanticConfig.PointerAliasingRules.strictAliasingOnDistinctTypes`
// in the consumer cycle (`D-OPT-LOAD-ALIAS-ANALYSIS`).
enum class StrictTbaa : std::uint8_t { No, Yes };

namespace detail {

// Pointee TypeId for a Ptr or Ref; `InvalidType` otherwise. Lattice
// stores pointee as `operands[0]` for both Ptr and Ref.
[[nodiscard]] inline TypeId mirPointeeType(
    TypeInterner const& interner, TypeId t) noexcept
{
    if (!t.valid()) return TypeId{};
    TypeKind const k = interner.kind(t);
    if (k != TypeKind::Ptr && k != TypeKind::Ref) return TypeId{};
    auto const ops = interner.operands(t);
    if (ops.empty()) return TypeId{};
    return ops.front();
}

// True iff both pointees are distinct non-Void primitive kinds.
// Interner canonicalization means same-kind primitives share TypeIds,
// so kind-difference is sufficient.
//
// Future-drift guard: this enumerates the v1 primitive kinds. If a new
// `TypeKind` enumerator lands (e.g. `BFloat16`, `F8`), the default arm
// below silently classifies it as non-primitive — distinct `Ptr<bfloat>`
// vs `Ptr<i32>` would fall to Rule 6 (Maybe) instead of Rule 5 (No),
// a precision regression. Anchored as `D-OPT-MIR-ALIAS-TYPEKIND-DRIFT`
// in plan 22 §3.1 — adding a primitive kind triggers a manual extension
// here; the trigger is "TypeKind enumerator added" and the closure is
// one-line.
[[nodiscard]] inline bool isDistinctPrimitivePair(
    TypeInterner const& interner,
    TypeId              pointeeA,
    TypeId              pointeeB) noexcept
{
    auto const isPrimitiveNonVoid = [&](TypeKind k) noexcept {
        switch (k) {
            case TypeKind::Bool:
            case TypeKind::I8:  case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
            case TypeKind::U8:  case TypeKind::U16: case TypeKind::U32: case TypeKind::U64:
            case TypeKind::F16: case TypeKind::F32: case TypeKind::F64: case TypeKind::F128:
            case TypeKind::Char:
            case TypeKind::Byte:
                return true;
            default:
                return false;
        }
    };
    TypeKind const kA = interner.kind(pointeeA);
    TypeKind const kB = interner.kind(pointeeB);
    return isPrimitiveNonVoid(kA) && isPrimitiveNonVoid(kB) && kA != kB;
}

} // namespace detail

// May-alias predicate for two MIR pointer-typed SSA values.
//
// Rules (first match wins):
//   1. Same SSA id                            → Yes
//   2. Both are distinct `Alloca` defs        → No   (distinct stack slots)
//   3. Either operand isn't Ptr/Ref-typed     → Maybe (caller exploring
//      pointer-ness on a non-pointer SSA value)
//   4. Either pointee is `Void`               → Maybe (universal escape
//      hatch — `void*` may legally alias anything)
//   5. Distinct primitive pointees AND `StrictTbaa::Yes`
//                                             → No   (C-style strict-
//      aliasing; opt-in via SemanticConfig)
//   6. Otherwise                              → Maybe (conservative)
//
// `strictTBAA` defaults to `StrictTbaa::No`; consumers thread the per-
// language value in. Until threaded, every pair stays Maybe — substrate
// is sound out of the box.
//
// Aborts loud on any arena-provenance violation in `Mir::instOpcode`/
// `Mir::instType` (out-of-range id, foreign-arena tag, or
// `InvalidMirInstId`). Consumers MUST pass ids that belong to `mir`.
//
// Out of scope (anchored):
//   — Pointer arithmetic / GEP-derived may-alias (needs interval analysis)
//   — Cross-function escape analysis (address-taken propagation)
//   — Heap-allocation tracking (malloc/calloc-distinct)
//   — Loop-carried clobber (MemorySSA: `D-OPT-MEMORYSSA-CLOBBER-WALK`)
[[nodiscard]] inline MirAliasResult mirMayAlias(
    Mir const&          mir,
    TypeInterner const& interner,
    MirInstId           ptrA,
    MirInstId           ptrB,
    StrictTbaa          strictTBAA = StrictTbaa::No)
{
    if (!ptrA.valid() || !ptrB.valid()) {
        std::fprintf(stderr,
            "dss::opt::analysis::mirMayAlias fatal: invalid SSA id "
            "(ptrA.valid=%d ptrB.valid=%d) — consumers must filter "
            "InvalidMirInstId before calling.\n",
            ptrA.valid() ? 1 : 0, ptrB.valid() ? 1 : 0);
        std::abort();
    }

    if (ptrA.v == ptrB.v) return MirAliasResult::Yes;                  // Rule 1

    MirOpcode const opA = mir.instOpcode(ptrA);
    MirOpcode const opB = mir.instOpcode(ptrB);
    if (opA == MirOpcode::Alloca && opB == MirOpcode::Alloca) {        // Rule 2
        return MirAliasResult::No;
    }

    TypeId const pointeeA = detail::mirPointeeType(interner, mir.instType(ptrA));
    TypeId const pointeeB = detail::mirPointeeType(interner, mir.instType(ptrB));
    if (!pointeeA.valid() || !pointeeB.valid()) return MirAliasResult::Maybe; // Rule 3

    if (interner.kind(pointeeA) == TypeKind::Void
     || interner.kind(pointeeB) == TypeKind::Void) {                   // Rule 4
        return MirAliasResult::Maybe;
    }

    if (strictTBAA == StrictTbaa::Yes
     && detail::isDistinctPrimitivePair(interner, pointeeA, pointeeB)) {
        return MirAliasResult::No;                                     // Rule 5
    }

    return MirAliasResult::Maybe;                                      // Rule 6
}

} // namespace dss::opt::analysis
