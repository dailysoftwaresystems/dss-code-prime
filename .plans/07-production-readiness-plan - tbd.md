# Production Readiness — Master Plan

> **Scope.** v1 ships **end-to-end binaries for every language in `src/source-config/languages/`** on **{Windows, Linux, macOS} × {x86_64, ARM64}**, gated by [`artifactProfile`](./06-artifact-profile-plan%20-%20tbd.md). v1.x extends to WASM / shader / transpile / iOS / Android via the new sub-plans (rev 2).
>
> **Rev 2 (2026-05-23) — hermetic + 3-IR + lattice-extensions.** Three architectural commitments (per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1):
> 1. **Hermetic compiler.** Own every byte source-to-binary; no external tool invocations.
> 2. **HIR → MIR → LIR.** Three IR layers, not one. ([`09-hir-plan`](./09-hir-plan%20-%20tbd.md), [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md))
> 3. **Core lattice + per-language type extensions.** ([`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md))
>
> These reframe several gap clusters; see §3 (IR — re-scoped), §5 (codegen — hermetic), §8 (new — FFI), §9 (new — transpile), §10 (new — shader/GPU), §11 (new — WASM).

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
| G-001 | **Parser driver shipped — phase #7 closed.** Schema-driven iterative RD (PA1) + Pratt walker (PA2) + panic-mode recovery + clang-style diagnostics (PA3) + real-world corpus stress (PA4) + LSP server skeleton with diagnostics (PA5a) + LSP semantic-stub method handlers (PA5b). PA-Walker-LeftRec added the left-recursive `wrapLastChildInFrame` substrate primitive. PA5a-prep closed v2-gap-catalog rows 12-declarator, 16, 29. 56 ctest suites green (post-08.55). | Largest "does this work at all" risk fully retired. See [`05-parser-plan - ok.md`](./05-parser-plan - ok.md). |
| G-002 | ✅ **Semantic phase SHIPPED (SE1–SE7).** [`08.6-semantic-plan - ok.md`](./08.6-semantic-plan%20-%20ok.md): one language-agnostic `analyze()` engine driven by a schema v4 `semantics` block; toy/c-subset/tsql onboarded purely by config. G-201..G-212 all closed (G-210 never-referenced unused-var closed 2026-05-26; write-only/dead-store remains for the optimizer). Two 5-agent review rounds. | Required for any non-trivial codegen. Type errors are the most common real-world diagnostic. |
| G-003 | **No IR.** `src/gen/intermediate/` is empty. No IR design, no SSA decision, no lowering pass.  | The hinge between frontend and backend. Wrong design here forces frontend OR backend rework. |
| G-004 | **Codegen is one Windows PE demo.** No ELF, no Mach-O, no ARM64, no IR-driven path. | v1's hardest single chunk of work. Three object formats × two arches × three runtimes (CRT / glibc / libSystem). |
| G-005 | **No project-config format.** Driver layer is unspecified; users can't actually invoke the compiler against a real project. | Without this, every other artifact has no entry point. |
| G-006 | **No compilation-unit model.** Parser produces one Tree per file; nothing bundles them. Semantic / IR / codegen would each invent their own answer. | See [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md) — phase #7.5 between parser and semantic. |

---

## 1. Frontend gaps (parser phase + beyond)

| ID    | Gap | Where it surfaces | Resolves via |
|-------|-----|-------------------|--------------|
| G-101 | Parser driver shipped (PA0–PA5b ✅, G-001 detail). Substrate, iterative RD driver, Pratt walker, recovery + diagnostic UX, real-world corpus stress, LSP server skeleton + diagnostics, and LSP semantic-stub method handlers all landed. | `src/analysis/syntactic/parser.{hpp,cpp}` + `src/lsp/*` + 56 ctest suites (post-08.55) | [`05-parser-plan - ok.md`](./05-parser-plan - ok.md) PA0–PA5b ✅ |
| G-102 | ✅ Operator precedence in the AST. Closed end-to-end in PA2 (v2-gap-catalog row 1). | `ParserCSubsetSmoke.FunctionBodyExpressionIsPrecedenceCorrect` | parser-plan PA2 ✅ |
| G-103 | ✅ Function calls `f(x, y)`. Closed by parser-plan PA4 — grouped-postfix `(` with `endsAt: ")"` + `bodyRule: argList`. Empty `f()` parses cleanly (argList is `optional`). | `ParserCSubsetSmoke.{FunctionCallParsesAsPostfix, EmptyArgumentCallParsesAsPostfix}` | parser-plan PA4 ✅ (v2-gap-catalog row 7) |
| G-104 | ✅ Arrays. Expression-side `a[0]` closed by PA4 (grouped-postfix `[`/`]` with `bodyRule: expression` routed through Pratt). Declarator-side `int a[10];` closed by PA5a-prep (`arrayDeclSuffix` shape attached to `varDeclTail`/`varDeclHead`). | `ParserCSubsetSmoke.{ArrayIndexParsesAsPostfix, ArrayIndexBodyClimbsPrecedence, TopLevelArrayDeclParses, InnerArrayDeclParses, ArrayDeclWithInitializerExpressionParses}` | parser-plan PA4 ✅ (expression) + PA5a-prep ✅ (declarator) (v2-gap-catalog row 12) |
| G-105 | Pointer ops `*p`, `&x`. **Expression side closed by parser-plan PA4** (prefix `*` → `unaryExpr`). Pointer/array *type-level* modelling (`int *p` declarator → a `Ptr`/`Array` lattice type) was explicitly NOT taken in phase #8 — c-subset uses C declarator syntax (not a named type-constructor rule), so it needs a `typeShapes`/grammar round; `&` addr-of still undeclared. Deferred to a future c-subset grammar/typeShapes round (post phase #8, not codegen-blocking for the current corpus). | c-subset.lang.json `operand` ✅; pointer/array typing ⏳ | parser-plan PA4 ✅; type-level pointer/array + `&` = future c-subset config round |
| G-106 | ✅ Postfix `x++` / `x--`. Closed by parser-plan PA4 — simple-postfix (`endsAt` absent) operator-table entries. | `ParserCSubsetSmoke.PostfixIncParsesAsPostfix` | parser-plan PA4 ✅ (v2-gap-catalog row 8) |
| G-107 | ✅ Compound assignment `+= -= *= /= %= &= \|= ^= <<= >>=`. Closed by PA5a-prep — tokens declared + added to the precedence-15 right-assoc operator group alongside `=`. | `ParserCSubsetSmoke.CompoundAssignmentParsesAsBinaryExpr` | parser-plan PA5a-prep ✅ (v2-gap-catalog row 16) |
| G-108 | Ternary `? :` — mixfix. | not-yet-tokens | **v3 candidate** — current `Prefix/Infix/Postfix` enum doesn't model mixfix (v2-gap-catalog row 17) |
| G-109 | ✅ **Float literal lexing.** **Closed by 08.55**: the `numberStyle` schema block (v4, additive) declares per-language numeric syntax. Tokenizer's `scanNumber()` is config-driven; c-subset/tsql/toy each declare their own numeric styles (or omit if not needed). v2-gap-catalog row 14 resolved. | tokenizer | — |
| G-110 | Multi-file translation units. The current model is one file → one tree. | [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md) CU1 + CU2 | ✅ **RESOLVED (CU2).** `CompilationUnit` type (CU1) + `UnitBuilder::addFile`/`addInMemory` multi-file aggregation (CU2, continue-on-failure, lexer+parser diagnostics unified per Tree, `D_*` driver codes). Semantic / IR / codegen consume CUs instead of bare Trees. (Cross-file *reference resolution* = G-111 / CU4.) |
| G-111 | `#include` / module imports. Both c-subset and tsql-subset (`USE database;`) need cross-file references. | [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md) CU4 | **Phase #7.5** — per-language `ImportResolver` populates `CrossTreeRef` edges before semantic consumes the CU. Schema-driven import syntax stays a v3 candidate; v1 dispatches resolver on language name. |
| G-112 | Preprocessor for full C (not c-subset). | n/a | **Explicitly out of v1.** c-subset omits the preprocessor by design. Full C99 is post-v1. |
| G-113 | T-SQL `GO` batch separator, `BEGIN ... END` blocks, cursors, `TRY/CATCH`. | tsql-subset.lang.json | Out of v1 if PA4 corpus doesn't need them; otherwise extend tsql-subset (mechanism exists — pure shape work per v2-gap-catalog §6 "Residual T-SQL gaps"). |
| G-114 | ✅ Error recovery quality benchmark. PA3 ✅ — bar set to "every error produces an actionable message; recovery continues without cascading beyond 3× the original error count" (pinned by `ParserRecovery.SingleErrorCascadeBoundedAtThreeX`). Real-world stress is PA4's job to validate. | `tests/analysis/syntactic/test_parser_recovery.cpp` | parser-plan PA3 ✅ |
| G-115 | ✅ Diagnostic UX end-to-end. PA3 closed the CLI-side renderer (`DiagnosticReporter::format()`, clang-style line/col/caret + multi-char `^^^` underline). PA5a closed the LSP-side translation (`src/lsp/diagnostic_translator.{hpp,cpp}` — UTF-8 byte spans → LSP `Diagnostic` with UTF-16 columns + severity mapping + composed message + code-name). Real editor sessions consume `textDocument/publishDiagnostics` via the PA5a server. | `tests/core/test_diagnostic_reporter.cpp` + `tests/lsp/test_diagnostic_translator.cpp` | parser-plan PA3 + PA5a ✅; driver-side CLI wiring still pending |
| G-118 | ✅ **Numeric-literal `flagsApplied` channel** is in place — the tokenizer routes per-kind flags through the schema's mechanism. Tracked closed as a consequence of 08.55's numberStyle plumbing. | `src/tokenizer/tokenizer.cpp` | Resolved alongside G-109. |

---

## 2. Semantic phase gaps (§8 phase #8)

> ✅ **CLOSED — phase #8 shipped (SE1–SE7, plan [`08.6-semantic-plan - ok.md`](./08.6-semantic-plan%20-%20ok.md)).** One language-agnostic `analyze()` engine driven by the `semantics` schema-v4 block produces a `SemanticModel` (symbol table + scope tree + node→SymbolId/TypeId + `S_*` diagnostics). G-201 stack-of-hashmaps scope tree ✅; G-202 scope resolution ✅; G-203 type representation = the SP2 `TypeKind`/`TypeId` lattice ✅; G-204 type checking (assignment + call arg + **return-type**, gap-closure A) ✅; G-205 const-correctness ✅; G-206 typedef ✅; G-207 function overloads + builtin functions ✅; G-208 cross-CU resolution via `crossRefs[]` injection ✅; G-209 forward refs (two-pass) ✅; G-210 unused-var: never-referenced **closed 2026-05-26** (config-driven `warnIfUnused` flag + SE7 `usesBySymbol` reverse index → `S_UnusedVariable` warning, no CFG); write-only/dead-store still deferred to optimizer; G-211 `S_*` codes ✅ (UndeclaredIdentifier/RedeclaredSymbol/TypeMismatch/NotCallable/ArgCountMismatch/UnknownType/ConstViolation/ReturnTypeMismatch/ControlOutsideLoop/**UnusedVariable**); G-212 death-test + fail-loud discipline ✅. **Pre-HIR gap-closure (2026-05-26)** added beyond the original list: return-type checking, duplicate-parameter detection, break/continue-outside-loop (`S_ControlOutsideLoop`), tsql bracket-id `[Name]` name matching, in-memory/include dedup, split `# include`. **Deferred-item closure pass (2026-05-26):** compound-assignment const-correctness (D2), tsql column-type resolution incl. a generic `builtinTypes.extension` facet (D3), `artifactProfiles` schema field + loader (D4-AP1), and the never-referenced unused-variable warning (D8). Everything config-driven (no per-language C++).

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
| G-210 | Unused-variable / dead-code analysis. | Never-referenced ✅ closed 2026-05-26 (semantic `warnIfUnused` + `usesBySymbol`). Write-only / dead-store / dead-code still deferred to the optimizer phase (need dataflow/liveness). |
| G-211 | ✅ **Closed (SE1–SE7).** `S_*` namespace shipped (0xE band, renders as `S`): `S_UndeclaredIdentifier`/`S_RedeclaredSymbol`/`S_TypeMismatch`/`S_NotCallable`/`S_ArgCountMismatch`/`S_UnknownType`/`S_ConstViolation`/`S_ReturnTypeMismatch`/`S_ControlOutsideLoop`. | Done. |
| G-212 | ✅ **Closed (SE1–SE7).** Semantic engine uses the fail-loud `*Fatal` posture (no `<cassert>`); death tests cover the cross-CU NodeId guard etc.; every recovery path emits an explicit `S_*` rather than silently degrading. | Done. |

---

## 3. IR phase gaps — re-scoped rev 2 as three-layer IR

> **Rev 2 (2026-05-23).** G-301's "default SSA over CFG" decision was wrong. A single IR cannot serve binary codegen, transpilation, WASM, and SPIR-V cleanly. Re-scoped as G-301a (HIR), G-301b (MIR), G-301c (LIR) — each with its own sub-plan.

| ID    | Gap | Notes |
|-------|-----|-------|
| G-301a | **HIR design.** Language-neutral, structured (if/while/for/switch preserved), typed, attribute side-tables. | Owned by [`09-hir-plan`](./09-hir-plan%20-%20tbd.md). The pivot layer for transpile + native lowering + shader + WASM. 🟡 HR1 ✅ 2026-05-26 (arena + node shapes + walker + ids + extension registry) + HR2 ✅ (typed expressions + operator registry + verifier) + HR3 ✅ (structured CF + break/continue & per-kind-arity verifier rules) + HR4 ✅ (declarations + extern surface + FfiMetadata side-table) + HR5 ✅ (attribute side-tables — SourceSpan/Shader/Transpile/Diagnostic + `hir_attrs.hpp` catalog; verifier emits real diagnostic spans) + HR6 ✅ (full verifier — block dead-code, return completeness, Call-arg-vs-FnSig, intrinsic-registered, shader-restriction subverifier; `HirIntrinsicRegistry`) + HR7 ✅ 2026-05-27 (round-trippable `.dsshir` text format — `emitHir`/`parseHir`, verify-on-load) + HR8 ✅ 2026-05-27 (config-driven CST→HIR lowering engine + `hirLowering` schema facet, proven on c-subset); HR9–HR11 pending. |
| G-301b | **MIR design.** SSA over CFG + structured-CF markers preserved as block annotations. | Owned by [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md). Structured-CF markers let WASM lowering skip Relooper. |
| G-301c | **LIR design.** Per-target ISA, virtual + physical registers, calling-conv lowered, stack frame materialized. | Owned by `12-mir-lir-plan` (same sub-plan, second half). Consumed by `13-assembler-plan`. |
| G-302 | IR text formats `.dsshir` / `.dssir` / `.dsslir` for debugging + golden test fixtures. | Three formats, one per layer. All round-trippable. Binary cache forms post-v1. 🟡 `.dsshir` ✅ 2026-05-27 (HR7, byte-identical round-trip + verify-on-load + golden corpus); `.dssir` / `.dsslir` pending (`12-mir-lir-plan` ML4 / ML8). |
| G-303 | Per-layer verifiers. HIR verifier (typed expr + structured CF + shader restrictions). MIR verifier (SSA dominance + structured-CF markers consistent). LIR verifier (no virtual regs after regalloc + calling-conv shape). | Fail-loud per `H_*` / `I_*` / `L_*` namespaces. |
| G-304 | CST → HIR lowering — **config-driven** (NOT per-language C++; master-plan thesis decision #4). A single language-agnostic engine (`src/hir/lowering/cst_to_hir`) reads each shipped `.lang.json`'s `hirLowering` schema block (additive facet, sibling of `semantics` / `imports` / `numberStyle` / `typeExtensions[]`) to map CST shapes → HIR; no `src/hir/lowering/<lang>_lowering.cpp` and no branch on `schema.name()`. A new language onboards by adding a `hirLowering` block, never engine edits. HIR → MIR + MIR → LIR are language-agnostic. | Per `09-hir-plan` HR8/HR9/HR10 + §2.5. 🟡 HR8 ✅ 2026-05-27 landed the engine + `hirLowering` facet (`src/hir/lowering/`), proven on c-subset; toy + tsql + c-subset's deferred constructs are HR9/HR10. |
| G-305 | Diagnostic namespaces. | `H_*` (HIR), `I_*` (MIR), `L_*` (LIR). |
| G-306 | MIR + LIR type systems. MIR uses canonical-lowered core lattice (`i1`..`i128`, `f16`..`f128`, `ptr`, `vector<T,N>`, `struct{}`, `array<T,N>`). LIR uses machine-only types. | Per `12-mir-lir-plan`. |
| G-307 | Calling convention metadata. SysV AMD64 / Microsoft x64 / AAPCS64 / Microsoft ARM64 / Apple ARM64. | Lattice members in core: `CcSysV`, `CcMS64`, `CcAAPCS64`, `CcApple`, `CcFastcall`, `CcThiscall`, `CcVectorcall`, `CcWasm`, `CcSpirv`. Per `08.5-substrate-prep-plan` SP2. |
| G-308 | Profile-driven lowering knobs. | Unchanged — `artifactProfile` flows through HIR → MIR → LIR → codegen via `CompilationContext`. |
| G-309 | **(new rev 2)** Structured-CF marker discipline through optimizer passes. Every MIR-level pass must preserve markers or explicitly invalidate them. | Verifier rejects untagged blocks after each optimizer pass in debug builds. Per `12-mir-lir-plan` §2.3. |
| G-310 | **(new rev 2)** HIR shader-shape restrictions: no recursion, no dynamic alloc, no fn-ptr, no libc, no goto, no host-pointer. | Per `17-shader-gpu-plan` §2.3. `SH_*` codes. |

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

### 5.5 In-tree linker (re-scoped rev 2: hermetic; no system linker)

> **Rev 2 (2026-05-23).** G-540's "system linker for v1" was wrong. The hermetic-compiler invariant requires owning the entire pipeline. G-540..G-543 dropped; replaced by per-format and per-platform-metadata gaps owned by [`14-linker-plan`](./14-linker-plan%20-%20tbd.md).

| ID    | Gap | Notes |
|-------|-----|-------|
| G-560 | **(new)** In-tree ELF writer (Linux x86_64 + ARM64; executables + shared libs; PLT/GOT; GNU_HASH; PT_GNU_RELRO). | `14-linker-plan` LK1. |
| G-561 | **(new)** In-tree PE/COFF writer (Windows x86_64 + ARM64; exe + DLL; subsystem flag per artifactProfile; base relocations; .idata IAT; .pdata/.xdata). | `14-linker-plan` LK2. |
| G-562 | **(new)** In-tree Mach-O writer (macOS x86_64 + ARM64; executables + dylibs; chained-fixups for Apple Silicon; LC_DYLD_INFO legacy path; LC_CODE_SIGNATURE placeholder). | `14-linker-plan` LK3. |
| G-563 | **(new)** Linker engine (symbol resolution + relocation application + section layout + per-format metadata). Format-agnostic; calls per-format writer. | `14-linker-plan` LK4. |
| G-564 | **(new)** TLS lowering per platform (Linux x86_64 `%fs:`, Linux ARM64 `tpidr_el0`, Windows TEB, macOS `_thread_vars`). | `14-linker-plan` LK5. |
| G-565 | **(new)** Dynamic linking + imports (PE IAT, ELF GOT/PLT, Mach-O bind opcodes). FFI imports from `11-ffi-plan` land here. | `14-linker-plan` LK6. |
| G-566 | **(new)** Codesign hook (Mach-O `LC_CODE_SIGNATURE` placeholder + PE attribute-cert reservation). | `14-linker-plan` LK7, filled by `16-codesign-publish-plan`. |
| G-567 | **(new — v1.x)** WASM module writer (post-v1; skeleton lands in v1). | `14-linker-plan` LK8, full impl `18-wasm-plan`. |
| G-568 | **(new — v1.x)** SPIR-V module writer (post-v1; skeleton lands in v1). | `14-linker-plan` LK9, full impl `17-shader-gpu-plan`. |
| G-569 | **(new)** End-to-end hermetic acceptance: build c-subset corpus on CI runner with no system linker installed. | `14-linker-plan` LK10. v1 acceptance gate. |
| G-570 | **(new)** Deterministic build-id: BLAKE3 of section contents → `.note.gnu.build-id` (ELF) / `LC_UUID` (Mach-O) / `IMAGE_DEBUG_DIRECTORY` (PE). | `14-linker-plan` §2.10. |
| G-571 | **(new)** In-tree assembler (x86_64 + ARM64 instruction encoding + relocation taxonomy). | Owned by [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md). |

### 5.6 In-tree codesign + publish (new rev 2)

| ID    | Gap | Notes |
|-------|-----|-------|
| G-590 | Apple Mach-O codesign (page hashes + Code Directory + SuperBlob + LC_CODE_SIGNATURE fill). | [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md) CS2. |
| G-591 | Apple notarization HTTP client + ticket stapling. | `16-codesign-publish-plan` CS3. |
| G-592 | iOS provisioning profile embedding + `.app` bundle assembler. | `16-codesign-publish-plan` CS4. |
| G-593 | Windows Authenticode (PE security directory + PKCS#7 + SpcIndirectDataContent). | `16-codesign-publish-plan` CS5. |
| G-594 | Windows RFC 3161 TSA timestamping. | `16-codesign-publish-plan` CS6. |
| G-595 | Android APK v3 signing (skeleton; post-v1 full impl). | `16-codesign-publish-plan` CS7. |
| G-596 | Vendored crypto substrate (BearSSL): SHA-256/384 + DER + X.509 + PKCS#7 + RSA/ECDSA + HTTPS. | `16-codesign-publish-plan` CS1. |
| G-597 | Apple-host-free local dev: in-tree Mach-O writer + in-tree codesign means no `xcrun` / `codesign` / `dsymutil` invocations on a non-Apple host. | Hermetic acceptance gate. |

---

## 6. Driver, project, distribution gaps (§4.1 program/)

| ID    | Gap | Notes |
|-------|-----|-------|
| G-601 | Project config file format (`.dss-project.json` or similar). | See [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) §2.2 for the proposed shape. Schema doc: `docs/project-config-spec.md` (new). |
| G-602 | `artifactProfile` mechanism end-to-end. | [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) AP1–AP4. **AP1 ✅ closed 2026-05-26** (`artifactProfiles` schema field + loader validation + `GrammarSchema::artifactProfiles()` accessor; 3 configs declare profiles). AP2–AP4 (project-config, driver enforcement, codegen) remain in phase #12. |
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
| G-711 | **Exhaustive-switch enforcement (warnings-as-errors latch).** No `-Wswitch`/`-Werror` is configured today, so the codebase-wide open-core dispatch idiom — an exhaustive `switch` over an enum with **no `default`** + a trailing "unreachable" return (`HirKind`/`TypeKind`/`HirOpKind` dispatch, `requiresValidType`, `arityOf`, `opName`, every `*Name()` / lowering switch) — is **convention-only, not compiler-enforced**. A future core enum member added without updating a switch silently returns a plausible-but-wrong value: e.g. `requiresValidType(NewExprKind)` → `false` makes the HIR verifier silently skip a genuinely untyped node; `arityOf(newBinaryOp)` → `Binary` mis-classifies. This is the one class of regression the "make every regression visible" posture currently can't see. | ⏳ pending. Surfaced by the HR2 5-agent review (silent-failure-hunter, conf 72; 2026-05-26). **Action:** enable `-Werror=switch` (GCC/Clang `-Wswitch`; MSVC `/we4062`) tree-wide as the minimum latch — ideally the stricter `-Wswitch-enum` (`/we4061`) on the open-core dispatch TUs — plus a one-time audit fixing any currently non-exhaustive switch. Cheap, high-leverage; guards every `core+extensions` dispatch site at compile time. |

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

## 8. FFI / precompiled-library ingestion (new rev 2)

> v1-blocking — without it, no libc → no useful binary. Owned by [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md).

| ID    | Gap | Notes |
|-------|-----|-------|
| G-801 | In-tree ELF reader (`.dynsym`/`.dynstr`/GNU_HASH/DT_NEEDED). | FF1. |
| G-802 | In-tree PE reader (IAT/ILT/.edata/.idata + `.lib` static archive). | FF1. |
| G-803 | In-tree Mach-O reader (LC_SYMTAB/LC_DYSYMTAB/LC_DYLD_INFO_ONLY + LC_DYLD_CHAINED_FIXUPS). | FF1. |
| G-804 | In-tree `ar` archive reader (BSD-style; libc.a / libm.a). | FF1. |
| G-805 | C header parser ("header mode" of c-subset frontend; typedef/struct/union/enum/extern + simple `#define`-only-macros). | FF2. Full preprocessor reserved post-v1 (G-810). |
| G-806 | ABI catalog per (lang × platform). Calling convention + LP64/LLP64 + struct padding + va_arg + small-aggregate-in-regs thresholds. | FF3. |
| G-807 | C name mangling (per-platform underscoring); Itanium + MSVC demangling reserved post-v1. | FF4. |
| G-808 | `ingest()` entry point + `HirAttribute<FfiMetadata>` populated on extern decls. | FF5. |
| G-809 | libc smoke: `extern printf(...)` end-to-end on all 6 (OS × arch). | FF6. |
| G-810 | (post-v1) C preprocessor for header mode (`#include` + function-like macros). | Reserved. v1 uses pre-reduced headers in `src/source-config/ffi-headers/`. |
| G-811 | (post-v1) Itanium + MSVC C++ demanglers. | FF7 / FF8. |
| G-812 | Pre-reduced headers for {libc, libSystem, msvcrt, kernel32} on each platform. | Ships under `src/source-config/ffi-headers/`. |
| G-813 | Symbol-existence pin: CI reads the actual platform libc binary and asserts every header-declared symbol is present. | Catches drift between pre-reduced headers and reality. |

---

## 9. Source-to-source translation (new rev 2)

> v1.x — first user likely c-subset → JS for a Web target. Owned by [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md).

| ID    | Gap | Notes |
|-------|-----|-------|
| G-901 | `.map.json` schema + loader. Hub-and-spoke (HIR↔language, not per-pair grammar mapping). | ST1. |
| G-902 | HIR(source) → HIR(target) walker. Type-mapping + kind-mapping + idiom hints. | ST2. |
| G-903 | HIR(target) → CST(target) builder. `emissionTemplates[]` on language schemas. | ST3. |
| G-904 | Target-schema pretty-printer with lexical conventions. | ST4. |
| G-905 | First flagship pair: c-subset → JavaScript. End-to-end correctness against host-binary stdout. | ST5. |
| G-906 | Round-trip discipline: native compile vs transpile + re-parse-target → diff. | ST6. |
| G-907 | New artifactProfile `transpile`; project config `transpileTarget` + `languagePair` fields. | `06-artifact-profile-plan` §3 already updated. |
| G-908 | `T_*` diagnostic namespace: `T_MissingKindMapping`, `T_AmbiguousMapping`, `T_KindNotMapped`, `T_ExtensionNotMapped`, `T_IdiomConflict`, `T_TargetSchemaReparse`. | |

---

## 10. Shader / GPU codegen (new rev 2)

> v1.x — lit up for the user's custom language. Owned by [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md).

| ID    | Gap | Notes |
|-------|-----|-------|
| G-1001 | HIR shader-shape lattice members in the core lattice: Vector / Matrix / Sampler / Texture / UAV / ConstantBuffer / WorkgroupShared. | SG1. Per `08.5-substrate-prep-plan` §2.2. |
| G-1002 | HIR shader-shape kinds: WorkgroupBarrier / DerivativeX/Y / TextureSample/Load / ImageStore / AtomicOp / Swizzle. | SG1. Per `09-hir-plan` §2.2 shader extensions. |
| G-1003 | HIR shader verifier with `SH_*` codes (no recursion / no dynamic alloc / no fn-ptr / no libc / no host-pointer). | SG2. |
| G-1004 | SPIR-V emitter (module header + memory model + entry points + execution modes + sections). | SG3. |
| G-1005 | SPIR-V type encoding (core lattice → OpType*). | SG4. |
| G-1006 | SPIR-V instruction lowering from MIR (with structured-CF marker re-use for OpLoopMerge / OpSelectionMerge). | SG5 + SG6. |
| G-1007 | SPIR-V decorations (Binding / DescriptorSet / Location / BuiltIn). | SG7. |
| G-1008 | Entry-point attribute parsing in HIR (`[[shader.vertex]]` / `[[shader.fragment]]` / `[[shader.compute(x,y,z)]]`). | SG8. |
| G-1009 | Same-source CPU + GPU functions (dual lowering for `[[shader.usable]] [[host.usable]]`). | `17-shader-gpu-plan` §2.4. |
| G-1010 | Reflection sidecar `.spv.json` with entry-points + bindings + push-constants. | `17-shader-gpu-plan` §2.8. |
| G-1011 | "Hello triangle" Vulkan validation-layer CI harness. | SG10. |

---

## 11. WASM backend (new rev 2)

> v1.x — web target. Owned by [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md). MUST NOT be foreclosed by IR design (structured-CF marker discipline in MIR is the bridge).

| ID    | Gap | Notes |
|-------|-----|-------|
| G-1101 | WASM module encoder (sections 1-12 + custom). | WA1–WA2. |
| G-1102 | Linear memory + Data section + i32 pointer arithmetic. | WA3. |
| G-1103 | Structured CF lowering from MIR structured-CF markers (block / loop / if / br / br_if / br_table). | WA4. |
| G-1104 | Import + export sections (FFI to JS / WASI hosts). | WA5. |
| G-1105 | Global section + start function. | WA6. |
| G-1106 | Name custom section (debug names). | WA7. |
| G-1107 | `wasm-validate`-clean output + round-trip via `wasm2wat` (oracles only). | WA8. |
| G-1108 | End-to-end "hello world" under wasmtime + Chromium V8 (oracles only). | WA9. |
| G-1109 | Deterministic byte output. | WA10. |
| G-1110 | Hermetic acceptance: no `wasm-ld` / `emscripten` invocation. | WA9. |

---

## 12. Sequencing — what unblocks what

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
                                                                                              (G-501..524) (G-560..571 in-tree)
                                                                                                          │
                                                                                                          ▼
                                                                                                  CI + corpus + perf
                                                                                                          │
                                                                                                          ▼
                                                                                                       v1 ship
```

The critical path runs through: **parser → compilation-unit → semantic → IR → codegen × 3 OS × 2 arch**. PA5 (LSP) and CU1..CU4 are **siblings** branching off PA4 — LSP doesn't depend on the CU layer (operates per-file). The artifactProfile mechanism plugs in alongside semantic (driver enforcement) and again at IR/codegen (lowering knobs). Optimizer can develop in parallel with codegen once IR is stable.

---

## 13. Post-v1 (deferred but tracked)

These are intentionally NOT v1 deliverables. Several formerly-post-v1 items moved to v1.x with rev 2; see the corresponding sub-plan for status.

- ~~LSP server / IDE integration~~ — **moved to v1** as parser-plan PA5 (diagnostics-only scaffolding); semantic-powered LSP methods post-phase-#8 in a dedicated LSP follow-up plan.
- Incremental parsing + build caching (G-605).
- Inlining + CSE + loop-invariant motion (G-404, G-405, G-406).
- Graph-coloring register allocation (G-512).
- ~~WASM target~~ — **moved to v1.x** as [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md) per rev 2.
- ~~Built-in linker~~ — **moved to v1 mandatory** as [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) per rev 2 (hermetic invariant).
- PGO / LTO.
- Full C99 (preprocessor + everything c-subset omits) as a separate language config.
- Stored-procedure deployment automation for tsql-subset's `sproc` artifactProfile.
- Differential testing (G-734).
- Cross-compilation (G-707) — partially obviated by the in-tree linker (Apple targets from non-Apple hosts).
- Static musl on Linux (G-521 alt).
- Exception handling (G-525) — reserved under [`21-runtime-reserved-plan`](./21-runtime-reserved-plan%20-%20tbd.md) §2.2.
- ~~TLS (G-516) + atomics (G-517)~~ — **TLS moved to v1** under `14-linker-plan` LK5 (initial-exec model); atomics still post-v1.
- C++ FFI demangling (Itanium + MSVC) — reserved under [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) FF7/FF8.
- iOS / Android native targets — v1.x via the in-tree linker + codesign sub-plans.
- VHDL / Verilog (HIR-HW) — reserved indefinitely; see [`19-hir-hw-reserved-plan`](./19-hir-hw-reserved-plan%20-%20tbd.md).
- User's custom language — reserved indefinitely; see [`20-custom-language-reserved-plan`](./20-custom-language-reserved-plan%20-%20tbd.md).
- Language runtime (GC / exceptions / coroutines) — reserved indefinitely; see [`21-runtime-reserved-plan`](./21-runtime-reserved-plan%20-%20tbd.md).

---

## 14. Acceptance — what "v1 production-ready" means

A PR titled "v1 — production-ready" is accepted when **every** item below is checked:

- [ ] All three shipped languages (`toy`, `c-subset`, `tsql-subset`) parse `tests/corpus/` programs end-to-end via the real parser (no hand-driven `TreeBuilder`).
- [ ] **Multi-file integration test** per language: c-subset two-file (`main.c` + `helper.h` with cross-file call), tsql-subset two-file (`schema.sql` + `data.sql` with concat ordering), toy degenerate single-file. Phase 7.5 `CompilationUnit` aggregates correctly.
- [x] **LSP server** starts via `dss-code-prime --lsp`, completes the `initialize` handshake, and publishes diagnostics in a real editor session (golden-file replay). Semantic-powered methods return empty results (lit up post-phase #8). ✅ **shipped in PA5a + PA5b** — 11 LSP test executables, 8 golden-file session replays, 56 ctest suites green (post-08.55).
- [ ] `artifactProfile` is enforced: a project asking for an unsupported profile gets `D_ArtifactProfileNotSupported` at config-load.
- [ ] Codegen produces working binaries for `cli` and `lib` profiles on **{Linux, Windows, macOS} × {x86_64, ARM64}** (3 × 2 = 6 targets).
- [ ] `script` and `sproc` profiles produce valid output for tsql-subset on every host.
- [ ] CI matrix runs on all 6 target combos + ASan/UBSan on Linux Clang.
- [ ] Compile-then-run integration tests pass on every CI runner.
- [ ] Diagnostic UX renders clang-quality output for every `P_*`/`S_*`/`H_*`/`I_*`/`L_*`/`K_*`/`F_*`/`A_*`/`B_*`/`G_*` namespace.
- [ ] Compile time for a 500-LOC c-subset program is < 1 second on a modern laptop.
- [ ] Generated binary perf is within 2× of `gcc -O0` for the c-subset corpus.
- [ ] `docs/{language,project,architecture,contributing}.md` are current.
- [ ] License declared, third-party deps audited (BearSSL vendored for codesign), reproducible-build sweep complete.
- [ ] **Hermetic acceptance (rev 2):** building the c-subset corpus on a CI runner with **no `ld` / `link.exe` / `ld64` / `lld` / `wasm-ld` / `clang` / `gcc` / `as` / `xcrun` / `codesign` / `signtool` / `dxc` / `dsymutil` / `mspdb` / `emscripten` / `wasm-ld` installed** produces a working signed binary on every target.
- [ ] Second engineer can onboard and ship a substrate PR within their first week (G-761 closure).

When all boxes are checked, this plan is closed and the project's first stable release lands.

---

## 15. Rev 2 quick index (added 2026-05-23)

New gap clusters from the universal-compiler decisions:

| Cluster | Section | Owning sub-plan |
|---|---|---|
| HIR + MIR + LIR (replaces single-SSA G-301) | §3 G-301a/b/c, G-309, G-310 | [`09-hir-plan`](./09-hir-plan%20-%20tbd.md), [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md) |
| Core type lattice + extensions | §3 G-307 | [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) |
| In-tree linker (replaces system-linker G-540..G-543) | §5.5 G-560..G-571 | [`14-linker-plan`](./14-linker-plan%20-%20tbd.md), [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) |
| In-tree codesign + publish | §5.6 G-590..G-597 | [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md) |
| FFI ingestion | §8 G-801..G-813 | [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) |
| Source translation | §9 G-901..G-908 | [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md) |
| Shader / GPU | §10 G-1001..G-1011 | [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) |
| WASM | §11 G-1101..G-1110 | [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md) |
| Debug info (in-tree DWARF + PDB) | (existing §5.4 + new sub-plan) | [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md) |
