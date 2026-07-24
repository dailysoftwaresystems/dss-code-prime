#pragma once

// Glob pattern support for the project manifest's `sources[]` expansion
// (D-AP2-SOURCES-GLOB). SOURCE-language / TARGET-CPU / LINKER-format AGNOSTIC:
// pure filename-pattern matching + a filesystem walk over path text, with no
// language / target / object-format identity anywhere. The driver
// (`Program::compileProject`) calls these to expand a manifest source pattern
// into concrete files BEFORE the multi-vs-single-CU routing count is taken, so a
// `"src/**/*.c"` entry routes exactly as if its matches had been listed
// literally. The matcher (`globMatch`) is filesystem-free + deterministic — the
// cheapest strong test surface; the filesystem walk (`expandGlob`) layers on top.

#include "core/export.hpp"

#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dss {

// True iff `pattern` contains a glob metacharacter (`*`, `?`, or `[`). A pattern
// with NONE is a LITERAL path (the driver keeps it verbatim — unchanged
// behavior); one with ANY is expanded against the filesystem. This is the SINGLE
// predicate that decides literal-vs-glob, so the driver and any future caller
// can never drift on the boundary.
[[nodiscard]] DSS_EXPORT bool hasGlobMetacharacters(std::string_view pattern) noexcept;

// Match a slash-separated PATH against a slash-separated glob PATTERN, segment by
// segment. Pure (NO filesystem access) + deterministic. Supported syntax:
//   *      — any run of characters WITHIN one path segment (does NOT cross '/')
//   ?      — exactly one character within a segment
//   [...]  — a character class: ranges (`a-z`), negation (`[!...]` / `[^...]`), a
//            literal `]` as the first member; an unterminated `[` is a literal
//   **     — a WHOLE segment that matches zero OR MORE path segments (recursive):
//            `a/**/b` matches `a/b`, `a/x/b`, `a/x/y/b`; `**/*.c` matches `x.c`
//            AND `sub/x.c`
// Both sides split on '/'. Host-separator-agnostic — callers pass forward-slash
// paths (a `std::filesystem::path`'s `generic_string()`).
[[nodiscard]] DSS_EXPORT bool globMatch(std::string_view pattern,
                                        std::string_view path);

// Expand a glob `pattern` against the filesystem, APPENDING every matching
// REGULAR file to `out` (SORTED lexicographically + de-duplicated — a
// deterministic, reproducible order across platforms). A RELATIVE pattern
// resolves against the process working directory (the same base a literal source
// uses); an ABSOLUTE pattern resolves directly. Only the filesystem subtree under
// the pattern's literal leading prefix is walked, and a non-`**` pattern prunes
// the walk at its fixed depth — so `src/**/*.c` never scans a sibling `build/`.
//
// Returns false and sets `ec` ONLY on a genuine filesystem I/O error (a directory
// that cannot be read during the walk — FAIL LOUD). A pattern that simply matches
// NOTHING (including a base directory that does not exist / is not a directory) is
// NOT an error here: it returns true and appends nothing — the CALLER owns the
// zero-match policy (the driver fails that loud with a pattern-naming
// diagnostic). `out` is APPENDED to (never cleared), matching the InputResolver
// convention.
[[nodiscard]] DSS_EXPORT bool expandGlob(std::string_view pattern,
                                         std::vector<std::string>& out,
                                         std::error_code& ec);

} // namespace dss
