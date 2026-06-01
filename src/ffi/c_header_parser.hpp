#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "ffi/import_surface.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Plan 11 FF2 — C header parser ("header mode"). Reads a pre-reduced
// C header (per plan 11 §4 Q1: hermetic, hand-curated under
// `src/dss-config/ffi-headers/<library>/`) and returns the symbol
// surface as `ImportSurface` rows — uniform with FF1's binary
// readers.
//
// Implementation is a thin orchestration over the c-subset frontend:
// load the shipped `c-subset` grammar, tokenize + parse + semantic-
// analyze + lower-to-HIR, then walk the resulting HIR module's
// top-level decls and emit one `ImportSurface` per `ExternFunction`
// / `ExternGlobal`. Typedefs are accepted but produce no rows
// (they contribute to type resolution only). Any other top-level
// construct fails loud (no silent "ignored" surface).
//
// Calling convention is NOT determined here — that's FF3 (ABI
// catalog). FF2 produces the structured signature placeholder
// (`ImportSurface::cSignature`) and `mangledName`; FF4 applies
// per-platform underscoring rules; FF3 binds the calling convention.
// Each tier composes on top of the previous one.

namespace dss::ffi {

// Closed-set of FF2 failure modes. Distinct from `BinaryReadErrorKind`
// because the failure surface is different (binary I/O failures are
// FF1's territory; this is source-text parse failures).
enum class HeaderReadErrorKind : std::uint8_t {
    FileOpenFailed         = 0,  // path doesn't exist / permission / I/O
    HeaderParseFailed      = 1,  // c-subset frontend reported errors
    HeaderHasFunctionBody  = 2,  // a non-extern function DEFINITION at top level
    HeaderHasNonExternDecl = 3,  // a non-extern non-typedef top-level decl
};

struct DSS_EXPORT HeaderReadError {
    HeaderReadErrorKind kind = HeaderReadErrorKind::HeaderParseFailed;
    std::string         detail;
};

[[nodiscard]] DSS_EXPORT std::string_view
    headerReadErrorKindName(HeaderReadErrorKind k) noexcept;

// Read a pre-reduced C header from disk and return its import surface.
// `importLibrary` names the owning library (e.g. "libc.so.6",
// "msvcrt.dll", "libSystem.B.dylib") — the header carries declarations
// but not the runtime library identity, so the caller must supply it
// (matches the project-config pattern in plan 11 §2.3).
//
// Diagnostics from the c-subset frontend pipe through `reporter`. On
// failure, the structured `HeaderReadError` carries the kind for
// programmatic dispatch; the diagnostic-reporter emission carries the
// F_* code for `--suppress` / `--warnings-as-errors` interaction.
//
// Empty `importLibrary` is rejected at the entry: a header with no
// owning library means the resulting ImportSurface rows can never be
// linked, so accepting the call would create silent-failure surface.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeader(std::filesystem::path const& headerPath,
            std::string_view             importLibrary,
            DiagnosticReporter&          reporter);

// In-memory variant. `headerPathLabel` names the buffer for
// diagnostics (e.g. a synthetic URI or `<test>` for fixtures).
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderFromText(std::string_view    text,
                    std::string_view    headerPathLabel,
                    std::string_view    importLibrary,
                    DiagnosticReporter& reporter);

} // namespace dss::ffi
