# MIR + LIR — Sub-Plan

> Mid-level IR (SSA over CFG, with structured-CF markers preserved) plus low-level IR (per-target ISA, virtual + physical registers, machine-shape instructions). Together they form the optimization + native-codegen middle tier between [HIR](./09-hir-plan%20-%20ok.md) and the [in-tree assembler](./13-assembler-plan%20-%20tbd.md) / [linker](./14-linker-plan%20-%20tbd.md).
>
> **Why both in one plan.** Their interface is tight: MIR's lowered shape directly determines LIR's instruction selection patterns. Splitting buys nothing and risks the seam.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟡 **in progress.** v1 production-critical. **ML1 ✅** (2026-05-27, `feature/mir-lir`): MIR skeleton — three arenas under one `MirModuleId`, fused value model (`MirValueId = MirInstId`), phi nodes, closed `MirOpcode` + `opcodeInfo()`, `StructCfMarker` block field, `MirLiteralPool`, build-once-freeze, create-then-fill builder. **ML2 ✅** (2026-05-27..28): HIR→MIR lowering through 6 cycles — straight-line + control flow + expression-level closure (Call/IntrinsicCall/Ternary/LogicalAnd-Or) + lvalue-via-alloca (AddressOf/Deref/MemberAccess/Index/SeqExpr) + Switch/Break/Continue + cross-fn Call verification + globals substrate (4th arena + fold-first/init-function fallback) + HIR Cast emission (C99 §6.3.1.8 implicit coercion) + ConstructAggregate (zero-seed + InsertValue chain). **ML3 ✅** (2026-05-28): MirVerifier substrate — 7 rule families, 12 `I_*` diagnostic codes at 0xA00x, Cooper-Harvey-Kennedy dominator + SSA use-dom-def + orphan-block detection. **ML4 ✅** (2026-05-28): `.dssir` text format + round-trip + parser-resilience. **ML5 ✅** (2026-05-29): LIR substrate + JSON-configured backend targets (`src/dss-config/targets/*.target.json`, plot-twist cycle 2a) + register file & calling conventions in JSON (cycle 2b) + MIR→LIR isel cycle 3 series CLOSED through cycle 3e — full c-subset HIR→MIR→LIR end-to-end with arithmetic, casts, CFG (Br/CondBr/Switch/Phi/ICmp), memory ops (Alloca/Load/Store/Gep), wide-literal pool, float/bitwise + cross-class Bitcast, Calls/IntrinsicCall/GlobalAddr, Aggregate ops (memory-flattening). New `LirVerifier` substrate with 3 cross-tier rules consuming the `MirToLirResult.lirToMir` mapping. **ML6 🟡 cycle 1 ✅** (2026-05-29, commit `bc596ae`): LIR liveness analysis substrate — `VRegBitset` sentinel-aware bounds-checked set, `LirLiveRange::make()` factory, `LirLiveness::forFunc()` accessor, RPO via `funcBlockAt`-enumeration (no contiguous-arena assumption), iterative backward dataflow + half-step position numbering, orphan-tolerant. Target-blind + source-blind. All 4 cycle-3e ML6-anchored deferrals folded (D-3e.1 two-pin lowerSwitch, D-3e.7 buildSyntheticFn promotion, D-3e.8 reverse Bitcast, D-3e.9 poisonValue cascade-prevention). 96/96 ctest. **ML6 cycle 2 ⏳ next**: linear-scan allocator consuming the substrate; co-designs sub-interval `LirLiveRange` (§3.1 D-ML6-1.1) with allocator's split-point selection. ML7 (callconv + frame) + ML8 (LIR text + round-trip) ⏳. |
| Predecessors  | ✅ [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) (arena reuse — SP1+SP2+SP3 complete). ✅ [`09-hir-plan`](./09-hir-plan%20-%20ok.md) (input — HR1–HR11 ✅ 2026-05-26..28 (HR8 = config-driven CST→HIR lowering on c-subset, incl. a `HirLiteralPool` of decoded literal values MIR reads; HR9 enriched toy into a typed language + un-deferred arrays → `Array<T,N>` types MIR will lower; HR10 added tsql-subset lowering), HR11 ✅ done 2026-05-28 (multi-language CU lowering) — plan 09 complete; HR7's `.dsshir` text format is the sibling-discipline reference for ML4's `.dssir` / ML8's `.dsslir` round-trip). |
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
├── lir.hpp / .cpp           # Lir + LirBuilder (target-blind)
├── lir_node.hpp             # LirInst/LirBlock/LirFunc PODs
├── lir_reg.hpp              # Virtual + physical register types
├── lir_regalloc.hpp / .cpp  # Linear-scan                       (ML6)
├── lir_callconv.hpp / .cpp  # Calling-convention lowering        (ML7)
├── lir_frame.hpp / .cpp     # Stack frame materialization        (ML6/ML7)
├── lir_verifier.hpp / .cpp                                       (ML7)
└── lir_text.hpp / .cpp      # .dsslir round-trippable text       (ML8)

# ML5 cycle 2 PLOT TWIST: targets are JSON-configured, mirroring the
# frontend GrammarSchema pattern. The old `src/lir/targets/*.hpp` per-
# processor headers from the cycle-1 sketch are DELETED — each target
# now lives entirely as a JSON file:
src/dss-config/targets/
├── x86_64.target.json       # x86_64 opcode set + (cycle 2b) regfile + ABI
└── arm64.target.json        # (drops in trivially once cycle 2b lands)

# The substrate consumes them via `TargetSchema::loadShipped("x86_64")`,
# returning a `shared_ptr<TargetSchema>`. `LirBuilder(TargetSchema const&)`
# is fully target-blind — opcodes are `uint16_t` keyed by `mnemonic`,
# dispatched through `schema.opcodeInfo(op)` / `schema.isTerminator(op)`.
src/core/types/target_schema.hpp / .cpp / _json.cpp
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
- **ML5 cycle 3e shipped 3 rules** (MemBase/MemOffset pairing; Store reg-class vs MIR pointee-type; vreg-class vs MIR-type — driven by `MirToLirResult.lirToMir` side-table).
- **Cycle-3e deferrals absorbed here** — D-3e.4 (`regClassForCoreType` explicit aggregate-case enumeration), D-3e.5 (`to_lir_reg_class` named helper in `lir_reg.hpp`), D-3e.6 (`regClassForCoreType(TypeId, TypeInterner const&)` overload). Folded together in the §2.9 prep cycle preceding SIMD lane-class work. See §3.1.

---

## 3. PR breakdown

| PR  | Title                                            | Scope |
|-----|--------------------------------------------------|-------|
| ML1 ✅ | MIR types, value/inst/block PODs, IDs            | **Done 2026-05-27** (`feature/mir-lir`). Strong IDs (`MirModuleId` tag + `MirInstId`/`MirBlockId`/`MirFuncId` arena ids; `MirValueId = MirInstId` alias per the fused model). Arena via `08.5` SP1 — three arenas under one module tag. **Ships `MirAttribute<T>`** as `substrate::ArenaAttribute<Mir, T>` (+ `MirBlockAttribute`/`MirFuncAttribute` on the sibling arenas) mirroring `NodeAttribute<T>` / `HirAttribute<T>`. Closed `MirOpcode` + `opcodeInfo()` table, `MirLiteralPool`, `instBlock` reverse-lookup, create-then-fill `MirBuilder` (forward branches), finish-time freeze validation. 6-perspective review; 87/87 ctest. |
| ML2 🟡 | HIR→MIR lowering                                 | **Cycle 1 ✅ done 2026-05-28**: new `src/mir/lowering/` OBJECT lib with `lowerToMir(hir, literals, interner, reporter) → HirToMirResult`. Vertical slice through straight-line c-subset: Function + Block + ReturnStmt + Literal + Ref + BinaryOp (16 ops, type-driven signed/unsigned/float pick). Implicit-void-return synthesis. New `MirBuilder::openBlockHasTerminator()` introspection. Failed lowerings seal blocks with `addUnreachable` (no aborts). Top-level Global/Extern*/ImportGroup emit fail-loud `H_UnsupportedLoweringForKind`. **Cycle 2 ✅ done 2026-05-28**: control flow lowering: `IfStmt` (diamond CFG with lazy join + unreachable-seal for both-arms-return), `WhileStmt` (header/body/exit), `DoWhileStmt` (body-then-CondBr), `ForStmt` (init/header/body/update/exit with update on the back-edge as LoopLatch). `UnaryOp` lowering (Neg, BitNot, logical Not lowered as `cmp eq operand, 0` — review-fix). New `MirBuilder::isBlockUnopened()` + lowerer `sealCreatedAsUnreachable` helper for idempotent recovery (every error path now seals all forward-created blocks so `finish()` never aborts). 88/88 ctest including 14 ML2 tests. **Cycle 3a ✅ done 2026-05-28**: expression-level closure pass — `lowerToMir` takes an optional `HirSourceMap*` and `unsupported()` diagnostics now carry span+buffer when bound (HR2 IOU finally closed for ML2). Four new expression lowerings: `Call` (`[callee, args...]` → MIR `Call` with `Optional` result-rule for void callees), `IntrinsicCall` (intrinsic id in payload), `Ternary` (diamond CFG with phi at join), `LogicalAnd`/`LogicalOr` (short-circuit diamond with phi). Forward-reference resolution via a pre-pass populating `functionSymbols` set; `Ref`-to-function emits `GlobalAddr(SymbolId)` whose result type is the FnSig directly (HIR convention). New `MirBuilder::currentlyOpenBlock()` introspection — strict: returns `InvalidMirBlock` when the last-opened block has already been sealed, so phi-predecessor capture errors are obvious rather than silent. 5-agent review fixes folded (set-disguised-as-map, forward-cycle comments, post-terminator sealed footgun). 88/88 ctest including 4 new tests. **Cycle 3b ✅ done 2026-05-28**: lvalue-via-alloca model — `lowerToMir` now takes `TypeInterner&` (non-const, to mint pointer types on demand, mirroring HIR lowering's convention); new `addressableLocal` map (sym.v → alloca MirInstId) with the load-bearing EITHER/OR invariant (`allocaForLocal` asserts the symbol isn't already in `symbolToValue`). `VarDecl` → `Alloca` + optional initial `Store`. `AssignStmt` → `Store(rhs, ptr)` via `lowerLvalueAddress` (`Ref(addressable-local)` → alloca; `Deref(ptr)` → pointer value — NO double-load). `AddressOf` returns the alloca directly for `Ref`, cancels `&*p`; `Deref` emits `Load(ptr)`; `Ref` lookup probes alloca first. Address-of-param is gap-free via a `collectAddressTakenSymbols` pre-pass that slot-promotes any param whose `AddressOf(Ref)` appears in the body (Arg + Alloca + Store on entry). 7 new tests pin: VarDecl(+init/-init), AssignStmt, AddressOf(local), AddressOf(param), pure-SSA negative control, and `*p = v` (the no-double-load contract). Also fixes a missing `<array>`/`<vector>` include in hir_to_mir.cpp that MSVC CI surfaced. 88/88 ctest, 25 in mir lowering. 5-agent review fixes folded inline (invariant hardening, forward-cycle prose scrub, `*p = v` coverage). **Cycle 3c ✅ done 2026-05-28**: MemberAccess + Index + SeqExpr lowered through a shared `lowerLvalueAddress` helper. `MemberAccess` emits `Gep(basePtr, const-0, const-fieldIdx)` then `Load`; `Index` emits `Gep(basePtr, idx)` for pointer bases and `Gep(basePtr, const-0, idx)` for array/struct bases (discrimination via `TypeKind::Ptr` in both the lvalue path AND `collectAddressTakenSymbols`, kept in sync). `AddressOf` now delegates to `lowerLvalueAddress`, so `&p->x` / `&arr[i]` work for free. The slot-promotion pre-pass extended to register params used as `s.field` / `s[i]` bases (non-pointer), so by-value struct/array params get a storage slot. New `constInt(int64)` helper mints Const+I32 for GEP indices. The `UnsupportedConstruct…` test repointed from `a[0]` (now supported) to `switch` (genuinely deferred — SwitchStmt/CaseArm + break/continue is a separate cycle); 5 new tests pin MemberAccess read/write, Index over pointer, AddressOf-MemberAccess, SeqExpr. **Cast lowering deferred (real blocker)**: HR doesn't emit `HirKind::Cast` from any frontend yet (no implicit-conversion wiring), so a MIR Cast lowering would be dead code — re-opens when HR starts emitting them. 88/88 ctest, 30 in mir lowering. 5-agent review fixes folded inline (generalized lvalue diagnostic, threaded HirNodeId anchor through `allocaForLocal`). **Plan 12 ML2 row: cycles 1–3 done.** **Cycle 4 ✅ done 2026-05-28**: Switch + Break + Continue control flow. New `BranchFrame {continueBB, breakBB, continueReferenced}` stack pushed by `While`/`DoWhile`/`For`/`Switch` lowering. `BreakStmt`/`ContinueStmt` resolve via `hir.branchDepth(node)` as a de Bruijn index. `SwitchStmt` lowers as a discriminant + one block per arm + a `SwitchJoin` exit; the first `default:` arm becomes `defaultTarget`, others rejected loud; arms run in declaration order with C-style fall-through (an arm body that doesn't self-terminate branches to the next arm's block; the last arm falls to exit); continue inside switch is fail-loud (the frame's `continueBB` is invalid). DoWhile reshape: continueBB exists between body and exit so `continue;` runs the cond-test rather than re-entering body; when the body self-seals AND no `continue;` resolves to the frame, the cond-test is elided (no dead lowering of the cond expression, no MIR bloat). c-subset grammar gained `continue;` (keyword + rule + selector + mapping); HR8 adds the one-line ContinueStmt mapping. Cycle 3b pre-pass extended to slot-promote params used as `AssignStmt` targets (the param-as-lvalue gap). 6 new tests pin switch + break + continue + fall-through + do-while elision; the cycle-2 WhileLoopBodyEmitsBackEdgeToHeader test reactivated now that AssignStmt is lowered. 88/88 ctest, 36 in mir lowering. 5-agent review fixes folded inline (do-while elision + empty-MIR assertion restored). **Cycle 5 ✅ done 2026-05-28**: cross-fn Call verification + per-fn isolation pinned. 2 new tests: `MultipleFunctionsEachGetIsolatedMirFunc` (proves the cycle-3b per-function reset of `symbolToValue`/`addressableLocal` prevents cross-pollution between functions in the same module) and `ForwardReferenceCallResolvesViaPrePass` (caller declared BEFORE callee — relies on the function-symbols pre-pass to populate `functionSymbols` before any function body lowers). 88/88 ctest, 38 in mir lowering. **MIR globals substrate ✅ done 2026-05-28** (commit `f3bc33d`): 4th arena (`Mir::GlobalArena`) added with the canonical pattern — `detail::MirGlobal` POD (≤32B, three mutually-exclusive init shapes: literal-pool index / `MirFuncId` init function / zero-init), `MirGlobalId` strong id, `MirBuilder::addGlobal` + `literalPoolAdd`, sentinel-aware `globalAt`/`moduleGlobalCount`, `MirGlobalAttribute<T>` typedef, ctor mismatch guard widened 3-way→4-way, freeze sweep validates `initFunc`/`initLiteralIndex` slot ranges. 5 new tests + 4 death tests pin builder shape, mutual-exclusion contract, freeze-boundary dangling-ref aborts. 40 in mir test binary, 88/88 ctest. **ML2 globals lowering ✅ done 2026-05-28** (commit `a13dfac`): full fold-first / fall-back-to-init-function policy. `tryConstFold` covers `Literal` + integer `UnaryOp(Neg/BitNot)` + integer `BinaryOp` (Add/Sub/Mul/Div/Rem/BitAnd/Or/Xor/Shl/Shr + 6 comparisons) on integer literals, with div-by-zero and out-of-range shift refusing to fold; non-foldable inits drop into a synthesized `__module_init__` MirFunc (`CcSysV` placeholder; ML7 maps to target). `Ref` resolution gains a 3rd case (`globalSymbols.contains(sym)` → `GlobalAddr+Load`); `lowerLvalueAddress` likewise resolves global refs to `GlobalAddr(pointer)` so `AssignStmt`/`AddressOf`/`MemberAccess` over globals just work. `emitGlobals_` order: addGlobal-everything first (so a per-init failure can't strip unrelated globals from MIR), then build the init-function body; per-init failure skips that Store and continues. 5 new lowering tests pin literal-fold + unary-fold + binary-fold + zero-init + cross-function read/write through GlobalAddr. **HR Cast emission + MIR Cast lowering ✅ done 2026-05-28** (this cycle): full implicit-coercion pass in HR — `TypeInterner::commonType` implements C99 §6.3.1.8 (float hierarchy → integer promotions per operand → same-signed widening → cross-signed tie-break), `coerce(child, target)` emits `HirKind::Cast` at 8 sites (BinaryOp commonType for both operands; LogicalAnd/Or operands to Bool; Return value to declared return type via `currentReturnType_` thread; AssignStmt rhs to lhs.type; VarDecl init to declared type; both Call sites for each arg to its FnSig param type; Ternary cond to Bool + arms to commonType; compound-assign via full C99 `(T)((commonT)lhs OP rhs)` model; If/While/For/DoWhile cond to Bool). MIR `mapCast(fromKind, toKind)` covers all 12 cast opcodes (Trunc/SExt/ZExt/FPTrunc/FPExt/SIToFP/UIToFP/FPToSI/FPToUI/Bitcast/IntToPtr/PtrToInt). `tryConstFold` extended with a `Cast` case so narrowing/widening literal initializers (`int g = 1 > 0;`) still fold to constant-init globals instead of synthesizing a `__module_init__`. Synthetic Cast nodes alias to their operand's source-map entry so diagnostics anchored at them locate to real source. 4-review-finding fixes folded inline: (1) `commonType` same-type early-return correctly bypassed when integer promotion is owed; (2) compound-assign coerces both operands to commonType + narrows result back to lhs; (3) source-map aliasing; (4) tryConstFold sees through Cast. 3 new lowering tests + 2 updated tests; golden file updated. **Float-arithmetic fold + extension-type casts** still delimited at [`12.5-const-eval-plan`](./12.5-const-eval-plan%20-%20ok.md) (CE5 + CE3 respectively). **ML2 cycles 1–5 closed. Cycle 6 ✅ done 2026-05-28** (commits `9d1b3ad` base + `e0c5ad9` 7-agent review fix-up + `[next]` 3-agent review fix-up #2): HIR `ConstructAggregate` → MIR. Strategy: const-fold first via `evaluateConstant` (whole-subtree literal → one `Const(MirAggregateValue, ty)`); else `Const(zeroLiteralOf(ty, activeUnionVariant?)) + InsertValue chain` over each child at index `i`. Consumes the D5.6 ExtractValue/InsertValue substrate (was landed-but-unused). **3-agent fix-up #2 (this cycle)** collapsed `zeroLiteralOf` + `zeroLiteralOfUnionWithChild` into ONE helper carrying `std::optional<TypeId> activeUnionVariant` (the invariant — union's seed must match active variant — is now expressed in the signature, not split across two helpers). Active-variant validation: invalid TypeId AND not-among-declared-variants both diagnose loud. Caller snapshots `reporter.errorCount()` before `zeroLiteralOf`; any in-recursion diagnostic short-circuits the chain (closes the silent-failure gap where `unsupported()` reported but the bogus literal still flowed into `addConst` + InsertValue). Diagnostic label uses `core` (was `interner.kind(type)`, inconsistent). 7 ConstructAggregate tests (all-const-fold, runtime-chain, union-runtime, union-non-zero-variant, array-runtime, chain-topology, nested-all-const). 89/89 ctest. Plan-12 ML2 still ⏸️ pending the globals-substrate sub-PR + cross-CU type-import (for Cast — also a downstream-of-HR real blocker, separately tracked). |
| ML3 🟡 | MIR verifier + dominator-tree analysis           | **Cycle 1 ✅ done 2026-05-28** (commits `5f9518c` + `b9a067e` 3-agent review fix-up): `MirVerifier` substrate mirroring `HirVerifier` conventions (class + collect-all `verify(reporter) → bool` + delta-on-errorCount + hitCap guard + `reportInst/Block/Func` overloads with node id in `d.actual` + optional `TypeInterner` injection). 7 rule families: (1) `checkStructuralInvariants` (re-run ML1's opcode/arity/result-rule/const-payload on the frozen module — covers direct-`Mir`-ctor path); (2) `checkEntryBlocks` (exactly 1 EntryBlock marker per function at slot 0); (3) `checkBlockTermination` (last inst is a terminator); (4) `checkStructCfMarkers` (count-paired: IfThen==IfJoin, IfElse≤IfJoin, LoopHeader==LoopExit, LoopLatch≤LoopHeader [latch is optional]; ExitBlock terminates in Return/Unreachable); (5) `checkPhiIncomings` (phi.pred ∈ CFG-predecessors via O(V+E) inversion of blockSuccessors); (6) `checkDomination` (per-function Cooper-Harvey-Kennedy iterative dominator + SSA use-dom-def + orphan-block detection via `I_UnreachableBlock`); (7) `checkTypeInvariants` (interner-gated — no Extension types in MIR, CondBr=Bool, Return matches FnSig, Arg.argIndex < paramCount). 12 new `I_*` diagnostic codes at 0xA00x (renders as "I" via the `0xAxxx` nibble). 19 MirVerifier unit tests + 1 end-to-end test running ML2-lowered MIR (multi-fn corpus with arithmetic + conditional + while-loop) through the verifier — proves no false-positives on production-shape MIR. Termination safety: bounded step counts in both `intersect` + `dominates` so malformed direct-ctor input never loops infinitely. 90/90 ctest. **Cycle 2 successor**: MirSourceMap injection slot (parallel to HirSourceMap) when ML2 starts populating source spans on MIR insts. Dominator tree is currently re-computed inside the verifier; promoting to a sibling `MirCfg` substrate library happens when the optimizer (plan 10) or liveness (ML6) becomes the second consumer. |
| ML4 ✅ | MIR text format + roundtrip                      | **Cycles 1 + 2 ✅ done 2026-05-28** (commits `3888308`+`e22462e`): `.dssir` parser + emitter; verifier validates on load. Mirrors HR7 conventions exactly — `MirTextContext` (optional `TypeInterner const*` + `vector<string> const* symbolNames`) for emit; `MirParseResult { Mir, TypeInterner, vector<string> symbolNames, bool ok }` for parse, non-movable so the rebuilt Mir's arena address stays stable. Two-pass parser: pass 1 `scanBlockHeaders` pre-creates all blocks with their declared markers (closes forward-block-ref wrong-marker bug); pass 2 parses bodies with phi-incoming + initfunc-global deferred resolution. Closed `MirOpcode` → simpler than HIR text (no `ext_ops`/`intrinsics` preamble). Inline structural type emit/parse (`appendType`/`parseType`) mirrors HIR's pattern. Recursive aggregate literal grammar covers ML2 cycle 6's ConstructAggregate const-fold output. 3 new `I_Text*` diagnostic codes at 0xA00D-0xA00F. 7-agent review fix-up folded inline: Switch arrow bug (`Minus`→`Arrow`), IntrinsicCall explicit parser case, initfunc deferred resolution via funcMap_, finalize short-circuits when errors_ set (defeats `MirBuilder::finish()` abort path that would have killed the process on parse errors). 13 MirText round-trip tests + 1 end-to-end test running ML2-lowered c-subset MIR (add + factorial with if/return/multiply) through byte-equal round-trip. 91/91 ctest. **Cycle 2 ✅ done 2026-05-28** (commits `56c0051`+ MEDIUM-finding fix-up): bail-on-failure at 12 catastrophically-cascading structural-boundary sites (parseGlobal `Colon`/`Eq`, parseFunction `Colon`/`LBrace`, parseBlock `LBrace`, parseInstruction per-opcode `LParen`/`LBracket`/`LBrace` for Const/Arg/GlobalAddr/IntrinsicCall/Phi/Switch); panic-mode recovery in module body loop (skip to `global`/`function`/`}` anchor after stray-token diagnostic); `parseBlock` reorder (`beginBlock` AFTER LBrace bail so the builder never enters an Open-state block on parse failure). New test `MissingFunctionLBraceDoesNotCascade` pins ≤ 3 diagnostics (was 5+ in cycle 1). 16 MirText unit tests; 91/91 ctest. **ML4 fully closed end-to-end.** |
| ML5 🟡 | LIR template + instruction selection (x86_64 + ARM64) | **Cycle 2b ✅ done 2026-05-29** (commits `6476336` base + `[fix-up]` 7-agent fold): register-file + calling-convention sections in target JSON; ALL 3 cycle-2a-deferred items folded (shared `substrate::mintMonotonicId<DssStrongId>` consolidating 8 minters; `TargetSchemaData::validate()` cross-field invariants returning `vector<ConfigDiagnostic>` with shaped JSON paths; transparent-hash `TransparentNameIndex` shared across 3 maps); 33 x86_64 registers (GPR + FPR + flags) with subOf-cycle detection; 2 calling conventions (SysV AMD64 + Microsoft x64) with full caller/callee-saved sets, stack alignment power-of-two check, shadow/redzone alignment validation; static_assert synchrony LirRegClass↔TargetRegClass; +21 new TargetSchema tests + 3 mint_monotonic_id substrate tests; 94/94 ctest. <br>| **PLOT TWIST cycle 2 — targets are JSON-configured.** "Config-driven, no per-language C++" extends to backends: a compile target is a JSON file in `src/dss-config/targets/*.target.json`, NOT C++ code. Cycle 1's hardcoded `targets/x86_64.hpp` was a workaround for the JSON loader not existing yet; cycle 2 lifts opcodes into JSON. **Cycle 1 ✅ done 2026-05-29** (commits `c0eb1b6`+`13e6902`): LIR substrate skeleton. Per-target instruction sets via runtime `TargetId` tag (chose over C++-templated `Lir<TargetTraits>` for simpler substrate — no template instantiation in arenas/attributes/verifier, target-blind passes that don't need re-instantiation, uniform hosting). New `src/lir/`: `lir_opcode.hpp` (TargetId enum + LirOpcodeInfo descriptor + LirResultRule), `lir_reg.hpp` (4-byte LirReg POD with id / regClass / isPhysical bit-fields + GPR/FPR/VR/Flags universal envelope), `lir_node.hpp` (32B PODs for LirInst/Block/Func + 8B LirOperand variant Reg/Imm/BlockRef/SymbolRef/Mem* + ArenaNames specializations), `lir.hpp/.cpp` (Lir + LirBuilder mirroring Mir/MirBuilder discipline — create-then-fill blocks, build-once-freeze), `targets/x86_64.hpp` (minimal Mov/Add/Sub/Cmp/Jmp/Jcc/Call/Ret opcode set + Cc enum + GPR ordinals). New LirModuleId/LirInstId/LirBlockId/LirFuncId strong-ids. Per-target opcode-info dispatch `lirOpcodeInfo(TargetId, uint16_t) → LirOpcodeInfo` (switch over TargetId — new targets add their case). Fix-up folded inline: (1) `beginBlock` reopen guard via UINT32_MAX sentinel, (2) terminator builders verify `lirIsTerminator()`, (3) `closeFunction_` verifies last-inst is terminator, (4) `addInst` opcode-range guard, (5) `LirReg::operator==` excludes `_pad`. 11 LIR tests including 3 death tests. **Cycle 2a ✅ done 2026-05-29** (commits `2609b70`+`695cc8b`+`[fix-up]`): JSON-config pivot. New `TargetSchema` (`src/core/types/target_schema.{hpp,cpp,_json.cpp}`) mirroring `GrammarSchema`'s loader pattern (loadShipped / loadFromFile / loadFromText). New `TargetSchemaId` strong-id replaces the cycle-1 `TargetId` enum. `Lir` stores `TargetSchemaId`; `LirBuilder` takes `TargetSchema const&`. Opcode dispatch is `schema.opcodeInfo(op)` / `schema.isTerminator(op)` — no per-target C++ switch. Directory rename `src/source-config/` → `src/dss-config/` (with `languages` → `sources` + new `targets` subdir); 4 test files + `GrammarSchema::loadShipped` + `SchemaCache::discoverShippedLanguages` updated. First concrete target `src/dss-config/targets/x86_64.target.json` (Mov/Add/Sub/Cmp/Jmp/Jcc/Call/Ret). **7-agent review fold-in** (this cycle): shared `findShippedConfig` substrate (`src/core/types/config_path_walk.{hpp,cpp}`) consumed by both `GrammarSchema::loadShipped` + `TargetSchema::loadShipped` (cwd-walk + name-rejection no longer duplicated); `detail::TargetSchemaData` moved out of public surface (mirrors `detail::GrammarSchemaData` — invariants are loader-enforced only); `src/lir/lir_opcode.hpp` re-export shim deleted (consumers include `core/types/target_schema.hpp` directly); CI windows-msvc fix via `.gitattributes` pinning `src/dss-config/**` to LF (matches `tests/corpus/**` discipline); new `tests/core/test_target_schema.cpp` with 14 negative-path tests mirroring `test_grammar_schema.cpp` (malformed JSON / missing version / unsupported version / missing target / missing+empty opcodes / slot-0 sentinel / duplicate mnemonic / invalid result-rule / out-of-range arity / loadShipped path-rejection / not-found / distinct-id mint). 11 LIR tests + 14 TargetSchema tests + 93/93 ctest. **Cycle 2c ✅** folded into cycle 3a: `abiModel` JSON field (`register-machine`/`operand-stack`/`result-id`) + `TargetCallingConvention.linkRegister` + `TransparentString{Hash,Eq}` promoted to `core/substrate/`. **Cycle 3a ✅ done 2026-05-29** (commits `c707688` base + `[fix-up]` 7-agent fold): MIR→LIR isel vertical slice (Arg/Const/Add/Sub/Mul/Return) consuming the cycle-2b opcode vocabulary; new `L_*` diagnostic family at 0xB00x; new `arg` virtual-ISA pseudo-op; first consumer of TargetSchema. **Cycle 3b ✅ done 2026-05-29**: CFG + comparisons (Br/CondBr/Switch/Phi/ICmp* + Unreachable) + 2 cycle-3a deferrals folded (MirAttribute<LirReg>+MirBlockAttribute swap; linkRegister ordinal cache). New `setcc`/`unreachable` opcodes + universal `TargetCondCode` enum. Phi resolution via predecessor-edge mov-insertion pass. **Cycle 3c ✅ done 2026-05-29**: memory ops (Alloca/Load/Store/Gep) + LirLiteralPool substrate (with `LirOperandKind::LiteralIndex` operand + new `lir/lir_literal_pool.{hpp,cpp}`) + wide-literal path + cast lowering (Trunc/SExt/ZExt/Bitcast/IntToPtr/PtrToInt) + `MnemonicCache` array collapse (type-design's cycle-3b recommendation). **Cycle 3d ✅ done 2026-05-29**: bitwise (And/Or/Xor/Shl/LShr/AShr/Not + integer Neg) + float arithmetic (FAdd/FSub/FMul/FDiv/FNeg) + float casts (FPTrunc/FPExt/FPToSI/FPToUI/SIToFP/UIToFP) + FPR-class machinery via `regClassForType(TypeInterner const&)` + cross-class Bitcast detection (movq_xclass for FPR↔GPR) + dynamic-index Gep (4-operand `lea`) + double-literal reactivation through the LirLiteralPool→FPR path. 19 new x86_64 opcodes. `lowerToLir` signature extended with `TypeInterner const&`. **Cycle 3e ✅ done 2026-05-29**: Calls (Call/IntrinsicCall/GlobalAddr) + Aggregate ops (ExtractValue/InsertValue via memory-flattening, degenerate zero-index path; non-zero indices defer to ML6 frame-layout) + new `LirVerifier` substrate (`src/lir/lir_verifier.{hpp,cpp}`) with 3 rules (MemBase/MemOffset pairing + Store reg-class-vs-pointee-type + vreg-class-vs-MIR-type). Promoted `regClassForCoreType(TypeKind) → TargetRegClass` to substrate (`core/types/target_schema.hpp`) — now consumed by lowerer + verifier (the architect's "wait for 2nd consumer" promotion condition met). Promoted `MnemonicSlot ↔ mnemonic` from positional `kMnemonics` array to per-row `kMnemonicRows{slot, mnemonic}` struct table with consteval alignment check (closes the cycle-3d swap-silent-corruption hazard). New `intrinsic_call` opcode. **Plan 12 ML5 cycle 3 series CLOSED** end-to-end — full c-subset corpus lowers HIR→MIR→LIR with the LirVerifier catching class-consistency violations. Cycle ML6 (linear-scan regalloc) ⏳ next. **Cycle 1b** dissolves into "drop a new `.target.json`" (no C++ work). Ships `LirAttribute<T>` typedef in `lir.hpp`. |
| ML6 🟡 | Linear-scan register allocation                  | Per `07-prod-readiness G-512`. Live-range computation + virtual-reg coloring + spill-slot insertion. **Cycle 3b ✅ done 2026-05-29**: rewrite pass + spill load/store insertion + `verifyLirPostRegalloc` rule. `src/lir/lir_rewrite.{hpp,cpp}` walks every inst and replaces virtual regs with physical assignments; spilled vregs become `frame_load <scratch>, slot` (USE) + `frame_store <scratch>, slot` (DEF) pseudo-op pairs. ML7 callconv lowering will materialize the pseudo-ops as per-target `mov reg, [SP + offset]`. `verifyLirPostRegalloc` moved to `lir_verifier.{hpp,cpp}` per architect HIGH (verifier tier separation). New diagnostic codes: `L_VirtualRegInPostRegalloc` (0xB003) for virtual-reg violations + `L_InvalidSpillSlotSentinel` (0xB004) for frame-op payload-0 violations. 7-agent review fold-in inline: scratch-pool extended from single-reg-per-class to ordered list with per-inst cursor (closes the silent miscompile path 2 agents flagged CRITICAL where multiple same-class spilled operands would share ONE scratch and lose earlier loads to later overwrites); rsp/rflags filter via cc allocatable pool (architect CRITICAL); cc==nullptr → loud Error rather than silently re-enabling rsp; mid-failure builder leak documented in failure-mode contract (return empty Lir + `ok=false`; consumers MUST check ok). 12 LirRewrite tests including `MultiSpilledSameClassOperandsGetDistinctScratches`, `VerifierFiresOnFrameOpWithZeroSlotPayload`, `SwitchBearingFunctionRewritesAndPreservesBlockCount`, `ScratchRegPickedFromCallingConventionPoolOnly`, `ExhaustedClassEmitsLoudFailureNotSilentClobber`. 98/98 ctest. **Cycle 3 series CLOSED** end-to-end (3a + 3b). **Cycle 3a ✅ done 2026-05-29**: folds the two cycle-2 deferrals + adds R_* diagnostic family. `LirRegAssignment.assignment` migrated to `std::variant<LirReg, LirSpillSlot>` (new strong id at `src/core/types/strong_ids.hpp`); `LirAllocation::ok()` is now a method derived from per-function `LirFuncAllocation::ok` (was stored field that could drift); `DiagnosticReporter&` threaded through `allocateRegisters`/`allocateFuncRegisters` with 5 new R_* codes (R_NoCallingConventions, R_CallingConventionLookupFailed, R_VRegHasNoClass, R_SpilledDueToPressure, R_SpilledDueToCrossCallExhaustion) at 0x4xxx. 7-agent review fold-in inline: per-function `R_SpillSummary` aggregate replaces per-vreg notes (closes the silent-cap-50 drop hazard); schema-wide check hoisted to module-level entry; evict-path now computes the EVICTED range's own `crossesCall` (was hardcoded false — would mis-attribute cross-call spills). New nibble-reservation comment for 0x5 → `O0xxx` (linker family) to pre-empt plan-14 conflict with C_*/D_*. 19 LirRegAlloc tests + 97/97 ctest. **Cycle 1 ✅ done 2026-05-29** (`bc596ae`): liveness analysis substrate (`src/lir/lir_liveness.{hpp,cpp}`) with `VRegBitset` sentinel-aware bounds-checking set, `LirLiveRange::make()` factory, `LirLiveness::forFunc(LirFuncId)`, RPO via `funcBlockAt`-enumerated mapping, half-step position numbering, orphan-block-tolerant. Target-blind + source-blind. All 4 cycle-3e ML6-anchored deferrals folded (D-3e.1/D-3e.7/D-3e.8/D-3e.9). 7-agent review fix-up inline. Real bug caught by tightened test: liveOut range-end overshoot by 1 (fixed via `lastUse = blockEndPos - 1`). 12 LirLiveness tests. **Cycle 2 ✅ done 2026-05-29**: linear-scan allocator (`src/lir/lir_regalloc.{hpp,cpp}`) — classical Poletto-Sarkar with same-class-latest-end spill heuristic + `requireCalleeSaved`-aware spill candidate search; `LirRegAssignment` factory with phys-XOR-spill invariant; `LirFuncAllocation`/`LirAllocation` with `forVReg`/`forFunc` accessors; call-aware constraint (ranges live PAST a call's late slot avoid caller-saved regs); reserved-register filtering (rsp/rflags never allocated — they appear in no calling-convention saved/arg/return set). **Target-agnostic via `TargetOpcodeInfo::isCall` flag** (new schema field — was previously matching "call"/"intrinsic_call" mnemonic strings, a thesis-decision-#4 violation). 7-agent review fix-up inline: `rangeCrossesCall` strict-crossing fix (was overcounting call-args-only vregs as crossing); `kLirRegClassCount` derived from `LirRegClass::Flags + 1` with `static_assert` synchrony; `cls == None` → fatal (was silent `continue`); test tautology closed via `ASSERT_GT(crossingCount, 0u)`; new `ReservedStackPointerNeverAllocated` test (pins the rsp-allocation CRITICAL); new `FprClassRangesGetFprRegisters` test (FPR coverage gap closed); stripped misattribution of "Wimmer-Franz" (heuristic is Poletto-Sarkar); promoted `LoweredLir` + `lowerCSubsetToLir` to shared `tests/lir/lowered_lir_fixture.hpp`; popReg LIFO O(1) (was O(N) `popFront`); unified `findSpillCandidate(requireCalleeSaved)`. 15 LirRegAlloc tests + 97/97 ctest. **Cycle 3 ⏳ next** (rewrite pass + spill load/store insertion + `LirVerifier` "no virtual registers after regalloc" rule): consumes `LirAllocation` side-table to rewrite `Lir` with phys regs + spill ops. ML7 (callconv + frame) + ML8 (LIR text + round-trip) ⏳. |
| ML7 | Calling-convention + stack frame materialization | SysV AMD64, Microsoft x64, AAPCS64, Microsoft ARM64. Prologue/epilogue. **Absorbs cycle-3e deferral D-3e.2 (IntrinsicCall void/non-void verifier rule) and D-3e.3 (`regClassForType(InvalidId)` diagnostic)** — see §3.1. |
| ML8 | LIR text format + verifier + round-trip          | `.dsslir` per-target; verifier validates; disasm round-trip pins. |

### Post-ML8 substrate items mapped here

- **`EvalOptions` env/policy split** (from plan [12.5 §0.2 D4](./12.5-const-eval-plan%20-%20ok.md)): when SizeOf / type-layout queries land alongside MIR's type-layout substrate, `const_eval`'s `EvalOptions` gets a second function-typed field (`resolveTypeLayout`). At that point the closure-carrying environment is extracted to a sibling `EvalEnvironment` struct (policy bools stay in `EvalOptions`); this avoids the config-object antipattern called out by plan 12.5 CE5's architecture review.

### 3.1 ML5 cycle 3e deferred items — explicit anchoring

Real-blocker deferrals from ML5 cycle 3e (landed 2026-05-29) must each have a future home. Anchored here per the standing rule "deferred items to be explicitly in some future part of the plan":

| # | Deferred item | Source | Real blocker | Landing window |
|---|---------------|--------|--------------|----------------|
| D-3e.1 | `lowerSwitch` first-cmp implicit-block-placement assertion (assert `lir.openBlock() == headerBlock` before emitting the first compare so a future builder refactor cannot silently route the compare into the wrong block) | silent-failure LOW | none — pure hardening; defer is cosmetic. | **ML6 cycle 1** (regalloc reshapes block boundaries; assertion goes in alongside that pass). |
| D-3e.2 | IntrinsicCall void/non-void result-validity verifier rule (4th `LirVerifier` rule: when MIR `IntrinsicCall` produces a typed result, the LIR `intrinsic_call` must have a valid result reg; when MIR result type is Void, LIR result must be `InvalidLirReg`) | silent-failure HIGH | none for the rule itself; the existing rules 1–3 already cover the high-frequency cases. Rule 4 wants the intrinsic registry's signature table, which lands with ML7 callconv lowering. | **ML7** (callconv pass already touches per-intrinsic signature plumbing). |
| D-3e.3 | `regClassForType(InvalidId)` diagnostic (currently the substrate helper silently returns `TargetRegClass::GPR` for `TypeKind::Invalid`; should emit a verifier-tier diagnostic or assert) | silent-failure MEDIUM | none — `Invalid` should never reach the lowerer in practice; the defensive diagnostic is hardening. | **§2.9** (folded into the cycle that adds the 4th LirVerifier rule — see D-3e.2). |
| D-3e.4 | `regClassForCoreType` default-arm → explicit aggregate cases (replace the `default: return GPR` arm with explicit `case Struct: case Union: case Array:` so a new `TypeKind` (e.g. SIMD lane) fires a compile-time `[[fallthrough]]` warning instead of silently inheriting GPR) | type-design enum-drift safety | none — purely defensive; current consumers don't hit the path. Waits for SIMD/vector opcodes (open question #7, reserved post-v1) so the aggregate-vs-vector split is real. | **§2.9** prep cycle preceding SIMD lane-class work; folded with D-3e.5+D-3e.6. |
| D-3e.5 | `to_lir_reg_class(TargetRegClass) → LirRegClass` helper in `lir_reg.hpp` (replace the inline `static_cast` at 3 sites in `lir_verifier.cpp` with a named helper carrying the substrate↔LIR class-mapping contract) | type-design hardening | none. Cosmetic; the `static_cast` is correct because of the `static_assert` synchrony pinned in cycle 2b. | **§2.9** prep cycle (folded with D-3e.4 + D-3e.6). |
| D-3e.6 | `regClassForCoreType(TypeId, TypeInterner const&) → TargetRegClass` overload (DRY against the 4 sites in `mir_to_lir.cpp` + `lir_verifier.cpp` that currently call `regClassForCoreType(interner.kind(ty))`) | architect MED | none. | **§2.9** prep cycle (folded with D-3e.4 + D-3e.5). |
| D-3e.7 | `buildSyntheticFn` test helper promotion (lift from `tests/lir/test_mir_to_lir.cpp` to a shared header) | test substrate | a second test file consuming it. Currently only `test_mir_to_lir.cpp` uses it. | **ML6** test cycle (linear-scan needs synthetic MIR for spill scenarios — that's the 2nd consumer). |
| D-3e.8 | Reverse Bitcast I64→F64 test (currently only F64→I64 covered) | test coverage | none. Lowerer's class-symmetric Bitcast handler is direction-agnostic; gap is purely test-coverage. | **ML6** test cycle (same fold-window as D-3e.7). |
| D-3e.9 | `condCodeForICmp` blind-deref hardening (return `std::optional<TargetCondCode>` so a future ICmp opcode addition doesn't blind-deref past the table) | cosmetic | none. Current dispatch switch already gates entry. | **ML6 cycle 1** ✅ done 2026-05-29 (call site now `has_value`-gates + `poisonValue` to suppress diagnostic cascade). |
| D-ML6-1.1 | Split-aware sub-interval `LirLiveRange` list (currently flat single interval per vreg; linear-scan with split-point selection wants disjoint `[start_i, end_i)` segments per vreg) | architect HIGH in ML6 cycle 1 review | ML6 cycle 2 verdict: flat intervals + call-aware constraint (callee-saved-only for live-across-call vregs) shipped and verified correct on all 15 regalloc tests. Sub-intervals refine spill-point quality (avoids burning a callee-saved reg for a vreg dead during the call window) — quality improvement, not correctness. | **ML6 cycle 4+** (post rewrite pass) — promote when register-pressure profile motivates it. |
| D-ML6-2.1 ✅ | `LirRegAssignment` → variant payload | **landed ML6 cycle 3a 2026-05-29**: `std::variant<LirReg, LirSpillSlot>` (new strong id) replaces struct-with-sentinel. Type-design + architect converged on the migration. |
| D-ML6-2.2 ✅ | `DiagnosticReporter&` threading | **landed ML6 cycle 3a 2026-05-29**: full `R_*` diagnostic family at 0x4xxx (R0xxx render), 5 codes covering schema-config errors + per-function spill summary. Per-function `ok` flag; module-level `LirAllocation::ok()` derived. |
| D-ML6-3.1 ✅ | Rewrite pass + spill load/store insertion + `LirVerifier` "no virtual registers after regalloc" rule | **landed ML6 cycle 3b 2026-05-29**: `src/lir/lir_rewrite.{hpp,cpp}` + `verifyLirPostRegalloc` in `lir_verifier.{hpp,cpp}` (architect HIGH tier-separation). `frame_load`/`frame_store` LIR pseudo-ops in `x86_64.target.json`. Per-class scratch POOL (not single scratch — closes the multi-spilled-same-class silent miscompile that 2 agents independently flagged CRITICAL). 7-agent re-review (5 hit a session limit in round 1; re-ran post-reset). |
| D-ML6-3.2 | `TargetOpcodeInfo::payloadKind` + verifier rule (closes the payload channel's type-safety hole — frame slot id, TargetCondCode, argIndex all share a raw uint32_t channel) | type-design top finding. Cross-cycle change touches every payload-using opcode (cycle 3a–3e). | **cycle 4+ (LIR substrate cleanup pass)** — fold alongside `kLirRegClassCount` + `PerLirRegClass<T>` promotion to `lir_reg.hpp` (type-design Action 2). |
| D-ML6-3.3 | Frame-op mnemonics promoted to `TargetSchema` fields (`frameLoadOpcode`, `frameStoreOpcode`) — currently hardcoded `"frame_load"`/`"frame_store"` in rewrite + verifier. Targets that name the pseudo-op differently silently miss the verifier rule. | silent-failure HIGH in ML6 cycle 3b review. Requires TargetSchema schema-side addition. | **ML7 callconv lowering cycle** — natural fold-in when ML7 starts consuming the frame ops for stack-pointer materialization. |
| D-ML6-3.4 | Multi-spilled-operand reg-rotation (current implementation pools per-class scratches and assigns by cursor; with > #scratches spilled same-class operands in one inst, emits loud Error rather than rotating registers via temp spills) | architect note ("substrate cleanup at cycle 4+"); currently no production corpus produces > 1 same-class spill per inst beyond the pool size. | **cycle 4+** — implement when register-pressure profile motivates. |

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
