#include "analysis/compilation_unit/compilation_unit.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <utility>

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
    , schema_(std::move(schema)) {
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

CompilationUnit UnitBuilder::finish() && {
    if (finished_) {
        cuFatal("UnitBuilder::finish() called twice");
    }
    finished_ = true;
    return CompilationUnit{
        CompilationUnit::PrivateTag{},
        id_,
        std::move(schema_),
        std::move(trees_),
        std::move(driverDiagnostics_),
        {} // crossRefs: empty in CU1 (L5/D4) — CU4 populates.
    };
}

} // namespace dss
