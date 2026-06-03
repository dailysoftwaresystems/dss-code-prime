#include "opt/passes/dce.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

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

// Per-function rebuilder. Uses the precomputed liveInsts + reachable
// block set + the rewrite map keyed on OLD-module inst-slot (.v) —
// same shape as ConstFold.
class FunctionRebuilder {
public:
    FunctionRebuilder(Mir const& src, MirBuilder& dst,
                      std::vector<MirBlockId> const& reachable,
                      std::unordered_set<std::uint32_t> const& liveInsts)
        : src_(src), dst_(dst), reachable_(reachable), liveInsts_(liveInsts) {}

    [[nodiscard]] std::size_t instructionsEliminated() const noexcept {
        return instructionsEliminated_;
    }

    void rebuildFunction(MirFuncId oldFn) {
        dst_.addFunction(src_.funcSignature(oldFn), src_.funcSymbol(oldFn),
                         src_.funcBinding(oldFn), src_.funcVisibility(oldFn));

        // Phase 1: pre-create every CFG-reachable block. Forward
        // branches (terminators targeting later blocks) need the
        // target ids before phase 2's terminator emits.
        std::unordered_map<std::uint32_t, MirBlockId> blockMap;
        blockMap.reserve(reachable_.size());
        for (MirBlockId const oldB : reachable_) {
            MirBlockId const newB = dst_.createBlock(src_.blockMarker(oldB));
            blockMap.emplace(oldB.v, newB);
        }

        // Phase 2: fill each block. Phi incomings deferred to phase 3.
        struct DeferredPhi {
            MirInstId oldPhi;
            MirInstId newPhi;
        };
        std::vector<DeferredPhi> deferredPhis;

        for (MirBlockId const oldB : reachable_) {
            MirBlockId const newB = blockMap.at(oldB.v);
            dst_.beginBlock(newB);

            std::uint32_t const ninst = src_.blockInstCount(oldB);
            for (std::uint32_t i = 0; i < ninst; ++i) {
                MirInstId const oldId = src_.blockInstAt(oldB, i);
                MirOpcode const op    = src_.instOpcode(oldId);

                // Terminator: always preserved (a block with no
                // terminator is malformed). Operands are remapped;
                // unreachable successor edges (which mirReversePostOrder
                // excluded by definition) cannot appear on a reachable
                // block's terminator AT THE OLD MODULE — the OLD CFG
                // is whatever lowerToMir built, fully reachable by
                // construction.
                if (opcodeInfo(op).isTerminator) {
                    emitTerminator(op, oldId, blockMap);
                    continue;
                }

                // Skip non-side-effect instruction whose result is
                // unused — operand-graph reachability missed it.
                if (!liveInsts_.count(oldId.v)) {
                    ++instructionsEliminated_;
                    continue;
                }

                if (op == MirOpcode::Phi) {
                    MirInstId const newPhi = dst_.addPhi(src_.instType(oldId));
                    rewrite_.emplace(oldId.v, newPhi);
                    deferredPhis.push_back({oldId, newPhi});
                    continue;
                }
                emitValue(op, oldId);
            }
        }

        // Phase 3: flush phi incomings. The rewrite map is now
        // complete (every reachable + live inst has an entry); back-
        // edge values resolve here. A phi at a reachable block CAN
        // legitimately have an incoming from an UNREACHABLE predecessor
        // (the verifier admits this — `mir_verifier.cpp` tolerates
        // rpoIndex==sentinel). Such incomings have no remapped block
        // (they were excluded from blockMap by RPO) — skip them. A
        // phi left with ZERO incomings is a structural violation;
        // fail loud.
        for (auto const& dp : deferredPhis) {
            std::size_t kept = 0;
            for (auto const& inc : src_.phiIncomings(dp.oldPhi)) {
                auto const predIt = blockMap.find(inc.pred.v);
                if (predIt == blockMap.end()) continue;  // unreachable pred
                MirInstId const newVal = rewriteOperand(inc.value);
                dst_.addPhiIncoming(dp.newPhi,
                                    MirPhiIncoming{newVal, predIt->second});
                ++kept;
            }
            if (kept == 0) {
                std::fprintf(stderr,
                    "dss::opt::passes::dce::FunctionRebuilder fatal: phi at "
                    "reachable block ended with zero incomings post-rebuild "
                    "(every predecessor was unreachable AND eliminated) — "
                    "this is a structural violation the verifier would "
                    "have rejected at the source MIR.\n");
                std::abort();
            }
        }
    }

private:
    [[nodiscard]] MirInstId rewriteOperand(MirInstId oldOp) const {
        auto const it = rewrite_.find(oldOp.v);
        if (it == rewrite_.end()) {
            std::fprintf(stderr,
                "dss::opt::passes::dce::FunctionRebuilder fatal: "
                "rewriteOperand: old MirInstId v=%u has no rewrite entry — "
                "scan-order violation OR operand referenced a "
                "DCE-eliminated instruction (D-OPT2-REWRITE-MAP-COMPLETENESS).\n",
                oldOp.v);
            std::abort();
        }
        return it->second;
    }

