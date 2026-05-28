#pragma once

#include "core/export.hpp"
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

struct MirLiteralValue {
    // `monostate` = a literal whose value is unknown (carried so a malformed
    // source still lowers + diagnoses). `core` is a denormalized hint enabling
    // pool-level inspection without consulting the interner; the Const
    // instruction's typeId is the authority — disambiguate char vs string by the
    // VARIANT ARM (uint64 vs string), never by `core`.
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string> value;
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
