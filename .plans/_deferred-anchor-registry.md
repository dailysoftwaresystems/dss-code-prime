# Deferred-Anchor Registry

> The canonical source of truth for every `D-*` anchor cited in `src/` but
> deferred to a future cycle. The leading underscore sorts this file before
> the numbered plans in directory listings — call this out as the registry,
> not a plan. Per-plan §3.1 rows are still the preferred home for anchors
> whose feature area maps cleanly onto a plan; this file catches the orphans
> + cross-cutting anchors that don't belong to any single plan.
>
> **CI/pre-commit guard** (`tools/check-anchor-registry.{sh,ps1}`): every
> `D-*` identifier appearing in `src/` MUST resolve to a row in this file
> OR a citation in any `.plans/*.md`. Pre-fix this recurred TWICE
> (memory: "the same gap silent-failure caught for MULTI-PAGE-GOT").
> The guard makes the system enforce the discipline — same shape as the
> agnosticism veto + the `CallConv` static_assert.
>
> **Schema (one row per anchor)**:
> ```
> | Anchor | Trigger | Closing work | Cross-refs |
> ```
>
> When an anchor closes, replace its row's status with `✅ CLOSED <date>`
> and cite the commit/SymbolId. Don't delete the row — the audit trail is
> load-bearing for future regressions.

---

## Anchor Index

