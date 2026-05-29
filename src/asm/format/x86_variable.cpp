#include "asm/format/x86_variable.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <span>

namespace dss::x86_variable {

namespace {

using lir_pass_util::report;

// Map an `OperandKindFilter` to the `LirOperandKind` the substrate
// actually carries on each LIR operand pool slot. Returns nullopt if
// the filter doesn't have a single matching LIR operand-kind (none
// today; reserved for future fields like a `Mem` filter that
// matches a leading `Reg + MemBase + MemOffset` triplet).
[[nodiscard]] constexpr std::optional<LirOperandKind>
filterToLirKind(OperandKindFilter f) noexcept {
    switch (f) {
        case OperandKindFilter::Reg:    return LirOperandKind::Reg;
        case OperandKindFilter::ImmInt: return LirOperandKind::ImmInt;
    }
    return std::nullopt;
}

// Returns true iff the instruction's source operand kinds match the
// variant guard exactly (length AND per-position kind).
[[nodiscard]] bool
operandsMatchGuard(std::span<LirOperand const>            instOps,
                   std::span<OperandKindFilter const>      guard) noexcept {
    if (instOps.size() != guard.size()) return false;
    for (std::size_t i = 0; i < guard.size(); ++i) {
        auto const wanted = filterToLirKind(guard[i]);
        if (!wanted.has_value()) return false;
        if (instOps[i].kind != *wanted) return false;
    }
    return true;
}

// Resolve a register operand's `hwEncoding` (the ModR/M ordinal the
// CPU expects). Emits a diagnostic + returns nullopt when the operand
// is not a physical register, the register table doesn't carry it,
// or `hwEncoding` doesn't fit in 4 bits (defense — the schema's
// validate() pins the field width, but verifying here closes the
// "schema bug masquerading as encoder bug" silent-failure path).
[[nodiscard]] std::optional<std::uint8_t>
hwEncodingOf(LirReg reg, TargetSchema const& schema,
             std::string_view mnemonic, DiagnosticReporter& reporter) {
    if (!reg.valid() || reg.isPhysical == 0) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': register operand is not a "
                           "physical register (post-regalloc invariant "
                           "broken)",
                           mnemonic));
        return std::nullopt;
    }
    auto const* info = schema.registerInfo(static_cast<std::uint16_t>(reg.id));
    if (info == nullptr) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': register ordinal {} not in "
                           "target schema '{}' register table",
                           mnemonic, static_cast<unsigned>(reg.id),
                           schema.name()));
        return std::nullopt;
    }
    if (info->hwEncoding > 15) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': register '{}' hwEncoding {} "
                           "exceeds 4 bits — x86-variable cannot encode",
                           mnemonic, info->name, info->hwEncoding));
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(info->hwEncoding);
}

