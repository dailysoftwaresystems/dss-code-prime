#include "mir/merge/synth_seh_funclets.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"       // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "ffi/mangling/c_mangle.hpp"   // applyCMangling (per-format personality name)
#include "mir/merge/synth_symbol_floor.hpp"  // continueSymbolIdsPastImports (the rebuild continues the module's ids)
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"  // rederiveStructCfMarkers (the relayout's duty)
#include "opt/passes/mir_rebuild_helper.hpp"

#include <algorithm>   // std::max
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>     // std::move
#include <vector>

namespace dss {

namespace {

using opt::passes::MirRebuildPolicy;
using opt::passes::MirFunctionRebuilder;
// The rebuild substrate's per-function OLD→NEW remaps
// (D-PERF-OPT-REBUILD-REMAP-IS-A-HASH-MAP) — the hook parameter types below.
using opt::passes::MirBlockRemap;
using opt::passes::MirInstRemap;

void emitErr(DiagnosticReporter& rep, std::string msg) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// ⓘ THE FUNCLET AND PERSONALITY SYMBOLS COME FROM THE MODULE (`MirBuilder::mintSymbolOrAbort`,
// the one door — see mir/merge/synth_symbol_floor.hpp), as the entry-shape and
// threads-shim passes' do: past every id the module holds (its globals load-bearing —
// synthetic string-literal globals hold the highest ids) AND every id the name table the
// module was made from holds, which names whatever symbol a funclet's id lands on.

// One collected `__try` region, resolved to concrete blocks + the minted funclet
// symbol. `filterBB` is the single block ending in SehFilterReturn (c115 lowers the
// filter EXPRESSION to one block). `bodyBlocks` is the guarded region's MEMBERSHIP —
// every block of the guarded body, in the source function's block order. Where the
// body's run ENDS is not a fact about this list: it is read off the rebuilt
// function's layout, after the relayout, by `verifyRegionLayout`.
struct Region {
    MirFuncId  parentFn{};
    std::uint32_t parentIndex = 0;   // `parentFn`'s position in the module's function list
    SymbolId   parentSym{};
    std::uint32_t regionId = 0;
    MirBlockId tryBB{};        // SehTryBegin succ[0] — the guarded body's entry
    MirBlockId filterBB{};     // SehTryBegin succ[1] (== the SehFilterReturn block)
    MirBlockId handlerBB{};    // SehFilterReturn succ[0]
    SymbolId   funcletSym{};
    std::vector<MirBlockId> bodyBlocks;  // the guarded body's blocks, source order
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

// ── THE VERIFIER OF THE LAYOUT (D-MIR-NESTED-TRY-REGIONS-REACH-THE-OUTER-HANDLER) ──
//
// ★★★ A SCOPE RECORD IS TWO ADDRESSES, AND EVERYTHING BETWEEN THEM IS GUARDED.
// The dispatcher never asks which blocks the source put in the `__try`: it asks
// whether the faulting address lies in [Begin, End). So the record is right only
// if the region's MEMBERSHIP — every block of the guarded body — is laid out as
// exactly ONE RUN of the function, and that run STARTS at the body's entry
// (whose address is Begin). A member outside the run faults unguarded; a
// non-member inside it has its faults delivered to a handler that is not its
// own; a handler inside its own run is re-entered by its own fault.
//
// This reads the REBUILT function — the layout the relayout actually produced —
// and re-derives the membership from that function's own CFG rather than from
// the list the relayout was handed, so a relayout that misplaced a block and a
// rebuild that re-keyed one wrongly both show here. It answers the run — its
// LAST block (the scope's End is the address of whatever is laid out after it)
// and the layout positions of its first and last block — or nothing, after
// reporting which block is out of place.
//
// The filter stub and the handler are never members (the walk does not enter
// them), so "exactly one run of members" is also "neither lies inside the run".
struct LaidRun {
    MirBlockId    last{};
    std::uint32_t firstPos = 0;   // the body's entry, as a position in the function's block list
    std::uint32_t lastPos  = 0;   // the run's last block, likewise (inclusive)
};

[[nodiscard]] std::optional<LaidRun>
verifyRegionLayout(Mir const& mir, MirFuncId fn, MirBlockId tryBB,
                   MirBlockId filterBB, MirBlockId handlerBB,
                   std::uint32_t regionId, DiagnosticReporter& reporter) {
    std::vector<MirBlockId> const members =
        computeGuardedBodyBlocks(mir, fn, tryBB, filterBB, handlerBB, regionId);
    std::uint32_t const nb = mir.funcBlockCount(fn);
    std::uint32_t begin = nb;
    for (std::uint32_t i = 0; i < nb; ++i) {
        if (mir.funcBlockAt(fn, i).v == tryBB.v) {
            begin = i;
            break;
        }
    }
    // `members` comes back in the function's block order, so the run is exact iff
    // its k-th member is the block laid out k places after the body's entry.
    for (std::size_t k = 0; k < members.size(); ++k) {
        std::uint32_t const want = begin + static_cast<std::uint32_t>(k);
        if (want < nb && mir.funcBlockAt(fn, want).v == members[k].v) continue;
        std::uint32_t at = nb;
        for (std::uint32_t i = 0; i < nb; ++i) {
            if (mir.funcBlockAt(fn, i).v == members[k].v) {
                at = i;
                break;
            }
        }
        emitErr(reporter, std::format(
            "synthesizeSehFunclets: guarded region {} is not laid out as one run of "
            "its function starting at the body's entry — the body has {} block(s), "
            "its entry is laid out at position {}, and block #{} of the body (in "
            "layout order) sits at position {} where the run needs position {}. A "
            "scope record guards every address between two blocks, so a fault in a "
            "misplaced block would reach another region's handler or none. This is "
            "the relayout disagreeing with the region's membership, not a property "
            "of the program", regionId, members.size(), begin, k, at, want));
        return std::nullopt;
    }
    if (members.empty() || begin == nb) {
        emitErr(reporter, std::format(
            "synthesizeSehFunclets: guarded region {} has no body in the rebuilt "
            "function (internal invariant violation)", regionId));
        return std::nullopt;
    }
    return LaidRun{members.back(), begin,
                   begin + static_cast<std::uint32_t>(members.size()) - 1u};
}

// A verbatim clone policy (synthesizePeStartup's IdentityClonePolicy) — every
// function that is NOT a SEH parent is re-added unchanged. (Not `final`:
// SehParentPolicy specializes it, keeping the all-blocks `selectBlocks`.)
class IdentityClonePolicy : public MirRebuildPolicy {
public:
    // The rebuild DRIVER this policy belongs to — printed by every
    // `MirFunctionRebuilder` fatal.
    // See D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS (one line: a wrapped
    // anchor name mints a second, unregistered anchor).
    // A MIR merge step, not a `kPassNameTable` pipeline pass. `virtual`
    // by inheritance: `SehParentPolicy` re-answers it, because the two policies
    // drive the rebuilder in the SAME loop (non-parent functions vs SEH parents)
    // and an abort that named them alike would hide which arm died.
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "SynthSehFunclets";
    }

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
    // Distinct from the base's name ON PURPOSE — see the note there. Both
    // policies drive the rebuilder inside one loop, so `[pass=SynthSehParent]`
    // vs `[pass=SynthSehFunclets]` is the difference between "the SEH parent
    // rewrite died" and "the verbatim clone of an unrelated function died".
    [[nodiscard]] std::string_view passName() const noexcept override {
        return "SynthSehParent";
    }

