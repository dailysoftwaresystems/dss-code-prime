#include "opt/passes/mem2reg.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss::opt::passes {

namespace {

// A reaching value at a program point is either an OLD-module
// instruction id (a value the rebuild will map via `rewrite`) or a
// Mem2Reg-inserted Phi (a NEW SSA def the rebuild has no source for
// — we key it by a synthetic marker).
struct ReachingValue {
    enum class Kind : std::uint8_t { OldInst, Phi };
    Kind          kind  = Kind::OldInst;
    std::uint32_t value = 0;  // OldInst: MirInstId.v ; Phi: marker key
};

// One Phi this pass plans to insert at block `blockOld` for alloca
// `allocaOldId`. The marker key `markerV` is the synthetic id we use
// to refer to this phi before it has been emitted into the rebuild's
// arena. Incomings are recorded during the rename walk; the rebuild's
// `onBlockBegin` allocates the new phi id; `finalizePhiIncomings`
// walks the incomings list + resolves each to a (newValue, newBlock)
// pair via the rebuilder's exposed maps.
struct PendingPhi {
    std::uint32_t allocaOldIdV = 0;
    std::uint32_t markerV      = 0;
    MirBlockId    blockOld{};
};

struct PendingIncoming {
    ReachingValue value;
    MirBlockId    predOld{};
};

class Mem2RegPolicy final : public MirRebuildPolicy {
public:
    Mem2RegPolicy(Mir const& src, TypeInterner const& interner)
        : src_(src), interner_(interner) {}

    [[nodiscard]] std::size_t allocasPromoted()   const noexcept { return allocasPromoted_; }
    [[nodiscard]] std::size_t phisInserted()      const noexcept { return phisInserted_; }
    [[nodiscard]] std::size_t loadsReplaced()     const noexcept { return loadsReplaced_; }
    [[nodiscard]] std::size_t storesEliminated()  const noexcept { return storesEliminated_; }

