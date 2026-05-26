# Source-to-source Translation — Sub-Plan (10)

> Owns **language-pair `.map.json`** schema + **HIR→HIR translation pass** + **target-CST construction** + **target-schema-aware pretty-printer**. Lets the compiler turn any configured source language into any configured target language, via [HIR](./09-hir-plan%20-%20tbd.md) as the lossless pivot. Promoted from `00-master` §9 long-running note in rev 2.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1.x — first user likely c-subset → JS for a Web target. Reserved scope today; design lands now to keep HIR honest. |
| Predecessors  | 🟡 [`09-hir-plan`](./09-hir-plan%20-%20tbd.md) (the pivot layer — HR1–HR4 ✅ 2026-05-26, HR5–HR11 pending). ⏳ [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) (core type lattice). |
| Successors    | Used by [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) for SPIR-V→DXIL/MSL/WGSL paths. Reserved consumer of [`19-hir-hw-reserved-plan`](./19-hir-hw-reserved-plan%20-%20tbd.md) for VHDL/Verilog emission. |
| Scope         | **Bounded.** ST1 schema. ST2 HIR→HIR translator. ST3 target-CST builder. ST4 target-schema pretty-printer. ST5 shipped language-pair maps (at least one v1.x flagship). |

---

## 1. Motivation

A new artifact-profile-style mechanism — `transpile` — selects this backend instead of native codegen. Avoids the IR-codegen lowering chain entirely.

