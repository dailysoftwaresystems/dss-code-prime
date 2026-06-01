#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "ffi/import_surface.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

// Plan 11 FF1 — binary import-surface reader. Read a shared library
// (`.so` / `.dll` / `.dylib`) on disk and return the list of exported
// symbols as `ImportSurface` rows.
//
// Format-blind dispatch: the entry point `readImports(path, reporter)`
// detects the format from the file's first bytes (ELF magic
// `\x7FELF`, PE `MZ`...`PE\0\0`, Mach-O `0xFEEDFACF` / `0xCAFEBABE`)
// and routes to the per-format implementation. Adding a new format
// = add a magic check + a per-format `readImports*` implementation
// in `ffi/<format>_binary_reader.{hpp,cpp}` + a dispatch arm.
//
// **Closure scope (FF1)**: ELF + PE + Mach-O readers shipped
// (FF1-ELF 2026-06-01, FF1-PE 2026-06-01, FF1-MachO 2026-06-01).
// Mach-O FAT (`0xCAFEBABE` universal) and 32-bit Mach-O
// (`0xFEEDFACE`) recognised but route to `UnsupportedFormat`
// with remediation-specific messages — anchors D-FF1-MACHO-FAT
// and D-FF1-MACHO-32 reserve the future arms.
//
// D-FF1-NEST CLOSED (FF1-MachO cycle 2026-06-01): per-format reader
// TUs live in `binary_readers/{elf,pe,macho}_reader.{hpp,cpp}` —
// shared byte primitives in `binary_readers/reader_common.hpp`.

namespace dss::ffi {

// Plan 11 §2.6 F_* diagnostic-code namespace (FFI errors). These are
// the binary-reader-side codes; the C-header-parser (FF2) reuses
// the same prefix.
//
// Returned via `BinaryReadError`'s `kind` field. Each kind carries a
// remediation-distinct user message. The codes are mapped to the
// codebase's `DiagnosticCode::F_*` enum entries via the integer
// `static_cast<DiagnosticCode>(...)` — same pattern as the
// link-tier K_* and assembler-tier A_* codes.
enum class BinaryReadErrorKind : std::uint8_t {
    FileOpenFailed     = 0,  // path doesn't exist / permission / IO
    FileEmpty          = 1,  // zero-byte file
    UnknownFormat      = 2,  // no recognised magic bytes
    UnsupportedFormat  = 3,  // magic recognised but variant unsupported (Mach-O FAT / Mach-O 32)
    CorruptedBinary    = 4,  // truncated section, invalid offsets, etc.
    UnsupportedElfClass = 5, // ELF32 / non-LE / etc. — v1 supports ELF64 LE only
    SectionNotFound    = 6,  // expected .dynsym / .dynstr missing
};

struct DSS_EXPORT BinaryReadError {
    BinaryReadErrorKind kind = BinaryReadErrorKind::UnknownFormat;
    std::string         detail;
};

[[nodiscard]] DSS_EXPORT std::string_view
    binaryReadErrorKindName(BinaryReadErrorKind k) noexcept;

// Test-exposed overflow-safe bounds check: does `[off, off+size)` lie
// inside `[0, totalSize)`? The naive `off + size > totalSize` wraps
// when `off + size` overflows u64; this guards via subtraction. Used
// internally by every binary reader's section-bounds validation.
//
// Exposed via the public header (not just .cpp) so tests can pin the
// wrap-bypass case directly — the silent-failure surface this guards
// is exactly what the post-fold #1 closed (a hostile/corrupted `.so`
// with `sh_offset = UINT64_MAX-4, sh_size = 8` would have slipped
// past the pre-fold check). Indirect ELF-synthesis coverage is
// fragile against parser-order refactors. (pr-test-analyzer Gap 1
// priority 9, post-fold #2.)
[[nodiscard]] DSS_EXPORT constexpr bool
rangeExceedsBuffer(std::uint64_t off, std::uint64_t size,
                   std::uint64_t totalSize) noexcept {
    return off > totalSize || size > totalSize - off;
}

// Read the import surface (exported symbols) from a shared library
// on disk. Detection is format-blind: the dispatch reads the first
// few bytes to identify ELF / PE / Mach-O and routes to the
// per-format implementation.
//
// Errors fail loud — none of "file empty / bad magic / corrupted /
// unsupported format" silently produces an empty surface. Successful
// returns produce `std::vector<ImportSurface>` with at least one row
// (an empty `.dynsym` `.so` is a corrupted library — flagged as
// `CorruptedBinary` rather than returned as an empty list).
//
// `reporter` is used for per-row diagnostics during parsing —
// individual symbols with corrupted name offsets emit
// `F_BinaryReaderPartialCorruption` at Warning severity (counter-
// aggregated; one Warning per parse summarizing N skips) and DO
// NOT abort. `--warnings-as-errors` elevates to fail-loud.
// D-FF1-PARTIAL-CORRUPTION-LOUD (2026-06-01).
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, BinaryReadError>
readImports(std::filesystem::path const& libraryPath,
            DiagnosticReporter&          reporter);

// Read the import surface from an in-memory byte buffer. Used by
// tests (synthesize a binary, read it back) and by future caching
// layers. Same semantics as `readImports(path, reporter)`.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, BinaryReadError>
readImportsFromBytes(std::span<std::uint8_t const> bytes,
                     std::string_view              libraryPathLabel,
                     DiagnosticReporter&           reporter);

} // namespace dss::ffi
