#include "opt/passes/licm.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"
#include "opt/analysis/mir_memory_clobbers.hpp"
#include "opt/passes/mir_id_remap.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss::opt::passes {

namespace {

using dss::opt::analysis::StrictTbaa;
using dss::opt::analysis::MirMemoryClobbers;

// ── Speculation safety: ONE fact, ONE owner ───────────────────────────
// The fact: "executing this opcode can FAULT."
//   * SDiv / UDiv / SMod / UMod raise at runtime — #DE on x86 IDIV for
//     divisor 0 or INT_MIN/-1 quotient overflow; similar on ARM cores
//     that trap.
//   * Load faults on a null / unmapped / misaligned pointer.
// Considered and deliberately NOT in the set: FDiv (IEEE division by
// zero yields inf/NaN in the default masked FP environment — no trap);
// Gep / Bitcast / PtrToInt / ExtractValue (address or register
// arithmetic, never a memory access); Alloca, Store, Call,
// IntrinsicCall, AtomicLoad (all carry `hasSideEffects` in the
// opcodeInfo table, so `isLicmCandidateOpcode` refuses them before this
// predicate is ever consulted — read off the table, not assumed).
//
// The CONSEQUENCE is identical for every member, so it is stated ONCE
// rather than growing one gate per opcode family: a may-fault candidate
// may be hoisted into the preheader ONLY out of a block that is
// GUARANTEED TO EXECUTE when the loop is entered
// (`blockRunsOnEveryLoopEntry`, built per loop in analyze()). Where that
// holds, the fault was going to happen on the first iteration anyway,
// and C makes both faults undefined behaviour — so relocating it
// preserves behaviour in the only sense the standard defines. Where it
// does NOT hold, hoisting INJECTS a fault into a program that had none:
//
//     for (i = 0; i < n; i++) { if (q) x = *p; }   /* q == 0, p == NULL */
//
// which is a MISCOMPILE, not a missed-precision matter
// (D-OPT6-LICM-SPECULATIVE-LOAD-HOIST — witnessed live at
// `--config=release` as an 0xC0000005 access violation while this gate
// covered division only).
//
// ⚠ THE EXACT BOUNDARY OF THE SOUNDNESS ARGUMENT, stated rather than
// left for someone to rediscover. "B dominates every exiting block"
// proves "if control LEAVES the loop, B ran". It does not prove B ran in
// an execution that never leaves — and one can be built: an inner
// non-terminating region sitting between the header and B (`for (;;) {
// for (;;) {} … x = *p; }`) reaches neither the exit nor B, so the
// original program hangs where the hoisted one faults. Closing that would
// mean proving termination. C23 6.8.5p6 instead lets an implementation
// ASSUME an iteration statement terminates unless its controlling
// expression is a constant expression, which is exactly the assumption
// LLVM's `isGuaranteedToExecute` + `mustprogress` rest on, so this gate
// matches the reference compilers. The `exits.empty()` refusal below
// covers the one shape the assumption does NOT cover and which is cheap
// to detect: a loop with no exiting block at all.
//
// The division arm keeps a refinement of its OWN beyond this gate:
// proving the divisor non-zero would admit hoists out of blocks that are
// NOT guaranteed to execute, which needs interval / value-range analysis
// (D-OPT6-LICM-TRAP-SAFE-HOIST). That is a strictly additional
// permission, not a different rule.
[[nodiscard]] bool mayFaultWhenSpeculated(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::SDiv: case MirOpcode::UDiv:
        case MirOpcode::SMod: case MirOpcode::UMod:
        case MirOpcode::Load:
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
    // analyze() loop additionally gates each Load via the pass-wide
    // clobber index (`MirMemoryClobbers::anyClobberInBlocks`, enumeration-
    // identical to the reference `mirAnyMayAliasingStoreInLoop` scan) — if
    // no may-aliasing Store sits in the loop body, the Load is
    // loop-invariant in the alias sense.
    //
    // NOTE what is NOT decided here: the may-fault opcodes (Load and the
    // divisions) stay CANDIDATES. Whether they may actually move is a
    // property of the candidate's BLOCK, not of its opcode, so it cannot
    // be answered from an opcode alone — `mayFaultWhenSpeculated` +
    // `blockRunsOnEveryLoopEntry` decide it together in analyze(). A
    // blanket opcode-level refusal here is what made the division arm
    // pass while the Load arm miscompiled.
    // Leaf opcodes (zero-operand value origins) have dedicated
    // builders on MirBuilder + carry no runtime computation worth
    // hoisting. Excluding them keeps the hoist emit-loop generic
    // (one `addInst` shape) and matches the user-visible semantics:
    // a Const isn't computed at runtime; relocating it has no
    // measurable benefit and would duplicate the rebuilder's
    // dedicated-builder dispatch logic at the LICM tier.
    //
    // ★ `BlockAddress` BELONGS TO THIS LIST BY THE LIST'S OWN RULE —
    // a zero-operand value origin whose builder is `addBlockAddress`
    // — and it was MISSING from it. It was stopped three lines above
    // instead, by `hasSideEffects`, which it carries for a completely
    // unrelated reason: its PRESENCE is the canonical mark that its
    // target block is address-taken, so DCE must not drop it
    // (mir_opcode.hpp). Protection by a coincidence of two
    // independent facts is not protection: had that flag ever been
    // revisited on its own terms, the hoist loop below would have
    // forwarded `instPayload` — a block id — into the rebuild's NEW
    // block numbering, pointing `&&label` at the wrong block with
    // nothing to observe it. `MirBuilder::addInst` now REFUSES the
    // opcode, so that route aborts rather than miscompiling; this
    // entry is what makes LICM correct on its own terms instead of
    // relying on the backstop.
    if (op == MirOpcode::Const || op == MirOpcode::Arg
     || op == MirOpcode::GlobalAddr
     || op == MirOpcode::BlockAddress) return false;
    return true;
}

// ONE spelling of this pass's name for EVERY diagnostic it can emit — the
// carve-out Info and every `MirFunctionRebuilder` fatal
// (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS). ★ Like `Cse`, this pass
// constructs the rebuilder at TWO sites (timed / untimed arms), so a
// per-construction-site string could disagree with itself; one constant cannot.
constexpr std::string_view kPassName = "Licm";

class LicmPolicy final : public MirRebuildPolicy {
public:
    LicmPolicy(Mir const& src, TypeInterner const& interner) noexcept
        : src_(src), interner_(interner),
          strictTbaa_(src.aliasingMode() == MirAliasingMode::StrictTBAA
                      ? StrictTbaa::Yes : StrictTbaa::No),
          charTypesAliasAll_(src.charTypesAliasAll()) {}

