# Production Readiness — Master Plan

> **Scope.** dss-code-prime is now committed to be a real production cross-platform compiler. v1 ships **end-to-end binaries for every language in `src/source-config/languages/`** (`toy`, `c-subset`, `tsql-subset`) on **{Windows, Linux, macOS} × {x86_64, ARM64}**, gated by the [`artifactProfile`](./06-artifact-profile-plan - tbd.md) compatibility mechanism. This plan catalogs everything missing between current state and that bar.
>
> **This is the master gap catalog for v1 production.** It supersedes the per-phase rows in `00-compiler-implementation-plan - tbd.md` §8 by reframing them as production deliverables with concrete acceptance criteria, and lists cross-cutting gaps (CI, perf, corpus, tooling) that don't fit any single phase.

---

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🔵 **opened.** v1 production target: end-to-end binaries for 3 shipped languages on 3 OS × 2 arch. No deliverables shipped against this plan yet — current state is "frontend infrastructure done, backend infrastructure largely unborn." |
| Predecessors  | ✅ core-types (T0–T12); ✅ schema-expressiveness v1 + v2 (PR0–PR8); ✅ substrate hardening (SH1–SH4); ✅ tokenizer (TZ1–TZ3 + review-fix r1). |
| Successors    | None — this plan IS the v1 bar. Post-v1 candidates (LSP, incremental parsing, PGO, additional architectures) are tracked in §9. |

### v1 deliverable in one sentence

> A single CLI invocation `dss-code-prime build my-project.dss-project.json` produces a working artifact (`.exe` / `.dll` / `.so` / `.dylib` / `.sql`) on Windows/Linux/macOS × x86_64/ARM64, for any language declared in `src/source-config/languages/`, with diagnostic UX equivalent to modern compilers (`clang`/`tsc`/`rustc`-level errors).

### Headline gaps

The §1–§7 sections below enumerate **127 distinct gaps** (numbered for cross-reference). The five most consequential:

| ID    | Gap                                                                 | Why it matters |
|-------|---------------------------------------------------------------------|----------------|
| G-001 | **Parser driver shipped (PA1–PA3 ✅).** Schema-driven iterative RD driver (PA1) + Pratt walker for operators (PA2) + panic-mode recovery with schema-declared `syncTokens` and load-time `followSetOf` + clang-style diagnostic rendering (PA3). 715 tests / 35 suites green. Corpus stress (PA4) and LSP scaffolding (PA5a/PA5b) remain. | Largest "does this work at all" risk fully retired. See [`05-parser-plan - tbd.md`](./05-parser-plan - tbd.md). |
| G-002 | **Semantic phase undesigned.** `src/analysis/semantic/` is a stub; no symbol table, no type checker, no scope resolver. | Required for any non-trivial codegen. Type errors are the most common real-world diagnostic. |
| G-003 | **No IR.** `src/gen/intermediate/` is empty. No IR design, no SSA decision, no lowering pass.  | The hinge between frontend and backend. Wrong design here forces frontend OR backend rework. |
| G-004 | **Codegen is one Windows PE demo.** No ELF, no Mach-O, no ARM64, no IR-driven path. | v1's hardest single chunk of work. Three object formats × two arches × three runtimes (CRT / glibc / libSystem). |
| G-005 | **No project-config format.** Driver layer is unspecified; users can't actually invoke the compiler against a real project. | Without this, every other artifact has no entry point. |
| G-006 | **No compilation-unit model.** Parser produces one Tree per file; nothing bundles them. Semantic / IR / codegen would each invent their own answer. | See [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md) — phase #7.5 between parser and semantic. |

---

## 1. Frontend gaps (parser phase + beyond)

