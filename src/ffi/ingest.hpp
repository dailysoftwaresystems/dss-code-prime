#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/c_header_parser.hpp"  // HeaderReadError
#include "ffi/import_surface.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"  // HirFfiMap
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Plan 11 FF5 — `ingest()`. Composes FF1 (binary readers), FF2 (C
// header parser), FF3 (ABI catalog), FF4 (C name mangling) into a
// single pipeline that populates `HirFfiMap` from real library
// surfaces.
//
// Source-language agnostic: takes a HIR module (built from any
// source language) + a list of (HirNodeId, canonicalName) extern
// declarations the caller pre-resolved from the SemanticModel.
// Target-blind: takes a TargetSchema + ObjectFormatSchema; uses FF3
// + FF4 closed-table dispatch on (target.name, format.kind) and
// `ObjectFormatKind` respectively.
// Linker-blind: produces `FfiMetadata` rows; the linker consumes
// them downstream via `collectExterns` in MIR lowering.
//
// At this cycle ELF-only via FF1's existing reader; FF1-PE +
// FF1-MachO triggers separately. The dispatch is internal to
// `binary_reader.cpp` — FF5 transparently picks up new formats.

namespace dss::ffi {

// ── IngestionSource (D-FF5-INGESTION-SOURCE) ───────────────────
// Closed-set of input shapes `ingest()` accepts. New variants land
// here when a new ingest shape arrives (e.g. archive `.a`/`.lib`,
// JSON-described surface, etc.).

struct DSS_EXPORT BinaryLibrarySource {
    // Path to a `.so` / `.dll` / `.dylib`. FF1 binary readers
    // discover the library's mangledName + identity from the
    // binary itself; no caller-supplied importLibrary required.
    std::filesystem::path path;
};

struct DSS_EXPORT CHeaderSource {
    std::filesystem::path path;
    // Owning library name (e.g. "libc.so.6") — headers don't
    // carry runtime library identity, so the caller supplies it.
    std::string importLibrary;
};

struct DSS_EXPORT CHeaderDirSource {
    std::filesystem::path dir;
    std::string importLibrary;
};

using IngestionSource = std::variant<BinaryLibrarySource,
                                      CHeaderSource,
                                      CHeaderDirSource>;

// ── ExternDeclRef — caller-provided HIR ↔ canonical-name map ──
//
// FF5 does not depend on `SemanticModel` — the caller pre-resolves
// each extern declaration's HirNodeId + canonical (undecorated) C
// identifier and passes the list. This keeps FF5 source-language
// agnostic + lets test fixtures construct the list directly.

struct DSS_EXPORT ExternDeclRef {
    HirNodeId        node;
    std::string_view canonicalName;
};

// ── HirIngestResult ─────────────────────────────────────────────

struct DSS_EXPORT HirIngestResult {
    std::size_t externsAnnotated = 0;  // # of (node, FfiMetadata) entries written to ffiMap
    std::size_t sourcesProcessed = 0;  // # of IngestionSource entries successfully read
    std::size_t rowsAggregated   = 0;  // # of ImportSurface rows in the union surface
    bool        ok               = false;
};

// ── Public entry point ─────────────────────────────────────────
//
// Read each `IngestionSource`, aggregate `ImportSurface` rows,
// unapply per-format mangling (FF4 strict) on binary-reader rows
// to recover canonical names, match each `externs[i].canonicalName`
// against the aggregated surface, apply FF4 to produce the
// linker-visible decorated mangledName, populate `ffiMap` with the
// resolved `FfiMetadata`.
//
// FF3 (`resolveAbi`) is invoked once to validate (target × format)
// compatibility; the result is not stored in `FfiMetadata` today
// (CallConv lives on the FnSig TypeId, per the post-FF3 design).
//
// Behavior:
//   * Missing source: caller-API-level — fails loud via reporter
//     on the FIRST source that fails to read.
//   * Duplicate mangledName across sources: silent for the first
//     occurrence; subsequent duplicates skipped with an info
//     diagnostic (the FFI spec treats first-source-wins as
//     deterministic; downstream linkers do the same).
//   * Extern in `externs` with no match in aggregated surface:
//     skipped without error — the user's source declares the
//     extern but no library/header provides it; the linker will
//     fail loud at link time with `K_SymbolUndefined`. FF5 reports
//     the count via `externsAnnotated < externs.size()` so the
//     driver can warn upfront if desired.
[[nodiscard]] DSS_EXPORT HirIngestResult
ingest(std::span<IngestionSource const> sources,
       std::span<ExternDeclRef const>   externs,
       TargetSchema const&              target,
       ObjectFormatSchema const&        format,
       HirFfiMap&                       ffiMap,
       DiagnosticReporter&              reporter);

// ── D-FF6-HEADER-DIR-READER — multi-file libraries ────────────
//
// Enumerate every `*.h` file under `headerDir` (non-recursive),
// invoke `readCHeader` on each in alphabetical order, and return
// the merged ImportSurface row list. Each file shares the same
// `importLibrary` identity (the directory is taken to be a single
// library's curated header set).
//
// Alphabetical-order is deterministic — round-trips with the
// shipped `src/dss-config/ffi-headers/<lib>/*.h` layout.
//
// Failure modes: any per-file failure halts the directory read
// and propagates the underlying HeaderReadError (operator gets the
// first failure's prose, not a summary across multiple files).
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderDirectory(std::filesystem::path const& headerDir,
                     std::string_view             importLibrary,
                     DiagnosticReporter&          reporter);

} // namespace dss::ffi
