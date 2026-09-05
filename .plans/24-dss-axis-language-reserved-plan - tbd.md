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
| Scope | **The language**: vocabulary + semantics + the async-first **native** runtime *contract* + the **memory/runtime config blocks** (§3.1a) + **annotations** (§3.1b) + **the borrow contract, static and concurrent** (§3.1c) + **the `isa.*` low-level semantic surface** (§3.6b) + the stdlib API + the feature manifest in §3. The **reclamation analysis and the Native Runtime services** are [`plan-09.5`](./09.5-dss-hir-plan.md)'s (they are language-agnostic); low-level runtime **primitives** (unwinder, scheduler, threading, atomics) are [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md)'s; FFI **machinery** is [`plan-11`](./11-ffi-plan%20-%20tbd.md)'s. ⚠ *"GC algorithm" was listed here and is now struck: there is no collector to choose an algorithm for (§2.3).* ⚠ *"annotations incl. `@manual`" was listed here and is now struck: the escape hatch is deleted from the design (§2.3a).* DSS Axis is "just another `.lang.json`" — **zero new engine C++** (Decision #4). |
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

- **2.1 Native world, not runtime world.** AOT-native. **No VM, ever.** Reflection and the memory-enforcement machinery ship as *linked runtime libraries* compiled into the binary (the Go / Swift model), never as a managed virtual machine. "No VM" is the claim; "we build the runtime ourselves" is the other half of the sentence — both are load-bearing.
- **2.2 Async-first, `Task<T>`-shaped.** `async` / `await` / `yield` are the default execution model, not a bolt-on: async constructors and async DI all participate in one scheduler. **The async unit is `Task<T>`** (and `Task` for no result) — the C#-style Task-based async pattern (TAP): every `async` function, constructor, and DI resolution returns a `Task<T>`, and `await` unwraps it. `Task<T>` is the single async currency across the language — the value the scheduler schedules. Sequential code is the special case, not the reverse. **The scheduler itself is the `async` service of [`plan-09.5`](./09.5-dss-hir-plan.md) DSS-HIR**, which Axis consumes rather than defines.

### 2.3 ★★★ The moat — the COMPILER owns memory, in every case

> **The architectural principle, stated once and without hedging:**
> **The programmer describes semantics. The compiler determines how those semantics are enforced — statically where it can prove them, at run time where it cannot. There is no third option, and there is no way for the programmer to take the wheel.**

*(Supersedes the async-GC thesis in full, 2026-08-12, and supersedes the refuse-the-residual thesis in full, 2026-09-04. Both are recorded rather than deleted, because the replacement is only legible against them — see §2.3a and §2.3b.)*

Memory management in DSS Axis is a **compiler responsibility with two realizations of one model**:

| | when | what is emitted | cost |
|---|---|---|---|
| **Proven statically** | the compiler establishes ownership / borrowing / aliasing / mutation / lifetime / escape / thread boundary from the program text | **nothing** — the proof becomes an optimization fact and the enforcement machinery is *deleted* (§3.1d) | zero |
| **Not proven statically** | the property is real but out of reach of the analysis — characteristically when an object crosses concurrent execution boundaries | the compiler **automatically lowers** the ownership/lifetime semantics into DSS runtime mechanisms **embedded in the native binary** (§3.1c) | the residual, and only at the sites that needed it |

The compiler decides which. **The programmer never selects between them and cannot.**

**The lifetime guarantee, both directions:** an object stays alive for **exactly as long as the program may legitimately require it**, and is reclaimed **as soon as** the compiler or the runtime record can establish that **no valid execution context can reach it any more**. Not later. Not at a convenient moment. Not when a queue is drained.

The mechanism vocabulary — the **T0–T4 static tiers**, the **TD dynamic tier**, and the **C0–C3 cycle lattice** — is owned by [`plan-09.5`](./09.5-dss-hir-plan.md) §4, because ownership is a property of the **value graph**, not of surface syntax. What this plan owns is the **language-level contract**: what the programmer may write, what they are promised, and what they are never asked to do. (`D-AXIS-COMPILER-OWNS-MEMORY`.)

#### ★ This is not garbage collection, and the claim is falsifiable rather than rhetorical

Calling the dynamic tier "GC with extra steps" is the obvious objection, so it is answered by property, not by assertion. Five hold, and **a garbage collector fails every one**:

1. **No traversal.** Nothing walks the heap, ever. There are no runtime type maps, no root sets, no shadow metadata, no mark phase. ★ This is 09.5 §4.3's own decisive argument against a *cycle collector*, and it applies here unchanged — the dynamic tier is a **per-object record**, not a heap survey.
2. **No deferral.** Reclamation happens when the last participant releases — **a program point in the emitted code**, not an entry on a queue drained later.
3. **No background thread, no safepoints, no stop-the-world.** Nothing pauses a task to reclaim, and no parked task holds reclamation back.
4. **Materialized per SITE, not per PROGRAM.** A collector is a property of the runtime — you have one whether or not you allocate. The dynamic tier is a property of *the sites that needed it*: a program whose residual is empty **links none of this machinery at all**, and that is a *measured* number in the tier-classification artifact ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.5), not a hope.
5. **It is removed by PROOF, not tuned by knobs.** A GC gets faster when you configure it. This gets **deleted** when the analysis improves — which is the same lever, pointed the other way, and it is the one the optimizer can pull (§3.1d).

⇒ **"Deterministic, compiler-directed memory management"** is the phrase. *"GC"*, *"collector"*, *"managed memory"* and *"automatic collection"* are all **wrong** about this design and must not be used for it — including in diagnostics, in the tier artifact, and in whatever documentation the language eventually ships. (`D-AXIS-NOT-GARBAGE-COLLECTION-VOCABULARY`.)

#### ⚠ The honesty clause — the ONE refusal that survives, and why it must

A dynamic record can enforce *when* an object dies and *who* may touch it. It **cannot reclaim a cycle** — that requires tracing, and tracing is the thing this design does not have. So the residual split is not uniform, and pretending otherwise would be the design lying about itself:

- **Lifetime, aliasing, mutation, escape and thread-boundary properties** — failure to prove them **never rejects the program**. The compiler materializes enforcement (§3.1c). This is the reversal recorded in §2.3b.
- **Acyclicity** — stays a **static, type-level decision** (C0–C3), and **C4 remains a refusal**, naming the SCC members, the exact field that closes it, and now **three** remedies: borrow the back-edge, declare it weak, or **give the region an owner** (§3.1f) ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.3). ⚠ **NARROWED 2026-09-04**: it fires only on an SCC whose **external in-edges cannot be enumerated** — through FFI, a raw pointer, or foreign-owned storage — which is the same boundary the next bullet names, reached from the other side.

★ **And the surviving refusal is STRUCTURAL, not epistemic** — which is what makes it acceptable in a design whose principle is "failure to prove is not rejection." It never says *"your program may well be fine and we could not prove it."* It says: **this type graph admits a shape that nothing short of a tracing collector can reclaim, and here are the two edits that remove it.** It is decided on the **type** graph, before any value exists, so it is reproducible, explainable, and fixable at the declaration site. (`D-AXIS-RESIDUAL-ENFORCES-NEVER-REFUSES`.)

★ **Second boundary, unchanged and still real: FFI.** A cycle formed through foreign code, through a raw pointer, or through memory a foreign library owns is not in the owning-edge type graph and is **not covered** ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.3's honesty clause). DSS is an FFI-heavy compiler, and *"Axis cannot leak"* will otherwise be repeated without its qualifier.

#### ★ Why this is still a moat — and a wider one than the version it replaces

The superseded thesis was strictly stronger than **Rust** (whose `Rc` cycles leak, silently, and are declared *safe*), stronger than **Swift** (which leaks without `weak`/`unowned`), and unlike **Koka/Lean** it *checks* acyclicity rather than assuming it from an inductive-only data model. **All of that survives** — the cycle lattice is untouched.

What is added is the half those languages answer with an escape hatch: **the un-provable case is now served rather than refused.** Rust serves it with `unsafe` and with `Rc`/`RefCell` the programmer must select; Swift serves it with annotations the programmer must get right; C++ serves it with a smart-pointer type the programmer must choose. **DSS Axis serves it by lowering the semantics the programmer already wrote.** The selection burden that every one of those languages puts on the author is, here, an analysis result.

⇒ the moat is: **zero-cost where provable, automatic where not, deterministic in both, and never the programmer's problem in either.** And because the static half costs nothing at run time, DSS Axis remains expressible in a kernel driver or a boot image ([`plan-28`](./28-driver-builder-plan.md)) — with the profile carve-out §3.1a states plainly.

### 2.3a ⛔ `@manual` is DELETED — recorded, with the argument that retired it

