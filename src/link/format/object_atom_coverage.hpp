#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Shared relocatable-object READER substrate: prove that no defined symbol's
// body was silently dropped — D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-
// LABEL-NOT-ATOM.
//
// ★★★ THE FAILURE THIS CONVERTS, AND WHY IT NEEDED A GUARD RATHER THAN A FIX.
//
// The three relocatable-object readers (`elf/pe/macho::readRelocatableObject`)
// slice a section's bytes into ATOMS — one `AssembledFunction` / `AssembledData`
// per defined symbol that starts a body. Which symbols START a body is decided
// per format, and the COFF and Mach-O readers decide it by EXTERNAL-ness: a
// non-external defined section symbol is recorded as a bodiless `ModuleSymbol`
// on the reasoning that it is an interior `&&label` (a computed-goto block
// label), which is NOT an atom boundary and must never split the function that
// contains it. That reasoning is correct for a label and WRONG for a whole
// file-local (`static`) function — and nlist_64 / IMAGE_SYMBOL carry no size
// field, so the two shapes are literally the same three numbers. ELF is
// unaffected only because `st_size` lets it slice any STT_FUNC with a non-empty
// extent regardless of BINDING.
//
// The consequence divided into a loud half and a silent half:
//   * CALLED file-local function -> loud. Nothing defines the relocation's
//     target atom, so the cross-CU resolve fails with `K_SymbolUndefined`.
//   * UNCALLED file-local function -> SILENT. Its bytes belong to no atom, so
//     the merge never sees them and the emitted section is short — byte-
//     identical output across a source change that should have grown `.text`.
//     Only the ZERO-atom case was refused, so PARTIAL coverage passed without a
//     word.
//
// ⚠ THE CLASSIFICATION IS NOT FIXED HERE, DELIBERATELY. Making
// `buildCompoundIndex` index `m.symbols` is the prescription that anchor
// already records as REFUTED: a `ModuleSymbol` carries no body and no offset,
// so indexing it converts a loud error into a link that branches into whatever
// bytes happen to land at that address. The real fix needs the readers to
// distinguish a local function from a local label, and the Mach-O half of that
// needs a wire-format change. Until then this makes the byte loss IMPOSSIBLE TO
// MISS instead of impossible to see.
//
// ★★ WHY THE CHECK IS ONE IMPLEMENTATION AND NOT THREE.
//
// The convergence is the point: all three readers reduce a wildly different
// on-disk shape to the SAME `(sectionKey, byteOffset)` staging before they slice
// atoms out of it. The coverage question — "does some reconstructed atom own the
// byte this defined symbol starts at?" — is asked entirely in those neutral
// coordinates, so it needs no ELF/COFF/Mach-O knowledge and there is no
// `if (format == ...)` here or anywhere below. `sectionKey` is whatever section
// identity the caller already uses (an ELF `st_shndx`, a COFF 1-based ordinal, a
// Mach-O `n_sect`) widened to u32 and never interpreted; it only has to compare
// equal for two things in the same section. This mirrors
// `link/format/interior_block_symbol_va.hpp` on the WRITER side: three walkers,
// one helper, differing only in the values they already computed.
//
// ★ WHY THE TEST IS "COVERED BY AN ATOM" AND NOT "ATOMS COVER THE SECTION".
// The stronger phrasing would be wrong for ELF and would break the one reader
// that is already correct: inter-function ALIGNMENT PADDING in `.text` belongs to
// no function and is legitimately uncovered (`elf_object_reader.cpp`'s step-6.5
// gap atoms deliberately fill DATA sections only, for exactly this reason). A
// symbol-anchored test asks only about bytes somebody NAMED, which is precisely
// the set whose loss is a miscompile.
//
// ★ WHY A GENUINE INTERIOR LABEL PASSES. An `&&label` lies INSIDE the function
// that contains it, so the enclosing atom covers its offset and the check is
// silent — no escape hatch, no format test, no name matching. That is the whole
// discrimination: a real interior label is covered, a demoted whole function is
// not.
//
// ⚠ KNOWN BOUNDARY, stated so nobody reads more into a green than it carries.
// A file-local function that TRAILS an external one is NOT caught: the preceding
// atom runs to the end of the section (the COFF/Mach-O slicing rule when no
// further boundary symbol exists), so the trailing symbol's offset IS covered
// and its bytes ride along inside the wrong atom rather than vanishing. Those
// bytes still reach the image; the misattribution is a separate defect on the
// same anchor. Distinguishing that case from an interior label needs the size a
// non-external symbol does not carry — i.e. exactly the wire-format work this
// guard stands in for.

