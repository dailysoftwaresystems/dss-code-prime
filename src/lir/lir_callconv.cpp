#include "lir/lir_callconv.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

namespace {

// Shorthand: shared diagnostic helper from lir_pass_util.
using lir_pass_util::report;

// Per-class width in bytes. The cc's saved-reg and spill-slot areas
// use the widest declared width per class as a uniform slot size —
// this keeps offsets aligned and the substrate target-blind.
[[nodiscard]] std::uint32_t
widthForClass(TargetSchema const& schema, LirRegClass cls) {
    std::uint32_t maxWidth = 0;
    for (auto const& info : schema.registers()) {
        if (static_cast<LirRegClass>(info.regClass) == cls) {
            if (info.widthBytes > maxWidth) maxWidth = info.widthBytes;
        }
    }
    return maxWidth;
}

// Round `n` up to a multiple of `align` (which must be a power of two).
[[nodiscard]] std::uint32_t
alignUp(std::uint32_t n, std::uint32_t align) {
    if (align == 0) return n;
    return (n + align - 1u) & ~(align - 1u);
}

// Walk every inst of a function and collect the set of phys-reg
// ordinals that appear as a result or as a Reg-kind operand AND that
// are in the cc's calleeSaved set. Returns each saved reg as a
// typed `LirReg` (class carried alongside ordinal) so prologue/
// epilogue emission picks the correct store/load opcode per class.
[[nodiscard]] std::vector<LirReg>
collectUsedCalleeSaved(Lir const& lir, LirFuncId fn,
                       TargetSchema const& schema,
                       TargetCallingConvention const& cc) {
    std::unordered_set<std::uint16_t> calleeSavedOrdinals;
    for (auto const& name : cc.calleeSaved) {
        if (auto ord = schema.registerByName(name); ord.has_value()) {
            calleeSavedOrdinals.insert(*ord);
        }
    }
    std::unordered_set<std::uint16_t> used;
    std::uint32_t const blockCount = lir.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const b = lir.funcBlockAt(fn, bi);
        std::uint32_t const n = lir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            LirReg const r = lir.instResult(inst);
            if (r.valid() && r.isPhysical != 0
                && calleeSavedOrdinals.contains(static_cast<std::uint16_t>(r.id))) {
                used.insert(static_cast<std::uint16_t>(r.id));
            }
            for (auto const& op : lir.instOperands(inst)) {
                if (op.kind == LirOperandKind::Reg && op.reg.valid()
                    && op.reg.isPhysical != 0
                    && calleeSavedOrdinals.contains(static_cast<std::uint16_t>(op.reg.id))) {
                    used.insert(static_cast<std::uint16_t>(op.reg.id));
                }
            }
        }
    }
    // Build the typed result in target-declared ordinal order. The
    // class is read from `schema.registerInfo(ord)` so an FPR
    // callee-saved (MS-x64 xmm6..xmm15) carries `LirRegClass::FPR`,
    // not a default GPR class.
    std::vector<std::uint16_t> ordinals(used.begin(), used.end());
    std::sort(ordinals.begin(), ordinals.end());
    std::vector<LirReg> out;
    out.reserve(ordinals.size());
    for (std::uint16_t ord : ordinals) {
        auto const* info = schema.registerInfo(ord);
        LirRegClass const cls = (info != nullptr)
            ? static_cast<LirRegClass>(info->regClass)
            : LirRegClass::GPR;
        out.push_back(makePhysicalReg(ord, cls));
    }
    return out;
}

// Returns std::nullopt iff the target declares no GPR-class registers
// (a schema misconfiguration — earlier silent fallback to 8 hid this).
[[nodiscard]] std::optional<FrameLayout>
computeFrameLayout(LirFuncAllocation const& alloc,
                   TargetSchema const& schema,
                   TargetCallingConvention const& cc,
                   std::vector<LirReg> savedRegs,
                   DiagnosticReporter& reporter) {
    std::uint32_t const slotWidth = std::max(widthForClass(schema, LirRegClass::GPR),
                                             widthForClass(schema, LirRegClass::FPR));
    if (slotWidth == 0) {
        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
               DiagnosticSeverity::Error,
               "computeFrameLayout: target declares no GPR/FPR registers "
               "with widthBytes > 0 — cannot determine slot size");
        return std::nullopt;
    }
    FrameLayout layout;
    layout.savedRegs        = std::move(savedRegs);
    layout.slotSize         = slotWidth;
    layout.savedRegAreaSize = static_cast<std::uint32_t>(layout.savedRegs.size()) * slotWidth;
    layout.spillAreaSize    = alloc.numSpillSlots * slotWidth;
    std::uint32_t const raw = layout.savedRegAreaSize + layout.spillAreaSize;
    std::uint32_t const align = (cc.stackAlignment > 0) ? cc.stackAlignment : 1u;
    layout.totalFrameSize   = alignUp(raw, align);
    return layout;
}

