#include "lir/lowering/mir_to_lir.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_opcode.hpp"

#include <array>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace dss {

namespace {

// Symbolic name of a MIR opcode, for diagnostic messages. Centralised so a
// future addition to `MirOpcode` is a one-line update; the lowerer's
// per-opcode dispatch is the authoritative consumer.
[[nodiscard]] std::string_view mirOpcodeName(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::Invalid:       return "Invalid";
        case MirOpcode::Arg:           return "Arg";
        case MirOpcode::Const:         return "Const";
        case MirOpcode::GlobalAddr:    return "GlobalAddr";
        case MirOpcode::Add:           return "Add";
        case MirOpcode::Sub:           return "Sub";
        case MirOpcode::Mul:           return "Mul";
        case MirOpcode::ICmpEq:        return "ICmpEq";
        case MirOpcode::ICmpNe:        return "ICmpNe";
        case MirOpcode::ICmpSlt:       return "ICmpSlt";
        case MirOpcode::ICmpSle:       return "ICmpSle";
        case MirOpcode::ICmpSgt:       return "ICmpSgt";
        case MirOpcode::ICmpSge:       return "ICmpSge";
        case MirOpcode::ICmpUlt:       return "ICmpUlt";
        case MirOpcode::ICmpUle:       return "ICmpUle";
        case MirOpcode::ICmpUgt:       return "ICmpUgt";
        case MirOpcode::ICmpUge:       return "ICmpUge";
        case MirOpcode::Br:            return "Br";
        case MirOpcode::CondBr:        return "CondBr";
        case MirOpcode::Switch:        return "Switch";
        case MirOpcode::IndirectBr:    return "IndirectBr";
        case MirOpcode::BlockAddress:  return "BlockAddress";
        case MirOpcode::Phi:           return "Phi";
        case MirOpcode::Return:        return "Return";
        case MirOpcode::Unreachable:   return "Unreachable";
        default:                       return "<deferred>";
    }
}

// Map a MIR ICmp opcode to the universal `TargetCondCode` carried in the
// LIR `setcc` / `jcc` payload. Target-blind: every register-machine
// backend (x86_64, ARM64, RISC-V) has these conditions natively; the
// assembler maps the enum to the physical encoding.
[[nodiscard]] std::optional<TargetCondCode> condCodeForICmp(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::ICmpEq:  return TargetCondCode::Eq;
        case MirOpcode::ICmpNe:  return TargetCondCode::Ne;
        case MirOpcode::ICmpSlt: return TargetCondCode::Slt;
        case MirOpcode::ICmpSle: return TargetCondCode::Sle;
        case MirOpcode::ICmpSgt: return TargetCondCode::Sgt;
        case MirOpcode::ICmpSge: return TargetCondCode::Sge;
        case MirOpcode::ICmpUlt: return TargetCondCode::Ult;
        case MirOpcode::ICmpUle: return TargetCondCode::Ule;
        case MirOpcode::ICmpUgt: return TargetCondCode::Ugt;
        case MirOpcode::ICmpUge: return TargetCondCode::Uge;
        default:                 return std::nullopt;
    }
}

[[nodiscard]] bool isMirTerminator(MirOpcode op) noexcept {
    return op == MirOpcode::Br
        || op == MirOpcode::CondBr
        || op == MirOpcode::Switch
        || op == MirOpcode::Return
        || op == MirOpcode::Unreachable
        || op == MirOpcode::IndirectBr;  // D-CSUBSET-COMPUTED-GOTO
}

// Single source of truth for the lowerer's opcode-mnemonic cache. The
// enum entry order MUST match `kMnemonicTable` below; the static_assert
// pins the alignment.
//
// Cycle 3c collapsed the cycle-3a-pattern of three parallel sequences
// (enum / optional<uint16_t> fields / bool array) into one indexable
// array — the type-design agent's recommendation closing the silent-
// drift hazard between the three. Now every new opcode = one enum row
// + one table row, never a missing-bool flag.
enum class MnemonicSlot : std::uint8_t {
    Arg, Mov, Add, Sub, Mul,
    // D-CSUBSET-DIVISION-OP-CODEGEN (cycle 10r split, 2026-06-04):
    // signed/unsigned divide pre+core slots. Cycle 10q used a single
    // compound slot with packed `[REX.W, 0x99, REX.W, 0xF7]` opcode
    // bytes — but the encoder auto-emits ONE REX prefix at the start
    // computed from operand high bits (e.g. REX.B for R14). The
    // embedded literal second 0x48 (REX.W only); per x86 decode the
    // LAST REX prefix before the opcode wins → the auto-REX 0x41 was
    // REX.B → IDIV operand-low-3-bits=6 decodes as RSI (without REX.B)
    // instead of R14 → divide-by-zero trap. Cycle 10r splits into pre
    // (CQO / XOR-RDX) and core (IDIV /7 / DIV /6) so each instruction
    // gets its own auto-REX. FLAG 1 silent-miscompile guard preserved:
    // SDiv routes pre=cqo + core=idiv_op; UDiv routes pre=xor_rdx_zero
    // + core=div_op. ARM64 closure (single 3-addr SDIV/UDIV with no
    // pre + no implicit RDX:RAX) is anchored
    // D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC.
    SDivPre, SDivCore, UDivPre, UDivCore,
    // D-CSUBSET-DIVISION-OP-CODEGEN type-fold (cycle 10r post-7-agent
    // review, 2026-06-04): the four divide slots above are PAIRED
    // (pre + core). The `DivSlotPair` struct below makes the pairing
    // a type-level invariant so callers cannot transpose pre/core
    // arguments to `lowerDiv` (a silent-miscompile vector that would
    // emit IDIV before CQO → reads uninitialized RDX → nondeterministic
    // quotient or trap, with no diagnostic). The two `constexpr`
    // instances `kSDivPair` / `kUDivPair` define the only valid pairs
    // the lowering dispatch can use.
    Cmp, Setcc, Jmp, Jcc,
    // D-CSUBSET-COMPUTED-GOTO: indirect branch through a register (x86 `jmp r/m64`
    // FF /4, arm64 `BR Xn`). The MIR IndirectBr's address operand is the register;
    // its address-taken successors ride as the LIR terminator's successor list.
    JmpIndirect,
    Ret, Unreachable,
    Alloca, Load, Store, Lea,        // cycle 3c memory ops
    SExt, ZExt, Trunc,                // cycle 3c cast lowering
    // cycle 3d bitwise + integer Neg
    And, Or, Xor, Shl, ShrL, ShrA, Not, Neg,
    // cycle 3d float arithmetic + casts
    FAdd, FSub, FMul, FDiv, FNeg,
    FpCvt, FpToSi, FpToUi, SiToFp, UiToFp,
    // cycle 3d cross-class move (movq via FPR↔GPR Bitcast)
    MovqXClass,
    // cycle 3e: Call + IntrinsicCall (GlobalAddr reuses Mov via SymbolRef)
    Call, IntrinsicCall,
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02): indirect
    // call through an extern-import IAT/GOT slot. x86_64 PE = `FF 15
    // disp32` (CALL [RIP+disp32]); the linker patches disp32 to the IAT
    // slot RVA. Direct `call` (E8 disp32) would execute the IAT slot's
    // BYTES as code rather than dereferencing it for the callee's
    // address — the 0xC0000005 puts SEGV had two layers: missing
    // shadow space AND wrong call opcode. Same schema opcode the
    // trampoline emitter uses for ExitProcess.
    CallIndirectViaExtern,
    // FC1 (V2-4.X full-C, 2026-06-10): NATIVE result-bearing division /
    // remainder verbs — Rule 1 of the capability-driven div/mod
    // realization (closes D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC). A target
    // that declares one of these mnemonics as a `result: value` opcode
    // gets a single 3-address LIR op (arm64 SDIV/UDIV; a future RISC-V
    // DIV/DIVU/REM/REMU hits all four). A target without them falls
    // back to the implicit-register pair (x86 cqo+idiv_op /
    // xor_rdx_zero+div_op) or — remainder only — the generic
    // rem = n − (n/d)·d expansion over div + mul + sub.
    SDivNative, UDivNative, SModNative, UModNative,
    // FC3.5 sweep-c2 (FCmp LIR lowering — D-COND-FLOAT-NAN-TRUTHINESS-
    // FCMP): float compare writing FLAGS, no register result — the
    // float sibling of `cmp`. x86_64 binds UCOMISD (66 0F 2E, width
    // 64) / UCOMISS (0F 2E, width 32); arm64 binds FCMP D/S forms.
    // The width axis on the variants discriminates F64/F32 exactly
    // like the integer 64/32 split.
    FCmp,
    // FC3.5 sweep-c3 (D-LIR-MOD-MSUB-FUSION): fused multiply-subtract —
    // msub r, q, d, n  =  n − q·d  (arm64 MSUB Xd,Xn,Xm,Xa = Xa−Xn·Xm).
    // Rule 3 of the div/mod realization PREFERS a declared `msub` over
    // the generic mul+sub remainder expansion (2-op modulo). Probe-by-
    // mnemonic like every other capability; targets without it keep the
    // expansion unchanged.
    MSub,
    // FC3.5 sweep-c3 (D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE): the keep-
    // other-bits 16-bit immediate inserts at bit positions 16/32/48
    // (arm64 MOVK Xd,#imm16,LSL #n). `lowerConst` splits a wide inline
    // constant into the minimal MOVZ + MOVK ladder when these are
    // declared — selection by constant MAGNITUDE lives in the lowering
    // (an operand's value cannot guard encoding variants); a target
    // without the family (x86: its mov imm32 form swallows the whole
    // inline range) keeps the single-mov materialization byte-
    // identically.
    MovkLsl16, MovkLsl32, MovkLsl48,
    // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): the caller-side virtual op that
    // captures the k-th return register of a struct-returning call into a vreg.
    // Materialized away by lir_callconv (a mov-from-returnReg), like `arg` — never
    // encoded. Declared in every target schema for substrate uniformity.
    RetPiece,
    // FC7 C3 (AAPCS64/Apple x8 sret): the callee entry read of the indirect-result
    // register. Like `RetPiece`/`arg`, materialized away by lir_callconv (a
    // mov-from-indirectResultRegister) — never encoded. Declared in every schema.
    ReadIndirectResult,
    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the two FRAME-relative va_list address
    // virtual ops. Like `arg`/`alloca`, materialized away by lir_callconv into a
    // `lea result, [sp + offset]` once the frame layout is known — never encoded.
    // VaRegSaveArea = &(the variadic prologue's register-save-area); VaOverflowArgArea
    // = &(the incoming overflow stack args). Declared in every schema for uniformity.
    VaRegSaveArea, VaOverflowArgArea,
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the Win64 HomogeneousPointer va_start
    // base. Like the two above but PAYLOAD-carrying (the named-arg slot count);
    // lir_callconv materializes it to `lea result, [sp + totalFrameSize +
    // callPushBytes + payload*outgoingSlotSize]` — the NO-SHADOW contiguous home
    // base (BLOCKER-1). Its presence ALSO triggers the Win64 prologue home-spill.
    VaHomeArgArea,
    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the callee-side mirror of
    // the caller's `ByValueStackArg` — the address of a fixed by-value aggregate
    // received WHOLLY from the incoming stack. Same materialization as
    // VaOverflowArgArea (incoming-overflow base + payload byte offset) but a DISTINCT
    // op so it is not a va_start-detection signal (it occurs in non-variadic fns).
    RecvByValueStackParam,
    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): a RE-REFERENCE of a body-local's
    // stack slot — "this fresh LirReg is the ADDRESS of local-alloca #k" (k =
    // PAYLOAD = the alloca's 0-based scan-order index within the function). The c69
    // entry-block alloca-hoist makes every alloca's address live from entry; reusing
    // ONE address vreg across that (now entry-spanning) range tips a register-
    // pressure cliff (the index_negative scratch-pool exhaustion). Instead the
    // lowering emits the `alloca` op ONCE (it reserves the slot, in scan order) and,
    // at EACH use of the address, a fresh `lea_frame_slot k` whose result has a tiny
    // def→use range — the conventional "rematerialize frame addresses" treatment, so
    // hoisting costs no register pressure. lir_callconv materializes it into the SAME
    // `lea result, [sp + offset]` the original alloca resolves to (offset looked up
    // from the alloca-index→offset map it builds in scan order), so every re-emit and
    // the original agree byte-for-byte on ONE reserved slot. Like the alloca/va_*
    // address ops it is materialized away (never encoded); declared in every schema.
    LeaFrameSlot,
    // c78 (D-CSUBSET-FLOAT-NEG-ENCODING): the x86 realization of float negate —
    // `xorpd/xorps xmm, [rip+mask]` against a 16-byte rodata SIGN-MASK. NOT a
    // separate MIR opcode: MIR FNeg stays abstract and `lowerFNeg` capability-
    // dispatches — a target that declares a NATIVE `fneg` encoding (arm64 FNEG)
    // emits it via MnemonicSlot::FNeg; a target WITHOUT one (x86, whose `fneg`
    // opcode carries no encoding) mints the mask + emits THIS op. Declared only
    // by x86 (`fneg_mask`); arm64 omits it (its cache id stays nullopt — never
    // consulted there). The div-family precedent (SDivPre/Core vs SDivNative)
    // for a per-target-realization-specific slot.
    FNegMask,
    Count_
};
constexpr std::size_t kMnemonicCount = static_cast<std::size_t>(MnemonicSlot::Count_);

// D-CSUBSET-DIVISION-OP-CODEGEN type-fold (cycle 10r 7-agent review,
// 2026-06-04): named-aggregate enforcing the (pre, core) ordering at
// the type system level. The MIR→LIR Div arm dispatches via these
// constants — there is no path for a caller to pass an arbitrary
// (MnemonicSlot, MnemonicSlot) pair to `lowerDiv`. ARM64 will add its
// own DivSlotPair (likely `{Invalid, Sdiv}` with a pre-op-absent
// sentinel) when the 2nd target lands; the substrate already
// supports that via `lowerDiv`'s preOp-absent fail-loud below — see
// D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC.
struct DivSlotPair {
    MnemonicSlot pre;
    MnemonicSlot core;
};
constexpr DivSlotPair kSDivPair{MnemonicSlot::SDivPre,  MnemonicSlot::SDivCore};
constexpr DivSlotPair kUDivPair{MnemonicSlot::UDivPre,  MnemonicSlot::UDivCore};

struct MnemonicCache {
    std::string_view             mnemonic;
    std::optional<std::uint16_t> id;
    bool                         missingReported = false;
};

// Per-row paired table closing the cycle-3d-anchored swap-silent-
// corruption hazard. Each row pins `slot ↔ mnemonic`; the consteval
// alignment check below enforces `kMnemonicRows[i].slot ==
// MnemonicSlot(i)` at compile time — a transposed entry now FAILS the
// build rather than silently misrouting every subsequent slot.
struct MnemonicRow { MnemonicSlot slot; std::string_view mnemonic; };
constexpr std::array<MnemonicRow, kMnemonicCount> kMnemonicRows{{
    {MnemonicSlot::Arg,           "arg"},
    {MnemonicSlot::Mov,           "mov"},
    {MnemonicSlot::Add,           "add"},
    {MnemonicSlot::Sub,           "sub"},
    {MnemonicSlot::Mul,           "mul"},
    {MnemonicSlot::SDivPre,       "cqo"},
    {MnemonicSlot::SDivCore,      "idiv_op"},
    {MnemonicSlot::UDivPre,       "xor_rdx_zero"},
    {MnemonicSlot::UDivCore,      "div_op"},
    {MnemonicSlot::Cmp,           "cmp"},
    {MnemonicSlot::Setcc,         "setcc"},
    {MnemonicSlot::Jmp,           "jmp"},
    {MnemonicSlot::Jcc,           "jcc"},
    {MnemonicSlot::JmpIndirect,   "jmp_indirect"},
    {MnemonicSlot::Ret,           "ret"},
    {MnemonicSlot::Unreachable,   "unreachable"},
    {MnemonicSlot::Alloca,        "alloca"},
    {MnemonicSlot::Load,          "load"},
    {MnemonicSlot::Store,         "store"},
    {MnemonicSlot::Lea,           "lea"},
    {MnemonicSlot::SExt,          "sext"},
    {MnemonicSlot::ZExt,          "zext"},
    {MnemonicSlot::Trunc,         "trunc"},
    {MnemonicSlot::And,           "and"},
    {MnemonicSlot::Or,            "or"},
    {MnemonicSlot::Xor,           "xor"},
    {MnemonicSlot::Shl,           "shl"},
    {MnemonicSlot::ShrL,          "shr_l"},
    {MnemonicSlot::ShrA,          "shr_a"},
    {MnemonicSlot::Not,           "not"},
    {MnemonicSlot::Neg,           "neg"},
    {MnemonicSlot::FAdd,          "fadd"},
    {MnemonicSlot::FSub,          "fsub"},
    {MnemonicSlot::FMul,          "fmul"},
    {MnemonicSlot::FDiv,          "fdiv"},
    {MnemonicSlot::FNeg,          "fneg"},
    {MnemonicSlot::FpCvt,         "fpcvt"},
    {MnemonicSlot::FpToSi,        "fp_to_si"},
    {MnemonicSlot::FpToUi,        "fp_to_ui"},
    {MnemonicSlot::SiToFp,        "si_to_fp"},
    {MnemonicSlot::UiToFp,        "ui_to_fp"},
    {MnemonicSlot::MovqXClass,    "movq_xclass"},
    {MnemonicSlot::Call,           "call"},
    {MnemonicSlot::IntrinsicCall,  "intrinsic_call"},
    {MnemonicSlot::CallIndirectViaExtern, "call_indirect_via_extern"},
    {MnemonicSlot::SDivNative,    "sdiv"},
    {MnemonicSlot::UDivNative,    "udiv"},
    {MnemonicSlot::SModNative,    "smod"},
    {MnemonicSlot::UModNative,    "umod"},
    {MnemonicSlot::FCmp,          "fcmp"},
    {MnemonicSlot::MSub,          "msub"},
    {MnemonicSlot::MovkLsl16,     "movk_lsl16"},
    {MnemonicSlot::MovkLsl32,     "movk_lsl32"},
    {MnemonicSlot::MovkLsl48,     "movk_lsl48"},
    {MnemonicSlot::RetPiece,      "ret_piece"},
    {MnemonicSlot::ReadIndirectResult, "read_indirect_result"},
    {MnemonicSlot::VaRegSaveArea,      "va_reg_save_area"},
    {MnemonicSlot::VaOverflowArgArea,  "va_overflow_arg_area"},
    {MnemonicSlot::VaHomeArgArea,      "va_home_arg_area"},
    {MnemonicSlot::RecvByValueStackParam, "recv_by_value_stack_param"},
    {MnemonicSlot::LeaFrameSlot,       "lea_frame_slot"},
    {MnemonicSlot::FNegMask,           "fneg_mask"},
}};
consteval bool kMnemonicRowsAligned() noexcept {
    for (std::size_t i = 0; i < kMnemonicRows.size(); ++i) {
        if (kMnemonicRows[i].slot != static_cast<MnemonicSlot>(i)) return false;
    }
    return true;
}
static_assert(kMnemonicRows.size() == kMnemonicCount,
    "MnemonicSlot enum and kMnemonicRows table drifted in COUNT");
static_assert(kMnemonicRowsAligned(),
    "MnemonicSlot rows out of order — kMnemonicRows[i].slot must equal "
    "MnemonicSlot(i) for every i (cycle-3d swap-silent-corruption hazard).");

// ── FC3.5 sweep-c2: MIR FCmp → target-condition realization plan ─────
//
// THE CORRECTNESS CRUX (D-COND-FLOAT-NAN-TRUTHINESS-FCMP): a float
// compare has FOUR outcomes — less / equal / greater / UNORDERED (a
// NaN operand) — and every Ordered predicate must be FALSE on
// unordered while Une must be TRUE (C 6.5.9: NaN != x holds). The
// flag patterns (derived instruction-by-instruction, pinned by the
// FloatCmpPredicateTruthTable test):
//
//   x86 UCOMISD/UCOMISS (Intel SDM Vol. 2A "UCOMISD"):
//     unordered → ZF=1 PF=1 CF=1;  a>b → all 0;
//     a<b → CF=1 only;             a==b → ZF=1 only.
//     seta  (cc 7) = CF=0∧ZF=0 → exactly Ogt (false on unordered).
//     setae (cc 3) = CF=0      → exactly Oge (false on unordered).
//     setb  (cc 2) = CF=1      → TRUE on unordered — NEVER usable
//                                for an ordered less-than; hence the
//                                SWAP canonicalization below.
//     sete  (cc 4) = ZF=1      → true on unordered too → Oeq needs
//                                the ∧-ordered composition.
//     setne (cc 5) = ZF=0      → false on unordered AND false on
//                                equal → exactly One as a SINGLE cc.
//     setp/setnp (cc A/B) = PF — after UCOMI, PF=1 IS "unordered":
//                                the universal Fuo/Ford conditions.
//
//   arm64 FCMP (ARM ARM C7.2 FCMP; condition table C1.2.4):
//     a==b → Z=1(C=1);  a<b → N=1;  a>b → C=1;  unordered → C=1,V=1.
//     EQ (0) = Z=1   → exactly Foeq (unordered has Z=0).
//     NE (1) = Z=0   → TRUE on unordered → exactly Fune (C's !=) and
//                      therefore NOT usable as One — the mirror of
//                      the x86 asymmetry (there setne IS One).
//     GT (C) = Z=0∧N=V → exactly Ogt (unordered: N=0≠V=1 → false).
//     GE (A) = N=V     → exactly Oge (same V=1 disambiguation).
//     HI (8) = C=1∧Z=0 → TRUE on unordered — the integer-ugt nibble
//                        is a NaN miscompile for floats; this is WHY
//                        the float codes are separate enum entries.
//     VS/VC (6/7) = V — after FCMP, V=1 IS "unordered": Fuo/Ford.
//
// CANONICALIZATION: Olt(a,b) ≡ Ogt(b,a) and Ole(a,b) ≡ Oge(b,a) hold
// EXACTLY under IEEE 754 (including the unordered outcome — both
// sides false), so the lowering swaps the fcmp operands and reuses
// Fogt/Foge. This is the classic x86 shape (no ordered-below setcc
// exists) and is harmless-correct on arm64 (MI would also work; the
// swap keeps ONE target-blind rule).
//
// COMPOSITIONS (universal, used when the target declares no single
// nibble for the predicate — the capability signal is the ABSENCE of
// the `condCodeEncoding` float entry):
//     Foeq = Eq ∧ Ford      Fone = Ne ∧ Ford      Fune = Ne ∨ Fuo
// Each is flag-exact on BOTH targets (the Ford/Fuo conjunct
// disambiguates the unordered case whichever way the equality flag
// conflates it); the shipped configs declare singles where the ISA
// has them — x86: fogt/foge/fone singles, foeq/fune composed;
// arm64: fogt/foge/foeq/fune singles, fone composed.
//
// The unordered-relational predicates (Ueq/Ult/Ule/Ugt/Uge) have NO
// C-subset producer (C's <,<=,>,>= are the ordered forms; == is Oeq;
// != is Une) and stay fail-loud-unsupported — returning nullopt here
// routes them to reportUnsupported, never a silent wrong-parity cc.
struct FloatCmpComposition {
    TargetCondCode partA;    // the flag-exact equality half (Eq / Ne)
    TargetCondCode partB;    // Ford (∧-ordered) or Fuo (∨-unordered)
    MnemonicSlot   combine;  // MnemonicSlot::And or ::Or
};
struct FloatCmpPlan {
    bool           swapOperands = false;
    TargetCondCode single{};   // the preferred single condition code
    std::optional<FloatCmpComposition> composition;  // fallback shape
};
[[nodiscard]] std::optional<FloatCmpPlan> floatCmpPlan(MirOpcode op) noexcept {
    using CC = TargetCondCode;
    switch (op) {
        case MirOpcode::FCmpOgt:
            return FloatCmpPlan{false, CC::Fogt, std::nullopt};
        case MirOpcode::FCmpOge:
            return FloatCmpPlan{false, CC::Foge, std::nullopt};
        case MirOpcode::FCmpOlt:   // a<b ≡ b>a (exact, incl. unordered)
            return FloatCmpPlan{true,  CC::Fogt, std::nullopt};
        case MirOpcode::FCmpOle:   // a<=b ≡ b>=a (exact, incl. unordered)
            return FloatCmpPlan{true,  CC::Foge, std::nullopt};
        case MirOpcode::FCmpOeq:
            return FloatCmpPlan{false, CC::Foeq,
                FloatCmpComposition{CC::Eq, CC::Ford, MnemonicSlot::And}};
        case MirOpcode::FCmpOne:
            return FloatCmpPlan{false, CC::Fone,
                FloatCmpComposition{CC::Ne, CC::Ford, MnemonicSlot::And}};
        case MirOpcode::FCmpUne:
            return FloatCmpPlan{false, CC::Fune,
                FloatCmpComposition{CC::Ne, CC::Fuo, MnemonicSlot::Or}};
        default:
            return std::nullopt;   // Ueq/Ult/Ule/Ugt/Uge: no producer,
                                   // fail loud at the dispatch arm
    }
}

// One transient lowerer per `lowerToLir` call. Holds the per-pass state
// the dispatcher methods read/write. Mirrors `Lowerer` in `hir_to_mir.cpp`.
struct Lowerer {
    Mir const&          mir;
    TargetSchema const& target;
    TypeInterner const& interner;
    DiagnosticReporter& reporter;
    LirBuilder          lir;

    // Single table of cached opcode ids. `kMnemonics[i].mnemonic` is the
    // JSON-side name, `cache_[i].id` is the resolved numeric opcode (or
    // nullopt when the target schema omits it), `cache_[i].missingReported`
    // gates the one-shot diagnostic.
    std::array<MnemonicCache, kMnemonicCount> cache_;

    // FC2 Part B: per-(register-class, op) opcode handles resolved from
    // the target's `registerClassOps[]` table (with the GPR universal
    // defaults — see TargetSchema::regClassOpOpcode). `nullopt` = the
    // class has no such operation; consumers fail loud at the use site
    // (a class/op combination a module never emits must not fail the
    // whole lowering at construction). Generalizes the lowerBitcast
    // class-dispatch pattern to EVERY mov/load/store emission.
    std::array<std::array<std::optional<std::uint16_t>, kRegClassOpCount>, 5>
        classOpCache_{};