| ID    | Gap | Where it surfaces | Resolves via |
|-------|-----|-------------------|--------------|
| G-101 | Parser driver shipped (PA0–PA3 ✅, G-001 detail). Substrate, iterative RD driver, Pratt walker, and recovery + diagnostic UX all landed; PA4 corpus stress + PA5a/PA5b LSP remain. | `src/analysis/syntactic/parser.{hpp,cpp}` + 715 tests across 35 suites | [`05-parser-plan - tbd.md`](./05-parser-plan - tbd.md) PA0–PA3 ✅, PA4–PA5b ⏳ |
| G-102 | ✅ Operator precedence in the AST. Closed end-to-end in PA2 (v2-gap-catalog row 1). | `ParserCSubsetSmoke.FunctionBodyExpressionIsPrecedenceCorrect` | parser-plan PA2 ✅ |
| G-103 | Function calls `f(x, y)` — no postfix-call shape. | c-subset.lang.json `operand` | parser-plan PA4 + operator-table postfix arity (v2-gap-catalog row 7) |
| G-104 | Array indexing `a[0]`. | c-subset.lang.json `operand` | parser-plan PA4 + postfix arity (v2-gap-catalog row 12) |
| G-105 | Pointer ops `*p`, `&x`. | c-subset.lang.json `operand` | parser-plan PA4 + prefix arity + typeRef-side change (v2-gap-catalog row 6) |
| G-106 | Postfix `x++` / `x--`. | c-subset.lang.json `operand` | parser-plan PA4 + postfix arity (v2-gap-catalog row 8) |
| G-107 | Compound assignment `+= -= *= /=`. | c-subset.lang.json `tokens` | Add to tokens + binary slot in `binaryOp` (v2-gap-catalog row 16) |
| G-108 | Ternary `? :` — mixfix. | not-yet-tokens | **v3 candidate** — current `Prefix/Infix/Postfix` enum doesn't model mixfix (v2-gap-catalog row 17) |
| G-109 | Float literal lexing (`3.14`, `1e10`, `1.0f` with suffix rules). | tokenizer | Hand-coded today in c-subset's tokenizer path; full numeric-style descriptor is v3 (v2-gap-catalog row 14) |
| G-110 | Multi-file translation units. The current model is one file → one tree. | [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md) CU1 + CU2 | **Phase #7.5** (new) — `CompilationUnit` type + `UnitBuilder` aggregator. Semantic / IR / codegen all consume CUs instead of bare Trees. |
| G-111 | `#include` / module imports. Both c-subset and tsql-subset (`USE database;`) need cross-file references. | [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md) CU4 | **Phase #7.5** — per-language `ImportResolver` populates `CrossTreeRef` edges before semantic consumes the CU. Schema-driven import syntax stays a v3 candidate; v1 dispatches resolver on language name. |
| G-112 | Preprocessor for full C (not c-subset). | n/a | **Explicitly out of v1.** c-subset omits the preprocessor by design. Full C99 is post-v1. |
| G-113 | T-SQL `GO` batch separator, `BEGIN ... END` blocks, cursors, `TRY/CATCH`. | tsql-subset.lang.json | Out of v1 if PA4 corpus doesn't need them; otherwise extend tsql-subset (mechanism exists — pure shape work per v2-gap-catalog §6 "Residual T-SQL gaps"). |
| G-114 | ✅ Error recovery quality benchmark. PA3 ✅ — bar set to "every error produces an actionable message; recovery continues without cascading beyond 3× the original error count" (pinned by `ParserRecovery.SingleErrorCascadeBoundedAtThreeX`). Real-world stress is PA4's job to validate. | `tests/analysis/syntactic/test_parser_recovery.cpp` | parser-plan PA3 ✅ |
| G-115 | ✅ Diagnostic UX — clang-style line/column/caret rendering. `DiagnosticReporter::format()` produces the display end-to-end; PA3 populated parser-side `expected`/`actual`/`ruleContext` so every parser-emitted code surfaces correctly. Multi-char `^^^` underline matches span length. CLI wiring + future LSP (PA5a) both consume the same renderer. | `tests/core/test_diagnostic_reporter.cpp` | parser-plan PA3 ✅; driver-side CLI wiring still pending |

---

## 2. Semantic phase gaps (§8 phase #8)

