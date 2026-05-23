# Compilation Unit Phase — Sub-Plan

> Bridges phase #7 (`analysis-syntactic`) and phase #8 (`analysis-semantic`). The parser produces one `Tree` per file; this phase introduces the **`CompilationUnit`** — the bundle of trees + driver-resolved cross-file references that the semantic phase, IR, and codegen all consume. Closes production-readiness gaps **G-110** (multi-file translation units) and **G-111** (imports/modules).
>
> **Why this isn't bundled into semantic.** The compilation-unit boundary is the cross-cutting decision that ALL downstream phases depend on: semantic walks `CompilationUnit.trees` to build a unified symbol table; IR lowers per-CU (one CU → one IR module); codegen / linker emit one artifact per CU. Bundling this into semantic would force semantic-phase PRs to re-litigate the CU shape every time. Keeping it standalone forces the design decision early, makes the cross-CU contract explicit, and lets the LSP (PA5) operate on per-file `Tree`s without needing the semantic phase to ship first.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1 production-critical; gates phase #8 (`analysis-semantic`). Production-readiness G-110 + G-111. |
| Predecessors  | ✅ Parser phase #7 closed ([`05-parser-plan - ok.md`](./05-parser-plan%20-%20tbd.md)) — PA0–PA5b all shipped. CU1 unblocked. LSP (PA5a/PA5b) ships per-file as designed; cross-file LSP features wait for a dedicated LSP follow-up plan post-phase #8. |
| Successors    | Phase #8 (`analysis-semantic`) consumes `CompilationUnit` instead of bare `Tree`. Phase #9 (IR) lowers per-CU. Phase #11 (codegen) emits one artifact per CU. |
| Scope         | **Bounded.** CU1: `CompilationUnit` type + driver-level aggregation. CU2: multi-file parser invocation + per-file diagnostic merging. CU3: cross-tree symbol-shape hook (the contract semantic phase consumes). CU4: per-language import-resolution shim (heuristic for v1; schema-driven in v3). |

---

## 1. Motivation

The current model is `parse(text) → Tree` — one file, one tree. Every real c-subset program spans `.c` + `.h` files. Every real T-SQL deployment is a directory of `.sql` files. Phase #8's symbol table must resolve `myFunc` declared in `foo.h` and called from `bar.c`. Without a CU layer:

- The semantic-phase symbol table grows two implementations (single-tree vs. multi-tree) and the second is bolted on.
- The LSP can't show "find references" across files.
- The codegen has no concept of "what files compose this binary" — the driver guesses, or it's hardcoded.

The CU layer makes the boundary explicit. Every later phase consumes a `CompilationUnit` and stops caring how many files went in.

---

## 2. Design

### 2.1 Files

```
src/analysis/compilation_unit/
├── CMakeLists.txt
├── compilation_unit.hpp / .cpp     # CompilationUnit type + arena
├── unit_builder.hpp / .cpp         # Driver-side aggregator
└── import_resolver.hpp / .cpp      # Per-language import-syntax dispatcher (CU4)

tests/analysis/compilation_unit/
├── CMakeLists.txt
├── test_compilation_unit.cpp       # unit type contract + arena lifetime
├── test_unit_builder.cpp           # multi-file aggregation pinning
└── test_import_resolver.cpp        # per-language resolution pins
```

`src/analysis/compilation_unit/` is a new sibling to `src/analysis/{lexical,syntactic,semantic}/` — distinct because it bridges them rather than being one of them.

### 2.2 Public API (proposed)

