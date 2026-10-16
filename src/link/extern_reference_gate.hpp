#pragma once

#include "asm/asm.hpp"                     // AssembledModule
#include "core/types/extern_import.hpp"    // ExternImport
#include "mir/mir.hpp"                     // Mir (the tier before relocations exist)
#include "mir/mir_literal_pool.hpp"        // forEachLiteralNode, MirSymbolAddrValue
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <unordered_set>
#include <variant>

// ★★ THE ONE ANSWER TO "DOES THIS MODULE USE THAT EXTERN", for every link-tier
// decision that keeps or follows an extern only when something uses it.
//
// D-LINK-EXTERN-IMPORT-REFERENCE-GATE states the rule: an extern import
// SURVIVES iff it is EAGER or REFERENCED, and "referenced" means a relocation in
// ANY function or data item targets the extern's symbol (an aggregate global can
// hold a function pointer to an extern -- the sqlite `aSyscall[]` shape) or the
// symbol of the slot that holds its address
// (D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING: a module that only takes `&w`
// names the slot in every relocation and the import in none) -- or the row is a
// use no relocation spells: an `/INCLUDE:`, a fallback, a common
// (`externIsReferenced`). Two decisions read it:
//   * the IMAGE's import table keeps exactly the surviving rows
//     (`rejectOrDropUnreferencedExterns`, linker.cpp) -- gcc's, clang's and
//     MSVC's rule that an unused declaration imports nothing (✔MEASURED, the
//     D-FFI-DESCRIPTOR-EAGER-IMPORT closure);
//   * the STATIC-ARCHIVE PULL satisfies exactly the surviving rows
//     (`pullStaticArchiveMembers`, compile_pipeline.cpp) -- an unused
//     declaration pulls no member, which is what every reference linker does,
//     because their objects carry no undefined symbol for a declaration nothing
//     uses. D-LK-ARCHIVE-PULL-TAKES-UNREFERENCED-EXTERNS-AS-REFERENCES: the pull
//     used to follow every extern a module DECLARED, so `#include <stdio.h>`
//     alone linked DSS's whole stdio runtime into a program that called nothing
//     (✔MEASURED P69 on pe64: `.text` 0x23 -> 0x7c3, imports 1 -> 6).
// And one decision reads the same rule a tier EARLIER, before any relocation
// exists -- `mirReferenceTargetIds` below:
//   * the SYNTHESIZED LIBRARY BODIES a module gets (`synthesizeThreadsShim`,
//     mir/merge) -- a recipe's body is synthesized only for a symbol the module
//     references, because a synthesized body is the same thing a pulled member
//     is: library code an unused declaration must not bring in.
//     D-MIR-THREADS-SHIM-SYNTHESIZES-UNREFERENCED-RECIPES: every recipe a
//     header declared used to be synthesized, so `#include <threads.h>` alone put
//     25 bodies and their 25 kernel32 imports into a pe program that called
//     nothing (✔MEASURED P69: `.text` 0x23 -> 0x81c, imports 1 -> 26), and
//     refused to compile on the one Mach-O format that declares no threads
//     vehicle.
//
// ★ HOISTED, NOT COPIED: a second statement of "referenced" would be free to
// drift from the first -- the pull following an extern the image then drops, or
// dropping one the image keeps -- and either way the two halves of one link
// would disagree about what the program uses.
//
// ★★ AND WHAT A REFERENCE TO A ROW THE GATE RESOLVES TO NOTHING COMPUTES (P69,
// D-LK-WEAK-UNDEFINED-SYMBOL-NAMED-DIRECTLY-IS-NOT-ADDRESS-ZERO). The gate binds a
// referenced WEAK row no unit defines and no library binds to nothing, on an image
// that cannot carry an undefined symbol; each REFERENCE to it then takes the value
// 0 by how it reads the symbol: through a slot, a slot holding 0; directly, the
// field computes from 0 where the image lets that field class reach it
// (`weakResolvedToNothing` in the image document, measured per reference linker)
// and is refused by name where it does not. That rule is
// `link/weak_resolved_to_nothing.hpp`, beside this header rather than in it,
// because it reads the link-tier format schema and this header is also read at
// the MIR tier (`synthesizeThreadsShim`).
namespace dss::linker {

// Every SymbolId a relocation in `m` targets, across functions AND data items.
[[nodiscard]] inline std::unordered_set<std::uint32_t>
relocationTargetIds(AssembledModule const& m) {
    std::unordered_set<std::uint32_t> targets;
    for (auto const& fn : m.functions) {
        for (auto const& rel : fn.relocations) targets.insert(rel.target.v);
    }
    for (auto const& di : m.dataItems) {
        for (auto const& rel : di.relocations) targets.insert(rel.target.v);
    }
    return targets;
}

// Whether the module uses `ext`: a relocation of it names the extern's own
// symbol, the symbol of the slot that holds its address, or the symbol of its
// CALL ENTRY taken as an address (`ExternImport::callEntrySymbol`, the PE import
// thunk a unit's `leaq puts(%rip)` reaches) -- one identity, three symbols; a
// module whose only use of the import is through one of the two others would
// otherwise read as not using it -- the row dropped, the slot or entry the
// walker binds gone with it, a relocation left pointing at nothing.
// ★ OR the row is a use with NO relocation (P69 round 3, lane `xa`,
// D-LK-COFF-READER-SKIPPED-EVERY-LINKER-DIRECTIVE): an `/INCLUDE:` makes it
// REQUIRED -- the link must define the name whether or not code names it
// (link.exe imports an unreferenced `/INCLUDE:puts`, ✔MEASURED 2026-10-06), and
// the archive search pulls the member that defines it; a row that still states
// a FALLBACK is the directive itself (a relocatable artifact hands
// `/alternatename:` on to its final linker; an image decides every fallback
// before its gate, `bindFallbackReferences`), which dropping would silently
// discard; and a COMMON row is not a reference but a DEFINITION
// (`ExternImport::commonSize`) -- dropping one would delete storage its object
// defines, and the archive search must still see it, because the members'
// format decides whether a member defining the name too is fetched
// (`archiveCommonResolution`).
[[nodiscard]] inline bool
externIsReferenced(ExternImport const&                      ext,
                   std::unordered_set<std::uint32_t> const& relocationTargets) {
    if (ext.requiredByDirective || !ext.fallbackName.empty() || ext.commonSize != 0u) {
        return true;
    }
    if (relocationTargets.contains(ext.symbol.v)) return true;
    if (ext.addressSlotSymbol.valid()
        && relocationTargets.contains(ext.addressSlotSymbol.v)) {
        return true;
    }
    return ext.callEntrySymbol.valid()
        && relocationTargets.contains(ext.callEntrySymbol.v);
}

// The gate itself: EAGER (a row the loader must resolve whether or not this
// module uses it) or REFERENCED.
[[nodiscard]] inline bool
externSurvivesReferenceGate(ExternImport const&                      ext,
                            std::unordered_set<std::uint32_t> const& relocationTargets) {
    return ext.isEagerImport || externIsReferenced(ext, relocationTargets);
}

// ★★ AND WHICH SURVIVING ROWS THE ARCHIVE SEARCH MAY STILL LEAVE UNRESOLVED (P69
// round 4, D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE). A WEAK
// reference is a use — the image keeps it, and binds it to nothing when nothing
// defines its name — but whether an archive member that defines the name is
// FETCHED for it is the members' format's to say (`archiveWeakReferenceSearch`):
// the ELF gABI says no ("The link editor does not extract archive members to
// resolve undefined weak symbols"; GNU ld and ld.lld agree), so do the COFF
// linkers for a weak external that states no library search, and ld64 fetches
// it as for a strong reference (✔MEASURED 2026-10-07, every reference linker,
// both ISAs). A row this answers TRUE for joins the search only by that rule;
// every other surviving row joins it unconditionally. Not such a row: a weak
// reference that states a FALLBACK (the fallback rule decides it,
// `/alternatename:` and the pull's fallback rounds), a COMMON (a definition), one
// a directive makes REQUIRED (`/INCLUDE:`), and a COFF weak external that asks
// for the library search itself (`searchesArchives`).
[[nodiscard]] inline bool
weakReferenceAwaitsTheArchiveRule(ExternImport const& ext) noexcept {
    return ext.binding == SymbolBinding::Weak && ext.fallbackName.empty()
        && ext.commonSize == 0u && !ext.requiredByDirective && !ext.searchesArchives;
}

// The SAME question at the MIR tier: every SymbolId the module's code or data
// NAMES. These are exactly the references codegen turns into the relocations
// `relocationTargetIds` reads -- a `GlobalAddr` (a call's callee, an address
// taken), a symbol-address leaf of a `Const`'s literal, and one of a global's
// initializer (an aggregate's function-pointer member included, at any brace
// depth) -- so a symbol is in this set iff the module READ holds a reference that
// lowers to a relocation targeting it. (A `BlockAddressExport` DEFINES its symbol;
// it is not a reference.)
// ★ WHICH MODULE THE SYNTHESIS READS (P69 round 4; the review's MINOR 7): the one
// the UNIT-stage optimize left, BEFORE the PROGRAM-stage optimize --
// `synthesizeLibraryShims` runs ahead of every route's final optimize
// (D-MIR-SYNTH-SHIM-SEAM-OPTIMIZE-PLACEMENT-ASYMMETRY). So a reference in code only the
// Program stage deletes still synthesizes its body, and the body STAYS: a WEAK body is
// a DCE root at LinkInput extent, where a later object of the same link may name it.
// ✔MEASURED (lane lm, pe64 exec, a <threads.h> recipe named only by a static function
// nothing calls): debug keeps the 0x4a-byte mtx_init body and its kernel32 import
// while the caller is gone; release keeps neither (the Unit stage deleted the caller
// first). The references split on the same source (runs 20261007-185652-72292454,
// 20261007-185810-abfd18c3): gcc 13.3.0 -O0 keeps the caller and imports
// `mtx_init@GLIBC_2.34`; clang 18.1.3 -O0 and cl 19.51 /Od emit neither; every -O2
// neither. ⓘ The rule reads this module rather than predicting the Program stage's:
// that stage's pipeline is configured, and overridable per site (the examples
// runner's `["Inlining"]` deletes nothing), so a prediction that ran ahead of it
// would leave a call to a body never synthesized, where this read costs one body of
// waste -- the one gcc -O0 keeps the import for.
[[nodiscard]] inline std::unordered_set<std::uint32_t>
mirReferenceTargetIds(Mir const& m) {
    std::unordered_set<std::uint32_t> targets;
    auto noteLiteral = [&](MirLiteralValue const& root) {
        forEachLiteralNode(root, [&](MirLiteralValue const& node) {
            if (auto const* sa = std::get_if<MirSymbolAddrValue>(&node.value)) {
                targets.insert(sa->symbol);
            }
        });
    };
    std::size_t const nf = m.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = m.funcAt(fi);
        std::uint32_t const nb = m.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = m.funcBlockAt(f, bi);
            std::uint32_t const ni = m.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const inst = m.blockInstAt(b, ii);
                switch (m.instOpcode(inst)) {
                case MirOpcode::GlobalAddr:
                    targets.insert(m.globalAddrSymbol(inst).v);
                    break;
                case MirOpcode::Const:
                    noteLiteral(m.literalValue(m.constLiteralIndex(inst)));
                    break;
                default:
                    break;
                }
            }
        }
    }
    std::size_t const ng = m.moduleGlobalCount();
    for (std::uint32_t gi = 0; gi < ng; ++gi) {
        std::uint32_t const lit = m.globalInitLiteralIndex(m.globalAt(gi));
        if (lit != UINT32_MAX) noteLiteral(m.literalValue(lit));
    }
    return targets;
}

}  // namespace dss::linker