    SehParentPolicy(std::vector<Region> const& regions, MirFuncId parentFn,
                    TypeId i32Ty)
        : i32Ty_(i32Ty) {
        for (auto const& r : regions) {
            if (r.parentFn.v != parentFn.v) continue;
            filterBlocks_.insert(r.filterBB.v);
            handlerByFilter_.emplace(r.filterBB.v, r.handlerBB);
            regionByFilter_.emplace(r.filterBB.v, r.regionId);
            regions_.push_back(&r);
        }
    }

    // c116b (D-WIN64-SEH-FUNCLETS): impose REGION-CONTIGUITY on the block layout —
    // each `__try` guarded body's blocks must occupy a single contiguous PC range so
    // its scope-table [Begin,End) covers exactly the region (no non-region block
    // interleaved). The optimizer's RPO block order can interleave the join/handler
    // between body blocks (empirically observed), so we relay out: walk the source
    // block order, and the first time a block of a region's body is reached, emit
    // that region's whole body as ONE run. The entry block stays index 0 (it
    // is never inside a guarded body — a __try cannot start at function entry in C:
    // the CRT/setup precedes it), so alloca scan-order (entry-only, c69) is preserved
    // ⇒ H1 slot-ids stay stable. Non-region blocks keep their relative order.
    //
    // ★★★ D-MIR-NESTED-TRY-REGIONS-REACH-THE-OUTER-HANDLER: THE RUN IS BUILT PER
    // NESTING LEVEL, ENTRY FIRST. A region inside another region's body is itself a
    // body that needs its own run INSIDE the outer one, and the predecessor of this
    // function did not give it one: it emitted the OUTERMOST body in source order
    // and skipped everything already emitted, so an inner body with a loop (whose
    // blocks the optimizer's order puts after the code that follows the loop) was
    // left in pieces, and its range — entry to last piece — then held blocks of the
    // outer body. A fault in those blocks belongs to the outer handler alone.
    //   * a run STARTS at its body's entry (`tryBB`): the scope's Begin is that
    //     block's address, so a body block laid out before it would be unguarded;
    //   * the rest of the body follows in source order, except that the first block
    //     met of a DIRECTLY nested region opens that region's own run right there;
    //   * the walk keeps its own stack — nesting depth is the source's to choose,
    //     and this pass must not spend a host frame per level of it.
    // `verifyRegionLayout` re-derives every region's membership from the REBUILT
    // function and refuses a layout in which any run and its membership disagree.
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        constexpr std::size_t kNone = static_cast<std::size_t>(-1);
        std::vector<std::unordered_set<std::uint32_t>> bodySet(regions_.size());
        for (std::size_t i = 0; i < regions_.size(); ++i) {
            for (MirBlockId const b : regions_[i]->bodyBlocks) bodySet[i].insert(b.v);
        }
        // The region whose run opens when block `b` is met while laying out region
        // `within` (kNone: at function level): the LARGEST body that holds `b` and
        // lies strictly inside `within` — bodies nest, so a region strictly inside
        // another has its entry among that one's blocks and fewer blocks than it.
        auto const outermostHolding = [&](std::uint32_t b, std::size_t within) {
            std::size_t best = kNone;
            for (std::size_t i = 0; i < regions_.size(); ++i) {
                if (i == within || !bodySet[i].contains(b)) continue;
                if (within != kNone
                    && (!bodySet[within].contains(regions_[i]->tryBB.v)
                        || bodySet[i].size() >= bodySet[within].size())) {
                    continue;
                }
                if (best == kNone || bodySet[i].size() > bodySet[best].size()) best = i;
            }
            return best;
        };

