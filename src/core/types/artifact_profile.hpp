#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The registered artifact-profile vocabulary (plan 06 §3) — the single
// source of truth shared by EVERY loader that validates a profile name, and
// (since `dependsOn`) the TABLE that declares how an artifact built under
// each profile COMPOSES into a consumer's build.
//
// Two loader consumers as of AP3 (2026-06-09), which is why this is hoisted
// out of any one loader (closes D-AP2-PROFILE-REGISTRY-SPLIT):
//   * the GRAMMAR loader (AP1) validates a language's `artifactProfiles[]`
//     — which profiles a language SUPPORTS;
//   * the OBJECT-FORMAT loader (AP3) validates a format's `artifactProfiles[]`
//     — which profiles a format SERVES (produces an artifact for).
// Both reject an unregistered entry at load time (`C_UnknownArtifactProfile`)
// so a config typo (`"clii"`) fails loud at its source rather than silently
// mis-declaring support/service downstream.
//
// ── the standing agnosticism veto, restated for the table form ─────────────
//
// The set is still a registered VOCABULARY, never a compile-time enum the
// engine switches on (plan 06 §3 rev 2): new profiles arrive with the backend
// plan that introduces them, as a ROW here — never as a branch somewhere
// else. Membership lookups over this table plus the per-language /
// per-format declared subsets remain the ONLY thing a loader does with it.
//
// What the table adds is the one thing a genuinely per-profile decision
// needs, placed where it cannot drift from the name: a declared composition
// VERB carried BY the row. The `dependsOn` driver must decide, per
// dependency, how that dependency folds into the consumer's build, and the
// naive spelling of that decision is `if (profile == "module") … else if
// (profile == "staticlib" || profile == "lib") … else fail` — the exact
// identity branch this header has always vetoed, and one that goes silently
// wrong for the TENTH profile (it lands in the `else`, and whether that is
// correct is an accident of which arm the author wrote last).
//
// So the invariant is sharper than "no branch at all", and it is the same one
// `program.cpp`'s static-archive fork already obeys — that fork dispatches on
// the FORMAT's declared `container`, "NEVER the artifactProfile
// (`artifact_profile.hpp`'s standing veto)" (`src/program/program.cpp`,
// D-FF1-AR-STATICLIB-DRIVER-WIRING):
//
//   ★ THE ENGINE SWITCHES ON THE COMPOSITION VERB, NEVER THE PROFILE NAME.
//     Adding a profile adds a ROW, never a branch. MEASURED at the time of
//     writing: ZERO profile-value string comparisons exist in `src/`, and the
//     way to keep that number at zero is to make the row carry the answer
//     rather than make each caller re-derive it.
//
// Concretely: the composition fork asks `dependencyCompositionForProfile()`
// and switches over the three enumerators. An eleventh profile that composes
// like `staticlib` is ONE row spelling `ArtifactLink`; the fork grows no arm,
// no call site is revisited, and no site can forget the new name. A profile
// whose composition is genuinely novel is the only thing that may add an
// enumerator — and then the compiler's exhaustiveness warning walks the
// author to every switch that must answer for it, which a string compare
// never would.

namespace dss {

// How an artifact built under a given profile composes into a CONSUMER's
// build when it is named as a dependency (`dependsOn` in `.dss-project.json`).
// This is the only per-profile axis the engine is permitted to switch on.
enum class DependencyComposition : std::uint8_t {
    // The dependency contributes SOURCES, not an artifact: its translation
    // units join the consumer's own set and are compiled/lowered with them.
    // There is no separate build product to link against — the unit exists
    // to be absorbed.
    SourceMerge,
    // The dependency is built to its OWN artifact and the consumer LINKS
    // against that artifact (an import/shared library for `lib`, an `ar`
    // archive for `staticlib`). The two builds stay separate.
    ArtifactLink,
    // The profile produces a TERMINAL deliverable — something a person or a
    // platform runs, not something another build consumes. Naming it in
    // `dependsOn` is a project-config error the driver must reject loudly:
    // absorbing a `cli` dependency as sources would splice its `main()` into
    // the consumer, and "linking" one is not a thing the profile can offer.
    NotConsumable,
};

// One registered profile: its name and the composition verb it declares.
// The two live in the same row precisely so a reader (and a reviewer) can
// never see one without the other.
struct ArtifactProfileRow {
    std::string_view      name;
    DependencyComposition dependencyComposition;
};

// The registered set. ORDER IS LOAD-BEARING: `registeredArtifactProfileList()`
// derives the human-readable diagnostic list from it in place, so a new
// profile is APPENDED — reordering silently rewrites every "registered
// profiles: …" message a user has ever been shown.
inline constexpr ArtifactProfileRow kRegisteredArtifactProfiles[] = {
    {"cli",       DependencyComposition::NotConsumable},
    {"gui",       DependencyComposition::NotConsumable},
    {"lib",       DependencyComposition::ArtifactLink},
    {"staticlib", DependencyComposition::ArtifactLink},
    {"script",    DependencyComposition::NotConsumable},
    {"sproc",     DependencyComposition::NotConsumable},
    {"transpile", DependencyComposition::NotConsumable},
    {"shader",    DependencyComposition::NotConsumable},
    {"hdl",       DependencyComposition::NotConsumable},
    {"module",    DependencyComposition::SourceMerge},
};

// True iff `profile` is a registered profile name. A generic membership
// test over the table's NAME column — no per-profile-name branch.
[[nodiscard]] constexpr bool
isRegisteredArtifactProfile(std::string_view profile) noexcept {
    for (auto const& row : kRegisteredArtifactProfiles) {
        if (row.name == profile) return true;
    }
    return false;
}

// The composition verb `profile` declares, or `std::nullopt` when the name
// is NOT registered.
//
// The empty optional is the fail-loud half of the contract, and it is not
// interchangeable with a "safe" default. EVERY enumerator is a valid
// INSTRUCTION to the driver — `NotConsumable` included: it means "reject this
// dependency because the profile is terminal", which is a decision, not an
// absence. Returning `NotConsumable` for an unregistered name would make a
// typo (`"modul"`) indistinguishable from a deliberately terminal profile and
// would produce the wrong diagnostic — a "profile cannot be a dependency"
// complaint about a name that does not exist. Callers must therefore
// discriminate the empty case FIRST and report the unknown NAME, using the
// `C_UnknownArtifactProfile` + `registeredArtifactProfileList()` pair every
// other consumer of this header already uses.
[[nodiscard]] constexpr std::optional<DependencyComposition>
dependencyCompositionForProfile(std::string_view profile) noexcept {
    for (auto const& row : kRegisteredArtifactProfiles) {
        if (row.name == profile) return row.dependencyComposition;
    }
    return std::nullopt;
}

// The registered set as a comma-joined string for diagnostics — the single
// source of truth for the human-readable list, derived from the table above
// so the two can never drift.
[[nodiscard]] inline std::string registeredArtifactProfileList() {
    std::string out;
    for (auto const& row : kRegisteredArtifactProfiles) {
        if (!out.empty()) out += ", ";
        out += row.name;
    }
    return out;
}

} // namespace dss
