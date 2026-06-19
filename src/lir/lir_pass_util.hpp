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

// Note: the `report(reporter, code, severity, msg)` shim previously
// lived here. Hoisted to `core/types/diagnostic_reporter.hpp` at LK10
// cycle 3 post-fold #2 (D-LK10-8 closure) — it's tier-agnostic and
// driver-tier callers (program/input_resolver) were dragging in LIR
// headers for it. Consumers now `using dss::report;` (or call
// `dss::report` qualified) without a LIR dependency.

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
// entrypoint via `info->terminatorKind` (single source of truth,
// shared with the `.dsslir` parser dispatch — replaces the
// successor-count-counting heuristic that earlier draft used).
//
// `passName` is the prefix used in any error diagnostic (e.g.
// "rewrite", "callconv") so the reporter caller is identifiable.
//
// Returns false on Switch (reserved — future LIR Switch lowering will
// add the dispatch) or `terminatorKind == None` (substrate invariant
// violation — `info->isTerminator()` should have already filtered the
// call site).
[[nodiscard]] DSS_EXPORT bool
emitTerminator(LirBuilder& b, std::uint16_t op,
               TargetOpcodeInfo const* info,
               std::span<LirBlockId const> succs,
               std::span<LirOperand const> newOps,
               std::uint32_t payload,
               std::uint8_t  flags,
               std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
               std::string_view passName,
               DiagnosticReporter& reporter);

// D-CSUBSET-BITFIELD-WIDE-UNIT: copy the source module's wide-literal
// pool into the destination builder, PRESERVING indices. Every pass
// that walks an input `Lir` and builds a fresh one copies `LiteralIndex`
// OPERANDS verbatim (the index is an opaque module-level reference), so
// the new builder's pool MUST hold the same entries at the same indices
// or those operands dangle (LirLiteralPool::at out-of-range at encode
// time). The destination builder MUST be freshly constructed (empty
// pool) so appending entries 0..N-1 in order reproduces the source
// indices exactly. Call once, right after `LirBuilder b{schema}`,
// before any `addInst`. (Before FC8 no real value rode `LiteralIndex`
// to the encoder — strings/floats never reached it — so this latent
// rebuild gap was invisible; the `mov r64, imm64` carrier exposes it.)
DSS_EXPORT void
copyLiteralPool(Lir const& src, LirBuilder& dst);

} // namespace dss::lir_pass_util
