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
