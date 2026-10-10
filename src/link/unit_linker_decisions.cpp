#include "link/unit_linker_decisions.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/object_symbol_names.hpp"   // ObjectSymbolNames::hasExternalLinkage
#include "link/fresh_symbol_ids.hpp"             // maxExistingSymbolIdV — the ONE taken-id scan

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

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

// WHO stated a directive — the object or archive member its request was read from — which every diagnostic about
// the request names (review-xa4 NIT 11). A unit handed to the link in memory carries no name.
[[nodiscard]] std::string who(std::string_view unit) {
    return unit.empty() ? std::string{"a linked object"} : std::format("'{}'", unit);
}

// "the directive '/STACK:1' of 'main.obj'": the OTHER party of a diagnostic that names two requests.
[[nodiscard]] std::string directiveOf(std::string_view spelled, std::string_view unit) {
    return std::format("the directive '{}' of {}", spelled, who(unit));
}

// Two requests for one thing whose vocabulary rows state DIFFERENT precedences have no order between them. The
// format's loader refuses two options of one meaning that disagree, so this is the net under a vocabulary built in
// memory.
void refusePrecedenceSplit(DiagnosticReporter& reporter, std::string_view heldSpelled, std::string_view heldUnit,
                           UnitRequestPrecedence held, std::string_view spelled, std::string_view unit,
                           UnitRequestPrecedence stated, std::string_view what) {
    refuse(reporter, std::format(
        "{}: the directive '{}' and {} both ask the image for {}, under two precedences ('{}' and '{}'): the "
        "format's directive vocabulary gives one request one 'unitPrecedence', and no order settles two.",
        who(unit), spelled, directiveOf(heldSpelled, heldUnit), what, kUnitRequestPrecedenceTable.name(stated),
        kUnitRequestPrecedenceTable.name(held)));
}

// A request that loses to another unit's DIFFERENT one is said so — it is never dropped in silence — naming both
// directives, both units, which stands and the precedence the format states for the option.
void warnDropped(DiagnosticReporter& reporter, std::string_view droppedSpelled, std::string_view droppedUnit,
                 std::string_view standsSpelled, std::string_view standsUnit, std::string_view what,
                 bool firstStands, UnitRequestPrecedence precedence, ObjectFormatSchema const& format) {
    warn(reporter, std::format(
        "{}: the directive '{}' asks the image for another {} than {}: the {} one stands -- format '{}' gives the "
        "option the precedence '{}' -- and the other is dropped",
        who(droppedUnit), droppedSpelled, what, directiveOf(standsSpelled, standsUnit),
        firstStands ? "first" : "later", format.name(), kUnitRequestPrecedenceTable.name(precedence)));
}

// One image setting that holds ONE value, as the link holds it while it reads the units' requests in unit order:
// the request that stands so far.
template <class V>
struct Standing {
    std::optional<V>        value;
    UnitImageRequest const* request = nullptr;
};

