# DSS Code Prime — Final Goal (Omega Plan)

> **This is the north star.** Every other plan in `.plans/` is a stepping stone to the vision captured here. When a design decision is ambiguous, this document is the tie-breaker: pick the path that keeps the omega vision reachable. When a shortcut is tempting, this document is the reminder of what the shortcut costs.
>
> **Not a roadmap, not a schedule.** Plan 00 is the roadmap; the numbered plans are the schedule. This is the *destination* — what the finished product is, why nobody else has it, and the order in which the pieces have to land for the dream to survive contact with reality.

---

## 1. The Thesis in One Paragraph

DSS Code Prime is a universal, config-driven compiler engine whose vocabulary grows until it can faithfully compile every modern language (C++, C#/.NET, Java, Python, PHP, JS/Node, and beyond). The engine pivots through a language-neutral typed HIR, which means the same engine that compiles those languages can transpile between them, lower them to native binaries on every OS/arch, lower them to GPU shaders, and lower them to WASM — all through a hermetic in-tree toolchain that owns every byte from source to signed binary. Once the engine is comprehensive enough, we design **the DSS Code Prime language** — informed empirically by what the HIR has shown us is universal across the languages we've compiled. We then transpile the C++ engine into DSS Code Prime via the engine's own source-to-source layer, becoming fully self-hosted. From that point, the entire ecosystem — database, operating system, runtime services — is built inside our own environment.

---

## 2. The Four Load-Bearing Properties

The product is defined by the intersection of four properties. Each one alone exists in some shipping toolchain; the intersection does not exist anywhere.

1. **Config-driven all phases.** Every phase — lex, parse, semantic, HIR lowering, MIR/LIR rules, FFI ingestion, codegen hints — is described by JSON config. The engine has zero per-language C++. A new language is a config PR, not an engineering quarter. Decision #4 of plan 00 is the load-bearing discipline: extend the vocabulary, never branch the engine on `schema.name()`.

   **What "config-driven" admits** (the three-bucket rule that closes the back-half drift):

   | Bucket | What it is | Where it lives | Config-driven? |
   |---|---|---|---|
   | **1. Declarative layout** | Fixed-width fields, opcode tables, section/symbol tables, reloc formulas, encoding rows (e.g. ModR/M slots, ARM bit-field templates), DWARF tag tables. | JSON | ✅ schema = bytes |
   | **2. Universal algorithm over declared vocabulary** | Isel (patterns + costs), instruction encoding (prefix/ModR/M assembly), procedural streams (Mach-O bind/lazy/rebase opcode trie, DWARF line program, CFI), regalloc, GOT/PLT synthesis. | One engine algorithm; JSON declares the vocabulary it walks | ✅ — this is "the hard part in the source code" |
   | **3. Identity-branching C++** | `if (schema.name() == "c-subset")`, `if (arch == "arm64") { ... } else if (arch == "x86_64") { ... }`, per-arch/per-format/per-language `.cpp` directories. | Nowhere — forbidden | ❌ Decision #4 violation |

   **The operative test, stated once:** config-driven is defined by **the absence of an identity branch, not the absence of an algorithm**. A 2,000-line universal bind-trie emitter that branches on nothing is fully thesis-compliant. A 10-line `if (format == "macho")` is not. The hard algorithms living in the engine ARE the design — not a compromise.

   **Bucket 2 is shape-keyed multi-walker dispatch, not a single mega-function.** In-tree precedent: `ChildLower` in `src/core/types/hir_lowering_config.hpp` declares a closed verb set (`Expr` / `FlatExpr` / `Ext` / `Ref` / `VarDecl`); each verb has its own specialized walker; the engine dispatches on the verb (a closed-vocabulary enum), never on language identity. Same pattern at the back half: encoders dispatch on `encoding.format`, stream emitters dispatch on section shape — each handler is a specialized walker, but they're shape-keyed (closed vocabulary), not identity-keyed (open per-target).

   **Arch-shape differences live in the JSON vocabulary, not in the engine.** x86 ModR/M, ARM bit-field encoding, RISC-V immediate splits are different *encoding-row formats* — bucket 1 declarations rich enough to express all three uniformly. The bucket-2 algorithm walks the declared rows; it never branches on which ISA produced them.

   **Existence proof:** ML5 cycle 2a (2026-05-29) pivoted from `Lir<TargetTraits>` (templated per arch) to `runtime TargetSchemaId + JSON config + universal engine`. The MIR→LIR isel is a ~2,000-line bucket-2 algorithm parameterized by the JSON vocabulary. Adding ARM64 became "drop a `.target.json`," zero engine work. This is the template for the back half (assembler / linker / debug-info / shader / WASM).

   **The one carve-out:** any genuinely unavoidable identity-specific code is a named, delimited real-blocker per [[feedback_no_deferring_without_blocker]] — never silent, never permanent. The expectation is that no such carve-out is needed; if one becomes necessary, that's a thesis falsification event worth its own review.

2. **HIR as a real pivot.** Language-neutral, structured, typed, with attribute side-tables for shader/FFI/transpile/async/effects/GC/etc. HIR is the layer where transpilation, native lowering, GPU lowering, and WASM lowering all converge. LLVM IR is too low for clean source-to-source; MLIR is a framework for dialects, not a pivot. HIR is what makes the engine multi-purpose instead of single-purpose.

3. **Hermetic end-to-end.** Own assembler, linker, DWARF/PDB writers, codesigner, crypto substrate. No `ld` / `link.exe` / `ld64` / `lld` / `wasm-ld` / `xcrun` / `clang` / `gcc` / `MSVC` / `dxc` / `glslc` / `dsymutil` / `mspdb` / `codesign` / `signtool` invocations. OS-supplied runtime libs, browsers, GPU drivers, and Apple developer certs are FFI targets and credentials — never tools. The payoff: cross-compile signed Mach-O from a Linux box with no Apple tooling. Nobody else can do that.

4. **Same-source CPU + GPU + WASM + source-to-source convergence.** One source file can lower to native binaries on every OS/arch, to SPIR-V shaders, to WASM, *and* transpile into any other supported language — through one engine, one HIR.

**Why nobody has it:**

| Property | LLVM/Clang | GCC | MSVC | Roslyn | GraalVM | Zig | Tree-sitter | Haxe |
|---|---|---|---|---|---|---|---|---|
| Multi-language single engine | ✓ | ✓ | ✗ | ✗ | ✓ | ✗ | ✗ | ✓ |
| Config-driven frontend | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | parse only | ✗ |
| Config-driven all phases | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| Hermetic toolchain | ✗ | ✗ | ✗ | ✗ | partial | ✓ | n/a | ✗ |
| HIR pivot for transpilation | ✗ | ✗ | ✗ | ✗ | partial | ✗ | ✗ | partial |
| CPU + GPU + WASM + source-to-source | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |

The intersection is unclaimed because each existing leader made an early architectural choice that forecloses the combination: LLVM's frontends are hand-written C++; Tree-sitter has no semantic or codegen layer; Zig is single-language; Haxe is hand-written and has no shader/native parity. The combination is the moat.

---

## 3. Vocabulary Growth: The Modern-Language Coverage Plan

The config vocabulary grows until it can faithfully compile every modern language. This is *the* execution thesis — if it holds, everything else follows. If it breaks, the architecture cracks.

**Tier 1 (post-v1 expansion):** C, C++, C# / .NET, Java, Python, PHP, JS / Node, TypeScript.

**Tier 2 (broader paradigms — important for HIR informing the DSS Code Prime language design):** Rust (ownership), Swift (ARC), Go (CSP / goroutines), Kotlin, Ruby, Lua, Haskell or OCaml (a real functional language).

**Tier 3 (specialty):** GLSL / HLSL / WGSL (shader), Solidity (smart contract), SQL dialects beyond tsql, R, MATLAB, VHDL/Verilog (covered by reserved plan 19).

Each language tier likely stresses the vocabulary in a new way:

- **C++** stresses templates, multiple inheritance, RAII, exception specifications.
- **.NET / Java** stress GC contracts, reflection, JIT-friendly metadata, generics with type erasure vs. reified.
- **Python / JS** stress dynamic typing, monkey-patching, prototype chains, late binding.
- **Rust** stresses ownership / borrow lifetimes as a first-class HIR concept.
- **Swift / ObjC** stress ARC, protocol-oriented dispatch, message passing.
- **Go** stresses goroutines + channels as a first-class concurrency primitive.
- **Haskell / OCaml** stress purity, monadic effects, ADTs with exhaustive matching.

The pattern: every new language likely adds one HIR attribute family + one runtime contract. That's a plan-sized round per language family (similar to HR1–HR11), not a single PR. The discipline that makes the rounds linear-additive rather than exponential is the same discipline that got HIR through HR1–HR11: extend the vocabulary, never language-branch the engine.

---

## 4. Built-in Language Capabilities (Beyond Syntax)

These are runtime contracts and language features that must exist in the engine — not just as syntax, but as HIR attributes with real lowerings.

- **`async` / `await` / `yield`** — state-machine rewriting at HIR level; runtime task scheduler with cooperative multitasking. Must lower to whatever async model the target language/runtime expects (state machines for C#, futures for Rust, microtask queues for JS, event loops for Python).
- **Garbage collection — async GC.** Differentiator. Concurrent GCs exist (Go, ZGC, Shenandoah, .NET background GC) but they all run on dedicated GC threads with at minimum microsecond-scale STW pauses. Async GC participates in the same scheduler as user `async` tasks — collection is itself an `async` task. This is research territory; if it ships, it is the moat for the DSS Code Prime language. Owned by plan 21 (runtime).
- **Reflection** — runtime type info, runtime method dispatch, runtime field access. HIR carries the metadata; runtime exposes it through a reified type system.
- **Exceptions** — first-class HIR concept with multiple lowerings: stack-unwinding (C++/C#/Java), Result-style (Rust), multi-return (Go), panic+recover (Go), Swift-style `throws`. The HIR `effect` system models all of these.
- **Effect systems** — capability-based and effect-based contracts; pure functions, IO, async, throws, allocates. Lets the language and the compiler reason about side effects without committing to one model.
- **Generics** — both monomorphized (C++/Rust style) and reified (C#/Java style), expressible at HIR level. Picks per-target.
- **Pattern matching** — exhaustive ADT matching, destructuring, guards.
- **First-class concurrency primitives** — actors, channels, async tasks, structured concurrency.
- **SIMD / vector / matrix** — already in the core type lattice; runtime + codegen lowers to ISA SIMD or scalar fallback.

Each of these is a plan-sized round, likely between v1 and the DSS Code Prime language design.

---

## 5. The Unified Ecosystem (DSS Code Prime Language Stdlib)

After the engine compiles modern languages, the HIR has empirically revealed which primitives are universal. The DSS Code Prime language stdlib is built around those primitives — unified, platform-blind, and works the same way on every OS / arch / runtime target.

- **Async DI** — first-class dependency injection that participates in the async scheduler. Resolution can be async; lifetimes can be scoped to async contexts.
- **Services** — unified up / down / recover lifecycle that works identically as a Windows Service, a systemd unit, a launchd daemon, an Android background service, or a Linux daemon. One contract, every platform.
- **Networking** — unified socket / HTTP / HTTPS / WebSocket / gRPC stack. No per-platform impedance mismatch.
- **Push notifications** — one API for APNs, FCM, WNS, Web Push.
- **Collections** — lists, queues, deques, stacks, maps, sets, trees (including B-trees, R-trees, persistent trees). Concurrent and persistent variants first-class.
- **File manager** — unified filesystem abstraction over NTFS, ext4, APFS, Android storage, browser FS, with platform-specific extensions exposed.
- **Parallel** — task parallelism + data parallelism + actors + channels, all unified with the async scheduler.
- **GPU** — same-source CPU + GPU functions (plan 17 already designs this). Write a function once; tag it for CPU and GPU; the engine lowers to both. No CUDA / OpenCL / shader-language impedance.
- **Persistence / serialization** — built-in (binary, JSON, MessagePack-style) with schema evolution.
- **Crypto** — vendored substrate (BearSSL per plan 16), exposed as language stdlib.
- **Time / dates / time zones** — one API, IANA tzdata vendored.
- **Logging / tracing / metrics** — structured, correlation-id aware, OpenTelemetry-compatible.

The stdlib is what makes the language usable; the engine is what makes the stdlib portable.

---

## 6. Self-Hosting Bootstrap

The point where DSS Code Prime stops being a project written *in* C++ and becomes a project written *in itself*. Every serious language toolchain (C, Go, Rust, Zig, TypeScript, Swift) has done this for the same reason: until the compiler is written in its own language, two codebases are being maintained (the host-language engine and the target language) and they drift.

**Sequencing (the only order that works):**

1. **v1 minimal E2E** — C++ engine, source → binary, the languages currently shipped. Plans 00 through 16.
2. **v2/v3 increments** — more language coverage; async, GC, reflection, exceptions land in HIR + runtime. Plans 17, 18 light up.
3. **The DSS Code Prime language — now named DSS Axis — exists** — designed *from HIR experience*, not in a vacuum. Plan 24 (DSS Axis; reserved — supersedes the placeholder plan 20) opens. The language is informed by what the HIR proved universal across the Tier 1 + Tier 2 languages, so it doesn't suffer the "designed in isolation, missing real-world concerns" failure mode.
4. **Transpile the C++ engine → DSS Code Prime** — plan 10 (source-to-source) stops being v1.x scope and becomes load-bearing. Phased, not big-bang: transpile the core first (HIR / MIR / LIR / passes), keep the platform layer in C++ for a release, then transpile the platform layer once the runtime is mature. Rust did this with `rustc`; Go did it all-at-once because its runtime was tiny.
5. **Self-hosted compiler** — the C++ codebase becomes reference / legacy; new development happens in DSS Code Prime.
6. **Database in DSS Code Prime** — leveraging the language's persistent collections, async I/O, and concurrency primitives.
7. **OS in DSS Code Prime** — leveraging the hermetic toolchain (no host OS toolchain needed to compile a freestanding binary) and the language's systems-level capabilities.

**The self-hosting correctness oracle (designed in from day one):**

The HIR is the pivot, so when the C++ engine transpiles to DSS Code Prime, both the original C++ frontend and the new DSS Code Prime frontend lower to the *same* HIR. The bootstrap CI test becomes:

> Compile input `X` through path A: `X` → C++ engine → binary B_a.
> Compile input `X` through path B: C++ engine → DSS Code Prime source → DSS Code Prime engine → binary B_b.
> Require `B_a == B_b` (bit-identical, modulo timestamps).

If those two paths diverge, either HIR isn't language-neutral, or a lowering config has a bug, or the transpilation layer has a bug. This is the regression net that makes the entire config-driven thesis falsifiable in production. Few transpilers ever get this test; we get it for free because we're self-hosting through our own pivot.

**Plan placeholder:** when bootstrap work starts, open `22-self-hosting-plan` (currently unreserved — will be its own document at that time).

---

## 7. The Endgame: Database and OS

The historical pattern: every serious OS came from a serious language (Unix from C, Windows NT from C/C++, Midori from C#, Fuchsia from C++/Rust, Redox from Rust). Every serious database came from a serious systems language (Postgres/MySQL/SQLite from C, CockroachDB/TiDB from Go, ScyllaDB from C++, Materialize from Rust). The pattern holds for the same reason: a database or OS is a multi-decade artifact that needs full control over memory, concurrency, and the toolchain. You cannot build one in a language you don't own.

By the time we reach this layer, we own:

- The compiler (hermetic, multi-target).
- The language (designed for systems work).
- The runtime (async GC, scheduler, GPU, networking).
- The toolchain (assembler, linker, debug-info, codesign).
- The cross-compilation story (every OS / arch from any host).

This is **the Lisp Machine / Smalltalk-80 / Oberon dream** — the entire stack in one language, no impedance mismatch at any layer. Those projects didn't fail because the dream was wrong; they failed because they were ahead of their hardware and the ecosystems weren't portable. We are not subject to those constraints: we target commodity hardware via our own assembler/linker, and the FFI plan (11) keeps us connected to existing ecosystems indefinitely. The "fully self-contained" vision is achievable now in a way it wasn't in 1985.

---

## 8. Risks and Disciplines

The execution risks are real, named, and tracked. The disciplines that mitigate them are the same disciplines that got the project to plan 09 complete.

### Risks

1. **Vocabulary explosion.** Every language stresses the schema vocabulary. If the vocabulary grows faster than the engine's ability to absorb it, the config eventually rivals the engine in complexity. *Mitigation:* every vocabulary extension must be additive, must be reusable across languages, and must be proven generic by at least one language other than its motivating one (the discipline already in use — Synth schemas, the c-subset→tsql proof for HIR lowering, etc.).
2. **Decision #4 violation.** The day someone slips `if (schema.name() == "c-subset")` into the engine is the day the thesis cracks. *Mitigation:* the standing "no workarounds" rule + the per-PR 5-agent review + the explicit cleanup rounds (e.g., 08.55) that retire any hardcoded language knowledge.
3. **HIR fragmentation.** If different languages need fundamentally different HIR shapes (e.g., dynamic dispatch can't be modeled the same way as static dispatch), HIR splits into multiple pivots and the transpilation story collapses. *Mitigation:* attribute side-tables instead of forking node shapes; HR5's attribute system was specifically designed for this growth.
4. **Backend scope.** Plans 13/14/15/16 (assembler / linker / debug-info / codesign) are the largest single chunk of v1 work and the area furthest from current expertise. Underestimating this is the most likely cause of v1 slipping by years. *Mitigation:* the plans are already broken into PR-sized chunks; ship in vertical slices (one OS × one arch end-to-end before broadening).
5. **Self-hosting timing.** Attempting self-hosting before the language is mature enough produces a worse codebase than the C++ original. *Mitigation:* sequencing rule — self-hosting only after the language has been used in anger for at least one substantial non-compiler project (likely the database or a smaller stdlib component).
6. **DSS Code Prime language overfitting.** If the language is designed after only the current shipped languages (toy / c-subset / tsql), it overfits to imperative + typed and won't serve dynamic, functional, or ownership-based use cases. *Mitigation:* before opening plan 20, the engine must have compiled at least one dynamic language (Python or JS) and one ownership/affine language (Rust). Three paradigms is the minimum for the HIR to inform an honest universal language design.
7. **Execution stamina.** A 10–20 year arc requires a sustained engineering culture, not a heroic sprint. *Mitigation:* the per-PR review cadence + the standing discipline of closing every gap (no silent deferrals — D-registry in plan 00) keeps quality from eroding under time pressure.

### Disciplines (standing rules — do not relax)

- **No workarounds.** Implement the clean complete solution now. No deferring without a real blocker (see memory `[[feedback_no_deferring_without_blocker]]`).
- **Per-PR 5-agent review + fix-everything pass.** Standing since v2 / substrate hardening (see memory `[[feedback_per_pr_review_cadence]]`).
- **Config-driven all phases.** No engine code branches on language name (see memory `[[project_config_driven_all_phases]]`).
- **Fail loud.** Every unsupported construct emits a real diagnostic, not a silent miscompile or a TODO comment. The HIR lowering's `H_UnsupportedLoweringForKind` is the template.
- **Strict-assertion testing.** Every regression visible (see skill `dss-code-prime` — strict assertion posture is what makes the architecture safe to evolve).
- **Substrate-tier review for substrate changes.** Anything touching arenas, attributes, the type lattice, HIR / MIR / LIR shapes gets the heaviest review treatment.

---

## 9. How to Use This Document

- **Tie-break design decisions** that affect long-horizon scope. If a choice would foreclose a section of this document, the choice is wrong.
- **Sanity-check new plans.** A new plan that doesn't connect to a section here probably belongs to a different project.
- **Onboard new contributors.** This is the "why are we doing this" doc; plan 00 is the "what are we doing first" doc.
- **Revisit yearly.** The vision shouldn't change, but the *order* of layers and the *language tier list* will sharpen as Tier 1 languages are onboarded.

The vision is coherent. The architecture supports it. The execution is the work of the next decade. Plan 00 covers the next few quarters; this document covers the next few decades.
