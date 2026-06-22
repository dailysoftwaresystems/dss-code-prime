#include "lir/lir_callconv.hpp"

#include "core/types/call_payload.hpp"
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

// FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): the number of outgoing
// overflow SLOTS a by-value-stack aggregate of `bytes` occupies = ceil(bytes /
// slotSize). One source of the slot formula so the pre-scan reservation
// (computeMaxOutgoingStackArgs) and the placement byte-copy (materializeOneFunc)
// can NEVER disagree — a divergence would either under-reserve the frame (the
// stack stores clobber the frame, a silent miscompile) or over-reserve it.
[[nodiscard]] inline std::uint32_t
byValueStackAggSlots(std::uint32_t bytes, std::uint32_t slotSize) noexcept {
    if (slotSize == 0) return 0;
    return (bytes + slotSize - 1u) / slotSize;
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
// ARM64 example (aapcs64, no GENERAL callee-saved, no spills, calls
// foo — therefore a NON-LEAF function):
//   The x30 link-register save is folded into `savedRegs` by
//   `materializeOneFunc` (gated on hasCalls + cc.linkRegister) — see
//   D-LK10-ENTRY-ARM64-NONLEAF-LINK-REGISTER. x30 is NOT in
//   cc.calleeSaved (it must not be allocatable), so the fold — not the
//   `collectUsedCalleeSaved` walk — is what makes it land here:
//   savedRegAreaSize = 8 (one slot for x30):
//   raw_with_shadow = max(8, 0) = 8
//   totalFrameSize  = alignedSizeWithBias(8, 16, 0) = 16
//   Prologue: `sub sp, 0x10` + `stur x30, [sp]`; epilogue reloads x30
//   before `ret` so the return targets the caller, not a clobbered LR.
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
//
// FC7 (D-FC7-MEMBER-ACCESS): forward-declared here — the slot-span formula
// (defined below near `functionLocalAllocaPayloads`) that BOTH this layout
// pass and the materialize pass call, keeping reserved-size and per-alloca
// offsets in lockstep.
[[nodiscard]] inline std::uint32_t
allocaSlotCount(std::uint32_t payload, std::uint32_t slotWidth) noexcept;

[[nodiscard]] std::optional<FrameLayout>
computeFrameLayout(LirFuncAllocation const& alloc,
                   TargetSchema const& schema,
                   TargetCallingConvention const& cc,
                   std::vector<LirReg> savedRegs,
                   bool hasCalls,
                   std::uint32_t outgoingArgSlots,
                   std::vector<std::uint32_t> const& allocaPayloads,
                   // FC12a-core (D-FC12A-VARIADIC-CALLEE): bytes for the variadic
                   // register-save-area (0 unless this function calls va_start).
                   std::uint32_t vaRegSaveAreaBytes,
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
    // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b) + FC7 (D-FC7-MEMBER-ACCESS):
    // local allocas sit ABOVE the spill area (positive RSP offset post-
    // prologue), in LIR scan order — the SAME order materializeOneFunc
    // assigns offsets, so the two stay in lockstep. Each alloca reserves
    // `allocaSlotCount(payload, slotWidth)` slots: a SCALAR local (payload
    // 0) = 1 slot (the pre-FC7 behaviour, preserved); a STRUCT/UNION local
    // (payload = its layout byte size) = ceil(size / slotWidth) slots, so
    // a >slotWidth aggregate reserves enough space and never overlaps its
    // neighbour. `slotWidth` (max(GPR,FPR), the spill stride) ≥ every C
    // scalar's alignment on the current targets, so a slotWidth-aligned
    // slot satisfies any field's alignment; a target with slotWidth < a
    // struct's alignment is a future concern (the layout `align` is
    // available to fail loud on then).
    layout.numLocalAllocas     = static_cast<std::uint32_t>(allocaPayloads.size());
    std::uint32_t totalLocalSlots = 0;
    for (std::uint32_t const p : allocaPayloads)
        totalLocalSlots += allocaSlotCount(p, slotWidth);
    layout.localAreaSize       = totalLocalSlots * slotWidth;
    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the variadic register-save-area is the
    // TOPMOST frame zone (above local allocas). Sized by the CC's vaListLayout, it
    // is 0 unless this function calls va_start (the caller passes its byte count).
    layout.vaRegSaveAreaSize   = vaRegSaveAreaBytes;
    layout.hasCalls            = hasCalls;
    // Frame zones stack from SP+0 upward: outgoing-args, saved regs,
    // spill slots, then local-alloca slots. Caller-side `frame_store
    // srcReg, [sp + outgoingOffset]` writes into this function's
    // outgoing area at offset `cc.shadowSpaceBytes + overflowIndex *
    // slotWidth` (the shadow space occupies the first
    // cc.shadowSpaceBytes bytes; explicit overflow args begin AFTER
    // it). Callee reads at `[sp + totalFrameSize + cc.callPushBytes +
    // cc.shadowSpaceBytes + overflowIndex * slotWidth]` from its own
    // post-prologue RSP — the caller's outgoing area sits above the
    // callee's full frame + the post-CALL return-address push. Local
    // allocas at `[sp + localAreaOffset() + i*slotWidth]` post-
    // prologue (D-CSUBSET-LOCAL-INT-CODEGEN); the materialize pass
    // emits `lea result, [sp + offset]` for each `alloca` opcode.
    std::uint32_t const rawPreShadow =
        layout.outgoingArgAreaSize + layout.savedRegAreaSize
        + layout.spillAreaSize    + layout.localAreaSize
        + layout.vaRegSaveAreaSize;
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

// FC12a-core (D-FC12A-VARIADIC-CALLEE) + FC12b (D-FC12B-WIN64-VARIADIC-CALLEE) + FC12c
// (D-FC12C-*-VARIADIC-CALLEE): does function `fn` call va_start? Detected by the
// presence of the CC's va_start BASE op — ONE of:
//   * `va_reg_save_area`   — SysVRegisterSave (the prologue spills arg regs into the
//     save-area) AND AAPCS64 Aapcs64DualCursor (its va_start anchors __gr_top/__vr_top
//     at the spilled GR/VR register-save-area — same op).
//   * `va_home_arg_area`   — Win64 HomogeneousPointer (named int regs → home slots).
//   * `va_overflow_arg_area` — Apple arm64 HomogeneousPointer with overflow-base
//     va_start (it has no home/save area — the start IS the overflow base). [FC12c
//     SHOULD-FIX: reuse the EXISTING va_overflow_arg_area op — do NOT mint a new
//     va_stack_arg_area op; teach this detector the third handle.]
// HIR→MIR emits one of these inside the va_start lowering. The presence — NOT the
// FnSig's isVariadic bit — is the precise signal: a variadic function that NEVER
// calls va_start needs no spill (it never reads the varargs). A `0` op handle (a CC
// that does not declare that strategy's op) never matches, so a non-variadic-callee
// target answers false uniformly.
[[nodiscard]] bool
functionUsesVaStart(Lir const& src, LirFuncId fn,
                    std::uint16_t vaRegSaveAreaOp,
                    std::uint16_t vaHomeArgAreaOp,
                    std::uint16_t vaOverflowArgAreaOp) noexcept {
    if (vaRegSaveAreaOp == 0 && vaHomeArgAreaOp == 0 && vaOverflowArgAreaOp == 0)
        return false;
    std::uint32_t const blockCount = src.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const blk = src.funcBlockAt(fn, bi);
        std::uint32_t const instN = src.blockInstCount(blk);
        for (std::uint32_t i = 0; i < instN; ++i) {
            std::uint16_t const op = src.instOpcode(src.blockInstAt(blk, i));
            if ((vaRegSaveAreaOp != 0 && op == vaRegSaveAreaOp)
                || (vaHomeArgAreaOp != 0 && op == vaHomeArgAreaOp)
                || (vaOverflowArgAreaOp != 0 && op == vaOverflowArgAreaOp))
                return true;
        }
    }
    return false;
}

// D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02): count the
// `alloca` virtual ops in `fn`. Each becomes one local-area frame
// slot of `slotSize` bytes; the materialize pass rewrites it to
// `lea result, [sp + localAreaOffset() + i * slotSize]` using the
// 3-op no-index `lea` form.
//
// Source-language and target agnostic: the predicate is mnemonic-
// match on the (per-target) `alloca` opcode handle. A target that
// declares no `alloca` opcode passes `allocaOp == 0` (invalid
// sentinel, never matches a real input opcode) and the count is
// zero for any function — the frame layout collapses to its
// pre-13.3b shape, preserving backward compatibility with all
// shader / WASM / operand-stack targets that don't have body-local
// allocas.
//
// Pre-scan happens at materializeOneFunc time (parallel to the
// `functionHasCalls` + `computeMaxOutgoingStackArgs` pre-scans),
// not at MIR→LIR time, so the count includes any post-regalloc
// inserts (none today, but defensive against future passes that
// could synthesize local slots).
//
// D-CSUBSET-ALLOCA-COUNT-CACHE (anchor, 7-agent fold F2): the count
// IS knowable at MIR→LIR time — each MIR Alloca maps 1:1 to an LIR
// alloca op. Caching it on `LirFuncAllocation` (one uint32_t per
// function) would eliminate this O(blocks × insts) re-scan for the
// common case of "target has alloca, function has 0 allocas". The
// re-scan is 1 of 4 existing per-function pre-scans (callee-saved
// collection + `functionHasCalls` + `computeMaxOutgoingStackArgs`)
// so the incremental cost is marginal today; closure waits for
// profile evidence. Trigger: profiling shows > 0.5% of compile
// time in this helper on multi-function modules.
// FC7 (D-FC7-MEMBER-ACCESS): the number of `slotWidth`-byte frame slots a
// SINGLE alloca reserves. A scalar local (payload 0 — the sentinel) takes
// ONE slot; a struct/union local (payload = its FC6 layout byte size,
// encoded on the MIR/LIR Alloca) takes ceil(size / slotWidth) slots. This
// is the SINGLE source of the slot formula — BOTH the frame-size sum
// (`computeFrameLayout`) and the per-alloca offset progression
// (`materializeOneFunc`) call it, so a struct local's RESERVED span and
// its materialized ADDRESS can never disagree (a divergence would overlap
// the struct onto its stack neighbour — a silent miscompile).
[[nodiscard]] inline std::uint32_t
allocaSlotCount(std::uint32_t payload, std::uint32_t slotWidth) noexcept {
    return (payload == 0u) ? 1u : (payload + slotWidth - 1u) / slotWidth;
}

// Collect the PAYLOAD (0 for a scalar local; the layout byte size for a
// struct/union local) of every `alloca` op in `fn`, in LIR scan order.
// The count is `.size()`; each payload drives `allocaSlotCount`. A target
// with no `alloca` opcode (allocaOp == 0 — shader / WASM operand-stack
// ABIs) yields an empty vector. Agnostic: a mnemonic-match on the per-
// target `alloca` handle. (Pre-scan timing + the caching trigger are
// unchanged — D-CSUBSET-ALLOCA-COUNT-CACHE.)
[[nodiscard]] std::vector<std::uint32_t>
functionLocalAllocaPayloads(Lir const& src, LirFuncId fn,
                            std::uint16_t allocaOp) {
    std::vector<std::uint32_t> payloads;
    if (allocaOp == 0) return payloads;
    std::uint32_t const blockCount = src.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const blk = src.funcBlockAt(fn, bi);
        std::uint32_t const instN = src.blockInstCount(blk);
        for (std::uint32_t i = 0; i < instN; ++i) {
            LirInstId const inst = src.blockInstAt(blk, i);
            if (src.instOpcode(inst) == allocaOp)
                payloads.push_back(src.instPayload(inst));
        }
    }
    return payloads;
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
    // FC12a-struct: the outgoing slot quantum (= GPR width) — the unit a by-value-
    // stack aggregate's bytes round up to. MUST equal FrameLayout.outgoingSlotSize
    // (computeFrameLayout sets it from widthForClass GPR), else the reservation and
    // the placement store offsets disagree (a frame clobber).
    std::uint32_t const outgoingSlotSize = widthForClass(schema, LirRegClass::GPR);

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

            // FC12c (D-FC12C-APPLE-ARM64-VARIADIC-CALLEE): the outgoing-arg-area
            // pre-scan MUST mirror the materialize placement loop's stack-forcing,
            // else the forced varargs write outside the reserved area. Under a CC that
            // always-stacks varargs, every operand past `fixedOperandCount` of a
            // variadic call is on the stack regardless of class/pool — count it here.
            std::uint32_t const payload = src.instPayload(inst);
            bool const variadicForcesStack =
                cc.variadicArgsAlwaysStack
                && ::dss::call_payload::isVariadic(payload);
            std::uint32_t const fixedOps =
                ::dss::call_payload::fixedOperandCount(payload);

            // Single pass over the arg operands (ops[1..]) that MIRRORS the
            // materialize placement loop exactly. A by-value-stack aggregate is a
            // (Reg, ByValueStackAgg) pair: the Reg is NOT a register arg (it does not
            // consume a class pool slot) and the pair occupies ceil(bytes / slot)
            // OVERFLOW slots UNCONDITIONALLY (FC12a-struct, D-FC12A-VARIADIC-MEMORY-
            // CLASS-STRUCT). `byValSlots` accumulates those; the per-class / slot-
            // aligned tallies count only ordinary register args; `forcedVarargs`
            // counts the always-stacked variadic region (carriers excluded — already
            // in byValSlots). `argRegionIdx` (the materialize loop's index, advanced
            // once per ARG-position incl. a carrier) gates the forced-vararg region.
            std::uint32_t gprArgs = 0, fprArgs = 0;     // register-arg class counts
            std::uint32_t slotArgs = 0;                 // slot-aligned register-arg count
            std::uint32_t byValSlots = 0;               // by-value-stack overflow slots
            std::uint32_t forcedVarargs = 0;            // always-stacked variadic region
            std::uint32_t argRegionIdx = 0;             // 0-based arg position
            for (std::size_t k = 1; k < ops.size(); ++k) {
                LirOperand const& argOp = ops[k];
                if (argOp.kind == LirOperandKind::ByValueStackAgg) {
                    // Marker — accounted with its preceding Reg; never a position.
                    continue;
                }
                // A Reg immediately FOLLOWED by a ByValueStackAgg marker is a
                // by-value-stack aggregate carrier (the address Reg). It occupies
                // overflow slots, NOT a register; advance argRegionIdx (it IS an arg
                // position for the forced-vararg boundary) but skip the class tally.
                bool const isByValCarrier =
                    argOp.kind == LirOperandKind::Reg
                    && (k + 1) < ops.size()
                    && ops[k + 1].kind == LirOperandKind::ByValueStackAgg;
                if (isByValCarrier) {
                    byValSlots += byValueStackAggSlots(ops[k + 1].byValueAggBytes,
                                                       outgoingSlotSize);
                    ++argRegionIdx;
                    continue;
                }
                // FC12c always-stack: a vararg (arg position past the fixed ones) under
                // such a CC is stacked regardless of class/pool. Count it as forced
                // overflow and do NOT add it to the class tallies (it never fills a
                // register slot). Named args fall through to the class tallies.
                if (variadicForcesStack && argRegionIdx >= fixedOps) {
                    ++forcedVarargs;
                    ++argRegionIdx;
                    continue;
                }
                // Ordinary register arg. Gate on Reg-kind only (silent-failure H1
                // audit fold, 2026-06-02): a non-Reg/non-marker operand is a future
                // isel bug the materialize gate reports loud — don't silently count it.
                if (argOp.kind != LirOperandKind::Reg) { ++argRegionIdx; continue; }
                if (argOp.reg.regClass() == LirRegClass::FPR) ++fprArgs;
                else ++gprArgs;
                ++slotArgs;
                ++argRegionIdx;
            }

            std::uint32_t overflow = byValSlots;
            if (variadicForcesStack) {
                // forcedVarargs already excludes carriers (counted in byValSlots).
                // Named-arg overflow under independent counters (Apple is NOT
                // slotAligned; bound by the larger pool defensively if it ever is).
                std::uint32_t const namedOverflow =
                    ((gprArgs > gprPoolSize) ? (gprArgs - gprPoolSize) : 0u)
                    + ((fprArgs > fprPoolSize) ? (fprArgs - fprPoolSize) : 0u);
                overflow += forcedVarargs + namedOverflow;
            } else if (cc.slotAligned) {
                // Each register arg consumes one shared slot regardless of class.
                if (slotArgs > slotAlignedPoolSize)
                    overflow += slotArgs - slotAlignedPoolSize;
            } else {
                std::uint32_t const gprOverflow =
                    (gprArgs > gprPoolSize) ? (gprArgs - gprPoolSize) : 0u;
                std::uint32_t const fprOverflow =
                    (fprArgs > fprPoolSize) ? (fprArgs - fprPoolSize) : 0u;
                overflow += gprOverflow + fprOverflow;
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

// D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02): emit
// `result = lea [SP + offset]` — the address of a frame-local slot
// in a register. `lea` operand layout per x86_64.target.json 3-op
// no-index variant: [base_reg, MemBase(scale), MemOffset(disp)] —
// 3 ops + result. Same operand shape as `load`/`store`; the
// distinction is the opcode (`lea` produces an EFFECTIVE ADDRESS,
// not a memory access). Used by the materialize pass to rewrite
// `alloca` instructions into `lea`-of-frame-slot — assigning each
// alloca a fixed offset above the spill area.
void emitFrameAddr(LirBuilder& b, std::uint16_t leaOp, LirReg result,
                   LirReg sp, std::int32_t offset) {
    std::array<LirOperand, 3> ops{
        LirOperand::makeReg(sp),
        LirOperand::makeMemBase(1),
        LirOperand::makeMemOffset(offset)
    };
    b.addInst(leaOp, result, ops);
}

// (emitPrologue / emitEpilogue moved below `classOpHandle` — FC2
// Part B made the saved-reg store/load mnemonics class-resolved.)

struct OpcodeHandles {
    // FC2 Part B: `mov`/`load`/`store` are no longer EMITTED through
    // these handles — every emission site resolves the class-correct
    // opcode via `classOpHandle` (registerClassOps). The three fields
    // stay in the required-resolve table as the load-time existence
    // gate: a target schema lacking the universal GPR bindings fails
    // ONCE here instead of per-instruction downstream.
    std::uint16_t mov;
    std::uint16_t add;
    std::uint16_t sub;
    std::uint16_t load;
    std::uint16_t store;
    std::uint16_t frameLoad;
    std::uint16_t frameStore;
    // ML7 cycle 2: virtual-op handles materialized by the callconv pass.
    std::uint16_t arg;
    // FC7 C1c: the caller-side struct-return piece read (mirror of `arg`).
    // Optional — only SysV struct returns emit it; a target without it leaves
    // this 0 (the `op == h.retPiece` look-ahead then never matches).
    std::uint16_t retPiece;
    // FC7 C3: the callee-side indirect-result (x8 sret) entry read (mirror of
    // `arg`). Optional — only a CC with a register-based sret (indirectResultRegister)
    // emits it; absent ⇒ 0, and `op == h.readIndirectResult` never matches.
    std::uint16_t readIndirectResult;
    std::uint16_t call;
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02):
    // indirect call via extern import. Same arg-setup semantics as
    // `call` (caller places args in the cc's argGprs/argFprs); the
    // distinction is the call-instruction byte form (FF 15 vs E8) +
    // how the linker patches disp32 (IAT slot RVA vs callee RVA).
    // The materialize pass treats both identically for arg setup.
    std::uint16_t callIndirectViaExtern;
    // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02): the
    // `alloca` virtual op is materialized by the callconv pass into
    // `lea result, [sp + localAreaOffset() + i*slotSize]` using the
    // 3-op no-index `lea` form. The opcode handle pair must both
    // resolve — a target without `lea` would naturally lack `alloca`
    // too (both anchored on register-machine ABIs); the loader
    // rejects loud if either is missing AND `alloca` is exercised.
    std::uint16_t alloca_;
    std::uint16_t lea;
    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the two frame-relative va_list address
    // virtual ops, materialized by this pass into `lea result, [sp + offset]`.
    // Optional — only a CC declaring a `vaListLayout` emits them; absent ⇒ field 0,
    // `op == h.vaRegSaveArea` never matches. The PRESENCE of a `va_reg_save_area`
    // inst in a function is also this pass's "function called va_start" signal.
    std::uint16_t vaRegSaveArea;
    std::uint16_t vaOverflowArgArea;
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the Win64 HomogeneousPointer va_start
    // base, materialized into `lea result, [sp + totalFrameSize + callPushBytes +
    // payload*outgoingSlotSize]` (NO shadow — BLOCKER-1). Optional (only the Win64
    // CC's lowering emits it); absent ⇒ field 0, never matches. Its presence is also
    // the Win64 prologue home-spill signal (functionUsesVaStart detects it too).
    std::uint16_t vaHomeArgArea;
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the xmm→r64 bit-copy (`movq_xmm_to_gpr`)
    // the caller emits to DUPLICATE each Win64 FP vararg into its matching integer
    // (home) register. Optional (x86_64 declares it; arm64 does not — AAPCS64 passes
    // FP varargs differently). Absent ⇒ 0; the FP-dup path requires it and fails loud
    // if a HomogeneousPointer CC reaches the dup with the opcode missing.
    std::uint16_t movqXmmToGpr;
    // FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE): the 128-bit VR spill store (`fstur_q`,
    // STUR Qt) the AAPCS64 variadic prologue uses to spill v0..v7 into the VR block of
    // the register-save-area (16B slots — the class `store` op is the 8-byte D-form,
    // which would spill only the low half). Optional (arm64 declares it; x86_64 does
    // not — its SSE save uses the 16-byte movaps via the class store). Absent ⇒ 0; the
    // AAPCS64 spill path requires it and fails loud if a dual-cursor CC reaches the
    // VR-spill with the opcode missing.
    std::uint16_t vaVrSpillStore;
    // D-ASM-AARCH64-LARGE-FRAME-IMM12: the SCALED imm12 unsigned-offset LDR/STR
    // (`load_u`/`store_u`), the large-frame siblings of the universal `load`/`store`
    // (unscaled imm9). The incoming-stack-arg load/store path picks these over
    // `load`/`store` when the frame offset exceeds the imm9 ±256 reach (a ≥9-fixed-
    // param callee). Optional — only a target with a scaled load/store form declares
    // them (arm64 does; x86_64 does not — its memory forms already carry a disp32).
    // Absent ⇒ field 0; the selection then keeps `load`/`store` and the encoder fails
    // loud on an out-of-imm9 offset (the residual D-ASM-AARCH64-FRAME-OFFSET-BEYOND-
    // IMM12). Resolved via the same optional-handle table (FOLD 2 — NOT a new
    // RegClassOp role; the scaled form is a single-ISA concern, parallel to fstur_q).
    std::uint16_t loadU;
    std::uint16_t storeU;
};

// FC2 Part B: resolve the class-correct opcode for a register-data-
// movement role (the registerClassOps table; GPR → the universal
// mov/load/store bindings). Mirrors the MIR→LIR consumer's fail-loud:
// silently emitting the GPR handle against an FPR ordinal assembles
// valid-looking-but-WRONG bytes (e.g. `mov` on an XMM hwEncoding).
[[nodiscard]] std::optional<std::uint16_t>
classOpHandle(TargetSchema const&  schema,
              LirRegClass          cls,
              RegClassOp           op,
              std::string_view     contextLabel,
              DiagnosticReporter&  reporter) {
    auto const handle = schema.regClassOpOpcode(
        static_cast<TargetRegClass>(static_cast<std::uint8_t>(cls)), op);
    if (!handle.has_value()) {
        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
               DiagnosticSeverity::Error,
               std::format("{}: target schema declares no '{}' operation "
                           "for register class '{}' — declare it in "
                           "`registerClassOps[]`; emitting the GPR "
                           "instruction form would silently mis-encode",
                           contextLabel, regClassOpName(op),
                           targetRegClassName(static_cast<TargetRegClass>(
                               static_cast<std::uint8_t>(cls)))));
    }
    return handle;
}

