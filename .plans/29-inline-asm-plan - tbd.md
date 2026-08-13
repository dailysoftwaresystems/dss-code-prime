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
| Status | 🟡 **IN PROGRESS — ✅ P1+P2 LANDED 2026-08-12; ★★★ P2.5 + THE FIRST HALF OF P3/P4 LANDED 2026-08-12: A STANDALONE `.s` FILE COMPILES AND RUNS.** ✔MEASURED end to end — `examples/asm/asm_arith_return42/main.s` (hand-written AT&T assembly, no C anywhere) compiled through the `encode` tier and EXECUTED returning 42 on **pe64-x86_64 natively on Windows** and on **elf64-x86_64 under WSL**, at debug AND under `--config=release`. The examples-runner arm ledger reports `2 verified (2 ran)` on a Windows host with the two ELF arms correctly `skipped-by-runOn`. **What landed:** the seven shared line-structure rules in `asm.lang.json` (`asmLine` … `asmOperandSeq`); `asm-x86_64-att.lang.json` — a dialect document that writes a token table, a root and a nine-rule operand production and inherits ALL the line grammar through `languageReferences`; the `assembly` schema block (rule landmarks + ONE operand-order fact + the spelling→opcode vocabulary); `src/asm/asm_text_to_lir.cpp` (text→physical-register LIR, target-blind and dialect-blind); and the driver's `encode` route in `program.cpp` + `assembleAsmUnit`. ★★ **AND FOUR SUBSTRATE DEFECTS THE FIRST REAL CONSUMER EXPOSED, each a config wire that was accepted and then ignored:** the tokenizer HARDCODED the `"
"` lexeme ([[D-TOKENIZER-NEWLINE-LEXEME-HARDCODED]]); the parser keyed trivia on the CORE KIND rather than on the schema ([[D-PARSER-TRIVIA-KEYED-ON-CORE-KIND-NOT-CONFIG]]) — together these made `ret 
 ret` parse as ONE instruction, a wrong parse rather than an error; the entry-closure scoping from §4.2 was INCOMPLETE, leaking `semantics.inlineAsm` into a host that imports only the standalone surface ([[D-CONFIG-LANGREF-KEYWISE-BLOCK-IGNORES-ENTRY-CLOSURE]]); and `requires` was scoped per-DOCUMENT where the contract is per-ENTRY ([[D-CONFIG-LANGREF-REQUIRES-SCOPED-BY-DOCUMENT-NOT-ENTRY]]). ⏳ **STILL OPEN, STATED PLAINLY — the `.s` half is NOT finished:** no arm64 dialect ([[D-ASM-ARM64-DIALECT-UNWRITTEN]], so the reuse claim is untested by a second dialect and the qemu-arm64 leg has no asm example); no memory operands, branches or calls ([[D-ASM-ATT-GAS-SURFACE-INCOMPLETE]], [[D-ASM-INTRA-FUNCTION-LABELS]]); and `.s` still needs an explicit `--language` ([[D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET]]). Those four rows are the cycle's net **+4 OPEN**, which FAILS the anchor-balance gate — an operator decision, not a shrug. |
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
| ✅ **P2.5** — **DONE 2026-08-12** | `asm.lang.json` gains the shared standalone LINE STRUCTURE; a DIALECT document supplies the lexical surface, the root and the operand production, so a standalone `.s` PARSES | P1+P2 | ✅ MET, and by a running binary rather than by a tree dump. (a) the `encode` tier + name table + `static_assert`; (b) the entry-closure-scoped merge — shapes, rule-scoped ROWS **and** key-wise BLOCKS (§4.2, and the third of those was a second cycle's finding); (c) `kStandaloneOnlyDocumentKeys`; (d) the seven shared rules; (e) `asm-x86_64-att.lang.json`. ★★ **THE EXIT CLAUSE THAT WAS FLAGGED AS UNMEETABLE HERE STILL IS, AND THE REASON CHANGED:** *"both modes exercising the SAME shared rules"* — the embedded template is an opaque string until P5 parses its contents, so the two modes share the line-structure rules' NAMES but no rule instance. What DID become provable is the reuse property one tier down: the dialect writes ZERO line-structure rules. ⚠ And that is a ONE-dialect measurement — [[D-ASM-ARM64-DIALECT-UNWRITTEN]] is what would make it a claim |
| 🟡 **P3** — HALF DONE | the instruction/register/directive vocabulary a `.s` resolves against, and WHERE each half lives | P2.5 | ✅ **The standalone half is met:** mnemonics resolve per target through the dialect's `assembly.instructions[]` → `TargetSchema::opcodeByMnemonic`; registers through `registerByName`; directives through a CLOSED verb set; an undeclared spelling of any of the three fails loud naming BOTH the dialect and the target; agnosticism scan clean (no arch/format/language identity in the walker). ★ The architecture question this phase existed to settle is settled and RECORDED in §4.3: `.target.json` keeps vocabulary only, a DIALECT is a language. ⏳ **Not met:** the EMBEDDED-asm constraint letters (`=a`/`=d` x86, `=r` arm64) — those are P5's operand binding and were never reachable from the standalone path |
| ✅ **P4** — **DONE 2026-08-13, PROVEN ON BOTH CPUs BY EXECUTION** | asm TEXT reaches the assembler — text→LIR→bytes. Partially closes [[D-CSUBSET-INLINE-ASM-TEXT]] | P2.5 + P3 | ✅★★★ **arm64 CLOSED 2026-08-13 BY A RUNNING BINARY.** `examples/asm/asm_arm64_branch_call/main.s` — hand-written aarch64 GAS, no C — compiled to `arm64:elf64-aarch64-linux-exec` and **EXECUTED under qemu-aarch64 returning 42** (`file`: `ELF 64-bit LSB executable, ARM aarch64`). The exit code depends on a TAKEN branch, a NOT-TAKEN branch with an UNLABELED fallthrough between them, and a call to a second function in the same file — so a dropped CFG edge or a mis-elected opcode changes it. ★ **The second dialect was the experiment, and it returned a real result:** `asm.lang.json`’s seven shared line rules needed NOT ONE BYTE, but the text→LIR ENGINE needed SIX changes (sigil-less role ambiguity, dialect-dependent width source, absent operand roles, kind-vector election insufficiency, non-producing-opcode destination loss, width-absent variant matching any width — the last a live SILENT MISCOMPILE on arm64 `mov`). ⚠ “The shared GRAMMAR held” and “nothing had to change” are different claims; conflating them is the over-claim this plan keeps having to walk back. — ✅ **`examples/asm/asm_arith_return42/main.s` + `expected.json` COMPILES AND RUNS, exit 42, on pe64-x86_64 (native Windows) and elf64-x86_64 (WSL), debug AND release.** The arithmetic is three dependent steps in three opcode families (`sub`/`mul`/`xor`, all two-address) so any dropped or reordered instruction changes the exit code — deliberately not `mov $42; ret`, which passes even with every arithmetic instruction removed. ⏳ **NOT met:** elf64-**arm64** (needs [[D-ASM-ARM64-DIALECT-UNWRITTEN]]), and `__asm__("nop")` in the EMBEDDED mode (needs the template text parsed through the standalone instruction rules — P5). ⚠ The [[D-CSUBSET-INLINE-ASM-TEXT]] row therefore stays OPEN: standalone `.s` text now reaches the assembler; embedded template text does not |
| **P5** | extended asm reaches register allocation — closes [[D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED]] | P4 + the `D-TARGET-IMPLICIT-REGISTER-CONSTRAINT` reuse assessment | `hwtime.h` compiles; `--scanstatus` back on in `legs.json` with `requiredDefines` proving it took; `scanstatus`/`scanstatus2` execute in the corpus |

⚠ **TWO EARLIER `P3`/`P4` ROWS WERE REMOVED FROM THE TABLE ABOVE ON 2026-08-12, NOT SILENTLY DROPPED.** They were leftovers from the phasing that existed *before* the operator's second decision (asm is a real input language), and they CONTRADICTED the rows that replaced them: the old `P3` was blocked on `P2` and scoped to constraint letters only, the old `P4` asked for `__asm__("nop")` and nothing standalone. The surviving `P3`/`P4` are blocked on **P2.5** and carry the `examples/asm/` end-to-end criterion the operator asked for. Two rows for one phase is not history, it is an ambiguity about what “P4 is done” means.

### 4.2 ★★★ A reference imports the ENTRY'S CLOSURE, not the document (P2.5, 2026-08-12)

`mergeLanguageReferences` used to fold in **every** shape of a referenced document. That was
invisible while `asm.lang.json` had one surface; the standalone half makes it break three ways, and
**one of them is a silent miscompile of the host language**:

1. ★★★ **`pipelineEntry.byRule` rows are matched by rule NAME, and every language's root shape is
   named `root`** (`data.rootRule = rules->intern("root")`). `asm.lang.json` declares
   `{rule: "root", tier: "encode"}` for its own `.s` unit. Unscoped, that row merges into
   `c-subset.lang.json` and declares **C's translation unit** as entering at the assembler — every
   C file bypassing HIR, MIR and the optimizer. Not a broken build: a silent miscompile of an
   entire host language, produced by a config that never mentions C.
2. **`root` collides**, so the duplicate-shape guard fires on a host that asked for nothing of the
   kind.
3. The standalone rules name the referenced document's **own token kinds**, which the host's
   tokenizer does not have — dead rules making FIRST-set computation answer questions about a
   language the host does not speak.

⇒ the merge now walks the reachable closure of the declared `entry` over the JSON bodies
(pre-interning) and imports only those shapes **and only the rule-scoped rows whose rule is in that
closure**. It also makes `entry` mean something: importing everything else said that declaration
did not matter. ✔MEASURED after the change: `test_diagnostic_corpus`, `test_mir_lowering_c_subset`,
`test_grammar_schema` and `test_language_references` all green, i.e. the whole `asmStmt` tail chain
is reachable and C lost nothing.

⚠ **Reachability is a conservative string walk** (any string naming one of the referenced
document's own shapes). It cannot over-include, because a shape name and a token kind cannot
collide — the shadow guard forbids exactly that — and anything the walk cannot see is not a rule
reference.

### 4.3 P3 — ⚠ A DESIGN THAT WAS BUILT, REVIEWED, AND REVERTED THE SAME DAY (2026-08-12)

★★★ **A DIALECT IS A LANGUAGE, NOT A TARGET PROPERTY. An `asmSyntax` facet putting assembly
GRAMMAR into `.target.json` was implemented, reviewed by the operator, and reverted before it
shipped.** Recorded in full because the facet was *plausible* — it loaded, it was config-driven, it
had no identity branch, and it was wrong anyway.

**The disqualifying test, ✔MEASURED with gcc on one target:**

| | store | load |
|---|---|---|
| AT&T (`gcc -S`) | `movq %rsi, (%rdi)` | `movq (%rdi), %rax` |
| Intel (`gcc -S -masm=intel`) | `mov QWORD PTR [rdi], rsi` | `mov rax, QWORD PTR [rdi]` |

One target, one compiler, **two dialects** — differing in register prefix (`%` / none), immediate
prefix, comment character, **operand order**, memory-operand form (`(%rdi)` / `QWORD PTR [rdi]`)
*and mnemonic spelling* (`movq` / `mov` + a width annotation). Per
[[feedback_reference_compilers_are_the_spec]] both are things a `.s` may legally say, so Intel is
not optional forever. ⇒ **`registerPrefix` is a function of (target, dialect), not of target.** The
reverted facet worked only because exactly one dialect per target existed, i.e. it was a
per-(X,Y) fact stored per-X — the same duplication shape as the verbatim `_fstat$INODE64` binding.

⚠⚠ **AND THE JUSTIFICATION FOR ITS WORST PART WAS FABRICATED — this is the correction that matters
most.** The facet declared `destinationOperand` PER INSTRUCTION, justified in three places (a code
comment, both target configs, and this plan) by the claim *"that flag is already false for x86
store forms"*. ✔MEASURED above: **it is false.** AT&T is uniformly destination-LAST including
stores; Intel is uniformly destination-FIRST. Both dialects are internally uniform, so operand
order is **exactly one dialect fact**, and encoding it per-instruction meant every future Intel
row flipping N entries to express it. The claim was asserted without a probe, and it invented the
requirement it was used to satisfy — the failure [[feedback_verify_before_asserting_2026_07_26]]
exists to prevent, committed inside the very cycle that was correcting a different comment for the
same reason.

**THE SURVIVING FINDING, which is real and independent of the reverted shape:**
★★ `opcodes[].mnemonic` is NOT assembly. ✔MEASURED across both shipped targets — the 86 x86_64 and
80 arm64 entries are DSS's *virtual-ISA* names (`shr_l`, `shr_a`, `jcc`, `setcc`, `xor_rdx_zero`,
`fneg_mask`, `sub_sp_reg`, `store_outgoing_arg`). A real `.s` writes `movq`/`shrq`/`je`/`sete` or
`mov`/`lsr`/`b.eq`/`cset`. Some overlap by coincidence (`mov`, `add`, `ret`) — which is exactly
what would have made reusing them *look* like it worked. So the assembly spelling layer is real and
still needed; only its HOME was wrong.

**THE CORRECTED SHAPE (to build at P2.5/P3):**
* **`.target.json` — vocabulary only.** `opcodes[]` and `registers[]` already are this and need no
  change. ⚠ Narrow-width register NAMES (`eax`→`rax`@32, `w0`→`x0`@32) are genuinely target facts,
  but their LOOKUP is dialect-entangled (whether `%eax` strips its sigil before resolving is a
  dialect rule), so they land WITH the dialect work rather than ahead of it.
* **`<dialect>.asm.lang.json` — grammar.** Prefixes, comment syntax, operand ORDER (one
  declaration), the memory-operand production, and the mnemonic spelling table. A dialect is a
  language — which is the decision already taken when assembly was ruled its own source language —
  and `languageReferences` is how it reaches a shared asm core. ✔CONFIRMED the `shapes` DSL can
  express the memory-operand production: it has `sequence` / `alt` / `repeat` / `optional` plus
  rule and token references, which covers both `disp(base,index,scale)` and `[base, #off]`.

⇒ **the memory-operand divergence is NOT "blocked on a grammar that does not exist"** — the grammar
file and its mechanism shipped at P1+P2. It is unwritten content in an existing section.

<details><summary>The reverted facet's own rationale, kept so it is not re-proposed</summary>

### 4.3 P3 substrate: the `asmSyntax` target facet (REVERTED — see above)

★★ **The finding that shaped it: `opcodes[].mnemonic` is NOT assembly.** ✔MEASURED across both
shipped targets — the 86 x86_64 and 80 arm64 entries are DSS's *virtual-ISA* names (`shr_l`,
`shr_a`, `jcc`, `setcc`, `xor_rdx_zero`, `fneg_mask`, `sub_sp_reg`, `store_outgoing_arg`). A real
`.s` writes `movq`/`shrq`/`sarq`/`je`/`sete` (x86, AT&T) or `mov`/`lsr`/`b.eq`/`cset` (arm64). Some
overlap by coincidence (`mov`, `add`, `ret`), which is what would have made reusing them *look*
like it worked. Reusing them would mean a `.s` had to be written in DSS's private IR spelling —
not "a real input language", and a violation of [[feedback_reference_compilers_are_the_spec]] where
`gas` is the reference for what a `.s` may say.