        std::vector<MirBlockId> order;
        std::unordered_set<std::uint32_t> emitted;
        std::uint32_t const n = src.funcBlockCount(fn);
        order.reserve(n);
        auto const emit = [&](MirBlockId b) {
            if (emitted.insert(b.v).second) order.push_back(b);
        };
        struct Frame {
            std::size_t region;
            std::size_t next;   // into that region's `bodyBlocks`
        };
        std::vector<Frame> open;
        auto const openRun = [&](std::size_t region) {
            emit(regions_[region]->tryBB);   // the run starts at the body's entry
            open.push_back(Frame{region, 0});
        };
        for (std::uint32_t i = 0; i < n; ++i) {
            MirBlockId const b = src.funcBlockAt(fn, i);
            if (emitted.contains(b.v)) continue;   // already emitted as part of a run
            std::size_t const top = outermostHolding(b.v, kNone);
            if (top == kNone) {
                emit(b);
                continue;
            }
            openRun(top);
            while (!open.empty()) {
                std::size_t const region = open.back().region;
                auto const& body = regions_[region]->bodyBlocks;
                if (open.back().next == body.size()) {
                    open.pop_back();
                    continue;
                }
                MirBlockId const rb = body[open.back().next++];
                if (emitted.contains(rb.v)) continue;
                std::size_t const child = outermostHolding(rb.v, region);
                if (child == kNone) emit(rb);
                else                openRun(child);
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
        MirInstRemap& rewrite,
        MirBlockRemap const& /*blockMap*/) override {
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
                         MirInstRemap const& /*rewrite*/,
                         MirBlockRemap const& blockMap)
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
        MirBlockId const* const handlerNew = blockMap.find(hIt->second.v);
        if (handlerNew == nullptr) return std::nullopt;
        return dst.addSehFilterReturn(cIt->second, *handlerNew, rIt->second);
    }

