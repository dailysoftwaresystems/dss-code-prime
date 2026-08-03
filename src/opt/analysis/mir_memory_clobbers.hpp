#pragma once

// MIR-tier memory-clobber INDEX (closes `D-OPT-MEMORYSSA-CLOBBER-WALK`).
//
// The pragmatic "MemorySSA-lite" for this substrate: a per-pass ledger of every
// memory-clobbering instruction (keyed on the ONE `opcodeClobbersMemory`
// positive list) + lazily-memoized per-endpoint CFG reachability, so the
// CSE/LICM Load-admission queries stop re-walking the CFG and re-scanning every
// instruction per Load query. The queries answer EXACTLY the same questions the
// reference walkers in `mir_alias.hpp` answer (`mirRegionBetween` +
// `mirAnyMayAliasingStoreInRegion` / `mirAnyMayAliasingStoreInLoop` — kept as
// the differential-test oracle), with the SAME `mirInstClobbersLoadPtr` alias
// predicate called at QUERY time — this class only memoizes the ENUMERATION
// (which instructions to test), never an alias judgment.
//
// WHY NOT a full LLVM MemorySSA (MemoryDef/MemoryUse/MemoryPhi + a
// walk-past-non-aliasing-defs clobber walker): the walk-past cache must key on
// (def, location) to be sound — a per-block cache that stops the backward walk
// at an alias-tested-negative block UNDER-REPORTS (a nearer non-aliasing Store
// masks a farther aliasing one → CSE admits a stale Load → silent miscompile;
// the design-audit constructed the concrete case, pinned as the
// FrontierStopsAtNonAliasingStore test). At this substrate's alias precision
// (`mirMayAlias` is mostly Maybe) the walk-past rarely fires, so the full
// machinery cannot pay for itself; the right cut is: cache the
// loadPtr-INDEPENDENT part (reachability + the clobber ledger), recompute the
// loadPtr-DEPENDENT part (the alias tests, bounded by ACTUAL clobber count).
// If the alias oracle ever gains precision, the walk-past refinement is the
// gated successor `D-OPT-MEMSSA-WALK-PAST-PRECISION`.
//
// Scope/lifetime: ONE instance per pass invocation (runCse/runLicm), built
// beside the pass's hoisted whole-module `preds` while the module is frozen
// (every pass rebuilds into a SEPARATE builder; `mir = finish()` happens only
// after the per-function loop). The stale-module guard below fail-louds any
// use-after-finish (the optimizer mints a fresh MirModuleId per rebuild).
// Agnostic: pure MIR-graph math — no source/target/format identity anywhere.

#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <vector>

