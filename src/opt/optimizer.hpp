#pragma once

// OPT substrate — the pass engine that owns optimizer-tier MIR
// rewrites. `optimize(Mir, TargetSchema, TypeInterner, OptPipeline,
// reporter) → OptResult`. The signature is target-blind; `TargetSchema`
// is consumed only by LIR-tier passes (OPT5+) that don't exist yet.
// The MIR-tier passes read no target state — they're universally
// correct rewrites on the SSA-over-CFG vocabulary.
//
// **D-OPT1-VERIFY-AFTER-EVERY-PASS**: `optimize()` runs
// `MirVerifier::verify` after every successful pass under ALL build
// modes (plan 22 §3 PR1 unconditional directive). Catches pass-level
// invariant violations before downstream MIR/LIR/asm consumers
// cascade misleading failures.
//
// **D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD**: DCE (future) MUST
// consult `funcBinding / funcVisibility / globalBinding /
// globalVisibility` via `isExternallyVisible(binding, visibility)`
// before deleting any symbol. Substrate already on MirFunc + MirGlobal.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <span>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dss::opt {

// Closed pass-id vocabulary. Every shipped pass has a stable
// ordinal that pipelines reference. D-OPT1-PASS-ID-STABILITY:
// ordinals are part of the pipeline-as-config contract — pipelines
// reference passes by NAME at the JSON tier; ordinals here are
// internal. Adding a pass appends to the end (never renumbers).
enum class PassId : std::uint8_t {
    Identity     = 0,
    ConstFold    = 1,
    Dce          = 2,
    Mem2Reg      = 3,
    CopyProp     = 4,
    Cse          = 5,
    SimplifyCfg  = 6,
    Licm         = 7,
    Inlining     = 8,
};

// Single source-of-truth for the {ordinal, name} pairing.
// `optPassIdFromName` + `optPassIdName` + the runPass switch + the
// `kPassIdCount` static_assert all derive from this — adding a
// new enumerator without extending the table fails the static_assert
// at compile time (D-OPT1-PASS-ID-STABILITY enforcement).
inline constexpr std::size_t kPassIdCount = 9;
inline constexpr std::pair<PassId, std::string_view> kPassNameTable[kPassIdCount] = {
    {PassId::Identity,    "Identity"},
    {PassId::ConstFold,   "ConstFold"},
    {PassId::Dce,         "Dce"},
    {PassId::Mem2Reg,     "Mem2Reg"},
    {PassId::CopyProp,    "CopyProp"},
    {PassId::Cse,         "Cse"},
    {PassId::SimplifyCfg, "SimplifyCfg"},
    {PassId::Licm,        "Licm"},
    {PassId::Inlining,    "Inlining"},
};
static_assert(kPassIdCount == static_cast<std::size_t>(PassId::Inlining) + 1,
              "PassId enum / kPassIdCount drift — add a row to "
              "kPassNameTable + the runPass arm in optimizer.cpp's "
              "switch when you append a new PassId enumerator "
              "(D-OPT1-PASS-ID-STABILITY).");
// Closes the second half of D-OPT1-PASS-ID-STABILITY — guarantees
// kPassNameTable entries appear in enumerator-ordinal order, so
// `kPassNameTable[i].first` == `static_cast<PassId>(i)`. A name-only
// drift (e.g. swapping table entries) trips this at compile time.
[[nodiscard]] constexpr bool kPassNameTableInOrder() noexcept {
    for (std::size_t i = 0; i < kPassIdCount; ++i) {
        if (static_cast<std::size_t>(kPassNameTable[i].first) != i) return false;
    }
    return true;
}
static_assert(kPassNameTableInOrder(),
              "kPassNameTable entries must appear in PassId ordinal order");

