#include "lir/lir_wide_call_args.hpp"

#include "core/types/call_payload.hpp"
// The by-value-stack-arg exhaust-class constants (kByValueStackArgExhaust*)
// live at the MIR boundary alongside the op whose payload carries them —
// same include lir_callconv uses for them.
#include "mir/mir_opcode.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <unordered_map>
#include <vector>

namespace dss {

namespace {

using dss::report;

// PROTOTYPE scope note: this pass materializes SCALAR overflow args only.
// A ByValueStackAgg carrier (a Reg immediately followed by a
// ByValueStackAgg marker) is ALREADY forced wholly to the stack by
// lir_callconv's placement loop and is NOT a register operand at regalloc
// pressure — so it is left on the Call untouched and its slot span advances
// the shared overflow cursor exactly as callconv does, keeping the scalar
// overflow indices this pass assigns consistent with the carriers callconv
// still places. (A production version folds ALL overflow placement — scalars
// here + carriers — into this single pre-regalloc site; see the design.)

// The outgoing-arg slot quantum. Every current ABI (SysV/Win64/AAPCS64/
// Apple) uses a pointer-width (= GPR width = 8) stack slot per stack-passed
// arg; lir_callconv derives it from widthForClass(GPR). The prototype only
// needs it for the by-value-agg carrier span; scalar args are one slot each.
constexpr std::uint32_t kOutgoingSlotBytes = 8u;

// Rewrite ONE function into builder `b`. Splits each Call's scalar overflow
// args into `store_outgoing_arg` carriers emitted before the (shrunken) call.
[[nodiscard]] bool
lowerOneFunc(Lir const& src, LirFuncId fn, TargetSchema const& schema,
             TargetCallingConvention const& cc, std::uint16_t storeOutgoingOp,
             LirBuilder& b, DiagnosticReporter& reporter) {
    std::uint32_t const gprPoolSize =
        static_cast<std::uint32_t>(cc.argGprs.size());
    std::uint32_t const fprPoolSize =
        static_cast<std::uint32_t>(cc.argFprs.size());
    std::uint32_t const slotAlignedPoolSize = std::max(gprPoolSize, fprPoolSize);

    auto const& funcInfo = src.funcArena().at(fn);
    b.addFunction(SymbolId{funcInfo.symbol});

    std::uint32_t const blockCount = src.funcBlockCount(fn);
    std::unordered_map<std::uint32_t, LirBlockId> srcToDst;
    srcToDst.reserve(blockCount);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi)
        srcToDst[src.funcBlockAt(fn, bi).v] = b.createBlock();

    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlock = src.funcBlockAt(fn, bi);
        b.beginBlock(srcToDst.at(srcBlock.v));