```cpp
namespace dss {

// Strong id stamped on every CompilationUnit so cross-CU references
// (post-v1: shared library boundaries) are catchable.
DSS_STRONG_ID(CompilationUnitId);

// One CompilationUnit = one logical product. Owns its trees + driver-
// level diagnostics + resolved cross-file references. Intentionally
// artifact-profile-agnostic — the profile lives on `CompilationContext`
// (see 06-artifact-profile-plan AP3) so the CU layer doesn't take a
// hard dependency on a sub-plan that ships in parallel and so the same
// CU shape works for cli/lib/script alike.
class DSS_EXPORT CompilationUnit {
public:
    CompilationUnit(CompilationUnitId,
                    std::shared_ptr<GrammarSchema const>);

    [[nodiscard]] CompilationUnitId            id() const noexcept;
    [[nodiscard]] GrammarSchema const&         schema() const noexcept;
    [[nodiscard]] std::span<Tree const>        trees() const noexcept;
    [[nodiscard]] DiagnosticReporter const&    driverDiagnostics() const noexcept;

    // Cross-file reference table (CU3): every Tree-local Identifier
    // node that resolves to a definition in ANOTHER Tree carries a
    // CrossTreeRef. Semantic-phase symbol table consumes this.
    [[nodiscard]] std::span<CrossTreeRef const> crossRefs() const noexcept;

    // Single-use: built by UnitBuilder, frozen here.
    CompilationUnit(CompilationUnit&&) noexcept = default;
    CompilationUnit(CompilationUnit const&)     = delete;
};

// Per-edge: "the Identifier at (sourceTree, sourceNode) refers to the
// definition at (targetTree, targetNode)". Populated by the import
// resolver (CU4) before the CU is handed to semantic.
struct DSS_EXPORT CrossTreeRef {
    TreeId  sourceTree;  NodeId sourceNode;
    TreeId  targetTree;  NodeId targetNode;
    // Optional: the import-syntax span that introduced the reference
    // (e.g. the `#include "x.h"` directive). Useful for diagnostics.
    std::optional<SourceSpan> importSpan;
};

// Driver-side aggregator: feed it files; finish() returns a frozen CU.
// Like CompilationUnit, intentionally artifact-profile-agnostic.
class DSS_EXPORT UnitBuilder {
public:
    explicit UnitBuilder(std::shared_ptr<GrammarSchema const>);
    void                            addFile(std::filesystem::path);
    void                            addInMemory(std::string source, std::string label);
    [[nodiscard]] CompilationUnit   finish() &&;
};

} // namespace dss
```

### 2.3 PR breakdown

| PR  | Title                                           | Scope                                                                                                                                                                                                       |
|-----|-------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| CU1 | `CompilationUnit` type + arena                  | The bare type: strong id, owned tree vector, owned driver diagnostics, owned cross-ref vector, schema + artifactProfile handles. No multi-file behavior yet — a single-tree CU is the smallest valid CU. Adds death tests for cross-CU NodeId misuse (analogous to SH3's cross-tree guard). |
| CU2 | Multi-file `UnitBuilder` + driver wiring        | `UnitBuilder::addFile` → tokenize → parse → push `Tree` into the CU. Driver diagnostics carry per-file `SourceBuffer` reference so the renderer can show the right file/line. CompilationUnit hands a `std::span<Tree const>` to consumers. Per-file order preserved (matters for some languages' include semantics). |
| CU3 | Cross-tree `SymbolId` shape + `CrossTreeRef`    | The contract semantic-phase consumes. `SymbolId` becomes CU-scoped (not tree-scoped). `NodeAttribute<SymbolId>` keys against the owning Tree's id; the symbol table's storage moves to the CU. Adds death tests for `NodeAttribute<SymbolId>` mismatches against other CUs (cross-CU NodeId leaks). |
| CU4 | Per-language import-resolution shim             | `ImportResolver` interface; built-in implementations per shipped language: **toy** = identity (no imports); **c-subset** = `#include "x.h"` resolves against the project's include-paths (v1 = same directory + project-config-declared include dirs; full system-header resolution post-v1); **tsql-subset** = either (a) concat-order (sqlcmd `:r`) or (b) cross-statement DB.schema reference (driver-injected from project config). Resolution populates the CU's `crossRefs` vector before semantic gets it. |

### 2.4 What's NOT in this phase

| Out of scope                                | Why                                                                                              |
|---------------------------------------------|--------------------------------------------------------------------------------------------------|
| Symbol table construction                   | Phase #8 — CU3 ships only the SHAPE the symbol table consumes.                                   |
| Type inference / checking                   | Phase #8.                                                                                        |
| Schema-declared import syntax (v3 candidate)| CU4 hardcodes resolution per shipped language. A future schema field (`imports.syntax: "C-include"` etc.) is v3 work — see [`v2-gap-catalog - tbd.md`](./v2-gap-catalog%20-%20tbd.md) row 28. |
| Full C preprocessor                         | Permanently out of c-subset scope. c-subset's `#include` resolution is a textual file-lookup, NOT macro expansion. Full C99 lives in a future `c99.lang.json` with its own preprocessor phase. |
| Incremental CU rebuild                      | Post-v1. CU rebuilds from scratch when any input file changes. |
| Shared-library / cross-CU references        | Post-v1. v1 ships single-CU binaries.                                                            |

