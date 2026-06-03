#pragma once

// MIR-tier Dead Code Elimination.
//
// Three reachability passes compose:
//
//  (1) Inter-procedural function/global live-symbol set. Seeded with
//      every function/global where `isExternallyVisible(binding,
//      visibility) == true`. BFS expands via live `GlobalAddr`
//      instructions inside live functions — each one references a
//      callee/global by SymbolId; the referenced function/global
//      joins the live-symbol set and re-triggers the BFS.
//      Functions/globals not in the final live-symbol set AND not
//      externally-visible are elided from the rebuilt module.
//
//  (2) Per-function CFG block reachability via `mirReversePostOrder`.
//      Blocks not reachable from `funcEntry` are dead — their
//      instructions are elided regardless of side-effect classification
//      (unreachable side effects are by definition unobservable).
//
//  (3) Per-function intra-block instruction live-set worklist. Roots
//      are every instruction with `opcodeInfo(op).hasSideEffects ==
//      true` (Store / Call / IntrinsicCall / Alloca / terminators) OR
//      `MirInstFlags::Volatile` set (volatile Load) in a reachable
//      block. BFS expands backward through `instOperands(id)`. Any
//      side-effect-free instruction not reached = dead, elided.
//
// **DCE-PROTECT CONTRACT** (D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD):
// the pass MUST consult `isExternallyVisible(funcBinding,
// funcVisibility)` / `isExternallyVisible(globalBinding,
// globalVisibility)` before deleting any function or global. A symbol
// that's externally visible is observable from outside the CU —
// deleting it is a miscompile. D-OPT2-DCE-LINKAGE-SYMTAB-ASSERTION
// pins this contract via test_dce_linkage.cpp.
//
// **TRAP** (the `dce_negative_pin` corpus example): an unconditional
// `x = 100` followed by a conditional `if (a > 0) x = 7` — both
// stores reach the `return x` join (the conditional one only on the
// taken path; the unconditional one always). Both must survive DCE
// despite the syntactic appearance that one might be overwritten.
// A naive value-numbering "last-store-wins" pass that ignores
// control flow would mis-delete the unconditional store. DCE solves
// this trivially: every `hasSideEffects=true` instruction in a CFG-
// reachable block is kept. The smarter "is this Store actually live
// at runtime?" pass is a copy-prop / store-sinking concern, not DCE.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>

namespace dss::opt::passes {

struct DceResult {
    bool        ok                    = false;
    std::size_t instructionsEliminated = 0;
    std::size_t blocksEliminated       = 0;
    std::size_t functionsEliminated    = 0;
    std::size_t globalsEliminated      = 0;
};

[[nodiscard]] DSS_EXPORT DceResult
runDce(Mir& mir, TypeInterner const& interner,
       DiagnosticReporter& reporter);

} // namespace dss::opt::passes
