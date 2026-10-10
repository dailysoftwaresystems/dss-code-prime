#include "lir/lir_liveness.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_asm_region.hpp"
#include "lir/lir_node.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dss {

namespace {

[[noreturn]] void livenessFatal(char const* what) {
    std::fputs("dss::LirLiveness fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Words needed to bit-pack vreg ids in `[0, numVRegs)`. Bit `k` is
// vreg id `k`; id 0 is the unused sentinel (`LirBuilder::newVReg`
// mints ids starting at 1). For `numVRegs == 0` we still allocate one
// word so a stray sentinel-only query has a place to land.
[[nodiscard]] std::uint32_t bitsetWordsFor(std::uint32_t numVRegs) noexcept {
    if (numVRegs == 0) return 1u;
    return (numVRegs + 63u) / 64u;
}

// Walk `Reg`-kind operands of an instruction; report each as a virtual-
// register use. The LIR memory-addressing triple encodes the BASE
// register as a plain `Reg` operand followed by `MemBase` (the scale
// field) and `MemOffset` (the displacement); the 4-operand `lea`
// arm adds an optional `Reg` index between base and MemBase. Walking
// all Reg-kind operands captures every use site. Physical registers
// (post-regalloc) are intentionally NOT tracked: this substrate is
// pre-regalloc-only; physical-reg liveness for clobber tracking
// (calls, fixed assignments) belongs in a separate pass.
// ★ P68 round 8 part 4: WHICH operands are read, and which registers an
// instruction writes, is asked of `lirForEachInstUse` / `lirForEachInstDef`
// (`lir_asm_region.hpp`), the ONE reader of both — an inline-asm bundle writes
// several registers through operand ROLES, and its write-only operands are
// not uses. Reading `instResult` here instead would give every statement
// output no definition at all: a range starting at 0, live across the whole
// function above the statement.
template <class OnUse>
void forEachUse(Lir const& lir, LirInstId id, OnUse&& onUse) {
    lirForEachInstUse(lir, id, [&](LirReg r) {
        if (r.isPhysical == 0) onUse(r);
    });
}

// `onDef(reg, early)` for every VIRTUAL register the instruction defines.
template <class OnDef>
void forEachDef(Lir const& lir, LirInstId id, OnDef&& onDef) {
    lirForEachInstDef(lir, id, [&](LirReg r, bool early) {
        if (r.isPhysical == 0) onDef(r, early);
    });
}

// A function's blocks, addressed by their position in the function's block
// list — `lir.funcBlockAt(fn, i)` for `i in [0, blockCount)` — rather than by
// arena-index arithmetic. The mapping `LirBlockId.v → index` is built once;
// this keeps every walk below robust against an arena layout that does not
// place a function's blocks at contiguous indices.
class FuncBlockIndex {
public:
    FuncBlockIndex(Lir const& lir, LirFuncId fn) {
        std::uint32_t const blockCount = lir.funcBlockCount(fn);
        if (blockCount == 0) return;
        minV_ = maxV_ = lir.funcBlockAt(fn, 0).v;
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            std::uint32_t const v = lir.funcBlockAt(fn, i).v;
            minV_ = std::min(minV_, v);
            maxV_ = std::max(maxV_, v);
        }
        indexOfBlockV_.assign(maxV_ - minV_ + 1u, UINT32_MAX);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            indexOfBlockV_[lir.funcBlockAt(fn, i).v - minV_] = i;
        }
    }

    // UINT32_MAX when `v` is not a block of the function.
    [[nodiscard]] std::uint32_t of(std::uint32_t v) const noexcept {
        if (indexOfBlockV_.empty() || v < minV_ || v > maxV_) return UINT32_MAX;
        return indexOfBlockV_[v - minV_];
    }

private:
    std::vector<std::uint32_t> indexOfBlockV_;
    std::uint32_t              minV_ = 0;
    std::uint32_t              maxV_ = 0;
};

// One guarded run (`LirGuardedRegion`), resolved against its function's block
// list and already judged well-formed: `firstIndex <= lastIndex`, and the
// landing block is a block of the function outside `[firstIndex, lastIndex]`.
struct ResolvedRun {
    std::uint32_t firstIndex   = 0;
    std::uint32_t lastIndex    = 0;
    std::uint32_t landingIndex = 0;
};

// The EXCEPTIONAL successors of every block of a function, indexed by the
// block's position in the function's block list: the landing block (as a
// position in that list) of every run that holds the block. A block of a nested
// run has one per level. Empty for a function without runs.
[[nodiscard]] std::vector<std::vector<std::uint32_t>>
exceptionalSuccessors(std::uint32_t blockCount, std::span<ResolvedRun const> runs) {
    std::vector<std::vector<std::uint32_t>> out;
    if (runs.empty()) return out;
    out.resize(blockCount);
    for (auto const& run : runs) {
        for (std::uint32_t k = run.firstIndex; k <= run.lastIndex; ++k) {
            auto& list = out[k];
            if (std::find(list.begin(), list.end(), run.landingIndex) == list.end()) {
                list.push_back(run.landingIndex);
            }
        }
    }
    return out;
}

// Compute reverse post-order of blocks reachable from `entry`. Any
// orphan blocks (unreachable from `entry`) are appended in arena
// order so the analysis is total over the function's block range.
// The LIR verifier does not currently flag orphans (no
// I_UnreachableBlock rule exists for LIR — that rule is MIR-only);
// this routine is therefore the sole defense and is intentionally
// total rather than fail-loud.
//
// ★ `exSuccs` (see `exceptionalSuccessors`) is walked AFTER a block's own
// successors, so a landing block is REACHED — from every block of its run — and
// takes its place in the order after the blocks that can transfer to it. Before
// the exceptional edge existed a landing block was an orphan, appended last in
// ARENA order together with everything only it reaches; a flat range is sound
// only when a definition precedes its uses in this order, which reverse
// post-order gives and arena order does not.
[[nodiscard]] std::vector<LirBlockId>
computeRpo(Lir const& lir, LirFuncId fn, FuncBlockIndex const& indexOf,
           std::vector<std::vector<std::uint32_t>> const& exSuccs) {
    std::uint32_t const blockCount = lir.funcBlockCount(fn);
    std::vector<LirBlockId> rpo;
    rpo.reserve(blockCount);
    if (blockCount == 0) return rpo;

    LirBlockId const firstBlock = lir.funcBlockAt(fn, 0);
    std::vector<std::uint8_t> visited(blockCount, 0);
    visited[indexOf.of(firstBlock.v)] = 1;

    struct Frame {
        LirBlockId block;
        std::uint32_t nextSucc = 0;
    };
    std::vector<Frame> stack;
    stack.push_back({firstBlock, 0});
    std::vector<LirBlockId> postOrder;
    postOrder.reserve(blockCount);

    while (!stack.empty()) {
        auto& top  = stack.back();
        auto const succs = lir.blockSuccessors(top.block);
        std::uint32_t const own = static_cast<std::uint32_t>(succs.size());
        std::uint32_t const topIndex = indexOf.of(top.block.v);
        std::uint32_t const extra =
            exSuccs.empty() || topIndex == UINT32_MAX
                ? 0u : static_cast<std::uint32_t>(exSuccs[topIndex].size());
        if (top.nextSucc < own + extra) {
            std::uint32_t const k = top.nextSucc++;
            std::uint32_t const si =
                k < own ? indexOf.of(succs[k].v) : exSuccs[topIndex][k - own];
            if (si != UINT32_MAX && !visited[si]) {
                visited[si] = 1;
                stack.push_back({lir.funcBlockAt(fn, si), 0});
            }
            continue;
        }
        postOrder.push_back(top.block);
        stack.pop_back();
    }

    rpo.assign(postOrder.rbegin(), postOrder.rend());

    // Append orphan blocks in arena order so analysis is total.
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        if (!visited[i]) rpo.push_back(lir.funcBlockAt(fn, i));
    }
    return rpo;
}