    // Run the analysis + rename DFS for one function. Records all
    // load replacements + pending phi incomings; the subsequent
    // rebuild + `finalizePhiIncomings` materialize the result.
    void analyze(MirFuncId fn);

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        // All blocks — Mem2Reg never elides blocks. (RPO order is
        // not required by the rebuild; the source's block order is
        // already a valid topological walk of the CFG.)
        std::uint32_t const blockCount = src.funcBlockCount(fn);
        std::vector<MirBlockId> all;
        all.reserve(blockCount);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            all.push_back(src.funcBlockAt(fn, i));
        }
        return all;
    }

    // Implements the IDF-phi-insertion contract documented at
    // MirRebuildPolicy::onBlockBegin (D-OPT-MIR-REBUILDER-ONBLOCKBEGIN-HOOK).
    void onBlockBegin(MirBlockId oldB, MirBlockId /*newB*/,
                      MirBuilder& dst,
                      std::unordered_map<std::uint32_t, MirInstId>& /*rewrite*/,
                      std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) override {
        auto it = phisByBlock_.find(oldB.v);
        if (it == phisByBlock_.end()) return;
        for (PendingPhi const& pp : it->second) {
            TypeId const elem = allocaElementType_.at(pp.allocaOldIdV);
            MirInstId const newPhi = dst.addPhi(elem);
            phiNewIdByMarker_[pp.markerV] = newPhi;
            ++phisInserted_;
        }
    }

    [[nodiscard]] bool shouldEmit(MirInstId oldId) override {
        MirOpcode const op = src_.instOpcode(oldId);
        if (op == MirOpcode::Alloca && promoted_.count(oldId.v)) {
            return false;  // alloca for a promoted slot vanishes
        }
        if (op == MirOpcode::Store) {
            auto const ops = src_.instOperands(oldId);
            if (ops.size() == 2 && promoted_.count(ops[1].v)) {
                ++storesEliminated_;
                return false;  // store to a promoted slot vanishes
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<MirInstId>
    tryRewrite(MirOpcode op, MirInstId oldId, MirBuilder& /*dst*/,
               std::unordered_map<std::uint32_t, MirInstId> const& rewrite) override {
        if (op != MirOpcode::Load) return std::nullopt;
        auto const ops = src_.instOperands(oldId);
        if (ops.size() != 1 || !promoted_.count(ops[0].v)) return std::nullopt;
        auto const it = loadReplacement_.find(oldId.v);
        if (it == loadReplacement_.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::Mem2Reg fatal: promoted Load oldId v=%u "
                "has no rename-walk reaching value — analyze() invariant "
                "violation (a Load on a promotable alloca must have been "
                "visited by the dom-tree DFS).\n", oldId.v);
            std::abort();
        }
        ++loadsReplaced_;
        return resolveToNewId(it->second, rewrite);
    }

    // Called by the pass driver AFTER `MirFunctionRebuilder::rebuildFunction`
    // returns. Walks the recorded phi-incoming list per inserted phi
    // and emits `dst.addPhiIncoming(newPhi, {newValue, newPred})`.
    void finalizePhiIncomings(
        MirBuilder& dst,
        std::unordered_map<std::uint32_t, MirBlockId> const& blockMap,
        std::unordered_map<std::uint32_t, MirInstId> const& rewrite) {
        for (auto const& [markerV, incomings] : phiIncomings_) {
            auto const phiIt = phiNewIdByMarker_.find(markerV);
            if (phiIt == phiNewIdByMarker_.end()) {
                std::fprintf(stderr,
                    "dss::opt::passes::Mem2Reg fatal: phi marker v=%u "
                    "has incomings but no emitted phi — onBlockBegin "
                    "invariant violation.\n", markerV);
                std::abort();
            }
            MirInstId const newPhi = phiIt->second;
            for (PendingIncoming const& inc : incomings) {
                auto const blkIt = blockMap.find(inc.predOld.v);
                if (blkIt == blockMap.end()) {
                    std::fprintf(stderr,
                        "dss::opt::passes::Mem2Reg fatal: phi incoming "
                        "predOld v=%u not in rebuild blockMap.\n",
                        inc.predOld.v);
                    std::abort();
                }
                MirInstId const newVal = resolveToNewId(inc.value, rewrite);
                dst.addPhiIncoming(newPhi, MirPhiIncoming{newVal, blkIt->second});
            }
        }
    }

    // Reset per-function state between functions in the same module.
    void resetPerFunction() {
        promoted_.clear();
        allocaElementType_.clear();
        phisByBlock_.clear();
        phiIncomings_.clear();
        phiNewIdByMarker_.clear();
        loadReplacement_.clear();
        nextMarker_ = 1;
    }

private:
    [[nodiscard]] MirInstId resolveToNewId(
        ReachingValue rv,
        std::unordered_map<std::uint32_t, MirInstId> const& rewrite) const {
        if (rv.kind == ReachingValue::Kind::Phi) {
            auto const it = phiNewIdByMarker_.find(rv.value);
            if (it == phiNewIdByMarker_.end()) {
                std::fprintf(stderr,
                    "dss::opt::passes::Mem2Reg fatal: ReachingValue::Phi "
                    "marker v=%u has no emitted phi id — block-walk order "
                    "violation.\n", rv.value);
                std::abort();
            }
            return it->second;
        }
        // OldInst — must be in the rebuild's rewrite map by now (the
        // value-producer was emitted earlier in scan order).
        auto const it = rewrite.find(rv.value);
        if (it == rewrite.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::Mem2Reg fatal: ReachingValue::OldInst "
                "v=%u has no rewrite entry — scan-order violation.\n",
                rv.value);
            std::abort();
        }
        return it->second;
    }

    void renameWalkIterative(
        MirBlockId entry,
        std::vector<std::vector<MirBlockId>> const& domChildren,
        std::unordered_map<std::uint32_t, std::vector<ReachingValue>>& stacks);

    Mir const&          src_;
    TypeInterner const& interner_;

    // ── analysis output ──
    std::unordered_set<std::uint32_t>            promoted_;            // alloca .v
    std::unordered_map<std::uint32_t, TypeId>    allocaElementType_;   // alloca .v → pointee type
    std::unordered_map<std::uint32_t, std::vector<PendingPhi>> phisByBlock_;  // block .v → phis to insert
    std::unordered_map<std::uint32_t, std::vector<PendingIncoming>> phiIncomings_;  // marker → incomings
    std::unordered_map<std::uint32_t, ReachingValue> loadReplacement_;  // load .v → reaching value

    // ── rebuild-time state ──
    std::unordered_map<std::uint32_t, MirInstId>  phiNewIdByMarker_;   // marker → new phi id

    // ── counters (accumulated across all functions in the module) ──
    std::size_t allocasPromoted_  = 0;
    std::size_t phisInserted_     = 0;
    std::size_t loadsReplaced_    = 0;
    std::size_t storesEliminated_ = 0;
    std::uint32_t nextMarker_     = 1;
};

