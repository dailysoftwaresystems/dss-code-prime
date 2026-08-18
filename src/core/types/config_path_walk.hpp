#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"   // LoadResult + ConfigDiagnostic
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

// Shared loader substrate for the `loadShipped(name)` lookup pattern.
// Both `GrammarSchema` (`src/dss-config/sources/<name>.lang.json`) and
// `TargetSchema` (`src/dss-config/targets/<name>.target.json`) need the
// same discovery: reject path-like names, then walk ONE precedence.
// This helper consolidates the shape so a future third config kind (e.g.,
// a passes manifest) drops into the same substrate. `findShippedConfigDir`
// below is the DIRECTORY form of the same precedence, for the callers that
// need the containing directory rather than one named file.
//
// ── THE PRECEDENCE, ONCE ──────────────────────────────────────────────────
//   1. `$DSS_CONFIG_ROOT/src/dss-config/…`  — the explicit override
//   2. `<this executable's dir>/…/dss-config/…` — the INSTALLED layout
//   3. the cwd ancestor walk (<= 8 hops), `<ancestor>/src/dss-config/…`
//
// `DSS_CONFIG_ROOT` (optional): a directory that CONTAINS `src/dss-config/`
// (absolute recommended; a relative value resolves against cwd). It is FIRST
// because an explicit override outranks discovery by definition — the test
// harness sets it to the repo root (`dss_add_test`, `integrated_tests`) so
// OUT-OF-TREE builds, whose ctest cwd has no `src/dss-config/` in its
// ancestry, find shipped config at all. A set-but-miss falls through.
//
// ★ ARM 2 EXISTS BECAUSE A PACKAGED COMPILER HAD NO WAY TO FIND ITS OWN
// CONFIG ([[D-PKG-NO-PACKAGING-PATH-SHIPS-THE-CONFIG-TREE]]). With only an
// override and a cwd walk, an installed `dsscp` at `/usr/bin` invoked from a
// user's project walked THAT project's ancestors and found nothing — so
// copying the config tree into a package would still not have worked, and
// `#include <stdio.h>` was unresolvable. The layout it probes is COMPUTED BY
// CMAKE from the same `GNUInstallDirs` variables the `install()` rules use
// (`cmake/DssInstall.cmake` -> `DSS_INSTALL_CONFIG_RELDIR`), so the install
// set and the lookup cannot disagree about where the tree lives.
//
// ★★ ARM 2 OUTRANKS ARM 3, AND THAT IS THE SAFETY PROPERTY. An installed
// compiler must use the config it was INSTALLED WITH, never whichever
// `src/dss-config/` happens to sit in the cwd's ancestry — that would let an
// unrelated directory silently redefine the compiler. The arm is INERT for a
// development build (a binary in `build/bin/dss/` has no installed layout
// around it), so nothing about the repo workflow changes. And once the
// installed root EXISTS it is AUTHORITATIVE: a miss inside it stops the
// search rather than falling through, because mixing two config trees in one
// compilation is a silent-wrongness of its own.
//
// ★★★ BINARY / CONFIG VERSION SKEW FAILS LOUD. A compiler paired with a
// config tree from another version does not fail — it compiles something
// subtly different, the stale-cached-object class. The installed arm composes
// the binary's OWN version into the path, so agreement is BY CONSTRUCTION and
// a mismatch is an ordinary loud not-found; the two repo-shaped arms read the
// tree's own `VERSION` (the file the build already reads) and REFUSE on a
// disagreement, naming both versions and the tree.

