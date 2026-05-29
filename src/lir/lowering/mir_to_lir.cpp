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
    // does not declare that mnemonic — `reportMissingOpcode` fires ONCE
    // per missing mnemonic (the `missingReported` bitset gates re-emit).
    std::optional<std::uint16_t> opArg;
    std::optional<std::uint16_t> opMov;
    std::optional<std::uint16_t> opAdd;
    std::optional<std::uint16_t> opSub;
    std::optional<std::uint16_t> opMul;
    std::optional<std::uint16_t> opRet;
    // Index parallel to the optionals above, one bit per cached mnemonic.
    // Used by `reportMissingOpcode` to suppress per-instruction spam — a
    // 10k-`Add`-with-no-`add`-opcode module emits ONE diagnostic, not 10k.
    enum MnemonicSlot : std::uint8_t { SlotArg, SlotMov, SlotAdd, SlotSub,
                                       SlotMul, SlotRet, SlotCount };
    std::array<bool, SlotCount> missingReported{};

    // Per-function state, reset by `lowerFunction`.
    // MIR value (== MirInstId) → its result LirReg. Keyed on the raw `.v`
    // to match `hir_to_mir.cpp`'s `symbolToValue` convention; a future
    // substrate-wide switch to typed-id keys lifts both at once.
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
        opMul = target.opcodeByMnemonic("mul");
        opRet = target.opcodeByMnemonic("ret");
    }

    // Issue a `L_RequiredLirOpcodeMissing` once when a foundational mnemonic
    // is absent from the target schema. Per-mnemonic flag suppresses spam
    // (the lowerer would otherwise emit one per instruction).
    void reportMissingOpcode(MnemonicSlot slot, std::string_view mnemonic,
                             std::string_view context) {
        if (missingReported[slot]) return;
        missingReported[slot] = true;
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

    // Look up the LirReg for a MIR value (== MirInstId). The dispatch order
    // of `lowerInst` ensures every value is mapped before its first use, so
    // a missing entry is a structural failure — `regForValue` returns
    // `nullopt` so the CALLER bails out (rather than emitting an LIR inst
    // that reads an uninitialised vreg). One diagnostic per missing source.
    [[nodiscard]] std::optional<LirReg> regForValue(MirInstId v) {
        auto it = valueToReg.find(v.v);
        if (it == valueToReg.end()) {
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
        return it->second;
    }

    // Record a freshly-defined value. Returns false (and emits a diagnostic)
    // if the value was already mapped — the SSA single-definition contract
    // would otherwise silently keep the FIRST mapping while the lowerer
    // emitted a second `addInst`, leaving downstream consumers reading the
    // wrong vreg.
    bool defineValue(MirInstId id, LirReg reg) {
        auto [it, inserted] = valueToReg.emplace(id.v, reg);
        if (!inserted) {
            reportDoubleDef(id);
            return false;
        }
        return true;
    }

    // ── per-instruction lowering ──────────────────────────────────────

    // Dispatch non-terminator MIR opcodes. `Return` is handled inline by
    // `lowerBlock` (it tracks the seal flag), so it does NOT appear here;
    // a stray Return reaching this dispatch is malformed MIR and falls
    // into `reportUnsupported` by design.
    void lowerInst(MirInstId id) {
        MirOpcode const op = mir.instOpcode(id);
        switch (op) {
            case MirOpcode::Arg:    return lowerArg(id);
            case MirOpcode::Const:  return lowerConst(id);
            case MirOpcode::Add:    return lowerBinaryOp(id, SlotAdd, "add", opAdd);
            case MirOpcode::Sub:    return lowerBinaryOp(id, SlotSub, "sub", opSub);
            case MirOpcode::Mul:    return lowerBinaryOp(id, SlotMul, "mul", opMul);
            default: break;
        }
        reportUnsupported(op, id);
    }

    void lowerArg(MirInstId id) {
        if (!opArg.has_value()) {
            reportMissingOpcode(SlotArg, "arg", "MIR Arg");
            return;
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        // The MIR `Arg` payload (parameter index) flows through to the LIR
        // `arg` inst's `payload` field — the JSON contract documented at
        // x86_64.target.json:23 is "minOperands=0, maxOperands=0; payload
        // carries the parameter index".
        lir.addInst(*opArg, result, std::span<LirOperand const>{},
                    /*payload=*/mir.argIndex(id));
        defineValue(id, result);
    }

    void lowerConst(MirInstId id) {
        if (!opMov.has_value()) {
            reportMissingOpcode(SlotMov, "mov", "MIR Const");
            return;
        }
        // Cycle 3a: only literals that fit in int32 lower cleanly. Larger
        // integer literals + float / string variants defer to cycle 3b's
        // literal-pool wiring (movabsq on x86_64, or a fresh ImmFloat
        // operand kind for floats). Bool literals encode as 0/1.
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
            return;  // intentionally NO defineValue: any later use of this
                     // MirInstId surfaces a fresh, locatable diagnostic via
                     // `regForValue` rather than threading a placeholder vreg.
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 1> ops{LirOperand::makeImmInt32(imm32)};
        lir.addInst(*opMov, result, ops);
        defineValue(id, result);
    }

    void lowerBinaryOp(MirInstId id, MnemonicSlot slot, std::string_view mnemonic,
                       std::optional<std::uint16_t> opcode) {
        if (!opcode.has_value()) {
            reportMissingOpcode(slot, mnemonic, mirOpcodeName(mir.instOpcode(id)));
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::optional<LirReg> const a = regForValue(operands[0]);
        std::optional<LirReg> const b = regForValue(operands[1]);
        if (!a.has_value() || !b.has_value()) {
            return;  // diagnostic already issued; don't emit a half-broken inst
        }
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 2> ops{LirOperand::makeReg(*a), LirOperand::makeReg(*b)};
        lir.addInst(*opcode, result, ops);
        defineValue(id, result);
    }

    // Return true iff a real `ret` LIR inst was emitted (sealing the block).
    // False when the lowering bailed (missing opcode / undefined operand /
    // wrong arity) — the caller may then emit a fallback seal AND know the
    // sealing happened in a recovery path, not the normal one.
    bool lowerReturn(MirInstId id) {
        if (!opRet.has_value()) {
            reportMissingOpcode(SlotRet, "ret", "MIR Return");
            return false;
        }
        auto const operands = mir.instOperands(id);
        if (operands.empty()) {
            lir.addReturn(*opRet, std::span<LirOperand const>{});
            return true;
        }
        if (operands.size() != 1) {
            // MIR Return is structurally ≤1 operand. >1 is a malformed
            // input — defense-in-depth.
            reportUnsupported(MirOpcode::Return, id);
            return false;
        }
        std::optional<LirReg> const v = regForValue(operands[0]);
        if (!v.has_value()) return false;
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*v)};
        lir.addReturn(*opRet, ops);
        return true;
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
            // `lowerReturn` is the only opcode that emits a terminator in
            // cycle 3a. Track its emission directly so the fallback-seal
            // below doesn't fire when the real seal succeeded — and DOES
            // fire when the missing-`ret`-opcode bail-out left the block
            // unsealed.
            if (op == MirOpcode::Return) {
                sealed = lowerReturn(inst);
                continue;
            }
            lowerInst(inst);
            // Defense-in-depth: a malformed MIR whose last instruction is
            // a non-terminator (e.g. `Add`) would otherwise silently fall
            // through to the fallback seal below. Report it loudly. The
            // ML3 verifier catches this at MIR-build time; this guard
            // protects callers who lower raw direct-Mir-ctor input.
            if (i + 1 == n) {
                MirOpcode const lastOp = op;
                bool const isTerminator =
                       lastOp == MirOpcode::Br
                    || lastOp == MirOpcode::CondBr
                    || lastOp == MirOpcode::Switch
                    || lastOp == MirOpcode::Return
                    || lastOp == MirOpcode::Unreachable;
                if (!isTerminator) {
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                    d.severity = DiagnosticSeverity::Error;
                    d.actual   = std::format(
                        "MIR block ended with non-terminator opcode '{}' (inst {}) "
                        "— structural violation",
                        mirOpcodeName(lastOp), inst.v);
                    reporter.report(std::move(d));
                }
            }
        }
        // Fallback seal for blocks whose MIR terminator was deferred to a
        // later cycle (Br/CondBr/Switch/Unreachable). The diagnostic was
        // already issued in `lowerInst`. If `opRet` is itself missing the
        // builder will abort at `finish()` time — that's the cleanest
        // failure mode given the target is unusable without `ret` anyway.
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
