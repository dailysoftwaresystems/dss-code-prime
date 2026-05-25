# Compilation Unit Phase — Sub-Plan

> Bridges phase #7 (`analysis-syntactic`) and phase #8 (`analysis-semantic`). Introduces the **`CompilationUnit`** — the bundle of trees + driver-resolved cross-file references that the semantic phase, IR, and codegen all consume. Closes production-readiness gaps **G-110** + **G-111**.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟢 **CU1 + CU2 + CU3 + CU4 shipped.** CU1: `CompilationUnit`/`UnitBuilder`/`CrossTreeRef`/`CompilationUnitId`. CU2: `addFile`/`addInMemory` (tokenize→parse→ingest, continue-on-failure), `D_*` driver codes, lexer+parser diagnostics unified in the Tree (cross-plan into 01/05 via `TreeBuilder::ingestDiagnostics` + optional `Parser` lexer-diag param). CU3: CU-scoped `SymbolId` strong id, `UnitAttribute<T>` (the CU-scoped NodeId→T side-table the semantic phase consumes), membership-based cross-CU `NodeId` guard (the SH3-analog deferred from CU1 §2.5 D3), and — per a sanctioned scope expansion (§2.7) — a minimal `populateDeclarationSymbols` walk exercising `UnitAttribute<SymbolId>` end-to-end. CU4: `ImportResolver` (dispatch by language name) populating `crossRefs` — **c-subset `#include` following** (required a grammar amendment: `#include`/`StringStart` tokens + `includeDirective` shape — the shipped grammars had NO import syntax, §2.8 C4-X1) + **tsql cross-statement table-name matching** (`qualifiedName`→`CREATE TABLE`, no grammar change) + toy identity; `D_UnresolvedImport`/`D_UnresolvedReference`; `UnitBuilder::addIncludeDir` + include-following loader. **G-110 ✅ + G-111 ✅ resolved.** Full suite 51/51 green; substrate-tier 5-agent review + fix pass on each. CU5/CU6 v1.x. v1 production-critical; gates phase #8 (`analysis-semantic`). |
| Predecessors  | ✅ Parser phase #7 closed ([`05-parser-plan - ok.md`](./05-parser-plan%20-%20ok.md)) — PA0–PA5b all shipped. CU1 unblocked. LSP (PA5a/PA5b) ships per-file as designed; cross-file LSP features wait for a dedicated LSP follow-up plan post-phase #8. |
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
| CU1 | `CompilationUnit` type + arena                  | The bare type: strong id (`CompilationUnitId`), owned tree vector, owned driver `DiagnosticReporter`, owned `CrossTreeRef` vector (empty until CU4), schema handle (artifact-profile-agnostic per §2.2). Single mutator: `UnitBuilder::addTree(Tree&&)` — no `addFile`/`addInMemory` yet (CU2). A single-tree CU is the smallest *non-trivial* CU; the **empty (zero-tree) CU is also valid** (degenerate case — the driver decides whether empty input is an error, not the type). Death tests pin **lifecycle invariants** (reading `schema()` on a moved-from CU aborts; double-`finish()`; `addTree` after `finish()`; null schema); the SH3-analog cross-CU NodeId guard is **deferred to CU3** — see §2.5. |
| CU2 | Multi-file `UnitBuilder` + driver wiring        | `UnitBuilder::addFile(path)` / `addInMemory(text, label)` → tokenize → parse → `addTree`. **Lexer + parser diagnostics unified in the `Tree`** via `TreeBuilder::ingestDiagnostics` + an optional `Parser` lexer-diag ctor param (Q1=C; see §2.6 C2-L1 — cross-plan into 01 + 05). Continue-on-failure (§2.6 C2-L2). First `D_*` codes: `D_FileNotFound`/`D_EmptyInput`/`D_DuplicateFile` (§2.6 C2-L3). Per-file order preserved (matters for include semantics). Entry-point is **not** here — re-routed to the driver/project-config layer (§2.6 C2-D2). |
| CU3 | Cross-tree `SymbolId` shape + `CrossTreeRef`    | The contract semantic-phase consumes. `SymbolId` becomes CU-scoped (not tree-scoped). `NodeAttribute<SymbolId>` keys against the owning Tree's id; the symbol table's storage moves to the CU. Ships **the SH3-analog cross-CU `NodeId` guard** (deferred from CU1 per §2.5) — death tests for `NodeAttribute<SymbolId>` mismatches against other CUs (cross-CU NodeId leaks). |
| CU4 | Per-language import-resolution shim             | `ImportResolver` interface; built-in implementations per shipped language: **toy** = identity (no imports); **c-subset** = `#include "x.h"` resolves against the project's include-paths (v1 = same directory + project-config-declared include dirs; full system-header resolution post-v1); **tsql-subset** = either (a) concat-order (sqlcmd `:r`) or (b) cross-statement DB.schema reference (driver-injected from project config). Resolution populates the CU's `crossRefs` vector before semantic gets it. |
| CU5 | **Multi-language CU support** (v1.1)              | Per the universal-compiler decisions in [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1 (rev 2): a single CU can contain files of **different source languages** when all converge on the same artifact via [HIR](./09-hir-plan%20-%20tbd.md). Use case: a project with `.c` (c-subset) files calling FFI exports of `.sql` (tsql-subset) sprocs, compiled to one CLI binary. Per-file schema attached to each `Tree`; HIR lowering dispatches on each file's source language; HIR-level cross-references resolve uniformly. **v1.1 deliverable** — not blocking v1. Open question: when the source languages disagree on artifactProfile, which wins? Default: per-file profiles aggregated by an artifact-policy attached to the project config. |
| CU6 | **Cross-CU references** (v1.x)                    | Lifts the v1 single-CU-per-binary assumption: a `CompilationUnit` can hold typed references to symbols defined in *another* `CompilationUnit`. Use cases: (a) **shared libraries / DLLs** — each lib is a CU; the executable's CU links against them at the symbol level; (b) **incremental compilation** — each translation unit becomes a CU; the linker combines them. Builds additively on `CompilationUnitId` provenance (CU1 L2) and the SH3-analog cross-CU `NodeId` guard (CU3 D3) — both already exist by the time CU6 lands, so CU6 introduces no retroactive substrate change. **Couples with** [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) **LK11** (cross-CU linking, also v1.x — see linker §2.12 for the LK11 scope, sequencing, and out-of-scope notes). **v1.x deliverable — trigger:** the first artifact profile that needs multiple CUs in one image (`lib` or `staticlib` consumed by a separate `cli`, or incremental rebuild scope) enters real scope. Until then, the v1 single-CU contract holds and CU6's substrate hooks (id + guard) sit idle but harmless. |

### 2.4 What's NOT in this phase

| Out of scope                                | Why                                                                                              |
|---------------------------------------------|--------------------------------------------------------------------------------------------------|
| Symbol table construction                   | Phase #8 — CU3 ships only the SHAPE the symbol table consumes.                                   |
| Type inference / checking                   | Phase #8.                                                                                        |
| Schema-declared import syntax (✅ landed) | CU4's import resolution is **config-driven** via a schema v4 `imports` block (strategy `include-following`/`name-matching`/`none` + its rule/token roles), read by ONE `ConfigDrivenImportResolver`. The earlier per-language-name dispatch (the per-language `CIncludeResolver`/`TsqlNameResolver`/`IdentityResolver` classes) **were retired** in 08.55 — thesis decision #4 is now honored end-to-end in CU4. Loader validates every referenced rule/token name at load time (`C_UnknownShape` / `C_UnknownToken` / `C_InvalidImports` / `C_MissingField`); a `genericity` test renames the shipped language and proves resolution still fires. |
| Full C preprocessor                         | Permanently out of c-subset scope. c-subset's `#include` resolution is a textual file-lookup, NOT macro expansion. Full C99 lives in a future `c99.lang.json` with its own preprocessor phase. |
| Incremental CU rebuild                      | Reserved for **CU6** (v1.x — see §2.3). v1 CUs rebuild from scratch when any input file changes; CU6 introduces incremental scope when its trigger fires. |
| Shared-library / cross-CU references        | Reserved for **CU6** (v1.x — see §2.3). v1 ships single-CU binaries.                             |

### 2.5 CU1 — locked decisions + explicit deferrals

> **Why this section exists.** "The bare type" understates several concrete cuts CU1 makes. Recording each one here with **what / why / where it lands** stops the deferrals from drifting into CU2/CU3 as undocumented surprises and gives every deferred item a named home. Decisions in this table are locked by the time CU1 PR opens; later CUs may revisit only with a load-bearing reason.

**Locked decisions (CU1 ships these shapes):**

| # | Decision | Resolution | Why |
|---|---|---|---|
| L1 | Tree storage in `CompilationUnit` | `std::vector<Tree>` (by value) with **post-`finish()` freeze invariant**. `NodeAttribute<T>` may only bind to a CU's trees after `UnitBuilder::finish()` returns. | Matches the §2.2 `std::span<Tree const>` accessor (only viable with by-value storage). Reallocation hazard against `NodeAttribute<T>`'s raw-pointer capture is eliminated by the freeze, not by `unique_ptr` indirection. |
| L2 | `CompilationUnitId` minting | Process-global atomic counter via static `CompilationUnit::nextId()`; called from `UnitBuilder` ctor; exposed via `UnitBuilder::id()`. Mirrors `TreeBuilder::nextTreeId()` exactly. | Existing project convention; callers never see the counter. `TreeId` discipline (provenance-tagged ids, untagged literals bypass the guard) is the template. |
| L3 | `DiagnosticReporter` storage | By-value member of `CompilationUnit` (not `unique_ptr`). | Header already includes `diagnostic_reporter.hpp` for the `driverDiagnostics() const&` accessor; `unique_ptr` buys no forward-decl benefit. `DiagnosticReporter` is move-able, so `CompilationUnit` move semantics are preserved. |
| L4 | `UnitBuilder` mutator surface in CU1 | **`addTree(Tree&&)` only.** No `addFile` / `addInMemory` in CU1. | "Bare type, single-tree CU is the smallest valid CU" — composition of tokenize+parse belongs in CU2. CU1 tests construct `Tree` via the existing E2E harness + `Parser` and hand the resulting tree to `addTree`. |
| L5 | `CrossTreeRef` struct location | Defined in `compilation_unit.hpp` alongside `CompilationUnit`; `crossRefs()` accessor returns an empty span in CU1. | Type-ships-with-shape: CU3/CU4 consumers see the struct from day one; only the *population* lands later. |
| L6 | `UnitBuilder::finish() &&` and `CompilationUnit` move-only | Both rvalue-qualified / single-use; `CompilationUnit(CompilationUnit const&) = delete`; `CompilationUnit(CompilationUnit&&) = default`. | Mirrors `TreeBuilder::finish() &&` + `Parser::parse() &&` + `Tree`'s move-only-by-value discipline. |

**Explicit deferrals (NOT in CU1; each has a named home and a recorded reason):**

| # | Deferred item | Lands in | Why deferred from CU1 |
|---|---|---|---|
| D1 | `UnitBuilder::addFile(path)` + `addInMemory(text, label)` (tokenize+parse internally) | **CU2** | CU1 ships the substrate primitive (`addTree`); CU2 is the multi-file composer that wraps `Tokenizer + Parser` into the builder's per-file ingestion. Keeping the composer out of CU1 lets CU1 tests pin the CU's shape independently of any tokenizer/parser flow inside `UnitBuilder`. |
| D2 | `D_*` driver-level diagnostic codes at hex `0xD001+` | **CU2** (lands here — see §2.6 C2-L3) | The driver-level `DiagnosticReporter` slot exists in CU1 (empty); CU1 emits no `D_*` codes because it has no I/O paths. CU2's `addFile`/`addInMemory` are the first failable driver operations, so CU2 opens the `0xD` block: `D_FileNotFound` / `D_EmptyInput` / `D_DuplicateFile`. **`D_SchemaLoadFailed` is NOT among them** — re-routed to the driver/program layer (§2.6 C2-D1), because the schema is a `UnitBuilder` ctor precondition (loaded before the CU exists), so schema-load failure is unreachable inside CU2. |
| D3 | SH3-analog cross-CU `NodeId` guard (i.e. `NodeId.cuTag` or membership-based enforcement) | **CU3** | The SH3 cross-tree guard exists because `NodeAttribute<T>` binds to a specific `TreeId`. The cross-**CU** version bites only when `NodeAttribute<SymbolId>` becomes CU-scoped — which IS CU3's contract change. CU1 has no cross-CU operation surface yet, so the guard would protect nothing. CU1 ships only **lifecycle-level** fatals (operating on a moved-from CU; calling `UnitBuilder::finish()` twice). |
| D4 | `CrossTreeRef` vector **population** | **CU4** | The `ImportResolver` interface that materializes cross-tree edges lives in CU4. CU1 ships the `CrossTreeRef` struct + empty vector; CU3 wires the semantic-phase consumption contract; CU4 produces the data. |
| D5 | Entry-point selection (which source is the `cli` artifact's entry) | **driver/program layer (phase #12) + `CompilationContext`** ([06](./06-artifact-profile-plan%20-%20tbd.md) AP2/AP3) — **re-routed from CU2** | **Re-routed (CU2 decision, Q4):** the entry-point is declared in `.dss-project.json`, parsed by the driver, and stamped onto `CompilationContext` beside the `artifactProfile`. CU2's earlier "first file added" heuristic is **dropped** — the project config is the source of truth, and the `CompilationUnit` stays entry-agnostic (consistent with D8 keeping the CU profile-agnostic). See §2.6 C2-D2. |
| D6 | Multi-language CUs (different `.lang.json` per tree in one CU) | **CU5** (v1.1) | Already plan-stated. Each `Tree` already carries its own `shared_ptr<GrammarSchema const>`, so CU1's by-value tree storage does not foreclose CU5; the CU-level `schema()` accessor stays the homogeneous-case convenience. |
| D7 | Cross-CU operations of any kind (references between two CUs) | **CU6** (this plan, v1.x — paired with [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) **LK11** per its §2.12) | v1 ships single-CU binaries. CU1's `CompilationUnitId` provenance + CU3's cross-CU `NodeId` guard exist so CU6/LK11 land additively, with no retroactive substrate change. Trigger: first artifact profile needing multiple CUs in one image. Until then, the v1 single-CU contract holds. |
| D8 | `artifactProfile` handles on the CU | **`CompilationContext`** ([06](./06-artifact-profile-plan%20-%20tbd.md) AP3) | §2.2 already states "intentionally artifact-profile-agnostic — the profile lives on `CompilationContext`." Earlier draft text in §2.3 erroneously listed "artifactProfile handles" in CU1; corrected — the CU itself never holds an `artifactProfile`. |

### 2.6 CU2 — locked decisions + explicit deferrals

> **Fulfills CU1 deferrals:** **D1** (`addFile`/`addInMemory`) ✅, **D2** (`D_*` codes — the file-level subset) ✅. **Re-routes D5** (entry-point) out of the CU layer entirely. Same discipline as §2.5: every cut names where it lands and why.

**Locked decisions (CU2 ships these shapes):**

| # | Decision | Resolution | Why |
|---|---|---|---|
| C2-L1 | **One file → one unified diagnostic stream** (answer to "where do lexer diags go", option C) | The `Tree` owns **all** of its file's diagnostics (lexer + parser). New `TreeBuilder::ingestDiagnostics(std::span<ParseDiagnostic const>)` (**plan 01** substrate); `Parser` ctor gains an optional trailing `std::unique_ptr<DiagnosticReporter> lexerDiagnostics = nullptr` (**plan 05**) that it folds into the builder's reporter before the walk. `addFile`/`addInMemory` pass the tokenizer's reporter through. | Clean semantics — consumers read one stream via `tree.diagnostics()`. Also closes the latent "lexer diagnostics silently dropped" gap (the LSP reads `tree.diagnostics()`, so it gets lexer errors for free once it opts in). Chosen over forwarding lexer diags into the CU driver reporter (semantic conflation: those diags are per-file, not driver-level) and over a shared-reporter refactor of both Tokenizer + TreeBuilder (more invasive). **Cross-plan**: touches two closed plans (01, 05) → CU2 reviewed at substrate tier, not feature tier. |
| C2-L2 | **Continue-on-failure** (plan §4 Q2) | A failed file emits a `D_*` diagnostic into the CU's driver `DiagnosticReporter` and processing continues to the next file; no `Tree` is added for the failed one. | Plan §4 Q2. The driver decides the final exit code from aggregate severity. A throwing `addFile` would abort the whole CU build. |
| C2-L3 | **Driver diagnostic codes** (all three) | `D_FileNotFound` (`0xD001`, Error), `D_EmptyInput` (`0xD002`, Info), `D_DuplicateFile` (`0xD003`, Warning). `addFile` dedups by `std::filesystem::weakly_canonical` path (warn + skip the repeat); `addInMemory` does **not** dedup (labels may legitimately repeat). Empty source → `D_EmptyInput` (Info) but the empty tree is still added (empty translation unit is valid, consistent with "empty CU is valid"). | Opens the `0xD` block per D2. Requires the `0xD` nibble in `diagnosticCodePrefix` + `diagnosticCodeName` cases + a `test_parse_diagnostic.cpp` pin. |
| C2-L4 | **`addFile` catches `SourceBuffer::fromFile`'s throw** | `fromFile` throws `std::runtime_error` on missing/unreadable files; `addFile` `try/catch`es → `D_FileNotFound` + continue. The 4-GiB size cap remains a hard `std::abort()` (out of scope — documented, not handled). | Prevents one bad path from aborting the entire CU build (the continue-on-failure contract depends on this). |

**Explicit deferrals (NOT in CU2; each names where it lands + why):**

| # | Deferred item | Lands in | Why deferred from CU2 |
|---|---|---|---|
| C2-D1 | `D_SchemaLoadFailed` + the `ConfigDiagnostic → ParseDiagnostic` bridge | **driver/program layer (phase #12)** | The schema is loaded *before* `UnitBuilder` is constructed (ctor precondition), so schema-load failure cannot occur inside `addFile`/`addInMemory`. The bridge belongs to the layer that calls `GrammarSchema::loadFromFile` and then builds the CU. Adding the code in CU2 would be dead/unreachable. |
| C2-D2 | Entry-point selection (the re-routed D5) | **driver/program layer (phase #12) + `CompilationContext`** ([06](./06-artifact-profile-plan%20-%20tbd.md) AP2/AP3) | The entry-point is declared in `.dss-project.json`; the driver parses it and stamps it on `CompilationContext` beside the `artifactProfile`. The CU stays entry-agnostic (mirrors D8). CU2 ships no entry flag and no "first file" heuristic. |
| C2-D3 | LSP adopting the unified lexer+parser stream | **LSP follow-up (post-phase #8)** | CU2 *adds the capability* (the optional `Parser` lexer-diag param) but only wires it from `UnitBuilder`. Changing the LSP parse job to pass `lexDiags` is a separate, low-risk follow-up — not in CU2's path. |
| C2-D4 | Position-sorted / interleaved diagnostic ordering | **deferred (rendering polish)** | CU2 seeds lexer diags before parser diags (lexer-first within a tree). A source-position-sorted merge is a later rendering concern; not needed for correctness. |
| C2-D5 | Cross-tree `SymbolId` shape + cross-CU `NodeId` guard | **CU3** (unchanged — §2.5 D3) | CU2 does multi-file aggregation only; the symbol-shape contract + SH3-analog guard remain CU3. |
| C2-D6 | `CrossTreeRef` population (import resolution) | **CU4** (unchanged — §2.5 D4) | CU2 produces independent per-file trees; resolving cross-file edges is CU4's `ImportResolver`. |

**Cross-plan amendments triggered by C2-L1** (recorded in the owning plans): **plan 01** (`01-tree-node-model-plan - ok.md`) adds `TreeBuilder::ingestDiagnostics`; **plan 05** (`05-parser-plan - ok.md`) adds the optional `Parser` lexer-diagnostics ctor param. Both are additive (default-`nullptr` / new method) — no existing caller changes.

### 2.7 CU3 — locked decisions + explicit deferrals

> **Fulfills CU1 deferral D3** (SH3-analog cross-CU `NodeId` guard) ✅ and introduces the CU-scoped `SymbolId` shape the semantic phase consumes. Same discipline as §2.5/§2.6.

**Locked decisions (CU3 ships these shapes):**

| # | Decision | Resolution | Why |
|---|---|---|---|
| C3-L1 | **Cross-CU guard = membership-based**, NOT a `NodeId.cuTag` field (the §2.5 D3 choice) | The CU-scoped table (`UnitAttribute<T>`) holds one `NodeAttribute<T>` per tree + a `TreeId.v → tree-index` routing map. A `NodeId` whose `treeTag` isn't in the map (it belongs to another CU's tree, or no CU) aborts via `detail::unit_attr::crossUnitFatal`. `NodeId` is **unchanged**. | `TreeId`s are globally unique, so a foreign id never matches any single bound tree and would otherwise just "miss" silently. Membership is the natural enforcement point because the CU already owns the tree set. A `cuTag` field would bloat every `NodeId` by 4 bytes and need threading through `emit_`/`RawTreeBuilder`/the dense iterator — for zero added catch power over membership. |
| C3-L2 | **`SymbolId` is CU-scoped**, minted per-CompilationUnit (not per-Tree) | `DSS_STRONG_ID(SymbolId)` + `InvalidSymbol` + hash in `strong_ids.hpp`, alongside the other ids. The minting counter is local to whatever produces symbols (CU3's populate walk uses a per-call monotonic counter from 1). | Plan-stated ("`SymbolId` interner is CU-scoped"). A symbol is meaningful only against the CU that produced it; the semantic phase keys its table by `SymbolId` and binds it to a `NodeId` via `UnitAttribute<SymbolId>`. |
| C3-L3 | **`UnitAttribute<T>` is the CU-analog of `NodeAttribute<T>`** — generic, header-only, treeTag-routed | `src/analysis/compilation_unit/unit_attribute.hpp`. `set`/`get`/`has`/`tryGet`/`erase`/`size`/`empty`/`forEach`, move-only, bound to `CompilationUnit const&`. Routes each `NodeId` by `treeTag`; delegates the sentinel/bounds/per-tree-tag checks to the routed `NodeAttribute`. | Mirrors the project's generic-primitive style and `NodeAttribute`'s contract. Reusable for any CU-scoped side-table, not just symbols. |
| C3-L4 | **Untagged-literal (`treeTag==0`) routing rule** | Routable only in a single-tree CU (→ the sole tree); ambiguous (fatal) in a multi-tree or empty CU. | Preserves `NodeAttribute`'s test ergonomics for the common single-tree case while refusing to guess in the genuinely ambiguous multi-tree case. |

**Sanctioned scope expansion (beyond the original §2.3 CU3 row):**

| # | Item | Decision | Why |
|---|---|---|---|
| C3-X1 | **Minimal `populateDeclarationSymbols` walk** ships in CU3 | `src/analysis/compilation_unit/symbol_population.{hpp,cpp}` — walks each tree and binds a distinct CU-scoped `SymbolId` to every `functionDecl`/`varDecl` name node. | The original §2.3 row scoped CU3 to "the SHAPE" and §2.4 put symbol-table construction in phase #8. This walk was **explicitly requested** to exercise `UnitAttribute<SymbolId>` end-to-end now. It is deliberately NOT real semantic analysis: **no** scope resolution, name lookup, shadowing/redeclaration handling, type association, or use→decl linking — those remain phase #8. Documented in-code as a phase-#8 placeholder. |

**Explicit deferrals (NOT in CU3):**

| # | Deferred item | Lands in | Why deferred from CU3 |
|---|---|---|---|
| C3-D1 | `CrossTreeRef` vector **population** | **CU4** (unchanged — §2.5 D4) | CU3 ships the symbol-binding shape; resolving cross-file edges is CU4's `ImportResolver`. `crossRefs()` stays empty. |
| C3-D2 | Real symbol table (scopes, name resolution, shadowing, types, use→decl links) | **phase #8 (`analysis-semantic`)** | CU3 ships the storage shape + the trivial populate placeholder only. |

### 2.8 CU4 — locked decisions + explicit deferrals

> **Fulfills CU1 deferral D4** (`CrossTreeRef` population) ✅ and closes **G-111** (import resolution). **Critical correction to §2.3:** the CU4 row assumed c-subset already had `#include` and tsql had `:r`; exploration confirmed **neither construct existed in any shipped grammar**, and there is no project-config / include-path layer yet (06 AP2/AP3 pending). CU4 was re-grounded on that reality (decisions below).

**Locked decisions (CU4 ships these shapes):**

| # | Decision | Resolution | Why |
|---|---|---|---|
| C4-L1 | **`ImportResolver` interface, dispatched by language NAME** | `import_resolver.{hpp,cpp}`: abstract `ImportResolver` + `chooseResolver(languageName)` → `CIncludeResolver` (`"CSubset"`) / `TsqlNameResolver` (`"TsqlSubset"`) / `IdentityResolver` (everything else, incl. toy). A `ResolutionContext` carries the growing tree vector, schema, driver diagnostics, include dirs, a `loadFile` callback, and the output `crossRefs`. | Plan Q5: schema-declared `imports.syntax` is a v3 candidate; name dispatch is the v1 shim for the 3 shipped languages. |
| C4-L2 | **`crossRefs` populated inside `UnitBuilder::finish()`** (before the `finished_` latch) | `finish()` runs `chooseResolver(schema)->resolve(ctx)` over the built trees, then constructs the CU with the populated vector. The resolver runs while `finished_` is still false so include-following can `addTree`. The CU stays immutable/frozen post-construction. | Keeps the CU contract (move-only, frozen) intact; the resolver is the only `crossRefs` producer (CU1 D4). |
| C4-L3 | **c-subset = include-following** (Q1: extend grammars + load) | Walk `includeDirective` nodes; resolve the quoted filename against the including file's dir + `addIncludeDir` dirs; **load missing files into the CU** (recursive, cycle/dedup-guarded by weakly-canonical path); emit `CrossTreeRef{filename token → included tree root, importSpan = directive span}`. | The acceptance test ("main.c includes helper.h → crossRef") is a file-inclusion edge; include-following is the faithful model the user chose. |
| C4-L4 | **tsql = cross-statement table-name matching** (Q2 preview) | Index each `CREATE TABLE`'s table name; match **table-position** `qualifiedName`s (parent ∈ {`tableRef`,`insertStmt`,`updateStmt`,`deleteStmt`}) to a definition in ANOTHER tree by case-insensitive last identifier; `importSpan = nullopt`. Column/operand `qualifiedName`s are excluded. No tsql grammar change. | tsql's cross-file unit is a table reference, not a file include; `qualifiedName`/`createTableStmt` already exist. |
| C4-L5 | **Unresolved references → driver Warnings, never silent** (Q3) | `D_UnresolvedImport` (missing `#include` file) / `D_UnresolvedReference` (no `CREATE TABLE` for a table ref) at `0xD004/0xD005`, severity Warning (phase #8 / FFI / a system catalog may still provide the target). Same-tree tsql matches are intra-file (no edge, no diagnostic). | Plan risk mitigation: "NOT a silent missing-ref." |

**Sanctioned scope expansion / cross-plan amendment:**

| # | Item | Decision | Why |
|---|---|---|---|
| C4-X1 | **c-subset grammar amendment** (`c-subset.lang.json`) | Added a `"#include"` → `IncludeKeyword` token, a `"\""` → `StringStart` token + a `"string"` lexer mode (so the quoted path lexes), and an `includeDirective: [IncludeKeyword, StringStart]` shape in the `topLevel` alt. Angle-bracket `<...>` includes and macro expansion remain out of scope (§2.4). | §2.3 assumed this existed; it didn't. The user chose "extend grammars" (Q1). Adding an unused alt branch + new tokens leaves existing c-subset corpus/smoke/golden trees unchanged (verified: mini_calc.c has no `#`/`"`). The filename is read from source text via the `StringStart` opener offset (the body is off-grammar `StringChar` tokens). |

**Explicit deferrals (NOT in CU4):**

| # | Deferred item | Lands in | Why deferred from CU4 |
|---|---|---|---|
| C4-D1 | `.dss-project.json` include-path declaration | **06 AP2/AP3** | CU4 ships the minimal `UnitBuilder::addIncludeDir` hook; the full project-config include-path layer is the artifact-profile plan's job. Same-directory resolution needs no config. |
| C4-D2 | Angle-bracket `#include <sys.h>` + system-header search + macro expansion | **post-v1 (`c99.lang.json`)** | §2.4: c-subset `#include` is a textual quoted-file lookup, not a preprocessor. |
| C4-D3 | Bracket-quoted tsql identifiers (`[Table]`) in name matching | **future** | v1 matches on the last `Identifier` token; an all-bracket name is skipped (documented limitation). |
| C4-D4 | Multi-language CUs / cross-CU references | **CU5 / CU6** (unchanged) | v1 single-language, single-CU. |

---

## 3. Acceptance criteria

- [x] **(CU2)** `UnitBuilder` aggregates ≥3 files into one `CompilationUnit` (`addFile`/`addInMemory`, mixed, order-preserving); each `Tree` keeps its `BufferId` linkage. Continue-on-failure + `D_FileNotFound`/`D_EmptyInput`/`D_DuplicateFile` + lexer+parser diagnostics unified in the Tree (Q1=C).
- [x] **(CU4)** c-subset two-file integration test: `main.c` `#include "helper.h"` → `helper.h` is loaded into the CU and `CompilationUnit.crossRefs()` contains the resolved edge (`test_import_resolver.cpp` `CSubsetIncludeLoadsTargetAndLinksIt`). Transitive includes + cycle termination + missing-include (`D_UnresolvedImport`) + cross-directory `addIncludeDir` also pinned.
- [x] **(CU4)** tsql-subset two-file integration test: `schema.sql` `CREATE TABLE` + `data.sql` `SELECT/DELETE FROM` resolve the table reference cross-tree (`importSpan = nullopt`); column-position `qualifiedName` excluded; unknown table → `D_UnresolvedReference`; same-file ref is intra-file (no edge).
- [x] **(CU1)** toy single-file CU works; empty (zero-tree) CU is valid too.
- [x] **(CU1)** `CompilationUnit` is move-only, single-use (built by `UnitBuilder::finish()`, consumed by phase #8). Death tests pin the contract: reading `schema()` on a moved-from CU, double-`finish()`, `addTree` after `finish()`, null schema.
- [x] **(CU1)** Per-PR 5-agent review cadence (substrate tier) executed for CU1 (correctness / type-design / silent-failure / test-coverage / simplicity) + fix-everything pass. CU3 also substrate tier; feature tier for CU2 + CU4.
- [x] **(CU2)** G-110 (multi-file translation units) ✅ resolved in [`07-production-readiness-plan`](./07-production-readiness-plan%20-%20tbd.md). G-111 (import resolution) waits for CU4.
- [x] **(CU3)** CU-scoped `SymbolId` strong id (+ `InvalidSymbol` + hash). `UnitAttribute<T>` routes a `NodeId` to the right tree across a multi-tree CU; membership-based cross-CU `NodeId` guard aborts on a foreign-tree id (death test pins the `crossUnitFatal` message); untagged-literal routing rule (single-tree only) + delegation to the per-tree `NodeAttribute` guard pinned by death tests.
- [x] **(CU3)** Minimal `populateDeclarationSymbols` binds a distinct CU-scoped `SymbolId` to every `functionDecl`/`varDecl` name node across a multi-file CU; `forEach` output compared exactly; uses / non-decl nodes left unbound; empty CU yields zero symbols. (Sanctioned scope expansion §2.7 C3-X1 — NOT real semantic analysis.) **Post-08.55**: the `populateDeclarationSymbols` CU3 placeholder was deleted; SE1's `SchemaDrivenSemantics` is the real mechanism.
- [x] **(CU3)** Substrate-tier 5-agent review + fix-everything pass executed.

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Does `CompilationUnit` own its `Tree`s by value (move-in) or by `shared_ptr`? | **By value** — single-use, single-owner. Sharing trees across CUs is post-v1. |
| 2 | When the parser fails partway through file N of M, does `UnitBuilder` continue to file N+1 or stop? | **Continue.** Each file gets its own `Tree` + diagnostics; the CU collects everything. Driver decides final exit code based on aggregate diagnostic severity. |
| 3 | How does the CU represent "this file is the entry point" (for `cli` artifactProfile)? | **Resolved (CU2, Q4):** the CU does **not** represent it. The entry-point is declared in `.dss-project.json`, parsed by the driver/program layer (phase #12), and stamped onto `CompilationContext` beside the `artifactProfile` ([06](./06-artifact-profile-plan%20-%20tbd.md) AP2/AP3). The earlier "first file added" heuristic is dropped — project config is the source of truth. See §2.6 C2-D2. |
| 4 | How do we represent files at different stages of compilation in the LSP scenario (one file dirty in the editor, others on disk)? | Out of v1 — incremental CU rebuild is post-v1. LSP re-parses the whole CU on `didChange` for now (acceptable for small projects; perf-bounded later). |
| 5 | Schema-side declaration of import syntax: when does it land? | **✅ Schema v4 `imports` block** — landed alongside the config-driven semantic/HIR work. Strategies (`include-following`/`name-matching`/`none`) + rule/token roles are declared per-language; the ONE `ConfigDrivenImportResolver` reads the block. No engine code branches on language name. The earlier per-language resolver classes are retired. |

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

---

## 7. PR landing log

| PR | Status | Commit / notes |
|---|---|---|
| CU4 | ✅ done | Per-language `ImportResolver` (`import_resolver.{hpp,cpp}`) dispatched by language name, run inside `UnitBuilder::finish()` to populate `crossRefs` (C4-L1/L2). **c-subset `#include` following** (C4-L3): grammar amended (`#include`/`StringStart` tokens + `string` mode + `includeDirective` shape — C4-X1, the shipped grammars had no import syntax); resolver loads missing headers into the CU (recursive, weakly-canonical cycle/dedup guard via new `pathToTreeIndex_`), emits `CrossTreeRef{filename → included root, importSpan = directive}`; filename read from source text off the `StringStart` opener. **tsql cross-statement matching** (C4-L4): table-position `qualifiedName` (parent ∈ tableRef/insert/update/delete) → `CREATE TABLE` of same name in another tree, case-insensitive last-identifier, `importSpan = nullopt`, column-position excluded, same-tree = intra-file. Unresolved → `D_UnresolvedImport`/`D_UnresolvedReference` (`0xD004/5`, Warning) (C4-L5). New `UnitBuilder::addIncludeDir` + `loadAndAdd_`; `parseAndAdd_` returns `TreeId`. Tests: `ImportResolver` suite (16: toy identity; c-subset link/transitive/cycle/missing/include-dir/empty-path/explicit-add-dedup/shared-header-diamond/in-memory-via-include-dir/in-memory-unresolved; tsql cross-file/qualified/unknown/same-file-intra/insert+update positions; exact `sourceNode`/`targetNode`/`importSpan` field checks). LANDMARK(CU4) toy assertions updated (identity → still empty). Full suite **51/51**. Substrate-tier 5-agent review + fix-everything pass (no bugs found — design validated: reallocation-safety, cycle termination, NodeId tagging, grammar load all confirmed): `rootOfTree` fail-loud guard; reuse `tokens::kIdentifier`; factor the shared tsql qualifiedName-walk; extend `toy_cu_fixture.hpp` with `loadShippedSchema`/`hasCode`/`countCode`; document the loaded-include-diagnostics-live-on-their-tree contract + in-memory/path-collision + `#include`-whitespace + bracket-id-name limitations; add the missing-coverage tests above. `_pending_` (initial; hash filled post-merge per housekeeping convention). |
| CU3 | ✅ done | CU-scoped `SymbolId` (`DSS_STRONG_ID` + `InvalidSymbol` + `DSS_HASH_ID` in `strong_ids.hpp`). New header-only `UnitAttribute<T>` (`src/analysis/compilation_unit/unit_attribute.hpp`) — the CU-scoped `NodeId`→`T` side-table: one `NodeAttribute<T>` per tree + a `TreeId.v → index` routing map; `set`/`get`/`has`/`tryGet`/`erase`/`size`/`empty`/`forEach`, move-only, bound to `CompilationUnit const&`. **Cross-CU guard = membership** (C3-L1): a `NodeId` whose `treeTag` isn't in the CU's tree set aborts via `detail::unit_attr::crossUnitFatal` (`NodeId` unchanged — no `cuTag`). Untagged literals route only in a single-tree CU (C3-L4); the routed `NodeAttribute` still applies its own sentinel/bounds/tag guards. **Sanctioned scope expansion** (§2.7 C3-X1): `symbol_population.{hpp,cpp}` ships `populateDeclarationSymbols(cu)` — a minimal walk binding a distinct CU-scoped `SymbolId` to every `functionDecl`/`varDecl` name node; deliberately NOT real semantic analysis (no scoping/resolution/types — phase #8). Tests: `UnitAttribute` (11 contract incl. routing/erase/forEach/dense-forEach/const-overloads/moved-from + 10 death: per-entry-point cross-CU foreign id ×5, untagged-in-multi-tree, InvalidNode→per-tree guard, empty-CU tagged + untagged, duplicate-TreeId construction guard) + `SymbolPopulation` (3: multi-file bind-exactly, uses-unbound, empty-unit). Shared `toy_cu_fixture.hpp` dedups the `makeToyUnit`/`loadToySchema` helpers. Full suite **50/50**. Substrate-tier 5-agent review + fix-everything pass (validate name node is an Identifier before binding; `static_assert` move-constructible; tighten cross-CU regex to pin the exact source TreeId; cover duplicate-TreeId / empty-CU / per-entry-point / const-overload / dense-forEach gaps; document the single-tree-untagged carve-out + empty-CU read/mutate asymmetry + defaulted-move divergence). `_pending_` (initial; hash filled post-merge per housekeeping convention). |
| CU2 | ✅ done | Multi-file `UnitBuilder`: `addFile(path)` + `addInMemory(text,label)` driving tokenize→parse→`addTree`, **continue-on-failure** (a bad file emits a driver diagnostic and processing continues). New `D_*` driver codes `D_FileNotFound`/`D_EmptyInput`/`D_DuplicateFile` (`0xD001-3`) + `0xD` nibble in `diagnosticCodePrefix`. **Q1=C — lexer + parser diagnostics unified in the Tree**: new `TreeBuilder::ingestDiagnostics` (plan 01 amendment) + optional `Parser` lexer-diag ctor param (plan 05 amendment), both additive/back-compat. `addFile` dedups by weakly-canonical path (`D_DuplicateFile` + skip); driver reporter constructed with `dedupWindow=0` so distinct missing files don't collapse; `fromFile` throw caught as `std::exception` (continue-on-failure covers OOM, not just missing-file). Entry-point explicitly NOT in the CU (re-routed to driver/project-config — §2.6 C2-D2). Tests: `CompilationUnitCU2` suite (in-memory/file/empty/duplicate-by-canonical-path/mixed/non-dedup + lexer-merge proof via `@`→`P_IllegalChar`) + direct `TreeBuilderIngest` tests (scope-stack-verbatim, empty-span-reached) + parser-level fold/no-fold tests + `D_*` name/prefix pins. Full suite 48/48. Substrate-tier 5-agent review + fix pass (driver-reporter dedup-off, broaden catch to `std::exception`, death-test relabel + real addFile guard, parseAndAdd_ empty-check fold, stale-comment fixes). `_pending_` (initial; hash filled post-merge per housekeeping convention). |
| CU1 | ✅ done | `CompilationUnit` + `UnitBuilder` + `CrossTreeRef` + `CompilationUnitId` — the bare type (`src/analysis/compilation_unit/`). CU is move-only / single-use; `UnitBuilder` is non-copyable + non-movable with `addTree(Tree&&)` as its only mutator (`addFile`/`addInMemory` = CU2). Trees stored `vector<Tree>` by value, frozen post-`finish()` (L1); `DiagnosticReporter` by value (L3); `crossRefs()` empty until CU4 (L5, with a `LANDMARK(CU4)` tripwire test); process-global `nextId()` atomic counter mirroring `TreeBuilder::nextTreeId` (L2). Empty + single-tree CUs both valid. Out-of-line `~UnitBuilder()` / `~CompilationUnit()` are load-bearing under `-fno-keep-inline-dllexport` (an implicit dtor of a dllexport class isn't emitted into the DLL → `STATUS_ENTRYPOINT_NOT_FOUND` for dllimport consumers). 13 tests (9 contract + 4 death: moved-from `schema()`, double-`finish()`, `addTree`-after-`finish()`, null schema) + 3 `CompilationUnitId` strong-id tests; full suite **48/48** green. Substrate-tier 5-agent review + fix-everything pass (moved-from `schema()` null-guard mirroring `Tree::schema`, move-assignment test, CU4 tripwire marker, plan-wording reconcile). `_pending_` (initial; hash filled post-merge per housekeeping convention). |
