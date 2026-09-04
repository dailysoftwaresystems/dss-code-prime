#include "opt/optimizer.hpp"

#include "core/substrate/phase_timers.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_verifier.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "opt/passes/const_fold.hpp"
#include "opt/passes/copy_prop.hpp"
#include "opt/passes/cse.hpp"
#include "opt/passes/dce.hpp"
#include "opt/passes/inlining.hpp"
#include "opt/passes/licm.hpp"
#include "opt/passes/mem2reg.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"
#include "opt/passes/simplify_cfg.hpp"

#include <format>
#include <string>
#include <unordered_set>
#include <vector>

// Per-pass dispatcher. `runPass` returns:
//   { ok: bool, mutated: bool }
// On `ok=false` it MUST have emitted at least one diagnostic
// (D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT — belt-and-suspenders
// guard in `optimize()` below).

namespace dss::opt {

namespace {

struct PassRunResult {
    bool ok      = false;
    bool mutated = false;
};

// `inlineLedger` is the per-`optimize()`-call memory the Inlining pass's
// CUMULATIVE per-caller growth budget is measured against. It is threaded
// from `optimize()` rather than owned by the pass because the pass is
// re-entered on every fixpoint iteration and a budget re-baselined per
// invocation compounds -- see `passes::InlineGrowthLedger`.
[[nodiscard]] PassRunResult runPass(PassId id, Mir& mir,
                                    TargetSchema const& /*target*/,
                                    TypeInterner const& interner,
                                    OptPipeline const& pipeline,
                                    DiagnosticReporter& reporter,
                                    passes::InlineGrowthLedger& inlineLedger,
                                    std::optional<bool> charIsUnsigned) {
    switch (id) {
        case PassId::Identity:
            return {true, false};  // no-op; exercises the engine wiring.
        case PassId::ConstFold: {
            auto const r =
                passes::runConstFold(mir, interner, reporter, charIsUnsigned);
            return {r.ok, r.instructionsFolded > 0};
        }
        case PassId::Dce: {
            auto const r = passes::runDce(mir, interner, reporter);
            return {r.ok,
                    r.instructionsEliminated + r.blocksEliminated
                  + r.functionsEliminated   + r.globalsEliminated > 0};
        }
        case PassId::Mem2Reg: {
            auto const r = passes::runMem2Reg(mir, interner, reporter);
            return {r.ok,
                    r.allocasPromoted + r.phisInserted
                  + r.loadsReplaced  + r.storesEliminated > 0};
        }
        case PassId::CopyProp: {
            auto const r = passes::runCopyProp(mir, interner, reporter);
            return {r.ok, r.phisCollapsed > 0};
        }
        case PassId::Cse: {
            auto const r = passes::runCse(mir, interner, reporter);
            return {r.ok, r.instructionsCsed > 0};
        }
        case PassId::SimplifyCfg: {
            // Marker maintenance rides the verify posture: with per-pass verify
            // OFF nothing reads markers between passes, so the pass skips its
            // whole-module re-derivation and optimize() re-stamps ONCE after
            // the pipeline (before the final verify).
            auto const r = passes::runSimplifyCfg(mir, interner, reporter,
                                                  pipeline.verifyEveryPass);
            return {r.ok,
                    r.branchesFolded + r.blocksJumpThreaded
                  + r.blocksMerged > 0};
        }
        case PassId::Licm: {
            auto const r = passes::runLicm(mir, interner, reporter);
            return {r.ok, r.instructionsHoisted > 0};
        }
        case PassId::Inlining: {
            // Marker maintenance rides the verify posture (see SimplifyCfg).
            auto const r = passes::runInlining(
                mir, interner, reporter, pipeline.inlineThreshold,
                pipeline.inlineCallerGrowthPercent, inlineLedger,
                pipeline.verifyEveryPass);
            return {r.ok, r.callsInlined > 0};
        }
    }
    // Enum-drift fallback. A future PassId enumerator added without
    // a matching switch arm above would silently no-op without the
    // explicit fail-loud below. The static_assert on kPassIdCount
    // catches this at compile time too; the runtime emit is the
    // belt-and-suspenders for a third-party adding an enumerator
    // via reinterpret_cast or unchecked numeric construction.
    ParseDiagnostic d;
    d.code     = DiagnosticCode::X_UnknownPassId;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::format(
        "opt::runPass: PassId ordinal {} has no dispatch arm — "
        "substrate-shape violation (D-OPT1-PASS-ID-STABILITY).",
        static_cast<int>(id));
    reporter.report(std::move(d));
    return {false, false};
}

// ★★★ THE INLINE-DEFINITION STRIP
// (D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED, C99 6.7.4p7).
//
// Removes every function that must not be emitted because it is an INLINE
// DEFINITION: it "does not provide an external definition for the function", it
// exists in the module ONLY so the inliner may use it as 6.7.4p7's "alternative
// to an external definition", and by the time the pipeline is done its job is
// over. This is LLVM's `EliminateAvailableExternally` in one screen, and it is
// the second half of a decision whose first half is in CST→HIR.
//
// ★★★ WHY IT IS AN EPILOGUE AND NOT A PASS. It must run on EVERY module at
// EVERY optimization level, and a pass runs only if a pipeline lists it. The
// debug pipeline is `["Identity"]`; had this been a pass that `release.pipeline
// .json` listed, a debug build would emit the body and DSS would start LINKING
// programs that gcc, clang and the C standard all say have no external
// definition — ✔MEASURED, all three fail at `-O0`. Gating it on "will we
// optimize" has the same defect. So: unconditional, after the loop, no
// configuration surface, and impossible to forget.
//
// ★★ HOW IT KNOWS WHICH FUNCTIONS. Not a flag — a property of the module. An
// inline definition is the ONLY function whose SymbolId is also declared by an
// `ExternImport`; HIR→MIR fails loud on that pair for every other producer
// ("Each SymbolId must belong to either a function OR an extern, never both"),
// and it sanctions it for exactly the C99 6.7.4p7 case. That matters because
// SymbolIds survive every rebuild verbatim, whereas a `MirFuncAttribute` keyed
// by MirFuncId is discarded the first time any pass rebuilds the module — and a
// lost mark here emits a body that must not exist. The carrier had to be
// something the module cannot forget.
//
// ★ AND WHAT SURVIVES IT. Nothing else changes. The `ExternImport` row stays, so
// a call the inliner did NOT take still binds to a sibling CU's external
// definition, or fails loud `K_SymbolUndefined` naming the symbol — the exact
// path, and the exact diagnostic, that this shape produced before the body
// existed at all. A call the inliner DID take leaves the row unreferenced, and
// the linker's `rejectOrDropUnreferencedExterns` drops it silently. Taking the
// function's ADDRESS is likewise unchanged and still fails loud, which is what
// both reference compilers do at every `-O` level (✔MEASURED) and what 6.7.4p7
// requires: it licenses using the inline definition for a CALL, not for an
// address.
//
// Returns the number of functions dropped (0 = the module had none, the common
// case, and the scan below is the only cost).
[[nodiscard]] std::size_t
stripInlineDefinitions(Mir& mir, std::span<ExternImport const> externImports) {
    if (externImports.empty()) return 0;
    std::unordered_set<std::uint32_t> externSyms;
    externSyms.reserve(externImports.size());
    for (ExternImport const& e : externImports) externSyms.insert(e.symbol.v);

    std::size_t const nf = mir.moduleFuncCount();
    // `oldOrdinalToNew[i]` = the rebuilt ordinal of old function `i`, or
    // UINT32_MAX when dropped. Built in the SAME walk that decides the drop, so
    // the table and the rebuild cannot disagree about which function is which.
    std::vector<std::uint32_t> oldOrdinalToNew(nf, UINT32_MAX);
    std::size_t dropped = 0;
    for (std::uint32_t i = 0; i < nf; ++i) {
        if (externSyms.contains(mir.funcSymbol(mir.funcAt(i)).v)) {
            ++dropped;
            continue;
        }
        oldOrdinalToNew[i] = static_cast<std::uint32_t>(i - dropped);
    }
    if (dropped == 0) return 0;

    MirBuilder builder;
    passes::MirIdentityRebuildPolicy policy;
    for (std::uint32_t i = 0; i < nf; ++i) {
        if (oldOrdinalToNew[i] == UINT32_MAX) continue;
        passes::MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(mir.funcAt(i));
    }
    // AFTER every surviving function is re-added — the helper's ordering
    // contract, and the remap it needs to re-point a runtime-init global now
    // that ordinals have shifted.
    // The name is what the helper's two fatals report — this epilogue is NOT a
    // `kPassNameTable` pass, so it names itself
    // (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS).
    passes::cloneGlobalsRemappingInitFunc(mir, builder, oldOrdinalToNew,
                                          "InlineDefinitionStrip");
    mir = std::move(builder).finish();
    return dropped;
}

// ── The schedule interpreter (P10 operator ruling, 2026-08-18) ────────
//
// The engine became the INTERPRETER of the normalized schedule tree:
// depth-first, left-to-right over the tree, fixpoint iteration outer /
// body inner. Every loaded or flat()-built schedule has a single
// top-level Fixpoint root, so for those the walk is EXACTLY the
// pre-tree (iteration × passes) loop nest — same invocation order, same
// call-global counters, same `iter=` trace bytes (behavior-preserving
// by construction; pinned by the byte/counter identity battery).
//
// The pinned interpreter contract, item by item:
//   1. invocation order: depth-first, left-to-right; fixpoint
//      iteration outer, body inner.
//   2. `mutated` is a boolean PER INVOCATION accumulated into the ONE
//      call-global OptResult (passesMutated + passMutationCount[p] —
//      sum(passMutationCount) == passesMutated, always).
//   3. early stop: per WHOLE traversal, checked AFTER the traversal
//      completes (a converged traversal still runs every pass AND
//      every per-pass verify). The delta scope for any fixpoint node
//      is a per-traversal snapshot of the call-global counter — an
//      inner fixpoint cannot see outer mutations (they all happened
//      before this node's traversal began); the root's snapshot IS the
//      pre-tree `mutatedAtIterStart`. `repeat` NEVER early-exits.
//   4. max:1 keeps the convergence check ACTIVE (a fixpoint whose one
//      traversal is mutation-free HAS converged); max:0 clamps to ONE
//      traversal, never zero.
//   5. fixedPointReached = every EXECUTED fixpoint node converged
//      before its cap (AND over all of them; the root included).
//   6. verifyEveryPass: after EVERY pass-leaf invocation, under
//      PhaseTimers::Scope{CompilePhase::Verify}, exactly as before —
//      the --time Optimize/Verify split survives inside the
//      interpreter.
//   7. trace: `iter=` is the INNERMOST enclosing fixpoint's iteration
//      index, starting at 0 per optimize() call. Single-fixpoint
//      schedules (every desugared flat doc + the two shipped docs)
//      emit byte-identical lines; deeper schedules add
//      ` path=<index-path>` ONLY when more than one fixpoint is open,
//      so depth-1 logs are unchanged.
//   8. a failure latches `stopped` and unwinds WITHOUT the epilogues —
//      the pre-tree `return result` out of the pass loop, verbatim.
struct ScheduleInterpreter {
    Mir& mir;
    TargetSchema const& target;
    TypeInterner const& interner;
    OptPipeline const& pipeline;
    DiagnosticReporter& reporter;
    OptResult& result;
    std::size_t const entryErrorCount;
    bool const optTrace;
    // Threaded to the Inlining leaf; see runPass's doc comment.
    passes::InlineGrowthLedger& inlineLedger;
    // [[D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS]]: threaded to the ConstFold leaf;
    // see `optimize`'s doc comment for why it is a relayed value and not read
    // off `target` here.
    std::optional<bool> charIsUnsigned;

