#pragma once

#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

// LIR node PODs (plan 12 §2.1 file tree → `lir_inst.hpp` + per-block /
// per-function shape). Each POD ≤ 32 bytes for cache density; the
// arena container assumes trivial-copyability.

namespace dss {

// `TargetCondCode` lives next to `TargetResultRule` / `TargetRegClass`
// in `core/types/target_schema.hpp` — same target-blind universal-
// vocabulary tier (compile-time enum, not JSON-driven). LIR pulls it
// in via target_schema.hpp's include.

// FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): `LirInst.flags` bit 0 is the
// operation-WIDTH discriminator the encoding-variant guards match on.
//   0 (default) = 64-bit — every pre-FC3 instruction and all
//                 hand-built test LIR (back-compat by construction).
//   1           = 32-bit — set by MIR→LIR lowering when the MIR
//                 instruction's type is I32/U32 (ALU/div/compare/
//                 trunc tiers; plumbing movs/loads/stores stay 64).
// The width rides `flags` (not a new POD field) deliberately: every
// existing flags-copying rewrite pass (lir_2addr_legalize, the
// regalloc rewriter, lir_callconv materialize) and the `.dsslir`
// text round-trip ("payload=N flags=M" is mandatory-emitted) carry
// it forward with ZERO changes — a width dropped at any rebuild
// would silently re-select the 64-bit forms (a miscompile), so the
// carrier that survives by construction is the safe one.
// kLirInstFlagWidth8 (D-CSUBSET-CHAR-STRING-VALUE-CODEGEN) adds the
// sub-native byte form (`char` ≡ I8): movsx/movzx r/m8, mov r8, sxtb,
// ldrb/strb. kLirInstFlagWidth16 (D-LIR-INT-MEMORY-WIDTH-EXACT) adds the
// half-word memory form (I16/U16): x86 0x66-prefixed mov / movzx r16,
// arm64 STURH/LDURH. The width bits are mutually exclusive (narrowest
// wins); the JSON loader accepts a guard width of 8, 16, 32, or 64.
inline constexpr std::uint8_t kLirInstFlagWidth32 = 0x01;
inline constexpr std::uint8_t kLirInstFlagWidth8  = 0x02;
inline constexpr std::uint8_t kLirInstFlagWidth16 = 0x04;

// The instruction's operation width in bits, derived from its flags.
[[nodiscard]] constexpr std::uint8_t
lirInstWidthBits(std::uint8_t flags) noexcept {
    if ((flags & kLirInstFlagWidth8) != 0)  return std::uint8_t{8};
    if ((flags & kLirInstFlagWidth16) != 0) return std::uint8_t{16};
    return (flags & kLirInstFlagWidth32) != 0 ? std::uint8_t{32}
                                              : std::uint8_t{64};
}

// Operand variant tag carried on each entry of the LIR operand pool.
// LIR operands are heterogeneous (registers / immediates / memory
// addressing modes); the kind discriminator picks the right field
// to read from the pool slot.
enum class LirOperandKind : std::uint8_t {
    None       = 0,
    Reg        = 1,  // virtual or physical register (pre/post regalloc)
    ImmInt     = 2,  // sign-extended int64 immediate
    // Slot 3 reserved — was `ImmFloat` (never minted, no union arm, no
    // payload field). Wide floats flow through the literal pool via
    // `LiteralIndex` (cycle 3c). Leaving the slot reserved keeps the
    // numeric values stable for callers that already serialized the
    // enum (none currently exist, but the enum value space is part of
    // the substrate contract).
    BlockRef   = 4,  // basic-block reference (br targets, etc.)
    SymbolRef  = 5,  // symbol reference (call targets, GlobalAddr)
    // Memory addressing modes (base + index + scale + displacement)
    // are spread across THREE pool entries to keep each pool slot a
    // uniform 8 bytes: a leading `Reg` operand for the base, then a
    // `MemBase` (the scale field), then a `MemOffset` (the displacement).
    // Cycle 3d's 4-operand `lea` arm adds an optional `Reg` index
    // operand between base and MemBase. The builder emits these as a
    // paired construct (cycle 3c lowerLoad/lowerStore/lowerGep);
    // `LirVerifier` will check the pairing once ML6 starts consuming
    // memory addresses.
    MemBase    = 6,
    MemOffset  = 7,
    // Pool index for wide literals (int64/double/string/aggregate). The
    // 8-byte operand POD cannot inline a 64-bit immediate without
    // breaking the cache-density invariant, so wide literals flow
    // through `LirLiteralPool` (cycle 3c). `litIndex` indexes into the
    // owning module's pool; consumers fetch via `lir.literalValue(idx)`.
    LiteralIndex = 8,
    // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): a by-value-
    // aggregate stack-arg MARKER on a Call's operand list. It carries the
    // aggregate's BYTE SIZE (`byValueAggBytes`) and ALWAYS immediately
    // FOLLOWS the `Reg` operand holding the aggregate's temp address —
    // mirroring the leading-Reg + trailing-descriptor convention the
    // memory addressing modes use (Reg + MemBase + MemOffset). The
    // preceding Reg is a normal register use (liveness/regalloc track it,
    // keeping the temp address live to the callconv pass); this marker is
    // invisible to liveness/regalloc (not a Reg) and tells lir_callconv to
    // pass that aggregate ENTIRELY in the outgoing overflow area by a
    // byte-wise copy (ceil(size / outgoingSlot) slots), never in a register
    // and never split (SysV §3.2.3/§3.5.7). CC-neutral: the size is the
    // only datum; the overflow placement is the callconv's, config-driven.
    ByValueStackAgg = 9,
};

// One slot in the operand pool. The tag picks the active field.
struct LirOperand {
    LirOperandKind kind = LirOperandKind::None;  // 1
    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: for a ByValueStackAgg
    // marker, which arg-register CLASS the placement EXHAUSTS once this aggregate is
    // stacked (0 = none/BACKFILL — SysV; 1 = GPR; 2 = FPR — AAPCS64 §B). lir_callconv
    // clamps the matching class cursor so a SUBSEQUENT arg/vararg of that class also
    // goes to memory (matching the callee's va_start clamp). Unused (0) for any other
    // operand kind. Repurposes one padding byte — no struct-size change.
    std::uint8_t   byValueAggExhaust = 0;        // 1
    std::uint8_t   _pad[2] = {};                 // 2
    union {
        LirReg        reg;        // 4 — kind == Reg
        std::int32_t  immInt32;   // 4 — kind == ImmInt (truncated; full int64 lives in scalar pool)
        std::uint32_t blockSlot;  // 4 — kind == BlockRef → LirBlockId.v
        std::uint32_t symbolV;    // 4 — kind == SymbolRef → SymbolId.v
        std::uint32_t scale;      // 4 — kind == MemBase (1/2/4/8)
        std::int32_t  offset;     // 4 — kind == MemOffset
        std::uint32_t litIndex;   // 4 — kind == LiteralIndex (into LirLiteralPool)
        std::uint32_t byValueAggBytes; // 4 — kind == ByValueStackAgg (aggregate byte size)
    };