// Iterate the N instructions in a block, yielding `(instId, earlyPos,
// latePos)` for each. `blockStartPos` is the position assigned to
// the block's first inst's early slot.
template <class F>
void forEachInstInBlock(Lir const& lir, LirBlockId b,
                        std::uint32_t blockStartPos, F&& f) {
    std::uint32_t const n = lir.blockInstCount(b);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t const early = blockStartPos + 2u * i;
        f(lir.blockInstAt(b, i), early, early + 1u);
    }
}

} // namespace

// ── VRegBitset ──────────────────────────────────────────────────────

void VRegBitset::resizeForCapacity(std::uint32_t numVRegs) {
    std::uint32_t const words = bitsetWordsFor(numVRegs);
    if (bits.size() < words) bits.resize(words, 0u);
    capacity = std::max(capacity, numVRegs);
}

bool VRegBitset::contains(std::uint32_t vregId) const noexcept {
    if (vregId == 0) return false;
    std::uint32_t const w = vregId >> 6;
    if (w >= bits.size()) return false;
    return (bits[w] >> (vregId & 63u)) & 1u;
}

void VRegBitset::insert(std::uint32_t vregId) {
    if (vregId == 0) return;  // sentinel: silent no-op (mirrors LirReg::valid)
    std::uint32_t const w = vregId >> 6;
    if (w >= bits.size()) bits.resize(w + 1u, 0u);
    if (vregId >= capacity) capacity = vregId + 1u;
    bits[w] |= std::uint64_t{1} << (vregId & 63u);
}

