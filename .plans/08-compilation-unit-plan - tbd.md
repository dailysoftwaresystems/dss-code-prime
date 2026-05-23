# Compilation Unit Phase — Sub-Plan

> Bridges phase #7 (`analysis-syntactic`) and phase #8 (`analysis-semantic`). Introduces the **`CompilationUnit`** — the bundle of trees + driver-resolved cross-file references that the semantic phase, IR, and codegen all consume. Closes production-readiness gaps **G-110** + **G-111**.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟢 **CU1 shipped** (`CompilationUnit` + `UnitBuilder` + `CrossTreeRef` + `CompilationUnitId`; 13 tests, full suite 48/48 green; substrate-tier 5-agent review + fix pass landed). CU2–CU4 ⏳ planned; CU5/CU6 v1.x. v1 production-critical; gates phase #8 (`analysis-semantic`). Production-readiness G-110 + G-111. |
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
| CU2 | Multi-file `UnitBuilder` + driver wiring        | `UnitBuilder::addFile` → tokenize → parse → push `Tree` into the CU. Driver diagnostics carry per-file `SourceBuffer` reference so the renderer can show the right file/line. CompilationUnit hands a `std::span<Tree const>` to consumers. Per-file order preserved (matters for some languages' include semantics). |
| CU3 | Cross-tree `SymbolId` shape + `CrossTreeRef`    | The contract semantic-phase consumes. `SymbolId` becomes CU-scoped (not tree-scoped). `NodeAttribute<SymbolId>` keys against the owning Tree's id; the symbol table's storage moves to the CU. Ships **the SH3-analog cross-CU `NodeId` guard** (deferred from CU1 per §2.5) — death tests for `NodeAttribute<SymbolId>` mismatches against other CUs (cross-CU NodeId leaks). |
| CU4 | Per-language import-resolution shim             | `ImportResolver` interface; built-in implementations per shipped language: **toy** = identity (no imports); **c-subset** = `#include "x.h"` resolves against the project's include-paths (v1 = same directory + project-config-declared include dirs; full system-header resolution post-v1); **tsql-subset** = either (a) concat-order (sqlcmd `:r`) or (b) cross-statement DB.schema reference (driver-injected from project config). Resolution populates the CU's `crossRefs` vector before semantic gets it. |
| CU5 | **Multi-language CU support** (v1.1)              | Per the universal-compiler decisions in [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1 (rev 2): a single CU can contain files of **different source languages** when all converge on the same artifact via [HIR](./09-hir-plan%20-%20tbd.md). Use case: a project with `.c` (c-subset) files calling FFI exports of `.sql` (tsql-subset) sprocs, compiled to one CLI binary. Per-file schema attached to each `Tree`; HIR lowering dispatches on each file's source language; HIR-level cross-references resolve uniformly. **v1.1 deliverable** — not blocking v1. Open question: when the source languages disagree on artifactProfile, which wins? Default: per-file profiles aggregated by an artifact-policy attached to the project config. |
| CU6 | **Cross-CU references** (v1.x)                    | Lifts the v1 single-CU-per-binary assumption: a `CompilationUnit` can hold typed references to symbols defined in *another* `CompilationUnit`. Use cases: (a) **shared libraries / DLLs** — each lib is a CU; the executable's CU links against them at the symbol level; (b) **incremental compilation** — each translation unit becomes a CU; the linker combines them. Builds additively on `CompilationUnitId` provenance (CU1 L2) and the SH3-analog cross-CU `NodeId` guard (CU3 D3) — both already exist by the time CU6 lands, so CU6 introduces no retroactive substrate change. **Couples with** [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) **LK11** (cross-CU linking, also v1.x — see linker §2.12 for the LK11 scope, sequencing, and out-of-scope notes). **v1.x deliverable — trigger:** the first artifact profile that needs multiple CUs in one image (`lib` or `staticlib` consumed by a separate `cli`, or incremental rebuild scope) enters real scope. Until then, the v1 single-CU contract holds and CU6's substrate hooks (id + guard) sit idle but harmless. |

### 2.4 What's NOT in this phase

| Out of scope                                | Why                                                                                              |
|---------------------------------------------|--------------------------------------------------------------------------------------------------|
| Symbol table construction                   | Phase #8 — CU3 ships only the SHAPE the symbol table consumes.                                   |
| Type inference / checking                   | Phase #8.                                                                                        |
| Schema-declared import syntax (v3 candidate)| CU4 hardcodes resolution per shipped language. A future schema field (`imports.syntax: "C-include"` etc.) is v3 work — see [`v2-gap-catalog - tbd.md`](./v2-gap-catalog%20-%20tbd.md) row 28. |
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
| D2 | `D_*` driver-level diagnostic codes (`D_FileNotFound`, `D_SchemaLoadFailed`, …) at hex `0xD001+` | **CU2** | The driver-level `DiagnosticReporter` slot exists in CU1 (empty); CU1 emits no `D_*` codes because it has no I/O paths or load operations. CU2's `addFile` is the first thing that can fail at the driver layer (file-not-found, schema-load-failure forwarding). The `0xD000`–`0xDFFF` block is reserved by convention but not populated. |
| D3 | SH3-analog cross-CU `NodeId` guard (i.e. `NodeId.cuTag` or membership-based enforcement) | **CU3** | The SH3 cross-tree guard exists because `NodeAttribute<T>` binds to a specific `TreeId`. The cross-**CU** version bites only when `NodeAttribute<SymbolId>` becomes CU-scoped — which IS CU3's contract change. CU1 has no cross-CU operation surface yet, so the guard would protect nothing. CU1 ships only **lifecycle-level** fatals (operating on a moved-from CU; calling `UnitBuilder::finish()` twice). |
| D4 | `CrossTreeRef` vector **population** | **CU4** | The `ImportResolver` interface that materializes cross-tree edges lives in CU4. CU1 ships the `CrossTreeRef` struct + empty vector; CU3 wires the semantic-phase consumption contract; CU4 produces the data. |
| D5 | Entry-point flag (which `Tree` is the `cli` artifact's entry) | **CU2** (§4 Q3 default: first file added) | CU1's trees are unordered from the CU's perspective; the "first file added is the entry" heuristic only becomes meaningful when `addFile` exists. Until then no caller can express "this one is the entry." |
| D6 | Multi-language CUs (different `.lang.json` per tree in one CU) | **CU5** (v1.1) | Already plan-stated. Each `Tree` already carries its own `shared_ptr<GrammarSchema const>`, so CU1's by-value tree storage does not foreclose CU5; the CU-level `schema()` accessor stays the homogeneous-case convenience. |
| D7 | Cross-CU operations of any kind (references between two CUs) | **CU6** (this plan, v1.x — paired with [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) **LK11** per its §2.12) | v1 ships single-CU binaries. CU1's `CompilationUnitId` provenance + CU3's cross-CU `NodeId` guard exist so CU6/LK11 land additively, with no retroactive substrate change. Trigger: first artifact profile needing multiple CUs in one image. Until then, the v1 single-CU contract holds. |
| D8 | `artifactProfile` handles on the CU | **`CompilationContext`** ([06](./06-artifact-profile-plan%20-%20tbd.md) AP3) | §2.2 already states "intentionally artifact-profile-agnostic — the profile lives on `CompilationContext`." Earlier draft text in §2.3 erroneously listed "artifactProfile handles" in CU1; corrected — the CU itself never holds an `artifactProfile`. |

---

## 3. Acceptance criteria

- [ ] `UnitBuilder` aggregates ≥3 files into one `CompilationUnit`; each `Tree` keeps its `BufferId` linkage so diagnostics render correct file paths.
- [ ] c-subset two-file integration test: `main.c` calls `helper()` declared in `helper.h`; `CompilationUnit.crossRefs()` contains the resolved edge.
- [ ] tsql-subset two-file integration test: `schema.sql` + `data.sql` concatenate cleanly into one CU; sequential cross-statement references resolve.
- [x] **(CU1)** toy single-file CU works; empty (zero-tree) CU is valid too. *Multi-file aggregation (≥3 files) lands in CU2 via `addFile`.*
- [x] **(CU1)** `CompilationUnit` is move-only, single-use (built by `UnitBuilder::finish()`, consumed by phase #8). Death tests pin the contract: reading `schema()` on a moved-from CU, double-`finish()`, `addTree` after `finish()`, null schema.
- [x] **(CU1)** Per-PR 5-agent review cadence (substrate tier) executed for CU1 (correctness / type-design / silent-failure / test-coverage / simplicity) + fix-everything pass. CU3 also substrate tier; feature tier for CU2 + CU4.
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

---

## 7. PR landing log

| PR | Status | Commit / notes |
|---|---|---|
| CU1 | ✅ done | `CompilationUnit` + `UnitBuilder` + `CrossTreeRef` + `CompilationUnitId` — the bare type (`src/analysis/compilation_unit/`). CU is move-only / single-use; `UnitBuilder` is non-copyable + non-movable with `addTree(Tree&&)` as its only mutator (`addFile`/`addInMemory` = CU2). Trees stored `vector<Tree>` by value, frozen post-`finish()` (L1); `DiagnosticReporter` by value (L3); `crossRefs()` empty until CU4 (L5, with a `LANDMARK(CU4)` tripwire test); process-global `nextId()` atomic counter mirroring `TreeBuilder::nextTreeId` (L2). Empty + single-tree CUs both valid. Out-of-line `~UnitBuilder()` / `~CompilationUnit()` are load-bearing under `-fno-keep-inline-dllexport` (an implicit dtor of a dllexport class isn't emitted into the DLL → `STATUS_ENTRYPOINT_NOT_FOUND` for dllimport consumers). 13 tests (9 contract + 4 death: moved-from `schema()`, double-`finish()`, `addTree`-after-`finish()`, null schema) + 3 `CompilationUnitId` strong-id tests; full suite **48/48** green. Substrate-tier 5-agent review + fix-everything pass (moved-from `schema()` null-guard mirroring `Tree::schema`, move-assignment test, CU4 tripwire marker, plan-wording reconcile). `_pending_` (initial; hash filled post-merge per housekeeping convention). |
