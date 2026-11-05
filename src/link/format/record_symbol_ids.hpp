#pragma once

#include "core/types/strong_ids.hpp"  // SymbolId

#include <cstdint>

// ═══ THE ID OF A SYMBOL-TABLE RECORD ═══════════════════════════════════════
//    D-LK-OBJECT-READERS-GAVE-RECORD-ZERO-THE-INVALID-SYMBOL-ID
//
// ★★★ THE FAILURE. An object reader names every symbol it reads by the index
// of its record in the object's symbol table, and index 0 is not an id: it is
// `SymbolId{}`, the INVALID id — the mark of an unlabelled data item, which
// every object writer and the image data layout pass over when they give
// symbols their records and their addresses (`asm.hpp`, "anonymous data"). An
// ELF table reserves record 0 for the null symbol, so its reader never met the
// collision. A COFF and a Mach-O table do not: record 0 is an ordinary symbol
// there, and the datum it names was read under the invalid id, taken for an
// unlabelled one, and written without its name.
//
// ✔MEASURED 2026-10-08.
//   * DSS's own data-only unit (`int shared = 7;`) writes the datum as record 0
//     on PE and on both Mach-O ISAs. Read back ALONE into a relocatable
//     artifact, the artifact held NO symbol at all and the build said nothing.
//     (ELF, the control, kept it; so did a link of two units, whose merge gives
//     every definition a new id.)
//   * Apple clang writes no local labels on x86_64, so record 0 of its objects
//     is the unit's first external definition — a datum as readily as a
//     function. The artifact DSS wrote of such an object lost that datum's
//     names, and Apple's ld refused it for an undefined symbol; the arm64
//     object opens with `ltmp0`, and its artifact was whole.
//   * MinGW's `strip --strip-unneeded`, `objcopy --strip-unneeded` and
//     `ld -r -x` each leave a COFF object whose record 0 is its first external
//     (gcc 13.2, binutils 2.42; GNU ld links each).
//
// ★★ THE RULE. The id of record i is i — and record 0, whose index is not an
// id, takes the first id past the table (the record count). Ids no record
// holds (a gap or padding atom, the reference row of a weak name, a directive's
// row) come after it, from ONE counter. Every reader states its ids through
// this class; the ELF reader too, whose record 0 names nothing, so that
// nothing of an ELF object moves. A pass that mints on the unit AFTER the read
// (the link's, for an entry a directive names or a name handed to a common)
// continues that counter: it asks the taken-id scan
// (`link/fresh_symbol_ids.hpp`), which reads every id this class gave — a
// record that became a symbol ROW and nothing else included.
//
// ★ WHY NOT `index + 1` FOR EVERY RECORD. A site that forgot that rule would
// mint the NEIGHBOUR record's id: a reference bound to the wrong symbol, which
// nothing downstream can tell from a right one. Under this rule a site that
// forgets it mints the right id for every record but 0 and, for record 0, the
// invalid id — which the link refuses by name (`linker.cpp`, the compound
// index). And a diagnostic's `symbol #N` stays record N of the object, in all
// three formats, for every record but the first.
//
// ★ NOTHING GOES BACK FROM AN ID TO A RECORD. A reader's own maps — the atom
// owners, the atoms, the externs, the boundaries — are keyed by the record
// INDEX and stay so; an id is made where a value enters the module, by `of`.
namespace dss::link::format {

class RecordSymbolIds {
public:
    explicit constexpr RecordSymbolIds(std::uint32_t recordCount) noexcept
        : recordCount_{recordCount}, nextFresh_{recordCount + 1u} {}

    // The id of record `index` of the table (`index` < the record count).
    [[nodiscard]] constexpr SymbolId of(std::uint32_t index) const noexcept {
        return SymbolId{index == 0u ? recordCount_ : index};
    }

    // An id no record holds and no earlier call returned.
    [[nodiscard]] constexpr SymbolId fresh() noexcept { return SymbolId{nextFresh_++}; }

private:
    std::uint32_t recordCount_;
    std::uint32_t nextFresh_;
};

}  // namespace dss::link::format