    // The rebuilder walks blocks in order; before each filterBB's terminator we
    // must know which block we're in (tryRewriteTerminator gets only the opcode).
    // `selectBlocks` is called once, so we track the "current" filterBB by having
    // the caller pre-register each filterBB's dropped insts and set curFilterBB_
    // via the onBlockBegin hook.
    void onBlockBegin(MirBlockId oldB, MirBlockId /*newB*/, MirBuilder& /*dst*/,
                      MirInstRemap& /*rewrite*/,
                      MirBlockRemap const& /*blockMap*/)
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
    // c116b: this parent's regions (borrowed from the Region vector, which
    // outlives this policy) — their memberships drive the contiguity relayout.
    std::vector<Region const*>        regions_;
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

// The opcodes whose result is a function of THEIR OPERANDS AND OF NOTHING ELSE:
// not of memory at the moment they run, not of the frame, the arguments or the
// blocks of the function they sit in, and with no effect of their own. Such an
// instruction computes the same value wherever it is placed, which is what lets
// a filter funclet clone one the optimizer moved out of the filter block (see
// `resolveOperand` in `emitFilterFuncletBody`).
//
// ⚠ AN ALLOW-LIST, DELIBERATELY. The complement — "everything without side
// effects that is not a load" — is shorter and rots the wrong way: a new opcode
// that reads the frame (the three `va_start` area addresses, a by-value stack
// parameter, an indirect-result read all exist already) would be cloned into the
// funclet without a word and read the FUNCLET's frame. Here a new opcode is
// refused by name until someone decides it belongs.
[[nodiscard]] constexpr bool isOperandOnlyValue(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::Const:
        case MirOpcode::GlobalAddr:
        // integer arithmetic
        case MirOpcode::Add:  case MirOpcode::Sub:  case MirOpcode::Mul:
        case MirOpcode::SDiv: case MirOpcode::UDiv: case MirOpcode::SMod:
        case MirOpcode::UMod: case MirOpcode::Neg:  case MirOpcode::UMulH:
        // floating arithmetic
        case MirOpcode::FAdd: case MirOpcode::FSub: case MirOpcode::FMul:
        case MirOpcode::FDiv: case MirOpcode::FNeg:
        // bitwise
        case MirOpcode::And:  case MirOpcode::Or:   case MirOpcode::Xor:
        case MirOpcode::Shl:  case MirOpcode::LShr: case MirOpcode::AShr:
        case MirOpcode::Not:  case MirOpcode::Popcount: case MirOpcode::Clz:
        case MirOpcode::Ctz:  case MirOpcode::Bswap:
        // comparisons
        case MirOpcode::ICmpEq:  case MirOpcode::ICmpNe:
        case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
        case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge:
        case MirOpcode::ICmpUlt: case MirOpcode::ICmpUle:
        case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge:
        case MirOpcode::FCmpOeq: case MirOpcode::FCmpOne:
        case MirOpcode::FCmpOlt: case MirOpcode::FCmpOle:
        case MirOpcode::FCmpOgt: case MirOpcode::FCmpOge:
        case MirOpcode::FCmpUeq: case MirOpcode::FCmpUne:
        case MirOpcode::FCmpUlt: case MirOpcode::FCmpUle:
        case MirOpcode::FCmpUgt: case MirOpcode::FCmpUge:
        // address and aggregate arithmetic
        case MirOpcode::Gep:
        case MirOpcode::ExtractValue: case MirOpcode::InsertValue:
        // conversions
        case MirOpcode::Trunc:   case MirOpcode::SExt:    case MirOpcode::ZExt:
        case MirOpcode::FPTrunc: case MirOpcode::FPExt:   case MirOpcode::Bitcast:
        case MirOpcode::IntToPtr: case MirOpcode::PtrToInt:
        case MirOpcode::FPToSI:  case MirOpcode::FPToUI:
        case MirOpcode::SIToFP:  case MirOpcode::UIToFP:
        // vectors
        case MirOpcode::VAdd: case MirOpcode::VSub: case MirOpcode::VMul:
        case MirOpcode::VShuffle: case MirOpcode::VExtract: case MirOpcode::VInsert:
            return true;
        default:
            return false;
    }
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

