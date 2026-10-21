#pragma once

#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/byte_emit.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════
// THE x64 UNWIND_INFO READ BACK INTO THE NEUTRAL CALL-FRAME VOCABULARY
// ═══════════════════════════════════════════════════════════════════════
//
// The decode half of the Win64 unwind encoding; the encode half is
// `buildFunctionUnwindInfo` in `pe.cpp`. The pair stands where
// `dwarf_cfi_decode.hpp` and `dwarf_cfi.hpp` stand for `.eh_frame`: a foreign
// object's frame description is read into `CfiFunction`, and the image's
// table is encoded from that, so a function is described by one model however
// it reached the link.
//
// ── WHY DECODE RATHER THAN CARRY THE BYTES ──
//
// Copying an object's UNWIND_INFO verbatim into the image would have needed
// no decoder, and it is rejected for two reasons:
//   (1) The bytes say nothing a writer of another format can read. A function
//       a COFF object brings and a DSS function sit in the same image and are
//       described by one writer from one model. A verbatim blob is a second
//       model, which only the PE image arm could ever write.
//   (2) The blob is not self-contained. Its handler field and every address
//       in the handler's data are relocations against the object's own
//       symbols, so a copy must still read and rebind each of them. A copy
//       that skipped the codes would keep exactly the part a reader cannot
//       check.
// The handler's DATA is the one part carried as bytes (`LanguageHandler` in
// `asm/asm.hpp`): its layout is the handler routine's own, and only that
// routine reads it.
//
// ── WHAT A CODE MEANS, AS THE OS UNWINDER READS IT ──
//
// The codes are applied in array order (descending CodeOffset), each one only
// when the faulting PC is at or past its CodeOffset. Every save code names its
// slot as an offset from the ESTABLISHER FRAME: RSP when the function names no
// frame register, and otherwise the frame register minus 16 x FrameOffset once
// the SET_FPREG code has run. In the body that is RSP after the fixed
// allocation, or the frame register's base. So:
//   * UWOP_PUSH_NONVOL r     -> CFA offset += 8, and r is at the new top:
//                               def_cfa_offset C; offset r, -C
//   * UWOP_ALLOC_SMALL/LARGE -> def_cfa_offset C + n (no op once a frame
//                               register is the base: the unwinder takes RSP
//                               back from the frame register, so an allocation
//                               after it moves nothing an unwind reads)
//   * UWOP_SET_FPREG         -> FP = RSP + 16F, so CFA = FP + (C - 16F):
//                               def_cfa FP, C - 16F
//   * UWOP_SAVE_NONVOL r, k  -> r at establisher frame + 8k:
//                               offset r, 8k - B
//   * UWOP_SAVE_XMM128 x, k  -> x at establisher frame + 16k:
//                               offset x, 16k - B
// B is the CFA's distance above the establisher frame: the RSP-based CFA
// offset at the SET_FPREG code when there is a frame register, and at the end
// of the prologue otherwise.
//
// ★ WHAT IS REFUSED, BY NAME: a version other than 1, chained unwind
//   information, UWOP_PUSH_MACHFRAME, the two _FAR saves and UWOP_ALLOC_LARGE
//   with op-info 1. ✔MEASURED P69 fold 3c: none of them appears in 31 objects
//   written by cl 19.44 (/Od and /O2), clang-cl 19.1.5 /O2 and mingw-w64 gcc 13.2.0
//   -O2, while every other code does. A form no reference wrote has no
//   measurement to decode against, and a guess would read as a carried frame.
//   Also refused: a code past SizeOfProlog (the encoder cannot state one), a
//   push after the frame register is set (the unwinder would read it at a
//   moved RSP), a frame register named with no SET_FPREG code (or the
//   reverse), and a register the target cannot name.

