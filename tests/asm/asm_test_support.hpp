#pragma once

// Shared helpers for asm tests. Promoted from per-test-file duplicates
// (simplifier review of AS2 cycle 2) so a future tests/asm/* file
// gains the same convenience without re-rolling the boilerplate.

#include "asm/disasm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_pass_util.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <variant>

namespace dss::test_support::asm_ {

// Count how many diagnostics in the reporter carry the given code.
// Tests use this for "exactly N of code X were emitted" assertions
// (the parallel-index continuity guarantee in AS1 cycle 1; the
// per-instruction fail-loud check in AS2 cycle 2).
[[nodiscard]] inline std::size_t
countDiagnostics(DiagnosticReporter const& rep, DiagnosticCode code) noexcept {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

// Round-trip verification helper (plan 13 §2.9 + AS5).
//
// TEST-ONLY (architect AS5 review). The function is `inline` here so
// each test TU emits its own copy; it explicitly does NOT live in
// the production library because:
//   * it takes a `Lir const&` reference — the linker (plan 14) does
//     NOT hold a Lir and must not be tempted to invoke this;
//   * the function's purpose is encoder-output verification, which
//     is a test concern, not a runtime concern.
//
// After the encoder produces bytes for one instruction, this helper
// re-extracts the per-slot operand values from those bytes (via
// `disassembleInst`) and asserts they match what the LIR operand was
// at encode time. Catches the silent-failure class where the encoder
// produces valid-but-WRONG bytes.
//
// `encodedBytes` MUST be the exact byte window for ONE instruction
// — caller strides through the function-level byte stream one inst
// at a time. Returns true iff every slot value matched; emits
// `A_RoundTripMismatch` into `reporter` on any disagreement.
[[nodiscard]] inline bool
roundTripVerify(TargetSchema const&            schema,
                Lir const&                     lir,
                LirInstId                      inst,
                std::span<std::uint8_t const>  encodedBytes,
                DiagnosticReporter&            reporter) {
    using dss::report;

    auto const opcode = lir.instOpcode(inst);
    auto const result = disassembleInst(schema, opcode, encodedBytes, reporter);
    if (!result.has_value()) {
        return false;
    }
    if (result->bytesConsumed != encodedBytes.size()) {
        report(reporter, DiagnosticCode::A_RoundTripMismatch,
               DiagnosticSeverity::Error,
               std::format("round-trip: opcode {} consumed {} bytes "
                           "but encoded image is {} bytes",
                           opcode, result->bytesConsumed,
                           encodedBytes.size()));
        return false;
    }

    auto const* info     = schema.opcodeInfo(opcode);
    auto const& variant  = info->encoding.variants[result->variantIndex];
    auto const  instOps  = lir.instOperands(inst);
    LirReg const resultReg = lir.instResult(inst);

    // D-AS5-3 closure: `DisassembledInst` splits into
    // `optional<DisassembledSlot> result` + `vector<DisassembledSlot>
    // wires`. The result-slot check runs against `result->result`;
    // the wire iteration runs against `result->wires` parallel-indexed
    // with `variant.wires`. No more positional cursor.
    auto const checkSlot = [&](DisassembledSlot const& slot,
                                std::int64_t expected,
                                std::string_view label) -> bool {
        // Symbol-bearing slots carry std::nullopt (Disp32 / Imm26).
        // The expected-value comparison is meaningful only when the
        // recovered value is present; the symbol-identity check runs
        // out of band (Relocation entry cross-reference).
        if (!slot.value.has_value()) {
            report(reporter, DiagnosticCode::A_RoundTripMismatch,
                   DiagnosticSeverity::Error,
                   std::format("round-trip: opcode '{}' slot '{}' "
                               "({}): disasm recovered nullopt but "
                               "caller expected concrete value {}",
                               info->mnemonic,
                               encodingSlotKindName(slot.kind),
                               label, expected));
            return false;
        }
        if (*slot.value != expected) {
            report(reporter, DiagnosticCode::A_RoundTripMismatch,
                   DiagnosticSeverity::Error,
                   std::format("round-trip: opcode '{}' slot '{}' "
                               "({}): expected value {} but disasm "
                               "recovered {}",
                               info->mnemonic,
                               encodingSlotKindName(slot.kind),
                               label, expected, *slot.value));
            return false;
        }
        return true;
    };

    if (variant.resultSlot.has_value()) {
        if (!result->result.has_value()) {
            report(reporter, DiagnosticCode::A_RoundTripMismatch,
                   DiagnosticSeverity::Error,
                   std::format("round-trip: opcode '{}' resultSlot "
                               "declared but disasm produced no result",
                               info->mnemonic));
            return false;
        }
        auto const* regInfo = schema.registerInfo(
            static_cast<std::uint16_t>(resultReg.id));
        if (regInfo == nullptr) {
            report(reporter, DiagnosticCode::A_RoundTripMismatch,
                   DiagnosticSeverity::Error,
                   std::format("round-trip: opcode '{}' result reg "
                               "ordinal {} not in target's register "
                               "table",
                               info->mnemonic,
                               static_cast<unsigned>(resultReg.id)));
            return false;
        }
        if (!checkSlot(*result->result,
                        static_cast<std::int64_t>(regInfo->hwEncoding),
                        "result")) {
            return false;
        }
    } else if (result->result.has_value()) {
        report(reporter, DiagnosticCode::A_RoundTripMismatch,
               DiagnosticSeverity::Error,
               std::format("round-trip: opcode '{}' variant declared no "
                           "resultSlot but disasm produced one",
                           info->mnemonic));
        return false;
    }

    if (result->wires.size() != variant.wires.size()) {
        report(reporter, DiagnosticCode::A_RoundTripMismatch,
               DiagnosticSeverity::Error,
               std::format("round-trip: opcode '{}' variant declared {} "
                           "wires but disasm produced {}",
                           info->mnemonic, variant.wires.size(),
                           result->wires.size()));
        return false;
    }
    for (std::size_t wi = 0; wi < variant.wires.size(); ++wi) {
        auto const& wire   = variant.wires[wi];
        auto const& slot   = result->wires[wi];
        auto const& srcOp  = instOps[wire.index];
        std::int64_t expected = 0;
        // Closed-enum switch (convergence-fix B, AS5 review): every
        // new LirOperandKind must declare its disasm-comparison
        // expectation. An if/elif chain silently treating unhandled
        // kinds as expected=0 would spuriously match a slot value of
        // 0 — the exact silent-failure class the oracle exists to catch.
        switch (srcOp.kind) {
            case LirOperandKind::Reg: {
                auto const* regInfo = schema.registerInfo(
                    static_cast<std::uint16_t>(srcOp.reg.id));
                if (regInfo == nullptr) {
                    report(reporter, DiagnosticCode::A_RoundTripMismatch,
                           DiagnosticSeverity::Error,
                           std::format("round-trip: opcode '{}' wire {} "
                                       "Reg operand ordinal {} not in "
                                       "register table",
                                       info->mnemonic, wire.index,
                                       static_cast<unsigned>(srcOp.reg.id)));
                    return false;
                }
                expected = static_cast<std::int64_t>(regInfo->hwEncoding);
                break;
            }
            case LirOperandKind::ImmInt:
                expected = static_cast<std::int64_t>(srcOp.immInt32);
                break;
            case LirOperandKind::SymbolRef:
                // Symbol-bearing slots: disasm reports nullopt
                // (D-AS5-3). The full symbol-identity check is the
                // caller's Relocation cross-reference. Confirm the
                // slot value IS nullopt then move on.
                if (slot.value.has_value()) {
                    report(reporter, DiagnosticCode::A_RoundTripMismatch,
                           DiagnosticSeverity::Error,
                           std::format("round-trip: opcode '{}' wire {} "
                                       "SymbolRef slot must be nullopt "
                                       "(linker patches at link time) "
                                       "but disasm recovered value {}",
                                       info->mnemonic, wire.index,
                                       *slot.value));
                    return false;
                }
                continue;  // skip the value-comparison checkSlot below
            case LirOperandKind::LiteralIndex: {
                // D-CSUBSET-BITFIELD-WIDE-UNIT: the wide pool literal of
                // `mov r64, imm64`. The VALUE lives in `LirLiteralPool`
                // (the 8-byte POD can't inline it); the disasm recovers
                // it as the raw int64 bit pattern from the io field. The
                // expected value is the pool's integer literal — fail
                // loud on a non-integer pool arm (floats route to rodata,
                // never reaching the encoder as a Const LiteralIndex).
                auto const& lit = lir.literalValue(srcOp.litIndex);
                if (auto const* i = std::get_if<std::int64_t>(&lit.value)) {
                    expected = *i;
                } else if (auto const* u =
                               std::get_if<std::uint64_t>(&lit.value)) {
                    expected = static_cast<std::int64_t>(*u);
                } else {
                    report(reporter, DiagnosticCode::A_RoundTripMismatch,
                           DiagnosticSeverity::Error,
                           std::format("round-trip: opcode '{}' wire {} "
                                       "LiteralIndex pool slot {} is not an "
                                       "integer literal",
                                       info->mnemonic, wire.index,
                                       srcOp.litIndex));
                    return false;
                }
                break;
            }
            case LirOperandKind::None:
            case LirOperandKind::BlockRef:
            case LirOperandKind::MemBase:
            case LirOperandKind::MemOffset:
                report(reporter, DiagnosticCode::A_RoundTripMismatch,
                       DiagnosticSeverity::Error,
                       std::format("round-trip: opcode '{}' wire {} "
                                   "LirOperandKind ordinal {} is not yet "
                                   "covered by the oracle — substrate "
                                   "drift, add a case arm when its "
                                   "encoder consumer lands",
                                   info->mnemonic, wire.index,
                                   static_cast<unsigned>(srcOp.kind)));
                return false;
        }
        if (!checkSlot(slot, expected,
                       std::format("wire[index={}]", wire.index))) {
            return false;
        }
    }

    // Wire arity check ran before the loop; result-slot presence
    // check ran above. D-AS5-3 closure: the structural split into
    // `result` + `wires` makes the previous positional cursor-vs-
    // slots.size() check redundant.
    return true;
}

} // namespace dss::test_support::asm_

// Pull `roundTripVerify` into the dss namespace as an inline alias
// so existing test call sites continue to resolve `dss::round
// TripVerify(...)` without a using-declaration. This is a header-
// only convenience; production code (which doesn't include this
// header) sees no such symbol.
namespace dss {
using dss::test_support::asm_::roundTripVerify;
}