namespace dss::opt::analysis {

// One memory-clobbering instruction in the ledger. Deliberately NO cached
// opcode / Store pointer operand (design-audit F1): the query calls the
// UNCHANGED `mirInstClobbersLoadPtr(mir, interner, loadPtr, inst, …)`, which
// re-reads the frozen module — so the alias predicate, its Store-operand
// convention, AND its fail-loud surface stay single-sited in mir_alias.hpp by
// construction (a build-time ops[1] extraction would abort on malformed Stores
// the old walk never evaluates — a fail-loud-surface change, not just a fork).
struct MirMemoryDef {
    MirInstId     inst;            // the clobbering instruction
    std::uint32_t instIdxInBlock;  // its index within its block (terminator included)
};

class MirMemoryClobbers {
public:
    // Build the whole-module ledger in ONE linear pass. `preds` MUST be
    // `mirBuildPredecessors(mir)` for the SAME module (the pass's hoisted map).
    MirMemoryClobbers(Mir const& mir,
                      std::vector<std::vector<MirBlockId>> const& preds)
        : mir_(mir)
        , preds_(preds)
        , moduleIdV_(mir.id().v)
        , blockCount_(static_cast<std::uint32_t>(mir.blockCount())) {
        // Design-audit F2: the boolean-identity proof rests on Store being in
        // the `opcodeClobbersMemory` positive list (the builder's filter is
        // then EXACTLY the support of `mirInstClobbersLoadPtr` — everything
        // skipped returns false unconditionally). Pin it at compile time.
        static_assert(opcodeClobbersMemory(MirOpcode::Store),
                      "MirMemoryClobbers completeness: Store must be in the "
                      "opcodeClobbersMemory positive list (mir_opcode.hpp) — "
                      "the ledger records exactly that list's instructions.");
        if (preds.size() != mir.blockCount()) {
            std::fprintf(stderr,
                "dss::opt::analysis::MirMemoryClobbers fatal: preds.size()=%zu "
                "!= mir.blockCount()=%zu — caller passed a stale/foreign "
                "predecessor map.\n",
                preds.size(), mir.blockCount());
            std::abort();
        }
        // Every function, every block, every instruction INCLUDING the
        // terminator slot (blockInstCount counts it) — a SehTryBegin block
        // terminator in a canonical block's tail must clobber exactly as the
        // reference scan sees it (the c115 SEH-region soundness surface).
        std::size_t const nf = mir.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f = mir.funcAt(fi);
            std::uint32_t const nb = mir.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                MirBlockId const b = mir.funcBlockAt(f, bi);
                std::uint32_t const ninst = mir.blockInstCount(b);
                for (std::uint32_t i = 0; i < ninst; ++i) {
                    MirInstId const id = mir.blockInstAt(b, i);
                    if (opcodeClobbersMemory(mir.instOpcode(id))) {
                        blockClobbers_[b.v].push_back(MirMemoryDef{id, i});
                    }
                }
            }
        }
        clobberBearingBlocks_.reserve(blockClobbers_.size());
        for (auto const& [slot, defs] : blockClobbers_) {
            (void)defs;
            clobberBearingBlocks_.push_back(slot);
        }
        std::sort(clobberBearingBlocks_.begin(), clobberBearingBlocks_.end());
    }

    MirMemoryClobbers(MirMemoryClobbers const&)            = delete;
    MirMemoryClobbers& operator=(MirMemoryClobbers const&) = delete;

    // Q1 — any clobber of `loadPtr` in `blk`'s index range [lo, hi).
    // Enumeration-identical to the reference per-instruction scan
    // (`storesClobber` in cse.cpp): same range, same program order, same
    // short-circuit; non-clobber instructions are skipped because
    // `mirInstClobbersLoadPtr` returns false for them unconditionally.
    [[nodiscard]] bool anyClobberInBlockRange(
        TypeInterner const& interner, MirInstId loadPtr, MirBlockId blk,
        std::uint32_t lo, std::uint32_t hi,
        StrictTbaa strictTbaa, bool charTypesAliasAll) const {
        checkModule_();
        auto const it = blockClobbers_.find(blk.v);
        if (it == blockClobbers_.end()) return false;
        for (MirMemoryDef const& d : it->second) {   // ascending instIdxInBlock
            if (d.instIdxInBlock < lo) continue;
            if (d.instIdxInBlock >= hi) break;
            if (mirInstClobbersLoadPtr(mir_, interner, loadPtr, d.inst,
                                       strictTbaa, charTypesAliasAll)) {
                return true;
            }
        }
        return false;
    }

    // Q2 — any clobber of `loadPtr` STRICTLY BETWEEN `canonicalBlock` and
    // `useBlock`: the same fwd-reachable(canonical) ∩ bwd-reachable(use) region
    // MINUS both endpoints that `mirRegionBetween` walks (roots included in the
    // reach sets, endpoints excluded here; slot 0 skipped for parity with the
    // reference's `v = 1` start), restricted to clobber-bearing blocks (blocks
    // with none cannot flip the existential), each alias-tested in ascending
    // slot order via the unchanged predicate.
    [[nodiscard]] bool anyClobberBetween(
        TypeInterner const& interner, MirInstId loadPtr,
        MirBlockId canonicalBlock, MirBlockId useBlock,
        StrictTbaa strictTbaa, bool charTypesAliasAll) const {
        checkModule_();
        if (!canonicalBlock.valid() || !useBlock.valid()) return false;
        if (blockCount_ == 0) return false;
        auto const& fwd = reach_(canonicalBlock, /*forward=*/true);
        auto const& bwd = reach_(useBlock,       /*forward=*/false);
        for (std::uint32_t const slot : clobberBearingBlocks_) {
            if (slot < 1u) continue;   // parity: the reference loop starts at v=1
            if (slot == canonicalBlock.v || slot == useBlock.v) continue;
            if (!fwd[slot] || !bwd[slot]) continue;
            for (MirMemoryDef const& d : blockClobbers_.at(slot)) {
                if (mirInstClobbersLoadPtr(mir_, interner, loadPtr, d.inst,
                                           strictTbaa, charTypesAliasAll)) {
                    return true;
                }
            }
        }
        return false;
    }

    // Q3 — any clobber of `loadPtr` anywhere in `blocks` (LICM's natural-loop
    // body). Enumeration-identical to the reference whole-body scan
    // (`mirAnyMayAliasingStoreInLoop`): every instruction of every listed block
    // is considered; the non-clobbers are skipped as always-false.
    [[nodiscard]] bool anyClobberInBlocks(
        TypeInterner const& interner, MirInstId loadPtr,
        std::span<MirBlockId const> blocks,
        StrictTbaa strictTbaa, bool charTypesAliasAll) const {
        checkModule_();
        for (MirBlockId const b : blocks) {
            auto const it = blockClobbers_.find(b.v);
            if (it == blockClobbers_.end()) continue;
            for (MirMemoryDef const& d : it->second) {
                if (mirInstClobbersLoadPtr(mir_, interner, loadPtr, d.inst,
                                           strictTbaa, charTypesAliasAll)) {
                    return true;
                }
            }
        }
        return false;
    }

    // Q4 — does `blk` lie on a cycle that does NOT pass through `avoid`?
    // (an invalid `avoid` ⇒ plain self-reachability). Forward walk from `blk`'s
    // successors that never expands THROUGH `avoid`, asking whether `blk` is
    // re-reached.
    //
    // WHY THIS EXISTS: a two-program-point Load query (CSE) decomposes the
    // canonical→use region into slices, and that decomposition is complete only
    // for an ACYCLIC path. When the use sits on a canonical-free cycle, execution
    // WRAPS and the use block's TAIL runs before the NEXT execution of the use —
    // so the caller must scan that tail too. This predicate is the trigger.
    // Reachability (not `mirNaturalLoops`) is deliberate: DSS has computed goto /
    // IndirectBr, so an IRREDUCIBLE cycle yields no natural loop and any
    // loop-structure-based trigger would silently miss it.
    //
    // ★ NOT `reach_(blk, /*forward=*/true)[blk.v]` — that bitmap PRE-MARKS ITS OWN
    // ROOT (see `reach_`), so such a predicate is CONSTANT-TRUE. It would still be
    // sound, but would silently degrade Load CSE to "never CSE across any later
    // clobber" — an unmeasured optimization loss no test could observe.
    //
    // Same step-cap + fail-loud discipline as `reach_`; not memoized (called only
    // after a cheap tail scan already found a candidate clobber).
    [[nodiscard]] bool blockReachesItselfAvoiding(MirBlockId blk,
                                                  MirBlockId avoid) const {
        checkModule_();
        if (!blk.valid() || blk.v >= blockCount_) return false;
        // Degenerate query: with `avoid == blk` the walk can never expand
        // through `avoid`, so it would answer "no" even for a genuine self
        // edge. Unreachable from the CSE call site (guarded by
        // `canonicalBlock.v != B.v`), but this method is public — be explicit
        // rather than accidentally correct.
        if (avoid.valid() && avoid.v == blk.v) return false;
        // Stamp-compare scratch, NOT a per-call zero-filled vector:
        // `blockCount_` is the WHOLE MODULE's block arena (see the ctor), so a
        // fresh `vector<uint8_t>(blockCount_)` would allocate + memset hundreds
        // of KB per qualifying Load candidate on a merged amalgamation, while
        // the walk only ever touches the CURRENT function's blocks. Same
        // mutable-memo pattern as `fwdMemo_`/`bwdMemo_` below. The `== 0u`
        // arm covers both the first call and stamp wrap-around.
        if (++reachSelfStamp_ == 0u || reachSelfSeen_.size() != blockCount_) {
            reachSelfSeen_.assign(blockCount_, 0u);
            reachSelfStamp_ = 1u;
        }
        std::vector<MirBlockId> work;
        // Returns true the moment `blk` is re-reached.
        auto step = [&](MirBlockId s) -> bool {
            if (!s.valid() || s.v >= blockCount_) return false;
            if (avoid.valid() && s.v == avoid.v) return false;  // never expand through
            if (s.v == blk.v) return true;
            if (reachSelfSeen_[s.v] != reachSelfStamp_) {
                reachSelfSeen_[s.v] = reachSelfStamp_;
                work.push_back(s);
            }
            return false;
        };
        for (MirBlockId const s : mir_.blockSuccessors(blk))
            if (step(s)) return true;
        std::uint32_t const stepCap = blockCount_ * 4u + 4u;
        std::uint32_t steps = 0;
        while (!work.empty()) {
            if (++steps > stepCap) {
                std::fprintf(stderr,
                    "dss::opt::analysis::MirMemoryClobbers fatal: step-cap "
                    "exceeded in self-reach walk from #%u — malformed CFG (the "
                    "verifier should have caught this).\n", blk.v);
                std::abort();
            }
            MirBlockId const b = work.back();
            work.pop_back();
            for (MirBlockId const s : mir_.blockSuccessors(b))
                if (step(s)) return true;
        }
        return false;
    }

