#include "ffi/ingest.hpp"

#include "core/substrate/path_identity.hpp"  // genericSpelling
#include "core/types/parse_diagnostic.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "ffi/binary_reader.hpp"
// `guessFormat` + `emitAndReturn`: the format-mismatch guard is the FF1
// classifier's ONLY target-aware consumer, so it reuses the dispatcher's own
// magic-byte table and the reader tier's one kind->F_* emit path rather than
// re-stating either (D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL).
#include "ffi/binary_readers/reader_common.hpp"
#include "ffi/mangling/c_mangle.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/hir_node.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::ffi {

// The recorded-import-identity ranking. Doc + rationale live on the declaration
// in `ingest.hpp`; the two callers are `ingest()` below (the C/HIR binder) and
// the `encode` tier's `.s` extern binder in `program/compile_pipeline.cpp`.
std::string recordedImportIdentity(std::string_view declaredImportName,
                                   std::string_view embeddedSoname,
                                   std::string_view readerLibraryPath) {
    if (!declaredImportName.empty()) return std::string{declaredImportName};
    if (!embeddedSoname.empty())     return std::string{embeddedSoname};
    return std::string{readerLibraryPath};
}

// ── The FormatGuess → ObjectFormatKind vocabulary map ───────────────────────
//
// A pure TRANSLATION between two closed enums, in the same shape as
// `toDiagnosticCode(BinaryReadErrorKind)` in `reader_common.hpp`: no arm does
// anything a different arm does not, so this is a TABLE, not a per-format
// policy branch. `nullopt` means "these bytes name no single object format",
// which is a real answer and not a failure — see the three sources of it below.
//
// It lives HERE rather than beside `guessFormat` on purpose. FF1 is target-BLIND
// by construction ("`readImports` carries no target", binary_reader.hpp), and
// `ObjectFormatKind` is the vocabulary of the tier that HAS a target; teaching
// the magic-byte classifier that vocabulary would be the first crack in the
// property that lets one reader serve every build.
//
// The two Mach-O variants map to `MachO` even though `readImports` refuses
// them: a universal `.dylib` handed to an ELF build is WRONGLY-FORMATTED first
// and unsliced second, and "this is a Mach-O binary, the target needs elf" is
// the message that gets the operator to the right file. On a Mach-O target the
// kinds agree, nothing is rejected here, and FF1's own `lipo -thin` /
// 32-bit-unsupported remediations reach the operator unchanged.
// ★★ RULED EXEMPT FROM THE IDENTITY-BRANCH VETO, 2026-08-28 (P44), WITH THE
// MEASUREMENT THAT MAKES IT A VERDICT RATHER THAN AN OPINION. Recorded HERE so
// the next reader does not re-open the question — `src/ffi/` is RULED SHARED
// SUBSTRATE in plan 23 §5.1, so five `ObjectFormatKind::` mentions in one
// function are exactly what an auditor should stop on.
//
// THE TEST THAT SETTLES IT IS *WHAT DOES THE VALUE DECIDE*, NEVER *WHERE DOES
// IT SIT* — the trap where a relocation's arithmetic gets substituted for its
// role. ✔MEASURED: this function has ONE consumer,
// `checkLibraryMatchesTargetFormat` a few lines below, and the whole use is
//
//     if (!detected.has_value() || *detected == format.kind()) return nullopt;
//
// — an EQUALITY between two values of one type, symmetric in both operands.
// There is no arm per format: one comparison, one message, and every format
// SPELLING in that message comes from `kObjectFormatKindTable` or from the
// schema, never a literal. Swap ELF for Mach-O on both sides and the behaviour
// is byte-identical. Nothing downstream is selected by WHICH kind this is.
//
// Contrast the four branches P44 deleted one tier over
// ([[D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES]]): each of those read the
// identity and CHOSE — a different extension, a different `ar` layout, a
// different reader. That is deciding. This is naming.
//
// ⓘ Adding a sixth object format does require a row here. That is not the
// defect either: it is a translation between two CLOSED SETS, backed by
// `-Werror=switch`, so an omission is a BUILD ERROR rather than a silent gap —
// the same discipline as `kObjectFormatKindTable` itself. What the veto
// forbids is an engine choosing behaviour from identity, not a classifier
// reporting what it found in the vocabulary the next tier speaks.
[[nodiscard]] static std::optional<ObjectFormatKind>
objectFormatKindOfGuess(FormatGuess g) noexcept {
    switch (g) {
        case FormatGuess::Elf:      return ObjectFormatKind::Elf;
        case FormatGuess::Pe:       return ObjectFormatKind::Pe;
        case FormatGuess::MachO64:  return ObjectFormatKind::MachO;
        case FormatGuess::MachOFat: return ObjectFormatKind::MachO;
        case FormatGuess::MachO32:  return ObjectFormatKind::MachO;
        // An `ar` archive is a CONTAINER: the members carry the object format,
        // the wrapper carries none. Classifying it from the global magic would
        // be a guess, and the member-level check already exists one tier down.
        case FormatGuess::Ar:       return std::nullopt;
        // Unrecognised bytes -- `readImportsFromBytes` owns that message (it
        // enumerates what IS recognised, which this function cannot).
        case FormatGuess::Unknown:  return std::nullopt;
    }
    return std::nullopt;
}