void emitPrologue(LirBuilder& b, FrameLayout const& layout,
                  TargetSchema const& schema, LirReg sp,
                  std::uint16_t subOp, DiagnosticReporter& reporter,
                  bool& ok) {
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
        // mis-classified as GPR. FC2 Part B: the store MNEMONIC also
        // resolves per class (registerClassOps) — fail loud when the
        // class declares none.
        auto const storeOp = classOpHandle(
            schema, layout.savedRegs[i].regClass(), RegClassOp::Store,
            "callconv: prologue saved-reg store", reporter);
        if (!storeOp.has_value()) { ok = false; return; }
        emitFrameStore(b, *storeOp, layout.savedRegs[i], sp,
                       static_cast<std::int32_t>(base + i * layout.slotSize));
    }
    ok = true;
}

void emitEpilogue(LirBuilder& b, FrameLayout const& layout,
                  TargetSchema const& schema, LirReg sp,
                  std::uint16_t addOp, DiagnosticReporter& reporter,
                  bool& ok) {
    // Reverse the prologue: load saved regs FIRST, then restore SP.
    // Same savedRegAreaOffset() bias as the prologue (mirrored
    // reads — silent miscompile if these diverge).
    std::uint32_t const base = layout.savedRegAreaOffset();
    for (std::size_t i = 0; i < layout.savedRegs.size(); ++i) {
        auto const loadOp = classOpHandle(
            schema, layout.savedRegs[i].regClass(), RegClassOp::Load,
            "callconv: epilogue saved-reg load", reporter);
        if (!loadOp.has_value()) { ok = false; return; }
        emitFrameLoad(b, *loadOp, layout.savedRegs[i], sp,
                      static_cast<std::int32_t>(base + i * layout.slotSize));
    }
    emitSpAdjust(b, addOp, sp, layout.totalFrameSize);
    ok = true;
}