namespace dss {

struct DSS_EXPORT ShippedConfigLocator {
    std::string_view name;             // user-supplied (e.g. "x86_64")
    std::string_view subdir;           // "sources" / "targets"
    std::string_view suffix;           // ".lang.json" / ".target.json"
    std::string_view kindLabel;        // "language" / "target" — diag prose
    DiagnosticCode   invalidNameCode;  // C_Invalid{Language,Target,Format}Name per kind
};

// The installed config root's path RELATIVE to the directory holding the
// installed executable (e.g. `../share/dss-code-prime/0.0.2/dss-config`).
//
// COMPUTED BY `cmake/DssInstall.cmake` from the same `GNUInstallDirs` variables
// its `install()` rules use, and forwarded here as a compile definition — so the
// layout has exactly ONE owner and the installed tree cannot end up somewhere
// the lookup does not probe. Exposed rather than kept private because it is the
// public shape of an installed DSS, and because a test that plants a synthetic
// installed layout must compose the SAME path the resolver will read.
[[nodiscard]] DSS_EXPORT std::string_view installedConfigRelDir();

// The directory holding the RUNNING executable, symlinks resolved, or nullopt
// when this host does not report it. Answered from the KERNEL's record of the
// loaded image (`GetModuleFileNameW` / `_NSGetExecutablePath` / `/proc/self/exe`)
// and NEVER from `argv[0]`, which is whatever the parent chose to pass and is
// routinely a bare command name. Cached: the answer cannot change in a process.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
runningExecutableDir();

// The INSTALLED config root implied by an executable directory — i.e.
// `<executableDir>/<DSS_INSTALL_CONFIG_RELDIR>`, normalised — or nullopt when
// that is not a directory (which is the normal answer for a development build,
// where nothing is installed around the binary).
//
// PURE, and separated from `runningExecutableDir()` for exactly that reason: it
// is the half a test can drive with a planted scratch layout. Planting a real
// installed tree beside the running test binaries would be visible to every
// other test executable in the same directory — under `ctest -j` that would
// silently outrank their cwd walk.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
installedConfigRootFrom(std::filesystem::path const& executableDir);

// Locate a shipped config file by the precedence documented at the top of this
// header. Returns the resolved path on success, or a single-entry diagnostic
// vector on rejection (bad name), on version SKEW (naming both versions and the
// tree), or on not-found — in which case the message LISTS EVERY PATH TRIED, so
// an installed binary that cannot find its config says so instead of producing a
// confusing downstream error about a missing language or a missing header.
[[nodiscard]] DSS_EXPORT LoadResult<std::filesystem::path>
findShippedConfig(ShippedConfigLocator const& loc);

// Locate a shipped config DIRECTORY (`src/dss-config/<subdir>`) — the directory
// analogue of `findShippedConfig`, and the ONE place that precedence lives for
// the directory form.
//
// WHY IT EXISTS. Four sites needed "resolve something under src/dss-config/" and
// three of them hand-rolled the walk: the shipped-lib descriptor scan
// (`ffi/shipped_lib_descriptor.cpp`), the LSP language discovery
// (`lsp/schema_cache.cpp`), and the driver's system-include dirs
// (`program.cpp::applySystemDirs`). The copies DRIFTED on the one thing that
// matters — the driver's forgot to read `DSS_CONFIG_ROOT` at all, so the shipped
// CLI could not resolve `#include <stdio.h>` from any cwd outside its own source
// tree even with the documented override set (MEASURED: cwd in-tree → rc 0; cwd
// `C:\` → `error[F001A]: got stdio.h`, same binary, override set in both arms).
// Route every new directory lookup through here rather than opening a fifth walk.
//
// PRECEDENCE — deliberately identical to `findShippedConfig`'s (they share one
// implementation, so they cannot end up reading DIFFERENT config trees):
//   1. `$DSS_CONFIG_ROOT/src/dss-config/<subdir>`, validated as a DIRECTORY
//   2. `<installed config root>/<subdir>`, same validation, AUTHORITATIVE
//   3. the cwd ancestor walk (up to 8 hops), same validation
//   4. `std::nullopt` — a set-but-miss override falls THROUGH to the arms below,
//      so a stale value never worsens discovery.
// Validation is `is_directory` rather than the file form's `exists` because that
// IS the analogue: `exists` would accept a plain file named `shippedLibs` and
// hand a caller a "directory" it cannot iterate.
//
// ⚠ A VERSION-SKEWED TREE AND A GENUINE MISS ARE THE SAME ANSWER HERE (nullopt),
// and that is a stated narrowing rather than an oversight — see the note at the
// return in the .cpp. The skew EXPLANATION comes from `findShippedConfig`, which
// every compilation reaches first. What matters for correctness holds either
// way: a skewed tree is never USED.
//
// `subdir` is CONFIG-SUPPLIED, not a user-supplied logical name — a literal
// ("sources", "shippedLibs") or a `.lang.json` value that may itself be nested
// (`semantics.shippedLibDirs`, e.g. "shippedLibs/windows-x86_64"). It therefore
// does NOT get the file form's path-like-name rejection, which exists to stop an
// untrusted `loadShipped` name escaping the config tree via `../`. Do not
// forward untrusted input here.
//
// `startPath`, when engaged, means "discover from exactly here": the env
// override is SKIPPED and the walk starts there instead of cwd. Only the LSP
// uses it, and it is load-bearing — the discovery fixtures point at a scratch
// dir, and honouring the ambient environment over an explicit caller argument
// would make them untestable.
//
// Returns `std::optional` rather than `LoadResult` on purpose: every caller
// already owns a distinct not-found behaviour (fall through to another tier /
// report "not located" / skip the dir so the miss fails loud downstream), and
// none of them would forward a diagnostic manufactured here.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
findShippedConfigDir(
    std::string_view                            subdir,
    std::optional<std::filesystem::path> const& startPath = std::nullopt);

} // namespace dss
