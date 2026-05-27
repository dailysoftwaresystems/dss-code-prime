# MIR + LIR — Sub-Plan

> Mid-level IR (SSA over CFG, with structured-CF markers preserved) plus low-level IR (per-target ISA, virtual + physical registers, machine-shape instructions). Together they form the optimization + native-codegen middle tier between [HIR](./09-hir-plan%20-%20tbd.md) and the [in-tree assembler](./13-assembler-plan%20-%20tbd.md) / [linker](./14-linker-plan%20-%20tbd.md).
>
> **Why both in one plan.** Their interface is tight: MIR's lowered shape directly determines LIR's instruction selection patterns. Splitting buys nothing and risks the seam.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1 production-critical. |
| Predecessors  | ⏳ [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) (arena reuse). 🟡 [`09-hir-plan`](./09-hir-plan%20-%20tbd.md) (input — HR1–HR9 ✅ 2026-05-26..27 (HR8 = config-driven CST→HIR lowering on c-subset, incl. a `HirLiteralPool` of decoded literal values MIR reads; HR9 enriched toy into a typed language + un-deferred arrays → `Array<T,N>` types MIR will lower), HR10–HR11 pending; HR7's `.dsshir` text format is the sibling-discipline reference for ML4's `.dssir` / ML8's `.dsslir` round-trip). |
| Successors    | ⏳ [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) consumes LIR. ⏳ Optimizer phase (master §10) operates on MIR. ⏳ [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) consumes assembled bytes + relocations. ⏳ [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) reuses MIR for shader optimization. ⏳ [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md) consumes MIR with structured-CF markers. |
| Scope         | **Bounded.** ML1–ML4 MIR. ML5–ML8 LIR. |

---

## 1. Motivation

HIR is too expression-shaped for dataflow analysis. Native codegen is too target-specific to optimize at. The middle layer is two-tiered:

- **MIR** is the optimizer's playground. SSA over CFG is the industry-standard shape — constant folding, DCE, copy prop, dominator analysis all fit cleanly. **Key DSS twist:** structured-CF markers (loop headers/latches/exits; if-then/else/join) are preserved as block annotations so MIR can downlower to WASM (which demands structured CF) without a Relooper-style recovery pass.
- **LIR** is the target-specific shape. Same dataflow concept, but virtual registers map to physical register files (GPR/FPR/VR per arch), instructions match the ISA tier, calling conventions are lowered to explicit moves, and stack frames are materialized. LIR is what the [assembler](./13-assembler-plan%20-%20tbd.md) consumes.

---

## 2. Design

### 2.1 Files

```
src/mir/
├── mir.hpp                  # Mir type (frozen)
├── mir_builder.hpp / .cpp   # HIR→MIR lowering
├── mir_block.hpp            # BasicBlock POD
├── mir_inst.hpp             # MirInstruction POD + opcodes
├── mir_value.hpp            # SSA value + MirValueId
├── mir_cfg.hpp              # Control-flow graph adjacency
├── mir_dom.hpp              # Dominator-tree analysis (separate from CFG)
├── mir_struct_cf.hpp        # Structured-CF marker discipline
├── mir_verifier.hpp / .cpp
└── mir_text.hpp / .cpp      # .dssir round-trippable text

src/lir/
├── lir.hpp                  # Lir type (frozen) — per-target
├── lir_builder.hpp / .cpp   # MIR→LIR lowering driver
├── lir_inst.hpp             # LirInstruction POD
├── lir_reg.hpp              # Virtual + physical register types
├── lir_regalloc.hpp / .cpp  # Linear-scan
├── lir_callconv.hpp / .cpp  # Calling-convention lowering
├── lir_frame.hpp / .cpp     # Stack frame materialization
├── lir_verifier.hpp / .cpp
├── lir_text.hpp / .cpp      # .dsslir round-trippable text
└── targets/
    ├── x86_64.hpp / .cpp    # x86_64 instruction tier + register file
    └── arm64.hpp / .cpp     # ARM64 instruction tier + register file
```

### 2.2 MIR shape

**Values** are SSA: `%v.NNN` — each defined exactly once, by exactly one instruction or block-parameter.

**Basic blocks** are POD with `id`, `instStart`/`instCount` (into the MIR's instruction arena), an explicit terminator instruction, and a structured-CF marker tag.

**Instructions**:

| Family | Opcodes |
|---|---|
| Arithmetic | `add`, `sub`, `mul`, `sdiv`, `udiv`, `smod`, `umod`, `neg`, `fadd`, `fsub`, `fmul`, `fdiv`, `fneg` |
| Bitwise | `and`, `or`, `xor`, `shl`, `lshr`, `ashr`, `not` |
| Comparison | `icmp.{eq,ne,slt,sle,sgt,sge,ult,ule,ugt,uge}`, `fcmp.{...}` |
| Memory | `load`, `store`, `gep` (getelementptr), `alloca` |
| Control | `br`, `cond_br`, `switch`, `return`, `unreachable`, `phi` |
| Calls | `call`, `intrinsic_call` |
| Casts | `trunc`, `sext`, `zext`, `fptrunc`, `fpext`, `bitcast`, `inttoptr`, `ptrtoint`, `fptosi`, `fptoui`, `sitofp`, `uitofp` |
| SIMD | `vadd`, `vsub`, `vmul`, `vshuffle`, `vextract`, `vinsert` (reserved post-v1) |

**MIR types** = the canonical-lowered core lattice: `i1`/`i8`/`i16`/`i32`/`i64`/`i128`/`f16`/`f32`/`f64`/`f128`/`ptr`/`vector<T,N>`/`struct{}`/`array<T,N>`. No language-extension types past this point — extensions resolved to concrete machine shapes during HIR→MIR.

### 2.3 Structured-CF marker discipline

Every MIR `BasicBlock` carries a `StructCfMarker` tag (one of: `LoopHeader`, `LoopLatch`, `LoopExit`, `IfThen`, `IfElse`, `IfJoin`, `SwitchHead`, `SwitchCase`, `SwitchJoin`, `Linear`, `EntryBlock`, `ExitBlock`).

**Invariant**: HIR→MIR lowering stamps these markers; every MIR-level optimization either preserves them or invalidates them via `MirCfg::invalidateStructCf()` (which downstream lowering then runs the Relooper algorithm to recover; only fallback path).

WASM lowering (`18-wasm-plan`) consumes these markers directly to produce `block`/`loop`/`if`/`br_if` without ever running Relooper.

### 2.4 MIR text format `.dssir`

```
function @main () -> i64 {
  bb.entry [Linear]:
    %0 = const i64 1
    %1 = const i64 2
    %2 = add i64 %0, %1
    return i64 %2
}
```

Round-trippable, verifier-validated on load. Used as golden-test fixtures + as the optimizer's interchange format.

### 2.5 MIR verifier

- Every SSA value defined exactly once.
- Every use dominated by its definition.
- Every block has exactly one terminator.
- Every `phi` has exactly one operand per predecessor.
- Structured-CF markers form a consistent tree (every `IfThen` has a matching `IfElse`/`IfJoin`; every `LoopHeader` has a matching `LoopLatch`/`LoopExit`).
- Type-consistency on every instruction.
- `I_*` diagnostic codes.

### 2.6 LIR shape

Per-target. The `Lir` type is parameterized over the target:

```cpp
template <class TargetTraits>
class Lir { ... };
```

`TargetTraits` provides:
- `Opcode` enum (target-specific instruction set)
- `RegClass` enum (GPR / FPR / VR / FLAGS)
- Physical register file (per arch)
- Calling-convention lowering rules
- Stack frame layout rules

**LIR instructions** are pre-encoding-ready — every operand is either a physical register, a virtual register awaiting allocation, an immediate, or a memory operand (base + offset + index + scale). After regalloc, all virtual regs become physical.

### 2.7 MIR→LIR lowering passes

1. **Instruction selection** — tile-matching MIR patterns to LIR opcodes. Greedy maximal-munch for v1; cost-driven dynamic programming reserved post-v1.
2. **Register allocation** — linear-scan (per G-512 / `07-prod-readiness-plan`). Spill slots materialized as `alloca`-shaped LIR stack-slot ops; reload at use sites.
3. **Calling-convention lowering** — every `call` produces explicit moves into ABI argument registers + stack slots per platform (SysV AMD64 / Microsoft x64 / AAPCS64 / Microsoft ARM64); every return produces moves into ABI return registers.
4. **Stack frame materialization** — prologue (push frame pointer, allocate stack), epilogue (restore + ret), spill/reload slots laid out, alignment enforced.

### 2.8 LIR text format `.dsslir`

Per-target:
```
;; target = x86_64-linux-sysv
function @main {
.entry:
    mov  rax, 1
    add  rax, 2
    ret
}
```

Round-trippable. Disassembler round-trip test pins encoding (see [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md)).

### 2.9 LIR verifier

- No virtual registers after regalloc.
- Every spill paired with a reload (or proven dead).
- Stack frame size matches declared local-storage footprint.
- Calling-convention shape matches platform.
- `L_*` diagnostic codes.

---

## 3. PR breakdown

| PR  | Title                                            | Scope |
|-----|--------------------------------------------------|-------|
| ML1 | MIR types, value/inst/block PODs, IDs            | Skeleton. Strong IDs (`MirValueId`, `MirBlockId`, `MirInstId`, `MirFuncId`). Arena via `08.5` SP1. **Ships `MirAttribute<T>` typedef** as `substrate::ArenaAttribute<MirArena, T>` mirroring the `NodeAttribute<T>` / `HirAttribute<T>` pattern. |
| ML2 | HIR→MIR lowering                                 | Walk HIR structured CF; emit SSA values + blocks; stamp `StructCfMarker` tags. Phi-node generation at join points. |
| ML3 | MIR verifier + dominator-tree analysis           | All structural invariants + dominator-tree as a separate analysis result; `I_*` codes. |
| ML4 | MIR text format + roundtrip                      | `.dssir` parser + emitter; verifier validates on load. |
| ML5 | LIR template + instruction selection (x86_64 + ARM64) | Tile-matching MIR patterns → LIR opcodes per target. Initial coverage: arithmetic / memory / branch / call / return — enough for c-subset corpus. **Ships `LirAttribute<T>` typedef** per target as `substrate::ArenaAttribute<LirArena<TargetTraits>, T>`. |
| ML6 | Linear-scan register allocation                  | Per `07-prod-readiness G-512`. Live-range computation + virtual-reg coloring + spill-slot insertion. |
| ML7 | Calling-convention + stack frame materialization | SysV AMD64, Microsoft x64, AAPCS64, Microsoft ARM64. Prologue/epilogue. |
| ML8 | LIR text format + verifier + round-trip          | `.dsslir` per-target; verifier validates; disasm round-trip pins. |

Substrate tier (5-agent review) for ML1, ML3, ML5, ML6 — touch substrate / cross-cutting algorithm contracts.

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Dominator tree: stored on MIR or recomputed on demand? | **Recomputed on demand** via a separate analysis pass (mirrors LLVM's `DominatorTreeAnalysis`); caching is the optimizer's concern. |
| 2 | Structured-CF markers: mandatory or recommended? | **Mandatory.** Every MIR block has a marker; verifier rejects untagged blocks. Optimizer passes that destroy structure must explicitly call `invalidateStructCf()` — which forces downstream consumers (WASM) onto a Relooper recovery path. |
| 3 | Function attributes (inline hint, align, freestanding, nounwind) — MIR or LIR? | **MIR** at function level; LIR inherits. Stored as `MirAttribute<FunctionAttrs>` side-table. |
| 4 | LIR per-target or one LIR with target attributes? | **Per-target** (templated). Different ISAs have different register classes, instruction shapes; one LIR makes the verifier nuanced for no gain. |
| 5 | Pre-regalloc peephole optimizations on LIR? | **Yes** (post-instruction-selection, pre-regalloc). Captures `mov rax, 0` → `xor rax, rax` and similar without polluting MIR. |
| 6 | Spill-slot reuse / coalescing? | **Yes, basic** — same-class-same-lifetime virtual regs share a slot. More sophisticated coalescing reserved post-v1. |
| 7 | SIMD / vector ops in v1 MIR? | **Reserved post-v1** — language-side support not yet shipped. MIR opcodes declared; lowering deferred. |
| 8 | Exception handling at MIR level? | **Reserved** — no v1 shipped language uses it. `invoke`/`landingpad` slot reserved in opcodes. |

---

## 5. Acceptance criteria

- [ ] c-subset corpus programs lower HIR → MIR cleanly; MIR verifier passes.
- [ ] MIR round-trips via `.dssir` text format (emit + parse + emit → byte-identical).
- [ ] Dominator-tree analysis correct on every v1 corpus function (compare against a known-good reference oracle: LLVM's `DominanceFrontier`).
- [ ] Structured-CF markers consistent post-lowering on every v1 corpus function.
- [ ] x86_64 and ARM64 LIR targets cover the c-subset corpus' MIR opcode set.
- [ ] Linear-scan regalloc handles every corpus function without spill-slot exhaustion.
- [ ] Calling convention conformance pinned by ABI golden tests (call `int add(int,int)` into a hand-written external C function on each platform; result correct).
- [ ] LIR round-trips via `.dsslir` per-target.

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Structured-CF marker discipline lapses during optimizer passes | Medium | High | Verifier runs after every optimizer pass in debug builds; `I_StructCfBroken` aborts the build with the offending pass named. |
| Linear-scan regalloc spills heavily on tight inner loops | Medium | Medium | Profile-driven; if a corpus function spills > N times, fall back to a register-rich path (defer to graph-coloring post-v1 per `07-prod-readiness G-512`). |
| Per-target LIR template causes code bloat | Low | Low | Targets share most logic via `TargetTraits`; only the ISA-specific instruction table is distinct. |
| Dominator-tree recomputation cost dominates optimizer time | Low | Medium | Cache via `MirAttribute<DomTreeSnapshot>`; invalidate on CFG mutation. |
| Calling-convention bugs surface only at native-link time | High | High | Per-platform ABI integration tests in CI from day one of ML7. |

---

## 7. Sequencing

```
09-hir ─► ML1 ─► ML2 ─► ML3 ─► ML4
                         │
                         ▼
                   optimizer (master §10)
                         │
                         ▼
                       ML5 ─► ML6 ─► ML7 ─► ML8
                                              │
                                              ▼
                                       13-assembler ─► 14-linker
                                              │
                                              ▼
                                       17-shader (parallel: SPIR-V from MIR + structured-CF)
                                              │
                                              ▼
                                       18-wasm (parallel: WASM from MIR + structured-CF)
```

ML1–ML4 (MIR) blocks the optimizer phase + ML5. ML5–ML8 (LIR) blocks the assembler. Shader and WASM consume MIR's structured-CF discipline; they don't gate native LIR.
