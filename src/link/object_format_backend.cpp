// The generic half of the object-format backend seam: resolution.
// D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125).
//
// This TU is SHARED SUBSTRATE and it enumerates nothing. It walks the span it
// is handed by `objectFormatBackendTable()` (defined one tier down, in
// `src/link/format/object_format_backends.cpp`) and asks each entry about
// itself. It names no format, includes no format header, and would compile
// unchanged against a table of twenty.

#include "link/object_format_backend.hpp"

#include <span>
#include <string_view>

namespace dss::link {

ObjectFormatBackend const*
objectFormatBackendByConfigName(std::string_view configName) noexcept {
    // ★★★ FAILS CLOSED. There is no default arm, no "first entry", no
    // fallback. An unrecognized spelling — including the reserved `unknown`
    // sentinel, which no backend claims — yields nullptr, and every caller in
    // the schema tier is written to REFUSE on nullptr rather than to skip a
    // rule.
    //
    // That direction is the one thing this whole design cannot get wrong. If a
    // null backend meant "no identity rules apply", every one of the 24 shipped
    // formats would validate CLEAN while validating NOTHING — and the negative
    // tests would not notice, because their fixtures reject for other reasons
    // too and only assert that SOMETHING rejected. The whole-family mutation
    // probe (`tests/link/test_object_format_backend_registry.cpp`) exists to
    // keep that state unreachable: it corrupts each required identity field of
    // each shipped format in turn and demands a reject at that exact JSON
    // pointer, so "the rules silently stopped running" fails 100+ assertions
    // instead of passing quietly.
    if (configName.empty()) return nullptr;
    for (auto const* backend : objectFormatBackendTable()) {
        if (backend != nullptr && backend->configName() == configName) {
            return backend;
        }
    }
    return nullptr;
}

} // namespace dss::link
