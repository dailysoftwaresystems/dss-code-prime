#include "ffi/ingest.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "ffi/binary_reader.hpp"
#include "ffi/mangling/c_mangle.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/hir_node.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::ffi {

namespace {

// Linkage / visibility conversion from the FFI `ImportSurface`
// closed enums to the HIR-side `FfiLinkage` / `FfiVisibility` closed
// enums. Closed-table dispatch — each pair of enums is independently
// versioned; never collapse them into one (each tier's enum has its
// own lifecycle).
[[nodiscard]] constexpr FfiLinkage
toFfiLinkage(SymbolLinkage l) noexcept {
    switch (l) {
        case SymbolLinkage::External: return FfiLinkage::Strong;
        case SymbolLinkage::Weak:     return FfiLinkage::Weak;
        case SymbolLinkage::Local:    return FfiLinkage::Common;
    }
    return FfiLinkage::Strong;
}

[[nodiscard]] constexpr FfiVisibility
toFfiVisibility(SymbolVisibility v) noexcept {
    switch (v) {
        case SymbolVisibility::Default:   return FfiVisibility::Default;
        case SymbolVisibility::Hidden:    return FfiVisibility::Hidden;
        case SymbolVisibility::Protected: return FfiVisibility::Protected;
        case SymbolVisibility::Internal:  return FfiVisibility::Hidden;
    }
    return FfiVisibility::Default;
}

// Read a single IngestionSource into a list of ImportSurface rows.
// FF1 (binary readers) for BinaryLibrarySource; FF2 (C header
// parser) for CHeaderSource; FF2 + FF6 multi-file for CHeaderDirSource.
// Returns std::nullopt on hard failure (each path emits its own
// F_* diagnostic via the underlying reader).
[[nodiscard]] std::vector<ImportSurface>
readSource(IngestionSource const& src, DiagnosticReporter& reporter,
           bool& outFailed) {
    return std::visit(
        [&](auto const& s) -> std::vector<ImportSurface> {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, BinaryLibrarySource>) {
                auto r = readImports(s.path, reporter);
                if (!r) { outFailed = true; return {}; }
                return std::move(*r);
            } else if constexpr (std::is_same_v<T, CHeaderSource>) {
                auto r = readCHeader(s.path, s.importLibrary, reporter);
                if (!r) { outFailed = true; return {}; }
                return std::move(*r);
            } else if constexpr (std::is_same_v<T, CHeaderDirSource>) {
                auto r = readCHeaderDirectory(s.dir, s.importLibrary,
                                              reporter);
                if (!r) { outFailed = true; return {}; }
                return std::move(*r);
            }
            outFailed = true;
            return {};
        },
        src);
}

// For binary-reader rows (which may carry a decorated name on
// formats with leading-underscore mangling), recover the canonical
// C identifier by unapplying the per-format decoration.
//
// FF2 (header parser) rows are already canonical (FF2 emits names
// verbatim from C declarations); FF1 rows from formats without
// decoration (ELF / Wasm / etc.) are also canonical. Only Mach-O
// binary readers feed decorated names today. The strict-mode
// unapply rejects a Mach-O input lacking the expected `_` prefix
// loud — that's a structural anomaly worth surfacing.
//
// Returns optional<string> to disambiguate three cases:
//   * has_value() + non-empty       → use this canonical name
//   * has_value() + empty           → caller MUST treat as a
//                                     structural anomaly (caller-
//                                     side reject — post-fold #6
//                                     C1 fix; empty-key emplace
//                                     would silently shadow other
//                                     symbols)
//   * !has_value()                  → strict-unapply rejected the
//                                     binary input; underlying
//                                     F_MangleMissingExpectedPrefix
//                                     already in the reporter
[[nodiscard]] std::optional<std::string>
toCanonicalName(ImportSurface const& row, ObjectFormatKind format,
                bool fromBinary, DiagnosticReporter& reporter) {
    if (!fromBinary) {
        // FF2 header-parser rows are already canonical by design.
        return row.mangledName;
    }
    auto canonical = unapplyCManglingStrict(row.mangledName, format, reporter);
    if (!canonical) {
        // Underlying diagnostic already emitted by strict unapply.
        return std::nullopt;
    }
    return std::move(*canonical);
}

} // namespace