    [[nodiscard]] std::string_view passName() const noexcept override {
        return kPassName;
    }

    [[nodiscard]] std::size_t instructionsHoisted() const noexcept {
        return instructionsHoisted_;
    }

    // `preds` = `mirBuildPredecessors(mir)` for the SAME module, computed ONCE
    // by runLicm and threaded in (invariant across every function in one Licm
    // pass — the same argument-identity hoist CSE received:
    // D-OPT-CSE-ANALYSIS-HOIST). `clobbers` = the pass-wide memory-clobber
    // index (D-OPT-MEMORYSSA-CLOBBER-WALK) — the Load hoist-admission gate
    // queries it instead of re-scanning every loop-body instruction per
    // candidate per fixed-point iteration.
    // `domScratch` = the pass-wide reusable dominator scratch
    // (D-OPT-DOMTREE-SCRATCH-REUSE) — byte-identical dom trees without the
    // per-function whole-module allocation storm.
    // `moduleSelfLoops` = the pass-wide self-looping-block index and
    // `candidateBuf` = the pass-wide scratch behind the SCOPED back-edge sweep
    // ([[D-OPT-LICM-NATURAL-LOOPS-MODULE-WIDE-SCAN]]). Both are owned by
    // `runLicm` and threaded in rather than rebuilt here: the index is a MODULE
    // property (one sweep per pass call, not one per function) and the buffer
    // exists so the per-function candidate list reuses its storage.
    void analyze(MirFuncId fn, DiagnosticReporter& reporter,
                 std::vector<std::vector<MirBlockId>> const& preds,
                 MirMemoryClobbers const& clobbers, MirDomScratch& domScratch,
                 std::span<std::uint32_t const> moduleSelfLoops,
                 std::vector<std::uint32_t>& candidateBuf);

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
        MirInstRemap& rewrite,
        MirBlockRemap const& /*blockMap*/) override {
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
                MirInstId const* const mapped = rewrite.find(o.v);
                if (mapped == nullptr) {
                    std::fprintf(stderr,
                        "dss::opt::passes::Licm fatal: hoisting old "
                        "inst v=%u from loop body but operand v=%u "
                        "is not in rewrite map — analysis-tier "
                        "eligibility check missed a chained-invariant "
                        "operand (D-OPT6-LICM-CHAINED-INVARIANTS).\n",
                        oldX.v, o.v);
                    std::abort();
                }
                newOps.push_back(*mapped);
            }
            MirInstId const newId = dst.addInst(op, newOps,
                                                src_.instType(oldX),
                                                src_.instPayload(oldX),
                                                src_.instFlags(oldX));
            rewrite.put(oldX.v, newId);
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
    bool const          charTypesAliasAll_;

