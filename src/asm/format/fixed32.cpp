#include "asm/format/fixed32.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <cassert>
#include <cstdint>
#include <format>
#include <optional>
#include <span>

namespace dss::fixed32 {

namespace {

using lir_pass_util::report;

// Substrate-tier helper shared with `x86_variable::encode`. Kept
// local because the project has not (yet) lifted a cross-walker
// helper module — when AS-future adds a third walker, this and the
// matching `hwEncodingOf` in x86_variable.cpp should hoist to a
// `src/asm/format/walker_util.{hpp,cpp}`. Anchored as a delimited
// cleanup at that future cycle.
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
    // AArch64-style 5-bit register fields can address 32 registers
    // (X0..X30 + XZR=31). A future widening (SVE vector regs use
    // a 5-bit-extended encoding scheme) would gain its own slot.
    if (info->hwEncoding > 31) {
        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': register '{}' hwEncoding {} "
                           "exceeds 5 bits — fixed32 cannot encode",
                           mnemonic, info->name, info->hwEncoding));
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(info->hwEncoding);
}

// Mirrors `operandsMatchGuard` in x86_variable.cpp. The function
// stays local (not lifted) for the same reason as `hwEncodingOf` —
// anchored as a future walker-util consolidation.
[[nodiscard]] constexpr std::optional<LirOperandKind>
filterToLirKind(OperandKindFilter f) noexcept {
    switch (f) {
        case OperandKindFilter::Reg:    return LirOperandKind::Reg;
        case OperandKindFilter::ImmInt: return LirOperandKind::ImmInt;
    }
    return std::nullopt;
}

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

// Per-slot bit-window descriptor. `lsb` is the bit position of the
// slot's least-significant bit inside the 32-bit fixed word. `width`
// is the slot's bit width.
struct SlotBitWindow {
    std::uint8_t lsb;
    std::uint8_t width;
};

// Map each fixed32 EncodingSlotKind to its bit window. The shape-vs-
// slot validate rule guarantees only Fixed32 slots reach this
// function for a fixed32 walker call.
[[nodiscard]] constexpr std::optional<SlotBitWindow>
windowFor(EncodingSlotKind s) noexcept {
    switch (s) {
        case EncodingSlotKind::Rd: return SlotBitWindow{ 0,  5 };
        case EncodingSlotKind::Rn: return SlotBitWindow{ 5,  5 };
        case EncodingSlotKind::Rm: return SlotBitWindow{ 16, 5 };
        // x86-variable slots — never reached on a fixed32 variant
        // (validate() rejects). Returning nullopt makes the walker
        // fail loud if a schema bypasses validate() somehow.
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
            return std::nullopt;
    }
    // Enum-drift fallback — a new EncodingSlotKind value added
    // without a matching arm above falls here. Returning nullopt
    // makes the walker fail loud (`orInto` emits a hard diagnostic).
    return std::nullopt;
}

void appendWordLE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v         & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
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
    assert(info != nullptr && "fixed32::encode requires non-null info");

    auto const instOps    = lir.instOperands(inst);
    LirReg const result = lir.instResult(inst);

    // First-match variant selection.
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

    // Track which slot windows have been written — defense-in-depth
    // mirror of x86_variable's `wroteModRm*` guard. validate() should
    // have rejected schemas declaring two writers to the same slot,
    // but if a future synthetic Lir reaches the encoder, the
    // assertion fails loud rather than silently OR-corrupting bits.
    // One bit per EncodingSlotKind value — sized from the shared name
    // table so adding a new slot to the enum + table is the SAME
    // change, no manual array-size update. Silent-failure-fix G.
    constexpr std::size_t kSlotCount = kEncodingSlotKindTable.rows.size();
    std::array<bool, kSlotCount> wroteSlot{};

    std::uint32_t word = selected->tmpl.fixedWord;

    auto const orInto = [&](EncodingSlotKind slot,
                            std::uint8_t value) -> bool {
        auto const w = windowFor(slot);
        if (!w.has_value()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': encoding slot '{}' is not "
                               "in the fixed32 vocabulary "
                               "(schema-vs-walker drift — validate() "
                               "should have rejected the schema)",
                               info->mnemonic,
                               encodingSlotKindName(slot)));
            return false;
        }
        auto const slotIdx = static_cast<std::size_t>(slot);
        // `slotIdx < kSlotCount` holds by the closed-enum invariant
        // (every enum value has a row in `kEncodingSlotKindTable`)
        // — but check defensively in case a future entry skips a row.
        if (slotIdx >= wroteSlot.size()) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': slot kind ordinal {} "
                               "exceeds tracked-slot count {} "
                               "(enum-vs-table drift)",
                               info->mnemonic, slotIdx, wroteSlot.size()));
            return false;
        }
        if (wroteSlot[slotIdx]) {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': second writer to "
                               "fixed32 slot '{}' (validate() "
                               "should have rejected the schema)",
                               info->mnemonic,
                               encodingSlotKindName(slot)));
            return false;
        }
        wroteSlot[slotIdx] = true;
        std::uint32_t const mask = ((1u << w->width) - 1u) << w->lsb;
        std::uint32_t const bits =
            (static_cast<std::uint32_t>(value) & ((1u << w->width) - 1u))
            << w->lsb;
        word = (word & ~mask) | bits;
        return true;
    };

    // Wire the result register into its declared slot.
    if (selected->resultSlot.has_value()) {
        auto const hw = hwEncodingOf(result, schema,
                                      info->mnemonic, reporter);
        if (!hw.has_value()) return false;
        if (!orInto(*selected->resultSlot, *hw)) return false;
    }

    // Wire each declared source operand into its slot.
    for (auto const& wire : selected->wires) {
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
            auto const hw = hwEncodingOf(srcOp.reg, schema,
                                          info->mnemonic, reporter);
            if (!hw.has_value()) return false;
            if (!orInto(wire.slotKind, *hw)) return false;
        } else {
            report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': variant operand {} has "
                               "non-Reg kind (ordinal {}) — fixed32 "
                               "cycle-3 scope is register-only",
                               info->mnemonic, wire.index,
                               static_cast<int>(srcOp.kind)));
            return false;
        }
    }

    appendWordLE(out, word);
    return true;
}

} // namespace dss::fixed32
