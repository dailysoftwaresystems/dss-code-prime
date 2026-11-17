#pragma once

#include "core/types/diagnostic_reporter.hpp"   // DiagnosticReporter, report
#include "core/types/object_format_kind.hpp"    // kFirstWriterReservedSymbolIdValue
#include "core/types/parse_diagnostic.hpp"      // K_SymbolIdSpaceExhausted
#include "core/types/strong_ids.hpp"            // SymbolId
#include "core/types/symbol_id_cursor.hpp"      // symbolIdOfIndex, SymbolIdEnd -- the one rule of a fresh id

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

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
// continues that counter: the module the reader builds carries this table's
// end, and the pass opens the module's door (`link/fresh_symbol_ids.hpp`) on
// it -- a record that became a symbol ROW and nothing else, and a fresh id the
// reader dropped, included.
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

// ★ P69 (lane `cs`, fold f26d -- review-csf26c2 N9): TWO SPACES, EACH BY THE ONE RULE OF A FRESH ID
// (core/types/symbol_id_cursor.hpp). A record's id is an INDEX id -- `symbolIdOfIndex` of its position, record 0's
// of the record count -- so the table ENDS below the first value a format writer reserves: a table whose count
// reaches it is refused at the read, by name. The ids no record holds are a COUNTING space past the table, taken
// through the cursor, which steps over a reserved value and refuses the last. This table is their door: it reports
// either refusal itself, once, and hands out the invalid id from then on; the reader that owns it refuses its whole
// output (`refused()`), so no module carries such an id. The module the reader builds carries the table's end
// (`end()`, `AssembledModule::symbolIdEnd`): an id `fresh` handed out and the reader later dropped is never handed
// out again by a pass that mints on the module.
class RecordSymbolIds {
public:
    RecordSymbolIds(std::size_t recordCount, DiagnosticReporter& reporter, std::string_view reader)
        : recordCount_{recordCount}, reporter_{&reporter}, reader_{reader} {
        if (recordCount_ != 0u && !symbolIdOfIndex(recordCount_).valid()) {
            refuse(std::format("its {} symbol records reach {}, the first SymbolId a format writer reserves -- a "
                               "record's id is its index, and an index space ends there",
                               recordCount_, kFirstWriterReservedSymbolIdValue));
            return;
        }
        fresh_.keepClearOfHighest(static_cast<std::uint32_t>(recordCount_));
    }

    // The id of record `index` of the table (`index` < the record count).
    [[nodiscard]] constexpr SymbolId of(std::uint32_t index) const noexcept {
        return symbolIdOfIndex(index == 0u ? recordCount_ : index);
    }

    // An id no record holds and no earlier call returned; the invalid id once the table is refused.
    [[nodiscard]] SymbolId fresh() {
        if (refused_) return SymbolId{};
        SymbolId const id = fresh_.take();
        if (!id.valid()) {
            refuse(std::format("its ids past the table are exhausted (the end is {})", fresh_.value()));
        }
        return id;
    }

    // The table or its fresh ids were refused (reported here): the reader refuses its output.
    [[nodiscard]] bool refused() const noexcept { return refused_; }
    // One past every id this table gave: the module the reader builds carries it.
    [[nodiscard]] SymbolIdEnd end() const noexcept { return fresh_; }

private:
    void refuse(std::string const& why) {
        refused_ = true;
        report(*reporter_, DiagnosticCode::K_SymbolIdSpaceExhausted, DiagnosticSeverity::Error,
               std::format("{}: the object's SymbolId space is exhausted: {}", reader_, why));
    }

    std::size_t         recordCount_;
    DiagnosticReporter* reporter_;
    std::string_view    reader_;
    SymbolIdEnd         fresh_;
    bool                refused_ = false;
};

}  // namespace dss::link::format
