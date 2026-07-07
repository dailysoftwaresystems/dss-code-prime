#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <cstdint>

// D-AS-REGALLOC-WIDE-CALL-OPERAND-COUNT (option E) — PROTOTYPE.
//
// Pre-regalloc LIR pass: for every Call whose outgoing scalar arguments
// exceed the active cc's register-passed capacity (config: argGprs /
// argFprs counts + slotAligned + variadicArgsAlwaysStack), materialize
// each OVERFLOW argument as a `store_outgoing_arg` carrier emitted BEFORE
// the call, and REMOVE that operand from the Call. After this pass no Call
// holds more register-operands than the machine passes in registers, so
// the linear-scan allocator + rewriter never exhaust the register file on
// a wide call (the func-2088 blocker).
//
// The overflow-slot indexing MIRRORS lir_callconv's post-regalloc placement
// loop EXACTLY (same monotonic source-order NSAA cursor, same per-class /
// slot-aligned pool logic, same ByValueStackAgg-carrier skip, same
// hasIndirectResult / firstArgIdx handling), so the store offset callconv
// computes for a `store_outgoing_arg` payload is byte-identical to what the
// old placement loop stored — the caller-store ↔ callee-load contract is
// unchanged.
//
// Runs after MIR→LIR (which has NO active-cc knowledge) and before
// liveness/regalloc (both of which receive callingConventionIndex), so this
// is the earliest tier that both knows the cc AND holds the LIR. The
// indirect-call callee (ops[0]) and the sret pointer are NEVER touched
// (FC4-c2 + FC7-C3 preserved).

namespace dss {

struct LirWideCallResult {
    Lir  lir{};
    bool ok = false;
};

// callingConventionIndex selects the active cc from the schema (same index
// the driver threads into allocateRegisters / materializeCallingConvention).
[[nodiscard]] DSS_EXPORT LirWideCallResult
lowerWideCallArgs(Lir const&          src,
                  TargetSchema const& schema,
                  std::uint16_t       callingConventionIndex,
                  DiagnosticReporter& reporter);

} // namespace dss
