#pragma once

#include "asm/asm.hpp"                     // AssembledModule, Relocation
#include "core/types/extern_import.hpp"    // ExternImport, SymbolBinding
#include "core/types/target_schema.hpp"    // TargetRelocationInfo, relocFormulaFacts
#include "link/branch_reloc_geometry.hpp"  // branchRelocGeometry
#include "link/branch_sites.hpp"           // BranchSites, isBranchReference
#include "link/format/byte_emit.hpp"       // readU32LEAt / writeU32LEAt
#include "link/object_format_schema.hpp"   // WeakResolvedToNothing, WeakNullReference

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ★★★ WHAT A REFERENCE NAMING A WEAK SYMBOL RESOLVED TO NOTHING COMPUTES
// (P69, lane `lm`, D-LK-WEAK-UNDEFINED-SYMBOL-NAMED-DIRECTLY-IS-NOT-ADDRESS-ZERO).
//
// The reference gate (`rejectOrDropUnreferencedExterns`, link/linker.cpp, reading
// `link/extern_reference_gate.hpp`) decides WHICH rows an image binds to nothing:
// a referenced WEAK symbol no linked unit defines and no library binds, on an
// image that cannot carry an undefined symbol. This header decides what each
// REFERENCE to such a symbol computes, and it is one statement for every
// producer — DSS's own code and data, a foreign object, a `.s` unit.
//
// ★ THE SYMBOL HAS TWO FACES, AND THE ONE A REFERENCE SEES IS A FACT OF HOW IT
// READS THE SYMBOL. The VALUE of nothing is 0 (the gABI: "an undefined weak symbol
// has a zero value"; AAELF64 "Weak References": zero for an absolute relocation).
//   * A reference that READS THROUGH A SLOT — DSS's own data references (its
//     unit's row says so, `ExternImport::readThroughSlot`) and every GOT load —
//     reads a slot holding 0: the null slot the gate gives the row, and a GOT
//     slot the GOT lowering mints holding 0 (`resolvedToNothing`).
//   * A reference that NAMES THE SYMBOL DIRECTLY computes its field from the
//     value. Until P69 the null slot WAS the symbol, so such a field computed the
//     slot's own, non-null address: ✔MEASURED 2026-10-07 at the merged P69 tree,
//     `extern int wd __attribute__((weak)); int *p = &wd;` linked on all five
//     images and the PE program returned 1 (`p != NULL`) where gcc, clang, GNU ld,
//     ld.lld and lld-link all give NULL — a silent wrong answer in DSS's own C.
// What a direct reference's field can hold is the image's and its target ABI's
// fact, declared per FIELD CLASS by the image document (`weakResolvedToNothing`,
// whose comments carry the per-linker measurements):
//   * ABSOLUTE   (`S + A`, a plain Linear field): a link-time constant, and not an
//                address the loader moves, so THE LINK WRITES IT HERE and drops
//                the relocation — no writer ever sees it and no load-time fix-up
//                (an ELF RELATIVE row, a PE base relocation, a Mach-O rebase) is
//                made for it, in any image;
//   * PCRELATIVE (`S + A - P`, not a branch — and an addressing-mode HALF, an ADD
//                or LDR page offset, which belongs with the page reference it
//                completes): a constant only where the writer places the image at
//                its link address, so the reference is retargeted to a symbol
//                whose ADDRESS IS 0 (`AssembledModule::nullAddressSymbols`) and
//                that writer resolves it (the loader admits `zero` only there,
//                `ObjectFormatBackend::writesNullAddressReferences`);
//   * BRANCH     (a call or a jump): `zero` as above, or `nextInstruction` — the
//                link writes the branch to the instruction after it (AAELF64, for a
//                system without dynamic pre-emption — which an image that binds the
//                symbol to nothing at link time is);
//   * an IMAGE-RELATIVE field (an RVA) can never hold address 0: refused.
// A class the document answers `refused` — or a document that declares no block —
// is refused BY NAME.
namespace dss::linker {

enum class WeakNullReferenceClass : std::uint8_t {
    ThroughASlot,   // reads a slot holding 0 — nothing to decide
    NotJudged,      // a kind the target does not declare (the kind unifier's) or a
                    // thread-local one (the TLS gate's)
    ImageRelative,  // an RVA
    Absolute,
    PcRelative,
    Branch,
};

[[nodiscard]] constexpr char const*
weakNullReferenceClassName(WeakNullReferenceClass c) noexcept {
    switch (c) {
        case WeakNullReferenceClass::ThroughASlot:  return "a reference through a slot";
        case WeakNullReferenceClass::NotJudged:     return "a reference this rule does not judge";
        case WeakNullReferenceClass::ImageRelative: return "an image-relative (RVA) field";
        case WeakNullReferenceClass::Absolute:      return "an absolute field";
        case WeakNullReferenceClass::PcRelative:    return "a PC-relative field that is not a branch";
        case WeakNullReferenceClass::Branch:        return "a branch";
    }
    return "an unclassified reference";
}

// The class of ONE relocation naming a row the image resolves to nothing.
// `inCode` = the relocation patches a function's bytes (`code`); a data item's
// pointer is never a branch and never a slot read. `rowReadsThroughSlot` says a
// unit of the link reads the name through its slot (`WeakSlotReaders` below, from
// `ExternImport::readThroughSlot`, which MIR→LIR stamps where it chose the slot
// shape and an object reader never sets); the caller has already refused a name
// another unit also names directly.
[[nodiscard]] inline WeakNullReferenceClass
classifyWeakNullReference(Relocation const&             rel,
                          std::span<std::uint8_t const> code,
                          bool                          inCode,
                          bool                          rowReadsThroughSlot,
                          TargetSchema const&           target,
                          ObjectFormatSchema const&     format,
                          BranchSites const&            sites) {
    auto const* tri = target.relocationInfo(rel.kind);
    if (tri == nullptr || tri->tls) return WeakNullReferenceClass::NotJudged;
    if (relocFormulaFacts(tri->formulaKind).isGotSlotRelative) {
        return WeakNullReferenceClass::ThroughASlot;
    }
    if (inCode && rowReadsThroughSlot) return WeakNullReferenceClass::ThroughASlot;
    if (tri->imageRelative) return WeakNullReferenceClass::ImageRelative;
    if (inCode && isBranchReference(rel, code, target, format, sites)) {
        return WeakNullReferenceClass::Branch;
    }
    if (tri->formulaKind == RelocFormulaKind::Linear && !tri->pcRelative) {
        return WeakNullReferenceClass::Absolute;
    }
    return WeakNullReferenceClass::PcRelative;
}

// The image's answer for a class; `refused` for every class the document does
// not let reach the value (and for the two this rule never decides, which the
// caller does not ask about).
[[nodiscard]] constexpr WeakNullReference
weakNullAnswer(WeakResolvedToNothing const& answers, WeakNullReferenceClass c) noexcept {
    switch (c) {
        case WeakNullReferenceClass::Absolute:   return answers.absolute;
        case WeakNullReferenceClass::PcRelative: return answers.pcRelative;
        case WeakNullReferenceClass::Branch:     return answers.branch;
        case WeakNullReferenceClass::ImageRelative:
        case WeakNullReferenceClass::ThroughASlot:
        case WeakNullReferenceClass::NotJudged:
            break;
    }
    return WeakNullReference::Refused;
}

// Is this answer one THE LINK writes as a constant (rather than one a writer
// resolves against a null-address symbol)?
[[nodiscard]] constexpr bool
linkWritesWeakNullConstant(WeakNullReferenceClass c, WeakNullReference a) noexcept {
    return (c == WeakNullReferenceClass::Absolute && a == WeakNullReference::Zero)
        || (c == WeakNullReferenceClass::Branch && a == WeakNullReference::NextInstruction);
}

// Write, into `bytes`, the field of a reference the link resolves to a CONSTANT
// (`linkWritesWeakNullConstant`), and say why not when it cannot:
//   * an ABSOLUTE field from S = 0 holds A plus the row's declared bias, written
//     exactly as the shared kernel writes a Linear field (`applyExecRelocations`):
//     little-endian, `widthBytes` wide, a narrower field checked to hold the value
//     as a signed one;
//   * a BRANCH to the next instruction: its displacement is the instruction word's
//     own width (`widthBytes`, 4 for every instruction-word formula), scaled and
//     placed by the target's branch geometry (`branchRelocGeometry`) into a field
//     the assembler left clear — the one source the kernel and the veneer pass
//     read too, so the three cannot disagree on the field's shape.
// Returns false with `why` set; nothing is written then.
[[nodiscard]] inline bool
writeWeakNullConstant(std::vector<std::uint8_t>&  bytes,
                      Relocation const&           rel,
                      TargetRelocationInfo const& tri,
                      WeakNullReferenceClass      cls,
                      std::string&                why) {
    auto const fitsSigned = [](std::int64_t v, unsigned bits) {
        std::int64_t const hi = (std::int64_t{1} << (bits - 1)) - 1;
        return v >= -hi - 1 && v <= hi;
    };
    if (cls == WeakNullReferenceClass::Absolute) {
        unsigned const width = tri.widthBytes;
        if (width == 0 || width > 8) {
            why = "its row declares no field width";
            return false;
        }
        if (static_cast<std::uint64_t>(rel.offset) + width > bytes.size()) {
            why = "the field runs past the bytes that carry it";
            return false;
        }
        std::int64_t const value = rel.addend + tri.addendBias;
        if (width < 8 && !fitsSigned(value, 8u * width)) {
            why = "the field cannot hold its own addend";
            return false;
        }
        auto const u = static_cast<std::uint64_t>(value);
        for (unsigned b = 0; b < width; ++b) {
            bytes[rel.offset + b] = static_cast<std::uint8_t>((u >> (8u * b)) & 0xFFu);
        }
        return true;
    }
    if (cls == WeakNullReferenceClass::Branch) {
        auto const g = ::dss::link::branchRelocGeometry(tri.formulaKind);
        if (g.fieldBits == 0 || tri.widthBytes != 4) {
            why = "its field is not an instruction-word branch whose displacement "
                  "the target's branch geometry places";
            return false;
        }
        if (static_cast<std::uint64_t>(rel.offset) + 4u > bytes.size()) {
            why = "the instruction word runs past the bytes that carry it";
            return false;
        }
        std::int64_t const delta = tri.widthBytes;   // the next instruction
        if ((delta & ((std::int64_t{1} << g.scaleLog2) - 1)) != 0) {
            why = "the next instruction is not at a displacement its field scales to";
            return false;
        }
        std::int64_t const value = delta >> g.scaleLog2;
        if (!fitsSigned(value, g.fieldBits)) {
            why = "its field cannot reach the next instruction";
            return false;
        }
        std::uint32_t const mask = (g.fieldBits >= 32u)
                                       ? 0xFFFFFFFFu
                                       : (((1u << g.fieldBits) - 1u) << g.lsb);
        std::uint32_t const inst = ::dss::link::format::detail::readU32LEAt(bytes, rel.offset);
        if ((inst & mask) != 0) {
            why = "the assembler did not leave the branch's displacement field clear";
            return false;
        }
        ::dss::link::format::detail::writeU32LEAt(
            bytes, rel.offset,
            inst | ((static_cast<std::uint32_t>(value) << g.lsb) & mask));
        return true;
    }
    why = "it is not a field the link resolves to a constant";
    return false;
}

// HOW THE LINK'S INPUT UNITS READ EACH WEAK NAME, asked before the merge folds
// their rows. `ExternImport::readThroughSlot` is a statement of ONE unit — the one
// MIR→LIR lowered — and the merge keeps one row per name, carrying the flag of
// whichever unit landed first. A merged row therefore cannot say how a code
// reference from ANOTHER unit reads the symbol, and either answer misreads one of
// them once the symbol is resolved to nothing: a slot reader taken for a direct
// reference loads through address 0, and a direct reference taken for a slot read
// computes the slot's own non-null address. So the gate reads these instead:
//   * `throughSlot` — the names some unit reads THROUGH ITS SLOT (its row says
//     so). Every non-GOT code reference to such a name is a slot read, unless
//   * `twoWays`     — another unit also NAMES it DIRECTLY from code (a non-GOT,
//     non-thread-local code relocation of a row that does not read through a
//     slot). No merged row can then say which a code reference is, so the gate
//     refuses every non-GOT code reference to such a name the image resolves to
//     nothing, by name, rather than give one unit the other's face.
// A GOT load reads a slot whichever unit makes it, and a data item's relocation
// names the symbol directly whichever unit carries it, so neither is ambiguous
// and neither is counted.
struct WeakSlotReaders {
    std::unordered_set<std::string> throughSlot;
    std::unordered_set<std::string> twoWays;
};

[[nodiscard]] inline WeakSlotReaders
weakSlotReadersOf(std::span<AssembledModule const> units, TargetSchema const& target) {
    WeakSlotReaders out;
    std::unordered_set<std::string> direct;
    for (auto const& m : units) {
        std::unordered_map<std::uint32_t, ExternImport const*> rows;
        for (auto const& ext : m.externImports) {
            if (ext.binding != SymbolBinding::Weak || ext.mangledName.empty()) continue;
            rows.emplace(ext.symbol.v, &ext);
            if (ext.readThroughSlot) out.throughSlot.insert(ext.mangledName);
        }
        if (rows.empty()) continue;
        for (auto const& fn : m.functions) {
            for (auto const& rel : fn.relocations) {
                auto const it = rows.find(rel.target.v);
                if (it == rows.end() || it->second->readThroughSlot) continue;
                auto const* tri = target.relocationInfo(rel.kind);
                if (tri == nullptr || tri->tls) continue;
                if (relocFormulaFacts(tri->formulaKind).isGotSlotRelative) continue;
                direct.insert(it->second->mangledName);
            }
        }
    }
    for (auto const& n : out.throughSlot) {
        if (direct.contains(n)) out.twoWays.insert(n);
    }
    return out;
}

}  // namespace dss::linker
