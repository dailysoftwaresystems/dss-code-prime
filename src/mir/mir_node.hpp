#pragma once

#include "core/substrate/arena_tag.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_id.hpp"   // TypeId
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <type_traits>

// MIR storage PODs (ML1). The MIR module is three dense arenas — instructions,
// basic blocks, functions — all tagged by one `MirModuleId` (the cross-module
// guard), dogfooding the SP1 substrate exactly as HIR does. The PODs live in
// `detail/` so consumers go through `Mir`'s accessors; several fields are
// pool offsets that are meaningless in isolation.
//
// Topology (per plan 12 §2.2):
//   function  → a contiguous range of blocks  [blockStart, blockStart+blockCount)
//   block     → a contiguous range of insts   [instStart,  instStart +instCount)
//             + a contiguous range of CFG successor block-ids in the succ pool
//   instruction → operands as a range into the operand pool (or, for Phi, the
//                 phi pool of incoming value/block pairs)
// Contiguous arena ranges (func→blocks, block→insts) need no indirection pool —
// the arena slot ordering IS the membership order, so the builder appends a
// block's instructions back-to-back. The only genuinely non-contiguous lists
// (operands, phi incomings, successors) live in module-owned pools.

namespace dss {

// In the FUSED value model a value IS the instruction that defines it, so a
// "value reference" is just the defining instruction's id. The alias documents
// intent at call sites (operands, results) without a second arena or any
// conversion boilerplate.
using MirValueId = MirInstId;

// ── per-instruction markers ──────────────────────────────────────────────────
//
// `Synthetic` marks instructions lowering inserted with no source origin (e.g.
// structured-CF scaffolding). `Volatile` marks a memory access the optimizer
// must not reorder or elide. Multiple flags may apply.
enum class MirInstFlags : std::uint8_t {
    None      = 0,
    Synthetic = 1u << 0,
    Volatile  = 1u << 1,
    // bits 2-7 reserved
};

[[nodiscard]] inline constexpr MirInstFlags operator|(MirInstFlags a, MirInstFlags b) noexcept {
    using U = std::underlying_type_t<MirInstFlags>;
    return static_cast<MirInstFlags>(static_cast<U>(a) | static_cast<U>(b));
}
[[nodiscard]] inline constexpr MirInstFlags operator&(MirInstFlags a, MirInstFlags b) noexcept {
    using U = std::underlying_type_t<MirInstFlags>;
    return static_cast<MirInstFlags>(static_cast<U>(a) & static_cast<U>(b));
}
[[nodiscard]] inline constexpr bool any(MirInstFlags v) noexcept {
    return static_cast<std::underlying_type_t<MirInstFlags>>(v) != 0;
}
[[nodiscard]] inline constexpr bool has(MirInstFlags v, MirInstFlags bit) noexcept {
    return any(v & bit);
}

// ── structured-CF markers (block metadata) ───────────────────────────────────
//
// Each block carries the structural control-flow role it plays in the source
// program (plan 12 §2.3). HIR→MIR lowering (ML2) stamps these; ML1 defaults
// every block to `Linear`. WASM lowering (plan 18) consumes them DIRECTLY to
// emit block/loop/if without ever running a Relooper recovery pass — which is
// why the marker is preserved as first-class block metadata rather than being
// re-derived. A marker is a property of the block, NOT a computation, so it is a
// block field and never appears in the instruction stream (keeping the stream
// clean for the optimizer and instruction selection).
//
// ML1 models ONE marker per block. A block can in principle play two structural
// roles at once (e.g. a LoopExit that is also an IfJoin); if a real consumer
// needs that, this becomes a small bitset without changing the POD layout (the
// field is already a full uint8). Kept single-role until then.
enum class StructCfMarker : std::uint8_t {
    Linear,       // no structural role (straight-line code) — the default
    EntryBlock,   // function entry
    ExitBlock,    // function exit (Return / Unreachable terminator)
    LoopHeader,   // loop entry; target of the back-edge; dominates the body
    LoopLatch,    // back-edge source (closes the loop)
    LoopExit,     // leaves the loop body
    IfThen,       // then-arm of a conditional
    IfElse,       // else-arm of a conditional
    IfJoin,       // post-dominating merge of an if
    SwitchHead,   // the switch discriminant block
    SwitchCase,   // a case arm
    SwitchJoin,   // post-dominating merge of a switch
};

// One incoming edge of a Phi: the value flowing in along a given predecessor
// block. Stored in the module's phi pool; a Phi instruction's operand range
// addresses this pool instead of the general operand pool.
struct MirPhiIncoming {
    MirValueId value{};  // the value flowing in (a defining instruction id)
    MirBlockId pred{};   // the predecessor block this value arrives from

