#include "lir/lir_liveness.hpp"

#include "lir/lir_node.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
template <class OnUse>
void forEachUse(Lir const& lir, LirInstId id, OnUse&& onUse) {
    auto const ops = lir.instOperands(id);
    for (auto const& op : ops) {
        if (op.kind == LirOperandKind::Reg && op.reg.valid()
            && op.reg.isPhysical == 0) {
            onUse(op.reg);
        }
    }
}

[[nodiscard]] bool hasDef(Lir const& lir, LirInstId id) noexcept {
    auto const r = lir.instResult(id);
    return r.valid() && r.isPhysical == 0;
}

// Compute reverse post-order of blocks reachable from `entry`. Any
// orphan blocks (unreachable from `entry`) are appended in arena
// order so the analysis is total over the function's block range.
// The LIR verifier does not currently flag orphans (no
// I_UnreachableBlock rule exists for LIR — that rule is MIR-only);
// this routine is therefore the sole defense and is intentionally
// total rather than fail-loud.
[[nodiscard]] std::vector<LirBlockId> computeRpo(Lir const& lir, LirFuncId fn) {
    std::uint32_t const blockCount = lir.funcBlockCount(fn);
    std::vector<LirBlockId> rpo;
    rpo.reserve(blockCount);
    if (blockCount == 0) return rpo;

    // Address each block of `fn` by its position in the function's
    // block range — `lir.funcBlockAt(fn, i)` for `i in [0, blockCount)`
    // — rather than by arena-index arithmetic. The mapping
    // `LirBlockId.v → index` is rebuilt up-front; this keeps the RPO
    // computation robust against future arena layouts that don't
    // place a function's blocks at contiguous indices.
    std::vector<std::uint32_t> indexOfBlockV;
    LirBlockId const firstBlock = lir.funcBlockAt(fn, 0);
    indexOfBlockV.reserve(blockCount);
    std::uint32_t minV = firstBlock.v;
    std::uint32_t maxV = firstBlock.v;
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        std::uint32_t const v = lir.funcBlockAt(fn, i).v;
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
    }
    std::uint32_t const span = maxV - minV + 1u;
    indexOfBlockV.assign(span, UINT32_MAX);
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        indexOfBlockV[lir.funcBlockAt(fn, i).v - minV] = i;
    }
    auto idxOf = [&](LirBlockId b) -> std::uint32_t {
        if (b.v < minV || b.v > maxV) return UINT32_MAX;
        return indexOfBlockV[b.v - minV];
    };

    std::vector<std::uint8_t> visited(blockCount, 0);
    visited[idxOf(firstBlock)] = 1;

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
        if (top.nextSucc < succs.size()) {
            LirBlockId const s = succs[top.nextSucc++];
            std::uint32_t const si = idxOf(s);
            if (si != UINT32_MAX && !visited[si]) {
                visited[si] = 1;
                stack.push_back({s, 0});
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

LirFuncLiveness analyzeFuncLiveness(Lir const& lir, LirFuncId fn) {
    LirFuncLiveness out;
    out.fn         = fn;
    out.blockOrder = computeRpo(lir, fn);
    std::uint32_t const blockCount = static_cast<std::uint32_t>(out.blockOrder.size());
    std::uint32_t const numVRegs   = lir.funcNumVRegs(fn);

    if (blockCount == 0) return out;

    // Per-block local USE and DEF sets + parallel sized liveIn/Out.
    std::vector<VRegBitset> use(blockCount), def(blockCount);
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
            if (hasDef(lir, inst)) {
                auto const r = lir.instResult(inst);
                def[bi].insert(r.id);
                killedLocally.insert(r.id);
            }
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
            // liveIn[B] = use[B] ∪ (liveOut[B] - def[B])
            VRegBitset newIn = out.liveOut[bi];
            newIn.subtractInPlace(def[bi]);
            newIn.unionInPlace(use[bi]);
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
            if (hasDef(lir, inst)) {
                auto const r = lir.instResult(inst);
                if (r.id >= state.size()) state.resize(r.id + 1);
                auto& s = state[r.id];
                s.everSeen = true;
                s.cls      = r.regClass();
                if (latePos < s.firstDef) s.firstDef = latePos;
            }
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
        LirReg vreg{};
        vreg.id         = id;
        vreg.classKind  = static_cast<std::uint32_t>(s.cls);
        vreg.isPhysical = 0;
        out.ranges.push_back(LirLiveRange::make(vreg, start, end));
    }
    std::sort(out.ranges.begin(), out.ranges.end(),
              [](LirLiveRange const& a, LirLiveRange const& b) {
                  return std::tie(a.start, a.vreg.id)
                       < std::tie(b.start, b.vreg.id);
              });
    return out;
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

} // namespace dss
