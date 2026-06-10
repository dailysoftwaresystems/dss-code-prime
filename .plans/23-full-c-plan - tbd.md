# C-Subset → Full C (ISO C23) — Sub-Plan (stepper **V2-4.X**)

> Upgrade the shipped **`c-subset`** language to **full C (ISO/IEC 9899:2024 / C23)** — across the WHOLE pipeline: grammar, tokenizer, syntactic + semantic analysis, the type system, HIR/MIR/LIR IRs, the ABI/calling-convention layer, the assembler/linker, the corpus runners, AND a complete C preprocessor. This is a **multi-month arc of many `/dss-cycle` cycles**, decomposed below so **every phase ships a green, runnable increment** (never a long-lived red branch).
>
> **Target = "C23 as mainstream compilers implement it."** The latest standard, in full — but a handful of genuinely-exotic C23 corners (`<threads.h>`, `<stdatomic.h>`, `<complex.h>`, `#embed`, arbitrary-width `_BitInt(N)`, VLAs, `long double` format divergence) are **named, pinned deferrals** (§3.2), not silently dropped and not on the critical path.
>
> **Non-negotiable bar (unchanged):** source/target/linker AGNOSTIC (config-driven vocabulary, never `if (lang/arch/format/cc == …)` in shared substrate — `src/{opt,mir,hir,lir,core}` + the parser/semantic engines); best-long-term / no workarounds; the hard part of each phase lands that phase; fail-loud; strict + **positioned-diagnostic** tests (leveraging the V2-4 `D-DIAG` harness) + a runnable corpus per phase with §A.5 **cross-target runtime closure** (the binary EXECUTES on each target's native CI leg — byte-pins are NOT runtime proof).
>
> **THE AGNOSTICISM CRUX (read before any backend phase).** Three phase-clusters carry per-target/per-format work and MUST honour the sanctioned-tier rule: per-CC/per-format logic lives ONLY in the realization tier (`src/asm` encoder, `src/link/format/*.cpp`, `src/lir/lir_callconv.cpp` / `mir_to_lir::lowerCall`), is **PARAMETERIZED by declared config** (`.target.json` / the ABI catalog / `.format.json`), and NEVER becomes an identity branch in shared substrate. "Config-driven" here means **parameters-in-config + a bounded per-CC/per-format ALGORITHM in the realization tier** — NOT a fiction that the whole algorithm is data walked by one generic engine (SysV eightbyte classification + AAPCS64 HFA detection do not reduce to a flat table). Every backend FC (FC6, FC9–FC12c) opens with this distinction pinned, and FC9 + FC13 carry an explicit plan-lock **§B** on "what is data vs what is per-CC/per-format code."

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟡 **IN PROGRESS — FC1 ✅ COMPLETE 2026-06-10** (cycle 1: binary `%` end-to-end x86_64+arm64 + role contract + capability-driven div/mod; cycle 2: C23 hex-floats + decimal `1.`/`.5` edge forms + decode de-hardcoding — zero deferrals left in FC1). Next: FC2 (C-style casts). Plan rev. after a strict 3-reviewer plan-lock (agnosticism, slicing, accuracy). Stepper row **V2-4.X**, between V2-4 (program-api CLI ✅) and V2-5 (RISC-V). |
| Scope         | **Full C23 language + full preprocessor + aggregate ABI on ALL current targets** (x86_64 + arm64 × PE/ELF/Mach-O). Decomposed into 5 clusters / ~19 cycle-phases (FC1–FC18, §0.1) so each ships green+runnable. Closes ~12 OPEN deferrals (§3.1). Exotic long-tail = named exclusions (§3.2). |
| Predecessors  | V2-4 (`D-DIAG-CLI-POSITION-RENDER-AND-ASSERT` ✅) — the positioned-diagnostic + `expectDiagnostics` corpus harness is REUSED to assert the many new C error classes. The rich existing `c-subset` grammar (struct/union/enum/member-access/designated-init/for/switch all already have rules) is the FRONT-END base. **`D-FF3-1`** (plan-11 FF3 — the per-target ABI *layout* TargetSchema extension: struct padding + integer/pointer sizes + va_arg, trigger "first non-LP64 target") is the upstream substrate that FC6 + FC12 realize. |
| Successors    | V2-5 (RISC-V) reuses the aggregate ABI substrate (FC9–FC12c) when adding a 4th calling convention. The full-C corpus becomes the agnosticism acid test for every later target/format. |
| Key finding   | The **front-end grammar is mostly complete** (struct/union/enum/member-access/designated-init/typedef/const/multi-level-pointers/variadic/full-control-flow all already have rules) — **but NOT function pointers** (the type lattice has the `FnPtr` *kind*, but no `(*fp)(args)` declarator grammar exists; that is a front-end GAP, see FC4). The type lattice already has `Struct`/`Union`/`Enum`/`Ptr`/`FnPtr`/`FnSig`/`Array` kinds; MIR is aggregate-capable (`Alloca`/`Gep`/`ExtractValue`/`InsertValue` opcodes exist); asm/link are structurally feature-agnostic (opaque bytes + opaque relocation tags). **The real work is the BACKEND: a struct-LAYOUT substrate (offsets/align/padding, per-ABI, the `D-FF3-1` realization), aggregate member-access codegen, and the aggregate+variadic ABI per target — plus the missing front-end pieces (`%`, casts, the usual-arithmetic-conversions engine, width types, function-pointer declarators, `goto`, multi-declarator) and the entire preprocessor.** |

---

## 0.1 Phase breakdown — the FC* cycles by cluster

Each FC phase is one or more `/dss-cycle` cycles ending at a green push with a runnable corpus + strict/positioned tests. Every FC row carries the implicit **per-FC agnosticism acceptance gate** (§5): no new `if (lang/arch/format/cc.name == …)` in shared substrate; new per-CC/per-format logic lives only in the sanctioned realization tier, config-parameterized. Status: ⏳ planned · 🟡 in progress · ✅ done.

### Cluster A — Front-end completeness *(cheap config-driven wins first: de-risk, close quick deferrals, unblock an authentic corpus)*
| Phase | What | Closes |
|---|---|---|
| **FC1** 🟡 | **Cycle 1 ✅ 2026-06-10 (the first full-C cycle):** binary `%` end-to-end on x86_64 (PE+ELF) **and arm64 (ELF+Mach-O)** — c-subset `%` token/operator/binaryOps (config-only; `%=` newly live); the **role-tagged projection contract** (`inputRoles`/`outputRoles` maps on `implicitRegisters` — BOTH sides per the plan-lock; loader-validated registered vocabulary; the lowering projects BY ROLE, positional indexing gone; red-on-disable levers via the schema-mutation substrate); the **capability-driven div/mod realization** (`lowerDivLike` rules: native verb opcode → implicit-register pair → generic rem = n−(n/d)·d expansion → fail-loud; arm64 gains `sdiv`/`udiv` + `neg` (surfaced by the corpus' negative literals) as pure JSON; all four SDiv/UDiv/SMod/UMod wired, UMod MIR-tier-tested); ConstFold SMod truncated semantics + the **INT64_MIN/-1 Overflow guard on BOTH Div+Rem** (fixed the pre-existing latent host-UB SDiv fold bug); the C23 `'` digit separator (config flip `_`→`'` + the universal **flanked-by-digits scanner rule**); 0b literal pins; `examples/c-subset/modulo` (4 target arms, truncated-vs-floored runtime discriminator, exit 42 — PE local + WSL x86_64 + qemu arm64 witnessed; Mach-O arm64 = macos-latest leg) + `float_mod_error` (positioned `%`-on-float fail-loud) + the division corpus gains 3 cross-target arms. 217/217 ctest. **Cycle 2 ✅ 2026-06-10 — FC1 COMPLETE (zero deferrals left):** C23 **hex-float literals** (`0x1.8p3`) via a per-prefix `float` continuation on `NumberPrefix` (`exponent` letters/sign + its OWN `exponentDigits` class — hex mantissa, DECIMAL exponent; exponent REQUIRED, committed-incomplete → ONE malformed token, never a silent split; entry gate admits `.` after a floating prefix so `0x.8p3` lexes) + **C23 decimal fractional-constant edge forms** closed IN-CYCLE per the user's no-deferrals directive (`trailingFraction`/`leadingFraction` opt-ins: `1.` and `.5` are floats in c-subset; default-false keeps range-operator languages split) + **decode de-hardcoding**: `decodeFloat` hoisted to `number_decode.hpp` and fixed (the old strip-every-'f' corrupted hex mantissas — `0x1.fp3` 15.5→8.0, red-on-disable demonstrated); `decodeInteger` prefix-radix now config-driven (the 0x/0b/0o/0 hardcode silently returned 0 for `$ff`), trailing-suffix-first strip (high-radix digit/suffix collision), a-z digit map (radix>16 was silently wrong). Loader: shared exponent parser (one source, unknown-key rejects on BOTH exponent objects), float-block validation incl. the exponent-letter-inside-digit-class dead-config reject. Engine-genericity pins on the synthetic schema (`$`+`^`+binary exponent digits; `%`+`@`+sign-not-optional+octal). NO runtime corpus — float VALUES are runtime-unobservable today (returns reject semantically; casts = FC2; FCmp has no LIR lowering — empirically probed); the value proof lives at the exact-double decode tier per the §A.5 carve-out, and **FC2 owns the first float runtime corpus** (see FC2 row). 218/218 ctest. | Cycle 1 CLOSED: `D-CSUBSET-MOD-OP-CODEGEN` ✅, `D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT` ✅, `D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC` ✅ (by capability-realization; the one-op/pre-coloring variant re-homed under `D-ML7-2.5`); NEW `D-LIR-MOD-MSUB-FUSION` (normal, peephole). Cycle 2: no anchors opened or closed — the would-be decimal-edge deferral was closed in-cycle. *(NOTE: `D-CSUBSET-DIVISION-OP-CODEGEN` was ALREADY CLOSED 2026-06-04 — cycle 1 added modulo, not division.)* |
| **FC2** ⏳ | C-style casts `(type)expr` as a distinct construct (today only compound-literal `(T){…}`). **Owns the FIRST float runtime corpus** (FC1c2-probed: float values are runtime-unobservable until casts exist — returns reject semantically, FCmp has no LIR lowering — so `(int)(0x1.8p3 + 0.25)`-style exit-code witnesses land HERE, closing the hex-float + decimal-float value loop at runtime; the FCmp LIR lowering gap surfaces with it if the corpus prefers comparisons). | — |
| **FC3** ⏳ | Width/signedness type system: `unsigned`/`signed`/`short`/`long long`/`_Bool`+`bool`/`true`/`false`; `float` distinct from `double`. **The hard part in-phase = the usual-arithmetic-conversions (UAC) engine** (integer promotion, unsigned-preserving rank rules, sign-extend-vs-zero-extend codegen on loads/casts/div) — today only widening ranks exist (`type_rules.hpp`), NOT the value-correct conversion lattice. **The width anchor closes via a `dataModel` mechanism, NOT a language-config constant**: `int`/`long`/`size_t`/pointer width resolves per the ACTIVE TARGET's data model (ILP32/LP64/LLP64) — the registry-prescribed `SemanticConfig` `dataModel` axis (or HIR→MIR ABI-conditioning), so the same source compiles to LP64 AND Win64-LLP64. Folds the 6 width-divergent shipped-descriptor symbols (`fseek`/`ftell`/`atol`/`strtol`/`strtoul`/`labs`). Closes the unsigned-div high-bit pin (moved here — unsigned now exists). RED-on-disable: same source → two targets → `long` is I32 (Win64) vs I64 (SysV); `-1 > 0u` is true. *(FC1c2 note: c-subset `floatSuffixes` lacks `l`/`L` — long-double suffixes land here with the `float`-vs-`double` split; `1.5L` today accidentally rides the integer-suffix fallback.)* | `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH`, `D-CSUBSET-UDIV-RUNTIME-HIGH-BIT-PIN` (moved from FC1) |
| **FC4** ⏳ | Declaration completeness: **function-pointer declarators** (`int (*fp)(int)` + fn-ptr typedef — a real GRAMMAR gap today, needs declarator-stacking); multiple declarators (`int x, y;`); `register`/`auto`; qualifiers `volatile`/`restrict` (PARSING here; `volatile`-honoring codegen lands in FC16); `__attribute__`/`[[…]]` **syntax** (semantics in FC16/FC17); the declaration-specifier-prefix-strip shared helper | `D-CSUBSET-LINKAGE-VISIBILITY-SYNTAX`, `D-DECL-PREFIX-STRIP-SHARED-HELPER` |
| **FC5** ⏳ | `goto`/labels; truly-empty statements; comma-operator-as-sequence. **§A.5:** corpus runs on each target's CI leg. *(NOTE: implicit `return 0` for `main` is ALREADY CLOSED 2026-06-02 — live in `c-subset.lang.json`; the OPEN successor is implicit-return for a NON-main entry name → `entryFunctionNames` split, optional in this phase.)* | — |

### Cluster B — Aggregate backend *(the gating engine work — realizes `D-FF3-1`)*
| Phase | What | Closes |
|---|---|---|
| **FC6** ⏳ | The struct/union/array **layout substrate**: field offsets + alignment + padding + total size + flexible-array-member handling. **AGNOSTICISM LOCUS (the C2 fix):** the per-ABI layout PARAMETERS (alignment rules, `long`/pointer width, padding/packing rules, bitfield allocation-unit rules) are **declared in `.target.json`** (the `D-FF3-1` cross-tier TargetSchema extension); a generic `type_layout` engine READS them and computes layout into an `ArenaAttribute<TypeId, StructLayout>` **side-table** (NOT in the interned `TypeRecord` — layout is target/ABI-dependent, so it must not bloat a language-level interned type; this is the codebase's established side-table pattern, the best-long-term choice). Fail-loud on a target that doesn't declare its layout params. | `D-FF3-1` (the layout half; va_arg half by FC12) |
| **FC7** ⏳ | Member-access codegen (`pt.x`/`pp->x` → `Gep`+offset in `hir_to_mir` `lowerMemberAccess`); by-value aggregate locals (`Alloca`+layout); aggregate + designated initializers → memory stores; runnable struct corpus | — |
| **FC8** ⏳ | Enums end-to-end (values + enum-typed exprs); **bitfields** (their packing rules are part of the FC6 per-ABI parameter set — MS vs SysV bitfield allocation differs, declared in `.target.json`, never hardcoded); non-string `const` aggregate globals in rodata. **§A.5:** exit-code/data-read corpus on x86_64+arm64 ELF + arm64 Mach-O (the rodata-aggregate-on-arm64 high-VA-reloc territory of `D-LK6-AARCH64-ADDABSLO12-HIGH-VA`). | `D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY` |

### Cluster C — Aggregate ABI *(the heaviest; "all current targets now" — user §B. Each FC = the COMPLETE ABI for ONE calling convention, runtime-closed on its native CI leg.)*
> **FC9 opens with a plan-lock §B** (parallel to FC13's) locking the parameters-vs-algorithm boundary for ALL of cluster C: the new config fields on `TargetCallingConvention`/the ABI catalog (e.g. `aggregateMemoryThresholdBytes`, `maxAggregateRegs`, `hfaMaxMembers`, `structReturnByAddress`, `indirectResultRegister`) vs the bounded per-CC classification ALGORITHM in `lir_callconv.cpp`/`mir_to_lir::lowerCall`. **Cluster C assumes FC3's width/signedness types are shipped** (struct field widths feed layout/ABI) — do NOT reorder C ahead of A.

| Phase | What | Closes |
|---|---|---|
| **FC9** ⏳ | x86_64 **SysV** aggregate ABI: struct-by-value args (the eightbyte INTEGER/SSE/MEMORY classification — bounded per-CC algorithm, config-parameterized), struct return (small in regs / large via hidden pointer). **§A.5 honesty:** runtime-closed on **ELF-x86_64** (WSL + the linux CI legs); the **Mach-O-x86_64 arm is byte-pinned-only + RUNTIME-PENDING** (no macOS-x86_64 CI leg — `macos-latest` is Apple Silicon; pinned to the existing macOS-x86_64-no-leg gap). | — |
| **FC10** ⏳ | x86_64 **Win64** aggregate ABI: aggregates >8B by address, hidden-pointer return + shadow space; PE runnable corpus | — |
| **FC11** ⏳ | arm64 **AAPCS64** + **Apple** aggregate ABI: HFA/HVA homogeneity in SIMD regs, by-address, indirect-result-location; ELF + Mach-O arm64 corpus (qemu + macos-latest — both runtime-close) | `D-FF3-APPLE-ARM64-ABI-DIVERGENCE` (+ arm64 ABI residue) |
| **FC12a** ⏳ | **SysV** variadic ABI: the `__va_list_tag` register-save-area (gp_offset/fp_offset/overflow_arg_area) + `va_start`/`va_arg`/`va_end` lowering; real `printf`-family FFI; ELF corpus | `D-FFI-DESCRIPTOR-VARIADIC-SIGNATURE` (the descriptor half) |
| **FC12b** ⏳ | **Win64** variadic ABI (register+shadow, homogeneous `va_list`); PE corpus | — |
| **FC12c** ⏳ | **AAPCS64 + Apple** variadic ABI (Apple = always-stack variadic — the `D-FF3-APPLE-ARM64-ABI-DIVERGENCE` variadic half); arm64 corpus | `D-FF3-APPLE-ARM64-ABI-DIVERGENCE` (variadic half) |

### Cluster D — Preprocessor *(PARALLEL/INTERLEAVABLE track — user §B; a NEW token-stream transform UPSTREAM of the parser, architecturally DISTINCT from the existing post-parse tree-merge `#include` resolver)*
> **⚠ §B DESIGN FORK at FC13-open.** The preprocessor MUST stay agnostic — a config-SELECTED/DECLARED pass (the `.lang.json` opts in + declares directive + macro syntax; the engine is a generic token-stream rewriter, building on the EXISTING config-driven `popAtNewline` directive-lexing substrate). It is NOT an `if (lang=="c")` in shared substrate, and NOT the current tree-merge `#include` (which loads a header as a separate `Tree` + `CrossTreeRef` AFTER parsing — a real PP splices TOKENS BEFORE parsing). The §B owns: (1) the config-declared-vocabulary vs generic-engine model; (2) the fate of the existing include-following resolver (co-exist vs supersede — `import_resolver`); (3) the multi-language opt-in pin (toy/tsql must get NO macro expansion — a strict test proves it). Disambiguate from plan-11 **FF9** (a separate post-v1 header-parser concept, NOT this pass).

| Phase | What | Closes |
|---|---|---|
| **FC13** ⏳ | Preprocessing substrate (C translation phases 3–4): object + function-like macro definition/expansion; `#include` TEXTUAL token expansion. (Resolves the FC13 §B above.) | — (new substrate) |
| **FC14** ⏳ | Conditional compilation: `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif` + `defined()`. **Shared substrate:** its `#if` integer-constant-expression (ICE) evaluator is the SAME const-expr evaluator FC3 (enum/array-size) + FC8 (`enum { A = 1<<3 }`) need — build it ONCE, agnostically (do not write two). | — |
| **FC15** ⏳ | Token-paste `##`, stringize `#`, predefined macros (`__FILE__`/`__LINE__`/`__STDC__`/`__STDC_VERSION__`…), `#pragma` (recognized), `__has_include`/`__has_c_attribute` | — |

### Cluster E — C completeness + C23 modern + conformance *(caps the arc)*
| Phase | What | Closes |
|---|---|---|
| **FC16** ⏳ | **Standard-C completeness the front-end parsed but doesn't yet MEAN** (the no-stub closure): `_Generic` (generic selection); REAL `static_assert`/`_Static_assert` (constant-expr eval + diagnostic, not just a keyword); `alignas`/`alignof` (`_Alignas`/`_Alignof` — interacts with FC6 layout); anonymous struct/union members (FC7 member-access); attribute **SEMANTICS** (`packed` — feeds FC6 layout; `noreturn`; at minimum the layout/correctness-affecting ones); wide/UTF char+string literals (`L"…"`, `u"…"`, `U"…"`, `u8"…"`, `L'x'`); **`volatile`-honoring codegen** (eliding a `volatile` access is a miscompile — a correctness feature, not syntax). | — |
| **FC17** ⏳ | C23-specific modern: `[[attributes]]` semantics, `typeof`/`typeof_unqual`, `constexpr`, `nullptr`/`nullptr_t`, enums with explicit underlying type | — |
| **FC18** ⏳ | A broad runnable C23 corpus (real-ish programs across all targets, §A.5) + positioned-diagnostic assertions for the new error classes (REUSES the V2-4 Part C `expectDiagnostics` harness); grows the golden diagnostic corpus | `D-DIAG-CORPUS-EVERY-CODE` (grows toward closure) |

---

## 0.2 Scope & design decisions (the user §B answers + the plan-lock locks, 2026-06-10)

Captured here so they are not re-litigated each cycle:

1. **C standard = C23, "as mainstream compilers implement it"** (user). The full language; the exotic long-tail (§3.2) is pinned-deferred, not blocking.
2. **Full preprocessor IS in scope** (Cluster D) — a complete macro + conditional-compilation + include-expansion pass; a NEW token-stream transform upstream of the parser (NOT the existing tree-merge resolver).
3. **Aggregate ABI on ALL current targets** (x86_64 + arm64 × PE/ELF/Mach-O) within V2-4.X (Cluster C). The runtime-closure honesty exception: Mach-O-x86_64 (FC9) is byte-pinned-only (no macOS-x86_64 CI leg).
4. **Recorded as this dedicated `plan-23` + a `V2-4.X` row in plan-00 §0.1.**
5. **Preprocessor ordering = PARALLEL/INTERLEAVABLE track** (Cluster D is upstream of parsing, independent of the type/ABI backend; pick per-cycle whether D or B/C advances).

**Plan-lock locks (folded from the 3-reviewer review, 2026-06-10):**
- **Width agnosticism (FC3):** widths resolve per the active target's `dataModel` axis, NOT a language-config constant — the registry-prescribed mechanism for `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH`.
- **Aggregate-ABI agnosticism (Cluster C):** parameters-in-config + bounded per-CC algorithm in the realization tier; an explicit FC9-open §B.
- **Layout agnosticism (FC6):** per-ABI layout params declared in `.target.json` (the `D-FF3-1` extension), read by a generic `type_layout` engine into a side-table.
- **Preprocessor agnosticism (FC13):** config-selected/declared generic pass + the multi-language opt-in pin; an explicit FC13-open §B.
- **Shared const-expr evaluator:** FC3 / FC8 / FC14 use ONE evaluator.
- **No over-claimed closures:** the already-shipped predecessors (§3.1) — 2 closed anchors (division, extern-call-dispatch) + the `main` implicit-return feature — are struck, not re-claimed; `D-LK10-ENTRY-MAIN-IMPLICIT-RETURN` stays OPEN (non-main successor).

---

## 1. Motivation & current state

`c-subset` today already RUNS real programs (factorial, fibonacci, `puts("hello")`, cross-CU linkage) on 3 OSes × 2 arches. Its grammar is rich — but it is a *subset*: a program using `%`, `unsigned`, a C-style cast, a function-pointer declarator, `goto`, a struct passed by value, a variadic call, or a `#define` either fails to parse or fails to codegen. "Full C" means a developer takes an ordinary C23 translation unit and DSS compiles + links + runs it.

The exploration (2026-06-10, three code-explorers) + a strict 3-reviewer plan-lock established the shape: the front-end is mostly there (minus function pointers + the UAC engine + the missing operators), the **backend (aggregate layout + member-offset codegen + aggregate/variadic ABI) is the bulk**, the **preprocessor is wholly absent**, and the ABI/width/layout work is the agnosticism-critical part (it must stay parameters-in-config + bounded realization code, never an identity branch). Hence the ordering: cheap front-end wins first (A), then the gating layout substrate (B), then the large per-target ABI (C), with the preprocessor (D) as a parallel track and C completeness + C23 polish + conformance (E) last.

---

## 2. The clusters (detail)

### 2.A — Front-end completeness (FC1–FC5)
Mostly config-driven (grammar + semantic + HIR-lowering config), with three real engine pieces: `%`/div-mod codegen (FC1), the **UAC engine + the `dataModel` width axis** (FC3 — the genuine hard part, a classic miscompile farm: requires a negative test like `-1 > 0u`), and **function-pointer declarator-stacking** (FC4 — a real grammar/parser gap, not config). Each phase: a runnable corpus arm + positioned-diagnostic tests for the new error cases.

### 2.B — Aggregate backend (FC6–FC8)
**FC6 (layout substrate) is the gating piece + realizes `D-FF3-1`.** The agnosticism design: per-ABI layout PARAMETERS in `.target.json` (the `D-FF3-1` cross-tier extension), a generic `type_layout` engine reading them into an `ArenaAttribute<TypeId, StructLayout>` side-table. **Why the side-table, not `TypeRecord`:** `TypeRecord` is a fixed trivially-copyable arena record for a *language-level interned type*; layout is *target/ABI-dependent*, so baking offsets into it would make an interned type target-specific (the WRONG long-term move). The side-table is the established codebase pattern and the clean choice. Bitfield packing (FC8) is part of the same per-ABI parameter set (MS vs SysV differ). FC7 threads layout into `hir_to_mir` (MIR already has `Alloca`/`Gep`/`ExtractValue`/`InsertValue`).

### 2.C — Aggregate ABI (FC9–FC12c) — the heaviest
The per-calling-convention struct-by-value + variadic lowering in the **sanctioned realization tier** (`lir_callconv.cpp` / `mir_to_lir::lowerCall`), **parameterized** by new `TargetCallingConvention`/ABI-catalog config fields (the FC9 §B locks the data-vs-code boundary). Each FC ships a runnable corpus with §A.5 cross-target runtime closure on its native CI leg — EXCEPT FC9's Mach-O-x86_64 arm (byte-pinned-only; no macOS-x86_64 leg). The classification ALGORITHMS (SysV eightbyte merge lattice; AAPCS64 HFA recursive homogeneity test) are bounded per-CC code — NOT a data-walked engine, NOT an identity branch in shared MIR/HIR. Variadic is split per-CC (FC12a/b/c) mirroring the aggregate split (one shippable target per phase; SysV's register-save-area `va_arg` alone rivals a whole FC9).

### 2.D — Preprocessor (FC13–FC15) — parallel track
A NEW token-stream pass between tokenization and parsing, building on the existing config-driven `popAtNewline` directive-lexing substrate — architecturally DISTINCT from the post-parse tree-merge `#include` resolver. **Agnosticism is the crux** (FC13 §B): config-selected/declared mechanism + a multi-language opt-in pin (toy/tsql get NO macro expansion). The `#if` ICE evaluator is the shared const-expr evaluator (FC3/FC8/FC14 — one engine).

### 2.E — C completeness + C23 modern + conformance (FC16–FC18)
FC16 gives MEANING to the standard-C features the front-end parses but doesn't yet implement (`_Generic`, real `static_assert`/`alignas`/`alignof`, anonymous members, attribute *semantics* incl. layout-affecting `packed`, wide/UTF literals, `volatile`-honoring codegen) — closing the parse-without-semantics stub class. FC17 adds the C23-specific surface. FC18 is the broad C23 corpus + the positioned-diagnostic conformance sweep that grows `D-DIAG-CORPUS-EVERY-CODE`.

---

## 3. Deferrals

### 3.1 — Deferrals this arc CLOSES (~12 OPEN anchors)
`D-CSUBSET-MOD-OP-CODEGEN`, `D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT`, `D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC` (FC1); `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH`, `D-CSUBSET-UDIV-RUNTIME-HIGH-BIT-PIN` (FC3); `D-CSUBSET-LINKAGE-VISIBILITY-SYNTAX`, `D-DECL-PREFIX-STRIP-SHARED-HELPER` (FC4); **`D-FF3-1`** (FC6 layout half + FC12a va_arg half — the load-bearing per-ABI layout substrate); `D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY` (FC8); `D-FF3-APPLE-ARM64-ABI-DIVERGENCE` (FC11 aggregate + FC12c variadic); `D-FFI-DESCRIPTOR-VARIADIC-SIGNATURE` (FC12a); `D-DIAG-CORPUS-EVERY-CODE` grows toward closure (FC18). Several ARM64/multi-format codegen anchors (`D-LK10-ENTRY-ARM64-*`) have triggers that FIRE as the full-C corpus exercises them — closed opportunistically inside the relevant FC.

> **ALREADY SHIPPED — do NOT re-claim as new FC work** (caught by the plan-lock accuracy review). Two genuinely-CLOSED **anchors**: `D-CSUBSET-DIVISION-OP-CODEGEN` (✅ 2026-06-04, signed division shipped — FC1 only adds modulo); `D-FFI-EXTERN-CALL-DISPATCH` (✅ 2026-06-08 — FC12 does variadic dispatch, a different concern). Plus one closed **feature** with no anchor row: the `main` implicit-return-0 (✅ 2026-06-02, live in `c-subset.lang.json`, config-only) — note the anchor `D-LK10-ENTRY-MAIN-IMPLICIT-RETURN` itself is the still-OPEN *non-main* successor (registry §, line ~109), which FC5 adds optionally; it is NOT a closed anchor.

### 3.2 — Named exclusions (pinned, NON-blocking, NOT in the V2-4.X critical path)
Honest scope-bounding for "C23 as mainstream-implemented," each its own future trigger-gated work (pinned as `D-FULLC-*` registry rows as each cluster lands, per §F):
- **VLAs** (`int a[n]`) — today fail-loud (`S_NonConstantArrayLength`); a permissive runtime-length arm is deferred.
- **`_Complex` / `<complex.h>`**, **`_Atomic` / `<stdatomic.h>`**, **`<threads.h>`** — large library + type-system features.
- **`setjmp`/`longjmp`** (non-local control flow), **inline assembly**, **`<tgmath.h>`** (type-generic math), **`<stdbit.h>`** niche bits.
- **C23 `#embed`**, **arbitrary-width `_BitInt(N)`** beyond the standard integer widths.
- **`long double` width/format divergence** (80-bit x87 on SysV-x86_64 vs 64-bit on Win64/ARM64 vs IEEE-128) — another platform-divergent primitive FC3 surfaces; better named-deferred than silently mis-bound (the `long` lesson).

---

## 4. Cross-plan relationships

- **Grammar/schema** ([02](./02-schema-expressiveness-v2-plan%20-%20ok.md), `c-subset.lang.json`): Cluster A (incl. the function-pointer declarator gap) + FC17 C23 syntax.
- **Semantic** ([08.6](./08.6-semantic-plan%20-%20ok.md)): the UAC engine + `dataModel` (FC3), member lookup (FC7), the shared const-expr evaluator (FC3/FC8/FC14).
- **HIR** ([09](./09-hir-plan%20-%20ok.md)) + **MIR/LIR** ([12](./12-mir-lir-plan%20-%20ok.md)): the `type_layout` side-table (FC6), member-access lowering (FC7), the aggregate ABI in `lir_callconv` (FC9–FC12c).
- **FFI** ([11](./11-ffi-plan%20-%20tbd.md)): **`D-FF3-1`** (the per-target ABI layout TargetSchema extension — realized by FC6/FC12) + variadic descriptors (FC12a). Disambiguate FC13 from plan-11 **FF9** (a separate post-v1 header-parser concept).
- **Assembler** ([13](./13-assembler-plan%20-%20tbd.md)) + **Linker** ([14](./14-linker-plan%20-%20tbd.md)): div/mod encoding (FC1), rodata aggregates (FC8).
- **Preprocessor** — a NEW concern (token-stream transform upstream of the parser); closest existing home is the tokenizer/source-translation plans ([04](./04-tokenizer-plan%20-%20ok.md), [10](./10-source-translation-plan%20-%20tbd.md)). FC13's §B decides its plan home + the existing-resolver interaction.
- **Diagnostics** — the V2-4 `D-DIAG-CLI-POSITION-RENDER-AND-ASSERT` harness (`tests/analysis/test_diagnostic_corpus.cpp` + the `expectDiagnostics` corpus field) is REUSED throughout for the new error classes.

---

## 5. Acceptance

V2-4.X is "done" when an ordinary C23 translation unit (structs-by-value, variadics, macros, conditional compilation, the full type system + conversions) **compiles → links → runs** on all current targets (x86_64 + arm64 × PE/ELF/Mach-O), proven by a broad runnable corpus (§A.5 cross-target runtime closure on every native CI leg — except the named Mach-O-x86_64 byte-pin gap) + strict positioned-diagnostic assertions for the error classes + the named-exclusion deferrals pinned.

**Per-FC acceptance gate (every cycle, alongside the strict-test + corpus lines):** *agnosticism — no new `if (lang/arch/format/cc.name == …)` in `src/{opt,mir,hir,lir,core}` or the parser/semantic engines; new per-CC/per-format/per-data-model logic lives ONLY in the sanctioned realization tier and is config-parameterized; no parse-without-semantics stub (a feature that parses must MEAN something or be a pinned exclusion).* Each FC phase is independently green-and-shippable; the arc never sits on a long red branch.