⇒ `.target.json` gains `asmSyntax`: `registerPrefix` / `immediatePrefix` / `commentPrefixes` /
`instructions[]` / `registerAliases[]`. **This is the measured answer to §5's open question about
N configs per architecture, and it is "no":** x86_64 declares `%`, `$`, `#`, destination LAST;
arm64 declares bare registers, `#`, `//`, destination FIRST. Same grammar, same engine, **no
identity branch** — only these values differ, exactly as §1's table said ("dialect … / target
selection").

Two deliberate shapes worth not re-litigating:
* **`destinationOperand` is per-INSTRUCTION, not a per-target "AT&T puts it last" flag** — that
  flag is already false for x86 store forms, so a per-target rule would need re-litigating the
  first time one instruction disagreed with it.
* **`registerAliases` (`eax`→`rax`, `w0`→`x0`) are NOT rows in `registers[]`** — that array IS the
  allocatable register file (regalloc enumerates it; `registerInfo(ordinal)` is keyed by position),
  so adding `eax` there would invent a second allocatable register aliasing `rax` and corrupt
  allocation for **every** language. An alias resolves to the same ordinal and carries only its
  width. The loader refuses an alias that shadows a real register name, because
  `asmRegisterBySpelling` consults the register file first and such a row could never resolve.
