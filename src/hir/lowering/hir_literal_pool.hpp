#pragma once

#include "core/export.hpp"
#include "core/types/type_lattice/core_type.hpp"   // TypeKind

#include <cstdint>
#include <variant>
#include <vector>

// Per-CU literal value pool (plan 09 HR8). HIR `Literal` nodes carry only a
// `literalIndex` (a per-occurrence ordinal); the decoded VALUE lives here,
// indexed by that ordinal. The value is decoded ONCE at lowering time (from the
// CST token text, honoring the language's `numberStyle`) and is first-class IR
// data — NOT recovered from a source span, so synthetic HIR (transpile /
// constant-fold, which has no CST) can still carry literal values. This is the
// store HR7's reserved inline-`.dsshir`-value syntax and MIR/codegen read from.
//
// The variant covers the c-subset literal surface (bool / signed / unsigned /
// floating). String/char encodings and 128-bit integers are additive when a
// language needs them.

namespace dss {

struct HirLiteralValue {
    // `monostate` = a literal whose value could not be decoded (a malformed
    // token); the lowering still emits the node + a diagnostic so analysis
    // continues. `core` records the decoded TypeKind for pool-level inspection
    // without consulting the interner (redundant with the node's typeId).
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double> value;
    TypeKind core = TypeKind::Void;
};

class DSS_EXPORT HirLiteralPool {
public:
    // Append a literal value; returns its index (the `literalIndex` payload of
    // the corresponding HIR `Literal` node). No dedup — every occurrence gets
    // its own slot (dedup is an optimizer concern; keeps add O(1)).
    [[nodiscard]] std::uint32_t add(HirLiteralValue v);

    [[nodiscard]] HirLiteralValue const& at(std::uint32_t index) const;
    [[nodiscard]] std::size_t             size() const noexcept { return pool_.size(); }
    [[nodiscard]] bool                    empty() const noexcept { return pool_.empty(); }

private:
    std::vector<HirLiteralValue> pool_;
};

} // namespace dss
