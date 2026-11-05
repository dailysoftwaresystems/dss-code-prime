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
// ★★ AND WHAT "OUTSIDE" MEANS DEPENDS ON THE MODULE'S EXTENT
// (D-OPT-DCE-DELETES-A-RELOCATABLE-MEMBERS-HIDDEN-DEFINITIONS; the fact is
// `opt::ModuleExtent`, documented where it is declared). For a
// `LinkInput` — one input of a link that completes it — every definition
// with EXTERNAL LINKAGE is a root, hidden ones included: another input of
// the same link resolves it by name. Only a `WholeImage` may read the
// image-level `isExternallyVisible` as its root predicate, and there the
// image's entry (`entryRoots`) is a root too: the entry trampoline names it.
// The defaults are the hand-built-fixture convention (the module IS the
// whole program, no entry), byte-identical for every direct caller.
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
#include "opt/optimizer.hpp"   // ModuleExtent

#include <cstddef>
#include <span>

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
       DiagnosticReporter& reporter,
       ModuleExtent extent = ModuleExtent::WholeImage,
       std::span<SymbolId const> entryRoots = {});

} // namespace dss::opt::passes
