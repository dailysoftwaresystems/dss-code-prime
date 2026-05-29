#include "lir/lowering/mir_to_lir.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_opcode.hpp"

#include <array>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>
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
        case MirOpcode::Return:        return "Return";
        default:                       return "<deferred>";
    }
}

// One transient lowerer per `lowerToLir` call. Holds the per-pass state
// the dispatcher methods read/write. Mirrors `Lowerer` in `hir_to_mir.cpp`
// — same single-struct-with-methods style.
struct Lowerer {
    Mir const&          mir;
    TargetSchema const& target;
    DiagnosticReporter& reporter;
    LirBuilder          lir;

    // Cached opcode mnemonics → numeric opcode indexes. Looked up once per
    // module, not per instruction. `std::nullopt` when the target schema
    // does not declare that mnemonic — the lowerer reports
    // `L_RequiredLirOpcodeMissing` once and skips affected instructions.
    std::optional<std::uint16_t> opArg;
    std::optional<std::uint16_t> opMov;
    std::optional<std::uint16_t> opAdd;
    std::optional<std::uint16_t> opSub;
    std::optional<std::uint16_t> opRet;

    // Per-function state, reset by `lowerFunction`.
    // MIR value (== MirInstId) → its result LirReg.
    std::unordered_map<std::uint32_t, LirReg> valueToReg;
    // MIR block → pre-created LIR block (pre-pass; lets forward branches
    // target a not-yet-emitted block once cycle 3b lands CFG).
    std::unordered_map<std::uint32_t, LirBlockId> mirBlockToLirBlock;

    // Whether the running lowering pass added any error-severity diagnostics.
    // Mirrors ML2's delta-on-errorCount; reset before each `lowerToLir` call.
    std::uint32_t baselineErrors = 0;
    bool hadError() const noexcept {
        return reporter.errorCount() != baselineErrors;
    }

    Lowerer(Mir const& m, TargetSchema const& t, DiagnosticReporter& r)
        : mir(m), target(t), reporter(r), lir(t) {
        baselineErrors = reporter.errorCount();
        opArg = target.opcodeByMnemonic("arg");
        opMov = target.opcodeByMnemonic("mov");
        opAdd = target.opcodeByMnemonic("add");
        opSub = target.opcodeByMnemonic("sub");
        opRet = target.opcodeByMnemonic("ret");
    }

