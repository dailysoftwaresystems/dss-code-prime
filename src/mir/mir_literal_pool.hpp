#pragma once

#include "core/export.hpp"
#include "core/types/bit_int_value.hpp"            // BitIntValue (C23 _BitInt const-fold arm)
#include "core/types/wide_float_value.hpp"         // WideFloatValue (LD-3 F80/F128 const-fold arm)
#include "core/types/type_lattice/core_type.hpp"   // TypeKind

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// Per-module literal value pool (ML1). A MIR `Const` instruction carries only a
// pool index in its `payload`; the decoded VALUE lives here. This mirrors the
// HIR `HirLiteralPool`: MIR keeps its own copy (a parallel IR owns parallel
// data) so the `mir` library depends only on `core`, exactly like `hir`. HIR→MIR
// lowering (ML2) copies each `HirLiteralValue` into a `MirLiteralValue`; the
// optimizer, when it rebuilds a module, copies the surviving entries (and
// constant-folding appends new ones). The `.dssir` text format (ML4) reads the
// value from here, so a synthetic module with no source still carries it.

namespace dss {

struct MirLiteralValue;

// Aggregate literal: mirror of `HirAggregateValue` on the MIR side.
// Produced by MIR-globals when the HIR const-eval folds an aggregate
// `ConstructAggregate` (D5.3) — module-scope `struct Point p = {1, 2};`
// lands as a const-init rather than degrading to a runtime-init
// `__module_init__` synthesis. The recursive shape mirrors the HIR side.
struct MirAggregateValue {
    std::vector<MirLiteralValue> fields;
};

// Symbol-address literal (F5 — D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): a global whose
// initializer is the LINK-TIME-CONSTANT address of another symbol — a string
// literal's rodata global (`char* g = "..."`), another global (`int* p = &x`),
// or a function (a function-pointer table). It is NOT a runtime initializer: the
// assembler emits a pointer-width slot + an absolute-64 RELOCATION against
// `symbol` (+ `addend`), and the linker fills the target's VA at link time.
// `symbol` is the target's `SymbolId` underlying value, kept raw so the pool
// stays core-only (the assembler reconstructs `SymbolId{symbol}`).
struct MirSymbolAddrValue {
    std::uint32_t symbol = 0;
    std::int64_t  addend = 0;
};

struct MirLiteralValue {
    // `monostate` = a literal whose value is unknown (carried so a malformed
    // source still lowers + diagnoses). `core` is a denormalized hint enabling
    // pool-level inspection without consulting the interner; the Const
    // instruction's typeId is the authority — disambiguate char vs string by the
    // VARIANT ARM (uint64 vs string), never by `core`. D5.3 adds the
    // `MirAggregateValue` arm for `core` ∈ {Struct, Union, Array}. C4b adds the
    // `BitIntValue` arm (`core == BitInt`) — the SAME host bit-precise value type
    // the HIR pool carries (D-CSUBSET-BITINT-WIDE-LITERAL): a narrow literal's
    // container value + a wide literal's limbs both flow through it; the globals
    // byte-emitter fails loud on it (wide `_BitInt` data-globals are deferred).
    // LD-3 adds the `WideFloatValue` arm (`core` ∈ {F80, F128}) — the SAME host
    // wide-float value type the HIR pool carries (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-
    // PRECISION): a FOLDED F80/F128 arithmetic result at TRUE 80/128-bit
    // precision. The globals byte-emitter encodes it via `appendWideFloatBits`
    // (its `get_if<WideFloatValue>` branch, checked BEFORE the `double` arm); an
    // UNFOLDED F80/F128 leaf still rides the pre-existing `double` arm's dedicated
    // `appendF80Extended`/`appendF128` widen path.
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string,
                 MirAggregateValue, MirSymbolAddrValue, BitIntValue, WideFloatValue> value;
    TypeKind core = TypeKind::Void;
};

class DSS_EXPORT MirLiteralPool {
public:
    // Append a literal value; returns its index (the Const instruction payload).
    // No dedup — every occurrence gets its own slot (dedup is an optimizer
    // concern; keeps add O(1)).
    [[nodiscard]] std::uint32_t add(MirLiteralValue v);

    [[nodiscard]] MirLiteralValue const& at(std::uint32_t index) const;
    [[nodiscard]] std::size_t            size()  const noexcept { return pool_.size(); }
    [[nodiscard]] bool                   empty() const noexcept { return pool_.empty(); }

private:
    std::vector<MirLiteralValue> pool_;
};

} // namespace dss