Walking the CST and emitting target text is lossy (type info, scope info, semantic-resolved structure are gone). Walking SSA-over-CFG is wrong-shape (idiomatic target text isn't recoverable from SSA). HIR is the right pivot — types resolved, scopes flattened, control flow recognizable.

---

## 2. Design

### 2.1 Files

```
src/transpile/
├── transpile.hpp                 # Public entry point
├── language_pair.hpp / .cpp      # Loads .map.json into a LanguagePairMap
├── hir_to_hir.hpp / .cpp         # ST2: HIR(source) → HIR(target) walker
├── hir_to_cst.hpp / .cpp         # ST3: HIR(target) → target CST construction
├── cst_pretty_print.hpp / .cpp   # ST4: target schema-aware text emitter
└── diagnostics.hpp               # T_* codes

src/source-config/language-pairs/
├── c-subset-to-javascript.map.json
├── c-subset-to-csharp.map.json
└── ... (per-pair authoring per ST5)

tests/transpile/
├── test_language_pair_loader.cpp
├── test_hir_to_hir_translation.cpp
├── test_target_cst_construction.cpp
├── test_pretty_printer.cpp
└── corpus/                       # Per-pair round-trip tests
```

### 2.2 `.map.json` schema

A language-pair map declares **how source-language HIR kinds + types map to target-language HIR kinds + types**. Loaded similarly to `.lang.json` (per `GrammarSchema::loadShipped` precedent).

Skeleton:
```jsonc
{
  "$schema": "language-pair-schema.json",
  "dssMapVersion": 1,

  "pair": {
    "source": "c-subset",
    "target": "javascript",
    "version": "0.1.0"
  },

  "typeMap": {
    "core": {
      "i32": "Number",
      "i64": "BigInt",
      "f32": "Number",
      "Ptr<T>":   { "kind": "ManualMapping", "note": "JS has no raw pointers — Ptr<T> maps to typed arrays or refs depending on context. See typeMap.ptrPolicy." }
    },
    "ptrPolicy": "TypedArray",
    "extensions": {
      "TSQL::Varchar<N>":  "string",
      "CSharp::Boxed<T>":  { "wrap": "Object", "via": "boxing" }
    }
  },

  "kindMap": {
    "BinaryOp.Add":     { "op": "+",  "associativity": "left",  "precedence": 13 },
    "Call":             { "kind": "JsCall" },
    "MemberAccess":     { "kind": "JsDot" },
    "IfStmt":           { "kind": "JsIf",  "elseRequired": false },
    "ForStmt":          { "kind": "JsFor" },
    "Return":           { "kind": "JsReturn" }
  },

  "idiomHints": {
    "preferEarlyReturn": true,
    "useStrict": true,
    "modulesAsEsm": true
  }
}
```

Loader produces a `LanguagePairMap` C++ object; missing kindMap entries → fatal `T_MissingKindMapping`; unknown source kinds → ditto; ambiguous mappings → `T_AmbiguousMapping`.

### 2.3 ST2 — HIR→HIR translator

Walks source-language HIR; emits target-language HIR using the pair map. Type mapping happens first (core lattice + extension mapping); then per-kind structural mapping; then idiom hints (early-return preference, prefix vs postfix conventions, etc.).

Translation is **lossy-by-explicit-declaration**: every HIR kind not covered by the pair map produces `T_KindNotMapped` and emits an `Error` node in the target HIR. The user-facing error points at the source span + the missing kindMap entry.

For shader languages, this is the SPIR-V → DXIL / MSL / WGSL path (reserved post-v1.x via `17-shader-gpu-plan`).

### 2.4 ST3 — HIR(target) → CST(target)

Each `.lang.json` provides an inverse mapping (`shapes.*` → emission templates). The target HIR is walked, and each HIR node is converted into a target-CST subtree using the target language's shape definitions. The target CST is fully schema-validated (same `GrammarSchema` machinery used for parsing).

For built-in target languages with no inverse mapping yet, the language schema gets a new optional `emissionTemplates[]` section (`dssSchemaVersion >= 3`):
```jsonc
"emissionTemplates": {
  "IfStmt":     "if ({{cond}}) {{then}}{{? else}} else {{else}}{{/if}}",
  "BinaryOp":   "{{lhs}} {{op}} {{rhs}}",
  "Call":       "{{callee}}({{args:,}})",
  "Function":   "function {{name}}({{params:,}}) {{body}}"
}
```

### 2.5 ST4 — pretty-printer

Walks the target CST + applies the target schema's lexical conventions (whitespace, indentation rules, semicolon insertion / no-insertion, blank-line policy). Output is the final text artifact.

Per-target style configurability via project config (post-v1: indent-width, brace-style, etc.).

### 2.6 Artifact profile integration

New artifactProfile value `transpile` (per [`06-artifact-profile-plan`](./06-artifact-profile-plan%20-%20tbd.md) §3 — added via the vocabulary unfreeze). The project config picks the target language + the pair-map filename:

```jsonc
{
  "language":        "c-subset",
  "artifactProfile": "transpile",
  "transpileTarget": "javascript",
  "languagePair":    "c-subset-to-javascript",
  "sources":         ["src/**/*.c"],
  "output":          "dist/myapp.js"
}
```

---

## 3. PR breakdown

| PR  | Title                                    | Scope |
|-----|------------------------------------------|-------|
| ST1 | `.map.json` schema + loader              | Loader produces `LanguagePairMap`; `T_*` codes for malformed maps. Mirrors `GrammarSchema::loadShipped` discipline. |
| ST2 | HIR→HIR translator                       | Walk source HIR, emit target HIR per pair map. Type mapping + kind mapping + idiom hints. |
| ST3 | HIR→CST(target) builder                  | New `emissionTemplates[]` section on `.lang.json`; HIR walker drives target-CST construction. |
| ST4 | Target-schema pretty-printer             | Walk target CST + apply lexical conventions; produce final text. |
| ST5 | First flagship pair: c-subset → JavaScript | End-to-end test: parse c-subset corpus → HIR → translate → JS HIR → JS CST → text. Output runs under Node + matches host c-subset binary output. |
| ST6 | Round-trip discipline                    | Compile X via the same compiler to native AND via transpile to language Y. Then re-parse the Y output with the Y schema; convert Y CST → Y HIR; diff against the translated HIR. Helps catch lossy translations. |

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Per-pair map authoring effort scales as N². How do we control? | **Hub-and-spoke**: every language pairs with HIR (the "core" hub) via its CST→HIR lowering. Transpile is HIR→HIR. We never write N² CST→CST maps; we write N CST↔HIR mappings. Pair-map is HIR-to-HIR vocabulary deltas only, not full grammar mapping. |
| 2 | What about partial translations (some constructs not mappable)? | **Hard-fail** by default; `T_KindNotMapped` is a fatal diagnostic. Project config can opt into `--allow-incomplete-transpile`, which emits stub functions for unmappable HIR and warns instead. |
| 3 | Idiom transformation depth? E.g. C `for(int i=0;i<n;i++)` → JS `for (let i = 0; i < n; i++)` versus → `Array.from({length:n}, (_, i) => ...)`. | Pair map declares **one** idiom per kind; secondary idioms post-v1 via `pair-pluralism` reserved feature. |
| 4 | Type-mapping completeness — does every source extension need a target mapping? | **Yes**, or `T_ExtensionNotMapped`. Forces explicit decisions; no silent any-typing. |
| 5 | Is the target-CST roundtripped through the target schema's parser as a verification step? | **Yes** — final acceptance gate. Generated text must re-parse cleanly with the target `.lang.json`. |
| 6 | Diagnostic codes? | `T_*` namespace: `T_MissingKindMapping`, `T_AmbiguousMapping`, `T_KindNotMapped`, `T_ExtensionNotMapped`, `T_IdiomConflict`, `T_TargetSchemaReparse`. |

---

## 5. Acceptance criteria

- [ ] `.map.json` files load via the schema-validating loader with `T_*` diagnostics on malformed input.
- [ ] First flagship pair (c-subset → JavaScript) round-trips: parse c-subset corpus → HIR → translate → JS HIR → JS CST → text → re-parse JS via Node + via our own JS schema; both succeed.
- [ ] Generated JS runs under Node and produces the same stdout/exit code as the native c-subset binary built by `14-linker-plan`.
- [ ] Type-mapping coverage: every core lattice member used by c-subset is mapped to a JS type via the pair map.
- [ ] Unmapped HIR kinds produce actionable `T_KindNotMapped` diagnostics with both source-span + map-file pointer.
- [ ] Hub-and-spoke discipline (§4.1): no pair-map directly references CST shapes; all mappings are HIR-vocabulary deltas.

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Per-pair authoring scales badly | Medium | Medium | Hub-and-spoke design (§4.1). Pair-map is HIR-to-HIR deltas, not full grammar mapping. |
| Lossy translation produces silently-wrong output | High | Critical | Hard-fail discipline (§4.2 default); `--allow-incomplete-transpile` is opt-in; round-trip discipline (ST6) catches drift. |
| Idiom mismatch — generated code is technically correct but jarring to human readers | High | Low | Pair-map ships with per-target idiom hints; user can override via project-config style block. |
| Target language's CST grammar can't accommodate a HIR construct (e.g. SQL HIR `Query` has no JS shape) | Medium | High | Per-pair map declares which HIR kinds are mappable; impossible kinds emit at HIR-verifier time, before transpile. |
| Pretty-printer formatting drift | Medium | Low | Round-trip discipline (re-parse generated text) catches structural drift; cosmetic drift acceptable. |

---

## 7. Sequencing

```
09-hir (HR11 multi-language) ─► ST1 ─► ST2 ─► ST3 ─► ST4 ─► ST5 (flagship) ─► ST6 (round-trip)
```

ST1 is loader-only; ST2/ST3/ST4 are sequential. ST5 pins the design with a real pair. ST6 is the verification harness.

Adjacent phases:
- [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) post-v1.x: SPIR-V→DXIL/MSL/WGSL is a transpile-style flow that may reuse this plan's mechanism (SPIR-V CST is unusual — likely treated as a target HIR variant).
- [`19-hir-hw-reserved-plan`](./19-hir-hw-reserved-plan%20-%20tbd.md) reserved: VHDL/Verilog emission via HIR-HW → target-CST → text reuses ST3/ST4.
