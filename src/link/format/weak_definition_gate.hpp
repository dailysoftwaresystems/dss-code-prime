#pragma once

#include "asm/asm.hpp"                          // AssembledModule
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"    // WeakDefinitionDialect
#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"            // detail::emit
#include "link/format/object_symbol_names.hpp"  // ObjectSymbolNames
#include "link/object_format_schema.hpp"

#include <format>
#include <span>
#include <string>
#include <string_view>

// ── THE WEAK-DEFINITION DIALECT GATE ──────────────────────────────────────
//    D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED (the key)
//    D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS (this)
//
// ONE question, asked the same way by every walker that encodes a weak
// definition: *has this format SAID how a weak definition is spelled, and is
// the spelling one I can write?*
//
// ★★ IT IS A SHARED HELPER RATHER THAN A PER-WALKER COPY BECAUSE THE THREE
// COPIES WOULD DRIFT, AND THIS TREE HAS THE RECEIPT. `definedNDesc` and
// `definedNType` in `macho.cpp` are single functions with three call sites
// each, for exactly the reason their comments give: three open-coded ternaries
// is how a name/binding pair drifts, which is what
// D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION was. A gate that
// refuses in one walker and shrugs in another is the same defect one tier up,
// and it is invisible until a format document loses a key.
//
// ★★★ FORMAT-AGNOSTIC BY CONSTRUCTION. Nothing here knows which format it is
// serving: the CALLER passes the dialect ITS encoder spells, so the comparison
// is `declared == spelled` — config vocabulary against config vocabulary. There
// is no `if (format == …)`, no kind, no backend identity. Adding a fourth
// dialect with a fourth walker arm needs no edit in this file.
//
// ★ SCAN FIRST, ASK SECOND — SO ABSENCE STAYS FREE. The question is asked only
// when the module actually carries a weak definition. A format that never meets
// one is never required to answer, which is the shape the 2026-08-20 operator
// ruling asked for: ONE dialect row, never a key every schema must carry. The
// obvious implementation (ask the schema up front, refuse a nullopt) makes the
// key effectively required and is the design that ruling rejected.
//
// ★ WHY A REFUSAL AND NOT A FALLBACK. Re-spelling a weak definition under
// whatever encoder is at hand publishes it as a STRONG definition: the linker
// then rejects a duplicate it was supposed to coalesce, or keeps the wrong
// body. That is a silent semantic change, so an unanswered or unspellable
// dialect stops the encode with `K_FormatLacksWeakDefinitionDialect`.

