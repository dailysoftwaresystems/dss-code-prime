#include "lir/lowering/mir_to_lir.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_opcode.hpp"

#include <array>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
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
        || op == MirOpcode::Unreachable;
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
    Cmp, Setcc, Jmp, Jcc,
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
    Count_
};
constexpr std::size_t kMnemonicCount = static_cast<std::size_t>(MnemonicSlot::Count_);

struct MnemonicCache {
    std::string_view             mnemonic;
    std::optional<std::uint16_t> id;
    bool                         missingReported = false;
};

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

    // Per-function state. Cleared at the top of `lowerFunction`.
    MirAttribute<LirReg>            valueToReg;
    MirBlockAttribute<LirBlockId>   mirBlockToLirBlock;

    // Whether the running lowering pass added any error-severity
    // diagnostics. Mirrors ML2's delta-on-errorCount; reset by the ctor.
    std::uint32_t baselineErrors = 0;
    bool hadError() const noexcept {
        return reporter.errorCount() != baselineErrors;
    }

    Lowerer(Mir const& m, TargetSchema const& t, TypeInterner const& i,
            DiagnosticReporter& r)
        : mir(m), target(t), interner(i), reporter(r), lir(t),
          valueToReg(m), mirBlockToLirBlock(m.blockArena()) {
        baselineErrors = reporter.errorCount();
        // Mnemonics in MnemonicSlot enum-declaration order. The
        // `static_assert` below closes the silent-drift hazard 4
        // review agents converged on: if a developer adds a slot to
        // `MnemonicSlot` but forgets to add its mnemonic here (or
        // vice-versa), compilation fails LOUD rather than silently
        // zero-filling the array and returning `nullopt` from every
        // `opcodeByMnemonic("")` lookup. Each slot is positionally
        // indexed; a single off-by-one would corrupt all subsequent
        // opcode lookups.
        constexpr std::array<std::string_view, kMnemonicCount> kMnemonics{{
            "arg", "mov", "add", "sub", "mul",
            "cmp", "setcc", "jmp", "jcc",
            "ret", "unreachable",
            "alloca", "load", "store", "lea",
            "sext", "zext", "trunc",
            // cycle 3d bitwise + Neg
            "and", "or", "xor", "shl", "shr_l", "shr_a", "not", "neg",
            // cycle 3d float arithmetic + casts
            "fadd", "fsub", "fmul", "fdiv", "fneg",
            "fpcvt", "fp_to_si", "fp_to_ui", "si_to_fp", "ui_to_fp",
            // cycle 3d cross-class move
            "movq_xclass",
        }};
        static_assert(kMnemonics.size() == kMnemonicCount,
            "MnemonicSlot enum and kMnemonics array drifted — every slot "
            "is positionally indexed; a missing or extra row would corrupt "
            "every subsequent opcode lookup silently. Keep the two in "
            "lockstep when adding opcodes.");
        for (std::size_t i = 0; i < kMnemonicCount; ++i) {
            cache_[i].mnemonic = kMnemonics[i];
            cache_[i].id       = target.opcodeByMnemonic(kMnemonics[i]);
        }
    }

    [[nodiscard]] std::optional<std::uint16_t> opcode(MnemonicSlot s) const {
        return cache_[static_cast<std::size_t>(s)].id;
    }

    // Map a MIR `TypeId` to the LIR register class that holds its
    // values. Float types (F32/F64) flow through FPR; integer/pointer
    // types flow through GPR; bool also stays in GPR (0/1 byte). The
    // map is intentionally simple — extension types fail-loud at the
    // call site rather than silently picking a class.
    [[nodiscard]] LirRegClass regClassForType(TypeId ty) const {
        if (!ty.valid()) return LirRegClass::GPR;
        switch (interner.kind(ty)) {
            case TypeKind::F32:
            case TypeKind::F64:
            case TypeKind::F16:
            case TypeKind::F128:
                return LirRegClass::FPR;
            default:
                return LirRegClass::GPR;
        }
    }

    [[nodiscard]] LirRegClass regClassFor(MirInstId id) const {
        return regClassForType(mir.instType(id));
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
        MirOpcode const op = mir.instOpcode(id);
        switch (op) {
            case MirOpcode::Arg:    return lowerArg(id);
            case MirOpcode::Const:  return lowerConst(id);
            case MirOpcode::Add:    return lowerBinaryOp(id, MnemonicSlot::Add);
            case MirOpcode::Sub:    return lowerBinaryOp(id, MnemonicSlot::Sub);
            case MirOpcode::Mul:    return lowerBinaryOp(id, MnemonicSlot::Mul);
            case MirOpcode::ICmpEq: case MirOpcode::ICmpNe:
            case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
            case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge:
            case MirOpcode::ICmpUlt: case MirOpcode::ICmpUle:
            case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge:
                return lowerICmp(id, *condCodeForICmp(op));
            case MirOpcode::Phi:    return;  // pre-pass-allocated; no body emission
            case MirOpcode::Alloca: return lowerAlloca(id);
            case MirOpcode::Load:   return lowerLoad(id);
            case MirOpcode::Store:  return lowerStore(id);
            case MirOpcode::Gep:    return lowerGep(id);
            case MirOpcode::Trunc:  return lowerCast(id, MnemonicSlot::Trunc, "MIR Trunc");
            case MirOpcode::SExt:   return lowerCast(id, MnemonicSlot::SExt,  "MIR SExt");
            case MirOpcode::ZExt:   return lowerCast(id, MnemonicSlot::ZExt,  "MIR ZExt");
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
            case MirOpcode::Shl:    return lowerBinaryOp(id, MnemonicSlot::Shl);
            case MirOpcode::LShr:   return lowerBinaryOp(id, MnemonicSlot::ShrL);
            case MirOpcode::AShr:   return lowerBinaryOp(id, MnemonicSlot::ShrA);
            case MirOpcode::Not:    return lowerUnaryOp(id, MnemonicSlot::Not);
            case MirOpcode::Neg:    return lowerUnaryOp(id, MnemonicSlot::Neg);
            // ── cycle 3d float arithmetic (FPR-class result) ───────
            case MirOpcode::FAdd:   return lowerBinaryOp(id, MnemonicSlot::FAdd);
            case MirOpcode::FSub:   return lowerBinaryOp(id, MnemonicSlot::FSub);
            case MirOpcode::FMul:   return lowerBinaryOp(id, MnemonicSlot::FMul);
            case MirOpcode::FDiv:   return lowerBinaryOp(id, MnemonicSlot::FDiv);
            case MirOpcode::FNeg:   return lowerUnaryOp(id, MnemonicSlot::FNeg);
            // ── cycle 3d float casts (the 6 conversion variants) ───
            case MirOpcode::FPTrunc: return lowerCast(id, MnemonicSlot::FpCvt,   "MIR FPTrunc");
            case MirOpcode::FPExt:   return lowerCast(id, MnemonicSlot::FpCvt,   "MIR FPExt");
            case MirOpcode::FPToSI:  return lowerCast(id, MnemonicSlot::FpToSi,  "MIR FPToSI");
            case MirOpcode::FPToUI:  return lowerCast(id, MnemonicSlot::FpToUi,  "MIR FPToUI");
            case MirOpcode::SIToFP:  return lowerCast(id, MnemonicSlot::SiToFp,  "MIR SIToFP");
            case MirOpcode::UIToFP:  return lowerCast(id, MnemonicSlot::UiToFp,  "MIR UIToFP");
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
        lir.addInst(*opcode(MnemonicSlot::Arg), result, std::span<LirOperand const>{},
                    /*payload=*/mir.argIndex(id));
        defineValue(id, result);
    }

    // Emit a single-operand `mov` and define the result vreg. Shared
    // tail of `lowerConst`'s narrow + wide paths (the only difference
    // between them is the operand-kind in `op`). Returns the result reg
    // so callers can chain.
    LirReg emitMovToFresh(MirInstId id, LirOperand op, LirRegClass cls) {
        LirReg const result = lir.newVReg(cls);
        std::array<LirOperand, 1> ops{op};
        lir.addInst(*opcode(MnemonicSlot::Mov), result, ops);
        defineValue(id, result);
        return result;
    }

    // Define a poisoned-value vreg + return false so the caller can
    // signal "lowering failed but downstream uses get a quiet
    // placeholder, not a cascade of `regForValue` diagnostics". The
    // diagnostic for the ROOT cause is the caller's responsibility.
    void poisonValue(MirInstId id) {
        LirReg const placeholder = lir.newVReg(LirRegClass::GPR);
        valueToReg.set(id, placeholder);
    }

    void lowerConst(MirInstId id) {
        if (!opcode(MnemonicSlot::Mov).has_value()) {
            reportMissingOpcode(MnemonicSlot::Mov, "MIR Const");
            return;
        }
        std::uint32_t const litIdx = mir.constLiteralIndex(id);
        MirLiteralValue const& lit = mir.literalValue(litIdx);

        // ── narrow integer path: inline ImmInt32 ──
        std::int32_t imm32 = 0;
        bool fits = false;
        if (auto const* i = std::get_if<std::int64_t>(&lit.value)) {
            if (*i >= std::numeric_limits<std::int32_t>::min()
             && *i <= std::numeric_limits<std::int32_t>::max()) {
                imm32 = static_cast<std::int32_t>(*i);
                fits  = true;
            }
        } else if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) {
            if (*u <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                imm32 = static_cast<std::int32_t>(*u);
                fits  = true;
            }
        } else if (auto const* b = std::get_if<bool>(&lit.value)) {
            imm32 = *b ? 1 : 0;
            fits  = true;
        }
        if (fits) {
            // Narrow integer literal — GPR-class regardless of the
            // declared MIR type since the immediate IS an integer.
            // (Bool literals are also routed here as 0/1.)
            emitMovToFresh(id, LirOperand::makeImmInt32(imm32), LirRegClass::GPR);
            return;
        }

        // ── wide-literal path: route through LirLiteralPool ──
        //
        // Routing: int32-fits → ImmInt (above); int64/uint64/string →
        // pool with GPR-class result; double → pool with FPR-class
        // result (cycle 3d enabled FPR consumption + the cross-class
        // movq path for downstream Bitcast); monostate / aggregate →
        // unsupported + poison so dependent uses surface ONE root-cause
        // diagnostic rather than a cascade of "used before definition".
        LirLiteralValue lirLit;
        lirLit.core = lit.core;
        LirRegClass resultCls = regClassFor(id);
        bool unsupportedVariant = false;
        if (auto const* i = std::get_if<std::int64_t>(&lit.value))       lirLit.value = *i;
        else if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) lirLit.value = *u;
        else if (auto const* d = std::get_if<double>(&lit.value))        lirLit.value = *d;
        else if (auto const* s = std::get_if<std::string>(&lit.value))   lirLit.value = *s;
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
        lir.addInst(*opcode(MnemonicSlot::Alloca), result,
                    std::span<LirOperand const>{}, payload);
        defineValue(id, result);
    }

    void lowerLoad(MirInstId id) {
        if (!opcode(MnemonicSlot::Load).has_value()) {
            reportMissingOpcode(MnemonicSlot::Load, "MIR Load");
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) { reportUnsupported(MirOpcode::Load, id); return; }
        std::optional<LirReg> const base = regForValue(operands[0]);
        if (!base.has_value()) return;
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 3> ops{
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        lir.addInst(*opcode(MnemonicSlot::Load), result, ops);
        defineValue(id, result);
    }

    void lowerStore(MirInstId id) {
        if (!opcode(MnemonicSlot::Store).has_value()) {
            reportMissingOpcode(MnemonicSlot::Store, "MIR Store");
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) { reportUnsupported(MirOpcode::Store, id); return; }
        std::optional<LirReg> const value = regForValue(operands[0]);
        std::optional<LirReg> const base  = regForValue(operands[1]);
        if (!value.has_value() || !base.has_value()) return;
        std::array<LirOperand, 4> ops{
            LirOperand::makeReg(*value),
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        lir.addInst(*opcode(MnemonicSlot::Store), InvalidLirReg, ops);
    }

    void lowerGep(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.empty()) { reportUnsupported(MirOpcode::Gep, id); return; }
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
        // The scale defaults to 1 for cycle 3d; element-size-driven
        // scales (sizeof(T) for typed-array Gep) co-design with ML6's
        // address-mode synthesis. Multi-index Gep — multiple
        // dereferences — folds into a chain at the optimizer layer.
        if (operands.size() == 2) {
            if (!opcode(MnemonicSlot::Lea).has_value()) {
                reportMissingOpcode(MnemonicSlot::Lea, "MIR Gep (dynamic-index)");
                return;
            }
            std::optional<LirReg> const index = regForValue(operands[1]);
            if (!index.has_value()) return;
            LirReg const result = lir.newVReg(LirRegClass::GPR);
            std::array<LirOperand, 4> ops{
                LirOperand::makeReg(*base),
                LirOperand::makeReg(*index),
                LirOperand::makeMemBase(1),
                LirOperand::makeMemOffset(0),
            };
            lir.addInst(*opcode(MnemonicSlot::Lea), result, ops);
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
    // UiToFp) all land here.
    void lowerCast(MirInstId id, MnemonicSlot slot, std::string_view context) {
        if (!opcode(slot).has_value()) {
            reportMissingOpcode(slot, context);
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::optional<LirReg> const src = regForValue(operands[0]);
        if (!src.has_value()) return;
        LirReg const result = lir.newVReg(regClassFor(id));
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*src)};
        lir.addInst(*opcode(slot), result, ops);
        defineValue(id, result);
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
        MnemonicSlot const slot =
            (srcCls == dstCls) ? MnemonicSlot::Mov : MnemonicSlot::MovqXClass;
        char const* const ctx =
            (srcCls == dstCls) ? "MIR Bitcast (same-class)" : "MIR Bitcast (cross-class)";
        if (!opcode(slot).has_value()) {
            reportMissingOpcode(slot, ctx);
            return;
        }
        LirReg const result = lir.newVReg(dstCls);
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*src)};
        lir.addInst(*opcode(slot), result, ops);
        defineValue(id, result);
    }

    void lowerBinaryOp(MirInstId id, MnemonicSlot slot) {
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
        std::optional<LirReg> const a = regForValue(operands[0]);
        std::optional<LirReg> const b = regForValue(operands[1]);
        if (!a.has_value() || !b.has_value()) return;
        // Cycle 3d: result reg class follows the MIR result type. F32/F64
        // → FPR (float arithmetic); everything else → GPR.
        LirReg const result = lir.newVReg(regClassFor(id));
        std::array<LirOperand, 2> ops{LirOperand::makeReg(*a), LirOperand::makeReg(*b)};
        lir.addInst(*op, result, ops);
        defineValue(id, result);
    }

    // Single-operand value-producing op (`not`, `neg`, `fneg`). Result
    // reg class follows the MIR result type.
    void lowerUnaryOp(MirInstId id, MnemonicSlot slot) {
        auto const op = opcode(slot);
        if (!op.has_value()) {
            reportMissingOpcode(slot, mirOpcodeName(mir.instOpcode(id)));
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::optional<LirReg> const a = regForValue(operands[0]);
        if (!a.has_value()) return;
        LirReg const result = lir.newVReg(regClassFor(id));
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*a)};
        lir.addInst(*op, result, ops);
        defineValue(id, result);
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
        std::array<LirOperand, 2> cmpOps{LirOperand::makeReg(*a), LirOperand::makeReg(*b)};
        lir.addInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps);

        LirReg const result = lir.newVReg(LirRegClass::GPR);
        lir.addInst(*opcode(MnemonicSlot::Setcc), result, std::span<LirOperand const>{},
                    /*payload=*/static_cast<std::uint32_t>(cond));
        defineValue(id, result);
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
        if (!opcode(MnemonicSlot::Mov).has_value()) {
            reportMissingOpcode(MnemonicSlot::Mov, "MIR Phi edge");
            return;
        }
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
        std::vector<LirReg> temps;
        temps.reserve(pairs.size());
        for (auto const& p : pairs) {
            LirReg const tmp = lir.newVReg(LirRegClass::GPR);
            std::array<LirOperand, 1> ops{LirOperand::makeReg(p.incoming)};
            lir.addInst(*opcode(MnemonicSlot::Mov), tmp, ops);
            temps.push_back(tmp);
        }
        // Step 2: write each phi-result from its captured temp.
        for (std::size_t k = 0; k < pairs.size(); ++k) {
            std::array<LirOperand, 1> ops{LirOperand::makeReg(temps[k])};
            lir.addInst(*opcode(MnemonicSlot::Mov), pairs[k].phi, ops);
        }
    }

    // Returns true iff the terminator was emitted (block sealed). `false`
    // signals the caller to use the fallback seal.
    bool lowerTerminator(MirBlockId mb, MirInstId termId) {
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
            lir.addReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
            return true;
        }
        if (operands.size() != 1) {
            reportUnsupported(MirOpcode::Return, id);
            return false;
        }
        std::optional<LirReg> const v = regForValue(operands[0]);
        if (!v.has_value()) return false;
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*v)};
        lir.addReturn(*opcode(MnemonicSlot::Ret), ops);
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
        lir.addBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(succs[0]));
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
        std::optional<LirReg> const cond = regForValue(operands[0]);
        if (!cond.has_value()) return false;
        if (!mirBlockToLirBlock.has(succs[0]) || !mirBlockToLirBlock.has(succs[1])) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = "MIR CondBr target block(s) have no LIR block mapping";
            reporter.report(std::move(d));
            return false;
        }
        // Naive: compare the Bool value against 0, jcc-Ne taken when true.
        // Optimizer fuses adjacent ICmp + CondBr to a single cmp/jcc-cond.
        // The jcc condition is written into the inst's `payload` field via
        // the cycle-3b-extended `addCondBr(payload=...)` overload — closes
        // the cycle-3b-review HIGH finding where the payload silently
        // defaulted to 0 (= TargetCondCode::Eq) and inverted the branch.
        std::array<LirOperand, 2> cmpOps{
            LirOperand::makeReg(*cond), LirOperand::makeImmInt32(0)};
        lir.addInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps);
        lir.addCondBr(*opcode(MnemonicSlot::Jcc), std::span<LirOperand const>{},
                      mirBlockToLirBlock.get(succs[0]),
                      mirBlockToLirBlock.get(succs[1]),
                      static_cast<std::uint32_t>(TargetCondCode::Ne));
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

        // Emit the first compare in-place; subsequent compares go in
        // freshly-allocated "next-compare" blocks that we register on the
        // open function.
        for (std::size_t i = 0; i < caseCount; ++i) {
            std::optional<LirReg> const caseConst = regForValue(operands[1 + i]);
            if (!caseConst.has_value()) return false;
            if (!mirBlockToLirBlock.has(succs[i])) {
                reportUnsupported(MirOpcode::Switch, id);
                return false;
            }
            std::array<LirOperand, 2> cmpOps{
                LirOperand::makeReg(*discrim), LirOperand::makeReg(*caseConst)};
            lir.addInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps);

            // Allocate a "next compare" block unless this is the last
            // case (in which case we fall through to a `jmp default`).
            // The jcc condition is `Eq` (case matches the constant).
            std::uint32_t const eqCond =
                static_cast<std::uint32_t>(TargetCondCode::Eq);
            LirBlockId nextBlock;
            bool const isLastCase = (i + 1 == caseCount);
            LirBlockId const caseTarget = mirBlockToLirBlock.get(succs[i]);
            if (isLastCase) {
                // Final compare: jcc-eq to case target, else jcc fallthrough
                // to a tiny "jmp default" block.
                LirBlockId const defaultJump = lir.createBlock();
                lir.addCondBr(*opcode(MnemonicSlot::Jcc), std::span<LirOperand const>{},
                              caseTarget, defaultJump, eqCond);
                lir.beginBlock(defaultJump);
                lir.addBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(defaultMir));
                return true;
            }
            nextBlock = lir.createBlock();
            lir.addCondBr(*opcode(MnemonicSlot::Jcc), std::span<LirOperand const>{},
                          caseTarget, nextBlock, eqCond);
            lir.beginBlock(nextBlock);
        }
        // No cases (only default): jmp default.
        lir.addBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(defaultMir));
        return true;
    }

    bool lowerUnreachable(MirInstId /*id*/) {
        if (!opcode(MnemonicSlot::Unreachable).has_value()) {
            reportMissingOpcode(MnemonicSlot::Unreachable, "MIR Unreachable");
            return false;
        }
        lir.addUnreachable(*opcode(MnemonicSlot::Unreachable));
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
                LirReg const r = lir.newVReg(LirRegClass::GPR);
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
                lir.addReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
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
            lir.addReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
        }
    }

    void lowerFunction(MirFuncId mf) {
        valueToReg.clear();
        mirBlockToLirBlock.clear();
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

    [[nodiscard]] MirToLirResult run() {
        std::size_t const fnCount = mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < fnCount; ++i) {
            lowerFunction(mir.funcAt(i));
        }
        return MirToLirResult{ std::move(lir).finish(), !hadError() };
    }
};

} // namespace

MirToLirResult lowerToLir(Mir const&          mir,
                          TargetSchema const& target,
                          TypeInterner const& interner,
                          DiagnosticReporter& reporter) {
    Lowerer L{mir, target, interner, reporter};
    return std::move(L).run();
}

} // namespace dss
