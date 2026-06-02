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
using dss::report;

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
//
// D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY closure (2026-06-02): the layout
// now incorporates the caller-side Win64 shadow-space requirement +
// post-CALL alignment-bias for any function that makes at least one
// call (`hasCalls == true`). Leaf functions (no calls) skip both —
// they don't need to home a callee's register args (no callee exists)
// and they don't need to re-align RSP for a call site that doesn't
// exist.
//
// Formula (non-leaf, register-machine ABI):
//   raw_with_shadow = max(savedRegAreaSize + spillAreaSize,
//                         cc.shadowSpaceBytes)
//   totalFrameSize  = alignedSizeWithBias(raw_with_shadow,
//                                          cc.stackAlignment,
//                                          cc.callPushBytes)
//
// Win64 example (ms_x64, no callee-saved, no spills, calls puts):
//   raw_with_shadow = max(0, 32) = 32
//   totalFrameSize  = alignedSizeWithBias(32, 16, 8) = 40
//   Prologue emits `sub rsp, 0x28`. After the sub, RSP ≡ 0 mod 16 at
//   any subsequent call site, AND 32 bytes of shadow space sit
//   below the return address for the callee to home rcx/rdx/r8/r9.
//
// SysV example (sysv_amd64, no callee-saved, no spills, calls
// libc.foo):
//   raw_with_shadow = max(0, 0) = 0       (no shadow space requirement)
//   totalFrameSize  = alignedSizeWithBias(0, 16, 8) = 8
//   Prologue emits `sub rsp, 8`. Re-aligns RSP from ≡8 mod 16
//   (post-CALL) to ≡0 mod 16 (callee-entry-aligned). No shadow
//   space needed; alignment-only.
//
// ARM64 example (aapcs64, no callee-saved beyond x30, no spills,
// calls foo):
//   raw_with_shadow = max(0, 0) = 0
//   totalFrameSize  = alignedSizeWithBias(0, 16, 0) = 0
//   No SP delta needed — the x30 save lands in `savedRegAreaSize`
//   via the existing callee-saved tracking.
//
// Leaf function (any cc, no calls):
//   totalFrameSize  = alignUp(savedRegAreaSize + spillAreaSize,
//                              stackAlignment)   (the existing rule)
//
// Closes the SEGV class that caused hello_puts to AV at 0xC0000005 —
// NOT a CRT-init issue (msvcrt's DllMain self-inits stdio). The same
// class of bug the trampoline cycle closed for the kernel→trampoline
// transition, now closed for the user-fn→extern transition one frame
// down.
[[nodiscard]] std::optional<FrameLayout>
computeFrameLayout(LirFuncAllocation const& alloc,
                   TargetSchema const& schema,
                   TargetCallingConvention const& cc,
                   std::vector<LirReg> savedRegs,
                   bool hasCalls,
                   std::uint32_t outgoingArgSlots,
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
    // D-ML7-2.2 + D-ML7-2.6 (co-closed 2026-06-02): outgoingArgAreaSize
    // is THIS function's reserved space for ITS calls. Encompasses
    // BOTH the callee's shadow space (Win64=32, SysV=0; reserved
    // unconditionally when hasCalls) AND any explicit stack-arg
    // overflow (`outgoingArgSlots * outgoingSlotSize`).
    //
    // **Critical: outgoing slots are POINTER-width** (GPR width =
    // 8 bytes on x86_64 / ARM64), NOT the larger of GPR/FPR. Per
    // Win64 + SysV + AAPCS64 ABIs each stack-passed arg occupies
    // exactly one pointer-width slot regardless of its register
    // class (a stack-passed `double` is still 8 bytes; FPR width
    // = 16 bytes is the XMM-register saved-reg width, irrelevant
    // for stack-arg passing). Using the larger slotSize here would
    // double-count outgoing-args and silently inflate every
    // non-leaf frame on x86_64. Exposed on FrameLayout so the
    // materialize call/arg arms use the SAME stride consistently.
    layout.outgoingSlotSize    = widthForClass(schema, LirRegClass::GPR);
    layout.outgoingArgAreaSize = hasCalls
        ? (static_cast<std::uint32_t>(cc.shadowSpaceBytes)
            + outgoingArgSlots * layout.outgoingSlotSize)
        : 0u;
    layout.savedRegAreaSize    = static_cast<std::uint32_t>(layout.savedRegs.size()) * slotWidth;
    layout.spillAreaSize       = alloc.numSpillSlots * slotWidth;
    layout.hasCalls            = hasCalls;
    // Frame zones stack from SP+0 upward: outgoing-args, saved regs,
    // spill slots. Caller-side `frame_store srcReg, [sp + outgoingOffset]`
    // writes into this function's outgoing area at offset
    // `cc.shadowSpaceBytes + overflowIndex * slotWidth` (the shadow
    // space occupies the first cc.shadowSpaceBytes bytes; explicit
    // overflow args begin AFTER it). Callee reads at
    // `[sp + totalFrameSize + cc.callPushBytes +
    // cc.shadowSpaceBytes + overflowIndex * slotWidth]` from its
    // own post-prologue RSP — the caller's outgoing area sits above
    // the callee's full frame + the post-CALL return-address push.
    std::uint32_t const rawPreShadow =
        layout.outgoingArgAreaSize + layout.savedRegAreaSize + layout.spillAreaSize;
    std::uint32_t const align = (cc.stackAlignment > 0) ? cc.stackAlignment : 1u;
    if (hasCalls) {
        // Non-leaf: alignedSizeWithBias with callPushBytes as the
        // bias so post-prologue RSP lands at ≡ 0 mod alignment at
        // the next call site. The shadow-space requirement is
        // already incorporated into rawPreShadow via
        // outgoingArgAreaSize, so no separate max() is needed.
        layout.totalFrameSize = alignedSizeWithBias(
            rawPreShadow, align,
            static_cast<std::uint32_t>(cc.callPushBytes));
    } else {
        // Leaf: existing rule (no callee to home args for, no call
        // site to re-align). outgoingArgSlots == 0 by construction
        // for leaf fns (computeMaxOutgoingStackArgs returns 0 when
        // hasCalls is false).
        layout.totalFrameSize = alignUp(rawPreShadow, align);
    }
    return layout;
}

