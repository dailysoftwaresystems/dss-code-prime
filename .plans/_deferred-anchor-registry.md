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
