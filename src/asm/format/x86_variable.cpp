#include "asm/format/x86_variable.hpp"

#include "asm/format/byte_emit.hpp"
#include "asm/format/walker_util.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <cassert>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

namespace dss::x86_variable {

namespace {

using dss::report;

// Map an `OperandKindFilter` to the `LirOperandKind` the substrate
// actually carries on each LIR operand pool slot. Returns nullopt if
// the filter doesn't have a single matching LIR operand-kind (none
// today; reserved for future fields like a `Mem` filter that
// matches a leading `Reg + MemBase + MemOffset` triplet).
// (`filterToLirKind`, `operandsMatchGuard`, and `hwEncodingOf`
// hoisted to `asm/format/walker_util.hpp` per D-AS3-2 closure.)
using walker_util::hwEncodingOf;
using walker_util::operandsMatchGuard;

// x86 ModR/M register fields are 4 bits wide (3-bit ModR/M.reg/rm +
// 1-bit REX extension). Declared at the call-site scope per
// simplicity + code-review consensus: the shape constant belongs
// next to the shape, not behind a passthrough shim.
constexpr std::uint8_t kX86RegFieldBits = 4;

// (`PendingRelocSlot` hoisted to walker_util.hpp; alias kept
// minimal.)
using walker_util::PendingRelocSlot;

// State accumulated while emitting one variant: the 3-bit codes
// destined for ModR/M.reg / ModR/M.rm + their high bits for REX.R /
// REX.B, plus the immediate(s) and pending symbol-relative slot
// that follow. The struct shape lets the slot wiring loop populate
// fields independently of emission order — emission writes REX →
// opcode → ModR/M → SIB → disp → imm → disp32 at the end.
//
// D-AS4-1 memory-addressing extension (cycle 2 of the load/store
// encoder support): when a variant wires a base register into
// `ModRmRmMem` (instead of the register-direct `ModRmRm`), the
// walker flips `modMode` to MemDisp32. The ModR/M.mod field then
// emits `10b` (32-bit displacement form) and — when the base's low
// 3 bits equal 4 (rsp/r12 family) — a SIB byte forcibly follows
// per the x86-64 ABI rule. The 4 displacement bytes come from a
// paired `Disp32Mem` slot (sourced from a LIR `MemOffset` operand).
struct EncodingState {
    bool                  hasModRm   = false;
    std::uint8_t          modRmReg3  = 0;       // low 3 bits → ModR/M.reg
    std::uint8_t          modRmRm3   = 0;       // low 3 bits → ModR/M.rm
    bool                  rexR       = false;   // high bit of ModRmReg slot's hwEncoding
    bool                  rexB       = false;   // high bit of ModRmRm slot's hwEncoding
    // `rexX` carries the SIB.index high bit. Set by the `SibIndex`
    // slot's wiring (D-AS4-5 closure 2026-06-01) from the index reg
    // hwEncoding bit 3. Stays false on no-index forms (the SIB byte
    // emits index=4 = no-index marker).
    bool                  rexX       = false;
    // Defense-in-depth: track which ModR/M sub-slots have been written.
    // `validate()` already rejects schemas that would double-write
    // (convergence-fix A), but a malformed `Lir` reaching the encoder
    // could trigger a duplicate via the wire loop — the assertion
    // catches it as a fail-loud rather than a silent overwrite.
    bool                  wroteModRmReg = false;
    bool                  wroteModRmRm  = false;
    // Addressing mode for the ModR/M.mod field. RegDirect (mod=11)
    // is the cycle-1 register-only shape; MemDisp32 (mod=10) emits
    // a 32-bit displacement and forces SIB when base.lo3 == 4.
    // Values chosen so `static_cast<uint8_t>(modMode)` IS the ModR/M.mod
    // field directly (no lookup table needed). RegDirect = 11b (mod=3);
    // MemDisp32 = 10b (mod=2 + 32-bit displacement follows).
    enum class ModMode : std::uint8_t { RegDirect = 0b11, MemDisp32 = 0b10 };
    ModMode               modMode    = ModMode::RegDirect;
    // Memory-mode 32-bit displacement (from a Disp32Mem slot —
    // immediate value, NOT relocated). std::optional makes the
    // ModR/M-mem + missing-Disp32Mem pairing fail loud rather than
    // silently emitting a zero offset.
    std::optional<std::int32_t> disp32Mem;
    // SIB.index slot (D-AS4-5). Set when a `SibIndex` wire fires;
    // optional makes the no-index vs with-index distinction explicit
    // (no-index → SIB byte emits index=4 marker; with-index → SIB
    // byte emits this register's low 3 bits + scale from MemBase).
    std::optional<std::uint8_t> sibIndex3;
    bool                  wroteSibIndex = false;
    // SIB.scale — 2-bit exponent (0=*1, 1=*2, 2=*4, 3=*8) derived
    // from the MemBase operand's `scale` field. Defaults to 0; when
    // a `SibIndex` is wired, the walker reads MemBase's scale via
    // the MemBaseScale wire and stores the exponent here.
    std::uint8_t          sibScaleExp = 0;
    std::vector<std::int32_t>     imm32s;        // immediate values to append (LE 4B each)
    std::optional<PendingRelocSlot> disp32;      // symbol-relative 32-bit slot (cycle 4)
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
        case EncodingSlotKind::ModRmRmMem:
            // D-AS4-1 memory-addressing: like ModRmRm but flips the
            // mod field to MemDisp32. The base register's low 3 bits
            // fill ModR/M.rm; the high bit drives REX.B. The SIB
            // forced-presence rule (when rm.lo3 == 4) fires at
            // emit time, not here.
            if (st.wroteModRmRm) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': second writer to "
                                   "ModR/M.rm slot (ModRmRmMem after a "
                                   "prior ModRmRm/ModRmRmMem wire)",
                                   mnemonic));
                return false;
            }
            st.hasModRm      = true;
            st.modRmRm3      = static_cast<std::uint8_t>(hwEnc & 0x7u);
            st.rexB          = (hwEnc & 0x8u) != 0u;
            st.wroteModRmRm  = true;
            st.modMode       = EncodingState::ModMode::MemDisp32;
            return true;
        case EncodingSlotKind::SibIndex:
            // D-AS4-5 indexed addressing: the index register's low 3
            // bits fill SIB.index; the high bit drives REX.X. With-
            // index forces a SIB byte unconditionally (independent of
            // the rsp/r12 force-presence rule for no-index).
            if (st.wroteSibIndex) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': second writer to "
                                   "SIB.index slot — only one index "
                                   "register per addressing form",
                                   mnemonic));
                return false;
            }
            // x86-64 ABI rule: SIB.index = 4 is the no-index marker;
            // it cannot encode a real rsp index (rsp = lo3 4). Reject
            // an rsp index loudly rather than silently producing a no-
            // index form. (silent-failure defense — without this, an
            // assembler bug that picked rsp as an index would emit a
            // valid-looking but wrong instruction.)
            if ((hwEnc & 0x7u) == 0b100u && (hwEnc & 0x8u) == 0u) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': rsp (lo3=4, hi=0) "
                                   "cannot be used as a SIB index — "
                                   "the x86-64 SIB encoding reserves "
                                   "index=4 as the no-index marker. "
                                   "Re-allocate the index to a non-rsp "
                                   "register.",
                                   mnemonic));
                return false;
            }
            st.sibIndex3      = static_cast<std::uint8_t>(hwEnc & 0x7u);
            st.rexX           = (hwEnc & 0x8u) != 0u;
            st.wroteSibIndex  = true;
            return true;
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Disp32Mem:
            // Imm32 / Disp32Mem expect an immediate-style value, not a
            // register hwEnc. Wired separately via the operand-kind
            // dispatch below. Reaching here means the JSON wired a
            // register into a non-register slot — fail loud.
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant wires a register "
                               "into a non-register slot '{}'",
                               mnemonic,
                               encodingSlotKindName(slot)));
            return false;
        case EncodingSlotKind::MemBaseScale:
            // MemBase carries a scale value, not a register. Fail loud.
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant wires a register "
                               "into a MemBaseScale slot",
                               mnemonic));
            return false;
        case EncodingSlotKind::Disp32:
        case EncodingSlotKind::Rd:
        case EncodingSlotKind::Rn:
        case EncodingSlotKind::Rm:
        case EncodingSlotKind::Imm26:
            // Other shapes — not handled by the x86 walker (the
            // schema's slotShapeFor + validate cross-check prevents
            // reaching here under a clean schema). Defense-in-depth
            // fail-loud.
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': slot '{}' belongs to a "
                               "different encoding shape and is not "
                               "handled by the x86 walker",
                               mnemonic,
                               encodingSlotKindName(slot)));
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

