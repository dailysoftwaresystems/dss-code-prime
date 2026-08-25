# The bar applies to the operator, not only the code

## ★★★ NEVER CITE A LINE NUMBER — CITE SOMETHING THE FILE CARRIES

**Operator rule, 2026-08-19, verbatim:** *"we must never document line numbers, we must document
method names, comment ids or defined anchors. everything that changes is unreliable."*

```
✗ src/mir/lowering.cpp  + a line number   <- moves the moment anything above it changes
✓ src/mir/lowering.cpp — lowerCallArgs()
✓ tests/CMakeLists.txt — the `no RUN_SERIAL` rationale
✓ [[D-TEST-INTEGRATED-FIXED-TEMP-PATH-COLLIDES]]
```

**A symbol survives every edit above it; a line number survives none.** And the failure is the bad
kind: a citation that BREAKS gets noticed, one that silently becomes WRONG still resolves, still
reads as evidence, and now points at unrelated prose.

⚠ **✔MEASURED twice in one cycle (P17), which is why this is a rule and not advice.** Inserting a
`# PURPOSE:` line at the top of eighteen scripts moved **16** citations off their subjects —
one citation into `check-anchor-registry.sh` had pointed at *"…staleness sweep commit message…"*
and pointed instead at *"Same pattern the developer-side audit grep uses…"*. Then the rows written to RECORD
that defect shipped **three more** wrong numbers of their own, each naming the first line of an
explanatory comment rather than the code it explains. Both were caught by independent audit; no
gate saw either.

**Enforced** by `plan_citations_guard` (ctest) over `.plans/**` and `.claude/**`, as a ratchet: the
2376 pre-existing citations sit in a per-document inventory whose ceilings may only come **down**.
A new one reds immediately. Converting one reds until its ceiling is lowered in the same commit:

```bash
python scripts/check-plan-citations/check-plan-citations.py --write
```


## C.-1 THE BAR APPLIES TO THE OPERATOR, NOT ONLY THE CODE

The bar in §A governs what gets committed. This section governs **what gets said** — because
a confident wrong claim is acted on, and costs more than an admitted unknown. Added
2026-07-26 after a session where the analysis held and the discipline did not, and where
**three of the operator's errors were caught by delegated agents rather than by the
operator**.

1. **Label every factual claim MEASURED / DOCUMENTED / INFERRED.** Never let an inferred
   claim wear the voice of a measured one. "I verified X" and "X is documented to be true"
   and "X follows from Y" are three different statements with three different failure modes.
