# C-Subset → Full C (ISO C23) — Sub-Plan (stepper **V2-4.X**)

> Upgrade the shipped **`c-subset`** language to **full C (ISO/IEC 9899:2024 / C23)** — across the WHOLE pipeline: grammar, tokenizer, syntactic + semantic analysis, the type system, HIR/MIR/LIR IRs, the ABI/calling-convention layer, the assembler/linker, the corpus runners, AND a complete C preprocessor. This is a **multi-month arc of many `/dss-cycle` cycles**, decomposed below so **every phase ships a green, runnable increment** (never a long-lived red branch).
>
> **Target = "C23 as mainstream compilers implement it."** The latest standard, in full — but a handful of genuinely-exotic C23 corners (`<threads.h>`, `<stdatomic.h>`, `<complex.h>`, `#embed`, arbitrary-width `_BitInt(N)`, VLAs) are **named, pinned deferrals** (§3.2), not silently dropped and not on the critical path.
>
> **Non-negotiable bar (unchanged):** source/target/linker AGNOSTIC (config-driven vocabulary, never `if (lang/arch/format == …)` in shared substrate); best-long-term / no workarounds; the hard part of each phase lands that phase; fail-loud; strict + **positioned-diagnostic** tests (leveraging the V2-4 `D-DIAG` harness) + a runnable corpus per phase (§A.5).

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **PLANNED 2026-06-10** (this plan created via `/feature-dev` with 4 user §B scope decisions — see §0.2). Stepper row **V2-4.X**, between V2-4 (program-api CLI ✅) and V2-5 (RISC-V). NOT started. |
| Scope         | **Full C23 language + full preprocessor + aggregate ABI on ALL current targets** (x86_64 + arm64 × PE/ELF/Mach-O). Decomposed into 5 clusters / ~17 cycle-phases (FC1–FC17, §0.1). Closes ~15 deferrals (§3.1). Exotic long-tail = named exclusions (§3.2). |
| Predecessors  | V2-4 (`D-DIAG-CLI-POSITION-RENDER-AND-ASSERT` ✅) — the positioned-diagnostic + `expectDiagnostics` corpus harness is REUSED to assert the many new C error classes. The rich existing `c-subset` grammar (struct/union/enum/member-access/designated-init/for/switch all already have rules) is the FRONT-END base. |
| Successors    | V2-5 (RISC-V) reuses the aggregate ABI substrate (FC9–FC12) when adding a 4th calling convention. The full-C corpus becomes the agnosticism acid test for every later target/format. |
| Key finding   | The **front-end grammar is surprisingly complete**; the type lattice already has `Struct`/`Union`/`Enum`/`Ptr`/`FnPtr`/`FnSig`/`Array` kinds; MIR is aggregate-capable (`InsertValue`/`ExtractValue`/`Alloca`); asm/link are feature-agnostic. **The real work is the BACKEND: a struct-LAYOUT module (offsets/align/padding), aggregate member-access codegen, and the aggregate+variadic ABI per target — plus the missing front-end pieces (`%`, casts, width types, `goto`, multi-declarator) and the entire preprocessor.** |

---

## 0.1 Phase breakdown — the FC* cycles by cluster

Each FC phase is one or more `/dss-cycle` cycles ending at a green push with a runnable corpus + strict/positioned tests. Status legend: ⏳ planned · 🟡 in progress · ✅ done.

### Cluster A — Front-end completeness *(cheap config-driven wins first: de-risk, close quick deferrals, unblock an authentic corpus)*
| Phase | What | Closes |
|---|---|---|
| **FC1** ⏳ | `%` modulo (the token is literally absent today) + full `idiv`/`RDX:RAX` div/mod codegen; hex-float literals; binary literals (`0b…`) + digit separators (`'`) | `D-CSUBSET-DIVISION-OP-CODEGEN`, `D-CSUBSET-MOD-OP-CODEGEN`, `D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT`, `D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC`, `D-CSUBSET-UDIV-RUNTIME-HIGH-BIT-PIN` |
| **FC2** ⏳ | C-style casts `(type)expr` as a distinct construct (today only compound-literal `(T){…}`) | — |
| **FC3** ⏳ | Width/signedness type system: `unsigned`/`signed`/`short`/`long long`/`_Bool`+`bool`/`true`/`false`; integer conversion-rank + the usual-arithmetic-conversions; `<stdint.h>` exact-width aliases; `float` as a type distinct from `double` | `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH` |
| **FC4** ⏳ | Declaration completeness: multiple declarators (`int x, y;`), `register`/`auto`, qualifiers `volatile`/`restrict`, `__attribute__`/`[[…]]` string-arg forms; the declaration-specifier-prefix-strip shared helper | `D-CSUBSET-LINKAGE-VISIBILITY-SYNTAX`, `D-DECL-PREFIX-STRIP-SHARED-HELPER` |
| **FC5** ⏳ | `goto`/labels; truly-empty statements; comma-operator-as-sequence; implicit `return 0` for `main` end-to-end | `D-LK10-ENTRY-MAIN-IMPLICIT-RETURN` |

