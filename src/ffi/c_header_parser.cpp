#include "ffi/c_header_parser.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace dss::ffi {

std::string_view
headerReadErrorKindName(HeaderReadErrorKind k) noexcept {
    switch (k) {
        case HeaderReadErrorKind::FileOpenFailed:               return "FileOpenFailed";
        case HeaderReadErrorKind::HeaderParseFailed:            return "HeaderParseFailed";
        case HeaderReadErrorKind::HeaderHasFunctionBody:        return "HeaderHasFunctionBody";
        case HeaderReadErrorKind::HeaderHasNonExternDecl:       return "HeaderHasNonExternDecl";
        case HeaderReadErrorKind::EmptyImportLibrary:           return "EmptyImportLibrary";
        case HeaderReadErrorKind::GrammarLoadFailed:            return "GrammarLoadFailed";
        case HeaderReadErrorKind::HeaderHasUnsupportedTopLevel: return "HeaderHasUnsupportedTopLevel";
        case HeaderReadErrorKind::InternalInvariant:            return "InternalInvariant";
    }
    return "Unknown";
}

namespace {

[[nodiscard]] constexpr DiagnosticCode
toDiagnosticCode(HeaderReadErrorKind k) noexcept {
    switch (k) {
        case HeaderReadErrorKind::FileOpenFailed:
            return DiagnosticCode::F_FileOpenFailed;
        case HeaderReadErrorKind::HeaderParseFailed:
            return DiagnosticCode::F_HeaderParseFailed;
        case HeaderReadErrorKind::HeaderHasFunctionBody:
            return DiagnosticCode::F_HeaderHasFunctionBody;
        case HeaderReadErrorKind::HeaderHasNonExternDecl:
            return DiagnosticCode::F_HeaderHasNonExternDecl;
        case HeaderReadErrorKind::EmptyImportLibrary:
            return DiagnosticCode::F_HeaderEmptyImportLibrary;
        case HeaderReadErrorKind::GrammarLoadFailed:
            return DiagnosticCode::F_HeaderGrammarLoadFailed;
        case HeaderReadErrorKind::HeaderHasUnsupportedTopLevel:
            return DiagnosticCode::F_HeaderHasUnsupportedTopLevel;
        case HeaderReadErrorKind::InternalInvariant:
            return DiagnosticCode::F_HeaderInternalInvariant;
    }
    return DiagnosticCode::F_HeaderInternalInvariant;
}

static_assert(static_cast<std::uint8_t>(HeaderReadErrorKind::InternalInvariant) == 7u,
              "HeaderReadErrorKind grew without updating "
              "toDiagnosticCode — add a switch arm for the new variant.");

// Emit + return helper, optionally stamping a source location.
[[nodiscard]] HeaderReadError
emitAndReturn(HeaderReadErrorKind kind, std::string detail,
              DiagnosticReporter& reporter,
              HirSourceLoc const* loc = nullptr) {
    ParseDiagnostic p;
    p.code     = toDiagnosticCode(kind);
    p.severity = DiagnosticSeverity::Error;
    p.actual   = detail;
    if (loc != nullptr) {
        p.buffer = loc->buffer;
        p.span   = loc->span;
    }
    reporter.report(std::move(p));
    return HeaderReadError{kind, std::move(detail)};
}

[[nodiscard]] std::expected<std::string, HeaderReadError>
slurpFile(std::filesystem::path const& path, DiagnosticReporter& reporter) {
    std::ifstream in{path, std::ios::binary};
    if (!in.is_open()) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::FileOpenFailed,
            "FFI header file could not be opened: " + path.generic_string(),
            reporter));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    // post-FF2 silent-failure C2 fix: mid-read I/O failure after a
    // successful open leaves the partial prefix in `ss` and looks
    // like a successful (but truncated) read. The c-subset parser
    // would then either silently accept the prefix or emit a
    // misleading parse error. Surface as FileOpenFailed loud.
    if (in.bad()) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::FileOpenFailed,
            "FFI header file read I/O error after open: " + path.generic_string(),
            reporter));
    }
    return ss.str();
}

} // namespace

