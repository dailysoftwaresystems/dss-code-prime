# HIR-HW (Hardware IR) — Reserved Sub-Plan (19)

> **Reserved scope.** Owns the **sibling-of-HIR** intermediate representation for hardware description languages (VHDL, Verilog, SystemVerilog). Per the user's stated final goals: "From any configured language ... build any configured language output ... this will include WASM and VHDL/Verilog."
>
> Software HIR ([`09-hir-plan`](./09-hir-plan%20-%20tbd.md)) is **fundamentally sequential**. Hardware is **concurrent + signal-typed + clock-bound**. The shapes do not unify cleanly. A sibling HIR-HW is the right answer.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🔒 **reserved.** No work begins until a real VHDL/Verilog target is requested. Plan exists to (a) prevent software HIR (plan 09) from designing in ways that foreclose hardware semantics and (b) reserve the namespace + name. |
| Predecessors  | ✅ [`09-hir-plan`](./09-hir-plan%20-%20tbd.md) (software HIR exists, proves the substrate — HR1 ✅ 2026-05-26 proves the open-core+extensions discipline this sibling-IR will mirror). ⏳ [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) (lattice + arena substrate reused). |
| Successors    | Hardware-targeting language pairs in [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md). |
| Scope         | **Unspecified — placeholder.** Will detail when triggered. |

---

## 1. Trigger

This plan opens when:
- A user explicitly requests VHDL or Verilog output for a project, OR
- A language schema declares hardware-shaped semantics (concurrent processes, signal types, clock domains), OR
- The user's custom language (per `20-custom-language-reserved-plan`) adopts hardware constructs.

Until then: namespace reserved; design assumed; no code lands.

---

## 2. Design (sketch only)

What HIR-HW must capture that software HIR cannot:

| Concept | Why HIR can't express it |
|---|---|
| **Concurrent processes** | HIR is sequential — function bodies execute top-down. Hardware processes run in parallel; ordering between them is undefined except at clock edges. |
| **Signal types** (`std_logic`, `std_logic_vector<N>`, `signed<N>`, `unsigned<N>`) | These are first-class lattice extensions; the lattice supports them, but their **assignment semantics** (signal vs variable: signal writes don't propagate until next delta cycle) differs from HIR's expression evaluation model. |
| **Clock domains + sensitivity lists** | `process(clk, reset) begin ... end` — only re-evaluates when clk/reset changes. No HIR construct represents "this block re-runs on input edge." |
| **Wires vs registers** | HIR knows mutable vs immutable; hardware also knows combinational (continuous assignment) vs sequential (clocked). |
| **Bit-level operations + fixed-point arithmetic** | Some software lattice operations exist; hardware needs **bit-precise** lattice members (`signed<33>` vs `signed<64>` — software HIR conflates close-but-different widths). |
| **Latency annotations / timing constraints** | Not a software concept. |

### 2.1 Key design constraints (locked in advance)

- **Sibling, not extension.** A separate `src/hir_hw/` tree. Same arena substrate (`08.5-substrate-prep-plan` SP1). Distinct ID space (`HirHwNodeId`, `HirHwSignalId`, `HirHwProcessId`).
- **Lattice reuse where it works.** Bit-vector lattice members (`Signed<N>`, `Unsigned<N>`, `BitVec<N>`, `StdLogic`, `StdLogicVec<N>`) live in the shared lattice (per `08.5-substrate-prep-plan` SP2 extension registry). Software code that uses them stays type-checked; hardware code uses them with signal semantics.
- **No MIR / LIR convergence.** HIR-HW lowers directly to target-language CST (VHDL / Verilog text) via reusable plumbing from [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md) (ST3 + ST4). There is **no SSA / register-allocation intermediate** — hardware doesn't have registers in the CPU sense.
- **Diagnostic namespace.** Provisionally `HW_*` (two-letter prefix to avoid colliding with `H_*` HIR). Finalized when this plan opens; current rev-2 conventions track `H_/I_/L_/K_/F_/T_/A_/B_/G_/SH_/W_/D_/R_/S_/P_/C_` per the per-plan declarations.

### 2.2 What plan 09 must NOT do (to keep HIR-HW future-open)

- Don't assume HIR's `Function` kind covers everything callable. Hardware processes don't return; they react.
- Don't bake "sequential statement order matters" into MIR-lowering invariants. HIR-HW won't go through MIR, but HIR's statement-list semantics shouldn't be hardcoded into the verifier (the verifier should accept "process" / "always-block" -style kinds in HIR-HW's namespace).
- Don't conflate `Vector<T, N>` SIMD semantics with bit-vector semantics. `Vector<f32, 4>` is SIMD; `Signed<33>` is a bit-precise integer. Both in the lattice; semantically distinct.

---

## 3. Open questions (deferred)

| # | Question |
|---|----------|
| 1 | Single HIR-HW or per-target (VHDL HIR vs Verilog HIR vs SystemVerilog HIR)? |
| 2 | Do we synthesize down to a netlist (gates) or stop at RTL (signal assignments)? |
| 3 | FPGA vs ASIC distinctions — relevant to our scope? |
| 4 | Hardware verification (testbenches, assertions, formal properties)? |
| 5 | Mixed software / hardware projects (one part HIR, another HIR-HW)? |

All deferred until the plan opens.

---

## 4. Sequencing

Not sequenced. Reserved.
