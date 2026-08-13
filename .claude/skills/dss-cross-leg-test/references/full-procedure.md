# The client, the matrix, running, adjudicating, and the hard rules — full text

## 0. The client — ASK IF NOT PROVIDED

This skill is **client-parameterised**. The client is the real-world corpus under test.

- **Today the only implemented client is `sqlite`** (`real-examples/c/sqlite/`).
- **If the invocation does not name a client, STOP AND ASK.** Do not default to sqlite —
  the whole point of the parameter is that more clients are coming, and silently picking
  one produces a verdict labelled with the wrong subject.
- If a client is named but not implemented, say so plainly, list the implemented ones, and
  ask whether to proceed with a different client. Do not invent a harness.

A client must supply, at minimum:

| the client provides | sqlite's answer |
|---|---|
| a driver pair (one per host family) | `build-and-test.sh` + `build-and-test.ps1` |
| a leg catalogue | `legs.json` |
| a shared resolver both drivers hard-require | `harness_legs.py` |
| a **CLI** artifact + a smoke gate | `sqlite3` + a 14-assertion smoke |
| a **UNITS** corpus + a tier | `testfixture` + `veryquick` |
| a **reference/oracle** build for attribution | gcc-built `reference-testfixture` / `reference-sqlite3` |

★ **The oracle is not optional.** Without a same-platform reference build you cannot
separate a DSS defect from an upstream or environment one, and every attribution becomes
an argument. See §5.

---

## 1. THE MATRIX — this is the deliverable

Two artifacts (**CLI**, **UNITS**) × two verbs (**BUILD**, **RUN**) × every (host × leg).

### 1a. BUILD — absolute, no exceptions

> **Every host must build every leg, using that host's own driver.**

A leg reporting `skipped-build-input-missing` on **any** host is a **FAILURE**, not an
accepted environmental skip. This is the operator's standing requirement, and the acceptance
test is mechanical:

```
skipped-build-input-missing occurrences: 0
```

⚠ **"The provider now has a dispatch arm" is NOT the criterion. The ARTIFACT must exist on
that host.** This row has been over-claimed on inference before.

### 1b. RUN — legitimately restricted

Nothing executes Mach-O on Windows or Linux. No Darwin→Linux launcher exists. Those stay
**classified skips** and do not fail the matrix. What is NOT acceptable is an *unclassified*
skip: every not-run outcome must carry a token from the closed vocabulary
(`skipped-by-runOn`, `skipped-emulator-missing`, `skipped-build-input-missing`, …). An empty
token means a leg vanished without a verdict, which is a harness defect.

## 3. Running it

Per host, drive the client's own driver. **Do not delegate the runs to an agent** — a
delegated build agent reliably yields mid-build and leaves an orphaned job. Drive them
foreground-blocking or via a harness-tracked background command that re-invokes you on exit.

```bash
# Linux/WSL, macOS, VPS
DSS_TIER=veryquick DSS_CONFIG=release bash ./build-and-test.sh
```

## 4. Adjudicating the result

A run is **GREEN** for the matrix when **all** of:

1. `skipped-build-input-missing` is **0** on every host. *(the absolute requirement)*
2. Every not-run cell carries a **classified** token, and each is either ⬛ structural or
   ⬜ environmental-with-a-named-missing-tool.
3. Every leg that DID run is green, or red **only** on failures attributed to a
   provenanced confound.
4. `poisoned` is **0**. A poisoned leg is a build that did not happen, wearing a run verdict.
5. Both round-trip evidence rows exist for anything newly built.

⚠ **`0 poisoned` and `0 environmental` are the two numbers that quietly decide the verdict.**
A run can show impressive corpus counts while three legs never compiled.

---

## 6. Hard rules

- **Never patch the staged upstream tree, and never exclude a test file to reach green.**
  The corpus is unmodified upstream. An upstream bug is root-caused, anchored, reported
  upstream, and the harness SURVIVES it.
- **The harness must survive everything.** A fixture abort is a recoverable outcome: name the
  file, resume after it, report the union. One bad unit must never cost the other thousand.
  But a segment that completes **zero** files with the **same** error is a *precondition
  failure*, not a resumable crash — it must say so instead of burning its resume budget.
- **Every issue found is anchored AND fixed** (`dss-cycle` §A.7). Reporting a defect is not
  handling it. If it cannot land now, it needs a named blocker and a trigger — not "later".

---

## 7. Report template

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

★ **State verified-by-execution separately from verified-structurally, always.** An argument
that a provider "reads no host identity, so it must work everywhere" is sound reasoning and
is *not* a measurement. Both belong in the report; conflating them is how this matrix got
over-claimed before.
