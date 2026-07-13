#pragma once

#include <cstdint>
#include <string_view>

// MIR opcode vocabulary (ML1). The MIR is the language-NEUTRAL mid-level IR:
// SSA over a CFG, with structured-CF markers preserved on blocks (plan 12 §2.2).
// Every source language has already been lowered through HIR, and every
// language-EXTENSION type is resolved to the concrete core lattice at the
// HIR→MIR boundary, so `MirOpcode` is a CLOSED enum — there is no extension
// registry (unlike HirKind/HirOpKind). New opcodes are added here additively as
// later phases need them; the enum is the single vocabulary.
//
// FUSED value model: a non-void instruction IS its SSA value (its MirInstId
// identifies the value it produces); operands are references to defining
// instruction ids. Values that don't originate from an ordinary computation —
// function parameters, literals, function/global addresses — are themselves
// value-producing opcodes (`Arg`/`Const`/`GlobalAddr`), so a `Call`'s callee and
// every operand is uniformly "the value defined by some instruction". `Phi`
// joins values at a CFG merge; its incoming (value, predecessor-block) pairs live
// in the module's phi pool, not the general operand pool.

namespace dss {

enum class MirOpcode : std::uint16_t {
    // Slot-0 sentinel default. A default-constructed MirInst must never read as a
    // real operation; `Invalid` carries an impossible arity so any accidental use
    // trips the verifier (mirrors HIR's `Error`-as-default discipline).
    Invalid,

    // ── value origins (the fused model's non-computed values) ──
    Arg,         // function parameter value; payload = parameter index
    Const,       // literal value;            payload = MirLiteralPool index
    GlobalAddr,  // address of a function/global as a value; payload = SymbolId.v
    // D-CSUBSET-COMPUTED-GOTO: the runtime address of a basic block, as a value
    // (the GNU `&&label`). payload = the target MirBlockId.v; result = a pointer
    // (void*). A pure value origin like GlobalAddr (no operands, no side effect) —
    // CSE-safe (the same block's address is one value). Codegen materializes it as
    // the address of a synthetic per-block symbol (mir_to_lir mints + emits a `lea`
    // / adrp+add). The PRESENCE of a BlockAddress(b) is ALSO the canonical mark
    // that block `b` is ADDRESS-TAKEN (Mir::isBlockAddressTaken scans for these),
    // so reachability / SimplifyCfg / the block-symbol emit all read one source.
    BlockAddress,

