# Shader / GPU Backend — Sub-Plan (17)

> Owns the **whole GPU story**: deriving GPU target descriptors by measuring the vendors' own toolchains (Track A), **generating GPU code** from MIR — SPIR-V *and* real GPU ISA (Track B), and **deploying + dispatching** it on the machine the program actually lands on (Track C). Plus the **shader-shape extensions to HIR** (intrinsics, binding-resource model, entry-point attributes, GPU restrictions). Hermetic per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.1 — no `dxc` / `glslc` / `shaderc` / `spirv-tools` invocations **in the production pipeline** (Track A's measurement harness is a config-authoring tool, not a build step — see §3.1).
>
> **Rev 4 (2026-08-12).** Two scope expansions, both operator decisions, both recorded here because each *reverses* a rev 3 position rather than extending it.
>
> **(1) We DO compile to GPU machine code.** Rev 3 said: *"**What we DO NOT compile:** SPIR-V → GPU machine code. The GPU driver does that at load time (NVIDIA PTX, AMD GCN/RDNA, Apple Metal IR, Intel Xe IR). We ship SPIR-V; the driver/runtime is outside the hermetic boundary. Writing GPU-ISA backends is reserved post-v1 (probably never — proprietary-driver-equivalent work)."* **That paragraph is SUPERSEDED, deliberately, not by argument but by method.** The thing that made GPU-ISA backends look like "probably never" was that the ISAs are undocumented — and that is exactly the problem [[feedback_reference_compilers_are_the_spec]] already solves everywhere else in this tree: `x86_64.target.json`'s predefined-macro rows were authored from recorded `clang -dM -E -x c /dev/null -target <triple>` runs, not from a specification. Track A does the same thing to `nvcc`/`metal`/`hipcc`/`ocloc`: compile probe programs with the vendor's compiler, disassemble the result with the vendor's disassembler, and **derive** `<gpu>.target.json` from what was measured. The ISA stops being undocumented the moment we own an instrument that reads it. Rev 3's cost estimate stands (this is the largest single undertaking in the plan tree); its *conclusion* does not.
>
> **(2) The GPU backend has a deployment half.** Rev 3 was an AOT story only: compile, emit `.spv`, done. Rev 4 adds the shape the product actually needs — one shipped program that adapts to whatever GPU it lands on, compiling for that GPU on first run and falling back to a mandatory CPU sibling whenever it cannot. That is Track C, and it makes the MIR text format (`.dssir`) a **shipped artifact** rather than test-only tooling, which has consequences of its own (§2.13).
>
> **What rev 3 got right and rev 4 keeps unchanged:** GPU lowering is **MIR-downstream** (same shape as plan 18's WASM), the MIR optimizer runs UPSTREAM of it so fully-optimized MIR is the input, lowering is a bucket-2 walker over a JSON-declared vocabulary, and the engine never branches on identity. Rev 4 does *not* relax any of that — it applies it to four more vendors. ⚠ For SPIR-V specifically, "bypassing LIR" also stands (a typed-value-stream bytecode has no register file to allocate); for **real GPU ISA it does not**, because a GPU ISA *is* a register machine — see §2.11.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1.x — lit up once the user's custom language begins (`20-custom-language-reserved-plan`). Reserved scope today; design lands now to keep HIR honest. **Linker substrate ✅ landed 2026-05-30 (plan 14 LK9)**: `spirv-1.6.format.json` + `src/link/format/spirv.{hpp,cpp}` ship the 5-word SPIR-V module header (magic + version 1.6 + generator + bound + reserved) + dispatch routing. Track B picks up at SG3 (SPIR-V instruction-stream emitter) on top of the landed substrate. **Track A (`GI*`) has no predecessor and can start immediately** — it is a standalone measurement harness that touches no compiler code, and everything downstream is gated on what it measures. |
| Tracks        | **A** `GI*` — GPU identification + target derivation (§3). **B** `SG*` — MIR → GPU code generation (§2): SPIR-V (SG1–SG13, rev 3) + real GPU ISA (SG14+, rev 4). **C** `GD*` — deployment, first-run compile, dispatch runtime (§4). See §0.1. |
| Rev 4 scope   | **NVIDIA + Apple + AMD + Intel**, all four, as the identifier's and the ISA backends' target vendor set (operator decision 2026-08-12). Artifact tier is **real GPU ISA** (SASS / AGX / RDNA / Xe), not a vendor virtual ISA and not SPIR-V-only — see the header note and §2.11. |
| Predecessors  | ✅ [`09-hir-plan`](./09-hir-plan%20-%20ok.md) (shader-shape HIR extensions — HR1 ✅ 2026-05-26 ships the open `HirKindRegistry` shader-shape extensions will register against, HR2 ✅ adds the typed-expression + `HirOpRegistry` substrate, HR3 ✅ adds structured control flow, HR4 ✅ adds the declaration + extern surface, HR5 ✅ adds the attribute side-tables — incl. the `ShaderIntrinsic` / `HirShaderMap` side-table shader lowering populates (stage / built-in / workgroup / binding), HR6 ✅ adds the verifier's HIR-level shader-restriction gate (`H_ShaderViolation`: recursion / indirect call / non-shader callee over `ShaderUsable` subtrees — the fuller `SH_*` checks remain this plan's SG2), HR7 ✅ 2026-05-27 adds the `.dsshir` text format (serializes the `ShaderIntrinsic` side-table + `ShaderUsable` flag + shader extension kinds — the shader-lowering test/debug surface), HR8 ✅ 2026-05-27 adds the config-driven CST→HIR lowering engine (the `hirLowering` facet shader-shape languages will use to map their CST to shader extension kinds), proven on c-subset; HR9 ✅ 2026-05-27 enriched toy into a typed language + un-deferred arrays end-to-end; HR10 ✅ + HR11 ✅ done 2026-05-28 — plan 09 (HIR) complete). ⏳ [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) (structured-CF markers carry into SPIR-V `OpLoopMerge` / `OpSelectionMerge`). |
| Successors    | [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md) for SPIR-V→{DXIL, MSL, WGSL} transpile post-v1. |
| Scope         | **Bounded, in three tracks.** v1 deliverable for the custom language remains "compute + vertex + fragment shaders compile to spirv-val-clean SPIR-V" (Track B, SG1–SG13). Rev 4 adds two further deliverables, each with its own bar: Track A ships a `<gpu>.target.json` for at least one real GPU of each in-scope vendor, every row carrying MEASURED provenance; Track C ships the deployment quartet — launcher shim, main executable with mandatory CPU siblings, `.dssir` MIR sidecar, runtime compiler — such that one built program runs GPU-accelerated on a machine whose GPU it was never told about, and runs *correctly on the CPU* on one whose GPU it does not recognize. |
| Mapped from elsewhere | **F16 const-eval Cast target** (from plan [12.5 §0.2 D1](./12.5-const-eval-plan%20-%20ok.md)) — ✅ **ALREADY CLOSED, no longer a shader-cycle prerequisite.** Rev 3 recorded this as a delimited prerequisite that an SG-cycle would own: half-precision folding in `const_eval` was gated on a soft-float helper, and the CE engine's `Cast` quadrant refused F16 with `UnsupportedTypeKind`. ⚠ **That text was carried forward unchanged into rev 4 and was stale — corrected 2026-08-12 after an independent fact-check caught it contradicting its own cited source.** Plan 12.5 §0.2 row D1 reads *"✅ closed `3b56591` … Closed by `narrowToHalf` soft-float helper (binary32 → binary16 mantissa rounding RTE) … (was) Plan 17 shader-gpu — no longer pending."* MEASURED in code 2026-08-12: `src/hir/const_eval_arith.hpp:486` defines `narrowToHalf`, dispatched from the width-16 `Cast` arm at line 576. **SG-cycles that introduce F16 literals inherit a working helper and owe nothing here.** |

---

## 0.1 Track map — and why the document order is not the dependency order

Rev 4 splits this plan into three tracks. Each mints its own PR prefix, per the tree's per-plan-prefix convention (`HR*` for HIR, `FC*` for full-C, `LK*` for the linker, …):

| Track | Prefix | Owns | §  |
|---|---|---|---|
| **A** | `GI*` | **GPU identification + target derivation.** The `gpu-identifier` harness: probe programs compiled and disassembled by the *vendors'* toolchains, whose output is derived into `<gpu>.target.json`. Dev/CI-time only; never runs on an end user's machine. | §3 |
| **B** | `SG*` | **MIR → GPU code generation.** SG1–SG13 (SPIR-V) are rev 3's, preserved verbatim. SG14+ (rev 4) add the real-ISA backends Track A's measurements make writable. | §2 |
| **C** | `GD*` | **Deployment + dispatch.** MIR sidecar, launcher shim, first-run detect-and-compile, artifact cache, mandatory CPU sibling, and the driver-API dispatch runtime that actually launches a kernel. | §4 |

**Dependency order is A → B(ISA) → C.** You cannot write a SASS encoder before you have read SASS, and you cannot deploy a GPU artifact before something can produce one. Track B's *SPIR-V* half (SG1–SG13) is independent of Track A and can proceed in parallel.

