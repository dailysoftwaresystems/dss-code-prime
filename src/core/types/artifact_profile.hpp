#pragma once

#include <string>
#include <string_view>

// The registered artifact-profile vocabulary (plan 06 §3) — the single
// source of truth shared by EVERY loader that validates a profile name.
//
// Two consumers as of AP3 (2026-06-09), which is why this is hoisted out
// of any one loader (closes D-AP2-PROFILE-REGISTRY-SPLIT):
//   * the GRAMMAR loader (AP1) validates a language's `artifactProfiles[]`
//     — which profiles a language SUPPORTS;
//   * the OBJECT-FORMAT loader (AP3) validates a format's `artifactProfiles[]`
//     — which profiles a format SERVES (produces an artifact for).
// Both reject an unregistered entry at load time (`C_UnknownArtifactProfile`)
// so a config typo (`"clii"`) fails loud at its source rather than silently
// mis-declaring support/service downstream.
//
// The set is a registered vocabulary, NOT a compile-time enum the engine
// switches on (plan 06 §3 rev 2): new profiles arrive with the backend
// plan that introduces them. The engine only ever does membership lookups
// over this list + the per-language / per-format declared subsets — never
// an `if (profile == "gui")` identity branch (the standing agnosticism veto).

namespace dss {

inline constexpr std::string_view kRegisteredArtifactProfiles[] = {
    "cli", "gui", "lib", "staticlib", "script", "sproc",
    "transpile", "shader", "hdl",
};

// True iff `profile` is a registered profile name. A generic membership
// test — no per-profile-name branch.
[[nodiscard]] constexpr bool
isRegisteredArtifactProfile(std::string_view profile) noexcept {
    for (auto const& p : kRegisteredArtifactProfiles) {
        if (p == profile) return true;
    }
    return false;
}

// The registered set as a comma-joined string for diagnostics — the single
// source of truth for the human-readable list, derived from the array above
// so the two can never drift.
[[nodiscard]] inline std::string registeredArtifactProfileList() {
    std::string out;
    for (auto const& p : kRegisteredArtifactProfiles) {
        if (!out.empty()) out += ", ";
        out += p;
    }
    return out;
}

} // namespace dss
