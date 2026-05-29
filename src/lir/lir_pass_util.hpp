#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// Shared substrate for LIR transformation passes (rewrite, callconv,
// future inlining/optimization passes). Folds the cycle-3b / ML7
// duplication identified by the simplifier as D-ML7-1.1: every pass
// that walks an input `Lir` and builds a fresh one re-implements the
// same diagnostic-emission, block-ref remapping, and terminator
// dispatch — all of which are tier-invariant (target-blind, source-
// blind, transformation-blind). Hoist once.

namespace dss::lir_pass_util {

// Emit a single diagnostic through the reporter. Identical to the
// shape used by every pass; centralized so the construction of
// `ParseDiagnostic` lives in one place.
DSS_EXPORT void
report(DiagnosticReporter& reporter, DiagnosticCode code,
       DiagnosticSeverity severity, std::string actual);

// Translate a NON-vreg operand: BlockRef gets remapped to dest-side
// block ids; everything else passes through. Vreg/spill resolution
// is pass-specific (rewrite handles vreg→phys+scratch; callconv
// doesn't touch vregs since they're already physical) and is NOT
// included here.
[[nodiscard]] DSS_EXPORT LirOperand
remapBlockRef(LirOperand const& op,
              std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst);

// Emit the rewritten terminator. The per-inst loop in each pass
// translates `newOps`; this routes to the matching `LirBuilder`
// entrypoint per successor count.
//
// `passName` is the prefix used in any error diagnostic (e.g.
// "rewrite", "callconv") so the reporter caller is identifiable.
//
// Returns false on > 2 successors (an unsupported substrate shape;
// future Switch lowering would need explicit handling).
[[nodiscard]] DSS_EXPORT bool
emitTerminator(LirBuilder& b, std::uint16_t op,
               TargetOpcodeInfo const* info,
               std::span<LirBlockId const> succs,
               std::span<LirOperand const> newOps,
               std::uint32_t payload,
               std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
               std::string_view passName,
               DiagnosticReporter& reporter);

} // namespace dss::lir_pass_util
