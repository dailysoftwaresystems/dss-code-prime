#pragma once

#include "asm/asm.hpp"                          // AssembledModule
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"    // WeakDefinitionDialect
#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"            // detail::emit
#include "link/format/object_symbol_names.hpp"  // ObjectSymbolNames
#include "link/object_format_schema.hpp"

#include <format>
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
[[nodiscard]] inline bool
requireWeakDefinitionDialect(AssembledModule const&    module,
                             ObjectFormatSchema const& fmt,
                             WeakDefinitionDialect     spelled,
                             std::string_view          where,
                             DiagnosticReporter&       reporter) {
    if (!moduleDefinesWeakSymbol(module)) return true;

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
                where, fmt.name(), weakDefinitionDialectName(spelled)));
        return false;
    }
    if (declared->dialect != spelled) {
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
                weakDefinitionDialectName(declared->dialect),
                weakDefinitionDialectName(spelled),
                weakDefinitionDialectName(declared->dialect)));
        return false;
    }
    return true;
}

} // namespace dss::link::format