    // Failure latch — once a pass or a verify fails, unwind without
    // running anything further (the pre-tree early `return result`).
    bool stopped = false;

    // Contract rule 5 accumulators, folded into result.fixedPointReached
    // ONLY on the normal exit path (a failure return keeps the
    // pessimistic `false` default).
    std::size_t completedFixpoints = 0;
    bool allFixpointsConverged = true;

    // ★★★ WHAT A TRUNCATED FIXPOINT LOOKS LIKE, RECORDED SO IT CAN BE SAID
    // OUT LOUD — `X_OptFixpointTruncated`, closing
    // D-OPT-FIXPOINT-CONVERGENCE-IS-COMPUTED-AND-DISCARDED.
    //
    // Before this, the engine DETECTED that it had capped an unconverged
    // optimization fixpoint and said nothing: `fixedPointReached` was
    // computed and had zero consumers anywhere in the tree. A silent ceiling
    // is exactly the class of thing the fail-loud bar exists to forbid — it
    // is the one signal that would tell a user their release build did not
    // finish optimizing, and it is why `{"max": 4}` truncating on the sqlite
    // corpus went unnoticed until a lane went looking for it.
    //
    // Recorded per NODE rather than as one module-level flag because a
    // schedule may hold several fixpoints, and "which one ran out" is the
    // whole actionable content: the cap that needs raising is that node's.
    struct TruncatedFixpoint {
        std::string   path;       // index-path from the root ("" = the root)
        std::uint32_t cap;        // the `max` it exhausted
        std::size_t   lastDelta;  // passes that mutated in the LAST traversal
    };
    std::vector<TruncatedFixpoint> truncated;

