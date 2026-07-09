#pragma once

#include "core/substrate/arena_tag.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"           // SymbolBinding / SymbolVisibility
#include "core/types/type_lattice/type_id.hpp"   // TypeId
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <type_traits>

// MIR storage PODs (ML1). The MIR module is four dense arenas — instructions,
// basic blocks, functions, and module-level globals — all tagged by one
// `MirModuleId` (the cross-module guard), dogfooding the SP1 substrate exactly
// as HIR does. The PODs live in `detail/` so consumers go through `Mir`'s
// accessors; several fields are pool offsets that are meaningless in isolation.
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
// must not reorder or elide. Also: two `Volatile` ops must never be reordered
// relative to each other, and any future instruction-scheduling / sinking pass
// MUST treat a `Volatile` op as a scheduling barrier w.r.t. every other
// `Volatile` or side-effecting op (today guaranteed structurally by the shared
// rebuild-walk's original-scan-order discipline; pin:
// TwoVolatileStoresToDifferentGlobalsKeepRelativeOrder). Multiple flags may apply.
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
// Each block carries the structural control-flow role it plays in the program
// (plan 12 §2.3). The marker is CANONICALLY DERIVED FROM THE CFG —
// `deriveStructCfMarkers` (mir/mir_struct_markers.hpp) computes it from
// dominators / post-dominators / natural loops, and every producer (HIR→MIR
// lowering, CFG-mutating optimizer passes, the cross-CU merge) re-stamps its
// output from that derivation after `finish()`; creation-time
// `createBlock(marker)` stamps are intent-documenting DEFAULTS the final
// stamping overwrites. The verifier checks stored == derived per reachable
// block (I_StructCfMismatch). A future WASM lowering (plan 18) still consumes
// the stored field directly — the derivation keeps it TRUSTWORTHY through
// arbitrary CFG transforms without a Relooper recovery pass at consume time.
// A marker is a per-block byte, never an instruction-stream entity (keeping
// the stream clean for the optimizer and instruction selection).
//
// ONE marker per block; the derivation's priority order (the spec in
// mir_struct_markers.hpp) IS the multi-role collision policy — e.g. a block
// that is both a back-edge target and an if-join derives LoopHeader (the
// higher-priority claim wins). If a real consumer ever needs multi-role
// visibility, this becomes a small bitset without changing the POD layout.
//
// DORMANT VALUES — NOT derived (and no producer emits them); they remain
// `mir_text` round-trip vocabulary only:
//   - ExitBlock:  no producer ever emitted it.
//   - LoopLatch:  not CFG-derivable (a while body-tail and a for-update block
//     can present identical CFG shapes); back-edge SOURCES are recoverable
//     from `mirNaturalLoops::backEdgeSources` when a consumer needs them.
//   - SwitchHead: never emitted; the discriminant block derives by the
//     lower-priority rules (usually Linear/EntryBlock).
enum class StructCfMarker : std::uint8_t {
    Linear,       // no structural role (straight-line code) — the default
    EntryBlock,   // function entry
    ExitBlock,    // DORMANT (see above) — function exit
    LoopHeader,   // loop entry; target of the back-edge; dominates the body
    LoopLatch,    // DORMANT (see above) — back-edge source
    LoopExit,     // target of a loop-exiting edge
    IfThen,       // then-arm of a conditional
    IfElse,       // else-arm of a conditional
    IfJoin,       // post-dominating merge of an if
    SwitchHead,   // DORMANT (see above) — the switch discriminant block
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
    // Secondary per-opcode scalar. Currently used ONLY by `Alloca`
    // (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): `payload` carries the aggregate
    // byte size (frame-slot sizing), so the local's EFFECTIVE alignment
    // (max of natural + `alignas`) rides here — MIR→LIR reads it to compute
    // each function's max local alignment (fed to the frame layout). 0 for
    // every other opcode + a scalar alloca that recorded no over-alignment
    // (its natural alignment is derivable, so 0 is a safe "no info" sentinel).
    // Grows MirInst 24→28 bytes — the static_assert below still holds.
    std::uint32_t payload2     = 0;                   // 4  — secondary per-opcode scalar
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
    // D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD (step 13.6 OPT1 gate,
    // 2026-06-03): linkage attributes for the optimizer's DCE pass.
    // `isExternallyVisible(binding, visibility)` is the DCE-protect
    // predicate; a function for which it returns true MUST NOT be
    // deleted by DCE even when no intra-module use exists. C-style
    // languages without `static` default both fields to (Global,
    // Default) — every function externally visible by language
    // convention. Front-ends with `static` / `inline` / `hidden`
    // emit the matching binding/visibility at HIR→MIR lowering.
    // Fits the existing 4-byte _pad slot — no struct-size growth.
    SymbolBinding    binding    = SymbolBinding::Global;     // 1
    SymbolVisibility visibility = SymbolVisibility::Default; // 1
    std::uint16_t _pad          = 0;                         // 2  — explicit padding
};
static_assert(sizeof(MirFunc) <= 32, "detail::MirFunc grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirFunc>);

// ── global POD ──────────────────────────────────────────────────────────────────
//
// A module-level storage cell. `type` is the declared variable type (the same
// lattice TypeId everything else uses); `symbol` is the declared `SymbolId.v`
// (lookup key for `GlobalAddr`). Initialization shape (mutually exclusive):
//   - `initLiteralIndex != UINT32_MAX`: constant initializer — index into the
//     module's `MirLiteralPool`. The literal's value is the global's initial
//     state at module load.
//   - `initFunc.valid()`: non-constant initializer — `initFunc` is a synthesized
//     `__module_init__` MirFunc whose body stores into this global as part of
//     module load. (Plan 12's ML2-globals cycle uses fold-first/fall-back-to-
//     init-function policy: the constant case is the common path.)
//   - Both UINT32_MAX / Invalid: declared with no initializer (zero-init by
//     C language convention; the runtime decides the zero pattern).
// Deliberate divergence from `MirBlock`'s `uint32_t func` field: `initFunc`
// here is a full `MirFuncId` (with arena tag) — globals are written + read
// across multiple lowering passes, so preserving provenance through the
// strong-id form is worth the 4 extra bytes vs. a raw u32. POD is 28B (no
// trailing pad), well under the 32B static_assert ceiling.
struct MirGlobal {
    TypeId        type             = InvalidType;       // 8
    std::uint32_t symbol           = 0;                 // 4  — declared SymbolId.v
    std::uint32_t initLiteralIndex = UINT32_MAX;        // 4  — into MirLiteralPool
    MirFuncId     initFunc{};                           // 8  — module-init function id (strong)
    // D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD (step 13.6 OPT1 gate,
    // 2026-06-03): same linkage discipline as MirFunc — DCE-protected
    // when `isExternallyVisible(binding, visibility)` returns true.
    // Externally-observable globals (a C-style file-scope `int g;`
    // with no `static`) MUST survive DCE / unused-symbol elimination.
    SymbolBinding    binding    = SymbolBinding::Global;     // 1
    SymbolVisibility visibility = SymbolVisibility::Default; // 1
    // D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL (writable data sections cycle): true iff
    // the source declared this global `const`. Read by the assembler's
    // section selection (`lowerMirGlobalsToDataItems`): an INITIALIZED global
    // routes to read-only `.rodata` when const, writable `.data` when mutable.
    // Default `false` (mutable) is the conservative writable default — a global
    // wrongly stamped mutable still lands in writable memory (never the read-
    // only-store crash). Consumes one byte of the former 2-byte pad → zero size
    // growth (the static_assert below still holds).
    bool             isConst    = false;                    // 1
    std::uint8_t     _pad       = 0;                         // 1  — explicit padding
    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): the EXPLICIT
    // `alignas(N)` alignment in bytes (a power of two ≤ 256), or 0 for no
    // override. Read by the assembler's data-item emission
    // (`lowerMirGlobalsToDataItems`), which raises the emitted symbol's section
    // alignment to `max(natural, this)` when nonzero. Threaded from the source's
    // `alignas` via the declaration-keyed `HirAlignmentMap` at HIR→MIR lowering.
    // Consumes the former 4-byte tail padding → zero size growth (28→32 bytes,
    // the static_assert below still holds).
    std::uint32_t    alignment  = 0;                         // 4
};
static_assert(sizeof(MirGlobal) <= 32, "detail::MirGlobal grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<MirGlobal>);

} // namespace detail

} // namespace dss

// Cross-arena guard wording (the SH3 / SP1 discipline) for the four MIR arenas.
// The primary `ArenaNames` template is a must-specialize tripwire (arena_tag.hpp),
// so these are mandatory before instantiating any ArenaContainer / ArenaAttribute
// over a MIR id. All four share `MirModuleId` as the arena tag — one module, one
// tag, four element-id spaces — and each names its own element so a fatal
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

template <>
struct ArenaNames<MirGlobalId, MirModuleId> {
    static constexpr char const* attribute = "MirGlobalAttribute";
    static constexpr char const* element   = "MirGlobalId";
    static constexpr char const* tag       = "MirModuleId";
    static constexpr char const* access    = "Mir::global";
};

} // namespace dss::substrate
