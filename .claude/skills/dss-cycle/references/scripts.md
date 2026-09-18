# The repository's scripts — what exists, and the rule for using it

**Read this before writing a script, a probe, or a one-off shell pipeline.** The
cycle's most common self-inflicted wound is re-implementing something that is
already here, in a form that has not yet been taught the edge case the original
learned the hard way.

## ★★★ THE RULE (operator instruction, 2026-08-19)

> *"if a tool has a problem, fix before using again, not workaround an own tool.
> reusable tools exists to avoid bunch of problems like mangling or edge cases"*

Three clauses, and all three are load-bearing:

1. **Check this list first.** If a script covers the job, use it. Not "something
   like it" typed inline — the script.
2. **A defect in one of these is FIXED, not routed around.** A workaround at the
   call site leaves the defect in place for the next caller and silently forks
   the behaviour. Fixing a shared script is ordinary in-scope cycle work; by the
   2026-08-15 ruling **no hard stop gates a fix**, anywhere.
3. **The fix carries the usual bar** — the script is one of the shared surfaces,
   so a repair gets a real diagnostic, a test that fails without it, and an
   anchor if anything is left undone.

⚠ These scripts hold this project's accumulated edge cases: `wsl.exe` quoting
(a variable once became `rsync -a --delete / /` and reported exit 0), heredocs
eating backslashes, `/mnt/c` clock skew, `rsync` excludes that must be anchored,
non-interactive ssh dropping `/opt/homebrew/bin`, `command -v` lying over ssh on
macOS. Re-typing the pipeline inline re-opens all of them at once.

## ★★ MANDATORY: creating, changing, or deleting a script updates this file

A cycle that adds a script, removes one, renames one, or changes what one is
**for** updates this reference **in the same commit** — the same rule the handoff
follows, and for the same reason: a reference that ships one commit late is a
reference the next reader cannot trust.

This is enforced, not merely asked. Each script declares its purpose once in a
`PURPOSE:` line in its own header; the table below and the one in
`scripts/README.md` are generated from those declarations and verified against
them by the `scripts_index_guard` ctest entry. A script whose purpose drifts from
its index entry is a **red gate**.

```bash
python scripts/check-scripts-index/check-scripts-index.py --write
```

## Layout

One directory per script, named for the script, siblings inside it:
`scripts/<name>/<name>.{sh,ps1,py}`, plus that script's own assets. The
**primary** script — the one to invoke — is `<name>.sh`, else `<name>.py`, else
`<name>.ps1`. Pairs are **capability-paired**: a change to one sibling lands in
the other in the same commit.

