#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"   // LoadResult + ConfigDiagnostic
#include "core/types/parse_diagnostic.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
// installed executable (e.g. `../share/dsscp/0.0.2/dss-config`).
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

// THE RESOLVED SYSTEM-INCLUDE DIRS for `grammar`: its declared
// `semantics.shippedLibDirs` strings mapped through `findShippedConfigDir` to
// ABSOLUTE directories, in the language's own order. This is the `/usr/include`
// analogue — the search path the ANGLE form `#include <h>` resolves against,
// and the corpus a predefined macro's `impliedSurface` claim is validated
// against. AGNOSTIC: every dir comes from the language document; nothing here
// knows a language, a target, or a format.
//
// ★★ ONE OWNER, BECAUSE THREE SITES HELD THREE DIFFERENT ANSWERS —
// D-LSP-HAS-NO-SYSTEM-INCLUDE-DIRS-AND-DROPS-THE-CU-DRIVER-DIAGNOSTICS.
// The driver defined this walk at `dss` NAMESPACE SCOPE inside
// `program/program.cpp`, with no header declaring it — only a forward
// declaration at the top of that same file, i.e. a signature with no owner.
// `program/dump_predefined_macros.cpp` re-walked `shippedLibDirs` with its own
// loop. And `src/lsp/` had NO copy at all, so the editor built every buffer
// with an EMPTY system path: `#include <stdbool.h>` failed in the editor while
// the compiler accepted it, and `#include <no_such_header.h>` — fatal
// `F_ShippedHeaderNotFound` for the compiler — was reported by the editor as a
// clean file.
//
// ★ IT LIVES HERE, and the alternative was measured rather than assumed. The
// other candidate was `analysis/compilation_unit/`, beside
// `UnitBuilder::addSystemDir`. But this answer is a property of the (language
// document, config root) pair and of NOTHING else — `dump_predefined_macros.cpp`
// wants the vector with no `UnitBuilder` anywhere in sight, and would have had
// to include the whole CU header (the one already carrying `/bigobj` on MSVC for
// its include surface) to reach a function about config paths. Putting it here,
// beside the `findShippedConfigDir` it wraps, is the same move
// `D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS` made for the project-manifest
// reader: down to the tier every reader already depends on, so the dependency
// runs one way. Binding the result to a builder is `applySystemDirs`, declared
// one tier up beside `UnitBuilder::addSystemDir` — where the `UnitBuilder` is.
//
// A dir that resolves NOWHERE is SKIPPED rather than reported. An unresolvable
// entry only matters if something actually asks for a header, and that miss
// then fails loud downstream as `F_ShippedHeaderNotFound`, positioned at the
// include that wanted it — whereas reporting here would fire on every compile.
//
// ⚠ THE PATHS ARE ABSOLUTE, and that is a REQUIREMENT rather than a tidiness:
// `ResolutionContext::systemDirs` documents its dirs as absolute. The cwd walk
// produces that for free; a RELATIVE `DSS_CONFIG_ROOT` — permitted, see the
// precedence above — would not.
[[nodiscard]] DSS_EXPORT std::vector<std::filesystem::path>
resolveSystemDirs(GrammarSchema const& grammar);

// ─────────────────────────────────────────────────────────────────────────────
// WHICH TREE ANSWERED — [[D-PROGRAM-CONFIG-DIR-WALK-RESOLVES-A-FOREIGN-TREE]]
// ─────────────────────────────────────────────────────────────────────────────
//
// ★★★ THE DEFECT, ✔MEASURED (P54, and by lane `el` before that). The precedence
// above is CORRECT and is not what is being changed here: what was missing is
// that its OUTCOME was invisible. A binary built from tree A, invoked with a
// working directory inside tree B, reads TREE B's `src/dss-config` — and then
// answers every vocabulary question out of it, perfectly, about config the
// caller did not think they were using. ✔MEASURED 2026-09-02, ONE variable
// changed (cwd), same binary, `DSS_CONFIG_ROOT` unset in both arms:
//     cwd in the build tree → rc 0
//     cwd in tree B         → rc 1, `unknown mnemonic 'fadd' … (assembly
//                             dialect 'AsmArm64Gas', target 'arm64')`
// which is indistinguishable from "the row you just added does not work". Lane
// `el` lost a cycle of measurement to exactly that reading.
//
// ⚠ THE REMEDY IS FAIL-LOUD, NOT FAIL-MORE. Running a binary against another
// tree's config is LEGITIMATE — `DSS_CONFIG_ROOT` exists to do it — so nothing
// below refuses anything. It reports.
//
// ★ THE NOT-FOUND CASE WAS ALREADY SELF-DIAGNOSING and is deliberately
// untouched: `findShippedConfig`'s miss lists EVERY path tried (✔MEASURED — an
// unknown `--language` prints nine `tried:` lines naming both trees). The blind
// case is the one where a document DID load, out of the wrong tree.

