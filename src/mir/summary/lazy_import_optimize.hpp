#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "mir/mir.hpp"
#include "mir/summary/summary_index.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
// OPT11 — THE LAZY IMPORT EDGE (plan 22 §0.2, D-OPT11-LAZY-IMPORT-EDGE)
// ═══════════════════════════════════════════════════════════════════════════
//
// `summary_index.hpp` is the DECISION half: it reads only summaries and answers
// "which body is where, and which ones may be imported at all". This file is the
// TRANSFORMATION half's missing edge: a per-TU optimize that, instead of being
// confined to a precomputed `importPlan`, ASKS the index for a body the moment
// its own module names one it does not have — `SummaryIndex::definitionOf`.
//
// ── ★★★ WHY THE EDGE HAS TO BE LAZY, IN ONE SENTENCE ──────────────────────
// An eager plan is computed from the summaries, and the summaries describe the
// module BEFORE it is optimized — so a direct call that `ConstFold` / `CopyProp`
// only exposes MID-FIXPOINT (a callee reached through a folded function pointer)
// names a callee no eager plan ever offered, and today's merged module splices it
// while an eager index would miss it. The lazy edge closes exactly that gap.
//
// ── ★★★ THE INVARIANT THIS FILE MUST NOT BREAK ────────────────────────────
// **The index decides AVAILABILITY; the unchanged `inlineLegalityGate` decides
// LEGALITY** on the post-import module. Nothing here inlines anything. It makes
// bodies PRESENT and then runs the ordinary optimizer, whose gate is the same
// code making the same decision on the same shapes. So the only way this file can
// lose inlining quality is by failing to make a body available — never by
// admitting one, which costs a wasted import and nothing else.
//
// ── ★★ THE TWO NESTED FIXPOINTS, AND WHAT EACH ONE BOUNDS ─────────────────
//
//   INNER — THE AVAILABILITY CLOSURE. Import every candidate the CURRENT module
//   names, then every candidate THOSE bodies name, until nothing new appears.
//   It is NOT depth-bounded: what keeps it from degenerating into the whole
//   program is `isInlineCandidate`, so the closure is "the transitive closure of
//   functions already small enough to inline", which is small. `maxImportDepth`
//   chunks the FETCH into batches of that many call-graph levels and changes
//   nothing else — the closure is computed in full BEFORE anything is cloned, so
//   a round costs exactly ONE merge whatever the depth.
//
//   OUTER — RE-OPTIMIZE ON NEW DEMAND. After `optimizeOne` runs, the module may
//   name callees it did not name before (the devirtualization case above). If it
//   does, close availability again and optimize again. Bounded by
//   `kMaxImportRounds` — deliberately NOT by the prefetch knob — and a round that
//   hits the bound with demand outstanding is REPORTED, never silently dropped.
//
// ★★★ AND THAT IS WHAT DISSOLVES THE §B TRANSITIVE-IMPORT FORK rather than
// settling it. The fork (A: a depth-bounded closure at K; B: a ThinLTO-style
// per-TU instruction-budget decay) is a QUALITY trade only while the edge is
// eager, because then depth is the last word on what a TU may ever see. Here the
// inner closure runs to exhaustion, so `maxImportDepth` cannot change WHICH
// bodies end up available — only how many batches it takes to fetch them.
// ✔The claim is checked by execution, not by argument:
// `LazyImportEdge.PrefetchDepthChangesTheBatchCountAndNothingElse` in
// `tests/mir/test_lazy_import_optimize.cpp` runs a four-module call chain at
// depth 1 and depth 4 and compares the resulting module structure, the bodies
// imported and the clone count — all equal — against the FETCH batch count,
// which moves 3 → 1.
// `perModuleImportInstBudget` (option B) remains honoured and remains the one
// knob that DOES change the outcome — so the fork's two horns are no longer
// alternatives: A is not a bound at all, and B is a budget the operator may set.
//
// ── ★★ THE AVAILABLE-EXTERNALLY MARKING, AND WHY NOTHING NEW WAS INVENTED ──
// An imported body must be present for the inliner and must NEVER be emitted —
// a second definition of a symbol another object already defines. DSS already
// has that exact mechanism and has had it since C99 6.7.4p7 support landed: a
// function whose SymbolId ALSO appears in the module's `ExternImport` table is
// an inline definition, and `optimize()`'s unconditional `stripInlineDefinitions`
// epilogue removes it after the pipeline has had every traversal to consume it.
// So an import is marked by ADDING AN EXTERN ROW, and the strip is the existing
// one. No new flag, no new pass, no new configuration surface.
//
// ── ★ WHAT AN IMPORT COSTS THE MODULE, AND WHY IT STILL LINKS ─────────────
// An imported body references symbols the importer does not define. Every such
// reference becomes an ordinary non-eager `ExternImport` — a cross-TU `extern`
// declaration, the same row a hand-written `extern int f(void);` produces — and
// the whole-program merge that runs afterwards resolves it to the real
// definition exactly as it resolves any other. A reference that CANNOT be
// satisfied that way (a `static` object of the source TU, which has identity and
// state and must never be duplicated) makes the candidate UNIMPORTABLE, and the
// refusal happens BEFORE the import: that is an AVAILABILITY decision, the only
// kind this file is allowed to make.
//
// ── AGNOSTIC ──────────────────────────────────────────────────────────────
// No language / target / object-format branch. `targetIdentity` is compared for
// equality only, and the optimizer is reached through a CALLBACK so the MIR tier
// never learns what a pipeline or a compile stage is.

