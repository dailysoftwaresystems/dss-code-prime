#pragma once

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/byte_emit.hpp"

#include <cstdint>
#include <format>
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
// The kernel reads exclusively from `TargetRelocationInfo` (the LK6
// cycle 1 structured-formula triple: `pcRelative` + `addendBias` +
// `widthBytes`). Object-format-specific knowledge stays in each
// walker: the caller passes `sectionVa` (the runtime VA of the
// section's base — ELF: `secText->virtualAddress`; PE: `ImageBase +
// RVA`; Mach-O: `secText.virtualAddress`) and a `diagPrefix` that
// fronts every diagnostic with the format's identity.
//
// Patch formula (uniform across all 3 formats):
//   value = S + A + (pcRelative ? -P : 0) + addendBias
// where S = `sectionVa + offset(target)`, A = `Relocation::addend`,
// P = `sectionVa + funcStart + Relocation::offset`. The `value` is
// written `widthBytes` LE into `text` at the patch site.
//
// Fail-loud guards (each surfaces as a single diagnostic and
// returns false):
//   * extern symbol target           → K_SymbolUndefined  (D-LK6-2)
//   * kind unregistered (target)     → K_RelocationKindMismatch
//   * widthBytes == 0                → K_RelocationKindMismatch (D-LK6-1)
//   * rel.offset + width > fn.bytes  → K_RelocationKindMismatch
//   * value out of signed widthBytes → K_RelocationKindMismatch
//
// Format-side `fmt.relocationByKind(rel.kind)` is checked by the
// caller (it's format-specific and the diagnostic wording differs);
// the kernel here only checks the target schema's row.

namespace dss::link::format {

[[nodiscard]] inline bool applyExecRelocations(
    std::vector<std::uint8_t>&  text,
    AssembledModule const&      module,
    std::span<std::uint64_t const> funcTextStart,
    std::unordered_map<SymbolId, std::uint64_t> const& textOffsetBySym,
    TargetSchema const&         targetSchema,
    std::uint64_t               sectionVa,
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
            // (1) intra-module target
            auto const tIt = textOffsetBySym.find(rel.target);
            if (tIt == textOffsetBySym.end()) {
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
            // (3) widthBytes != 0 (non-linear formulas — D-LK6-1)
            if (tri->widthBytes == 0) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     prefixStr + ": relocation '" + tri->name
                           + "' has no structured-formula widthBytes "
                             "— the formula needs a non-linear "
                             "transform (mask / shift / bit-field) "
                             "the LK6 cycle 1 linear shape cannot "
                             "express. Anchored: plan 14 §3.1 "
                             "D-LK6-1.");
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
            // (5) compute value
            std::uint64_t const patchOff = fnStart + rel.offset;
            std::uint64_t const S = sectionVa + tIt->second;
            std::int64_t  const A = rel.addend;
            std::uint64_t const P = sectionVa + patchOff;
            std::int64_t  const value =
                static_cast<std::int64_t>(S) + A
                + (tri->pcRelative ? -static_cast<std::int64_t>(P) : 0)
                + tri->addendBias;
            // (6) range-check signed-fit in widthBytes
            if (tri->widthBytes < 8) {
                std::int64_t const sMax =
                    (std::int64_t{1} << (8 * tri->widthBytes - 1)) - 1;
                std::int64_t const sMin = -sMax - 1;
                if (value < sMin || value > sMax) {
                    emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                         prefixStr + ": relocation '" + tri->name
                             + "' value " + std::to_string(value)
                             + " does not fit signed in widthBytes="
                             + std::to_string(static_cast<int>(tri->widthBytes))
                             + " (range [" + std::to_string(sMin)
                             + ", " + std::to_string(sMax) + "]) — "
                               "would silently truncate.");
                    return false;
                }
            }
            // (7) write `widthBytes` LE
            auto const uVal = static_cast<std::uint64_t>(value);
            for (std::uint8_t b = 0; b < tri->widthBytes; ++b) {
                text[patchOff + b] =
                    static_cast<std::uint8_t>((uVal >> (8u * b)) & 0xFFu);
            }
        }
    }
    return true;
}

} // namespace dss::link::format
