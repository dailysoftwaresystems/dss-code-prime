#include "mir/merge/synth_seh_funclets.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"       // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <algorithm>   // std::max
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>     // std::move
#include <vector>

namespace dss {

namespace {

using opt::passes::MirRebuildPolicy;
using opt::passes::MirFunctionRebuilder;

void emitErr(DiagnosticReporter& rep, std::string msg) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// Max SymbolId.v across every function, module global, and extern import — the
// floor for minting fresh synthetic funclet/personality symbols (mirrors
// synthesizePeStartup's maxSymbolIdV; the globals scan is load-bearing for the
// same reason — synthetic string-literal globals hold the highest ids).
[[nodiscard]] std::uint32_t
maxSymbolIdV(Mir const& mir, std::vector<ExternImport> const& externs) {
    std::uint32_t maxV = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        maxV = std::max(maxV, mir.funcSymbol(mir.funcAt(i)).v);
    }
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        maxV = std::max(maxV, mir.globalSymbol(mir.globalAt(i)).v);
    }
    for (auto const& e : externs) maxV = std::max(maxV, e.symbol.v);
    return maxV;
}

// One collected `__try` region, resolved to concrete blocks + the minted funclet
// symbol. `filterBB` is the single block ending in SehFilterReturn (c115 lowers the
// filter EXPRESSION to one block). `bodyBlocks` is the guarded region's block set in
// the CONTIGUOUS layout order the parent rebuild imposes (entry first); `endBB` is
// its LAST block (the scope [Begin,End) end resolves to one-past it at link time).
struct Region {
    MirFuncId  parentFn{};
    SymbolId   parentSym{};
    std::uint32_t regionId = 0;
    MirBlockId tryBB{};        // SehTryBegin succ[0] — the guarded body's entry
    MirBlockId filterBB{};     // SehTryBegin succ[1] (== the SehFilterReturn block)
    MirBlockId handlerBB{};    // SehFilterReturn succ[0]
    MirBlockId endBB{};        // the guarded body's LAST block in the contiguous layout
    SymbolId   funcletSym{};
    std::vector<MirBlockId> bodyBlocks;  // the guarded region's blocks, layout order
};

// Compute a `__try` region's GUARDED-BODY block set: every block reachable from
// `tryBB` along forward edges WITHOUT (a) following the successors of a block that
// contains a `SehTryEnd` for this region (those lead OUT of the guarded body to the
// join continuation) and (b) entering the filter/handler blocks (they run post-fault,
// not in the guarded region). This bounds arbitrary internal CFG shapes — loops,
// nested conditionals — exactly (the loop back-edges stay in-region; the fall-through
// exit at SehTryEnd is the region's only forward exit). Returned in a DETERMINISTIC
// order (the block's index in the function's block list) so the layout is stable.
[[nodiscard]] std::vector<MirBlockId>
computeGuardedBodyBlocks(Mir const& mir, MirFuncId fn, MirBlockId tryBB,
                         MirBlockId filterBB, MirBlockId handlerBB,
                         std::uint32_t regionId) {
    std::unordered_set<std::uint32_t> inRegion;
    std::vector<MirBlockId> worklist{tryBB};
    inRegion.insert(tryBB.v);
    while (!worklist.empty()) {
        MirBlockId const b = worklist.back();
        worklist.pop_back();
        // Does this block hold THIS region's SehTryEnd? If so it is the guarded
        // body's fall-through exit — include it, but do NOT follow its successors
        // (they leave the region).
        bool holdsTryEnd = false;
        std::uint32_t const n = mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (mir.instOpcode(id) == MirOpcode::SehTryEnd
                && mir.instPayload(id) == regionId) {
                holdsTryEnd = true;
                break;
            }
        }
        if (holdsTryEnd) continue;
        for (MirBlockId const s : mir.blockSuccessors(b)) {
            if (s.v == filterBB.v || s.v == handlerBB.v) continue;  // out of region
            if (inRegion.insert(s.v).second) worklist.push_back(s);
        }
    }
    // Emit in function-block-list order for determinism.
    std::vector<MirBlockId> ordered;
    ordered.reserve(inRegion.size());
    std::uint32_t const nb = mir.funcBlockCount(fn);
    for (std::uint32_t i = 0; i < nb; ++i) {
        MirBlockId const b = mir.funcBlockAt(fn, i);
        if (inRegion.contains(b.v)) ordered.push_back(b);
    }
    return ordered;
}