// Emit `<op> SP, bytes` — destructive 2-operand form. Caller supplies
// the resolved opcode directly (sub for prologue, add for epilogue);
// the call site cannot pass a stale/invalid opcode by mistake (the
// pre-fold unified helper passed `0` for the unused arm, which would
// have dispatched to the schema's invalid-sentinel opcode if the sign
// ever flipped). One helper, one operation, no sentinel arm.
void emitSpAdjust(LirBuilder& b, std::uint16_t op, LirReg sp,
                  std::uint32_t bytes) {
    if (bytes == 0) return;
    std::array<LirOperand, 2> ops{
        LirOperand::makeReg(sp),
        LirOperand::makeImmInt32(static_cast<std::int32_t>(bytes))
    };
    b.addInst(op, sp, ops);
}

// Emit `store reg, [SP + offset]` (saved-reg store, or frame-store
// materialization). `store` operand layout per x86_64.target.json:
// [value_reg, base_reg, MemBase(scale), MemOffset(disp)] — 4 ops, no result.
void emitFrameStore(LirBuilder& b, std::uint16_t storeOp, LirReg value,
                    LirReg sp, std::int32_t offset) {
    std::array<LirOperand, 4> ops{
        LirOperand::makeReg(value),
        LirOperand::makeReg(sp),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(offset)
    };
    b.addInst(storeOp, InvalidLirReg, ops);
}

// Emit `result = load [SP + offset]`. `load` operand layout:
// [base_reg, MemBase(scale), MemOffset(disp)] — 3 ops + result.
void emitFrameLoad(LirBuilder& b, std::uint16_t loadOp, LirReg result,
                   LirReg sp, std::int32_t offset) {
    std::array<LirOperand, 3> ops{
        LirOperand::makeReg(sp),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(offset)
    };
    b.addInst(loadOp, result, ops);
}

void emitPrologue(LirBuilder& b, FrameLayout const& layout,
                  LirReg sp, std::uint16_t subOp, std::uint16_t storeOp) {
    emitSpAdjust(b, subOp, sp, layout.totalFrameSize);
    for (std::size_t i = 0; i < layout.savedRegs.size(); ++i) {
        // saved reg carries its own class (set by collectUsedCalleeSaved
        // via schema.registerInfo) — FPR callee-saves (MS-x64 xmm6..15)
        // store with FPR-class encoding rather than being silently
        // mis-classified as GPR.
        emitFrameStore(b, storeOp, layout.savedRegs[i], sp,
                       static_cast<std::int32_t>(i * layout.slotSize));
    }
}

void emitEpilogue(LirBuilder& b, FrameLayout const& layout,
                  LirReg sp, std::uint16_t addOp, std::uint16_t loadOp) {
    // Reverse the prologue: load saved regs FIRST, then restore SP.
    for (std::size_t i = 0; i < layout.savedRegs.size(); ++i) {
        emitFrameLoad(b, loadOp, layout.savedRegs[i], sp,
                      static_cast<std::int32_t>(i * layout.slotSize));
    }
    emitSpAdjust(b, addOp, sp, layout.totalFrameSize);
}

struct OpcodeHandles {
    std::uint16_t mov;
    std::uint16_t add;
    std::uint16_t sub;
    std::uint16_t load;
    std::uint16_t store;
    std::uint16_t frameLoad;
    std::uint16_t frameStore;
    // ML7 cycle 2: virtual-op handles materialized by the callconv pass.
    std::uint16_t arg;
    std::uint16_t call;
};