    // Per-function state. Cleared at the top of `lowerFunction`.
    MirAttribute<LirReg>            valueToReg;
    MirBlockAttribute<LirBlockId>   mirBlockToLirBlock;

    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): MIR Alloca value id → its
    // 0-based scan-order slot index (the k threaded on a `lea_frame_slot`). An
    // Alloca is NOT entered into `valueToReg`; instead `regForValue` consults this
    // map FIRST and, for an alloca value, emits a fresh `lea_frame_slot k` at the
    // USE site (a tiny def→use range) rather than handing back one entry-spanning
    // address vreg. `allocaLirCount_` is the running count of `alloca` LIR ops this
    // function has emitted — it IS the callconv scan-order index (lir_callconv's
    // `functionLocalAllocaPayloads` walks blocks-then-insts in the SAME order this
    // lowering emits them), so the k recorded here is exactly the index callconv
    // assigns the alloca's frame offset under. Both reset per function.
    std::unordered_map<std::uint32_t, std::uint32_t> allocaSlotIndex_;
    std::uint32_t                   allocaLirCount_ = 0;

    // D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD (FC3.5 sweep-c3): per-function
    // MIR value-use census — value id → (use count, last user). Built
    // by `computeValueUses` over EVERY instruction's `instOperands`
    // PLUS every Phi's `phiIncomings` (phi incoming values live
    // OUTSIDE instOperands; missing them would under-count a phi-read
    // GlobalAddr and fold away a lea the phi still needs — a silent
    // miscompile). Consumed by the GlobalAddr+Load riprel fold's
    // single-use analysis. When count == 1, `user` IS the single user.
    struct MirUseEntry { std::uint32_t count = 0; MirInstId user{}; };
    std::unordered_map<std::uint32_t, MirUseEntry> mirValueUses_;
    // GlobalAddr insts whose lea emission was elided because their
    // single use is a riprel-foldable Load (see
    // `globalAddrRiprelFoldsIntoLoad`). `lowerLoad` consults this to
    // emit the SymbolRef load form instead of reading a (never-
    // defined) address vreg. Any OTHER consumer reaching a folded
    // value via `regForValue` fails loud on the undefined vreg —
    // a disagreement between the two fold sites can never be silent.
    std::unordered_set<std::uint32_t> foldedGlobalAddrs_;

    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB (c94): Const insts whose
    // materialization is ELIDED because their SOLE use is a const-disp Gep
    // that folds the value into a `MemOffset` IMMEDIATE (`lowerGep` reads it
    // via `constIntegerValue`, never `regForValue`). Without this, the
    // main-dispatch `lowerConst` materializes the Const into a register
    // (e.g. arm64 MOVZ/MOVK of the displacement) that the folded `lea`/`ADD`/
    // `SUB` never reads — a DEAD const materialization, sign-AGNOSTIC (it hit
    // BOTH the positive `s.x` member-offset `ADD` and the negative `p[-N]`
    // `SUB` alike; the c93 audit measured it as the residual dead-const on the
    // negative path). `lowerConst` consults this to SKIP materialization; the
    // single-use census guarantees no other consumer needs the register, so
    // the value is never `regForValue`'d — a fold-site disagreement would fail
    // loud on the undefined vreg, never silently mis-encode. Mirrors the
    // `foldedGlobalAddrs_` fold-skip precedent exactly.
    std::unordered_set<std::uint32_t> foldedConstDisps_;

    // Module-tier (NOT per-function) reverse-mapping LirInstId.v →
    // MirInstId. Grows as the lowerer emits LIR insts (slot 0 is the
    // arena's invalid sentinel, default-initialized `InvalidMirInst`).
    // The LirVerifier consumes this to cross-reference LIR vregs
    // against MIR types WITHOUT the cycle-3e positional-alignment
    // hazard that silently skipped switch-bearing functions.
    std::vector<MirInstId>          lirToMir;
    // Current MIR inst being lowered. Set by `lowerInst` /
    // `lowerTerminator` / pre-pass methods so the per-inst LIR emit
    // helpers can record the source. `InvalidMirInst` means "no MIR
    // source" (e.g., Switch's synthetic next-compare blocks, phi-edge
    // parallel-copy moves).
    MirInstId                       currentMir{};

    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02): set
    // of SymbolIds that name extern imports (populated from the
    // caller-supplied `externImports` vector at construction time).
    // `lowerCall` consults this set when the callee is a GlobalAddr
    // — an extern-targeting call must lower to
    // `CallIndirectViaExtern` (FF 15 disp32 — dereferences the IAT
    // slot) rather than `Call` (E8 disp32 — interprets the IAT slot
    // bytes as code).
    std::unordered_set<std::uint32_t> externSymbols;

    // D-FFI-EXTERN-CALL-DISPATCH: the ACTIVE OBJECT FORMAT's extern-call
    // shape. `lowerCall` reads this to pick `CallIndirectViaExtern`
    // (indirect-slot — PE/Mach-O deref an IAT/__got pointer slot) vs the
    // plain `Call` opcode (direct-plt — ELF direct branch to the linker
    // PLT stub). `std::nullopt` = the format declared none; a module with
    // extern imports under a nullopt dispatch is a fail-loud at ctor (no
    // silent default — the wrong shape miscompiles).
    std::optional<ExternCallDispatch> externCallDispatch_;

    // D-CSUBSET-COMPUTED-GOTO: synthetic per-block symbol minting for `&&label`
    // block-address materialization. A block whose address is taken gets ONE local
    // symbol (deduped by MIR block id within the module). `nextBlockSym_` is seeded
    // lazily to 1 + max(existing function/global SymbolId) on first use so a minted
    // id can't collide with a user symbol (same discipline as hir_to_mir's
    // string-literal synthetic minter). `blockToSym_` keys the dedup off the MIR
    // block id (stable for the duration of THIS lowering — the LIR-pass block
    // renumbering happens AFTER, and the BlockRef operand carried on the emitted
    // `lea` is what survives those passes via remapBlockRef).
    std::optional<std::uint32_t>                  nextBlockSym_;
    std::unordered_map<std::uint32_t, SymbolId>   blockToSym_;
    [[nodiscard]] SymbolId mintBlockSymbol(MirBlockId block) {
        if (auto it = blockToSym_.find(block.v); it != blockToSym_.end()) {
            return it->second;
        }
        if (!nextBlockSym_.has_value()) {
            std::uint32_t maxV = 0;
            for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
                if (std::uint32_t const v = mir.funcSymbol(mir.funcAt(fi)).v; v > maxV) {
                    maxV = v;
                }
            }
            for (std::uint32_t gi = 0; gi < mir.moduleGlobalCount(); ++gi) {
                if (std::uint32_t const v = mir.globalSymbol(mir.globalAt(gi)).v; v > maxV) {
                    maxV = v;
                }
            }
            // EXTERN imports occupy SymbolIds too (the exec entry/_start/exit
            // trampoline externs are minted past the function/global high-water at
            // HIR→MIR time). A block symbol minted into their range would collide
            // at link (K_SymbolUndefined "declared more than once"), so clear them.
            for (std::uint32_t const ev : externSymbols) {
                if (ev > maxV) maxV = ev;
            }
            nextBlockSym_ = maxV + 1u;
        }
        SymbolId const sym{(*nextBlockSym_)++};
        blockToSym_.emplace(block.v, sym);
        return sym;
    }

    // D-OPT-SWITCH-JUMP-TABLE (c70): the jump-table lowerer's state.
    //   * `jumpTableDescriptors_` accumulates one descriptor per dense switch;
    //     `run()` moves it into the result for `compile_pipeline.cpp` to consume.
    //   * `currentFuncIndex_` is the index of the function currently being
    //     lowered (== the LIR `funcAt(i)` index, since `run()` lowers functions
    //     in module order) — recorded on each descriptor so the pipeline can
    //     find the right `AssembledFunction` to bind block symbols into.
    std::vector<JumpTableDescriptor> jumpTableDescriptors_;
    std::uint32_t                    currentFuncIndex_ = 0;

    // c78 (D-CSUBSET-FLOAT-NEG-ENCODING): one entry per x86-style float-negate
    // realized as `xorpd/xorps xmm, [rip+mask]`. `run()` moves it into the
    // result for `compile_pipeline.cpp` to materialize as 16-byte, 16-byte-
    // aligned `.rodata` sign-mask items. Empty on a native-fneg target (arm64).
    std::vector<SignMaskConstant> signMaskConstants_;

    // Mint a fresh synthetic SymbolId for a jump table's `.data` item. Draws
    // from the SAME monotone `nextBlockSym_` sequence `mintBlockSymbol` uses, so
    // a table symbol can never collide with a block symbol (or a user / extern
    // symbol — the shared lazy seed sits past every function/global/extern id).
    // Not deduped (each dense switch gets its own table).
    [[nodiscard]] SymbolId mintJumpTableSymbol() {
        if (!nextBlockSym_.has_value()) {
            std::uint32_t maxV = 0;
            for (std::uint32_t fi = 0; fi < mir.moduleFuncCount(); ++fi) {
                if (std::uint32_t const v = mir.funcSymbol(mir.funcAt(fi)).v; v > maxV) {
                    maxV = v;
                }
            }
            for (std::uint32_t gi = 0; gi < mir.moduleGlobalCount(); ++gi) {
                if (std::uint32_t const v = mir.globalSymbol(mir.globalAt(gi)).v; v > maxV) {
                    maxV = v;
                }
            }
            for (std::uint32_t const ev : externSymbols) {
                if (ev > maxV) maxV = ev;
            }
            nextBlockSym_ = maxV + 1u;
        }
        return SymbolId{(*nextBlockSym_)++};
    }

    // c78 (D-CSUBSET-FLOAT-NEG-ENCODING): a fresh synthetic SymbolId for a float-
    // negate sign-mask `.rodata` item. Draws from the SAME monotone
    // `nextBlockSym_` sequence (via `mintJumpTableSymbol`), so a mask symbol
    // can never collide with a block/table/user/extern symbol. Per-occurrence.
    [[nodiscard]] SymbolId mintSignMaskSymbol() { return mintJumpTableSymbol(); }

    // Whether the running lowering pass added any error-severity
    // diagnostics. Mirrors ML2's delta-on-errorCount; reset by the ctor.
    std::uint32_t baselineErrors = 0;
    bool hadError() const noexcept {
        return reporter.errorCount() != baselineErrors;
    }

    Lowerer(Mir const& m, TargetSchema const& t, TypeInterner const& i,
            DiagnosticReporter& r,
            std::span<ExternImport const> externImports,
            std::optional<ExternCallDispatch> externCallDispatch)
        : mir(m), target(t), interner(i), reporter(r), lir(t),
          valueToReg(m), mirBlockToLirBlock(m.blockArena()),
          externCallDispatch_(externCallDispatch) {
        baselineErrors = reporter.errorCount();
        externSymbols.reserve(externImports.size());
        for (auto const& e : externImports) {
            externSymbols.insert(e.symbol.v);
        }
        // Mnemonics in MnemonicSlot enum-declaration order. The
        // `static_assert` below closes the silent-drift hazard 4
        // review agents converged on: if a developer adds a slot to
        // `MnemonicSlot` but forgets to add its mnemonic here (or
        // vice-versa), compilation fails LOUD rather than silently
        // zero-filling the array and returning `nullopt` from every
        // `opcodeByMnemonic("")` lookup. Each slot is positionally
        // indexed; a single off-by-one would corrupt all subsequent
        // opcode lookups.
        // Cycle 3e: pairing of MnemonicSlot ↔ mnemonic-string is now
        // verified at compile time by `kMnemonicRowsAligned` declared
        // alongside the table at namespace scope (below). The constexpr
        // table iterator gives BOTH count-drift AND row-order-drift
        // protection — the cycle-3d count-only static_assert allowed a
        // transposition to compile silently; this closes that.
        for (auto const& r : kMnemonicRows) {
            auto const i = static_cast<std::size_t>(r.slot);
            cache_[i].mnemonic = r.mnemonic;
            cache_[i].id       = target.opcodeByMnemonic(r.mnemonic);
        }

        // FC2 Part B: resolve the per-class move/load/store handles
        // once (mirrors the mnemonic cache). Unresolvable cells stay
        // nullopt — diagnosed at the emitting site, not here.
        for (std::size_t c = 0; c < classOpCache_.size(); ++c) {
            for (std::size_t o = 0; o < kRegClassOpCount; ++o) {
                classOpCache_[c][o] = target.regClassOpOpcode(
                    static_cast<TargetRegClass>(c),
                    static_cast<RegClassOp>(o));
            }
        }

        // D-FFI-EXTERN-CALL-DISPATCH (was D-LK10-ENTRY-ML7-FRAME-BIAS-
        // UNIFY 2nd-order audit fold): fail loud UPFRONT at construction
        // if the module declares extern imports but the active object
        // FORMAT cannot dispatch an extern call — before any lowering
        // work. The extern-call shape is a property of the FORMAT (its
        // dynamic-import model), NOT the target: the SAME x86_64 target
        // needs `call_indirect_via_extern` (FF 15, deref the IAT slot)
        // under PE but the plain `call` (E8, direct branch to the PLT
        // stub) under ELF — picking the wrong one miscompiles (FF 15
        // through an ELF PLT stub dereferences code as a pointer →
        // SIGSEGV). So the gate is keyed on `externCallDispatch_`:
        //   * nullopt          → the format declared no dispatch model
        //                        (NO silent default to either shape).
        //   * indirect-slot    → requires `call_indirect_via_extern`.
        //   * direct-plt       → requires the universal `call` opcode.
        // Static modules (no externs) skip the gate entirely — they
        // legitimately need neither the dispatch model nor the opcode.
        if (!externImports.empty()) {
            auto const callIndirectMissing = !cache_[static_cast<std::size_t>(
                MnemonicSlot::CallIndirectViaExtern)].id.has_value();
            auto const callMissing =
                !cache_[static_cast<std::size_t>(MnemonicSlot::Call)]
                     .id.has_value();
            if (!externCallDispatch_.has_value()) {
                dss::report(reporter,
                    DiagnosticCode::L_RequiredLirOpcodeMissing,
                    DiagnosticSeverity::Error,
                    "MIR→LIR: module declares extern imports but the active "
                    "object format declares no `externCallDispatch` shape — "
                    "extern calls have no defined call-site form. Declare "
                    "`externCallDispatch` in the format's `.format.json` "
                    "(\"indirect-slot\" for PE IAT / Mach-O __got, "
                    "\"direct-plt\" for ELF PLT). (D-FFI-EXTERN-CALL-DISPATCH.)");
            } else if (*externCallDispatch_ == ExternCallDispatch::IndirectSlot
                       && callIndirectMissing) {
                dss::report(reporter,
                    DiagnosticCode::L_RequiredLirOpcodeMissing,
                    DiagnosticSeverity::Error,
                    "MIR→LIR: the active format uses `indirect-slot` extern "
                    "dispatch but the target schema does not declare a "
                    "`call_indirect_via_extern` opcode — IAT/__got-indirect "
                    "extern calls cannot be lowered. Add the indirect-call "
                    "encoding to the target's `.target.json` `opcodes[]` "
                    "(x86_64 uses `FF 15 disp32`). (D-FFI-EXTERN-CALL-DISPATCH.)");
            } else if (*externCallDispatch_ == ExternCallDispatch::DirectPlt
                       && callMissing) {
                dss::report(reporter,
                    DiagnosticCode::L_RequiredLirOpcodeMissing,
                    DiagnosticSeverity::Error,
                    "MIR→LIR: the active format uses `direct-plt` extern "
                    "dispatch but the target schema does not declare a "
                    "`call` opcode — extern calls lower to a plain direct "
                    "call to the linker-synthesized PLT stub, which needs "
                    "the universal `call` encoding (x86_64 `E8 disp32`, "
                    "ARM64 `BL imm26`). (D-FFI-EXTERN-CALL-DISPATCH.)");
            }
        }
    }

    [[nodiscard]] std::optional<std::uint16_t> opcode(MnemonicSlot s) const {
        return cache_[static_cast<std::size_t>(s)].id;
    }

    // D-CSUBSET-BITFIELD-WIDE-UNIT: does the target declare a `mov`
    // variant that accepts a single `imm64` (LiteralIndex) operand?
    // Capability probe — the SANCTIONED selection pattern (probe the
    // declared variant vocabulary, NEVER `if (arch == ...)`). True for
    // x86_64 (the `mov r64, imm64` = B8+rd io variant); false for arm64
    // (which materializes wide constants via the MOVZ/MOVK ladder
    // instead — probed separately by the movk_lsl* mnemonic family).
    [[nodiscard]] bool targetHasMovImm64() const {
        auto const movId = opcode(MnemonicSlot::Mov);
        if (!movId.has_value()) return false;
        auto const* info = target.opcodeInfo(*movId);
        if (info == nullptr) return false;
        for (auto const& v : info->encoding.variants) {
            if (v.operandKinds.size() == 1
                && v.operandKinds[0] == OperandKindFilter::LiteralIndex) {
                return true;
            }
        }
        return false;
    }

    // D-CSUBSET-BITFIELD-WIDE-UNIT: does the target declare the MOVZ/MOVK
    // wide-immediate ladder (all three movk_lsl* shift slots)? Capability
    // probe by mnemonic presence — the established pattern (see
    // `materializeInlineIntConst`). True for arm64; false for x86_64.
    // ALL-OR-NOTHING: a half-declared family never silently emits a
    // partial constant.
    [[nodiscard]] bool targetHasMovkLadder() const {
        return opcode(MnemonicSlot::MovkLsl16).has_value()
            && opcode(MnemonicSlot::MovkLsl32).has_value()
            && opcode(MnemonicSlot::MovkLsl48).has_value();
    }

    // (D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB, c94: the c93
    // `targetHasSignedDispLea` capability probe is GONE. A negative-disp Gep no
    // longer needs a lowering-time fold — BOTH targets encode the plain 3-op
    // `lea [base + MemOffset(disp<0)]` natively: x86 via its signed disp32
    // field, arm64 via the config's `negMemoffset` SUB variants routed by the
    // shared sign matcher. The lowering is target-blind; the config picks the
    // encoding. See lowerGep's const-disp arm.)

    // FC2 Part B: the opcode that performs `op` on a value of register
    // class `cls` (the registerClassOps resolution). GPR resolves to
    // the universal mov/load/store; a class with no declared operation
    // returns nullopt — callers MUST route through
    // `reportMissingClassOp` rather than falling back to the GPR
    // handle (the silent class-blind miscompile this table kills).
    [[nodiscard]] std::optional<std::uint16_t>
    classOp(LirRegClass cls, RegClassOp op) const {
        auto const c = static_cast<std::size_t>(cls);
        if (c >= classOpCache_.size()) return std::nullopt;
        return classOpCache_[c][static_cast<std::size_t>(op)];
    }

    void reportMissingClassOp(LirRegClass cls, RegClassOp op,
                              std::string_view context) {
        dss::report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
            DiagnosticSeverity::Error,
            std::format(
                "target '{}' declares no '{}' operation for register "
                "class '{}' (required for lowering {}) — declare it in "
                "the target's `registerClassOps[]` (the universal "
                "mov/load/store bindings cover only the GPR class); "
                "falling back to the GPR instruction forms would "
                "silently mis-encode",
                target.name(), regClassOpName(op),
                targetRegClassName(static_cast<TargetRegClass>(cls)),
                context));
    }

    // Map a MIR `TypeId` to the LIR register class that holds its
    // values. F16/F32/F64/F128 → FPR; Vector/Matrix → VR (SIMD);
    // integer/bool/pointer → GPR; default for aggregates (Struct/
    // Union/Array/Enum/Tuple/Slice) and any future variant arm →
    // GPR with explicit "scoped to ML5; cycle 3e aggregate-flattening
    // will decide the real shape" note. Invalid TypeId is the only
    // genuinely-unhandled case — we fail-loud-deferred those at the
    // narrow-vs-wide gate in `lowerConst`, so reaching here with an
    // invalid TypeId is itself a structural violation; we return GPR
    // as a defensive default but the using-site should have flagged
    // it earlier.
    [[nodiscard]] LirRegClass regClassForType(TypeId ty) const {
        if (!ty.valid()) return LirRegClass::GPR;
        // Delegate to the substrate-tier helper in `target_schema.hpp`
        // so ML6/ML7/LirVerifier share the same mapping. TargetRegClass
        // and LirRegClass are numerically aligned (the static_assert in
        // `lir_reg.hpp` pins it), so the cast is safe by-construction.
        return static_cast<LirRegClass>(regClassForCoreType(interner.kind(ty)));
    }

    [[nodiscard]] LirRegClass regClassFor(MirInstId id) const {
        return regClassForType(mir.instType(id));
    }

    // FC2 Part B width gate (D-TARGET-ENCODING-WIDTH-GUARD), WIDENED by
    // FC3.5 sweep-c2 (D-CSUBSET-F32-CODEGEN closure): the scalar float
    // tier now ships TWO encoded widths — F64 (the FC2 substrate: F2-
    // prefixed SSE / arm64 D-forms) and F32 (F3-prefixed SSE scalar-
    // single / arm64 S-forms) — selected by the SAME width axis the
    // integer 64/32 split rides (`kLirInstFlagWidth32` ↔ the variant
    // guards' `width` key; `widthFlagsForType` maps F32 → 32). This is
    // the only tier where MIR types are still visible (post-regalloc
    // LIR carries register CLASSES, not widths), so the gate lives
    // here: any FPR-class type flowing into a width-keyed float
    // mnemonic must be one of the ENCODED widths. F16/F128 stay
    // fail-loud (no encodings at any width — first-match could
    // otherwise pick a wrong-width form). Returns true (no-op) for
    // non-FPR types. Applied exactly where float encodings exist
    // (FAdd, FSub, FMul, FDiv, FCmp operands, FPToSI source, FPTrunc/FPExt
    // source+result, FPR-class Load, and — since c78 — FNeg, whose
    // capability-dispatched lowering (native FNEG / sign-mask XORPD) gates
    // here so F16/F128 fail loud before a wrong-width mask/opcode is picked
    // (D-CSUBSET-FLOAT-NEG-ENCODING).
    [[nodiscard]] bool requireEncodedFloatWidth(MirInstId id, TypeId ty,
                                                std::string_view context) {
        if (!ty.valid()) return true;  // upstream diagnosed the type hole
        if (regClassForType(ty) != LirRegClass::FPR) return true;
        TypeKind const k = interner.kind(ty);
        if (k == TypeKind::F64 || k == TypeKind::F32) return true;
        dss::report(reporter,
            DiagnosticCode::L_UnsupportedLoweringForOpcode,
            DiagnosticSeverity::Error,
            std::format(
                "{}: float TypeKind ordinal {} is not lowerable to "
                "target '{}' — only F64 and F32 have scalar float "
                "encodings this cycle; proceeding would silently select "
                "a wrong-width instruction form "
                "(D-TARGET-ENCODING-WIDTH-GUARD)",
                context, static_cast<unsigned>(k), target.name()));
        poisonValue(id);
        return false;
    }

    // FC3 width gate (D-CSUBSET-32BIT-ALU-FORMS — c1 erected it, c2
    // NARROWED it): the integer ALU / div / compare tier now ships TWO
    // encoded widths — 64-bit (REX.W x86 / X-register arm64, the
    // substrate default) and 32-bit (no-REX.W x86 forms, auto-zero-
    // extending / arm64 W-forms, sf=0) selected by the width axis
    // (`kLirInstFlagWidth32` on LirInst.flags ↔ the variant guards'
    // `width` key). I32/U32 therefore COMPUTE AT 32 BITS — true C
    // `int`/`unsigned int` semantics including the DEFINED unsigned
    // wraparound (0xFFFFFFFFu + 1u == 0). Still GATED loudly (a
    // narrower width whose semantics C DEFINES but no encoded form
    // exists would be a silent miscompile, not a diagnostic):
    //   * U8/U16/I8/I16 — defined wraparound/conversion semantics at
    //     8/16 bits; no 8/16-bit ALU forms are encoded (a later FC
    //     extends the width axis — the loader already rejects guard
    //     widths outside {32, 64}).
    //   * I128/U128  — WIDER than the registers; 64-wide compute would
    //     silently truncate.
    //   * Bool/Char/Byte — never reach an ALU op in a post-promotion
    //     language (the UAC block promotes them to the int floor); an
    //     appearance here means a language without promotion is doing
    //     sub-int compute — same fail-loud.
    // COMPARISONS gate on their OPERANDS' type (the result is Bool);
    // Bool-typed compare operands stay allowed — the `!x` lowering's
    // `ICmpEq(x, 0)` over Bool is width-exact for 0/1 values.
    // CONVERSIONS (c2): each conversion mnemonic has an INHERENT
    // (source, dest) width pair, so the gate walls off the shapes
    // with no realization rather than letting first-match pick a
    // wrong-width encoding:
    //   * Trunc — realized for 32-bit results only (x86 `mov r32,r32`
    //     zero-extends = C's mod-2^32 conversion; arm64 W-form ORR
    //     mov). 16/8/Bool results stay fail-loud here AND at the
    //     assembler (the trunc variants carry `width: 32`, so a
    //     non-32 trunc inst matches nothing — belt and suspenders).
    //   * SExt — realized for I32 sources (x86 movsxd r64,r/m32; arm64
    //     SXTW) AND Char sources (x86 movsx r64,r/m8; arm64 SXTB — the
    //     byte form, D-CSUBSET-CHAR-STRING-VALUE-CODEGEN), discriminated
    //     by the SOURCE width flag (32 vs 8). I8/I16 sources stay gated
    //     (no realization yet) — fail-loud here AND at the assembler.
    //   * ZExt — realized for Bool sources only (x86 movzx r64,r/m8;
    //     arm64 W-form ORR over a 0/1 register). A U32 source through
    //     the x86 byte-form would silently read ONE byte — the
    //     32-to-64 zero-extend realization (`mov r32,r32`, already
    //     declared) lands with its first consumer
    //     (D-CSUBSET-ZEXT-32-TO-64).
    // Shifts (FC3.5 sweep-c1): Shl/LShr/AShr are now ENCODED at both
    // widths (x86 D3/C1 CL+imm8 forms via the implicit-count "count"
    // role contract; arm64 LSLV/LSRV/ASRV X+W forms) — they pass this
    // TYPE gate at 32/64 and lower through `lowerShift`. Bitwise
    // (audit-residue sweep c1, D-AUDIT-BITWISE-UNWALL-WITNESS): And/Or
    // are ENCODED end-to-end on BOTH targets (x86 21/09 reg-reg at
    // widths 64+32 since FC3.5 sweep-c2 — added for the composed-FCmp
    // materialization, which also un-walled source-level `&`/`|`;
    // arm64 AND/ORR X-form, width-absent variants) — witnessed by
    // examples/c-subset/bitwise_and_or. The REMAINING walls, per
    // target: x86 declares xor/not WITHOUT encodings (they pass this
    // gate, lower, and fail loud at the assembler —
    // A_NoEncodingDeclared); arm64 ENCODES xor (EOR) but declares no
    // `not` mnemonic at all (Not fails loud at lowering,
    // L_RequiredLirOpcodeMissing). So Xor walls on x86 only; Not walls
    // on both targets, at different tiers.
    [[nodiscard]] bool requireNativeIntWidth(MirInstId id, MirOpcode op) {
        auto const gatedKind = [](TypeKind k) noexcept {
            switch (k) {
                case TypeKind::U8:  case TypeKind::U16:
                case TypeKind::I8:  case TypeKind::I16:
                case TypeKind::I128: case TypeKind::U128:
                case TypeKind::Char: case TypeKind::Byte:
                    return true;
                default:
                    return false;
            }
        };
        auto const fail = [&](TypeId ty, std::string_view what) {
            dss::report(reporter,
                DiagnosticCode::L_UnsupportedLoweringForOpcode,
                DiagnosticSeverity::Error,
                std::format(
                    "{}: integer TypeKind ordinal {} has no native-width "
                    "ALU forms on target '{}' this cycle — the shipped "
                    "integer encodings compute 64- or 32-bit-wide, which "
                    "would silently violate the type's defined wraparound/"
                    "comparison semantics (D-CSUBSET-32BIT-ALU-FORMS)",
                    what, static_cast<unsigned>(interner.kind(ty)),
                    target.name()));
            poisonValue(id);
            return false;
        };
        // Conversion-shape reject: the mnemonic's inherent width pair
        // has no realization for this (source/result) kind.
        auto const failConv = [&](TypeId ty, std::string_view what,
                                  std::string_view realized) {
            dss::report(reporter,
                DiagnosticCode::L_UnsupportedLoweringForOpcode,
                DiagnosticSeverity::Error,
                std::format(
                    "{}: TypeKind ordinal {} has no encoded conversion "
                    "form on target '{}' this cycle — only {} is realized; "
                    "first-match would otherwise pick a wrong-width "
                    "encoding (D-CSUBSET-32BIT-ALU-FORMS)",
                    what, static_cast<unsigned>(interner.kind(ty)),
                    target.name(), realized));
            poisonValue(id);
            return false;
        };
        switch (op) {
            case MirOpcode::Add: case MirOpcode::Sub: case MirOpcode::Mul:
            case MirOpcode::SDiv: case MirOpcode::UDiv:
            case MirOpcode::SMod: case MirOpcode::UMod:
            case MirOpcode::And: case MirOpcode::Or: case MirOpcode::Xor:
            case MirOpcode::Shl: case MirOpcode::LShr: case MirOpcode::AShr:
            case MirOpcode::Neg: case MirOpcode::Not: {
                TypeId const ty = mir.instType(id);
                // Bool ALU compute is gated too (unreachable post-
                // promotion; reaching it = sub-int compute).
                if (ty.valid() && (gatedKind(interner.kind(ty))
                                   || interner.kind(ty) == TypeKind::Bool)) {
                    return fail(ty, mirOpcodeName(op));
                }
                return true;
            }
            case MirOpcode::ICmpEq:  case MirOpcode::ICmpNe:
            case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
            case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge:
            case MirOpcode::ICmpUlt: case MirOpcode::ICmpUle:
            case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge: {
                for (MirInstId const operand : mir.instOperands(id)) {
                    TypeId const ty = mir.instType(operand);
                    if (ty.valid() && gatedKind(interner.kind(ty))) {
                        return fail(ty, "ICmp operand");
                    }
                }
                return true;
            }
            case MirOpcode::Trunc: {
                // Result-width gate: the 32-bit form (I32/U32) and the SUB-NATIVE
                // forms (Char/I8/U8/I16/U16) — D-CSUBSET-SUBNATIVE-ALU-FORMS +
                // D-CSUBSET-CHAR-INT-WIDENING. A sub-native Trunc routes through the
                // SAME width-32 `mov r32,r32` realization (registerOpWidthFlags
                // collapses the byte/half width → 32): it keeps the low 32 bits, and
                // the narrowing-to-1/2-bytes is realized lazily at the next byte/half-
                // EXACT consumer (a SExt/ZExt's movsx/movzx r/m8 or r/m16 / SXTH /
                // UXTH / UXTB, or a width-exact STORE) — every narrow read is
                // low-bits-only, so the value is C-correct (`short s=70000` reads
                // back 4464; `char c=300` reads back 44). An I64-result Trunc cannot
                // arise (its source would be a gated I128); Byte stays REJECTED (no C
                // narrowing story this cycle — fail loud, never a silent narrow).
                TypeId const ty = mir.instType(id);
                if (ty.valid()) {
                    TypeKind const k = interner.kind(ty);
                    // D-CSUBSET-SUBNATIVE-ALU-FORMS: a sub-native Trunc result
                    // (I8/U8/I16/U16, joining Char) routes through
                    // registerOpWidthFlags → the promoted width-32 `mov` (low bits
                    // kept); the actual narrowing realizes at the byte/half-exact
                    // consumer. An I64/I128 result cannot arise (gated sources).
                    if (k != TypeKind::I32 && k != TypeKind::U32
                        && k != TypeKind::Char
                        && k != TypeKind::I8  && k != TypeKind::U8
                        && k != TypeKind::I16 && k != TypeKind::U16) {
                        return failConv(ty, "Trunc result",
                                        "the 32-bit, 16-bit, and byte result forms");
                    }
                }
                return true;
            }
            case MirOpcode::SExt: {
                // Source-width gate: the I32-source form (movsxd / SXTW, a
                // 32-bit source window) and the Char/I8-source BYTE form
                // (movsx r32,r/m8 / sxtb — D-CSUBSET-CHAR-STRING-VALUE-CODEGEN),
                // discriminated by the SOURCE type's width flag on the LIR inst.
                auto const operands = mir.instOperands(id);
                if (!operands.empty()) {
                    TypeId const ty = mir.instType(operands[0]);
                    // D-CSUBSET-SUBNATIVE-ALU-FORMS: signed narrow read-back — an I16
                    // source → movsx r/m16 / SXTH; an I8 source → the byte form
                    // (movsx r/m8 / SXTB, shared with Char). I32 → movsxd / SXTW.
                    if (ty.valid()
                        && interner.kind(ty) != TypeKind::I32
                        && interner.kind(ty) != TypeKind::Char
                        && interner.kind(ty) != TypeKind::I8
                        && interner.kind(ty) != TypeKind::I16) {
                        return failConv(ty, "SExt source",
                                        "the I32-, I16-, and byte-source forms");
                    }
                }
                return true;
            }
            case MirOpcode::ZExt: {
                // Source-width gate (FC3.5 sweep-c1 widened —
                // D-CSUBSET-ZEXT-32-TO-64 closed): TWO source shapes
                // are realized, discriminated by the SOURCE type's
                // width on the LIR inst (see the ZExt dispatch arm):
                //   * Bool — the byte widener (x86 movzx r64, r/m8;
                //     arm64 W-ORR mov over a 0/1 register);
                //   * U32  — the 32-to-64 zero-extend (x86
                //     `mov r32, r32` width-32 form, auto-zero-
                //     extending; arm64 the SAME W-ORR word — a
                //     W-register write zeroes the upper 32 bits).
                // An I32 source stays REJECTED: C's I32→wider
                // conversion sign-extends (mapCast routes it to SExt);
                // a ZExt-from-I32 reaching here is a lowering bug, not
                // a missing realization. U8/U16/Char sources through
                // the x86 byte-form would read one byte of a wider
                // value — fail loud until their forms are encoded.
                auto const operands = mir.instOperands(id);
                if (!operands.empty()) {
                    TypeId const ty = mir.instType(operands[0]);
                    // D-CSUBSET-SUBNATIVE-ALU-FORMS: unsigned narrow read-back — a U16
                    // source → movzx r/m16 / UXTH; a U8 source → movzx r/m8 / UXTB.
                    // Bool/U32 keep their existing widener forms.
                    if (ty.valid()
                        && interner.kind(ty) != TypeKind::Bool
                        && interner.kind(ty) != TypeKind::U32
                        && interner.kind(ty) != TypeKind::U8
                        && interner.kind(ty) != TypeKind::U16) {
                        return failConv(ty, "ZExt source",
                                        "the Bool-, U32-, U16-, and U8-source forms");
                    }
                }
                return true;
            }
            default:
                return true;
        }
    }

    // FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): the LIR width flag for a MIR
    // type — the SINGLE point where TypeKind becomes the width axis.
    // I32/U32 compute at 32 bits (true C int semantics; x86 32-bit ops
    // auto-zero-extend, arm64 W-forms clear the upper word); FC3.5
    // sweep-c2 adds F32 → 32 (D-CSUBSET-F32-CODEGEN: the F3-prefixed
    // SSE scalar-single forms / arm64 S-forms ride the SAME axis — an
    // F32-typed float op selects the width-32 variants of fadd / fdiv
    // / fcmp / fp_to_si / movsd_load / fldur …). Everything else stays
    // at the 64-bit substrate default (I64/U64/F64/Ptr native; Bool/
    // byte ops are width-invariant at 64; the gated kinds never reach
    // an emission site). Applied at the COMPUTE tier + FPR-class
    // memory ops — integer plumbing movs / loads / stores / spills
    // stay 64-bit full-width (value-correct: every 32-bit-typed
    // CONSUMER reads only the low 32 bits of its operand registers, and
    // widening happens only through the explicit conversion mnemonics),
    // but an FPR LOAD/STORE must be width-exact: an 8-byte movsd read
    // of a 4-byte F32 rodata item would read past the item (see
    // lowerLoad/lowerStore). FPR-class register COPIES stay width-
    // blind by design (x86 movaps / arm64 fmov-D copy the containing
    // register; the low 4 bytes — the F32 value — ride along), and
    // regalloc spill/reload uses the width-default 8-byte forms over
    // 8-byte slots (value-preserving for both float widths).
    // C 6.7.2.2: an enum has NO representation of its own — it IS its
    // underlying integer type (scalars[0], default I32). Every width decision
    // below MUST see that underlying scalar, never the `Enum` wrapper: a raw
    // `interner.kind()` on an enum falls to the `default` (64-bit) arm and
    // emits an OVER-WIDE access of a PACKED enum struct-field / array-element
    // — the D-LIR-INT-MEMORY-WIDTH-EXACT clobber class (a scalar enum local
    // sits in its own ≥8-byte slot, so it MASKS this — caught in the FC8
    // enum review). This is the single enum→underlying resolve point for the
    // width tier: `widthFlagsForType` + `memAccessWidthFlags` (and, via the
    // former, `registerOpWidthFlags`) all switch on `reprKind(ty)`, so the
    // projection is by-construction at every width site. `regClassForType`
    // needs no change — `regClassForCoreType` already maps Enum → GPR; and the
    // layout authority (`computeLayout`) already sizes an enum as its
    // underlying, so field offsets and this access width can never disagree.
    // Aggregates (Struct/Union/Array) keep their own kind — they are not
    // scalars and route through the multi-leaf memory path, not these switches.
    [[nodiscard]] TypeKind reprKind(TypeId ty) const {
        TypeKind const k = interner.kind(ty);
        if (k == TypeKind::Enum) {
            auto const sc = interner.scalars(ty);
            if (!sc.empty()) return static_cast<TypeKind>(sc[0]);
        }
        return k;
    }

    [[nodiscard]] std::uint8_t widthFlagsForType(TypeId ty) const {
        if (!ty.valid()) return 0;
        switch (reprKind(ty)) {
            case TypeKind::I32: case TypeKind::U32: case TypeKind::F32:
                return kLirInstFlagWidth32;
            // Sub-native EXTENSION-SOURCE / memory widths (D-CSUBSET-SUBNATIVE-ALU-FORMS
            // + D-CSUBSET-CHAR-STRING-VALUE-CODEGEN): the byte/half-word conversion forms
            // (movsx/movzx r32,r/m8 + r/m16; sxtb/sxth/uxtb/uxth) read EXACTLY the low
            // 1/2 bytes of a register. `char`/`signed char`/`unsigned char` (I8/U8) are
            // 1 byte; `short`/`unsigned short` (I16/U16) are 2 — and a memory op of one
            // MUST be width-exact (a 64-bit load of a 1-byte element reads 7 bytes past).
            // registerOpWidthFlags re-promotes these to 32 for register PLUMBING — only
            // the extension SOURCE + memory access stay byte/half-exact.
            case TypeKind::Char: case TypeKind::I8: case TypeKind::U8:
                return kLirInstFlagWidth8;
            case TypeKind::I16: case TypeKind::U16:
                return kLirInstFlagWidth16;
            default:
                return 0;
        }
    }

    // The width flag for a MEMORY access (Load/Store) of `ty` into class `cls`. An
    // FPR access must be width-EXACT (movss vs movsd — an 8-byte read of a 4-byte
    // F32 reads past it). A GPR integer access must ALSO be byte-EXACT to its
    // type's size: a 64-bit access of a PACKED sub-8-byte element (an array
    // element or a struct field — NOT a stand-alone scalar local, which sits in
    // its own ≥8-byte slot) writes/reads past it, clobbering the neighbour or
    // overrunning the frame slot (D-LIR-INT-MEMORY-WIDTH-EXACT — the bug the
    // array-storage cycle exposed; the FC7 struct-field path was latently
    // affected too). Width by integer byte size; 8-byte ints/pointers keep the
    // width-default (0 ⇒ 64) full-register access.
    [[nodiscard]] std::uint8_t memAccessWidthFlags(TypeId ty, LirRegClass cls) const {
        if (cls == LirRegClass::FPR) return widthFlagsForType(ty);
        if (!ty.valid()) return 0;
        switch (reprKind(ty)) {   // enum → underlying int (see reprKind)
            // 1-byte integers (Char already required byte-exactness;
            // I8/U8/Bool/Byte join it — a packed signed/unsigned-char or bool
            // element must be 1-byte-exact too).
            case TypeKind::Char: case TypeKind::Byte:
            case TypeKind::I8:   case TypeKind::U8: case TypeKind::Bool:
                return kLirInstFlagWidth8;
            // 2-byte integers (short). The memory access is width-exact; the
            // short→int promote (SExt/ZExt-from-16) is a SEPARATE gap
            // (D-CSUBSET-SUBNATIVE-ALU-FORMS), so value use still fails loud.
            case TypeKind::I16: case TypeKind::U16:
                return kLirInstFlagWidth16;
            // 4-byte integers (int; and `long` under LLP64, already resolved to
            // I32 by the dataModel — width-32 is the correct 4-byte access).
            case TypeKind::I32: case TypeKind::U32:
                return kLirInstFlagWidth32;
            // 8-byte integers/pointers — width-default full-slot access.
            default:
                return 0;
        }
    }

    // The width for a REGISTER-RESIDENT value operation — const
    // materialization, a register copy/mov, a narrowing Trunc result — of
    // type `ty`. D-CSUBSET-CHAR-STRING-VALUE-CODEGEN: a sub-native byte
    // type (`char` ≡ I8) has NO byte-width register-immediate / register-
    // move / register-ALU form (and an 8-bit register WRITE is a partial-
    // register hazard — stale upper bits), so a char VALUE lives PROMOTED
    // in a ≥32-bit register, low-bits-significant — exactly C integer
    // promotion (6.3.1.1). The byte width therefore collapses to the 32-
    // bit form (whose 32-bit register write zero-extends into the full
    // register; the low byte holds the char). This is the COMPLEMENT of
    // the two byte-EXACT contexts: MEMORY access (`memAccessWidthFlags` —
    // a byte load/store must touch exactly 1 byte) and an extension SOURCE
    // (`widthFlagsForType` at the SExt/ZExt source — movsx/movzx r/m8
    // reads exactly the low byte). Only register PLUMBING promotes; memory
    // and extension sources stay byte-exact. (Every non-byte type is
    // returned unchanged, so this is byte-identical for all pre-char code.)
    [[nodiscard]] std::uint8_t registerOpWidthFlags(TypeId ty) const {
        std::uint8_t const w = widthFlagsForType(ty);
        // A sub-32-bit register WRITE is a partial-register hazard (stale upper
        // bits), so a sub-native value lives PROMOTED in a 32-bit register (low
        // bits significant — C integer promotion 6.3.1.1). BOTH the byte (Char/
        // I8/U8) and half-word (I16/U16) widths collapse to the 32-bit plumbing
        // form; the narrowing realizes lazily at the byte/half-exact CONSUMER
        // (a SExt/ZExt source movsx/movzx, or a width-exact store).
        return (w == kLirInstFlagWidth8 || w == kLirInstFlagWidth16)
                   ? kLirInstFlagWidth32 : w;
    }

    // ── diagnostics ──────────────────────────────────────────────────

    void reportMissingOpcode(MnemonicSlot slot, std::string_view context) {
        auto& entry = cache_[static_cast<std::size_t>(slot)];
        if (entry.missingReported) return;
        entry.missingReported = true;
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_RequiredLirOpcodeMissing;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "target '{}' declares no '{}' opcode (required for lowering {})",
            target.name(), entry.mnemonic, context);
        reporter.report(std::move(d));
    }

    void reportUnsupported(MirOpcode op, MirInstId at) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "MIR opcode '{}' is not yet lowered to target '{}' (inst {})",
            mirOpcodeName(op), target.name(), at.v);
        reporter.report(std::move(d));
    }

    void reportDoubleDef(MirInstId at) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "MIR inst %{} would re-define a value already mapped — structural "
            "violation (SSA single-definition broken)",
            at.v);
        reporter.report(std::move(d));
    }

    // ── value/block map plumbing ─────────────────────────────────────

    [[nodiscard]] std::optional<LirReg> regForValue(MirInstId v) {
        // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): a body-local alloca's
        // address is NOT cached in `valueToReg` — it is rematerialized at EACH use
        // so its live range stays tiny (no spill / scratch-pool exhaustion under the
        // c69 entry-hoist). Emit a fresh `lea_frame_slot k` here and return it. This
        // is the SOLE chokepoint every alloca-address consumer (Load/Store base,
        // address-of, pointer arithmetic, call args, phi-edge moves) funnels
        // through, so one interception covers them all. The emit lands in the
        // CURRENT open block at the use position (regForValue is only called while
        // lowering a use), so the recomputed `lea` precedes the use — and, for a
        // phi-incoming alloca, lands in the predecessor before its branch (correct:
        // the address is valid anywhere in the frame).
        if (v.valid()) {
            if (auto it = allocaSlotIndex_.find(v.v);
                it != allocaSlotIndex_.end()) {
                return emitLeaFrameSlot(it->second);
            }
        }
        if (!v.valid() || !valueToReg.has(v)) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "MIR value %{} used before definition during LIR lowering — "
                "either a structural violation or a deferred-opcode dependency",
                v.v);
            reporter.report(std::move(d));
            return std::nullopt;
        }
        return valueToReg.get(v);
    }

    bool defineValue(MirInstId id, LirReg reg) {
        if (valueToReg.has(id)) {
            reportDoubleDef(id);
            return false;
        }
        valueToReg.set(id, reg);
        return true;
    }

    // ── per-instruction lowering ─────────────────────────────────────

    // Non-terminator dispatcher (terminators flow through `lowerTerminator`
    // and Phi is a pre-pass no-op).
    void lowerInst(MirInstId id) {
        currentMir = id;
        MirOpcode const op = mir.instOpcode(id);
        // FC3 c1: sub-native integer compute fails loud BEFORE any arm
        // (D-CSUBSET-32BIT-ALU-FORMS — see requireNativeIntWidth).
        if (!requireNativeIntWidth(id, op)) return;
        switch (op) {
            case MirOpcode::Arg:    return lowerArg(id);
            case MirOpcode::ReturnPiece: return lowerReturnPiece(id);
            case MirOpcode::ReadIndirectResult: return lowerReadIndirectResult(id);
            case MirOpcode::VaRegSaveAreaAddr:
                return lowerVaFrameAddr(id, MnemonicSlot::VaRegSaveArea,
                                        "MIR VaRegSaveAreaAddr");
            case MirOpcode::VaOverflowArgAreaAddr:
                return lowerVaOverflowArgArea(id);
            case MirOpcode::VaHomeArgAreaAddr:
                return lowerVaHomeArgArea(id);
            case MirOpcode::RecvByValueStackParam:
                return lowerRecvByValueStackParam(id);
            case MirOpcode::Const:  return lowerConst(id);
            case MirOpcode::Add:    return lowerBinaryOp(id, MnemonicSlot::Add);
            case MirOpcode::Sub:    return lowerBinaryOp(id, MnemonicSlot::Sub);
            case MirOpcode::Mul:    return lowerBinaryOp(id, MnemonicSlot::Mul);
            case MirOpcode::SDiv:   return lowerDivLike(id, /*isSigned=*/true,  /*wantRemainder=*/false);
            case MirOpcode::UDiv:   return lowerDivLike(id, /*isSigned=*/false, /*wantRemainder=*/false);
            case MirOpcode::SMod:   return lowerDivLike(id, /*isSigned=*/true,  /*wantRemainder=*/true);
            case MirOpcode::UMod:   return lowerDivLike(id, /*isSigned=*/false, /*wantRemainder=*/true);
            case MirOpcode::ICmpEq: case MirOpcode::ICmpNe:
            case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
            case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge:
            case MirOpcode::ICmpUlt: case MirOpcode::ICmpUle:
            case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge: {
                // Even though this dispatch arm gates `op` to the
                // ICmp* set, route through the optional return so a
                // future MirOpcode added to this arm but NOT to
                // `condCodeForICmp`'s switch surfaces as a loud
                // diagnostic instead of UB blind-deref. The poison
                // call prevents downstream `regForValue` from
                // cascading "used before definition" diagnostics.
                auto const cc = condCodeForICmp(op);
                if (!cc.has_value()) {
                    reportUnsupported(op, id);
                    poisonValue(id);
                    return;
                }
                return lowerICmp(id, *cc);
            }
            // ── FC3.5 sweep-c2: float comparisons ──────────────────
            // The 7 C-reachable predicates (Oeq/One/Olt/Ole/Ogt/Oge +
            // Une) lower via the capability-driven plan; the 5
            // unordered-relational predicates (Ueq/Ult/Ule/Ugt/Uge)
            // have NO C producer and fail loud through the nullopt
            // plan — never a silent wrong-parity condition.
            case MirOpcode::FCmpOeq: case MirOpcode::FCmpOne:
            case MirOpcode::FCmpOlt: case MirOpcode::FCmpOle:
            case MirOpcode::FCmpOgt: case MirOpcode::FCmpOge:
            case MirOpcode::FCmpUeq: case MirOpcode::FCmpUne:
            case MirOpcode::FCmpUlt: case MirOpcode::FCmpUle:
            case MirOpcode::FCmpUgt: case MirOpcode::FCmpUge:
                return lowerFCmp(id);
            case MirOpcode::Phi:    return;  // pre-pass-allocated; no body emission
            case MirOpcode::Alloca: return lowerAlloca(id);
            case MirOpcode::Load:   return lowerLoad(id);
            case MirOpcode::Store:  return lowerStore(id);
            case MirOpcode::Gep:    return lowerGep(id);
            case MirOpcode::Trunc:
                // D-CSUBSET-CHAR-INT-WIDENING (int→char direction): a Trunc
                // whose RESULT is `char` keys on `registerOpWidthFlags` (the
                // promoted width-32 `mov r32,r32` — keeps the low 32 bits,
                // the low byte being the char), NOT the byte-exact result
                // width (there is no 8-bit trunc form; the char is consumed
                // low-bits-only). Every wider Trunc (I64→I32) is unchanged.
                return lowerCast(id, MnemonicSlot::Trunc, "MIR Trunc",
                                 registerOpWidthFlags(mir.instType(id)));
            case MirOpcode::SExt: {
                // Like ZExt, the encoded SExt form is selected by the SOURCE
                // type's width (D-CSUBSET-CHAR-STRING-VALUE-CODEGEN): an I32 source
                // → the movsxd/SXTW 32-bit form; a Char source → the byte form
                // (movsx r32,r/m8 / sxtb). The width axis rides the same LirInst flag.
                auto const sextOps = mir.instOperands(id);
                std::uint8_t const srcWidthFlags = (sextOps.size() == 1)
                    ? widthFlagsForType(mir.instType(sextOps[0]))
                    : 0;
                return lowerCast(id, MnemonicSlot::SExt, "MIR SExt", srcWidthFlags);
            }
            case MirOpcode::ZExt: {
                // FC3.5 sweep-c1 (D-CSUBSET-ZEXT-32-TO-64): zext is
                // the ONE conversion whose encoded form is selected by
                // the SOURCE type's width, not the result's (the
                // result is always the 64-bit register) — a Bool
                // source keeps the width-default byte widener (x86
                // movzx r64, r/m8), a U32 source selects the width-32
                // variant (x86 `mov r32, r32`, whose 32-bit register
                // write zero-extends; routing a U32 through the byte
                // form would silently read ONE byte). The width axis
                // rides the same LirInst flag the ALU tier uses; the
                // requireNativeIntWidth ZExt arm has already walled
                // off every other source kind.
                auto const zextOps = mir.instOperands(id);
                std::uint8_t const srcWidthFlags = (zextOps.size() == 1)
                    ? widthFlagsForType(mir.instType(zextOps[0]))
                    : 0;
                return lowerCast(id, MnemonicSlot::ZExt, "MIR ZExt",
                                 srcWidthFlags);
            }
            case MirOpcode::Bitcast:    return lowerBitcast(id);
            case MirOpcode::IntToPtr:
            case MirOpcode::PtrToInt:
                // Identity cast between integer and pointer on x86_64
                // — both are 64-bit GPR-class values. Single `mov`.
                return lowerCast(id, MnemonicSlot::Mov, "MIR IntToPtr/PtrToInt");
            // ── cycle 3d bitwise + Neg ─────────────────────────────
            case MirOpcode::And:    return lowerBinaryOp(id, MnemonicSlot::And);
            case MirOpcode::Or:     return lowerBinaryOp(id, MnemonicSlot::Or);
            case MirOpcode::Xor:    return lowerBinaryOp(id, MnemonicSlot::Xor);
            // FC3.5 sweep-c1: shifts route through the capability-
            // driven realization (immediate-count / implicit-count
            // register / native 3-address — see lowerShift).
            case MirOpcode::Shl:    return lowerShift(id, MnemonicSlot::Shl);
            case MirOpcode::LShr:   return lowerShift(id, MnemonicSlot::ShrL);
            case MirOpcode::AShr:   return lowerShift(id, MnemonicSlot::ShrA);
            case MirOpcode::Not:    return lowerUnaryOp(id, MnemonicSlot::Not);
            case MirOpcode::Neg:    return lowerUnaryOp(id, MnemonicSlot::Neg);
            // ── cycle 3d float arithmetic (FPR-class result) ───────
            case MirOpcode::FAdd:
                // FC2 Part B / FC3.5 c2: fadd carries the F64 addsd +
                // F32 addss encodings, width-axis-selected — gate the
                // width before the class-blind lowering (the result
                // type IS the operands' type post-UAC, so the default
                // result-width key is the right axis).
                if (!requireEncodedFloatWidth(id, mir.instType(id),
                                              "MIR FAdd")) return;
                return lowerBinaryOp(id, MnemonicSlot::FAdd);
            case MirOpcode::FSub:
                // Cluster-F F3: fsub gains its encodings (SUBSD/SUBSS, arm64
                // FSUB D/S) — same width gate+axis as FAdd/FDiv (an F16/F128
                // FSub would otherwise first-match the width:64 SUBSD variant —
                // the wrong-width silent-miscompile this gate walls off).
                if (!requireEncodedFloatWidth(id, mir.instType(id),
                                              "MIR FSub")) return;
                return lowerBinaryOp(id, MnemonicSlot::FSub);
            case MirOpcode::FMul:
                // Cluster-F F3: fmul gains its encodings (MULSD/MULSS, arm64
                // FMUL D/S) — same gate+axis.
                if (!requireEncodedFloatWidth(id, mir.instType(id),
                                              "MIR FMul")) return;
                return lowerBinaryOp(id, MnemonicSlot::FMul);
            case MirOpcode::FDiv:
                // FC3.5 sweep-c2: fdiv gains its first encodings
                // (DIVSD/DIVSS, arm64 FDIV D/S) — the NaN-construction
                // path (0.0/0.0). Same gate+axis as FAdd.
                if (!requireEncodedFloatWidth(id, mir.instType(id),
                                              "MIR FDiv")) return;
                return lowerBinaryOp(id, MnemonicSlot::FDiv);
            case MirOpcode::FNeg:   return lowerFNeg(id);
            // ── cycle 3d float casts (the 6 conversion variants) ───
            case MirOpcode::FPTrunc:
            case MirOpcode::FPExt: {
                // FC3.5 sweep-c2: fpcvt gains the F64↔F32 encodings
                // (x86 CVTSD2SS F2 0F 5A / CVTSS2SD F3 0F 5A; arm64
                // FCVT S↔D). ONE mnemonic, SOURCE-width-keyed
                // variants: FPExt's F32 source selects the width-32
                // form (cvtss2sd — widen), FPTrunc's F64 source the
                // width-64 form (cvtsd2ss — narrow); the result-width
                // default would pick exactly the WRONG sibling, so
                // this is the second consumer of the ZExt-style
                // widthOverride. Gate BOTH ends: an F16/F128 source
                // or result has no encoded conversion at any width.
                auto const cvtOps = mir.instOperands(id);
                if (!requireEncodedFloatWidth(id, mir.instType(id),
                        op == MirOpcode::FPExt ? "MIR FPExt (result)"
                                               : "MIR FPTrunc (result)")) {
                    return;
                }
                if (cvtOps.size() == 1
                    && !requireEncodedFloatWidth(id, mir.instType(cvtOps[0]),
                            op == MirOpcode::FPExt ? "MIR FPExt (source)"
                                                   : "MIR FPTrunc (source)")) {
                    return;
                }
                std::uint8_t const cvtSrcWidth = (cvtOps.size() == 1)
                    ? widthFlagsForType(mir.instType(cvtOps[0]))
                    : 0;
                return lowerCast(id, MnemonicSlot::FpCvt,
                                 op == MirOpcode::FPExt ? "MIR FPExt"
                                                        : "MIR FPTrunc",
                                 cvtSrcWidth);
            }
            case MirOpcode::FPToSI: {
                // FC2 Part B / FC3.5 c2: fp_to_si carries the F64
                // cvttsd2si + F32 cvttss2si encodings — the SOURCE
                // operand's float width is the encoded axis (the
                // result is integer/GPR and stays the 64-bit form;
                // its low 32 bits serve I32 results). The result-
                // width default (an I32 result → width-32) would
                // mis-key the SOURCE axis, so thread the override.
                auto const convOps = mir.instOperands(id);
                if (convOps.size() == 1
                    && !requireEncodedFloatWidth(id, mir.instType(convOps[0]),
                                                 "MIR FPToSI (source)")) {
                    return;
                }
                std::uint8_t const fpsiSrcWidth = (convOps.size() == 1)
                    ? widthFlagsForType(mir.instType(convOps[0]))
                    : 0;
                return lowerCast(id, MnemonicSlot::FpToSi, "MIR FPToSI",
                                 fpsiSrcWidth);
            }
            case MirOpcode::FPToUI: {
                // c78 (D-CSUBSET-FP-TO-UI-CODEGEN): fp_to_ui carries the
                // SAME CVTTSD2SI/CVTTSS2SI encodings as fp_to_si, keyed on
                // the SOURCE float width. A double→U32 conversion (sqlite's
                // occurrence) truncates via CVTTSD2SI r64 and the U32 result
                // reads its low 32 bits — VALUE-CORRECT because any double in
                // the U32 range [0, 2^32) fits the signed-64 CVTTSD2SI result
                // exactly (gcc emits the identical `cvttsd2si rax, xmm0`). The
                // full-range unsigned-i64 case (a double ≥ 2^63) needs the
                // conditional-subtract sequence — deferred
                // (D-CSUBSET-UI-FROM-FP-UNSIGNED-I64; sqlite stays in range).
                // Threads the source width like FPToSI (the result-width
                // default would mis-key the SOURCE axis).
                auto const fuOps = mir.instOperands(id);
                if (fuOps.size() == 1
                    && !requireEncodedFloatWidth(id, mir.instType(fuOps[0]),
                                                 "MIR FPToUI (source)")) {
                    return;
                }
                std::uint8_t const fpuiSrcWidth = (fuOps.size() == 1)
                    ? widthFlagsForType(mir.instType(fuOps[0]))
                    : 0;
                return lowerCast(id, MnemonicSlot::FpToUi, "MIR FPToUI",
                                 fpuiSrcWidth);
            }
            case MirOpcode::SIToFP:
            case MirOpcode::UIToFP: {
                // D-CSUBSET-INT-FLOAT-CONVERSION (int→float codegen): cvtsi2sd /
                // SCVTF. Like FPToSI's MIRROR, the encoded form keys on the SOURCE
                // INTEGER width, NOT the (float) result width — a 64-bit source
                // selects the REX.W cvtsi2sd xmm,r64 / SCVTF Dd,Xn form, a 32-bit
                // source the no-REX.W cvtsi2sd xmm,r32 / SCVTF Dd,Wn form. The
                // result-width default would mis-key the source axis (and a float
                // result has no integer width anyway), so thread the source's int
                // width as the override. The DESTINATION float is fixed at F64 (sd /
                // Dd) this cycle on BOTH targets — the variant guard carries ONE
                // width axis and the source-int axis OWNS it (REX.W / Wn-vs-Xn must
                // be exact), so a NON-F64 result (int→F32) has no encoding and FAILS
                // LOUD here rather than silently selecting a wrong-width form
                // (D-CSUBSET-INT-TO-F32-CODEGEN, deferred; sqlite uses `double`
                // only). A NARROW source (Char/I8/I16 — widthFlagsForType → 8/16)
                // also has no declared variant and fails loud at the matcher
                // (no partial-register conversion this cycle); the C int literal
                // `5` is I32 and the sqlite blocker is I64, both encoded.
                TypeKind const resultK = interner.kind(mir.instType(id));
                if (resultK != TypeKind::F64) {
                    dss::report(reporter,
                        DiagnosticCode::L_UnsupportedLoweringForOpcode,
                        DiagnosticSeverity::Error,
                        std::format(
                            "MIR {}: integer→float result TypeKind ordinal {} is "
                            "not lowerable to target '{}' — only an F64 (double) "
                            "destination has an int→float encoding this cycle; "
                            "proceeding would silently select a wrong-width "
                            "instruction form (D-CSUBSET-INT-TO-F32-CODEGEN)",
                            op == MirOpcode::SIToFP ? "SIToFP" : "UIToFP",
                            static_cast<unsigned>(resultK), target.name()));
                    poisonValue(id);
                    return;
                }
                auto const i2fOps = mir.instOperands(id);
                std::uint8_t const i2fSrcWidth = (i2fOps.size() == 1)
                    ? widthFlagsForType(mir.instType(i2fOps[0]))
                    : 0;
                return lowerCast(id,
                                 op == MirOpcode::SIToFP ? MnemonicSlot::SiToFp
                                                         : MnemonicSlot::UiToFp,
                                 op == MirOpcode::SIToFP ? "MIR SIToFP"
                                                         : "MIR UIToFP",
                                 i2fSrcWidth);
            }
            // ── cycle 3e: Calls + GlobalAddr ───────────────────────
            case MirOpcode::GlobalAddr:    return lowerGlobalAddr(id);
            // D-CSUBSET-COMPUTED-GOTO: `&&label` block address materialization.
            case MirOpcode::BlockAddress:  return lowerBlockAddress(id);
            case MirOpcode::ByValueStackArg: return lowerByValueStackArg(id);
            case MirOpcode::Call:          return lowerCall(id);
            case MirOpcode::IntrinsicCall: return lowerIntrinsicCall(id);
            // ── cycle 3e: aggregate ops (memory-flattening lowering) ──
            case MirOpcode::ExtractValue:  return lowerExtractValue(id);
            case MirOpcode::InsertValue:   return lowerInsertValue(id);
            default: break;
        }
        reportUnsupported(op, id);
    }

    void lowerArg(MirInstId id) {
        if (!opcode(MnemonicSlot::Arg).has_value()) {
            reportMissingOpcode(MnemonicSlot::Arg, "MIR Arg");
            return;
        }
        // Cycle 3d: Arg's result reg class follows the parameter type
        // (F32/F64 → FPR; integer/ptr → GPR). ML7 callconv lowering
        // reads the result class to pick the right arg-passing
        // register per the cycle-2b TargetCallingConvention.
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*opcode(MnemonicSlot::Arg), result, std::span<LirOperand const>{},
                    /*payload=*/mir.argIndex(id));
        defineValue(id, result);
    }

    // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): the k-th return-register piece
    // of a struct-returning call. The MIR `[call]` operand is the ordering anchor
    // only (it keeps this read immediately after its call); the LIR `ret_piece` is
    // a leaf carrying the per-class return ordinal as payload. lir_callconv
    // captures it from the cc's return register (the caller-side mirror of `arg`).
    void lowerReturnPiece(MirInstId id) {
        if (!opcode(MnemonicSlot::RetPiece).has_value()) {
            reportMissingOpcode(MnemonicSlot::RetPiece, "MIR ReturnPiece");
            return;
        }
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*opcode(MnemonicSlot::RetPiece), result, std::span<LirOperand const>{},
                    /*payload=*/mir.returnPieceOrdinal(id));
        defineValue(id, result);
    }

    // FC7 C3 (AAPCS64/Apple x8 sret): the callee-side entry read of the indirect-
    // result register. A leaf (no operands), result = the incoming result-storage
    // pointer. The virtual `read_indirect_result` LIR op carries no payload;
    // lir_callconv materializes it at entry as `mov result, <indirectResultRegister>`
    // (the callee mirror of `arg`). Fail loud if the schema lacks the opcode.
    void lowerReadIndirectResult(MirInstId id) {
        if (!opcode(MnemonicSlot::ReadIndirectResult).has_value()) {
            reportMissingOpcode(MnemonicSlot::ReadIndirectResult,
                                "MIR ReadIndirectResult");
            return;
        }
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*opcode(MnemonicSlot::ReadIndirectResult), result,
                 std::span<LirOperand const>{}, /*payload=*/0);
        defineValue(id, result);
    }

    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the frame-relative VaRegSaveAreaAddr leaf
    // lowers to its virtual LIR op (no operands, GPR-class pointer result, payload 0 —
    // the register-save-area base carries NO displacement). lir_callconv materializes
    // it into a `lea result, [sp + offset]` once it owns the frame layout — the
    // `alloca`/`read_indirect_result` precedent. Fail loud if the schema lacks the
    // opcode. (VaOverflowArgAreaAddr is lowered SEPARATELY — see lowerVaOverflowArgArea
    // — because it THREADS the MIR fixed-stack-arg displacement payload.)
    void lowerVaFrameAddr(MirInstId id, MnemonicSlot slot, char const* what) {
        if (!opcode(slot).has_value()) {
            reportMissingOpcode(slot, what);
            return;
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);   // a pointer
        emitInst(*opcode(slot), result, std::span<LirOperand const>{}, /*payload=*/0);
        defineValue(id, result);
    }

    // FC12-deferral④ (D-FC12A/C-VARIADIC-OVERFLOW-FIXED-STACK-ARGS): the
    // VaOverflowArgAreaAddr leaf, like lowerVaFrameAddr but THREADS the MIR payload
    // (the fixed-stack-arg byte displacement — bytes of named params that overflowed
    // onto the incoming stack; 0 for the common case) into the LIR op. lir_callconv
    // reads it and adds it to the overflow base `totalFrameSize + callPushBytes +
    // shadowSpaceBytes + payload`. Without this threading the displacement would be
    // discarded (the generic lowerVaFrameAddr hardcodes payload 0) → overflow_arg_area
    // / __stack would point AT a named stack param, a silent miscompile.
    void lowerVaOverflowArgArea(MirInstId id) {
        if (!opcode(MnemonicSlot::VaOverflowArgArea).has_value()) {
            reportMissingOpcode(MnemonicSlot::VaOverflowArgArea,
                                "MIR VaOverflowArgAreaAddr");
            return;
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);   // a pointer
        emitInst(*opcode(MnemonicSlot::VaOverflowArgArea), result,
                 std::span<LirOperand const>{}, /*payload=*/mir.instPayload(id));
        defineValue(id, result);
    }

    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the callee-side mirror of
    // lowerByValueStackArg — a fixed by-value aggregate received WHOLLY from the
    // incoming stack. Like lowerVaOverflowArgArea, THREADS the MIR payload (this
    // aggregate's byte offset within the incoming overflow area) into the LIR op;
    // lir_callconv materializes it to `lea result, [sp + totalFrameSize +
    // callPushBytes + shadowSpaceBytes + payload]`. HIR→MIR byte-copies from `result`
    // into the param's local slot.
    void lowerRecvByValueStackParam(MirInstId id) {
        if (!opcode(MnemonicSlot::RecvByValueStackParam).has_value()) {
            reportMissingOpcode(MnemonicSlot::RecvByValueStackParam,
                                "MIR RecvByValueStackParam");
            return;
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);   // a pointer
        emitInst(*opcode(MnemonicSlot::RecvByValueStackParam), result,
                 std::span<LirOperand const>{}, /*payload=*/mir.instPayload(id));
        defineValue(id, result);
    }

    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the Win64 HomogeneousPointer va_start
    // base. Like lowerVaFrameAddr but THREADS the MIR payload (the named-arg slot
    // count) into the LIR op — lir_callconv reads it to compute the contiguous home
    // offset `totalFrameSize + callPushBytes + payload*outgoingSlotSize`.
    void lowerVaHomeArgArea(MirInstId id) {
        if (!opcode(MnemonicSlot::VaHomeArgArea).has_value()) {
            reportMissingOpcode(MnemonicSlot::VaHomeArgArea, "MIR VaHomeArgAreaAddr");
            return;
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);   // a pointer
        emitInst(*opcode(MnemonicSlot::VaHomeArgArea), result,
                 std::span<LirOperand const>{}, /*payload=*/mir.instPayload(id));
        defineValue(id, result);
    }

    // Emit a single-operand class-correct register write and define the
    // result vreg. Shared tail of `lowerConst`'s narrow + wide paths
    // (the only difference between them is the operand-kind in `op`).
    // FC2 Part B: the mnemonic comes from the per-register-class table
    // (GPR → the universal `mov`; FPR → the class's declared move, e.g.
    // x86_64 movaps) — a GPR mov against an FPR ordinal mis-encodes.
    // FC3 c2: optional width `flags` (lowerConst threads the const's
    // type width so a 32-bit-typed immediate selects the EXACT 32-bit
    // mov-imm form; every other caller's copy stays width-default).
    // Returns the result reg so callers can chain.
    LirReg emitMovToFresh(MirInstId id, LirOperand op, LirRegClass cls,
                          std::uint8_t flags = 0) {
        auto const movOp = classOp(cls, RegClassOp::Move);
        if (!movOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Move, "MIR value copy");
            poisonValue(id);
            return valueToReg.get(id);
        }
        LirReg const result = lir.newVReg(cls);
        std::array<LirOperand, 1> ops{op};
        emitInst(*movOp, result, ops, /*payload=*/0, flags);
        defineValue(id, result);
        return result;
    }

    // D-OPT-SWITCH-JUMP-TABLE (c70): materialize a full 64-bit integer constant
    // into a FRESH GPR vreg with NO MIR-value binding (`emitMovToFresh` /
    // `materializeInlineIntConst` bind to a MirInstId; the jump-table lowering
    // needs synthetic scratch constants — the `* 8` slot-scale multiplier, and a
    // wide `minCase` / `span-1` bound that does not fit the target's `cmp`/`sub`
    // immediate — that have no MIR value). Emits a single `mov reg, imm32` when
    // the value fits a mov-immediate; otherwise (a value needing higher 16-bit
    // chunks on arm64) the MOVZ+MOVK ladder, the SAME minimal-chain logic as
    // `materializeInlineIntConst`, so a wide bound is arch-correct on arm64 (x86
    // takes the single mov-imm64 form). Returns nullopt (with a fail-loud
    // diagnostic) if a required opcode is undeclared.
    [[nodiscard]] std::optional<LirReg>
    emitBareConstToFresh(std::int64_t value) {
        auto const movOp = classOp(LirRegClass::GPR, RegClassOp::Move);
        if (!movOp.has_value()) {
            reportMissingClassOp(LirRegClass::GPR, RegClassOp::Move,
                                 "jump-table scratch constant");
            return std::nullopt;
        }
        std::uint64_t const pattern = static_cast<std::uint64_t>(value);
        std::array<std::uint16_t, 4> chunks{};
        for (std::size_t k = 0; k < 4; ++k) {
            chunks[k] = static_cast<std::uint16_t>(pattern >> (16 * k));
        }
        bool needsChain = false;
        for (std::size_t k = 1; k < 4; ++k) {
            if (chunks[k] != 0) { needsChain = true; break; }
        }
        // Single mov-imm covers the whole value on x86 (imm64 form) and any value
        // whose high 48 bits are zero on arm64 (a plain MOVZ). A signed value with
        // sign bits set in the upper chunks (e.g. a small negative) also takes the
        // single-mov path on x86; arm64 requires the ladder only for a genuinely
        // wide MAGNITUDE — the callers here pass a non-negative bound in
        // [0, 2^20-1] (span-1) or a signed minCase in i32 range, both of which the
        // single mov handles on x86, and on arm64 the ladder handles the >16-bit
        // magnitudes.
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        if (!needsChain || !targetHasMovkLadder()) {
            std::array<LirOperand, 1> ops{
                LirOperand::makeImmInt32(static_cast<std::int32_t>(value))};
            emitInst(*movOp, result, ops);   // 64-bit (default width)
            return result;
        }
        // arm64 MOVZ + MOVK ladder: seed chunk0 (MOVZ zeroes the register), then
        // one MOVK per nonzero higher chunk. MOVK is `requires2Address` (it MERGES
        // a 16-bit chunk into the EXISTING register), so each MOVK carries the
        // register as its FIRST operand + the immediate as its second, writing
        // back into the SAME register — the exact operand shape
        // `materializeViaMovkLadder` uses. (A negative value fills all high chunks
        // with 0xFFFF → the full 4-op ladder, matching `materializeInlineIntConst`.)
        static constexpr std::array<MnemonicSlot, 3> kMovkSlots{
            MnemonicSlot::MovkLsl16, MnemonicSlot::MovkLsl32,
            MnemonicSlot::MovkLsl48};
        {
            std::array<LirOperand, 1> ops{
                LirOperand::makeImmInt32(static_cast<std::int32_t>(chunks[0]))};
            emitInst(*movOp, result, ops);
        }
        for (std::size_t k = 1; k < 4; ++k) {
            if (chunks[k] == 0) continue;
            auto const slotOp = opcode(kMovkSlots[k - 1]);
            if (!slotOp.has_value()) {
                reportMissingOpcode(kMovkSlots[k - 1], "jump-table wide constant");
                return std::nullopt;
            }
            std::array<LirOperand, 2> ops{
                LirOperand::makeReg(result),
                LirOperand::makeImmInt32(static_cast<std::int32_t>(chunks[k]))};
            emitInst(*slotOp, result, ops, /*payload=*/0);
        }
        return result;
    }

    // Build an ALU-op operand for a constant that is either an in-window immediate
    // (arm64 `imm12`, unsigned 0..4095 — the tightest across the targets) or a
    // register-materialized value (for a wider or negative constant). Returns
    // nullopt on a materialization failure. Used by the jump-table bounds
    // sub/cmp so a large `minCase` / `span-1` stays arch-correct on arm64.
    [[nodiscard]] std::optional<LirOperand>
    aluImmOrReg(std::int64_t value) {
        if (value >= 0 && value <= 4095) {
            return LirOperand::makeImmInt32(static_cast<std::int32_t>(value));
        }
        std::optional<LirReg> const r = emitBareConstToFresh(value);
        if (!r.has_value()) return std::nullopt;
        return LirOperand::makeReg(*r);
    }

    // Define a poisoned-value vreg + return false so the caller can
    // signal "lowering failed but downstream uses get a quiet
    // placeholder, not a cascade of `regForValue` diagnostics". The
    // diagnostic for the ROOT cause is the caller's responsibility.
    void poisonValue(MirInstId id) {
        LirReg const placeholder = lir.newVReg(LirRegClass::GPR);
        valueToReg.set(id, placeholder);
    }

    // D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE (FC3.5 sweep-c3): materialize
    // an inline (int32-carried) integer constant.
    //
    //   * Single-chunk values (every 16-bit chunk above chunk 0 is
    //     zero) emit the ONE mov-imm byte-identically to the pre-sweep
    //     path — x86's `mov r, imm32` and arm64's MOVZ both cover
    //     them.
    //   * Wider values: when the target declares the MOVK ladder
    //     (`movk_lsl16/32/48` — probed by mnemonic presence, the
    //     sanctioned capability pattern), emit the MINIMAL chain:
    //     mov #chunk0 (arm64: MOVZ, zeroing the whole register) + one
    //     `movk_lslN` per NONZERO higher chunk, each a 2-address
    //     result==operand[0] insert into the SAME vreg. Selection by
    //     MAGNITUDE lives here in the lowering — an operand's VALUE
    //     cannot guard encoding variants.
    //   * Without the ladder (x86): the single mov keeps the whole
    //     value — its imm32 form swallows the full inline range, so
    //     nothing changes. A fixed32 target with a 16-bit mov-imm
    //     window and NO declared ladder keeps today's encode-time
    //     fail-loud (A_ImmediateOperandOutOfRange) — the ladder is
    //     ALL-OR-NOTHING per needed chunk; a half-declared family
    //     never silently emits a partial constant.
    //
    // The effective bit pattern: width-32 constants are the raw low
    // 32 bits (the 32-bit write zero-extends — chunks 2/3 never
    // exist); width-64 constants are the SIGN-EXTENDED 64-bit pattern
    // (the x86 `mov r64, imm32` semantics every consumer was lowered
    // against) — a negative constant therefore emits the full 4-op
    // ladder (chunk0 + 0xFFFF-filled high chunks). A 2-op MOVN-seeded
    // chain is a deliberate non-goal this cycle (peephole-class).
    void materializeInlineIntConst(MirInstId id, std::int32_t imm32,
                                   std::uint8_t widthFlags) {
        bool const is32 = lirInstWidthBits(widthFlags) == 32;
        std::uint64_t const pattern = is32
            ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(imm32))
            : static_cast<std::uint64_t>(static_cast<std::int64_t>(imm32));
        std::array<std::uint16_t, 4> chunks{};
        for (std::size_t k = 0; k < 4; ++k) {
            chunks[k] = static_cast<std::uint16_t>(pattern >> (16 * k));
        }
        std::size_t const nChunks = is32 ? 2 : 4;
        bool needsChain = false;
        for (std::size_t k = 1; k < nChunks; ++k) {
            if (chunks[k] != 0) { needsChain = true; break; }
        }
        bool const ladderDeclared = needsChain && targetHasMovkLadder();
        if (!needsChain || !ladderDeclared) {
            emitMovToFresh(id, LirOperand::makeImmInt32(imm32),
                           LirRegClass::GPR, widthFlags);
            return;
        }
        materializeViaMovkLadder(id, pattern, widthFlags);
    }

    // D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE / D-CSUBSET-BITFIELD-WIDE-UNIT:
    // emit the MOVZ + MOVK ladder for the full 64-bit `pattern`. Seeds
    // chunk0 with a mov-imm (arm64 MOVZ zeroes the whole register), then
    // one `movk_lslN` per NONZERO higher 16-bit chunk — the MINIMAL
    // chain. `widthFlags` of 32 truncates to the low 2 chunks (the
    // 32-bit write zero-extends; chunks 2/3 never exist); width 64 walks
    // all 4. Caller has verified the ladder is declared
    // (`targetHasMovkLadder()`); a chunk needing an undeclared slot is a
    // fail-loud `reportMissingOpcode` (never a silent partial constant).
    void materializeViaMovkLadder(MirInstId id, std::uint64_t pattern,
                                  std::uint8_t widthFlags) {
        bool const is32 = lirInstWidthBits(widthFlags) == 32;
        std::size_t const nChunks = is32 ? 2 : 4;
        std::array<std::uint16_t, 4> chunks{};
        for (std::size_t k = 0; k < 4; ++k) {
            chunks[k] = static_cast<std::uint16_t>(pattern >> (16 * k));
        }
        // The ladder slots, positionally chunk index 1..3.
        static constexpr std::array<MnemonicSlot, 3> kMovkSlots{
            MnemonicSlot::MovkLsl16, MnemonicSlot::MovkLsl32,
            MnemonicSlot::MovkLsl48};
        LirReg const r = emitMovToFresh(
            id,
            LirOperand::makeImmInt32(
                static_cast<std::int32_t>(chunks[0])),
            LirRegClass::GPR, widthFlags);
        for (std::size_t k = 1; k < nChunks; ++k) {
            if (chunks[k] == 0) continue;
            auto const slotOp = opcode(kMovkSlots[k - 1]);
            if (!slotOp.has_value()) {
                reportMissingOpcode(kMovkSlots[k - 1],
                                    "MIR Const wide-immediate ladder");
                return;
            }
            std::array<LirOperand, 2> const movkOps{
                LirOperand::makeReg(r),
                LirOperand::makeImmInt32(
                    static_cast<std::int32_t>(chunks[k]))};
            emitInst(*slotOp, r, movkOps, /*payload=*/0, widthFlags);
        }
    }

    void lowerConst(MirInstId id) {
        // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB (c94): if this Const's SOLE
        // use is a const-disp Gep that folds it into a `MemOffset` immediate
        // (`lowerGep` reads it via `constIntegerValue`, never `regForValue`),
        // SKIP the materialization — it would be a dead register write (the
        // arm64 MOVZ/MOVK of the displacement the folded `SUB`/`ADD` never
        // reads). Recorded in `foldedConstDisps_` so a stray `regForValue` on
        // this value (a fold-site disagreement) fails LOUD on the undefined
        // vreg rather than silently mis-encoding. Sign-agnostic: eliminates the
        // dead const on BOTH the positive member-offset `ADD` and the negative
        // `p[-N]` `SUB`. Mirrors the `foldedGlobalAddrs_` skip in `lowerGlobalAddr`.
        if (constDispFoldsIntoGep(id)) {
            foldedConstDisps_.insert(id.v);
            return;
        }
        if (!opcode(MnemonicSlot::Mov).has_value()) {
            reportMissingOpcode(MnemonicSlot::Mov, "MIR Const");
            return;
        }
        std::uint32_t const litIdx = mir.constLiteralIndex(id);
        MirLiteralValue const& lit = mir.literalValue(litIdx);

        // FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): a 32-bit-TYPED constant
        // (I32/U32) materializes through the width-32 mov-imm form —
        // the EXACT (non-sign-extending) 32-bit immediate write whose
        // zero-extension makes the producer upper-bit-clean. This also
        // WIDENS the inline range for 32-typed constants to the full
        // [INT32_MIN, UINT32_MAX]: an `unsigned int` 0xFFFFFFFF (e.g.
        // minted by ConstFold folding the u32_wraparound chain) IS a
        // 4-byte immediate by construction — routing it to the
        // LiteralPool dead end (no encoder consumes LiteralIndex) was
        // a 64-bit-era artifact of the int32-signed range check.
        // `registerOpWidthFlags` (not the byte-exact `widthFlagsForType`):
        // a char-TYPED constant materializes through the width-32 mov-imm
        // form (the value zero-extends into the full register; the low byte
        // IS the char) — there is no 8-bit mov-imm form, and a char value
        // is consumed low-bits-only (a later byte STORE / SExt re-reads the
        // low byte). D-CSUBSET-CHAR-STRING-VALUE-CODEGEN.
        std::uint8_t const constWidthFlags =
            registerOpWidthFlags(mir.instType(id));
        bool const is32Typed = constWidthFlags != 0;

        // ── narrow integer path: inline ImmInt32 ──
        std::int32_t imm32 = 0;
        bool fits = false;
        if (auto const* i = std::get_if<std::int64_t>(&lit.value)) {
            if (*i >= std::numeric_limits<std::int32_t>::min()
             && *i <= std::numeric_limits<std::int32_t>::max()) {
                imm32 = static_cast<std::int32_t>(*i);
                fits  = true;
            } else if (is32Typed && *i >= 0
                       && *i <= std::numeric_limits<std::uint32_t>::max()) {
                // U32-range value carried in the int64 arm: the low 32
                // bits ARE the value (the width-32 form writes them raw).
                imm32 = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(*i));
                fits  = true;
            }
        } else if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) {
            std::uint64_t const cap = is32Typed
                ? std::numeric_limits<std::uint32_t>::max()
                : static_cast<std::uint64_t>(
                      std::numeric_limits<std::int32_t>::max());
            if (*u <= cap) {
                imm32 = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(*u));
                fits  = true;
            }
        } else if (auto const* b = std::get_if<bool>(&lit.value)) {
            imm32 = *b ? 1 : 0;
            fits  = true;
        }
        if (fits) {
            // Narrow integer literal — GPR-class regardless of the
            // declared MIR type since the immediate IS an integer.
            // (Bool literals are also routed here as 0/1 at the
            // 64-bit default width.)
            //
            // D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE (FC3.5 sweep-c3): when
            // the value does not fit the single mov-imm form's
            // immediate window on a MOVK-declaring target, the
            // materialization splits into the minimal MOVZ + MOVK
            // ladder instead (see the helper).
            materializeInlineIntConst(id, imm32, constWidthFlags);
            return;
        }

        // ── wide-literal path ──
        //
        // Routing: int32-fits → ImmInt (above); wide int64/uint64 →
        // capability-probed (below, D-CSUBSET-BITFIELD-WIDE-UNIT);
        // string → pool with GPR-class result; monostate / aggregate →
        // unsupported + poison so dependent uses surface ONE root-cause
        // diagnostic rather than a cascade of "used before definition".
        // The float fail-loud runs FIRST (it must precede any pool
        // routing — a double must never reach the LiteralIndex path).
        //
        // FC2 Part B (F64 constant materialization): float (FPR-class)
        // constants must NOT reach MIR→LIR as `Const` — HIR→MIR
        // promotes F64 literals to anonymous rodata globals
        // (GlobalAddr + Load, the string-literal shape). The prior
        // double→LiteralPool route was a DEAD END: no encoding variant
        // consumes a LiteralIndex operand, so every FPR literal hit
        // A_NoMatchingEncodingVariant at assemble time. Fail loud HERE
        // (where the contract is visible) on EITHER signal — a double
        // variant arm, or an FPR-class result type (catches a non-
        // double literal hand-typed F64). Non-F64 float widths have
        // no promotion either (D-TARGET-ENCODING-WIDTH-GUARD).
        LirRegClass resultCls = regClassFor(id);
        if (std::holds_alternative<double>(lit.value)
            || resultCls == LirRegClass::FPR) {
            dss::report(reporter,
                DiagnosticCode::L_UnsupportedLoweringForOpcode,
                DiagnosticSeverity::Error,
                std::format(
                    "MIR Const %{} is a float (FPR-class) literal at "
                    "MIR→LIR — float constants must be promoted to "
                    "anonymous rodata globals (GlobalAddr + Load) at "
                    "HIR→MIR; the LirLiteralPool LiteralIndex route has "
                    "no encoder path (FC2 Part B; non-F64 widths: "
                    "D-TARGET-ENCODING-WIDTH-GUARD)", id.v));
            poisonValue(id);
            return;
        }
        // ── wide INTEGER constant (D-CSUBSET-BITFIELD-WIDE-UNIT) ──
        //
        // A >imm32 integer reaches here (e.g. a 40-bit bit-field mask, or
        // `unsigned long long x = 0xFFFFFFFF'FF`). It materializes at
        // width-64 by TARGET CAPABILITY — the sanctioned probe pattern
        // (declared variant/mnemonic vocabulary, NEVER `if (arch==...)`):
        //   * `mov r64, imm64` declared (x86_64) → keep the LiteralPool
        //     carrier (the wide value can't ride the 8-byte LirOperand
        //     POD inline) and emit ONE `mov r64, imm64` (B8+rd io). The
        //     encoder reads the value back from the pool.
        //   * the MOVZ/MOVK ladder declared (arm64) → emit the minimal
        //     MOVZ #chunk0 + MOVK-per-nonzero-chunk inline chain (no pool
        //     entry needed — each chunk rides an imm32 operand).
        //   * neither → fail loud (a target with no wide-const form).
        // Float literals are handled above (rodata promotion); strings
        // fall through to the pool path below. The SIGN/ZERO extension to
        // 64 bits matches what every 64-bit consumer was lowered against.
        {
            bool isWideInt = false;
            std::uint64_t value64 = 0;
            // Preserve the ORIGINAL variant arm (int64 vs uint64) for the
            // pool entry — the bit pattern is identical either way (the
            // encoder reads both via the same Imm64 path), but keeping the
            // arm avoids a surprising signedness flip for any pool
            // inspector. `value64` is the bit pattern, used by the ladder.
            LirLiteralValue lirLit;
            lirLit.core = lit.core;
            if (auto const* i = std::get_if<std::int64_t>(&lit.value)) {
                value64      = static_cast<std::uint64_t>(*i);
                lirLit.value = *i;
                isWideInt    = true;
            } else if (auto const* u =
                           std::get_if<std::uint64_t>(&lit.value)) {
                value64      = *u;
                lirLit.value = *u;
                isWideInt    = true;
            }
            if (isWideInt) {
                if (targetHasMovImm64()) {
                    std::uint32_t const lirLitIdx =
                        lir.literalPoolAdd(std::move(lirLit));
                    // width-default flags (0) = 64 bits — the imm64
                    // variant is width-64-keyed, and 64 is the
                    // `lirInstWidthBits` default (there is no explicit
                    // Width64 flag; the absent-flag state IS 64-bit).
                    emitMovToFresh(id,
                                   LirOperand::makeLiteralIndex(lirLitIdx),
                                   LirRegClass::GPR,
                                   /*flags=*/0);
                    return;
                }
                if (targetHasMovkLadder()) {
                    // width-default (0) = 64-bit ladder (all 4 chunks).
                    materializeViaMovkLadder(id, value64, /*flags=*/0);
                    return;
                }
                dss::report(reporter,
                    DiagnosticCode::L_UnsupportedLoweringForOpcode,
                    DiagnosticSeverity::Error,
                    std::format(
                        "MIR Const %{} is a 64-bit integer literal wider "
                        "than imm32, but the target declares neither a "
                        "`mov r64, imm64` variant nor the MOVZ/MOVK wide-"
                        "immediate ladder — it has no way to materialize a "
                        "wide constant (D-CSUBSET-BITFIELD-WIDE-UNIT)",
                        id.v));
                poisonValue(id);
                return;
            }
        }

        // ── remaining pool-routed literals (strings) ──
        //
        // Routing: int32-fits → ImmInt (above); wide int → capability
        // (above); string → pool with GPR-class result; monostate /
        // aggregate → unsupported + poison so dependent uses surface ONE
        // root-cause diagnostic rather than a cascade of "used before
        // definition".
        LirLiteralValue lirLit;
        lirLit.core = lit.core;
        bool unsupportedVariant = false;
        if (auto const* s = std::get_if<std::string>(&lit.value))   lirLit.value = *s;
        else {
            // monostate (malformed-source recovery), aggregate (cycle
            // 3e), or any future variant arm: fail loud + poison.
            unsupportedVariant = true;
        }
        if (unsupportedVariant) {
            reportUnsupported(MirOpcode::Const, id);
            poisonValue(id);
            return;
        }
        std::uint32_t const lirLitIdx = lir.literalPoolAdd(std::move(lirLit));
        emitMovToFresh(id, LirOperand::makeLiteralIndex(lirLitIdx), resultCls);
    }

    // ── memory ops (cycle 3c) ────────────────────────────────────────

    void lowerAlloca(MirInstId id) {
        if (!opcode(MnemonicSlot::Alloca).has_value()) {
            reportMissingOpcode(MnemonicSlot::Alloca, "MIR Alloca");
            return;
        }
        std::uint32_t const payload = mir.instPayload(id);
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        emitInst(*opcode(MnemonicSlot::Alloca), result,
                    std::span<LirOperand const>{}, payload);
        // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): the `alloca` op above
        // RESERVES the slot (lir_callconv assigns it a frame offset in scan order).
        // Record this alloca's scan-order index so EACH use of its address can be
        // rematerialized via `lea_frame_slot` instead of holding `result` live
        // across the whole (entry-spanning, post-hoist) range. `result` itself is
        // left def-only (a tiny range) — it is never entered into `valueToReg`, so
        // `regForValue` always routes alloca-address uses through the remat path.
        // `allocaLirCount_` post-increments: the index recorded is the 0-based
        // position of this `alloca` op among the function's alloca ops, == the
        // index lir_callconv's scan assigns its offset under.
        if (opcode(MnemonicSlot::LeaFrameSlot).has_value()) {
            allocaSlotIndex_.emplace(id.v, allocaLirCount_);
        } else {
            // Target declares `alloca` but no `lea_frame_slot` — cannot
            // rematerialize. Fall back to the pre-c69 single-vreg model (define
            // the address so `regForValue` hands it back directly). Correct, just
            // without the pressure relief (no shipped target hits this: both x86_64
            // and arm64 declare lea_frame_slot; a register-machine schema with
            // `alloca` but no `lea_frame_slot` is a config that predates c69).
            defineValue(id, result);
        }
        ++allocaLirCount_;
    }

    // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): emit a fresh `lea_frame_slot k`
    // re-reference of body-local-alloca `slotIndex`'s address into a brand-new GPR
    // vreg, and return it. Called from `regForValue` at each use of an alloca
    // address, so every use gets its OWN tiny-range address value (no long live
    // range, no spill, no scratch-pool exhaustion). lir_callconv materializes the op
    // into `lea result, [sp + offsetOf(slotIndex) + byteDisp]` — the SAME base offset
    // the original `alloca` resolves to, PLUS an optional constant byte displacement
    // `byteDisp` (default 0). The result is a pointer (GPR class), mirroring the
    // original alloca's result class.
    //
    // `byteDisp` folds a CONSTANT-offset Gep whose base is THIS alloca into the frame
    // reference (see lowerGep): `&local[constIdx]` / `&s.field` becomes ONE
    // frame-relative `lea [sp + slotOff + disp]` (base = sp) instead of a `lea_frame_
    // slot` (base) + a `lea [base + disp]` (Gep) pair. This is BOTH cheaper (one lea)
    // AND correctness-load-bearing on arm64: the Gep's `lea result,[base+disp]` with a
    // >16 MiB `disp` lowers to the arm64 MOVZ/MOVK 3-word macro `MOVZ Xd;MOVK Xd;ADD
    // Xd,base,Xd`, which CLOBBERS `base` when the regalloc coalesces the (tiny-range,
    // post-remat) `lea_frame_slot` base into the Gep result (base == Xd → MOVZ
    // destroys base before the ADD) — a SIGSEGV (large_frame_beyond_16mib). Folding to
    // `[sp + slotOff + disp]` makes the base `sp` (a reserved physreg, NEVER the
    // result), so the same MOVZ/MOVK macro is safe (sp != Xd). The disp rides an
    // ImmInt operand (preserved verbatim across the rewrite / legalize rebuilds).
    [[nodiscard]] std::optional<LirReg>
    emitLeaFrameSlot(std::uint32_t slotIndex, std::int64_t byteDisp = 0) {
        auto const op = opcode(MnemonicSlot::LeaFrameSlot);
        if (!op.has_value()) {
            // Unreachable: `lowerAlloca` only records into `allocaSlotIndex_` when
            // this opcode resolves. Defensive fail-loud against a future skew.
            reportMissingOpcode(MnemonicSlot::LeaFrameSlot,
                                "MIR Alloca address rematerialization");
            return std::nullopt;
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);   // a pointer
        if (byteDisp == 0) {
            emitInst(*op, result, std::span<LirOperand const>{},
                     /*payload=*/slotIndex);
        } else {
            // The ImmInt displacement operand — lir_callconv adds it to the slot
            // offset. A >2 GiB displacement is out of the int32 frame-offset domain;
            // caller (lowerGep) guards the range before folding, so we assert-fit here.
            std::array<LirOperand, 1> ops{LirOperand::makeImmInt32(
                static_cast<std::int32_t>(byteDisp))};
            emitInst(*op, result, ops, /*payload=*/slotIndex);
        }
        return result;
    }

    void lowerLoad(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) { reportUnsupported(MirOpcode::Load, id); return; }
        // D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD (FC3.5 sweep-c3): the
        // address comes from a single-use GlobalAddr whose lea was
        // elided (see `lowerGlobalAddr`) — emit the load's declared
        // symbol-relative form directly: ONE `movsd/movss xmm,
        // [rip+sym]` instead of the lea + [base] pair. The width gate
        // and class/width selection below are the SAME as the generic
        // path; only the operand shape differs ([SymbolRef] vs
        // [base, MemBase, MemOffset]).
        if (foldedGlobalAddrs_.contains(operands[0].v)) {
            if (!requireEncodedFloatWidth(id, mir.instType(id), "MIR Load"))
                return;
            LirRegClass const cls = regClassFor(id);
            auto const loadOp = classOp(cls, RegClassOp::Load);
            if (!loadOp.has_value()) {
                // Unreachable under the fold predicate (it probed this
                // class-op); fail loud like the generic path if a
                // future edit breaks the agreement.
                reportMissingClassOp(cls, RegClassOp::Load, "MIR Load");
                poisonValue(id);
                return;
            }
            // Byte-EXACT for a Char load (D-CSUBSET-CHAR-STRING-VALUE-
            // CODEGEN) exactly as the generic path below — a folded
            // [rip+sym] load of a 1-byte item must read 1 byte, not 8.
            // memAccessWidthFlags == the old FPR-width / GPR-0 ternary for
            // every non-Char type, so this is byte-identical pre-char.
            std::uint8_t const loadWidthFlags =
                memAccessWidthFlags(mir.instType(id), cls);
            SymbolId const sym = mir.globalAddrSymbol(operands[0]);
            LirReg const result = lir.newVReg(cls);
            std::array<LirOperand, 1> ops{LirOperand::makeSymbolRef(sym.v)};
            emitInst(*loadOp, result, ops, /*payload=*/0, loadWidthFlags);
            defineValue(id, result);
            return;
        }
        std::optional<LirReg> const base = regForValue(operands[0]);
        if (!base.has_value()) return;
        // Cycle 3d FPR-class plumbing closure: a Load of an F64 must
        // produce an FPR-class result, not GPR. Closes the silent-
        // failure-hunter HIGH finding from cycle 3d's review.
        // FC2 Part B / FC3.5 c2: an FPR-class load selects the
        // movsd_load/fldur mnemonic via registerClassOps and the WIDTH
        // axis picks the F64 (8-byte) vs F32 (4-byte movss / LDUR-S)
        // variant — the access width must be TYPE-EXACT for FPR loads
        // (an 8-byte read of a 4-byte F32 rodata item reads past the
        // item; potential fault at a section edge). Integer/GPR loads
        // keep the width-default full-slot read (their loaded values
        // are consumed low-bits-only). The gate walls off F16/F128.
        if (!requireEncodedFloatWidth(id, mir.instType(id), "MIR Load")) return;
        // FC2 Part B: the load mnemonic follows the RESULT's register
        // class (registerClassOps — GPR `load`, FPR e.g. movsd_load).
        LirRegClass const cls = regClassFor(id);
        auto const loadOp = classOp(cls, RegClassOp::Load);
        if (!loadOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Load, "MIR Load");
            poisonValue(id);
            return;
        }
        std::uint8_t const loadWidthFlags =
            memAccessWidthFlags(mir.instType(id), cls);
        LirReg const result = lir.newVReg(cls);
        std::array<LirOperand, 3> ops{
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        emitInst(*loadOp, result, ops, /*payload=*/0, loadWidthFlags);
        defineValue(id, result);
    }

    void lowerStore(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) { reportUnsupported(MirOpcode::Store, id); return; }
        std::optional<LirReg> const value = regForValue(operands[0]);
        std::optional<LirReg> const base  = regForValue(operands[1]);
        if (!value.has_value() || !base.has_value()) return;
        // FC2 Part B: the store mnemonic follows the stored VALUE's
        // register class (x86_64 fpr → movsd_store since the PE-float
        // closure, 2026-06-10). A class with no declared store fails
        // loud here instead of silently GPR-storing 8 bytes of XMM
        // ordinal. FC3.5 c2: FPR-class stores are width-exact like
        // loads — the stored value's TYPE width picks the F64/F32
        // variant (movsd vs movss / STUR-D vs STUR-S).
        LirRegClass const cls = value->regClass();
        auto const storeOp = classOp(cls, RegClassOp::Store);
        if (!storeOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Store, "MIR Store");
            return;
        }
        std::uint8_t const storeWidthFlags =
            memAccessWidthFlags(mir.instType(operands[0]), cls);
        std::array<LirOperand, 4> ops{
            LirOperand::makeReg(*value),
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        emitInst(*storeOp, InvalidLirReg, ops, /*payload=*/0,
                 storeWidthFlags);
    }

    void lowerGep(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.empty()) { reportUnsupported(MirOpcode::Gep, id); return; }
        // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): a Gep with a body-local
        // ALLOCA base and a CONSTANT displacement (`&local[constIdx]` / `&s.field`)
        // folds into ONE frame-relative `lea [sp + slotOff + disp]` (base = sp) — see
        // emitLeaFrameSlot's docblock for the WHY (cheaper + the arm64 >16 MiB
        // base==result SIGSEGV it avoids). Handled UP FRONT, before `regForValue`
        // materializes the base, so no dead base-`lea_frame_slot` is emitted. Only the
        // exact `[allocaBase, const]` 2-operand shape folds; every other shape (a
        // derived-pointer base, a runtime index, an out-of-int32 disp) takes the
        // general path below unchanged.
        if (opcode(MnemonicSlot::LeaFrameSlot).has_value()
            && operands.size() == 2) {
            if (auto slot = allocaSlotIndex_.find(operands[0].v);
                slot != allocaSlotIndex_.end()) {
                if (auto const disp = constIntegerValue(operands[1]);
                    disp.has_value()
                    && *disp >= std::numeric_limits<std::int32_t>::min()
                    && *disp <= std::numeric_limits<std::int32_t>::max()) {
                    if (auto folded = emitLeaFrameSlot(slot->second, *disp);
                        folded.has_value()) {
                        defineValue(id, *folded);
                    }
                    return;
                }
            }
        }
        std::optional<LirReg> const base = regForValue(operands[0]);
        if (!base.has_value()) return;
        // No-index degenerate: emit explicit `mov result, base` rather
        // than `lea result, [base + 0]` so the identity is visible at
        // LIR and reuses ML6's coalescer path.
        if (operands.size() == 1) {
            if (!opcode(MnemonicSlot::Mov).has_value()) {
                reportMissingOpcode(MnemonicSlot::Mov, "MIR Gep (no-index)");
                return;
            }
            emitMovToFresh(id, LirOperand::makeReg(*base), LirRegClass::GPR);
            return;
        }
        // Cycle 3d: dynamic-index Gep with a single index. Emits
        // `lea result, [base + index*1 + 0]` via the 4-operand `lea`
        // form (`[base_reg, index_reg, MemBase(scale=1), MemOffset(0)]`).
        // GEP-INDEX CONTRACT (D-MIR-STORAGE-ARRAY-INDEX-GEP, Option A): the
        // index operand is always a BYTE offset — a struct-field displacement
        // (const) or an array index already pre-scaled by sizeof(elem) at
        // HIR→MIR (scaleIndexToBytes). So scale=1 is correct by contract; no
        // element-size scale is carried here. Folding a `Mul-by-pow2 + Gep`
        // into a hardware-scaled `lea` is a future address-mode-synthesis
        // OPTIMIZATION (D-AS4-ARM64-INDEXED-LEA-SCALE), not a correctness need.
        // Multi-index Gep — multiple dereferences — folds into a chain at the
        // optimizer layer.
        if (operands.size() == 2) {
            if (!opcode(MnemonicSlot::Lea).has_value()) {
                reportMissingOpcode(MnemonicSlot::Lea, "MIR Gep (dynamic-index)");
                return;
            }
            // FC7 (D-FC7-MEMBER-ACCESS): a CONSTANT second operand is a
            // BYTE DISPLACEMENT — a struct field offset resolved at HIR→MIR
            // via the FC6 `computeLayout` engine (`s.x` → Gep[base,
            // Const(offset)]). Emit the 3-op base+disp `lea [base + disp]`
            // (the no-index form BOTH targets ship: x86 `lea r,[base+disp32]`
            // / arm64 `ADD Xd,Xn,#imm12`). A VREG second operand is a
            // runtime array index → the 4-op base+index form below. The
            // ≥3-op (multi-index) form stays unsupported (the array/struct
            // Index arm's 3-op shape is out of FC7 scope).
            if (auto const disp = constIntegerValue(operands[1])) {
                if (*disp < std::numeric_limits<std::int32_t>::min()
                    || *disp > std::numeric_limits<std::int32_t>::max()) {
                    // A field offset > 2GiB is pathological — fail loud
                    // rather than truncate to a wrong address.
                    reportUnsupported(MirOpcode::Gep, id);
                    return;
                }
                // (The body-local ALLOCA-base + const-disp fold is handled UP FRONT
                // at lowerGep's top — see there. This general path is for a
                // derived-pointer base.)
                //
                // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB (c94): a NEGATIVE
                // constant displacement (`p[-N]` → -N*sizeof(*p), e.g. a `char*`
                // lookback `z[-1]` or a ConstFold-folded `int*` `p[-5]`) rides the
                // `MemOffset` slot — SIGN AND ALL. Both targets now encode the
                // plain 3-op `lea [base + MemOffset(disp)]` NATIVELY, in ONE
                // instruction, with NO fold: x86 via its SIGNED disp32 field;
                // arm64 via the c94 `negMemoffset` SUB variants (`SUB Xd,Xn,#|disp|`
                // / shifted / MOVZ-MOVK), which the variant matcher routes to by
                // the memoffset's SIGN. This REPLACES c93's config-gated fold
                // (`targetHasSignedDispLea` → materialize the disp into a fresh
                // GPR + 4-op base+index `ADD Xd,Xn,Xm`), which cost 5-7 arm64
                // instructions including a DEAD const materialization (the MIR
                // Const(-N) lowered by the main dispatch then left unused). The
                // native SUB is one word (|disp|≤4095) — no fold, no dead const,
                // no base+index. Target-agnostic: the SINGLE 3-op path below
                // handles every disp on every target; the config picks the
                // encoding.
                //
                // (c93 STEP-0 named the sqlite trigger: 17 negative const-disp
                // Geps across 9 functions, ALL disp=-1 — the 1-byte-stride
                // `p[-1]`/`z[-1]` char-buffer lookback, which `scaleIndexToBytes`
                // leaves as a bare negative Const with NO ConstFold, so it fires
                // at DEBUG too. That is why the sqlite DEBUG compile hit this.)
                LirReg const result = lir.newVReg(LirRegClass::GPR);
                std::array<LirOperand, 3> ops{
                    LirOperand::makeReg(*base),
                    LirOperand::makeMemBase(1),
                    LirOperand::makeMemOffset(
                        static_cast<std::int32_t>(*disp)),
                };
                emitInst(*opcode(MnemonicSlot::Lea), result, ops);
                defineValue(id, result);
                return;
            }
            std::optional<LirReg> const index = regForValue(operands[1]);
            if (!index.has_value()) return;
            // c42 (D-CSUBSET-INDEX-NEGATIVE-WIDEN): the runtime index is a BYTE
            // offset the 4-op `lea` / arm64 ADD consumes as a FULL 64-bit address
            // register. A sub-64-bit index (I32/U32 — the usual `int`/`unsigned`
            // subscript) only defines the low 32 bits; the upper half holds
            // whatever the producer left. For a NEGATIVE offset (`p[-1]` →
            // -sizeof(*p), e.g. 0xFFFFFFF4) the hardware does NOT sign-extend a
            // 32-bit source into the 64-bit address — it reads as a huge positive
            // value → wrong address → SIGSEGV. A SILENT run-time miscompile:
            // `*(p-1)` (which lowers the pointer arithmetic, then a plain Load)
            // works, but `p[-1]` (this Gep-index path) crashes. Widen the index
            // to a full 64-bit register first — SIGN-extend a signed source (the
            // C subscript is sign-extended) / ZERO-extend an unsigned one. An
            // already-64-bit index (I64/U64/Ptr — e.g. c41's pre-widened `p ± n`
            // byte offset) needs no widen, and is left untouched so c41 is not
            // double-extended. A subscript expression is integer-PROMOTED (C
            // 6.3.1.1) before it reaches here, so in practice ONLY the I32 (SExt),
            // U32 (ZExt), and 64-bit (no-op) arms fire — an enum index promotes to
            // its underlying int (I32 → SExt), it does NOT reach `default`. The
            // narrow Char/I8/I16/U8/U16 arms are DEFENSIVE: they mirror the SExt /
            // ZExt source-width gates (the SExt arm takes Char/I8/I16/I32, the
            // ZExt arm U8/U16/U32) so the widen stays correct-by-construction —
            // and always one those gates accept — IF a future non-promoted narrow
            // index ever reaches here; `default` is a fail-safe for a non-integer
            // kind (which a Gep index should never be) → no widen, status quo. The
            // CONSTANT-displacement form above is unaffected: the hardware sign-extends
            // disp32 as part of the `lea [base + disp32]` encoding, so a constant
            // negative subscript was already correct and never reaches here. The
            // widen vreg is short-lived (consumed by the very next `lea`) and
            // coalesceable — unlike a MIR-tier widen it adds no
            // address-computation-spanning live range, so deep index chains keep
            // their register budget.
            LirReg lirIndex = *index;
            std::optional<MnemonicSlot> widenSlot;
            switch (interner.kind(mir.instType(operands[1]))) {
                case TypeKind::I64: case TypeKind::U64: case TypeKind::Ptr:
                    break;  // already a full 64-bit address register
                case TypeKind::U8: case TypeKind::U16: case TypeKind::U32:
                    widenSlot = MnemonicSlot::ZExt;  // unsigned → zero-extend
                    break;
                case TypeKind::Char: case TypeKind::I8:
                case TypeKind::I16:  case TypeKind::I32:
                    widenSlot = MnemonicSlot::SExt;  // signed → sign-extend
                    break;
                default:
                    break;  // non-integer/unexpected — a promoted index never reaches here
            }
            if (widenSlot.has_value()) {
                if (!opcode(*widenSlot).has_value()) {
                    reportMissingOpcode(*widenSlot, "MIR Gep (runtime index widen)");
                    return;
                }
                LirReg const index64 = lir.newVReg(LirRegClass::GPR);
                std::array<LirOperand, 1> widenOps{LirOperand::makeReg(*index)};
                emitInst(*opcode(*widenSlot), index64, widenOps, /*payload=*/0,
                         widthFlagsForType(mir.instType(operands[1])));
                lirIndex = index64;
            }
            LirReg const result = lir.newVReg(LirRegClass::GPR);
            std::array<LirOperand, 4> ops{
                LirOperand::makeReg(*base),
                LirOperand::makeReg(lirIndex),
                LirOperand::makeMemBase(1),
                LirOperand::makeMemOffset(0),
            };
            emitInst(*opcode(MnemonicSlot::Lea), result, ops);
            defineValue(id, result);
            return;
        }
        // Multi-index Gep (≥3 operands) — defer to cycle 3e (the
        // optimizer or a Gep-flattening pass is the natural consumer
        // since chains of dereferences need address-mode synthesis).
        reportUnsupported(MirOpcode::Gep, id);
    }

    // Single-operand value-producing cast. The result register class
    // follows the MIR result type (FPR for float-result casts; GPR
    // otherwise). Cycle 3d's float casts (FpCvt/FpToSi/FpToUi/SiToFp/
    // UiToFp) all land here. Delegates to the shared lowerNAryOp; the
    // `context` argument is now redundant with `mirOpcodeName` and
    // dropped (simplifier review). FC3.5: optional `widthOverride`
    // threads a non-result width key (ZExt's SOURCE width — see the
    // dispatch arm; D-CSUBSET-ZEXT-32-TO-64).
    void lowerCast(MirInstId id, MnemonicSlot slot, std::string_view /*context*/,
                   std::optional<std::uint8_t> widthOverride = std::nullopt) {
        lowerNAryOp<1>(id, slot, widthOverride);
    }

    // Bitcast lowering — closes the cycle-3c-anchored hazard 3 review
    // agents flagged. If src + dst share a register class (GPR↔GPR or
    // FPR↔FPR), emit a plain `mov` (bit-pattern-preserving). If they
    // differ, emit `movq_xclass` (cycle-3d cross-class move that AS1
    // maps to x86_64 `movq xmm,rax` or `movq rax,xmm`). A regression
    // omitting the cross-class case would silently mis-lower float
    // reinterpretation.
    void lowerBitcast(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) {
            reportUnsupported(MirOpcode::Bitcast, id);
            return;
        }
        std::optional<LirReg> const src = regForValue(operands[0]);
        if (!src.has_value()) return;
        LirRegClass const srcCls = src->regClass();
        LirRegClass const dstCls = regClassFor(id);
        // FC2 Part B: the same-class arm now routes through the
        // per-register-class move table (this function WAS the
        // class-dispatch pattern the table generalizes) — an FPR↔FPR
        // bitcast copies via the class's move (x86_64 movaps), never
        // the GPR mov. Cross-class keeps the dedicated movq_xclass.
        std::optional<std::uint16_t> copyOp;
        if (srcCls == dstCls) {
            copyOp = classOp(dstCls, RegClassOp::Move);
            if (!copyOp.has_value()) {
                reportMissingClassOp(dstCls, RegClassOp::Move,
                                     "MIR Bitcast (same-class)");
                poisonValue(id);
                return;
            }
        } else {
            if (!opcode(MnemonicSlot::MovqXClass).has_value()) {
                reportMissingOpcode(MnemonicSlot::MovqXClass,
                                    "MIR Bitcast (cross-class)");
                return;
            }
            copyOp = opcode(MnemonicSlot::MovqXClass);
        }
        LirReg const result = lir.newVReg(dstCls);
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*src)};
        emitInst(*copyOp, result, ops);
        defineValue(id, result);
    }

    // ── cycle 3e: Calls + GlobalAddr ─────────────────────────────────

    // MIR GlobalAddr → LIR `lea result, [rip + sym]` (D-LK4-RODATA-
    // PRODUCER 2026-06-02 — closes the prior `mov SymbolRef` gap).
    // The address materialization is a pure 7-byte RIP-relative
    // load-effective-address that emits a `rel32` Relocation
    // against the symbol. Cross-target shape: ARM64 will declare
    // its own LEA variant (ADRP+ADD pair, 2-instruction macro-op
    // — anchored under D-LK10-ENTRY-ARM64).
    //
    // ML7 cycle 2 peephole impact: when this GlobalAddr's result is
    // consumed ONLY by a direct `Call`, the Call lowerer folds the
    // SymbolId straight into the LIR call's SymbolRef operand and
    // never reads this GlobalAddr's emitted vreg. The LEA still
    // emits 7 bytes into the function body as a dead def; regalloc
    // handles the dead vreg, but the bytes remain in the binary.
    // Dead-LEA elimination for direct-call-only GlobalAddrs is
    // anchored at D-ML7-2.9 (carried forward from the prior `mov`
    // shape — same elimination opportunity, same trigger).
    //
    // Pre-D-LK4-RODATA-PRODUCER shape was `mov result, SymbolRef`,
    // which tripped `A_NoMatchingEncodingVariant` at assemble-time
    // because no `mov` variant accepted a SymbolRef operand. The
    // direct-call peephole sidesteps the LEA's vreg (the call's
    // SymbolRef operand is folded from the MIR-level GlobalAddr
    // SymbolId, not from the LEA's result) — but `lowerGlobalAddr`
    // still RUNS in source order, so the LEA's bytes still land in
    // the binary as a dead def. Non-call uses (returning a global's
    // address, loading from a global, etc.) were blocked by the
    // missing `mov` SymbolRef variant pre-D-LK4-RODATA-PRODUCER;
    // the LEA-RIP-rel variant unblocks them.
    // D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD (FC3.5 sweep-c3): true iff
    // `gaId`'s value has EXACTLY ONE MIR use, that use is a Load whose
    // sole (address) operand is this value, and the load's result-
    // class load mnemonic declares a single-SymbolRef-operand encoding
    // variant at the load's effective width. When true, the lea+load
    // pair folds to ONE symbol-relative load (x86: `movsd/movss
    // xmm, [rip+sym]` — the FC2 riprel variants' first pipeline
    // consumer). CAPABILITY-DRIVEN, never per-arch: a mnemonic without
    // a declared [symbol] variant (x86 GPR `load`; arm64 `fldur` —
    // arm64's lea is the 2-word ADRP+ADD macro, so a riprel-LDR fold
    // is a different encoding shape, deliberately out of scope) keeps
    // the generic pair byte-identically. Both fold sites
    // (`lowerGlobalAddr` skip-lea + `lowerLoad` symbol-form emit)
    // consult this ONE predicate via the `foldedGlobalAddrs_` set, so
    // they can never disagree; a stale set entry surfaces as a loud
    // undefined-vreg failure in `regForValue`, never a silent wrong
    // address.
    [[nodiscard]] bool globalAddrRiprelFoldsIntoLoad(MirInstId gaId) {
        auto const it = mirValueUses_.find(gaId.v);
        if (it == mirValueUses_.end() || it->second.count != 1) {
            return false;  // zero or multiple users — keep the lea
        }
        MirInstId const user = it->second.user;
        if (mir.instOpcode(user) != MirOpcode::Load) return false;
        auto const userOps = mir.instOperands(user);
        if (userOps.size() != 1 || userOps[0].v != gaId.v) return false;
        // Probe the load's class-op mnemonic for a [symbol] variant at
        // the load's width (the same class/width selection lowerLoad
        // makes — GPR loads are width-default 64; FPR loads carry the
        // loaded type's width so the F64/F32 riprel forms key apart).
        LirRegClass const cls = regClassFor(user);
        auto const loadOp = classOp(cls, RegClassOp::Load);
        if (!loadOp.has_value()) return false;  // lowerLoad fails loud
        auto const* info = target.opcodeInfo(*loadOp);
        if (info == nullptr) return false;
        // SAME width derivation the fold EMIT (`lowerLoad`) uses, so the
        // predicate and the emit can never disagree (a Char load probes —
        // and would fold to — the byte-width symbol variant). Byte-
        // identical to the old FPR-width / GPR-0 ternary for non-Char.
        std::uint8_t const loadWidthFlags =
            memAccessWidthFlags(mir.instType(user), cls);
        std::uint8_t const widthBits = lirInstWidthBits(loadWidthFlags);
        for (auto const& v : info->encoding.variants) {
            if (v.operandKinds.size() == 1
                && v.operandKinds[0] == OperandKindFilter::SymbolRef
                && (v.guardWidthBits == 0 || v.guardWidthBits == widthBits)) {
                return true;
            }
        }
        return false;
    }

    void lowerGlobalAddr(MirInstId id) {
        // D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD: when the sole consumer is
        // a load whose mnemonic declares the symbol-relative variant,
        // emit NO lea here — `lowerLoad` folds the symbol straight
        // into the load (lea+load pair → ONE riprel load). The
        // single-use guarantee means no other consumer can miss the
        // (never-defined) address vreg.
        if (globalAddrRiprelFoldsIntoLoad(id)) {
            foldedGlobalAddrs_.insert(id.v);
            return;
        }
        if (!opcode(MnemonicSlot::Lea).has_value()) {
            reportMissingOpcode(MnemonicSlot::Lea, "MIR GlobalAddr");
            return;
        }
        SymbolId const sym = mir.globalAddrSymbol(id);
        LirReg const result = lir.newVReg(regClassFor(id));
        std::array<LirOperand, 1> ops{LirOperand::makeSymbolRef(sym.v)};
        emitInst(*opcode(MnemonicSlot::Lea), result, ops);
        defineValue(id, result);
    }

    // D-CSUBSET-COMPUTED-GOTO: `&&label` → materialize the target block's runtime
    // address into a register, exactly like `lowerGlobalAddr` materializes a global
    // — a `lea` of a SYNTHETIC per-block symbol (rel32 on x86; adrp+add on arm64;
    // the linker assigns the symbol the interior-block VA). The `lea` carries a
    // SECOND operand — a `BlockRef` naming the target LIR block — so the assembler
    // can bind the synthetic symbol to that block's byte offset. The BlockRef rides
    // through every LIR rewrite pass (remapBlockRef keeps it current); the SymbolRef
    // rides unchanged. The two operands together are the block-symbol binding.
    void lowerBlockAddress(MirInstId id) {
        if (!opcode(MnemonicSlot::Lea).has_value()) {
            reportMissingOpcode(MnemonicSlot::Lea, "MIR BlockAddress");
            return;
        }
        MirBlockId const mirTarget = mir.blockAddressTarget(id);
        if (!mirBlockToLirBlock.has(mirTarget)) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "MIR BlockAddress target block %{} has no LIR block mapping",
                mirTarget.v);
            reporter.report(std::move(d));
            return;
        }
        LirBlockId const lirTarget = mirBlockToLirBlock.get(mirTarget);
        SymbolId const sym = mintBlockSymbol(mirTarget);
        LirReg const result = lir.newVReg(regClassFor(id));
        std::array<LirOperand, 2> ops{
            LirOperand::makeSymbolRef(sym.v),
            LirOperand::makeBlockRef(lirTarget.v)};
        emitInst(*opcode(MnemonicSlot::Lea), result, ops);
        defineValue(id, result);
    }

    // D-CSUBSET-COMPUTED-GOTO: `goto *p` → `jmp_indirect <reg>` (x86 jmp r/m64 FF/4
    // / arm64 BR Xn). operand[0] is the computed address; the address-taken
    // successor blocks ride as the LIR terminator's successor list (each remapped to
    // its LIR block), mirroring how `lowerSwitch`/`lowerBr` carry block successors.
    bool lowerIndirectBr(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opcode(MnemonicSlot::JmpIndirect).has_value()) {
            reportMissingOpcode(MnemonicSlot::JmpIndirect, "MIR IndirectBr");
            return false;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1 || succs.empty()) {
            reportUnsupported(MirOpcode::IndirectBr, id);
            return false;
        }
        std::optional<LirReg> const addr = regForValue(operands[0]);
        if (!addr.has_value()) return false;
        std::vector<LirBlockId> lirSuccs;
        lirSuccs.reserve(succs.size());
        for (MirBlockId const s : succs) {
            if (!mirBlockToLirBlock.has(s)) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR IndirectBr successor block %{} has no LIR block mapping",
                    s.v);
                reporter.report(std::move(d));
                return false;
            }
            lirSuccs.push_back(mirBlockToLirBlock.get(s));
        }
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*addr)};
        emitIndirectBr(*opcode(MnemonicSlot::JmpIndirect), ops, lirSuccs);
        return true;
    }

    // MIR Call → LIR `call callee, args...`. Convention:
    //   operand[0] = callee value (typically a GlobalAddr — direct
    //                call — peepholed into a SymbolRef LIR operand;
    //                anything else passes through as a Reg callee =
    //                the indirect-call path, materialized to
    //                `call <reg>` by lir_callconv — FC4 c2)
    //   operand[1..N] = argument values
    // ML7 cycle 2 (landed) rewrites this to the explicit
    // mov-to-arg-register + call + mov-from-return-register sequence
    // using the ML5 cycle-2b `TargetCallingConvention` sections
    // (argGprs/argFprs/returnGprs/returnFprs). The LIR shape coming
    // OUT of isel is the abstract `call(callee, args...)` form —
    // target-blind — and the callconv pass rewrites it to the
    // schema-encodable single-SymbolRef form.
    // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): the by-value-aggregate
    // stack-arg carrier. Its sole operand is the temp ADDRESS (a pointer value); its
    // result THREADS THAT ADDRESS through (a pure value-map alias, NO LIR emitted) so
    // a later `lowerCall` resolving this operand gets the address register. The
    // payload (aggregate byte size) is read by `lowerCall` directly off the MIR op to
    // build the `ByValueStackAgg` size marker — it never needs a register. The carrier
    // is only ever consumed by a Call (it sits in the Call's operand list at the
    // aggregate's argument position); aliasing the address keeps the value-map total
    // without an extra mov. (A stray ByValueStackArg whose result is consumed by
    // anything OTHER than a Call's by-value-stack arm would simply read the address,
    // which is harmless — but no such source path exists.)
    void lowerByValueStackArg(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) {
            reportUnsupported(MirOpcode::ByValueStackArg, id);
            return;
        }
        std::optional<LirReg> const addr = regForValue(operands[0]);
        if (!addr.has_value()) return;   // poisoned upstream; diagnostic already out
        defineValue(id, *addr);          // alias: result IS the address vreg
    }

    void lowerCall(MirInstId id) {
        // Cycle 3e fix-up: opcode resolution now goes through the
        // MnemonicCache (was ad-hoc `target.opcodeByMnemonic("call")`
        // before — flagged by code-reviewer as MED follow-up). Cached
        // resolution + one-shot diagnostic for missing mnemonic.
        if (!opcode(MnemonicSlot::Call).has_value()) {
            reportMissingOpcode(MnemonicSlot::Call, "MIR Call");
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.empty()) {
            reportUnsupported(MirOpcode::Call, id);
            return;
        }

        // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02):
        // pick the right call opcode based on whether the callee is
        // an extern import. Two distinct x86_64 encodings:
        //   * `call` (E8 disp32): direct call; the linker patches
        //     disp32 to the callee's RVA. Correct for module-internal
        //     functions whose bodies live in the same image.
        //   * `call_indirect_via_extern` (FF 15 disp32): indirect
        //     call via [RIP+disp32]; the linker patches disp32 to
        //     the IAT slot RVA. The CPU dereferences the slot to
        //     reach the loader-fixed-up callee address. Correct for
        //     extern imports (puts via msvcrt.dll, ExitProcess via
        //     kernel32.dll, etc.).
        // Using `call` for an extern target would execute the IAT
        // slot's bytes as code — guaranteed SEGV. Detected via the
        // `externSymbols` set populated at Lowerer construction from
        // the caller-supplied `externImports` vector.
        // Cycle 3e fix-up: HOIST all regForValue checks BEFORE any
        // state mutation. The cycle-3e first cut built the operand
        // vector inline; a mid-loop arg-not-mapped bail-out would
        // leave the partial operand list dangling AND skip
        // `defineValue`, producing cascading "used before definition"
        // diagnostics that drown the root cause. Same discipline as
        // `lowerStore`/`lowerInsertValue`.

        // ML7 cycle 2 peephole: if the callee operand is a `GlobalAddr`
        // MIR instruction, fold its SymbolId directly into the LIR
        // call's `SymbolRef` operand (the direct-call form). This
        // matches the target schema's `call` encoding variant guard
        // `["symbol"]` AND lets ML7 cycle 2's callconv materialization
        // pass leave the callee operand intact while just rewriting
        // the args. The indirect-call form (callee is any other MIR
        // instruction — a function-pointer load, an Arg, a call
        // result) keeps the callee's value REGISTER at ops[0]; the
        // callconv pass materializes it as `call <reg>` against the
        // schema's `["reg"]` encoding variant (x86 FF /2, arm64 BLR
        // — FC4 c2, D-CSUBSET-FNPTR-INDIRECT-CALL closed).
        MirInstId const calleeMir = operands[0];
        bool const calleeIsGlobalAddr =
            mir.instOpcode(calleeMir) == MirOpcode::GlobalAddr;

        // Determine extern-vs-internal based on the GlobalAddr's
        // SymbolId. An indirect callee (no GlobalAddr) is never an
        // extern-import call site — it dispatches through the value
        // in the register, not through an import slot.
        bool calleeIsExtern = false;
        SymbolId calleeSym{};
        if (calleeIsGlobalAddr) {
            calleeSym = mir.globalAddrSymbol(calleeMir);
            calleeIsExtern = externSymbols.contains(calleeSym.v);
        }

        // D-FFI-EXTERN-CALL-DISPATCH: an extern call's CALL-SITE shape is
        // the ACTIVE OBJECT FORMAT's, not a fixed assumption. Only an
        // `indirect-slot` format (PE IAT / Mach-O __got) dereferences a
        // pointer slot via `call_indirect_via_extern` (FF 15); a
        // `direct-plt` format (ELF) makes a PLAIN DIRECT `call` (E8 / BL)
        // to the linker-synthesized PLT stub — byte-identical to an
        // intra-module call, the linker's symbolVa→stub mapping + the
        // call reloc do the indirection. A non-extern call is always the
        // direct `Call`. (The ctor guard already fail-louded if externs
        // are present under a nullopt / under-equipped format, so an
        // extern reaching here has a valid, opcode-backed dispatch model;
        // a nullopt compares unequal to IndirectSlot → falls to `Call`.)
        bool const useIndirectExtern =
            calleeIsExtern
            && externCallDispatch_.has_value()
            && externCallUsesIndirectShape(*externCallDispatch_);
        MnemonicSlot const callSlot = useIndirectExtern
            ? MnemonicSlot::CallIndirectViaExtern
            : MnemonicSlot::Call;
        if (!opcode(callSlot).has_value()) {
            reportMissingOpcode(callSlot,
                useIndirectExtern
                    ? "MIR Call (extern-import target)"
                    : "MIR Call");
            return;
        }

        // Resolve every arg operand's register FIRST (the hoist discipline:
        // a mid-build bail-out must not leave a partial operand list dangling).
        // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): an operand whose
        // defining MIR op is `ByValueStackArg` is the by-value-aggregate stack-arg
        // carrier — its (already-resolved) register holds the temp ADDRESS and it
        // expands to TWO LIR operands (the address Reg + a `ByValueStackAgg` size
        // marker). For a normal operand it's a single Reg. We resolve all the regs
        // up front, then build `ops` (so the partial-list hazard can't happen).
        std::size_t const firstArgIdx = calleeIsGlobalAddr ? 1u : 0u;
        std::vector<LirReg> argRegs;
        argRegs.reserve(operands.size());
        for (std::size_t i = firstArgIdx; i < operands.size(); ++i) {
            std::optional<LirReg> const r = regForValue(operands[i]);
            if (!r.has_value()) return;
            argRegs.push_back(*r);
        }

        // Build the LIR operand list.
        // Direct: [SymbolRef(sym), arg0, arg1, ...]
        // Indirect-via-extern: [SymbolRef(sym), arg0, arg1, ...] —
        //     same shape; the SymbolRef tags an extern (resolved to
        //     IAT slot RVA at link time; the opcode encoding
        //     dereferences it).
        // Indirect (fn-pointer): [callee_reg, arg0, arg1, ...]
        //     (FC4 c2 — materialized as `call <reg>` by lir_callconv
        //     against the schema's `["reg"]` encoding variant).
        // A by-value-stack aggregate arg expands to [..., addrReg, ByValueStackAgg].
        std::vector<LirOperand> ops;
        ops.reserve(argRegs.size() + 2);
        if (calleeIsGlobalAddr) {
            ops.push_back(LirOperand::makeSymbolRef(calleeSym.v));
        }
        for (std::size_t k = 0; k < argRegs.size(); ++k) {
            MirInstId const operandMir = operands[firstArgIdx + k];
            ops.push_back(LirOperand::makeReg(argRegs[k]));
            if (mir.instOpcode(operandMir) == MirOpcode::ByValueStackArg) {
                // The preceding Reg is the aggregate's temp address; this marker
                // carries the byte size + the exhaust class (the MIR op's payload,
                // per the shared kByValueStackArg* encoding) so lir_callconv reserves
                // ceil(size / outgoingSlot) overflow slots + byte-copies the temp into
                // the outgoing area instead of register-passing it, AND clamps the
                // exhausted arg-cursor (AAPCS64) so a later same-class arg also stacks.
                std::uint32_t const bvPayload = mir.instPayload(operandMir);
                ops.push_back(LirOperand::makeByValueStackAgg(
                    bvPayload & kByValueStackArgSizeMask,
                    static_cast<std::uint8_t>(
                        (bvPayload >> kByValueStackArgExhaustShift) & 0x3u)));
            }
        }

        // Result class follows MIR's result type. Void-returning calls
        // produce an invalid result vreg (no value).
        TypeId const resultTy = mir.instType(id);
        bool const isVoid = !resultTy.valid()
                         || interner.kind(resultTy) == TypeKind::Void;
        // D-LANG-VARIADIC (step 13.4): forward the MIR Call's
        // variadic-payload bits (isVariadic + fixedOperandCount) to the
        // LIR Call so the post-regalloc ML7 materialize pass can
        // emit the platform's variadic-call setup (SysV `mov al,
        // <xmm-arg-count>` etc.). Non-variadic calls keep payload=0.
        std::uint32_t const payload = mir.instPayload(id);
        if (isVoid) {
            emitInst(*opcode(callSlot), InvalidLirReg, ops, payload);
            return;
        }
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*opcode(callSlot), result, ops, payload);
        defineValue(id, result);
    }

    // MIR IntrinsicCall → LIR `intrinsic_call`. Operands are the args;
    // payload carries the intrinsic id. Result is optional (per the
    // schema's `result: "optional"` for the opcode). AS1 maps the
    // intrinsic id to its concrete sequence per target.
    //
    // Cycle 3e fix-up: hoist all regForValue checks BEFORE state
    // mutation (mirrors lowerCall fix). A mid-loop arg-not-mapped
    // bail-out would otherwise leave a partial operand list dangling.
    void lowerIntrinsicCall(MirInstId id) {
        if (!opcode(MnemonicSlot::IntrinsicCall).has_value()) {
            reportMissingOpcode(MnemonicSlot::IntrinsicCall, "MIR IntrinsicCall");
            return;
        }
        auto const operands = mir.instOperands(id);
        std::vector<LirReg> argRegs;
        argRegs.reserve(operands.size());
        for (auto const opnd : operands) {
            std::optional<LirReg> const r = regForValue(opnd);
            if (!r.has_value()) return;
            argRegs.push_back(*r);
        }
        std::vector<LirOperand> ops;
        ops.reserve(argRegs.size());
        for (auto const r : argRegs) ops.push_back(LirOperand::makeReg(r));

        std::uint32_t const intrId = mir.intrinsicId(id);
        TypeId const resultTy = mir.instType(id);
        bool const isVoid = !resultTy.valid()
                         || interner.kind(resultTy) == TypeKind::Void;
        if (isVoid) {
            emitInst(*opcode(MnemonicSlot::IntrinsicCall), InvalidLirReg, ops, intrId);
            return;
        }
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*opcode(MnemonicSlot::IntrinsicCall), result, ops, intrId);
        defineValue(id, result);
    }

    // Cycle 3e fix-up: shared helper for aggregate-op index validation.
    // The cycle-3e first cut only accepted `int64_t` zero — the
    // `uint64_t` and `bool` variants of `MirLiteralValue` silently fell
    // through to `reportUnsupported` even when the value WAS zero.
    // Type-design + silent-failure-hunter flagged this as a real latent
    // bug since MIR text round-trip accepts both `lit int N` and
    // `lit uint N` variants. Now accepts int64==0 OR uint64==0 OR bool
    // false (the three literal variants `lowerConst` produces).
    [[nodiscard]] bool isZeroIntegerLiteral(MirInstId idxOp) const {
        if (mir.instOpcode(idxOp) != MirOpcode::Const) return false;
        std::uint32_t const litIdx = mir.constLiteralIndex(idxOp);
        MirLiteralValue const& lit = mir.literalValue(litIdx);
        if (auto const* i = std::get_if<std::int64_t>(&lit.value))  return *i == 0;
        if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) return *u == 0;
        if (auto const* b = std::get_if<bool>(&lit.value))          return !*b;
        return false;
    }

    // FC7 (D-FC7-MEMBER-ACCESS): the integer value of a Const operand, or
    // nullopt if the operand is not an integer Const. Distinguishes a
    // CONSTANT Gep index (a struct field byte-offset resolved at HIR→MIR)
    // from a runtime VREG index (an array subscript) — the byte-offset
    // form lowers to the base+disp `lea`, the vreg form to base+index.
    [[nodiscard]] std::optional<std::int64_t>
    constIntegerValue(MirInstId idxOp) const {
        if (mir.instOpcode(idxOp) != MirOpcode::Const) return std::nullopt;
        std::uint32_t const litIdx = mir.constLiteralIndex(idxOp);
        MirLiteralValue const& lit = mir.literalValue(litIdx);
        if (auto const* i = std::get_if<std::int64_t>(&lit.value))  return *i;
        if (auto const* u = std::get_if<std::uint64_t>(&lit.value))
            return static_cast<std::int64_t>(*u);
        if (auto const* b = std::get_if<bool>(&lit.value))          return *b ? 1 : 0;
        return std::nullopt;
    }

    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB (c94): would this Const's value
    // be folded into a `MemOffset` IMMEDIATE by a const-disp Gep, making its
    // register materialization DEAD? True iff (a) the Const is an INTEGER Const
    // whose value fits int32 (the fold path's range gate — both the frame-slot
    // fold and the general derived-pointer const-disp fold require it), AND
    // (b) it is SINGLE-USE, AND (c) its sole user is a 2-operand Gep in which
    // the Const is `operands[1]` (the displacement — operand 0 is the base
    // pointer, always a register-producing value, never this Const). Under
    // those conditions `lowerGep` reads the value via `constIntegerValue` and
    // emits `MemOffset(value)` — the register is never `regForValue`'d, so
    // `lowerConst` may skip materializing it. Sign-AGNOSTIC (a positive struct-
    // field offset AND a negative `p[-N]` disp both qualify). A Const with any
    // OTHER use, a non-Gep user, a runtime-index Gep (Const at operand 0 is
    // impossible; a 3+-operand Gep isn't the const-disp form), or an out-of-
    // int32 value is NOT folded → materialized normally. `computeValueUses`
    // must have run (it does — `lowerFunction` calls it before block lowering).
    [[nodiscard]] bool constDispFoldsIntoGep(MirInstId constId) const {
        if (mir.instOpcode(constId) != MirOpcode::Const) return false;
        // The value must fit int32 — the exact gate both fold paths apply
        // before emitting a MemOffset (a >2GiB disp fails loud / takes the
        // general path un-folded).
        auto const v = constIntegerValue(constId);
        if (!v.has_value()
            || *v < std::numeric_limits<std::int32_t>::min()
            || *v > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        auto const it = mirValueUses_.find(constId.v);
        if (it == mirValueUses_.end() || it->second.count != 1) return false;
        MirInstId const user = it->second.user;
        if (mir.instOpcode(user) != MirOpcode::Gep) return false;
        auto const userOps = mir.instOperands(user);
        // Only the 2-operand [base, const-disp] shape folds the disp into an
        // immediate; a ≥3-operand (multi-index) Gep does not, and operand 0 is
        // always the base pointer (never this Const).
        return userOps.size() == 2 && userOps[1].v == constId.v;
    }

    // ── cycle 3e: aggregate ops (memory-flattening lowering) ─────────
    //
    // MIR ExtractValue/InsertValue operate on aggregate values directly
    // (D5.6 substrate). The cycle-3e LIR lowering routes them through
    // memory: each aggregate value is materialized via an Alloca, and
    // ExtractValue/InsertValue lower as Load/Store at the appropriate
    // byte offset. This is correct + simple but adds extra memory
    // traffic; ML6's optimizer (plan 11) will promote-to-registers
    // where the alloca doesn't escape.
    //
    // Cycle 3e ships the DEGENERATE shape: zero offset (the first
    // field). Real field-offset computation needs the MIR type's
    // layout from the interner, which is co-designed with ML6's
    // frame-layout phase. Until that lands, ExtractValue/InsertValue
    // with a non-zero index defers to ML6 frame-layout.

    void lowerExtractValue(MirInstId id) {
        // MIR operands: [aggregate, index_const_1, index_const_2, ...].
        // The aggregate's value flows from a ConstructAggregate (ML2
        // cycle 6) which is built via alloca + insertvalue chain in
        // MIR. For cycle 3e: only the zero-index path lowers; non-zero
        // indices defer (would need type-layout lookup).
        auto const operands = mir.instOperands(id);
        if (operands.size() < 2) {
            reportUnsupported(MirOpcode::ExtractValue, id);
            return;
        }
        // Every index must be a zero-valued Const-literal for cycle
        // 3e's degenerate path. Non-zero indices defer to ML6 frame-
        // layout co-design (need type-layout-driven field offsets).
        for (std::size_t k = 1; k < operands.size(); ++k) {
            if (!isZeroIntegerLiteral(operands[k])) {
                reportUnsupported(MirOpcode::ExtractValue, id);
                return;
            }
        }
        // Degenerate first-field path: emit `load` from base. FC2
        // Part B: class-routed like lowerLoad (an F64 first field
        // would otherwise GPR-load into an FPR vreg).
        std::optional<LirReg> const base = regForValue(operands[0]);
        if (!base.has_value()) return;
        if (!requireEncodedFloatWidth(id, mir.instType(id),
                                      "MIR ExtractValue")) return;
        LirRegClass const cls = regClassFor(id);
        auto const loadOp = classOp(cls, RegClassOp::Load);
        if (!loadOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Load, "MIR ExtractValue");
            poisonValue(id);
            return;
        }
        // FC3.5 c2: FPR width-exact like lowerLoad.
        // Byte-EXACT for a Char first-field (D-CSUBSET-CHAR-STRING-VALUE-
        // CODEGEN) exactly as lowerLoad — extracting a 1-byte field must
        // read 1 byte. memAccessWidthFlags == the old FPR-width / GPR-0
        // ternary for every non-Char type (byte-identical pre-char).
        std::uint8_t const extWidthFlags =
            memAccessWidthFlags(mir.instType(id), cls);
        LirReg const result = lir.newVReg(cls);
        std::array<LirOperand, 3> ops{
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        emitInst(*loadOp, result, ops, /*payload=*/0, extWidthFlags);
        defineValue(id, result);
    }

    void lowerInsertValue(MirInstId id) {
        // MIR operands: [aggregate, value, index_const_1, ...].
        // Cycle 3e: degenerate zero-index → emit `store value, base`
        // and return the (modified) aggregate base. Multi-byte offsets
        // defer to ML6 frame-layout.
        auto const operands = mir.instOperands(id);
        if (operands.size() < 3) {
            reportUnsupported(MirOpcode::InsertValue, id);
            return;
        }
        for (std::size_t k = 2; k < operands.size(); ++k) {
            if (!isZeroIntegerLiteral(operands[k])) {
                reportUnsupported(MirOpcode::InsertValue, id);
                return;
            }
        }
        std::optional<LirReg> const base  = regForValue(operands[0]);
        std::optional<LirReg> const value = regForValue(operands[1]);
        if (!base.has_value() || !value.has_value()) return;
        // FC2 Part B: class-routed like lowerStore — the stored
        // VALUE's class picks the mnemonic; a class without a
        // declared store fails loud. FC3.5 c2: FPR width-exact like
        // lowerStore (the stored value's type width).
        LirRegClass const cls = value->regClass();
        auto const storeOp = classOp(cls, RegClassOp::Store);
        if (!storeOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Store, "MIR InsertValue");
            return;
        }
        // Byte-EXACT for a Char first-field store (D-CSUBSET-CHAR-STRING-
        // VALUE-CODEGEN) exactly as lowerStore — inserting a 1-byte field
        // must write 1 byte, not clobber 7 neighbours. memAccessWidthFlags
        // == the old FPR-width / GPR-0 ternary for non-Char (byte-identical
        // pre-char).
        std::uint8_t const insWidthFlags =
            memAccessWidthFlags(mir.instType(operands[1]), cls);
        std::array<LirOperand, 4> ops{
            LirOperand::makeReg(*value),
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        emitInst(*storeOp, InvalidLirReg, ops, /*payload=*/0,
                 insWidthFlags);
        // The result of InsertValue is conceptually the modified
        // aggregate. In the memory-flattening model the aggregate IS
        // the base pointer; reuse it as the result. ML6's promote-to-
        // registers will turn this into a proper SSA update.
        defineValue(id, *base);
    }

    // Shared N-operand value-producing lowering. Consolidates the
    // identical scaffolding of `lowerCast`/`lowerBinaryOp`/`lowerUnaryOp`
    // (opcode-resolve → operand-arity-check → regForValue each operand
    // → newVReg(regClassFor(id)) → addInst → defineValue). Per the
    // cycle-3d simplifier review.
    template <std::size_t Arity>
    void lowerNAryOp(MirInstId id, MnemonicSlot slot,
                     std::optional<std::uint8_t> widthOverride = std::nullopt) {
        auto const op = opcode(slot);
        if (!op.has_value()) {
            reportMissingOpcode(slot, mirOpcodeName(mir.instOpcode(id)));
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != Arity) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::array<LirOperand, Arity> ops;
        for (std::size_t k = 0; k < Arity; ++k) {
            std::optional<LirReg> const r = regForValue(operands[k]);
            if (!r.has_value()) return;
            ops[k] = LirOperand::makeReg(*r);
        }
        // Result reg class follows the MIR result type — FPR for float
        // ops, VR for vector ops, GPR otherwise.
        LirReg const result = lir.newVReg(regClassFor(id));
        // FC3 c2: the RESULT type's width selects the encoded form
        // (I32/U32 → the 32-bit variants; D-CSUBSET-32BIT-ALU-FORMS).
        // Width-invariant ops (floats, conversions with inherent
        // widths, Bool) map to the 64-bit default and keep matching
        // their width-absent variants byte-identically.
        // FC3.5 sweep-c1: `widthOverride` serves the one conversion
        // whose encoded form keys on a NON-result width — ZExt's
        // source (D-CSUBSET-ZEXT-32-TO-64). Every other caller leaves
        // it unset and keeps the result-type rule byte-identically.
        emitInst(*op, result, ops, /*payload=*/0,
                 widthOverride.has_value()
                     ? *widthOverride
                     : widthFlagsForType(mir.instType(id)));
        defineValue(id, result);
    }

    void lowerBinaryOp(MirInstId id, MnemonicSlot slot) { lowerNAryOp<2>(id, slot); }
    void lowerUnaryOp (MirInstId id, MnemonicSlot slot) { lowerNAryOp<1>(id, slot); }

    // ── c78 (D-CSUBSET-FLOAT-NEG-ENCODING): capability-driven MIR FNeg ──
    //
    // `-someFloat`/`-someDouble`. There is NO single agnostic instruction:
    //   * arm64 has a NATIVE FNEG (Dd,Dn / Sd,Sn) — 1 op, flips the sign bit.
    //   * x86 has NO fp-negate instruction; the value-optimal realization is
    //     gcc's `xorpd/xorps xmm, [rip+.LC0]` against a 16-byte rodata SIGN-
    //     MASK (bit 63 set for F64 / bit 31 for F32). XOR is chosen for IEEE
    //     exactness — it flips ONLY the sign bit, so -(+0.0)=-0.0,
    //     -(-0.0)=+0.0, and -NaN preserves the payload; `0.0 - x` gives +0.0
    //     for x=+0.0 (wrong signed zero) and mishandles NaN sign.
    //
    // The realization is selected by CAPABILITY (the sanctioned pattern the
    // div/mod + shift lowerings use — probe the DECLARED opcode vocabulary,
    // NEVER `if (arch == ...)`):
    //   Rule 1 — NATIVE: the target's `fneg` opcode declares an encoding
    //     (arm64) → the generic 1-op unary lowering (MnemonicSlot::FNeg).
    //   Rule 2 — SIGN-MASK XOR: no native `fneg` encoding but a `fneg_mask`
    //     opcode is declared (x86) → mint a 16-byte, 16-byte-aligned rodata
    //     sign-mask (accumulated as a SignMaskConstant the pipeline emits)
    //     and emit `fneg_mask xmm, [rip+mask]` with operands [value, mask].
    //     The op is 2-address (requires2Address): the legalize inserts
    //     `movaps dst, value` so the XORPD destination register holds the
    //     value; the mask rides op[1] as a SymbolRef → the riprel.disp32
    //     encoder slot (rel32 reloc).
    //   Else → fail loud (the target declares neither realization).
    //
    // The width axis (F64 vs F32) selects BOTH the encoded variant (XORPD vs
    // XORPS / D-form vs S-form FNEG) AND the mask pattern (bit 63 vs bit 31).
    // F16/F128 fail loud at the width gate (no scalar float encodings) — a
    // wrong-width mask would be a silent miscompile.
    void lowerFNeg(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        // Width gate: only F64/F32 have scalar float encodings AND a
        // well-defined sign-mask pattern. F16/F128 → loud (poisons id).
        if (!requireEncodedFloatWidth(id, mir.instType(id), "MIR FNeg")) {
            return;
        }

        // Rule 1 — NATIVE fp-negate (arm64 FNEG): the `fneg` opcode declares
        // an encoding. Probe DECLARED-ENCODING PRESENCE, not mere mnemonic
        // presence (x86 declares `fneg` too, but with NO variants — the
        // abstract-op probe target).
        if (auto const nativeOp = opcode(MnemonicSlot::FNeg);
            nativeOp.has_value()) {
            auto const* ni = target.opcodeInfo(*nativeOp);
            if (ni != nullptr && !ni->encoding.variants.empty()) {
                return lowerUnaryOp(id, MnemonicSlot::FNeg);
            }
        }

        // Rule 2 — SIGN-MASK XOR (x86): the target declares `fneg_mask`.
        auto const maskOp = opcode(MnemonicSlot::FNegMask);
        if (!maskOp.has_value()) {
            // Neither a native fneg encoding NOR a fneg_mask op — the target
            // declares no float-negate realization. Fail loud (never a silent
            // wrong value). reportMissingOpcode names the slot + context.
            reportMissingOpcode(MnemonicSlot::FNegMask, "MIR FNeg");
            return;
        }

        std::optional<LirReg> const value = regForValue(operands[0]);
        if (!value.has_value()) return;

        TypeKind const k = interner.kind(mir.instType(id));
        bool const isF64 = (k == TypeKind::F64);   // F32 already width-gated

        // Mint the sign-mask rodata symbol + record its descriptor (the
        // pipeline materializes the 16-byte, 16-byte-aligned `.rodata` item).
        SymbolId const maskSym = mintSignMaskSymbol();
        signMaskConstants_.push_back(SignMaskConstant{maskSym, isF64});

        // Emit `fneg_mask xmm, [rip+mask]`. op[0] = the value (the 2-address
        // legalize copies it into the result register = XORPD destination);
        // op[1] = the mask symbol (→ riprel.disp32). Width selects XORPD/XORPS.
        LirReg const result = lir.newVReg(regClassFor(id));
        std::array<LirOperand, 2> const ops{
            LirOperand::makeReg(*value),
            LirOperand::makeSymbolRef(maskSym.v)};
        emitInst(*maskOp, result, ops, /*payload=*/0,
                 widthFlagsForType(mir.instType(id)));
        defineValue(id, result);
    }

    // ── FC3.5 sweep-c1: capability-driven MIR Shl/LShr/AShr lowering ──
    // (closes the D-CSUBSET-32BIT-ALU-FORMS shifts residue)
    //
    // ONE arm serves all three shift MIR opcodes on every target,
    // selecting the realization from the opcode's DECLARED capability
    // (the FC1 div/mod probing pattern — never an arch identity):
    //
    //   Rule 1 — IMMEDIATE COUNT: the count operand is a MIR integer
    //     Const whose value fits the imm8 byte AND the opcode declares
    //     a [reg, imm] encoding variant → emit the 2-operand
    //     (value, imm) form (x86 `SHL/SHR/SAR r/m, imm8` = C1 /4 /5
    //     /7 ib). A target without an imm variant (arm64 — the
    //     LSL-imm UBFM aliases are a deliberate peephole deferral)
    //     skips this rule; the materialized const vreg feeds rule 2/3
    //     like any operand.
    //
    //   Rule 2 — IMPLICIT-COUNT REGISTER: the opcode declares
    //     `implicitRegisters` → the count lives in a fixed register
    //     the core op reads implicitly (x86: CL — there is no
    //     3-address reg-count shift). The register is resolved BY
    //     ROLE ("count") from `inputRoles` — the FC1 idiv "dividend"
    //     contract; a declaration missing the role is a
    //     misconfiguration → fail loud, never positional. Sequence:
    //       mov  <count-role>_phys, count_vreg   ; pin the count
    //       SHIFT result_vreg, value_vreg        ; requires2Address
    //                                            ; legalize makes
    //                                            ; result == value
    //     Regalloc keeps live ranges out of the count register across
    //     the shift via the same covered-position implicit-clobber
    //     exclusion the div pair uses (the opcode declares the count
    //     register clobbered: the REALIZATION's pin-mov destroys its
    //     prior value).
    //
    //   Rule 3 — NATIVE 3-ADDRESS: no implicitRegisters → the target
    //     shifts by a plain register operand (arm64 LSLV/LSRV/ASRV
    //     Xd, Xn, Xm; a future RISC-V SLL/SRL/SRA likewise) → the
    //     generic 2-operand lowering.
    //
    // C semantics note (C23 6.5.7p3): shift counts >= the promoted
    // left operand's width (or negative) are UNDEFINED — no masking
    // is emitted; both ISAs mask in hardware (x86 count & 63/& 31
    // incl. the imm8 form; arm64 LSLV/LSRV/ASRV use Xm mod 64 /
    // Wm mod 32), so a UB count yields the hardware's masked result
    // rather than a trap. The width axis keys on the RESULT type
    // (= C's promoted LEFT operand, the shift's UAC rule) — the
    // COUNT's own width is irrelevant to the encoding (CL reads one
    // byte; the V-forms read the low bits of Xm/Wm).
    bool isShiftCountImm8(MirInstId countId,
                          std::optional<std::int32_t>& out) const {
        if (mir.instOpcode(countId) != MirOpcode::Const) return false;
        MirLiteralValue const& lit =
            mir.literalValue(mir.constLiteralIndex(countId));
        std::int64_t v = 0;
        if (auto const* i = std::get_if<std::int64_t>(&lit.value)) {
            v = *i;
        } else if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) {
            if (*u > 255u) return false;
            v = static_cast<std::int64_t>(*u);
        } else {
            return false;
        }
        if (v < 0 || v > 255) return false;
        out = static_cast<std::int32_t>(v);
        return true;
    }

    // Does the opcode declare ANY [Reg, ImmInt]-guarded encoding
    // variant? Capability probe for the immediate-count form —
    // config-driven (reads the schema's declared variants), no
    // arch identity.
    [[nodiscard]] static bool
    declaresRegImmVariant(TargetOpcodeInfo const& info) noexcept {
        for (auto const& v : info.encoding.variants) {
            if (v.operandKinds.size() == 2
                && v.operandKinds[0] == OperandKindFilter::Reg
                && v.operandKinds[1] == OperandKindFilter::ImmInt) {
                return true;
            }
        }
        return false;
    }

    void lowerShift(MirInstId id, MnemonicSlot slot) {
        auto const op = opcode(slot);
        if (!op.has_value()) {
            reportMissingOpcode(slot, mirOpcodeName(mir.instOpcode(id)));
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        auto const* info = target.opcodeInfo(*op);
        if (info == nullptr) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::optional<LirReg> const value = regForValue(operands[0]);
        if (!value.has_value()) return;
        // The width axis keys on the RESULT type (C's promoted left
        // operand — shifts do NOT unify with the count's type).
        std::uint8_t const widthFlags = widthFlagsForType(mir.instType(id));

        // Rule 1 — immediate count.
        std::optional<std::int32_t> imm;
        if (isShiftCountImm8(operands[1], imm)
            && declaresRegImmVariant(*info)) {
            LirReg const result = lir.newVReg(regClassFor(id));
            std::array<LirOperand, 2> const ops{
                LirOperand::makeReg(*value),
                LirOperand::makeImmInt32(*imm)};
            emitInst(*op, result, ops, /*payload=*/0, widthFlags);
            defineValue(id, result);
            return;
        }

        std::optional<LirReg> const count = regForValue(operands[1]);
        if (!count.has_value()) return;

        // Rule 2 — implicit-count register (resolved BY ROLE).
        if (info->implicitRegisters.has_value()) {
            auto const& ir = *info->implicitRegisters;
            auto const countOrdinal = ir.inputOrdinalForRole("count");
            if (!countOrdinal.has_value()) {
                reportMissingImplicitRole(*op, "inputRoles", "count", id);
                return;
            }
            auto const movOp = opcode(MnemonicSlot::Mov);
            if (!movOp.has_value()) {
                reportMissingOpcode(MnemonicSlot::Mov,
                                    "MIR shift (count pin)");
                return;
            }
            // Register class from the schema's register table — the
            // F1 div-lowering rule (never hardcoded GPR).
            auto const* countRegInfo = target.registerInfo(*countOrdinal);
            if (countRegInfo == nullptr) {
                reportUnsupported(mir.instOpcode(id), id);
                return;
            }
            LirReg const countPhys = makePhysicalReg(
                *countOrdinal,
                static_cast<LirRegClass>(countRegInfo->regClass));
            // 1. Pin the count into the role-declared register
            //    (64-bit full copy — the core reads only the low
            //    byte/bits it needs; mirrors the div dividend pin).
            std::array<LirOperand, 1> const pinOps{
                LirOperand::makeReg(*count)};
            emitInst(*movOp, countPhys, pinOps);
            // 2. The core shift: ONE explicit operand (the value;
            //    requires2Address legalize copies it into the result
            //    so the r/m operand IS the destination). Regalloc
            //    reads the implicit-register declaration and protects
            //    the count register with TWO rules: covering OPERAND
            //    ranges are excluded (covered-position rule), and the
            //    RESULT vreg is excluded too (result-def rule —
            //    required because the legalize's `mov result, value`
            //    lands BEFORE this op; result==count would clobber
            //    the pin under pressure).
            LirReg const result = lir.newVReg(regClassFor(id));
            std::array<LirOperand, 1> const shiftOps{
                LirOperand::makeReg(*value)};
            emitInst(*op, result, shiftOps, /*payload=*/0, widthFlags);
            defineValue(id, result);
            return;
        }

        // Rule 3 — native 3-address (count as a plain reg operand).
        LirReg const result = lir.newVReg(regClassFor(id));
        std::array<LirOperand, 2> const ops{
            LirOperand::makeReg(*value), LirOperand::makeReg(*count)};
        emitInst(*op, result, ops, /*payload=*/0, widthFlags);
        defineValue(id, result);
    }

    // ── FC1 (V2-4.X): capability-driven MIR SDiv/UDiv/SMod/UMod lowering ──
    // (cycle 10r D-CSUBSET-DIVISION-OP-CODEGEN substrate, generalized
    //  2026-06-10 to close D-CSUBSET-MOD-OP-CODEGEN +
    //  D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT +
    //  D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC)
    //
    // ONE arm serves all four division-family MIR opcodes on every
    // target, selecting the realization from the target's DECLARED
    // OPCODE VOCABULARY (mnemonic-presence probing — the sanctioned
    // pattern `lea`/`call` already use; never an arch identity):
    //
    //   Rule 1 — NATIVE: the target declares a `result: value` opcode
    //     under the verb's own mnemonic ("sdiv"/"udiv"/"smod"/"umod")
    //     → ONE 3-address LIR op. (arm64 SDIV/UDIV; a future RISC-V
    //     DIV/DIVU/REM/REMU hits all four.) A `result: none`
    //     declaration under these mnemonics is a schema
    //     misdeclaration → fail loud, never a silent fall-through to
    //     another realization the author did not intend.
    //
    //   Rule 2 — IMPLICIT-REGISTER PAIR: the target declares the
    //     pre/core pair (x86: cqo+idiv_op signed; xor_rdx_zero+div_op
    //     unsigned) → the 4-LIR-op sequence in `emitImplicitPairDiv`;
    //     the SSA result is captured from the implicit output
    //     selected BY ROLE ("quotient" for div, "remainder" for mod)
    //     via the core op's `outputRoles` declaration — never by
    //     positional index (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-
    //     CONTRACT: a JSON reorder of `outputs` can no longer
    //     silently flip a quotient capture into a remainder capture).
    //     The dividend pin likewise resolves via `inputRoles` role
    //     "dividend". A HALF-declared pair (pre without core or vice
    //     versa) is a misconfiguration → fail loud naming the absent
    //     half — it does NOT silently fall through to rule 3.
    //
    //   Rule 3 — EXPANSION (remainder only): no native rem and no
    //     pair, but a div realization (rule 1/2) exists → the
    //     target-blind arithmetic identity  rem = n − (n / d) · d.
    //     C truncated-remainder semantics hold by construction — the
    //     identity IS C23 6.5.5's definition of % over truncated /.
    //     The subtract half is capability-driven in PREFERENCE order
    //     (D-LIR-MOD-MSUB-FUSION ✅ FC3.5 sweep-c3):
    //       (a) a declared fused `msub` (arm64 MSUB Xd,Xn,Xm,Xa =
    //           Xa − Xn·Xm) → msub r, q, d, n — ONE op (2-op modulo,
    //           what production compilers emit);
    //       (b) else the universal `mul` + `sub` pair (3-op modulo).
    //     A declared-but-result-less `msub` is a misdeclaration →
    //     fail loud, never a silent fall-through to (b).
    //
    //   Else → fail loud (no division realization declared).
    //
    // The previously sketched alternative — one result-bearing LIR op
    // whose result vreg the REGALLOC pins to the implicit output
    // register — was rejected at the FC1 plan-lock: no regalloc
    // pre-coloring mechanism exists (call results use an explicit
    // post-regalloc mov in `materializeOneFunc`), and building one
    // for a perf-only delta would speculatively erect the deferred
    // D-ML7-2.5 substrate.
    //
    // **Cycle 10r split rationale** (preserved) — cycle 10q packaged
    // CQO+IDIV into a single compound op with opcode bytes `[0x48,
    // 0x99, 0x48, 0xF7]`. The encoder auto-emits ONE REX prefix at
    // the start (e.g. `0x41` for REX.B when the divisor is in R14);
    // the embedded second `0x48` (REX.W only) then SUPERSEDED the
    // auto-REX prefix, losing REX.B. The IDIV operand decodes as
    // `modrm.rm low 3 bits` = 6 = RSI (without REX.B) instead of
    // R14 — dividing by garbage → STATUS_INTEGER_DIVIDE_BY_ZERO.
    // Splitting into two opcodes lets each instruction's REX be
    // auto-computed correctly with its operand's high bit.
    //
    // FLAG 1 (silent-miscompile guard, 2026-06-04, preserved): SDiv/
    // SMod route pre=cqo + core=idiv_op; UDiv/UMod route
    // pre=xor_rdx_zero + core=div_op. Routing the unsigned ops
    // through idiv would mis-sign-interpret any dividend ≥ INT_MAX
    // as negative — silent miscompile.
    void lowerDivLike(MirInstId id, bool isSigned, bool wantRemainder) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::optional<LirReg> const dividend = regForValue(operands[0]);
        std::optional<LirReg> const divisor  = regForValue(operands[1]);
        if (!dividend.has_value() || !divisor.has_value()) return;
        auto const result =
            emitDivLikeValue(id, isSigned, wantRemainder, *dividend, *divisor);
        if (!result.has_value()) return;  // diagnostic already emitted
        defineValue(id, *result);
    }

    // Emits the LIR realization of one division-family value and
    // returns the vreg holding the requested quotient/remainder
    // (nullopt = no realization available / misdeclared; diagnostic
    // already emitted). Split from `lowerDivLike` so rule 3's
    // expansion can realize its inner DIVISION through the same
    // rule-1/2 probing without re-running operand validation or
    // defining the MIR value twice.
    [[nodiscard]] std::optional<LirReg> emitDivLikeValue(
            MirInstId id, bool isSigned, bool wantRemainder,
            LirReg dividend, LirReg divisor) {
        // Rule 1 — native result-bearing opcode under the verb's mnemonic.
        MnemonicSlot const nativeSlot = wantRemainder
            ? (isSigned ? MnemonicSlot::SModNative : MnemonicSlot::UModNative)
            : (isSigned ? MnemonicSlot::SDivNative : MnemonicSlot::UDivNative);
        // FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): every COMPUTE op of the
        // realization (native div / pre+core pair / the mul+sub of the
        // remainder expansion) carries the MIR value's width so the
        // encoder picks the matching forms (I32/U32 → CDQ + 32-bit
        // idiv/div on x86, W-form sdiv/udiv/mul/sub on arm64). The
        // dividend-pin and capture MOVs stay 64-bit full-width copies
        // (value-correct: the 32-bit core op reads/writes only the low
        // 32 bits it defines).
        std::uint8_t const widthFlags = widthFlagsForType(mir.instType(id));
        if (auto const nativeOp = opcode(nativeSlot); nativeOp.has_value()) {
            auto const* ni = target.opcodeInfo(*nativeOp);
            if (ni == nullptr || ni->result != TargetResultRule::Value) {
                reportUnsupported(mir.instOpcode(id), id);
                return std::nullopt;
            }
            LirReg const result = lir.newVReg(regClassFor(id));
            std::array<LirOperand, 2> const ops{
                LirOperand::makeReg(dividend), LirOperand::makeReg(divisor)};
            emitInst(*nativeOp, result, ops, /*payload=*/0, widthFlags);
            return result;
        }
        // Rule 2 — implicit-register pre/core pair.
        DivSlotPair const pair = isSigned ? kSDivPair : kUDivPair;
        auto const preOp  = opcode(pair.pre);
        auto const coreOp = opcode(pair.core);
        if (preOp.has_value() || coreOp.has_value()) {
            if (!preOp.has_value()) {
                reportMissingOpcode(pair.pre,
                                    mirOpcodeName(mir.instOpcode(id)));
                return std::nullopt;
            }
            if (!coreOp.has_value()) {
                reportMissingOpcode(pair.core,
                                    mirOpcodeName(mir.instOpcode(id)));
                return std::nullopt;
            }
            return emitImplicitPairDiv(id, *preOp, *coreOp, wantRemainder,
                                       dividend, divisor);
        }
        // Rule 3 — remainder via the identity rem = n − (n/d)·d over a
        // rule-1/2 division.
        if (wantRemainder) {
            auto const quotient = emitDivLikeValue(
                id, isSigned, /*wantRemainder=*/false, dividend, divisor);
            if (!quotient.has_value()) return std::nullopt;
            // (a) Fused multiply-subtract when the target declares one
            // (D-LIR-MOD-MSUB-FUSION): msub r, q, d, n = n − q·d in ONE
            // instruction. Probed by mnemonic presence — the sanctioned
            // capability pattern; x86 never reaches rule 3 (its rule-2
            // pair realizes the remainder), so this fires for fixed32-
            // class targets that declare `msub` (arm64) and any future
            // ISA with a fused form. A target without `msub` keeps the
            // (b) mul+sub expansion below byte-identically.
            if (auto const msubOp = opcode(MnemonicSlot::MSub);
                msubOp.has_value()) {
                auto const* mi = target.opcodeInfo(*msubOp);
                if (mi == nullptr || mi->result != TargetResultRule::Value) {
                    // Misdeclared fused op (no value result) — fail loud
                    // like rule 1's native-verb misdeclaration; falling
                    // through to mul+sub would silently mask the broken
                    // declaration.
                    reportUnsupported(mir.instOpcode(id), id);
                    return std::nullopt;
                }
                LirReg const remainder = lir.newVReg(regClassFor(id));
                std::array<LirOperand, 3> const msubOps{
                    LirOperand::makeReg(*quotient),
                    LirOperand::makeReg(divisor),
                    LirOperand::makeReg(dividend)};
                emitInst(*msubOp, remainder, msubOps, /*payload=*/0,
                         widthFlags);
                return remainder;
            }
            // (b) Generic expansion over the universal mul + sub verbs.
            auto const mulOp = opcode(MnemonicSlot::Mul);
            if (!mulOp.has_value()) {
                reportMissingOpcode(MnemonicSlot::Mul,
                                    mirOpcodeName(mir.instOpcode(id)));
                return std::nullopt;
            }
            auto const subOp = opcode(MnemonicSlot::Sub);
            if (!subOp.has_value()) {
                reportMissingOpcode(MnemonicSlot::Sub,
                                    mirOpcodeName(mir.instOpcode(id)));
                return std::nullopt;
            }
            LirReg const product = lir.newVReg(regClassFor(id));
            std::array<LirOperand, 2> const mulOps{
                LirOperand::makeReg(*quotient), LirOperand::makeReg(divisor)};
            emitInst(*mulOp, product, mulOps, /*payload=*/0, widthFlags);
            LirReg const remainder = lir.newVReg(regClassFor(id));
            std::array<LirOperand, 2> const subOps{
                LirOperand::makeReg(dividend), LirOperand::makeReg(product)};
            emitInst(*subOp, remainder, subOps, /*payload=*/0, widthFlags);
            return remainder;
        }
        // No realization for a plain division: report the native verb
        // mnemonic as the canonical missing capability.
        reportMissingOpcode(nativeSlot, mirOpcodeName(mir.instOpcode(id)));
        return std::nullopt;
    }

    // Rule 2 body — the implicit-register pre/core sequence:
    //   mov  <dividend-role>_phys, dividend_vreg   ; pin dividend
    //   PRE                                        ; cqo / xor_rdx_zero
    //   CORE divisor_vreg                          ; idiv_op / div_op
    //   mov  result_vreg, <capture-role>_phys      ; quotient/remainder
    [[nodiscard]] std::optional<LirReg> emitImplicitPairDiv(
            MirInstId id, std::uint16_t preOpId, std::uint16_t coreOpId,
            bool wantRemainder, LirReg dividend, LirReg divisor) {
        auto const movOp = opcode(MnemonicSlot::Mov);
        if (!movOp.has_value()) {
            reportMissingOpcode(MnemonicSlot::Mov, "MIR Div/Mod (capture)");
            return std::nullopt;
        }
        // 7-agent review fold F2 (silent-failure 8/10, 2026-06-04):
        // validate that BOTH the pre AND core op carry implicit-
        // register declarations. If a future JSON edit drops
        // `implicitRegisters` from `cqo` or `xor_rdx_zero`, regalloc's
        // `collectImplicitClobberPositions` skips that position with
        // no diagnostic — divisor vreg could land in RDX, get zeroed
        // by xor, idiv would divide by zero. Fail loud at the lowering
        // tier captures the misconfiguration BEFORE regalloc silently
        // mis-allocates.
        auto const* preInfo = target.opcodeInfo(preOpId);
        if (preInfo == nullptr
         || !preInfo->implicitRegisters.has_value()
         || preInfo->implicitRegisters->clobberedOrdinals.empty()) {
            reportUnsupported(mir.instOpcode(id), id);
            return std::nullopt;
        }
        auto const* coreInfo = target.opcodeInfo(coreOpId);
        if (coreInfo == nullptr
         || !coreInfo->implicitRegisters.has_value()) {
            reportUnsupported(mir.instOpcode(id), id);
            return std::nullopt;
        }
        // Role-based register resolution (D-CSUBSET-MOD-OP-CODEGEN-
        // OUTPUT-INDEX-CONTRACT): the dividend pin and the captured
        // output are looked up BY ROLE from the core op's
        // inputRoles/outputRoles declarations — positional indexing
        // is gone from this projection path. An op missing the
        // required role is a misconfiguration → fail loud.
        auto const& coreIr = *coreInfo->implicitRegisters;
        auto const dividendOrdinal = coreIr.inputOrdinalForRole("dividend");
        if (!dividendOrdinal.has_value()) {
            reportMissingImplicitRole(coreOpId, "inputRoles", "dividend", id);
            return std::nullopt;
        }
        char const* const captureRole =
            wantRemainder ? "remainder" : "quotient";
        auto const captureOrdinal = coreIr.outputOrdinalForRole(captureRole);
        if (!captureOrdinal.has_value()) {
            reportMissingImplicitRole(coreOpId, "outputRoles", captureRole,
                                      id);
            return std::nullopt;
        }
        // 7-agent fold F1 (HIGH, 2026-06-04): read register CLASS
        // from the schema's register table (NOT hardcoded GPR). A
        // hypothetical future target whose dividend lives in an
        // FPR-class register would silently misallocate if this
        // hardcoded `LirRegClass::GPR`. The cast is sound — the two
        // enums are statically asserted identical-shape at
        // `lir_reg.hpp:96-100`.
        auto const* dividendRegInfo = target.registerInfo(*dividendOrdinal);
        auto const* captureRegInfo  = target.registerInfo(*captureOrdinal);
        if (dividendRegInfo == nullptr || captureRegInfo == nullptr) {
            reportUnsupported(mir.instOpcode(id), id);
            return std::nullopt;
        }
        LirRegClass const dividendCls =
            static_cast<LirRegClass>(dividendRegInfo->regClass);
        LirRegClass const captureCls =
            static_cast<LirRegClass>(captureRegInfo->regClass);
        LirReg const dividendPhys =
            makePhysicalReg(*dividendOrdinal, dividendCls);

        // FC3 c2: the PRE and CORE ops carry the value's width — for
        // I32/U32 the pair selects the 32-bit forms (x86: CDQ [99, the
        // no-REX.W cqo sibling] + 32-bit F7 /7 idiv / /6 div). The
        // pin/capture movs stay 64-bit (full-register copies).
        std::uint8_t const pairWidthFlags =
            widthFlagsForType(mir.instType(id));

        // 1. Pin dividend into the role-declared register.
        std::array<LirOperand, 1> const movInOps{
            LirOperand::makeReg(dividend)};
        emitInst(*movOp, dividendPhys, movInOps);

        // 2. Emit PRE op (CQO / XOR RDX,RDX). Zero operands, no
        //    SSA result. The op declares its own implicit-input/
        //    output/clobber set (RAX → RDX for CQO; RDX clobber for
        //    XOR). Regalloc reads each PRE op's implicit-register
        //    declaration independently — no extra coupling required.
        emitInst(preOpId, InvalidLirReg, std::span<LirOperand const>{},
                 /*payload=*/0, pairWidthFlags);

        // 3. Emit CORE op (IDIV /7 or DIV /6). One operand (divisor
        //    in modrm.rm), no SSA result; the op declares
        //    `result: none` because outputs live in implicit phys
        //    regs. Regalloc reads the implicit-clobbered set from
        //    the schema and excludes the clobbered regs from any
        //    range that COVERS this position.
        std::array<LirOperand, 1> const divOps{
            LirOperand::makeReg(divisor)};
        emitInst(coreOpId, InvalidLirReg, divOps,
                 /*payload=*/0, pairWidthFlags);

        // 4. Capture the role-selected output (quotient for div,
        //    remainder for mod) into a fresh SSA result.
        LirReg const result = lir.newVReg(regClassFor(id));
        LirReg const capturePhys =
            makePhysicalReg(*captureOrdinal, captureCls);
        std::array<LirOperand, 1> const captureOps{
            LirOperand::makeReg(capturePhys)};
        emitInst(*movOp, result, captureOps);
        return result;
    }

    void reportMissingImplicitRole(std::uint16_t opId, char const* mapName,
                                   char const* role, MirInstId at) {
        auto const* info = target.opcodeInfo(opId);
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_RequiredLirOpcodeMissing;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "target '{}': opcode '{}' declares no '{}' role in "
            "implicitRegisters.{} — required to lower MIR '{}' (inst {}); "
            "the div/mod projection resolves registers BY ROLE, never by "
            "positional index (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-"
            "CONTRACT)",
            target.name(), info != nullptr ? info->mnemonic : "?",
            role, mapName, mirOpcodeName(mir.instOpcode(at)), at.v);
        reporter.report(std::move(d));
    }

    // MIR ICmp{Eq,Ne,Slt,...} → LIR `cmp` + `setcc(cond)` pair. Naive
    // (no peephole with subsequent CondBr); the optimizer's compare-flag
    // forwarding pass will collapse `cmp/setcc/cmp 0/jcc-ne` to
    // `cmp/jcc-cond` once a use-count analysis lands.
    void lowerICmp(MirInstId id, TargetCondCode cond) {
        if (!opcode(MnemonicSlot::Cmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Cmp, "MIR ICmp");
            return;
        }
        if (!opcode(MnemonicSlot::Setcc).has_value()) {
            reportMissingOpcode(MnemonicSlot::Setcc, "MIR ICmp");
            return;
        }
        if (!opcode(MnemonicSlot::ZExt).has_value()) {
            reportMissingOpcode(MnemonicSlot::ZExt, "MIR ICmp");
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::optional<LirReg> const a = regForValue(operands[0]);
        std::optional<LirReg> const b = regForValue(operands[1]);
        if (!a.has_value() || !b.has_value()) return;
        // FLAGS is implicit pre-regalloc; ML6 makes it explicit. Emit the
        // cmp + setcc as paired instructions; the substrate guarantees no
        // value-producing inst sits between them (cycle 3a fallback-seal
        // + cycle 3b ICmp-immediately-followed-by-setcc).
        // FC3 c2: the compare width follows the OPERANDS' type (the
        // result is Bool) — post-UAC both operands carry the same
        // type, so operand 0 is authoritative. A U32/I32 compare
        // selects the 32-bit cmp form (D-CSUBSET-32BIT-ALU-FORMS).
        std::array<LirOperand, 2> cmpOps{LirOperand::makeReg(*a), LirOperand::makeReg(*b)};
        emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps,
                 /*payload=*/0, widthFlagsForType(mir.instType(operands[0])));

        // D-LIR-SETCC-WIDTH-CONTRACT (step 13.5 cycle 1 post-fold,
        // code-reviewer C2): emit setcc into a separate "byte" vreg,
        // then movzx the low byte into the result vreg. This makes
        // the substrate contract explicit: setcc writes the LOW
        // BYTE of its r/m8 destination; the upper 56 bits are
        // architecturally undefined per Intel SDM. The standalone
        // zext makes the zero-extend a first-class LIR operation
        // so the result is always a CLEAN 0/1 r64 — `cmp r64, 0`
        // / `cmp r64, r64` consumers downstream cannot read garbage.
        // The ICmp+CondBr fusion at `lowerCondBr` still elides the
        // cmp-against-0 (the setcc + zext become "dead" via D-LIR-
        // SETCC-DEAD-AFTER-FUSION DCE in 13.6), but non-adjacent
        // ICmp uses (bool stored, bool returned, bool ternary) now
        // get correct width semantics for free. Clean SSA: setcc
        // defines `b8`; zext consumes `b8` AND defines `result`.
        LirReg const b8 = lir.newVReg(LirRegClass::GPR);
        emitInst(*opcode(MnemonicSlot::Setcc), b8, std::span<LirOperand const>{},
                    /*payload=*/static_cast<std::uint32_t>(cond));
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 1> zextOps{LirOperand::makeReg(b8)};
        emitInst(*opcode(MnemonicSlot::ZExt), result, zextOps);
        defineValue(id, result);
    }

    // ── FC3.5 sweep-c2: MIR FCmp lowering ────────────────────────────
    //
    // Shape (mirrors lowerICmp's cmp+setcc+zext): emit the float
    // compare (`fcmp` — UCOMISD/UCOMISS on x86, FCMP D/S on arm64;
    // width-axis-selected by the OPERANDS' float type), then
    // materialize the predicate from the flags by the capability-
    // driven plan (`floatCmpPlan` — see its derivation comment):
    //
    //  * single-cc (the target declares the predicate's float nibble
    //    in `condCodeEncoding`): ONE setcc(cc) + zext — identical to
    //    the integer tail.
    //  * composed (nibble undeclared — x86 Oeq/Une, arm64 One): TWO
    //    setcc+zext pairs over the flag-exact halves (the integer
    //    Eq/Ne nibble + the Ford/Fuo parity/overflow nibble) folded
    //    by `and`/`or`. FLAGS survive the interleaved insts: setcc /
    //    movzx (x86) and CSINC / ORR-mov (arm64) do not write flags,
    //    and any regalloc spill movs between them don't either — the
    //    second setcc still reads the fcmp's flags.
    //
    // Operand-swap canonicalization (Olt/Ole → swapped Fogt/Foge) is
    // part of the plan; the swap is exact under IEEE 754 including
    // the unordered outcome (both sides false).
    void lowerFCmp(MirInstId id) {
        MirOpcode const op = mir.instOpcode(id);
        auto const plan = floatCmpPlan(op);
        if (!plan.has_value()) {
            // Ueq/Ult/Ule/Ugt/Uge — no C producer, no realization.
            reportUnsupported(op, id);
            poisonValue(id);
            return;
        }
        if (!opcode(MnemonicSlot::FCmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::FCmp, "MIR FCmp");
            poisonValue(id);
            return;
        }
        if (!opcode(MnemonicSlot::Setcc).has_value()) {
            reportMissingOpcode(MnemonicSlot::Setcc, "MIR FCmp");
            poisonValue(id);
            return;
        }
        if (!opcode(MnemonicSlot::ZExt).has_value()) {
            reportMissingOpcode(MnemonicSlot::ZExt, "MIR FCmp");
            poisonValue(id);
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) {
            reportUnsupported(op, id);
            poisonValue(id);
            return;
        }
        // Gate + width: the compare's encoded width follows the
        // OPERANDS' float type (the result is Bool); post-UAC both
        // operands carry the same type, so operand 0 is authoritative
        // (the lowerICmp rule).
        if (!requireEncodedFloatWidth(id, mir.instType(operands[0]),
                                      "MIR FCmp operand")) {
            return;
        }
        if (!emitFloatCompare(id, *plan, operands)) {
            poisonValue(id);
            return;
        }
        // Materialize per the plan: single setcc when the target
        // declares the nibble; else the composed two-setcc shape.
        if (target.condCodeEncoding(plan->single).has_value()) {
            LirReg const b8 = lir.newVReg(LirRegClass::GPR);
            emitInst(*opcode(MnemonicSlot::Setcc), b8,
                     std::span<LirOperand const>{},
                     /*payload=*/static_cast<std::uint32_t>(plan->single));
            LirReg const result = lir.newVReg(LirRegClass::GPR);
            std::array<LirOperand, 1> zextOps{LirOperand::makeReg(b8)};
            emitInst(*opcode(MnemonicSlot::ZExt), result, zextOps);
            defineValue(id, result);
            return;
        }
        if (!plan->composition.has_value()) {
            // A required-single predicate (Ogt/Oge family) whose
            // nibble the target does not declare: no composed
            // fallback exists — fail loud naming the missing entry.
            dss::report(reporter,
                DiagnosticCode::L_RequiredLirOpcodeMissing,
                DiagnosticSeverity::Error,
                std::format(
                    "MIR→LIR: target '{}' declares no `condCodeEncoding` "
                    "entry for float condition '{}' (required to lower "
                    "MIR '{}') and the predicate has no composed "
                    "realization — declare the nibble in the target "
                    "schema (FC3.5 FCmp lowering)",
                    target.name(), targetCondCodeName(plan->single),
                    mirOpcodeName(op)));
            poisonValue(id);
            return;
        }
        FloatCmpComposition const& comp = *plan->composition;
        if (!target.condCodeEncoding(comp.partA).has_value()
            || !target.condCodeEncoding(comp.partB).has_value()) {
            dss::report(reporter,
                DiagnosticCode::L_RequiredLirOpcodeMissing,
                DiagnosticSeverity::Error,
                std::format(
                    "MIR→LIR: target '{}' declares neither a single "
                    "`condCodeEncoding` entry for float condition '{}' "
                    "nor both composition ingredients '{}' + '{}' "
                    "(required to lower MIR '{}') — declare one of the "
                    "two realizations (FC3.5 FCmp lowering)",
                    target.name(), targetCondCodeName(plan->single),
                    targetCondCodeName(comp.partA),
                    targetCondCodeName(comp.partB), mirOpcodeName(op)));
            poisonValue(id);
            return;
        }
        auto const combineOp = opcode(comp.combine);
        if (!combineOp.has_value()) {
            reportMissingOpcode(comp.combine, "MIR FCmp (composed)");
            poisonValue(id);
            return;
        }
        auto const setccZext = [&](TargetCondCode cc) -> LirReg {
            LirReg const b8 = lir.newVReg(LirRegClass::GPR);
            emitInst(*opcode(MnemonicSlot::Setcc), b8,
                     std::span<LirOperand const>{},
                     /*payload=*/static_cast<std::uint32_t>(cc));
            LirReg const wide = lir.newVReg(LirRegClass::GPR);
            std::array<LirOperand, 1> ops{LirOperand::makeReg(b8)};
            emitInst(*opcode(MnemonicSlot::ZExt), wide, ops);
            return wide;
        };
        LirReg const a = setccZext(comp.partA);
        LirReg const b = setccZext(comp.partB);
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 2> combineOps{
            LirOperand::makeReg(a), LirOperand::makeReg(b)};
        emitInst(*combineOp, result, combineOps);
        defineValue(id, result);
    }

    // Emit the `fcmp` flag-writing compare for `id` per the plan's
    // operand order. Shared by lowerFCmp (materialization — which
    // poisons `id` on false) and lowerCondBr's fused single-cc branch
    // path (which must NOT poison: the FCmp value was already defined
    // by its own lowering). Returns false when an operand vreg is
    // missing (already diagnosed by regForValue).
    bool emitFloatCompare(MirInstId id, FloatCmpPlan const& plan,
                          std::span<MirInstId const> operands) {
        std::optional<LirReg> const a = regForValue(operands[0]);
        std::optional<LirReg> const b = regForValue(operands[1]);
        if (!a.has_value() || !b.has_value()) {
            return false;
        }
        LirReg const lhs = plan.swapOperands ? *b : *a;
        LirReg const rhs = plan.swapOperands ? *a : *b;
        std::array<LirOperand, 2> cmpOps{
            LirOperand::makeReg(lhs), LirOperand::makeReg(rhs)};
        emitInst(*opcode(MnemonicSlot::FCmp), InvalidLirReg, cmpOps,
                 /*payload=*/0,
                 widthFlagsForType(mir.instType(operands[0])));
        return true;
    }

    // ── terminator + phi resolution ──────────────────────────────────

    // Emit the phi-edge moves for one (predecessor, successor) edge.
    //
    // Algorithm — parallel-copy resolution via temporaries:
    //   For each phi P in `successorMir` with incoming I_P from
    //   `currentMir`, build the pair (phiReg_P, incomingReg_P). Multiple
    //   phis in the same block may form a copy graph where phi A's
    //   incoming is phi B's pre-allocated result (the classic SSA-
    //   destruction "swap" hazard). Naive sequential `mov phi, incoming`
    //   would corrupt later reads of the overwritten incomingReg.
    //
    //   Two-step emission breaks all dependency cycles unconditionally:
    //   (1) for each (phiReg, incomingReg) pair, mint a fresh `tmpReg`
    //       and emit `mov tmpReg, incomingReg`. The incomingReg's value
    //       is now captured in a vreg that no subsequent phi-move reads.
    //   (2) for each pair, emit `mov phiReg, tmpReg`.
    //
    //   Trade-off: O(n) extra vregs vs O(n^2) cycle detection. Pre-
    //   regalloc this is cheap; ML6 coalescing will collapse the temps
    //   into direct copies where the dep-graph allows.
    //
    // The diagnostic shape pins:
    //  - phi without a pre-allocated vreg (substrate bug) → loud
    //  - incoming `value` resolves to a missing vreg → phi-specific
    //    diagnostic (NOT just regForValue's generic one), and the move
    //    is SKIPPED so a poisoned tmp does not silently propagate
    //  - phi without an incoming for this predecessor → loud
    void emitPhiMovesForEdge(MirBlockId currentMir, MirBlockId successorMir) {
        // Collect (phiReg, incomingReg) pairs for this edge.
        struct Pair { LirReg phi; LirReg incoming; };
        std::vector<Pair> pairs;
        std::uint32_t const n = mir.blockInstCount(successorMir);
        for (std::uint32_t i = 0; i < n; ++i) {
            MirInstId const inst = mir.blockInstAt(successorMir, i);
            if (mir.instOpcode(inst) != MirOpcode::Phi) continue;
            if (!valueToReg.has(inst)) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR Phi %{} reached edge-move emission without a "
                    "pre-allocated LirReg — pre-pass invariant broken",
                    inst.v);
                reporter.report(std::move(d));
                continue;
            }
            LirReg const phiReg = valueToReg.get(inst);
            bool matched = false;
            for (MirPhiIncoming const& inc : mir.phiIncomings(inst)) {
                if (inc.pred != currentMir) continue;
                matched = true;
                std::optional<LirReg> const v = regForValue(inc.value);
                if (!v.has_value()) {
                    // regForValue already emitted a generic diagnostic;
                    // add the phi-edge-specific context so the failure is
                    // locally attributable.
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                    d.severity = DiagnosticSeverity::Error;
                    d.actual   = std::format(
                        "MIR Phi %{} edge from predecessor block %{} lost "
                        "its incoming value %{} — phi result will be "
                        "unwritten on this edge",
                        inst.v, currentMir.v, inc.value.v);
                    reporter.report(std::move(d));
                    break;
                }
                pairs.push_back(Pair{phiReg, *v});
                break;
            }
            if (!matched) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR Phi %{} has no incoming for predecessor block %{}",
                    inst.v, currentMir.v);
                reporter.report(std::move(d));
            }
        }
        if (pairs.empty()) return;
        // Step 1: copy every incoming into a fresh temp. Breaks any
        // dependency cycle on the phi-reg side unconditionally.
        // Cycle 3d FPR plumbing: each temp's class follows the phi's
        // (which equals the incoming's, since SSA guarantees same-
        // typed flow on each edge). An F64 phi must use FPR-class
        // temps so the parallel-copy chain stays consistent.
        // FC2 Part B: each copy's mnemonic follows the pair's class
        // via the registerClassOps table (FPR pairs copy via movaps,
        // never the GPR mov).
        std::vector<LirReg> temps;
        temps.reserve(pairs.size());
        for (auto const& p : pairs) {
            auto const movOp = classOp(p.phi.regClass(), RegClassOp::Move);
            if (!movOp.has_value()) {
                reportMissingClassOp(p.phi.regClass(), RegClassOp::Move,
                                     "MIR Phi edge");
                return;
            }
            LirReg const tmp = lir.newVReg(p.phi.regClass());
            std::array<LirOperand, 1> ops{LirOperand::makeReg(p.incoming)};
            emitInst(*movOp, tmp, ops);
            temps.push_back(tmp);
        }
        // Step 2: write each phi-result from its captured temp.
        for (std::size_t k = 0; k < pairs.size(); ++k) {
            auto const movOp = classOp(pairs[k].phi.regClass(), RegClassOp::Move);
            if (!movOp.has_value()) return;  // diagnosed in step 1
            std::array<LirOperand, 1> ops{LirOperand::makeReg(temps[k])};
            emitInst(*movOp, pairs[k].phi, ops);
        }
    }

    // Returns true iff the terminator was emitted (block sealed). `false`
    // signals the caller to use the fallback seal.
    bool lowerTerminator(MirBlockId mb, MirInstId termId) {
        currentMir = termId;
        MirOpcode const op = mir.instOpcode(termId);
        auto succs = mir.blockSuccessors(mb);

        // Emit phi-edge moves for EVERY MIR successor of this block, in
        // declared order, BEFORE the terminator. This dominates every use
        // of the phi result in the successor regardless of which jcc arm
        // ends up taking the edge.
        for (MirBlockId s : succs) {
            emitPhiMovesForEdge(mb, s);
        }

        switch (op) {
            case MirOpcode::Return:      return lowerReturn(termId);
            case MirOpcode::Br:          return lowerBr(termId, succs);
            case MirOpcode::CondBr:      return lowerCondBr(termId, succs);
            case MirOpcode::Switch:      return lowerSwitch(termId, succs);
            case MirOpcode::IndirectBr:  return lowerIndirectBr(termId, succs);
            case MirOpcode::Unreachable: return lowerUnreachable(termId);
            default:                     break;
        }
        reportUnsupported(op, termId);
        return false;
    }

    bool lowerReturn(MirInstId id) {
        if (!opcode(MnemonicSlot::Ret).has_value()) {
            reportMissingOpcode(MnemonicSlot::Ret, "MIR Return");
            return false;
        }
        auto const operands = mir.instOperands(id);
        if (operands.empty()) {
            emitReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
            return true;
        }
        // FC7 C1c: a by-value struct/union returned IN REGISTERS carries N
        // eightbyte pieces (SysV ≤16B → 2); a scalar / sret-pointer return
        // carries one. lir_callconv moves each piece into its per-class return
        // register (cycle-broken) before the ret. Carry every operand as a reg.
        std::vector<LirOperand> ops;
        ops.reserve(operands.size());
        for (MirInstId const operand : operands) {
            std::optional<LirReg> const v = regForValue(operand);
            if (!v.has_value()) return false;
            ops.push_back(LirOperand::makeReg(*v));
        }
        emitReturn(*opcode(MnemonicSlot::Ret), ops);
        return true;
    }

    bool lowerBr(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opcode(MnemonicSlot::Jmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Jmp, "MIR Br");
            return false;
        }
        if (succs.size() != 1) {
            reportUnsupported(MirOpcode::Br, id);
            return false;
        }
        if (!mirBlockToLirBlock.has(succs[0])) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "MIR Br target block %{} has no LIR block mapping",
                succs[0].v);
            reporter.report(std::move(d));
            return false;
        }
        emitBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(succs[0]));
        return true;
    }

    bool lowerCondBr(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opcode(MnemonicSlot::Cmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Cmp, "MIR CondBr");
            return false;
        }
        if (!opcode(MnemonicSlot::Jcc).has_value()) {
            reportMissingOpcode(MnemonicSlot::Jcc, "MIR CondBr");
            return false;
        }
        if (succs.size() != 2) {
            reportUnsupported(MirOpcode::CondBr, id);
            return false;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) {
            reportUnsupported(MirOpcode::CondBr, id);
            return false;
        }
        if (!mirBlockToLirBlock.has(succs[0]) || !mirBlockToLirBlock.has(succs[1])) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = "MIR CondBr target block(s) have no LIR block mapping";
            reporter.report(std::move(d));
            return false;
        }
        LirBlockId const lirIfTrue  = mirBlockToLirBlock.get(succs[0]);
        LirBlockId const lirIfFalse = mirBlockToLirBlock.get(succs[1]);
        // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1):
        // ICmp+CondBr FUSION. When the cond operand was produced by
        // an ICmp instruction, replace the naive
        // `cmp result_b8, 0; jcc-NE` sequence (which would read the
        // garbage upper 56 bits of setcc's r8 result and trip the
        // branch the wrong way) with a single fused `cmp lhs, rhs;
        // jcc-cond` using the ICmp's args and condition directly.
        // The setcc emitted by lowerICmp becomes dead code (its
        // result has no LIR consumer in this fused path); a future
        // dead-LIR-instruction-elimination pass anchored
        // D-LIR-SETCC-DEAD-AFTER-FUSION removes the wasted bytes.
        // This pre-empts the optimizer fusion (planned at 13.6)
        // because the substrate would otherwise be load-bearing
        // broken for every `if/while` until then.
        //
        // The non-fusable arm (cond from a non-ICmp source — bool
        // param, bool load, etc.) keeps the existing cmp-against-0
        // path. Those producers ARE responsible for zero-extending
        // their bool result; they hit the cmp-r64-against-0 path
        // correctly.
        MirInstId const condInst = operands[0];
        MirOpcode const condOp   = mir.instOpcode(condInst);
        auto const fusedCc       = condCodeForICmp(condOp);
        // FC3.5 sweep-c2: FCmp+CondBr fusion — ONLY for predicates the
        // target realizes as a SINGLE declared condition code (the
        // capability check below). A composed predicate (x86 Oeq/Une,
        // arm64 One) takes the non-fused arm: lowerFCmp already
        // materialized the composed Bool, and the cmp-against-0 + jne
        // path branches on it — correct, just not fused (a jcc-pair /
        // flag-forwarding fusion for composed float conditions is an
        // optimizer peephole, deliberately NOT done here).
        auto const floatPlan = floatCmpPlan(condOp);
        bool const fuseFloat =
            floatPlan.has_value()
            && opcode(MnemonicSlot::FCmp).has_value()
            && target.condCodeEncoding(floatPlan->single).has_value();
        TargetCondCode jccCond;
        if (fuseFloat) {
            auto const fcmpOperands = mir.instOperands(condInst);
            if (fcmpOperands.size() != 2) {
                reportUnsupported(condOp, condInst);
                return false;
            }
            if (!requireEncodedFloatWidth(condInst,
                                          mir.instType(fcmpOperands[0]),
                                          "MIR FCmp operand (fused)")) {
                return false;
            }
            if (!emitFloatCompare(condInst, *floatPlan, fcmpOperands)) {
                return false;
            }
            jccCond = floatPlan->single;
        } else if (fusedCc.has_value()) {
            auto const icmpOperands = mir.instOperands(condInst);
            if (icmpOperands.size() != 2) {
                reportUnsupported(condOp, condInst);
                return false;
            }
            std::optional<LirReg> const a = regForValue(icmpOperands[0]);
            std::optional<LirReg> const b = regForValue(icmpOperands[1]);
            if (!a.has_value() || !b.has_value()) return false;
            std::array<LirOperand, 2> cmpOps{
                LirOperand::makeReg(*a), LirOperand::makeReg(*b)};
            // FC3 c2: the fused compare's width follows the ICmp
            // OPERANDS' type — the same rule as lowerICmp's cmp
            // (D-CSUBSET-32BIT-ALU-FORMS). Pinned (audit-residue sweep
            // c1, D-AUDIT-FUSED-CMP-WIDTH-PIN): the Fused{I32,I64}…
            // width pins in tests/lir/test_mir_to_lir.cpp + the
            // examples/c-subset/fused_negative_compare runtime witness
            // (width-64 here flips its exit 42 → 7: a zero-extended
            // negative I32 reads as positive).
            emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps,
                     /*payload=*/0,
                     widthFlagsForType(mir.instType(icmpOperands[0])));
            jccCond = *fusedCc;
        } else {
            std::optional<LirReg> const cond = regForValue(operands[0]);
            if (!cond.has_value()) return false;
            std::array<LirOperand, 2> cmpOps{
                LirOperand::makeReg(*cond), LirOperand::makeImmInt32(0)};
            // The non-fused arm's cond is a Bool/byte producer (its
            // own producer zero-extended it) — width follows its type
            // (Bool → the 64-bit default; the imm-form cmp variants).
            emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps,
                     /*payload=*/0,
                     widthFlagsForType(mir.instType(operands[0])));
            jccCond = TargetCondCode::Ne;
        }
        // Pass both successors as BlockRef operands. jcc remains
        // the block's terminator (schema's `cond-br` shape, 2
        // successors); the encoder reads operand[0] (taken target)
        // for the BlockRel32 displacement AND emits a trailing
        // unconditional `jmp` to operand[1] (fallthrough) so the
        // LIR block layout doesn't need to guarantee fallthrough
        // order. A future optimizer pass elides the redundant
        // trailing jmp when ifFalse IS the next-laid-out block
        // (anchored D-OPT-JCC-FALLTHROUGH).
        std::array<LirOperand, 2> jccOps{
            LirOperand::makeBlockRef(lirIfTrue.v),
            LirOperand::makeBlockRef(lirIfFalse.v)};
        emitCondBr(*opcode(MnemonicSlot::Jcc), jccOps,
                      lirIfTrue, lirIfFalse,
                      static_cast<std::uint32_t>(jccCond));
        return true;
    }

    // D-OPT-SWITCH-JUMP-TABLE (c70): the jump-table density heuristic.
    //   * A dense switch is one whose value SPAN (maxCase-minCase+1) is at most
    //     `kJumpTableDensityFactor` times its case COUNT — i.e. at most that many
    //     table slots per real case (gap slots point at the default block). 3 is
    //     a middle ground: tighter than a pure "any range" table (which could
    //     waste memory on a `case 0; case 1000000;` pair) yet loose enough that
    //     real dispatch tables with a scattering of gaps (sqlite's VDBE has a few)
    //     still qualify. gcc/clang use ~1.5–2; 3 leans slightly toward tables,
    //     which is safe here because a gap costs only 8 idle `.data` bytes, never
    //     an instruction on the hot path.
    //   * `kJumpTableMinCases` keeps tiny switches (< 8 cases) on the cheap
    //     immediate-compare chain, where a table's fixed overhead (a `.data`
    //     item + bounds check + two `lea`s + a load + an indirect branch)
    //     outweighs the O(1) benefit.
    //   * `kJumpTableMaxSlots` is a hard cap so a pathological but "dense-ratio"
    //     range (e.g. millions of cases) can't request an enormous table.
    // RED-ON-DISABLE: raise `kJumpTableMinCases` past any real case count (e.g.
    // to SIZE_MAX) to force EVERY switch down the immediate-compare chain — that
    // proves the sparse path alone also fixes the register pressure, and,
    // combined with reverting the immediate-compare change, reproduces the
    // original scratch-pool exhaustion.
    static constexpr std::size_t kJumpTableMinCases      = 8;
    static constexpr std::uint64_t kJumpTableDensityFactor = 3;
    static constexpr std::uint64_t kJumpTableMaxSlots    = 1u << 20;  // 1M slots (8 MiB)

    // Attempt the dense jump-table lowering of a MIR Switch. Returns true iff it
    // took the table path (and fully emitted the switch); false means the caller
    // must fall through to the compare chain (not dense / a non-constant case /
    // a missing opcode / a range too wide). Emits NO diagnostics on a false
    // return — a fall-through is not an error.
    //
    // MIR Switch convention: operands = [discriminant, caseConst*N]; successors =
    // [caseTarget*N, default]. The emitted shape (arch-blind — every opcode ships
    // on x86_64 AND arm64) is:
    //
    //   ; in the switch-bearing header block (phi moves already emitted here):
    //   idx    = discrim - minCase            ; sub, at the discriminant width
    //   cmp idx, (span-1)                      ; UNSIGNED compare
    //   jcc(Ugt) default, bodyBlock            ; one unsigned test covers BOTH
    //                                          ;   idx<0 (wraps huge) and idx>max
    //   ; bodyBlock:
    //   idx64  = (s/z)ext idx                  ; widen to a 64-bit address index
    //   off    = idx64 << 3                    ; scale by slot size (8)
    //   base   = lea [jumpTableSymbol]         ; table base address
    //   addr   = lea [base + off]              ; &table[idx]
    //   target = load [addr]                   ; the case block's runtime address
    //   jmp *target  (succs = all distinct case blocks + default)
    //
    // The `.data` table itself (span 8-byte slots, each an abs64 reloc to a
    // synthetic per-block symbol) is NOT emitted here — a JumpTableDescriptor is
    // recorded and `compile_pipeline.cpp` materializes the AssembledData after
    // `assemble()` resolves block byte offsets. See JumpTableDescriptor.
    bool tryLowerSwitchJumpTable(MirInstId id,
                                 std::span<MirInstId const> operands,
                                 std::span<MirBlockId const> succs,
                                 std::size_t caseCount) {
        if (caseCount < kJumpTableMinCases) return false;

        // Every case value must be a fold-constant (a C case label is an integer
        // constant expression — this always holds; a non-constant is malformed
        // MIR and we simply decline the table, letting the compare chain report).
        // Collect (value → case index) and min/max in one pass.
        std::vector<std::int64_t> caseValues;
        caseValues.reserve(caseCount);
        std::int64_t minCase = std::numeric_limits<std::int64_t>::max();
        std::int64_t maxCase = std::numeric_limits<std::int64_t>::min();
        for (std::size_t i = 0; i < caseCount; ++i) {
            std::optional<std::int64_t> const v = constIntegerValue(operands[1 + i]);
            if (!v.has_value()) return false;
            caseValues.push_back(*v);
            if (*v < minCase) minCase = *v;
            if (*v > maxCase) maxCase = *v;
        }

        // Span = maxCase - minCase + 1, computed WIDE to dodge signed overflow
        // (e.g. minCase INT64_MIN, maxCase INT64_MAX). The unsigned difference of
        // two int64s is exact; +1 is the slot count.
        std::uint64_t const spanMinusOne =
            static_cast<std::uint64_t>(maxCase) - static_cast<std::uint64_t>(minCase);
        if (spanMinusOne == std::numeric_limits<std::uint64_t>::max()) {
            return false;  // span+1 would overflow — decline (compare chain).
        }
        std::uint64_t const span = spanMinusOne + 1;
        if (span > kJumpTableMaxSlots) return false;               // absurdly wide
        if (span > kJumpTableDensityFactor * caseCount) return false;  // too sparse
        // minCase must fit i32 so `idx = discrim - minCase` and the bounds `cmp`
        // stay clean immediate forms. True for every C `int`/`enum` discriminant
        // (case labels are `int`); a huge-negative label on a 64-bit discriminant
        // is declined to the compare chain (its register fallback handles it).
        if (minCase < std::numeric_limits<std::int32_t>::min()
            || minCase > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }

        // All opcodes the table shape needs — decline (fall to compare chain) if
        // any is absent for this target rather than emitting a half-formed table.
        auto const subOp  = opcode(MnemonicSlot::Sub);
        auto const mulOp  = opcode(MnemonicSlot::Mul);
        auto const leaOp  = opcode(MnemonicSlot::Lea);
        auto const jmpInd = opcode(MnemonicSlot::JmpIndirect);
        auto const loadOp = classOp(LirRegClass::GPR, RegClassOp::Load);
        if (!subOp.has_value() || !mulOp.has_value() || !leaOp.has_value()
            || !jmpInd.has_value() || !loadOp.has_value()
            || !opcode(MnemonicSlot::Cmp).has_value()
            || !opcode(MnemonicSlot::Jcc).has_value()) {
            return false;
        }
        // The index widen needs SExt (the discriminant is a signed C `int`; the
        // bounded index is non-negative after the bounds check, so a zero-extend
        // would be equally correct, but SExt matches the source-signedness rule).
        // Classify the discriminant width/signedness to pick the widen: an
        // already-64-bit discriminant needs none; a signed sub-64 one SExt's; an
        // unsigned sub-64 one ZExt's (mirrors the c42 GEP-index widen rule so an
        // `unsigned`/`unsigned short`/`unsigned char` discriminant with the high
        // bit set is NOT mis-sign-extended).
        std::optional<MnemonicSlot> discrimWidenSlot;
        switch (interner.kind(mir.instType(operands[0]))) {
            case TypeKind::I64: case TypeKind::U64: case TypeKind::Ptr:
                break;  // already 64-bit
            case TypeKind::U8: case TypeKind::U16: case TypeKind::U32:
                discrimWidenSlot = MnemonicSlot::ZExt;
                break;
            case TypeKind::Char: case TypeKind::I8:
            case TypeKind::I16:  case TypeKind::I32:
                discrimWidenSlot = MnemonicSlot::SExt;
                break;
            default:
                return false;  // a non-integer discriminant is not a C switch shape
        }
        if (discrimWidenSlot.has_value()
            && !opcode(*discrimWidenSlot).has_value()) {
            return false;
        }

        // Every target block (case + default) must have a LIR block.
        for (std::size_t i = 0; i < caseCount; ++i) {
            if (!mirBlockToLirBlock.has(succs[i])) return false;
        }
        MirBlockId const defaultMir = succs[caseCount];  // already checked by caller

        std::optional<LirReg> const discrim = regForValue(operands[0]);
        if (!discrim.has_value()) return false;

        std::uint8_t const discrimWidth = widthFlagsForType(mir.instType(operands[0]));

        // ── Bounds check, emitted into the still-open switch-bearing (header)
        //    block so the phi-edge moves already emitted there keep dominating
        //    every target (identical discipline to the compare chain). ──────────
        LirBlockId const header = lir.openBlock();

        // WIDEN the discriminant to a full 64-bit register FIRST, then do the
        // index arithmetic + bounds test + address scale all at 64-bit. Two
        // reasons: (1) c42 D-CSUBSET-INDEX-NEGATIVE-WIDEN — a sub-64-bit value fed
        // into the 64-bit address read leaves the upper half garbage; sign-
        // extending up front makes `discrim - minCase` correct in 64 bits for a
        // negative discriminant / negative minCase. (2) Arch-uniformity — arm64's
        // `sub reg, imm32` variant is 64-bit-only (no width-32 imm sub), so a
        // 32-bit immediate index-sub would fail to encode; at 64-bit it matches on
        // both ISAs. A signed C `int` discriminant is SExt'd, an unsigned one
        // ZExt'd (discrimWidenSlot); an already-64-bit discriminant is used as-is.
        LirReg discrim64 = *discrim;
        if (discrimWidenSlot.has_value()) {
            LirReg const widened = lir.newVReg(LirRegClass::GPR);
            std::array<LirOperand, 1> wOps{LirOperand::makeReg(*discrim)};
            emitInst(*opcode(*discrimWidenSlot), widened, wOps, /*payload=*/0,
                     discrimWidth);
            discrim64 = widened;
        }

        // idx = discrim64 - minCase, at 64-bit width. minCase is an immediate when
        // it fits the arch-blind window [0, 4095]; a wider or negative minCase is
        // register-materialized first (arm64's `sub`-imm is a non-negative imm12).
        LirReg const idxReg = lir.newVReg(LirRegClass::GPR);
        {
            std::optional<LirOperand> const minOperand = aluImmOrReg(minCase);
            if (!minOperand.has_value()) return false;
            std::array<LirOperand, 2> subOps{
                LirOperand::makeReg(discrim64), *minOperand};
            emitInst(*subOp, idxReg, subOps);   // 64-bit (default width)
        }

        // cmp idx, (span-1) ; jcc(Ugt) default, body  — ONE unsigned test at
        // 64-bit. A negative idx (discrim below min) wraps to a huge u64 > span-1;
        // an idx above max is directly > span-1 — one branch covers both. `span-1`
        // is an immediate when it fits [0, 4095]; a wider span (up to 2^20-1) is
        // register-materialized first so the `cmp` stays encodable on arm64.
        {
            std::optional<LirOperand> const spanOperand =
                aluImmOrReg(static_cast<std::int64_t>(spanMinusOne));
            if (!spanOperand.has_value()) return false;
            std::array<LirOperand, 2> cmpOps{
                LirOperand::makeReg(idxReg), *spanOperand};
            emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps);  // 64-bit
        }
        LirBlockId const body        = lir.createBlock();
        LirBlockId const defaultLir  = mirBlockToLirBlock.get(defaultMir);
        {
            std::uint32_t const ugtCond =
                static_cast<std::uint32_t>(TargetCondCode::Ugt);
            std::array<LirOperand, 2> jccOps{
                LirOperand::makeBlockRef(defaultLir.v),
                LirOperand::makeBlockRef(body.v)};
            emitCondBr(*opcode(MnemonicSlot::Jcc), jccOps, defaultLir, body, ugtCond);
        }

        // ── Body block: scale, load the code address, indirect-jump. `idxReg` is
        //    already a full 64-bit non-negative index here (bounded to
        //    [0, span-1]), so no further widen is needed. ────────────────────────
        lir.beginBlock(body);
        LirReg const idx64 = idxReg;

        // off = idx64 * 8  (scale by the 8-byte slot size; the 4-op `lea`'s index
        // is a raw byte offset — scale=1 — on both ISAs, so pre-scale here,
        // exactly as the GEP path pre-scales an array index). Scaling uses `mul`
        // by a materialized-8 register, NOT a shift: the shift opcodes are NOT
        // shape-uniform across ISAs (x86 `shl` takes an imm8 or the implicit CL
        // count; arm64 `shl`/LSL takes a reg count) — `mul reg,reg` is the plain
        // 3-address form BOTH ISAs declare identically, keeping this lowering
        // arch-blind with no capability branch.
        std::optional<LirReg> const eightReg = emitBareConstToFresh(8);
        if (!eightReg.has_value()) return false;
        LirReg const offReg = lir.newVReg(LirRegClass::GPR);
        {
            std::array<LirOperand, 2> mulOps{
                LirOperand::makeReg(idx64), LirOperand::makeReg(*eightReg)};
            emitInst(*mulOp, offReg, mulOps);   // 64-bit multiply (default width)
        }

        // base = lea [jumpTableSymbol]  — 1-op SymbolRef form (like lowerGlobalAddr).
        SymbolId const tableSym = mintJumpTableSymbol();
        LirReg const baseReg = lir.newVReg(LirRegClass::GPR);
        {
            std::array<LirOperand, 1> baseOps{LirOperand::makeSymbolRef(tableSym.v)};
            emitInst(*leaOp, baseReg, baseOps);
        }

        // addr = lea [base + off]  — 4-op indexed form (scale 1, disp 0).
        LirReg const addrReg = lir.newVReg(LirRegClass::GPR);
        {
            std::array<LirOperand, 4> addrOps{
                LirOperand::makeReg(baseReg),
                LirOperand::makeReg(offReg),
                LirOperand::makeMemBase(1),
                LirOperand::makeMemOffset(0)};
            emitInst(*leaOp, addrReg, addrOps);
        }

        // target = load [addr]  — 3-op load form, 64-bit (a code address).
        LirReg const targetReg = lir.newVReg(LirRegClass::GPR);
        {
            std::array<LirOperand, 3> ldOps{
                LirOperand::makeReg(addrReg),
                LirOperand::makeMemBase(1),
                LirOperand::makeMemOffset(0)};
            emitInst(*loadOp, targetReg, ldOps);   // default (64-bit) width
        }

        // jmp *target — successors = every DISTINCT case block + default (the
        // computed-goto contract: all possible destinations are declared so the
        // CFG / DCE / regalloc see them). Dedup by LIR block id.
        std::vector<LirBlockId> indSuccs;
        {
            std::unordered_set<std::uint32_t> seen;
            auto addSucc = [&](LirBlockId b) {
                if (seen.insert(b.v).second) indSuccs.push_back(b);
            };
            for (std::size_t i = 0; i < caseCount; ++i) {
                addSucc(mirBlockToLirBlock.get(succs[i]));
            }
            addSucc(defaultLir);
        }
        {
            std::array<LirOperand, 1> jOps{LirOperand::makeReg(targetReg)};
            emitIndirectBr(*jmpInd, jOps, indSuccs);
        }

        // ── Build the descriptor: one slot per value in [minCase, maxCase]. ────
        JumpTableDescriptor desc;
        desc.tableSymbol = tableSym;
        desc.slotCount   = static_cast<std::size_t>(span);
        desc.funcIndex   = currentFuncIndex_;
        // value → case index (last-writer-wins is irrelevant — case labels are
        // unique per C 6.8.4.2; a duplicate would already be an HIR error).
        std::unordered_map<std::int64_t, std::size_t> valueToCase;
        valueToCase.reserve(caseCount);
        for (std::size_t i = 0; i < caseCount; ++i) valueToCase.emplace(caseValues[i], i);
        desc.slotBindings.reserve(desc.slotCount);
        for (std::size_t j = 0; j < desc.slotCount; ++j) {
            std::int64_t const value = minCase + static_cast<std::int64_t>(j);
            MirBlockId targetMir = defaultMir;
            if (auto it = valueToCase.find(value); it != valueToCase.end()) {
                targetMir = succs[it->second];
            }
            LirBlockId const lirBlock = mirBlockToLirBlock.get(targetMir);
            // Mint (dedup by MIR block) the synthetic per-block symbol.
            SymbolId const blkSym = mintBlockSymbol(targetMir);
            desc.blockSymbols[lirBlock.v] = blkSym;
            desc.slotBindings.emplace_back(lirBlock.v, j);
        }
        jumpTableDescriptors_.push_back(std::move(desc));
        return true;
    }

    // Cascading-compare lowering of MIR Switch. Operands: [discriminant,
    // case_const_0, case_const_1, ...]. Successors: [case_target_0, ...,
    // case_target_{N-1}, default_target].
    //
    // For each case i in [0, N): emit a fresh "compare" LIR block that does
    // `cmp discrim, case_const[i]; jcc(eq) case_target[i], next_compare`.
    // The chain terminates at a block that `jmp default_target`.
    //
    // We've already emitted phi-edge moves for ALL successors at the top of
    // `lowerTerminator`. The cascading-compare path here flows through
    // freshly-created compare blocks; jumping into the case_target preserves
    // the dominance of the phi moves (which were emitted at the END of the
    // ORIGINAL switch-bearing block, before this first jmp).
    bool lowerSwitch(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opcode(MnemonicSlot::Cmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Cmp, "MIR Switch");
            return false;
        }
        if (!opcode(MnemonicSlot::Jmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Jmp, "MIR Switch");
            return false;
        }
        if (!opcode(MnemonicSlot::Jcc).has_value()) {
            reportMissingOpcode(MnemonicSlot::Jcc, "MIR Switch");
            return false;
        }
        auto const operands = mir.instOperands(id);
        if (operands.empty() || succs.size() < 1
            || operands.size() != succs.size()) {
            // Convention: 1 discriminant + N case-constants in operands;
            // N case-targets + 1 default = N+1 successors. So operands.size
            // = N+1 and succs.size = N+1 too.
            reportUnsupported(MirOpcode::Switch, id);
            return false;
        }
        std::optional<LirReg> const discrim = regForValue(operands[0]);
        if (!discrim.has_value()) return false;

        std::size_t const caseCount = operands.size() - 1;
        MirBlockId const defaultMir = succs[caseCount];
        if (!mirBlockToLirBlock.has(defaultMir)) {
            reportUnsupported(MirOpcode::Switch, id);
            return false;
        }

        // D-OPT-SWITCH-JUMP-TABLE (c70): a DENSE switch lowers to an O(1) jump
        // table (bounds check + indexed load of a code address + indirect
        // branch) instead of the O(n) compare chain below. This is BOTH the
        // register-pressure cure (no per-case anything) AND the run-time speed
        // sqlite's per-bytecode VDBE dispatch needs. `tryLowerSwitchJumpTable`
        // returns true iff it took the table path (dense enough + every case a
        // fitting constant + no missing opcode); otherwise the compare chain
        // below runs unchanged (now with immediate compares). Attempted BEFORE
        // the `switchHeader` pin / any compare block is created — the table path
        // emits its FIRST instruction (the bounds-check sub) into the still-open
        // switch-bearing block, preserving the phi-move dominance the compare
        // chain also relies on.
        if (tryLowerSwitchJumpTable(id, operands, succs, caseCount)) {
            return true;
        }

        // Pin the implicit invariant that the FIRST compare AND its
        // paired jcc both land in the switch-bearing block that was
        // open when `lowerSwitch` was called. A future refactor that
        // creates a new block between entry and the first compare —
        // or between the first compare and its jcc — would shift
        // emission into a different block and silently break
        // dominance for the phi moves the cycle-3b prepass emitted on
        // the ORIGINAL switch-bearing block. Two pin sites cover the
        // pre-cmp and the pre-jcc emission boundaries.
        LirBlockId const switchHeader = lir.openBlock();

        // Emit the first compare in-place; subsequent compares go in
        // freshly-allocated "next-compare" blocks that we register on the
        // open function.
        for (std::size_t i = 0; i < caseCount; ++i) {
            if (!mirBlockToLirBlock.has(succs[i])) {
                reportUnsupported(MirOpcode::Switch, id);
                return false;
            }
            // D-OPT-SWITCH-JUMP-TABLE (c70): the case constant is compared as an
            // IMMEDIATE (`cmp discrim, imm32`) rather than materialized into a
            // live register. The pre-c70 shape allocated a vreg per case via
            // `regForValue(operands[1+i])`; for a large switch those N case-const
            // vregs were ALL live across the compare chain (each read at its own
            // cmp) → the regalloc drowned (L_VirtualRegInPostRegalloc, the sqlite
            // ~348-case VDBE switch, 286 spills). An immediate carries no vreg, so
            // the chain's register footprint is O(1). A value that does NOT fit
            // the CONSERVATIVE ARCH-BLIND immediate window [0, 4095] FALLS BACK to
            // register materialization (correct on every target, re-incurring a
            // single vreg for that one case). The window is arm64's `cmp` `imm12`
            // (unsigned 12-bit, 0..4095) — the TIGHTEST `cmp`-immediate reach
            // across the targets (x86 `cmp reg, imm32` is far wider; arm64 has no
            // negative `cmp`-imm form here). Small non-negative case labels (the
            // overwhelming majority — enum/opcode dispatch) take the immediate
            // form; wide or negative labels take the register fallback. Case
            // labels are integer constant expressions (C 6.8.4.2), so
            // `constIntegerValue` folds every one — a non-constant case operand is
            // malformed MIR and fails loud.
            std::optional<std::int64_t> const caseVal =
                constIntegerValue(operands[1 + i]);
            LirOperand caseOperand;
            if (caseVal.has_value() && *caseVal >= 0 && *caseVal <= 4095) {
                caseOperand = LirOperand::makeImmInt32(
                    static_cast<std::int32_t>(*caseVal));
            } else {
                std::optional<LirReg> const caseConst =
                    regForValue(operands[1 + i]);
                if (!caseConst.has_value()) return false;
                caseOperand = LirOperand::makeReg(*caseConst);
            }
            // First-iteration pre-cmp pin: the open block must still
            // be `switchHeader` when the first compare emits.
            if (i == 0 && lir.openBlock() != switchHeader) {
                reportUnsupported(MirOpcode::Switch, id);
                return false;
            }
            std::array<LirOperand, 2> cmpOps{
                LirOperand::makeReg(*discrim), caseOperand};
            // FC3 c2: the cascade compares follow the DISCRIMINANT's
            // type width (D-CSUBSET-32BIT-ALU-FORMS).
            emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps,
                     /*payload=*/0,
                     widthFlagsForType(mir.instType(operands[0])));

            // First-iteration pre-jcc pin: the open block must still
            // be `switchHeader` after the compare emits and before
            // the jcc emits (the jcc is the terminator that seals the
            // header block — anything between cmp and jcc must stay
            // on the header for phi-move dominance).
            if (i == 0 && lir.openBlock() != switchHeader) {
                reportUnsupported(MirOpcode::Switch, id);
                return false;
            }

            // Allocate a "next compare" block unless this is the last
            // case (in which case we fall through to a `jmp default`).
            // The jcc condition is `Eq` (case matches the constant).
            std::uint32_t const eqCond =
                static_cast<std::uint32_t>(TargetCondCode::Eq);
            LirBlockId nextBlock;
            bool const isLastCase = (i + 1 == caseCount);
            LirBlockId const caseTarget = mirBlockToLirBlock.get(succs[i]);
            // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1
            // post-fold, code-reviewer C1): jcc now requires the
            // 2 BlockRef operands as its operand list (matches the
            // schema's `["blockref", "blockref"]` guard). Pre-fold
            // the switch lowering passed empty operands — the
            // encoder would have failed `A_NoMatchingEncodingVariant`
            // on EVERY switch statement; the gap slipped because
            // no runnable corpus example exercises switch yet.
            if (isLastCase) {
                // Final compare: jcc-eq to case target, else jcc fallthrough
                // to a tiny "jmp default" block.
                LirBlockId const defaultJump = lir.createBlock();
                std::array<LirOperand, 2> jccOps{
                    LirOperand::makeBlockRef(caseTarget.v),
                    LirOperand::makeBlockRef(defaultJump.v)};
                emitCondBr(*opcode(MnemonicSlot::Jcc), jccOps,
                              caseTarget, defaultJump, eqCond);
                lir.beginBlock(defaultJump);
                emitBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(defaultMir));
                return true;
            }
            nextBlock = lir.createBlock();
            std::array<LirOperand, 2> jccOps{
                LirOperand::makeBlockRef(caseTarget.v),
                LirOperand::makeBlockRef(nextBlock.v)};
            emitCondBr(*opcode(MnemonicSlot::Jcc), jccOps,
                          caseTarget, nextBlock, eqCond);
            lir.beginBlock(nextBlock);
        }
        // No cases (only default): jmp default.
        emitBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(defaultMir));
        return true;
    }

    bool lowerUnreachable(MirInstId /*id*/) {
        if (!opcode(MnemonicSlot::Unreachable).has_value()) {
            reportMissingOpcode(MnemonicSlot::Unreachable, "MIR Unreachable");
            return false;
        }
        emitUnreachable(*opcode(MnemonicSlot::Unreachable));
        return true;
    }

    // ── per-block / per-function lowering ────────────────────────────

    // Pre-pass over a function's Phi insts: allocate a fresh LirReg for
    // each phi result so predecessor blocks can emit `mov phi_reg, ...`
    // moves at the back-edge BEFORE the phi block is itself lowered.
    void prepassAllocatePhis(MirFuncId mf) {
        std::uint32_t const blockCount = mir.funcBlockCount(mf);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            MirBlockId const mb = mir.funcBlockAt(mf, bi);
            std::uint32_t const n = mir.blockInstCount(mb);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const inst = mir.blockInstAt(mb, i);
                if (mir.instOpcode(inst) != MirOpcode::Phi) continue;
                // Cycle 3d FPR plumbing: phi result class follows the
                // phi's MIR result type. An F64-typed phi (ternary
                // join on doubles, loop-carried float) must use an
                // FPR-class vreg so downstream FPR consumers see the
                // right class.
                LirReg const r = lir.newVReg(regClassFor(inst));
                defineValue(inst, r);
            }
        }
    }

    void lowerBlock(MirBlockId mb) {
        LirBlockId const lb = mirBlockToLirBlock.get(mb);
        lir.beginBlock(lb);
        std::uint32_t const n = mir.blockInstCount(mb);
        if (n == 0) {
            // Empty MIR block — defensive seal so the LIR module finishes.
            if (opcode(MnemonicSlot::Ret).has_value()) {
                emitReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
            }
            return;
        }
        // Lower non-terminator instructions first.
        //
        // Two structural-violation guards run as we iterate:
        //   - terminator in non-last position
        //   - Phi at a non-leading position (Phi must precede every non-
        //     Phi inst by MIR's structural rule; cycle 3b's `lowerInst`
        //     no-ops Phi by design so we must catch mis-positioned Phis
        //     loud here)
        MirInstId const term = mir.blockInstAt(mb, n - 1);
        bool sawNonPhi = false;
        for (std::uint32_t i = 0; i + 1 < n; ++i) {
            MirInstId const inst = mir.blockInstAt(mb, i);
            MirOpcode const op = mir.instOpcode(inst);
            if (isMirTerminator(op)) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR terminator '{}' (inst {}) in non-last position — "
                    "structural violation",
                    mirOpcodeName(op), inst.v);
                reporter.report(std::move(d));
            }
            if (op == MirOpcode::Phi && sawNonPhi) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR Phi (inst {}) follows a non-Phi instruction — "
                    "structural violation (Phis must lead the block)",
                    inst.v);
                reporter.report(std::move(d));
            }
            if (op != MirOpcode::Phi) sawNonPhi = true;
            lowerInst(inst);
        }
        // Then the terminator (which is also responsible for emitting
        // phi-edge moves to all its MIR successors BEFORE the LIR
        // terminator itself).
        bool sealed = false;
        if (isMirTerminator(mir.instOpcode(term))) {
            sealed = lowerTerminator(mb, term);
        } else {
            // Last inst is not a terminator — also malformed MIR.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "MIR block ended with non-terminator opcode '{}' (inst {}) "
                "— structural violation",
                mirOpcodeName(mir.instOpcode(term)), term.v);
            reporter.report(std::move(d));
            // Still lower the non-terminator (so its diagnostics surface)
            lowerInst(term);
        }
        // Fallback seal for blocks whose MIR terminator was deferred to a
        // later cycle. The diagnostic was already issued.
        if (!sealed && opcode(MnemonicSlot::Ret).has_value()) {
            emitReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
        }
    }

    // D-LIR-GLOBALADDR-LOAD-RIPREL-FOLD: per-function MIR value-use
    // census. ONE pass over every block's instructions counting each
    // operand-position read, PLUS every Phi's incoming values (stored
    // outside `instOperands` — see `mirValueUses_`'s member comment
    // for the miscompile this guards). Switch case values are
    // MirInstId refs to Const insts and ride instOperands like the
    // scrutinee — the plain operand walk counts them.
    void computeValueUses(MirFuncId mf) {
        mirValueUses_.clear();
        foldedGlobalAddrs_.clear();
        foldedConstDisps_.clear();
        std::uint32_t const blockCount = mir.funcBlockCount(mf);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            MirBlockId const mb = mir.funcBlockAt(mf, bi);
            std::uint32_t const n = mir.blockInstCount(mb);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const inst = mir.blockInstAt(mb, i);
                // Phi operands live in `phiIncomings` EXCLUSIVELY —
                // `instOperands` on a Phi is a Mir fail-loud.
                if (mir.instOpcode(inst) == MirOpcode::Phi) {
                    for (MirPhiIncoming const& inc : mir.phiIncomings(inst)) {
                        auto& e = mirValueUses_[inc.value.v];
                        ++e.count;
                        e.user = inst;
                    }
                    continue;
                }
                for (auto const& op : mir.instOperands(inst)) {
                    auto& e = mirValueUses_[op.v];
                    ++e.count;
                    e.user = inst;
                }
            }
        }
    }

    void lowerFunction(MirFuncId mf) {
        valueToReg.clear();
        mirBlockToLirBlock.clear();
        // D-CSUBSET-ALLOCA-ADDRESS-REMATERIALIZE (c69): per-function reset so the
        // slot-index counter restarts at 0 for each function — matching the
        // per-function scan callconv runs (`functionLocalAllocaPayloads` is per-fn).
        allocaSlotIndex_.clear();
        allocaLirCount_ = 0;
        computeValueUses(mf);
        lir.addFunction(mir.funcSymbol(mf));

        // Pre-pass 1: pre-allocate LIR blocks (1:1 with MIR blocks).
        std::uint32_t const blockCount = mir.funcBlockCount(mf);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            MirBlockId const mb = mir.funcBlockAt(mf, i);
            LirBlockId const lb = lir.createBlock();
            mirBlockToLirBlock.set(mb, lb);
        }
        // Pre-pass 2: allocate vregs for all Phi results so back-edge
        // predecessor moves resolve cleanly.
        prepassAllocatePhis(mf);

        // Pass 3: lower bodies block-by-block in MIR's declared order.
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            MirBlockId const mb = mir.funcBlockAt(mf, i);
            lowerBlock(mb);
        }
    }

    // Record the LIR inst → source MIR inst mapping. Resizes the
    // `lirToMir` vector so `lirToMir[li.v]` is always the producer.
    void recordSource(LirInstId li) {
        if (!li.valid()) return;
        if (li.v >= lirToMir.size()) lirToMir.resize(li.v + 1);
        lirToMir[li.v] = currentMir;
    }

    // Forwarding wrappers around `LirBuilder` emitters that AUTO-RECORD
    // the source MIR inst (via `currentMir`). Cycle 3e fix-up: every
    // emission site was previously calling `lir.addInst`/`addBr`/etc.
    // directly, leaving `lirToMir` empty so the verifier was forced to
    // walk by position (the architect-flagged Switch hazard). These
    // wrappers + the `currentMir` discipline keep the mapping
    // continuous.
    LirInstId emitInst(std::uint16_t op, LirReg result,
                       std::span<LirOperand const> ops,
                       std::uint32_t payload = 0,
                       std::uint8_t  flags   = 0) {
        LirInstId const li = lir.addInst(op, result, ops, payload, flags);
        recordSource(li);
        return li;
    }
    LirInstId emitBr(std::uint16_t op, LirBlockId target) {
        LirInstId const li = lir.addBr(op, target);
        recordSource(li);
        return li;
    }
    LirInstId emitCondBr(std::uint16_t op, std::span<LirOperand const> ops,
                         LirBlockId ifT, LirBlockId ifF,
                         std::uint32_t payload = 0) {
        LirInstId const li = lir.addCondBr(op, ops, ifT, ifF, payload);
        recordSource(li);
        return li;
    }
    LirInstId emitReturn(std::uint16_t op, std::span<LirOperand const> ops) {
        LirInstId const li = lir.addReturn(op, ops);
        recordSource(li);
        return li;
    }
    LirInstId emitIndirectBr(std::uint16_t op, std::span<LirOperand const> ops,
                             std::span<LirBlockId const> targets) {
        LirInstId const li = lir.addIndirectBr(op, ops, targets);
        recordSource(li);
        return li;
    }
    LirInstId emitUnreachable(std::uint16_t op) {
        LirInstId const li = lir.addUnreachable(op);
        recordSource(li);
        return li;
    }

    [[nodiscard]] MirToLirResult run() {
        std::size_t const fnCount = mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < fnCount; ++i) {
            // D-OPT-SWITCH-JUMP-TABLE (c70): `lir.addFunction` runs in this same
            // order inside `lowerFunction`, so the LIR `funcAt(i)` index equals
            // this loop index — record it for any jump-table descriptor emitted
            // while lowering this function.
            currentFuncIndex_ = i;
            lowerFunction(mir.funcAt(i));
        }
        Lir frozen = std::move(lir).finish();
        // Ensure lirToMir spans every LIR inst slot (any trailing
        // slots without recorded sources default to InvalidMirInst).
        if (lirToMir.size() < frozen.nodeCount()) {
            lirToMir.resize(frozen.nodeCount());
        }
        // Designated initializers prevent a future field reorder
        // from silently rebinding the positional `{}` for
        // `externImports`. (code-simplifier + code-reviewer fold,
        // LK6 cycle 2d post-fold review.)
        return MirToLirResult{
            .lir                  = std::move(frozen),
            .lirToMir             = std::move(lirToMir),
            .jumpTableDescriptors = std::move(jumpTableDescriptors_),
            .signMaskConstants    = std::move(signMaskConstants_),
            .externImports        = {},
            .ok                   = !hadError()
        };
    }
};

} // namespace

