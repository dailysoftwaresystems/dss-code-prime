# PR Scope — every open row and every plan, placed (30)

> **Asked by the operator 2026-10-22:** *"make a scope for this PR and the next PR to address all of our anchors
> (and eventually implement the full debug info in our compiler)"*, then *"put everything in the plans please"*.
>
> This plan owns WHICH open row lands in WHICH PR, in what order, and where each plan's remaining work goes.
> **Every open row is placed in §8, each exactly once.** The production registry stays the authority for a row's
> content; this plan only places it. `dssharness read-anchors --pending --open` re-derives every count.
>
> Every figure is MEASURED at the commit named in §0, unless it is labelled INFERRED. Re-derive; never quote.
> The decisions in §4 are the orchestrator's, under the operator's order of 2026-09-21 (*"you do everything"*),
> and each one is veto-able.

## 0. Where we are (MEASURED 2026-10-28, main `c5be892d` = checkpoint 33, plus fold H4b = checkpoint 34)

**679 open rows.**

| band | OPEN | DISCLOSED | GATED | total |
|---|---|---|---|---|
| P0 | 2 | 22 | — | 24 |
| P1 | 44 | 48 | 1 | 93 |
| P2 | 140 | 54 | 22 | 216 |
| P3 | 221 | 7 | 103 | 331 |
| P4 | 8 | — | 3 | 11 |
| P5 | 4 | — | — | 4 |

**Where they go** (§8 lists every id):

| PR | rows | what |
|---|---|---|
| **PR #59** (current) | 93 | the four lanes' rows, every remaining P0, the rows ruled into folds 3d/3e, L2b and H5, two fixed rows awaiting their arms |
| **the next PR** | 469 | every remaining actionable row, in three rounds; plus full debug information (§3) |
| **the capability PR** | 21 | plan 28's boot half, plan 09.5's tier skeleton with its C harmlessness proof, plan 16's codesign |
| **the wasm32 PR** | 1 | plan 18, the first ILP32 target |
| **the C++ PR** | 2 | the second source language and the second embedding language |
| **opened by a trigger** | 93 | plan 24 (50), plan 28's driver half (19), plan 09.5's lattices (10) and services (3), plan 17 GPU (10), plan 27's gui profile (1) |

**Throughput, MEASURED:**
- Rows closed per ISO week, from the done registry's close dates (read 2026-10-22):
  - weeks 33–36: 161, 147, 197, 127;
  - weeks 37–41: 40, 44, 163, 54, 32;
  - week 43 so far: 13.
