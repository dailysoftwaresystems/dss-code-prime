#pragma once

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/byte_emit.hpp"

#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Shared in-place relocation applier — plan 14 LK6 cycle 1 closure
// hoisted across the 3 image-side walkers (ELF ET_EXEC, PE PE32+,
// Mach-O MH_EXECUTE). Closes simplifier #1 + #2 + architect O5
// 3-agent convergence from the LK2+LK3 cycle 2 review.
//
// LK6 cycle 2a generalisation: the caller now passes an
// **absolute symbol-VA map** (`symbolVa: SymbolId → uint64_t`)
// rather than a section-relative offset map + sectionVa add. This
// is the substrate shape that supports extern imports: an
// `ExternImport` symbol's VA points into `.idata` (PE) / `.got`
// (ELF) / __got (Mach-O) — a different section than the patch
// site's `.text`. With absolute VAs, the kernel doesn't care what
// section a target lives in.
//
// The kernel reads exclusively from `TargetRelocationInfo` (the LK6
// cycle 1 structured-formula triple: `pcRelative` + `addendBias` +
// `widthBytes`). Object-format-specific knowledge stays in each
// walker: the caller pre-computes `symbolVa` for both intra-module
// functions AND extern imports, plus `patchSectionVa` (the runtime
// VA of the section containing the patch sites — `.text` in cycle
// scope), and a `diagPrefix` that fronts every diagnostic with the
// format's identity.
//
// Patch formula dispatched on `TargetRelocationInfo::formulaKind`
// (D-LK6-1 closure — 2026-06-01):
//   * Linear:                value = S + A + (pcRelative ? -P : 0) + addendBias
//                            written `widthBytes` LE into `text` at the patch site.
//                            Covers x86_64 rel32 / abs32 / abs64 + ARM64 abs64.
//   * Aarch64Call26:         value = (S + A - P) >> 2; signed 26-bit;
//                            OR (value & 0x03FFFFFF) into the 32-bit
//                            instruction word at `text[patchOff..+4]`.
//   * Aarch64AdrPrelPgHi21:  value = ((S + A) >> 12) - (P >> 12);
//                            signed 21-bit; ADRP-split bits[1:0]→immlo[30:29],
//                            bits[20:2]→immhi[23:5].
//   * Aarch64AddAbsLo12:     value = (S + A) & 0xFFF;
//                            OR (value << 10) into ADD imm12 [21:10].
//
// where S = `symbolVa[target]`, A = `Relocation::addend`,
// P = `patchSectionVa + funcStart + Relocation::offset`.
//
// The non-Linear variants READ the 32-bit instruction word from
// `text` (the assembler emitted the opcode + zero immediate field),
// OR the computed value into the appropriate bitfield, and write
// the modified word back. This contrasts with Linear which simply
// OVERWRITES `widthBytes` bytes.
//
// Fail-loud guards (each surfaces as a single diagnostic and
// returns false):
//   * unresolved symbol target       → K_SymbolUndefined  (D-LK6-2 —
//       per-format closure: PE half closed at LK6 cycle 2a; ELF
//       half at D-LK6-4 / cycle 2b; Mach-O half at D-LK6-5 /
//       cycle 2c; the kernel itself is ready for all three.)
//   * kind unregistered (target)     → K_RelocationKindMismatch
//   * widthBytes == 0                → K_RelocationKindMismatch (D-LK6-1)
//   * rel.offset + width > fn.bytes  → K_RelocationKindMismatch
//   * value out of signed widthBytes → K_RelocationKindMismatch
//
// Format-side `fmt.relocationByKind(rel.kind)` is checked by the
// caller (it's format-specific and the diagnostic wording differs);
// the kernel here only checks the target schema's row.
//
// Per-format `symbolVa[sym]` population convention (cycle 2a+):
//   * Functions:       imageBase (PE) / 0 (ELF/MachO) + section VA
//                      + offset within `.text`.
//   * PE externs:      imageBase + .idata IAT slot RVA.
//   * ELF externs (cycle 2b, anchored D-LK6-4):
//       - R_X86_64_PLT32 / call-to-extern  → PLT stub absolute VA
//         (the PLT stub itself dispatches through the GOT slot).
//       - R_X86_64_GOTPCREL / load-from-GOT → GOT slot absolute VA.
//   * MachO externs (cycle 2c, anchored D-LK6-5):
//       - X86_64_RELOC_GOT_LOAD → `__got` slot absolute VA.

