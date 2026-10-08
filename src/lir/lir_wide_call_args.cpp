#include "lir/lir_wide_call_args.hpp"

#include "core/types/call_payload.hpp"
// The by-value-stack-arg exhaust-class constants (kByValueStackArgExhaust*)
// live at the MIR boundary alongside the op whose payload carries them —
// same include lir_callconv uses for them.
#include "mir/mir_opcode.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_pass_util.hpp"
#include "lir/lir_reg.hpp"

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

// D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES: THIS PASS OWNS
// EVERY OUTGOING-ARGUMENT BYTE OFFSET, and it is the only tier that can.
//
// A stacked SCALAR is materialized here as a `store_outgoing_arg` carrier whose
// payload IS its byte offset. ★ So, since P69 round 4, is a stacked by-value
// AGGREGATE (D-AS-REGALLOC-WIDE-CALL-AGGREGATE-ADDRESS-OPERANDS): its bytes are
// COPIED here, before allocation, from the temp that holds them into its placed
// outgoing bytes — one GPR-wide `load` from [temp + k] into a fresh virtual
// register and one `store_outgoing_arg` carrier at placement + k per chunk — and
// the (address, ByValueStackAgg) carrier leaves the Call. ⚠⚠ This file used to
// keep that carrier ON the Call, "not a register operand at regalloc pressure",
// for `lir_callconv` to copy post-regalloc. Its ADDRESS is a register operand
// all the same: ✔MEASURED (round-4 dsscp, x86_64 ELF and Mach-O x86_64) a call
// passing eight ints and ten or eleven long doubles (each an x87 MEMORY-class
// aggregate on SysV) exhausted the rewriter's reload scratch at the call
// (`L_VirtualRegInPostRegalloc` — a spilled carrier address is scratch-reloaded,
// while a spilled register ARG is loaded straight into its ABI register by
// callconv), and one passing fourteen left callconv no free caller-saved GPR to
// copy through (`L_CcRegLookupFailed`); gcc and clang compile both. Copied before
// allocation, each address is read by single-operand loads the allocator places
// like any other, and the Call carries only what its ABI passes in registers.
// (The byte-copy needs no physical register: its temporaries are virtual.)
//
// ⚠⚠ THE ALTERNATIVE IS THE SILENT MISCOMPILE THIS ROW NAMES, and it shipped.
// While the aggregate's placement was re-derived in `lir_callconv`, that
// derivation ran over the list this pass had ALREADY SHORTENED — the scalars it
// removed were no longer there to advance a cursor — so the second cursor
// restarted inside bytes the first had handed out. One stacked scalar followed
// by one stacked aggregate was enough: `stur x20,[sp]` placed the scalar at
// outgoing +0 and the aggregate's first eightbyte was then written over the same
// bytes, destroying the argument before the call, on both shipped pipelines,
// with no diagnostic.
//
// ★ THE PROTOTYPE NOTE THIS REPLACED said a production version would fold ALL
// overflow placement into this single pre-regalloc site. That is what happened —
// and since P69 round 4 the aggregate's MATERIALIZATION is here too, so the one
// site that decides where an outgoing byte lands also writes it. `lir_callconv`
// still copies a carrier it is handed (a hand-built module's); the pipeline no
// longer hands it one.

// The outgoing-arg slot QUANTUM — the unit the overflow area is reserved and
// aligned in. Every current ABI (SysV/Win64/AAPCS64/Apple) uses a pointer-width
// (= GPR width = 8) quantum; lir_callconv derives it from widthForClass(GPR).
// ⚠ It is no longer the same thing as "the space one stacked argument takes":
// D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED made that a per-CC,
// per-axis rule that `StackArgCursor` owns. This value is what the cursor rounds
// TO, not what it advances BY. It is also the CHUNK a stacked aggregate is copied
// in: GPR-wide, the width callconv's post-regalloc copy used, over the same span
// (the placement reserves whole slots and the temp is rounded to 16, so the last
// chunk's read stays inside the temp and its write inside the aggregate's slots).
constexpr std::uint32_t kOutgoingSlotBytes = 8u;