    // Trace state. `iter` is the innermost enclosing FIXPOINT's
    // iteration index (a `repeat` contributes no counter — its leaves
    // keep the enclosing fixpoint's). `nodePath` is the leaf's
    // index-path from the root ("0.2" = root's child 0's child 2),
    // printed only when more than one fixpoint is open.
    unsigned iter = 0;
    std::size_t fixpointDepth = 0;
    std::string nodePath;

    void run(OptPipelineNode const& n, std::string path) {
        if (stopped) return;
        nodePath = std::move(path);
        switch (n.kind) {
        case OptPipelineNode::Kind::Leaf:
            runLeaf(n.passId);
            return;
        case OptPipelineNode::Kind::Repeat:
            // Bounded unroll, NO convergence check — ever (rule 3).
            // Repeat never clamps: count 0 unrolls zero times; the
            // engine's zero-invocation entry guard refuses such a
            // tree before the walk begins.
            for (std::uint32_t k = 0; k < n.count && !stopped; ++k) {
                runChildren(n);
            }
            return;
        case OptPipelineNode::Kind::Fixpoint:
            runFixpoint(n);
            return;
        }
    }

    void runChildren(OptPipelineNode const& n) {
        // The base must be captured BEFORE the loop: every nested run()
        // reassigns `nodePath`, so deriving child i's path from the live
        // member after the first subtree completes would label siblings
        // under the LAST-VISITED leaf's path (audit finding: labels only,
        // but wrong labels under the exact deep-schedule conditions the
        // path exists to disambiguate).
        std::string const base = nodePath;
        for (std::size_t i = 0; i < n.children.size() && !stopped; ++i) {
            std::string childPath = base.empty()
                ? std::to_string(i)
                : base + "." + std::to_string(i);
            run(n.children[i], std::move(childPath));
        }
    }

