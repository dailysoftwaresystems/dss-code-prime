# DSS Code Prime — Universal Compiler Implementation Plan

> **Universal-compiler thesis (rev 2 — 2026-05-23).** Three architectural decisions now anchor every downstream plan:
>
> 1. **Hermetic.** We own every byte from source to signed binary. No `ld` / `link.exe` / `ld64` / `lld` / `wasm-ld` / `xcrun` / `clang` / `gcc` / `MSVC` / `dxc` / `glslc` / `dsymutil` / `mspdb` / `codesign` / `signtool` invocations. OS-supplied runtime libs (libc.so / libSystem.dylib / msvcrt.dll / ntdll.dll), browser/wasmtime, GPU drivers, and Apple developer certs are FFI targets and credentials — never tools.
> 2. **Three IR layers: HIR → MIR → LIR.** [HIR](./09-hir-plan%20-%20ok.md) (language-neutral, structured, typed — the pivot for transpilation, native lowering, and shader/WASM lowering) → [MIR](./12-mir-lir-plan%20-%20ok.md) (SSA over CFG with structured-CF markers preserved for WASM) → [LIR](./12-mir-lir-plan%20-%20ok.md) (per-target ISA). A single SSA IR cannot serve both binary codegen *and* source-to-source transpilation cleanly; HIR is the layer where the seven goals converge.
> 3. **Core type lattice + per-language extensions.** Universal core (primitives, ptr/ref/fnptr, struct/union/tuple, array/slice, vector/matrix, function signatures with calling-convention attribute) lives in [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) §2.2; each `.lang.json` registers extension type-kinds (C++ MemberPtr, C# Boxed/Delegate/GcRef, TSQL Varchar<N>/RowType, VHDL Std_Logic, shader Sampler/Texture). Transpilation maps via the core; extensions need explicit map entries.
> 4. **Config-driven all the way down — the engine has NO per-language / per-target / per-format C++.** The thesis "the source language is configurable" applies to *every* phase, not just lex/parse. Semantic analysis ([`08.6-semantic-plan`](./08.6-semantic-plan%20-%20tbd.md)) reads a `semantics` block from `.lang.json` (schema v4) via one language-agnostic engine; CST→HIR lowering ([`09-hir-plan`](./09-hir-plan%20-%20ok.md)) is likewise schema-described. ML5 cycle 2a (2026-05-29) extended the same discipline to backend targets: a compile target is a `.target.json` file, ARM64 is "drop a JSON file." **No phase may branch on `schema.name()` / `target.name()` / `format.name()` or hardcode a language/target/format's rules in C++.**
>
>    **What "config-driven" admits — the three-bucket rule (clarification added 2026-05-29 to close back-half drift):**
>
>    | Bucket | What it is | Where it lives | Config-driven? |
>    |---|---|---|---|
>    | **1. Declarative layout** | Fixed-width fields, opcode tables, section/symbol tables, reloc formulas, encoding rows (e.g. ModR/M slots, ARM bit-field templates), DWARF tag tables. | JSON | ✅ schema = bytes |
>    | **2. Universal algorithm over declared vocabulary** | Isel (patterns + costs), instruction encoding (prefix/ModR/M assembly), procedural streams (Mach-O bind/lazy/rebase opcode trie, DWARF line program, CFI), regalloc, GOT/PLT synthesis. | One engine algorithm; JSON declares the vocabulary it walks | ✅ — this is "the hard part in the source code" |
>    | **3. Identity-branching C++** | `if (schema.name() == "c-subset")`, `if (arch == "arm64")`, per-arch / per-format / per-language `.cpp` directories. | Nowhere — forbidden | ❌ Decision #4 violation |
>
>    **The operative test, stated once:** config-driven is defined by **the absence of an identity branch, not the absence of an algorithm**. A 2,000-line universal bind-trie emitter that branches on nothing is fully thesis-compliant. A 10-line `if (format == "macho")` is not. The hard algorithms living in the engine ARE the design — not a compromise, not an escape hatch.
>
>    **Bucket 2 is not "one giant universal function." It is shape-keyed dispatch over a closed vocabulary, with one specialized walker per shape.** In-tree precedent: `ChildLower` in `src/core/types/hir_lowering_config.hpp` declares a closed set of lowering verbs (`Expr` / `FlatExpr` / `Ext` / `Ref` / `VarDecl`); the CST→HIR engine has one specialized handler per verb; the JSON config declares which verb to use per child slot; the engine dispatches on the verb (a closed-vocabulary enum), never on language identity. The handlers are bucket-2 walkers — each is shape-keyed (e.g. "handle a flat-expr sequence"), not identity-keyed ("handle c-subset"). Adding a new language uses the existing verbs; adding a new verb is one new bucket-2 walker + one new config-string. Same pattern applies to the back half: assembler-encoders dispatch on `encoding.format` (x86-variable / fixed32 / wasm-leb / spirv-word); linker stream emitters dispatch on `format-section.shape` (linear-bytes / opcode-trie / chained-fixups); debug-info section emitters dispatch on `dwarf.section.shape` (string-table / opcode-stream / state-machine). The closed-vocabulary enum is bucket-1 declaration; each handler is bucket-2 algorithm; no walker branches on target/format/arch identity.
>
>    **Arch-shape / format-shape differences live in the JSON vocabulary, not in the engine.** x86 ModR/M, ARM bit-field encoding, RISC-V immediate splits, Mach-O bind opcodes vs ELF rela formulas — all different *vocabulary instances*. Bucket 1 must be general enough to express each one uniformly; the bucket-2 algorithm walks the declared rows and never branches on which target/format produced them.
>
>    **Existence proof:** the ML5 cycle 2a pivot. Pre-pivot, the LIR substrate had per-arch C++ (`Lir<TargetTraits>`, `targets/x86_64.hpp`). Post-pivot, the LIR substrate is target-blind: `runtime TargetSchemaId + JSON config + universal engine`. The MIR→LIR isel is a ~2,000-line bucket-2 algorithm parameterized by the JSON vocabulary; adding ARM64 became "drop a `.target.json`," zero engine work. This is the template for the back half — assembler ([`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md)), linker ([`14-linker-plan`](./14-linker-plan%20-%20tbd.md)), debug-info ([`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md)), shader ([`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md)), WASM ([`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md)) all follow the same bucket-1+2 shape.
>
>    **The one carve-out:** any genuinely unavoidable identity-specific code is a named, delimited real-blocker (per the standing "no deferring without a real blocker" rule) — never silent, never permanent. The expectation is that no such carve-out is needed; if one becomes necessary, it's a thesis falsification event worth its own review pass.
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
> - **[`08.5-substrate-prep-plan - ok.md`](./08.5-substrate-prep-plan%20-%20ok.md)** — **new (rev 2).** Three-PR refactor between CU plan and semantic phase: SP1 generalize the arena + attribute substrate so HIR/MIR/LIR can reuse `Tree`'s engineering primitives; SP2 ship the core type lattice + per-language extension registry; SP3 type-aware designator resolution (`compositeScopeFor(TypeId)` + Pass-2 type-position stamping + InitSlot tree's `nested` substrate). Substrate-tier review. **✅ done (SP1 + SP2 + SP3)** — `src/core/substrate/` + `src/core/types/type_lattice/`; `typeExtensions[]` schema v3.
> - **[`08.55-pre-se1-cleanup-plan - ok.md`](./08.55-pre-se1-cleanup-plan%20-%20ok.md)** — **new.** A focused cleanup PR between 08.5 and 08.6, forced by a 5-agent audit (2026-05-25): retires the remaining engine-side source-language hardcoding under the user's standing "NO CAVEATS, NO WORKAROUNDS" rule (decision #4). Schema-drives the Pratt wrapper-rule names (`expr.wrapperRules`), schema-drives numeric lexing (`numberStyle` block), prunes pre-interned token kinds to universal categories, retires `well_known_names`/`tree_views`/`symbol_population`, makes the LSP shipped-language list a directory scan with loud-fail `ShippedDirNotFound`/`ShippedDirEmpty` error kinds. **✅ shipped (commit `1a433c2`).** Unblocked SE1.
> - **[`08.6-semantic-plan - ok.md`](./08.6-semantic-plan%20-%20ok.md)** — Opens phase #8 (`analysis-semantic`): symbol table + scope resolution + type checking. Load-bearing decision: semantics are **config-driven** (a `semantics` block in `.lang.json`, schema **v4**) read by one language-agnostic `analyze()` engine — **no per-language C++**, honoring the universal-compiler thesis (decision #4 below). Seven PRs (SE1 toy → SE2 c-subset → SE3 tsql → SE4 const → SE5 typedef → SE6 functions/calls/overloads → SE7 LSP). **✅ SE1–SE7 all shipped** (two 5-agent review rounds; 64/64 ctest; engine stays language-agnostic via the `semantics`/`kindByChild`/`callRules`/`builtinFunctions` config facets, proven generic by the Synth2/Synth3 schemas). Closes G-201..G-212.
> - **[`09-hir-plan - ok.md`](./09-hir-plan%20-%20ok.md)** — **new (rev 2).** The pivot layer — language-neutral, structured, typed HIR with shader/FFI/transpile attribute side-tables. CST→HIR lowering is **config-driven** (per thesis decision #4: the lowering is schema-described, NOT per-language C++) so a new language lowers to HIR via config, not engine edits. Eleven PRs (HR1–HR11). **✅ COMPLETE (HR1–HR11 all landed 2026-05-26..28 on `feature/hir-1`): HR1 ✅ (commit `406d5c7` on `feature/hir-1`, 2026-05-26 — arena + node shapes + walker + ids + extension registry, 69/69 ctest) + HR2 ✅ (2026-05-26 — typed expressions: operator open-core+registry, typed builder helpers, `HirVerifier` expression-typing rule, 72/72 ctest) + HR3 ✅ (2026-05-26 — structured CF: statement builders + typed read-accessor layer + break/continue & per-kind-arity verifier rules, 73/73 ctest) + HR4 ✅ (2026-05-26 — declarations + extern surface: 7 declaration builders/accessors, FfiMetadata side-table, checkDeclarationShape verifier rule, 74/74 ctest) + HR5 ✅ (2026-05-26 — attribute system + side-tables: 4 value-struct headers + `hir_attrs.hpp` catalog, verifier emits real diagnostic spans via optional `HirSourceMap` (closes the HR2 span-stash IOU), dedup key folds in `actual`, 75/75 ctest) + HR6 ✅ (2026-05-27 — full verifier: block dead-code + return completeness + Call-arg-vs-FnSig + intrinsic-registered + shader-restriction subverifier, new `HirIntrinsicRegistry`, optional `TypeInterner` injection, `H_UnknownIntrinsic`/`H_ShaderViolation`, 76/76 ctest) + HR7 ✅ (2026-05-27 — round-trippable `.dsshir` text format: `emitHir`/`parseHir` + non-owning `HirTextContext` + heap-stable `HirParseResult`, inline structural types, positional `%N` symbol handles, all 5 side-tables inline, verify-on-load, in-memory + golden-corpus tests, `H_TextMalformed`/`H_TextVersionMismatch`/`H_TextUnknownName`, 77/77 ctest) + HR8 ✅ (2026-05-27 — config-driven CST→HIR lowering engine + `hirLowering` schema facet, proven end-to-end on c-subset (reordered ahead of toy); per-expression type inference + a `HirLiteralPool` of decoded literal values; verify-on-load; deferred constructs fail loud via `H_UnsupportedLoweringForKind`; closed an HR7 return-value-attr parse bug; 78/78 ctest) + HR9 ✅ (2026-05-27 — `toy` enriched in place into a real typed language + a generic lowering test proving the one engine handles a second grammar; **arrays un-deferred end-to-end** via a config-driven `DeclarationRule.arraySuffix` declarator-suffix descriptor + semantic-time constant-length eval (`S_NonConstantArrayLength`/`S_ArrayLengthOutOfRange`, fail-loud, no pointer decay); shared `decodeInteger` (`number_decode.hpp`); compound-assign/`++`/externs/genericity earlier; 5-agent review + fix pass caught a silent negative-length wrap; 80/80 ctest) **+ gap-closure pass 2026-05-28** completing the frontend→HIR layer: char/string literal VALUES (coalesce-body lexer — the former TZ4 blocker is resolved), HIR `SeqExpr` (value-yielding `++`/assignment-as-expression/complex-lvalue compound-assign), pointers, ternary `?:`, `#include` skip, `.dsshir` inline literal values (81/81) + HR10 ✅ (2026-05-27..28 — CST→HIR lowering for **tsql-subset** through the SAME language-agnostic engine: role-explicit SQL extension nodes via a generic `childGathering` config vocabulary + `ChildLower` enum, flat-expr lowering, coalesced/doubled-delimiter string VALUES → `Array<Char,N>`, `NULL`→`TSQL::Null`, relational names→`refExtensionKind` leaf (not core `Ref`), `ReferenceRule.hardParents` positional table-vs-column resolution, SQL calls reuse `callRules`; 5-perspective review + fix pass clean; 82/82 ctest). **HR11 ✅ done 2026-05-28 (multi-language CU lowering). Plan 09 (HIR) COMPLETE — HR1–HR11 all ✅.**
> - **[`10-source-translation-plan - tbd.md`](./10-source-translation-plan%20-%20tbd.md)** — **new (rev 2).** Promotes the formerly-long-running source-translation note (§9) into a real phase. Language-pair `.map.json` + HIR→HIR walker + target-CST builder + pretty-printer. Six PRs (ST1–ST6). v1.x. **⏳ planned.**
> - **[`11-ffi-plan - tbd.md`](./11-ffi-plan%20-%20tbd.md)** — **new (rev 2).** Hermetic FFI: in-tree ELF/PE/Mach-O/ar readers, C header mode parser, ABI catalog, name mangling, HIR extern-decl ingestion. Six v1 PRs (FF1–FF6). C++ mangling, full preprocessor reserved post-v1. **⏳ planned.**
> - **[`12-mir-lir-plan - ok.md`](./12-mir-lir-plan%20-%20ok.md)** — **new (rev 2).** MIR (SSA over CFG + structured-CF markers preserved) + LIR (per-target JSON-configured ISA, virtual+physical regs, calling-conv lowered, frame materialized). Eight PRs (ML1–ML8). **✅ done 2026-05-27..29 — ML1–ML8 closed end-to-end** including ML8 cycle 3 substrate cleanup (TargetTerminatorKind enum + schema-driven dispatch; `isTerminator` field deleted, derived from `terminatorKind`). ML7 cycle 2 ARM64-stackPointer + ABI goldens still anchored as legitimate next-cycle work.
> - **[`13-assembler-plan - tbd.md`](./13-assembler-plan%20-%20tbd.md)** — **new (rev 2).** In-tree machine-code encoder for x86_64 + ARM64. Hand-written encoding tables per Intel SDM / ARM ARM. Per-(arch×format) relocation taxonomy. Six PRs (AS1–AS6). **✅ done 2026-05-29 — AS1–AS6 closed end-to-end** (`feature/as-1`) including shape-keyed byte-encoder walkers (`x86-variable` + `fixed32`), round-trip oracle disassembler, relocation taxonomy unifier, SourceMapEntry stamping at dispatch level, end-to-end c-subset corpus pipeline.
> - **[`14-linker-plan - tbd.md`](./14-linker-plan%20-%20tbd.md)** — **new (rev 2; config-driven shift rev 3 2026-05-29).** In-tree linker — JSON-configured object formats (`src/dss-config/object-formats/*.format.json` for ELF / PE / Mach-O / WASM-skeleton / SPIR-V-skeleton) + ONE format-blind engine + symbol resolution + relocation application + per-platform metadata + file emission + driver pipeline wiring. Ten PRs (LK1–LK10). Mirrors the `GrammarSchema` (frontend) + `TargetSchema` (backend ISA) config-driven pattern — new object format = drop a JSON file, no engine edits. **Largest single chunk of backend work.** **✅ LK1–LK10 CLOSED end-to-end 2026-05-30** on `feature/lk-111`: LK4 substrate + LK1/LK2/LK3 writers (.o + executable images ET_EXEC/.exe/MH_EXECUTE) + LK6 cycle 1 reloc-apply + LK6 cycle 2a–d dynamic linking (PE IAT / ELF GOT+PLT / Mach-O LC_DYLD_INFO_ONLY / HIR→AS extern thread-through) + LK7 codesign placeholders + LK8/LK9 WASM/SPIR-V skeletons + LK10 file emission (`dss::linker::writeImage`) + driver pipeline wiring (`Program::compileFiles` / `compileDirectory`). LK5 (TLS) deferred to first TLS-bearing corpus; LK11 (cross-CU) deferred to v1.x.
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
| **Total ctest cases** | **166/166 ctest, 100% pass** post step-13.5 cycle 2 closure end-to-end (2026-06-03 — D-CSUBSET-WHILE-LOOP-SUBSTRATE + D-CSUBSET-MULTI-FN-WIN64-CC + D-LIR-SETCC-WIDTH-CONTRACT + D-ASM-ENCODE-FAILURE-FUNCTION-ROLLBACK + 14 runnable c-subset corpus examples for OPT1 differential verification). Growth trace: 56 suites at SE1-pre baseline → HIR (HR1–HR11) → MIR (ML1–ML4) → LIR (ML5–ML8) → AS1–AS6 (assembler) → LK4 substrate → LK1–LK3 + cycle 2 executable images + LK6 cycle 1–2d (dyn link) + LK7 (codesign) + LK8/LK9 (WASM/SPIR-V skeletons) + LK10 cycle 1 (file emission) + LK10 cycle 2 (driver pipeline + 24 new program tests) → 13.1–13.4 substrate (ML7-2.2/2.6 + void* typing + null-ptr-const + extern-library-syntax + hello_writefile + D-LANG-VARIADIC + hello_printf/printf_int) → 13.5 cycle 1+2 (while-loop substrate + 14 corpus examples). |
| Substrate hardening (sub-plan SH1–SH4) — landing-log generator + Linux CI matrix + cross-tree `NodeId` guard + v2 follow-ups | ✅ **done** — all four PRs shipped. |
| Tokenizer (sub-plan TZ1–TZ3 + review-fix round 1) — `SourceReader`, `Tokenizer`, `TokenStream`, `LexerModeStack`, `stringStyle` body handling, comment modes, `C_BodyDefaultKindInShape` loader guard, synthesis-allowlist assertion, fail-loud `E2EHarness`. **Post-TZ3: numeric lexing is now fully config-driven** (08.55 retired the hand-coded C-style `scanNumber()`; each language declares its `numberStyle` block, schema v4). | ✅ **done** — phase #5 closed; phase #6 (`analysis-lexical`) ✅ subsumed |
| **Parser** (sub-plan PA0–PA5b + PA5a-prep + PA-Walker-LeftRec) — substrate `SchemaWalker` ✅; iterative RD driver ✅; Pratt walker ✅; recovery + diagnostic UX ✅; corpus stress ✅; c-subset readiness shapes ✅; postfix-chain walker redesign ✅; LSP server + LSP semantic-stub handlers ✅ | ✅ **all 7 PRs done — parser phase closed.** PA0–PA3 as before; PA4 onboarded real corpus programs (c-subset / tsql-subset / toy) and closed v2-gap-catalog rows 6/7/8/12 (postfix call/index/inc/dec, prefix deref). PA4 follow-ups (PA4-F1..F8) landed substrate refactors (`OperatorTable::Entry → optional<GroupedPostfix>`, `bodyDefaultTokenKinds` dedup), additional test coverage, and corpus golden harness. PA5a-prep closed v2-gap-catalog rows 12-declarator, 16 (compound assignment), 29 (`extern` declaration). PA-Walker-LeftRec added the `TreeBuilder::wrapLastChildInFrame` substrate primitive + rewrote the Pratt walker's postfix branch. PA5a shipped the LSP server skeleton + diagnostics (stdio JSON-RPC, UTF-16 position encoding, document store with monotonic parseGeneration stale-suppression, two-mode schema resolution, thread-pool executor); PA5b added the 6 semantic-stub method handlers (`textDocument/{hover,completion,definition,references,rename,signatureHelp}`) returning LSP-spec defaults. A cross-cutting 5-agent review on the integrated LSP stack closed 22 follow-up items in a single commit. Semantic-powered LSP methods land post-phase #8 in a spun-off `09-lsp-plan`. **Post-PA5: the Pratt walker now reads its wrapper-rule RuleIds from `schema.exprWrapperRules(exprRule)` (08.55), not from any hardcoded constants — thesis decision #4 end-to-end.** **56 ctest suites green** (post 08.55). |
| **Compilation unit** (sub-plan CU1–CU6) — multi-file `CompilationUnit` + per-language `ImportResolver` | 🟢 **CU1 + CU2 + CU3 + CU4 done.** CU1 type/builder; CU2 multi-file `addFile`/`addInMemory` + `D_*` codes + lexer+parser diagnostics unified in the Tree (cross-plan amendments to 01/05); CU3 CU-scoped `SymbolId` + `UnitAttribute<T>` (the CU-scoped `NodeId`→`T` side-table phase #8 consumes) + membership-based cross-CU `NodeId` guard + sanctioned minimal `populateDeclarationSymbols` walk (§2.7 C3-X1); CU4 per-language `ImportResolver` populating `crossRefs` — c-subset `#include` following (grammar amended, §2.8 C4-X1) + tsql cross-statement table-name matching + toy identity + `D_UnresolvedImport`/`D_UnresolvedReference`. **G-110 ✅ + G-111 ✅ resolved.** CU5/CU6 v1.x. Phase #7.5 complete. See [`08-compilation-unit-plan - tbd.md`](./08-compilation-unit-plan - tbd.md). Bridges parser (per-file `Tree`) and semantic (cross-file symbol table). |
| **Semantic** — symbol table, scope resolution, type checking | ✅ **done (SE1–SE7)** (phase #8; sub-plan [`08.6-semantic-plan`](./08.6-semantic-plan%20-%20ok.md)). One language-agnostic `analyze()` engine driven by a `semantics` schema-v4 block produces a `SemanticModel` (symbol table + scope tree + node→SymbolId / node→TypeId side-tables + `S_*` diagnostics). Toy/c-subset/tsql onboarded purely by config; cross-file visibility is `crossRefs[]`-import-driven. SE4 const-correctness, SE5 typedefs, SE6 functions/calls/overloads (`kindByChild`/`callRules`/`builtinFunctions` facets), SE7 LSP wiring all shipped. **G-201..G-212 closed.** Two 5-agent review rounds; 64/64. |
| **IR** — HIR + MIR + LIR (rev 2; replaces single-IR plan) | ✅ done — **HIR HR1–HR11 ✅ (plan 09 COMPLETE, 2026-05-26..28, `feature/hir-1`)**. **MIR ✅ ML1–ML4 done 2026-05-27..28** (`feature/mir-lir`): ML1 skeleton, ML2 HIR→MIR (6 cycles incl. Cast emission), ML3 MirVerifier (7 rule families + `I_*` 0xA00x), ML4 `.dssir` text + round-trip. **LIR ✅ ML5–ML8 done 2026-05-29 + ML7 cycle 2 done 2026-05-30**: ML5 JSON-configured targets + register-file + calling conventions in JSON + MIR→LIR isel cycles 1–3e for arithmetic/cast/CFG/memory/wide-literal/float/bitwise/Calls/Aggregates with `LirVerifier`; ML6 liveness (`bc596ae`) + linear-scan + variant migration + R_* family + rewrite pass + `verifyLirPostRegalloc`; ML7 cycle 1 callconv + stack frame (`5362766`); ML7 cycle 2 ✅ (`b9b5304`) `arg` + `call` virtual-op materialization + GlobalAddr→SymbolRef peephole + move-cycle detector + 4 new L_* codes; ML8 cycle 1 `.dsslir` EMITTER (`040d496`) + cycle 2 PARSER + verify-on-load + round-trip + schema version (`9622b38`); **D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY ✅ 2026-06-02** (Win64 shadow-space + post-CALL alignment-bias for non-leaf functions via new `cc.callPushBytes` field + `FrameLayout.hasCalls` + `functionHasCalls` scan); **MIR→LIR extern-call opcode selection ✅ 2026-06-02** (new `MnemonicSlot::CallIndirectViaExtern` + `externSymbols` set — extern callees now lower as FF 15 indirect-via-IAT, internal as E8 direct). New diag codes: `L_*` 0xB00x family. **Remaining**: D-ML7-2.2 ✅ **CLOSED 2026-06-02** (stack-passed args); D-ML7-2.4 indirect function-pointer calls (anchored); D-ML7-2.5 regalloc pre-coloring for arg vregs (perf optimization); cross-CU type-import for HR Cast (downstream-of-HR real blocker). 166/166 ctest (latest). Production-readiness §3 (G-301a/b/c + G-302..G-310). Owned by [`09-hir-plan`](./09-hir-plan%20-%20ok.md) (HIR pivot; **HR1–HR11 ✅**) + [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) (MIR + LIR; **ML1–ML8 ✅**). |
| **Optimizer** — multi-tier, config-driven (source/target/linker-agnostic): HIR-transpile + MIR + LIR + per-target. v1 mandatory subset: const folding + DCE + copy propagation + dominator tree + liveness | ⏳ **READY-TO-OPEN 2026-06-03** — sub-plan [`22-optimizer-plan`](./22-optimizer-plan%20-%20tbd.md) (rev 1). **Gate condition ✅ MET 2026-06-02**: LK10 cycle 2 file-emission + D-LK10-ENTRY Stage 1 + D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY produced runnable on-disk executables (`int42.c`, `hello_puts`, `hello_writefile`, `hello_printf` on Windows x86_64). **Corpus condition ✅ MET 2026-06-03**: 14 runnable c-subset programs (sum_loop / max_of_3 / factorial / fibonacci / cmp_all_signed_conds / nested_loop / early_return / countdown_sum / multi_function / recursive_factorial / two_helpers / helper_before_main / loop_invariant / cse_candidate) provide differential-verification gates spanning arithmetic / conditional CF / loops / multi-fn / recursion / I/O+variadics / OPT1-transform-safety (LICM + CSE). Heuristics-as-data from PR1 → OPT10 autotuner. OPT3–OPT9 (inlining/CSE/LICM/scheduling/vectorization) roadmap; v1 = OPT1–OPT2. Production-readiness §4 (G-401..G-410). |
| **Codegen + in-tree linker + assembler** — ELF + PE + Mach-O × x86_64 + ARM64 + per-platform ABI + **hermetic linker** (rev 2; replaces system-linker integration) | ✅ **CLOSED + RUNNABLE end-to-end 2026-06-02** for x86_64 Windows. Owners: [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) + [`14-linker-plan`](./14-linker-plan%20-%20tbd.md). **Assembler ✅ COMPLETE (AS1–AS6 landed 2026-05-29 on `feature/as-1`)** — x86_64 + ARM64 byte encoding via shape-keyed walkers, round-trip oracle disassembler, relocation taxonomy unifier, source-map stamping. **Linker ✅ COMPLETE (LK1–LK10 landed 2026-05-30 on `feature/lk-111`) + RUNNABLE (D-LK10-ENTRY Stage 1 + D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY landed 2026-06-02)** — `ObjectFormatSchema` + format-blind engine + 11 `K_*` codes at 0x8xxx + 5 `D_*` driver-tier codes at 0xD007–0xD00B + cross-side reloc unifier; per-format walkers for ELF / PE / Mach-O / WASM (skeleton) / SPIR-V (skeleton); relocatable .o/.obj/.o AND executable images ET_EXEC/.exe/MH_EXECUTE; dynamic linking (PE IAT runtime-proven via 2-DLL hello_puts / ELF GOT+PLT / Mach-O LC_DYLD_INFO_ONLY); codesign placeholders (LK7); file emission (`dss::linker::writeImage`); driver pipeline wired (`Program::compileFiles` / `compileDirectory`). ✅ **First byte-correct c-subset e2e ACHIEVED 2026-06-02**: `int42.c` (Stage 1 exit-42) + `hello_puts/main.c` (Stage 2 Windows half — prints "hello\r\n" + exits 42 via msvcrt+kernel32 IAT). Remaining v1 deferrals: LK5 (TLS — first TLS-bearing corpus trigger); LK11 (cross-CU — v1.x); cross-host Stage 2 (Linux/macOS/ARM64) gated on D-LK10-ENTRY-ARM64 + D-LK10-ENTRY-MACHO-EXIT + CI runners; variadic `printf` gated on ML7 stack-args + variadic ABI substrate. Production-readiness §5 (G-501..G-571). |
| **Driver / project config** — `dss-code-prime build my-project.dss-project.json` | 🟢 **partial — programmatic API landed.** `Program::compileFiles` / `compileDirectory` wired through the full pipeline at LK10 cycle 2 (2026-05-30); `compileProject` fails loud `D_PlanNotLanded` pending plan 06 `.dsp` parser. CLI argument routing (`--compile` / `--target` / `--output`) is LK10 cycle 3. Production-readiness §6 (G-601..G-609) + [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md) AP2. |
| **`artifactProfile` mechanism** — language config declares supported profiles; project config picks one; codegen reads it | ⏳ pending. See [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan - tbd.md). Gates `gen-link` acceptance. |
| **Debug info** — DWARF (ELF + Mach-O) + PDB (PE) | ⏳ pending. Production-readiness §5.4 (G-530..G-533). |
| CI/CD pipelines | 🟦 Linux x86_64 + Windows x86_64 + macOS x86_64 ✅; **ARM64 across all three OS still pending** (G-704..G-706). Sanitizers partial. Fuzzing not yet wired. |
| **Real-world corpus** — `tests/corpus/{c-subset,tsql-subset,toy}/` programs that exercise the full grammar | ✅ parser-side done (PA4 — `mini_calc.c`, `schema_and_dml.sql`, `demo.toy` driven through `Parser::parse()` with golden-tree byte-comparison via `DSS_REFRESH_GOLDENS`). ✅ **End-to-end "runs a compiled binary" ACHIEVED 2026-06-02** (Windows host) — `examples/c-subset/{int42,hello_puts,...}/` compile → link → spawn → assert OS exit code + captured stdout byte-for-byte. **Remaining**: cross-host equivalents (Linux/macOS/ARM64) gated by D-LK10-ENTRY-ARM64 + Mach-O exit mechanism + CI runners on each (OS × arch). Production-readiness §7.3 (G-730..G-734). |
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
| ~~5~~ | [08.5](./08.5-substrate-prep-plan%20-%20ok.md) SP1 | Generalize arena + `NodeAttribute<T>` for HIR/MIR/LIR reuse (`ArenaContainer`/`ArenaBuilder`/`ArenaAttribute`; Tree + TreeBuilder rebuilt on it) | step 4 ✅ | ✅ **done** (54/54 suite green, full extraction, substrate-tier 5-agent review + fix pass) |
| ~~6~~ | [08.5](./08.5-substrate-prep-plan%20-%20ok.md) SP2 | Core type lattice + per-language extension registry (`TypeKind`/`TypeRecord`/`TypeInterner`/`TypeRegistry`/`TypeLattice`; `typeExtensions[]` schema v3) | step 5 ✅ | ✅ **done** (57/57 suite green, bound-but-separate, substrate-tier 5-agent review + fix pass) |
| ~~7a~~ | [08.55](./08.55-pre-se1-cleanup-plan%20-%20ok.md) | **Pre-SE1 genericity cleanup**: schema-drive Pratt wrappers, schema-drive numeric lexing, prune pre-interned kinds, retire toy-shaped headers + CU3 placeholder, LSP list config-driven. | step 6 ✅ | ✅ **done** (commit `1a433c2`) |
| ~~7~~ | [08.6](./08.6-semantic-plan%20-%20ok.md) SE1–SE7 | Symbol table + scope resolution + type checking — **config-driven** (`semantics` block, schema v4; one language-agnostic engine). Engine + toy/c-subset/tsql configs; per-tree-root scopes + `crossRefs[]` import injection; const-correctness; typedefs; functions/calls/overloads (`kindByChild` discriminator); LSP wiring (6 handlers). 64/64. | step 7a ✅ | ✅ **SE1–SE7 done** |
| 8 | [09](./09-hir-plan%20-%20ok.md) HR1–HR11 | HIR — language-neutral structured typed pivot; CST→HIR lowering per shipped language | step 7 | ✅ **plan 09 (HIR) COMPLETE — HR1–HR11 ✅** (2026-05-26..28, `feature/hir-1`; HR8 = config-driven lowering engine on c-subset; HR9 = toy enriched into a typed language + arrays un-deferred + gap-closure (char/string VALUES, SeqExpr, pointers, ternary); HR10 = tsql-subset lowering — role-explicit SQL extension nodes via generic `childGathering` config; HR11 = multi-language CU lowering, all language-agnostic). **Post-closure substrate fix** 2026-05-28 (`feature/mir-lir` commit `3e6b523`): `lowerTopLevel` was skipping the `coerce(initE, type)` call for module-global initializers; uncovered while writing plan-12.5's CE5 MIR e2e test; one-line fix to a missed call site of the same language-blind helper. **D5.3 (designated initializers) — READY TO RESUME** 2026-05-28: architecture + full C99 §6.7.8 scope frozen (2 cycles, recursive aggregate variant arm, all 4 context sites, dot-chained designators); both parser-substrate prerequisites ✅ done — sub-cycle A `6a8f146` (token-leaf branches in speculative alts) + sub-cycle B `14d789b` + `19c0b8b` (walker recognizes auto-interned Pratt wrappers, suppresses false-positive desync latch via O(1) `wrapDepth_ > 0` check; 3-agent review-fix pass). |
| 9 | [11](./11-ffi-plan%20-%20tbd.md) FF1–FF6 | FFI — hermetic ELF/PE/Mach-O/ar readers + C-header mode parser + ABI catalog + name mangling | step 8 | ✅ **MOSTLY DONE 2026-06-01..02**: FF1-ELF + FF2 + FF3 + FF4 + FF5 + FF6 (Windows hello_puts: msvcrt.puts + 2-DLL IAT + runtime print asserted byte-for-byte) all closed. **Remaining**: FF1-PE / FF1-MachO binary readers (anchored — triggered by first PE/macOS corpus needing extern resolution via shipped binaries rather than headers); cross-host FF6 equivalents (Linux/macOS/ARM64) gated on D-LK10-ENTRY-ARM64 + D-LK10-ENTRY-MACHO-EXIT; variadic `printf` gated on ML7 stack-args closure. |
| 10 | [12](./12-mir-lir-plan%20-%20ok.md) ML1–ML8 | MIR (SSA over CFG + structured-CF markers) + LIR (per-target ISA) | step 8 | ✅ **ML1–ML8 closed end-to-end 2026-05-27..29 + ML7 cycle 2 closed 2026-05-30 + frame-bias-unify 2026-06-02** (`feature/mir-lir` / `feature/lk-111`). **MIR ML1–ML4 ✅**: skeleton + HIR→MIR (6 cycles incl. Cast emission) + MirVerifier (7 rule families, `I_*` 0xA00x) + `.dssir` text round-trip. **D5 sub-arc ✅** (structs/unions/enums + designated init + brace-init + ConstructAggregate). **Plan 12.5 ✅** (CE1–CE5 + D6/D7). **LIR ML5–ML8 ✅**: ML5 JSON-configured targets + register-file + calling conventions in JSON + MIR→LIR isel through cycle 3e; ML6 liveness + linear-scan + rewrite pass + `verifyLirPostRegalloc`; ML7 cycle 1 callconv + stack frame (`5362766`); ML7 cycle 2 ✅ (`b9b5304`) `arg` + `call` virtual-op materialization + GlobalAddr→SymbolRef peephole + move-cycle detector + 4 new L_* codes; ML8 cycle 1 `.dsslir` EMITTER (`040d496`) + cycle 2 PARSER + verify-on-load + round-trip + schema version (`9622b38`); **D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY ✅ 2026-06-02** Win64 shadow-space + post-CALL alignment-bias for non-leaf functions; **MIR→LIR extern-call opcode selection ✅ 2026-06-02** new MnemonicSlot::CallIndirectViaExtern + externSymbols set on Lowerer. **Remaining**: D-ML7-2.2 stack-passed args (anchored — triggered by first 5+ arg call site, e.g. `printf` / `WriteFile`); D-ML7-2.4 indirect function-pointer calls (anchored); D-ML7-2.5 regalloc pre-coloring perf optimization. 149/149 ctest. ML6–ML8 ✅ parallel-with step 9. |
| 10.5 | [12.5](./12.5-const-eval-plan%20-%20ok.md) CE1–CE5 | **Shared constants-evaluation engine** in `src/hir/const_eval.{hpp,cpp}` (corrected location from initial draft — engine reads HIR so it cannot live in the lattice library below HIR). Lifts ML2's inline `tryConstFold` into one language-blind engine consumed by MIR-globals init, HIR verifier (array-length, future enum-bounds), D5.5 enum-value computation, and the optimizer's const-fold pass. Closes the float-arithmetic fold gap delimited by ML2 globals + the `Cast`-fold gap delimited by HR Cast emission. | step 10 | ✅ **CE1–CE5 done 2026-05-28** (engine + integer fold + Ref callback + target-aware Cast + commonType-tagged BinaryOp + short-circuit LogicalAnd/Or/Ternary + `allowFloat` IEEE 754 policy via host `<cmath>` with F32 narrowing, NaN-unordered comparisons, float→int unconditional refuse-on-out-of-int64; F16/F128 + lossy-conversion knob + schema-driven `allowFloat` + env/policy split + result cache delimited as plan-12.5 §0.2 deferred-by-design; 71 unit tests + 4 MIR end-to-end; 89/89). **Plan 12.5 CLOSED.** |
| 12 | [13](./13-assembler-plan%20-%20tbd.md) AS1–AS6 | x86_64 + ARM64 hand-encoder + per-(arch×format) relocation taxonomy | step 10 | ✅ **AS1–AS6 closed end-to-end 2026-05-29** (`feature/as-1`). x86_64 + ARM64 byte encoding via shape-keyed walkers (`x86-variable` + `fixed32`); round-trip oracle disassembler; relocation taxonomy unifier (target-side); SourceMapEntry stamping at dispatch level; end-to-end c-subset corpus pipeline test. `A_*` diag family at 0x1xxx. |
| 13 | [14](./14-linker-plan%20-%20tbd.md) LK1–LK10 | In-tree linker — JSON-configured object formats (`*.format.json` for ELF/PE/Mach-O/WASM/SPIR-V) + format-blind engine + symbol resolution + relocs + dynamic linking + file emission + driver wiring | steps 9, 12, **P1** | ✅ **LK1–LK10 CLOSED + RUNNABLE end-to-end 2026-06-02** (`feature/lk-111`). LK4 substrate + LK1/LK2/LK3 per-format writers + LK6 cycle 1 intra-module reloc-apply + LK6 cycle 2a–d dynamic linking + LK7 codesign placeholders + LK8/LK9 skeletons + LK10 file emission + LK10 cycle 2 driver pipeline wiring + **★ D-LK10-ENTRY Stage 1 ✅ + frame-bias-unify ✅** (first runnable DSS binary on Windows; hello_puts prints "hello\r\n" + exits 42 asserted byte-for-byte). LK5 (TLS) + LK11 (cross-CU) deferred. |
| **`★ STAGE 2 — STRICT SEQUENCE: c-subset host-runnable corpus expansion (2026-06-02+)`** | | | | |
| ~~13.1~~ | [12](./12-mir-lir-plan%20-%20ok.md) **D-ML7-2.2** stack-passed args | ✅ **CLOSED 2026-06-02 — co-closed with D-ML7-2.6 (Win64 slot-aligned).** FrameLayout extended with `outgoingArgAreaSize` + `outgoingSlotSize`. `computeMaxOutgoingStackArgs` pre-scans isCall insts. New `cc.slotAligned` field drives Win64 (ms_x64 = true) vs SysV/AAPCS64 (false) overflow semantics. Materialize call arm emits stack stores BEFORE register moves (hazard avoidance); arg arm reads stack-resident args via frame_load. Saved-reg prologue/epilogue offsets corrected (CRITICAL audit fold C1 — was missing `savedRegAreaOffset()` bias). `L_StackPassedArgUnsupported = 0xB007` stays as defensive guard. NEW anchors: D-ML7-2.10 (latent independent-counter payload mixed-class miscompile), D-ML7-2.6-MIXED-CLASS-FIXTURE (Win64 mixed int/float pin awaits c-subset float-param support). 149/149 ctest. | — | — |
| ~~13.2~~ | [01](./01-tree-node-model-plan%20-%20ok.md) c-subset `void*` typing | ✅ **CLOSED 2026-06-02 — D-LANG-POINTER-VOID-CONVERT + 8-agent audit fold.** `void*` opaque-pointer support landed via shared substrate: `SemanticConfig::PointerConversionRules { implicitToVoidPtr, implicitFromVoidPtr }` (directional split — C admits both, C++ self-host will admit only T*→void*). `isAssignable` extended with Ptr<Void> ↔ Ptr<T> arms gated on per-language rules (default-false = strict-reject). HIR `coerce()` emits matching `Cast(Ptr<T>↔Ptr<Void>)` nodes. c-subset.lang.json declares both directions true (C-standard §6.3.2.3). **THREE silent-failure surfaces closed in same commit** (audit-grade fixes, not workarounds): the void-pointer negative-pin test (`DistinctTypedPointersRemainMismatch`) failed because `checkCall`'s arg-assignability used `s.typeAt(argNodes[i])` which returns InvalidType for bare identifier-reference expression wrappers — silently suppressing the mismatch diagnostic. The 8-agent fold surfaced TWO parallel sites with the same wrapper-vs-leaf gap: `checkMemberAccess`'s `lhsType` lookup (suppressed S_NotAPointer / S_NotAComposite / field-type write-back for `p->x` on bare-identifier `p`); declaration-initializer's `initTy` lookup (suppressed BOTH type-inference AND mismatch diagnostic on `T* x = bareRef;` patterns). All three switched to `subtreeType()` (the existing DFS-descent helper). **JSON loader hardened** with typo-defense — unknown sub-keys under `pointerConversions` now fail-loud (`C_InvalidSemantics`); pre-fix a typo like `implictToVoidPtr` would silently fall back to default-false strict. **HIR coerce() comment corrected** (false-hazard claim about a "default-false SemanticConfig used by the lowerer below" — no such fallback path exists). **3 new anchors registered** (all audit-fold): D-LANG-VOIDPTR-ARITH-REJECT (default-reject; sizeof(void) undefined; GCC extension under config-flag); D-LANG-VOIDPTR-FN-CONVERT (default-reject; UB in standard C; universal compiler permit); D-HIR-VERIFIER-POINTER-CONVERT-CONTRACT (pin the verifier's post-coerce invariant — explicit Cast nodes required at HIR-tier); D-LANG-VOIDPTR-PREDICATE-GATE (type-design D2 — future per-element-type predicates); D-TYPERULES-PTRRULES-PASS-BY-VALUE (type-design D4 — marginal pass-by-value vs const-ref). **Test discipline**: `countCode` strict-pin form replaces any-bool sawTypeMismatch (audit-fold G1 criticality 8); added 3 return-direction + init-direction tests (G2/G3 coverage of the 2 other isAssignable call sites). 7 new tests total; 149/149 ctest. **Agnosticism re-verified by all 8 agents**: zero hardcoded source/target/format names in shared substrate (type_rules.hpp / semantic_analyzer.cpp / cst_to_hir.cpp / semantic_config.hpp); per-language values live only in c-subset.lang.json. **REJECTED across agents** (anchored): predicate-gate (type-design D2 anchor only — YAGNI for v1, additive when needed), pass-by-value (type-design D4 anchor only — marginal, address-on-3rd-rules-block). | step 13.1 ✅ | ✅ **CLOSED 2026-06-02** |
| ~~13.3a~~ | semantic substrate | ✅ **CLOSED 2026-06-02 — hello_writefile prereq substrate.** Three substrate cycles surfaced + closed when writing the hello_writefile example: (a) **D-LANG-NULL-POINTER-CONSTANT** — `nullPointerConstantFromIntegerZero: bool` field on `SemanticConfig::PointerConversionRules`; value-aware helper `isLiteralIntegerZero` admits the literal `0` to any `Ptr<*>` at the 3 isAssignable sites (call-arg, return, init); HIR `coerce()` materializes as `Cast(IntLit(0), Ptr<*>)` (MIR `IntToPtr` lowering routes to a pointer-width zero in the dest register). C-standard §6.3.2.3.3 covered for the common bare-`0` case; future const-eval-aware admission (`0+0`, `(int)0`) anchored as **D-LANG-NULL-PTR-CONST-EVAL**. (b) **D-CSUBSET-EXTERN-LIBRARY-SYNTAX** — trailing `stringLiteralExpr` inside `externFuncTail` (between `)` and `;`) carries a per-symbol import-library override. Example: `extern void* GetStdHandle(int n) "kernel32.dll";`. New fields: `HirExternRecord.libraryOverride` + `ExternDeclRef.libraryOverride`. `synthesizeFfiFromSourceDecls` prefers the per-extern override over the format-level default. Source-agnostic — any grammar producing a `stringLiteralExpr` child in extern tails gets per-symbol routing for free. (c) **D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS** partial closure — `subtreeType` now STOPS descent at any internal node whose visible children include an operator-token (any arity, via `OperatorTable.lookup`). This fixes a 2nd-order regression introduced by 13.2's audit fold: unary `&x` / `*p` (type-changing wrappers) would have returned the inner leaf's type instead of the wrapper's evaluated type — silently mis-admitting `int*` ↔ `int` at call-arg / init / return sites. Source-agnostic via schema's OperatorTable. Full closure (pass-2 expression typing for unary/binary/ternary wrappers) anchored. 6 new semantic-tier tests + 1 HIR Cast-emission test; 149/149 ctest. | step 13.2 ✅ | ✅ **CLOSED 2026-06-02** |
| ~~13.3b~~ | LIR/asm: local-int codegen | ✅ **CLOSED 2026-06-02 — D-CSUBSET-LOCAL-INT-CODEGEN substrate.** Three coupled pieces landed: (a) `FrameLayout` extended with `localAreaSize` + `numLocalAllocas` + `localAreaOffset()` accessor — locals sit ABOVE the spill area (positive RSP offset post-prologue, x86_64 disp8 encoding density + .pdata adjacency); (b) `materializeOneFunc` gained an `alloca` arm immediately after the `arg` arm rewriting each `alloca` LIR op → `lea result, [sp + localAreaOffset() + i * slotSize]` using the existing 3-op no-index `lea` form (scan-order slot assignment via shared `functionLocalAllocaCount` pre-scan + `localAllocaIndex` counter); (c) `neg` encoding added to `x86_64.target.json` (`REX.W F7 /3`, 3 bytes, modrmRegExt=3, requires2Address=true). `h.alloca_` + `h.lea` are optional `OpcodeHandles` — shader/WASM targets without body-local allocas legitimately omit both. 7-agent audit fold: **F1 HIGH** (3-agent convergence — alloca-arm docblock severed the call-arm comment block; moved to land after the `arg` arm where it structurally belongs); **F2 HIGH silent-failure** (`functionLocalAllocaCount` re-scans every function unconditionally; anchored D-CSUBSET-ALLOCA-COUNT-CACHE for MIR→LIR-time caching, trigger=profiler evidence); **F3 HIGH test-coverage** (added 2 substrate-tier tests: `AllocaMaterializesToLeaInScanOrder` pinning 2+ allocas → 2 leas with distinct monotonic offsets; `AllocaWithVirtualResultFailsLoud` pinning post-regalloc invariant + diagnostic-code specificity; third negative pin anchored D-CSUBSET-LOCAL-INT-CODEGEN-NEGATIVE-PIN, trigger=custom-schema-mutation test infrastructure). ANCHOR-LATER: D-FRAMELAYOUT-COUNT-FIELDS-AUDIT (A1, redundant byte+count fields), D-OPCODE-HANDLES-NAMING-SWEEP (A2, `alloca_` trailing-underscore), D-LIR-ALLOCA-LOWERING-EXTRACTION (A3, separate pass when dynamic stack growth lands). Agnosticism re-verified: zero `if (target == ...)` branches; mnemonic-based dispatch; ARM64 lands by JSON-only addition. 150/150 ctest (+ examples/c-subset/hello_writefile passing). | step 13.3a ✅ | ✅ **CLOSED 2026-06-02** |
| ~~13.3~~ | examples + plan 14 | ✅ **CLOSED 2026-06-02 — ★ THE FIRST DSS-EMITTED WINDOWS BINARY THAT PRINTS WITH ZERO MSVCRT DEPENDENCY.** `examples/c-subset/hello_writefile/main.c` calls `kernel32.GetStdHandle(-11)` → `kernel32.WriteFile(h, "hello\r\n", 7, &written, 0)` → returns 42. Execution-verified by `examples_runner` on Windows host: both `exitCode==42` AND `capturedStdout=="hello\r\n"` asserted byte-for-byte. Closes D-LK10-KERNEL32-WRITE-PATH. **Substrate exercised end-to-end**: D-ML7-2.2 stack-args (lpOverlapped on stack); D-LANG-POINTER-VOID-CONVERT (void* HANDLE/lpBuffer/lpOverlapped); D-LANG-NULL-POINTER-CONSTANT (literal `0` as null pointer constant for lpOverlapped); D-CSUBSET-EXTERN-LIBRARY-SYNTAX (`"kernel32.dll"` per-symbol library override); D-CSUBSET-LOCAL-INT-CODEGEN (`int written;` alloca→lea + `-11` neg encoding); D-LK10-ENTRY Stage 1 (trampoline ExitProcess injection). Compared to hello_puts (step 13.1's payoff, leaned on msvcrt DllMain self-init), this is more honest — the binary depends on NOTHING but the loader resolving kernel32's IAT entries. 150/150 ctest. | steps 13.1, 13.2 ✅, 13.3a ✅, 13.3b ✅ | ✅ **CLOSED 2026-06-02** |
| ~~13.4~~ | [12](./12-mir-lir-plan%20-%20ok.md) variadic ABI + c-subset `...` grammar | ✅ **CLOSED 2026-06-02 — D-LANG-VARIADIC substrate + first DSS-emitted printf binary.** Five-tier substrate landed end-to-end: (a) **Type system**: `TypeInterner::fnSig(...)` 4-arg overload adds `isVariadic` parameter; scalars[1]=1 encoding (preserving non-variadic scalars=[cc] default for every pre-13.4 call site, scalars=[cc, 1] for variadic); `fnIsVariadic()` accessor. (b) **Grammar**: c-subset `EllipsisOp` token + `paramOrEllipsis` alt-rule wrapping `param | ellipsisParam`; `paramList` repeats `Comma paramOrEllipsis`. (c) **Semantic**: `DeclarationRule::variadicMarker: optional<SchemaTokenId>` (source-agnostic — language declares its own marker; c-subset declares `EllipsisOp`); `paramsSubtreeHasVariadicMarker()` walks the params subtree for the marker token; semantic_analyzer's Function-decl path passes the result to the 4-arg `fnSig` builder. Call-arity check admits `argNodes.size() >= fixedCount` for variadic FnSigs (`tooFewForVariadic || wrongCountForFixed`). (d) **MIR/LIR Call payload**: new `core/types/call_payload.hpp` encodes `(isVariadic, fixedArgCount)` into the call inst's u32 payload (bit 31 = isVariadic; bits 0..30 = fixedArgCount; non-variadic always payload=0); HIR→MIR Call lowering stamps the payload from the callee's FnSig; MIR→LIR Call lowering propagates it; HIR Verifier admits args beyond fixed params for variadic FnSigs (assignability loop bounded at `params.size()` not `args.size()`). (e) **ABI emission**: `TargetCallingConvention::variadicVectorCountReg: optional<NamedRegisterRef>` (SysV declares `rax`/AL per §3.5.7; Win64/AAPCS64 leave empty — `D-TARGET-SUBREGISTER-ALIASES` anchors future `al` byte-alias support so the JSON can name the ABI-spec register exactly); ML7 materialize call arm emits `mov <countReg>, <fpr-count-in-vararg-region>` before the call instruction when `isVariadic(payload) && cc.variadicVectorCountReg.has_value()`. Count is FPR-class args in `[fixedArgCount, ops.size())` — c-subset has no float type today so count is always 0, but the loop is type-driven (future float varargs light up zero-substrate-change). **Examples**: `hello_printf` (1-arg call: `printf("hello\n")`, exit 42 + stdout "hello\r\n"); `printf_int` (2-arg variadic: `printf("answer=%d\n", 42)`, exit 0 + stdout "answer=42\r\n"). Both pin the full path semantic → HIR → MIR → LIR → assembled binary → OS run. **5-agent audit fold**: (1) **Simplifier HIGH** — `paramsSubtreeHasVariadicMarker` was a tree-walker duplicate of the existing `subtreeContainsToken` (SE4 const-marker scan); collapsed both consumers to the single walker (-22 LOC). (2) **Silent-failure HIGH-1** — `fnIsVariadic` was empty-scalar-tolerant; added `latticeFatal` on 0-slot FnSig (pins the cc-scalar invariant). (3) **Silent-failure HIGH-3** — HIR→MIR call-payload encoder silently truncated `params.size() > kFixedArgMask` (2^31); added fail-loud `unsupported()` at encode site. (4) **Silent-failure HIGH-5** — same site silently degraded a Ptr-with-empty-operands to non-variadic; fail-loud `unsupported()`. (5) **Silent-failure MEDIUM-1** — semantic checkCall's S_ArgCountMismatch now carries the "fixed " word for variadic-too-few (mirrors HIR verifier diagnostic shape). (6) **Comment-analyzer HIGH** — hello_printf docblock wrongly claimed `printf("%d\n", 42)` (actual is `printf("hello\n")`); fixed + cross-ref to printf_int. (7) **Comment-analyzer HIGH** — lir_callconv count-mov rationale (rax-aliasing-returnGprs[0]) was incorrect; rax isn't a SysV arg-passing GPR — corrected to "future indirect-call shape OR parallel-copy serializer could use it as scratch." (8) **Test-analyzer rating-9** — 3 new HIR verifier tests pin the variadic-arity 3-way matrix (admit args>params, reject args<fixed with "fixed " word, non-variadic still rejects args!=params). (9) **Silent-failure MEDIUM-2** — added Win64 negative test (`MsX64CcDoesNotDeclareVariadicVectorCountReg`) — a regression copy-pasting SysV's `variadicVectorCountReg` into ms_x64 surfaces fail-loud. **Anchored future cycles**: `D-ML7-VARIADIC-WIN64-DOUBLE-SPILL` (Win64 vararg-double-spill — every float vararg duplicates in matching GPR slot — surfaces when c-subset gets float types); `D-TARGET-SUBREGISTER-ALIASES` (register-table aliases for `al`/`ax`/`eax` so cc fields name ABI-spec subregisters exactly); `D-LANG-VA-LIST` (`va_list`/`va_arg`/`va_start`/`va_end` callee-side vararg consumption — not needed for caller-side printf, blocks user-defined variadic functions); `D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD` (sweep cycle — a typo in `"variadicMarker"` / `"variadicVectorCountReg"` JSON keys silently disables the feature with no diagnostic; project-wide sweep to add unknown-keys validation following the void* JSON loader precedent); `D-LANG-VARIADIC-MARKER-DESCENT-BOUNDARY` ✅ **CLOSED in same cycle (post-fold)** — `subtreeContainsToken` now takes an optional `declByRule` map and stops descent at any nested decl-rule node when supplied. Both consumers (SE4 const-marker + D-LANG-VARIADIC variadic-marker) pass it. Generalizes to any future "marker token within a decl subtree" scan; `D-ML7-VARIADIC-COUNT-MODE` (code-architect Q1) — `variadicVectorCountReg` + FPR-count scan generalizes to `variadicCountMode: enum {FPRCount, GPRCount, TotalArgCount}` sibling field when the first non-FPR-count ABI arrives (MIPS O32 total-vararg-arg-count etc.); `D-CALL-PAYLOAD-WIDENING-GATE` (code-architect Q2) — `call_payload.hpp` owns the u32 bit layout; every new call-shape bit (tail-call marker, musttail, indirect-call type tag) declared there; widen both `MirInst::payload` + `LirInst::payload` to u64 when a 32nd bit is needed (not pointer-to-struct — payloads stored inline in arenas); `D-ML7-CC-PRECALL-IMM-LOADS` (code-architect Q3) — when a 2nd pre-call register-load pattern lands (fixed syscall-number register etc.), generalize single `variadicVectorCountReg` field to `preCallImmediateLoads: [{reg, value}]` array on CC; `D-LANG-VARARG-PROMOTION-CHECK` (code-architect Q4) — per-language default-argument-promotion rules for vararg positions via optional `varargPromotionRules` block in SemanticConfig, checked in semantic_analyzer's checkCall AFTER fixed-param loop (HIR verifier's vararg-region skip stays correct permanently at that tier — this is a source-language check, not a type-tier check); `D-CALLPAYLOAD-STRONG-WRAPPER` (type-design Q1) — convert `call_payload` namespace to `struct CallPayload { uint32_t bits; }` with explicit conversions when a 2nd opcode reuses u32 payload differently (trivial mechanical when triggered). Grammar: mini_calc.c.tree refreshed (intentional shape change — non-variadic params now wrap in `paramOrEllipsis > param`). 152/152 ctest including 3 new HIR verifier tests + 2 new lir_callconv schema-tier tests + 2 new TypeInterner variadic tests. | step 13.1 ✅ | ✅ **CLOSED 2026-06-02** |
| ~~13.5 cycle 1+2~~ | examples + corpus expansion | ✅ **CLOSED 2026-06-03 — D-CSUBSET-WHILE-LOOP-SUBSTRATE + D-CSUBSET-MULTI-FN-WIN64-CC + D-LIR-SETCC-WIDTH-CONTRACT + D-ASM-ENCODE-FAILURE-FUNCTION-ROLLBACK + 14 runnable corpus examples + cross-platform CI fix.** First non-trivial host-runnable c-subset programs with INTRA-FUNCTION CONTROL FLOW. The sum-loop example (`while (i<=10) { sum += i; i++; }` → exit 55) surfaced 4 declared-but-unencoded x86_64 opcodes (jmp / cmp / jcc / setcc) AND the maxof3 example (sequential if/else) surfaced an ICmp+CondBr garbage-upper-bits bug. **Substrate landed** (single cycle, target-agnostic): (a) per-target `condCodeEncoding[10]` table mapping abstract TargetCondCode → ISA-numeric encoding (x86: 4-bit nibble in opcode-byte low; ARM64 will declare its own table for B.cc bits 0..3); (b) `EncodingSlotKind::CondCodeNibble` + `template.condCodeFromPayload: bool` driving cond-from-payload OR-into-opcode-byte at encode time; (c) `EncodingSlotKind::BlockRel32` + per-function block-offset table + per-function patch list in asm.cpp resolving intra-function PC-rel branches at assemble time (NOT linker relocs); (d) `Wire.prefixOpcodeBytes` admitting compound encodings (jcc emits `0F 8x rel32; E9 rel32` — 11 bytes — via wire[1].prefixOpcodeBytes=[0xE9] bridging the cond-branch's rel32 to the trailing uncond jmp's rel32); (e) `OperandKindFilter::BlockRef` + walker_util `BlockRelPatch` + `filterToLirKind` arm; (f) MIR→LIR `lowerCondBr` passes both successors as BlockRef operands AND fuses adjacent ICmp+CondBr into `cmp lhs, rhs; jcc-cond` (the naive `cmp result_b8, 0` would compare against setcc's garbage upper 56 bits and trip the branch the wrong way — the maxof3 bug). Validate's "single-writer slot" rule extended with BlockRel32 admitted-as-multi-writer (jcc has 2 BlockRel32 wires for taken+fallthrough). 4 new encoding rows in x86_64.target.json. Examples: `sum_loop/` (exit 55), `max_of_3/` (exit 13), `factorial/` (exit 120 — multiplicative loop), `fibonacci/` (exit 55 — 2-var rotation; phi cross-iteration stress), `cmp_all_signed_conds/` (exit 14 = bit-packed witness of 6 signed cond-codes — per test-analyzer rec). **7-agent post-fold** (cycle 2): (1) silent-failure CRITICAL #2 + HIGH #3 — `(void)encodeInst` laundered per-inst failures, partial bytes leaked into subsequent block-offset captures corrupting intra-function branch patches → closed via D-ASM-ENCODE-FAILURE-FUNCTION-ROLLBACK (track funcEncodeOk, truncate partial bytes per-inst, drop entire function on any failure + emit new `A_FunctionEncodeAborted` summary). (2) code-reviewer C1 — `lowerSwitch` passed empty operands to jcc which now requires 2 BlockRef operands → closed (every switch statement would have failed `A_NoMatchingEncodingVariant`; no corpus example exercised switch yet, so silent gap). (3) code-reviewer C2 — setcc-width contract bug: setcc writes ONLY the low byte; `cmp r64, 0` would read garbage upper 56 bits + setcc r/m8 without REX prefix references ah/ch/dh/bh aliases (not spl/bpl/sil/dil) for r4..r7 destinations. Closed via D-LIR-SETCC-WIDTH-CONTRACT: new `template.forceRexPrefix` flag (setcc declares it true for proper low-byte access on all 16 GPRs); new `zext` encoding (`REX.W 0F B6 /r` — movzx r64, r/m8); lowerICmp now emits `cmp → setcc b8 → zext result` (proper SSA, no multi-def). Closes the silent miscompile for any future MIR pattern that stores/returns a bool. (4) architect FOLD-NOW + type-design — rel32 formula was hardcoded in shared `assemble()` (agnosticism break per standing rules) → closed via `BlockRelPatchKind: enum { X86Rel32, Arm64Imm19, Arm64Imm26 }` on `BlockRelPatch`; resolver dispatches via kind; ARM64 arms fail-loud `A_NoEncodingShapeWalker` until D-AS3-BLOCK-REL-IMM19/26 closes them. (5) simplifier F1 — new `asm_byte_emit::writeU32LEAt` helper replaces hand-coded 4-byte LE store in the patch loop. New diagnostic code `A_FunctionEncodeAborted = 0x1006`. **Corpus shape note** (user emphasis 2026-06-03): each runnable example is a future OPT1 (13.6) differential-verification gate. Cycle-2 expansion (factorial / fibonacci / cmp_all_signed_conds) anchors distinct CFG shapes (multiplicative loop / 2-var phi rotation / sequential if/else stack). Anchored future cycles: D-OPT-JCC-FALLTHROUGH (elide trailing uncond jmp when ifFalse is next-laid-out block; saves 5 bytes per jcc); D-LIR-SETCC-DEAD-AFTER-FUSION (setcc's result is dead after ICmp+CondBr fusion — DCE removes wasted bytes); D-LIR-SETCC-WIDTH-CONTRACT (model byte/qword in LirReg so zero-extend isn't ad-hoc); D-AS3-COND-CODE-ARM64 (ARM64's cond-code table); D-AS3-BLOCK-REL-IMM19/26 (ARM64 branch displacement encoding); D-CSUBSET-LONG-BRANCH (function bodies > 2GB need rel32-overflow thunks). D-LIR-TRUNC-ENCODING (`trunc` mnemonic has no encoding; unblocks bool-stored / bool-returned MIR patterns); D-CSUBSET-UNSIGNED-CMP-CORPUS (unsigned cond-codes corpus once c-subset gains `unsigned`); D-LIR-FUSION-USE-COUNT-GATE (single-use guard on ICmp+CondBr fusion when a 2nd consumer of the setcc result arrives); D-SCHEMA-JSON-DUPLICATE-KEY-DETECT (silent-failure HIGH #1 — JSON object duplicate keys silently keep last value; project-wide sweep); D-CSUBSET-BOOL-ZEXT-CONTRACT (architect — document where bool zero-extension is guaranteed for the non-fused fallback arm); ~~D-CSUBSET-MULTI-FN-WIN64-CC~~ ✅ **CLOSED in cycle 2 post-fold 2026-06-03** — the original anchor mis-diagnosed (suspected wrong CC); actual bug was the trampoline injector's `resolveUserEntrySymbol` falling back to `functions[0]` when format.entryPoint was empty, silently picking the first-declared function (helper, not main). Closed via new `AssembledModule.userEntrySymbol: optional<SymbolId>` populated by compile_pipeline from grammar.semantics().declarations[].implicitReturnZeroForFunctionNames (source-agnostic — c-subset declares "main", future languages name their own). 7-agent fold (3 agents, strong convergence): (a) silent-failure HIGH #1 + code-reviewer #1 — silent first-match ambiguity → ALL matches now collected, fail-loud `K_SymbolUndefined` if >1 (never silently pick); (b) silent-failure HIGH #3 + code-architect Q5 — `functions[0]` fallback was the original bug class; now ONLY triggers for single-function modules (multi-function modules with no explicit userEntrySymbol nor format.entryPoint emit NotFound rather than silently invoke wrong fn); (c) code-architect Q3 — inline type-discriminator comments on `imageEntryOverride` (INDEX) and `userEntrySymbol` (SymbolId). New runnable example `examples/c-subset/multi_function/` (max-of-3 via helper, exit 7) pins the end-to-end fix. Newly anchored: D-CSUBSET-ENTRY-NAME-SPLIT (split `implicitReturnZeroForFunctionNames` from entry-fn-name list when a language gets implicit-return-0 on a non-entry function); D-CSUBSET-MULTI-FN-ENTRY-LK1-1-RETIRE (D-LK1-1 real-symbol-name preservation will let format.entryPoint match by name, retiring AssembledModule.userEntrySymbol); D-LK10-ENTRY-CASCADE-DEDUP (silent-failure MEDIUM #4 — encode-failure cascade dedup when assemble dropped the entry function); D-LK10-ENTRY-RESOLVE-INDEX (silent-failure LOW #5 — anchor O(1) entry-fn lookup when >1k functions land). **Cycle 2 corpus expansion added more runnables**: nested_loop/ (3*4=12; CFG nested back-edges + counter live-across-loops); early_return/ (cumulative sum exits at 21 via `return` inside loop body's `if`); countdown_sum/ (10+9+...+1=55 via `n != 0` cond — exercises Ne cond-code for loop-exit, distinct from sum_loop's `<=`); multi_function/ (max-of-3 via helper, exit 7); recursive_factorial/ (fact(5)=120 self-recursion); two_helpers/ (doubled→square chain, exit 100); helper_before_main/ (sub(50,8)=42 regression pin for D-CSUBSET-MULTI-FN-WIN64-CC); loop_invariant/ (D-OPT1-LICM-CORPUS — 5 iter × a*b=12 = 60; pins LICM-safe rewrites); cse_candidate/ (D-OPT1-CSE-CORPUS — (5+3)*2 + (5+3)*3 = 40; pins CSE-safe value-numbering). Total: **14 runnable corpus examples** exercising distinct CFG / arithmetic shapes — each is a future OPT1 (13.6) differential-verification gate. 166/166 ctest (latest). **Cross-platform CI fix 2026-06-03** (commit `a6bc3d2`): explicit `<algorithm>` + `<format>` includes + `std::ranges::any_of` → `std::any_of` in compile_pipeline.cpp (Linux gcc-13 + macOS Apple-clang both lacked transitive includes that MSVC provided). | step 13.4 | ✅ **CLOSED 2026-06-03** |
| 13.6 cycle 1 | [`22`](./22-optimizer-plan%20-%20tbd.md) OPT1 | ✅ **CLOSED 2026-06-03 — OPT1 cycle 1 substrate + corpus negative pins.** **Cycle 1 deliverable**: (a) **5 OPT1-correctness negative pins** per test-analyzer rec (dce_negative_pin → exit 100, const_fold_inside_expr → exit 42, copy_prop_across_join → exit 10, licm_conditional_mutation → exit 68, cse_noncommutative → exit 58). Each pins a SPECIFIC bug class a future OPT1/OPT2 pass could introduce, with exit-code distance making the bug bisectable. (b) `src/opt/optimizer.{hpp,cpp}` with `optimize(Mir, TargetSchema, OptPipeline, reporter) → Mir` + closed `PassId` enum (cycle 1: only `Identity` no-op). (c) compile_pipeline step 3.5 between MIR build + MIR→LIR, wired with default `[Identity]` pipeline so cycle 1 exercises the engine's loop + dispatch end-to-end without behavioral change. (d) New `X_UnknownPassId = 0x2001` diagnostic (the X_* family opener for the optimizer tier). **3-agent post-fold** (strong convergence): code-reviewer C1 missing `<vector>` include → fixed; C2 + silent-failure F1 silent enum-drift fallback → X_UnknownPassId now emits before return-false; I1 `OptPipeline::name` string_view dangle risk → switched to owned `std::string` before OPT2 JSON pipelines land; silent-failure F2 empty-pipeline never exercises Identity → default is now `{Identity}` so the dispatch loop ALWAYS runs; silent-failure F3 false-return-without-diagnostic contract → entryErrorCount snapshot + assertion belt-and-suspenders in optimize(); silent-failure F4 cse_noncommutative comment arithmetic corrected (70 buggy-CSE, not 76). **Newly anchored future cycles**: D-OPT1-PASS-ID-STABILITY (ordinal-append-only); D-OPT1-VERIFY-AFTER-EVERY-PASS (slot for OPT2's first real pass to land the verifier hook); D-OPT1-PIPELINE-FROM-CONFIG (resolve pipeline name → PassId list from JSON when OPT2 lands); D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT (every false-return path emits a diagnostic); D-OPT1-PASS-RUN-MAX-ITER (`vector<PassId>` → `vector<PassRun>` when first fixed-point pass lands); D-OPT1-PIPELINE-BUDGET (OPT10 autotuner concern); D-OPT1-PIPELINE-MAX-ITERATIONS (whole-pipeline-fixed-point if/when needed). Total corpus: **19 runnable c-subset programs** (14 positive shape + 5 OPT1-negative-pin). 172/172 ctest. **NEXT cycle 2 = OPT2 const-fold** (the highest-leverage pass; reuses CE1-CE5's const-eval engine; differential-verified against all 19 corpus pins). Per code-architect Q3: jump straight to const-fold — dominators (mir_dominators) NOT needed yet; lands later with copy-prop. | step 13.5 ✅ | ✅ **cycle 1 CLOSED 2026-06-03** |
| 13.7 cycle 1 | [`22`](./22-optimizer-plan%20-%20tbd.md) OPT2 const-fold | ✅ **CLOSED 2026-06-03 — MIR-tier ConstFold pass + JSON pipeline loader + verifier hook + differential-verify runner + OptResult shape + FFI dead-code removal.** PassId::ConstFold landed alongside the engine substrate: `runConstFold(Mir, TypeInterner, reporter) → ConstFoldResult` rebuilds the MIR functionally via `MirBuilder`, walks each function's blocks/insts in scan order with a `MirInstId.v → MirInstId` rewrite map, folds any pure-functional integer/bool/bitwise/ICmp opcode whose operands all resolve to `Const` via the `hir/const_eval_arith.hpp` helpers (`applyBinaryInt` / `applyUnaryInt` / `wrapToIntTarget`). Wrap semantics (no per-inst `nsw`/`nuw`); div-by-zero + shift-out-of-range defer to runtime via verbatim copy. **JSON pipeline loader** (D-OPT1-PIPELINE-FROM-CONFIG): `*.pipeline.json` 7-step loader mirroring `TargetSchema::loadFromText`; ships `debug.pipeline.json` (Identity) + `release.pipeline.json` (Identity+ConstFold) under `src/dss-config/pipelines/`. Unknown-keys + empty-passes-array reject loud (`X_PipelineMalformed`); unknown pass names → `X_UnknownPassName` (config-load analog of runtime `X_UnknownPassId`); 4 new diagnostic codes 0x2002-0x2005. **PassId single-source table** (`kPassNameTable`): one row per pass; `optPassIdFromName` + `optPassIdName` + ordinal-stability static_assert all derive from it. Closes the 4-site drift the 7-agent review surfaced. **D-OPT1-VERIFY-AFTER-EVERY-PASS** live: `MirVerifier::verify` runs after every successful pass under all build modes; verifier-detected SSA violations are build breaks. `optimize()` signature widened with `TypeInterner const&` so the verifier's interner-gated rule set (CondBr-is-Bool, Return-matches-FnSig, Arg-in-range, no-Extension-types) is active. **OptResult shape** (D-OPT1-OPT-RESULT-SHAPE): bool→`OptResult { ok, passesRun, passesMutated, fixedPointReached }`. **examples_runner differential-verify** (D-OPT1-DIFFERENTIAL-VERIFY-RUNNER): manifests declare `optimizedPipelines: [{label, passes}]`; runner compiles + spawns each arm with `Program::setOptimizerPipelineOverride`, asserts baseline-equal exit + stdout. `ArmResult` is a tri-state enum (Ran / SkippedCrossHost / Poisoned) — closes the silent compile-fail-vs-cross-host conflation. First arm pinned on `const_fold_inside_expr` (exit 42, constfold-only); other 4 negative pins arm up when their target pass lands. **MirBuilder rewrite-map fail-loud** (D-OPT2-REWRITE-MAP-COMPLETENESS): rewriteOperand aborts on missing entry; pre-fold the invalid `MirInstId{}` was admitted by three downstream safety nets (cross-arena check + 2 verifier `if (!op.valid()) continue` arms). **3 unit-test files** under `tests/opt/`: test_optimizer (UnknownPassId 1-count + OptResult shape + optPassIdFromName all-enumerators); test_const_fold (Add/Sub fold + Arg-not-foldable + div-zero defer + ICmp→Bool + **G1 i32-wrap-on-overflow** + **G3 nested constCache pickup** + **G4 multi-block CondBr CFG preservation**); test_pipeline_loader (shipped debug/release + 6 reject paths inc. empty-array + malformed-JSON + non-string-entry). **FFI dead-code removal**: `src/dss-config/ffi-headers/` tree + `findShippedFfiHeader` + `readCHeaderShipped` + `HeaderReadErrorKind::InvalidShippedPath` + 8 corresponding tests removed — sole consumers were the tests of the headers themselves; no production caller. Diagnostic codes `C_InvalidShippedFfiHeaderPath` (0xC034) + `F_HeaderInvalidShippedPath` (0x500F) retired (numbers reserved, marked "RETIRED 2026-06-03"). FF1/FF2/FF5 substrate (readCHeader/readCHeaderFromText/binary_readers/ingest()) stays as latent capability — unused features, not dead code. **7-agent review fold** consolidated (Critical: rewriteOperand fail-loud + runtime-init-globals ok=true skip + fs::file_size no-throw; High: PassId table consolidation + isICmp removal + closed-key helper + ArmResult enum + OptPipeline empty-passes reject + comment cleanup). **6 anchors registered** for follow-up: D-OPT-COMPILE-OPTIONS-STRUCT (8th parameter trigger), D-OPT-FIXED-POINT-LOOP (sub-pipeline design), D-OPT-INTERNER-MUTABLE (first type-creating pass), D-OPT-PASS-METRICS (sink-pattern), D-OPT-PRE-PIPELINE-VERIFY (debug-mode), D-OPT-RUNNER-SHARD (corpus×pipeline scaling). 175/175 ctest. Differential-verify runner caught zero issues — both arms identical exit 42 on const_fold_inside_expr, confirming ConstFold neither breaks nor mis-optimizes the live-edge pattern. | step 13.6 ✅ | ✅ **cycle 1 CLOSED 2026-06-03** |
| 13.7 cycle 2+ | [`22`](./22-optimizer-plan%20-%20tbd.md) OPT3+ | DCE (D-OPT2-DCE-LINKAGE-SYMTAB-ASSERTION pin) → copy-prop + dominator-tree + liveness → LICM → CSE/GVN → tier-neutral `scalar_eval` consolidation → cost/machine/peephole/pipeline as JSON → OPT10 autotuner. | step 13.7 cycle 1 ✅ | ⏳ planned |
| 13.8 | [`11`](./11-ffi-plan%20-%20tbd.md) **FF11** | **Language-agnostic shipped-lib ingestion** — call shipped-library externs (libc/kernel32/libSystem stdlib) WITHOUT inline `extern` re-declaration. Shared platform-keyed neutral store (`src/dss-config/shippedLibs/<platform>/*.json`, described once across languages) + per-language `shippedLibDirs` ordered search-path (references, doesn't own) + neutral JSON `ImportSurface` format (one universal reader; `.h` = one dialect, not the format) + lazy resolution feeding the EXISTING `ingest()` matcher. Closes D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC + D-FFI-HEADER-VALIDATION-OPTIONAL. **Scheduled right after the optimizer arc** — off the optimizer's critical path; NOT dependency-blocked by it (reuses the proven `ImportSurface` / `ingest()` / `FfiMetadata` back half — a discovery front-door + neutral JSON reader, not a new pipeline). | step 13.7 (scheduled-after, not dependency-blocked) | ⏳ planned |
| **`★ STAGE 2 — CROSS-HOST (gated on CI runners or hardware; do NOT pursue without execution-verification)`** | | | | |
| 13X.1 | [14](./14-linker-plan%20-%20tbd.md) **D-LK10-ENTRY-ARM64** | ARM64 syscall (`svc #0`) + GOT/PLT macro-op (ADRP + LDR + BLR) encoding rows in `arm64.target.json` + per-format processExit blocks for `elf64-aarch64-linux-exec`. The trampoline emitter is bucket-2 universal — zero code changes; JSON-only. **BLOCKED on**: ARM64 hardware OR ARM64 CI runner. Without execution verification this would land byte-correct but unverified, regressing the methodology that caught the SEGV + the 42+0=84 silent miscompile. | (D-FF6-RUNTIME-PRINT ✅) + ARM64 runner | ⏳ deferred until verifiable |
| 13X.2 | [14](./14-linker-plan%20-%20tbd.md) **D-LK10-ENTRY-MACHO-EXIT** | Mach-O exit mechanism — BSD `exit` syscall (`0x2000001`) OR libSystem `_exit` by-name import. ProcessExit row in `macho64-x86_64-darwin-exec.format.json`. **BLOCKED on**: macOS hardware OR macOS CI runner. Same execution-verification gate as 13X.1. | (D-FF6-RUNTIME-PRINT ✅) + macOS runner | ⏳ deferred until verifiable |
| 13X.3 | [11](./11-ffi-plan%20-%20tbd.md) FF1-PE + FF1-MachO | PE/COFF export-table reader (`.edata` + `.idata`) + Mach-O `LC_SYMTAB` + `LC_DYSYMTAB` + `LC_DYLD_INFO_ONLY` bind-opcode reader. **Not a blocker for by-name imports** (the runtime loader resolves by name; hello_puts proves this works without binary readers). Value is signature validation against shipped DLLs/dylibs — nice-to-have. | trigger: first PE/macOS corpus that needs binary-reader-validated externs | ⏳ low priority |
| 13X.4 | [14](./14-linker-plan%20-%20tbd.md) **D-LK10-CRT-INIT-INVOKE** | msvcrt CRT init shim (`_initterm` + `__getmainargs` + `_iob_func`) from trampoline prologue before `call main`. **NOT the blocker** for `puts` (msvcrt's DllMain self-inits stdio; proven 2026-06-02). Required for: `printf` with `%f` (locale-dependent), `fopen` (locale-aware paths), `atexit`, static-init / C++ ctors. Anchored as future enabler. | trigger: first c-subset example crashing on uninitialized CRT state (printf-with-float / fopen / locale) | ⏳ low priority |
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

> **v1 paths (two distinct arcs from step 13 — pursue Stage 2 first, signed binary second):**
>
> - **Stage 2 — c-subset corpus expansion (strict sequence, 2026-06-02+):** 13 → **13.1** (ML7-2.2 stack args) → **13.2** (c-subset `void*`) → **13.3** (hello_writefile — CRT-free hermetic print) → **13.4** (variadic ABI + c-subset `...` grammar) → **13.5** (non-trivial host-runnable corpus expansion) → **13.6** (OPT1 const-fold + DCE + copy-prop) → **13.7** (OPT2–OPT10 multi-tier optimizer) → **13.8** (FF11 — language-agnostic shipped-lib ingestion: shared platform-keyed neutral JSON store + per-language `shippedLibDirs` search-path + lazy resolution onto the existing `ingest()` matcher). Steps 13.1–13.5 build the corpus the optimizer differentially verifies against; sequencing optimizer after corpus is deliberate ("runs everywhere" before "runs fast" — but "runs USEFULLY" before either). 13.8 is scheduled after the optimizer (the user's call) but is NOT dependency-blocked by it — it reuses the proven FFI back half, so it could move earlier if priorities shift.
> - **First signed binary:** 13 → 14 (debug-info) → 15 (codesign) → 16 (build CLI). Independent of Stage 2 file-wise (no shared editing surface). Can run in parallel structurally but the audit-fold discipline (7-agent + cross-plan sweep per cycle) favors serial focus — pick one cycle's worth of work per landing.
> - **Cross-host (steps 13X.*) — BLOCKED on CI runners or hardware.** ARM64 + Mach-O codegen would land byte-correct but unverified-at-runtime, regressing the execution-verification methodology that has caught every silent bug on this branch (the SEGV; the regalloc 42+0=84; the wrong-call-opcode IAT trap). Do NOT pursue these without a host that can spawn the produced binary. FF1-PE/MachO binary readers similarly are nice-to-have (signature validation), not blocker (by-name imports work without them).
>
> **P1 (artifact-profile)** must land before step 13 and step 16 reach acceptance — schedule it anywhere in parallel from step 1 onward.

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
| D5 | **c-subset structs / unions / enums** | 🟡 **PULLED FORWARD 2026-05-28** (before ML2) to make ML2's HIR→MIR lowering honor "no real-blocker deferrals" on `MemberAccess`/`ConstructAggregate`/`TypeDecl`. **Kitchen-sink scope**: core structs + typedef'd/anon + designated initializers + unions + enums + first-class MIR aggregates (ExtractValue/InsertValue). **6-PR sub-arc**: **D5.1 core structs ✅** → D5.2 typedef'd/anon → **D5.3 designated init ✅ (cycle 1a + 1b + SP3)** → **D5.4 unions ✅** → **D5.5 enums ✅** → **D5.6 MIR aggregate opcodes ✅** → then ML2. **D5.6 landed 2026-05-28** (commit `6007fde`+ review fix-up `65cbd53`): `MirOpcode::ExtractValue` + `InsertValue` first-class aggregate read/write opcodes with Gep-shaped operand layout `[agg, (value,) idx0, idx1, ...]` (indices are Const-i32 operands). `MirBuilder::addExtractValue` / `addInsertValue` helpers + shared `appendIndexPathOperands_` private helper. 7-agent review fix-up: docstring contradictions corrected (was `scalars[0..N)`, now `operands[1..N)`); InsertValue result-type-must-match-aggregate-type fatal added; missing tests landed (path-value-correctness, InsertValue empty-path death, InsertValue nested, result-type mismatch death). 9 D5.6 tests + 89/89 ctest. **D5 sub-arc CLOSED** — ML2 can now wire `MemberAccess` → ExtractValue and `ConstructAggregate` → chained InsertValue. **D5.6-FU1 anchored** (scalars-encoded path) — substrate-tier rework opportunity flagged by simplifier; type-design reviewer recommends staying with Gep convention for use-list/RAUW uniformity. Decision deferred to ML2/ML3 verifier cycle when first concrete consumer measures the cost. **D5.5 landed 2026-05-28** (commit `f0ddfa8`): `TypeKind::Enum` + `TypeInterner::enumType(name, underlying=I32)`; `CompositeKind::Enum` as third Pass 1.5 dispatch; Pass 1.5 enum branch sets each enumerator's type to the enum TypeId AND lifts enumerator bindings to the enclosing scope so C-classic `enum E { A } ... A` works; same-name collision in the enclosing scope emits S_RedeclaredSymbol; `requireFieldTypes` gate conditional on composite kind; c-subset grammar adds `enumDecl`/`enumerator`/`enumTypeRef` (mirrors D5.3's braceInitList trailing-comma pattern to avoid FIRST-token alt-ambiguity); HIR text format adds `enum "Name"` emit/parse (round-trip currently lossy for non-I32 underlying — v1 schemas don't expose underlying selection). 4 D5.5 tests; 36/36 D5 lowering tests total (20 D5.3 + 12 D5.4 + 4 D5.5); 89/89 ctest. **§D5-addressable-followups CLOSURE PASS 2026-05-28** (commit `fe87994`): the bundleable items landed in one focused cycle to avoid passing through the same code twice ahead of ML2 cycle 6. **CLOSED**: D5.4-FU1 (`HirVerifier::checkConstructAggregate` rule — Struct=N, Union=1, Array=length, with per-child type-match; subsumes FU4 positional type-mismatch); D5.4-FU3 simplifier helper consolidation (`peelToDesignatorLeaf` + `firstIdentifierToken` + unified `synthZeroOrError` Struct/Union arm); D5.5-FU2 `liftToEnclosingScope` config opt-out (c-subset opts in explicitly; future Rust-style enum schemas can omit); D5.5-FU3 `synthZeroOrError` Enum arm (zero literal tagged AS the enum TypeId, preserving nominal identity); D5.5-FU5 HIR text underlying-type round-trip (`enum "E" : <kindOrdinal>` when underlying ≠ I32); D5.5-FU4 test coverage (added `D5_5_EnumHirTextRoundTrip` pinning the full declare → lower → emit → re-parse → verify path). **STILL QUEUED** (tier 2 — own substrate cycles): D5.4-FU2 explicit variant-tag attribute (deferred to ML2 union-lowering cycle when codegen needs the explicit tag — implicit-tag-by-child-type is now type-checked by the new verifier rule); D5.6-FU1 scalars-encoded path (measurement-gated, stay with Gep); ~~Plan 12.5 §0.2 D6 CST-side const-eval~~ ✅ **closed 2026-05-28** (commits `723bea4`+`dcbfe2b`: shared CST engine + 3-site wiring + 14 tests + 7-agent review fix-up — Ref support for `isConst`-bound symbols via `CstSymbolInitResolver` closure; D5.3/D5.5 non-literal cases now fold); Plan 12.5 §0.2 D1b F128; nominal-collision intern-key disambiguator (struct/union/enum). **§D5-addressable-followups queued 2026-05-28** (NOT deferrals — addressable inline; cycle order is non-blocking; surfaced by the D5.3/D5.4 7-agent reviews + the latest audit): (a) **D5.4-FU1 `ConstructAggregate` arity-by-TypeKind verifier rule** — Struct=fieldCount / Union=1 / Array=length. Promotes the helper-only invariant in `lowerBraceInit`/`lowerUnionBraceInit`/`synthZeroOrError` to a checked HirVerifier rule. ~30 LOC + a negative test. Cost-low; lands any time the verifier surface is open. (b) **D5.4-FU2 Explicit variant-tag attribute on union `ConstructAggregate`** — `HirAttribute<std::uint32_t> activeVariant` populated by `lowerUnionBraceInit` from the resolved variant index. Closes the implicit-tag gap MIR/codegen would otherwise inherit (member-access on a union currently obliges MIR to walk back to the producing aggregate to recover which variant is active). Additive side-table; no opcode change. (c) **D5.4-FU3 simplifier-helper consolidation** — extract `peelToDesignatorLeaf(NodeId) → {core,rule}` + `firstIdentifierToken(NodeId)`; fold `synthZeroOrError` Struct/Union arms into one composite arm parameterized by `(unionKind ? 1 : N)`. Pure refactor; replaces 3 hand-rolled copies. (d) **D5.4-FU4 positional-type-mismatch diagnostic** — bug-reviewer flagged silent path: positional union init with a value whose type doesn't match the first variant currently lowers without a diagnostic (the verifier doesn't type-check ConstructAggregate children). The arity verifier rule from (a) is the natural place to add per-child type-match for both struct and union. (e) **Plan 12.5 §0.2 D6 CST-side const-eval** — anchored already in plan 12.5; the only D5.3 substrate-blocker still locked-in via `D5_3_NonLiteralIndexDesignatorEmitsDiag`. Fresh-audit candidate: the current engine "consumes HIR" but might grow a CST sibling with modest work; revisit before ML2 resumes. (g) **D5.5-FU1 explicit enumerator value computation** — `enum E { A = 5, B }` parses cleanly but the explicit value is NOT yet computed; Pass 1.5 only sets the type. C99 semantics: explicit value goes through const-eval; subsequent enumerators auto-increment (`A=5, B=6`). Needs an EnumValue side-table on the SemanticModel (TypeId+fieldIndex → int64) populated by Pass 1.5 via const-eval on the explicit-value expression. (h) **D5.5-FU2 `liftToEnclosingScope` config opt-out** — currently the "lift enumerator bindings to enclosing scope" behavior is hardcoded to `CompositeKind::Enum`. A future enum-bearing language wanting Rust-style `E::A`-only would have no opt-out. Add `FieldChildrenDescriptor::liftToEnclosingScope: bool` (default false, c-subset opts in explicitly). (i) **D5.5-FU3 `synthZeroOrError` Enum arm** — currently falls through to the scalar default, which produces an I32 zero literal tagged as the underlying I32 type, not the enum TypeId. Should emit a zero literal tagged with the enum's TypeId. (j) **D5.5-FU4 test-coverage gaps from review** — assert enumerator's actual type (not just "no errors"); negative test for the redeclaration-on-lift diagnostic; HIR text round-trip for an enum-typed program. (k) **D5.5-FU5 HIR text underlying-type round-trip** — `enum "Name"` emits without the underlying; parser re-interns with default I32. Round-trip is currently lossy for non-I32 underlying (no v1 consumer needs this; revisit when a schema declares `enum E : u8`). (f) **Plan 12.5 §0.2 D1b F128 Cast target** — `HirLiteralValue::value` variant lacks a `long double` / `__float128` arm. Listed in plan 12.5 §0.2 as deferred-by-design; "no v1 consumer" may not satisfy the no-deferrals rule. Fresh-audit candidate; would need a substrate variant extension + soft-float helper paired (similar to D1's `narrowToHalf`). Items (a)–(d) intentionally sit AFTER D5.5/D5.6 in the cycle order because they touch the verifier/HIR substrate which D5.5 enums + D5.6 aggregate opcodes will also touch — batching them avoids two passes over the same code. **D5.4 landed 2026-05-28** (commit `968f301` on `feature/mir-lir`): c-subset adds `UnionKeyword`/`unionDecl`/`unionField`/`unionTypeRef`; new `FieldChildrenDescriptor::compositeKind: CompositeKind::{Struct,Union}` additive config field; Pass 1.5 dispatches struct vs union interning by the flag; new `lowerUnionBraceInit` implements C99 §6.7.8 union semantics (positional → first variant, designator → named variant, empty `{}` → first-variant zero-fill, multi-element / unknown-variant / index-designator all emit specific diagnostics); `synthZeroOrError`'s Union arm now produces a 1-child aggregate (was N-child like struct — wrong). 7 D5.4 tests; 27/27 D5 lowering tests total (20 D5.3 + 7 D5.4); 89/89 ctest. **D5.4 landed 2026-05-28** (commit `968f301` on `feature/mir-lir`): c-subset adds `UnionKeyword`/`unionDecl`/`unionField`/`unionTypeRef`; new `FieldChildrenDescriptor::compositeKind: CompositeKind::{Struct,Union}` additive config field; Pass 1.5 dispatches struct vs union interning by the flag; new `lowerUnionBraceInit` implements C99 §6.7.8 union semantics (positional → first variant, designator → named variant, empty `{}` → first-variant zero-fill, multi-element / unknown-variant / index-designator all emit specific diagnostics); `synthZeroOrError`'s Union arm now produces a 1-child aggregate (was N-child like struct — wrong). 7 D5.4 tests; 27/27 D5 lowering tests total (20 D5.3 + 7 D5.4); 89/89 ctest. **D5.1 landed in 5 additive checkpoints** (`feature/mir-lir`): `251eca0` (config vocab + `SymbolRecord::fieldIndex`/`structScope`), `9079916` (Pass 1/1.5/2 engine consumption + `S_NotAPointer`/`S_NotAComposite` codes + cross-field loader validation), `09916d5` (`followerRule` operator-table primitive + `MemberAccessRule.operatorToken` discriminator), Cycle 3 (struct schema wired end-to-end through parse+semantic: `struct Foo { ... };` definitions + member access in PARAM and LOCAL VAR positions; struct types compose via `structType(name, fields)`; field uses resolve via Pass 2), and Cycle 4 (HIR MemberAccess lowering: `s.x` → `MemberAccess(Ref, idx)`, `p->x` → `MemberAccess(Deref(p), idx)`; new `HirVerifier::checkMemberAccess` rule for fieldIndex-bounds + base-must-be-composite; defensive lowering check that resolved symbol is actually a field; dot-form + arrow-form + verifier-negative tests). **Scope cut documented**: top-level `struct Foo x;` globals + `struct Foo make()` struct-returning functions deferred to D5.2 — converting `topLevelDecl`/`externDecl` to `typeRefAllowingStruct` would re-introduce the StructKeyword first-token collision with `structDecl` in the `topLevel` alt; D5.2 lands the shared-prefix factoring + typedef'd structs (`typedef struct Foo Foo;`) that lifts the limitation. **D5.2 cycle 1 ✅** added `Identifier` to `typeBase` (typedef-alias at top level + bare-struct-tag usage in type position — known C divergence: single namespace + SE5 alias path means every struct tag is implicitly usable as a bare type name; pinned with 3 tests including the same-name redeclaration negative). **D5.2 cycle 2 attempted + reverted (diagnosis refined cycle 3)**: marking `statement` alt speculative + adding `Identifier` to `typeBaseAllowingStruct` caused widespread parser-test regressions. **Cycle 3 investigation corrected the root cause**: TreeBuilder::Checkpoint DOES already snapshot+rollback the DiagnosticReporter (line 1231 of tree_builder.cpp), so diagnostic rollback is NOT the issue. The real architectural blocker is the **speculative-alt branch-success criterion**: `trySpeculativeBranch` commits the first branch whose frame closes structurally, even if the branch needed internal recovery to get there. For `f(a, b);` inside a block: speculative `statement` tries `varDecl` first (Identifier in FIRST after the typeBase widening); `varDecl` parses `f` as `typeBase`, expects another Identifier, sees `(`, recovers internally (emits diag, advances, frame closes structurally) → probe sees frame closed → commits → rest of input mis-parsed. Fix requires either: (a) tightening the speculative-success criterion (`probe.emittedDiag() || probe.hasInnerErrors()` already exist but aren't all wired), (b) k>1 lookahead disambiguation, or (c) restructuring alts so FIRST sets are unambiguous (defeats the purpose). **(a) attempted cycle 4 + reverted**: adding the missing `probe.emittedDiag()` pre-commit check improved the situation (7→5 failures, no aborts) but left `f(a,b);`-style calls in blocks failing both branches with `P_BacktrackFailed` — deeper interaction with SchemaWalker desync-latch and/or per-probe diagsBefore_ baseline still needs untangling in a focused substrate-tier sub-PR. **Decision**: pivot to ML2 (D5.1 + D5.2 cycle 1 are sufficient for `MemberAccess` + `TypeDecl` end-to-end; `ConstructAggregate` stays a real-blocker defer until D5.3). **ML2 cycle 1 ✅ landed 2026-05-28**: `src/mir/lowering/` OBJECT lib + straight-line vertical slice through c-subset (Function + Block + Return + Literal + Ref + 16 BinaryOps + implicit-void-return synthesis + fail-loud unsupported handling). 88/88 ctest. See plan 12 ML2 row. **All remaining D5 sub-features** (block-scope alias, anonymous structs `struct { … } var;`, top-level `struct Foo x;` via shared-prefix factoring, designated initializers in init context) **share the same speculative-alt or multi-token-disambiguation engine need**. Recommended path: either focused substrate-tier sub-PR on speculative-alt diagnostic rollback (unlocks every remaining D5 feature), OR pivot to ML2 with D5.1 + D5.2 cycle 1 substrate (sufficient for `MemberAccess` + `TypeDecl` lowering; `ConstructAggregate` stays a documented fail-loud-defer in ML2 until D5.3). **87/87 ctest** throughout. | feature (multi-PR) | the 6-PR D5 sub-arc (D5.1 ✅; D5.2 in progress) | maximally honest c-subset for ML2 |
| D6 | **tsql JOINs / subqueries / CTEs / stored-procs / transactions** | Language-surface expansion; current tsql is single-table query/DML + CREATE TABLE. | feature (v1.x) | a future `tsql-subset` expansion round | same |
| ~~D7~~ | **Ternary `? :` (mixfix)** | ✅ **closed 2026-05-28 (gap-closure G6).** Added `OperatorArity::Ternary` (4th arity, fits the 2-bit key-pack) + `OperatorTable::Entry.ternaryMiddle` + an optional `expr.wrapperRules.ternary`; a `DefaultPrattWalker` mixfix branch (condition at prec+1, middle at minPrec 0, else at prec → right-assoc) + `lowerTernary` → the existing HIR `Ternary` node. Config-driven, no `?:` special-casing. | (closed) | — | — |
| D8 | ~~**Unused-variable**~~ → **write-only / dead-store only** | ✅ **never-referenced closed 2026-05-26** (registry had this mis-classified as optimizer-blocked). A config-driven `warnIfUnused` flag on `DeclarationRule` + a post-pass-2 sweep of SE7's `usesBySymbol` reverse index emits `S_UnusedVariable` (a WARNING) for an opted-in symbol with an empty use-set — no CFG needed. Per-decl opt-in (c-subset locals, not params/globals/columns); proven generic via Synth5. **Remaining:** write-only / dead-store ("assigned but never read") and dead-code — those DO need dataflow/liveness, so they stay with the optimizer. | blocked (optimizer) | [`22`](./22-optimizer-plan%20-%20tbd.md) optimizer (dead-code → OPT2 DCE; write-only/dead-store → OPT3 DSE); [07](./07-production-readiness-plan%20-%20tbd.md) G-210 | optimizer phase (CFG/liveness exists) |
| D9 | **Use-before-init / unreachable-after-return** | Needs a control-flow graph; the semantic layer is pre-CFG. HIR/MIR will have the CFG to do this strength of analysis. | blocked (HIR/MIR) | phase #9 HIR + [`22`](./22-optimizer-plan%20-%20tbd.md) optimizer (CFG/dominators in OPT1) | CFG exists |
| D10 | **CU6 cross-CU references / incremental rebuild** | v1 is single-CU-per-binary; cross-CU + incremental are paired CU6 ↔ LK11 substrate hooks already reserved. | blocked (v1.x) | [08](./08-compilation-unit-plan%20-%20tbd.md) CU6 + [14](./14-linker-plan%20-%20tbd.md) LK11 | artifact profile needing multiple CUs |
| D11 | **Full C declarators (c-subset)** — function pointers `int (*f)()`, arrays-of-pointers `int *a[10]`, multi-declarator `int *a, b` | Successor to the now-closed D1. Gap-closure G5 (2026-05-28) gave c-subset single-declarator pointer DEPTH (`int *p`/`int **p` → `Ptr` via `semantics.pointerToken`) and HR9 gave array suffixes (`int a[10]`) — but the FULL C declarator grammar (parenthesized declarators binding a name through pointer+function+array layers, and comma-separated multi-declarators where `*` binds per-declarator) is a distinct grammar+semantic round. Not failing loud today: these forms simply don't parse (a parse error), they don't miscompile. Config + grammar work, no downstream phase needed. | feature (grammar) | a future c-subset declarator-grammar round; relates to D5 (structs) language-surface expansion | a corpus program needs function pointers / arrays-of-pointers / multi-declarators |

> **Pre-HIR readiness:** none of the remaining items blocks opening **HR1** (HIR). After the 2026-05-26 closure pass the open set is: **D1** (pointer/array type-level — feature/grammar round, *when* a corpus needs typed pointers or HIR lowers them), **D3-residual** (`VARCHAR(N)` parameter — feature/grammar round, *when* tsql needs precise length typing), **D4-residual** (AP2–AP4 + CLI — owned by phase #12 `program-api`), **D5/D6** (c-subset and tsql language-surface expansion — v1.x feature rounds, *when* the credibility-as-real-language goal or a corpus needs them), **D7** (ternary/mixfix — its own schema-v3 round, *when* a shipped language needs it), **D9** (use-before-init / unreachable — *when* HIR's CFG exists), and **D10** (cross-CU / incremental — *when* an artifact profile needs multiple CUs). D9 is the only one HIR itself helps unblock.

## 0.3 Diagnostic-code nibble registry (cross-plan authority)

**Single authoritative table for the diagnostic-code high-nibble allocation.** The shipped implementation lives in `src/core/types/parse_diagnostic.cpp`'s `diagnosticCodePrefix()` switch (the runtime that maps the numeric code to its rendered prefix letter). This table mirrors that; **the cpp file is the source of truth, this is the planning view** so cross-plan PRs don't accidentally re-use a slot. Update both when a new family lands.

| Nibble | Rendered prefix | Family | Owner plan | Status |
|---|---|---|---|---|
| `0x0xxx` | `P_` | Parse | [`05`](./05-parser-plan%20-%20ok.md) | ✅ shipped |
| `0x1xxx` | `A_` | Assembler (byte-encoding pass) | [`13`](./13-assembler-plan%20-%20tbd.md) | ✅ shipped (AS1 cycle 1 — 2026-05-29) |
| `0x2xxx` | `X_` | Optimizer (pass pipeline / cost-model / verify-after-pass / linkage-liveness) | [`22`](./22-optimizer-plan%20-%20tbd.md) | ⏳ reserved (claimed by plan 22 rev 1; lands with OPT1) |
| `0x3xxx` | — | (reserved — free) | — | available |
| `0x4xxx` | `R_` | Register allocator | [`12`](./12-mir-lir-plan%20-%20ok.md) | ✅ shipped (ML6 cycle 3a) |
| `0x5xxx` | `O_` / `F_` | Object format / linker / FFI | [`14`](./14-linker-plan%20-%20tbd.md), [`11`](./11-ffi-plan%20-%20tbd.md) | ✅ live (FF1–FF5 codes 0x5000–0x5019 shipped — F_* prefix used for the FFI half; linker O_* codes still reserved within the band per the original allocation) |
| `0x6xxx` | `W_` | WAT/WASM verifier + emit-side | [`18`](./18-wasm-plan%20-%20tbd.md) §2.9b | ⏳ reserved (post-rev-3 allocation) |
| `0x7xxx` | `V_` | SPIR-V verifier + emit-side | [`17`](./17-shader-gpu-plan%20-%20tbd.md) §2.9 | ⏳ reserved (post-rev-3 allocation) |
| `0x8xxx` | `K_` | Linker (object format engine + reloc apply + file emission) | [`14`](./14-linker-plan%20-%20tbd.md) | ✅ shipped (LK1–LK10 + post-fold additions through 2026-06-02; 14 K_* codes 0x8001–0x800B + 0x800D–0x800F — `K_EntryPointResolvesToExtern`, `K_DuplicateDataSymbol`, `K_BssDataHasBytes`) |
| `0x9xxx` | `P_` | Parse (internal-invariant range, distinct from 0x0xxx user-facing) | [`05`](./05-parser-plan%20-%20ok.md) | ✅ shipped |
| `0xAxxx` | `I_` | MIR (IR-gen mid-level + verifier) | [`12`](./12-mir-lir-plan%20-%20ok.md) | ✅ shipped (ML3) |
| `0xBxxx` | `L_` | LIR lowering + verifier | [`12`](./12-mir-lir-plan%20-%20ok.md) | ✅ shipped (ML5–ML8) |
| `0xCxxx` | `C_` | Config-loader (`.lang.json` / `.target.json` / future `.format.json`) | [`02`](./02-schema-expressiveness-v2-plan%20-%20ok.md) + cross-plan | ✅ shipped (0xC001..0xC033 in production) |
| `0xDxxx` | `D_` | Driver / compilation-unit / import resolver | [`08`](./08-compilation-unit-plan%20-%20tbd.md) | ✅ shipped (CU2+ ImportResolver) |
| `0xExxx` | `S_` | Semantic analysis | [`08.6`](./08.6-semantic-plan%20-%20ok.md) | ✅ shipped (SE1–SE7) |
| `0xFxxx` | `H_` | HIR (verifier + text + lowering) | [`09`](./09-hir-plan%20-%20ok.md) | ✅ shipped (HR1–HR11) |

**Allocation discipline:**
- Each family owns its full nibble (4096 codes); cycles within a plan allocate codes incrementally without coordinating with other plans.
- Before adding a new family, **claim the slot here AND in `parse_diagnostic.cpp`'s `diagnosticCodePrefix()` switch** in the same PR. Reading either in isolation is the failure mode.
- Free slots: `0x3xxx` (`0x2xxx` now claimed by plan 22 — optimizer `X_*`). Post-v1 candidates (JVM IL, .NET IL, future shader-stage validators, future debug-info-emit) draw from `0x3xxx`.
- **DO NOT subdivide existing nibbles across families.** Pre-rev-3, plans 18 + 17 had each claimed sub-bands `0xC1xx` / `0xC2xx` within the `C_*` config nibble — silent collision with the shipped `C_001..C_033` range. Corrected to top-level nibble ownership.

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
│  │  dss-config   │──▶│   source-factory      │──▶│  core/types   │       │
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
│   ├── dss-config/                          # ── DSS Configuration Files ──
│   │   ├── README.md                        # How to write a language / target config
│   │   ├── schemas/
│   │   │   └── language-schema.json         # JSON Schema for validation
│   │   ├── sources/                         # per-language grammars (was `languages/`)
│   │   │   └── example.lang.json            # Example language definition
│   │   └── targets/                         # per-target ISA configs (ML5+)
│   │       └── x86_64.target.json           # Example target definition
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

**Status (2026-05-30): shipped at plan 14 LK10 cycle 2.** The v1 surface diverges from the historical TargetInfo-struct sketch — targets are passed as `"<targetName>:<formatName>"` strings (e.g. `"x86_64:elf64-x86_64-linux"`) so the C-ABI (`dss_compile_directory`) stays a flat `const char**` array. The richer `TargetInfo`-style struct will arrive when LK10 cycle 3 ships CLI flag routing (`--target`/`--format`) backed by `TargetSpec` parsing (see `src/program/target_spec.hpp`).

```cpp
class Program {
public:
    Program() = default;
    ~Program() = default;

    /// CLI entry point — currently dispatches `--lsp` mode only.
    int run(int argc, char* argv[]);

    /// Compile from a .dsp project file (self-contained — language, files, targets defined inside).
    /// Cycle 2 stub: fails loud with `D_PlanNotLanded` until plan 06 (`.dsp` parser) ships.
    int compileProject(const std::string& projectFilePath);

    /// Compile an explicit list of source files for the given language to the given target(s).
    /// Each target encodes "<targetName>:<formatName>" — both halves are `loadShipped` keys.
    int compileFiles(
        const std::vector<std::string>& sourceFiles,
        const std::string& languageName,
        const std::vector<std::string>& targets
    );

    /// Compile all matching source files found in a directory (recursive scan by language extension).
    int compileDirectory(
        const std::string& directoryPath,
        const std::string& languageName,
        const std::vector<std::string>& targets
    );
};
```

**Behavior (post-LK10 cycle 2):**
- `compileFiles` loads the language schema once, builds ONE multi-file `CompilationUnit`, drains CU + per-Tree + SemanticModel diagnostics into the run-wide reporter, then per-target compiles through the full HIR → MIR → LIR → ASM → link → writeImage pipeline (`src/program/compile_pipeline.{hpp,cpp}::compileSingleUnit`).
- Output path: `<cwd>/target/<formatName>/<stem><ext>`; extension derived from `ObjectFormatKind × objectType` via `TargetSpec::outputExtension`. Plan 06 will eventually own the artifact-profile-driven scheme (D-LK10-3).
- `compileDirectory` recursively walks the directory, filters by the language schema's declared `fileExtensions`, sorts deterministically, delegates to `compileFiles`. (Plan 00 §4.1.3's `InputResolver` class is anchored at D-LK10-1 for LK10 cycle 3.)
- Returns 0 on success; 1 on any tier failure with diagnostics drained to stderr.
- Cycle 2 acceptance is WIRING (output directory creation proves the pipeline routed through every tier); byte-on-disk e2e auto-tightens when plan 12 ML7 cycle 2 + plan 13 AS load/store/ret-variant cycles close (D-LK10-2).

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

### 4.3 `dss-config/` — DSS Configuration Files (JSON)

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

| Target spec | OS | Architecture | Object format | Status |
|---|---|---|---|---|
| `x86_64:elf64-x86_64-linux` / `-exec` | Linux | x86_64 | ELF (.o / executable) | ✅ shipped (LK1 + LK1 cycle 2 + LK6) |
| `x86_64:pe64-x86_64-windows` / `-exec` | Windows | x86_64 | PE/COFF (.obj / .exe) | ✅ shipped (LK2 + LK2 cycle 2 + LK6 cycle 2a) |
| `x86_64:macho64-x86_64-darwin` / `-exec` | macOS | x86_64 | Mach-O (.o / executable) | ✅ shipped (LK3 + LK3 cycle 2 + LK6 cycle 2c) |
| `arm64:*` | Linux / macOS / iOS / Android | ARM64 | ELF / Mach-O | ⏳ first non-x86_64 cycle anchored at D-LK6-1 (assembler ✅ ARM64 already shipped at AS3) |
| `*:wasm32-v1` | Web | WASM | WebAssembly (.wasm) | ⏳ skeleton walker shipped (LK8); section emitters land in plan 18 |
| `*:spirv-1.6` | GPU shader | SPIR-V | SPIR-V module (.spv) | ⏳ skeleton walker shipped (LK9); instruction stream lands in plan 17 |

**Historical note (2026-05-30):** the v1 sketch listed `TargetWindowsX86_64` and sibling C++ classes; that hand-rolled `src/gen/` was retired at LK10 cycle 2 (config-driven `*.target.json` + `*.format.json` walkers superseded it). The `TargetConfig`/`TargetBase` factory abstraction never shipped — `TargetSchema::loadShipped(name)` + `ObjectFormatSchema::loadShipped(name)` are the substrate, called directly by the driver.

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
| 7.6 | ⏳ **new (rev 2)** — sub-plan [`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) | `substrate-prep` | Generalize arena + ship type lattice | compilation-unit-model | SP1 generalize `Tree`'s arena + `NodeAttribute<T>` so HIR/MIR/LIR reuse the same substrate. SP2 ship core type lattice + per-language extension registry. Substrate-tier (5-agent review). **v1 must deliver:** zero behavior delta on existing Tree code; the lattice is in place before semantic phase opens. |
| 8  | ⏳ pending | `analysis-semantic` | Implement semantic analysis | substrate-prep | Symbol table + scope resolution + type checking + const-correctness + typedef resolution. **Consumes `CompilationUnit`** (from phase 7.5) — symbol table spans every Tree in the CU + cross-file refs are pre-resolved. Populates `NodeAttribute<TypeId>` / `NodeAttribute<SymbolId>` over each CST **against the lattice from 7.6**. New `S_*` diagnostic namespace. Production-readiness §2 (G-201..G-212). |
| 8.5 | ✅ **COMPLETE (rev 2 — HR1–HR11 done)** — sub-plan [`09-hir-plan`](./09-hir-plan%20-%20ok.md) | `hir` | High-level IR | analysis-semantic | The pivot layer. Language-neutral, structured, typed HIR with FFI / shader / transpile attribute side-tables; CST→HIR lowering per shipped language. Eleven PRs (HR1–HR11). HR1 ✅ 2026-05-26 (`feature/hir-1` commit `406d5c7` — arena + node shapes + walker + ids + extension registry) + HR2 ✅ 2026-05-26 (typed expressions + operator registry + verifier) + HR3 ✅ 2026-05-26 (structured CF + break/continue & per-kind-arity verifier rules) + HR4 ✅ 2026-05-26 (declarations + extern surface + FfiMetadata side-table) + HR5 ✅ 2026-05-26 (attribute system + side-tables + `hir_attrs.hpp` catalog; verifier emits real diagnostic spans via optional `HirSourceMap`) + HR6 ✅ 2026-05-27 (full verifier — block dead-code / return completeness / Call-arg-vs-FnSig / intrinsic-registered / shader-restriction subverifier; `HirIntrinsicRegistry`; optional `TypeInterner` injection) + HR7 ✅ 2026-05-27 (round-trippable `.dsshir` text format — `emitHir`/`parseHir`, inline structural types, verify-on-load) + HR8 ✅ 2026-05-27 (config-driven CST→HIR lowering engine + `hirLowering` schema facet, proven on c-subset; per-expression type inference + literal-value pool; verify-on-load) + HR9 ✅ 2026-05-27..28 (`toy` enriched into a typed language + generic lowering test; arrays un-deferred end-to-end via a config-driven declarator-suffix descriptor + semantic-time constant-length eval; + gap-closure: char/string VALUES, SeqExpr, pointers, ternary) + HR10 ✅ 2026-05-27..28 (tsql-subset lowering — role-explicit SQL extension nodes via a generic `childGathering` config vocabulary + `ChildLower` enum, flat-expr lowering, coalesced/doubled-delimiter strings, `NULL`/relational-name extensions, `ReferenceRule.hardParents`; same language-agnostic engine); **HR11 ✅ done 2026-05-28. Plan 09 (HIR) COMPLETE.** **Single largest substrate addition in v1.** Blocks every downstream sink (transpile / MIR / SPIR-V / WASM). |
| 9  | ✅ **COMPLETE (ML1–ML8 done 2026-05-27..29)** — sub-plan [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) | `mir-lir` | Mid-level + low-level IR | hir | MIR (SSA over CFG + structured-CF markers; replaces single-IR G-301) and LIR (per-target ISA). Eight PRs (ML1–ML8). **All eight closed end-to-end** on `feature/mir-lir`: ML1 MIR skeleton + ML2 HIR→MIR (6 cycles incl. Cast emission) + ML3 MirVerifier (7 rule families, `I_*` 0xA00x) + ML4 `.dssir` text round-trip; ML5 JSON-configured targets + register-file + calling conventions + MIR→LIR isel cycle 3 series + `LirVerifier`; ML6 liveness + linear-scan + variant migration + R_* family + rewrite pass + `verifyLirPostRegalloc`; ML7 cycle 1 callconv + stack frame (`5362766`); ML8 cycle 1 `.dsslir` EMITTER (`040d496`) + cycle 2 PARSER + verify-on-load + round-trip + schema version (`9622b38`). Remaining open: ML7 cycle 2 (ARM64 stackPointer + ABI goldens) — anchored as legitimate next-cycle work. 100/100 ctest. |
| 9.5 | ⏳ **new (rev 2)** — sub-plan [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) | `ffi` | FFI binary readers + extern ingestion | hir | Hermetic ELF / PE / Mach-O / ar readers; C-header mode parser; ABI catalog; name mangling; HIR `ExternFunction`/`ExternGlobal` population. Six v1 PRs (FF1–FF6). **v1-blocking** — without it, no libc, no useful binary. |
| 10.5 | ✅ **AS1–AS6 closed 2026-05-29** — sub-plan [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) | `assembler` | In-tree machine-code encoder | mir-lir | x86_64 + ARM64 encoding via shape-keyed walkers (`x86-variable` + `fixed32`); per-(arch×format) relocation taxonomy unifier (target-side at plan 13 §2.6 + format-side at plan 14 LK4); round-trip oracle disassembler (`disasm.{hpp,cpp}` + per-shape inverse walkers); SourceMapEntry stamping at dispatch level. Six PRs (AS1–AS6) all landed on `feature/as-1`. |
| 11 | ⏳ **in flight** — sub-plan [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) | `linker` | In-tree linker + JSON-configured object formats | assembler, ffi, **artifactProfile** | **Hermetic** (replaces system-linker integration). **Config-driven object formats** (rev 3): `src/dss-config/object-formats/*.format.json` for ELF / PE / Mach-O / WASM / SPIR-V + ONE format-blind engine + TLS lowering + dynamic imports. **LK4 substrate + LK1–LK3 writers + LK1 ET_EXEC + LK6 c1 reloc-apply ✅ landed 2026-05-29..30** (`object_format_schema.*` + `linker.*` + per-format `elf`/`pe`/`macho` walkers; `.o`/`.obj` + executable images byte-validated in-memory; `K_*` diag at 0x8xxx). Ten PRs (LK1–LK10); remaining LK5 (TLS) / LK6 c2 (dynamic+extern) / LK7 (codesign) / **LK10 (hermetic on-disk file)** ⏳; LK8 WASM-skeleton + LK9 SPIR-V-skeleton for v1.x. |
| 11.4 | ⏳ **planned — sub-plan [`22-optimizer-plan`](./22-optimizer-plan%20-%20tbd.md)** | `gen-optimizer` | Multi-tier config-driven optimizer (HIR-transpile / MIR / LIR / per-target) — **source/target/linker-agnostic** | mir-lir; **gated after step 11 LK10 first emits a runnable executable file** (not yet — in-memory `LinkedImage::bytes` only today) | **v1 mandatory:** const fold + DCE + copy prop + dominator-tree + liveness (OPT1–OPT2). OPT3–OPT9 (redundancy / CF / LIR-peephole / coalesce / loops / inline / schedule / vectorize) + OPT10 autotuner = roadmap. Heuristics-as-data from PR1; `X_*` diag at 0x2xxx. Production-readiness §4. |
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
- ~~**G-301 IR design** (was: "default SSA over CFG").~~ **Resolved rev 2 2026-05-23:** **three IR layers** — HIR ([`09-hir-plan`](./09-hir-plan%20-%20ok.md)) → MIR (SSA over CFG with structured-CF markers; [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md)) → LIR (per-target ISA; same plan). Single IR cannot serve both transpilation and binary codegen cleanly.
- ~~**Linker strategy** (was: "system linker for v1, built-in post-v1").~~ **Resolved rev 2 2026-05-23:** **in-tree linker from day one** ([`14-linker-plan`](./14-linker-plan%20-%20tbd.md)). The hermetic-compiler invariant rules out shelling to `ld`/`link.exe`/`ld64`/`lld`/`wasm-ld`. Side-effect: Apple-host-free local dev.
- ~~**Type system** (was: "TypeKind enum + TypeId interner").~~ **Resolved rev 2 2026-05-23:** **core lattice + per-language extensions** ([`08.5-substrate-prep-plan`](./08.5-substrate-prep-plan%20-%20ok.md) §2.2). Each `.lang.json` registers extension type-kinds via `typeExtensions[]`.
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
