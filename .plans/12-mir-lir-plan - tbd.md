# MIR + LIR — Sub-Plan

> Mid-level IR (SSA over CFG, with structured-CF markers preserved) plus low-level IR (per-target ISA, virtual + physical registers, machine-shape instructions). Together they form the optimization + native-codegen middle tier between [HIR](./09-hir-plan%20-%20tbd.md) and the [in-tree assembler](./13-assembler-plan%20-%20tbd.md) / [linker](./14-linker-plan%20-%20tbd.md).
>
> **Why both in one plan.** Their interface is tight: MIR's lowered shape directly determines LIR's instruction selection patterns. Splitting buys nothing and risks the seam.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟡 **in progress.** v1 production-critical. **ML1 ✅ done 2026-05-27** (`feature/mir-lir`): MIR skeleton + minimal builder — three arenas (inst/block/func) under one `MirModuleId`, **fused value model** (`MirValueId = MirInstId`; a non-void inst IS its SSA value), phi nodes, **closed `MirOpcode` enum + `opcodeInfo()` descriptor table** (single source of truth, `-Wswitch-enum` latched; carries operand arity **and CFG successor arity** so terminator builders + verifier consume the same table; `MirResultRule{None,Value,Optional}` for void-callee calls; value-origin opcodes `Arg`/`Const`/`GlobalAddr` so a `Call` callee is uniformly operand[0]), `StructCfMarker` block field (ML2 stamps; never an instruction), operand/phi/succ pools (control-flow kept off operands → type-safe + O(1) CFG), `instBlock` reverse-lookup (the MIR `parentOf_`), `MirLiteralPool` (mirrors `HirLiteralPool`), `MirAttribute`/`MirBlockAttribute`/`MirFuncAttribute`. **Build-once-freeze** (optimizer rebuilds functionally; CU-scoped TypeIds survive). **create-then-fill builder** (`createBlock`+`beginBlock`) — required for forward branches; phi incomings backpatched + flushed at `finish()`; **finish-time freeze sweep** validates every pooled id references a real inst/block. 6-perspective substrate review + fix-everything pass; 87/87 ctest. ML2–ML8 ⏳. |
| Predecessors  | ⏳ [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) (arena reuse). ✅ [`09-hir-plan`](./09-hir-plan%20-%20tbd.md) (input — HR1–HR11 ✅ 2026-05-26..28 (HR8 = config-driven CST→HIR lowering on c-subset, incl. a `HirLiteralPool` of decoded literal values MIR reads; HR9 enriched toy into a typed language + un-deferred arrays → `Array<T,N>` types MIR will lower; HR10 added tsql-subset lowering), HR11 ✅ done 2026-05-28 (multi-language CU lowering) — plan 09 complete; HR7's `.dsshir` text format is the sibling-discipline reference for ML4's `.dssir` / ML8's `.dsslir` round-trip). |
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

> **Single-role today, bitset if needed.** ML1 stores **one** `StructCfMarker` per block (a `uint8` field). A block can in principle play two structural roles at once (e.g. a `LoopExit` that is also an `IfJoin`). If a real consumer ever needs that, the field becomes a small bitset *without changing the POD layout* (it is already a full `uint8`). Kept single-role until a consumer forces it — no speculative multi-role modelling now.

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
- Every block has exactly one terminator (the last instruction; ML1 builder seals this, ML3 re-checks the frozen module).
- Every `phi` has exactly one operand per predecessor, and each `phi` incoming `pred` is an actual CFG predecessor of the phi's block.
- Structured-CF markers form a consistent tree (every `IfThen` has a matching `IfElse`/`IfJoin`; every `LoopHeader` has a matching `LoopLatch`/`LoopExit`). Each function has **exactly one `EntryBlock`** and it is the function's first block; `ExitBlock`s terminate in `Return`/`Unreachable`.
- Type-consistency on every instruction, including the **terminator-specific** checks ML1 deliberately deferred (see below): a `CondBr` condition is `Bool`/i1; a `Return`'s value-presence and type match the function's `FnSig` return (void ⇒ no value).
- **Opcode-shape conformance**: operand count within `[min,max]`, **CFG successor count within `[min,max]`** (`Br`=1, `CondBr`=2, `Switch`≥1 (cases+default), `Return`/`Unreachable`=0), and the result-type rule (`None`/`Value`/`Optional`) honored — all three read from the ML1 `opcodeInfo()` descriptor (successor arity added to the table in ML1 alongside operand arity, so terminator builders + verifier consume the same single source of truth and never drift). The ML1 builder asserts operand and successor counts at construction; the ML3 verifier re-checks this on any frozen module (including the direct-`Mir`-ctor path the builder doesn't own).
- **Payload validity**: a `Const`'s `payload` is a valid `MirLiteralPool` index (**ML1: enforced by construction** — `addConst` is the only path to a `Const` and always uses the pool's returned index; `addInst` rejects the `Const` opcode); an `IntrinsicCall`'s intrinsic id is registered (deferred to ML3 — MIR doesn't have its own intrinsic registry yet); an `Arg` index is within the function's parameter count (deferred to ML3 — needs `FnSig` decode via the `TypeInterner`, which the `mir` lib deliberately doesn't link).
- No `TypeKind::Extension` types survive into MIR (all language-extension types resolved to the core lattice at the HIR→MIR boundary — ML2's invariant, re-asserted here).
- `I_*` diagnostic codes.

> **ML1→ML3 deferral note (honest triage).** The ML1 skeleton (done 2026-05-27) enforces every *structural* invariant that can be checked without a downstream layer — at construction time:
> - one terminator per block, operand-count bounds, result-type rule, **CFG successor-count vs opcode** (all four via the `opcodeInfo()` single-source descriptor);
> - every pooled id references a real inst/block (finish-time freeze sweep);
> - branch targets belong to the open function;
> - `Const.payload` is a valid literal-pool index (only path is `addConst`; `addInst` rejects the `Const` opcode);
> - `instBlock_` lockstep with the instruction arena; one `MirModuleId` across all three arenas.
>
> The remaining checks above genuinely need a downstream layer and so are ML3's job (not deferred for convenience, deferred because the layer doesn't exist yet): SSA use-dominated-by-def (dominator tree), terminator type-matching (`TypeInterner` to read `FnSig`/Bool — the `mir` lib doesn't link the interner), phi-incoming-pred ⊆ CFG-predecessors (predecessor adjacency = `MirCfg`), single/first `EntryBlock` (markers populated by ML2), `Arg`-index ≤ parameter count and `IntrinsicCall`-id registered (interner / future MIR intrinsic registry), no `TypeKind::Extension` in MIR (interner). The ML3 verifier also re-runs the ML1 builder-time checks on the frozen module so a direct-`Mir`-ctor construction path is covered the same way.

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
| ML1 ✅ | MIR types, value/inst/block PODs, IDs            | **Done 2026-05-27** (`feature/mir-lir`). Strong IDs (`MirModuleId` tag + `MirInstId`/`MirBlockId`/`MirFuncId` arena ids; `MirValueId = MirInstId` alias per the fused model). Arena via `08.5` SP1 — three arenas under one module tag. **Ships `MirAttribute<T>`** as `substrate::ArenaAttribute<Mir, T>` (+ `MirBlockAttribute`/`MirFuncAttribute` on the sibling arenas) mirroring `NodeAttribute<T>` / `HirAttribute<T>`. Closed `MirOpcode` + `opcodeInfo()` table, `MirLiteralPool`, `instBlock` reverse-lookup, create-then-fill `MirBuilder` (forward branches), finish-time freeze validation. 6-perspective review; 87/87 ctest. |
| ML2 🟡 | HIR→MIR lowering                                 | **Cycle 1 ✅ done 2026-05-28**: new `src/mir/lowering/` OBJECT lib with `lowerToMir(hir, literals, interner, reporter) → HirToMirResult`. Vertical slice through straight-line c-subset: Function + Block + ReturnStmt + Literal + Ref + BinaryOp (16 ops, type-driven signed/unsigned/float pick). Implicit-void-return synthesis. New `MirBuilder::openBlockHasTerminator()` introspection. Failed lowerings seal blocks with `addUnreachable` (no aborts). Top-level Global/Extern*/ImportGroup emit fail-loud `H_UnsupportedLoweringForKind`. **Cycle 2 ✅ done 2026-05-28**: control flow lowering: `IfStmt` (diamond CFG with lazy join + unreachable-seal for both-arms-return), `WhileStmt` (header/body/exit), `DoWhileStmt` (body-then-CondBr), `ForStmt` (init/header/body/update/exit with update on the back-edge as LoopLatch). `UnaryOp` lowering (Neg, BitNot, logical Not lowered as `cmp eq operand, 0` — review-fix). New `MirBuilder::isBlockUnopened()` + lowerer `sealCreatedAsUnreachable` helper for idempotent recovery (every error path now seals all forward-created blocks so `finish()` never aborts). 88/88 ctest including 14 ML2 tests. **Cycle 3a ✅ done 2026-05-28**: expression-level closure pass — `lowerToMir` takes an optional `HirSourceMap*` and `unsupported()` diagnostics now carry span+buffer when bound (HR2 IOU finally closed for ML2). Four new expression lowerings: `Call` (`[callee, args...]` → MIR `Call` with `Optional` result-rule for void callees), `IntrinsicCall` (intrinsic id in payload), `Ternary` (diamond CFG with phi at join), `LogicalAnd`/`LogicalOr` (short-circuit diamond with phi). Forward-reference resolution via a pre-pass populating `functionSymbols` set; `Ref`-to-function emits `GlobalAddr(SymbolId)` whose result type is the FnSig directly (HIR convention). New `MirBuilder::currentlyOpenBlock()` introspection — strict: returns `InvalidMirBlock` when the last-opened block has already been sealed, so phi-predecessor capture errors are obvious rather than silent. 5-agent review fixes folded (set-disguised-as-map, forward-cycle comments, post-terminator sealed footgun). 88/88 ctest including 4 new tests. **Cycle 3b ✅ done 2026-05-28**: lvalue-via-alloca model — `lowerToMir` now takes `TypeInterner&` (non-const, to mint pointer types on demand, mirroring HIR lowering's convention); new `addressableLocal` map (sym.v → alloca MirInstId) with the load-bearing EITHER/OR invariant (`allocaForLocal` asserts the symbol isn't already in `symbolToValue`). `VarDecl` → `Alloca` + optional initial `Store`. `AssignStmt` → `Store(rhs, ptr)` via `lowerLvalueAddress` (`Ref(addressable-local)` → alloca; `Deref(ptr)` → pointer value — NO double-load). `AddressOf` returns the alloca directly for `Ref`, cancels `&*p`; `Deref` emits `Load(ptr)`; `Ref` lookup probes alloca first. Address-of-param is gap-free via a `collectAddressTakenSymbols` pre-pass that slot-promotes any param whose `AddressOf(Ref)` appears in the body (Arg + Alloca + Store on entry). 7 new tests pin: VarDecl(+init/-init), AssignStmt, AddressOf(local), AddressOf(param), pure-SSA negative control, and `*p = v` (the no-double-load contract). Also fixes a missing `<array>`/`<vector>` include in hir_to_mir.cpp that MSVC CI surfaced. 88/88 ctest, 25 in mir lowering. 5-agent review fixes folded inline (invariant hardening, forward-cycle prose scrub, `*p = v` coverage). **Cycle 3c ✅ done 2026-05-28**: MemberAccess + Index + SeqExpr lowered through a shared `lowerLvalueAddress` helper. `MemberAccess` emits `Gep(basePtr, const-0, const-fieldIdx)` then `Load`; `Index` emits `Gep(basePtr, idx)` for pointer bases and `Gep(basePtr, const-0, idx)` for array/struct bases (discrimination via `TypeKind::Ptr` in both the lvalue path AND `collectAddressTakenSymbols`, kept in sync). `AddressOf` now delegates to `lowerLvalueAddress`, so `&p->x` / `&arr[i]` work for free. The slot-promotion pre-pass extended to register params used as `s.field` / `s[i]` bases (non-pointer), so by-value struct/array params get a storage slot. New `constInt(int64)` helper mints Const+I32 for GEP indices. The `UnsupportedConstruct…` test repointed from `a[0]` (now supported) to `switch` (genuinely deferred — SwitchStmt/CaseArm + break/continue is a separate cycle); 5 new tests pin MemberAccess read/write, Index over pointer, AddressOf-MemberAccess, SeqExpr. **Cast lowering deferred (real blocker)**: HR doesn't emit `HirKind::Cast` from any frontend yet (no implicit-conversion wiring), so a MIR Cast lowering would be dead code — re-opens when HR starts emitting them. 88/88 ctest, 30 in mir lowering. 5-agent review fixes folded inline (generalized lvalue diagnostic, threaded HirNodeId anchor through `allocaForLocal`). **Plan 12 ML2 row: cycles 1–3 done; ML2 next: control-flow gaps (Switch/Break/Continue) + addressable-globals + cross-fn Call resolution**. |
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