    void runFixpoint(OptPipelineNode const& n) {
        // Today's maxIterations==0 defense, verbatim: clamp to ONE
        // traversal, never zero (rule 4).
        std::uint32_t const cap = n.count == 0 ? std::uint32_t{1} : n.count;
        unsigned const savedIter = iter;
        std::string const selfPath = nodePath;
        std::size_t lastDelta = 0;
        ++fixpointDepth;
        bool converged = false;
        for (std::uint32_t i = 0; i < cap; ++i) {
            iter = i;  // the innermost enclosing fixpoint's counter
            // The delta scope for THIS node (rule 3): a snapshot of the
            // call-global counter at traversal start. Mutations by any
            // outer sibling before this node entered are already in the
            // snapshot; nothing outside this node runs during its
            // traversal, so the delta is exactly this node's own.
            std::size_t const mutatedAtTraversalStart = result.passesMutated;
            runChildren(n);
            if (stopped) break;
            // The LAST traversal's delta, kept for the truncation report:
            // "the cap was hit AND the traversal was still mutating" is the
            // claim, and this is the number that substantiates it.
            lastDelta = result.passesMutated - mutatedAtTraversalStart;
            // Fixed-point check: a full traversal with zero new
            // passes-mutated means no remaining transformation inside
            // this scope enables another.
            if (result.passesMutated == mutatedAtTraversalStart) {
                converged = true;
                break;
            }
        }
        if (!stopped) {
            ++completedFixpoints;
            allFixpointsConverged = allFixpointsConverged && converged;
            if (!converged) {
                truncated.push_back(
                    TruncatedFixpoint{selfPath, cap, lastDelta});
            }
        }
        --fixpointDepth;
        iter = savedIter;
    }