// A verbatim clone policy (synthesizePeStartup's IdentityClonePolicy) — every
// function that is NOT a SEH parent is re-added unchanged. (Not `final`:
// SehParentPolicy specializes it, keeping the all-blocks `selectBlocks`.)
class IdentityClonePolicy : public MirRebuildPolicy {
public:
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        std::vector<MirBlockId> blocks;
        std::uint32_t const n = src.funcBlockCount(fn);
        blocks.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) blocks.push_back(src.funcBlockAt(fn, i));
        return blocks;
    }
};

// The SEH-parent rebuild policy. For the parent function it keeps EVERY block, but
// reduces each region's `filterBB` to the H2-fiction stub `[Const i32 0;
// SehFilterReturn(const, id) → handlerBB]`: the original filter value-insts are
// dropped (`shouldEmit`=false), a fresh Const is injected right before the
// terminator (`onBlockBeforeTerminator`), and the SehFilterReturn is re-emitted
// referencing that Const (`tryRewriteTerminator`). This keeps the
// SehFilterReturn→handlerBB CFG edge (handlerBB forward-reachable + single-pred)
// while removing every SehException* read from the parent — they live only in the
// funclet. Every non-filter block is copied verbatim.
class SehParentPolicy final : public IdentityClonePolicy {
public:
    SehParentPolicy(std::vector<Region> const& regions, MirFuncId parentFn,
                    TypeId i32Ty)
        : i32Ty_(i32Ty) {
        for (auto const& r : regions) {
            if (r.parentFn.v != parentFn.v) continue;
            filterBlocks_.insert(r.filterBB.v);
            handlerByFilter_.emplace(r.filterBB.v, r.handlerBB);
            regionByFilter_.emplace(r.filterBB.v, r.regionId);
            regionBodies_.push_back(&r.bodyBlocks);
        }
    }