**The prior design (2026-08-12 – 2026-09-04) carried `@manual`:** an annotation on classes, functions and bare blocks that **suspended tiers T0–T4** for the values it governed and handed reclamation to the programmer, with `free(x)` modelled as a consuming move so use-after-free became use-after-move. It is **removed from DSS Axis entirely**, along with every equivalent — there is **no `@manual`, no `@unmanaged`, no `free`, no `delete`, no per-object opt-out of the memory model under any spelling.** (`D-AXIS-NO-MANUAL-ESCAPE-HATCH`.)

★ **It is retired on its merits, not by decree — because the case it was invented for does not need it.** The motivating example was always the same: *a DMA buffer whose lifetime the hardware owns*. Read carefully, that is not a request for manual memory management. It is a **semantic fact about the object**: its storage is owned by an external agent, so the compiler must not reclaim it and must not assume it can. That fact belongs in the **description**, where the compiler can act on it — and the language already has the surface for it, because a value whose storage a foreign agent owns is exactly what the **FFI boundary** (§2.3, honesty clause) describes.

⇒ the un-inferable case is answered by **telling the compiler the truth about the object**, never by **switching the compiler off for it**. One surface, one model, and the "how much of this program is hand-managed" number the old design proudly measured is now **structurally zero**.

⚠ **Three consequences, so nothing is left dangling:**
- **The `@manual` vs `@kernel` naming analysis is moot.** It was a real, MEASURED finding (*kernel* collides three ways in this repo — [`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) for GPU compute kernels, [`plan-28`](./28-driver-builder-plan.md) for OS kernels and again for boot payload kernels) and it stands as a **naming precedent for any future annotation**, but there is no longer an annotation to name.
- **`free`-as-a-consuming-move goes with it.** The insight was elegant and it has nothing left to apply to: with no programmer-callable `free`, use-after-free is not a diagnostic the language can produce, because the construct that produces it does not exist. [`plan-09.5`](./09.5-dss-hir-plan.md) retires `D-DSSHIR-FREE-IS-A-CONSUMING-MOVE` on the same grounds. The underlying **move machinery is untouched** — T1 still consumes on move, and that is what makes the cross-thread transfer in §3.1c checkable.
- **Freestanding is unaffected, and always was.** A driver never needed `@manual`: T0–T3 are compile-time and work in a kernel. The old plan said so in a ⚠ note; the note is now simply true without a caveat. [`plan-28`](./28-driver-builder-plan.md) is updated in the same change.

### 2.3b ⛔ "Refuse the residual" is REVERSED — recorded, with what it cost

**The prior design refused what it could not prove:** *"Where the compiler cannot prove a program safe, it refuses at compile time rather than leaking at run time"* ([`plan-09.5`](./09.5-dss-hir-plan.md) §4), resolving the (i) accept-every-program / (ii) no-collector / (iii) no-leaks trilemma by **giving up total (i)**.

**That is reversed.** The new resolution keeps **(ii)** and **(iii)** and recovers **(i) except for the type-level cycle refusal** — by adding a fourth term the trilemma never contained: **deterministic dynamic enforcement that is neither a collector nor deferred.**

★ **What the old position actually cost, stated plainly, because it is the reason for the change:** a refusal is the compiler telling a programmer that a **valid** program is inexpressible *because the analysis is not strong enough yet*. That makes the language's accepted set a function of **this year's inference quality** — every improvement to the analysis silently changes what compiles, and every programmer meets the analysis's frontier as a wall with no way through. The dynamic tier converts that wall into **a cost that shows up in the tier artifact and shrinks as the analysis improves.** A programmer meets a *slower* program, not an *impossible* one — and the tier artifact tells them exactly where and why (§3.1d).

⇒ **the accepted set of the language stops depending on the strength of the compiler's proofs.** That is the property being bought, and it is worth more than the purity of the old refusal.

- **2.4 Native-floor, adaptive-ceiling.** AOT-fast startup, low memory, flat latency **by default**. The base service (§3.7) hosts a shared **profiler** plus the **owned compiler embedded as a library**, so a long-lived service can profile itself and **recompile its hot paths live** — JIT-class adaptivity **without a VM**, because the optimiser is *our own native compiler invoked as a service component*, not an interpreter under the core. Adaptivity is an **opt-in ceiling on a native floor**, not an always-on tax. ★ And it composes with §2.3 in a way no GC-bearing language can match: a hot path recompiled with better ownership information **loses runtime enforcement it previously carried**. (`D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING`.)
- **2.5 Closed-world reflection native; open-world via interpret-libs.** Full reflection — RTTI, runtime dispatch, field access — is compiled **native** (compiler emits metadata, a runtime library reads it). The genuinely dynamic tail (`eval`, runtime class generation, hot codegen) is served by **optional interpret-libs** — a named *future job*, a library you add, **never a runtime under the core**. (`D-AXIS-CLOSED-WORLD-REFLECTION`, `D-AXIS-OPEN-WORLD-INTERPRET-LIBS`.)
- **2.6 Hermetic, config-driven, owned every byte.** DSS Axis is "just another `.lang.json`" (per [`plan-20`](./20-custom-language-reserved-plan%20-%20tbd.md) — no new engine work), lowering through the HIR pivot ([`plan-09`](./09-hir-plan%20-%20ok.md)) to every target. The entire stack from source to silicon is owned — the "build it without copying the giants" thesis, extended one layer up into the language itself.
- **2.7 Easy as Node.** Ergonomics goal, first-class: as easy to **use, assign, construct, and destruct** as JS/Node — with native safety and speed underneath. Everything good in Dart, C#, and Java, without the ceremony. (`D-AXIS-EASY-AS-NODE-ERGONOMICS`.)
- **2.8 ★ Computational control and memory responsibility are SEPARATE AXES.** The programmer may descend as far toward the machine as they like — ordinary Axis, then `isa.*` machine-level operations (§3.6b) — and **memory management does not descend with them.** Every rung operates on **Axis values**; the compiler owns residence, allocation, lifetime, ownership and reclamation at all of them. This is the principle §6 states in full, and it is what makes "high-level assembly with semantic memory management" a coherent phrase rather than a contradiction. (`D-AXIS-CONTROL-AND-MEMORY-ARE-SEPARATE-AXES`.)

---

## 3. Feature manifest (committed *direction*; detailed design deferred)

Everything below is **committed as direction**. Exact syntax and semantics land when §1 triggers. Items already promised by [`ZZ-final-goal`](./ZZ-final-goal.md) are cross-referenced; items genuinely new to this plan carry a `D-AXIS-*` anchor (§6).

### 3.1 Async & concurrency (async-first)
- `async` / `await` / `yield` — `async` fns return **`Task<T>`** / `Task`; `await` unwraps a `Task<T>`; `yield` produces an async stream whose elements are awaitable. State-machine rewriting at HIR level; scheduler in [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) ([`ZZ-final-goal`](./ZZ-final-goal.md) §4). (`D-AXIS-ASYNC-TASK-SHAPE`.)
- **Async constructors** — `async new(...) → `**`Task<Self>`**: construction that awaits. (`D-AXIS-ASYNC-CONSTRUCTORS`.)
- **Async DI** — dependency injection that participates in the async scheduler; **resolution returns `Task<T>`**, async-scoped lifetimes ([`ZZ-final-goal`](./ZZ-final-goal.md) §5 + language-side API here). (`D-AXIS-ASYNC-DI`.)
- Structured concurrency, actors, channels ([`ZZ-final-goal`](./ZZ-final-goal.md) §4).

### 3.1a Memory & runtime — the two language-config blocks

Axis declares two **independent** blocks. Their independence is load-bearing: the **static** half of `memory` is a compile-time pass with no runtime service, `runtime` links real code. `memory` static-only + `runtime` off is the **freestanding profile** [`plan-28`](./28-driver-builder-plan.md) needs for drivers and boot images.

```jsonc
"memory": {
  // ★ NOT a boolean. A boolean would carry five properties at once — which
  // mechanisms are permitted, their precedence, what happens at the residual,
  // whether the residual diagnoses or falls back, and whether the per-value
  // classification is observable — and each would arrive later as its own key.
  // Declared as a permitted-tier LATTICE instead. See plan-09.5 §4.2/§4.3.
  "lattice": ["T0", "T1", "T2", "T3", "T4", "TD"],  // TD = the dynamic ownership
                                                    // record. T5 does not exist.
  "cycles":  ["C0", "C1", "C2", "C3"]               // C4 does not exist — refuse
},
"runtime": {
  "enable": true      // the DSS Native Runtime — plan-09.5 §3
}
```

⚠ **`autoCollectMemory` as a boolean is deliberately not the spelling.** **MEASURED 2026-08-12**: the key does not exist anywhere in this tree, so nothing is being migrated — the lattice is the first spelling, not a replacement for a shipped one. (`D-AXIS-MEMORY-LATTICE-CONFIG`, `D-AXIS-RUNTIME-ENABLE-CONFIG`.)

#### ⚠ The profile carve-out — where "never refuses" is not free, stated rather than smoothed

**TD needs two things: atomics, and somewhere to put the record.** Atomics are ISA-level and available freestanding ([`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) §2.4). Storage is not automatic: a GPU body has no host allocator (§3.6a), and a boot image has none until it builds one.

