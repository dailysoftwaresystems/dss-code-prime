# The verification battery + guardrail enforcement

## D. The verification battery — concrete

Run all of these from the repo root and read the *actual* output, not the commit message.

| Check | Command | Pass condition |
|---|---|---|
| Build | `cmake --build build` | clean, no link errors |
| Full suite | `ctest --test-dir build --output-on-failure` | `100% tests passed, 0 failed` |
| Anchor guard | `scripts/check-anchor-registry/check-anchor-registry.ps1` (or `.sh`) | `anchor-registry: OK (N … all resolve)` |
| Agnosticism scan | `grep -rnE "==\s*(Target\|Arch\|Format\|Lang\|ObjectFormat)Kind::\|target\s*==\|arch\s*==\|format\s*==\|isX86\|isPE\|isELF\|lang\s*==\|\"x86\|\"rax\"\|\"rdx\"" src/opt/ src/mir/ src/lir/ src/asm/ src/link/` (extend to the cycle's touched area) — **tier the result, but the tiers are NAMED, not guessed:** in the **universal pipeline** (`src/opt/` `src/mir/` `src/lir/` `src/hir/` `src/core/` `src/analysis/` `src/tokenizer/` `src/preprocess/` `src/link/` **minus** `src/link/format/`) any live identity branch is a violation, full stop. The ONLY sanctioned realization tiers are **`src/asm/<arch>` encoders** and **`src/link/format/`** — a backend there may know which format it implements, because that is what it exists to do. Everywhere else a `kind ==` / `target ==` hit is a violation unless it reads a `.target.json`/`.format.json` field.<br><br>★★ **`src/link/` IS SHARED SUBSTRATE — THIS ROW USED TO SAY OTHERWISE AND IT WAS WRONG.** It previously grouped the whole of `src/link/` with `src/asm/` as a "per-format layer" where dispatch "may be by-design", and cited `src/link/object_format_schema*.cpp`'s `ObjectFormatKind::*` branches as a *"Known live example to adjudicate"*. An auditor following that instrument as written would have passed those sites by — and did, for many cycles: **25 live enumerator comparisons, a `kind` field DEFAULTING to `Elf`, and 2 kind-keyed tables** accumulated behind it. (The anchor row and several TF-C122 notes say "26 comparisons"; re-measured in TF-C125 that is 25 comparisons + the defaulted field — 26 *sites*, not 26 comparisons.) The operator's 2026-08-06 ruling settled the question: *"we must never have identity branches, so this must be addressed"*, with **no loader exception** — the standing defence that a schema loader must know which sub-schema applies was examined and REJECTED. See [[D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES]] for the current status; TF-C125 removed the schema tier's 25 comparisons + the defaulted `kind` field + both kind-keyed tables, and pinned the two schema TUs with a **compile error** (the type's name is `#define`d out of reach below the includes, so re-introducing it fails the build in every spelling — unqualified and qualified alike). ⚠ **Whether that row is CLOSED is the registry's verdict, not this checklist's premise** — an earlier draft of this paragraph asserted the closure here while the row still read OPEN, which is the instrument pre-declaring an audit's conclusion. Read the row.<br><br>⚠ **AND A TABLE KEYED ON IDENTITY IS AN IDENTITY BRANCH.** The grep above only finds `==`. It does NOT find `constexpr Row kTable[] = { { ObjectFormatKind::Pe, … } }`, and this tree twice defended exactly that in prose (*"expressed as a TABLE … not an if-chain"*). Both comments were struck in TF-C125. When auditing, grep for `ObjectFormatKind` / `CallConv` / arch names appearing as **table columns or map keys**, not only as comparison operands — the consumer still has to know an identity to index it, which is the whole defect.<br><br>★ **STILL-OPEN EXAMPLES, so this row names real work instead of a closed one:** `src/ffi/abi/abi_catalog.cpp` (`kAbiCatalog`, keyed on `(targetName, ObjectFormatKind)`), and `src/program/` — `target_spec.cpp`'s output-EXTENSION `switch` (+ its `.lib`/`.a` ternary), `cross_validate_target_format.cpp`'s machine-code `switch`, `compile_pipeline.cpp`'s archive-member reader `switch` (+ its ar-flavor ternary), `program.cpp`'s `.obj`/`.o` pick — **26 `ObjectFormatKind::` mentions across 5 branches, MEASURED TF-C125**. ⚠ **`src/ffi/` and `src/program/` are BOTH SHARED SUBSTRATE** (ruled TF-C125 — see the tier note in plan 23 §5): neither is a sanctioned realization tier, and their absence from the enumeration was a documentation gap, never an exemption. | **empty**, or every hit is a comment / diagnostic string / a `.target.json`-driven read — never a live identity branch in shared code |
| Overnight / cycle delta | `git log --oneline <baseline>..HEAD` | each commit maps to a real priority or a pinned anchor |
| WIP peek (dirty tree) | `git status -s` + targeted `git show`/`Read` of the dirty files | inspect, do not build; report "in flight" |

**Agnosticism scan caveat:** a clean grep is necessary, not sufficient — it catches `if`-on-identity
but not the *subtle* hardcodes in §E (a conservative default baked into shared code reads as clean to
the regex). Always pair the scan with §E.

---

## F. Guardrail enforcement — go / no-go

- **OPT7 / inlining — `G-406` (plan 07) + cross-CU sub-anchor `D-OPT7-1` (plan 22) — hard stop.** If an
  inlining / inter-procedural pass was opened *autonomously*, that is a violation; it is a
  supervised-decision boundary (matches `dss-cycle` §D). A *supervised* opening (user go + a §B brief)
  is expected and fine — only an autonomous opening is a finding. Flag it.
- **Trigger-gated anchors are not TODOs.** A row that DECLARES a trigger must remain open until that
  trigger fires; a closure with no fired trigger is a finding (§E #5). ⚠ **Find them with the
  instrument, never from a list in prose:** `grep -n 'trigger-gated' .plans/_deferred-anchor-registry.md`
  enumerates every row that declares one, and
  `python scripts/check-anchor-balance/check-anchor-balance.py` says which are still open. This bullet
  USED TO name `D-OPT-MEMORYSSA-CLOBBER-WALK` and `D-OPT4-1-NON-LINEAR-MARKER-MERGE`
  as its two exemplars, and both HAVE BEEN CLOSED since — so a rule that was correct was routing
  auditors to closed rows. A named exemplar is a status claim with a shelf life
  (D-COMMENT-A-CLAIM-TRUE-WHEN-TYPED-AND-FALSE-WHEN-THE-COMMIT-LANDED); the rule is what belongs
  here, and the instrument is what supplies today's examples.
- **Correctness-critical anchors need a demonstrated negative pin.** Any silent-miscompile-class
  closure (e.g. `D-OPT6-LICM-TRAP-SAFE-HOIST`) must ship a program that **breaks iff** the transform
  mis-fires, and the pin must be shown **red-on-disable** — not merely present (§E #4). No pin →
  not closed, regardless of review.
- **No red pushes.** The full gate (§A.6) holds at every commit. A pushed red — even one fixed in a
  follow-up — is a finding; note whether the implementer's *local* gate has a blind spot (§E #7) that
  let it through.
- **Legitimate single-target residual ≠ violation.** A target-specific code shape that is correct
  for the only built target, with the agnostic generalization *anchored* for the 2nd target, is an
  honest deferral — distinct from a hardcode that wrongs an existing consumer (§E #1). Confirm the
  anchor exists and the trigger is "2nd target lands".

---
