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
| asm **syntax** (template, operand list, clobber list) | `src/dss-config/sources/asm.lang.json` — a peer of `c.lang.json` | assembly IS a language; its grammar belongs beside the other grammars, not nested inside C's |
| asm **vocabulary** (instructions, registers, **constraint letters**) | `.target.json` | it is per-CPU. ✔MEASURED: `=a`/`=d` are x86, `=r` is arm64 — *in the same header* |
| asm **dialect** (Intel vs AT&T, gas vs masm) | `asm.lang.json` / target selection | a property of the assembly language, which C has no business knowing |
| the **embedding** (`__asm__ (...)` in C) | a **cross-language reference** from `c.lang.json` | the genuinely reusable half |

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

`c.lang.json` declares `"::"` → `ColonColonOp` (C23 attribute namespaces need it). Maximal munch
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

★★★ **OPERATOR DECISION 2026-08-12 — P1 AND P2 ARE MERGED. THE GRAMMAR NEVER LANDS IN `c.lang.json`.**
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
| ✅ **P1+P2** (merged) — **LANDED 2026-08-12** | `src/dss-config/sources/asm.lang.json` as a peer language carrying the full extended-asm grammar + the **cross-language reference mechanism** + a PRECISE fail-loud semantic refusal naming the unsupported constraints | **none — startable now** | (a) the pre-P1 cascade becomes **ONE** diagnostic and the functions AFTER the asm statement are still parsed normally; (b) the asm grammar has exactly ONE owner and `c.lang.json` contains NO asm grammar rule; (c) ✅ **TESTED, NOT ASSERTED — AND IT PASSED:** a 2nd host (`toy.lang.json`) bound asm through `languageReferences` and ✔MEASURED **18 shapes before, 18 after — ZERO new grammar rules**, inheriting the asm language's OWN semantics for free (`S0057` non-empty template, `S0062` extended-asm refusal, `S0063` label-section-without-`goto`), which is what “reused” was supposed to mean. ⚠ **The binding was an EXPERIMENT and was REVERTED** — `toy.lang.json` ships with no `languageReferences`, so this claim rests on that measurement and NOT on shipped config; re-run it before re-quoting it. ★★ **AND THE DURABLE GUARD THAT ACTUALLY SHIPPED — added here 2026-08-12 after an independent audit found this criterion resting ENTIRELY on the reverted experiment, whose 18-shape number is unreproducible from the tree: cite THIS as the evidence, and keep the experiment's caveat as history.** ✔TREE `tests/core/test_language_references.cpp` — **14 tests**, registered at `tests/core/CMakeLists.txt:47-48` — supplies the second consumer PERMANENTLY instead of borrowing one: a genuinely non-C-shaped synthetic host schema built as a JSON string in-test (`AsmHostProbeIsNotCShaped` pins that it is not C in disguise), loaded through the REAL loader, resolving the REAL shipped `src/dss-config/sources/asm.lang.json`. It asserts all **12** asm rules arrive **COMPILED, not merely interned** — a NON-EMPTY FIRST set each, since a name-only check passes on a rule that was interned but left BODILESS — rebound to the HOST's own alien token kinds, with one walked to completion through the schema's own `enterRule`/`advance`/`leaveRule`; plus the `semantics.inlineAsm`, `hirLowering` and `pipelineEntry` companion rows arriving FROM the asm language rather than from the host; plus **three fail-closed red-on-disable arms exercised on every run** (`WithoutTheReferenceEveryAsmRuleIsAbsent`, `WithoutTheReferenceNoCompanionRowArrives`, `ReachingAsmWithoutTheReferenceFailsLoud`), the disable being a SECOND schema string in the SAME process, so no file is mutated and nothing must be restored. Row: [[D-TEST-LANGUAGE-REFERENCE-REUSE-UNGUARDED]]; **(c) is met by that suite, not by the experiment**; (d) ⚠ **PARTIAL — NOT MET AS WRITTEN. Corrected 2026-08-12 by the same audit; it had been left stamped as met.** red-on-disable *"remove the reference and C's asm stops parsing"* has **NO test against the shipped `c.lang.json`**. The nearest proxy is `LanguageReferences.ReachingAsmWithoutTheReferenceFailsLoud`, which strips the reference from the in-test SYNTHETIC host and asserts the loud failure THERE — a real red arm for the MECHANISM, and a different claim from the shipped C config, where a deleted `languageReferences` entry is caught by nothing named here. ⇒ **owed: the same red-on-disable against shipped C.** Proven-on-the-synthetic-host is the honest status |
| ✅ **P2.5** — **DONE 2026-08-12** | `asm.lang.json` gains the shared standalone LINE STRUCTURE; a DIALECT document supplies the lexical surface, the root and the operand production, so a standalone `.s` PARSES | P1+P2 | ✅ MET, and by a running binary rather than by a tree dump. (a) the `encode` tier + name table + `static_assert`; (b) the entry-closure-scoped merge — shapes, rule-scoped ROWS **and** key-wise BLOCKS (§4.2, and the third of those was a second cycle's finding); (c) `kStandaloneOnlyDocumentKeys`; (d) the seven shared rules; (e) `asm-x86_64-att.lang.json`. ★★ **THE EXIT CLAUSE THAT WAS FLAGGED AS UNMEETABLE HERE STILL IS, AND THE REASON CHANGED:** *"both modes exercising the SAME shared rules"* — the embedded template is an opaque string until P5 parses its contents, so the two modes share the line-structure rules' NAMES but no rule instance. What DID become provable is the reuse property one tier down: the dialect writes ZERO line-structure rules. ⚠ And that is a ONE-dialect measurement — [[D-ASM-ARM64-DIALECT-UNWRITTEN]] is what would make it a claim |
| 🟡 **P3** — HALF DONE | the instruction/register/directive vocabulary a `.s` resolves against, and WHERE each half lives | P2.5 | ✅ **The standalone half is met:** mnemonics resolve per target through the dialect's `assembly.instructions[]` → `TargetSchema::opcodeByMnemonic`; registers through `registerByName`; directives through a CLOSED verb set; an undeclared spelling of any of the three fails loud naming BOTH the dialect and the target; agnosticism scan clean (no arch/format/language identity in the walker). ★ The architecture question this phase existed to settle is settled and RECORDED in §4.3: `.target.json` keeps vocabulary only, a DIALECT is a language. ⏳ **Not met:** the EMBEDDED-asm constraint letters (`=a`/`=d` x86, `=r` arm64) — those are P5's operand binding and were never reachable from the standalone path |
| ✅ **P4** — **DONE 2026-08-13, PROVEN ON BOTH CPUs BY EXECUTION** | asm TEXT reaches the assembler — text→LIR→bytes. Partially closes [[D-CSUBSET-INLINE-ASM-TEXT]] | P2.5 + P3 | ✅★★★ **arm64 CLOSED 2026-08-13 BY A RUNNING BINARY.** `examples/asm/asm_arm64_branch_call/main.s` — hand-written aarch64 GAS, no C — compiled to `arm64:elf64-aarch64-linux-exec` and **EXECUTED under qemu-aarch64 returning 42** (`file`: `ELF 64-bit LSB executable, ARM aarch64`). The exit code depends on a TAKEN branch, a NOT-TAKEN branch with an UNLABELED fallthrough between them, and a call to a second function in the same file — so a dropped CFG edge or a mis-elected opcode changes it. ★ **The second dialect was the experiment, and it returned a real result:** `asm.lang.json`’s seven shared line rules needed NOT ONE BYTE, but the text→LIR ENGINE needed SIX changes (sigil-less role ambiguity, dialect-dependent width source, absent operand roles, kind-vector election insufficiency, non-producing-opcode destination loss, width-absent variant matching any width — the last a live SILENT MISCOMPILE on arm64 `mov`). ⚠ “The shared GRAMMAR held” and “nothing had to change” are different claims; conflating them is the over-claim this plan keeps having to walk back. — ✅ **`examples/asm/asm_arith_return42/main.s` + `expected.json` COMPILES AND RUNS, exit 42, on pe64-x86_64 (native Windows) and elf64-x86_64 (WSL), debug AND release.** The arithmetic is three dependent steps in three opcode families (`sub`/`mul`/`xor`, all two-address) so any dropped or reordered instruction changes the exit code — deliberately not `mov $42; ret`, which passes even with every arithmetic instruction removed. ⏳ **NOT met:** elf64-**arm64** (needs [[D-ASM-ARM64-DIALECT-UNWRITTEN]]), and `__asm__("nop")` in the EMBEDDED mode (needs the template text parsed through the standalone instruction rules — P5). ⚠ The [[D-CSUBSET-INLINE-ASM-TEXT]] row therefore stays OPEN: standalone `.s` text now reaches the assembler; embedded template text does not |
| 🟠 **P5 — IN FLIGHT 2026-08-14** | extended asm reaches register allocation — closes [[D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED]] | P4 + the `D-TARGET-IMPLICIT-REGISTER-CONSTRAINT` reuse assessment — ✅ **assessment DONE, see §4.0: the STRUCT is reused, the CARRIER is not** | `hwtime.h` compiles; `--scanstatus` back on in `legs.json` with `requiredDefines` proving it took; `scanstatus`/`scanstatus2` execute in the corpus. ★★★ **NO P5a/P5b SPLIT — A PROPOSED SPLIT WAS REJECTED 2026-08-14 BECAUSE ITS BLOCKER DID NOT SURVIVE MEASUREMENT, AND THE REFUTATION IS RECORDED SO IT IS NOT RE-DERIVED.** The implementation plan proposed deferring arm64 on the grounds that sqlite `hwtime.h`'s arm64 arm (`mrs %0, cntvct_el0`) needs a system-register table plus a **15-bit** `EncodingSlotKind` that does not exist. ✔MEASURED by the independent audit, all three legs refuted: (1) `cntvct_el0`'s absence from `registers[]` is IRRELEVANT — `TPIDR_EL0` is also absent and DSS already encodes `MRS Xd, TPIDR_EL0` today; (2) the shipped **`tlsbase`** row in `arm64.target.json` encodes MRS as a **ZERO-OPERAND fixed word** `0xD53BD040 \| Rd`, its own `$comment` saying *"zero new slot vocabulary"* — `cntvct_el0` is structurally identical (`0xD53BE000 \| Rd`), so no slot is needed; (3) *"`hwEncodingOf` hard-caps fixed32 at 5 bits"* was MISSTATED — it caps at the **caller-supplied** `maxBitWidth` and handles `>= 16` explicitly; the 5 is `kFixed32RegFieldBits`, a register-field constant, and a fixed-word MRS never reaches it for the sysreg at all. ⇒ the residual work is one `.target.json` opcode row + one dialect instruction row. ⚠ **AND THE SPLIT WOULD HAVE BEEN AN OVER-CLAIMED CLOSE**: this phase's own exit criterion is *"`hwtime.h` compiles"*, and hwtime.h's arm64 arm **is** the `mrs` — so "P5a done" would have stamped a phase whose stated criterion it did not meet. |

⚠ **TWO EARLIER `P3`/`P4` ROWS WERE REMOVED FROM THE TABLE ABOVE ON 2026-08-12, NOT SILENTLY DROPPED.** They were leftovers from the phasing that existed *before* the operator's second decision (asm is a real input language), and they CONTRADICTED the rows that replaced them: the old `P3` was blocked on `P2` and scoped to constraint letters only, the old `P4` asked for `__asm__("nop")` and nothing standalone. The surviving `P3`/`P4` are blocked on **P2.5** and carry the `examples/asm/` end-to-end criterion the operator asked for. Two rows for one phase is not history, it is an ambiguity about what “P4 is done” means.

### 4.2 ★★★ A reference imports the ENTRY'S CLOSURE, not the document (P2.5, 2026-08-12)

`mergeLanguageReferences` used to fold in **every** shape of a referenced document. That was
invisible while `asm.lang.json` had one surface; the standalone half makes it break three ways, and
**one of them is a silent miscompile of the host language**:

1. ★★★ **`pipelineEntry.byRule` rows are matched by rule NAME, and every language's root shape is
   named `root`** (`data.rootRule = rules->intern("root")`). `asm.lang.json` declares
   `{rule: "root", tier: "encode"}` for its own `.s` unit. Unscoped, that row merges into
   `c.lang.json` and declares **C's translation unit** as entering at the assembler — every
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

### 4.0 ✅ P5 CARRIER DECIDED 2026-08-14 — a per-INSTRUCTION constraint pool, and NO new operand kind

**Operator ruling on the §B.** The implementation plan proposed a new `LirOperandKind::ClobberReg`,
justified by *"a side-table would be silently dropped by the rebuild passes"*. The independent
design-audit **refuted that justification** and the operator rejected the operand kind outright.

**What ships:** the per-instruction constraint set REUSES the existing `ImplicitRegisterConstraint`
struct (`target_schema.hpp:2530`) in a module-level pool indexed from `LirInst::_pad2`
(`lir_node.hpp:249`, already labelled *"future field"*).

★★★ **REUSE THE STRUCT, NOT THE CARRIER — and the distinction is the whole ruling.** Option 3 was
*"generalize the existing `TargetOpcodeInfo::implicitRegisters` binding site to also be written by the
lowering"*, and it was rejected because that field is populated FROM CONFIG
(`target_schema_json.cpp:1800`): making the lowering a second writer gives **one field two writers**,
the two-sources-of-truth shape this codebase rejects on sight, and it corrupts a config surface that
works today. The decided design keeps **two DISTINCT carriers with clean separate ownership** — config
writes the per-opcode one, lowering writes the per-instruction pool — sharing only the TYPE.
Operator, verbatim: *"Same type, two owners is fine; one field, two writers is not."*
⛔ **DO NOT touch the per-opcode binding site.**

★★ **THE COST WAS OVERSTATED IN THE FORK, AND THE CORRECTION IS THE DESIGN.** The fork said *"3 explicit
copy sites, and a missed copy is silent"*, implying three hand-rolled copies. ✔MEASURED: they are all
the SAME CALL to ONE shared helper, `lir_pass_util::copyLiteralPool(src, b)`
(`lir_pass_util.hpp:80` / `.cpp:126`). ⇒ **EXTEND THAT HELPER to carry both pools and rename it to what
it now does** (`copyModuleSideStructures` or similar). The call sites then need **ZERO** edits and the
constraint pool rides along automatically. Do NOT add a second parallel copy.
⚠⚠ **AND THERE ARE FOUR CALL SITES, NOT THREE — the plan AND the audit both said three.** ✔MEASURED at
`d4c2836b`: `lir_2addr_legalize.cpp:80`, `lir_callconv.cpp:3971`, `lir_rewrite.cpp:929` **and
`lir_wide_call_args.cpp:220`** (a genuine rebuild pass — `LirBuilder b{schema}` → `finish()`, its own
comment reads *"same discipline as rewrite/callconv"*). That fourth pass is precisely the one a
hand-rolled per-pass copy would have forgotten, which is the argument for the shared helper stated as a
measurement rather than as taste. **Every "survives the rebuild" claim and test says FOUR.**

★ **CLOSE THE SILENT-DROP RISK — DO NOT INHERIT IT.** *"Same risk class as the shipped literal pool"* is
an argument from precedent and **the precedent carries the bug**: a missed copy is a SILENT MISCOMPILE
(the clobbers vanish, the allocator reuses a clobbered register, wrong code ships green). Two parts,
both mandatory: **(1) structural** — the single extended helper, so there is no per-pass copy code left
to forget; **(2) a FAIL-LOUD BACKSTOP** — after each rebuild pass, assert every side-structure index
still resolves (a `_pad2` index outside the rebuilt pool, or a pool whose entry count dropped, fails
loud with a real diagnostic), catching anyone who bypasses the helper. ⇒ **this also fixes the shipped
literal pool, which has the identical exposure today** — required by §A.7 (every issue encountered is
anchored AND handled, never worked around), not optional scope.

★★★ **THE CONFIG-DRIVEN HALF, WHICH THE FORK DID NOT ASK ABOUT.** The ruling settles the CARRIER; it does
not settle the VOCABULARY. **Which constraint letter means which register is TARGET vocabulary and MUST
live in `.target.json`** — `"a"`→`rax` is x86, arm64's letters are entirely different. Reuse the
config-loading path `ImplicitRegisterConstraint` already has rather than minting an asm-private letter
table. ⚠ *"If this lands as a C++ switch it is an agnosticism break that no grep will catch until arm64
inline asm arrives"* — the same failure the `asmSyntax` facet reversal (§4.3) exists to prevent, in the
opposite direction.

**Mandatory tests, each red-on-disable proven by reverting and watching it go red:** the clobber
survives all FOUR rebuild passes, asserted **after each pass individually** (a drop in pass 1 restored
by luck in pass 4 reads as green) · a fixed-register INPUT `"a"(x)` lowers with the value in the named
register (**the case the rejected operand kind structurally could not express** — pin it, never assume
it) · the allocator does NOT assign a clobbered register across the block, as a BEHAVIOUR test with
constructed pressure, not a structural one · the verifier backstop FIRES on a hand-built dangling
`_pad2` index, asserting the diagnostic code (**exercise the failure arm, do not read it**) · the same
letter resolves to DIFFERENT registers under two targets, proving the mapping is config not code · the
literal pool's own survival is still pinned after the rename.

### 4.4 ✅ P5 CARRIAGE DECIDED 2026-08-14 (second operator ruling) — REUSE `ReturnPiece`; ZERO NEW VALUE-PRODUCING OPCODES

**Operator ruling on the §B.** The design-audit's plan proposed a new `MirOpcode::AsmOutputPiece`
modelled on `ReturnPiece`. The operator accepted the MODEL (asm outputs are SSA values carried as
pieces) and **rejected the new opcode**: `ReturnPiece` itself is reused, and `InlineAsm` takes
`Call`'s row shape. Rejected alternative: carrying outputs as MEMORY operands (the asm block takes
the output lvalues' addresses and stores through them) — it makes the C local address-taken, which
does not merely block mem2reg but degrades SROA and every alias-sensitive pass **including the LICM
already in the tree**, on precisely the hot paths inline asm exists to serve.

★★★ **THE ONE-FACT TEST, which is the whole ruling.** `ReturnPiece` encodes *"this value is result
k of the producer I am anchored to"* — and nothing else. ✔MEASURED and re-verified 2026-08-14 at the
in-flight tree: the row is `{1, 1, 0, 0, R::Value, false, true, false, "returnpiece"}`
(`mir_opcode.hpp:525`); `Call` is `{1, N, 0, 0, R::Optional, false, true, false, "call"}` (`:520`);
`mir_to_lir.cpp:2389-2393` states verbatim that *"The MIR `[call]` operand is the ordering anchor
only … the LIR `ret_piece` is a leaf carrying the per-class return ordinal as payload"*; and
`hir_to_mir.cpp:7600` — *"Piece 0 IS the call's own result; pieces 1..N-1 are ReturnPiece reads."*
⇒ the only call-specific thing about the verb is the word `Return` in its name. Two opcodes would
encode ONE fact, so **every** consumer — verifier, regalloc, `lir_2addr_legalize`, mem2reg, DCE,
scheduler, `lir_text` — would carry both arms forever, and the next multi-result producer would mint
a third. Operator, naming the queue: x86 `div`/`idiv` (quotient+remainder), flags+value arithmetic,
wide-return calls, **DSS Axis multi-return**. ⇒ this is [[the language-private verb set rule]] applied
one level down: **a CONSTRUCT-private verb breaks agnosticism exactly the way a LANGUAGE-private one
does**, and no single site looks like an identity branch while it happens.

⚠ **ONE DEVIATION FROM "VERBATIM", found by verifying rather than assuming:** `Call` is `{1, N}` —
minimum **one** operand, the callee. An asm block may have **zero** inputs, so `InlineAsm` needs
`{0, N}`. The row is Call-*shaped*, not Call-identical; say so rather than copying the literal.

**§4.4.1 — the ONE honest new fact, and where it must be owned.** `ret_piece` is an OPTIONAL,
config-declared LIR opcode (`lir_callconv.cpp:1844`); pieces are captured by a look-ahead from the
producer that **requires ADJACENCY**, tracked in `consumedRetPieces` (`:2258`), an unconsumed piece
being FAIL-LOUD (`:2522-2529`, *"never silently mis-capture"*), all pieces taken together as ONE
cycle-broken parallel move. **All of that is reusable unchanged.** Exactly one property is
call-specific: *which physical register a piece is captured from* — today implicitly
`cc.returnRegs[class][ordinal]`, inferred from *"the producer is a Call"*. ⇒ **FIX THE CORE:**
promote *"where do my result pieces live"* from an implicit callconv constant to a **property the
PRODUCER declares** (Call → the cc's return registers, unchanged; InlineAsm → the constraint-bound
registers). No consumer ever branches on producer identity — the same derivability keying that made
M6 free last cycle.
★ **HARD GATE on that property:** with it landed and no asm in the source, x86_64 + arm64 + pe64
output must be **BYTE-IDENTICAL** to pre-change. If it is not, the property was modelled wrong —
**stop and report; do not re-baseline goldens to absorb it.**

**§4.4.2 — the rename, done NOW because this is the cheapest moment that will ever exist.**
`ReturnPiece`/`ret_piece` → `ResultPiece`/`result_piece`. Mechanical, but it reaches the **shipped
target JSONs** (`ret_piece` is a config-looked-up string in both) and `lir_text` goldens, so the
churn is at its global minimum before the P5 corpus and goldens exist and rises monotonically
afterwards. **One mechanical commit, separate from the semantic work,** so the diff stays reviewable.

**§4.4.3 — constraint letters bind to SHIPPED vocabulary, never to asm-private flags.**
`"=r"` → a result piece + a register class, **the class coming from the CONSTRAINT, not the MIR
type** (`"=x"` legally binds an integer-typed value to an SSE register); `"+r"` → one input TIED to
one output piece, reusing `lir_2addr_legalize`'s existing two-address tying; `"=&r"` → earlyclobber,
the same "destination must not alias a source" constraint a target instruction can already need;
`"=m"` → a memory output, which is the operand the SOURCE asked for. ⚠ Where the core cannot express
one of these, **that is a CORE gap to fix in the core**, never an asm-private override.

**§4.4.4 ✔MEASURED 2026-08-14 — the four constraint forms against the core, one verdict each.**
The ruling labelled all of this INFERRED and demanded measurement before code. Result:

| form | verdict | detail |
|---|---|---|
| `"=r"` / `"=x"` (class from the CONSTRAINT, not the type) | ✅ **EXISTS — no core change** | `LirBuilder::newVReg(LirRegClass)` (`lir.hpp:216`) is the **only** creation API; there is no type-taking overload. **40+ shipped sites already pass a class independent of the MIR type** (e.g. `mir_to_lir.cpp:2449` `lowerVaFrameAddr` — type-blind, class-declared). Downstream reads the class off the `LirReg` itself (`lir_regalloc.cpp:960`, `lir_callconv.cpp:3699`). ⚠ One tripwire: `checkVregClassMatchesMirType` (`lir_verifier.cpp:397-436`) flags a result whose class ≠ `regClassForCoreType(MIR type)` and already carries a skip-list for `Phi`/`Alloca`/`GlobalAddr` (`:417-419`) — `"=x"` on an integer needs a fourth arm. Harmless today (zero production call sites) but **a concurrent lane is wiring the verifiers in**, so this must land with it. |
| `"+r"` (tied read-write) | 🟠 **PARTIAL — core gap, fix in the core** | The tie is a per-opcode **bool** `requires2Address` (`target_schema.hpp:2729`) hardwired to *result == operand[0]* at **four** sites, `0` a literal in every one: `lir_2addr_legalize.cpp:133-149`, `:151-156`, `:179-185`, and the allocator mirror `lir_regalloc.cpp:1026-1039` (*"Skip operand[0] (legitimate coalesce target)"*). No `(result k, operand j)` pair is expressible and there is **one** result slot (`lir_node.hpp:340`). Nothing else in the pipeline expresses tying (grep for `tied`/`sameAsInput`/`tiedTo`: nothing). ⇒ replace the bool with an operand INDEX defaulting to 0, thread it through the four literal-`0` sites; **every two-address target benefits**, which is why it belongs in the core. |
| `"=&r"` (earlyclobber) | 🔴 **ABSENT — nothing exists** | Exhaustive grep over `src/` and both shipped `.target.json` for `earlyclobber`/`noAlias`/`distinctFrom`/`dstNotSrc`/`mustDiffer` returns **two prose hits and no mechanism**; no shipped instruction declares such a requirement. Nearest behavioural analogue is the `requires2Address` result exclusion, which deliberately PERMITS operand[0] aliasing. ★ **The carrier question is settled by measurement:** `LirInst::flags` is a `std::uint8_t` (`lir_node.hpp:338`) with exactly three constants used (`0x01`/`0x02`/`0x04` width bits) ⇒ **five free bits**, and `flags` is threaded through **every** rebuild pass (`lir_2addr_legalize.cpp:213`,`:220` · `lir_callconv.cpp:3677`,`:3938` · `lir_rewrite.cpp:696` · `lir_wide_call_args.cpp:101`,`:198` · `lir_text.cpp:1967`) — i.e. it **survives rebuilds by construction**, which the `_pad2` side-pool handle explicitly does not (`lir_node.hpp:349-365`). A flag bit is the strictly safer carrier. |
| `"=m"` | 🟠 **PARTIAL** | Memory as an OPERAND is fully modelled (base / index / `MemBase`=scale / `MemOffset`=disp, `lir_node.hpp:73-81`). Memory as a **RESULT cannot be expressed**: `LirInst::result` is typed `LirReg` (`:340`), structurally incapable of holding a memory reference. ⇒ an `"=m"` output must lower to the store-class shape (`result: none` + operand list), never a memory result. |

★★★ **§4.4.5 — THE RULING'S OWN §8 CONTINGENCY HAS FIRED, AND ITS PRE-AUTHORISED RESOLUTION APPLIES.**
The ruling said: *"if `ReturnPiece`'s payload turns out to be semantically per-CLASS in a way asm cannot
express ⇒ the payload is carrying TWO facts (ordinal + class) and must be split — still a core fix,
still not a new opcode."* ✔MEASURED: **it is carrying two facts, and only one is stored.**
`addReturnPiece(call, ordinal, pieceType, …)` (`mir.hpp:534`) takes **no class parameter**;
`hir_to_mir.cpp:7604-7614` runs separate `gprRet`/`fprRet` counters, picks the ordinal from the
piece's class and then **discards the class**, re-encoding it as a TYPE via `pieceType()`
(`:7272-7287`, Fpr→F32/F64/F128, everything else→I64) whose own comment (`:7271`) states it outright:
*"The piece's register CLASS follows from the type."* Recovery is `regClassFor(id)` at
`mir_to_lir.cpp:2414`, and consumption is `returnReg(schema, cc, rpRes.regClass(), payload, …)` where
**the class selects the register POOL and the ordinal indexes it** (`lir_callconv.cpp:1520`).
⇒ a constraint choosing a class independently of the type (`"=x"` on an integer) would index the
**wrong pool, silently**, because GPR is the else-branch default. **Split the payload so the class is
carried explicitly rather than inferred from the MIR type** — core fix, no new opcode.

⚠ **AND A LATENT BUG FOUND IN PASSING, WHICH THE SPLIT MUST NOT WALK PAST:** `cc.returnVrs` exists
(`target_schema.hpp:558`) but `returnReg` **never reads it** — `lir_callconv.cpp:1520` is a two-way
`(cls == FPR) ? returnFprs : returnGprs`, so a **VR-class piece silently takes the GPR branch**. Same
else-branch-default shape as the defect above, already shipped. Anchor it and handle it.

★ **§4.4.6 — the §4.4.1 producer-declared piece source needs a CONSUMER, not a carrier.** ✔MEASURED:
`returnReg`'s three inputs are `(cc, class, ordinal)` and no producer-side channel is consulted at
any of its three call sites (`lir_callconv.cpp:3699`, `:3719-3722`, `:3866`). But
`LirRegConstraintPool` **already carries per-instruction `outputNames`/`outputOrdinals`** — precisely
*"this instruction writes register X"* — with a builder API and a reader that have **zero consumers
outside round-trip and rebuild-carry**. ⇒ the work is to consume the existing channel in
`returnReg`/regalloc. Do NOT build a second one.

### 4.5 ✅ `asm goto` WITH OUTPUTS — OPERATOR RULING 2026-08-14: **IT LANDS THIS CYCLE**

⚠⚠ **RECORDED WITH ITS COUNTER-ARGUMENT, because the ruling went AGAINST a technically sound
objection and a later cycle must not "correct" it back.** The objection, ✔MEASURED and confirmed:
the entire piece-capture machinery rests on the piece **immediately following** its producer
(`lir_callconv.cpp:2255`, `:2522-2529`, where broken adjacency is already fail-loud). A terminator
has nothing after it in its block ⇒ for `asm goto` the adjacency invariant is **structurally
unsatisfiable**, and the pieces must be placed at the **successor block heads** — the same place an
edge parallel-move lives. The proposal was therefore to refuse it loudly this cycle via a
derivability question (*"can a result piece be placed at this producer's definition point?"*),
reusing `lowerReturnPiece`'s F128 `reportUnsupported` + `poisonValue` arm (`mir_to_lir.cpp:2409-2412`).

**The operator ruled to BUILD IT instead.** ⇒ the edge-placement rule lands this cycle: result pieces
at the head of every successor block, **splitting critical edges** where a successor has more than one
predecessor. The placement rule is ADDITIVE to §4.4's carriage and changes none of it. The honest
argument for the ruling is the standing one: gcc 11+ and clang both accept the form, so under
[[feedback_reference_compilers_are_the_spec]] refusing it is a real divergence, and §A.3 forbids
slicing the hard part off a cycle whose blocker has been discharged.
⇒ **[[D-CSUBSET-INLINE-ASM-GOTO]] CLOSES this cycle** rather than being narrowed to an
outputs-on-edges remainder. Do not open a sibling row for the placement rule.

### 4.6 ✅ `%N` OPERAND BINDING IS **STRUCTURAL** (decided 2026-08-14 on the bar, not an operator fork)

The template is opaque to the ALLOCATOR (§1 correction 1) but is parsed by the ASSEMBLER through the
**dialect grammar the standalone `.s` path already uses** — one text→LIR engine, two callers.
`%0` is a placeholder **token/rule the dialect declares**, resolving at expansion time to the asm
operand's vreg, emitted as an ordinary `LirOperand::makeReg`. ⇒ regalloc, rewrite, callconv and the
encoders work unchanged and **no post-regalloc binding pass is needed at all** (an earlier phrasing
of this decision said "bind placeholder → allocated register after regalloc"; that was wrong).

⛔ **REJECTED — render the operand as TEXT (`%eax` / `x0` / `$7`) into the template and re-lex.** The
renderer would restate sigils the dialect token table **already declares for parsing** — one fact,
two owners, the shape [[D-CONFIG-ASM-DIALECT-DECLARED-AS-TARGET-VOCABULARY]] was reverted for.

★★★ **AND THE MEASUREMENT THAT KILLS THE OBVIOUS IMPLEMENTATION.** ✔MEASURED 2026-08-14 on gcc
13.3.0, source fed as base64 to stdin to remove all shell/printf ambiguity:

| C source template | gcc emits |
|---|---|
| basic `__asm__("xorl %eax, %eax")` | `xorl %eax, %eax` — `%` is **LITERAL** |
| extended `__asm__("xorl %eax, %eax" : : "r"(a))` | **ERROR**: *"operand number missing after %-letter"* |
| extended `"A%%B C%0D"` | `A%B C%ediD` |
| extended `"A%%0B"` | `A%0B` — the emitted `%0` is **NOT** re-read as an operand |

⇒ **(1)** template lexing genuinely DIFFERS between basic and extended, so the basic/extended
discriminator is load-bearing at the LEXER, not only at the semantic gate. **(2)** Any
"unescape `%%`→`%` into a buffer, then lex placeholders" design **silently binds `%%0` to operand 0
— a miscompile gcc does not have.** Resolution: declare the `%%` escape token AHEAD of the
placeholder token so maximal munch decides it.

★★★ **§4.6.1 CONFLICT SETTLED 2026-08-14 — THE §2a TABLE IS WRONG AND MUST NOT BE RE-QUOTED.**
§2a recorded `__asm__("xorl %eax,%eax" ::: "eax")` as working on gcc AND clang. ✔RE-MEASURED twice,
in two independent runs, sources fed as **base64** so no shell/printf/heredoc could alter a byte,
on `gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1)` and `Ubuntu clang version 18.1.3` (unsuffixed `clang`
confirmed ABSENT):

| source | gcc 13.3.0 | clang 18.1.3 |
|---|---|---|
| `__asm__("xorl %eax,%eax");` — BASIC | rc=0, emits `xorl %eax,%eax` | rc=0, same |
| `__asm__("xorl %eax,%eax" ::: "eax");` — EXTENDED | **rc=1** *"invalid 'asm': operand number missing after %-letter"* | **rc=1** *"invalid % escape in inline assembly string"* |
| `__asm__("xorl %%eax,%%eax" ::: "eax");` | rc=0, emits `xorl %eax,%eax` | rc=0, same |

**ANY colon makes it extended** — `:` alone, `:::`, and the `volatile` spelling all error identically.
⇒ **in an extended template `%`+letter is a MODIFIER, not a sigil** (`%w0` is a valid modifier and
works), so the template surface and the `.s` surface **genuinely differ lexically**.
⚠⚠ **CONSEQUENCE FOR THE NEGATIVE MISCOMPILE PIN: it must be written `%%eax`, not `%eax`.** A pin
written the §2a way would not compile under the reference compilers at all.

⚠ **§4.6.2 — A PIN THIS PLAN ASKED FOR CANNOT EXIST, and the correction is the useful part.** The
brief demanded a token-DECLARATION-ORDER pin (`%%` red on reordering the two rows). ✔MEASURED in
source: `tokenizer.cpp`'s `longestMatch` probes **down from the schema's longest declared lexeme**
and returns the first hit, so 2-byte `%%` beats 1-byte `%` **by LENGTH, for every input, in any row
order**; `priority`/declaration order only break ties among the meanings of ONE lexeme. And
`nlohmann::json`'s default object is a `std::map`, so **a `.json` file's row order does not survive
the parse at all**. Reordering is provably inert ⇒ the requested pin would have asserted **nothing**
— a vacuous test of exactly the kind this project's red-on-disable discipline exists to catch, and
it was asked for by the orchestrator. What shipped instead pins the only mutation that CAN change
the outcome: **deleting the `%%` row**, as a true in-process differential that also `ADD_FAILURE`s if
the shipped document has no `%%` row to erase.

★★★ **§4.6.3 — THE PLACEHOLDER CANNOT BE A SIBLING ALT. THIS IS A REAL FORK, MEASURED BY EXERCISE.**
`%` is ALREADY `RegisterSigil` (AT&T) / `TypeSigil` (arm64), and `detectAmbiguousAlternatives`
**refuses two sibling alts sharing a FIRST token**. This was not read, it was **exercised** —
`X86AttCannotExpressAPercentNumberOperandAsASiblingAlt` and its arm64 twin add the arm in-process
and assert the loader refuses it, naming the shared token; both green. Combined with §4.6.1 (the two
surfaces differ lexically in the reference compilers) this is a **design fact, not a loader
limitation**. ⇒ the placeholder needs `%` to carry a SECOND kind in template context. The shipped
mechanism for exactly that is a **per-mode `tokens` override (`lexerModeTokens`)**, already in the
tree and documented for this purpose — reuse, not invention. Recommended shape: the dialect declares
an `asm-template` lexer mode (`%`→placeholder, `%%`→escape) plus an `assembly.templateLexerMode` key
so the template entry selects it without a hardcoded string. ⓘ The lane deliberately did **not** ship
half of it, because its entry point is consumer-side and belongs with the template→LIR lane.

> ⚠ **SUPERSEDED 2026-08-17 — the paragraph above is the HISTORICAL recommendation and its second
> half is now WRONG. Do not implement it as written.** The `lexerModeTokens` mechanism and the
> `assembly.templateLexerMode` key were both adopted, exactly as recommended. What changed is **who
> declares the BYTES**: a dialect declaring `%`→placeholder / `%%`→escape in its own mode is precisely
> the two-owner defect [[D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER]] recorded,
> and it is now a **LOAD ERROR** naming both sites. The shipped split is: the **DIALECT** declares the
> capability (`assembly.templateLexerMode`) and its own token **KINDS** (`bindTokens`); the
> **LANGUAGE** (`asm.lang.json`) declares the **BYTES** (`semantics.inlineAsmTemplateLexemes`); the
> **LOADER** intersects them and synthesizes the mode rows. Both dialects' `asm-template` modes are
> now literally `{}`. ★ The reasoning that settles it: `%0`/`%%`/`%l[…]` are GNU **C-extension**
> syntax that gcc substitutes *before* the assembler sees the text — gas never receives a `%0`, it
> receives the substituted register name — so the sigils are a LANGUAGE fact and the dialect's job
> begins after substitution. Kept rather than deleted because it records why the mechanism was chosen,
> which is still correct and still the reason the mechanism is there.

### 4.7 ✅ OPERATOR RULING 2026-08-14 — **POSITIONAL OPERAND SELECTORS**, and why the sysreg-operand alternative is an AGNOSTICISM BREAK

**The problem.** Two independent lanes hit the same wall and grepped the same empty result:
there is **no vocabulary for "this operand SELECTS the opcode"**. gas writes
`mrs <Xd>, cntvct_el0` and `cset <Xd>, eq`; the target opcodes are `cntvct` (zero-operand,
counter in the fixed word) and `setcc` (cond as a `TargetCondCode`). A naive spelling row hands the
lowering a leftover operand and **every line fails loud**.

⛔ **REJECTED — model system registers as a real operand kind with a sysreg table.** ✔MEASURED both
targets: x86_64 `rdtsc` (`:450-460`) is `min/maxOperands 0`, `implicitRegisters.outputs [rax,rdx]`,
`outputRoles {low, high}`; arm64 `cntvct` (`:1663-1667`) is `min/maxOperands 0`, `result value`,
`fixedWord 0xD53BE040`. They differ in RESULT MODEL — and that difference is real and fine — but
**both are ZERO-OPERAND with WHICH COUNTER baked into the opcode**. ⇒ *"read the hardware counter"*
is **ONE VERB across ISAs**; `cntvct_el0` is an AArch64 spelling detail exactly as `edx:eax` is an
x86 one. Making the arm64 read a generic `mrs` + sysreg operand gives it a **different SHAPE from
the x86 read**, so every shared consumer — starting with `hwtime.h`'s own lowering — acquires a
per-arch shape distinction. **That is the `if (arch == …)` the bar hard-vetoes, arriving through
CONFIG rather than C++, which is the slower and worse way for it to arrive.** It would also require
rewriting a shipped opcode a green codegen path already consumes — a regression risk bought with
nothing. ★ The opcode's own `$comment` already made this argument: *"a generic `mrs` could only ever
read one system register."*

✅ **SHIPS — selectors declared POSITIONALLY.** ⚠ **NOT "the literal operand"** — that key is already
false about the instruction's mirror image: gas writes `msr tpidr_el0, x0`, selector **FIRST**, and
this dialect declares `operandOrder: destinationFirst`, so a position-blind rule would read the
SELECTOR as the destination. Shape (spelling is the lane's; the PROPERTY is the ruling):
```json
{ "spelling": "mrs", "operandSelectors": [ { "index": 1, "name": "cntvct_el0" } ], "opcodes": ["cntvct"] }
```
**Semantics, stated so the lowering cannot drift:** a selector token is **consumed BY THE MATCH** —
it never becomes an operand, never reaches the target, and is **excluded from the destinationFirst
reading**. So the lowering sees exactly one operand (`x0`, the destination) and **zero remaining**,
satisfying `maxOperands: 0` with nothing left over. A selector at **index 0 SUPPRESSES
destinationFirst for that position** — specify it now, but do **NOT** ship an `msr` row (nothing
needs one; §A.2 cuts both ways). The point is that the KEY must not be wrong about `msr`.

★★★ **WHY THIS CLEARS §A.2 AND THE ALTERNATIVE DOES NOT — the scoring, not a preference: ONE
mechanism closes TWO ALREADY-ANCHORED rows.** The dialect file itself says they are one defect
(*"the same shape as `cset x0, eq`: an operand ROLE this grammar has no form for"*):
[[D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED]] and
[[D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED]]. Selectors close both —
`{"spelling":"cset","operandSelectors":[{"index":1,"name":"lo"}],"opcodes":["setcc"],"cond":"ult"}`
× 12, mirroring the shipped `b.eq`…`b.cs` rows exactly. (a) has **two anchored witnesses**; the
alternative has **zero**.
⚠ **`name:"lo"` beside `cond:"ult"` is NOT one fact written twice** — it is the gas spelling mapped
to the `TargetCondCode`, the same non-identity the shipped `b.lo`/`ult` row already carries. **DO NOT
"simplify" it by deriving `cond` from the selector string**; that derivation is precisely the
letter-pattern-matching the `b.<cc>` comment warns **silently miscompiles every unsigned comparison**.
⚠ **HONESTY CLAUSE, and it goes in the row:** both witnesses are in **ONE dialect**. This project's
own standard is in the `nop` comment — *"a mechanism witnessed by one signatory is the weaker claim
this project keeps having to walk back."* Two rows in one dialect is stronger than one and weaker
than two dialects. Say so; do not present it as settled cross-dialect because the key is loader-owned.

★★ **§4.7.1 THE HARDENING WITHOUT WHICH THIS RE-CREATES A DEFECT THIS FILE ALREADY FIXED.** If a
selector row and a non-selector row for the same spelling can BOTH match, the loader **MUST REFUSE
AT LOAD TIME, naming both rows**. **NEVER first-match. NEVER most-specific-silently-wins.** Precedent
is in this exact file, twice: `ldr`/`ldur` were split into separate rows precisely because guard
election *"would take the first and silently encode LDR where the programmer wrote LDUR"*, and
`operandForms` already runs an undecidable-pair check that fails the load. **An ambiguity check is
the price of admission for this key** — without it, selectors buy the `cntvct` row by planting the
`ldr`/`ldur` bug one level up.

**§4.7.2 MEASURE BEFORE WRITING — do not trust this section.** (1) Does `cntvct_el0` lex as ONE
Identifier under this dialect's identifier rule? [[D-ASM-DIALECT-IDENTIFIER-CONTINUATION-NOT-CONFIGURABLE]]
closed last cycle so it should — **confirm digits+underscores are actually covered**, do not assume
the close reached them. (2) Does `aarch64-linux-gnu-as` accept `MRS X0, CNTVCT_EL0`? Match the
reference assembler the way `.global` vs `.globl` was settled; **if it is case-insensitive the
selector match must be too.** (3) Confirm `cntvct`'s encoding variant guard is `operandKinds: []`
(width-absent) so no `width` key is needed — same reasoning as the `nop` row. (4) Confirm nothing in
`src/asm/` needs a **spelling-specific branch**; if the generic walk cannot express it, **the KEY is
wrong — report that, do not add an `if` for `mrs`.**

**§4.7.3 TESTS.** Delete the selector ⇒ `mrs x0, cntvct_el0` FAILS LOUD naming the spelling and the
dialect (**not** a fall-back to a generic match). A WRONG sysreg (`mrs x0, tpidr_el0`) ⇒ fails loud
as an unknown spelling — **this is the row proving the selector actually SELECTS rather than being
ignored.** A deliberately ambiguous second row ⇒ **LOAD ERROR naming both** (§4.7.1); exercise the
arm, do not read it.
★★★ **THE ONE THAT PROVES THE ACTUAL CLAIM — SAME VERB, TWO FRONT ENDS:** assemble
`mrs x0, cntvct_el0` through the `.s` path **and** emit the counter read through C's `hwtime.h`
path, and assert the encoded word is **IDENTICAL (`0xD53BE040`)**. That is the claim — that the
dialect and the codegen reach ONE verb — and it is what P5's exit criterion actually needs. **A test
that only checks the `.s` assembles proves the row exists, not that it is the same instruction the
compiler emits.**

**§4.7.4 ANCHOR DISPOSITION.** CLOSE [[D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED]]. CLOSE
[[D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED]] in the SAME cycle **if the `cset` rows land**; if
they do not, **say so explicitly and leave it open — a mechanism that COULD close a row has not
closed it.** OPEN one trigger-gated row with a **testable** trigger, not a vibe: *"a shipped target
requires an `mrs`/`msr` whose system register is NOT one of a small set of concrete DSS verbs — i.e.
the sysreg must be carried as DATA into a real encoding slot."* ★ Record that selectors **FORECLOSE
NOTHING**: if that trigger fires, a real sysreg operand kind coexists with selector rows — the
selector row is a **SPECIALIZATION**, the same relation `nop` has to a generic form — *provided
§4.7.1's ambiguity refusal is in place.* That is the second reason §4.7.1 is not optional.

### 4.8 ✅ P5c 2026-08-17 — THE INPUT HALF WAS NEVER WIRED, AND THREE EXAMPLES COULD NOT SEE IT

The arc's own retrospective, recorded here because it is a lesson about how this plan's phases were
VERIFIED rather than about what they built.

**What shipped broken.** `expandInlineAsm` materialised only *pinned* inputs into their bound
register; the loop opened `if (!ins[j].pinned) continue;`. `bindAsmOperand` mints a fresh vreg for an
unpinned operand, and for an INPUT nothing else ever writes it. ✔MEASURED: `__asm__("movl %1, %0" :
"=r"(r) : "r"(a))` with `a == 42` compiled **rc=0** and returned **0**, on both `pe64-x86_64` and
`elf64-x86_64`, at debug and release. The disassembly named it exactly — the input's load defined the
register the OUTPUT had been allocated, and the template's `%1` read one nothing wrote.

★★ **THE FALSE SYMMETRY.** The capture loop twelve lines below correctly skips unpinned OUTPUTS —
*the template writes that vreg, so a copy would be dead* — and the input loop reads as its mirror
image. It is not one: **an output is written by the TEMPLATE; an input must be written by the
LOWERING.** Two loops that look like a matched pair, one of them inverted. It is also precisely the
read-as-undefined outcome the `"+r"` refusal in the SAME FILE exists to prevent — the guard was
written against one entrance and the plain `"r"` path walked in the other.

★★★ **WHY THE CORPUS COULD NOT SEE IT, and this is the durable lesson for §4's remaining phases.**
The three shipped inline-asm examples declared **ZERO input operands between them**: `c_inline_asm`
is the empty template, `c_inline_asm_extended` is register-PINNED outputs (`rdtsc`) on x86_64 and a
pure CLOBBER list on aarch64. ⇒ **A PHASE'S COVERAGE IS AS WIDE AS THE OPERAND SHAPES ITS EXAMPLES
NAME, NEVER AS THE NUMBER OF EXAMPLES.** Counting examples said inline asm was well covered; counting
SHAPES said inputs had never once run. §4.4's exit criteria should be read that way: enumerate the
shapes, not the fixtures. ⚠ It also re-reads P5's headline honestly — `hwtime.h` compiling was a TRUE
result, and true *because* `rdtsc` has outputs and no inputs, so the motivating construct could not
have caught this.

★★ **AND THE PIN NEARLY SHIPPED A FALSE CLAIM.** The replacement example first asserted it
"discriminates at both arms, because a register nothing wrote is undefined at every optimization
level" — airtight-sounding and wrong. ✔MEASURED with the mutant restored and the example reduced to
its call-shaped two-input helper: **baseline exited 42 (GREEN — the mutant SURVIVED)** while release
exited 1; the allocator had left the right value in the register the template read. The same defect
lowered directly in `main` reddened the BASELINE arm. ⇒ a pin that survives because an undefined
register happened to hold the right value is not a pin, and which shape gets that luck cannot be read
off the source. `examples/c/c_inline_asm_operands` therefore carries **both** shapes and must
not be simplified to one.

Rows: `D-LIR-ASM-UNPINNED-INPUT-NEVER-MATERIALISED`,
`D-MIR-ASM-OUTPUT-STORE-BACK-BYPASSES-THE-SCALAR-FUNNEL`,
`D-TEST-MIR-ASM-DESCRIPTOR-NEW-FIELDS-UNPINNED-THROUGH-REBUILD` (all born ✅ CLOSED).

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
  ✅ **DECIDED 2026-08-14 — FOLLOW CLANG: ACCEPT the no-label form.** Operator ruling on the §B. Two of
  the three references accept it (clang 18.1.3 + 19.1.1; gcc 13.3 rejects), which is the majority rule
  this project already applies elsewhere, and it makes the `goto` qualifier inert when no labels are
  present exactly as `volatile` is inert for a no-output asm. ⇒ the named blocker on
  [[D-CSUBSET-INLINE-ASM-GOTO]] (*"operator decision"*) is **DISCHARGED**, so under §A.3 the CFG half
  lands in the deciding cycle rather than being re-deferred.
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

### `asmLabel` deliberately REMAINS in `c.lang.json` (decision, 2026-08-12)

Recorded so a later cycle does not “tidy” it into the asm language on the strength of its name. `asmLabel` is
GCC's **declarator decoration** — `int gv __asm("myglobal")`, GCC 6.47.5 *Controlling Names Used in
Assembler Code* — and what it renames is a **C symbol**. There is no template, no operands, no clobbers; its
payload is a **symbol name, not assembly text**. Moving it into `asm.lang.json` would put a C declarator
feature into the asm language and **invert the thesis of §1** (assembly is a source language, not a bag of
constructs whose names contain “asm”). ✔TREE: `c.lang.json` `shapes.asmLabel`, the `initDeclarator`
after-declarator run, and `semantics.asmLabelRule`; the feature itself is [[D-CSUBSET-ASM-LABEL-SYMBOL-RENAME]]
(✅ closed TF-C88). **The dividing line to apply to the next candidate: does the construct carry ASSEMBLY, or
does it carry a NAME?**

---

## 6. What this arc is NOT

Not "make sqlite's `build(Default)` compile". That is two corpus files of ~10,800 and would be a poor reason
to do any of this. See §0 "Why it exists".