// The access width of one aggregate-copy chunk, off the SAME number the copy
// steps by (D-LIR-CALLEE-SAVED-AND-SPILL-STORES-DEFAULT-TO-WIDTH-64-BELOW-THE-SLOT:
// a step and an access width taken from two places drift apart). A slot the LIR
// instruction model cannot state as a width fails the BUILD.
static_assert(lirInstWidthFlagForBits(kOutgoingSlotBytes * 8u).has_value(),
              "the outgoing slot must be a width a LIR memory access can state");
constexpr std::uint8_t kOutgoingChunkWidthFlags =
    lirInstWidthFlagForBits(kOutgoingSlotBytes * 8u).value();

// The highest VIRTUAL register id `fn` names — any instruction's result or Reg
// operand (a bundle's operands are its outer instruction's) — or 0. The rebuilt
// function copies these ids verbatim, so a register this pass mints must start
// above it (`LirBuilder::reserveVRegIdsThrough`).
[[nodiscard]] std::uint32_t highestVirtualRegId(Lir const& src, LirFuncId fn) {
    std::uint32_t highest = src.funcNumVRegs(fn);
    auto const note = [&](LirReg r) {
        if (r.valid() && r.isPhysical == 0 && r.id > highest) highest = r.id;
    };
    std::uint32_t const blockCount = src.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const blk = src.funcBlockAt(fn, bi);
        std::uint32_t const n = src.blockInstCount(blk);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = src.blockInstAt(blk, i);
            note(src.instResult(inst));
            for (auto const& o : src.instOperands(inst))
                if (o.kind == LirOperandKind::Reg) note(o.reg);
        }
    }
    return highest;
}

