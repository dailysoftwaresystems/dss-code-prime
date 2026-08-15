#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/project_config.hpp"
#include "core/types/resolve_library_spec.hpp"
#include "program/cli_args.hpp"     // CompileConfig
#include "program/git_acquire.hpp"  // IGitRunner

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

// dependency_resolver — `dependsOn` RESOLUTION (plan 06 §5.1, AP6).
//
// The loader parses `dependsOn` and stops (`project_config.hpp` is a pure
// parser); the driver refused any non-empty list (`D_PlanNotLanded`) until this
// unit landed. This is the engine behind that surface: it walks the dependency
// graph, acquires what has to be acquired, decides how each dependency COMPOSES
// into the consumer's build, and hands the driver back exactly two things — a
// source list to merge and a per-target library list to link.
//
// ── WHAT IT ANSWERS, AND WHY EACH ANSWER IS SHAPED THE WAY IT IS ─────────────
//
// **Composition is dispatched on the DEPENDENCY'S OWN composition VERB** (B.2),
// read out of the registered profile table (`core/types/artifact_profile.hpp`)
// — never on a profile NAME, and never on the CONSUMER's profile. `SourceMerge`
// contributes translation units to the consumer's own compilation;
// `ArtifactLink` builds to its own artifact which the consumer links against;
// `NotConsumable` is a loud reject. An UNREGISTERED name is not
// `NotConsumable`: it is a typo, and it gets `C_UnknownArtifactProfile` naming
// the registered set, because "this profile cannot be a dependency" is a
// confidently wrong answer about a profile that does not exist.
//
// **The object format a dependency is built with is DERIVED FROM THE CONSUMER'S
// TARGET, never read off the dependency's own `targets[]`** (B.10, and it is
// the decision that reverses the obvious design — read the allocation note at
// `D_DependencyTargetFormatUnresolvable`). `targets[]` states the platforms a
// project builds FOR ITSELF; reading it as a CAPABILITY CLAIM makes a perfectly
// portable dependency that merely has not listed arm64 reject an arm64
// consumer. The derivation is the unique format F with
// `F.kind() == consumerFormat.kind()`, `dep.artifactProfile ∈
// F.artifactProfiles()`, and `crossValidateTargetFormat(target, F)` passing —
// three DECLARED facts, no format-name parsing anywhere — and it fails CLOSED on
// zero (0xD022) or two-or-more (0xD023) candidates.
//
// **`.dss-deps` is ONE cache for the WHOLE graph, at the ROOT consumer's
// manifest directory** (M2 / B.4). Not the process cwd, and not one per node: a
// `path` dependency's tree may be read-only, and a cache under it would make a
// build of somebody else's checkout write into it. `DependencyCache` takes the
// directory as a constructor argument precisely so this cannot be re-derived
// somewhere else; this unit is the one caller that supplies it.
//
// **A dependency is built on a FRESH `Program`** (M3). ✔MEASURED,
// `Program::compileProject` mutates persistent driver state unconditionally at
// six sites and `setArtifactName` is a plain assignment, so a nested build on
// the SAME `Program` would stamp the DEPENDENCY's artifact name onto the ROOT's
// binary and leak its `-I` / `-D` into the root compile. The fresh object gets
// `compileConfig` / `jobs` / `executor` / `gitRunner` explicitly propagated,
// because a dependency silently built Debug under a Release root is the same
// class of silent difference.
//
// SOURCE-language / TARGET-CPU / LINKER-format AGNOSTIC. Every fork in this
// unit is over a declared verb or a closed enum — `DependencyComposition`,
// `ObjectFormatKind` equality, `artifactProfiles()` set membership, and the
// format's declared CONTAINER (`isStaticArchive()`). No language, CPU or object
// format is named anywhere, and no artifact-profile VALUE is compared against a
// literal.

namespace dss::substrate { class IExecutor; }

