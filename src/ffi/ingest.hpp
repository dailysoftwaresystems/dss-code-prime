#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/binary_reader.hpp"    // BinaryReadError (readImportsForTargetFormat)
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
// Header-source ingest is c-only today (FF2's grammar);
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

// ── D-FFI-DECLARED-IMPORT-NAME — the recorded-identity precedence ──
//
// Reading a library binary answers TWO separate questions, and this struct
// keeps them separate:
//   * WHICH SYMBOLS exist -> always the file at `path` (the FF1 reader).
//   * WHAT IDENTITY to record for them (the ELF DT_NEEDED / Mach-O
//     LC_LOAD_DYLIB / PE import-descriptor name the LOADER resolves at
//     runtime) -> a THREE-LEVEL precedence, highest first:
//
//       1. `declaredImportName`  — the caller STATES it (below).
//       2. `row.soname`          — the binary's OWN embedded identity, which
//                                  the FF1 readers extract (c171,
//                                  D-FF1-READER-SONAME): ELF DT_SONAME /
//                                  Mach-O LC_ID_DYLIB install name / PE
//                                  export-directory DllName.
//       3. `importName`          — the path-derived basename FALLBACK (below).
//
// The decision site is `ingest()` in `ingest.cpp` (search
// `meta.importLibrary =`); it is the ONLY place the three levels are ranked.
// FORMAT-BLIND on purpose: the readers normalise all three object formats'
// embedded identities into the one `row.soname` field, so the precedence has
// no per-format arm.
struct DSS_EXPORT BinaryLibrarySource {
    // Path to a `.so` / `.dll` / `.dylib`. FF1 binary readers
    // discover the library's mangledName + identity from the
    // binary itself; no caller-supplied importLibrary required.
    std::filesystem::path path;
    // PRECEDENCE LEVEL 3 (LOWEST) — the path-derived basename FALLBACK.
    //
    // c162 (D-FF1-READER-CONSUMER) introduced this as the library IDENTITY to
    // record in each resolved extern's import. EMPTY (the default) ⇒ `ingest()`
    // binds each matched extern to the binary reader's own `libraryPath` label
    // (== the on-disk path passed to `readImports`) -- the pre-c162 behavior
    // every header/JSON caller relies on. NON-EMPTY ⇒ `ingest()` replaces
    // `libraryPath` on every row read from THIS source with `importName`, so
    // the linker records a loader-resolvable name rather than the absolute
    // build-time path (a Windows path in an ELF DT_NEEDED would never load).
    //
    // c171 (`D-FF1-READER-SONAME`) demoted it: the FF1 readers now EXTRACT the
    // binary's OWN embedded identity into `row.soname`, and `ingest()` PREFERS
    // that whenever the binary declares one. So this is the FALLBACK the driver
    // supplies for a library carrying NO embedded soname (the file's basename,
    // exactly what a foreign linker records for a `-l<name>` / `gcc -shared`
    // library with no explicit `-soname`), NOT the primary source it once was.
    //
    // Historical note, because the word matters: c162's comment called this an
    // "override". It is NOT the override any more -- `declaredImportName` is.
    std::string importName;
    // PRECEDENCE LEVEL 1 (HIGHEST) — the caller-STATED runtime identity.
    //
    // EMPTY (the default) = NOT STATED ⇒ falls through to level 2 (the embedded
    // soname), then level 3. NON-EMPTY ⇒ this string IS the recorded import
    // library, beating even an embedded soname. Never whitespace-only or
    // otherwise degenerate: the CLI / project-manifest boundary rejects those
    // LOUD (`CliArgsError::InvalidResolveLibrary` / `C_MalformedJson`) so an
    // unusable identity can never reach here silently.
    //
    // WHY it must outrank the embedded soname: CROSS-COMPILATION from a
    // STAND-IN library binary. To build a Darwin artifact on a Windows/Linux
    // host we read the Tcl symbols out of a MacPorts `.dylib` whose
    // LC_ID_DYLIB is `/opt/local/lib/libtcl8.6.dylib`. Level 2 would record
    // that MacPorts prefix as the LC_LOAD_DYLIB, and the artifact would then
    // demand MacPorts at that exact path on the target Mac -- a dyld load
    // FAILURE at runtime, with no build error anywhere. A real toolchain
    // covers this with a sysroot stub / `.tbd` / `-dylib_file`: SYMBOLS from
    // the file you can read, IDENTITY from the declaration. This field is that
    // declaration, and it is the only level a caller can state.
    //
    // LAST field so every existing positional aggregate initializer
    // (`BinaryLibrarySource{path, basename}`, fixtures included) keeps
    // compiling and defaults it to "" == not stated.
    std::string declaredImportName;
};