// Rewrite ONE function into builder `b`. Splits each Call's scalar overflow
// args into `store_outgoing_arg` carriers emitted before the (shrunken) call,
// and copies each stacked by-value aggregate into its outgoing bytes there too
// (GPR `load` `gprLoadOp` + `store_outgoing_arg` per chunk).
[[nodiscard]] bool
lowerOneFunc(Lir const& src, LirFuncId fn, TargetSchema const& schema,
             TargetCallingConvention const& cc, std::uint16_t storeOutgoingOp,
             std::uint16_t gprLoadOp, LirBuilder& b, DiagnosticReporter& reporter) {

    auto const& funcInfo = src.funcArena().at(fn);
    b.addFunction(SymbolId{funcInfo.symbol});
    // Every copied id is taken: the chunk temporaries mint above them.
    b.reserveVRegIdsThrough(highestVirtualRegId(src, fn));

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
            // P69 round 4: each stacked by-value aggregate this Call passes — the
            // temp's address, the aggregate's size and its placed byte offset —
            // copied into the outgoing area before the Call (see the file header).
            struct AggCopy {
                LirReg        address;
                std::uint32_t bytes;
                std::uint32_t byteOffset;
            };
            std::vector<AggCopy> aggCopies;

            // Preserve ops[0] (callee: SymbolRef direct / Reg indirect) and,
            // when present, ops[1] (the sret pointer routed to x8) — NEVER
            // touched (FC4-c2 indirect callee + FC7-C3 sret preserved).
            for (std::size_t k = 0; k < firstArgIdx && k < ops.size(); ++k)
                keepOps.push_back(ops[k]);

            ArgCursors argCursors{schema, cc};
            // ★★★ THE ONE CURSOR OVER THIS CALL'S OUTGOING-ARGUMENT AREA.
            // Scalars AND aggregates are placed from it, in source order, and
            // nothing downstream places anything
            // (D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES); the
            // packing rule it applies is the shipped CC's
            // (D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED).
            StackArgCursor stackCursor{cc, kOutgoingSlotBytes};
            std::uint32_t argRegionIdx = 0;  // 0-based arg position
            // D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES: the
            // vararg boundary is a POSITION, and this pass RENUMBERS positions.
            // `fixedOperandCount` counts arg positions produced by the callee's
            // FIXED parameters; every walk downstream tests `argRegionIdx >=
            // fixedOperandCount` against the SHRUNKEN list, so the boundary has to
            // be restated in the shrunken list's own numbering or those walks read
            // a vararg as a named argument. `keptFixedArgs` is that restatement:
            // the kept positions are an order-preserving subsequence, so the first
            // `keptFixedArgs` of them are exactly the ones the fixed region
            // produced.
            std::uint32_t keptFixedArgs = 0;
            for (std::size_t k = firstArgIdx; k < ops.size(); ++k) {
                LirOperand const& argOp = ops[k];
                if (lirIsByValueStackAggDescriptor(ops, k)) {
                    // A descriptor is consumed WITH its carrier by the arm
                    // below, which emits the whole triple and steps past it — so
                    // reaching one HERE means it has no carrier in front of it.
                    // Passing it through would leave a size marker describing
                    // nothing; dropping it would lose an argument silently.
                    report(reporter,
                           DiagnosticCode::L_UnsupportedLoweringForOpcode,
                           DiagnosticSeverity::Error,
                           std::format(
                               "lowerWideCallArgs: call inst {} carries a "
                               "by-value stacked-aggregate descriptor at operand "
                               "{} with no address register in front of it — the "
                               "carrier is a (Reg, ByValueStackAgg[, MemOffset]) "
                               "group and this one is malformed", inst.v, k));
                    return false;
                }
                // One kept ARG POSITION, recorded before any `continue`. Only
                // positions the FIXED region produced move the vararg boundary,
                // and `argRegionIdx` is still the ORIGINAL numbering at every
                // call site below — which is what makes the restatement exact.
                auto const noteKeptPosition = [&] {
                    if (argRegionIdx < fixedOps) ++keptFixedArgs;
                };
                if (lirIsByValueStackAggCarrier(ops, k)) {
                    // Wholly-stacked aggregate: its byte OFFSET is decided here,
                    // from the one cursor, and its bytes are COPIED there before the
                    // Call (`aggCopies`, emitted below) — so the carrier, whose
                    // address is a register operand the allocator would otherwise
                    // have to keep live INTO the Call, leaves it (P69 round 4,
                    // D-AS-REGALLOC-WIDE-CALL-AGGREGATE-ADDRESS-OPERANDS; see the
                    // file header).
                    std::uint32_t const aggBytes = ops[k + 1].byValueAggBytes;
                    if (lirByValueStackAggPlacedOffset(ops, k).has_value()) {
                        report(reporter,
                               DiagnosticCode::L_UnsupportedLoweringForOpcode,
                               DiagnosticSeverity::Error,
                               std::format(
                                   "lowerWideCallArgs: call inst {} arg {} already "
                                   "states an outgoing-argument placement — this "
                                   "pass is the one that assigns it, so an input "
                                   "that carries one has been through here before "
                                   "and a second placement would be taken from a "
                                   "cursor that restarts at zero", inst.v,
                                   argRegionIdx));
                        return false;
                    }
                    // D-CSUBSET-LONG-DOUBLE-STACK-ARG-ALIGNMENT: the carrier states
                    // its datum's own alignment; the cursor clamps it by the CC's
                    // declared cap. 0 (unstated) floors at the slot = the classic
                    // flat stride.
                    std::uint32_t const aggOffset =
                        stackCursor.placeNamedAggregate(
                            aggBytes, ops[k + 1].byValueAggAlign());
                    // The temp's address is a pointer: GPR, or the copy below has
                    // no register to read it through. Refused by name, never
                    // copied through a register of another class.
                    if (argOp.reg.regClass() != LirRegClass::GPR) {
                        report(reporter,
                               DiagnosticCode::L_UnsupportedLoweringForOpcode,
                               DiagnosticSeverity::Error,
                               std::format(
                                   "lowerWideCallArgs: call inst {} arg {} is a "
                                   "by-value stacked aggregate whose address "
                                   "register is of class '{}', not a general "
                                   "register — the copy into its outgoing bytes "
                                   "reads the aggregate through that address",
                                   inst.v, argRegionIdx,
                                   lirRegClassName(argOp.reg.regClass())));
                        return false;
                    }
                    aggCopies.push_back({argOp.reg, aggBytes, aggOffset});
                    std::uint8_t const ex = ops[k + 1].byValueAggExhaust;
                    if (ex == kByValueStackArgExhaustGpr)
                        argCursors.exhaust(LirRegClass::GPR);
                    else if (ex == kByValueStackArgExhaustFpr)
                        argCursors.exhaust(LirRegClass::FPR);
                    // NOT a kept position: the aggregate left the Call, so the
                    // restated vararg boundary must not count it.
                    ++argRegionIdx;
                    ++k;   // the marker is consumed with its carrier
                    continue;
                }
                if (argOp.kind != LirOperandKind::Reg) {
                    // Non-Reg/non-marker scalar operand — a future isel bug the
                    // callconv gate reports loud; keep verbatim (don't split).
                    keepOps.push_back(argOp);
                    noteKeptPosition();
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
                    noteKeptPosition();
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
            // Then each stacked aggregate, one chunk at a time: a GPR-wide `load`
            // from [temp + k] into a fresh virtual register and a
            // `store_outgoing_arg` of it at placement + k. The chunk is the
            // outgoing SLOT, so the bytes written are exactly the whole slots the
            // cursor reserved, and the last read stays inside the temp (HIR->MIR
            // rounds it to an eightbyte multiple; an x87 datum's home is 16).
            // Each chunk register lives from its load to its store, and the
            // temp's address only until its last chunk — none of it is live
            // into the Call.
            // ⓘ k is bounded by the aggregate's size, and on every shipped
            // convention the GPR load's short form carries it: SysV x86_64 —
            // the one convention that stacks an aggregate of any size — reads
            // through disp32; AAPCS64 and Apple stack at most a 64-byte HFA/HVA
            // (a larger composite is passed by reference), inside LDUR's
            // -256..255; Win64 stacks nothing wider than 8 bytes. A displacement
            // a form cannot carry is refused by the encoder by name, never
            // mis-encoded.
            for (auto const& c : aggCopies) {
                for (std::uint32_t off = 0; off < c.bytes;
                     off += kOutgoingSlotBytes) {
                    LirReg const chunk = b.newVReg(LirRegClass::GPR);
                    std::array<LirOperand, 3> const ld{
                        LirOperand::makeReg(c.address),
                        LirOperand::makeMemBase(1),
                        LirOperand::makeMemOffset(static_cast<std::int32_t>(off))};
                    b.addInst(gprLoadOp, chunk, ld, /*payload=*/0,
                              kOutgoingChunkWidthFlags);
                    std::array<LirOperand, 1> const so{LirOperand::makeReg(chunk)};
                    b.addInst(storeOutgoingOp, InvalidLirReg, so,
                              c.byteOffset + off, kOutgoingChunkWidthFlags);
                }
            }
            // D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES: the
            // shrunken Call carries the RENUMBERED vararg boundary and says, in
            // its flags, that its outgoing stack arguments are already placed.
            // Both restate a fact about POSITIONS that stopped being true the
            // moment operands left the list; a consumer reading the ORIGINAL
            // boundary against the shrunken list reads a vararg as a named
            // argument (which is how the SysV variadic vector count silently
            // under-counted), and a consumer that places anything of its own
            // writes into bytes this cursor already handed out.
            // ⚠ The boundary is restated ONLY for a variadic call, because
            // `fixedOperandCount` is consulted only when `isVariadic` and a
            // non-variadic call's payload is 0 — leaving it byte-identical.
            std::uint32_t const outPayload =
                ::dss::call_payload::isVariadic(payload)
                    ? ::dss::call_payload::encode(
                          true, keptFixedArgs,
                          ::dss::call_payload::hasIndirectResult(payload))
                    : payload;
            std::uint8_t const outFlags =
                static_cast<std::uint8_t>(flags | kLirInstFlagOutgoingArgsPlaced);
            // D-LIR-PER-INST-REG-CONSTRAINTS: 1 → N `store_outgoing_arg`
            // carriers + 1 shrunken Call. The side data belongs to the CALL —
            // the carriers are this pass's own outgoing-arg plumbing.
            LirInstId const dstCall =
                b.addInst(op, result, keepOps, outPayload, outFlags);
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
    // The general-register class's own LOAD — what a stacked aggregate's bytes
    // are read from its temp with. Resolved once; a target that declares none is
    // refused here, by name, before any function is rebuilt.
    auto const gprLoadOp =
        schema.regClassOpOpcode(TargetRegClass::GPR, RegClassOp::Load);
    if (!gprLoadOp.has_value()) {
        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
               DiagnosticSeverity::Error,
               "lowerWideCallArgs: target schema declares no general-register "
               "load (registerClassOps gpr 'load'), which copying a stacked "
               "by-value aggregate into its outgoing bytes reads the aggregate "
               "with");
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
        if (!lowerOneFunc(src, fn, schema, *cc, *storeOutgoingOp, *gprLoadOp, b,
                          reporter))
            return out;
    }
    out.lir = std::move(b).finish();
    out.ok = out.lir.moduleFuncCount() == funcCount;
    return out;
}

} // namespace dss
