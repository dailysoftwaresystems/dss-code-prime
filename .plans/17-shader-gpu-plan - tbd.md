# Shader / GPU Backend — Sub-Plan (17)

> Owns **SPIR-V code generation** plus the **shader-shape extensions to HIR** (intrinsics, binding-resource model, entry-point attributes, GPU restrictions). Hermetic per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.1 — no `dxc` / `glslc` / `shaderc` / `spirv-tools` invocations.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1.x — lit up once the user's custom language begins (`20-custom-language-reserved-plan`). Reserved scope today; design lands now to keep HIR honest. |
| Predecessors  | 🟡 [`09-hir-plan`](./09-hir-plan%20-%20tbd.md) (shader-shape HIR extensions — HR1 ✅ 2026-05-26 ships the open `HirKindRegistry` shader-shape extensions will register against, HR2 ✅ adds the typed-expression + `HirOpRegistry` substrate, HR3 ✅ adds structured control flow; HR4–HR11 pending). ⏳ [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md) (structured-CF markers carry into SPIR-V `OpLoopMerge` / `OpSelectionMerge`). |
| Successors    | [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md) for SPIR-V→{DXIL, MSL, WGSL} transpile post-v1. |
| Scope         | **Bounded.** SG1–SG10. v1 deliverable for the custom language is "compute + vertex + fragment shaders compile to spirv-val-clean SPIR-V." |

---

## 1. Motivation

Same-source CPU + GPU code in one language is the architectural target — the substrate must support it from day one, not bolt it on later. The discipline this plan enforces:
- Vector / matrix / sampler / texture / UAV / push-constant types are **first-class in the core type lattice** (`08.5-substrate-prep-plan` §2.2), not language extensions.
- HIR carries **stage attributes** (`[[shader.vertex]]` / `[[shader.fragment]]` / `[[shader.compute(8,8,1)]]`) and **shared-language attributes** (`[[shader.usable]]` + `[[host.usable]]`).
- HIR carries a **shader-restriction verifier** (no recursion, no dynamic alloc, no fn-ptrs, no libc calls) that runs on functions flagged shader-usable.
- MIR's structured-CF marker discipline (`12-mir-lir-plan` §2.3) is exactly what SPIR-V requires; we reuse it.

Reference implementations (`spirv-tools`) serve as test oracles only.

---

## 2. Design

### 2.1 Files

```
src/shader/
├── shader.hpp                  # Public entry point
├── intrinsics.hpp              # Intrinsic library declaration
├── verifier.hpp / .cpp         # Shader-restriction verifier (recursion/alloc/etc.)
├── spirv/
│   ├── spirv_emitter.hpp / .cpp  # Module + header + sections
│   ├── spirv_types.hpp / .cpp    # Lattice → OpType*
│   ├── spirv_inst.hpp / .cpp     # MIR instruction → SPIR-V opcode lowering
│   ├── spirv_struct_cf.hpp/.cpp  # OpLoopMerge / OpSelectionMerge from MIR markers
│   └── spirv_decorate.hpp / .cpp # Decorate Binding/DescriptorSet/Location/BuiltIn
└── reflection.hpp / .cpp       # Sidecar .spv.json describing entry-points + bindings
```

### 2.2 HIR shader extensions

**Two distinct registration paths — do not conflate them:**

- Shader **types** (`Sampler`, `Texture<>`, `UAV<>`, `ConstantBuffer<>`, `WorkgroupShared<>`): registered via the language schema's `typeExtensions[]` block (v3, SP2) into the `TypeRegistry` (per [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) §2.2). Listed in the "Shader-shape extensions" table below.
- Shader **HIR kinds** (`WorkgroupBarrier`, `DerivativeX`/`Y`, `TextureSample`, `TextureLoad`, `ImageStore`, `AtomicOp`, etc.): registered via the language schema's `hirLowering` block (v4, planned per [`09-hir-plan`](./09-hir-plan%20-%20tbd.md) §2.5) into the `HirKindRegistry` (`HirKind ≥ 256`). They are **registered HIR extension kinds, not hardcoded core HIR enum members** — the open core + per-language registered-extensions pattern from `09-hir-plan` §2.2 applies. Listed in the table immediately below.

