#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <filesystem>
#include <string>
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
[[nodiscard]] DSS_EXPORT bool
compileSingleUnit(CompilationUnit const&         cu,
                  GrammarSchema const&           grammar,
                  TargetSchema const&            target,
                  ObjectFormatSchema const&      format,
                  std::filesystem::path const&   outPath,
                  DiagnosticReporter&            reporter);

} // namespace dss