    // c116b (D-WIN64-SEH-FUNCLETS): impose REGION-CONTIGUITY on the block layout —
    // each `__try` guarded body's blocks must occupy a single contiguous PC range so
    // its scope-table [Begin,End) covers exactly the region (no non-region block
    // interleaved). The optimizer's RPO block order can interleave the join/handler
    // between body blocks (empirically observed), so we relay out: walk the source
    // block order, and the FIRST time a region's entry (tryBB) is reached, emit that
    // region's whole body contiguously (in its precomputed order); every already-
    // emitted region body block is then skipped. The entry block stays index 0 (it
    // is never inside a guarded body — a __try cannot start at function entry in C:
    // the CRT/setup precedes it), so alloca scan-order (entry-only, c69) is preserved
    // ⇒ H1 slot-ids stay stable. Non-region blocks keep their relative order.
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        // Map each region-body block to its owning region's ordered body list.
        std::unordered_map<std::uint32_t, std::vector<MirBlockId> const*> bodyOf;
        for (auto const* body : regionBodies_) {
            for (MirBlockId const b : *body) bodyOf[b.v] = body;
        }
        std::vector<MirBlockId> order;
        std::unordered_set<std::uint32_t> emitted;
        std::uint32_t const n = src.funcBlockCount(fn);
        order.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            MirBlockId const b = src.funcBlockAt(fn, i);
            if (emitted.contains(b.v)) continue;   // already emitted as part of a body
            auto it = bodyOf.find(b.v);
            if (it == bodyOf.end()) {
                order.push_back(b);
                emitted.insert(b.v);
                continue;
            }
            // First block of a region body reached — emit the WHOLE body contiguously
            // in its precomputed (deterministic) order.
            for (MirBlockId const rb : *it->second) {
                if (emitted.insert(rb.v).second) order.push_back(rb);
            }
        }
        return order;
    }

    [[nodiscard]] bool shouldEmit(MirInstId oldId) override {
        // In a filterBB, drop every original non-terminator inst (the filter
        // expr, incl. SehExceptionCode/Info). `src_` is not visible here, so the
        // caller records the filterBB's non-terminator inst ids for us to test.
        return !droppedInsts_.contains(oldId.v);
    }

    void onBlockBeforeTerminator(
        MirBlockId oldB, MirBlockId /*newB*/, MirBuilder& dst,
        std::unordered_map<std::uint32_t, MirInstId>& rewrite,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) override {
        if (!filterBlocks_.contains(oldB.v)) return;
        // Inject the stub's Const i32 0 (the SehFilterReturn operand — a pure
        // marker value, never used at runtime). Record it so tryRewriteTerminator
        // can reference it. Keyed by the filter block id (one stub per filterBB).
        MirLiteralValue zero;
        zero.value = std::int64_t{0};
        zero.core  = TypeKind::I32;
        stubConstByFilter_[oldB.v] = dst.addConst(std::move(zero), i32Ty_);
        (void)rewrite;
    }

    [[nodiscard]] std::optional<MirInstId>
    tryRewriteTerminator(MirOpcode op, MirInstId /*oldId*/, MirBuilder& dst,
                         std::unordered_map<std::uint32_t, MirInstId> const& /*rewrite*/,
                         std::unordered_map<std::uint32_t, MirBlockId> const& blockMap)
        override {
        // Only the filterBB's terminator (a SehFilterReturn) is rewritten; every
        // other terminator takes the standard clone arm.
        if (op != MirOpcode::SehFilterReturn) return std::nullopt;
        if (curFilterBB_.v == 0) return std::nullopt;  // not a filterBB
        auto const cIt = stubConstByFilter_.find(curFilterBB_.v);
        auto const hIt = handlerByFilter_.find(curFilterBB_.v);
        auto const rIt = regionByFilter_.find(curFilterBB_.v);
        if (cIt == stubConstByFilter_.end() || hIt == handlerByFilter_.end()
            || rIt == regionByFilter_.end()) {
            return std::nullopt;
        }
        auto const hbIt = blockMap.find(hIt->second.v);
        if (hbIt == blockMap.end()) return std::nullopt;
        return dst.addSehFilterReturn(cIt->second, hbIt->second, rIt->second);
    }

    // The rebuilder walks blocks in order; before each filterBB's terminator we
    // must know which block we're in (tryRewriteTerminator gets only the opcode).
    // `selectBlocks` is called once, so we track the "current" filterBB by having
    // the caller pre-register each filterBB's dropped insts and set curFilterBB_
    // via the onBlockBegin hook.
    void onBlockBegin(MirBlockId oldB, MirBlockId /*newB*/, MirBuilder& /*dst*/,
                      std::unordered_map<std::uint32_t, MirInstId>& /*rewrite*/,
                      std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/)
        override {
        curFilterBB_ = filterBlocks_.contains(oldB.v) ? oldB : MirBlockId{};
    }

    void registerDroppedInst(MirInstId id) { droppedInsts_.insert(id.v); }

private:
    TypeId                            i32Ty_;
    std::unordered_set<std::uint32_t> filterBlocks_;
    std::unordered_set<std::uint32_t> droppedInsts_;
    std::unordered_map<std::uint32_t, MirBlockId> handlerByFilter_;
    std::unordered_map<std::uint32_t, std::uint32_t> regionByFilter_;
    std::unordered_map<std::uint32_t, MirInstId>  stubConstByFilter_;
    MirBlockId                        curFilterBB_{};
    // c116b: each guarded region's ordered body-block list (borrowed from the
    // Region vector, which outlives this policy) — drives the contiguity relayout.
    std::vector<std::vector<MirBlockId> const*> regionBodies_;
};

