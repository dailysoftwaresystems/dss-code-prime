#pragma once

#include "core/types/strong_ids.hpp"
#include "lir/lir_opcode.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <type_traits>

// LIR node PODs (plan 12 §2.1 file tree → `lir_inst.hpp` + per-block /
// per-function shape). Each POD ≤ 32 bytes for cache density; the
// arena container assumes trivial-copyability.

namespace dss {

// Operand variant tag carried on each entry of the LIR operand pool.
// LIR operands are heterogeneous (registers / immediates / memory
// addressing modes); the kind discriminator picks the right field
// to read from the pool slot.
enum class LirOperandKind : std::uint8_t {
    None       = 0,
    Reg        = 1,  // virtual or physical register (pre/post regalloc)
    ImmInt     = 2,  // sign-extended int64 immediate
    ImmFloat   = 3,  // double immediate (passed verbatim to encoder)
    BlockRef   = 4,  // basic-block reference (br targets, etc.)
    SymbolRef  = 5,  // symbol reference (call targets, GlobalAddr)
    // Memory operands (base + index + scale + displacement) are a
    // sequence of two pool entries (a `MemBase` followed by a
    // `MemOffset`) so each pool slot stays a uniform 8 bytes. The
    // builder emits them as a paired construct; the verifier checks
    // the pairing.
    MemBase    = 6,
    MemOffset  = 7,
};

// One slot in the operand pool. The tag picks the active field.
struct LirOperand {
    LirOperandKind kind = LirOperandKind::None;  // 1
    std::uint8_t   _pad[3] = {};                 // 3
    union {
        LirReg        reg;        // 4 — kind == Reg
        std::int32_t  immInt32;   // 4 — kind == ImmInt (truncated; full int64 lives in scalar pool)
        std::uint32_t blockSlot;  // 4 — kind == BlockRef → LirBlockId.v
        std::uint32_t symbolV;    // 4 — kind == SymbolRef → SymbolId.v
        std::uint32_t scale;      // 4 — kind == MemBase (1/2/4/8)
        std::int32_t  offset;     // 4 — kind == MemOffset
    };

    constexpr LirOperand() noexcept : kind(LirOperandKind::None), reg{} {}
};
static_assert(sizeof(LirOperand) == 8, "LirOperand POD must stay 8 bytes");
static_assert(std::is_trivially_copyable_v<LirOperand>);

namespace detail {

// ── instruction POD ──────────────────────────────────────────────
//
// Cache-density 32 bytes. The `opcode` field is a raw `std::uint16_t`
// that the `Lir::targetId()` selects between per-target enums
// (`X86_64Opcode`, `ARM64Opcode`). Operands live in the module's
// operand pool, addressed by `[operandStart, operandStart + operandCount)`.
// The optional result register is in `result`; whether it's
// meaningful is per-opcode (Result::Value / Optional).
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
