#include "lir/lir_2addr_legalize.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_rewrite.hpp"

#include <array>
#include <format>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dss {

namespace {

using dss::report;

// Per-pass shared cache for "the register-copy opcode in this schema",
// resolved PER REGISTER CLASS (FC2 Part B — the registerClassOps
// table). The implicit 2-address copy must use the destination's
// class-correct move (x86_64: GPR `mov`, FPR `movaps`) — a GPR mov
// against an FPR ordinal assembles to valid-looking-but-wrong bytes.
struct PassState {
    TargetSchema const& schema;
    // One lazily-resolved cell per LirRegClass ordinal; the inner
    // optional<optional> distinguishes "not yet looked up" from
    // "looked up, class has no move".
    std::array<std::optional<std::optional<std::uint16_t>>, 5> movByClass{};

    [[nodiscard]] std::optional<std::uint16_t>
    resolveMov(LirRegClass cls, DiagnosticReporter& reporter) {
        auto const c = static_cast<std::size_t>(cls);
        if (c >= movByClass.size()) return std::nullopt;
        if (!movByClass[c].has_value()) {
            movByClass[c] = schema.regClassOpOpcode(
                static_cast<TargetRegClass>(c), RegClassOp::Move);
            if (!movByClass[c]->has_value()) {
                report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                       DiagnosticSeverity::Error,
                       std::format("2-address legalize: target schema '{}' "
                                   "declares no 'move' operation for "
                                   "register class '{}' (registerClassOps) "
                                   "— cannot synthesize the implicit "
                                   "register copy; a GPR mov against this "
                                   "class would silently mis-encode",
                                   schema.name(),
                                   targetRegClassName(
                                       static_cast<TargetRegClass>(c))));
            }
        }
        return *movByClass[c];
    }
};

