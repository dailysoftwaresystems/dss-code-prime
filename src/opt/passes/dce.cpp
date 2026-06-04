#include "opt/passes/dce.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss::opt::passes {

namespace {

// True iff this instruction must be preserved regardless of result
// usage. Covers `hasSideEffects=true` opcodes (Store/Alloca/Call/
// IntrinsicCall + terminators) AND the explicit Volatile flag on
// memory ops (the optimizer must not reorder or elide volatile
// accesses). Terminators are caught by `hasSideEffects` per the
// opcode table.
[[nodiscard]] bool isSideEffectRoot(MirOpcode op, MirInstFlags flags) noexcept {
    if (opcodeInfo(op).hasSideEffects) return true;
    if (has(flags, MirInstFlags::Volatile)) return true;
    return false;
}

// True iff a function / global must survive DCE because it is
// observable from outside the compilation unit. Reads the linkage
// attributes threaded by D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD.
[[nodiscard]] bool funcIsRoot(Mir const& mir, MirFuncId f) noexcept {
    return isExternallyVisible(mir.funcBinding(f), mir.funcVisibility(f));
}
[[nodiscard]] bool globalIsRoot(Mir const& mir, MirGlobalId g) noexcept {
    return isExternallyVisible(mir.globalBinding(g), mir.globalVisibility(g));
}

// Per-function live-instruction analysis: returns the set of OLD-
// module instruction-slot ids (.v) that must be preserved within
// the given reachable-block list. Free function — stateless;
// invariant is "blocks are CFG-reachable from the function entry."
[[nodiscard]] std::unordered_set<std::uint32_t>
computeLiveInsts(Mir const& mir,
                 std::vector<MirBlockId> const& reachable) {
    // Reachable-block set as a flat hash for O(1) predecessor lookup
    // (used to filter phi incomings — see below).
    std::unordered_set<std::uint32_t> reachableSet;
    reachableSet.reserve(reachable.size());
    for (MirBlockId const b : reachable) reachableSet.insert(b.v);

    std::unordered_set<std::uint32_t> live;
    std::deque<MirInstId> worklist;

    // Seed: every side-effect / terminator / volatile instruction
    // in a reachable block. Phi instructions are not seeded directly
    // — they survive via the operand walk from any live consumer.
    for (MirBlockId const b : reachable) {
        std::uint32_t const n = mir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (isSideEffectRoot(mir.instOpcode(id), mir.instFlags(id))) {
                if (live.insert(id.v).second) worklist.push_back(id);
            }
        }
    }

    // BFS backward over operands (or phi incomings for Phi nodes).
    // For phi: intersect incomings with the reachable predecessor
    // set — Phase-3 rebuild also drops unreachable-pred incomings,
    // so marking their value live would over-approximate the live
    // set (latent risk if anything other than the rebuild ever
    // consumes `liveInsts`).
    while (!worklist.empty()) {
        MirInstId const id = worklist.front();
        worklist.pop_front();
        MirOpcode const op = mir.instOpcode(id);
        if (op == MirOpcode::Phi) {
            for (auto const& inc : mir.phiIncomings(id)) {
                if (!reachableSet.count(inc.pred.v)) continue;
                if (live.insert(inc.value.v).second) {
                    worklist.push_back(inc.value);
                }
            }
        } else {
            for (MirInstId const operand : mir.instOperands(id)) {
                if (live.insert(operand.v).second) {
                    worklist.push_back(operand);
                }
            }
        }
    }
    return live;
}

// Per-function analysis cache — reachable blocks + live insts.
// Computed once per live function in `LiveSymbolScanner` and reused
// by the FunctionRebuilder. Pre-fold this was computed twice per
// function (once for liveSymbols, once again for the rebuild) — a
// silent O(N) duplication AND a correctness-drift risk if the seed
// logic diverged between the two computations.
struct FuncLiveSet {
    std::vector<MirBlockId>           reachable;
    std::unordered_set<std::uint32_t> liveInsts;
};

// Inter-procedural live-symbol BFS. Seeds with externally-visible
// functions + globals; expands via live `GlobalAddr` instructions in
// live functions. Returns the SymbolId.v values that MUST be preserved
// AND a per-live-function `FuncLiveSet` so the rebuilder can reuse
// the reachable/liveInsts pair (no second scan).
struct SymbolScanResult {
    std::unordered_set<std::uint32_t> liveSymbols;
    std::unordered_map<std::uint32_t, FuncLiveSet> perFunc;  // keyed by MirFuncId.v
};

