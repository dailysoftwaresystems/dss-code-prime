---
name: dss-cross-leg-test
description: >
  Run and adjudicate the DSS Code Prime CROSS-LEG matrix for a real-world client corpus
  (today: sqlite; the skill is client-parameterised for the ones that follow). The matrix
  is host × leg × {BUILD, RUN} × {CLI, UNITS}, plus the ROUND-TRIP half — a built artifact
  must actually EXECUTE on its target platform. BUILD is an absolute requirement on every
  host; RUN is legitimately restricted by platform (nothing runs Mach-O on Windows or
  Linux). Each host uses ITS OWN driver: Windows → build-and-test.ps1, Linux/WSL and macOS
  → build-and-test.sh. If no client is named, the skill ASKS — it never assumes sqlite.
---

# DSS Code Prime — cross-leg matrix test

Answers one question, for one client corpus, with evidence rather than inference:

> **Does every host build every leg, and does every artifact we build actually run
> somewhere real?**

---

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

### 1c. The report shape

Emit both tables. Never collapse them — they answer different questions.

**Table 1 — host × leg**, one cell per (host, leg), each cell carrying four facts:

```
| host (driver)      | leg            | BUILD | CLI run        | UNITS run                |
|--------------------|----------------|-------|----------------|--------------------------|
| Windows (.ps1)     | pe64-x86_64    | ✅    | ✅ 14/14       | ✅ 0 / 330,970           |
| Windows (.ps1)     | elf64-x86_64   | ✅    | ✅ 14/14 (wsl) | ✅ 2 / 330,435 (2 conf.) |
| Windows (.ps1)     | macho64-arm64  | ✅    | ⬛ structural  | ⬛ structural            |
| macOS (.sh)        | macho64-arm64  | ✅    | ✅ 14/14       | ✅ 1 / 331,745 (1 conf.) |
| arm64 VPS (.sh)    | pe64-x86_64    | ✅    | ⬜ no wine     | ⬜ no wine               |
```

Legend, and use exactly these — the distinction is the point:
- ✅ ran, green
- ❌ ran, red *(name the failures)*
- ⬛ **structural** — this platform cannot execute this format, ever
- ⬜ **environmental** — a declared launcher exists but is absent here *(name it)*
- 🔴 **BUILD FAILURE** — the only cell type that fails the matrix outright

**Table 2 — the round trip. THE COMPLETE ENUMERATION, NOT A SAMPLE.** A build that never
executes on its target is half a claim.

★★ **THE OBLIGATION IS EVERY CELL, INCLUDING THE ONES THAT LOOK EXOTIC.** macOS must build
ELF *and* PE, and those must run on Linux and Windows. Linux must build Mach-O *and* PE, and
those must run on macOS and Windows. Windows must build Mach-O *and* ELF, and those must run
on macOS and Linux. **Cross-emission is only half the claim — the artifact has to behave on
the machine it was emitted for**, and the only way to know is to carry it there and run it.

⚠ **DO NOT SHIP THIS TABLE AS AN ILLUSTRATION.** Enumerate all **4 hosts × 5 legs = 20 build
cells**, each for **BOTH artifacts** (CLI and UNITS) — 40 obligations. A partial table reads
as coverage; that is exactly how this matrix was under-claimed before. A cell where the
BUILD HOST can itself execute the target (Windows→pe64, macOS→macho, Linux→elf) is satisfied
natively and says so — it is still a row, never an omission.

