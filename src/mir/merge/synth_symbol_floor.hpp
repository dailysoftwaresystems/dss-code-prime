#pragma once

// THE ONE FLOOR A MIR-TIER SYNTHESIS PASS MINTS A FRESH SYMBOL ABOVE.
//
// Three passes append symbols to a module after the front end has numbered it:
// `realizeEntryShape` (the pre-main init, a GLOBAL definition, and the CRT imports it
// calls), `synthesizeSehFunclets` (the filter funclets and the personality import) and
// `synthesizeThreadsShim` (the vehicle's helper imports). The CALLER then names every
// id through ITS OWN table — the semantic model's records on the single-CU route
// (`model.recordFor(id)`, indexed by id), the merge's `symbolNames` on the
// whole-program route — and a symbol the table names enters the module's symbol table
// under that name. So a fresh id has to clear two spaces, not one:
//   * every id the MODULE holds — a defined function, a module global, an extern
//     import (the globals are load-bearing: HIR→MIR mints its synthetic string-literal
//     globals ABOVE every function and extern, so in a real program the highest id is
//     usually a global's);
//   * every id the NAME TABLE holds, most of which the module never sees — a local, a
//     parameter, a typedef, a tag, a field, an enum or shipped constant
//     (`thrd_success`), the predefined `__func__`.
// ✔MEASURED P69 round 4 (lane lm): `int main(int argc, char **argv, char **envp)` with
// no string literal, built for x86_64:elf64-x86_64-linux-exec, carried `T __func__` in
// its symbol table — the synthesized stack-vector init, whose id (the module's highest
// + 1) the semantic table gives to main's `__func__`, published GLOBAL under that name.
// The <threads.h> call_once adapter was minted as `_thrd_success` the same way until
// P69. Each pass scanned the module alone, in a private copy of the scan; this header
// is the one copy, and it reads both spaces. (Its link-tier sibling is
// link/fresh_symbol_ids.hpp's `maxExistingSymbolIdV`, hoisted for the same reason: an
// assembled module's symbols already carry their names, so no table stands beside it.)
//
// `nameTableEnd` is ONE PAST the highest id the caller's name table holds: the
// single-CU seams pass `model.symbols().size()` — the floor HIR→MIR lifts its own
// synthetic globals over (`syntheticSymbolFloor`; D-MIR-SYNTHETIC-GLOBAL-SYMBOL-ALIAS
// is this rule for those globals) — and the merged seams `nameTableEndOf(symbolNames)`.
// Every pass takes it as a REQUIRED argument, never a defaulted one: a seam that
// omitted it would mint blind again and still compile. Only a hand-built module with
// no name table at all (a unit test's) passes `kNoNameTable`.

#include "core/types/extern_import.hpp"   // ExternImport
#include "mir/mir.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

namespace dss {

// The `nameTableEnd` of a module no name table names (a hand-built test module).
inline constexpr std::uint32_t kNoNameTable = 0;

// One past the largest id a merged module's `symbolNames` holds (0 when empty).
[[nodiscard]] inline std::uint32_t
nameTableEndOf(std::unordered_map<std::uint32_t, std::string> const& symbolNames) noexcept {
    std::uint32_t end = 0;
    for (auto const& [v, name] : symbolNames) end = std::max(end, v + 1u);
    return end;
}

// The highest SymbolId.v already TAKEN — by the module (every defined function, module
// global and extern import) or by the caller's name table (every id below
// `nameTableEnd`). A pass mints `taken + 1`, `taken + 2`, ….
[[nodiscard]] inline std::uint32_t
highestTakenSymbolIdV(Mir const& mir, std::span<ExternImport const> externs,
                      std::uint32_t nameTableEnd) noexcept {
    std::uint32_t taken = nameTableEnd == 0 ? 0 : nameTableEnd - 1u;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i)
        taken = std::max(taken, mir.funcSymbol(mir.funcAt(i)).v);
    std::size_t const ng = mir.moduleGlobalCount();
    for (std::uint32_t i = 0; i < ng; ++i)
        taken = std::max(taken, mir.globalSymbol(mir.globalAt(i)).v);
    for (auto const& e : externs) taken = std::max(taken, e.symbol.v);
    return taken;
}

} // namespace dss
