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

#include "analysis/syntactic/binder_sketch.hpp"   // AmbiguousTypeNameCandidate (FC2 sidecar)
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
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
                    std::vector<CrossTreeRef>            crossRefs,
                    std::vector<std::filesystem::path>   shippedLibDescriptors,
                    std::uint32_t                        typeNameReparseCount = 0,
                    std::vector<std::shared_ptr<SourceBuffer>> auxiliaryBuffers = {});

    ~CompilationUnit();  // out-of-line; mirrors Tree's discipline.

    CompilationUnit(CompilationUnit const&)            = delete;
    CompilationUnit& operator=(CompilationUnit const&) = delete;
    CompilationUnit(CompilationUnit&&) noexcept;
    CompilationUnit& operator=(CompilationUnit&&) noexcept;

    [[nodiscard]] CompilationUnitId              id()                const noexcept;

    // Homogeneous-case schema accessor (every Tree in the CU shares this schema
    // in CU1-CU4). In a multi-language CU (HR11/CU5) this returns the PRIMARY —
    // the builder's first registered schema (`schemas_[0]`), NOT necessarily any
    // particular tree's schema. For language-accurate work use per-`Tree::schema()`
    // or `compositeSourceLanguage()`; the per-phase engines all dispatch on
    // `tree.schema()`, never this accessor.
    [[nodiscard]] GrammarSchema const&           schema()            const noexcept;

    // Frozen after construction. Span's data pointer is stable for the
    // CU's lifetime — UnitBuilder seals the vector at `finish()` and the
    // CU exposes no post-construction mutator (L1).
    [[nodiscard]] std::span<Tree const>          trees()             const noexcept;

    // HR11: the CU's DISTINCT per-tree source-language names joined in tree
    // order ("CSubset+TsqlSubset"); a homogeneous CU yields its one name. A
    // best-effort informational label for downstream module / type-lattice
    // `sourceLanguage` tags — purely descriptive, never a dispatch key.
    [[nodiscard]] std::string                    compositeSourceLanguage() const;

    // Driver-level diagnostics (file-not-found, schema-load forwarding,
    // ...). Empty in CU1 — the first D_* codes land in CU2.
    [[nodiscard]] DiagnosticReporter const&      driverDiagnostics() const noexcept;

    // Always returns an empty span in CU1-CU3. CU4's ImportResolver is the
    // only producer (D4); the accessor + struct ship now so CU3's semantic-
    // phase contract sees the consumption shape from day one (L5).
    // LANDMARK(CU4): when ImportResolver populates crossRefs, the empty-span
    // tests in test_compilation_unit.cpp must be revisited deliberately.
    [[nodiscard]] std::span<CrossTreeRef const>  crossRefs()         const noexcept;

    // FF11 neutral-JSON shipped-library descriptors
    // (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC): the absolute paths of every
    // shipped-lib JSON descriptor an angle/system `#include <h>` resolved to.
    // Empty unless a tree did `#include <h>` that mapped to a `<stem>.json` on
    // the system search path. The semantic phase reads each descriptor and
    // mints its extern symbols into scope (the builtinFunctions analogue) — a
    // descriptor is a neutral symbol table, NOT a parsed source Tree, so it
    // produces no entry in `trees()`/`crossRefs()`. CU4's ImportResolver is the
    // only producer.
    [[nodiscard]] std::span<std::filesystem::path const>
    shippedLibDescriptors() const noexcept;

    // Process-global monotonic id counter. Mirrors `TreeBuilder::nextTreeId`
    // exactly. Public so test harnesses can mint a `CompilationUnitId` that
    // does not collide with builder-produced ids — same posture as
    // `TreeBuilder::nextTreeId()`.
    [[nodiscard]] static CompilationUnitId nextId() noexcept;

    // FC2 observability: how many trees the type-name oracle REPARSED
    // during finish() (cross-file ambiguous-cast resolution). 0 when no
    // tree recorded a resolvable AmbiguousTypeNameCandidate — the pin
    // that the reparse pass costs nothing on candidate-free builds.
    // Pure observability, NOT a behavior knob.
    [[nodiscard]] std::uint32_t typeNameReparseCount() const noexcept {
        return typeNameReparseCount_;
    }

    // FC13: auxiliary source buffers that diagnostics may reference but which
    // are NOT a parsed tree's `source()` -- the C preprocessor's origin
    // buffers (the original main file + every quote-`#include`'d header). A
    // header-origin (or post-splice main-origin) diagnostic is remapped onto
    // one of these by the PP line-map, so the driver MUST register them with
    // the diagnostic `BufferRegistry` for positioned rendering (otherwise the
    // remapped diagnostic renders as `--> <unknown-buffer:N>`). Empty for a CU
    // whose files were not preprocessed. A buffer here is a neutral text
    // source for attribution, NOT a compiled unit (it yields no `trees()`
    // entry).
    [[nodiscard]] std::span<std::shared_ptr<SourceBuffer> const>
    auxiliaryBuffers() const noexcept;

