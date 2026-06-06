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

#include <array>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace dss::ffi {

namespace {

// Closed-table mapping `HeaderReadErrorKind` → name + F_* code,
// mirroring `kTargetArchMachineCodes` / `kRelocFormulaTable` from
// elsewhere in the codebase (post-FF2-#2 simplifier #2 fold).
// Replaces two parallel exhaustive switches over the same enum —
// add a new row and both `headerReadErrorKindName` +
// `toDiagnosticCode` pick it up; the static_assert below pins
// length so a forgotten row is a compile error.
struct HeaderReadErrorRow {
    HeaderReadErrorKind kind;
    std::string_view    name;
    DiagnosticCode      code;
};

constexpr std::array<HeaderReadErrorRow, 8> kHeaderReadErrorTable{{
    { HeaderReadErrorKind::FileOpenFailed,               "FileOpenFailed",               DiagnosticCode::F_FileOpenFailed               },
    { HeaderReadErrorKind::HeaderParseFailed,            "HeaderParseFailed",            DiagnosticCode::F_HeaderParseFailed            },
    { HeaderReadErrorKind::HeaderHasFunctionBody,        "HeaderHasFunctionBody",        DiagnosticCode::F_HeaderHasFunctionBody        },
    { HeaderReadErrorKind::HeaderHasNonExternDecl,       "HeaderHasNonExternDecl",       DiagnosticCode::F_HeaderHasNonExternDecl       },
    { HeaderReadErrorKind::EmptyImportLibrary,           "EmptyImportLibrary",           DiagnosticCode::F_HeaderEmptyImportLibrary     },
    { HeaderReadErrorKind::GrammarLoadFailed,            "GrammarLoadFailed",            DiagnosticCode::F_HeaderGrammarLoadFailed      },
    { HeaderReadErrorKind::HeaderHasUnsupportedTopLevel, "HeaderHasUnsupportedTopLevel", DiagnosticCode::F_HeaderHasUnsupportedTopLevel },
    { HeaderReadErrorKind::InternalInvariant,            "InternalInvariant",            DiagnosticCode::F_HeaderInternalInvariant      },
}};

static_assert(static_cast<std::uint8_t>(HeaderReadErrorKind::InternalInvariant) + 1u
                  == kHeaderReadErrorTable.size(),
              "kHeaderReadErrorTable must hold one row per HeaderReadErrorKind "
              "variant — add a row when adding a variant.");

consteval bool kHeaderReadErrorTableRowsAlignWithKind() {
    for (std::size_t i = 0; i < kHeaderReadErrorTable.size(); ++i) {
        if (static_cast<std::size_t>(kHeaderReadErrorTable[i].kind) != i) return false;
    }
    return true;
}
static_assert(kHeaderReadErrorTableRowsAlignWithKind(),
              "kHeaderReadErrorTable row order must match the "
              "HeaderReadErrorKind underlying values — a paste-error "
              "row in the wrong slot would otherwise silently map "
              "the wrong code to the wrong kind.");

[[nodiscard]] constexpr DiagnosticCode
toDiagnosticCode(HeaderReadErrorKind k) noexcept {
    return kHeaderReadErrorTable[static_cast<std::size_t>(k)].code;
}

// Compose a self-sufficient FF2 wrap message that includes the
// first underlying ConfigDiagnostic's prose. Without this, an
// operator with `--suppress=C_*` set sees only the FF2 wrap with
// the trailing "see preceding C_* diagnostics" pointer to nothing
// (silent-failure C2 post-FF2-#2 fold).
[[nodiscard]] std::string
firstConfigCauseInline(std::span<ConfigDiagnostic const> diags) {
    for (auto const& cd : diags) {
        if (!cd.message.empty()) return cd.message;
        if (!cd.path.empty())    return std::string{"at "} + cd.path;
    }
    return {};
}

// Emit + return helper, optionally stamping a source location. The
// `loc` parameter (when set) is threaded into BOTH the emitted
// `ParseDiagnostic` (for the reporter pipeline) AND the returned
// `HeaderReadError::at` field (for programmatic consumers without
// reporter access). D-FF2-2 fold: the struct-side mirror closes the
// gap where LSP / test pins had to re-parse reporter prose to locate
// the offending decl. `HirSourceLoc{}` is the documented absent
// value (source_span.hpp:31-39) — no optional wrapper.
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
    HeaderReadError err{kind, std::move(detail), HirSourceLoc::absent()};
    if (loc != nullptr) err.at = *loc;
    return err;
}