⇒ **TD requires a declared storage provider.** `runtime.enable = true` supplies one; a freestanding profile may supply its own. **A profile that omits `TD` from its lattice declares that dynamic enforcement is unavailable there — and in that profile, and only there, an unprovable residual is a build refusal naming the missing provider.** (`D-AXIS-DYNAMIC-TIER-NEEDS-A-STORAGE-PROVIDER`.)

★ **This is not the escape hatch returning under another name, and the difference is exactly the one §2.3 is built on.** A narrowed lattice is a **deployment declaration in config** about what the *machine* can offer. It never hands memory management to the programmer, never appears at a declaration site, and never varies per object. The compiler still owns the decision; it has simply been told, once, which mechanisms exist on this target. The two blocks stay independently switchable ([`plan-09.5`](./09.5-dss-hir-plan.md) lock 3).

### 3.1b Annotations — a first-class surface, with no memory escape in it

**DSS Axis has annotations, and they take parameters.** `@name` / `@name(arg, key: value)` on declarations, members, and statement blocks. They are a first-class surface, not attributes bolted on: effect declarations (§3.3) and the driver/boot execution-context contract ([`plan-28`](./28-driver-builder-plan.md) §3.2) ride them. (`D-AXIS-ANNOTATIONS-WITH-PARAMETERS`.)

⛔ **No annotation suspends, weakens, or opts out of the memory model** — see §2.3a. An annotation may *describe* an object (including that a foreign agent owns its storage, which the compiler then acts on); none may *disable the compiler's responsibility for it*. This is a standing constraint on every annotation the language ever gains, not a fact about the ones it has today.

### 3.1c ★ Static borrow and concurrent borrow — one model, two realizations

The language distinguishes two **situations**, never two syntaxes. The programmer writes the same code in both; the compiler decides which one it is.

**Static borrow — the object stays in one execution context.** When ownership, aliasing, mutation and lifetime are all provable within a single execution context, the borrow is **compile-time semantic information only**. There is nothing to emit.

```text
create object
borrow object
use object
release object
```

⇒ compiles to **ordinary native operations with no borrow-management instructions whatsoever**. Not a cheap one — *none*. The optimizer is entitled to behave as though the ownership relationships were never written, because it has the proof that says so (§3.1d). This is tiers **T0–T3** of [`plan-09.5`](./09.5-dss-hir-plan.md) §4.2. (`D-AXIS-STATIC-BORROW-ZERO-COST`.)

**Concurrent borrow — the object escapes into several execution contexts.** When an object crosses thread or task boundaries and its lifetime or access relationships cannot be closed statically, the compiler **automatically materializes** the required control.

```text
Object
  |
  +-- Thread A
  |
  +-- Thread B
  |
  +-- Thread C
```

**The object stays alive while any participating execution context still has a valid requirement to access it**, and is destroyed at the point the last one relinquishes — a point in the emitted code, not a deferred sweep.

★ **The control structure may track substantially more than a reference count**, which is precisely why calling it refcounting understates it. It may need: active ownership/borrow participants · readers · writers · thread participation · synchronization state · destruction state · lifetime state · allocation metadata · whatever else the model requires.

⚠ **The exact runtime representation is an implementation detail and is NOT fixed here.** It is [`plan-09.5`](./09.5-dss-hir-plan.md)'s — the **TD tier**, which stands beside T4 rather than after it: T4 is the *single-context* dynamic residual (a precise, elided, non-deferred count), TD is the *multi-context* one (a record). **The language guarantee is the only thing this plan fixes: the compiler determines when dynamic enforcement is necessary, and materializes it without being asked.** (`D-AXIS-CONCURRENT-BORROW-DYNAMIC-RECORD`.)

★ **This is where §3.8's toolkit gets its soundness.** "A value moved to another thread must be proven uniquely owned (T1) or shared through a mechanism the lattice can see" was the open problem the old design left to the analysis and refused when it failed. **TD is that mechanism** — the third answer beside "proven unique" and "refused." The T1 move machinery still does the checking; what changed is what happens when it cannot conclude. (`D-AXIS-PAR-OWNERSHIP-ACROSS-THREADS`.)

### 3.1d ★ A proof is an OPTIMIZATION FACT — not a checkbox the analysis ticks

Ownership, lifetime, aliasing and concurrency proofs are **first-class optimization information**, carried in the IR and consumed by the optimizer like any other fact.

```text
              Ownership / Lifetime Analysis
                         |
              +----------+----------+
              |                     |
          Proven statically      Not proven
              |                     |
              v                     v
       Compile-time fact       Runtime enforcement
              |                     |
              v                     v
       Optimize/eliminate       DSS runtime
              |                     |
              +----------+----------+
                         |
                         v
                    Native code
```

Three consequences, and each is a requirement rather than a hope:

1. **Enforcement is emitted, then eliminated — never conditionally skipped.** The analysis does not decide whether to *insert* machinery; it produces facts, and the facts *delete* machinery. That ordering matters: a pass that skips insertion is correct only if the analysis is, whereas a pass that deletes on a proof leaves the un-proven case correct by construction.
2. **The runtime is therefore not a permanent cost.** It is materialized only where proof is insufficient. ★ **If the compiler can prove enforcement unnecessary, removing it is mandatory, not optional** — a design principle, not an optimization preference, because a runtime cost that survives its own disproof is the cliff §2.3's property 4 promises does not exist.
3. **The facts outlive the front end.** They are IR-level, so they reach MIR/LIR and the optimizer ([`plan-22`](./22-optimizer-plan%20-%20tbd.md)) — an ownership proof is an aliasing fact, and aliasing facts are what an optimizer is starved of. ★ **The memory model is a source of optimization power, not a tax on it.** (`D-AXIS-PROOF-IS-AN-OPTIMIZATION-FACT`.)

★ **A DECLARED fact enters the same channel as a derived one** — §3.5a's `readonly` / `noescape` are not a second mechanism: they are facts the analysis could not derive, entering the identical IR attribute the analysis writes, consumed by the identical passes. ⚠ The difference is provenance, and it must survive into the artifact: a **derived** fact is proven, a **declared** one is asserted, and "how much of this program rests on assertion" is a number §3.5a makes measurable rather than a feeling.

★ **And it is visible.** Every value's tier is in the emitted tier-classification artifact ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.5), assertable in tests, with **the reason inference failed** at every site that fell to TD. A hot loop sliding T1 → TD is a **test that goes red**, not a report someone might read. That is the mechanism that stops "enforce where unproven" from decaying into "enforce everywhere."

### 3.1e ⛔ No `unsafe` — and no equivalent, under any spelling

**DSS Axis has no `unsafe` block, no `unsafe` function, and no mechanism by which the programmer bypasses the memory model.** Not Rust's `unsafe`, not C#'s `unsafe`/`fixed`, not a raw-pointer type that opts out, not a compiler flag. (`D-AXIS-NO-UNSAFE-BLOCK`.)

★ **The reason this is affordable here and is not in Rust:** `unsafe` exists because a static-only model must reject what it cannot prove, so a language that rejects needs a door for the programs that were fine anyway. **DSS Axis does not reject them** — it enforces them (§2.3b). Removing the rejection removes the reason for the door.

```text
Can prove statically?
    |
    +-- yes --> encode proof in IR --> optimize away enforcement
    |
    +-- no  --> generate DSS runtime enforcement
```

⇒ the burden this places on the language is explicit and accepted: **Axis must give the compiler enough semantic vocabulary to describe both the statically provable and the dynamically determined case.** Where a programmer would reach for `unsafe`, the language owes them a way to *say what they mean* instead. That is a design obligation on every feature §1 eventually opens, and the FFI boundary (§3.5, §2.3) is where it ends — the one place the compiler is *told* it cannot see, rather than *failing* to see.

### 3.1f ★★ `@ownsCycle` — declaring where a cycle ENDS

**Operator ruling 2026-09-04.** §2.3's honesty clause leaves exactly one refusal standing: an owning-edge SCC that nothing breaks. This is the declaration that lets a programmer answer it **by describing the program** rather than by restructuring it — the first concrete answer to §5 Q12.

```
@ownsCycle class Scene { … }        // instances of Scene own their cyclic region
@ownsCycle fn buildGraph() { … }    // the cycle built here dies with this call
@ownsCycle { … }                    // …and as a bare block
```

