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

// One transient lowerer per `lowerToLir` call. Holds the per-pass state
// the dispatcher methods read/write. Mirrors `Lowerer` in `hir_to_mir.cpp`.
struct Lowerer {
    Mir const&          mir;
    TargetSchema const& target;
    DiagnosticReporter& reporter;
    LirBuilder          lir;

    // Cached opcode mnemonics → numeric indexes. Looked up once at Lowerer
    // construction. `std::nullopt` when the target schema does not declare
    // the mnemonic; `reportMissingOpcode` fires ONCE per missing slot.
    std::optional<std::uint16_t> opArg;
    std::optional<std::uint16_t> opMov;
    std::optional<std::uint16_t> opAdd;
    std::optional<std::uint16_t> opSub;
    std::optional<std::uint16_t> opMul;
    std::optional<std::uint16_t> opCmp;
    std::optional<std::uint16_t> opSetcc;
    std::optional<std::uint16_t> opJmp;
    std::optional<std::uint16_t> opJcc;
    std::optional<std::uint16_t> opRet;
    std::optional<std::uint16_t> opUnreachable;
    enum MnemonicSlot : std::uint8_t {
        SlotArg, SlotMov, SlotAdd, SlotSub, SlotMul,
        SlotCmp, SlotSetcc, SlotJmp, SlotJcc,
        SlotRet, SlotUnreachable, SlotCount
    };
    std::array<bool, SlotCount> missingReported{};

    // Per-function state. Cleared at the top of `lowerFunction`.
    //
    // `valueToReg`: typed side-table mapping MIR instruction id → LIR
    // virtual register that materialised its result. Switched from the
    // cycle-3a `unordered_map<uint32_t, LirReg>` to MirAttribute<LirReg>
    // for type safety + dense-vector backing once coverage crosses the
    // substrate threshold — exactly the architect agent's cycle-3a
    // follow-up.
    MirAttribute<LirReg>            valueToReg;
    // `mirBlockToLirBlock`: parallel block-tier map. MirBlockAttribute
    // binds to the block arena (sibling of the instruction arena).
    MirBlockAttribute<LirBlockId>   mirBlockToLirBlock;

    // Whether the running lowering pass added any error-severity
    // diagnostics. Mirrors ML2's delta-on-errorCount; reset by the ctor.
    std::uint32_t baselineErrors = 0;
    bool hadError() const noexcept {
        return reporter.errorCount() != baselineErrors;
    }

    Lowerer(Mir const& m, TargetSchema const& t, DiagnosticReporter& r)
        : mir(m), target(t), reporter(r), lir(t),
          valueToReg(m), mirBlockToLirBlock(m.blockArena()) {
        baselineErrors = reporter.errorCount();
        opArg         = target.opcodeByMnemonic("arg");
        opMov         = target.opcodeByMnemonic("mov");
        opAdd         = target.opcodeByMnemonic("add");
        opSub         = target.opcodeByMnemonic("sub");
        opMul         = target.opcodeByMnemonic("mul");
        opCmp         = target.opcodeByMnemonic("cmp");
        opSetcc       = target.opcodeByMnemonic("setcc");
        opJmp         = target.opcodeByMnemonic("jmp");
        opJcc         = target.opcodeByMnemonic("jcc");
        opRet         = target.opcodeByMnemonic("ret");
        opUnreachable = target.opcodeByMnemonic("unreachable");
    }