void VRegBitset::erase(std::uint32_t vregId) noexcept {
    std::uint32_t const w = vregId >> 6;
    if (w >= bits.size()) return;
    bits[w] &= ~(std::uint64_t{1} << (vregId & 63u));
}

bool VRegBitset::unionInPlace(VRegBitset const& other) {
    if (other.bits.size() > bits.size()) bits.resize(other.bits.size(), 0u);
    if (other.capacity > capacity) capacity = other.capacity;
    bool changed = false;
    for (std::size_t w = 0; w < other.bits.size(); ++w) {
        std::uint64_t const before = bits[w];
        bits[w] |= other.bits[w];
        if (bits[w] != before) changed = true;
    }
    return changed;
}

void VRegBitset::subtractInPlace(VRegBitset const& mask) {
    std::size_t const n = std::min(bits.size(), mask.bits.size());
    for (std::size_t w = 0; w < n; ++w) bits[w] &= ~mask.bits[w];
}

void VRegBitset::clear() noexcept {
    std::fill(bits.begin(), bits.end(), 0u);
}

// ── LirLiveRange ────────────────────────────────────────────────────

LirLiveRange LirLiveRange::make(LirReg vreg, std::uint32_t start, std::uint32_t end) {
    if (vreg.isPhysical != 0) {
        livenessFatal("LirLiveRange::make: physical-reg range "
                      "(substrate is pre-regalloc only)");
    }
    if (end <= start) {
        livenessFatal("LirLiveRange::make: empty range (end <= start)");
    }
    return LirLiveRange{vreg, start, end};
}

// ── LirLiveness ─────────────────────────────────────────────────────

LirFuncLiveness const* LirLiveness::forFunc(LirFuncId fn) const noexcept {
    for (auto const& flow : perFunc) {
        if (flow.fn.v == fn.v && flow.fn.arenaTag == fn.arenaTag) return &flow;
    }
    return nullptr;
}

bool lirRangeEntersALanding(LirLiveRange const&            r,
                            std::span<std::uint32_t const> landingEntryPositions) noexcept {
    // The first landing entry strictly after the range's start; ascending, so
    // if that one is not inside the range no later one is.
    auto const lo = std::upper_bound(landingEntryPositions.begin(),
                                     landingEntryPositions.end(), r.start);
    return lo != landingEntryPositions.end() && *lo < r.end;
}

namespace {

// One function's result together with what only the analysis itself holds: the
// per-block DEF sets, indexed like `flow.liveIn`. The region check reads them.
struct FuncAnalysis {
    LirFuncLiveness         flow;
    std::vector<VRegBitset> def;
};

[[nodiscard]] FuncAnalysis
analyzeFuncWithRuns(Lir const& lir, LirFuncId fn, std::span<ResolvedRun const> runs) {
    FuncAnalysis result;
    LirFuncLiveness& out = result.flow;
    out.fn = fn;
    FuncBlockIndex const indexOf{lir, fn};
    // Indexed by a block's position in the FUNCTION's block list.
    std::vector<std::vector<std::uint32_t>> const exSuccs =
        exceptionalSuccessors(lir.funcBlockCount(fn), runs);
    out.blockOrder = computeRpo(lir, fn, indexOf, exSuccs);
    std::uint32_t const blockCount = static_cast<std::uint32_t>(out.blockOrder.size());
    std::uint32_t const numVRegs   = lir.funcNumVRegs(fn);

    if (blockCount == 0) return result;

    // Per-block local USE and DEF sets + parallel sized liveIn/Out.
    std::vector<VRegBitset> use(blockCount);
    std::vector<VRegBitset>& def = result.def;
    def.assign(blockCount, VRegBitset{});
    out.liveIn.assign(blockCount,  VRegBitset{});
    out.liveOut.assign(blockCount, VRegBitset{});
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        use[bi].resizeForCapacity(numVRegs);
        def[bi].resizeForCapacity(numVRegs);
        out.liveIn[bi].resizeForCapacity(numVRegs);
        out.liveOut[bi].resizeForCapacity(numVRegs);
    }

