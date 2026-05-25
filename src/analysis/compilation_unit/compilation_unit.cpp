#include "analysis/compilation_unit/compilation_unit.hpp"

#include "analysis/compilation_unit/import_resolver.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/tokenizer.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dss {

namespace {

// Release-mode-fatal guard. Mirrors `treeFatal` in tree.cpp and `tbFatal`
// in tree_builder.cpp — same style, same exit posture.
[[noreturn]] void cuFatal(char const* what) {
    std::fputs("dss::CompilationUnit fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Build + report a driver-level diagnostic (no source span — driver errors
// reference a path/label, not a byte range). The renderer handles an
// invalid/absent buffer gracefully.
void reportDriver(DiagnosticReporter& rep,
                  DiagnosticCode      code,
                  DiagnosticSeverity  severity,
                  BufferId            buffer,
                  std::string         actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = severity;
    d.buffer   = buffer;
    d.span     = SourceSpan::empty(0);
    d.actual   = std::move(actual);
    rep.report(std::move(d));
}

} // namespace

// ── CompilationUnit::nextId ───────────────────────────────────────────────
// Process-global monotonic counter starting at 1; 0 is InvalidCompilationUnit.
CompilationUnitId CompilationUnit::nextId() noexcept {
    static std::atomic<std::uint32_t> counter{1};
    return CompilationUnitId{counter.fetch_add(1, std::memory_order_relaxed)};
}

// ── CompilationUnit lifecycle ─────────────────────────────────────────────
CompilationUnit::CompilationUnit(PrivateTag,
                                 CompilationUnitId                    id,
                                 std::shared_ptr<GrammarSchema const> schema,
                                 std::vector<Tree>                    trees,
                                 DiagnosticReporter                   driverDiagnostics,
                                 std::vector<CrossTreeRef>            crossRefs)
    : id_(id)
    , schema_(std::move(schema))
    , trees_(std::move(trees))
    , driverDiagnostics_(std::move(driverDiagnostics))
    , crossRefs_(std::move(crossRefs)) {}

CompilationUnit::~CompilationUnit()                                            = default;
CompilationUnit::CompilationUnit(CompilationUnit&&) noexcept                   = default;
CompilationUnit& CompilationUnit::operator=(CompilationUnit&&) noexcept        = default;

// ── CompilationUnit accessors ─────────────────────────────────────────────
CompilationUnitId             CompilationUnit::id()                const noexcept { return id_; }
std::span<Tree const>         CompilationUnit::trees()             const noexcept { return trees_; }
DiagnosticReporter const&     CompilationUnit::driverDiagnostics() const noexcept { return driverDiagnostics_; }
std::span<CrossTreeRef const> CompilationUnit::crossRefs()         const noexcept { return crossRefs_; }

GrammarSchema const& CompilationUnit::schema() const noexcept {
    // Mirrors Tree::schema (tree.cpp): a moved-from CU has a null schema_
    // (the shared_ptr was moved out). Dereferencing it is UB; abort loudly
    // instead — same fail-loud posture as the rest of the substrate. The
    // other accessors return empty spans / a valid reporter ref on a
    // moved-from CU and are intentionally safe to read.
    if (!schema_) {
        cuFatal("CompilationUnit::schema: no schema (moved-from CompilationUnit?)");
    }
    return *schema_;
}

// ── UnitBuilder ───────────────────────────────────────────────────────────
UnitBuilder::UnitBuilder(std::shared_ptr<GrammarSchema const> schema)
    : id_(CompilationUnit::nextId())
    , schema_(std::move(schema))
    // Driver diagnostics are keyed by PATH, not by source span — every
    // D_FileNotFound / D_DuplicateFile shares (code, InvalidBuffer, empty
    // span), so the reporter's span-based dedup window would silently
    // collapse N distinct missing files into one message. Disable dedup on
    // this reporter so each bad path surfaces. (Per-tree reporters keep
    // their dedup; this disables it only for the CU's driver-level stream.)
    , driverDiagnostics_(DiagnosticReporter::Config{.dedupWindow = 0}) {
    if (!schema_) {
        cuFatal("UnitBuilder: schema is null");
    }
}

UnitBuilder::~UnitBuilder() = default;

CompilationUnitId UnitBuilder::id() const noexcept { return id_; }

void UnitBuilder::addTree(Tree&& tree) {
    if (finished_) {
        cuFatal("UnitBuilder::addTree called after finish()");
    }
    trees_.push_back(std::move(tree));
}

TreeId UnitBuilder::parseAndAdd_(std::shared_ptr<SourceBuffer> src) {
    // Empty translation unit is valid (consistent with "empty CU is valid"):
    // note it as Info but still parse + add the (empty) tree. Lives here so
    // both addFile and addInMemory get the check from one place; the buffer
    // name (path for files, label for in-memory) identifies it.
    if (src->size() == 0) {
        reportDriver(driverDiagnostics_, DiagnosticCode::D_EmptyInput,
                     DiagnosticSeverity::Info, src->id(),
                     std::string{src->name()});
    }
    // Canonical tokenize → parse → ingest sequence (mirrors the LSP parse
    // path). The tokenizer's lexer diagnostics are handed to the Parser,
    // which folds them into the produced Tree's reporter (§2.6 C2-L1), so
    // the finished Tree owns lexer + parser diagnostics in one stream.
    Tokenizer tk{src, schema_};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema_, std::move(stream), {}, std::move(lexDiags)};
    addTree(std::move(p).parse().tree);
    return trees_.back().id();
}

void UnitBuilder::addIncludeDir(std::filesystem::path dir) {
    if (finished_) {
        cuFatal("UnitBuilder::addIncludeDir called after finish()");
    }
    includeDirs_.push_back(std::move(dir));
}

TreeId UnitBuilder::loadAndAdd_(std::filesystem::path const& path, bool& ok) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    std::string const key = (ec ? path.lexically_normal() : canonical).string();