// Resolve a cc register-name reference to a typed `LirReg` (forward decl — the
// definition follows; the variadic spill below uses it to turn argGprs/argFprs
// names into physical LirRegs).
[[nodiscard]] std::optional<LirReg>
resolveCcReg(TargetSchema const& schema, std::string_view name, LirRegClass cls,
             std::string_view contextLabel, DiagnosticReporter& reporter);

// FC12a-core (D-FC12A-VARIADIC-CALLEE) + FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): emit
// the variadic prologue arg-register spill, STRATEGY-DISPATCHED on the CC's
// vaListLayout.
//
//   * SysVRegisterSave (SysV §3.5.7): spill the integer arg regs (rdi..r9,
//     `gpSaveCount` at `gpSlotBytes` stride) then the SSE arg regs (xmm0..7,
//     `fpSaveCount` at `fpSlotBytes` stride, the SSE block AFTER the GPR block) into
//     the register-save-area at `vaRegSaveAreaOffset()`. `va_start` sets
//     reg_save_area = &this zone and reads slots via gp_offset/fp_offset. The SSE
//     block is spilled UNCONDITIONALLY (al-gated skip is anchored
//     D-FC12A-VARIADIC-AL-GATED-SPILL; correctness does not depend on it).
//   * HomogeneousPointer (Win64): spill the FULL integer home register pool
//     (argGprs[0..min(4,argGprs)-1] = rcx/rdx/r8/r9; NOT named-arg-count-bounded —
//     the VARARG home slots must be initialized too) into their HOME slots at the CONTIGUOUS
//     no-shadow base `totalFrameSize + callPushBytes + i*outgoingSlotSize` — the
//     SAME base the va_home_arg_area leaf materializes (BLOCKER-1 congruence), so
//     spill-target == va_arg-read-target. NO register-save-area (vaRegSaveAreaSize
//     stays 0). NO SSE spill: an FP vararg is duplicated into its home GPR slot by
//     the CALLER (the FP-dup in the variadic-call path), and a named FP arg is read
//     by name (never via va_arg), so the callee spills no XMMs here.
//   * Aapcs64DualCursor (FC12c): spill x0..x7 (GR block, 8B each via the class GPR
//     store) then v0..v7 (VR block, 16B each via the 128-bit `fstur_q` = `vaVrSpill
//     Store`) into the callee-local register-save-area at `vaRegSaveAreaOffset()`,
//     ascending index order (x0/v0 at the block head). `va_start`'s NEGATIVE
//     __gr_offs/__vr_offs then address the right slot from the (past-the-block)
//     __gr_top/__vr_top cursors. UNLIKE Win64's home spill, the base is the callee's
//     OWN save zone, not the caller's incoming area.
[[nodiscard]] bool
emitVariadicPrologueSpill(LirBuilder& b, FrameLayout const& layout,
                          TargetSchema const& schema,
                          TargetCallingConvention const& cc, LirReg sp,
                          std::uint16_t vaVrSpillStore, std::uint16_t addOp,
                          DiagnosticReporter& reporter) {
    if (!cc.vaListLayout.has_value()) return true;   // guarded by caller, defensive
    VaListLayout const& vl = *cc.vaListLayout;

    if (vl.strategy == VaListStrategy::HomogeneousPointer) {
        // FC12c: Apple arm64 (variadicUsesOverflowBase) has NO home area — its named
        // args stay in their registers (read via SSA) and every vararg is stacked by
        // the CALLER, so there is NOTHING to spill in the prologue. Win64
        // (variadicUsesOverflowBase=false) spills the integer home register pool below.
        if (vl.variadicUsesOverflowBase) return true;
        // Win64 home spill: store the integer home registers (rcx/rdx/r8/r9) into
        // their home slots at the contiguous no-shadow base. CRITICAL: spill ALL
        // `min(4, argGprs)` of them, NOT just the `namedArgCount` named ones — the
        // VARARG home slots (slots namedArgCount..3) carry varargs the caller passed
        // in registers (rdx/r8/r9), and `va_arg` reads them FROM THE HOME SLOTS, so a
        // named-only spill would leave the vararg slots uninitialized → va_arg reads
        // garbage (the exact bug a named-only bound caused). The named slots
        // (the named ones, not the varargs) are redundantly spilled — harmless (the
        // body reads its named params via the `arg` SSA values from the same
        // registers). This is the standard MSVC variadic prologue (spill the full
        // integer home space). FP varargs ride the matching home GPR via the caller's
        // FP-dup, so spilling the GPRs alone covers both int AND fp varargs — no XMM
        // spill here. The home base is byte-identical to the va_home_arg_area
        // materialization (BLOCKER-1); the congruence pin asserts the two base
        // expressions match. (The va_home base multiplier — the named-arg slot count —
        // rides the VaHomeArgAreaAddr MIR payload, NOT this spill, which
        // unconditionally covers the full integer home pool.)
        std::uint32_t const homeBase =
            layout.totalFrameSize
            + static_cast<std::uint32_t>(cc.callPushBytes);
        auto const gpStore = classOpHandle(schema, LirRegClass::GPR, RegClassOp::Store,
                                           "callconv: Win64 home GPR spill", reporter);
        if (!gpStore.has_value()) return false;
        // The home space holds exactly the integer arg-register pool (4 on Win64).
        std::uint32_t const gpN =
            static_cast<std::uint32_t>(cc.argGprs.size());
        for (std::uint32_t i = 0; i < gpN; ++i) {
            auto const reg = resolveCcReg(schema, cc.argGprs[i], LirRegClass::GPR,
                                          "callconv: Win64 home GPR spill", reporter);
            if (!reg.has_value()) return false;
            emitFrameStore(b, *gpStore, *reg, sp,
                           static_cast<std::int32_t>(
                               homeBase + i * layout.outgoingSlotSize));
        }
        return true;
    }

    if (vl.strategy == VaListStrategy::Aapcs64DualCursor) {
        // ── AAPCS64: spill x0..x7 (GR block) then v0..v7 (VR block) ──
        // Into the callee-local register-save-area at vaRegSaveAreaOffset(), ascending
        // index order so __gr_offs=-(8-fixedGpr)*8 / __vr_offs=-(8-fixedFpr)*16 (added
        // to the past-the-block __gr_top/__vr_top) address the right slot.
        //
        // The save area is the TOPMOST frame zone, so vaRegSaveAreaOffset() + the VR
        // block (up to +192) can EXCEED the AArch64 unscaled STUR/STR `imm9` reach
        // (±256). So materialize the save-area BASE into a scratch register ONCE
        // (`add scratch, sp, #vaRegSaveAreaOffset()`) and spill at SMALL offsets off
        // it (0..176, always within imm9) — the robust ARM64 form that does not depend
        // on the frame staying small. The scratch is a caller-saved GPR that is neither
        // an arg GPR (x0..x7, being spilled) NOR the indirect-result register (x8 — the
        // live incoming sret pointer for a variadic fn that ALSO returns a >16B struct;
        // `read_indirect_result` reads it LATER, in the per-inst loop, so clobbering x8
        // here would silently mis-route the struct result into the callee's own frame).
        // fp/lr (x29/x30) and sp are calleeSaved (not in callerSaved), so the
        // callerSaved iteration never reaches them — only the arg-GPR + IRR exclusions
        // are needed on top of it. The picker therefore lands on x9 (the first genuinely
        // free intra-procedure scratch). At the prologue spill point — immediately after
        // the SP-adjust + saved-reg stores, before any body inst — the only live values
        // are the incoming args (x0..x7) and the incoming x8 sret pointer; every other
        // caller-saved GPR holds nothing.
        if (vaVrSpillStore == 0) {
            report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                   DiagnosticSeverity::Error,
                   "callconv: AAPCS64 variadic prologue VR spill requires the "
                   "128-bit 'fstur_q' opcode (D-FC12C-AAPCS64-VARIADIC-CALLEE)");
            return false;
        }
        // Pick a caller-saved GPR that is neither an arg register being spilled (x0..x7)
        // NOR the indirect-result register (x8 — the live incoming sret pointer; see the
        // block comment above). Config-derived over cc.callerSaved; on AAPCS64/Apple this
        // lands on x9 (the first intra-procedure scratch). Reverting the IRR exclusion
        // makes the picker fall back onto x8 and CLOBBER the sret pointer — silently
        // mis-routing a >16B struct return; the structural pin
        // `Aapcs64VariadicStructReturnPrologueScratchAvoidsX8` is the red-on-disable.
        auto isArgGpr = [&](std::uint16_t ord) {
            for (auto const& name : cc.argGprs)
                if (auto const a = schema.registerByName(name);
                    a.has_value() && *a == ord)
                    return true;
            return false;
        };
        // The IRR (x8) is declared only by CCs with register-based sret (AAPCS64/Apple);
        // its ordinal is the avoid-set's second member. has_value()-guarded so a CC
        // without one (none here, but defensively) adds nothing.
        std::optional<std::uint16_t> const irrOrd =
            cc.indirectResultRegister.has_value()
                ? std::optional<std::uint16_t>(cc.indirectResultRegister->ordinal)
                : std::nullopt;
        std::optional<LirReg> scratch;
        for (std::string_view const name : cc.callerSaved) {
            auto const ord = schema.registerByName(name);
            if (!ord.has_value()) continue;
            auto const* info = schema.registerInfo(*ord);
            if (info == nullptr || info->regClass != TargetRegClass::GPR) continue;
            if (isArgGpr(*ord)) continue;
            if (irrOrd.has_value() && *ord == *irrOrd) continue;
            scratch = makePhysicalReg(*ord, LirRegClass::GPR);
            break;
        }
        if (!scratch.has_value()) {
            report(reporter, DiagnosticCode::L_CcRegLookupFailed,
                   DiagnosticSeverity::Error,
                   "callconv: AAPCS64 variadic prologue spill found no free "
                   "caller-saved scratch GPR for the save-area base "
                   "(D-FC12C-AAPCS64-VARIADIC-CALLEE)");
            return false;
        }
        // scratch = sp + vaRegSaveAreaOffset() (one ADD-imm).
        {
            std::array<LirOperand, 2> addOps{
                LirOperand::makeReg(sp),
                LirOperand::makeImmInt32(
                    static_cast<std::int32_t>(layout.vaRegSaveAreaOffset()))
            };
            b.addInst(addOp, *scratch, addOps);
        }
        // GR block: 8-byte slots via the class GPR store (the SAME 8-byte store the
        // SysV GPR spill uses — x-register width), at [scratch + i*8].
        auto const gpStore = classOpHandle(schema, LirRegClass::GPR, RegClassOp::Store,
                                           "callconv: AAPCS64 variadic GR spill",
                                           reporter);
        if (!gpStore.has_value()) return false;
        std::uint32_t const gpN =
            std::min<std::uint32_t>(vl.gpSaveCount,
                                    static_cast<std::uint32_t>(cc.argGprs.size()));
        for (std::uint32_t i = 0; i < gpN; ++i) {
            auto const reg = resolveCcReg(schema, cc.argGprs[i], LirRegClass::GPR,
                                          "callconv: AAPCS64 variadic GR spill",
                                          reporter);
            if (!reg.has_value()) return false;
            emitFrameStore(b, *gpStore, *reg, *scratch,
                           static_cast<std::int32_t>(i * vl.gpSlotBytes));
        }
        // VR block: 16-byte slots via the 128-bit `fstur_q` (the class store is the
        // 8-byte D-form, which would spill only the low half of a v-register), at
        // [scratch + gpBlock + i*16]. The VR block follows the full GR block.
        std::uint32_t const vrBase = vl.gpSaveCount * vl.gpSlotBytes;
        std::uint32_t const fpN =
            std::min<std::uint32_t>(vl.fpSaveCount,
                                    static_cast<std::uint32_t>(cc.argFprs.size()));
        for (std::uint32_t i = 0; i < fpN; ++i) {
            auto const reg = resolveCcReg(schema, cc.argFprs[i], LirRegClass::FPR,
                                          "callconv: AAPCS64 variadic VR spill",
                                          reporter);
            if (!reg.has_value()) return false;
            emitFrameStore(b, vaVrSpillStore, *reg, *scratch,
                           static_cast<std::int32_t>(vrBase + i * vl.fpSlotBytes));
        }
        return true;
    }

    if (vl.strategy != VaListStrategy::SysVRegisterSave) {
        // Any future un-handled strategy: fail loud rather than silently run the SysV
        // spill on a layout it does not describe.
        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
               DiagnosticSeverity::Error,
               std::format("callconv: variadic prologue spill for va_list strategy "
                           "'{}' is not realized (internal: unknown VaListStrategy)",
                           vaListStrategyName(vl.strategy)));
        return false;
    }

    // ── SysVRegisterSave ──
    std::uint32_t const base = layout.vaRegSaveAreaOffset();

    // Integer arg regs → [sp + base + i*gpSlotBytes]. Spill min(gpSaveCount,
    // argGprs.size()) — the geometry assumes they agree (SysV: 6 == 6), but the min
    // keeps it safe against a misconfigured CC declaring fewer arg regs than slots.
    auto const gpStore = classOpHandle(schema, LirRegClass::GPR, RegClassOp::Store,
                                       "callconv: variadic GPR save spill", reporter);
    if (!gpStore.has_value()) return false;
    std::uint32_t const gpN =
        std::min<std::uint32_t>(vl.gpSaveCount,
                                static_cast<std::uint32_t>(cc.argGprs.size()));
    for (std::uint32_t i = 0; i < gpN; ++i) {
        auto const reg = resolveCcReg(schema, cc.argGprs[i], LirRegClass::GPR,
                                      "callconv: variadic GPR save spill", reporter);
        if (!reg.has_value()) return false;
        emitFrameStore(b, *gpStore, *reg, sp,
                       static_cast<std::int32_t>(base + i * vl.gpSlotBytes));
    }

    // SSE arg regs → [sp + base + gpSaveCount*gpSlotBytes + i*fpSlotBytes] (the SSE
    // block follows the full GPR block, matching va_start's fp_offset = 48 + ...).
    std::uint32_t const fpBase = base + vl.gpSaveCount * vl.gpSlotBytes;
    auto const fpStore = classOpHandle(schema, LirRegClass::FPR, RegClassOp::Store,
                                       "callconv: variadic SSE save spill", reporter);
    if (!fpStore.has_value()) return false;
    std::uint32_t const fpN =
        std::min<std::uint32_t>(vl.fpSaveCount,
                                static_cast<std::uint32_t>(cc.argFprs.size()));
    for (std::uint32_t i = 0; i < fpN; ++i) {
        auto const reg = resolveCcReg(schema, cc.argFprs[i], LirRegClass::FPR,
                                      "callconv: variadic SSE save spill", reporter);
        if (!reg.has_value()) return false;
        emitFrameStore(b, *fpStore, *reg, sp,
                       static_cast<std::int32_t>(fpBase + i * vl.fpSlotBytes));
    }
    return true;
}

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