**What it claims:** *the reference cycle reachable from this owner is reclaimed when this owner dies.* Nothing more. It does not suspend a tier, does not name a mechanism, and does not free anything by hand.

**The rule is bidirectional, and both halves were already the house rule:**

| | |
|---|---|
| an SCC survives with **no** owner declared and none inferable | ⛔ **compile error** — C4, unchanged in shape ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.3) |
| `@ownsCycle` on something that owns **no** SCC | ⛔ **compile error — inert config**, exactly as a `weak` that breaks no SCC is refused (`D-DSSHIR-WEAK-MUST-BREAK-AN-SCC`) |
| `@ownsCycle` whose members **escape** the owner | ⛔ **compile error** — the claim is false, and freeing on it would be a use-after-free |
| `@ownsCycle` the analysis **verifies** | ✅ the SCC becomes a reclamation unit ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.3a) |

★★ **Why this is NOT the escape hatch returning, and the test is §2.3a's own.** *An annotation may **describe** an object; none may **disable the compiler's responsibility** for it.* `@manual` said *"suspend T0–T4, I will call `free`"* — responsibility transferred, nothing checked. `@ownsCycle` says *"this cycle ends here"* — a **semantic claim the compiler verifies and may reject**. The programmer supplies a fact; the compiler still owns every decision that follows from it. That is the same species as `weak` (C3), which this design has carried from the start. ⇒ **it passes lock 10, and for a reason rather than by luck.**

⚠⚠ **The one way this could become an escape hatch is if it were ever BELIEVED instead of checked.** A declaration trusted where the analysis is weak produces a **use-after-free** rather than a leak — strictly worse than the refusal it replaced. **Unverifiable therefore means refused, never assumed.** This is the constraint the whole feature stands on ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.3a).

★ **Spelling — and `@loopBoundary` was MEASURED and rejected, on this repo's own naming precedent.** ✔**MEASURED 2026-09-04**: *loop* appears **1,093× across `.plans/` and 1,635× in `src/`**, every one of them meaning **iteration** — [`plan-22`](./22-optimizer-plan%20-%20tbd.md) alone is built on loop invariants, loop unrolling and loop nests. *boundary* appears **382× / 403×**, meaning the **FFI boundary**, the **module boundary** and lane boundaries. A name colliding **two ways at four figures** is unreadable; the `@kernel` spelling was rejected over a collision of **18** (§2.3a). `@cycleRoot` was also considered and rejected: *root* is GC vocabulary (`GcRoot`, and a root set is where **tracing** starts), which §2.3 forbids this design from borrowing. **`@ownsCycle` reads as a predicate about the annotated thing, works unchanged on a type, a function and a block, and takes parameters if the design later needs `@ownsCycle(entry: …)`.** ⚠ *cycle* is itself overloaded repo-wide — 5,250 occurrences, nearly all meaning a **development** cycle — but it is the memory model's settled term (the C0–C3 *cycle lattice*, *cycle collector*), so this inherits an existing ambiguity rather than creating a new one. (`D-AXIS-OWNS-CYCLE-ANNOTATION`.)

★ **What it buys, stated as three things rather than one.** (1) It **converts a refusal into a diagnosis** — *"you said the cycle ends here; `Node.parent` escapes via `register()`"* instead of *"I cannot prove this."* (2) It puts the intent **in the diff**, where an inferred region is invisible. (3) It is the only way to express a cycle whose termination is a **program fact** rather than a structural one — a graph built at startup and torn down at shutdown, where every edge is genuine ownership and no field is naturally `weak`.

⚠ **It must stay RARE, and that is a requirement on inference, not on programmers.** Region membership is inferable in the common cases ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.3 C1), and where it is inferred the annotation is **refused as inert** — so the surface cannot spread into ceremony. "Declare it wherever there is a cycle" would be the Rust-lifetime tax this language exists to avoid; **declare it only where inference cannot close it** is the contract. ★ And the reclamation-granularity cost — a member lives until its whole region dies — is **precision, never soundness**, reported per region in the tier artifact.

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
- **No GC of any kind** — compiler-owned memory, static where provable and deterministically enforced where not (§2.3); lattices in [`plan-09.5`](./09.5-dss-hir-plan.md) §4. ⚠ The dynamic tier is **not** a collector and must never be described as one (§2.3, `D-AXIS-NOT-GARBAGE-COLLECTION-VOCABULARY`).
- **The DSS Native Runtime** ([`plan-09.5`](./09.5-dss-hir-plan.md) §3) supplies threads / async / filesystem / sockets / timers / synchronization / workers / gui, absorbing platform discrepancies — opt-in via `runtime.enable` (§3.1a). ★ It is also the default **storage provider** the TD tier needs (§3.1a carve-out), and it is **authored in HIR** (§3.9).
- **Native-floor / adaptive-ceiling** — profiler + owned-compiler runtime recompiler in the base service (§2.4). (`D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING`.)
- Closed-world reflection native; open-world via optional **interpret-libs** (§2.5). (`D-AXIS-OPEN-WORLD-INTERPRET-LIBS`.)

### 3.5 FFI — import / export (first-class)
- **Import** — consume C / C++ / OS-supplied libraries; language-side `import` / `extern` syntax here, machinery in [`plan-11`](./11-ffi-plan%20-%20tbd.md) (binary readers, header parser, ABI catalog, mangling). (`D-AXIS-FFI-IMPORT`.)
- **Export** — DSS Axis libraries callable **natively from other languages** — the cross-language native-libs vision: a lib written in Axis consumed by a C, C#, or Python program, no VM, no marshalling VM boundary. (`D-AXIS-FFI-EXPORT`.)
- Hermetic throughout — no external runtime dependency; extern decls + shipped descriptors, same machinery the shipped languages use.
- ★ **This is where the memory model's proof ends, and it is the ONLY place** (§2.3 honesty clause). With `@manual` gone, FFI carries the whole boundary — so the vocabulary for *describing* what happens across it is load-bearing, not a convenience, and the diagnostics must say so when a type reaches it ([`plan-09.5`](./09.5-dss-hir-plan.md) `D-DSSHIR-RECLAMATION-FFI-BOUNDARY`). ★★ **§3.5a is the first half of that vocabulary** — `readonly` / `noescape` describe what a foreign callee does with an Axis object handed OUT. Foreign storage coming **IN** is still unspecified (§5 Q12).

### 3.5a ★★ FFI parameter contracts — `readonly` and `noescape`

**Operator ruling 2026-09-04.** §3.5 says the FFI edge is where the memory model's proof ends. **These two reserved words are how a programmer moves that edge outward** — not by weakening the model, but by supplying the one class of fact the compiler can never derive: *what the foreign callee does with an object it was handed.*

```
Car a;  Bike b;  int c;

ffi.call(a, readonly b, c);
//       ^            ^  a scalar by value — the callee cannot touch it
//       |            no contract: the compiler assumes the worst about `a`
//       `b`: the callee rewrites nothing reachable from it
```

#### ★ Why this is LIFETIME control, not a peephole

**An Axis object's fields are where the ownership edges live.** Foreign code that can write those fields rewrites the graph the memory model reasons about, invisibly:

- it **overwrites a field holding a reference** → the old target may now be unreachable and nothing knows → **leak**;
- it **stores a reference into a field** → an edge appears that was never in the graph → a region proved acyclic (§3.1f) may now be cyclic, or the compiler reclaims something it believes nothing points at → **use-after-free**;
- it **keeps a reference to the object itself** → reclaiming at scope end leaves foreign code holding a dangling pointer.

⇒ **an unannotated FFI call with an object parameter is a PROOF-DESTROYING EVENT for everything reachable from that parameter.** In §2.3's terms every static proof about that subgraph dies at the call, so the subgraph slides **T1/T2/T3 → TD** — or is **refused** where TD is unavailable, which is exactly the freestanding and `gpu` profiles (§3.1a). In an FFI-heavy compiler that is the difference between the static tiers being usable and being theoretical.

#### The two words, and they are ORTHOGONAL

| word | the claim | the graph edge it protects |
|---|---|---|
| `readonly` | the callee **modifies nothing reachable from** this object through Axis-owned edges | the edges **inside** the object |
| `noescape` | the callee **keeps no reference** to this object past the call's return | the edges **into** the object |

Either, both, or neither. A callee may legitimately mutate without retaining (`memset`-shaped) or retain without mutating (a registration callback). ⚠ **`readonly` does not imply `noescape` and must never be read as implying it** — they protect opposite directions, and conflating them is a use-after-free.

