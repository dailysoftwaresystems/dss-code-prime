#pragma once

// ★ MOVED DOWN FROM `src/program/` ([[D-LSP-HEADER-CASE-RULE-NOT-WORKSPACE-AWARE]]).
// The LSP decides which of a workspace's manifests build an open document by
// asking THIS function which files each manifest names — the build's own answer,
// not a second reader's — and the driver tier is one the LSP may not include
// (`WorkspaceProject.TheLspNeverReachesUpIntoTheDriverTier`). The same move
// `project_config` and `target_spec` made before it, for the same reason.
//
// ── Project `sources[]` → the concrete file list the CU route counts ────────
//
// The manifest's `sources[]` is a list of ENTRIES, not files: an entry may be a
// literal path or a glob PATTERN, two entries may name the same file, and the
// count that decides multi-vs-single-CU routing (`routesToMultiUnit`) is the
// count AFTER both are resolved. This unit owns that resolution end to end —
// expansion, the zero-match / I-O fail-loud policy, and the cross-entry dedup —
// so there is exactly ONE answer to "which files does this manifest name".
//
// ★ WHY IT IS ITS OWN UNIT (AP6 M4). It was a 54-line block inside
// `Program::compileProject`, which could only ever resolve ONE manifest: the
// root's, against the process cwd. AP6 must resolve a DEPENDENCY's manifest
// too, and a dependency declares its sources relative to ITS OWN directory. A
// second copy of the block with a base threaded through it would be a second,
// driftable answer to the same question — and the two answers would disagree
// about exactly the thing that is hard here (see the literal note below). One
// function, one `baseDir` parameter, one policy.
//
// SOURCE-language / TARGET-CPU / LINKER-format AGNOSTIC: path text and
// filesystem calls only. Nothing here inspects an extension, a language, a
// target or an object format.

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dss {

// ════ THE ONE BASE RULE FOR EVERY PATH A MANIFEST HOLDS ══════════════════════
//
// [[D-PROJECT-ROOT-MANIFEST-PATHS-RESOLVE-AGAINST-THE-INVOCATION-DIRECTORY]].
// Every relative path a `.dss-project.json` holds — `sources` (literal and glob),
// `includes`, `resolveLibraries`, `output`, the directory its build hooks run
// in, a hook program named by a path, `dependsOn` — names a location relative to
// THAT MANIFEST'S OWN DIRECTORY. The root manifest and a dependency manifest
// alike; the build, the dependency builds, the hook runner and the editor all
// resolve through the two functions below, so they agree by construction.
// Command-line flags (`--output`, `-I`, `--resolve-library`, `--project` itself)
// are NOT manifest paths and stay relative to where the command runs, as for
// every compiler.
//
// ✔WHY, measured before it was decided (lane `cr`, P68 round 8 part 6): the
// root used to resolve its relative paths against the PROCESS working directory,
// and from a directory holding look-alikes a root build compiled the WRONG
// `src/main.c`, took the wrong header and ran the wrong hook script — rc 0 every
// time; the root manifest already resolved `dependsOn` against its own directory
// while resolving its `sources` against the cwd; and CMake, MSBuild, Cargo and
// Meson all resolve a manifest's paths against the manifest's directory. The
// argument the spec gave for dependencies — a manifest's meaning must not depend
// on where it was invoked from — applies to the root just as much.
//
// LEXICAL, on purpose: nothing here touches the filesystem, so a path a pre-build
// hook has not generated yet resolves exactly like one that exists, and `..`
// segments are left for the OS to walk.

// The directory a manifest's relative paths are relative to: its own directory,
// as SPELLED by the path the manifest was named by. A manifest named with no
// directory component (`--project app.dss-project.json`) answers EMPTY, which
// means the process working directory — the directory that manifest is in.
[[nodiscard]] DSS_EXPORT std::filesystem::path
manifestDirectoryOf(std::filesystem::path const& manifestPath);

// THE RULE: `entry` resolved against `manifestDirectory`. A ROOTED entry
// (`isRootedPath`, which keeps a `//host/share` authority rooted where
// `is_absolute()` does not) or an EMPTY directory leaves `entry` as it is;
// otherwise the result is `manifestDirectory / entry`.
[[nodiscard]] DSS_EXPORT std::filesystem::path
resolveManifestPath(std::filesystem::path const& manifestDirectory,
                    std::filesystem::path const& entry);

// The same rule for an entry the manifest SPELLED as text (`sources`,
// `includes`, `output`, a hook's `run[0]`): an entry the rule leaves alone comes
// back exactly as written, character for character; a re-based one comes back
// in the lossless generic spelling (`core::genericSpelling`).
[[nodiscard]] DSS_EXPORT std::string
resolveManifestPathSpelling(std::filesystem::path const& manifestDirectory,
                            std::string const&           entry);

// What a manifest applies to the sources IT NAMES when they are compiled inside
// ANOTHER manifest's build — a `module` dependency's sources merged into its
// consumer. [[D-DEPS-MODULE-INCLUDES-AND-DEFINES-SILENTLY-DROPPED]]: the module's
// `includes` (each resolved against the module's directory) are searched BEFORE
// the consumer's, and its `defines` follow the consumer's, so the module's own
// value wins a conflict (with the preprocessor's redefinition warning). They
// reach the MODULE's sources only, never the consumer's own. The consumer's
// environment still reaches the module's sources, as it always has —
// [[D-DEPS-SOURCEMERGE-INHERITS-THE-CONSUMERS-COMPILATION-ENVIRONMENT]], gated on
// AP7.
struct DSS_EXPORT ManifestSourceSettings {
    std::vector<std::string> includeDirs;
    std::vector<std::string> defines;
    friend bool operator==(ManifestSourceSettings const&,
                           ManifestSourceSettings const&) = default;
};

// Resolve a manifest's `sources[]` entries into the concrete, de-duplicated file
// list a build compiles, IN MANIFEST ORDER.
//
// `baseDir` — THE MANIFEST'S DIRECTORY (`manifestDirectoryOf`), for the root and
// a dependency alike. Every RELATIVE entry — LITERAL ENTRIES INCLUDED — resolves
// against it through `resolveManifestPath`, and a glob is walked from it. EMPTY
// (a manifest named with no directory component) ⇒ the process working
// directory, which is that manifest's directory; a literal is then kept VERBATIM,
// character for character. An ABSOLUTE entry is never re-based.
//
// ★ THE LITERAL HALF IS THE POINT, NOT AN AFTERTHOUGHT (AP6 M4a). Re-basing only
// the GLOB expansion looks complete and is not: ✔MEASURED, a `sources[]` entry
// with no glob metacharacter was kept verbatim and later opened by
// `UnitBuilder::addFile` — i.e. against the PROCESS cwd. `"sources":
// ["src/lib.c"]` is the overwhelmingly common form, so the half left un-based
// would have been the half everybody writes. Worse, it fails by READING THE
// WRONG FILE (the consumer's own `src/lib.c`, if it has one) rather than by
// failing loud — a silent miscompile, not a missing input. Both halves re-base
// here, together, or neither does.
//
// ORDER IS LOAD-BEARING and is preserved exactly: entries keep their manifest
// positions, each glob's own matches are sorted lexicographically, and the FIRST
// occurrence of a duplicate wins (keeping its own spelling). The artifact is
// named from the stem of `[0]` when the manifest states no `artifactName`
// (`program.cpp`'s `sourceStem`), and archive member names follow it too — so
// re-ordering this list silently RENAMES the emitted binary.
//
// DEDUP KEY: `weakly_canonical` (AP6 §3.4). The predecessor normalized with
// `lexically_normal` and conceded absolute-vs-relative spellings of one file as
// an "accepted un-caught extreme edge". Once a merged build draws sources from
// two manifests — one contributing ABSOLUTE paths, the other RELATIVE ones —
// that edge is the NORMAL case, and its consequence is a duplicate CU and a
// duplicate-symbol LINK error the diagnostic cannot tie back to any manifest.
// `weakly_canonical` answers for a path that does not exist yet (a pre-build hook
// may not have generated it), which is why it and not `canonical`.
//
// FAIL LOUD, returning `std::nullopt` with exactly one diagnostic already
// reported on `rep` (the CALLER owns draining):
//   * a glob matching ZERO files          → `D_FileNotFound`, naming the pattern
//                                            AND the base it was resolved against;
//   * a filesystem I/O error, in the glob walk OR while canonicalizing an entry
//                                          → `D_DirectoryScanFailed`.
// A literal that does not exist is NOT rejected here — unchanged: it fails
// downstream at CU build, where the diagnostic can point at the file.
[[nodiscard]] DSS_EXPORT std::optional<std::vector<std::string>>
expandAndDedupProjectSources(std::vector<std::string> const& sources,
                             std::filesystem::path const&    baseDir,
                             DiagnosticReporter&             rep);

} // namespace dss
