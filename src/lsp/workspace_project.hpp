#pragma once

#include "core/export.hpp"
#include "lsp/schema_cache.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ── THE EDITOR'S TARGET CHANNEL (D-LSP-ASSEMBLY-DIALECT-UNSERVABLE) ──────────
//
// `SchemaCache::resolveByExtension` REFUSES an extension more than one shipped
// language claims — `.s`/`.S` are claimed by both `asm-x86_64-att` and
// `asm-arm64-gas`, and answering with either would let registration order
// decide what an assembly file MEANS. Refusing is honest, but it leaves an
// editor opening a `.s` with no schema and no language service at all.
//
// The tie-breaker is the compile TARGET, and the target is a workspace fact,
// not a file fact. This header is the channel that carries it:
//
//     workspace root(s)  →  `*.dss-project.json`  →  `targets[]`
//                        →  `<target>.target.json`
//                        →  `defaultAssemblyLanguage`
//                        →  a PREFERRED LANGUAGE NAME
//
// Every arrow is an EXISTING mechanism. `dss::loadProjectConfig`
// (`core/types/project_config.hpp`) is the same parser `Program::compileProject`
// uses — the manifest's schema, its closed key vocabulary and its diagnostics
// are all inherited rather than restated. `dss::TargetSpec::parse` is the same
// `<targetName>:<formatName>` splitter the driver uses. `TargetSchema::
// loadShipped` + `defaultAssemblyLanguage()` is the same lookup
// `Program::resolveGrammarForTarget` performs when a `.s` is compiled with no
// explicit `--language`. Nothing here re-implements project discovery; the ONE
// thing this file adds is "which directory to look in", because the compiler
// never needed that question answered (`--project <file>` is an explicit path)
// and an editor has nothing else to go on.
//
// ⚠ CONSEQUENCE, STATED NOT BURIED: a workspace with no project file on disk
// gets NO tie-breaker, and therefore still no language service on a `.s`. That
// is the accepted cost of resolving this from the manifest. It surfaces as a
// DIAGNOSTIC naming the directory that was searched and the suffix that was
// looked for — never as a silently schema-less document.

namespace dss::lsp {

// Why a workspace could not name a preferred source language. Each value is a
// DIFFERENT operator action, which is the whole reason the set is not one
// "workspace lookup failed" code:
//   * NoWorkspaceRoot        — open a folder (or start the server with one).
//   * ProjectFileNotFound    — add a `*.dss-project.json` to the root.
//   * ProjectFileLoadFailed  — fix the manifest; the shared parser's own
//                              diagnostic is carried through verbatim.
//   * TargetSpecMalformed    — fix a `targets[]` entry's `<target>:<format>`.
//   * TargetConfigLoadFailed — the named target has no loadable
//                              `<name>.target.json`.
//   * TargetDeclaresNoAssemblyLanguage — add `defaultAssemblyLanguage` to that
//                              target document.
//
// ★ THERE IS DELIBERATELY NO `ProjectDeclaresNoTarget`. A manifest that names
// no target is ALREADY refused by the shared parser (`targets` is a required
// non-empty array of non-empty strings → `C_MissingField`), so a second check
// here would be an unreachable branch — a guard that can never fire is a guard
// that asserts nothing. The mode is still distinct to the operator, because
// `ProjectFileLoadFailed` carries the parser's message, which NAMES the
// `targets` field. Pinned by
// `WorkspaceProject.ManifestNamingNoTargetIsRejectedByTheSharedParser`.
enum class WorkspaceProjectErrorKind : std::uint8_t {
    NoWorkspaceRoot,
    ProjectFileNotFound,
    ProjectFileLoadFailed,
    TargetSpecMalformed,
    TargetConfigLoadFailed,
    TargetDeclaresNoAssemblyLanguage,
};

[[nodiscard]] DSS_EXPORT std::string_view
    workspaceProjectErrorName(WorkspaceProjectErrorKind kind) noexcept;

struct DSS_EXPORT WorkspaceProjectError {
    WorkspaceProjectErrorKind kind;
    std::string               detail;