MirToLirResult lowerToLir(Mir const&          mir,
                          TargetSchema const& target,
                          TypeInterner const& interner,
                          DiagnosticReporter& reporter,
                          std::vector<ExternImport> externImports,
                          std::optional<ExternCallDispatch> externCallDispatch) {
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02): pass
    // the externImports vector to the Lowerer so it can distinguish
    // extern-targeting calls from module-internal direct calls.
    // D-FFI-EXTERN-CALL-DISPATCH: also pass the active format's
    // extern-call shape — `lowerCall` selects `call_indirect_via_extern`
    // (indirect-slot: PE/Mach-O IAT/__got) vs the plain `call`
    // (direct-plt: ELF PLT stub) from it. nullopt + extern imports =
    // fail-loud (no silent default to either shape).
    Lowerer L{mir, target, interner, reporter, externImports,
              externCallDispatch};
    MirToLirResult result = std::move(L).run();
    // Append (not overwrite) so any future LIR-tier extern synthesis
    // — e.g. runtime-helper imports like `__chkstk` / `__divti3` /
    // decimal-runtime hooks the lowerer may need to materialize —
    // is not silently clobbered. The inner `run()` initializes
    // `result.externImports` to `{}` today (no LIR-side synthesis
    // yet); if/when a future cycle starts emitting externs from
    // the LIR lowerer, those rows will compose with the caller-
    // supplied vector instead of being dropped. (silent-failure
    // MEDIUM fold, LK6 cycle 2d post-fold review.)
    result.externImports.insert(result.externImports.end(),
                                std::make_move_iterator(externImports.begin()),
                                std::make_move_iterator(externImports.end()));
    return result;
}

} // namespace dss
