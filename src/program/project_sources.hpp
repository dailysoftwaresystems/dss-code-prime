#pragma once

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

// Resolve a manifest's `sources[]` entries into the concrete, de-duplicated file
// list a build compiles, IN MANIFEST ORDER.
//
// `baseDir` — WHAT A RELATIVE ENTRY IS RELATIVE TO. EMPTY ⇒ the process working
// directory, byte-for-byte the pre-AP6 behaviour (a literal is kept VERBATIM,
// character for character, and a glob is walked from the cwd). NON-EMPTY ⇒ every
// RELATIVE entry — LITERAL ENTRIES INCLUDED — resolves against `baseDir`, and the
// returned strings are spelled `baseDir`-rooted so the caller can open them from
// any cwd. An ABSOLUTE entry is never re-based under either setting.
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
