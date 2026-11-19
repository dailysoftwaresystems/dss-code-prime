#pragma once

// THE ONE VOCABULARY OF A PROGRAM'S ENTRY NAMES (P69 fold 3e, lane `xa`: the review of fold 3d's class C, its X1 and
// neighbour N3).
//
// A grammar declares the names a program may be entered at, each with the materialization verbs a program entered
// there may take: a language on its declaration forms (`semantics.declarations[].entryFunctions`, a row per
// signature), an assembly dialect -- which has no declarations -- on its `assembly.entryLabels`, in the same
// name-and-verb shape. An object format realizes some verbs (its `entryVerbs`); one that starts no program realizes
// none. The names a program may be entered at ON THAT FORMAT are the names of which it realizes at least one row, and
// this is the one place that says which:
//   * the driver's election spells each on the binary (`entryLinkNames`, compile_pipeline: the format's C
//     decoration) to read an object's or an assembly unit's entry, and to name what a program without one lacks;
//   * the semantic tier asks it whether a definition's NAME makes the definition an entry on the pair's format, and
//     which row a definition whose signature is none of the realized rows is entered with (the richest).
// ✔MEASURED before (lane `xa`, the r36 dsscp, against cl 19.44, clang-cl 19.1.5, MinGW-w64 gcc 13.2, gcc 13.3 and
// clang 18.1.3): `int wmain(int, unsigned int **)` beside `main` was refused S_EntryShapeNotDeclared on every
// format, where every reference links it and runs `main` (11) -- an ordinary function where the format realizes no
// wide verb, an entry candidate below `main` where it does (cl: LNK4067) -- and `int main(int, double)` was refused
// where gcc (-Wmain) and cl link it and run it (11).
// It takes the format's realized verbs, not the format: the semantic tier holds the verbs of the pair it analyzes
// for, and asks per tree, of the tree's own grammar.

#include "core/export.hpp"
#include "core/types/entry_shape.hpp"      // EntryMaterialization
#include "core/types/grammar_schema.hpp"   // GrammarSchema

#include <span>
#include <string>
#include <vector>

namespace dss {

// One entry name of a grammar of which a format realizes at least one row, with the RICHEST of those rows -- the
// most parameters, the first such row in the grammar's order: the verb every reference's startup calls an entry
// whose prototype it does not know with (an object's, and a definition whose signature is none of the rows).
struct DSS_EXPORT RealizedEntryName {
    std::string          name;   // the grammar's spelling (`main`), never the binary's (`_main` on Mach-O)
    EntryMaterialization verb = EntryMaterialization::None;
};

// Each entry name `grammar` declares, a language's rows and a dialect's labels alike, of which a row's verb is in
// `realizedVerbs` (an object format's `entryVerbs`), in the grammar's order. Empty where `realizedVerbs` is (a format
// that starts no program).
[[nodiscard]] DSS_EXPORT std::vector<RealizedEntryName>
realizedEntryNames(GrammarSchema const& grammar, std::span<EntryMaterialization const> realizedVerbs);

// Every verb a row of `grammar` names, a language's and a dialect's alike, each once, in the grammar's order: what a
// caller with no format in hand (a direct-API analysis, the language server) counts as realized.
[[nodiscard]] DSS_EXPORT std::vector<EntryMaterialization> declaredEntryVerbs(GrammarSchema const& grammar);

}  // namespace dss
