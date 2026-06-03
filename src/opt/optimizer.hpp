#pragma once

// OPT1 cycle 1 substrate — the pass engine that owns optimizer-tier
// MIR rewrites. Plan 22 PR1 spec: `optimize(Mir, TargetSchema,
// OptPipeline, reporter) → Mir`. The signature is target-blind; the
// `TargetSchema` is consumed only by the LIR-tier passes (OPT5+) that
// don't exist yet. The MIR-tier passes (OPT2 const-fold / DCE / copy-
// prop / peephole) read no target state — they're universally correct
// rewrites on the SSA-over-CFG vocabulary.
//
// **Cycle 1 scope (this commit)**: the API + a no-op identity pass.
// The pipeline runs end-to-end through the compile_pipeline at the
// MIR→LIR boundary; every example in the corpus exits with the same
// runtime code as without the optimizer (because the no-op pass
// preserves MIR byte-for-byte). Future cycles land actual passes
// inside this scaffold without churning every existing call site.
//
// **D-OPT1-VERIFY-AFTER-EVERY-PASS**: when the first real pass lands
// (OPT2 const-fold), the optimizer runs `MirVerifier::verify` after
// each pass under ALL build modes (not Debug-only — per plan 22 §3
// PR1 "unconditional" directive). The check is cheap relative to
// the pass cost and catches any pass-level invariant violation
// before downstream MIR/LIR/asm consumers cascade.
//
// **D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD** (already closed
// 2026-06-03): the DCE pass MUST consult `funcBinding /
// funcVisibility / globalBinding / globalVisibility` via
// `isExternallyVisible(binding, visibility)` before deleting any
// symbol. The substrate guarantees the attributes exist on every
// MirFunc + MirGlobal; the pass guarantees it consults them.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "mir/mir.hpp"

#include <string>
#include <vector>

namespace dss::opt {

// OPT1 pass-id vocabulary. Closed enum — every shipped pass has a
// stable ordinal that pipelines reference. New passes get a new
// enumerator + a per-pass handler in `optimize_impl.cpp` (cycle 1
// has only the no-op identity).
//
// **D-OPT1-PASS-ID-STABILITY**: ordinals are part of the
// pipeline-as-config contract. Pipelines reference passes by NAME
// at the JSON tier (via `optPassIdFromName`); the ordinals here are
// internal. Adding a pass appends to the end (never renumbers).
enum class PassId : std::uint8_t {
    Identity = 0,  // no-op — exercises the pipeline end-to-end. Cycle 1.
    // Future cycles append: ConstFold, Dce, CopyProp, Peephole, ...
};

// A pipeline is an ordered list of passes to run on each MIR
// function. Loaded from `src/dss-config/pipelines/*.pipeline.json`
// at compile time; the same vocabulary will later be searched by
// the OPT10 autotuner.
//
// Cycle 1: pipelines are empty by default. The CLI / compile
// pipeline injects no passes unless `-O1` / `-O2` is requested AND
// the named pipeline declares them. Default policy is "no
// optimization at the MIR tier" — the binary that lands today
// matches the binary the prior cycle produced byte-for-byte.
struct OptPipeline {
    // Code-reviewer post-fold I1 (2026-06-03): `name` is OWNED
    // (std::string), not a string_view, because plan 22 §2.5 routes
    // pipeline selection through artifact-profile JSON — a view
    // over a parsed-JSON owned-string would dangle the moment the
    // pipeline outlives the source json::value. Owned at cycle 1 =
    // zero lifetime-audit debt when OPT2 + OPT10 land.
    std::string name;
    std::vector<PassId> passes;
};

// Run the configured pipeline over every function in `mir`. Returns
// the same MIR object (passes mutate in-place via `MirBuilder`-style
// rewrites that the future passes own). The reporter receives any
// `X_*` diagnostic the pipeline emits.
//
// Cycle 1: no passes => no-op. The function exists to (a) prove the
// wiring through compile_pipeline doesn't regress any existing
// example AND (b) give every future pass a single entry point to
// hook into.
[[nodiscard]] DSS_EXPORT bool optimize(Mir& mir,
                                       TargetSchema const& target,
                                       OptPipeline const& pipeline,
                                       DiagnosticReporter& reporter);

} // namespace dss::opt