std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderFromText(std::string_view    text,
                    std::string_view    headerPathLabel,
                    std::string_view    importLibrary,
                    DiagnosticReporter& reporter) {
    // Caller-API gate: empty importLibrary would produce unlinkable
    // rows downstream — silent-failure surface unless rejected here.
    // Distinct kind + code (EmptyImportLibrary / F_HeaderEmptyImportLibrary)
    // from parse failures so `--suppress` on parse-noise doesn't
    // also hide caller-API misuse.
    if (importLibrary.empty()) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::EmptyImportLibrary,
            std::string{"FFI header reader requires a non-empty importLibrary "
                        "(header '"} + std::string{headerPathLabel} + "' has "
            "no owning library — caller must supply one, e.g. \"libc.so.6\").",
            reporter));
    }

    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) {
        // Forward the underlying C_* diagnostics so an operator sees
        // the actual config bug (bad JSON, missing field, etc.) not
        // only the FF2 wrap (silent-failure-hunter C1 fold).
        forwardConfigDiagnostics(loaded.error(), reporter);
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::GrammarLoadFailed,
            "FFI header reader could not load shipped c-subset grammar "
            "(see preceding C_* diagnostics).",
            reporter));
    }

    UnitBuilder builder{*loaded};
    builder.addInMemory(std::string{text}, std::string{headerPathLabel});
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu);

    // Drain the accumulated CU diagnostics (tokenize → parse →
    // semantic) into the caller's reporter so original P_*/L_*/S_*
    // codes surface — the FF2-layer F_* wraps the verdict, not the
    // underlying cause.
    for (auto const& d : model.diagnostics().all()) {
        reporter.report(d);
    }
    if (model.hasErrors()) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::HeaderParseFailed,
            std::string{"c-subset frontend rejected header '"}
                + std::string{headerPathLabel} + "' — see preceding diagnostics.",
            reporter));
    }

    auto loweringResult = lowerToHir(model, reporter);
    if (!loweringResult->ok) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::HeaderParseFailed,
            std::string{"CST→HIR lowering rejected header '"}
                + std::string{headerPathLabel} + "' — see preceding diagnostics.",
            reporter));
    }

    Hir const& hir = loweringResult->hir;
    HirSourceMap const& sourceMap = loweringResult->sourceMap;

    auto locFor = [&sourceMap](HirNodeId n) -> HirSourceLoc {
        if (auto const* loc = sourceMap.tryGet(n)) return *loc;
        return HirSourceLoc{};
    };

    std::vector<ImportSurface> rows;
    rows.reserve(hir.moduleDecls(hir.root()).size());

    for (HirNodeId d : hir.moduleDecls(hir.root())) {
        HirKind kind = hir.kind(d);
        HirSourceLoc loc = locFor(d);
        switch (kind) {
            case HirKind::ExternFunction:
            case HirKind::ExternGlobal: {
                SymbolId sym = (kind == HirKind::ExternFunction)
                                 ? hir.externFunctionSymbol(d)
                                 : hir.externGlobalSymbol(d);
                SymbolRecord const* rec = model.recordFor(sym);
                if (rec == nullptr || rec->name.empty()) {
                    return std::unexpected(emitAndReturn(
                        HeaderReadErrorKind::InternalInvariant,
                        std::string{"internal: extern decl in header '"}
                            + std::string{headerPathLabel}
                            + "' resolved to a symbol with no name — "
                              "file a bug report.",
                        reporter, &loc));
                }
                ImportSurface row{};
                row.mangledName = rec->name;
                row.libraryPath = std::string{importLibrary};
                row.kind        = (kind == HirKind::ExternFunction)
                                    ? SymbolKind::Function
                                    : SymbolKind::Object;
                row.visibility  = SymbolVisibility::Default;
                row.linkage     = SymbolLinkage::External;
                // cSignature left nullopt — FF3 (ABI catalog) attaches
                // the resolved FnSig via the HIR side-table.
                rows.push_back(std::move(row));
                break;
            }
            case HirKind::TypeDecl:
                // typedef / struct / union / enum: contributes to
                // type resolution, no surface row.
                break;
            case HirKind::Function:
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::HeaderHasFunctionBody,
                    std::string{"header '"} + std::string{headerPathLabel}
                        + "' contains a non-extern function DEFINITION "
                        + "with a body — headers in FF2 v1 are "
                        + "declaration-only.",
                    reporter, &loc));
            case HirKind::Global:
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::HeaderHasNonExternDecl,
                    std::string{"header '"} + std::string{headerPathLabel}
                        + "' contains a non-extern global variable "
                          "definition — headers in FF2 v1 declare "
                          "externs only.",
                    reporter, &loc));
            case HirKind::ImportGroup:
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::HeaderHasUnsupportedTopLevel,
                    std::string{"header '"} + std::string{headerPathLabel}
                        + "' contains an #include / import group — FF2 v1 "
                          "does not yet follow includes. Copy declarations "
                          "directly into the curated header, or anchor a "
                          "follow-up to extend the c-subset import resolver.",
                    reporter, &loc));
            case HirKind::Error:
                // Lowering's recovery sentinel. ok=true guard above
                // means the lowering didn't increment errorCount, but
                // a stray Error node here is still an invariant
                // violation worth surfacing distinctly.
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::InternalInvariant,
                    std::string{"internal: lowering produced an Error "
                                "recovery node at top level of header '"}
                        + std::string{headerPathLabel}
                        + "' despite ok=true — file a bug report.",
                    reporter, &loc));
            default:
                // Any other HirKind at module scope (`Module`,
                // statements, expressions, `Unreachable`, `Extension`,
                // future kinds) is structurally unexpected. Fail loud
                // with the distinct unsupported-top-level code so
                // future grammar evolution can't silently slip past.
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::HeaderHasUnsupportedTopLevel,
                    std::string{"header '"} + std::string{headerPathLabel}
                        + "' contains an unsupported top-level "
                          "declaration shape.",
                    reporter, &loc));
        }
    }
    return rows;
}

std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeader(std::filesystem::path const& headerPath,
            std::string_view             importLibrary,
            DiagnosticReporter&          reporter) {
    auto contents = slurpFile(headerPath, reporter);
    if (!contents) return std::unexpected(contents.error());
    return readCHeaderFromText(*contents, headerPath.generic_string(),
                                importLibrary, reporter);
}

std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderShipped(std::string_view    headerRelPath,
                   std::string_view    importLibrary,
                   DiagnosticReporter& reporter) {
    auto located = findShippedFfiHeader(headerRelPath);
    if (!located) {
        forwardConfigDiagnostics(located.error(), reporter);
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::FileOpenFailed,
            std::string{"shipped FFI header not found: "}
                + std::string{headerRelPath},
            reporter));
    }
    return readCHeader(*located, importLibrary, reporter);
}

} // namespace dss::ffi