```
| built on   | target         | must execute on      | CLI  | UNITS | evidence / gap        |
|------------|----------------|----------------------|------|-------|-----------------------|
| Windows    | pe64-x86_64    | Windows (native)     |  ✅  |  ✅   | 14/14 · 0/330,970     |
| Windows    | elf64-x86_64   | Linux / WSL          |  ✅  |  ✅   | 14/14 · 330,436 tests |
| Windows    | elf64-arm64    | VPS or qemu-aarch64  |  ⬜  |  ⬜   |                       |
| Windows    | macho64-arm64  | macOS (native)       |  ✅  |  ⬜   | TF-C113               |
| Windows    | macho64-x86_64 | macOS (Rosetta)      |  ⬜  |  ⬜   |                       |
| macOS      | macho64-arm64  | macOS (native)       |  ✅  |  ✅   | 14/14 · 1/331,745     |
| macOS      | macho64-x86_64 | macOS (Rosetta)      |  ✅  |  ✅   | 14/14 · 1/331,741     |
| macOS      | elf64-arm64    | Linux / VPS          |  ⬜  |  ✅   | 0 errors / 192        |
| macOS      | elf64-x86_64   | Linux x86_64         |  ⬜  |  ⬜   |                       |
| macOS      | pe64-x86_64    | Windows              |  ⬜  |  ⬜   |                       |
| Linux/WSL  | elf64-x86_64   | Linux (native)       |  ✅  |  ✅   |                       |
| Linux/WSL  | elf64-arm64    | qemu / VPS           |  ⬜  |  ✅   |                       |
| Linux/WSL  | pe64-x86_64    | Windows              |  ✅  |  ⬜   | banner+CRUD+integrity |
| Linux/WSL  | macho64-arm64  | macOS                |  ⬜  |  ⬜   |                       |
| Linux/WSL  | macho64-x86_64 | macOS                |  ⬜  |  ⬜   |                       |
| arm64 VPS  | elf64-arm64    | VPS (native)         |  ✅  |  ✅   | 14/14 · 1/331,333     |
| arm64 VPS  | elf64-x86_64   | Linux x86_64         |  ⬜  |  ⬜   |                       |
| arm64 VPS  | pe64-x86_64    | Windows (native)     |  ✅  |  ✅   | CRUD+integrity · 0/192|
| arm64 VPS  | macho64-arm64  | macOS (native)       |  ✅  |  ✅   | CRUD+integrity · 0/192|
| arm64 VPS  | macho64-x86_64 | macOS (Rosetta)      |  ✅  |  ✅   | CRUD+integrity · 0/192|
```

★ **Note what this table makes visible that Table 1 hides**: Table 1 can be entirely green —
every host builds every leg — while most of Table 2 is empty. Those are *different claims*.
"It compiled for that target" and "it works on that target" are separated by exactly the
class of defect this project has already shipped once (see below).

