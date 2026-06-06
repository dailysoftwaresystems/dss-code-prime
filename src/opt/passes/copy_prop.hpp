#pragma once

// MIR-tier SSA copy-prop — trivial Phi-collapse.
//
// **Scope (OPT4 c3)**: this pass implements the SSA Phi-collapse
// fragment of copy-prop. A Phi node whose non-self incomings ALL
// resolve to the same value `V` is redundant — every use of the Phi
// reads `V`. Uses are redirected to `V`; the now-dead Phi survives
// the rebuild and is swept by the subsequent DCE pass.
//
// **Worklist + path compression**: a Phi P1 may have an incoming
// that is another Phi P2; if P2 collapses to V, P1's effective
// incomings re-evaluate and may also collapse. The analysis walks a
// worklist until quiescent + path-compresses the result so
// `collapseMap[P1]` resolves directly to V (not to P2).
//
// **SSA self-references** (loop-header Phis) are filtered AFTER
// transitive resolution: a Phi with incomings {self, V} collapses
// to V (the self-reference is an artifact of the back-edge, not a
// distinct value). A Phi with incomings {self, V1, V2} (V1 ≠ V2)
// does NOT collapse — it carries real merge information.
//
// **Mechanism**: the pass overrides `MirRebuildPolicy::substituteOldOperand`
// to redirect any old-id reference to a collapsed Phi back to its
// canonical target. The rebuilder's wrapped operand-resolution path
// (`rewriteOperand(substituteOldOperand(o))`) maps target's old id
// to its new id — emitted by SSA's def-dominates-use invariant +
// RPO block order before any user is processed. The Phi-collapse
// pass does NOT skip emission of the collapsed Phi; it stays as
// dead-code which DCE eliminates next.
//
// **Why not the general operand-substitution variant**: a general
// copy-prop (replace use of `x` with `y` where `y` dominates) needs
// a per-block dominator cursor (anchored: D-OPT-COPYPROP-BLOCK-CURSOR).
// Phi-collapse doesn't need it — the Phi inherently encodes the
// dominance relation (the incoming value dominates the Phi's block).
//
// **Fixed-point**: the pass is internally fixed-point (worklist
// drains until quiescent). Running it twice produces no further
// mutations (idempotent). Pipeline-level fixed-point — re-running
// CopyProp + DCE alternately — anchors as D-OPT-FIXED-POINT-LOOP
// but doesn't activate until the second mutually-enabling pass
// arrives.
//
// **Runtime-init globals carve-out**: mirrors ConstFold + DCE +
// Mem2Reg — module with `globalInitFunc.valid()` skips with Info.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>

namespace dss::opt::passes {

struct CopyPropResult {
    bool        ok               = false;
    std::size_t phisCollapsed    = 0;
};

[[nodiscard]] DSS_EXPORT CopyPropResult
runCopyProp(Mir& mir, TypeInterner const& interner,
            DiagnosticReporter& reporter);

} // namespace dss::opt::passes