[[nodiscard]] SymbolScanResult scanLiveSymbols(Mir const& mir) {
    SymbolScanResult out;
    std::deque<MirFuncId> funcWorklist;

    // SymbolId.v → MirFuncId map for BFS expansion. MirBuilder
    // admits multiple functions with the same SymbolId (weak
    // aliases, COMDAT, hand-built test fixtures) AND admits
    // symbol.v == 0 (anonymous / synthetic thunks). Both shapes
    // would silently miscompile here: `emplace` drops the second
    // entry → only the first is dispatched by the BFS → subsequent
    // functions with the same symbol enter `liveSymbols` (via the
    // shared symbol) but never get a `perFunc` entry → step 3's
    // fail-loud fires with a misleading "scanLiveSymbols invariant"
    // message. Fail loud HERE so the user/test author sees the
    // actual root cause (input MIR has aliasing symbols).
    std::unordered_map<std::uint32_t, MirFuncId> symToFunc;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const sv = mir.funcSymbol(f).v;
        if (sv == 0) {
            std::fprintf(stderr,
                "dss::opt::passes::scanLiveSymbols fatal: function "
                "funcId v=%u has SymbolId v=0 (anonymous / synthetic) — "
                "DCE's symbol-keyed BFS cannot disambiguate. Caller "
                "must assign a unique non-zero SymbolId before invoking "
                "the optimizer (D-OPT3-DCE-ANONYMOUS-SYMBOL).\n", f.v);
            std::abort();
        }
        auto const [it, inserted] = symToFunc.emplace(sv, f);
        if (!inserted) {
            std::fprintf(stderr,
                "dss::opt::passes::scanLiveSymbols fatal: SymbolId v=%u "
                "appears on multiple functions (funcId v=%u and v=%u) — "
                "DCE's inter-procedural BFS requires unique SymbolIds. "
                "Weak/COMDAT-style aliasing is not supported in this cycle "
                "(D-OPT3-DCE-DUPLICATE-SYMBOL).\n",
                sv, it->second.v, f.v);
            std::abort();
        }
    }

    // Phase 1: seed with externally-visible roots.
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (funcIsRoot(mir, f)) {
            if (out.liveSymbols.insert(mir.funcSymbol(f).v).second) {
                funcWorklist.push_back(f);
            }
        }
    }
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        MirGlobalId const g = mir.globalAt(i);
        if (globalIsRoot(mir, g)) {
            out.liveSymbols.insert(mir.globalSymbol(g).v);
        }
    }

    // Phase 2: BFS — for each live function, compute its live-set
    // ONCE + scan reachable blocks for live GlobalAddr referrals.
    while (!funcWorklist.empty()) {
        MirFuncId const f = funcWorklist.front();
        funcWorklist.pop_front();

        FuncLiveSet lset;
        lset.reachable = mirReversePostOrder(mir, mir.funcEntry(f));
        lset.liveInsts = computeLiveInsts(mir, lset.reachable);

        for (MirBlockId const b : lset.reachable) {
            std::uint32_t const n = mir.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const id = mir.blockInstAt(b, i);
                if (mir.instOpcode(id) != MirOpcode::GlobalAddr) continue;
                if (!lset.liveInsts.count(id.v)) continue;
                SymbolId const sym = mir.globalAddrSymbol(id);
                if (!out.liveSymbols.insert(sym.v).second) continue;
                auto it = symToFunc.find(sym.v);
                if (it != symToFunc.end()) {
                    funcWorklist.push_back(it->second);
                }
            }
        }
        out.perFunc.emplace(f.v, std::move(lset));
    }
    return out;
}

// DCE rebuild policy: filters dead non-Phi value insts via the
// shared MirFunctionRebuilder's `shouldEmit` hook + filters
// unreachable-pred phi incomings via `acceptPhiIncoming`. Terminator
// results are not recorded in the rewrite map (the original DCE
// convention — terminator results are never read as operands so the
// hashmap inserts are pure overhead).
class DcePolicy : public MirRebuildPolicy {
public:
    DcePolicy(std::vector<MirBlockId> const& reachable,
              std::unordered_set<std::uint32_t> const& liveInsts)
        : reachable_(reachable), liveInsts_(liveInsts) {}

    [[nodiscard]] std::size_t instructionsEliminated() const noexcept {
        return eliminated_;
    }

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& /*src*/, MirFuncId /*fn*/) override {
        return reachable_;
    }

    [[nodiscard]] bool shouldEmit(MirInstId oldId) override {
        if (liveInsts_.count(oldId.v) != 0) return true;
        ++eliminated_;
        return false;
    }

    [[nodiscard]] bool acceptPhiIncoming(
        MirPhiIncoming const& inc,
        std::unordered_map<std::uint32_t, MirBlockId> const& blockMap) override {
        return blockMap.count(inc.pred.v) != 0;
    }

    [[nodiscard]] bool recordTerminatorInRewrite() const noexcept override {
        return false;
    }

