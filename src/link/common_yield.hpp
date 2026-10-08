#pragma once

#include "asm/asm.hpp"                     // ModuleSymbol
#include "link/object_format_schema.hpp"   // ObjectFormatSchema, CommonYieldsTo

#include <optional>

namespace dss::linker {

// Does a COMMON (tentative) definition of a name YIELD to `definition`, another
// definition of that name — does the definition replace the common (true), or
// does the common outrank it (false)? ONE answer for every place a common meets
// a definition (P69 round 4,
// D-LK-COMMON-OUTRANKED-A-WEAK-DEFINITION-IN-EVERY-FORMAT): the link that
// allocates commons (`allocateCommonDefinitions`, `link/linker.cpp`) and the
// archive search (`pullStaticArchiveMembers`, `program/compile_pipeline.cpp`),
// which asks it twice — of a MEMBER, to decide which one a common's name
// fetches, so the member fetched is the definition the link then lets win; and
// of every definition ALREADY in the link, to decide whether the name is still a
// common at all, so a weak definition the common outranks does not end its
// search (send-back 5: until then the search took any linked definition of the
// name for an answer). Until round 4's end each place spelled the rule for
// itself.
//   * A STRONG definition replaces a common under every linker measured.
//   * A WEAK definition whose own spelling yields to a common
//     (`ModuleSymbol::yieldsToACommon`: a COFF weak external whose default is a
//     body, MinGW gcc's `__attribute__((weak))` — PE/COFF 5.5.3 uses the default
//     only "if sym1 is not present at link time") never does.
//   * Any other WEAK definition replaces it where the LINK's document says a
//     common yields to any definition (`commonYieldsTo`: `anyDefinition`, Mach-O
//     and PE) and loses to it where only a strong one does (`strongDefinition`,
//     ELF: the System V gABI).
//   * A LOCAL symbol is no definition a common of another unit meets.
// nullopt: `definition` is one the document decides and `linkFormat` does not
// state `commonYieldsTo`; each caller refuses by name, in its own words, rather
// than guess a meaning the families disagree on.
[[nodiscard]] inline std::optional<bool>
commonYieldsToDefinition(ObjectFormatSchema const& linkFormat,
                         ModuleSymbol const&       definition) noexcept {
    if (definition.binding == SymbolBinding::Global) return true;
    if (definition.binding != SymbolBinding::Weak || definition.yieldsToACommon) return false;
    auto const yields = linkFormat.commonYieldsTo();
    if (!yields.has_value()) return std::nullopt;
    return *yields == CommonYieldsTo::AnyDefinition;
}

}  // namespace dss::linker
