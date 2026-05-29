#include "lir/lir_callconv.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"

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

void report(DiagnosticReporter& reporter, DiagnosticCode code,
            DiagnosticSeverity severity, std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = severity;
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
}

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

// Emit `sub SP, bytes` — destructive 2-operand form. Both helpers
// take the resolved opcode directly so the call site cannot pass a
// stale/invalid opcode by mistake (the previous unified helper passed
// `0` for the unused arm, which would dispatch to the schema's
// invalid sentinel opcode if the sign ever flipped).
void emitSpSub(LirBuilder& b, std::uint16_t subOp, LirReg sp,
               std::uint32_t bytes) {
    if (bytes == 0) return;
    std::array<LirOperand, 2> ops{
        LirOperand::makeReg(sp),
        LirOperand::makeImmInt32(static_cast<std::int32_t>(bytes))
    };
    b.addInst(subOp, sp, ops);
}

void emitSpAdd(LirBuilder& b, std::uint16_t addOp, LirReg sp,
               std::uint32_t bytes) {
    if (bytes == 0) return;
    std::array<LirOperand, 2> ops{
        LirOperand::makeReg(sp),
        LirOperand::makeImmInt32(static_cast<std::int32_t>(bytes))
    };
    b.addInst(addOp, sp, ops);
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
    emitSpSub(b, subOp, sp, layout.totalFrameSize);
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
    emitSpAdd(b, addOp, sp, layout.totalFrameSize);
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
    auto lookup = [&](std::string_view mnem,
                      std::string const& what) -> std::optional<std::uint16_t> {
        auto const op = schema.opcodeByMnemonic(mnem);
        if (!op.has_value()) {
            report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                   DiagnosticSeverity::Error,
                   std::format("target schema missing '{}' opcode required for {}",
                               mnem, what));
            return std::nullopt;
        }
        return *op;
    };
    OpcodeHandles h{};
    if (auto v = lookup("mov", "callconv lowering"); v) h.mov = *v; else return std::nullopt;
    if (auto v = lookup("add", "callconv lowering"); v) h.add = *v; else return std::nullopt;
    if (auto v = lookup("sub", "callconv lowering"); v) h.sub = *v; else return std::nullopt;
    if (auto v = lookup("load", "callconv lowering"); v) h.load = *v; else return std::nullopt;
    if (auto v = lookup("store", "callconv lowering"); v) h.store = *v; else return std::nullopt;
    if (auto v = lookup(schema.frameLoadMnemonic(), "callconv lowering"); v)
        h.frameLoad = *v;
    else return std::nullopt;
    if (auto v = lookup(schema.frameStoreMnemonic(), "callconv lowering"); v)
        h.frameStore = *v;
    else return std::nullopt;
    return h;
}

// Translate a non-branch operand: BlockRef gets remapped to dest-side
// block ids; everything else passes through.
[[nodiscard]] LirOperand
remapOperand(LirOperand const& op,
             std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst) {
    if (op.kind == LirOperandKind::BlockRef) {
        auto it = srcToDst.find(op.blockSlot);
        if (it != srcToDst.end()) return LirOperand::makeBlockRef(it->second.v);
    }
    return op;
}

[[nodiscard]] bool
emitTerminator(LirBuilder& b, std::uint16_t op, TargetOpcodeInfo const* info,
               std::span<LirBlockId const> succs,
               std::span<LirOperand const> newOps,
               std::uint32_t payload,
               std::unordered_map<std::uint32_t, LirBlockId> const& srcToDst,
               DiagnosticReporter& reporter) {
    switch (succs.size()) {
        case 0:
            if (info != nullptr
                && info->minSuccessors == 0 && info->maxSuccessors == 0
                && info->result == TargetResultRule::None
                && newOps.empty()) {
                b.addUnreachable(op);
            } else {
                b.addReturn(op, newOps);
            }
            return true;
        case 1:
            b.addBr(op, srcToDst.at(succs[0].v));
            return true;
        case 2:
            b.addCondBr(op, newOps,
                        srcToDst.at(succs[0].v),
                        srcToDst.at(succs[1].v), payload);
            return true;
        default:
            report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
                   DiagnosticSeverity::Error,
                   std::format("callconv: terminator opcode {} has {} "
                               "successors; only 0/1/2 supported",
                               static_cast<unsigned>(op),
                               static_cast<unsigned>(succs.size())));
            return false;
    }
}

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
            for (auto const& o : ops) newOps.push_back(remapOperand(o, srcToDst));

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
                if (!emitTerminator(b, op, info, succs, newOps, payload,
                                    srcToDst, reporter)) {
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
