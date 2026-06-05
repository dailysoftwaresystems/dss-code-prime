#pragma once

#include "analysis/semantic/semantic_model.hpp"  // SemanticModel (CuMirModule member, move-only)
#include "asm/asm.hpp"  // AssembledModule (assembleUnit return + linkAndWrite span)
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"  // ExternImport (CuMirModule member)
#include "core/types/grammar_schema.hpp"
#include "core/types/strong_ids.hpp"  // CompilationUnitId (CuMirModule member)
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "mir/mir.hpp"  // Mir (CuMirModule member, move-only)
#include "opt/optimizer.hpp"
#include "program/cli_args.hpp"  // CompileConfig

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Driver-tier "one CU → one (target, format) → one artifact"
// pipeline kernel (plan 14 LK10 cycle 2).
//
// Wraps the full HIR → MIR → LIR → ASM → link → writeImage chain
// behind a single substrate entry point. `Program::compileFiles`
// and `Program::compileDirectory` (LK10 cycle 2) plus the future
// CLI router (LK10 cycle 3) reuse this kernel verbatim.
//
// The kernel is source-language-blind (consumes any GrammarSchema),
// target-blind (consumes any TargetSchema), and linker-format-blind
// (consumes any ObjectFormatSchema). All three are passed in by
// reference; the kernel never branches on language name, target
// arch, or format kind.
//
// Output path is fully caller-owned — the kernel does NOT pick
// extensions or output directories. `program.cpp` derives those
// via `TargetSpec::outputExtension(...)` for the v1 convention;
// plan 06 (artifact profiles) will eventually own the policy.

namespace dss {

class CompilationUnit; // fwd-decl — `compile_pipeline.cpp` includes the full header

// Drain every diagnostic from `src` into `dst`. Shared driver-tier
// helper — used by `program.cpp::compileFiles` (drains CU +
// per-Tree reporters) AND by `compile_pipeline.cpp::compileSingleUnit`
// (drains the SemanticModel's reporter after `analyze`). Hoisted
// out of the program-anon namespace at LK10 cycle 2 post-fold
// review #1 (silent-failure-hunter F9 — eliminates the inline drain
// duplicate that risked future divergence).
//
// **Policy-aware semantics (D-LK10-7 closed at LK10 cycle 3):**
// `dst.report(d)` re-traverses `dst`'s `DiagnosticPolicy` (suppress
// / overrides / warningsAsErrors). With the CLI now wiring
// `--warnings-as-errors` and `--suppress=<code>` through a
// `DiagnosticReporter::Config` to `compileFiles`/`compileDirectory`,
// the policy ALSO applies to per-tier drains coming through
// `copyDiagnostics`. That's the intended shape — the user's
// `--suppress` applies uniformly across the front-half and back-half
// of the pipeline. (D-LK10-7's `copyDiagnosticsRaw` alternative
// becomes unnecessary; this routing IS the design.)
DSS_EXPORT void copyDiagnostics(DiagnosticReporter const& src,
                                 DiagnosticReporter&       dst);

// Compile a single CompilationUnit through the full HIR→write
// pipeline for one (target, format) pair. Returns true iff every
// tier succeeded AND `writeImage` committed bytes to disk.
//
// Failure modes (all fail loud via `reporter`):
//   * Any tier emits an error (H_/I_/L_/R_/A_/K_) — the kernel
//     halts at the failing tier and returns false.
//   * `dss::linker::link` produces a non-`ok()` image.
//   * `dss::linker::writeImage` returns false (parent missing,
//     open failed, short write, ...).
//
// The caller is responsible for:
//   * Constructing the CompilationUnit (parse, addFile/addInMemory).
//   * Computing the output path (extension + directory).
//   * Creating any required parent directories before calling — the
//     kernel does NOT `create_directories` (mirrors the writer
//     substrate's contract; auto-mkdir would mask config bugs).
// `callingConventionIndex` is the per-(target, format) cc ordinal
// resolved by `dss::ffi::resolveAbi` in the driver before reaching
// this kernel. Threaded through to the LIR allocator so prologue/
// epilogue emission picks the correct cc table row. Pre-D-FF3-3
// every compile silently used index 0 — a real miscompile on
// non-default-cc targets (PE64 + x86_64 silently emitted SysV
// register assignments instead of MS_x64).
// D-OPT-COMPILE-OPTIONS-STRUCT: consolidates the trailing-nullable
// parameters that were individually positional. Adding future knobs
// (emitDebugInfo, ltoMode, inlineThreshold, ...) is a zero-signature-
// churn struct-field addition.
struct CompileOptions {
    // Selects the default optimizer pipeline when `pipelineOverride`
    // is null. Resolved via `resolvePipelineName` (a constexpr table
    // indexed by ordinal — NO `if (config == Release)` branches per
    // D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG agnosticism contract).
    CompileConfig config = CompileConfig::Debug;

