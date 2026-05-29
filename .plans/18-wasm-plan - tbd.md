# WASM Backend — Sub-Plan

> Hermetic WebAssembly backend: HIR/MIR → `.wasm` bytecode + module structure, with no `wasm-ld` / `emscripten` / `binaryen` / `wasm-opt` in the build chain. WASM is structurally divergent from native ISAs (stack machine, structured control flow, linear memory) — designing for it FIRST validates that the IR layering generalizes beyond x86/ARM. `wasm-validate` and `wasm2wat` are test oracles only; they are never invoked at compile time.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1.x deliverable (post-v1 — not in the initial 6-combo native matrix). The IR design must NOT foreclose it from day one. |
| Predecessors  | [`09-hir-plan`](09-hir-plan%20-%20ok.md) (HIR shape), [`12-mir-lir-plan`](12-mir-lir-plan%20-%20tbd.md) (structured-CF markers preserved through MIR — load-bearing), [`06-artifact-profile-plan`](06-artifact-profile-plan%20-%20tbd.md) (`web-wasm` profile selection). |
| Successors    | [`14-linker-plan`](14-linker-plan%20-%20tbd.md) LK8 (WASM "linking" — Function/Table index renumbering, Import/Export merge); [`15-debug-info-plan`](15-debug-info-plan%20-%20tbd.md) (WASM DWARF / custom-section debug names). |
| Scope         | **Bounded.** WA1–WA10 below. v1.x = MVP single-module, single-memory, JS-host + WASI `preview1`. Reserved post-v1.x: SIMD, threads, GC types, component model, exception handling. |

---

## 1. Motivation

### 1.1 The user goal

The stated final goal is "any configured language compiles to **Windows, Linux, Android, iOS, macOS, web**." The web target is WebAssembly. WASM is not optional; it is one of the six platform endpoints the project commits to.

### 1.2 Why design for WASM first (even though it ships post-v1)

WASM is **structurally divergent** from every native ISA we plan to support:

- It is a **stack machine**, not a register machine. Operands flow through an implicit operand stack; there is no concept of "register allocation" in the traditional sense (locals are slot-indexed).
- It has **structured control flow only**. There are NO arbitrary jumps — only `block`, `loop`, `if`, `br`, `br_if`, `br_table`. Branches target labels on the structured-CF stack, not addresses.
- It uses **linear memory**, not a flat address space with segments. Pointers are `i32` (or `i64` under memory64) offsets into a single, growable, page-aligned byte array.
- It has **no traditional linker**. Linking is intra-module (Function / Table indices) and inter-module (Import / Export). There are no relocations as the native world uses the term.

If the IR layering (HIR → MIR → LIR) survives lowering to WASM cleanly — without bolting on a special "WASM mode" that bypasses the optimizer — that is strong evidence the layering is sound. Designing for WASM first **forces** the structured-CF markers (plan 12) to be real first-class IR citizens, not a native-codegen afterthought.

### 1.3 The hermetic invariant rules out `emscripten`

`emscripten` is a full Clang/LLVM-based C/C++ toolchain plus a JavaScript glue generator plus a `binaryen` optimizer. Pulling it in would violate the hermetic-compiler invariant the same way pulling in `link.exe` or `ld` would. The browser (or wasmtime, or wasmer, or any other WASM runtime) is treated as the **runtime artifact** — an FFI target reached via imports / exports, never as a compile-time tool.

### 1.4 Optimizer reuse

The same MIR optimizer that runs ahead of native codegen also runs ahead of WASM codegen. Constant folding, dead-code elimination, inlining, structured-CF preservation — every pass that produces well-formed MIR produces input the WASM backend can consume. Single optimizer surface, broader payoff: every MIR improvement helps both web and native output.

---

## 2. Design

### 2.1 WASM as a "LIR-equivalent"

WASM bytecode IS the LIR for the `web-wasm` target. The mental model stays uniform with native:

| Native | WASM |
|---|---|
| LIR (x86 / ARM ops) | WASM-LIR (stack-machine ops) |
| Assembler (LIR → bytes) | WASM encoder (LEB128 + section framing) |
| Linker (relocations + symbol table) | WASM "linker" (Function/Table renumber + Import/Export merge) |

