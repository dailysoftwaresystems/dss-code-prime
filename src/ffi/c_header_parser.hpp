#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "ffi/import_surface.hpp"
#include "hir/attributes/source_span.hpp"  // HirSourceLoc

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Plan 11 FF2 — C header parser ("header mode"). Reads a pre-reduced
// C header from disk OR in-memory text + returns the symbol surface
// as `ImportSurface` rows — uniform with FF1's binary readers. Thin
// orchestration over the c-subset frontend (tokenize + parse +
// semantic-analyze + lower-to-HIR); typedefs are absorbed into the
// type system, everything else either yields a row or fails loud.
//
// FF-latent (no live production caller as of 2026-06-03). The
// shipped-headers consumer (`readCHeaderShipped`) + the
// `src/dss-config/ffi-headers/` tree were removed as dead code; this
// substrate remains compiled and tested as anchored future
// capability (D-FF2 in plan 11).

namespace dss::ffi {

// Closed-set FF2 failure modes — distinct remediations → distinct
// kinds. 1:1 with `F_Header*` diagnostic codes via the
// `kHeaderReadErrorTable` (c_header_parser.cpp). Variants are
// EXPLICITLY numbered so a maintainer cannot accidentally insert a
// new variant in the middle without renumbering — keeps the table
// row-aligned.
enum class HeaderReadErrorKind : std::uint8_t {
    FileOpenFailed               = 0,  // path doesn't exist / permission / I/O
    HeaderParseFailed            = 1,  // c-subset frontend (tokenize/parse/semantic/lower) errors
    HeaderHasFunctionBody        = 2,  // a non-extern function DEFINITION at top level
    HeaderHasNonExternDecl       = 3,  // a non-extern non-typedef global definition
    EmptyImportLibrary           = 4,  // caller passed empty importLibrary — caller-API bug
    GrammarLoadFailed            = 5,  // shipped c-subset grammar JSON could not load
    HeaderHasUnsupportedTopLevel = 6,  // ImportGroup (#include), future HirKind, etc.
    InternalInvariant            = 7,  // compiler-bug surface — file a bug
};

// D-FF2-2: source location of the offending construct, set by per-decl
// rejection sites (HeaderHasFunctionBody / HeaderHasNonExternDecl /
// HeaderHasUnsupportedTopLevel / InternalInvariant) and by HeaderParseFailed
// when a downstream lowering emitted a span-bearing diagnostic. Entry-point
// errors that have no node-level locus (FileOpenFailed / EmptyImportLibrary /
// GrammarLoadFailed) leave this default-constructed —
// `at.isAbsent() == true`, the canonical "no source location" value per
// `hir/attributes/source_span.hpp:30-60`. No `std::optional` wrapper —
// `HirSourceLoc{}` is already the documented absent sentinel (post-fold #7
// type-design T1 fold: one absence representation, not two; post-fold #9
// type-design Q1 anchor: `HirSourceLoc::absent()` is the canonical spelling
// at producer sites).
// Programmatic consumers (LSP, test pins, future introspection) read the
// location directly from the struct rather than re-parsing reporter prose.
struct DSS_EXPORT HeaderReadError {
    HeaderReadErrorKind kind = HeaderReadErrorKind::HeaderParseFailed;
    std::string         detail;
    HirSourceLoc        at{};
};

[[nodiscard]] DSS_EXPORT std::string_view
    headerReadErrorKindName(HeaderReadErrorKind k) noexcept;

// Read a pre-reduced C header from disk and return its import
// surface. `importLibrary` names the owning library (`"libc.so.6"`,
// `"msvcrt.dll"`, `"libSystem.B.dylib"`) — headers carry declarations
// but not the runtime library identity, so the caller must supply it
// (matches plan 11 §2.3). Empty `importLibrary` is rejected at the
// entry with `EmptyImportLibrary` (silent-failure surface: a row
// downstream with no library identity would be unlinkable).
//
// Diagnostics from the c-subset frontend pipe through `reporter`
// (P_*/L_*/S_*/H_*) AND the FF2-layer F_* verdict; `--suppress` /
// `--warnings-as-errors` apply. Underlying causes that were
// suppressed by user policy are inlined into the wrap message so
// the FF2 verdict stays self-sufficient.
//
// Lifetime: `importLibrary` / `text` / `headerPathLabel` are not
// retained past return — the function copies what it needs into
// `ImportSurface::libraryPath` and diagnostic messages.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeader(std::filesystem::path const& headerPath,
            std::string_view             importLibrary,
            DiagnosticReporter&          reporter);

// In-memory variant. `headerPathLabel` names the buffer for diagnostics
// (e.g. a synthetic URI or `<test>` for fixtures).
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderFromText(std::string_view    text,
                    std::string_view    headerPathLabel,
                    std::string_view    importLibrary,
                    DiagnosticReporter& reporter);

} // namespace dss::ffi