// Resolve a cc register-name reference to a typed `LirReg`. Returns
// nullopt + a loud diagnostic if the name doesn't exist in the
// schema's `registers[]` (would be a schema misconfiguration that
// `TargetSchemaData::validate` should normally catch — defensive
// here so a loader-bypass path doesn't silently produce a junk reg).
[[nodiscard]] std::optional<LirReg>
resolveCcReg(TargetSchema const& schema,
             std::string_view    name,
             LirRegClass         cls,
             std::string_view    contextLabel,
             DiagnosticReporter& reporter) {
    auto const ord = schema.registerByName(name);
    if (!ord.has_value()) {
        report(reporter, DiagnosticCode::L_CcRegLookupFailed,
               DiagnosticSeverity::Error,
               std::format("{}: calling convention declares register '{}' "
                           "but the target schema's registers[] does not "
                           "declare it — schema misconfiguration",
                           contextLabel, name));
        return std::nullopt;
    }
    return makePhysicalReg(*ord, cls);
}

// Emit a single-source `mov dest, src`. The mov instruction shape
// in the substrate is `mov dest, src_reg` — operand layout
// [src_reg], result = dest. Schema-uniform across SysV/MS-x64/AAPCS64
// because the cc-blind `mov` opcode operates on regs, not the cc.
void emitMov(LirBuilder& b, std::uint16_t movOp, LirReg dest, LirReg src) {
    std::array<LirOperand, 1> ops{LirOperand::makeReg(src)};
    b.addInst(movOp, dest, ops);
}

// Emit `mov dest, src` only when `dest.id != src.id` — the regalloc-
// already-picked-this-reg no-op skip used by every ML7 cycle 2
// materialization site (arg copy, call arg-passing, call return).
// Trivial inline at 3 sites; the named helper makes the intent
// ("skip mov when same reg") immediately readable.
void maybeMov(LirBuilder& b, std::uint16_t movOp, LirReg dest, LirReg src) {
    if (dest.id != src.id) emitMov(b, movOp, dest, src);
}

// Lookup the i-th arg-passing source register for the given class.
// GPR class uses `cc.argGprs`; FPR uses `cc.argFprs`. Returns nullopt
// when `index >= pool.size()` (stack-passed; D-ML7-2.2).
[[nodiscard]] std::optional<LirReg>
argPassingReg(TargetSchema const&            schema,
              TargetCallingConvention const& cc,
              std::uint32_t                  index,
              LirRegClass                    cls,
              std::string_view               contextLabel,
              DiagnosticReporter&            reporter) {
    auto const& pool = (cls == LirRegClass::FPR) ? cc.argFprs : cc.argGprs;
    if (index >= pool.size()) {
        report(reporter, DiagnosticCode::L_StackPassedArgUnsupported,
               DiagnosticSeverity::Error,
               std::format("{}: arg index {} requires stack passing "
                           "(cc '{}' has only {} {} arg-passing registers); "
                           "stack-passed args are anchored at D-ML7-2.2",
                           contextLabel,
                           index, cc.name, pool.size(),
                           (cls == LirRegClass::FPR) ? "FPR" : "GPR"));
        return std::nullopt;
    }
    return resolveCcReg(schema, pool[index], cls, contextLabel, reporter);
}

// Lookup the primary return register (slot 0) for the given class.
// Multi-register returns (SysV's rax+rdx for >64-bit aggregates) are
// not yet exercised — anchored as a future cycle when the first
// >64-bit aggregate return surfaces.
[[nodiscard]] std::optional<LirReg>
returnReg(TargetSchema const&            schema,
          TargetCallingConvention const& cc,
          LirRegClass                    cls,
          std::string_view               contextLabel,
          DiagnosticReporter&            reporter) {
    auto const& pool = (cls == LirRegClass::FPR) ? cc.returnFprs : cc.returnGprs;
    if (pool.empty()) {
        report(reporter, DiagnosticCode::L_CcRegLookupFailed,
               DiagnosticSeverity::Error,
               std::format("{}: calling convention '{}' declares no {} "
                           "return registers but the call has a {} result",
                           contextLabel, cc.name,
                           (cls == LirRegClass::FPR) ? "FPR" : "GPR",
                           (cls == LirRegClass::FPR) ? "float" : "integer"));
        return std::nullopt;
    }
    return resolveCcReg(schema, pool[0], cls, contextLabel, reporter);
}