There is no special "structured-MIR-to-bytes" path that bypasses LIR. The LIR layer for WASM is a thin wrapper over WASM opcodes — but conceptually it IS LIR. (See §4 open question 1 for the trade-off discussion.)

### 2.2 Lowering path

```
HIR  ─►  MIR  ─►  WASM-LIR  ─►  .wasm bytes
        (structured-CF        (sectioned binary
         markers preserved     module)
         per plan 12)
```

Critically: **no arbitrary CFG flattening**. The structured-CF markers from MIR map 1:1 to WASM `block` / `loop` / `if` / `br_if` / `br_table`. If MIR ever loses the markers (regression in plan 12 discipline), WASM lowering breaks loudly — which is the desired failure mode.

### 2.3 WASM type system mapping

| HIR core lattice | WASM core type | Notes |
|---|---|---|
| `i8`, `i16`, `i32` | `i32` | Narrower ints widened; sign tracked separately for ops that care (`shr_s` vs `shr_u`). |
| `i64` | `i64` | |
| `f32` | `f32` | |
| `f64` | `f64` | |
| vector (post-v1) | `v128` | Emit only when HIR uses vector types; default off. |
| function pointer | `funcref` | Table-indirect call. |
| host-opaque handle | `externref` | Reserved for FFI handles. |
| struct / array / tuple | (linear memory layout) | NOT a WASM type — laid out in linear memory; access via `i32` base + offset. |
| pointer | `i32` | Offset into linear memory. (memory64 reserved.) |

Function signatures lower to WASM `func` types with explicit param-list / result-list. Multi-value returns (post-MVP WASM, now standard) are used directly — no return-via-pointer trick.

### 2.4 Module structure (binary format, 12 sections)

The encoder emits sections in this fixed order:

| # | Section | v1.x contents |
|---|---|---|
| 0 | Magic + version | `\0asm` + `0x01000000` |
| 1 | Type | Function signatures (deduped). |
| 2 | Import | Imported funcs / tables / memories / globals — FFI targets (JS bindings, WASI exports, host imports). |
| 3 | Function | Function-to-Type index map. |
| 4 | Table | Indirect-call tables (one per `funcref` table). v1.x: one table for function pointers. |
| 5 | Memory | Linear-memory declarations (initial + max page count, 64 KiB pages). v1.x: one memory. |
| 6 | Global | Module-level globals — stack pointer (mutable `i32`), constants, etc. |
| 7 | Export | Funcs / tables / memories / globals exposed to the host. |
| 8 | Start | Optional start-function index (runs on instantiate). |
| 9 | Element | Table initialization (function-table population). |
| 10 | Code | Function bodies (locals declaration + bytecode). |
| 11 | Data | Linear-memory initialization (static strings, constant tables). |
| 12 | DataCount | Data-segment count (required for bulk-memory ops). |

Custom sections, emitted between standard sections:

- `name` — debug names for functions, locals, globals, types, etc. (huge readability win for `wasm2wat` diffs).
- `producers` — toolchain metadata (`dss-code-prime`, version).
- `target_features` — declared feature set (bulk-memory, sign-extension-ops, etc.); informs runtimes which engine features are required.

DWARF (post-v1.x — see plan 15) lands in further custom sections (`.debug_info`, `.debug_line`, etc.) when debug-info emission is enabled.

### 2.5 Linear memory model

- One default linear memory in v1.x. Pages are 64 KiB. Initial and max page counts come from project-config knobs (defaults: initial 1, max 16 — i.e. 64 KiB to 1 MiB).
- **HIR pointers → `i32` offsets** into linear memory. Pointer arithmetic is `i32` arithmetic. Null is offset 0 (and offset 0 is reserved — never allocated).
- **Static data** (string literals, constant tables, vtables once we have them) is emitted into the Data section at fixed offsets. The codegen owns a static-data arena that hands out offsets monotonically.
- **Stack pointer** is an `i32` mutable global, manually managed by codegen. Function prologue: `global.get $sp; i32.const N; i32.sub; global.set $sp`. Epilogue: restore. Locals that escape (address-taken) live on the linear-memory stack; non-escaping locals stay in WASM `local` slots.
- **Heap** is reserved for post-v1.x. Two options to be decided: (a) custom-language allocator emitted in linear memory, (b) JS-host-provided `malloc`/`free` imports. Default leaning: (a) for the wasmtime profile, (b) for the browser profile — but the choice is a project-config switch, not hardcoded.