    // ── integer arithmetic ──
    Add, Sub, Mul, SDiv, UDiv, SMod, UMod, Neg,
    // c103 (D-CSUBSET-INTRINSIC-UMULH): unsigned MUL-HIGH -- the high 64 bits of
    // the u64*u64 128-bit product. The `__umulh` builtin (sqlite3Multiply128/160)
    // lowers to this; x86 `mul r/m64` captures RDX, arm64 native `umulh`. Pure +
    // commutative (high(a*b) == high(b*a)), like Mul.
    UMulH,
    // c104 (D-CSUBSET-INTRINSIC-ATOMIC-CAS): atomic compare-and-swap -- operands
    // [ptr, comparand, newval], result = the ORIGINAL value at *ptr; iff
    // original==comparand the newval is stored, ATOMICALLY (seq-cst-ish: x86
    // `lock cmpxchg` is a full barrier; arm64 LDAXR/STLXR is acquire-release --
    // the correct lowering for Win32 InterlockedCompareExchange semantics).
    // hasSideEffects=TRUE (a store): DCE keeps it live even when the result is
    // unused, CSE never dedups two CASes, LICM never hoists one. NOT commutative.
    AtomicCas,
    // c113 (D-CSUBSET-INTRINSIC-BARRIER): _ReadWriteBarrier -- an MSVC COMPILER
    // reordering fence. ZERO operands, produces NO value (R::None), emits NO
    // runtime instruction; hasSideEffects=TRUE so DCE keeps it and CSE/LICM
    // never move IT, and it is in the `opcodeClobbersMemory` positive list so
    // the CSE/LICM clobber walk treats it as a full memory clobber -- no
    // Load/Store is reordered ACROSS it. A pure compile-time ordering
    // constraint -- identical on every target/format.
    CompilerBarrier,
    // ── floating-point arithmetic ──
    FAdd, FSub, FMul, FDiv, FNeg,
    // ── bitwise ──
    And, Or, Xor, Shl, LShr, AShr, Not,
    // ── integer comparison (result = Bool/i1) ──
    ICmpEq, ICmpNe, ICmpSlt, ICmpSle, ICmpSgt, ICmpSge,
    ICmpUlt, ICmpUle, ICmpUgt, ICmpUge,
    // ── floating-point comparison (ordered O* / unordered U*) ──
    FCmpOeq, FCmpOne, FCmpOlt, FCmpOle, FCmpOgt, FCmpOge,
    FCmpUeq, FCmpUne, FCmpUlt, FCmpUle, FCmpUgt, FCmpUge,
    // ── memory ──
    Alloca, Load, Store, Gep,
    // ── first-class aggregates (D5.6) ──
    // ExtractValue: read a field/element FROM an aggregate VALUE by
    // statically-known index. operands = [aggregate, idx0, idx1, ...]
    // where each `idx_k` is a Const-i32 MirInstId (Gep-shaped). Result
    // = the element's type. Distinct from `Load` — operates on an
    // in-register aggregate value, not memory.
    ExtractValue,
    // InsertValue: produce a NEW aggregate VALUE by replacing one
    // field/element of `aggregate` with `value`. operands = [aggregate,
    // value, idx0, idx1, ...] — same Const-i32 path convention. Result
    // = the aggregate's type (same shape, one slot replaced).
    InsertValue,
    // ── casts ──
    Trunc, SExt, ZExt, FPTrunc, FPExt, Bitcast,
    IntToPtr, PtrToInt, FPToSI, FPToUI, SIToFP, UIToFP,
    // ── calls ──
    Call,          // operands [callee, args...]; result Optional (void ⇒ no value)
    IntrinsicCall, // operands [args...]; payload = intrinsic id; result Optional
    // The k-th return-register piece of a preceding struct-returning Call
    // (FC7 C1c, D-FC7-SYSV-STRUCT-RETURN-IN-REGS). operand [call] anchors it to
    // its call (ordering + no cross-call CSE + DCE-safe); payload = the PER-CLASS
    // return-register ordinal (≥1 — piece 0 is the Call's own result). Result =
    // the piece's register type (I64/F64). The caller-side mirror of `Arg`.
    ReturnPiece,
    // FC7 C3 (AAPCS64/Apple x8 sret). The CALLEE-side entry read of the indirect-
    // result-register (x8): the incoming address of the caller-allocated result
    // storage for a >16-byte by-value RETURN. A leaf value-origin like `Arg`, but
    // sourced from the cc's `indirectResultRegister` instead of an arg register;
    // result = a pointer. Used ONLY when the CC's sret is register-based
    // (aggregateSretViaHiddenArg=false), NOT the SysV/Win64 hidden-arg path.
    // (The CALLER side needs NO opcode: the sret-pointer is a normal prepended
    // Call operand routed to the IRR by the `call_payload::kIndirectResultBit`
    // flag — see the IRR-reroute design in lir_callconv. No WriteIndirectResult.)
    ReadIndirectResult,
    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the two FRAME-relative address value-
    // origins a `va_start` writes into the `__va_list_tag`. Both are leaves (like
    // `Arg`/`ReadIndirectResult`): the concrete byte offset is unknown until the
    // LIR callconv pass owns the frame layout, so they survive to LIR as virtual
    // ops that materialize into `lea reg, [sp + offset]`. Result = a pointer; side-
    // effecting so DCE can't drop them and no pass hoists them off their function.
    //   VaRegSaveAreaAddr:     &(the register-save-area the variadic prologue spills
    //                          rdi..r9 + al-gated xmm0..7 into).
    //   VaOverflowArgAreaAddr: &(the incoming STACK args — where overflow varargs
    //                          live; geometry mirrors the stack-resident `arg` read).
    // The PRESENCE of a VaRegSaveAreaAddr in a function is ALSO the LIR pass's
    // signal that the function called va_start and so needs the save-area reserved +
    // the prologue spill (self-contained — no FnSig lookup, no threaded flag).
    //
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the Win64 HomogeneousPointer va_start
    // leaf. Win64's named-arg HOME space (rcx/rdx/r8/r9 slots) and the stack overflow
    // are CONTIGUOUS in the caller's outgoing area, so a single base address +
    // namedArgCount slots positions `ap` past ALL named args (home or stack). Carries
    // the NAMED-ARG SLOT COUNT as its PAYLOAD; lir_callconv materializes it to
    // `lea result, [sp + totalFrameSize + callPushBytes + payload*outgoingSlotSize]`
    // — the SAME base as the va-overflow leaf MINUS shadowSpaceBytes (the home space
    // IS the shadow space). Its PRESENCE is also the Win64 prologue-spill signal.
    //   VaHomeArgAreaAddr:     &(home[namedArgCount]) — the first vararg under Win64.
    VaRegSaveAreaAddr, VaOverflowArgAreaAddr, VaHomeArgAreaAddr,
    // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): a first-class BY-VALUE-
    // AGGREGATE STACK ARG carrier. Some ABIs require a by-value aggregate to be
    // passed ENTIRELY in the outgoing overflow (stack) area UNCONDITIONALLY — even
    // when arg registers are free — and never split across registers and the stack
    // (SysV §3.2.3/§3.5.7: a MEMORY-class >16B aggregate, OR an InRegisters
    // aggregate whose eightbyte pieces do not all fit in the remaining arg
    // registers). The callconv's greedy register-then-overflow placement has no
    // per-arg force-to-stack lever, so this op is the lever: it wraps the temp
    // ADDRESS of a callee-owned copy (operand 0 — a freshAggregateTemp filled by
    // lowerByteWiseCopy, same as the by-reference arg) and carries the aggregate's
    // BYTE SIZE as its PAYLOAD. It appears as ONE Call operand at the aggregate's
    // left-to-right argument position; mir_to_lir lowers it to a Reg (the address,
    // kept live by regalloc) immediately followed by a `ByValueStackAgg` marker
    // operand carrying the size, and lir_callconv reserves ceil(size / outgoing-
    // slot) overflow slots + byte-copies the temp into the outgoing area (it does
    // NOT consume an arg register and does NOT inflate the SSE/AL count). CC-NEUTRAL
    // by construction (the size is the only datum; the overflow geometry is the
    // callconv's, config-driven) so the Win64 + AAPCS64 struct-vararg cycles reuse
    // it. Result = a pointer (the temp address, threaded through); side-effecting so
    // DCE can't drop it and no pass hoists it off its call.
    ByValueStackArg,
    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the CALLEE-side mirror of
    // `ByValueStackArg`. When a FIXED by-value aggregate PARAM straddles the
    // reg/stack boundary it is received WHOLLY from the incoming overflow (stack)
    // area — consuming ZERO arg registers — never split. This leaf yields the
    // ADDRESS of that incoming aggregate (lir_callconv materializes it like
    // `VaOverflowArgAreaAddr`: `lea result, [sp + totalFrameSize + callPushBytes +
    // shadowSpaceBytes + payload]`); the PAYLOAD is the aggregate's byte offset
    // WITHIN the incoming overflow area (0 = first/only overflowed fixed param).
    // HIR→MIR byte-copies from this address into the param's local slot (the
    // by-reference reception precedent). DELIBERATELY a distinct opcode from the
    // va_* leaves: those triple as lir_callconv's "this function called va_start"
    // signal, and a stacked fixed aggregate occurs in NON-variadic functions too —
    // reusing one would falsely trigger the variadic prologue spill. 0 operands,
    // value result (a pointer), side-effecting so it pins to entry + DCE can't drop.
    RecvByValueStackParam,
    // ── SSA join ──
    Phi,           // operand range addresses the PHI pool, not the operand pool
    // ── terminators (exactly one, last in a block; successors live in succ pool) ──
    Br, CondBr, Switch, Return, Unreachable,
    // D-CSUBSET-COMPUTED-GOTO: `goto *expr` — an indirect branch to a COMPUTED
    // address. operand[0] = the address value; successors = EVERY address-taken
    // block in the function (variadic, modeled exactly like Switch's successor
    // list). Listing all address-taken blocks as successors makes the CFG correct
    // BY CONSTRUCTION — reachability/DCE see them reachable, phi-validation sees
    // the indirect predecessor (MF-1; blockSuccessors is generic, so RPO/preds/
    // dominators/verifier handle it like any variadic-successor terminator).
    IndirectBr,
    // ── c115 SEH (D-WIN64-SEH-FUNCLETS): MSVC `__try { … } __except (f) { … }`.
    // The region skeleton in a flat CFG:
    //   SehTryBegin — a 0-operand TERMINATOR with exactly 2 successors
    //     [tryEntry, filterEntry]; payload = the per-function SEH region id.
    //     The filter/handler blocks hang off the CFG here (reachable, dominated);
    //     the tryEntry edge is the normal path. In the clobber list: fault-time
    //     memory state must be ordered at the region boundary.
    //   SehFilterReturn — the filter block's TERMINATOR: operand[0] = the i32
    //     filter value (EXECUTE_HANDLER 1 / CONTINUE_SEARCH 0 / CONTINUE_EXEC -1),
    //     1 successor [handlerEntry]; payload = the region id. In the clobber
    //     list (audit F1): on x64 SEH, INNER termination handlers run BETWEEN
    //     filter evaluation and handler entry (RtlUnwindEx phase 2), so a load
    //     may not be CSE'd from filter into handler.
    //   SehTryEnd — a 0-operand side-effecting MARKER at the guarded body's
    //     single fall-through exit (option (C): D-CSUBSET-SEH-EARLY-EXIT keeps
    //     it the ONLY exit); payload = the region id. In the clobber list.
    //   SehExceptionCode / SehExceptionInfo — 0-operand VALUE ops (u32 / ptr):
    //     the `_exception_code()` / `_exception_info()` intrinsics, wired to the
    //     __C_specific_handler dispatch context by the c116 funclet lowering.
    // Until c116, mir_to_lir FAILS LOUD on all five (every target).
    SehTryBegin, SehFilterReturn, SehTryEnd, SehExceptionCode, SehExceptionInfo,
    // ── c116 (D-WIN64-SEH-FUNCLETS, H1): generic frame-slot recovery. ──
    // `RecoverParentFrameSlot`: operand[0] = an establisher-frame base pointer
    // (a NORMAL SSA value — e.g. a funclet's establisher-frame parameter, NEVER a
    // hardcoded register); payload = the 0-based scan-order slot index of a local
    // in the frame that base points at. Result = a pointer to that frame slot. It
    // lowers (at callconv) to `lea result, [base + frameSlotOffset(slotIndex)]`,
    // where `frameSlotOffset` comes from the config-driven FrameLayout of the
    // frame the base establishes — the SAME localAreaOffset() + slot*slotSize
    // geometry `alloca` / `lea_frame_slot` resolve against. AGNOSTIC: base is an
    // operand, the offset is from FrameLayout, and the op names nothing
    // arch/format/SEH-specific — it is the generic analog of LLVM's
    // `llvm.localrecover`. The c116 SEH filter funclet is its first consumer
    // (recovering a parent local the __except filter reads); a fault-time
    // establisher frame == the parent's post-prologue SP (FrameRegister=0), so
    // `[base + off]` == the parent's `[SP + off]`.
    RecoverParentFrameSlot,
    // ── SIMD (reserved post-v1; vocabulary fixed now) ──
    VAdd, VSub, VMul, VShuffle, VExtract, VInsert,

