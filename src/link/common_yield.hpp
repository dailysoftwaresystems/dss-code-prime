#pragma once

#include "asm/asm.hpp"                     // ModuleSymbol
#include "core/types/config_key_vocabulary.hpp"  // renderAllowedList — the ONE closed-set renderer
#include "core/types/enum_name_table.hpp"  // allNames
#include "core/types/parse_diagnostic.hpp" // DiagnosticCode — the code a refusal of the fold carries
#include "core/types/symbol_attrs.hpp"     // WeakDefinitionKind
#include "link/cross_cu_resolve.hpp"       // the ranks `CrossCuDef::weakRank` carries
#include "link/object_format_schema.hpp"   // ObjectFormatSchema, CommonYieldsTo

#include <cstdint>
#include <format>
#include <optional>
#include <string>

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
//
// ★ TWO SPELLINGS, ONE STATEMENT (P69 fold 3). The question is about a
// definition's BINDING and its weak KIND, and the cross-unit MIR merge asks it
// of a definition that is not a `ModuleSymbol` yet (`mergeCuMirs`, a MIR
// function or global). So the rule is written once, over the two facts, and
// the `ModuleSymbol` form hands it its row's.
[[nodiscard]] inline std::optional<bool>
commonYieldsToDefinition(ObjectFormatSchema const&         linkFormat,
                         SymbolBinding                     binding,
                         std::optional<WeakDefinitionKind> weakKind) noexcept {
    if (binding == SymbolBinding::Global) return true;
    if (binding != SymbolBinding::Weak) return false;
    auto const yields = linkFormat.commonYieldsTo();
    if (!yields.has_value()) return std::nullopt;
    switch (*yields) {
        case CommonYieldsTo::StrongDefinition:
            return false;
        case CommonYieldsTo::AnyDefinition:
            return true;
        case CommonYieldsTo::NonOverridableDefinition:
            return weakKind != WeakDefinitionKind::Overridable;
    }
    return std::nullopt;   // a value outside the closed set answers nothing
}
[[nodiscard]] inline std::optional<bool>
commonYieldsToDefinition(ObjectFormatSchema const& linkFormat,
                         ModuleSymbol const&       definition) noexcept {
    return commonYieldsToDefinition(linkFormat, definition.binding,
                                    definition.weakKind);
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
// Written over the KIND, for the reason `commonYieldsToDefinition` states above;
// the `ModuleSymbol` form hands it its row's.
[[nodiscard]] inline std::uint8_t
weakDefinitionRank(ObjectFormatSchema const&         linkFormat,
                   std::optional<WeakDefinitionKind> weakKind) noexcept {
    bool const yieldsToTheRest =
        weakKind == WeakDefinitionKind::Overridable
        && linkFormat.commonYieldsTo() == CommonYieldsTo::NonOverridableDefinition;
    return yieldsToTheRest ? kYieldingWeakDefinition : kUnrankedWeakDefinition;
}
[[nodiscard]] inline std::uint8_t
weakDefinitionRank(ObjectFormatSchema const& linkFormat,
                   ModuleSymbol const&       definition) noexcept {
    return weakDefinitionRank(linkFormat, definition.weakKind);
}

// WHAT A COMMON IS BESIDE THIS WEAK DEFINITION, in the cross-unit fold's own
// vocabulary (`CrossCuDef::commonBesideIt`) — `commonYieldsToDefinition` read
// once more, so the fold of a tentative definition beside a weak one
// (`resolveCrossCuDefs`) and the allocation of an object's common beside one
// (`allocateCommonDefinitions`) cannot answer differently. A document that does
// not state `commonYieldsTo` answers `Unstated`, and the fold hands the pair
// back for its caller to refuse by the key's name.
[[nodiscard]] inline CommonBesideWeakAnswer
commonBesideAWeakDefinition(ObjectFormatSchema const&         linkFormat,
                            std::optional<WeakDefinitionKind> weakKind) noexcept {
    auto const yields =
        commonYieldsToDefinition(linkFormat, SymbolBinding::Weak, weakKind);
    if (!yields.has_value()) return CommonBesideWeakAnswer::Unstated;
    return *yields ? CommonBesideWeakAnswer::Yields : CommonBesideWeakAnswer::Outranks;
}

// WHAT THIS WEAK DEFINITION IS BESIDE A STRONG ONE OF ITS NAME — the link
// document's `weakDefinitionBesideStrong` (`WeakBesideStrong`,
// link/object_format_schema.hpp), in the fold's vocabulary
// (`CrossCuDef::besideStrong`). The ONE reader of that key: the unit linker's
// fold and the cross-unit MIR merge both state a definition's answer through
// here. A document that does not state the key — or states it per kind for a
// definition that states none — answers `Unstated`.
[[nodiscard]] inline WeakBesideStrongAnswer
weakBesideAStrongDefinition(ObjectFormatSchema const&         linkFormat,
                            std::optional<WeakDefinitionKind> weakKind) noexcept {
    auto const answer = linkFormat.weakDefinitionBesideStrong().answerFor(weakKind);
    if (!answer.has_value()) return WeakBesideStrongAnswer::Unstated;
    switch (*answer) {
        case WeakBesideStrong::Yields:   return WeakBesideStrongAnswer::Yields;
        case WeakBesideStrong::Collides: return WeakBesideStrongAnswer::Collides;
    }
    return WeakBesideStrongAnswer::Unstated;   // a value outside the closed set answers nothing
}

// ★★★ THE ONE PLACE A WEAK DEFINITION'S KIND BECOMES WHAT THE CROSS-UNIT FOLD
// READS OF IT (P69 fold 3). THREE DOORS hand `resolveCrossCuDefs` their
// definitions — the unit linker (`link/linker.cpp`, a row of an object or of a
// compiled unit), the cross-unit MIR merge (`mir/merge/mir_merge.cpp`, a MIR
// function or global) and the whole-program summary index under `--lto=thin`
// (`mir/summary/summary_index.cpp`, a summary row) — and until this function
// each stated what it happened to know: the first a rank, the other two
// nothing, so one program elected one definition through one door and another
// through the next (✔MEASURED 2026-10-12, a select-any definition beside an
// overridable one: 11 through the unit linker in both orders, 44 through the
// merge with the overridable unit named first). Each door now calls THIS with
// the definition's kind, and the three statements are made together or not at
// all:
//   * its RANK among the weak definitions of its name;
//   * what it is beside a STRONG definition — replaced, or a duplicate;
//   * what a COMMON is beside it — yields, or outranks it.
// A definition that is not weak is left exactly as its door built it.
inline void stateTheLinksAnswers(CrossCuDef&                       definition,
                                 ObjectFormatSchema const&         linkFormat,
                                 std::optional<WeakDefinitionKind> weakKind) noexcept {
    if (definition.binding != SymbolBinding::Weak) return;
    definition.weakRank       = weakDefinitionRank(linkFormat, weakKind);
    definition.besideStrong   = weakBesideAStrongDefinition(linkFormat, weakKind);
    definition.commonBesideIt = commonBesideAWeakDefinition(linkFormat, weakKind);
}

// ★ THE ONE WORDING OF WHAT THOSE ANSWERS REFUSE (P69 fold 3). The fold records
// two outcomes as DATA and reports neither (`CrossCuResolution::collisions`,
// `::undecided`), so each of its three doors must say them — and a refusal
// worded three times is three refusals the day one of them is amended: one door
// naming the key, the next an older one. So the code and the sentence are made
// HERE, from the fold's record and the link's document, and a door adds only
// its own name in front.
struct CrossCuRefusal {
    DiagnosticCode code = DiagnosticCode::K_SymbolRedefinedAcrossUnits;
    std::string    text;
};

// A weak definition of a kind the link's format does not let a strong one
// replace, beside a strong one.
//   ★ THE CODE IS `K_SymbolRedefinedAcrossUnits`, the code of the two-strong
//   refusal: it IS a multiply-defined symbol — link.exe's LNK2005, lld-link's
//   "duplicate symbol", GNU ld's "multiple definition" for this very pair — and
//   the sentence says which rule was broken. Until P69 fold 3 the strong
//   definition won in silence (✔MEASURED 2026-10-12 on cl's, clang's and MinGW
//   gcc's objects: `__declspec(selectany) int v = 1;` beside `int v = 2;`
//   linked and exited 22 where all three reference linkers refuse).
[[nodiscard]] inline CrossCuRefusal
refusalOf(CrossCuCollision const& collision, ObjectFormatSchema const& linkFormat) {
    return CrossCuRefusal{
        DiagnosticCode::K_SymbolRedefinedAcrossUnits,
        std::format(
            "symbol \"{}\" is defined STRONG in CU #{} and as a weak definition in "
            "CU #{} of a kind this link's format ('{}') does not let a strong "
            "definition replace — its document states `weakDefinitionBesideStrong` "
            "as '{}' for that kind. The two are a duplicate definition: a "
            "definition the link may keep one copy of is one of several COPIES, "
            "and a strong definition is not one of them.",
            collision.name, collision.strong.cuId.v, collision.weak.cuId.v,
            linkFormat.name(),
            kWeakBesideStrongTable.name(WeakBesideStrong::Collides))};
}

// The link's document was ASKED and does not say — refused by the key's name
// rather than decided by a guess the formats disagree on. Two questions reach
// here (`CrossCuQuestion`): a weak definition beside a strong one, and a common
// beside a weak one.
[[nodiscard]] inline CrossCuRefusal
refusalOf(CrossCuUndecided const& undecided, ObjectFormatSchema const& linkFormat) {
    if (undecided.question == CrossCuQuestion::WeakBesideStrong) {
        return CrossCuRefusal{
            DiagnosticCode::K_NoMatchingObjectFormat,
            std::format(
                "symbol \"{}\" is defined STRONG in CU #{} and WEAK in CU #{}, and "
                "this link's format ('{}') does not say what a weak definition{} "
                "is beside a strong one of its name: its document states no "
                "`weakDefinitionBesideStrong` ({}) for it. The formats disagree — "
                "one replaces the weak definition, another refuses the pair as a "
                "duplicate — so the link refuses rather than guess.",
                undecided.name, undecided.definition.cuId.v, undecided.weak.cuId.v,
                linkFormat.name(),
                linkFormat.weakDefinitionBesideStrong().perKind()
                    ? " that states no kind" : "",
                dss::detail::renderAllowedList(allNames(kWeakBesideStrongTable),
                                               " | "))};
    }
    return CrossCuRefusal{
        DiagnosticCode::K_NoMatchingObjectFormat,
        std::format(
            "symbol \"{}\" is defined tentatively (a common) in CU #{} and WEAK in "
            "CU #{}, and this link's format ('{}') does not say whether a common "
            "yields to a weak definition of its name or outranks it: its document "
            "states no `commonYieldsTo` ({}) for it. The formats disagree, so the "
            "link refuses rather than guess.",
            undecided.name, undecided.definition.cuId.v, undecided.weak.cuId.v,
            linkFormat.name(),
            dss::detail::renderAllowedList(allNames(kCommonYieldsToTable), " | "))};
}

}  // namespace dss::linker