// Legalize ONE function into `b`.
//
// ★★★ TWO FAILURE CHANNELS, AND CONFLATING THEM IS THE DEFECT THIS SHAPE
// CLOSES (D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE).
//
//   * the SOFT channel — `legalized`, set false for a schema-config failure
//     (an opcode declaring `requires2Address` on a non-register first
//     operand; a register class with no `move`). The pass carries on and
//     still hands back a structurally well-formed module; the caller must
//     refuse to assemble it, which `ok()` makes it do.
//   * the HARD channel — the `bool` RETURN, false when the destination
//     builder has been left UNUSABLE. A terminator the shared dispatch
//     REFUSES appends nothing, so the open block ends without a terminator,
//     and from that point every further builder call is a process kill:
//     the next `beginBlock` fatals on "current block has no terminator",
//     `addFunction`/`finish()` fatal inside `closeFunction_` on "block
//     opened but never filled" / "block's last instruction is not a
//     terminator". A refusal that crashes is not a refusal, so the caller
//     must stop driving the builder — it cannot merely note the failure.
//
// Same two-channel split `rewriteWithAllocation` uses (`anyFunctionFailed`
// beside `rewriteOneFunc`'s bail), and the same reason.
[[nodiscard]] bool
legalizeOneFunc(Lir const& src, LirFuncId srcFn, TargetSchema const& schema,
                PassState& state, LirBuilder& b, bool& legalized,
                DiagnosticReporter& reporter) {
    (void)b.addFunction(src.funcSymbol(srcFn));

    // Map src block-id → dst block-id so BlockRef operands on
    // terminators can be rewritten correctly.
    std::unordered_map<std::uint32_t, LirBlockId> srcToDst;
    std::uint32_t const blockCount = src.funcBlockCount(srcFn);
    // First pass: pre-create destination blocks so forward
    // branches resolve.
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const dst = b.createBlock();
        srcToDst[src.funcBlockAt(srcFn, bi).v] = dst;
    }
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlk = src.funcBlockAt(srcFn, bi);
        LirBlockId const dstBlk = srcToDst[srcBlk.v];
        b.beginBlock(dstBlk);

        std::uint32_t const instCount = src.blockInstCount(srcBlk);
        for (std::uint32_t ii = 0; ii < instCount; ++ii) {
            LirInstId const inst = src.blockInstAt(srcBlk, ii);
            auto const opcode = src.instOpcode(inst);
            auto const* info  = schema.opcodeInfo(opcode);
            auto const result_reg = src.instResult(inst);
            auto const ops    = src.instOperands(inst);

            // Translate operands first — remap BlockRef from src
            // block ids to dst block ids; everything else passes
            // through (vregs / phys regs / imm / sym / mem all
            // unchanged at this pass).
            std::vector<LirOperand> newOps;
            newOps.reserve(ops.size());
            for (auto const& op : ops) {
                newOps.push_back(
                    lir_pass_util::remapBlockRef(op, srcToDst));
            }

            // `requires2Address` is DEFINED as the reg-reg shape
            // (the TIED operand must be a register so a `mov` can
            // copy it to the destination). Convergence-fix E: emit a
            // hard diagnostic if a schema declares
            // `requires2Address: true` on an opcode whose tied
            // operand isn't a Reg — silently skipping legalize
            // would let the assembler emit the wrong-shape bytes.
            //
            // ★ THE TIED OPERAND IS WHICHEVER ONE THE SCHEMA NAMES
            // (D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE). It was the literal
            // `0` here and at the two sites below; `requires2Address`
            // now carries the index, so a `(result, operand j)` tie is
            // expressible and every two-address target — not just an
            // asm-private path — can declare one.
            std::size_t const tied =
                (info != nullptr && info->requires2Address.has_value())
                    ? *info->requires2Address : 0u;
            if (info != nullptr && info->requires2Address.has_value()) {
                if (newOps.size() <= tied
                    || newOps[tied].kind != LirOperandKind::Reg) {
                    report(reporter,
                           DiagnosticCode::L_UnsupportedLoweringForOpcode,
                           DiagnosticSeverity::Error,
                           std::format("2-address legalize: opcode "
                                       "'{}' ties its result to "
                                       "operand {}, but that operand is "
                                       "not a register (the instruction "
                                       "carries {}) — legalize "
                                       "cannot synthesize the "
                                       "implicit copy",
                                       info->mnemonic, tied,
                                       newOps.size()));
                    legalized = false;
                }
            }

            bool const needsLegalize =
                info != nullptr && info->requires2Address.has_value()
                && newOps.size() > tied
                && newOps[tied].kind == LirOperandKind::Reg
                && result_reg.valid()
                && newOps[tied].reg != result_reg;

            if (needsLegalize) {
                auto const movOp =
                    state.resolveMov(result_reg.regClass(), reporter);
                if (!movOp.has_value()) {
                    // Convergence-fix A: cannot synthesize the
                    // implicit `mov` (schema lacks `mov`). The
                    // legalize invariant `result == operands[0]`
                    // is NOT guaranteed for this function. Mark
                    // the pass result as failed so `ok()` reports
                    // false and downstream MUST NOT consume `lir`
                    // for assembly. We still emit the original
                    // instruction (preserving parallel-index
                    // discipline) so the consumer sees a
                    // well-formed shape on inspection.
                    legalized = false;
                } else {
                    // Emit `mov result, operands[tied]` BEFORE the
                    // original inst. The mov's source is the
                    // SAME `LirReg` operand that the binary op
                    // would have read; the dest is the binary
                    // op's result vreg/preg.
                    LirOperand const movSrc[] = { newOps[tied] };
                    (void)b.addInst(*movOp, result_reg, movSrc);
                    // Rewrite the original inst's tied operand to
                    // point at the destination — the binary op
                    // now reads from its own result reg (the
                    // 2-address constraint).
                    newOps[tied] = LirOperand::makeReg(result_reg);
                }
            }

            // Emit the (possibly legalized) instruction. Mirror
            // the terminator-dispatch from `lir_pass_util` so
            // br/cond-br/return/unreachable route correctly.
            //
            // D-LIR-PER-INST-REG-CONSTRAINTS: the source instruction's
            // per-instruction side data rides onto the instruction
            // emitted HERE — never onto the implicit copy this pass may
            // have prepended above. The mapping is 1 → (0 or 1 `mov`) +
            // 1, and the constraint set belongs to the OPERATION, not to
            // the copy that legalizes its operand shape.
            if (info != nullptr && info->isTerminator()) {
                std::vector<LirBlockId> succs;
                auto const srcSuccs = src.blockSuccessors(srcBlk);
                succs.reserve(srcSuccs.size());
                for (auto const& s : srcSuccs) {
                    auto it = srcToDst.find(s.v);
                    if (it != srcToDst.end()) succs.push_back(it->second);
                }
                // D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE: the refusal
                // is TERMINAL for this rebuild. The dispatch already
                // reported WHY (an unsupported `Switch`, a successor count
                // its declared kind forbids, an opcode with no schema
                // entry); what it cannot do is append anything, so the
                // block this pass opened stays unterminated and the builder
                // is now a loaded gun. Unwind instead of noting it.
                if (!lir_pass_util::emitTerminator(
                        b, opcode, info, succs, newOps,
                        src.instPayload(inst), src.instFlags(inst),
                        srcToDst, "2-address-legalize", reporter)) {
                    return false;
                }
                // Gated on the emit having SUCCEEDED: a refused
                // terminator appends nothing, and `lastInst()` would then
                // name the instruction BEFORE it.
                lir_pass_util::carryInstSideData(src, inst, b);
            } else {
                LirInstId const dstInst =
                    b.addInst(opcode, result_reg, newOps,
                              src.instPayload(inst),
                              src.instFlags(inst));
                lir_pass_util::carryInstSideData(src, inst, b, dstInst);
            }
        }
    }
    return true;
}

} // namespace

