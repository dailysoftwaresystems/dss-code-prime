#pragma once

// ★★★ WHERE A SECTION-RELATIVE REFERENCE BINDS — ONE RULE FOR EVERY READER.
//
// An assembler reduces a reference to a file-local object to "the SECTION's
// symbol + an offset". gas does it for every local symbol on ELF, and for
// EVERY same-file symbol, global ones included, on PE/COFF (✔MEASURED
// 2026-09-23, mingw gcc 13 -O2: `counter = 5` with a GLOBAL `counter` is
// `IMAGE_REL_AMD64_REL32 .data` with 0xc in place). DSS reads a section as
// symbol-bounded atoms, so such a reference has to be rebound to ONE atom with
// a residual addend. Doing that keeps the image's value identical:
//
//     S + A  ==  S' + A'   where  S' = the atom's start,  A' = S + A - S'
//
// ★★ WHICH ATOM IS UNKNOWABLE FROM THE RECORD, and that is why the section must
// be a UNIT (`inputSectionPlacement`, `InputSectionSlice`). An x86-64
// instruction that carries an immediate after its displacement folds the
// immediate's width into the addend, so `movl $5, x(%rip)` records "x - 4".
// That is the same record as a plain load from the four bytes before `x`. The
// reference therefore names an address somewhere in [S + A, S + A + 4], and an
// atom boundary can fall inside that window. Binding to either atom is exact
// only if the two keep their offset from each other. The format guarantees
// exactly that, because an ELF or PE/COFF input section is never split. So the
// rule here is safe only because the section is laid out as one block.
// ✔MEASURED 2026-09-23 before that was true: gcc -O2 `static int y, x` stores
// bound to the preceding object, the merge moved the objects apart, and the
// program ran to 33 where gcc's link runs to 42.
//
// THE RULE:
//   1. an atom CONTAINS the search offset: bind to it (a function atom first,
//      then a data atom, the order the readers always used);
//   2. none does, but the section is a unit and has atoms: bind to its
//      LOWEST atom. That covers a store to the section's first object
//      (search offset -4: ✔MEASURED, refused LOUD before), an end-of-array
//      marker, and bss padding. The residual may be negative or run past the
//      atom, and it is exact because every atom keeps its offset;
//   3. otherwise refuse, saying why.
//
// ★★ WHICH TARGETS ARE SECTION-RELATIVE — a section's own symbol is only the
// shape gas writes. The rule applies to ANY relocation target that is DEFINED
// in a section and owns no reconstructed body: a section-definition symbol, a
// size-0 marker, and an INTERIOR LABEL — a block label inside a function. DSS's
// own COFF and Mach-O writers name every jump-table slot and every `&&label`
// through such a label (class STATIC type 0 / N_ALT_ENTRY in the code section),
// and cl.exe names its `$LN` case targets with class LABEL. Each reader decides
// "defined in a section, and no atom" in its own symbol vocabulary; everything
// after that decision — the search offset, the atom, the residual — is this
// file's (D-LINK-OBJECT-READERS-DROP-INTERIOR-SYMBOL-OFFSET: until P69 the COFF
// reader rebound only section-definition symbols and the Mach-O reader nothing,
// so a DSS static library holding a dense switch could not be linked into a pe
// or Mach-O image — `K_SymbolUndefined` once per table slot).

#include "link/object_format_schema.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace dss::link::format {

// ★ THE ONE STATEMENT OF WHEN AN OBJECT's SECTIONS ARE UNITS. `unit`: always.
// `subsectionsWhenDeclared`: unless the OBJECT declares its sections divisible
// at symbols. Only Mach-O's MH_SUBSECTIONS_VIA_SYMBOLS does that, and its
// reader passes the flag. An ELF or COFF object declares nothing, so under
// either rule its sections are units.
[[nodiscard]] constexpr bool inputSectionsAreUnits(
        InputSectionPlacement placement,
        bool                  objectDeclaresSubsections) noexcept {
    return placement == InputSectionPlacement::Unit || !objectDeclaresSubsections;
}

// One reconstructed atom of the target section: its [start, start + len) and
// the index of the AssembledFunction or AssembledData that holds its bytes.
struct SectionAtomSpan {
    std::uint64_t start  = 0;
    std::uint64_t len    = 0;
    std::size_t   outIdx = 0;
};

