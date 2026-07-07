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
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
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
// inline the small leaf/helper callees the c-subset frontend emits,
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

// A pipeline is an ordered list of passes to run on each MIR
// function. Loaded from `src/dss-config/pipelines/*.pipeline.json`
// (D-OPT1-PIPELINE-FROM-CONFIG) or constructed inline by tests +
// the examples_runner's differential-verify arm
// (D-OPT1-DIFFERENTIAL-VERIFY-RUNNER).
//
// `name` is OWNED (std::string) — a view over a parsed-JSON
// owned-string would dangle the moment the pipeline outlives the
// source json::value. Owned (not view) — pipeline outlives its
// source json::value without lifetime audit.
struct OptPipeline {
    std::string         name;
    std::vector<PassId> passes;
    // Pipeline-level fixed-point loop (D-OPT-FIXED-POINT-LOOP +
    // D-OPT1-PASS-RUN-MAX-ITER): the engine reruns the entire
    // `passes` sequence up to `maxIterations` times or until a full
    // iteration produces zero `passesMutated`. Default = 1 (single
    // pass, the historical behavior). Set higher in pipelines that
    // include mutually-enabling passes (e.g. ConstFold + SimplifyCfg
    // + Dce — ConstFold folds `if (5<3)` to `if (false)`, SimplifyCfg
    // folds the CondBr AND prunes the dead arm its own fold disconnects
    // (post-fold reachability — the rebuild emits no orphan block), then
    // DCE sweeps the now-dead instructions, potentially exposing more
    // ConstFold opportunities). The loader rejects 0 (silent
    // no-op trap) and caps at 32 (an upper bound large enough for
    // any realistic mutually-enabling cluster — anything more is
    // either a non-converging pass or a pathological input).
    std::uint8_t        maxIterations = 1;
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
};

// Substrate bound on `OptPipeline::maxIterations` — the loader
// rejects values outside [1, kMaxPipelineIterations]. 32 fits in
// uint8_t comfortably and is large enough for any realistic
// mutually-enabling cluster (ConstFold + SimplifyCfg + DCE converge
// in O(log #blocks) in practice). Width chosen for invariant
// expression: a u16 admits 0 and 33..65535 — silent traps the
// loader's runtime check has to catch.
inline constexpr std::uint8_t kMaxPipelineIterations = 32;

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
// all iterations of the pipeline-level fixed-point loop (a pipeline
// with `maxIterations=4` and 7 passes that runs 3 iterations reports
// `passesRun = 21`). `fixedPointReached` is true iff a full iteration
// produced zero `passesMutated` (mutually-enabling cluster converged).
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
[[nodiscard]] DSS_EXPORT OptResult optimize(Mir& mir,
                                            TargetSchema const& target,
                                            TypeInterner const& interner,
                                            OptPipeline const& pipeline,
                                            DiagnosticReporter& reporter);

} // namespace dss::opt
