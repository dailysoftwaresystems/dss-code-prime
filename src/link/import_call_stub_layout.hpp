#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────
// [[D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH]]
// WHERE AN IMPORT'S CALL STUB LANDS, ASKED OF THE WRITER THAT PUTS IT THERE
// ─────────────────────────────────────────────────────────────────────
//
// ★★★ THE BRANCH-VENEER PASS WAS BLIND TO EXACTLY ONE KIND OF TARGET, AND IT
// WAS THE ONE A LARGE IMAGE ALWAYS HITS. A call to an import lands on the stub
// the FORMAT WRITER emits for it (ELF `.plt`, Mach-O `__stubs`), and that
// address is not a property of the module at all: the writer chooses it, after
// `.text`, long after the veneer pass has run. ✔MEASURED 2026-09-17: every
// arm64 executable whose `.text` passed the `call26` reach was refused at the
// linker's OWN synthetic entry, whose last act is a call to the process-exit
// import.
//
// ★★ THE REFERENCES DECIDE THIS WITH THE STUB'S ADDRESS, AND SO DOES DSS NOW.
// 📄 GNU ld 2.42 (`_bfd_aarch64_add_call_stub_entries`) and ld.lld 18.1.3
// (`AArch64::needsThunk`) both measure a call to an import against its PLT
// ENTRY, and ld64.lld estimates where `__stubs` will land once `__text` is
// laid out (`estimateStubsInRangeVA`). The writer is the only component that
// knows that address, so the writer ANSWERS here, through its backend
// (`ObjectFormatBackend::importCallStubLayout`), and ASSERTS its real layout
// against the answer when it lays the stubs out. Two owners of one fact are
// made to agree loudly rather than trusted to.
//
// ⓘ AN UPPER BOUND, NOT AN ADDRESS, and that is all the pass needs: a distance
// it over-estimates can only ADD a veneer, never omit one that was needed. The
// bound must hold for EVERY `.text` size, because the pass inserts veneers
// into `.text` after reading it.
//
// ⚠ ONE LAYOUT IS MODELLED: stubs PAST the end of `.text`. That is where every
// DSS writer puts them, and where lld and ld64.lld put theirs (✔MEASURED).
// GNU ld puts `.plt` BEFORE `.text`; a writer that ever does the same must
// extend this answer, not bend it.
namespace dss::link {

struct DSS_EXPORT ImportCallStubLayout {
    // For every import a CALL can land on a stub for: the most bytes that
    // stub can lie past the end of `.text`. An import absent from the map has
    // no call stub in this output (a data import, or a format that binds
    // nothing), and a branch to it is left to the relocation applier.
    std::unordered_map<SymbolId, std::uint64_t> maxPastTextEnd;
};

}  // namespace dss::link