namespace dss::mirsum {

// ★★ THE OUTER LOOP'S BOUND, AND IT IS DELIBERATELY *NOT* `maxImportDepth`.
//
// An outer round happens only when an `optimizeOne` EXPOSED a direct call no
// earlier round could see — a devirtualization cascade. Tying the bound to the
// prefetch knob would make a latency setting decide how much cross-CU inlining a
// program gets, which is exactly the property this arc has to guarantee it does
// NOT do. So it is a substrate constant with its own argument, on the same
// footing as `kMaxPipelineIterations`: a program that needs a ninth cascade is
// pathological, and hitting the cap is REPORTED (X_OptFixpointTruncated) rather
// than silently costing quality with no witness.
inline constexpr std::uint32_t kMaxImportRounds = 8;

// One TU as the lazy import edge sees it. DECOMPOSED for the same reason
// `MergeCuInput` is: a MIR-tier test can hand-build every field.
//
// ⚠ `symbolNames` IS A TABLE, NOT A CALLBACK, AND THAT IS A CONCURRENCY RULE.
// N importers run in parallel and every one of them reads every OTHER TU's
// names. A `std::function` reaching into a live `SemanticModel` would put N
// threads inside one CU's symbol table; a flat table built once, serially,
// cannot. Index is `SymbolId.v`; an id past the end, or an empty entry, means
// "this symbol has no declared name" — module-private, never matched across TUs.
struct LazyImportCu {
    Mir const*                      mir = nullptr;
    // ★ THE BARE INTERNER, NOT A LATTICE, AND THAT IS WHAT LETS A BODY ARRIVE
    // FROM A `.dss.mir` SECTION. `decodeModuleBody` hands back a module and the
    // `TypeInterner` its types were re-interned into — there is no lattice on
    // that path, and demanding one here would have made the encoded provider
    // unusable and the section decorative. The one fact a lattice carries that
    // an interner does not (its registry's source language) is a property of the
    // BUILD, not of a CU, so `lazyImportOptimize` takes it as one argument.
    TypeInterner const*             interner = nullptr;
    std::vector<std::string> const* symbolNames = nullptr;
    std::span<ExternImport const>   externImports;
    // This TU's REFERENCED-ONLY shipped-library shim symbols (SymbolId.v →
    // recipe id), the same map `MergeCuInput::synthRecipes` takes and for the
    // same reason: a shim is referenced with NO definition and NO extern row, so
    // a clone of a body that names one ABORTS unless the planner was told the
    // symbol exists. Read for the IMPORTER only — a shim referenced by a body
    // being imported FROM another TU makes that body unimportable, because the
    // importer has no way to learn which recipe synthesizes it.
    std::unordered_map<std::uint32_t, std::string> const* synthRecipes = nullptr;
};

// What one TU's lazy-import optimize produced.
//
// ⚠ AN IMPORT REPLACES THE TU'S TYPE LATTICE AND SYMBOL NUMBERING. Cloning a
// body across a TU boundary re-interns its types into a fresh host lattice and
// unifies the two symbol spaces, so the module handed back no longer belongs to
// the interner it arrived in. `host` / `symbolNames` are populated IFF
// `importedBodies > 0`, and a caller that keeps the old interner in that case is
// reading a module through the wrong lattice — which is why they are returned
// together with the module rather than left for the caller to remember.
struct LazyImportOutcome {
    bool          ok             = false;
    // ★ THE REWRITTEN MODULE, ENGAGED IFF SOMETHING WAS IMPORTED.
    //
    // ⚠ RETURNED RATHER THAN WRITTEN THROUGH A REFERENCE, AND THAT IS A
    // CONCURRENCY REQUIREMENT, NOT A STYLE PREFERENCE. N importers run at once
    // and every one of them reads every OTHER TU's module as a potential import
    // SOURCE. An in-place rewrite would have TU #7 mutating the very module TU
    // #3 is cloning out of. With every input const, the stage is shared-nothing
    // by construction and the caller installs the results serially afterwards.
    std::optional<Mir> mir;
    std::uint32_t importedBodies = 0;   // distinct bodies paged in
    std::uint32_t importBatches  = 0;   // FETCH batches (`maxImportDepth` levels each)
    std::uint32_t importMerges   = 0;   // clone calls — ONE per round that imported
    std::uint32_t optimizeRuns   = 0;   // `optimizeOne` invocations (>= 1)
    // ⓘ THE FORK-DISSOLUTION INSTRUMENT, AND IT TAKES ALL THREE COUNTERS TO
    // STATE THE CLAIM. `importedBodies` and `importMerges` must NOT depend on
    // `maxImportDepth` — that is "depth is not a quality knob", and it is also
    // what keeps the merge's symbol renumbering out of a latency knob's hands.
    // `importBatches` MUST depend on it — that is "depth is a prefetch knob",
    // and without it a test would also pass on an implementation that had
    // silently stopped honouring depth at all.
    std::uint32_t demandLeftAtBound = 0;

