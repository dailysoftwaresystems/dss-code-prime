#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <cstdint>
#include <vector>

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
// ★★★ THIS PASS OWNS EVERY OUTGOING-ARGUMENT BYTE OFFSET
// (D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES), and it is the
// only tier that CAN: it is the last one to see the call's COMPLETE argument
// list, because removing the overflow scalars is its own job. Anything that
// re-derives a placement afterwards is walking a list this pass shortened, so
// its cursor restarts inside bytes already handed out — which is exactly the
// silent caller-side miscompile the row above records (a stacked scalar and the
// first eightbyte of a stacked aggregate written to the SAME bytes).
//
// So the pass emits, for one call:
//   * each stacked SCALAR as a `store_outgoing_arg` whose PAYLOAD is its byte
//     offset and whose `flags` are the access width the cursor chose, and
//   * each stacked by-value AGGREGATE as ITS BYTES, copied here: one GPR-wide
//     `load` from [temp + k] into a fresh virtual register and one
//     `store_outgoing_arg` at placement + k per chunk, emitted before the Call,
//     and the `(Reg, ByValueStackAgg)` carrier REMOVED from the Call,
// and it stamps `kLirInstFlagOutgoingArgsPlaced` on the shrunken Call, which is
// how `lir_callconv` KNOWS the placements are made and that it must compute none.
//
// ★ THE CARRIER'S STAGE RULE. Every Call goes through this pass, so after it NO
// instruction carries the `(Reg, ByValueStackAgg[, MemOffset])` form: the
// aggregate's address is read by single-operand loads the allocator places
// like any other, not kept live into the Call. The LIR verifier states it —
// `verifyLirStackedAggregatesLowered`, which this pass runs on the module it
// produced (so a terminator Call, whose operands are carried verbatim, that
// held one is refused here rather than passed on) and which
// `verifyLirPostRegalloc` runs at every later checkpoint — so liveness, the
// allocator, the rewrite and `lir_callconv` keep no arm for the form. An input
// that already states a placement (the trailing `MemOffset` this pass used to
// leave on the Call for `lir_callconv` to copy after allocation) is REFUSED: it
// has been through here before. The form's only readers are this pass and the
// text codec (a pre-pass module written out and read back).
//
// D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: the byte placement rule
// itself lives in `StackArgCursor` (lir_callconv.hpp) — the same object the
// outgoing-area RESERVATION and the CALLEE's incoming reads walk, so a caller
// store and a callee load cannot disagree. Under every `Slot`-packing CC an
// offset is `idx*slot` and the width flags are 0 (= 64-bit), i.e. exactly the
// pre-Apple encoding.
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
    // D-LIR-DESCRIPTOR-BLOCK-IDS-SHIFTED-BY-A-BLOCK-INSERTING-PASS: every rebuild publishes its block entry
    // image, and this pass rebuilds block for block, so what it publishes is the identity IT PERFORMED —
    // written down where each source block is begun, from the map the rebuild goes through. Indexed by the
    // SOURCE module's block arena (`LirBlockId.v`; slot 0 holds 0): the `.v` of the block of `lir` where
    // that block's instructions begin. `lir/lir_descriptor_blocks.hpp` follows the blocks data names
    // through it, and refuses a rebuild that publishes none.
    std::vector<std::uint32_t> blockEntryImage;
};

// callingConventionIndex selects the active cc from the schema (same index
// the driver threads into allocateRegisters / materializeCallingConvention).
[[nodiscard]] DSS_EXPORT LirWideCallResult
lowerWideCallArgs(Lir const&          src,
                  TargetSchema const& schema,
                  std::uint16_t       callingConventionIndex,
                  DiagnosticReporter& reporter);

} // namespace dss