| ID    | Gap | Notes |
|-------|-----|-------|
| G-201 | Symbol table design. Linear chain of scopes? Hash-of-hashes? Persistent immutable? **CU-scoped** post phase 7.5. | Decide before phase #8 PR1. Default: stack-of-hashmaps owned by `CompilationUnit`; `SymbolId` interner is CU-scoped; cross-tree refs come pre-resolved from `CompilationUnit.crossRefs()`. |
| G-202 | Scope resolution pass — walk the CST, populate `NodeAttribute<SymbolId>` on every identifier reference. | Reuses the existing `NodeAttribute<T>` machinery (T8). |
| G-203 | Type representation. Nominal? Structural? Trait-based? | C-subset wants nominal + pointer types; tsql-subset wants nominal + SQL row types. Decide on a sum type that covers both — `TypeKind` enum + `TypeId` interner on a per-tree side table. |
| G-204 | Type inference / checking. C-subset is mostly type-annotated; tsql-subset wants column type inference from `CREATE TABLE` declarations + cross-statement flow. | C-side: minimal — verify expression types match LHS at assignment + arg-list match at calls. T-SQL side: needs the symbol table for table/column resolution. |
| G-205 | Const-correctness. `const int x = 5; x = 6;` must error. | Treat `const` as a type-attribute flag, not a separate Type. Reuse `NodeFlags` or a parallel `TypeFlags`. |
| G-206 | Typedef resolution (v2-gap-catalog row 2). The schema mechanism exists; the symbol-table side is what closes it. | Adds an `Identifier → TypeId` lookup at the scope-resolution pass. |
| G-207 | Function overload resolution. C-subset doesn't have overloads; T-SQL has built-in function overloads (`COALESCE`, etc.). | Per-language decision. Defaults: c-subset = no overloads, single name → single symbol; tsql-subset = built-in function overload table on the language config. |
| G-208 | Cross-translation-unit symbol resolution. Two `.c` files share a `.h` declaration. | Depends on G-110/G-111. Symbol table must accept multi-tree input. |
| G-209 | Forward-reference resolution. Functions calling functions declared later in the file. | Two-pass semantic walk: declarations first, references second. Standard pattern; just needs implementing. |
| G-210 | Unused-variable / dead-code analysis. | Out of v1 if the codegen doesn't require it. Defer to the optimizer phase. |
| G-211 | Diagnostic codes for semantic errors. `S_*` namespace (alongside `P_*`/`C_*`/`D_*`). | New diagnostic prefix; codes for type-mismatch, undeclared-identifier, redeclaration, const-violation, etc. |
| G-212 | Death-test discipline for the semantic phase — match the substrate's "fail-loud over silent fallback" posture. | Apply the same `DSS_ASSERT` style and explicit `S_*` emission on every recovery path. |

---

## 3. IR phase gaps (§8 phase #9)

| ID    | Gap | Notes |
|-------|-----|-------|
| G-301 | IR design. SSA? CFG-of-basic-blocks? Three-address? Stack? | Default: **SSA over a CFG** — industry-standard, well-understood, plays nicely with the optimizer passes phase #10 lists. |
| G-302 | IR text format for debugging / caching. | A round-trippable `.dssir` text format + a binary form for build caches. Both are non-trivial; the text form is v1, binary is post-v1. |
| G-303 | IR verifier. Walk the IR, assert structural invariants (every SSA def dominates its uses, every basic block has a terminator, etc.). | Essential for catching backend bugs early. Same fail-loud discipline as the frontend's strict-assertion testing. |
| G-304 | Lowering CST → IR. The pass that walks the tree (with semantic attributes attached) and emits IR. | Per-language: each language config implicitly defines its lowering (a `cli` artifactProfile lowers `main` → IR-level `entry` block; a `script` profile lowers differently). |
| G-305 | IR diagnostic namespace `I_*`. | Same as the others — fail-loud on invariant violations. |
| G-306 | IR type system — distinct from the source-language type system. Backend types (`i32`, `i64`, `f32`, `f64`, `ptr`, aggregates). | Lower source types into IR types at the start of lowering. Mismatched-arity / wrong-type IR ops abort. |
| G-307 | Calling convention metadata on IR function definitions. | Needed before codegen. Per-platform: SysV AMD64 / Windows x64 / AArch64 AAPCS64 / Apple ARM64. |
| G-308 | Profile-driven lowering knobs. `artifactProfile = lib` vs `cli` emit different IR-level entry-block patterns; `script` skips IR entirely. | Tracked in `06-artifact-profile-plan - tbd.md` AP3 — the `CompilationContext` carries the resolved profile. |

