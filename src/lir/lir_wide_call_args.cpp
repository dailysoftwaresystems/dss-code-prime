#include "lir/lir_wide_call_args.hpp"

#include "core/types/call_payload.hpp"
// The by-value-stack-arg exhaust-class constants (kByValueStackArgExhaust*)
// live at the MIR boundary alongside the op whose payload carries them —
// same include lir_callconv uses for them.
#include "mir/mir_opcode.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_callconv.hpp"
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

// The outgoing-arg slot QUANTUM — the unit the overflow area is reserved and
// aligned in. Every current ABI (SysV/Win64/AAPCS64/Apple) uses a pointer-width
// (= GPR width = 8) quantum; lir_callconv derives it from widthForClass(GPR).
// ⚠ It is no longer the same thing as "the space one stacked argument takes":
// D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED made that a per-CC,
// per-axis rule that `StackArgCursor` owns. This value is what the cursor rounds
// TO, not what it advances BY.
constexpr std::uint32_t kOutgoingSlotBytes = 8u;

// Rewrite ONE function into builder `b`. Splits each Call's scalar overflow
// args into `store_outgoing_arg` carriers emitted before the (shrunken) call.
[[nodiscard]] bool
lowerOneFunc(Lir const& src, LirFuncId fn, TargetSchema const& schema,
             TargetCallingConvention const& cc, std::uint16_t storeOutgoingOp,
             LirBuilder& b, DiagnosticReporter& reporter) {

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
                // D-LIR-PER-INST-REG-CONSTRAINTS: `emitTerminator` is the last
                // thing this arm appends, so `lastInst()` IS the rebuilt
                // terminator (the emit succeeded — the arm above returned
                // otherwise).
                lir_pass_util::carryInstSideData(src, inst, b);
                continue;
            }

            if (info == nullptr || !info->isCall) {
                LirInstId const dstInst =
                    b.addInst(op, result, newOps, payload, flags);
                lir_pass_util::carryInstSideData(src, inst, b, dstInst);
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
            // D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: the carrier
            // now states a BYTE OFFSET (was a slot INDEX) plus the store's access
            // WIDTH. Under every `Slot`-packing CC the offset is `idx*slot` and
            // the width is 0 (= 64-bit), so the emitted carriers — and the frame
            // stores callconv materializes from them — are byte-identical.
            struct OutStore {
                LirReg        value;
                std::uint32_t byteOffset;
                std::uint8_t  widthFlags;
            };
            std::vector<OutStore> stores;

            // Preserve ops[0] (callee: SymbolRef direct / Reg indirect) and,
            // when present, ops[1] (the sret pointer routed to x8) — NEVER
            // touched (FC4-c2 indirect callee + FC7-C3 sret preserved).
            for (std::size_t k = 0; k < firstArgIdx && k < ops.size(); ++k)
                keepOps.push_back(ops[k]);

            ArgCursors argCursors{schema, cc};
            // ★ THE SAME OBJECT the three other stacked-arg walks use, so the
            // placement rule is stated ONCE (D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED).
            StackArgCursor stackCursor{cc, kOutgoingSlotBytes};
            // The cursor lir_callconv will independently run over the SHRUNKEN call
            // — carriers only, because the scalars are removed here. Kept beside the
            // real one so "the two passes agree" is CHECKED rather than assumed; see
            // the refusal below.
            StackArgCursor carrierOnlyCursor{cc, kOutgoingSlotBytes};
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
                    // ⚠⚠ D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES
                    // — A LIVE SILENT MISCOMPILE, PRE-EXISTING, AND THIS IS THE
                    // MISSING HALF OF A REFUSAL THAT ALREADY EXISTS ON THE OTHER
                    // SIDE. This pass removes scalar overflow args from the Call and
                    // places them from ITS cursor; the CARRIERS stay on the Call and
                    // lir_callconv places them from a cursor of its own, which
                    // necessarily restarts at 0 because the scalars are no longer
                    // there to advance it. So the moment a stacked SCALAR precedes a
                    // stacked AGGREGATE in one call, the two passes hand out the SAME
                    // bytes twice.
                    // ✔MEASURED 2026-08-24 at cycle P31 on
                    // `arm64:elf64-aarch64-linux-exec`, calling a function POINTER of
                    // type `void(*)(int×8, int, struct{int,int,int})` (a direct call
                    // cannot reach it — HIR→MIR already refuses the CALLEE of this
                    // shape with D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS,
                    // which is exactly the refusal this one mirrors):
                    //     stur x20, [sp]        <- the scalar `9` at outgoing +0
                    //     ldur x0,  [x19]
                    //     stur x0,  [sp]        <- the aggregate's first eightbyte,
                    //                              SAME BYTES, scalar destroyed
                    //     stur x0,  [sp, #8]
                    // No diagnostic, both shipped pipelines. Refuse rather than emit
                    // it: the real repair is ONE pass owning ALL overflow placement
                    // (scalars AND carriers), which is a design change with its own
                    // row, not something to smuggle in behind a stacked-arg cycle.
                    if (stackCursor.bytes() != carrierOnlyCursor.bytes()) {
                        report(reporter,
                               DiagnosticCode::L_UnsupportedLoweringForOpcode,
                               DiagnosticSeverity::Error,
                               std::format(
                                   "lowerWideCallArgs: call inst {} passes a "
                                   "by-value aggregate on the stack AFTER a scalar "
                                   "argument that also overflowed onto the stack. "
                                   "The scalar's placement is decided here and the "
                                   "aggregate's in lir_callconv, from cursors that "
                                   "cannot see each other ({} vs {} bytes consumed) "
                                   "— the two would be written to overlapping "
                                   "outgoing-argument bytes. Refusing rather than "
                                   "emitting the overlap",
                                   inst.v, stackCursor.bytes(),
                                   carrierOnlyCursor.bytes()));
                        return false;
                    }
                    (void)carrierOnlyCursor.placeNamedAggregate(aggBytes);
                    (void)stackCursor.placeNamedAggregate(aggBytes);
                    std::uint8_t const ex = ops[k + 1].byValueAggExhaust;
                    if (ex == kByValueStackArgExhaustGpr)
                        argCursors.exhaust(LirRegClass::GPR);
                    else if (ex == kByValueStackArgExhaustFpr)
                        argCursors.exhaust(LirRegClass::FPR);
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
                // ★ THE SAME OBJECT callconv's placement loop walks, so
                // "advance the shared cursors exactly as callconv does" is no
                // longer a promise this file keeps by hand
                // (D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR).
                // ⚠ A class with no pool row gets index 0 / poolSize 0, i.e.
                // the OVERFLOW arm — which routes it to a `store_outgoing_arg`
                // carrier and leaves the loud refusal to the placement site.
                auto const slot = argCursors.next(cls);
                std::uint32_t const argIndex = slot.has_value() ? slot->index : 0u;
                std::uint32_t const poolSize = slot.has_value() ? slot->poolSize : 0u;
                bool const forceStack =
                    variadicForcesStack && argRegionIdx >= fixedOps;
                if (argIndex < poolSize && !forceStack) {
                    keepOps.push_back(argOp);      // register-resident
                } else {
                    // D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: a
                    // VARARG and a NAMED scalar are DIFFERENT axes — ✔MEASURED,
                    // Apple packs named scalars naturally and keeps 8-byte slots
                    // for varargs — so the region decides which rule applies. The
                    // vararg predicate is the SAME `argRegionIdx >= fixedOps` the
                    // placement + pre-scan use, so all three agree on the boundary.
                    bool const isVariadicRegion =
                        ::dss::call_payload::isVariadic(payload)
                        && argRegionIdx >= fixedOps;
                    std::uint32_t const nat = argOp.argNaturalBytes;
                    // ⚠ FAIL LOUD ON A DROPPED CARRIER. Under NATURAL packing an
                    // unstated natural size would silently revert THIS argument to
                    // slot padding while the callee reads it packed — a boundary
                    // miscompile with no diagnostic. Refuse instead.
                    if (nat == 0
                        && (isVariadicRegion
                                ? cc.stackArgPacking.variadic
                                : cc.stackArgPacking.namedScalars)
                               == StackArgPacking::Natural) {
                        report(reporter,
                               DiagnosticCode::L_UnsupportedLoweringForOpcode,
                               DiagnosticSeverity::Error,
                               std::format(
                                   "lowerWideCallArgs: call inst {} arg {} "
                                   "overflows onto the stack under a calling "
                                   "convention that packs stacked arguments "
                                   "naturally, but its operand states no natural "
                                   "byte size — the MIR->LIR arg-size carrier was "
                                   "dropped, and padding it to a whole slot here "
                                   "would place it where the callee does not read "
                                   "it", inst.v, argRegionIdx));
                        return false;
                    }
                    auto const p = isVariadicRegion
                                       ? stackCursor.placeVariadic(nat)
                                       : stackCursor.placeNamedScalar(nat);
                    stores.push_back({argOp.reg, p.byteOffset, p.widthFlags});
                }
                ++argRegionIdx;
            }

            // Emit the stores first (each value vreg's live range ends here),
            // then the shrunken Call. store_outgoing_arg is hasSideEffects, so
            // no later pass reorders it off its call.
            for (auto const& s : stores) {
                std::array<LirOperand, 1> so{LirOperand::makeReg(s.value)};
                b.addInst(storeOutgoingOp, InvalidLirReg, so, s.byteOffset,
                          s.widthFlags);
            }
            // D-LIR-PER-INST-REG-CONSTRAINTS: 1 → N `store_outgoing_arg`
            // carriers + 1 shrunken Call. The side data belongs to the CALL —
            // the carriers are this pass's own outgoing-arg plumbing.
            LirInstId const dstCall =
                b.addInst(op, result, keepOps, payload, flags);
            lir_pass_util::carryInstSideData(src, inst, b, dstCall);
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
    // Carry every module side structure across the rebuild in one call — the
    // wide-literal pool (LiteralIndex operands reference it by index) and the
    // register-constraint pool. Same discipline as rewrite/callconv; the
    // per-instruction handles into the second pool ride `carryInstSideData`
    // inside `lowerOneFunc`.
    lir_pass_util::copyModuleSideStructures(src, b);
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