[[nodiscard]] std::optional<OpcodeHandles>
resolveOpcodes(TargetSchema const& schema, DiagnosticReporter& reporter) {
    OpcodeHandles h{};
    // Field-pointer + mnemonic pairs drive the lookup loop. The fixed
    // ones are compile-time literals; the frame-{load,store} mnemonics
    // are schema-configurable so the target may rename them. Single
    // diagnostic-emission site beats seven copy-pasted if-let-else
    // ladders (simplifier M6).
    struct Entry {
        std::uint16_t OpcodeHandles::* field;
        std::string_view mnem;
    };
    std::array<Entry, 9> const table{{
        {&OpcodeHandles::mov,        "mov"},
        {&OpcodeHandles::add,        "add"},
        {&OpcodeHandles::sub,        "sub"},
        {&OpcodeHandles::load,       "load"},
        {&OpcodeHandles::store,      "store"},
        {&OpcodeHandles::frameLoad,  schema.frameLoadMnemonic()},
        {&OpcodeHandles::frameStore, schema.frameStoreMnemonic()},
        // ML7 cycle 2: arg + call materialized inside this pass.
        {&OpcodeHandles::arg,        "arg"},
        {&OpcodeHandles::call,       "call"},
    }};
    for (auto const& [field, mnem] : table) {
        auto const op = schema.opcodeByMnemonic(mnem);
        if (!op.has_value()) {
            report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                   DiagnosticSeverity::Error,
                   std::format("target schema missing '{}' opcode required for "
                               "callconv lowering",
                               mnem));
            return std::nullopt;
        }
        h.*field = *op;
    }
    return h;
}

// remapOperand + emitTerminator are now in lir_pass_util.