    // Value equality over the WHOLE struct, `detail` included. The liveness
    // refresh (`LspServer::refreshWorkspacePreference_`) republishes open
    // documents only when a freshly-resolved preference DIFFERS from the held
    // one; comparing `kind` alone would call "the manifest now fails for a
    // different reason" no change at all, and the editor would keep showing the
    // stale reason after the user had already half-fixed the file.
    [[nodiscard]] bool operator==(WorkspaceProjectError const&) const = default;
};

// What the workspace's declared targets prefer, and which manifests said so.
// `languages` is deduplicated, in first-seen order over
// (manifest order × that manifest's `targets[]` order).
//
// ★ A LIST, NOT A NAME — the same lesson `extClaimants_` learned. A workspace
// may declare `x86_64:…` AND `arm64:…`, and those prefer DIFFERENT assembly
// dialects. Collapsing that to one name would resurrect first-wins one layer
// up: the tie-break would silently answer with whichever target the manifest
// happened to list first. Two entries here means the workspace does NOT break
// the tie, and `SchemaCache::resolveByExtension` says so.
struct DSS_EXPORT WorkspaceLanguagePreference {
    std::vector<std::filesystem::path> projectFiles;
    std::vector<std::string>           languages;

    // Both vectors participate. `projectFiles` is deliberately part of the
    // identity even though only `languages` steers resolution: adding a second
    // manifest that happens to prefer the same dialect still changes what the
    // "no language service" message NAMES, and that message is republished from
    // this value.
    [[nodiscard]] bool operator==(WorkspaceLanguagePreference const&) const
        = default;
};

using WorkspacePreferenceResult =
    std::expected<WorkspaceLanguagePreference, WorkspaceProjectError>;

// Filename suffix that marks a project manifest. A SUFFIX, not a fixed name:
// the shipped convention is `<artifact>.<leg>.dss-project.json` (see the
// generated sqlite manifests), and a workspace legitimately holds several —
// one per leg. The bare name `.dss-project.json` matches too, trivially.
// EVERY match at the root is read; their preferences UNION. That is the only
// rule that cannot be gamed by file order.
inline constexpr std::string_view kProjectFileSuffix = ".dss-project.json";

// Read every `*.dss-project.json` directly inside each workspace root and
// return the union of its targets' `defaultAssemblyLanguage` names.
//
// FAIL-CLOSED on every partial answer:
//   * a manifest that will not parse            → ProjectFileLoadFailed
//   * a `targets[]` entry that will not split   → TargetSpecMalformed
//   * a target whose document will not load     → TargetConfigLoadFailed
//   * a target that declares no assembly lang   → TargetDeclaresNoAssemblyLanguage
//
// That last one is the arguable case, so the reasoning is written down: a
// preference is a property of the target SET, and a set one of whose members is
// silent cannot speak for the set. Skipping the silent target would NARROW the
// preference — potentially from two languages to one — and silently narrowing
// is exactly how a tie gets broken by accident. It is also the rule the
// compiler already applies: `Program::resolveGrammarForTarget` fails loud
// naming the target when `--language` is absent and the target declares no
// dialect. One rule, both halves of the program.
//
// Not cached here, and deliberately so: the server RE-CALLS it whenever the
// manifest set may have moved (`initialize`, every `didOpen`, every `didSave`,
// and the four `workspace/did*` notifications) and republishes the open
// documents whose answer changed — `LspServer::refreshWorkspacePreference_`,
// D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE. Caching here would put the
// staleness one layer lower instead of removing it. Cost per call: one
// `directory_iterator` per root, one JSON parse per manifest, one
// `TargetSchema::loadShipped` per declared target — which is why the refresh is
// hung off open/save and NOT off `didChange` (a per-keystroke message).
// Pure w.r.t. its arguments plus the filesystem.
[[nodiscard]] DSS_EXPORT WorkspacePreferenceResult
    resolveWorkspaceLanguagePreference(
        std::span<std::filesystem::path const> workspaceRoots);

// `file://` URI → filesystem path. Percent-decodes, drops the (empty or
// `localhost`) authority, and un-slashes a Windows drive path (`/C:/x` →
// `C:/x`). `std::nullopt` for anything that is not a `file:` URI — a remote
// or virtual document has no directory to search, and guessing one would be
// the silent-answer failure this whole file exists to prevent.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
    pathFromFileUri(std::string_view uri);

// The message an editor sees when a document opens with no schema. Joins the
// RESOLVER's reason (which claimants exist, what the preference matched) to the
// WORKSPACE's reason (why no preference was available) — neither half is
// actionable alone, and the resolver cannot know the second.
//
// Appends the workspace half ONLY for `AmbiguousExtension`: for
// `NoExtensionMatch` / `ShippedDir*` the workspace preference is irrelevant and
// naming it would send the operator to the wrong file.
[[nodiscard]] DSS_EXPORT std::string describeUnresolvedSchema(
    std::string_view                 fileExtension,
    SchemaResolveError const&        schemaError,
    WorkspacePreferenceResult const& preference);

} // namespace dss::lsp