* **No fall-through to `opcodeByMnemonic`** for an unknown assembly mnemonic. It would silently
  accept `shr_l` in a `.s`, and — worse — silently accept an x86 spelling on arm64 wherever the two
  vocabularies happened to collide.

⚠ **The spelling list's LENGTH is a vocabulary size, not a missing mechanism** (13 x86_64 / 10
arm64 today): an undeclared mnemonic is a precise diagnostic naming the target, so growing it is a
config edit with no code change. What is genuinely NOT yet modelled is the **memory-operand
grammar** — `disp(base,index,scale)` vs `[base, #off]` — which is real syntax rather than
vocabulary and is the one place the two dialects need different *rules*. It is out of the minimal
`.s` path (a function computing in registers and returning needs no memory operand) and must be
anchored rather than assumed.

</details>

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

### ✅ P2.5 DECIDED 2026-08-12 — `asmUnit` enters at a FOURTH tier, `encode`

**Operator decision:** add a fourth tier rather than redefine `lir`. `PipelineTier` is now
`{Hir, Mir, Lir, Encode}`; `lir` keeps its existing meaning (enter at **pre-regalloc, vreg-based**
LIR) and `encode` means **the assembler's input**. Config spelling `"encode"`.

★★ **THE QUESTION BELOW UNDERSTATED THE BOUNDARY, AND THE CORRECTION IS THE LOAD-BEARING PART.**
It said `asmUnit` must enter *after register allocation*. ✔MEASURED in `compile_pipeline.cpp`, a
`.s` must enter after **three** passes, not one — `allocateRegisters` (:956) reassigns the
programmer's registers, `legalizeTwoAddress` (:971) rewrites the instruction forms, and
`materializeCallingConvention` (:1008) **injects a prologue/epilogue over the one the programmer
wrote**. `src/asm/asm.hpp:42-49` states both halves as the assembler's own input contract ("every
vreg has been replaced by a physical register" AND "prologue/epilogue + frame_load/frame_store
materialized"). An implementer who took the old wording literally would have entered one pass too
early and had the callconv pass silently frame a hand-written function.

★ **The name is not invented:** `substrate::CompilePhase` already partitions the back half into
`LowerLir`, `Regalloc` ("liveness + allocation + rewrite + 2-addr legalize + callconv") and
`Encode` ("assemble to bytes"). `lir` is the entry before `Regalloc`; `encode` is the entry after
it. A tier with no real phase boundary behind it is a name that will start lying.

⚠ **AND THE SHIPPED COMMENT SAID THE OPPOSITE.** `pipeline_entry_config.hpp:16-22` read *"a `.s` …
is the post-register-allocation, physical-register machine tier THAT LIR MODELS"* — which
`compile_pipeline.cpp:912` contradicts in its own first line ("MIR → LIR (vreg-based)"). It was
never a live defect (`lir` was refused at load), but it sat exactly where P2.5's implementer would
read it. Corrected in place with the measurement above. Same failure family as the `unistd.json`
and UCRT-P5 cases: **a comment recording the full fact while the code uses half of it.**

<details><summary>The question as originally posed (kept — the reasoning is still the record)</summary>

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

</details>

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