[[nodiscard]] bool
materializeOneFunc(Lir const& src, LirFuncId fn,
                   TargetSchema const& schema,
                   TargetCallingConvention const& cc,
                   LirFuncAllocation const& alloc,
                   LirBuilder& b, OpcodeHandles const& h,
                   FrameLayout& outLayout,
                   DiagnosticReporter& reporter) {
    if (!cc.stackPointer.has_value()) {
        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
               DiagnosticSeverity::Error,
               std::format("calling convention '{}' declares no stackPointer "
                           "register — cannot materialize prologue/epilogue",
                           cc.name));
        return false;
    }
    LirReg const sp = makePhysicalReg(cc.stackPointer->ordinal, LirRegClass::GPR);

    std::vector<LirReg> usedSaved = collectUsedCalleeSaved(src, fn, schema, cc);
    auto layoutOpt = computeFrameLayout(alloc, schema, cc, std::move(usedSaved), reporter);
    if (!layoutOpt.has_value()) return false;
    outLayout = std::move(*layoutOpt);

    auto const& funcInfo = src.funcArena().at(fn);
    b.addFunction(SymbolId{funcInfo.symbol});

    std::uint32_t const blockCount = src.funcBlockCount(fn);
    std::unordered_map<std::uint32_t, LirBlockId> srcToDst;
    srcToDst.reserve(blockCount);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlock = src.funcBlockAt(fn, bi);
        srcToDst[srcBlock.v] = b.createBlock();
    }

    std::uint32_t const slotSize = outLayout.slotSize;

    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlock = src.funcBlockAt(fn, bi);
        LirBlockId const dstBlock = srcToDst.at(srcBlock.v);
        b.beginBlock(dstBlock);

        if (bi == 0) {
            emitPrologue(b, outLayout, sp, h.sub, h.store);
        }

        std::uint32_t const instN = src.blockInstCount(srcBlock);
        for (std::uint32_t i = 0; i < instN; ++i) {
            LirInstId const inst = src.blockInstAt(srcBlock, i);
            std::uint16_t const op = src.instOpcode(inst);
            auto const ops = src.instOperands(inst);
            LirReg const result = src.instResult(inst);
            std::uint32_t const payload = src.instPayload(inst);

            auto const* info = schema.opcodeInfo(op);
            bool const isTerm = (info != nullptr && info->isTerminator());

            // ML7 cycle 2: materialize virtual `arg` op.
            //
            // After regalloc, `result` is a physical register (whatever
            // the allocator picked for the parameter's vreg). The cc
            // declares which physical register the caller placed the
            // k-th param in (`cc.argGprs[k]` for GPR, `cc.argFprs[k]`
            // for FPR). Emit a `mov result, sourceArgReg` to copy the
            // param into the regalloc-chosen home — or skip when
            // regalloc happened to pick the source reg itself (no-op).
            //
            // Out-of-range arg indices fall into D-ML7-2.2 (stack-
            // passed). A future regalloc pre-coloring hint
            // (D-ML7-2.5) would eliminate most of these movs by
            // pre-pinning the param vreg to its cc arg reg; for v1
            // correctness, the unconditional mov is the right shape.
            if (op == h.arg) {
                if (!result.valid() || result.isPhysical == 0) {
                    report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
                           DiagnosticSeverity::Error,
                           std::format("callconv: arg inst {} has no "
                                       "physical-reg result after regalloc",
                                       inst.v));
                    return false;
                }
                auto const argSrc =
                    argPassingReg(schema, cc, payload, result.regClass(),
                                  "materializeOneFunc: arg", reporter);
                if (!argSrc.has_value()) return false;
                maybeMov(b, h.mov, result, *argSrc);
                continue;
            }

            // ML7 cycle 2: materialize virtual `call` op into the
            // explicit ABI sequence.
            //
            // Input shape (post-regalloc): `result?, call callee, arg0,
            // arg1, ...` — operands are [callee, arg0..argN-1] where
            // callee is a SymbolRef (direct call) and each arg is a
            // physical register holding the value at the call site.
            //
            // Output shape:
            //   mov destArgReg_0, arg0   (per cc.argGprs/argFprs by class)
            //   mov destArgReg_1, arg1
            //   ...
            //   call <callee_symbol>     (only the symbol operand —
            //                             matches the existing
            //                             x86-variable encoding variant
            //                             guard `["symbol"]`)
            //   mov result, returnReg    (only if result is non-void;
            //                             returnReg picked from
            //                             cc.returnGprs[0] / returnFprs[0]
            //                             by result class)
            //
            // Indirect calls (`call <reg>`) are anchored at D-ML7-2.4.
            // Move-graph cycles (when regalloc pins src args to dest
            // arg-passing regs in a way that produces a cycle —
            // e.g. arg0 in argGprs[1], arg1 in argGprs[0] would need
            // `mov rdi, rsi; mov rsi, rdi`, where the second read sees
            // the clobbered rdi) trip `L_MoveCycleUnsupported` loud.
            // Proper parallel-copy resolution is anchored at D-ML7-2.3.
            if (op == h.call) {
                if (ops.empty()) {
                    report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
                           DiagnosticSeverity::Error,
                           std::format("callconv: call inst {} has no "
                                       "operands (expected [callee, args...])",
                                       inst.v));
                    return false;
                }
                LirOperand const calleeOp = ops[0];
                if (calleeOp.kind != LirOperandKind::SymbolRef) {
                    report(reporter,
                           DiagnosticCode::L_IndirectCallUnsupported,
                           DiagnosticSeverity::Error,
                           std::format("callconv: call inst {}'s callee "
                                       "operand is not a SymbolRef — indirect "
                                       "calls are anchored at D-ML7-2.4",
                                       inst.v));
                    return false;
                }
                // Plan the move-to-arg-register sequence up front so a
                // move-cycle check can run BEFORE any mov is emitted.
                // Each entry is (destination arg-passing physreg,
                // source physreg holding the value).
                struct ArgMove {
                    LirReg dest;
                    LirReg src;
                };
                std::vector<ArgMove> argMoves;
                argMoves.reserve(ops.size());
                std::uint32_t gprIdx = 0;
                std::uint32_t fprIdx = 0;
                for (std::size_t i = 1; i < ops.size(); ++i) {
                    LirOperand const& argOp = ops[i];
                    if (argOp.kind != LirOperandKind::Reg
                        || argOp.reg.isPhysical == 0) {
                        report(reporter,
                               DiagnosticCode::L_VirtualRegInPostRegalloc,
                               DiagnosticSeverity::Error,
                               std::format("callconv: call inst {} arg {} "
                                           "is not a physical-reg operand "
                                           "after regalloc", inst.v, i - 1));
                        return false;
                    }
                    LirReg const srcReg = argOp.reg;
                    LirRegClass const cls = srcReg.regClass();
                    std::uint32_t const argIndex =
                        (cls == LirRegClass::FPR) ? fprIdx++ : gprIdx++;
                    auto const destReg =
                        argPassingReg(schema, cc, argIndex, cls,
                                      "materializeOneFunc: call", reporter);
                    if (!destReg.has_value()) return false;
                    argMoves.push_back({*destReg, srcReg});
                }
                // Move-cycle detection (silent-failure-hunter CRITICAL
                // F1 fold). The naive emit-in-order shape is correct
                // ONLY when no destination of a NOT-yet-emitted mov is
                // also a source of an ALREADY-emitted mov — otherwise
                // the second mov reads a clobbered register. A trivial
                // O(N^2) scan suffices for the v1 N<=6 (SysV) /
                // N<=4 (MS-x64) GPR-arg shape.
                //
                // Detection: for each move i, check whether its source
                // is the destination of any subsequent move j>i AND
                // dest_j != src_j (i.e. j is a real mov, not a no-op).
                // If yes, the in-order emission would clobber src_i
                // before move i runs — fail loud anchored to D-ML7-2.3.
                for (std::size_t i = 0; i < argMoves.size(); ++i) {
                    if (argMoves[i].dest.id == argMoves[i].src.id) continue;
                    for (std::size_t j = i + 1; j < argMoves.size(); ++j) {
                        if (argMoves[j].dest.id == argMoves[j].src.id) continue;
                        if (argMoves[j].dest.id == argMoves[i].src.id) {
                            report(reporter,
                                   DiagnosticCode::L_MoveCycleUnsupported,
                                   DiagnosticSeverity::Error,
                                   std::format("callconv: call inst {} arg-"
                                               "passing moves form a cycle "
                                               "(move {} src reg #{} is the "
                                               "destination of later move {}); "
                                               "parallel-copy resolution is "
                                               "anchored at D-ML7-2.3",
                                               inst.v, i,
                                               static_cast<unsigned>(argMoves[i].src.id),
                                               j));
                            return false;
                        }
                    }
                }
                // Cycle-free — emit in order.
                for (auto const& m : argMoves) {
                    maybeMov(b, h.mov, m.dest, m.src);
                }
                // Emit `call <callee_symbol>` — single-symbol operand
                // form (matches the schema's variant guard).
                std::array<LirOperand, 1> callOps{calleeOp};
                b.addInst(h.call, InvalidLirReg, callOps,
                          payload, src.instFlags(inst));
                // Move return register into result (only if non-void).
                if (result.valid()) {
                    if (result.isPhysical == 0) {
                        report(reporter,
                               DiagnosticCode::L_VirtualRegInPostRegalloc,
                               DiagnosticSeverity::Error,
                               std::format("callconv: call inst {} result "
                                           "is not a physical reg after regalloc",
                                           inst.v));
                        return false;
                    }
                    auto const retReg =
                        returnReg(schema, cc, result.regClass(),
                                  "materializeOneFunc: call", reporter);
                    if (!retReg.has_value()) return false;
                    maybeMov(b, h.mov, result, *retReg);
                }
                continue;
            }

            // Materialize frame_load / frame_store.
            if (op == h.frameLoad) {
                LirSpillSlot const slot{payload};
                std::int32_t const offset = static_cast<std::int32_t>(
                    outLayout.spillAreaOffset() + (slot.v - 1u) * slotSize);
                emitFrameLoad(b, h.load, result, sp, offset);
                continue;
            }
            if (op == h.frameStore) {
                LirSpillSlot const slot{payload};
                std::int32_t const offset = static_cast<std::int32_t>(
                    outLayout.spillAreaOffset() + (slot.v - 1u) * slotSize);
                if (ops.empty() || ops[0].kind != LirOperandKind::Reg) {
                    report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
                           DiagnosticSeverity::Error,
                           std::format("callconv: frame_store inst {} has "
                                       "malformed operand list", inst.v));
                    return false;
                }
                emitFrameStore(b, h.store, ops[0].reg, sp, offset);
                continue;
            }

            std::vector<LirOperand> newOps;
            newOps.reserve(ops.size());
            for (auto const& o : ops) newOps.push_back(lir_pass_util::remapBlockRef(o, srcToDst));

            if (isTerm) {
                bool const isReturn =
                    info != nullptr && info->minSuccessors == 0
                    && info->maxSuccessors == 0
                    && info->result == TargetResultRule::None;
                auto const succs = src.blockSuccessors(srcBlock);
                if (isReturn && succs.empty()) {
                    // Emit epilogue BEFORE the return.
                    emitEpilogue(b, outLayout, sp, h.add, h.load);
                }
                if (!lir_pass_util::emitTerminator(b, op, info, succs, newOps,
                                                   payload, src.instFlags(inst),
                                                   srcToDst,
                                                   "callconv", reporter)) {
                    return false;
                }
            } else {
                b.addInst(op, result, newOps, payload, src.instFlags(inst));
            }
        }
    }

    return true;
}

} // namespace