private:
    CompilationUnitId                    id_;
    std::shared_ptr<GrammarSchema const> schema_;
    std::vector<Tree>                    trees_;
    DiagnosticReporter                   driverDiagnostics_;
    std::vector<CrossTreeRef>            crossRefs_;
    std::vector<std::filesystem::path>   shippedLibDescriptors_;  // FF11 neutral-JSON descriptor paths
    std::uint32_t                        typeNameReparseCount_ = 0;  // FC2 oracle observability
    std::vector<std::shared_ptr<SourceBuffer>> auxiliaryBuffers_;    // FC13 PP origin buffers (header/main) for diagnostic rendering
};

// Single-use builder for CompilationUnit. Non-copyable + non-movable, same
// posture as Parser/TreeBuilder: the only legitimate consumption is
// `std::move(builder).finish()`. CU1's mutator surface is just addTree;
// CU2 adds addFile / addInMemory.
class DSS_EXPORT UnitBuilder {
public:
    // Single-language builder: the one schema is the CU's primary (the
    // `CompilationUnit::schema()` convenience) AND the only entry in the schema
    // registry, so `addFile` always routes to it. Every CU1-CU4 caller uses this.
    explicit UnitBuilder(std::shared_ptr<GrammarSchema const> schema);

    // Multi-language builder (HR11/CU5): `schemas` is the registry `addFile`
    // routes against by file extension; `schemas[0]` is the CU's primary. Must
    // be non-empty.
    explicit UnitBuilder(std::vector<std::shared_ptr<GrammarSchema const>> schemas);

    // Register an additional source language so `addFile` can route a path to it
    // by matching the extension against each registered schema's `fileExtensions`
    // (first registered match wins). NOT required for the explicit-schema
    // `addInMemory` overload — that auto-registers its schema at parse. Aborts
    // after finish().
    void registerSchema(std::shared_ptr<GrammarSchema const> schema);

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
    // in-memory sources are explicit. Uses the CU's primary schema. Aborts
    // if called after finish().
    void addInMemory(std::string source, std::string label);

    // Multi-language in-memory variant (HR11/CU5): parse `source` under an
    // explicit `schema` (an in-memory buffer has no extension to route by, so
    // the language is named directly). `schema` need not be in the registry.
    // Aborts if called after finish().
    void addInMemory(std::string source, std::string label,
                     std::shared_ptr<GrammarSchema const> schema);

    // Declare a directory the c-subset import resolver searches for
    // `#include "x.h"` targets (in addition to the including file's own
    // directory). The full `.dss-project.json` include-path layer is AP2's
    // job; this is the minimal hook CU4 needs. Aborts if called after finish().
    void addIncludeDir(std::filesystem::path dir);

    // FF11: declare a SYSTEM include directory (absolute) the import
    // resolver searches for the ANGLE form `#include <h>` (distinct from
    // `addIncludeDir`, the quote form's search). The analogue of C's
    // /usr/include; the production driver resolves the language's
    // `semantics.shippedLibDirs` config strings to absolute dirs and
    // calls this per dir. Aborts if called after finish().
    void addSystemDir(std::filesystem::path dir);