// Lookup the `ordinal`-th return register of the given class. Slot 0 is the
// primary return register (the scalar / first-eightbyte result); higher slots
// are the additional eightbyte pieces of an in-register struct return (SysV's
// rax+rdx / xmm0+xmm1 — FC7 C1c, D-FC7-SYSV-STRUCT-RETURN-IN-REGS). `ordinal` is
// the PER-CLASS index (GPR and FPR pieces counted separately).
[[nodiscard]] std::optional<LirReg>
returnReg(TargetSchema const&            schema,
          TargetCallingConvention const& cc,
          LirRegClass                    cls,
          std::uint32_t                  ordinal,
          std::string_view               contextLabel,
          DiagnosticReporter&            reporter) {
    auto const& pool = (cls == LirRegClass::FPR) ? cc.returnFprs : cc.returnGprs;
    if (ordinal >= pool.size()) {
        report(reporter, DiagnosticCode::L_CcRegLookupFailed,
               DiagnosticSeverity::Error,
               std::format("{}: calling convention '{}' has only {} {} return "
                           "register(s) but a {} result needs return-register "
                           "ordinal {}",
                           contextLabel, cc.name, pool.size(),
                           (cls == LirRegClass::FPR) ? "FPR" : "GPR",
                           (cls == LirRegClass::FPR) ? "float" : "integer",
                           ordinal));
        return std::nullopt;
    }
    return resolveCcReg(schema, pool[ordinal], cls, contextLabel, reporter);
}

// FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): one `dst <- src` register copy in
// a parallel-move set (the return-register PIECES of a by-value struct return).
struct RegMove { LirReg dst; LirReg src; };

// Pick a caller-saved register of class `cls` not among the registers `moves`
// touches — a free scratch at a call/return boundary (every caller-saved reg is
// dead there except the return registers themselves, which are in the move set).
// Agnostic: reads `cc.callerSaved`, never a hardcoded register. nullopt + loud
// when none is free (would require more live return regs than exist — impossible
// for SysV's ≤2 pieces).
[[nodiscard]] std::optional<LirReg>
pickScratchReg(TargetSchema const& schema, TargetCallingConvention const& cc,
               LirRegClass cls, std::span<RegMove const> moves,
               std::string_view ctx, DiagnosticReporter& reporter) {
    TargetRegClass const want =
        static_cast<TargetRegClass>(static_cast<std::uint8_t>(cls));
    auto involved = [&](std::uint16_t id) {
        for (auto const& m : moves)
            if (m.dst.id == id || m.src.id == id) return true;
        return false;
    };
    for (std::string_view const name : cc.callerSaved) {
        auto const ord = schema.registerByName(name);
        if (!ord.has_value()) continue;
        auto const* info = schema.registerInfo(*ord);
        if (info == nullptr || info->regClass != want) continue;
        if (involved(*ord)) continue;
        return makePhysicalReg(*ord, cls);
    }
    report(reporter, DiagnosticCode::L_MoveCycleUnsupported, DiagnosticSeverity::Error,
           std::format("{}: no free caller-saved scratch register of the piece's "
                       "class to break a return-register move cycle", ctx));
    return std::nullopt;
}

// FC7 C3 (AAPCS64/Apple x8 sret): resolve the cc's indirect-result register to a
// typed `LirReg`. Used by BOTH sides of register-based sret — the callee entry
// `read_indirect_result` materialization and the caller's IRR-reroute of a
// `hasIndirectResult` call. nullopt + loud if the CC declares none (a flagged call
// / ReadIndirectResult reached a CC without an indirectResultRegister — a config or
// HIR→MIR-threading invariant break) or the resolved ordinal is out of range.
// `validate()` guarantees the register is GPR-class; the class is read back from the
// schema register table (never hardcoded) so a future non-GPR IRR resolves correctly.
[[nodiscard]] std::optional<LirReg>
indirectResultReg(TargetSchema const& schema, TargetCallingConvention const& cc,
                  std::string_view ctx, DiagnosticReporter& reporter) {
    if (!cc.indirectResultRegister.has_value()) {
        report(reporter, DiagnosticCode::L_CcRegLookupFailed, DiagnosticSeverity::Error,
               std::format("{}: calling convention '{}' declares no indirect-result "
                           "register, but an x8-sret call/return requires one "
                           "(HIR→MIR set the indirect-result path for a CC whose "
                           "aggregateSretViaHiddenArg is true)", ctx, cc.name));
        return std::nullopt;
    }
    std::uint16_t const ord = cc.indirectResultRegister->ordinal;
    auto const* info = schema.registerInfo(ord);
    if (info == nullptr) {
        report(reporter, DiagnosticCode::L_CcRegLookupFailed, DiagnosticSeverity::Error,
               std::format("{}: cc '{}' indirectResultRegister ordinal {} is out of "
                           "range", ctx, cc.name, ord));
        return std::nullopt;
    }
    return makePhysicalReg(
        ord, static_cast<LirRegClass>(static_cast<std::uint8_t>(info->regClass)));
}

