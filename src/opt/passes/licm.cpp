#include "opt/passes/licm.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss::opt::passes {

namespace {

using dss::opt::analysis::StrictTbaa;
using dss::opt::analysis::mirAnyMayAliasingStoreInLoop;

// Trap-eligible opcodes: divisions and modulo may raise at runtime
// (#DE on x86 for IDIV with divisor=0 / quotient overflow; similar
// on ARM under specific cores). Hoisting these out of a loop whose
// trip count could be zero would execute the trap-eligible op
// UNCONDITIONALLY, changing observable program behavior. Until
// interval / value-range analysis proves the divisor is non-zero,
// these stay in the loop body (D-OPT6-LICM-TRAP-SAFE-HOIST).
[[nodiscard]] bool isTrapEligible(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::SDiv: case MirOpcode::UDiv:
        case MirOpcode::SMod: case MirOpcode::UMod:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool isLicmCandidateOpcode(MirOpcode op) noexcept {
    if (isTerminator(op)) return false;
    if (isPhi(op)) return false;
    if (opcodeInfo(op).hasSideEffects) return false;
    // Load admission (cycle 10b): Load IS a hoist candidate now. The
    // analyze() loop additionally gates each Load via
    // `mirAnyMayAliasingStoreInLoop` — if no may-aliasing Store sits
    // in the loop body, the Load is loop-invariant in the alias sense.
    if (isTrapEligible(op)) return false;     // D-OPT6-LICM-TRAP-SAFE-HOIST
    // Leaf opcodes (zero-operand value origins) have dedicated
    // builders on MirBuilder + carry no runtime computation worth
    // hoisting. Excluding them keeps the hoist emit-loop generic
    // (one `addInst` shape) and matches the user-visible semantics:
    // a Const isn't computed at runtime; relocating it has no
    // measurable benefit and would duplicate the rebuilder's
    // dedicated-builder dispatch logic at the LICM tier.
    if (op == MirOpcode::Const || op == MirOpcode::Arg
     || op == MirOpcode::GlobalAddr) return false;
    return true;
}

class LicmPolicy final : public MirRebuildPolicy {
public:
    LicmPolicy(Mir const& src, TypeInterner const& interner) noexcept
        : src_(src), interner_(interner),
          strictTbaa_(src.aliasingMode() == MirAliasingMode::StrictTBAA
                      ? StrictTbaa::Yes : StrictTbaa::No) {}

    [[nodiscard]] std::size_t instructionsHoisted() const noexcept {
        return instructionsHoisted_;
    }

    void analyze(MirFuncId fn);

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        // RPO so a hoisted inst's operands (loop-external defs) are
        // in `rewrite_` by the time the preheader's
        // `onBlockBeforeTerminator` fires — operands of a loop-
        // invariant inst are defined OUTSIDE the loop, i.e. in
        // dom-ancestors of the preheader, which precede the
        // preheader in any RPO walk.
        return mirReversePostOrder(src, src.funcEntry(fn));
    }

    [[nodiscard]] bool shouldEmit(MirInstId oldId) override {
        // Hoisted insts are omitted from their ORIGINAL block;
        // they will be re-emitted in the preheader via
        // `onBlockBeforeTerminator`.
        return hoistedInsts_.count(oldId) == 0;
    }

    void onBlockBeforeTerminator(
        MirBlockId oldB, MirBlockId newB,
        MirBuilder& dst,
        std::unordered_map<std::uint32_t, MirInstId>& rewrite,
        std::unordered_map<std::uint32_t, MirBlockId> const& /*blockMap*/) override {
        (void)newB;
        auto it = hoistPlan_.find(oldB);
        if (it == hoistPlan_.end()) return;
        for (MirInstId const oldX : it->second) {
            // Emit a clone of oldX into the open preheader. Each
            // operand resolves through `rewrite` — guaranteed to be
            // populated since LICM only hoists insts whose operands
            // are defined OUTSIDE the loop body (in dom-ancestors
            // of this preheader → emitted earlier in RPO).
            MirOpcode const op = src_.instOpcode(oldX);
            auto const oldOps = src_.instOperands(oldX);
            std::vector<MirInstId> newOps;
            newOps.reserve(oldOps.size());
            for (MirInstId const o : oldOps) {
                auto rit = rewrite.find(o.v);
                if (rit == rewrite.end()) {
                    std::fprintf(stderr,
                        "dss::opt::passes::Licm fatal: hoisting old "
                        "inst v=%u from loop body but operand v=%u "
                        "is not in rewrite map — analysis-tier "
                        "eligibility check missed a chained-invariant "
                        "operand (D-OPT6-LICM-CHAINED-INVARIANTS).\n",
                        oldX.v, o.v);
                    std::abort();
                }
                newOps.push_back(rit->second);
            }
            MirInstId const newId = dst.addInst(op, newOps,
                                                src_.instType(oldX),
                                                src_.instPayload(oldX),
                                                src_.instFlags(oldX));
            rewrite.emplace(oldX.v, newId);
            ++instructionsHoisted_;
        }
    }

    void resetPerFunction() {
        hoistedInsts_.clear();
        hoistPlan_.clear();
    }

private:
    // Single chokepoint for the parallel-container coherence
    // invariant (`id ∈ hoistedInsts_ ⟺ ∃preheader: id ∈
    // hoistPlan_[preheader]`). A future maintainer adding a second
    // insertion site would face a deliberate one-call refactor
    // rather than risking silent drift between `shouldEmit`'s skip
    // and `onBlockBeforeTerminator`'s emit (an inst in
    // `hoistedInsts_` but not `hoistPlan_` → deleted, not hoisted →
    // miscompile).
    void recordHoist(MirInstId id, MirBlockId preheader) {
        auto [it, inserted] = hoistedInsts_.insert(id);
        if (!inserted) {
            std::fprintf(stderr,
                "dss::opt::passes::Licm fatal: inst oldId v=%u "
                "already hoisted; second recordHoist call would "
                "produce divergent (set, plan) state — analyze() "
                "must visit each candidate at most once.\n", id.v);
            std::abort();
        }
        hoistPlan_[preheader].push_back(id);
    }

    Mir const&          src_;
    TypeInterner const& interner_;
    StrictTbaa const    strictTbaa_;

    // Per-function analysis state. Counters live across functions.
    std::unordered_set<MirInstId> hoistedInsts_;                          // body-side: skip via shouldEmit
    std::unordered_map<MirBlockId, std::vector<MirInstId>> hoistPlan_;    // preheader-side: emit-list, in body scan order

    std::size_t instructionsHoisted_ = 0;
};

void LicmPolicy::analyze(MirFuncId fn) {
    resetPerFunction();
    MirBlockId const entry = src_.funcEntry(fn);
    auto const rpo = mirReversePostOrder(src_, entry);
    if (rpo.empty()) return;

    auto const preds = mirBuildPredecessors(src_);
    auto const dom   = computeMirDomTree(src_, entry, rpo, preds);
    auto const loops = mirNaturalLoops(src_, dom, preds);
    if (loops.empty()) return;

    // For each loop: locate the unique non-back-edge predecessor of
    // the header (the preheader) if it exists; otherwise skip the
    // loop entirely (preheader insertion is deferred).
    for (MirNaturalLoop const& loop : loops) {
        std::unordered_set<std::uint32_t> bodySet;
        bodySet.reserve(loop.body.size());
        for (MirBlockId const b : loop.body) bodySet.insert(b.v);

        // Preheader: the header's predecessor that's NOT in the loop
        // body. Must be exactly one for c1's scope.
        MirBlockId preheader{};
        bool ambiguous = false;
        if (loop.header.v >= preds.size()) continue;
        for (MirBlockId const p : preds[loop.header.v]) {
            if (bodySet.count(p.v)) continue;  // back-edge predecessor
            if (preheader.valid()) { ambiguous = true; break; }
            preheader = p;
        }
        if (ambiguous || !preheader.valid()) continue;

        // For each inst in the loop body (skip the header's Phis
        // implicitly since Phi isn't a candidate opcode):
        //   - Eligibility: opcode + flags.
        //   - All operands defined OUTSIDE the loop body.
        for (MirBlockId const b : loop.body) {
            std::uint32_t const ninst = src_.blockInstCount(b);
            for (std::uint32_t i = 0; i < ninst; ++i) {
                MirInstId const id = src_.blockInstAt(b, i);
                MirOpcode const op = src_.instOpcode(id);
                if (!isLicmCandidateOpcode(op)) continue;
                if (has(src_.instFlags(id), MirInstFlags::Volatile)) continue;

                // All-operands-outside-loop check. The operand's
                // block is `src_.instBlock(operand)`; if it's IN
                // the loop body, the inst is NOT a c1 hoist target
                // (chained invariants are anchored for a future
                // cycle).
                bool allOutside = true;
                for (MirInstId const o : src_.instOperands(id)) {
                    MirBlockId const opBlock = src_.instBlock(o);
                    // Defensive: an invalid operand-block would
                    // suggest a malformed source MIR (verifier should
                    // have caught it); treat as not-hoistable so
                    // LICM emits a no-op rather than silently
                    // claiming "outside" for an unresolved operand.
                    if (!opBlock.valid()) { allOutside = false; break; }
                    if (bodySet.count(opBlock.v)) {
                        allOutside = false;
                        break;
                    }
                }
                if (!allOutside) continue;
                // Load admission gate: a Load is hoist-eligible only
                // when no Store in the loop body may alias its pointer.
                // The pointer-operand-defined-outside check above is a
                // necessary but not sufficient condition; a Store
                // whose target may alias the loaded location would
                // make the Load's value loop-variant.
                if (op == MirOpcode::Load) {
                    auto const lops = src_.instOperands(id);
                    if (lops.empty()) {
                        std::fprintf(stderr,
                            "dss::opt::passes::Licm fatal: Load inst "
                            "v=%u has zero operands — verifier-contract "
                            "violation.\n", id.v);
                        std::abort();
                    }
                    if (mirAnyMayAliasingStoreInLoop(
                            src_, interner_, lops[0], loop.body,
                            strictTbaa_)) {
                        continue;  // clobbered in body
                    }
                }
                // Nested-loop dedup (CRITICAL fix): an inst that's
                // invariant in BOTH an inner and an outer enclosing
                // loop (e.g. operands all in function entry) would
                // appear as a candidate in BOTH loops. The first
                // record wins; subsequent visits skip — this prevents
                // `recordHoist`'s duplicate-guard from firing on
                // legitimate nested-loop input. The loop iteration
                // order (sorted by header.v) typically hits the
                // outer loop first, hoisting to the OUTERMOST valid
                // preheader (deepest hoist).
                if (hoistedInsts_.count(id)) continue;
                recordHoist(id, preheader);
            }
        }
    }
}

} // namespace

LicmResult runLicm(Mir& mir, TypeInterner const& interner,
                   DiagnosticReporter& reporter) {
    LicmResult result{};
    MirBuilder builder;

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, "Licm")
        == GlobalClonePrelude::CarvedOut) {
        result.ok = true;
        return result;
    }

    LicmPolicy policy{mir, interner};
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        policy.analyze(f);
        MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(f);
    }

    result.instructionsHoisted = policy.instructionsHoisted();
    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