// Classify the file WITHOUT reading it and WITHOUT reporting anything: the
// leading bytes are all `guessFormat` consumes (8 for `ar`, 4 for ELF/Mach-O,
// 2 for PE). Every failure -- cannot open, short, empty -- returns `nullopt`
// and defers to `readImports`, which opens the same path two lines later and
// reports F_FileOpenFailed / F_FileEmpty with its own wording. A SILENT probe
// whose failures are handled loud by the very next call is exactly the contract
// `isArArchiveFile` (program/compile_pipeline.cpp) already runs on this same
// flag's inputs; duplicating the open-failure diagnostic here would double-
// report one broken path.
[[nodiscard]] static std::optional<ObjectFormatKind>
peekObjectFormatKind(std::filesystem::path const& libraryPath) {
    std::ifstream in(libraryPath, std::ios::binary);
    if (!in) return std::nullopt;
    std::uint8_t buf[8] = {};
    in.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(sizeof(buf)));
    auto const got = static_cast<std::size_t>(in.gcount());
    if (got == 0u) return std::nullopt;
    return objectFormatKindOfGuess(
        guessFormat(std::span<std::uint8_t const>{buf, got}));
}

// ★ THE COMPARISON ITSELF — the SINGLE owner of "is this library's format the
// one this build emits", and of the sentence that says it isn't. Doc + rationale
// on the declaration in `ingest.hpp`.
std::optional<BinaryReadError>
checkLibraryMatchesTargetFormat(std::filesystem::path const& libraryPath,
                                ObjectFormatSchema const&    format,
                                DiagnosticReporter&          reporter) {
    auto const detected = peekObjectFormatKind(libraryPath);
    if (!detected.has_value() || *detected == format.kind()) return std::nullopt;
    // Message shape mirrors `elf::readRelocatableObject`'s wrong-schema refusal
    // (link/format/elf_object_reader.cpp): name the input, name what it IS, name
    // what was needed, and say what to do. Every format spelling comes from the
    // closed `kObjectFormatKindTable` or from the schema -- never a literal.
    return emitAndReturn(
        BinaryReadErrorKind::UnsupportedFormat,
        std::format(
            "--resolve-library '{0}': this file's object format is {1}, but the "
            "build emits object format '{2}' (kind {3}) -- a {1} library cannot "
            "satisfy a {3} link, and binding its exports would record an import "
            "the loader can never resolve. Point --resolve-library at the {3} "
            "build of this library, or build for a {1} target.",
            core::genericSpelling(libraryPath),
            objectFormatKindName(*detected),
            format.name(),
            objectFormatKindName(format.kind())),
        reporter);
}