// ★★★ THE RECORDED-IMPORT-IDENTITY DECISION, AS A FUNCTION — the ONE place the
// three levels documented on `BinaryLibrarySource` above are ranked.
//
// The linker emits `ExternImport.libraryPath` as the artifact's DT_NEEDED /
// LC_LOAD_DYLIB / PE import-descriptor name, so this expression IS the emitted
// binary's runtime dependency. It was an inline conditional inside `ingest()`
// while `ingest()` was the only binder; the `encode` pipeline tier
// (D-ASM-RESOLVE-LIBRARY-SILENTLY-IGNORED-ON-ENCODE-TIER) is a SECOND binder —
// a hand-written `.s` names its externs as ON-BINARY symbols and never has a
// canonical C identifier to mangle, so it matches a library's export table by a
// different key and cannot go through `ingest()`'s HIR-node-keyed path. Two
// binders that ranked the identity levels separately would be exactly the
// "second owner of one fact" this codebase keeps deleting (the per-language
// `externLibraryByFormat` default that UCRT-P4 removed), so the ranking became
// a function instead of being copied.
//
//   1. `declaredImportName` — the caller STATED it (`--resolve-library
//      <path>=<import-name>`). Beats everything: the file being READ may be a
//      cross-compilation STAND-IN whose own embedded identity names a path that
//      will not exist on the target.
//   2. `embeddedSoname` — the binary's OWN identity (ELF DT_SONAME / Mach-O
//      LC_ID_DYLIB / PE export DllName), normalised into `ImportSurface::soname`
//      by the FF1 readers.
//   3. `readerLibraryPath` — the reader's own label for the file, which the
//      driver supplies as the path BASENAME for a library declaring no soname.
//
// FORMAT-BLIND and LANGUAGE-BLIND: no arm branches on object format, target or
// source language — the readers already collapsed all three formats' embedded
// identities into one field, and both callers pass plain strings.
[[nodiscard]] DSS_EXPORT std::string
recordedImportIdentity(std::string_view declaredImportName,
                       std::string_view embeddedSoname,
                       std::string_view readerLibraryPath);

// ★★★ THE TARGET-AWARE READ — the ONE place a `--resolve-library` binary's own
// object format is compared against the format the build is emitting.
// (D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL, TF-C116.)
//
// ── WHY A GUARD IS NEEDED AT ALL, AND WHY THE OLD ONE WAS AN ACCIDENT ───────
// ✔MEASURED (the row's own probe P5): feeding a PE `.dll` to `--resolve-library`
// on a Mach-O target already failed loud — rc=1, 50 × F_MangleMissingExpectedPrefix.
// But it failed through the `_`-DECORATION gate, not through any format check:
// Mach-O demands a leading underscore and PE exports carry none, so the two
// formats disagreed about SPELLING and the mismatch was caught as a side effect.
// ELF and PE are BOTH UNDECORATED, so that accident covers neither direction of
// the elf↔pe pair: the export names match verbatim, every extern binds, and the
// artifact records a DT_NEEDED / import-descriptor naming a library of the wrong
// format — a LOAD-time death with no build-time signal, the exact silent shape
// this project converts to compile-time errors everywhere else.
//
// ── WHY IT IS ONE FUNCTION AND NOT A CHECK AT EACH BINDER ───────────────────
// ✔MEASURED: TWO independent binders reach `ffi::readImports` — `ingest()`'s
// `readSource` (the C/HIR path) and `bindAsmExternImports` in
// `program/compile_pipeline.cpp` (the `encode`/`.s` path, which calls the reader
// DIRECTLY, with no `ingest()` in between). That surface has ALREADY paid for
// duplication once: the asm binder RESTATES `ingest()`'s ELF symbol-version
// policy clause for clause under a comment admitting it is a copy. A guard
// written twice is a guard that will be updated once, so this is the same
// remedy `recordedImportIdentity` above applies to the identity ranking — ONE
// function, two callers. Neither binder may call `ffi::readImports` for a
// `--resolve-library` input; both call this.
//
// ── AGNOSTIC BY CONSTRUCTION ────────────────────────────────────────────────
// Both sides of the comparison are DECLARED DATA in closed vocabularies. The
// left is `ffi::guessFormat` — the FF1 dispatcher's own magic-byte classifier,
// the facility the row observed "the reader ALREADY knows the format it
// detected". The right is `ObjectFormatSchema::kind()`, resolved from the
// format JSON's backend. Nothing here compares a format NAME, an extension, or
// a target string; adding a fourth object format adds one row to one table.
//
// Behaviour, in order:
//   * The file's magic classifies to an object-format kind that DIFFERS from
//     `format.kind()` → REJECT LOUD (`F_UnsupportedBinaryFormat`, the code the
//     ELF/COFF/Mach-O relocatable readers already use for "this input is not
//     the format this consumer needs"), naming the path, what the file IS, and
//     what the target needs. Nothing is read.
//   * Kinds agree, or the magic classifies to no single object format (an `ar`
//     CONTAINER, or unrecognised bytes) → delegate verbatim to `readImports`,
//     which keeps every existing failure mode and message intact (FileEmpty,
//     UnknownFormat, the Mach-O FAT `lipo -thin` remediation, CorruptedBinary…).
//     An `ar` archive declares no object format of its OWN — its MEMBERS do —
//     and the static-archive path checks them one tier down where the member
//     bytes are in hand (`elf/pe/macho::readRelocatableObject` reject a foreign
//     member's magic loud); pre-empting that here with a container-level guess
//     would be the name-matching this function exists to avoid.
//   * The file cannot be opened / is empty → indistinguishable from
//     "unclassifiable" here, so it also delegates and `readImports` reports
//     F_FileOpenFailed / F_FileEmpty exactly as before. Fail-closed is not the
//     right posture for a probe whose failure is already loudly handled two
//     lines later.
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, BinaryReadError>
readImportsForTargetFormat(std::filesystem::path const& libraryPath,
                           ObjectFormatSchema const&    format,
                           DiagnosticReporter&          reporter);