---

## 3. Acceptance criteria

- [ ] `UnitBuilder` aggregates ≥3 files into one `CompilationUnit`; each `Tree` keeps its `BufferId` linkage so diagnostics render correct file paths.
- [ ] c-subset two-file integration test: `main.c` calls `helper()` declared in `helper.h`; `CompilationUnit.crossRefs()` contains the resolved edge.
- [ ] tsql-subset two-file integration test: `schema.sql` + `data.sql` concatenate cleanly into one CU; sequential cross-statement references resolve.
- [ ] toy single-file CU still works (degenerate case shouldn't regress).
- [ ] `CompilationUnit` is move-only, single-use (built by `UnitBuilder::finish()`, consumed by phase #8). Death tests pin the contract.
- [ ] Per-PR 5-agent review cadence (substrate tier) for CU1 + CU3 (touch attribute-table contracts); feature tier for CU2 + CU4.
- [ ] [`07-production-readiness-plan - tbd.md`](./07-production-readiness-plan%20-%20tbd.md) gaps G-110 + G-111 flipped to ✅ resolved when this phase ships.

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Does `CompilationUnit` own its `Tree`s by value (move-in) or by `shared_ptr`? | **By value** — single-use, single-owner. Sharing trees across CUs is post-v1. |
| 2 | When the parser fails partway through file N of M, does `UnitBuilder` continue to file N+1 or stop? | **Continue.** Each file gets its own `Tree` + diagnostics; the CU collects everything. Driver decides final exit code based on aggregate diagnostic severity. |
| 3 | How does the CU represent "this file is the entry point" (for `cli` artifactProfile)? | New flag on `Tree` (or on `UnitBuilder::addFile`) marking the entry-point file. Codegen reads it. v1: first file added is the entry. v2: declared in project config. |
| 4 | How do we represent files at different stages of compilation in the LSP scenario (one file dirty in the editor, others on disk)? | Out of v1 — incremental CU rebuild is post-v1. LSP re-parses the whole CU on `didChange` for now (acceptable for small projects; perf-bounded later). |
| 5 | Schema-side declaration of import syntax: when does it land? | v3 schema candidate (`imports.syntax: "C-include"` or `"SQL-USE-statement"`). Until then, CU4's resolver dispatches on language NAME, which is brittle but works for the 3 shipped languages. |

---

## 5. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| `SymbolId` becoming CU-scoped retroactively breaks existing NodeAttribute<T> assumptions | Medium | High | CU3 review tier is "substrate" — full 5-agent review on the attribute-table contract change. SH3's `treeTag` discipline is the template. |
| Import resolution heuristics (CU4) produce wrong-looking cross-refs that semantic phase trusts | Medium | High | Pin every resolver path with a "before semantic" integration test. Any unresolved `#include` emits `D_UnresolvedImport`, NOT a silent missing-ref. |
| LSP and CU interact badly (CU is whole-program; LSP is per-file) | Medium | Medium | LSP operates at the `Tree` layer for PA5 (parser-only diagnostics). Cross-file LSP features wait for a dedicated LSP follow-up plan post-phase #8. |
| Multi-file diagnostic rendering shows wrong file paths | Low | Medium | `SourceBuffer` already carries its label; the diagnostic renderer (PA3) consumes it. Just need to make sure `UnitBuilder` preserves the buffer reference per tree. |

---

## 6. Sequencing with adjacent phases

```
parser PA0 ─► PA1 ─► PA2 ─► PA3 ─► PA4 ─┬─► PA5a ─► PA5b   (LSP, per-file)
                │
                ▼
              CU1 ─► CU2 ─► CU3 ─► CU4
                                    │
                                    ▼
                                phase #8 (semantic)
                                    │
                                    ▼
                                phase #9 (IR)
```

PA5 (LSP) and CU1..CU4 are **siblings**, not sequential — LSP doesn't need CompilationUnit (it operates per-file). Both consume parser output independently. They reconverge when the LSP gains cross-file features post-phase #8.
