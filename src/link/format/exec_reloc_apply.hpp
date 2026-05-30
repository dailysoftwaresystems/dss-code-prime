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
// Patch formula (uniform across all 3 formats):
//   value = S + A + (pcRelative ? -P : 0) + addendBias
// where S = `symbolVa[target]`, A = `Relocation::addend`,
// P = `patchSectionVa + funcStart + Relocation::offset`. The
// `value` is written `widthBytes` LE into `text` at the patch site.
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
            // (5) compute value — S is absolute (caller pre-computed
            //     the symbol VA, which may live in `.text` for
            //     functions or `.idata` / GOT / __got for externs);
            //     P is the patch site VA, always inside the
            //     `patchSectionVa` section in cycle 2a scope.
            std::uint64_t const patchOff = fnStart + rel.offset;
            std::uint64_t const S = tIt->second;
            std::int64_t  const A = rel.addend;
            std::uint64_t const P = patchSectionVa + patchOff;
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