// D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY: detect whether function `fn` in
// the source LIR makes any function call. Used by `materializeOneFunc`
// to gate shadow-space + alignment-bias allocation in the prologue.
//
// Scans every instruction in every block. The schema's
// `TargetOpcodeInfo::isCall` flag is the load-bearing signal — it is
// set per opcode in the JSON (`"isCall": true`) and is true for BOTH
// direct `call` AND `call_indirect_via_extern`. Mnemonic-matching
// would silently miss the indirect variant (the IAT path puts uses).
// The schema already uses `isCall` for regalloc's cross-call-live
// exclusion (`lir_regalloc.cpp::collectCallPositions`) — same flag,
// same surface, no duplication.
[[nodiscard]] bool
functionHasCalls(Lir const& src, LirFuncId fn,
                 TargetSchema const& schema) noexcept {
    std::uint32_t const blockCount = src.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const blk = src.funcBlockAt(fn, bi);
        std::uint32_t const instN = src.blockInstCount(blk);
        for (std::uint32_t i = 0; i < instN; ++i) {
            LirInstId const inst = src.blockInstAt(blk, i);
            std::uint16_t const op = src.instOpcode(inst);
            auto const* info = schema.opcodeInfo(op);
            if (info != nullptr && info->isCall) return true;
        }
    }
    return false;
}

