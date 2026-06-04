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
#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <unordered_set>
#include <vector>

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

// ── Region + loop clobber walkers ────────────────────────────────────
//
// CSE/LICM Load admission needs to answer "is there a may-aliasing
// Store between two program points?" The MVP implementation walks a
// bounded BLOCK SET (NOT path enumeration — exponential in the worst
// case): for each block reachable from `from` AND from which `to` is
// reachable, scan its Stores and call `mirMayAlias` on each store's
// pointer operand vs the load's pointer.
//
// This is O(loads × region × stores) worst-case — fine for the v1
// corpus but the classic place that doesn't scale. The standard upgrade
// is MemorySSA — compute each load's clobbering def once at function
// scope, then CSE/LICM/DSE query in ~O(1). Anchored at
// `D-OPT-MEMORYSSA-CLOBBER-WALK`; built when the region-walk step-cap
// fires on real input OR a third memory-clobber consumer (DSE) lands.

// Compute the STRICTLY-BETWEEN region: blocks forward-reachable from
// `loadBlock` AND backward-reachable from `useBlock`, EXCLUDING both
// endpoints (caller owns the in-block walks for `loadBlock`'s tail
// and `useBlock`'s head — they have the inst-index context needed to
// scope those scans). Returns block ids deterministic-sorted by slot.
//
// Naming: `loadBlock` is where the dominator Load lives (the "from"
// end of the path); `useBlock` is where the use being CSE'd-against
// lives (the "to" end). The names lock direction so a caller can't
// silently swap arguments — see the docblock on `mirMayAlias` for
// the same StrictTbaa rationale.
//
// Bounded by step-cap = block-count * 4 + 4; fail-loud on overflow
// (signals a malformed CFG the verifier should have caught).
[[nodiscard]] inline std::vector<MirBlockId>
mirRegionBetween(
    Mir const&        mir,
    MirBlockId        loadBlock,
    MirBlockId        useBlock)
{
    std::vector<MirBlockId> result;
    if (!loadBlock.valid() || !useBlock.valid()) return result;

    std::uint32_t const blockCount = static_cast<std::uint32_t>(mir.blockCount());
    if (blockCount == 0) return result;
    std::uint32_t const stepCap = blockCount * 4u + 4u;

    // Forward-reachable from `loadBlock`.
    std::unordered_set<std::uint32_t> fwd;
    {
        std::vector<MirBlockId> work;
        work.push_back(loadBlock);
        fwd.insert(loadBlock.v);
        std::uint32_t steps = 0;
        while (!work.empty()) {
            if (++steps > stepCap) {
                std::fprintf(stderr,
                    "dss::opt::analysis::mirRegionBetween fatal: "
                    "step-cap exceeded in forward walk from #%u — "
                    "malformed CFG (the verifier should have caught "
                    "this; D-OPT-MEMORYSSA-CLOBBER-WALK trigger).\n",
                    loadBlock.v);
                std::abort();
            }
            MirBlockId const b = work.back();
            work.pop_back();
            for (MirBlockId const s : mir.blockSuccessors(b)) {
                if (!s.valid() || s.v >= blockCount) continue;
                if (fwd.insert(s.v).second) work.push_back(s);
            }
        }
    }

    // Backward-reachable from `useBlock` (via predecessors).
    auto const preds = mirBuildPredecessors(mir);
    std::unordered_set<std::uint32_t> bwd;
    {
        std::vector<MirBlockId> work;
        work.push_back(useBlock);
        bwd.insert(useBlock.v);
        std::uint32_t steps = 0;
        while (!work.empty()) {
            if (++steps > stepCap) {
                std::fprintf(stderr,
                    "dss::opt::analysis::mirRegionBetween fatal: "
                    "step-cap exceeded in backward walk to #%u — "
                    "malformed CFG.\n", useBlock.v);
                std::abort();
            }
            MirBlockId const b = work.back();
            work.pop_back();
            if (b.v >= preds.size()) continue;
            for (MirBlockId const p : preds[b.v]) {
                if (!p.valid() || p.v >= blockCount) continue;
                if (bwd.insert(p.v).second) work.push_back(p);
            }
        }
    }

    // Intersection minus BOTH endpoints — caller's in-block walks own
    // loadBlock's tail (after canonical Load) + useBlock's head (before
    // current use). This keeps the region walker symmetric (excludes
    // both) and prevents the dead-code-masking-bug class where a
    // useBlock-included region + a redundant head-of-useBlock scan
    // could disagree on which scanner found a clobber.
    for (std::uint32_t v = 1; v < blockCount; ++v) {
        if (v == loadBlock.v) continue;
        if (v == useBlock.v) continue;
        if (fwd.count(v) && bwd.count(v)) {
            result.push_back(MirBlockId{v, mir.id().v});
        }
    }
    // result is already sorted by `v` because the loop emits in order.
    return result;
}

// True iff any Store instruction in the named region's blocks may
// alias the given Load pointer. Walks each block's Stores in scan
// order; for each, calls `mirMayAlias(loadPtr, storePtrOperand)`. The
// Store's pointer operand convention is `operands[1]` (operand 0 is
// the stored value); this matches Mem2Reg's `Store[op1]` gate.
//
// Returns true on the first may-aliasing Store found (short-circuit).
// `strictTBAA` threaded from `Mir::aliasingMode()` (cycle 10c wiring)
// or fixed by the caller in the meantime.
[[nodiscard]] inline bool
mirAnyMayAliasingStoreInRegion(
    Mir const&                       mir,
    TypeInterner const&              interner,
    MirInstId                        loadPtr,
    std::span<MirBlockId const>      region,
    StrictTbaa                       strictTBAA = StrictTbaa::No)
{
    for (MirBlockId const b : region) {
        std::uint32_t const ninst = mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < ninst; ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (mir.instOpcode(id) != MirOpcode::Store) continue;
            auto const ops = mir.instOperands(id);
            if (ops.size() < 2) {
                std::fprintf(stderr,
                    "dss::opt::analysis::mirAnyMayAliasingStoreInRegion "
                    "fatal: Store inst v=%u has fewer than 2 operands "
                    "— verifier-contract violation.\n", id.v);
                std::abort();
            }
            MirInstId const storePtr = ops[1];
            if (mirMayAlias(mir, interner, loadPtr, storePtr, strictTBAA)
                != MirAliasResult::No) {
                return true;  // could clobber
            }
        }
    }
    return false;
}

// True iff any Store in the loop body may alias the given Load
// pointer. Convenience wrapper over `mirAnyMayAliasingStoreInRegion`
// for LICM's hoist-admission gate. Loop body comes from
// `mirNaturalLoops` (dom-tree natural-loop analysis).
[[nodiscard]] inline bool
mirAnyMayAliasingStoreInLoop(
    Mir const&                       mir,
    TypeInterner const&              interner,
    MirInstId                        loadPtr,
    std::span<MirBlockId const>      loopBody,
    StrictTbaa                       strictTBAA = StrictTbaa::No)
{
    return mirAnyMayAliasingStoreInRegion(mir, interner, loadPtr,
                                          loopBody, strictTBAA);
}

} // namespace dss::opt::analysis