    // Per-function analysis state. Counters live across functions.
    std::unordered_set<MirInstId> hoistedInsts_;                          // body-side: skip via shouldEmit
    std::unordered_map<MirBlockId, std::vector<MirInstId>> hoistPlan_;    // preheader-side: emit-list, in body scan order

    std::size_t instructionsHoisted_ = 0;

public:
    // Env-gated DSS_OPT_TRACE sub-timing accumulators (read by runLicm's
    // one-line-per-pass-call trace; zero-cost when the trace is off).
    std::uint64_t traceDomNs   = 0;
    std::uint64_t traceLoopsNs = 0;
};

void LicmPolicy::analyze(MirFuncId fn, DiagnosticReporter& reporter,
                         std::vector<std::vector<MirBlockId>> const& preds,
                         MirMemoryClobbers const& clobbers,
                         MirDomScratch& domScratch,
                         std::span<std::uint32_t const> moduleSelfLoops,
                         std::vector<std::uint32_t>& candidateBuf) {
    resetPerFunction();
    MirBlockId const entry = src_.funcEntry(fn);
    auto const rpo = mirReversePostOrder(src_, entry);
    if (rpo.empty()) return;

    // `preds` is the caller's precomputed whole-module predecessor map
    // (invariant this pass); the scratch-backed dom is byte-identical to the
    // fresh path and valid until the NEXT analyze() call (function-local use
    // only — const& binding, per the D-OPT-DOMTREE-SCRATCH-REUSE contract).
    static bool const trace = std::getenv("DSS_OPT_TRACE") != nullptr;
    auto const tDom0 = trace ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    auto const& dom = computeMirDomTree(src_, entry, rpo, preds, domScratch);
    auto const tDom1 = trace ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    // ★★ THE BACK-EDGE SOURCE SWEEP IS SCOPED TO THIS FUNCTION
    // ([[D-OPT-LICM-NATURAL-LOOPS-MODULE-WIDE-SCAN]]). `dom` is a ONE-FUNCTION
    // tree, so the whole-module overload swept every block of the module for
    // every function — O(functions × module blocks), quadratic in module size,
    // and it was ALL of LICM's cost on a merged program module. ✔MEASURED
    // 2026-08-25 (cycle P36), release dsscp over the 103-TU full-source sqlite
    // corpus at `--jobs 1`: `loops=2,239 ms` of each 2,552 ms LICM call, four
    // calls per whole-program pipeline = 8.98 s of a 79.3 s build (11.3%), all
    // of it on the SERIAL side of the -j1→-j4 scaling.
    //
    // `mirBackEdgeCandidates` supplies exactly the superset the scoped
    // overload's completeness clause names (`rpo ∪ this function's range ∪ the
    // module's self-looping blocks`), so the forest is BYTE-IDENTICAL to the
    // whole-module sweep's — including the pseudo-loops the sweep manufactures
    // for FOREIGN self-looping blocks, which LICM must keep seeing until
    // [[D-MIR-STRUCTCF-DERIVATION-REACHES-PAST-THE-FUNCTION]] is taken
    // deliberately. This is a cost change, never a behaviour change.
    mirBackEdgeCandidates(src_, fn, rpo, moduleSelfLoops, candidateBuf);
    auto const loops = mirNaturalLoops(
        src_, dom, preds, std::span<std::uint32_t const>{candidateBuf});
    if (trace) {
        auto const tLoops1 = std::chrono::steady_clock::now();
        traceDomNs += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tDom1 - tDom0)
                .count());
        traceLoopsNs += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tLoops1 - tDom1)
                .count());
    }
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
        if (ambiguous || !preheader.valid()) {
            // D-OPT6-LICM-PREHEADER-INSERTION (cycle 10l, 2026-06-04):
            // pre-cycle this `continue` was silent — the pass silently
            // skipped hoist-eligible loops with no unique non-back-
            // edge predecessor (ambiguous = multiple non-back-edge
            // preds; !preheader.valid() = unreachable loop header).
            // The skipped effectiveness is observable now via an
            // Info-severity diagnostic citing the deferred anchor
            // (preheader insertion + Phi-incoming merge). Mirror of
            // DCE's runtime-init-globals Info skip pattern.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::X_OptPassSkipped;
            d.severity = DiagnosticSeverity::Info;
            d.actual   = std::format(
                "opt::Licm: skipped loop with header v={} — {} "
                "non-back-edge predecessor(s); preheader insertion "
                "+ Phi-incoming merge not yet implemented "
                "(D-OPT6-LICM-PREHEADER-INSERTION).",
                loop.header.v,
                ambiguous ? ">1" : "0");
            reporter.report(std::move(d));
            continue;
        }

        // ── Speculation-safety facts for THIS loop ───────────────────
        // (D-OPT6-LICM-SPECULATIVE-LOAD-HOIST; see
        // `mayFaultWhenSpeculated` for the rule these three facts serve.)
        // Computed ONCE per loop in a single O(|body|) scan, and only
        // CONSULTED when a may-fault candidate actually turns up — a loop
        // containing none pays nothing beyond this scan.
        //
        // (1) `entryIsUnconditional` — the hoist lands in the preheader
        //     BEFORE its terminator. `preheader` above is only "the
        //     header's unique predecessor outside the loop"; it may still
        //     branch SOMEWHERE ELSE. Then executing the preheader does
        //     NOT imply entering the loop — `if (c) for (…) x = *p;`,
        //     where the `if` block IS the preheader — and a may-fault op
        //     hoisted into it runs on a path the loop never ran on.
        // (2) `exits` — every loop block with a successor OUTSIDE the
        //     body, i.e. every block through which control can LEAVE the
        //     loop. Cross-edges are the COMPLETE enumeration, and the
        //     reason deserves stating because a zero-successor block
        //     (Return / Unreachable) looks like a missing case: such a
        //     block can never BE in `loop.body`. `mirNaturalLoops` builds
        //     the body by walking PREDECESSORS back from the back-edge
        //     sources, and a block that reaches nothing is a predecessor
        //     of nothing on that walk. MEASURED, not reasoned: a fixture
        //     with a Return inside the loop region behaves identically
        //     with and without a zero-successor arm here. The source
        //     shape `for (…) { if (r) return 0; … x = *p; }` is still
        //     covered — by the cross-edge from the BRANCHING block to the
        //     out-of-body return block, which makes the branching block
        //     an exit that the dereference does not dominate.
        //     An EMPTY set means control can never leave the loop, so no
        //     block below the header is guaranteed to run at all.
        // (3) `bodyHasCall` — a Call may never return (exit / abort /
        //     longjmp / a nested infinite loop). Control then never
        //     reaches any exit, and (2)'s dominance argument — "control
        //     left through E, and B dominates E, therefore B ran" —
        //     proves nothing. This condition is NECESSARILY whole-body
        //     rather than "the blocks that dominate the candidate": a
        //     call in a SIBLING arm, `for (…) { if (c) exit(0); … x =
        //     *p; }`, stops the candidate from executing just as
        //     effectively as one that precedes it. Load hoists were
        //     already refused in such loops (Call is in
        //     `opcodeClobbersMemory`, so the clobber gate catches them);
        //     this condition is what makes the DIVISION arm sound too.
        auto const preheaderSucc = src_.blockSuccessors(preheader);
        bool const entryIsUnconditional =
            preheaderSucc.size() == 1
            && preheaderSucc[0].v == loop.header.v;
        std::vector<MirBlockId> exits;
        bool bodyHasCall = false;
        for (MirBlockId const b : loop.body) {
            for (MirBlockId const s : src_.blockSuccessors(b)) {
                if (bodySet.count(s.v)) continue;
                exits.push_back(b);
                break;
            }
            // Written only ever to `true`, never assigned the test's
            // result: a plain `bodyHasCall = (o == Call || …)` would be
            // correct ONLY because the loop condition below stops the
            // scan, and would silently RESET the flag the moment someone
            // widened that condition. Monotone by construction instead.
            std::uint32_t const nb = src_.blockInstCount(b);
            for (std::uint32_t i = 0; i < nb && !bodyHasCall; ++i) {
                MirOpcode const o = src_.instOpcode(src_.blockInstAt(b, i));
                if (o == MirOpcode::Call || o == MirOpcode::IntrinsicCall) {
                    bodyHasCall = true;
                }
            }
        }

        // Is block `b` guaranteed to execute whenever this loop is
        // entered? Memoized per block: the dominance walk is O(idom
        // chain) per exit and the chained-invariant fixed point below
        // re-visits every block on every round.
        //
        // Every uncertain answer resolves to `false` — including the
        // dominator walk's `GaveUp`, which is reported once per loop as
        // Info rather than silently mistaken for "not dominated"
        // (mir_dom.hpp's tri-state contract). Refusing to hoist is
        // always behaviour-preserving, so `false` is the fail-safe pole.
        std::unordered_map<std::uint32_t, bool> runsOnEntry;
        bool domGaveUpReported = false;
        auto blockRunsOnEveryLoopEntry = [&](MirBlockId b) -> bool {
            if (auto const it = runsOnEntry.find(b.v); it != runsOnEntry.end()) {
                return it->second;
            }
            bool ok = entryIsUnconditional && !bodyHasCall;
            // Entering the loop IS executing the header, so the header
            // needs no dominance proof. Any other block must lie on
            // EVERY path by which control leaves the loop.
            if (ok && b.v != loop.header.v) {
                ok = !exits.empty();
                for (MirBlockId const e : exits) {
                    if (!ok) break;
                    MirDomResult const dr = mirDominatesBlock(b, e, dom);
                    if (dr == MirDomResult::Dominates) continue;
                    ok = false;
                    if (dr != MirDomResult::GaveUp || domGaveUpReported) break;
                    domGaveUpReported = true;
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::X_OptPassSkipped;
                    d.severity = DiagnosticSeverity::Info;
                    d.actual   = std::format(
                        "opt::Licm: refused may-fault hoists out of block "
                        "v={} in the loop with header v={} — the dominator "
                        "walk to loop exit v={} hit its step cap, so "
                        "guaranteed-to-execute could not be decided; "
                        "refusing to hoist is the behaviour-preserving "
                        "answer (D-OPT6-LICM-SPECULATIVE-LOAD-HOIST).",
                        b.v, loop.header.v, e.v);
                    reporter.report(std::move(d));
                }
            }
            runsOnEntry.emplace(b.v, ok);
            return ok;
        };

        // For each inst in the loop body (skip the header's Phis
        // implicitly since Phi isn't a candidate opcode):
        //   - Eligibility: opcode + flags.
        //   - All operands defined OUTSIDE the loop body
        //     OR already hoisted earlier this loop (chained
        //     invariants, D-OPT6-LICM-CHAINED-INVARIANTS, cycle
        //     10j, 2026-06-04).
        //
        // Chained-invariant fixed point: a body inst whose operands
        // include some `y = x*c` where `x = a+b` is itself
        // hoist-eligible is also hoistable — but only AFTER `x` has
        // been recorded. The iterative loop re-scans until no new
        // candidates surface, monotonically growing
        // `hoistedInThisLoop`. Termination: the set can only grow
        // by `|loop.body inst count|` total; a no-progress
        // iteration breaks via `changed=false`.
        //
        // Iter cap (post-fold 2026-06-04 audit FOLD-NOW): the cap
        // DERIVES from the structural bound — `sum of blockInstCount
        // over loop.body + 1` — so the abort is a "recordHoist
        // behavior changed" guard, NOT a chain-length ceiling. The
        // prior `kMaxIter = 64` literal would have falsely aborted
        // on legitimate >64-link chains (e.g., a body with 100
        // sequentially-dependent invariants).
        std::unordered_set<MirInstId> hoistedInThisLoop;
        bool changed = true;
        std::size_t iterCap = 0;
        std::size_t maxIter = 1;  // +1 slack on the structural bound
        for (MirBlockId const b : loop.body) {
            maxIter += src_.blockInstCount(b);
        }
        while (changed) {
            if (iterCap++ >= maxIter) {
                // Defensive step cap: the per-loop fixed point is
                // structurally bounded by `|body insts|`, but an
                // unanticipated recordHoist behavior change could
                // in principle loop forever — fail loud rather
                // than hang.
                std::fprintf(stderr,
                    "dss::opt::passes::Licm fatal: chained-invariant "
                    "fixed point exceeded %zu iterations on a single "
                    "loop (header v=%u, body inst count + 1 = %zu) — "
                    "substrate-contract violation "
                    "(D-OPT6-LICM-CHAINED-INVARIANTS).\n",
                    iterCap, loop.header.v, maxIter);
                std::abort();
            }
            changed = false;
            for (MirBlockId const b : loop.body) {
                std::uint32_t const ninst = src_.blockInstCount(b);
                for (std::uint32_t i = 0; i < ninst; ++i) {
                    MirInstId const id = src_.blockInstAt(b, i);
                    if (hoistedInThisLoop.count(id)) continue;
                    MirOpcode const op = src_.instOpcode(id);
                    if (!isLicmCandidateOpcode(op)) continue;
                    if (has(src_.instFlags(id), MirInstFlags::Volatile)) continue;

                    // Operand check: defined outside the loop body
                    // OR already in this-loop's hoisted set.
                    bool allOutside = true;
                    for (MirInstId const o : src_.instOperands(id)) {
                        MirBlockId const opBlock = src_.instBlock(o);
                        if (!opBlock.valid()) { allOutside = false; break; }
                        if (bodySet.count(opBlock.v)) {
                            // Operand defined in loop body — accept
                            // ONLY if it was hoisted earlier this
                            // round (chained invariant). Otherwise
                            // it's loop-variant and disqualifies.
                            if (!hoistedInThisLoop.count(o)) {
                                allOutside = false;
                                break;
                            }
                        }
                    }
                    if (!allOutside) continue;
                    // Load admission gate: a Load is hoist-eligible only
                    // when no Store in the loop body may alias its
                    // pointer. This answers "is the loaded VALUE
                    // invariant?" — and ONLY that. Whether the load is
                    // SAFE TO EXECUTE speculatively is a separate
                    // question, answered by the may-fault gate below.
                    // (TF-C58's soundness argument cites this whole-body
                    // clobber scan, which IS sufficient for that purpose
                    // and is unaffected by the gate below.)
                    if (op == MirOpcode::Load) {
                        auto const lops = src_.instOperands(id);
                        if (lops.empty()) {
                            std::fprintf(stderr,
                                "dss::opt::passes::Licm fatal: Load inst "
                                "v=%u has zero operands — verifier-contract "
                                "violation.\n", id.v);
                            std::abort();
                        }
                        if (clobbers.anyClobberInBlocks(
                                interner_, lops[0], loop.body,
                                strictTbaa_, charTypesAliasAll_)) {
                            continue;  // clobbered in body
                        }
                    }
                    // ── The may-fault gate: the ONE consequence of the
                    // ONE fact `mayFaultWhenSpeculated` records. A Load
                    // or a division may only leave a block that runs on
                    // every entry into this loop; anywhere else, hoisting
                    // would execute a fault the source never reached
                    // (D-OPT6-LICM-SPECULATIVE-LOAD-HOIST). Deliberately
                    // NOT paired with a diagnostic: unlike the
                    // ambiguous-preheader skip above this is the CORRECT
                    // and permanent answer for the shape, not a deferred
                    // capability, and the shape (`if (p) …` inside a
                    // loop) is pervasive in ordinary C — an Info per
                    // occurrence would bury the diagnostics that mean
                    // something.
                    if (mayFaultWhenSpeculated(op)
                        && !blockRunsOnEveryLoopEntry(b)) {
                        continue;
                    }
                    // Nested-loop dedup (CRITICAL fix): an inst that's
                    // invariant in BOTH an inner and an outer enclosing
                    // loop (e.g. operands all in function entry) would
                    // appear as a candidate in BOTH loops. The first
                    // record wins; subsequent visits skip.
                    if (hoistedInsts_.count(id)) continue;
                    recordHoist(id, preheader);
                    hoistedInThisLoop.insert(id);
                    changed = true;
                }
            }
        }
    }
}

} // namespace