private:
    // Design-audit F3: the optimizer mints a fresh MirModuleId per rebuild and
    // `mir = std::move(builder).finish()` reassigns the SAME variable this
    // object's reference binds — so a use-after-finish flips the id/blockCount
    // and fails loud here instead of silently indexing a different module.
    void checkModule_() const {
        if (mir_.id().v != moduleIdV_
            || static_cast<std::uint32_t>(mir_.blockCount()) != blockCount_) {
            std::fprintf(stderr,
                "dss::opt::analysis::MirMemoryClobbers fatal: module changed "
                "under the ledger (built id=%u blocks=%u, now id=%u blocks=%zu) "
                "— use-after-finish() / stale-pass object.\n",
                moduleIdV_, blockCount_, mir_.id().v, mir_.blockCount());
            std::abort();
        }
    }

    // Lazily-memoized dense reachability bitmap from `from` (design-audit F5).
    // forward=true walks blockSuccessors (the reference fwd BFS); forward=false
    // walks the caller's preds map (the reference bwd BFS). Same per-pop
    // step-cap discipline + edge guards as `mirRegionBetween`, verbatim.
    [[nodiscard]] std::vector<std::uint8_t> const&
    reach_(MirBlockId from, bool forward) const {
        auto& memo = forward ? fwdMemo_ : bwdMemo_;
        auto const it = memo.find(from.v);
        if (it != memo.end()) return it->second;

        std::vector<std::uint8_t> reach(blockCount_, 0u);
        std::vector<MirBlockId> work;
        work.push_back(from);
        reach[from.v] = 1u;   // root included; endpoints excluded at the query
        std::uint32_t const stepCap = blockCount_ * 4u + 4u;
        std::uint32_t steps = 0;
        while (!work.empty()) {
            if (++steps > stepCap) {
                std::fprintf(stderr,
                    "dss::opt::analysis::MirMemoryClobbers fatal: step-cap "
                    "exceeded in %s walk from #%u — malformed CFG (the "
                    "verifier should have caught this).\n",
                    forward ? "forward" : "backward", from.v);
                std::abort();
            }
            MirBlockId const b = work.back();
            work.pop_back();
            if (forward) {
                for (MirBlockId const s : mir_.blockSuccessors(b)) {
                    if (!s.valid() || s.v >= blockCount_) continue;
                    if (!reach[s.v]) { reach[s.v] = 1u; work.push_back(s); }
                }
            } else {
                if (b.v >= preds_.size()) continue;
                for (MirBlockId const p : preds_[b.v]) {
                    if (!p.valid() || p.v >= blockCount_) continue;
                    if (!reach[p.v]) { reach[p.v] = 1u; work.push_back(p); }
                }
            }
        }
        return memo.emplace(from.v, std::move(reach)).first->second;
    }

    Mir const&                                  mir_;
    std::vector<std::vector<MirBlockId>> const& preds_;
    std::uint32_t const                         moduleIdV_;
    std::uint32_t const                         blockCount_;
    std::unordered_map<std::uint32_t, std::vector<MirMemoryDef>> blockClobbers_;
    std::vector<std::uint32_t>                  clobberBearingBlocks_;  // sorted
    mutable std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> fwdMemo_;
    mutable std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> bwdMemo_;
    // Stamp-compare scratch for `blockReachesItselfAvoiding` — reused across
    // calls so the walk costs O(touched blocks), not O(module blocks), per
    // query. Not a memo: the ANSWER is not cached (it depends on `avoid`),
    // only the visited-set storage is.
    mutable std::vector<std::uint32_t>          reachSelfSeen_;
    mutable std::uint32_t                       reachSelfStamp_ = 0u;
};

} // namespace dss::opt::analysis