    // Number each instruction in RPO traversal order.
    std::vector<std::uint32_t> blockFirstPos(blockCount, 0);
    std::uint32_t orderIdx = 0;
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        blockFirstPos[bi] = orderIdx * 2u;
        std::uint32_t const n = lir.blockInstCount(out.blockOrder[bi]);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(out.blockOrder[bi], i);
            out.positionToInst.push_back(inst);
            out.positionToInst.push_back(inst);
            ++orderIdx;
        }
    }
    out.totalPositions = orderIdx * 2u;

    // Compute USE / DEF per block. USE[B] is upward-exposed: a vreg
    // used before any local def in B. DEF[B] is the set of vregs
    // defined anywhere in B.
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        VRegBitset killedLocally;
        killedLocally.resizeForCapacity(numVRegs);
        forEachInstInBlock(lir, out.blockOrder[bi], 0,
                           [&](LirInstId inst, std::uint32_t, std::uint32_t) {
            forEachUse(lir, inst, [&](LirReg r) {
                if (!killedLocally.contains(r.id)) use[bi].insert(r.id);
            });
            forEachDef(lir, inst, [&](LirReg r, bool) {
                def[bi].insert(r.id);
                killedLocally.insert(r.id);
            });
        });
    }

    // Iterative backward dataflow to fixpoint. Process in reverse RPO
    // so back-edges converge fast. This is the unconditional fixpoint
    // variant (no priority worklist); ML6 cycle 2 can promote to a
    // worklist if profile shows it dominates compile time.
    //
    // `LirBlockId.v → blockOrder index` mapping rebuilt via the
    // `funcBlockAt`-enumerated `blockOrder` (no contiguous-arena
    // assumption).
    std::vector<std::uint32_t> orderOfBlockV;
    std::uint32_t minV = out.blockOrder[0].v, maxV = out.blockOrder[0].v;
    for (auto const& b : out.blockOrder) {
        minV = std::min(minV, b.v);
        maxV = std::max(maxV, b.v);
    }
    orderOfBlockV.assign(maxV - minV + 1u, UINT32_MAX);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        orderOfBlockV[out.blockOrder[bi].v - minV] = bi;
    }
    auto orderIdxOf = [&](LirBlockId b) -> std::uint32_t {
        if (b.v < minV || b.v > maxV) return UINT32_MAX;
        return orderOfBlockV[b.v - minV];
    };

    // The exceptional successors again, in THIS walk's vocabulary: for each
    // block-order index, the block-order indices of the landing blocks of every
    // guarded run that holds the block.
    std::vector<std::vector<std::uint32_t>> landingsOf;
    if (!exSuccs.empty()) {
        landingsOf.resize(blockCount);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            std::uint32_t const fi = indexOf.of(out.blockOrder[bi].v);
            if (fi == UINT32_MAX) continue;
            for (std::uint32_t const landingFi : exSuccs[fi]) {
                std::uint32_t const li =
                    orderIdxOf(lir.funcBlockAt(fn, landingFi));
                if (li != UINT32_MAX) landingsOf[bi].push_back(li);
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::uint32_t bk = blockCount; bk > 0; --bk) {
            std::uint32_t const bi = bk - 1;
            auto const succs = lir.blockSuccessors(out.blockOrder[bi]);
            // liveOut[B] = union over successors S of liveIn[S]
            for (auto const& s : succs) {
                std::uint32_t const sIdx = orderIdxOf(s);
                if (sIdx == UINT32_MAX) continue;
                if (out.liveOut[bi].unionInPlace(out.liveIn[sIdx])) changed = true;
            }
            // … and over the EXCEPTIONAL ones: what a landing block reads is
            // live at the end of — and, nothing in the run defining it, all
            // through — every block control can leave for it.
            if (!landingsOf.empty()) {
                for (std::uint32_t const li : landingsOf[bi]) {
                    if (out.liveOut[bi].unionInPlace(out.liveIn[li])) changed = true;
                }
            }
            // liveIn[B] = use[B] ∪ (liveOut[B] - def[B])
            VRegBitset newIn = out.liveOut[bi];
            newIn.subtractInPlace(def[bi]);
            // `(void)`: `newIn` is built from scratch each iteration, so
            // "did it grow" says nothing about the FIXPOINT — the word-wise
            // comparison two lines down is what decides `changed` here. The
            // discard is deliberate and now spelled, because the `[[nodiscard]]`
            // on `unionInPlace` exists to make the OTHER kind (a forgotten
            // result at the `liveOut` join above) a compile diagnostic.
            (void)newIn.unionInPlace(use[bi]);
            // Detect-by-word: only the changed flag matters here.
            if (newIn.bits != out.liveIn[bi].bits) {
                out.liveIn[bi] = std::move(newIn);
                changed = true;
            }
        }
    }

    // Build live ranges by walking each block in RPO and tracking
    // each vreg's firstDef + lastUse. Positions increase monotonically
    // across the walk.
    //
    // For values truly live-in to the function entry (parameters via
    // `arg` pseudo-op, defined at the entry block's positions 0/1),
    // `firstDef = latePos` of the `arg` inst is set by the def-side
    // branch below. For vregs that appear ONLY as a use in some block
    // (a malformed-LIR shape; the verifier should catch it), we fall
    // through to the emission with `start = 0` so the range remains
    // expressible.
    struct RangeState {
        std::uint32_t firstDef = UINT32_MAX;
        std::uint32_t lastUse  = 0;
        bool          everSeen = false;
        LirRegClass   cls      = LirRegClass::None;
    };
    std::vector<RangeState> state(numVRegs);

    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        std::uint32_t const blockStartPos = blockFirstPos[bi];
        forEachInstInBlock(lir, out.blockOrder[bi], blockStartPos,
                           [&](LirInstId inst, std::uint32_t earlyPos,
                               std::uint32_t latePos) {
            forEachUse(lir, inst, [&](LirReg r) {
                if (r.id >= state.size()) state.resize(r.id + 1);
                auto& s = state[r.id];
                s.everSeen = true;
                s.cls      = r.regClass();
                if (earlyPos > s.lastUse) s.lastUse = earlyPos;
            });
            forEachDef(lir, inst, [&](LirReg r, bool early) {
                if (r.id >= state.size()) state.resize(r.id + 1);
                auto& s = state[r.id];
                s.everSeen = true;
                s.cls      = r.regClass();
                // ── EARLY-CLOBBER (`kLirInstFlagEarlyClobberResult`) ──
                // An ordinary instruction reads every operand before it
                // writes anything, so its def belongs at the LATE slot:
                // the inputs' ranges end at `earlyPos + 1 == latePos`,
                // they expire exactly as the result is allocated, and the
                // result may reuse an input's register. That reuse IS the
                // reference-compiler behaviour for a plain `"=r"`.
                // An EARLY-CLOBBER result may be written before the inputs
                // have all been read, so its range must OVERLAP the slot at
                // which they are read: record the def at `earlyPos`. Nothing
                // then expires under it and the linear scan cannot hand the
                // result an input's register — no second exclusion list, no
                // allocator special case.
                // ⚠ The slot is the discriminating variable, NOT which
                // instruction of an expansion carries the def: for the
                // single-instruction shape (the one inline asm actually
                // needs) the def is already on the first instruction.
                // Range well-formedness is preserved: the emission below
                // computes `end = max(lastUse + 1, start + 1)`, so
                // `LirLiveRange::make`'s `start < end` holds even when the
                // result is never used.
                // ★ `early` comes from the one helper: the flag above on an
                // ordinary result, the `EarlyDef` role on a bundle operand
                // (P68 round 8 part 4) — the same slot rule, two carriers.
                std::uint32_t const defPos = early ? earlyPos : latePos;
                if (defPos < s.firstDef) s.firstDef = defPos;
            });
        });
        // Vregs live-out of this block extend their `lastUse` to the
        // LAST position the value is live AT inside this block — the
        // late slot of the block's last instruction. `blockEndPos` is
        // the half-open boundary (one past that); the range emission
        // adds +1 to `lastUse` to produce the half-open `end`, so we
        // store `blockEndPos - 1` here. Captures back-edge-extends-
        // loop-body for loops without overshooting `totalPositions`.
        std::uint32_t const blockEndPos =
            blockStartPos + 2u * lir.blockInstCount(out.blockOrder[bi]);
        if (blockEndPos == 0u) continue;  // empty block (shouldn't happen)
        std::uint32_t const lastLivePos = blockEndPos - 1u;
        auto const& lOut = out.liveOut[bi];
        for (std::size_t w = 0; w < lOut.bits.size(); ++w) {
            std::uint64_t bits = lOut.bits[w];
            while (bits) {
                std::uint32_t const lo = static_cast<std::uint32_t>(std::countr_zero(bits));
                std::uint32_t const id = static_cast<std::uint32_t>(w << 6) + lo;
                bits &= bits - 1u;
                if (id >= state.size()) state.resize(id + 1);
                auto& s = state[id];
                if (lastLivePos > s.lastUse) s.lastUse = lastLivePos;
                s.everSeen = true;
            }
        }
    }

    // Emit ranges for every vreg ever seen. Skip id 0 (invalid
    // sentinel — `LirBuilder::newVReg` never mints it).
    for (std::uint32_t id = 1; id < state.size(); ++id) {
        auto const& s = state[id];
        if (!s.everSeen) continue;
        std::uint32_t const start = (s.firstDef == UINT32_MAX) ? 0u : s.firstDef;
        std::uint32_t const end   = (s.lastUse > start) ? (s.lastUse + 1u)
                                                        : (start + 1u);
        // D-LIR-POSITIONAL-LIRREG-INIT-MISCLASSIFIES-SILENTLY: this used to
        // assemble the register field-by-field, which is the one remaining
        // way to write `classKind` without the type system checking that the
        // value IS a register class. `makeVirtualReg` states both facts —
        // typed class, virtual — in one expression.
        out.ranges.push_back(
            LirLiveRange::make(makeVirtualReg(id, s.cls), start, end));
    }
    std::sort(out.ranges.begin(), out.ranges.end(),
              [](LirLiveRange const& a, LirLiveRange const& b) {
                  return std::tie(a.start, a.vreg.id)
                       < std::tie(b.start, b.vreg.id);
              });

    // The guarded runs, in this result's vocabulary.
    out.guardedRuns.reserve(runs.size());
    for (auto const& run : runs) {
        LirFuncLiveness::GuardedRun g;
        g.blockOrderIndices.reserve(run.lastIndex - run.firstIndex + 1u);
        for (std::uint32_t k = run.firstIndex; k <= run.lastIndex; ++k) {
            std::uint32_t const bi = orderIdxOf(lir.funcBlockAt(fn, k));
            if (bi != UINT32_MAX) g.blockOrderIndices.push_back(bi);
        }
        g.landingOrderIndex    = orderIdxOf(lir.funcBlockAt(fn, run.landingIndex));
        g.landingEntryPosition = blockFirstPos[g.landingOrderIndex];
        out.landingEntryPositions.push_back(g.landingEntryPosition);
        out.guardedRuns.push_back(std::move(g));
    }
    std::sort(out.landingEntryPositions.begin(), out.landingEntryPositions.end());
    out.landingEntryPositions.erase(
        std::unique(out.landingEntryPositions.begin(), out.landingEntryPositions.end()),
        out.landingEntryPositions.end());
    return result;
}