private:
    std::vector<MirBlockId> const&           reachable_;
    std::unordered_set<std::uint32_t> const& liveInsts_;
    std::size_t                              eliminated_ = 0;
};

} // namespace

DceResult runDce(Mir& mir, TypeInterner const& /*interner*/,
                 DiagnosticReporter& reporter) {
    DceResult result{};
    MirBuilder builder;
    // DCE bypasses `cloneGlobalsOrCarveOut` (see comment below); we
    // duplicate that helper's `setAliasingMode` propagation here so the
    // fixed-point pipeline loop doesn't silently downgrade strict-TBAA
    // to Permissive on the iteration following DCE
    // (D-OPT-LOAD-ALIAS-ANALYSIS-PIPELINE-PROPAGATE).
    builder.setAliasingMode(mir.aliasingMode());

    // DCE has the same runtime-init carve-out as the other passes but
    // CANNOT use the shared `cloneGlobalsOrCarveOut` helper: DCE elides
    // dead globals based on the live-symbol BFS result, so its global-
    // clone loop is filtered. The carve-out itself remains shared.
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        if (mir.globalInitFunc(mir.globalAt(i)).valid()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::X_OptPassSkipped;
            d.severity = DiagnosticSeverity::Info;
            d.actual   = "opt::Dce: skipped — module has >= 1 runtime-init "
                         "global; func-id remap not yet implemented "
                         "(D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS).";
            reporter.report(std::move(d));
            result.ok = true;
            return result;
        }
    }

    // Step 1: inter-procedural live-symbol BFS — also returns per-
    // function (reachable, liveInsts) so the rebuild reuses them
    // (the scanner computes them en route).
    auto const symScan = scanLiveSymbols(mir);
    auto const& liveSymbols = symScan.liveSymbols;
    for (std::uint32_t i = 0; i < ng; ++i) {
        MirGlobalId const g = mir.globalAt(i);
        if (!liveSymbols.count(mir.globalSymbol(g).v)) {
            ++result.globalsEliminated;
            continue;
        }
        std::uint32_t const initIdx = mir.globalInitLiteralIndex(g);
        std::uint32_t newInitIdx = UINT32_MAX;
        if (initIdx != UINT32_MAX) {
            newInitIdx = builder.literalPoolAdd(mir.literalValue(initIdx));
        }
        builder.addGlobal(mir.globalType(g), mir.globalSymbol(g),
                          newInitIdx, MirFuncId{},
                          mir.globalBinding(g), mir.globalVisibility(g));
    }

    // Step 3: walk each function. Dead functions are simply NOT
    // cloned. Live functions reuse the per-function (reachable,
    // liveInsts) computed during the symbol BFS — no second scan.
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (!liveSymbols.count(mir.funcSymbol(f).v)) {
            // Whole-function elimination: count this function AND
            // every one of its blocks so `blocksEliminated` reflects
            // the total elision (not only blocks dropped from
            // surviving functions). Pre-fold the counter undercounted
            // — a consumer ratioing block-elimination against total
            // blocks would have seen misleading data on modules where
            // DCE drops entire functions.
            ++result.functionsEliminated;
            result.blocksEliminated += mir.funcBlockCount(f);
            continue;
        }
        auto const it = symScan.perFunc.find(f.v);
        if (it == symScan.perFunc.end()) {
            // A live function with no per-function live-set entry —
            // scanLiveSymbols was supposed to populate perFunc[f.v]
            // for every function whose symbol entered liveSymbols.
            // Anonymous (symbol.v=0) and duplicate-symbol cases now
            // fail loud INSIDE scanLiveSymbols (see D-OPT3-DCE-
            // ANONYMOUS-SYMBOL + D-OPT3-DCE-DUPLICATE-SYMBOL), so
            // reaching this site means scanLiveSymbols dropped a
            // function on a path other than those two.
            std::fprintf(stderr,
                "dss::opt::passes::runDce fatal: liveSymbols includes "
                "fn symbol %u but perFunc has no entry for funcId v=%u — "
                "scanLiveSymbols BFS invariant violation (every live "
                "function-symbol must be visited).\n",
                mir.funcSymbol(f).v, f.v);
            std::abort();
        }
        // `funcBlockCount` is per-function, sentinel-excluded;
        // `lset.reachable` is the surviving CFG-reachable set.
        FuncLiveSet const& lset = it->second;
        std::uint32_t const oldBlockCount = mir.funcBlockCount(f);
        if (lset.reachable.size() < oldBlockCount) {
            result.blocksEliminated += (oldBlockCount - lset.reachable.size());
        }
        DcePolicy policy{lset.reachable, lset.liveInsts};
        MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(f);
        result.instructionsEliminated += policy.instructionsEliminated();
    }

    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