namespace dss::link::format {

// The x64 unwind operation numbers (the format's own vocabulary). The encoder
// in `pe.cpp` writes the same numbers.
namespace win64 {
inline constexpr std::uint8_t kUwopPushNonvol     = 0;
inline constexpr std::uint8_t kUwopAllocLarge     = 1;
inline constexpr std::uint8_t kUwopAllocSmall     = 2;
inline constexpr std::uint8_t kUwopSetFpReg       = 3;
inline constexpr std::uint8_t kUwopSaveNonvol     = 4;
inline constexpr std::uint8_t kUwopSaveNonvolFar  = 5;
inline constexpr std::uint8_t kUwopEpilog         = 6;
inline constexpr std::uint8_t kUwopSpare          = 7;
inline constexpr std::uint8_t kUwopSaveXmm128     = 8;
inline constexpr std::uint8_t kUwopSaveXmm128Far  = 9;
inline constexpr std::uint8_t kUwopPushMachframe  = 10;

// The sections the format keeps them in, by the names its producers give them
// and `pe.cpp` writes: the function table (one RUNTIME_FUNCTION per described
// function) and the unwind records it points at. A producer may suffix either,
// `$x` (cl's grouping) or `.x` (mingw-w64 gcc's `.xdata.unlikely` beside its
// `.text.unlikely`, ✔MEASURED P69 fold 3c).
inline constexpr std::string_view kFunctionTableSection = ".pdata";
inline constexpr std::string_view kUnwindInfoSection    = ".xdata";

[[nodiscard]] constexpr bool isSectionOf(std::string_view name, std::string_view family) noexcept {
    if (name == family) return true;
    return name.size() > family.size() && name.substr(0, family.size()) == family
        && (name[family.size()] == '$' || name[family.size()] == '.');
}

// UNWIND_INFO's Flags (the high five bits of its first byte).
inline constexpr std::uint8_t kUnwFlagEHandler  = 0x1u;  // called while the dispatcher searches
inline constexpr std::uint8_t kUnwFlagUHandler  = 0x2u;  // called while it unwinds
inline constexpr std::uint8_t kUnwFlagChainInfo = 0x4u;  // a RUNTIME_FUNCTION follows the codes

[[nodiscard]] constexpr std::string_view unwindOpName(std::uint8_t op) noexcept {
    switch (op) {
        case kUwopPushNonvol:    return "UWOP_PUSH_NONVOL";
        case kUwopAllocLarge:    return "UWOP_ALLOC_LARGE";
        case kUwopAllocSmall:    return "UWOP_ALLOC_SMALL";
        case kUwopSetFpReg:      return "UWOP_SET_FPREG";
        case kUwopSaveNonvol:    return "UWOP_SAVE_NONVOL";
        case kUwopSaveNonvolFar: return "UWOP_SAVE_NONVOL_FAR";
        case kUwopEpilog:        return "UWOP_EPILOG";
        case kUwopSpare:         return "UWOP_SPARE_CODE";
        case kUwopSaveXmm128:    return "UWOP_SAVE_XMM128";
        case kUwopSaveXmm128Far: return "UWOP_SAVE_XMM128_FAR";
        case kUwopPushMachframe: return "UWOP_PUSH_MACHFRAME";
    }
    return "an undefined operation";
}
} // namespace win64

// The registers an unwind code can name: a general register and a whole
// 16-byte vector register, each by its 4-bit hardware number. Built from the
// target document, so the decoder states no register of its own. An unwind
// code's register field names no reading of a shared number, so it gets the
// document's DEFAULT reading (`encodingRoleIsDefault`): on x86_64 number 5 is
// `rbp`, never `rip`, which shares it as a memory operand's base.
struct Win64UnwindRegisters {
    std::array<std::optional<std::uint16_t>, 16> general{};
    std::array<std::optional<std::uint16_t>, 16> vector{};

    // A number two registers of one class answer is refused when it is read,
    // not when the table is built: a document may carry such a pair for a
    // number no code of a given object names.
    std::array<bool, 16> generalAmbiguous{};
    std::array<bool, 16> vectorAmbiguous{};