### Cluster B — Aggregate backend *(the gating engine work)*
| Phase | What | Closes |
|---|---|---|
| **FC6** ⏳ | **`type_layout` module** (`src/…/type_layout.{hpp,cpp}`): struct/union/array field offsets + alignment + padding + total size, config-driven per ABI. The substrate every aggregate feature depends on. | — (substrate) |
| **FC7** ⏳ | Member-access codegen (`pt.x`/`pp->x` → GEP+offset in `hir_to_mir` `lowerMemberAccess`); by-value aggregate locals (`alloca`+layout); aggregate + designated initializers → memory stores; runnable struct corpus | — |
| **FC8** ⏳ | Enums end-to-end (values + enum-typed exprs); bitfields; non-string `const` aggregate globals in rodata | `D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY` |

### Cluster C — Aggregate ABI *(the biggest; "all current targets now" — per user §B)*
| Phase | What | Closes |
|---|---|---|
| **FC9** ⏳ | x86_64 **SysV** aggregate ABI: struct-by-value args (classify INTEGER/SSE/MEMORY), struct return (small in regs / large via hidden pointer); ELF + Mach-O runnable corpus | — |
| **FC10** ⏳ | x86_64 **Win64** aggregate ABI: aggregates >8B passed by address, hidden-pointer return + shadow space; PE runnable corpus | — |
| **FC11** ⏳ | arm64 **AAPCS64** + **Apple** aggregate ABI: HFA/HVA in SIMD regs, by-address, indirect-result-location; ELF + Mach-O arm64 corpus (qemu + macos-latest) | `D-FF3-APPLE-ARM64-ABI-DIVERGENCE` + arm64 ABI residue |
| **FC12** ⏳ | Variadic ABI: `<stdarg.h>` `va_list`/`va_start`/`va_arg`/`va_end` per target; variadic call lowering (stack-based SysV vs register+shadow Win64 vs AAPCS64-always-stack); real `printf`-family variadic FFI; multi-format extern-call dispatch parity | `D-FFI-DESCRIPTOR-VARIADIC-SIGNATURE`, `D-FFI-EXTERN-CALL-DISPATCH` (+ its trigger) |

### Cluster D — Preprocessor *(PARALLEL/INTERLEAVABLE track — user §B; upstream of parsing, no backend dependency)*
| Phase | What | Closes |
|---|---|---|
| **FC13** ⏳ | Preprocessing substrate (C translation phases 3–4): object + function-like macro definition/expansion; `#include` textual expansion. **⚠ §B DESIGN FORK at cycle-open:** the preprocessor MUST stay agnostic — a config-SELECTED/DECLARED pass (the `.lang.json` opts in + declares directive + macro syntax; the engine is a generic token-stream rewriter), NOT an `if (lang=="c")` in shared substrate. Bring the design options to a plan-lock §B before building. | — (new substrate; see §B at FC13) |
| **FC14** ⏳ | Conditional compilation: `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif` + the `#if` integer-constant-expression evaluator + the `defined()` operator | — |
| **FC15** ⏳ | Token-paste `##`, stringize `#`, predefined macros (`__FILE__`/`__LINE__`/`__STDC__`/`__STDC_VERSION__`…), `#pragma` (recognized, mostly no-op), `__has_include`/`__has_c_attribute` | — |

### Cluster E — C23 modern features + conformance *(caps the arc)*
| Phase | What | Closes |
|---|---|---|
| **FC16** ⏳ | C23 surface: `[[attributes]]`, `typeof`/`typeof_unqual`, `constexpr`, `nullptr`/`nullptr_t`, `static_assert` keyword, enums with explicit underlying type, `bool`/`true`/`false` as keywords (if not already in FC3) | — |
| **FC17** ⏳ | A broad runnable C23 corpus (real-ish programs across all targets) + positioned-diagnostic assertions for the new error classes (REUSES the V2-4 Part C `expectDiagnostics` harness); grows the golden diagnostic corpus | `D-DIAG-CORPUS-EVERY-CODE` (grows toward closure) |

