#pragma once

// Call-graph strongly-connected-component (SCC) analysis — the RECURSION
// SAFETY substrate for the function inliner (OPT7 cycle 3).
//
// One free function — `computeCallGraphSccs` — answering "which functions
// belong to the same recursive cycle?" over MIR vocabulary ONLY. The
// inliner consults it to REFUSE inlining any call whose caller + callee
// share an SCC: a singleton SCC with a self-edge is the self-recursive
// case (`int f(){ return f(); }`); a multi-member SCC is the mutual-
// recursive case (`f→g→f`). Refusing the whole cycle generalizes the
// cycle-1/2 self-recursion check to mutual recursion, so the optimizer
// never unrolls an unbounded recursion at inline time.
//
// AGNOSTIC: takes `Mir const&` ONLY — no TargetSchema / object-format /
// source-language input. The call graph is built SELF-CONTAINED from the
// module's own function symbols + each `Call`'s direct `GlobalAddr`
// callee; an indirect call (callee operand not a `GlobalAddr` to a
// defined function) contributes NO edge (it can't be inlined anyway, so
// it can't drive an inline-time recursion). Node ids are `MirFuncId.v`.
//
// Long-term note: this is a Tarjan SCC over the direct call graph — the
// standard, linear-time recursion-detection primitive. It is INTRA-
// module (the merged whole-program MIR is one module by the time the
// inliner runs — see `D-OPT7-CROSSCU-MIR-MERGE`), so a cross-CU recursive
// cycle is already a single-module cycle here.

#include "core/export.hpp"
#include "mir/mir.hpp"

#include <cstdint>
#include <unordered_map>

namespace dss::opt::analysis {

// Map every DEFINED function in `mir` to the id of the SCC it belongs to.
// Key = `MirFuncId.v`; value = an opaque SCC id (`std::uint32_t`). Two
// functions share a value IFF they are in the same strongly-connected
// component of the direct call graph. A function in NO cycle is its own
// singleton SCC (a unique value). A function with a SELF-edge (it calls
// itself directly) is a singleton SCC whose membership the inliner still
// treats as recursive — because the inliner's test is
// `funcToScc.at(caller.v) == funcToScc.at(callee.v)`, and for a self-call
// caller==callee, so that equality holds (singleton self-loop ⇒ refused).
//
// SCC id values are dense + deterministic (assigned in Tarjan completion
// order) but otherwise opaque: callers MUST compare ids for equality, not
// interpret their magnitude. Only the EQUALITY relation is contractual.
//
// Aborts loud (via `Mir`'s arena accessors) on any malformed id; the
// module's own functions/blocks/insts are always well-formed here.
[[nodiscard]] DSS_EXPORT std::unordered_map<std::uint32_t, std::uint32_t>
computeCallGraphSccs(Mir const& mir);

} // namespace dss::opt::analysis