    [[nodiscard]] static Win64UnwindRegisters of(TargetSchema const& target) {
        Win64UnwindRegisters out;
        auto const regs = target.registers();
        for (std::size_t i = 0; i < regs.size(); ++i) {
            TargetRegisterInfo const& r = regs[i];
            if (!r.subOf.empty() || r.hwEncoding > 0x0Fu) continue;
            if (!r.encodingRole.empty() && !r.encodingRoleIsDefault) continue;
            bool const isGeneral = r.regClass == TargetRegClass::GPR && r.widthBytes == 8u;
            bool const isVector  = r.regClass == TargetRegClass::FPR && r.widthBytes == 16u;
            if (!isGeneral && !isVector) continue;
            auto& slot      = isGeneral ? out.general : out.vector;
            auto& ambiguous = isGeneral ? out.generalAmbiguous : out.vectorAmbiguous;
            if (slot[r.hwEncoding].has_value()) ambiguous[r.hwEncoding] = true;
            slot[r.hwEncoding] = static_cast<std::uint16_t>(i);
        }
        return out;
    }
};

// One UNWIND_INFO read back.
struct DecodedWin64Unwind {
    // The frame description. `codeLength` is the caller's to set: it is the
    // RUNTIME_FUNCTION's extent, which the record itself does not state.
    CfiFunction cfi;
    bool        exceptionPhase = false;   // UNW_FLAG_EHANDLER
    bool        unwindPhase    = false;   // UNW_FLAG_UHANDLER
    // Byte offset, within the decoded section, of the field past the code
    // array: the handler's address when either phase is set.
    std::uint32_t afterCodes = 0;
};

[[nodiscard]] inline std::optional<DecodedWin64Unwind>
decodeWin64UnwindInfo(std::span<std::uint8_t const> section,
                      std::uint32_t                 offset,
                      Win64UnwindRegisters const&   regs,
                      CfiInitialState const&        entry,
                      std::string_view              contextLabel,
                      DiagnosticReporter&           reporter) {
    auto refuse = [&](DiagnosticCode code, std::string msg) -> std::optional<DecodedWin64Unwind> {
        detail::emit(reporter, code,
             std::string{contextLabel} + ": the unwind information at byte offset "
                 + std::to_string(offset) + " " + msg);
        return std::nullopt;
    };
    if (static_cast<std::size_t>(offset) + 4u > section.size()) {
        return refuse(DiagnosticCode::F_CorruptedBinary,
                      "runs past the end of its " + std::to_string(section.size())
                          + "-byte section");
    }
    std::uint8_t const versionAndFlags = section[offset];
    std::uint8_t const version         = versionAndFlags & 0x07u;
    std::uint8_t const flags           = static_cast<std::uint8_t>(versionAndFlags >> 3);
    std::uint8_t const sizeOfProlog    = section[offset + 1u];
    std::uint8_t const countOfCodes    = section[offset + 2u];
    std::uint8_t const frameRegister   = section[offset + 3u] & 0x0Fu;
    std::uint8_t const frameOffset     = static_cast<std::uint8_t>(section[offset + 3u] >> 4);
    if (version != 1u) {
        return refuse(DiagnosticCode::K_UnwindRuleUnrepresentable,
                      "is version " + std::to_string(version)
                          + "; the version this reader decodes is 1, the only one cl, clang-cl "
                            "and mingw-w64 gcc wrote for any C function measured, and a later "
                            "version's epilogue codes are not read here");
    }
    if ((flags & win64::kUnwFlagChainInfo) != 0u) {
        return refuse(DiagnosticCode::K_UnwindRuleUnrepresentable,
                      "is chained to another function's (UNW_FLAG_CHAININFO); no reference "
                      "compiler wrote one for any C function measured, and a chained "
                      "description is carried by no model here");
    }
    std::size_t const codesEnd =
        static_cast<std::size_t>(offset) + 4u
        + 2u * (static_cast<std::size_t>(countOfCodes) + (countOfCodes & 1u));
    if (codesEnd > section.size()) {
        return refuse(DiagnosticCode::F_CorruptedBinary,
                      "states " + std::to_string(countOfCodes)
                          + " code slot(s), which run past the end of its section");
    }
    auto node = [&](std::size_t k) -> std::uint16_t {
        std::size_t const at = static_cast<std::size_t>(offset) + 4u + 2u * k;
        return static_cast<std::uint16_t>(section[at] | (section[at + 1u] << 8));
    };

    // The codes in array order, each with its operand.
    struct Code {
        std::uint8_t  codeOffset;
        std::uint8_t  op;
        std::uint8_t  info;
        std::uint32_t operand;
    };
    std::vector<Code> codes;
    for (std::size_t k = 0; k < countOfCodes;) {
        std::uint16_t const head = node(k);
        Code c{static_cast<std::uint8_t>(head & 0xFFu),
               static_cast<std::uint8_t>((head >> 8) & 0x0Fu),
               static_cast<std::uint8_t>(head >> 12), 0u};
        std::size_t slots = 1;
        switch (c.op) {
            case win64::kUwopPushNonvol:
            case win64::kUwopAllocSmall:
            case win64::kUwopSetFpReg:
                break;
            case win64::kUwopAllocLarge:
                if (c.info != 0u) {
                    return refuse(DiagnosticCode::K_UnwindRuleUnrepresentable,
                                  "states UWOP_ALLOC_LARGE with op-info " + std::to_string(c.info)
                                      + " (an allocation in a 32-bit operand); no reference "
                                        "compiler wrote one for any C function measured");
                }
                slots = 2;
                break;
            case win64::kUwopSaveNonvol:
            case win64::kUwopSaveXmm128:
                slots = 2;
                break;
            default:
                return refuse(DiagnosticCode::K_UnwindRuleUnrepresentable,
                              "states " + std::string{win64::unwindOpName(c.op)} + " (operation "
                                  + std::to_string(c.op) + ") at prologue byte "
                                  + std::to_string(c.codeOffset)
                                  + "; no reference compiler wrote it for any C function "
                                    "measured, and it is not read here");
        }
        if (k + slots > countOfCodes) {
            return refuse(DiagnosticCode::F_CorruptedBinary,
                          "ends inside the operand of its last code");
        }
        if (slots == 2) c.operand = node(k + 1);
        codes.push_back(c);
        k += slots;
    }
    for (std::size_t i = 0; i < codes.size(); ++i) {
        if (codes[i].codeOffset > sizeOfProlog) {
            return refuse(DiagnosticCode::K_UnwindRuleUnrepresentable,
                          "states " + std::string{win64::unwindOpName(codes[i].op)}
                              + " at byte " + std::to_string(codes[i].codeOffset)
                              + ", past its own " + std::to_string(sizeOfProlog)
                              + "-byte prologue");
        }
        if (i > 0 && codes[i].codeOffset > codes[i - 1].codeOffset) {
            return refuse(DiagnosticCode::F_CorruptedBinary,
                          "lists its codes out of descending order");
        }
    }

    auto generalReg = [&](std::uint8_t hw, std::string_view what)
        -> std::optional<std::uint16_t> {
        if (regs.generalAmbiguous[hw] || !regs.general[hw].has_value()) {
            (void)refuse(DiagnosticCode::K_UnwindRuleUnrepresentable,
                         std::string{what} + " names general register number "
                             + std::to_string(hw)
                             + ", which the target document does not name as exactly one "
                               "8-byte general register");
            return std::nullopt;
        }
        return regs.general[hw];
    };

    // The CFA's distance above the establisher frame (B above): read before any
    // op is written, because a save code may precede the allocation it is
    // measured after.
    bool         sawSetFp = false;
    std::int64_t cfa      = entry.cfaOffset;
    std::int64_t base     = 0;
    for (auto it = codes.rbegin(); it != codes.rend(); ++it) {
        if (it->op == win64::kUwopSetFpReg) {
            if (sawSetFp) {
                return refuse(DiagnosticCode::F_CorruptedBinary, "sets its frame register twice");
            }
            sawSetFp = true;
            base     = cfa;
            continue;
        }
        if (sawSetFp) continue;
        if (it->op == win64::kUwopPushNonvol) cfa += 8;
        if (it->op == win64::kUwopAllocSmall) cfa += 8 * (static_cast<std::int64_t>(it->info) + 1);
        if (it->op == win64::kUwopAllocLarge) cfa += 8 * static_cast<std::int64_t>(it->operand);
    }
    if (!sawSetFp) base = cfa;
    if (sawSetFp != (frameRegister != 0u)) {
        return refuse(DiagnosticCode::F_CorruptedBinary,
                      sawSetFp ? "sets a frame register with UWOP_SET_FPREG while naming none"
                               : "names frame register number " + std::to_string(frameRegister)
                                     + " while no UWOP_SET_FPREG code establishes it");
    }

    DecodedWin64Unwind out;
    out.exceptionPhase = (flags & win64::kUnwFlagEHandler) != 0u;
    out.unwindPhase    = (flags & win64::kUnwFlagUHandler) != 0u;
    out.afterCodes     = static_cast<std::uint32_t>(codesEnd);
    out.cfi.initial       = entry;
    out.cfi.prologueEndPc = sizeOfProlog;
    cfa = entry.cfaOffset;
    bool framed = false;
    for (auto it = codes.rbegin(); it != codes.rend(); ++it) {
        Code const& c = *it;
        std::uint32_t const pc = c.codeOffset;
        switch (c.op) {
            case win64::kUwopPushNonvol: {
                if (framed) {
                    return refuse(DiagnosticCode::K_UnwindRuleUnrepresentable,
                                  "pushes a register at prologue byte " + std::to_string(pc)
                                      + ", after its frame register is set; the unwinder would "
                                        "read it at a stack pointer the body moves");
                }
                auto const r = generalReg(c.info, "UWOP_PUSH_NONVOL");
                if (!r.has_value()) return std::nullopt;
                cfa += 8;
                out.cfi.ops.push_back(CfiOp{pc, CfiOpKind::DefCfaOffset, {}, {}, cfa, 0u});
                out.cfi.ops.push_back(
                    CfiOp{pc, CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(*r), {}, -cfa, 0u});
                break;
            }
            case win64::kUwopAllocSmall:
            case win64::kUwopAllocLarge: {
                std::int64_t const n = c.op == win64::kUwopAllocSmall
                                           ? 8 * (static_cast<std::int64_t>(c.info) + 1)
                                           : 8 * static_cast<std::int64_t>(c.operand);
                cfa += n;
                if (!framed) {
                    out.cfi.ops.push_back(CfiOp{pc, CfiOpKind::DefCfaOffset, {}, {}, cfa, 0u});
                }
                break;
            }
            case win64::kUwopSetFpReg: {
                auto const r = generalReg(frameRegister, "UWOP_SET_FPREG");
                if (!r.has_value()) return std::nullopt;
                framed = true;
                out.cfi.ops.push_back(CfiOp{pc, CfiOpKind::DefCfa, CfiRegRef::physical(*r), {},
                                            cfa - 16 * static_cast<std::int64_t>(frameOffset), 0u});
                break;
            }
            case win64::kUwopSaveNonvol: {
                auto const r = generalReg(c.info, "UWOP_SAVE_NONVOL");
                if (!r.has_value()) return std::nullopt;
                out.cfi.ops.push_back(CfiOp{pc, CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(*r), {},
                                            8 * static_cast<std::int64_t>(c.operand) - base, 0u});
                break;
            }
            case win64::kUwopSaveXmm128: {
                if (regs.vectorAmbiguous[c.info] || !regs.vector[c.info].has_value()) {
                    return refuse(DiagnosticCode::K_UnwindRuleUnrepresentable,
                                  "UWOP_SAVE_XMM128 names vector register number "
                                      + std::to_string(c.info)
                                      + ", which the target document does not name as exactly "
                                        "one 16-byte vector register");
                }
                out.cfi.ops.push_back(CfiOp{pc, CfiOpKind::RegAtCfaOffset,
                                            CfiRegRef::physical(*regs.vector[c.info]), {},
                                            16 * static_cast<std::int64_t>(c.operand) - base, 0u});
                break;
            }
            default:
                break;   // every other operation was refused above
        }
    }
    return out;
}

} // namespace dss::link::format