★★ **THE TRANSPORT MUST CARRY WHAT THE HARNESS STAGES — otherwise the round trip tests
something you would never ship.** ✔MEASURED 2026-08-06: a VPS-built `testfixture.exe` was
hand-carried to Windows with its DLLs but WITHOUT Tcl's script library. All **192 tests
passed** and the process still exited **rc=1**, on `unknown encoding "cp1252"` raised from
`finish_test` — i.e. a non-zero exit that looked like a failure, arriving AFTER every
assertion had already succeeded. Staging the library and setting `TCL_LIBRARY` flipped it
to rc=0. Same family as the macOS `init.tcl` wall: **a library's code is not all a library
needs.** When transporting an artifact by hand, carry its `scriptLibraryDir` (or the target
host's own staged copy) and set the loader/data variables the driver would have set.

⚠ **The UNITS round trip is heavier than the CLI's and must not be quietly skipped for that
reason.** Running a cross-built testfixture on the target needs the test corpus and Tcl's
script library staged there too. That is a logistics cost, not a licence to report the CLI
cell and leave UNITS blank — mark it ⬜ with the missing input named, never omit the row.

★ **Why this half exists, in one measured example:** a cross-built `sqlite3` once loaded,
ran, and printed a correct version banner **while silently corrupting every database it
wrote** (a `$INODE64` misbinding). Compiling and linking is not evidence that an artifact
behaves. For the CLI, the round-trip evidence must include a **durable-state check** — write
a FILE database, reopen it in a separate process, `PRAGMA integrity_check`. Never `:memory:`;
it has concealed a crash in this project before.

---

## 2. Hosts and drivers

| host | driver | notes |
|---|---|---|
| Windows | `build-and-test.ps1` | pe64 native; ELF legs run through the declared `wsl.exe -e` launcher |
| Linux / WSL x86_64 | `build-and-test.sh` | elf64-x86_64 native; elf64-arm64 under `qemu-aarch64` |
| macOS (Apple Silicon) | `build-and-test.sh` | macho-arm64 native; macho-x86_64 under `arch -x86_64` (Rosetta) |
| arm64 Linux VPS | `build-and-test.sh` | native aarch64 — the best control for "is this provider really declaration-driven?" |

★ **One driver per host, and `.sh` on Windows is NOT a supported configuration.** The `.sh`
driver's `case "$(uname -s)"` accepts only `Linux` and `Darwin`, and that is the design, not
a gap — `.ps1` *is* the Windows driver. Its `die` states the real contract. Do not "fix" it:
a second driver on a host that already has one is duplication, not independence, and would
make the two harnesses the same experiment wearing two names.

★ **The VPS is the strongest control in the set.** It is native aarch64 with no Mac, no
Windows and no `/mnt/c`. If `elf64-x86_64` and `pe64-x86_64` build *there*, the providers are
genuinely declaration-driven rather than "worked because the host happened to match".

---

## 3. Running it

Per host, drive the client's own driver. **Do not delegate the runs to an agent** — a
delegated build agent reliably yields mid-build and leaves an orphaned job. Drive them
foreground-blocking or via a harness-tracked background command that re-invokes you on exit.

```bash
# Linux/WSL, macOS, VPS
DSS_TIER=veryquick DSS_CONFIG=release bash ./build-and-test.sh
```

### Operational rules that have each cost a run

- **`SRC_DIR` is mandatory when the checkout is not at `$HOME/src/dss-code-prime`.** The
  harness REFUSES to run rather than silently clone `main` — because an unattended multi-hour
  corpus run would validate a compiler that is not the branch under test, and the results
  section would print that commit as if it were. Set `SRC_DIR=<checkout>` explicitly.
- **Remote runs must survive the session close**: `setsid nohup … < /dev/null &`, then
  **verify with `pgrep`**. A launch line that printed is not a process that is running.
- **Pin Tcl when the host's default disagrees with the legs' libraries** (`DSS_TCL_VERSION=8.6`).
  Every leg's libtcl is 8.6; a host whose default Tcl is 9.0 will otherwise compile against a
  9.0 header and link an 8.6 library. The per-leg coherence check now catches this and says so.
- **Capture rc DIRECTLY, never after a pipe.** `cmd | head; echo $?` reports *head's* status.
  This has produced false "clean" readings in this project more than once.
- **A machine run must pull COMMITTED state**, so it cannot race local edits. That is also
  what makes it safe to launch remote runs while agents are still editing locally.
- **Never quote a corpus number without its upstream commit.** The harness pulls upstream on
  every run, so two runs execute different corpora. `1 error / 331,745` is meaningless without
  `sqlite @ <sha>`.

---

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

## 5. Attribution — before blaming the compiler

**Never report a red cell as a DSS defect without running the oracle.** The client ships a
same-platform reference build for exactly this:

```
wsl.exe -e <stage>/reference-testfixture <the same staged .test>
```

**If the reference fails the same way, it is upstream or environment — not DSS.**

### The two attribution traps this project has actually fallen into

1. **Read the assertion VALUES, not the test NAMES.** A 57-failure population named
   `wal2-*`, `walsetlk-*`, `journal3-*`, `e_walauto-*` was diagnosed as the WAL/journal
   *timing* family and routed to a known clock defect. The values said
   `expected [00644 00400 00644]` / `got [00777 00555 00777]` — it was the file **permission**
   family, and only 1 of 57 was clock-related. A test's name is a label someone chose; its
   assertion is the measurement.
2. **A different population is not a control.** "pe64 passed in the same run" proved nothing,
   because every failing family there was gated `ne "Windows NT"` — pe64 never executed those
   assertions at all.

### Grep the registry BEFORE commissioning an experiment

Search `_deferred-anchor-registry.md` for the leg, the artifact, the test family and the
symptom; cite what you find or state that nothing matched. A 2×2 attribution was once
commissioned from scratch whose identical experiment and verdict were already in the registry
from seven cycles earlier — and the un-cited row would have pre-empted three false statements
that reached a commit. **Recall finds what is similar; grep finds what is the same.**

### Known confound families (sqlite) — all provenanced per leg in `legs.json`

- **WSL2 `CLOCK_REALTIME` oscillates ±34.47 s** — the `walsetlk`/`busy2` timing family.
- **DrvFs has no `metadata` mount option** — `chmod 644 → stat 777`, `400 → 555`. Any
  permission assertion fails when a launched leg's rundir sits on `/mnt/c`.
- **`zipfile-25.0`** — an upstream symlink/`fread` leak, compiler- and filesystem-independent.

⚠ A confound must carry `earnedOn` / `earnedAt` / `mechanism` / `anchor`. A suppression rule
without provenance is either inert or unearned — one shipped for weeks matching nothing at
all, and was invisible precisely because it never fired.

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