---

## 0.2 Scope & design decisions (the user §B answers, 2026-06-10)

Captured here so they are not re-litigated each cycle:

1. **C standard = C23, "as mainstream compilers implement it"** (user: "the most complete/modern one, isn't it C23?"). The full language; the exotic long-tail (§3.2) is pinned-deferred, not blocking.
2. **Full preprocessor IS in scope** (Cluster D) — a complete macro + conditional-compilation + include-expansion pass, NOT a basic subset.
3. **Aggregate ABI on ALL current targets** (x86_64 + arm64 × PE/ELF/Mach-O) within V2-4.X — not x86_64-first. The widest, most complete ABI effort (Cluster C, FC9–FC12).
4. **Recorded as this dedicated `plan-23` + a `V2-4.X` row in plan-00 §0.1** (not folded into existing plans).
5. **Preprocessor ordering = PARALLEL/INTERLEAVABLE track** (Cluster D is upstream of parsing and independent of the type/ABI backend; pick per-cycle whether D or B/C advances — D is NOT a hard prerequisite of B/C, and B/C are NOT prerequisites of D). It can move earlier when an authentic real-header corpus is wanted sooner.

**Open §B for a future cycle (do NOT resolve now):** the preprocessor's AGNOSTICISM model (FC13) — config-selected generic pass vs config-declared macro/directive vocabulary vs a named config-selected C-specific component. Routes through a plan-lock §B at FC13-open.

---

## 1. Motivation & current state

`c-subset` today already RUNS real programs (factorial, fibonacci, `puts("hello")`, cross-CU linkage) on 3 OSes × 2 arches. Its grammar is rich — but it is a *subset*: a program using `%`, `unsigned`, a C-style cast, `goto`, a struct passed by value, a variadic call, or a `#define` either fails to parse or fails to codegen. "Full C" means a developer can take an ordinary C23 translation unit and DSS compiles + links + runs it.

The exploration (2026-06-10, three code-explorers) established the shape of the work: the front-end is mostly there, the **backend (aggregate layout + member-offset codegen + aggregate/variadic ABI) is the bulk**, the **preprocessor is wholly absent**, and a band of **front-end gaps** (`%`, casts, widths, `goto`, multi-declarator) are cheap. Hence the ordering: cheap front-end wins first (Cluster A), then the gating layout substrate (B), then the large per-target ABI (C), with the preprocessor (D) as a parallel track and C23 polish + conformance (E) last.

---

## 2. The clusters (detail)

### 2.A — Front-end completeness (FC1–FC5)
Mostly config-driven (grammar tokens/rules + semantic + HIR-lowering config), with codegen for `%`/div-mod and the width-aware type lattice as the real engine pieces. Each phase: grammar + semantic + lowering + a runnable corpus arm + positioned-diagnostic tests for the new error cases. FC1 is the natural opener (it closes 5 division/mod anchors and is high-value/low-risk).

### 2.B — Aggregate backend (FC6–FC8)
**FC6 (`type_layout`) is the gating substrate** — struct/union/array offset/alignment/padding/size, config-driven per ABI, fail-loud on unsupported shapes. FC7 threads it into `hir_to_mir` member-access + alloca + initializer lowering (MIR already has `InsertValue`/`ExtractValue`/`Alloca`). FC8 finishes enums/bitfields/`const` aggregate rodata. The type system needs only a struct-layout **side-table** (`ArenaAttribute<TypeId, StructLayout>`), not a core-lattice redesign.

### 2.C — Aggregate ABI (FC9–FC12) — the heaviest
The per-calling-convention struct-by-value + variadic lowering in `lir_callconv.cpp` / `mir_to_lir.cpp::lowerCall`. Each FC ships a runnable corpus on its target(s) with the §A.5 cross-target runtime-closure discipline (native CI leg green, not just byte-pins). SysV (FC9) → Win64 (FC10) → AAPCS64+Apple (FC11) → variadic (FC12, interacts with all three). This is where "all targets now" is realized.

### 2.D — Preprocessor (FC13–FC15) — parallel track
A token-stream pass between tokenization and parsing. **Agnosticism is the crux** (FC13 §B): it must be a config-selected/declared mechanism, never a hardcoded language branch in shared substrate. Develop independently of B/C; can interleave or front-load.

