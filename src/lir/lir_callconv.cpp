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
};

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
    std::array<Entry, 7> const table{{
        {&OpcodeHandles::mov,        "mov"},
        {&OpcodeHandles::add,        "add"},
        {&OpcodeHandles::sub,        "sub"},
        {&OpcodeHandles::load,       "load"},
        {&OpcodeHandles::store,      "store"},
        {&OpcodeHandles::frameLoad,  schema.frameLoadMnemonic()},
        {&OpcodeHandles::frameStore, schema.frameStoreMnemonic()},
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
            bool const isTerm = (info != nullptr && info->isTerminator);

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
                                                   payload, srcToDst,
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
