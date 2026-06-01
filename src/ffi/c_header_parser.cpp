#include "ffi/c_header_parser.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace dss::ffi {

std::string_view
headerReadErrorKindName(HeaderReadErrorKind k) noexcept {
    switch (k) {
        case HeaderReadErrorKind::FileOpenFailed:         return "FileOpenFailed";
        case HeaderReadErrorKind::HeaderParseFailed:      return "HeaderParseFailed";
        case HeaderReadErrorKind::HeaderHasFunctionBody:  return "HeaderHasFunctionBody";
        case HeaderReadErrorKind::HeaderHasNonExternDecl: return "HeaderHasNonExternDecl";
    }
    return "Unknown";
}

namespace {

// Map an `HeaderReadErrorKind` to the structured `DiagnosticCode::F_*`
// value. Same discipline as `binary_reader.cpp::toDiagnosticCode`:
// the kind enum is the function-return shape; the F_* code is the
// in-reporter shape that downstream policy (`--suppress`,
// `--warnings-as-errors`) consumes.
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
    }
    // Unreachable per the closed enum. Fall through with the
    // closest-fit code rather than `None` so a future enum-variant
    // addition without a switch update still produces a
    // policy-targetable diagnostic. (Mirrors binary_reader.cpp's
    // post-fold #2 silent-failure Q2 fix.)
    return DiagnosticCode::F_HeaderParseFailed;
}

static_assert(static_cast<std::uint8_t>(HeaderReadErrorKind::HeaderHasNonExternDecl) == 3u,
              "HeaderReadErrorKind grew without updating "
              "toDiagnosticCode — add a switch arm for the new variant.");

[[nodiscard]] HeaderReadError
emitAndReturn(HeaderReadErrorKind kind, std::string detail,
              DiagnosticReporter& reporter) {
    dss::report(reporter, toDiagnosticCode(kind),
                DiagnosticSeverity::Error, detail);
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
    return ss.str();
}

} // namespace

std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderFromText(std::string_view    text,
                    std::string_view    headerPathLabel,
                    std::string_view    importLibrary,
                    DiagnosticReporter& reporter) {
    // Empty `importLibrary` is rejected at the entry — see header comment.
    // A row whose linker cannot resolve to a runtime library is
    // silent-failure surface: `ingest()` would happily forward it, and
    // the linker would either silently drop it or fail loud with a
    // different downstream code that doesn't name the real bug
    // (caller forgot to specify the library).
    if (importLibrary.empty()) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::HeaderParseFailed,
            std::string{"FFI header reader requires a non-empty importLibrary "
                        "(header '"} + std::string{headerPathLabel} + "' has "
            "no owning library — caller must supply one, e.g. \"libc.so.6\").",
            reporter));
    }

    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::HeaderParseFailed,
            "FFI header reader could not load shipped c-subset grammar.",
            reporter));
    }

    DiagnosticReporter feSilent;  // local — c-subset frontend diagnostics
                                  // get folded back into `reporter` below
                                  // (after dispatch), so we can preserve
                                  // the FF2-specific ordering.
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::string{text}, std::string{headerPathLabel});
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu);

    // Drain semantic-phase diagnostics into the caller's reporter so
    // tokenize/parse/semantic errors surface with their original codes
    // (P_* / S_* etc.) — the FF2-layer F_* wraps the verdict, not the
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

    auto loweringResult = lowerToHir(model, feSilent);
    for (auto const& d : feSilent.all()) {
        reporter.report(d);
    }
    if (!loweringResult->ok) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::HeaderParseFailed,
            std::string{"CST→HIR lowering rejected header '"}
                + std::string{headerPathLabel} + "' — see preceding diagnostics.",
            reporter));
    }

    Hir const& hir = loweringResult->hir;
    std::vector<ImportSurface> rows;
    rows.reserve(hir.moduleDecls(hir.root()).size());

    for (HirNodeId d : hir.moduleDecls(hir.root())) {
        HirKind kind = hir.kind(d);
        switch (kind) {
            case HirKind::ExternFunction:
            case HirKind::ExternGlobal: {
                SymbolId sym = (kind == HirKind::ExternFunction)
                                 ? hir.externFunctionSymbol(d)
                                 : hir.externGlobalSymbol(d);
                SymbolRecord const* rec = model.recordFor(sym);
                if (rec == nullptr || rec->name.empty()) {
                    return std::unexpected(emitAndReturn(
                        HeaderReadErrorKind::HeaderParseFailed,
                        std::string{"extern decl in header '"}
                            + std::string{headerPathLabel}
                            + "' has no resolvable symbol name "
                            + "(internal semantic-phase invariant violated).",
                        reporter));
                }
                ImportSurface row{};
                row.mangledName = rec->name;
                row.libraryPath = std::string{importLibrary};
                row.kind        = (kind == HirKind::ExternFunction)
                                    ? SymbolKind::Function
                                    : SymbolKind::Object;
                row.visibility  = SymbolVisibility::Default;
                row.linkage     = SymbolLinkage::External;
                // cSignature stays nullopt — FF3 ABI catalog computes
                // the canonical signature string from the resolved
                // FnSig TypeId; FF2 carries the structured semantic
                // result via the HIR side-table, NOT a duplicate
                // free-form string. (Captures the "single source of
                // truth" rule for the signature: the lattice TypeId.)
                rows.push_back(std::move(row));
                break;
            }
            case HirKind::TypeDecl:
                // typedef contributes to type resolution only — no
                // ImportSurface row. The c-subset frontend has already
                // bound the typedef name to a TypeId; downstream
                // consumers of the header's externs see resolved
                // FnSigs with the typedef-introduced names already
                // dereferenced.
                break;
            case HirKind::Function:
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::HeaderHasFunctionBody,
                    std::string{"header '"} + std::string{headerPathLabel}
                        + "' contains a non-extern function DEFINITION "
                        + "with a body — headers in FF2 v1 are "
                        + "declaration-only. Move the definition to a "
                        + ".c translation unit or convert it to an "
                        + "`extern` declaration.",
                    reporter));
            case HirKind::Global:
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::HeaderHasNonExternDecl,
                    std::string{"header '"} + std::string{headerPathLabel}
                        + "' contains a non-extern global variable "
                        + "definition — headers in FF2 v1 declare "
                        + "externs only (no tentative definitions).",
                    reporter));
            default:
                // Any other top-level decl (ImportGroup, Error, etc.)
                // is unsupported in header mode. Fail loud rather than
                // silently dropping the surface.
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::HeaderHasNonExternDecl,
                    std::string{"header '"} + std::string{headerPathLabel}
                        + "' contains an unsupported top-level declaration.",
                    reporter));
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

} // namespace dss::ffi