### 2.E — C23 modern + conformance (FC16–FC17)
`[[attributes]]`, `typeof`, `constexpr`, `nullptr`, etc. (mostly grammar + semantic), then a broad C23 corpus + the positioned-diagnostic conformance sweep that grows `D-DIAG-CORPUS-EVERY-CODE`.

---

## 3. Deferrals

### 3.1 — Deferrals this arc CLOSES (~15)
`D-CSUBSET-DIVISION-OP-CODEGEN`, `D-CSUBSET-MOD-OP-CODEGEN`, `D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT`, `D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC`, `D-CSUBSET-UDIV-RUNTIME-HIGH-BIT-PIN` (FC1); `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH` (FC3); `D-CSUBSET-LINKAGE-VISIBILITY-SYNTAX`, `D-DECL-PREFIX-STRIP-SHARED-HELPER` (FC4); `D-LK10-ENTRY-MAIN-IMPLICIT-RETURN` (FC5); `D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY` (FC8); `D-FF3-APPLE-ARM64-ABI-DIVERGENCE` (FC11); `D-FFI-DESCRIPTOR-VARIADIC-SIGNATURE`, `D-FFI-EXTERN-CALL-DISPATCH` (FC12); `D-DIAG-CORPUS-EVERY-CODE` grows toward closure (FC17). Several ARM64/multi-format codegen anchors (`D-LK10-ENTRY-ARM64-*`) have triggers that FIRE as the full-C corpus exercises them — closed opportunistically inside the relevant FC.

### 3.2 — Named exclusions (pinned, NON-blocking, NOT in the V2-4.X critical path)
Each is honest scope-bounding for "C23 as mainstream-implemented," to be its own future trigger-gated work:
- **VLAs** (`int a[n]`) — today fail-loud (`S_NonConstantArrayLength`); a permissive runtime-length arm is deferred.
- **`_Complex` / `<complex.h>`**, **`_Atomic` / `<stdatomic.h>`**, **`<threads.h>`** — large library + type-system features.
- **`setjmp`/`longjmp`** (non-local control flow), **inline assembly**, **`<tgmath.h>`** (type-generic math).
- **C23 `#embed`**, **arbitrary-width `_BitInt(N)`** beyond the standard integer widths, **`<stdbit.h>`** niche bits.

> These will be pinned as individual `D-FULLC-*` registry rows as each cluster lands (per §F of the cycle discipline), so they are tracked, not forgotten.

---

## 4. Cross-plan relationships

- **Grammar/schema** ([02](./02-schema-expressiveness-v2-plan%20-%20ok.md), `src/dss-config/sources/c-subset.lang.json`): Cluster A + the FC16 C23 syntax.
- **Semantic** ([08.6](./08.6-semantic-plan%20-%20ok.md)): width/rank rules (FC3), member lookup (FC7), preprocessor symbol interplay.
- **HIR** ([09](./09-hir-plan%20-%20ok.md)) + **MIR/LIR** ([12](./12-mir-lir-plan%20-%20ok.md)): the `type_layout` substrate (FC6), member-access lowering (FC7), the aggregate ABI in `lir_callconv` (FC9–FC12).
- **FFI** ([11](./11-ffi-plan%20-%20tbd.md)): variadic descriptors + multi-format extern-call dispatch (FC12).
- **Assembler** ([13](./13-assembler-plan%20-%20tbd.md)) + **Linker** ([14](./14-linker-plan%20-%20tbd.md)): div/mod encoding (FC1), rodata aggregates (FC8), entry-point implicit return (FC5).
- **Preprocessor** — a NEW concern; closest existing home is the tokenizer/source-translation plans ([04](./04-tokenizer-plan%20-%20ok.md), [10](./10-source-translation-plan%20-%20tbd.md)). FC13's §B decides whether it gets its own plan section.
- **Diagnostics** — the V2-4 `D-DIAG-CLI-POSITION-RENDER-AND-ASSERT` harness (`tests/analysis/test_diagnostic_corpus.cpp` + the `expectDiagnostics` corpus field) is REUSED throughout for the new error classes.

---

## 5. Acceptance

V2-4.X is "done" when an ordinary C23 translation unit (using structs-by-value, variadics, macros, conditional compilation, the full type system) **compiles → links → runs** on all current targets (x86_64 + arm64 × PE/ELF/Mach-O), proven by a broad runnable corpus (the §A.5 cross-target runtime-closure discipline on every native CI leg) + strict positioned-diagnostic assertions for the error classes + the named-exclusion deferrals pinned. Each FC phase is independently green-and-shippable; the arc never sits on a long red branch.