void Mem2RegPolicy::analyze(MirFuncId fn) {
    resetPerFunction();

    // CFG reachability: a block unreachable from `entry` cannot
    // contribute to any SSA computation. The rename DFS walks the
    // dom tree (which only spans reachable blocks); analysis must
    // mirror this scope or the gate would classify allocas based on
    // unreachable uses + the rename walk would miss them at runtime.
    MirBlockId const entry = src_.funcEntry(fn);
    auto const rpo = mirReversePostOrder(src_, entry);
    std::unordered_set<std::uint32_t> reachable;
    reachable.reserve(rpo.size());
    for (MirBlockId const b : rpo) reachable.insert(b.v);

    // Step 1: find allocas + their pointee types. Restricted to
    // reachable blocks — an alloca in dead code stays as-is.
    std::unordered_map<std::uint32_t, TypeId> allocaType;
    for (MirBlockId const b : rpo) {
        std::uint32_t const ninst = src_.blockInstCount(b);
        for (std::uint32_t i = 0; i < ninst; ++i) {
            MirInstId const id = src_.blockInstAt(b, i);
            if (src_.instOpcode(id) != MirOpcode::Alloca) continue;
            // Array alloca (operand count > 0) is NOT promotable —
            // promoting would lose memory identity. Only the scalar
            // form (zero operands) is eligible.
            if (!src_.instOperands(id).empty()) continue;
            // Volatile alloca: the user opted into observable memory
            // semantics; never promote.
            if (has(src_.instFlags(id), MirInstFlags::Volatile)) continue;
            TypeId const ptrTy = src_.instType(id);
            auto const ptrOps  = interner_.operands(ptrTy);
            if (ptrOps.empty()) continue;  // malformed Ptr — leave alone
            allocaType[id.v] = ptrOps[0];
        }
    }
    if (allocaType.empty()) return;

    // Step 2: promotability gate. Walk every inst (in reachable
    // blocks); any use of an alloca's RESULT other than Load[op0] /
    // Store[op1] disqualifies. Volatile Load / Store also disqualify
    // (the user requested observable semantics).
    std::unordered_set<std::uint32_t> nonPromotable;
    for (MirBlockId const b : rpo) {
        std::uint32_t const ninst = src_.blockInstCount(b);
        for (std::uint32_t i = 0; i < ninst; ++i) {
            MirInstId const id = src_.blockInstAt(b, i);
            MirOpcode const op = src_.instOpcode(id);
            if (op == MirOpcode::Phi) {
                // Phi consuming an alloca's RESULT as a value means
                // the pointer escaped through SSA — disqualify.
                for (auto const& inc : src_.phiIncomings(id)) {
                    if (allocaType.count(inc.value.v)) {
                        nonPromotable.insert(inc.value.v);
                    }
                }
                continue;
            }
            bool const isVolatile = has(src_.instFlags(id), MirInstFlags::Volatile);
            auto const ops = src_.instOperands(id);
            for (std::size_t opi = 0; opi < ops.size(); ++opi) {
                std::uint32_t const v = ops[opi].v;
                if (!allocaType.count(v)) continue;
                bool safe = false;
                if (op == MirOpcode::Load  && opi == 0 && !isVolatile) safe = true;
                if (op == MirOpcode::Store && opi == 1 && !isVolatile) safe = true;
                if (!safe) nonPromotable.insert(v);
            }
        }
    }

    // Final promoted set.
    for (auto const& [aid, ty] : allocaType) {
        if (nonPromotable.count(aid)) continue;
        promoted_.insert(aid);
        allocaElementType_[aid] = ty;
    }
    if (promoted_.empty()) return;

    // Step 3: dom info.
    auto const preds = mirBuildPredecessors(src_);
    auto const dom   = computeMirDomTree(src_, entry, rpo, preds);
    auto const df    = mirDominanceFrontier(src_, dom, preds);
    auto const dchild = mirDomTreeChildren(src_, dom);

    // gaveUp-block gate: an alloca whose def-blocks or IDF members
    // touch a `gaveUp`-flagged block (idom couldn't be resolved)
    // cannot be safely promoted — the frontier under-reports the
    // join set, so Mem2Reg would silently miss a required Phi.
    // The verifier emits `I_VerifierFailure` for the underlying
    // CFG defect; here we refuse to promote rather than miscompile.
    auto touchesGaveUp = [&dom](MirBlockId b) {
        return b.v < dom.gaveUp.size() && dom.gaveUp[b.v] != 0;
    };

    // Step 4: per-promoted-alloca IDF → schedule phi insertions.
    std::unordered_map<std::uint32_t, std::vector<MirBlockId>> defBlocksOf;
    for (MirBlockId const b : rpo) {
        std::uint32_t const ninst = src_.blockInstCount(b);
        for (std::uint32_t i = 0; i < ninst; ++i) {
            MirInstId const id = src_.blockInstAt(b, i);
            MirOpcode const op = src_.instOpcode(id);
            if (op == MirOpcode::Store) {
                auto const ops = src_.instOperands(id);
                if (ops.size() == 2 && promoted_.count(ops[1].v)) {
                    defBlocksOf[ops[1].v].push_back(b);
                }
                continue;
            }
            // Each promoted alloca's own block is also a "def" site
            // for IDF purposes (the alloca itself defines an undef
            // initial value). Without this, an alloca with no Store
            // on every path to a Load would silently skip Phi
            // insertion → uninit-read silent miscompile.
            if (op == MirOpcode::Alloca && promoted_.count(id.v)) {
                defBlocksOf[id.v].push_back(b);
            }
        }
    }
    // Drop allocas that touch gaveUp blocks at any def-site or in their IDF.
    std::unordered_set<std::uint32_t> gateOutByGaveUp;
    for (std::uint32_t aid : promoted_) {
        auto const& defs = defBlocksOf[aid];
        for (MirBlockId const db : defs) {
            if (touchesGaveUp(db)) { gateOutByGaveUp.insert(aid); break; }
        }
        if (gateOutByGaveUp.count(aid)) continue;
        auto const idf = mirIteratedDominanceFrontier(defs, df);
        for (MirBlockId const ib : idf) {
            if (touchesGaveUp(ib)) { gateOutByGaveUp.insert(aid); break; }
        }
    }
    for (std::uint32_t aid : gateOutByGaveUp) {
        promoted_.erase(aid);
        allocaElementType_.erase(aid);
    }
    if (promoted_.empty()) return;
    allocasPromoted_ += promoted_.size();

    for (std::uint32_t aid : promoted_) {
        auto const& defs = defBlocksOf[aid];
        if (defs.empty()) {
            // Step 1 unconditionally adds the alloca's own block as a
            // def for every promoted alloca; reaching here means the
            // scan-order invariant broke. Fail loud.
            std::fprintf(stderr,
                "dss::opt::passes::Mem2Reg fatal: promoted alloca "
                "oldId v=%u has empty defBlocksOf — scan-order "
                "invariant violation (the alloca's own block must "
                "always be a def site).\n", aid);
            std::abort();
        }
        auto const idf = mirIteratedDominanceFrontier(defs, df);
        for (MirBlockId const idfB : idf) {
            PendingPhi pp;
            pp.allocaOldIdV = aid;
            pp.markerV      = nextMarker_++;
            pp.blockOld     = idfB;
            phisByBlock_[idfB.v].push_back(pp);
        }
    }

    // Step 5: rename walk — iterative dom-tree DFS. An explicit
    // stack avoids unbounded recursion on deep dom trees (linear
    // if-ladders, deep nesting); the dom tree's depth is bounded by
    // `blockCount` but the call stack isn't a budget we control.
    std::unordered_map<std::uint32_t, std::vector<ReachingValue>> stacks;
    renameWalkIterative(entry, dchild, stacks);

    // Step 6: every inserted phi must have one incoming per
    // predecessor of its block in the reachable subgraph. A short
    // count is a silent miscompile (the phi joined fewer paths than
    // exist). Fail loud rather than emit a malformed phi.
    for (auto const& [markerV, incomings] : phiIncomings_) {
        // Find the PendingPhi for this marker to learn its block.
        MirBlockId block{};
        std::uint32_t aid = 0;
        for (auto const& [blkV, pphis] : phisByBlock_) {
            for (PendingPhi const& pp : pphis) {
                if (pp.markerV == markerV) { block = pp.blockOld; aid = pp.allocaOldIdV; break; }
            }
            if (block.valid()) break;
        }
        if (!block.valid()) continue;  // dropped during gaveUp gate
        std::size_t expected = 0;
        if (block.v < preds.size()) {
            for (MirBlockId const p : preds[block.v]) {
                if (reachable.count(p.v)) ++expected;
            }
        }
        if (incomings.size() != expected) {
            std::fprintf(stderr,
                "dss::opt::passes::Mem2Reg fatal: phi marker v=%u "
                "for alloca oldId v=%u at block v=%u has %zu incomings "
                "but expects %zu (one per reachable predecessor) — "
                "rename-walk completeness violation.\n",
                markerV, aid, block.v, incomings.size(), expected);
            std::abort();
        }
    }
}

