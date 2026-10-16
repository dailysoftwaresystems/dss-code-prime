#pragma once

// HOW A MIR-TIER SYNTHESIS PASS CONTINUES THE MODULE'S SYMBOL-ID SPACE.
//
// Three passes append symbols to a module after the front end has numbered it:
// `realizeEntryShape` (the pre-main init, a GLOBAL definition, and the CRT imports it
// calls), `synthesizeSehFunclets` (the filter funclets and the personality import) and
// `synthesizeThreadsShim` (the vehicle's helper imports). The CALLER then names every
// id through ITS OWN table — the semantic model's records on the single-CU route
// (`model.recordFor(id)`, indexed by id), the merge's `symbolNames` on the
// whole-program route — and a symbol the table names enters the module's symbol table
// under that name. So a fresh id has to clear every id the NAME TABLE holds, most of
// which the module never sees — a local, a parameter, a typedef, a tag, a field, an
// enum or shipped constant (`thrd_success`), the predefined `__func__`.
// ✔MEASURED P69 round 4 (lane lm): `int main(int argc, char **argv, char **envp)` with
// no string literal, built for x86_64:elf64-x86_64-linux-exec, carried `T __func__` in
// its symbol table — the synthesized stack-vector init, whose id (the module's highest
// + 1) the semantic table gives to main's `__func__`, published GLOBAL under that name.
// The <threads.h> call_once adapter was minted as `_thrd_success` the same way until
// P69.
//
// THE MODULE CARRIES THAT END (P69 round 5): `Mir::symbolIdEnd`, stated once where the
// module is made from its table and carried by every rebuild, and a fresh id comes from
// the module's builder alone (`MirBuilder::mintSymbolOrAbort`) — the one door. Round 4
// had instead handed each of these passes the table's end as an argument, from four
// seams; the optimizer's minter was a fifth site with no seam, and minted inside the
// table (✔MEASURED 2026-10-08, D-MIR-SYNTHESIZED-SYMBOL-MINTED-INSIDE-THE-NAME-TABLE).
// A pass takes no end now, so no seam can pass a wrong one or forget it. (The link-tier
// sibling is link/fresh_symbol_ids.hpp's `maxExistingSymbolIdV`: an assembled module's
// symbols already carry their names, so no table stands beside it.)
//
// What is left here is the ONE thing these three passes share and the module cannot
// do alone: an import's row travels BESIDE the module (`externImports`), so the pass is
// handed the rows as a second argument. In a module made from a table every row's id is
// below the module's end already (HIR→MIR and the merge both carry their imports' ids
// into it); a hand-built pair — a unit test's module and rows — is tied by nothing, and
// this keeps such a pair numbered right as well.

#include "core/types/extern_import.hpp"   // ExternImport
#include "mir/mir.hpp"

#include <span>

namespace dss {

// Opens a synthesis pass's rebuild of `mir` into `builder`: the rebuilt module
// continues `mir`'s symbol-id space, kept clear of every import row handed beside it.
// Call it BEFORE the first `mintSymbolOrAbort` — which aborts on a builder that was
// never told whose ids it continues.
inline void continueSymbolIdsPastImports(MirBuilder& builder, Mir const& mir,
                                         std::span<ExternImport const> imports) noexcept {
    builder.continueSymbolIdsOf(mir);
    for (ExternImport const& e : imports) builder.keepSymbolIdsClearOf(e.symbol);
}

} // namespace dss