<!-- BEGIN GENERATED SCRIPT INDEX -->
| Script | Runs as | Purpose |
| --- | --- | --- |
| **`anchors`** | `anchors.py` | read and write deferred-anchor registry rows in the one canonical form, so a row is never hand-assembled. |
| **`apply-registry-row`** | `apply-registry-row.py` | replace one deferred-anchor registry row with a lane's verbatim row text from a file. |
| **`burndown-queue`** | `burndown-queue.py` | re-derive the prioritized burndown queue from the registry, production errors first. |
| **`check-anchor-balance`** | `check-anchor-balance.py` | refuse a cycle that ends with more OPEN deferral-registry rows than it began. |
| **`check-anchor-registry`** | `check-anchor-registry.ps1`, `check-anchor-registry.sh` | refuse a `D-*` anchor cited in a scanned root that resolves to no registry row, and refuse a markdown table row whose unescaped pipes would silently drop cells. |
| **`check-diagnostic-codes`** | `check-diagnostic-codes.py` | refuse a duplicate, implicitly-numbered, or newly-uncovered `DiagnosticCode` ordinal. |
| **`check-doc-census`** | `check-doc-census.py`, `source-census.py` | refuse a documented figure that a census refutes, in prose or in a source comment, and repair it in place. |
| **`check-enum-name-table-guards`** | `check-enum-name-table-guards.py` | refuse an `EnumNameTable` vocabulary declared in `src/` without a `DSS_CHECK_ENUM_NAME_TABLE` well-formedness assert. |
| **`check-export-macro-placement`** | `check-export-macro-placement.py` | refuse DSS_EXPORT on a member of an already-exported class, which is MSVC error C2487. |
| **`check-guard-output-encoding`** | `check-guard-output-encoding.py` | refuse a Python script whose report cannot carry a non-cp1252 character through a pipe. |
| **`check-line-endings`** | `check-line-endings.ps1`, `check-line-endings.sh` | refuse a tracked text blob that carries a CR, and a CR instrument that cannot see one. |
| **`check-lsp-coordinates`** | `check-lsp-coordinates.py` | refuse a raw coordinate conversion in src/lsp/ outside lsp_coordinates.cpp — the anti-regression device for LSP positions resolved in synthesized preprocessor coordinates. |
| **`check-ninja-deps`** | `check-ninja-deps.py` | refuse a gate over a build directory whose objects recorded no header dependencies. |
| **`check-no-abort-in-tests`** | `check-no-abort-in-tests.py` | refuse a new live `abort()` call site in test or test-support code. |
| **`check-orphan-tests`** | `check-orphan-tests.ps1`, `check-orphan-tests.sh` | refuse a test source that no CMake target compiles and no ctest entry runs. |
| **`check-path-identity`** | `check-path-identity.py` | refuse a second path canonicalizer -- path resolution lives in exactly one place. |
| **`check-pkg-pipeline`** | `check-pkg-pipeline.py` | pin the package pipeline's release-path step sequence and refuse an artifacts-only run that can reach a release. |
| **`check-plan-citations`** | `check-plan-citations.py` | refuse a new `path:line` citation in the plans -- a citation names a stable reference, never a line number. |
| **`check-retyped-closed-sets`** | `check-retyped-closed-sets.py` | census the diagnostics that RETYPE a closed vocabulary instead of projecting it. |
| **`check-scripts-index`** | `check-scripts-index.py` | refuse a script that no index documents, and an index entry that no script backs. |
| **`check-shell-portability`** | `check-shell-portability.py` | refuse a tracked shell script that cannot run on bash 3.2 without declaring it. |
| **`check-stale-blockers`** | `check-stale-blockers.py` | list OPEN registry rows whose Closing-work cell waits on a blocker that has since CLOSED. |
| **`check-stale-refusal-citations`** | `check-stale-refusal-citations.py` | refuse a new present-tense refusal sentence that cites an anchor row already marked CLOSED. |
| **`check-wall-clock-in-tests`** | `check-wall-clock-in-tests.py` | refuse a new wall-clock duration literal in test code outside the shared measured budget. |
| **`check-wrapped-anchor-ids`** | `check-wrapped-anchor-ids.py` | refuse a NEW anchor id split across a line break, which no grep can ever return. |
| **`cmake-import`** | `cmake-import.ps1`, `cmake-import.py`, `cmake-import.sh` | convert a CMake project into a DSS `.dss-project.json` manifest. |
| **`compile-bench`** | `compile-bench.py`, `compile-bench.sh` | time dsscp against gcc/clang/MSVC/tcc on ONE host over a subject size ladder, naming every reference it could not find. |
| **`corpus-census`** | `corpus-census.ps1`, `corpus-census.py`, `corpus-census.sh`, `test-corpus-census.py` | census the real-example corpus into a run-identified report instead of one overwritten log. |
| **`examples-census`** | `examples-census.py` | re-derive every corpus-manifest figure examples/README.md states, by parsing the manifests. |
| **`lane-fold`** | `lane-fold.py` | seed a lane worktree from the main tree, fold only that lane's real changes back, and land it with its rows applied and its evidence preserved. |
| **`lane-worktree`** | `lane-worktree.ps1`, `lane-worktree.sh`, `test-lane-worktree.sh` | create and remove lane worktrees inside the ignored .worktrees/, refusing any root that would exceed Windows MAX_PATH. |
| **`leg-tree`** | `leg-tree.sh`, `test-leg-tree.sh` | put a gate host's own clone on the tree under test before a leg, and restore it to pristine afterwards. |
| **`owning-tree`** | `owning-tree.py` | name the DSS tree a script's own file lives in -- walked up from that file, never taken from the caller's working directory or git environment. |
| **`pragma-profile-census`** | `pragma-profile-census.ps1`, `pragma-profile-census.py`, `pragma-profile-census.sh` | census `#pragma` usage across the corpus and hold the profile to its expected shape. |
| **`profile-compile`** | `profile-compile-dispatch.sh`, `profile-compile-support.py`, `profile-compile.sh` | compile one fixed subject with a RELEASE dsscp on this host and report where the time went, so the HOST is the only variable across legs. |
| **`refresh_landing_log`** | `refresh_landing_log.py`, `test_refresh_landing_log.py` | regenerate the PR landing-log hash anchors in the plans from git log. |
| **`repo-secrets`** | `repo-secrets.sh` | print the `.secrets` directory a checkout must read, following a lane worktree back to the main checkout that holds it. |
| **`repo-tree`** | `repo-tree.ps1` | resolve the repository tree a PowerShell script is standing in, and keep that script's git enumeration and its file reads on the SAME root. |
| **`sqlite-round-trip`** | `sqlite-round-trip.py` | carry a sqlite leg's DSS-built artefacts to a machine that runs their target and prove they EXECUTE there (the round trip). |
| **`sqlite-runtime-bench`** | `sqlite-runtime-bench.py` | measure the RUNTIME of an emitted sqlite3 binary, the standing runtime-differential instrument. |
<!-- END GENERATED SCRIPT INDEX -->

## The ones a cycle reaches for most

★★★ **THE GATE RUNNERS ARE NOT IN THIS DIRECTORY ANY MORE.** Building a leg,
testing one, putting a host's copy on the tree under test and reaching a carriage
are `dssharness` verbs reading `.harness-config/config.json`. `dssharness legs`
lists what can run where; `dssharness help <topic>` and `<verb> --help` are the
interface, and a brief states one only if its author has RUN it.

