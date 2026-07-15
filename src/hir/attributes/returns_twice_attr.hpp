#pragma once

// FC17.9(c) (D-CSUBSET-SETJMP): returns-twice side-table value. Attached per-node
// via `HirAttribute<ReturnsTwiceAttr>` to a USER-LEVEL `Call` HIR node whose callee
// is a DIRECT reference to a function symbol declared `returnsTwice` (a shipped
// `setjmp`/`_setjmp`). Populated by CST->HIR lowering from the callee's
// `SymbolRecord.returnsTwice` (set by the semantic phase from the descriptor's
// `returnsTwice` bit); read at HIR->MIR lowering and OR'd into the emitted `Call`'s
// `MirInstFlags::ReturnsTwice`.
//
// This is the EXACT structural mirror of `VolatileAttr`: both are populated from a
// per-symbol bool, keyed on the USE node (here the Call; there the access), and flow
// to a MIR instruction FLAG (here the Call; there the Load/Store) that the optimizer
// consults. The distinction from `noreturn` (which is ALSO a per-symbol bool but is
// HIR-DISCHARGED into an `Unreachable` block and never reaches MIR) is exactly why a
// side-table + MIR carrier is needed: the returns-twice optimizer treatment lives in
// MIR passes (mem2reg / inliner), so the fact MUST survive to MIR.
//
// DIRECT-callee only (the `isDirectNoreturnCall` discipline): a returns-twice function
// is address-takeable, so an INDIRECT / ternary / cast callee lowers to a non-Ref node
// and is conservatively NOT flagged (a missed flag is safe for the walking-skeleton —
// determinate cases already work because a Call is a memory barrier and longjmp
// restores callee-saved+SP; the only unsafe direction is a spurious flag, which the
// direct-Ref gate prevents). A node with NO attribute defaults to `returnsTwice = false`
// — an ordinary Call the optimizer may freely transform. The side-table stays sparse:
// only returns-twice calls are annotated.

namespace dss {

struct ReturnsTwiceAttr {
    bool returnsTwice = false;
};

} // namespace dss
