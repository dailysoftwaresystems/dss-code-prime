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

    return true;
}

} // namespace dss
