#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"     // SymbolVisibility, kSymbolVisibilityTable
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/lowering/cst_to_hir.hpp"     // CstToHirResult

#include <algorithm>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// ★★ THE ONE STEP THAT SETTLES A VISIBILITY THE TARGET'S FORMAT CANNOT WRITE (P69 fold f26b, lane `cs`).
//
// The C front end takes all four GNU visibilities, and the HIR lowering records each declaration's as written — it
// knows no object format. A symbol table that has no field for one of them writes another in its place, and what
// each format's compilers write is the FORMAT DOCUMENT's to say (`visibilityWrittenAs`, see `VisibilityStandIn` in
// link/object_format_schema.hpp, where the references' answers are measured). The pipeline runs THIS once per unit,
// right after the unit is lowered to HIR and before any tier reads its linkage (`buildCuHirImpl`,
// program/compile_pipeline.cpp), with the target format's statement handed in as plain data: every declaration —
// and every alias — whose visibility the format cannot write is given the one the document states, and the program
// is told, as the format's compilers tell it (`H_VisibilityWrittenAsAnother`, a warning). So the writers only ever
// see a visibility their format writes, and what is written is the document's answer, never a writer's own mapping:
// a document that states another stand-in writes another symbol.
//
// `standIns`: any range of records with `.unwritable` and `.writtenAs` (`SymbolVisibility`) —
// `ObjectFormatSchema::visibilityStandIns()`; empty, nothing is settled. `nameOf`: a symbol's declared name, for the
// warning. Diagnostics come in declaration order (node id), the aliases after. An alias's linkage is held twice —
// on its declaration and on the alias record the writers read (`HirSymbolAlias`) — and both are settled; the
// program is told ONCE, at the declaration, and by the alias's name only where its declaration was not told.
namespace dss {

template <class StandIns, class NameOf>
void settleVisibilitiesWrittenAs(CstToHirResult& unit, StandIns const& standIns, std::string_view formatName,
                                 NameOf&& nameOf, DiagnosticReporter& reporter) {
    auto const standInFor = [&](SymbolVisibility v) -> std::optional<SymbolVisibility> {
        for (auto const& s : standIns) {
            if (s.unwritable == v) return s.writtenAs;
        }
        return std::nullopt;
    };
    if (std::begin(standIns) == std::end(standIns)) return;

    Hir const& hir      = unit.hir;
    auto const symbolOf = [&](HirNodeId node) -> SymbolId {
        switch (hir.kind(node)) {
            case HirKind::Function:       return hir.functionSymbol(node);
            case HirKind::Global:         return hir.globalSymbol(node);
            case HirKind::ExternFunction: return hir.externFunctionSymbol(node);
            case HirKind::ExternGlobal:   return hir.externGlobalSymbol(node);
            default:                      return SymbolId{};
        }
    };
    auto const warn = [&](std::string const& what, SymbolVisibility stated, SymbolVisibility written,
                          HirSourceLoc const* at) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::H_VisibilityWrittenAsAnother;
        d.severity = DiagnosticSeverity::Warning;
        if (at != nullptr && at->isPresent()) {
            d.buffer = at->buffer;
            d.span   = at->span;
        }
        d.actual = std::format(
            "{} states visibility '{}', which format '{}' cannot write; it is written as '{}', as the format's "
            "compilers write it (the format document's `visibilityWrittenAs`)",
            what, kSymbolVisibilityTable.name(stated), formatName, kSymbolVisibilityTable.name(written));
        reporter.report(std::move(d));
    };

    std::vector<HirNodeId> settled;
    for (auto const& [id, attr] : unit.linkageMap) {
        if (standInFor(attr.visibility).has_value()) settled.push_back(id);
    }
    std::sort(settled.begin(), settled.end(), [](HirNodeId a, HirNodeId b) { return a.v < b.v; });
    std::unordered_set<std::uint32_t> told;   // the symbols whose declaration was told
    for (HirNodeId const node : settled) {
        LinkageAttr&           attr    = unit.linkageMap.get(node);
        SymbolVisibility const stated  = attr.visibility;
        SymbolVisibility const written = *standInFor(stated);
        attr.visibility                = written;
        SymbolId const   sym  = symbolOf(node);
        if (sym.valid()) told.insert(sym.v);
        std::string const name = sym.valid() ? std::string{nameOf(sym)} : std::string{};
        warn(name.empty() ? std::string{"a declaration"} : std::format("'{}'", name), stated, written,
             unit.sourceMap.has(node) ? &unit.sourceMap.get(node) : nullptr);
    }
    for (HirSymbolAlias& alias : unit.symbolAliases) {
        auto const written = standInFor(alias.linkage.visibility);
        if (!written.has_value()) continue;
        SymbolVisibility const stated = alias.linkage.visibility;
        alias.linkage.visibility      = *written;
        if (told.contains(alias.alias.v)) continue;   // told at its declaration
        std::string const name        = std::string{nameOf(alias.alias)};
        warn(std::format("the alias '{}'", name), stated, *written, nullptr);
    }
}

}  // namespace dss