LirTwoAddrLegalizeResult
legalizeTwoAddress(Lir const&          src,
                   TargetSchema const& schema,
                   DiagnosticReporter& reporter) {
    LirTwoAddrLegalizeResult result;
    if (src.moduleFuncCount() == 0) {
        // Empty module (a declaration-only / all-preprocessed-out TU): nothing
        // to legalize. expectedFuncCount stays 0 (== moduleFuncCount()); mark
        // allFunctionsLegalized vacuously true so `ok()` reports SUCCESS — the
        // empty module lowers to a valid empty relocatable object rather than
        // silently rejecting the compile (D-CSUBSET-TESTTU-SILENT-EXIT1).
        result.allFunctionsLegalized = true;
        return result;
    }
    result.expectedFuncCount = src.moduleFuncCount();
    result.allFunctionsLegalized = true;

    PassState state{schema};
    LirBuilder b{schema};
    // Carry every module SIDE STRUCTURE across the rebuild in one call —
    // the wide-literal pool (D-CSUBSET-BITFIELD-WIDE-UNIT: `LiteralIndex`
    // operands copied below reference it by index, e.g. the `mov r64,
    // imm64` carrier for a 64-bit constant) and the per-instruction
    // register-constraint pool (D-LIR-PER-INST-REG-CONSTRAINTS). The
    // per-INSTRUCTION handle into that second pool is NOT carried by this
    // call — it cannot be, the rebuild re-creates the instructions — see
    // the `carryInstSideData` call at the bottom of the per-inst loop.
    lir_pass_util::copyModuleSideStructures(src, b);

    std::size_t const funcCount = src.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < funcCount; ++fi) {
        if (legalizeOneFunc(src, src.funcAt(fi), schema, state, b,
                            result.allFunctionsLegalized, reporter)) {
            continue;
        }
        // D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE. Mid-failure the
        // builder holds a half-open function whose current block never got
        // its terminator, so `finish()` is not merely wrong here — it is a
        // `std::abort()` inside `closeFunction_`, a process kill naming the
        // BUILDER from a pass that had reported nothing. Return WITHOUT
        // touching `b` again; `result.lir` stays the empty module and
        // `ok()` is false on BOTH of its clauses (0 != expectedFuncCount,
        // and the flag below), so no caller can consume this for assembly.
        // Byte-for-byte the unwind `rewriteWithAllocation` performs on a
        // failed `rewriteOneFunc`.
        result.allFunctionsLegalized = false;
        return result;
    }

    result.lir = std::move(b).finish();
    // ★ THE OTHER MISSING STAGE BOUNDARY
    // (D-LIR-PEEPHOLE-CALLCONV-IDENTITY-COPY-CLAIM-HAS-NO-INSTRUMENT).
    // Env-gated and zero-cost when unset. This pass SYNTHESIZES class moves,
    // so it is a producer of the population `lir_peephole` R1 consumes and it
    // has to be separable from the allocator's residue upstream of it. The
    // `post-rewrite`/`post-callconv` pair alone could not separate them.
    dumpLirFuncs(result.lir, schema, "post-legalize");
    return result;
}

} // namespace dss
