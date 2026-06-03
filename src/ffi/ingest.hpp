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
#include <optional>
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
// Source-language agnostic: takes a caller-resolved list of
// (HirNodeId, canonicalName) extern declarations — never inspects
// the HIR tree, never reads SemanticModel. Caller pre-resolves
// names from whatever source language produced the externs.
// Header-source ingest is c-subset-only today (FF2's grammar);
// binary-source ingest is format-blind (FF1 dispatches on magic
// bytes).
// Target-blind: takes a TargetSchema + ObjectFormatSchema; uses FF3
// + FF4 closed-table dispatch on (target.name, format.kind) and
// `ObjectFormatKind` respectively.
// Linker-blind: produces `FfiMetadata` rows; the linker consumes
// them downstream via `collectExterns` in MIR lowering.
//
// All three FF1 readers (ELF, PE, Mach-O) have landed (see
// `src/ffi/binary_readers/`); the dispatch is internal to
// `binary_reader.cpp` and FF5 transparently routes a
// `BinaryLibrarySource{path}` to the right reader based on the
// file's magic bytes.

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
    // D-CSUBSET-EXTERN-LIBRARY-SYNTAX closure (step 13.3, 2026-06-02):
    // empty = the FFI synthesize stage uses the caller-supplied
    // format-level default `importLibrary`; non-empty = per-symbol
    // override (e.g. "kernel32.dll" for GetStdHandle/WriteFile when
    // the language default is "msvcrt.dll"). Source-language
    // agnostic — any language whose lowerer populates this field
    // gets per-symbol routing.
    std::string_view libraryOverride{};
};

// ── HirIngestResult ─────────────────────────────────────────────

// Result struct. `ok()` returns true iff the reporter held zero
// errors at the moment the producer (`ingest()` or
// `synthesizeFfiFromSourceDecls()`) returned. The reporter is the
// single source of truth — `ok()` is a snapshot taken at return so
// later reporter activity (by other code on the same reporter)
// can't mutate it retroactively. Default-constructed value has
// `ok() == false` (errorCountAtReturn = nullopt) — that's the
// safe sentinel: any caller that obtains a HirIngestResult through
// a path bypassing the producers correctly sees ok=false rather than
// inheriting an accidental ok=true from the default. (post-fold #6
// type-design Q1 fold: private snapshot + factory-only set; the
// producers are the only paths that can populate it. FF6 Slice 2
// 2026-06-02 extended the producer set to include
// synthesizeFfiFromSourceDecls.)
//
// IMPORTANT: `ok() == false` does NOT imply `ffiMap` is unmodified.
// Partial annotations are possible alongside !ok() (e.g., a
// duplicate-symbol Warning promoted to Error by `--warnings-as-errors`
// after some externs were already annotated). Callers deciding
// downstream behavior MUST inspect `externsAnnotated` alongside
// `ok()`, never `ok()` alone.
class DSS_EXPORT HirIngestResult {
public:
    std::size_t externsAnnotated = 0;  // # of (node, FfiMetadata) entries written to ffiMap
    std::size_t sourcesProcessed = 0;  // # of IngestionSource entries successfully read
    std::size_t rowsAggregated   = 0;  // # of ImportSurface rows in the union surface

    [[nodiscard]] bool ok() const noexcept {
        return errorCountAtReturn_.has_value()
            && *errorCountAtReturn_ == 0u;
    }
    [[nodiscard]] std::size_t errorCount() const noexcept {
        return errorCountAtReturn_.value_or(0u);
    }