---

## 4. Optimizer gaps (§8 phase #10)

The master plan lists eight specific passes (constant folding, propagation, DCE, CSE, copy propagation, strength reduction, loop-invariant motion, plus CFG/liveness/reaching-definitions analyses). For v1 production, the question is **which subset is mandatory vs optional**.

| ID    | Gap | v1 status |
|-------|-----|-----------|
| G-401 | Constant folding. Trivially safe; produces noticeably smaller binaries. | **Mandatory.** Even at `-O0`. |
| G-402 | Dead code elimination (block-level). Same. | **Mandatory.** Required for sensible binary sizes. |
| G-403 | Copy propagation + value numbering. | **Mandatory at -O1.** Otherwise the IR is full of redundant temporaries. |
| G-404 | CSE (common subexpression elimination). | **Optional v1.** Real gains but bug-prone; defer to v1.1. |
| G-405 | Loop-invariant code motion. | **Optional v1.** Requires loop detection on the CFG — non-trivial. |
| G-406 | Inlining. | **Out of v1.** Significant complexity; needs a heuristic + size budget. |
| G-407 | Strength reduction (`x*8` → `x<<3` etc.). | **Optional v1.** Backend can do these as peepholes instead. |
| G-408 | Dominator-tree analysis. | **Mandatory.** SSA requires it. |
| G-409 | Liveness analysis. | **Mandatory.** Register allocation requires it. |
| G-410 | Reaching definitions. | **Optional v1.** Required for some optimizations not in the v1 set. |

**v1 acceptance:** Constant folding + DCE + copy propagation + dominator tree + liveness. Other passes deferred to v1.1.

---

## 5. Backend / codegen gaps (§8 phase #11)

This is the **largest single chunk of work** in v1.

### 5.1 Object file formats

| ID    | Gap | Notes |
|-------|-----|-------|
| G-501 | ELF emission for Linux. | Headers + program headers + section headers + symbol table + relocations. Crate-from-scratch OR vendor `libelf`-style code. |
| G-502 | PE emission for Windows. The current demo writes one. | DOS stub + PE header + section table + import directory + relocation table. Demo proves the structure works; needs IR-driven path. |
| G-503 | Mach-O emission for macOS. | Header + load commands + segments + symbol table + LINKEDIT. Apple Silicon adds chained-fixups requirement (new format). |

### 5.2 Architectures

| ID    | Gap | Notes |
|-------|-----|-------|
| G-510 | x86_64 instruction selection + encoding. | ISA reference: Intel SDM. Encoding is finicky (REX prefixes, ModR/M, SIB). |
| G-511 | ARM64 / AArch64 instruction selection + encoding. | Fixed-width 32-bit instructions — simpler encoding than x86, but more registers + a different ABI. |
| G-512 | Register allocation. Graph-coloring? Linear-scan? | **Linear scan** is the v1 pick — simpler, faster compile times, "good enough" code quality. Graph coloring is v1.1+. |
| G-513 | Calling-convention conformance per platform. | Linux x86_64 = SysV AMD64; Win x86_64 = Microsoft x64 (different reg map, shadow space); macOS x86_64 = SysV with quirks; ARM64 = AAPCS64 (uniform across Linux/macOS); Win ARM64 = Microsoft ARM64 (FP register-pair quirks). |
| G-514 | Stack frame layout — prologue/epilogue, frame pointer omission, alignment. | Per ABI. Easy to get wrong; pin with assembler-diff snapshot tests. |
| G-515 | Position-independent code. Required for ELF/`.so` and Mach-O. Win PE doesn't require PIC but does require base-relocations. | Affects every load/store of global data. |
| G-516 | Thread-local storage. Per-platform (`__thread` on ELF, `_declspec(thread)` on PE, `__thread` on Mach-O). | Out of v1 if c-subset doesn't use it. Decide. |
| G-517 | Atomics + memory model. | Out of v1 if no shipped language needs them. C-subset doesn't currently express atomics. |

