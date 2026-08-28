#pragma once

#include <cstdint>

// D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED: the PROVENANCE a
// `D-CSUBSET-LOCAL-STATIC` promotion throws away.
//
// A block-scope `static` is lowered as a hidden MODULE GLOBAL and NOTHING is
// appended to the function body (see the `staticStorage` arm of CST→HIR's
// declarator lowering), which is the right shape — the storage IS static and the
// initializer IS load-time. But the initializer travels with it, and an
// initializer may name something that only exists INSIDE the function it left:
// `static void *tbl[] = {&&L0, &&L1};`.
//
// ★ WHY A BARE ORDINAL CANNOT BE RESOLVED WITHOUT THIS. Label ordinals are
// PER-FUNCTION and restart at 0 for every function body (CST→HIR saves/restores
// `labelOrdinals_` + `nextLabelOrdinal_` around each one), so two functions that
// each write `static void *p = &&L;` produce two globals whose initializers both
// say "label ordinal 0". Scanning module globals for a `LabelAddressOf` and
// attributing the hit to whichever function is being lowered is therefore not a
// heuristic that is usually right — it is ambiguous by construction, and it
// resolves one function's label address to ANOTHER function's block. This
// attribute is the missing half of the key.
//
// Keyed on the promoted GLOBAL node (the declaration-keyed discipline of
// `HirMutabilityMap` / `HirThreadLocalMap`). Absent for every file-scope global:
// a file-scope initializer has no enclosing function, and no label is in scope
// there, so absence is a complete answer rather than a missing entry.

namespace dss {

struct EnclosingFunctionAttr {
    // `SymbolId.v` of the function whose body the declaration was written in.
    // Stored raw (like every other attribute value struct, which carry no
    // `Hir`/`SymbolId` dependency of their own).
    std::uint32_t functionSymbol = 0;
};

} // namespace dss