// The COMPARISON on its own, extracted so that the sentence above has exactly
// ONE author no matter how many places need to make it.
//
// `nullopt` = NO OBJECTION: the formats agree, or the leading bytes name no
// single object format (an `ar` container, unrecognised bytes, an unopenable or
// empty file — all deferred, see above). Engaged = REJECTED, with
// `F_UnsupportedBinaryFormat` ALREADY REPORTED; the returned `BinaryReadError`
// carries the same detail for a caller that propagates a structured error.
//
// ★ WHY THIS IS SEPARATE FROM THE READ, rather than the read being the only
// door. `compile_pipeline`'s step 2.5-pre runs an EAGER, UNCONDITIONAL probe
// over every `--resolve-library` entry, precisely because the reader is reached
// only through `ingest()` and a TU with no binary-governed externs never routes
// there — the flag would otherwise be validated only by luck of what the TU
// happens to reference. That probe already fails a MISSING path loud on exactly
// that argument; a library of the WRONG FORMAT is the same species of operator
// (or, since AP6's `setResolveLibraryAdditionsByTarget`, MACHINE) mistake, and
// it is knowable from 8 bytes without reading the library at all. So the probe
// asks this function, and the check keeps one owner instead of two.
[[nodiscard]] DSS_EXPORT std::optional<BinaryReadError>
checkLibraryMatchesTargetFormat(std::filesystem::path const& libraryPath,
                                ObjectFormatSchema const&    format,
                                DiagnosticReporter&          reporter);

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
    // the language default is "msvcrt.dll"). A SINGLE string — already
    // resolved for the active object format by the caller (Model 3,
    // 2026-06-09): the upstream `HirExternRecord.libraryOverride` is a
    // per-format map, FOLDED to this string by compile_pipeline where the
    // format is in scope, so FF5 stays target-agnostic. Source-language
    // agnostic — any language whose lowerer populates the map gets routing.
    std::string_view libraryOverride{};
    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): TRUE ⇒ this extern is a
    // bare-prototype cross-TU reference that must carry NO import library —
    // the synthesize stage leaves `FfiMetadata.importLibrary` EMPTY instead of
    // falling back to the format-level default (that fallback is exactly what
    // this flag opts out of) and stamps `FfiMetadata.noLibraryBinding` so the
    // HIR→MIR extern pre-pass admits the empty library. Resolution then
    // belongs to the link tier: a sibling-TU definition, or a LOUD undefined-
    // symbol reject. Mutually exclusive with a non-empty `libraryOverride`.
    bool noLibraryBinding = false;
    // c156 (D-LK-ELF-SYMBOL-VERSIONING): the REQUIRED ELF symbol version
    // (e.g. "GLIBC_2.3"), already resolved for the active (arch, format) by the
    // descriptor reader. Empty ⇒ unversioned. A plain string (already resolved,
    // not a per-format map), threaded verbatim to FfiMetadata.version.
    std::string_view version{};
    // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: TRUE ⇒ an EAGER import — one the
    // linker's reference gate keeps even when no relocation references it.
    // Threaded verbatim to `FfiMetadata.isEagerImport` by the FFI
    // synthesize/ingest stages, which are CONDUITS and never decide it.
    // ⚠ NO PRODUCER REACHING THIS STAGE SETS IT SINCE P57. It used to be TRUE for
    // every shipped-descriptor import (producer C) — the retired eager-import law
    // [[D-FFI-DESCRIPTOR-EAGER-IMPORT]] — and descriptor rows are now non-eager
    // like producers A and B. The one remaining eager producer in the tree is the
    // SEH personality, which is minted at the MIR tier and never travels this
    // path. The field stays because EAGER is still a representable and meaningful
    // property: "referenced by something the reloc-based gate cannot see".
    // INVARIANT: isEagerImport ⟹ library-bound.
    bool isEagerImport = false;
    // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): the EXPLICIT assembler name
    // this extern was
    // declared with, threaded verbatim from `HirExternRecord.asmName`. EMPTY = none
    // ⇒ every downstream name is computed exactly as before this cycle. When set it
    // REPLACES the format's C mangling for `FfiMetadata.mangledName` (via
    // `ffi::linkNameFor`) — the import requests the labelled symbol, not the mangled
    // C identifier — AND becomes the un-decorated key the binary-match lookup uses,
    // because the library really exports the LABEL. PER-DECLARATOR, unlike
    // `libraryOverride`. LAST field, mirroring `HirExternRecord`, so existing
    // positional aggregate initializers (fixtures included) keep compiling and
    // default it to "".
    std::string_view asmName{};
    // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): the per-target LINK
    // BASE NAME a shipped-library descriptor declared for this extern, already
    // resolved for the active (arch, format) by the descriptor reader and
    // UNDECORATED — threaded verbatim from `HirExternRecord.linkName`. EMPTY =
    // none ⇒ every downstream name is computed exactly as before this cycle.
    //
    // When set it replaces the BASE that `ffi::linkNameFor` decorates (NOT the
    // decoration itself, which `asmName` above does): `linkName:"fstat$INODE64"`
    // yields `_fstat$INODE64` on Mach-O and `fstat$INODE64` on ELF, because the
    // leading underscore belongs to the FORMAT. It also becomes the un-decorated
    // key the binary-match lookup uses, for the same reason `asmName` does — the
    // library really exports the aliased name, so keying on the plain C
    // identifier would silently miss the row.
    //
    // LAST field, mirroring `HirExternRecord`, so existing positional aggregate
    // initializers (the ~30 fixtures in tests/ffi) keep compiling and default it
    // to "".
    std::string_view linkName{};
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
//     SILENTLY SKIPPED here (no FfiMetadata written) -- `ingest()`
//     is a bind MECHANISM. Its sole production caller
//     (compile_pipeline step 2.5, c162 / D-FF1-READER-CONSUMER)
//     inspects `ffiMap` after the call to see which externs bound to
//     a `--resolve-library` binary, then applies the VALIDATION
//     POLICY to the unmatched ones: a bare `extern puts;` (a real
//     system symbol, not #included) falls through to its
//     format-default library, while a genuine typo (in neither the
//     binaries nor any shipped descriptor) fails loud
//     `F_FfiResolveLibrarySymbolAbsent`. That descriptor-aware policy
//     lives in the caller, not here (`ingest()` has no shipped-
//     descriptor knowledge); a blanket fail-loud here would wrongly
//     reject a legitimate mixed program.
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
// Alphabetical-order is deterministic — makes test assertions
// stable across platforms (FF-latent substrate; no live production
// caller as of 2026-06-03).
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
// extern to the per-symbol library the ROW carries. No header /
// binary read is required.
//
// ★★ UCRT-P4 (Decision 1): THERE IS NO LONGER A FORMAT-LEVEL DEFAULT
// LIBRARY, AND THE `importLibrary` PARAMETER IS GONE WITH IT.
// A row's import library now comes from exactly one place — the row
// itself (`ExternDeclRef::libraryOverride`, which upstream fills from
// the PLATFORM's shipped-descriptor realization or from a source
// `extern "lib" …`). The former per-LANGUAGE `externLibraryByFormat`
// default was a GUESS standing in for the descriptor corpus and a
// SECOND OWNER of a fact the corpus already owns: it named ONE image
// for every symbol of every header, so a hand-written
// `extern int printf(const char*, ...);` bound the LEGACY pe CRT while
// the same program's `#include`d stdio surface was correctly realized
// as UCRT shims — two C runtimes, no diagnostic (MEASURED). A row with
// nothing to bind is now UNBOUND on purpose and resolves at the LINK
// tier (C23 5.1.1.2 phase 8), which is where every C toolchain reports
// an unresolved external.
//
// Used by `compileSingleUnit` for every source-declared extern. A
// future cycle layers header-driven validation back in via
// `ingest()` for languages that want compile-time signature
// validation against shipped headers (anchored
// `D-FFI-HEADER-VALIDATION-OPTIONAL`).
//
// Failure modes:
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
    TargetSchema const&            target,
    ObjectFormatSchema const&      format,
    HirFfiMap&                     ffiMap,
    DiagnosticReporter&            reporter);

} // namespace dss::ffi