    constexpr bool operator==(MirPhiIncoming const&) const noexcept = default;
};
static_assert(std::is_trivially_copyable_v<MirPhiIncoming>);

namespace detail {

// ── instruction POD ───────────────────────────────────────────────────────────
//
// Fused: simultaneously the instruction and the SSA value it defines (the value
// id == this instruction's MirInstId). A value-less instruction (Store, the
// terminators, a void Call) carries `typeId == InvalidType`. `operandStart` /
// `operandCount` address the module operand pool — EXCEPT for `Phi`, where they
// address the phi pool (gated by `opcode == MirOpcode::Phi`). Parent/owner links
// live on the block, not here, keeping the node small for scan-hot passes.
//
// `payload` is per-opcode: Const → literal-pool index; GlobalAddr → SymbolId.v;
// Arg → parameter index; IntrinsicCall → intrinsic id; otherwise unused.
struct MirInst {
    MirOpcode     opcode       = MirOpcode::Invalid;  // 2  — Invalid: visibly-bogus default
    MirInstFlags  flags        = MirInstFlags::None;  // 1
    std::uint8_t  _pad         = 0;                   // 1  — explicit padding
    TypeId        typeId       = InvalidType;         // 8  — result type (Invalid if value-less)
    std::uint32_t operandStart = 0;                   // 4  — into operand pool (or phi pool if Phi)
    std::uint32_t operandCount = 0;                   // 4
    std::uint32_t payload      = 0;                   // 4  — per-opcode scalar
};
static_assert(sizeof(MirInst) <= 32, "detail::MirInst grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirInst>);

// ── basic-block POD ────────────────────────────────────────────────────────────
//
// Owns a contiguous instruction range and a contiguous CFG-successor range. The
// terminator is the last instruction in the range (derived, not stored — single
// source of truth). Successor block-ids live in the module succ pool so the CFG
// is recoverable in O(1) per block WITHOUT parsing the terminator's operands —
// keeping dataflow (operands) and control-flow (successors) cleanly separated.
struct MirBlock {
    std::uint32_t  instStart = 0;                       // 4  — into the instruction arena
    std::uint32_t  instCount = 0;                       // 4  — includes the terminator
    std::uint32_t  succStart = 0;                       // 4  — into the succ pool
    std::uint32_t  succCount = 0;                       // 4
    std::uint32_t  func      = 0;                       // 4  — owning MirFuncId.v (module-implied tag)
    StructCfMarker marker    = StructCfMarker::Linear;  // 1
    std::uint8_t   _pad[3]   = {};                      // 3  — explicit padding
};
static_assert(sizeof(MirBlock) <= 32, "detail::MirBlock grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirBlock>);

// ── function POD ────────────────────────────────────────────────────────────────
//
// Owns a contiguous block range. `signature` is the FnSig in the CU's type
// lattice (the same TypeId discipline HIR's Function uses — interned + shared,
// not a child). The entry block is the function's first block. `symbol` is the
// declared SymbolId.v.
struct MirFunc {
    TypeId        signature  = InvalidType;  // 8  — FnSig TypeId
    std::uint32_t blockStart = 0;            // 4  — into the block arena
    std::uint32_t blockCount = 0;            // 4
    std::uint32_t symbol     = 0;            // 4  — declared SymbolId.v
    std::uint32_t _pad       = 0;            // 4  — explicit padding
};
static_assert(sizeof(MirFunc) <= 32, "detail::MirFunc grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirFunc>);

} // namespace detail

} // namespace dss

// Cross-arena guard wording (the SH3 / SP1 discipline) for the three MIR arenas.
// The primary `ArenaNames` template is a must-specialize tripwire (arena_tag.hpp),
// so these are mandatory before instantiating any ArenaContainer / ArenaAttribute
// over a MIR id. All three share `MirModuleId` as the arena tag — one module, one
// tag, three element-id spaces — and each names its own element so a fatal
// message identifies which tier's guard fired.
namespace dss::substrate {

template <>
struct ArenaNames<MirInstId, MirModuleId> {
    static constexpr char const* attribute = "MirAttribute";
    static constexpr char const* element   = "MirInstId";
    static constexpr char const* tag       = "MirModuleId";
    static constexpr char const* access    = "Mir::inst";
};

template <>
struct ArenaNames<MirBlockId, MirModuleId> {
    static constexpr char const* attribute = "MirBlockAttribute";
    static constexpr char const* element   = "MirBlockId";
    static constexpr char const* tag       = "MirModuleId";
    static constexpr char const* access    = "Mir::block";
};

template <>
struct ArenaNames<MirFuncId, MirModuleId> {
    static constexpr char const* attribute = "MirFuncAttribute";
    static constexpr char const* element   = "MirFuncId";
    static constexpr char const* tag       = "MirModuleId";
    static constexpr char const* access    = "Mir::func";
};

} // namespace dss::substrate
