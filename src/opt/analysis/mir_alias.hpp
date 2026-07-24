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
// `D-OPT-MEMORYSSA-CLOBBER-WALK` is CLOSED by `mir_memory_clobbers.hpp`
// (`MirMemoryClobbers` — the pass-wide clobber index + memoized reachability;
// one build per pass, cheap queries) — but note that closure was a PERFORMANCE
// re-implementation of the SAME query semantics, NOT a precision upgrade. It
// did not, by itself, make any consumer loop-aware; the CONSUMER still owns
// composing the queries into a sound region cover. TF-C58 is the cautionary
// case: this docblock's old "loop-carried clobber → owned by
// D-OPT-MEMORYSSA-CLOBBER-WALK" + ":12 that anchor is CLOSED" read together as
// "handled", while CSE's open-coded three-slice cover silently omitted the
// back-edge tail → a real miscompile. See `D-OPT-CSE-LOAD-BACKEDGE-TAIL`.
// The region/loop walkers BELOW are kept
// as the REFERENCE implementation: they define the query semantics, and the
// differential tests assert the index's answers equal these walkers' answers
// on every curated shape. Production consumers (CSE, LICM) query the index.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_id.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_opcode.hpp"

#include <algorithm>
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
    // `Char` and `Byte` ARE classified as primitive here. The Rule 5
    // char-exception (above the caller's switch) is now per-language
    // configurable via `charTypesAliasAll` — when a language sets
    // false (Rust / strict-typed DSL), strict-TBAA MUST be able to
    // distinguish `char*` from `int*` and return No. Classifying
    // Char/Byte as primitive lets Rule 6 fire correctly in that path.
    auto const isPrimitiveNonVoid = [&](TypeKind k) noexcept {
        switch (k) {
            case TypeKind::Bool:
            case TypeKind::I8:  case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
            case TypeKind::U8:  case TypeKind::U16: case TypeKind::U32: case TypeKind::U64:
            case TypeKind::F16: case TypeKind::F32: case TypeKind::F64: case TypeKind::F80:
            case TypeKind::F128:
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
//   5. Either pointee is a character type AND `charTypesAliasAll`
//                                             → Maybe (C99 §6.5 ¶7
//      character-type exception — C/C++/Objective-C declare
//      `charTypesAliasAll = true`; Rust / strict-typed DSLs declare
//      `charTypesAliasAll = false` so this rule is bypassed and
//      Rule 6's strict-TBAA can return No on `char*` vs `int*`)
//   6. Distinct primitive pointees AND `StrictTbaa::Yes`
//                                             → No   (C-style strict-
//      aliasing; opt-in via SemanticConfig)
//   7. Otherwise                              → Maybe (conservative)
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
//   — Loop-carried clobber is NOT out of scope and is NOT free: `mirMayAlias`
//     is a pure two-pointer predicate with no notion of control flow, so a
//     CONSUMER that compares two program POINTS owes a region cover that
//     includes every path between them — the back edge included. Composing
//     that cover is the consumer's obligation (`D-OPT-CSE-LOAD-BACKEDGE-TAIL`);
//     `MirMemoryClobbers::blockReachesItselfAvoiding` is the wrap-detection
//     primitive provided for it. Do NOT read this bullet as "the index handles
//     loops for you".
// Defaults (`StrictTbaa::No` + `charTypesAliasAll = true`) match the
// MOST CONSERVATIVE direction — a consumer that forgets to thread the
// per-language values gets soundness, never wrong-No. Production
// consumers (CSE / LICM Load admission) MUST read from
// `Mir::aliasingMode()` + `Mir::charTypesAliasAll()` to get language-
// correct precision; the defaults are for the test layer and for
// callers exploring alias-ness on values whose language origin is
// indeterminate.
[[nodiscard]] inline MirAliasResult mirMayAlias(
    Mir const&          mir,
    TypeInterner const& interner,
    MirInstId           ptrA,
    MirInstId           ptrB,
    StrictTbaa          strictTBAA       = StrictTbaa::No,
    bool                charTypesAliasAll = true)
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

    // Rule 5: C99 §6.5 ¶7 character-type exception (per-language opt-in).
    // Char/Byte may alias an object of ANY type when the source language
    // declares this semantic. Default `true` matches C/C++/Objective-C;
    // a Rust frontend (or a strict-typed DSL where `char` is opaque)
    // would set `false` via `Mir::charTypesAliasAll()` so a `char*`
    // vs `i32*` strict-TBAA query CAN return `No`. Threading anchor:
    // closes `D-OPT-MIR-ALIAS-CHAR-EXCEPTION-OVERRIDE`.
    if (charTypesAliasAll) {
        auto const isCharType = [&](TypeId t) noexcept {
            TypeKind const k = interner.kind(t);
            return k == TypeKind::Char || k == TypeKind::Byte;
        };
        if (isCharType(pointeeA) || isCharType(pointeeB)) {
            return MirAliasResult::Maybe;
        }
    }

    if (strictTBAA == StrictTbaa::Yes
     && detail::isDistinctPrimitivePair(interner, pointeeA, pointeeB)) {
        return MirAliasResult::No;                                     // Rule 6
    }

    return MirAliasResult::Maybe;                                      // Rule 7
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
// REFERENCE implementation (test oracle since D-OPT-MEMORYSSA-CLOBBER-WALK
// closed): production CSE queries `MirMemoryClobbers::anyClobberBetween`, whose
// region semantics are DEFINED by this walker — the differential pins assert
// equality. `preds` MUST be `mirBuildPredecessors(mir)` for the SAME module —
// the caller computes it ONCE (it is invariant for the module's lifetime within
// a single rebuild pass: the module is `const` until the pass's `finish()`).
[[nodiscard]] inline std::vector<MirBlockId>
mirRegionBetween(
    Mir const&                                  mir,
    MirBlockId                                  loadBlock,
    MirBlockId                                  useBlock,
    std::vector<std::vector<MirBlockId>> const& preds)
{
    std::vector<MirBlockId> result;
    if (!loadBlock.valid() || !useBlock.valid()) return result;

    std::uint32_t const blockCount = static_cast<std::uint32_t>(mir.blockCount());
    if (blockCount == 0) return result;
    // The caller's precomputed whole-module preds must match this module — a
    // function-local or stale-module preds would silently mis-scope the region.
    if (preds.size() != mir.blockCount()) {
        std::fprintf(stderr,
            "dss::opt::analysis::mirRegionBetween fatal: preds.size()=%zu != "
            "mir.blockCount()=%zu — caller passed a stale/foreign predecessor map.\n",
            preds.size(), mir.blockCount());
        std::abort();
    }
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

    // Backward-reachable from `useBlock` (via predecessors — `preds`, computed
    // ONCE by the caller for this module; invariant across all queries, so no
    // per-query whole-module rebuild here anymore).
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
    // Iterate the SMALLER reachability set (both are function-local, typically
    // tiny) instead of [1, blockCount) — the per-query whole-module scan was the
    // second half of the CSE hot-path cost. The explicit sort restores the
    // by-slot postcondition (an unordered_set walk is unordered); the resulting
    // set + order are byte-identical to the old whole-module loop (same endpoint
    // exclusions, bounds `1 <= v < blockCount`, and module tag).
    auto const& smaller = fwd.size() <= bwd.size() ? fwd : bwd;
    auto const& larger  = fwd.size() <= bwd.size() ? bwd : fwd;
    for (std::uint32_t const v : smaller) {
        if (v < 1u || v >= blockCount) continue;
        if (v == loadBlock.v || v == useBlock.v) continue;
        if (larger.count(v)) {
            result.push_back(MirBlockId{v, mir.id().v});
        }
    }
    std::sort(result.begin(), result.end(),
              [](MirBlockId a, MirBlockId b) { return a.v < b.v; });
    return result;
}

// THE per-instruction Load-motion clobber predicate — the ONE chokepoint
// every "may this instruction invalidate a Load-reuse/hoist across it?"
// scan funnels through (the CSE in-block slices + the region walker below
// + LICM's loop wrapper), so a clobber class added here covers every
// consumer by construction (the multi-site-contract rule).
//
//   * Store — PRECISE: `mirMayAlias(loadPtr, storePtrOperand)`. The Store's
//     pointer operand convention is `operands[1]` (operand 0 is the stored
//     value); this matches Mem2Reg's `Store[op1]` gate.
//   * any OTHER memory-CLOBBERING op (c113, D-CSUBSET-INTRINSIC-BARRIER,
//     audit-F1) — an OPAQUE clobber: a Call/IntrinsicCall may write memory
//     the Load's pointer aliases, an AtomicCas IS a store, a
//     CompilerBarrier (_ReadWriteBarrier) is an ordering fence that must
//     forbid motion across it — and no precise alias test is possible for
//     any of them. Keyed on `opcodeClobbersMemory` (the declarative
//     positive list beside `opcodeInfo`), NOT on `hasSideEffects` — that
//     is a DCE-liveness flag, true for terminators/Alloca/the Va* leaves,
//     none of which write aliasable memory; conflating the two disables
//     Load motion wholesale (every loop body ends in a terminator — the
//     review-caught LICM red).
//   * everything else — pure or non-writing; never a clobber.
[[nodiscard]] inline bool
mirInstClobbersLoadPtr(
    Mir const&          mir,
    TypeInterner const& interner,
    MirInstId           loadPtr,
    MirInstId           inst,
    StrictTbaa          strictTBAA        = StrictTbaa::No,
    bool                charTypesAliasAll = true)
{
    MirOpcode const op = mir.instOpcode(inst);
    if (op != MirOpcode::Store) {
        return opcodeClobbersMemory(op);
    }
    auto const ops = mir.instOperands(inst);
    if (ops.size() < 2) {
        std::fprintf(stderr,
            "dss::opt::analysis::mirInstClobbersLoadPtr "
            "fatal: Store inst v=%u has fewer than 2 operands "
            "— verifier-contract violation.\n", inst.v);
        std::abort();
    }
    return mirMayAlias(mir, interner, loadPtr, ops[1],
                       strictTBAA, charTypesAliasAll)
           != MirAliasResult::No;
}

// True iff any instruction in the named region's blocks may clobber the
// given Load pointer (see `mirInstClobbersLoadPtr` — precise for Stores,
// opaque for other side-effecting ops). Returns true on the first clobber
// found (short-circuit). `strictTBAA` threaded from `Mir::aliasingMode()`
// (cycle 10c wiring) or fixed by the caller in the meantime.
[[nodiscard]] inline bool
mirAnyMayAliasingStoreInRegion(
    Mir const&                       mir,
    TypeInterner const&              interner,
    MirInstId                        loadPtr,
    std::span<MirBlockId const>      region,
    StrictTbaa                       strictTBAA       = StrictTbaa::No,
    bool                             charTypesAliasAll = true)
{
    for (MirBlockId const b : region) {
        std::uint32_t const ninst = mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < ninst; ++i) {
            if (mirInstClobbersLoadPtr(mir, interner, loadPtr,
                                       mir.blockInstAt(b, i),
                                       strictTBAA, charTypesAliasAll)) {
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
    StrictTbaa                       strictTBAA       = StrictTbaa::No,
    bool                             charTypesAliasAll = true)
{
    return mirAnyMayAliasingStoreInRegion(mir, interner, loadPtr,
                                          loopBody, strictTBAA,
                                          charTypesAliasAll);
}

} // namespace dss::opt::analysis