- **`dssharness build` / `test`** — each leg in its own variant-keyed build
  directory inside the tree the command runs in. The witness discipline is theirs:
  a pass needs exit 0 **and** a declared `successPattern` matching the command's
  own output, never anything the harness wrote. A tree that moved under the run, a
  build directory another live run holds, and a zero exit with no witness each
  report as themselves, with their own exit codes.
- **`dssharness sync`** — puts a host's copy in step with this tree, deleting what
  the source no longer has and verifying the copy afterwards. It writes by content
  hash, so an unchanged file's mtime never moves and incremental builds survive a
  transport. `--dry-run` lists every path it would write.
- **`dssharness host-exec --ssh <host>` / `--wsl <distro>`** — run a DssHarness
  command in that host's own checkout. ⚠ It runs a DssHarness command, never an
  arbitrary shell string; a procedure of this repository's own belongs in a
  predefined runner under `.harness-config/runner/actions`.
- **`dssharness check-ci-legs`** — read the CI verdict per leg from job metadata.
  ★ It exits **2** when the matrix did not run at all, because an empty answer is
  indistinguishable from every leg passing and must never be read as one.
- **`check-anchor-balance`** — the mandatory end-of-cycle receipt. The report
  line's numbers come from here; never from a previous message.
- **`check-line-endings`**, **`check-orphan-tests`**, **`check-scripts-index`** —
  ctest-wired guards. They run anyway; run them early when a change touches plans,
  scripts, or test wiring.

<!-- CR-INSTRUMENT-QUOTED:BEGIN — this section QUOTES the blind idioms in order
     to warn about them; it does not run one as a measurement. -->

## ★★★ NEVER HAND-ROLL A LINE-ENDING CHECK — the idiom lies on this host

**Asking "are THESE files clean?" has an entry point. Use it:**

```
scripts/check-line-endings/check-line-endings.sh  --files PATH...   # or --files-from -
scripts/check-line-endings/check-line-endings.ps1 --files PATH...
```

Exit **0** all clean · **1** a CR was found · **2** a path could not be measured
(missing, unreadable, a directory) — never a silent skip. It works on tracked,
untracked and outside-the-repo paths, so a lane's scratchpad is fair game.

⚠⚠ **DO NOT write your own.** ✔MEASURED 2026-08-27 (P42) on Git Bash against a
`printf 'a\r\nb\n'` control verified by `od -c` to hold exactly one CR, beside a
pure-LF twin — **the obvious instruments are wrong in BOTH directions:**

| what you would type | CRLF file | pure-LF file | correct |
|---|---|---|---|
| `grep -c $'\r'` (captured in `$(...)`) | **2** | **2** | 1 / 0 — false POSITIVE, always |
| `awk '/\r$/'` | **0** | **0** | 1 / 0 — **false NEGATIVE, always** |
| `sed -n '/\r/p'` | **0** | **0** | 1 / 0 — false NEGATIVE |
| `tr -dc '\r' < f \| wc -c` | **1** | **0** | ✅ correct |

- **The false positive:** the literal `$'\r'` written *inside* a command
  substitution expands to the **empty string**, so `n=$(grep -c $'\r' f)` runs
  `grep -c ''` and returns the file's **line count** — on a clean file too.
  (A CR held in a *variable* is fine; `$'\t'` in the same spot is fine. It is
  this spelling, in this position.)
- **The false negative, the dangerous one:** Git Bash `grep` and `sed` read in
  **text mode** and strip the trailing CR *before matching*, so they report a
  clean tree over a file that is entirely CRLF. `grep -U` sees it; `-a` does not.
  A *mid-line* CR is found by everything — the blindness is aimed precisely at
  the only CR anyone hunts.
- ★★ **This is why it survived: under WSL/Linux all three are CORRECT.** An
  idiom sanity-checked on Linux, or read out of the GNU manual, looks sound and
  then lies only on Windows — the primary dev host. **An instrument verified on
  the wrong leg is verified nowhere.**

A lane in P42 certified thirteen files "pure LF" with `awk '/\r$/'` and was
measuring nothing. `line_endings_guard`'s **Check F** now refuses these
spellings repo-wide, so they cannot be committed. If a line *quotes* the idiom
as documentation, put `CR-INSTRUMENT-QUOTED` on it, or wrap the block in
`CR-INSTRUMENT-QUOTED:BEGIN` / `:END` — the guard uses that same marker for its
own warnings rather than exempting itself by path.

<!-- CR-INSTRUMENT-QUOTED:END -->

## Where the rest of the gate battery is documented

`references/gate-and-cross-plan.md` gives the full fail-loud battery in the order
a cycle runs it, including which of these scripts each step invokes and what a
green line from it looks like. This file answers *what exists*; that one answers
*when to run it*.
