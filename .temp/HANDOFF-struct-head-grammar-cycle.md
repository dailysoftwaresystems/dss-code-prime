# HANDOFF — top-level struct/union/enum head grammar + schema-compiler FOLLOW fix

**Written 2026-06-17. Read this top-to-bottom; it assumes ZERO prior context.**
This is a mid-cycle handoff for an IN-PROGRESS `dss-cycle`. The work is committed +
pushed (see "Git state" below), full Windows ctest is **GREEN 317/317**, but the cycle
is NOT closed — there is remaining IN-SCOPE work (codegen + cleanup + cross-target witness
+ review/audit) before the anchor can be marked `✅ CLOSED`.

---

## 0. ENVIRONMENT — how to build/test/run (memorize this first)

- **Repo:** `C:\Source\DailySoftware\dss-code-prime` (Windows 11, PowerShell + Git-Bash both
  available). On another machine, clone + checkout branch **`feature/0-0-2-p9`**.
- **Build:** `cmake --build build` (MSVC, multi-config; artifacts in `build/bin/dss/Debug/`).
  If `build/` doesn't exist: `cmake -B build` first.
- **Full test suite:** `ctest --test-dir build -C Debug --output-on-failure`
  (the `-C Debug` is REQUIRED — MSVC is multi-config). **Current: 317/317 PASS.**
- **Run one gtest exe:** `build/bin/dss/Debug/dss_<suite>.exe [--gtest_filter='*Name*']`.
  Useful ones this cycle: `dss_hir_test_hir_lowering_c_subset`,
  `dss_analysis_syntactic_test_parser_cast_expr`, `dss_analysis_syntactic_test_parser_c_subset_smoke`,
  `dss_core_test_c_subset`, `dss_core_test_contextual_keywords`, `dss_analysis_syntactic_test_corpus`.
- **Compile one C file → native PE + run (x86_64 Windows):**
  ```
  build/bin/dss/Debug/dss-code-prime.exe --compile <file.c> --language c-subset \
     --target "x86_64:pe64-x86_64-windows-exec" --output <outdir>
  <outdir>/<file>.exe ; echo "exit=$?"
  ```
  (Flags: `--language` REQUIRED; `--output <dir>` (NOT `-o`); target spec MUST have the `:`).
- **Corpus example runner (drives full Program::compileFiles for all 4 targets):**
  `build/bin/dss/Debug/dss_examples_runner.exe "<ABSOLUTE path to examples/c-subset/<name>>"`
  On Windows it RUNS the x86_64-PE artifact and asserts exit/stdout from `expected.json`;
  the elf/macho artifacts are produced but `runOn` excludes the Windows host (INFO, not fail).