namespace dss::link::format {

[[nodiscard]] inline bool applyExecRelocations(
    std::vector<std::uint8_t>&  text,
    AssembledModule const&      module,
    std::span<std::uint64_t const> funcTextStart,
    std::unordered_map<SymbolId, std::uint64_t> const& symbolVa,
    TargetSchema const&         targetSchema,
    std::uint64_t               patchSectionVa,
    std::string_view            diagPrefix,
    DiagnosticReporter&         reporter) {
    using ::dss::link::format::detail::emit;
    auto const prefixStr = std::string{diagPrefix};

    if (text.empty()) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             prefixStr + ": `.text` is empty — every "
                           "AssembledFunction contributed zero bytes. "
                           "An exec with no instructions would crash "
                           "at entry.");
        return false;
    }

    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        auto const& fn = module.functions[fi];
        std::uint64_t const fnStart = funcTextStart[fi];
        for (auto const& rel : fn.relocations) {
            // (1) target resolves in caller-provided VA map
            //     (intra-module function + extern import slot)
            auto const tIt = symbolVa.find(rel.target);
            if (tIt == symbolVa.end()) {
                emit(reporter, DiagnosticCode::K_SymbolUndefined,
                     prefixStr + ": relocation target symbol #"
                           + std::to_string(rel.target.v)
                           + " is not defined by any AssembledFunction. "
                             "Extern / cross-module relocs are FFI / "
                             "dynamic linking — plan 14 §3.1 D-LK6-2.");
                return false;
            }
            // (2) reloc kind registered on target schema
            auto const* tri = targetSchema.relocationInfo(rel.kind);
            if (tri == nullptr) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     prefixStr + ": relocation kind "
                           + std::to_string(rel.kind.v)
                           + " has no TargetRelocationInfo on the "
                             "target schema.");
                return false;
            }
            // (3) widthBytes != 0 — every formula kind has a fixed
            //     write width; widthBytes==0 means the JSON declared
            //     no width (pre-D-LK6-1 placeholder) or the loader
            //     misconfigured the row.
            if (tri->widthBytes == 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     prefixStr + ": relocation '" + tri->name
                           + "' has widthBytes=0 — JSON loader must "
                             "set 4 or 8 for Linear and 4 for non-"
                             "Linear formula kinds.");
                return false;
            }
            // (4) patch fits inside originating function's bytes
            if (static_cast<std::uint64_t>(rel.offset) + tri->widthBytes
                    > fn.bytes.size()) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     prefixStr + ": relocation offset "
                           + std::to_string(rel.offset)
                           + " + widthBytes "
                           + std::to_string(static_cast<int>(tri->widthBytes))
                           + " exceeds function symbol #"
                           + std::to_string(fn.symbol.v) + "'s size "
                           + std::to_string(fn.bytes.size())
                           + " — would overrun adjacent function.");
                return false;
            }
            // (5) compute & apply per formula kind. S is absolute
            //     (caller pre-computed the symbol VA, which may live
            //     in `.text` for functions or `.idata` / GOT / __got
            //     for externs); P is the patch site VA, always inside
            //     the `patchSectionVa` section in cycle 2a scope.
            std::uint64_t const patchOff = fnStart + rel.offset;
            std::uint64_t const S = tIt->second;
            std::int64_t  const A = rel.addend;
            std::uint64_t const P = patchSectionVa + patchOff;

            // Range-check helper for signed-N-bit fit.
            auto const fitsSignedNBits = [&](std::int64_t v, int bits) -> bool {
                std::int64_t const sMax = (std::int64_t{1} << (bits - 1)) - 1;
                std::int64_t const sMin = -sMax - 1;
                return v >= sMin && v <= sMax;
            };
            // Read / write the 32-bit instruction word at patchOff
            // via `byte_emit.hpp` (post-fold #1 — hoisted to share
            // with future positional patchers).
            auto const readInst32  = [&] { return detail::readU32LEAt(text, patchOff); };
            auto const writeInst32 = [&](std::uint32_t inst) { detail::writeU32LEAt(text, patchOff, inst); };
            // Reject non-Linear pre-OR contamination (the bitfield to
            // be written must be zero in the assembler-emitted base
            // instruction). Without this, an assembler that mistakenly
            // emitted a partial immediate would silently corrupt the
            // patched instruction.
            auto const rejectIfBitfieldDirty = [&](std::uint32_t inst,
                                                    std::uint32_t mask) -> bool {
                if ((inst & mask) != 0) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         prefixStr + ": relocation '" + tri->name
                            + "' base instruction at patch site has "
                              "non-zero bits in the immediate field — "
                              "assembler must emit the opcode with the "
                              "immediate cleared (the linker OR's the "
                              "computed value in).");
                    return true;
                }
                return false;
            };

            switch (tri->formulaKind) {
                case RelocFormulaKind::Linear: {
                    std::int64_t const value =
                        static_cast<std::int64_t>(S) + A
                        + (tri->pcRelative ? -static_cast<std::int64_t>(P) : 0)
                        + tri->addendBias;
                    if (tri->widthBytes < 8) {
                        if (!fitsSignedNBits(value, 8 * tri->widthBytes)) {
                            emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                                 prefixStr + ": relocation '" + tri->name
                                     + "' value " + std::to_string(value)
                                     + " does not fit signed in widthBytes="
                                     + std::to_string(static_cast<int>(tri->widthBytes))
                                     + " — would silently truncate.");
                            return false;
                        }
                    }
                    auto const uVal = static_cast<std::uint64_t>(value);
                    for (std::uint8_t b = 0; b < tri->widthBytes; ++b) {
                        text[patchOff + b] =
                            static_cast<std::uint8_t>((uVal >> (8u * b)) & 0xFFu);
                    }
                    break;
                }
                case RelocFormulaKind::Aarch64Call26: {
                    // value = (S + A - P) >> 2; signed 26 bits.
                    std::int64_t const delta =
                        static_cast<std::int64_t>(S) + A
                        - static_cast<std::int64_t>(P);
                    if ((delta & 0x3) != 0) {
                        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                             prefixStr + ": relocation '" + tri->name
                                 + "' (S+A-P)=" + std::to_string(delta)
                                 + " is not 4-byte aligned — ARM64 "
                                   "branch target must be word-aligned.");
                        return false;
                    }
                    std::int64_t const value = delta >> 2;
                    if (!fitsSignedNBits(value, 26)) {
                        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                             prefixStr + ": relocation '" + tri->name
                                 + "' shifted value " + std::to_string(value)
                                 + " does not fit signed 26-bit — "
                                   "branch out of ±128 MiB range.");
                        return false;
                    }
                    auto const inst = readInst32();
                    if (rejectIfBitfieldDirty(inst, 0x03FFFFFFu)) return false;
                    auto const bits = static_cast<std::uint32_t>(value) & 0x03FFFFFFu;
                    writeInst32(inst | bits);
                    break;
                }
                case RelocFormulaKind::Aarch64AdrPrelPgHi21: {
                    // value = ((S+A) >> 12) - (P >> 12); signed 21 bits.
                    std::int64_t const SA = static_cast<std::int64_t>(S) + A;
                    std::int64_t const value =
                        (SA >> 12) - (static_cast<std::int64_t>(P) >> 12);
                    if (!fitsSignedNBits(value, 21)) {
                        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                             prefixStr + ": relocation '" + tri->name
                                 + "' page-pair value " + std::to_string(value)
                                 + " does not fit signed 21-bit — "
                                   "ADRP target out of ±4 GiB page range.");
                        return false;
                    }
                    // ADRP encoding: immlo at bits [30:29] holds value[1:0];
                    // immhi at bits [23:5] holds value[20:2].
                    auto const v = static_cast<std::uint32_t>(value) & 0x1FFFFFu;
                    std::uint32_t const immlo = (v & 0x3u) << 29;
                    std::uint32_t const immhi = ((v >> 2) & 0x7FFFFu) << 5;
                    std::uint32_t const mask  = (0x3u << 29) | (0x7FFFFu << 5);
                    auto const inst = readInst32();
                    if (rejectIfBitfieldDirty(inst, mask)) return false;
                    writeInst32(inst | immlo | immhi);
                    break;
                }
                case RelocFormulaKind::Aarch64AddAbsLo12: {
                    // value = (S+A) & 0xFFF; ADD imm12 at bits[21:10].
                    // Reject S+A outside [0, UINT32_MAX]: ADD_ABS_LO12_NC
                    // requires a non-negative absolute address that fits
                    // in 32 bits (the paired ADRP companion computes the
                    // page base from the same 32-bit space). Negative
                    // SA cast to uint32_t produces garbage low-12;
                    // SA > UINT32_MAX silently truncates the high bits
                    // with no ADRP-companion correspondence. (architect
                    // Q6 post-fold #1 + silent-failure audit CRITICAL-1
                    // post-fold #2.)
                    std::int64_t const SA = static_cast<std::int64_t>(S) + A;
                    if (SA < 0
                     || SA > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
                        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                             prefixStr + ": relocation '" + tri->name
                                 + "' got S+A=" + std::to_string(SA)
                                 + " — ADD_ABS_LO12_NC requires "
                                   "0 ≤ S+A ≤ UINT32_MAX (paired with an "
                                   "ADRP that computes the page base from "
                                   "the same 32-bit space; out-of-range "
                                   "values would silently truncate to "
                                   "garbage low-12).");
                        return false;
                    }
                    auto const v = static_cast<std::uint32_t>(SA) & 0xFFFu;
                    std::uint32_t const bits = v << 10;
                    std::uint32_t const mask = 0xFFFu << 10;
                    auto const inst = readInst32();
                    if (rejectIfBitfieldDirty(inst, mask)) return false;
                    writeInst32(inst | bits);
                    break;
                }
            }
        }
    }
    return true;
}

} // namespace dss::link::format