        std::uint32_t const instN = src.blockInstCount(srcBlock);
        for (std::uint32_t i = 0; i < instN; ++i) {
            LirInstId const inst = src.blockInstAt(srcBlock, i);
            std::uint16_t const op = src.instOpcode(inst);
            auto const ops = src.instOperands(inst);
            LirReg const result = src.instResult(inst);
            std::uint32_t const payload = src.instPayload(inst);
            std::uint8_t const flags = src.instFlags(inst);
            auto const* info = schema.opcodeInfo(op);
            bool const isTerm = (info != nullptr && info->isTerminator());

            // Operands pass through verbatim (BlockRefs remapped by the shared
            // helper). Vregs are untouched — this pass runs pre-regalloc and
            // only splits a Call; every value stays a vreg.
            std::vector<LirOperand> newOps;
            newOps.reserve(ops.size());
            for (auto const& o : ops)
                newOps.push_back(lir_pass_util::remapBlockRef(o, srcToDst));

            if (isTerm) {
                auto const succs = src.blockSuccessors(srcBlock);
                if (!lir_pass_util::emitTerminator(b, op, info, succs, newOps,
                                                   payload, flags, srcToDst,
                                                   "widecall", reporter))
                    return false;
                continue;
            }

            if (info == nullptr || !info->isCall) {
                b.addInst(op, result, newOps, payload, flags);
                continue;
            }

            // ── A (non-terminator) Call. Walk its args exactly like
            //    lir_callconv's placement loop, splitting scalar overflow args
            //    into store_outgoing_arg carriers.
            bool const hasIrr = ::dss::call_payload::hasIndirectResult(payload);
            std::size_t const firstArgIdx = hasIrr ? 2u : 1u;
            bool const variadicForcesStack =
                cc.variadicArgsAlwaysStack
                && ::dss::call_payload::isVariadic(payload);
            std::uint32_t const fixedOps =
                ::dss::call_payload::fixedOperandCount(payload);

            std::vector<LirOperand> keepOps;
            keepOps.reserve(ops.size());
            struct OutStore { LirReg value; std::uint32_t slot; };
            std::vector<OutStore> stores;

            // Preserve ops[0] (callee: SymbolRef direct / Reg indirect) and,
            // when present, ops[1] (the sret pointer routed to x8) — NEVER
            // touched (FC4-c2 indirect callee + FC7-C3 sret preserved).
            for (std::size_t k = 0; k < firstArgIdx && k < ops.size(); ++k)
                keepOps.push_back(ops[k]);

            std::uint32_t gprIdx = 0, fprIdx = 0, slotIdx = 0;
            std::uint32_t overflowIdx = 0;   // monotonic outgoing-slot cursor
            std::uint32_t argRegionIdx = 0;  // 0-based arg position
            for (std::size_t k = firstArgIdx; k < ops.size(); ++k) {
                LirOperand const& argOp = ops[k];
                if (argOp.kind == LirOperandKind::ByValueStackAgg) {
                    keepOps.push_back(argOp);   // kept with its carrier
                    continue;
                }
                bool const isByValCarrier =
                    argOp.kind == LirOperandKind::Reg
                    && (k + 1) < ops.size()
                    && ops[k + 1].kind == LirOperandKind::ByValueStackAgg;
                if (isByValCarrier) {
                    // Wholly-stacked aggregate: not a register operand at
                    // pressure — leave on the Call for callconv to byte-copy.
                    // Advance the shared cursors exactly as callconv does.
                    keepOps.push_back(argOp);
                    std::uint32_t const aggBytes = ops[k + 1].byValueAggBytes;
                    overflowIdx +=
                        (aggBytes + kOutgoingSlotBytes - 1u) / kOutgoingSlotBytes;
                    std::uint8_t const ex = ops[k + 1].byValueAggExhaust;
                    if (ex == kByValueStackArgExhaustGpr)
                        gprIdx = std::max(gprIdx, gprPoolSize);
                    else if (ex == kByValueStackArgExhaustFpr)
                        fprIdx = std::max(fprIdx, fprPoolSize);
                    ++argRegionIdx;
                    continue;
                }
                if (argOp.kind != LirOperandKind::Reg) {
                    // Non-Reg/non-marker scalar operand — a future isel bug the
                    // callconv gate reports loud; keep verbatim (don't split).
                    keepOps.push_back(argOp);
                    ++argRegionIdx;
                    continue;
                }
                LirRegClass const cls = argOp.reg.regClass();
                std::uint32_t argIndex, poolSize;
                if (cc.slotAligned) {
                    argIndex = slotIdx++;
                    poolSize = slotAlignedPoolSize;
                } else if (cls == LirRegClass::FPR) {
                    argIndex = fprIdx++;
                    poolSize = fprPoolSize;
                } else {
                    argIndex = gprIdx++;
                    poolSize = gprPoolSize;
                }
                bool const forceStack =
                    variadicForcesStack && argRegionIdx >= fixedOps;
                if (argIndex < poolSize && !forceStack) {
                    keepOps.push_back(argOp);      // register-resident
                } else {
                    stores.push_back({argOp.reg, overflowIdx});  // overflow
                    ++overflowIdx;
                }
                ++argRegionIdx;
            }

            // Emit the stores first (each value vreg's live range ends here),
            // then the shrunken Call. store_outgoing_arg is hasSideEffects, so
            // no later pass reorders it off its call.
            for (auto const& s : stores) {
                std::array<LirOperand, 1> so{LirOperand::makeReg(s.value)};
                b.addInst(storeOutgoingOp, InvalidLirReg, so, s.slot, /*flags=*/0);
            }
            b.addInst(op, result, keepOps, payload, flags);
        }
    }
    return true;
}

} // namespace

LirWideCallResult
lowerWideCallArgs(Lir const& src, TargetSchema const& schema,
                  std::uint16_t callingConventionIndex,
                  DiagnosticReporter& reporter) {
    LirWideCallResult out;
    auto const* cc = schema.callingConvention(callingConventionIndex);
    if (cc == nullptr) {
        report(reporter, DiagnosticCode::R_CallingConventionLookupFailed,
               DiagnosticSeverity::Error,
               std::format("lowerWideCallArgs: invalid cc index {}",
                           static_cast<unsigned>(callingConventionIndex)));
        return out;
    }
    auto const storeOutgoingOp = schema.opcodeByMnemonic("store_outgoing_arg");
    if (!storeOutgoingOp.has_value()) {
        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
               DiagnosticSeverity::Error,
               "lowerWideCallArgs: target schema missing 'store_outgoing_arg' "
               "opcode required for wide-call arg materialization");
        return out;
    }

    LirBuilder b{schema};
    // Carry the wide-literal pool across the rebuild (LiteralIndex operands
    // reference it by index) — same discipline as rewrite/callconv.
    lir_pass_util::copyLiteralPool(src, b);
    std::size_t const funcCount = src.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < funcCount; ++fi) {
        LirFuncId const fn = src.funcAt(fi);
        if (!lowerOneFunc(src, fn, schema, *cc, *storeOutgoingOp, b, reporter))
            return out;
    }
    out.lir = std::move(b).finish();
    out.ok = out.lir.moduleFuncCount() == funcCount;
    return out;
}

} // namespace dss
