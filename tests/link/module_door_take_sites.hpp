#pragma once

// ★ THE UNIVERSE OF THE LINK TIER'S TAKE SITES (P69, lane `cs`, fold f26d -- main's ruling on the take sites): every
// place a link-tier pass asks a door of link/fresh_symbol_ids.hpp for a fresh SymbolId, by the label it gives the
// door (the literal its refusal names, `{}` where the label formats a value). Two tests read this one list:
//   * tests/link/test_module_door_take_sites_guard.cpp (repo-guard) reads src/ and refuses a take site this list does
//     not hold, and a row of this list no site of src/ states;
//   * tests/link/test_module_door_take_sites.cpp drives every row with its door at the edge of its space and holds it
//     to the rule: refused BY NAME, once, and what its caller holds or receives exactly as it was.
// A row is a take on the module door (`ModuleSymbolIds::take`, the label its last argument) or the construction of
// the door of a new space (`ReportedSymbolIds{start, sink, label}`, whose every take answers to that label).

#include <string_view>

namespace dss::test {

struct LinkTakeSite {
    std::string_view file;    // the source that states the label, from the repository root
    std::string_view label;   // the label, as the source spells its literal
};

inline constexpr LinkTakeSite kLinkTakeSites[] = {
    {"src/link/branch_veneers.cpp", "branch-veneer: the {} veneer(s)"},
    {"src/link/entry_trampoline.cpp", "entry-trampoline: the {} id(s) of the trampoline and its exit"},
    {"src/link/format/coff_object_reader.cpp", "pe: a reference row a unit's linker directive asks for"},
    {"src/link/format/pe.cpp", "pe::encodeExec: the address slot an import-slot fix-up reads"},
    {"src/link/format/pe.cpp", "pe::encodeExec: the import-slot residue runner"},
    {"src/link/format/pe.cpp", "pe::encodeExec: the residue runner's thread-template base"},
    {"src/link/import_address_references.hpp", "linker: the {} import-address row(s) of a unit"},
    {"src/link/linker.cpp", "linker: a null-address symbol"},
    {"src/link/linker.cpp", "linker: the object-carried data-import slot for '{}'"},
    {"src/link/linker.cpp", "linker: the merged module"},
    {"src/link/linker.cpp", "linker: the body that leaves its id to the weak name '{}'"},
    {"src/link/linker.cpp", "linker: the reference row that keeps the weak name '{}'"},
    {"src/link/linker.cpp", "linker: a GOT slot"},
    {"src/link/pc_relative_import_address.cpp", "linker: an import's call entry a unit is bound to"},
    {"src/link/unit_linker_decisions.cpp", "linker: the reference the entry '{}' requires"},
};

}  // namespace dss::test