std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderDirectory(std::filesystem::path const& headerDir,
                     std::string_view             importLibrary,
                     DiagnosticReporter&          reporter) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(headerDir, ec)) {
        return std::unexpected(HeaderReadError{
            HeaderReadErrorKind::FileOpenFailed,
            std::string{"readCHeaderDirectory: not a directory: "}
                + headerDir.generic_string()
        });
    }
    if (importLibrary.empty()) {
        return std::unexpected(HeaderReadError{
            HeaderReadErrorKind::EmptyImportLibrary,
            "readCHeaderDirectory requires a non-empty importLibrary"
        });
    }
    std::vector<fs::path> headers;
    for (auto const& entry : fs::directory_iterator{headerDir, ec}) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".h") {
            headers.push_back(entry.path());
        }
    }
    // Deterministic order: alphabetical by filename — makes test
    // assertions stable across platforms when a directory-based
    // header library is fed to FF5 (no live production caller as of
    // 2026-06-03; FF-latent substrate).
    std::sort(headers.begin(), headers.end());

    std::vector<ImportSurface> aggregated;
    std::size_t failedFiles = 0;
    // post-fold #5 silent-failure H1: collect per-file failures
    // instead of halting on the first. A typo in `stdlib.h` should
    // NOT silently amputate `stdio.h`, `string.h`, etc. — the
    // operator needs to see every parse failure AND get the
    // partial surface for the files that did parse. Only fail
    // the whole directory read if EVERY file failed.
    std::optional<HeaderReadError> firstError;
    for (auto const& path : headers) {
        auto r = readCHeader(path, importLibrary, reporter);
        if (!r) {
            ++failedFiles;
            if (!firstError) firstError = std::move(r.error());
            continue;
        }
        aggregated.insert(aggregated.end(),
                          std::make_move_iterator(r->begin()),
                          std::make_move_iterator(r->end()));
    }
    if (!headers.empty() && failedFiles == headers.size()) {
        return std::unexpected(std::move(*firstError));
    }
    return aggregated;
}