    // Non-null: bypasses the JSON registry; used by the examples_runner's
    // differential-verify arm + MIR unit tests (D-OPT1-DIFFERENTIAL-
    // VERIFY-RUNNER).
    ::dss::opt::OptPipeline const* pipelineOverride = nullptr;
};

// Resolve `CompileConfig` to a shipped pipeline name. Uses a
// constexpr table of {ordinal, name} pairs — adding a new
// CompileConfig enumerator without extending the table fails the
// static_assert below at compile time. Mirrors the kPassNameTable
// precedent in optimizer.hpp INCLUDING the "in-order" check that
// catches a row-swap (e.g. accidentally putting "release" first):
// without the swap guard, a future edit could silently flip
// Debug→release and Release→debug at zero compile cost.
inline constexpr std::size_t kCompileConfigCount = 2;
inline constexpr std::pair<CompileConfig, std::string_view>
kPipelineNameTable[kCompileConfigCount] = {
    {CompileConfig::Debug,   "debug"},
    {CompileConfig::Release, "release"},
};
static_assert(kCompileConfigCount ==
              static_cast<std::size_t>(CompileConfig::Release) + 1,
              "CompileConfig / kPipelineNameTable drift — add a row to "
              "kPipelineNameTable when a new CompileConfig enumerator "
              "lands (D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG).");

[[nodiscard]] constexpr bool kPipelineNameTableInOrder() noexcept {
    for (std::size_t i = 0; i < kCompileConfigCount; ++i) {
        if (static_cast<std::size_t>(kPipelineNameTable[i].first) != i) {
            return false;
        }
    }
    return true;
}
static_assert(kPipelineNameTableInOrder(),
              "kPipelineNameTable entries must appear in CompileConfig "
              "ordinal order — a row-swap would silently flip the "
              "Debug↔Release mapping.");

// Returns nullopt on an out-of-range `CompileConfig` ordinal (e.g.
// `static_cast<CompileConfig>(99)` produced by a buggy CLI parser
// or a future enumerator added without a table row). Callers
// fail loud — silent fallback to "debug" would let a release build
// silently degrade to debug-pipeline.
[[nodiscard]] constexpr std::optional<std::string_view>
resolvePipelineName(CompileConfig config) noexcept {
    auto const idx = static_cast<std::size_t>(config);
    if (idx >= kCompileConfigCount) return std::nullopt;
    return kPipelineNameTable[idx].second;
}

[[nodiscard]] DSS_EXPORT bool
compileSingleUnit(CompilationUnit const&         cu,
                  GrammarSchema const&           grammar,
                  TargetSchema const&            target,
                  ObjectFormatSchema const&      format,
                  std::uint16_t                  callingConventionIndex,
                  std::filesystem::path const&   outPath,
                  DiagnosticReporter&            reporter,
                  CompileOptions const&          opts = {});

// Assemble ONE CompilationUnit to its `AssembledModule` (the per-CU half of
// `compileSingleUnit` — no link, no write). Returns nullopt on any tier failure
// (diagnostics emitted via `reporter`). The multi-CU driver (CU6) calls this per
// CU, collects the N modules, then `linkAndWrite`s them into one merged image.
//
// Implemented as `buildCuMir(...)` composed with `lowerCuMirToAssembly(...)` — the
// two halves below. The single-CU output is byte-identical to the former monolithic
// `buildAssembledModule`; the split exists so the multi-CU driver can build EVERY
// CU's MIR (loop 1) before lowering any (loop 2) — the prerequisite shape for a
// future whole-program MIR merge (cycle 25). Most callers use this composed entry.
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
assembleUnit(CompilationUnit const&         cu,
             GrammarSchema const&           grammar,
             TargetSchema const&            target,
             ObjectFormatSchema const&      format,
             std::uint16_t                  callingConventionIndex,
             DiagnosticReporter&            reporter,
             CompileOptions const&          opts = {});

// ── assembleUnit's two halves (Cycle 24) ──────────────────────────────────────
//
// The verified MIR/LIR seam: everything through the optimizer is the BUILD half
// (`buildCuMir`); MIR→LIR onward is the LOWER half (`lowerCuMirToAssembly`). The
// `CuMirModule` carries every piece of state the lower half reads across the seam —
// crucially the move-only `SemanticModel`, whose `TypeLattice` owns the type
// interner that MIR→LIR + the optimizer + the symbol-table populate all consume.
// Holding the model BY VALUE keeps the interner alive after MIR-build returns, so
// loop 1 can build all CUs' MIR and loop 2 can lower them later.
//
// Move-only (the `Mir` + `SemanticModel` members are both move-only). The
// `grammar` / `target` references are non-owning POINTERS into the caller's
// schemas, which outlive both loops (owned by `compileOneTarget`).
struct DSS_EXPORT CuMirModule {
    Mir                       mir;             // the optimized module
    SemanticModel             model;           // MOVED in — owns the interner past MIR-build
    std::vector<ExternImport> externImports;   // MOVED into lowerToLir by the lower half
    CompilationUnitId         cuId{};          // == cu.id(); stamped onto the AssembledModule