    // Issue a `L_RequiredLirOpcodeMissing` once when a foundational mnemonic
    // is absent from the target schema. Used by the per-opcode lowering
    // methods before they reach for an `std::optional<uint16_t>` that's
    // nullopt.
    void reportMissingOpcode(std::string_view mnemonic, std::string_view context) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_RequiredLirOpcodeMissing;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "target '{}' declares no '{}' opcode (required for lowering {})",
            target.name(), mnemonic, context);
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

    // Look up the LirReg for a MIR value (== MirInstId). The dispatch order
    // of `lowerInst` ensures every value is mapped before its first use, so
    // a missing entry is a structural failure — abort loudly rather than
    // silently produce InvalidLirReg.
    [[nodiscard]] LirReg regForValue(MirInstId v) {
        auto it = valueToReg.find(v.v);
        if (it == valueToReg.end()) {
            // Out-of-order use: defensive seal — emit a fresh GPR vreg and
            // record it, AND emit a diagnostic so the failure isn't silent.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "MIR value %{} used before definition during LIR lowering",
                v.v);
            reporter.report(std::move(d));
            LirReg const placeholder = lir.newVReg(LirRegClass::GPR);
            valueToReg.emplace(v.v, placeholder);
            return placeholder;
        }
        return it->second;
    }

    // Build an operand pool entry referencing a vreg.
    [[nodiscard]] static LirOperand regOperand(LirReg r) noexcept {
        LirOperand o{};
        o.kind = LirOperandKind::Reg;
        o.reg  = r;
        return o;
    }

    // Build an operand pool entry holding an int32 immediate. (Wider
    // immediates need a literal-pool encoding cycle 3 doesn't ship yet —
    // the `LirOperand` POD's union arm `immInt32` is the cycle-3a shape.)
    [[nodiscard]] static LirOperand immInt32Operand(std::int32_t v) noexcept {
        LirOperand o{};
        o.kind     = LirOperandKind::ImmInt;
        o.immInt32 = v;
        return o;
    }

    // ── per-instruction lowering ──────────────────────────────────────

    void lowerInst(MirInstId id) {
        MirOpcode const op = mir.instOpcode(id);
        switch (op) {
            case MirOpcode::Arg:    return lowerArg(id);
            case MirOpcode::Const:  return lowerConst(id);
            case MirOpcode::Add:    return lowerBinaryOp(id, "add", opAdd);
            case MirOpcode::Sub:    return lowerBinaryOp(id, "sub", opSub);
            case MirOpcode::Return: return lowerReturn(id);
            default: break;
        }
        reportUnsupported(op, id);
    }

    void lowerArg(MirInstId id) {
        if (!opArg.has_value()) {
            reportMissingOpcode("arg", "MIR Arg");
            return;
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        lir.addInst(*opArg, result, std::span<LirOperand const>{},
                    /*payload=*/mir.argIndex(id));
        valueToReg.emplace(id.v, result);
    }

    void lowerConst(MirInstId id) {
        if (!opMov.has_value()) {
            reportMissingOpcode("mov", "MIR Const");
            return;
        }
        // Cycle 3a: only integer literals that fit in int32 lower cleanly.
        // Larger / non-integer literals defer to cycle 3b's literal-pool
        // wiring (movabsq on x86_64).
        std::uint32_t const litIdx = mir.constLiteralIndex(id);
        MirLiteralValue const& lit = mir.literalValue(litIdx);
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
        if (!fits) {
            reportUnsupported(MirOpcode::Const, id);
            return;
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 1> ops{immInt32Operand(imm32)};
        lir.addInst(*opMov, result, ops);
        valueToReg.emplace(id.v, result);
    }

    void lowerBinaryOp(MirInstId id, std::string_view mnemonic,
                       std::optional<std::uint16_t> opcode) {
        if (!opcode.has_value()) {
            reportMissingOpcode(mnemonic, mirOpcodeName(mir.instOpcode(id)));
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        LirReg const a = regForValue(operands[0]);
        LirReg const b = regForValue(operands[1]);
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 2> ops{regOperand(a), regOperand(b)};
        lir.addInst(*opcode, result, ops);
        valueToReg.emplace(id.v, result);
    }

    void lowerReturn(MirInstId id) {
        if (!opRet.has_value()) {
            reportMissingOpcode("ret", "MIR Return");
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.empty()) {
            lir.addReturn(*opRet, std::span<LirOperand const>{});
            return;
        }
        // MIR Return with a single value operand.
        LirReg const v = regForValue(operands[0]);
        std::array<LirOperand, 1> ops{regOperand(v)};
        lir.addReturn(*opRet, ops);
    }

    // ── per-block / per-function lowering ─────────────────────────────

    void lowerBlock(MirBlockId mb) {
        LirBlockId const lb = mirBlockToLirBlock.at(mb.v);
        lir.beginBlock(lb);
        std::uint32_t const n = mir.blockInstCount(mb);
        bool sealed = false;
        for (std::uint32_t i = 0; i < n; ++i) {
            MirInstId const inst = mir.blockInstAt(mb, i);
            MirOpcode const op = mir.instOpcode(inst);
            lowerInst(inst);
            // Only MirOpcode::Return reaches `LirBuilder::addReturn` in
            // cycle 3a. Other MIR terminators (Br/CondBr/Switch/Unreachable)
            // hit the fail-loud-deferral path in `lowerInst` and leave the
            // LIR block unsealed.
            if (op == MirOpcode::Return) sealed = true;
        }
        // Fallback seal: cycle 3a's `lowerInst` does not yet emit terminators
        // for Br/CondBr/Switch/Unreachable; without a fallback the
        // `LirBuilder::finish()` open-block guard would abort the process
        // even though the unsupported-opcode diagnostic was already issued.
        // Same fail-loud-deferral discipline as ML2 cycle 1 (which sealed
        // unsupported-shape blocks with MIR's `addUnreachable`).
        if (!sealed && opRet.has_value()) {
            lir.addReturn(*opRet, std::span<LirOperand const>{});
        }
    }

    void lowerFunction(MirFuncId mf) {
        valueToReg.clear();
        mirBlockToLirBlock.clear();
        lir.addFunction(mir.funcSymbol(mf));

        // Pre-pass: create one LIR block per MIR block so forward branches
        // (cycle 3b+) can target them.
        std::uint32_t const blockCount = mir.funcBlockCount(mf);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            MirBlockId const mb = mir.funcBlockAt(mf, i);
            LirBlockId const lb = lir.createBlock();
            mirBlockToLirBlock.emplace(mb.v, lb);
        }
        // Lower bodies block-by-block in MIR's declared order.
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
                          DiagnosticReporter& reporter) {
    Lowerer L{mir, target, reporter};
    return std::move(L).run();
}

} // namespace dss