    void runLeaf(PassId p) {
        std::chrono::steady_clock::time_point t0;
        if (optTrace) {
            // Env-gated per-pass trace (DSS_OPT_TRACE=1). Flushed
            // start/done lines so a KILLED run still shows the pass it
            // hung in — the direct diagnostic for a non-converging
            // fixpoint or a pathological/looping pass on a huge
            // function. Depth-1 lines are BYTE-IDENTICAL to the
            // pre-tree engine's; deeper ones add ` path=` only.
            auto const nm = optPassIdName(p);
            if (fixpointDepth > 1) {
                std::fprintf(stderr, "opt: iter=%u path=%.*s pass=%.*s start\n",
                             iter,
                             static_cast<int>(nodePath.size()),
                             nodePath.data(),
                             static_cast<int>(nm.size()), nm.data());
            } else {
                std::fprintf(stderr, "opt: iter=%u pass=%.*s start\n",
                             iter,
                             static_cast<int>(nm.size()), nm.data());
            }
            std::fflush(stderr);
            // Zero the shared rebuild-time accumulator so `rebuild=` below
            // reports THIS pass only (see `optRebuildNsTake` in
            // opt/passes/mir_rebuild_helper.hpp).
            (void)passes::optRebuildNsTake();
            (void)passes::optRebuildInstsTake();
            t0 = std::chrono::steady_clock::now();
        }
        auto const passResult =
            runPass(p, mir, target, interner, pipeline, reporter,
                    inlineLedger, charIsUnsigned);
        if (optTrace) {
            auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            // The per-function REBUILD half, summed over every function this
            // pass rebuilt. `ms - rebuild` is the pass's ANALYSIS + module
            // setup + finish() half — the fraction that reads a READ-ONLY
            // `Mir` and therefore bounds what can ever run function-parallel.
            auto const rebuildNs  = passes::optRebuildNsTake();
            auto const rebuildMs  = static_cast<long long>(rebuildNs / 1000000u);
            auto const rebuiltIns = passes::optRebuildInstsTake();
            auto const nsPerInst  = static_cast<long long>(
                rebuiltIns ? rebuildNs / rebuiltIns : 0);
            auto const nm = optPassIdName(p);
            if (fixpointDepth > 1) {
                std::fprintf(stderr,
                             "opt: iter=%u path=%.*s pass=%.*s done %lldms "
                             "mutated=%d rebuild=%lldms\n",
                             iter,
                             static_cast<int>(nodePath.size()),
                             nodePath.data(),
                             static_cast<int>(nm.size()), nm.data(),
                             static_cast<long long>(ms),
                             passResult.mutated ? 1 : 0, rebuildMs);
            } else {
                std::fprintf(stderr,
                             "opt: iter=%u pass=%.*s done %lldms mutated=%d "
                             "rebuild=%lldms insts=%llu ns/inst=%lld\n",
                             iter,
                             static_cast<int>(nm.size()), nm.data(),
                             static_cast<long long>(ms),
                             passResult.mutated ? 1 : 0, rebuildMs,
                             static_cast<unsigned long long>(rebuiltIns),
                             nsPerInst);
            }
            std::fflush(stderr);
        }
        ++result.passesRun;
        if (!passResult.ok) {
            if (reporter.errorCount() <= entryErrorCount) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::X_OptReturnFalseWithoutDiagnostic;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = "opt::optimize: pass returned ok=false WITHOUT "
                             "emitting a diagnostic — substrate contract "
                             "violation (D-OPT1-RETURN-FALSE-DIAGNOSTIC-"
                             "CONTRACT).";
                reporter.report(std::move(d));
            }
            stopped = true;
            return;
        }
        if (passResult.mutated) {
            ++result.passesMutated;
            // Per-pass effectiveness signal (D-OPT-PASS-METRICS):
            // record each invocation where this PassId mutated.
            // Consumed by effectiveness tests asserting that a
            // pass fired enough times to prove the
            // mutually-enabling cluster converged (e.g.
            // `passMutationCount[ConstFold] >= 2` proves the
            // re-fold post-Mem2Reg happened).
            //
            // The `kPassIdCount` static_assert at the enum
            // declaration site keeps this index in range; an
            // out-of-range `p` getting past `runPass` would also
            // have already routed through the `X_UnknownPassId`
            // fail-loud arm + early-returned. Reaching here with
            // an OOR ordinal is a substrate-contract violation —
            // fail loud rather than silently drop the count.
            auto const idx = static_cast<std::size_t>(p);
            if (idx >= kPassIdCount) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::X_UnknownPassId;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "opt::optimize: PassId ordinal {} bypassed the "
                    "runPass enum-drift guard AND reached the "
                    "passMutationCount increment with mutated=true "
                    "— substrate-shape violation "
                    "(D-OPT1-PASS-ID-STABILITY).",
                    static_cast<int>(p));
                reporter.report(std::move(d));
                stopped = true;
                return;
            }
            ++result.passMutationCount[idx];
        }

