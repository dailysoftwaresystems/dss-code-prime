#include "link/unit_linker_decisions.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/object_symbol_names.hpp"   // ObjectSymbolNames::hasExternalLinkage

#include <format>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace dss::linker {

namespace {

void warn(DiagnosticReporter& reporter, std::string message) {
    report(reporter, DiagnosticCode::K_LinkerDirectiveIgnored, DiagnosticSeverity::Warning,
           "linker: " + std::move(message));
}

void refuse(DiagnosticReporter& reporter, std::string message) {
    report(reporter, DiagnosticCode::K_LinkerDirectiveUnhonourable, DiagnosticSeverity::Error,
           "linker: " + std::move(message));
}

}  // namespace

std::optional<UnitLinkerDecisions>
decideUnitLinkerRequests(UnitLinkerRequests const& requests,
                         ObjectFormatSchema const& format,
                         ImageRequest const&       programRequest,
                         DiagnosticReporter&       reporter) {
    UnitLinkerDecisions out;
    // A relocatable artifact decides nothing: its final linker reads every request from the tokens the writer hands
    // on (`UnitLinkerRequests::handOn`), the refused ones included -- that linker can honour them.
    if (requests.empty() || !format.isImageFlavor()) return out;
    // What the readers dropped as the reference linkers drop it, said by the link that drops it.
    for (auto const& message : requests.warnings) warn(reporter, message);
    // A program the OS starts, or a library a program loads: the loader reads the stack and heap sizes, and starts
    // the process, from the program's image alone.
    bool const program = format.isExecFlavor();
    bool refused = false;

    std::optional<UnitEntryRequest> entry;
    std::string                     entrySpelled;
    std::unordered_map<std::string, std::pair<std::string, std::string>> mismatchKeys;   // key -> value, spelled
    for (auto const& request : requests.image) {
        std::visit(
            [&](auto const& r) {
                using R = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<R, UnitUnhonourableRequest>) {
                    // The format's vocabulary row is `refused`: its reason says why no DSS image can carry it and
                    // names the row that carries the work. A relocatable artifact would hand it on instead.
                    refuse(reporter, std::format("a linked object's directive '{}' cannot be honoured by this image: "
                                                 "{}",
                                                 request.spelled, r.reason));
                    refused = true;
                } else if constexpr (std::is_same_v<R, UnitDllImageRequest>) {
                    // D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE: link.exe turns its output into a DLL
                    // (✔MEASURED 2026-10-07); the build's target is what a DSS link makes.
                    if (program) {
                        refuse(reporter, std::format(
                            "a linked object's directive '{}' asks for a DLL, and this build makes a program "
                            "(format '{}'): the build's target decides what a DSS link makes. Build the object "
                            "into a shared-library target, or drop the directive.",
                            request.spelled, format.name()));
                        refused = true;
                    }
                    // A link that makes a shared library already satisfies it.
                } else if constexpr (std::is_same_v<R, UnitStackRequest> || std::is_same_v<R, UnitHeapRequest>) {
                    constexpr bool stack = std::is_same_v<R, UnitStackRequest>;
                    if (!program) {
                        warn(reporter, std::format(
                            "a linked object's directive '{}' asks for a {} size, and this build makes a shared "
                            "library (format '{}'): the loader reads the {} size from the program's image alone "
                            "(link.exe writes the DLL's copy, which nothing reads), so it is ignored",
                            request.spelled, stack ? "stack" : "heap", format.name(), stack ? "stack" : "heap"));
                        return;
                    }
                    auto& slot = stack ? out.image.stack : out.image.heap;
                    // The program's own `--stack-reserve` wins over every unit's request, commit and all; between
                    // units the FIRST wins (link.exe; lld-link takes the last).
                    if (stack && programRequest.stackReserveBytes.has_value()) return;
                    if (!slot.has_value()) slot = r.sizes;
                } else if constexpr (std::is_same_v<R, UnitSubsystemRequest>) {
                    out.image.subsystem = r.setting;   // the LAST wins (both linkers)
                } else if constexpr (std::is_same_v<R, UnitEntryRequest>) {
                    if (!program) {
                        // D-LK2-DLL-DLLMAIN-ENTRY: a DSS DLL has no entry yet (AddressOfEntryPoint 0); link.exe
                        // starts a DLL's notifications at the named function.
                        refuse(reporter, std::format(
                            "a linked object's directive '{}' names the entry of a shared library, and a DSS shared "
                            "library has no entry (its loader notifications are not called): build the object into "
                            "a program, or drop the directive.",
                            request.spelled));
                        refused = true;
                        return;
                    }
                    if (!entry.has_value()) {
                        entry        = r;
                        entrySpelled = request.spelled;
                    } else if (entry->symbol != r.symbol) {
                        // link.exe keeps the FIRST and warns LNK4258 on a later, different one (lld-link keeps the
                        // last); one stated twice alike is silent under both (✔MEASURED 2026-10-07).
                        warn(reporter, std::format(
                            "a linked object's directive '{}' names another entry than the earlier '{}': the first "
                            "stands, as link.exe keeps it (LNK4258)",
                            request.spelled, entrySpelled));
                    }
                } else if constexpr (std::is_same_v<R, UnitSectionAttributeRequest>) {
                    out.image.sections.push_back(r.request);   // in order (both linkers)
                } else if constexpr (std::is_same_v<R, UnitChecksumRequest>) {
                    out.image.checksum = true;
                } else if constexpr (std::is_same_v<R, UnitImageVersionRequest>) {
                    out.image.imageVersion = r.version;   // the LAST wins
                } else if constexpr (std::is_same_v<R, UnitImageBaseRequest>) {
                    if (!out.image.imageBase.has_value()) out.image.imageBase = r.address;   // the FIRST wins
                } else if constexpr (std::is_same_v<R, UnitSectionAlignmentRequest>) {
                    out.image.sectionAlignment = r.bytes;   // the LAST wins
                } else if constexpr (std::is_same_v<R, UnitMismatchCheck>) {
                    // link.exe and lld-link refuse a disagreement (LNK2038 / "mismatch detected"), GNU ld links it
                    // (✔MEASURED 2026-10-06/07): acceptance is the union's, so a DSS link links it and says so.
                    auto const [it, inserted] = mismatchKeys.try_emplace(r.key, r.value, request.spelled);
                    if (!inserted && it->second.first != r.value) {
                        warn(reporter, std::format(
                            "the linked objects disagree on '{}': '{}' and '{}' -- link.exe and lld-link refuse "
                            "the link (LNK2038), GNU ld links it, and so does this one",
                            r.key, it->second.second, request.spelled));
                    }
                }
            },
            request.value);
    }
    if (refused) return std::nullopt;

    // A runtime startup is the target's own; any other names the function the image starts at.
    if (entry.has_value() && !entry->runtimeStartup) out.entrySymbol = entry->symbol;

    // EXPORTS, link-wide: one per exported name. A name stated again for the same definition is one export (both
    // linkers); for ANOTHER definition, link.exe warns LNK4197 and keeps the FIRST, and so does lld-link, silently
    // (✔MEASURED 2026-10-07).
    std::unordered_map<std::string, std::size_t> exportByName;
    for (auto const& e : requests.exports) {
        auto const [it, inserted] = exportByName.try_emplace(e.exportedName, out.image.exports.size());
        if (inserted) {
            out.image.exports.push_back(e);
            continue;
        }
        auto const& first = out.image.exports[it->second];
        if (first.internalName != e.internalName) {
            warn(reporter, std::format(
                "a linked object's directive '{}' exports '{}' again, for '{}' where '{}' exported '{}': the first "
                "stands, as link.exe keeps it (LNK4197)",
                e.spelled, e.exportedName, e.internalName, first.spelled, first.internalName));
        }
    }

    // HIDES, link-wide, once each.
    std::unordered_set<std::string> hidden;
    for (auto const& name : requests.hides) {
        if (hidden.insert(name).second) out.hides.push_back(name);
    }

    // The writer must realize what was decided for it; one that cannot is refused by name rather than handed
    // requests it would drop.
    auto const* backend = format.backend();
    if (!out.image.empty() && (backend == nullptr || !backend->realizesDirectiveImageSettings())) {
        refuse(reporter, std::format(
            "the linked objects' directives ask the image for a stack, heap, subsystem, version, base, alignment, "
            "checksum, section attributes or exports, and the writer of format '{}' realizes none of them: link "
            "the objects into a format whose writer does, or drop the directives.",
            format.name()));
        return std::nullopt;
    }
    return out;
}