    constexpr LirOperand() noexcept : kind(LirOperandKind::None), reg{} {}

    // ── constexpr factories ────────────────────────────────────────
    //
    // Every operand-building call site (MIR→LIR isel cycles 3a-d, ARM64
    // isel, AS1 assembler, golden-text emitter) ends up filling these
    // exact union arms. Centralised so the discriminator + arm always
    // stay in sync — accidentally setting `kind=Reg` while writing
    // `immInt32` would otherwise be a quiet aliasing bug.
    [[nodiscard]] static constexpr LirOperand makeReg(LirReg r) noexcept {
        LirOperand o{};
        o.kind = LirOperandKind::Reg;
        o.reg  = r;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeImmInt32(std::int32_t v) noexcept {
        LirOperand o{};
        o.kind     = LirOperandKind::ImmInt;
        o.immInt32 = v;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeBlockRef(std::uint32_t v) noexcept {
        LirOperand o{};
        o.kind      = LirOperandKind::BlockRef;
        o.blockSlot = v;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeSymbolRef(std::uint32_t v) noexcept {
        LirOperand o{};
        o.kind     = LirOperandKind::SymbolRef;
        o.symbolV  = v;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeMemBase(std::uint32_t scale) noexcept {
        LirOperand o{};
        o.kind  = LirOperandKind::MemBase;
        o.scale = scale;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeMemOffset(std::int32_t offset) noexcept {
        LirOperand o{};
        o.kind   = LirOperandKind::MemOffset;
        o.offset = offset;
        return o;
    }
    [[nodiscard]] static constexpr LirOperand makeLiteralIndex(std::uint32_t idx) noexcept {
        LirOperand o{};
        o.kind     = LirOperandKind::LiteralIndex;
        o.litIndex = idx;
        return o;
    }
    // FC12a-struct: the by-value-aggregate stack-arg size marker. ALWAYS emitted
    // immediately AFTER the Reg operand holding the aggregate's temp address.
    // `exhaust` (D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS): the arg-register
    // class lir_callconv exhausts after placing this stacked aggregate (0 none/backfill,
    // 1 GPR, 2 FPR) — SysV passes 0, AAPCS64 passes the straddling aggregate's class.
    [[nodiscard]] static constexpr LirOperand
    makeByValueStackAgg(std::uint32_t bytes, std::uint8_t exhaust = 0) noexcept {
        LirOperand o{};
        o.kind             = LirOperandKind::ByValueStackAgg;
        o.byValueAggExhaust = exhaust;
        o.byValueAggBytes  = bytes;
        return o;
    }
};
static_assert(sizeof(LirOperand) == 8, "LirOperand POD must stay 8 bytes");
static_assert(std::is_trivially_copyable_v<LirOperand>);

namespace detail {

// ── instruction POD ──────────────────────────────────────────────
//
// Cache-density 32 bytes. The `opcode` field is a `std::uint16_t`
// whose meaning is defined entirely by the `TargetSchema` the module
// was built with (cycle 2 pivot: opcodes live in JSON, not C++ enums).
// Consumers look up opcode semantics via `schema.opcodeInfo(opcode)`,
// never via a C++ enum. Operands live in the module's operand pool,
// addressed by `[operandStart, operandStart + operandCount)`. The
// optional result register is in `result`; whether it's meaningful
// is per-opcode (TargetResultRule::Value / Optional).
struct LirInst {
    std::uint16_t opcode       = 0;          // 2 — Invalid sentinel = 0
    std::uint8_t  flags        = 0;          // 1
    std::uint8_t  _pad         = 0;          // 1
    LirReg        result       = InvalidLirReg;  // 4 — result reg, if any
    std::uint32_t operandStart = 0;          // 4 — into operand pool
    std::uint32_t operandCount = 0;          // 4
    std::uint32_t payload      = 0;          // 4 — per-opcode scalar (label id, etc.)
    std::uint32_t _pad2        = 0;          // 4 — explicit pad to 24B; future field
};
static_assert(sizeof(LirInst) <= 32, "detail::LirInst grew unexpectedly");
static_assert(std::is_trivially_copyable_v<LirInst>);

// ── basic-block POD ──────────────────────────────────────────────
//
// Mirrors MirBlock: owns a contiguous instruction range + successor
// range, carries an owning function id.
struct LirBlock {
    std::uint32_t instStart = 0;             // 4 — into instruction arena
    std::uint32_t instCount = 0;             // 4 — includes terminator
    std::uint32_t succStart = 0;             // 4 — into succ pool
    std::uint32_t succCount = 0;             // 4
    std::uint32_t func      = 0;             // 4 — owning LirFuncId.v
    std::uint32_t _pad[3]   = {};            // 12 — explicit padding to 32B
};
static_assert(sizeof(LirBlock) <= 32, "detail::LirBlock grew unexpectedly");
static_assert(std::is_trivially_copyable_v<LirBlock>);

// ── function POD ─────────────────────────────────────────────────
struct LirFunc {
    std::uint32_t blockStart = 0;            // 4 — into block arena
    std::uint32_t blockCount = 0;            // 4
    std::uint32_t symbol     = 0;            // 4 — declared SymbolId.v
    std::uint32_t numVRegs   = 0;            // 4 — virtual-register count
    std::uint32_t _pad[4]    = {};           // 16 — explicit padding to 32B
};
static_assert(sizeof(LirFunc) <= 32, "detail::LirFunc grew unexpectedly");
static_assert(std::is_trivially_copyable_v<LirFunc>);

} // namespace detail

} // namespace dss

// ArenaNames specializations — one per arena, four entity names
// (attribute/element/tag/access). Pattern mirrors Mir's per-tier
// declarations in mir_node.hpp.
namespace dss::substrate {

template <>
struct ArenaNames<LirInstId, LirModuleId> {
    static constexpr char const* attribute = "LirAttribute";
    static constexpr char const* element   = "LirInstId";
    static constexpr char const* tag       = "LirModuleId";
    static constexpr char const* access    = "Lir::inst";
};

template <>
struct ArenaNames<LirBlockId, LirModuleId> {
    static constexpr char const* attribute = "LirBlockAttribute";
    static constexpr char const* element   = "LirBlockId";
    static constexpr char const* tag       = "LirModuleId";
    static constexpr char const* access    = "Lir::block";
};

template <>
struct ArenaNames<LirFuncId, LirModuleId> {
    static constexpr char const* attribute = "LirFuncAttribute";
    static constexpr char const* element   = "LirFuncId";
    static constexpr char const* tag       = "LirModuleId";
    static constexpr char const* access    = "Lir::func";
};

} // namespace dss::substrate