    // Snapshot-once setter — only the producer functions'
    // (`ingest()` and `synthesizeFfiFromSourceDecls()`) internal
    // `returnWithSnapshot` lambda should call this. The "once"
    // semantic is enforced (a second call is a no-op): the trap
    // we're closing is "default-construct → ok() == true" by
    // accident; we don't need to forbid double-write.
    void snapshotErrorCountOnce(std::size_t n) noexcept {
        if (!errorCountAtReturn_.has_value()) {
            errorCountAtReturn_ = n;
        }
    }

private:
    // nullopt sentinel = "never snapshotted by ingest()". A
    // default-constructed HirIngestResult has `ok() == false` —
    // any caller obtaining a result through a path that bypasses
    // `ingest()`'s returnWithSnapshot lambda correctly sees
    // not-ok, NOT an accidental ok=true from the previous bool
    // default.
    std::optional<std::size_t> errorCountAtReturn_;
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
//     occurrence; subsequent duplicates skipped with a Warning-level
//     diagnostic (`F_FfiIngestDuplicateSymbol`) so the audit log
//     captures the shadowing. Downstream linkers reject true
//     link-time collisions independently. NOTE: under
//     `--warnings-as-errors`, the duplicate warning is PROMOTED to
//     Error → `result.ok() == false` even though the FFI design
//     treats first-source-wins as non-fatal. The `ffiMap` is still
//     partially populated with the first-source bindings. Callers
//     must inspect `externsAnnotated` rather than branching solely
//     on `ok()` to decide whether to consume ffiMap.
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
// Failure modes (post-fold #5 H1): per-file failures are collected.
// The directory read returns a partial surface with rows from every
// file that DID parse, and only returns std::unexpected when EVERY
// file failed. Per-file failure diagnostics already reach the
// reporter via `readCHeader`; the propagated HeaderReadError carries
// the FIRST failure's detail for triage convenience.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderDirectory(std::filesystem::path const& headerDir,
                     std::string_view             importLibrary,
                     DiagnosticReporter&          reporter);

// ── synthesizeFfiFromSourceDecls (FF6 Slice 2, 2026-06-02) ────
//
// Source-declared sibling of `ingest()`. Where `ingest()` validates
// each caller-supplied extern against an aggregated ImportSurface
// produced by reading external headers / binaries, this function
// TRUSTS each caller-supplied `ExternDeclRef` as authoritative:
// the source language's extern declaration IS the signature (the
// HIR FnSig was minted from it upstream by the CST→HIR lowerer),
// so all that remains is to (a) apply per-format FF4 C-mangling to
// produce the linker-visible decorated name and (b) bind every
// extern to the caller-supplied per-format `importLibrary`. No
// header / binary read is required.
//
// Used by `compileSingleUnit` when the active language declares
// `DeclarationRule.externLibraryByFormat` for the target's
// `ObjectFormatKind` — the canonical c-subset path for the FF6
// hello-world milestone (puts in msvcrt.dll on PE-x86_64). A
// future cycle layers header-driven validation back in via
// `ingest()` for languages that want compile-time signature
// validation against shipped headers (anchored
// `D-FFI-HEADER-VALIDATION-OPTIONAL`).
//
// Failure modes:
//   * Empty `importLibrary` → `F_FfiNoImportLibraryForFormat`
//     (unsuppressable). Means the language's `externLibraryByFormat`
//     map has no entry for `format.kind()`.
//   * Empty `ExternDeclRef::canonicalName` → `F_FfiIngestEmptyCanonical`
//     (shared with `ingest()`; same trap — would silently shadow
//     legitimately-distinct symbols in any downstream by-name
//     lookup).
//   * `resolveAbi` cc==nullptr (operand-stack / result-id) →
//     `F_FfiIngestAbiModelUnsupported` (shared; permanent
//     architectural exclusion of WASM/SPIR-V from FF4 C-mangling).
//
// On success, every extern in `externs` produces one
// `FfiMetadata{ mangledName, importLibrary, linkage=Strong,
// visibility=Default }` entry in `ffiMap`, keyed on the extern's
// HirNodeId. `externsAnnotated == externs.size()` on a clean run.
[[nodiscard]] DSS_EXPORT HirIngestResult
synthesizeFfiFromSourceDecls(
    std::span<ExternDeclRef const> externs,
    std::string_view               importLibrary,
    TargetSchema const&            target,
    ObjectFormatSchema const&      format,
    HirFfiMap&                     ffiMap,
    DiagnosticReporter&            reporter);

} // namespace dss::ffi
