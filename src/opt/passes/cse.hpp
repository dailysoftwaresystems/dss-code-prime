#pragma once

// MIR-tier Common Subexpression Elimination + Global Value Numbering.
//
// **Scope (OPT5 c1)**: dom-tree-scoped value numbering on pure SSA
// instructions. For each opcode-and-operand-canonical-tuple seen
// earlier on the current dom-tree path, redirect every later use to
// the earlier definition. The dead duplicate survives the rebuild
// and is swept by the subsequent DCE pass (same shape as CopyProp).
//
// **Value-numbering key**: `(opcode, resultType, canonical_operands)`.
// Operand canonicalization (a) transitively resolves each operand
// through the in-progress `cseMap_` (path compression at end), and
// (b) for opcodes where `isCommutative(op)` returns true AND
// `operands.size() == 2`, sorts the operand pair by id so
// `op(a, b)` and `op(b, a)` hash equal. Asymmetric comparisons /
// non-commutative arithmetic (Sub, SDiv, Shl, ICmpSlt, ...) are
// NOT canonicalized — the corpus pin `cse_noncommutative` enforces
// exit 58 vs the buggy 70 a permissive predicate would produce
// (D-OPT1-CSE-NONCOMMUTATIVE-PIN).
//
// **Dominance scoping**: the pass runs an iterative dom-tree DFS
// (Visit/Leave frames + explicit stack — same shape as Mem2Reg's
// rename walk) maintaining a hash table whose entries are scoped to
// the current dom-tree subtree. An entry added at block B is visible
// to every block B dominates and is rolled back when the DFS leaves
// B's subtree. This ensures every CSE replacement satisfies SSA's
// def-dominates-use invariant.
//
// **Excluded opcodes**: side-effecting (`opcodeInfo.hasSideEffects`),
// terminators, `Phi` (CopyProp's Phi-collapse handles trivial cases;
// non-trivial Phi CSE is a separate concern), `Load` (alias-unsafe;
// CSE'ing a Load before alias analysis lands is incorrect when an
// intervening Store may have mutated memory), and Volatile-flagged
// instructions (observable semantics).
//
// **Why not include Load**: Load CSE requires proving no intervening
// Store could have rewritten the loaded memory. Without alias
// analysis the conservative answer is "any Store may alias any Load"
// → Load CSE is illegal in any block containing a Store. The narrow
// case (Load + no Store anywhere in the function) buys little; the
// general case lands with the alias-analysis substrate.
//
// **Runtime-init globals carve-out**: same shape as ConstFold + DCE
// + Mem2Reg + CopyProp via the shared `cloneGlobalsOrCarveOut`
// helper (D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>

namespace dss::opt::passes {

struct CseResult {
    bool        ok                  = false;
    std::size_t instructionsCsed    = 0;
};

[[nodiscard]] DSS_EXPORT CseResult
runCse(Mir& mir, TypeInterner const& interner,
       DiagnosticReporter& reporter);

} // namespace dss::opt::passes
