#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "mir/summary/mir_summary.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
// OPT11 — THE GLOBAL PASS OVER THE SUMMARIES (plan 22 §0.2)
// ═══════════════════════════════════════════════════════════════════════════
//
// ★★★ **THIS PASS READS ONLY SUMMARIES. IT NEVER TOUCHES ONE BYTE OF MIR.**
// That is the entire ThinLTO economy and it is why `.dss.summary` and
// `.dss.mir` are two SEPARATE sections: the whole-program decision pages in
// the small one and leaves the large one on disk. A change here that needs a
// body has broken the architecture — the decision must be re-expressed as a
// summary fact instead.
//
// It answers three whole-program questions no per-TU pass can:
//
//   1. WHICH DEFINITION WINS for each externally-visible name — delegated to
//      `linker::resolveCrossCuDefs`, the SAME single source of truth
//      `mergeCuMirs` uses, so the index and the merge cannot disagree.
//   2. WHICH SYMBOLS ARE LIVE — the inter-procedural BFS with the global
//      initializer fixpoint that `scanLiveSymbols` runs on the merged module
//      today. Without it the index would ship dead code the merge deletes.
//   3. WHAT EACH TU MAY IMPORT — the candidate set, plus the two
//      whole-program facts (`escapedSymbols`, `sccOf`) each TU's optimizer
//      must be handed so `inlineLegalityGate` stays exactly as conservative
//      as it is today.
//
// ── ★★★ WHY THIS IS NOT A QUALITY TRADE-OFF ───────────────────────────────
//
// ✔MEASURED (P36, this lane, re-derived from lane-f's `remap-j4.log` and
// confirmed identical across all four of its runs): today's program-stage
// `Inlining` performs **2922** cross-CU splices — 1643 + 646 + 225 + 408 over
// the four iterations of `release.pipeline.json`'s `{"fixpoint": {"max": 4}}`.
// That is the bar. Three properties of this design meet it structurally
// rather than by tuning:
//
//   (a) THE GATE IS UNCHANGED. The index decides AVAILABILITY; the existing
//       `inlineLegalityGate` decides LEGALITY, on the post-import module, with
//       the same code and the same shapes. So the only way quality can be lost
//       is by failing to make a body available.
//   (b) THE CANDIDATE FILTER IS PERMISSIVE. `isInlineCandidate` below refuses
//       only what the gate refuses UNCONDITIONALLY on facts a summary can
//       carry. Every borderline case is admitted and left to the gate.
//   (c) ★★ IMPORT IS LAZY, SO DEPTH IS NOT A QUALITY KNOB. The per-TU
//       optimizer runs its OWN fixpoint; when its gate wants a body it does
//       not have, it asks the index for it (`definitionOf`). The precomputed
//       `importPlan` is therefore a PREFETCH HINT, not a boundary.
//
// ★★★ AND (c) IS WHAT DISSOLVES THE TRANSITIVE-IMPORT FORK. Stated as
// "unbounded vs. zero", the fork has no good answer: unbounded degenerates
// back to the merged module (every TU pages in the whole program), and zero
// forfeits the 1279 splices today's iterations 2-4 contribute — 44% of the
// bar. But BOTH horns assume the import set must be decided up front. With a
// lazy edge, a TU pages in exactly the bodies its own gate asks for, which its
// own fixpoint bound (`max: 4`) already limits; depth then controls only how
// much is fetched SPECULATIVELY, and costs latency rather than quality.
// `maxImportDepth` below is consequently a prefetch tuning parameter with a
// documented no-quality-consequence guarantee — provided the lazy edge is
// actually wired. ⚠ Until it is, depth IS a quality bound: see
// [[D-OPT11-LAZY-IMPORT-EDGE]].
//
// ── AGNOSTIC ──────────────────────────────────────────────────────────────
// No language / target / object-format branch. `targetIdentity` is compared
// for equality only. Every container that feeds a DECISION is ordered or
// sorted before it is read — see the determinism note on `SummaryIndex`.