// Iterative dom-tree DFS. The classical Cytron-Ferrante rename is
// usually written recursively (push on enter, pop on return from the
// subtree), but recursion is unbounded by the dom-tree's depth.
// We use a single explicit stack of "frames" — each frame either
// VISITS a block (process insts, push values, queue children) or
// LEAVES a block (pop the snapshot of pushes made at that block).
void Mem2RegPolicy::renameWalkIterative(
    MirBlockId entry,
    std::vector<std::vector<MirBlockId>> const& domChildren,
    std::unordered_map<std::uint32_t, std::vector<ReachingValue>>& stacks) {

    enum class FrameKind : std::uint8_t { Visit, Leave };
    struct Frame {
        FrameKind  kind;
        MirBlockId block;
        std::size_t snapshotMark{};  // index into `snapshots` to roll back to
    };
    // Stack of all pushes (alloca .v, prior stack size). We snapshot
    // the index BEFORE a Visit's pushes and roll back to it on Leave.
    std::vector<std::pair<std::uint32_t, std::size_t>> snapshots;
    std::vector<Frame> work;
    work.push_back({FrameKind::Visit, entry, snapshots.size()});

    auto recordPush = [&](std::uint32_t aid, ReachingValue rv) {
        auto& st = stacks[aid];
        snapshots.emplace_back(aid, st.size());
        st.push_back(rv);
    };

    while (!work.empty()) {
        Frame const f = work.back();
        work.pop_back();

        if (f.kind == FrameKind::Leave) {
            // Roll back every push made on this block's behalf.
            while (snapshots.size() > f.snapshotMark) {
                auto const [aid, sz] = snapshots.back();
                snapshots.pop_back();
                stacks[aid].resize(sz);
            }
            continue;
        }

        // Visit B.
        MirBlockId const B = f.block;
        std::size_t const snapshotMark = snapshots.size();

        // Push inserted Phis for this block onto each promoted
        // alloca's stack — they become the current SSA value until
        // a Store later in the block overrides.
        auto pit = phisByBlock_.find(B.v);
        if (pit != phisByBlock_.end()) {
            for (PendingPhi const& pp : pit->second) {
                ReachingValue rv{ReachingValue::Kind::Phi, pp.markerV};
                recordPush(pp.allocaOldIdV, rv);
            }
        }

        // Walk B's instructions in scan order.
        std::uint32_t const ninst = src_.blockInstCount(B);
        for (std::uint32_t i = 0; i < ninst; ++i) {
            MirInstId const id = src_.blockInstAt(B, i);
            MirOpcode const op = src_.instOpcode(id);
            if (op == MirOpcode::Store) {
                auto const ops = src_.instOperands(id);
                if (ops.size() == 2 && promoted_.count(ops[1].v)) {
                    ReachingValue rv{ReachingValue::Kind::OldInst, ops[0].v};
                    recordPush(ops[1].v, rv);
                }
                continue;
            }
            if (op == MirOpcode::Load) {
                auto const ops = src_.instOperands(id);
                if (ops.size() == 1 && promoted_.count(ops[0].v)) {
                    auto& st = stacks[ops[0].v];
                    if (st.empty()) {
                        std::fprintf(stderr,
                            "dss::opt::passes::Mem2Reg fatal: Load oldId "
                            "v=%u reads promoted alloca oldId v=%u with "
                            "no reaching value (uninitialized read of a "
                            "promotable stack slot — undefined behavior; "
                            "verifier should have flagged this earlier).\n",
                            id.v, ops[0].v);
                        std::abort();
                    }
                    loadReplacement_[id.v] = st.back();
                }
                continue;
            }
            // Non-Load/Store insts: the rebuilder's rewriteOperand path
            // handles their operand remapping — the rename walk only
            // manipulates the alloca-value stacks.
        }

        // Successor phi-incoming fill: each phi scheduled at a
        // successor gets (top-of-stack, B) for each of its promoted
        // allocas.
        auto const succs = src_.blockSuccessors(B);
        for (MirBlockId const S : succs) {
            auto sit = phisByBlock_.find(S.v);
            if (sit == phisByBlock_.end()) continue;
            for (PendingPhi const& pp : sit->second) {
                auto stIt = stacks.find(pp.allocaOldIdV);
                if (stIt == stacks.end() || stIt->second.empty()) {
                    std::fprintf(stderr,
                        "dss::opt::passes::Mem2Reg fatal: phi at block "
                        "v=%u for alloca oldId v=%u has no reaching value "
                        "from predecessor block v=%u (uninit / unreachable "
                        "input — semantic violation).\n",
                        S.v, pp.allocaOldIdV, B.v);
                    std::abort();
                }
                PendingIncoming inc;
                inc.value   = stIt->second.back();
                inc.predOld = B;
                phiIncomings_[pp.markerV].push_back(inc);
            }
        }

        // Queue: leave THIS block AFTER all dom-tree children are
        // visited. Push Leave first so it sits below the children
        // on the LIFO stack; push children in reverse so the first
        // child is processed first (visit order matches recursion).
        work.push_back({FrameKind::Leave, B, snapshotMark});
        if (B.v < domChildren.size()) {
            auto const& kids = domChildren[B.v];
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                work.push_back({FrameKind::Visit, *it, snapshots.size()});
            }
        }
    }
}

} // namespace

