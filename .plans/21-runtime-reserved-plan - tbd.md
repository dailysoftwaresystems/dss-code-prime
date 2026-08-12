# Runtime — Reserved Sub-Plan (21)

> **Reserved scope.** Owns the **language runtime**: GC, exception unwinder, coroutines, threading primitives. Distinct from the **OS-supplied runtime libs** (libc.so / libSystem.dylib / msvcrt.dll) which are consumed via [FFI](./11-ffi-plan%20-%20tbd.md).
>
> v1 deliberately ships languages with **no runtime** (toy / c-subset / tsql-subset all work via FFI to libc). When the [custom language](./20-custom-language-reserved-plan%20-%20tbd.md) or a full C# / Java port arrives, we'll need our own runtime. This plan reserves the namespace.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🔒 **reserved.** Triggered when a shipped language declares "needs GC" or "needs exceptions" or "needs coroutines." |
| Predecessors  | ⏳ v1 ship. |
| Successors    | First-class consumer is [`20-custom-language-reserved-plan`](./20-custom-language-reserved-plan%20-%20tbd.md). |
| Scope         | **Unspecified.** Three independent modules (GC / exceptions / coroutines), each with its own design when triggered. |

---

## 1. Trigger

This plan opens module-by-module:

- **GC** module: triggered when the first GC-managed language onboards — ⚠ **C#/Java/Python/JS, NOT DSS Axis** (§2.1: Axis renounced GC for proven-static reclamation, [`plan-09.5`](./09.5-dss-hir-plan.md) §4).
- **Exception unwinder**: triggered when the first throwing language onboards (full C++ with exceptions, C#, Java). C-subset omits exceptions by design.
- **Coroutines / async**: triggered when the first language with coroutines onboards.
- **Threading + memory model**: triggered when shared-memory concurrency is in scope (TLS already in [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) LK5; atomics + fences + happens-before relationships live here).

---

## 2. What it owns (when triggered)

### 2.1 GC — ⚠ **NO LONGER DSS AXIS'S PATH** (2026-08-12)

★ **DSS Axis renounced garbage collection outright.** It uses **proven-static reclamation** — the T0–T4 mechanism lattice and the C0–C3 cycle lattice owned by [`plan-09.5`](./09.5-dss-hir-plan.md) §4 — with **no GC, no cycle collector, and no deferred reclamation, ever**. So this module is **no longer triggered by [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md)**, and the algorithm choice below is not a decision Axis is waiting on.

⚠ **This module is NOT retired, and the distinction matters.** [`ZZ-final-goal`](./ZZ-final-goal.md) §3 commits the engine to compiling **C#, Java, Python and JS** — all of which require a real collector. **GC left the language; it did not leave the engine** ([`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §4.2, `D-AXIS-ENGINE-KEEPS-GC-CAPABILITY`). The trigger simply moves: this module now opens for **the first GC-bearing language the engine onboards**, not for Axis.

When it does open, it still owns:

- Algorithm choice: mark-and-sweep / generational / refcount / hybrid.
- Per [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) MIR `GcRoot` / `GcSafepoint` / `GcBarrier` intrinsics — reserved space in the MIR opcode table; full impl here. ⚠ With Axis no longer a consumer, whether those reserved slots are **kept for the managed languages, repurposed** (regions/arenas could use safepoint-shaped markers) **or retired** is open — [`plan-09.5`](./09.5-dss-hir-plan.md) §8 Q7.
- Stack walking (interaction with debug-info CFI).
- Object-header layout (vtable pointer, class id, mark bit, generation).

**What replaced it for Axis is not here.** The ownership/escape/borrow/region analysis is a **compile-time pass over the value graph**, so it lives in [`plan-09.5`](./09.5-dss-hir-plan.md), not in a runtime plan — it links no code and needs no runtime service, which is precisely what makes Axis expressible in drivers and boot images ([`plan-28`](./28-driver-builder-plan.md)).

### 2.2 Exception unwinder

- Itanium ABI two-phase unwind (Linux + macOS).
- Windows SEH (already partially scoped via [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md) DB8 `.pdata` / `.xdata`; runtime side here).
- Landing pad emission in MIR / LIR.
- `throw` / `catch` / `finally` semantics.

### 2.3 Coroutines / async

- Stackful (fibers) vs stackless (state machines).
- Compiler transformation: HIR → MIR pass that splits coroutine bodies into state-machine chunks.
- Runtime support: scheduler, completion notification, cancellation.

### 2.4 Threading + memory model

- Memory ordering (acquire / release / seq_cst).
- Atomic primitives lowered to ISA atomics (x86_64 `LOCK XCHG`; ARM64 `LDXR` / `STXR`).
- Mutex / condvar primitives — FFI to OS-supplied pthread / Win32 (per [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md)) OR in-tree implementations atop futex / WaitOnAddress.
- **Impl substrate for [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §3.8** — DSS Axis's parallelism & synchronization toolkit (interlocked, semaphores, locks, barriers, channels, concurrent collections) is the *language-level API*; **this section owns the low-level primitives it lowers to** (atomics, fences, raw mutex/condvar). Toolkit families carry the `D-AXIS-PAR-*` anchors. The async forms compose with §2.3's scheduler — ⚠ *and no longer with an async GC: there is no collector (§2.1). The open concurrency question is now **ownership across threads**, [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §3.8.* ★ The **scheduler itself** is now the `async` service of [`plan-09.5`](./09.5-dss-hir-plan.md) §3; this section owns the **primitives it lowers to**, unchanged.

---

## 3. Hermetic invariant still applies

The runtime is **part of our compiler**, not a tool we shell out to. GC code is C++ inside `src/runtime/`; exception unwinder is similar; coroutine state machines are generated by our MIR pass.

Open question (deferred): does the runtime ship as a separate static lib that user programs link, or is it inlined per-program at codegen time? Default if unanswered: **inlined** per-program for v1.x; separate runtime lib reserved post-v1.x.

---

## 4. Sequencing

Not sequenced. Reserved. Module-by-module triggers per §1.