// Which arm of the precedence answered. Reported rather than inferred: a caller
// that re-derived it would be answering an adjacent question, and the three
// arms mean three different things about whether the outcome is a surprise.
enum class ConfigRootArm : std::uint8_t {
    EnvOverride,      // 1. $DSS_CONFIG_ROOT — the operator named the tree
    InstalledLayout,  // 2. beside the executable — agreement is by construction
    CwdWalk,          // 3. an ancestor of the working directory — DISCOVERED
};

[[nodiscard]] DSS_EXPORT std::string_view
configRootArmLabel(ConfigRootArm arm) noexcept;

struct DSS_EXPORT ResolvedConfigRoot {
    std::filesystem::path root;   // the `dss-config` directory itself
    ConfigRootArm         arm{};

    // `$DSS_CONFIG_ROOT` was SET and did not answer, so `arm`/`root` came from
    // a later arm. Carries the value as the operator spelled it.
    //
    // ★★ THE FALL-THROUGH ON A SET-BUT-MISSED OVERRIDE IS CORRECT AND IS NOT
    // CHANGED — a stale override must never worsen discovery. What was wrong is
    // that it was SILENT, which is the same invisible-outcome class as the
    // foreign tree above and has already cost a measured 5x false regression
    // (the speedtest1 benchmark's pin was one directory too deep, missed, and
    // fell through to the very cwd walk it existed to prevent). That row fixed
    // its own pin and recorded the rest as "a production question, raised
    // rather than taken":
    // [[D-BENCH-CONFIG-ROOT-PIN-IS-ONE-LEVEL-TOO-DEEP-AND-SILENTLY-DOES-NOTHING]]
    std::optional<std::string> ignoredOverride;
};

// The config ROOT for this invocation, by the same `resolveByPrecedence` the
// two lookups above run — never a second, private walk.
//
// ⚠ IT ANSWERS A SLIGHTLY WIDER QUESTION THAN `findShippedConfig`, AND THE
// DIFFERENCE IS STATED RATHER THAN GLOSSED: this probes `<root>` as a
// DIRECTORY, so it stops at the first ancestor that has a `src/dss-config/` at
// all, while the file form continues past an ancestor whose tree lacks the
// requested leaf. They can therefore disagree for a PARTIALLY POPULATED tree.
// Nothing that must be exact reads this — the provenance note below is keyed on
// the arm and used for a report line, and `configDocumentOrigin()` on the loaded
// schema is what a DIAGNOSTIC names, because that is the tree that actually
// answered it.
[[nodiscard]] DSS_EXPORT std::optional<ResolvedConfigRoot>
resolveShippedConfigRoot();

// The config ROOT a resolved shipped-document path came out of — i.e. the
// inverse of the `<root>/<subdir>/<leaf>` composition `resolveByPrecedence`
// performs. It lives HERE, beside that composition, because an inverse written
// anywhere else is a second owner of the layout and free to drift from it.
//
// nullopt when `document` has no two parents to strip — a schema built from
// text carries a LABEL (`<inline>`) rather than a path, and inventing a root
// for it would name a directory nobody read.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
configRootOfShippedDocument(std::filesystem::path const& document);

// The SOURCE TREE this compiler was built from — `CMAKE_SOURCE_DIR`, baked by
// `src/core/CMakeLists.txt`. It is a comparison operand and NEVER a lookup arm;
// see that file for why adding it to the precedence would be a defect.
[[nodiscard]] DSS_EXPORT std::string_view buildSourceDir();

// The one-line provenance sentence for a resolved root, or nullopt when there
// is nothing surprising to say. PURE with respect to discovery — it is handed
// the answer rather than going to look for one — which is what lets a test
// drive every arm against a planted tree.
//
// ENGAGED ON EITHER OF TWO SURPRISES, and on neither otherwise:
//   1. THE CWD WALK answered a root that is not this compiler's own
//      `<buildSourceDir>/src/dss-config`. The other two arms cannot produce
//      this surprise: `$DSS_CONFIG_ROOT` that ANSWERED is the operator saying
//      which tree to use (narrating an explicit instruction back is noise, not
//      attribution), and the installed layout composes the binary's own version
//      into the path.
//   2. `$DSS_CONFIG_ROOT` was SET AND IGNORED — an explicit instruction that
//      silently did nothing. The fall-through itself is unchanged and correct.
// Both hold together often (a mis-spelled override is exactly how a caller ends
// up in a foreign tree), and the note then states the CAUSE before the
// CONSEQUENCE.
// ⓘ The comparison is `fs::equivalent`, not string equality: a lane worktree, a
// junction and a mapped drive are all spellings of one directory, and a
// spelling-based answer would report a foreign tree where there is none. An
// error from `equivalent` means one side does not exist — which, since the
// resolved root was just probed, means the BUILD tree is absent (a relocated or
// shipped binary), i.e. genuinely a different tree.
[[nodiscard]] DSS_EXPORT std::optional<std::string>
configRootProvenanceNoteFor(ResolvedConfigRoot const&    resolved,
                            std::filesystem::path const& buildTree);

// The same question for THIS invocation: resolve, then judge. nullopt when no
// config root resolves at all — a genuine miss is `findShippedConfig`'s to
// report, and it already names every path it tried.
[[nodiscard]] DSS_EXPORT std::optional<std::string> configRootProvenanceNote();

} // namespace dss
