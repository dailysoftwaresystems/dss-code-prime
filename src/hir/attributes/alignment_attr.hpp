#pragma once

#include <cstdint>

// Declaration explicit-alignment side-table value. Attached per-node via
// `HirAttribute<AlignmentAttr>` to a NATIVE declaration HIR node (Global or
// VarDecl) that carried a source-level `alignas(N)` / `alignas(T)` specifier
// (C11/C23 6.7.5) — `alignas(16) int g;` → `alignmentBytes = 16`. Populated by
// CST→HIR lowering from the bound symbol's `SymbolRecord.explicitAlignment`
// (which the semantic phase already validated: power-of-two, ≤256, ≥ the
// declaration's natural alignment, in a legal context). Read at HIR→MIR
// lowering:
//   * a GLOBAL's value is stamped onto `MirGlobal.alignment`, which the
//     assembler's data-item emission (`lowerMirGlobalsToDataItems`) raises the
//     emitted symbol's section alignment to (max with the type's natural align);
//   * a LOCAL's value combines with the type's natural alignment to form the
//     alloca's EFFECTIVE alignment, threaded to the frame layout so the slot is
//     placed at the required boundary (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN).
//
// Deliberately distinct from `MutabilityAttr` / `VolatileAttr`: alignment is an
// orthogonal axis (an `alignas(16) const volatile int g;` is over-aligned AND
// read-only AND volatile — three independent side-tables), so each map stays
// sparse on its own axis.
//
// A declaration node with NO attribute defaults to "no explicit override" — the
// natural (type-derived) alignment applies, which is the correct default for
// every un-annotated variable. The `alignas(0)` no-op (6.7.5p3) is left
// nullopt in `SymbolRecord.explicitAlignment`, so it never reaches here — the
// side-table is populated ONLY when a real (>0) override was recorded, keeping
// it sparse.

namespace dss {

struct AlignmentAttr {
    // The explicit `alignas` alignment in bytes — a power of two in [1, 256]
    // (the semantic phase guarantees the range). Never 0: a 0/absent override
    // is represented by the ABSENCE of an attribute, not a zero value.
    std::uint32_t alignmentBytes = 0;
};

} // namespace dss
