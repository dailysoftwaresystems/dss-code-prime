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

    // ── integer arithmetic ──
    Add, Sub, Mul, SDiv, UDiv, SMod, UMod, Neg,
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
    VaRegSaveAreaAddr, VaOverflowArgAreaAddr,
    // ── SSA join ──
    Phi,           // operand range addresses the PHI pool, not the operand pool
    // ── terminators (exactly one, last in a block; successors live in succ pool) ──
    Br, CondBr, Switch, Return, Unreachable,
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

        // integer arithmetic.
        case MirOpcode::Add:  return {2, 2, 0, 0, R::Value, false, false, false, "add"};
        case MirOpcode::Sub:  return {2, 2, 0, 0, R::Value, false, false, false, "sub"};
        case MirOpcode::Mul:  return {2, 2, 0, 0, R::Value, false, false, false, "mul"};
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

        // memory. Alloca yields a pointer; an optional operand is the element
        // count (array alloca). It is flagged side-effecting so DCE cannot drop a
        // stack slot whose address escaped (via Store/Call) even when the SSA
        // result looks unused. Store writes [value, ptr] and yields no value.
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

        // phi — operand range addresses the PHI pool (incoming value/block pairs).
        case MirOpcode::Phi: return {0, N, 0, 0, R::Value, false, false, true, "phi"};

        // terminators. CFG successors live in the succ pool (NOT operands), so the
        // successor-arity columns — not the operand ones — bound the edge count:
        // Br → 1; CondBr → 2 (true, false); Switch → ≥1 (case targets + default);
        // Return / Unreachable → 0.
        case MirOpcode::Br:          return {0, 0, 1, 1, R::None, true, true, false, "br"};
        case MirOpcode::CondBr:      return {1, 1, 2, 2, R::None, true, true, false, "condbr"};
        case MirOpcode::Switch:      return {1, N, 1, S, R::None, true, true, false, "switch"};
        // FC7 C1c: a by-value struct returned IN REGISTERS carries N eightbyte/HFA
        // PIECE operands (each a return-register value); a scalar return carries 1, a
        // void return 0. The bound must admit N — `1` truncated every multi-piece
        // return at the verifier's structural check, the upper guard of the same
        // miscompile the clone sites caused (masked on x86_64 by arg/return register
        // aliasing, exposed on AAPCS64). The per-piece type/count is checked in
        // checkTypeInvariants (the FC7 C1c multi-Return rule).
        case MirOpcode::Return:      return {0, N, 0, 0, R::None, true, true, false, "return"};
        case MirOpcode::Unreachable: return {0, 0, 0, 0, R::None, true, true, false, "unreachable"};

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
        case MirOpcode::Add:    case MirOpcode::Mul:
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

} // namespace dss