    // Single-use, rvalue-qualified (L6). The `finished_` latch catches the
    // `std::move(b).finish(); std::move(b).finish();` corner case — `std::move`
    // does not consume the lvalue, so a second rvalue-qualified call is
    // syntactically valid and must be a runtime fatal.
    [[nodiscard]] CompilationUnit finish() &&;

private:
    // Shared tail of addFile/addInMemory: tokenize → parse (folding lexer
    // diagnostics into the Tree) UNDER `schema` → addTree. `src`/`schema` must
    // be non-null. Returns the appended Tree's id.
    TreeId parseAndAdd_(std::shared_ptr<SourceBuffer> src,
                        std::shared_ptr<GrammarSchema const> schema);

    // Load + parse `path` under `schema`, deduplicating by weakly-canonical path
    // against files already added (via addFile or a prior include). Returns the
    // resulting Tree's id; sets `ok` false (InvalidTree) when unreadable. This
    // backs the import resolver's include-following in finish().
    TreeId loadAndAdd_(std::filesystem::path const& path, bool& ok,
                       std::shared_ptr<GrammarSchema const> schema);

    // Resolve `path`'s source language by matching its extension against each
    // registered schema's `fileExtensions` (first match wins). Returns null on
    // no match (empty/unknown extension); `addFile` applies the single-schema
    // fall-through or emits `D_UnknownFileExtension`.
    [[nodiscard]] std::shared_ptr<GrammarSchema const>
    schemaForPath_(std::filesystem::path const& path) const;

    // FC2: per-tree parse sidecar, index-parallel to `trees_` (alignment
    // is by construction: addTree appends an EMPTY sidecar; parseAndAdd_
    // then fills the back one). Carries each parse's ambiguous type-name
    // candidates + exported global type names (the oracle's inputs) and
    // the source/schema handles a one-shot reparse needs. An externally
    // built tree pushed via addTree keeps the empty sidecar — it is
    // never reparsed (no candidates, no source handle).
    struct TreeParseSidecar {
        std::vector<AmbiguousTypeNameCandidate> candidates;
        std::vector<std::string>                globalTypeNames;
        std::shared_ptr<SourceBuffer>           source;   // null for addTree trees
        std::shared_ptr<GrammarSchema const>    schema;   // null for addTree trees
        // FC13: when the file went through the C preprocessor, `source` is the
        // SYNTHESIZED buffer and these carry the preprocessed token stream
        // (Eof-terminated) + the line-map remap closure. The FC2 type-name
        // oracle's one-shot reparse rebuilds an identical TokenStream from
        // `ppTokens` (instead of re-tokenizing raw `source`, which would lose
        // macro expansion) and re-applies `ppRemap` to the reparsed tree's
        // diagnostics. Empty/null when the file was not preprocessed.
        std::vector<Token>                              ppTokens;
        std::function<void(BufferId&, SourceSpan&)>     ppRemap;
    };

    CompilationUnitId                    id_;
    std::shared_ptr<GrammarSchema const> schema_;        // primary (= schemas_[0])
    std::vector<std::shared_ptr<GrammarSchema const>> schemas_;  // registry, by extension
    std::vector<Tree>                    trees_;
    DiagnosticReporter                   driverDiagnostics_;
    std::unordered_set<std::string>      seenPaths_;   // weakly-canonical, for addFile dedup
    // Weakly-canonical path → index into trees_, for include-following dedup
    // (resolve an #include to an already-loaded tree instead of re-parsing).
    std::unordered_map<std::string, std::size_t> pathToTreeIndex_;
    std::vector<std::filesystem::path>   includeDirs_;
    std::vector<std::filesystem::path>   systemDirs_;   // FF11 angle-include search path
    std::vector<TreeParseSidecar>        sidecars_;     // FC2; parallel to trees_
    // FC13: the C preprocessor's origin buffers (original main + every spliced
    // header), accumulated across every preprocessed file, handed to the CU as
    // `auxiliaryBuffers()` so the driver can register them for diagnostic
    // rendering (a remapped header/main diagnostic resolves to a real buffer).
    std::vector<std::shared_ptr<SourceBuffer>> auxiliaryBuffers_;
    bool                                 finished_ = false;
};

} // namespace dss