void refuseRegion(DiagnosticReporter& reporter, std::string message) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::L_SideStructureIndexDangling;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(message);
    reporter.report(std::move(d));
}

// True iff some run that enters the landing block at `landingIndex` holds the
// block at `blockIndex` (both positions in one function's block list).
[[nodiscard]] bool aRunEnteringHolds(std::span<ResolvedRun const> runs,
                                     std::uint32_t landingIndex,
                                     std::uint32_t blockIndex) noexcept {
    for (ResolvedRun const& r : runs) {
        if (r.landingIndex == landingIndex && blockIndex >= r.firstIndex
            && blockIndex <= r.lastIndex) {
            return true;
        }
    }
    return false;
}

} // namespace

LirFuncLiveness analyzeFuncLiveness(Lir const& lir, LirFuncId fn) {
    return analyzeFuncWithRuns(lir, fn, {}).flow;
}

LirLiveness analyzeLiveness(Lir const& lir) {
    LirLiveness out;
    std::size_t const fnCount = lir.moduleFuncCount();
    out.perFunc.reserve(fnCount);
    for (std::size_t i = 0; i < fnCount; ++i) {
        out.perFunc.push_back(
            analyzeFuncLiveness(lir, lir.funcAt(static_cast<std::uint32_t>(i))));
    }
    return out;
}