**Document order is B, A, C — which is not the dependency order, and that is deliberate.** §2's subsection numbering is **FROZEN**: `§2.2`, `§2.3`, `§2.4`, `§2.5` and `§2.9` are all cited from outside this file, including from *shipped source* — `src/link/format/spirv.cpp:109` emits a diagnostic containing the literal string "bypasses LIR per plan 17 §2.5", and [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) cites §2.9 for the `V_*` 0x7xxx nibble allocation while [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) cites §2.2. Renumbering to get a prettier narrative would silently falsify a diagnostic string in a compiled binary. New material is therefore **appended** (§2.11+, §3, §4) and the tail sections shift down (old §3→§5, §4→§6, §5→§7, §6→§8, §7→§9); no §3+ citation of this plan exists, so the tail shift breaks nothing. ★ This is the same rule the anchor registry states about its own rows: edit in place, never reserialize.

---

## 1. Motivation

Same-source CPU + GPU code in one language is the architectural target — the substrate must support it from day one, not bolt it on later. The discipline this plan enforces:
- Vector / matrix / sampler / texture / UAV / push-constant types are **first-class in the core type lattice** (`08.5-substrate-prep-plan` §2.2), not language extensions.
- HIR carries **stage attributes** (`[[shader.vertex]]` / `[[shader.fragment]]` / `[[shader.compute(8,8,1)]]`) and **shared-language attributes** (`[[shader.usable]]` + `[[host.usable]]`).
- HIR carries a **shader-restriction verifier** (no recursion, no dynamic alloc, no fn-ptrs, no libc calls) that runs on functions flagged shader-usable.
- MIR's structured-CF marker discipline (`12-mir-lir-plan` §2.3) is exactly what SPIR-V requires; we reuse it.

Reference implementations (`spirv-tools`) serve as test oracles only.

### 1.1 The deployment thesis (rev 4)

AOT GPU compilation has a distribution problem that AOT CPU compilation does not: **you cannot know at build time which GPU the program will run on**, and unlike CPUs — where `x86_64` names a stable contract two decades wide — GPU ISAs change between chip generations and sometimes between driver releases. Everyone else solves this by shipping a vendor IR and letting the driver finish the job at load time. We are not doing that (see the header note), so we solve it by **moving the last compilation step to the machine that has the answer**:

> Ship optimized MIR next to the program. On first run, identify the GPU. If it is one we have a target descriptor for, compile the MIR to that GPU's ISA, cache the result, and restart into it. If it is not — or if there is no GPU, or anything at all goes wrong — run the **CPU sibling**, which is compiled into the main executable and is always present.

Three properties fall out of this that are worth stating as *goals*, not side effects:

1. **The program is never broken by an unknown GPU.** The set of GPUs we recognize is a shipped, versioned config set; a machine outside it is an ordinary, expected, non-error outcome. This is what makes a real-ISA strategy survivable at all — the fallback is not a degraded mode, it is the correct program.
2. **The CPU sibling is the differential oracle, not just a fallback.** ★ This is the load-bearing test insight of the whole plan. Every GPU-lowered function has, by construction, a host-lowered twin from the *same* HIR — so any GPU codegen defect is detectable by running both and comparing, on real hardware, without a reference GPU compiler in the loop. That is precisely the method the tree already uses to convict CPU miscompiles (a `release` arm compared against `debug`, an arm64 leg against x86_64); the CPU sibling extends it to a target whose output we otherwise could not check. It is also why §2.14 makes the sibling **mandatory** rather than opt-in: an unpaired GPU function is an *unverifiable* GPU function.
3. **The GPU stops being a build-time decision.** One artifact set, every machine.

⚠ **The honest cost, stated up front:** this hands an end-user machine a compiler and asks it to write an executable artifact at runtime. That is a real security, packaging and platform-policy surface (W^X, hardened runtime, AMFI, antivirus heuristics, and platforms — iOS, some consoles — where it is simply forbidden). §4.9 owns it; it is not hand-waved.

---

## 2. Design — Track B: MIR → GPU code generation

> **§2.1–§2.10 are rev 3's and are preserved as written** (numbering frozen per §0.1; only two stale internal cross-references were repointed when the tail sections shifted). §2.11–§2.14 are rev 4.

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

- Shader **types** (`Sampler`, `Texture<>`, `UAV<>`, `ConstantBuffer<>`, `WorkgroupShared<>`): registered via the language schema's `typeExtensions[]` block (v3, SP2) into the `TypeRegistry` (per [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) §2.2). Listed in the "Shader-shape extensions" table below.
- Shader **HIR kinds** (`WorkgroupBarrier`, `DerivativeX`/`Y`, `TextureSample`, `TextureLoad`, `ImageStore`, `AtomicOp`, etc.): registered via the language schema's `hirLowering` block (v4, planned per [`09-hir-plan`](./09-hir-plan%20-%20ok.md) §2.5) into the `HirKindRegistry` (`HirKind ≥ 256`). They are **registered HIR extension kinds, not hardcoded core HIR enum members** — the open core + per-language registered-extensions pattern from `09-hir-plan` §2.2 applies. Listed in the table immediately below.

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

Lattice membership (per [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) §2.2):

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

Both lowerings produce instructions with `HirSourceMap` (`HirAttribute<HirSourceLoc>`) pointing at the same user-authored function. Debug info per `15-debug-info-plan` reflects the dual nature (DWARF + SPIR-V `OpLine` both reference the same source span).

### 2.5 Lowering strategy

**HIR → MIR → SPIR-V** (default).

Reuse the MIR optimizer (constant folding, DCE, copy propagation). Structured-CF markers from `12-mir-lir-plan §2.3` map 1:1 to SPIR-V.

**Shader decorations migrate to MIR via the `MirShaderAttribute` side-table** (per plan 12 §3.1 D-ML2-2.1). HIR carries shader metadata in `ShaderIntrinsic` + `HirShaderMap` (per §2.2 above); HIR→MIR lowering populates a parallel `MirShaderAttribute` side-table (mirror of `MirSourceMap`) so the MIR→SPIR-V walker reads its decoration vocabulary from MIR directly. The walker NEVER reaches back into HIR — that would violate the layering. Side-table contents: per-MIR-function entry-point stage + workgroup-size; per-MIR-symbol binding + descriptorSet + location + storage-class + builtin tag. This is the load-bearing prerequisite for the bypass-LIR SPIR-V path; without it the walker has nothing to decorate from.

**`spirv.target.json` declares `abiModel: "result-id"`** (per plan 12 §3.1 D-ML5-X.1). The schema's validate() rules per-shape mandate `decorations[]` + `storageClasses[]` + `entryPoints[]` for result-id targets (parallel to register-machine's mandated `registers[]` + `callingConventions[]`). Lands when the second target class lands.

Structured-CF markers from `12-mir-lir-plan §2.3` map 1:1 to SPIR-V:
- `LoopHeader` / `LoopLatch` / `LoopExit` → `OpLoopMerge` + `OpBranchConditional`
- `IfThen` / `IfElse` / `IfJoin` → `OpSelectionMerge` + `OpBranchConditional`
- `SwitchHead` / `SwitchCase` / `SwitchJoin` → `OpSelectionMerge` + `OpSwitch` (the `SwitchJoin` block carries the merge label)

Open question §6.1 (rev 3 numbered this §4.1; the tail shifted at rev 4): should we add a `HIR → SPIR-V direct` path that skips MIR for trivial leaf shaders? Default: no — reuse MIR for everything.

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

### 2.9 `SpirvVerifier` — in-tree structural verifier (v1.x scope)

The bypass-LIR SPIR-V path doesn't get the `LirVerifier` correctness gate native targets rely on. Production correctness must NOT depend solely on external oracles (`spirv-val`, reference drivers) — those are correctly excluded from the build per the hermetic invariant, but that means at compile time we have only "trust the walker" unless we add a structural verifier.

`SpirvVerifier` runs **AFTER the MIR→SPIR-V walker emits a SPIR-V module** and **BEFORE the binary encoder serializes it**. Mirrors `LirVerifier`'s discipline (bucket-2 rules over a JSON-declared opcode vocabulary). v1.x mandatory — the same correctness tier `verifyLirText` occupies for `.dsslir`. Rule families:

| Rule family | What it checks |
|---|---|
| `checkResultIdMintedBeforeUse` | Every `%result` referenced in an operand position was minted by an earlier producer instruction in the module |
| `checkResultIdUniqueness` | Every `OpResultId` is unique across the module (no double-mints) |
| `checkDecorationTargetValid` | Every `OpDecorate`/`OpMemberDecorate` target-id refers to an extant declaration |
| `checkStorageClassCompatible` | `OpVariable`'s storage-class matches the pointer-type it produces (per SPIR-V §3.7) |
| `checkCapabilityDeclared` | Every opcode that requires a capability (e.g. `OpControlBarrier` → `WorkgroupBarrier`) has `OpCapability` declared at module head |
| `checkStructuredCfBalance` | Every `OpLoopMerge`/`OpSelectionMerge` has matching merge-target + continue-target structure (parallel to plan 12 MirVerifier's `checkStructCfMarkers`) |
| `checkEntryPointInterface` | Every `OpEntryPoint` interface-id list is well-formed (declared variables, storage-class IO matching stage) |

New `V_*` diagnostic family at 0x7xxx (V for SPIR-V verifier — `S_*` is already Semantic; parallel to `L_*` 0xBxxx for LIR, `W_*` 0x6xxx for WAT verifier per plan 18 §2.9b). **Allocation per the central nibble registry in [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.2 — earlier draft proposed `SV_*` 0xC2xx which silently collided with the shipped `C_*` config family (0xC001..0xC033 in `parse_diagnostic.hpp`).** The reference oracles `spirv-val` and `spirv-cross` REMAIN test-only — used to cross-check that every `SpirvVerifier`-clean module is also `spirv-val`-clean. **The contract is: every emitted .spv must pass `SpirvVerifier` BEFORE the encoder writes bytes. A spirv-val failure on a SpirvVerifier-clean module is a verifier-rule gap that gets filed and folded, not a "ship it anyway" outcome.**

### 2.10 SPIR-V binary minifier (v1.x scope — NOT post-v1)

Parallel to plan 18 §2.11 (WASM minifier). v1.x mandatory — the consumer ecosystem (Vulkan loaders, Metal/DirectX translation layers, mobile GPU drivers) penalizes oversized SPIR-V modules at load time. We own the entire pipeline including stripping.

**Runs AFTER the binary encoder, BEFORE writing the `.spv` artifact.** Bucket-2 algorithm over a JSON-declared strip-rule schema on `spirv.target.json`:

```jsonc
"minifier": {
  "stripRules": [
    { "name": "drop-opname-debug",    "params": [] /* drop `OpName` / `OpMemberName` decorations */ },
    { "name": "drop-opstring",        "params": [] /* drop `OpString` source-file decorations */ },
    { "name": "drop-opsource",        "params": [] /* drop `OpSource` / `OpSourceExtension` decorations */ },
    { "name": "drop-opline",          "params": [] /* drop `OpLine` debug-info */ },
    { "name": "remap-result-ids",     "params": [] /* renumber sparse result-ids to dense (smaller LEB-equivalent in v1.x = smaller word offsets in v2+) */ },
    { "name": "dead-decoration-elim", "params": [] /* drop decorations on result-ids that don't appear in any instruction */ }
  ],
  "profiles": {
    "debug":   { "enabled": [] },                                                                                          // full debug info
    "release": { "enabled": ["drop-opname-debug", "drop-opstring", "drop-opsource", "dead-decoration-elim"] },
    "minified":{ "enabled": ["*"] }                                                                                        // drop all debug + remap ids
  }
}
```

Same shape-keyed dispatch as the format encoder (closed strip-rule vocabulary; each rule has one bucket-2 implementation; engine consults `profile.enabled[]` data, no identity branches). Profile flows from artifactProfile: `gpu-debug` → `debug`; `gpu-release` → `release`; `gpu-mobile` → `minified`.

**Acceptance** (folded into §7; rev 3 numbered it §5): `minified` profile reduces a custom-language compute-shader corpus `.spv` by ≥ 25% vs `debug`, with byte-identical execution under the spirv-val + reference-driver oracles. Hashed in CI.

**Why NOT a `spv-opt`-equivalent dependency:** the hermetic invariant rules out `spirv-tools` invocations at compile time. The minifier is in-tree like plan 18's; `spirv-opt` / `spirv-cross` appear only in TEST fixtures as oracles.

### 2.11 GPU ISA backends (rev 4) — and the claim that a GPU is a register machine

Rev 3 treated "GPU backend" as one thing and priced it as proprietary-driver-equivalent work. That price was set against the wrong shape. **A GPU ISA is a register machine** — NVIDIA SASS has a numbered general register file plus predicate and uniform registers; AMD RDNA has VGPRs and SGPRs; Apple AGX and Intel Xe likewise have a general register file. ⚠ *Register-file sizes and class structure are deliberately not stated here* — they are per-architecture facts that §3.2's register-pressure ladder MEASURES, and a number written into a plan from memory is exactly the kind of unmeasured claim this plan is built to avoid. The tree already owns a *target-blind, JSON-driven* register-machine pipeline: MIR→LIR isel, liveness, register allocation, two-address legalization, calling-convention materialization and the encoder are all parameterized by `*.target.json` and contain no architecture identity branches (this is the ML5 cycle-2a existence proof that [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) cites as the template for the whole back half).

So the working hypothesis — and it is a **hypothesis, to be tested by Track A before any engine change is proposed**, not an assumption — is:

> A GPU ISA target declares `abiModel: "register-machine"` and reuses the existing lowering chain, with the GPU-specific facts arriving as **new declared vocabulary** in `<gpu>.target.json` rather than as new engine paths.

⚠ Four things are genuinely different, and pretending otherwise is how this becomes a silent miscompile factory. Each is named here as *vocabulary to be designed*, not as a branch to be written:

| Divergence | What it is | Why it is not just "an opcode table" |
|---|---|---|
| **Memory spaces** | global / shared / local / constant / kernel-param, each with its own load-store encodings and coherence rules | MIR pointers are flat today. An address-space concept has to reach MIR, and a pointer that loses its space is a wrong-memory access, not a slow one. |
| **Predication + divergence** | SIMT: a branch is per-lane. AMD carries an explicit `EXEC` mask; NVIDIA uses predicates plus reconvergence points | MIR's structured-CF markers (§2.5) give the *shape* the reconvergence needs, but mask materialization is a real lowering pass with no CPU analogue. |
| **Launch ABI** | Kernel parameters arrive in a constant bank / kernarg segment, not a stack frame; there is no conventional call stack | `callingConventions[]` as declared today describes arg registers and stack slots. A kernarg segment is a third shape. |
| **Register budget as a correctness *and* occupancy constraint** | The allocator must hit a per-kernel register target; overshooting spills to slow memory and collapses parallelism | Existing regalloc treats spilling as a cost. Here it is closer to a bar. |

**The container is a separate question from the ISA, and rev 4 answers it the same way rev 3 answered SPIR-V's:** a new `*.format.json` plus an `ObjectFormatBackend` implementation registered in `objectFormatBackendTable()` — the exact extension point `elf`/`pe`/`macho`/`wasm`/`spirv` already use. Usefully, **three of the four vendor containers are ELF-shaped** (NVIDIA cubin, AMD code object, Intel zebin); only Apple's `.metallib` is its own thing.

★ **What the GPU artifact is NOT.** The operator brief says "compile MIR into GPU lib," and there is one easy way to build the wrong thing here. The **GPU artifact is a vendor GPU module** (cubin / code object / `.metallib` / zebin) loaded by the *driver API* — it is **not** a host `.dll`/`.so`/`.dylib`, and it is never loaded by the host dynamic loader. The pe/elf/dylib requirement in the brief attaches to a different artifact: the **compiler** shipped as a native library, which is §4.6 — and which, as measured, **already exists** (`add_library(dss-code-prime-lib SHARED …)` builds a `.dll`/`.so`/`.dylib` today).

### 2.12 The vendor virtual ISA is an oracle, not a product tier

Every in-scope vendor exposes a documented rung above the ISA: PTX (NVIDIA), AIR/LLVM bitcode (Apple), LLVM IR with published GCN/RDNA ISA docs (AMD), SPIR-V (Intel). Rev 4 does **not** ship those as the artifact — that option was considered and declined. It uses them as a **differential instrument during bring-up**: for a given probe, compare *our* ISA against the vendor compiler's ISA for the same virtual-rung input, which localizes a defect to one lowering step instead of to "the GPU backend."

This is not a hedge and it is not a fallback tier; it is the same method the tree uses to convict CPU codegen (release vs debug, arm64 vs x86_64, and now the CPU sibling of §2.14). It lives in test fixtures, never in the production pipeline.

### 2.13 The MIR sidecar becomes a shipped artifact — three consequences

Track C ships optimized MIR next to the program. The carrier already exists and is better than it needs to be: `src/mir/mir_text.{hpp,cpp}` implements `.dssir`, with a **byte-identical round-trip contract** (`emitMir(parseMir(emitMir(m))) == emitMir(m)`) and `MirVerifier` run on load. Three things are nonetheless true today and all three are load-bearing:

1. ★ **`.dssir` cannot round-trip side-tables, and §2.5 requires exactly that.** `mir_text.hpp` states it plainly: *"No 5 side-tables. Side-table support deferred until ML2 starts populating a MirSourceMap."* But §2.5's whole design is that shader decorations reach the MIR→GPU walker via the `MirShaderAttribute` side-table, because the walker may not reach back into HIR. **A sidecar that drops the side-table produces an undecorated module: no entry-point stage, no workgroup size, no bindings.** This is the single hardest prerequisite in Track C and it belongs to Track B ⇒ [[D-GPU-MIR-SIDECAR-SIDETABLE-ROUNDTRIP]].
2. **Nothing wires `.dssir` into the driver.** `emitMir`/`parseMir` are called only from tests — `tests/mir/test_mir_text.cpp` and `tests/mir/test_mir_lowering_c_subset.cpp` — and from `mir_text.cpp` itself; nothing under `src/program/` touches them, there is no `--emit-mir` flag and no reload path. The format is real and tested, but it is dormant tooling, not a pipeline stage.
3. **Shipping it promotes it to a versioned compatibility surface.** A cached GPU artifact is keyed on the sidecar's content (§4.4), so a format change is a cache invalidation, not a load failure — but a sidecar written by one compiler version and read by another must either verify-and-refuse or be versioned. ⇒ [[D-GPU-MIR-SIDECAR-FORMAT-STABILITY]]. **Recommendation: ship the text form first** (its round-trip is already proven and a human can read a bug report's sidecar), with a binary form behind the same version field once size is measured to matter rather than assumed to.

### 2.14 The CPU sibling is mandatory, and the verifier enforces it

§2.4 makes dual lowering *available* to a function tagged both `[[shader.usable]]` and `[[host.usable]]`. Rev 4 makes it **required** for any entry point reachable through Track C's dispatch path, and adds the verifier rule that says so:

- `SH_GpuEntryPointWithoutHostSibling` — a compute entry point that Track C may dispatch, whose enclosing function is not also host-lowerable, is rejected at HIR verification time.

Two reasons, and the second is the important one: without a sibling there is no fallback when the GPU is absent or unrecognized (§1.1 property 1), and without a sibling there is **no oracle** — the GPU output becomes unverifiable except against a vendor compiler we have deliberately excluded from the pipeline (§1.1 property 2).

⚠ **Precision, because the rule is wrong if stated too broadly:** this binds **compute entry points dispatched through Track C**. It does *not* bind graphics stages — a vertex or fragment shader has no meaningful CPU sibling, and demanding one would be a rule invented to be tidy rather than to catch anything. ⇒ [[D-GPU-CPU-SIBLING-MANDATORY]] carries the exact scope test.

---

## 3. Design — Track A: GPU identification and target derivation

### 3.1 What the identifier is — and why it is not a hermeticity breach

`ZZ-final-goal` §2.3 is categorical: *"OS-supplied runtime libs, browsers, GPU drivers, and Apple developer certs are FFI targets and credentials — never tools."* Track A does not violate that, and the reason is a distinction the tree already draws in three tiers:

| Tier | Example already in the tree | External tools? |
|---|---|---|
| **Production pipeline** | `dss-code-prime` compiling sqlite | **Zero.** No `ld`/`clang`/`dxc`/`codesign`. |
| **CI oracles** | `spirv-val` cross-check (§2.9); the gcc reference leg in the sqlite harness | Yes, in tests only — never gating an emitted byte. |
| **Config-authoring measurement** | `x86_64.target.json`'s endianness rows, MEASURED 2026-08-04 with `clang-19 -dM -E -x c /dev/null -target …`; shipped-lib symbols verified with `nm -D` / `objdump -p` before being declared | Yes — run by a human, recorded as provenance, output committed as reviewed config. |

**Track A is tier three, industrialized.** It compiles probe programs with `nvcc`/`metal`/`hipcc`/`ocloc`, disassembles with `nvdisasm`/`llvm-objdump`/`iga64`, and derives `<gpu>.target.json` from what came back — the same act as reading `clang -dM -E` output to learn what `__BYTE_ORDER__` should be, applied to an instruction encoding instead of a macro.

★ **The invariant that keeps this honest, stated as a falsifier:** *no output of the identifier is consumed at build time or at runtime — only committed, human-reviewed config is.* If a DSS build ever requires `nvcc` to be installed, or if the identifier is ever invoked from CMake or from the first-run path, the carve-out has been violated and this section is wrong.

### 3.2 The probe corpus

Probes are small programs — C through each vendor's C dialect first, DSS Axis once plan 20/24 provides it — and they follow the `examples/c/` discipline exactly: **each probe isolates ONE fact**, so that a disassembly diff attributes cleanly to it, and each carries a `$comment` recording what is red when the fact changes. A probe that exercises five things at once produces a listing nobody can attribute.

| Family | Isolates | Yields |
|---|---|---|
| Scalar arithmetic ladder | one operation × one width × one type per probe | opcode mnemonics, operand shapes, width encoding |
| Address-space ladder | one load/store per space (global/shared/local/constant/param) | space-qualified encodings — the §2.11 memory-space vocabulary |
| Control-flow shapes | `if` / `loop` / `switch`, uniform vs divergent condition | branch idiom, predication, reconvergence/mask discipline |
| Barrier + atomic set | one sync primitive per probe | synchronization opcodes and their scope operands |
| Kernel signature ladder | 0/1/N params, scalar vs pointer vs aggregate | **launch ABI** — where parameters actually land |
| Register-pressure ladder | live-value count stepped upward until spilling | register file size, spill idiom, the occupancy cliff |
| Built-in variable set | thread/block/grid index reads | special-register reads |

★ **The derivation must be differential.** A single disassembly listing conflates *the ISA* with *the vendor compiler's choices for that program*. Facts are derived from **diffs between probes that differ in exactly one dimension** — the same reason the c-subset corpus mutates one keyword row at a time and records which arms go red.

### 3.3 Per-vendor toolchain matrix

| Vendor | Compile | Disassemble | Machine-readable metadata | Container | Virtual rung (§2.12) |
|---|---|---|---|---|---|
| **NVIDIA** | `nvcc` | `cuobjdump -sass`, `nvdisasm` | `cuobjdump -elf` (register counts, shared-mem) | cubin (**ELF**) | PTX (`nvcc --ptx`, publicly specified) |
| **AMD** | `hipcc` / `clang -x hip` | `llvm-objdump -d --mcpu=…` | `.note` msgpack **kernel descriptor** (VGPR/SGPR counts, kernarg layout) | code object (**ELF**) | LLVM IR + **published GCN/RDNA ISA manuals** |
| **Intel** | `ocloc` | `ocloc disasm`, IGA (`iga64`, open-source) | zebin `.note` | zebin (**ELF**) | SPIR-V — which Track B already emits |
| **Apple** | `xcrun metal` | ⚠ weakest link — no vendor ISA disassembler | `.metallib` reflection | `.metallib` (own format) | AIR (LLVM bitcode) |

Two asymmetries worth planning around rather than discovering: **AMD is the easiest real-ISA target** (full public ISA docs, an open LLVM backend, and a kernel descriptor emitted in machine-readable form — it hands us most of §2.11's launch-ABI answer directly), and **Apple is the hardest** (AGX is undocumented and Apple ships no ISA disassembler, so AGX derivation depends on differential inference from AIR→ISA pairs and is the one leg that may not reach ISA at all in v1.x). Sequencing should reflect that, not alphabetical order.

### 3.4 From disassembly to `<gpu>.target.json`

The identifier emits a **candidate** descriptor plus a **provenance record** — for every derived row: the probe that produced it, the exact command line, the toolchain version, and the raw listing excerpt. A human reviews and commits.

⚠ **It never auto-commits, and this is not caution — it is the config convention.** Every shipped config in this tree carries prose recording *how* a value was measured and *what goes red if it is wrong*; those `$comment` blocks are the reason a later cycle can tell a measurement from an inference. A generated-and-committed file would erase exactly that, and `.plans`/descriptors are hand-aligned for the same reason.

### 3.5 Where GPU target configs live — and the loader constraint (MEASURED)

The brief specifies `src/dss-config/targets/gpus/`. **As specified, that path is not reachable today**, and the reason is a deliberate security control rather than an oversight:

- `findShippedConfig` (`src/core/types/config_path_walk.cpp:18-26`) **rejects any `loc.name` containing `/` or `\`** — an explicit `../`-traversal defense covering callers that forward an untrusted name (LSP requests, driver flags).
- `TargetSchema::loadShipped(name)` composes the flat path `src/dss-config/targets/<name>.target.json` (the diagnostic at `src/program/program.cpp:305` states this literally).

So a target named `gpus/nvidia-sm86` is refused before the filesystem is touched. The mechanism to fix it already exists — `ShippedConfigLocator` **already carries a `subdir` field**, which is how `targets/` vs `object-formats/` vs `sources/` is selected — so the work is to let a target *family* declare its subdirectory and have the resolver consult a **closed, declared set** of subdirs.

⚠ Two constraints on that change, both non-negotiable: the traversal defense must survive (the subdir comes from declared config, **never** from the requested name), and the standing agnosticism veto forbids any `if (name looks like a GPU)` branch in the resolver — the resolver learns "these are the subdirs to search," not "GPUs are special." ⇒ [[D-GPU-TARGET-CONFIG-SUBDIR-RESOLUTION]].

### 3.6 Runtime GPU identity — the detection half

Detection runs on the **end user's** machine and therefore may use **none** of §3.3's tooling. Two levels, both data:

1. **Driver-API enumeration** — the same API Track C needs for dispatch anyway (`cuDeviceGetAttribute(COMPUTE_CAPABILITY…)`, `MTLDevice`, `hipGetDeviceProperties`, `zeDeviceGetProperties`). This yields the architecture identity the target descriptor is keyed on.
2. **PCI vendor:device** as corroboration, via ordinary OS enumeration.

★ **Matching must be an equality test over declared data, never a heuristic.** The identity key a `<gpu>.target.json` declares is exactly the key detection produces; "close enough" matching on a GPU ISA is a silent miscompile. An unmatched device is a normal outcome (§4.2 step 4), not an error.

**Driver version is not part of identity but is part of the cache key** (§4.4): the same chip under a newer driver can change kernel-metadata expectations even when its ISA does not. ⇒ [[D-GPU-ISA-DRIVER-VERSION-DRIFT]].

### 3.7 Harness shape

`scripts/gpu-identifier/gpu-identifier.{ps1,sh}` are **thin launchers over one shared `gpu-identifier.py`** — the `scripts/pragma-profile-census/` pattern verbatim, adopted for its stated reason: both launchers call the same implementation, *"so the two platforms cannot drift into disagreeing about the corpus."* The `.ps1` never shells out to bash. Exit-code propagation follows the same discipline (`$LASTEXITCODE` captured into a local immediately, nothing between — and `rc` read directly, never after a pipe).

⚠ **No machine has all four vendor toolchains, and probably no machine ever will.** Each vendor leg is independently skippable, and **a skip is REPORTED with its reason, never silent** — the same rule the sqlite harness follows for `skipped-by-runOn` legs, and for the same reason: silence about a leg reads as coverage. ⇒ [[D-GPU-IDENTIFIER-TOOLCHAIN-AVAILABILITY]].

---

## 4. Design — Track C: deployment, first-run compilation, dispatch

### 4.1 The shipped artifact set

| Artifact | What it is | Built when |
|---|---|---|
| **Launcher shim** — `<app>` | The binary the user invokes. Small, dependency-light, contains **no compiler**. Does detection, cache lookup, and (if needed) spawns the compiler; then launches the payload. Emitted by DSS itself, per host format. | Build time |
| **Main executable** — `<app>-main` | The real program. Contains **every CPU sibling** (§2.14) and the dispatch runtime (§4.7). Runs correctly with or without a GPU artifact. | Build time |
| **MIR sidecar** — `<app>.dssir` | Optimized MIR for the GPU-dispatched functions **only** — not the whole program. §2.13. | Build time |
| **Runtime compiler** — `dss-code-prime.{dll,so,dylib}` + a small spawnable driver | The MIR→GPU-ISA compiler. Reused, not newly built: this SHARED library **already exists**. §4.6. | Build time (ships as-is) |
| **Shipped GPU config** — `targets/gpus/*.target.json` (+ formats) | The set of GPUs this program can compile for. A device outside this set is the §4.2-step-4 fallback. | Build time |
| **GPU module** — `<key>.<vendor-ext>` | cubin / code object / `.metallib` / zebin. **Produced on the end-user machine.** Not a host shared library (§2.11). | **First run** |
| **Manifest** | Records GPU identity, driver version, sidecar hash, compiler version, target-config version, and any poison marker (§4.8). | **First run** |

### 4.2 The first-run state machine

Every step's failure edge lands on the same place — the CPU sibling — and every one records why.

```
shim start
  │
  ├─1─ read manifest (absent ⇒ first run)
  │
  ├─2─ resolve GPU identity via driver API (§3.6)
  │       └─ none / no driver ────────────────────────► launch main, CPU mode ✔
  │
  ├─3─ compute cache key (§4.4); artifact present + key matches
  │       └─ hit ─────────────────────────────────────► launch main, GPU artifact ✔
  │
  ├─4─ is identity in the shipped target set? (equality, §3.6)
  │       └─ no ── record "unsupported: <identity>" ───► launch main, CPU mode ✔
  │
  ├─5─ spawn compiler child (sidecar, target, out-path)
  │       └─ failure ── record poison marker (§4.8) ──► launch main, CPU mode ✔
  │
  └─6─ write artifact atomically (temp + rename), update manifest
          └──────────────────────────────────────────► launch main, GPU artifact ✔
```

★ **The launcher shim removes the need for the restart, and that is a deliberate improvement on the brief.** The brief asked for: run on CPU → detect → compile → *"safely exit before the user can effectively use it"* → restart into the GPU build. The goal behind that phrasing is that the user must never get a half-initialized session. A shim achieves the goal **structurally** — all of steps 1–6 happen *before the payload process ever starts*, so there is no session to abandon, no argv/stdio/cwd fidelity problem, and no self-exec loop to guard against.

The cost is honest and belongs in the open questions: **first launch pays compile latency up front** instead of running on CPU while compiling. Two policies, and the choice is a knob rather than an architecture change:

- **`eager`** (default) — step 5 blocks; first launch is slower, and is GPU-accelerated from its first frame.
- **`deferred`** — the shim launches CPU mode immediately and compiles in the background; the *next* launch is accelerated. Closest to the brief's original sequencing, and the right default for interactive apps with a visible startup.

⇒ §6 Q13.

### 4.3 The launcher shim

A small native executable per host format, **built by DSS itself** (dogfooding: the shim is exactly the kind of small, dependency-light program the toolchain should be able to emit). It links no compiler and spawns rather than loads, so its own dependency set stays minimal — which matters because it is the process the user's shortcuts, package manager and OS integration point at. Naming, install layout and packaging of the pair belong to [`26-publish-plan`](./26-publish-plan%20-%20tbd.md), not here; this plan owns only the requirement that the pair exists and which member does what.

### 4.4 Artifact location and cache key

**Location:** the main executable's own directory first, the per-user cache directory (`%LOCALAPPDATA%` / `$XDG_CACHE_HOME` / `~/Library/Caches`) as fallback. The exe directory covers portable and development installs; the fallback covers read-only and system-wide installs (`Program Files`, `/usr/bin`) and multi-user machines, where the exe directory is not writable and two users may have different GPUs.

**Key** — content-addressed over everything whose change invalidates the artifact:

```
key = H( gpu-identity ∥ driver-version ∥ H(<app>.dssir) ∥ dss-compiler-version ∥ target-config-version )
```

★ **A stale entry must be a MISS, never a MISMATCH.** Every input that could make a cached artifact wrong is *in the key*, so the failure mode of staleness is recompilation, not the execution of a kernel built for a different chip. Writes are atomic (temp + rename) so a killed first run cannot leave a torn artifact that a later run trusts. Concurrent first launches either take a lock or perform benign duplicate work — never a partial read.

### 4.5 The CPU sibling contract

The sibling lives in the main executable and is **always present** (§2.14 makes this a verifier-enforced property, not a convention). Selection at runtime is a **data lookup** — "is a GPU module loaded that provides this kernel?" — never a compile-time branch and never an identity test on vendor or device.

### 4.6 The runtime compiler

**Reuses `dss-code-prime-lib` as-is** (operator decision 2026-08-12). This costs nothing to build because it already exists: `src/CMakeLists.txt:58` aggregates all **22** object libraries (counted at lines 59–80) into one SHARED library with hidden default visibility and `DSS_EXPORT`-marked exports, and `dss::Program` already exposes a programmatic `compileFiles`/`compileProject` surface — the CLI is a thin wrapper over it.

**The compiler runs in a spawned child process** (operator decision). A compiler fault, OOM or hang cannot take down the application; the failure surfaces as an exit code the shim can record and act on; and the application binary does not inherit the C++ runtime the compiler was built against.

Two consequences to plan for rather than discover:

- ⚠ **The child needs `src/dss-config/` at runtime.** Config resolution walks the cwd or reads `DSS_CONFIG_ROOT`; a deployed compiler with no config tree resolves nothing. The shipped set must therefore include the GPU target descriptors and object-format descriptors the compile will touch. This is a packaging requirement, not a detail.
- ⚠ **We ship the entire frontend to run a path that needs none of it.** The MIR→ISA route uses `core`/`mir`/`opt`/`lir`/`asm`/`link`; the tokenizer, parser, preprocessor, semantic analyzer and HIR layers are dead weight in the redistributable. Reusing the existing library is the right *first* move — it proves the mechanism before optimizing it — but the carve-out is real work that should be driven by a **measured** object-library dependency set rather than a guessed one. ⇒ [[D-GPU-RUNTIME-COMPILER-MINIMAL-LIB]].

### 4.7 The dispatch runtime — and the eager-import hazard

Track C owns dispatch (operator decision): enumerate devices, load the GPU module, launch kernels, move buffers. Per `ZZ-final-goal` §2.3, the driver is an **FFI target**: `libcuda`/`nvcuda.dll`, `Metal.framework`, `libamdhip64`, Level Zero.

★★ **This is where the tree's own machinery would produce exactly the wrong outcome, and the plan exists partly to stop it.**

`[[D-FFI-DESCRIPTOR-EAGER-IMPORT]]` is a load-bearing invariant here: DSS **eager-imports every symbol a shipped-lib descriptor declares**, whether or not the program calls it (`semantic_analyzer.cpp` injects the descriptor's entire `symbols` list into scope, and each becomes a load-time import). One absent symbol breaks the **entire binary's load** — PE `0xC0000139`, ELF exit 127.

GPU driver libraries are **absent by definition on the machines the CPU fallback exists for**. So:

> Declaring the GPU driver API as an ordinary shipped-lib descriptor would make every DSS-built GPU-capable program **fail to load** on any machine without that vendor's driver installed — the precise inverse of the §1.1 guarantee this whole track is built around.

The dispatch FFI must therefore be **late-bound**: `dlopen`/`LoadLibrary` + per-symbol resolution, with absence as an ordinary, expected runtime outcome routing to the CPU sibling. ⚠ **DSS has no late-binding FFI surface today** — every FFI path is descriptor-declared and eager. That capability is new work owned by this track, and it is worth designing generally rather than for GPUs specifically, because *every* optional dependency has this shape. ⇒ [[D-GPU-DISPATCH-FFI-LATE-BOUND]].

The scope of the launch path itself: enumerate, load module, set kernel args, launch, synchronize, copy buffers in/out. Streams, async pipelining, multi-GPU scheduling and unified memory are **deferred with named anchors**, not silently omitted.

### 4.8 Failure posture

**Fail-loud does not mean fatal.** Every failure below is recorded with a real diagnostic and routes to a correct program:

| Failure | Outcome |
|---|---|
| No driver library present | CPU sibling. Recorded once. |
| Driver present, no device | CPU sibling. Recorded once. |
| Device identity not in shipped target set | CPU sibling. Identity recorded verbatim so it can become a future target. |
| Sidecar missing / fails `MirVerifier` on load | CPU sibling. **Loud** — this is a build or packaging defect, not an environment fact. |
| Compiler child fails or times out | CPU sibling + **poison marker keyed on the cache key**. |
| GPU module fails to load in the driver | CPU sibling + poison marker. |
| Kernel launch fails at runtime | CPU sibling for that call. Loud. |

★ **The poison marker exists so a reproducible failure is not retried on every launch** — otherwise a permanently-failing compile costs the user a compiler run per start, forever. It is keyed on the full cache key, so any change that could plausibly fix the failure (new driver, new app version, new compiler) clears it automatically; a documented manual clear exists for the rest.

⚠ **A silent fallback is a defect, not a feature.** If the program quietly runs 40× slower because a GPU compile failed and nobody was told, that is the same class of bug as a silent miscompile: the wrong outcome, unobserved. Every fallback is recorded and queryable.

### 4.9 Runtime code generation is a platform-policy surface

Writing an executable artifact on an end-user machine at runtime is a real constraint surface, and it is where this design is most likely to meet an unmovable object:

- **macOS / AMFI.** DSS already emits its own ad-hoc `CS_SuperBlob` signatures that AMFI accepts on real Apple Silicon (TF-C121), so the capability exists. ⚠ But do **not** assume it transfers: a `.metallib` handed to the Metal driver may not traverse the AMFI mach-o mapping path at all. **This must be measured, not inferred** — the tree has been burned before by a true measurement carrying a false inference.
- **Windows.** Write-then-execute is a heuristic antivirus reacts to; artifact location and file naming matter more than they should.
- **Linux.** `noexec` mounts and SELinux/AppArmor policy can forbid the cache location.
- **Forbidden outright.** iOS/tvOS, consoles and some enterprise lockdowns do not permit runtime code generation at all. **The design is already correct there** — those platforms take the CPU sibling permanently, which is a supported outcome rather than a failure.

⇒ [[D-GPU-ARTIFACT-RUNTIME-CODESIGN]].

---

## 5. PR breakdown

> Three tracks, three prefixes (§0.1). **SG1–SG13 are rev 3's and are unchanged**, including their numbering — plan 00 cites the `SG1–SG10` range, and renumbering would falsify it.

### 5.1 Track B — MIR → GPU code generation (`SG*`)

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
| SG11| **`SpirvVerifier` substrate** (per §2.9)           | 7 rule families per §2.9 table. New `V_*` diagnostic family at 0x7xxx (per the central nibble registry in plan 00 §1.2). v1.x mandatory — production correctness gate, not test-only oracle. Same correctness tier `verifyLirText` occupies for `.dsslir`. |
| SG12| **SPIR-V minifier substrate** (per §2.10)          | Strip-rule schema + engine + the four `release`-profile rules (drop-opname-debug, drop-opstring, drop-opsource, dead-decoration-elim). v1.x mandatory — gates "shader backend done." |
| SG13| Minifier `minified`-profile rules                | drop-opline + remap-result-ids (the size-aggressive rules). Closes the ≥ 25% size-reduction acceptance bar. |

Substrate tier for SG1, SG6 (touch lattice + structured-CF contract), SG11 (`SpirvVerifier` rule families), SG12 (strip-rule schema). **SG11/SG12/SG13 are mandatory v1.x deliverables, NOT deferred.**

Rev 4 adds the real-ISA half. These are gated on Track A having measured the ISA in question (§0.1), and each is **per-vendor**, not one omnibus PR:

| PR  | Title                                            | Scope |
|-----|--------------------------------------------------|-------|
| SG14| **MIR address-space vocabulary**                  | The §2.11 memory-space concept reaching MIR + the type lattice: global / shared / local / constant / kernel-param. Substrate tier — touches MIR's pointer model, so it lands before any ISA backend. Red-on-disable: a pointer that loses its space must fail loud, never silently become global. |
| SG15| **Divergence + predication lowering**             | Exec-mask / predicate materialization from the §2.5 structured-CF markers. A MIR→LIR pass, target-blind, parameterized by the target's declared divergence model. |
| SG16| **Kernel launch ABI in `callingConventions[]`**   | The kernarg/constant-bank parameter shape as a third declared convention alongside register+stack. Schema + validate() rules. |
| SG17| **First ISA backend — AMD (RDNA/GCN)**            | Sequenced first, not alphabetically: public ISA manuals + open LLVM backend + a machine-readable kernel descriptor make it the cheapest place to discover what §2.11's hypothesis got wrong. Code-object (ELF) container backend + `<gpu>.target.json` encodings. |
| SG18| **ISA backend — NVIDIA (SASS)**                   | cubin (ELF) container + SASS encodings from Track A's derivation. |
| SG19| **ISA backend — Intel (Xe)**                      | zebin (ELF) container + Xe encodings. Intel also consumes SPIR-V natively, so SG19 can be differentially checked against Track B's own SPIR-V output. |
| SG20| **ISA backend — Apple (AGX)**                     | ⚠ Highest risk, sequenced last: no vendor ISA disassembler (§3.3). May land as AIR-only in v1.x with the ISA step deferred behind a named anchor rather than forced. |
| SG21| **`.dssir` side-table round-trip**                | Closes [[D-GPU-MIR-SIDECAR-SIDETABLE-ROUNDTRIP]] — the `MirShaderAttribute` side-table must survive emit→parse, or the sidecar arrives undecorated (§2.13). **Load-bearing prerequisite for all of Track C.** |
| SG22| **`--emit-mir` driver wiring + sidecar versioning**| Wires the dormant `.dssir` emitter into the driver and gives it a version field. Closes [[D-GPU-MIR-SIDECAR-FORMAT-STABILITY]]. |
| SG23| **Mandatory CPU-sibling verifier rule**           | `SH_GpuEntryPointWithoutHostSibling` per §2.14, scoped to Track-C-dispatchable compute entry points. Strict red-on-disable. |

### 5.2 Track A — GPU identification and target derivation (`GI*`)

| PR  | Title                                            | Scope |
|-----|--------------------------------------------------|-------|
| GI1 | **Harness skeleton + toolchain discovery**        | `scripts/gpu-identifier/gpu-identifier.{ps1,sh,py}` per the pragma-census pattern (§3.7). Detects which vendor toolchains exist; **reports every skip with its reason**. Closes the reporting half of [[D-GPU-IDENTIFIER-TOOLCHAIN-AVAILABILITY]]. |
| GI2 | **Probe corpus — scalar + memory ladders**        | §3.2 families 1–2, one fact per probe, each with its `$comment` provenance. |
| GI3 | **Probe corpus — control flow, barriers, launch ABI, register pressure** | §3.2 families 3–7. The launch-ABI and register-pressure ladders are what SG16/SG17 consume. |
| GI4 | **Differential derivation engine**                | Diffs probe pairs that vary in one dimension (§3.2) and emits a **candidate** `<gpu>.target.json` + a provenance record (probe, command line, toolchain version, raw excerpt). Never auto-commits (§3.4). |
| GI5 | **`targets/gpus/` subdirectory resolution**       | The declared-subdir change to `findShippedConfig` (§3.5), preserving the traversal defense and adding **no** identity branch. Closes [[D-GPU-TARGET-CONFIG-SUBDIR-RESOLUTION]]. |
| GI6 | **Runtime GPU identity**                          | Driver-API enumeration + PCI corroboration (§3.6), equality-matched against declared identity. Shared by Track C step 2. |
| GI7 | **First committed descriptor per vendor**         | One real GPU per in-scope vendor, every row carrying MEASURED provenance. This is Track A's exit criterion. |

### 5.3 Track C — deployment, first-run compilation, dispatch (`GD*`)

| PR  | Title                                            | Scope |
|-----|--------------------------------------------------|-------|
| GD1 | **Launcher shim**                                 | The per-format shim (§4.3), emitted by DSS itself. No compiler linked; spawns. |
| GD2 | **Artifact cache + manifest**                     | Content-addressed key (§4.4), exe-dir-then-cache-dir resolution, atomic write, concurrent-launch safety. |
| GD3 | **First-run state machine**                       | §4.2 steps 1–6 including every fallback edge, plus the `eager`/`deferred` policy knob. |
| GD4 | **Late-bound FFI substrate**                      | ★ The `dlopen`/`LoadLibrary` + per-symbol-resolve capability DSS does not have today (§4.7). Designed **generally** — every optional dependency has this shape, not just GPUs. Closes [[D-GPU-DISPATCH-FFI-LATE-BOUND]]. |
| GD5 | **Dispatch runtime — minimal launch path**        | Enumerate, load module, set args, launch, synchronize, buffer in/out. Streams / async / multi-GPU / unified memory deferred with named anchors. |
| GD6 | **Runtime compiler child + config packaging**     | The spawnable compiler driver over the existing SHARED library, plus shipping the `src/dss-config/` subset it resolves at runtime (§4.6). |
| GD7 | **Failure posture + poison markers**              | §4.8 in full: every fallback recorded and queryable, no silent slow path. |
| GD8 | **Platform-policy conformance**                   | §4.9 — the AMFI measurement (**measured, not inferred**), plus the documented permanent-CPU platforms. Closes [[D-GPU-ARTIFACT-RUNTIME-CODESIGN]]. |
| GD9 | **End-to-end differential acceptance**            | One built program, N machines: GPU-accelerated where recognized, CPU-correct where not, and **GPU output compared against the CPU sibling** on every machine that runs both (§1.1 property 2). |

### 5.4 Deferred anchors introduced by rev 4

Per the registry's own rule, anchors whose feature area maps cleanly onto a plan live in that plan; these are that set. Column shape matches `_deferred-anchor-registry.md` (`Anchor | Trigger | Closing work | Cross-refs`). **All rows are OPEN** — none carries `✅`.

| Anchor | Trigger | Closing work | Cross-refs |
|---|---|---|---|
| `D-GPU-MIR-SIDECAR-SIDETABLE-ROUNDTRIP` | **OPEN — named 2026-08-12 (rev 4).** `.dssir` cannot round-trip MIR side-tables (`mir_text.hpp`: *"No 5 side-tables. Side-table support deferred…"*), but §2.5 routes all shader decoration through the `MirShaderAttribute` side-table. A sidecar written today arrives at the GPU walker **undecorated** — no stage, no workgroup size, no bindings. | Extend `emitMir`/`parseMir` to carry side-tables under the existing byte-identical round-trip contract. **Priority: HIGH — blocks all of Track C.** | SG21; §2.13; plan 12 §3.1 D-ML2-2.1 |
| `D-GPU-MIR-SIDECAR-FORMAT-STABILITY` | **OPEN — named 2026-08-12 (rev 4).** Shipping `.dssir` next to a product promotes a test-only format to a versioned compatibility surface; nothing wires it into the driver today (`emitMir` is called only from tests). | Version field + verify-or-refuse on load + an `--emit-mir` driver flag. Text form first (round-trip already proven); binary form only once size is MEASURED to matter. | SG22; §2.13 |
| `D-GPU-DISPATCH-FFI-LATE-BOUND` | **OPEN — named 2026-08-12 (rev 4).** ★ Declaring a GPU driver API as an ordinary shipped-lib descriptor would make every GPU-capable binary **fail to LOAD** on a machine without that vendor's driver (`D-FFI-DESCRIPTOR-EAGER-IMPORT`: pe `0xC0000139` / elf exit 127) — the inverse of the CPU-fallback guarantee. | A late-binding FFI surface (`dlopen`/`LoadLibrary` + per-symbol resolve) where absence is an ordinary runtime outcome. Design generally: every optional dependency has this shape. **Priority: HIGH.** | GD4; §4.7; `D-FFI-DESCRIPTOR-EAGER-IMPORT` |
| `D-GPU-TARGET-CONFIG-SUBDIR-RESOLUTION` | **OPEN — named 2026-08-12 (rev 4).** `targets/gpus/` is unreachable: `findShippedConfig` rejects any name containing `/` or `\` (traversal defense, `config_path_walk.cpp:18-26`) and `loadShipped` composes a flat path. | Let a target family declare its subdir from a **closed declared set**; keep the traversal defense; add no identity branch to the resolver. | GI5; §3.5 |
| `D-GPU-ISA-ABIMODEL-DIVERGENCE` | **OPEN — named 2026-08-12 (rev 4).** §2.11's hypothesis — that `abiModel: register-machine` suffices for a GPU ISA — is UNTESTED. Memory spaces, predication/divergence, the kernarg launch ABI and register-budget-as-a-bar are the four known gaps. | Track A probes answer it empirically **before** any engine change is proposed; whatever remains becomes declared vocabulary, never an engine branch. | SG14–SG17; §2.11 |
| `D-GPU-CPU-SIBLING-MANDATORY` | **OPEN — named 2026-08-12 (rev 4).** §2.14 requires a host sibling for Track-C-dispatchable compute entry points — the fallback AND the differential oracle. Scope must exclude graphics stages, where the rule would be tidy but catch nothing. | `SH_GpuEntryPointWithoutHostSibling` + the exact scope test, strict red-on-disable. | SG23; §2.14; §2.4 |
| `D-GPU-ARTIFACT-RUNTIME-CODESIGN` | **OPEN — named 2026-08-12 (rev 4).** A runtime-produced artifact meets AMFI / antivirus / `noexec` / hardened-runtime policy. DSS's ad-hoc `CS_SuperBlob` passes AMFI for mach-o (TF-C121) — ⚠ whether a `.metallib` traverses that path at all is **UNMEASURED**. | Measure the Metal load path on real hardware; document the permanently-CPU platforms as a supported outcome. | GD8; §4.9 |
| `D-GPU-RUNTIME-COMPILER-MINIMAL-LIB` | **OPEN — named 2026-08-12 (rev 4).** Reusing `dss-code-prime-lib` redistributes the entire frontend (tokenizer/parser/semantic/HIR) to run a MIR→ISA path that uses none of it. Accepted deliberately to prove the mechanism first. | Carve a minimal SHARED target from a **measured** object-library dependency set, not a guessed one. | GD6; §4.6 |
| `D-GPU-IDENTIFIER-TOOLCHAIN-AVAILABILITY` | **OPEN — named 2026-08-12 (rev 4).** No machine carries all four vendor toolchains; a silently-skipped vendor leg reads as coverage. | Per-vendor independent skippability with the skip and its reason REPORTED, mirroring the sqlite harness's `skipped-by-runOn` discipline. | GI1; §3.7 |
| `D-GPU-ISA-DRIVER-VERSION-DRIFT` | **OPEN — named 2026-08-12 (rev 4).** The same chip under a newer driver can change kernel-metadata expectations without changing its ISA — so driver version is not identity, but a cached artifact built against the old one may be wrong. | Driver version participates in the cache key (§4.4) so drift is a MISS, never a mismatch; re-derivation policy when a vendor ships a breaking driver. | GI6; §3.6; §4.4 |

---

## 6. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | HIR→SPIR-V direct path (skip MIR for leaf shaders)? | **No — resolved by rev 3 framing.** Always via MIR. The MIR optimizer runs upstream of ALL structured-bytecode targets (WASM + SPIR-V) — that's the load-bearing claim. Skipping MIR for shaders would drop the optimizer for shaders specifically, which is exactly the failure mode plan 10's "syntactic source-to-source has its own opt-out" carve-out covers. Shader compile time has not been a problem with MIR in the loop. |
| 2 | Target SPIR-V version? | **1.6** — covers Vulkan 1.3 baseline. |
| 3 | Vulkan / Metal / D3D12 / WebGPU coverage? | Native SPIR-V; Metal/D3D12/WebGPU via post-v1 `10-source-translation-plan` transpile. |
| 4 | Same-source CPU+GPU function dispatch? | **Yes** — `[[shader.usable]] [[host.usable]]` triggers dual lowering. |
| 5 | Shader debugging (RenderDoc / Nsight / Xcode GPU debugger)? | v1.x: `OpLine` debug instructions for line numbers (via `15-debug-info-plan` §2.7 — open question §8). Full local-variable debug post-v1. |
| 6 | Reflection sidecar format? | `.spv.json` per §2.8. Stable schema; versioned. |
| 7 | Ray-tracing extensions (`SPV_KHR_ray_tracing`)? | Reserved post-v1.x. |
| 8 | Subgroup ops (warp-level intrinsics)? | Reserved — added when a real ray-tracing or compute-heavy workload demands them. |
| 9 | SPIR-V diagnostic namespace? | `SH_*` for HIR-side shader violations; SPIR-V emission errors share `SH_*` (e.g. `SH_SpirVCapabilityMissing`). |
| 10 | ISA-backend diagnostic namespace? | **Open.** `SH_*` covers shader-shape violations and SPIR-V emission. Real-ISA backends emit through the *existing* register-machine chain (§2.11), so their failures may belong to `A_*` (assembler) and `L_*` (LIR verifier) rather than to a new family. Decide when SG17 measures how much of that chain is actually reused — allocating a nibble before that is guessing. Allocation goes through the central registry in [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.2 (§2.9's `SV_*`→`V_*` collision is the cautionary precedent). |
| 11 | Does `abiModel: register-machine` suffice for a GPU ISA? | **Open, and deliberately so — this is [[D-GPU-ISA-ABIMODEL-DIVERGENCE]].** The §2.11 hypothesis is that it does, with four named gaps arriving as declared vocabulary. Track A answers it by measurement before any engine change is proposed. Default if the hypothesis fails: a new `abiModel` value, never an identity branch. |
| 12 | Sidecar scope — GPU functions only, or the whole program? | **GPU-dispatched functions only** (§4.1). Shipping whole-program MIR would bloat the artifact and expose far more of the program than the GPU path needs. Revisit only if cross-function optimization at first-run compile time is shown to matter. |
| 13 | First-launch policy — `eager` or `deferred`? | **`eager` default, `deferred` available** (§4.2). Eager means first launch is slower but GPU-accelerated from its first frame; deferred runs CPU immediately and accelerates the *next* launch, which is closer to the brief's original sequencing and is the better default for interactive apps with a visible startup. A per-application knob, not an architecture fork. |
| 14 | Graphics stages and Track C | **Out of scope for dispatch.** Track C dispatches **compute**. Vertex/fragment stages are consumed by a renderer ([`27-gui-plan`](./27-gui-plan.md) §9 stakes out Vulkan/Metal as the Layer-2 renderer), which is a different consumer with a different lifecycle. This is also why §2.14's sibling rule is scoped to compute. |
| 15 | Multi-GPU machines | **Deferred with an anchor, not silently.** v1.x targets the driver-reported default device. Selection policy, per-device artifacts (the cache key already distinguishes them) and simultaneous multi-device dispatch are named deferrals under GD5. |

**Resolved at rev 4 (operator decisions, 2026-08-12 — recorded so they are not re-litigated):** artifact tier is **real GPU ISA**, not a vendor virtual ISA and not SPIR-V-only (§2.11); vendor scope is **all four** — NVIDIA, Apple, AMD, Intel (§3.3); the runtime compiler **reuses `dss-code-prime-lib`** rather than a new minimal target (§4.6, with [[D-GPU-RUNTIME-COMPILER-MINIMAL-LIB]] left open); restart is handled by a **launcher shim**, which removes the need for a restart entirely (§4.2, §4.3); the compile runs in a **spawned child process** (§4.6); the artifact lands **next to the executable with a per-user cache fallback** (§4.4); and Track C **owns the dispatch runtime**, not just detection (§4.7).

---

## 7. Acceptance criteria

- [ ] Custom-language HIR shader subset lowers to spirv-val-clean SPIR-V.
- [ ] "Hello triangle" vertex+fragment shader produced by our toolchain renders identically to a `dxc`-compiled equivalent in a CI Vulkan validation-layer harness.
- [ ] Compute shader compiles + dispatches + produces correct buffer output on a Vulkan-compute-capable CI runner.
- [ ] Shader verifier rejects all SPIR-V constraint violations with actionable `SH_*` diagnostics.
- [ ] Same-source CPU + GPU function lowering: a function tagged both `[[shader.usable]]` and `[[host.usable]]` produces correct SPIR-V *and* correct native code referencing the same source span.
- [ ] Hermetic acceptance: no `dxc` / `glslc` / `shaderc` / `spirv-tools` / `spirv-opt` / `spirv-cross` invocation in the production pipeline (oracles only in CI).
- [ ] **`SpirvVerifier` acceptance** (per §2.9): every emitted SPIR-V module passes `SpirvVerifier`'s 7 rule families BEFORE the binary encoder writes bytes. Cross-check with `spirv-val` oracle in CI — any `SpirvVerifier`-clean module that fails `spirv-val` is a verifier-rule gap (file + fold), NOT a ship-it outcome.
- [ ] **Minifier acceptance** (per §2.10): `minified` profile reduces a custom-language compute-shader corpus `.spv` by ≥ 25% vs `debug`, with byte-identical execution under the spirv-val + reference-driver oracles. Hash-pinned in CI per SG13.

**Track A (rev 4):**

- [ ] A committed `<gpu>.target.json` for **at least one real GPU per in-scope vendor**, with every derived row carrying MEASURED provenance (probe, command line, toolchain version, listing excerpt) — the standard `x86_64.target.json`'s predefine rows already meet.
- [ ] Every derived fact comes from a **differential** comparison of probes varying in one dimension (§3.2), never from a single listing read as if it were a specification.
- [ ] The identifier runs on a machine with **only one** vendor toolchain installed and reports the other three legs as skipped, **each with its reason** — a silent skip fails this criterion.
- [ ] **Hermetic falsifier holds:** no DSS build and no first-run path invokes `nvcc`/`metal`/`hipcc`/`ocloc`, and CI proves it by building with none of them installed.

**Track B ISA half (rev 4):**

- [ ] A compute kernel compiled by DSS to a real GPU ISA **executes on that GPU and produces output identical to its CPU sibling** — this is the acceptance criterion, and it needs no vendor compiler in the loop.
- [ ] An address-space-losing pointer fails loud (SG14), never silently becoming a global-space access.
- [ ] `.dssir` round-trips the `MirShaderAttribute` side-table byte-identically (SG21) — without this the sidecar is undecorated and Track C cannot work at all.

**Track C (rev 4):**

- [ ] **One built program, N machines.** GPU-accelerated where the device is recognized; **correct on the CPU** where it is not, where there is no GPU, and where the driver is absent entirely.
- [ ] A machine with **no vendor driver installed loads and runs the program** — the late-bound dispatch requirement (§4.7). A load failure here is the specific defect [[D-GPU-DISPATCH-FFI-LATE-BOUND]] exists to prevent.
- [ ] Second launch uses the cached artifact; a driver upgrade, an app rebuild or a compiler upgrade produces a cache **miss and recompile**, never a mismatch.
- [ ] A killed first run leaves no artifact a later run will trust (atomic write, §4.4).
- [ ] Every fallback is **recorded and queryable**; a silent slow path fails this criterion as surely as a wrong answer would.
- [ ] A reproducible compile failure is **not retried on every launch** (poison marker), and clears automatically when any keyed input changes.

---

## 8. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| SPIR-V capability negotiation drift | Medium | Medium | Validate capabilities against Vulkan baseline at module emission; reject if a feature requires a capability we don't declare. |
| Shader debugging absent in v1.x | High | Low | Reserved; reflection sidecar + `OpLine` give "good enough" for first release. |
| Same-source dual-lowering complexity (libc gating per lowering mode) | High | High | Verifier mode-aware: when lowering as shader, every callee checked `ShaderUsable`; when lowering as host, no restrictions. Tests cross every realistic shape. |
| First-class lattice members for GPU types pressure semantic phase | Medium | Medium | Lattice extensions registered per `08.5-substrate-prep-plan §2.2` — language schemas declare which lattice members they expose. Non-shader languages don't see GPU types. |
| **Real-ISA scope is the largest undertaking in the plan tree** | — | High | Rev 3's cost estimate was right even though its conclusion was wrong. Mitigation is structural, not optimistic: the CPU sibling makes every GPU result *checkable* (§1.1), the fallback makes an unfinished vendor *harmless*, and vendors are sequenced by measurability (AMD first, Apple last) so the cheapest leg discovers what §2.11 got wrong. |
| **Apple AGX may not be reachable in v1.x** | High | Medium | No vendor ISA disassembler exists (§3.3). Named, not absorbed: SG20 may land AIR-only with the ISA step deferred behind an anchor. The Apple leg still ships — via the CPU sibling — rather than blocking the release. |
| **Vendor driver update breaks a derived descriptor** | Medium | High | Driver version is in the cache key (§4.4) so stale artifacts MISS rather than mismatch; [[D-GPU-ISA-DRIVER-VERSION-DRIFT]] owns the re-derivation policy. ⚠ The residual risk is a driver that changes ISA *semantics* without changing anything we key on — which the CPU-sibling differential is the only real defence against. |
| **A silent CPU fallback hides a broken GPU path** | Medium | High | §4.8 makes every fallback recorded and queryable. Stated as its own risk because it is the failure mode that *looks* like success — the same class as a silent miscompile, and the reason "fail-loud ≠ fatal" is spelled out rather than assumed. |
| **Runtime code generation blocked by platform policy** | Medium | Medium | §4.9. Some platforms (iOS, consoles, locked-down enterprise) forbid it outright — there the CPU sibling is permanent, which is a **supported outcome**, not a failure. The AMFI question for `.metallib` is explicitly UNMEASURED and must not be inferred from the mach-o result. |
| **Eager-import breaks GPU-less machines** | — | Critical | Would make every GPU-capable binary fail to LOAD without the vendor driver (§4.7). Fully understood and designed against ([[D-GPU-DISPATCH-FFI-LATE-BOUND]], GD4) — listed here because the tree's default FFI mechanism produces it *by default*, so it is a live hazard until GD4 lands, not a hypothetical. |

---

## 9. Sequencing

Track A gates the ISA half of Track B, which gates the useful half of Track C. Track B's SPIR-V half and Track C's plumbing are independent of Track A and can proceed in parallel.

```
                    ┌─────────────────────── Track A (GI*) — measure first ────────────────────────┐
                    │  GI1 ─► GI2 ─► GI3 ─► GI4 ─► GI7 (a descriptor per vendor)                    │
                    │   └─► GI5 (targets/gpus resolution)      GI6 (runtime identity) ──────┐      │
                    └───────────────────────────────┬──────────────────────────────────────┬┴──────┘
                                                    │ answers D-GPU-ISA-ABIMODEL-DIVERGENCE │
09-hir + 12-mir-lir ─► SG1 ─► SG2 ─► SG3 ─► … ─► SG13   (Track B, SPIR-V — rev 3, unblocked) │
                    │                               │                                        │
                    │                               ▼                                        │
                    │      SG14 (address spaces) ─► SG15 (divergence) ─► SG16 (launch ABI)    │
                    │                               │                                        │
                    │                               ▼                                        │
                    │      SG17 (AMD) ─► SG18 (NVIDIA) ─► SG19 (Intel) ─► SG20 (Apple ⚠)     │
                    │                                                                        │
                    └─► SG21 (.dssir side-tables) ─► SG22 (--emit-mir) ─► SG23 (sibling rule) │
                                        │                                                    │
                                        ▼                                                    ▼
                    ┌──────────────────────── Track C (GD*) ──────────────────────────────────┐
                    │  GD4 (late-bound FFI) ─► GD5 (dispatch) ─┐                              │
                    │  GD1 (shim) ─► GD2 (cache) ─► GD3 (state machine) ─► GD6 ─► GD7 ─► GD8  │
                    │                                          └─────────► GD9 (end-to-end)   │
                    └─────────────────────────────────────────────────────────────────────────┘
                                                    │
                                                    ▼
                                      10-source-translation
                                      (SPIR-V → DXIL/MSL/WGSL)
                                      (post-v1.x)
```

**Critical path, named:** `GI1→GI4→GI7` (you cannot encode an ISA you have not read) and `SG21` (you cannot deploy a sidecar that drops its decorations). Everything else has slack.