    // Resolve an operand: if already mapped, return it; otherwise it is defined
    // OUTSIDE filterBB and the funclet has to obtain it some other way. Returns
    // nullopt (reported) when it cannot.
    //
    // ★★★ D-MIR-TRY-FILTER-SHARED-PURE-VALUE-REFUSED-IN-RELEASE: WHAT A FILTER
    // FUNCLET CAN REACH. It is a function of its own, entered by the dispatcher
    // DURING THE SEARCH — on top of the faulting frame, before anything is
    // unwound — with two arguments: the exception pointers and the parent's
    // frame. It keeps none of the parent's registers. So a value of the parent
    // reaches it in exactly two ways:
    //   * a PARENT FRAME SLOT (`Alloca`), read through the establisher frame
    //     (`RecoverParentFrameSlot`) — which is why HIR→MIR forces every symbol a
    //     filter names into the frame;
    //   * a value the funclet can COMPUTE AGAIN: an instruction whose result is a
    //     function of its operands and of nothing else (`isOperandOnlyValue`),
    //     its operands reached the same two ways. The optimizer creates these:
    //     CSE gives a filter's `GlobalAddr g` or `a + 4` the dominating copy
    //     computed before the region, and LICM hoists one out of a loop the
    //     region sits in. ✔MEASURED 2026-10-08: a filter reading a global the
    //     function also wrote before the region compiled at baseline and was
    //     REFUSED in release — this lambda copied `Const` and `GlobalAddr` when
    //     they were defined inside the filter block and refused the same
    //     instruction defined outside it. Cloning the cone puts the expression
    //     back where the source wrote it.
    // Everything else — an argument, a load, a call's result, a block's address —
    // belongs to the parent's registers, memory at an earlier moment, or blocks,
    // and stays a refusal.
    // The cone is walked with a work stack: its depth is the expression's.
    auto resolveOperand = [&](MirInstId o) -> std::optional<MirInstId> {
        if (auto it = map.find(o.v); it != map.end()) return it->second;
        std::vector<MirInstId> pending{o};
        while (!pending.empty()) {
            MirInstId const cur = pending.back();
            if (map.contains(cur.v)) {
                pending.pop_back();
                continue;
            }
            MirOpcode const op = mir.instOpcode(cur);
            if (op == MirOpcode::Alloca) {
                auto slotIt = allocaSlotId.find(cur.v);
                if (slotIt == allocaSlotId.end()) {
                    emitErr(reporter, "synthesizeSehFunclets: the SEH filter references a "
                            "parent local whose frame slot could not be resolved "
                            "(D-WIN64-SEH-FUNCLETS)");
                    return std::nullopt;
                }
                // The recovered address has the alloca's own pointer result type.
                map[cur.v] = builder.addInst(
                    MirOpcode::RecoverParentFrameSlot,
                    std::array<MirInstId, 1>{establisherArg}, mir.instType(cur),
                    /*payload=*/slotIt->second);
                pending.pop_back();
                continue;
            }
            if (!isOperandOnlyValue(op)) {
                emitErr(reporter, std::format(
                        "synthesizeSehFunclets: the SEH filter expression "
                        "references a value defined outside the filter block that is not a "
                        "recoverable parent local (only exception code/info + parent locals "
                        "are supported) (D-WIN64-SEH-FUNCLETS) — the value is a '{}', which a "
                        "filter funclet can neither read from the parent's frame nor "
                        "compute again from its operands", opcodeInfo(op).mnemonic));
                return std::nullopt;
            }
            // Operands first; a non-phi value's operands are defined before it,
            // so the cone is a DAG and this terminates.
            bool waiting = false;
            for (MirInstId const x : mir.instOperands(cur)) {
                if (!map.contains(x.v)) {
                    pending.push_back(x);
                    waiting = true;
                }
            }
            if (waiting) continue;
            if (op == MirOpcode::Const) {
                map[cur.v] = builder.addConst(
                    mir.literalValue(mir.constLiteralIndex(cur)), mir.instType(cur));
            } else if (op == MirOpcode::GlobalAddr) {
                map[cur.v] = builder.addGlobalAddr(mir.globalAddrSymbol(cur),
                                                   mir.instType(cur));
            } else {
                std::vector<MirInstId> newOps;
                for (MirInstId const x : mir.instOperands(cur)) newOps.push_back(map.at(x.v));
                map[cur.v] = builder.addInst(op, newOps, mir.instType(cur),
                                             mir.instPayload(cur), mir.instFlags(cur),
                                             mir.instPayload2(cur));
            }
            pending.pop_back();
        }
        return map.at(o.v);
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
            case MirOpcode::BlockAddress: {
                // ★ THIS CLONER IS THE SIXTH VERBATIM-COPY SITE, AND IT WAS THE ONE
                // PROTECTED BY NOTHING. `&&label` inside a `__except(...)` filter
                // expression (`__except(f(&&L))`) reached the `default:` arm below,
                // which forwards `mir.instPayload(oldId)` — a block id in the PARENT
                // function — into a funclet whose blocks are entirely different ones.
                // The address emitted would name whatever block happens to hold that
                // ordinal in the funclet, or none. `MirBuilder::addInst` now REFUSES
                // the opcode, so deleting this arm aborts rather than miscompiling;
                // this arm exists so the answer is a REPORTED refusal instead, because
                // the construct is valid C that a user can write and a compiler must
                // not abort on.
                //
                // A re-map is not available and would not be meaningful: the funclet
                // is a SEPARATE function, so a parent block has no counterpart in it,
                // and taking its address would need the parent's own block symbol
                // threaded through the funclet's relocations. Refuse, loud and
                // specific, exactly as the Arg arm above does for its own shape.
                emitErr(reporter, "synthesizeSehFunclets: the SEH filter expression "
                        "takes the address of a label (`&&label`) — the filter is "
                        "cloned into a SEPARATE funclet function whose blocks are not "
                        "the parent's, so a parent block address cannot be carried "
                        "(D-WIN64-SEH-FUNCLETS)");
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

bool synthesizeSehFunclets(Mir&                                  mir,
                           TypeInterner&                         interner,
                           std::vector<ExternImport>&            externImports,
                           std::optional<SehPersonality> const&  sehPersonality,
                           CSymbolDecorationScheme               scheme,
                           std::string_view                      formatName,
                           std::vector<MirSehScope>&             outScopes,
                           DiagnosticReporter&                   reporter) {
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
    //     shared across all regions. The symbols are minted from the module the
    //     rebuild in (2) fills, so its builder opens here: it continues the
    //     source's symbol ids. (A return before (2) drops the builder and the ids
    //     it minted with it; `mir` is untouched.)
    constexpr char const* kMinter = "synthesizeSehFunclets";
    MirBuilder builder;
    continueSymbolIdsPastImports(builder, mir, externImports);
    SymbolId const personalitySym = builder.mintSymbolOrAbort(kMinter);

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
            r.parentIndex = fi;
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
                // Anchored: D-CSUBSET-SEH-EARLY-EXIT (a return or goto out of a `__try`).
                emitErr(reporter, "synthesizeSehFunclets: the guarded body has no "
                        "SehTryEnd fall-through exit (a return or goto that leaves a "
                        "__try block is not supported: leave it through its end) "
                        "(D-WIN64-SEH-FUNCLETS)");
                return false;
            }
            // The scope PC range is [begin, end). SehParentPolicy lays the body out
            // as one run that starts at `tryBB`; the pipeline computes `end` as the
            // offset of whatever block is laid out immediately after the run's last
            // block, which `verifyRegionLayout` reads off the rebuilt function.

            r.funcletSym = builder.mintSymbolOrAbort(kMinter);
            regions.push_back(r);
        }
    }
    if (regions.empty()) return true;  // scan said yes but none resolved — no-op

