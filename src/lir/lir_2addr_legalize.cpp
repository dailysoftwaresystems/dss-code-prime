#include "lir/lir_2addr_legalize.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

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

} // namespace

LirTwoAddrLegalizeResult
legalizeTwoAddress(Lir const&          src,
                   TargetSchema const& schema,
                   DiagnosticReporter& reporter) {
    LirTwoAddrLegalizeResult result;
    if (src.moduleFuncCount() == 0) {
        return result;
    }
    result.expectedFuncCount = src.moduleFuncCount();
    result.allFunctionsLegalized = true;

    PassState state{schema};
    LirBuilder b{schema};
    // D-CSUBSET-BITFIELD-WIDE-UNIT: carry the wide-literal pool across
    // the rebuild — `LiteralIndex` operands copied below reference it by
    // index (e.g. the `mov r64, imm64` carrier for a 64-bit constant).
    lir_pass_util::copyLiteralPool(src, b);

    std::size_t const funcCount = src.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < funcCount; ++fi) {
        LirFuncId const srcFn = src.funcAt(fi);
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
                // (operand 0 must be a register so a `mov` can copy
                // it to the destination). Convergence-fix E: emit a
                // hard diagnostic if a schema declares
                // `requires2Address: true` on an opcode whose first
                // operand isn't a Reg — silently skipping legalize
                // would let the assembler emit the wrong-shape bytes.
                if (info != nullptr && info->requires2Address) {
                    if (newOps.empty()
                        || newOps[0].kind != LirOperandKind::Reg) {
                        report(reporter,
                               DiagnosticCode::L_UnsupportedLoweringForOpcode,
                               DiagnosticSeverity::Error,
                               std::format("2-address legalize: opcode "
                                           "'{}' declares "
                                           "`requires2Address: true` "
                                           "but its first operand is "
                                           "not a register — legalize "
                                           "cannot synthesize the "
                                           "implicit copy",
                                           info->mnemonic));
                        result.allFunctionsLegalized = false;
                    }
                }

                bool const needsLegalize =
                    info != nullptr && info->requires2Address
                    && !newOps.empty()
                    && newOps[0].kind == LirOperandKind::Reg
                    && result_reg.valid()
                    && newOps[0].reg != result_reg;

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
                        result.allFunctionsLegalized = false;
                    } else {
                        // Emit `mov result, operands[0]` BEFORE the
                        // original inst. The mov's source is the
                        // SAME `LirReg` operand that the binary op
                        // would have read; the dest is the binary
                        // op's result vreg/preg.
                        LirOperand const movSrc[] = { newOps[0] };
                        (void)b.addInst(*movOp, result_reg, movSrc);
                        // Rewrite the original inst's operands[0] to
                        // point at the destination — the binary op
                        // now reads from its own result reg (the
                        // 2-address constraint).
                        newOps[0] = LirOperand::makeReg(result_reg);
                    }
                }

                // Emit the (possibly legalized) instruction. Mirror
                // the terminator-dispatch from `lir_pass_util` so
                // br/cond-br/return/unreachable route correctly.
                if (info != nullptr && info->isTerminator()) {
                    std::vector<LirBlockId> succs;
                    auto const srcSuccs = src.blockSuccessors(srcBlk);
                    succs.reserve(srcSuccs.size());
                    for (auto const& s : srcSuccs) {
                        auto it = srcToDst.find(s.v);
                        if (it != srcToDst.end()) succs.push_back(it->second);
                    }
                    (void)lir_pass_util::emitTerminator(
                        b, opcode, info, succs, newOps,
                        src.instPayload(inst), src.instFlags(inst),
                        srcToDst, "2-address-legalize", reporter);
                } else {
                    (void)b.addInst(opcode, result_reg, newOps,
                                    src.instPayload(inst),
                                    src.instFlags(inst));
                }
            }
        }
    }

    result.lir = std::move(b).finish();
    return result;
}

} // namespace dss
