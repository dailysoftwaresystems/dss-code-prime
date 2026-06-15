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
| Predecessors | ⏳ Full C ([`plan-23`](./23-full-c-plan%20-%20tbd.md)) · ⏳ the v2 gap ([`v2-gap-catalog`](./v2-gap-catalog%20-%20tbd.md)) · ⏳ at least one language per paradigm compiled (static-OOP **C++**, managed/GC **C#/Java**, dynamic **Python/JS**, ownership **Rust**) · ⏳ runtime substrate ([`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md)) · ✅ HIR pivot ([`plan-09`](./09-hir-plan%20-%20ok.md)) · ⏳ FFI ([`plan-11`](./11-ffi-plan%20-%20tbd.md)) · ⏳ source-to-source ([`plan-10`](./10-source-translation-plan%20-%20tbd.md)). |
| Successors | **Self-hosting** — the C++ engine transpiles into DSS Axis via [`plan-10`](./10-source-translation-plan%20-%20tbd.md) under the bit-identical HIR oracle ([`ZZ-final-goal`](./ZZ-final-goal.md) §6). Then **database in DSS Axis**, then **OS in DSS Axis** ([`ZZ-final-goal`](./ZZ-final-goal.md) §7). |
| Scope | **The language**: vocabulary + semantics + the async-first **native** runtime *contract* + the stdlib API + the feature manifest in §3. Runtime **implementation** (GC algorithm, unwinder, scheduler, threading) is deferred to [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md); FFI **machinery** to [`plan-11`](./11-ffi-plan%20-%20tbd.md). DSS Axis is "just another `.lang.json`" — **zero new engine C++** (Decision #4). |
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
- **2.2 Async-first, `Task<T>`-shaped.** `async` / `await` / `yield` are the default execution model, not a bolt-on: async constructors, async DI, and the async GC all participate in one scheduler. **The async unit is `Task<T>`** (and `Task` for no result) — the C#-style Task-based async pattern (TAP): every `async` function, constructor, and DI resolution returns a `Task<T>`, and `await` unwraps it. `Task<T>` is the single async currency across the language — the value the scheduler schedules and the async GC collects against. Sequential code is the special case, not the reverse.
- **2.3 The async-GC moat.** A garbage collector that participates in the **same scheduler as user `async` tasks** — collection is *itself* an async task, with no dedicated stop-the-world GC thread. Named by [`ZZ-final-goal`](./ZZ-final-goal.md) §4 as *"the moat for the DSS Code Prime language."* The **runtime implementation** is [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md); the **language-level contract** (what `async`/allocation/safepoints mean to the programmer) is owned here.
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
- **Async GC** — §2.3; contract here, impl in [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md). (`D-AXIS-ASYNC-GC-LANGUAGE-CONTRACT`.)
- Structured concurrency, actors, channels ([`ZZ-final-goal`](./ZZ-final-goal.md) §4).

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
- Native GC — async GC ([`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) impl).
- **Native-floor / adaptive-ceiling** — profiler + owned-compiler runtime recompiler in the base service (§2.4). (`D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING`.)
- Closed-world reflection native; open-world via optional **interpret-libs** (§2.5). (`D-AXIS-OPEN-WORLD-INTERPRET-LIBS`.)

### 3.5 FFI — import / export (first-class)
- **Import** — consume C / C++ / OS-supplied libraries; language-side `import` / `extern` syntax here, machinery in [`plan-11`](./11-ffi-plan%20-%20tbd.md) (binary readers, header parser, ABI catalog, mangling). (`D-AXIS-FFI-IMPORT`.)
- **Export** — DSS Axis libraries callable **natively from other languages** — the cross-language native-libs vision: a lib written in Axis consumed by a C, C#, or Python program, no VM, no marshalling VM boundary. (`D-AXIS-FFI-EXPORT`.)
- Hermetic throughout — no external runtime dependency; extern decls + shipped descriptors, same machinery the shipped languages use.

### 3.6 Targets — same-source everywhere
- **Native** — 3 OS × 2+ arch via the engine backend ([`plan-12`](./12-mir-lir-plan%20-%20ok.md) / [`13`](./13-assembler-plan%20-%20tbd.md) / [`14`](./14-linker-plan%20-%20tbd.md)).
- **GPU** — same-source CPU + GPU functions ([`plan-17`](./17-shader-gpu-plan%20-%20tbd.md)); syntax here, SPIR-V codegen there.
- **WASM** — [`plan-18`](./18-wasm-plan%20-%20tbd.md).
- **Transpile** — to any configured language via source-to-source ([`plan-10`](./10-source-translation-plan%20-%20tbd.md)).

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

**Async-GC coherence.** Because the async forms yield at scheduler safepoints, the async GC (itself a scheduler task, §2.3) composes with contended waits: parked-but-cooperative work doesn't stall collection, and collection doesn't stall the toolkit. The parallelism toolkit and the async-GC moat are designed to **share one scheduler, not fight over a thread.**

---

## 4. Architectural locks (the engine must NOT foreclose these)

Guardrails the v2/engine work must honour so DSS Axis stays future-open (the [`plan-20`](./20-custom-language-reserved-plan%20-%20tbd.md) §2.2 discipline, extended):

1. **No VM assumption anywhere.** The runtime contract stays *library-linkable*, never VM-hosted. The HIR/MIR runtime intrinsics (`GcRoot`/`GcSafepoint`/`GcBarrier`, effect/exception markers) must remain emittable into a self-contained native binary.
2. **Pluggable runtime models.** The HIR must keep GC, exceptions, async, and effects as **attribute families**, never assuming one model — so Axis's async-GC + effect system slot in beside C's no-GC and Rust's ownership without engine branching.
3. **Native, closed-world reflection.** Reflection metadata must be emittable into the binary and readable by a runtime *library* — no JIT/VM required for the closed-world surface.
4. **Embeddable compiler.** The owned compiler must be invocable *as a library* from a running program, so the base-service profiler can drive runtime recompilation (the adaptive ceiling). (`D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING`.)
5. **FFI export.** The engine's symbol/ABI/export machinery must support emitting DSS Axis libraries with stable, callable-from-other-languages interfaces. (`D-AXIS-FFI-EXPORT`.)
6. **Agnosticism (Decision #4).** DSS Axis is a `.lang.json` + lowering config — **zero per-language engine C++**. The day the engine branches on the language name is the day this plan's substrate cracks.
7. **`Task<T>` is the awaitable type.** The HIR async lowering must target `Task<T>` / `Task` as a *real type* in the type system (C#-style TAP) — so async functions, constructors, and DI all share one awaitable currency that `await`, the scheduler, and the async GC compose around. The engine's async-attribute lowering must stay neutral enough to also model other languages' awaitables (JS `Promise`, Rust `Future`, C++ coroutine `task`), with `Task<T>` as DSS Axis's choice — not a hardcoded engine assumption. (`D-AXIS-ASYNC-TASK-SHAPE`.)
8. **Parallelism lowers as a closed intrinsic vocabulary.** Atomic ops, memory-ordering fences, and safepoints must live in the HIR/MIR as a *closed intrinsic set* ([`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) §2.4 / [`plan-12`](./12-mir-lir-plan%20-%20ok.md) `GcSafepoint`), so the §3.8 toolkit lowers config-driven to ISA atomics — never per-language concurrency C++ (Decision #4).

---

## 5. Open questions (deferred until triggered)

| # | Question |
|---|----------|
| 1 | Typing: static with full inference (default lean — native + "easy as Node")? Gradual? Any dynamic surface beyond the interpret-libs tail? |
| 2 | Memory model: async GC is committed — but the algorithm (concurrent mark / region / hybrid) is [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md)'s. Any value types / arenas / opt-out for hot paths? |
| 3 | Generics: monomorphised vs reified default; how the per-target choice is expressed. |
| 4 | Exact syntax — the concrete grammar that delivers "easy as Node" with native semantics. |
| 5 | Where the closed-world ↔ open-world (interpret-libs) boundary is drawn, and how a program opts into the dynamic tail. |
| 6 | Module / package system + registry; how `import`/`export` map to it. |
| 7 | Self-hosting timeline — when (and in what order) the C++ engine transpiles into DSS Axis under the bit-identical HIR oracle ([`ZZ-final-goal`](./ZZ-final-goal.md) §6). |

All deferred until §1 trigger conditions are met.

---

## 6. Deferred anchors (owned by this plan; register when it opens)

These **24** `D-AXIS-*` anchors are **reserved/future** — they live here until the plan opens, then move into [`_deferred-anchor-registry`](./_deferred-anchor-registry.md) as active rows. (Reserved-plan anchors are not yet in `src/`, so the CI anchor-guard does not require registry rows today.)

| Anchor | Owns |
|--------|------|
| `D-AXIS-ASYNC-CONSTRUCTORS` | `async new(...)` construction-that-awaits |
| `D-AXIS-ASYNC-DI` | language-side async dependency injection API |
| `D-AXIS-ASYNC-GC-LANGUAGE-CONTRACT` | the programmer-facing async-GC contract (impl → [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md)) |
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
