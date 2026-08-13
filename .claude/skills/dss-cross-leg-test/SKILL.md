---
name: dss-cross-leg-test
description: >
  Run and adjudicate the DSS Code Prime CROSS-LEG matrix for a real-world client corpus — host × leg
  × {BUILD, RUN} × {CLI, UNITS}, plus the ROUND TRIP half where a built artifact must actually
  EXECUTE on its target platform. Use this whenever the user asks to run the cross-leg matrix, the
  leg matrix, or the full multi-platform test; asks whether every host still builds every target;
  asks to verify sqlite (or another client corpus) across Windows, Linux/WSL, macOS, or the arm64
  VPS; or asks to prove a built artifact runs where it was aimed — even if they never say "skill".
  BUILD is absolute on every host; RUN is legitimately restricted by platform (nothing runs Mach-O on
  Windows or Linux). Each host uses ITS OWN driver. If no client is named it ASKS — it never assumes
  sqlite. NOT for running the ordinary local ctest gate or a development cycle (use dss-cycle), and
  NOT for auditing committed code (use dss-audit).
user-invocable: true
argument-hint: "[client name, e.g. sqlite]"
---

# DSS Code Prime — cross-leg matrix test

Two artifacts (**CLI**, **UNITS**) × two verbs (**BUILD**, **RUN**) × every (host × leg), plus the
round trip: a built artifact must execute on its target platform.

## When to use

- Verifying that every host still builds every target after a change to codegen, linking, or drivers.
- Proving a cross-built artifact actually runs where it was aimed, not merely that it linked.
- Re-establishing the matrix before a release or after a toolchain change.

**Not this skill:** the ordinary local ctest gate or a development cycle → `dss-cycle`. Auditing
committed code → `dss-audit`.

## Step 0 — the client, ASK IF NOT PROVIDED

This skill is **client-parameterised**. The client is the real-world corpus under test.

- Today the only implemented client is **`sqlite`** (`real-examples/c/sqlite/`).
- **If the invocation does not name a client, STOP AND ASK.** Do not default to sqlite — the whole
  point of the parameter is that more clients are coming, and silently picking one produces a verdict
  labelled with the wrong subject.
- If a client is named but not implemented, say so plainly, list the implemented ones, and ask
  whether to proceed with a different one. Do not invent a harness.

A client must supply a driver pair (one per host family), a leg catalogue, a shared resolver both
drivers hard-require, a CLI artifact plus smoke gate, a UNITS corpus plus tier, and a
**reference/oracle build**. ★ **The oracle is not optional** — without a same-platform reference
build you cannot separate a DSS defect from an upstream or environment one, and every attribution
becomes an argument.

## The two verbs, and why they differ

**BUILD is absolute.** Every host must build every leg, using that host's own driver. A leg reporting
`skipped-build-input-missing` on **any** host is a **FAILURE**, not an accepted environmental skip.
The acceptance test is mechanical: `skipped-build-input-missing occurrences: 0`.

⚠ **"The provider now has a dispatch arm" is NOT the criterion — the ARTIFACT must exist on that
host.** This row has been over-claimed on inference before.

**RUN is legitimately restricted.** Nothing executes Mach-O on Windows or Linux; no Darwin→Linux
launcher exists. Those stay **classified** skips and do not fail the matrix. What is unacceptable is
an *unclassified* skip: every not-run outcome carries a token from the closed vocabulary
(`skipped-by-runOn`, `skipped-emulator-missing`, `skipped-build-input-missing`, …). An empty token
means a leg vanished without a verdict, which is a harness defect.

## Workflow

1. **Establish the client** (step 0 above). Ask if it was not named.
2. **Per host, drive the client's own driver** — `build-and-test.sh` on Linux/WSL/macOS/VPS,
   `build-and-test.ps1` on Windows. **Do not delegate the runs to an agent**: a delegated build agent
   reliably yields mid-build and leaves an orphaned job. Drive them foreground-blocking, or via a
   harness-tracked background command that re-invokes you on exit.
   ```bash
   DSS_TIER=veryquick DSS_CONFIG=release bash ./build-and-test.sh
   ```
3. **Collect every cell** — host × leg × {BUILD, CLI run, UNITS run}, plus the round-trip rows.
4. **Attribute every red cell** before blaming the compiler — DSS, upstream, or environment, with the
   oracle's output as the evidence.
5. **Adjudicate** against the five conditions below and render the report.

## Adjudicating — GREEN requires all five

1. `skipped-build-input-missing` is **0** on every host. *(the absolute requirement)*
2. Every not-run cell carries a **classified** token, each either ⬛ structural or ⬜
   environmental-with-a-named-missing-tool.
3. Every leg that DID run is green, or red **only** on failures attributed to a provenanced confound.
4. `poisoned` is **0**. A poisoned leg is a build that did not happen, wearing a run verdict.
5. Both round-trip evidence rows exist for anything newly built.

⚠ **`0 poisoned` and `0 environmental` are the two numbers that quietly decide the verdict.** A run
can show impressive corpus counts while three legs never compiled.

## Output contract

```
CROSS-LEG MATRIX — client: <name>   compiler @ <sha>   corpus @ <upstream sha>

BUILD (absolute)     : <n>/<n> host×leg cells   build-input-missing: <n>   poisoned: <n>
RUN                  : <n> verified · <n> structural · <n> environmental
ROUND TRIP           : <n>/<n> cells proven by execution

[Table 1 — host × leg × {BUILD, CLI run, UNITS run}]
[Table 2 — round trip]

VERDICT: GREEN | RED (<the failing cells, named>)
Attribution for every red cell: DSS | upstream | environment — with the oracle's output.
Anchors opened/closed this run.
```

★ **State verified-by-execution separately from verified-structurally, always.** An argument that a
provider "reads no host identity, so it must work everywhere" is sound reasoning and is *not* a
measurement. Both belong in the report; conflating them is how this matrix got over-claimed before.

## File map

- Read `references/report-shape.md` when filling the report — the worked detail behind both tables,
  the cell vocabulary, and what each column must carry.
- Read `references/hosts-and-drivers.md` before running anything — which driver each host uses, and
  the operational rules that have each already cost a run.
- Read `references/attribution.md` at step 4 — the two attribution traps this project actually fell
  into, why to grep the registry before commissioning an experiment, and the known confound families
  with their per-leg provenance.

## Failure modes this skill exists to prevent

- **Accepting a build skip.** `skipped-build-input-missing` is never environmental; it is a failure.
- **Assuming the client.** Defaulting to sqlite mislabels the verdict's subject.
- **Patching the staged upstream tree or excluding a test file to reach green.** The corpus is
  unmodified upstream; an upstream bug is root-caused, anchored, and reported upstream, and the
  harness survives it.
- **Letting one bad unit cost the other thousand.** A fixture abort is recoverable — name the file,
  resume after it, report the union. But a segment completing **zero** files with the **same** error
  is a precondition failure, not a resumable crash, and must say so rather than burn its resume budget.
- **Reporting a defect instead of handling it.** Every issue found is anchored *and* fixed; if it
  cannot land now it needs a named blocker and a trigger, not "later".
- **Conflating structural reasoning with execution evidence.**