    Count_  // keep last — counts the members
};

static_assert(static_cast<std::uint32_t>(MirOpcode::Count_) <= 0xFFFF,
              "MirOpcode must fit in its uint16_t storage");

// Whether an opcode produces an SSA value (and therefore whether its result
// `typeId` must be valid). `Optional` is for calls, which produce a value iff the
// callee's return type is non-void — both a valid and an invalid result type are
// legitimate, so neither the builder nor the verifier may force the issue.
enum class MirResultRule : std::uint8_t {
    None,      // never produces a value — result typeId MUST be InvalidType
    Value,     // always produces a value — result typeId MUST be valid()
    Optional,  // may produce a value (Call / IntrinsicCall) — either is legal
};

// Variadic-operand sentinel for `MirOpcodeInfo::maxOperands`.
inline constexpr std::uint8_t kMirUnboundedOperands = 0xFF;

// Variadic-successor sentinel for `MirOpcodeInfo::maxSuccessors` (Switch).
inline constexpr std::uint8_t kMirUnboundedSuccessors = 0xFF;

// D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the `ByValueStackArg` op's
// uint32 PAYLOAD packs the aggregate byte size (low 30 bits) + the arg-register class
// the CALLER's placement EXHAUSTS once the aggregate is stacked (high 2 bits: 0 =
// none/BACKFILL [SysV], 1 = GPR, 2 = FPR [AAPCS64 §B]). hir_to_mir encodes it,
// mir_to_lir unpacks it onto the LIR `ByValueStackAgg` marker, lir_callconv clamps the
// matching arg-cursor so a subsequent arg/vararg of that class also goes to memory
// (matching the callee's va_start clamp). An aggregate is never ≥1 GiB so 30 bits hold
// the size; the encode site fails loud if it would not.
inline constexpr std::uint32_t kByValueStackArgSizeMask     = 0x3FFFFFFFu;
inline constexpr unsigned      kByValueStackArgExhaustShift = 30;
inline constexpr std::uint8_t  kByValueStackArgExhaustNone  = 0;
inline constexpr std::uint8_t  kByValueStackArgExhaustGpr   = 1;
inline constexpr std::uint8_t  kByValueStackArgExhaustFpr   = 2;

// The single source of truth for an opcode's shape. The builder consults the
// operand/successor bounds + result rule at construction; the ML3 verifier, ML4
// text, ML5 instruction selection, and the optimizer all read this one table
// instead of duplicating per-opcode `switch`es. CFG successor arity lives here
// too (not just operands) so the terminator builders and the verifier validate
// against ONE descriptor.
//
// **Design note vs HIR.** HIR splits per-kind static facts into separate
// standalone constexpr functions (`childArity` / `requiresValidType` /
// `isLoopKind` / `isBranchTargetKind`), which is friendlier when an *extension*
// registry adds new kinds (each new predicate is a tiny function, no shared
// schema). MIR fuses them into one descriptor because (a) the MIR opcode
// vocabulary is **closed** — there is no extension registry, so the table is
// finite and authored once — and (b) every consumer (builder, verifier, text,
// isel, optimizer) wants the *same row* for *any* opcode in one read, which a
// unified struct delivers without N calls. The `{0,0}` cells on non-terminator
// successor columns are the visible cost; the cross-field consistency at one
// row is the visible win. Don't paper this over later by splitting MIR into
// HIR's style — they're solving different problems.
struct MirOpcodeInfo {
    std::uint8_t     minOperands;     // minimum operand count
    std::uint8_t     maxOperands;     // kMirUnboundedOperands == variadic
    std::uint8_t     minSuccessors;   // minimum CFG successor blocks (0 for non-terminators)
    std::uint8_t     maxSuccessors;   // kMirUnboundedSuccessors == variadic (Switch)
    MirResultRule    result;          // result-type discipline
    bool             isTerminator;    // ends a basic block (exactly one per block)
    bool             hasSideEffects;  // not removable purely because its result is unused
    bool             usesPhiPool;     // operand range addresses the phi pool (Phi only)
    std::string_view mnemonic;        // for .dssir text + diagnostics
};

[[nodiscard]] constexpr MirOpcodeInfo opcodeInfo(MirOpcode op) noexcept {
    using R = MirResultRule;
    constexpr std::uint8_t N = kMirUnboundedOperands;
    constexpr std::uint8_t S = kMirUnboundedSuccessors;
    // Columns: minOp, maxOp, minSucc, maxSucc, result, isTerm, sideEffect, phiPool, mnemonic.
    switch (op) {
        // sentinel: impossible {1,0} operand arity surfaces any accidental use loudly.
        case MirOpcode::Invalid: return {1, 0, 0, 0, R::None, false, false, false, "invalid"};

        // value origins (leaves — no operands).
        case MirOpcode::Arg:        return {0, 0, 0, 0, R::Value, false, false, false, "arg"};
        case MirOpcode::Const:      return {0, 0, 0, 0, R::Value, false, false, false, "const"};
        case MirOpcode::GlobalAddr: return {0, 0, 0, 0, R::Value, false, false, false, "globaladdr"};
        // D-CSUBSET-COMPUTED-GOTO: block-address value (payload = target block id);
        // a leaf like GlobalAddr — no operands, a pointer result. Marked
        // SIDE-EFFECTING so DCE never drops it even if the `&&label` value looks
        // unused: its PRESENCE is the canonical mark that its target block is
        // address-taken (Mir::isBlockAddressTaken), which the SimplifyCfg fold-guard
        // (MF-B) and the IndirectBr's baked successor set both rely on. If DCE could
        // remove a "dead" BlockAddress, isBlockAddressTaken would flip to false and
        // SimplifyCfg could fold a block the IndirectBr still lists as a successor —
        // a dangling edge. Pinning it keeps the address-taken set stable.
        case MirOpcode::BlockAddress: return {0, 0, 0, 0, R::Value, false, true, false, "blockaddress"};

        // integer arithmetic.
        case MirOpcode::Add:  return {2, 2, 0, 0, R::Value, false, false, false, "add"};
        case MirOpcode::Sub:  return {2, 2, 0, 0, R::Value, false, false, false, "sub"};
        case MirOpcode::Mul:  return {2, 2, 0, 0, R::Value, false, false, false, "mul"};
        case MirOpcode::UMulH: return {2, 2, 0, 0, R::Value, false, false, false, "umulh"};
        case MirOpcode::AtomicCas: return {3, 3, 0, 0, R::Value, false, true, false, "atomic_cas"};
        // 0 operands, NO result (R::None), side-effecting (never DCE'd, never
        // CSE'd/hoisted) + in the opcodeClobbersMemory list (a fence to
        // Load/Store motion across it). Lowers to ZERO instructions.
        case MirOpcode::CompilerBarrier: return {0, 0, 0, 0, R::None, false, true, false, "compiler_barrier"};
        case MirOpcode::SDiv: return {2, 2, 0, 0, R::Value, false, false, false, "sdiv"};
        case MirOpcode::UDiv: return {2, 2, 0, 0, R::Value, false, false, false, "udiv"};
        case MirOpcode::SMod: return {2, 2, 0, 0, R::Value, false, false, false, "smod"};
        case MirOpcode::UMod: return {2, 2, 0, 0, R::Value, false, false, false, "umod"};
        case MirOpcode::Neg:  return {1, 1, 0, 0, R::Value, false, false, false, "neg"};

        // floating-point arithmetic.
        case MirOpcode::FAdd: return {2, 2, 0, 0, R::Value, false, false, false, "fadd"};
        case MirOpcode::FSub: return {2, 2, 0, 0, R::Value, false, false, false, "fsub"};
        case MirOpcode::FMul: return {2, 2, 0, 0, R::Value, false, false, false, "fmul"};
        case MirOpcode::FDiv: return {2, 2, 0, 0, R::Value, false, false, false, "fdiv"};
        case MirOpcode::FNeg: return {1, 1, 0, 0, R::Value, false, false, false, "fneg"};

        // bitwise.
        case MirOpcode::And:  return {2, 2, 0, 0, R::Value, false, false, false, "and"};
        case MirOpcode::Or:   return {2, 2, 0, 0, R::Value, false, false, false, "or"};
        case MirOpcode::Xor:  return {2, 2, 0, 0, R::Value, false, false, false, "xor"};
        case MirOpcode::Shl:  return {2, 2, 0, 0, R::Value, false, false, false, "shl"};
        case MirOpcode::LShr: return {2, 2, 0, 0, R::Value, false, false, false, "lshr"};
        case MirOpcode::AShr: return {2, 2, 0, 0, R::Value, false, false, false, "ashr"};
        case MirOpcode::Not:  return {1, 1, 0, 0, R::Value, false, false, false, "not"};

        // integer comparison.
        case MirOpcode::ICmpEq:  return {2, 2, 0, 0, R::Value, false, false, false, "icmp.eq"};
        case MirOpcode::ICmpNe:  return {2, 2, 0, 0, R::Value, false, false, false, "icmp.ne"};
        case MirOpcode::ICmpSlt: return {2, 2, 0, 0, R::Value, false, false, false, "icmp.slt"};
        case MirOpcode::ICmpSle: return {2, 2, 0, 0, R::Value, false, false, false, "icmp.sle"};
        case MirOpcode::ICmpSgt: return {2, 2, 0, 0, R::Value, false, false, false, "icmp.sgt"};
        case MirOpcode::ICmpSge: return {2, 2, 0, 0, R::Value, false, false, false, "icmp.sge"};
        case MirOpcode::ICmpUlt: return {2, 2, 0, 0, R::Value, false, false, false, "icmp.ult"};
        case MirOpcode::ICmpUle: return {2, 2, 0, 0, R::Value, false, false, false, "icmp.ule"};
        case MirOpcode::ICmpUgt: return {2, 2, 0, 0, R::Value, false, false, false, "icmp.ugt"};
        case MirOpcode::ICmpUge: return {2, 2, 0, 0, R::Value, false, false, false, "icmp.uge"};

        // floating-point comparison.
        case MirOpcode::FCmpOeq: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.oeq"};
        case MirOpcode::FCmpOne: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.one"};
        case MirOpcode::FCmpOlt: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.olt"};
        case MirOpcode::FCmpOle: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.ole"};
        case MirOpcode::FCmpOgt: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.ogt"};
        case MirOpcode::FCmpOge: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.oge"};
        case MirOpcode::FCmpUeq: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.ueq"};
        case MirOpcode::FCmpUne: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.une"};
        case MirOpcode::FCmpUlt: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.ult"};
        case MirOpcode::FCmpUle: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.ule"};
        case MirOpcode::FCmpUgt: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.ugt"};
        case MirOpcode::FCmpUge: return {2, 2, 0, 0, R::Value, false, false, false, "fcmp.uge"};

        // memory. Alloca yields a pointer; an optional operand is the TOTAL RUNTIME
        // BYTE SIZE of a variable-length array (VLA C1a, D-CSUBSET-VLA — with a ZERO
        // primary payload, the runtime-sized sentinel; a FIXED alloca carries its
        // byte size in the payload and NO operand). It is flagged side-effecting so
        // DCE cannot drop a stack slot whose address escaped (via Store/Call) even
        // when the SSA result looks unused. Store writes [value, ptr], yields nothing.
        case MirOpcode::Alloca: return {0, 1, 0, 0, R::Value, false, true,  false, "alloca"};
        case MirOpcode::Load:   return {1, 1, 0, 0, R::Value, false, false, false, "load"};
        case MirOpcode::Store:  return {2, 2, 0, 0, R::None,  false, true,  false, "store"};
        case MirOpcode::Gep:    return {1, N, 0, 0, R::Value, false, false, false, "gep"};
        // D5.6: first-class aggregate read/write. Operand layout
        // matches Gep's convention — indices ride as MirInstId
        // operands (Const-typed integers), not as a separate scalar
        // span; this keeps a single uniform "instructions reference
        // other instructions" model across the IR.
        //   ExtractValue: [aggregate, idx0, idx1, ...]; minOperands=2.
        //   InsertValue:  [aggregate, value, idx0, idx1, ...]; minOperands=3.
        // Result is a value (the element's type / the aggregate's type
        // respectively), no side effect, not a terminator.
        case MirOpcode::ExtractValue: return {2, N, 0, 0, R::Value, false, false, false, "extractvalue"};
        case MirOpcode::InsertValue:  return {3, N, 0, 0, R::Value, false, false, false, "insertvalue"};

        // casts (operand → result type).
        case MirOpcode::Trunc:    return {1, 1, 0, 0, R::Value, false, false, false, "trunc"};
        case MirOpcode::SExt:     return {1, 1, 0, 0, R::Value, false, false, false, "sext"};
        case MirOpcode::ZExt:     return {1, 1, 0, 0, R::Value, false, false, false, "zext"};
        case MirOpcode::FPTrunc:  return {1, 1, 0, 0, R::Value, false, false, false, "fptrunc"};
        case MirOpcode::FPExt:    return {1, 1, 0, 0, R::Value, false, false, false, "fpext"};
        case MirOpcode::Bitcast:  return {1, 1, 0, 0, R::Value, false, false, false, "bitcast"};
        case MirOpcode::IntToPtr: return {1, 1, 0, 0, R::Value, false, false, false, "inttoptr"};
        case MirOpcode::PtrToInt: return {1, 1, 0, 0, R::Value, false, false, false, "ptrtoint"};
        case MirOpcode::FPToSI:   return {1, 1, 0, 0, R::Value, false, false, false, "fptosi"};
        case MirOpcode::FPToUI:   return {1, 1, 0, 0, R::Value, false, false, false, "fptoui"};
        case MirOpcode::SIToFP:   return {1, 1, 0, 0, R::Value, false, false, false, "sitofp"};
        case MirOpcode::UIToFP:   return {1, 1, 0, 0, R::Value, false, false, false, "uitofp"};

        // calls (result Optional — void callee ⇒ no value).
        case MirOpcode::Call:          return {1, N, 0, 0, R::Optional, false, true, false, "call"};
        case MirOpcode::IntrinsicCall: return {0, N, 0, 0, R::Optional, false, true, false, "intrinsic"};
        // ReturnPiece: [call]; payload = per-class return-register ordinal. Side-
        // effecting so DCE can't drop it and no pass hoists it above its Call (it
        // reads a physical return register valid only immediately post-call).
        case MirOpcode::ReturnPiece:   return {1, 1, 0, 0, R::Value, false, true, false, "returnpiece"};
        // ReadIndirectResult: a leaf value-origin (reads x8 at entry) — mirror of
        // Arg; side-effecting so it pins to entry and DCE can't drop it.
        case MirOpcode::ReadIndirectResult:  return {0, 0, 0, 0, R::Value, false, true, false, "readindirectresult"};
        // FC12a-core: frame-relative va_list address leaves (mirror ReadIndirectResult:
        // 0 operands, value result, side-effecting so they pin to their function).
        case MirOpcode::VaRegSaveAreaAddr:     return {0, 0, 0, 0, R::Value, false, true, false, "varegsavearea"};
        case MirOpcode::VaOverflowArgAreaAddr: return {0, 0, 0, 0, R::Value, false, true, false, "vaoverflowargarea"};
        // FC12b: like VaOverflowArgAreaAddr but PAYLOAD-carrying (the named-arg slot
        // count) — a value leaf, side-effecting so it pins to its function + DCE
        // can't drop it; lir_callconv reads the payload for the contiguous offset.
        case MirOpcode::VaHomeArgAreaAddr:     return {0, 0, 0, 0, R::Value, false, true, false, "vahomeargarea"};
        // FC12a-struct: by-value-aggregate stack-arg carrier. 1 operand (the temp
        // address); value result (the pointer, threaded through); side-effecting so
        // it pins to its call + DCE can't drop it. payload = aggregate byte size.
        case MirOpcode::ByValueStackArg:       return {1, 1, 0, 0, R::Value, false, true, false, "byvaluestackarg"};
        // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: callee-side mirror —
        // a 0-operand value leaf (the incoming stacked-aggregate address), side-
        // effecting so it pins to entry + DCE can't drop it. payload = the byte
        // offset of this aggregate within the incoming overflow area.
        case MirOpcode::RecvByValueStackParam: return {0, 0, 0, 0, R::Value, false, true, false, "recvbyvaluestackparam"};

        // phi — operand range addresses the PHI pool (incoming value/block pairs).
        case MirOpcode::Phi: return {0, N, 0, 0, R::Value, false, false, true, "phi"};

        // terminators. CFG successors live in the succ pool (NOT operands), so the
        // successor-arity columns — not the operand ones — bound the edge count:
        // Br → 1; CondBr → 2 (true, false); Switch → ≥1 (case targets + default);
        // Return / Unreachable → 0.
        case MirOpcode::Br:          return {0, 0, 1, 1, R::None, true, true, false, "br"};
        case MirOpcode::CondBr:      return {1, 1, 2, 2, R::None, true, true, false, "condbr"};
        case MirOpcode::Switch:      return {1, N, 1, S, R::None, true, true, false, "switch"};
        // D-CSUBSET-COMPUTED-GOTO: indirect branch. EXACTLY 1 operand (the address
        // value); successors = every address-taken block (variadic, like Switch).
        case MirOpcode::IndirectBr:  return {1, 1, 1, S, R::None, true, true, false, "indirectbr"};
        // FC7 C1c: a by-value struct returned IN REGISTERS carries N eightbyte/HFA
        // PIECE operands (each a return-register value); a scalar return carries 1, a
        // void return 0. The bound must admit N — `1` truncated every multi-piece
        // return at the verifier's structural check, the upper guard of the same
        // miscompile the clone sites caused (masked on x86_64 by arg/return register
        // aliasing, exposed on AAPCS64). The per-piece type/count is checked in
        // checkTypeInvariants (the FC7 C1c multi-Return rule).
        case MirOpcode::Return:      return {0, N, 0, 0, R::None, true, true, false, "return"};
        case MirOpcode::Unreachable: return {0, 0, 0, 0, R::None, true, true, false, "unreachable"};

        // c115 SEH (D-WIN64-SEH-FUNCLETS). SehTryBegin: terminator, successors
        // [tryEntry, filterEntry] (the CondBr shape, 0 operands). SehFilterReturn:
        // terminator, operand [filterValue i32], successor [handlerEntry].
        // SehTryEnd: the guarded body's fall-through marker (CompilerBarrier's
        // shape). SehExceptionCode/Info: 0-operand value intrinsics (side-
        // effecting so DCE keeps and CSE never merges them — their value is
        // dispatch-context state, not a pure function).
        case MirOpcode::SehTryBegin:      return {0, 0, 2, 2, R::None, true, true, false, "seh_try_begin"};
        case MirOpcode::SehFilterReturn:  return {1, 1, 1, 1, R::None, true, true, false, "seh_filter_return"};
        case MirOpcode::SehTryEnd:        return {0, 0, 0, 0, R::None, false, true, false, "seh_try_end"};
        case MirOpcode::SehExceptionCode: return {0, 0, 0, 0, R::Value, false, true, false, "seh_exception_code"};
        case MirOpcode::SehExceptionInfo: return {0, 0, 0, 0, R::Value, false, true, false, "seh_exception_info"};
        // c116 H1 (D-WIN64-SEH-FUNCLETS): 1 operand (the establisher base pointer),
        // payload = slot index; result = a pointer to that frame slot. A pure
        // address computation (like `alloca`/`lea_frame_slot`): NOT side-effecting,
        // NOT a terminator. Synth appends it in the funclet AFTER the optimizer, and
        // the pipeline runs no MIR-tier DCE/CSE afterward, so its result-use survives.
        case MirOpcode::RecoverParentFrameSlot:
                                          return {1, 1, 0, 0, R::Value, false, false, false, "recover_parent_frame_slot"};

        // SIMD (reserved — provisional arities).
        case MirOpcode::VAdd:     return {2, 2, 0, 0, R::Value, false, false, false, "vadd"};
        case MirOpcode::VSub:     return {2, 2, 0, 0, R::Value, false, false, false, "vsub"};
        case MirOpcode::VMul:     return {2, 2, 0, 0, R::Value, false, false, false, "vmul"};
        case MirOpcode::VShuffle: return {2, N, 0, 0, R::Value, false, false, false, "vshuffle"};
        case MirOpcode::VExtract: return {2, 2, 0, 0, R::Value, false, false, false, "vextract"};
        case MirOpcode::VInsert:  return {3, 3, 0, 0, R::Value, false, false, false, "vinsert"};

        case MirOpcode::Count_: break;  // not a real opcode
    }
    // No real opcode reaches here — every enumerator has a case above (the
    // -Wswitch-enum latch, G-711, keeps it that way). A future opcode added
    // without a row gets the impossible {1, 0} operand arity, which the verifier
    // flags for every instance rather than silently inheriting permissive bounds.
    return {1, 0, 0, 0, MirResultRule::None, false, false, false, "?"};
}

// Convenience predicates over the descriptor (read it once, ask by name).
[[nodiscard]] constexpr bool isTerminator(MirOpcode op) noexcept {
    return opcodeInfo(op).isTerminator;
}
[[nodiscard]] constexpr bool isPhi(MirOpcode op) noexcept {
    return op == MirOpcode::Phi;
}
// Operand-order-insensitive opcodes: `op(a, b)` and `op(b, a)` yield
// equal results. CSE / GVN consults this to canonicalize the value-
// numbering key (sort operands by id before hashing) so the two
// surface forms hash equal. Asymmetric comparisons (Slt/Ult/Sgt/Ugt
// and unequal-predicate FCmp variants) are NOT commutative; Sub /
// SDiv / Shl / etc. are NOT commutative. The `cse_noncommutative`
// corpus example pins the buggy-vs-correct exit divergence
// (D-OPT1-CSE-NONCOMMUTATIVE-PIN).
[[nodiscard]] constexpr bool isCommutative(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::Add:    case MirOpcode::Mul:    case MirOpcode::UMulH:
        case MirOpcode::And:    case MirOpcode::Or:    case MirOpcode::Xor:
        case MirOpcode::FAdd:   case MirOpcode::FMul:
        case MirOpcode::ICmpEq: case MirOpcode::ICmpNe:
        case MirOpcode::FCmpOeq: case MirOpcode::FCmpOne:
        case MirOpcode::FCmpUeq: case MirOpcode::FCmpUne:
        case MirOpcode::VAdd:   case MirOpcode::VMul:
            return true;
        default:
            return false;
    }
}
[[nodiscard]] constexpr MirResultRule resultRule(MirOpcode op) noexcept {
    return opcodeInfo(op).result;
}
[[nodiscard]] constexpr std::string_view mnemonic(MirOpcode op) noexcept {
    return opcodeInfo(op).mnemonic;
}
// Memory-CLOBBER classification (c113, D-CSUBSET-INTRINSIC-BARRIER audit-F1
// + its review correction) — DISTINCT from `hasSideEffects`, which is a
// DCE-LIVENESS flag ("not removable purely because its result is unused")
// and is true for many ops that write NO aliasable memory: every
// terminator (Br/CondBr/Switch/Return/...), Alloca (a FRESH slot cannot
// alias a pre-existing pointer), the Va*/address-materialization leaves,
// BlockAddress, ReturnPiece. Conflating the two disables Load motion
// wholesale (every loop body ends in a terminator — LICM would hoist
// nothing; the review-caught red). An op CLOBBERS memory iff executing it
// may WRITE (or fence) memory an independent Load's pointer could alias:
//   * Store         — writes *operands[1] (callers alias-test it precisely)
//   * Call /        — an opaque callee may write anything reachable
//     IntrinsicCall
//   * AtomicCas     — a store (the CAS write)
//   * CompilerBarrier — an ordering FENCE: no write, but Load/Store motion
//     across it is forbidden by contract (_ReadWriteBarrier)
// Consumed by the Load-motion clobber walk (opt/analysis/mir_alias.hpp,
// the CSE/LICM shared chokepoint). A future memory-writing op joins THIS
// list (the `isCommutative` positive-list convention over the closed MIR
// verb set — never a lang/arch/format identity).
[[nodiscard]] constexpr bool opcodeClobbersMemory(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::Store:
        case MirOpcode::Call:
        case MirOpcode::IntrinsicCall:
        case MirOpcode::AtomicCas:
        case MirOpcode::CompilerBarrier:
        // c115 SEH region boundaries: memory state must be exactly ordered at
        // SehTryBegin (the filter/handler observe fault-time memory — pre-try
        // loads may not be forwarded past it) and SehTryEnd. SehFilterReturn is
        // in the list per the c115 design-audit F1: on x64 SEH, INNER frames'
        // termination handlers execute BETWEEN filter evaluation and handler
        // entry (RtlUnwindEx phase 2), so a load may not be CSE'd from the
        // filter block into the handler block across it.
        case MirOpcode::SehTryBegin:
        case MirOpcode::SehTryEnd:
        case MirOpcode::SehFilterReturn:
            return true;
        default:
            return false;
    }
}

} // namespace dss