### 5.3 Runtime + ABI

| ID    | Gap | Notes |
|-------|-----|-------|
| G-520 | C runtime linkage on Windows. MSVCRT? UCRT? | UCRT is the modern pick. Means linking against `ucrtbase.dll` + the import library. |
| G-521 | glibc / musl on Linux. | Default: link against the host's glibc. Static musl as an opt-in for portable binaries (post-v1). |
| G-522 | libSystem.dylib on macOS. | Single libSystem entry-point — no choice here. |
| G-523 | Crt0 / startup runtime — the code that runs before `main`. | Per-platform. Easy to get wrong; pin with "Hello, World"-tier integration tests on every platform. |
| G-524 | DLL entry point (`DllMain`) on Windows / `_init`+`_fini` on ELF / `__attribute__((constructor))` on Mach-O. | Required for `lib` artifactProfile. |
| G-525 | Exception handling tables. | Out of v1 if no shipped language throws. C-subset doesn't currently express exceptions. |

### 5.4 Debug info

| ID    | Gap | Notes |
|-------|-----|-------|
| G-530 | DWARF emission for ELF + Mach-O. | DWARF 4 or 5. Sections: `.debug_info` / `.debug_line` / `.debug_abbrev` / etc. Without this, `gdb`/`lldb` can't step through generated binaries. |
| G-531 | PDB emission for PE. | PDB is Microsoft-proprietary; the format is documented but complex. LLVM has a working implementation worth studying. Without it, WinDbg / Visual Studio debugger can't step. |
| G-532 | Source-position fidelity through CST → IR → codegen. Every instruction needs a `SourceSpan` mapping. | Existing tree carries `SourceSpan`; IR lowering preserves it; codegen emits debug-info entries pointing at it. |
| G-533 | Variable lifetime tracking for the debugger. | Out of v1 if the v1 bar is "step through and see line numbers" rather than "inspect locals." |

### 5.5 Linker integration

| ID    | Gap | Notes |
|-------|-----|-------|
| G-540 | Strategy decision: invoke system linker (`ld`/`link.exe`/Apple `ld`) or ship a built-in linker? | **System linker** for v1. Simpler, well-tested, supports all the platform quirks. Built-in linker (à la `mold`/`lld`) is a post-v1 perf play. |
| G-541 | Linker flag plumbing per `artifactProfile` × platform. | Profile-driven flag tables in `gen/link/`. |
| G-542 | Driver-level temp-file management for the linker invocation (object files, response files). | Standard temp-dir handling; cross-platform via `std::filesystem`. |
| G-543 | Linker error capture + repackaging into the diagnostic stream. | A linker failure shouldn't be a raw stderr dump; parse the linker's output and produce structured `D_*` diagnostics. Per-linker (LLD vs ld.bfd vs ld.gold vs Apple ld vs link.exe). |

---

## 6. Driver, project, distribution gaps (§4.1 program/)