Mem2RegResult runMem2Reg(Mir& mir, TypeInterner const& interner,
                         DiagnosticReporter& reporter) {
    Mem2RegResult result{};
    MirBuilder builder;

    // Runtime-init globals carve-out (D-OPT2-CONST-FOLD-RUNTIME-INIT-
    // GLOBALS shared anchor). Same shape as ConstFold + DCE.
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        if (mir.globalInitFunc(mir.globalAt(i)).valid()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::X_OptPassSkipped;
            d.severity = DiagnosticSeverity::Info;
            d.actual   = "opt::Mem2Reg: skipped — module has >= 1 "
                         "runtime-init global; func-id remap not yet "
                         "implemented (D-OPT2-CONST-FOLD-RUNTIME-INIT-"
                         "GLOBALS — Mem2Reg shares the same carve-out).";
            reporter.report(std::move(d));
            result.ok = true;
            return result;
        }
    }

    // Clone globals before any function (same discipline as ConstFold).
    for (std::uint32_t i = 0; i < ng; ++i) {
        MirGlobalId const g = mir.globalAt(i);
        std::uint32_t const initIdx = mir.globalInitLiteralIndex(g);
        std::uint32_t newInitIdx = UINT32_MAX;
        if (initIdx != UINT32_MAX) {
            newInitIdx = builder.literalPoolAdd(mir.literalValue(initIdx));
        }
        builder.addGlobal(mir.globalType(g), mir.globalSymbol(g),
                          newInitIdx, MirFuncId{},
                          mir.globalBinding(g), mir.globalVisibility(g));
    }

    // Per-function: analyze → rebuild → finalize phi incomings.
    Mem2RegPolicy policy{mir, interner};
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        policy.analyze(f);
        MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(f);
        policy.finalizePhiIncomings(builder, rb.blockMap(), rb.rewriteMap());
    }

    result.allocasPromoted   = policy.allocasPromoted();
    result.phisInserted      = policy.phisInserted();
    result.loadsReplaced     = policy.loadsReplaced();
    result.storesEliminated  = policy.storesEliminated();

    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