// D-AS4-1 + D-AS4-5 memory-addressing: validate a MemBase operand's
// scale and store the SIB.scale exponent for emission. Scale ∈
// {1,2,4,8} (D-AS4-5 generalisation from cycle-1's scale==1-only).
// The slot writes no bytes directly; the exponent feeds the SIB
// byte when a `SibIndex` is also wired (or the existing rsp/r12
// force-presence rule fires on no-index).
[[nodiscard]] bool
wireMemBaseScale(EncodingState& st, EncodingSlotKind slot,
                 std::uint32_t scale,
                 std::string_view mnemonic,
                 DiagnosticReporter& reporter) {
    if (slot != EncodingSlotKind::MemBaseScale) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': MemBase operand wired into "
                           "a non-MemBaseScale slot '{}'",
                           mnemonic, encodingSlotKindName(slot)));
        return false;
    }
    // x86-64 SIB.scale field is a 2-bit exponent; legal scales are
    // {1, 2, 4, 8}. Reject anything else loudly.
    std::uint8_t scaleExp = 0;
    switch (scale) {
        case 1u: scaleExp = 0; break;
        case 2u: scaleExp = 1; break;
        case 4u: scaleExp = 2; break;
        case 8u: scaleExp = 3; break;
        default:
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': MemBase scale {} is not "
                               "a legal x86-64 SIB scale (accepted: "
                               "1, 2, 4, 8)",
                               mnemonic, scale));
            return false;
    }
    st.sibScaleExp = scaleExp;
    return true;
}

