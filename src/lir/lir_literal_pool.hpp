#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"              // SymbolId
#include "core/types/type_lattice/core_type.hpp"  // TypeKind

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// Per-module LIR literal value pool (ML5 cycle 3c). A LIR instruction
// referencing a wide constant carries a `LirOperandKind::LiteralIndex`
// operand whose `litIndex` indexes into this pool; the decoded VALUE
// lives here. Mirrors `MirLiteralPool` for the same architectural
// reason: a parallel IR owns parallel data so the `lir` library depends
// only on `core`. Wide-literal handling (int64/double/string) flows
// MIR Const → MIR literal pool → LIR Const → LIR literal pool; small
// integer literals continue to use the `LirOperandKind::ImmInt`
// inline-encoded operand arm. ML8's `.dsslir` text format will read
// from here, same discipline as MIR `.dssir`.

namespace dss {

struct LirLiteralValue;

// LIR aggregate literal — used when an MIR const-fold produces a
// composite value that flows into a LIR const-materialization
// instruction. ML5 cycle 3c does not yet emit one (cycle 3d's aggregate
// ops will), but the variant arm is reserved so the pool's shape
// matches MIR's parallel arm-for-arm.
struct LirAggregateValue {
    std::vector<LirLiteralValue> fields;
};

// ★★★ A LINK-TIME VALUE: `symbol`'s ADDRESS PLUS `addend`. It is the value of an
// assembly operand written `msg+4(%rip)` — known exactly once the linker has
// placed `symbol`, and to no tier before it, so the instruction carries a
// RELOCATION for it rather than a number (D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER).
//
// ★★ IT LIVES IN THIS POOL FOR THE REASON EVERY OTHER ARM DOES: the 8-byte
// `LirOperand` cannot hold a 4-byte symbol AND an 8-byte addend, and a LIR
// operand naming a wide value does so by index (`LiteralIndex`,
// `MemSymbolOffset`). ⚠ It has no MIR twin — MIR names a global's address by
// instruction, never as a pool value — so this arm is the one place the LIR
// pool is not arm-for-arm the MIR pool.
struct LirSymbolAddress {
    SymbolId     symbol{};
    std::int64_t addend = 0;
};

struct LirLiteralValue {
    // `monostate` = unknown / poison (carried so a malformed input still
    // round-trips). `core` is a denormalized hint for pool-level
    // inspection without consulting the interner — the producing inst's
    // result-type is the authority. Disambiguate char vs string by the
    // VARIANT ARM (uint64 vs string), never by `core`.
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
                 std::string, LirAggregateValue, LirSymbolAddress> value;
    TypeKind core = TypeKind::Void;
};

class DSS_EXPORT LirLiteralPool {
public:
    // Append a literal value; returns its index (consumed by the LIR
    // inst's `LirOperandKind::LiteralIndex` operand `litIndex` field).
    // No dedup — every occurrence gets its own slot (dedup is an
    // optimizer concern). Same discipline as `MirLiteralPool::add`.
    [[nodiscard]] std::uint32_t add(LirLiteralValue v);

    [[nodiscard]] LirLiteralValue const& at(std::uint32_t index) const;
    [[nodiscard]] std::size_t            size()  const noexcept { return pool_.size(); }
    [[nodiscard]] bool                   empty() const noexcept { return pool_.empty(); }

private:
    std::vector<LirLiteralValue> pool_;
};

} // namespace dss