    // ── diagnostics ──────────────────────────────────────────────────

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
            case MirOpcode::Add:    return lowerBinaryOp(id, SlotAdd, "add", opAdd);
            case MirOpcode::Sub:    return lowerBinaryOp(id, SlotSub, "sub", opSub);
            case MirOpcode::Mul:    return lowerBinaryOp(id, SlotMul, "mul", opMul);
            case MirOpcode::ICmpEq: case MirOpcode::ICmpNe:
            case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
            case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge:
            case MirOpcode::ICmpUlt: case MirOpcode::ICmpUle:
            case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge:
                return lowerICmp(id, *condCodeForICmp(op));
            case MirOpcode::Phi:    return;  // pre-pass-allocated; no body emission
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
        lir.addInst(*opArg, result, std::span<LirOperand const>{},
                    /*payload=*/mir.argIndex(id));
        defineValue(id, result);
    }

    void lowerConst(MirInstId id) {
        if (!opMov.has_value()) {
            reportMissingOpcode(SlotMov, "mov", "MIR Const");
            return;
        }
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
        if (!a.has_value() || !b.has_value()) return;
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 2> ops{LirOperand::makeReg(*a), LirOperand::makeReg(*b)};
        lir.addInst(*opcode, result, ops);
        defineValue(id, result);
    }

    // MIR ICmp{Eq,Ne,Slt,...} → LIR `cmp` + `setcc(cond)` pair. Naive
    // (no peephole with subsequent CondBr); the optimizer's compare-flag
    // forwarding pass will collapse `cmp/setcc/cmp 0/jcc-ne` to
    // `cmp/jcc-cond` once a use-count analysis lands.
    void lowerICmp(MirInstId id, TargetCondCode cond) {
        if (!opCmp.has_value()) {
            reportMissingOpcode(SlotCmp, "cmp", "MIR ICmp");
            return;
        }
        if (!opSetcc.has_value()) {
            reportMissingOpcode(SlotSetcc, "setcc", "MIR ICmp");
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
        lir.addInst(*opCmp, InvalidLirReg, cmpOps);

        LirReg const result = lir.newVReg(LirRegClass::GPR);
        lir.addInst(*opSetcc, result, std::span<LirOperand const>{},
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
        if (!opMov.has_value()) {
            reportMissingOpcode(SlotMov, "mov", "MIR Phi edge");
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
            lir.addInst(*opMov, tmp, ops);
            temps.push_back(tmp);
        }
        // Step 2: write each phi-result from its captured temp.
        for (std::size_t k = 0; k < pairs.size(); ++k) {
            std::array<LirOperand, 1> ops{LirOperand::makeReg(temps[k])};
            lir.addInst(*opMov, pairs[k].phi, ops);
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
            reportUnsupported(MirOpcode::Return, id);
            return false;
        }
        std::optional<LirReg> const v = regForValue(operands[0]);
        if (!v.has_value()) return false;
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*v)};
        lir.addReturn(*opRet, ops);
        return true;
    }

    bool lowerBr(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opJmp.has_value()) {
            reportMissingOpcode(SlotJmp, "jmp", "MIR Br");
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
        lir.addBr(*opJmp, mirBlockToLirBlock.get(succs[0]));
        return true;
    }

    bool lowerCondBr(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opCmp.has_value()) {
            reportMissingOpcode(SlotCmp, "cmp", "MIR CondBr");
            return false;
        }
        if (!opJcc.has_value()) {
            reportMissingOpcode(SlotJcc, "jcc", "MIR CondBr");
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
        lir.addInst(*opCmp, InvalidLirReg, cmpOps);
        lir.addCondBr(*opJcc, std::span<LirOperand const>{},
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
        if (!opCmp.has_value()) {
            reportMissingOpcode(SlotCmp, "cmp", "MIR Switch");
            return false;
        }
        if (!opJmp.has_value()) {
            reportMissingOpcode(SlotJmp, "jmp", "MIR Switch");
            return false;
        }
        if (!opJcc.has_value()) {
            reportMissingOpcode(SlotJcc, "jcc", "MIR Switch");
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
            lir.addInst(*opCmp, InvalidLirReg, cmpOps);

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
                lir.addCondBr(*opJcc, std::span<LirOperand const>{},
                              caseTarget, defaultJump, eqCond);
                lir.beginBlock(defaultJump);
                lir.addBr(*opJmp, mirBlockToLirBlock.get(defaultMir));
                return true;
            }
            nextBlock = lir.createBlock();
            lir.addCondBr(*opJcc, std::span<LirOperand const>{},
                          caseTarget, nextBlock, eqCond);
            lir.beginBlock(nextBlock);
        }
        // No cases (only default): jmp default.
        lir.addBr(*opJmp, mirBlockToLirBlock.get(defaultMir));
        return true;
    }

    bool lowerUnreachable(MirInstId /*id*/) {
        if (!opUnreachable.has_value()) {
            reportMissingOpcode(SlotUnreachable, "unreachable", "MIR Unreachable");
            return false;
        }
        lir.addUnreachable(*opUnreachable);
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
            if (opRet.has_value()) {
                lir.addReturn(*opRet, std::span<LirOperand const>{});
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
        if (!sealed && opRet.has_value()) {
            lir.addReturn(*opRet, std::span<LirOperand const>{});
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
                          DiagnosticReporter& reporter) {
    Lowerer L{mir, target, reporter};
    return std::move(L).run();
}

} // namespace dss