The shader-extension HIR kinds (registered via `hirLowering` per the language schema, into the `HirKindRegistry`):

| Kind | Purpose |
|---|---|
| `WorkgroupBarrier` | `barrier()` — SPIR-V `OpControlBarrier` |
| `DerivativeX` / `DerivativeY` | Pixel-shader derivatives → `OpDPdx` / `OpDPdy` |
| `TextureSample` | `sample(tex, sampler, uv)` → `OpImageSampleImplicitLod` |
| `TextureLoad` | `textureLoad(tex, coord, lod)` → `OpImageFetch` |
| `ImageStore` | `imageStore(uav, coord, val)` → `OpImageWrite` |
| `AtomicOp` | `atomicAdd`/`atomicMax`/etc. → `OpAtomic*` |
| `Swizzle` | `v.xyz` → `OpVectorShuffle` |

Lattice membership (per [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) §2.2):

**Core lattice** (universal — non-shader code uses these too):
- `Vector<T, N>` where N ∈ {2, 3, 4}, T ∈ {f16, f32, f64, i32, u32, bool}
- `Matrix<T, R, C>`

**Shader-shape extensions** (registered by the language schema via `typeExtensions[]`):
- `Sampler` — opaque handle
- `Texture<dim, format>` — dim ∈ {1D, 2D, 3D, Cube, 2DArray, CubeArray, 2DMS}, format ∈ {Rgba8, Rgba16f, …}
- `UAV<T>` — read-write storage image / buffer
- `ConstantBuffer<T>` — uniform buffer
- `WorkgroupShared<T>` — `Workgroup` SPIR-V storage class

Extension registration happens once per CU when a shader-shape language schema is loaded; non-shader languages don't see them.

### 2.3 Shader restrictions (verifier — `SH_*`)

Functions flagged `ShaderUsable` (via `[[shader.*]]` attributes lowered into `HirFlags`) are verified at HIR-level:

- **No recursion**: call graph must be a DAG (verified via DFS-tarjan). `SH_RecursionDisallowed`.
- **No dynamic allocation**: `IntrinsicCall` to `malloc`/`free` rejected. `SH_DynamicAllocDisallowed`.
- **No function pointers**: `FnPtr<T>` in any expression rejected. `SH_FnPtrDisallowed`.
- **No libc calls**: every callee must itself be `ShaderUsable` or a registered shader intrinsic. `SH_NonShaderCall`.
- **No goto** (already HIR-level — structured CF mandatory).
- **No `Ptr<T>` into host memory** — only `WorkgroupShared<T>`, `ConstantBuffer<T>`, `UAV<T>`. `SH_HostPointerInShader`.

### 2.4 Same-source CPU + GPU functions

A function tagged BOTH `[[shader.usable]]` AND `[[host.usable]]` lowers twice:
- **Shader lowering**: HIR (after shader-verifier pass) → MIR → SPIR-V via this plan.
- **Host lowering**: HIR → MIR → LIR → native bytes via `12-mir-lir` + `13-assembler` + `14-linker`.

Both lowerings produce instructions with `HirAttribute<SourceSpan>` pointing at the same user-authored function. Debug info per `15-debug-info-plan` reflects the dual nature (DWARF + SPIR-V `OpLine` both reference the same source span).

### 2.5 Lowering strategy

**HIR → MIR → SPIR-V** (default).

Reuse the MIR optimizer (constant folding, DCE, copy propagation). Structured-CF markers from `12-mir-lir-plan §2.3` map 1:1 to SPIR-V:
- `LoopHeader` / `LoopLatch` / `LoopExit` → `OpLoopMerge` + `OpBranchConditional`
- `IfThen` / `IfElse` / `IfJoin` → `OpSelectionMerge` + `OpBranchConditional`
- `SwitchHead` / `SwitchCase` / `SwitchJoin` → `OpSelectionMerge` + `OpSwitch` (the `SwitchJoin` block carries the merge label)

