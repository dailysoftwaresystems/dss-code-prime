# DSS Axis — The DSS Code Prime Language — Reserved Sub-Plan (24)

> **Reserved scope.** Owns the design of **DSS Axis** — DSS Code Prime's *own* language: the empirical distillation of everything the HIR proved universal across every language the engine compiles, plus everything the author has suffered with in existing languages and intends to fix. The north star: **"the most modern and complete language" — async-first, fully native (no VM), hermetic, owned every byte.**
>
> **Naming.** *DSS Axis* is the **language**. *DSS Code Prime* remains the **engine / compiler / project**. This plan establishes "DSS Axis" as the official language name, superseding the placeholder term "the DSS Code Prime language" used in [`ZZ-final-goal`](./ZZ-final-goal.md).
>
> **Supersedes [`plan-20`](./20-custom-language-reserved-plan%20-%20tbd.md)** (the thin early reservation of the same language). plan-20 is kept as a historical breadcrumb and points here.
>
> **Sequencing.** Executed **after the v2 gap** ([`v2-gap-catalog`](./v2-gap-catalog%20-%20tbd.md)) and after the engine has compiled enough *diverse paradigms* that the design is empirically grounded rather than theorised. See §1.

## 0. Status (snapshot)

| | |
|---|---|
| Status | 🔒 **reserved.** No design work begins until §1 triggers. The language **will** overfit if designed before the engine has compiled diverse paradigms (see [`ZZ-final-goal`](./ZZ-final-goal.md) §8 risk #6). |
| Predecessors | ⏳ Full C ([`plan-23`](./23-full-c-plan%20-%20tbd.md)) · ⏳ the v2 gap ([`v2-gap-catalog`](./v2-gap-catalog%20-%20tbd.md)) · ⏳ at least one language per paradigm compiled (static-OOP **C++**, managed/GC **C#/Java**, dynamic **Python/JS**, ownership **Rust**) · ⏳ runtime substrate ([`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md)) · ⏳ **DSS-HIR ([`plan-09.5`](./09.5-dss-hir-plan.md)) — the reclamation lattices + the Native Runtime Axis consumes** · ✅ HIR pivot ([`plan-09`](./09-hir-plan%20-%20ok.md)) · ⏳ FFI ([`plan-11`](./11-ffi-plan%20-%20tbd.md)) · ⏳ source-to-source ([`plan-10`](./10-source-translation-plan%20-%20tbd.md)). |
| Successors | **Self-hosting** — the C++ engine transpiles into DSS Axis via [`plan-10`](./10-source-translation-plan%20-%20tbd.md) under the bit-identical HIR oracle ([`ZZ-final-goal`](./ZZ-final-goal.md) §6). Then **database in DSS Axis**, then **OS in DSS Axis** ([`ZZ-final-goal`](./ZZ-final-goal.md) §7). |
| Scope | **The language**: vocabulary + semantics + the async-first **native** runtime *contract* + the **memory/runtime config blocks** (§3.1a) + **annotations incl. `@manual`** (§3.1b) + the stdlib API + the feature manifest in §3. The **reclamation analysis and the Native Runtime services** are [`plan-09.5`](./09.5-dss-hir-plan.md)'s (they are language-agnostic); low-level runtime **primitives** (unwinder, scheduler, threading, atomics) are [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md)'s; FFI **machinery** is [`plan-11`](./11-ffi-plan%20-%20tbd.md)'s. ⚠ *"GC algorithm" was listed here and is now struck: there is no collector to choose an algorithm for (§2.3).* DSS Axis is "just another `.lang.json`" — **zero new engine C++** (Decision #4). |
| Supersedes | [`plan-20`](./20-custom-language-reserved-plan%20-%20tbd.md) — same language, thinner/earlier reservation. |
| Mapped from elsewhere | [`ZZ-final-goal`](./ZZ-final-goal.md) §3 (vocabulary growth) + §4 (built-in capabilities) + §5 (unified ecosystem) + §6 (self-hosting). DSS Axis is the **language output** of those decisions — informed by the engine's experience, not the driver of it. |

---

## 1. Trigger

This plan opens when **both** hold:

1. **The v2 gap is closed** and full C ([`plan-23`](./23-full-c-plan%20-%20tbd.md)) has shipped.
2. **The engine has compiled enough diverse paradigms to ground the design empirically** — minimally: imperative (**C**, in hand), static-typed OOP (**C++**), managed/GC (**C# or Java**), dynamic (**Python or JS**), and ownership/affine (**Rust**). The universal-engine thesis is *validated by a handful of diverse paradigms*; DSS Axis is then the distillation of what the HIR proved universal across them.

Until then, **design is explicitly deferred.** A language authored before this evidence exists overfits to "imperative + static" and misses the dynamic, managed, and ownership concerns that make a modern language complete. This is the standing discipline of [`ZZ-final-goal`](./ZZ-final-goal.md) §6 (design *from* HIR experience, not in a vacuum) and §8 risk #6 (overfitting).

---

## 2. The thesis — what makes DSS Axis different

The design north star is one sentence: **everything the author suffers with in existing languages, fixed — with the ergonomics of Node and the speed and ownership of native.**

- **2.1 Native world, not runtime world.** AOT-native. **No VM, ever.** GC and reflection ship as *linked runtime libraries* compiled into the binary (the Go / Swift model), never as a managed virtual machine. "No VM" is the claim; "we build the runtime ourselves" is the other half of the sentence — both are load-bearing.
- **2.2 Async-first, `Task<T>`-shaped.** `async` / `await` / `yield` are the default execution model, not a bolt-on: async constructors and async DI all participate in one scheduler. **The async unit is `Task<T>`** (and `Task` for no result) — the C#-style Task-based async pattern (TAP): every `async` function, constructor, and DI resolution returns a `Task<T>`, and `await` unwraps it. `Task<T>` is the single async currency across the language — the value the scheduler schedules. Sequential code is the special case, not the reverse. **The scheduler itself is the `async` service of [`plan-09.5`](./09.5-dss-hir-plan.md) DSS-HIR**, which Axis consumes rather than defines.
- **2.3 ★ The no-GC moat — proven-static reclamation.** *(Supersedes the async-GC thesis in full, 2026-08-12. The design it replaces is recorded rather than deleted, because the replacement is only legible against it: async GC was **"a garbage collector that participates in the same scheduler as user `async` tasks — collection is itself an async task, with no dedicated stop-the-world GC thread,"** named by [`ZZ-final-goal`](./ZZ-final-goal.md) §4 as "the moat for the DSS Code Prime language.")*

  **There is no garbage collector, no cycle collector, and no deferred reclamation — ever.** Memory is reclaimed at program points the compiler chose and can name, through the **T0–T4 mechanism lattice** and the **C0–C3 cycle lattice** owned by [`plan-09.5`](./09.5-dss-hir-plan.md) §4. Where the compiler cannot prove a program safe, it **refuses at compile time** rather than leaking at run time.

  ★ **Why this is a stronger moat than the GC was.** Async GC was a better *implementation* of a category everyone else already occupies. Proven-static reclamation with **cycles decided at the type level** is a different category: it is strictly stronger than **Rust** (whose `Rc` cycles leak, silently, and are declared *safe*), stronger than **Swift** (which leaks without `weak`/`unowned` annotations), and unlike **Koka/Lean** it *checks* acyclicity rather than assuming it from an inductive-only data model. And it costs nothing at run time, which is what makes DSS Axis expressible in a kernel driver or a boot image ([`plan-28`](./28-driver-builder-plan.md)) — something no GC-bearing language can claim.

  The **language-level contract** (what ownership, borrowing, `weak` and `@manual` mean to the programmer) is owned here; **the analysis is [`plan-09.5`](./09.5-dss-hir-plan.md)'s**, because ownership is a property of the value graph, not of syntax.
- **2.4 Native-floor, adaptive-ceiling.** AOT-fast startup, low memory, flat latency **by default**. The base service (§3.7) hosts a shared **profiler** plus the **owned compiler embedded as a library**, so a long-lived service can profile itself and **recompile its hot paths live** — JIT-class adaptivity **without a VM**, because the optimiser is *our own native compiler invoked as a service component*, not an interpreter under the core. Adaptivity is an **opt-in ceiling on a native floor**, not an always-on tax. (`D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING`.)
- **2.5 Closed-world reflection native; open-world via interpret-libs.** Full reflection — RTTI, runtime dispatch, field access — is compiled **native** (compiler emits metadata, a runtime library reads it). The genuinely dynamic tail (`eval`, runtime class generation, hot codegen) is served by **optional interpret-libs** — a named *future job*, a library you add, **never a runtime under the core**. (`D-AXIS-CLOSED-WORLD-REFLECTION`, `D-AXIS-OPEN-WORLD-INTERPRET-LIBS`.)
- **2.6 Hermetic, config-driven, owned every byte.** DSS Axis is "just another `.lang.json`" (per [`plan-20`](./20-custom-language-reserved-plan%20-%20tbd.md) — no new engine work), lowering through the HIR pivot ([`plan-09`](./09-hir-plan%20-%20ok.md)) to every target. The entire stack from source to silicon is owned — the "build it without copying the giants" thesis, extended one layer up into the language itself.
- **2.7 Easy as Node.** Ergonomics goal, first-class: as easy to **use, assign, construct, and destruct** as JS/Node — with native safety and speed underneath. Everything good in Dart, C#, and Java, without the ceremony. (`D-AXIS-EASY-AS-NODE-ERGONOMICS`.)

---

## 3. Feature manifest (committed *direction*; detailed design deferred)

Everything below is **committed as direction**. Exact syntax and semantics land when §1 triggers. Items already promised by [`ZZ-final-goal`](./ZZ-final-goal.md) are cross-referenced; items genuinely new to this plan carry a `D-AXIS-*` anchor (§6).

### 3.1 Async & concurrency (async-first)
- `async` / `await` / `yield` — `async` fns return **`Task<T>`** / `Task`; `await` unwraps a `Task<T>`; `yield` produces an async stream whose elements are awaitable. State-machine rewriting at HIR level; scheduler in [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) ([`ZZ-final-goal`](./ZZ-final-goal.md) §4). (`D-AXIS-ASYNC-TASK-SHAPE`.)
- **Async constructors** — `async new(...) → `**`Task<Self>`**: construction that awaits. (`D-AXIS-ASYNC-CONSTRUCTORS`.)
- **Async DI** — dependency injection that participates in the async scheduler; **resolution returns `Task<T>`**, async-scoped lifetimes ([`ZZ-final-goal`](./ZZ-final-goal.md) §5 + language-side API here). (`D-AXIS-ASYNC-DI`.)
- Structured concurrency, actors, channels ([`ZZ-final-goal`](./ZZ-final-goal.md) §4).

### 3.1a Memory & runtime — the two language-config blocks

Axis declares two **independent** blocks. Their independence is load-bearing: `memory` is a **compile-time pass** with no runtime service, `runtime` links real code. `memory` on + `runtime` off is the **freestanding profile** [`plan-28`](./28-driver-builder-plan.md) needs for drivers and boot images.

```jsonc
"memory": {
  // ★ NOT a boolean. A boolean would carry five properties at once — which
  // mechanisms are permitted, their precedence, what happens at the residual,
  // whether the residual diagnoses or falls back, and whether the per-value
  // classification is observable — and each would arrive later as its own key.
  // Declared as a permitted-tier LATTICE instead. See plan-09.5 §4.2/§4.3.
  "lattice": ["T0", "T1", "T2", "T3", "T4"],   // T5 does not exist
  "cycles":  ["C0", "C1", "C2", "C3"]          // C4 does not exist — refuse
},
"runtime": {
  "enable": true      // the DSS Native Runtime — plan-09.5 §3
}
```

⚠ **`autoCollectMemory` as a boolean is deliberately not the spelling.** **MEASURED 2026-08-12**: the key does not exist anywhere in this tree, so nothing is being migrated — the lattice is the first spelling, not a replacement for a shipped one. (`D-AXIS-MEMORY-LATTICE-CONFIG`, `D-AXIS-RUNTIME-ENABLE-CONFIG`.)

### 3.1b Annotations — and `@manual`, the declared escape from the lattice

**DSS Axis has annotations, and they take parameters.** `@name` / `@name(arg, key: value)` on declarations, members, and statement blocks. They are a first-class surface, not attributes bolted on: the lattice escape below, effect declarations (§3.3), and the driver/boot execution-context contract ([`plan-28`](./28-driver-builder-plan.md) §3.2) all ride them. (`D-AXIS-ANNOTATIONS-WITH-PARAMETERS`.)

**`@manual` — programmer-owned memory.** Suspends automatic reclamation for the values it governs.

```
@manual class DmaRegion { … }      // every value of this type is programmer-owned
@manual fn buildPipeline() { … }   // every allocation in this body
@manual { … }                      // just this block
```

- Inside a `@manual` scope, tiers T0–T4 are **suspended**; the programmer calls `free`.
- ★ **`free(x)` is modelled as a MOVE that consumes `x`** — so *"once freed it cannot be used again unless reassigned"* falls straight out of the T1 unique-ownership machinery, with **no new analysis**: use-after-free and double-free become the *same diagnostic* as use-after-move, and reassignment naturally revives the binding. (`D-AXIS-MANUAL-ANNOTATION` · analysis → [`plan-09.5`](./09.5-dss-hir-plan.md) `D-DSSHIR-FREE-IS-A-CONSUMING-MOVE`.)
- A `@manual` value that **provably does not escape and is never freed** is a provable leak and is **refused**. One that escapes is the programmer's declared responsibility — the escape hatch, and where the proof ends.
- `@manual` appears in the tier-classification artifact ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.5), so "how much of this program is hand-managed" is a **measured number**, not a feeling.

★ **Spelling: `@manual`, deliberately NOT `@kernel`.** **MEASURED 2026-08-12** — *kernel* is already a three-way collision in this repo: [`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) uses it **18×** for **GPU compute kernels** (kernel-param address space, kernarg segment, per-kernel register budget), and [`plan-28`](./28-driver-builder-plan.md) uses it for **OS kernels** and again for **boot payload kernels**. A fourth meaning in the memory model would make the word unreadable. `@manualFree` was also considered and rejected: it names only the `free` half, while the annotation's actual effect is suspending T0–T4. Since annotations take parameters, `@manual(...)` stays open for refinement without a rename.

⚠ **`@manual` is ORTHOGONAL to freestanding, and the `@kernel` spelling actively invited conflating them.** A driver does **not** need `@manual` — T0–T3 are compile-time and work fine in a kernel (§3.1a). `@manual` is for the genuinely un-inferable case, e.g. a DMA buffer whose lifetime the hardware owns.

### 3.2 Ergonomics & syntax sugar ("easy as Node")
- **Lambda operations** — closures, first-class functions, concise lambda syntax.
- **Destructuring + spread** — bind-destructure and `...` spread for arrays/objects/params. (`D-AXIS-DESTRUCTURE-SPREAD`.)
- **Collection operators** — array append with `+`, spread with `...`, friendly map/set/list operators. (`D-AXIS-COLLECTION-OPERATORS`.)
- **Easy clone / deep-clone** — built-in shallow + deep clone, auto-derivable; no boilerplate. (`D-AXIS-CLONE-DEEPCLONE`.)
- **Dynamic constructors** — runtime selection/dispatch of constructors (reflection-backed). (`D-AXIS-DYNAMIC-CONSTRUCTORS`.)
- **Pattern matching** — exhaustive ADT matching, destructuring, guards ([`ZZ-final-goal`](./ZZ-final-goal.md) §4).

### 3.3 Type system & metaprogramming
- **Comprehensive full reflection** — RTTI, runtime method dispatch, runtime field access; **closed-world, native** (§2.5) ([`ZZ-final-goal`](./ZZ-final-goal.md) §4).
- **Generics** — both monomorphised (C++/Rust style) and reified (C#/Java style), picked per target ([`ZZ-final-goal`](./ZZ-final-goal.md) §4).
- **Effect system** — pure / IO / async / throws / allocates contracts ([`ZZ-final-goal`](./ZZ-final-goal.md) §4).
- **Exceptions** — first-class, multiple lowerings via the HIR effect system; unwinder in [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) ([`ZZ-final-goal`](./ZZ-final-goal.md) §4).
- **"Everything Dart / C# / Java has"** — the full modern-typed OOP surface (properties, interfaces, records, enums-with-data, nullable types, extension methods), without the ceremony. (`D-AXIS-MANAGED-LANG-PARITY`.)

### 3.4 Runtime model (native, no VM)
- AOT-native, **no VM** (§2.1).
- **No GC of any kind** — proven-static reclamation (§2.3), lattices in [`plan-09.5`](./09.5-dss-hir-plan.md) §4.
- **The DSS Native Runtime** ([`plan-09.5`](./09.5-dss-hir-plan.md) §3) supplies threads / async / filesystem / sockets / timers / synchronization / workers / gui, absorbing platform discrepancies — opt-in via `runtime.enable` (§3.1a).
- **Native-floor / adaptive-ceiling** — profiler + owned-compiler runtime recompiler in the base service (§2.4). (`D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING`.)
- Closed-world reflection native; open-world via optional **interpret-libs** (§2.5). (`D-AXIS-OPEN-WORLD-INTERPRET-LIBS`.)

### 3.5 FFI — import / export (first-class)
- **Import** — consume C / C++ / OS-supplied libraries; language-side `import` / `extern` syntax here, machinery in [`plan-11`](./11-ffi-plan%20-%20tbd.md) (binary readers, header parser, ABI catalog, mangling). (`D-AXIS-FFI-IMPORT`.)
- **Export** — DSS Axis libraries callable **natively from other languages** — the cross-language native-libs vision: a lib written in Axis consumed by a C, C#, or Python program, no VM, no marshalling VM boundary. (`D-AXIS-FFI-EXPORT`.)
- Hermetic throughout — no external runtime dependency; extern decls + shipped descriptors, same machinery the shipped languages use.

### 3.6 Targets — same-source everywhere
- **Native** — 3 OS × 2+ arch via the engine backend ([`plan-12`](./12-mir-lir-plan%20-%20ok.md) / [`13`](./13-assembler-plan%20-%20tbd.md) / [`14`](./14-linker-plan%20-%20tbd.md)).
- **GPU** — same-source CPU + GPU functions ([`plan-17`](./17-shader-gpu-plan%20-%20tbd.md)); syntax here (§3.6a), SPIR-V codegen there.
- **WASM** — [`plan-18`](./18-wasm-plan%20-%20tbd.md).
- **Transpile** — to any configured language via source-to-source ([`plan-10`](./10-source-translation-plan%20-%20tbd.md)).

### 3.6a `gpu` / `gpu?` — execution-target modifiers

Two modifiers, on **methods** and on **bare blocks**:

```
gpu  fn convolve(…) { … }      // MANDATORY GPU — refuses to run without one
gpu  { … }                     // …and as a block

gpu? fn convolve(…) { … }      // GPU IF AVAILABLE — falls back to CPU
gpu? { … }
```

**Spelling: a keyword, not an annotation.** `gpu` changes *what code is legal inside it* (a restricted subset — below), which is the property that makes `async` and Rust's `unsafe` keywords rather than attributes. The trailing `?` reads as "optional" consistently with nullable types in the Dart/C#/TS family §3.3 draws from. ⚠ This is a **second modifier surface** alongside §3.1b's annotations, and two surfaces for one job is a smell worth settling deliberately — §5 Q8. (`D-AXIS-GPU-EXECUTION-MODIFIER`.)

#### What the two actually differ in — **dispatch policy only**

★ **MEASURED 2026-08-12, and it resolves the design cleanly:** [`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) §2.14 makes the CPU sibling **mandatory**, and *not* as a fallback tier — as a **differential oracle**: *"Every GPU-lowered function has, by construction, a host-lowered twin from the same HIR — so any GPU codegen defect is detectable by running both and comparing, on real hardware, without a reference GPU compiler in the loop… an unpaired GPU function is an **unverifiable** GPU function."*

⇒ **`gpu` cannot suppress the CPU sibling.** The twin exists either way, for verification. So the two modifiers emit *the same artifacts* and differ **only in what the dispatcher is permitted to do when no usable GPU is found**:

| | CPU sibling emitted | No GPU at run time |
|---|---|---|
| `gpu` | ✅ yes (oracle) | **refuse** — a named runtime error |
| `gpu?` | ✅ yes (oracle **and** fallback) | run the sibling |

This is a stronger position than it looks: `gpu`'s refusal is a **deliberate policy choice, not a capability limit** — a correct CPU path demonstrably exists and is being declined on purpose.

#### ★ Why `gpu` exists at all: no silent fallback

A silent GPU→CPU fallback is a **10–100× performance cliff that produces correct answers**, which is exactly the failure class this project refuses everywhere else. It is the hardware analogue of §3.1a's lattice sliding T1→T4 unnoticed — and the answer is the same: **the fallback must be declared, not discovered.** `gpu?` declares it; `gpu` refuses it. Neither ever happens quietly. (`D-AXIS-GPU-NO-SILENT-FALLBACK`.)

**Two failure tiers, and the earlier one is preferred:**
- **Build time** — `gpu` (mandatory) targeting a configuration with no SPIR-V path is a **build refusal**, naming the target. `gpu?` on the same target legitimately emits CPU-only.
- **Run time** — device absent or unusable: `gpu` exits with a **named diagnostic identifying the device requirement**, never a quiet downgrade.

#### The restricted subset — and the enforcement already ships

A `gpu` body is a **language subset**: no host heap, no host FFI, restricted recursion, address-space-carrying pointers, a register budget that is a correctness bar rather than a cost ([`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) §: *"a pointer that loses its space is a wrong-memory access, not a slow one"*).

★ **MEASURED**: the enforcement mechanism is **already in the tree** — `H_ShaderViolation = 0xF005` fires on *"a node inside a `ShaderUsable`-flagged function subtree"* ([`src/core/types/parse_diagnostic.hpp:1541`](src/core/types/parse_diagnostic.hpp:1541)), and [`plan-09`](./09-hir-plan%20-%20ok.md) HR6 shipped the shader-restriction subverifier. **`gpu` / `gpu?` are the language surface for a flag the HIR verifier already enforces** — not a new mechanism.

#### ★ `gpu` is a lattice profile (§3.1a), like freestanding

There is no host allocator on a GPU, so **T4 compile-time refcounting is unavailable inside a `gpu` body**; the reachable tiers are **T0** (stack) and **T3** (regions — the natural fit for shared/local memory). A `gpu` block therefore *declares a narrower lattice*, exactly as a driver does with `runtime.enable = false`. Two profiles, one mechanism — and the tier-classification artifact ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.5) reports GPU bodies alongside everything else. `@manual` inside a `gpu` body is a separate question (§5 Q9). (`D-AXIS-GPU-BLOCK-LATTICE-PROFILE`.)

#### ⚠ The honesty clause: `gpu?` does not promise identical results

CPU and GPU floating point differ — FMA contraction, denormal handling, transcendental precision. **A `gpu?` function may return different values depending on which path ran.** That must be written down or it will be assumed away.

★ And it is **not a new question**: [`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) §2.14's differential oracle *compares* the GPU output against the CPU twin, so it already needs an answer to "how equal is equal." **The oracle's comparison tolerance and `gpu?`'s result-equivalence promise are the same question and must have exactly one answer** — a tolerance the oracle accepts but the language does not disclose would be a silent numerical difference shipped to users. (`D-AXIS-GPU-FALLBACK-RESULT-EQUIVALENCE`.)

### 3.7 Standard library — the unified ecosystem
Cross-references [`ZZ-final-goal`](./ZZ-final-goal.md) §5; authored *in DSS Axis itself* with FFI extern decls to OS APIs.
- **Base service** — unified up / down / recover lifecycle, identical as a web service, website, Windows Service, systemd unit, launchd daemon, or Android background service. **One contract, every platform** — and the home of the shared profiler + runtime-recompiler hook (§2.4).
- **Async DI** (§3.1), **collections** (concurrent + persistent first-class), **networking** (socket/HTTP/WS/gRPC), **file manager**, **parallel** (full toolkit → §3.8), **persistence/serialization**, **crypto** (vendored per [`plan-16`](./16-codesign-publish-plan%20-%20tbd.md)), **time/tz**, **logging/tracing/metrics**.

### 3.8 Parallelism & synchronization toolkit

A comprehensive, first-class concurrency toolkit — *"a really good set,"* not a token few. **Dual-API commitment** (`D-AXIS-PAR-DUAL-API`): every primitive that can *wait* ships in **both** a **blocking** form (parks the thread) **and** an **async** form (returns `Task<T>` / `Task`, yields to the scheduler) — co-equal, C#-style (`Monitor`/`lock` + `SemaphoreSlim.WaitAsync`). Atomics/interlocked are lock-free and non-blocking by nature, so they carry no blocking/async split. The **API surface lives here**; the **low-level substrate** (atomics → ISA, memory ordering, raw mutex/condvar, scheduler, cancellation) is [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) §2.3/§2.4.

1. **Atomics & interlocked** — atomic load/store/CAS, `Interlocked` increment / decrement / add / exchange / compare-exchange, atomic flags, explicit memory ordering (relaxed / acquire / release / seq_cst). Lock-free; lowered to ISA atomics by [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) §2.4. (`D-AXIS-PAR-ATOMICS`.)
2. **Locks** — mutex, recursive/reentrant mutex, reader-writer lock, spinlock, scoped `lock` block; blocking + `await lock.acquireAsync()`. (`D-AXIS-PAR-LOCKS`.)
3. **Semaphores** — counting + binary; blocking `acquire()` + async `acquireAsync() → Task`. (`D-AXIS-PAR-SEMAPHORES`.)
4. **Sync primitives** — barrier, countdown latch, condition variable / monitor, auto/manual-reset event, once / lazy-init; blocking + async. (`D-AXIS-PAR-SYNC-PRIMITIVES`.)
5. **Structured concurrency** — task groups / nurseries (await-all, cancel-on-error), `parallel for` / data-parallel map-reduce, channels (bounded/unbounded, MPSC/MPMC), actors — all `Task<T>`-shaped and scheduler-integrated ([`ZZ-final-goal`](./ZZ-final-goal.md) §4/§5). (`D-AXIS-PAR-STRUCTURED-CONCURRENCY`.)
6. **Concurrent & lock-free collections** — concurrent queue / stack / deque / dict / set / bag; lock-free + persistent/immutable variants (composes with §3.7 collections). (`D-AXIS-PAR-CONCURRENT-COLLECTIONS`.)
7. **Cancellation & timeouts** — cooperative cancellation tokens that propagate through task groups, deadlines / timeouts, linked cancellation. (`D-AXIS-PAR-CANCELLATION`.)

**Reclamation coherence.** *(Replaces the former "Async-GC coherence" paragraph — the property it asserted was that collection and contended waits share one scheduler without fighting over a thread. With no collector, that whole class of interaction is gone rather than solved.)* Because reclamation is **compile-time and deterministic** (§2.3), there is no collector to stall and none to be stalled by: a parked task holds no collection back, and nothing pauses a task to collect. ★ The concurrency question that *does* remain is the real one — **ownership across threads**: a value moved to another thread must be proven uniquely owned (T1) or shared through a mechanism the lattice can see. That is [`plan-09.5`](./09.5-dss-hir-plan.md) §4's analysis meeting §2.4's primitives, and it is where this toolkit's soundness is actually decided. (`D-AXIS-PAR-OWNERSHIP-ACROSS-THREADS`.)

---

## 4. Architectural locks (the engine must NOT foreclose these)

Guardrails the v2/engine work must honour so DSS Axis stays future-open (the [`plan-20`](./20-custom-language-reserved-plan%20-%20tbd.md) §2.2 discipline, extended):

1. **No VM assumption anywhere.** The runtime contract stays *library-linkable*, never VM-hosted. The HIR/MIR runtime intrinsics (effect/exception markers; the reserved `GcRoot`/`GcSafepoint`/`GcBarrier` slots — now candidates for retirement or repurposing, [`plan-09.5`](./09.5-dss-hir-plan.md) §8 Q7) must remain emittable into a self-contained native binary.
2. **Pluggable runtime models.** The HIR must keep GC, exceptions, async, and effects as **attribute families**, never assuming one model — so Axis's ownership lattice + effect system slot in beside C's manual model, Rust's ownership, and **a GC-bearing language the engine may yet onboard**. ★ Axis renouncing GC does **not** license the engine to drop GC support: C#, Java, Python and JS all need it, and [`ZZ-final-goal`](./ZZ-final-goal.md) §3 commits to compiling them. **GC leaves the language; it does not leave the engine.** (`D-AXIS-ENGINE-KEEPS-GC-CAPABILITY`.)
2a. **The disabled-tier path stays complete.** Axis consumes [`plan-09.5`](./09.5-dss-hir-plan.md) DSS-HIR, but no engine code may assume a DSS-HIR node exists — a language bringing its own async/ownership model lowers through plain HIR ([`plan-09.5`](./09.5-dss-hir-plan.md) §2.3).
3. **Native, closed-world reflection.** Reflection metadata must be emittable into the binary and readable by a runtime *library* — no JIT/VM required for the closed-world surface.
4. **Embeddable compiler.** The owned compiler must be invocable *as a library* from a running program, so the base-service profiler can drive runtime recompilation (the adaptive ceiling). (`D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING`.)
5. **FFI export.** The engine's symbol/ABI/export machinery must support emitting DSS Axis libraries with stable, callable-from-other-languages interfaces. (`D-AXIS-FFI-EXPORT`.)
6. **Agnosticism (Decision #4).** DSS Axis is a `.lang.json` + lowering config — **zero per-language engine C++**. The day the engine branches on the language name is the day this plan's substrate cracks.
7. **`Task<T>` is the awaitable type.** The async lowering must target `Task<T>` / `Task` as a *real type* in the type system (C#-style TAP) — so async functions, constructors, and DI all share one awaitable currency that `await` and the scheduler compose around. The engine's async-attribute lowering must stay neutral enough to also model other languages' awaitables (JS `Promise`, Rust `Future`, C++ coroutine `task`), with `Task<T>` as DSS Axis's choice — not a hardcoded engine assumption. ★ The lowering now lands in [`plan-09.5`](./09.5-dss-hir-plan.md)'s `async` service rather than directly in HIR; lock 2a keeps the plain-HIR path open for languages that decline it. (`D-AXIS-ASYNC-TASK-SHAPE`.)
8. **Parallelism lowers as a closed intrinsic vocabulary.** Atomic ops, memory-ordering fences, and safepoints must live in the HIR/MIR as a *closed intrinsic set* ([`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) §2.4 / [`plan-12`](./12-mir-lir-plan%20-%20ok.md) `GcSafepoint`), so the §3.8 toolkit lowers config-driven to ISA atomics — never per-language concurrency C++ (Decision #4).

---

## 5. Open questions (deferred until triggered)

| # | Question |
|---|----------|
| 1 | Typing: static with full inference (default lean — native + "easy as Node")? Gradual? Any dynamic surface beyond the interpret-libs tail? |
| 2 | ~~Memory model: async GC~~ **RESOLVED 2026-08-12 — no GC.** Proven-static reclamation (§2.3); lattices, staging and the residual policy are [`plan-09.5`](./09.5-dss-hir-plan.md) §4/§7. What remains open *here* is the **language surface**: the spelling of `weak`, whether borrows are ever written explicitly, and how a region/arena is named in source. |
| 3 | Generics: monomorphised vs reified default; how the per-target choice is expressed. |
| 4 | Exact syntax — the concrete grammar that delivers "easy as Node" with native semantics. |
| 5 | Where the closed-world ↔ open-world (interpret-libs) boundary is drawn, and how a program opts into the dynamic tail. |
| 6 | Module / package system + registry; how `import`/`export` map to it. |
| 7 | Self-hosting timeline — when (and in what order) the C++ engine transpiles into DSS Axis under the bit-identical HIR oracle ([`ZZ-final-goal`](./ZZ-final-goal.md) §6). |
| 8 | ⚠ **Two modifier surfaces.** §3.1b established annotations-with-parameters (`@manual`), §3.6a adds keyword modifiers (`gpu` / `gpu?`). Which jobs belong to which — and is `gpu?` really `@gpu(fallback: true)`? The keyword case rests on `gpu` restricting what is *legal inside* it, like `async`/`unsafe`; `@manual` only changes reclamation. Settle the boundary before either ships. |
| 9 | Inside a `gpu` body: are **address spaces** (global / shared / local / constant) written by the programmer or inferred? "Easy as Node" argues inferred — but [`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) warns a pointer that loses its space is a *wrong-memory access*, so inference must be total or the fallback is a refusal. And what does `@manual` mean inside a `gpu` body — GPU-side allocation, or refused? |
| 10 | Does `gpu` / `gpu?` compose with `async` (§3.1) — is `gpu async fn` a dispatch that awaits its own completion, and is that the natural spelling for a kernel launch? |

All deferred until §1 trigger conditions are met.

---

## 6. Deferred anchors (owned by this plan; register when it opens)

These **33** `D-AXIS-*` anchors are **reserved/future** — they live here until the plan opens, then move into [`_deferred-anchor-registry-production`](./_deferred-anchor-registry-production.md) as active rows. (Reserved-plan anchors are not yet in `src/`, so the CI anchor-guard does not require registry rows today.)

| Anchor | Owns |
|--------|------|
| `D-AXIS-ASYNC-CONSTRUCTORS` | `async new(...)` construction-that-awaits |
| `D-AXIS-ASYNC-DI` | language-side async dependency injection API |
| `D-AXIS-MEMORY-LATTICE-CONFIG` | the `memory` block — a declared tier lattice, never a boolean (§3.1a) |
| `D-AXIS-RUNTIME-ENABLE-CONFIG` | the `runtime` block — opt-in DSS Native Runtime, independent of `memory` (§3.1a) |
| `D-AXIS-ANNOTATIONS-WITH-PARAMETERS` | `@name(...)` on declarations, members and blocks (§3.1b) |
| `D-AXIS-MANUAL-ANNOTATION` | `@manual` — programmer-owned memory; `free` consumes (§3.1b) |
| `D-AXIS-ENGINE-KEEPS-GC-CAPABILITY` | ★ GC leaves the LANGUAGE, not the ENGINE — C#/Java/Python/JS still need it (§4.2) |
| `D-AXIS-PAR-OWNERSHIP-ACROSS-THREADS` | moving a value across threads must be provable under the lattice (§3.8) |
| `D-AXIS-GPU-EXECUTION-MODIFIER` | `gpu` / `gpu?` on methods and blocks; keyword not annotation (§3.6a) |
| `D-AXIS-GPU-NO-SILENT-FALLBACK` | ★ a GPU→CPU downgrade is declared (`gpu?`) or refused (`gpu`), never quiet (§3.6a) |
| `D-AXIS-GPU-BLOCK-LATTICE-PROFILE` | a `gpu` body declares a narrower memory lattice — no T4 without a host allocator (§3.6a) |
| `D-AXIS-GPU-FALLBACK-RESULT-EQUIVALENCE` | ★ `gpu?` FP-equivalence and plan-17's oracle tolerance are ONE question, ONE answer (§3.6a) |
| `D-AXIS-ASYNC-TASK-SHAPE` | `Task<T>` / `Task` as the awaitable type across async fns, constructors, DI (C#-style TAP) |
| `D-AXIS-DESTRUCTURE-SPREAD` | bind-destructure + `...` spread |
| `D-AXIS-COLLECTION-OPERATORS` | array `+` append, spread, friendly collection ops |
| `D-AXIS-CLONE-DEEPCLONE` | built-in / auto-derived shallow + deep clone |
| `D-AXIS-DYNAMIC-CONSTRUCTORS` | runtime constructor selection/dispatch |
| `D-AXIS-MANAGED-LANG-PARITY` | the Dart/C#/Java modern-OOP surface, ceremony-free |
| `D-AXIS-CLOSED-WORLD-REFLECTION` | native (no-VM) closed-world reflection metadata + runtime lib |
| `D-AXIS-OPEN-WORLD-INTERPRET-LIBS` | the optional eval/dynamic-codegen interpret-libs tail |
| `D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING` | base-service profiler + owned-compiler runtime recompiler |
| `D-AXIS-FFI-IMPORT` | language-side `import`/`extern` (machinery → [`plan-11`](./11-ffi-plan%20-%20tbd.md)) |
| `D-AXIS-FFI-EXPORT` | DSS Axis libs callable natively from other languages |
| `D-AXIS-EASY-AS-NODE-ERGONOMICS` | use/assign/construct/destruct fluency goal |
| `D-AXIS-SELF-HOST-TRANSPILE` | C++ engine → DSS Axis via [`plan-10`](./10-source-translation-plan%20-%20tbd.md) under the HIR oracle |
| `D-AXIS-PAR-DUAL-API` | every waiting primitive ships co-equal blocking + async (`Task<T>`) forms |
| `D-AXIS-PAR-ATOMICS` | atomics + interlocked + explicit memory ordering (→ ISA via [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) §2.4) |
| `D-AXIS-PAR-LOCKS` | mutex / recursive / reader-writer / spinlock / scoped lock |
| `D-AXIS-PAR-SEMAPHORES` | counting + binary semaphores (blocking + async) |
| `D-AXIS-PAR-SYNC-PRIMITIVES` | barrier / latch / condvar / event / once / lazy |
| `D-AXIS-PAR-STRUCTURED-CONCURRENCY` | task groups, parallel-for / data-parallel, channels, actors |
| `D-AXIS-PAR-CONCURRENT-COLLECTIONS` | concurrent + lock-free + persistent collections |
| `D-AXIS-PAR-CANCELLATION` | cancellation tokens, deadlines, linked cancellation |

---

## 7. Sequencing

**Not sequenced. Reserved.** No cycles until §1 triggers. When it opens, this file is renamed to ` - ok.md`, gains a `## 0.1 Stepper` with a tier overview + cycle-by-cycle log (the active-plan shape), and the §6 anchors migrate into the registry as work begins.

The order of the language's own arc, when it comes, follows [`ZZ-final-goal`](./ZZ-final-goal.md) §6: **design from HIR experience → author DSS Axis → transpile the C++ engine into it (self-host; `D-AXIS-SELF-HOST-TRANSPILE`) → database → OS.** This plan is the reservation for the first of those steps.