namespace dss::link::format {

// A DEFINED, section-backed symbol that a reader recorded WITHOUT a body.
//
// Callers stage ONLY symbols in a section whose `SectionKind` RESOLVED — real
// code/data the `AssembledModule` models. A symbol in an unmodeled metadata
// section (`.eh_frame`, `.debug$S`, `.pdata`) reconstructs no body BY DESIGN and
// is not a dropped body, so it is not a candidate. Callers also skip symbols
// that are section IDENTITIES rather than bodies (an ELF `STT_SECTION`, a COFF
// section-definition symbol).
struct BodilessDefinedSymbol {
    std::uint32_t sectionKey    = 0;  // caller's own section identity — opaque here
    std::uint64_t sectionOffset = 0;  // section-relative byte offset of the symbol
    // The symbol's declared extent, when the FORMAT declares one (ELF
    // `st_size`). `nullopt` when it does not (COFF `IMAGE_SYMBOL` and Mach-O
    // `nlist_64` have no size field) — which is itself the reason those two
    // readers cannot tell a local function from a local label.
    std::optional<std::uint64_t> declaredSize;
    std::string                  name;
    std::string                  sectionName;  // for the diagnostic only
};

// One reconstructed atom's byte extent within its section — the SAME
// `[start, start+len)` interval the reader already built to route relocation
// sites, restated in the neutral coordinates above.
struct ReconstructedAtomExtent {
    std::uint32_t sectionKey = 0;
    std::uint64_t start      = 0;
    std::uint64_t len        = 0;
};

// Fail loud NAMING the symbol when a staged bodiless defined symbol's offset is
// owned by no reconstructed atom of its section.
//
// A symbol the format declares to be ZERO bytes long is a MARKER, not a body —
// an ARM `$d`/`$x` mapping symbol, an empty object, a size-less text label — so
// it is skipped: there are no bytes to lose. A `nullopt` extent is NOT treated
// as zero; it is unknown, and an unknown-extent symbol outside every atom is the
// exact shape of the dropped file-local function.
//
// `readerName` prefixes the message the way each reader already prefixes its own
// refusals (e.g. "pe::readRelocatableObject"). Emits AT MOST ONE diagnostic (the
// first uncovered symbol) and returns false; returns true with no diagnostics
// when every staged symbol is covered — the common case, and the case for every
// object DSS itself writes on ELF.
[[nodiscard]] inline bool everyDefinedSymbolIsCoveredByAnAtom(
    std::span<BodilessDefinedSymbol const>   bodiless,
    std::span<ReconstructedAtomExtent const> atoms,
    std::string_view                         readerName,
    DiagnosticReporter&                      reporter) {
    for (auto const& sym : bodiless) {
        if (sym.declaredSize.has_value() && *sym.declaredSize == 0u) continue;
        bool covered = false;
        for (auto const& atom : atoms) {
            if (atom.sectionKey != sym.sectionKey) continue;
            if (sym.sectionOffset >= atom.start
                && sym.sectionOffset - atom.start < atom.len) {
                covered = true;
                break;
            }
        }
        if (covered) continue;
        dss::report(
            reporter, DiagnosticCode::F_ObjectReaderSymbolBodyDropped,
            DiagnosticSeverity::Error,
            std::string{readerName} + ": defined symbol '" + sym.name
                + "' lies at offset " + std::to_string(sym.sectionOffset)
                + " of section '" + sym.sectionName
                + "', which no reconstructed atom covers -- its bytes would be "
                  "silently dropped from the linked image. This reader treats a "
                  "non-external defined symbol as an interior block label rather "
                  "than an atom boundary, which is wrong for a file-local "
                  "(`static`) function; refusing to link a partially "
                  "reconstructed object. "
                  "D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM.");
        return false;
    }
    return true;
}

} // namespace dss::link::format
