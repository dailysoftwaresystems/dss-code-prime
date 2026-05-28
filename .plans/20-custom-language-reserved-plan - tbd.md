# Custom Language Onboarding — Reserved Sub-Plan (20)

> **Reserved scope.** The user's stated final goal: "We will, after validating languages until full C++, C#, TSQL, SQLite support, have our own comprehensive language." Plus: "We will have GPU communication (for game development), using the exact same language of our own we'll create."
>
> This plan tracks the eventual user-authored language. The compiler substrate is built to make custom languages "just another `.lang.json`" — no new core work needed. This plan owns the **language design + standard library + custom-runtime decisions**, not the engine.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🔒 **reserved.** Triggered once v1 ships and C++ / C# / TSQL / SQLite validation is mature. |
| Predecessors  | ⏳ v1 ship per [`07-production-readiness-plan`](./07-production-readiness-plan%20-%20tbd.md). ⏳ Schema v3 (per [`06-artifact-profile-plan`](./06-artifact-profile-plan%20-%20tbd.md) + lattice extension support). ⏳ Mature C++/C# language configs (post-v1 work). |
| Successors    | First-class user of [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) (the same-source CPU+GPU goal). |
| Scope         | **Unspecified.** Design lands when triggered. |
| Mapped from elsewhere | **Schema-driven `allowFloat`** (from plan [12.5 §0.2 D3](./12.5-const-eval-plan%20-%20tbd.md)): if the custom language adopts non-IEEE float semantics (decimal float, fixed-point, saturating arithmetic), the `EvalOptions::allowFloat` knob — today opted in unconditionally by MIR-globals because every v1 shipped language is IEEE 754 — must become per-schema in `HirLoweringConfig`. The custom-language cycle that introduces non-IEEE semantics owns the substrate change. Until then no v1 schema needs it. |

---

## 1. Trigger

This plan opens when:
- The shipped languages (toy, c-subset, tsql-subset, eventually full c99 / c++20 / c# / sqlite) have validated the substrate's expressiveness, AND
- The user is ready to commit a custom-language vocabulary.

Until then: this plan exists to declare that **no new substrate work is required** — the custom language is "just another schema + lowering."

---

## 2. What the engine already gives the custom language

- **A schema-driven frontend.** Write a `.lang.json`; the same tokenizer + parser drives it.
- **A core type lattice + extension registry.** Per [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md). The custom language declares its types as either core members or extensions.
- **HIR as the pivot.** Lower CST → HIR via a custom-language-specific lowering pass.
- **Same-source CPU + GPU.** The HIR shader-shape mechanism (per [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) §2.4) makes `[[shader.usable]] [[host.usable]]` functions dual-lowering targets.
- **Multi-target codegen.** Native (3 OS × 2 arch via [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md) + [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) + [`14-linker-plan`](./14-linker-plan%20-%20tbd.md)), shader (SPIR-V), web (WASM via [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md)), transpile (any other configured language via [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md)).
- **Debug info, codesign, FFI.** All in-tree, all hermetic — same machinery the shipped languages use.

---

## 3. What this plan owns (when triggered)

1. **Language vocabulary** — syntax, semantics, the `.lang.json` itself.
2. **Standard library design** — built-ins (collections / IO / math / etc.), authored in the language itself with FFI extern decls to OS-supplied runtime.
3. **Runtime decisions** (deferred to [`21-runtime-reserved-plan`](./21-runtime-reserved-plan%20-%20tbd.md)):
   - GC (mark-sweep / refcount / static / hybrid)?
   - Exception model?
   - Coroutines / async?
   - Threading model + memory model?
4. **GPU integration** — which game-dev affordances live in the language vs. library (compute shaders, vertex pipelines, render-pass abstractions).
5. **Distribution + packaging** — module system, dependency resolution, registry.

---

## 4. Open questions (deferred)

| # | Question |
|---|----------|
| 1 | Static vs dynamic typing? |
| 2 | Memory management model (manual / GC / ARC / borrow checker)? |
| 3 | Concurrency model (threads / async / actors / data-parallel)? |
| 4 | Metaprogramming (macros / generics / reflection)? |
| 5 | First-class concepts for game development (entity-component-system, scene graph)? |
| 6 | Source-form authoring (text-only vs structural editing)? |
| 7 | Bootstrap path — does the custom language eventually rewrite its own compiler? |

All deferred until the plan opens.

---

## 5. Sequencing

Not sequenced. Reserved.
