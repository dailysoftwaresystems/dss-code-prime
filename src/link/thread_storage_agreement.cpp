#include "link/thread_storage_agreement.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// See the header for the rule, where it sits and what it does not judge.
namespace dss::linker {

namespace {

// How one unit's relocations reach one of its symbols: the first tls-flagged
// kind and the first ordinary kind naming it (a data item's relocation is an
// address, so it is ordinary), and the first function holding one.
struct Reach {
    std::optional<std::string> tlsKind;
    std::optional<std::string> ordinaryKind;
    std::optional<SymbolId>    firstFunction;
};

[[nodiscard]] std::string kindName(TargetSchema const& target, RelocationKind kind) {
    auto const* info = target.relocationInfo(kind);
    return info != nullptr ? info->name : std::format("kind {}", kind.v);
}

[[nodiscard]] std::unordered_map<std::uint32_t, Reach>
reachIn(AssembledModule const& m, TargetSchema const& target) {
    std::unordered_map<std::uint32_t, Reach> out;
    for (auto const& fn : m.functions) {
        for (auto const& rel : fn.relocations) {
            auto const* info = target.relocationInfo(rel.kind);
            Reach&      r    = out[rel.target.v];
            auto&       kind = (info != nullptr && info->tls) ? r.tlsKind : r.ordinaryKind;
            if (!kind.has_value()) kind = kindName(target, rel.kind);
            if (!r.firstFunction.has_value()) r.firstFunction = fn.symbol;
        }
    }
    for (auto const& d : m.dataItems) {
        for (auto const& rel : d.relocations) {
            Reach& r = out[rel.target.v];
            if (!r.ordinaryKind.has_value()) r.ordinaryKind = kindName(target, rel.kind);
        }
    }
    return out;
}

[[nodiscard]] std::string nameIn(AssembledModule const& m, SymbolId s) {
    for (auto const& row : m.symbols) {
        if (row.symbol == s) return row.name;
    }
    return {};
}

enum class DefinitionShape { ThreadLocalObject, OrdinaryObject, Function };

[[nodiscard]] DefinitionShape shapeOf(AssembledModule const& m, SymbolId s) {
    for (auto const& d : m.dataItems) {
        if (d.symbol != s) continue;
        bool const threadLocal = d.section == DataSectionKind::Tdata || d.section == DataSectionKind::Tbss;
        return threadLocal ? DefinitionShape::ThreadLocalObject : DefinitionShape::OrdinaryObject;
    }
    for (auto const& fn : m.functions) {
        if (fn.symbol == s) return DefinitionShape::Function;
    }
    return DefinitionShape::OrdinaryObject;
}

} // namespace

std::size_t reportThreadStorageDisagreements(std::span<AssembledModule const>         modules,
                                             std::span<LinkedImage::CrossCuRef const> references,
                                             TargetSchema const&                      targetSchema,
                                             DiagnosticReporter&                      reporter) {
    std::unordered_map<std::uint32_t, std::size_t> indexOfCu;
    for (std::size_t i = 0; i < modules.size(); ++i) indexOfCu.emplace(modules[i].cuId.v, i);
    std::unordered_map<std::size_t, std::unordered_map<std::uint32_t, Reach>> reachByModule;
    std::unordered_set<LinkedSymbolKey>                                       judged;
    std::size_t                                                               reported = 0;
    for (auto const& ref : references) {
        if (!judged.insert(ref.reference).second) continue;
        auto const ri = indexOfCu.find(ref.reference.cuId.v);
        auto const di = indexOfCu.find(ref.definition.cuId.v);
        // A pairing outside the span is the resolver's invariant breach, which the merge reports by name.
        if (ri == indexOfCu.end() || di == indexOfCu.end()) continue;
        AssembledModule const& refUnit = modules[ri->second];
        AssembledModule const& defUnit = modules[di->second];
        ExternImport const*    row     = nullptr;
        for (auto const& e : refUnit.externImports) {
            if (e.symbol == ref.reference.symbol) {
                row = &e;
                break;
            }
        }
        if (row == nullptr) continue;   // likewise the merge's to report
        auto rit = reachByModule.find(ri->second);
        if (rit == reachByModule.end()) rit = reachByModule.emplace(ri->second, reachIn(refUnit, targetSchema)).first;
        auto const   found = rit->second.find(ref.reference.symbol.v);
        Reach const  none{};
        Reach const& r = found != rit->second.end() ? found->second : none;
        // Named by no relocation of its unit: judged only where its object's own symbol record states the storage
        // duration (the header says why, and which reference linkers judge one).
        bool const reached = r.tlsKind.has_value() || r.ordinaryKind.has_value();
        if (!reached && !row->recordStatesStorageDuration) continue;
        bool const            refIsTls = row->isThreadLocal || r.tlsKind.has_value();
        DefinitionShape const shape    = shapeOf(defUnit, ref.definition.symbol);
        bool const            defIsTls = shape == DefinitionShape::ThreadLocalObject;
        if (refIsTls == defIsTls) continue;

        std::string const fnName = r.firstFunction.has_value() ? nameIn(refUnit, *r.firstFunction) : std::string{};
        std::string const who    = fnName.empty() ? std::format("CU #{}", refUnit.cuId.v)
                                                  : std::format("CU #{} (in `{}`)", refUnit.cuId.v, fnName);
        std::string message;
        if (!refIsTls) {
            std::string const how =
                reached ? std::format("refers to it as an ORDINARY object (through the '{}' relocation)",
                                      r.ordinaryKind.value_or(std::string{"?"}))
                        : std::string{"holds it as an ORDINARY object (its object's symbol record states one -- a "
                                      "COMMON, a tentative definition -- though no code of the unit reads it)"};
            message = std::format(
                "symbol '{}': {} {}, but the "
                "definition the link binds it to, in CU #{}, has THREAD STORAGE DURATION. C23 6.7.2p3 requires "
                "`thread_local` on every declaration of one object, and neither access can stand in for the other: an "
                "ordinary reference reaches ONE address for the whole process, and a thread-local object is one object "
                "per thread, at an offset from the thread pointer. GNU ld refuses the same link (\"TLS definition in "
                "... mismatches non-TLS reference in ...\"). Declare the reference `thread_local`, or link the "
                "ordinary definition it was written against.",
                row->mangledName, who, how, defUnit.cuId.v);
        } else {
            std::string const how =
                row->isThreadLocal
                    ? std::string{"declares it `thread_local`"}
                    : std::format("reaches it as a THREAD-LOCAL object (through the '{}' relocation)",
                                  r.tlsKind.value_or(std::string{"?"}));
            message = std::format(
                "symbol '{}': {} {}, but the definition the link binds it to, in CU #{}, is an ORDINARY {}. C23 "
                "6.7.2p3 requires `thread_local` on every declaration of one object, and neither access can stand in "
                "for the other: a thread-local reference reaches its object at an offset from the thread pointer, "
                "and an ordinary {} is at one address for the whole process. GNU ld refuses the same link (\"TLS "
                "reference in ... mismatches non-TLS definition in ...\"). Define it `thread_local`, or declare the "
                "reference without it.",
                row->mangledName, who, how, defUnit.cuId.v,
                shape == DefinitionShape::Function ? "function" : "object",
                shape == DefinitionShape::Function ? "function" : "object");
        }
        dss::report(reporter, DiagnosticCode::K_ExternImportAttributeConflict, DiagnosticSeverity::Error,
                    std::move(message));
        ++reported;
    }
    return reported;
}

} // namespace dss::linker