// The target-aware read. Doc + rationale live on the declaration in
// `ingest.hpp`; the two callers are `readSource` below (the C/HIR binder) and
// `bindAsmExternImports` in `program/compile_pipeline.cpp` (the encode tier) --
// the same two-binder shape, and the same one-function remedy, as
// `recordedImportIdentity` above.
std::expected<std::vector<ImportSurface>, BinaryReadError>
readImportsForTargetFormat(std::filesystem::path const& libraryPath,
                           ObjectFormatSchema const&    format,
                           DiagnosticReporter&          reporter) {
    if (auto rejected =
            checkLibraryMatchesTargetFormat(libraryPath, format, reporter)) {
        return std::unexpected(std::move(*rejected));
    }
    return readImports(libraryPath, reporter);
}

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
//
// `format` is threaded in for the BinaryLibrarySource arm alone: a binary
// source is the one shape whose CONTENT can disagree with the target's object
// format, and `readImportsForTargetFormat` is the shared chokepoint that says
// so (D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL). The header arms
// are unaffected -- a `.h` declares no object format, so there is nothing to
// compare and no arm here branches on `format` itself.
[[nodiscard]] std::vector<ImportSurface>
readSource(IngestionSource const& src, ObjectFormatSchema const& format,
           DiagnosticReporter& reporter, bool& outFailed) {
    return std::visit(
        [&](auto const& s) -> std::vector<ImportSurface> {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, BinaryLibrarySource>) {
                auto r = readImportsForTargetFormat(s.path, format, reporter);
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
toCanonicalName(ImportSurface const& row, CSymbolDecorationScheme scheme,
                bool fromBinary, DiagnosticReporter& reporter) {
    if (!fromBinary) {
        // FF2 header-parser rows are already canonical by design.
        return row.mangledName;
    }
    auto canonical = unapplyCManglingStrict(row.mangledName, scheme, reporter);
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
                + core::genericSpelling(headerDir)
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
        // D-FFI-DECLARED-IMPORT-NAME: the caller-STATED runtime identity of the
        // SOURCE this row came from (`BinaryLibrarySource::declaredImportName`;
        // empty == not stated). Carried per-row because the precedence is
        // decided per-EXTERN below, where only the matched row is in scope --
        // the source it came from is no longer reachable there.
        std::string declaredImportName;
    };
    std::vector<TaggedRow> aggregated;

    for (auto const& src : sources) {
        bool failed = false;
        auto rows = readSource(src, format, reporter, failed);
        if (failed) return returnWithSnapshot();
        bool const fromBinary =
            std::holds_alternative<BinaryLibrarySource>(src);
        // c162 (D-FF1-READER-CONSUMER): a BinaryLibrarySource carrying a
        // non-empty `importName` OVERRIDES the reader's path-derived
        // `libraryPath` on every row it produced, so the resolved extern's
        // import records the loader-resolvable soname/DLL-name (the file's
        // basename) rather than the absolute build-time path. Empty leaves
        // the reader's label intact (the pre-c162 header/JSON behavior).
        // This is precedence LEVEL 3 -- levels 1 + 2 are ranked over it at
        // the per-extern decision site below.
        std::string declaredImportName;  // D-FFI-DECLARED-IMPORT-NAME (level 1)
        if (fromBinary) {
            auto const& bin = std::get<BinaryLibrarySource>(src);
            if (!bin.importName.empty()) {
                for (auto& r : rows) r.libraryPath = bin.importName;
            }
            declaredImportName = bin.declaredImportName;
        }
        aggregated.reserve(aggregated.size() + rows.size());
        for (auto& r : rows) {
            aggregated.push_back({std::move(r), fromBinary, declaredImportName});
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
            tagged.row, format.cSymbolDecoration().scheme, tagged.fromBinary,
            reporter);
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
        // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): the LIBRARY-MATCH key is
        // the extern's
        // ON-BINARY name un-decorated, not its C identifier. Without a label
        // `unapplyCMangling(applyCMangling(name))` is `name` on every format, so
        // this is byte-identical to the previous `bySymbol.find(canonicalName)`;
        // WITH one it is the label's canonical form, which is the only key that can
        // match. The library really exports the LABEL — `open` declared
        // `__DARWIN_ALIAS_C(open)` is `_open…` on disk — so keying on the C
        // identifier would silently miss the row and drop the extern through to the
        // format-default library with no diagnostic.
        // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): the descriptor's
        // per-target link BASE name rides the same call for the same reason —
        // libSystem's x86_64 slice exports `_fstat$INODE64`, so that (un-
        // decorated: `fstat$INODE64`) is the only key that can match its row.
        std::string const linkerName =
            linkNameFor(ext.canonicalName, ext.asmName,
                        format.cSymbolDecoration().scheme, ext.linkName);
        auto it = bySymbol.find(
            unapplyCMangling(linkerName, format.cSymbolDecoration().scheme));
        if (it == bySymbol.end()) continue;  // unmatched -> caller applies policy
        TaggedRow const& matched = *it->second;

        FfiMetadata meta{};
        meta.mangledName   = linkerName;
        meta.linkage       = toFfiLinkage(matched.row.linkage);
        meta.visibility    = toFfiVisibility(matched.row.visibility);
        // ★ THE RECORDED-IMPORT-IDENTITY DECISION SITE ★ — the ONE place the
        // three levels documented on `BinaryLibrarySource` (ingest.hpp) are
        // ranked. The linker's DT_NEEDED / LC_LOAD_DYLIB / PE import-descriptor
        // name is emitted from `ExternImport.libraryPath` == this field, so
        // this expression IS the artifact's runtime dependency.
        //
        //   1. D-FFI-DECLARED-IMPORT-NAME — the caller STATED the identity.
        //      Beats everything: the file we READ may be a cross-compilation
        //      STAND-IN whose own embedded identity names a path that will not
        //      exist on the target (a MacPorts `/opt/local/...` LC_ID_DYLIB
        //      read on a Windows host). Symbols from the file, identity from
        //      the declaration -- the sysroot-stub / `.tbd` / `-dylib_file`
        //      contract. Empty == not stated, so it falls through.
        //   2. D-FF1-READER-SONAME (c171) — the binary's OWN embedded identity
        //      (ELF DT_SONAME / Mach-O LC_ID_DYLIB install name / PE export
        //      DllName, all normalised into `row.soname` by the FF1 readers).
        //      Exactly what a real linker records when it is handed the real
        //      library, so it is right whenever no declaration overrides it.
        //   3. the reader's `libraryPath` label — the c162 driver-supplied
        //      basename stand-in (`BinaryLibrarySource::importName`), for a
        //      library that declares no soname at all (the `gcc -shared`
        //      no-`-soname` shape).
        //
        // FORMAT-BLIND: no arm of this branches on ObjectFormatKind -- the
        // readers already collapsed all three formats' embedded identities
        // into `row.soname`.
        // ⚠ THE RANKING ITSELF NOW LIVES IN `recordedImportIdentity` (ingest.hpp)
        // because the `encode` tier binds `.s` externs against the same
        // `--resolve-library` binaries without being able to come through this
        // function. Same decision, still ONE implementation.
        meta.importLibrary = recordedImportIdentity(matched.declaredImportName,
                                                    matched.row.soname,
                                                    matched.row.libraryPath);
        // ── THE REQUIRED-SYMBOL-VERSION DECISION SITE ────────────────────
        // c156 (D-LK-ELF-SYMBOL-VERSIONING) established the rail: a
        // non-empty version here becomes a `.gnu.version_r` requirement
        // against `meta.importLibrary` in the emitted ELF image, so ld.so
        // binds THAT version instead of an unversioned reference silently
        // landing on a library's OLDEST compat instance.
        //
        // TF-C124 (D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION) gives the
        // rail a SECOND source. Until it, only `ext.version` — a shipped
        // DESCRIPTOR's declaration — could ever be non-empty, so the whole
        // mechanism was unreachable for every library acquired without a
        // descriptor, which is every third-party library this project links
        // (Tcl, zlib). The binary itself records the answer; now we read it.
        //
        //   1. the DECLARATION (`ext.version`) — a descriptor pinned this
        //      symbol to an exact version for this (arch, format). Beats the
        //      observation for the same reason `declaredImportName` beats
        //      the binary's own soname above: a declaration is a statement
        //      about the RUNTIME library, an observation is a fact about the
        //      FILE WE READ, and those are allowed to differ.
        //   2. the OBSERVATION (`matched.row.elfSymbolVersion`) — what the
        //      library we read records for this export, under the two
        //      conditions below.
        //
        // FORMAT-BLIND: no arm asks what object format is active. PE and
        // Mach-O rows simply carry no `elfSymbolVersion` (their formats have
        // no per-symbol version — see `ffi/import_surface.hpp`), so this
        // expression yields the same empty string there that it always did.
        meta.version = std::string{ext.version};
        if (meta.version.empty()) {
            auto const& obs = matched.row.elfSymbolVersion;
            // (a) DEFAULT VERSIONS ONLY. A `sym@VER` compat definition is
            //     kept alive only for binaries that were linked before the
            //     default moved; re-requesting one because we happened to
            //     walk past it in `.dynsym` would MANUFACTURE the exact bug
            //     D-LK-ELF-SYMBOL-VERSIONING was opened for — libc.so.6
            //     exports both `realpath@@GLIBC_2.3` and the NULL-buffer-
            //     rejecting `realpath@GLIBC_2.2.5`, and which one a
            //     first-source-wins map keeps is a fact about `.dynsym`
            //     ORDER. Leaving a compat row unversioned preserves today's
            //     behaviour exactly: the reference binds the default.
            // (b) ONLY ABOUT THE FILE WE ACTUALLY READ. `meta.importLibrary`
            //     above may be a caller's DECLARED identity that deliberately
            //     differs from the binary on disk — the cross-compilation
            //     stand-in / `.tbd` contract (D-FFI-DECLARED-IMPORT-NAME).
            //     The verneed we emit names `importLibrary`, so requesting a
            //     version we saw in a DIFFERENT file would demand it of a
            //     library whose version set we never observed, turning a
            //     working link into a load-time failure. When the declared
            //     identity agrees with what the file says about itself (or
            //     there was no declaration), the file IS the library named
            //     and its versions are ours to request.
            if (obs.has_value() && obs->isDefaultVersion) {
                std::string const& observedIdentity =
                    matched.row.soname.empty() ? matched.row.libraryPath
                                               : matched.row.soname;
                if (meta.importLibrary == observedIdentity) {
                    meta.version = obs->name;
                }
            }
        }
        // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: carry the eager marker (parity
        // with the FF5 source-decl path). Eager imports flow through
        // `synthesizeFfiFromSourceDecls`, not this binary-reader path — but an
        // eager ExternDeclRef routed here must not silently drop the field.
        meta.isEagerImport = ext.isEagerImport;
        // The raw OBSERVED embedded soname of the binary that was READ.
        // Populated now that the FF1 readers extract it; `ExternImport` carries
        // no separate soname yet, so DT_NEEDED rides `importLibrary` above (a
        // distinct ExternImport.soname path is the future refinement, not
        // needed for the runtime-correct dependency).
        //
        // NOT re-pointed by a level-1 declaration (D-FFI-DECLARED-IMPORT-NAME):
        // this field answers "what did the file we read declare about itself",
        // `importLibrary` answers "what identity do we RECORD". When a caller
        // states an identity the two legitimately differ (that is the whole
        // point of a stand-in binary), and forging this one to match would
        // destroy the only evidence of which file was actually read. Consumed
        // today only by the HIR text dump (`hir_text.cpp`).
        meta.soname = matched.row.soname;

        ffiMap.set(ext.node, std::move(meta));
        ++result.externsAnnotated;
    }

    return returnWithSnapshot();
}

HirIngestResult
synthesizeFfiFromSourceDecls(
    std::span<ExternDeclRef const> externs,
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

    // (2) ── RETIRED: THE FORMAT-LEVEL LIBRARY-IDENTITY GATE ──────────────
    //
    // This step used to REJECT the whole module when the active language
    // declared no `externLibraryByFormat` entry for `format.kind()`, and its
    // advice text told the operator to add one (naming a specific legacy pe
    // CRT). Both the field and the gate are gone (UCRT-P4, Decision 1).
    //
    // WHY THE GATE CANNOT SURVIVE THE FIELD: it asserted "a language MUST name
    // one runtime image per object format". That is not a fact about a
    // LANGUAGE — it is a fact about a PLATFORM, and the shipped-descriptor
    // corpus already owns it PER SYMBOL (`stdio.json` is UCRT while
    // `setjmp.json` is deliberately not; `math.json` is `libm.so.6` on elf
    // while the rest of libc is `libc.so.6`). A single per-language string
    // could not express that, so it was a GUESS, and the guess is what bound a
    // hand-written `extern int printf(const char*, ...);` to the wrong C
    // runtime while the `#include`d siblings were realized correctly.
    //
    // WHAT REPLACED IT: a row with no library is UNBOUND on purpose
    // (`noLibraryBinding`), and C23 5.1.1.2 phase 8 resolves it at LINK — a
    // sibling TU's definition, a `--resolve-library` export, or a LOUD
    // K_SymbolUndefined. So "no library for this extern" is no longer an error
    // condition at all; it is a routing outcome. `F_FfiNoImportLibraryForFormat`
    // is retained in the diagnostic enum for name stability (the
    // `F_FfiResolveLibrarySymbolAbsent` precedent) and is no longer emitted.
    //
    // (3) Per-extern: validate non-empty canonical, apply FF4
    // C-mangling, write FfiMetadata. No surface match required —
    // the source's `extern` declaration IS the authoritative
    // signature (already in HIR as a FnSig on the ExternFunction
    // node). The linker will fail loud at the loader stage with
    // K_SymbolUndefined if the runtime library doesn't actually
    // export the symbol; that's the correct surface for "library
    // missing the symbol".
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
        // TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): `linkNameFor` returns an
        // explicit
        // assembler name VERBATIM and falls back to `applyCMangling` when there is
        // none — byte-identical for every extern declared without a label. This is
        // the IMPORT rail's single naming point; it MUST agree with the definition
        // rail (`program.cpp`'s merge-key lambda), which routes through the same
        // function, or a labelled definition and a labelled reference to it stop
        // collapsing at merge time and the call is emitted as a dynamic import.
        // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): `linkName` is the
        // fourth input to the SAME single naming point — the descriptor-declared
        // base name for this target, decorated by the format's rule (so the
        // definition rail's `nameOf`, which passes the SymbolRecord's copy of the
        // identical string, produces the identical bytes).
        meta.mangledName   = linkNameFor(ext.canonicalName, ext.asmName,
                                         format.cSymbolDecoration().scheme,
                                         ext.linkName);
        meta.linkage       = FfiLinkage::Strong;
        meta.visibility    = FfiVisibility::Default;
        // D-CSUBSET-EXTERN-LIBRARY-SYNTAX closure (step 13.3) + UCRT-P4
        // (Decision 1): the ROW's per-symbol library is now the ONLY
        // source of an import library. It arrives from the PLATFORM's
        // shipped-descriptor realization (already folded to this
        // format's image upstream) or from a source `extern "lib" …`.
        // There is no format-level fallback left to fall back TO —
        // deleting it is what made a hand-written prototype and an
        // `#include`d one realize IDENTICALLY. Source-language
        // agnostic: any language whose lowerer populates the field
        // gets per-symbol routing with no substrate change.
        //
        // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): a
        // `noLibraryBinding` extern is UNBOUND on purpose — its
        // importLibrary stays EMPTY and the reference resolves at the
        // link tier (a sibling-TU definition, or a LOUD
        // undefined-symbol reject). The flag is stamped through so
        // the HIR→MIR extern pre-pass admits the empty library. The
        // two spellings of "nothing to bind" now agree by
        // construction: an empty override yields an empty library
        // either way, and the flag is what says it was DELIBERATE.
        meta.noLibraryBinding = ext.noLibraryBinding;
        if (!ext.noLibraryBinding)
            meta.importLibrary = std::string{ext.libraryOverride};
        // c156 (D-LK-ELF-SYMBOL-VERSIONING): the REQUIRED ELF symbol version,
        // already resolved for the active (arch, format) by the descriptor
        // reader. Rides to the MIR ExternImport → the ELF writer's
        // .gnu.version_r. Empty (the default) ⇒ unversioned.
        meta.version = std::string{ext.version};
        // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: carry the eager marker (producer
        // C shipped-descriptor imports) to the MIR ExternImport → the linker's
        // reference gate, which keeps an eager row even when unreferenced.
        meta.isEagerImport = ext.isEagerImport;
        // `soname` left empty — same convention as `ingest()`.

        ffiMap.set(ext.node, std::move(meta));
        ++result.externsAnnotated;
    }

    return returnWithSnapshot();
}

} // namespace dss::ffi