- Since the cycle base `71648598` (2026-09-30), `check-anchor-balance` counts **28 closed and 87 opened (3 created, 84 disclosed): counted −25, REAL net +59** (2026-10-28, at checkpoint 34's tree). The registry grew, mostly by disclosed rows: debt this round found, not debt it created. Rows born closed are not counted.
- (read 2026-10-22) **24 open rows (8 of them P0) are already FIXED** and wait only for their mutation arms to read `passed` on main.

## 1. PR #59 — the exit scope

The PR ends as the operator ordered (2026-10-08):
1. every lane re-reviewed clean and every arm passed;
2. the 8-run gate ({Debug, Release} × four legs) plus ASan;
3. the sqlite testfixture recompiled, plus `veryquick`;
4. a final review, the handoff rewritten, commit, push;
5. the orchestrator adds the `Run Pipes` label and watches CI to green.

The merge is the operator's.

### 1.1 Folds, in order

`L2a` ✔ (checkpoint 33) `→ H4b` ✔ (checkpoint 34, folded ahead of f26b: it was ready and depends on nothing of `cs`'s) `→ f26b → L2a2 → H5 → 3d → f26c → L2b → 3e`, then the added work of §1.2 on the lanes that free first. L2a2 is lane `lm`'s answer to the review of L2a (its typedef finding goes to `cs`'s f26c, with the name-before-declaration P0).

| lane | rows (§8 A.1) | what it is |
|---|---|---|
| `lm` | 34 | constant evaluation, VLA `sizeof`, statement expressions, try/filter MIR, HIR brace placement; then the try/except P0 pair and the `#if` truth P0; L2a's new rows, open until main's sweep of their arms (L2a2), and arithmetic on a pointer to a VLA (L2b) |
| `cs` | 24 | attributes, weak/select-any, declarations and type rules, visibility |
| `xa` | 25 | object readers and writers, COMDAT selections, unwind and handlers, initializer tables, relocatable `__eh_frame`, TLS; the all-object link's order and its client object's imports (3d), a library datum, data symbols and a shared library's initializers (3e) |
| `hm` | 6 | the PE platform runtime (H5: the stack probe and the /GS family), the census row it opened, P68's two audit rows; the filesystem substrate's row until main's sweep of its arms (H4b folded) |
| first lane to free | 2 | the shipped-constants P0, and the SysV memory-class ABI P0 |
| round-close sweep | 2 | fixed rows whose arms have not been read |

### 1.2 Added to PR #59 by this plan

- **Foreign debug info is carried** (operator ruling 2026-10-22: *"this PR is already a monster anyway"*). Its row is born in fold 3d. The work:
  - ELF: concatenate and relocate the non-alloc sections.
  - Mach-O: write a debug map pointing at the input objects.
  - PE: keep the DWARF sections in the image, AND write a PDB (plus the debug directory's RSDS record) from CodeView `.debug$S` / `.debug$T`.
  - It goes to the first lane that frees, as its own fold. Its new files land first; the writer-integration hunks follow fold 3d.
- **Every remaining P0**, each measured for size first: the six rows in §8 A.1 outside the four lanes' own lists.
- **The fixed rows** close on the round-close sweep of every arm.

### 1.3 Estimate

**About 7–9 more days** (INFERRED):
- The critical path is lane `xa`'s 3d (2.5–3 days), then 3e (about 1 day).
- The debug-info carry and the P0 additions run in parallel on the lanes that free first.
- The exit gate takes about 1 day.

## 2. The next PR — every remaining actionable row, and full debug information

### 2.1 Rules (the standing orders, restated as this PR's acceptance)

- **The REAL net falls every round.** Closed outnumbers opened, born-closed rows counted on both sides. A rise is a HARD STOP.
- A defect a lane meets is FIXED in that wave. A new disclosed row needs a MEASURED size and the orchestrator's ruling, and neither cycle size nor a PR exit is a reason to roll it (operator, 2026-10-08 and 2026-10-22).
- A gated row whose trigger waits on a component we build is not gated: build the prerequisite. Each gated row is re-verdicted at its round's start.
- The OPT7 / inlining rows stay behind the skill's standing hard stop: the operator is asked when lane C reaches them.

### 2.2 Lanes and rounds (§8 A.2 lists every id)

| lane | areas | R1 (P0–P1) | R2 (P2) | R3 (P3–P5) | total |
|---|---|---|---|---|---|
| A | C front end + HIR | 30 | 79 | 15 | 124 |
| B | linker / object formats + assembler / targets | 11 | 57 | 79 | 147 |
| C | MIR / LIR / optimizer + FFI / config + driver / diagnostics / substrate + harness / docs | 9 | 59 | 130 | 198 |
| D | full debug information (§3) | its own rows, born in its design | | | |

Each fold is a pushed checkpoint. Each round ends with the 8-run gate, the arm sweep and a CI read.

### 2.3 Estimate

**About 6–10 weeks** (INFERRED):
- At the August–September pace (130–190 closes a week, measured weeks 33–36 and 39), the 469 rows take about 4 weeks with three lanes.
- At the October pace (30–55 a week, on rows that each needed three references measured), they take about 12 weeks.
- R3's rows are mostly small, so the expectation sits between those.
- Lane D runs alongside (§3.3).

## 3. Full debug information in the compiler ([`15`](./15-debug-info-plan%20-%20tbd.md))

### 3.1 State, 2026-10-22

- READ (the writers' code): unwind information already ships: `.eh_frame` on ELF, `.pdata` / `.xdata` on PE, and the Mach-O `__eh_frame`. Mach-O `__unwind_info` is in fold 3d.
- MEASURED (lane `xa`, the checkpoint-32 compiler): the compiler emits no debug information. `-g`, `-g3`, `-gdwarf-5`, `--debug` and `/Zi` are refused by name ("unknown flag"), and no output of any configuration holds a debug section or a PDB.
- Foreign debug information is carried by PR #59 (§1.2).

### 3.2 Phases (plan 15's DB1–DB12, made "full": locals and optimized code included)

| phase | content | done when |
|---|---|---|
| D1 | the source-position chain, CST → HIR → MIR → LIR → bytes (plan 15 §2.2); `-g` accepted, config-driven; the MIR and LIR verifiers refuse an instruction with no span unless it is marked synthetic | every corpus program carries a complete map |
| D2 | DWARF 5 line tables plus compile-unit and subprogram DIEs, for ELF and Mach-O | `llvm-dwarfdump --verify` is clean; gdb and lldb stop on every line of the corpus smoke |
| D3 | types: the core lattice → DIEs once, with a language's extension types declared in its `.lang.json` `debugInfo` facet | every corpus type prints in gdb and lldb |
| D4 | variables: globals, parameters and locals; frame-base locations; `.debug_loclists` for optimized code; lexical blocks | `print` of every local at every line of the smoke, at `--config=release` too |
| D5 | PDB: MSF, PDB info, TPI / IPI, DBI with module streams and `DEBUG_S_LINES`, publics and globals; the image's RSDS debug directory; locals through CodeView ranges | `llvm-pdbutil` is clean; cdb steps and prints the smoke |
| D6 | oracles and determinism: rebuilt output byte-identical; scripted debugger runs on every leg | the debugger smoke is part of the gate |
| D7 | own and foreign debug information merged through `-r` and into images, one engine with PR #59's carry | a program mixing DSS and gcc / cl objects debugs in all three |

### 3.3 Estimate and stance

- **About 6–8 weeks for one lane** (INFERRED). The PDB is the largest piece: plan 15 sizes LLVM's at about 10k lines.
- Decided (veto-able):
  - DWARF 5, and a PDB sidecar (plan 15 §4).
  - "Full" puts locals and optimized-code locations in scope; plan 15's "post-v1" for D4 is superseded.
  - Where Mach-O keeps DWARF is decided by measurement at D2's design audit: the image's `__DWARF` (plan 15 §4 Q3) or ld64's debug map. The criterion is that lldb on the Mac leg debugs both a DSS-only program and a mixed one.

## 4. Decisions (the seven rows that name one; decided, veto-able)

| row | the decision | DECIDED | why |
|---|---|---|---|
| [[D-CSUBSET-THREAD-LOCAL-MACHO-OVERALIGN]] | measure dyld's TLV block alignment on real Apple hardware? | **yes**, next PR R1, lane B | the Mac leg IS real Apple hardware; a measurement, not a product choice |
| [[D-CONFIG-GRAMMAR-ISA-AND-IDENTIFIERCLASS-BELONG-IN-THE-LANGUAGE-BLOCK]] | cut a `.lang.json` schema version bump? | **yes, once**, next PR R2, lane A | bundle every breaking `.lang.json` change R1–R2 finds into one bump |
| [[D-CRYPTO-SHA256-HOME-CONVERGENCE]] | start plan 16's CMS / DER / x509 substrate? | **yes**, the capability PR | after debug info, so a signature covers the debug sections it signs |
| [[D-FFI-STDINT-PTR-WIDTH-ILP32]] | ship an ILP32 target? | **yes: wasm32**, the wasm32 PR | plan 18 already plans it and its linker substrate exists; i386 and armv7 wait for a consumer |
| [[D-DEPS-SOURCEMERGE-INHERITS-THE-CONSUMERS-COMPILATION-ENVIRONMENT]] | which second general-purpose source language? | **C++**, the C++ PR | the operator's 2026-08-24 order deferred only C++; plan 24's trigger and plan 27's dependencies both name it |
| [[D-CONFIG-INLINE-ASM-TEMPLATE-NUMBERING-HAS-NO-DECLARED-OWNER]] | which second embedding language? | **C++**, the C++ PR | C++ embeds the same GNU and MSVC asm forms; it is the second embedder by construction |
| [[D-AP2-COMPILATION-CONTEXT]] | an artifact profile beyond `(target:format)`? | **yes, `gui`**, when plan 27 opens | plan 27 is that profile's owner, and it depends on the C++ front end |

## 5. After the next PR — the PRs that follow, in order

1. **The capability PR** — work that needs no new language:
   - plan 28's **boot half**: the non-hosted exit mechanism, an empty `runtimeLibraries` table, the `boot` profile with its non-executability gate, and the payload binding (16 rows). Plan 28 §1.1 names these exact four facts;
   - plan 09.5's **tier skeleton and its C harmlessness proof**: C emits byte-identically with the tier present (4 rows; plan 09.5 §1.1 calls it buildable now);
   - plan 16, **codesign and publish** (1 row, plus the plan's CS1–CS7), then plan 26's publish pipeline goes live.
2. **The wasm32 PR** — plan 18 (WA1–WA10), the first ILP32 target (1 row). Plan 10's c → JS translation opens behind it if a Web target asks for it.
3. **The C++ PR** — a new numbered plan is written first (its design audited like any plan). Its exceptions open plan 21's runtime. It is the second language and the second embedder (2 rows), and it unblocks plan 27.
4. **Opened by a trigger, in this order**, each when its plan's own §1 trigger fires:
   - plan 27 GUI and its `gui` profile (1 row), behind the C++ front end;
   - plan 09.5's **reclamation lattices** (10 rows), with the first language that declares a reclamation lattice;
   - plan 24, **DSS Axis** (50 rows), after full C, the v2 gap, and one language per paradigm (C++, a managed one, a dynamic one, an ownership one), per its §1;
   - plan 28's **driver half** (19 rows), after plan 24 and one bus modelled end to end;
   - plan 09.5's **runtime services** (3 rows), each behind a consumer language with `runtime.enable`;
   - plan 17, **GPU** (9 rows), when plan 20's custom language begins;
   - plan 19, hardware HIR (no rows), when a VHDL or Verilog target is requested.

## 6. Every plan, and where its remaining work lands

| plan | state (its own status line) | where the rest lands |
|---|---|---|
| 00 master | the stepper | §0.1 points here for PR placement |
| 01–05, 08.5, 08.55, 08.6, 09, 12, 12.5, 24-iterative, 25 | done | — |
| 06 artifact profile | AP1–AP5 done | its open rows: next PR, lane C; the `gui` profile: plan 27 |
| 07 production readiness | opened, no arc run against it | its G-gates audited in the next PR's R3, lane C |
| 08 compilation unit | CU1–CU4 shipped | its 20 rows: next PR R3, lane C |
| 09.5 DSS-HIR | reserved, split trigger | skeleton: capability PR; lattices and services: §5 item 4 |
| 10 source translation | planned | behind the wasm32 PR, when a Web target asks |
| 11 FFI | mostly done | its rows: PR #59 and next PR, lane C |
| 13 assembler | closed | its rows: next PR, lane B |
| 14 linker | closed, runnable | its rows: PR #59 (xa) and next PR, lane B |
| 15 debug info | planned | foreign carry: PR #59; the compiler's own: next PR, lane D (§3) |
| 16 codesign / publish | planned | the capability PR |
| 17 GPU | planned | §5 item 4 |
| 18 WASM | planned | the wasm32 PR |
| 19 hardware HIR | reserved | §5 item 4 |
| 20 custom language | reserved | after v1 ships and the C++ validation is mature (its §1) |
| 21 runtime | reserved | opens with C++'s exceptions (the C++ PR) |
| 22 optimizer | in flight | its rows: next PR, lane C; OPT7 behind the hard stop |
| 23 full C | in progress | its rows: PR #59 (lm, cs) and next PR, lane A; it closes when lane A's rows close |
| 24 DSS Axis | reserved | §5 item 4 |
| 26 publish | scaffolded, inert | goes live after plan 16 (the capability PR) |
| 27 GUI | draft | after the C++ PR |
| 28 drivers / boot | reserved, split trigger | boot half: capability PR; driver half: §5 item 4 |
| 29 inline asm | in progress | its rows: next PR, lanes A and B |
| v2 gap catalog | catalog | its closure is part of plan 24's trigger |

## 7. Risks

| risk | measured or seen | mitigation |
|---|---|---|
| the registry grows faster than it closes | +46 counted since `71648598` | §2.1's net-falls rule, read at every fold |
| host memory and VPS disk with four lanes | commit exhausted once (2026-10-21); one arm64 build fits the VPS at a time | DssHarness admission; capped probes; a build cleaned after its finals |
| a usage limit cuts a lane mid-step | seen in earlier cycles | resume the same agent id; findings kept in files, not only in messages |
| reference-matching cost (each row measured on gcc, clang and MSVC) | the October pace | probes shared in the work tree; one census per question, not one per row |

## 8. Every open row, placed

Generated from the production registry at plan 30's measurement: 679 rows, each placed once.
A row closed since then still resolves (the done registry), and the next refresh drops it.

### A.1 PR #59 (current)

#### cs (f26b, f26c) — 24

- [[D-C-AN-ATTRIBUTE-AFTER-A-REFERRED-TAG-NAMES-ANOTHER-TAG-AND-ONE-IN-A-TYPE-NAME-IS-REFUSED]] (P0)
- [[D-C-PE-DECLSPEC-IS-ERASED-SO-ALIGN-AND-THREAD-STORAGE-ARE-SILENTLY-DROPPED]] (P0)
- [[D-C-WEAK-ON-AN-INFERRED-TYPE-OBJECT-IS-SILENTLY-DROPPED]] (P0)
- [[D-LK-WEAK-NAME-REFERENCE-BOUND-TO-THE-BODY-NOT-THE-NAME]] (P0)
- [[D-MIR-SYNTHESIZED-SYMBOL-MINTED-INSIDE-THE-NAME-TABLE]] (P0)
- [[D-C-A-CALLING-CONVENTION-ATTRIBUTE-NAMING-ANOTHER-CONVENTION-IS-REFUSED-NOT-EMITTED]] (P1)
- [[D-C-A-CONSTANTS-MEMBER-NAME-IS-RESOLVED-BY-A-SCOPE-WALK]] (P1)
- [[D-C-A-NON-VOID-FUNCTION-WHOSE-END-IS-REACHABLE-IS-REFUSED]] (P1)
- [[D-C-COMMON-AND-NOCOMMON-ATTRIBUTES-ARE-WARNED-AND-DROPPED]] (P1)
- [[D-C-GNU-VECTOR-SIZE-ATTRIBUTE-WARNED-AND-IGNORED]] (P1)
- [[D-C-SECTION-ATTRIBUTE-IS-WARNED-AND-DROPPED]] (P1)
- [[D-C-SIZEOF-OF-AN-ARRAY-OF-UNKNOWN-SIZE-IS-REFUSED-WHERE-MSVC-ACCEPTS-IT]] (P1)
- [[D-CSUBSET-ATTRIBUTE-ALIAS-TARGET-NO-SURFACE]] (P1)
- [[D-CSUBSET-ATTRIBUTE-TYPE-POSITION]] (P1)
- [[D-LIR-GUARDED-RANGE-DOES-NOT-COVER-BLOCKS-THE-LOWERING-CREATES]] (P1)
- [[D-LK-FOREIGN-OBJECT-THREAD-LOCAL-DEFINITION-REFUSED-AT-READ]] (P1)
- [[D-C-A-MEMBER-WITH-A-RUNTIME-BOUND-IS-READ-AS-A-FLEXIBLE-MEMBER]] (P2)
- [[D-C-A-POINTER-TO-AN-ARRAY-OF-UNKNOWN-SIZE-IS-WARNED-INCOMPATIBLE]] (P2)
- [[D-C-AN-ARRAY-OF-UNKNOWN-SIZE-IS-REFUSED-WHERE-IT-IS-NO-OBJECT]] (P2)
- [[D-CSUBSET-COMPOSITE-ALIGNED-HIR-TEXT]] (P2)
- [[D-CSUBSET-WEAK-PROTO-NO-SAME-TU-PROPAGATION]] (P2)
- [[D-DSSHIR-NATIVE-SIDE-TABLES-HAVE-NO-TEXT-FORM]] (P2)
- [[D-LK-OBJECT-STV-PROTECTED-DROPPED]] (P2)
- [[D-LK3-EXEC-EXPORT-SCOPE-NOT-CONFIG-DRIVEN]] (P3)

#### cs or lm, whichever frees first — 1

- [[D-TARGET-SYSV-AMD64-MEMORY-CLASS-ARGUMENTS-GO-BY-POINTER-AND-X87-RESULTS-BY-SRET]] (P0)

#### hm (H5) — 6

- [[D-CORE-MINGW-BUILDS-WALKS-AND-REMOVALS-THAT-FOLLOW-A-LINK]] (P1)
- [[D-LK-PE-FOREIGN-OBJECT-GS-COOKIE-ROUTINES-UNDEFINED]] (P1)
- [[D-LK-PE-FOREIGN-OBJECT-STACK-PROBE-ROUTINE-UNDEFINED]] (P1)
- [[D-HARNESS-PROTOTYPE-CENSUS-DEFAULT-REFERENCE-IS-ABSENT-ON-ITS-WINDOWS-LEGS]] (P2)
- [[D-AUDIT-P68-ROUND-12-MINOR-FINDINGS]] (P3)
- [[D-AUDIT-P68-ROUND-13-MINOR-FINDINGS]] (P3)

#### lm (L2a2, L2b) — 34

- [[D-C-A-BIT-FIELD-NARROWER-THAN-INT-IS-NOT-PROMOTED-TO-INT]] (P0)
- [[D-C-A-DECLARATORS-NAME-IS-BOUND-AT-THE-END-OF-ITS-DECLARATION]] (P0)
- [[D-C-AN-EXPRESSIONS-VALUE-KEEPS-THE-TYPE-OF-THE-OBJECT-IT-WAS-READ-FROM]] (P0)
- [[D-C-AN-ORDINARY-IDENTIFIER-DOES-NOT-HIDE-A-TYPEDEF-IN-A-TYPE-NAME]] (P0)
- [[D-C-CONSTANT-AGGREGATE-MEMBER-READ-WITHOUT-ITS-INITIALIZERS-OPERATOR]] (P0)
- [[D-C-CONSTANT-BIT-FIELD-MEMBER-IS-READ-AT-ITS-DECLARED-TYPE-NOT-ITS-WIDTH]] (P0)
- [[D-C-GENERIC-SELECTION-BEFORE-THE-EXPRESSION-PASS-SELECTS-DEFAULT]] (P0)
- [[D-CE-UNSIGNED-UNARY-RESULT-NOT-REDUCED-TO-ITS-TYPE]] (P0)
- [[D-CSUBSET-CONST-REF-DECLARED-NARROWING]] (P0)
- [[D-LIR-NO-EXCEPTIONAL-EDGE-INTO-A-TRY-HANDLER]] (P0)
- [[D-MIR-NESTED-TRY-REGIONS-REACH-THE-OUTER-HANDLER]] (P0)
- [[D-MIR-TRY-HANDLER-SHARED-FILTER-VALUE-ABORTS-IN-RELEASE]] (P0)
- [[D-PP-IF-TRUTH-RESULT-IS-AN-INT-NOT-INTMAX]] (P0)
- [[D-C-A-CONSTANT-EXPRESSION-POSITION-ACCEPTS-A-BARE-COMMA-AND-REFUSES-A-PARENTHESISED-ONE]] (P1)
- [[D-C-A-DUPLICATE-CASE-VALUE-IS-ACCEPTED]] (P1)
- [[D-C-A-PARAMETERS-ARRAY-SIZE-EXPRESSION-IS-NOT-EVALUATED-ON-ENTRY]] (P1)
- [[D-C-A-SWITCH-BODY-THAT-IS-NOT-A-COMPOUND-STATEMENT-IS-REFUSED]] (P1)
- [[D-C-A-TAG-OR-A-MEMBER-ANSWERS-THE-TYPE-NAME-TRIAGE-AS-AN-ORDINARY-NAME]] (P1)
- [[D-C-A-TYPEOF-OF-A-VARIABLE-LENGTH-ARRAY-TYPE-IS-NOT-LOWERED]] (P1)
- [[D-C-FLOAT-H-AND-FLOAT-PREDEFINES-MISSING]] (P1)
- [[D-C-GNU-CASE-RANGES-AND-RANGE-DESIGNATORS-ARE-NOT-PARSED]] (P1)
- [[D-C-GNU-STATEMENT-EXPRESSION]] (P1)
- [[D-C-NEITHER-CONSTANT-EVALUATOR-HOLDS-A-COMPLEX-VALUE]] (P1)
- [[D-C-SIZEOF-OF-A-VLA-TYPED-OPERAND-SKIPS-THE-OPERANDS-SIDE-EFFECTS]] (P1)
- [[D-CE-CAST-OF-AN-UNSIGNED-CONSTANT-ABOVE-INT64-DOES-NOT-FOLD]] (P1)
- [[D-CORE-A-LEFT-RECURSIVE-SHAPE-LOADS-THOUGH-THE-SPEC-DOCUMENTS-ITS-REFUSAL]] (P1)
- [[D-HIR-BRACE-PEEL-TAKES-AN-OPERATORS-OPERAND-FOR-THE-ELEMENT]] (P1)
- [[D-HIR-CST-CONST-EVAL-RECURSES-PER-CONDITIONAL-AND-LOGICAL-LEVEL]] (P1)
- [[D-HIR-WHOLE-STRUCTURE-BRACE-ELEMENT-IS-PLACED-BY-TYPE-STAMPS]] (P1)
- [[D-MIR-TRY-FILTER-WITH-A-BRANCH-REFUSED]] (P1)
- [[D-MIR-VERIFIER-PASSES-A-PHI-MISSING-A-PREDECESSORS-INPUT]] (P1)
- [[D-PARSE-A-PARENTHESISED-CHAIN-COSTS-PARSE-WORK-EXPONENTIAL-IN-ITS-DEPTH]] (P1)
- [[D-C-A-SCALAR-INITIALIZER-IN-MORE-BRACES-OR-WITH-MORE-ELEMENTS-IS-REFUSED]] (P2)
- [[D-C-ARITHMETIC-ON-A-POINTER-TO-A-VLA-IS-REFUSED]] (P2)

#### the dss-config holder — 1

- [[D-FFI-SHIPPED-CONSTANTS-TYPED-OTHERWISE-THAN-THE-REAL-HEADERS]] (P0)

#### the round-close sweep (fixed, awaiting its arms) — 2

- [[D-FFI-DARWIN-SYSCTL-CONSTANTS-EMPTY]] (P2)
- [[D-PLAN00-D14-FC4-DECLARATOR-SUBSTRATE-SCOPE-DESIGN-CHOSEN]] (P3)

#### xa (3d, 3e) — 25

- [[D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE]] (P0)
- [[D-LK-FOREIGN-OBJECT-INITIALIZER-TABLES-NEVER-RUN]] (P0)
- [[D-LK-FOREIGN-OBJECT-NAMED-TO-THE-LINK-BINDS-NO-LIBRARY-IMPORT]] (P0)
- [[D-LK-OBJECT-READERS-GAVE-RECORD-ZERO-THE-INVALID-SYMBOL-ID]] (P0)
- [[D-ASM-TEMPLATE-CANNOT-NAME-A-SYMBOL-OF-THE-EMBEDDING-PROGRAM]] (P1)
- [[D-LK-COFF-READER-REFUSES-A-CUSTOM-NAMED-SECTION]] (P1)
- [[D-LK-COFF-READER-REFUSES-GNU-DOTTED-TEXT-SUBSECTIONS]] (P1)
- [[D-LK-FOREIGN-OBJECT-THREAD-LOCAL-RELOCATIONS-REFUSED-AT-READ]] (P1)
- [[D-LK-MACHO-ARM64-EH-FRAME-SECTION-REFUSED-AT-READ]] (P1)
- [[D-LK-MACHO-LD-R-EH-FRAME-RELOCATIONS-REFUSED-AT-READ]] (P1)
- [[D-LK-MEMBER-DIRECT-LIBRARY-DATUM-BOUND-TO-THE-SLOT]] (P1)
- [[D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE]] (P1)
- [[D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER]] (P1)
- [[D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS]] (P1)
- [[D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED]] (P2)
- [[D-LINK-ELF-IMAGE-NO-DATA-SYMBOLS-IN-SYMTAB]] (P2)
- [[D-LINK-MACHO-IMAGE-NO-DATA-SYMBOLS-IN-SYMTAB]] (P2)
- [[D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION]] (P2)
- [[D-LK-PE-DLL-EXPORTS-AN-OVERRIDABLE-WEAK-NAME]] (P2)
- [[D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE]] (P2)
- [[D-LK-SHARED-LIBRARY-RUNS-NO-STATIC-INITIALIZERS]] (P2)
- [[D-PROGRAM-ENTRY-IN-AN-OBJECT-INPUT-BESIDE-A-SOURCE-IS-UNDEFINED]] (P2)
- [[D-PROGRAM-MERGED-ROUTE-DROPS-THE-ASSEMBLY-FORMAT-KIND]] (P2)
- [[D-WIN64-HUGE-FRAME-ALLOC]] (P2)
- [[D-LK11-FORMAT-WEAK-SEMANTICS]] (P3)

### A.2 The next PR

#### R1, lane A — 31

- [[D-C-FENV-ACCESS-PRAGMA-NOT-HONOURED]] (P1)
- [[D-C-FENV-H-NOT-SHIPPED]] (P1)
- [[D-C-STATEMENT-EXPRESSION-OF-SEVERAL-ITEMS-IS-NOT-A-CONSTANT]] (P1)
- [[D-C-WIDE-INTEGER-LONG-DOUBLE-CONVERSIONS-REFUSED]] (P1)
- [[D-CSUBSET-APPLE-SDK-HEADER-SURFACE]] (P1)
- [[D-CSUBSET-APPLE-SDK-REAL-HEADER-PARSE-CAPABILITY]] (P1)
- [[D-CSUBSET-ASM-LABEL-ON-SYNTHESIZED-SHIM-SYMBOL]] (P1)
- [[D-CSUBSET-ATOMIC-NONLOCKFREE]] (P1)
- [[D-CSUBSET-ATOMIC-SPECIFIER-FORM]] (P1)
- [[D-CSUBSET-CONSTEXPR-EXACT-REPRESENTABILITY]] (P1)
- [[D-CSUBSET-DECL-GRAMMAR-LOW-RESIDUES]] (P1)
- [[D-CSUBSET-ENUM-UNDERLYING-BASE-DRIFT]] (P1)
- [[D-CSUBSET-EXTERN-LIBRARY-SYNTAX-IS-NOT-VALID-C]] (P1)
- [[D-CSUBSET-IMAGINARY-LITERAL-SUFFIX]] (P1)
- [[D-CSUBSET-INT128-VA-ARG]] (P1)
- [[D-CSUBSET-LINKAGE-SPECIFIER-VOCABULARY-INCOMPLETE-VS-REAL-HEADERS]] (P1)
- [[D-CSUBSET-LONG-DOUBLE-CONSTFOLD-SUBNORMAL-RESULT]] (P1)
- [[D-CSUBSET-NULLPTR-T-DECLARABLE]] (P1)
- [[D-CSUBSET-PE-SETJMP-ARITY-DIVERGES-FROM-MSVC]] (P1)
- [[D-CSUBSET-QUALIFIER-DISCARD-AT-CALL-ARG-UNDIAGNOSED]] (P1)
- [[D-CSUBSET-REAL-AND-IMAG-PART-OPERATORS-UNSUPPORTED]] (P1)
- [[D-CSUBSET-SEH-EARLY-EXIT]] (P1)
- [[D-CSUBSET-SEH-LABEL-ADDR]] (P1)
- [[D-CSUBSET-SUBNATIVE-ALU-FORMS]] (P1)
- [[D-CSUBSET-TCL9-HEADER-CONSTRUCTS]] (P1)
- [[D-CSUBSET-TGMATH-SURFACE]] (P1)
- [[D-CSUBSET-UNKNOWN-TYPE-NAME-POPULATION]] (P1)
- [[D-FULLC-STDBIT-ADDRESSABLE-FN]] (P1)
- [[D-HIR-A-BRACE-INITIALIZED-ARRAY-COSTS-ITS-LENGTH-NOT-ITS-INITIALIZER]] (P1)
- [[D-PARSE-INJECTED-BUILTIN-TYPE-NAME-SKETCH-GAP]] (P1)
- [[D-PP-EMBED-STREAMING]] (P1)

#### R1, lane B — 10

- [[D-ASM-ARM64-GAS-SURFACE-INCOMPLETE]] (P1)
- [[D-ASM-ATT-GCC-OUTPUT-SURFACE-REFUSED]] (P1)
- [[D-ASM-LIR-TIER-UNBOUND-OPERAND-REFUSAL-QUOTES-THE-PLAIN-SPELLING]] (P1)
- [[D-ASM-SYSTEM-REGISTER-AS-ENCODED-DATA-UNMODELLED]] (P1)
- [[D-LK-DLL-TLS-MODEL]] (P1)
- [[D-LK-DYN-TLS-MODEL]] (P1)
- [[D-LK-RELOCATABLE-TBSS-UNSUPPORTED]] (P1)
- [[D-LK7-CODESIGN-PAGESIZE-OVERFLOW-HARDENING]] (P1)
- [[D-WIN64-XMM-UNWIND-RESTORE]] (P1)
- [[D-CSUBSET-THREAD-LOCAL-MACHO-OVERALIGN]] (P2)

#### R1, lane C — 10

- [[D-CONFIG-ASM-TEMPLATE-CAPABILITY-NO-LONGER-OPTIONAL-FOR-A-SHARED-SURFACE-IMPORTER]] (P1)
- [[D-CONFIG-NON-ALPHABETIC-MODIFIER-LETTER-WOULD-SPLIT-THE-TWO-TIERS]] (P1)
- [[D-DIAG-OPERATOR-FACING-DIAGNOSTIC-CITES-A-CLOSED-DEFERRAL-ROW]] (P1)
- [[D-FFI-DARWIN-SYS-MOUNT-PARTIAL]] (P1)
- [[D-FFI-DESCRIPTOR-VOCABULARY-TAG-RESIDUALS]] (P1)
- [[D-FFI-SHIPPED-LIBS-OS-ONLY]] (P1)
- [[D-FFI-SHIPPED-ROW-KIND-NOT-CROSSCHECKED-AGAINST-DECLARATOR]] (P1)
- [[D-MIR-SEH-FILTER-CLONER-HAS-NO-INLINE-ASM-ARM]] (P1)
- [[D-MIR-TRY-FILTER-SHARED-PURE-VALUE-REFUSED-IN-RELEASE]] (P1)
- [[D-HARNESS-SCRIPTS-INDEX-CENSUS-CANNOT-RESOLVE-A-CALL-IT-HANDS-A-PATH-VALUE]] (P3)

#### R2, lane A — 78

- [[D-C-A-COMPUTED-GOTO-IN-A-VLA-SCOPE-IS-REFUSED]] (P2)
- [[D-C-A-CONST-POINTEE-REACHED-THROUGH-A-CALL-RESULT-IS-WRITABLE]] (P2)
- [[D-C-A-GNU-LOCAL-LABEL-DECLARATION-IS-REFUSED]] (P2)
- [[D-C-A-GNU-NESTED-FUNCTION-DEFINITION-IS-REFUSED]] (P2)
- [[D-C-A-LABEL-ADDRESS-DIFFERENCE-IN-A-STATIC-INITIALIZER-IS-REFUSED]] (P2)
- [[D-C-A-LIBRARY-CALL-A-REFERENCE-FOLDS-IN-A-STATIC-INITIALIZER-IS-REFUSED]] (P2)
- [[D-C-A-QUALIFIER-AMONG-THE-TYPE-SPECIFIERS-IS-A-PARSE-ERROR]] (P2)
- [[D-C-A-REDECLARATION-DIFFERING-ONLY-IN-SIGNEDNESS-IS-REFUSED-WHERE-MSVC-WARNS]] (P2)
- [[D-C-ADDRESS-OF-AN-RVALUE-REPORTED-AS-AN-UNSUPPORTED-LOWERING]] (P2)
- [[D-C-C23-TAG-COMPATIBILITY-UNIMPLEMENTED]] (P2)
- [[D-C-FLOAT128-AND-FLOAT80-TYPES-UNSUPPORTED]] (P2)
- [[D-C-GLOBAL-REGISTER-VARIABLE-UNSUPPORTED]] (P2)
- [[D-C-GNU-BUILTIN-FAMILIES-GCC-AND-CLANG-RUN-ARE-UNDECLARED]] (P2)
- [[D-C-HEADER-VERSION-MACROS-MISSING-OR-WRONG]] (P2)
- [[D-C-ISO-HEADERS-DECLARE-NON-ISO-NAMES-UNCONDITIONALLY]] (P2)
- [[D-C-LIBRARY-FUNCTIONS-THE-REFERENCES-RUN-HAVE-NO-SHIPPED-DESCRIPTOR]] (P2)
- [[D-C-MACHO-PREDEFINES-LACK-APPLE-PLATFORM-IDENTITY]] (P2)
- [[D-C-PE-CALLING-CONVENTION-MACROS-ARE-PREDEFINED-WITHOUT-WINDOWS-H]] (P2)
- [[D-C-SEMANTIC-FIXED-CAP-LOOPS-MAY-ANSWER-AT-THE-CAP]] (P2)
- [[D-C-SIGNAL-H-INCOMPLETE-AND-ABSENT-ON-PE]] (P2)
- [[D-C-SIZEOF-OF-AN-ENUMERATION-INSIDE-ITS-OWN-LIST-IS-REFUSED-WHERE-MSVC-ACCEPTS-IT]] (P2)
- [[D-C-STDDEF-H-LACKS-MAX-ALIGN-T]] (P2)
- [[D-C-SYS-MMAN-H-SHIPS-ONLY-THE-SURFACE-SQLITE-USES]] (P2)
- [[D-C-THE-GNU-AUTO-TYPE-SPECIFIER-IS-AN-UNKNOWN-TYPE]] (P2)
- [[D-C-UCHAR-H-NOT-SHIPPED]] (P2)
- [[D-C-VA-START-VA-ARG-AND-VA-END-ARE-KEYWORDS-WITHOUT-STDARG-H]] (P2)
- [[D-C-WCHAR-H-NOT-SHIPPED]] (P2)
- [[D-CONFIG-GRAMMAR-ISA-AND-IDENTIFIERCLASS-BELONG-IN-THE-LANGUAGE-BLOCK]] (P2)
- [[D-CPP-LINE-DIRECTIVE-DIAGNOSTIC-POSITION]] (P2)
- [[D-CSUBSET-ADDRESS-CONSTANT-GLOBAL-FOLD]] (P2)
- [[D-CSUBSET-ALIGNOF-VALUE-OVERALIGNED-OBJECT]] (P2)
- [[D-CSUBSET-ATTRIBUTE-ACCESS-NO-CLANG-CLEAN-WITNESS]] (P2)
- [[D-CSUBSET-AUTO-DERIVED-DECLARATOR]] (P2)
- [[D-CSUBSET-BUILTIN-CONSTANT-P]] (P2)
- [[D-CSUBSET-BUILTIN-SHADOWED-BY-PROTOTYPE-REVERTS-TO-EXTERN]] (P2)
- [[D-CSUBSET-COMPLEX-ANNEX-G]] (P2)
- [[D-CSUBSET-COMPLEX-MONOMORPH-F64]] (P2)
- [[D-CSUBSET-COMPLEX-TRANSCENDENTAL]] (P2)
- [[D-CSUBSET-ENUM-OPAQUE-FORWARD-DECL]] (P2)
- [[D-CSUBSET-ENUM-UNDERLYING-CHAR-BOOL]] (P2)
- [[D-CSUBSET-IMAGINARY-TYPE]] (P2)
- [[D-CSUBSET-MATH-FLOAT-VARIANTS-PE]] (P2)
- [[D-CSUBSET-MIXED-NAMED-ANON-BITFIELD]] (P2)
- [[D-CSUBSET-NODISCARD-INDIRECT-CALLEE]] (P2)
- [[D-CSUBSET-NORETURN-VALUE-POSITION]] (P2)
- [[D-CSUBSET-NULLPTR-CONSTEXPR-FOLD]] (P2)
- [[D-CSUBSET-PARAM-INLINE-COMPOSITE-DEFINITION]] (P2)
- [[D-CSUBSET-PAREN-TYPEDEF-FORWARD-USE-RESEED]] (P2)
- [[D-CSUBSET-POINTER-ARITH-FLOAT-OPERAND]] (P2)
- [[D-CSUBSET-PTR-TO-NARROW-INT-TRUNCATE]] (P2)
- [[D-CSUBSET-SEH-FINALLY]] (P2)
- [[D-CSUBSET-SEH-LEAVE]] (P2)
- [[D-CSUBSET-SETJMP-SIGMASK]] (P2)
- [[D-CSUBSET-STRING-ARRAY-BRACED-INIT]] (P2)
- [[D-CSUBSET-THREAD-LOCAL-BLOCK-SCOPE-EXTERN]] (P2)
- [[D-CSUBSET-THREAD-LOCAL-CROSS-TU-MISMATCH]] (P2)
- [[D-CSUBSET-THREAD-LOCAL-DYNAMIC-TLS]] (P2)
- [[D-CSUBSET-THREAD-LOCAL-FREESTANDING-EXEC]] (P2)
- [[D-CSUBSET-THREAD-LOCAL-MACHO-TDATA-RELOC]] (P2)
- [[D-CSUBSET-THREAD-LOCAL-PE-CALLBACKS]] (P2)
- [[D-CSUBSET-THREAD-LOCAL-PE-LARGE-ZEROFILL]] (P2)
- [[D-FC12A-VARIADIC-AL-GATED-SPILL]] (P2)
- [[D-FC7-INLINE-MULTI-PIECE-RETURN]] (P2)
- [[D-HR10-FLAT-EXPR-TABLE-BLIND]] (P2)
- [[D-PARSE-IDENTIFIER-KIND-HARDCODED-VS-CONFIG]] (P2)
- [[D-PARSE-RECOVERY-MULTI-LEVEL-UNWIND]] (P2)
- [[D-PP-DATETIME-BUF-SIZED-FOR-VALID-TM-ONLY]] (P2)
- [[D-PP-DESCRIPTOR-STEM-REWRITE-NARROW-ROUND-TRIP]] (P2)
- [[D-PP-DIAGNOSTIC-ATTRIBUTED-TO-BUILTIN-PROLOGUE]] (P2)
- [[D-PP-EMBED-RESOURCE-CASE-RULE-FOLLOWS-TARGET-NOT-TREE]] (P2)
- [[D-PP-HEADER-NAME-REQUEST-BYTES-TOOLCHAIN-ENCODING]] (P2)
- [[D-PP-IF-OPERAND-PARSE-NO-SHORTCIRCUIT]] (P2)
- [[D-PP-INCLUDED-FILE-NAME-RESPELLED-WITH-THE-HOST-SEPARATOR]] (P2)
- [[D-PP-ORDINARY-EXPANSION-TOKEN-HAS-NO-EXPANSION-SITE]] (P2)
- [[D-PP-PRESCAN-DESCRIPTOR-MACROS-UNTRACKED]] (P2)
- [[D-PP-PRESCAN-MINT-RETOKENIZE-PERF]] (P2)
- [[D-SEMANTIC-MAYBE-UNINITIALIZED-WARN]] (P2)
- [[D-TOKENIZER-FORMFEED-SCHEMA-CURSOR-DESYNC]] (P2)

#### R2, lane B — 53

- [[D-AS-REWRITE-CALL-HOISTED-ABOVE-ARG-CAPTURE]] (P2)
- [[D-AS4-ARM64-INDEXED-LEA-SCALE]] (P2)
- [[D-AS4-ARM64-LARGE-DISP-LEA-BASE-CLOBBER]] (P2)
- [[D-AS4-DISP8-ENCODING]] (P2)
- [[D-ASM-AARCH64-FRAME-OFFSET-BEYOND-2GIB]] (P2)
- [[D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED]] (P2)
- [[D-ASM-TARGET-DECLARES-NO-BYTE-ORDER]] (P2)
- [[D-ASM-TEMPLATE-IS-LEXED-TWICE]] (P2)
- [[D-ASM-TEMPLATE-OPERAND-IN-A-DROPPED-ALTERNATIVE-REFUSED]] (P2)
- [[D-ASM-TEMPLATE-OWN-LABEL-ADDRESS-REFUSED]] (P2)
- [[D-ASM-X86-BRANCHES-ARE-ALWAYS-REL32]] (P2)
- [[D-ASM-X86-SMALL-IMMEDIATES-TAKE-THE-WIDE-FORM]] (P2)
- [[D-CODEGEN-FRAME-SIZE-VS-NATIVE-2X]] (P2)
- [[D-FF1-AR-COFF-IMPORT]] (P2)
- [[D-FF1-DISPATCH-TABLE]] (P2)
- [[D-FF1-MACHO-32]] (P2)
- [[D-FF1-MACHO-FAT]] (P2)
- [[D-FF1-MACHO-READER]] (P2)
- [[D-FF1-MACHO-SECT-KIND]] (P2)
- [[D-FF1-MACHO-VARIANT-KIND]] (P2)
- [[D-FF1-MACHO-WEAK-DEF]] (P2)
- [[D-FF1-PARTIAL-CORRUPTION-LOUD]] (P2)
- [[D-FF1-PE-READER]] (P2)
- [[D-FF2-MSG-JARGON]] (P2)
- [[D-FF2-UNSUPP-FULL-SWEEP]] (P2)
- [[D-FF5-EXTERNDECLREF-VALIDATE]] (P2)
- [[D-LINK-A-DEEP-OUTPUT-DIRECTORY-FAILS-THE-WINDOWS-IMAGE-WRITE]] (P2)
- [[D-LINK-MACHO-STATIC-EXEC-SYMTAB-UNALIGNED]] (P2)
- [[D-LK-ARM64-DEFINED-GLOBAL-ADDR-PIE-GOT]] (P2)
- [[D-LK-DEDUP-INTERNAL-BREACH-MISFILED-AS-SYMBOL-UNDEFINED]] (P2)
- [[D-LK-DYN-TEXT-ABS-RELOC]] (P2)
- [[D-LK-FOREIGN-CALL-TO-A-WEAK-SYMBOL-RESOLVED-TO-NOTHING-HAS-NO-STUB]] (P2)
- [[D-LK-MACHO-ARM64-RELOC-ADDEND-PAIR-UNMODELLED]] (P2)
- [[D-LK-MACHO-MISSING-CONST-SECTION-RETURNS-EMPTY-SILENTLY]] (P2)
- [[D-LK-MACHO-MULTI-ITEM-SECTION-PADDING]] (P2)
- [[D-LK-MACHO-STACK-RESERVE-LC-MAIN]] (P2)
- [[D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC]] (P2)
- [[D-LK-STATIC-LINK-PCREL-MULTIATOM-DATA]] (P2)
- [[D-LK1-MACHO-RODATA-DATAITEM-RELOC]] (P2)
- [[D-LK2-DLL-DLLMAIN-ENTRY]] (P2)
- [[D-LK4-RODATA-PRODUCER-EXOTIC]] (P2)
- [[D-LK4-RODATA-WALKER-RELOC-BASE]] (P2)
- [[D-LK7-CODESIGN-LINKERSIGNED-FLAG]] (P2)
- [[D-MERGE-CONTEXT-PREFIX-CLOBBER]] (P2)
- [[D-MERGE-CROSSCU-INTRINSIC-ID-NAMESPACE]] (P2)
- [[D-MERGE-DEDUP-PREFIX-COLLISION]] (P2)
- [[D-MERGE-DEFINEDORIMPORTED-WALKS-FUNCTIONS-ONLY]] (P2)
- [[D-MERGE-LOWER-EMPTY-NAME-GUARD]] (P2)
- [[D-MERGE-POLICY-IDEMPOTENCY]] (P2)
- [[D-MERGE-UNREACHABLE-BLOCK]] (P2)
- [[D-UNWIND-STATIC-ET-EXEC-NO-EH-FRAME]] (P2)
- [[D-WIN64-CHKSTK-LARGE-PROLOGUE]] (P2)
- [[D-WIN64-SEH-SCOPE]] (P2)

#### R2, lane C — 59

- [[D-AP2-ENTRY-POINT-CONVENTION]] (P2)
- [[D-AP2-MULTIPLE-SOURCES-SEMANTICS]] (P2)
- [[D-AP2-TARGET-NAME-DEFAULT-FORMAT]] (P2)
- [[D-CLI-UNDOCUMENTED-DEFINE-AND-INCLUDE-FLAGS]] (P2)
- [[D-CONFIG-ATTRIBUTE-ARG-RULE-DOCUMENTED-BUT-UNIMPLEMENTED]] (P2)
- [[D-CONFIG-FORMAT-DECLARES-NO-UNIFORM-ARCHITECTURE]] (P2)
- [[D-CONFIG-FORMAT-DOC-KEY-DOCUMENTS-NONEXISTENT-KNOB]] (P2)
- [[D-CONFIG-GNUC-VERSION-TRUTHFULNESS-COUPLING]] (P2)
- [[D-CORE-OFFSET-CLAMP-FAMILY-INCOMPLETE]] (P2)
- [[D-DIAG-0X5XXX-NIBBLE-CLAIMED-BY-TWO-FAMILIES]] (P2)
- [[D-DIAG-CODE-RANGE-0X5XXX-DOUBLE-ALLOCATED]] (P2)
- [[D-DIAG-ENUM-AND-ITS-SWITCH-ARM-ARE-A-TWO-FILE-ATOMIC-EDIT]] (P2)
- [[D-DIAG-GOT-PREFIX-BREAKS-SENTENCE-SHAPED-MESSAGES]] (P2)
- [[D-DIAG-INCLUDE-LOCATION-ATTRIBUTION]] (P2)
- [[D-DIAG-NO-NEAREST-MATCH-SUGGESTION]] (P2)
- [[D-DIAG-PATH-NARROWING-PROJECT-WIDE]] (P2)
- [[D-DIAG-SHIPPED-HEADER-MISS-NO-NEAREST-MATCH]] (P2)
- [[D-DIAG-SOLE-STATEMENT-REJECTS-MAY-SUPPRESS-TO-A-SILENT-EXIT]] (P2)
- [[D-DIAG-SOURCE-LINE-POST-PREPROCESS-OFFSET]] (P2)
- [[D-DIAG-SURFACE-CLAIM-MASKS-THE-DESCRIPTOR-ROOT-CAUSE]] (P2)
- [[D-DRIVER-MIXED-LANGUAGE-CU-UNREACHABLE]] (P2)
- [[D-FFI-ANGLE-INCLUDE-LINE-SCOPED-HEADER]] (P2)
- [[D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN]] (P2)
- [[D-FFI-DARWIN-FLOCK-PROVENANCE]] (P2)
- [[D-FFI-DARWIN-LOCK-CONSTANT-PROVENANCE]] (P2)
- [[D-FFI-DARWIN-MALLOC-ZONE-FN-PROVENANCE]] (P2)
- [[D-FFI-DARWIN-SYS-PARAM-PARTIAL]] (P2)
- [[D-FFI-DARWIN-SYS-UNISTD-SUBHEADER]] (P2)
- [[D-FFI-DARWIN-TARGETCONDITIONALS-EMPTY]] (P2)
- [[D-FFI-DESCRIPTOR-BY-NAME-UNSELECTED-DEPENDENT-SILENT]] (P2)
- [[D-FFI-DESCRIPTOR-MACRO-VARIANTS-FORMAT-ONLY]] (P2)
- [[D-FFI-DESCRIPTOR-SHADOWS-DASH-I-HEADER]] (P2)
- [[D-FFI-DESCRIPTOR-UNIONS-NO-VARIANTS]] (P2)
- [[D-FFI-ELF-ATEXIT-REGISTERS-NO-DSO-HANDLE]] (P2)
- [[D-FFI-ELF-READER-SURFACES-VERSION-DEFINITION-PSEUDO-SYMBOLS]] (P2)
- [[D-FFI-RESOLVE-LIBRARY-DEMANDS-A-BINARY]] (P2)
- [[D-FFI-SHIPPED-LIB-WHEN-FORMAT-COMPARED-BY-NAME]] (P2)
- [[D-FFI-SHIPPED-PROTOTYPES-DIFFER-FROM-THE-REAL-HEADERS]] (P2)
- [[D-FFI-UCRT-HEADER-DECLARES-MBCASEMAP-AS-ARRAY]] (P2)
- [[D-FFI-VESTIGIAL-PE-LIBRARY-BINDINGS]] (P2)
- [[D-HARNESS-PROTOTYPE-CENSUS-PASSES-WITH-NO-VERDICTS]] (P2)
- [[D-LIR-LLSC-SPILL-EXCLUSION]] (P2)
- [[D-LIR-PHI-EDGE-COPIES-NOT-SPLIT-FOR-INDIRECTBR]] (P2)
- [[D-LIR-VERIFY-VREG-CLASS-RULE-ASSUMES-A-ONE-TO-ONE-LIR-TO-MIR-MAP]] (P2)
- [[D-LSP-A-POSITION-AT-THE-END-OF-A-DOCUMENT-ANSWERS-NOTHING]] (P2)
- [[D-LSP-DEPENDENCY-MANIFESTS-UNREAD-MODULE-SOURCES-CHECKED-UNDER-THE-ROOTS-SETTINGS]] (P2)
- [[D-LSP-NO-OUTBOUND-REQUEST-CHANNEL]] (P2)
- [[D-MIR-SYNTH-SHIM-HELPER-SIGNATURES-DUPLICATE-THE-DESCRIPTOR]] (P2)
- [[D-MIR-SYNTH-THREADS-SHIM-VOID-PTR-SIGNATURE-LOSES-CHECKABILITY]] (P2)
- [[D-OPT-CONSTANT-FOLD-SKIPS-FLOAT-CONVERSIONS]] (P2)
- [[D-OPT-RELEASE-PIPELINE-GAPS-SURFACED-BY-BYTE-IDENTITY-TRIAGE]] (P2)
- [[D-PERF-CU-POOL-SCALES-HALF-AS-WELL-AS-SEPARATE-PROCESSES]] (P2)
- [[D-PERF-DSS-FULL-SOURCE-SQLITE-COMPILE-TIME-ROSE-A-FIFTH-TO-A-QUARTER-SINCE-2026-08-28]] (P2)
- [[D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX]] (P2)
- [[D-PIPELINE-CU5-MULTIFILE-EXTERN-DATA]] (P2)
- [[D-PROCESS-ESCAPING-HOPS-COLLAPSE-BACKSLASHES-BEFORE-THE-FILE-SEES-THEM]] (P2)
- [[D-SQLITE-CLI-CROSS-TU-PUBLIC-API-UNRESOLVED]] (P2)
- [[D-SQLITE-CLI-TERMINAL-WIDTH-SHADOWED-SILENTLY]] (P2)
- [[D-TEST-SIZEOF-THROUGH-A-POINTER-TO-A-VLA-PIN-ACCEPTS-ANY-SLOT-LOAD]] (P2)

#### R3, lane A — 15

- [[D-C-CONST-QUALIFIED-NAMED-VOID-PARAMETER-DECLARATION-REFUSED]] (P3)
- [[D-C-DUPLICATE-ARRAY-PARAMETER-QUALIFIERS-ARE-ACCEPTED]] (P3)
- [[D-C-SIZEOF-AN-UNDECLARED-ENUM-TAG-IS-REFUSED-WHERE-MSVC-BUILDS-IT]] (P3)
- [[D-CSUBSET-ALLOCA-COUNT-CACHE]] (P3)
- [[D-CSUBSET-CMP-COND-CORPUS]] (P3)
- [[D-CSUBSET-CONST-PTR-DECAY]] (P3)
- [[D-CSUBSET-LINKAGE-SPECIFIERS]] (P3)
- [[D-CSUBSET-NORETURN-THROUGH-MEMBER-INDIRECT-CALL]] (P3)
- [[D-CSUBSET-THREAD-LOCAL-INITIAL-EXEC]] (P3)
- [[D-CSUBSET-UNSIGNED-CMP-CORPUS]] (P3)
- [[D-ENUM-NAME-TABLE-STATIC-ASSERTS]] (P3)
- [[D-LANG-NULL-PTR-CONST-EVAL]] (P3)
- [[D-LANG-NUMERIC-PROMOTION-CONFIG]] (P3)
- [[D-LANG-STRUCTURAL-DECAY-OPT-OUT]] (P3)
- [[D-PP-FC2-ORACLE-RETIRE]] (P4)

#### R3, lane B — 79

- [[D-AS3-1]] (P3)
- [[D-AS3-3]] (P3)
- [[D-AS3-4]] (P3)
- [[D-AS3-5]] (P3)
- [[D-AS3-6]] (P3)
- [[D-AS4-1]] (P3)
- [[D-AS4-2]] (P3)
- [[D-AS4-4]] (P3)
- [[D-AS4-5-ISCALL-IMPLICITRESULT-SEPARATION-AS4-INTRODUCES-ISCALL]] (P3)
- [[D-AS4-6]] (P3)
- [[D-AS4-7]] (P3)
- [[D-AS4-8]] (P3)
- [[D-AS5-1]] (P3)
- [[D-AS5-MULTIWORD-DISASM]] (P3)
- [[D-AS5-ROUNDTRIP-MEMORY-OPERAND-COVERAGE]] (P3)
- [[D-ASM-ARM64-CBZ-CBNZ-ALWAYS-EMIT-A-SECOND-WORD]] (P3)
- [[D-ASM-FALLTHROUGH-INTO-A-LABEL-EMITS-A-JUMP]] (P3)
- [[D-ASM-UNREACHABLE-TAIL-EMITS-A-TRAP]] (P3)
- [[D-FRAMELAYOUT-COUNT-FIELDS-AUDIT]] (P3)
- [[D-LK-MACHO-BSS-ONLY-LINKEDIT-STRUCTURAL-PIN]] (P3)
- [[D-LK-TEXT-ABS64-BASE-RELOC]] (P3)
- [[D-LK1-1]] (P3)
- [[D-LK1-5]] (P3)
- [[D-LK1-RODATA]] (P3)
- [[D-LK10-10]] (P3)
- [[D-LK10-3]] (P3)
- [[D-LK10-4]] (P3)
- [[D-LK10-5]] (P3)
- [[D-LK10-9]] (P3)
- [[D-LK10-ENTRY-LIRTOMIRSENTINEL]] (P3)
- [[D-LK10-ENTRY-MODULE-COW]] (P3)
- [[D-LK10-ENTRY-SYNTH-PREFIX-SCHEMA]] (P3)
- [[D-LK10-ENTRY-SYSCALL-IMM64]] (P3)
- [[D-LK10-KERNEL32-WRITE-PATH]] (P3)
- [[D-LK11-COMPILE-DIRECTORY-CU-MODE]] (P3)
- [[D-LK2-2]] (P3)
- [[D-LK4-10]] (P3)
- [[D-LK4-11]] (P3)
- [[D-LK4-4]] (P3)
- [[D-LK4-5]] (P3)
- [[D-LK4-6]] (P3)
- [[D-LK4-RODATA-PRODUCER-EXOTIC-FLOAT]] (P3)
- [[D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY-LITERAL-DECAY]] (P3)
- [[D-LK4-RODATA-PRODUCER-RANGE-GUARD]] (P3)
- [[D-LK4-RODATA-PRODUCER-RUNTIME-INIT]] (P3)
- [[D-LK6-10]] (P3)
- [[D-LK6-11]] (P3)
- [[D-LK6-12]] (P3)
- [[D-LK6-13]] (P3)
- [[D-LK6-14]] (P3)
- [[D-LK6-14-32]] (P3)
- [[D-LK6-14-ARM64E]] (P3)
- [[D-LK6-14-CHAINEDPTR-FORMAT-COUPLING]] (P3)
- [[D-LK6-14-MACHO-WEAK-DEF]] (P3)
- [[D-LK6-14-MULTI-PAGE-GOT]] (P3)
- [[D-LK6-14-REBASE]] (P3)
- [[D-LK6-15]] (P3)
- [[D-LK6-16]] (P3)
- [[D-LK6-17]] (P3)
- [[D-LK6-18]] (P3)
- [[D-LK6-2]] (P3)
- [[D-LK6-9]] (P3)
- [[D-LK7-1]] (P3)
- [[D-LK7-2]] (P3)
- [[D-LK7-3]] (P3)
- [[D-LK7-4]] (P3)
- [[D-LK7-5]] (P3)
- [[D-LK7-6]] (P3)
- [[D-LK7-7]] (P3)
- [[D-LK9-1]] (P3)
- [[D-ML7-EXTERNSYMBOLS-LIVE-VIEW]] (P3)
- [[D-MULTI-CU-POINTER-ALIAS-AGGREGATION]] (P3)
- [[D-OPCODE-HANDLES-NAMING-SWEEP]] (P3)
- [[D-FF3-4-MICROSOFT-ARM64-CC-ROW]] (P4)
- [[D-FF4-1-PE32-STDCALL-DECORATION]] (P4)
- [[D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD]] (P4)
- [[D-LK6-14-NAME-OFFSET-OVERFLOW]] (P4)
- [[D-MERGE-CONTEXT-PREFIX-SIDE-TABLE]] (P4)
- [[D-MERGE-SCRATCH-FRESH]] (P4)

#### R3, lane C — 134

- [[D-BUILD-AR-READER-FREE-NONHEAP-WARNING-ON-AARCH64-GCC13]] (P3)
- [[D-COMPILE-ONE-TARGET-NO-LEAK]] (P3)
- [[D-DIAG-CODES-WITH-NO-COMPILED-TEST-REFERENCE]] (P3)
- [[D-DIAG-CORPUS-EVERY-CODE]] (P3)
- [[D-FFI-ABI-GATE-HELPER]] (P3)
- [[D-FFI-EXTERN-RECORD-VIEW-UNIFY]] (P3)
- [[D-FFI-HEADER-VALIDATION-OPTIONAL]] (P3)
- [[D-FFI-INGEST-RESULT-PRODUCER-TOKEN]] (P3)
- [[D-FFI-SYNTH-ANNOTATION-COUNT-VERIFY]] (P3)
- [[D-GATE-CONFIG-PATH-CWD-RELATIVE]] (P3)
- [[D-GATE-MACOS-DEBUG-LEG-COSTS-SEVEN-TIMES-ITS-OWN-BASELINE]] (P3)
- [[D-GLOB-BASE-DIR-HOST-CASE-SENSITIVITY]] (P3)
- [[D-LIR-ALLOCA-LOWERING-EXTRACTION]] (P3)
- [[D-LIR-CALL-EXTERN-VARIANT-UNIFY]] (P3)
- [[D-LSP-UNLISTED-HEADER-CHECKED-UNDER-EVERY-MANIFEST-NOT-ITS-INCLUDERS]] (P3)
- [[D-OPT-CARVE-OUT-COUNT-REPORTING]] (P3)
- [[D-OPT-COMPILE-OPTIONS-VARIANT]] (P3)
- [[D-OPT-COPYPROP-BLOCK-CURSOR]] (P3)
- [[D-OPT-DIAGNOSTIC-CODE-SPLIT-OOR-VS-FILE]] (P3)
- [[D-OPT-INTERNER-MUTABLE]] (P3)
- [[D-OPT-MIR-ALIAS-RULE3-FAIL-LOUD]] (P3)
- [[D-OPT-MIR-ALIAS-TYPEKIND-DRIFT]] (P3)
- [[D-OPT-MIR-DOMTREE-CLASS-INVARIANT]] (P3)
- [[D-OPT-PIPELINE-OVERRIDE-VALUE-TYPE]] (P3)
- [[D-OPT-PRE-PIPELINE-VERIFY]] (P3)
- [[D-OPT-REBUILD-OPERAND-ID-PHASE-TYPING]] (P3)
- [[D-OPT-RUNNER-SHARD]] (P3)
- [[D-OPT1-0]] (P3)
- [[D-OPT1-1]] (P3)
- [[D-OPT1-2]] (P3)
- [[D-OPT1-CONST-FOLD-MIR-PIN]] (P3)
- [[D-OPT1-COPY-PROP-JOIN-PIN]] (P3)
- [[D-OPT1-CSE-NONCOMMUTATIVE-PIN]] (P3)
- [[D-OPT1-DCE-NEGATIVE-PIN]] (P3)
- [[D-OPT1-LICM-CONDITIONAL-PIN]] (P3)
- [[D-OPT1-PASS-ID-STABILITY]] (P3)
- [[D-OPT1-PIPELINE-BUDGET]] (P3)
- [[D-OPT1-PIPELINE-VALUE-TYPE]] (P3)
- [[D-OPT1-RETURN-FALSE-CONTRACT-UNIT-PIN]] (P3)
- [[D-OPT10-1]] (P3)
- [[D-OPT10-2]] (P3)
- [[D-OPT2-1]] (P3)
- [[D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS]] (P3)
- [[D-OPT2b-1]] (P3)
- [[D-OPT3-DCE-ANONYMOUS-SYMBOL]] (P3)
- [[D-OPT3-DCE-DUPLICATE-SYMBOL]] (P3)
- [[D-OPT4-2]] (P3)
- [[D-OPT5-1]] (P3)
- [[D-OPT6-1]] (P3)
- [[D-OPT6-LICM-PREHEADER-INSERTION]] (P3)
- [[D-OPT6-LICM-TRAP-SAFE-HOIST]] (P3)
- [[D-OPT7-LINKAGE-HIR-TO-MIR-MAPPING]] (P3)
- [[D-OPT9-1]] (P3)
- [[D-PLAN00-D10-CU6-CROSS-REFERENCES-INCREMENTAL-REBUILD]] (P3)
- [[D-PLAN00-D12-SHIPPED-LIB-FFI-MODEL-NEUTRAL-DESCRIPTOR]] (P3)
- [[D-PLAN00-D13-HOSTED-FORMAT-PROCESS-EXIT-LIBC-EXIT]] (P3)
- [[D-PLAN00-D15-RELEASE-PIPELINE-CLOSURE-SEQUENCING-THE-MARKER]] (P3)
- [[D-PLAN00-D5-SUBSET-STRUCTS-UNIONS-ENUMS]] (P3)
- [[D-PLAN00-D6-TSQL-JOINS-SUBQUERIES-CTES-STORED-PROCS]] (P3)
- [[D-PLAN00-D9-USE-BEFORE-INIT-UNREACHABLE-AFTER-RETURN]] (P3)
- [[D-PLAN08-C2-D1-SCHEMALOADFAILED-THE-CONFIGDIAGNOSTIC-PARSEDIAGNOSTIC-BRIDGE]] (P3)
- [[D-PLAN08-C2-D2-ENTRY-POINT-SELECTION-THE-ROUTED]] (P3)
- [[D-PLAN08-C2-D3-LSP-ADOPTING-THE-UNIFIED-LEXER-PARSER]] (P3)
- [[D-PLAN08-C2-D4-POSITION-SORTED-INTERLEAVED-DIAGNOSTIC-ORDERING]] (P3)
- [[D-PLAN08-C2-D5-CROSS-TREE-SYMBOLID-SHAPE-CROSS-NODEID]] (P3)
- [[D-PLAN08-C2-D6-CROSSTREEREF-POPULATION-IMPORT-RESOLUTION]] (P3)
- [[D-PLAN08-C3-D1-CROSSTREEREF-VECTOR-POPULATION]] (P3)
- [[D-PLAN08-C3-D2-REAL-SYMBOL-TABLE-SCOPES-NAME-RESOLUTION]] (P3)
- [[D-PLAN08-C4-D1-DSS-PROJECT-JSON-INCLUDE-PATH-DECLARATION]] (P3)
- [[D-PLAN08-C4-D2-ANGLE-BRACKET-INCLUDE-SYS-SYSTEM-HEADER]] (P3)
- [[D-PLAN08-C4-D3-BRACKET-QUOTED-TSQL-IDENTIFIERS-TABLE-NAME]] (P3)
- [[D-PLAN08-C4-D4-MULTI-LANGUAGE-CUS-CROSS-REFERENCES]] (P3)
- [[D-PLAN08-D1-UNITBUILDER-ADDFILE-PATH-ADDINMEMORY-TEXT-LABEL]] (P3)
- [[D-PLAN08-D2-DRIVER-LEVEL-DIAGNOSTIC-CODES-HEX-0XD001]] (P3)
- [[D-PLAN08-D3-SH3-ANALOG-CROSS-NODEID-GUARD-NODEID]] (P3)
- [[D-PLAN08-D4-CROSSTREEREF-VECTOR-POPULATION]] (P3)
- [[D-PLAN08-D5-ENTRY-POINT-SELECTION-WHICH-SOURCE-THE]] (P3)
- [[D-PLAN08-D6-MULTI-LANGUAGE-CUS-DIFFERENT-LANG-JSON]] (P3)
- [[D-PLAN08-D7-CROSS-OPERATIONS-ANY-KIND-REFERENCES-BETWEEN]] (P3)
- [[D-PLAN08-D8-ARTIFACTPROFILE-HANDLES-THE]] (P3)
- [[D-PLAN12-BUILDSYNTHETICFN-TEST-HELPER-PROMOTION-LIFT-FROM-TESTS-LIR]] (P3)
- [[D-PLAN12-CONDCODEFORICMP-BLIND-DEREF-HARDENING-RETURN-STD-OPTIONAL-TARGETCONDCODE]] (P3)
- [[D-PLAN12-CROSS-TYPE-IMPORT-FOR-HIR-CAST-WHEN-HIR]] (P3)
- [[D-PLAN12-DIAGNOSTICREPORTER-THREADING]] (P3)
- [[D-PLAN12-DSSLIR-PARSER-EMITLIR-PARSELIR-EMITLIR-EMITLIR-ROUND-TRIP]] (P3)
- [[D-PLAN12-FRAME-MNEMONICS-PROMOTED-TARGETSCHEMA-FIELDS]] (P3)
- [[D-PLAN12-HOIST-COLLECTINSTSTATS-SHARED-TEST-SUPPORT-HEADER-THE-ABI]] (P3)
- [[D-PLAN12-INTRINSICCALL-ROUND-TRIP-TEST]] (P3)
- [[D-PLAN12-INTRINSICCALL-VOID-NON-VOID-RESULT-VALIDITY-VERIFIER-RULE]] (P3)
- [[D-PLAN12-INTRINSICCALL-VOID-NON-VOID-RESULT-VALIDITY-VERIFIER-RULE-2]] (P3)
- [[D-PLAN12-LIR-REG-CLASS-TARGETREGCLASS-LIRREGCLASS-HELPER-LIR-REG]] (P3)
- [[D-PLAN12-LIRREGASSIGNMENT-VARIANT-PAYLOAD]] (P3)
- [[D-PLAN12-LIRVERIFIER-PRE-EMIT-INVOCATION-POLICY]] (P3)
- [[D-PLAN12-LOWERSWITCH-FIRST-CMP-IMPLICIT-BLOCK-PLACEMENT-ASSERTION-ASSERT]] (P3)
- [[D-PLAN12-MIR-DIRECTCALLSYMBOL-MIRINSTID-CALL-STD-OPTIONAL-SYMBOLID-SUBSTRATE]] (P3)
- [[D-PLAN12-MIRSHADERATTRIBUTE-SIDE-TABLE-FOR-SPIR-DECORATIONS-SHADER-DECORATIONS]] (P3)
- [[D-PLAN12-MIRSOURCEMAP-INJECTION-SLOT-MIRVERIFIER-CYCLE-STASHES-NODE-ACTUAL]] (P3)
- [[D-PLAN12-MULTI-SPILLED-OPERAND-REG-ROTATION-CURRENT-IMPLEMENTATION-POOLS]] (P3)
- [[D-PLAN12-NAMEDREGISTERREF-COLLAPSE-STACKPOINTERREF-LINKREGISTERREF-WERE-EXACT-COPY-STRUCTS]] (P3)
- [[D-PLAN12-PUSH-POP-OPTIMIZATION-PEEPHOLE-STORE-REG-SUB-PUSH]] (P3)
- [[D-PLAN12-REGALLOC-PRE-COLORING-HINT-FOR-ARG-CALL-ARG]] (P3)
- [[D-PLAN12-REGCLASSFORCORETYPE-DEFAULT-ARM-EXPLICIT-AGGREGATE-CASES-REPLACE-THE]] (P3)
- [[D-PLAN12-REGCLASSFORCORETYPE-TYPEID-TYPEINTERNER-CONST-TARGETREGCLASS-OVERLOAD-DRY-AGAINST]] (P3)
- [[D-PLAN12-REGCLASSFORTYPE-INVALIDID-DIAGNOSTIC-CURRENTLY-THE-SUBSTRATE-HELPER-SILENTLY]] (P3)
- [[D-PLAN12-REGCLASSFORTYPE-INVALIDID-DIAGNOSTIC-VERIFIED-LONGER-APPLICABLE-TYPEKIND-ENUM]] (P3)
- [[D-PLAN12-REVERSE-BITCAST-I64-F64-TEST-CURRENTLY-ONLY-F64]] (P3)
- [[D-PLAN12-REWRITE-PASS-SPILL-LOAD-STORE-INSERTION-LIRVERIFIER-VIRTUAL]] (P3)
- [[D-PLAN12-SCHEMA-VERSION-TAG-DSSLIR-PREAMBLE]] (P3)
- [[D-PLAN12-SHARED-LIR-PASS-UTIL-HPP-FOR-EMITTERMINATOR-REPORT]] (P3)
- [[D-PLAN12-SHARED-TEXT-FORMAT-SUBSTRATE-SPANNING-MIR-LIR-HIR]] (P3)
- [[D-PLAN12-SPLIT-AWARE-SUB-INTERVAL-LIRLIVERANGE-LIST-CURRENTLY-FLAT]] (P3)
- [[D-PLAN12-SUB-INTERVAL-LIRLIVERANGE-LIST-DEFERRED-FROM-ML6-CYCLE]] (P3)
- [[D-PLAN12-TARGETOPCODEINFO-PAYLOADKIND-DISCRIMINATOR-PAYLOAD-FIELD-CARRIES-SPILL-SLOT]] (P3)
- [[D-PLAN12-TARGETOPCODEINFO-PAYLOADKIND-VERIFIER-RULE-CLOSES-THE-PAYLOAD-CHANNEL]] (P3)
- [[D-PLAN12-TARGETOPCODEINFO-TERMINATORKIND-ENUM-SCHEMA-DRIVEN-DISPATCH]] (P3)
- [[D-PLAN12-TARGETSCHEMA-MULTI-SHAPE-DISCRIMINATOR-WIRING-TARGETABIMODEL-ENUM-REGISTERMACHINE]] (P3)
- [[D-PLAN12-WIN64-X64-MIXED-INT-FLOAT-ARG-STACK-OVERFLOW]] (P3)
- [[D-PLAN14-PER-MACHINE-SPLIT-FOR-ENCODEELFEXECDYNAMIC-LK6-ADDED-PER]] (P3)
- [[D-PLAN14-TARGETARCHKIND-CLOSED-ENUM-TARGETSCHEMA-STRING-KEY-ENUM-KEY]] (P3)
- [[D-PLAN14-TARGETARCHMACHINECODES-ROW-RESHAPE-STD-OPTIONAL-U16-U16-U32]] (P3)
- [[D-RUN-HARNESS-LARGE-STDOUT-DEADLOCK]] (P3)
- [[D-RUN-HARNESS-STDIO-SPLIT]] (P3)
- [[D-SCRIPT-RUN-GATE-LOG-HOLDER-IDENTITY-KEYS-ON-A-WALL-CLOCK-A-STEPPING-HOST-REWRITES]] (P3)
- [[D-SQLITE-GCC-REFERENCE-FIXTURE-AS-ORACLE]] (P3)
- [[D-TEST-IGNORE-LIST-IS-A-LICENSE-TO-DROP]] (P3)
- [[D-TEST-NATIVE-PROBE-COMPILE-FAILS-UNDER-CONCURRENT-LOAD]] (P3)
- [[D-DOC-EXTERNLIBRARYBYFORMAT-STILL-DESCRIBED-AS-LIVE]] (P4)
- [[D-DOC-SHIPPED-LIB-DESCRIPTOR-HPP-SAYS-SYMBOLS-IS-REQUIRED]] (P4)
- [[D-DOC-VARIADIC-FN-CLAIMED-UNSUPPORTED-IN-TWO-PLACES-BUT-SHIPS]] (P4)
- [[D-OPT7-INLINE-LEGALITY-GATE]] (P4)
- [[D-SQLITE-PE64-ALL-TIER-OOM-FILE-HANDLE-LEAK]] (P5)
- [[D-SQLITE-PE64-MMAP-PERMUTATION-WINDOWS-CONFOUNDS]] (P5)
- [[D-UPSTREAM-SQLITE-FTS5TOK2-SCRIPT-LEVEL-ARRAY-READ-ABORTS-THE-FIXTURE]] (P5)
- [[D-UPSTREAM-SQLITE-SESSIONCONFLICT-LEAKS-THE-GLOBAL-LOG-CALLBACK]] (P5)

### A.3 The capability PR

#### plan 09.5 tier skeleton + C harmlessness proof — 4

- [[D-DSSHIR-CSUBSET-BYTE-IDENTICAL-NOOP]] (P3)
- [[D-DSSHIR-DISABLED-PATH-COMPLETE]] (P3)
- [[D-DSSHIR-FACETS-INDEPENDENT]] (P3)
- [[D-DSSHIR-TIER-PLACEMENT]] (P3)

#### plan 16 codesign — 1

- [[D-CRYPTO-SHA256-HOME-CONVERGENCE]] (P2)

#### plan 28 boot half — 16

- [[D-BOOT-ARTIFACT-NOT-HOSTED-EXECUTABLE]] (P3)
- [[D-BOOT-EARLY-FAILURE-CONSOLE]] (P3)
- [[D-BOOT-ENTRY-VERBS]] (P3)
- [[D-BOOT-FLAT-CONTAINER]] (P3)
- [[D-BOOT-HANDOFF-PROTOCOL-VERSIONED]] (P3)
- [[D-BOOT-IBOOT-CONTRACT]] (P3)
- [[D-BOOT-IMAGE-READER-SHARED-WITH-FFI]] (P3)
- [[D-BOOT-INSTALL-REQUIRES-SYSTEM-PRIVILEGE]] (P3)
- [[D-BOOT-NO-SELF-INSTALLING-ARTIFACT]] (P3)
- [[D-BOOT-NONHOSTED-SPINE]] (P3)
- [[D-BOOT-OVMF-VM-LEG]] (P3)
- [[D-BOOT-PAYLOAD-BINDING]] (P3)
- [[D-BOOT-PAYLOAD-CHAIN-VERIFICATION]] (P3)
- [[D-BOOT-PE-BASE-RELOCATIONS]] (P3)
- [[D-BOOT-SECUREBOOT-KEY-ENROLLMENT]] (P3)
- [[D-BOOT-VEHICLE-ENUMERATOR]] (P3)

### A.4 The wasm32 PR

#### plan 18 wasm32 — 1

- [[D-FFI-STDINT-PTR-WIDTH-ILP32]] (P2)

### A.5 The C++ PR

#### C++ front end — 2

- [[D-CONFIG-INLINE-ASM-TEMPLATE-NUMBERING-HAS-NO-DECLARED-OWNER]] (P1)
- [[D-DEPS-SOURCEMERGE-INHERITS-THE-CONSUMERS-COMPILATION-ENVIRONMENT]] (P2)

### A.6 Opened by a trigger

#### plan 09.5 reclamation lattices — 10

- [[D-DSSHIR-CYCLE-TYPE-GRAPH-SCC]] (P3)
- [[D-DSSHIR-CYCLIC-REGION-RECLAIMED-AS-A-UNIT]] (P3)
- [[D-DSSHIR-DYNAMIC-OWNERSHIP-RECORD]] (P3)
- [[D-DSSHIR-MECHANISM-LATTICE]] (P3)
- [[D-DSSHIR-MODULE-OWNERSHIP-SIGNATURE]] (P3)
- [[D-DSSHIR-NO-CYCLE-COLLECTOR]] (P3)
- [[D-DSSHIR-NO-T5-TIER]] (P3)
- [[D-DSSHIR-RECLAMATION-FFI-BOUNDARY]] (P3)
- [[D-DSSHIR-TIER-CLASSIFICATION-ARTIFACT]] (P3)
- [[D-DSSHIR-WEAK-MUST-BREAK-AN-SCC]] (P3)

#### plan 09.5 runtime-services half — 3

- [[D-DSSHIR-SERVICE-MUST-ABSORB-A-DISCREPANCY]] (P3)
- [[D-DSSHIR-SERVICE-OWNERSHIP-SPLIT]] (P3)
- [[D-DSSHIR-WORKERS-NO-THREAD-FALLBACK]] (P3)

#### plan 17 GPU — 10

- [[D-GPU-ARTIFACT-RUNTIME-CODESIGN]] (P3)
- [[D-GPU-CPU-SIBLING-MANDATORY]] (P3)
- [[D-GPU-DISPATCH-FFI-LATE-BOUND]] (P3)
- [[D-GPU-IDENTIFIER-TOOLCHAIN-AVAILABILITY]] (P3)
- [[D-GPU-ISA-ABIMODEL-DIVERGENCE]] (P3)
- [[D-GPU-ISA-DRIVER-VERSION-DRIFT]] (P3)
- [[D-GPU-MIR-SIDECAR-FORMAT-STABILITY]] (P3)
- [[D-GPU-MIR-SIDECAR-SIDETABLE-ROUNDTRIP]] (P3)
- [[D-GPU-RUNTIME-COMPILER-MINIMAL-LIB]] (P3)
- [[D-GPU-TARGET-CONFIG-SUBDIR-RESOLUTION]] (P3)

#### plan 24 DSS Axis — 50

- [[D-AXIS-ANNOTATIONS-WITH-PARAMETERS]] (P3)
- [[D-AXIS-ASYNC-CONSTRUCTORS]] (P3)
- [[D-AXIS-ASYNC-DI]] (P3)
- [[D-AXIS-ASYNC-TASK-SHAPE]] (P3)
- [[D-AXIS-CLONE-DEEPCLONE]] (P3)
- [[D-AXIS-CLOSED-WORLD-REFLECTION]] (P3)
- [[D-AXIS-COLLECTION-OPERATORS]] (P3)
- [[D-AXIS-COMPILER-OWNS-MEMORY]] (P3)
- [[D-AXIS-CONCURRENT-BORROW-DYNAMIC-RECORD]] (P3)
- [[D-AXIS-CONTROL-AND-MEMORY-ARE-SEPARATE-AXES]] (P3)
- [[D-AXIS-DESTRUCTURE-SPREAD]] (P3)
- [[D-AXIS-DYNAMIC-CONSTRUCTORS]] (P3)
- [[D-AXIS-DYNAMIC-TIER-NEEDS-A-STORAGE-PROVIDER]] (P3)
- [[D-AXIS-EASY-AS-NODE-ERGONOMICS]] (P3)
- [[D-AXIS-ENGINE-KEEPS-GC-CAPABILITY]] (P3)
- [[D-AXIS-FFI-CONTRACT-VERIFICATION-ASYMMETRY]] (P3)
- [[D-AXIS-FFI-EXPORT]] (P3)
- [[D-AXIS-FFI-IMPORT]] (P3)
- [[D-AXIS-FFI-PARAMETER-CONTRACTS]] (P3)
- [[D-AXIS-GPU-BLOCK-LATTICE-PROFILE]] (P3)
- [[D-AXIS-GPU-EXECUTION-MODIFIER]] (P3)
- [[D-AXIS-GPU-FALLBACK-RESULT-EQUIVALENCE]] (P3)
- [[D-AXIS-GPU-NO-SILENT-FALLBACK]] (P3)
- [[D-AXIS-HIR-SOURCE-MUST-BE-HUMAN-AUTHORABLE]] (P3)
- [[D-AXIS-ISA-IS-NOT-INLINE-ASM]] (P3)
- [[D-AXIS-ISA-OPERATIONS-STAY-SEMANTIC]] (P3)
- [[D-AXIS-ISA-VOCABULARY-FROM-TARGET-CONFIG]] (P3)
- [[D-AXIS-MANAGED-LANG-PARITY]] (P3)
- [[D-AXIS-MEMORY-LATTICE-CONFIG]] (P3)
- [[D-AXIS-NATIVE-FLOOR-ADAPTIVE-CEILING]] (P3)
- [[D-AXIS-NO-MANUAL-ESCAPE-HATCH]] (P3)
- [[D-AXIS-NO-UNSAFE-BLOCK]] (P3)
- [[D-AXIS-NOT-GARBAGE-COLLECTION-VOCABULARY]] (P3)
- [[D-AXIS-OPEN-WORLD-INTERPRET-LIBS]] (P3)
- [[D-AXIS-OWNS-CYCLE-ANNOTATION]] (P3)
- [[D-AXIS-PAR-ATOMICS]] (P3)
- [[D-AXIS-PAR-CANCELLATION]] (P3)
- [[D-AXIS-PAR-CONCURRENT-COLLECTIONS]] (P3)
- [[D-AXIS-PAR-DUAL-API]] (P3)
- [[D-AXIS-PAR-LOCKS]] (P3)
- [[D-AXIS-PAR-OWNERSHIP-ACROSS-THREADS]] (P3)
- [[D-AXIS-PAR-SEMAPHORES]] (P3)
- [[D-AXIS-PAR-STRUCTURED-CONCURRENCY]] (P3)
- [[D-AXIS-PAR-SYNC-PRIMITIVES]] (P3)
- [[D-AXIS-PROOF-IS-AN-OPTIMIZATION-FACT]] (P3)
- [[D-AXIS-RESIDUAL-ENFORCES-NEVER-REFUSES]] (P3)
- [[D-AXIS-RUNTIME-AUTHORED-IN-HIR]] (P3)
- [[D-AXIS-RUNTIME-ENABLE-CONFIG]] (P3)
- [[D-AXIS-SELF-HOST-TRANSPILE]] (P3)
- [[D-AXIS-STATIC-BORROW-ZERO-COST]] (P3)

#### plan 27 GUI: the gui profile — 1

- [[D-AP2-COMPILATION-CONTEXT]] (P2)

#### plan 28 driver half — 19

- [[D-DRV-BUS-TRANSPORT-AXIS]] (P3)
- [[D-DRV-DARWIN-DRIVERKIT-ENTITLEMENT]] (P3)
- [[D-DRV-DARWIN-VM-LEG-EXTERNALLY-GATED]] (P3)
- [[D-DRV-DEXT-BUNDLE-PACKAGING]] (P3)
- [[D-DRV-DSSOS-NATIVE-VEHICLE]] (P3)
- [[D-DRV-ENTRY-VERBS]] (P3)
- [[D-DRV-EXECUTION-CONTEXT-EFFECT]] (P3)
- [[D-DRV-EXECUTION-MODE-AXIS]] (P3)
- [[D-DRV-FREESTANDING-LANGUAGE-PROFILE]] (P3)
- [[D-DRV-GRAPHICS-CLASS-SCOPE-LIMIT]] (P3)
- [[D-DRV-HOST-SERVICES-CONTRACT]] (P3)
- [[D-DRV-KERNEL-LIBRARY-ROLE]] (P3)
- [[D-DRV-LINUX-GPL-SYMBOL-GATING]] (P3)
- [[D-DRV-LINUX-KERNEL-ABI-PINNING]] (P3)
- [[D-DRV-OSDRIVER-CONTRACT]] (P3)
- [[D-DRV-VEHICLE-ENUMERATOR]] (P3)
- [[D-DRV-VM-LEG-HARNESS]] (P3)
- [[D-DRV-WINDOWS-ATTESTATION-SIGNING]] (P3)
- [[D-DRV-WINDOWS-INF-DRIVER-PACKAGE]] (P3)

