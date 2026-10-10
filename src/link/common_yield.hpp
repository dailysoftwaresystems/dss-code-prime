#pragma once

#include "asm/asm.hpp"                     // ModuleSymbol
#include "core/types/symbol_attrs.hpp"     // WeakDefinitionKind
#include "link/cross_cu_resolve.hpp"       // the ranks `CrossCuDef::weakRank` carries
#include "link/object_format_schema.hpp"   // ObjectFormatSchema, CommonYieldsTo

#include <cstdint>
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
//   * A WEAK definition, as the LINK's document says (`commonYieldsTo`): it
//     replaces the common where a common yields to any definition
//     (`anyDefinition`, Mach-O); it loses to it where only a strong one does
//     (`strongDefinition`, ELF: the System V gABI); and where the format has two
//     kinds of weak definition and the common stands BETWEEN them
//     (`nonOverridableDefinition`, PE) the definition's own kind decides
//     (`ModuleSymbol::weakKind`): one stated OVERRIDABLE — a COFF weak external
//     whose default is a body, MinGW gcc's and clang's `__attribute__((weak))`;
//     PE/COFF 5.5.3 uses the default only "if sym1 is not present at link time"
//     — yields to the common, and every other one replaces it.
//   * A LOCAL symbol is no definition a common of another unit meets.
// nullopt: `definition` is one the document decides and `linkFormat` does not
// state `commonYieldsTo`; each caller refuses by name, in its own words, rather
// than guess a meaning the families disagree on.
[[nodiscard]] inline std::optional<bool>
commonYieldsToDefinition(ObjectFormatSchema const& linkFormat,
                         ModuleSymbol const&       definition) noexcept {
    if (definition.binding == SymbolBinding::Global) return true;
    if (definition.binding != SymbolBinding::Weak) return false;
    auto const yields = linkFormat.commonYieldsTo();
    if (!yields.has_value()) return std::nullopt;
    switch (*yields) {
        case CommonYieldsTo::StrongDefinition:
            return false;
        case CommonYieldsTo::AnyDefinition:
            return true;
        case CommonYieldsTo::NonOverridableDefinition:
            return definition.weakKind != WeakDefinitionKind::Overridable;
    }
    return std::nullopt;   // a value outside the closed set answers nothing
}

// WHERE A WEAK DEFINITION STANDS AMONG THE WEAK DEFINITIONS OF ITS NAME — the
// rank the cross-unit fold is handed (`CrossCuDef::weakRank`; P69,
// D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER). It
// is the SAME fact of the link's document as the answer above, read once more:
// where a common stands BETWEEN the format's two kinds of weak definition
// (`nonOverridableDefinition`), the kind the common outranks is outranked by
// the kind the common yields to as well — strong > select-any > common >
// overridable is one order (✔MEASURED 2026-10-08 on PE: GNU ld 2.42, link.exe
// 14.44 and lld-link 19.1.5 run one name defined both ways with the select-any
// body in both object orders, and with it beside a common in all six). So a
// definition STATED overridable yields to every other weak definition of its
// name there. Everywhere else — a document that ranks no kind against a common,
// or states nothing; a definition of the other kind, or of no stated kind —
// weak definitions fold as they always did.
[[nodiscard]] inline std::uint8_t
weakDefinitionRank(ObjectFormatSchema const& linkFormat,
                   ModuleSymbol const&       definition) noexcept {
    bool const yieldsToTheRest =
        definition.weakKind == WeakDefinitionKind::Overridable
        && linkFormat.commonYieldsTo() == CommonYieldsTo::NonOverridableDefinition;
    return yieldsToTheRest ? kYieldingWeakDefinition : kUnrankedWeakDefinition;
}

}  // namespace dss::linker