std::optional<bool>
applyUnitLinkerDecisions(AssembledModule const&     in,
                         UnitLinkerDecisions const& decisions,
                         AssembledModule&           out,
                         DiagnosticReporter&        reporter) {
    bool changes = false;
    std::unordered_set<std::string> const hidden{decisions.hides.begin(), decisions.hides.end()};
    for (auto const& ms : in.symbols) {
        if (hidden.contains(ms.name) && link::format::ObjectSymbolNames::hasExternalLinkage(ms)
            && ms.visibility != SymbolVisibility::Hidden) {
            changes = true;
        }
    }
    std::optional<std::size_t> entryIndex;
    if (decisions.entrySymbol.has_value()) {
        std::string const& name = *decisions.entrySymbol;
        ModuleSymbol const* definition = nullptr;
        for (auto const& ms : in.symbols) {
            if (ms.name == name && link::format::ObjectSymbolNames::hasExternalLinkage(ms)) definition = &ms;
        }
        if (definition != nullptr) {
            for (std::size_t i = 0; i < in.functions.size(); ++i) {
                if (in.functions[i].symbol == definition->symbol) entryIndex = i;
            }
        }
        if (!entryIndex.has_value()) {
            // A datum: both reference linkers take one, and both images fault at start (0xC0000005, ✔MEASURED
            // 2026-10-07). Nothing: LNK2001 / LNK1561.
            report(reporter, DiagnosticCode::K_LinkerDirectiveUnhonourable, DiagnosticSeverity::Error,
                   std::format("linker: a linked object's directive names '{}' as the image's entry, and {}: the "
                               "image would start nowhere it can run. Define the entry function in a linked unit, "
                               "or drop the directive.",
                               name,
                               definition != nullptr ? "it is a datum, not a function"
                                                     : "no unit of the link defines a function of that name"));
            return std::nullopt;
        }
        if (in.imageEntryOverride.has_value()) {
            report(reporter, DiagnosticCode::K_LinkerDirectiveUnhonourable, DiagnosticSeverity::Error,
                   std::format("linker: a linked object's directive names '{}' as the image's entry, and the module "
                               "already carries an image entry override (functions[{}]): one image has one entry.",
                               name, *in.imageEntryOverride));
            return std::nullopt;
        }
        changes = true;
    }
    if (!changes) return false;
    out = in;
    for (auto& ms : out.symbols) {
        if (hidden.contains(ms.name) && link::format::ObjectSymbolNames::hasExternalLinkage(ms)) {
            ms.visibility = SymbolVisibility::Hidden;
        }
    }
    if (entryIndex.has_value()) out.imageEntryOverride = *entryIndex;
    return true;
}

void dropPulledMemberEntryRequests(UnitLinkerRequests& requests) {
    std::vector<UnitImageRequest> kept;
    kept.reserve(requests.image.size());
    for (auto& request : requests.image) {
        auto const* entry = std::get_if<UnitEntryRequest>(&request.value);
        if (entry == nullptr) {
            kept.push_back(std::move(request));
            continue;
        }
        if (!entry->runtimeStartup) {
            requests.warnings.push_back(std::format(
                "an archive member's directive '{}' names the image's entry, and the archive search pulled that "
                "member: link.exe fixes the entry before it searches an archive (GNU ld reads no `/ENTRY:`), so the "
                "image starts in its target's startup as theirs do. Name the object to the link itself for its "
                "entry to count.",
                request.spelled));
        }
    }
    requests.image = std::move(kept);
}

}  // namespace dss::linker
