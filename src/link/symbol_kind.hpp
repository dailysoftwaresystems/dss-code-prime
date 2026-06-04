#pragma once

#include "core/types/strong_ids.hpp"  // CompilationUnitId, SymbolId

#include <cstddef>
#include <cstdint>
#include <functional>

// D-LK4-3 — symbol-namespace promotion. The linker holds the symbols of every
// CompilationUnit it links. `SymbolId` is per-arena (per-CU), so two CUs can mint
// the same integer for DIFFERENT symbols — a silent collision the moment their
// symbol tables coexist (the multi-CU merge LK11 performs). The linker therefore
// indexes by the COMPOUND key `(CompilationUnitId, SymbolId)`, never the bare
// `SymbolId`.
//
// `SymbolKind` consolidates the three former parallel symbol sets (functions /
// extern imports / rodata data items) into one map's value
// (D-LK4-RODATA-LINKER-SYMBOL-KIND-MAP) — one map-find per relocation instead of
// three `.contains()`, plus the which-kind-defined-this distinction the
// cross-reference diagnostics consult. Closed enum; future kinds (TlsDefined /
// WeakDefined) extend it without resolution-loop churn.

namespace dss {

enum class SymbolKind : std::uint8_t {
    Defined = 0,  // an intra-module function definition (AssembledFunction)
    Extern  = 1,  // an FFI import (ExternImport), resolved via the import table
    Data    = 2,  // a rodata / data item (AssembledData)
};

// A `SymbolId` scoped to its defining CompilationUnit — the linker's collision-
// proof symbol-index key. Equality + hash compare the id payloads only (`.v`),
// matching the strong-id convention.
struct LinkedSymbolKey {
    CompilationUnitId cuId{};
    SymbolId          symbol{};

    [[nodiscard]] friend bool operator==(LinkedSymbolKey const& a,
                                         LinkedSymbolKey const& b) noexcept {
        return a.cuId.v == b.cuId.v && a.symbol.v == b.symbol.v;
    }
};

} // namespace dss

template <>
struct std::hash<dss::LinkedSymbolKey> {
    [[nodiscard]] std::size_t operator()(dss::LinkedSymbolKey const& k) const noexcept {
        // boost-style hash_combine of the two 32-bit ids — avoids the trivial
        // collisions a bare XOR produces on adjacent ids.
        std::size_t h = std::hash<std::uint32_t>{}(k.cuId.v);
        h ^= std::hash<std::uint32_t>{}(k.symbol.v) + 0x9e3779b97f4a7c15ULL
             + (h << 6) + (h >> 2);
        return h;
    }
};
