# HIR — High-level IR Sub-Plan

> The **pivot layer** between source-language CST and every downstream lowering. Language-neutral, structured, fully typed. Every transpile target, every native codegen path, and every shader/GPU/WASM backend lowers *from* HIR — never directly from the source-language CST.
>
> Decision rationale anchored in `00-master.md` §9: a single SSA-over-CFG IR cannot serve both binary codegen *and* source-to-source transpilation cleanly. HIR preserves structured control flow + typed expressions + language extensions; lower-level IRs ([MIR/LIR](./12-mir-lir-plan%20-%20tbd.md), [SPIR-V](./17-shader-gpu-plan%20-%20tbd.md), [WASM](./18-wasm-plan%20-%20tbd.md)) are downstream sinks. Transpilation ([plan 10](./10-source-translation-plan%20-%20tbd.md)) is HIR→HIR via a language-pair map.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟡 **in progress.** HR1 ✅ (substrate-tier: arena + node shapes + walker + ids + extension registry — landed 2026-05-26 on `feature/hir-1`). HR2–HR11 still to do. v1 production-critical; the single largest substrate addition in v1. |
| Predecessors  | ⏳ [`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) (HIR is CU-scoped). ⏳ [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) (HIR uses the generalized arena substrate + core type lattice). ⏳ Phase #8 semantic (HIR consumes `NodeAttribute<TypeId>` + `NodeAttribute<SymbolId>` from the CST). |
| Successors    | [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md) walks HIR. [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md) lowers HIR→MIR. [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) lowers shader-shaped HIR→SPIR-V. [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md) lowers HIR (with structured-CF markers from MIR) to WASM bytecode. [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) emits extern decls as HIR nodes. |
| Scope         | **Bounded.** HR1 arena + node shapes. HR2 typed expressions. HR3 structured CF. HR4 declarations (functions / globals / types / externs). HR5 attribute system (shader intrinsics, FFI ABI metadata, source-position preservation). HR6 verifier. HR7 text format. HR8 **config-driven** CST→HIR lowering engine (one language-agnostic driver reading each language's `hirLowering` config block — NOT per-language C++; see §2.5 + thesis decision #4). |

---

## 1. Motivation

A single-level IR cannot serve transpilation, WASM, and shader codegen cleanly: transpilation needs preserved types + structured CF; WASM requires structured CF natively (no Relooper); SPIR-V requires SSA-shaped structured CF + shader restrictions. HIR is the layer where all three converge — typed expressions, structured CF, attribute side-tables for stage / FFI / transpile metadata. Lower-level IRs ([MIR/LIR](./12-mir-lir-plan%20-%20tbd.md), [SPIR-V](./17-shader-gpu-plan%20-%20tbd.md), [WASM](./18-wasm-plan%20-%20tbd.md)) are downstream sinks.

---

## 2. Design

### 2.1 Files

```
src/hir/
├── CMakeLists.txt
├── hir.hpp                       # Public Hir type (frozen) + builder handoff
├── hir_node.hpp                  # detail::HirNode POD + HirKind enum + HirFlags
├── hir_builder.hpp / .cpp        # CST→HIR lowering driver
├── hir_cursor.hpp / .cpp         # Walker (mirrors TreeCursor; reuses substrate)
├── hir_verifier.hpp / .cpp       # Structural invariant checker
├── hir_text.hpp / .cpp           # .dsshir round-trippable text format
├── hir_attrs.hpp                 # Per-attribute typedefs (sugar over ArenaAttribute<HirArena, T>)
├── attributes/
│   ├── source_span.hpp           # SourceSpan preservation (HirSourceMap)
│   ├── shader_intrinsics.hpp     # Per-node shader annotations
│   ├── ffi_metadata.hpp          # Calling-convention + linkage attrs
│   ├── transpile_hints.hpp       # Per-node target-language preference hints
│   └── ...
└── lowering/
    ├── cst_to_hir.hpp / .cpp     # The ONE language-agnostic lowering engine (reads `hirLowering` config)
    └── hir_lowering_config.hpp   # Parsed `hirLowering` schema block (CST-shape → HIR mapping)

tests/hir/
├── test_hir_arena.cpp
├── test_hir_lowering_toy.cpp        # fixture: drive the engine over toy's config
├── test_hir_lowering_c_subset.cpp   # fixture: drive the engine over c-subset's config
├── test_hir_lowering_tsql.cpp       # fixture: drive the engine over tsql's config
├── test_hir_lowering_generic.cpp    # genericity: synthetic schema → proves no language assumptions leaked
├── test_hir_verifier.cpp
└── test_hir_text_format.cpp
```

### 2.2 The HIR shape

`HirKind` is an **open enum — mirroring the core type lattice's `TypeKind` model (SP2)**: a fixed **universal core** in `[0, 256)` plus a **registered-extension space** `≥ 256`. The core is the paradigm-neutral, structured-typed-imperative vocabulary that composes to represent the vast majority of language constructs; anything a core kind can't express is a **registered extension kind** (declared per language/domain in a `HirKindRegistry`, mirroring `TypeRegistry`), **never a new hardcoded core member**. This is what makes HIR generic enough to represent **any** source language without growing the core — the same core+extensions discipline that lets the type lattice carry C++ `MemberPtr`, C# `Delegate`, TSQL `RowType`, and VHDL `Std_Logic` without bloating the primitive set.

**Core groups (`[0,256)` — universal, fixed):**

| Group | Kinds |
|---|---|
| **Modules / Declarations** | `Module`, `Function`, `Global`, `TypeDecl`, `ExternFunction`, `ExternGlobal`, `ImportGroup` |
| **Structured Statements** | `Block`, `IfStmt`, `WhileStmt`, `DoWhileStmt`, `ForStmt`, `SwitchStmt`, `CaseArm`, `BreakStmt`, `ContinueStmt`, `ReturnStmt`, `ExprStmt`, `VarDecl`, `AssignStmt` |
| **Expressions** | `Literal`, `Ref` (read symbol), `Call`, `IntrinsicCall`, `BinaryOp`, `UnaryOp`, `Cast`, `MemberAccess`, `Index`, `Swizzle`, `ConstructAggregate`, `Ternary`, `LogicalAnd`, `LogicalOr`, `SizeOf`, `AddressOf`, `Deref` |
| **Types-as-values** | `TypeRef` (carries a `TypeId` from the lattice) |
| **Special** | `Unreachable`, `Error` (recovery sentinel — analog of CST's Error leaf) |

**Extension kinds (`≥ 256` — registered per language/domain, NOT core):** shader (`WorkgroupBarrier`, `DerivativeX`/`Y`, `TextureSample`, `TextureLoad`, `ImageStore`, `AtomicOp`), SQL (`Query`, `DmlInsert`, `DmlUpdate`, `DmlDelete`, `DdlCreate`, `Cte`, `QueryBlock`), and any future domain. Each is registered by the language/domain that needs it (carrying its operand/attribute shape + which backends consume it); a backend that doesn't recognize an extension kind emits `H_UnsupportedKindForBackend` — never a silent miscompile. The core engine, verifier, and every backend are written against the core + the registry, never against a hardcoded shader/SQL kind.

Extension HIR kinds are **declared per-language** in the schema (the `hirLowering` block, schema v4 — additive sibling of `semantics`/`imports`/`numberStyle`/`typeExtensions[]`), mirroring how shader/SQL extension *types* are declared via `typeExtensions[]` per [08.5 SP2](./08.5-substrate-prep-plan%20-%20tbd.md). Both follow the open core + per-language registered-extensions pattern.

**Generic enough to "support everything":** paradigms beyond the structured-imperative core — OOP dispatch (vtables/interfaces), closures + captures, algebraic data types + pattern matching, exceptions (`TryStmt` reserved), async/coroutines, GC reference types — are representable as **core compositions + registered extension kinds + attribute side-tables**, so a new language onboards by registering kinds, not by editing the core. The single deliberate boundary: a **fundamentally non-imperative paradigm** (concurrent + signal-typed hardware) does NOT force into this HIR — it gets the reserved sibling IR [`19-hir-hw-reserved-plan`](./19-hir-hw-reserved-plan%20-%20tbd.md). One IR cannot honestly model both software and hardware; "support everything" = core + extensions for the software family, + a sibling IR where the paradigm genuinely differs.

Each `HirNode` POD carries:
```cpp
struct HirNode {
    HirKind kind;
    HirFlags flags;          // bit flags: HasError, Synthetic, ShaderUsable, HostUsable, …
    TypeId typeId;           // every expression node carries its resolved type
    uint32_t childStart;     // index into child-id array
    uint32_t childCount;
    uint32_t payload;        // operator-id / literal-index / intrinsic-id, etc.
    // SourceSpan + per-node attributes live in side-tables.
};
```

The HIR **node POD** is small (~24 bytes) so a million-node module stays manageable. Per-node side-tables (`HirAttribute<SourceSpan>`, `HirAttribute<FfiMetadata>`, `HirAttribute<ShaderIntrinsicData>`) carry everything that's not universal.

### 2.3 Structured CF discipline

HIR control flow is **always structured**: every `IfStmt` has a then-block (and optional else-block); every loop has a body block; every `BreakStmt`/`ContinueStmt` references the nearest enclosing loop or switch by index (`break 0` = innermost; `break 1` = one out, matching WASM semantics). There is **no `goto`**.

Languages whose CST permits goto (none yet shipped, but full C99 eventually) get lowered to structured form during CST→HIR; the lowering may emit synthetic `IfStmt` / loop scaffolding to preserve semantics. This is the "no Relooper later" payoff.

### 2.4 Typed expressions

Every expression node carries a `TypeId` resolved from the core type lattice (per `08.5-substrate-prep-plan` SP2). Untyped HIR is rejected by the verifier — semantic phase #8 is the prerequisite. Lowering populates `typeId` per node during CST→HIR; any failure produces an `H_TypeUnresolved` diagnostic and an `Error` HIR node.

### 2.5 Config-driven lowering (NOT per-language C++)

> **Per master-plan thesis decision #4, CST→HIR lowering is config-driven — there is no per-language lowering C++ and no branch on `schema.name()`.**

Each shipped `.lang.json` declares a **`hirLowering` block** (additive schema facet, v4 — sibling of the `semantics` block from [`08.6-semantic-plan`](./08.6-semantic-plan%20-%20tbd.md)) mapping CST shapes to HIR: `cstRule`/`tokenKind` → `HirKind`, child-role → HIR operand, type-expression → lattice constructor, symbol → intrinsic. A **single language-agnostic `CstToHirLowering` engine** reads that block + the semantic-populated `UnitAttribute<TypeId>` / `UnitAttribute<SymbolId>` and emits HIR for **any** language. A new language lowers by adding a `hirLowering` block — **no engine edits**. When the vocabulary can't express a construct, extend it additively (as schema v2 did for grammar); never language-branch the engine. An incomplete map fails loud with `H_UnsupportedLoweringForKind` — never a silent skip.

For a multi-language CU (per CU5 in `08-compilation-unit-plan`), each file's CST runs through the same engine reading that file's language's `hirLowering` config; the resulting HIR modules merge into one HIR program in the CU. Symbol resolution across files happens at semantic time (CU3); HIR only sees pre-resolved cross-file `Ref` nodes.

### 2.6 Attribute system

The same `ArenaAttribute<HirArena, T>` substrate (SP1) gives HIR a side-table mechanism identical to `Tree`'s. Standard side-tables:

| Attribute | Carries | Populated by |
|---|---|---|
| `HirAttribute<SourceSpan>` | Byte span back to the originating source buffer | CST→HIR lowering, preserved through MIR/LIR per `15-debug-info-plan` |
| `HirAttribute<FfiMetadata>` | Calling convention, linkage, name-mangling rule, imported library | FFI ingestion (`11-ffi-plan`), HIR `ExternFunction` nodes |
| `HirAttribute<ShaderIntrinsic>` | Stage hint (vertex/fragment/compute), workgroup dimensions, binding indices | Shader lowering (`17-shader-gpu-plan`) |
| `HirAttribute<TranspileHint>` | Per-node target-language preference (e.g. "this `Index` should map to JS `[]` not C-style pointer-arithmetic") | Transpile pass (`10-source-translation-plan`) consumers; populated by source language's lowering |
| `HirAttribute<DiagnosticInfo>` | Per-node recovery info if `HasError` flag set | CST→HIR lowering on broken paths |

### 2.7 Text format `.dsshir`

Round-trippable. Same discipline as MIR/LIR text formats per `12-mir-lir-plan`. Verifier validates on load; text format is the debug + test-fixture surface.

Skeleton example (toy `let x = 1 + 2;`):
```
module toy "single.toy" {
  function main () -> void {
    block {
      var_decl %x : i64 = binary_op<Add> (literal 1 : i64) (literal 2 : i64)
    }
  }
}
```

### 2.8 Verifier

Structural invariants (fail-loud per `H_*` codes):

- Every expression node has `typeId.valid()`.
- Every `Block` body terminates structurally (no fall-through past `Return` / `Unreachable`).
- Every `BreakStmt`/`ContinueStmt` index references an enclosing loop/switch.
- Every `Call`'s argument types match the callee's `FnSig`.
- Every `IntrinsicCall` references a registered intrinsic in the lattice extension registry.
- Shader-flagged subtrees pass the shader-restriction verifier (no recursion, no libc calls, no dynamic alloc).
- HIR is immutable post-`finish()` — same handoff pattern as `Tree`.

---

## 3. PR breakdown

| PR  | Title                                    | Scope |
|-----|------------------------------------------|-------|
| HR1 | HIR arena + node shapes ✅                | Instantiate `ArenaContainer` with `HirNode` POD; ship the **open `HirKind`** (core `[0,256)` + `HirKindRegistry` for extension kinds `≥256`, mirroring `TypeKind`/`TypeRegistry`) + `HirFlags`; basic walker (`HirCursor`); ID strong-typing (`HirNodeId`, `HirModuleId`, `HirKindId`). **Landed 2026-05-26** on `feature/hir-1` — POD kept small (28B) with parent links in a parallel `Hir::parentOf_` array so kind/type sweeps stay cache-dense; `HirKindDescriptor` uses the passkey idiom to gate construction to the registry; custom move ops on `Hir` make moved-from observably empty (mirrors `ArenaAttribute` discipline). All HR1 acceptance criteria (cross-module guard, open-core composes via `HirKindRegistry`, `HirAttribute<T>` binds, `[[nodiscard]]` movement) covered by 5 test executables + a `Synth::Widget` genericity proof. |
| HR2 | Typed expressions                        | Literal / Ref / BinaryOp / UnaryOp / Call / Cast / MemberAccess / Index / IntrinsicCall. Verifier asserts every expr has `typeId.valid()`. |
| HR3 | Structured CF                            | Block / IfStmt / WhileStmt / DoWhileStmt / ForStmt / SwitchStmt / Break / Continue / Return / ExprStmt / VarDecl / AssignStmt. Verifier asserts every break/continue references a valid enclosing scope. |
| HR4 | Declarations + extern surface             | Module / Function / Global / TypeDecl / ExternFunction / ExternGlobal / ImportGroup. FFI metadata side-table populated. |
| HR5 | Attribute system + side-tables           | SourceSpan, FfiMetadata, ShaderIntrinsic, TranspileHint, DiagnosticInfo. |
| HR6 | Verifier                                 | All structural invariants; `H_*` diagnostic codes; shader-restriction subverifier. |
| HR7 | Text format `.dsshir`                    | Round-trippable; verifier integration on load. |
| HR8 | CST→HIR lowering — toy                   | First language. End-to-end: parse toy → semantic → CST→HIR → verifier passes. |
| HR9 | CST→HIR lowering — c-subset              | Most complex CST shape (pointers, declarators, struct/union/enum when they land). |
| HR10| CST→HIR lowering — tsql-subset           | SQL-shaped HIR nodes (Query, DmlInsert, etc.). Pins the per-language extensibility story. |
| HR11| Multi-language CU lowering               | Per CU5 from `08-compilation-unit-plan`. One CU → one HIR program with per-file lowering. |

Substrate tier (5-agent review) for HR1, HR2, HR3, HR5, HR6 (touch substrate contracts).

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Is `HirArena` per-CU or per-function? | **Per-CU** — one HIR module per CU. Per-function would shred attribute side-tables across thousands of arenas. |
| 2 | Does HIR carry source-language identity per-node, or is it fully neutral? | **Fully neutral.** The language is recorded at the `Module` level via `Module.sourceLanguage`; downstream consumers that need per-node language info (rare) read the Module-level field. |
| 3 | Are SQL-shaped nodes a first-class HIR kind, or a lattice extension? | **First-class iff a v1-or-v1.x shipped language uses the construct.** SQL (`Query`/`DmlInsert`/`Cte`) is first-class because tsql-subset is v1-shipped. Shader nodes (`WorkgroupBarrier` etc.) are first-class because shader-shape lowering ships in v1.x via `17-shader-gpu-plan`. HDL nodes get a **sibling IR** (`19-hir-hw-reserved-plan`), not HIR membership — concurrent / signal / clock semantics don't unify with sequential HIR. **Shader resource types** (`Sampler`/`Texture<>`/`UAV<>` etc.) are **lattice extensions** registered by shader-shape language schemas (per `08.5-substrate-prep-plan` §2.2) — extension types stay narrow + opt-in, while HIR kinds stay universal-visible. |
| 4 | Does HIR keep CST node references, or fully detach? | **Fully detach** — only `HirAttribute<SourceSpan>` survives. CST `NodeId`s would tie HIR to the CST's lifetime; HIR consumers want lifetime independence (transpile produces a target HIR that never had a CST). |
| 5 | Does HIR have generics / templates as first-class? | **First-class generic placeholders** (`TypeRef` to a `Param<name>` lattice node); instantiation is the lowering's responsibility (lower a generic function once per concrete arg list). Recursive instantiation is verifier-bounded. |
| 6 | Does HIR carry phi nodes? | **No** — phi belongs to MIR (SSA). HIR is pre-SSA, expression-tree-shaped. |
| 7 | What about exception handling? | **HIR-level `TryStmt` reserved**; v1 shipped languages don't use it. Lowering to MIR is via two-color edges (normal + unwind). Defer to plan 12 if a v1 language needs it. |
| 8 | Diagnostic namespace? | `H_*` codes. `H_TypeUnresolved`, `H_InvalidBreak`, `H_UnknownIntrinsic`, `H_ShaderViolation`, `H_VerifierFailure`. Config-load errors in `hirLowering` use the standard `C_*` band (mirror of `imports` / `numberStyle` / `wrapperRules` loader codes) — `H_*` is reserved for verifier / lowering-time failures only. |

---

## 5. Acceptance criteria

- [ ] All three shipped languages (toy, c-subset, tsql-subset) lower their full corpus from CST → HIR cleanly via `Parser::parse → SemanticPass → CstToHir`.
- [ ] HIR verifier rejects every malformed input enumerated in §2.8 with a specific `H_*` code.
- [ ] HIR text format round-trips: emit a HIR module → parse it → emit again → byte-identical.
- [ ] HIR is immutable post-`finish()`; death tests prove cross-CU `HirNodeId` misuse aborts (analog of SH3 `treeTag` discipline).
- [ ] Shader-restriction verifier rejects (with `H_ShaderViolation`) every disallowed construct (recursion, dynamic alloc, fn-ptr) when a function carries the `ShaderUsable` flag.
- [ ] Multi-language CU lowering: a c-subset `.c` and a tsql-subset `.sql` in one CU produce one HIR program with both functions present and cross-references resolved.
- [ ] Source-position preservation: every emitted MIR/LIR/bytes instruction (downstream) traces back through HIR `HirAttribute<SourceSpan>` to the originating CST.

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| HirKind core bloats as languages onboard | High | Low | **Resolved by the open core+extensions model (§2.2):** new constructs are *registered extension kinds* (`≥256` via `HirKindRegistry`), not core enum members. The core `[0,256)` stays fixed; the registry absorbs all growth, exactly as `TypeKind`/`TypeRegistry` do. Discipline still applies to *core* additions (verifier rule + lowering coverage + a downstream consumer), but domain/language kinds never touch the core. |
| Extension kinds force backends to handle nodes they'll never emit | Medium | Low | A backend that doesn't recognize an extension kind emits `H_UnsupportedKindForBackend` at lowering entry (verifier sub-check); it never silently miscompiles. Each extension kind declares the backends that consume it, so coverage is checkable. |
| Structured-CF discipline (§2.3) blocks fast lowering for c-subset's `goto` (post-v1 full C99) | Low | Medium | Synthetic-scaffold pattern documented; goto-rich lowering deferred to v1.x when full C99 lands. |
| Generic instantiation balloons HIR size | Low | Medium | Instantiation cache keyed on `(generic, concrete-args-tuple)`; instantiation budget per CU (open question §5 default: 1000). |

---

## 7. Sequencing

```
phase #8 (semantic) ─► HR1 ─► HR2 ─► HR3 ─► HR4 ─► HR5 ─► HR6 ─► HR7
                                                                       │
                                            ┌──────────────────────────┼──────────────────────┐
                                            ▼                          ▼                      ▼
                                          HR8 (toy)               HR9 (c-subset)        HR10 (tsql-subset)
                                                                       │
                                                                       ▼
                                                                     HR11 (multi-lang CU)
                                                                       │
                                                                       ▼
                                          ┌─────────────┬───────────────┼────────────────┐
                                          ▼             ▼               ▼                ▼
                                  10-source-trans    12-mir-lir   17-shader-gpu    18-wasm
                                  (HIR→HIR)          (HIR→MIR)     (HIR→SPIR-V)    (HIR→WASM)
```

HR1–HR7 are sequential within the substrate. HR8/HR9/HR10 are parallel per language. HR11 depends on all three plus `08-compilation-unit-plan` CU5. Once HR11 ships, every downstream sink (transpile / MIR / SPIR-V / WASM) is unblocked.