std::optional<LirLiveness>
analyzeLiveness(Lir const& lir, std::span<LirGuardedRegion const> regions,
                DiagnosticReporter& reporter) {
    std::uint32_t const fnCount = static_cast<std::uint32_t>(lir.moduleFuncCount());

    // (1) SHAPE. Every region is resolved against its function's block list
    // before anything is analyzed with it; one that cannot be is refused by
    // name. `k` is the region's position in `regions`, which is how every
    // refusal below names it.
    std::vector<std::vector<ResolvedRun>>   runsOf(fnCount);
    std::vector<std::vector<std::uint32_t>> regionOf(fnCount);   // parallel: its `k`
    bool shapely = true;
    for (std::uint32_t k = 0; k < regions.size(); ++k) {
        LirGuardedRegion const& r = regions[k];
        if (r.funcIndex >= fnCount) {
            refuseRegion(reporter, std::format(
                "guarded region #{} names function #{}, and the module has {} function(s)",
                k, r.funcIndex, fnCount));
            shapely = false;
            continue;
        }
        FuncBlockIndex const indexOf{lir, lir.funcAt(r.funcIndex)};
        std::uint32_t const first   = indexOf.of(r.firstBlockV);
        std::uint32_t const last    = indexOf.of(r.lastBlockV);
        std::uint32_t const landing = indexOf.of(r.landingBlockV);
        auto const notABlock = [&](char const* what, std::uint32_t v) {
            refuseRegion(reporter, std::format(
                "guarded region #{} of function #{} names block {} as its {} block, "
                "which is not a block of that function", k, r.funcIndex, v, what));
            shapely = false;
        };
        if (first == UINT32_MAX)   notABlock("first", r.firstBlockV);
        if (last == UINT32_MAX)    notABlock("last", r.lastBlockV);
        if (landing == UINT32_MAX) notABlock("landing", r.landingBlockV);
        if (first == UINT32_MAX || last == UINT32_MAX || landing == UINT32_MAX) continue;
        if (first > last) {
            refuseRegion(reporter, std::format(
                "guarded region #{} of function #{} runs from block {} to block {}, and "
                "the first is laid out after the last", k, r.funcIndex, r.firstBlockV,
                r.lastBlockV));
            shapely = false;
            continue;
        }
        if (landing >= first && landing <= last) {
            refuseRegion(reporter, std::format(
                "guarded region #{} of function #{} holds its own landing block {} inside "
                "the run {}..{}: a fault in the landing block would be delivered to it",
                k, r.funcIndex, r.landingBlockV, r.firstBlockV, r.lastBlockV));
            shapely = false;
            continue;
        }
        runsOf[r.funcIndex].push_back(ResolvedRun{first, last, landing});
        regionOf[r.funcIndex].push_back(k);
    }
    if (!shapely) return std::nullopt;

    // (1b) THE ORDER OF THE RECORDS. `regions` is each function's table in the
    // order it will be written, and the party that reads the table gives a fault
    // to the FIRST record whose range holds its address. So, of two records of
    // one function that share a block, the earlier must be the DEEPER region's:
    //   * its run lies inside the later one's; and
    //   * when the two runs are the very same blocks — a run that belongs to a
    //     region and to the region around it — the earlier record's landing
    //     block, the inner one, lies inside a run of the later record's landing
    //     block (an outer body holds the inner landing; never the reverse).
    // A list that breaks either is a table that hands a fault to the wrong
    // landing, whichever tier ordered it, and is refused.
    //
    // The walk keeps, per function, the runs no later run has yet taken in. They
    // are pairwise apart — a run that arrives takes in every kept run it meets, or
    // is refused — so keyed by first block they are in order of last block too.
    bool ordered = true;
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        std::vector<ResolvedRun> const& runs = runsOf[fi];
        std::map<std::uint32_t, std::size_t> kept;   // a run's first block → its index in `runs`
        for (std::size_t ri = 0; ri < runs.size(); ++ri) {
            ResolvedRun const&      later = runs[ri];
            LirGuardedRegion const& laterRegion = regions[regionOf[fi][ri]];
            bool refused = false;
            // Every kept run that shares a block with this one: backwards from
            // the last that begins at or before this one's last block.
            auto it = kept.upper_bound(later.lastIndex);
            while (it != kept.begin()) {
                auto const prev = std::prev(it);
                std::size_t const        ei = prev->second;
                ResolvedRun const&       earlier = runs[ei];
                LirGuardedRegion const&  earlierRegion = regions[regionOf[fi][ei]];
                if (earlier.lastIndex < later.firstIndex) break;   // apart; so is every run before it
                if (earlier.firstIndex < later.firstIndex || earlier.lastIndex > later.lastIndex) {
                    refuseRegion(reporter, std::format(
                        "guarded region #{} of function #{} (blocks {}..{}) is listed before "
                        "guarded region #{} (blocks {}..{}), shares a block with it and does "
                        "not lie inside it: a fault is given to the first record whose range "
                        "holds its address, so the record of the deeper region must come first",
                        regionOf[fi][ei], fi, earlierRegion.firstBlockV, earlierRegion.lastBlockV,
                        regionOf[fi][ri], laterRegion.firstBlockV, laterRegion.lastBlockV));
                    refused = true;
                    break;
                }
                bool const sameBlocks = earlier.firstIndex == later.firstIndex
                                     && earlier.lastIndex == later.lastIndex;
                if (sameBlocks && earlier.landingIndex != later.landingIndex
                    && !aRunEnteringHolds(runs, later.landingIndex, earlier.landingIndex)) {
                    refuseRegion(reporter, std::format(
                        "guarded regions #{} and #{} of function #{} hold the same blocks "
                        "{}..{}, and the landing block {} of the first does not lie inside a "
                        "run that enters the landing block {} of the second: of two records "
                        "over the same blocks the deeper region's must come first",
                        regionOf[fi][ei], regionOf[fi][ri], fi, laterRegion.firstBlockV,
                        laterRegion.lastBlockV, earlierRegion.landingBlockV,
                        laterRegion.landingBlockV));
                    refused = true;
                    break;
                }
                it = kept.erase(prev);
            }
            if (refused) {
                ordered = false;
                continue;
            }
            kept.insert_or_assign(later.firstIndex, ri);
        }
    }
    if (!ordered) return std::nullopt;

    // (2) THE ANALYSIS, and (3) NO LANDING READS A VALUE ITS RUN DEFINES.
    LirLiveness out;
    out.perFunc.reserve(fnCount);
    bool deliverable = true;
    for (std::uint32_t fi = 0; fi < fnCount; ++fi) {
        FuncAnalysis analysis = analyzeFuncWithRuns(lir, lir.funcAt(fi), runsOf[fi]);
        LirFuncLiveness const& flow = analysis.flow;
        for (std::size_t ri = 0; ri < flow.guardedRuns.size(); ++ri) {
            auto const& run = flow.guardedRuns[ri];
            VRegBitset const& read = flow.liveIn[run.landingOrderIndex];
            // The first register a block of the run defines that the landing
            // reads; one refusal per region is enough to name the defect.
            bool refused = false;
            for (std::uint32_t const bi : run.blockOrderIndices) {
                VRegBitset const& defined = analysis.def[bi];
                std::size_t const words = std::min(read.bits.size(), defined.bits.size());
                for (std::size_t w = 0; w < words && !refused; ++w) {
                    std::uint64_t const both = read.bits[w] & defined.bits[w];
                    if (both == 0) continue;
                    std::uint32_t const id = static_cast<std::uint32_t>(w << 6)
                        + static_cast<std::uint32_t>(std::countr_zero(both));
                    refuseRegion(reporter, std::format(
                        "the landing block {} of guarded region #{} of function #{} reads "
                        "virtual register {}, which block {} of the run defines: a value "
                        "defined inside a guarded run cannot be delivered to its landing "
                        "block, because the transfer may come before the definition",
                        flow.blockOrder[run.landingOrderIndex].v, regionOf[fi][ri], fi, id,
                        flow.blockOrder[bi].v));
                    refused = true;
                }
                if (refused) break;
            }
            if (refused) deliverable = false;
        }
        out.perFunc.push_back(std::move(analysis.flow));
    }
    if (!deliverable) return std::nullopt;
    return out;
}

} // namespace dss