namespace dss::mirsum {

// The index's policy — 100% config-driven, sourced from the `OptPipeline` the
// build is already running. NOTHING here is a new invented constant.
struct SummaryIndexPolicy {
    // The cost bound, straight from `OptPipeline::inlineThreshold` (the
    // pipeline JSON's `inlineThreshold`, default 50). The candidate filter
    // applies the SAME `>` comparison `inlineLegalityGate` rule 6 applies, so
    // a callee the gate would refuse on size is never even offered.
    std::uint32_t inlineThreshold = 50;

    // How many call-graph edges deep to PREFETCH. Sourced from the Inlining
    // fixpoint's own `max` (4 in `release.pipeline.json`) because that is
    // exactly how many levels the in-module inliner can collapse in one run —
    // so a depth-`max` prefetch has already fetched everything a converging
    // fixpoint can ask for, and deeper is speculative.
    //
    // ⓘ The bound is NOT "N levels of arbitrary functions" — every edge must
    // first pass `isInlineCandidate`, so it is "N levels of functions already
    // small enough to inline". That is what keeps the frontier from
    // degenerating into the whole program: the transitive closure of small
    // leaf-ish helpers is small.
    std::uint32_t maxImportDepth = 4;

    // A per-module ceiling on PREFETCHED instructions, or 0 for no ceiling
    // (the default). Present because it is the one knob that turns this into
    // LLVM ThinLTO's budget-based import policy — the alternative to a depth
    // bound — without a second implementation: set a budget, raise the depth.
    // ⚠ A nonzero budget makes the outcome depend on the traversal ORDER in
    // which imports are charged, so `buildSummaryIndex` charges them in a
    // fully deterministic order (see the determinism note below) rather than
    // in whatever order a worklist happens to pop.
    std::uint32_t perModuleImportInstBudget = 0;
};

// One "module M should be able to inline function F, defined in module D"
// decision.
struct ImportDecision {
    std::uint32_t importerModule = 0;   // index into the `summaries` span
    std::uint32_t definingModule = 0;   // index into the `summaries` span
    std::string   calleeName;
    std::uint32_t calleeInstCount = 0;  // what it costs to import
    std::uint32_t depth = 0;            // call-graph edges from a root call site
};

// Where a name's winning DEFINITION lives.
struct DefiningSite {
    std::uint32_t moduleIndex = 0;
    std::uint32_t functionIndex = 0;   // index into that summary's `functions`
    bool          isFunction = true;   // false = a global definition
};

// ── Tier-2 cache-key inputs for one module ────────────────────────────────
//
// ★★★ THE SILENT MISCOMPILE THIS EXISTS TO PREVENT: keying the POST-IMPORT
// object on the TU alone means editing a callee leaves a STALE INLINED COPY of
// its old body inside the caller's cached object. The Tier-2 key must
// therefore compose the importer's own Tier-1 digest with the ordered set of
// (imported symbol, DEFINING MODULE'S Tier-1 digest) and the policy's own
// identity — so any change to any imported definition, or to the policy that
// chose it, invalidates the importer.
//
// The existing asymmetry rule decides every judgement call here:
// OVER-invalidation costs one recompile; UNDER-invalidation ships wrong bytes.
struct Tier2KeyInputs {
    std::uint32_t moduleIndex = 0;
    std::string   ownDigest;
    // (imported symbol name, defining module's Tier-1 digest), SORTED by name
    // — a set, not a sequence, so a reordering of the import plan cannot
    // change the key and cause a spurious rebuild.
    std::vector<std::pair<std::string, std::string>> importedFrom;
    // The policy's identity — a change to `inlineThreshold` or the prefetch
    // bounds changes what was inlined, so it MUST be in the key.
    std::string   policyIdentity;
};

// The whole-program answer.
//
// ── DETERMINISM ───────────────────────────────────────────────────────────
// Every field a decision reads is either an ORDERED vector or a map keyed by a
// value whose iteration is never used to make a choice. The hash maps here are
// LOOKUP tables only — `buildSummaryIndex` never range-fors one to decide
// anything, and every produced vector is sorted by a total order on
// (module, name). That is what lets N parallel TU optimizations produce a
// byte-identical artifact run to run.
struct SummaryIndex {
    // Externally-visible name → its winning definition. The winner is chosen
    // by `linker::resolveCrossCuDefs` — strong shadows weak, two-strong is a
    // reported conflict.
    std::unordered_map<std::string, DefiningSite> winners;