[[nodiscard]] inline std::optional<PassId>
optPassIdFromName(std::string_view name) noexcept {
    for (auto const& [id, n] : kPassNameTable) {
        if (n == name) return id;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string_view
optPassIdName(PassId id) noexcept {
    auto const idx = static_cast<std::size_t>(id);
    return idx < kPassIdCount ? kPassNameTable[idx].second : "<unknown>";
}

// Default inline COST bound (OPT7 cycle 28): `OptPipeline::
// inlineThreshold` defaults to this when the pipeline JSON omits the
// key (and a programmatic `OptPipeline{}` construction inherits it).
// 50 MIR instructions is a conservative size cap — large enough to
// inline the small leaf/helper callees the c frontend emits,
// small enough that shipping `Inlining` in `release.pipeline.json`
// cannot blow up code size on a big callee. A size-based bloat bound;
// the SOPHISTICATED cost model (call-site hotness, growth-vs-benefit)
// remains deferred (D-OPT7-INLINE-LEGALITY-GATE).
inline constexpr std::uint32_t kDefaultInlineThreshold = 50;
// Substrate UPPER bound on `OptPipeline::inlineThreshold` — the loader
// rejects values outside [1, kMaxInlineThreshold]. 0 is a silent
// refuse-all trap (rejected at load); the cap is a large sanity bound
// (a callee with 100000+ MIR instructions is pathological, and a
// threshold that high effectively means "inline everything"). Width
// is uint32 (a callee's instruction-count can exceed a uint8/uint16
// range; `<cstdint>` keeps it GCC-portable).
inline constexpr std::uint32_t kMaxInlineThreshold = 100000;

// ★★★ THE PER-CALLER CUMULATIVE GROWTH BUDGET (P36 Lane R) — the bound
// `inlineThreshold` was never able to be.
//
// `inlineThreshold` is a PER-CALLEE size bound: it refuses ONE callee that
// is too big. NOTHING bounded CUMULATIVE growth — twenty callees of 49
// instructions each are twenty legal inlines under a threshold of 50 — so
// the only thing standing between the release pipeline and unbounded code
// growth was the fixpoint's iteration cap. ✔MEASURED (P36, 103-TU sqlite):
// with the cap raised the per-iteration splice count converges on EXACTLY
// 2x per iteration; the inlining fixpoint DIVERGES. `{"fixpoint": {"max"}}`
// was a growth bound wearing an iteration-count costume, which is why
// raising it traded a working compiler for an out-of-memory kill.
//
// `inlineCallerGrowthPercent` is that missing bound, stated where it
// belongs: a caller may grow, ACROSS THE WHOLE `optimize()` CALL (every
// fixpoint iteration together, via `passes::InlineGrowthLedger`), by at
// most this percentage of its OWN ORIGINAL instruction count — the size it
// had when the Inlining pass first saw it in this call.
//
// ★ WHY PER-CALLER AND NOT PER-MODULE, which is the design point that is
// easy to get subtly wrong: a MODULE-WIDE budget makes the emitted program
// depend on TRAVERSAL ORDER — on which caller happens to spend the shared
// budget first. The operator has already ruled on the neighbouring OPT11
// work that optimized output must be byte-identical for any prefetch depth,
// and the UNIT stage optimizes CUs CONCURRENTLY (`--jobs`), so a shared
// counter would not merely be order-sensitive, it would be racy. A
// per-caller budget is order-independent BY CONSTRUCTION: each caller's
// ceiling is a function of its own original size and nothing else, so no
// caller can consume another's. (GCC draws exactly this line: its
// `large-function-growth` is the per-function half we implement; its
// `inline-unit-growth` is the module-wide half we must not.)
//
// TERMINATION, which is the property the fixpoint never had: every caller
// has a FIXED ceiling that does not move as it grows, so total module size
// is bounded by SUM over f of (original(f) + allowance(f)) regardless of how
// many iterations run. Raising `max` can then only improve CONVERGENCE; it
// can no longer change how big the program gets.
inline constexpr std::uint32_t kDefaultInlineCallerGrowthPercent = 30;
// Substrate UPPER bound on `OptPipeline::inlineCallerGrowthPercent` — the
// loader rejects values outside [0, kMaxInlineCallerGrowthPercent].
//
// ⚠ 0 IS LEGAL HERE AND IS *NOT* A SILENT REFUSE-ALL (the reason
// `inlineThreshold` rejects 0 does not carry over): the allowance has a
// FLOOR of `inlineThreshold` instructions — see `inlineCallerGrowthPercent`
// — so growth 0 still admits one maximal legal callee per caller. It means
// "inline only what fits in the floor", a real and useful posture.
// The cap is a large sanity bound; at 100000% the budget is effectively
// unbounded, which is what a hand-built test fixture wants.
inline constexpr std::uint32_t kMaxInlineCallerGrowthPercent = 100000;

// ── The pipeline schedule tree (P10 operator ruling, 2026-08-18) ──────
//
// A pipeline document's `passes` array is a RECURSIVE SCHEDULE TREE the
// engine interprets depth-first, left-to-right. THREE node kinds,
// CLOSED — nothing else exists, by ruling:
//
//   Leaf      `"PassName"`                              one pass invocation
//   Repeat    {"repeat":  {"count": N, "passes":[…]}}   bounded unroll —
//             exactly N body traversals, NO convergence check, ever
//   Fixpoint  {"fixpoint": {"max":  N, "passes":[…]}}   the pre-tree
//             whole-pipeline `maxIterations` semantics VERBATIM (rerun
//             until a traversal mutates nothing, at most N), scoped to
//             any sub-sequence
//
// NO seq node (a combinator's `children` vector IS the sequence — the
// array is the sequence), NO conditionals (a `when:` would smuggle
// `if(arch)` back through config — forbidden), NO per-step pass params,
// NO parallel node. Unknown node kinds / unknown keys fail loud at load
// (X_PipelineMalformed, D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD).
//
// FLAT STAYS VALID: `passes: ["A","B"]` + top-level `maxIterations: N`
// desugars at load to ONE top-level Fixpoint{N, [A, B]} — the loop nest
// (iteration × passes) maps 1:1 onto the pre-tree engine's, so every
// observable (invocation order, counters, `iter=` trace bytes) is
// preserved exactly. ONE SPELLING PER DOCUMENT: a document using ANY
// structural node (repeat/fixpoint) REFUSES top-level `maxIterations`.
//
// Every loaded document NORMALIZES to a root Fixpoint{N ≥ 1, […]} —
// N = the flat document's `maxIterations`, or 1 for a structural
// document — with one deliberate collapse: a document whose entire
// `passes` is ONE `fixpoint` node uses THAT node as the root, so a
// structural `fixpoint{max:4, [9 passes]}` desugars to EXACTLY the flat
// `passes` + `maxIterations: 4` twin's schedule (identical tree,
// identical counters, depth-1 `iter=` trace).
//
// Members are public + assignable (`OptPipeline` must stay
// move-assignable — compile_pipeline rebinds a loaded pipeline by
// move), but a built tree is IMMUTABLE BY CONVENTION: nothing outside
// the loader and the factories below ever writes it; the engine only
// reads.
struct OptPipelineNode {
    enum class Kind : std::uint8_t { Leaf, Repeat, Fixpoint };

    // The default-constructed node is the EMPTY SCHEDULE — a Fixpoint
    // over no passes. It computes ZERO pass invocations, and
    // `optimize()` refuses it, exactly as the pre-tree `OptPipeline{}`
    // (empty `passes` list) was refused. Use [Identity] for an explicit
    // no-op.
    Kind                        kind = Kind::Fixpoint;
    PassId                      passId = PassId::Identity;  // Leaf only
    std::uint32_t               count = 1;                  // Repeat/Fixpoint bound
    std::vector<OptPipelineNode> children;                  // Repeat/Fixpoint body

    [[nodiscard]] static OptPipelineNode leaf(PassId id) {
        OptPipelineNode n;
        n.kind = Kind::Leaf;
        n.passId = id;
        n.count = 1;
        return n;
    }
    [[nodiscard]] static OptPipelineNode
    repeat(std::uint32_t count_, std::vector<OptPipelineNode> body) {
        OptPipelineNode n;
        n.kind = Kind::Repeat;
        n.count = count_;
        n.children = std::move(body);
        return n;
    }
    [[nodiscard]] static OptPipelineNode
    fixpoint(std::uint32_t max_, std::vector<OptPipelineNode> body) {
        OptPipelineNode n;
        n.kind = Kind::Fixpoint;
        n.count = max_;
        n.children = std::move(body);
        return n;
    }

    // Structural identity — the loader's flat-vs-structural
    // desugaring pin compares loaded trees with this.
    bool operator==(OptPipelineNode const&) const = default;
};

// Leaf-sequence helper: {A, B, C} → [leaf(A), leaf(B), leaf(C)].
[[nodiscard]] inline std::vector<OptPipelineNode>
optPipelineLeaves(std::vector<PassId> ids) {
    std::vector<OptPipelineNode> out;
    out.reserve(ids.size());
    for (auto id : ids) out.push_back(OptPipelineNode::leaf(id));
    return out;
}

// Substrate bound on EVERY repeat `count` / fixpoint `max` (and the
// flat document's top-level `maxIterations`) — the loader rejects
// values outside [1, kMaxPipelineIterations]. The pre-tree whole-
// pipeline bound, unchanged: 0 is a silent no-op trap; 32 is large
// enough for any realistic mutually-enabling cluster (ConstFold +
// SimplifyCfg + DCE converge in O(log #blocks) in practice).
inline constexpr std::uint8_t kMaxPipelineIterations = 32;

// LOAD-TIME BUDGETS (fail-loud, all X_PipelineMalformed at load):
// nesting depth — the number of combinator (repeat/fixpoint) nodes on
// the longest root-to-leaf chain, COUNTING the normalized root —
// ≤ kMaxPipelineDepth; and total worst-case unrolled invocations
// ≤ kMaxPipelineInvocations, computed by the ONE shared static cost
// function below so the loader and the engine's entry guard can never
// disagree about what a schedule runs. Empty `passes` bodies are
// rejected at parse, BEFORE cost evaluation (existing empty-pipeline
// discipline).
inline constexpr std::uint8_t  kMaxPipelineDepth       = 8;
inline constexpr std::uint16_t kMaxPipelineInvocations = 4096;

// The ONE shared static cost function: worst-case pass invocations.
// Leaf = 1, sequence = Σ, Repeat{n,b} = n·cost(b), Fixpoint{m,b} =
// m·cost(b) — with the fixpoint max:0 → one-traversal clamp applied
// HERE too (the interpreter runs exactly one traversal on max:0), so
// cost(schedule) is precisely what the engine would run at the cap.
// Saturates just above the budget so chained multiplies can never
// overflow regardless of how pathological a programmatic tree is.
[[nodiscard]] inline std::uint64_t
optPipelineCost(OptPipelineNode const& n) {
    switch (n.kind) {
    case OptPipelineNode::Kind::Leaf:
        return 1;
    case OptPipelineNode::Kind::Repeat:
    case OptPipelineNode::Kind::Fixpoint: {
        std::uint64_t const eff =
            (n.kind == OptPipelineNode::Kind::Fixpoint && n.count == 0)
                ? std::uint64_t{1}
                : n.count;
        std::uint64_t body = 0;
        for (auto const& c : n.children) {
            body += optPipelineCost(c);
            if (body > kMaxPipelineInvocations) return kMaxPipelineInvocations + 1;
        }
        std::uint64_t const total = body * eff;
        return total > kMaxPipelineInvocations ? kMaxPipelineInvocations + 1
                                               : total;
    }
    }
    return 0;
}

// A pipeline is a SCHEDULE TREE of passes to run on each MIR function.
// Loaded from `src/dss-config/pipelines/*.pipeline.json`
// (D-OPT1-PIPELINE-FROM-CONFIG) or constructed inline by tests +
// the examples_runner's differential-verify arm
// (D-OPT1-DIFFERENTIAL-VERIFY-RUNNER).
//
// `name` is OWNED (std::string) — a view over a parsed-JSON
// owned-string would dangle the moment the pipeline outlives the
// source json::value. Owned (not view) — pipeline outlives its
// source json::value without lifetime audit.
struct OptPipeline {
    std::string       name;
    // P10 stage topology (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE): optional
    // NAME of the pipeline document that runs at the UNIT (per-CU) stage
    // instead of this one — read by `optimizeModule`'s stage routing, at
    // the Unit site only. EMPTY = this document runs at both stages (the
    // pre-P10 behavior; what `debug` ships). A NAME, never an embedded
    // schedule: the grammar owns schedules, this key owns only topology.
    // The Program stage ignores it — the link-time schedule is always
    // the config's own document.
    std::string       unitPipelineName;
    // The schedule tree (see OptPipelineNode). After any load or
    // flat() construction the root is a Fixpoint — uniform interpreter
    // entry, uniform `iter=` numbering.
    OptPipelineNode   schedule;
    // Inline COST MODEL (OPT7 cycle 28) — a size-based bloat bound. The
    // §2.9 legality gate inlines a callee ONLY IF its instruction-count
    // is `<= inlineThreshold`; a callee larger than this is conservatively
    // REFUSED (too large to inline profitably). This is the FIRST inline
    // profitability heuristic — it bounds the code-size growth shipping
    // `Inlining` in `release.pipeline.json` can cause. Config-driven (the
    // pipeline JSON's optional `inlineThreshold`) — NOT a language /
    // target / format branch. Default = `kDefaultInlineThreshold` (the
    // value the loader fills when the key is absent + a programmatic
    // construction inherits). FAIL-SAFE: a threshold BELOW the smallest
    // callee refuses everything; threshold 1 (the loader's minimum) admits
    // only a 1-instruction callee. The loader rejects 0 (a silent
    // refuse-all trap) and caps at `kMaxInlineThreshold`.
    std::uint32_t       inlineThreshold = kDefaultInlineThreshold;
    // The PER-CALLER CUMULATIVE growth budget (see
    // `kDefaultInlineCallerGrowthPercent` for the whole argument). A caller
    // may grow across the WHOLE optimize() call by at most
    //     max(original * inlineCallerGrowthPercent / 100, inlineThreshold)
    // instructions, where `original` is its size the first time the Inlining
    // pass saw it in this call. The `inlineThreshold` FLOOR is not a fudge
    // factor: without it the per-callee threshold would be a lie for small
    // callers (a 4-instruction wrapper could never absorb the 30-instruction
    // helper the threshold says is inlinable), and wrappers are precisely
    // where inlining pays. Config-driven (the pipeline JSON's optional
    // `inlineCallerGrowthPercent`) — NOT a language / target / format branch.
    std::uint32_t       inlineCallerGrowthPercent =
        kDefaultInlineCallerGrowthPercent;
    // Verify frequency (D-OPT1-VERIFY-FREQUENCY-CONFIG). `true` (the safe
    // default) runs `MirVerifier` after EVERY successful pass — the developer
    // posture (LLVM `opt -verify-each` / GCC `--enable-checking=yes`): it
    // pinpoints the exact pass that produced invalid MIR. `false` runs the
    // verifier ONCE after the whole pipeline (before codegen consumes the
    // module) — the release/production posture (LLVM/GCC trust their tested
    // passes + verify at boundaries). Per-pass verify over a large module
    // (SQLite) is ~N-passes × N-iterations full-module verifies — minutes; the
    // once-at-end verify still catches a structurally-broken optimizer output
    // before it corrupts LIR/asm. Config-driven per pipeline (NOT a build-mode
    // branch in the engine), so `release.pipeline.json` can opt into verify-each
    // on demand when hunting a release-only miscompile.
    bool                verifyEveryPass = true;

    OptPipeline() = default;
    OptPipeline(OptPipeline const&) = default;
    OptPipeline(OptPipeline&&) = default;
    OptPipeline& operator=(OptPipeline const&) = default;
    OptPipeline& operator=(OptPipeline&&) = default;

    // The FLAT factory — the pre-tree construction shape (name, pass
    // list, whole-pipeline iteration bound). Desugars to
    // Fixpoint{maxIterations, [passes…]}: the loop nest (iteration ×
    // passes) maps 1:1 onto the pre-tree engine's, so a flat() build
    // behaves observably identically to the old aggregate build.
    // maxIterations 0 is NOT rejected here (programmatic construction)
    // — the engine clamps it to ONE traversal exactly as the pre-tree
    // engine did; the LOADER rejects it at load time.
    [[nodiscard]] static OptPipeline
    flat(std::string name_, std::vector<PassId> passes_,
         std::uint8_t maxIterations = 1) {
        OptPipeline p;
        p.name = std::move(name_);
        p.schedule = OptPipelineNode::fixpoint(
            maxIterations, optPipelineLeaves(std::move(passes_)));
        return p;
    }

    // Construction compatibility with the pre-tree aggregate shapes —
    // every existing `OptPipeline{"name", {passes…}[, maxIterations]}`
    // call site keeps compiling and desugars through flat() (ONE
    // desugaring path; the compatibility ctors add no second meaning).
    OptPipeline(std::string name_, std::vector<PassId> passes_)
        : OptPipeline(flat(std::move(name_), std::move(passes_), 1)) {}
    OptPipeline(std::string name_, std::vector<PassId> passes_,
                std::uint8_t maxIterations_)
        : OptPipeline(flat(std::move(name_), std::move(passes_),
                           maxIterations_)) {}
};

// `LoadResult<T>` mirrors `TargetSchema::LoadResult` / `ObjectFormatSchema::LoadResult`.
// Same 7-step loader shape across all config tiers.
template <class T>
using LoadResult = std::expected<T, std::vector<ConfigDiagnostic>>;

// Load a pipeline by name. Walks ancestors of CWD looking for
// `src/dss-config/pipelines/<name>.pipeline.json` (shared
// `findShippedConfig` substrate). Returns the loaded pipeline or
// the accumulated config diagnostics. Distinguishes "file not
// found" (X_PipelineNameResolutionFailed) from "file malformed"
// (X_PipelineMalformed / X_PipelineVersionMismatch /
// X_UnknownPassName) at the diagnostic-code level.
[[nodiscard]] DSS_EXPORT LoadResult<OptPipeline>
loadShippedPipeline(std::string_view name);

// Parse pipeline JSON from in-memory text. `sourceLabel` is the
// path or synthetic name attached to every emitted diagnostic.
// Used by `loadShippedPipeline` AND directly by tests + the
// examples_runner (so a manifest's inline `optimizedPipelines: [...]`
// can build pipelines without touching the filesystem).
[[nodiscard]] DSS_EXPORT LoadResult<OptPipeline>
loadPipelineFromText(std::string_view jsonText,
                     std::string_view sourceLabel);

// Structured optimizer result. `ok` is the equivalent of the old
// `bool` return. `passesRun` + `passesMutated` are CUMULATIVE across
// the whole schedule tree — every node's invocations accumulate into
// the same call-global counters (a pipeline with a fixpoint{max:4}
// over 7 passes that runs 3 iterations reports `passesRun = 21`).
// `fixedPointReached` is true iff EVERY executed fixpoint node
// converged before its cap (a full traversal produced zero new
// `passesMutated`); for the single-root schedules every loaded or
// flat()-built pipeline has, this is exactly the pre-tree rule.
// Default = `false` so an early-return path (verifier failure,
// substrate-contract violation) doesn't masquerade as "converged."
//
// `passMutationCount[PassId]` is per-pass cumulative mutation count
// (D-OPT-PASS-METRICS). Each entry records how many iterations of
// the pipeline-level loop where that PassId returned mutated=true.
// This is the EFFECTIVENESS-signal substrate: a test asserting
// `passMutationCount[ConstFold] >= 2` proves the mutually-enabling
// cluster fired (ConstFold ran at least twice — once originally,
// once post-Mem2Reg via the fixed-point loop). Sized by the closed
// PassId enum's count so the `static_assert(kPassIdCount == ...)`
// drift guard keeps this array honest against future PassId growth.
struct OptResult {
    bool        ok                = false;
    std::size_t passesRun         = 0;
    std::size_t passesMutated     = 0;
    bool        fixedPointReached = false;
    std::array<std::size_t, kPassIdCount> passMutationCount = {};

    // Typed accessor — preferred over raw `passMutationCount[
    // static_cast<size_t>(PassId::X)]` at call sites; the cast is
    // hidden behind a single bounds-checked-by-type entry point.
    [[nodiscard]] std::size_t mutationCount(PassId id) const noexcept {
        return passMutationCount[static_cast<std::size_t>(id)];
    }
};

// Run the configured pipeline over every function in `mir`. Returns
// the same MIR object (each pass rebuilds it functionally via
// `MirBuilder` and replaces it). The reporter receives any `X_*`
// diagnostic the pipeline emits.
//
// `TypeInterner const&` is consumed by the verifier hook
// (D-OPT1-VERIFY-AFTER-EVERY-PASS) for the interner-gated rule set
// (CondBr-is-Bool, Return-matches-FnSig, Arg-in-range, no-Extension-
// types). Without it, `checkTypeInvariants` is silently skipped.
// `externImports` is the module's extern declaration table — the SAME vector the
// LOWER half moves into MIR→LIR. It is read for exactly one thing: the
// unconditional inline-definition strip epilogue
// (D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED, C99 6.7.4p7). A
// function whose SymbolId also appears here is an inline definition — a body the
// module carries so the inliner may use it, and which must NEVER be emitted —
// and the epilogue removes it after the pipeline has had its chance to inline it.
//
// ⚠ DEFAULTS TO EMPTY, AND THE DEFAULT IS "STRIP NOTHING". That is right for
// every hand-built MIR fixture (a module with no extern table cannot contain the
// function/extern pair by construction), and it is safe rather than merely
// convenient: an omitted table leaves the module exactly as the pipeline left it.
// The real front end always passes its table, so the only way to reach an
// unstripped inline definition is to build one by hand and then not declare it.
[[nodiscard]] DSS_EXPORT OptResult optimize(Mir& mir,
                                            TargetSchema const& target,
                                            TypeInterner const& interner,
                                            OptPipeline const& pipeline,
                                            DiagnosticReporter& reporter,
                                            std::span<ExternImport const>
                                                externImports = {});

} // namespace dss::opt