namespace dss::link::format {

// Does this module carry a symbol row the writer would emit as a WEAK
// definition?
//
// ★ TWO PASSES, CHEAP ONE FIRST. A weak definition REQUIRES some
// `ModuleSymbol` row to carry `SymbolBinding::Weak` — a necessary condition
// testable with one allocation-free linear scan. Only when that scan finds
// something is the `ObjectSymbolNames` index built and the precise question
// asked. The overwhelmingly common module (no weak row at all) therefore pays
// a scan of `module.symbols` and nothing else.
//
// ⚠ THE PRECISE QUESTION INCLUDES ALIAS ROWS, and that is wider than a
// canonical-only test. `module.symbols` may carry SEVERAL rows for one
// `SymbolId` (D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS), and
// `definedBinding` is first-row-wins — so a GLOBAL definition with a WEAK
// alias, which is exactly what gcc emits for
// `__attribute__((weak, alias("strong_fn")))`, has a canonical binding of
// Global and still puts a weak definition on the wire through the alias pass.
// Testing only the canonical would let that module through the gate unasked.
[[nodiscard]] inline bool
moduleDefinesWeakSymbol(AssembledModule const& module) {
    bool anyWeakRow = false;
    for (auto const& s : module.symbols) {
        if (s.binding == SymbolBinding::Weak) { anyWeakRow = true; break; }
    }
    if (!anyWeakRow) return false;

    ObjectSymbolNames const names{module};
    auto isWeakDefinedId = [&](SymbolId id) {
        if (names.definedBinding(id) == SymbolBinding::Weak) return true;
        for (ModuleSymbol const* alias : names.definedAliases(id)) {
            if (alias->binding == SymbolBinding::Weak) return true;
        }
        return false;
    };
    for (auto const& fn : module.functions) {
        if (isWeakDefinedId(fn.symbol)) return true;
    }
    for (auto const& d : module.dataItems) {
        // An anonymous item carries no `ModuleSymbol` row and so can never be
        // weak — it is referenced by section offset, never by name.
        if (d.symbol == SymbolId{}) continue;
        if (isWeakDefinedId(d.symbol)) return true;
    }
    return false;
}

// The gate. Returns TRUE when the walker may proceed — either because the
// module carries no weak definition at all, or because the format declared the
// dialect this walker spells. Returns FALSE having ALREADY emitted the
// refusal.
//
// `spelled` — the dialect THIS walker's encoder writes.
// `where`   — the walker's own diagnostic prefix ("elf::encode", …), so one
//             shared message serves every caller without naming a format.
//
// ★ TWO REFUSALS, NOT ONE, BECAUSE THEIR REMEDIATIONS DIFFER. An UNANSWERED
// schema needs the block added; a schema that answered with a dialect this
// walker has no encoder for needs the DECLARATION fixed (or that encoder
// built). Collapsing them into one message sends half the readers to the wrong
// file.
//
// `spelled` is a SET since P69: a format may have more than one mechanism for a
// weak definition and say per KIND which a definition takes
// (`weakDefinition.byKind`), and its walker then spells each. The comparison
// stays config vocabulary against config vocabulary — EVERY dialect the
// document can answer with must be one this walker writes — so a document
// that gives some kind a spelling the walker lacks is refused the moment the
// module holds a weak definition, whichever kind that one is.
[[nodiscard]] inline bool
requireWeakDefinitionDialects(AssembledModule const&                 module,
                              ObjectFormatSchema const&              fmt,
                              std::span<WeakDefinitionDialect const> spelled,
                              std::string_view                       where,
                              DiagnosticReporter&                    reporter) {
    if (!moduleDefinesWeakSymbol(module)) return true;

    std::string spelledList;
    for (WeakDefinitionDialect const d : spelled) {
        if (!spelledList.empty()) spelledList += "' or '";
        spelledList += weakDefinitionDialectName(d);
    }
    auto const declared = fmt.weakDefinition();
    if (!declared.has_value()) {
        detail::emit(
            reporter, DiagnosticCode::K_FormatLacksWeakDefinitionDialect,
            std::format(
                "{}: this module defines a WEAK symbol, but format '{}' "
                "declares no 'weakDefinition' block, so it has not said HOW a "
                "weak definition is spelled. Declare 'weakDefinition': {{ "
                "\"dialect\": \"{}\" }} — emitting the body under an assumed "
                "spelling would publish it as a STRONG definition on any "
                "format whose dialect differs. "
                "D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED.",
                where, fmt.name(),
                spelled.empty() ? std::string_view{}
                                : weakDefinitionDialectName(spelled.front())));
        return false;
    }
    for (WeakDefinitionDialect const stated : fmt.weakDefinitionDialectsStated()) {
        bool written = false;
        for (WeakDefinitionDialect const d : spelled) written = written || d == stated;
        if (written) continue;
        detail::emit(
            reporter, DiagnosticCode::K_FormatLacksWeakDefinitionDialect,
            std::format(
                "{}: format '{}' declares weakDefinition dialect '{}', but "
                "this walker spells a weak definition only as '{}'. The "
                "definition is refused rather than re-spelled: a weak body "
                "written under the wrong dialect is published as a STRONG "
                "definition. Fix the declaration, or land the encoder for "
                "'{}'. D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED.",
                where, fmt.name(),
                weakDefinitionDialectName(stated),
                spelledList,
                weakDefinitionDialectName(stated)));
        return false;
    }
    return true;
}

// The gate for a walker that spells ONE dialect — every caller until P69, and
// still the ELF and Mach-O walkers.
[[nodiscard]] inline bool
requireWeakDefinitionDialect(AssembledModule const&    module,
                             ObjectFormatSchema const& fmt,
                             WeakDefinitionDialect     spelled,
                             std::string_view          where,
                             DiagnosticReporter&       reporter) {
    return requireWeakDefinitionDialects(
        module, fmt, std::span<WeakDefinitionDialect const>{&spelled, 1}, where, reporter);
}

// ── A DEFINITION THAT IS THE ONE INSTANCE OF ITS NAME IN THE PROCESS ───────
//    (P69 fold 3c)
//
// A row whose unit states it (`ModuleSymbol::oneInstancePerProcess` — g++'s
// STB_GNU_UNIQUE inline variables, as the ELF reader reads them through its
// document) is written with the binding the OUTPUT document states for it. Two
// ways to get that wrong, two refusals by name (`K_SymbolBindingNotModelled`),
// both before any byte:
//   * the document states no such binding
//     (`ObjectFormatSchema::oneInstancePerProcessReadAs`): written under a plain
//     binding the definition is one instance per IMAGE, and a library loaded
//     beside the program keeps a copy of its own;
//   * the row's binding is not the one the document reads such a definition AS:
//     written with the document's binding, the next read takes it for a
//     definition it is not.
// EVERY writer asks it — ELF, which spells the binding, and PE and Mach-O,
// whose documents state none — so no writer drops the property in silence.
[[nodiscard]] inline bool
moduleStatesOneInstancePerProcess(AssembledModule const& module) noexcept {
    for (auto const& s : module.symbols) {
        if (s.oneInstancePerProcess) return true;
    }
    return false;
}

[[nodiscard]] inline bool
requireOneInstancePerProcessSpelling(AssembledModule const&    module,
                                     ObjectFormatSchema const& fmt,
                                     std::string_view          where,
                                     DiagnosticReporter&       reporter) {
    auto const readAs = fmt.oneInstancePerProcessReadAs();
    for (auto const& s : module.symbols) {
        if (!s.oneInstancePerProcess) continue;
        if (!readAs.has_value()) {
            detail::emit(
                reporter, DiagnosticCode::K_SymbolBindingNotModelled,
                std::format(
                    "{}: '{}' is a definition its unit states is the ONE instance "
                    "of its name in the process (what g++ writes for a C++ inline "
                    "variable or a template's static data member), and format "
                    "'{}' states no binding that spells one. Refused rather than "
                    "written under a plain binding, which would make it one "
                    "instance per image.",
                    where, s.name, fmt.name()));
            return false;
        }
        if (s.binding != *readAs) {
            detail::emit(
                reporter, DiagnosticCode::K_SymbolBindingNotModelled,
                std::format(
                    "{}: '{}' is the one instance of its name in the process, "
                    "bound '{}', and format '{}' reads such a definition as "
                    "'{}'. Refused rather than written with a binding the next "
                    "read takes for a definition it is not.",
                    where, s.name, symbolBindingName(s.binding), fmt.name(),
                    symbolBindingName(*readAs)));
            return false;
        }
    }
    return true;
}

} // namespace dss::link::format