    void emitValue(MirOpcode op, MirInstId oldId) {
        if (op == MirOpcode::Const) {
            MirInstId const newId = dst_.addConst(
                src_.literalValue(src_.constLiteralIndex(oldId)),
                src_.instType(oldId));
            rewrite_.emplace(oldId.v, newId);
            return;
        }
        if (op == MirOpcode::Arg) {
            MirInstId const newId = dst_.addArg(src_.argIndex(oldId),
                                                src_.instType(oldId));
            rewrite_.emplace(oldId.v, newId);
            return;
        }
        if (op == MirOpcode::GlobalAddr) {
            MirInstId const newId = dst_.addGlobalAddr(
                src_.globalAddrSymbol(oldId), src_.instType(oldId));
            rewrite_.emplace(oldId.v, newId);
            return;
        }
        // Generic computation: remap operands, copy verbatim.
        auto const oldOps = src_.instOperands(oldId);
        std::vector<MirInstId> newOps;
        newOps.reserve(oldOps.size());
        for (auto o : oldOps) newOps.push_back(rewriteOperand(o));
        MirInstId const newId = dst_.addInst(op, newOps, src_.instType(oldId),
                                             src_.instPayload(oldId),
                                             src_.instFlags(oldId));
        rewrite_.emplace(oldId.v, newId);
    }

    void emitTerminator(MirOpcode op, MirInstId oldId,
                        std::unordered_map<std::uint32_t, MirBlockId> const& blockMap) {
        auto const oldOps  = src_.instOperands(oldId);
        auto const oldBlk  = src_.instBlock(oldId);
        auto const oldSucc = src_.blockSuccessors(oldBlk);
        switch (op) {
            case MirOpcode::Br: {
                dst_.addBr(blockMap.at(oldSucc[0].v));
                return;
            }
            case MirOpcode::CondBr: {
                MirInstId const cond = rewriteOperand(oldOps[0]);
                dst_.addCondBr(cond, blockMap.at(oldSucc[0].v),
                                     blockMap.at(oldSucc[1].v));
                return;
            }
            case MirOpcode::Switch: {
                MirInstId const disc = rewriteOperand(oldOps[0]);
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                std::size_t const ncases = oldSucc.size() - 1;
                cases.reserve(ncases);
                for (std::size_t i = 0; i < ncases; ++i) {
                    cases.emplace_back(rewriteOperand(oldOps[1 + i]),
                                       blockMap.at(oldSucc[i].v));
                }
                dst_.addSwitch(disc, cases, blockMap.at(oldSucc[ncases].v));
                return;
            }
            case MirOpcode::Return: {
                std::optional<MirInstId> retVal;
                if (!oldOps.empty()) retVal = rewriteOperand(oldOps[0]);
                dst_.addReturn(retVal);
                return;
            }
            case MirOpcode::Unreachable: {
                dst_.addUnreachable();
                return;
            }
            default:
                std::fprintf(stderr,
                    "dss::opt::passes::dce::FunctionRebuilder fatal: "
                    "emitTerminator: MirOpcode %d marked isTerminator but "
                    "no clone arm — add an arm here when introducing a new "
                    "terminator opcode.\n", static_cast<int>(op));
                std::abort();
        }
    }

    Mir const& src_;
    MirBuilder& dst_;
    std::vector<MirBlockId> const& reachable_;
    std::unordered_set<std::uint32_t> const& liveInsts_;
    std::unordered_map<std::uint32_t, MirInstId> rewrite_;
    std::size_t instructionsEliminated_ = 0;
};

} // namespace

DceResult runDce(Mir& mir, TypeInterner const& /*interner*/,
                 DiagnosticReporter& reporter) {
    DceResult result{};
    MirBuilder builder;

    // Runtime-init globals carve-out: a global with `initFunc.valid()`
    // requires a two-pass func-id remap across the rebuild — not yet
    // implemented. Caller keeps the unoptimized MIR. Emit Info
    // X_OptPassSkipped so the user / tooling can observe that DCE
    // deliberately declined to run on this module (mirroring
    // ConstFold's parallel carve-out — without the diagnostic, this
    // silently re-opens the failure pattern a prior cycle closed).
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i) {
        if (mir.globalInitFunc(mir.globalAt(i)).valid()) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::X_OptPassSkipped;
            d.severity = DiagnosticSeverity::Info;
            d.actual   = "opt::Dce: skipped — module has >= 1 runtime-init "
                         "global; func-id remap not yet implemented "
                         "(D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS — DCE "
                         "shares the same carve-out).";
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
        FunctionRebuilder rb{mir, builder, lset.reachable, lset.liveInsts};
        rb.rebuildFunction(f);
        result.instructionsEliminated += rb.instructionsEliminated();
    }

    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
