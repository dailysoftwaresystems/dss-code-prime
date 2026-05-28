# DSS Code Prime — Universal Compiler Implementation Plan

> **Universal-compiler thesis (rev 2 — 2026-05-23).** Three architectural decisions now anchor every downstream plan:
>
> 1. **Hermetic.** We own every byte from source to signed binary. No `ld` / `link.exe` / `ld64` / `lld` / `wasm-ld` / `xcrun` / `clang` / `gcc` / `MSVC` / `dxc` / `glslc` / `dsymutil` / `mspdb` / `codesign` / `signtool` invocations. OS-supplied runtime libs (libc.so / libSystem.dylib / msvcrt.dll / ntdll.dll), browser/wasmtime, GPU drivers, and Apple developer certs are FFI targets and credentials — never tools.
> 2. **Three IR layers: HIR → MIR → LIR.** [HIR](./09-hir-plan%20-%20tbd.md) (language-neutral, structured, typed — the pivot for transpilation, native lowering, and shader/WASM lowering) → [MIR](./12-mir-lir-plan%20-%20tbd.md) (SSA over CFG with structured-CF markers preserved for WASM) → [LIR](./12-mir-lir-plan%20-%20tbd.md) (per-target ISA). A single SSA IR cannot serve both binary codegen *and* source-to-source transpilation cleanly; HIR is the layer where the seven goals converge.
> 3. **Core type lattice + per-language extensions.** Universal core (primitives, ptr/ref/fnptr, struct/union/tuple, array/slice, vector/matrix, function signatures with calling-convention attribute) lives in [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) §2.2; each `.lang.json` registers extension type-kinds (C++ MemberPtr, C# Boxed/Delegate/GcRef, TSQL Varchar<N>/RowType, VHDL Std_Logic, shader Sampler/Texture). Transpilation maps via the core; extensions need explicit map entries.
> 4. **Config-driven all the way down — the engine has NO per-language C++.** The thesis "the source language is configurable" applies to *every* phase, not just lex/parse. Semantic analysis ([`08.6-semantic-plan`](./08.6-semantic-plan%20-%20tbd.md)) reads a `semantics` block from `.lang.json` (schema v4) via one language-agnostic engine; CST→HIR lowering ([`09-hir-plan`](./09-hir-plan%20-%20tbd.md)) is likewise schema-described. **No phase may branch on `schema.name()` or hardcode a language's rules in C++** — a new language is onboarded purely by adding config. Genuinely exceptional per-language logic, if it ever arises, is an explicit documented escape-hatch behind a config-default interface, never the primary mechanism. When config can't yet express something, extend the config vocabulary (additively, as schema v2 did for grammar), don't language-branch the engine.
>
> **v1 production target.** End-to-end binaries for every language in `src/source-config/languages/` (`toy`, `c-subset`, `tsql-subset`) on **{Windows, Linux, macOS} × {x86_64, ARM64}**, gated by the [`artifactProfile`](./06-artifact-profile-plan%20-%20tbd.md) compatibility mechanism — produced via the **hermetic in-tree toolchain** (assembler + linker + debug-info writer + codesigner all owned). **See [`07-production-readiness-plan - tbd.md`](./07-production-readiness-plan - tbd.md) for the master gap catalog**.
>
> **v1.x targets** (reserved namespaces; designs land now to keep substrate honest): WASM ([`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md)), shader/GPU ([`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md)), source-to-source transpilation ([`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md)), FFI ingestion of precompiled libraries ([`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md)).
>
> **Reserved (no work until trigger):** VHDL/Verilog ([`19-hir-hw-reserved-plan`](./19-hir-hw-reserved-plan%20-%20tbd.md)), user's custom language ([`20-custom-language-reserved-plan`](./20-custom-language-reserved-plan%20-%20tbd.md)), language runtime ([`21-runtime-reserved-plan`](./21-runtime-reserved-plan%20-%20tbd.md)).
>
> **Status & sub-plans.** This document is the high-level master plan.
> Detailed designs for individual modules now live in dedicated sub-plans:
>
> - **[`01-tree-node-model-plan - ok.md`](./01-tree-node-model-plan - ok.md)** — the tree/node data structure, `GrammarSchema` config loader, `TreeBuilder`, diagnostics. **Supersedes** the relevant pieces of §4.2.2 (`ast.hpp`), §4.3 (config JSON shape), §4.4 (`source-factory/` internals), §4.5 (tokenizer trivia handling), and §4.6.2 (parser/AST construction) of this document. Where this document and the sub-plan disagree, **the sub-plan wins**; in-line `> AMENDED:` notes in the sections below redirect readers.
> - **[`02-schema-expressiveness-v2-plan - ok.md`](./02-schema-expressiveness-v2-plan - ok.md)** — additive extensions to the v1 schema for operator precedence, contextual keywords, scope-stack patterns, speculative `alt`, string interpolation, and custom string-literal variants. **✅ done.** PR0 (c-subset + gap catalog), PR1 (operator precedence), PR2a (real `SchemaCursor` walker), PR2b (contextual keywords + `reservedWordPolicy`), PR3 (`scopeRequire`), PR4 (`TreeBuilder::Checkpoint` + speculative-alt loader plumbing), PR5 (`lexerModes` + `LexerModeStack` + `modeOp`), PR6 (`stringStyle` + `SchemaId`/`StringStyleId` strong ids), PR7 (tsql-subset stress test proving v2 is empirically sufficient), PR8 (cross-plan close-out) all shipped + review-fixed. Unlocks the eventual `languages-onboarding-plan.md`.
> - **[`03-substrate-hardening-plan - ok.md`](./03-substrate-hardening-plan - ok.md)** — originally three small de-risking PRs (SH1–SH3) that land **between v2 and the tokenizer phase**; extended with SH4 (v2 follow-ups: CI hygiene + shape-based `switch`/`case`/`default` adoption + multi-level AltChoice nesting stress test). **✅ done.**
> - **[`04-tokenizer-plan - ok.md`](./04-tokenizer-plan - ok.md)** — opens parent phase #5: `Tokenizer` + `TokenStream` + `SourceReader`, mode-aware schema resolution, hand-coded numeric lexing, `LexerModeStack` integration, `stringStyle` body handling, comment modes. TZ1 (bare tokenizer + toy E2E flip), TZ2 (modes + strings + comments + c-subset comment mode), TZ3 (c-subset / tsql-subset E2E flips + 3 semantic gaps closed) all shipped; TZ3 review-fix round 1 (13 items) shipped on top. Master plan §8 row 6 (`analysis-lexical`) is **subsumed** into this plan + the v2 schema-aware builder. **✅ done.**
> - **[`05-parser-plan - ok.md`](./05-parser-plan - ok.md)** — opens parent phase #7 (`analysis-syntactic`): substrate refactor + schema-driven RD driver + Pratt walker + recovery + corpus + LSP scaffolding. **Seven PRs** (PA0 `SchemaWalker` extraction ✅, PA1 RD driver ✅, PA2 Pratt walker ✅, PA3 recovery + diagnostic UX ✅, PA4 corpus stress ✅, PA5a LSP server + diagnostics ✅, PA5b LSP semantic-stub method handlers ✅). Plus shipped sub-workitems: PA-Walker-LeftRec (postfix-chain walker redesign), PA5a-prep (c-subset readiness shape additions), PA4-followups (8 items), and an LSP cross-cutting review fix-everything pass on the integrated stack. LSP spins off to `09-lsp-plan` once scope outgrows parser-plan. Closes `v2-gap-catalog - tbd.md` row 1 and production-readiness G-115 (diagnostic UX) end-to-end. **🟢 all 7 PRs ✅ done — parser phase closed.**
> - **[`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md)** — new cross-cutting concept: `artifactProfile` field on language configs (declares supported outputs: `cli`/`gui`/`lib`/`staticlib`/`script`/`sproc`) + project configs (picks one). Driver enforces compatibility at load time. Codegen reads it to pick entry-point names, subsystem flags, output extensions. Four PRs (AP1 schema + loader, AP2 project config + driver, AP3 `CompilationContext` plumbing, AP4 onboard shipped languages). **⏳ planned.** Gates §11 (`gen-link`).
> - **[`07-production-readiness-plan - tbd.md`](./07-production-readiness-plan - tbd.md)** — **master gap catalog for v1 production.** 127 numbered gaps (G-001..G-762) covering everything between current state and shipping v1 binaries on 3 OS × 2 arch. Cross-references every sub-plan above. Owns the acceptance criteria. **🔵 opened.**
> - **[`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md)** — opens **phase #7.5** (`compilation-unit-model`): bridges parser (per-file `Tree`) and semantic (cross-file symbol table) via a `CompilationUnit` type + driver-level aggregator + per-language import resolver. Now five PRs (CU1 type, CU2 multi-file driver, CU3 cross-tree symbol shape, CU4 import resolver, **CU5 multi-language CUs** — added rev 2). Closes production-readiness G-110 + G-111. **⏳ planned**, gated by parser PA1 shipping; precedes phase #8 (`analysis-semantic`).
> - **[`08.5-substrate-prep-plan - tbd.md`](./08.5-substrate-prep-plan%20-%20tbd.md)** — **new (rev 2).** Two-PR refactor between CU plan and semantic phase: SP1 generalize the arena + attribute substrate so HIR/MIR/LIR can reuse `Tree`'s engineering primitives; SP2 ship the core type lattice + per-language extension registry. Substrate-tier review. **✅ done (SP1 + SP2)** — `src/core/substrate/` + `src/core/types/type_lattice/`; `typeExtensions[]` schema v3. Unblocks phase #8.
> - **[`08.55-pre-se1-cleanup-plan - ok.md`](./08.55-pre-se1-cleanup-plan%20-%20ok.md)** — **new.** A focused cleanup PR between 08.5 and 08.6, forced by a 5-agent audit (2026-05-25): retires the remaining engine-side source-language hardcoding under the user's standing "NO CAVEATS, NO WORKAROUNDS" rule (decision #4). Schema-drives the Pratt wrapper-rule names (`expr.wrapperRules`), schema-drives numeric lexing (`numberStyle` block), prunes pre-interned token kinds to universal categories, retires `well_known_names`/`tree_views`/`symbol_population`, makes the LSP shipped-language list a directory scan with loud-fail `ShippedDirNotFound`/`ShippedDirEmpty` error kinds. **✅ shipped (commit `1a433c2`).** Unblocked SE1.
> - **[`08.6-semantic-plan - ok.md`](./08.6-semantic-plan%20-%20ok.md)** — Opens phase #8 (`analysis-semantic`): symbol table + scope resolution + type checking. Load-bearing decision: semantics are **config-driven** (a `semantics` block in `.lang.json`, schema **v4**) read by one language-agnostic `analyze()` engine — **no per-language C++**, honoring the universal-compiler thesis (decision #4 below). Seven PRs (SE1 toy → SE2 c-subset → SE3 tsql → SE4 const → SE5 typedef → SE6 functions/calls/overloads → SE7 LSP). **✅ SE1–SE7 all shipped** (two 5-agent review rounds; 64/64 ctest; engine stays language-agnostic via the `semantics`/`kindByChild`/`callRules`/`builtinFunctions` config facets, proven generic by the Synth2/Synth3 schemas). Closes G-201..G-212.
> - **[`09-hir-plan - tbd.md`](./09-hir-plan%20-%20tbd.md)** — **new (rev 2).** The pivot layer — language-neutral, structured, typed HIR with shader/FFI/transpile attribute side-tables. CST→HIR lowering is **config-driven** (per thesis decision #4: the lowering is schema-described, NOT per-language C++) so a new language lowers to HIR via config, not engine edits. Eleven PRs (HR1–HR11). **✅ COMPLETE (HR1–HR11 all landed 2026-05-26..28 on `feature/hir-1`): HR1 ✅ (commit `406d5c7` on `feature/hir-1`, 2026-05-26 — arena + node shapes + walker + ids + extension registry, 69/69 ctest) + HR2 ✅ (2026-05-26 — typed expressions: operator open-core+registry, typed builder helpers, `HirVerifier` expression-typing rule, 72/72 ctest) + HR3 ✅ (2026-05-26 — structured CF: statement builders + typed read-accessor layer + break/continue & per-kind-arity verifier rules, 73/73 ctest) + HR4 ✅ (2026-05-26 — declarations + extern surface: 7 declaration builders/accessors, FfiMetadata side-table, checkDeclarationShape verifier rule, 74/74 ctest) + HR5 ✅ (2026-05-26 — attribute system + side-tables: 4 value-struct headers + `hir_attrs.hpp` catalog, verifier emits real diagnostic spans via optional `HirSourceMap` (closes the HR2 span-stash IOU), dedup key folds in `actual`, 75/75 ctest) + HR6 ✅ (2026-05-27 — full verifier: block dead-code + return completeness + Call-arg-vs-FnSig + intrinsic-registered + shader-restriction subverifier, new `HirIntrinsicRegistry`, optional `TypeInterner` injection, `H_UnknownIntrinsic`/`H_ShaderViolation`, 76/76 ctest) + HR7 ✅ (2026-05-27 — round-trippable `.dsshir` text format: `emitHir`/`parseHir` + non-owning `HirTextContext` + heap-stable `HirParseResult`, inline structural types, positional `%N` symbol handles, all 5 side-tables inline, verify-on-load, in-memory + golden-corpus tests, `H_TextMalformed`/`H_TextVersionMismatch`/`H_TextUnknownName`, 77/77 ctest) + HR8 ✅ (2026-05-27 — config-driven CST→HIR lowering engine + `hirLowering` schema facet, proven end-to-end on c-subset (reordered ahead of toy); per-expression type inference + a `HirLiteralPool` of decoded literal values; verify-on-load; deferred constructs fail loud via `H_UnsupportedLoweringForKind`; closed an HR7 return-value-attr parse bug; 78/78 ctest) + HR9 ✅ (2026-05-27 — `toy` enriched in place into a real typed language + a generic lowering test proving the one engine handles a second grammar; **arrays un-deferred end-to-end** via a config-driven `DeclarationRule.arraySuffix` declarator-suffix descriptor + semantic-time constant-length eval (`S_NonConstantArrayLength`/`S_ArrayLengthOutOfRange`, fail-loud, no pointer decay); shared `decodeInteger` (`number_decode.hpp`); compound-assign/`++`/externs/genericity earlier; 5-agent review + fix pass caught a silent negative-length wrap; 80/80 ctest) **+ gap-closure pass 2026-05-28** completing the frontend→HIR layer: char/string literal VALUES (coalesce-body lexer — the former TZ4 blocker is resolved), HIR `SeqExpr` (value-yielding `++`/assignment-as-expression/complex-lvalue compound-assign), pointers, ternary `?:`, `#include` skip, `.dsshir` inline literal values (81/81) + HR10 ✅ (2026-05-27..28 — CST→HIR lowering for **tsql-subset** through the SAME language-agnostic engine: role-explicit SQL extension nodes via a generic `childGathering` config vocabulary + `ChildLower` enum, flat-expr lowering, coalesced/doubled-delimiter string VALUES → `Array<Char,N>`, `NULL`→`TSQL::Null`, relational names→`refExtensionKind` leaf (not core `Ref`), `ReferenceRule.hardParents` positional table-vs-column resolution, SQL calls reuse `callRules`; 5-perspective review + fix pass clean; 82/82 ctest). **HR11 ✅ done 2026-05-28 (multi-language CU lowering). Plan 09 (HIR) COMPLETE — HR1–HR11 all ✅.**
> - **[`10-source-translation-plan - tbd.md`](./10-source-translation-plan%20-%20tbd.md)** — **new (rev 2).** Promotes the formerly-long-running source-translation note (§9) into a real phase. Language-pair `.map.json` + HIR→HIR walker + target-CST builder + pretty-printer. Six PRs (ST1–ST6). v1.x. **⏳ planned.**
> - **[`11-ffi-plan - tbd.md`](./11-ffi-plan%20-%20tbd.md)** — **new (rev 2).** Hermetic FFI: in-tree ELF/PE/Mach-O/ar readers, C header mode parser, ABI catalog, name mangling, HIR extern-decl ingestion. Six v1 PRs (FF1–FF6). C++ mangling, full preprocessor reserved post-v1. **⏳ planned.**
> - **[`12-mir-lir-plan - tbd.md`](./12-mir-lir-plan%20-%20tbd.md)** — **new (rev 2).** MIR (SSA over CFG + structured-CF markers preserved) + LIR (per-target ISA, virtual+physical regs, calling-conv lowered, frame materialized). Eight PRs (ML1–ML8). **⏳ planned.**
> - **[`13-assembler-plan - tbd.md`](./13-assembler-plan%20-%20tbd.md)** — **new (rev 2).** In-tree machine-code encoder for x86_64 + ARM64. Hand-written encoding tables per Intel SDM / ARM ARM. Per-(arch×format) relocation taxonomy. Six PRs (AS1–AS6). **⏳ planned.**
> - **[`14-linker-plan - tbd.md`](./14-linker-plan%20-%20tbd.md)** — **new (rev 2).** In-tree linker — ELF / PE / Mach-O / WASM-skeleton / SPIR-V-skeleton object writers + symbol resolution + relocation application + per-platform metadata + TLS lowering. Ten PRs (LK1–LK10). **Largest single chunk of backend work.** **⏳ planned.**
> - **[`15-debug-info-plan - tbd.md`](./15-debug-info-plan%20-%20tbd.md)** — **new (rev 2).** DWARF 5 writer (ELF + Mach-O) + PDB writer (PE) + CFI / SEH / compact-unwind tables + source-position chain. Twelve PRs (DB1–DB12). **⏳ planned.**
> - **[`16-codesign-publish-plan - tbd.md`](./16-codesign-publish-plan%20-%20tbd.md)** — **new (rev 2).** Hermetic codesign + publish: Apple Mach-O codesign + notarization stapling, iOS provisioning, Windows Authenticode + RFC 3161 TSA, Android APK v3 (skeleton). In-tree crypto substrate (BearSSL vendored). Nine PRs (CS1–CS9). Apple-host-free local dev. **⏳ planned.**
> - **[`17-shader-gpu-plan - tbd.md`](./17-shader-gpu-plan%20-%20tbd.md)** — **new (rev 2).** SPIR-V codegen + shader-shape HIR extensions + binding-resource model + same-source CPU+GPU function dual-lowering. Ten PRs (SG1–SG10). v1.x — lit up for the custom language. **⏳ planned (reserved scope).**
> - **[`18-wasm-plan - tbd.md`](./18-wasm-plan%20-%20tbd.md)** — **new (rev 2).** WASM backend — HIR/MIR → WASM bytecode using structured-CF markers (no Relooper needed). WASM as "LIR-equivalent" target. Ten PRs (WA1–WA10). v1.x. **⏳ planned.**
> - **[`19-hir-hw-reserved-plan - tbd.md`](./19-hir-hw-reserved-plan%20-%20tbd.md)** — **new (rev 2).** Reserved sibling-IR for VHDL / Verilog / SystemVerilog (concurrent + signal-typed + clock-bound). Software HIR and HIR-HW do not unify cleanly. 🔒 reserved.
> - **[`20-custom-language-reserved-plan - tbd.md`](./20-custom-language-reserved-plan%20-%20tbd.md)** — **new (rev 2).** Reserved for the user's eventual custom language. The engine already supports it via the schema mechanism; this plan owns vocabulary + stdlib + runtime decisions. 🔒 reserved.
> - **[`21-runtime-reserved-plan - tbd.md`](./21-runtime-reserved-plan%20-%20tbd.md)** — **new (rev 2).** Reserved for GC / exception unwinder / coroutines / threading primitives. Distinct from OS-supplied runtime libs (those are FFI targets). 🔒 reserved.
>
> See §8 (Implementation Phases) for the current cross-plan dependency view.

## 0. Current Status (snapshot)

> **v1 production target locked.** End-to-end binaries for `toy` / `c-subset` / `tsql-subset` on **{Windows, Linux, macOS} × {x86_64, ARM64}**. Codegen, semantic, IR, project driver, and the new `artifactProfile` mechanism all still ⏳ — see [`07-production-readiness-plan - tbd.md`](./07-production-readiness-plan - tbd.md) for the 127-item gap catalog. The frontend (tokenizer + builder + schema cursor) is the only chunk that's done.

| Area | State |
|---|---|
| Build system (CMake 4.0 floor, C++23, FetchContent of nlohmann/json 3.12.0 + GoogleTest 1.17.0) | ✅ working |
| Core types — full sub-plan T0–T12 (tree/node/diagnostics/schema + `TreeBuilder` + `TreeCursor` + `tree_visitor` + `NodeAttribute<T>` + typed views + E2E + CMake wireup + onboarding docs) | ✅ **complete** |
| Schema expressiveness v2 (sub-plan PR0–PR8) — c-subset + operator precedence (`OperatorTable`) + real `SchemaCursor` walker + contextual keywords + `scopeRequire` + `TreeBuilder::Checkpoint` + `lexerModes`/`LexerModeStack`/`modeOp` + `stringStyle` descriptor + tsql-subset empirical stress + cross-plan close-out | ✅ **done** — all 10 PRs (PR0, PR1, PR2a, PR2b, PR3–PR8) shipped + review-fixed |
| **Total ctest cases** | **56 ctest suites, 100% pass** post CU1–4 + SP1–2 + 08.55 pre-SE1 cleanup. (Was 58 prior to 08.55; the two retired suites — `test_tree_views`, `test_symbol_population` — are intentional deletions per thesis decision #4: toy-shaped views and the CU3 placeholder no longer exist.) |
| Substrate hardening (sub-plan SH1–SH4) — landing-log generator + Linux CI matrix + cross-tree `NodeId` guard + v2 follow-ups | ✅ **done** — all four PRs shipped. |
| Tokenizer (sub-plan TZ1–TZ3 + review-fix round 1) — `SourceReader`, `Tokenizer`, `TokenStream`, `LexerModeStack`, `stringStyle` body handling, comment modes, `C_BodyDefaultKindInShape` loader guard, synthesis-allowlist assertion, fail-loud `E2EHarness`. **Post-TZ3: numeric lexing is now fully config-driven** (08.55 retired the hand-coded C-style `scanNumber()`; each language declares its `numberStyle` block, schema v4). | ✅ **done** — phase #5 closed; phase #6 (`analysis-lexical`) ✅ subsumed |
| **Parser** (sub-plan PA0–PA5b + PA5a-prep + PA-Walker-LeftRec) — substrate `SchemaWalker` ✅; iterative RD driver ✅; Pratt walker ✅; recovery + diagnostic UX ✅; corpus stress ✅; c-subset readiness shapes ✅; postfix-chain walker redesign ✅; LSP server + LSP semantic-stub handlers ✅ | ✅ **all 7 PRs done — parser phase closed.** PA0–PA3 as before; PA4 onboarded real corpus programs (c-subset / tsql-subset / toy) and closed v2-gap-catalog rows 6/7/8/12 (postfix call/index/inc/dec, prefix deref). PA4 follow-ups (PA4-F1..F8) landed substrate refactors (`OperatorTable::Entry → optional<GroupedPostfix>`, `bodyDefaultTokenKinds` dedup), additional test coverage, and corpus golden harness. PA5a-prep closed v2-gap-catalog rows 12-declarator, 16 (compound assignment), 29 (`extern` declaration). PA-Walker-LeftRec added the `TreeBuilder::wrapLastChildInFrame` substrate primitive + rewrote the Pratt walker's postfix branch. PA5a shipped the LSP server skeleton + diagnostics (stdio JSON-RPC, UTF-16 position encoding, document store with monotonic parseGeneration stale-suppression, two-mode schema resolution, thread-pool executor); PA5b added the 6 semantic-stub method handlers (`textDocument/{hover,completion,definition,references,rename,signatureHelp}`) returning LSP-spec defaults. A cross-cutting 5-agent review on the integrated LSP stack closed 22 follow-up items in a single commit. Semantic-powered LSP methods land post-phase #8 in a spun-off `09-lsp-plan`. **Post-PA5: the Pratt walker now reads its wrapper-rule RuleIds from `schema.exprWrapperRules(exprRule)` (08.55), not from any hardcoded constants — thesis decision #4 end-to-end.** **56 ctest suites green** (post 08.55). |
| **Compilation unit** (sub-plan CU1–CU6) — multi-file `CompilationUnit` + per-language `ImportResolver` | 🟢 **CU1 + CU2 + CU3 + CU4 done.** CU1 type/builder; CU2 multi-file `addFile`/`addInMemory` + `D_*` codes + lexer+parser diagnostics unified in the Tree (cross-plan amendments to 01/05); CU3 CU-scoped `SymbolId` + `UnitAttribute<T>` (the CU-scoped `NodeId`→`T` side-table phase #8 consumes) + membership-based cross-CU `NodeId` guard + sanctioned minimal `populateDeclarationSymbols` walk (§2.7 C3-X1); CU4 per-language `ImportResolver` populating `crossRefs` — c-subset `#include` following (grammar amended, §2.8 C4-X1) + tsql cross-statement table-name matching + toy identity + `D_UnresolvedImport`/`D_UnresolvedReference`. **G-110 ✅ + G-111 ✅ resolved.** CU5/CU6 v1.x. Phase #7.5 complete. See [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md). Bridges parser (per-file `Tree`) and semantic (cross-file symbol table). |
| **Semantic** — symbol table, scope resolution, type checking | ✅ **done (SE1–SE7)** (phase #8; sub-plan [`08.6-semantic-plan`](./08.6-semantic-plan%20-%20ok.md)). One language-agnostic `analyze()` engine driven by a `semantics` schema-v4 block produces a `SemanticModel` (symbol table + scope tree + node→SymbolId / node→TypeId side-tables + `S_*` diagnostics). Toy/c-subset/tsql onboarded purely by config; cross-file visibility is `crossRefs[]`-import-driven. SE4 const-correctness, SE5 typedefs, SE6 functions/calls/overloads (`kindByChild`/`callRules`/`builtinFunctions` facets), SE7 LSP wiring all shipped. **G-201..G-212 closed.** Two 5-agent review rounds; 64/64. |
| **IR** — HIR + MIR + LIR (rev 2; replaces single-IR plan) | 🟡 in progress — **HIR HR1–HR11 ✅ (plan 09 COMPLETE, 2026-05-26..28, `feature/hir-1`)**; HR8 = config-driven CST→HIR lowering engine proven on c-subset; HR9 = toy enriched into a typed language + arrays un-deferred + gap-closure (char/string VALUES, SeqExpr, pointers, ternary); HR10 = tsql-subset lowering; HR11 = multi-language CU lowering. **MIR/LIR: ML1 ✅ done 2026-05-27** (`feature/mir-lir` — MIR skeleton: fused value model, 3 arenas/one MirModuleId, phi nodes, closed MirOpcode + opcodeInfo table, build-once-freeze, create-then-fill builder, instBlock reverse-lookup, finish-time freeze sweep; 6-perspective review; 87/87); ML2–ML8 ⏳. Production-readiness §3 (G-301a/b/c + G-302..G-310). Owned by [`09-hir-plan`](./09-hir-plan%20-%20tbd.md) (HIR — the pivot; **HR1–HR11 done ✅**) + [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md) (MIR SSA-over-CFG with structured-CF markers + LIR per-target ISA; **ML1 ✅, ML2–ML8 ⏳**). |
| **Optimizer** — constant folding + DCE + copy propagation + dominator tree + liveness (v1 mandatory subset) | ⏳ pending (phase #10). Production-readiness §4 (G-401..G-410). Inlining + CSE + LICM deferred to v1.1. |
| **Codegen + in-tree linker + assembler** — ELF + PE + Mach-O × x86_64 + ARM64 + per-platform ABI + **hermetic linker** (rev 2; replaces system-linker integration) | ⏳ pending. **Largest single chunk of v1 work.** Production-readiness §5 (G-501..G-571). Owners: [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) + [`14-linker-plan`](./14-linker-plan%20-%20tbd.md). Current state: Windows PE demo only, not IR-driven. |
| **Driver / project config** — `dss-code-prime build my-project.dss-project.json` | ⏳ pending. Production-readiness §6 (G-601..G-609) + [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) AP2. |
| **`artifactProfile` mechanism** — language config declares supported profiles; project config picks one; codegen reads it | ⏳ pending. See [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md). Gates `gen-link` acceptance. |
| **Debug info** — DWARF (ELF + Mach-O) + PDB (PE) | ⏳ pending. Production-readiness §5.4 (G-530..G-533). |
| CI/CD pipelines | 🟦 Linux x86_64 + Windows x86_64 + macOS x86_64 ✅; **ARM64 across all three OS still pending** (G-704..G-706). Sanitizers partial. Fuzzing not yet wired. |
| **Real-world corpus** — `tests/corpus/{c-subset,tsql-subset,toy}/` programs that exercise the full grammar | ✅ parser-side done (PA4 — `mini_calc.c`, `schema_and_dml.sql`, `demo.toy` driven through `Parser::parse()` with golden-tree byte-comparison via `DSS_REFRESH_GOLDENS`). End-to-end "runs a compiled binary" still pending — gated on codegen (§11). Production-readiness §7.3 (G-730..G-734). |
| **Diagnostic UX** — clang-quality rendered source context with caret + actionable messages | ⏳ pending. Substrate exists in tree.diagnostics(); rendering layer in the driver doesn't. Production-readiness G-115 + G-608. |
| `source-factory/` thin facade | ⏳ pending (low priority; loader-level work already done in `GrammarSchema::loadShipped`). |
| Docker / cross-compile toolchains | ⏳ pending. |
| **Process / discipline** — per-PR review tiering (substrate / feature / doc), onboarding doc, second-engineer ramp | ⏳ pending. Production-readiness §7.6 (G-760..G-762). |

Drill into the [sub-plan §0 status table](./01-tree-node-model-plan - ok.md#0-current-status-snapshot) for tree/node phase detail.

## 0.1. Stepper — next-up by block

> **Definition of "next" used here:** a step is eligible to start the moment **all its dependencies are ✅ shipped** — nothing pending blocks it. Critical-path steps are numbered first; sidewise-but-unblocked work is grouped at the bottom. This is a condensed view of [§8](#8-implementation-phases-todos) — see §8 for full phase metadata, and each sub-plan for PR-level scope.
>
> Read top-to-bottom: step **1** is what to start now; the **Blocked by** column tells you why every other step has to wait.

| Step | Plan / PR | What | Blocked by | Status |
|---|---|---|---|---|
| ~~1~~ | [08](./08-compilation-unit-plan%20-%20tbd.md) **CU1** | `CompilationUnit` + `UnitBuilder` + `CrossTreeRef` + `CompilationUnitId` (single-tree/empty CU; move-only; `addTree` only) | — | ✅ **done** (13 tests, 48/48 suite green, 5-agent review) |
| ~~2~~ | [08](./08-compilation-unit-plan%20-%20tbd.md) CU2 | Multi-file `UnitBuilder` (`addFile`/`addInMemory`); `D_*` codes; lexer+parser diagnostics unified in the Tree (cross-plan 01/05) | step 1 ✅ | ✅ **done** (48/48 suite, 5-agent review) |
| ~~3~~ | [08](./08-compilation-unit-plan%20-%20tbd.md) CU3 | CU-scoped `SymbolId` + `UnitAttribute<T>` (CU-scoped `NodeId`→`T` side-table) + membership-based cross-CU `NodeId` guard + sanctioned minimal `populateDeclarationSymbols` walk (§2.7) | step 2 ✅ | ✅ **done** (21 + 3 tests, 50/50 suite green, substrate-tier 5-agent review + fix pass) |
| ~~4~~ | [08](./08-compilation-unit-plan%20-%20tbd.md) CU4 | Per-language `ImportResolver` populating `crossRefs`: c-subset `#include` following (grammar amended), tsql cross-statement table matching, toy identity, `D_Unresolved*` diagnostics | step 3 ✅ | ✅ **done** (16 tests, 51/51 suite green, substrate-tier 5-agent review + fix pass) |
| ~~5~~ | [08.5](./08.5-substrate-prep-plan%20-%20tbd.md) SP1 | Generalize arena + `NodeAttribute<T>` for HIR/MIR/LIR reuse (`ArenaContainer`/`ArenaBuilder`/`ArenaAttribute`; Tree + TreeBuilder rebuilt on it) | step 4 ✅ | ✅ **done** (54/54 suite green, full extraction, substrate-tier 5-agent review + fix pass) |
| ~~6~~ | [08.5](./08.5-substrate-prep-plan%20-%20tbd.md) SP2 | Core type lattice + per-language extension registry (`TypeKind`/`TypeRecord`/`TypeInterner`/`TypeRegistry`/`TypeLattice`; `typeExtensions[]` schema v3) | step 5 ✅ | ✅ **done** (57/57 suite green, bound-but-separate, substrate-tier 5-agent review + fix pass) |
| ~~7a~~ | [08.55](./08.55-pre-se1-cleanup-plan%20-%20ok.md) | **Pre-SE1 genericity cleanup**: schema-drive Pratt wrappers, schema-drive numeric lexing, prune pre-interned kinds, retire toy-shaped headers + CU3 placeholder, LSP list config-driven. | step 6 ✅ | ✅ **done** (commit `1a433c2`) |
| ~~7~~ | [08.6](./08.6-semantic-plan%20-%20ok.md) SE1–SE7 | Symbol table + scope resolution + type checking — **config-driven** (`semantics` block, schema v4; one language-agnostic engine). Engine + toy/c-subset/tsql configs; per-tree-root scopes + `crossRefs[]` import injection; const-correctness; typedefs; functions/calls/overloads (`kindByChild` discriminator); LSP wiring (6 handlers). 64/64. | step 7a ✅ | ✅ **SE1–SE7 done** |
| 8 | [09](./09-hir-plan%20-%20tbd.md) HR1–HR11 | HIR — language-neutral structured typed pivot; CST→HIR lowering per shipped language | step 7 | ✅ **plan 09 (HIR) COMPLETE — HR1–HR11 ✅** (2026-05-26..28, `feature/hir-1`; HR8 = config-driven lowering engine on c-subset; HR9 = toy enriched into a typed language + arrays un-deferred + gap-closure (char/string VALUES, SeqExpr, pointers, ternary); HR10 = tsql-subset lowering — role-explicit SQL extension nodes via generic `childGathering` config; HR11 = multi-language CU lowering, all language-agnostic) |
| 9 | [11](./11-ffi-plan%20-%20tbd.md) FF1–FF6 | FFI — hermetic ELF/PE/Mach-O/ar readers + C-header mode parser + ABI catalog + name mangling | step 8 | ⏳ — parallel with step 10 |
| 10 | [12](./12-mir-lir-plan%20-%20tbd.md) ML1–ML8 | MIR (SSA over CFG + structured-CF markers) + LIR (per-target ISA) | step 8 | 🟡 **ML1 ✅ done** 2026-05-27 (`feature/mir-lir`: MIR skeleton — fused value model, 3 arenas/one MirModuleId, phi, closed MirOpcode + opcodeInfo table, create-then-fill builder, instBlock reverse-lookup, finish-time freeze sweep; 6-perspective review; 87/87). **ML2 🟡 cycles 1–3 done** 2026-05-28 (`feature/mir-lir`: comprehensive cycle 3 across 3a (expression closure: Call/Ternary/LogicalAnd-Or + source-map), 3b (lvalue-via-alloca + slot-promoted params), and 3c (MemberAccess/Index/SeqExpr via shared `lowerLvalueAddress`; `AddressOf` delegates; slot-promotion pre-pass also covers `s.field`/`s[i]` bases). Cast deferred — HR emits no Cast nodes yet (real blocker downstream of HR). 88/88 ctest, 30 in mir lowering. **ML2 remaining**: Switch/Break/Continue control flow, addressable globals, cross-fn Call resolution. ML3–ML8 ⏳ — parallel with step 9 |
| 11 | §8 `gen-optimizer` | v1 mandatory: const fold + DCE + copy prop + dominator-tree + liveness | step 10 | ⏳ — parallel with step 12 |
| 12 | [13](./13-assembler-plan%20-%20tbd.md) AS1–AS6 | x86_64 + ARM64 hand-encoder + per-(arch×format) relocation taxonomy | step 10 | ⏳ — parallel with step 11 |
| 13 | [14](./14-linker-plan%20-%20tbd.md) LK1–LK10 | In-tree linker — ELF/PE/Mach-O writers + symbol resolution + relocs + TLS lowering | steps 9, 12, **P1** | ⏳ — largest single chunk of backend work |
| 14 | [15](./15-debug-info-plan%20-%20tbd.md) DB1–DB12 | DWARF 5 (ELF + Mach-O) + PDB (PE) + CFI / SEH / compact-unwind | steps 10, 13 | ⏳ |
| 15 | [16](./16-codesign-publish-plan%20-%20tbd.md) CS1–CS9 | Apple codesign + notarization + Authenticode + RFC 3161 + APK v3 (in-tree crypto via vendored BearSSL) | steps 13, 14 | ⏳ |
| 16 | §8 `program-api` | `dss-code-prime build my-project.dss-project.json` CLI + project loader + clang-quality diag renderer | steps 13, **P1** | 🟦 skeleton; finish blocked |
| **v1.x — unblock when their deps land** | | | | |
| 17 | [18](./18-wasm-plan%20-%20tbd.md) WA1–WA10 | WASM backend via structured-CF markers (no Relooper) | steps 8, 10, 13 | ⏳ |
| 18 | [17](./17-shader-gpu-plan%20-%20tbd.md) SG1–SG10 | SPIR-V codegen + shader HIR + GPU bindings + same-source CPU+GPU dual-lowering | steps 8, 10 | ⏳ |
| 19 | [10](./10-source-translation-plan%20-%20tbd.md) ST1–ST6 | Source-to-source translation via HIR pivot | step 8 | ⏳ |
| 20 | [08](./08-compilation-unit-plan%20-%20tbd.md) CU5 | Multi-language CUs (one binary, mixed source languages, HIR convergence) | step 4 + step 8 | ⏳ v1.1 |
| 21 | [08](./08-compilation-unit-plan%20-%20tbd.md) CU6 + [14](./14-linker-plan%20-%20tbd.md) **LK11** | Cross-CU references — lifts the single-CU-per-binary assumption; shared libraries / DLLs / incremental compilation (paired CU6 ↔ LK11; substrate hooks already in CU1+CU3) | step 4 + step 13 + trigger (artifact profile needing multiple CUs) | ⏳ v1.x |
| **Sidewise — unblocked NOW, can run in parallel** | | | | |
| P1 | [06](./06-artifact-profile-plan%20-%20tbd.md) AP1–AP4 | `artifactProfile` mechanism (registered-set vocab) — **gates steps 13 + 16 acceptance** | — | ⏳ unblocked; parallel-eligible with steps 1–12 |
| P2 | §8 `docker-setup` (residual) | Dockerfile + cross-compile toolchains | — | ⏳ unblocked; cross-cutting |
| P3 | §8 `testing` ARM64 + fuzzing | ARM64 CI legs (G-704..G-706) + nightly fuzzing (G-710) + extended sanitizer coverage (G-709) | — | 🟦 partial; rolls continuously |
| **Reserved — no work until trigger** | | | | |
| R1 | [19](./19-hir-hw-reserved-plan%20-%20tbd.md) | Hardware HIR sibling (VHDL / Verilog / SystemVerilog) | step 8 + trigger | 🔒 |
| R2 | [20](./20-custom-language-reserved-plan%20-%20tbd.md) | User's eventual custom language (vocab + stdlib + runtime decisions) | all v1 + step 17 + step 18 + trigger | 🔒 |
| R3 | [21](./21-runtime-reserved-plan%20-%20tbd.md) | Language runtime (GC / exceptions / coroutines / threading) | step 8 + step 10 + trigger | 🔒 |

> **v1 critical path (first signed binary):** 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → **{9 ‖ 10}** → **{11 ‖ 12}** → 13 → 14 → 15 → 16. **P1 (artifact-profile)** must land before step 13 and step 16 reach acceptance — schedule it anywhere in parallel from step 1 onward.

## 0.2. Deferred & Known-Open Items (registry)

> **Single authoritative list of everything consciously left open as of the pre-HIR gap-closure (2026-05-26).** Each item names WHY it's deferred, the plan/phase that OWNS its eventual closure, and the TRIGGER. Nothing here is a silent gap — it's all tracked. `cheap-config` = closeable any time as additive `.lang.json` config with no engine work (do when a corpus/language needs it); `blocked` = genuinely needs a downstream layer; `feature` = language-surface expansion (v1.x). When an item closes, strike it through with the closing commit.
>
> This registry is the index; the detail lives in the owning plan (linked). Do not duplicate detail here — it drifts.
>
> **Closure pass 2026-05-26:** a "what can be done now" triage closed the items genuinely reachable with the current (pre-HIR) implementation — **D2** (full), **D3** (type-resolution; only the `VARCHAR(N)` *parameter* remains), **D4-AP1** (the `artifactProfiles` schema field + loader), and **D8** (the never-referenced unused-variable warning — the registry had mis-classified this as optimizer-blocked, but SE7's `usesBySymbol` reverse index makes it a pure semantic-layer check). Closed/narrowed rows are struck through below; the surviving rows carry a refined reason + an explicit *when*.

| # | Deferred item | Why deferred (not a silent gap) | Class | Owner / closure | Trigger |
|---|---|---|---|---|---|
| ~~D1~~ | **Pointer / array *type-level* modelling** + `&` addr-of (c-subset) | ✅ **fully closed.** Array half in HR9 (2026-05-27): `int a[10]`→`Array<I32,10>` via `DeclarationRule.arraySuffix` + semantic const-length eval. Pointer half in gap-closure G5 (2026-05-28): `int *p`/`int **p`→`Ptr` via a config-driven `semantics.pointerToken` declarator-DEPTH count in `resolveTypeNode`; `&` added as a prefix operator (AddressOf→Ptr, Deref→pointee result types were already in lowering). **Successor (pinned → D11):** FULL C declarators — function pointers `int (*f)()`, arrays-of-pointers, multi-declarator `int *a, b` — are NOT covered (single-declarator pointer depth only). | feature/config | — (closed; full declarators → D11) | — |
| ~~D2~~ | ~~**Compound-assignment const-correctness** (c-subset)~~ | ✅ **closed 2026-05-26.** Added the 10 compound-assign operator tokens (`+=`/`-=`/`*=`/`/=`/`%=`/`&=`/`\|=`/`^=`/`<<=`/`>>=`) as gated `assignments` entries; `const int x=1; x+=2;` now flags `S_ConstViolation`. The engine `assignByRule` index became multi-entry (one rule → many gated entries) with a load-time guard that an ungated entry must be a rule's sole entry. Pure config + generic engine change. | ~~cheap-config~~ | — | — |
| D3 | ~~**tsql column-type coverage**~~ → **`VARCHAR(N)` parameter only** | ✅ **type-resolution closed 2026-05-26.** A `columnDecl` declaration + `createTableStmt`-as-scope mint columns as typed symbols; `INT`→`I32`, `BIT`→`Bool` via `builtinTypes`, and `VARCHAR`→the `TSQL::Varchar` extension via a NEW generic `builtinTypes.extension` facet (any language maps a type name → a registered `typeExtensions` entry). **Remaining:** only the `VARCHAR(N)` *length parameter* — the tsql grammar's `typeRef` is `alt[IntKw,VarcharKw,BitKw]` and never parses `VARCHAR(50)`, so the extension is built un-parameterized. That is a grammar-expressiveness gap, not a type-system one. (`DECIMAL`/`MONEY`/`DATE`/`NVARCHAR` are not yet in the grammar at all → see D6 language-surface.) | feature (grammar) | a tsql grammar round that parses parameterized type refs, then thread the parsed `N` into `interner().extension(...args)` | tsql needs precise `VARCHAR(N)` typing, or HIR lowering of SQL types |
| D4 | **Driver / project-config / `artifactProfile` (AP2–AP4) / CLI diagnostic rendering** | ✅ **AP1 closed 2026-05-26:** the optional top-level `artifactProfiles: [...]` schema field + loader validation against the registered profile vocabulary (`cli`/`gui`/`lib`/`staticlib`/`script`/`sproc`/`transpile`/`shader`/`hdl`) + a `GrammarSchema::artifactProfiles()` accessor; the three shipped configs declare their profiles. **Remaining (AP2–AP4 + CLI):** nothing yet *consumes* the declared profiles — the `.dss-project.json` loader, the driver that enforces a profile, codegen plumbing, and wiring the existing `DiagnosticReporter::format()` renderer into a CLI are a whole phase, not a gap. | blocked (own phase) | phase #12 `program-api` + [06](./06-artifact-profile-plan%20-%20tbd.md) AP2–AP4; [07](./07-production-readiness-plan%20-%20tbd.md) G-601..G-609 | step 16 (`program-api`) |
| D5 | **c-subset structs / unions / enums** | 🟡 **PULLED FORWARD 2026-05-28** (before ML2) to make ML2's HIR→MIR lowering honor "no real-blocker deferrals" on `MemberAccess`/`ConstructAggregate`/`TypeDecl`. **Kitchen-sink scope**: core structs + typedef'd/anon + designated initializers + unions + enums + first-class MIR aggregates (ExtractValue/InsertValue). **6-PR sub-arc**: **D5.1 core structs ✅** → D5.2 typedef'd/anon → D5.3 designated init → D5.4 unions → D5.5 enums → D5.6/ML2-prelude MIR aggregate opcodes → then ML2. **D5.1 landed in 5 additive checkpoints** (`feature/mir-lir`): `251eca0` (config vocab + `SymbolRecord::fieldIndex`/`structScope`), `9079916` (Pass 1/1.5/2 engine consumption + `S_NotAPointer`/`S_NotAComposite` codes + cross-field loader validation), `09916d5` (`followerRule` operator-table primitive + `MemberAccessRule.operatorToken` discriminator), Cycle 3 (struct schema wired end-to-end through parse+semantic: `struct Foo { ... };` definitions + member access in PARAM and LOCAL VAR positions; struct types compose via `structType(name, fields)`; field uses resolve via Pass 2), and Cycle 4 (HIR MemberAccess lowering: `s.x` → `MemberAccess(Ref, idx)`, `p->x` → `MemberAccess(Deref(p), idx)`; new `HirVerifier::checkMemberAccess` rule for fieldIndex-bounds + base-must-be-composite; defensive lowering check that resolved symbol is actually a field; dot-form + arrow-form + verifier-negative tests). **Scope cut documented**: top-level `struct Foo x;` globals + `struct Foo make()` struct-returning functions deferred to D5.2 — converting `topLevelDecl`/`externDecl` to `typeRefAllowingStruct` would re-introduce the StructKeyword first-token collision with `structDecl` in the `topLevel` alt; D5.2 lands the shared-prefix factoring + typedef'd structs (`typedef struct Foo Foo;`) that lifts the limitation. **D5.2 cycle 1 ✅** added `Identifier` to `typeBase` (typedef-alias at top level + bare-struct-tag usage in type position — known C divergence: single namespace + SE5 alias path means every struct tag is implicitly usable as a bare type name; pinned with 3 tests including the same-name redeclaration negative). **D5.2 cycle 2 attempted + reverted (diagnosis refined cycle 3)**: marking `statement` alt speculative + adding `Identifier` to `typeBaseAllowingStruct` caused widespread parser-test regressions. **Cycle 3 investigation corrected the root cause**: TreeBuilder::Checkpoint DOES already snapshot+rollback the DiagnosticReporter (line 1231 of tree_builder.cpp), so diagnostic rollback is NOT the issue. The real architectural blocker is the **speculative-alt branch-success criterion**: `trySpeculativeBranch` commits the first branch whose frame closes structurally, even if the branch needed internal recovery to get there. For `f(a, b);` inside a block: speculative `statement` tries `varDecl` first (Identifier in FIRST after the typeBase widening); `varDecl` parses `f` as `typeBase`, expects another Identifier, sees `(`, recovers internally (emits diag, advances, frame closes structurally) → probe sees frame closed → commits → rest of input mis-parsed. Fix requires either: (a) tightening the speculative-success criterion (`probe.emittedDiag() || probe.hasInnerErrors()` already exist but aren't all wired), (b) k>1 lookahead disambiguation, or (c) restructuring alts so FIRST sets are unambiguous (defeats the purpose). **(a) attempted cycle 4 + reverted**: adding the missing `probe.emittedDiag()` pre-commit check improved the situation (7→5 failures, no aborts) but left `f(a,b);`-style calls in blocks failing both branches with `P_BacktrackFailed` — deeper interaction with SchemaWalker desync-latch and/or per-probe diagsBefore_ baseline still needs untangling in a focused substrate-tier sub-PR. **Decision**: pivot to ML2 (D5.1 + D5.2 cycle 1 are sufficient for `MemberAccess` + `TypeDecl` end-to-end; `ConstructAggregate` stays a real-blocker defer until D5.3). **ML2 cycle 1 ✅ landed 2026-05-28**: `src/mir/lowering/` OBJECT lib + straight-line vertical slice through c-subset (Function + Block + Return + Literal + Ref + 16 BinaryOps + implicit-void-return synthesis + fail-loud unsupported handling). 88/88 ctest. See plan 12 ML2 row. **All remaining D5 sub-features** (block-scope alias, anonymous structs `struct { … } var;`, top-level `struct Foo x;` via shared-prefix factoring, designated initializers in init context) **share the same speculative-alt or multi-token-disambiguation engine need**. Recommended path: either focused substrate-tier sub-PR on speculative-alt diagnostic rollback (unlocks every remaining D5 feature), OR pivot to ML2 with D5.1 + D5.2 cycle 1 substrate (sufficient for `MemberAccess` + `TypeDecl` lowering; `ConstructAggregate` stays a documented fail-loud-defer in ML2 until D5.3). **87/87 ctest** throughout. | feature (multi-PR) | the 6-PR D5 sub-arc (D5.1 ✅; D5.2 in progress) | maximally honest c-subset for ML2 |
| D6 | **tsql JOINs / subqueries / CTEs / stored-procs / transactions** | Language-surface expansion; current tsql is single-table query/DML + CREATE TABLE. | feature (v1.x) | a future `tsql-subset` expansion round | same |
| ~~D7~~ | **Ternary `? :` (mixfix)** | ✅ **closed 2026-05-28 (gap-closure G6).** Added `OperatorArity::Ternary` (4th arity, fits the 2-bit key-pack) + `OperatorTable::Entry.ternaryMiddle` + an optional `expr.wrapperRules.ternary`; a `DefaultPrattWalker` mixfix branch (condition at prec+1, middle at minPrec 0, else at prec → right-assoc) + `lowerTernary` → the existing HIR `Ternary` node. Config-driven, no `?:` special-casing. | (closed) | — | — |
| D8 | ~~**Unused-variable**~~ → **write-only / dead-store only** | ✅ **never-referenced closed 2026-05-26** (registry had this mis-classified as optimizer-blocked). A config-driven `warnIfUnused` flag on `DeclarationRule` + a post-pass-2 sweep of SE7's `usesBySymbol` reverse index emits `S_UnusedVariable` (a WARNING) for an opted-in symbol with an empty use-set — no CFG needed. Per-decl opt-in (c-subset locals, not params/globals/columns); proven generic via Synth5. **Remaining:** write-only / dead-store ("assigned but never read") and dead-code — those DO need dataflow/liveness, so they stay with the optimizer. | blocked (optimizer) | phase #10 `gen-optimizer`; [07](./07-production-readiness-plan%20-%20tbd.md) G-210 | optimizer phase (CFG/liveness exists) |
| D9 | **Use-before-init / unreachable-after-return** | Needs a control-flow graph; the semantic layer is pre-CFG. HIR/MIR will have the CFG to do this strength of analysis. | blocked (HIR/MIR) | phase #9 HIR + phase #10 optimizer | CFG exists |
| D10 | **CU6 cross-CU references / incremental rebuild** | v1 is single-CU-per-binary; cross-CU + incremental are paired CU6 ↔ LK11 substrate hooks already reserved. | blocked (v1.x) | [08](./08-compilation-unit-plan%20-%20tbd.md) CU6 + [14](./14-linker-plan%20-%20tbd.md) LK11 | artifact profile needing multiple CUs |
| D11 | **Full C declarators (c-subset)** — function pointers `int (*f)()`, arrays-of-pointers `int *a[10]`, multi-declarator `int *a, b` | Successor to the now-closed D1. Gap-closure G5 (2026-05-28) gave c-subset single-declarator pointer DEPTH (`int *p`/`int **p` → `Ptr` via `semantics.pointerToken`) and HR9 gave array suffixes (`int a[10]`) — but the FULL C declarator grammar (parenthesized declarators binding a name through pointer+function+array layers, and comma-separated multi-declarators where `*` binds per-declarator) is a distinct grammar+semantic round. Not failing loud today: these forms simply don't parse (a parse error), they don't miscompile. Config + grammar work, no downstream phase needed. | feature (grammar) | a future c-subset declarator-grammar round; relates to D5 (structs) language-surface expansion | a corpus program needs function pointers / arrays-of-pointers / multi-declarators |

> **Pre-HIR readiness:** none of the remaining items blocks opening **HR1** (HIR). After the 2026-05-26 closure pass the open set is: **D1** (pointer/array type-level — feature/grammar round, *when* a corpus needs typed pointers or HIR lowers them), **D3-residual** (`VARCHAR(N)` parameter — feature/grammar round, *when* tsql needs precise length typing), **D4-residual** (AP2–AP4 + CLI — owned by phase #12 `program-api`), **D5/D6** (c-subset and tsql language-surface expansion — v1.x feature rounds, *when* the credibility-as-real-language goal or a corpus needs them), **D7** (ternary/mixfix — its own schema-v3 round, *when* a shipped language needs it), **D9** (use-before-init / unreachable — *when* HIR's CFG exists), and **D10** (cross-CU / incremental — *when* an artifact profile needs multiple CUs). D9 is the only one HIR itself helps unblock.

## 1. Vision & Overview

**DSS Code Prime** is a universal, configurable compiler written in C++. Its core design principle is that **both the source language and the target platform are configurable**, making it a single compiler engine capable of compiling _any_ defined language to _any_ supported target.

### Key Design Goals

| Goal | Description |
|---|---|
| **Language-Agnostic Input** | Any programming language can be defined via a JSON configuration file that describes its full syntax, grammar, type system, and semantics. |
| **Multi-Target Output** | Compile to Windows, Linux, macOS, iOS, Android, and Web (WASM) — each with their accessible processor architectures (x86_64, ARM64, RISC-V, WASM, etc.). |
| **Three-Phase Analysis** | Classical compiler pipeline: Lexical → Syntactic → Semantic analysis. |
| **Portable Build** | Runs natively on Windows, Linux, and macOS. Docker image provided for reproducible cross-compilation environments. |
| **Extensible Architecture** | New languages are added via config files; new targets via pluggable backend modules. |

---

## 2. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                          DSS Code Prime                                  │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │                        program (Public API)                        │  │
│  │  Receives: project file | file list | directory path               │  │
│  │  Receives: source language name                                    │  │
│  │  Receives: target platform(s) — one or many                        │  │
│  │  Resolves source files by extension, dispatches compilation        │  │
│  └───────────────────────────┬────────────────────────────────────────┘  │
│                              │                                           │
│                              ▼                                           │
│  ┌──────────────┐   ┌───────────────────────┐   ┌───────────────┐       │
│  │ source-config │──▶│   source-factory      │──▶│  core/types   │       │
│  │  (JSON files) │   │ (parser + validator)  │   │  (in-memory   │       │
│  └──────────────┘   └───────────────────────┘   │   lang model) │       │
│                                                  └──────┬────────┘       │
│                                                         │                │
│  ┌──────────────────────────────────────────────────────┘                │
│  │                                                                       │
│  ▼                                                                       │
│  ┌────────────┐   ┌──────────────────────────────────┐                   │
│  │ tokenizer  │──▶│           analysis                │                   │
│  │ (char→tok) │   │  ┌─────────┬──────────┬────────┐ │                   │
│  └────────────┘   │  │ lexical │syntactic │semantic│ │                   │
│                    │  │ (rules  │(parser + │(types, │ │                   │
│                    │  │  check) │ AST)     │ scope) │ │                   │
│                    │  └─────────┴──────────┴────────┘ │                   │
│                    └──────────────┬───────────────────┘                   │
│                                  │                                       │
│                                  ▼                                       │
│                    ┌─────────────────────────────────────────────────┐   │
│                    │                    gen                           │   │
│                    │  ┌──────────────┬────────────┬───────────────┐  │   │
│                    │  │ intermediate │ optimizer  │     link      │  │   │
│                    │  │ (AST → IR)   │ (IR passes │ (target emit  │  │   │
│                    │  │              │  & xforms) │  + linking)   │  │   │
│                    │  └──────────────┴────────────┴───────────────┘  │   │
│                    └─────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────┘
```

### Data Flow Summary

```
User Input (via program API)
    │  ─ project file (.dsp), file list, or directory path
    │  ─ source language name (e.g. "ExampleLang")
    │  ─ target platform(s) (e.g. ["linux-x86_64", "web-wasm"])
    │
    ▼
[program] ── resolves input → discovers source files by extension
    │         loads language config via source-factory
    │         iterates targets, dispatches per-file compilation
    │
    ▼ (for each source file × each target)
[source-factory] ── loads language definition from JSON
    │
    ▼
[tokenizer] ── raw characters → token stream
    │
    ▼
[analysis/lexical] ── validates tokens against language rules
    │
    ▼
[analysis/syntactic] ── token stream → Abstract Syntax Tree (AST)
    │
    ▼
[analysis/semantic] ── type checking, scope resolution, semantic validation
    │
    ▼
[gen/intermediate] ── AST → Intermediate Representation (IR)
    │
    ▼
[gen/optimizer] ── IR optimization passes (constant folding, DCE, CSE, etc.)
    │
    ▼
[gen/link] ── optimized IR → target-specific machine code, linking, output binary
    │
    ▼
Executable / Library / WASM module (per target)
```

---

## 3. Project File Structure

```
dss-code-prime/
│
├── .plans/
│   └── 00-compiler-implementation-plan - tbd.md      # This document
│
├── CMakeLists.txt                           # Root CMake build (cross-platform)
├── README.md
├── .gitignore
│
├── docker/
│   ├── Dockerfile                           # Multi-stage build image
│   ├── docker-compose.yml                   # Dev/CI compose file
│   └── toolchains/                          # CMake toolchain files for cross-compile
│       ├── linux-x86_64.cmake
│       ├── linux-arm64.cmake
│       ├── windows-x86_64.cmake
│       ├── macos-x86_64.cmake
│       ├── macos-arm64.cmake
│       ├── ios-arm64.cmake
│       ├── android-arm64.cmake
│       └── web-wasm.cmake
│
├── docs/
│   ├── architecture.md                      # Detailed architecture documentation
│   ├── language-config-spec.md              # JSON config specification
│   └── target-config-spec.md                # Target platform specification
│
├── libs/                                    # Third-party dependencies
│   └── README.md                            # Dependency documentation
│
├── src/
│   ├── CMakeLists.txt                       # Src-level CMake
│   ├── main.cpp                             # CLI entry point (thin — delegates to program)
│   │
│   ├── program/                             # ── Public API / Driver ──
│   │   ├── CMakeLists.txt
│   │   ├── program.hpp                      # Public API: the open interface for compilation
│   │   ├── program.cpp                      # Orchestrates input resolution + multi-target compilation
│   │   ├── input_resolver.hpp               # Resolves project / file list / directory → source files
│   │   ├── input_resolver.cpp
│   │   ├── project_file.hpp                 # Parses .dsp project file (project definition)
│   │   ├── project_file.cpp
│   │   ├── compilation_request.hpp          # Request model: source lang, files, targets
│   │   └── compilation_result.hpp           # Result model: per-target output paths + diagnostics
│   │
│   ├── core/                                # ── Shared types & utilities ──
│   │   ├── CMakeLists.txt
│   │   ├── compiler.hpp                     # Top-level compiler orchestrator interface
│   │   ├── compiler.cpp                     # Orchestrates the full pipeline
│   │   ├── types/
│   │   │   ├── token.hpp                    # Token type (kind, value, location)
│   │   │   ├── ast.hpp                      # AST node definitions (base + variants)
│   │   │   ├── ir.hpp                       # Intermediate Representation node types
│   │   │   ├── symbol.hpp                   # Symbol table entry type
│   │   │   ├── source_location.hpp          # File/line/col position tracking
│   │   │   └── target_info.hpp              # Target OS + processor descriptor
│   │   ├── error/
│   │   │   ├── error.hpp                    # Error/Warning/Info diagnostic type
│   │   │   ├── error.cpp
│   │   │   ├── error_reporter.hpp           # Collects and formats diagnostics
│   │   │   └── error_reporter.cpp
│   │   └── utils/
│   │       ├── file_io.hpp                  # Cross-platform file reading utilities
│   │       ├── file_io.cpp
│   │       ├── string_utils.hpp             # String manipulation helpers
│   │       └── string_utils.cpp
│   │
│   ├── source-config/                       # ── Language Definition Files ──
│   │   ├── README.md                        # How to write a language config
│   │   ├── schemas/
│   │   │   └── language-schema.json         # JSON Schema for validation
│   │   └── languages/
│   │       └── example.lang.json            # Example language definition
│   │
│   ├── source-factory/                # ── Config Parser & Loader ──
│   │   ├── CMakeLists.txt
│   │   ├── config_reader.hpp                # Public API: load(path) → LanguageConfig
│   │   ├── config_reader.cpp                # JSON parsing + hydration into models
│   │   ├── models/
│   │   │   ├── language_config.hpp          # Root model: holds full language definition
│   │   │   ├── token_definition.hpp         # Defines a token kind (regex, keyword list, etc.)
│   │   │   ├── grammar_rule.hpp             # BNF/PEG-style production rule model
│   │   │   ├── type_system_config.hpp       # Primitive types, type rules, coercion
│   │   │   ├── operator_definition.hpp      # Operators: symbol, precedence, associativity
│   │   │   └── semantic_rule.hpp            # Semantic constraints (e.g. "variables must be declared")
│   │   └── validators/
│   │       ├── config_validator.hpp          # Validates loaded config for completeness/consistency
│   │       └── config_validator.cpp
│   │
│   ├── tokenizer/                           # ── Tokenization (char stream → tokens) ──
│   │   ├── CMakeLists.txt
│   │   ├── tokenizer.hpp                    # Public API: tokenize(source, lang_config) → TokenStream
│   │   ├── tokenizer.cpp                    # Core tokenization engine (driven by token definitions)
│   │   ├── token_stream.hpp                 # Iterable token container with peek/advance
│   │   ├── token_stream.cpp
│   │   ├── source_reader.hpp                # Buffered character reader with location tracking
│   │   └── source_reader.cpp
│   │
│   ├── analysis/                            # ── Three-Phase Analysis ──
│   │   ├── CMakeLists.txt
│   │   │
│   │   ├── lexical/                         # Phase 1: Lexical Analysis
│   │   │   ├── lexer.hpp                    # Public API: validates & classifies token stream
│   │   │   ├── lexer.cpp                    # Applies lexical rules from language config
│   │   │   ├── lexical_rules.hpp            # Rule engine: keyword matching, literal validation
│   │   │   └── lexical_rules.cpp
│   │   │
│   │   ├── syntactic/                       # Phase 2: Syntactic Analysis (Parsing)
│   │   │   ├── parser.hpp                   # Public API: parse(tokens, grammar) → AST
│   │   │   ├── parser.cpp                   # Recursive descent / table-driven parser
│   │   │   ├── grammar.hpp                  # Runtime grammar representation (from config)
│   │   │   ├── grammar.cpp                  # Grammar loading and first/follow set computation
│   │   │   ├── ast_builder.hpp              # Constructs AST nodes during parsing
│   │   │   └── ast_builder.cpp
│   │   │
│   │   └── semantic/                        # Phase 3: Semantic Analysis
│   │       ├── semantic_analyzer.hpp        # Public API: analyze(AST) → annotated AST
│   │       ├── semantic_analyzer.cpp        # Orchestrates all semantic passes
│   │       ├── symbol_table.hpp             # Scoped symbol table (variables, functions, types)
│   │       ├── symbol_table.cpp
│   │       ├── type_checker.hpp             # Type inference and checking engine
│   │       ├── type_checker.cpp
│   │       ├── scope_resolver.hpp           # Scope entry/exit, name resolution
│   │       └── scope_resolver.cpp
│   │
│   └── gen/                                 # ── Code Generation ──
│       ├── CMakeLists.txt
│       │
│       ├── intermediate/                    # IR Generation
│       │   ├── ir_generator.hpp             # Public API: generate(AST) → IR
│       │   ├── ir_generator.cpp             # Walks annotated AST, emits IR nodes
│       │   └── ir_node.hpp                  # IR instruction set (three-address code style)
│       │
│       ├── optimizer/                       # IR Optimization (target-independent)
│       │   ├── optimizer.hpp                # Public API: optimize(IR) → optimized IR
│       │   ├── optimizer.cpp                # Runs the optimization pass pipeline
│       │   ├── pass.hpp                     # Abstract base class for optimization passes
│       │   ├── passes/                      # Individual optimization pass implementations
│       │   │   ├── constant_folding.hpp     # Evaluate compile-time constant expressions
│       │   │   ├── constant_folding.cpp
│       │   │   ├── constant_propagation.hpp # Replace variables with known constant values
│       │   │   ├── constant_propagation.cpp
│       │   │   ├── dead_code_elimination.hpp # Remove unreachable / unused instructions
│       │   │   ├── dead_code_elimination.cpp
│       │   │   ├── common_subexpr_elim.hpp  # Reuse already-computed expressions
│       │   │   ├── common_subexpr_elim.cpp
│       │   │   ├── copy_propagation.hpp     # Replace copies with original values
│       │   │   ├── copy_propagation.cpp
│       │   │   ├── strength_reduction.hpp   # Replace expensive ops with cheaper equivalents
│       │   │   ├── strength_reduction.cpp
│       │   │   ├── loop_invariant_motion.hpp # Hoist invariant computations out of loops
│       │   │   └── loop_invariant_motion.cpp
│       │   └── analysis/                    # IR analysis utilities used by passes
│       │       ├── cfg_builder.hpp          # Build control-flow graph from IR
│       │       ├── cfg_builder.cpp
│       │       ├── liveness.hpp             # Variable liveness analysis
│       │       ├── liveness.cpp
│       │       ├── reaching_defs.hpp        # Reaching definitions analysis
│       │       └── reaching_defs.cpp
│       │
│       └── link/                            # Target Code Emission & Linking
│           ├── linker.hpp                   # Public API: link(IR, target) → output binary
│           ├── linker.cpp                   # Resolves symbols, invokes target emitter
│           ├── target_config.hpp            # Target descriptor (OS, arch, ABI)
│           ├── target_config.cpp
│           └── targets/                     # Per-target code emitters
│               ├── target_base.hpp          # Abstract base class for all targets
│               ├── target_windows_x86_64.hpp
│               ├── target_windows_x86_64.cpp
│               ├── target_linux_x86_64.hpp
│               ├── target_linux_x86_64.cpp
│               ├── target_linux_arm64.hpp
│               ├── target_linux_arm64.cpp
│               ├── target_macos_x86_64.hpp
│               ├── target_macos_x86_64.cpp
│               ├── target_macos_arm64.hpp
│               ├── target_macos_arm64.cpp
│               ├── target_ios_arm64.hpp
│               ├── target_ios_arm64.cpp
│               ├── target_android_arm64.hpp
│               ├── target_android_arm64.cpp
│               ├── target_web_wasm.hpp
│               └── target_web_wasm.cpp
│
└── tests/                                   # ── Test Suite (mirrors src/) ──
    ├── CMakeLists.txt
    ├── program/
    │   ├── test_program.cpp
    │   ├── test_input_resolver.cpp
    │   └── test_project_file.cpp
    ├── core/
    │   ├── test_error_reporter.cpp
    │   └── test_types.cpp
    ├── source-factory/
    │   ├── test_config_reader.cpp
    │   └── test_config_validator.cpp
    ├── tokenizer/
    │   ├── test_tokenizer.cpp
    │   ├── test_token_stream.cpp
    │   └── test_source_reader.cpp
    ├── analysis/
    │   ├── lexical/
    │   │   └── test_lexer.cpp
    │   ├── syntactic/
    │   │   ├── test_parser.cpp
    │   │   └── test_grammar.cpp
    │   └── semantic/
    │       ├── test_semantic_analyzer.cpp
    │       ├── test_symbol_table.cpp
    │       └── test_type_checker.cpp
    └── gen/
        ├── intermediate/
        │   └── test_ir_generator.cpp
        ├── optimizer/
        │   ├── test_optimizer.cpp
        │   ├── test_constant_folding.cpp
        │   ├── test_dead_code_elimination.cpp
        │   └── test_cfg_builder.cpp
        └── link/
            └── test_linker.cpp
```

---

## 4. Module Detailed Documentation

---

### 4.1 `program/` — Public API & Driver

The **program** module is the open API layer — the entry point that external consumers (CLI, IDE integrations, CI pipelines) use to invoke compilation. It handles **what** to compile and **where** to output, then delegates the **how** to the compiler pipeline.

#### 4.1.1 Input Modes

The API accepts three input modes:

| Mode | Description | Example |
|---|---|---|
| **Project file** | A `.dsp` (DSS Project) JSON file that declares source language, source directories/files, targets, and output configuration. | `program.compile("myapp.dsp")` |
| **File list** | An explicit list of source file paths, plus language name and target(s). | `program.compile(files, "ExampleLang", targets)` |
| **Directory** | A directory path — the program scans it recursively for files matching the language's `fileExtensions` from the config. | `program.compile("./src/", "ExampleLang", targets)` |

#### 4.1.2 `program.hpp/.cpp` — Public API

```cpp
class Program {
public:
    Program();
    ~Program();

    /// Compile from a .dsp project file (self-contained — language, files, targets defined inside).
    CompilationResult compileProject(const std::string& projectFilePath);

    /// Compile an explicit list of source files for the given language to the given target(s).
    CompilationResult compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<TargetInfo>& targets
    );

    /// Compile all matching source files found in a directory (recursive scan by extension).
    CompilationResult compileDirectory(
        const std::string& directoryPath,
        const std::string& languageName,
        const std::vector<TargetInfo>& targets
    );

private:
    /// Shared implementation: compile resolved files for each target.
    CompilationResult compileResolved(
        const std::vector<std::string>& resolvedFiles,
        const LanguageConfig& langConfig,
        const std::vector<TargetInfo>& targets
    );
};
```

**Behavior:**
- Loads the language config once (via `source-factory`), reuses it for all files
- For each target in the targets list, runs the full pipeline (tokenize → analyze → gen) per source file
- Collects per-file, per-target results into a single `CompilationResult`
- Thread-safe: files can be compiled in parallel (future optimization)

#### 4.1.3 `input_resolver.hpp/.cpp` — Input Resolution

```cpp
class InputResolver {
public:
    /// Resolve a directory to a list of source files matching the given extensions.
    static std::vector<std::string> resolveDirectory(
        const std::string& directoryPath,
        const std::vector<std::string>& fileExtensions,
        bool recursive = true
    );

    /// Validate that all files in a list exist and are readable.
    static std::vector<std::string> validateFiles(const std::vector<std::string>& filePaths);

    /// Resolve a mixed input (could be a file, directory, or glob pattern).
    static std::vector<std::string> resolve(
        const std::string& input,
        const std::vector<std::string>& fileExtensions
    );
};
```

**Responsibilities:**
- Recursively scans directories for files matching configured extensions (e.g. `.exl`, `.example`)
- Validates file existence and read permissions
- Supports glob patterns (e.g. `src/**/*.exl`)
- Returns sorted, deduplicated absolute paths
- Cross-platform (Windows `\` vs POSIX `/`)

#### 4.1.4 `project_file.hpp/.cpp` — Project File Parser

A `.dsp` (DSS Project) file is a JSON document that bundles all compilation parameters:

```jsonc
{
  "project": {
    "name": "MyApplication",
    "version": "1.0.0"
  },
  "source": {
    "language": "ExampleLang",
    "include": [
      "src/",
      "lib/helpers.exl"
    ],
    "exclude": [
      "src/tests/",
      "src/deprecated/"
    ]
  },
  "targets": [
    { "os": "linux",   "arch": "x86_64" },
    { "os": "windows", "arch": "x86_64" },
    { "os": "web",     "arch": "wasm"   }
  ],
  "output": {
    "directory": "build/",
    "nameTemplate": "${project.name}-${target.os}-${target.arch}"
  }
}
```

```cpp
class ProjectFile {
public:
    /// Parse a .dsp project file.
    static ProjectFile load(const std::string& projectFilePath);

    std::string projectName;
    std::string projectVersion;
    std::string languageName;
    std::vector<std::string> includePaths;   // files and/or directories
    std::vector<std::string> excludePaths;   // exclusion patterns
    std::vector<TargetInfo> targets;
    std::string outputDirectory;
    std::string nameTemplate;
};
```

#### 4.1.5 `compilation_request.hpp` — Request Model

```cpp
struct CompilationRequest {
    std::vector<std::string> sourceFiles;    // Resolved absolute paths
    std::string languageName;                // Language config to load
    std::vector<TargetInfo> targets;         // One or more targets
    std::string outputDirectory;             // Where to write outputs
    std::string nameTemplate;                // Output naming pattern (optional)
};
```

#### 4.1.6 `compilation_result.hpp` — Result Model

```cpp
struct FileTargetResult {
    std::string sourceFile;
    TargetInfo target;
    bool success;
    std::string outputPath;                  // Path to generated binary (on success)
    std::vector<Error> diagnostics;          // Errors, warnings, info for this file+target
};

struct CompilationResult {
    bool overallSuccess;                     // True if all files × all targets succeeded
    std::vector<FileTargetResult> results;   // Per-file, per-target results
    std::vector<Error> globalDiagnostics;    // Diagnostics not tied to a specific file

    /// Convenience: get all results for a specific target.
    std::vector<FileTargetResult> resultsForTarget(const TargetInfo& target) const;

    /// Convenience: get all failed results.
    std::vector<FileTargetResult> failures() const;
};
```

---

### 4.2 `core/` — Shared Types & Utilities

The **core** module contains types and utilities shared across every stage of the compiler. Nothing in `core/` depends on any other `src/` module — it is the foundation layer.

#### 4.2.1 `core/compiler.hpp/.cpp` — Compiler Orchestrator

The top-level class that wires the entire pipeline together:

```cpp
class Compiler {
public:
    /// Construct with a language config path and target specification.
    Compiler(const std::string& langConfigPath, const TargetInfo& target);

    /// Compile a source file, returning diagnostics and (on success) the output path.
    CompileResult compile(const std::string& sourceFilePath);

private:
    std::unique_ptr<LanguageConfig> langConfig_;
    TargetInfo target_;
    ErrorReporter reporter_;
};
```

**Responsibilities:**
- Loads the language configuration via `source-factory`
- Orchestrates: Tokenize → Lex → Parse → Semantic → IR Gen → Optimize → Link
- Collects and returns diagnostics from all phases

#### 4.2.2 `core/types/` — Fundamental Data Types

> **AMENDED by [`01-tree-node-model-plan - ok.md`](./01-tree-node-model-plan - ok.md) §5.1–§5.11.** The `ast.hpp` line below is replaced by the full tree/node model (`Tree`, `Node`, `NodeFlags`, `TreeBuilder`, `TreeCursor`, `NodeAttribute<T>`, typed views). `token.hpp` is replaced by the sub-plan's `Token` + `CoreTokenKind` + `SchemaTokenId` split. `source_location.hpp` is replaced by `SourceBuffer` + `SourceSpan` (byte offsets; line/col derived). The list below is kept for historical reference only; new work follows the sub-plan.

| File | Purpose |
|---|---|
| `token.hpp` | `Token` struct: `{ TokenKind kind; std::string lexeme; SourceLocation loc; }` — the universal token representation used from tokenizer through parser. |
| `ast.hpp` | AST node hierarchy. Base `ASTNode` with variants: `LiteralNode`, `BinaryExprNode`, `UnaryExprNode`, `IdentifierNode`, `FunctionDeclNode`, `BlockNode`, `IfNode`, `WhileNode`, `ReturnNode`, etc. Uses a visitor pattern for traversal. |
| `ir.hpp` | IR instruction types: `IRInstruction` base with `IRBinaryOp`, `IRUnaryOp`, `IRLoad`, `IRStore`, `IRCall`, `IRBranch`, `IRLabel`, `IRReturn`, etc. Three-address code style. |
| `symbol.hpp` | `Symbol` struct: `{ std::string name; TypeInfo type; ScopeLevel scope; SourceLocation declLoc; bool isInitialized; }` |
| `source_location.hpp` | `SourceLocation` struct: `{ std::string filePath; size_t line; size_t column; }` — used in every diagnostic message. |
| `target_info.hpp` | `TargetInfo` struct: `{ TargetOS os; TargetArch arch; std::string abi; }` — describes the compilation target. |

#### 4.2.3 `core/error/` — Diagnostic System

> **AMENDED.** Replaced by `ParseDiagnostic` + `DiagnosticReporter` + `DiagnosticPolicy` in [01-tree-node-model-plan - ok.md §5.13–§5.14](./01-tree-node-model-plan - ok.md). The diagnostic system is now structured (`expected`/`actual`/`scopeStack`/`related`), uses a stable `DiagnosticCode` enum, supports per-code suppression/promotion, and lives at `src/core/types/`. Drop `src/core/error/` from the layout.

A centralized error reporting system used by every compiler phase:

- **`Error`**: Holds severity (Error/Warning/Info/Hint), message, source location, and an error code.
- **`ErrorReporter`**: Accumulates diagnostics, supports formatted output, can abort on first error or collect all.

#### 4.2.4 `core/utils/` — Cross-Platform Utilities

- **`file_io`**: Read file to string, check existence, resolve paths (abstracts Windows/POSIX differences).
- **`string_utils`**: UTF-8 aware helpers, escape sequences, string trimming.

---

### 4.3 `source-config/` — Language Definition Files (JSON)

> **AMENDED by [01-tree-node-model-plan - ok.md §5.12](./01-tree-node-model-plan - ok.md).** The config schema below describes the *original* design (BNF rules, `lexical`/`syntactic`/`semantic` top-level sections). The sub-plan defines the *current* schema: a `tokens` map with multi-typed lexeme meanings (priority-tiebroken), `keywords`, `scopes` (validity rules + opens/closesScope), and `shapes` (the expected node tree with `sequence` / `alt` / `optional` / `repeat`). The schema is versioned via `dssSchemaVersion`. Configs are loaded via `GrammarSchema::loadFromFile` and produce a `Result<…>` with `C####` diagnostics on malformed input. The JSON sketch in this section is kept for historical context only.

This directory contains the **JSON configuration files** that define programming languages. Each file fully describes a language's lexical, syntactic, and semantic rules — the compiler engine reads these at startup.

#### 4.3.1 JSON Schema Structure (`schemas/language-schema.json`)

The schema defines the expected shape of every language config file. Top-level sections:

```jsonc
{
  "$schema": "language-schema.json",
  "language": {
    "name": "ExampleLang",
    "version": "1.0.0",
    "fileExtensions": [".exl", ".example"]
  },

  "lexical": {
    "comments": {
      "singleLine": "//",
      "multiLineStart": "/*",
      "multiLineEnd": "*/"
    },
    "whitespace": {
      "significant": false,
      "newlineSignificant": false
    },
    "literals": {
      "integer": { "pattern": "[0-9]+", "suffixes": ["u", "l", "ul"] },
      "float": { "pattern": "[0-9]+\\.[0-9]+", "suffixes": ["f", "d"] },
      "string": { "delimiters": ["\""], "escapeChar": "\\", "multiline": false },
      "char": { "delimiters": ["'"], "escapeChar": "\\" },
      "boolean": { "trueKeyword": "true", "falseKeyword": "false" },
      "null": { "keyword": "null" }
    },
    "keywords": [
      "if", "else", "while", "for", "return", "function",
      "var", "const", "class", "import", "export"
    ],
    "operators": [
      { "symbol": "+",  "precedence": 10, "associativity": "left",  "type": "binary" },
      { "symbol": "-",  "precedence": 10, "associativity": "left",  "type": "binary" },
      { "symbol": "*",  "precedence": 20, "associativity": "left",  "type": "binary" },
      { "symbol": "/",  "precedence": 20, "associativity": "left",  "type": "binary" },
      { "symbol": "=",  "precedence": 1,  "associativity": "right", "type": "binary" },
      { "symbol": "==", "precedence": 5,  "associativity": "left",  "type": "binary" },
      { "symbol": "!=", "precedence": 5,  "associativity": "left",  "type": "binary" },
      { "symbol": "!",  "precedence": 30, "associativity": "right", "type": "unary_prefix" },
      { "symbol": "++", "precedence": 30, "associativity": "right", "type": "unary_prefix" }
    ],
    "delimiters": {
      "statementTerminator": ";",
      "blockStart": "{",
      "blockEnd": "}",
      "parenStart": "(",
      "parenEnd": ")",
      "bracketStart": "[",
      "bracketEnd": "]",
      "separator": ","
    },
    "identifiers": {
      "startPattern": "[a-zA-Z_]",
      "continuePattern": "[a-zA-Z0-9_]",
      "caseSensitive": true
    }
  },

  "syntactic": {
    "grammar": {
      "format": "BNF",
      "rules": [
        { "name": "program",          "production": "statement*" },
        { "name": "statement",        "production": "variableDecl | functionDecl | expressionStmt | ifStmt | whileStmt | returnStmt | block" },
        { "name": "variableDecl",     "production": "('var' | 'const') IDENTIFIER (':' type)? ('=' expression)? ';'" },
        { "name": "functionDecl",     "production": "'function' IDENTIFIER '(' paramList? ')' (':' type)? block" },
        { "name": "paramList",        "production": "param (',' param)*" },
        { "name": "param",            "production": "IDENTIFIER ':' type" },
        { "name": "block",            "production": "'{' statement* '}'" },
        { "name": "ifStmt",           "production": "'if' '(' expression ')' block ('else' (ifStmt | block))?" },
        { "name": "whileStmt",        "production": "'while' '(' expression ')' block" },
        { "name": "returnStmt",       "production": "'return' expression? ';'" },
        { "name": "expressionStmt",   "production": "expression ';'" },
        { "name": "expression",       "production": "assignment" },
        { "name": "assignment",       "production": "equality ('=' assignment)?" },
        { "name": "equality",         "production": "comparison (('==' | '!=') comparison)*" },
        { "name": "comparison",       "production": "addition (('<' | '>' | '<=' | '>=') addition)*" },
        { "name": "addition",         "production": "multiplication (('+' | '-') multiplication)*" },
        { "name": "multiplication",   "production": "unary (('*' | '/') unary)*" },
        { "name": "unary",            "production": "('!' | '-' | '++') unary | primary" },
        { "name": "primary",          "production": "INTEGER | FLOAT | STRING | BOOLEAN | NULL | IDENTIFIER | '(' expression ')' | functionCall" },
        { "name": "functionCall",     "production": "IDENTIFIER '(' argList? ')'" },
        { "name": "argList",          "production": "expression (',' expression)*" },
        { "name": "type",             "production": "IDENTIFIER ('<' typeList '>')?" },
        { "name": "typeList",         "production": "type (',' type)*" }
      ]
    }
  },

  "semantic": {
    "typeSystem": {
      "primitiveTypes": ["int", "float", "double", "string", "bool", "void"],
      "typeInference": true,
      "implicitConversions": [
        { "from": "int", "to": "float" },
        { "from": "int", "to": "double" },
        { "from": "float", "to": "double" }
      ]
    },
    "scoping": {
      "model": "block",
      "allowShadowing": true,
      "hoisting": false
    },
    "rules": [
      { "id": "VAR_DECL_BEFORE_USE",     "description": "Variables must be declared before use", "severity": "error" },
      { "id": "TYPE_MISMATCH",            "description": "Assignment type must match or be implicitly convertible", "severity": "error" },
      { "id": "CONST_REASSIGNMENT",       "description": "Cannot reassign a const variable", "severity": "error" },
      { "id": "RETURN_TYPE_MATCH",        "description": "Return value must match function return type", "severity": "error" },
      { "id": "UNUSED_VARIABLE",          "description": "Variable declared but never used", "severity": "warning" },
      { "id": "UNREACHABLE_CODE",         "description": "Code after return statement", "severity": "warning" }
    ]
  }
}
```

#### 4.3.2 `languages/` — Shipped Language Definitions

Initially ships with `example.lang.json` as a reference implementation. Future language configs (e.g., a C-subset, a scripting language) can be added here.

---

### 4.4 `source-factory/` — Config Parser & Loader

> **AMENDED.** `LanguageConfig`, the `models/` directory, and `ConfigValidator` are **replaced** by `GrammarSchema` defined in [01-tree-node-model-plan - ok.md §5.12](./01-tree-node-model-plan - ok.md). `src/source-factory/models/` and `src/source-factory/validators/` are dropped from the layout. What remains of `source-factory/` is a thin facade: it resolves a language name (e.g. `"csharp"`) to a config-file path (shipped under `src/source-config/languages/`) and calls `GrammarSchema::loadFromFile`. JSON parsing happens inside `grammar_schema_json.cpp` in `core/types/`; nlohmann/json never leaks past that translation unit.

Reads a `.lang.json` file and hydrates it into a strongly-typed C++ object model.

#### 4.4.1 `config_reader.hpp/.cpp`

```cpp
class ConfigReader {
public:
    /// Load and parse a language config file. Throws on I/O or parse error.
    static std::unique_ptr<LanguageConfig> load(const std::string& configFilePath);

private:
    static void parseLexicalSection(const json& j, LanguageConfig& config);
    static void parseSyntacticSection(const json& j, LanguageConfig& config);
    static void parseSemanticSection(const json& j, LanguageConfig& config);
};
```

**JSON library:** [nlohmann/json](https://github.com/nlohmann/json) (header-only, MIT license).

#### 4.4.2 `models/` — In-Memory Language Model

| File | Class | Key Fields |
|---|---|---|
| `language_config.hpp` | `LanguageConfig` | `name`, `version`, `fileExtensions`, `lexical`, `syntactic`, `semantic` — root container. |
| `token_definition.hpp` | `TokenDefinition` | `kind` (keyword, operator, literal, etc.), `pattern` (regex), `value` (exact string). |
| `grammar_rule.hpp` | `GrammarRule` | `name`, `production` (string), `alternatives` (parsed list of symbol sequences). |
| `type_system_config.hpp` | `TypeSystemConfig` | `primitiveTypes`, `typeInference` flag, `implicitConversions` list. |
| `operator_definition.hpp` | `OperatorDefinition` | `symbol`, `precedence`, `associativity`, `type` (binary/unary_prefix/unary_postfix). |
| `semantic_rule.hpp` | `SemanticRule` | `id`, `description`, `severity` (error/warning). |

#### 4.4.3 `validators/` — Config Validation

- **`ConfigValidator`**: Checks that the loaded config is internally consistent:
  - All grammar non-terminals are defined
  - Operator precedences don't conflict
  - Keywords don't collide with identifier patterns
  - Semantic rules reference valid types
  - All required sections are present

---

### 4.5 `tokenizer/` — Character Stream to Token Stream

> **AMENDED.** The tokenizer **emits every token, including whitespace, newlines, and comments** — it does **not** call `skipWhitespaceAndComments`. Trivia tokens carry `CoreTokenKind::Whitespace` / `LineComment` / `BlockComment`; the schema-aware resolver inside `TreeBuilder::pushToken` (see [01-tree-node-model-plan - ok.md §5.7](./01-tree-node-model-plan - ok.md)) applies `NodeFlags::EmptySpace` per the language config. This preserves source fidelity for formatters/IDE tooling. The `Tokenizer` constructor takes a `GrammarSchema` (sub-plan §5.12), not the obsolete `LanguageConfig`.

The tokenizer is the **first stage** of the pipeline. It reads raw source characters and emits a stream of `Token` objects based on the loaded language config's lexical definitions.

#### 4.5.1 `tokenizer.hpp/.cpp`

```cpp
class Tokenizer {
public:
    /// Construct tokenizer for a given language config.
    explicit Tokenizer(const LanguageConfig& config);

    /// Tokenize an entire source file into a token stream.
    TokenStream tokenize(const std::string& sourceFilePath);

    /// Tokenize from a string (for testing / REPL).
    TokenStream tokenizeString(const std::string& source, const std::string& fileName = "<string>");

private:
    const LanguageConfig& config_;

    Token nextToken(SourceReader& reader);
    Token readStringLiteral(SourceReader& reader);
    Token readNumberLiteral(SourceReader& reader);
    Token readIdentifierOrKeyword(SourceReader& reader);
    Token readOperatorOrDelimiter(SourceReader& reader);
    void skipWhitespaceAndComments(SourceReader& reader);
};
```

**How it works:**
1. Uses `SourceReader` to consume characters one at a time
2. Matches characters against the language config's patterns (longest match wins)
3. Differentiates keywords from identifiers by checking the keyword list
4. Handles string escapes, multi-line strings, nested comments per config
5. Tracks source location for every token

#### 4.5.2 `token_stream.hpp/.cpp`

```cpp
class TokenStream {
public:
    const Token& peek() const;           // Look at current token
    const Token& peekAhead(size_t n);    // Look ahead n tokens
    Token advance();                      // Consume and return current token
    bool match(TokenKind kind);           // Consume if matches, return true/false
    Token expect(TokenKind kind);         // Consume if matches, else error
    bool isAtEnd() const;
    SourceLocation currentLocation() const;

private:
    std::vector<Token> tokens_;
    size_t position_ = 0;
};
```

#### 4.5.3 `source_reader.hpp/.cpp`

Buffered character reader with:
- `peek()` / `advance()` / `isAtEnd()` interface
- Automatic line/column tracking
- UTF-8 aware character consumption

---

### 4.6 `analysis/` — Three-Phase Analysis

---

#### 4.6.1 `analysis/lexical/` — Phase 1: Lexical Analysis

While the tokenizer splits characters into tokens, the **lexical analyzer** validates and enriches them using language-specific rules.

```cpp
class Lexer {
public:
    explicit Lexer(const LanguageConfig& config, ErrorReporter& reporter);

    /// Validate and enrich the token stream. Returns validated stream.
    TokenStream analyze(TokenStream&& rawTokens);

private:
    LexicalRules rules_;
    ErrorReporter& reporter_;
};
```

**Responsibilities:**
- Validates that all tokens match allowed patterns (rejects malformed literals, unknown symbols)
- Classifies ambiguous tokens (e.g., `-` as unary vs binary based on context)
- Validates string escape sequences
- Validates numeric literal suffixes and ranges
- Reports lexical errors with precise source locations

**`LexicalRules`** — Engine that compiles the language config's lexical section into efficient matchers (regex or DFA-based).

---

#### 4.6.2 `analysis/syntactic/` — Phase 2: Syntactic Analysis (Parsing)

> **AMENDED.** The parser produces a **CST** (`Tree`) via the schema-aware `TreeBuilder` from [01-tree-node-model-plan - ok.md §5.7](./01-tree-node-model-plan - ok.md), not an AST hierarchy. The "recursive descent" framing below describes *how* the parser drives `TreeBuilder` (call `open(ruleId)` / `pushToken` / let `OpenScope` close on RAII). Validation is performed by the builder against `GrammarSchema`; errors become `Error`/`Missing` nodes + structured `ParseDiagnostic`s, never exceptions. The AST is recovered as a *view* over the CST via the AST cursor mode (skips `NodeFlags::EmptySpace`).

Transforms the validated token stream into an **Abstract Syntax Tree (AST)**.

```cpp
class Parser {
public:
    Parser(const LanguageConfig& config, ErrorReporter& reporter);

    /// Parse token stream into an AST.
    std::unique_ptr<ASTNode> parse(TokenStream& tokens);

private:
    Grammar grammar_;
    ASTBuilder builder_;
    ErrorReporter& reporter_;

    // Recursive descent methods (generated from grammar rules)
    std::unique_ptr<ASTNode> parseRule(const std::string& ruleName, TokenStream& tokens);
    std::unique_ptr<ASTNode> parseExpression(TokenStream& tokens, int minPrecedence);
};
```

**How it works:**
- The `Grammar` class loads the BNF rules from the language config and computes FIRST/FOLLOW sets
- The parser uses **recursive descent** for statement-level constructs
- Uses **Pratt parsing** (precedence climbing) for expressions, driven by operator definitions from the config
- Error recovery via synchronization tokens (e.g., skip to next `;` or `}`)

**`ASTBuilder`** — Factory for creating AST nodes. Attaches source locations and parent pointers.

**`Grammar`** — Runtime grammar representation:
- Parses BNF production strings into structured alternatives
- Computes FIRST and FOLLOW sets for conflict detection
- Validates grammar is unambiguous (or flags ambiguities for Pratt resolution)

---

#### 4.6.3 `analysis/semantic/` — Phase 3: Semantic Analysis

Validates the AST for **meaning** — type correctness, scope rules, and language constraints.

```cpp
class SemanticAnalyzer {
public:
    SemanticAnalyzer(const LanguageConfig& config, ErrorReporter& reporter);

    /// Perform all semantic passes on the AST. Annotates AST in-place.
    void analyze(ASTNode& root);

private:
    SymbolTable symbolTable_;
    TypeChecker typeChecker_;
    ScopeResolver scopeResolver_;
    ErrorReporter& reporter_;

    void resolveNames(ASTNode& node);    // Pass 1: Name resolution
    void checkTypes(ASTNode& node);      // Pass 2: Type checking
    void checkSemanticRules(ASTNode& node); // Pass 3: Custom rules from config
};
```

**Sub-components:**

| Component | Purpose |
|---|---|
| **`SymbolTable`** | Scoped hash map of symbols. Supports nested scopes (push/pop). Stores variable types, function signatures, class definitions. |
| **`TypeChecker`** | Infers expression types bottom-up. Validates assignments, function calls, returns. Applies implicit conversions from config. |
| **`ScopeResolver`** | Manages scope stack. Resolves identifiers to their declarations. Enforces scoping model (block/function/global) per config. Detects use-before-declare. |

**Semantic passes (in order):**
1. **Name Resolution** — Walk AST, register declarations in symbol table, resolve references
2. **Type Checking** — Infer and check types for all expressions and statements
3. **Custom Rule Checking** — Apply semantic rules from the config (unused variables, unreachable code, const enforcement)

---

### 4.7 `gen/` — Code Generation

---

#### 4.7.1 `gen/intermediate/` — IR Generation

> **AMENDED.** Input is the `Tree` (CST) plus the `NodeAttribute<T>` side-tables populated by semantic analysis (`NodeAttribute<TypeInfo>`, `NodeAttribute<SymbolId>`, …) — see [01-tree-node-model-plan - ok.md §5.10](./01-tree-node-model-plan - ok.md). The generator walks the tree via `TreeCursor` in AST mode and **must not** mutate the tree (immutable post-`finish()`). It bails before emitting IR if `tree.diagnostics().hasErrors()` and strict mode is on; otherwise it generates best-effort IR for the error-free regions.

Transforms the semantically-validated AST into a **target-independent Intermediate Representation**.

```cpp
class IRGenerator {
public:
    explicit IRGenerator(ErrorReporter& reporter);

    /// Generate IR from a semantically-annotated AST.
    IRProgram generate(const ASTNode& root);

private:
    std::vector<IRInstruction> instructions_;
    size_t tempCounter_ = 0;
    size_t labelCounter_ = 0;

    IRValue generateExpression(const ASTNode& expr);
    void generateStatement(const ASTNode& stmt);
    std::string newTemp();     // Generate unique temporary variable
    std::string newLabel();    // Generate unique label
};
```

**IR Design** — Three-address code with the following instructions:
- `BINARY_OP dest, left, op, right` — Arithmetic/logic operations
- `UNARY_OP dest, op, operand` — Unary operations
- `LOAD dest, source` — Load variable value
- `STORE dest, source` — Store to variable
- `CALL dest, function, args...` — Function call
- `PARAM value` — Push function parameter
- `LABEL name` — Label for jumps
- `JUMP label` — Unconditional jump
- `JUMP_IF cond, label` — Conditional jump
- `JUMP_IF_NOT cond, label` — Conditional jump (negated)
- `RETURN value?` — Function return
- `FUNC_BEGIN name` / `FUNC_END` — Function boundaries
- `ALLOC dest, type` — Allocate stack space

---

#### 4.7.2 `gen/optimizer/` — IR Optimizer

The optimizer is a **dedicated module** that sits between IR generation and target emission. It operates **on the IR** (not the AST, not the machine code) because:

1. **IR is simpler than AST** — uniform three-address instructions are easier to analyze than a tree of heterogeneous nodes
2. **Target-independent** — every optimization here benefits all 8 target backends simultaneously
3. **IR is designed for it** — three-address code / SSA representations were specifically created to enable efficient dataflow analysis and transformation
4. **Separated concern** — keeping optimization as its own module makes passes independently testable, orderable, and toggleable

##### Pipeline Architecture

The optimizer runs a configurable **pipeline of passes** over the IR. Each pass implements a common interface and can be enabled/disabled or reordered:

```cpp
/// Abstract base for all optimization passes.
class OptimizationPass {
public:
    virtual ~OptimizationPass() = default;
    virtual std::string name() const = 0;
    virtual bool run(IRProgram& program) = 0;  // Returns true if IR was modified
};

/// Runs the full optimization pipeline.
class Optimizer {
public:
    explicit Optimizer(ErrorReporter& reporter);

    /// Register a pass to the pipeline.
    void addPass(std::unique_ptr<OptimizationPass> pass);

    /// Run all registered passes. Repeats until no pass modifies the IR (fixed-point).
    void optimize(IRProgram& program);

    /// Convenience: build the default optimization pipeline.
    static Optimizer createDefault(ErrorReporter& reporter);

private:
    std::vector<std::unique_ptr<OptimizationPass>> passes_;
    ErrorReporter& reporter_;
    size_t maxIterations_ = 10;  // Safety bound for fixed-point iteration
};
```

##### Optimization Passes

| Pass | What it does | Example |
|---|---|---|
| **Constant Folding** | Evaluates expressions with known constant operands at compile time. | `t1 = 3 + 4` → `t1 = 7` |
| **Constant Propagation** | When a variable is assigned a constant, replaces subsequent uses with that constant. | `x = 5; y = x + 1` → `x = 5; y = 5 + 1` |
| **Dead Code Elimination** | Removes instructions whose results are never used, and unreachable code after unconditional jumps/returns. | Unused `t2 = a * b` removed if `t2` is never read |
| **Common Subexpression Elimination** | When the same expression is computed multiple times (with unchanged operands), reuses the first result. | `t1 = a + b; ... t3 = a + b` → `t1 = a + b; ... t3 = t1` |
| **Copy Propagation** | When `x = y` and `y` doesn't change, replaces uses of `x` with `y`, enabling further dead code elimination. | `t1 = t0; use(t1)` → `use(t0)` |
| **Strength Reduction** | Replaces expensive operations with cheaper equivalents. | `x * 2` → `x + x`; `x * 8` → `x << 3` |
| **Loop-Invariant Code Motion** | Moves computations that produce the same result on every iteration to before the loop. | `for(...) { t = a + b; ... }` → `t = a + b; for(...) { ... }` |

##### IR Analysis Utilities

The passes rely on shared analysis infrastructure:

| Analysis | Purpose |
|---|---|
| **CFG Builder** | Constructs a Control-Flow Graph from the linear IR instruction list. Basic blocks and edges between them. Required by most passes. |
| **Liveness Analysis** | Determines which variables are "live" (will be read later) at each point. Powers dead code elimination and future register allocation. |
| **Reaching Definitions** | For each point in the program, determines which definitions (assignments) could have produced each variable's current value. Powers constant propagation and CSE. |

##### Default Pass Ordering

The optimizer runs passes in this order (repeating until stable):

```
1. Constant Folding          ─┐
2. Constant Propagation       │  These feed each other:
3. Copy Propagation           │  folding creates constants,
4. Common Subexpr Elimination │  propagation enables more folding
5. Dead Code Elimination     ─┘
6. Strength Reduction
7. Loop-Invariant Code Motion
```

---

#### 4.7.3 `gen/link/` — Target-Specific Emission & Linking

Transforms the optimized IR into **target-specific machine code** and produces the final output (executable, library, or WASM module).

```cpp
class Linker {
public:
    Linker(const TargetInfo& target, ErrorReporter& reporter);

    /// Emit target code and link into final output.
    LinkResult link(const IRProgram& program, const std::string& outputPath);

private:
    std::unique_ptr<TargetBase> target_;
    ErrorReporter& reporter_;
};
```

**`TargetBase`** — Abstract base class for all target emitters:

```cpp
class TargetBase {
public:
    virtual ~TargetBase() = default;

    /// Emit target-specific code from IR.
    virtual std::vector<uint8_t> emit(const IRProgram& program) = 0;

    /// Get the file extension for this target's output.
    virtual std::string outputExtension() const = 0;

    /// Get the target's pointer size in bytes.
    virtual size_t pointerSize() const = 0;

    /// Get the target's endianness.
    virtual Endianness endianness() const = 0;
};
```

**Supported Targets:**

| Target Class | OS | Architecture | Output Format |
|---|---|---|---|
| `TargetWindowsX86_64` | Windows | x86_64 | PE/COFF (.exe) |
| `TargetLinuxX86_64` | Linux | x86_64 | ELF |
| `TargetLinuxARM64` | Linux | ARM64 | ELF |
| `TargetMacOSX86_64` | macOS | x86_64 | Mach-O |
| `TargetMacOSARM64` | macOS | ARM64 (Apple Silicon) | Mach-O |
| `TargetIOSARM64` | iOS | ARM64 | Mach-O |
| `TargetAndroidARM64` | Android | ARM64 | ELF (Android NDK) |
| `TargetWebWASM` | Web | WASM | WebAssembly (.wasm) |

**`TargetConfig`** — Maps `TargetInfo` (OS + arch) to the correct `TargetBase` subclass. Factory pattern.

---

## 5. Docker Configuration

### 5.1 `docker/Dockerfile`

Multi-stage image providing:
- **Build stage**: GCC/Clang, CMake, Ninja, nlohmann-json headers
- **Cross-compile stage**: Target toolchains (MinGW for Windows, Android NDK, Emscripten for WASM, osxcross for macOS)
- **Runtime stage**: Minimal image with just the compiler binary

### 5.2 `docker/toolchains/`

CMake toolchain files for each cross-compilation target. Each file sets:
- `CMAKE_SYSTEM_NAME` / `CMAKE_SYSTEM_PROCESSOR`
- Compiler paths (`CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`)
- Sysroot paths
- ABI flags

---

## 6. Build System

**CMake 4.0+** (current latest-stable on project inception, 4.3.2 at time of writing). Build system structure:

- Root `CMakeLists.txt` sets **C++23** standard, hidden-by-default visibility, `enable_testing()`, and pulls deps via `FetchContent` (no system packages required on any platform).
- Each `src/` subdirectory has its own `CMakeLists.txt` producing an object library; the shared `dss-code-prime-lib` is composed from object targets.
- Libraries link: `core` ← `source-factory` ← `tokenizer` ← `analysis` ← `gen` (intermediate + optimizer + link) ← `program`
- `main.cpp` links `program` (which transitively pulls everything) into the `dss-code-prime` executable
- `tests/CMakeLists.txt` uses **GoogleTest** (fetched via `FetchContent`); `ctest` runs the suite.

---

## 7. Third-Party Dependencies

Engine is **standard-library-first**. Most needs are met by C++23 stdlib:
- `std::expected<T, E>` — fallible results (replaces a Result<T> polyfill)
- `std::format` — string formatting (replaces fmt dependency)
- `std::filesystem` — cross-platform paths
- `std::span`, `std::optional`, `std::ranges` — non-owning views, nullable values, traversal

External dependencies are kept to two, both header-mostly and pulled via `FetchContent`:

| Library | Purpose | License |
|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing for language configs (used **only** by `grammar_schema_json.cpp`) | MIT |
| [GoogleTest](https://github.com/google/googletest) | Unit testing framework (only when `DSS_BUILD_TESTS=ON`) | BSD-3 |

Cross-platform compiler matrix: **MSVC 17.5+, GCC 13+, Clang 16+** (the LTS-grade releases that ship full C++23 support). Build verified via WSL or Docker on Linux/macOS in parallel with native Windows builds.

---

## 8. Implementation Phases (Todos)

> **v1 acceptance criteria per remaining phase.** Each `⏳`/`🔵`/`🟦` row below now carries an explicit "v1 must deliver" line so we don't drift on scope. The 127-gap catalog in [`07-production-readiness-plan - tbd.md`](./07-production-readiness-plan - tbd.md) is the canonical detail; this table is the per-phase summary.

| # | Status | ID | Title | Dependencies | Description |
|---|---|---|---|---|---|
| 1  | ✅ mostly done (Docker still pending) | `scaffold-project` | Scaffold project & build system | — | Create CMakeLists.txt files, directory structure, Docker setup. Current: CMake 4.0 floor, C++23, hidden visibility, shared lib + exe wired, integrated test target alive. **Pending:** Dockerfile + toolchains. |
| 2  | ✅ done (12/12 of sub-plan complete) | `core-types` | Implement core types | scaffold-project | **See [01-tree-node-model-plan - ok.md](./01-tree-node-model-plan - ok.md) — checkpoint snapshot at top.** **Done:** T0 build deps, T1 source primitives + strong IDs + interners (transparent heterogeneous lookup), T2 Tree storage + Node + NodeFlags, T3 ParseDiagnostic + DiagnosticReporter + DiagnosticPolicy (FNV-1a64 dedup with ruleContext), T4 GrammarSchema + SchemaCursor + ScopeKind + JSON loader + `toy.lang.json`, T5 schema-aware `TreeBuilder` with RAII `OpenScope`, cascade-cookie tracking, release-mode invariant guards, recovery + EOF synthesis, T6 `TreeCursor` with CST/AST modes, opaque Bookmark with TreeId guard, cycle-capped depth/parent walks, convenience forwarders, T7 `tree_visitor.hpp` header-only `walkPreOrder`/`walkPostOrder` with `WalkAction` skip/stop control and subtree-bounded traversal, verified zero allocations on the 10K-node walk via a global `operator new` counter, T8 `NodeAttribute<T>` header-only side-table with sparse↔dense auto-promotion at 50% coverage / 16-node floor, Tree-bound with nodeCount bounds checks (cross-tree guard is bounds-based, not full membership — documented caveat in §5.10), mutable + const accessors, forward iterator over both backings with internal-gap skipping, custom move ops that leave the source observably empty, T9 typed views (`tree_views.hpp` + `well_known_names.hpp` — **retired in 08.55** per thesis decision #4; engine reads structure config-driven via `semantics`/`hirLowering` blocks): seven header-only views (IdentifierView, LiteralView with cached Kind enum, BinaryExprView, BlockView, FunctionDeclView, VarDeclView, ExprStmtView), each with unchecked ctor + `::from()` factory returning `std::optional`, EmptySpace-skipping structural accessors via internal `nthVisibleChild` helper, trivially-copyable POD layout; new `Tree::hasSchema()` / `Tree::hasDiagnostics()` probes let token-level `from()` return nullopt cleanly on schema-less trees, T10 end-to-end integration test (`test_tree_end_to_end.cpp`) ties the full stack together: shipped toy.lang.json loaded from disk → SourceBuffer → TreeBuilder driven by a sequential TokenSeq helper → Tree → walkPreOrder AST traversal → indented `rule:`/`tok:` pretty-printer → string-equality assertion + diagnostic-code assertion, with 3 happy paths (varDecl, exprStmt, multi-statement), 1 T9-views-resolve test against the real parse, and 5 broken-path recovery flavors (unknown token with Error-leaf walk, unclosed scopes at EOF, truncated after keyword, explicit `pushError`, scope-stack underflow via `}`), T11 CMake wireup audit (zero orphan files; DSS_EXPORT properly applied), T12 onboarding docs (`docs/tree-model.md` + `docs/language-config-spec.md` with cookbook-pin test) (**278 test cases, 100% pass**). **Sub-plan complete.** Next-tier work: parser layer (parent plan phase #7) and schema-expressiveness-v2 (precedence, contextual keywords, etc.). The obsolete `core/error/` directory is dropped (subsumed by `DiagnosticReporter`). |
| 3  | ✅ subsumed into #2 (T4) | `source-config-schema` | Design language config schema | scaffold-project | The JSON schema is defined inside `grammar_schema_json.cpp`; `toy.lang.json` is the reference. Shipped languages (`csharp.lang.json` etc.) are authored later in `languages-onboarding-plan.md`. |
| 4  | ⏳ pending — depends on #2 | `source-factory` | Implement source factory | core-types | **Now a thin facade** that resolves a language name → config-file path and calls `GrammarSchema::loadFromFile`. Note: `GrammarSchema::loadShipped` already does both for the engine; this phase is the public API thin wrapper. `models/` and `validators/` directories are dropped. |
| 4.5 | ✅ done — sub-plan [`03-substrate-hardening-plan - ok.md`](./03-substrate-hardening-plan - ok.md) | `substrate-hardening` | SH1 landing-log generator + SH2 Linux CI matrix + SH3 cross-tree `NodeId` guard + SH4 v2 follow-ups | core-types, schema-v2 | De-risked the substrate before the tokenizer/parser/semantic phases land on top of it. SH1 stops landing-log drift (recurring bug class through v2). SH2 verified the cross-platform claim via the existing `DSS.DevOps@v2` multi-OS matrix (Linux/GCC, Linux/Clang+ASan, Windows/MSVC, macOS/AppleClang). SH3 closed the cross-tree `NodeId` caveat (`NodeId.treeTag` + tag validation in `NodeAttribute<T>` / `Tree::node_`). SH4 bundled three v2 follow-ups (CI hygiene wiring of SH1's tool; shape-based `switch`/`case`/`default` adoption in c-subset; multi-level AltChoice nesting stress test). 7 commits + 1 merge (PR #3); 531 cases / 26 suites, 100% pass. **Unblocks** phase #5. |
| 5  | ✅ done — sub-plan [`04-tokenizer-plan - ok.md`](./04-tokenizer-plan - ok.md) | `tokenizer` | Implement tokenizer | core-types, source-factory, substrate-hardening | TZ1 (bare tokenizer + toy E2E flip) + TZ2 (lexer modes + strings + comments) + TZ3 (c-subset + tsql-subset E2E flip + body-mode pins) + TZ3 review-fix round 1 (13 items addressed; new loader pass `C_BodyDefaultKindInShape`, `makeSyntheticMeaning` helper + synthesis-allowlist assertion, `E2EHarness` fail-loud + clean-diags dtor assertion, 7 new unit pins). Live `Tokenizer`-driven `TokenStream` drives every E2E test. TZ3 surfaced and closed three real semantic gaps in the builder/tokenizer: built-in-kind trust when per-lexeme table has no entry, body-mode cursor-advance skip, and multi-char operator wins over id-runs (T-SQL `N'`-style). Closes `v2-gap-catalog - tbd.md` row 3 end-to-end. 625 cases / 29 suites, 100% pass. |
| 6  | ✅ subsumed into #5 (see `04-tokenizer-plan - ok.md` §2.7) | `analysis-lexical` | (subsumed) | tokenizer | The v2 schema-aware `TreeBuilder` already does the validation phase 6 was envisioned for (`scopeRequire` filtering, `expectedSet`-driven contextual demotion, `P_SchemaCursorDesync`). The remaining lexical concerns (escape-sequence integrity, numeric ranges, comment / string body handling) live inside `src/tokenizer/` per `04-tokenizer-plan - ok.md`. |
| 7  | ✅ **CLOSED — all 7 PRs done** — sub-plan [`05-parser-plan - ok.md`](./05-parser-plan - ok.md) | `analysis-syntactic` | Implement syntactic analysis | analysis-lexical | ✅ `SchemaWalker` substrate extraction (PA0); ✅ schema-driven RD driver with try-each-branch speculation (PA1); ✅ Pratt walker for operator precedence (PA2, closes v2-gap-catalog row 1); ✅ panic-mode recovery + `followSetOf` + clang-style diagnostic rendering (PA3, closes G-115 end-to-end); ✅ real-world corpus stress + grouped-postfix + off-grammar body drain (PA4); ✅ LSP server skeleton with diagnostics over stdio JSON-RPC + UTF-16 position encoding + monotonic-generation stale-suppression (PA5a); ✅ LSP semantic-stub method handlers for hover/completion/definition/references/rename/signatureHelp (PA5b). PA-Walker-LeftRec added `wrapLastChildInFrame` substrate primitive; PA5a-prep closed v2-gap-catalog rows 12-declarator/16/29. Hand-driven `tests/core/test_*_subset.cpp` flipped to `Parser::parse()`; `tests/corpus/{toy,c-subset,tsql-subset}/` programs parse cleanly with golden trees; LSP server publishes diagnostics in real editor sessions (47 ctest suites green at parser-phase close; now 56/56 post-08.55). |
| 7.5 | 🟢 **CU1 + CU2 done; CU3–CU4 pending** — sub-plan [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md) | `compilation-unit-model` | Multi-file translation units + import resolution | analysis-syntactic | Bridges parser (per-file `Tree`) and semantic (cross-file symbol table). Six PRs (CU1–CU6; CU5 multi-language added rev 2; CU6 cross-CU references paired with [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) LK11 per its §2.12). **CU1 ✅** (bare type) + **CU2 ✅** (`addFile`/`addInMemory`, `D_*` codes, lexer+parser diagnostics unified in the Tree — cross-plan into 01/05); 48/48 suite. **G-110 ✅ resolved** (G-111 = CU4). **v1:** CU1–CU4. **v1.1:** CU5. **v1.x:** CU6 ↔ LK11; trigger = first artifact profile needing multiple CUs in one image. |
| 7.6 | ⏳ **new (rev 2)** — sub-plan [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) | `substrate-prep` | Generalize arena + ship type lattice | compilation-unit-model | SP1 generalize `Tree`'s arena + `NodeAttribute<T>` so HIR/MIR/LIR reuse the same substrate. SP2 ship core type lattice + per-language extension registry. Substrate-tier (5-agent review). **v1 must deliver:** zero behavior delta on existing Tree code; the lattice is in place before semantic phase opens. |
| 8  | ⏳ pending | `analysis-semantic` | Implement semantic analysis | substrate-prep | Symbol table + scope resolution + type checking + const-correctness + typedef resolution. **Consumes `CompilationUnit`** (from phase 7.5) — symbol table spans every Tree in the CU + cross-file refs are pre-resolved. Populates `NodeAttribute<TypeId>` / `NodeAttribute<SymbolId>` over each CST **against the lattice from 7.6**. New `S_*` diagnostic namespace. Production-readiness §2 (G-201..G-212). |
| 8.5 | ✅ **COMPLETE (rev 2 — HR1–HR11 done)** — sub-plan [`09-hir-plan`](./09-hir-plan%20-%20tbd.md) | `hir` | High-level IR | analysis-semantic | The pivot layer. Language-neutral, structured, typed HIR with FFI / shader / transpile attribute side-tables; CST→HIR lowering per shipped language. Eleven PRs (HR1–HR11). HR1 ✅ 2026-05-26 (`feature/hir-1` commit `406d5c7` — arena + node shapes + walker + ids + extension registry) + HR2 ✅ 2026-05-26 (typed expressions + operator registry + verifier) + HR3 ✅ 2026-05-26 (structured CF + break/continue & per-kind-arity verifier rules) + HR4 ✅ 2026-05-26 (declarations + extern surface + FfiMetadata side-table) + HR5 ✅ 2026-05-26 (attribute system + side-tables + `hir_attrs.hpp` catalog; verifier emits real diagnostic spans via optional `HirSourceMap`) + HR6 ✅ 2026-05-27 (full verifier — block dead-code / return completeness / Call-arg-vs-FnSig / intrinsic-registered / shader-restriction subverifier; `HirIntrinsicRegistry`; optional `TypeInterner` injection) + HR7 ✅ 2026-05-27 (round-trippable `.dsshir` text format — `emitHir`/`parseHir`, inline structural types, verify-on-load) + HR8 ✅ 2026-05-27 (config-driven CST→HIR lowering engine + `hirLowering` schema facet, proven on c-subset; per-expression type inference + literal-value pool; verify-on-load) + HR9 ✅ 2026-05-27..28 (`toy` enriched into a typed language + generic lowering test; arrays un-deferred end-to-end via a config-driven declarator-suffix descriptor + semantic-time constant-length eval; + gap-closure: char/string VALUES, SeqExpr, pointers, ternary) + HR10 ✅ 2026-05-27..28 (tsql-subset lowering — role-explicit SQL extension nodes via a generic `childGathering` config vocabulary + `ChildLower` enum, flat-expr lowering, coalesced/doubled-delimiter strings, `NULL`/relational-name extensions, `ReferenceRule.hardParents`; same language-agnostic engine); **HR11 ✅ done 2026-05-28. Plan 09 (HIR) COMPLETE.** **Single largest substrate addition in v1.** Blocks every downstream sink (transpile / MIR / SPIR-V / WASM). |
| 9  | 🟡 **in progress** — sub-plan [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md) | `mir-lir` | Mid-level + low-level IR | hir | MIR (SSA over CFG + structured-CF markers; replaces single-IR G-301) and LIR (per-target ISA). Eight PRs (ML1–ML8). **ML1 ✅ done 2026-05-27** (`feature/mir-lir` — MIR skeleton: fused value model (`MirValueId = MirInstId`), 3 arenas under one `MirModuleId`, phi nodes, closed `MirOpcode` + `opcodeInfo()` descriptor table carrying operand AND CFG successor arity (the single source of truth for terminator builders + ML3 verifier), `MirResultRule{None,Value,Optional}`, value-origin opcodes `Arg`/`Const`/`GlobalAddr`, `StructCfMarker` block field, build-once-freeze, create-then-fill `MirBuilder` (forward branches), `instBlock` reverse-lookup, finish-time freeze validation sweep, `MirLiteralPool`; 6-perspective review + homeless-items pass; 87/87 ctest). ML2–ML8 ⏳. |
| 9.5 | ⏳ **new (rev 2)** — sub-plan [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) | `ffi` | FFI binary readers + extern ingestion | hir | Hermetic ELF / PE / Mach-O / ar readers; C-header mode parser; ABI catalog; name mangling; HIR `ExternFunction`/`ExternGlobal` population. Six v1 PRs (FF1–FF6). **v1-blocking** — without it, no libc, no useful binary. |
| 10 | ⏳ pending | `gen-optimizer` | Implement IR optimizer (operates on MIR) | mir-lir | **v1 mandatory:** const folding + DCE + copy prop + dominator-tree + liveness. v1.1: CSE / LICM / inlining / strength reduction. Production-readiness §4. |
| 10.5 | ⏳ **new (rev 2)** — sub-plan [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) | `assembler` | In-tree machine-code encoder | mir-lir | x86_64 + ARM64 encoding tables; per-(arch×format) relocation taxonomy; round-trip disassembler oracle. Six PRs (AS1–AS6). |
| 11 | ⏳ pending — sub-plan [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) | `linker` | In-tree linker + object writers | assembler, ffi, **artifactProfile** | **Hermetic** (replaces system-linker integration). ELF / PE / Mach-O writers + linker engine + TLS lowering + dynamic imports. Ten PRs (LK1–LK10). WASM-skeleton (LK8) + SPIR-V-skeleton (LK9) for v1.x backends. **Largest single chunk of backend work.** |
| 11.5 | ⏳ **new (rev 2)** — sub-plan [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md) | `debug-info` | DWARF + PDB + CFI / SEH | linker, mir-lir | DWARF 5 writer (ELF + Mach-O), PDB writer (PE), Win64 SEH, Mach-O compact-unwind, source-position chain through HIR/MIR/LIR/bytes. Twelve PRs (DB1–DB12). |
| 11.6 | ⏳ **new (rev 2)** — sub-plan [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md) | `codesign-publish` | Hermetic signing + notarization | linker, debug-info | Apple Mach-O codesign + notarization stapling, iOS provisioning, Authenticode + RFC 3161, APK v3 (skeleton). In-tree crypto via vendored BearSSL. Nine PRs (CS1–CS9). Apple-host-free. |
| 12 | 🟦 skeleton only | `program-api` | Implement program API & driver | core-types, linker, **artifactProfile** | **v1 must deliver:** `dss-code-prime build my-project.dss-project.json` CLI entry point; `.dss-project.json` loader; driver enforces `project.artifactProfile ∈ language.artifactProfiles` (`D_*` namespace); `CompilationContext` carries resolved profile through every phase; diagnostic-rendering layer produces clang-quality output; deterministic byte-identical output. Production-readiness §6 (G-601..G-609). |
| 12.5 | 🔵 sub-plan [`06-artifact-profile-plan`](./06-artifact-profile-plan%20-%20tbd.md) | `artifact-profile` | `artifactProfile` mechanism end-to-end (registered-set vocab — rev 2) | core-types | Language config declares supported profiles (now registered-set, not compile-time enum: `cli`/`gui`/`lib`/`staticlib`/`script`/`sproc`/`transpile`/`shader`/`hdl`); project config picks one; driver enforces; codegen reads. Four PRs (AP1–AP4). Gates phase #11 acceptance. |
| 13 | ⏳ pending — sub-plan [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md) | `wasm-backend` | WASM backend | hir, mir-lir, linker | HIR/MIR → WASM bytecode using structured-CF markers (no Relooper). WASM as "LIR-equivalent." Ten PRs (WA1–WA10). v1.x. Browser + wasmtime as runtime targets via FFI. |
| 13.5 | ⏳ **new (rev 2)** — sub-plan [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md) | `shader-gpu` | SPIR-V codegen + shader HIR + GPU bindings | hir, mir-lir | Same-source CPU+GPU functions via dual-lowering. Ten PRs (SG1–SG10). v1.x — lit up for the custom language. |
| 13.6 | ⏳ **new (rev 2)** — sub-plan [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md) | `transpile` | Source-to-source translation via HIR pivot | hir | Language-pair `.map.json` + HIR→HIR walker + target-CST builder + pretty-printer. Six PRs (ST1–ST6). v1.x. Promotes the former §9 long-running note. |
| 13.7 | 🔒 **reserved (rev 2)** — sub-plan [`19-hir-hw-reserved-plan`](./19-hir-hw-reserved-plan%20-%20tbd.md) | `hir-hw` | Hardware IR (VHDL / Verilog / SystemVerilog) | hir, substrate-prep | Sibling-of-HIR IR for concurrent + signal-typed + clock-bound hardware. **No work until trigger.** Plan exists to prevent software HIR from foreclosing hardware semantics. |
| 13.8 | 🔒 **reserved (rev 2)** — sub-plan [`20-custom-language-reserved-plan`](./20-custom-language-reserved-plan%20-%20tbd.md) | `custom-language` | User's eventual custom language | all v1 + shader + WASM | The engine already supports it via the schema mechanism; this plan owns language vocabulary + standard library + runtime decisions (the latter via 21). **No new core work needed** — validates substrate. |
| 13.9 | 🔒 **reserved (rev 2)** — sub-plan [`21-runtime-reserved-plan`](./21-runtime-reserved-plan%20-%20tbd.md) | `runtime` | Language runtime (GC / exceptions / coroutines / threading) | hir, mir-lir | Triggered module-by-module: GC when a GC'd language onboards; unwinder when a throwing language onboards; coroutines when async-shaped language onboards. Distinct from OS-supplied runtime libs (those are FFI targets). |
| 14 | 🟦 in progress (core test suite live) | `testing` | Comprehensive test suite | all above | **Current:** 29 ctest suites / 625 individual cases over `core/types/` + tokenizer + the three shipped `.lang.json` configs — all green on Linux/GCC + Linux/Clang+ASan + Windows/MSVC + macOS/AppleClang. **v1 must add:** real-world `tests/corpus/` programs per language (parser-plan PA4 prereq); "compile then run" CI tests on every runner; ARM64 CI legs (G-704..G-706); fuzzers (G-710). Production-readiness §7.3 (G-730..G-734). |
| 15 | 🟦 partial (CI/CD pipelines done — Docker not) | `docker-setup` | Docker & CI setup | scaffold-project | **Done:** `cpp-app-pr.yml` / `cpp-app-pkg.yml` / `cpp-app-deploy.yml` reusable workflows in [DSS.DevOps](../../DSS.DevOps/.github/workflows/) + consumer workflows in `dss-code-prime/.github/workflows/`. **v1 must add:** ARM64 runners for Linux + Windows + macOS (G-704..G-706); fuzzing nightly (G-710); extended sanitizer coverage (G-709). Cross-compile toolchains: out of v1. |

---

## 9. Open Questions & Notes

> **For the full v1 open-question list see [`07-production-readiness-plan - tbd.md`](./07-production-readiness-plan - tbd.md) §1–§7.** The entries below are the long-running design notes; the open *decisions* needed before each remaining phase ships are tracked in each sub-plan's §4 / open-questions section.

### Resolved

- ~~**Grammar format**: BNF chosen for readability; PEG is an alternative if ambiguity becomes an issue.~~ **Resolved** by [01-tree-node-model-plan - ok.md §5.12](./01-tree-node-model-plan - ok.md).
- ~~**Error recovery**: Parser will implement panic-mode recovery.~~ **Resolved** by [01-tree-node-model-plan - ok.md §5.15](./01-tree-node-model-plan - ok.md) + parser-plan PA3.
- ~~**Cross-platform scope.**~~ **Resolved 2026-05-21:** v1 = **{Windows, Linux, macOS} × {x86_64, ARM64}**. v1.x adds WASM + iOS + Android.
- ~~**v1 language scope.**~~ **Resolved 2026-05-21:** v1 = `toy` + `c-subset` + `tsql-subset`. Full C99 / C++ / C# / SQLite onboarding post-v1.
- ~~**Output target distinction.**~~ **Resolved 2026-05-21 + amended rev 2 2026-05-23:** `artifactProfile` mechanism with a **registered-set** vocabulary (no more compile-time enum). Adds `transpile`/`shader`/`hdl` to v1's `cli`/`gui`/`lib`/`staticlib`/`script`/`sproc`. See [`06-artifact-profile-plan`](./06-artifact-profile-plan%20-%20tbd.md) §3 rewrite.
- ~~**G-301 IR design** (was: "default SSA over CFG").~~ **Resolved rev 2 2026-05-23:** **three IR layers** — HIR ([`09-hir-plan`](./09-hir-plan%20-%20tbd.md)) → MIR (SSA over CFG with structured-CF markers; [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20tbd.md)) → LIR (per-target ISA; same plan). Single IR cannot serve both transpilation and binary codegen cleanly.
- ~~**Linker strategy** (was: "system linker for v1, built-in post-v1").~~ **Resolved rev 2 2026-05-23:** **in-tree linker from day one** ([`14-linker-plan`](./14-linker-plan%20-%20tbd.md)). The hermetic-compiler invariant rules out shelling to `ld`/`link.exe`/`ld64`/`lld`/`wasm-ld`. Side-effect: Apple-host-free local dev.
- ~~**Type system** (was: "TypeKind enum + TypeId interner").~~ **Resolved rev 2 2026-05-23:** **core lattice + per-language extensions** ([`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20tbd.md) §2.2). Each `.lang.json` registers extension type-kinds via `typeExtensions[]`.
- ~~**Source-to-target language translation.**~~ **Resolved rev 2 2026-05-23:** promoted out of "long-running" into [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md). HIR is the pivot; language-pair `.map.json` declares HIR-to-HIR mappings; target-CST construction reuses each target language's schema; pretty-printer applies the target's lexical conventions.
- ~~**Apple host requirement.**~~ **Resolved rev 2 2026-05-23:** **never required for local dev.** In-tree Mach-O writer + in-tree codesign + in-tree notarization HTTP client (with the Apple cert as a credential, not a tool). See [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) LK3 + [`16-codesign-publish-plan`](./16-codesign-publish-plan%20-%20tbd.md).
- ~~**FFI / precompiled library reading.**~~ **Resolved rev 2 2026-05-23:** dedicated sub-plan [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md). In-tree binary readers + C-header mode parser + ABI catalog + name mangling. v1-blocking (without it, no libc → no useful binary).

### Still open — decide before the phase ships

- **G-201: Symbol table design.** Stack-of-hashmaps + `SymbolId` interner CU-scoped is the default. Decide before phase #8 PR1.
- **G-203: Type lattice extension surface.** Each `.lang.json` declares its `typeExtensions[]` (per `08.5-substrate-prep-plan` SP2). The lattice shape is decided; the per-shipped-language extension lists are not. Decide alongside phase #8.
- **G-114: Recovery quality benchmark.** Clang-level? tsc-level? Pin with stakeholders.
- **G-762: Milestone calendar.** Substantially expanded after rev 2. Substrate-prep + HIR + MIR/LIR + assembler + linker + debug-info + codesign + FFI all v1-blocking. Default (negotiable): parser closed Q2 ✅; CU + substrate-prep Q3; semantic Q3-Q4; HIR + FFI Q4; MIR/LIR + assembler + linker Q1 2027; debug-info + codesign + first end-to-end signed binary Q2 2027.
- **G-760: PR review tiering.** 5-agent review for substrate; 2-agent (code + tests) for feature; skip for docs.

### Long-running design notes

- **Standard library**: Owned by [`21-runtime-reserved-plan`](./21-runtime-reserved-plan%20-%20tbd.md) when triggered.
- **LLVM backend**: **Permanently out of scope** post rev 2. The hermetic-compiler invariant rules out LLVM as a runtime dependency. We own the entire pipeline.
- **Incremental compilation**: Not in v1 scope. Full recompilation per invocation. The arena + immutability design *permits* it later.
- **Register allocation**: Linear-scan for v1 (`12-mir-lir-plan` ML6); graph-coloring reserved post-v1.x.
- **Bootstrap**: The compiler is C++23 today. When the user's custom language (`20-custom-language-reserved-plan`) matures, a self-hosted rewrite is on the table — not committed.