    // ── Register the FORMAT-DECLARED personality import ────────────────────
    //
    // SEH-gated + on demand (the c101 loader law; NEVER in windows.json symbols).
    //
    // ★★ UCRT-P4: THE TWO LITERALS THAT USED TO BE HERE ARE GONE. This block read
    // `pers.mangledName = "__C_specific_handler"` and `pers.libraryPath =
    // "msvcrt.dll"` — a Microsoft-extension routine name and a legacy CRT image
    // spelled inside `src/mir`, on a pass that runs for EVERY format. Nothing
    // could override them and no test could see them, which is exactly how the
    // second one survived the whole pe CRT migration unnoticed.
    //
    // The REFUSAL below is placed HERE and not at the top of the function on
    // purpose: the fast presence scan has already returned for every module
    // without a `__try`, so a format that declares no personality stays fully
    // usable for every program that does not use SEH. The gate fires only when a
    // guarded region actually resolved and therefore actually needs a handler.
    if (!sehPersonality.has_value()) {
        // Anchored: D-FFI-PE-CRT-UCRT-MIGRATION (the personality routine's image).
        emitErr(reporter, std::format(
                    "synthesizeSehFunclets: {} SEH region(s) resolved but object "
                    "format '{}' declares NO `sehPersonality` block, so there is "
                    "no unwinder-personality routine to name in the emitted "
                    "unwind info and no image to import it from. `__try`/"
                    "`__except` is a Microsoft extension, not C23 — a format "
                    "realizes it only by DECLARING its personality (routine + "
                    "runtimeLibraries role). Build this translation unit for a "
                    "format that declares one. ⚠ DO NOT reflexively 'just add "
                    "the block' to silence this: on a format whose writer emits "
                    "no unwind info, declaring a personality turns this LOUD, "
                    "CORRECT refusal into a SILENT MISCOMPILE. ✔MEASURED on the "
                    "pe64 relocatable-obj and staticlib arms — with the block "
                    "declared they compile rc=0 and the artifact carries `.text` "
                    "ONLY: no `.pdata`, no `.xdata`, filter funclets and scope "
                    "table dropped, so `__except` can never run. Add the block "
                    "ONLY to a format whose writer actually emits unwind info "
                    "(the pe64 exec and dll arms do; both were witnessed by "
                    "RUNNING a guarded division-by-zero through to its "
                    "`__except`). See D-LK-PE-OBJ-ARM-CARRIES-NO-UNWIND-INFO. "
                    "(D-WIN64-SEH-FUNCLETS.)",
                    regions.size(), formatName));
        return false;
    }
    {
        ExternImport pers;
        pers.symbol      = personalitySym;
        // C-mangled for the active format through the SAME `applyCMangling` the
        // FFI ingest uses. The old literal was undecorated — correct on pe
        // (`scheme: none`) and silently wrong the instant a `leading-underscore`
        // format declared a personality.
        pers.mangledName =
            dss::ffi::applyCMangling(sehPersonality->mangledName, scheme);
        pers.libraryPath = sehPersonality->libraryPath;
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
    //     (`builder` was opened in (1), where the symbols were minted from it.)
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
            // The rebuild's block map is a dense per-function remap, not an
            // iterable container — so enumerate the KEYS from the source
            // function (`SehParentPolicy::selectBlocks` only ever reorders this
            // function's own blocks, never adds a foreign one) and query each.
            // Same pair set as the old `for (auto const& [oldV, newB] : …)`.
            std::uint32_t const nb = mir.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                MirBlockId const oldB = mir.funcBlockAt(f, bi);
                if (MirBlockId const* const newB = rb.blockMap().find(oldB.v)) {
                    oldToNewBlock[oldB.v] = *newB;
                }
            }
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