// D-ML7-2.2 (closed co-with-D-ML7-2.6, 2026-06-02): compute the
// maximum number of stack-passed-arg slots ACROSS all call sites in
// `fn`. The function's prologue must reserve enough outgoing-args
// area to accommodate the WIDEST call (any call with more args than
// the cc's register pool). Per-call site overflow rules:
//
//   * Slot-aligned cc (`cc.slotAligned == true` — Win64 ms_x64):
//     each arg consumes one shared slot regardless of class. Total
//     slots = args.size(). Pool size = max(argGprs, argFprs).
//     Overflow = max(0, total_slots - pool_size).
//   * Independent-counters cc (`cc.slotAligned == false` —
//     SysV / AAPCS64): gpr and fpr counters advance separately.
//     Overflow = max(0, gprArgs - argGprs.size()) +
//                max(0, fprArgs - argFprs.size()).
//
// Args are identified by their LirReg's class (GPR vs FPR). Callee
// is operand[0] (a SymbolRef post-isel), so args are operands[1..N].
//
// Same `TargetOpcodeInfo::isCall` flag the prologue allocator + the
// regalloc cross-call-live exclusion use — single source of truth.
[[nodiscard]] std::uint32_t
computeMaxOutgoingStackArgs(Lir const& src, LirFuncId fn,
                            TargetSchema const& schema,
                            TargetCallingConvention const& cc) noexcept {
    std::uint32_t const gprPoolSize =
        static_cast<std::uint32_t>(cc.argGprs.size());
    std::uint32_t const fprPoolSize =
        static_cast<std::uint32_t>(cc.argFprs.size());
    std::uint32_t const slotAlignedPoolSize =
        std::max(gprPoolSize, fprPoolSize);

    std::uint32_t maxOverflow = 0;
    std::uint32_t const blockCount = src.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const blk = src.funcBlockAt(fn, bi);
        std::uint32_t const instN = src.blockInstCount(blk);
        for (std::uint32_t i = 0; i < instN; ++i) {
            LirInstId const inst = src.blockInstAt(blk, i);
            std::uint16_t const op = src.instOpcode(inst);
            auto const* info = schema.opcodeInfo(op);
            if (info == nullptr || !info->isCall) continue;

            auto const ops = src.instOperands(inst);
            if (ops.empty()) continue;  // malformed; the materialize
                                         // pass will report loud.
            // Operand[0] is the callee SymbolRef (post-isel peephole);
            // args are operands[1..].
            std::uint32_t const argCount =
                static_cast<std::uint32_t>(ops.size() - 1);

            std::uint32_t overflow = 0;
            if (cc.slotAligned) {
                // Each arg consumes one shared slot regardless of class.
                if (argCount > slotAlignedPoolSize) {
                    overflow = argCount - slotAlignedPoolSize;
                }
            } else {
                // Independent counters per class. Gate on Reg-kind
                // operands only (silent-failure H1 audit fold,
                // 2026-06-02): the materialize call arm reports
                // `L_VirtualRegInPostRegalloc` for non-Reg arg
                // operands, but THIS pre-scan runs BEFORE the
                // materialize gate. A future isel arm that puts an
                // Imm or SymbolRef in operand[1..] would otherwise
                // silently inflate this fn's frame by counting the
                // non-Reg as a GPR arg.
                std::uint32_t gprArgs = 0, fprArgs = 0;
                for (std::size_t k = 1; k < ops.size(); ++k) {
                    LirOperand const& argOp = ops[k];
                    if (argOp.kind != LirOperandKind::Reg) continue;
                    if (argOp.reg.regClass() == LirRegClass::FPR) {
                        ++fprArgs;
                    } else {
                        ++gprArgs;
                    }
                }
                std::uint32_t const gprOverflow =
                    (gprArgs > gprPoolSize) ? (gprArgs - gprPoolSize) : 0u;
                std::uint32_t const fprOverflow =
                    (fprArgs > fprPoolSize) ? (fprArgs - fprPoolSize) : 0u;
                overflow = gprOverflow + fprOverflow;
            }
            if (overflow > maxOverflow) maxOverflow = overflow;
        }
    }
    return maxOverflow;
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
    // D-ML7-2.2 audit-fold (2026-06-02 silent-failure CRITICAL C1):
    // saved regs sit at [SP + savedRegAreaOffset() + i*slotSize),
    // NOT [SP + i*slotSize). The new outgoing-args area pushes saved
    // regs upward by `outgoingArgAreaSize` bytes. Pre-fold the
    // emitter wrote saved regs into the outgoing-area memory,
    // silently corrupting outgoing stack args (and reading garbage
    // back at the epilogue).
    std::uint32_t const base = layout.savedRegAreaOffset();
    for (std::size_t i = 0; i < layout.savedRegs.size(); ++i) {
        // saved reg carries its own class (set by collectUsedCalleeSaved
        // via schema.registerInfo) — FPR callee-saves (MS-x64 xmm6..15)
        // store with FPR-class encoding rather than being silently
        // mis-classified as GPR.
        emitFrameStore(b, storeOp, layout.savedRegs[i], sp,
                       static_cast<std::int32_t>(base + i * layout.slotSize));
    }
}