| ID    | Gap | Notes |
|-------|-----|-------|
| G-601 | Project config file format (`.dss-project.json` or similar). | See [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) §2.2 for the proposed shape. Schema doc: `docs/project-config-spec.md` (new). |
| G-602 | `artifactProfile` mechanism end-to-end. | [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) AP1–AP4. |
| G-603 | CLI surface. `dss-code-prime build`, `dss-code-prime check`, `dss-code-prime fmt` (post-v1). | New `program/cli.cpp` parsing argv into commands. |
| G-604 | Output-path conventions per `artifactProfile` × platform. `dist/foo.exe` vs `dist/libfoo.so` vs `dist/foo.dylib`. | Driver-level convention; codegen emits to wherever the driver says. |
| G-605 | Build caching. If sources haven't changed, don't re-tokenize/parse/lower. | Post-v1. v1 = always full rebuild. |
| G-606 | Deterministic output. Same input → same byte-identical binary. | v1 acceptance. Required for reproducible-build workflows. |
| G-607 | Multi-file compilation orchestration — which files compile in which order, dependency tracking. | Depends on G-110. v1 picks a simple strategy (single-tree per input file; link at the end). |
| G-608 | Diagnostic-rendering layer in the driver. Renders `tree.diagnostics()` as clang-style display. | Driver-level, not parser-level. parser-plan PA3 produces the data; the driver renders it. |
| G-609 | Exit code conventions. 0 = success, 1 = source error (P_*/C_*/S_*/I_* diagnostics), 2 = driver error (D_* diagnostics), 3 = internal compiler error. | Codify in driver. |

---

## 7. Cross-cutting gaps (CI, perf, corpus, distribution)

### 7.1 CI matrix

| ID    | Gap | Status |
|-------|-----|--------|
| G-701 | Linux x86_64 (GCC + Clang). | ✅ done via DSS.DevOps@v2 multi-OS matrix (SH2). |
| G-702 | Windows x86_64 (MSVC). | ✅ done. |
| G-703 | macOS x86_64 (AppleClang). | ✅ done (SH2 opt-in). |
| G-704 | Linux ARM64. | ⏳ pending. AWS Graviton runners or self-hosted. |
| G-705 | Windows ARM64. | ⏳ pending. GitHub Actions has Windows-ARM64 runners as of 2024. |
| G-706 | macOS ARM64 (Apple Silicon). | ⏳ pending. GitHub Actions has macos-14 runners. |
| G-707 | Cross-compilation matrix. Build host = Linux x86_64, target = Win/Mac ARM64. | Post-v1 unless v1 requires it. Default: every target is built on its native host. |
| G-708 | End-to-end integration tests on each runner — compile a real program, run it, assert output. | Required for v1. Currently the test suite is all unit/integration at the library level; nothing actually runs a compiled binary. |
| G-709 | Sanitizer coverage. ASan, UBSan, MSan (Linux Clang only), TSan. | ✅ partially done (Linux/Clang+ASan from SH2). Extend to UBSan + add to PR-gating. |
| G-710 | Fuzzing. AFL / libfuzzer against the tokenizer + parser + JSON loader. | v1: at least one fuzzer per public entry point, run nightly. Catches more than 5-agent review for input-domain bugs. |

### 7.2 Performance

| ID    | Gap | Notes |
|-------|-----|-------|
| G-720 | Compile-time perf baseline. "Tokenize + parse + lower + codegen `N`-LOC program in `M` ms." | Pick a baseline corpus (the `tests/corpus/` real programs from parser-plan PA4). Pin a number in CI as a soft regression check. |
| G-721 | Memory budget for the compiler process. | Especially for IDE / LSP use later. v1: just measure + document. Optimization is post-v1. |
| G-722 | Generated-binary perf vs hand-tuned C compiler. | Off-by-2x at v1 is acceptable; off-by-10x means linear-scan regalloc isn't pulling its weight. |
| G-723 | Profile the compiler itself. Where does v1 spend its cycles? | v1 acceptance check. Hot-spot a non-trivial corpus compile; document the top-10 functions. |

### 7.3 Corpus + real-world stress

| ID    | Gap | Notes |
|-------|-----|-------|
| G-730 | `tests/corpus/c-subset/` — ~5 real programs covering pointer ops, function calls, arrays, control flow, struct (if struct lands in c-subset; otherwise plain functions + locals). | parser-plan PA4 prereq. |
| G-731 | `tests/corpus/tsql-subset/` — ~5 real scripts covering DDL + DML + multi-statement batches. | parser-plan PA4 prereq. |
| G-732 | `tests/corpus/toy/` — at least one non-trivial toy program. | parser-plan PA4. |
| G-733 | "Compile then run" tests in CI. Compile a corpus program, run the produced binary, assert exit code + stdout match expected. | Per platform. Costs CI minutes; pays for itself instantly. |
| G-734 | Differential testing — compile a c-subset program with dss-code-prime AND with `gcc -O0`, run both, assert identical behavior. | Post-v1 unless v1 needs it. Strong correctness signal. |