    // Non-owning back-references the lower half reads. The caller's GrammarSchema /
    // TargetSchema outlive the CuMirModule (they live across both driver loops).
    GrammarSchema const*      grammar = nullptr;  // entry-name list + (unused-by-lower) policy
    TargetSchema const*       target  = nullptr;  // MIR→LIR + assemble target
    std::uint16_t             callingConventionIndex = 0;
};

// BUILD half: semantic analysis → HIR → FFI synthesis → MIR → optimize. Returns the
// `CuMirModule` carrying the optimized MIR + the SemanticModel (interner owner) +
// extern imports + the cuId/schema refs the lower half needs. Returns nullopt on any
// front-half tier failure (diagnostics emitted via `reporter`).
[[nodiscard]] DSS_EXPORT std::optional<CuMirModule>
buildCuMir(CompilationUnit const&         cu,
           GrammarSchema const&           grammar,
           TargetSchema const&            target,
           ObjectFormatSchema const&      format,
           std::uint16_t                  callingConventionIndex,
           DiagnosticReporter&            reporter,
           CompileOptions const&          opts = {});

// LOWER half: MIR → LIR → liveness → regalloc → rewrite → legalize → callconv →
// assemble → symbol-table populate → user-entry scan. Consumes the `CuMirModule`
// (its `externImports` are MOVED into MIR→LIR; its `mir` + `model` are read). Returns
// nullopt on any back-half tier failure (diagnostics emitted via `reporter`).
[[nodiscard]] DSS_EXPORT std::optional<AssembledModule>
lowerCuMirToAssembly(CuMirModule&        cuMir,
                     DiagnosticReporter& reporter);

// Link N assembled CUs into one image + commit to `outPath` (the shared half of
// `compileSingleUnit`). N==1 is the v1 single-CU path; N>1 triggers the linker's
// cross-CU merge (LK11a resolution + LK11b byte emission). Returns true iff the
// image is `ok()`, no link-tier error fired, and `writeImage` committed bytes.
[[nodiscard]] DSS_EXPORT bool
linkAndWrite(std::span<AssembledModule const> modules,
             TargetSchema const&              target,
             ObjectFormatSchema const&        format,
             std::filesystem::path const&     outPath,
             DiagnosticReporter&              reporter);

} // namespace dss