// Where a section-relative reference binds.
struct SectionRelativeBinding {
    std::size_t  outIdx     = 0;      // into `functions` or `dataItems`
    bool         isFunction = false;  // which of the two `outIdx` indexes
    std::int64_t residual   = 0;      // the addend, from that atom's start
};

// The atoms one section reconstructed, in the shape the rule reads. Every reader
// keeps its atoms per section as records carrying `start`, `len` and `outIdx`;
// `bySection` maps the reader's own section key to a vector of them.
template <class IntervalsBySection, class SectionKey>
[[nodiscard]] std::vector<SectionAtomSpan>
sectionAtomSpans(IntervalsBySection const& bySection, SectionKey key) {
    std::vector<SectionAtomSpan> spans;
    if (auto const it = bySection.find(key); it != bySection.end()) {
        for (auto const& v : it->second) {
            spans.push_back(SectionAtomSpan{v.start, v.len, v.outIdx});
        }
    }
    return spans;
}

// The offset that CHOOSES the atom: the reference's target offset in its
// section, `bindBase` — except for a DATA-section PC-relative SELF-reference (a
// relative jump table), whose displacement is based at its own table, i.e. at
// the referencing atom's start, not at the next instruction. Its target is then
// `bindBase + addendBias - offsetInReferencingAtom`. The residual is still
// measured from `bindBase` whatever atom is chosen.
[[nodiscard]] constexpr std::int64_t
sectionRelativeSearchOffset(std::int64_t bindBase, bool pcRelative,
                            bool referenceIsInData, std::int64_t addendBias,
                            std::int64_t offsetInReferencingAtom) noexcept {
    if (pcRelative && referenceIsInData) {
        return bindBase + addendBias - offsetInReferencingAtom;
    }
    return bindBase;
}

// `bindBase`: the section offset the reference names (section symbol value +
// recovered addend). The residual is measured from it, whatever atom is chosen.
// `searchOffset`: the offset used to CHOOSE the atom — `bindBase`, except for a
// data-section PC-relative self-reference, whose base is its own table.
// `sectionIsAUnit`: the section's atoms keep their offsets from each other, so
// a reference may bind through any of them (rule 2).
[[nodiscard]] inline std::expected<SectionRelativeBinding, std::string>
bindSectionRelativeReference(std::span<SectionAtomSpan const> functionAtoms,
                             std::span<SectionAtomSpan const> dataAtoms,
                             std::int64_t                     bindBase,
                             std::int64_t                     searchOffset,
                             bool                             sectionIsAUnit) {
    auto containing = [&](std::span<SectionAtomSpan const> atoms)
        -> SectionAtomSpan const* {
        if (searchOffset < 0) return nullptr;
        auto const off = static_cast<std::uint64_t>(searchOffset);
        for (auto const& a : atoms) {
            if (off >= a.start && off < a.start + a.len) return &a;
        }
        return nullptr;
    };
    auto bindTo = [&](SectionAtomSpan const& a, bool isFunction) {
        return SectionRelativeBinding{
            a.outIdx, isFunction, bindBase - static_cast<std::int64_t>(a.start)};
    };

    if (SectionAtomSpan const* f = containing(functionAtoms)) return bindTo(*f, true);
    if (SectionAtomSpan const* d = containing(dataAtoms)) return bindTo(*d, false);

    if (!sectionIsAUnit) {
        return std::unexpected(std::string{
            "lands in no reconstructed atom, and this section's atoms may be "
            "placed independently, so no atom can stand in for the offset"});
    }
    auto lowest = [](std::span<SectionAtomSpan const> atoms) -> SectionAtomSpan const* {
        auto const it = std::min_element(
            atoms.begin(), atoms.end(),
            [](SectionAtomSpan const& a, SectionAtomSpan const& b) {
                return a.start < b.start;
            });
        return it == atoms.end() ? nullptr : &*it;
    };
    SectionAtomSpan const* const lowFn   = lowest(functionAtoms);
    SectionAtomSpan const* const lowData = lowest(dataAtoms);
    if (lowFn != nullptr && (lowData == nullptr || lowFn->start <= lowData->start)) {
        return bindTo(*lowFn, true);
    }
    if (lowData != nullptr) return bindTo(*lowData, false);
    return std::unexpected(std::string{
        "lands in a section that reconstructed no atom at all -- there is no "
        "body to bind the reference through (a reference into unmodeled "
        "section content)"});
}

} // namespace dss::link::format