// Build a parent function's ALLOCA scan-order slot-id map: each Alloca inst id → its
// 0-based index in the function's alloca scan order (function-block order × in-block
// order). H1 (D-WIN64-SEH-FUNCLETS) uses this to resolve a filter's parent-local
// reference to a stable frame slot. The scan order MUST match lir_callconv's
// `functionLocalAllocaPayloads` (same block-then-inst walk) — and since c69 hoists
// EVERY alloca into the entry block, the index is invariant under the SehParentPolicy
// block reorder (which never moves the entry block). NOTE: computed on the ORIGINAL
// (pre-rebuild) `mir`; the funclet body clone reads original filterBB operands, so
// the original alloca ids are the right keys.
[[nodiscard]] std::unordered_map<std::uint32_t, std::uint32_t>
parentAllocaSlotIds(Mir const& mir, MirFuncId fn) {
    std::unordered_map<std::uint32_t, std::uint32_t> slotIds;
    std::uint32_t idx = 0;
    std::uint32_t const nb = mir.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        std::uint32_t const ni = mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < ni; ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (mir.instOpcode(id) == MirOpcode::Alloca) {
                slotIds[id.v] = idx++;
            }
        }
    }
    return slotIds;
}

// Emit the filter FUNCLET body directly into `builder` (the current open function),
// cloning `filterBB`'s non-terminator insts with the SEH rewrites. Returns false
// (reported) on an unsupported inst. `exPtrArg` is the funclet's arg0
// (EXCEPTION_POINTERS*, ms_x64 rcx); `establisherArg` is arg1 (the establisher-frame
// base — the parent's post-prologue SP at fault time). `allocaSlotId` maps each
// PARENT alloca inst id → its 0-based scan-order slot index (H1). `pVoidTy` =
// ptr<void>; `u32Ty` = u32.
//
// c116b H1 (D-WIN64-SEH-FUNCLETS): the filter reads parent locals (sqlite's `pWal`).
// Because mem2reg skips SEH functions, a parent local is an ALLOCA in the parent's
// entry block and the filter references it as `Load [parentAlloca]`. The alloca's
// value id is defined OUTSIDE filterBB, so on first use we materialize
// `RecoverParentFrameSlot(establisher, slotId)` (a generic frame-slot address off
// the establisher base) and map the alloca to it — then `Load [recovered]` reads the
// parent local from the parent frame. Any OTHER out-of-block operand (not a parent
// alloca) is unrecoverable → fail loud.
[[nodiscard]] bool emitFilterFuncletBody(
    Mir const& mir, MirBuilder& builder, MirBlockId filterBB, MirInstId exPtrArg,
    MirInstId establisherArg,
    std::unordered_map<std::uint32_t, std::uint32_t> const& allocaSlotId,
    TypeId pVoidTy, TypeId u32Ty, TypeId i32Ty, TypeId u64Ty,
    DiagnosticReporter& reporter) {
    // old value id → new value id, within the funclet.
    std::unordered_map<std::uint32_t, MirInstId> map;

    // Resolve an operand: if already mapped, return it; if it is a PARENT ALLOCA
    // (defined outside filterBB, op == Alloca), materialize a RecoverParentFrameSlot
    // for it (once) and return that; otherwise fail loud (unrecoverable out-of-block
    // reference). Returns nullopt (reported) on failure.
    auto resolveOperand = [&](MirInstId o) -> std::optional<MirInstId> {
        if (auto it = map.find(o.v); it != map.end()) return it->second;
        // Not defined in filterBB — is it a parent alloca we can recover?
        if (mir.instOpcode(o) == MirOpcode::Alloca) {
            auto slotIt = allocaSlotId.find(o.v);
            if (slotIt == allocaSlotId.end()) {
                emitErr(reporter, "synthesizeSehFunclets: the SEH filter references a "
                        "parent local whose frame slot could not be resolved "
                        "(D-WIN64-SEH-FUNCLETS)");
                return std::nullopt;
            }
            // The recovered address has the alloca's own pointer result type.
            MirInstId const recovered = builder.addInst(
                MirOpcode::RecoverParentFrameSlot,
                std::array<MirInstId, 1>{establisherArg}, mir.instType(o),
                /*payload=*/slotIt->second);
            map[o.v] = recovered;
            return recovered;
        }
        emitErr(reporter, "synthesizeSehFunclets: the SEH filter expression "
                "references a value defined outside the filter block that is not a "
                "recoverable parent local (only exception code/info + parent locals "
                "are supported) (D-WIN64-SEH-FUNCLETS)");
        return std::nullopt;
    };

    std::uint32_t const n = mir.blockInstCount(filterBB);
    for (std::uint32_t i = 0; i < n; ++i) {
        MirInstId const oldId = mir.blockInstAt(filterBB, i);
        MirOpcode const op    = mir.instOpcode(oldId);
        if (opcodeInfo(op).isTerminator) break;  // the SehFilterReturn — handled by caller
        switch (op) {
            case MirOpcode::SehExceptionInfo: {
                // → the funclet's arg0 (EXCEPTION_POINTERS*). Bitcast to the op's
                // result type if it differs (both are pointers).
                map[oldId.v] = exPtrArg;
                break;
            }
            case MirOpcode::SehExceptionCode: {
                // → *(u32*)*(void**)arg0 : EXCEPTION_POINTERS.ExceptionRecord is at
                // offset 0 (a ptr<EXCEPTION_RECORD>), and EXCEPTION_RECORD.
                // ExceptionCode is at offset 0 (u32). So two loads at offset 0.
                MirInstId const recPtr = builder.addInst(
                    MirOpcode::Load, std::array<MirInstId, 1>{exPtrArg}, pVoidTy);
                MirInstId const code = builder.addInst(
                    MirOpcode::Load, std::array<MirInstId, 1>{recPtr}, u32Ty);
                map[oldId.v] = code;
                break;
            }
            case MirOpcode::Const: {
                map[oldId.v] = builder.addConst(
                    mir.literalValue(mir.constLiteralIndex(oldId)), mir.instType(oldId));
                break;
            }
            case MirOpcode::GlobalAddr: {
                map[oldId.v] = builder.addGlobalAddr(mir.globalAddrSymbol(oldId),
                                                     mir.instType(oldId));
                break;
            }
            case MirOpcode::Arg: {
                // A funclet-internal Arg would be arg0/arg1 — but the filter EXPR
                // never references the funclet's own params (they don't exist in the
                // parent). A parent Arg used in the filter surfaces as a Load of the
                // Arg's spill alloca, handled via H1. A raw Arg here is malformed.
                emitErr(reporter, "synthesizeSehFunclets: the SEH filter references a "
                        "raw parameter value — parent params are read via their frame "
                        "slot (H1), not a funclet Arg (D-WIN64-SEH-FUNCLETS)");
                return false;
            }
            default: {
                // A general (side-effect-free or Load) filter inst: clone verbatim
                // with resolved operands (each either in-block or a recoverable
                // parent local). SehException*/Const/GlobalAddr are handled above; a
                // Call (sqlite's `sehExceptionFilter(...)`) or Load falls here.
                auto const ops = mir.instOperands(oldId);
                std::vector<MirInstId> newOps;
                newOps.reserve(ops.size());
                for (MirInstId o : ops) {
                    auto m = resolveOperand(o);
                    if (!m.has_value()) return false;   // reported
                    newOps.push_back(*m);
                }
                map[oldId.v] = builder.addInst(op, newOps, mir.instType(oldId),
                                               mir.instPayload(oldId),
                                               mir.instFlags(oldId),
                                               // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN:
                                               // carry the Alloca alignment channel
                                               // if the filter body declares a local.
                                               mir.instPayload2(oldId));
                break;
            }
        }
    }
    // The terminator: SehFilterReturn(fval) → Return(fval).
    MirInstId const term = mir.blockTerminator(filterBB);
    if (mir.instOpcode(term) != MirOpcode::SehFilterReturn) {
        emitErr(reporter, "synthesizeSehFunclets: filter block does not end in "
                "SehFilterReturn — the SEH filter EXPRESSION must be a single basic "
                "block (D-WIN64-SEH-FUNCLETS)");
        return false;
    }
    auto const termOps = mir.instOperands(term);
    if (termOps.size() != 1) {
        emitErr(reporter, "synthesizeSehFunclets: malformed SehFilterReturn");
        return false;
    }
    auto fvalNew = resolveOperand(termOps[0]);
    if (!fvalNew.has_value()) return false;   // reported
    (void)i32Ty;
    (void)u64Ty;
    builder.addReturn(*fvalNew);
    return true;
}

} // namespace