LicmResult runLicm(Mir& mir, TypeInterner const& interner,
                   DiagnosticReporter& reporter) {
    LicmResult result{};
    MirBuilder builder;

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, kPassName)
        == GlobalClonePrelude::CarvedOut) {
        result.ok = true;
        return result;
    }

    LicmPolicy policy{mir, interner};
    std::size_t const nf = mir.moduleFuncCount();
    // Compute the whole-module predecessor map + the memory-clobber index ONCE
    // for the entire pass — both invariant while `mir` is read-only (the rebuild
    // writes a SEPARATE builder, finalized only after this loop). Removes the
    // per-function whole-module preds rebuild (the CSE-audited argument-identity
    // hoist) + the per-candidate-per-iteration loop-body instruction scans
    // (D-OPT-MEMORYSSA-CLOBBER-WALK).
    static bool const trace = std::getenv("DSS_OPT_TRACE") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto const tSetup = now();
    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const clobbers{mir, preds};
    MirDomScratch domScratch;   // one per pass call (D-OPT-DOMTREE-SCRATCH-REUSE)
    // The module's self-looping-block index: a MODULE property, so it is swept
    // ONCE per pass call and every function reuses it, exactly as
    // `rederiveStructCfMarkers` does. `candidateBuf` is the per-function
    // candidate list's storage, reused rather than reallocated
    // ([[D-OPT-LICM-NATURAL-LOOPS-MODULE-WIDE-SCAN]]).
    std::vector<std::uint32_t> moduleSelfLoops;
    mirModuleSelfLoopBlocks(mir, moduleSelfLoops);
    std::vector<std::uint32_t> candidateBuf;
    long long const setupMs = trace
        ? std::chrono::duration_cast<std::chrono::milliseconds>(now() - tSetup)
              .count()
        : 0;
    std::uint64_t analyzeNs = 0, rebuildNs = 0;
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        auto const tA = trace ? now() : std::chrono::steady_clock::time_point{};
        policy.analyze(f, reporter, preds, clobbers, domScratch,
                       std::span<std::uint32_t const>{moduleSelfLoops},
                       candidateBuf);
        if (trace) {
            auto const tR = now();
            analyzeNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(tR - tA)
                    .count());
            MirFunctionRebuilder rb{mir, builder, policy};
            rb.rebuildFunction(f);
            rebuildNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now() - tR)
                    .count());
        } else {
            MirFunctionRebuilder rb{mir, builder, policy};
            rb.rebuildFunction(f);
        }
    }
    if (trace) {
        std::uint64_t const domLoops = policy.traceDomNs + policy.traceLoopsNs;
        std::fprintf(stderr,
            "opt:   Licm sub: preds+index=%lldms analyze=%llums (dom=%llums "
            "loops=%llums hoist=%llums) rebuild=%llums\n",
            setupMs,
            static_cast<unsigned long long>(analyzeNs / 1000000u),
            static_cast<unsigned long long>(policy.traceDomNs / 1000000u),
            static_cast<unsigned long long>(policy.traceLoopsNs / 1000000u),
            static_cast<unsigned long long>(
                (analyzeNs - std::min(analyzeNs, domLoops)) / 1000000u),
            static_cast<unsigned long long>(rebuildNs / 1000000u));
    }

    result.instructionsHoisted = policy.instructionsHoisted();
    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