// Settle `request`, whose value is `value`, against the setting's standing request by the precedence the request's
// own vocabulary row states: `first` keeps the standing one; `last` takes the new one, and so does `inOrder` (each
// applied in turn, a single value ends at the last). Where the two values differ the one that does not stand is
// warned about; one stated twice alike is silent. False after refusing two requests of two precedences.
template <class V>
[[nodiscard]] bool settle(Standing<V>& standing, V const& value, UnitImageRequest const& request,
                          std::string_view what, ObjectFormatSchema const& format, DiagnosticReporter& reporter) {
    if (standing.request == nullptr) {
        standing.value   = value;
        standing.request = &request;
        return true;
    }
    UnitImageRequest const& held = *standing.request;
    if (held.precedence != request.precedence) {
        refusePrecedenceSplit(reporter, held.spelled, held.unit, held.precedence, request.spelled, request.unit,
                              request.precedence, std::format("its {}", what));
        return false;
    }
    bool const firstStands = request.precedence == UnitRequestPrecedence::First;
    if (!(*standing.value == value)) {
        UnitImageRequest const& dropped = firstStands ? request : held;
        UnitImageRequest const& stands  = firstStands ? held : request;
        warnDropped(reporter, dropped.spelled, dropped.unit, stands.spelled, stands.unit, what, firstStands,
                    request.precedence, format);
    }
    if (!firstStands) {
        standing.value   = value;
        standing.request = &request;
    }
    return true;
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
    // A program the OS starts, or a library a program loads.
    bool const program = format.isExecFlavor();
    bool refused = false;

    // What a link that makes a SHARED LIBRARY does with a request is its vocabulary row's answer
    // (`SharedLibraryDisposition`), with the row's reason: taken as a program takes it, warned and dropped, or
    // refused by name. True when the request is to be decided.
    auto const taken = [&](std::string_view spelled, std::string_view unit, SharedLibraryDisposition disposition,
                           std::string_view reason) {
        if (program || disposition == SharedLibraryDisposition::Honoured) return true;
        if (disposition == SharedLibraryDisposition::Ignored) {
            warn(reporter, std::format(
                "{}: the directive '{}' is ignored, because this build makes a shared library (format '{}'): {}",
                who(unit), spelled, format.name(), reason));
        } else {
            refuse(reporter, std::format(
                "{}: the directive '{}' cannot be honoured, because this build makes a shared library (format "
                "'{}'): {}",
                who(unit), spelled, format.name(), reason));
            refused = true;
        }
        return false;
    };

    Standing<ReserveCommit>        stack;
    Standing<ReserveCommit>        heap;
    Standing<UnitSubsystemRequest> subsystem;
    Standing<UnitEntryRequest>     entry;
    Standing<VersionPair>          imageVersion;
    Standing<std::uint64_t>        imageBase;
    Standing<std::uint32_t>        sectionAlignment;
    struct SectionEdit {
        SectionAttributeRequest request;
        UnitImageRequest const* from = nullptr;
    };
    std::vector<SectionEdit> sections;
    std::unordered_map<std::string, UnitImageRequest const*> mismatchKeys;   // key -> the first request stating it
    auto const settled = [&](bool ok) { refused = refused || !ok; };

    for (auto const& request : requests.image) {
        if (!taken(request.spelled, request.unit, request.inSharedLibrary, request.inSharedLibraryReason)) continue;
        std::visit(
            [&](auto const& r) {
                using R = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<R, UnitUnhonourableRequest>) {
                    // The format's vocabulary row is `refused`: its reason says why no DSS image can carry it and
                    // names the row that carries the work. A relocatable artifact would hand it on instead.
                    refuse(reporter, std::format("{}: the directive '{}' cannot be honoured by this image: {}",
                                                 who(request.unit), request.spelled, r.reason));
                    refused = true;
                } else if constexpr (std::is_same_v<R, UnitDllImageRequest>) {
                    // D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE: the request IS "make a shared library"
                    // (link.exe turns its output into a DLL, ✔MEASURED 2026-10-07); the build's target is what a
                    // DSS link makes, so a link that makes one already satisfies it and a program's refuses it.
                    if (program) {
                        refuse(reporter, std::format(
                            "{}: the directive '{}' asks for a DLL, and this build makes a program (format '{}'): "
                            "the build's target decides what a DSS link makes. Build the object into a "
                            "shared-library target, or drop the directive.",
                            who(request.unit), request.spelled, format.name()));
                        refused = true;
                    }
                } else if constexpr (std::is_same_v<R, UnitStackRequest>) {
                    // The program's own `--stack-reserve` wins over every unit's request, commit and all: the
                    // user's explicit choice over an object's (DSS's rule, as link.exe's command line beats a
                    // directive).
                    if (programRequest.stackReserveBytes.has_value()) return;
                    settled(settle(stack, r.sizes, request, "stack size", format, reporter));
                } else if constexpr (std::is_same_v<R, UnitHeapRequest>) {
                    settled(settle(heap, r.sizes, request, "heap size", format, reporter));
                } else if constexpr (std::is_same_v<R, UnitSubsystemRequest>) {
                    settled(settle(subsystem, r, request, "subsystem", format, reporter));
                } else if constexpr (std::is_same_v<R, UnitEntryRequest>) {
                    settled(settle(entry, r, request, "entry", format, reporter));
                } else if constexpr (std::is_same_v<R, UnitSectionAttributeRequest>) {
                    // A section's attribute edits: `inOrder` applies every one, in unit order (they compose);
                    // `first` and `last` keep one request per section name.
                    auto const prior = std::find_if(sections.begin(), sections.end(), [&](SectionEdit const& s) {
                        return s.request.section == r.request.section;
                    });
                    if (prior != sections.end() && prior->from->precedence != request.precedence) {
                        refusePrecedenceSplit(reporter, prior->from->spelled, prior->from->unit,
                                              prior->from->precedence, request.spelled, request.unit,
                                              request.precedence,
                                              std::format("the attributes of its section '{}'", r.request.section));
                        refused = true;
                        return;
                    }
                    if (prior == sections.end() || request.precedence == UnitRequestPrecedence::InOrder) {
                        sections.push_back(SectionEdit{r.request, &request});
                        return;
                    }
                    bool const firstStands = request.precedence == UnitRequestPrecedence::First;
                    if (prior->request.setMask != r.request.setMask
                        || prior->request.clearMask != r.request.clearMask) {
                        UnitImageRequest const& dropped = firstStands ? request : *prior->from;
                        UnitImageRequest const& stands  = firstStands ? *prior->from : request;
                        warnDropped(reporter, dropped.spelled, dropped.unit, stands.spelled, stands.unit,
                                    std::format("set of attributes for its section '{}'", r.request.section),
                                    firstStands, request.precedence, format);
                    }
                    if (!firstStands) {
                        sections.erase(prior);
                        sections.push_back(SectionEdit{r.request, &request});
                    }
                } else if constexpr (std::is_same_v<R, UnitChecksumRequest>) {
                    out.image.checksum = true;   // a flag: any request raises it, whatever the precedence
                } else if constexpr (std::is_same_v<R, UnitImageVersionRequest>) {
                    settled(settle(imageVersion, r.version, request, "image version", format, reporter));
                } else if constexpr (std::is_same_v<R, UnitImageBaseRequest>) {
                    settled(settle(imageBase, r.address, request, "preferred base", format, reporter));
                } else if constexpr (std::is_same_v<R, UnitSectionAlignmentRequest>) {
                    settled(settle(sectionAlignment, r.bytes, request, "section alignment", format, reporter));
                } else if constexpr (std::is_same_v<R, UnitMismatchCheck>) {
                    // link.exe and lld-link refuse a disagreement and GNU ld links it (✔MEASURED 2026-10-06/07):
                    // acceptance is the union's, so a DSS link links it and says so.
                    auto const [it, inserted] = mismatchKeys.try_emplace(r.key, &request);
                    if (inserted) return;
                    auto const& first = std::get<UnitMismatchCheck>(it->second->value);
                    if (first.value != r.value) {
                        warn(reporter, std::format(
                            "the linked objects disagree on '{}': {} and {} -- link.exe and lld-link refuse the "
                            "link, GNU ld links it, and so does this one",
                            r.key, directiveOf(it->second->spelled, it->second->unit),
                            directiveOf(request.spelled, request.unit)));
                    }
                }
            },
            request.value);
    }
    // A value no DSS image can take is refused where it STANDS, and only there: one another unit's request
    // replaces was dropped above, as the format's own linker drops it.
    if (subsystem.request != nullptr && !subsystem.value->unsupported.empty()) {
        refuse(reporter, std::format("{}: the directive '{}' cannot be honoured by this image: {}",
                                     who(subsystem.request->unit), subsystem.request->spelled,
                                     subsystem.value->unsupported));
        refused = true;
    }
    if (entry.request != nullptr && !entry.value->unsupported.empty()) {
        refuse(reporter, std::format("{}: the directive '{}' cannot be honoured by this image: {}",
                                     who(entry.request->unit), entry.request->spelled, entry.value->unsupported));
        refused = true;
    }

    // EXPORTS, link-wide: one per exported name. A name stated again for the same definition is one export; for
    // ANOTHER definition, the row's precedence says which stands (✔MEASURED 2026-10-07: link.exe and lld-link both
    // keep the first), and the other is warned about.
    std::unordered_map<std::string, std::size_t> exportByName;
    for (auto const& e : requests.exports) {
        if (!taken(e.spelled, e.unit, e.inSharedLibrary, e.inSharedLibraryReason)) continue;
        auto const [it, inserted] = exportByName.try_emplace(e.exportedName, out.image.exports.size());
        if (inserted) {
            out.image.exports.push_back(e);
            continue;
        }
        UnitExportRequest& held = out.image.exports[it->second];
        if (held.precedence != e.precedence) {
            refusePrecedenceSplit(reporter, held.spelled, held.unit, held.precedence, e.spelled, e.unit,
                                  e.precedence, std::format("its export '{}'", e.exportedName));
            refused = true;
            continue;
        }
        bool const firstStands = e.precedence == UnitRequestPrecedence::First;
        if (held.internalName != e.internalName) {
            UnitExportRequest const& dropped = firstStands ? e : held;
            UnitExportRequest const& stands  = firstStands ? held : e;
            warnDropped(reporter, dropped.spelled, dropped.unit, stands.spelled, stands.unit,
                        std::format("definition under its export '{}' ('{}', not '{}')", e.exportedName,
                                    dropped.internalName, stands.internalName),
                        firstStands, e.precedence, format);
        }
        if (!firstStands) held = e;
    }
    if (refused) return std::nullopt;

    out.image.stack            = stack.value;
    out.image.heap             = heap.value;
    out.image.imageVersion     = imageVersion.value;
    out.image.imageBase        = imageBase.value;
    out.image.sectionAlignment = sectionAlignment.value;
    if (subsystem.value.has_value()) out.image.subsystem = subsystem.value->setting;
    for (auto& s : sections) out.image.sections.push_back(std::move(s.request));
    // A runtime startup is the target's own; any other names the function the image starts at.
    if (entry.value.has_value() && !entry.value->runtimeStartup) {
        out.entrySymbol   = entry.value->symbol;
        out.entryStatedBy = std::format("{}: the directive '{}'", who(entry.request->unit), entry.request->spelled);
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
        std::vector<std::string> units;
        auto const note = [&](std::string const& unit) {
            if (std::find(units.begin(), units.end(), unit) == units.end()) units.push_back(unit);
        };
        for (auto const& r : requests.image) note(r.unit);
        for (auto const& e : requests.exports) note(e.unit);
        std::string stated;
        for (auto const& unit : units) stated += (stated.empty() ? "" : ", ") + who(unit);
        refuse(reporter, std::format(
            "the directives of {} ask the image for a stack, heap, subsystem, version, base, alignment, checksum, "
            "section attributes or exports, and the writer of format '{}' realizes none of them: link the objects "
            "into a format whose writer does, or drop the directives.",
            stated, format.name()));
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
        std::string const  statedBy =
            decisions.entryStatedBy.empty() ? std::string{"a linked object's directive"} : decisions.entryStatedBy;
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
            // 2026-10-07). Nothing: both refuse the name as unresolved.
            report(reporter, DiagnosticCode::K_LinkerDirectiveUnhonourable, DiagnosticSeverity::Error,
                   std::format("linker: {} names '{}' as the image's entry, and {}: the image would start nowhere "
                               "it can run. Define the entry function in a linked unit, or drop the directive.",
                               statedBy, name,
                               definition != nullptr ? "it is a datum, not a function"
                                                     : "no unit of the link defines a function of that name"));
            return std::nullopt;
        }
        if (in.imageEntryOverride.has_value()) {
            report(reporter, DiagnosticCode::K_LinkerDirectiveUnhonourable, DiagnosticSeverity::Error,
                   std::format("linker: {} names '{}' as the image's entry, and the module already carries an image "
                               "entry override (functions[{}]): one image has one entry.",
                               statedBy, name, *in.imageEntryOverride));
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
        // The C runtime's own startup names the startup the image has anyway; any other entry — a function, or a
        // runtime startup no DSS image provides — is one the image will not start at, and the link says so.
        if (!entry->runtimeStartup) {
            requests.warnings.push_back(std::format(
                "{}: the directive '{}' names the image's entry, and the archive search pulled that member: "
                "link.exe fixes the entry before it searches an archive (GNU ld reads no `/ENTRY:`), so the image "
                "starts in its target's startup as theirs do. Name the object to the link itself for its entry to "
                "count.",
                who(request.unit), request.spelled));
        }
    }
    requests.image = std::move(kept);
}