HirIngestResult
ingest(std::span<IngestionSource const> sources,
       std::span<ExternDeclRef const>   externs,
       TargetSchema const&              target,
       ObjectFormatSchema const&        format,
       HirFfiMap&                       ffiMap,
       DiagnosticReporter&              reporter) {
    HirIngestResult result{};

    // Every return path — early or normal — funnels through this
    // helper so HirIngestResult's `errorCountAtReturn_` snapshot
    // semantics are uniform. Default-constructed `result` has
    // `errorCountAtReturn_ == nullopt` → `ok() == false`; only the
    // returnWithSnapshot path engages the optional. Together with
    // the friend-declaration on `ingest`, the population path is
    // structurally pinned: no other caller can construct an
    // ok()==true result.
    auto returnWithSnapshot = [&]() -> HirIngestResult {
        result.snapshotErrorCountOnce(reporter.errorCount());
        return result;
    };

    // (1) FF3 cross-validation — fail loud if the (target, format)
    // tuple isn't representable by the catalog. Post-fold-#5
    // silent-failure CRITICAL-2: also short-circuit on
    // operand-stack / result-id abi-models (cc=nullptr) — FF4's
    // C-mangling rules don't apply to WASM's import-namespace
    // dispatch or SPIR-V's resultId surface. Producing
    // FfiMetadata via FF4 for those targets would silently emit
    // wrong-shape metadata once plan 17/18 grows real ingestion
    // paths.
    {
        auto abi = resolveAbi(target, format, reporter);
        if (!abi) return returnWithSnapshot();
        if (abi->cc == nullptr) {
            // post-fold #6 silent-failure C2: dedicated code (not
            // `D_PlanNotLanded` reuse). The (operand-stack /
            // result-id) → no-FF4-C-mangling pairing is a
            // permanent architectural exclusion, NOT a pending-
            // arrival surface. plan 17 (SPIR-V) + plan 18 (WASM)
            // own their own ingest surfaces; FF5 will never apply.
            dss::report(reporter, DiagnosticCode::F_FfiIngestAbiModelUnsupported,
                        DiagnosticSeverity::Error,
                        std::format("FF5 ingest: target '{}' abiModel '{}' "
                                    "is not supported by the FF4 C-mangling "
                                    "path; SPIR-V (plan 17) and WASM "
                                    "(plan 18) own their own ingest surfaces.",
                                    target.name(),
                                    targetAbiModelName(target.abiModel())));
            return returnWithSnapshot();
        }
    }

    // (2) Aggregate ImportSurface rows from every source, tagged
    // by whether they came from a binary (FF1) or header (FF2/FF6)
    // — binary rows need FF4 unapply to recover canonical names.
    struct TaggedRow {
        ImportSurface row;
        bool fromBinary = false;
    };
    std::vector<TaggedRow> aggregated;

    for (auto const& src : sources) {
        bool failed = false;
        auto rows = readSource(src, reporter, failed);
        if (failed) return returnWithSnapshot();
        bool const fromBinary =
            std::holds_alternative<BinaryLibrarySource>(src);
        // c162 (D-FF1-READER-CONSUMER): a BinaryLibrarySource carrying a
        // non-empty `importName` OVERRIDES the reader's path-derived
        // `libraryPath` on every row it produced, so the resolved extern's
        // import records the loader-resolvable soname/DLL-name (the file's
        // basename) rather than the absolute build-time path. Empty leaves
        // the reader's label intact (the pre-c162 header/JSON behavior).
        if (fromBinary) {
            auto const& bin = std::get<BinaryLibrarySource>(src);
            if (!bin.importName.empty()) {
                for (auto& r : rows) r.libraryPath = bin.importName;
            }
        }
        aggregated.reserve(aggregated.size() + rows.size());
        for (auto& r : rows) {
            aggregated.push_back({std::move(r), fromBinary});
        }
        ++result.sourcesProcessed;
    }
    result.rowsAggregated = aggregated.size();

    // (3) Build a canonical-name → TaggedRow index for O(1) match.
    // First-source-wins on duplicates — emits a Warning-level
    // diagnostic for each shadowed row so audit logs capture the
    // shadowing, but doesn't fail (this is a local FF5 design
    // choice; downstream linkers reject true link-time symbol
    // collisions independently).
    std::unordered_map<std::string, TaggedRow const*> bySymbol;
    bySymbol.reserve(aggregated.size());
    for (auto const& tagged : aggregated) {
        auto canonical = toCanonicalName(
            tagged.row, format.kind(), tagged.fromBinary, reporter);
        if (!canonical) continue;  // strict-unapply already reported
        // post-fold #6 silent-failure C1: empty canonical name would
        // emplace `bySymbol[""]` and silently shadow every subsequent
        // empty-named row + silently match a `ExternDeclRef{node, ""}`
        // caller-side bug. Reject loud.
        if (canonical->empty()) {
            dss::report(reporter, DiagnosticCode::F_FfiIngestEmptyCanonical,
                        DiagnosticSeverity::Error,
                        std::format("FF5 ingest: source '{}' produced an "
                                    "empty canonical name from mangledName "
                                    "'{}' — structural anomaly, skipping row.",
                                    tagged.row.libraryPath,
                                    tagged.row.mangledName));
            continue;
        }
        auto [it, inserted] = bySymbol.emplace(std::move(*canonical), &tagged);
        if (!inserted) {
            // First-source-wins is the local design choice (an
            // operator who exposes the same symbol from two
            // libraries gets the first one); the linker would
            // reject a true link-time collision separately. Use
            // the dedicated F_FfiIngestDuplicateSymbol code (NOT
            // F_HeaderParseFailed — that's the per-file parse-
            // failure code; the cross-source duplicate is a
            // different remediation surface).
            dss::report(reporter, DiagnosticCode::F_FfiIngestDuplicateSymbol,
                        DiagnosticSeverity::Warning,
                        std::format("FFI ingest: duplicate symbol '{}' "
                                    "from source '{}' shadowed by earlier "
                                    "definition from '{}' (first-source-"
                                    "wins).",
                                    it->first, tagged.row.libraryPath,
                                    it->second->row.libraryPath));
        }
    }

    // (4) Walk the caller-supplied externs; BIND FfiMetadata for each
    // that MATCHES a row in the aggregated surface. An extern that
    // matches NO row is SILENTLY SKIPPED here -- `ingest()` is a bind
    // MECHANISM, not the policy owner. Its sole production caller
    // (compile_pipeline step 2.5, c162 / D-FF1-READER-CONSUMER) inspects
    // `ffiMap` AFTER this call to see which externs bound to a
    // `--resolve-library` binary, then applies the VALIDATION POLICY to
    // the unmatched ones: a bare `extern puts;` (a real system symbol the
    // user did not #include) falls through to its format-default library,
    // while a genuine typo (in neither the binaries nor any shipped
    // descriptor) fails loud. That policy needs shipped-descriptor
    // knowledge `ingest()` does not have -- keeping the skip silent HERE
    // and the fail-loud in the descriptor-aware caller is the clean split
    // (the alternative -- a blanket fail-loud in `ingest()` -- would
    // wrongly reject a legitimate `bare extern puts + --resolve-library
    // ownlib` program).
    for (auto const& ext : externs) {
        // post-fold #6 silent-failure C1: caller-side empty
        // canonicalName would match the empty-string key (if any
        // somehow slipped past the producer-side guard above) and
        // silently bind whatever the first empty-named row was.
        // Reject loud.
        if (ext.canonicalName.empty()) {
            dss::report(reporter, DiagnosticCode::F_FfiIngestEmptyCanonical,
                        DiagnosticSeverity::Error,
                        "FF5 ingest: caller-supplied ExternDeclRef has "
                        "empty canonicalName — structural anomaly, "
                        "skipping match.");
            continue;
        }
        auto it = bySymbol.find(std::string{ext.canonicalName});
        if (it == bySymbol.end()) continue;  // unmatched -> caller applies policy
        TaggedRow const& matched = *it->second;

        // Apply FF4 to produce the linker-visible decorated name.
        std::string const linkerName =
            applyCMangling(ext.canonicalName, format.kind());

        FfiMetadata meta{};
        meta.mangledName   = linkerName;
        meta.linkage       = toFfiLinkage(matched.row.linkage);
        meta.visibility    = toFfiVisibility(matched.row.visibility);
        meta.importLibrary = matched.row.libraryPath;
        // c156 (D-LK-ELF-SYMBOL-VERSIONING): carry the required symbol
        // version (parity with the FF5 source-decl path). Dormant today
        // (FF1 is the binary-reader ingestion path), but a versioned
        // ExternDeclRef routed here must not silently drop its version.
        meta.version = std::string{ext.version};
        // `soname` left empty — FF1 doesn't yet populate
        // DT_SONAME / Mach-O install_name; that's a future
        // refinement of `ImportSurface`.

        ffiMap.set(ext.node, std::move(meta));
        ++result.externsAnnotated;
    }

    return returnWithSnapshot();
}

