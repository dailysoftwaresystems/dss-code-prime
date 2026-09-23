#include "link/image_request.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <format>
#include <string>

namespace dss {

namespace {

// The remedy tail of a refusal. DECLARED, not inferred: formats that all
// merely "lack the capability" need sharply different advice — a PE dll HAS
// the header field and the loader ignores it, a relocatable object has no such
// field at all, an ELF program takes its stack from the process limit — and
// none of that is derivable without asking WHICH format this is. So the schema
// declares a closed reason verb and this looks the prose up in the vocabulary
// table (core/types/object_format_kind.hpp).
//
// A format that declared no verb still gets REFUSED, just with a generic tail:
// fail-closed degrades to less-specific, NEVER to silent.
[[nodiscard]] std::string remedyFor(ObjectFormatSchema const& format) {
    auto const reason = format.stackReserveUnsupportedReason();
    if (!reason.has_value()) {
        return "This format declares no 'stackReserveUnsupportedReason' verb "
               "either, so no specific remedy can be offered -- build for a "
               "format that declares 'stackReserveControl', or drop the "
               "request for this target.";
    }
    return std::format("{} (declared reason: '{}')",
                       stackReserveUnsupportedRemedy(*reason),
                       stackReserveUnsupportedReasonName(*reason));
}

} // namespace

bool enforceImageRequest(ImageRequest const&       request,
                         ObjectFormatSchema const& format,
                         std::string_view          contextLabel,
                         DiagnosticReporter&       reporter) {
    if (request.empty()) return true;   // nothing asked ⇒ nothing to enforce

    // ── stack reserve (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH) ──────────
    if (request.stackReserveBytes.has_value()) {
        std::uint64_t const want = *request.stackReserveBytes;
        auto const cap = format.stackReserveControl();
        if (!cap.has_value()) {
            report(reporter,
                   DiagnosticCode::K_FormatLacksStackReserveControl,
                   DiagnosticSeverity::Error,
                   std::format(
                       "{}: a stack reserve of {} bytes was requested "
                       "(project 'stackReserve' / CLI --stack-reserve) but "
                       "object format '{}' declares no 'stackReserveControl' "
                       "capability, so nothing in the emitted artifact can "
                       "carry it. The request is REFUSED rather than dropped: "
                       "a build that reported success while writing a field "
                       "nothing reads would hand you a program with the "
                       "DEFAULT stack and no way to tell. {} "
                       "D-SQLITE-PE64-FULL-TIER-STACK-DEPTH.",
                       contextLabel, want, format.name(), remedyFor(format)));
            return false;
        }
        // Range + alignment, checked against the numbers the FORMAT declared
        // (never a constant baked in here). REJECTED, never clamped or
        // rounded: silently emitting a different number than the one the user
        // asked for is the knob-that-lies class this capability exists to
        // close, and at runtime it is indistinguishable from the original bug.
        char const* violation = nullptr;
        if (want < cap->minimumBytes)      violation = "below the minimum";
        else if (want > cap->maximumBytes) violation = "above the maximum";
        else if ((want % cap->granularityBytes) != 0)
                                          violation = "misaligned";
        if (violation != nullptr) {
            report(reporter, DiagnosticCode::K_InvalidStackReserveRequest,
                   DiagnosticSeverity::Error,
                   std::format(
                       "{}: requested stack reserve of {} bytes is {} for "
                       "object format '{}' (vehicle '{}'), which declares "
                       "minimumBytes={}, maximumBytes={}, granularityBytes={}."
                       " The request is REFUSED, not rounded -- an image whose "
                       "stack differs from the number the build asked for is "
                       "undiagnosable at runtime. Pick a value in range and a "
                       "multiple of the granularity. "
                       "D-SQLITE-PE64-FULL-TIER-STACK-DEPTH.",
                       contextLabel, want, violation, format.name(),
                       stackReserveVehicleName(cap->vehicle),
                       cap->minimumBytes, cap->maximumBytes,
                       cap->granularityBytes));
            return false;
        }
    }

    // ── runpaths (D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH) ───────────────
    //
    // Only an entry NO carrier could hold is refused here — the rule is
    // `runpathEntryRefusal`'s, stated once in link/runpath.hpp. Whether THIS
    // format carries runpaths at all is not a refusal (see
    // `reportUnrecordedRunpaths` below), so this stays a pure predicate on
    // success and safe to call twice on one path.
    for (std::size_t i = 0; i < request.runpaths.size(); ++i) {
        if (auto const why = runpathEntryRefusal(request.runpaths[i])) {
            report(reporter, DiagnosticCode::K_InvalidRunpathRequest,
                   DiagnosticSeverity::Error,
                   std::format(
                       "{}: runpath entry #{} (CLI --rpath / project "
                       "'runpaths') is REFUSED for object format '{}': {}. "
                       "D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH.",
                       contextLabel, i + 1, format.name(), *why));
            return false;
        }
    }

    return true;
}

bool reportUnrecordedRunpaths(ImageRequest const&       request,
                              ObjectFormatSchema const& format,
                              std::string_view          contextLabel,
                              DiagnosticReporter&       reporter) {
    if (request.runpaths.empty() || format.runpath().has_value()) return false;
    // WHERE the loader looks instead — never inferred from a format identity:
    // an object or archive is answered by a CAPABILITY question (nothing loads
    // it), an image format by the reason verb its own document declares.
    std::string why;
    if (!format.isImageFlavor()) {
        why = "An object file or an archive is not loaded by any loader, so "
              "nothing would ever read a runpath recorded in it: the runpath "
              "belongs to the final link that makes the executable or shared "
              "library";
    } else if (auto const reason = format.runpathUnsupportedReason()) {
        why = std::format("{} (declared reason: '{}')",
                          runpathUnsupportedExplanation(*reason),
                          runpathUnsupportedReasonName(*reason));
    } else {
        why = std::string{runpathUnsupportedExplanation(
            RunpathUnsupportedReason::Unspecified)};
    }
    report(reporter, DiagnosticCode::K_FormatLacksRunpath,
           DiagnosticSeverity::Warning,
           std::format(
               "{}: {} runpath entr{} requested (CLI --rpath / project "
               "'runpaths'), but object format '{}' declares no 'runpath' "
               "carrier, so NOTHING is recorded and the artifact is written "
               "without it — the request is accepted, as the references accept "
               "it for a format that has nowhere to put it. {}. "
               "D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH.",
               contextLabel, request.runpaths.size(),
               request.runpaths.size() == 1 ? "y was" : "ies were",
               format.name(), why));
    return true;
}

std::optional<std::vector<std::string>>
runpathsToRecord(ImageRequest const&       request,
                 ObjectFormatSchema const& format,
                 RunpathCarrier            walkerCarrier,
                 std::string_view          contextLabel,
                 DiagnosticReporter&       reporter) {
    auto const& decl = format.runpath();
    if (request.runpaths.empty() || !decl.has_value()) {
        return std::vector<std::string>{};
    }
    std::string problem;
    for (auto const& p : runpathDeclarationProblems(*decl)) {
        problem += std::format(" '/runpath/{}': {}.", p.key, p.message);
    }
    if (problem.empty() && decl->carrier != walkerCarrier) {
        problem = std::format(
            " it names carrier '{}', but this walker records '{}' — it would "
            "accept the request and write something else, or nothing.",
            runpathCarrierName(decl->carrier),
            runpathCarrierName(walkerCarrier));
    }
    if (!problem.empty()) {
        report(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
               DiagnosticSeverity::Error,
               std::format(
                   "{}: object format '{}' declares a runpath this walker "
                   "cannot record:{} The document loader refuses this at load; "
                   "reaching it means the schema was built without it. "
                   "D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH.",
                   contextLabel, format.name(), problem));
        return std::nullopt;
    }
    return recordedRunpaths(request.runpaths, *decl);
}

} // namespace dss