    // Canonicalize StructCfMarkers module-wide from the CFG — the same call, at
    // the same point, as `realizeEntryShape` (in `synth_pe_startup.cpp`, whose
    // FILE name is all that survives of the old `synthesizePeStartup`, kept so
    // `git log --follow` stays intact), `synthesizeStdioShim`,
    // `synthesizeThreadsShim` and `mergeCuMirs`. This pass was the only module
    // rebuild in `src/mir/merge/` that did not make it
    // ([[D-MIR-SYNTH-PASSES-UNVERIFIED-ON-SINGLE-CU-PATH]]).
    //
    // ★ WHY IT IS NEEDED HERE, WHICH IS NOT THE REASON THE SIBLINGS NEED IT.
    // They synthesize MULTI-BLOCK bodies whose blocks are created with default
    // markers. This pass creates almost nothing — it REORDERS, laying each
    // guarded region's body out contiguously, and `MirFunctionRebuilder` copies
    // every source block's stored marker onto its new block. A reorder looks
    // harmless because the derivation is a function of the CFG, and ✔MEASURED
    // (cycle P63) it IS harmless for the whole `__try` corpus and for a simple
    // diamond region. But rules 4 and 5 of the derivation iterate in FUNCTION
    // BLOCK ORDER and claim FIRST-CLAIM-WINS, so a block that two CondBr heads
    // would label differently is decided by which head comes first — and moving
    // a region's exit head in front of a non-region head is precisely what this
    // relayout does. `SynthSehFunclets.RelayoutLeavesStructCfMarkersCanonical`
    // builds that shape: without this line it ships `IfThen` where the verifier
    // derives `IfElse`.
    //
    // ⚠ AND THE CONSEQUENCE CHANGED IN THE SAME CYCLE. A stale marker used to be
    // an invisible inconsistency, because nothing verified behind this pass. The
    // post-synthesis verify now runs at BOTH driver seams AFTER it, so the same
    // stale marker is a hard compile REFUSAL on a user's `__try` program. The
    // fix belongs here, in the pass that moved the blocks — never in the
    // verifier, which would delete the honesty check to hide the defect.
    //
    // Cheap where it does not apply: the fast presence scan has already returned
    // for every module without a `__try`, so no non-SEH build reaches this.
    rederiveStructCfMarkers(mir);

    // (3) Emit the scope records, re-keyed to the REBUILT module's block ids (the
    //     rebuild minted fresh ids in a new arena). The LIR lowering then maps each
    //     new MIR block to its LIR block; the pipeline binds byte offsets.
    //
    // ★ EVERY RUN IS VERIFIED ON THE REBUILT FUNCTION BEFORE A RECORD NAMES IT
    //   (`verifyRegionLayout`): a record is a pair of addresses, and what the
    //   dispatcher will treat as guarded is whatever lies between them.
    struct Laid {
        Region const* region = nullptr;
        MirBlockId    begin{};
        MirBlockId    last{};
        MirBlockId    handler{};
        std::uint32_t firstPos = 0;
        std::uint32_t lastPos  = 0;
    };
    std::vector<Laid> laid;
    laid.reserve(regions.size());
    for (std::size_t i = 0; i < regions.size(); ++i) {
        Region const& r = regions[i];
        auto beginIt  = oldToNewBlock.find(r.tryBB.v);
        auto filterIt = oldToNewBlock.find(r.filterBB.v);
        auto handIt   = oldToNewBlock.find(r.handlerBB.v);
        if (beginIt == oldToNewBlock.end() || filterIt == oldToNewBlock.end()
            || handIt == oldToNewBlock.end()) {
            emitErr(reporter, "synthesizeSehFunclets: a SEH region's block was "
                    "dropped by the rebuild (internal invariant violation, "
                    "D-WIN64-SEH-FUNCLETS)");
            return false;
        }
        auto const run = verifyRegionLayout(
            mir, mir.funcAt(r.parentIndex), beginIt->second, filterIt->second,
            handIt->second, r.regionId, reporter);
        if (!run.has_value()) return false;   // reported
        laid.push_back(Laid{&r, beginIt->second, run->last, handIt->second,
                            run->firstPos, run->lastPos});
    }

