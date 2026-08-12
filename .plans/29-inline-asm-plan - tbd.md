# Inline Assembly — Sub-Plan (29)

> **Graduated from `.temp/PLAN-inline-asm-arc-2026-08-12.md` on 2026-08-12, when P1 opened**, per that
> document's own instruction ("Graduate this into a numbered plan when P1 opens"). `.temp/` is gitignored
> scratch; this file is the durable owner. The architecture below is an **operator decision taken
> 2026-08-12**, recorded verbatim in the [[D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED]] registry row so it
> cannot be re-litigated by a later cycle.
>
> **The thesis in one line: assembly is a SOURCE LANGUAGE, not a C feature.**

## 0. Status (snapshot)

| | |
|---|---|
| Status | 🟡 **IN PROGRESS — ✅ P1+P2 LANDED 2026-08-12** (merged by operator decision, §4). ⏳ **P2.5 · P3 · P4 · P5 open**, each behind a NAMED prerequisite — and **P2.5 opens with an OPERATOR DECISION**, not with implementation (§5). ✔MEASURED at landing: the sqlite `hwtime.h:43` construct costs **53 diagnostics → 1** (`S0062`, quoting `outputs: "=a", "=d"`); the refusal moved **parse tier → semantic tier**; assembly became its own language (`src/dss-config/sources/asm.lang.json` — 12 rules over 2 rule holes + 13 token holes); and the reuse property was **TESTED, not asserted** (§4 exit criterion (c)). |
| Predecessors | ✅ [`05-parser-plan`](./05-parser-plan%20-%20ok.md) (predictive descent + FIRST-set alt dispatch) · ✅ [`08.6-semantic-plan`](./08.6-semantic-plan%20-%20ok.md) (the `pass2Post` facet walk P1's refusal lives in) · ✅ [`13-assembler-plan`](./13-assembler-plan%20-%20tbd.md) (the byte encoders P4 must reach) |
| Successors | ⏳ [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) register allocation — P5 binds asm operands to it |
| Scope | The GNU inline-asm surface end to end: syntax → its own language config → per-target vocabulary → real asm text → register allocation. |
| Why it exists | **Not** "make sqlite's Default config build" — that would be a poor reason for this much work. Extended asm is pervasive in systems C (every fast path, atomic, barrier and syscall stub), and the arc buys DSS a **cross-language embedding mechanism** it will want again (SQL-in-C, GLSL-in-C++, inline IL). |

---

## 1. The architecture (operator decision, 2026-08-12)

| piece | home | why there |
|---|---|---|
| asm **syntax** (template, operand list, clobber list) | `src/dss-config/sources/asm.lang.json` — a peer of `c-subset.lang.json` | assembly IS a language; its grammar belongs beside the other grammars, not nested inside C's |
| asm **vocabulary** (instructions, registers, **constraint letters**) | `.target.json` | it is per-CPU. ✔MEASURED: `=a`/`=d` are x86, `=r` is arm64 — *in the same header* |
| asm **dialect** (Intel vs AT&T, gas vs masm) | `asm.lang.json` / target selection | a property of the assembly language, which C has no business knowing |
| the **embedding** (`__asm__ (...)` in C) | a **cross-language reference** from `c-subset.lang.json` | the genuinely reusable half |

### Two corrections to the original sketch, both load-bearing

**1 — there is NO language-level `skipToAssembler`.** Right for *basic* asm, a **silent miscompile** for
*extended* asm: in `__asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi))` the operands are C lvalues with
register constraints, so the block **participates in register allocation**. Skip it to the assembler and the
allocator keeps something live in eax — corrupted, with no diagnostic. ⇒ **the tier boundary runs THROUGH
the construct**: the template text is opaque and belongs to the assembler; the operand bindings and clobber
set must survive as IR the allocator can see. The pipeline entry point is a property of the **construct**
(basic vs extended), never of the language.

**2 — constraint letters are TARGET vocabulary.** Not asm-language vocabulary, not C vocabulary. Declared in
`.target.json`, looked up by the engine, **undeclared letter ⇒ fail loud**. This is what keeps the whole arc
agnostic: no `if (arch == …)` anywhere.

---

## 2. The measured requirement set

✔MEASURED 2026-08-12 against upstream sqlite `a393a5e05d`, with a positive control that the subject really
was sqlite. `src/hwtime.h` carries **FOUR** `sqlite3Hwtime` variants — the header alone spans two
architectures and a multi-line template:

```
:35  x86     __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
:43  x86_64  __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
:51  arm64   __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (cnt));
:60          multi-line template
```

**Minimum viable constraint set: `=a`, `=d`, `=r`, `%0`-style operand substitution, multi-line templates.**
That `=a`/`=d` are x86 while `=r` is arm64 *in one header* is the direct evidence for correction 2.

### 2.1 The reference-compiler spec for the SYNTAX (✔MEASURED 2026-08-12)

gcc 13.3.0 · clang 18.1.3 · clang 19.1.1, `-std=gnu17 -fsyntax-only`, positive control + 6 negative controls,
**all three unanimous** except where noted. Per [[feedback_reference_compilers_are_the_spec]] this table *is*
the specification.

| property | verdict |
|---|---|
| qualifier slot | ORDER-FREE subset of `{volatile, inline, goto}`, any order, `__`-spellings included |
| `const` as a qualifier | **REJECTED** — gcc: *"'const' is not a valid 'asm' qualifier"* |
| duplicate qualifier | **REJECTED**, and `volatile __volatile__` counts as duplicate ⇒ dedup is by **KIND, not spelling** |
| non-goto section count | 0–3 colons OK; **4 colons REJECTED** |
| goto section count | **exactly 4**; 3 REJECTED, 5 REJECTED |
| `asm goto` with outputs | **ACCEPTED** (GCC 11+/clang) — there is no "goto implies empty outputs" rule |
| `asm goto` with NO labels | ⚠ **DIVERGENCE**: gcc REJECTS, both clangs ACCEPT. Moot for P1; a **§B decision at P5** |
| template | MANDATORY — `__asm__ ()` REJECTED |
| trailing comma in an operand list | REJECTED |
| duplicate clobbers | ACCEPTED (no dedup rule at the syntax tier) |
| operand value | a FULL expression — `"r"(a, b)` ACCEPTED, so the comma operator is legal inside the parens |
| symbolic operand names | `[out] "=r"(x)` ACCEPTED |
| empty-section forms | `("" : : )`, `("" : : : )`, `("" :: )`, `("" ::: )` all ACCEPTED — and they **build and RUN** |

### 2.2 ★★ The tokenization trap — `:::` is NOT three colons

`c-subset.lang.json` declares `"::"` → `ColonColonOp` (C23 attribute namespaces need it). Maximal munch
therefore applies inside the asm tail. ✔MEASURED against the real DSS binary:

- `("" ::: "memory")` → `ColonColonOp` + `Colon`
- `("" :: "r"(x))` → a single `ColonColonOp`

A grammar written as N `Colon` slots parses the sqlite shape and **fails the single most common inline-asm
idiom in systems C** (the `:::"memory"` compiler barrier). The resolution is in §3.

---

## 3. The grammar model — BOUNDARIES, not slots

A `Colon` advances one section; a `ColonColonOp` advances **two at once** and forces the section between them
empty (nothing can be written inside a two-character token). Sections are
`template [: outputs [: inputs [: clobbers [: labels]]]]`.

★★ **Every fused (`ColonColonOp`) arm is its OWN NAMED RULE.** This is not stylistic. An independent design
audit found that an inline fused arm whose label list is empty leaves **no node evidence at all**, making
`("" ::::)` indistinguishable from the valid `("" : : : )` — and ✔MEASURED, the three 4-colon-no-goto
spellings `("" : : ::)`, `("" :: ::)`, `("" ::::)` are **rejected by all three reference compilers**, so they
would have compiled clean. Naming the arm makes "is this section present?" a pure `RuleId` question, which is
also the tree's own convention (`nodeHasVisibleChildOfRule`, *"keyed on a config-resolved RuleId, never a
keyword"*). Section ROLE is likewise the parent rule: an `asmOperandList` under `asmOutputsTail` is outputs,
under `asmInputsTail`/`asmInputsTailFused` it is inputs.

Rules: `asmStmt`, `asmOutputsTail`, `asmInputsTail`, `asmInputsTailFused`, `asmClobbersTail`,
`asmClobbersTailFused`, `asmLabelsTail`, `asmLabelsTailFused`, `asmOperandList`, `asmOperand`,
`asmClobberList`, `asmGotoLabelList`. The qualifier run is `{repeat {alt [VolatileKeyword, InlineKeyword,
GotoKeyword]}}` — the shipped `arrayDeclSuffix` precedent (C99's `int a[static const 10]`).

**Predictive throughout**: every choice point is disjoint by token kind (`{Colon}` vs `{ColonColonOp}`;
`{BracketOpen}∪string-openers` vs `{Colon,ColonColonOp,ParenClose}`; `{V,I,G}` vs `{ParenOpen}`). No
speculation is introduced.

---

## 4. Phases — each blocked by a NAMED prerequisite, not by size

★★★ **OPERATOR DECISION 2026-08-12 — P1 AND P2 ARE MERGED. THE GRAMMAR NEVER LANDS IN `c-subset.lang.json`.**
The original phasing had P1 write the extended-asm grammar into C and P2 move it to `asm.lang.json`. The operator
rejected that sequencing on the grounds that it writes the grammar twice and passes through exactly the state
this arc exists to correct (*"a new source for asm inside `src/dss-config/sources` instead of inlining into C
language… then we use the file as reference"*). Asked whether to take the cheap route if the mechanism proved
expensive, the answer was explicit: **"whatever it costs"**, and the reason is the point of the whole design —
*"we don't have this mechanism, and is the only one acceptable, so asm language is reused."*
⇒ **The cross-language reference IS the deliverable, not a wrapper around one.** A merge-only shortcut that
happens to work for a single host language does NOT satisfy this decision: the mechanism must make `asm.lang.json`
a genuinely REUSABLE language, such that a second host (SQL-in-C, GLSL-in-C++, inline IL) references it with
zero new grammar. ✔MEASURED 2026-08-12 before the decision: no such mechanism exists today — `imports` is
`#include`/name-matching semantics, not a rule reference, and all three shipped `.lang.json` files are
self-contained.
⚠ **Consequence for ownership:** the asm language owns its OWN `semantics` — the refusal "extended asm is not
yet supported" is a property of the asm language's implementation maturity, not of C. A host language that
references asm inherits the refusal for free; that inheritance is what "reused" means and is the property to
test.

★★★ **OPERATOR DECISION 2026-08-12 (second) — ASM IS A REAL INPUT LANGUAGE, NOT ONLY AN EMBEDDED ONE.**
Asked whether `asm.lang.json` lowers through HIR/MIR/LIR or goes straight to the assembler, and whether a
standalone `.s` file is compilable, the operator answered: *"yes, it becomes a real input language too, as
c-subset is today, also, we need `examples/asm` as we do have `examples/c-subset` to prove it end-to-end.
Doesn't need to be ALL C examples, just need to prove asm works."*

⇒ **`asm.lang.json` has TWO entry points and must be a COMPLETE language config, not a set of holes:**

| mode | entry | who lexes | operands are |
|---|---|---|---|
| **embedded** (`__asm__(…)` inside C) | `asmStmt` | the HOST's tokenizer | HOST expressions (`"=a" (lo)` binds a C lvalue) — the `requires`/bind holes |
| **standalone** (`foo.s` compiled directly) | an `asmUnit` root | **asm.lang.json's OWN tokens** | asm's own operands (registers, immediates, labels, memory refs) |

The two modes share the token-name vocabulary: `requires.tokens` are satisfied by **asm.lang.json's own token
table** when standalone, and by the **host's bindings** when embedded. That symmetry is what keeps one
grammar serving both and is the design property to protect.

⚠ **HONEST SIZING — the standalone half is P3+P4, and it is the big one.** A running `examples/asm/` binary
needs the whole text→bytes path: mnemonics and operands parsed, resolved against the **`.target.json` opcode
vocabulary** (P3), then lowered into LIR for the existing encoders (P4). ✔MEASURED: `src/asm/` consumes
**LIR** (post-regalloc, physical-register-only — plan 13 §0) and has **no text entry point**; the
`D-CSUBSET-INLINE-ASM-TEXT` row calls this "the huge PER-TARGET arc". So the operator's request pulls P3 and
P4 forward into the same arc as the embedding work. It is not a reason to decline — it is a reason not to
pretend the embedding phase delivers it.

| P | what | prerequisite | exit criterion |
|---|---|---|---|
| ✅ **P1+P2** (merged) — **LANDED 2026-08-12** | `src/dss-config/sources/asm.lang.json` as a peer language carrying the full extended-asm grammar + the **cross-language reference mechanism** + a PRECISE fail-loud semantic refusal naming the unsupported constraints | **none — startable now** | (a) the pre-P1 cascade becomes **ONE** diagnostic and the functions AFTER the asm statement are still parsed normally; (b) the asm grammar has exactly ONE owner and `c-subset.lang.json` contains NO asm grammar rule; (c) ✅ **TESTED, NOT ASSERTED — AND IT PASSED:** a 2nd host (`toy.lang.json`) bound asm through `languageReferences` and ✔MEASURED **18 shapes before, 18 after — ZERO new grammar rules**, inheriting the asm language's OWN semantics for free (`S0057` non-empty template, `S0062` extended-asm refusal, `S0063` label-section-without-`goto`), which is what “reused” was supposed to mean. ⚠ **The binding was an EXPERIMENT and was REVERTED** — `toy.lang.json` ships with no `languageReferences`, so this claim rests on that measurement and NOT on shipped config; re-run it before re-quoting it. ★★ **AND THE DURABLE GUARD THAT ACTUALLY SHIPPED — added here 2026-08-12 after an independent audit found this criterion resting ENTIRELY on the reverted experiment, whose 18-shape number is unreproducible from the tree: cite THIS as the evidence, and keep the experiment's caveat as history.** ✔TREE `tests/core/test_language_references.cpp` — **14 tests**, registered at `tests/core/CMakeLists.txt:47-48` — supplies the second consumer PERMANENTLY instead of borrowing one: a genuinely non-C-shaped synthetic host schema built as a JSON string in-test (`AsmHostProbeIsNotCShaped` pins that it is not C in disguise), loaded through the REAL loader, resolving the REAL shipped `src/dss-config/sources/asm.lang.json`. It asserts all **12** asm rules arrive **COMPILED, not merely interned** — a NON-EMPTY FIRST set each, since a name-only check passes on a rule that was interned but left BODILESS — rebound to the HOST's own alien token kinds, with one walked to completion through the schema's own `enterRule`/`advance`/`leaveRule`; plus the `semantics.inlineAsm`, `hirLowering` and `pipelineEntry` companion rows arriving FROM the asm language rather than from the host; plus **three fail-closed red-on-disable arms exercised on every run** (`WithoutTheReferenceEveryAsmRuleIsAbsent`, `WithoutTheReferenceNoCompanionRowArrives`, `ReachingAsmWithoutTheReferenceFailsLoud`), the disable being a SECOND schema string in the SAME process, so no file is mutated and nothing must be restored. Row: [[D-TEST-LANGUAGE-REFERENCE-REUSE-UNGUARDED]]; **(c) is met by that suite, not by the experiment**; (d) ⚠ **PARTIAL — NOT MET AS WRITTEN. Corrected 2026-08-12 by the same audit; it had been left stamped as met.** red-on-disable *"remove the reference and C's asm stops parsing"* has **NO test against the shipped `c-subset.lang.json`**. The nearest proxy is `LanguageReferences.ReachingAsmWithoutTheReferenceFailsLoud`, which strips the reference from the in-test SYNTHETIC host and asserts the loud failure THERE — a real red arm for the MECHANISM, and a different claim from the shipped C config, where a deleted `languageReferences` entry is caught by nothing named here. ⇒ **owed: the same red-on-disable against shipped C.** Proven-on-the-synthetic-host is the honest status |
| **P2.5** | `asm.lang.json` gains its OWN lexical surface + an `asmUnit` root + an artifact profile, so a standalone `.s` file PARSES | P1+P2 | a `.s` file parses to a well-formed tree with no host language present — the referenced-vs-standalone symmetry proven by BOTH modes exercising the SAME shared rules |
| **P3** | constraint + **instruction/register** vocabulary in `.target.json` | P2.5 | `=a`/`=d` on x86_64, `=r` on arm64; mnemonics resolve per target; an undeclared letter or mnemonic fails loud; agnosticism scan clean |
| **P4** | asm TEXT reaches the assembler — text→LIR→bytes. Closes [[D-CSUBSET-INLINE-ASM-TEXT]] | P2.5 + P3 | ★ **`examples/asm/<name>/main.s` + `expected.json` COMPILES AND RUNS**, returning its asserted exit code on pe64 + elf64-x86_64 + elf64-arm64, debug and release — the operator's "prove it end-to-end". Plus `__asm__("nop")` running in the embedded mode |
| **P5** | extended asm reaches register allocation — closes [[D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED]] | P4 + the `D-TARGET-IMPLICIT-REGISTER-CONSTRAINT` reuse assessment | `hwtime.h` compiles; `--scanstatus` back on in `legs.json` with `requiredDefines` proving it took; `scanstatus`/`scanstatus2` execute in the corpus |

⚠ **TWO EARLIER `P3`/`P4` ROWS WERE REMOVED FROM THE TABLE ABOVE ON 2026-08-12, NOT SILENTLY DROPPED.** They were leftovers from the phasing that existed *before* the operator's second decision (asm is a real input language), and they CONTRADICTED the rows that replaced them: the old `P3` was blocked on `P2` and scoped to constraint letters only, the old `P4` asked for `__asm__("nop")` and nothing standalone. The surviving `P3`/`P4` are blocked on **P2.5** and carry the `examples/asm/` end-to-end criterion the operator asked for. Two rows for one phase is not history, it is an ambiguity about what “P4 is done” means.

★★★ **P5 CANNOT LAND WITHOUT ITS NEGATIVE PIN** (§D correctness-critical, silent-miscompile class): a program
that **corrupts** a value iff the clobber list is ignored — hold a live value in eax across an asm block that
clobbers it, and assert the value survives. Review-only is **not admissible** here.

### 4.1 Reuse to assess before P5 — the biggest unknown in the estimate

[[D-TARGET-IMPLICIT-REGISTER-CONSTRAINT]] (✅ CLOSED 2026-06-04) is already a **config-driven substrate for
"this operand must live in these specific registers"** — per-opcode register names with parallel ordinals,
built for x86 `idiv` RDX:RAX and shift-by-CL, explicitly with no target identity branch. Extended-asm operand
constraints are the **same shape**. If that reuse holds, P5's allocator-facing half is far less greenfield
than it looks. **Assess this before P5.**

---

## 5. Interactions and open questions

- **[[D-CSUBSET-INLINE-ASM-SPELLING]] (bare `asm`) is NOT closed by this arc.** ✔MEASURED across
  `c17`/`gnu17`/`c2x`/`gnu2x` on gcc and clang: `asm` is an ordinary **identifier** in ISO mode and a
  **keyword** in GNU mode (both compilers' default). DSS has **no standard-mode axis** to hang that on, and
  inventing a per-keyword boolean would be that axis in disguise. **§B when someone wants it.**
- **`asm goto` with no label section** is the gcc/clang divergence in §2.1. P1 refuses all `goto` forms
  anyway, so no side need be picked yet; **P5 must bring it to the operator as a §B**, because picking
  silently is exactly what [[feedback_reference_compilers_are_the_spec]] exists to prevent.
- **Does `asm.lang.json` need to be N configs per architecture?** The plan says no — syntax in `asm.lang.json`,
  vocabulary from the target — but that split is an **INFERENCE until P2 tests it**. If x86 and arm64 asm need
  genuinely different *grammars* rather than different *vocabularies*, P2's shape changes.
- [[D-CSUBSET-INLINE-ASM-OPERANDS]] and [[D-CSUBSET-INLINE-ASM-GOTO]] are folded into this arc (P1 grammar /
  P5 semantics) rather than left as parallel trackers. Both registry rows were updated 2026-08-12 to say the
  grammar half is done and the binding/CFG half is P5.

### ★★ P2.5 OPEN QUESTION — which pipeline tier does `asmUnit` enter at? (OPERATOR DECISION)

The `pipelineEntry` mechanism itself LANDED with P1+P2 — `src/core/types/pipeline_entry_config.hpp`, declared
**per RULE** (never per language, per §1 correction 1), over a **closed** tier vocabulary `hir` / `mir` / `lir`,
deliberately with **no `optimize` sibling key** because “no optimization” must be a CONSEQUENCE of the tier
rather than a second switch that can drift from it. The embedded `asmStmt` is settled. **The standalone
`asmUnit` is not**, and the reason is not obvious from the tier names.

A hand-written `.s` names **PHYSICAL registers**. `lir`, as this pipeline currently means it, does not.
✔MEASURED in `src/program/compile_pipeline.cpp`: `lowerMirModuleToAssembly` produces **vreg-based** LIR and
then runs wide-call argument splitting, liveness and `allocateRegisters` **over it** (the function's own
header states the order: MIR → LIR → liveness → regalloc → rewrite), while `src/asm/` consumes the
**post-regalloc, physical-register-only** LIR (plan 13 §0). ⇒ declaring `asmUnit` as `lir` would hand the
programmer's register choices to the allocator **to rewrite** — the same **miscompile of intent** as
optimizing the block, one step further down, and precisely the failure `pipelineEntry` was built to prevent.

⇒ **`asmUnit` must enter AFTER register allocation.** Whether that is spelled by REDEFINING `lir` as the
post-regalloc tier (and giving the pre-regalloc one another name) or by adding a **FOURTH tier name** is an
**operator decision at P2.5**: it changes a closed, already-shipped vocabulary, so it is not an implementer's
call to make quietly. Bring both options with their blast radius.

### `asmLabel` deliberately REMAINS in `c-subset.lang.json` (decision, 2026-08-12)

Recorded so a later cycle does not “tidy” it into the asm language on the strength of its name. `asmLabel` is
GCC's **declarator decoration** — `int gv __asm("myglobal")`, GCC 6.47.5 *Controlling Names Used in
Assembler Code* — and what it renames is a **C symbol**. There is no template, no operands, no clobbers; its
payload is a **symbol name, not assembly text**. Moving it into `asm.lang.json` would put a C declarator
feature into the asm language and **invert the thesis of §1** (assembly is a source language, not a bag of
constructs whose names contain “asm”). ✔TREE: `c-subset.lang.json` `shapes.asmLabel`, the `initDeclarator`
after-declarator run, and `semantics.asmLabelRule`; the feature itself is [[D-CSUBSET-ASM-LABEL-SYMBOL-RENAME]]
(✅ closed TF-C88). **The dividing line to apply to the next candidate: does the construct carry ASSEMBLY, or
does it carry a NAME?**

---

## 6. What this arc is NOT

Not "make sqlite's `build(Default)` compile". That is two corpus files of ~10,800 and would be a poor reason
to do any of this. See §0 "Why it exists".