// Append an `int32_t` immediate in little-endian byte order.
void appendImm32LE(std::vector<std::uint8_t>& out, std::int32_t v) {
    auto const u = static_cast<std::uint32_t>(v);
    out.push_back(static_cast<std::uint8_t>(u         & 0xFF));
    out.push_back(static_cast<std::uint8_t>((u >>  8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((u >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((u >> 24) & 0xFF));
}

// State accumulated while emitting one variant: the 3-bit codes
// destined for ModR/M.reg / ModR/M.rm + their high bits for REX.R /
// REX.B, plus the immediate(s) that follow. The struct shape lets
// the slot wiring loop populate fields independently of emission
// order — emission writes REX → opcode → ModR/M → imm at the end.
struct EncodingState {
    bool                  hasModRm   = false;
    std::uint8_t          modRmReg3  = 0;       // low 3 bits → ModR/M.reg
    std::uint8_t          modRmRm3   = 0;       // low 3 bits → ModR/M.rm
    bool                  rexR       = false;   // high bit of ModRmReg slot's hwEncoding
    bool                  rexB       = false;   // high bit of ModRmRm slot's hwEncoding
    // `rexX` is reserved for SIB.index high bit — declared now so the
    // REX-byte assembly downstream is structurally complete; no cycle-2
    // walker writes it (current scope: register-direct only, no SIB).
    // SIB-bearing memory addressing modes will set this when they
    // land (paired with the corresponding `EncodingSlotKind::SibIndex`
    // entry that doesn't exist yet).
    bool                  rexX       = false;
    // Defense-in-depth: track which ModR/M sub-slots have been written.
    // `validate()` already rejects schemas that would double-write
    // (convergence-fix A), but a malformed `Lir` reaching the encoder
    // could trigger a duplicate via the wire loop — the assertion
    // catches it as a fail-loud rather than a silent overwrite.
    bool                  wroteModRmReg = false;
    bool                  wroteModRmRm  = false;
    std::vector<std::int32_t> imm32s;           // emitted after ModR/M, in order
};

// Wire a value (register hwEncoding OR immediate) into the named
// slot. Returns false on inconsistency (e.g. two operands trying to
// fill the same slot) — fail-loud rather than silently overwriting.
[[nodiscard]] bool
wireSlot(EncodingState& st, EncodingSlotKind slot,
         std::uint8_t hwEnc, std::string_view mnemonic,
         DiagnosticReporter& reporter) {
    switch (slot) {
        case EncodingSlotKind::ModRmReg:
            if (st.wroteModRmReg) {
                // validate() should have rejected this schema, but if
                // a future synthetic-Lir path reaches the encoder, the
                // assertion fails loud rather than silently overwriting
                // the already-written register bits.
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': second writer to "
                                   "ModR/M.reg slot (validate() should "
                                   "have rejected the schema)",
                                   mnemonic));
                return false;
            }
            st.hasModRm       = true;
            st.modRmReg3      = static_cast<std::uint8_t>(hwEnc & 0x7u);
            st.rexR           = (hwEnc & 0x8u) != 0u;
            st.wroteModRmReg  = true;
            return true;
        case EncodingSlotKind::ModRmRm:
            if (st.wroteModRmRm) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': second writer to "
                                   "ModR/M.rm slot (validate() should "
                                   "have rejected the schema)",
                                   mnemonic));
                return false;
            }
            st.hasModRm      = true;
            st.modRmRm3      = static_cast<std::uint8_t>(hwEnc & 0x7u);
            st.rexB          = (hwEnc & 0x8u) != 0u;
            st.wroteModRmRm  = true;
            return true;
        case EncodingSlotKind::Imm32:
            // Imm32 expects an immediate value, not a register hwEnc.
            // Wired separately via `wireImm32`. Reaching here means
            // the JSON wired a register into an Imm32 slot — fail
            // loud rather than silently dropping the high bits.
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant wires a register "
                               "into an Imm32 slot",
                               mnemonic));
            return false;
    }
    // Enum-drift fallback. A new `EncodingSlotKind` value added without
    // a matching switch arm would otherwise silently `return false` —
    // fail loud per the silent-failure review (mirrors the same pattern
    // in asm.cpp's TargetEncodingShape dispatch).
    report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
           DiagnosticSeverity::Error,
           std::format("opcode '{}': unknown encoding-slot ordinal {} "
                       "(internal-invariant: a new EncodingSlotKind "
                       "value was added without updating the x86 walker)",
                       mnemonic, static_cast<int>(slot)));
    return false;
}

[[nodiscard]] bool
wireImm32(EncodingState& st, EncodingSlotKind slot, std::int32_t v,
          std::string_view mnemonic, DiagnosticReporter& reporter) {
    if (slot != EncodingSlotKind::Imm32) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': variant wires an immediate "
                           "into a non-Imm32 slot",
                           mnemonic));
        return false;
    }
    st.imm32s.push_back(v);
    return true;
}

} // namespace