LirCallconvResult
materializeCallingConvention(Lir const&           src,
                             TargetSchema const&  schema,
                             LirAllocation const& alloc,
                             DiagnosticReporter&  reporter) {
    LirCallconvResult out;

    auto opcodes = resolveOpcodes(schema, reporter);
    if (!opcodes.has_value()) {
        return out;  // empty Lir + empty perFunc — `ok()` returns false
    }

    LirBuilder b{schema};
    std::size_t const fnCount = src.moduleFuncCount();
    out.perFunc.reserve(fnCount);

    // Allocations are looked up by parallel index. The rewrite pass
    // (lir_rewrite.cpp) produced `src` as a fresh module whose
    // `LirFuncId` arena tags differ from the original, so the
    // strong-id `LirAllocation::forFunc(fn)` lookup would fail. We
    // additionally cross-check the per-function `symbol` to catch the
    // case where the rewrite pass ever reorders, drops, or inserts
    // functions — the comment-only contract becomes a structural
    // assertion at the symbol layer.
    if (alloc.perFunc.size() != fnCount) {
        report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
               DiagnosticSeverity::Error,
               std::format("materializeCallingConvention: function count "
                           "mismatch — src has {} but alloc has {}",
                           fnCount, alloc.perFunc.size()));
        return LirCallconvResult{};
    }
    for (std::uint32_t i = 0; i < fnCount; ++i) {
        LirFuncId const fn = src.funcAt(i);
        LirFuncAllocation const& funcAlloc = alloc.perFunc[i];
        // Cross-check the rewrite preserved the per-function symbol
        // identity. `LirFuncAllocation::originalSymbol` was stamped
        // by the allocator from the original LIR. Matching against
        // the rewritten LIR's `funcInfo.symbol` proves the parallel-
        // index correspondence is intact.
        std::uint32_t const rewrittenSymbol =
            src.funcArena().at(fn).symbol;
        if (funcAlloc.originalSymbol.v != 0
            && funcAlloc.originalSymbol.v != rewrittenSymbol) {
            report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
                   DiagnosticSeverity::Error,
                   std::format("materializeCallingConvention: function {} "
                               "symbol mismatch (allocation:{} vs rewritten:{}) "
                               "— rewrite pass reordered/dropped functions",
                               fn.v,
                               funcAlloc.originalSymbol.v,
                               rewrittenSymbol));
            return LirCallconvResult{};
        }
        if (!funcAlloc.ok) {
            report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
                   DiagnosticSeverity::Error,
                   std::format("materializeCallingConvention: function {} has "
                               "no valid allocation (skipped)", fn.v));
            return LirCallconvResult{};
        }
        auto const* cc = schema.callingConvention(funcAlloc.callingConventionIndex);
        if (cc == nullptr) {
            report(reporter, DiagnosticCode::R_CallingConventionLookupFailed,
                   DiagnosticSeverity::Error,
                   std::format("materializeCallingConvention: function {} "
                               "references invalid cc index {}",
                               fn.v,
                               static_cast<unsigned>(funcAlloc.callingConventionIndex)));
            return LirCallconvResult{};
        }
        FrameLayout layout;
        if (!materializeOneFunc(src, fn, schema, *cc, funcAlloc, b, *opcodes,
                                layout, reporter)) {
            return LirCallconvResult{};
        }
        out.perFunc.push_back(std::move(layout));
    }

    out.lir = std::move(b).finish();
    // `ok()` is derived from output shape — no stored bool to drift.
    return out;
}

} // namespace dss