void emitEpilogue(LirBuilder& b, FrameLayout const& layout,
                  LirReg sp, std::uint16_t addOp, std::uint16_t loadOp) {
    // Reverse the prologue: load saved regs FIRST, then restore SP.
    // Same savedRegAreaOffset() bias as the prologue (mirrored
    // reads — silent miscompile if these diverge).
    std::uint32_t const base = layout.savedRegAreaOffset();
    for (std::size_t i = 0; i < layout.savedRegs.size(); ++i) {
        emitFrameLoad(b, loadOp, layout.savedRegs[i], sp,
                      static_cast<std::int32_t>(base + i * layout.slotSize));
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
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02):
    // indirect call via extern import. Same arg-setup semantics as
    // `call` (caller places args in the cc's argGprs/argFprs); the
    // distinction is the call-instruction byte form (FF 15 vs E8) +
    // how the linker patches disp32 (IAT slot RVA vs callee RVA).
    // The materialize pass treats both identically for arg setup.
    std::uint16_t callIndirectViaExtern;
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
    // `optional == true` means: if the schema doesn't declare this
    // mnemonic, leave the field at 0 (the invalid-sentinel opcode)
    // and continue rather than failing loud. The materialize loop's
    // `op == h.callIndirectViaExtern` check then never matches
    // (a real input opcode is always > 0). This preserves
    // agnosticism for a target schema that genuinely doesn't NEED
    // the opcode (e.g. ARM64's GOT/PLT macro-op encoding lands in a
    // future cycle; the schema may not declare
    // `call_indirect_via_extern` until then — and a c-subset module
    // with no extern calls under ARM64 should still lower cleanly).
    // Required opcodes still fail loud on missing.
    //
    // Audit-fold critical (code-reviewer C1): without this, my prior
    // unconditional addition of `call_indirect_via_extern` to the
    // table broke ARM64's resolveOpcodes for every function — even
    // those with no extern calls. Closed-set requiredness only for
    // opcodes the pass GENUINELY cannot operate without.
    struct Entry {
        std::uint16_t OpcodeHandles::* field;
        std::string_view mnem;
        bool optional;
    };
    std::array<Entry, 10> const table{{
        {&OpcodeHandles::mov,        "mov",        false},
        {&OpcodeHandles::add,        "add",        false},
        {&OpcodeHandles::sub,        "sub",        false},
        {&OpcodeHandles::load,       "load",       false},
        {&OpcodeHandles::store,      "store",      false},
        {&OpcodeHandles::frameLoad,  schema.frameLoadMnemonic(),  false},
        {&OpcodeHandles::frameStore, schema.frameStoreMnemonic(), false},
        // ML7 cycle 2: arg + call materialized inside this pass.
        {&OpcodeHandles::arg,        "arg",        false},
        {&OpcodeHandles::call,       "call",       false},
        // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02):
        // optional — a target without dynamic-import support legitimately
        // omits this opcode. MIR→LIR's separate per-call extern check at
        // `lowerCall` fails loud upstream if a call to a missing-opcode
        // extern is lowered, so the absence here is safe.
        {&OpcodeHandles::callIndirectViaExtern,
         "call_indirect_via_extern", true},
    }};
    for (auto const& [field, mnem, optional] : table) {
        auto const op = schema.opcodeByMnemonic(mnem);
        if (!op.has_value()) {
            if (optional) {
                // Leave field at 0 (invalid sentinel — never matches
                // a real input opcode).
                continue;
            }
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
    bool const hasCalls = functionHasCalls(src, fn, schema);
    // D-ML7-2.2: pre-scan call sites for the maximum stack-arg
    // overflow across all calls. The prologue reserves enough
    // outgoing-arg-area bytes for the widest call.
    std::uint32_t const outgoingArgSlots = hasCalls
        ? computeMaxOutgoingStackArgs(src, fn, schema, cc)
        : 0u;
    auto layoutOpt = computeFrameLayout(alloc, schema, cc,
                                        std::move(usedSaved),
                                        hasCalls, outgoingArgSlots,
                                        reporter);
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
            // D-ML7-2.2 + D-ML7-2.6 closure (2026-06-02): when the
            // arg index overflows the register pool, the arg lives in
            // the caller's outgoing-args area on the stack. The
            // callee reads it via `frame_load result, [sp +
            // totalFrameSize + callPushBytes + shadowSpaceBytes +
            // overflowIndex * outgoingSlotSize]` — the caller's
            // outgoing area sits above this fn's full frame + the
            // post-CALL return-address push, and stack args start
            // AFTER the shadow space at offset `shadowSpaceBytes`
            // within the outgoing area. `outgoingSlotSize` (= GPR
            // width = pointer width = 8 on x86_64/ARM64), NOT the
            // larger `slotSize` (= max(GPR, FPR) = 16 on x86_64 due
            // to XMM 128-bit registers).
            //
            // Slot-aligned cc (Win64 ms_x64): payload is the flat
            // slot index; pool size = max(argGprs, argFprs); register
            // resident when slot < poolSize, else stack-resident.
            // Independent counters (SysV/AAPCS64): payload IS the
            // per-class index (existing semantics — see D-ML7-2.10
            // anchor for the mixed-class latent gap).
            //
            // A future regalloc pre-coloring hint (D-ML7-2.5) would
            // eliminate most register-resident movs by pre-pinning
            // the param vreg to its cc arg reg; for v1 correctness,
            // the unconditional mov is the right shape.
            if (op == h.arg) {
                if (!result.valid() || result.isPhysical == 0) {
                    report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
                           DiagnosticSeverity::Error,
                           std::format("callconv: arg inst {} has no "
                                       "physical-reg result after regalloc",
                                       inst.v));
                    return false;
                }
                LirRegClass const cls = result.regClass();
                std::uint32_t const slotAlignedPoolSize = std::max(
                    static_cast<std::uint32_t>(cc.argGprs.size()),
                    static_cast<std::uint32_t>(cc.argFprs.size()));
                std::uint32_t const classPoolSize = static_cast<std::uint32_t>(
                    (cls == LirRegClass::FPR) ? cc.argFprs.size() : cc.argGprs.size());
                std::uint32_t const poolSize = cc.slotAligned
                    ? slotAlignedPoolSize : classPoolSize;
                if (payload < poolSize) {
                    // Register-resident: existing path.
                    auto const argSrc =
                        argPassingReg(schema, cc, payload, cls,
                                      "materializeOneFunc: arg", reporter);
                    if (!argSrc.has_value()) return false;
                    maybeMov(b, h.mov, result, *argSrc);
                } else {
                    // Stack-resident: read from caller's outgoing area.
                    // Offset within outgoing area = shadowSpaceBytes +
                    // (overflow_index * outgoingSlotSize). Callee reads
                    // it at totalFrameSize + callPushBytes above its
                    // own SP. outgoingSlotSize (= GPR width, 8 on
                    // x86_64) is the per-stack-arg stride — NOT
                    // slotSize (max of GPR/FPR = 16 on x86_64) which
                    // is the spill-slot stride.
                    std::uint32_t const overflowIdx = payload - poolSize;
                    std::int32_t const offset = static_cast<std::int32_t>(
                        outLayout.totalFrameSize
                        + static_cast<std::uint32_t>(cc.callPushBytes)
                        + static_cast<std::uint32_t>(cc.shadowSpaceBytes)
                        + overflowIdx * outLayout.outgoingSlotSize);
                    emitFrameLoad(b, h.load, result, sp, offset);
                }
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
            // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold: arg-setup
            // + return-value handling are identical across every call-
            // shaped opcode (direct `call`, `call_indirect_via_extern`,
            // and any future variant a target schema adds with
            // `isCall: true`). Gate on the SAME `TargetOpcodeInfo::isCall`
            // flag that `functionHasCalls` uses + the `hasCalls`
            // detection scan uses — single source of truth.
            // Mnemonic-equality match (`op == h.call || op ==
            // h.callIndirectViaExtern`) was the prior shape; it would
            // silently miss a 3rd call-shaped opcode (e.g. a future
            // `call_indirect_reg` for function-pointer support
            // anchored at D-ML7-2.4) — the silent-failure audit pin.
            if (info != nullptr && info->isCall) {
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
                // Plan the move-to-arg-register sequence (and any
                // stack-arg spills) up front so a move-cycle check
                // can run BEFORE any mov is emitted AND stack stores
                // happen BEFORE register moves (so the stack-store
                // reads its src reg before any register move could
                // clobber it).
                struct ArgMove {
                    LirReg dest;
                    LirReg src;
                };
                struct StackArgStore {
                    LirReg       src;
                    std::int32_t offset;  // from THIS fn's SP-post-prologue
                };
                std::vector<ArgMove> argMoves;
                std::vector<StackArgStore> stackStores;
                argMoves.reserve(ops.size());
                // D-ML7-2.6: under slot-aligned cc (Win64 ms_x64),
                // each arg consumes one shared slot index regardless
                // of class. Under independent counters (SysV/AAPCS64),
                // gpr/fpr counters advance separately.
                std::uint32_t const slotAlignedPoolSize = std::max(
                    static_cast<std::uint32_t>(cc.argGprs.size()),
                    static_cast<std::uint32_t>(cc.argFprs.size()));
                std::uint32_t gprIdx = 0;
                std::uint32_t fprIdx = 0;
                std::uint32_t slotIdx = 0;
                std::uint32_t overflowIdx = 0;  // count of stack-args so far
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
                    std::uint32_t argIndex;
                    std::uint32_t poolSize;
                    if (cc.slotAligned) {
                        argIndex  = slotIdx++;
                        poolSize  = slotAlignedPoolSize;
                    } else {
                        if (cls == LirRegClass::FPR) {
                            argIndex = fprIdx++;
                            poolSize = static_cast<std::uint32_t>(cc.argFprs.size());
                        } else {
                            argIndex = gprIdx++;
                            poolSize = static_cast<std::uint32_t>(cc.argGprs.size());
                        }
                    }
                    if (argIndex < poolSize) {
                        // Register-resident arg: existing path.
                        auto const destReg =
                            argPassingReg(schema, cc, argIndex, cls,
                                          "materializeOneFunc: call", reporter);
                        if (!destReg.has_value()) return false;
                        argMoves.push_back({*destReg, srcReg});
                    } else {
                        // D-ML7-2.2 stack-arg overflow: spill srcReg
                        // into THIS fn's outgoing-args area at
                        // [sp + shadowSpaceBytes + overflowIdx * slotSize].
                        // Stack stores are emitted BEFORE register
                        // moves below so the src reg is read before
                        // any later register move could clobber it.
                        std::int32_t const offset =
                            static_cast<std::int32_t>(
                                static_cast<std::uint32_t>(cc.shadowSpaceBytes)
                                + overflowIdx * outLayout.outgoingSlotSize);
                        stackStores.push_back({srcReg, offset});
                        ++overflowIdx;
                    }
                }
                // Move-ordering hazard detection (silent-failure-hunter
                // CRITICAL F1 fold + post-fold inversion fix). The
                // naive emit-in-order shape is correct ONLY when, for
                // every pair (i, j) with i < j, emitting move i does
                // not clobber a register that move j still needs to
                // read. The hazard predicate is:
                //
                //     dest_i == src_j   for some j > i
                //
                // (Move i WRITES dest_i. Move j READS src_j. If
                // dest_i == src_j and i runs first, the value move j
                // needed is gone.)
                //
                // The previous version of this loop used the inverted
                // predicate `dest_j == src_i` which is SAFE in in-order
                // emission (move i reads src_i before move j writes
                // dest_j) — that loop accidentally still caught true
                // 2-cycle swaps because swaps satisfy BOTH predicates
                // symmetrically, but it false-rejected valid orderable
                // sequences like (arg0: rdi←rsi, arg1: rsi←rcx). The
                // current predicate fires on the true hazard only.
                //
                // The detector is conservative: it rejects any
                // orderable chain where a true cycle is structurally
                // possible (e.g. (a←b, b←a) — a real swap) AND
                // multi-step shapes that the in-order emission cannot
                // handle without re-ordering. Proper parallel-copy
                // resolution (topo-sort + scratch-reg cycle breaking)
                // is anchored at D-ML7-2.3 and would accept all
                // orderable shapes by re-sorting moves before emit.
                for (std::size_t i = 0; i < argMoves.size(); ++i) {
                    if (argMoves[i].dest.id == argMoves[i].src.id) continue;
                    for (std::size_t j = i + 1; j < argMoves.size(); ++j) {
                        if (argMoves[j].dest.id == argMoves[j].src.id) continue;
                        // Hazard: move i writes dest_i which move j
                        // still needs to read as src_j.
                        if (argMoves[i].dest.id == argMoves[j].src.id) {
                            report(reporter,
                                   DiagnosticCode::L_MoveCycleUnsupported,
                                   DiagnosticSeverity::Error,
                                   std::format("callconv: call inst {} arg-"
                                               "passing moves have an order "
                                               "hazard (move {} dest reg #{} "
                                               "is the source of later move "
                                               "{} — emitting in order would "
                                               "clobber it); parallel-copy "
                                               "resolution is anchored at "
                                               "D-ML7-2.3",
                                               inst.v, i,
                                               static_cast<unsigned>(argMoves[i].dest.id),
                                               j));
                            return false;
                        }
                    }
                }
                // D-ML7-2.2 (closed 2026-06-02): emit stack-arg
                // stores FIRST, before any register move. The
                // stack-store reads its src reg; if a later register
                // move would have written to that reg, the store
                // would see the clobbered value. Stack stores never
                // write to a register, so they can't participate in
                // the register-move-cycle hazard — they're safe to
                // emit first unconditionally. Offset is from THIS
                // fn's SP-post-prologue (the outgoing-args area sits
                // at [SP+0..outgoingArgAreaSize)).
                for (auto const& s : stackStores) {
                    emitFrameStore(b, h.store, s.src, sp, s.offset);
                }
                // Cycle-free — emit register moves in order.
                for (auto const& m : argMoves) {
                    maybeMov(b, h.mov, m.dest, m.src);
                }
                // Emit the call instruction. Pass through the input
                // opcode (h.call OR h.callIndirectViaExtern) so the
                // assembler picks the right encoding. Single-symbol
                // operand form for both (matches each schema variant
                // guard `["symbol"]`).
                std::array<LirOperand, 1> callOps{calleeOp};
                b.addInst(op, InvalidLirReg, callOps,
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
                    // ML7 cycle 2: callee-side return-value
                    // materialization. If `ret` carries a [reg]
                    // operand (value-returning function), move the
                    // value into cc.returnGprs[0]/returnFprs[0]
                    // BEFORE the epilogue runs, then emit the ret
                    // with NO operand (the value is implicit via
                    // the ABI). Mirror of the caller-side mov-from-
                    // returnReg in the `call` materialization
                    // above. Void-returning functions (empty ops)
                    // skip this block and fall through to the
                    // epilogue + no-op-operand ret.
                    if (!ops.empty()) {
                        if (ops.size() != 1
                            || ops[0].kind != LirOperandKind::Reg
                            || ops[0].reg.isPhysical == 0) {
                            report(reporter,
                                   DiagnosticCode::L_VirtualRegInPostRegalloc,
                                   DiagnosticSeverity::Error,
                                   std::format("callconv: ret inst {} has "
                                               "non-physical-reg operand "
                                               "after regalloc", inst.v));
                            return false;
                        }
                        LirReg const valReg = ops[0].reg;
                        auto const retReg =
                            returnReg(schema, cc, valReg.regClass(),
                                      "materializeOneFunc: ret", reporter);
                        if (!retReg.has_value()) return false;
                        maybeMov(b, h.mov, *retReg, valReg);
                        // Strip the operand from the ret — the
                        // value is now in the cc's return reg.
                        newOps.clear();
                    }
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