| Anchor | Trigger | Closing work | Cross-refs |
|---|---|---|---|
| `D-AS4-DISP8-ENCODING` | First non-test consumer needs the shorter `disp8` form (e.g. `lea [rsp + small_imm]` where the offset fits in a signed byte). | Add a `Disp8` variant alongside `Disp32` in `EncodingSlotKind`; teach the encoder to pick the narrower form when the immediate fits. | plan 13 §2.4 (encoding shapes) |
| `D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD` | Step 13.5 cycle 1 silent-failure HIGH #3 — `(void)encodeInst` laundering partial bytes + patch loop's `continue` past failures. | ✅ **CLOSED 2026-06-03** (commit `1ad61cd`) — funcEncodeOk + truncate partial bytes + drop entire function + `A_FunctionEncodeAborted=0x1006`. | plan 13; D-ASM-ENCODE-FAILURE-FUNCTION-ROLLBACK |
| `D-CAP-MARKER-COMPILE-DIR-PIN` | First time a multi-file `--directory` compile silently picks the wrong target subset. | Add a pin in the program test suite that asserts `compileDirectory`'s file enumeration produces the expected set; surface dropped files via a new `D_*` driver code. | plan 14 §LK10 driver |
| `D-CAP-MARKER-MULTI-TARGET-E2E-PIN` | First multi-target build (e.g. `--target x86_64:pe ... --target x86_64:elf`) lands without verifying each target's binary independently. | Examples_runner extension: per-target expectedStdout / exitCode columns; multi-target verifier reads each spec's row. | plan 14 §LK10 driver; example expected.json schema |
| `D-COMPILE-ONE-TARGET-NO-LEAK` | A future bug in `compileOneTarget` leaks state into the next compile (e.g. reused arenas, stale diagnostics). | Lifetime contract tests; arena ownership invariant pinned via `static_assert` on move-only + a per-call reset hook. | plan 14 §LK10 driver |
| `D-CSUBSET-BINOP-RIGHT-CLOBBER` | ✅ **CLOSED 2026-06-02** (commit `2a6e9fe`) via regalloc 2-addr-aware exclusion-of-operand-physregs from result-allocation. | (closed) | plan 12 ML6 regalloc; MEMORY.md project_d_csubset_binop_right_clobber_2026_06_02 |
| `D-CSUBSET-DEREF-LOADSTORE-SMOKE` | ✅ **CLOSED 2026-06-04** (cycle 10h) — first compile→run gate for Deref-as-rvalue (`*p`) AND Deref-as-lvalue (`*p = v`) under c-subset. Pre-10h only param-type pointer spelling (`int*`, `char*`) was corpus-exercised — the Deref operator itself had no source→.exe assertion. | New corpus fixtures `examples/c-subset/deref_load/` + `examples/c-subset/deref_store/` (one-arg helper `int read_through(int* p){return *p;}` and `int write_through(int* p){*p=42;return 0;}` paired with `int main()` materializing a local + `&x`). Two-fixture split for independent failure attribution: load-codegen bug surfaces to deref_load, store-codegen bug to deref_store. | plan 22 (alias arc capstone pre-req); D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT |
| `D-CSUBSET-LONG-PRIMITIVE` | ✅ **CLOSED 2026-06-04** (cycle 10h) — adds `long` keyword → I64 core to c-subset.lang.json. Pre-10h c-subset declared only `int`(I32), `char`(Char), `void`(Void); no non-char wider primitive existed, so `mirMayAlias` Rule 6 (distinct non-char primitives → No alias) could not be exercised on any c-subset corpus row. | Four 1-line additions to c-subset.lang.json (keyword token + typeBase alt + typeForRef alt + builtinTypes) + I32→I64 sext encoding row added to x86_64.target.json (movsxd r64, r/m32 = REX.W 0x63 /r). Smoke fixture `examples/c-subset/long_primitive_smoke/` pins end-to-end with `0 - 1` sentinel (post-fold discriminator: positive small witnesses can't distinguish sext from zext or full-width from low-32 ABI return). | plan 22 (alias arc capstone pre-req); pairs with D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT; D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH |
| `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH` | First time a c-subset corpus row needs `long` to bind to I32 (LLP64 / Win64 ABI) instead of I64 (LP64 / SysV ABI). Today `c-subset.lang.json` binds `long → I64` unconditionally — that's correct for LP64 Linux/macOS, silently wrong for Win64 LLP64 (Windows `long`=I32). Today's corpus runs Win64 only because `pe64-x86_64-windows-exec` is the sole `targets[*].spec`, but the `long`→I64 binding bakes a data-model decision into the source-language config in a way that violates the agnosticism rule (lang config should not assume ABI). | Extend the `SemanticConfig` schema with a `dataModel` axis (e.g., `"ILP32"` / `"LP64"` / `"LLP64"`) that `builtinTypes[*].core` can vary on; OR add an ABI-conditioning layer in the HIR→MIR lowering that overrides core types per active target. Closes the silent-mismatch when the same c-subset source compiles to two different ABIs. Pairs with the multi-target capstone (D-CAP-MARKER-MULTI-TARGET-E2E-PIN). | plan 22 (alias arc); pairs with D-CSUBSET-LONG-PRIMITIVE (which made this decision provisional) |
| `D-OPT-IDENTITYREBUILD-CALLER-WRAP-REQUIRED` | ✅ **CLOSED 2026-06-04** (cycle 10h post-fold) — `identityRebuild` test helper signature refactored from `void identityRebuild(Mir const& src, Mir& out)` to `[[nodiscard]] bool identityRebuild(...)`. The void+ASSERT_NO_FATAL_FAILURE shape required caller discipline; a caller writing `identityRebuild(src, dst);` without the wrap would silently allow tests to proceed with a half-built `dst`. `[[nodiscard]]` triggers an unused-result warning at the missing-check call site, converting the silent-failure trap into a compile-time gate. | Refactor + ADD_FAILURE on the carve-out arm (vs the prior ASSERT_EQ which required void return); both existing test bodies updated to `ASSERT_TRUE(identityRebuild(...))`. | tests/opt/test_mir_rebuild_helper.cpp; same compile-time-enforcement pattern as `[[nodiscard]] Result<T>` patterns elsewhere in the codebase |
| `D-TARGET-ENCODING-WIDTH-GUARD` | First time a target needs distinct encodings for different operand WIDTHS (not just kinds like reg/imm/mem) — e.g., sext I8→I64 vs I32→I64 are different x86 opcodes (`0F BE` vs `63`). Today's variant-guard only supports `operandKinds` discrimination; same trap exists for the existing zext (BYTE→r64 single-variant) and trunc (no encoding) mnemonics. | Extend the variant `guard` schema with an `operandWidths` array discriminator + teach the encoder to dispatch on the source operand's width-of-record. Then add the missing sext variants (I8→I16, I8→I32, I8→I64, I16→I32, I16→I64) + the trunc encoding (mov r32, r32 zeroes upper 32). Pre-fold the same trap masks any I8/I16 widening / narrowing as silent miscompiles. | plan 13 §2.4 (encoding shapes); pairs with D-CSUBSET-LONG-PRIMITIVE (which closed only the I32→I64 variant — others anchored here) |
| `D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT` | First compile→run binary differential demonstrating the strict-aliasing arm of cycles 10a-10g (`strictAliasingOnDistinctTypes` + Rule 6 distinct-primitive `mirMayAlias`). Re-scoped from the original `D-OPT-ALIAS-ARC-CORPUS-CAPSTONE` after 10h discovery: c-subset is C → `charTypesAliasAll=true` is correct → Rule 5 (char-exception) fires first on any `int*`/`char*` pair → no precision win observable on that path under default config. Strict-arm via two non-char distinct primitives is the under-pinned, agnosticism-clean path. | Cycle 10i deliverable: requires `D-CSUBSET-LONG-PRIMITIVE` (adds `long`→I64 to c-subset.lang.json) as pre-req, then a two-armed fixture `examples/c-subset/strict_alias_cse_capstone/` with `int f(int* pI, long* pL){int a=*pI;*pL=7;return a+*pI;}` — primary effectiveness gate: MIR Load-count decrease on the second `*pI` under CSE-enabled arm vs baseline; complementary check: `passMutationCount[Cse] >= 1`. | plan 22 OPT5 (CSE alias arc); pairs with D-CSUBSET-DEREF-LOADSTORE-SMOKE pre-req |
| `D-FF1-DISPATCH-TABLE` | First time the FFI binary-reader dispatch is hot in a profile (i.e., reading >100 .o per CU). | Replace the if-else chain with a compile-time dispatch table indexed by `ObjectFormatKind`. | plan 11 FF1 |
| `D-FF1-MACHO-32` | First i386 / 32-bit ARM corpus needs MachO Mach-O 32-bit reader. | Add the `mach_header` (vs `mach_header_64`) shape variant + dispatch on cputype. | plan 11 FF1 |
| `D-FF1-MACHO-FAT` | First Apple "fat binary" (multiple slices for ARM64 + x86_64) needs slice picking. | Parse the `fat_arch[]` index + select the slice matching the current target. | plan 11 FF1 |
| `D-FF1-MACHO-SECT-KIND` | First MachO reader use case needs section-kind classification beyond text/data (e.g. coalesced sections, dyld stubs). | Map `section_64.flags` to a closed-enum `MachOSectionKind`. | plan 11 FF1 |
| `D-FF1-MACHO-VARIANT-KIND` | First MachO reader needs the Variant-class section semantics. | Closed-enum `MachOVariantKind`; per-variant emitter. | plan 11 FF1 |
| `D-FF1-MACHO-WEAK-DEF` | First MachO module declares a weak definition (`__attribute__((weak))`). | `N_WEAK_DEF` bit + symbol-table emit; linker side dedups. | plan 11 FF1 |
| `D-FF1-PARTIAL-CORRUPTION-LOUD` | First time a partially-truncated input `.o` (network read interrupted) reaches the reader. | Pre-flight size + checksum check; fail-loud `F_FfiInputTruncated` before any per-section walker runs. | plan 11 FF1 |
| `D-FF1-PARTIAL-CORRUPTION-MACHO` | Same shape but Mach-O specific (a truncated `LC_*` command). | Per-command bounds check inside the Mach-O walker. | plan 11 FF1 |
| `D-FF1-PE-OBJECT-EXPORTS` | First PE `.lib` archive's member `.obj` has exports (symbols destined for the export table). | Read `IMAGE_EXPORT_DIRECTORY`; thread exports through to the symbol table. | plan 11 FF1 |
| `D-FF2-MSG-JARGON` | First time a user-facing FF2 diagnostic's jargon is flagged by a non-DSS-fluent dev. | Sweep FF2 diagnostic strings; replace internal-implementation jargon with end-user terms. | plan 11 FF2 |
| `D-FF2-UNSUPP-FULL-SWEEP` | First time a deferred FF2 declaration shape (e.g. nested type, opaque enum) lands in real corpus. | Full SWEEP across the FF2 declaration-shape switch; promote every deferred arm to a real handler OR a per-arm fail-loud code. | plan 11 FF2 |
| `D-FF2-UNSUPP-INFO-WAE-ASYMMETRY` | First user observes the `Info`-vs-`Warning-as-Error` asymmetry in unsupported-decl diagnostics. | Unify the severity policy under one knob; document. | plan 11 FF2 |
| `D-FF5-EXTERNDECLREF-VALIDATE` | First time an `ExternDeclRef`'s symbol-name passes synthesis but doesn't resolve at link time. | Add a pre-synthesis validation that walks every `ExternDeclRef` against the FFI registry. | plan 11 FF5 |
| `D-LIR-SETCC-MOVZX-COMPOUND` | ✅ **CLOSED 2026-06-03** (commit `1ad61cd`) — superseded by D-LIR-SETCC-WIDTH-CONTRACT closure (zext encoding + lowerICmp emits cmp→setcc-b8→zext-result). | (closed) | plan 12 ML5; D-LIR-SETCC-WIDTH-CONTRACT |
| `D-LK10-ENTRY-MAIN-IMPLICIT-RETURN` | A future c-subset function decl uses implicit-return-0 semantics for a non-`main` function name. | Split `implicitReturnZeroForFunctionNames` from a separate `entryFunctionNames` list per architect Q1 in cycle-2 fold. | plan 14 §LK10 entry; D-CSUBSET-ENTRY-NAME-SPLIT |
| `D-LK10-ENTRY-RESOLVE-ENTRY-FN-IDX` | First time the entry-fn resolution shape needs a 3-agent FOLD-NOW (already happened at Slice C). | ✅ **CLOSED 2026-06-02** (commit `1a0b414`) — shared `resolveEntryFnIdx` helper; 5 walker sites collapsed to single-line calls. | plan 14 §LK10 entry |
| `D-LK4-RODATA-PRODUCER-EXOTIC` | First exotic literal type (F16 / F128 / bfloat / decimal) needs rodata emission. | Encoder arm per type's bit width + endianness. | plan 14 LK4 rodata producer |
| `D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY` | First non-string array literal at file scope (e.g. `int arr[3] = {1,2,3};`). | Encoder arm for non-string aggregate constants. | plan 14 LK4 |
| `D-LK4-RODATA-PRODUCER-NONSTRING-GLOBAL` | First non-string global initializer beyond the existing string-literal path. | Generalized constant-encoding walker. | plan 14 LK4 |
| `D-LK4-RODATA-WALKER-RELOC-BASE` | First non-PE format needs the rodata-walker reloc-base extension. | Per-format `rodataRelocBase` lookup driven by format schema. | plan 14 LK4 |
| `D-LK6-14-INTEGRATION-PAYLOAD` | ✅ **CLOSED 2026-06-01** (cited at `parse_diagnostic.hpp:698-699` + `macho.cpp:1194-1197` as the closed pair to D-LK6-14-INTEGRATION-GOT-SLOTS). Chained-fixups payload-encoder unit-test substrate. | (closed) | plan 14 LK6 cycle 14; companion D-LK6-14-INTEGRATION-GOT-SLOTS |
| `D-LK6-14-NAME-OFFSET-OVERFLOW` | First Mach-O chained-fixups module whose import-name table exceeds u32 range. | Promote `nameOffset` to u64; or paginate the name table. | plan 14 LK6 cycle 14 |
| `D-MERGE-CONTEXT-PREFIX-CLOBBER` | First time `mergeWithTargetContext` clobbers a prefix from a sibling import. | Per-prefix-source side-table (the `D-MERGE-CONTEXT-PREFIX-SIDE-TABLE` companion). | plan 11 FF / merge policy |
| `D-MERGE-CONTEXT-PREFIX-SIDE-TABLE` | Companion to D-MERGE-CONTEXT-PREFIX-CLOBBER — same closure cycle. | (paired with above) | plan 11 FF / merge |
| `D-MERGE-DEDUP-PREFIX-COLLISION` | First time two distinct prefix sources dedup to the same key but produce different values. | Fail-loud on collision with both source citations. | plan 11 FF / merge |
| `D-MERGE-POLICY-IDEMPOTENCY` | First time the merge policy needs idempotency (re-running the merge on already-merged input changes the output). | Define an `isMerged()` predicate + skip-when-already-merged guard. | plan 11 FF / merge |
| `D-MERGE-SCRATCH-FRESH` | First time the merge's scratch context is silently reused across calls. | Reset / fresh-construct the scratch on each merge entry. | plan 11 FF / merge |

---

## Allowlist (code-internal pins, NOT deferred work)

Anchor-shaped strings in `src/` that are **not** deferred-work markers and
should be **ignored** by the CI guard. Add a row when an in-code constant
or diagnostic-message identifier matches the `D-*` regex but isn't a
deferred-work anchor.

| Pattern | Reason |
|---|---|
| (none today) | Reserved for future internal pins. |