1b. **★★ BEFORE COMMISSIONING AN EXPERIMENT, GREP THE REGISTRY FOR A MATCHED CONTROL THAT
   ALREADY EXISTS.** Search `_deferred-anchor-registry*.md` for the failing artefact, the leg,
   the test family and the symptom — then CITE what you find, or state explicitly that nothing
   matched. ✔MEASURED 2026-08-06: a 2×2 attribution (compiler × rundir filesystem) was
   commissioned from scratch for 57 sqlite failures; the identical experiment with the identical
   verdict was **already in the registry** from seven cycles earlier
   (`D-HARNESS-WSL-LAUNCHED-LEG-RUNDIR-IS-DRVFS`), and the row was findable — the leg name, the
   driver and the word `rundir` all appear in it. ⇒ **the cost was not the duplicated work: the
   un-cited row would have pre-empted THREE FALSE STATEMENTS that reached a commit** ("previously
   green", "the Tcl move is the prime suspect", "the WAL/journal TIMING family"). ★ THE FAILURE
   MODE IS SPECIFIC AND WORTH NAMING: I searched MEMORY for a matching *confound* and recall
   surfaced a plausible NEIGHBOUR (the WSL2 clock defect — real, but the wrong population); a
   grep would have surfaced the exact CONTROL. **Recall finds what is similar; grep finds what
   is the same.** ⇒ anchoring every issue is worthless if the next cycle does not READ the
   anchors before investigating (`D-PROCESS-CHECK-THE-REGISTRY-FOR-A-MATCHED-CONTROL-BEFORE-COMMISSIONING-ONE`).
   ★★ **AND GREP THE DEFECT'S VOCABULARY, NOT THE NAME YOU WOULD PICK — the rule above was
   FOLLOWED and still missed, by the operator who wrote it.** ✔MEASURED 2026-08-07 (TF-C126): a
   recipe derivation was found harvesting `tool/lemon.c` / `lempar.c` / `mksourceid.c` as target
   TUs. The registry WAS grepped first — for anchor NAMES matching `RECIPE|MAKE-N|DERIV|LEMON|
   TU-LIST` — and returned nothing, so a fresh row was written. The existing row was
   [[D-HARNESS-FIXTURE-TU-SCRAPE-ABSORBS-BUILD-HOST-TOOLS]]: same three files, same whole-blob
   cause, the same fix prescribed — **and it had PRE-REGISTERED the exact firing condition that
   had just fired** ("safe today only because its reference build normally builds `lemon` FIRST …
   if that build fails early, the fixture set absorbs the tools too"). It was missed because it is
   spelled `TU-SCRAPE` / `BUILD-HOST-TOOLS`, and neither token was in the search. ⇒ **grep the
   SYMPTOM, the ARTEFACT and the FILE NAMES that appear in the evidence (`lemon.c`, `whole-blob`,
   `link-line`), never only the title you have in mind** — a name-shaped grep finds the rows you
   would have written, not the rows that exist. ⚠ Two costs, and the second is the larger: the
   duplicated investigation, and the SPLIT AUDIT TRAIL — the original row's closing work demanded
   the migration "show the BEFORE and AFTER sets and account for every difference", a requirement
   the new row did not carry, so it was nearly satisfied by a COUNT match alone. **When a duplicate
   is discovered: update the ORIGINAL in place, cross-reference both, delete NEITHER.**
1c. **★ READ THE ASSERTION VALUES, NOT THE TEST NAMES.** ✔MEASURED the same day: a 57-failure
   population named `wal2-*`, `walsetlk-*`, `journal3-*`, `e_walauto-*` was diagnosed as the
   WAL/journal *timing* family and routed to a known clock defect. The values said
   `expected [00644 00400 00644]` / `got [00777 00555 00777]` — it was the file-**permission**
   family, and only 1 of the 57 was clock-related. A test's NAME is a label someone chose; its
   ASSERTION is the measurement. Reading the values is what cracked it.
2. **Never state what a loader/parser/config accepts, rejects, or defaults to without
   reading it.** "The sibling family does X" is a hypothesis. (Original case: `.format.json`
   was asserted twice to enforce a closed root-key set, and at the time it did not — unknown
   keys were silently ignored, so a typo'd capability presented as "unsupported".)
   ★★ **AND THIS VERY EXAMPLE THEN BECAME THE RULE'S BEST DEMONSTRATION — AGAINST THE
   OPERATOR, TF-C97.** TF-C75 gave `.format.json` a real closed key set
   (`kFormatDocumentKeys` + the rejection loop in `src/link/object_format_schema_json.cpp`),
   so unknown keys are now REJECTED. Nobody updated this note, and the operator kept quoting
   it as a live fact for several cycles — including into a delegation brief — until an agent
   read the loader and refuted it. **A "known trap" recalled from memory is a claim like any
   other, and a stale caution is worse than none: it points at the wrong hazard.** Here the
   real risk was the exact INVERSE of the warning — adding a key to the 24 format files
   WITHOUT adding it to the vocabulary would have broken every shipped format at LOAD, while
   the note had you bracing for a silent no-op. **Re-read the loader every time; do not trust
   this list, including this entry.**
3. **Re-validate after EVERY edit, not once per file.** JSON parse, `bash -n`, parse-check.
   The *second* edit is the one that breaks it. (Real case: a validated `stdio.json` was
   edited again and shipped with unescaped quotes, breaking every `#include <stdio.h>`.)
4. **Never read state a background job is writing.** A racing read is not evidence.
   Snapshot, or wait for the job.
5. **Capture exit codes DIRECTLY, never after a pipe.** Use `${PIPESTATUS[0]}` or capture
   then filter. Use **heredocs** for anything containing quotes — commit messages, JSON,
   prose — never `-m "…"`.
6. **Before ANY comparison/experiment: list the variables and state which are controlled.**
   If a default differs between arms (stack reserve, CRT, optimisation, arch, toolchain),
   the experiment is VOID until matched. Put this in the delegation brief so the agent can
   catch the operator. (Real case: a native-vs-DSS control specified as-built would have
   concluded "DSS frames are the bug" — MinGW reserves 2 MB, DSS 1 MB.)
7. **A hypothesis written into an anchor is a LABELLED SUSPECT** until a test kills or
   confirms it — and when it dies, the refutation stays in the row so it cannot be
   re-proposed.
8. **When a delegated agent contradicts the operator, that is the system working.** Verify
   its claim, then update. Never defend the original because the operator wrote it.
9. **Prefer fixing the CLASS over the instance.** Excluding the failing case or patching
   around it treats the symptom; the next instance will be a different case.
