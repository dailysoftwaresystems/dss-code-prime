#include "opt/passes/copy_prop.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss::opt::passes {

namespace {

class CopyPropPolicy final : public MirRebuildPolicy {
public:
    explicit CopyPropPolicy(Mir const& src) : src_(src) {}

    [[nodiscard]] std::size_t phisCollapsed() const noexcept {
        return phisCollapsed_;
    }

    void analyze(MirFuncId fn);

    // RPO block order so SSA's def-dominates-use invariant translates
    // to "def emitted before user in scan order" — required for the
    // `substituteOldOperand` → `rewriteOperand` chain to find the
    // collapse target's new id in the rewrite map at lookup time.
    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        return mirReversePostOrder(src, src.funcEntry(fn));
    }

    [[nodiscard]] MirInstId substituteOldOperand(MirInstId oldOp) override {
        auto it = collapsed_.find(oldOp);
        if (it == collapsed_.end()) return oldOp;
        return it->second;
    }

    // Reset per-function analysis state between functions in the
    // same module. Counters accumulate.
    void resetPerFunction() {
        collapsed_.clear();
    }

private:
    [[nodiscard]] MirInstId resolveTransitive(MirInstId v) const {
        // Walk the collapse chain v → collapsed[v] → ... bounded by
        // the map size (a cycle would mean we collapsed a Phi to
        // itself, which is filtered upstream). Fail loud on overrun.
        std::uint32_t cap = static_cast<std::uint32_t>(collapsed_.size()) + 1;
        while (cap-- > 0) {
            auto it = collapsed_.find(v);
            if (it == collapsed_.end()) return v;
            v = it->second;
        }
        std::fprintf(stderr,
            "dss::opt::passes::CopyProp fatal: resolveTransitive "
            "exceeded chain length walking from v=%u — collapse map "
            "contains a cycle (self-reference filter upstream broke).\n",
            v.v);
        std::abort();
    }

    Mir const& src_;
    // Old-id → old-id collapse map. Path-compressed after analysis
    // so a lookup is O(1). Cleared between functions; counters live.
    std::unordered_map<MirInstId, MirInstId> collapsed_;
    std::size_t phisCollapsed_ = 0;
};

void CopyPropPolicy::analyze(MirFuncId fn) {
    resetPerFunction();

    // Step 1: collect every Phi in the function + build the reverse-
    // use map (which Phis incoming-from each old value). The reverse
    // map drives worklist re-evaluation when a Phi collapses — any
    // Phi that had P as an incoming may now have one fewer distinct
    // value and become collapsible too.
    std::vector<MirInstId> allPhis;
    std::uint32_t const blockCount = src_.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        MirBlockId const b = src_.funcBlockAt(fn, bi);
        std::uint32_t const ninst = src_.blockInstCount(b);
        for (std::uint32_t i = 0; i < ninst; ++i) {
            MirInstId const id = src_.blockInstAt(b, i);
            if (src_.instOpcode(id) == MirOpcode::Phi) {
                allPhis.push_back(id);
            }
        }
    }
    if (allPhis.empty()) return;

    std::unordered_map<MirInstId, std::vector<MirInstId>> phiUsers;
    for (MirInstId const P : allPhis) {
        for (auto const& inc : src_.phiIncomings(P)) {
            phiUsers[inc.value].push_back(P);
        }
    }

    // Step 2: worklist iteration. A Phi collapses iff its set of
    // distinct non-self incomings (after transitive resolution
    // through the in-progress collapse map) has exactly one element.
    std::deque<MirInstId> worklist(allPhis.begin(), allPhis.end());
    while (!worklist.empty()) {
        MirInstId const P = worklist.front();
        worklist.pop_front();
        if (collapsed_.count(P)) continue;

        std::unordered_set<MirInstId> distinct;
        for (auto const& inc : src_.phiIncomings(P)) {
            MirInstId const v = resolveTransitive(inc.value);
            if (v.v == P.v) continue;  // SSA self-reference (back-edge)
            distinct.insert(v);
            if (distinct.size() > 1) break;  // early exit — not collapsible
        }

        // distinct.size() == 0: every incoming transitively resolves
        // to P itself — a dead self-referential Phi (e.g. mutual-Phi
        // cycle P1↔P2 with no external in-edge). Leave it uncollapsed
        // so DCE sweeps it as zero-use SSA, NOT collapse-to-self.
        // distinct.size() > 1: real merge — preserve the Phi.
        if (distinct.size() != 1) continue;
        MirInstId const target = *distinct.begin();
        collapsed_[P] = target;

        auto it = phiUsers.find(P);
        if (it == phiUsers.end()) continue;
        for (MirInstId const Q : it->second) {
            if (!collapsed_.count(Q)) worklist.push_back(Q);
        }
    }

    // Step 3: path-compress so every collapse-target is a
    // non-collapsed value. After compression, no target may itself
    // be a collapse-map key — the compression terminates at a
    // non-collapsed value. Verify the invariant: if any target
    // still appears as a key, the substitution chain at rebuild
    // time would yield the wrong NEW id (rewriteOperand would
    // succeed on the intermediate Phi's new id rather than the
    // final value). Fail loud rather than silently miscompile.
    for (auto& [p, t] : collapsed_) {
        t = resolveTransitive(t);
    }
    for (auto const& [p, t] : collapsed_) {
        if (collapsed_.count(t)) {
            std::fprintf(stderr,
                "dss::opt::passes::CopyProp fatal: post-compression "
                "invariant broken — target v=%u of Phi v=%u is itself "
                "a collapse-map key. resolveTransitive should have "
                "terminated at a non-collapsed value.\n", t.v, p.v);
            std::abort();
        }
    }

    phisCollapsed_ += collapsed_.size();
}

} // namespace

CopyPropResult runCopyProp(Mir& mir, TypeInterner const& /*interner*/,
                           DiagnosticReporter& reporter) {
    CopyPropResult result{};
    MirBuilder builder;

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, "CopyProp")
        == GlobalClonePrelude::CarvedOut) {
        result.ok = true;
        return result;
    }

    CopyPropPolicy policy{mir};
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        policy.analyze(f);
        MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(f);
    }

    result.phisCollapsed = policy.phisCollapsed();
    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