// Emit a set of parallel register copies so every SOURCE is read before its
// register is overwritten. Non-cyclic moves emit in dependency order; a true cycle
// (e.g. the two eightbytes of a {long,long} return landing cross-wise in rax/rdx,
// or a 3-/4-FPR AAPCS64 HFA return cycle — FC7 C3) is broken with a scratch
// register. D-ML7-2.3's arg path only REJECTS cycles; return pieces genuinely need
// the break. The scratch-break linearizes a cycle of ANY length through ONE scratch
// (it redirects all readers of one source to the scratch, freeing that source so the
// progress scan drains the resulting chain), reused across disjoint cycles as `moves`
// shrinks; `pickScratchReg` is the fail-loud backstop when no scratch of the class is
// free (impossible while the move set leaves ≥1 caller-saved reg of that class idle).
[[nodiscard]] bool
emitParallelRegMoves(LirBuilder& b, TargetSchema const& schema,
                     TargetCallingConvention const& cc,
                     std::vector<RegMove> moves, std::string_view ctx,
                     DiagnosticReporter& reporter) {
    std::erase_if(moves, [](RegMove const& m) { return m.dst.id == m.src.id; });
    auto isPendingSrc = [&](std::uint16_t id) {
        for (auto const& m : moves)
            if (m.src.id == id) return true;
        return false;
    };
    while (!moves.empty()) {
        bool progressed = false;
        for (std::size_t i = 0; i < moves.size(); ++i) {
            // Safe to emit now iff nobody still needs to READ moves[i].dst.
            if (isPendingSrc(moves[i].dst.id)) continue;
            auto const mv = classOpHandle(schema, moves[i].dst.regClass(),
                                          RegClassOp::Move, ctx, reporter);
            if (!mv.has_value()) return false;
            emitMov(b, *mv, moves[i].dst, moves[i].src);
            moves.erase(moves.begin() + static_cast<std::ptrdiff_t>(i));
            progressed = true;
            break;
        }
        if (progressed) continue;
        // Only cycles remain. Break ONE cycle per outer iteration; the break
        // generalizes to any cycle length (FC7 C3 raised this from the SysV-only
        // ≤2-piece gate — a 3-/4-FPR HFA return can form a ≥3-cycle): copy one
        // member's source aside, then redirect every reader of that source to the
        // scratch — freeing the source register so a safe move opens up next
        // iteration, and the progress scan drains the rest of the (now linear)
        // chain. Disjoint cycles are handled across successive outer iterations.
        LirReg const cycSrc = moves.front().src;
        auto const scratch = pickScratchReg(schema, cc, cycSrc.regClass(),
                                            moves, ctx, reporter);
        if (!scratch.has_value()) return false;
        auto const mv = classOpHandle(schema, cycSrc.regClass(),
                                      RegClassOp::Move, ctx, reporter);
        if (!mv.has_value()) return false;
        emitMov(b, *mv, *scratch, cycSrc);
        for (auto& m : moves)
            if (m.src.id == cycSrc.id) m.src = *scratch;
    }
    return true;
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
    std::array<Entry, 21> const table{{
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
        // FC7 C1c: optional — only a CC with in-register struct returns emits
        // `ret_piece`. Absent ⇒ field stays 0; the `op == h.retPiece` look-ahead
        // never matches (real opcodes are > 0), so no struct-return capture runs.
        {&OpcodeHandles::retPiece,   "ret_piece",  true},
        // FC7 C3: optional — only a register-based-sret CC (AAPCS64/Apple x8)
        // emits `read_indirect_result`. Absent ⇒ field 0; `op == h.readIndirectResult`
        // never matches. x86_64 declares the opcode for uniformity but never emits it.
        {&OpcodeHandles::readIndirectResult, "read_indirect_result", true},
        // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02):
        // optional — a target without dynamic-import support legitimately
        // omits this opcode. MIR→LIR's separate per-call extern check at
        // `lowerCall` fails loud upstream if a call to a missing-opcode
        // extern is lowered, so the absence here is safe.
        {&OpcodeHandles::callIndirectViaExtern,
         "call_indirect_via_extern", true},
        // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02):
        // optional — a target without body-local allocas (e.g., a
        // shader / WASM target where locals are operand-stack
        // values, not stack slots) legitimately omits both. When
        // present they MUST both resolve: a target declaring
        // `alloca` without `lea` (or vice versa) is a schema
        // misconfiguration. The materialize pass fails loud if it
        // encounters an `alloca` instruction without the `lea`
        // handle resolved.
        {&OpcodeHandles::alloca_,    "alloca",     true},
        {&OpcodeHandles::lea,        "lea",        true},
        // FC12a-core (D-FC12A-VARIADIC-CALLEE): optional — only a CC with a
        // `vaListLayout` emits these. Materialized into `lea [sp + offset]`; both
        // need `lea` (guaranteed present on any register-machine target that also
        // declares them). Absent ⇒ field 0; never matches a real input opcode.
        {&OpcodeHandles::vaRegSaveArea,     "va_reg_save_area",     true},
        {&OpcodeHandles::vaOverflowArgArea, "va_overflow_arg_area", true},
        {&OpcodeHandles::vaHomeArgArea,     "va_home_arg_area",     true},
        {&OpcodeHandles::movqXmmToGpr,      "movq_xmm_to_gpr",      true},
        // FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE): optional — only the AAPCS64 dual-
        // cursor prologue's VR spill emits the 128-bit `fstur_q`. Absent ⇒ field 0;
        // the spill fails loud if a dual-cursor CC reaches the VR-spill without it.
        {&OpcodeHandles::vaVrSpillStore,    "fstur_q",              true},
        // D-ASM-AARCH64-LARGE-FRAME-IMM12: optional — only a target with a scaled
        // unsigned-offset load/store form declares these (arm64). Absent ⇒ field 0;
        // the incoming-stack-arg selection then keeps `load`/`store` (the encoder
        // fails loud on an out-of-imm9 offset — the residual). Fail-loud once HERE,
        // not per-inst (FOLD 2 — the fstur_q/lea optional-handle pattern).
        {&OpcodeHandles::loadU,             "load_u",               true},
        {&OpcodeHandles::storeU,            "store_u",              true},
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
    // D-LK10-ENTRY-ARM64-NONLEAF-LINK-REGISTER (2026-06-08): a calling
    // convention that carries the return address in a LINK REGISTER
    // (AAPCS64 -> x30) instead of pushing it on the stack at the call
    // site (x86_64 `call` -> callPushBytes=8) makes that register live
    // across the ENTIRE body of a NON-LEAF function: every `bl`/`call`
    // overwrites it. Such a function MUST spill the link register in its
    // prologue and reload it before `ret`, or the epilogue `ret` jumps
    // to the clobbered link reg (the address after the last call) rather
    // than the caller — a self-loop whose repeated epilogue `add sp`
    // walks SP off the stack into a guard page (SIGSEGV). The link reg is
    // deliberately ABSENT from `cc.calleeSaved` (it must never be handed
    // to the register allocator as a general callee-save), so it is
    // folded into the saved set HERE, gated on `hasCalls` so leaf
    // functions keep their minimal frame. Fully config-driven: an
    // architecture with no `cc.linkRegister` (x86_64 — return address is
    // on the stack) takes the no-op path; there is no arch/format branch.
    if (hasCalls && cc.linkRegister.has_value()) {
        LirReg const lr =
            makePhysicalReg(cc.linkRegister->ordinal, LirRegClass::GPR);
        bool alreadySaved = false;
        for (LirReg const& r : usedSaved) {
            if (r.isPhysical != 0 && r.id == lr.id) {
                alreadySaved = true;
                break;
            }
        }
        if (!alreadySaved) usedSaved.push_back(lr);
    }
    // D-ML7-2.2: pre-scan call sites for the maximum stack-arg
    // overflow across all calls. The prologue reserves enough
    // outgoing-arg-area bytes for the widest call.
    std::uint32_t const outgoingArgSlots = hasCalls
        ? computeMaxOutgoingStackArgs(src, fn, schema, cc)
        : 0u;
    // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02): pre-
    // scan `alloca` opcodes; the prologue reserves one slotSize-byte
    // slot per body-local declaration. Order-by-scan = order-by-
    // materialize-arm — the materialize loop assigns the same index
    // i to the same alloca as the count pre-scan visited.
    std::vector<std::uint32_t> const localAllocaPayloads =
        functionLocalAllocaPayloads(src, fn, h.alloca_);
    // FC12a/b/c: does this function call va_start? Detected by the presence of ANY
    // va_start base op — `va_reg_save_area` (SysV + AAPCS64), `va_home_arg_area`
    // (Win64), or `va_overflow_arg_area` (Apple arm64). The op-presence test + the
    // CC's has_value() guard keep this fully config-driven + fail-safe: a CC with no
    // vaListLayout declares no op (handles stay 0).
    bool const usesVaStart =
        functionUsesVaStart(src, fn, h.vaRegSaveArea, h.vaHomeArgArea,
                            h.vaOverflowArgArea)
        && cc.vaListLayout.has_value();
    // FC12b/c: a callee-local register-save-area zone is reserved by the strategies
    // that SPILL arg registers into a callee frame zone — SysVRegisterSave (6 GP + 8
    // SSE) AND Aapcs64DualCursor (8 GR + 8 VR = 192B). Win64 + Apple arm64
    // (HomogeneousPointer) spill into the caller's area / not at all, so their zone
    // stays 0 (gated on the strategy, not just presence).
    bool const usesCalleeRegSaveZone =
        usesVaStart
        && (cc.vaListLayout->strategy == VaListStrategy::SysVRegisterSave
            || cc.vaListLayout->strategy == VaListStrategy::Aapcs64DualCursor);
    std::uint32_t const vaRegSaveAreaBytes =
        usesCalleeRegSaveZone ? cc.vaListLayout->regSaveAreaBytes() : 0u;
    auto layoutOpt = computeFrameLayout(alloc, schema, cc,
                                        std::move(usedSaved),
                                        hasCalls, outgoingArgSlots,
                                        localAllocaPayloads,
                                        vaRegSaveAreaBytes,
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

    // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b) + FC7 (D-FC7-MEMBER-ACCESS):
    // a running BYTE offset advanced once per `alloca` instruction in scan
    // order — by `allocaSlotCount(payload) * slotSize`, the SAME formula +
    // scan order `functionLocalAllocaPayloads`/`computeFrameLayout` used at
    // frame-layout time, so each alloca's offset is stable AND its span
    // matches the reserved `localAreaSize` (no neighbour overlap).
    std::uint32_t localAllocaByteOffset = 0;

    // FC7 C1c: `ret_piece` instructions captured by their struct-returning call's
    // look-ahead (they must immediately follow the call). A `ret_piece` reached in
    // the loop WITHOUT being in this set means an optimizer broke that adjacency →
    // fail loud rather than mis-capture.
    std::unordered_set<std::uint32_t> consumedRetPieces;

    for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
        LirBlockId const srcBlock = src.funcBlockAt(fn, bi);
        LirBlockId const dstBlock = srcToDst.at(srcBlock.v);
        b.beginBlock(dstBlock);

        if (bi == 0) {
            bool prologueOk = false;
            emitPrologue(b, outLayout, schema, sp, h.sub, reporter,
                         prologueOk);
            if (!prologueOk) return false;
            // FC12a/b/c: a function that calls va_start spills its arg registers
            // immediately after the prologue's SP-adjust + saved-reg stores (so the
            // save-area / home space, addressed by va_start, points at live values).
            // The spill is strategy-dispatched (SysV register-save / Win64 home /
            // AAPCS64 GR+VR save / Apple no-op); straight-line stores — no CFG.
            if (usesVaStart
                && !emitVariadicPrologueSpill(b, outLayout, schema, cc, sp,
                                              h.vaVrSpillStore, h.add, reporter))
                return false;
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
            // per-class index. HIR→MIR emits each param/struct-piece Arg
            // with a monotonic per-class counter (D-ML7-2.10 ✅ CLOSED by
            // FC7 C1b — the mixed-class latent gap is fixed: a scalar
            // param's payload is now its per-class index, not the param
            // index, so an int-then-float-then-int signature lands the
            // float in xmm0, not xmm1).
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
                    // Register-resident: existing path. FC2 Part B:
                    // the copy mnemonic follows the param's class
                    // (FPR args move via movaps, not the GPR mov).
                    auto const argSrc =
                        argPassingReg(schema, cc, payload, cls,
                                      "materializeOneFunc: arg", reporter);
                    if (!argSrc.has_value()) return false;
                    auto const argMov = classOpHandle(
                        schema, cls, RegClassOp::Move,
                        "materializeOneFunc: arg copy", reporter);
                    if (!argMov.has_value()) return false;
                    maybeMov(b, *argMov, result, *argSrc);
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
                    auto const argLoad = classOpHandle(
                        schema, cls, RegClassOp::Load,
                        "materializeOneFunc: stack-resident arg load",
                        reporter);
                    if (!argLoad.has_value()) return false;
                    // D-ASM-AARCH64-LARGE-FRAME-IMM12: pick the load
                    // mnemonic from the OFFSET VALUE. The unscaled form
                    // (`load`, AArch64 LDUR imm9) reaches only ±256; a
                    // ≥9-fixed-param callee loads its 9th incoming-stack
                    // param at `[sp + totalFrameSize + ...]`, which exceeds
                    // imm9 once the frame (register-save-area + locals)
                    // grows past 255. The scaled form (`load_u`, LDR
                    // imm12-scaled) reaches 4095*accessSize. This MUST be
                    // decided here, not in the encoder: the variant
                    // selector commits on operand KINDS (both forms share
                    // [reg, membase, memoffset]) and cannot inspect the
                    // offset value, and the encoder does not backtrack on
                    // an encode-range failure (§B.1 FORCED). Pure
                    // arithmetic + config-handle lookup — NO arch/cc/format
                    // identity branch: the swap is gated on the resolved
                    // load BEING the universal `load` (so its scaled twin
                    // `h.loadU` applies — an FPR/other-class load keeps its
                    // class form), and `h.loadU` is 0 on a target without a
                    // scaled form (x86_64 — whose memory ops already carry
                    // disp32, so the imm9 path is never hit). The access
                    // size is the scalar stack-arg stride; emitFrameLoad
                    // builds a default-width (64-bit ⇒ 8-byte) load, so the
                    // scale is `outgoingSlotSize`. A frame offset that fits
                    // neither imm9 nor a scaled imm12 (e.g. unaligned, or
                    // beyond 32760) stays fail-loud at the encoder — the
                    // residual D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12.
                    std::uint16_t loadOpcode = *argLoad;
                    bool const fitsImm9 = offset >= -256 && offset <= 255;
                    if (!fitsImm9 && *argLoad == h.load && h.loadU != 0) {
                        std::uint32_t const bytes =
                            std::max(1u, outLayout.outgoingSlotSize);
                        if (offset >= 0
                            && static_cast<std::uint32_t>(offset) % bytes == 0
                            && static_cast<std::uint32_t>(offset) / bytes <= 4095u) {
                            loadOpcode = h.loadU;
                        }
                        // else: leave loadOpcode = *argLoad; the encoder
                        // fails loud (A_ImmediateOperandOutOfRange) — the
                        // residual unencodable-offset case.
                    }
                    emitFrameLoad(b, loadOpcode, result, sp, offset);
                }
                continue;
            }

            // FC7 C3 (AAPCS64/Apple x8 sret): the callee-side `read_indirect_result`
            // materializes at function entry as `mov result, <indirectResultRegister>`
            // — the callee mirror of the register-resident `arg` move (the incoming
            // sret pointer arrives in x8, not an arg register). Class-routed via
            // `classOpHandle` like every other move; the IRR is GPR (validate()
            // enforces it). Only a register-based-sret CC emits it, so `h.read-
            // IndirectResult == 0` (the optional handle) skips this on every other
            // target.
            if (h.readIndirectResult != 0 && op == h.readIndirectResult) {
                if (!result.valid() || result.isPhysical == 0) {
                    report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
                           DiagnosticSeverity::Error,
                           std::format("callconv: read_indirect_result inst {} has "
                                       "no physical-reg result after regalloc",
                                       inst.v));
                    return false;
                }
                auto const irr = indirectResultReg(
                    schema, cc, "materializeOneFunc: read_indirect_result", reporter);
                if (!irr.has_value()) return false;
                auto const mv = classOpHandle(
                    schema, result.regClass(), RegClassOp::Move,
                    "materializeOneFunc: read_indirect_result copy", reporter);
                if (!mv.has_value()) return false;
                maybeMov(b, *mv, result, *irr);
                continue;
            }

            // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a `ret_piece` captures
            // the k-th return register of its struct-returning call. It is
            // CONSUMED by that call's look-ahead (below), which captures all the
            // pieces together as ONE cycle-broken parallel move. Reaching one here
            // unconsumed means an optimizer broke the call→piece adjacency the
            // capture relies on → fail loud, never silently mis-capture a
            // clobbered return register.
            if (h.retPiece != 0 && op == h.retPiece) {
                if (consumedRetPieces.count(inst.v) != 0) continue;
                report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
                       DiagnosticSeverity::Error,
                       std::format("callconv: ret_piece inst {} is not adjacent to "
                                   "its struct-returning call — the return-piece "
                                   "reads must immediately follow the call "
                                   "(D-FC7-SYSV-STRUCT-RETURN-IN-REGS)",
                                   inst.v));
                return false;
            }

            // D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b, 2026-06-02):
            // materialize `alloca` virtual op into `lea result,
            // [sp + localAreaOffset() + i * slotSize]`. Placed next
            // to the `arg` arm because both produce a frame-resident
            // value into a physreg result (vs. the call arm which is
            // an explicit ABI-shaped emission sequence). `h.alloca_`
            // is the (optional) opcode handle; the loader leaves it
            // 0 for targets that don't declare alloca (shaders, WASM
            // — operand-stack ABIs have no body-local stack slots),
            // and `op == 0` never matches a real input opcode.
            // `h.lea` MUST resolve when h.alloca_ does — a schema
            // declaring one without the other is a misconfiguration
            // that we fail loud on at the per-instruction site.
            if (h.alloca_ != 0 && op == h.alloca_) {
                if (h.lea == 0) {
                    report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                           DiagnosticSeverity::Error,
                           std::format("callconv: target schema declares "
                                       "'alloca' opcode but no 'lea' opcode "
                                       "— required to materialize alloca as "
                                       "`lea result, [sp + offset]` per "
                                       "D-CSUBSET-LOCAL-INT-CODEGEN"));
                    return false;
                }
                if (!result.valid() || result.isPhysical == 0) {
                    report(reporter, DiagnosticCode::L_VirtualRegInPostRegalloc,
                           DiagnosticSeverity::Error,
                           std::format("callconv: alloca inst {} has no "
                                       "physical-reg result after regalloc",
                                       inst.v));
                    return false;
                }
                // Assign this alloca its frame offset = the running byte
                // offset (matches functionLocalAllocaPayloads' traversal —
                // shared loop nesting + identical visit order keep the two
                // in sync), then advance by THIS alloca's slot span
                // (`payload` = its byte size, 0 = scalar = 1 slot).
                std::int32_t const offset = static_cast<std::int32_t>(
                    outLayout.localAreaOffset() + localAllocaByteOffset);
                localAllocaByteOffset +=
                    allocaSlotCount(payload, outLayout.slotSize)
                    * outLayout.slotSize;
                emitFrameAddr(b, h.lea, result, sp, offset);
                continue;
            }

            // FC12a-core (D-FC12A-VARIADIC-CALLEE): materialize the two va_list
            // address virtual ops into `lea result, [sp + offset]` (the alloca
            // precedent). The register-save-area is the topmost frame zone
            // (`vaRegSaveAreaOffset()`); the overflow-arg area is the caller's
            // incoming stack args — the SAME geometry the stack-resident `arg` read
            // uses (`totalFrameSize + callPushBytes + shadowSpaceBytes`, i.e. the
            // FIRST incoming overflow slot). `va_arg` then walks forward from there.
            if (h.vaRegSaveArea != 0 && op == h.vaRegSaveArea) {
                if (h.lea == 0 || !result.valid() || result.isPhysical == 0) {
                    report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                           DiagnosticSeverity::Error,
                           "callconv: va_reg_save_area materialization needs 'lea' "
                           "+ a physical-reg result (D-FC12A-VARIADIC-CALLEE)");
                    return false;
                }
                emitFrameAddr(b, h.lea, result, sp,
                              static_cast<std::int32_t>(outLayout.vaRegSaveAreaOffset()));
                continue;
            }
            if (h.vaOverflowArgArea != 0 && op == h.vaOverflowArgArea) {
                if (h.lea == 0 || !result.valid() || result.isPhysical == 0) {
                    report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                           DiagnosticSeverity::Error,
                           "callconv: va_overflow_arg_area materialization needs "
                           "'lea' + a physical-reg result (D-FC12A-VARIADIC-CALLEE)");
                    return false;
                }
                // FC12b (BLOCKER-1) WARNING: this `totalFrameSize + callPushBytes`
                // base ALSO equals the Win64 va_home_arg_area home base — BUT the
                // Win64 home base must NOT include `shadowSpaceBytes` (the home space
                // IS the caller's shadow space, so adding shadow would skip it). The
                // Win64 home is a SEPARATE op (`va_home_arg_area`, materialized below
                // WITHOUT the shadow term). Do NOT remove the shadow term here to
                // "share" with Win64, and do NOT add a shadow term to the Win64 arm
                // without revisiting the entire Win64 va_arg walk — every va_arg
                // would then read one home-slot too far (a silent miscompile the
                // congruence pin in test_lir_callconv guards).
                // FC12-deferral④ (D-FC12A/C-VARIADIC-OVERFLOW-FIXED-STACK-ARGS): the
                // MIR payload is the fixed-stack-arg byte displacement — bytes of named
                // params that overflowed onto the incoming stack, so overflow_arg_area /
                // __stack must skip them to point at the FIRST vararg. 0 for the common
                // case (no fixed-param overflow). The displacement is in BYTES and
                // assumes the incoming-slot size == gpSlotBytes (true SysV/AAPCS64,
                // where MIR baked the count * gpSlotBytes).
                std::uint32_t const fixedStackBytes = payload;
                std::int32_t const ovfOffset = static_cast<std::int32_t>(
                    outLayout.totalFrameSize
                    + static_cast<std::uint32_t>(cc.callPushBytes)
                    + static_cast<std::uint32_t>(cc.shadowSpaceBytes)
                    + fixedStackBytes);
                emitFrameAddr(b, h.lea, result, sp, ovfOffset);
                continue;
            }
            // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE, BLOCKER-1): the Win64
            // HomogeneousPointer va_start base. The named-arg home space
            // (rcx/rdx/r8/r9 slots, = the caller's shadow space) and the 5th+ stack
            // args are CONTIGUOUS from `totalFrameSize + callPushBytes` (NO
            // shadowSpaceBytes — the home space IS the shadow space). `ap` =
            // base + namedArgCount*outgoingSlotSize positions past ALL named args
            // (home OR stack), so the linear va_arg walk crosses home→overflow
            // correctly. The named-arg slot count rides the MIR payload. This base is
            // byte-identical to the prologue home-spill target (emitVariadicPrologue
            // Spill) — the congruence pin asserts that symmetry.
            if (h.vaHomeArgArea != 0 && op == h.vaHomeArgArea) {
                if (h.lea == 0 || !result.valid() || result.isPhysical == 0) {
                    report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                           DiagnosticSeverity::Error,
                           "callconv: va_home_arg_area materialization needs 'lea' "
                           "+ a physical-reg result (D-FC12B-WIN64-VARIADIC-CALLEE)");
                    return false;
                }
                std::uint32_t const namedArgCount = src.instPayload(inst);
                std::int32_t const homeOffset = static_cast<std::int32_t>(
                    outLayout.totalFrameSize
                    + static_cast<std::uint32_t>(cc.callPushBytes)
                    + namedArgCount * outLayout.outgoingSlotSize);
                emitFrameAddr(b, h.lea, result, sp, homeOffset);
                continue;
            }

            // ML7 cycle 2 (+ FC4 c2 indirect): materialize virtual
            // `call` op into the explicit ABI sequence.
            //
            // Input shape (post-regalloc): `result?, call callee, arg0,
            // arg1, ...` — operands are [callee, arg0..argN-1] where
            // callee is a SymbolRef (direct call) OR a physical Reg
            // (indirect call through a function-pointer value — FC4
            // c2), and each arg is a physical register holding the
            // value at the call site. (mir_to_lir keeps the callee at
            // ops[0] in BOTH shapes: it folds a GlobalAddr callee into
            // a SymbolRef and passes any other callee value through as
            // its Reg — `firstArgIdx = calleeIsGlobalAddr ? 1 : 0`.)
            //
            // Output shape (IDENTICAL planning for both callee kinds —
            // arg moves, stack-arg stores, hazard detection, variadic
            // count-reg, result capture are all shared):
            //   mov destArgReg_0, arg0   (per cc.argGprs/argFprs by class)
            //   mov destArgReg_1, arg1
            //   ...
            //   call <callee_symbol>     (single-operand form; matches
            //     | call <callee_reg>     the schema's per-callee-kind
            //                             encoding variant guards
            //                             `["symbol"]` / `["reg"]`)
            //   mov result, returnReg    (only if result is non-void;
            //                             returnReg picked from
            //                             cc.returnGprs[0] / returnFprs[0]
            //                             by result class)
            //
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
            // silently miss a 3rd call-shaped opcode — the silent-
            // failure audit pin. (The indirect form itself landed as a
            // VARIANT of `call`, not a 3rd opcode, exactly so this
            // sharing holds by construction.)
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
                bool const calleeIsSymbol =
                    calleeOp.kind == LirOperandKind::SymbolRef;
                bool const calleeIsReg =
                    calleeOp.kind == LirOperandKind::Reg;
                if (!calleeIsSymbol && !calleeIsReg) {
                    // Fail-loud TOTALITY backstop: a callee operand
                    // kind the materializer does not understand is an
                    // upstream lowering bug — never guess. The code
                    // stays ALIVE for the residual kinds even though
                    // the Reg (indirect) form is now encoded (FC4 c2).
                    report(reporter,
                           DiagnosticCode::L_IndirectCallUnsupported,
                           DiagnosticSeverity::Error,
                           std::format("callconv: call inst {}'s callee "
                                       "operand kind is neither SymbolRef "
                                       "(direct) nor Reg (indirect) — "
                                       "unsupported callee shape",
                                       inst.v));
                    return false;
                }
                if (calleeIsReg && calleeOp.reg.isPhysical == 0) {
                    report(reporter,
                           DiagnosticCode::L_VirtualRegInPostRegalloc,
                           DiagnosticSeverity::Error,
                           std::format("callconv: call inst {}'s Reg "
                                       "callee is not a physical reg "
                                       "after regalloc", inst.v));
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
                // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): a by-value-
                // stack aggregate arg — byte-copy `bytes` from [addr] into the
                // outgoing area at `dstOffset`. Emitted in the stack-store phase
                // (before any register move) so the source `addr` reg is read while
                // still intact. `addr` is the carrier's (already-physical) Reg.
                struct ByValStackCopy {
                    LirReg       addr;
                    std::int32_t dstOffset;  // from THIS fn's SP-post-prologue
                    std::uint32_t bytes;
                };
                std::vector<ArgMove> argMoves;
                std::vector<StackArgStore> stackStores;
                std::vector<ByValStackCopy> byValStackCopies;
                argMoves.reserve(ops.size());
                // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE, PART 5): Win64 passes each FP
                // VARARG in BOTH the XMM and the matching integer (home) register; the
                // callee's va_arg(double) reads the home (GPR) slot. So the CALLER
                // DUPLICATES each register-resident FP vararg into argGprs[slot] via
                // `movq_xmm_to_gpr`. DERIVED from the strategy (no free-floating bool):
                // the dup fires only for a variadic call under HomogeneousPointer.
                // Fixed FP params + overflow (stack) FP args are NOT duplicated.
                bool const isVariadicCall =
                    ::dss::call_payload::isVariadic(payload);
                bool const win64FpDup =
                    isVariadicCall
                    && cc.vaListLayout.has_value()
                    && cc.vaListLayout->strategy
                           == VaListStrategy::HomogeneousPointer;
                std::uint32_t const fixedOperandCnt =
                    ::dss::call_payload::fixedOperandCount(payload);
                // FC12c (D-FC12C-APPLE-ARM64-VARIADIC-CALLEE): Apple arm64 passes EVERY
                // variadic arg (operand past the fixed ones) on the stack regardless of
                // free arg registers. Gated on the CC flag + the call's isVariadic bit,
                // so AAPCS64 + the x86 CCs (flag false) keep register-then-stack
                // placement. The class counters still advance for the named args (they
                // ARE register-placed), so forcing only the vararg region to the stack
                // keeps the named-arg register assignment correct.
                bool const variadicForcesStack =
                    isVariadicCall && cc.variadicArgsAlwaysStack;
                // Each dup: store the FP vararg's home GPR (dest) ← its xmm dest reg
                // (src). Emitted AFTER the arg moves (the xmm holds the value by then)
                // — the home GPR is the vararg slot's GPR, read by no other arg move.
                std::vector<ArgMove> fpDupMoves;
                // FC7 C3 (AAPCS64/Apple x8 sret): a `hasIndirectResult` call
                // PREPENDS the sret pointer at ops[1] (right after the callee at
                // ops[0]). It is routed to the cc's indirect-result register (x8),
                // NOT an arg register — so the real-arg scan starts past it
                // (firstArgIdx == 2) and the IRR move is appended to argMoves below
                // (hazard-checked + emitted with the arg moves). On every other CC
                // (hidden-arg / non-sret) the flag is clear and firstArgIdx == 1.
                bool const hasIrr =
                    ::dss::call_payload::hasIndirectResult(payload);
                std::size_t const firstArgIdx = hasIrr ? 2u : 1u;
                if (hasIrr && ops.size() < 2) {
                    report(reporter, DiagnosticCode::L_UnsupportedLoweringForOpcode,
                           DiagnosticSeverity::Error,
                           std::format("callconv: indirect-result call inst {} has "
                                       "no sret-pointer operand (expected it "
                                       "prepended at operand 1)", inst.v));
                    return false;
                }
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
                std::uint32_t overflowIdx = 0;  // count of stack-arg SLOTS so far
                // FC12a-struct: arg POSITION (advanced once per arg, incl. a by-value-
                // stack aggregate carrier; NOT per raw operand — a carrier is a Reg +
                // a ByValueStackAgg marker, two operands but ONE arg). The forced-
                // vararg boundary + the pre-scan both index by this, so they agree.
                std::uint32_t argRegionIdx = 0;
                for (std::size_t i = firstArgIdx; i < ops.size(); ++i) {
                    LirOperand const& argOp = ops[i];
                    // FC12a-struct: the ByValueStackAgg marker is consumed WITH its
                    // preceding Reg (below) — never on its own. Skip it here.
                    if (argOp.kind == LirOperandKind::ByValueStackAgg) continue;
                    if (argOp.kind != LirOperandKind::Reg
                        || argOp.reg.isPhysical == 0) {
                        report(reporter,
                               DiagnosticCode::L_VirtualRegInPostRegalloc,
                               DiagnosticSeverity::Error,
                               std::format("callconv: call inst {} arg {} "
                                           "is not a physical-reg operand "
                                           "after regalloc", inst.v,
                                           argRegionIdx));
                        return false;
                    }
                    LirReg const srcReg = argOp.reg;
                    LirRegClass const cls = srcReg.regClass();
                    // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): a Reg
                    // immediately FOLLOWED by a ByValueStackAgg marker is the by-value-
                    // stack aggregate carrier. It is passed ENTIRELY in the overflow
                    // area by a byte-copy from its address reg — UNCONDITIONALLY, never
                    // in a register, never split (SysV §3.2.3/§3.5.7). It consumes NO
                    // arg register (the class/slot counters do NOT advance) and reserves
                    // ceil(bytes / slot) overflow slots. This is the force-to-stack
                    // lever the greedy register-then-overflow placement otherwise lacks.
                    if ((i + 1) < ops.size()
                        && ops[i + 1].kind == LirOperandKind::ByValueStackAgg) {
                        std::uint32_t const aggBytes = ops[i + 1].byValueAggBytes;
                        std::int32_t const dstOffset =
                            static_cast<std::int32_t>(
                                static_cast<std::uint32_t>(cc.shadowSpaceBytes)
                                + overflowIdx * outLayout.outgoingSlotSize);
                        byValStackCopies.push_back({srcReg, dstOffset, aggBytes});
                        overflowIdx += byValueStackAggSlots(
                            aggBytes, outLayout.outgoingSlotSize);
                        ++argRegionIdx;
                        continue;
                    }
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
                    // FC12c: a VARARG (operand past the fixed ones) under a CC that
                    // always-stacks varargs is forced to the overflow area even if a
                    // register slot is free. Named args (argRegionIdx < fixedOperandCnt)
                    // are unaffected — they stay register-placed via argIndex<poolSize.
                    bool const forceStack =
                        variadicForcesStack && argRegionIdx >= fixedOperandCnt;
                    if (argIndex < poolSize && !forceStack) {
                        // Register-resident arg: existing path.
                        auto const destReg =
                            argPassingReg(schema, cc, argIndex, cls,
                                          "materializeOneFunc: call", reporter);
                        if (!destReg.has_value()) return false;
                        argMoves.push_back({*destReg, srcReg});
                        // FC12b PART 5: a register-resident FP VARARG under Win64 also
                        // duplicates into its home integer reg argGprs[slot]. Win64 is
                        // slot-aligned so `argIndex` IS the slot index; argGprs[slot]
                        // pairs with argFprs[slot]. Only the VARARG region (operand
                        // index past the fixed operands) is duplicated.
                        if (win64FpDup && cls == LirRegClass::FPR
                            && argRegionIdx >= fixedOperandCnt
                            && argIndex
                                   < static_cast<std::uint32_t>(cc.argGprs.size())) {
                            auto const homeGpr = resolveCcReg(
                                schema, cc.argGprs[argIndex], LirRegClass::GPR,
                                "materializeOneFunc: Win64 FP-vararg dup", reporter);
                            if (!homeGpr.has_value()) return false;
                            // dest = home GPR, src = the xmm dest (filled by the arg
                            // move). Stored in fpDupMoves; emitted after arg moves.
                            fpDupMoves.push_back({*homeGpr, *destReg});
                        }
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
                    ++argRegionIdx;   // this arg position is done (one per arg)
                }
                // FC7 C3: route the prepended sret pointer (ops[1]) to the cc's
                // indirect-result register. APPENDED to argMoves (last) so it joins
                // the SAME parallel-move hazard analysis + in-order emit as the arg
                // moves: an arg that regalloc parked in the IRR (x8) is read by its
                // own EARLIER arg-move before this LATER move overwrites x8 (safe);
                // a genuine cross-dependency (the sret ptr parked in an arg-dest reg
                // an arg-move overwrites, or vice-versa) trips the same loud
                // L_MoveCycleUnsupported the arg path already uses (D-ML7-2.3) —
                // never a silent clobber. R is a real Call operand ⇒ regalloc keeps
                // it live to here (no post-regalloc dangling).
                if (hasIrr) {
                    auto const irr = indirectResultReg(
                        schema, cc, "materializeOneFunc: call indirect-result",
                        reporter);
                    if (!irr.has_value()) return false;
                    LirOperand const& sretOp = ops[1];
                    if (sretOp.kind != LirOperandKind::Reg
                        || sretOp.reg.isPhysical == 0) {
                        report(reporter,
                               DiagnosticCode::L_VirtualRegInPostRegalloc,
                               DiagnosticSeverity::Error,
                               std::format("callconv: indirect-result call inst {} "
                                           "sret-pointer operand is not a physical "
                                           "reg after regalloc", inst.v));
                        return false;
                    }
                    argMoves.push_back({*irr, sretOp.reg});
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
                // FC4 c2 indirect-callee backstop (the red-on-disable
                // lever for the regalloc-tier rules): a Reg callee that
                // regalloc parked in one of THIS call's arg-passing
                // destination registers (or in the cc's variadic
                // vector-count register on a variadic call) would be
                // OVERWRITTEN by the moves emitted below, and the call
                // would jump THROUGH AN ARGUMENT VALUE — silent
                // garbage. Two upstream rules make this unreachable:
                // the allocator excludes argGprs ∪ argFprs (+ countReg)
                // from any indirect-callee range (lir_regalloc.cpp),
                // and the spill-reload scratch filter keeps a SPILLED
                // callee's reload scratch out of the same set
                // (lir_rewrite.cpp). This check converts any future
                // regression of either rule into a loud compile error.
                // Same-reg moves are skipped: `maybeMov` emits nothing
                // for dest == src, so they cannot clobber.
                if (calleeIsReg) {
                    for (std::size_t i = 0; i < argMoves.size(); ++i) {
                        if (argMoves[i].dest.id == argMoves[i].src.id) {
                            continue;  // no mov emitted — no clobber
                        }
                        if (argMoves[i].dest.id == calleeOp.reg.id) {
                            report(reporter,
                                   DiagnosticCode::
                                       L_IndirectCalleeClobberedByArgSetup,
                                   DiagnosticSeverity::Error,
                                   std::format(
                                       "callconv: call inst {}'s Reg callee "
                                       "(reg #{}) is the destination of its "
                                       "own arg-passing move {} — the move "
                                       "would overwrite the callee before "
                                       "the call consumes it (the regalloc "
                                       "indirect-callee arg-reg exclusion / "
                                       "spill-reload scratch filter "
                                       "invariant was violated upstream)",
                                       inst.v,
                                       static_cast<unsigned>(calleeOp.reg.id),
                                       i));
                            return false;
                        }
                    }
                    if (::dss::call_payload::isVariadic(payload)
                        && cc.variadicVectorCountReg.has_value()
                        && cc.variadicVectorCountReg->ordinal
                               == calleeOp.reg.id) {
                        report(reporter,
                               DiagnosticCode::
                                   L_IndirectCalleeClobberedByArgSetup,
                               DiagnosticSeverity::Error,
                               std::format(
                                   "callconv: variadic call inst {}'s Reg "
                                   "callee (reg #{}) is the cc's variadic "
                                   "vector-count register — the count-reg "
                                   "set below would overwrite the callee "
                                   "before the call consumes it (regalloc "
                                   "indirect-callee exclusion invariant "
                                   "violated upstream)",
                                   inst.v,
                                   static_cast<unsigned>(calleeOp.reg.id)));
                        return false;
                    }
                    // FC12b PART 5: a Win64 FP-vararg dup writes a home GPR
                    // (argGprs[slot]); if an indirect callee was parked there it would
                    // be clobbered before the call. The regalloc indirect-callee
                    // arg-reg exclusion already keeps the callee out of argGprs, so
                    // this is a backstop converting any regression into a loud error
                    // (never a silent jump-through-a-duplicated-vararg).
                    for (auto const& m : fpDupMoves) {
                        if (m.dest.id == calleeOp.reg.id) {
                            report(reporter,
                                   DiagnosticCode::
                                       L_IndirectCalleeClobberedByArgSetup,
                                   DiagnosticSeverity::Error,
                                   std::format(
                                       "callconv: Win64 variadic call inst {}'s Reg "
                                       "callee (reg #{}) is the home integer register "
                                       "of an FP vararg — the FP-dup would overwrite "
                                       "the callee before the call consumes it "
                                       "(regalloc indirect-callee arg-reg exclusion "
                                       "invariant violated upstream)",
                                       inst.v,
                                       static_cast<unsigned>(calleeOp.reg.id)));
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
                    // FC2 Part B: stack-arg stores resolve the store
                    // mnemonic from the VALUE's class.
                    auto const stkStore = classOpHandle(
                        schema, s.src.regClass(), RegClassOp::Store,
                        "materializeOneFunc: call stack-arg store",
                        reporter);
                    if (!stkStore.has_value()) return false;
                    emitFrameStore(b, *stkStore, s.src, sp, s.offset);
                }
                // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): emit the by-
                // value-stack aggregate byte-copies — ALSO in the stack-store phase
                // (before any register move), so each carrier's source ADDRESS reg is
                // read while still intact. Each copy reads chunks from [addr + off] and
                // writes them to the outgoing area at [sp + dstOffset + off], via a
                // caller-saved scratch GPR. The scratch must avoid: SP, the indirect
                // callee reg, the variadic vector-count reg, AND every carrier's
                // address reg (a later carrier's address must survive an earlier copy).
                // GPR-chunked (load/store class GPR) — the byte size rounds to whole
                // outgoing slots, so the copy can over-read into the temp's own padding
                // (the temp is a 16-rounded freshAggregateTemp) but never past it.
                if (!byValStackCopies.empty()) {
                    auto const gpLoad = classOpHandle(
                        schema, LirRegClass::GPR, RegClassOp::Load,
                        "materializeOneFunc: by-value-stack agg copy load", reporter);
                    auto const gpStore = classOpHandle(
                        schema, LirRegClass::GPR, RegClassOp::Store,
                        "materializeOneFunc: by-value-stack agg copy store", reporter);
                    if (!gpLoad.has_value() || !gpStore.has_value()) return false;
                    std::uint32_t const chunk =
                        widthForClass(schema, LirRegClass::GPR);
                    if (chunk == 0) {
                        report(reporter, DiagnosticCode::L_CcRegLookupFailed,
                               DiagnosticSeverity::Error,
                               "callconv: by-value-stack aggregate copy requires a "
                               "nonzero GPR width (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT)");
                        return false;
                    }
                    // FOLD (adversarial-review BLOCKER-3, silent miscompile): the
                    // byte-copies are emitted HERE, in the stack-store phase, BEFORE the
                    // register arg-moves (`argMoves`, below) and the Win64 FP-vararg
                    // duplications (`fpDupMoves`, below) read their `.src`. So the scratch
                    // must additionally avoid every register those LATER moves still read
                    // as a SOURCE — otherwise, if regalloc parked an outgoing scalar arg
                    // VALUE in the chosen scratch, the byte-copy would clobber it before
                    // its `mov dest, src` could read it, and that argument would silently
                    // receive the struct's bytes. The sret indirect-result source is
                    // ALREADY appended to `argMoves` above (so iterating argMove sources
                    // covers it); `fpDupMove` sources are XMM/FPR regs (never GPR
                    // candidates) but are included for completeness.
                    //
                    // NOTE the avoid-set is the precise SOURCE set, NOT the whole
                    // `cc.argGprs` pool: an arg-move's DEST register (e.g. an argGpr that
                    // only RECEIVES a value) is written by that move AFTER this copy
                    // finishes storing to memory, so the copy's transient write to it is
                    // already dead — it is safe scratch. Blanket-rejecting all argGprs
                    // (the lir_rewrite collectAllocatable precedent, which guards a
                    // DIFFERENT mechanism: a spill-reload that lands BETWEEN the
                    // not-yet-emitted arg moves) would here falsely fail-loud on a valid
                    // register-exhaustion call that legitimately fills every argGpr as a
                    // DEST (see examples/c-subset/varargs_struct_split: 1 fixed + 5 longs
                    // exhaust rdi..r9, all dests) — so we exclude only argGprs that are
                    // ALSO a live source, which `liveArgSrc` already captures.
                    std::unordered_set<std::uint16_t> liveArgSrc;
                    for (auto const& m : argMoves)
                        if (m.src.isPhysical != 0) liveArgSrc.insert(m.src.id);
                    for (auto const& m : fpDupMoves)
                        if (m.src.isPhysical != 0) liveArgSrc.insert(m.src.id);
                    // Pick a caller-saved GPR scratch avoiding the hazard set above.
                    std::optional<LirReg> scratch;
                    for (std::string_view const name : cc.callerSaved) {
                        auto const ord = schema.registerByName(name);
                        if (!ord.has_value()) continue;
                        auto const* rinfo = schema.registerInfo(*ord);
                        if (rinfo == nullptr
                            || rinfo->regClass != TargetRegClass::GPR) continue;
                        bool clash = false;
                        if (calleeIsReg && *ord == calleeOp.reg.id) clash = true;
                        if (!clash
                            && ::dss::call_payload::isVariadic(payload)
                            && cc.variadicVectorCountReg.has_value()
                            && cc.variadicVectorCountReg->ordinal == *ord)
                            clash = true;
                        if (!clash)
                            for (auto const& cpy : byValStackCopies)
                                if (cpy.addr.id == *ord) { clash = true; break; }
                        // FOLD: reject any reg a later arg-move/FP-dup source reads
                        // (the silent arg-clobber avoid-set).
                        if (!clash && liveArgSrc.contains(*ord)) clash = true;
                        if (clash) continue;
                        scratch = makePhysicalReg(*ord, LirRegClass::GPR);
                        break;
                    }
                    if (!scratch.has_value()) {
                        report(reporter, DiagnosticCode::L_CcRegLookupFailed,
                               DiagnosticSeverity::Error,
                               std::format("callconv: call inst {} found no free caller-"
                                           "saved scratch GPR for a by-value-stack "
                                           "aggregate copy "
                                           "(D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT)",
                                           inst.v));
                        return false;
                    }
                    for (auto const& cpy : byValStackCopies) {
                        // Copy ceil(bytes/chunk)*chunk bytes in `chunk`-wide steps. The
                        // dst slots were reserved as whole outgoing slots (= chunk), so
                        // the tail step writes a full slot (reading the temp's padding)
                        // — bounded by the 16-rounded temp + the reserved slot span.
                        std::uint32_t off = 0;
                        for (; off < cpy.bytes; off += chunk) {
                            emitFrameLoad(b, *gpLoad, *scratch, cpy.addr,
                                          static_cast<std::int32_t>(off));
                            emitFrameStore(b, *gpStore, *scratch, sp,
                                           cpy.dstOffset + static_cast<std::int32_t>(off));
                        }
                    }
                }
                // Cycle-free — emit register moves in order. FC2
                // Part B: each move's mnemonic follows its class
                // (FPR arg-passing moves use movaps).
                for (auto const& m : argMoves) {
                    auto const argMov = classOpHandle(
                        schema, m.dest.regClass(), RegClassOp::Move,
                        "materializeOneFunc: call arg-passing move",
                        reporter);
                    if (!argMov.has_value()) return false;
                    maybeMov(b, *argMov, m.dest, m.src);
                }
                // FC12b PART 5: emit the Win64 FP-vararg GPR duplications AFTER the
                // arg moves (the xmm dest now holds the value). Each is `movq_xmm_to_
                // gpr homeGpr, xmm` — a cross-class bit-copy of the FP vararg into its
                // home integer register so the callee's va_arg(double) reads it from
                // the home slot. The home GPR is the vararg slot's GPR (read by no arg
                // move), so emitting last is safe. Fail loud if the opcode is missing.
                if (!fpDupMoves.empty()) {
                    if (h.movqXmmToGpr == 0) {
                        report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
                               DiagnosticSeverity::Error,
                               std::format("callconv: Win64 variadic call inst {} needs "
                                           "'movq_xmm_to_gpr' to duplicate an FP vararg "
                                           "into its home integer register, but the "
                                           "target schema does not declare it "
                                           "(D-FC12B-WIN64-VARIADIC-CALLEE)", inst.v));
                        return false;
                    }
                    for (auto const& m : fpDupMoves) {
                        // dest = home GPR (result slot), src = the xmm (wired operand).
                        std::array<LirOperand, 1> dupOps{LirOperand::makeReg(m.src)};
                        b.addInst(h.movqXmmToGpr, m.dest, dupOps);
                    }
                }
                // D-LANG-VARIADIC (step 13.4): when the call is
                // variadic AND the cc declares a vector-arg count
                // register, emit `mov <countReg>, <count>` per
                // SysV §3.5.7 (see `variadicVectorCountReg`
                // docblock in target_schema.hpp). Emitted AFTER
                // arg-passing moves + stack-arg stores: the cc's
                // countReg (e.g. SysV `rax`) is NOT in the arg-
                // passing GPR pool, but an indirect-call callee or
                // a parallel-copy serializer could use it as a
                // scratch — emitting last guarantees no later code
                // reads or writes it before the call consumes it.
                // (This comment PREDICTED the FC4 c2 indirect-call
                // collision: a variadic indirect call's Reg callee
                // must also avoid the countReg — the regalloc
                // exclusion handles it, and the
                // L_IndirectCalleeClobberedByArgSetup backstop above
                // converts any regression into a loud error.)
                if (::dss::call_payload::isVariadic(payload)
                    && cc.variadicVectorCountReg.has_value()) {
                    std::uint32_t const fixedCount =
                        ::dss::call_payload::fixedOperandCount(payload);
                    std::uint32_t vectorArgsInVararg = 0;
                    for (std::size_t i = firstArgIdx; i < ops.size(); ++i) {
                        // ops[0] is the callee; an x8-sret call also has the sret
                        // pointer at ops[1] (firstArgIdx==2) — skip it so the
                        // operand index counts only arg-region operands. `fixedCount`
                        // is in OPERAND units (a by-value struct fixed param expands
                        // to several scalar register-piece operands — FC12a-struct),
                        // so this boundary is the count of operands the FIXED params
                        // produced, not the param count.
                        std::size_t const argIdx = i - firstArgIdx;
                        if (argIdx < fixedCount) continue;
                        // FC12a-struct: a by-value-stack aggregate is a (Reg,
                        // ByValueStackAgg) pair — the GPR-class address Reg fails the
                        // FPR test below, and the marker is non-Reg, so NEITHER inflates
                        // the SSE/AL count (a by-value MEMORY-class struct vararg has no
                        // SSE register pieces — SysV §3.5.7). No special-case needed.
                        if (ops[i].kind != LirOperandKind::Reg) continue;
                        if (ops[i].reg.regClass() == LirRegClass::FPR) {
                            ++vectorArgsInVararg;
                        }
                    }
                    // D-LANG-VARIADIC (step 13.4) post-fold (type-design
                    // analyzer rec): derive the countReg's class from
                    // the SCHEMA's register-table entry, NOT a
                    // hardcoded LirRegClass::GPR. SysV's rax is GPR;
                    // a hypothetical future ABI that chose an XMM
                    // register would have its class threaded through
                    // automatically. Closes silent-failure surface
                    // where a non-GPR cc.variadicVectorCountReg would
                    // silently misclass the emitted vreg.
                    auto const* regInfo = schema.registerInfo(
                        cc.variadicVectorCountReg->ordinal);
                    if (regInfo == nullptr) {
                        report(reporter,
                               DiagnosticCode::L_CcRegLookupFailed,
                               DiagnosticSeverity::Error,
                               std::format("callconv: cc '{}' "
                                           "variadicVectorCountReg ordinal "
                                           "{} is out of range",
                                           cc.name,
                                           cc.variadicVectorCountReg->ordinal));
                        return false;
                    }
                    LirReg const countReg = makePhysicalReg(
                        cc.variadicVectorCountReg->ordinal,
                        static_cast<LirRegClass>(
                            static_cast<std::uint8_t>(regInfo->regClass)));
                    // FC2 Part B: class-routed like every other move
                    // (the countReg is GPR on every shipped ABI; a
                    // hypothetical FPR countReg resolves its class's
                    // move and the imm operand then fails loud at
                    // encode — no silent GPR fallback either way).
                    auto const countMov = classOpHandle(
                        schema, countReg.regClass(), RegClassOp::Move,
                        "materializeOneFunc: variadic count-reg set",
                        reporter);
                    if (!countMov.has_value()) return false;
                    std::array<LirOperand, 1> immOps{
                        LirOperand::makeImmInt32(
                            static_cast<std::int32_t>(vectorArgsInVararg))};
                    b.addInst(*countMov, countReg, immOps);
                }
                // Emit the call instruction. Pass through the input
                // opcode (h.call OR h.callIndirectViaExtern) so the
                // assembler picks the right encoding. Single-operand
                // form for every shape: a SymbolRef callee matches the
                // schema variant guard `["symbol"]` (direct E8 / BL /
                // FF 15-via-extern), a Reg callee matches `["reg"]`
                // (indirect FF /2 / BLR — FC4 c2).
                std::array<LirOperand, 1> callOps{calleeOp};
                b.addInst(op, InvalidLirReg, callOps,
                          payload, src.instFlags(inst));
                // Capture the call's return register(s) into their result
                // vreg(s). For a scalar call that is a single move of the primary
                // return register. For a by-value struct return (FC7 C1c) the
                // call's result is eightbyte PIECE 0 and the contiguous
                // `ret_piece` insts HIR→MIR emits right after the call carry
                // pieces 1..N-1 (each with its per-class return ordinal as
                // payload); all are captured as ONE cycle-broken parallel move so
                // a cross-wise piece landing (e.g. {long,long} in rax/rdx) can't
                // clobber a register another piece still needs.
                std::vector<RegMove> retMoves;
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
                    auto const rr = returnReg(schema, cc, result.regClass(), 0,
                                              "materializeOneFunc: call result",
                                              reporter);
                    if (!rr.has_value()) return false;
                    retMoves.push_back({result, *rr});
                }
                if (h.retPiece != 0) {
                    for (std::uint32_t j = i + 1; j < instN; ++j) {
                        LirInstId const rp = src.blockInstAt(srcBlock, j);
                        if (src.instOpcode(rp) != h.retPiece) break;
                        LirReg const rpRes = src.instResult(rp);
                        if (!rpRes.valid() || rpRes.isPhysical == 0) {
                            report(reporter,
                                   DiagnosticCode::L_VirtualRegInPostRegalloc,
                                   DiagnosticSeverity::Error,
                                   std::format("callconv: ret_piece inst {} has no "
                                               "physical-reg result after regalloc",
                                               rp.v));
                            return false;
                        }
                        auto const rr =
                            returnReg(schema, cc, rpRes.regClass(),
                                      src.instPayload(rp),
                                      "materializeOneFunc: ret_piece", reporter);
                        if (!rr.has_value()) return false;
                        retMoves.push_back({rpRes, *rr});
                        consumedRetPieces.insert(rp.v);
                    }
                }
                if (!retMoves.empty()
                    && !emitParallelRegMoves(
                           b, schema, cc, std::move(retMoves),
                           "materializeOneFunc: call-result capture", reporter)) {
                    return false;
                }
                continue;
            }

            // Materialize frame_load / frame_store. FC2 Part B: spill
            // reloads/stores resolve their mnemonic from the spilled
            // value's register class — an FPR spill must never round-
            // trip through the GPR mov (silent 8-byte mis-encode).
            if (op == h.frameLoad) {
                LirSpillSlot const slot{payload};
                std::int32_t const offset = static_cast<std::int32_t>(
                    outLayout.spillAreaOffset() + (slot.v - 1u) * slotSize);
                auto const spillLoad = classOpHandle(
                    schema, result.regClass(), RegClassOp::Load,
                    "materializeOneFunc: spill reload", reporter);
                if (!spillLoad.has_value()) return false;
                emitFrameLoad(b, *spillLoad, result, sp, offset);
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
                auto const spillStore = classOpHandle(
                    schema, ops[0].reg.regClass(), RegClassOp::Store,
                    "materializeOneFunc: spill store", reporter);
                if (!spillStore.has_value()) return false;
                emitFrameStore(b, *spillStore, ops[0].reg, sp, offset);
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
                        // FC7 C1c: a by-value struct return carries N eightbyte
                        // pieces (SysV ≤16B → 2); a scalar / sret-pointer return
                        // carries 1. Move each piece into its PER-CLASS return
                        // register (GPR/FPR counted separately — returns are
                        // per-class on every ABI, unlike slot-aligned args), as
                        // ONE cycle-broken parallel move so cross-wise pieces
                        // (e.g. a {long,long} landing rax↔rdx) can't clobber.
                        std::uint32_t gprRet = 0;
                        std::uint32_t fprRet = 0;
                        std::vector<RegMove> retMoves;
                        retMoves.reserve(ops.size());
                        for (auto const& o : ops) {
                            if (o.kind != LirOperandKind::Reg
                                || o.reg.isPhysical == 0) {
                                report(reporter,
                                       DiagnosticCode::L_VirtualRegInPostRegalloc,
                                       DiagnosticSeverity::Error,
                                       std::format("callconv: ret inst {} has "
                                                   "non-physical-reg operand "
                                                   "after regalloc", inst.v));
                                return false;
                            }
                            LirRegClass const cls = o.reg.regClass();
                            std::uint32_t const ord =
                                (cls == LirRegClass::FPR) ? fprRet++ : gprRet++;
                            auto const rr =
                                returnReg(schema, cc, cls, ord,
                                          "materializeOneFunc: ret", reporter);
                            if (!rr.has_value()) return false;
                            retMoves.push_back({*rr, o.reg});
                        }
                        if (!emitParallelRegMoves(
                                b, schema, cc, std::move(retMoves),
                                "materializeOneFunc: ret-value", reporter)) {
                            return false;
                        }
                        // Strip the operands from the ret — the values are now in
                        // the cc's return registers.
                        newOps.clear();
                    }
                    // Emit epilogue BEFORE the return.
                    bool epilogueOk = false;
                    emitEpilogue(b, outLayout, schema, sp, h.add, reporter,
                                 epilogueOk);
                    if (!epilogueOk) return false;
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
    // D-CSUBSET-BITFIELD-WIDE-UNIT: carry the wide-literal pool across
    // the rebuild (LiteralIndex operands reference it by index).
    lir_pass_util::copyLiteralPool(src, b);
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