HirIngestResult
synthesizeFfiFromSourceDecls(
    std::span<ExternDeclRef const> externs,
    std::string_view               importLibrary,
    TargetSchema const&            target,
    ObjectFormatSchema const&      format,
    HirFfiMap&                     ffiMap,
    DiagnosticReporter&            reporter) {
    HirIngestResult result{};

    auto returnWithSnapshot = [&]() -> HirIngestResult {
        result.snapshotErrorCountOnce(reporter.errorCount());
        return result;
    };

    // (1) FF3 cross-validation — same gate as `ingest()`. SPIR-V /
    // WASM (abiModel: operand-stack / result-id) reject loud: their
    // import surfaces aren't FF4-mangled. Plan 17/18 own those
    // paths.
    {
        auto abi = resolveAbi(target, format, reporter);
        if (!abi) return returnWithSnapshot();
        if (abi->cc == nullptr) {
            dss::report(reporter,
                        DiagnosticCode::F_FfiIngestAbiModelUnsupported,
                        DiagnosticSeverity::Error,
                        std::format("FF5 synthesizeFfiFromSourceDecls: "
                                    "target '{}' abiModel '{}' is not "
                                    "supported by the FF4 C-mangling "
                                    "path; SPIR-V (plan 17) and WASM "
                                    "(plan 18) own their own ingest "
                                    "surfaces.",
                                    target.name(),
                                    targetAbiModelName(target.abiModel())));
            return returnWithSnapshot();
        }
    }

    // (2) Per-format library identity must be configured. An empty
    // string means the active language's
    // `DeclarationRule.externLibraryByFormat` map has no entry for
    // `format.kind()`. Fail loud upstream so the operator fixes
    // the language config rather than chasing a downstream
    // K_FormatLacksImportSupport.
    if (importLibrary.empty()) {
        dss::report(reporter,
                    DiagnosticCode::F_FfiNoImportLibraryForFormat,
                    DiagnosticSeverity::Error,
                    std::format("FF5 synthesizeFfiFromSourceDecls: the "
                                "active language declared no "
                                "`externLibraryByFormat` entry for "
                                "object format '{}'. Add a per-format "
                                "library name (e.g. \"pe\": "
                                "\"msvcrt.dll\") to the language's "
                                "semantics JSON so source-declared "
                                "externs can resolve to a runtime "
                                "library.",
                                objectFormatKindName(format.kind())));
        return returnWithSnapshot();
    }

    // (3) Per-extern: validate non-empty canonical, apply FF4
    // C-mangling, write FfiMetadata. No surface match required —
    // the source's `extern` declaration IS the authoritative
    // signature (already in HIR as a FnSig on the ExternFunction
    // node). The linker will fail loud at the loader stage with
    // K_SymbolUndefined if the runtime library doesn't actually
    // export the symbol; that's the correct surface for "library
    // missing the symbol" (different audience from "language
    // config missing the library").
    std::string const libCopy{importLibrary};
    for (auto const& ext : externs) {
        if (ext.canonicalName.empty()) {
            dss::report(reporter,
                        DiagnosticCode::F_FfiIngestEmptyCanonical,
                        DiagnosticSeverity::Error,
                        "FF5 synthesizeFfiFromSourceDecls: caller-"
                        "supplied ExternDeclRef has empty "
                        "canonicalName — structural anomaly, "
                        "skipping match.");
            continue;
        }

        FfiMetadata meta{};
        meta.mangledName   = applyCMangling(ext.canonicalName,
                                            format.kind());
        meta.linkage       = FfiLinkage::Strong;
        meta.visibility    = FfiVisibility::Default;
        // D-CSUBSET-EXTERN-LIBRARY-SYNTAX closure (step 13.3): a
        // per-symbol library override on the ExternDeclRef wins over
        // the format-level default. Empty override = use the
        // format-level fallback. Source-language agnostic — any
        // language whose lowerer populates the override gets
        // per-symbol routing without further substrate change.
        //
        // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): a
        // `noLibraryBinding` extern OPTS OUT of the format-default
        // fallback entirely — its importLibrary stays EMPTY on
        // purpose (a bare-prototype cross-TU reference resolves at
        // the link tier: a sibling-TU definition, or a LOUD
        // undefined-symbol reject). The flag is stamped through so
        // the HIR→MIR extern pre-pass admits the empty library.
        meta.noLibraryBinding = ext.noLibraryBinding;
        if (!ext.noLibraryBinding) {
            meta.importLibrary = ext.libraryOverride.empty()
                                     ? libCopy
                                     : std::string{ext.libraryOverride};
        }
        // c156 (D-LK-ELF-SYMBOL-VERSIONING): the REQUIRED ELF symbol version,
        // already resolved for the active (arch, format) by the descriptor
        // reader. Rides to the MIR ExternImport → the ELF writer's
        // .gnu.version_r. Empty (the default) ⇒ unversioned.
        meta.version = std::string{ext.version};
        // `soname` left empty — same convention as `ingest()`.

        ffiMap.set(ext.node, std::move(meta));
        ++result.externsAnnotated;
    }

    return returnWithSnapshot();
}

} // namespace dss::ffi
