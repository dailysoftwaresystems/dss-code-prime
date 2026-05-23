#pragma once

// CompilationUnit (CU) — the bundle of trees + driver-resolved cross-tree
// references + driver-level diagnostics that the semantic phase, IR, and
// codegen all consume. Phase #7.5 substrate; bridges parser (per-file Tree)
// and semantic (cross-file symbol table).
//
// CU1 (this file) ships the bare type: strong id, owned tree vector, owned
// driver diagnostics, owned CrossTreeRef vector (empty until CU4), schema
// handle. Single mutator: `UnitBuilder::addTree(Tree&&)` — no addFile or
// addInMemory yet (CU2). A single-tree CU is the smallest valid CU.
//
// Locked decisions L1-L6 + deferrals D1-D8 live in
// `.plans/08-compilation-unit-plan - tbd.md` §2.5.

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace dss {

// Per-edge cross-tree reference. Struct ships in CU1 (L5); the vector
// stays empty until CU4's ImportResolver populates it.
struct DSS_EXPORT CrossTreeRef {
    TreeId sourceTree;
    NodeId sourceNode;
    TreeId targetTree;
    NodeId targetNode;
    // The import-syntax span that introduced the reference (e.g. the
    // `#include "x.h"` directive). Optional — sqlcmd `:r` concatenation
    // has no per-reference span. nullopt until CU4 populates it.
    std::optional<SourceSpan> importSpan;
};

// Single CompilationUnit. Move-only, single-use (built by UnitBuilder::finish,
// consumed by phase #8). Artifact-profile-agnostic (D8): the profile lives
// on CompilationContext (06-artifact-profile-plan AP3), not here.
class DSS_EXPORT CompilationUnit {
public:
    // Internal construction tag. UnitBuilder::finish() && is the only caller;
    // the public-but-tag-gated ctor avoids exposing field details in a
    // detail:: struct without paying for a separate header. (Deliberately
    // diverges from Tree's detail::TreeData gating — same intent, fewer
    // moving parts for a 5-field type; don't "unify" the two patterns.)
    struct DSS_EXPORT PrivateTag {};
    CompilationUnit(PrivateTag,
                    CompilationUnitId                    id,
                    std::shared_ptr<GrammarSchema const> schema,
                    std::vector<Tree>                    trees,
                    DiagnosticReporter                   driverDiagnostics,
                    std::vector<CrossTreeRef>            crossRefs);

    ~CompilationUnit();  // out-of-line; mirrors Tree's discipline.

    CompilationUnit(CompilationUnit const&)            = delete;
    CompilationUnit& operator=(CompilationUnit const&) = delete;
    CompilationUnit(CompilationUnit&&) noexcept;
    CompilationUnit& operator=(CompilationUnit&&) noexcept;

    [[nodiscard]] CompilationUnitId              id()                const noexcept;

    // Homogeneous-case schema accessor (every Tree in the CU shares this
    // schema in CU1-CU4). CU5 (v1.1) introduces per-Tree schemas for
    // multi-language CUs; this accessor stays valid as a same-schema
    // convenience and degrades to "first tree's schema" semantics there.
    [[nodiscard]] GrammarSchema const&           schema()            const noexcept;

    // Frozen after construction. Span's data pointer is stable for the
    // CU's lifetime — UnitBuilder seals the vector at `finish()` and the
    // CU exposes no post-construction mutator (L1).
    [[nodiscard]] std::span<Tree const>          trees()             const noexcept;

    // Driver-level diagnostics (file-not-found, schema-load forwarding,
    // ...). Empty in CU1 — the first D_* codes land in CU2.
    [[nodiscard]] DiagnosticReporter const&      driverDiagnostics() const noexcept;

    // Always returns an empty span in CU1-CU3. CU4's ImportResolver is the
    // only producer (D4); the accessor + struct ship now so CU3's semantic-
    // phase contract sees the consumption shape from day one (L5).
    // LANDMARK(CU4): when ImportResolver populates crossRefs, the empty-span
    // tests in test_compilation_unit.cpp must be revisited deliberately.
    [[nodiscard]] std::span<CrossTreeRef const>  crossRefs()         const noexcept;

    // Process-global monotonic id counter. Mirrors `TreeBuilder::nextTreeId`
    // exactly. Public so test harnesses can mint a `CompilationUnitId` that
    // does not collide with builder-produced ids — same posture as
    // `TreeBuilder::nextTreeId()`.
    [[nodiscard]] static CompilationUnitId nextId() noexcept;

private:
    CompilationUnitId                    id_;
    std::shared_ptr<GrammarSchema const> schema_;
    std::vector<Tree>                    trees_;
    DiagnosticReporter                   driverDiagnostics_;
    std::vector<CrossTreeRef>            crossRefs_;
};

// Single-use builder for CompilationUnit. Non-copyable + non-movable, same
// posture as Parser/TreeBuilder: the only legitimate consumption is
// `std::move(builder).finish()`. CU1's mutator surface is just addTree;
// CU2 adds addFile / addInMemory.
class DSS_EXPORT UnitBuilder {
public:
    explicit UnitBuilder(std::shared_ptr<GrammarSchema const> schema);

    ~UnitBuilder();  // out-of-line: an inline (implicit) dtor is not emitted
                     // into the DLL under -fno-keep-inline-dllexport, leaving
                     // dllimport consumers with an unresolved entry point.

    UnitBuilder(UnitBuilder const&)            = delete;
    UnitBuilder& operator=(UnitBuilder const&) = delete;
    UnitBuilder(UnitBuilder&&)                 = delete;
    UnitBuilder& operator=(UnitBuilder&&)      = delete;

    // The id the finished CU will carry. Stable from construction; useful
    // for callers that need to record the id before calling finish().
    // Mirrors `TreeBuilder::treeId()`.
    [[nodiscard]] CompilationUnitId id() const noexcept;

    // Low-level mutator: push an already-built Tree. Aborts if called
    // after finish(). CU2's addFile/addInMemory compose on top of it.
    void addTree(Tree&& tree);

    // Read `path`, tokenize, parse, and add the resulting Tree (CU2).
    // Continue-on-failure (§2.6 C2-L2): a missing/unreadable file emits
    // `D_FileNotFound` into the driver diagnostics and returns without
    // adding a tree. A path already added (by weakly-canonical comparison)
    // emits `D_DuplicateFile` and is skipped. The tokenizer's lexer
    // diagnostics are folded into the produced Tree (one stream per file,
    // §2.6 C2-L1). Aborts if called after finish().
    void addFile(std::filesystem::path path);

    // In-memory variant of addFile: `label` names the buffer for
    // diagnostics (e.g. a URI or synthetic name). No deduplication —
    // in-memory sources are explicit. Aborts if called after finish().
    void addInMemory(std::string source, std::string label);

    // Single-use, rvalue-qualified (L6). The `finished_` latch catches the
    // `std::move(b).finish(); std::move(b).finish();` corner case — `std::move`
    // does not consume the lvalue, so a second rvalue-qualified call is
    // syntactically valid and must be a runtime fatal.
    [[nodiscard]] CompilationUnit finish() &&;

private:
    // Shared tail of addFile/addInMemory: tokenize → parse (folding lexer
    // diagnostics into the Tree) → addTree. `src` must be non-null.
    void parseAndAdd_(std::shared_ptr<SourceBuffer> src);

    CompilationUnitId                    id_;
    std::shared_ptr<GrammarSchema const> schema_;
    std::vector<Tree>                    trees_;
    DiagnosticReporter                   driverDiagnostics_;
    std::unordered_set<std::string>      seenPaths_;   // weakly-canonical, for addFile dedup
    bool                                 finished_ = false;
};

} // namespace dss