### 7.4 Documentation

| ID    | Gap | Notes |
|-------|-----|-------|
| G-740 | `docs/language-config-spec.md` covers v1 + v2 + the new `artifactProfile` field. v3 fields landed post-v2 (G-109's numeric-style descriptor, G-108's mixfix) need their own sections when they ship. | Keep current. |
| G-741 | `docs/project-config-spec.md` — NEW for the project-config file format. | See artifact-profile-plan AP2. |
| G-742 | `docs/architecture.md` — top-level architecture diagram + data flow. The compiler-implementation-plan has it inline; pull into a doc engineers actually read. | Post-v1 unless onboarding pressure makes it urgent. |
| G-743 | `docs/contributing.md` — per-PR review cadence, plan-tracking discipline, landing-log invariants, strict-assertion test posture. | Required if a second engineer onboards. Documents the discipline that's currently in heads. |
| G-744 | Per-language onboarding guides. "How to write a `.lang.json`." | Currently `language-config-spec.md` covers the schema; a tutorial walking through writing toy-from-scratch would close the new-language gap. |
| G-745 | Per-target onboarding guides. "How to add a new platform." | Needed when the first post-v1 platform request lands (WASM, embedded, ...). |

### 7.5 Versioning + compatibility

| ID    | Gap | Notes |
|-------|-----|-------|
| G-750 | `dssSchemaVersion` strategy across v3+ bumps. | `06-artifact-profile-plan - tbd.md` proposes v3. Lock down the bump rules: bumps are additive (new fields default to v2-equivalent behavior); breaking changes require a new major (v3 = `4.x.x` release of the compiler binary). |
| G-751 | Public API stability of `dss::*` headers. Today the DLL is built; consumers depend on the headers. | Define the public-API surface (probably: everything in `core/types/` headers that's `DSS_EXPORT`). Promise binary compat across minor versions. |
| G-752 | License + IP cleanliness for shipping a real product. | Decide license. Audit third-party deps (`nlohmann/json`, googletest). |
| G-753 | Reproducible builds — same compiler binary built from same source on same OS is byte-identical. | Standard sweep: strip timestamps, normalize paths, etc. |

### 7.6 Process

| ID    | Gap | Notes |
|-------|-----|-------|
| G-760 | Per-PR 5-agent review discipline scales for plumbing PRs but slows velocity for "real product" pace. | **Action: categorize PRs.** "Substrate" PRs (changes to `core/types/` headers, schema loader, tree builder) keep the full 5-agent review. "Feature" PRs (new shapes, new tests, language config edits) drop to 2-agent (`code` + `tests`). "Doc" PRs skip review. Codify in CONTRIBUTING. |
| G-761 | Solo+AI fragility — no successor. | Onboarding doc (G-743). Pair second engineer in by end of v1. |
| G-762 | Forcing-function loop. v1 production scope is now defined; v1 milestones are not yet calendar-anchored. | TBD with stakeholders. Default: PA1 + PA2 ship Q3; semantic + IR ship Q4; first end-to-end c-subset → ELF binary by year-end. Negotiate. |

---

## 8. Sequencing — what unblocks what

```
[done] tokenizer ✅
    │
    ▼
[next] parser PA0 ─► PA1 ─► PA2 ─► PA3 ─► PA4 ─┬─► PA5a ─► PA5b   (LSP, per-file)
                                                │
                                                ▼
                                        CU1 ─► CU2 ─► CU3 ─► CU4   (phase 7.5)
                                                                │
                                                                ▼
                                                semantic (G-201..212) ─► artifactProfile AP3 ─► IR (G-301..308)
                                                                                │                     │
                                                                                ▼                     ▼
                                                                project-config + driver ──────► optimizer (G-401..410)
                                                                                │                     │
                                                                                ▼                     ▼
                                                            artifactProfile AP1+AP2+AP4 ────► codegen ─► linker
                                                                                              (G-501..524) (G-540..543)
                                                                                                          │
                                                                                                          ▼
                                                                                                  CI + corpus + perf
                                                                                                          │
                                                                                                          ▼
                                                                                                       v1 ship
```

The critical path runs through: **parser → compilation-unit → semantic → IR → codegen × 3 OS × 2 arch**. PA5 (LSP) and CU1..CU4 are **siblings** branching off PA4 — LSP doesn't depend on the CU layer (operates per-file). The artifactProfile mechanism plugs in alongside semantic (driver enforcement) and again at IR/codegen (lowering knobs). Optimizer can develop in parallel with codegen once IR is stable.

---

## 9. Post-v1 (deferred but tracked)

These are intentionally NOT v1 deliverables. Tracking them here so v1 work doesn't make decisions that close them off.

- ~~LSP server / IDE integration~~ — **moved to v1** as parser-plan PA5 (diagnostics-only scaffolding); semantic-powered LSP methods (hover/completion/goto-def/references/rename) remain post-v1 in a dedicated LSP follow-up plan after phase #8.
- Incremental parsing + build caching (G-605) — LSP currently re-parses on every `didChange`; build caching is the perf play that follows.
- Inlining + CSE + loop-invariant motion (G-404, G-405, G-406)
- Graph-coloring register allocation (G-512)
- WASM target
- Built-in linker (à la `lld`/`mold`)
- PGO / LTO
- Full C99 (preprocessor + everything c-subset omits) as a separate language config
- Stored-procedure deployment automation for tsql-subset's `sproc` artifactProfile
- Differential testing (G-734)
- Cross-compilation (G-707)
- Static musl on Linux (G-521 alt)
- Exception handling (G-525)
- TLS (G-516) + atomics (G-517) if a language needs them

---

## 10. Acceptance — what "v1 production-ready" means

A PR titled "v1 — production-ready" is accepted when **every** item below is checked:

- [ ] All three shipped languages (`toy`, `c-subset`, `tsql-subset`) parse `tests/corpus/` programs end-to-end via the real parser (no hand-driven `TreeBuilder`).
- [ ] **Multi-file integration test** per language: c-subset two-file (`main.c` + `helper.h` with cross-file call), tsql-subset two-file (`schema.sql` + `data.sql` with concat ordering), toy degenerate single-file. Phase 7.5 `CompilationUnit` aggregates correctly.
- [ ] **LSP server** starts via `dss-code-prime --lsp`, completes the `initialize` handshake, and publishes diagnostics in a real editor session (golden-file replay). Semantic-powered methods return empty results (lit up post-phase #8).
- [ ] `artifactProfile` is enforced: a project asking for an unsupported profile gets `D_ArtifactProfileNotSupported` at config-load.
- [ ] Codegen produces working binaries for `cli` and `lib` profiles on **{Linux, Windows, macOS} × {x86_64, ARM64}** (3 × 2 = 6 targets).
- [ ] `script` and `sproc` profiles produce valid output for tsql-subset on every host.
- [ ] CI matrix runs on all 6 target combos + ASan/UBSan on Linux Clang.
- [ ] Compile-then-run integration tests pass on every CI runner.
- [ ] Diagnostic UX renders clang-quality output (line + col + caret + actionable message) for every `P_*`/`S_*` diagnostic class.
- [ ] Compile time for a 500-LOC c-subset program is < 1 second on a modern laptop.
- [ ] Generated binary perf is within 2× of `gcc -O0` for the c-subset corpus.
- [ ] `docs/{language,project,architecture,contributing}.md` are current.
- [ ] License declared, third-party deps audited, reproducible-build sweep complete.
- [ ] Second engineer can onboard and ship a substrate PR within their first week (G-761 closure).

When all boxes are checked, this plan is closed and the project's first stable release lands.