    // ★★★ D-MIR-NESTED-TRY-REGIONS-REACH-THE-OUTER-HANDLER: THE RECORDS OF ONE
    // FUNCTION GO OUT REGION BY REGION, EACH REGION AFTER EVERY REGION INSIDE IT,
    // SIBLINGS BY ADDRESS (the header states the rule for every tier). Here a
    // region is one run, so that is: IN ASCENDING ORDER OF THE END OF THEIR RUN,
    // and of two runs that end together the one that BEGINS LATER first. The handler routine walks
    // a function's records in table order and gives the fault to the FIRST record
    // whose range holds the faulting address and whose filter accepts it — and an
    // outer region's range holds every address of the regions inside it.
    // ✔MEASURED 2026-10-08 on pe64, with the records in the order they were
    // collected (region-id order — the order the source OPENS the regions,
    // outermost first): the image's table read `outer, inner`, and a fault in the
    // inner body ran the OUTER handler. Two regions whose handlers touch only a
    // frame local, no loop: the simplest nesting there is.
    // ✔MEASURED the same day, the reference (cl 19.51 x64, /Od and /O2, a program
    // printing its own table): two deep `[+20,+32) [+20,+57)`; three deep
    // `[+20,+32) [+20,+59) [+20,+87)`; two siblings inside one parent
    // `[+20,+32) [+40,+59) [+20,+87)`. That is this order exactly: a region after
    // every region inside it, siblings by address.
    //
    // The order is a property of RUNS THAT NEST. Two runs of one function are
    // either apart or one inside the other — a `__try` is a statement, so bodies
    // nest as statements do — and the sweep below REFUSES the pair that is neither
    // (it would have no innermost) and the pair that is the same run twice.
    std::stable_sort(laid.begin(), laid.end(), [](Laid const& a, Laid const& b) {
        if (a.region->parentIndex != b.region->parentIndex) {
            return a.region->parentIndex < b.region->parentIndex;
        }
        if (a.firstPos != b.firstPos) return a.firstPos < b.firstPos;
        return a.lastPos > b.lastPos;
    });
    {
        // `laid` is, per function, in order of each run's FIRST block, the longer
        // run first: every run that holds the current one is still on `open`.
        std::vector<Laid const*> open;
        for (Laid const& cur : laid) {
            while (!open.empty()
                   && (open.back()->region->parentIndex != cur.region->parentIndex
                       || open.back()->lastPos < cur.firstPos)) {
                open.pop_back();
            }
            if (!open.empty()) {
                Laid const& outer = *open.back();
                bool const sameRun = outer.firstPos == cur.firstPos
                                  && outer.lastPos == cur.lastPos;
                if (sameRun || cur.lastPos > outer.lastPos) {
                    emitErr(reporter, std::format(
                        "synthesizeSehFunclets: guarded regions {} and {} of one function "
                        "are laid out at positions {}..{} and {}..{} — {}. Which handler a "
                        "fault reaches is decided by the order of the records whose ranges "
                        "hold its address, and that order exists only when, of any two "
                        "ranges that share an address, one lies inside the other",
                        outer.region->regionId, cur.region->regionId, outer.firstPos,
                        outer.lastPos, cur.firstPos, cur.lastPos,
                        sameRun ? "the same run twice"
                                : "they overlap and neither holds the other"));
                    return false;
                }
            }
            open.push_back(&cur);
        }
    }
    std::stable_sort(laid.begin(), laid.end(), [](Laid const& a, Laid const& b) {
        if (a.region->parentIndex != b.region->parentIndex) {
            return a.region->parentIndex < b.region->parentIndex;
        }
        if (a.lastPos != b.lastPos) return a.lastPos < b.lastPos;
        return a.firstPos > b.firstPos;
    });
    outScopes.reserve(laid.size());
    for (Laid const& l : laid) {
        MirSehScope s;
        s.parentFuncSymbol    = l.region->parentSym;
        s.beginBlock          = l.begin;
        s.endBlock            = l.last;
        s.handlerBlock        = l.handler;
        s.filterFuncletSymbol = l.region->funcletSym;
        s.personalitySymbol   = personalitySym;
        outScopes.push_back(s);
    }
    return true;
}

} // namespace dss