★ **`readonly` is DEEP, not shallow, and this is not a detail.** If the callee receives `b` and writes `b.wheel.bolt`, it has **broken** the claim. A shallow reading — only `b`'s own fields — buys nothing, because the ownership graph extends past the first hop and it is the whole reachable subgraph whose proofs must survive. **DOCUMENTED**: LLVM's parameter `readonly` is the deep reading, so precedent agrees; it is written here because the cheap version is the one an implementer reaches for first.

#### ★ The default is pessimistic — and it is already what the engine does

Absent a word, the compiler assumes the callee **both** modifies and retains. So **forgetting a contract is slow, never wrong**; only *asserting* one carries risk. An optimistic default would make a missing word a silent miscompile, which inverts the whole safety posture.

✔**MEASURED 2026-09-04**: this is not a new posture but a lift on an existing one. `src/opt/analysis/mir_escape.hpp`'s `mirPointerUseKind` classifies pointer uses through a whitelist **whose default arm is `Escapes`**, so a pointer handed to a call publishes its slot and every fact about it dies. **These two words are the first thing that could ever make that default not fire.** The consumers are already built and already measured: `mir_alias.hpp` (Rule 3b), `mir_memory_clobbers.hpp`, and through them CSE and LICM — the same pipeline whose clobber index took LICM **107s → 60.5s** on sqlite ([`plan-22`](./22-optimizer-plan%20-%20tbd.md), `D-OPT-MEMORYSSA-CLOBBER-WALK`). ★ **Unlike almost everything else in this plan, this feature has a shipped consumer waiting for it.**

#### ⚠⚠ The verification asymmetry — the two words are NOT equally safe