    // Populated IFF `importedBodies > 0` (see the type's note).
    std::unique_ptr<TypeLattice>                   host;
    std::unordered_map<std::uint32_t, std::string> symbolNames;
    std::vector<ExternImport>                      externImports;
    // The importer's shim-recipe map, RE-KEYED onto the merged symbol ids.
    // ⚠ The caller must carry this forward instead of its original: a merge
    // renumbers, so the map that went in describes symbols that no longer exist,
    // and handing the stale one to the whole-program merge aborts the build on
    // the first `GlobalAddr` naming a shim.
    std::unordered_map<std::uint32_t, std::string> synthRecipes;
};

// ★★★ THE PRECONDITION EVERY ON-DEMAND FETCH RESTS ON, CHECKED ONCE.
//
// `DefiningSite` names a body by an ORDINAL into the defining module's summary,
// and "summary function #i IS `mir.funcAt(i)`" holds only while that summary was
// built from THAT module. Pair a summary with the wrong module — a cached one, a
// stale one, one from a sibling target — and every fetch resolves to a real
// function with the WRONG BODY. An optimizer that inlines the wrong body is a
// silent miscompile with no diagnostic anywhere, which is the one outcome this
// arc may not have.
//
// ⚠ THE KEY IS NOT THE ORDINAL AND NOT A DECLARED NAME EITHER — it is the
// CORRESPONDENCE, verified here against content the summary cannot fake: every
// summarized function's `symbol` AND declared name must equal the module's own,
// at the same ordinal, for functions and globals alike. ✔That is the P36 lesson
// (`CuBuildKey::languageName` keyed on a declared name and was fixed) applied
// one tier out: a name is a lookup key, never a proof of identity.
//
// O(functions + globals) over the whole program, ONCE — never per importer.
// Call it before the first `lazyImportOptimize`; a false return means the index
// must not be used at all.
[[nodiscard]] DSS_EXPORT bool
summariesDescribeModules(std::span<LazyImportCu const>  cus,
                         std::span<ModuleSummary const> summaries,
                         DiagnosticReporter&            reporter);

// The optimizer, as a callback. `(mir, interner, externImports) -> ok`.
//
// It is a callback rather than a direct `dss::opt::optimize` call so this file
// stays a MIR-tier leaf: the driver supplies the stage, the pipeline document
// and the target, none of which the import edge has any business knowing. A
// test supplies an inliner-only pipeline, or a no-op.
using LazyOptimizeFn =
    std::function<bool(Mir&, TypeInterner const&, std::span<ExternImport const>)>;

// Run the lazy-import optimize for ONE TU.
//
// `importer` indexes `cus` / `summaries` (the driver's CU order — the same order
// `buildSummaryIndex` was handed). The importer's own module is `cus[importer]`,
// read like any other; the rewritten one comes back in `LazyImportOutcome::mir`,
// engaged only if a body was actually imported.
//
// ★ REENTRANT AND SHARED-NOTHING. Every argument but `reporter` is const, so N
// calls may run concurrently over one `SummaryIndex` and one set of summaries.
// `reporter` must be per-importer (the driver's established scratch-reporter
// shape) — it is the one shared-mutable sink otherwise.
[[nodiscard]] DSS_EXPORT LazyImportOutcome
lazyImportOptimize(std::uint32_t                   importer,
                   std::span<LazyImportCu const>   cus,
                   std::span<ModuleSummary const>  summaries,
                   SummaryIndex const&             index,
                   SummaryIndexPolicy const&       policy,
                   std::string_view                sourceLanguage,
                   LazyOptimizeFn const&           optimizeOne,
                   DiagnosticReporter&             reporter);

// ── the pieces, exposed because each one is separately provable ────────────

// Every symbol NAME `fn` of `mir` references, through ANY of the three carriers
// a cross-module clone must remap: a `GlobalAddr`, a `BlockAddressExport`, and a
// symbol-address value nested anywhere inside a `Const` instruction's literal.
//
// ⚠ THE THIRD CARRIER IS WHY THIS IS NOT `SummaryFunction::symbolRefs`. The
// summary records `GlobalAddr` references only — enough for the liveness BFS it
// was built for, and NOT enough to decide whether a body can be moved. Deciding
// importability from the summary alone would silently admit a body whose literal
// names a symbol the importer cannot resolve.
//
// `sawUnnamed` is set when a referenced symbol has no declared name AND is not a
// block-export symbol (which is anonymous by construction and cloned as a fresh
// module-private symbol). An unnamed reference cannot be matched across a TU
// boundary at all, so it makes the body unimportable.
struct BodyReferences {
    std::vector<std::string> names;        // sorted + deduplicated
    bool                     sawUnnamed = false;
};
[[nodiscard]] DSS_EXPORT BodyReferences
collectBodyReferences(Mir const& mir, MirFuncId fn,
                      std::span<std::string const> symbolNames);

// Can `calleeName`'s winning definition be imported INTO module `importer`?
// The availability half of the decision, and the whole of it: it applies
// `isInlineCandidate` (the permissive summary filter) and then the
// satisfiability rule above. It never asks whether the gate would inline the
// call — that is the gate's word, on the post-import module.
[[nodiscard]] DSS_EXPORT bool
isImportable(std::uint32_t importer, std::string const& calleeName,
             std::span<LazyImportCu const> cus,
             std::span<ModuleSummary const> summaries,
             SummaryIndex const& index, SummaryIndexPolicy const& policy);

} // namespace dss::mirsum