- **arm64 runtime witness (NOT on Windows host):** build under WSL (`build-wsl/`) and run the
  examples_runner under qemu with **`QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` EXPORTED** (else
  exit 255 — PT_INTERP needs the prefix; it's a harness-env quirk, NOT codegen). macOS/Mach-O
  = the `macos-latest` CI leg post-push.
- **★ SCHEMA IS LIVE-LOADED:** `src/dss-config/sources/*.lang.json` is read from `src/` at
  RUNTIME — editing a `.lang.json` takes effect with NO rebuild (just re-run the exe). C++
  changes DO need `cmake --build build`. **Caveat:** the test loader `loadShipped()` is
  STRICTER than the CLI — it rejects `C_AmbiguousAlternatives` the CLI tolerates (bit us once;
  see lesson L2).

---

## 1. THE TASK & how we got here

**Anchor being closed:** `D-CSUBSET-STRUCT-BODY-VARDECL-POSITION` (in
`.plans/_deferred-anchor-registry.md`) — the last pre-FC8 found issue. Make top-level
struct/union/enum **BODY** heads (`struct P { int a; } v;`, `struct P {…};`) and **bare
tag-REF** heads (`struct Tri mkTri(void)`, `struct Q g;`) parse + compile + run, PLUS
multi-level / recursive nested-body struct fields (`struct Outer { struct Inner {…} in; } …`).

**Standing user directive:** "loop dss-cycles until FC8, do as many as needed." Then for THIS
cycle, via AskUserQuestion the user chose **"Full fix now (dedicated multi-part cycle) — fix
the parser/grammar AND the codegen gaps"** and **"consider examples with multi level or even
recursive levels to work too, make this work!"**. Mid-cycle, when a deep pre-existing parser
bug surfaced (enum trailing-comma under speculation), the user chose **"Fix the schema-compiler
bug too"** (the root-cause FIRST/FOLLOW fix) over the pragmatic pin.

**The two scoped codegen gaps (from the original §B):** (A) aggregate-global-init→rodata =
IN SCOPE (still TODO — see §4). (B) zero-init global→BSS = OUT of scope (separate pre-existing
anchor `D-LK4-RODATA-PRODUCER-BSS-EMIT`, affects scalar `int x;` too — witness via INITIALIZED
globals only).

**Baseline before this cycle:** commit `d8ebf96` (316/316 Win + 316/316 build-wsl).

---

## 2. WHAT IS DONE (committed) — every change, with WHY

8 src files + 1 new example. All changes are committed (see §Git). Full ctest 317/317.

### 2a. Grammar — `src/dss-config/sources/c-subset.lang.json`
- **`topLevel`** (~235): REMOVED the `structDecl`/`unionDecl`/`enumDecl` alts. Now
  `[ includeDirective, typedefDecl, topLevelDecl, externDecl ]`. (Folding the bare top-level
  composite defs into `topLevelDecl` avoids a 2-level speculation that was unsound.)
- **`topLevelDecl`** (~321): `initDeclaratorList` is now **OPTIONAL** — a bare `struct P {…};`
  is head + NO declarator + `;`.
- **`topLevelHead`** (~325): inner alt is now `[ typeSpecifierSeq, topLevelCompositeSpec,
  Identifier ]`. **`typeSpecifierSeq` is DIRECT** (NOT wrapped) so a plain `int x;` keeps its
  pre-existing parse-tree shape (this is why the int-shape tests pass unchanged). The 3 FIRST
  sets are pairwise disjoint → the OUTER alt is non-speculative.
- **NEW rule `topLevelCompositeSpec`** (right after topLevelHead): the SPECULATIVE
  `[ structSpecifierBody, structTypeRef, unionSpecifierBody, unionTypeRef, enumSpecifierBody,
  enumTypeRef ]`, lookahead 256. Body forms probe first; a ref with no `{` rolls back to
  `*TypeRef`. MUST be a NAMED rule (inline at this depth fails — lesson L1).
- **`enumSpecifierBody`** (~493): REVERTED to the ORIGINAL `enumerator (Comma enumerator?)*`
  form. (We tried `(Comma enumerator)* Comma?` — rejected by `loadShipped` as ambiguous. The
  real fix was the schema compiler, §2d.)

### 2b. Semantic — `src/analysis/semantic/semantic_analyzer.cpp`
- **fieldIndex densification** (the type-composition chokepoint, ~1827, inside the
  `decl.fieldChildren` block): after `std::sort(fields…)`, renumber EACH field symbol's
  `fieldIndex` to its dense position `i` (`s.symbols.at(fields[i].sym).fieldIndex = i`).
  `FieldEntry` gained a `SymbolId sym;` member, pushed at collection (~1824).
  **WHY:** Pass-1 stamps `fieldIndex = bindings.size()` (TOTAL bindings in scope). A nested
  inline `struct Inner {…}` tag binds into the struct scope BETWEEN two fields, inflating the
  later field's ordinal (in=0, z=2). The interned struct packs fields DENSELY (positions 0,1),
  and 3 consumers read `fieldIndex` as an absolute index (HIR MemberAccess payload at
  cst_to_hir.cpp:1796; union variant; designator path). Densifying at the chokepoint makes
  `symbol.fieldIndex == type field position` by construction. This was a LATENT bug my feature
  exposed (the `MemberAccess … out of bounds` H_VerifierFailure).
- **NEW free function `floatToNamespaceScope(s, cfg, tree, scope)`** (defined just before
  `pass1`, ~1264): walks a scope reference up past any **declarator-dominator** scope (a
  `declByRule` scope whose `declarations` row has NO `fieldChildren`, e.g. `topLevelDecl`)
  to the nearest block/file scope. STOPS at a composite-body scope (member namespace), a
  block, or the file/CU root. Driven by `declByRule`/`fieldChildren` membership — NO rule-name
  branch. Used at:
  - **the composite tag bind site** (~1430): `ScopeId const bindScope =
    decl.fieldChildren.has_value() ? floatToNamespaceScope(…current) : current;`
  - **the enum enumerator republish** (~1974): `floatToNamespaceScope(…, structScope.parent)`.
  **WHY:** `topLevelDecl` IS a scope (it dominates a function's params across declarator+body;
  its `$scopesComment` literally assumed "nothing binds into the scope"). A composite TAG (or
  enum enumerator) minted in a FILE-SCOPE decl's head (`struct P {…} v;`) would otherwise bind
  into that transient param-dominator scope and VANISH when it pops — invisible to the next
  declaration (the `S_UnknownType`/`S_UndeclaredIdentifier` we saw). C11 6.2.1: tags belong to
  the enclosing block/file scope.

### 2c. HIR — `src/hir/lowering/cst_to_hir.cpp`
- **NEW member `findCompositeSpecifierIn(node)`** (just before `lowerTopLevelInto`): DFS for
  the first descendant whose rule is a `fieldChildren` (composite) Type declarations row.
- **`lowerTopLevelInto` no-declarator branch** (after the `isFn` block, before the globals
  loop): if there are NO named declarators and it's not a function, emit a `TypeDecl` from
  `findCompositeSpecifierIn(node)` (reusing `lowerTypeDecl`). **WHY:** a bare `struct P {…};`
  is now a `topLevelDecl` (hirKind `Decl`) with empty declarators, not the retired `structDecl`
  (hirKind `TypeDecl`). Without this it emits NOTHING → the `D5_*` HIR tests that assert a
  `TypeDecl` failed. A head that introduces no tag (`int ;`) → no composite spec → nothing
  emitted (the semantic tier owns the "declares nothing" diagnostic — see §4.3 TODO).

### 2d. ★ Schema compiler (THE ROOT FIX) — `grammar_schema_json.cpp` + `compiled_shape.hpp`
- **NEW pass `recomputeAltExpectedSets(data)`** (`grammar_schema_json.cpp`, defined just before
  `computeFollowSets`; CALLED in the orchestration ~2786 between `computeNullableTails` and
  `computeFollowSets`): a monotone fixed-point that re-unions every AltChoice position's
  `expectedSet = ∪ branches[i].expectedSet` until stable.
- **NEW mutator `Position::setExpectedSet(...)`** (`compiled_shape.hpp`, next to
  `setNullableTail`).
  **WHY (this is the crux — read carefully):** AltChoice `expectedSet` was computed EAGERLY at
  build time. `repeat` uses a "tie-the-knot": it reserves the loop-entry slot as a placeholder
  `End` (EMPTY expectedSet), builds the loop BODY against it, THEN overwrites it with the real
  AltChoice. An `optional`/`alt` INSIDE the body whose fall-through `cont` is that loop entry
  therefore captured the placeholder's EMPTY set — permanently missing the loop's exit FOLLOW.
  Concretely: the enum `enumerator (Comma enumerator?)*` before `}` — the trailing
  `{optional enumerator}` never learned `}` could follow it, so under a SPECULATIVE probe
  (`topLevelCompositeSpec`/`typedefHead`) the body parse hit `P_NoAlternativeMatched`
  (`nullableTail=false` because `}` was required after, and `}` not in the optional's
  expectedSet) → the probe rolled back to the ref form → `{…}` mis-parsed as a function body
  block. The fixpoint propagates the finalized loop-entry expectedSet back. Ordering matters:
  AFTER `detectAmbiguousAlternatives` (so the more-complete sets can't manufacture a false
  FIRST/FIRST overlap) and BEFORE `computeFollowSets` (which reads expectedSets). This ALSO
  fixed a pre-existing bug: `typedef enum { A, B, } T;` (trailing comma) never parsed before.

### 2e. Binder sketch (parse-time type-name oracle) — `binder_sketch.{hpp,cpp}` + `parser.cpp`
- Float **TYPE** bindings past declarator-dominator scopes (the parse-time mirror of
  `floatToNamespaceScope`). Added: `dominatorScopeRules_` set (scope rules whose `BinderDecl`
  is NOT a Type — computed in ctor), a `liveScopeIsDominator_` bool stack parallel to
  `liveScopes_`, `openScope(RuleId rule)` (was `openScope()`; parser.cpp:1053 updated to pass
  `rule`), `record()` walks live scopes back-to-front skipping dominators for TYPE bindings,
  and `Snapshot.liveScopeDominator` for speculation rollback.
  **WHY:** `globalTypeNames()` returns bindings at `scope==0`. A file-scope `struct S {…};`
  tag is recorded as `structSpecifierBody`'s frame closes WHILE `topLevelDecl`'s scope is still
  live → it bound at scope 1, dropped from the export surface (`GlobalTypeNamesExported` test
  failed: got 1, expected 2). Value bindings (params, locals) do NOT float — they stay local.

### 2f. Example — `examples/c-subset/struct_body_nested/`
- `main.c`: `typedef struct { struct Inner { int x; int y; } in; int z; } Outer;` + a local
  body-head `struct Pt { int a; } p;`, `return o.in.x + o.in.y + o.z + p.a;` == 42.
- `expected.json`: exit 42, 4 targets + a release `optimizedPipelines` arm.
- WITNESSED exit 42 on x86_64-PE. (arm64 qemu witness still TODO — see §4.7.)

---

## 3. WHAT WORKS NOW (witnessed, x86_64-PE native, exit 42 unless noted)

- Top-level `struct`/`union`: bare def (`struct P {…};` → TypeDecl), headed-with-declarator
  (`struct P {…} v;`), tag-ref-as-return-type (`struct Tri mkTri(void){…}`). [ref-return RUNS
  exit 42 via `/tmp/tl_fn.c`; bare-def + headed-global PARSE+typecheck — headed *global* needs
  §4.2 codegen to RUN].
- **Multi-level / recursive nested-body struct fields** — `struct_body_nested` exit 42.
- Top-level `enum E {…};` + **trailing comma** — parses, 120/120 c-subset HIR tests.
- `typedef enum {…,} T;` trailing comma — now parses (pre-existing bug fixed by §2d).
- Full Windows ctest **317/317** (was 316 baseline; +1 = struct_body_nested).

---

## 4. WHAT IS PENDING — the EXACT next steps, in order

### 4.1 Re-confirm the baseline on the new machine
`cmake --build build && ctest --test-dir build -C Debug --output-on-failure` → expect 317/317.
(There is an OLD unrelated `git stash@{0}` on `main` — IGNORE it, not part of this work.)

### 4.2 ★ encodeAggregateToBytes — IN-SCOPE codegen gap (user's "fix codegen gaps")
Make an INITIALIZED struct global run: `struct P { int x; int y; } v = { 20, 22 };`.
Today it fails loud at `D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL` (`src/asm/asm.cpp` ~622, the
`primitiveByteSize` miss arm in `lowerMirGlobalsToDataItems`).
- **Plan:** change `lowerMirGlobalsToDataItems` signature (asm.cpp ~516, decl asm.hpp ~412) to
  also take `std::optional<AggregateLayoutParams>` + `DataModel`; the SOLE caller is
  `compile_pipeline.cpp` ~451 (`target`/`format` are in scope there — see how `analyze()` at
  ~158 reads `format.dataModel()` + `target.aggregateLayoutLoaded()/aggregateLayout()`).
- Add a recursive `encodeAggregateToBytes(ty, MirLiteralValue, interner, lp, dm, buf, base)`
  that MIRRORS `collectLeaves` in `src/core/types/aggregate_abi.cpp:35` (struct/union via
  `computeLayout(...)->fieldOffsets`, array via element stride `elemLay->size`, scalar leaf
  writes LE bytes by `scalarByteSize`). `computeLayout(TypeId, interner, AggregateLayoutParams,
  DataModel)` → `StructLayout{size, align, fieldOffsets}` (`type_layout.hpp`). The aggregate
  init is a `MirAggregateValue { std::vector<MirLiteralValue> fields }` variant arm of
  `MirLiteralValue` (`mir_literal_pool.hpp`; recursive — nested aggregates nest). Dispatch on
  `std::holds_alternative<MirAggregateValue>(v.value)` BEFORE the `primitiveByteSize` gate
  (mirror the `std::string` arm at ~608). Reuse the existing scalar float-narrow logic
  (~640-672) for F32/F64 leaves; F16/F128 stay fail-loud.
- This CLOSES `D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL` and lets `struct P {…} v = {…};` /
  `struct Q g = {…};` RUN. Zero-init (`struct P v;`) stays fail-loud under
  `D-LK4-RODATA-PRODUCER-BSS-EMIT` (OUT of scope — witness via INITIALIZED globals only).

### 4.3 "declares nothing" fail-loud (C6.7p2)
`int ;` (no declarator, head introduces no tag) must emit `S_DeclarationDeclaresNothing`.
Verify whether the semantic analyzer already diagnoses it (there's an
`S_DeclarationDeclaresNothing` emit at semantic_analyzer.cpp ~1346 for abstract declarators in
named positions). If a bare `int;` slips through silently, add the diagnostic. (The HIR
no-declarator branch in §2c emits nothing for a non-composite head — correct, the diagnostic
is the semantic tier's job.)

### 4.4 Dead-rule cleanup
`structDecl`/`unionDecl`/`enumDecl` rules are now UNREFERENCED in `shapes` (removed from
`topLevel`; NOT used by any statement rule — verified). They still have semantics rows
(~1018-1027), hirKind rows (~1187-1189), and `scopes` entries (~1053). The grammar LOADS fine
(harmless dead config), but for no-dead-config either delete the rules+rows OR add a
`$comment` that they're retained intentionally. Decide + do.

### 4.5 Pin enum→int conversion (NEW anchor, pre-existing gap)
`return BLUE;` (BLUE an enumerator, fn returns int) → `S_ReturnTypeMismatch` (S0008). This is
PRE-EXISTING (affects `typedef enum {…} Color;` too — verified) — `isAssignable`
(`type_rules.hpp` ~114) has NO enum↔int arm. It is ORTHOGONAL to this anchor (enum VALUE
semantics, not head PARSING). Register a NEW anchor `D-CSUBSET-ENUM-INT-CONVERSION` (open,
fail-loud, pre-existing). Enum is witnessed at the DECLARATION tier (120 HIR tests: parse,
values computed, bare-name use, trailing comma); struct carries the RUNTIME witness.
**If the user wants enum values usable** (a follow-up, NOT required to close this anchor): add
an `isAssignable` Enum↔int arm gated like `charConvertsToArith` (the char arm ~159 is the exact
template, closed `D-CSUBSET-CHAR-INT-WIDENING`), + HIR `coerce()` Enum→I32 cast + MIR `mapCast`
Enum↔I32 (likely identity — enum is I32-backed via `enumType(name, TypeKind::I32)`).

### 4.6 Tests / corpus (strict, red-on-disable)
- NEW corpus `examples/c-subset/struct_body_top_level/`: bare `struct P {…};` (TypeDecl) +
  `struct Tri mkTri(void){…}` (ref-return, RUNS) + (once §4.2 lands) an INITIALIZED struct
  global `struct P {…} v = {…};`. exit 42, 4 targets, release arm.
- NEW corpus for enum top-level (a TypeDecl + trailing comma; runtime use blocked by §4.5).
- A **schema unit pin** (red-on-disable for §2d): assert that for an `optional`-inside-`repeat`
  before a required closer, the optional position's `expectedSet` includes the closer token —
  i.e. disable `recomputeAltExpectedSets` and watch it go red. (Look at
  `tests/core/.../grammar_schema*` for the pattern.)
- Tree-shape pins: `struct P {…};`→TypeDecl vs `struct P {…} v;`→Global (the optional-
  declarator disambiguation). MIR/byte pins for the aggregate global once §4.2 lands.

### 4.7 Cross-target witness
arm64-ELF via build-wsl + qemu (`QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` EXPORTED) for
struct_body_nested + the new corpus. macOS/Mach-O = the `macos-latest` CI leg post-push.

### 4.8 Gate → review → audit (dss-cycle Steps 3.5/5/6/8.5)
This is a SUBSTRATE change (schema compiler + semantic scoping) → it WARRANTS a Step-3.5
plan-lock-style independent review of the *approach* AND a Step-8.5 `dss-audit` of the built
diff (run as INDEPENDENT subagents — `pr-review-toolkit:review-pr` for code review, the
`dss-audit` skill lens for the self-audit). Specifically have them scrutinize: the
`recomputeAltExpectedSets` fixpoint (blast radius = EVERY grammar's expectedSets — the 317/317
green across toy/tsql/c-subset is the safety net, but verify no FOLLOW/recovery regression),
the `floatToNamespaceScope` agnosticism (no rule-name branch — driven by declByRule/
fieldChildren), and the binder-sketch float (cast-disambiguation correctness).

### 4.9 Cross-plan + memory + commit + push (Step 8)
- `_deferred-anchor-registry.md`: mark `D-CSUBSET-STRUCT-BODY-VARDECL-POSITION` `✅ CLOSED`
  (with the closing commit) ONLY after §4.2 + witnesses land; ADD `D-CSUBSET-ENUM-INT-CONVERSION`
  and (if you split it) a schema-fix anchor `D-PARSE-SCHEMA-NESTED-NULLABLE-FOLLOW` (✅ closed
  this cycle — the recomputeAltExpectedSets fix).
- plan-00 §0 status table + §0.1 stepper (flip + ctest count); plan-23 §3.1 FC-detail row.
- Memory entry (the running cycle-log convention; see `memory/MEMORY.md` + the per-topic files).
- Then FC8 is the next boundary (a big feature cluster — likely a §B/substrate sign-off first).

---

## 5. LESSONS / GOTCHAS discovered this cycle (don't re-learn these)

- **L1 — inline speculative alt fails at the top-level-decl depth.** An inline
  `{alt:[…], speculative:true}` at the topLevelHead position hits the FC4-c1 depth-0 candidate
  replay (parser.cpp ~1504) which forces the first candidate non-speculatively → no rollback to
  the ref form. A **NAMED** speculative rule (`topLevelCompositeSpec`, like `typeSpecifierForDecl`
  / `typedefHead`) works. This is why `topLevelCompositeSpec` is a standalone rule.
- **L2 — `loadShipped` (test loader) is STRICTER than the CLI.** The CLI tolerated the
  ambiguous `(Comma enumerator)* Comma?` enum reformulation (parsed fine); `loadShipped`
  rejected it (`C_AmbiguousAlternatives`) → the c-subset HIR suite died with
  `loadShipped(c-subset) failed`. ALWAYS validate grammar changes against the test loader, not
  just a CLI compile.
- **L3 — the schema computes `expectedSet` EAGERLY but `nullableTail` via a FIXPOINT.** Forward
  references (the `repeat` tie-the-knot loop entry) leave eager expectedSets stale-empty. The
  new `recomputeAltExpectedSets` fixpoint is the structural twin of `computeNullableTails`.
- **L4 — `topLevelDecl` is a SCOPE (param dominator).** Anything minted in its HEAD (composite
  tags, enum enumerators) must FLOAT to the enclosing namespace — done in BOTH the semantic
  analyzer (`floatToNamespaceScope`) AND the binder sketch (parse-time). Forgetting either gives
  invisible tags (semantic) or a missing export-surface entry (sketch).
- **L5 — `SymbolRecord::fieldIndex` must be the DENSE type-position.** It was the total scope
  binding count; a nested inline type tag interleaved among fields inflates it. Densify at the
  single type-composition chokepoint, never trust the bind-time ordinal.
- **L6 — runtime witness needs INITIALIZED globals.** Zero-init globals (scalar AND struct) hit
  the separate BSS anchor `D-LK4-RODATA-PRODUCER-BSS-EMIT` (out of scope). Use `= {…}` inits.

---

## 6. Git state at handoff
- Branch `feature/0-0-2-p9`. This work is committed on top of `d8ebf96` (the cycle baseline)
  and pushed. See `git log --oneline -3` for the exact commit hash of this handoff.
- Files changed (src): `analysis/semantic/semantic_analyzer.cpp`,
  `analysis/syntactic/{binder_sketch.cpp,binder_sketch.hpp,parser.cpp}`,
  `core/types/{compiled_shape.hpp,grammar_schema_json.cpp}`,
  `dss-config/sources/c-subset.lang.json`, `hir/lowering/cst_to_hir.cpp`. Plus the new
  `examples/c-subset/struct_body_nested/`.
- Older scoping notes from earlier in this cycle (superseded by THIS file, but kept for the
  archaeology): `.temp/full-fix-struct-head-grammar.md`, `.temp/next-cycle-struct-head-grammar.md`.

**RESUME AT §4.1, then §4.2.** The grammar+semantic+schema spine is DONE and green; the
remaining work is the aggregate-global codegen (§4.2), the small cleanups (§4.3/4.4/4.5), the
corpus + cross-target witnesses (§4.6/4.7), and the review/audit/cross-plan close (§4.8/4.9).