bool encode(Lir const&                  lir,
            TargetSchema const&         schema,
            LirInstId                   inst,
            TargetOpcodeInfo const*     info,
            std::span<MirInstId const>  /*lirToMir*/,
            std::vector<std::uint8_t>&  out,
            std::vector<Relocation>&    /*relocs*/,
            std::vector<SourceMapEntry>& /*srcMap*/,
            DiagnosticReporter&         reporter) {
    auto const instOps = lir.instOperands(inst);
    LirReg const result = lir.instResult(inst);

    // Find the first variant whose guard matches the operand kinds.
    TargetEncodingVariant const* selected = nullptr;
    for (auto const& v : info->encoding.variants) {
        if (operandsMatchGuard(instOps, v.operandKinds)) {
            selected = &v;
            break;
        }
    }
    if (selected == nullptr) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': no encoding variant matches "
                           "this instruction's operand kinds (declared "
                           "variants: {})",
                           info->mnemonic,
                           info->encoding.variants.size()));
        return false;
    }

    EncodingState st;

    // Wire the result register into its declared slot.
    if (selected->resultSlot.has_value()) {
        auto const hw = hwEncodingOf(result, schema, info->mnemonic, reporter);
        if (!hw.has_value()) return false;
        if (!wireSlot(st, *selected->resultSlot, *hw,
                      info->mnemonic, reporter)) {
            return false;
        }
    }

    // Wire each declared source operand into its slot.
    for (auto const& wire : selected->wires) {
        // The validate() rule guarantees wire.index < operandKinds.size(),
        // and the guard match guarantees operandKinds.size() == instOps.size().
        // Defense-in-depth: re-check the bound so a corrupted Lir
        // (test fixture or future synthetic) fails loud here.
        if (wire.index >= instOps.size()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant operand wire index "
                               "{} exceeds operand count {}",
                               info->mnemonic, wire.index, instOps.size()));
            return false;
        }
        auto const& srcOp = instOps[wire.index];
        if (srcOp.kind == LirOperandKind::Reg) {
            auto const hw = hwEncodingOf(srcOp.reg, schema, info->mnemonic, reporter);
            if (!hw.has_value()) return false;
            if (!wireSlot(st, wire.slotKind, *hw,
                          info->mnemonic, reporter)) {
                return false;
            }
        } else if (srcOp.kind == LirOperandKind::ImmInt) {
            if (!wireImm32(st, wire.slotKind, srcOp.immInt32,
                            info->mnemonic, reporter)) {
                return false;
            }
        } else {
            // Guard already screened by operandsMatchGuard, so this
            // only fires on a corrupted Lir.
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant operand {} has "
                               "unexpected LirOperandKind ordinal {}",
                               info->mnemonic, wire.index,
                               static_cast<int>(srcOp.kind)));
            return false;
        }
    }

    // ── Emit bytes in canonical x86 order ─────────────────────────
    // 1) Legacy prefixes (none for this cycle).
    // 2) REX prefix: emit iff W, R, or B is set. The 4 bits compose
    //    `0x40 | (W<<3) | (R<<2) | (X<<1) | B`. X is reserved here
    //    (SIB.index high bit) — joins when memory addressing modes
    //    do.
    bool const rexW = selected->tmpl.rexW;
    bool const rexR = st.rexR;
    bool const rexX = st.rexX;  // reserved — SIB.index high bit (no
                                 // cycle-2 walker writes this).
    bool const rexB = st.rexB;
    if (rexW || rexR || rexX || rexB) {
        std::uint8_t const rex =
            static_cast<std::uint8_t>(0x40u
            | (rexW ? 0x08u : 0u)
            | (rexR ? 0x04u : 0u)
            | (rexX ? 0x02u : 0u)
            | (rexB ? 0x01u : 0u));
        out.push_back(rex);
    }

    // 3) Opcode bytes (declared by the variant template).
    for (auto b : selected->tmpl.opcodeBytes) {
        out.push_back(b);
    }

    // 4) ModR/M byte. Emit iff any slot wired into ModR/M, OR the
    //    template uses `modrmRegExt` (which is a ModR/M.reg digit
    //    extension and only meaningful WITH a ModR/M byte).
    if (st.hasModRm || selected->tmpl.modrmRegExt.has_value()) {
        std::uint8_t const modField = 0b11u;  // mod=3, register-direct
        std::uint8_t const regField =
            selected->tmpl.modrmRegExt.has_value()
                ? static_cast<std::uint8_t>(*selected->tmpl.modrmRegExt & 0x7u)
                : st.modRmReg3;
        std::uint8_t const rmField  = st.modRmRm3;
        std::uint8_t const modrm    = static_cast<std::uint8_t>(
            (modField << 6) | (regField << 3) | rmField);
        out.push_back(modrm);
    }

    // 5) SIB byte: not yet (register-direct mod=3 needs no SIB; the
    //    "forced SIB when ModR/M.rm == 4" rule applies only to
    //    memory addressing modes, which land alongside Load/Store).

    // 6) Displacement: not yet (same — memory addressing modes).

    // 7) Immediates: append in slot-wiring order.
    for (auto v : st.imm32s) {
        appendImm32LE(out, v);
    }

    return true;
}

} // namespace dss::x86_variable