    // ★★★ THE WHOLE-PROGRAM ESCAPE SET — the union of every TU's
    // `escapedSymbolNames`. `inlineLegalityGate` rule 4 refuses a callee whose
    // address escapes ANYWHERE IN THE MODULE, and today "the module" is the
    // whole program. Handing this to each per-TU optimize is what keeps rule 4
    // as conservative as it is today; WITHOUT IT a callee whose address is
    // taken in TU B would be inlined in TU A and its out-of-line body could be
    // dropped while an indirect call still reaches for it.
    std::unordered_set<std::string> escapedSymbols;

    // ★★★ THE WHOLE-PROGRAM CALL-GRAPH SCC, name → scc id. `inlineLegalityGate`
    // rule 3 refuses a call whose caller and callee share an SCC — the
    // recursion bound. A per-TU SCC would miss mutual recursion that crosses a
    // TU boundary (`f` in A calls `g` in B calls `f`), and inlining inside
    // such a cycle unrolls an unbounded recursion at inline time.
    std::unordered_map<std::string, std::uint32_t> sccOf;

    // The whole-program LIVE symbol set — the `scanLiveSymbols` lift. Seeded
    // with externally-visible roots, expanded through `symbolRefs` and
    // `initSymbolRefs` to a fixpoint.
    std::unordered_set<std::string> liveSymbols;

    // Per module, the NAMED symbols it defines that are NOT live — what the
    // per-TU DCE may delete that a TU-local analysis could never prove.
    // Sorted per module.
    std::vector<std::vector<std::string>> deadSymbolsPerModule;

    // The prefetch plan, sorted by (importerModule, calleeName) so it is a
    // stable value regardless of how it was discovered.
    std::vector<ImportDecision> importPlan;

    // One row per module, in module order.
    std::vector<Tier2KeyInputs> tier2KeyInputs;

    // Names defined more than once with STRONG binding across TUs. Reported,
    // not silently resolved.
    std::vector<std::string> conflictingNames;

    // Look up a name's winning definition, or nullopt. THE LAZY IMPORT EDGE'S
    // entry point: a per-TU optimizer whose gate wants a body it does not have
    // asks here, rather than being confined to `importPlan`.
    [[nodiscard]] std::optional<DefiningSite>
    definitionOf(std::string const& name) const {
        auto const it = winners.find(name);
        if (it == winners.end()) return std::nullopt;
        return it->second;
    }
};

// Would `f` survive the parts of `inlineLegalityGate` a SUMMARY can decide?
//
// ⚠ PERMISSIVE BY CONTRACT. It must never exclude a callee the gate would
// accept — that is the only direction in which the index can silently lose
// inlining quality. It may admit one the gate will refuse; the cost is a
// wasted import. See the (b) clause in this file's header.
//
// `escaped` and `callerScc`/`calleeScc` are the WHOLE-PROGRAM facts; passing
// the per-TU ones would make this less conservative than today's merge.
[[nodiscard]] DSS_EXPORT bool
isInlineCandidate(SummaryFunction const& f, SummaryIndexPolicy const& policy,
                  std::unordered_set<std::string> const& escaped);

// Build the index. `summaries` are the per-TU summaries in a STABLE order the
// caller fixes (the driver's CU order) — module indices in the result refer to
// positions in this span.
//
// Returns nullopt only on a structural failure: a summary whose
// `targetIdentity` disagrees with the others (a summary mixed across targets,
// which the self-identity fields exist to catch). A two-strong symbol conflict
// is REPORTED and recorded in `conflictingNames`, and the build proceeds with
// the resolver's winner — matching `mergeCuMirs`, which does the same thing
// for the same reason.
[[nodiscard]] DSS_EXPORT std::optional<SummaryIndex>
buildSummaryIndex(std::span<ModuleSummary const> summaries,
                  SummaryIndexPolicy const&      policy,
                  DiagnosticReporter&            reporter);

} // namespace dss::mirsum