Open question §4.1: should we add a `HIR → SPIR-V direct` path that skips MIR for trivial leaf shaders? Default: no — reuse MIR for everything.

### 2.6 SPIR-V emission

Module header: magic `0x07230203`, version `0x00010600` (SPIR-V 1.6).

Sections (in SPIR-V's required order):
1. Capability declarations (`OpCapability Shader` + driven-by-feature: `Float16`, `Int8`, `StorageImageMultisample`, etc.)
2. Extension imports (`OpExtension`)
3. ExtInstImport (`OpExtInstImport "GLSL.std.450"`)
4. Memory model (`OpMemoryModel Logical GLSL450`)
5. Entry points (`OpEntryPoint Vertex %main "main" %inputs %outputs ...`)
6. Execution modes (`OpExecutionMode %main OriginUpperLeft` / `LocalSize x y z`)
7. Debug instructions (`OpSource`, `OpLine` per `15-debug-info-plan` §2.7)
8. Annotations / decorations (`OpDecorate %binding Binding 0`, `OpDecorate %ds DescriptorSet 1`, `OpDecorate %pos BuiltIn Position`)
9. Type / constant / global variable declarations
10. Function definitions (one `OpFunction` ... `OpFunctionEnd` per function)

### 2.7 Binding-resource model

Vulkan-shaped (D3D12 / Metal map via post-v1 transpile in `10-source-translation-plan`):

- Descriptor sets (0–3 typical; numbered)
- Bindings within a set (typed: sampled image, storage image, uniform buffer, storage buffer, sampler)
- Push constants — special binding; one block per pipeline; limited size (128 bytes typical)
- Per-stage interface variables — vertex inputs (`Location 0..N`), fragment outputs (`Location 0..N`)
- Built-ins — `Position`, `FragCoord`, `GlobalInvocationID`, `LocalInvocationID`, etc. via `BuiltIn` decoration

Authored in source language via attributes (e.g. `[[binding(set=0, slot=2)]]` on a `Texture<2D, Rgba8>` declaration).

### 2.8 Reflection sidecar

Sidecar `.spv.json` per `.spv`:
```json
{
  "entry_points": [
    { "name": "main", "stage": "fragment", "interface": {...} }
  ],
  "bindings": [
    { "set": 0, "binding": 0, "kind": "uniform_buffer", "type": "Camera" },
    { "set": 0, "binding": 1, "kind": "sampled_image", "type": "Texture2D<rgba8>" }
  ],
  "push_constants": { "size": 64, "fields": [...] }
}
```

Engine integration reads the sidecar to set up Vulkan descriptor set layouts.

---

## 3. PR breakdown

| PR  | Title                                            | Scope |
|-----|--------------------------------------------------|-------|
| SG1 | HIR shader extension types + intrinsic library    | Lattice members (`Vector<T,N>`, `Matrix<T,R,C>`, `Sampler`, `Texture<>`, `UAV<>`, etc.). Intrinsic registration in `core/types/type_lattice/`. |
| SG2 | HIR shader-restriction verifier                  | Recursion / dynamic-alloc / fn-ptr / libc-call rejection with `SH_*` codes. |
| SG3 | SPIR-V emitter skeleton                          | Module header + memory model + first entry point. |
| SG4 | SPIR-V type encoding                             | Core lattice → `OpType*`. Composite types (vec / mat / image / sampler / structures). |
| SG5 | SPIR-V function bodies (arithmetic + memory)     | MIR instruction → SPIR-V opcode lowering. |
| SG6 | SPIR-V structured CF                             | `OpLoopMerge` / `OpSelectionMerge` from MIR `StructCfMarker` tags. |
| SG7 | SPIR-V decorations                               | Binding / DescriptorSet / Location / BuiltIn. |
| SG8 | Entry-point attribute parsing in HIR             | `[[shader.vertex]]` / `[[shader.fragment]]` / `[[shader.compute(x,y,z)]]`. |
| SG9 | Round-trip + spirv-val oracle tests              | Emit → `spirv-val` (oracle) → assert valid. Round-trip via `spirv-as`/`spirv-dis` text. |
| SG10| End-to-end "hello triangle" Vulkan harness       | Compile vertex + fragment shaders, render a triangle in a CI Vulkan harness, assert frame correctness. |

Substrate tier for SG1, SG6 (touch lattice + structured-CF contract).

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | HIR→SPIR-V direct path (skip MIR for leaf shaders)? | **No** — always via MIR. Reuse the optimizer. Revisit if shader compile time becomes a problem. |
| 2 | Target SPIR-V version? | **1.6** — covers Vulkan 1.3 baseline. |
| 3 | Vulkan / Metal / D3D12 / WebGPU coverage? | Native SPIR-V; Metal/D3D12/WebGPU via post-v1 `10-source-translation-plan` transpile. |
| 4 | Same-source CPU+GPU function dispatch? | **Yes** — `[[shader.usable]] [[host.usable]]` triggers dual lowering. |
| 5 | Shader debugging (RenderDoc / Nsight / Xcode GPU debugger)? | v1.x: `OpLine` debug instructions for line numbers (via `15-debug-info-plan` §2.7 — open question §8). Full local-variable debug post-v1. |
| 6 | Reflection sidecar format? | `.spv.json` per §2.8. Stable schema; versioned. |
| 7 | Ray-tracing extensions (`SPV_KHR_ray_tracing`)? | Reserved post-v1.x. |
| 8 | Subgroup ops (warp-level intrinsics)? | Reserved — added when a real ray-tracing or compute-heavy workload demands them. |
| 9 | SPIR-V diagnostic namespace? | `SH_*` for HIR-side shader violations; SPIR-V emission errors share `SH_*` (e.g. `SH_SpirVCapabilityMissing`). |

---

## 5. Acceptance criteria

- [ ] Custom-language HIR shader subset lowers to spirv-val-clean SPIR-V.
- [ ] "Hello triangle" vertex+fragment shader produced by our toolchain renders identically to a `dxc`-compiled equivalent in a CI Vulkan validation-layer harness.
- [ ] Compute shader compiles + dispatches + produces correct buffer output on a Vulkan-compute-capable CI runner.
- [ ] Shader verifier rejects all SPIR-V constraint violations with actionable `SH_*` diagnostics.
- [ ] Same-source CPU + GPU function lowering: a function tagged both `[[shader.usable]]` and `[[host.usable]]` produces correct SPIR-V *and* correct native code referencing the same source span.
- [ ] Hermetic acceptance: no `dxc` / `glslc` / `shaderc` / `spirv-tools` invocation in the production pipeline (oracles only in CI).

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| SPIR-V capability negotiation drift | Medium | Medium | Validate capabilities against Vulkan baseline at module emission; reject if a feature requires a capability we don't declare. |
| Shader debugging absent in v1.x | High | Low | Reserved; reflection sidecar + `OpLine` give "good enough" for first release. |
| Same-source dual-lowering complexity (libc gating per lowering mode) | High | High | Verifier mode-aware: when lowering as shader, every callee checked `ShaderUsable`; when lowering as host, no restrictions. Tests cross every realistic shape. |
| First-class lattice members for GPU types pressure semantic phase | Medium | Medium | Lattice extensions registered per `08.5-substrate-prep-plan §2.2` — language schemas declare which lattice members they expose. Non-shader languages don't see GPU types. |

---

## 7. Sequencing

```
09-hir + 12-mir-lir ─► SG1 ─► SG2 ─► SG3 ─► SG4 ─► SG5 ─► SG6 ─► SG7 ─► SG8 ─► SG9 ─► SG10
                                                                                          │
                                                                                          ▼
                                                                            10-source-translation
                                                                            (SPIR-V → DXIL/MSL/WGSL)
                                                                            (post-v1.x)
```