### 2.6 Imports as FFI

The language's FFI mechanism (plan 11) is the user-facing way to declare imports. The WASM backend translates declared FFI imports into Section 2 entries.

- **JS-binding profile** (browser): imports take the shape `(import "env" "print" (func (param i32)))`. The host JavaScript constructs the `env` object on instantiation. Sidecar import-surface JSON (per plan 17's `.spv.json` precedent) documents the expected shape — the user wires it into their page.
- **WASI `preview1` profile** (wasmtime / wasmer / node `--experimental-wasi-modules`): imports take the shape `(import "wasi_snapshot_preview1" "fd_write" (func ...))`. The runtime fulfills them.
- **Host-import shape** is selected by `artifactProfile` (plan 6). `web-wasm-browser` vs `web-wasm-wasi` are distinct profiles emitting different Import sections from the same source.

### 2.7 WASM "linking"

WASM has no traditional symbol table. Linking is:

- **Intra-module** — Function and Table indices are dense, zero-based, assigned in declaration order. The encoder owns the index space.
- **Inter-module** — Imports and Exports name things by `(module, name)` string pairs. The host wires them on instantiation.

Multi-CU WASM build (post-v1.x for now; relevant once we ship multi-file WASM projects):

- Concatenate all CU function lists into one Code section.
- Renumber Function and Table indices into a single module-wide index space.
- Dedupe Type entries (same signature → same Type index).
- Merge Data segments; recompute offsets if any segment is relative.
- For any unresolved extern reference, emit an Import entry.
- Generate Export entries for everything the project config marks as externally visible.

These duties live in [`14-linker-plan`](14-linker-plan%20-%20tbd.md) LK8 — coordinate there to keep the work in one place rather than splitting it.

### 2.8 Structured-CF lowering — the critical correctness piece

The structured-CF markers from plan 12 carry the labels needed to compute the branch depth (`N`) for each `br` / `br_if`.

| HIR construct | WASM lowering |
|---|---|
| `if cond then ... end` | `<cond>` `if (result …) <then> end` |
| `if cond then ... else ... end` | `<cond>` `if (result …) <then> else <else> end` |
| `while cond { body }` | `block $brk` `loop $cont` `<cond>` `i32.eqz` `br_if $brk` `<body>` `br $cont` `end` `end` |
| `for (init; cond; step) { body }` | `<init>` then while-shape with `<step>` before the `br $cont` |
| `switch x { case k₁: ... case kₙ: ... default: ... }` | Wrap cases in nested `block`s; `<x>` `br_table $c₁ $c₂ … $default`; each case body ends with `br $end_of_switch` |
| `break` (inside loop N levels deep) | `br N` — depth computed from the structured-CF marker stack |
| `continue` (inside loop) | `br N` to the `loop` label (not the surrounding `block`) |
| `return` | `return` (always valid; WASM allows multi-value return) |

The structured-CF marker stack in MIR (plan 12) is the source of truth for the branch depth `N`. There is no need to re-derive structure in the WASM backend — if MIR is well-formed, lowering is mechanical.

### 2.9 Reserved post-v1.x

- **SIMD** (`v128` + 200+ vector ops) — emit only when HIR uses vector types; default off. Gated by `target_features` declaration in custom section.
- **Threads + atomics** — Vulkan-style relaxed memory model; reserved until we have a story for concurrent custom-language semantics.
- **GC types** (`structref` / `arrayref` from the WASM GC proposal) — reserved for the future GC'd custom language; not relevant to c-subset or T-SQL.
- **Component model** (WASI 0.3+) — reserved; track upstream stability before committing.
- **Exception handling** (`try`/`catch`/`throw` opcodes) — reserved; v1.x lowers exceptions to early-return / status-code (matches c-subset's reality of no exceptions).
- **memory64** (`i64` pointers) — reserved; v1.x is `i32` linear memory.

### 2.10 Driver integration

Project config selects `target: "web-wasm"` (plus a profile sub-selector — `"browser"` or `"wasi"`). The driver dispatches to this backend.

Output:
- **`.wasm`** — the module binary. The primary artifact.
- **`.wat`** (debug builds only) — text disassembly. Generated by our own emitter (not by `wasm2wat`) so the artifact stays hermetic; `wasm2wat` is used only as a test oracle to diff against our `.wat`.
- **`.imports.json`** (sidecar) — describes the import surface so the host (JS page, wasmtime CLI, etc.) knows what to wire. Mirrors plan 17's `.spv.json` precedent.
- **`.js` glue** — out of v1.x (see §4 open question 5). User wires the imports themselves.

---

## 3. PR breakdown

| PR | Scope |
|---|---|
| WA1 | WASM section encoder skeleton + module header (`\0asm` + `0x01`). Section framing (id byte + LEB128 length + payload). LEB128 (signed + unsigned) encoders + tests. |
| WA2 | Type / Function / Code section emission for trivial leaf functions (no locals, no memory, no branches). E.g. `add(i32, i32) -> i32`. Validates encoder framing end-to-end. |
| WA3 | Memory section + Data section + `i32` pointer arithmetic. Static string literal lowering. Stack-pointer global. |
| WA4 | Structured-CF lowering (`if` / `loop` / `block` / `br` / `br_if` / `br_table`) from MIR structured-CF markers. The load-bearing PR — gates everything past trivial leaf functions. |
| WA5 | Import + Export sections. JS-binding shape and WASI `preview1` shape, selected by `artifactProfile` sub-selector. |
| WA6 | Global section + Start section. Module-level mutable / immutable globals. Optional start function. |
| WA7 | `name` custom section (function names, local names, type names). Massive `wasm2wat` readability boost for debugging codegen regressions. |
| WA8 | Round-trip tests — emit `.wasm`, run `wasm-validate` (oracle), run `wasm2wat` and diff against our own `.wat` emitter. Golden `.wasm` byte snapshots for stable inputs. |
| WA9 | End-to-end "hello, world" — a c-subset corpus program compiles to `.wasm`, runs under wasmtime in CI, prints expected output. |
| WA10 | WASM-LIR ↔ MIR contract pin: same MIR input produces byte-identical WASM output across runs / platforms. Deterministic-output regression guard. |

Sequencing: WA1 → WA2 → WA3 → WA4 is the critical path (encoder, then leaf funcs, then memory, then control flow). WA5 / WA6 / WA7 are parallel-able once WA4 lands. WA8 / WA9 / WA10 are validation PRs that gate "WASM backend done."

---

## 4. Open questions

**(1) Model WASM as its own LIR target, or as "MIR-direct-to-bytes"?** Default: **own LIR target**, for symmetry with native. One mental model across all backends: LIR + assembler + linker. WASM happens to have a trivial "assembler" (LEB128 + section framing) and a trivial "linker" (index renumber + Import/Export merge), but the conceptual layering is the same. Re-evaluate if WASM-specific quirks (e.g. the operand stack vs registers gap) force enough special-casing that the LIR layer becomes a fiction. Decision deferred to WA4.

**(2) Diagnostic namespace.** Default: **`W_*` standalone.** WASM is divergent enough from native ISAs that grouping its diagnostics with the linker (`K_*`) or generic assembler (`A_*`) namespaces would obscure the WASM-specific failure modes (validation errors, type-mismatch on the operand stack, structured-CF marker mismatches). `W_*` keeps WASM diagnostics together and discoverable. Reconsidered if the namespace stays small (<10 codes) for a long time — could fold into `K_*` then.

**(3) Browser vs wasmtime host abstraction.** Default: language-config declares the host shape via `artifactProfile` sub-selector; the WASM backend emits per-host import-table conventions. No runtime detection, no portable host shim — the user picks at compile time. Re-evaluate if multiple hosts share enough import shape to warrant a "neutral" profile.

**(4) WASI version target.** Default: **`wasi_snapshot_preview1`** for v1.x — stable, widely supported, well-documented. Component model + WASI 0.3+ are reserved for post-v1.x once upstream stabilizes.

**(5) JS-glue auto-generation (Emscripten-style).** Default: **out of v1.x.** Host integration is the user's responsibility. We emit a clean `.wasm` plus an `.imports.json` sidecar documenting the import surface; users wire the JS themselves. Avoids re-implementing a chunk of Emscripten just to support trivial cases. Reconsidered when we ship a real browser-targeted example app.

**(6) SourceMap or DWARF for WASM debug info?** Default: **WASM DWARF** (Chrome DevTools supports it; wasmtime and Node also support it). Reuse plan 15's DWARF emitter with WASM-section embedding (`.debug_info` etc. as custom sections). SourceMap is text-source-oriented and less expressive for compiled languages — wrong tool. Re-evaluate if WASM DWARF tooling deteriorates in the major runtimes.

---

## 5. Acceptance

- [ ] A c-subset corpus program compiles to a `wasm-validate`-clean `.wasm` module.
- [ ] The compiled `.wasm` runs under both wasmtime and Chromium V8 with byte-identical results (oracles only — neither runtime is required at compile time).
- [ ] Structured-CF lowering preserves semantic equivalence — loops terminate where they should, branches reach the correct target, `br_table` dispatches correctly. Verified by execution-trace diff against a reference interpreter on the corpus.
- [ ] Imports resolve correctly against a sample JS host (browser profile) AND a sample WASI host (wasmtime profile). Both work from the same source via profile sub-selector.
- [ ] **Deterministic output:** same input MIR → byte-identical `.wasm` across runs, across platforms (Windows / Linux / macOS build hosts). Hash-pinned in CI.
- [ ] No `wasm-ld`, `emscripten`, `binaryen`, or `wasm-opt` invocation anywhere in the build chain. `wasm-validate` and `wasm2wat` appear only in test fixtures.
- [ ] `name` custom section populated — `wasm2wat` output is human-readable.
- [ ] `.imports.json` sidecar accurately reflects the emitted Import section.

---

## 6. Risks

- **Structured-CF marker discipline failure** (regression in plan 12) silently produces WASM that passes `wasm-validate` but executes incorrectly — e.g. a `br` with the wrong depth lands in a different loop. Mitigation: golden `.wasm` byte snapshots on a stable corpus (WA10) + execution-trace diff against a reference interpreter (WA9). Both must be wired in CI before WA4 is considered done.
- **Component model + WASI churn.** Upstream is still moving. Mitigation: pin firmly to `wasi_snapshot_preview1` for v1.x; reserve `preview2`+ in a separate namespace and revisit only post-v1.x. Do not chase the moving target.
- **Browser-host integration ergonomics.** Risk: we emit a `.wasm` that users can't easily consume from JS because the import surface is opaque. Mitigation: `.imports.json` sidecar (plan 17 precedent) describes the surface; document a minimal HTML+JS example per browser-profile release.
- **Linear-memory stack-pointer corruption.** Bug class: a missed prologue/epilogue, or a wrong frame size, silently corrupts the linear-memory stack. Mitigation: stack-pointer guard pages (reserve linear memory below the initial SP and trap on access via `unreachable` sentinel writes); strict assertions in codegen on prologue/epilogue pairing.
- **Index-space drift between Function / Table / Type / Memory / Global sections.** A bug renumbering Function indices but not their references in Element / Code / Export sections produces invalid modules. Mitigation: encoder owns ALL index assignment behind a typed-index API (`FuncIdx`, `TableIdx`, `TypeIdx`, `MemIdx`, `GlobalIdx` as distinct types — substrate-hardening discipline applies), so no caller can pass an `i32` where a typed index is expected.

---

## 7. Sequencing

- **Depends on:** [`09-hir-plan`](09-hir-plan%20-%20ok.md) (HIR shape), [`12-mir-lir-plan`](12-mir-lir-plan%20-%20tbd.md) (structured-CF marker preservation through MIR — load-bearing), [`06-artifact-profile-plan`](06-artifact-profile-plan%20-%20tbd.md) (`web-wasm-browser` / `web-wasm-wasi` profiles).
- **Coordinates with:** [`14-linker-plan`](14-linker-plan%20-%20tbd.md) LK8 (WASM "linker" duties live there); [`15-debug-info-plan`](15-debug-info-plan%20-%20tbd.md) (WASM DWARF custom-section embedding); [`17-shader-gpu-plan`](17-shader-gpu-plan%20-%20tbd.md) (sidecar `.imports.json` precedent mirrors `.spv.json`).
- **Runs in parallel with:** native codegen — does NOT block any v1 6-combo native matrix work. Both backends consume the same MIR; both benefit from MIR optimizer improvements; neither is on the other's critical path.
- **Post-v1.x.** This plan ships in v1.x, AFTER the v1 native matrix is green. Designing it now ensures the IR layering accommodates WASM from day one; the IMPLEMENTATION lands after v1.