namespace dss {

// The manifest filename a `path` dependency must carry at its root (B.1). One
// spelling, quoted verbatim into `D_DependencyManifestNotFound`, so the name
// the message tells the user to create is the name the resolver looked for.
inline constexpr std::string_view kDependencyManifestName = ".dss-project.json";

// The `deps/` component of U-9's artifact layout —
// `<consumer output base>/deps/<derived-dep-name>/<formatName>/<file>`. Spelled
// once here because a test that pins the layout must not re-type it.
inline constexpr std::string_view kDependencyOutputDirName = "deps";

// ── THE RECURSION DEPTH CAP (plan v2 §3 item 9) ─────────────────────────────
//
// The DFS stack catches CYCLES; nothing catches a graph that is legal, acyclic
// and simply very deep. That class is CI-INVISIBLE by this project's own
// standing note — a deep-recursion stack overflow passes the Release/MinGW legs
// and surfaces on MSVC-Debug — and its failure mode is a process death with no
// diagnostic at all, which is the opposite of every rule in this file.
//
// The number is a BUDGET, not a measurement of the C++ stack: each level holds
// a manifest, its expanded source list and a scratch reporter, and no real
// project nests prerequisites 64 deep. A graph that does is either generated or
// wrong, and either way the operator needs told rather than crashed at.
inline constexpr std::size_t kMaxDependencyDepth = 64;

// What a resolved graph contributes to the consumer's build. Exactly two
// fields, because those are exactly the two ways a dependency can compose.
struct DSS_EXPORT DependencyResolution {
    // `SourceMerge` contributions, already expanded and de-duplicated against
    // each contributing manifest's OWN directory (B.3), in DFS order.
    //
    // ★ THE CONSUMER'S OWN SOURCES ARE NOT IN HERE, AND THAT IS M4(b). The
    // artifact is named from `sourceFiles.front()`'s stem when the manifest
    // states no `artifactName`, and archive member names follow it — so the
    // caller concatenates ITS OWN sources FIRST and appends this, or adding a
    // `module` dependency silently RENAMES the output binary.
    std::vector<std::string> mergedSources;

    // `ArtifactLink` contributions, keyed by the CONSUMER's own
    // `<targetName>:<formatName>` spec string — the key
    // `Program::setResolveLibraryAdditionsByTarget` takes. A dependency is
    // built ONCE PER CONSUMER TARGET, so the artifact for one target is a
    // DIFFERENT FILE from the artifact for another; a program-wide list would
    // hand every target every other target's binaries.
    std::map<std::string, std::vector<ResolveLibrarySpec>> libraryAdditionsByTarget;
};

// Everything the walk needs that is not in the manifest itself.
struct DSS_EXPORT DependencyResolveRequest {
    // The ROOT consumer's manifest path. Its canonical DIRECTORY is both the
    // `.dss-deps` location (M2) and the first cycle-detection key; the
    // directory is derived from this ONE input rather than passed beside it, so
    // the two cannot disagree. `ProjectConfig` carries no path of its own,
    // which is why this has to be threaded at all.
    std::filesystem::path rootManifestPath;

    // The CONSUMER's target specs, verbatim, in manifest order. These — and
    // never a dependency's own `targets[]` — are the platforms every
    // `ArtifactLink` dependency is built for (B.10).
    std::vector<std::string> targets;

    // U-9's base: a dependency artifact lands at
    // `<artifactOutputBase>/deps/<name>/<formatName>/<file>`, never inside the
    // dependency's own tree, which may be read-only. The caller passes
    // `--output` when given and `<cwd>/target` otherwise — the same rule
    // `resolveArtifactOutputDir` applies to the consumer's own artifact.
    std::filesystem::path artifactOutputBase;

    // M3: propagated verbatim onto every dependency's fresh `Program`.
    CompileConfig         compileConfig = CompileConfig::Debug;
    unsigned              jobs          = 0;
    substrate::IExecutor* executor      = nullptr;

    // `--force-git-cache`: bypass the cache-hit short-circuit and re-fetch.
    // U-10 — a silent no-op when the graph declares no git dependency, which
    // falls out of the cache being opened LAZILY (see the .cpp).
    bool forceGitCache = false;
};

// Resolve `rootConfig`'s `dependsOn` graph.
//
// Returns `std::nullopt` after emitting at least one diagnostic on `rep`; the
// caller must ABANDON the build. Draining is the CALLER's, matching every other
// fail-loud seam in `Program::compileProject`.
//
// An EMPTY `dependsOn` returns an empty resolution having touched nothing — no
// `.dss-deps`, no git probe, no filesystem write — so the overwhelmingly common
// manifest pays nothing for this feature existing.
//
// `git` is BORROWED and must outlive the call (the `substrate::IExecutor`
// non-owning-injection shape). It is an interface rather than a concrete
// `SystemGitRunner` for the reason `git_acquire.hpp` records at length: two of
// B.4's four cache outcomes are network FAILURES, which no test could reach
// deterministically through real git.
[[nodiscard]] DSS_EXPORT std::optional<DependencyResolution>
resolveProjectDependencies(ProjectConfig const&            rootConfig,
                           DependencyResolveRequest const& request,
                           IGitRunner&                     git,
                           DiagnosticReporter&             rep);

} // namespace dss