// Post-fold #7 silent-failure F2: when the c-subset frontend (parse /
// semantic / lowering) rejects, the underlying diagnostic carries a
// (buffer, span) pointing at the offending construct — but the FF2
// wrap kind (HeaderParseFailed) is the same across "no span possible"
// (e.g. tokenizer EOF) and "span available downstream" (e.g.
// H_ExternHasInitializer at lowering). Mirror the locus into the
// returned struct when any reported Error carried one, so the
// struct-side `at` is informative for the LSP / test path. First
// span-bearing Error wins; later diagnostics are typically cascade
// reports of the same construct.
//
// Post-fold #8 type-design Q4: signature takes `std::span` so the
// caller bounds the scan to diagnostics emitted DURING this call via
// `subspan(errStart)`. Pre-fix this was an `offset` parameter on a
// reporter ref — the span signature is honest about what's scanned
// AND makes cross-call contamination impossible by construction.
// `readCHeaderDirectory` (ingest.cpp loop) is the concrete consumer
// that drove the bound — without it, file #2's HeaderReadError.at
// would inherit file #1's leftover span on the shared reporter.
//
// "Useful locus" filter delegates to `HirSourceLoc::spansText()` — see
// that predicate's docstring for the caret-vs-text-bearing semantic.
[[nodiscard]] HirSourceLoc
firstReportedErrorSpan(std::span<ParseDiagnostic const> diags) noexcept {
    for (auto const& d : diags) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        HirSourceLoc const loc{d.buffer, d.span};
        // Post-fold #10 type-design Q2 fold: `spansText()` encodes the
        // "covering locus, not a caret pointer" requirement the
        // previous inline `buffer.valid() + length() > 0` checks
        // expressed implicitly.
        if (!loc.spansText()) continue;
        return loc;
    }
    return HirSourceLoc::absent();
}

// Post-fold #9 silent-failure H1: first underlying Error's `actual`
// string for self-sufficient wrap messages. Mirror of
// `firstConfigCauseInline` for the ParseDiagnostic band — used when
// `--suppress=H_*` drops the underlying diagnostic so the FF2 wrap
// would otherwise point at non-existent "preceding diagnostics".
// Inlining the cause keeps the verdict self-sufficient under any
// suppress policy.
[[nodiscard]] std::string
firstReportedErrorCauseInline(std::span<ParseDiagnostic const> diags) {
    for (auto const& d : diags) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        if (!d.actual.empty()) return d.actual;
    }
    return {};
}

// Post-fold #9 simplifier + M1 clamp: bound the reporter scan to
// diagnostics emitted DURING this call. `std::min` clamp defends
// against a future refactor that calls `truncateTo` (rollback) on
// the reporter mid-call, which would otherwise make
// `subspan(errStart)` UB when errStart > reporter.all().size().
// Today the path is append-only — clamp is defense-in-depth.
[[nodiscard]] std::span<ParseDiagnostic const>
diagsSince(DiagnosticReporter const& reporter,
           std::size_t               errStart) noexcept {
    auto all = reporter.all();
    return all.subspan(std::min(errStart, all.size()));
}

// Post-fold #8 simplifier (was "R4"; harmonized post-fold #9): the
// two reporter-scan-with-cause sites in `readCHeaderFromText`
// (semantic-fail wrap + lowering-fail wrap) share this shape.
// Mirrors the existing `firstConfigCauseInline` pattern. Post-fold
// #9 H1: when the span scan finds no locus (every underlying error
// was suppressed by policy), inline the first Error's `actual`
// string into the detail so the wrap stays self-sufficient under
// `--suppress=H_*` (silent-failure HIGH fold).
[[nodiscard]] HeaderReadError
emitWithFirstReportedCause(HeaderReadErrorKind              kind,
                           std::string                      detail,
                           std::span<ParseDiagnostic const> diagsThisCall,
                           DiagnosticReporter&              reporter) {
    HirSourceLoc const cause = firstReportedErrorSpan(diagsThisCall);
    // Post-fold #12 F3: producer-consumer symmetry — firstReportedErrorSpan
    // filters by `spansText()` (covering locus, not caret), so the consumer
    // tests the same predicate. Pre-fix the consumer used `isPresent()`
    // which would have accepted a caret-only locus if a future producer
    // ever returned one — a defensive harmonization, not a current bug.
    HirSourceLoc const* causePtr = cause.spansText() ? &cause : nullptr;
    if (causePtr == nullptr) {
        std::string const inlined =
            firstReportedErrorCauseInline(diagsThisCall);
        if (!inlined.empty()) {
            detail += " (underlying cause: ";
            detail += inlined;
            detail += ")";
        }
    }
    return emitAndReturn(kind, std::move(detail), reporter, causePtr);
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
    // Silent-failure C2 (mid-read I/O truncation) + C3 (output-side
    // OOM): a successful `is_open()` followed by `rdbuf` drain that
    // hits either an input badbit (disk/network I/O error) OR an
    // output badbit (allocation failure mid-stream) leaves a partial
    // prefix in `ss`. Without checking BOTH, the parser would either
    // silently accept truncated input or emit a misleading parse
    // error. (post-FF2-#2 silent-failure CRITICAL fold.)
    if (in.bad() || ss.bad()) {
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::FileOpenFailed,
            std::string{"FFI header file I/O error after open: "}
                + path.generic_string()
                + (ss.bad() ? " (output buffer allocation failure)" : ""),
            reporter));
    }
    return ss.str();
}

} // namespace