Neither claim can be checked statically: the callee's body is in an object file someone else compiled. By §3.1f's test that would make both escape hatches — **except that the claim is about foreign code nothing can ever see, so it adds information to a void rather than trading a proof for a promise** (§2.3's honesty clause). What keeps that from being a licence is that **both are checkable at RUN TIME, in a debug arm — but not equally well**, and the difference must be stated rather than discovered:

| word | debug-arm check | strength |
|---|---|---|
| `readonly` | the compiler knows the object's layout and can compute its reachable set — **hash the subgraph before the call, compare after, fail loud on a move** | ★ **direct.** A violation is caught the first time it happens, deterministically |
| `noescape` | foreign memory is unobservable, so the claim cannot be checked — only its **consequence**: quarantine the storage at reclamation behind an inaccessible guard page, so a later foreign access **faults loudly instead of corrupting silently** | ⚠ **by consequence only, and probabilistic** — it catches a violation that is *exercised*, never the claim itself |

⇒ **`noescape` is the sharper knife and the plan says so plainly.** A wrong `readonly` is caught by construction in a debug build; a wrong `noescape` is caught only if the foreign code actually dereferences after reclamation, on a run that happens to do it. ⚠ That does not make it unusable — it makes it the one that needs the strongest evidence before it is written, and it is why the debug-arm quarantine is a **requirement of the feature, not an optional hardening**. (`D-AXIS-FFI-CONTRACT-VERIFICATION-ASYMMETRY`.)

#### The rules, and two of them are this repo's rule reached a third time

- **On a by-value scalar, either word is REFUSED as inert.** `int c` cannot be modified or retained by the callee, so the word declares nothing — and an inert declaration is a compile error here, exactly as a `weak` that breaks no SCC ([`plan-09.5`](./09.5-dss-hir-plan.md) `D-DSSHIR-WEAK-MUST-BREAK-AN-SCC`) and an `@ownsCycle` that owns none (§3.1f) are refused. **Same rule, third application.**
- **Outside an FFI call the words do not exist.** They are not type qualifiers, not binding modifiers, and carry no meaning on a declaration — a claim about a *callee* has no referent where there is no callee.
- **The contract may be DEFAULTED by the descriptor and REFINED at the call.** `strdup` retains nothing on every call, and that belongs once in the shipped-lib descriptor row ([`plan-11`](./11-ffi-plan%20-%20tbd.md), which already carries a per-symbol signature). But `ioctl(fd, cmd, arg)` writes `arg` or not **depending on `cmd`**, which is a call-site fact no declaration can hold. ⇒ the call site is the primary surface and the descriptor supplies a default under it. ★ Where headers already declare it — SAL `_In_`/`_Out_`/`_Inout_`, GCC `__attribute__((access(read_only, N)))` — **harvest it rather than retype it**; [`plan-11`](./11-ffi-plan%20-%20tbd.md) already parses C headers, and a hand-written set stays trustworthy only while it stays small.

#### ★ Spelling — both names MEASURED against this tree

✔**RE-MEASURED 2026-09-04**, occurrences as whole words in `.plans/` / `src/` / `src/dss-config/`:

| candidate | count | verdict |
|---|---|---|
| **`readonly`** | **1 / 0 / 0** | ✅ the emptiest slot in the repo, and **DOCUMENTED** as LLVM's exact parameter attribute for exactly this claim |
| **`noescape`** | **0 / 0 / 0** | ✅ unused entirely, and **DOCUMENTED** as Clang's `__attribute__((noescape))` / Swift's `@noescape` for exactly this claim |
| `frozen` | 89 / 198 / 0 | ❌ taken in `src/` |
| `pinned` | 1002 / 290 / 87 | ❌ the project's word for a test pin |
| `borrowed` | 11 / 24 / 3 | ❌ ⚠ **the sharpest rejection**: T2 *borrow* is a **proven** non-owning reference; this is an **asserted** one. One word for both would make "is this checked or claimed?" unanswerable at the use site |
| `scoped` / `retained` / `transient` | 298 / 95 / 21 in plans | ❌ ordinary prose, too noisy to reserve |
| `final` / `volatile` | — | ❌ `final` names a property of the **binding**, which is not what is being claimed; `volatile` is a C keyword **this compiler implements** (✔996× in `src/`, including `c.lang.json` and `semantic_analyzer.cpp`) — redefining it inside the front end that implements the real one is the `@kernel` collision an order of magnitude worse |

⚠ **One caveat that must be written down once:** C# and Java readers will read `readonly` as a **binding** modifier ("assignable only in the constructor"). It is not. It is a claim about **the callee's behaviour during this one call**, and says nothing about `b` anywhere else. (`D-AXIS-FFI-PARAMETER-CONTRACTS`.)

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

A silent GPU→CPU fallback is a **10–100× performance cliff that produces correct answers**, which is exactly the failure class this project refuses everywhere else. It is the hardware analogue of §3.1a's lattice sliding T1→TD unnoticed — and the answer is the same: **the fallback must be declared, not discovered.** `gpu?` declares it; `gpu` refuses it. Neither ever happens quietly. (`D-AXIS-GPU-NO-SILENT-FALLBACK`.)

**Two failure tiers, and the earlier one is preferred:**
- **Build time** — `gpu` (mandatory) targeting a configuration with no SPIR-V path is a **build refusal**, naming the target. `gpu?` on the same target legitimately emits CPU-only.
- **Run time** — device absent or unusable: `gpu` exits with a **named diagnostic identifying the device requirement**, never a quiet downgrade.

#### The restricted subset — and the enforcement already ships

A `gpu` body is a **language subset**: no host heap, no host FFI, restricted recursion, address-space-carrying pointers, a register budget that is a correctness bar rather than a cost ([`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) §: *"a pointer that loses its space is a wrong-memory access, not a slow one"*).

★ **MEASURED**: the enforcement mechanism is **already in the tree** — the `H_ShaderViolation` diagnostic fires on *"a node inside a `ShaderUsable`-flagged function subtree"* (`src/core/types/parse_diagnostic.hpp`, the `H_ShaderViolation` entry), and [`plan-09`](./09-hir-plan%20-%20ok.md) HR6 shipped the shader-restriction subverifier. **`gpu` / `gpu?` are the language surface for a flag the HIR verifier already enforces** — not a new mechanism.

#### ★ `gpu` is a lattice profile (§3.1a), like freestanding

There is no host allocator on a GPU, so **T4 compile-time refcounting and the TD dynamic record are both unavailable inside a `gpu` body**; the reachable tiers are **T0** (stack) and **T3** (regions — the natural fit for shared/local memory). A `gpu` block therefore *declares a narrower lattice*, exactly as a driver does with `runtime.enable = false`, and it is the canonical instance of §3.1a's storage-provider carve-out: **inside a `gpu` body the residual is a refusal, because there is nowhere to materialize enforcement.** Two profiles, one mechanism — and the tier-classification artifact ([`plan-09.5`](./09.5-dss-hir-plan.md) §4.5) reports GPU bodies alongside everything else. (`D-AXIS-GPU-BLOCK-LATTICE-PROFILE`.)

#### ⚠ The honesty clause: `gpu?` does not promise identical results

CPU and GPU floating point differ — FMA contraction, denormal handling, transcendental precision. **A `gpu?` function may return different values depending on which path ran.** That must be written down or it will be assumed away.

★ And it is **not a new question**: [`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) §2.14's differential oracle *compares* the GPU output against the CPU twin, so it already needs an answer to "how equal is equal." **The oracle's comparison tolerance and `gpu?`'s result-equivalence promise are the same question and must have exactly one answer** — a tolerance the oracle accepts but the language does not disclose would be a silent numerical difference shipped to users. (`D-AXIS-GPU-FALLBACK-RESULT-EQUIVALENCE`.)

### 3.6b ★ `isa.*` — low-level operations that stay Axis operations

DSS Axis dedicates part of its syntax to **low-level operations expressed in DSS semantics**:

```
isa.mov(a, b);
```

These may correspond closely to machine operations — a move, a register operation, a memory operation, a barrier — but **they are not raw assembly and they do not escape the Axis memory model.** `a` and `b` are **Axis variables carrying Axis semantic values**, not registers and not addresses.

**What the programmer gains:** control over **which computation happens**.
**What the programmer does NOT gain — and is never asked to take on:** responsibility for the memory underneath it. The compiler still determines:

- where the values reside · whether storage must be allocated · object lifetime · ownership
- whether a value can be moved directly or must be copied · when storage can be reclaimed
- and it still enforces the concurrency/lifetime rules of §3.1c when required.

⇒ **high-level assembly with semantic memory management.** Descending the computational ladder does not transfer the memory ladder — there is no memory ladder to descend (§2.8, §6). (`D-AXIS-ISA-OPERATIONS-STAY-SEMANTIC`.)

★ **This is the same split the register allocator already embodies**, which is why it is implementable rather than aspirational: MIR/LIR operations name *computation*, and residence is the allocator's decision ([`plan-12`](./12-mir-lir-plan%20-%20ok.md)). `isa.mov(a, b)` names an operation and leaves residence exactly where it already lives. **INFERRED**, and it is the load-bearing feasibility claim of this section.

#### ⚠ `isa.*` is NOT inline assembly, and the two must never be conflated

[`plan-29`](./29-inline-asm-plan%20-%20tbd.md) settled that **assembly is its own source language** — a `.s` file or an `asm` unit is a *different language* whose values are machine registers and whose memory the compiler does not track. **`isa.*` is Axis.** Same-looking surface, opposite contract:

| | operates on | memory model | language |
|---|---|---|---|
| `.s` / `asm` unit ([`plan-29`](./29-inline-asm-plan%20-%20tbd.md)) | machine registers and addresses | none — untracked | a **separate** source language |
| `isa.*` (this plan) | **Axis values** | **full Axis model**, §2.3 | **DSS Axis** |

Both remain available; they answer different needs. Writing this down is not pedantry — a reader who assumes `isa.mov` is a spelling of `asm { mov }` will assume the memory model stops there, which is precisely the failure this section exists to prevent. (`D-AXIS-ISA-IS-NOT-INLINE-ASM`.)

#### ★ The vocabulary is target config, so this costs zero engine C++

`isa.*` names resolve against `opcodes[].mnemonic` in the active `.target.json` — **DSS's virtual-ISA vocabulary, which already exists**. ✔**RE-MEASURED 2026-09-04**: `x86_64.target.json` carries **111** opcode entries and `arm64.target.json` **131**, both including `mov`, `add`, `cmp`, `load`, `store`, `call`, `ret`. [`plan-29`](./29-inline-asm-plan%20-%20tbd.md) established the complementary half — that these names are *not* assembly spellings (a real `.s` writes `movq` / `b.eq`), which is exactly why they are the right vocabulary for a **semantic** low-level surface and the wrong one for a `.s` file.

⇒ `isa.*` is **target data, not engine code**: adding an operation is a `.target.json` edit, and the surface is agnostic by construction (Decision #4). ⚠ **An `isa.*` name the active target does not define is a build refusal naming the target** — the same shape as `gpu` on a target with no SPIR-V path (§3.6a), for the same reason: a silent substitution is a performance or semantics cliff that produces plausible output. (`D-AXIS-ISA-VOCABULARY-FROM-TARGET-CONFIG`.)

⇒ and an Axis program that uses `isa.*` is **target-specific by construction**. Whether a portable form exists (an `isa?` fallback to ordinary lowering, mirroring `gpu?`) is §5 Q11 — deliberately not invented here.

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

**Reclamation coherence.** *(Replaces the former "Async-GC coherence" paragraph — the property it asserted was that collection and contended waits share one scheduler without fighting over a thread. With no collector, that whole class of interaction is gone rather than solved.)* There is no collector to stall and none to be stalled by: a parked task holds no reclamation back, and nothing pauses a task to reclaim. ★ The concurrency question that *does* remain is the real one — **ownership across threads** — and §3.1c now answers it in all three cases rather than two: proven unique (T1, free), proven shared through a visible mechanism (T2/T3, free), **or enforced by the dynamic record (TD)** where neither holds. The old design refused the third case; this one serves it, which is what makes this toolkit expressible without an escape hatch. (`D-AXIS-PAR-OWNERSHIP-ACROSS-THREADS`.)

### 3.9 ★★ The runtime is authored in HIR — 100% of it, and not yet

**Operator ruling, 2026-09-04:** the DSS Axis runtime — every mechanism §2.3's dynamic tier lowers into, and the DSS Native Runtime services beside it — **will be written in HIR source code**. Not in C. Not in C++. Not in anything the project already ships as a source language. **100% of our runtime will and must be our own HIR.** (`D-AXIS-RUNTIME-AUTHORED-IN-HIR`.)

**Why it is possible:** HIR is the **C/C++-expressible floor** ([`plan-09.5`](./09.5-dss-hir-plan.md) §2.1). Anything C could express to build a runtime, HIR can express — so authoring the runtime in HIR is not a reach beyond the tier, it is the tier used at its own ceiling. And it closes the last gap in the hermetic invariant ([`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) §3, [`ZZ-final-goal`](./ZZ-final-goal.md) §2 property 3): **the runtime under a DSS binary is compiled by DSS from source DSS owns**, with no foreign front end in its build at all. [`plan-09.5`](./09.5-dss-hir-plan.md) §5 already said the runtime is *"written in DSS Axis (or C) and compiled by DSS, never shelled out to."* **This ruling removes the "(or C)."**

⚠⚠ **THE TIMING IS PART OF THE RULING, AND IT IS NOT NOW.** The migration happens **when C++ is fully ready** — because HIR's ceiling *is* C/C++ expressibility, so **the HIR surface rich enough to carry a runtime is precisely the one that compiling C++ proves out.** Migrating earlier means discovering the missing HIR one runtime feature at a time, in the substrate everything else stands on. Until then, whatever the runtime is authored in **is a stated interim, never the design.**

| | |
|---|---|
| **Trigger** | C++ support is complete — the same predecessor §1 already requires for this plan to open at all. |
| **Before it** | the runtime may be authored in C; ⚠ every such file is **interim by declaration**, and no design may assume C stays under the runtime. |
| **After it** | HIR source only. A C file under the runtime becomes a defect, not a choice. |

★ **A consequence nobody has flagged yet, and it upgrades an open question from cosmetic to blocking.** [`plan-09.5`](./09.5-dss-hir-plan.md) §0.1 / §8 Q1 records that **`.dsshir` is already taken — by plain HIR's round-trip text format** ([`plan-09`](./09-hir-plan%20-%20ok.md) HR7's `emitHir`/`parseHir`). While HIR text was only an emitted debugging artifact, that collision was a naming nuisance. **Once the runtime is authored in it, HIR text becomes first-class source** — version-controlled, diffed, reviewed, and sitting in a directory beside the tier that shares its name. Two further requirements follow, neither of which the round-trip format was designed for:

1. **HIR text must be HUMAN-WRITABLE, not merely round-trippable.** **INFERRED**, and it is a real requirement change: HR7's format was built to be *emitted and re-parsed*, and a format that survives a round trip is not automatically one a person can author, review or diff. Ergonomics, diagnostics on malformed input, and comment/formatting preservation all become requirements they were not before.
2. **The naming collision must be settled before the first runtime file is written**, not after — the cheapest moment is while zero files exist.

⇒ **this ruling makes [`plan-09.5`](./09.5-dss-hir-plan.md) §8 Q1 a prerequisite of the migration**, not an open question to carry indefinitely. (`D-AXIS-HIR-SOURCE-MUST-BE-HUMAN-AUTHORABLE`.)

---

## 4. Architectural locks (the engine must NOT foreclose these)

Guardrails the v2/engine work must honour so DSS Axis stays future-open (the [`plan-20`](./20-custom-language-reserved-plan%20-%20tbd.md) §2.2 discipline, extended):

1. **No VM assumption anywhere.** The runtime contract stays *library-linkable*, never VM-hosted. The HIR/MIR runtime intrinsics (effect/exception markers; the reserved `GcRoot`/`GcSafepoint`/`GcBarrier` slots — now candidates for retirement or repurposing, [`plan-09.5`](./09.5-dss-hir-plan.md) §8 Q7) must remain emittable into a self-contained native binary. ⚠ **The TD tier is not a claimant on those slots** — it has no safepoints and no barriers, which is property 3 of §2.3; if it turns out to want them, that is evidence something has drifted toward a collector and the drift is the finding.
2. **Pluggable runtime models.** The HIR must keep GC, exceptions, async, and effects as **attribute families**, never assuming one model — so Axis's ownership lattice + effect system slot in beside C's manual model, Rust's ownership, and **a GC-bearing language the engine may yet onboard**. ★ Axis renouncing GC does **not** license the engine to drop GC support: C#, Java, Python and JS all need it, and [`ZZ-final-goal`](./ZZ-final-goal.md) §3 commits to compiling them. **GC leaves the language; it does not leave the engine.** (`D-AXIS-ENGINE-KEEPS-GC-CAPABILITY`.)
2a. **The disabled-tier path stays complete.** Axis consumes [`plan-09.5`](./09.5-dss-hir-plan.md) DSS-HIR, but no engine code may assume a DSS-HIR node exists — a language bringing its own async/ownership model lowers through plain HIR ([`plan-09.5`](./09.5-dss-hir-plan.md) §2.3).
3. **Native, closed-world reflection.** Reflection metadata must be emittable into the binary and readable by a runtime *library* — no JIT/VM required for the closed-world surface.
4. **Embeddable compiler.** The owned compiler must be invocable *as a library* from a running program, so the base-service profiler can drive runtime recompilation (the adaptive ceiling). (`D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING`.)
5. **FFI export.** The engine's symbol/ABI/export machinery must support emitting DSS Axis libraries with stable, callable-from-other-languages interfaces. (`D-AXIS-FFI-EXPORT`.)
6. **Agnosticism (Decision #4).** DSS Axis is a `.lang.json` + lowering config — **zero per-language engine C++**. The day the engine branches on the language name is the day this plan's substrate cracks. ★ This binds `isa.*` too: its vocabulary is `.target.json` data (§3.6b), never a table in `src/`.
7. **`Task<T>` is the awaitable type.** The async lowering must target `Task<T>` / `Task` as a *real type* in the type system (C#-style TAP) — so async functions, constructors, and DI all share one awaitable currency that `await` and the scheduler compose around. The engine's async-attribute lowering must stay neutral enough to also model other languages' awaitables (JS `Promise`, Rust `Future`, C++ coroutine `task`), with `Task<T>` as DSS Axis's choice — not a hardcoded engine assumption. ★ The lowering now lands in [`plan-09.5`](./09.5-dss-hir-plan.md)'s `async` service rather than directly in HIR; lock 2a keeps the plain-HIR path open for languages that decline it. (`D-AXIS-ASYNC-TASK-SHAPE`.)
8. **Parallelism lowers as a closed intrinsic vocabulary.** Atomic ops, memory-ordering fences, and safepoints must live in the HIR/MIR as a *closed intrinsic set* ([`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) §2.4 / [`plan-12`](./12-mir-lir-plan%20-%20ok.md) `GcSafepoint`), so the §3.8 toolkit lowers config-driven to ISA atomics — never per-language concurrency C++ (Decision #4).
9. ★ **Ownership facts must survive lowering.** A proof discovered in the front end is worthless if it evaporates before the optimizer. Ownership / lifetime / aliasing / escape facts must be **representable in HIR and carried into MIR/LIR** as first-class attributes, not front-end-local state (§3.1d). This is not retrofittable: an IR that cannot say "these two pointers never alias, proven" forces every consumer to re-derive it or assume the worst. (`D-AXIS-PROOF-IS-AN-OPTIMIZATION-FACT`.)
10. ★ **No memory escape hatch may be introduced by any later feature.** Not an annotation, not a keyword, not a pragma, not a config key at the *declaration* level. §3.1a's lattice narrowing is a **deployment profile**, which is the deliberate and only exception — it constrains what the compiler may use, and never who is responsible (§2.3a, §3.1e). ★ **The test any future proposal must pass is §3.1f's**: a declaration that the compiler **verifies, and refuses when it cannot** is a description and is allowed; one **taken on faith** is an escape hatch whatever it is called — and it fails *worse* than `@manual` did, because an unchecked lifetime claim yields a use-after-free rather than a leak. (`D-AXIS-NO-MANUAL-ESCAPE-HATCH`, `D-AXIS-NO-UNSAFE-BLOCK`.)
11. ★ **HIR must be authorable as source.** The runtime migrates into HIR text (§3.9), so HIR's text format cannot remain an emit-only debugging artifact — and the `.dsshir` naming collision must be settled before the first runtime file exists ([`plan-09.5`](./09.5-dss-hir-plan.md) §8 Q1). (`D-AXIS-HIR-SOURCE-MUST-BE-HUMAN-AUTHORABLE`.)

---

## 5. Open questions (deferred until triggered)

| # | Question |
|---|----------|
| 1 | Typing: static with full inference (default lean — native + "easy as Node")? Gradual? Any dynamic surface beyond the interpret-libs tail? |
| 2 | ~~Memory model: async GC~~ ~~**RESOLVED 2026-08-12 — no GC**, refuse the residual~~ **RE-RESOLVED 2026-09-04 — no GC, and the residual is ENFORCED rather than refused** (§2.3, §2.3b). Lattices, staging and the dynamic tier are [`plan-09.5`](./09.5-dss-hir-plan.md) §4/§7. What remains open *here* is the **language surface**: the spelling of `weak`, whether borrows are ever written explicitly, and how a region/arena is named in source. |
| 3 | Generics: monomorphised vs reified default; how the per-target choice is expressed. |
| 4 | Exact syntax — the concrete grammar that delivers "easy as Node" with native semantics. |
| 5 | Where the closed-world ↔ open-world (interpret-libs) boundary is drawn, and how a program opts into the dynamic tail. |
| 6 | Module / package system + registry; how `import`/`export` map to it. |
| 7 | Self-hosting timeline — when (and in what order) the C++ engine transpiles into DSS Axis under the bit-identical HIR oracle ([`ZZ-final-goal`](./ZZ-final-goal.md) §6). |
| 8 | ⚠ **Two modifier surfaces.** §3.1b establishes annotations-with-parameters, §3.6a adds keyword modifiers (`gpu` / `gpu?`), §3.6b adds a namespaced call form (`isa.*`). Which jobs belong to which — and is `gpu?` really `@gpu(fallback: true)`? The keyword case rests on `gpu` restricting what is *legal inside* it, like `async`. ★ The retirement of `@manual` **removed the strongest counter-example** (an annotation that changed semantics without restricting content), so this question is now easier, not moot. Settle the boundary before any of the three ships. |
| 9 | Inside a `gpu` body: are **address spaces** (global / shared / local / constant) written by the programmer or inferred? "Easy as Node" argues inferred — but [`plan-17`](./17-shader-gpu-plan%20-%20tbd.md) warns a pointer that loses its space is a *wrong-memory access*, so inference must be total or the fallback is a refusal. |
| 10 | Does `gpu` / `gpu?` compose with `async` (§3.1) — is `gpu async fn` a dispatch that awaits its own completion, and is that the natural spelling for a kernel launch? |
| 11 | ★ **Does `isa.*` get a portable form?** `gpu?` exists because a CPU twin always exists; the analogue for `isa.*` would be "use this operation where the target has it, otherwise lower the equivalent Axis expression." Is that a real need or an invitation to write target-specific code that *looks* portable (§3.6b)? |
| 12 | ★ **What does the programmer WRITE when they would have written `@manual`?** §2.3a's answer is "describe the object." **TWO OF THREE ANSWERED 2026-09-04.** A cycle → `@ownsCycle` (§3.1f), verified or rejected. An Axis object handed OUT across FFI → `readonly` / `noescape` (§3.5a), debug-verified rather than statically checked. **STILL OPEN: foreign storage coming IN** — the DMA buffer whose lifetime the hardware owns, the allocation a foreign library will free itself. That vocabulary is unspecified and is now the *last* piece of the boundary, so it should copy §3.5a's shape (a claim, a pessimistic default, an inert-declaration refusal, and a debug arm that catches a false one) rather than be invented separately. |
| 13 | ★ **What is TD's observable surface, if any?** The representation is [`plan-09.5`](./09.5-dss-hir-plan.md)'s implementation detail (§3.1c) — but can a program *ask* (how many participants, am I the last)? A query is useful for diagnostics and is also the crack through which programmer-visible lifetime management returns. Default lean: **no**, and the tier artifact (§3.1d) is the answer to every question a query would have served. |
| 14 | ★ **Where does the destructor run for a TD-tier object?** The last participant releases — but *on which thread*, and what is legal in that body? A destructor running on an arbitrary thread is a well-known hazard, and it is a **language-contract** question rather than an analysis one, so it belongs here rather than in [`plan-09.5`](./09.5-dss-hir-plan.md). |

All deferred until §1 trigger conditions are met.

---

## 6. ★★★ The architectural principle, stated in full

> **DSS Axis separates computational control from memory-management responsibility.**
>
> The programmer may explicitly request increasingly low-level computational operations, including `isa.*` operations. **The compiler retains responsibility for memory, ownership, lifetime, allocation, reclamation and concurrency safety at every level, without exception.**

```text
High-level Axis
    |
    | semantic variables
    v
Low-level Axis / ISA operations
    |
    | still semantic Axis values
    v
Code Prime HIR/MIR/LIR
    |
    v
Native machine code
```

**At no point does lowering to a lower-level operation transfer memory-management responsibility to the programmer.** There is one ladder — computational control — and descending it changes what the program *does*, never who is responsible for what it *owns*.

The coherence the design must hold, end to end:

```text
Axis syntax
    ↓
HIR semantic representation
    ↓
ownership / lifetime / concurrency analysis
    ↓
static proof OR dynamic enforcement
    ↓
optimization
    ↓
MIR / LIR
    ↓
native code
```

★ **The compiler — never the programmer — turns Axis's memory semantics into the minimum necessary runtime and native machinery.** *Minimum* is the operative word: enforcement that a proof shows unnecessary is **removed**, not merely made cheap (§3.1d).

⛔ **The absence of `@manual` and of `unsafe` is intentional and fundamental.** It is not a gap to be filled by a later convenience feature, and lock 10 exists so a future cycle cannot reintroduce one without confronting this section.

---

## 7. Deferred anchors (owned by this plan; register when it opens)

These **50** `D-AXIS-*` anchors are **reserved/future** — they live here until the plan opens, then move into [`_deferred-anchor-registry-production`](./_deferred-anchor-registry-production.md) as active rows. (Reserved-plan anchors are not yet in `src/`, so the CI anchor-guard does not require registry rows today.)

⛔ **Retired 2026-09-04, recorded so it is not re-minted:** `D-AXIS-MANUAL-ANNOTATION` (the `@manual` escape hatch) — deleted with the design it named, §2.3a. [`plan-09.5`](./09.5-dss-hir-plan.md) retires its analysis counterpart on the same grounds.

| Anchor | Owns |
|--------|------|
| `D-AXIS-COMPILER-OWNS-MEMORY` | ★★★ the compiler owns memory in every case; the programmer describes semantics (§2.3) |
| `D-AXIS-NO-MANUAL-ESCAPE-HATCH` | ⛔ no `@manual` and no equivalent, under any spelling — the absence is a designed property (§2.3a) |
| `D-AXIS-NO-UNSAFE-BLOCK` | ⛔ no `unsafe` mode; the model has no bypass (§3.1e) |
| `D-AXIS-RESIDUAL-ENFORCES-NEVER-REFUSES` | ★ failure to prove generates enforcement, not rejection — the cycle refusal is the one survivor (§2.3) |
| `D-AXIS-OWNS-CYCLE-ANNOTATION` | ★★ `@ownsCycle` — a CHECKED claim that a cycle ends here; inert or unverifiable = refused (§3.1f) |
| `D-AXIS-NOT-GARBAGE-COLLECTION-VOCABULARY` | ★ five falsifiable properties; "GC"/"collector"/"managed" are wrong about this design (§2.3) |
| `D-AXIS-STATIC-BORROW-ZERO-COST` | a proven single-context borrow emits **no** runtime machinery (§3.1c) |
| `D-AXIS-CONCURRENT-BORROW-DYNAMIC-RECORD` | the cross-context residual materializes an ownership record; representation is 09.5's (§3.1c) |
| `D-AXIS-PROOF-IS-AN-OPTIMIZATION-FACT` | proofs enter the IR and survive into MIR/LIR; enforcement is deleted, not skipped (§3.1d, lock 9) |
| `D-AXIS-DYNAMIC-TIER-NEEDS-A-STORAGE-PROVIDER` | ⚠ TD needs atomics + storage; a profile without one refuses its residual (§3.1a) |
| `D-AXIS-CONTROL-AND-MEMORY-ARE-SEPARATE-AXES` | ★★★ descending the computational ladder never transfers memory responsibility (§2.8, §6) |
| `D-AXIS-ISA-OPERATIONS-STAY-SEMANTIC` | `isa.*` operates on Axis values under the full Axis memory model (§3.6b) |
| `D-AXIS-ISA-IS-NOT-INLINE-ASM` | ⚠ `isa.*` is Axis; a `.s`/`asm` unit is a separate language with no memory model (§3.6b) |
| `D-AXIS-ISA-VOCABULARY-FROM-TARGET-CONFIG` | `isa.*` names resolve against `.target.json` `opcodes[]`; unknown name = build refusal (§3.6b) |
| `D-AXIS-RUNTIME-AUTHORED-IN-HIR` | ★★ 100% of the runtime in HIR source, gated on C++ readiness (§3.9) |
| `D-AXIS-HIR-SOURCE-MUST-BE-HUMAN-AUTHORABLE` | HIR text becomes real source, so round-trippable is no longer sufficient (§3.9, lock 11) |
| `D-AXIS-ASYNC-CONSTRUCTORS` | `async new(...)` construction-that-awaits |
| `D-AXIS-ASYNC-DI` | language-side async dependency injection API |
| `D-AXIS-MEMORY-LATTICE-CONFIG` | the `memory` block — a declared tier lattice, never a boolean (§3.1a) |
| `D-AXIS-RUNTIME-ENABLE-CONFIG` | the `runtime` block — opt-in DSS Native Runtime, independent of `memory` (§3.1a) |
| `D-AXIS-ANNOTATIONS-WITH-PARAMETERS` | `@name(...)` on declarations, members and blocks — with no memory escape in it (§3.1b) |
| `D-AXIS-ENGINE-KEEPS-GC-CAPABILITY` | ★ GC leaves the LANGUAGE, not the ENGINE — C#/Java/Python/JS still need it (§4.2) |
| `D-AXIS-PAR-OWNERSHIP-ACROSS-THREADS` | moving a value across threads: proven unique, visibly shared, or TD-enforced (§3.1c, §3.8) |
| `D-AXIS-GPU-EXECUTION-MODIFIER` | `gpu` / `gpu?` on methods and blocks; keyword not annotation (§3.6a) |
| `D-AXIS-GPU-NO-SILENT-FALLBACK` | ★ a GPU→CPU downgrade is declared (`gpu?`) or refused (`gpu`), never quiet (§3.6a) |
| `D-AXIS-GPU-BLOCK-LATTICE-PROFILE` | a `gpu` body declares a narrower lattice — no T4 and no TD without a host allocator (§3.6a) |
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
| `D-AXIS-FFI-PARAMETER-CONTRACTS` | ★★ `readonly` / `noescape` at an FFI call — deep, orthogonal, pessimistic by default, inert on a scalar (§3.5a) |
| `D-AXIS-FFI-CONTRACT-VERIFICATION-ASYMMETRY` | ⚠ `readonly` is debug-checkable directly; `noescape` only by consequence — the quarantine arm is required, not optional (§3.5a) |
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

## 8. Sequencing

**Not sequenced. Reserved.** No cycles until §1 triggers. When it opens, this file is renamed to ` - ok.md`, gains a `## 0.1 Stepper` with a tier overview + cycle-by-cycle log (the active-plan shape), and the §7 anchors migrate into the registry as work begins.

The order of the language's own arc, when it comes, follows [`ZZ-final-goal`](./ZZ-final-goal.md) §6: **design from HIR experience → author DSS Axis → transpile the C++ engine into it (self-host; `D-AXIS-SELF-HOST-TRANSPILE`) → database → OS.** This plan is the reservation for the first of those steps.

★ **Two things in this plan are NOT gated on §1 and must not wait for it**, because both constrain work happening now:

1. **The architectural locks (§4)** — every one is a constraint on engine work in flight. Lock 9 in particular (ownership facts survive lowering) is *not retrofittable* and lands with the IR, not with the language.
2. **The runtime-authoring ruling (§3.9)** — its trigger is C++ readiness, and its prerequisite ([`plan-09.5`](./09.5-dss-hir-plan.md) §8 Q1, the `.dsshir` collision) is cheapest to settle **while zero HIR source files exist**. That is now, not at §1.
