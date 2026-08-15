#pragma once

#include "core/export.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <cstdint>
#include <vector>

// ── the per-INSTRUCTION forbidden-register question, asked in ONE place ──
//
// D-LIR-PER-INST-REG-CONSTRAINTS. Fixed-register semantics reach the register
// allocator through TWO carriers, and every consumer needs BOTH:
//
//   * `TargetOpcodeInfo::implicitRegisters` — the per-OPCODE contract, written
//     by the target JSON loader (x86 `idiv` ties RDX:RAX; shift-by-CL needs its
//     count in CL). One row per opcode, so it cannot express a constraint that
//     differs between two instructions sharing that opcode.
//   * `Lir::instRegConstraints` — the per-INSTRUCTION contract, written by a
//     lowering. Two `asm` statements share one opcode and declare DIFFERENT
//     clobbers, which is exactly the shape the opcode row cannot hold.
//
// ★★★ WHY THIS FUNCTION EXISTS AT ALL, AND IT IS NOT DE-DUPLICATION FOR ITS OWN
// SAKE. ✔MEASURED 2026-08-15: the `inputs ∪ clobbered` union was hand-rolled at
// THREE sites — `collectImplicitClobberPositions` and the `requires2Address`
// result exclusion (both `lir_regalloc.cpp`) and the spill-scratch forbid
// (`lir_rewrite.cpp`) — and ALL THREE read ONLY the opcode carrier. So the
// per-instruction handle was threaded faithfully through all four rebuild
// passes, verified by `verifyLirRebuild`, round-tripped through `.dsslir`… and
// then IGNORED by the only consumer that can act on it. A constraint that is
// carried and not consulted is indistinguishable, from every structural test,
// from a constraint that is honoured — and it miscompiles silently: the
// allocator hands out a register the instruction destroys.
//
// One accessor, three callers. A FOURTH consumer that asks the same question
// gets the same answer by construction, and a fifth CARRIER (should one ever be
// added) is wired here once rather than found by grep three times.
//
// ★ WHY IT LIVES IN ITS OWN TU RATHER THAN IN `lir_pass_util`. That header is
// scoped, in its own words, to the "shared substrate for LIR transformation
// PASSES … every pass that walks an input `Lir` and builds a fresh one" —
// rebuild plumbing. `lir_regalloc` is not a rebuild pass and builds no module;
// it is a consumer of the constraint, as is the rewriter's scratch selection.
// This is a QUERY over a frozen module, not rebuild plumbing, and it is on the
// allocator's hot path where `lir_pass_util`'s builder-side dependencies buy
// nothing. (`lir_pass_util::incomingArgRegister` is the precedent for the
// SHAPE — one formula, shared by the two consumers that ask the same question,
// coupled so they cannot drift.)
//
// Agnostic by construction: reads the schema's declaration and the module's
// pool, and holds no opinion about source language, CPU or object format. No
// register names, no mnemonics, no `if (arch == …)`.

namespace dss {

// Append this instruction's EFFECTIVE forbidden-register ordinals —
// `(inputs ∪ clobbered)` over the per-OPCODE carrier and the per-INSTRUCTION
// carrier — to `out`, skipping any ordinal `out` already holds.
//
// ⚠ OUTPUTS ARE DELIBERATELY NOT FORBIDDEN, and the omission is load-bearing
// rather than an oversight: the instruction reads its operands BEFORE writing
// its outputs, so an operand sharing a register with an output is fine. The
// omission is SAFE only because `outputs ⊆ clobbered` is enforced — by the
// loader for the per-opcode carrier (`target_schema_json.cpp`). A producer
// filling the per-INSTRUCTION carrier owes the same discipline: every
// register-pinned OUTPUT must also enter that instruction's `clobberedNames`.
//
// Dedup is against the WHOLE of `out`, not just this call's additions, so a
// caller that has already staged unrelated exclusions (the allocator's
// operand[1..N] slice) can append into the same buffer without duplicates.
//
// ⚠ Aborts on a DANGLING per-instruction handle — `Lir::instRegConstraints`'s
// documented contract, and the state `verifyLirSideStructures` reports as a
// diagnostic first.
DSS_EXPORT void
appendEffectiveForbiddenOrdinals(Lir const& lir, TargetSchema const& schema,
                                 LirInstId inst,
                                 std::vector<std::uint16_t>& out);

// Same union, into a fresh vector. Empty iff the instruction declares no
// implicit registers on EITHER carrier.
[[nodiscard]] DSS_EXPORT std::vector<std::uint16_t>
effectiveForbiddenOrdinals(Lir const& lir, TargetSchema const& schema,
                           LirInstId inst);

} // namespace dss
