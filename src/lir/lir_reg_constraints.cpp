#include "lir/lir_reg_constraints.hpp"

namespace dss {

namespace {

void appendDedup(std::vector<std::uint16_t>& out, std::uint16_t ord) {
    for (std::uint16_t const e : out) {
        if (e == ord) return;
    }
    out.push_back(ord);
}

// `inputs ∪ clobbered` from ONE carrier, deduped against everything already in
// `out`. Both halves are required and the union is not symmetric guesswork —
// each half prevents a DIFFERENT silent miscompile, and both were shipped bugs:
//
//   * an operand parked on an implicit INPUT register is overwritten by the
//     input-pinning move the lowering emits BEFORE the instruction reads the
//     operand (x86 `mov rax, dividend` destroying the divisor: 100/100 = 1);
//   * an operand parked on an implicit CLOBBERED register is destroyed by the
//     instruction's own pre-emit (CQO writes RDX before IDIV reads operand 0).
//
// An ordinal legitimately appears in both sets (idiv's RDX is input high-half
// AND clobbered remainder), which is why the dedup is not optional bookkeeping:
// the exclusion buffers are scanned linearly by `tryAllocateExcluding` and
// `findSpillCandidate`, so duplicates are pure cost.
void appendConstraintSet(std::vector<std::uint16_t>&        out,
                         ImplicitRegisterConstraint const&  c) {
    out.reserve(out.size() + c.inputOrdinals.size()
                + c.clobberedOrdinals.size());
    for (std::uint16_t const o : c.inputOrdinals)     appendDedup(out, o);
    for (std::uint16_t const o : c.clobberedOrdinals) appendDedup(out, o);
}

} // namespace

void appendEffectiveForbiddenOrdinals(Lir const& lir,
                                      TargetSchema const& schema,
                                      LirInstId inst,
                                      std::vector<std::uint16_t>& out) {
    // Carrier 1 — the per-OPCODE contract the target JSON declares.
    if (auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
        info != nullptr && info->implicitRegisters.has_value()) {
        appendConstraintSet(out, *info->implicitRegisters);
    }
    // Carrier 2 — the per-INSTRUCTION contract a lowering declared. Additive,
    // never a replacement: an inline-asm statement lowered onto an opcode that
    // ALSO has fixed-register semantics is constrained by both, and neither
    // writer may quietly relax the other's declaration.
    if (auto const* perInst = lir.instRegConstraints(inst);
        perInst != nullptr) {
        appendConstraintSet(out, *perInst);
    }
}

std::vector<std::uint16_t>
effectiveForbiddenOrdinals(Lir const& lir, TargetSchema const& schema,
                           LirInstId inst) {
    std::vector<std::uint16_t> out;
    appendEffectiveForbiddenOrdinals(lir, schema, inst, out);
    return out;
}

} // namespace dss