bool synthesizeSehFunclets(Mir&                        mir,
                           TypeInterner&               interner,
                           std::vector<ExternImport>&  externImports,
                           std::vector<MirSehScope>&   outScopes,
                           DiagnosticReporter&         reporter) {
    // (0) Fast presence scan — no SehTryBegin anywhere ⇒ clean no-op.
    bool anySeh = false;
    std::size_t const nf0 = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf0 && !anySeh; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            if (mir.blockInstCount(b) == 0) continue;
            if (mir.instOpcode(mir.blockTerminator(b)) == MirOpcode::SehTryBegin) {
                anySeh = true;
                break;
            }
        }
    }
    if (!anySeh) return true;

    // (1) Collect every region + mint funclet symbols. One personality import is
    //     shared across all regions.
    std::uint32_t maxV = maxSymbolIdV(mir, externImports);
    SymbolId const personalitySym{maxV + 1};
    std::uint32_t nextSymV = maxV + 1;

    std::vector<Region> regions;
    for (std::uint32_t fi = 0; fi < nf0; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            if (mir.blockInstCount(b) == 0) continue;
            MirInstId const term = mir.blockTerminator(b);
            if (mir.instOpcode(term) != MirOpcode::SehTryBegin) continue;

            auto const succs = mir.blockSuccessors(b);
            if (succs.size() != 2) {
                emitErr(reporter, "synthesizeSehFunclets: SehTryBegin must have 2 "
                        "successors (D-WIN64-SEH-FUNCLETS)");
                return false;
            }
            Region r;
            r.parentFn  = f;
            r.parentSym = mir.funcSymbol(f);
            r.regionId  = mir.instPayload(term);
            r.tryBB     = succs[0];
            r.filterBB  = succs[1];

            // The SEH filter EXPRESSION is one basic block (c115 lowers it so, and
            // the MIR verifier enforces it): filterBB ends directly in
            // SehFilterReturn with 1 successor (the handler).
            MirInstId const fterm = mir.blockTerminator(r.filterBB);
            if (mir.instOpcode(fterm) != MirOpcode::SehFilterReturn) {
                emitErr(reporter, "synthesizeSehFunclets: the SEH filter block must "
                        "end in SehFilterReturn (D-WIN64-SEH-FUNCLETS)");
                return false;
            }
            auto const fsuccs = mir.blockSuccessors(r.filterBB);
            if (fsuccs.size() != 1) {
                emitErr(reporter, "synthesizeSehFunclets: SehFilterReturn must have "
                        "1 successor (the handler) (D-WIN64-SEH-FUNCLETS)");
                return false;
            }
            r.handlerBB = fsuccs[0];

            // c116b: compute the guarded body's block set (may be MULTI-block — a
            // loop/conditional/memcpy). The reorder in SehParentPolicy lays these out
            // CONTIGUOUSLY so the scope [Begin,End) is one PC range. Verify SehTryEnd
            // exists in the region (the fall-through exit) — else the region is
            // unbounded (a return/throw inside __try, D-CSUBSET-SEH-EARLY-EXIT).
            r.bodyBlocks = computeGuardedBodyBlocks(mir, f, r.tryBB, r.filterBB,
                                                    r.handlerBB, r.regionId);
            bool foundEnd = false;
            for (MirBlockId const bb : r.bodyBlocks) {
                std::uint32_t const bn = mir.blockInstCount(bb);
                for (std::uint32_t i = 0; i < bn; ++i) {
                    if (mir.instOpcode(mir.blockInstAt(bb, i)) == MirOpcode::SehTryEnd
                        && mir.instPayload(mir.blockInstAt(bb, i)) == r.regionId) {
                        foundEnd = true;
                        break;
                    }
                }
                if (foundEnd) break;
            }
            if (!foundEnd || r.bodyBlocks.empty()) {
                emitErr(reporter, "synthesizeSehFunclets: the guarded body has no "
                        "SehTryEnd fall-through exit (an early return/goto out of a "
                        "__try is D-CSUBSET-SEH-EARLY-EXIT) (D-WIN64-SEH-FUNCLETS)");
                return false;
            }
            // The scope PC range is [begin, end). SehParentPolicy lays the body out
            // contiguously in `bodyBlocks` order, so `endBB` = the LAST body block;
            // the pipeline computes `end` as the offset of whatever block is laid out
            // immediately after it (the first non-region block after the contiguous
            // run). `beginBlock` = tryBB (bodyBlocks[0] by construction — the entry).
            r.endBB = r.bodyBlocks.back();

            r.funcletSym = SymbolId{++nextSymV};
            regions.push_back(r);
        }
    }
    if (regions.empty()) return true;  // scan said yes but none resolved — no-op

    // Register the __C_specific_handler personality import (SEH-gated, on demand —
    // the c101 loader law; NEVER in windows.json symbols).
    {
        ExternImport pers;
        pers.symbol      = personalitySym;
        pers.mangledName = "__C_specific_handler";
        pers.libraryPath = "msvcrt.dll";
        pers.isData      = false;
        // D-LINK-EXTERN-IMPORT-REFERENCE-GATE (TF-C44): this personality import is
        // referenced by the pe UNWIND_INFO's EHANDLER handler-RVA field (the SEH scope
        // descriptor's `personalitySymbol` / `SehHandlerPatch`), NOT by a function/data
        // RELOCATION — so the linker's reloc-based reference gate cannot see the
        // reference and would DROP it as an unreferenced non-eager import (→ pe::encodeExec
        // "SEH personality symbol has no import thunk"). It is a compiler-SYNTHESIZED,
        // always-needed runtime import (minted ONLY when a SEH region resolved, and wired
        // into the unwind info by construction), so it is EXEMPT from the reference gate —
        // the same "keep regardless of reloc-referencing" contract as a descriptor eager
        // import. Red-on-disable: clear this bit → the seh_catch_* examples' pe64 compile
        // fails loud at the pe writer.
        pers.isEagerImport = true;
        externImports.push_back(std::move(pers));
    }

    // Which functions are SEH parents (need SehParentPolicy)?
    std::unordered_set<std::uint32_t> parentFns;
    for (auto const& r : regions) parentFns.insert(r.parentFn.v);

    // Types for the funclet signatures + bodies.
    TypeId const i32Ty   = interner.primitive(TypeKind::I32);
    TypeId const u32Ty   = interner.primitive(TypeKind::U32);
    TypeId const voidTy  = interner.primitive(TypeKind::Void);
    TypeId const pVoidTy = interner.pointer(voidTy);
    // int filter(void* exceptionPointers, u64 establisher) — ms_x64.
    TypeId const u64Ty   = interner.primitive(TypeKind::U64);
    std::array<TypeId, 2> const funcletParams{pVoidTy, u64Ty};
    TypeId const funcletSig =
        interner.fnSig(funcletParams, i32Ty, CallConv::CcMS64);

    // (2) Rebuild the frozen module: clone every original function (SEH parents via
    //     the stub policy), then append one funclet per region, then globals.
    //     The rebuild mints FRESH block ids in a new arena, so capture each SEH
    //     parent's old→new block map to re-key the scope records afterward.
    MirBuilder builder;
    IdentityClonePolicy identity;
    std::unordered_map<std::uint32_t, MirBlockId> oldToNewBlock;  // old.v → new block
    for (std::uint32_t fi = 0; fi < nf0; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        if (parentFns.contains(f.v)) {
            SehParentPolicy policy{regions, f, i32Ty};
            // Pre-register the dropped (filter-body) insts for this parent.
            for (auto const& r : regions) {
                if (r.parentFn.v != f.v) continue;
                std::uint32_t const cnt = mir.blockInstCount(r.filterBB);
                for (std::uint32_t i = 0; i < cnt; ++i) {
                    MirInstId const id = mir.blockInstAt(r.filterBB, i);
                    if (!opcodeInfo(mir.instOpcode(id)).isTerminator) {
                        policy.registerDroppedInst(id);
                    }
                }
            }
            MirFunctionRebuilder rb{mir, builder, policy};
            rb.rebuildFunction(f);
            for (auto const& [oldV, newB] : rb.blockMap()) oldToNewBlock[oldV] = newB;
        } else {
            MirFunctionRebuilder rb{mir, builder, identity};
            rb.rebuildFunction(f);
        }
    }

    // Append the funclets. Each is a single-block function reading arg0 (exception
    // pointers) + arg1 (establisher frame). A filter that reads a parent local (H1)
    // recovers it off arg1 via RecoverParentFrameSlot + the parent's alloca slot map.
    for (auto const& r : regions) {
        std::unordered_map<std::uint32_t, std::uint32_t> const allocaSlotId =
            parentAllocaSlotIds(mir, r.parentFn);
        (void)builder.addFunction(funcletSig, r.funcletSym, SymbolBinding::Local,
                                  SymbolVisibility::Default);
        MirBlockId const entry = builder.createBlock(StructCfMarker::EntryBlock);
        builder.beginBlock(entry);
        MirInstId const exPtr       = builder.addArg(0, pVoidTy);
        // arg1 = the establisher frame (u64) — the base RecoverParentFrameSlot reads
        // parent locals off. Materialized whether or not the filter uses it (a filter
        // over only the exception code leaves it dead — regalloc drops it).
        MirInstId const establisher = builder.addArg(1, u64Ty);
        if (!emitFilterFuncletBody(mir, builder, r.filterBB, exPtr, establisher,
                                   allocaSlotId, pVoidTy, u32Ty, i32Ty, u64Ty,
                                   reporter)) {
            return false;
        }
    }

    opt::passes::cloneGlobalsVerbatim(mir, builder);
    mir = std::move(builder).finish();

    // (3) Emit the scope records, re-keyed to the REBUILT module's block ids (the
    //     rebuild minted fresh ids in a new arena). The LIR lowering then maps each
    //     new MIR block to its LIR block; the pipeline binds byte offsets.
    outScopes.reserve(regions.size());
    for (auto const& r : regions) {
        auto beginIt = oldToNewBlock.find(r.tryBB.v);
        auto endIt   = oldToNewBlock.find(r.endBB.v);
        auto handIt  = oldToNewBlock.find(r.handlerBB.v);
        if (beginIt == oldToNewBlock.end() || endIt == oldToNewBlock.end()
            || handIt == oldToNewBlock.end()) {
            emitErr(reporter, "synthesizeSehFunclets: a SEH region's block was "
                    "dropped by the rebuild (internal invariant violation, "
                    "D-WIN64-SEH-FUNCLETS)");
            return false;
        }
        MirSehScope s;
        s.parentFuncSymbol    = r.parentSym;
        s.beginBlock          = beginIt->second;
        s.endBlock            = endIt->second;
        s.handlerBlock        = handIt->second;
        s.filterFuncletSymbol = r.funcletSym;
        s.personalitySymbol   = personalitySym;
        outScopes.push_back(s);
    }
    return true;
}

} // namespace dss
