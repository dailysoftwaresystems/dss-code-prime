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
// Returns the canonical name on success, empty string on failure
// (caller skips the row with a logged underlying diagnostic).
[[nodiscard]] std::string
toCanonicalName(ImportSurface const& row, ObjectFormatKind format,
                bool fromBinary, DiagnosticReporter& reporter) {
    if (!fromBinary) {
        // FF2 header-parser rows are already canonical by design.
        return row.mangledName;
    }
    auto canonical = unapplyCManglingStrict(row.mangledName, format, reporter);
    if (!canonical) {
        // Underlying diagnostic already emitted by strict unapply.
        return {};
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
    // Deterministic order: alphabetical by filename — round-trips
    // with the shipped `src/dss-config/ffi-headers/<lib>/*.h` layout
    // and makes test assertions stable across platforms.
    std::sort(headers.begin(), headers.end());

    std::vector<ImportSurface> aggregated;
    for (auto const& path : headers) {
        auto r = readCHeader(path, importLibrary, reporter);
        if (!r) return std::unexpected(std::move(r.error()));
        aggregated.insert(aggregated.end(),
                          std::make_move_iterator(r->begin()),
                          std::make_move_iterator(r->end()));
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

    // (1) FF3 cross-validation — fail loud if the (target, format)
    // tuple isn't representable by the catalog. The resolved cc
    // is not stored in FfiMetadata (CallConv lives on the FnSig
    // TypeId per post-FF3 design); this call is a structural
    // gate, not a data producer.
    {
        auto abi = resolveAbi(target, format, reporter);
        if (!abi) return result;
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
        if (failed) return result;
        bool const fromBinary =
            std::holds_alternative<BinaryLibrarySource>(src);
        aggregated.reserve(aggregated.size() + rows.size());
        for (auto& r : rows) {
            aggregated.push_back({std::move(r), fromBinary});
        }
        ++result.sourcesProcessed;
    }
    result.rowsAggregated = aggregated.size();

    // (3) Build a canonical-name → TaggedRow index for O(1) match.
    // First-source-wins on duplicates — emits an info-level
    // diagnostic for each shadowed row so audit logs capture the
    // shadowing, but doesn't fail (the FFI design treats
    // first-wins as deterministic per plan §4.2).
    std::unordered_map<std::string, TaggedRow const*> bySymbol;
    bySymbol.reserve(aggregated.size());
    for (auto const& tagged : aggregated) {
        std::string canonical = toCanonicalName(
            tagged.row, format.kind(), tagged.fromBinary, reporter);
        if (canonical.empty()) continue;  // strict-unapply already reported
        auto [it, inserted] = bySymbol.emplace(std::move(canonical), &tagged);
        if (!inserted) {
            dss::report(reporter, DiagnosticCode::F_HeaderParseFailed,
                        DiagnosticSeverity::Warning,
                        std::format("FFI ingest: duplicate symbol '{}' "
                                    "from source '{}' shadowed by earlier "
                                    "definition from '{}' (first-source-"
                                    "wins per FFI spec).",
                                    it->first, tagged.row.libraryPath,
                                    it->second->row.libraryPath));
        }
    }

    // (4) Walk the caller-supplied externs; populate FfiMetadata
    // for each match. Unmatched externs are silently skipped —
    // the downstream linker will fail loud with K_SymbolUndefined
    // if no other source provides them; FF5 surfacing them here
    // would be a false-positive (the user might have a different
    // ingestion source planned for a later cycle).
    for (auto const& ext : externs) {
        auto it = bySymbol.find(std::string{ext.canonicalName});
        if (it == bySymbol.end()) continue;
        TaggedRow const& matched = *it->second;

        // Apply FF4 to produce the linker-visible decorated name.
        std::string const linkerName =
            applyCMangling(ext.canonicalName, format.kind());

        FfiMetadata meta{};
        meta.mangledName   = linkerName;
        meta.linkage       = toFfiLinkage(matched.row.linkage);
        meta.visibility    = toFfiVisibility(matched.row.visibility);
        meta.importLibrary = matched.row.libraryPath;
        // `soname` left empty — FF1 doesn't yet populate
        // DT_SONAME / Mach-O install_name; that's a future
        // refinement of `ImportSurface`.

        ffiMap.set(ext.node, std::move(meta));
        ++result.externsAnnotated;
    }

    result.ok = (reporter.errorCount() == 0u);
    return result;
}

} // namespace dss::ffi