void requireEntryReference(std::span<AssembledModule> namedUnits) {
    // WHICH request stands, by the precedence each request's own vocabulary row states — the question
    // `decideUnitLinkerRequests` settles for the image (`settle`: `first` keeps the standing one, `last` and
    // `inOrder` take the later), asked here of the units named to the link, in their order, before the archive
    // search runs. Two requests of two precedences have no order between them: that link is refused by name by the
    // decision, and nothing is required of it here.
    AssembledModule*        standingUnit = nullptr;
    UnitImageRequest const* standing     = nullptr;
    for (auto& unit : namedUnits) {
        for (auto const& request : unit.linkerRequests.image) {
            if (std::get_if<UnitEntryRequest>(&request.value) == nullptr) continue;
            if (standing != nullptr) {
                if (standing->precedence != request.precedence) return;
                if (request.precedence == UnitRequestPrecedence::First) continue;
            }
            standing     = &request;
            standingUnit = &unit;
        }
    }
    if (standing == nullptr) return;
    AssembledModule& unit  = *standingUnit;
    auto const&      entry = std::get<UnitEntryRequest>(standing->value);
    if (entry.runtimeStartup || !entry.unsupported.empty()) return;
    std::string const name = entry.symbol;   // `standing` points into the unit this function is about to grow
    for (auto const& ms : unit.symbols) {
        if (ms.name == name && link::format::ObjectSymbolNames::hasExternalLinkage(ms)) return;   // its own definition
    }
    for (auto& e : unit.externImports) {
        if (e.mangledName != name) continue;
        // A COMMON of the name is the unit's own (tentative) definition — a datum, which the entry refuses by
        // name; any other row is its reference, now required.
        if (e.commonSize == 0u) e.requiredByDirective = true;
        return;
    }
    ExternImport required;
    required.symbol              = SymbolId{maxExistingSymbolIdV(unit) + 1u};
    required.mangledName         = name;
    // A directive states no code-vs-data kind; the definition the link finds decides it.
    required.kindOrigin          = ExternKindOrigin::Pending;
    required.requiredByDirective = true;
    unit.externImports.push_back(std::move(required));
}

void requireEntryReference(AssembledModule& unit) {
    requireEntryReference(std::span<AssembledModule>{&unit, 1});
}

}  // namespace dss::linker