    // Already loaded (by addFile or a previous include) → reuse, no re-parse.
    if (auto it = pathToTreeIndex_.find(key); it != pathToTreeIndex_.end()) {
        ok = true;
        return trees_[it->second].id();
    }

    std::shared_ptr<SourceBuffer> src;
    try {
        src = SourceBuffer::fromFile(path);
    } catch (std::exception const&) {
        ok = false;
        return InvalidTree;  // caller (resolver) emits D_UnresolvedImport.
    }
    seenPaths_.insert(key);
    TreeId const id = parseAndAdd_(std::move(src));
    pathToTreeIndex_[key] = trees_.size() - 1;
    ok = true;
    return id;
}

void UnitBuilder::addInMemory(std::string source, std::string label) {
    if (finished_) {
        cuFatal("UnitBuilder::addInMemory called after finish()");
    }
    parseAndAdd_(SourceBuffer::fromString(std::move(source), std::move(label)));
}

void UnitBuilder::addFile(std::filesystem::path path) {
    if (finished_) {
        cuFatal("UnitBuilder::addFile called after finish()");
    }

    // Dedup by weakly-canonical path (handles `.`/`..`/symlinks without
    // requiring the file to exist). On canonicalization failure fall back
    // to the lexically-normal form so a bad path is still keyed stably.
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    std::string const key =
        (ec ? path.lexically_normal() : canonical).string();
    if (!seenPaths_.insert(key).second) {
        reportDriver(driverDiagnostics_, DiagnosticCode::D_DuplicateFile,
                     DiagnosticSeverity::Warning, InvalidBuffer, path.string());
        return;  // already added — skip the re-parse.
    }

    std::shared_ptr<SourceBuffer> src;
    try {
        src = SourceBuffer::fromFile(path);
    } catch (std::exception const& e) {
        // Missing/unreadable file, mid-read IO error, or a read-time
        // allocation failure (bad_alloc/length_error) — all *expected*
        // runtime failures at this boundary. Continue-on-failure (§2.6
        // C2-L2): record + return, never propagate (a throw would abort the
        // whole CU build). The 4-GiB cap is a deliberate hard abort inside
        // fromFile and is intentionally not catchable here.
        reportDriver(driverDiagnostics_, DiagnosticCode::D_FileNotFound,
                     DiagnosticSeverity::Error, InvalidBuffer, e.what());
        return;
    }
    parseAndAdd_(std::move(src));
    // Record the path→tree mapping so a later #include resolving to this same
    // file dedups against it instead of re-parsing.
    pathToTreeIndex_[key] = trees_.size() - 1;
}

CompilationUnit UnitBuilder::finish() && {
    if (finished_) {
        cuFatal("UnitBuilder::finish() called twice");
    }

    // Resolve imports BEFORE marking finished: the c-subset resolver may load
    // additional included files (include-following) via the loadFile callback,
    // which routes through addTree — and addTree aborts once finished_ is set.
    std::vector<CrossTreeRef> crossRefs;
    auto const resolver = chooseResolver(*schema_);
    ResolutionContext context{
        trees_,
        *schema_,
        driverDiagnostics_,
        includeDirs_,
        [this](std::filesystem::path const& path, bool& ok) { return loadAndAdd_(path, ok); },
        crossRefs,
    };
    resolver->resolve(context);

    finished_ = true;
    return CompilationUnit{
        CompilationUnit::PrivateTag{},
        id_,
        std::move(schema_),
        std::move(trees_),
        std::move(driverDiagnostics_),
        std::move(crossRefs),
    };
}

} // namespace dss