        // D-OPT1-VERIFY-AFTER-EVERY-PASS / D-OPT1-VERIFY-FREQUENCY-CONFIG —
        // verify after EVERY pass only under the DEVELOPER posture
        // (`verifyEveryPass`, the safe default: LLVM `-verify-each` / GCC
        // `--enable-checking=yes` — pinpoints the pass that produced invalid
        // MIR). The RELEASE posture (verifyEveryPass=false) verifies ONCE
        // after the whole pipeline (below), trusting tested passes — the
        // LLVM/GCC production split. Per-pass verify over a large module is
        // ~passes × iterations full-module verifies (minutes on SQLite).
        // (Sub-scoped as `Verify` so --time separates verify's share from
        // the passes + per-pass rebuild inside the Optimize phase.)
        if (pipeline.verifyEveryPass) {
            substrate::PhaseTimers::Scope verifyScope{
                substrate::CompilePhase::Verify};
            MirVerifier verifier{mir, &interner};
            if (!verifier.verify(reporter)) {
                stopped = true;
                return;
            }
        }
    }
};

} // namespace

OptResult optimize(Mir& mir,
                   TargetSchema const& target,
                   TypeInterner const& interner,
                   OptPipeline const& pipeline,
                   DiagnosticReporter& reporter,
                   std::span<ExternImport const> externImports,
                   std::optional<bool> charIsUnsigned) {
    // D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT: a false return MUST
    // be paired with a new error. Snapshot + belt-and-suspenders
    // emit below covers any future failure path that forgets to.
    auto const entryErrorCount = reporter.errorCount();

    OptResult result{};

    // Symmetric defense, generalized with the schedule tree: the JSON
    // loader rejects empty `passes` at load time, but a caller
    // constructing OptPipeline{} directly in code (test fixtures,
    // future programmatic builders, the CompileConfig→pipeline mapping
    // when it lands) could bypass that check and silently produce an
    // "optimizer ran nothing" result observationally indistinguishable
    // from a successful run. The tree generalization of "empty
    // pipeline" is "computes ZERO invocations" — an empty body, a
    // zero-unroll repeat, any shape that runs nothing. Reject at the
    // engine entrypoint too (the shared static cost function is the
    // ONE definition of what a schedule runs) — use [Identity] for an
    // explicit no-op.
    if (optPipelineCost(pipeline.schedule) == 0) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::X_PipelineMalformed;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = "opt::optimize: pipeline schedule computes zero pass "
                     "invocations — substrate contract violation. Use "
                     "[Identity] for an explicit no-op pipeline.";
        reporter.report(std::move(d));
        return result;
    }

    // ONE ledger per optimize() call — the memory that makes the Inlining
    // pass's per-caller growth budget CUMULATIVE across this schedule's
    // iterations instead of re-baselining on each one. Scoped here, so the
    // unit and program stages each get their own.
    passes::InlineGrowthLedger inlineLedger;

    ScheduleInterpreter interp{mir, target, interner, pipeline, reporter,
                               result, entryErrorCount,
                               std::getenv("DSS_OPT_TRACE") != nullptr,
                               inlineLedger, charIsUnsigned};
    interp.run(pipeline.schedule, std::string{});
    if (interp.stopped) {
        // A failed pass / failed verify unwinds WITHOUT the epilogues —
        // the pre-tree `return result` out of the pass loop, verbatim
        // (fixedPointReached keeps its pessimistic `false` default).
        return result;
    }
    // Rule 5: converged iff EVERY executed fixpoint node converged
    // before its cap. Single-root schedules (every loaded/flat doc)
    // reduce this to the pre-tree rule exactly.
    result.fixedPointReached =
        interp.completedFixpoints > 0 && interp.allFixpointsConverged;

    // ★★★ THE FLAG NOW HAS A CONSUMER, AND IT IS THIS ONE
    // (D-OPT-FIXPOINT-CONVERGENCE-IS-COMPUTED-AND-DISCARDED).
    //
    // `fixedPointReached` is the GATE on the report, deliberately, rather
    // than the report being emitted from inside `runFixpoint`. Two reasons,
    // and both are about the flag rather than about convenience: it makes the
    // value load-bearing at the site that computes it — flip the computation
    // and the diagnostic disappears, which is what makes the red-on-disable
    // arm meaningful — and it keeps the emission on the NORMAL exit path
    // only, so a pass or verifier failure unwinds without adding a
    // truncation complaint to a build that already failed for a real reason.
    //
    // ⚠ NOT AN ERROR, AND THAT IS A JUDGEMENT, NOT A HEDGE. A truncated
    // fixpoint emits a CORRECT program — just not the program a converged
    // pipeline would have emitted. Failing the build here would refuse
    // programs that compile fine today. `Warning` is the honest register:
    // the compiler did not finish the job it was asked to do, and the user
    // is entitled to know before they measure the result.
    if (!result.fixedPointReached) {
        for (auto const& t : interp.truncated) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::X_OptFixpointTruncated;
            d.severity = DiagnosticSeverity::Warning;
            d.actual   = std::format(
                "opt::optimize: pipeline '{}' fixpoint node {} hit its cap of "
                "{} iteration(s) with {} pass(es) still mutating the module on "
                "the final traversal — the pipeline stopped because it ran out "
                "of iterations, NOT because it converged. The emitted program "
                "is correct but is not the program a converged pipeline would "
                "have emitted. Raise that node's `max` in the pipeline "
                "document, or reduce what re-exposes work between iterations.",
                pipeline.name,
                t.path.empty() ? std::string{"<root>"} : t.path,
                t.cap, t.lastDelta);
            reporter.report(std::move(d));
        }
    }

    // ★★★ THE INLINE-DEFINITION STRIP, unconditional and ahead of the final
    // verify (D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED). Placed
    // HERE, after the whole schedule tree, for two reasons that pull the same
    // way: the inliner must have had every traversal to consume these bodies
    // before they go, and the module that reaches codegen must be the module
    // the final verify approved. `result.ok` is untouched — the strip cannot
    // fail (it aborts loud on the one impossible input, a dropped module-init
    // function), and reporting it as a pass would put a mandatory normalize
    // into a configurable surface, which is exactly what it must not be.
    (void)stripInlineDefinitions(mir, externImports);

    // RELEASE posture (D-OPT1-VERIFY-FREQUENCY-CONFIG): a pipeline that did NOT
    // verify after every pass verifies its FINAL MIR exactly once here — a
    // structurally-broken optimizer output must still be a build break before
    // codegen consumes it, but re-verifying the whole module after every pass
    // over a large module (SQLite) is minutes wasted on a tested pipeline.
    if (!pipeline.verifyEveryPass && reporter.errorCount() == entryErrorCount) {
        // Under this posture the CFG-mutating passes (SimplifyCfg / Inlining)
        // skipped their per-call whole-module marker re-derivation (markers
        // feed only the verifier — nothing reads them between passes). Re-stamp
        // every block ONCE from the FINAL CFG here, so the final verify (and
        // anything downstream) sees canonical markers. This one call replaces
        // up to the whole tree's unrolled per-pass derivations (~2min on
        // SQLite).
        rederiveStructCfMarkers(mir);
        substrate::PhaseTimers::Scope verifyScope{substrate::CompilePhase::Verify};
        MirVerifier verifier{mir, &interner};
        if (!verifier.verify(reporter)) {
            return result;
        }
    }

    result.ok = (reporter.errorCount() == entryErrorCount);
    return result;
}

} // namespace dss::opt