// D-AS4-1 memory-addressing: stash a MemOffset operand's signed
// 32-bit displacement for emission after ModR/M (and SIB when
// present). std::optional collision check makes a future
// double-write (or a malformed schema) fail loud.
[[nodiscard]] bool
wireDisp32Mem(EncodingState& st, EncodingSlotKind slot, std::int32_t v,
              std::string_view mnemonic, DiagnosticReporter& reporter) {
    if (slot != EncodingSlotKind::Disp32Mem) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': MemOffset operand wired into "
                           "a non-Disp32Mem slot '{}'",
                           mnemonic, encodingSlotKindName(slot)));
        return false;
    }
    if (st.disp32Mem.has_value()) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': second writer to Disp32Mem "
                           "slot (only one memory displacement per "
                           "instruction in cycle scope)",
                           mnemonic));
        return false;
    }
    st.disp32Mem = v;
    return true;
}

} // namespace

bool encode(Lir const&                  lir,
            TargetSchema const&         schema,
            LirInstId                   inst,
            TargetOpcodeInfo const*     info,
            // `lirToMir` + `srcMap` reserved for future per-byte-range
            // stamping (e.g. if a single LIR inst encodes to multiple
            // discontinuous byte ranges — currently never the case).
            // The whole-instruction SourceMapEntry stamp happens at
            // the dispatch level in `asm.cpp::encodeInst` so each
            // walker sees only the bytes/relocs it directly produces.
            std::span<MirInstId const>  /*lirToMir*/,
            std::vector<std::uint8_t>&  out,
            std::vector<Relocation>&    relocs,
            std::vector<SourceMapEntry>& /*srcMap*/,
            DiagnosticReporter&         reporter) {
    // Substrate contract — `asm.cpp`'s dispatch screens
    // `opcodeInfo(opcode) != nullptr` BEFORE routing to a format
    // walker. Defensive assertion: a future caller bypassing the
    // dispatch (e.g. a unit test that constructs an inst with an
    // out-of-range opcode and calls `encode` directly) should fail
    // loud rather than dereference a null pointer below.
    assert(info != nullptr && "x86_variable::encode requires non-null info");

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
        auto const hw = hwEncodingOf(result, schema, info->mnemonic,
                                      kX86RegFieldBits, reporter);
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
            auto const hw = hwEncodingOf(srcOp.reg, schema, info->mnemonic,
                                          kX86RegFieldBits, reporter);
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
        } else if (srcOp.kind == LirOperandKind::MemBase) {
            // D-AS4-1 + D-AS4-5 memory-addressing: MemBase carries the
            // scale for `[base + index*scale + disp]` addressing.
            // Cycle-1 (closed at LK10 cycle 2) handled scale==1 only;
            // D-AS4-5 generalises to scale ∈ {1,2,4,8}.
            if (!wireMemBaseScale(st, wire.slotKind, srcOp.scale,
                                   info->mnemonic, reporter)) {
                return false;
            }
        } else if (srcOp.kind == LirOperandKind::MemOffset) {
            // D-AS4-1 memory-addressing: MemOffset carries the signed
            // 32-bit displacement for [base + disp32] addressing.
            if (!wireDisp32Mem(st, wire.slotKind, srcOp.offset,
                                info->mnemonic, reporter)) {
                return false;
            }
        } else if (srcOp.kind == LirOperandKind::SymbolRef) {
            // Plan 13 AS4 — symbol-bearing wire emits a Relocation.
            // Cycle scope: only Disp32 supports the symbol-relative
            // encoding on x86-variable. validate() pairs Disp32 with
            // a non-empty `wire.relocationKind`; we re-check
            // defensively in case a malformed Lir bypassed validate.
            if (wire.slotKind != EncodingSlotKind::Disp32) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': SymbolRef operand wired "
                                   "to non-Disp32 slot '{}' — cycle-4 "
                                   "x86-variable scope supports symbol "
                                   "references only on Disp32 slots",
                                   info->mnemonic,
                                   encodingSlotKindName(wire.slotKind)));
                return false;
            }
            if (!wire.relocationKind.has_value()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': wire to Disp32 slot has "
                                   "no `relocationKind` — validate() "
                                   "should have rejected this schema",
                                   info->mnemonic));
                return false;
            }
            if (st.disp32.has_value()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("opcode '{}': second writer to Disp32 "
                                   "slot — only one symbol-relative "
                                   "slot per instruction in cycle-4",
                                   info->mnemonic));
                return false;
            }
            st.disp32 = PendingRelocSlot{
                *wire.relocationKind,
                SymbolId{srcOp.symbolV}
            };
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
    //    D-AS4-1: `modMode` selects mod=11 (register-direct) vs
    //    mod=10 (memory + disp32). The walker accumulates modMode
    //    via the slot-wiring loop (ModRmRm → RegDirect, ModRmRmMem →
    //    MemDisp32).
    // Decide whether a SIB byte follows. Two triggers:
    //   (a) D-AS4-1 force-presence: memory mode + rm.lo3 == 4.
    //   (b) D-AS4-5 indexed addressing: a SibIndex wire fired.
    // When SIB follows, ModR/M.rm MUST be 4 (the "SIB follows"
    // marker); the actual base register's lo3 goes into SIB.base.
    // The pre-existing no-index force-presence path "worked by
    // accident" because rsp/r12 (lo3=4) doubled as both the marker
    // and the base — for non-{rsp,r12} indexed forms, that
    // coincidence doesn't hold and we MUST emit 4 explicitly.
    bool const memModeNeedsSibForce =
        (st.modMode != EncodingState::ModMode::RegDirect)
        && (st.modRmRm3 == 0b100u);
    bool const hasIndex = st.wroteSibIndex;
    bool const sibFollows = memModeNeedsSibForce || hasIndex;
    if (st.hasModRm || selected->tmpl.modrmRegExt.has_value()) {
        std::uint8_t const modField =
            static_cast<std::uint8_t>(st.modMode);
        std::uint8_t const regField =
            selected->tmpl.modrmRegExt.has_value()
                ? static_cast<std::uint8_t>(*selected->tmpl.modrmRegExt & 0x7u)
                : st.modRmReg3;
        std::uint8_t const rmField  = sibFollows
            ? static_cast<std::uint8_t>(0b100u)   // SIB-follows marker
            : st.modRmRm3;
        std::uint8_t const modrm    = static_cast<std::uint8_t>(
            (modField << 6) | (regField << 3) | rmField);
        out.push_back(modrm);
    }

    // 4.5) D-AS4-5 coherence: a SibIndex wire is only meaningful with
    //      a ModRmRmMem base (the indexed form is a memory-addressing
    //      mode; register-direct mode has no SIB). Fail loud if a
    //      schema declared SibIndex without ModRmRmMem — silent
    //      "SIB emitted with mod=11" would produce a wrong instruction.
    if (st.wroteSibIndex
        && st.modMode != EncodingState::ModMode::MemDisp32) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': variant wires SibIndex but "
                           "no ModRmRmMem (register-direct ModR/M mode) "
                           "— indexed addressing requires a memory base. "
                           "Schema is malformed.",
                           info->mnemonic));
        return false;
    }

    // 5) SIB byte. Emitted in TWO cases:
    //    (a) D-AS4-1 force-presence rule: ModR/M.mod != 11 AND
    //        ModR/M.rm.lo3 == 4 (rsp/r12 family — the rm encoding
    //        that otherwise means "SIB follows"). No index register;
    //        SIB encodes `[base + 0 + disp]` with index=4 (no-index
    //        marker) and scale=0.
    //    (b) D-AS4-5 indexed addressing: a `SibIndex` wire fired
    //        (st.wroteSibIndex). SIB encodes `[base + index*scale + disp]`
    //        with index = st.sibIndex3 (from the SibIndex wire's
    //        register operand), scale exponent = st.sibScaleExp
    //        (from MemBaseScale's scale field), base = st.modRmRm3.
    //
    //    SIB byte layout: scale<<6 | index<<3 | base
    //    Indexed form's REX.X drives the index high bit (already set
    //    in st.rexX by the SibIndex wire above; emitted in the REX
    //    prefix step earlier).
    // (Re-use `sibFollows` computed above for the ModR/M.rm encoding.)
    if (sibFollows) {
        std::uint8_t const sibIndex = hasIndex
            ? *st.sibIndex3
            : static_cast<std::uint8_t>(0b100u);  // no-index marker
        std::uint8_t const sibScale = hasIndex ? st.sibScaleExp : 0u;
        std::uint8_t const sib = static_cast<std::uint8_t>(
            (sibScale << 6) | (sibIndex << 3) | st.modRmRm3);
        out.push_back(sib);
    }

    // 6) Memory displacement (D-AS4-1). When modMode == MemDisp32,
    //    emit the 4-byte LE displacement from the Disp32Mem slot.
    //    Missing displacement in mem-mode is fail-loud (a schema
    //    that wires ModRmRmMem without a paired Disp32Mem is a
    //    malformed variant — surfacing here rather than silently
    //    emitting zero).
    if (st.modMode == EncodingState::ModMode::MemDisp32) {
        if (!st.disp32Mem.has_value()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant uses ModRmRmMem "
                               "(memory addressing) but no Disp32Mem "
                               "wire — schema is malformed",
                               info->mnemonic));
            return false;
        }
        asm_byte_emit::appendImm32LE(out, *st.disp32Mem);
    } else if (st.disp32Mem.has_value()) {
        // Symmetric guard (silent-failure-hunter F1 post-fold): a
        // schema that wires `Disp32Mem` but uses `ModRmRm` (register-
        // direct) instead of `ModRmRmMem` would silently stash the
        // displacement in EncodingState and never emit it. The forward
        // direction is caught above; this is the inverse. Fail loud
        // rather than drop bytes silently — anchored as belt-and-
        // suspenders against a future malformed schema variant.
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': variant wires Disp32Mem but "
                           "uses register-direct ModR/M mode (no "
                           "ModRmRmMem wire) — the displacement would "
                           "be silently dropped; schema is malformed",
                           info->mnemonic));
        return false;
    }

    // 7) Immediates: append in slot-wiring order.
    for (auto v : st.imm32s) {
        asm_byte_emit::appendImm32LE(out, v);
    }

    // 8) Disp32 (symbol-relative, plan 13 AS4). The relocation
    //    offset is the byte position WHERE the 4 zero placeholder
    //    bytes go — `out.size()` at emit time. The linker (plan 14)
    //    patches the 4 bytes in-place per the kind's formula.
    //    INVARIANT (silent-failure F4): in cycle-4 Disp32 is always
    //    the TRAILING bytes of an instruction (`call rel32`,
    //    `jmp rel32`, etc.), so capturing offset at emit time pins
    //    the correct patch site. A future x86 instruction that
    //    interleaves Disp32 with later prefixes/operands needs
    //    per-slot byte-offset tracking — anchored at plan 13 §3.1
    //    D-AS4-3.
    //    Cycle-4 addend is always 0 (no composite operands yet);
    //    plan 14 may later interpret a wire-declared addend bias.
    if (st.disp32.has_value()) {
        walker_util::appendPendingReloc(relocs, out, *st.disp32);
        // 4 placeholder bytes (linker patches these).
        asm_byte_emit::appendU32LE(out, 0u);
    }

    return true;
}

} // namespace dss::x86_variable