std::string_view
headerReadErrorKindName(HeaderReadErrorKind k) noexcept {
    auto const idx = static_cast<std::size_t>(k);
    if (idx >= kHeaderReadErrorTable.size()) return "Unknown";
    return kHeaderReadErrorTable[idx].name;
}

std::expected<std::vector<ImportSurface>, HeaderReadError>
readCHeaderFromText(std::string_view    text,
                    std::string_view    headerPathLabel,
                    std::string_view    importLibrary,
                    DiagnosticReporter& reporter) {
    // Post-fold #8: snapshot reporter size at entry so the F2
    // first-Error-span scan is bounded to diagnostics emitted by THIS
    // call, not by prior callers' leftover diagnostics on the same
    // shared reporter.
    std::size_t const errStart = reporter.all().size();
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
        // Forward C_* diagnostics so they reach the reporter (subject
        // to user `--suppress` policy). Inline the first cause into
        // the FF2 wrap so the verdict is self-sufficient even when
        // C_* codes are suppressed (silent-failure C2 fold).
        std::string const cause = firstConfigCauseInline(loaded.error());
        forwardConfigDiagnostics(loaded.error(), reporter);
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::GrammarLoadFailed,
            std::string{"FFI header reader could not load shipped c-subset "
                        "grammar"} + (cause.empty() ? "" : ": " + cause)
                + ".",
            reporter));
    }

    UnitBuilder builder{*loaded};
    builder.addInMemory(std::string{text}, std::string{headerPathLabel});
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    SemanticModel model = analyze(cu);

    for (auto const& d : model.diagnostics().all()) {
        reporter.report(d);
    }
    if (model.hasErrors()) {
        return std::unexpected(emitWithFirstReportedCause(
            HeaderReadErrorKind::HeaderParseFailed,
            std::string{"c-subset frontend rejected header '"}
                + std::string{headerPathLabel} + "' — see preceding diagnostics.",
            diagsSince(reporter, errStart),
            reporter));
    }

    auto loweringResult = lowerToHir(model, reporter);
    if (loweringResult == nullptr) {
        // Defense-in-depth: `lowerToHir` is documented to return a
        // non-null unique_ptr today, but a future contract change to
        // signal catastrophic failure via nullptr would UB through
        // the `->ok` deref below. (silent-failure H1 fold.)
        return std::unexpected(emitAndReturn(
            HeaderReadErrorKind::InternalInvariant,
            std::string{"internal: lowerToHir returned nullptr for header '"}
                + std::string{headerPathLabel} + "' — file a bug report.",
            reporter));
    }
    if (!loweringResult->ok) {
        return std::unexpected(emitWithFirstReportedCause(
            HeaderReadErrorKind::HeaderParseFailed,
            std::string{"CST→HIR lowering rejected header '"}
                + std::string{headerPathLabel} + "' — see preceding diagnostics.",
            diagsSince(reporter, errStart),
            reporter));
    }

    Hir const& hir = loweringResult->hir;
    HirSourceMap const& sourceMap = loweringResult->sourceMap;

    auto locFor = [&sourceMap](HirNodeId n) -> HirSourceLoc {
        if (auto const* loc = sourceMap.tryGet(n)) return *loc;
        return HirSourceLoc::absent();
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
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::InternalInvariant,
                    std::string{"internal: lowering produced an Error "
                                "recovery node at top level of header '"}
                        + std::string{headerPathLabel}
                        + "' despite ok=true — file a bug report.",
                    reporter, &loc));
            default:
                // Future HirKinds + any module-scope shape FF2 doesn't
                // expect (Module, Block, statements, expressions,
                // Unreachable, Extension). The numeric kind value is
                // surfaced so a future regression report names exactly
                // what landed (silent-failure H5 post-FF2-#2 fold).
                return std::unexpected(emitAndReturn(
                    HeaderReadErrorKind::HeaderHasUnsupportedTopLevel,
                    std::string{"header '"} + std::string{headerPathLabel}
                        + "' contains an unsupported top-level "
                          "declaration shape (HirKind="
                        + std::to_string(static_cast<unsigned>(kind))
                        + ") — extend FF2's kind-switch or remove the "
                          "construct from the curated header.",
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

} // namespace dss::ffi
