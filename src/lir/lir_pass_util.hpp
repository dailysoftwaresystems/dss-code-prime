#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
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

// D-AS-REWRITE-SPILL-SCRATCH-INCOMING-ARG-CLOBBER: resolve the physical
// INCOMING argument register a register-machine `arg` op's parameter arrives
// in, or classify why there is none. ONE formula, shared by the two consumers
// that ask the SAME question ("which physical register still holds a live
// incoming param"):
//   (a) the regalloc position-aware occupied-arg exclusion
//       (collectArgRegisterOccupied, lir_regalloc.cpp) — keeps a vreg HOME off
//       a still-live arg register;
//   (b) the rewriter spill-scratch forbid (rewriteOneFunc, lir_rewrite.cpp) —
//       keeps a transient reload SCRATCH off it.
// Coupling them here is what makes them provably agree (a drift would re-open
// the incoming-param clobber this anchor closes).
//
// NOT the arg MATERIALIZATION path: lir_callconv's `h.arg` emits the actual
// `mov home, argReg` and needs the register NAME plus the slot-aligned pool
// size for its cursor-desync assert — a different output, deliberately left
// untouched. The register-resident test here mirrors the collector's existing
// per-class-pool `payload < pool.size()` (byte-identical on every shipped cc).
// A hypothetical slot-aligned cc with UNEQUAL arg pool sizes is the only shape
// where this per-class test would diverge from `h.arg`'s `max(g,f)` test; none
// ships, and both consumers here treat the divergent slot as "no register to
// protect" (safe — a stack-passed param has no incoming register to clobber).
// DOMAIN NOTE (pre-existing, unreached — preserved byte-for-byte from the
// pre-hoist collector): a non-FPR result class resolves through `argGprs`, so a
// VR-class (vector) param would protect a GPR ordinal rather than its v-register.
// Unreached by the c-subset (no vector params; AAPCS64 F128 aliases the FPR
// d-view). If a vector-param ABI ever ships, add its arg-vector pool arm here.
enum class IncomingArgRegKind : std::uint8_t {
    Register,          // arrives in a physical register (ordinal below)
    StackPassed,       // payload past the arg-register pool → caller's stack
    UnresolvableName,  // the cc names a register absent from the target table
};
struct IncomingArgReg {
    IncomingArgRegKind kind{IncomingArgRegKind::StackPassed};
    std::uint16_t      ordinal{0};  // meaningful iff kind == Register
};

[[nodiscard]] DSS_EXPORT IncomingArgReg
incomingArgRegister(TargetSchema const&            schema,
                    TargetCallingConvention const& cc,
                    LirRegClass                    resultClass,
                    std::uint32_t                  payload);

} // namespace dss::lir_pass_util
