# DSS Code Prime — HANDOFF

> **REWRITTEN at the end of every cycle** (`/dss-cycle` Step 8.1) and **READ FIRST at the start of
> every cycle** (Step 0). §1–§4 are a *replacement* — stale lines are deleted, not appended past.
> **§5 TIMELINE is the sole exception and accumulates.** State is what is true now; the timeline is
> how it got here.
>
> Every claim is labelled ✔**MEASURED** / 📄**DOCUMENTED** / 🧠**INFERRED**. An unlabelled claim here
> is a defect: this file is read by someone with no context, which is exactly when an unmarked
> inference does the most damage.

**Last updated:** 2026-09-01 — cycles **P14 … P50**.

---

# §0 — RESUME HERE (a session with no context reads this block first)

**State, ✔measured at the tip and not re-quoted:** branch `feature/c23-conformance-burndown-5`,
**PR #56 OPEN**. ⚠ **NO SHA IS PINNED HERE ON PURPOSE** — a handoff cannot name its own commit,
since writing it moves HEAD, and the public-repo bot rebases and squashes besides. **P50 landed as
TWO commits**: the operator-ruled segment-threshold flip alone, then wave 1; re-derive with
`git log --oneline -4`. `git worktree list` → **the repo only**.

★★★ **P50 RAN ONE SOLO ITEM AND THEN FOUR LANES AT THE CAP, CLOSING SEVEN ROWS — FIVE OF WHICH
THE GATE CAN SEE — AND OPENING TWO THAT ARE NAMED AS P51's FIRST ITEMS.** The solo item was the
operator's R3 from P49: the anchor guard's segment threshold. Wave 1 was `li ch as t2`, every
lane from the PRODUCTION bucket.

⚠⚠ **THE CYCLE'S TWO SHARPEST FINDINGS ARE BOTH ABOUT MY OWN INSTRUMENTS, AND BOTH FAILED IN THE
DIRECTION WHOSE RESULT LOOKS LIKE SUCCESS. Read §0.5 before trusting any tooling here.** One
destroyed a lane's entire evidence tree behind a `&&` that swallowed its own error; the other
deleted a registry row from one file and failed to write it to the other, leaving it in NO
registry — which `check-anchor-balance`, counting rows by name, reads as a CLOSURE. Both are
fixed at the tool and pinned; both are born-closed harness rows.

## §0.1 — The four gate legs, verbatim

```
cmake --build build/dbg  &&  ctest --test-dir build/dbg --output-on-failure -j 12
wsl.exe -e bash /mnt/c/Source/DailySoftware/dss-code-prime/scripts/wsl-leg/wsl-leg.sh --mode full
wsl.exe -e bash /mnt/c/Source/DailySoftware/dss-code-prime/scripts/remote-leg/remote-leg.sh --carriage arm64-vps --mode full
wsl.exe -e bash /mnt/c/Source/DailySoftware/dss-code-prime/scripts/remote-leg/remote-leg.sh --carriage macos     --mode full
```

⛔⛔ **RUN THOSE THREE FROM POWERSHELL, WITH AN ABSOLUTE `/mnt/c/...` PATH, AND NOT FROM GIT
BASH.** Git Bash's MSYS argument conversion rewrites a leading `/mnt/c/...` into
`C:/Program Files/Git/mnt/c/...` before `wsl.exe` ever sees it.

⚠⚠ **NEVER READ A LEG'S — OR A GATE'S — VERDICT THROUGH A PIPE.** `scripts/run-gate/` captures rc
DIRECTLY, refuses to call a run successful without a caller-supplied success witness, and writes
both to the log. Use it for every gate; read the LOG, not the notification.

⚠ **A leg tests the tree AS SYNCED.** ✔P50: three legs were dispatched, a fourth lane then
folded, and all three had to be killed and re-run — they were measuring a tree that no longer
existed. **Fold everything first, then gate.** Killing them left all three hosts clean
(HEAD at the driver commit, `git status` rc=0 with 0 dirty, verified directly), but that is the
carriage's restore working, not a licence to interleave.

⚠ **A `.plans/`-only edit still needs the repo guards re-run** — several read `.plans/**`:
`ctest --test-dir build/dbg -L repo-guard` takes ~2.5 min. **There are 21 of them since P50.**

## §0.2 — The reference toolchains, and WHERE MSVC IS

`DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` is only checkable if all three can be RUN, and each must be
probed **separately** — "the reference" is not one voice.

| reference | version | how to reach it |
|---|---|---|
| gcc | 13.3.0 | inside WSL: `wsl.exe -e gcc` |
| clang | 18.1.3 | inside WSL: `wsl.exe -e clang` |
| mingw-w64 gcc | 13.2.0 | on PATH natively as `gcc` on the Windows host |
| **MSVC** | **19.51.36252 (VS 18 / MSVC 14.51)** | **`C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe`** |
| **Apple clang** | **21.0.0 (clang-2100.1.1.101), MacOSX.sdk 26.5** | **on the Mac, via `scripts/ssh-macos/`** |

★★ **MSVC IS REACHABLE WITHOUT `vcvars` FOR A FRONT-END QUESTION.** `cl.exe /c` compiles only —
write the fixture so it includes **no system header**.
★★★ **AND MSVC IS THE REFERENCE THAT KEEPS CHANGING THE ANSWER. ✔P50, twice:** it SILENTLY
ACCEPTS most `static`↔non-`static` linkage-mismatched redeclarations that gcc rejects, which
narrowed a row's whole reject set; and P49's `_Generic` row had never probed it at all. **A
gcc-only matrix in a row is not a measurement of the union — re-probe all three before closing.**
⚠ `mingw-w64 gcc` and `WSL gcc` are DIFFERENT references and they DISAGREE.

## §0.3 — What is OWED, stated so it is not mistaken for done

1. **`D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE` is THREE cycles old** — opened P47,
   untaken by P48, P49 and P50. Enumeration-root must equal read-root. **Well past the
   no-follow-ups exception boundary: take it in P51 or state plainly why not.**
2. **The `veryquick` integration debt is DISCHARGED** (P49, pe64 leg: 3 errors out of 394,437
   tests, zero DSS-attributable; the P47 confound rows were seen to APPLY and the repaired
   oracle classifier reported `SAME-PLATFORM`). ⓘ What remains unexercised, and is NOT a standing
   exit: the elf/macho corpus legs at this tree, and
   [[D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE]]. `veryquick` still exits 1 by design —
   `vtabH-3.1` is deliberately undeclared; read the ledger, do not chase it.
3. **`speedtest1` is NOT part of the standing exit** — owed only by a cycle that explicitly
   targets compile time or the optimizer pipeline. **P50 does not meet the predicate.**
4. **The clang ASan/UBSan leg is still absent from `scripts/`** — anchored twice
   (`D-CI-ASAN-LEG-WALL-CLOCK-GROWS-WITH-THE-CORPUS`, `D-CI-ARM64-EXAMPLES-NEVER-SANITIZED`).
5. ⚠ **WSL's leg repo carries a SECOND registered worktree, `~/src/dss-sq` at `f8ebafb2`
   (detached), left from P47's lane `sq`.** It is a SIBLING directory, so the leg's own checkout
   reads 0 dirty and the examples glob does not see it — but a gate host is meant to hold the
   repo and nothing else. **Not removed by P50 deliberately:** it is another cycle's artifact and
   `git worktree remove` there could destroy unmeasured work. Measure it, then remove it.
6. **`D-SQLITE-CLI-BUILT-ON-NO-LEG` is CLOSED** (Table 1 20/20, Table 2 40/40). ⚠ Its 40 cells
   were run with **no matched reference control** (`hasReference: false` on every payload), so
   they prove each artefact RUNS CORRECTLY on its target platform — **not** a DSS-vs-gcc
   differential. Do not cite them as one.

## §0.4 — Traps, and the ones that were paid for AGAIN in P50

- ⛔⛔ **`cmd && cmd2` ON ONE LINE HID A FAILED `cp` AND COST A LANE ITS ENTIRE EVIDENCE TREE.**
  ✔P50: the destination's parent did not exist, `cp` failed, `&&` swallowed the echo, and **the
  ABSENCE of output read as success** — then the next command destroyed the only copy.
  ⇒ `lane-worktree remove` now REFUSES a lane whose scratchpad holds files unless given
  `--preserve-to <dir>` (it does the copy and VERIFIES it) or `--discard-scratchpad`. **Use
  `--preserve-to`. Never hand-roll the copy again.**
- ⛔ **A quoted heredoc EATS BACKSLASHES, and so does an inline `python -c` through Git Bash.**
  Any throwaway script goes in a FILE, asserting by `os.path.realpath` PREFIX that it is not
  inside the repository before it deletes anything.
- ⛔⛔ **`grep -P` IS UNUSABLE ON THIS HOST** — *"-P supports only unibyte and UTF-8 locales"* —
  and it **returns 0 silently**. Write a boundary-anchored Python matcher instead.
- ⛔ **A Windows command line caps near 32 KB.** ✔P50: a 56 KB row could not be passed as argv at
  all (`WinError 206`). Splitting the cell to fit would be the workaround; call the same writer
  IN-PROCESS instead (`scratchpad/p50/apply_big.py` is the pattern).
- ⛔ **MSYS ARGUMENT CONVERSION** — see §0.1. **A `| tail` MASKS THE EXIT CODE** — see §0.1.
- ⚠⚠ **A `cd` PERSISTS BETWEEN Bash TOOL CALLS.** ✔Paid again in P50: a `cd .worktrees/t2` made
  the next call resolve a scratchpad path against the wrong root and report "No such file".
- ⚠⚠ **AN INSTRUMENT THAT CANNOT RUN IS NOT A GREEN ONE.** ✔P50: a mutant driver reported
  *"VACUOUS — the pin stayed green"* on an `rc=127` that was `bash` missing from the subprocess
  PATH; the pin had never executed. **Separate CANNOT-RUN from RED from GREEN by the pin's own
  terminal verdict line, never by the exit code alone.**
- ⚠ **A LANE'S BUILD OUTPUTS CAN RIDE ITS EXAMPLE DIRECTORY INTO THE COMMIT.** ✔P50: lane `as`
  left `outa/main` and `outar/main` inside its new `examples/c/…/` directory; `lane-fold` offered
  them as sources. **Read the fold list before applying it.**

## §0.5 — P50 IN ONE PARAGRAPH

**One solo item, then four lanes at the cap.** The cycle opened with the operator-ruled
segment-threshold flip (landed alone as `67714dad`), then wave 1: `li ch as t2`.
✔`check-anchor-balance --base 67714dad` ⇒ **closed 5, opened 2, net −3** COUNTED; **7 closed
and 2 opened IN TRUTH**, because two rows were born-closed inside the cycle and the gate counts
rows by NAME [[feedback-the-gate-cannot-see-a-row-born-in-the-cycle]]. Registry OPEN
**841 → 838**; buckets **PRODUCTION 324 · harness 189 · per-plan 323**, which sums to the gate's
own denominator. ⚠ Both OPENED rows are P51's first items and each NAMES why it was not taken
now — the ruling's "brutally rare exception" clause satisfied explicitly rather than by silence.

**The flip.** `check-anchor-registry`'s core moved `{2,}` → `{1,}` in BOTH twins with four new
self-test arms each (17 → 21), taking **distinct src-cited anchors under guard 1548 → 1652
(+104)**. 98 guard self-test fixture ids stopped being anchor-shaped in SOURCE while every
RUNTIME value stayed byte-identical — `check-anchor-balance.py` alone took 172 token-exact seam
edits applied through Python's tokenizer so a seam could not land inside the wrong literal.
⚠ **Two of my own instrument errors were caught by RUNNING the flipped guard rather than trusting
the planning scan**, and the second is the instructive one: my re-measurement of the twelve
outside-`scripts/` danglers was CIRCULAR — three resolved only through the disclosure row's own
sizing prose, which my closure then rewrote.

**What the lanes landed.** `li` closed the linkage-mismatch and `_Noreturn`-on-object rows;
`ch` routed three target-blind char-signedness sites through the one target-aware chokepoint;
`as` landed R5 plus the class-scoped modifier letters its own precondition named; `t2` took the
sqlite round-trip table **12 → 40 of 40**, closing `D-SQLITE-CLI-BUILT-ON-NO-LEG`.

★★ **THREE OF THE FOUR LANES REFUTED THE BRIEF I GAVE THEM, TWICE IN THE DIRECTION THAT WOULD
HAVE SHIPPED A CONFORMANCE REGRESSION.** `li`'s linkage row carried a gcc-only matrix; ✔MSVC
19.51 SILENTLY ACCEPTS most static-vs-non-static orderings, so the unanimous-reject set the
disjunction permits is far narrower than the row demanded — and `static int f(void);
int f(void){…} static int f(void);` is unanimously ACCEPTED, C 6.2.2p4 inheritance, so the fix
needed propagation across the merge and not just a check. `li`'s `_Noreturn` row demanded a hard
error "expecting unanimous rejection"; ✔**gcc ACCEPTS every non-function `_Noreturn` with a
warning**, so under the disjunction DSS must accept, and it closed as a WARNING. `as` found the
references SPLIT on an FP letter applied to an immediate, and that the arm64 dialect comment's
claim that FP letters render EMPTY on `"r"` operands was simply false. ⇒
[[feedback-a-brief-premise-is-a-hypothesis]], again, and the briefs were mine.

⚠⚠ **AND TWO INSTRUMENTS OF MINE DESTROYED EVIDENCE BEFORE I FIXED THEM. Both are born-closed
harness rows, and both failed in the direction whose RESULT LOOKS LIKE SUCCESS.**
(1) [[D-CYCLE-LANE-WORKTREE-REMOVE-DISCARDS-AN-UNPRESERVED-SCRATCHPAD]] — I ran
`cp -r … && echo preserved` and `lane-worktree remove t2` on ONE command line; the destination's
parent did not exist, `cp` failed, `&&` swallowed the echo, and **the ABSENCE of output read as
success** while the next command destroyed the only copy. Lane `t2`'s 14 result JSONs and its md5
ledger are gone; its row states the loss rather than citing a path that will not open. The same
command was correct for `gi` and `tw` in P49 only because that parent happened to exist.
(2) [[D-GATE-ANCHORS-A-MOVE-IS-NOT-ATOMIC-AND-LOST-A-ROW]] — `anchors.py`'s close-as-move DELETED
a row from production and failed to append it to the archive, leaving it in **NO registry at
all**. ★ **`check-anchor-balance` counts OPEN rows BY NAME, so a vanished row reads EXACTLY like
a closed one** — silent loss reported as progress. Recovered from `git show HEAD:` (never
`git checkout --`, which would have discarded four legitimate closures in the same file), and a
full census — every row NAME at HEAD across all three registries against the working tree,
**2083 → 2084, zero vanished** — proved nothing else had been lost.

## §0.6 — THE NEXT SET

**1. `D-CSUBSET-LINKAGE-INHERITED-INTERNAL-EMITS-GLOBAL`** (P1, opened by P50) — DSS REFUSES a
legal two-TU C program. P50 shipped the semantic half (`SymbolRecord.isInternalLinkage`, OR-
propagated across every merge) and the EMISSION half never asks it: `cst_to_hir`'s linkage fold
reads the definition's own specifier tokens, so an inherited-internal definition emits GLOBAL and
two such TUs collide at `K_SymbolRedefinedAcrossUnits`. All three references build and run it.
**The closing predicate is in the row**; the pin must be a TWO-TU example, a shape the corpus does
not yet have.

**2. `D-CSUBSET-NORETURN-KEYWORD-PARAMETER-AND-TYPEDEF-POSITIONS`** (P2, opened by P50) — the
`_Noreturn` keyword is a PARSE error in a parameter's specifiers and in a typedef head while gcc
accepts both. Below the union; grammar-alternative work in `c.lang.json`, and the semantic arm
that gives it meaning already shipped in P50.

**3. `D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE`** — enumeration-root must equal read-root.
Opened in P47, untaken by P48, P49 and P50. **Three cycles old and well past the exception
boundary. Take it or state plainly why not.**

**4. `D-CSUBSET-C11-THREADS-MACHO-MTX-PLAIN-RECURSIVE`** — still the one `<threads.h>` item open,
a mutex TYPE rather than a function. Small, bounded, finishes the header.

**Then the production P0 band, ✔re-derived at the tip** with
`python scripts/burndown-queue/burndown-queue.py --band P0 --schedulable`. ⚠ **The band is a sort
key, not a verdict — read the row before acting on it.** At the tip of P50:

- `D-CSUBSET-INLINE-FUNCTION-SPECIFIER` — ⚠ read its own text first: the specifier SHIPPED in
  TF-C79 and the row stays open only for witnesses (b) and (c), which are re-measurements of two
  consumer rows. It may be closable by measurement alone.
- `D-CSUBSET-PACKED-ATOMIC-MEMBER` and `D-CSUBSET-PACKED-BITFIELD-INTERACTION` — both were
  ⏳ GATED on `D-CSUBSET-PACKED`, which is ✅ CLOSED, so **the gate has fired and both are
  takeable**. ⓘ The ATOMIC one may be MOOT: `_Atomic` is not yet modelled as a type qualifier, so
  the shape may be inexpressible — measure before designing.
- `D-CSUBSET-POINTER-ARITH-ENUM-INDEX` — guarded fail-loud, not a crash; needs `hir_to_mir`.
- `D-FC7-INLINE-MULTI-PIECE-RETURN` — banded P0 by the sieve but its own text says **P2
  DIVERGENT/LOW**; the silent-miscompile half closed in 2026-06-16. A band-vs-row disagreement,
  and the row wins.
- `D-TARGET-ENCODING-WIDTH-GUARD` — likewise self-described **P4 RECORD/LOW**, rewritten to
  post-FC3.5 truth. Read before scheduling.

⚠ **PICK NON-CONFLICTING FILE SETS.** The two `PACKED` rows and the `INLINE` row plausibly contend
in `src/analysis/semantic/**`; the enum-index row wants `src/mir/lowering/hir_to_mir.cpp`, which
P50's `ch` lane held. Use `scripts/lane-worktree/` + `scripts/lane-fold/` so the check is
mechanical rather than remembered.

★★ **AND THE OPERATOR'S STANDING NOTE STILL GOVERNS:** *"treat its predicates as measured claims,
not settled law, and tell me when one blocks honest work rather than routing around it."* Two of
the five rows listed above disagree with their own band; three lanes refuted their briefs this
cycle. **Read the row, not the summary.**

---

★★★ **P49: EIGHT ROWS CLOSED AND ONE OPENED ACROSS SIX LANES IN TWO WAVES, AND AN OPERATOR RULING TURNED MY ONE-ROW FIX INTO A SEVENTY-ROW DISCLOSURE.** ✔`check-anchor-balance --base c1754511` ⇒ **closed 5, opened 1, net −4** COUNTED; **8 closed and 1 opened IN TRUTH**, three rows born-closed inside the cycle. The one row OPENED was `🔵` disclosed pre-existing debt, which the gate exempts by design. Registry OPEN **846 → 842**; buckets **PRODUCTION 329 · harness 190 · per-plan 323**.

**Wave 1 `vl th fi lt`; wave 2 `gi tw`.** `vl` finished the VLA arc's residue as ONE seam rather than five special cases; `th` measured the pe `struct timespec` layout and found **a live latent miscompile that had already shipped** — `time.json` and `sys/stat.json` declared it FLAT `{i64,i64}` while both list `pe`, so every pe TU wrote 8 bytes into a 4-byte `tv_nsec`, invisible because `sizeof` is 16 and `tv_nsec` sits at offset 8 in BOTH worlds and only the WIDTH moves; `fi` closed the silently-dropped `--resolve-library` identity on a merged archive; `lt` made `lowerCToLir` refuse to report success having produced no instructions, which is what every `tests/lir` pin's meaning rested on; `gi` made a generic selection an integer constant expression; `tw` completed `<threads.h>` on all three object formats.

★★★ **THE OPERATOR RULING IS THE CYCLE.** `scripts/anchors/anchors.py` refused to UPDATE a row the registry already held, because `make_row` enforced the anchor guard's ≥3-segment id rule on every path. I proposed renaming the blocked id. **The operator REFUSED THE RENAME NOW AND LATER** and corrected three of my measurements: the rename costs 291 citations across 87 files (I said 350/90 by counting 13 SIBLING ids a whole-word matcher excludes), it fixes **ONE ROW OF SEVENTY**, and the registry's own ANCHOR-NAME RULE MANUFACTURES the invisible ids — its worked example, `D-CSUBSET-ALWAYS-INLINE` → `D-CSUBSET-ALWAYSINLINE`, takes a four-segment id the guard checks and produces a three-segment id it ignores. **The rule written to prevent guard failures is what produces guard-invisible anchors.** Two rows carry it: [[D-GATE-ANCHORS-WRITER-CANNOT-MAINTAIN-A-ROW-THE-REGISTRY-ALREADY-HOLDS]] (born closed — the segment count became a MINTING rule; `place_row`'s existing-row refusal was always the stronger identity check) and [[D-GATE-ANCHOR-REGISTRY-SEGMENT-THRESHOLD-HIDES-SEVENTY-ROWS]] (the disclosure, closed by P50's flip).

★★ **THE CYCLE'S OWN INSTRUMENTS PRODUCED THREE FINDINGS, ALL FAILING TOWARD *CLEAN*.** The writer seized up hardest exactly where the registry is weakest — the rows it refused to maintain are the ones the guard does not watch. The guard then caught **my own self-test fixture** `D-AAA-BBB-CCC` as an anchor cited with no row, the very class the disclosure describes, demonstrating itself while the disclosure was being written; de-anchored through the fragment pattern, never allowlisted. And `check-anchor-balance` ARM 6 caught `D-CSUBSET-VLA` with a `✅ CLOSED` Status column over a Trigger still leading 🟢.

⚠ **TWO SENTENCES IN THAT SAME CLOSED ROW HAD ALREADY GONE FALSE INSIDE THE CYCLE** — both said a LIR pin *"is RED"*, true when lane `vl` wrote them and false once lane `lt` folded and the pin was inverted. ⇒ **when a lane reports work it could not do because a sibling held the tree, the orchestrator owes the row a second pass after that sibling folds** — the lane cannot, because it is gone.

⚠ **AND TWO CORPUS COMMENTS CITED THE CLOSED VLA ROW AS A LIVE BLOCKER; THE FIX WAS NOT THE GUARD'S SUGGESTED ONE.** ✔I re-measured both through the shipped CLI before touching a word: a FILE-scope VLA still refuses with `S_NonConstantArrayLength` and a VLA struct member with `S_FlexibleArraySoleMember`, and **both refusals are ISO C's own constraints that every reference enforces** — outside anything the closed row deferred. The CLAIM was true and the CITATION was the stale half. Editing those headers then moved the subject lines and reddened `analysis/test_diagnostic_corpus`, whose goldens pin `line:col`; refreshed and verified cell-wise that **only the LINE moved**.

★★ **THE `veryquick` INTEGRATION DEBT FROM P47 WAS DISCHARGED, AND DISCHARGING IT FOUND A FRESH DEFECT.** pe64 leg: **3 errors out of 394,437 tests, zero DSS-attributable**, CLI smoke 14/14; the P47 confound rows were SEEN TO APPLY (`scanstatus-5.1.2` and `sessionnoact-4.3` matched and excused) and the repaired oracle classifier reported `elf64-x86_64: SAME-PLATFORM` — the case that read NO ORACLE on every leg before P47's fix. ⚠ But the first run used a `build/rel` compiler **four cycles stale** while printing `[OK] … PROVED current`: the currency probe answers CONFIG drift, Step 9's `+N files differ from HEAD` compares the WORKING TREE to HEAD, and **neither answers "was this binary built from these sources"** — ✔measured, that line read `+56` byte-identically in the stale run and the fresh one. [[D-HARNESS-PS1-REUSES-A-RELEASE-BINARY-OLDER-THAN-THE-SOURCES-IT-COMPILES]]: the located tree is now REFRESHED, as the `.sh` twin always did. ★ The fix's own first run then caught a defect in the fix — a scriptblock returning `& cmake …; return $LASTEXITCODE` returns `[…ninja output…, 0]`, so a SUCCESSFUL build read as failed.

**Gate, ✔MEASURED at the folded tree, every leg through `scripts/run-gate/`:** Windows **1870/1870** (all 20 repo guards) · WSL x86_64 **1850/1850** · arm64-VPS **1850/1850** · macOS **1850/1850** — 1850 = 1870 minus the 20 root-host-only guards, and that identity is the cross-leg cross-check. ✔Both remote hosts re-verified DIRECTLY after their legs (`HEAD`, `git status` **rc=0** with 0 dirty, 1 worktree) — the rc matters, because a failed status piped to a counter also reads 0 dirty.

---

★★★ **P48: TEN ROWS CLOSED AND ONE OPENED ACROSS SIX LANES IN TWO WAVES, FIVE LANES REFUTED THE PREMISE THEY WERE HANDED, AND THE CYCLE'S MOST INSTRUCTIVE EVENT WAS A CLOSURE OF MY OWN THAT I HAD TO WITHDRAW.** ✔`check-anchor-balance --base dac121cc` ⇒ **closed 8, opened 1, net −7** COUNTED, of which 1 is a bookkeeping closure (`✅🧾`); **10 closed and 1 opened IN TRUTH**, because two rows were born-closed inside the cycle and the gate counts rows by NAME across base and tip [[feedback-the-gate-cannot-see-a-row-born-in-the-cycle]]. Registry OPEN **530 → 523**; buckets **PRODUCTION 333 · harness 190 · per-plan 323**, sum **846**.

**Wave 1 — `pp sm al ff`; `st` replaced `ff` and `cq` replaced `sm` as each exited**, so the operator's cap of four held throughout. Every lane was picked from **P0 WRONG-OUTPUT in the PRODUCTION bucket**.

**What landed:** computed `#include MACRO` now RESOLVES rather than being silently dropped (lane `pp`, and the "minimum" fail-loud path the row offered was refused because all three references resolve it); the Darwin `malloc_zone_t` 176-byte opaque tail is GONE with all 25 members typed and executed on real hardware, and `fsid_t` was confirmed correct so `sys/mount.json` needed no edit at all (lane `ff`); over-aligned stack locals are HONOURED by reserving frame headroom and rounding the address, with no arch-keyed code and no new opcode, config key or diagnostic code (lane `al`); `const` enforcement now reaches every non-plain-identifier lvalue including declared-const fields (lanes `sm` + `cq`); and the shipped type-alias spellings are keyed on `format` **with** `dataModel` instead of `dataModel` alone (lane `st`).

⚠⚠ **A ROW WAS CLOSED THAT SHOULD NOT HAVE BEEN, AND THE CLOSURE WAS WITHDRAWN INSIDE THE SAME CYCLE.** `D-CSUBSET-VLA` read *"ARC COMPLETE"* with an **EMPTY closing-work cell**, which is exactly the `dss-cycle` skill's *"shipped but only the glyph is open"* class. Its own witnesses were verified (six corpus examples present, `ctest` 24/24, `__STDC_NO_VLA__` correctly absent) and it was marked ✅. **Every one of those measurements was true and the conclusion was still false**: ✔gcc 13.3.0 and clang 18.1.3 both COMPILE AND RUN `typedef int R[n]; R a[2];` and `R *p = a;`, which DSS refused — and that exclusion was recorded in a **test comment**, never in the row. ⇒ **an empty closing-work cell is not evidence that nothing is owed; it is evidence that nobody wrote down what was.** [[feedback-a-rows-premise-has-a-shelf-life]] runs BOTH ways: a status cell can go stale toward OPEN, and it can go stale toward COMPLETE. ★ **`check-stale-refusal-citations` is what caught it**, reddening the moment the closure landed over three sentences that assert a refusal while citing the now-closed row. Its own suggested fix — past-tense governors — would have cleared the guard and buried a real conformance gap under three tidy comments. **Read a stale-refusal red as a question about the CLOSURE first and the SENTENCE second.** (P49 closed the row for real, on the residue this withdrawal named.)

⚠⚠ **`lane-fold` WAS SILENTLY DISCARDING EVERY DELETION.** ✔MEASURED folding lane `al`: it printed *"WROTE 17 path(s)"*, exited 0, and left behind the example that lane had REPLACED — one asserting a refusal its own change had made legal. `git status` always reported the `D` record; `classify()` hit `if not os.path.isfile(src): continue` and dropped it. It fails toward keeping stale assertions alive, and the resulting red would have been charged to the lane's compiler work rather than to the tool. FIXED with the same drift-refusal a copy carries, deletions printed on their own line, and two REMOVE-direction self-test arms ([[D-CYCLE-LANE-FOLD-DROPS-A-LANE-S-DELETION]]). ✔It worked in P49 on the first real deletion it met.

★★ **FIVE LANES REFUTED A PREMISE THEY WERE HANDED, AND THREE OF THOSE REFUTATIONS CHANGED WHAT SHIPPED — TWICE BY STOPPING A CONFORMANCE REGRESSION THE ROW ITSELF DEMANDED.** `sm` measured that `D-CSUBSET-POINTER-DIFF-EDGE-CASES`'s requested "loud diagnostic" would have put DSS BELOW the union (MSVC compiles `char* - int*`), and that DSS's EXISTING refusal was already below it and CONTEXT-DEPENDENT — `(int)(a-b)` silent while `long n = a-b;` refused, the same expression judged by where it landed. `cq` measured that supplying the const marker alone would NEWLY refuse `struct S { const int v : 3; };`, which gcc and mingw-w64 gcc compile, and built a severity fork rather than ship the regression. `al` refuted its row's prescribed dynamic-SP-realignment design and built something strictly smaller. ⇒ **a brief's premise is a HYPOTHESIS** [[feedback-a-brief-premise-is-a-hypothesis]].

★ **AND TWO LANES FOUND SILENT DEFECTS OUTSIDE THEIR OWN SUBJECTS.** `al` found `recover_parent_frame_slot` computing a Win64 SEH parent local's address from an **alloca scan index** treated as a slot index — already wrong for any parent carrying a multi-slot local ahead of the referenced one — and fixed it; it then found that its OWN fix would have introduced a NEW silent miscompile in the same area, which nothing had ever exercised because the construct was refused before. `st` found `decodeShippedFor` hard-coding `DataModel::Lp64` for every format, so a test was asking about `(Pe, LP64)` — a pair no target is.

⚠⚠ **THE REGISTRY BECAME THREE FILES DURING THIS CYCLE, BY AN OPERATOR CHANGE MADE IN A PARALLEL SESSION, AND IT LANDED IN THE SAME COMMIT.** Closed rows are ARCHIVED to `.plans/_deferred-anchor-registry-done.md`, the row shape gained two columns — `| Anchor | Priority | Status | Trigger | Closing work | Cross-refs |` with `Priority` `P0`..`P5` and a THREE-VALUE controlled status vocabulary (`✅ CLOSED` / `🟠 OPEN` / `⏳ GATED`) — and `scripts/anchors/` is the deterministic door: `read-anchor`, `read-anchors`, `write-anchor`, `set-anchor`, each a `.sh`/`.ps1` launcher over ONE `anchors.py` so the pair cannot drift. ⇒ **STOP HAND-WRITING REGISTRY ROWS.** ★ **The glyph is still the contract**: `is_closed` is *"the cell OPENS with ✅ after stripping `*_ `"*, complement defined and never enumerated, so the new `CLOSED` word is for the reader and the glyph is what the battery agrees on.

★ **THE COMMIT CARRIED A SECOND CYCLE'S WORTH OF CLOSURES THAT ARE NOT P48's.** The parallel session filed **five rows, ALL BORN CLOSED** — `D-CONFIG-A-LANGUAGE-IS-LOOKED-UP-BY-ITS-DECLARED-NAME-NOT-ITS-DOCUMENT-STEM`, `D-RUNTIME-OBJECT-CACHE-IS-WIRED-TO-NOTHING`, `D-GATE-ANCHOR-REGISTRY-RETIRED-ID-SCAN-READS-ONE-CELL-LAYOUT`, `D-STATE-DRIVER-COUNTS-THE-ALLOWLIST-AS-OPEN-ANCHORS`, `D-HARNESS-SPEEDTEST1-BENCH-MEASURES-ONLY-THE-FIRST-REFERENCE-COMPILER-IT-FINDS`. ⇒ across the whole commit: **15 closed, 1 opened IN TRUTH; `closed 8, opened 1` COUNTED**.

✔**THE MIGRATION WAS REVIEWED BY MEASUREMENT, NOT ACCEPTED**: anchor ids across the two old files at `dac121cc` versus the three new ones — **2070 → 2078 rows, ZERO LOST**, the eight gained being P48's three and the parallel session's five. ⚠ And the citation ratchet was checked for the one edit that is a HAND-SEEDED ceiling rather than a tool output: **zero raised**, three lowered (harness 503→251, production 1462→359, `malloc.json` 9→7), one added for the new archive at 1351. ⓘ The seeded ceiling is **4 BELOW** what a pure relocation would need, conservative in the only direction the ratchet permits.

⚠⚠ **AND MY FIRST REVIEW INSTRUMENT WAS WRONG IN THE INSTRUCTIVE WAY.** It reported two rows flipping CLOSED → OPEN. ✔Both read `⚠️` at HEAD *and* now, byte-identical — the "flip" was my reader ENUMERATING a glyph list that omitted `⚠️` and falling through to a different cell under the old shape. **The project's own rule is to define the COMPLEMENT and never enumerate glyphs**, and a review script that breaks it manufactures findings. ★ The explicit `Status` COLUMN is what makes that class impossible.

**Gate, ✔MEASURED at the folded tree, all four legs through `scripts/run-gate/`.** Windows 1854/1854 (all 20 repo guards, re-run after the handoff edit) · WSL x86_64 1834/1834 · arm64-VPS 1834/1834 · macOS 1834/1834 — the 20-test difference is the root-host-only guard set, and that identity is the cross-leg cross-check. ⚠ **SEQUENCING, STATED RATHER THAN GLOSSED:** the four legs ran against a tree that did NOT carry the registry-policy change, which landed at 09:54–10:05, after the Windows leg.

---

★★★ **P47: TEN ROWS CLOSED IN TRUTH AND FOUR BY THE GATE'S COUNT, ACROSS SEVEN LANES IN TWO WAVES — AND THE CYCLE'S THREE SHARPEST MOMENTS WERE ALL A MEASUREMENT KILLING A PLAUSIBLE DESIGN, TWICE MINE.** ✔MEASURED `check-anchor-balance --base f8ebafb2` ⇒ **closed 4, opened 2, net −2** counted; **10 closed, 2 opened in truth** (§0.5 owes both figures and so does every report). Registry **532 → 530**, production **344 → 340**.

**Wave 1 — `mm` `r1` `bf` `sq`; Wave 2 — `ac` `fl` `fo` `cw`** (the orchestrator replaced each lane as it exited, holding the operator's cap of four).

**The four production closures.** `mm` re-keyed the pre-scan memo to `(PathIdentity, SHA-256 of the file's bytes)` and **deleted** `size`/`mtime` rather than tightening beside a third term — the read now precedes the lookup, because there is nothing to key on until the bytes are in hand. `bf` gave `Lvalue` an ordered member-hop chain so an anonymous-member or array-arrow bit-field base reconstructs through ONE code path, and **kept the refusal** as a structural backstop (`declineMemberLvalue`) instead of deleting it, so a future base shape cannot re-open the silent full-unit store merely by not being enumerated. `fl` closed the FFI library-map hole. `ac` built the dependency artifact cache at the `buildCus` boundary, reusing the runtime object store whole.

**Every one of those four lanes refuted a premise it was handed, and three of the refutations changed what got built.**
- `mm` refuted **its own row's cost claim**. The row said a content digest *"costs one hash of a buffer the code is holding"* — true of `ConfigDocumentMemoStore`, false here, because the memo's HIT path previously did no read at all. ✔MEASURED by interleaved A/B with **two complete driver sets** (the code is in `libdsscp.dll`; an exe-only swap measures nothing): **+1.59% at 41 TUs, +3.37% at 161 TUs ≈ 19 ms/MB**. 🧠INFERRED against sqlite's 109.2 MB of include reads: ~2.1 s against the memo's ✔measured 10.2 s saving — **the memo keeps about four fifths of its win**, and that is the price of an exact key.
- `mm` also refuted the SCOPE premise it inherited (that bytes can only change at a driver-controlled build-phase boundary): `LspServer::run()` is a `while (!exitReceived_)` loop rebuilding a `CompilationUnit` per request with only the MAIN document in memory, so every `#include` is read from a disk an external editor writes to. An invalidation epoch could not have been shown correct. **The content digest was the only exact key.**
- `ac` refuted **the row's key-term list**, and in the dangerous direction: the terms named covered only what the FRONT END parsed, so everything reaching the LINK without passing through it — transitive dependency artifacts, `resolveLibraries`, object inputs, shipped runtime archives — was uncovered. That is **under-invalidation, which ships wrong bytes**, not over-invalidation, which costs a rebuild.
- `fl` refuted the row's CENTRAL premise: the *"unenumerated fallthrough"* had been a stated arm (`ShippedRealizationStatus::NoLibraryForFormat`) since `60eb8ed8` — **two days after the row was written**. It then found the residual the row never named: a `library` key present naming the **empty string**, which `decodeLibraryMap` accepted while its sibling `decodeRealizationMap` already refuses exactly that; one row, two verdicts, no diagnostic. And it **refused the row's recommended fix on measurement** — a load-time "available ⇒ must name an image" rule would fail loud on the six most central C descriptors, because **125 of 567 symbol rows** declare no `availableObjectFormats` at all while naming images only for elf/pe/macho.

**The arm64 arc is the cycle's spine, and it took three lanes.** `r1` declared arm64's SIMD&FP file ONCE — `v0..v31` are the `fpr` class members, `d`/`s`/`h`/`b` are `subOf` views, `argVrs`/`returnVrs` are **deleted** — and witnessed it by execution under qemu: eight simultaneously-live `"w"` operands allocate, which the old empty `vr` free list made impossible. ⚠ **But neither of its rows closed**, because the wider FP class doubled `frameSlotStride` and pushed FP frame accesses past `fstur`/`fldur`'s ±256 unscaled reach — `examples/c/varargs_aapcs64_struct` red, ×9 diagnostics, **failing LOUD**. Per the operator's *"if you depend on something to be built … this needed build enters in priority list"*, the orchestrator **folded the red tree with a rollback snapshot** and dispatched `fo` rather than filing a follow-up.
★ `r1` also caught a silent miscompile it would have introduced: `lowerWideFloatSoftcall` resolves its register class DYNAMICALLY, so it never spells `VR`; with `v0` becoming `fpr` it would have marshalled every `__addtf3`-family call at width 64 instead of 128. **No grep finds that class** — the defect is an ABSENT argument, not a present wrong one.

**`fo` closed the blocker and refuted BOTH attributions, including the orchestrator's.** ✔MEASURED: `D-ASM-AARCH64-FRAME-OFFSET-BEYOND-SCALED-IMM12` (✅ CLOSED, P42-era) does **not** own this — it covers the genuinely UNENCODABLE tail, and these offsets (256–320 ⇒ imm12 32–40) are trivially encodable; there was simply **no form declared** to encode them with. ✔And the orchestrator's `frameSlotStride` suspicion is refuted: the `max(GPR, FPR)` floor is deliberate, deriving downward would shrink x86_64 from 16 to 8 in every function without an FPR spill, and **the fix needed zero change there**. The actual shape: `RegClassOp` gains `LoadScaled`/`StoreScaled`, arm64 declares `fldr_u`/`fstr_u` at 32/64/128, and `selectFrameMemOp` takes a **per-class twin table** instead of a `universalGprOp` parameter — which also removed the integer file's special case, previously resolved by a SECOND mechanism. **The encoder needed no change at all**: its `Imm12Scaled` handler already derived the scale from `instWidth/8`.

**`cw` closed a latent silent-wrongness and refuted the orchestrator's preferred design with an ABI measurement.** The row (filed hours earlier from `fo`'s finding) proposed deriving the save width from the register class's declared full width. ✔MEASURED: that **would have broken arm64** — MSVC emits `movaps` because Win64 preserves xmm6–15 **in full**, while 📄AAPCS64 §6.1.2 preserves only the **low 64 bits** of v8–v15. The same default is wrong on one target and right on the other, so the width is now **DECLARED** (`calleeSavedPreservedBits` on the calling convention, absent ⇒ whole register) for the ABI-facing save and the class's natural width for spills — **two different questions that a single "full width" rule would have conflated.** `ms_x64` needed no new config; arm64 is byte-identical, its 64 now coming from a declaration instead of a code default. ★ It also found **a third instance nobody had named** — `sysv_amd64`'s variadic SSE save half-filled each of eight 16-byte XMM slots on **every** Linux/macOS variadic callee — **an existing test that was PINNING the defect in place** (`…SpillsViaMovsdStore` *required* the 8-byte form; the MOVSD form is now pinned ABSENT), and **two false `$comment`s in shipped config** asserting Win64 needs only the low 64 bits and that spill slots are 8 bytes. `src/link/format/pe.cpp` disagreed and was right.

**Lane `sq` cleared the carriage family that was blocking every lane from every gate host** — four born-closed rows: `leg-tree` refusing a detached driver, carriages unable to identify a cross-namespace lane worktree, carriages looking for `.secrets/` inside a worktree, and the macOS carriage calling ONE mDNS miss a down host. ⚠ **The second of those failed toward CLEAN**: `remote-leg.sh` reported `dirty: 0 path(s)` for such a tree because `git status --porcelain 2>/dev/null | wc -l` counts zero when git FAILED — byte-identical to a genuinely pristine reading, and those two lines are the driver's only record of which tree it carried. `sq` took `D-SQLITE-CLI-BUILT-ON-NO-LEG`'s **Table 1 from 0 to 20 of 20** cells proven by execution (four hosts × five legs × both artefacts) and Table 2 from 0 to 12 of 40, and refuted **both** of P46's named blockers — the "stale `c-subset` regenerator" has no root to fix (the generator writes `"language": "c"` unconditionally and has since P36), and "the Mac's sshd is down" was our own instrument reading one mDNS miss as a verdict, which had cost that row 22 of 40 obligations.

⚠⚠ **THE ORCHESTRATOR'S OWN INSTRUMENTS FAILED THREE TIMES, ALL QUIETLY AND ALL TOWARD *LESS*.** A citation-cell merge split only on `·` while one row used `;`, so a single matched subject discarded the whole chunk and dropped `target_schema.hpp` plus two wiki-links. A containment check built as a sliding window called a legitimate cell-head prepend a content drop — a TRUE answer to the WRONG question, since a prepend must fail such a check by construction. And an `amend_row` assertion caught the orchestrator about to re-status a ✅ CLOSED row to ★★ by prepending an amendment whose first glyph became the status — **that one was caught by the assertion, which is the argument for writing the assertion.** [[feedback-an-instrument-that-answers-an-adjacent-question]] is now a four-time finding.

★ **A COVERAGE ASYMMETRY WAS CONFIRMED AS POLICY, AND ITS DEFECTIVE HALF ANCHORED.** `integrated_tests/c/varargs_aapcs64_struct` read **Passed** while the identical example failed to encode for arm64 under the in-process runner. ✔The asymmetry itself is deliberate and documented (`D-TEST-INTEGRATED-RUNNER-BUILDS-ONLY-THE-HOST-RUNNABLE-SPEC-…`, ✅ closed). **The defect is that `SKIP_RETURN_CODE` is set on the `adjudicate` entry and NOT on the per-example ones**, so an entry whose every arm is structurally skipped exits 0 and ctest prints `Passed` — the runner's own report says *"0 verified"* and *"1 of 1 declared target arms NOT verified"*, and the one-line summary everybody actually reads contradicts it. Amended into `D-TEST-STRICT-ARM-VERDICTS-INERT-ON-WINDOWS` rather than minted as a duplicate.

**Gate, ✔MEASURED at the folded tree, all four legs through `scripts/run-gate/` so a zero exit without the success witness would have been REFUSED.** **Windows 1825/1825 (all 20 repo guards) · WSL x86_64 1805/1805 · arm64-VPS 1805/1805 · macOS 1805/1805** — 1805 = 1825 minus the 20 root-host-only guards, and that identity is the cross-leg cross-check. ✔**Both remote hosts re-verified DIRECTLY after their legs** — `HEAD=f8ebafb2, dirty=0, worktrees=1` on each — rather than trusting the leg's own restore line, because a truncated log has made a completed restore look unwitnessed before. ⚠ Both hosts KEEP their build roots on purpose (`leg-tree` says so explicitly); those roots were built from a tree since discarded, so **rebuild before trusting any binary found in them**.

---

★★★ **P46: ELEVEN ROWS CLOSED IN TRUTH AND THREE BY THE GATE'S COUNT, ONE ROW LEFT OPEN ON PURPOSE, AND THE CYCLE'S TWO SHARPEST FINDINGS WERE A LANE REFUTING MY BRIEF AND MY OWN PROBE REFUTING A LANE THAT WAS RIGHT.** ✔MEASURED `check-anchor-balance --base f865897c` ⇒ **closed 3, opened 1, net −2** counted; **11 closed in truth, 1 opened** (§0.5 owes both figures and so does every report).

### ★★★ THE HEADLINE: TWICE THIS CYCLE AN INSTRUMENT ANSWERED AN ADJACENT QUESTION, AND BOTH TIMES IT FAILED TOWARD *CLEAN*

Not a lane defect either time — **both were mine, and both were caught by USING the instrument rather than by auditing it.**

1. **`burndown-queue.py` banded by the anchor's SPELLING where the ruling defines the bucket by the FILE.** `D-CONF-REFERENCE-DIFFERENTIAL-ORACLE` lives in `-harness.md` and was printed as the **#1 row of the P0 WRONG-OUTPUT band**, with nothing on the line saying harness, because `D-CONF-` is not in `NS_HARNESS` and never was. ⚠ **I read the queue top-down to pick the 4th lane and came ONE STEP from seeding a lane on a harness row**, which the standing ruling forbids outright. ✔Census: **18 harness rows banded above P3, two of them in P0**. The reverse direction — a PRODUCTION row hidden under P3 — measured **0 today, and only by luck of spelling**. Fixed so the FILE decides in BOTH directions; 8 self-test arms now ship and run on every invocation, with a two-mutant red-on-disable transcript through the real CLI.
2. **`lane-worktree.sh remove` REPORTED SUCCESS OVER 4.4 GB STILL ON DISK.** Lane `cm` was folded mid-flight with its `.git` emptied, so `git worktree remove` could not see it and exited non-zero; control fell through to `prune` and printed `removed …` unconditionally. ⚠ **The failure is invisible from the caller** — `git worktree list` agrees the worktree is gone, because the registration WAS pruned. Only `ls .worktrees/` disagrees and nothing in the cycle runs it. Now it removes, **verifies**, and only then speaks. ⚠ Fixing it dragged in a hazard first: **`cmd_add` validated the lane name and `cmd_remove` did not** — harmless while the verb merely declined an unknown path, and not harmless the moment an `rm -rf` stands behind it.

### ★★★ THE SECOND HEADLINE: A LANE WAS RIGHT AND MY PROBE SAID IT WAS WRONG

Lane `dw` reported that ten `tgmath.json` macros now carry three format arms asserting a difference that no longer exists. ✔My probe compared whole arms by serialisation and reported **0 collapsible** — because `when: {format: …}` differs BY CONSTRUCTION in every arm. **The question is not *"are the arms identical"* but *"are they identical APART FROM THE SELECTOR"*.** Re-measured correctly, `dw`'s list reproduced exactly: **ten collapsible (`sqrt sin cos tan asin acos atan exp log pow`), two genuinely format-dependent (`fabs`, `ldexp`, because pe lacks `fabsf`/`ldexpf`)**. ⇒ a confident, true answer to the wrong question would have discarded a correct finding from a lane that had done the work properly. **The collapse landed** (25129 → 20183 chars, 20 arm-lines out), and it is NOT cosmetic: this compiler is config-driven, so a user can add a format, and under the old structure those ten macros went **silently undefined** for it.

### THE LANES

| lane | closed | the part worth carrying |
|---|---|---|
| `pn` | `D-PP-SKIPPED-CONDITIONAL-GROUP-VALIDATED-AS-A-PHASE-7-NUMBER` | **Three defects wearing one symptom**, and the blocker for the WHOLE sqlite corpus. The phase-7 gate was spelled with ONE code name; the phase-3 pp-number tail was **unreachable from the prefix arm**; `isExponentLetter` read **half its config**, so `0x1e+2` BUILT AN ARTIFACT where gcc and clang both refuse. ✔Both sqlite artefacts now `rc=0`. It also **declined** my second row with three measured reasons — the control loop working. |
| `jk` | `D-CONFIG-A-DUPLICATE-JSON-KEY-IS-DROPPED-WITHOUT-A-DIAGNOSTIC` | **Eleven ingestion sites, THREE spellings** — a `json::parse` grep alone misses two real readers. One owner (`detail::parseConfigDocument`), refusal not warning, and a tier-3 scanner that refuses a NEW reader outside the owner. ⚠ Its own guard-stripper had the defect it exists to catch (digit separators `1'000` blanked whole regions). |
| `dw` | `D-CSUBSET-TGMATH-COMPLEX` | The Darwin oracle, unblocked by the operator's *"MacOS is UP"*. **66 `nm -u` probes**, all eleven complex symbols plain on both arches; then a **link-and-run** the oracle does not ask for, and Mach-O arm64 binaries built on Windows and executed on the Mac. Refuted my brief's probe header (`tgmath.h`, not `complex.h`) and my "also check `f`/`l` variants" (no such rows exist). |
| `cm` | `D-CSUBSET-COMPLEX-TO-REAL-IMPLICIT-CONVERSION-REFUSED` (+1 born closed) | The conversion is 8 lines of C++; the `<tgmath.h>` complex dispatch shipped with it as **pure config**. Deliberately wrote its Mach-O test to FAIL once the oracle landed — a tripwire, not a hole — which `dw` then replaced one-for-one. |
| `xm` | `D-TARGET-NO-CROSS-CLASS-MOVE-VERB` (+1 born closed) | `registerClassOps` keyed by the **ordered pair** `{class, to}`, `to` omitted = the diagonal. **Two call sites lost their class comparison entirely.** |
| `dg` | (2 born closed) | Config-document memo FIFO→LRU, capacity 16→128. |
| `dc`, `rt` | — | `rt` created the skipped-group row `pn` then closed, and left `scripts/sqlite-round-trip/`. |

### ⚠ WHAT THIS CYCLE DID NOT DO, NAMED SO IT IS NOT MISTAKEN FOR DONE

- **The PR exit regime is still unspent** (§0.3.1). The README still says *"THREE hosts"*.
- **The one open row** (§0.6) is scoped and owner-shaped, not started.
- **`_Generic`'s unselected arm** (§0.6, last paragraph) is still un-anchored by design.
- **Ten `variants` collapsed; `fabs`/`ldexp` were left** — correctly, and the row says why.

★★★ **P45: FOUR ROWS CLOSED, ONE CORRECTLY REFUSED, AND THE TWO DEFECTS THAT MATTERED MOST WERE IN MY OWN INSTRUMENTS.** ✔MEASURED `check-anchor-balance --base 94971261` ⇒ **closed 3, opened 0, net −3** counted, **6 closed in truth** (three rows born closed are invisible to the gate — see §0.5, which owes both figures and so does every report).

### ★★★ THE HEADLINE: A LANE THAT REFUSED TO CLOSE ITS ROW WAS THE MOST VALUABLE LANE

Lane `cx` was briefed to close `D-CSUBSET-COMPLEX-TO-REAL-IMPLICIT-CONVERSION-REFUSED`. It measured all four references separately — gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0 all accept complex→real in **both** assignment and argument position; MSVC **abstains** (no `_Complex` specifier at all, `error C2146`) — so under the bar the conversion is **REQUIRED**. It then built the fix and **reproduced a silent wrong answer**: with the conversion shipped alone, `#include <tgmath.h>` + `sqrt(z)` on `(0,4i)` compiles and exits **0** where all three references dispatch to `csqrt` and exit **1**. The tgmath binding lives in `tgmath.json` + `c.lang.json`, which the lane did not own.

⇒ It shipped **comment-only** (behaviour byte-identical to its seed, proven by diff against seed-time backups) and stopped. ★ **That is the control loop working, and the report must not read it as a lane underdelivering.** The row stays 🟠 OPEN with an owner named in §0.6.

★★ **AND IT REFUTED THE ROW'S OWN RATIONALE.** `tgmath.json` asserts that `(double)z` "silently drops the imaginary part → a conformance MISCOMPILE". ✔All three references do exactly that drop for `<math.h>`'s `double sqrt(double)` (exit 0). **The drop is CONFORMANT; the miscompile is a tgmath MACRO reaching a real prototype at all.** Three pins had copied the same false rationale — corrected in place, assertions untouched.

### ★★★ TWO HARNESS DEFECTS, BOTH IN INSTRUMENTS I PROMOTED THIS CYCLE, BOTH FOUND BY USING THEM

**1. `lane-fold` walked an untracked directory past `.gitignore`.** ✔MEASURED on the live fold of `cx`: the dry run offered **five** paths as the lane's work and **two were `scripts/{lane-fold,apply-registry-row}/__pycache__/*.cpython-314.pyc`** — gitignored, and listed by `git status` in neither tree. The route in has nothing to do with Python: **both script directories are NEW THIS CYCLE and therefore UNTRACKED**, so `git status --porcelain` reports the bare directory `?? scripts/lane-fold/`, and `expand_paths` answered that with `os.walk`. Any untracked directory a lane owns folds its droppings. **It was both directions** — `seed` and `fold` share `changed_paths`, so the walk had already shipped those files INTO the lane, which is why their md5 could move and present them back as the lane's own work.

⚠⚠ **THE IRONY IS THE FINDING.** The function directly BELOW that walk carries a comment written *this same cycle, by the same author*, proving the `.worktrees` floor must **not** rely on `.gitignore` — safety by accident of a file you do not own. That reasoning is right, and it is about the direction where TRUSTING the ignore file makes you unsafe. **The walk fifteen lines above fails in the opposite direction: it never CONSULTS it.** One instrument, two opposite ignore-file mistakes, one screen, one afternoon.

**2. `check-plan-citations`'s self-test blamed the guard for the tree.** ✔MEASURED twice in one cycle: the suite printed **`self-test FAILED -- this guard is NOT proven able to fail`** while the guard was working perfectly, and running `--write` on `.plans/` — **ZERO change to the script** — flipped it to `OK - 39 arms`. `selftest()` builds its fixture as a `copytree` of the live tree and its design is *perturb → assert red → restore → assert green*, so **every green arm asserts the LIVE TREE is green**. The trigger was therefore **somebody doing the right thing**: a stale ceiling is what the ratchet reports when a lane converts a `path:line` citation to a stable reference — debt going DOWN. Two conditions, one headline, and the headline named the wrong one.

★ Both fixed, both with red-on-disable transcripts (REMOVE-direction, md5 moved AND returned, controls both sides, failing arm read by NAME). The control-arm discipline was **not** weakened and no arm was removed; arm 0 is unchanged in code and stronger in meaning.

### THE OTHER LANES

- **`td`** — `D-CSUBSET-TYPEDEF-HEAD-DECORATION-TYPE-HIJACK` ✅. Refuted its own brief: `lowerTypeDecl`'s strip-aware lowering was **already live** on every C typedef; the comment saying otherwise had rotted.
- **`bf`** — `D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT` ✅, and **the row's prescribed fix would have repaired five ABIs and broken the one that was already right**. It was never one mislaid fold; it was a missing per-ABI axis, now `unnamedBitFieldAlignment` in 22 format documents. End-to-end by EXECUTION: pe64 **27/27 vs cl.exe**, elf64-aarch64 **27/27 IDENTICAL to gcc** under qemu, macho64-arm64 **25/27 vs Apple clang with the DSS-built Mach-O executed on the Mac itself**.
- **`am`** — the x86_64 SSE dialect rows, and it refuted the FP row's trigger sentence: *"live in every emitted aarch64 image"* describes an **EMPTY set**, because `"w"`-bound FP operands are refused one tier below by the cross-class arm.
- **`pp`** — `D-PP-PRAGMA-RECOGNIZED-SEMANTICS` ✅. `#pragma once` is honoured on IDENTITY (the operator's 2026-08-28 ruling) and `#pragma STDC` is recognized, accepted and per-form judged: **6 satisfied silently / 3 diverging, accepted and named**. It found the row's own closing pin unsatisfiable — *"FP_CONTRACT OFF disables FMA contraction"* — because there is no contraction to disable.

### ⚠ WHAT THIS CYCLE DID NOT DO, NAMED SO IT IS NOT MISTAKEN FOR DONE

- The **sqlite legs and the README** (§0.3.1) — untouched, third cycle running.
- The **`_Generic` unselected-arm divergence** is recorded in §0.6 and **has no row**; that is deliberate, and the lane that fixes it files the row born closed.
- **No lanes were seeded for the next set** — the operator halted the loop at this commit.


### ★★★ THE TWO BRIEFS, IN FULL — a scratchpad does not survive a session

⚠ Every premise below was RE-MEASURED by me on the P45 tree rather than relayed from the
ruling; each says which instrument took it. A premise taken by READING source is labelled
as such and is a HYPOTHESIS the lane must confirm by running the compiler.

### Lane brief — R6, the cross-class move slot

Row to close: **`D-TARGET-NO-CROSS-CLASS-MOVE-VERB`** (production).
Operator ruling of 2026-08-28, clause R6. This lane depends on neither R1/R2/R3 nor
R4/R5/R7 — the operator's words were *"start R6 now"*.

---

### What the operator ruled (relay, not my words)

> **R6 — the cross-class move slot.** `{from, to, mnemonic}` or generalize to class×class —
> *the lane picks whichever leaves ONE table rather than two*. Closes
> `D-TARGET-NO-CROSS-CLASS-MOVE-VERB`.

And the two corrections that set it up, which the operator flagged as load-bearing:

> **C1** — the cross-class move verb is **not missing vocabulary**. `movq_gpr_to_xmm` and
> `movq_xmm_to_gpr` already ship byte-pinned on both targets. What is missing is the **SLOT**:
> `TargetRegisterClassOps` is indexed by ONE class, so the cross-product has nowhere to live.
> Settling it by routing through memory is **REFUSED**.
>
> **C2** — `MnemonicSlot::MovqXClass` is **the defect, not the foundation**. It gets DELETED.

---

### Premises I have MEASURED (2026-08-29, on the P45 integrated tree)

Every line below I ran myself. Re-measure anything you intend to rely on — a brief's premise
is a hypothesis, mine included, and three of four lanes refuted one of mine last cycle.

1. **`registerClassOps` is indexed by ONE class, in the config and therefore in the schema.**
   ✔MEASURED by parsing both target documents:
   - `x86_64`: `[{class: fpr, move: movaps, load: movsd_load, store: movsd_store}]`
   - `arm64`: `[{class: fpr, move: fmov, load: fldur, store: fstur}, {class: vr, load: fldur_q, store: fstur_q}]`

   There is no `from`/`to` anywhere in the shape. This is C1 confirmed: the vocabulary exists,
   the SLOT does not.

2. **Both cross-class mnemonics ship, byte-pinned, on BOTH targets.** ✔MEASURED by name lookup
   over the opcode tables: `movq_gpr_to_xmm` and `movq_xmm_to_gpr` are declared on x86_64 (104
   opcodes) and on arm64 (87). x86_64 pins `movq xmm1, rdx` = `66 48 0F 6E CA`; arm64 pins
   `fmov s3, w7` = `0x1E2700E3` and `fmov x7, d3` = `0x9E660067`, verified in their own
   `$comment`s against `aarch64-linux-gnu-as`.

3. **Both targets' own comments already state why one bidirectional opcode cannot work**, in
   almost the operator's words — the variant guard keys only on `(operandKinds, width)`, both
   directions are `reg` at the same width, so a single opcode carrying both *"would silently
   pick a direction"*. Read the `$comment` on `movq_xmm_to_gpr` in either document before you
   design anything: the argument you need is already written there, by whoever declined to
   take the shortcut.

4. **`MnemonicSlot::MovqXClass` has exactly one consumer**: the cross-class arm of
   `lowerBitcast` in `src/lir/lowering/mir_to_lir.cpp`. ✔MEASURED with
   `git grep -n "MovqXClass" -- src/` — the enumerator, its `"movq_xclass"` spelling in the
   slot-name table, and the three lines in `lowerBitcast`. Nothing else names it.

5. **`lowerBitcast`'s same-class arm ALREADY routes through the per-class table**
   (`classOp(dstCls, RegClassOp::Move)`), and its own comment says that arm *"WAS the
   class-dispatch pattern the table generalizes"*. ⇒ **The shape R6 asks for is the shape this
   function is already half-way into.** The cross-class arm is the half that never got the
   table, and `MovqXClass` is the placeholder it got instead.

6. **x86_64 DECLARES `movq_xclass` — and the row carries NO `encoding` key.** ✔MEASURED by
   dumping the opcode row: it has `mnemonic`, `result`, `minOperands`/`maxOperands`,
   `minSuccessors`/`maxSuccessors`, a `$comment`, and nothing else. arm64 does **not** declare
   it at all (✔measured the same way), and says so deliberately in `movq_xmm_to_gpr`'s comment:
   *"arm64 therefore still declares NO `movq_xclass` and a cross-class Bitcast still fails loud
   there; closing that is a different row's work, not this one's."* **You are that row.**

---

### The question that decides what this row IS — answered by READING, not by running

An opcode declared with no `encoding` **fails loud**. ✔MEASURED **by reading source, which is a
weaker instrument than running it, and I am saying so**: `TargetEncodingShape::None` is the
default in `src/core/types/target_schema.hpp` (*"no encoding declared (substrate refuses to
guess)"*), and `src/asm/asm.cpp`'s encode switch answers that case with
`DiagnosticCode::A_NoEncodingDeclared` at `DiagnosticSeverity::Error` and returns false.
`asm_variant_elect.hpp` carries the matching election rejection: *"the target declares the
opcode but gives it no encoding."*

⇒ **There is NO live silent miscompile here.** A cross-class `Bitcast` refuses on **both**
targets today — on x86_64 through `movq_xclass`'s missing encoding, on arm64 through
`lowerBitcast`'s `reportMissingOpcode`. **R6 is an ENABLEMENT, and the row must say that
plainly** rather than borrowing the urgency of a wrong-output defect.

⚠ **Confirm it by EXECUTION anyway, before you design.** Reading source is how this project
generates hypotheses, not how it settles them, and the difference between *refuses* and *emits
nothing* is exactly the kind of thing a switch statement can mislead you about. Build a program
that produces a cross-class MIR `Bitcast`, on x86_64 **and** on arm64, run it through the
shipped CLI, and read the actual rc and diagnostic code. If it does **not** refuse, stop and
tell me — that finding outranks this entire brief.

---

### What to build

**One table, not two.** That is the operator's tie-breaker and it is the whole design brief.
Whether the schema key is a `{from, to, mnemonic}` list or `registerClassOps` generalized to a
class×class matrix is **your call, made on that criterion** — but the same-class move must not
end up in one table and the cross-class move in another, because the identical question
("how do I copy a bit pattern from class A to class B?") would then have two homes and a
future reader would find the wrong one.

Non-negotiable, from the bar and from the ruling:

- **`MnemonicSlot::MovqXClass` is DELETED** — the enumerator, its name-table row, and the
  `lowerBitcast` arm that reads it. Not deprecated, not left unreachable. C2 is explicit.
- **`movq_xclass` comes out of `x86_64.target.json`** with it. An opcode nothing can name is
  dead vocabulary, and the load-time closed-key check should be the thing that tells you if
  you missed a reference.
- **Route-through-memory is REFUSED** as a settlement. The operator named it; do not
  rediscover it.
- **Every new schema surface owes its own load-time refusal AND its own registry row, born
  closed.** A `to` class that names nothing, a `{from,to}` pair declared twice, a pair whose
  mnemonic is not a declared opcode — each is a config error that must be refused where the
  config is judged, with a message naming the JSON path. That is the house rule the aliased-
  views block in `target_schema.cpp` demonstrates; read it as the model.
- **NET OPEN ≤ 0.** Rows you open, you close in this lane.

### What lands as a result, and must be shown by execution

arm64's cross-class Bitcast goes from *fails loud* to *works*, using bytes that already ship.
Pin it: a C program that bit-casts across classes, built and **EXECUTED** on
`elf64-aarch64-linux-exec` under qemu **with `QEMU_LD_PREFIX=/usr/aarch64-linux-gnu`**, and on
x86_64. Never emit-and-eyeball. A corpus example under `examples/c/` with a `release` arm, and
arms in both examples runners (`tests/examples` in-process AND `integrated_tests` CLI) — a
capability change must hit both.

### Red-on-disable

REMOVE-direction mutants only (an ADD-direction mutant stays green when the real config loses
the feature). For a config-driven change **at least one mutant must delete the KEY from the
JSON**, not the C++ — every C++ mutant is blind to the difference between a live key and dead
config. Assert build rc; md5 moved AND returned; verdict through `ctest`, never a bare `.exe`;
read failing NAMES, not counts; a CONTROL arm on both sides; bump mtime on restore or ninja
skips the rebuild and your "clean" control measures the mutant.

### Files

Yours: `src/core/types/target_schema.{hpp,cpp}` and its `_json` sibling,
`src/lir/lowering/mir_to_lir.cpp`, `src/dss-config/targets/{x86_64,arm64}.target.json`, your
own tests and example.

⚠ **`src/asm/asm_template_to_lir.cpp` is NOT yours** — it is contended, and the R1/R2/R3 and
R4/R5/R7 lanes need it. If your work reaches into it, STOP and tell me rather than editing it.

### Deliverable

`scratchpad/p45/row-R6.md` — the row, ONE physical line, exactly 4 content cells, LF only, the
anchor id whole and never wrapped. I apply it; you do not touch either registry file. Nothing
committed, nothing pushed, worktree left in place.


---

### Lane brief — R1 + R2 + R3, arm64 declares its SIMD&FP file ONCE

Rows this lane must close (production), all of them, no follow-ups:

- **`D-TARGET-ARM64-W-CONSTRAINT-BINDS-A-CLASS-NO-C-VALUE-EVER-LIVES-IN`**
- **`D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS`**

and it must leave `D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE`'s load-time
refusal honest — see *"what happens to the tripwire"* below, which is the subtlest part of the
whole lane.

Operator ruling of 2026-08-28, clauses R1/R2/R3. **Sequence: this lane depends on nothing, but
R4+R5+R7 depends on it.** It contends with nobody currently in flight if `asm_template_to_lir.cpp`
stays out (see Files).

---

### The ruling (relay)

> **C3** — the aliasing model **already exists and is called `subOf`**.
> `D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS` *"does not need the arc it gates. It
> needs arm64 to stop declaring one physical file twice."*
>
> **R1** — arm64 declares its SIMD&FP file ONCE: `v0..v31` are the class members (16 bytes, no
> `subOf`); `q`/`d`/`s`/`h`/`b` become `subOf: v_k` views; the class label is `fpr`; `vr` keeps
> **NO** arm64 members; `asmConstraints` rebinds `w → fpr`. **`regClassForCoreType` is NOT
> touched — it is right already; the config was wrong.**
>
> **R2** — width views mean width-elected verbs: fold `fldur_q`/`fstur_q` onto the `guard.width`
> axis and **DELETE** the `_q` mnemonics. The width-128 reg-to-reg FP move is a **DIFFERENT
> AArch64 instruction** (ORR, `mov v.16b`) — not a wider FMOV.
>
> **R3** — the AAPCS64 callee-saved guarantee is **64 bits wide** and the cc lists cannot say so.
> A cc register entry may carry `preservedWidthBits`; **absent = full width**. Demoting v8..v15
> to caller-saved is **REFUSED**.

⚠ The operator's own words on all of this: *"every claim taken from reading source rather than
running the compiler is a HYPOTHESIS, including C3. A lane that REFUTES one says so and stops;
it does not build a workaround around my sentence."*

---

### Premises I have MEASURED (2026-08-29, on the P45 integrated tree)

Re-measure anything you rely on. Mine are reads and JSON parses, not compiler runs, and I say
so per item.

1. **arm64 really does declare one physical file twice.** ✔MEASURED by parsing
   `src/dss-config/targets/arm64.target.json`: 129 register rows — `gpr` 65 (32 of them
   carrying `subOf`, the `w` views), `fpr` **32** (`d0..d31`, `widthBytes` 8, `dwarfNumber`
   64..95, **zero `subOf`**), `vr` **32** (`v0..v31`, `widthBytes` 16, `dwarfNumber` **64..95
   again**, **zero `subOf`**). Same `hwEncoding` in each pair. C3 confirmed at the config.

2. **`subOf`'s shape, from the rows that already use it:**
   `{"name": "w0", "class": "gpr", "widthBytes": 4, "subOf": "x0", "hwEncoding": 0}` — same
   class, narrower width, names its parent by name, and **carries no `dwarfNumber`**. That last
   detail is a decision you must make deliberately for the FP views, not inherit — see (5).

3. **`asmConstraints` binds `w → vr`.** ✔MEASURED:
   `{"letter": "w", "binds": "registerClass", "registerClass": "vr"}`. And `vr`'s own `$comment`
   says its rows are *"DELIBERATELY absent from the AAPCS64 calling-convention register lists …
   so lir_regalloc's buildFreeLists SKIPS them (not allocatable) — no VR virtual register is
   ever minted."* ⇒ **`w` binds a class no C value can ever live in.** That is
   `D-TARGET-ARM64-W-CONSTRAINT-BINDS-A-CLASS-NO-C-VALUE-EVER-LIVES-IN`, measured directly, and
   R1's rebind is its fix.

4. **The existing load-time tripwire, and read this block before you touch anything.**
   `src/core/types/target_schema.cpp` carries a refusal named
   `D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE`, whose comment states:
   - it fires when two rows of **different classes** share a `dwarfNumber` and both become
     allocatable; `subOf` deliberately does **not** cover that shape;
   - `dwarfNumber` is the field and `hwEncoding` is **not** (hwEncoding is a per-file number, so
     a rule reading it would refuse the integer and vector files on both targets);
   - **it was proven by building the naive fix as a mutant** (P28): `d7` carried both a VR value
     and an ordinary `double` at rc=0 with no diagnostic;
   - it names `D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS` in its own message as the
     arc that lifts it.

   ★★ **What happens to it under R1 is the question this lane turns on.** With `vr` empty on
   arm64 and the d/s/h/b/q rows carrying `subOf`, the double-declaration this block judges
   **ceases to exist** — the block should stop having anything to fire on, not be deleted for
   being quiet. **Do not remove it.** It still guards the shape on any future target. But its
   comment's central claim — *"WHAT LIFTS THIS RULE … is a cycle, not a config edit"* — becomes
   **false**, and leaving that sentence standing is exactly the rotted-premise failure this
   project keeps paying for. Rewrite the comment to say what actually lifted it, and pin that
   the block still refuses by **synthesizing the negative** (a fixture that re-declares the two
   classes and asserts the refusal), never by observing that it is silent.

5. **The DWARF reverse map already handles shared numbers, and its rule is load-bearing.**
   ✔MEASURED in `src/link/format/dwarf_cfi_decode.hpp`, `DwarfRegisterReverseMap::build`: two
   rows sharing a `dwarfNumber` with the **same** `hwEncoding` are width views and **the
   NARROWEST wins**; with different `hwEncoding` it **refuses** with a message about writing a
   table a debugger follows into the wrong frame. Its comment says the narrowest choice is *"a
   statement about what a save rule MEANS"* — on AArch64 the callee-save contract covers the low
   64 bits of v8–v15, *"which is exactly `d8`–`d15`, and exactly what GNU `as` translates
   `.cfi_offset d8` into."*
   ⇒ **`d8..d15` must keep their `dwarfNumber`s through this change**, even though the `w` views
   they now resemble carry none. If you strip `dwarfNumber` from the d views, the reverse map
   starts answering `v8`, and `.cfi_offset` decoding silently changes meaning. **Pin this with
   an execution-level CFI test, not a unit assertion about the map.**

6. **`classFullWidthBits` takes the WIDEST register in the class**, and does **not** skip
   `subOf` rows. ✔MEASURED in `src/lir/lir_regalloc.cpp`. Today `fpr`'s widest is 8 bytes ⇒ 64
   bits. **After R1 it becomes 128**, because `v0..v31` join the class.
   ⇒ Its caller asks *"is this copy writing the WHOLE register"* to decide coalescing. A 64-bit
   `fmov d0, d1` would stop comparing equal to 128 and **coalescing would stop happening for
   ordinary FP copies**. The function's own comment says it is conservative *"in the direction
   that matters: a narrow copy can never equal it, so a partial-register write is never mistaken
   for a full one"* — so this fails toward **missed optimization, not wrong code**. That is a
   real regression you must MEASURE (release arm64 disassembly, before and after) and FIX, not
   accept silently. The honest fix is probably to ask the width question of the register's own
   row rather than of its class's maximum; the comment already notes that `lir_peephole`'s R1
   does exactly that post-regalloc.

7. **Spill-slot sizing.** The operator named it and I have **not** measured it. A `double`
   allocated into a class whose members are 16 bytes wide must not silently get a 16-byte spill
   slot (waste) nor an 8-byte one written by a 16-byte store (corruption). Measure it by
   execution — a function with more live `double`s than physical registers, built and run on
   `elf64-aarch64-linux-exec` under qemu — and read the actual stack offsets.

---

### R2, in the terms the ruling set

Folding `fldur_q`/`fstur_q` onto the `guard.width` axis is the same move the target already
makes for `si_to_fp`/`ui_to_fp` and for `movq_gpr_to_xmm` (✔measured: those key their variants
on a width axis and select FMOV `Dd,Xn` vs `Sd,Wn` from it). Follow the shape that is already
there.

⚠ **The width-128 reg-to-reg FP move is NOT a wider FMOV.** The operator named the instruction:
AArch64 spells a 128-bit register-to-register SIMD move as **ORR** (`mov v_d.16b, v_n.16b` is
the assembler alias). If you fold a `move` onto the width axis of the `fpr` class row, the
width-128 arm needs ORR's bytes, not FMOV's. **Byte-pin it against `aarch64-linux-gnu-as` and
say so in the `$comment`, as every other row in that file does.**

ⓘ Today `registerClassOps` declares `vr` with `load`/`store` and **no `move`**, deliberately —
its `$comment` explains the trigger discipline (*"declaring a Q-form FMOV with no consumer would
be dead vocabulary"*). Under R1 there is no `vr` row on arm64 at all, so that discipline
question moves onto the `fpr` row's width axis: **declare the width-128 move only if something
consumes it.** If nothing does, say so in the comment rather than declaring it.

### R3, in the terms the ruling set

`preservedWidthBits` on a cc register entry, **absent = full width**. This is new schema
surface, so by the standing rule it owes:
- its own **load-time refusal** (a value that is not a positive multiple of 8, a value wider
  than the register it annotates, the key on a target that cannot mean it), with a message
  naming the JSON path; and
- its own **registry row, born ✅ CLOSED**.

⚠ **Demoting v8..v15 to caller-saved is REFUSED.** The operator named it. It would be correct
and catastrophic for code quality, and it is the shortcut this clause exists to forbid.

The consumer is whatever decides how many bytes a callee-save spill of a v8..v15 register
writes. AAPCS64 guarantees the **low 64 bits** only; saving 16 would be wrong in the sense that
matters for an unwinder reading a CFI table that says 8. Pin the emitted prologue/epilogue bytes
and the CFI, by execution.

---

### The bar, restated for this lane

- **`regClassForCoreType` IS NOT TOUCHED.** The operator was explicit: *"it is right already;
  the config was wrong."* If you find yourself wanting to edit it, that is the signal to stop
  and report, not to edit it.
- **Route-arounds are refused.** No `if (arch == ...)`, no host-keyed or arch-keyed branch in
  `src/{opt,mir,hir,lir,core,analysis,asm,tokenizer,link,preprocess}`. Vocabulary lives in the
  `.target.json`.
- **Every new schema surface owes a load-time refusal and a born-closed row.** NET OPEN ≤ 0.
- **Four legs at the final tree**, and the arm64 leg is an **EXECUTION** arm under qemu with
  `QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` — never emit-and-eyeball.
- **Red-on-disable, REMOVE-direction only.** At least one mutant must delete a **JSON key**, not
  C++ — a C++ mutant cannot distinguish a live key from dead config. Assert build rc; md5 moved
  AND returned; verdict through `ctest`, never a bare `.exe`; failing NAMES not counts; control
  arms both sides; bump mtime on restore.
- **A corpus example** under `examples/c/` with a `release` arm, wired into **both** examples
  runners.

### Files

Yours: `src/dss-config/targets/arm64.target.json`, `src/core/types/target_schema.{hpp,cpp}` and
its `_json` sibling, `src/lir/lir_regalloc.cpp`, `src/lir/lir_callconv.{hpp,cpp}`,
`src/link/format/dwarf_cfi*.hpp`, your own tests and example.

⚠ **`src/asm/asm_template_to_lir.cpp` is NOT yours** — R4+R5+R7 needs it and so may R6. If your
work reaches into it, STOP and tell me.
⚠ **`src/lir/lowering/mir_to_lir.cpp` may be held by the R6 lane.** Check with me before editing
it.

### Deliverable

`scratchpad/p45/row-R1R2R3.md` — one file per row you close, each ONE physical line, exactly 4
content cells, LF only, anchor ids whole and never wrapped. I apply them; you touch neither
registry file. Nothing committed, nothing pushed, worktree left in place.


---

★★★ **P44: TWENTY-ONE ANCHORS CLOSED ACROSS NINE LANES, THE GATE CAN SEE NINETEEN OF THEM, AND NOTHING THIS CYCLE OPENED IS STILL OPEN.** ✔MEASURED `check-anchor-balance --base b1684e7f` ⇒ **closed 19, opened 0, net −19**; registry **556 → 537** (**production 349 OPEN / harness 188**, and the sum is the cross-check). Both anchor gates run, plus `stale-refusal-citations`, `wrapped-anchor-ids`, `plan-citations`, `path-identity` and `retyped-closed-sets`, all rc=0. ⚠⚠ **THE GATE CANNOT SEE TWO OF THE TWENTY-ONE, AND THE REPORT OWES BOTH FIGURES**: it counts ROWS BY NAME across base and tip, so a row MINTED and closed inside the cycle (lane `a` → lane `h`) and a row written BORN CLOSED (lane `i`) are invisible — ✔measured from both bases, `b1684e7f` reads closed 19 and `6c6d6077` reads closed 1, and 19+1≠21. That blindness is RIGHT for *"did this cycle leave more open than it found?"* and WRONG for *"what did it fix?"*; **never soften the instrument, report both numbers.**

★★★ **P44's HEADLINE: TWO LANES SHIPPED TWO CORRECT HALVES, BOTH CORRECTLY DECLINED TO CLOSE THE ROW, AND THE COMPOSED BINARY WAS STILL 0/30.** The cause was one tier below either lane: bare `fs::absolute` re-roots a path that already names an AUTHORITY (`//host/share`) onto the local drive. ★ **A lane that ships a correct half and declines to close is the process working**; the orchestrator owes the composition measurement, and nobody's green was wrong.

★★ **AND THE SECOND-BIGGEST FINDING WAS A BRIEF OF MINE BEING REFUTED.** Lane `h` was told to emit `.init_array` / `__mod_init_func` / `.CRT$XCU`; it measured `readelf -d` on a DSS artifact instead — **no `DT_INIT_ARRAY`, no `DT_INIT`** — and observed that ld.so walks the TAG, not the section, while PE never links the UCRT startup. **DSS links no crt: the synthesized `_start` IS the runtime**, so such a section would be bytes nothing reads. Four lanes measured one of my premises this cycle and **three refuted it**. A brief's premise is a HYPOTHESIS carrying its author's confidence and not their measurement.

★★★ **P43: THE TWO RED CI LEGS WERE TWO DIFFERENT WAYS OF NOT BEING ABLE TO SEE A DEFECT LOCALLY, AND BOTH ARE NOW STATIC OR DETERMINISTIC.** ✔MEASURED `check-anchor-balance --base 73f74972`: **closed 0, opened 0, net +0**, registry+plans **879 → 879** — the correct reading for a cycle whose five rows are ALL **BORN CLOSED**, since a row that did not exist at base cannot be counted as newly closed. **Three production fixes and two harness fixes.** ⚠ **Only TWO of the five were the reported CI failures.** Repairing the Windows BUILD break ran the MSVC suite for the first time since P34 and uncovered two production defects underneath it — one a symlink escape in the staging-temp claim; a third was faced while taking the gate itself and fixed in the lane that hit it, per *"harness we fix as we need when we face the problem (NEVER LATER)"*. ★ **A build break does not just stop a build: it hides every test behind it, and the longer it stands the more it hides.**

★★★ **THE WINDOWS LEG: `DSS_EXPORT` ON A MEMBER OF AN ALREADY-`DSS_EXPORT` CLASS IS MSVC ERROR C2487, AND MSVC IS THE ONE COMPILER NO LOCAL LEG RUNS.** ✔MEASURED: `windows-msvc-release` on CI run 33156833090 failed to BUILD `src/link/entry_trampoline.cpp` and `src/link/image_request.cpp`, naming `ObjectFormatData::addSectionRow`. GCC and Clang accept the shape silently, and the four-leg gate is Windows/**MinGW-GCC**, WSL GCC, qemu arm64 GCC and macOS Clang — so the declaration landed in **P34 (`5085664a`)** and sat green through **eight cycles and a 1708/1708 local Windows gate**. ⇒ **The instance is one deleted macro; the CYCLE'S deliverable is that the rule is now a static check every leg can run** — `export_macro_placement_guard`, a BAN (live population zero), whose boundary was MEASURED against `cl` one arm at a time: member function / static member function / static member data are C2487, while a nested class, a nested struct and a `friend` declaration are ACCEPTED. ⚠ **A guard keyed on "any `DSS_EXPORT` inside an exported class" would have refused NINE live sites the compiler is happy with** and been switched off the same day. ✔The repair is verified BY EXECUTION under Visual Studio 18 with CI's own generator and build type, not by reading: `MSVC_LINK_TARGET_OK`, zero `error C`, both objects present. [[D-BUILD-EXPORT-MACRO-ON-AN-EXPORTED-CLASS-MEMBER-BREAKS-MSVC]]

★★★ **THE LINUX GCC LEG: A PROPERTY THAT IS A COUNT WAS PINNED WITH A RATIO OF TWO WALL-CLOCK SAMPLES.** `PpIncludeNoRework` read **x1.0483 against a 0.85 bound on `linux-gcc-release` and PASSED on `linux-arm64-gcc-release` in the SAME run at the SAME commit** — two half-second arms on a shared two-vCPU runner, where scheduling noise is the same order as the effect. ⚠ **The test file's own header called a ratio *"insensitive to host speed and to load"*; a ratio's MEANING is host-insensitive and its MEASUREMENT is not.** ⇒ **The fix is to ask the property what kind of quantity it is**, and the split is the reusable half: the include defect is *the same file read N times instead of once* — a COUNT — so `PreScanMemoCounters` now publishes the pre-scan's `builds`/`hits` and the case asserts **exact integers** (12 units sharing one header ⇒ 1 build + 11 hits; 12 distinct byte-identical headers ⇒ 12 builds + 0 hits), false by a factor of `kUnits` on any host at any load. The sibling `#if` defect is a **memcpy whose only trace is time**, so a counter for it would have **no writer** in the fixed code and could never fire — that case keeps the clock and fixes the **ESTIMATOR** instead (`min` over 3 interleaved rounds; noise is additive and one-sided), bound untouched. [[D-TEST-PP-NO-REWORK-PINS-A-COUNT-WITH-A-WALL-CLOCK-RATIO]]

⚠ **THE SAME GAP EXPLAINS BOTH LEGS, AND IT IS THE ONE THING TO CARRY FORWARD: A RULE ENFORCED ONLY BY A REMOTE JOB IS A RULE THAT GETS BROKEN.** One was a compiler nobody runs locally; the other was a runner nobody can reproduce locally. Neither was fixed by making CI more tolerant — one became a static check, the other became an integer.

★★★ **P42: 68 DEFECTS FIXED, AND THE GATE CAN ONLY SEE 28 OF THEM — PLUS AN AUDIT THAT CORRECTED 84 ROWS THAT WERE ALREADY FIXED AND STILL READ OPEN.** ✔MEASURED `check-anchor-balance --base 301e2a63`: **closed 112, opened 3, net −109**, registry **665 → 556**. ⚠ **Those two sentences are different numbers and both are needed.** Diffing anchor ids AND their status cells against `301e2a63`: **28** pre-existing rows flipped OPEN→CLOSED, **40** rows were **BORN CLOSED** (found and fixed inside the cycle, filed already ✅ because the rule is *close, do not file*), and **84** were re-statused by the end-of-cycle audit as **already fixed in behaviour while their cell still read OPEN**. ⇒ **P42 fixed 68 defects; the audit corrected 84 records; the OPEN count fell 109.** ★ **The better a cycle obeys "close, do not file", the LESS of it a row-counting gate can see** — a born-closed row did not exist at base, so it cannot be counted as newly closed. Report both or neither. **62 of the 68 are PRODUCTION; all 6 harness closures are born-closed** — not one was scheduled, every one was a blocker fixed in the lane that hit it, which is exactly what *"harness we fix as we need when we face the problem (NEVER LATER)"* prescribes.

★★★ **THE FOUR-LEG UNIT GATE IS GREEN AT THE FINAL TREE — Win 1774/1774 (all 19 repo guards) · WSL 1755/1755 · arm64 VPS 1755/1755 · macOS 1755/1755** (1755 = 1774 − the 19 root-host-only guards). Every leg verified on **three** independent channels: the recorded rc, `run-gate`'s success witness, and a count of `***Failed|***Exception|***Timeout` lines (0 on all four). ⚠⚠ **THE SINGLE MOST CONSEQUENTIAL NEAR-MISS OF THE CYCLE: both example runners discover examples through a CONFIGURE-TIME GLOB.** `build/dbg` knew **1330** entries; after `cmake` reconfigure, **1384**. Ten of this cycle's ~27 new examples were then asserted **BY NAME in both runners** before any result was trusted. Without that step all four numbers would have been green about the *old corpus*. **Reconfigure and assert by name is now mandatory on every leg.**

★★★ **sqlite `veryquick`: ZERO DSS-ATTRIBUTABLE FAILURES on Windows (5 legs built, 3 ran ~394k tests each) and macOS (both Darwin legs, smoke 14/14).** Every residual attributed: known earned confounds, plus two that were run to ground. **`scanstatus-5.1.2` is UPSTREAM, not DSS** — ✔the gcc 13.3.0 reference testfixture in `stage/` reproduces it IDENTICALLY (`nEst 9.0` where the test expects `8.0`), same host, same staged source, same defines; it fails on all five target legs across three host platforms under both compilers. **`vtabH-3.1` on pe64 is this machine's hidden pt-BR `C:\Arquivos de Programas` junction** (`Hidden, System, ReparsePoint`) — Tcl's `glob` skips hidden entries, sqlite's `fsdir` does not; it passes on both ELF legs and under the Linux gcc reference. ⚠ The pe64 same-platform oracle **cannot** be built at all: mingw fails on upstream's `ext/misc/fileio.c` where DSS compiles it ([[D-HARNESS-PE64-HAS-NO-SAME-PLATFORM-ORACLE]]).

★★ **COMPILE TIME FELL HARD ON WINDOWS: `speedtest1` full-source build −j1 64.54 → 33.80 s, −j4 36.16 → 13.19 s (−64%)**, same upstream `6f1110c`, same host, with the reference arms moving only within ordinary variation (gcc −j4 7.38 → 7.77 s). The gap at −j4 went **4.9× gcc → 1.70×** and **8.0× MSVC → 2.90×**. ⚠ **Only the Windows table was re-measured**; Linux and macOS were not, and README now says so rather than letting one fresh row imply a fresh page.

⚠⚠ **THREE ROWS ARE OPEN AND EACH IS A NAMED EXCEPTION CARRYING ITS CLOSING PREDICATE — none is a follow-up.** `D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE` (needs the ~160-row aarch64 FP sub-view roster + 5 dialect modifier letters; ⚠ flipping `fpr`/`vr` to `registerNatural` without them is MEASURED to break working scalar-FP templates) · `D-CSUBSET-COMPLEX-TO-REAL-IMPLICIT-CONVERSION-REFUSED` (**built, measured and WITHDRAWN**: with the arm in, `sqrt(z)` on `(0,4i)` returned 0 where gcc returns 1, because `tgmath.json` borrows its `_Complex` loudness from that very defect — needs the tgmath complex dispatch first) · `D-CSUBSET-REAL-AND-IMAG-PART-OPERATORS-UNSUPPORTED` (ground read; a half-built `HirOpKind` judged worse than a clean handover).

★★★ **THE CYCLE'S MOST REPEATED LESSON, FOUR INDEPENDENT INSTANCES: REBUILD BEFORE YOU TRUST ANY RED.** The config schema **refuses an unknown key by design** (a silently-dropped key would switch a feature off with no diagnostic), so the moment a lane adds a `.target.json` or `predefinedMacros` facet, **every binary built earlier fails to load the shipped config — and the symptoms look unrelated to each other**: a test that swallows the failure CRASHES, the driver prints `C_MalformedJson`/`C_InvalidPreprocess`, an old build root says `loadShipped(c) failed`. ✔It produced a false "MIR regression" verdict, a lane's false "another lane is breaking the tree" report, and a failed sqlite leg — all in one evening. ⇒ **Before any gate, rebuild EVERY build root the gate will use, not just the one you are looking at** (`build/dbg` for units; **`build/rel` for sqlite and the benchmark**), and **before attributing N symptoms to N causes, ask whether something shared and recent is refusing something the old artifacts still carry.**

★★★ **P41: FOUR LANES, EIGHT ROWS, ZERO OPENED — AND THE NUMBER THAT MATTERS IS NOT THE NET.** ✔MEASURED `check-anchor-balance --base 0c151f2a`: **closed 4, opened 0, net −4**, registry+plans 992 → 988. ⚠ **READ THAT WITH ITS CAVEAT OR IT WILL MISLEAD YOU: SEVEN OF THE EIGHT ROWS ARE *BORN CLOSED*, WHICH IS NET ZERO FOR A GATE THAT COUNTS ROWS**, and three of the four counted closures (`D-ML7-2.3`, `D-FULLC-STDBIT-ARM64-CNT-POPCOUNT`, `D-FULLC-STDBIT-BIG-ENDIAN-NATIVE`) are P40's work riding in the same uncommitted tree. P41's own counted closure is exactly one: `D-LSP-DIAGNOSTIC-RENDERED-AGAINST-THE-OPEN-DOCUMENT-IGNORING-ITS-BUFFER`. The cycle nevertheless shipped **six production fixes and one harness fix that had never had rows at all** — the row count cannot see work that was never filed, which is the intended consequence of *close, do not file* and is the reason this line reports closed/opened/net **and** the born-closed split rather than one total. ✔MEASURED per bucket with the gate's OWN scanner (`scan_document`, not a re-typed predicate): **production 473 OPEN / 1386 rows · harness 192 OPEN / 598 rows · total 665**, which sums to the gate's own registry figure. ⚠ A hand-rolled count of the same files said 673 — **8 rows adrift, all in the harness bucket** — which is precisely why the skill mandates reusing the gate's scanner and cross-checking the sum.

★★★ **LANE K — THE LSP ANSWERED EVERY POSITION QUERY IN SYNTHESIZED PREPROCESSOR COORDINATES, AND THE DEFECT WAS NEVER ARITHMETIC.** `src/lsp/` named a BUFFER nowhere while THREE coordinate spaces were live (document / synth / header-origin), so every handler reached for `tree.source()`, which is always nearest to hand. It stayed invisible because **the synth buffer is constructed WITH THE MAIN SOURCE'S NAME**, so a wrong answer names a plausible file at a shifted line, and the tests passed only because their fixture made all three spaces coincide. ⚠ **PRE-EXISTING, NOT CAUSED BY THE PROLOGUE**: it was already wrong for any C file with a leading `#include` or any build using `--define`. Fixed as a TYPE (`dss::lsp::DocumentCoordinates`) with `tree.source()` made UNREACHABLE from handlers by a new `lsp_coordinate_ownership_guard` — a rule only a reader enforces is the hole this class keeps returning through. ★★ **THREE BRIEF CLAIMS REFUTED BY MEASUREMENT, INCLUDING ONE OF MY OWN:** an `#if 0` region is **not** a zero-image case (the synth buffer is TEXT concatenation, so a dead branch's TOKENS are elided while its text keeps a perfectly good image and merely owns no nodes); `publishDiagnostics_` was a **different** bug (both streams were already remapped to origin, so it was correct coordinates rendered against the wrong buffer); and a UNC path is not representable through `fs::path` on the gating toolchain. Four REMOVE-direction mutants, each md5-distinct, each reddening a DIFFERENT named test, one reddening the guard itself.

★★★ **LANE N — ONE CAUSE, FOUR SYMPTOMS, TWO TIERS, AND THE BRIEF'S SCOPING PREMISE WAS WRONG.** C 6.7.6p1 permits redundant parentheses, so `int (foo)(int x) { … }` defines `foo` — the glibc/musl idiom for a name that is also a function-like macro. A SINGLE-LEVEL look at the name's own direct declarator cannot step out through a parenthesis, which produced the `S0018` refusal, a prototype that silently bound as an OBJECT, parameters scoped as a function POINTER's, and an arity mismatch in CST→HIR. ✔MEASURED gcc 13.3.0 and clang 18.1.3 SEPARATELY, every battery carrying a deliberately-broken NEGATIVE control: both compile AND RUN all twelve accepting shapes, and everything both refuse stays refused. ★★ **IT ALSO CLOSED THREE SILENT ACCEPTANCES THE OLD BLINDNESS WAS HIDING**: `int f[3](int x){…}` (the predecessor answered "function" if ANY suffix was one), `int f(int)(int);` (refused only by ACCIDENT, in the typedef spelling, under a message naming a construct the source does not contain), and `struct S { int f(int); };` — **pre-existing, shared with the plain spelling**, closed rather than widened. ⚠⚠ **THE AT-MOST-ONE-OWNER RULE COST A CLOSURE HERE AND THAT IS WORTH SEEING**: the fourth symptom lives in `src/hir/lowering/cst_to_hir.cpp`, lane O's file, so lane N proved the 3-line hunk in an isolated worktree and refused to write it. Lane O landed it and re-measured 1361/1361. The rule is right — it is what stops two lanes corrupting each other's file — but a defect can straddle the partition, and the lane that finds it may not be able to finish it.

★★★ **LANE O — THE TRUTHINESS CHOKEPOINT MISSED THREE WHOLE TYPE KINDS, AND THE FIRST RED-ON-DISABLE RUN WAS THE INSTRUMENT.** C's scalar type (6.2.5p21) is arithmetic ∪ pointer, arithmetic INCLUDES enumerated types (6.2.5p17), a function designator joins by 6.3.2.1p4 and `nullptr_t` by 6.3.2.4p2 — `coerceCondition` covered the arith-core kinds plus Ptr plus the c91 Array decay, so Enum, FnSig and NullptrT fell through UNCHANGED and hit the verifier ON LEGAL INPUT. ✔MEASURED with a 297-probe battery (33 scalar shapes × 9 controlling-expression contexts): **222/297 → 261/297**; clang accepts 297/297 and gcc's only refusals are the 18 `_BitInt` probes, refused because gcc 13 has no `_BitInt` TYPE — not because it rejects the construct. ⚠ **THE INVARIANT WAS RIGHT AND IS UNTOUCHED**: relaxing CondBr-expects-Bool would have traded a loud refusal for a silent miscompile. ⭐ **THE TEMPTING WRONG FIX IS PINNED AGAINST**: MIR lowers Cast-to-Bool as `Trunc`, keeping only the LOW BIT, so every enumerator used as a truth value in the corpus example is EVEN and nonzero (EVEN=4, W_HIGH=256) — a `Cast(Enum → Bool)` would call `if (EVEN)` false. ⚠⚠ **ITS FIRST ROD RUN CAME BACK ALL-GREEN AND WAS MEASURING NOTHING**: it drove the build from Python with a Windows cwd, CMake refused the resulting `/mnt/c/...` cache path, **no mutant ever compiled**, and ctest re-ran unchanged binaries. **The md5 column is what exposed it** — the third assertion, that the mutant was READ, is not optional.

★★ **LANE P — A GUARD REFUSED A STANDARD-MANDATED SHAPE ON A PREMISE THAT IS FALSE FOR HALF ITS DOMAIN.** `checkMacroSymbolShadowing` refused *(formats overlap) ∧ (no body references the name)* and told the author nothing could ever reach the symbol. That holds for an OBJECT-LIKE macro only: a function-like macro expands only before `(` (C 6.10.3p10), so C 7.1.4p2 keeps the function addressable in as many words. ✔MEASURED by `nm -u` **on the emitted object**, gcc and clang separately, agreeing on all ten rows, with both controls discriminating every run. ★ **AND THE FORM IS READ PER *BODY*, NOT PER ENTRY** — `decodeShippedMacros` refuses an entry carrying both a body key and `variants`, so a variants macro's `params` can only live on the variant and one arm may be function-like while its sibling is not. ✔Corpus re-measured: the narrowing changed **no in-tree verdict**; it only stopped refusing a shape the corpus had not yet needed. It also clears the FIRST blocker on `D-FULLC-STDBIT-ADDRESSABLE-FN`, whose next blocker is now named and measured rather than guessed.

★★★ **THE ORCHESTRATOR SHIPPED A MECHANISM THAT COULD NOT BE USED, AND A LANE FOUND IT.** The 2026-08-26 ruling put lane worktrees at `.worktrees/<name>`; `tests/CMakeLists.txt` placed the TF-C118 out-of-tree arm's working directory by GUESSING from the build layout — in-tree ⇒ `${CMAKE_SOURCE_DIR}/..` — which for a worktree is `<repo>/.worktrees`, still inside `<repo>`. ★ **STEPPING OUT OF `CMAKE_SOURCE_DIR` IS NOT STEPPING OUT OF THE REPOSITORY**: a worktree holds its own `src/dss-config`, so the NEAREST holder is the worktree and only the OUTERMOST is the repo. A guess-then-verify shape can only REPORT that mismatch, never repair it, so **every sanctioned lane worktree was unconfigurable**. ✔MEASURED on a real one: `.worktrees/n` held a `CMakeCache.txt` with no `build.ninja` and no `CTestTestfile.cmake`. Fixed by DERIVING the location from the same predicate the check asserts. ⚠ It must stay OUTSIDE the repository — "no ancestor holds `src/dss-config`" IS the condition under test, so moving it inside makes the arm silently vacuous.

⚠⚠ **TWO REPO GUARDS CAUGHT REAL DEFECTS DURING THE FOLD, AND ONE WAS THE FOLD'S OWN.** `anchor_registry_guard` found the new harness row appended INSIDE the Allowlist table, whose header declares TWO columns — its last two cells would have been **silently dropped by the renderer, invisible in the raw text and invisible in the diff**. The cause was an "end of table" rule that found the last table line in the FILE; a table ends at the next section heading, and the registry's own header warns about exactly this. `stale_refusal_citations_guard` found lane O calling `D-CSUBSET-NULLPTR` a *"still-open gap"* when it closed 2026-07-09 — the SENTENCE was true and measured, only the CITATION was stale; the pointer was dropped rather than a row minted, because the defect goes into the next cycle.

★★★ **THE macOS LEG EARNED ITS PLACE: IT CAUGHT A DEFECT THE OTHER THREE LEGS CANNOT SEE, AND THE DEFECT WAS AN ASSERTION THAT PINNED ONE PLATFORM'S `std::filesystem` BEHAVIOUR.** `WorkspaceProject.FileUriRoundTrip` hardcoded `file:///server/share/x.c` for a path built from `//server/share/x.c` — 1688/1689 on macOS, green on Windows and WSL. ✔MEASURED, and BOTH platforms are right: MinGW's `fs::path` reports an EMPTY `root_name()` and DISCARDS the UNC authority at construction, so the URI has one leading slash; **libc++ on Darwin PRESERVES the `//`, because POSIX makes a leading `//` implementation-defined and `//server/share` there is a LOCAL path, not a network authority**, so `fileUriFromPath` correctly emits `file:////server/share/x.c` — empty authority plus that path. ★ **Treating Darwin's `server` as a URI authority would have been the ACTUAL bug.** The production code was correct on both legs and the round-trip assertion immediately after the failing one PASSED on macOS: **this was a test defect, not a compiler one.** ⚠⚠ **THE LANE'S COMMENT WAS CAREFUL AND STILL WRONG, WHICH IS THE PART WORTH CARRYING:** it said, accurately, that it pinned *"what THIS leg actually does rather than a portability claim it cannot keep"* — but **"this leg" is FOUR legs**, and an assertion measured on one of them is a portability claim whether or not it is phrased as one. ⇒ Both known-correct renderings are now named EXPLICITLY with why each is correct, so a THIRD rendering still reds; the expectation was deliberately NOT derived from `p`, because deriving it would make the test re-implement the function it tests and assert nothing. RED-ON-DISABLE (product mutated, not the test — authority detection forced ON so it emits `file://server/share/x.c`): clean `d4e13690` 100% → mutant `7240b781` **RED, 50%** → restored `da461ca1` 100%, every md5 moved so every arm was compiled and READ.

⚠⚠ **AND MY OWN FIRST RUN OF THAT RED-ON-DISABLE WAS VACUOUS, FOR THE THIRD DISTINCT REASON THIS CYCLE.** The mutation tripped `-Werror` while the build output was silenced, so the build failed invisibly, ctest re-ran the UNCHANGED binary, and the arm reported **green with a byte-identical md5** — the same failure lane O reported and that is written two paragraphs above in this very file. Then the heredoc ate the backslash out of the mutation's `u8` character literal (**write the script to a FILE**), and then `set -o pipefail` plus a DIAGNOSTIC `grep` that legitimately matched nothing returned 1 and killed the run after the clean arm. ⇒ **Three rules, and none of them is new: ASSERT the build succeeded, ASSERT the md5 moved, and never let an informational pipeline decide a verdict.** A mutant that was never compiled is indistinguishable from a passing test, and reading the transcript will not tell you which one you have.

★★★ **WHAT P41 LEAVES ON THE TABLE — LANE O MEASURED FIVE MORE, ALL ACCEPTED BY BOTH gcc AND clang, NONE IN ITS FILE SET, AND ONE IS NOT A DIAGNOSTIC AT ALL.** ⚠ **A `nullptr_t` PARAMETER HARD-CRASHES THE COMPILER**: `static int f(typeof(nullptr) p)` → `dss::substrate fatal: TypeInterner::get: TypeId out of range`, an ABORT where the bar requires fail-loud. Then: an enum-typed file-scope global → `K_NoMatchingObjectFormat` (TypeKind 24 is Enum; `static int g = 3;` works) in `lowerMirGlobalsToDataItems`; a `nullptr_t`-typed OBJECT → `I_StoreValueTypeMismatch` and a `nullptr_t` RETURN → `S0004`; `_Complex` in a condition → `I_TerminatorTypeMismatch`, the fourth member of lane O's own class and **genuinely blocked** on complex `==`/`!=` which `hir_to_mir` already defers under `D-CSUBSET-COMPLEX`; and `_Bool b = <array | _BitInt | function designator | _Complex>` → `S0003`, `isAssignable`'s `scalarConvertsToBool` list. ⇒ **These are the next cycle's first items, hard crash first.** They are recorded here rather than as rows on purpose: under *close, do not file* a row minted only to be closed next cycle is paperwork that raises the open count for no reader's benefit.

ⓘ **FOUR-LEG GATE, EVERY LEG RUN AT THE *FINAL* TREE** — which is the part worth stating, because the macOS leg first reddened at **1688/1689** and was re-run IN FULL after the fix rather than re-verified on the one suite that failed: Windows **1708/1708** (incl. all **19** `repo-guard` entries, `145.51 sec*proc`), WSL x86_64 **1689/1689** in `279.29 s`, qemu arm64 **18/18** under `-DDSS_STRICT_ARM_VERDICTS=ON`, macOS arm64 **1689/1689** in `690.58 s`. ⚠ **`1689 = 1708 − 19`, and the 19 are the `repo-guard` label the remote legs exclude ON PURPOSE** (`-LE repo-guard`): those guards interrogate the *tree*, the root host ran all 19 green, and a remote host answering git questions about a tree it only partly holds is exactly how an innocent guard reddens — ✔MEASURED P34, where `plan_citations_guard` counted 4908 citations against a live 2853 and cost hours on a subject that was never wrong. ⚠⚠ **AND THE arm64 EXECUTION PROOF IS NOT THE arm-STRICT LEG.** That leg is SCOPED to the eight examples this cycle touched and emits **no arm ledger at all**; a claim that arm64 code was *spawned and ran* rests on the **WSL leg's** emulator witness — `[arm-ledger] c/builtin_bitcount: 6 verified (6 ran, 0 expect-error) … 0 emulator-missing … 0 poisoned` — which is a separate assertion from `18/18`, and I stated the two as one before checking. ⓘ Leg cost, ✔MEASURED and recorded because the previous handoff's "~90 minutes" was folklore: macOS runs the full 1689 in **11.5 minutes** wall clock at `-j 6` (3459 CPU-seconds ÷ 6 ≈ 9.6 min, plus a ccache-warm build). The 90 came from a COLD leg. The long pole is three shuffle-seeded suites — `test_preprocessor_shuffled` **158 s**, `test_fc3_width_semantics_shuffled` **81 s**, `test_semantic_analyzer_c_shuffled` **44 s** — each serial WITHIN itself, so `-j` cannot split them and one alone is a 2.6-minute critical path.

★★★ **P39 (`e051ac88`): FIVE ROWS CLOSED BY FOUR LANES, NONE OPENED, NET −5 — AND THE CYCLE'S MOST USEFUL FINDING IS THAT A LANE'S HANDOVER WOULD HAVE SWITCHED OFF THE ELEVEN TESTS THAT PROVE IT.** Lane E closed `D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC` (the inliner refuses what it cannot prove frame-safe; the row's own premise refuted — the hazard was never going to arrive as a registry-minted `IntrinsicCall`). Lane F closed `D-OPT-MEMSSA-WALK-PAST-PRECISION` with a points-to/escape substrate (`MirPointerEscape`, default arm `Escapes`), `mirMayAlias` rule 3b and a `(def,location)`-keyed memo — the 2026-07-07 deferral rested on ONE premise, that the skip almost never fires at current precision, and that premise is now false BY CONSTRUCTION. Lane G closed `D-OPT-JCC-FALLTHROUGH` (peephole R2 + verifier rule 1b, both consulting ONE predicate). Lane H closed `D-OPT11-LAZY-IMPORT-EDGE` (subset `FunctionCloner`, digest-bound `.dss.mir` body codec, `--lto=<full|thin>`; thin moves 54% of the serial residue into parallel, full stays byte-identical). ⚠⚠ **THE HANDOVER LESSON, WHICH GENERALISES TO EVERY CROSS-TIER HANDOVER:** lane G could not commit the `.target.json` half (one lane holds `src/dss-config/**`), so its tests NECESSARILY pinned the PRE-application state. ✔MEASURED against a config COPY reached through `DSS_CONFIG_ROOT` before touching the tree: applying the handover as handed turns **ELEVEN** tests red — `test_lir_peephole` 20/20 → 6 FAILED, `test_lir_text` 58/58 → 5 FAILED — and not on stale assertions but on a **LOAD REFUSAL**, because the fixture synthesized the declaring target by APPENDING the variant and the shipped document now already had one. ⇒ ★★ **A FIXTURE MUST SYNTHESIZE THE NEGATIVE, NEVER THE POSITIVE:** `mutateShippedTargetSchemaDoc` throws on a byte-identical mutation, so a REMOVAL that finds nothing to remove is LOUD, while an ADDITION always "succeeds" and goes on reporting green over a config tree that has LOST the feature. `tests/lir/fallthrough_form_schema.hpp` is the corrected pattern. ★ And `BothShippedTargetsDeclareTheFallthroughEncodingForm` now reads the SHIPPED file, making it the only test that notices the vocabulary being edited away — the exact hole that let that row be marked ✅ while R2 was a no-op by construction. ⚠ Applied as a TEXT edit, not with the lane's own applier, which round-trips both documents through `json.dump(indent=2)`; the applied rows were then MEASURED semantically identical to the benchmarked ones, so the size figures CARRY rather than being re-quoted: x86_64 **−11635 (−1.64%)** over 565 images, arm64 **−9248 (−1.62%)** over 564, 332 changed each, **ZERO grew**. ★★★ **RED-ON-DISABLE NEEDS A THIRD ASSERTION — THAT THE MUTANT WAS READ.** ✔MEASURED: two of four arms on `D-GATE-THE-WSL-LEG-CANNOT-PROVE-ITS-OWN-ARM64-LEG-RAN` asserted NOTHING and both looked green — shadowing `qemu-aarch64` on `PATH` does nothing because Linux dispatches aarch64 through **binfmt_misc**, which invokes the interpreter by ABSOLUTE path; and the second predicate was written `emulator-missing: [1-9]` when the runner prints `0 emulator-missing`, **count first**, so it could never fire. Both were caught ONLY by asserting the mutant's output DIFFERED from the clean output at all. ⚠ That row also records why the WSL leg could not prove its own reason for existing: the test count is IDENTICAL with or without an emulator (1691 Windows − 18 root-host-only guards = 1673), and `--output-on-failure` DISCARDS the `[arm-ledger]`/`[coverage-boundary]` lines that answer the question, because only PASSING tests print them. `wsl-leg.sh --mode full` now refuses unless an `arm64:` spec appears under `ran=` with no nonzero `emulator-missing`. ⚠ `D-CARRIAGE-REPO-URL-ROLE-IS-INVISIBLE-TO-THE-PATHS-GUARD`: the fix the row PRESCRIBED was itself defective and running it proved so — admitting URL-shaped role names yields two false reds including upstream `sqlite.git`, which no compare-against-the-project-name rule can accept; what landed is agreement AT THE CLONE SITE (a URL cloned into a repository-root constant must name the same repository), org-blind and allowlist-free. ⓘ **FOUR-LEG GATE, all on the committed tree:** Windows **1691/1691** (+ all 18 repo guards re-run after later `.plans`/guard edits), WSL x86_64 **1673/1673**, qemu arm64 **proven RUN not merely counted**, macOS arm64 **1673/1673**. ⚠ A `16 vs 20` result reported mid-cycle as a possible miscompile is **REFUTED**: four binaries, 120 runs, no binary produced both answers; the split tracks the SOURCE and gcc agrees with the shipped one at every `-O`. ⚠ **A SOURCE mutant is NOT isolated by a per-lane `build/<lane>` root** — one lane's mutant was compiled by another and reported as the tree's state, costing a full false-regression investigation. Isolate the SOURCE.

★★ **P38 (`57b75813`):** *"nine rows closed, none opened, and the P36 regression that broke every multi-CU jump table."* 📄DOCUMENTED from the commit subject; re-derive its detail from `git show 57b75813` rather than from this line.

★★★ **P37: THE sqlite PROBE FOUND A P36 REGRESSION THAT BROKE EVERY MULTI-CU PROGRAM CARRYING A JUMP TABLE — the cross-CU merge remapped a function's symbol and its relocations but not its `blockSymbols`, so a switch table pointed at ids the merged module never declared (1313 undefined symbols on the sqlite3 CLI for pe64, 1483 on testfixture). FIXED, red-on-disable exercised with an md5 witness, and pinned by a two-CU corpus arm that exits 42. ⚠ THE OPERATOR'S FIRST HYPOTHESIS (`D-OPT11-LAZY-IMPORT-EDGE`) WAS REFUTED BY ONE COMMAND — the same manifest fails at `--config=debug` too, and debug carries no import machinery. P36 REMAINS TRUE:** THE GOAL CORPUS BUILDS AND RUNS AT RELEASE, AND THE THING IN THE WAY WAS ONE C TYPE COUNTED TWICE.** ✔MEASURED, Windows x86_64, 103-TU sqlite via `--project … --config=release --jobs 4`: **rc=1, 5 × `I_StoreValueTypeMismatch`, NO ARTIFACT → rc=0, ZERO errors, a 5,108,736-byte `speedtest1.exe` that RUNS** (`--size 3 --testset main`, every test executes, `PRAGMA integrity_check` passes, exit 0). ★★ **THE DEFECT: `reinternType` KEYED A HOST COMPOSITE ON ITS SOURCE DECLARATION SITE**, so `typedef struct Bitvec Bitvec;` in a header gave every TU its own incomplete `Bitvec` and the merge kept them all apart. Host identity is now a cross-CU STRUCTURAL identity (`CompositeIdentityIndex`), and a forward declaration RESOLVES to its tag's definition rather than being erased. ★★★ **FOUR ATTEMPTS, AND THE THREE FAILURES ARE WORTH MORE THAN THE FIX** — (1) a full recursive layout digest still forked `BtCursor`, because *the completeness of something you only POINT AT was leaking into your own identity*, this very defect one level down; (2) the obvious repair (tag-only behind a pointer) made two structs share a key while their fields still reinterned apart and `completeComposite` ABORTED ⇒ **the invariant everything since is built on: equal identity ⇒ every field reinterns to the same host type**; (3) the exact de Bruijn digest was CORRECT and took **952 s of CPU with no output** on sqlite's mutually recursive `sqlite3`/`Vdbe`/`Parse` cluster — a subtree that back-references an OUTER composite cannot be memoized ⇒ replaced by ordinary **iterative partition refinement**, which converges in **2 rounds over 14,663 nodes**; and (4) a bug in the memo key itself, which bit-packed a mode flag into bit 63 and then XOR-ed a constant whose bit 63 is also set. **Hash a key; never bit-pack one into a word whose width nobody re-checked.** ★★★ **AND THE LAST FORK WAS NOT IN THE MERGE AT ALL: A PER-CU AST NODE ID INSIDE A TYPE'S NAME.** ✔The index counts its own failures, and that count is what found it — **98 tags FORKED, EVERY ONE WITH A SINGLE LOCAL LAYOUT SIGNATURE** (`Parse`, `Table`, `Select`, `Index`, `KeyInfo` …), which cannot be a layout difference. An anonymous member is bound as `<anon:RULE:NODEID>` and that name reaches the interned TYPE, so two CUs including one header give one anonymous `union` two names and **every named struct that reaches it inherits the split**. The spelling had THREE inline readers and no owner; it has one now (`core/types/anon_member_name.hpp`). **98 → 0.** ★ **THE VERIFIER'S TYPE-MISMATCH MESSAGES NOW NAME THE TYPE, NOT JUST THE ID** — *"typed 5167 (kind 27) … pointee 6074 (kind 27)"* said *"two pointers, different numbers"* and stopped; it now reads `Ptr<Struct 'Bitvec'>` vs `Ptr<Struct 'Bitvec' incomplete>`, **and that sentence is what identified defect (1)**. The id stays — it is the only thing that distinguishes two forks of one spelling. ⚠⚠ **P36 RAN ACROSS TWO SESSIONS AND THE INTERRUPTION COST THE REPORTS, NOT THE WORK.** Every one of nine lanes' CODE is in the tree and gated. Lanes J, P and S wrote their rows to FILES and those applied verbatim; lanes **G, K, L, N, Q and R reported by citing a scratchpad PATH**, and their row text, red-on-disable transcripts and mutant md5s went with the session. The fold RE-DERIVED their rows by measuring the tree — the staticlib member names and the object-input link re-proved by RUNNING the shipped binary, the summary sections by reading the ten shipped format documents — but **a passing test is not proof that the test can fail**, so six rows close on the fix being present and exercised, say so in as many words, and owe their falsifiability arm. ⚠ **AND A STALE TEST HAD TO BE INVERTED:** `AsmDataSection.Int128GlobalFailsLoud` asserted a refusal a lane had since implemented — **a test that asserts a refusal is a claim about the product, and inverting the product without inverting the test leaves a gate that contradicts the compiler.** ⚠ **A TEST SOURCE CARRYING FOUR RAW NUL BYTES** made `grep` call it binary, which blinded every text-scanning guard and made `anchor_registry_guard` report the sentence *"Binary file … matches"* AS AN ANCHOR ID. Fixed; the loud half was the smaller half. ⚠ RE-MEASURE the balance with `python scripts/check-anchor-balance/check-anchor-balance.py` rather than re-quoting any figure on this line.
**Branch:** `feature/c23-conformance-burndown-4` · **HEAD:** the P39 commit — re-derive it with `git log --oneline -1` rather than reading a hash here: the public-repo bot rebases and squash-merges, so a written hash is UNSTABLE. ⚠ **Any `dss-code-prime` spelled as the BUILT COMMAND in a row or commit message older than 2026-08-24 is HISTORICAL, not stale** — the tool is named `dsscp` since that date, while the PROJECT and the REPOSITORY keep `dss-code-prime` (`project(…)`, the GitHub URLs, the funding and issue-template identity, `.claude/skills/dss-code-prime/`, and **every checkout path on every host**). ⚠ **Any path spelled `tools/…` in a commit message or a row older than 2026-08-19 is HISTORICAL, not stale** — that directory no longer exists; every script lives at `scripts/<name>/<name>.{sh,ps1,py}`. ⚠ **Likewise, any `c-subset` in a commit message or a row older than 2026-08-24 is HISTORICAL, not stale** — the C front end has been named `c` since that date, and the mapping is MECHANICAL — ✔MEASURED 2026-08-24 by applying it to every path-shaped mention in `.plans/**` + `.claude/**` and asking the filesystem: **834 of 862 resolve**, the 28-mention residue being examples deleted or renamed for reasons unrelated to this rename, so a miss is pre-existing staleness rather than a hole in the rule: `examples/c-subset/…` reads `examples/c/…`, `tests/corpus/c-subset/…` reads `tests/corpus/c/…`, `tests/corpus/diagnostics/c-subset/…` reads `tests/corpus/diagnostics/c/…`, `c-subset.lang.json` reads `c.lang.json`, and `--language c-subset` reads `--language c`. ⚠ `tsql-subset` is a DIFFERENT language and is spelled correctly — it is not part of this mapping. ★★ **BOTH `D-C-*` AND `D-CSUBSET-*` ANCHOR-ID PREFIXES DENOTE THE C LANGUAGE**: the 426 `D-CSUBSET-*` ids are FROZEN by operator ruling and are never to be renamed, so a grep for either prefix ALONE misses part of the language's rows.

---

## 0.000000000000000000000000000000000000 ★★★ THE PRIORITIZED BURNDOWN LOOP — PRODUCTION ERRORS FIRST

> ⚠⚠ **THE REGISTRY BECAME TWO FILES ON 2026-08-25 (cycle P34).**
> `.plans/_deferred-anchor-registry-production.md` (1386 rows, 473 OPEN) and
> `.plans/_deferred-anchor-registry-harness.md` (598 rows, 192 OPEN, plus the Allowlist).
> **PRODUCTION** is a defect a user of the compiler could hit; **HARNESS** is one only we can hit.
> Every instrument globs `_deferred-anchor-registry*.md`, so `burndown-queue` and
> `check-anchor-balance` read both with no flag — but a HUMAN reading one file is reading half
> the registry, and the production half is the one the operator asked to be able to see.


**Operator instruction 2026-08-24, verbatim:** *"grab anchors from this list + the handoff, make a
prioritized /loop considering production errors the most prioritized items and address all of them.
best long term solution with no workaround, first class implementation, 100% config driven."*

### ★★★ THE QUEUE IS AN INSTRUMENT, NOT A LIST — AND THAT IS THE WHOLE POINT

```bash
python scripts/burndown-queue/burndown-queue.py --band P0 --schedulable --evidence
```

⚠ **DO NOT READ THE NEXT PICK OFF THIS PAGE. RE-DERIVE IT.** This project's own memory carries the
rule in capitals, and it was paid for **six** times — twice reaching the operator: a queue named an
already-closed row; a backlog answer recommended a row closed months earlier; four claims in one
handoff header went stale at once; and on 2026-08-24 alone three separate status claims in this file
were wrong in three different directions. A written queue is stale the moment the next cycle closes a
row. `burndown-queue` reads the rows and sorts them, so the queue is a **VIEW**, never a copy.

★ It REUSES `check-anchor-balance` rather than re-implementing it — that script owns this
repository's hard-won vocabulary for *is this row open*, *is it gated*, *has its trigger fired*,
*is its opener discharged*, a regex family whose comments record six separate defects including two
where a NEGATED or ATTRIBUTIVE mention flipped a verdict. ✔The two instruments agree on the
population exactly: **1023 OPEN**, zero residue.

### THE BANDS — "production error" means the shipped compiler does something WRONG, not that it is missing something

| band | meaning |
|---|---|
| **P0 WRONG-OUTPUT** | DSS produces an incorrect result, drops something silently, or crashes on legal input. **Ships a bad binary. Nothing outranks these.** |
| **P1 REFUSED** | DSS refuses, cannot parse, cannot link or cannot build something a reference accepts — real code that does not compile today. |
| **P2 DIVERGENT** | a conformance divergence or an absent capability in a PRODUCT namespace. |
| **P3 HARNESS** | the test / gate / build / cycle instruments. Costs confidence rather than correctness. |
| **P4 RECORD** | plans, registry, documentation. |
| **P5 ENV** | environment and upstream — explicitly not ours to fix in the compiler. |

### ✔MEASURED 2026-08-24 at `a881b2a1` — RE-MEASURE, DO NOT RE-QUOTE

```
BAND       total  schedulable   RED  ORANGE  YELLOW  GREEN
  P0         109           98    37      16       5      3   WRONG-OUTPUT
  P1         161          143    40      34       5      1   REFUSED
  P2         575          529    49      35      10     13   DIVERGENT
  P3         150          148    51      52       6     19   HARNESS
  P4          23           23    10       5       2      4   RECORD
  P5           5            5     3       0       0      1   ENV
                1023 OPEN — 946 SCHEDULABLE, 77 gated on a trigger that has not fired
```

★ **THE ARITHMETIC, STATED RATHER THAN AVOIDED:** *"address all of them"* over P0+P1 is **241
schedulable rows**. At P32's measured rate — 8 rows closed by 4 lanes in one cycle — that is
**≈30 cycles** for the production bands and roughly a hundred for the whole registry. The loop below
is built to be run that many times, which is why its next pick is a COMMAND and not a bullet.

### ⚠ THE BAND IS A SORT KEY, NOT A VERDICT, AND THE INSTRUMENT SAYS SO ON EVERY RUN

✔MEASURED, and it is the same defect this project already caught once: the first draft banded **201**
rows P0, and reading the head of that band showed the sieve counting rows that **explicitly deny**
being production errors — *"NOT A MISCOMPILE"*, *"FAIL-LOUD, NEVER A MISCOMPILE"*, *"this is NOT a
silent miscompile"*. Three negation classes had to be separated, and only the first is ordinary
negation: **DENIAL** (*"not a miscompile"*), **REQUIREMENT** (*"must NOT silently truncate"* — a rule
the FIX must satisfy, not a description of the defect), and **COUNTERFACTUAL** (*"would have SILENTLY
ACCEPTED"* — a hazard avoided is not a hazard shipped). ⇒ **201 → 109**, a 92-row correction.
★ Every demoted row is PRINTED under `DEMOTED OUT OF P0` with the phrase that demoted it: a sieve
that hides what it disbelieved cannot be checked, and the direction it would hide in is the
dangerous one. ⓘ Precedent: the *"103 misglyphed registry rows"* census whose true number was **4**.

### ★★ HOW THE LOOP CONSUMES IT — THE NAMESPACE IS THE LANE PARTITION

✔MEASURED: the P0+P1 schedulable set spreads over 35 and 38 namespaces respectively, and the
namespace maps almost exactly onto a FILE SET — `D-CSUBSET-*`/`D-C-*` on the C front end and
`c.lang.json`, `D-LK-*`/`D-LINK-*` on `src/link/**` and the format documents, `D-LIR-*`/`D-MIR-*` on
the backend tiers, `D-DIAG-*` on the diagnostic vocabulary, `D-PP-*` on the preprocessor. So a wave
is chosen by taking the highest band and dealing the rows out **by namespace**, one namespace per
lane.

⚠ **`D-CSUBSET-*` DOMINATES BOTH PRODUCTION BANDS — 29 in P0 and 47 in P1 — AND THAT IS THE
BOTTLENECK, NOT THE PRIORITY.** Every one of them lands on `src/dss-config/sources/c.lang.json` or
the C front end, and at most ONE lane per wave may write `src/dss-config/**` in a shared tree,
because `tests/CMakeLists.txt` points every test in every lane's private `build/<lane>` at the ONE
live config tree. Left alone, that caps the loop at one C lane per cycle and makes 76 rows serial.

★★ **TWO THINGS LIFT THE CAP, AND P33 IS RUNNING BOTH:**
1. **A lane in its own `git worktree` gets its own `src/dss-config/`**, so `DSS_CONFIG_ROOT` resolves
   inside its tree and it may write config freely. The cost is a source copy plus a clean build; the
   fold applies its diff by explicit path. P33 wave 1 runs two lanes this way.
2. **`D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED`** — a ctest-RUN-time
   config snapshot at the `dss_add_test` chokepoint — removes the exposure at the reader instead of
   the writer, for all 1,258 `loadShipped` call sites in `tests/` at once. It is P33 wave 1 lane A.

⇒ **The standing wave shape: 4 lanes, each its own worktree, dealt by namespace, in band order.**

### THE NEXT PICKS — P0 RED, SCHEDULABLE, GROUPED BY THE LANE THAT WOULD TAKE THEM

⚠ **Re-derive before starting any of these.** They are printed so a reader can see the SHAPE of the
head of the queue, not so a cycle can copy them.

**`D-CSUBSET-*` — 12 RED, the largest single lane's worth, and the C front end is the bottleneck:**
`ZERO-WIDTH-BITFIELD-ALIGNMENT` (a MEASURED silent layout miscompile whose trigger has FIRED) ·
`DARWIN-STRUCT-LAYOUT-DISAGREEMENT` · `SUPPRESSED-SHIPPED-ROW-SIGNATURE-UNCHECKED` (a live silent
wrong-ABI call) · `SYNC-BUILTIN-BARRIER` (invisible on a single thread, which is why it survived) ·
`TYPEDEF-HEAD-DECORATION-TYPE-HIJACK` · `LINKAGE-SPECIFIER-CONFLICT-SILENT-LAST-WINS` ·
`INT128-DATA-GLOBAL`, `INT128-FLOAT-CONV`, `INT128-NARROWING-CAST-SITE-INCOMPLETE` (one arc) ·
`ATTRIBUTE-IGNORED-FOR-DECL-KIND-SILENT` · `BLOCK-SCOPE-UNKNOWN-ATTRIBUTE-SILENT` ·
`GNU-DEPRECATED-MESSAGE-SILENTLY-DROPPED`.

**`D-DIAG-*` — 4 RED, one lane, one vocabulary:** `CODE-PREFIX-DEFAULT-IS-SILENT` ·
`LINE-NUMBERS-ARE-POST-EXPANSION-WHILE-THE-FILE-NAME-IS-ORIGINAL` · `MAXPERCODE-SILENT-COALESCE` ·
`UNSUPPRESSABLE-FAMILY-UNDECIDED`.

**`D-PP-*` — 3 RED, the preprocessor:** `DEFINED-VIA-MACRO-EXPANSION` ·
`HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW` · `REMAP-ORIGIN-OFFSET-UNVALIDATED`.

**The backend tiers — 5 RED across two adjacent file sets:** `D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE`
(*a refusal that crashes is not a refusal*) · `D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED` ·
`D-MIR-SYNTH-PASSES-UNVERIFIED-ON-SINGLE-CU-PATH` · `D-MIR-VERIFIER-STORE-CALLARG-TYPE-BLIND` ·
`D-HIR-CONSTEVAL-UNSIGNED-WRAPAROUND-NOT-MODULAR`.

**`D-FFI-*` — 2 RED:** `ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY` (⚠ also an
agnosticism break by its own title) · `DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED`.

**Singletons that still ship wrong output:** `D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE` ·
`D-LANG-TYPE-IDENTITY-QUALIFIER-BLIND-VS-C23-REDECL` · `D-MIRTEXT-GLOBAL-FLAGS-DROPPED-BY-ROUNDTRIP` ·
`D-LK-PE-OBJ-ARM-CARRIES-NO-UNWIND-INFO` (⚠ **same family as P33 lane D's row** — check whether one
change closes both before scheduling it separately) · `D-CONF-REFERENCE-DIFFERENTIAL-ORACLE` ·
`D-SQLITE-CLI-BUILT-ON-NO-LEG` · `D-PROGRAM-PROJECT-WIDE-PARSE-GATE-MASKS-CENSUS` ·
`D-LINK-WRITER-DANGLING-SYMLINK-CLAIM-MISROUTE`.

### ★★★ THE SEQUENCE AFTER THE BURNDOWN — OPERATOR-SET 2026-08-24, AND IT OVERRIDES THE PLAN'S OWN NUMBERING

**1. THE ANCHOR BURNDOWN** — this loop, P0 first, until the production bands are clear.

**2. FC20 — C STANDARDS BECOME LANGUAGE DOCUMENTS** (plan 23 FC20, operator-decided across four
exchanges). ★ A `-std` ENUM would be `if (std >= C23)` scattered through the front end — a
source-identity branch in shared substrate, i.e. the hard veto. A standard is a LANGUAGE DOCUMENT
and the CLI flag is a document SELECTOR: `--language c23` resolves to *load `c23.lang.json`*, and
the engine learns nothing about standards at all. ✔**THE SELECTOR ALREADY EXISTS AND ALREADY
RESOLVES THAT EXACT FILENAME** — measured by running the shipped binary: `--language c23` fails
with `error[C_InvalidLanguageName] c23: no shipped language config found`, having tried
`src/dss-config/sources/c23.lang.json`. The mechanism is built; it has no document to find.
⇒ FIRES: [[D-CSUBSET-BARE-ASM-ACCEPTANCE-IS-UNCONDITIONAL]] (gated on exactly a strict-conformance
mode existing) · [[D-CONFIG-GRAMMAR-ISA-AND-IDENTIFIERCLASS-BELONG-IN-THE-LANGUAGE-BLOCK]] ·
[[D-CONF-CORPUS-NO-DIRECTION-FOR-A-C23-REMOVED-CONSTRUCT]] (whose trigger cell names FC20 by name).
⚠ FC20 **fires** that last one; it does not **close** it. Its closing work is in the CORPUS, not the
compiler — a `@max-stdc` companion to `@min-stdc` so the two keys BRACKET a probe to a level band,
plus a third direction arm. ⚠⚠ And the obvious shortcut is the trap the row spells out: filing a
C23-REMOVED construct as `@direction B / @min-stdc 202311` goes GREEN while silently redefining what
every other direction-B row asserts, and `@direction A / @min-stdc 0` is worse — gcc at 202000
accepts, so if DSS also accepted, `pin()` returns **Agreement** and the corpus would CERTIFY a
C23-removed construct as conformant.

**3. FC19 — CONFORMANCE ON A BIG-ENDIAN TARGET (s390x)** — operator-sequenced HERE, 2026-08-24.
⚠⚠ **THIS REVERSES THE PLAN'S NUMERIC ORDER AND THE REVERSAL IS DELIBERATE: FC19 RUNS AFTER FC20.**
A future reader must not "correct" it back to numeric order — plan 23 numbers these 19 then 20, and
the operator has since sequenced them 20 then 19.
★★★ Why it matters, in the plan's own words: **every conformance claim this project makes has only
ever been tested on little-endian machines.** Struct and union layout, bit-field allocation, integer
representation, `unsigned char` aliasing and the `__BYTE_ORDER__` / `__LITTLE_ENDIAN__` /
`__BIG_ENDIAN__` predefines are all C-VISIBLE and all endianness-dependent — so a whole class of C
semantics is UNEXERCISED, not merely untested on one CPU.
★ **s390x, and not something cheaper, because it is the only commercially live big-endian platform**
— real distro ports, real glibc, and therefore the only one that can run the sqlite corpus, which is
what "real support" has to mean. ✔**THE WHOLE CHAIN WAS VERIFIED BY EXECUTION on 2026-08-24 before
the plan row was written**: `s390x-linux-gnu-gcc 13.3.0` links a hosted ELF64 big-endian IBM S/390
binary, `qemu-s390x 8.2.2` RUNS it, and a program returning the first byte of `0x12345678` exits
**18 (0x12)** — big-endianness observed by RUNNING, not read from a manual. `sizeof(long)==8`, so
s390x is LP64 like the existing elf64 legs and brings NO data-model confound.
★★ It is also big-endian in its INSTRUCTION STREAM (`eb bf f0 58 00 24  stmg %r11,%r15,88(%r15)`
reads MSB-first), which is strictly more than `aarch64_be` can ever test — A64 instructions are
little-endian in memory at both endiannesses.
⚠ Plan 23 §2.F sizes it as **four phases, not one**. Do not plan it as a target-descriptor edit.
⇒ FIRES / UNBLOCKS: [[D-ASM-TARGET-DECLARES-NO-BYTE-ORDER]] (gated until 2026-08-24 precisely
BECAUSE no shipped target declared a byte order — now dischargeable by a target existing, not by
argument) · the `endianness` target key the descriptors deliberately withheld (*"the day a consumer
exists, it gets declared and THEN gets its coherence example"*) · [[D-PP-ENDIANNESS-PREDEFINES]]'s
untested half · [[D-FULLC-STDBIT-BIG-ENDIAN-NATIVE]] · [[D-FFI-STDINT-PTR-WIDTH-ILP32]] (names
itself the same shape) · [[D-LK4-RODATA-PRODUCER-EXOTIC]] (endianness per type width).

### THE RULES THAT BIND EVERY CYCLE OF THIS LOOP

- **Best long-term, no workaround, first-class, 100% config driven** — operator, restated
  2026-08-24. Vocabulary goes in the `.lang`/`.target`/`.format` documents; the engine never
  branches on language, arch or format identity.
- **CLOSE, DO NOT FILE.** A new row is a LAST RESORT. Fixable inside owned paths without breaking
  the bar ⇒ FIX IT and record it inside a row already being closed. *"Refused but not fixed"* is
  **NOT** closed.
- **A deferral trigger is a PREDICATE, not a ritual** — measure it, and if it is false the gate never
  applied. ✔Two were measured false on 2026-08-24 before a lane started: the `__alignof__` row's
  *"the references disagree"* (gcc 13.3.0, clang 19.1.1 and clang 18.1.3 all answer **64**), and the
  `D-FF1-` row's *"anchor ids are FROZEN"* (that ruling covers the 426 `D-CSUBSET-*` ids only).
- **77 rows are gated on a trigger that has NOT fired.** They are not in the queue and must not be
  dragged into it; `--schedulable` is the flag that keeps them out.

> ⚠ **SUPERSEDED BY THE P33 BURNDOWN LOOP ABOVE, 2026-08-25 — KEPT BECAUSE ITS TICKED ROWS ARE THIS
> FILE'S ONLY RECORD THAT THOSE ITEMS WERE TAKEN.** Read it as HISTORY. The live queue is the
> `burndown-queue` VIEW above; re-derive every pick from the registry, never from either list.

## 0.00000000000000000 ★★★ THE OPERATOR-SELECTED QUEUE — TICK EACH BOX AS IT LANDS

**Selected by the operator 2026-08-19** from the AP5/AP6 commit + asm-feature (this PR) pending deferrals, in
their stated ORDER: **pending feature completion → production errors → harness/test errors → others.**
★ **THIS IS THE LIVE QUEUE. Re-derive each pick from the REGISTRY before starting it** (a list is a pointer, not
a verdict — twice now an item on a list like this was already closed underneath it), tick its box in the SAME
commit that lands it, and never delete a ticked row.

- [x] **P16 — `D-UPSTREAM-SQLITE-FILEIO-WINDIRENT-IS-MSVC-ONLY` + `D-HARNESS-FAILING-REFERENCE-ORACLE-COLLAPSES-TO-NO-ORACLE`. ✅ DONE 2026-08-19.**
      BOTH CLOSED, net −1. The oracle-collapse anchor had been **cited since 2026-08-18 without ever existing** —
      written and fixed in the same commit. The windirent row closed because its central claim is measured false:
      DSS **and MSVC** both compile `ext/misc/fileio.c`; only mingw-on-Windows does not, which is a configuration
      upstream never claims. ⇒ nothing to report upstream for THAT one.
- [x] **P17 — OPERATOR-INSERTED, not from this list: the `scripts/` consolidation + the script index. ✅ DONE 2026-08-19.**
      Six rows, all BORN CLOSED, net 0 — two of them opened by the INDEPENDENT AUDITS at the end of the
      cycle rather than by the work itself, and a third rewritten because an audit measured its central
      claim false. `tools/` (24 tracked files) merged into `scripts/` one-directory-per-script;
      **17 repo-root derivations** were one level short afterwards and each was repaired and asserted;
      every script declares a `PURPOSE:` line; `scripts/README.md` and `references/scripts.md` are GENERATED
      from those declarations and held to the tree by `scripts_index_guard` (11 red-on-disable arms).
      `/dss-cycle` now MANDATES that a script added/renamed/deleted/repurposed updates the reference in the
      same commit. Also: the gate ran **serially on every host** — `run-gate` and `local-build` now default
      `CTEST_PARALLEL_LEVEL=8` (✔9741 ms → 2648 ms on six tests; the full suite was 899 tests / 2602 s).
- [x] **P18 — OPERATOR RULING: `D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED` WITHDRAWN. ✅ DONE 2026-08-19.**
      The anchor asked for a gate that every `.sh` carry a `.ps1`. ✔MEASURED: **11 of 21 script directories
      have no `.ps1` and every one is correct** — eight are Python (cross-platform already), two are POSIX-only
      by nature (`wsl-leg` runs inside a WSL distro; `profile-compile` drives a POSIX toolchain). A gate cannot
      tell a deliberate POSIX-only script from a forgotten twin, so it would need an eleven-entry allowlist —
      the convention written twice, reddening honest work by default. Rule now lives in `dss-cycle/SKILL.md`
      and the `dss-code-prime` skill (conventions authority). Net −1.
- [x] **P19 — OPERATOR RULING: `D-GATE-SCRIPT-PS1-CONTENT-DRIFT-UNCHECKED` WITHDRAWN + 3 false citations repaired. ✅ DONE 2026-08-19.**
      Twin parity is a REVIEW obligation, not a detector: *"the parity must be checked in the review, before the
      commit, when the script is being created or modified. Not after and not a script to it."* An independent
      audit classified all **50** surviving citations of the two withdrawn rows and found **three that asserted
      something false** — one claiming the sibling row *stays OPEN*, two instructing a future cycle to build the
      forbidden guard. All three repaired. Net −1.
- [x] **P20 — `D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER` CLOSED. ✅ DONE 2026-08-19.**
      `asm goto` lowers, compiles and RUNS: 42 on pe64 native, WSL elf64-x86_64 and qemu-aarch64, at
      debug AND release; macho64-arm64 compiles (no runner off-Mac). The queued line above predicted a
      §B design fork; there was none — every alternative was refuted by measurement rather than being
      merely less attractive, so the hard part landed. **What the row did NOT name and the cycle found:**
      the fall-through edge was missing from the MIR CFG entirely. Net −1.
- [x] **P21 — `D-EXAMPLES-DEPENDSON-NO-RELEASE-OPTIMIZER-ARM` CLOSED. ✅ DONE 2026-08-20.** Net 0.
      The queued line above described a MANIFEST edit. ✔MEASURED: that edit alone would have asserted
      NOTHING — both runners built the prerequisite library at the BASELINE configuration in every arm, so
      a `release` arm re-compiled only the final exec and linked it against a DEBUG archive. Both runners
      fixed, the examples given something for the pipeline to transform, and all **12** `dependsOn` entries
      armed with `mustDifferFromBaseline`. ✔All 12 library images differ debug→release across four targets;
      macho64-arm64 and elf64-aarch64 verified BY EXECUTION on real hardware, exit 42 in all eight arms.
      **The row predicted a finding and got two** — see §0.00000000000000000000000.
- [x] **P22 — `D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM` CLOSED. ✅ DONE 2026-08-20.** OPERATOR-ORDERED
      2026-08-20 for the IMMEDIATE next cycle** (*"remember to address anchor in the immediate next cycle.
      only best long term solutions! no workarounds"*). DSS cannot link a **file-local** function out of an
      archive on pe64 or macho64: those readers classify a non-external defined symbol as a bodyless block
      label instead of an atom, so its bytes never enter the image. P21 landed the FAIL-LOUD half (a
      reader-agnostic partial-coverage guard, `F_ObjectReaderSymbolBodyDropped`), so the loss can no longer
      be silent — what remains is the CLASSIFICATION. ⚠ Reading the row first is not optional: it records a
      boundary the guard does NOT catch (a file-local function TRAILING an external one rides along inside
      the wrong atom), and a third, independent bug on macho64-x86_64 (conflicting relocation `nativeId`s
      between the object and image vocabularies). ⚠ The mach-o option involves `MH_SUBSECTIONS_VIA_SYMBOLS`,
      a SHIPPING WIRE-FORMAT change that Apple's ld64 reads as a licence for symbol-granularity
      dead-stripping — if the design lands there, it is a §B, not a lane decision.
      ✅ **IT LANDED THERE, AND IT WAS NOT A §B** — no fork existed once the evidence was in: the flag
      WITHOUT `N_ALT_ENTRY` markers is catastrophic (five dense-switch examples crash, ~85% of `__text`
      stripped) and WITH them `__text` is byte-identical, so one option was refuted by measurement rather
      than merely being less attractive. Widened witness: **525 objects** through real ld64 under
      `-dead_strip`, flag ON vs OFF, **0 run disagreements**. A `static` helper now links out of an archive
      on ALL FIVE legs, run-witnessed at both configs, macho64-x86_64 included (Rosetta, on the Mac).
      ⚠ **The row named THREE further problems and understated the fourth.** The `nativeId` conflict it
      mentioned was not two honest encodings — the image documents were SELF-REFUTING under the packing
      their own comments state. And the leg it did not mention at all, `D-LK-MACHO-ISDATA-NO-CALL-SIGNAL`,
      turned out to mean macho64-x86_64 had **no working static-library path whatsoever**. Net −2.
- [x] **P23 — THE THREE ROWS P22 OPENED, PLUS `D-HARNESS-PE64-LIB-ACQUISITION-IS-HOST-DEPENDENT`. ✅ DONE 2026-08-20.**
      Operator argument, verbatim: *"keep going. address all three rows +
      D-HARNESS-PE64-LIB-ACQUISITION-IS-HOST-DEPENDENT in this cycle"*. **All four CLOSED**, plus three more
      that fell out on the way (`D-HARNESS-ELF-LEG-HOST-SYSTEM-PROVIDER-UNSATISFIABLE-OFF-LINUX`,
      `D-LK-OBJECT-WEAK-DEF-RELOCATABLE`, `D-LK-PE-ALTERNATENAME-DECLARE-AND-REFUSE`). Net **−1**
      (1018 → 1017, closed 7, opened 6) — the first improvement in three cycles, and still thin.
      **TWO OPERATOR RULINGS WERE TAKEN MID-CYCLE AND MUST NOT BE RE-LITIGATED** (both are recorded in full
      in the cycle section below): (1) build the weak-DEFINITION machinery, whose COFF spelling is **COMDAT
      select-any**, NOT `WEAK_EXTERNAL` — *"an implementation gap and a format incapability are different
      facts; never let the first be recorded as the second"*; (2) build the `WEAK_EXTERNAL` reader+writer pair,
      **reader first**, as a DIFFERENT MECHANISM FOR A DIFFERENT FACT (a weak ALIAS, not a weak definition).
      ⚠ The alias row closed on **both tiers, all three formats** — object and final image — and the
      arc was not finished until `mergeModules` stopped dropping every alias name the moment more than one
      module was linked. **A capability nothing can reach is not a capability.**
- [x] **P24 — `D-HARNESS-PE64-LIB-ACQUISITION-IS-HOST-DEPENDENT` (HIGH). ✅ DONE 2026-08-20 in cycle P23**,
      pulled forward by the operator argument above rather than run as its own cycle. Its prescribed work had
      ALREADY LANDED at `3e86a187` without the row being marked; what was outstanding was the clause that
      DECIDES it — the cross-host re-measurement, which is the measurement P13 never took.
      ✔5 of 5 legs resolve build inputs from a COLD cache on the native aarch64 VPS (`ls /mnt` EMPTY),
      8 pinned archives / 11,621,552 B / 8-of-8 sha256 verified, plus an OFFLINE warm 5/5 and a NEGATIVE
      CONTROL returning **0/5** so the instrument can say no. Closed its ELF sibling on the same evidence.
- [x] **P24 — `D-TEST-INTEGRATED-RUNNER-WALKS-EVERY-EXAMPLE-IN-ONE-THREAD`. ✅ DONE 2026-08-21**,
      pulled ahead of P25 by operator instruction. Closed 1, opened 0, four rows born closed.
- [x] **P25 — `D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR`. ✅ DONE
      2026-08-21.** The branch's only 🔴 HIGH, closed with a four-mutant red-on-disable pin.
      Closed 1, opened 1 (trigger-gated), 1 born closed, 1 opened-and-closed inside the cycle.
- [ ] **P26 — THE REST OF THE P25 BATCH: every row P23 opened that P25 did not reach.** ⚠ The
      four-lane partition below is unchanged EXCEPT that the LIR lane is DONE — re-derive from
      the REGISTRY at the pick, never from this list. ⛔ **`D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS`
      IS NOT IN THIS QUEUE AND MUST NOT BE PICKED**: it is TRIGGER-GATED and MUST-NOT-BUILD until
      a source construct can form a 128-bit operand that reaches the allocator as a VR-class
      value. Report "trigger not fired"; do not treat backlog order as a licence.
- [ ] **P25 (ORIGINAL SCOPE, FOR REFERENCE). Every row P23 opened, in FOUR DISJOINT FILE SETS, plus a §B the operator
      resolved as OPTION 3.** ✔Re-derived from the REGISTRY 2026-08-21 at commit `c6ef80e4`: **14 of the
      15 listed rows are still OPEN**; the one P24 closed is
      `D-TEST-INTEGRATED-RUNNER-WALKS-EVERY-EXAMPLE-IN-ONE-THREAD`. Partitioned by **disjoint file
      sets**, which is the axis that actually bounds a cycle — not row count.
      * **LIR lane** (`src/lir/*`, `src/dss-config/targets/arm64.target.json`) —
        `D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR`, the only 🔴 HIGH. Scope
        set by the operator ruling below: **OPTION 3 — the total row map + the no-pool refusal land
        HERE; the sub-register-aware allocator is P26 entry 1.**
      * **Text-tier lane** (`src/hir/hir_text.cpp`, `src/mir/mir_text.{cpp,hpp}`, `src/mir/mir.cpp`,
        `src/mir/mir_opcode.hpp`) — `D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM`,
        `D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET`, `D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED`,
        `D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS`. ⚠ The four are ONE lane because
        they SHARE `hir_text.cpp`/`mir_text.cpp` — splitting them would collide, not parallelise. ⏳ The
        last is a SUSPECT: **verify or discharge, never patch on suspicion.**
      * **Linker lane** (`src/link/format/{elf,macho,coff_object_reader}.cpp`) —
        `D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS` (take the
        `weakDefinitionDialects()` backend accessor in the SAME change — with one consulting writer it
        would have exactly one row to check), `D-LK-COFF-NAMELESS-UNDEF-EXTERN-SILENTLY-DROPPED`.
      * **Harness lane** (`real-examples/c/sqlite/*`, `scripts/ssh-arm64-vps/`) —
        `D-HARNESS-ARM64-VPS-CHECKOUT-IS-STALE-AND-ITS-PREBUILT-COMPILER-REFUSES-ITS-OWN-CONFIG`,
        `D-HARNESS-ACQUISITION-REPORT-HAS-NO-ROLE-SO-BOTH-DRIVERS-RE-PICK-THE-TCL-Z-PAIR`. ★ **Runs
        EARLY, and for a reason that is not tidiness: the stale VPS checkout will bite THIS cycle's own
        gate leg.** The two are one lane because they share `build-and-test.sh`.
      * **Wave 2, if the cycle has room — they COLLIDE with wave 1 and must follow it:**
        `D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY` (shares `enum_name_table.hpp`
        with the text tier and `object_format_kind.hpp` with the linker),
        `D-TYPEKIND-PASCALCASE-SPELLINGS-HAVE-TWO-OWNERS` +
        `D-CONFIG-GRAMMAR-LOADER-INLINE-CHAIN-VOCABULARIES-REMAIN` (both share
        `grammar_schema_json.cpp`), `D-GATE-ANCHOR-BALANCE-SELFTEST-FIXTURES-ARE-ANCHOR-SHAPED`
        (standalone, cheap).
      ⚠ **DELIBERATELY NOT BUILT, and that is the correct call rather than an omission:**
      `D-GATE-WRAPPED-CITATION-WITH-LEADING-HYPHEN-CONTINUATION-UNRECOVERED` — its own cell argues
      against building it (no failing case, known false-positive surface). Building what a row argues
      against is the speculative build §A.2 forbids, pointing the other way.
      ⚠ **NOT PICKABLE, said rather than silently skipped:**
      `D-CYCLE-LANE-SOURCE-TREE-IS-SHARED-SO-ONE-LANE-CAN-BLOCK-ANOTHERS-BUILD` is **§B — operator
      decision** (per-lane worktrees vs. the shared tree), and
      `D-CONFIG-GRAMMAR-ISA-AND-IDENTIFIERCLASS-BELONG-IN-THE-LANGUAGE-BLOCK` is **TRIGGER-GATED**.
- [ ] **P26 ENTRY 1 — THE SUB-REGISTER-AWARE ALLOCATOR (the AAPCS64 v/d register-view arc).
      ★★★ PLACED HERE BY OPERATOR CONDITION 3, 2026-08-21, AND THE PLACEMENT IS THE POINT.** Verbatim:
      *"'P26's headline' is not sufficient. It must be P26's FIRST queue entry. … 'Headline' is a
      softer word than the rule allows, and the difference is exactly how a queued row becomes a
      permanent one."* This entry is what makes P25's Option 3 **sequencing rather than deferral**; it
      pushes `P26+ — THE REST OF WHAT P21 AND P22 FOUND` down one slot.
      **What it must build:** declare the aliasing through the EXISTING facility (`d_k.subOf = v_k` —
      `w0.subOf = x0` is declared today and the vector file's identical relationship is not), flip the
      AAPCS64 cc lists to the v-views, and teach the allocator that assigning a parent CONSUMES its
      children. ⚠ **`TargetSchema::validate()` refuses a cc list naming a sub-register TODAY**, and its
      own comment states why the shortcut is not available: *"'make sub-registers allocatable' is not
      the alternative — a sub-register-aware allocator is, and that is a cycle, not a config edit."*
      `arm64.target.json`'s `v0` row says the same thing independently: *"A future SIMD/vector cycle
      that makes VR allocatable must add the subOf aliasing + callerSaved membership then."*
      ★ **P25 leaves this STRICTLY EASIER, not merely pending:** the arg-side row table is a
      prerequisite either way, and P25 builds it with the counter identity already expressed
      (operator Condition 1), so this entry touches the ALLOCATOR and not the call-convention tables.
      ★★ **AND IT MUST FLIP AN INVERTED PIN BACK** — P25's no-pool refusal test names this anchor in
      the test body precisely so this entry cannot close green without restoring the capability
      (operator Condition 4). ⚠ A guard rewritten to match whatever the code now does asserts nothing;
      the one P25 leaves behind names what it is temporarily standing in for.
- [ ] **P25 DETAIL (the sizing P23 wrote; re-derive each row from the REGISTRY before starting it) — EVERY ROW CYCLE P23 OPENED. ✔MEASURED at the committed tree: 22 opened — 19 PICKABLE,
      2 GATED, and 1 DISCLOSED PRE-EXISTING** (the instrument counts that last one separately:
      *opened 22 (created 21, disclosed-pre-existing 1)*, so it is NOT one of the 21 this cycle made). ★★★ **OPERATOR RULING 2026-08-21, verbatim:** *"we can commit
      + push with the positive balance due to the really long cycle this time, but record in handoff so these
      4 are closed next cycle."* ⚠ Said exactly: the ruling was given while the visible count was four; the
      fold then finished and the true count is the list below. **The RULING is what governs — this cycle's
      openings are next cycle's first work — and the LIST is what it governs.** The gate itself is NOT
      softened: `check-anchor-balance` still refuses `after > before` and still reported FAIL at +13.
      Standing order refined by the operator 2026-08-20:
      a cycle MAY end with rows it opened still OPEN, **provided the NEXT `/dss-cycle` addresses them first if
      addressable** — they become the next numbered entry and push what was queued down a slot. ★ Re-derive
      each from the REGISTRY before starting it; the sizing below is this cycle's and is not a verdict.
      * `D-TYPEKIND-PASCALCASE-SPELLINGS-HAVE-TWO-OWNERS` — ✔**the two owners ALREADY DISAGREE.**
        `lir_text.cpp`'s `typeKindName` and `type_reintern.cpp`'s `typeKindName` are two independent
        exhaustive switches over one PascalCase vocabulary. Ordinary lane: mint the table, delegate both.
      * `D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS` — the dialect key ships with
        **one** consulting writer (`pe::encode`'s Obj arm) because declaring it on the 10 ELF and 8 Mach-O
        documents today would create the very drift its parent row exists to prevent. ★★ Take the
        `weakDefinitionDialects()` backend accessor (mirroring `stackReserveVehicles()`) **in that same
        change and not before** — with one consulting writer it would have had exactly one row to check.
      * `D-HARNESS-ARM64-VPS-CHECKOUT-IS-STALE-AND-ITS-PREBUILT-COMPILER-REFUSES-ITS-OWN-CONFIG` — OPERATIONAL,
        and ✔it will bite the next cross-leg run: the VPS checkout sits at **`b52784a6` (P5c)** with **2,501**
        dirty files and a prebuilt compiler that refuses its own pipeline config, while the harness uses
        `SRC_DIR` as-is and never pulls.
      * `D-HARNESS-ACQUISITION-REPORT-HAS-NO-ROLE-SO-BOTH-DRIVERS-RE-PICK-THE-TCL-Z-PAIR` — `--acquire`
        reports `libraries[].as` and `.path` but not WHICH acquired file is Tcl and which is zlib, so both
        drivers re-pick by filename downstream of the instrument that already knew.
      ★★★ **AND THIRTEEN MORE, ALL OPENED BY THE STEP-10 AUDIT FOLD. Deliberately UNSIZED here:** a sizing
      written without re-measuring is exactly what the standing order forbids, and every one of these was
      named hours after the four above. Re-derive each from the REGISTRY at the pick.
      * `D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR` — 🔴 **HIGH, and the only one.**
        Reframed by lane R as a **LIVE SILENT MISCOMPILE**, not a latent gap. Take it FIRST.
      * `D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY` — five sites whose compile-time
        guard cannot fail on the case all four of their comments claim it catches.
      * `D-LK-COFF-NAMELESS-UNDEF-EXTERN-SILENTLY-DROPPED` — the twin of a row lane P closed.
      * `D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM` — made LOUD this cycle, not closed.
      * `D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED` · `D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET` ·
        `D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS` (⏳ SUSPECT — INFERRED, NOT EXECUTED;
        verify or discharge, never patch on suspicion) — the `.dssir` text tier.
      * `D-CONFIG-GRAMMAR-LOADER-INLINE-CHAIN-VOCABULARIES-REMAIN` — 12 chain vocabularies, 16 hits, all in
        one file. Enumerated and mechanical; this is WORK, not a row, and it was left only because the
        operator closed the cycle to new lanes.
      * `D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME` — the precondition lane R named.
      * `D-TEST-VOCABULARY-PROBE-HELPER-FOURTH-COPY-OUTSIDE-THE-EXTRACTED-HEADER` — a fourth copy.
      * `D-COMMENT-POSITIONAL-CITATIONS-IN-SRC-AND-TESTS` — the never-cite-a-line-number rule reaches
        `.plans/**` and `.claude/**` through `plan_citations_guard` and NOTHING checks `src/` or `tests/`;
        ✔3 of 4 sampled in one file were already pointing at the wrong code.
      * `D-GATE-BALANCE-EXEMPTS-A-DISCLOSED-OPENING-BUT-NOT-A-BOOKKEEPING-CLOSURE` — ⚠ read its row before
        touching it: the 6 stale closure glyphs it carries were deliberately NOT repaired this cycle,
        because repairing them while the gate was red is motivated measurement however correct each is.
      * ★★★ `D-TEST-INTEGRATED-RUNNER-WALKS-EVERY-EXAMPLE-IN-ONE-THREAD` — **OPERATOR-INSTRUCTED
        2026-08-21, verbatim:** *"please create an anchor to paralelize integrated_tests + report each
        integrated test item as pass or fail as a sub item of integrated tests unit (our runner to do
        that)"*. **TWO required properties: the examples run CONCURRENTLY, and EVERY example reports its
        OWN pass/fail as a sub-item — one aggregate verdict is not acceptable.** Today the corpus has TWO
        runners and only one does either: `tests/examples/` is ~450 separate ctest entries,
        `integrated_tests` is ONE entry walking every example in a single thread. ✔423 s on the VPS,
        3,317 s on macOS, 566 s on Windows — the floor under every `-j` number this project quotes, and a
        red example names only the aggregate.
        ⚠ **Read the row before sizing — it CORRECTS ITSELF.** Its first draft rejected per-example ctest
        entries as "a second copy of the corpus walk in CMake"; ✔re-measured, `tests/examples/CMakeLists.txt`
        ALREADY does that walk (`GLOB_RECURSE` + `foreach`) and is where its 450 entries come from. The
        objection was to HAND-LISTING, not to discovery. ⇒ recommended shape: reuse that discovery, one
        entry per example, which satisfies BOTH properties at once. The real work is the prerequisites:
        per-ENTRY (not per-run) scratch identity, a race-free `dependsOn` prerequisite cache, and the
        `CwdGuard` chdir — which is safe one-process-per-entry and would NOT be under a thread pool.
        ⓘ ✔`integrated_tests` is NOT a gtest binary (plain `main()`), and this repo uses
        `gtest_discover_tests` nowhere — so sub-items cannot come from gtest discovery as things stand.
      * `D-GATE-ANCHOR-BALANCE-SELFTEST-FIXTURES-ARE-ANCHOR-SHAPED` ·
        `D-GATE-WRAPPED-CITATION-WITH-LEADING-HYPHEN-CONTINUATION-UNRECOVERED` (⚠ the second is
        DELIBERATELY NOT BUILT — it has no failing case and a known false-positive surface).
      ⚠ **TWO MORE ROWS P23 OPENED ARE NOT PICKABLE, AND THAT IS SAID RATHER THAN SILENTLY SKIPPED:**
      `D-CYCLE-LANE-SOURCE-TREE-IS-SHARED-SO-ONE-LANE-CAN-BLOCK-ANOTHERS-BUILD` is **§B — OPERATOR DECISION**
      (per-lane worktrees vs. the shared tree; bring it AS a §B, do not decide it in a lane), and
      `D-CONFIG-GRAMMAR-ISA-AND-IDENTIFIERCLASS-BELONG-IN-THE-LANGUAGE-BLOCK` is **TRIGGER-GATED**.
      ⚠ **AND ONE IS NOT THIS CYCLE'S DEBT AT ALL:** `D-HARNESS-VPS-SSH-PS1-DOES-NOT-EXPAND-HOME` is
      🔵 **DISCLOSED PRE-EXISTING** — surfaced by lane Q, exempt from the net, and real regardless.
- [x] **P26 — OPERATOR-INSERTED, not from this list: the three red CI legs. ✅ DONE 2026-08-22.**
      Argument: *"CI failed for linux-clang-asan, macos and windows"*. Three legs, three different
      causes; detail in §0.00000000000000000000000000000. SEVEN rows BORN CLOSED, two 🔵 DISCLOSED
      (both checkable in `3ce4e336`), net +0 after the exemption. ⚠ The two disclosed rows are the
      only judgement call in the cycle and are flagged for operator veto — if either reads as debt this
      cycle CREATED, it becomes P27's first work.
- [x] **P27 — OPERATOR-INSERTED: the two CI legs still red after P26. ✅ DONE 2026-08-22.**
      Detail in §0.000000000000000000000000000000. Two rows BORN CLOSED, one CLOSED
      ([[D-CI-WINDOWS-CTEST-COST-IS-UNMEASURED]], answered by the first Windows ctest run that ever
      happened) and one OPENED and sized rather than absorbed, net ±0. Both defects were instruments that were correct about their subject and
      wrong about their environment.
- [x] **P28 — THE REST OF THE P25 BATCH, plus the branch's HIGH row, plus the operator's VR ruling. ✅ DONE 2026-08-23.**
      Thirteen lanes. Balance **1036 → 1022, net −14** (closed 22, opened 8 — 6 created, 2 disclosed,
      7 bookkeeping-only). Detail in §0.0000000000000000000000000000000.
      ★★ The pick had to resolve a **contradiction inside this very queue**: the entry below made
      `D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS` P26's mandatory first item, while the
      entry above it said ⛔ MUST-NOT-BE-PICKED. Re-derived from the REGISTRY, reported rather than
      chosen — and the **operator then ruled the gate itself was the error**.
      ⚠ Two rows the operator's ruling settled are recorded so they are not re-litigated: the ⛔ stays
      on the ALLOCATOR (not the operand), and its trigger is now known to have been **TRUE all along**
      with an instrument that could not see it.
- [x] **P29 — THE ROWS P28 OPENED, FIRST, per the standing order refined 2026-08-20.** Re-derive each
      from the REGISTRY at the pick; this list is a pointer, not a verdict.
      1. **`D-ANCHOR-ID-WRAPPED-ACROSS-A-LINE-BREAK-IS-INVISIBLE-TO-EVERY-GREP`** — ✔**239 wrap sites
         across 126 governed files**, each a REGISTERED id split across a line break and therefore
         invisible to every grep AND to `check-anchor-balance`. ★ Its own remedy is ORDERED: **build
         the detector as a ratcheted arm FIRST, then un-wrap in a quiet tree.** P28 reversed that
         order and then added a 240th instance, which the step-10 audit caught.
      2. **`D-ASM-MEMORY-CONSTRAINT-OUTPUT-FORM-NOT-REALIZED`** — ⚠ its gate was **WITHDRAWN** by the
         P28 audit fold: gcc compiles `"=m"` on both shipped targets, so bar §A.3b makes it
         **REQUIRED**, and a required capability with no blocker is queued work, not a trigger.
      3. **`D-HARNESS-EXAMPLE-RUNNERS-ALWAYS-COMPILE-AN-ABSOLUTE-SOURCE-PATH`** — the reason a HIGH
         user-facing defect shipped behind a green 1539-test gate. ⚠ The audit judged its blocker
         OVERSIZED: it blocks on the maximal fix (moving the cwd for ~580 examples) and never costed
         the additive one (ONE example exercising the bare-relative form).
      4. **`D-TEST-WALL-CLOCK-LITERAL-INVENTORY-IS-DEBT`** (48 literals, 11 files) and
         **`D-PLANS-GATED-ROWS-NAME-NO-OPENER`** (59 rows) — both ratcheted burn-downs.
      ⛔ **NOT PICKABLE, said rather than silently skipped:** `D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS`
      (⛔ MUST-NOT-BUILD; **its witness now EXISTS** — the mutant-N1 disassembly — so the operator's
      §3 step 4 condition is met and the arc awaits their word), `D-TARGET-NO-CROSS-CLASS-MOVE-VERB`
      and `D-TARGET-ARM64-W-CONSTRAINT-BINDS-A-CLASS-NO-C-VALUE-EVER-LIVES-IN` (both **§B**, and the
      lane that found the first recommends deciding them **together with the allocator arc**, since
      an aliasing-aware allocator needs the same verb).
      ✅ **DONE 2026-08-24 — WITH ONE ADDRESSABLE ROW LEFT OPEN, ROLLED INTO P30 PER THE STANDING
      ORDER REFINED 2026-08-20.** **FOUR of the five** addressable P28 openings are CLOSED;
      `D-PLANS-GATED-ROWS-NAME-NO-OPENER` is NOT, and it is item 1 of the P30 entry below.
      Balance at the lock: 1022 → 1022, net +0; gate 1580/1580 — ⚠ re-measure with
      `python scripts/check-anchor-balance/check-anchor-balance.py` rather than re-quoting this line.
      Detail in §0.00000000000000000000000000000000.
- [ ] **P30 — THE ROW P29 LEFT OPEN, THEN THE ROWS P29 OPENED, per the standing order refined
      2026-08-20.** P29 opened **2 created** rows and 3 disclosed **and left one of its own queued
      rows open**; re-derive each from the REGISTRY at the pick — this list is a pointer, not a
      verdict.
      1. ★★ **`D-PLANS-GATED-ROWS-NAME-NO-OPENER` — THE ADDRESSABLE P28 ROW P29 DID NOT CLOSE, AND
         THE ONE WHOSE DEBT IT GREW.** ✔MEASURED, both predicates over the same tree: the base-ref
         `is_gated` finds **30** openerless gated rows, the corrected one **70**
         (`python scripts/check-anchor-balance/check-anchor-balance.py` prints the live figure every
         run). The jump is the instrument finally seeing what the rule always condemned, not debt
         P29 created. ⚠ The row still quotes P28's `59`, measured at `cf27fe8b` under the old
         predicate — re-measure before acting on it. **Blocker (NAMED, and it is the row's own):**
         naming an opener is a judgement about each row's subject, so a lane that guessed 70 openers
         would be manufacturing dependencies. ⇒ burn it down in ratcheted batches, or reclassify the
         rows that no row can ever open as NEGATIVE RESULTS, which is what the REFUTED-DESIGN rows
         already do correctly.
      2. ⛔ **`D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE` — §B, NOT PICKABLE without an
         operator word.** Both arms are sized in the row. ⚠ Option (B) is NOT the cheap arm:
         diagnosing the divergence needs the same per-target natural-width declaration that FIXING
         it needs, and (B) can never make `char`/`short` work on aarch64 at all. ⚠ The row's blast
         radius is a MEASUREMENT, not a constant: re-run
         `clang-19 --target=<t> -fsyntax-only -Wasm-operand-widths` before quoting it — a second
         example (`c_inline_asm_memory_arithmetic`, 19 aarch64 references) already diverges beyond
         the one file the row enumerates.
      3. ⛔ **`D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES` — §B, the SECOND row P29
         created, named by its own independent step-10 audit.** gas assembles `mov $0x10000, %cx`
         and `mov $-32769, %cx` (the second with NO diagnostic at all); DSS refuses both. The two
         rules this project lives by point in opposite directions here — §A.3b says match a working
         reference, fail-loud says never silently narrow — so **no lane may resolve it**. ⓘ Whichever
         arm is taken, the test comment must stop calling DSS's own window a conformance fact.
      4. **`D-GATE-FOUR-PYTHON-PRIMARIES-REACH-UTF-8-TOO-LATE-OR-NEVER`** — 🔵 disclosed;
         `check-retyped-closed-sets` and `check-shell-portability` reconfigure only inside `main()`,
         `refresh_landing_log` and `sqlite-runtime-bench` never. Sized: the same five-line block each,
         plus deleting the inventory entry. Latent today, not live.
      5. **`D-GATE-WALL-CLOCK-GUARD-CMAKE-COMMENT-DESCRIBES-A-RATCHET-THAT-IS-EMPTY`** — 🔵
         the root `CMakeLists.txt` comment is stale three ways (48 literals → 0; "the inventory is
         DEBT" → empty; "23 matcher arms" → 35). Replacement text is in the row.
      6. **`D-PLANS-OPT7-INLINE-LEGALITY-GATE-ROW-DECLARES-NO-TRIGGER-OF-ITS-OWN`** — 🔵 one
         cell (`Trigger: none`, the escape this cycle built). ⚠ An instrument-side heuristic was
         considered and REFUSED: it fixes one of the row's two hits and would exonerate genuine gates.
      ★★ **THEN THE CENSUS BACKLOG, which is the real work and is already enumerated:**
      **16 rows needing an OPERATOR DECISION** and **16 INCONCLUSIVE**, both listed by the gated-row
      census. ⚠ The class-4 list is mostly SHIP decisions (an ILP32 target, a big-endian target,
      static Mach-O, a second language) — including
      `D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS`, **a MUST-NOT-BUILD ruling standing over
      a trigger the row itself records as ALREADY TRUE.**
      ⚠ **The openerless-gated-row population is item 1 above and is NOT restated here** — a figure
      written twice is a figure that goes stale in one place first. Read it from
      `python scripts/check-anchor-balance/check-anchor-balance.py`, which prints it every run.
      ⚠ **A STRUCTURAL GAP IN THE OPERATOR'S OWN RULE, found by the census and not patched around:**
      five rows wait on something schedulable that has **no ROW** — the OPT5 LIR address-mode phase,
      the separate-compilation driver, Plan-16 CS1, the `gui` profile. Plans 16/22/27 schedule these as
      PHASES and declare no anchor rows, so *"name the ROW"* cannot be satisfied without minting
      placeholders. Classified (4) and the DECISION named instead.
- [ ] **P30+ — THE REST OF WHAT P21 AND P22 FOUND, in registry order.** Operator instruction 2026-08-20: *"start a
      /loop using /dss-cycle each iteration to address ALL anchors found in _handoff §0."* Re-derive each pick
      from the REGISTRY, never from this list.
- [x] **OPERATOR CALL — ✅ FILED 2026-08-19: <https://sqlite.org/bugs/forumpost/97cd29ca44624113c73b30f5d2504729e6ffc5c5ebcba137078ea1a868cd97c9>.**
      The `%p` half only, which is the half that warranted it. ✔The toolchain table is what makes it
      compiler-independent: `sprintf("ptr:%p")` yields `ptr:0000007BD12FFB50` on **MSVC**, `ptr:00007ffffe2ffedc`
      on **mingw** (`ptr:00007FFFFE2FFEDC` with ANSI stdio off) and `ptr:00007FFFFF924B88` on **DSS**, against
      glibc's `ptr:0x55f10ad6f3a8` — so **not one Windows C runtime** emits the `0x` that
      `test/scanstatus2.test:268` rebuilds, while `src/test1.c:78` keys the registry with `%p`. Framed as a
      TEST-HARNESS defect; nothing in the library is implicated.
      ⚠ **The pe64 corpus leg STAYS RED at `scanstatus2-5.1` until upstream acts** — the standing rule forbids
      patching the staged tree or excluding the file, and a filed report does not change that. The redness is now
      named, root-caused, non-DSS, and reported.
      ⓘ **(b) the `fileio.c`/`windirent.h` `_MSC_VER` interlock was RETIRED, not filed** — ✔`cl` compiles that TU
      rc=0 because upstream ships Windows through `Makefile.msc`; only the mingw-on-Windows path breaks, a
      configuration upstream never claims. A reference works ⇒ by bar §A.3b DSS working is REQUIRED.

**ALSO IN BUCKET 1, NOT YET SELECTED** (offer them when the four above are done):
`D-CONFIG-ASM-TEMPLATE-CAPABILITY-NO-LONGER-OPTIONAL-FOR-A-SHARED-SURFACE-IMPORTER` (trigger FIRED) ·
`D-ASM-ARM64-GAS-SURFACE-INCOMPLETE` (⚠ **re-measure first** — it predates the CFI producer landing).
**BUCKET 2 (production errors) IS EMPTY IN THESE TWO FAMILIES** — every asm/AP5 row is trigger-gated, fail-loud
or LOW. The live user-facing one on this branch is `D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED` (HIGH: `S*`
diagnostics are never remapped through `#line`).
⛔ **BUCKET 4 — DO NOT PICK, and say so rather than silently skipping:** §B-gated
`D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` + `D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL`;
trigger NOT fired `D-ASM-TARGET-DECLARES-NO-BYTE-ORDER`, `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED`,
`D-ASM-SYSTEM-REGISTER-AS-ENCODED-DATA-UNMODELLED`, `D-CSUBSET-BARE-ASM-ACCEPTANCE-IS-UNCONDITIONAL`; SUSPECT
`D-MIR-SEH-FILTER-CLONER-HAS-NO-INLINE-ASM-ARM` (verify or discharge, never patch on suspicion); LOW
`D-ASM-TEMPLATE-IS-LEXED-TWICE`, `D-CSUBSET-ASM-LABEL-ON-SYNTHESIZED-SHIM-SYMBOL`,
`D-ASM-AARCH64-FRAME-OFFSET-BEYOND-SCALED-IMM12`, `D-ASM-AARCH64-FRAME-OFFSET-BEYOND-2GIB`.
ⓘ Two rows a family-grep catches that are NOT defects: `D-OPT-REBUILD-POLICY-NEUTERED-STATE-HOOK` is a 🟢
SHIPPED design record, and `D-TEST-MACOS-LEG-EMSDK-PROFILE-REPLACES-PATH-HIDING-HOMEBREW` is a 🔵 DISCLOSED
pre-existing environment fact.

---

### ★★★ THE §B RULING THAT SET P25's LIR SCOPE — OPERATOR 2026-08-21, NOT TO BE RE-LITIGATED

**DECISION: OPTION 3** — the total arg-side row map and the no-pool refusal land in P25; the
sub-register-aware allocator is P26 entry 1. The other eight rows stay in P25.

★★★ **THE ARGUMENT THAT SETTLED IT, AND IT CORRECTED THE BRIEF THAT PROPOSED THE FORK.** The options
were presented with "Option 2 makes `w` a diagnostic, and gcc supports it, so by §A.3b this is not the
end state." Operator, verbatim: *"DSS DOES NOT SUPPORT `w` TODAY. … That is not a capability. It is a
SILENT MISCOMPILE WEARING A CAPABILITY'S CLOTHES. So the choice is not 'capability vs refusal'. It is
SILENT-WRONG vs LOUD-REFUSED, and the bar answers that with no discretion at all."* ★ **Option 2
removes a FALSE capability; it does not create an under-capability** — the under-capability against gcc
already existed, for as long as the else-branch has. §A.3b bites on the END STATE, which is why P26
entry 1 is mandatory rather than aspirational.

★★ **AND IT IS NOT A NEW DESIGN — IT IS THE RETURN SIDE'S PROVEN TWIN, ONE CYCLE LATER.**
`returnRegisterForClass`'s own banner records the IDENTICAL defect on the sibling function in the same
words: `returnVrs` *"was declared … populated by the loader … and this function never read it"*, the VR
result took the GPR ELSE branch, wrong-file capture, no diagnostic. ⇒ **build the same answer**: an
arg-side row table mirroring `kReturnPoolRows`, with the same anti-padding `static_assert` (a short
initializer list value-initializes a null pointer-to-member and compiles clean — that mutant was
already measured on the return side) and the accepted set RENDERED FROM THE ROWS. **Do not mint a
second pattern beside the one P23 built and pinned.**

**THE FOUR BINDING CONDITIONS:**
1. ★★ **THE COUNTER IDENTITY IS THE ONE THING THIS MUST NOT GET WRONG.** The return table can index
   each class's pool independently; **the arg side cannot** — AAPCS64 has ONE NSRN, so a v-view and a
   d-view arg draw from the SAME cursor. The table must express **which counter a row draws from,
   distinctly from which pool it names**. Get it right now and P26 touches only the allocator; get it
   wrong and P26 REWRITES this table, which would make P25 a down-payment on nothing.
2. ★ **"NO POOL" AND "POOL EXHAUSTED" ARE DIFFERENT FACTS AND MUST NOT SHARE A DIAGNOSTIC.** Once
   `argVrs` is read, an arm64 VR arg hits `index >= pool.size()` against an EMPTY pool and falls into
   `L_StackPassedArgUnsupported`, which would say the cc *"has only 0 VR arg-passing registers"* and
   point at stack passing — naming the wrong fact. The cc declares **no VR arg pool at all**: a config
   statement about what the target declared allocatable, not a capacity overflow the stack could
   absorb. Its own refusal names the class, the cc, and that fact — and that refusal is what pays for
   `D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME`.
3. ★★★ **P26 ENTRY 1, NOT "P26's HEADLINE"** — see the queue above. *"'Headline' is a softer word than
   the rule allows, and the difference is exactly how a queued row becomes a permanent one."*
4. ★★ **THE `w` PIN IS INVERTED, NEVER DELETED**, and it names the anchor P26 must flip back.
   ⚠ **THE CONDITION AS WRITTEN ASSUMED A TEST THAT DOES NOT EXIST, and that is recorded rather than
   quietly satisfied:** ✔MEASURED — the only `w` witness in the tree is
   `InlineAsmConstraintParse.OneLetterTwoTargetsIsAConfigAnswerNotACodeBranch`, which asserts the
   LETTER IS DECLARED BY CONFIG (`arm->asmConstraint("w") != nullptr`) and stays TRUE under Option 2.
   **There is no end-to-end `w` codegen test to invert.** ⇒ P25's no-pool refusal pin becomes the FIRST
   end-to-end `w` witness the tree has ever had, and it carries the marker naming what P26 restores.

### ✔ WHAT THE §B's OWN MEASUREMENTS FOUND — INCLUDING TWO THINGS THAT REFUTE THE BRIEF

- ★★ **THE ROW'S DISASSEMBLY IS CORRECTED, NOT RE-QUOTED.** The row records `ldur q1` and *"the
  `double` lands in d1/q1 where AAPCS64 requires d0"*. ✔RE-MEASURED at `c6ef80e4`, release,
  `elf64-aarch64-linux`: it is **q0**, and **BOTH arguments are wrong, not one** —
  `ldur q0, [sp,#24]` reloads the `"w"` output, then `fmov d0, d15` **destroys it** (d0 is the low
  half of q0) while placing the second argument, and **d1 is never set at all**. rc=0, no diagnostic.
  ✔CONTROL, `aarch64-linux-gnu-gcc -O2`, identical source: `fmov d1, #1.0` / `nop` / `b sink` — the
  `"w"` output allocated straight into d0, `1.0` into d1, tail call.
- ⚠ **"NOTHING READS `argVrs`" IS FALSE.** ✔MEASURED: the AAPCS64 binary128 boundary in
  `mir_to_lir.cpp` reads `cc->argVrs` and `cc->returnVrs` at several sites, with a cursor **literally
  named `nsrn`**. The field is not inert; it has a live consumer whose counter this cycle must join
  rather than duplicate — *a fact with an owner does not get a second owner*.
- ⏳ **AND THAT CONSUMER'S CURSOR COUNTS ONLY F128 ARGS** — a `double` never advances it — which
  predicts exactly the collision `TargetCallingConvention::argVrs`'s own comment says is impossible
  (*"an F64 and an F128 sharing a signature never collide by ordinal"*). ✔**NOT REPRODUCED: two
  source shapes (`sink(double, long double)` by constant and by parameter) BOTH refuse upstream with
  `L_UnsupportedLoweringForOpcode`**, so the door is closed and this is **LATENT, NOT LIVE**. Filed as
  a SUSPECT ⇒ `D-LIR-F128-ARG-NSRN-CURSOR-COUNTS-ONLY-F128-ARGS`. ★ It was predicted as live and the
  measurement refuted it; the refutation is the deliverable.
- ✔ **THE BLAST-RADIUS PREMISE IS VERIFIED, NOT RELAYED** — it is what makes Option 3 tolerable.
  **INSTRUMENT:** both corpus runners enumerate one glob, `DSS_EXAMPLE_MANIFESTS` =
  `examples/*/*/expected.json` (**613** examples, hoisted to the root `CMakeLists.txt` in P24); a
  `"[=+]?w"` constraint search over `.c/.h/.s/.S/.json` under `examples/` returns **0 files**, and the
  same search over the sqlite real-example tree returns **0**. No corpus example exercises `"w"`.

---

## 0.00000000000000000000000000000000000000000 ★★★ CYCLE P44 — NINE LANES CLOSED TWENTY-ONE ANCHORS, THE GATE CAN ONLY SEE NINETEEN OF THEM, AND THE ONE THAT MATTERED MOST WAS FOUND BY MEASURING THE COMPOSITION OF TWO GREEN LANES

**Balance at this commit, re-measured rather than re-quoted:**
`python scripts/check-anchor-balance/check-anchor-balance.py --base b1684e7f` ⇒
**closed 19, opened 0, net −19**; registry **556 → 537**. Per bucket via the gate's own
`scan_document`: **production 349 OPEN / harness 188 OPEN / sum 537**, which equals the gate's
registry figure — that sum is the cross-check, and it is the only thing that catches a
mis-bucketed or double-counted row.

⚠⚠ **AND THE GATE CANNOT SEE TWO OF THE TWENTY-ONE ANCHORS THIS CYCLE CLOSED, WHICH IS
WHY THE HEADLINE SAYS BOTH NUMBERS.** `check-anchor-balance` counts ROWS BY NAME across the base
and the tip, so a row that did not exist at `b1684e7f` cannot be counted as newly closed no matter
what it records. Two rows are in exactly that position: lane `a` MINTED
`D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN` mid-cycle and lane `h` closed it,
and lane `i` wrote `D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS` BORN CLOSED.
✔MEASURED both ways: against `b1684e7f` the gate reads **closed 19, opened 0**; against the
cycle's own first commit `6c6d6077` it reads **closed 1, opened 0**, and 19 + 1 ≠ 21 because
the born-closed row is invisible from BOTH bases. ★ The blindness is the RIGHT behaviour for
the question the gate is asked (*"did this cycle leave more open than it found?"*) and the WRONG
number for *"what did this cycle fix?"* — so a cycle report owes both, and a lane whose whole
output is a born-closed row reads as **zero** in the only figure the ruling watches.
`scripts/check-anchor-registry/check-anchor-registry.sh` ⇒ **rc=0**, 0 cell-width violations,
15 self-test arms proving the guard can fail. **Both gates were run**: a green balance is not
evidence that nothing was opened.

`D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN` — minted by lane `a` when
closing a refusal exposed what the refusal had been hiding — **is CLOSED at this commit by lane
`h`**, in the cycle that opened it. Nothing this cycle opened is still open.

### ★★★ THE HEADLINE: TWO LANES SHIPPED TWO CORRECT HALVES, BOTH CORRECTLY DECLINED TO CLOSE THE ROW, AND THE COMPOSED BINARY WAS STILL BROKEN

`D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED` had two halves in two lanes. Lane `f` shipped the
driver-side `-I` acceptance; lane `g` root-caused the resolver (undefined behaviour — a reference
bound to the `native()` of a by-value temporary — and measured `//wsl.localhost` answering YES
**141/200** by reference vs **0/200** by value). Each verified its own worktree. Each refused to
close a row whose other half it had never compiled. **Both judgements were right.**

The composed binary — the first artefact ever to hold both — resolved a UNC `-I` header **0/30 for
both slash spellings**, with a byte-identical local-absolute control at **30/30**.

**The cause sat one tier outside everything either lane owned.** `applyIncludeDirs` ran every `-I`
value through a bare `fs::absolute`. ✔MEASURED, one path object, `.string()`:

| property | value |
|---|---|
| `is_absolute()` | **false** |
| `has_root_name()` / `root_name()` | **false** / **empty** |
| `absolute()` | **`C:\wsl.localhost\Ubuntu\home\rafael\p44_unc_inc`** |
| `exists(input)` / `exists(absolute())` | **true** / **false** |
| `weakly_canonical` | **`<error> No such file or directory`** |

It does not prepend the cwd. It **re-roots a path naming another machine onto the local drive, and
reports no error.** Every tier below the driver was then handed a directory that cannot exist and
answered correctly about it — which is exactly why all of `tests/core` was green over a live defect.

⇒ ★★★ **A LANE'S GREEN IS A STATEMENT ABOUT ITS OWN TREE. THE COMPOSITION IS A NEW OBJECT NOBODY
HAS MEASURED**, and where two lanes deliberately split one row, the seam between them is the one
place neither lane's gate can reach. Measure the composed tree against the row's own closing
predicate before closing it — not the sum of two reports.

### ★★★ THREE FINDINGS FROM THAT FIX, EACH GENERAL

**(1) `fs::absolute` and `weakly_canonical` fail in OPPOSITE directions, and only one needs a guard.**
✔MEASURED on the same reachable UNC path: `weakly_canonical` **errors**, so every call site's
existing keep-the-original-on-error arm was already correct — `config_path_walk`'s executable
resolution is **not** a further instance, and that was measured rather than assumed (a sibling's
report covered the UNREACHABLE case, which behaves differently). `fs::absolute` **succeeds wrongly**,
which no error arm can catch. **A helper that fails toward an ERROR is defended by the caller's
existing error arm; one that fails toward a WRONG ANSWER is naked.** That distinction is the right
sort key for a call-site audit and cut a ~25-file list to a handful.

**(2) ONE PATH, THREE TRANSFORMS, EACH REMOVING ONE SEPARATOR — and a partial fix reads exactly like
a complete one.** The artifact-written report ran `absolute` → `lexically_normal` → `generic_string`.
Fixing only `absolute` moved the reported path from `C:\host\share\…` to `/host/share/…`: still
wrong, still naming the local drive, **now wrong for a different reason**, and both spellings are
equally absent from disk. It was caught only because the mutation arm that reverted that fix
reddened **NOTHING** — the fix had shipped unpinned — and the pin written in response then **failed
at baseline**, which is how collapses 2 and 3 were found at all.

**(3) THE DISCRIMINATOR IS THE SEPARATOR RUN, NOT `isRootedPath` AND NOT `is_absolute()`.** A run of
ONE (`/foo`) genuinely is a location on the current drive and MUST still be made absolute; reusing
the row's own exported `isRootedPath` here would have been **a regression dressed as a fix**. Pinned
by `AbsoluteStillResolvesSingleSeparatorAndRelative`. `core::absoluteKeepingRoot` and
`core::normalizeKeepingRoot` now sit beside `core::genericSpelling`, three transforms sharing ONE
invariant and ONE private discriminator (`leadingSeparatorRun >= 2`).

**RED-ON-DISABLE, five arms, VALID.** Baseline 45 OK / 0 FAILED / **0 SKIPPED** (no vacuous arm on
this leg). Every arm: build rc=0, per-TU OBJECT md5 MOVED and RETURNED, run through `ctest`, failing
NAMES read, `core/test_header_name_matching` carried as a CONTROL and green (16 arms) throughout.
m1 (substrate guard neutered) → **3 RED**; m2 (driver reverted) → **1 RED, the driver pin ALONE —
so the resolver-tier pins do NOT cover a driver-tier wiring defect**; m3a/m3b/m3c (each of the three
report transforms reverted independently) → **1 RED each, the same pin**, proving all three are
independently load-bearing.

### THE FOUR-LEG GATE AT THIS COMMIT — measured on THIS tree, not re-quoted from the previous set

| leg | result |
|---|---|
| Windows (root host) | **1793/1793**, all 20 repo guards — and the 20 were **re-run alone after the last `.plans/` edit**, since the full pass predated it |
| WSL x86_64 | **1773/1773**, `run-gate` OK on a tool-emitted witness, **+ emulator witness: arm64 artifacts were spawned and RAN** |
| arm64 VPS (native) | **1773/1773**, `run-gate` OK |
| macOS (native arm64) | **1773/1773**, `run-gate` OK |

**1773 = 1793 − the 20 root-host-only repo guards, on every leg, no residual.** The count rose
1790 → 1793 with lane `h`'s three new tests. ★ **The macOS leg is the one that tests lane
`h`'s least-certain claim** — that Mach-O runs its static initializers through the same
`entryTrampoline` as every other exec flavor rather than needing a loader-recognized section —
because `examples/c/gnu_constructor_attribute_runs` declares a `macho64-arm64-darwin-exec` target
with `runOn: ["darwin"]`. It is not a formality on that leg. ⓘ The macOS leg ran
~27 min of `ctest` against the other legs' ~13; that was **probed live** (`ctest` at 27:08 elapsed
with a freshly-spawned `dsscp` child, load 2.33) rather than assumed to be a hang — a silent leg is a
question, not a verdict.

Residue scan on the composed tree: **42 marker candidates across 146 changed files, ZERO live** —
every hit under `src/` is a comment or a fixture (the preprocessor discussing `#if 0`, English
"mutate the top", and one `MUTANT` token inside a `$comment` field where a lane recorded its own
red-on-disable measurement).

### ★★★ LANE `h`: THE ATTRIBUTE RUNS — AND THE BRIEF'S SECOND HALF WAS REFUTED BY READING THE EMITTED IMAGE

`__attribute__((constructor))` / `((destructor))` are honoured end to end. ✔MEASURED: the row's
own program exits **42** (it exited **0**, with a warning naming the attribute it had dropped) on
`pe64-x86_64-windows-exec` natively, `elf64-x86_64-linux-exec` under WSL and
`elf64-aarch64-linux-exec` under `qemu-aarch64`, at `--config=debug` AND `--config=release`. A
six-initializer probe gives `c101 c102 cBARE | MAIN | dBARE d102 d101` on every leg and both
configs — byte-identical to gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0, each probed
SEPARATELY.

★★ **THE BRIEF SAID TO EMIT `.init_array` / `__mod_init_func` / `.CRT$XCU`. THE LANE
REFUTED THAT BY MEASUREMENT RATHER THAN SKIPPING IT.** ✔`readelf -d` on a DSS
`elf64-x86_64-linux-exec` artifact shows an eleven-entry dynamic section with **no `DT_INIT_ARRAY`
and no `DT_INIT`** — and ld.so walks the TAG, never the section; PE never links the UCRT
startup object, so `_initterm` never runs. Those sections are how a program tells a **C runtime**
what to run before main, and **DSS links no crt: `injectEntryTrampoline` synthesises `_start`, and
that entry IS the runtime.** On every image DSS produces and starts, such a section would be bytes
nothing reads. What shipped instead is the axis that is real and per-format:
`staticInitializers.runner`, declared beside `processExit` / `processArgs` / `entryVerbs` —
every exec flavor says `entryTrampoline`; a shared library, a relocatable object and a static
library declare NOTHING, so the linker REFUSES such a program **by name** rather than building one
whose initializers never run.
⚠ **This contradicts the brief I wrote, including its claim about Mach-O**, which I had said
would need `imageLoader`; it uses `entryTrampoline` like the others. **THREE OF THE FOUR LANES THAT
MEASURED A PREMISE OF MINE THIS CYCLE REFUTED IT** (`e` on the config tier, `d` withdrawing its own
residual, `h` here). A brief's premise is a HYPOTHESIS carrying its author's measurement and no
more; relaying one inherits that, and this one was wrong the day it was written rather than decayed.

★★ **TWO REAL BUGS, BOTH FOUND BY REVIEW AND NEITHER BY A TEST.** (1) DCE deleted every
`static` constructor at `--config=release` — invisible in debug, whose pipeline is `Identity`,
which is why the corpus example's release arm is load-bearing and not a formality. (2) The
before-entry calls **clobbered the caller-saved argument registers**, so `main(int argc, char
**argv)` received garbage: argc/argv are materialised into the convention's ARGUMENT registers
before the ABI prologue, and a `call` inserted between that and `call main` destroys them.

⚠⚠ **AND THE WITNESS FOR THAT SECOND FIX WAS VACUOUS THREE TIMES, EVERY ONE CAUGHT BY
RUNNING THE MUTANT RATHER THAN BY READING THE PIN.** (i) the example's constructors were pure
arithmetic and never touched an argument register, so argc survived the missing park by accident;
(ii) on Windows only the `pe64` arm runs, and PE fetches argv through **CRT accessor calls**, so the
park is INERT on the one leg observing it; (iii) the structural pin's bound `> 10` was cleared by the
mutant at 19, because deleting the RESTORE half leaves the SAVE half. Calibrated bound is now
`>= 25`, and the end-to-end statement is made on the leg that can make it: the example built for
`elf64-x86_64-linux-exec` exits 42 clean, **9** under the mutant (its own `argc != 1` code, firing by
name) and 42 on restore. **Six mutants, all REMOVE-direction, control green in every arm, object md5
moved AND returned on all five code arms.**

⚠ **THE MUTATION HARNESS ITSELF HAD THE ONE DEFECT SUCH A HARNESS MUST NEVER HAVE**, and it is
recorded because it is the INSTRUMENT: its first version restored by searching for the REPLACEMENT
text and asserting exactly one occurrence — the wrong question for a DELETION, since one arm's
replacement was the EMPTY STRING and counting an empty string over a file returns its LENGTH. The
guard tripped at 24512, the restore was SKIPPED, and `dce.cpp` was left MUTATED in the tree. Restore
is now a full-content snapshot with an md5 check on the way back, and the tree was verified clean.

### ★★ LANE `i`: THE PATH-SPELLING AUDIT — AND THREE OF THE WORST SITES WERE NOT THE FUNCTION THE ROW NAMED

The census that opened the row handed over 15 files; the audit found the class in **four more**, and
three of the worst instances were not `generic_string()` at all. **Eight sites were judged PROVABLY
SAFE and left untouched, each with its reason** (`lexically_relative()` results — ✔measured
leading-separator run **0** — plus `filename()` and `extension()`, none of which can carry a
root), and the rest converted. ★ The row named **three sites in two files the lane did not own**
and marked them OWNED AND PENDING rather than filing a follow-up; they were discharged in this
commit's pre-commit gate once lane `h` freed the files. `src/link/writer.cpp`'s `pathForDiag` was the
sharpest: **its two arms disagreed about which file they name** — the `try` arm's
`generic_string()` collapsed a UNC path's leading separator run to one (naming a path on the local
drive root), while the `catch` arm's `u8string()` kept the run and also kept native separators. One
path object, two spellings, and which one a user saw depended on whether narrowing happened to throw
for an unrelated reason. Both arms now go through the run-preserving transforms and differ in
ENCODING only, which is the one axis that fallback exists for.

✔**THE LIST IS EXHAUSTED RATHER THAN SHORTENED**, measured after the gate: **zero** bare
`fs::absolute` and **zero** `generic_string()` / `generic_u8string()` on a path that can carry a root
anywhere in `src/`. Of the fourteen remaining mentions, five are comments citing the rule, eight are
the sites this row judged safe, and one is `src/lsp/workspace_project.cpp` — a PRIOR sighting of
the same defect, handled locally and correctly: it detects the authority from `root_name()` precisely
BECAUSE the generic rendering had already eaten the run.

### THE OTHER LANES

| lane | outcome |
|---|---|
| `a` | 3 rows; refuses-what-a-reference-accepts 5 → 3. Minted the constructor row when closing a refusal exposed what it hid. |
| `b` | 3 preprocessor rows (token paste ×2, origin-offset validation). It ALSO measured the UNC row at **23/30** and explicitly declined both to close it and to ship a test — *"a test that passes four times in five is indistinguishable from a flaky harness, and a green run would be read as a close"*. ★ That judgement was correct and is the reason the eventual pin is honest: the row became pinnable only once lane `g` made the behaviour deterministic. |
| `c` | 4 rows; 26 identity branches → 0; refused closure-by-delegation. |
| `d` | 6 rows across 5 waves; wave 4 measured the COFF/Mach-O twins **immune** and withdrew its own residual. |
| `e` | `D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY` closed (a THIRD tier was hardcoding the severity, refuting the brief's premise); then `D-C23-REDECL-QUALIFIER-AXIS-HAS-THREE-UNCLAIMED-SOURCES` closed across all three sources. ⚠ It INTRODUCED the over-reach its own row forbade — a claim for every signature whose bits merely happened to be zero — and DSS's own shipped runtime shims stopped compiling. Caught by measurement, fixed with a producer-side gate. |
| `f` | `imagerel32` **withdrawn by measurement**; compile-error pin 6 → 3; `-I` acceptance shipped as a **warning**, because an error would put DSS ABOVE the union (gcc and MSVC are both silent rc=0 on every shape). |
| `g` | The UNC resolver UB, above. |

### ⚠ WHAT THIS CYCLE DID NOT DO, NAMED SO IT IS NOT MISTAKEN FOR DONE

- **`#pragma once` IS REFUSED BY DSS** (`P_PreprocessorPragma`, exit 1), found while building the UNC
  fixture — every arm went red, the control included. The owning row `D-PP-PRAGMA-RECOGNIZED-SEMANTICS`
  is OPEN, already concedes the bar makes it REQUIRED, and files it **"QUEUED WORK, low priority"**
  while describing the behaviour as *"currently have NO effect"*. ★★ **That description was already
  false when written**: a sibling row records the loud refusal since 2026-07-29, three weeks earlier.
  *"Has no effect"* reads as a minor gap; **"refuses the translation unit" means every real-world
  header using the commonest guard idiom fails to compile**, against an idiom gcc, clang and MSVC all
  accept. ⇒ **The stale sentence did not merely misdescribe the defect, it set its priority.** Not
  dispatched only because it contends with lanes `h` and `i` for `src/analysis/**`, `src/dss-config/**`
  and `src/core/substrate/path_identity.*`. ⚠ Re-date its premise too: *"honouring the pragma would be
  a guess"* was true when written, and `core::PathIdentity` landed 2026-08-18 precisely so that every
  spelling of one file reduces to one key — the machinery an include-once set needs **had already been
  built, by someone else, for another reason**.
  ★★★ **THE OPERATOR RULED ON IT 2026-08-28 AND THE RULING BOUNDS THE BAR ITSELF.**
  ✔MEASURED across all three references separately, one self-contained program per question:
  gcc, clang and MSVC all dedup `./h.h`, `sub/../h.h`, a symlink and a hard link — but on **two
  different files with byte-identical contents**, gcc DEDUPS (content-keyed) while clang and MSVC do
  NOT (identity-keyed). A mechanical reading of the disjunction picks gcc. **The operator chose
  IDENTITY-KEYED**: `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` decides whether a construct is
  ACCEPTED, and does **not** automatically decide what a program MEANS when the references disagree
  about the meaning. Content-keying does not merely accept more — it **silently omits text** when
  a macro is redefined between two `#include`s of byte-identical headers, which is the class the bar
  most abhors. DSS will deliberately refuse a program gcc compiles, with the cost recorded in the row
  so a later cycle does not "discover" the divergence and fix it back. **Dispatchable now: lanes `h`
  and `i` have landed and the file-set contention is gone.**
- **The wider path-spelling audit LANDED as lane `i`** — see its subsection above. ✔Zero
  residue in `src/` afterwards, measured rather than asserted.
- **sqlite `veryquick` / `speedtest1` not re-run.** Deferred to the cycle's closing gate rather than
  spent per commit: it costs hours and measures a tree that still changes with every landing set.
  ⚠ **This is the STANDING PR EXIT REGIME and PR #56 inherits it undischarged**: units + sqlite
  `veryquick` + `speedtest1` on FOUR legs, then the README — which still says *"THREE hosts"*
  and has **no arm64-VPS table**.

### ★★★ START HERE — the state at THIS COMMIT (the tip of `feature/c23-conformance-burndown-5`, PR #56), for a session with no context

**Worktrees:** none. Lanes `a`, `b`, `d`, `h`, `i` were removed after folding; `git worktree list`
should show only the repo itself. **PR #56 is open** on
`feature/c23-conformance-burndown-5`; both P44 commits are pushed.

**The next lane set, from `scripts/burndown-queue/burndown-queue.py` — four disjoint tiers so
they cannot contend.** ⚠ Re-derive every status from the registry before acting: a row's
status lives in its status cell, and this list will be stale the moment something lands.

1. ★★★ **`D-PP-PRAGMA-RECOGNIZED-SEMANTICS`** (preprocess tier) — **take this
   first, and it is READY TO BRIEF**: the measurement and the operator's ruling are both already
   IN the row. `#pragma once` does not merely lack effect, **DSS REFUSES the translation unit**
   (`P_PreprocessorPragma`, exit 1) against an idiom gcc, clang and MSVC all accept, so every
   real-world header using the commonest guard fails to compile. The dedup key is **RULED
   IDENTITY, not content** (operator 2026-08-28): `core::PathIdentity`, which landed 2026-08-18
   and is exactly the machinery needed. The full 7-case reference matrix and the stated cost of
   the deliberate divergence from gcc are in the row.
2. **`D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT`** (type-layout tier) — P0 RED, a **silent
   layout miscompile**, trigger fired, independent of `#pragma pack`. Ground truth is in the row.
3. **`D-CSUBSET-TYPEDEF-HEAD-DECORATION-TYPE-HIJACK`** (semantic/HIR tier) — P0 RED; the row
   exists to stop the tempting WRONG fix being re-proposed, so read it before designing.
4. **`D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE`** (asm tier) — a NAMED
   EXCEPTION carrying its own closing predicate; check whether the infrastructure it waits on now
   exists, per *"a buildable prerequisite is not a gate"*.

⚠ **All four may want `src/dss-config/sources/c.lang.json`.** The fold instrument REFUSES a
destination the main tree has drifted on, so a collision fails loud rather than silently reverting
a sibling — but tell each lane to keep its config edit minimal and to report the exact keys it
added.

⚠⚠ **THE ORCHESTRATOR'S CYCLE INSTRUMENTS LIVE IN A SESSION-SCOPED SCRATCHPAD AND DO NOT
SURVIVE A NEW SESSION.** They are at
`C:\Users\rafae\AppData\Local\Temp\claude\C--Source-DailySoftware-dss-code-prime\0da5ab3a-fae5-4d34-b834-a49c20f74be6\scratchpad\p44\`
and that directory still exists on disk — **read them from there rather than rewriting them.**
The ones that earned their keep: `porcelain.py` (`git status -z`, because git C-QUOTES a path with
spaces and this repo holds one), `seed_lane.py` + `fold_seeded.py` (a lane's contribution is
*its status set MINUS seeded paths whose md5 is unchanged*, and the fold REFUSES a destination the
main tree has drifted on), `apply_one_row.py` (replaces one row from a FILE, never retyped — a
retyped row can WRAP an anchor id, which does not fail: it goes invisible and mints a false one),
and `bucket_counts.py` (per-bucket OPEN split **through the gate's own `scan_document`**, because
a hand-rolled row scanner counted commented-out rows and reported 562 where the gate said 556).
★ **They are cycle machinery, they get rewritten every session, and `scripts/wsl-leg.sh`'s own
header records that exact waste happening three times in one session. Decide at the START of the
next cycle whether they are promoted into `scripts/`** — recorded as a decision to take, not
filed as a row nobody closes.

### ⓘ A HARNESS DEFECT FIXED THE MOMENT IT BLOCKED, per the standing ruling

Seeding lanes `h`/`i` died creating a directory named `".plans`. `git status --porcelain` **C-quotes**
a path that needs it, and this repository holds one — `.plans/23-full-c-plan - tbd.md` (spaces).
Three orchestrator instruments read the path as `line[3:]`. Fixed at the source with one `-z` helper
(NUL-separated, never quoted — so no quoting convention is left to reimplement; `core.quotePath=false`
would NOT have sufficed, as it governs non-ASCII bytes and not the quoting a space triggers).
✔**The repo's own scripts were checked BEFORE the fix and are NOT affected** — every one only counts
porcelain LINES, and a quoted path is still one line. No repo anchor is owed.
⚠ The failure mode worth remembering: on earlier seeds the naive parse did **not** error, it just
silently omitted that path.

## 0.0000000000000000000000000000000000000000 ★★★ CYCLE P43 — TWO RED CI LEGS, TWO DIFFERENT BLIND SPOTS, AND NEITHER WAS CLOSED BY LOOSENING ANYTHING

> Invoked on the operator's words: *"windows and linux gcc CI failed. address the root cause with no
> workarounds, 100% config driven, first class implementation and no follow ups"*. Both failures were
> on CI run **33156833090** (PR #55) at `73f74972`; `linux-arm64-gcc-release` was GREEN in that same
> run, which is what made both diagnoses possible.

### The Windows leg — a build break, in a compiler no local leg runs

✔**MEASURED.** `windows-msvc-release` failed at **Build**, not at Test:

```
src\link\..\link/object_format_schema.hpp: error C2487: 'addSectionRow':
    member of dll interface class may not be declared with dll interface
```

`ObjectFormatData` is `struct DSS_EXPORT`, and its member `addSectionRow` carried `DSS_EXPORT` too.
`src/core/export.hpp` expands that macro to `__declspec(dllexport)` under MSVC, to
`__attribute__((visibility("default")))` under GCC and Clang, and to nothing in a static build. **Only
MSVC calls the repetition an error.**

★★★ **THE STRUCTURAL FACT, WHICH IS THE ACTUAL DEFECT: the four-leg gate is Windows/MinGW-GCC, WSL
x86_64 GCC, qemu arm64 GCC and macOS Clang. MSVC exists ONLY in CI.** ✔The declaration landed in
cycle **P34 (`5085664a`)** and survived **eight cycles and a 1708/1708 local Windows gate** before a
fourteen-minute remote job saw it.

**What shipped:**
- The macro is deleted from the declaration — the enclosing class already exports every member — with
  the omission annotated at the site so it is not re-added.
- `scripts/check-export-macro-placement/` + the `export_macro_placement_guard` ctest entry: a **BAN**,
  not a ratchet, because the live population across `src/`, `tests/`, `integrated_tests/` and `libs/`
  is **zero** once the site is repaired, and there is no legitimate instance to grandfather.

⚠ **THE BOUNDARY WAS MEASURED AGAINST `cl`, ONE ARM PER SHAPE, NOT REASONED** — and it is the reason
the guard is usable at all. Inside a `struct DSS_EXPORT Outer`:

| shape | MSVC |
|---|---|
| member function | **error C2487** |
| static member function | **error C2487** |
| static member data | **error C2487** |
| nested class / nested struct | accepted |
| `friend` declaration | accepted |

⇒ **A guard keyed on *any* `DSS_EXPORT` inside an exported class would have refused NINE live sites**
— `PhaseTimers::Scope`, `TreeCursor::Bookmark`, `TokenStream::Bookmark`, `TreeBuilder::OpenScope`,
`TreeBuilder::Checkpoint`, `DiagnosticReporter::Snapshot`, `LexerModeStack::Snapshot`,
`SchemaWalker::Snapshot`, `CompilationUnit::PrivateTag` — **and would have been switched off the same
day.**

✔**VERIFIED BY EXECUTION, NOT BY READING.** `build/p43-msvc` configured with CI's own
`-G Ninja -DCMAKE_BUILD_TYPE=Release -DDSS_BUILD_TESTS=ON` under Visual Studio 18's `vcvars64`; the
`link` target built to completion (`MSVC_LINK_TARGET_OK`), **zero `error C` in the log**, and both
objects CI could not produce are on disk.

✔**RED-ON-DISABLE VALID.** The defect was re-applied to the live header; the guard's ctest form
refused it naming the file and the owning class; both control arms rc=0; the subject's md5 moved and
returned. ⓘ Its first run reported the wrong LINE (288 for a declaration near 1250) — the shared
comment stripper preserves newlines but **not length**, so a stripped offset must be counted in the
STRIPPED text. Repaired, and pinned by a self-test arm that puts a long comment block ahead of the
violation so a regression to raw-offset counting cannot pass.

### The linux-gcc leg — a count pinned with a clock

✔**MEASURED.** `analysis/preprocess/test_preprocess_no_rework` failed:

```
Expected: (ratio) < (0.85), actual: 1.0483303905332229
    (543916.069 us vs 518840.314 us)
```

…and **PASSED on `linux-arm64-gcc-release` in the same run at the same commit**. Two half-second arms
on a shared two-vCPU runner: the noise is the same order as the effect.

⚠ **THE FILE'S OWN HEADER ARGUED THE OPPOSITE** — *"the bound is deliberately loose … so the case is
insensitive to host speed and to load"* — and that sentence is the defect in one line. **A ratio's
MEANING is host-insensitive; its MEASUREMENT is two samples.** The header is corrected in place
rather than left to read as evidence.

⚠ **AND IT IS THE SAME SPECIES `check-wall-clock-in-tests` ALREADY REFUSES**, one level of
indirection out: that guard's own docstring says *"a wall-clock assertion sized on a developer
machine is the same defect wearing an assertion's clothes"*, but its subject is a `chrono` **literal**
— and a RATIO has none, so it measured green over this.

★★★ **THE FIX IS TO ASK THE PROPERTY WHAT KIND OF QUANTITY IT IS, AND THE SPLIT IS THE PART WORTH
CARRYING:**

- **A COUNT gets a counter.** [[D-PERF-PP-EVERY-INCLUDE-RE-READS-AND-RE-TOKENIZES-THE-SAME-HEADER]] is
  *the same file is read N times instead of once*. `PreScanMemoCounters` publishes the pre-scan's
  `builds` and `hits` — relaxed atomics on the include path, static accessors plus a test-only
  `reset()`, the shape `substrate::PhaseTimers` already established. The case now asserts **exact
  integers**: 12 units sharing ONE header ⇒ **1 build, 11 hits**; 12 units naming 12 byte-identical
  DISTINCT headers ⇒ **12 builds, 0 hits**. Identical on every host at every load, and **false by a
  factor of `kUnits`** the moment the memo goes unconsulted — against the ~1.7× the ratio ever had.
  Renamed to what it proves: `OneHeaderAcrossManyUnitsIsReadAndTokenizedOnce`.
- **A COST keeps a clock, and the ESTIMATOR is what gets fixed.**
  [[D-PERF-PP-IF-REMATERIALIZES-THE-WHOLE-SYNTH-BUFFER-PER-EVALUATION]] is a **memcpy whose only trace
  is time**. ⚠ A counter for it would have **NO WRITER** in the fixed code — the copy is gone — so it
  could never fire: a fixture synthesizing nothing, which is the failure this project keeps closing
  elsewhere. That case therefore keeps its ratio, **bound untouched at 2.0**, and takes `min` over **3
  INTERLEAVED rounds**, because scheduling noise is additive and one-sided (a descheduled arm can only
  be measured slower than it is). `kRounds` is a repetition count, not a duration, so it cannot go
  stale on a slower host.

✔**RED-ON-DISABLE VALID, TWO MUTANTS**, verdicts through `ctest` and read by failing NAME:
**M1** the memo is never consulted — the original defect put back — reds with *12 units sharing ONE
header read, spliced and tokenized it 12 times*; **M2** the hit counter is never incremented — the
**anti-vacuity** arm, since a `builds == 1` that held because the other eleven units never asked would
otherwise read green — reds with *built 1 time but served 0 times, where 11 was due*. Both controls
rc=0, md5 moved and returned, every build rc asserted.

⚠⚠ **AND THE FIRST RUN OF THAT CONTROL WAS INVALID, FOR A REASON THAT WILL RECUR.** `shutil.copy2`
restores the backup's **mtime**, which is OLDER than the objects the mutant build produced — so ninja
called them up to date, skipped the rebuild, and the closing control measured **the MUTANT while
reading a pristine md5**. This is the tree's own *"REBUILD BEFORE YOU TRUST ANY RED"* wearing a new
hat: the restore now bumps the mtime, and the OPENING control forces its own rebuild too, because a
previous run can leave a stale object behind for the next one to inherit.

### The third defect — found by taking the gate, not by reading CI

✔**MEASURED while running the four-leg gate for this very commit.**
`remote-leg --carriage macos` resolved the host for `leg-tree prepare` — reaching it at
192.168.0.71 and moving its clone from `301e2a63` with 2854 dirty paths to a pristine
`73f74972` — and then **failed to resolve the same `.local` name for the rsync seconds
later**. A leg calls the carriage eight or so times and `ssh-macos.sh` re-ran its whole
resolver on every one, so a run's success depended on N independent mDNS lookups ALL
succeeding. Under WSL the only arm of that resolver which answers a `.local` name is a
hop out to the Windows resolver.

★★ **THE SHAPE WAS ALREADY ON FILE AND MIS-ATTRIBUTED.** `ssh-macos.sh`'s own header
records *"mDNS then answered for the rsync and failed for the build minutes later, so the
run got FAR enough to look like the override had worked"* — filed under
[[D-SCRIPT-MACOS-HOST-OVERRIDE-DOES-NOT-CROSS-THE-WSLENV-BOUNDARY]], i.e. as a missing
environment variable. ⇒ It is not. It is a property of **asking repeatedly**, and it
survives every fix aimed at any single lookup. ⓘ ✔Five consecutive direct lookups all
returned the address, so the failure is transient — which is precisely why a per-call
resolver hides it until a leg is half-finished.

⇒ `ssh-macos.sh` gains `--resolve` and `ssh-macos.ps1` the matching `-Resolve` (twin
parity, same commit); `remote-leg.sh` calls it ONCE per macos leg and exports
`DSS_MACOS_HOST` for that process only. ⛔ **It pins no address anywhere persistent** —
the Mac is on DHCP, which is why that config deliberately stores the NAME; only the RUN
gets an address, for its own lifetime. ⓘ The failed run discharged its cleanup correctly:
`leg-tree restore` ran on the die path and reported *0 dirty, at 73f74972*, so no host was
left staged. [[D-SCRIPT-MACOS-LEG-RERESOLVES-THE-HOST-AT-EVERY-CARRIAGE-CALL]]

### ★★★ AND FIXING THE BUILD BREAK UNCOVERED TWO PRODUCTION DEFECTS, ONE OF THEM A SYMLINK ESCAPE

⚠⚠ **THE MSVC JOB HAD NEVER REACHED ITS *TEST* STEP.** The build has been broken since P34,
so repairing it ran that suite for the first time — **1775 tests, 6 cases failing in 4 binaries**,
none of them a regression and none of them reachable before. Two were real defects in shipped code.

**(1) The Windows exclusive claim followed a planted dangling symlink.**
[[D-LINK-WRITER-WINDOWS-EXCLUSIVE-CLAIM-FOLLOWS-A-DANGLING-SYMLINK]]
`CREATE_NEW` is **not** the Windows spelling of `O_EXCL`. POSIX.1 *requires* `O_CREAT|O_EXCL` to
fail with `EEXIST` when the pathname is a symbolic link — target existing or not — and that mandate
is the entire reason the staging-temp claim is safe there. Windows carries no such rule: it
traverses the reparse point and creates the **target**, so the compiler's staged artifact goes
wherever the link pointed. ✔MEASURED: `claim(p)` returned TRUE where the case demands false, then
`fs::exists(p)` returned TRUE — because the claim had just created the target through the link.

⚠⚠ **AND IT WAS INVISIBLE FOR A REASON WORTH CARRYING, NOT BECAUSE NOBODY LOOKED.** The guards
exist. Their own header states — correctly, and measured — that MinGW's libstdc++ `create_symlink`
returns **ENOSYS**, so the shipping Windows toolchain *cannot construct the input*; they therefore
**SKIP** on Windows and assert only on POSIX. ★ **A test that skips on the one platform whose
primitive differs is not covering that platform — it is reporting that it did not, and the report
was read as a pass.** ⇒ Fixed at the ONE owner (`detail::createExclusiveBinary`, which the runtime
object cache already reuses rather than re-deriving): `CreateFileW` with
**`FILE_FLAG_OPEN_REPARSE_POINT`**, which restores the POSIX guarantee exactly — the create no
longer traverses the link, so an occupied name fails `ERROR_FILE_EXISTS`. Both properties the old
`wbxN` spelling carried (no-inherit, binary) are preserved and their measurements are at the site.

**(2) An LSP `file://` URI with a UNC authority did not round-trip.**
[[D-LSP-FILE-URI-WITH-A-UNC-AUTHORITY-DOES-NOT-ROUND-TRIP]]
`pathFromFileUri` refused every non-empty, non-`localhost` authority, so on the one platform that
models UNC roots the LSP **could not parse back the URI its own `fileUriFromPath` had just
emitted** — a Windows client opening a file on a network share was unreachable. ⚠ **Two live
assertions were in direct contradiction the moment MSVC ran**: `FileUriRoundTrip` requires
`file://server/share/x.c` to round-trip while `NonFileUriIsRefusedRatherThanGuessed` asserted a
flat `EXPECT_FALSE` on the same URI shape. Neither had ever been challenged, because no Windows leg
could build MSVC.
⇒ The predicate is asked of the **path type**, never of a platform macro: `platformModelsUncRoots()`
tests `fs::path{"//dss-unc-probe/share"}.root_name()`. `fileUriFromPath` already emits an authority
exactly when that is non-empty, so asking the same question in the inverse makes the two directions
**provably symmetric** instead of two hand-kept lists that drift. Where UNC roots exist the
authority is restored; where they do not, a named authority is still refused — there the text names
a remote HOST and turning it into a path would be the guess the sibling case exists to prevent.
★ Both tests are fixed **as tests, not widened**: `FileUriRoundTrip` gains the THIRD ARM its own
comment predicted (*"this needs a third arm, NOT a widened check"*), and the refusal case splits
into two NAMED arms.

ⓘ The other four failures were not defects in the repo. `HeaderNameMatching.ShippedConfigTreeHasNo`-
`CaseCollidingPaths` threw an opaque locale-language exception (*"no mapping for the Unicode
character in the target multi-byte code page"*) because that case sweeps the WHOLE working tree and
this checkout held a stray **empty directory literally named `C:`** — the U+F03A mangling a
mis-quoted WSL command leaves behind, untracked, and exactly the accident the standing orders warn
about. Removed. ⚠ A fresh CI checkout has none, so that one would not have reddened there; it is
recorded because the case's verdict depending on untracked junk, and THROWING rather than naming
the file, is worth knowing before it costs somebody an afternoon.

### What this cycle did NOT do

- ⓘ **This bullet is CORRECTED rather than deleted, because its correction is the cycle's largest
  finding.** It read: *"the MSVC leg's test results are unknown at the tree this cycle ships; a green
  MSVC suite is not claimed here."* True when typed. The full MSVC leg was then run locally under
  Visual Studio 18 with CI's own generator and build type — and it is what surfaced the two
  production defects above. ✔The four affected binaries are now green under MSVC.
- ⚠ **`linux-clang-asan` was still running** when both diagnoses were taken. No claim is made about it.
- ⓘ `build/` carries dozens of stale lane trees and hundreds of `p3x-*.log` files from earlier
  cycles. Not swept: it is gitignored working state in a tree a concurrent workstream shares, and
  deleting it is the operator's call, not a cycle's side effect.

---

## 0.000000000000000000000000000000000000000 ★★★ CYCLE P36 — THE GOAL CORPUS BUILDS AND RUNS AT RELEASE, AND THE THING IN THE WAY WAS ONE C TYPE COUNTED TWICE

> This cycle ran across TWO sessions. The first launched nine lanes and was
> interrupted; the second folded them. Read §"WHAT THE INTERRUPTION COST" before
> trusting any lane attribution below — the code all landed, the *reports* did not.

### ★★★ THE HEADLINE — sqlite COMPILES AND RUNS AT `--config=release`, WITH `Inlining` IN

✔MEASURED at the fold, Windows x86_64, release `dsscp`, 103-TU sqlite via
`--project … speedtest1.dss-project.json --config=release --jobs 4`:

| | before | after |
|---|---|---|
| exit | **1** | **0** |
| `error[…]` | **5 × `I_StoreValueTypeMismatch`** | **0** |
| artifact | **none** | **5,108,736 B `speedtest1.exe`** |

★★ **AND IT RUNS.** `speedtest1 --size 3 --testset main` on this host: every test
executes, `PRAGMA integrity_check` passes, **exit 0**. That is the round trip —
103 translation units compiled at release with the inliner on, linked, executed —
and it is the first time this corpus has produced a running artifact at release.

⚠ **THE GATE AND THE CORPUS ARE STILL TWO DIFFERENT MEASUREMENTS AND ARE STATED
SEPARATELY.** `tests/harness/test_sqlite_harness_legs.cpp` — the only sqlite-named
`ctest` entry — asserts the **leg resolver**, not the build. A green `ctest` says
nothing about whether sqlite compiles, which is exactly why P36 spent a session
believing a green suite while the goal corpus was red.

### ★★★ THE DEFECT — HOST IDENTITY WAS THE DECLARATION SITE, SO ONE C TYPE FORKED PER CU

`reinternType` keyed a host composite on `(srcId.arenaTag << 32) | srcId.v`,
documented as *"distinct source composites stay distinct"* — true, and it also
keeps a type distinct from ITSELF. sqlite declares `typedef struct Bitvec Bitvec;`
in a header and defines it only in `bitvec.c`, so every TU handling a `Bitvec*`
contributed its OWN incomplete `Bitvec`. The verifier's new Store rule then
reported 5 errors whose two sides were one C type spelled two ways —
`TOTAL mismatching stores=8 · structurally-identical=8 · genuinely-different=0`.

★ **THE RULE THIS RESTORES IS THE ONE THE HEADER ALREADY DECLARED:** *"the host's
hash-consing can canonicalize structurally-identical types from different CUs to
one TypeId."* Every non-composite kind already obeyed it. Composites were the
exception only because a composite may CONTAIN A CYCLE, so its host id must be
minted before its fields are known — and the decl-site key stood in for an
identity nobody had computed. It is computable, from the source side, before any
minting.

★★★ **FOUR ATTEMPTS, AND EVERY ONE OF THE THREE FAILURES IS WORTH MORE THAN THE
FIX.** They are recorded here because each looked correct and each was caught by
a MEASUREMENT rather than by review.

1. **Full recursive layout digest** → `BtCursor` still forked. A TU that has seen
   `struct Btree { … }` and one that has only seen `struct Btree;` give the
   ENCLOSING struct different digests. ⇒ **the completeness of something you only
   POINT AT was leaking into your own identity** — this very defect, one level down.
2. **Tag-only behind a pointer** (the obvious repair) → `dss::TypeLattice fatal:
   completeComposite: composite re-completed with different fields`. Two enclosing
   structs shared a key while their pointer fields still reinterned apart.
   ⇒ **THE INVARIANT, and everything since is built on it: equal identity ⇒ every
   field reinterns to the same host type.** Erasing information the reintern still
   has breaks it; RESOLVING instead (an incomplete composite is replaced by its
   tag's definition, by one shared `resolveDefinition` both halves call) loses
   nothing.
3. **Exact recursive digest with de Bruijn backreferences** → correct, and **952 s
   of CPU with no output**, killed by PID. A subtree that back-references an OUTER
   composite cannot be memoized, so inside a mutually recursive cluster nothing is
   cacheable and the walk re-expands once per path. sqlite's
   `sqlite3`/`Vdbe`/`Parse`/`Expr` cluster is exactly that shape.
   ⇒ replaced by **iterative partition refinement** — `h(0) = local`,
   `h(k+1)[n] = mix(local[n], h(k)[refs…])`, stop when a round adds no class.
   Ordinary, standard, and cycles are just edges. **Same corpus: converges in 2
   rounds over 14,663 nodes.**
4. ⚠ **AND A BUG IN THE MEMO ITSELF, WHICH IS THE ONE TO FEAR:** the cache key
   bit-packed a mode flag into bit 63 and XOR-ed a golden-ratio constant whose bit
   63 is also set, so one mode's lookup could return the other's entry. Silent, and
   only on the second lookup. **Hash a key, never bit-pack one into a word whose
   width nobody re-checked.**

### ★★★ AND THE LAST ONE WAS NOT IN THE MERGE AT ALL — A PER-CU AST NODE ID INSIDE A TYPE'S NAME

With the refinement in place the corpus still emitted 2 errors, both
`Ptr<Struct 'BtCursor'>` against `Ptr<Struct 'BtCursor'>` — same tag, both
COMPLETE. ✔The instrument that found it is a count the index now keeps of its own
failures: **98 tags FORKED, every one of them with a SINGLE local layout
signature** — `Parse`, `Table`, `Select`, `Index`, `KeyInfo` and 93 others. One
local signature and many identities cannot be a layout difference, so identity was
leaking in from below.

★★ **IT WAS THE NAME.** An anonymous member is bound as `<anon:RULE:NODEID>`,
where `NODEID` is a per-CU AST node index — and that name reaches the interned
TYPE. Two CUs including the same header therefore give one anonymous `union` two
names, and **every named struct that reaches one inherits the split.** The spelling
had THREE inline readers and no owner; it has one now
(`core/types/anon_member_name.hpp`), which keeps the RULE (`structSpec` and
`unionSpec` overlay their fields differently) and drops the node id. **98 → 0.**

⚠ **THE NAME IS PART OF THE HOST KEY, SO BOTH HALVES TAKE IT OR NEITHER DOES.**
Normalising it for the identity but not for `forwardComposite(kind, name, key)`
gave two anonymous composites ONE key and TWO host types, and their shared parent
aborted on the conflicting re-completion — with the type graph otherwise perfectly
unified. The same class of mistake as attempt 2, one call site over.

### ★★ THE DIAGNOSTIC THAT PAID FOR ITSELF INSIDE THE CYCLE THAT WROTE IT

`I_StoreValueTypeMismatch` and `I_CallSignatureMismatch` named their types by raw
interner id and `TypeKind` ordinal: *"value typed 5167 (kind 27) into an address
whose pointee is 6074 (kind 27)"*. Kind 27 is `Ptr` on both sides and a pointer has
no vocabulary name, so the message said *"two pointers, different numbers"* and
stopped — and reading it took a throwaway instrument that then died with the
session. `describeType` now spells the type beside the id (**the id stays** — it is
the only thing that distinguishes two forks of one spelling), and it marks an
INCOMPLETE composite, which is the load-bearing half. The next run said
`Ptr<Struct 'Bitvec'>` vs `Ptr<Struct 'Bitvec' incomplete>`, and that sentence is
what identified defect 1 above. ★ **A diagnostic whose reader has to build a tool
is not a diagnostic.**

### ⚠⚠ WHAT THE INTERRUPTION COST — THE CODE LANDED, THE REPORTS DID NOT

Nine lanes ran in session one. **Every lane's CODE is in the tree and gated.**
Lanes J, P and S wrote their registry rows to FILES, and those applied verbatim.
Lanes G, K, L, N, Q and R reported by CITING A SCRATCHPAD PATH, and those paths
held working files, not deliverables — so their row text, their red-on-disable
transcripts and their mutant md5s are gone.

★ This is [[D-CYCLE-A-LANE-DELIVERABLE-LEFT-IN-THE-SCRATCHPAD-IS-INVISIBLE-TO-THE-FOLD]]
at full scale, and the row is widened with it. **The one-line test stands: if the
orchestrator would have to open a file to fold your work, the work is not reported
yet.**

**What the fold did instead, and what it could not do.** Every row it closed for a
lost lane was RE-DERIVED BY MEASURING THE TREE — the staticlib member names and the
object-input link were re-proved by RUNNING the shipped binary, the summary
sections by reading the ten shipped format documents, the growth budget by
re-reading the trace. What could NOT be re-derived is the falsifiability evidence:
**a passing test is not proof that the test can fail**, so those rows close on the
fix being present and exercised, say so in as many words, and owe their
red-on-disable arm to whichever cycle next touches those files.

⚠ **AND ONE STALE TEST HAD TO BE INVERTED.** `AsmDataSection.Int128GlobalFailsLoud`
asserted a refusal that a lane had since implemented, so the baseline gate was RED
on a test contradicting the shipped binary. **A test that asserts a refusal is a
claim about the product; inverting the product without inverting the test leaves a
gate that disagrees with the compiler.** Replaced by a positive test asserting the
SIXTEEN BYTES, in three cases with three distinct failure modes.

### THE GATE

✔ALL FOUR LEGS GREEN, each on a tree that was NOT moving under it. ⚠ The Windows
number was taken TWICE and only the second is quoted: the first ran while
`.plans/**` was still being edited, and `.plans/**` is the SUBJECT of five
guards, so that run measured a tree that no longer existed
([[D-CYCLE-THE-ORCHESTRATOR-EDITED-PLANS-UNDER-A-RUNNING-LANE-AND-FLIPPED-ITS-GATE]],
self-inflicted, caught before it was quoted).

| leg | result |
|---|---|
| Windows x86_64 `ctest` (baseline, before the fold's own work) | **1668 / 1671** — 2 plan guards (the held row batch) + 1 stale test |
| **Windows x86_64 `ctest` (frozen tree)** | **1671 / 1671**, 0 failed, 551.6 s |
| **WSL x86_64 + qemu arm64** | **1653 / 1653**, 0 failed, 252.4 s (guards `-LE repo-guard`: the root host ran them) |
| **macOS arm64 (Apple Silicon)** | **1671 / 1671**, 0 failed — configure 10 s, build 181 s, ctest 682 s |
| **arm64 VPS (native aarch64)** | **1653 / 1653**, 0 failed |
| **103-TU sqlite, `--config=release`** | **rc=0, 0 errors, 5,108,736 B in 15.1 s — and the artifact RUNS (exit 0, `PRAGMA integrity_check` passes)** |

⚠ **A CARRIAGE DEFECT MEASURED WHILE THE LEGS RAN, AND NOT YET FIXED:** the macOS
push shipped **`.kilo/` — 61 MB, 3,671 files** including a `node_modules` tree.
Git IGNORES it (via `.git/info/exclude` plus `.kilo/.gitignore`), but the carriage
excludes are a HAND-MAINTAINED ENUMERATION and their `node_modules` entry is
ANCHORED (`./node_modules`), so `.kilo/node_modules` sails straight through.
★ **The general form is the row worth writing: the transport re-derives, badly, a
question `git` already answers exactly.** Excluding what `git status --ignored`
reports would make *"a gate host holds the repo and nothing else"* true by
construction instead of by enumeration — and an enumeration cannot discover the
next directory some tool drops in the root. ⓘ It cost transfer time and gate-host
disk, not correctness: no leg reddened. **Deliberately NOT fixed between a green
gate and the commit** — `scripts/**` is a ctest subject, and editing one there
would have voided the four leg results this table reports.

### ★★★★ THE STANDING ROADMAP — operator, 2026-08-26. THIS ORDERS EVERYTHING BELOW.

> *"so the goal is: optimizations (compile time + optimization pipeline), then production
> anchors then FC20 and FC19"*

| # | phase | what it means |
|---|---|---|
| **1** | **OPTIMIZATIONS** — and it is TWO halves, both in this phase | **(a) COMPILE TIME**: the campaign pinned in `project-compile-time-baseline-2026-08-25` — 77% of a one-line compile is not compilation, and the unidentified 383 ms decides the cache payoff. **(b) THE OPTIMIZATION PIPELINE**: the `D-OPT*` burndown. |
| **2** | **PRODUCTION ANCHORS** | the rest of `_deferred-anchor-registry-production.md`, which is already the standing priority (operator, 2026-08-25). |
| **3** | **FC20** | plan 23. |
| **4** | **FC19** | big-endian conformance on s390x. ⚠ **FC20 BEFORE FC19 — that reversal of plan 23's numeric order is DELIBERATE and a future reader must not "correct" it.** Four phases, not a target-descriptor edit. |

⚠ **This roadmap does not repeal the no-follow-ups rule** — it says which work is picked NEXT,
never that a row opened in phase 1 may be carried into phase 3. A row you open, you close.

⚠ **A sequencing constraint measured 2026-08-26:** FC19 declares the `endianness` target key and
so writes `src/core/types/target_schema.*` + `src/dss-config/targets/**`. It may not run beside
any lane holding those files — the P38 register-class lane held exactly them.

### ★★★ NEXT — in order

1. ★★★ **RE-RUN THE FOUR-HOST sqlite PROBE AND THE speedtest1 BENCHMARK — THEY WERE
   CANCELLED MID-FLIGHT, ON PURPOSE.** The operator's instruction was to run the probe
   and the benchmark on all four hosts and update the README; the probe found
   [[D-LINK-MERGE-DOES-NOT-REMAP-BLOCK-SYMBOLS]] on its first host and everything was
   stopped to fix that first (*"cancel ALL other probes. let's address issues first,
   then we re start them on a fixed basis"*). ⚠ **NO README NUMBER WAS UPDATED THIS
   CYCLE AND NONE SHOULD BE** — every figure now in the README predates this fix, and
   a benchmark taken against a compiler that could not link the subject is not a
   number. ⓘ What IS re-usable from the aborted run: the macOS driver self-test and
   the VPS `SRC_DIR` are both fixed, so the next run starts from four hosts that can
   actually reach the corpus.
   ★ Known before starting: the pe64 leg has **NO CONTROL** — its mingw reference
   oracle fails to compile `ext/misc/fileio.c` on missing `windows.h` types
   (`CP_UTF8`/`WCHAR`/`HANDLE`), so the harness correctly reports NO ORACLE and that
   leg's failures are unattributable until someone fixes the oracle's define set.
2. **[[D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED]]** — opened this cycle,
   OPEN. Both gcc and clang accept `static void* const tbl[] = {&&L0};`; DSS refuses
   with `H0009`. Filed rather than fixed because the corpus arm reached the same
   jump-table lowering through an ISO-C dense `switch`, so nothing was blocked on it.
3. **[[D-OPT11-LAZY-IMPORT-EDGE]]** is still OPEN and was NOT this cycle's defect —
   ⚠ it was the operator's first hypothesis for the pe64 failure and was refuted by
   measurement (the same manifest fails at `--config=debug`, which carries no
   summary/index import machinery). Its own option-A contamination note stands.
4. ✅ **DONE 2026-08-26, and it is kept only so a reader can see the list was
   maintained rather than quietly edited.** All four legs ran green — the figures
   are in the gate table above, not restated here. ⚠ The clause *"a gate host holds
   the repo and nothing else"* was **NOT** true when they ran, and proving it took a
   follow-up: see [[D-SCRIPT-CARRIAGE-EXCLUDES-ARE-A-HAND-LIST-AND-MISS-NESTED-IGNORED-TREES]].
   ⓘ **The legs' verdicts survive it.** What leaked was ignored trees (`.kilo/`,
   `test-scratch/`, `__pycache__`, `.temp/`) — nothing the examples runner globs and
   nothing `ctest` reads — which is why this is a follow-up and not a re-gate. ★ The
   distinction worth keeping: a stale `examples/` tree would have invalidated them,
   because that runner globs; a stale `node_modules` is transport waste. **Ask what
   the stale tree is an INPUT to** — the same question that governs which files an
   orchestrator may edit under a running lane.
5. **⚠ OPERATOR DECISION — `.secrets/` KEY MATERIAL SAT ON BOTH GATE HOSTS.**
   ✔MEASURED 2026-08-26: all four files (`macos.env`, `macos.key`, `arm64-vps.env`,
   `arm64-vps.key`) were on the Mac **and** on the VPS, md5-identical to the local
   originals — so each host held the *other* host's private key. Removed from both,
   after hashing to prove the originals were intact; the derivation withholds them
   now because git ignores them. ★ Only `remote-leg.sh` had ever named `.secrets` in
   its exclude list, and it STILL leaked, because **rsync does not delete an EXCLUDED
   path** — an exclude added after the fact protects the stale copy instead of
   removing it. **The remaining question is not cleanup, it is ROTATION, and that is
   the operator's alone.** These keys reach real machines and this repo is slated to
   go public.
6. **The red-on-disable arms owed for the lost lanes' rows** — named in each row.
   These are the falsifiability half of six rows that closed on presence and
   execution alone.
7. **The four re-verdict rows the balance gate keeps flagging.** ⚠ ✔MEASURED at
   this fold: the gate's *"opener discharged"* heuristic is NOT the same predicate
   as *"trigger fired"*. `D-FULLC-STDBIT-BIG-ENDIAN-NATIVE`'s trigger is *a
   big-endian target lands*, which has not happened, and
   `D-FULLC-STDBIT-ADDRESSABLE-FN`'s is *a program takes `&stdc_*`*. Three of the
   four are the guard being right about the opener and wrong about the gate; the
   honest fix is to annotate each row, not to flip a status.
8. **README correction still owed** from session one: the *"the answer is the pool"*
   claim was refuted, a `-flto` arm is wanted, `5.4×` → `5.5×`. ⚠ **NOT DONE, and
   deliberately not guessed:** the refuting measurement was in a lost lane report.
   Re-measure before editing.
9. **The interner-level arc** behind the merge fix: `sameRepresentation` still
   compares a composite's operands by raw TypeId, so representation neutrality does
   not survive one level of indirection. 1–2 cycles, with this cycle's measurement
   as its witness. The `instType(cid).v != instType(actual).v` guard in
   `inlining.cpp` is named as a consumer that gets to shrink.
10. **A MEASURED `.sh`/`.ps1` PARITY GAP IN `macos-leg`, recorded rather than filed.**
   ✔MEASURED at this fold by reading both siblings: `macos-leg.sh` accepts
   `--guards`, `--mode`, `--tree` and `--build-type`; `macos-leg.ps1` accepts none
   of the four (`-Src`, `-Filter`, `-Jobs`, `-NoPush`, `-Dst`, `-ResetTo` only).
   The `.sh` header dates those flags to P35, so they landed in one sibling.
   ⓘ **`--prune` is NOT part of the gap** — it is not a user flag at all; both
   siblings pass `-Prune` to the carriage unconditionally, which is the P34
   ruling met. ⚠ It blocked nothing here (a plain full leg needs none of the
   four), which is why it is a note and not a row — but a Windows-side caller
   wanting `--mode build` today has no way to ask for it.
   ⚠⚠ **THE `--guards` QUARTER OF THIS NOTE WAS WRONG TO CALL HARMLESS, AND IS NOW
   FIXED AND FILED: [[D-SCRIPT-MACOS-LEG-PS1-CANNOT-SKIP-THE-REPO-GUARDS]].** The
   `.ps1` did not merely lack the flag — it had no `-LE repo-guard` line at all, so
   every PowerShell-driven macOS leg RAN the repo guards whatever was asked for.
   ✔MEASURED at the P36 gate: macOS **1671** against WSL and VPS **1653**, the
   difference being exactly the 18 `repo-guard` entries. ★ **The judgement that went
   wrong is worth keeping: "it blocked nothing" was true of the other three flags and
   false of this one, because a missing flag whose DEFAULT the caller cannot reach is
   not a missing option, it is a different behaviour.** A parity gap is harmless only
   when the missing half has no default worth having. The other three flags remain
   open and remain a note, on exactly that test.
11. **Two worktrees deliberately KEPT**, with uncommitted work nobody has
   adjudicated: `C:/Source/DailySoftware/dss-lane-r` (136 dirty files) and
   `dss-perf-probe` (4, including a `src/perf_sampler.cpp` that exists nowhere
   else). Five others were removed after verifying their work is in HEAD or in the
   shared tree. ⛔ The three `.claude/worktrees/agent-*` trees belong to other
   sessions and must not be touched.

---

## 0.00000000000000000000000000000000000000 ★★★ CYCLE P35 — COMPILE TIME, AND THE LAST HOST WHERE DSS LOST DREW LEVEL BEFORE THE CYCLE ENDED

**Operator instruction, verbatim:** *"let's fix compile DSS time then measure agains our references"*.

### ★★★ THE HEADLINE, AND IT REVERSES WHAT THIS PROJECT BELIEVED

✔MEASURED with a NEW instrument (`scripts/compile-bench/`), 20 runs after 3 warm-ups, whole task
source→executable, each compiler on its own command line. ⚠ **THE WINDOWS ROWS ARE `min`, NOT MEDIAN,
AND THAT IS THE INSTRUMENT'S OWN RULE RATHER THAN A CHOICE THAT FLATTERS**: its caveat 8 refuses any
before/after difference smaller than the `spread` column, this host's spread is 44–46 ms, and the delta
being claimed is 35.4 ms. By median the improvement is UNPROVEN; by `min` — the run that got the most
machine — the two distributions do not overlap at all. The other two hosts' rows are medians on quiet
hosts and are labelled as such; ★ **do not divide a number from one of these rows by a number from
another reading.** Lane E independently read the same pre-fix binary at 184 ms where this instrument read
138.2 ms, which is exactly how far a cross-session ratio can be wrong.

| host | dsscp `tiny` | best reference | verdict |
|---|---|---|---|
| **macOS arm64** (post-fix) | **25.2 ms** | Apple clang 21 — 31.5 ms | **DSS 1.25× FASTER** |
| **WSL x86_64** (PRE-fix binary) | **41.7 ms** | clang 18.1.3 — 188.1 ms · gcc 13.3.0 — 327.1 ms | **DSS 4.5× FASTER** |
| **Windows x86_64** (mid-cycle, lanes A/B/D only) | **128.9 ms** | gcc 13.2.0 — 90.7 ms | DSS 1.42× SLOWER |
| **Windows x86_64** (FINAL, lane E folded) | **93.5 ms** | gcc 13.2.0 — 90.9 ms | **DSS 1.03× — LEVEL** |

★★★ **THE FIRST READING OF THIS SAID "WINDOWS IS ~5× OFF ITS POSIX SIBLINGS" AND BLAMED THE HOST.
THAT WAS WRONG, AND IT WAS WRONG IN THE MOST EXPENSIVE DIRECTION — a host is the one thing nobody can
fix, so a wrong host attribution retires a question instead of answering it.** ✔THE CONTROL, taken only
because the operator asked whether the comparison was fair: same host, same source, same binary, **only
the target format changing** — `pe64-x86_64-windows` **3 CUs / 158 ms**, `elf64-x86_64-linux` **1 CU /
63 ms**, `macho64-arm64-darwin` **1 CU / 64 ms**. The two extra CUs are **pe64-ONLY** — `runtime/platform/src/dirent.c` and `unistd.c`, DSS's own POSIX implementation half, bound to the format by a config `realization` map and not by any engine branch — and cost **95 ms
of a 158 ms compile (60%)**. Cross-compiling the same file to ELF *on Windows* is 63 ms against WSL's
41.7 ms ⇒ the genuine Windows-HOST penalty is **~1.5×**, and ⚠ even that is not a floor, because the WSL
half of the pair is a PRE-fix binary. **Most of what looked like a host problem was DSS rebuilding the
Windows runtime half on every invocation.**
★ THE LESSON, and it is the same one twice in two cycles: **an instrument that reports a DIFFERENCE does
not report its CAUSE.** Two hosts differ in host AND in realized-CU count; naming the host required
holding the other variable still, and nobody had. ⚠ THE SPREAD COLUMN (0.9–2.6 ms on macOS, 32–47 ms on Windows) IS STILL A REAL HOST SIGNAL — Windows is
genuinely noisier — but it is NOT the bulk of the gap, and reading it as though it were is what produced
the wrong first verdict.

⚠ [[D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX]] measured a ~1.2× residual and stays OPEN —
that figure was taken on a **103-TU build, which amortizes a per-invocation floor away**. On a SMALL
compile the same-machine gap was read as ~5×. ⚠ **THAT FIGURE IS RETIRED — it descends from the host
attribution corrected above, and the number that replaced it is 93.5 ms on Windows against 41.7 ms on
WSL.** ⚠ Even that pair is not yet a real comparison, because the WSL half is a PRE-fix binary and no
post-fix WSL reading has been taken. Both rows measure different things; the row should be re-verdicted
against a matched pair, not against either of these.

### ★★★ WHY WINDOWS COMPILES MORE TRANSLATION UNITS, AND WHY THAT IS NOT THE BUG

⚠ **THE FIRST WRITE-UP OF THIS CYCLE CALLED THEM "the two extra CUs" AND LEFT IT THERE, WHICH INVITES
EXACTLY ONE WRONG READING — that they are waste to be deleted.** The operator supplied the framing
unprompted, 2026-08-25: *"windows compiles more TU than posix because we do have a posix C file own
implementation that mimics the posix implementation so windows have the same interface (which is
correct) … That's why this cache will do the trick for real big projects"*.

✔VERIFIED IN THE TREE. `src/dss-config/runtime/` holds **exactly two files** — `platform/src/dirent.c`
(181 lines) and `platform/src/unistd.c` (139 lines). Each is bound to the `pe` object format by one
line in its own descriptor:

```
shippedLibs/dirent.json:  "realization": { "pe": { "source": "runtime/platform/src/dirent.c" } }
shippedLibs/unistd.json:  "realization": { "pe": { "source": "runtime/platform/src/unistd.c" } }
```

★ **THAT MAP IS THE WHOLE MECHANISM, AND IT IS AGNOSTIC BY CONSTRUCTION.** `library` says which IMAGE a
symbol is imported from; `realization` says WHETHER it is imported at all. On `elf`/`macho` the map
carries no key, the import default stands, and the driver adds nothing — **1 CU**. On `pe` the driver
adds the named file as an ordinary extra translation unit compiled FOR THE TARGET on ANY host —
**3 CUs**. There is no `if (format == pe)` anywhere in the engine; the difference is a config document.

★★ **AND THE UNITS ARE THE CONSERVATIVE ANSWER, NOT THE EXPEDIENT ONE.** Windows exports no POSIX
directory API from ucrtbase OR kernel32, and the nearest-looking substitutes are traps the tree already
documents: ucrtbase exports `_sleep` which takes MILLISECONDS where POSIX `sleep` takes SECONDS, so a
`linkName` onto it would link clean, load clean, and sleep 1/1000th of the requested time. gcc's literal
answer here is "link libmingwex" — which DSS cannot take without breaking
[[D-HARNESS-CROSS-HOST-ANY-TARGET]] and re-adopting the third-party runtime the pe→UCRT migration ran to
eliminate. So DSS ships the body, and the body is checked by the compiler: each unit `#include`s the
header its descriptor publishes, so a signature that drifts STOPS COMPILING.

★★★ **THE CONSEQUENCE FOR THE CACHE, WHICH IS WHY THE OPERATOR RAISED IT.** This is a FIXED cost — two
units, whatever the project's size — but it is fixed **PER INVOCATION**, not per project. ✔DEDUCED FROM
MEASURED DATA, and the arithmetic is decisive rather than suggestive: the runtime half costs **95 ms**
while the measured **per-TU slope is 8.5 ms** at `-j6`, so it CANNOT be inside the slope — it is inside
the 125 ms floor, compiled once per invocation however many TUs that invocation carries. ⚠ Say "deduced"
and not "measured": no counter was read for these two units the way `load-config.runs` was read for
config. 🧠INFERRED and NOT yet measured at all: a project driven one-TU-per-invocation, which is what every
`make`-style build does, pays that floor **N times** — 100 files ⇒ 200 redundant runtime-unit compiles,
order **9.5 s** of pure repetition. A
content-addressed object cache keyed on (target × format × config × toolchain stamp) collapses all of it
to one, permanently and across future builds. **That is the case for wiring the cache, and it is
strongest exactly where the operator said it would be: real projects.**

⚠ **DO NOT QUOTE 95 ms AS THE CACHE'S EXPECTED SAVING — IT IS AN UPPER BOUND, AND THE ERROR WOULD BE IN
THE FLATTERING DIRECTION.** That figure is the pe64-minus-elf64 delta on one host, and pe64 also LINKS
three objects where elf64 links one. A cache removes the COMPILE of the two units; it does not remove
their link, their descriptor resolution, or the archive read that replaces the compile. **The realized
saving is 95 ms MINUS whatever of that delta is link — and nobody has split those two yet.** The split
is a one-command measurement (cold cache vs warm, same target, artifact probed) and it belongs in the
row that wires the cache, not in a forecast.

### ★★★ THE FINDING THE OPERATOR'S QUESTION PRODUCED — A CACHE THAT EXISTS, PASSES ITS TESTS, AND IS WIRED TO NOTHING

The operator asked whether gcc was reusing objects while DSS was not. ✔BOTH DIRECTIONS MEASURED, and the
answer is that the comparison is unfair **to DSS**:

- **gcc and clang are not cached.** Windows `gcc` is the real 1.7 MB binary (a `ccache` IS on PATH and
  nothing routes gcc through it); WSL `/usr/bin/gcc` → `gcc-13` and `/usr/bin/clang` → `llvm-18`, with
  `/usr/lib/ccache` NOT on PATH, and WSL ccache reported **0 hits out of 14 cacheable calls** all session.
- **DSS is not being flattered either — it is being PENALISED.** gcc links a PREBUILT libc and never
  compiles it; DSS compiles two shipped runtime source units from source on every single invocation. That
  work is inside DSS's 138 ms and absent from gcc's 95 ms.

★★★ **AND `src/program/runtime_object_cache.{hpp,cpp}` EXISTS TO PREVENT EXACTLY THAT, IS FULLY
IMPLEMENTED, HAS ITS OWN ~800-LINE TEST FILE, AND HAS NO PRODUCTION CALLER.** ✔MEASURED three ways:
every exported function — `computeRuntimeObjectKey`, `storeRuntimeObject`, `lookupRuntimeObject`,
`resolveRuntimeCacheRoots`, `runtimeKeyDocumentPath`, `buildStampPathSegment`,
`runtimeCacheBuildStampSegment` — has **ZERO** call sites outside its own implementation file
(`resolveArchiveSiblingFormat` shares the file, has 4 callers, and is unrelated); `compile_pipeline.cpp`
INCLUDES the header and calls nothing from it; and setting `DSS_RUNTIME_CACHE_DIR` to an empty directory
wrote **0 entries** with cold and "warm" timings indistinguishable.

★ **The operator named the fix before the diagnosis was finished:** *"we just compile the prebuilt items
(hermetic cost) once, so big for projects it will be almost invisible"* — which is precisely the design
already sitting in that file, content-addressed and build-stamp-keyed. It is a **wiring** job, not a
design job.

### THE FOUR LANES — every row BORN CLOSED, zero opened

**A — [[D-CONFIG-A-SCHEMA-DOCUMENT-IS-REBUILT-ONCE-PER-LOAD-INSIDE-ONE-PROCESS]]** (production).
P34's lane-SEAL memo folded forward, EXTENDED to `TargetSchema` and `ObjectFormatSchema`, plus three
new `--time` phases. ✔`test_semantic_analyzer_c` **70.13 s → 15.35 s (4.6×)**, `test_hir_lowering_c`
**36.56 s → 12.86 s (2.8×)**. Measured by an INTERLEAVED A/B against a one-token mutant, because this
host drifts ±20% over tens of minutes and two readings taken apart would have credited the drift to
the fix.

**B — [[D-DRIVER-SHIPPED-SOURCE-RESOLUTION-COMPILES-EVERY-SHIPPED-GRAMMAR]]** (production). Compiling
one `.c` file fully CONSTRUCTED the grammar of every shipped language — T-SQL, Toy, both asm dialects
— **twice**, to read one array from each. A SAX reader that aborts when the `language` block closes
replaced it: 6 grammar constructions → 1.

**C — [[D-HARNESS-NO-INSTRUMENT-COMPARES-DSSCP-COMPILE-TIME-AGAINST-THE-REFERENCE-COMPILERS]]**
(harness). The instrument above. ★ Its subject LADDER is the deliverable, not the numbers: a fixed
per-invocation FLOOR and a per-TU SLOPE produce the same figure on a single-file benchmark and call
for DIFFERENT fixes, so no single-subject measurement could have told the operator which to buy.

**D — [[D-PERF-SIMPLIFYCFG-ADDRESS-TAKEN-QUERY-RESCANS-THE-WHOLE-FUNCTION]]** (production). A
`--config=release` compile spent **92% of its `optimize` phase in a pass reporting `mutated=0`**.
`Mir::isBlockAddressTaken` is DERIVED — it rescans every instruction of the function per call — and
SimplifyCfg called it once per block before any cheap gate. ✔SimplifyCfg **215 ms → 8 ms**, `optimize`
270 → 61 ms, and the emitted artifact is **BYTE-IDENTICAL over all 630 `examples/c` subjects**.

### ★★ WHAT THE FIXES BOUGHT, AND WHERE THEY DID NOT

✔MEASURED on Windows, RELEASE, quiet host, median of 20:

```
release arm            BEFORE     AFTER      gcc      ratio then -> now
tiny   (1 line)        171.6      138.2      95.1     1.79x -> 1.45x
hello  (3 headers)     195.6      141.6     106.1     1.83x -> 1.33x
mid    (396 lines)     187.8      148.3     103.5     1.82x -> 1.43x
large  (4224 lines)    579.5      312.6     116.0     4.97x -> 2.69x
floor / per-TU @-j6   155.5/17.2 125.1/8.5  57.4/50.3
```

★ **DSS's PER-TU COST IS NOW 5.9× BETTER THAN gcc's; the entire deficit is the 68 ms of floor.**

⚠ **AND THE CONFIG FIX BOUGHT MUCH LESS IN RELEASE THAN IN DEBUG — say so rather than quoting the
flattering number.** ✔Debug showed −19% on the floor; release shows `build-config` at **15 ms**, not
the ~100 ms Debug spends, because release parses config ~7× faster to begin with. A cycle that
measured only its Debug lane trees would have over-claimed by roughly 5×.

### ★★★ WHAT `[other]` IS NOW, AND WHY THAT IS THE REAL DELIVERABLE

Before this cycle a one-line Windows RELEASE compile was 125 ms of `[other]` — wall time inside no
phase at all, and therefore unattributable. It is now **88 ms, with config split out and NAMED**:

```
locate-config   2 ms / 10 runs      the precedence walk (deliberately NOT memoised: its
                                    answer depends on cwd and the environment)
load-config     6 ms /  7 runs      read + digest + memo lookup — all a HIT costs
build-config   15 ms /  3 runs      the work a hit skips; its `runs` IS the miss count
[other]        88 ms                still unattributed
```

⚠ ✔MEASURED and it rules out the obvious suspect: `load-config.runs` is **6 at 1 TU and 6 at 17 TUs**
(per-PROCESS) while `[other]` still grows **~+8.9 ms per TU**. **So config loading is NOT the per-CU
residue, and the next hunt must start somewhere else** — 🧠INFERRED candidates, none measured: the
artifact write, the driver's own CLI/teardown work, and NTFS+AV per-file cost. `--version` is ~7 ms,
so pre-`main` DLL load is already excluded.

### THE CARRIAGE — three harness rows, all BORN CLOSED, all faced rather than filed

- [[D-SCRIPT-WSL-LEG-RSYNCS-AGENT-WORKTREES-ONTO-THE-GATE-HOST]] — the P34 "a gate host holds the repo
  and nothing else" ruling reached two carriages out of three. ✔MEASURED before the fix: **9,661
  worktree files out of 34,831 — 28% of the tree under test was somebody's uncommitted lane.**
- [[D-SCRIPT-WSL-LEG-HAS-NO-BUILD-ONLY-MODE-AND-AN-UNKNOWN-MODE-RAN-A-FULL-GATE]] and its macOS twin
  [[D-SCRIPT-MACOS-LEG-HAS-NO-BUILD-ONLY-MODE-SO-NO-LEG-CAN-BE-BENCHMARKED]] — neither POSIX carriage
  could produce a RELEASE driver, which is why macOS had **never been timed**. ★ The non-cosmetic half:
  **a new terminal state needs a new witness** — the macOS driver authenticates a run by grepping for
  `REMOTE_CTEST_RC[<token>]`, so a mode that never runs ctest would have had a successful build refused
  as UNKNOWN. It emits `REMOTE_BUILD_RC[<token>]` and the driver picks the key by mode.

### ★★★ LANE E — THE CACHE THAT EXISTED, PASSED ITS TESTS, AND WAS WIRED TO NOTHING

[[D-RUNTIME-OBJECT-CACHE-IS-WIRED-TO-NOTHING]] (production, BORN CLOSED). Launched off the operator's own
diagnosis — *"we just compile the prebuilt items (hermetic cost) once, so big for projects it will be almost
invisible"* — which named the fix before this cycle had finished the diagnosis, because the design was
already sitting in the tree.

★★★ **THE LANE REFUTED ITS BRIEF ON THE FIRST READING, AND THAT IS THE CONTROL LOOP WORKING.** The brief
said the realized runtime units reached the LINK and needed hooking to the cache. ✔They did not:
`buildShippedSourceCus` compiled them into ordinary `CompilationUnit`s and APPENDED them to `cus`, so the
whole-program MIR merge swallowed the demanded subset. There was no `--resolve-library` seam to wire, and
the job was not a hookup but a change to **how DSS's runtime half enters the image**.

**THE SHAPE, and one property dictated all of it:** per target, each realized unit is attributed to its
declaring descriptor(s), the archive-writing sibling is resolved through the production
`resolveArchiveSiblingFormat`, the key is computed, the cache is consulted. A HIT yields the cached `.a`; a
MISS runs a nested default `Program` build into a staging area and stores it. ★★★ **The archive route is
taken UNCONDITIONALLY, because a HIT and a MISS must emit the SAME IMAGE** — merging CUs on a miss and
pulling members on a hit would make a cold and a warm build emit different code from identical inputs, so a
green test could go red purely by being run twice. **A cache whose hit rate changes the artifact is not a
cache.**

⚠ **STATED RATHER THAN LEFT TO BE FOUND:** there is now no cross-module MIR inlining between user code and
`opendir`'s body, and member selection is by SYMBOL REFERENCE (the armap worklist) rather than by "did this
build resolve that descriptor". The second is strictly finer; the first is a real loss of optimization
scope, accepted for the hit/miss equality above. ★ **Flagged for operator veto** — it is a deliberate
trade, not an oversight.

**TWO DEFECTS ITS OWN NEW RED-ON-DISABLE FOUND IN ITS OWN WORK**, which is the shape of a lane testing
itself honestly: (i) runtime archives handed to a STATIC-LIBRARY output were fat-merged whole, so a one-CU
`-staticlib` emitted a three-member `.lib`; (ii) a runtime unit that failed to compile exited 1 **and still
wrote the artifact** — an image missing a runtime body links clean and dies at run time, i.e. the exact
failure the mechanism exists to prevent, reintroduced one tier down.

**MEASURED PRIZE** (medians of 12 INTERLEAVED iterations, output dir emptied before every run and the
artifact probed after): one-line C **184 → 131.5 ms (−28.5%)**; a 4224-line TU **375 → 326.5 ms (−12.9%)**.
★ The saving is **FLAT at ~48–52 ms across a 1-line and a 4224-line subject** — the signature of a removed
FIXED cost rather than of noise. The operator's payoff case, **20 SEPARATE invocations**: **3544 → 2516 ms
(−29.0%)**, and even COLD 2650 ms, because the single miss amortises inside the first build.

⚠ **A COST THE LANE INTRODUCED, RECORDED WITH ITS NUMBER RATHER THAN NETTED OUT OF THE HEADLINE:**
`build-config` goes 11 ms / 3 runs → 25 ms / 29 runs on the warm arm, because `resolveArchiveSiblingFormat`
loads all 24 shipped object-format documents on every invocation to PROVE the sibling is unique. It cannot
be skipped — the sibling is a key term — and it eats ~16 ms of the ~52 ms gross. A persistent format index
is the next lever.

### ★★★ THE FOLD CAUGHT A DEFECT TWO OF THE THREE GATE HOSTS CANNOT SEE

[[D-CONFIG-A-LANGUAGE-IS-LOOKED-UP-BY-ITS-DECLARED-NAME-NOT-ITS-DOCUMENT-STEM]] (production, BORN CLOSED).

`src/dss-config/sources/c.lang.json` declares `language.name = "C"`. The file the config tree is INDEXED
by is `c.lang.json`. Lane E's nested runtime build passed the DECLARED name as its `--language` argument,
so it resolved `sources/C.lang.json`.

```
Windows  (NTFS,  case-insensitive)   1656 / 1656   GREEN
macOS    (APFS,  case-insensitive)   1656 / 1656   GREEN
WSL      (ext4,  case-SENSITIVE)     1111 / 1638   527 FAILED
arm64VPS (ext4,  case-SENSITIVE)     1111 / 1638   527 FAILED
```

★★★ **TWO OF THIS PROJECT'S THREE GATE HOSTS ARE STRUCTURALLY INCAPABLE OF OBSERVING THIS DEFECT CLASS,
AND SO IS `fs::exists`.** A case-only divergence is invisible to every question asked of a
case-insensitive filesystem. ⇒ **the pin compares STRINGS and asks the filesystem nothing** — a
`std::string` comparison is case-sensitive on every host there is, which is the only reason the witness
means the same thing on all four.

⚠ **AND THE TWO LINUX HOSTS AGREED TO THE TEST — 527 of 1638, twice, independently.** That is what turned
"a leg is red" into "the tree is wrong": a single red leg is a host until a second host reproduces it.

★★ **THE PIN FOUND A SECOND SITE THE FIX HAD MISSED, AND THE ASYMMETRY IS THE REASON.** With the
`--language` argument corrected, the key still read `doc=language:sources/C.lang.json` from a different
call site. It was harmless in substance — its digest was right, and a duplicate `unit-language` term
carried the correct spelling — but it is the same defect wearing a smaller consequence. ⓘ It surfaced ONLY
because the pin asserts the **absence of the wrong spelling anywhere in the key** rather than the
**presence of the right one in the term under repair**; the presence-only form would have passed and the
second site would have shipped.

★ **THE FIX IS THE CLASS, NOT THE TWO INSTANCES.** `GrammarSchema::configName()` now returns the
`.lang.json` stem, derived from the document's FILENAME rather than from the entry point — `loadShipped`
knows the stem, `loadFromFile` does not go through it, and both share one memo entry, so deriving it in
`loadShipped` would make the answer depend on which call populated the memo first. It is EMPTY for an
inline grammar and the caller **refuses rather than falling back to `name()`**: that substitution is how
the defect arose, so it is the one thing the fix must not offer.

### ⚠ AND THE ORCHESTRATOR REPEATED A TRAP THIS FILE ALREADY DOCUMENTS, VERBATIM

All three leg invocations were written as `… > log 2>&1; echo "LEG_RC=$?"`, so **the echo became the
terminal state and every leg reported exit 0** — including the arm64 VPS run that had just lost 527
tests. ✔This is the P31 finding word for word (*"never append a command after `run-gate.sh`"*), reached
by a different route: not after `run-gate.sh` this time, but after a background invocation whose exit
code the harness reports.

★ **It cost nothing only because the logs were read instead of the exit codes** — the leg scripts
themselves were correct throughout and printed `[X] remote-leg: arm64-vps leg failed (rc=8)` plainly.
⇒ the rule generalizes: **never append anything after a command whose exit status is the measurement**,
and a notification's exit code is worth less than the log it points at.

### ★★★ THE END-OF-CYCLE BENCHMARK — THREE HOSTS, AND ONE CLAIM THAT ONLY THREE HOSTS COULD SETTLE

**Operator instruction, verbatim:** *"compare in this host against gcc and msvc the sqlite's speedtest1, in
linux/macos also the sqlite's speedtest1 but gcc and clang compile/execute time and update readme"*, then
*"we already have the benchmark inside here real-examples\c\sqlite, just a matter to adapt it"* — which was
correct, and is why nothing new was written.

✔MEASURED — `test/speedtest1.c` over the same **103 full-source TUs**, cold builds (fresh object dir per
repeat), median of 3; runs median of 5:

```
host                     arm            -j1      -j4      run      1->4 scaling
Windows 11 x86_64 (32c)  dsscp         64.54    36.16    3.485     1.78x
  upstream 6f1110c       gcc 13.2.0    26.71     7.38    2.472     3.62x
                         MSVC          13.74     4.50    3.094     3.05x
Linux x86_64 WSL2 (32c)  dsscp         40.40    22.02    3.086     1.83x
  upstream 93f6407070    gcc 13.3.0    17.65     4.94    2.177     3.57x
                         clang 18.1.3  14.73     4.04    2.160     3.65x
macOS arm64 (10c)        dsscp         36.63    17.94    1.396     2.04x
  upstream 55bf04a530    Apple clang   10.69     2.96    0.784     3.61x
```

⚠ **THREE DIFFERENT UPSTREAM REVISIONS AND ONE HOST WITH A THIRD OF THE CORES ⇒ DO NOT READ ACROSS THE
TABLES.** Within a host every arm compiled the same source on the same machine; between hosts nothing is
claimed, and the README says so twice.

★★★ **THE DELIVERABLE IS NOT THE NUMBERS, IT IS THE CLAIM THEY SETTLE.**
[[D-PERF-CU-POOL-SCALES-HALF-AS-WELL-AS-SEPARATE-PROCESSES]] was a one-host observation, and one host
cannot separate *"our pool scales badly"* from *"this host schedules badly"*. ✔It now reproduces on
**three operating systems, four toolchains and two ISAs**: dsscp 1.78–2.04× where every reference is
3.05–3.65×. **The answer is the pool.** By Amdahl that is roughly a third of a full-source release build
on a serial path, and it is now the largest addressable item in DSS's compile-time story — far larger
than anything this cycle touched.

⚠ **THIS CYCLE'S OWN WINS DO NOT APPEAR HERE, AND THAT IS THE CORRECT RESULT RATHER THAN A DISAPPOINTMENT.**
The floor fell 128.9 → 93.5 ms (27% of a one-file compile) and moved these numbers by **less than their
run-to-run spread**, because a 103-TU build pays a fixed floor ONCE. ✔The control says the hosts were
comparable: gcc's Windows run time reads **2.472 s** against the README's **2.473 s** from 2026-08-21.
★ **Compile time has TWO problems — a per-invocation floor and per-CU throughput — and each needs its own
instrument. `compile-bench` sees the first; this sees the second.**

### ⚠ THE BENCHMARK COULD NOT HAVE ANSWERED THE QUESTION AS IT STOOD — THREE DEFECTS, ALL BORN CLOSED

1. [[D-HARNESS-SPEEDTEST1-BENCH-MEASURES-ONLY-THE-FIRST-REFERENCE-COMPILER-IT-FINDS]] — the reference was
   `command -v gcc || command -v clang`, so on a host carrying both, gcc always won the `||` and **the
   clang arm could never appear under any host configuration**. The operator asked for clang; the tool
   was structurally unable to produce it.
2. [[D-HARNESS-SPEEDTEST1-BENCH-ASKS-make-FOR-A-TARGET-SPELLED-THE-POSIX-WAY]] — `sqlite3d` where upstream
   declares `sqlite3d$(T.exe)`. On Windows that matched no rule and make answered `Nothing to be done`
   **with exit status 0 and an empty recipe**. ★ A recurrence of
   [[D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING]] one layer up.
3. ★★ **AND THE macOS PROBE CAUGHT THE SHARPEST ONE BEFORE IT COULD BE PUBLISHED.** `/usr/bin/gcc` and
   `/usr/bin/clang` are DISTINCT FILES that both report `Apple clang version 21.0.0` — so the resolved-path
   dedupe passed both, and because candidates are tried `gcc` first, **the README would have gained a row
   reading "gcc 21.0.0", a version gcc has never had, over a measurement of Apple clang.** ⇒ dedupe on the
   VERSION LINE, and take the LABEL from what the compiler says it is rather than from what we typed:
   `command -v gcc` answers WHERE a name resolves, never WHAT lives there. Same principle as (2), which is
   why they are one edit apart in the same file.

### ⚠ FOUR TRAPS THIS CYCLE WALKED INTO, ALL OF THEM ALREADY WRITTEN DOWN

1. **A quoted heredoc ate a backslash AGAIN** — `\n` inside a C string literal became a real newline
   and the file did not compile. Fourth relapse. ⇒ content with a backslash goes through the Write
   tool, never a heredoc.
2. **`wsl.exe -e bash /tmp/x.sh` from Git Bash SILENTLY DID NOTHING AND EXITED 0** — MSYS rewrote the
   argument to `C:/Users/.../Temp/x.sh`. ⇒ use `-lc '<literal starting with a word>'`, and note that
   `/tmp` in WSL does not survive a distro restart; put the script in `$HOME`.
3. **A registry row was inserted at end-of-FILE and landed in the two-column ALLOWLIST table**, where
   the renderer silently drops the trailing two cells. Caught by `anchor-registry`'s cell-width check.
   ⇒ the insertion point is the last row of the **table whose header has four columns**, found by
   locating that header — never "the last line in the file that looks like a row".
4. **A hand sanity-check of the WSL numbers read `0.00 s` for a compile that had FAILED** — it ran
   `dsscp` from `/tmp`, where config discovery cannot resolve, and the reading was one step from being
   repeated as "35× faster than gcc". ★ Lane C's instrument does not have this hole: it empties the
   run directory before every run and probes for the artifact after. **The tool was more careful than
   the person checking it.**

### ★ THE ORCHESTRATOR'S OWN BRIEFS WERE WRONG TWICE, AND BOTH WERE CAUGHT BY A LANE

- It told all four lanes a registry row has **six** pipe-delimited cells. ✔It has **four** — histogram
  over every real row: production {4: 1313}, harness {4: 572}. Lane C measured the header instead of
  trusting the brief and said so.
- It briefed lane A to design a config cache **that already existed**, finished and measured, in the
  P34 lane-SEAL worktree — and the design it briefed had a correctness hole SEAL had already closed
  (`languageReferences` means a host document's digest says nothing about its dependency). ⇒ **Step 0
  means reading the handoff to the END, not the first screen of it.**

### THE PAUSE GATE P34 LEFT, DISCHARGED BY MEASUREMENT

P34 deferred the memo for an operator ruling: three tests obtain "two schemas" by loading one document
twice, which the memo collapses. ✔Lane A measured the predicate FALSE — all 15 `schemaId()` sites in
`src/` are a dedup, a map key or an ownership test, and every one becomes MORE correct when duplicates
collapse; no emitter reads a schema id, so it cannot reach artifact bytes. **The only four observers
anywhere were tests, and all four were STRENGTHENED rather than relaxed** — two now load under two
distinct `sourceLabel`s (a key term) and keep their assertions verbatim, the death arm GAINING an
`ASSERT_NE` it never had so it cannot go vacuously green; two changed subject to two DIFFERENT
documents, the property the id must actually carry and one neither old form exercised.
★ Flagged for operator VETO rather than treated as settled — per the §B-predicate rule, a lane may
discharge a gate by measuring its predicate false **provided the measurement is recorded and the cycle
report says so.**

### ★★★ NEXT — in order, and item 1 changed underneath this block when lane E landed

⚠ **THIS BLOCK WAS WRITTEN BEFORE LANE E AND ITS FIRST ITEM SAID WINDOWS WAS "the ONLY host where DSS
loses" WITH AN 88 ms DEFICIT. THAT IS NO LONGER TRUE** — the small-compile ratio is **1.03×** and the
multi-TU ratios are **0.47× / 0.23×**. Corrected rather than left standing, because the stale form would
have sent the next cycle hunting a deficit that no longer exists.

1. **ATTRIBUTE WHAT IS LEFT OF `[other]`, BUT AS A THROUGHPUT QUESTION, NOT A DEFICIT.** ✔MEASURED that
   it is NOT config (split out and named this cycle) and NOT pre-`main` DLL load (`--version` is ~7 ms,
   and `--time` starts inside `main`). ★ The one place a reference is still clearly ahead is the
   **`large` RELEASE arm — dsscp 282.9 ms vs gcc -O2 115.0 ms (2.46×)** on a 4224-line switch-dense TU,
   and note dsscp's own DEFAULT arm runs that subject in 221.9 ms, so **DSS's release pipeline is paying
   61 ms for that file and needs to show what it buys.** That is a concrete, reproducible subject —
   prefer it over a general hunt. The method is the one that just worked: **extend phase coverage over
   the driver's own non-pipeline work** — CLI parse, artifact write, teardown — until `[other]` is
   genuinely unaccounted rather than a lump. ⚠ **Instrument first, patch never-on-suspicion**; this cycle
   produced four refuted hypotheses (mine: "quadratic in successors"; mine: "stat-validated cache"; the
   tree's: "config load is I/O-dominated"; and the brief I gave lane E: "the realized runtime units reach
   the link") and every one died on a direct measurement.
2. **Re-verdict [[D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX]] against the small-compile number.**
   Its ~1.2× residual was taken on a 103-TU build that amortizes a per-invocation floor away, so it is
   answering a different question and should say which. ⚠ **DO NOT CARRY THE "~5×" FIGURE AN EARLIER
   DRAFT OF THIS BLOCK USED** — it descends from the host attribution this cycle refuted. The honest
   Windows-vs-WSL same-source gap is now **93.5 ms vs 41.7 ms**, and ⚠ **even that is not a real number
   yet, because the WSL half is a PRE-fix binary and no post-fix WSL reading has been taken.** Take that
   reading before re-verdicting anything.
3. **[[D-PERF-CU-POOL-SCALES-HALF-AS-WELL-AS-SEPARATE-PROCESSES]] is now one step from attributable.**
   Its stated closing work is *"attribute the serial ~36%"* and its stated method is *"instrument first"*.
   ✔This cycle removed config from the suspect list; ⚠ `[other]` still grows **~+8.9 ms per TU** and
   nothing names it. That per-CU residue and this row are plausibly the same defect — 🧠INFERRED, and it
   is exactly the kind of guess item 1's instrument would settle.
4. **The `loadFromText` half of the config memo, DELIBERATELY NOT DONE.** ✔Its own cost is ~5 ms once per
   distinct document per process, and doing it breaks THREE more test sites that obtain "two schemas" by
   loading identical bytes twice — `TargetSchemaContentDigest.SameTextTwiceYieldsTheSameDigest`,
   `ObjectFormatSchemaContentDigest.SameTextTwiceYieldsTheSameDigest`, and the ~54-call-site mutant
   battery in `tests/test_support/mutate_target_schema.hpp`. It wants its own cycle and its own gate, not
   a drive-by inside a 400-line change.
5. **`Mir::isBlockAddressTaken` is still O(function) per call** (`src/mir/mir.cpp`). ✔SimplifyCfg was its
   only caller in `src/`, so the compiler-wide cost is retired — but **a future caller re-acquires the
   quadratic**, and the guard against that is this sentence rather than any test.

## 0.0000000000000000000000000000000000000 ★★★ CYCLE P34 — FOUR LANES, THREE ROWS CLOSED PLUS THREE BORN CLOSED, ZERO OPENED, AND THE REGISTRY SPLIT IN TWO

### ★★★ THE macOS LEG WAS MEASURING A TREE THAT EXISTS NOWHERE — three carriage defects, all BORN CLOSED

⚠ **This is the part of P34 a re-reader most needs, because the symptom pointed at an innocent
subject.** `plan_citations_guard` failed on macOS while the identical guard was `rc=0` locally. It
was not the guard, and it was not the code.

- ✔**MEASURED: the Mac held 16,312 files against a local 6,660.** `ssh-macos --push` is a `tar`
  extract and **tar extraction never deletes**, so the remote checkout was the UNION of every tree
  ever pushed — including `.plans/_deferred-anchor-registry.md`, deleted locally in THIS cycle and
  still sitting there at 6.7 MB. The guard counted **4908 citations across 213 documents** where the
  live tree has **2853 across 212**. The arithmetic closes exactly: 2853 + that file's 2054.
  ⇒ [[D-SCRIPT-REMOTE-PUSH-ACCUMULATES-AND-SHIPS-AGENT-WORKTREES]].
- ✔**MEASURED: 9,638 of those files were `.claude/worktrees/**`** — a full copy of the repo per live
  agent, shipped to the gate host every push. The examples runner **globs `examples/<lang>/*`**, so a
  gate host holding a worktree can run somebody's uncommitted corpus and report it as the cycle's.
- ✔**MEASURED: two legs ran against one `build/dbg`,** and the second's `rm -rf` ran underneath the
  first's live `ctest`, which then spent **2 h 34 m** walking a tree being rebuilt under it. Both
  tee'd to one log; `tee` truncates on open but each writer keeps its OWN offset, so a killed leg's
  `REMOTE_CTEST_RC=143` sat at ~2 MB while the live leg wrote from ~0 — and the witness was
  `grep … | tail -1`. ★★ **The dangerous direction is the mirror image: a stale `=0` outliving a live
  failure is a FALSE GREEN by the identical mechanism.** ✔The splice proven at byte level, not
  inferred: one `ctest.log` held **467 rows saying `/1650` and 4 saying `/1635`**.
  ⇒ [[D-SCRIPT-MACOS-LEG-WITNESS-CAN-BE-ANOTHER-RUN-S-EXIT-CODE]].
- ✔**MEASURED: the leg ran ctest SERIALLY** — 674 of 1650 entries in 22 minutes. This REGRESSED
  [[D-SCRIPT-REMOTE-LEG-CTEST-TAKES-THE-REMOTE-SERIAL-DEFAULT]], closed 2026-08-21; `macos-leg` was
  written on 2026-08-25, four days later, and never carried it. ★★ **A fix that lives in one script
  is not a fix of the class** — a closed row cannot see a file that did not exist when it closed.
  ⇒ [[D-SCRIPT-MACOS-LEG-REGRESSED-THE-SERIAL-CTEST-FIX]].

★★★ **OPERATOR RULING 2026-08-25, and it corrected this orchestrator mid-cycle:** *"default for
testing is -j4 to not use 100% of the machine on our tests"*. The first draft of the parallelism fix
copied `remote-leg.sh`'s precedent of probing the remote core count, which ✔answers **10** on this
Mac — it would have claimed the whole machine. **The probe answered the wrong question**: not *how
many cores exist* but *how many may a guest take*. ⓘ `local-build`/`run-gate` keep their documented
**8** (a 32-core workstation deliberately running the Windows and WSL legs at once) — recorded as a
decision rather than silently matched.

★★★ **OPERATOR RULING 2026-08-25 — A GATE HOST HOLDS THE REPO AND NOTHING ELSE:** *"keep macos and
vps linux arm64 updated with our repo files, and free of stale files/worktrees. you own the
cleanup."* Written into `.claude/skills/dss-cycle/SKILL.md`. ⚠ It NARROWS but does not repeal the
standing order against cleaning those hosts: no `git clean`, no `reset --hard`, no `checkout --`
unless the operator names it. What is authorised is removing what the repo does not have.
✔**EXERCISED on the real host, not read:** a dry run (`comm -13` of both file lists) PREDICTED 9,652
removals; the run reported **`PRUNED=9652 FRESH=6660`** and the Mac's post-prune count is **6660 —
exactly the local tree**. ⓵ A second pass was owed and caught by looking: `find -delete` removes
FILES, so hollow `.claude/worktrees/agent-*` DIRECTORIES survived and the host still *read* as
holding worktrees; an empty-directory sweep now follows.

### ★★★ TWO PARALLELISM RULINGS, AND A BUILD-SPEED INVESTIGATION WHOSE PREMISE WAS FALSE

**RULING 1 — guards run on the ROOT host only.** *"why do we even run the guards in the legs?"*
… *"the root is whichever the host is using you (claude). every indirect leg (wsl, vps, macos,
whatever), do not need guards!"* A repo guard checks the SOURCE TREE, and the tree is
byte-identical on every leg, so every leg after the first re-derived a result it could not
change. ✔MEASURED: **18 guard entries, 159.1 s** on Windows, 84% of it in three entries
(`plan_citations_guard` 80.93 s, `anchor_registry_guard` 37.18 s, `orphan_tests_guard` 15.84 s);
on macOS those three had not finished after **367 s**. Now labelled `repo-guard`; indirect legs
pass `-LE repo-guard`. ✔EXERCISED: `ctest -N` **1650 → 1632**, exactly 18 dropped.
⚠ THE COVERAGE THIS COSTS: `CMakeLists` dispatches on `WIN32`, so the `.sh` guard twins are now
exercised **in CI only**. ⇒ [[D-GATE-EVERY-LEG-RE-RAN-THE-REPO-GUARDS-OVER-AN-IDENTICAL-TREE]].

**RULING 2 — six cores, everywhere, build and test.** *"never use all CPUS, the idea is to keep
build + tests + run always at 4 cpus"*, amended same-day to *"make it 6 cores, not 4,
everywhere!"* ✔MEASURED while applying it: **five sites were building with NO limit at all** —
a bare `cmake --build` hands off to ninja, whose default is all cores — including **both CI
workflows**, while every parallelism discussion in this repo's history had been about ctest.
26 edits, 11 files, one spelling (`${DSS_JOBS:-6}`). The `getconf _NPROCESSORS_ONLN` probe is
DELETED, not clamped: it answered *how many cores exist* when the question is *how many may a
guest take*, and it made the level differ per host (macOS 10, VPS 4) so no leg was comparable to
another. ⇒ [[D-BUILD-PARALLELISM-WAS-UNBOUNDED-AT-FIVE-SITES-INCLUDING-BOTH-CI-WORKFLOWS]].

**★★★ AND THE BUILD WAS NEVER SLOW.** The whole build-speed arc rested on *"4780 s summed build
CPU, 64.7% of it tests"*, derived by summing `.ninja_log` durations. **A `.ninja_log` duration is
WALL TIME UNDER WHATEVER `-j` THAT BUILD RAN AT** — summing it counts contention as cost.
✔MEASURED: a **clean build of all 838 targets at `-j6` takes 394 s**; the summed model claimed
4281 s, overstating the work ~1.8×, exactly the SMT factor on this 16-physical/32-logical box.
★★ **THREE THEORIES DIED ON DIRECT MEASUREMENT, EACH ONE MINE:** *header volume* — ✔the four
biggest headers are **78–94% comment** (`parse_diagnostic.hpp` 313 KB → **18.9 KB of code**), and
comments are stripped before parsing; *`-gsplit-dwarf`* — ✔**slower**, 18.27 s vs 15.84 s;
*memory-bound* — ✔that multiplied one TU's peak by 32 assuming simultaneous peaks, and the 2.3×
per-job slowdown it rested on is just SMT at full occupancy. A fourth died on structure:
**UNITY_BUILD is inapplicable**, since ✔325 test `.cpp` produce **325 executables, one source per
target**, and unity merges sources WITHIN a target. PCH is already enabled for every test target;
link is **3.3%**. ⇒ [[D-INSTRUMENT-NINJA-LOG-TIMES-ARE-CONTENDED-WALL-CLOCK-NOT-CPU-COST]].

★ **WHAT SURVIVED AND IS STILL TRUE:** **57.0 MB of 70.6 MB COMDAT emission is emitted then
discarded (80.7%)** across 45,349 symbols in ≥2 objects — but it is a LONG TAIL (the top 25 are
~4 MB), inherent to 325 separate `-O0 -g` programs, with no single `extern template` that moves
it. ⚠ The two levers that WOULD move it were put to the operator and **rejected by name** —
fewer test executables (costs the 1650-entry attribution) and a lower debug level (✔`-g0` 10.36 s
vs `-g` 15.84 s, 35% of a TU) — on the grounds that *"we are looking for real compile time gain,
not shortcuts"*. Both are withdrawn, not deferred.

★★ **THE REUSABLE LESSON, and it is the reason this is in the handoff rather than only in a row:
an instrument that reports TIME UNDER LOAD is reporting the load, not the work — and it will rank
things correctly while getting the magnitude, and therefore the decision, wrong.** Ranking the
slowest targets from `.ninja_log` was and remains valid. Totalling them into a budget was not,
and four hypotheses were built on that total before anyone timed a build.

### ★★★ THE FIRST GATE WHERE ALL FOUR LEGS FINISHED, AND BOTH NEW REDS WERE HOST-DEPENDENCE

Root **1650/1650** and WSL **1632/1632** were green. The macOS and arm64 legs each produced a
failure that **no x86_64 host can see**, and neither was caused by this cycle's changes
(✔`grammar_schema_json.cpp` is not among the 30 changed `src/` files; ✔the `integrated_tests/runner.cpp`
diff is four COMMENT lines renaming the split registry).

**macOS — four LSP binaries died `Bus error`.** ✔The crash report named it rather than leaving it to
inference: `EXC_BAD_ACCESS (SIGBUS)`, **"Thread stack size exceeded"**, innermost frame
`___chkstk_darwin`, on a thread whose outermost frames are `__thread_proxy` / `_pthread_start` — a
`std::thread`, not the existing `runOnLargeStack` worker. ★★ **AND IT WAS NOT DEEP RECURSION, WHICH
IS WHAT IT LOOKED LIKE**: ✔39 frames, ~10 of them `dss::detail::`. ✔`-Wframe-larger-than` on the
failing host: **`buildSchemaFromJsonText` = 415,360 bytes IN ONE FRAME** under `clang -O0` (**31,568**
under `gcc` — 13×, and the whole reason no Linux leg saw it). 405 KiB of macOS's 512 KiB default
secondary-thread stack. ⇒ [[D-TEST-LSP-HARNESS-RAN-THE-SERVER-LOOP-ON-A-HOST-DEFAULT-STACK]].

**arm64 VPS — `integrated_tests/coverage-boundary` refused, correctly.** `selectCoverageBoundarySubject`
called a target host-bindable when `runOn` listed the host's **OS**; `runOn` names operating systems
only, so nothing consulted the **processor**. ✔On aarch64 it selected `asm/asm_arith_return42` — both
specs x86_64 — so both runners reported `ran=-` and clause C5 refused over an empty overlap. ★ **The
guard was working; the selector handed it a vacuous subject.** ⇒
[[D-TEST-COVERAGE-BOUNDARY-SELECTOR-READ-THE-HOST-OS-BUT-NOT-THE-HOST-ARCH]].

★★★ **THE SHARED MECHANISM, WHICH IS WHY THESE TWO ARE IN ONE BLOCK: BOTH READ A HOST PROPERTY
THROUGH A PROXY THAT HAPPENS TO CORRELATE ON THE MACHINE THE WORK WAS DONE ON.** "a thread has enough
stack" and "the OS admits this target" are both true-by-accident on x86_64 Linux and Windows. Neither
is a wrong line of code you could find by reading it; both are assumptions that only a differently-
shaped host can falsify. ⇒ **the value of a leg is the assumptions it can break, not the tests it
re-passes** — and until this gate, the macOS leg had NEVER reached the LSP block (✔`grep -c` on the
prior run's log: **0**) and no arm64 host had run the corpus selector.

⚠ **WHAT WAS DELIBERATELY *NOT* CHANGED, each with its measurement.** (1) **Production was never
exposed** by the LSP defect, and the first reading of it said otherwise — ✔the schema resolve happens
in `resolveSchemaForUri_` on the server-loop thread BEFORE `enqueueParse_` submits, and `runLspMode`
runs that loop on MAIN. (2) **`ThreadPool` keeps the host default**: its workers are unmeasured here
and ✔`test_compile_pipeline` exercises a real `ThreadPool{4}` green on macOS — changing it would be
speculative churn in the middle of a gate. (3) **The 13,739-line `buildSchemaFromJsonText`
(lines 4353–18091) is a REAL, SEPARATE defect and is NOT closed by either row.** It is what makes the
frame 405 KiB at `-O0`; with the stack now stated it is no longer a CORRECTNESS problem, which is
precisely why it should be picked deliberately rather than smuggled into a gate fix.

★ **New shared primitive:** `substrate::StackSizedThread` — the ONE place that knows how to ask a host
for a thread with a chosen stack size — and `runOnLargeStack` is now implemented **on** it rather than
beside it, so the `_WIN32`/POSIX arms cannot drift. ✔RED-ON-DISABLE EXERCISED: the test asks for 64 MiB
and TOUCHES 16 MiB (above **every** host default, so it discriminates on all three); the mutant that
ignores the stated size was built and RUN — `***Exception: SegFault`, rc=8, subject md5 `29994502…` →
`514f8c9a…`. ⚠ **md5 is NOT a valid restoration witness on this subject** — every build embeds a
recomputed `DSS_BUILD_STAMP`, so the restored binary differs at identical source; restoration was
verified by source content instead. That refines the standing rule rather than contradicting it.

### ⏭ DEFERRED INTO THE NEXT CYCLE, DELIBERATELY — the config-load memo (lane SEAL)

Lane SEAL's work is COMPLETE and MEASURED but is **not in this commit**. It lives in the worktree
`.claude/worktrees/agent-a40b841510933621d` (branch `worktree-agent-a40b841510933621d`), with its
build tree at `C:/Source/DailySoftware/dss-seal-build`. **Do not remove either until it lands.**

★★ **It REFUTED its own brief, which is the control loop working.** The brief blamed the schema
ctor's sealing block for ~540 ms; ✔MEASURED, sealing is **1.0 ms of a 62.9 ms load (1.6%)** and
`sealAltBranchRules` alone is **0.4 ms**, with the load *sub*-linear in bytes. The real cost is
**redundant loads**: one `dsscp --compile` of a one-line `.c` runs **18 config loads for 8 distinct
documents, 312.3 ms of a 647 ms process**, because `program.cpp`'s extension→language resolver fully
builds six grammars **twice** just to read `fileExtensions()`. Fixed by a content-addressed memo keyed
on **(schema family, sourceLabel, SHA-256 of the bytes)** — the digest the loader already computes —
with a **dependency ledger** re-read on every lookup, because `c.lang.json` + an EDITED
`asm.lang.json` digest identically and that would have been a silent miscompile.
AFTER: `semantic_analyzer_c` **59.5 s → 12.4 s**, `hir_lowering_c` 29.5 → 11.4 s, one-line C compile
603 → 498 ms.

⚠ **It carries a real PAUSE GATE and that is why it is not folded here:** three existing tests
(`GrammarSchemaContentDigest.SameTextTwiceYieldsTheSameDigest`,
`TsqlSubset.SchemaIdsAreDistinctPerLoad`, `StringStyleDeath.CrossSchemaStringStyleLookupAborts`)
obtain "two schemas" by loading the **same document twice** — the exact redundancy the memo removes.
The invariant they protect is preserved (two *different* documents still get different ids); what
changed is that two loads of an *identical* document are now one schema. **Ruling needed** before
those three are edited.


**Gate: 1016 → 1013, `closed 3, opened 0`, `anchor-balance: OK`.** Four lanes, disjoint file
sets, three in `git worktree`s so only one lane held `src/dss-config/**` in the main tree.
★ **Third consecutive cycle with ZERO rows opened.**

### ★★★ THE REGISTRY IS NOW TWO FILES — operator instruction, agreed and applied

> *"we need to split `.plans/_deferred-anchor-registry.md` into production issues and tools/harness
> issues. that's killing me. 2 deferred anchor registry files, one for prod, other for tool/harness.
> please. do it also, the priority is real errors, not cosmetics"*

| file | rows | **OPEN** |
|---|---|---|
| `_deferred-anchor-registry-production.md` | 1357 | **495** |
| `_deferred-anchor-registry-harness.md` | 574 | **194** |

**THE LINE, so a borderline row is decided by a rule and not by taste:** PRODUCTION is a defect a
USER OF THE COMPILER could hit — in the shipped binary or the config it reads. HARNESS is a defect
only WE can hit — tests, gates, guards, cycle machinery, plans, scripts, carriages, CI.
⚠ `D-CONFIG-*` (74) and `D-DIAG-*` (49) are PRODUCTION deliberately: a `.lang/.target/.format.json`
document IS the compiler's behaviour in this architecture, and a diagnostic is its output to a user.
⚠ A row's bucket follows the **DEFECT**, never the instrument that found it.
★ Every consumer **globs `_deferred-anchor-registry*.md`**, so a third split costs nothing.

### ⚠⚠ THE SPLIT'S OWN VERIFICATION WAS WRONG TWICE, AND BOTH WERE CAUGHT BY MEASURING TOTALS

1. **It verified ROWS and lost PROSE.** The first output dropped the 14-line intro that lives
   BETWEEN `## Anchor Index` and its table — the authoring rules: *"EXACTLY four cells"*, *"a raw
   `\|` splits a cell EVEN INSIDE BACKTICKS"*, the ` ═══ ` seam convention. ★ Losing the guidance
   that PREVENTS malformed rows, while splitting the file into two places where rows get written,
   is the worst possible thing to drop. ⚠ Noticed only because the two outputs summed to 48 KB LESS
   than the input — **a byte total caught what a row check could not see**.
2. **It required 4 cells and silently skipped 37 legal 2-cell rows.** The registry's own prose says
   they are legal (*"A row carrying only its first two cells renders with the trailing two columns
   blank — that is correct and loses nothing"*). ★★ **A partition that drops rows it does not
   recognise is not a partition; it is a filter wearing one's clothes.** The script now reconciles
   row-line TOTALS and fails on any delta.
★ The `## Anchor Index (continued)` section is **retired rather than replicated**: its own prose
records that it existed only because rows were appended at end-of-FILE instead of end-of-TABLE,
under a 2-column header that silently ate their trailing cells. Each output now has exactly ONE
index, so the accident cannot recur — and the RULE it teaches is carried into both intros.

### ★ ONE GUARD NEEDED ONE FUNCTION, NOT A MIGRATION
✔MEASURED before writing any of it: `check-anchor-balance` already enumerates every `.md` under
`.plans/`; `check-anchor-registry.{sh,ps1}` already resolve a `src/` citation against ANY
`.plans/*.md`; `check-stale-refusal-citations` excludes `.plans/` outright. Four edits total.
⚠ **But the balance guard keyed rows as `path#anchor`, so the split read as `closed 692, opened
689`** — 1400 rows of phantom churn that would have buried any real movement. Fixed at the root:
a registry row's identity is its ANCHOR ID, and which file holds it is a filing decision.
`row_key()` canonicalises every registry path to one key, which also makes MOVING a row between the
two buckets correctly a no-op.

### ★★ WAVE 1 — 3 CLOSED, 3 BORN CLOSED, 1 NARROWED, 1 HALF-DONE, **0 OPENED**
- **Lane A** — `D-CSUBSET-VLA-INITIALIZER` + `D-DIAG-ARRAY-SUFFIX-REPORTS-ONLY-THE-LEXEME` +
  `D-HIR-SENTINEL-ARRAY-LENGTH-EXPANDED-AS-A-COUNT` (all born closed) and
  `D-CSUBSET-VLA-PTR-INIT-FORM-TYPING` (closed as a free side effect, its recorded root cause
  measured WRONG). ★★ **It found a PRE-EXISTING COMPILER CRASH**: `struct S { int n; char c[]; };
  struct S s = {};` — which gcc and clang both compile — died with `std::length_error`, and the VLA
  form HUNG. Four sites cast a SIGNED sentinel length (-1 / -2) to an unsigned count.
  ⚠ It was invisible because **DSS refused the input that reaches it** — fixing a conformance
  refusal is what exposed the crash behind it.
- **Lane B** — `D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES`. The config already held the
  discriminator (slot width vs `guard.width`); **zero new schema keys, and two hardcoded literals
  DELETED**. Found and fixed a sibling defect the row never named (`movb $-1, %al`).
- **Lane C** — the Mach-O x86_64 half of the merged-foreign-unwind carry; the discriminating-field
  ruling implemented as given, **no new `SectionKind`**. Runs on the operator's Mac, exit 42, with
  Apple's own `dwarfdump` reading the merged frames back. arm64 + PE remain, measured.
- **Lane D** — `D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME`. All three of the operator's items were
  ALREADY satisfied — established by measurement, not assumption — and the undertaking is now
  **permanently unnecessary**: a 236-row measured oracle reds when ANY Mach-O-visible imported
  function lacks a measurement. ⚠ It found the real gap: the import surface grew **232 → 277**, so
  ~45 symbols entered the eager-import set with nobody asking the platform what it calls them.

### ★★★ macOS IS A GATE LEG FOR THE FIRST TIME
`scripts/macos-leg/` (both siblings). The carriage could not push a tree at all: `--rsync` execs the
LOCAL rsync and ✔Git Bash has none. New `--push` (tar over ssh) — and the 2026-08-18 note that
retired the tar transport (*"the login profile consumes stdin"*) is ✔RE-MEASURED FALSE: 1000 bytes
of `/dev/urandom` arrive with an identical md5.
⚠ **`nm -gU /usr/lib/libSystem.B.dylib` is dead on macOS 26** — the FILE does not exist (system
libraries moved into the dyld shared cache; `/usr/lib` retains 12 `.dylib` files). ✔`dlopen` on that
path still works, so the loader kept it and only the filesystem lost it. The replacement is an
ORDERED CHAIN, because the right instrument is a property of the HOST VERSION: SDK `.tbd` first (the
only tier that FLATTENS an umbrella — `libSystem` has **6** own exports; `_fstat$INODE64` lives in
`libsystem_kernel.dylib`'s 4756), then `dyld_info` on the sub-library, then the on-disk file for
macOS ≤ 10.15. ⛔ `dlsym` is NOT a tier: it resolves `fstat` and returns NULL for `fstat$INODE64`.
★ **DSS itself needed no change** — `macho_reader.cpp` already parses `LC_REEXPORT_DYLIB` and the
export-trie forwarder terminal. One grep would have established that before any of the investigation;
recorded in [[feedback_verify_before_asserting]] as *measure scope before BUILDING, not only before
asserting*.

### ⚠ FOUR PIPE-MASKED STATUSES IN TWO CYCLES — the recurring instrument failure
`run-gate` piped through `tail` (P33), the macOS driver piped through `tail` (twice, P34), and
`nm … | grep` reporting a false ABSENT because the PIPELINE exits 0 while `nm` exits 1. ⇒ the rule
*"never append a command after run-gate"* generalises to **anything that becomes the terminal exit
status**, and the durable answer is not discipline: every leg driver emits a witness and REFUSES
when it is absent.

## 0.000000000000000000000000000000000000 ★★★ CYCLE P33 — SEVEN ROWS CLOSED, ZERO OPENED, AND FOUR "OPERATOR DECISIONS" DISSOLVED BY MEASUREMENT

**Gate: registry 1023 → 1016, `closed 7, opened 0`, `anchor-balance: OK`.** Four wave-1 lanes,
disjoint file sets, two in `git worktree`s. ★ **Zero new rows across four lanes** — the second
consecutive cycle to meet [[feedback-close-do-not-file]], and this time three separate would-be
rows were dissolved by measurement rather than filed.

### ★★★ THE OPERATOR AMENDED THE BAR: `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C`

> *"if no reference like gcc, clang or msvc accepts something that iso C does accept, we must accept"*
> — and restated the same day: *"two ways: if any of references accept something, we must accept.
> if none accept but isoc accepts, we must accept. the idea is to work. we are just as permissive as
> our references + iso C"*

§A.3b was a DISJUNCTION over the reference roster. It is now a **UNION with the standard**, and it
binds BOTH ways: **below** the union is a conformance defect (refusing what works somewhere, or what
ISO requires); **above** it is an invented extension. Neither side is the safe one — a refusal breaks
real programs, an invention makes DSS the only compiler that takes a program, which nothing
downstream can warn about. Written into `README.md`, `.claude/skills/dss-code-prime/SKILL.md` and
[[feedback_reference_compilers_are_the_spec]] at operator request.

⚠ **ONE SHARP CONSEQUENCE, NOT OBVIOUS FROM THE WORDING, AND IT IS WAVE-2 WORK.**
`reference_conformance.probes` treats `@expect-ref accept` as an EXISTENTIAL claim: an unwitnessing
roster reports **`not-witnessed`**, never a failure. Under the new clause that verdict **can no
longer be read as "no obligation"** — a construct ISO C requires which no reference implements is
still required of DSS, and the harness is currently SILENT about exactly that case. The corpus needs
a distinct verdict for *"the standard requires it, the roster does not witness it, ship it anyway"*.

### ★★★ FOUR ASKS WENT UP TO THE OPERATOR. THREE SHOULD NEVER HAVE, AND MEASURING SAID SO

The operator's reply — *"as per my read, all of them are auto answerable, right?"* — was correct on
every one. What the re-measurement found is that **my own framing was wrong on two of the three**:

1. **`D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES` — NO CONFLICT EXISTED.** I reported the
   handoff and the row disagreeing about whether a ruling had been made. ✔They carry the SAME ruling;
   I had read the closing cell TRUNCATED AT 1500 CHARS and reported the tail fragment as an anomaly.
   ⇒ **A REGISTRY ROW IS ONE LINE OF MANY KB — cut the cell out (`tr '|' '\n' | sed -n '<N>p'`), never
   read a prefix of the line.** ★ And it was derivable without the ruling: (A) truncate silently
   breaks fail-loud, (B) keep refusing breaks §A.3b, so neither arm is defensible and it was never a
   real fork. The third arm is FORCED, because **the disjunction rule constrains what DSS must
   COMPILE, never what it must stay QUIET about.**
2. **`D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME` — the undertaking is a STAND-IN, not a requirement.**
   The row asks the operator to check an agent's report against three items. Its own words say why:
   *"a fix reported landed is not a fix measured landed"*. The standing orders already name what beats
   a report, and it is not a second pair of eyes — a MEASUREMENT plus a red-on-disable pin. Wave 2
   builds it, so no confirmation is ever owed again.
3. **THE `D-FF1-` RENAME — I PROPOSED IT AND IT WOULD HAVE BEEN DESTRUCTIVE.**
   `D-PLANS-AN-FFI-ROW-IS-NAMESPACED-D-FF1-WITH-A-DIGIT-...` calls `D-FF1-AR-BSD-VARIANT` a digit-for-
   letter typo of `D-FFI-`, sized at 15 sites. ✔MEASURED: **`FF1` is a DECLARED WORK-PACKAGE LABEL** —
   the plans spell the family `FF1 → FF2 → … → FF6`, 211 bare `FF1` references — and `D-FF1-*` is that
   package's namespace with **24 distinct ids** (`D-FF1-AR-READER`, `D-FF1-MACHO-FAT`, `D-FF1-PE-READER`,
   …), every one about ARCHIVE / Mach-O / PE **file formats**, none about FFI. True size **39 registry
   + 102 `src/` occurrences**, because the row measured ONE id and generalized. Renaming would have
   pulled a row out of its own namespace and orphaned it from 23 siblings.
   ⇒ close by refutation; keep candidate (c) with its rule INVERTED — derive the DECLARED namespace set
   from the plans' work-package labels and red on any `D-<NS>-` not in it. The hazard is HIGHER than the
   row thought: two confusable spellings that are both live and both correct.
4. **LANE C RAISED A FOURTH MID-CYCLE, AND IT DISSOLVED TOO** — see below.

### ★★ WAVE 1 — FOUR LANES: 7 CLOSED, 1 NARROWED, 1 NOT DONE, 0 OPENED

- **Lane A** — `D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED`. A per-run
  SNAPSHOT of `src/dss-config` (`cmake/DssConfigSnapshot.cmake`), `configRoot()` split from
  `repoRoot()`, `private_config_root.hpp` deleted. Its worktree gate 1624/1624.
- **Lane B** — `D-CSUBSET-TYPEOF-UNQUAL-GNU-SPELLING` · `D-C-PREPROCESSED-INPUT-REFUSES-GCC-LINEMARKERS`
  · `D-CSUBSET-COUNTER-MACRO-NOT-EXPANDED`. `__COUNTER__` is a new `counter` predefined-macro KIND —
  the only STATEFUL one, state owned by the engine, name still in config. NOT DONE:
  `D-CSUBSET-IMAGINARY-LITERAL-SUFFIX`, whose sequencing note it REFUTED on the way past.
- **Lane C** — `D-CSUBSET-ATTRIBUTE-DEPRECATED-TYPES` · `D-CSUBSET-CONSTEXPR-AGGREGATE-TYPE` ·
  `D-CSUBSET-CONSTEXPR-POINTER-CAST-NULL`.
- **Lane D** — `D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE` **NOT CLOSED, materially
  narrowed**: ELF now carries a merged foreign function's unwind description, witnessed by a running
  `backtrace()` walking through two gcc-built frames on both ELF legs (exit 42 vs 2, 4 FDEs vs 2, under
  a config mutant with NOTHING REBUILT). Mach-O and PE are the named remainder. It says NOT DONE plainly,
  which is the standard.

### ★★ LANE C'S "OPERATOR FORK" WAS REFUTED — AND DSS WAS WRONG IN BOTH DIRECTIONS AT ONCE

Lane C reported `int a[n] = {…}` as needing a ruling, on the premise *"gcc admits `int a[n] = {};`,
clang refuses even that"*. ✔RE-MEASURED, 4 cases × 3 compilers × 2 standards, two matched controls:

| case | gcc 13.3.0 | clang 19.1.1 | clang 18.1.3 | DSS |
|---|---|---|---|---|
| `int a[argc] = {1,2,3};` | REFUSE | REFUSE | REFUSE | **ACCEPT, `sizeof` 12** |
| `int a[argc] = {};` | ACCEPT | ACCEPT | ACCEPT | **REFUSE S000C** |

**The roster is UNANIMOUS on both forms.** clang does not refuse the empty form at either standard in
either version. ✔AND THE STANDARD SETTLES IT — N3220 §6.7.10p4: *"An entity of variable length array
type shall not be initialized except by an empty initializer."* ⇒ the non-empty form is a CONSTRAINT
VIOLATION, so the operator's new ISO clause does not engage; the empty form is obligated TWICE OVER.
★★ **ONE ROOT CAUSE, BOTH DIVERGENCES:** `resolveArrayLength`'s `allowFlexibleArray \|\| initNode.valid()`
arm is tested ABOVE the VLA arm, so a present-but-non-constant bound is read as ABSENT and the object
is re-sized from the brace list — 3 elements in the first case, 0 in the second. ★ A THIRD defect fell
out of the same probe: that `S000C` renders its message body as the literal `{}`.
★★ **THE GENERAL LESSON, WORTH MORE THAN THE ROW: under the amended bar a UNANIMOUS REFUSAL must be
checked against the standard TEXT before it is read as a gap — most unanimous refusals are a
constraint being obeyed.** Fixed in wave 2; NO ROW MINTED.

### ★★ LANE D'S ARCHITECTURAL FORK WAS RESOLVED, NOT FORWARDED — AND IT IS NOT A FORK

A Mach-O x86_64 object carries `__TEXT,__eh_frame` ALONGSIDE `__LD,__compact_unwind`, but the loader
enforces kind-uniqueness (`data.sectionKindIndex.emplace(info.kind, idx)` → *"duplicate section kind"*),
so a format cannot declare two `unwind` rows. Candidates: (a) a second `SectionKind` enumerator,
(b) a discriminating field on the section row.
⇒ **(b), and (a) is not defensible.** `section_kind.hpp`'s own docblock: the taxonomy is *"format-blind
names the substrate engine speaks"*, *"per-format JSON owns the name + flags"*. Compact-vs-DWARF is a
per-format ENCODING fact ⇒ an `UnwindCompact`/`UnwindDwarf` split re-encodes FORMAT IDENTITY into the
shared taxonomy, the hard veto. ⚠ The apparent precedent is not one: `ShStrtab` splits from `Strtab` on
CONSUMER PATH, and both concepts exist in EVERY format — an unwind encoding does not.

### ⚠ TWO DISCHARGES-BY-MEASUREMENT FOR OPERATOR VETO (§B predicate rule)

1. **Lane B, `D-CSUBSET-TYPEOF-UNQUAL-GNU-SPELLING`** — closed by SUPERSESSION, not by its trigger.
   ✔The roster has NOT moved (still 1-of-3); the **2026-08-19 §A.3b ruling retired the MAJORITY rule**
   the row rested on. ✅ Operator asked and answered the same day: *"the rule keeps. clang 19 accepts,
   right? and is more recent! so we must accept too!"*
2. **Lane D, `D-FF1-AR-BSD-CORPUS-EXAMPLE-NEEDS-A-PREBUILT-ARCHIVE-KEY-IN-BOTH-RUNNERS`** — predicate
   MEASURED FALSE: `prebuiltLibraries` shipped in P32 in BOTH runners and lane D's example uses it.

### ★ WHAT ELSE LANDED

- **`scripts/burndown-queue/burndown-queue.py`** promoted (33 scripts, both indexes regenerated). It
  REUSES `check-anchor-balance`'s row vocabulary; its negation sieve cut a naive 201-row P0 band to
  **109** and it PRINTS what it disbelieved and what it could not read.
- **The mixed-tree config residual CLOSED — 18 sites across 16 files**, driven by the composition site
  rather than a file-wide substitution, so `test_object_format_backend_registry.cpp`'s SECOND
  `findRepoRoot()` (a grep-shaped test over `src/`) correctly still reads the checkout.
- ⚠ `sed -i` **reported success and silently did not apply an edit**, caught only by re-measuring the
  cell count afterwards. And counting registry separators needs `total | minus grep -oF '\|'` — a BRE
  reads `\|` as ALTERNATION, matches empty, and cheerfully reports zero.

## 0.00000000000000000000000000000000000 ★★★ CYCLE P32 — THE FIRST NET-NEGATIVE CYCLE, AND THE OPERATOR HAD TO SAY SO

**Balance: 1032 → 1023 — closed 10, opened 1**, plus two rows filed BORN CLOSED (which move
neither tally, correctly). **Gate 1622/1622 on Windows x86_64**; suite 1603 → 1622.
⚠ RE-MEASURE, never re-quote: `python scripts/check-anchor-balance/check-anchor-balance.py`.

### ★★★ THE INSTRUCTION THAT CHANGED THE CYCLE, MID-CYCLE

Verbatim, on seeing this orchestrator FILE a row rather than fix it: *“and this?
D-GATE-NO-CTEST-ENTRY-SEES-THE-MULTI-HOST-CARRIAGE-SCRIPTS... I HATE your way to do things...
every time opens more anchors than closes, seems incapable of solving problems.... it's getting
me on my nerves”*.

✔The complaint is MEASURED, not a mood: P31 closed **5 earned** rows against **18** opened.
And the trigger was the worst possible instance — the orchestrator had just BROKEN the
multi-host carriage with its own rename, and its response was to write a row about it.

**The standing order that follows, and it belongs in every lane brief:**
- **A new row is a LAST RESORT, not a deliverable.** Fixable inside owned paths without breaking
  the bar ⇒ **FIX IT**, and record it inside a row already being closed.
- **Open a row only when all three hold:** outside owned paths, needs an operator decision or
  another subsystem, AND leaving it unrecorded loses a measured fact. Say which.
- **“Refused but not fixed” is NOT closed** and is not an acceptable resting state for
  anything the cycle was asked to close. Report it as NOT DONE with what the real fix needs.
- ★ It does NOT repeal *anchor every issue found*. What changed is WHERE the fact is
  recorded — inside the row you are closing, not as a new open one.

✔**IT WORKED, MEASURABLY:** the carriage row was taken by the orchestrator and closed the same
day; the four lanes launched afterwards opened **ZERO** rows between them while closing eight, and
three of them fixed additional defects they found and wrote them into the row they were already
closing.

### ★★ THE RENAME LANDED — AND OVER-REACHED BEHIND A GREEN SUITE

`ff9746f0`, its own commit on a clean tree. Width **COMMAND IDENTITY** (operator-ruled): `dsscp`
wherever the string is the TOOL'S name; `dss-code-prime` wherever it is the PROJECT'S or the
REPOSITORY'S. 96 files, 91 of them verified to carry ONLY lines that gain `dsscp` or lose
`dss-code-prime`, the other five exempt BY NAME with a reason.

⚠⚠ **IT REWROTE THIRTEEN REPOSITORY CHECKOUT PATHS ACROSS SIX FILES** —
`wsl-leg.sh` (`SRC`, `DST`), `remote-leg.sh` (`REMOTE_DIR`, twice),
`profile-compile-dispatch.sh` (`RREPO`, all three legs), the sqlite harness's `SRC_DIR` in two
drivers plus a self-test fixture, and the citation inventory's `.claude/skills/dss-code-prime/`
key — pointing the entire multi-host carriage at directories that exist on no host.
★★★ **AND THE SUITE WAS 1603/1603 GREEN WHILE THAT WAS TRUE.** Exactly one of the
thirteen reddened anything, and by accident: `plan_citations_guard`, because its ceiling key
stopped naming a real document. `ctest` does not run the WSL leg, the remote legs, or the sqlite
harness driver, so the other twelve reddened nothing at all — two thirds of this project's
standing 3-leg gate removed without a diagnostic.

★ **THE CAUSE IS THE REUSABLE PART: the instrument's protect list ENUMERATED FIVE SPELLINGS,
and a list of spellings cannot discover a class it does not contain.** A GitHub URL, `project(`,
the funding line and two prose phrases were all its author thought of. The reviewer's question is
not *“is this list right?”* but ***“what CLASS of string does this rename have no
authority over?”*** — and a checkout path is the repository's identity in the most
literal form available: `~/src/dss-code-prime` names a directory that EXISTS on four real hosts,
which nobody renamed and which this repository cannot rename.

⚠ This is the SECOND consecutive cycle in which a rename broke an instrument no gate covers.
P31's rule — *classify by whether the file EXECUTES, not by which directory it sits in* —
was applied correctly here; these files DO execute. The hole is new and different: **the file
executes, but the STRING inside it is a FOREIGN identity.**

### ✅ `carriage_paths_guard` — OPENED AND CLOSED IN THE SAME CYCLE

`d157f5b5`. **The invariant is AGREEMENT, not RESOLUTION**, and that is what lets it be a `ctest`
entry at all: checking that a path resolves would need the host it names, which is exactly why
nothing checked it. Every repository path a carriage script names must end in the name `project()`
declares in `CMakeLists.txt`. ⚠ Deliberately NOT *“match the checkout's own basename”*
— a fork, a worktree or a CI runner's `s/` directory is an honest tree with a different name,
and a guard that reds those gets disabled.

It keys on the **variable's ROLE**, never a file list, so a NEW carriage script is covered the day
it lands; it has FLOORS, because a role-keyed scan's failure mode is to find nothing and report
success over an empty set. ✔MEASURED: 9 repository-root constants and 3 skill-directory
references. ✔RED-ON-DISABLE through `ctest` on a REAL mutation with the subject proved read:
`remote-leg.sh` md5 `99d8eb5c…` → `78af0ab9…`, guard FAILED naming ``dsscp`` (a
string that exists only in the mutant), restored, PASSED. Seven self-test arms, each asserting the
MESSAGE — including **two that pin what it must NOT red on** (an interpolated tail is not a
claim it can check; a relative `src/…` is not a checkout path), because the expensive failure
for a guard is not missing a defect, it is crying wolf until somebody disables it.

★ **TWO `dss-state` DEFECTS WERE FIXED THERE RATHER THAN FILED**, both the same species —
an instrument no ctest entry exercises, silently not working. Its CLI search named
`build/bin/dss/`, `build-rel/` and `build-dbg/`, none of which exist under this repository's
one-root layout, so **`dss-state` exited 2 before running a single probe** and had done so since
the layout changed; it now ENUMERATES `build/*/bin/dss/`. And its `engineSiblings` staleness list
named `dsscp.dll` while the build produces `libdsscp.dll` — with the `lib` prefix — so
`.filter(existsSync)` yielded the EMPTY list and the verdict collapsed to the exe alone, **which
is exactly what the comment three lines above it forbids**. It now lists every real spelling AND
**REFUSES on an empty list**: the extra spelling repairs today, the refusal is what makes the next
one loud. ✔Both verified by execution — 143 probes, report renders.

### ★★ WAVE 1 — FOUR LANES, EIGHT ROWS CLOSED, ZERO OPENED

★★★ **THE BINDING CONSTRAINT ON PARTITIONING IS NOT FILE OVERLAP, IT IS
`DSS_CONFIG_ROOT`, AND IT IS NOW MEASURED RATHER THAN ASSUMED.** ✔`tests/CMakeLists.txt`
gives every test entry `ENVIRONMENT "DSS_CONFIG_ROOT=${CMAKE_SOURCE_DIR}"` and
`findShippedConfig()` prefers that over its cwd-walk ⇒ **every test in every lane's private
`build/<lane>` reads the ONE LIVE `src/dss-config/` in the shared source tree.** A per-lane build
tree isolates artifacts; it does NOT isolate configuration. ⇒ **at most ONE lane per wave may
edit `src/dss-config/**`.** ✔THREE separate lanes caught `c.lang.json` mid-write during this
wave, one of them unparseable outright (a raw LF inside a `$comment`), each producing a large
corpus red that was NOT a defect. **A corpus number taken while another lane holds the config is
not attributable to anything.**

**Lane A — the C attribute surface (4 closed, 1 NOT DONE).**
`__attribute__((__const__))` now compiles — the single top parse blocker for real glibc
headers, 50 occurrences in one 1819-line preprocessed TU. ★★ **BOTH SIDES MOVED
BECAUSE THE LOADER NOW REFUSES A CONFIG WHERE ONLY ONE DID:** a new Clause-C check walks the rules
reachable from the attribute shapes and reds on EITHER one-sided state with its own actionable
message, so the *grammar parses it and the reader silently drops it* hazard is closed by a guard
rather than by care. Both directions exercised. A new shape-language element form
(`{"tokenClass": …}` + a top-level `tokenClasses` block, `allKeywordKinds: true` unioning the
document's OWN keyword kinds) is the mechanism. ✔It also CORRECTED the row's reference figures
by probing separately: gcc refuses ~**30** reserved words in the GNU spelling, not four —
conclusion unchanged, DSS takes clang's superset per §A.3b.
⚠ `D-CSUBSET-ATTRIBUTE-DEPRECATED-TYPES` is **NOT DONE** and is reported as such: part of it
already works at HEAD and its remedy column is stale, and the real fix needs an emit-gate design
inside `resolveTypeNodeImpl`, which runs speculatively, repeatedly and under reporter rollback.
It stopped rather than half-land it on the hottest path in the semantic tier. **That is the
correct answer to “refused is not closed” — not a refusal dressed as a closure.**

**Lane B — shipped-config reads (2 closed).** ★★★ **THE ROW'S OWN PREMISE WAS
HALF WRONG AND THE CORRECTION IS THE FINDING.** It said 6 of 9 drains were unchecked and 3 were
checked. ✔MEASURED by execution with a purpose-built four-stream probe:
`ostream::operator<<(streambuf*)` extracts THROUGH THE STREAMBUF POINTER and never touches the
`istream` object, so `in.bad()` after `buf << in.rdbuf()` is **false in every case, including one
where the streambuf THROWS mid-read** ⇒ the 3/6 split was counting COMMENTS, and **9 of 9 were
unchecked in the only sense that matters**. Worse, libstdc++'s `basic_filebuf` maps an OS read
error to `eof()` rather than throwing, so a SHORT READ sets no bit at all: **the only detector for
a truncated read is a BYTE COUNT.** One helper, nine sites, zero hand-written drains left in
`src/`, plus a source-scan guard that fails closed. Two live defects fixed in passing and recorded
in the row: `loadShippedPipeline` opened in TEXT mode (Windows translated CRLF before the parser
saw the bytes), and `readResourceBytes` — the `#embed` reader, a BYTE-EXACT consumer — had
only the vacuous check, so a torn resource embedded a silent prefix. **That one was a silent
miscompile.** The torn-config crash was ATTRIBUTED before it was fixed, as its row demanded:
neither suite was a `noexcept` escape and neither was a product defect. It also burned
`D-TEST-ABORT-IN-A-FIXTURE-HAS-NO-GUARD` down **61→44 sites, 29→24 files** without opening
anything.

**Lane C — the corpus consumes a foreign archive (1 closed).** A new `prebuiltLibraries`
manifest key in BOTH runners, with a `containerWitness` the bytes must contain; the example links
two real BSD archives DSS did not build (`__.SYMDEF` + ELF members, `__.SYMDEF SORTED` + Mach-O
members) and exits 42 from symbols defined nowhere in the repository. 1264/1264 on Windows and on
Linux; four mutations, each red then restored. ★★ **AND IT REFUTED ITS BRIEF WITH A
MEASUREMENT, WHICH IS WORTH MORE THAN THE ROW:** the FF1 row claimed
`integrated_tests/coverage-boundary` *“would catch a one-sided implementation”*, and this
orchestrator relayed that claim into the brief WITHOUT RUNNING IT. Under a deliberate one-sided
mutation it stayed **GREEN** — it compares COMPILE-COVERAGE sets over a subject that never
includes this example. What caught it is `runRunnerVocabularyPin` inside
`integrated_tests/cli-surface`, which diffs the two runner sources' manifest-key literals.
★ **`coverage-boundary` catches one-sided COMPILE COVERAGE; `runRunnerVocabularyPin` catches
one-sided MANIFEST VOCABULARY. They are not substitutes.** It also DECLINED to teach the `ar`
writer BSD, with a reason rather than a shrug: the flavour is chosen in `compile_pipeline.cpp` and
belongs on the `.format.json` documents, and the CLI runner links `nlohmann_json` alone, so a
`Bsd` arm would have shipped with NO CALLER.

**Lane D — the outgoing-argument cursor (1 closed).** ONE pass owns every outgoing-argument
byte offset now, and the refusal that stood in for the repair is **DELETED, not kept beside one**.
★★ **A SECOND SILENT MISCOMPILE OF THE SAME ROOT CAUSE WAS FOUND AND FIXED IN THE SAME
CHANGE:** `fixedOperandCount` is a POSITION boundary and that pass RENUMBERS positions, so DSS
emitted `mov $0x0,%rax` — AL=0 — where gcc 13.3.0 emits `mov $0x1,%eax` for the identical
variadic call, and gcc's own callee prologue gates its xmm spill on `test %al,%al`.
⚠ **INVISIBLE DSS-TO-DSS, which is why it survived:** DSS's own variadic prologue spills
xmm0–7 UNCONDITIONALLY, so no corpus of DSS callers and DSS callees can ever see it.
Red-on-disable ×2 with both mutant exit codes PREDICTED from the ABI layout before being
measured. ⚠ It also stated the honest limit of its own example: **under the mutant the
`examples/` arm stayed GREEN on Windows**, because this host only COMPILES the ELF and Mach-O legs
and a silent miscompile compiles fine; the runtime red was taken on WSL.

### ★ WHAT ELSE LANDED

- **`scripts/examples-census/examples-census.py`** — the corpus-manifest census, as an
  INSTRUMENT rather than the fourth throwaway parser. `examples/README.md` records that the same
  census had been re-derived ad hoc at least three times, once returning *“a plausible
  zero”* for every manifest and once publishing four figures that were already stale when they
  shipped. ✔Its whole block is re-derived at this commit (**634** manifests, **2,129** target
  entries, **707** optimizer arms), and `prebuiltLibraries` is documented there.

### ⚠ WHAT P32 OWES P33 — A QUEUE, NOT A MOOD

1. **`D-CSUBSET-ATTRIBUTE-DEPRECATED-TYPES`** — lane A's NOT DONE, with the design named:
   thread the flag onto the TAG/typedef symbol at the Pass-1 mint, and emit inside
   `resolveTypeNodeImpl`'s tag/alias arms, which needs an emit-gate because they run
   speculatively and under reporter rollback. ⚠ Part of the row is ALREADY CLOSED at HEAD and
   its remedy column is stale — re-read it before starting.
2. **The remaining rows P31 opened**, re-derived from the guard rather than from any list:
   the imaginary-literal suffix, `__COUNTER__`, the cast-operand budget cliff, the gcc
   linemarkers, the two `D-LK-*` rows (both need `.format.json` vocabulary, so they are a config
   lane), `D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED` (**runs ALONE**
   — its honest closure changes how EVERY test resolves its config), and the two `.plans/`
   rows.
3. **The `D-FF1-` namespace decision** — still a genuine operator fork, still not invented
   around.
4. ★ **A candidate worth considering rather than assuming:** make the `examples/README.md`
   census a GENERATED block checked by `examples-census --check` in `ctest`, so it cannot rot a
   fifth time. Not done here because it restructures a user-facing document, which is a decision
   rather than a fix.

## 0.0000000000000000000000000000000000 ★★★ CYCLE P31 — THE CONFORMANCE LANDED, AND SIX OF THIS PROJECT'S OWN INSTRUMENTS WERE FOUND LYING

**Eight lanes (A–H), in waves of four.** Gate **1603/1603 in 482.78 s (Windows x86_64, build+ctest chained) · 1603/1603 in 363.08 s (WSL x86_64 + qemu arm64, CLEAN build)**. Balance **1023 → 1032** — closed 9 (⚠ **four of them bookkeeping-only**, marked `✅🧾`: the work pre-dated this cycle, so the cycle is credited with nothing for them) against **18** opened, which the guard reports as **13 more OPEN rows created than closed**. Suite **1584 → 1603 tests**. 74 tracked files modified, 19 added, +4103/−525. ⚠ Every number in this section is re-derivable: `python scripts/check-anchor-balance/check-anchor-balance.py` prints the balance figures on every run. **RE-MEASURE. Do not re-quote.**

★★★ **THE THEME, AND IT IS NOT THE FEATURE WORK: THREE CORRECTIONS TO THE RED-ON-DISABLE PROOF STANDARD IN ONE CYCLE, EACH FAILING IN THE FLATTERING DIRECTION.** This project treats a red-on-disable observation as the one measurement it calls proof. P31 found three separate ways a green one lies, and every one of them PASSES the check — so the lane believes it holds proof and stops looking, which is worse than having no check at all.
1. **A PE image's md5 is not a compiled-in proof** (`D-TEST-A-PE-IMAGE-MD5-IS-NOT-A-COMPILED-IN-PROOF`, lane F). ✔MEASURED: the shipped DLL's md5 moved **between two builds of IDENTICAL sources** (`5e6cbe74…` vs `10eb22ea…`) because a PE image carries a LINK TIMESTAMP — and it moves for config-only mutants that recompile nothing. A moved image md5 proves a LINK happened, which happens either way. ⇒ the subject became the mutated TU's `.obj`.
2. **A moved object md5 is not a reached-the-binary proof** (`D-TEST-A-MOVED-OBJECT-MD5-IS-NOT-A-REACHED-THE-BINARY-PROOF`, lane G, ONE CYCLE after (1) prescribed the object). ✔MEASURED: a mutant's ctest run came back GREEN twice with the object md5 correctly moved — the compile succeeded, the LINK failed (`ld.exe: cannot open output file … Permission denied`, a stalled ctest child holding the DLL), so ctest ran the **PREVIOUS** binary. ⇒ the BUILD'S RETURN CODE is the other half, checked in the same run. ★ Taken together: **the image moves when nothing changed, and the object moves when nothing shipped.**
3. **A run is void when ANY shared input moves under it — not only when a BUILD does** (`D-TEST-A-CONFIG-WRITE-DURING-A-GATE-TEARS-EVERY-READER`, lane G). ✔MEASURED: a full gate reported **1102 passed / 501 FAILED with nothing building at any point**, because a shipped `.json` was rewritten in place mid-run. Re-run quiesced: **1602/1603**. ★ The TELL is a **CLIFF** — contiguous failures from one point that never recover (pass at sequence 1101, fail at 1102, red through 1599) — where a genuine regression SCATTERS. ★ And the control is one command, cheaper than diagnosing any single red: compile a trivial input with the suspect's own binary against the live tree.
⇒ All three are now in `.claude/skills/dss-cycle/references/the-bar.md`, and **the message-names-the-refusal check outranks all of them**: a mutant can reach the right artefact and still exercise nothing.

★★ **AND THE THREE INSTRUMENT FAILURES THAT WERE THE ORCHESTRATOR'S OWN — plus a fourth it nearly shipped.**
- **It edited `.plans/**` under a running lane and flipped that lane's gate** (`D-CYCLE-THE-ORCHESTRATOR-EDITED-PLANS-UNDER-A-RUNNING-LANE-AND-FLIPPED-ITS-GATE`). A lane's `plan_citations_guard` went RED then GREEN **with no edit of its own in between**, because the orchestrator applied registry rows and re-baselined the citation ratchet while that gate was in flight. The existing rule covered `src/dss-config/**` and stopped there, because the hazard had been framed as *a config document is an input to the compiler*. ★ **`.plans/**` is an input to a GUARD, and a guard is a ctest entry** — five guards take it as their subject. ⇒ **ask what a file is an INPUT to, not which directory it lives in**; both instances of this defect came from reasoning about the directory. ⓘ The lane caught it in the FLATTERING direction (red→green) and measured it rather than concluding its earlier red had been a flake.
- **The "103 misglyphed rows" census was a keyword sieve mistaken for a verdict; the real number was 4.** ✔RE-MEASURED: **16 rows open with a done-word, 12 of those name a residue in the same cell, 4 candidates** — and all four were then read in full and closed. ★ The over-count has one mechanism worth more than the number: **a PARTIAL CLOSURE LEADS WITH THE PART THAT CLOSED** (`D-CSUBSET-SUBNATIVE-ALU-FORMS` opens *"CONVERSION FORMS ✅ CLOSED"* and later says *"RESIDUE STILL OPEN"*), so the sieve counted a CONVENTION as the defect. ⚠ And the second sieve was wrong too, in the dangerous direction: its prefix class swallowed the `DIS` of *DISCLOSED* and reported a row as saying CLOSED. What survives is the structural half — the registry documents ONE closed spelling while its authors use at least two, and `check-anchor-balance` still cannot see the difference between *closed* and *shipped-with-a-stated-residue*.
- **The fold gate reported itself GREEN while exiting 8** (`D-CYCLE-A-TRAILING-ECHO-REPLACED-A-FAILED-GATE-S-EXIT-CODE-WITH-ZERO`). ✔MEASURED: the P31 fold gate exited **8** with `stale_refusal_citations_guard` RED — `99% tests passed, 1 tests failed out of 1603` — and the background-task notification said **`completed (exit code 0)`**. The invocation ended `…; echo "RUNGATE_EXIT=$?"`: the echo READ the failure correctly, printed `RUNGATE_EXIT=8` into the captured output, and then, being the last command in the compound, **became the command's terminal state** — the one thing that channel carries. ★ **The witness added to make the exit code visible is what discarded it.** `run-gate.sh` is not at fault: it detected the failure, named it, and exited 8. ⇒ **never append a command after it**; where a witness line is wanted, preserve the code across it (`…; rc=$?; echo …; exit "$rc"`). ⚠ Under `/loop` the next step writes that gate's figure into the handoff and commits, so this fails in the direction nothing looks at twice. It was caught for one reason, with nothing enforcing it: **the log was read before the notification was believed.**
- **And the fourth, caught at the fold before it shipped:** those four glyph repairs were first recorded as ordinary closures, which would have credited this cycle with four closes it did not earn. They now carry the registry's `✅🧾` receipt mark — the open population still drops by four, the cycle's net is credited with nothing. ★ `check-anchor-balance`'s own header calls marking a closure you earned "a false statement about history"; **omitting the mark from one you did not earn is the same false statement in the direction that helps.**

★ **AND ONE GUARD EARNED ITS KEEP AT THE FOLD, BY PREDICTING ITS OWN CASE.** `stale_refusal_citations_guard` failed the fold on `examples/c/gnu_statement_expression/expected.json`, which said *"landing either alone leaves `math.h` and `assert.h` still refused"* while citing `D-C-GNU-EXTENSION-KEYWORD` — a row closed **in that same fold**. The guard's own message names the mechanism verbatim: *"most often because a sibling lane closed the row IN THIS COMMIT"*. ★ **The sentence was a design RATIONALE, true of the world it was written in and false the instant its premise was discharged — and nothing else in this repository re-reads a rationale after the thing it argues for has landed.** Repaired with a past-tense governor plus the measured present. ⚠ **NOT by padding it past the guard's 26-character claim window**, which would have cleared the check while leaving a human reader the identical false fact — the distinction between fixing the sentence and defeating the instrument that read it.

★★★ **THE CONFORMANCE THAT LANDED — chosen by an IMPACT INSTRUMENT, not by inspection.** Lane G stopped reading the remainder list and built one: `gcc -E` over ordinary translation units plus a `/usr/include` census with escaped patterns and a positive control per row.
- **`__extension__`** — 261 occurrences across 40 C headers, 23 reaching the `-E` output of a plain 9-header TU. ★★ **AND IT IS *NOT* A DECLARATION SPECIFIER**, which is the design the row exists to record: every other GNU dunder word in `c.lang.json` is a keyword-table alias onto an existing kind, and that shape would have accepted **four spellings no reference accepts** — with every POSITIVE test still passing. ✔MEASURED across gcc 13.3.0 + clang 19.1.7 + cl 19.44, one TU per case: **the refusals are the sole discriminator.** ⓘ MSVC's acceptance is RECOGNITION, not leniency — the matched negative control `__no_such_keyword__ int x;` is C2054.
- **GNU statement expressions `({ … })`** — 59 occurrences across 15 headers, and **7 sites write the two together as `__extension__ ({`**, which is why they are one batch: with only one of them landed, `math.h` and `assert.h` WERE still refused, and both landed together. ★★ **The substrate was already built for it:** `HirKind::SeqExpr`'s own header comment names GCC statement expressions as what it models, `collectLocalDecls` already walked the whole body subtree, and MIR's SeqExpr arm already ran each statement child through `lowerStmt`. **Zero new HirKinds, zero MIR, zero LIR** — only the front end was missing.
- ✔END TO END: a real 1819-line `gcc -E -P` glibc TU now produces **zero** parse errors from either construct.
- Also landed: `_Alignof` on a value operand · `__builtin_offsetof` · `__builtin_types_compatible_p` · `__builtin_choose_expr` · Apple ARM64 natural stack-argument packing (new `stackArgPacking` vocabulary over a closed `slot`|`natural` enum — omitted ⇒ every existing calling convention byte-unchanged) · BSD `ar` archives, read from REAL committed fixtures because a hand-authored one would only test the reader against its author's reading of the format · and Mach-O `__compact_unwind`, which unblocks **every stock macOS archive** (the design settled by measuring Apple's own linker: ld64 CONSUMES `__compact_unwind` and synthesises `__unwind_info`, so "carry it through" was refuted by the platform).

★★ **A CANDIDATE KILLED BY A MEASUREMENT, AND IT WAS THE INSTRUMENT'S FAULT:** the remainder list's *"case ranges — 14 corpus hits"* is an artifact of an unescaped `(` in an ERE. Every hit was English prose (*"in case of…"*). ✔Re-measured with escaped patterns and a positive control: **zero** real `case A ... B:` in the sqlite corpus and in `/usr/include`.

★★★ **THE FLAKY TEST WAS US.** `ffi/test_c_header_parser` failed once at 8-way parallelism and passed alone. ✔MEASURED: it re-opens the live 478 KB `src/dss-config/sources/c.lang.json` on every one of its ~25 cases — **handle open 76.6% of the suite's wall time** — because **no `loadShipped` entry point caches**: each calls `findShippedConfig` then `loadFromFile` unconditionally. What tears it is this project's OWN red-on-disable convention: a mutant harness rewriting a shipped document in place while a neighbouring gate runs. Truncate hammer **156/156 red**; atomic `os.replace` **0/18** — Windows refuses the WRITER while a reader holds the file, so the tearing shape is the one we use. Fixed at the READER with a per-process private copy taken at **RUN** time — ★ never at build time, because a build-time copy would silently GREEN every config-level red-on-disable, the convention being *mutate a `.json` and re-run ctest without rebuilding*. No retry, no `RUN_SERIAL`, no raised timeout, no widened assertion.
⚠ **AND THE POPULATION IS FAR WIDER THAN THE SUITE THAT FLAKED** (`D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED`, OPEN, its trigger ALREADY FIRED): 150 test sources reach `GrammarSchema::loadShipped`, duty cycles span 0–67.9%, and `analysis/semantic/test_semantic_analyzer_c` holds that handle ~42 s per run against the subject's former ~0.9 s. ★ Worse, that census is scoped to the LANGUAGE document, the least-read of the three — ✔MEASURED call sites in `tests/`: `TargetSchema::loadShipped` **788**, `ObjectFormatSchema::loadShipped` **294**, `GrammarSchema::loadShipped` **152**. `.target.json` and `.format.json` are read **~7× as often**, **and they are exactly the documents our mutant harnesses rewrite most.**

★★ **THE EVIDENCE-DESTROYING GATE, FIXED** (`D-GATE-CTEST-VERDICT-LOSES-THE-FAILING-TEST-S-OWN-OUTPUT`). The flake cost a full lane to re-derive because ctest keeps a failing test's output in exactly ONE place — `<build>/Testing/Temporary/LastTest.log` — which the NEXT run OVERWRITES. The confirming re-run replaced it with its own passing text; the only surviving artefact was a 30-byte `LastTestsFailed.log` naming the test. `run-gate.{sh,ps1}` now default `CTEST_OUTPUT_ON_FAILURE=1` through ctest's own env channel — never argv injection, because the wrapper runs an ARBITRARY command — costing nothing on a green run. Two more run-gate holes closed alongside: the `.ps1` twin forwarded **neither** variable through `WSLENV`, so every `.ps1` gate that shelled into WSL ran SERIAL and SILENT on the far side while the wrapper believed otherwise; and a log path beginning with `-` was accepted, producing a stray `-LogPath` file in the repo root and a refusal about something else entirely.

★★ **A FAIL-LOUD HOLE UNDER ALL OF IT, FOUND AND NOT FIXED** (`D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT`). ✔MEASURED: of the **9** `<< …rdbuf()` drain sites in `src/`, **3 check the stream and 6 do not** — `GrammarSchema::loadFromFile`, `TargetSchema::loadFromFile`, `ObjectFormatSchema::loadFromFile`, `mergeLanguageReferences`, `dss::opt::loadShippedPipeline`, and the shipped-lib descriptor loader (which additionally CACHES its result). ★★ **The project already ruled this class CRITICAL and fixed it once, in one path**: `dss::ffi::slurpFile` carries the fix and the reasoning verbatim, and the identical shape was left in the loader every `loadShipped` goes through. ⚠ Its red-on-disable is the hard part and must not be faked: **a SHORT file reads cleanly to EOF and sets no bad bit**, which is exactly why this hole was invisible to the truncate hammer that found everything else.

★ **THE OTHER LIVE FINDINGS OPENED**, all measured, none guessed:
- **`__attribute__((__const__))` is the top parse blocker for real glibc** and was on nobody's remainder list — 50 occurrences in one 1819-line TU; neutralising that ONE spelling takes it from 50 parse errors to **0**. ⚠ **A grammar-only fix would be a SILENT ATTRIBUTE DROP**: `collectAttrClauses::ownsName` and `extractOneAttrClause` both locate the clause name by `tokenKind == cfg.identifierToken`, so a keyword-named clause would parse, match no effect row, and vanish. The two sides move together or not at all.
- A **cast operand** longer than the `operand` alt's 1024-token probe budget is refused — **proven pre-existing by a control**: a plain `(int)(0 + 0 - 0 …)` with no statement expression cliffs the same way at ~200 terms.
- `subtreeType` **walks into a statement-expression body** and yields a false type mismatch (bisected: only *deref of a body-local, as the yield* fails).
- `gcc -E` **linemarkers are refused**, so preprocessed input needs `-P` — which discards exactly the file/line provenance a conformance census wants.
- Two suites (`tokenizer/test_tokenizer`, `hir/test_hir_lowering_c`) **CRASH** with `0xC0000409` on a torn config rather than failing loud, which under ctest reads as a different failure class entirely.

⚠ **BALANCE IS ADVERSE AND IT IS PRESENTED, NOT WIDENED AWAY.** 18 rows opened against 5 earned closes. **16 of the 18 are defects FOUND, not work deferred** — several found by instruments this cycle built for other purposes, which is what a cycle spent auditing its own measuring equipment produces. Under [[feedback-open-anchors-roll-into-the-next-cycle]] they roll into P32, which takes them first.

✅★★★ **WHAT P32 TOOK, IN ORDER — ALL THREE ITEMS LANDED; kept as written because the WIDTH RULING in item 1 still governs every future mention of the two names.**
1. **The binary rename to `dsscp` — AN EXPLICIT OPERATOR REQUEST, not a queue item this cycle invented** (*"please also add a cycle so dss code prime build artifact output name is dsscp instead of dss-code-prime. CI and cmake should know this"*). ⚠ Its width was RULED by the operator and the ruling is narrower than the word "rename" suggests: **"Command identity"** — `dsscp` for everything typed or read as the TOOL'S NAME (the built artifact, CI invocations, CMake target and install names, documentation that shows a command line); **`dss-code-prime` STAYS** wherever the string is the PROJECT'S or the REPOSITORY'S identity. Do not widen it to a global search-and-replace. ⓘ The instruments exist and were re-dry-run against the grown tree at this commit: `scratchpad/p32/orch/hand_rewrites.py` then `scratchpad/p32/orch/rename_to_dsscp.py` — **487 substitutions across 97 files, 8 protected sites, 28 prose files skipped, 3 refused correctly**; the refusal arm MUST disappear once the hand rewrites land, and if it does not, the rename is incomplete rather than done. ★ It gets its OWN commit on a clean tree, because a mechanical rename mixed into feature work is unreviewable.
2. **The 18 rows P31 opened**, per the roll-forward rule above.
3. **The 12 rows `check-anchor-balance` reports as ALREADY UNBLOCKED** — an opener that has closed or a trigger that has fired, so they are schedulable NOW and are not deferrals at all. Re-derive the list from the guard, never from this line: `python scripts/check-anchor-balance/check-anchor-balance.py`. ⓘ One of them, `D-ASM-X86-IMMEDIATE-WINDOW-REFUSES-WHAT-GAS-TRUNCATES`, already carries an operator ruling (2026-08-24): **accept the wider immediate AND always diagnose** — so it needs implementation, not a decision.
⚠ **AND ONE THING P32 SHOULD *NOT* DO**, because a previous handoff called it the next cycle's first item and this cycle measured otherwise: the `c-subset` documentary sweep. See the closing item of this section — the live instruction it was really about has been fixed and verified by execution, and the remaining files are history, one deliberately-preserved narrative, and the operator's exempt `settings.local.json`.

★ **ONE STANDING ITEM CLOSED BY MEASUREMENT RATHER THAN BY WORK.** The prior handoff called a `c-subset` documentary sweep "the next cycle's first item" because *a live instruction among them is now wrong*. ✔MEASURED: the live instruction was `.claude/skills/dss-state/driver.mjs`, which carried two `--language c-subset` invocations — the whole `dss-state` probe battery had been broken since the rename. **Fixed and verified by execution** (exit 0, artifact emitted). The remaining 36 files are `.plans/` history, one HISTORICAL narrative in a skill reference (`examples/c-subset/double_to_unsigned`, the name that run actually reported — rewriting it would falsify the record), and the operator's `settings.local.json`, which is a PERMISSION boundary and exempt. ⇒ **the sweep should NOT be run**; the item was over-stated and is now closed.

---

## 0.000000000000000000000000000000000 ★★★ CYCLE P30 — THREE SILENT MISCOMPILES, AND THE ENCODING TABLE LEARNS TO STATE A SEQUENCE INSTEAD OF ONE INSTRUCTION

**Gate ✔MEASURED at this commit: 1584/1584 in 461.72 s** (`scripts/run-gate/run-gate.sh` with the
tool-emitted witness `100% tests passed`; a caller-authored success string is refused). **Balance
✔MEASURED: closed 6, opened 7, OPEN 1022 → 1023** — net +1, of which one is a disclosed
pre-existing row and exempt. ⚠ Re-measure both with the scripts; do not re-quote this line.

★★★ **THE UNIFYING FINDING: THREE OF THE FOUR DEFECTS WERE DECLARATIONS THAT COULD
ONLY EXPRESS HALF THE FACT, AND EACH STAYED GREEN FOR MONTHS BECAUSE THE TEST WRITTEN TO CATCH IT
COULD NOT TELL THE TWO HALVES APART.**

### The silent miscompiles — all witnessed by EXECUTION, none by code-reading

**1. Apple ARM64 `va_arg` returned the WRONG ARGUMENT.** `lowerVaStart`'s `HomogeneousPointer` /
`variadicUsesOverflowBase` branch emitted `VaOverflowArgAreaAddr` with **no payload**, so the entire
fixed-stack displacement was discarded and the first `va_arg` read the last NAMED parameter. Scoped
to `apple_arm64` alone — AAPCS64 ELF and every x86_64 leg were correct. One omitted operand; the
LIR consumer already threaded it. ✔Apple Silicon: exit **210** fixed / **54** mutant, debug AND
release, **with the emitted artifact's md5 changing between arms** rather than only an mtime. Apple
clang 21 returns 210.
★★ **THE EXAMPLE WRITTEN TO CATCH THIS ALREADY DECLARED THE darwin-arm64 LEG AND PASSED.**
`varargs_overflow_fixed_stack`'s own `main.c` says *“RED-ON-ZERO-DISPLACEMENT”*, but its
seven named ints were calibrated for SysV's **six**-GPR pool; on Apple's **eight** nothing overflows,
the displacement is 0 by construction, and the assertion was true of the broken and the correct
implementation alike. **That leg is REMOVED rather than recalibrated** — a red-on-disable claim
is a UNIVERSAL about the source and must hold on every target it declares.
★ **And the row's proposed fix was RIGHT FOR A REASON THE ROW DOES NOT GIVE**: ✔Apple packs
named stack args at NATURAL size (a second stacked `int` at incoming+4) while the cursor counts 8 per
scalar. The payload is correct only because DSS's own `lir_callconv` pads to `outgoingSlotSize` on
BOTH sides. Anyone re-deriving from Apple's ABI alone would have concluded the row was wrong —
and that fact is itself a divergence, now
[[D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED]].

**2. x86 unsigned↔float was wrong in FOUR arms, not the two the rows named.**
`x86_64.target.json` declared the SIGNED converters for the unsigned opcodes, because SSE2 has no
unsigned scalar convert. ✔Measured against gcc-13, clang-19 and Apple clang 21 (which agree
10/10): `(unsigned long long)1.0e19` gave 2^63 from BOTH a `double` and a `float` source, and
`(double)(1ULL<<63)` gave a negative double. ★ **THE FOURTH ARM WAS NAMED BY NO ROW AT ALL** —
`(double)(unsigned)0xFFFFFFFF` returned **−1.0**, because the width-32 variant emitted
`cvtsi2sd xmm, r32`, a signed **32-bit** read. The old row's own comment carried the full fact
(*“a U32 always fits the signed-64 result exactly”*) while the declaration used half of it.

### The architecture — OPERATOR-RULED, and the premise that sized it was measured FALSE first

An opcode row could declare only ONE instruction, and **1:1 is the DEGENERATE CASE** of *“this
operation is realized by these machine instructions”*. A two-valued strategy enum was rejected as
**“an arch branch with a different spelling”**: no `if (arch == x86)` appears, but the switch
arm is x86-shaped and lives in shared substrate, and x86 has more of these coming (`popcount` without
POPCNT, 128-bit multiply-high, float min/max NaN semantics). `TargetOpcodeInfo` now carries a
`lowering` block — a DIFFERENT TIER from `encoding` (LIR-instruction→bytes vs
MIR-operation→LIR-instructions), consumed while virtual registers still exist so a sequence's
temporaries are REGISTER-ALLOCATED rather than stealing scratch. **arm64 declares a ONE-STEP
sequence, not a `native` flag** — the same kind of row, one entry long, and
`lowerViaDeclaredSequence` cannot tell a one-step sequence from a seven-step one.
★★ **THE BLOCKING PREMISE WAS KILLED BY MEASUREMENT BEFORE ANY OF IT WAS BUILT.** Both the
row and my own brief assumed the fix needed a compare and a branch, because that is what gcc emits.
✔clang-19 emits **ZERO jumps** across all three conversion functions and does not even use
`cmov`. Under the disjunction rule DSS may take clang's algebra, so `TargetCondCode` never entered
into it. ⇒ **the question that decides a design is often not the one the row asks.**

### The conformance oracle, repaired

direction-A **17 → 18**, divergences **30 → 31**, over a full 5-oracle roster.
`a_nested_function_gnu` was hidden behind an `@expect-ref varies` waiver while MinGW gcc 13.2 and WSL
gcc 13.3 both ACCEPT it — and ★ **the waiver's author reasoned CORRECTLY from a corpus header
sentence that had stopped being true**: it documented `accept` as *“at least one judging oracle
must accept, else RED”*, which ceased to hold on 2026-08-11 when `pin()`/`corroborate()` were
split, and went stale instead of failing loud. `b_elided_initializer_element`'s note said the element
lands at index 2; ✔measured by `objdump` AND by execution, `{ 1, , 3 }` emits `{1,3,0}` with the
`3` at index **1**.
⚠ **A GREEN RUN DOES NOT IMPLY A FULL ROSTER** — an unwitnessed accept routes to
`NotWitnessed`, which never reds. WSL cold-started with `HCS_E_CONNECTION_TIMEOUT` on one run this
cycle; inside that window the census would have dropped to 2 oracles and still exited 0. **Always
read the `ORACLES USED (n)` line.** ✔With the full roster, **8** rows (not the 2 previously
believed) would have become FALSE direction-B without clang.

### Inline-asm width-view modifiers

`%w`/`%x` on arm64, `%b`/`%w`/`%k`/`%q` on x86-64. ✔**`%w` means 32 bits on aarch64 and 16 on
x86-64**, so a SHARED letter table is wrong by construction and the vocabulary belongs to the DIALECT
document — landed with **zero** changes to `mir_to_lir.cpp`. ✔Also measured: a width view on
an `"m"` or `"i"` binding is accepted and IGNORED by gcc, so refusing it would refuse what both
references accept.

### ✅ THE RENAME LANDED — `c-subset` → `c`, its own commit, on a clean tree

✔MEASURED: **1,433 path renames and 5,993 content substitutions across 964 files**;
`c-subset.lang.json` → `c.lang.json`, `examples/c-subset/` → `examples/c/` (608 entries),
`CSubset` → `C` in 72 files of C++ identifiers. Gate **1584/1584 in 437.33 s** — the SAME
test count as before the rename, which is the check that nothing was silently dropped from a glob.
✔`--language c` loads and reports `language=c`. ✔The 426 frozen `D-CSUBSET-*` ids survive
at 6,560 citations (every substitution is case-SENSITIVE, so the frozen `CSUBSET` spelling is
structurally out of reach — asserted per-file anyway).

★★ **`.plans/**` AND `.claude/**` WERE EXCLUDED FROM THE CONTENT SUBSTITUTION, AND THE
REASON IS THAT THE RENAME WOULD OTHERWISE HAVE DESTROYED ITS OWN DOCUMENTATION** — plan 23's
FC20 section states it as `` `c-subset` → `c` ``, which substitutes to `` `c` → `c` ``.
This follows the precedent this file already carries for the scripts consolidation: *“any path
spelled `tools/…` in a commit message or a row older than 2026-08-19 is HISTORICAL, not
stale”*. A row records what was true when it was written; rewriting it makes it lie about its
own date.
⚠⚠ **THE COST IS REAL AND IS OWED, NOT ABSOLVED: 41 documentary files still name the old
token, and any passage among them giving a LIVE INSTRUCTION** — *“edit
`c-subset.lang.json` to…”* — **is now wrong.** Distinguishing a historical mention
from a live instruction cannot be done by grep; it needs reading. That sweep is the next cycle's
work and it is listed below.

### ⚠ WHAT THIS CYCLE OWES THE NEXT ONE

1. **SWEEP THE 41 DOCUMENTARY FILES THAT STILL SAY `c-subset`** — separating HISTORICAL
   mentions (which stay, and are correct) from LIVE INSTRUCTIONS naming a path that no longer
   exists (which are now wrong). ⚠ Not greppable: the same string is right in one sentence and
   wrong in the next, so this is a read, not a substitution.
2. **The seven rows this cycle opened**, per the standing order's own refinement. The two largest are
   [[D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED]] (needs a MIXED-COMPILER witness — no
   single-compiler test can distinguish the two conventions) and
   [[D-CONF-CORPUS-NO-DIRECTION-FOR-A-C23-REMOVED-CONSTRUCT]] (fires the moment a `c11`/`c17` document
   exists, which FC20 plans).
3. **Tranche 1 of the asm work**: 27 operand refs to respell, and 8 shipped examples whose comments
   still claim the width modifier is `S0067`-refused.
4. **The hand-coded C++ expansions** `lowerPopcount`/`lowerClz`/`lowerCtz`/`lowerBswap`/`lowerFNeg`
   and the three shift rules — now migratable onto the declared-sequence table.

ⓘ **Two guards caught the orchestrator's OWN errors at the gate, and both were the guard working
as designed:** the citation ratchet demanded its ceilings come DOWN after positional citations were
removed (unclaimed headroom is where the next one hides), and the registry guard caught three
UNESCAPED PIPES in `{source\|temp\|imm\|const}` — content read as column boundaries, 7 cells
against a 4-cell header — inside a row being written ABOUT a different trap. ★ That failure
mode is invisible from both sides: nothing in the raw text shows it and nothing in the diff shows it.

---

## 0.00000000000000000000000000000000 CYCLE P29 — THE ROWS P28 OPENED, AND WHAT MEASURING THEM FOUND UNDERNEATH

**Nine lanes across five waves, then a step-10 audit, then a repair lane whose own probe found a live
abort. Balance 1022 → 1022, net +0** (closed 6, opened 6 — **2 created**, 4
disclosed-pre-existing, 2 bookkeeping-only, **1 born closed**). Gate **1580/1580** in 431.59 s, up from
1562. ⓘ The net is +0 rather than −1 because the last finding was SIZED HONESTLY rather than
written small: 71 operator-facing diagnostics cite a closed row, which is a cycle of work and so a
disclosed row — debt that already existed, which the balance gate exempts — not a number massaged
to keep a headline.
⚠ **THE BALANCE FIGURE IS THE INSTRUMENT'S, NOT THIS PARAGRAPH'S** — run
`python scripts/check-anchor-balance/check-anchor-balance.py`, which prints every one of those
numbers on every run. It is quoted here only so a reader knows what it said at the lock; the first
draft of this section quoted `1022 → 1020, net −2, opened 4, 1 created` in four places and every one
of them erred in the direction that flattered the cycle.

**The pick was the standing order, unmodified:** the operator's argument was *"proceed to next cycle
(which must also include this `D-ANCHOR-ID-WRAPPED-ACROSS-A-LINE-BREAK-IS-INVISIBLE-TO-EVERY-GREP`)"*,
and §0's P29 entry already said *the rows P28 opened, first*. Re-derived from the REGISTRY at HEAD:
**9 rows carried a P28 date and were still OPEN** — 5 addressable, 4 explicitly not (two §B, one ⛔
awaiting an operator word, one trigger-gated with its opener named). ★★ **FOUR of the five
addressable ones are closed — NOT five.** The fifth,
`D-PLANS-GATED-ROWS-NAME-NO-OPENER`, is still 🔵 OPEN in the registry, and it is the one row whose
debt this cycle GREW (the corrected `is_gated` reports 70 openerless gated rows where the base-ref
predicate over the same tree reports 30). It rolls into P30 as an addressable item rather than being
dropped: **an open row that no queue entry names is invisible.**

### ★★★ THE DESIGN AUDIT EARNED ITS PLACE, AND ONE OF ITS SEVEN BLOCKERS WOULD HAVE SUNK THE GATE

Verdict PROCEED WITH CORRECTIONS; 7 blockers and 8 corrections, all folded into the briefs before any
lane launched. The three that changed outcomes:

- **B1 — every brief omitted the mandated "ANCHOR *AND CLOSE*" paragraph.** ✔The measured precedent
  is four lane briefs producing 22 open rows. The audit's arithmetic put the cycle at **≤3 plausible
  closures against five lanes told only to anchor**, i.e. the balance gate would have refused. Pasted
  verbatim into the common brief along with the *"is this the work I was sent to do?"* test.
- **B2 — lane C's gate regex `-R "examples"` matches ZERO `integrated_tests/` entries.** That lane
  would have reported green having never executed the CLI-subprocess half of its own deliverable —
  precisely the silent-harness bug its own brief warned it about.
- **B4 — the `"=m"` refusal is keyed on `OperandKindFilter::MemBase`, the FORM, deliberately not the
  letter**, so `"+m"` rides the same predicate into `tieAsmReadWriteOperands`, which collapses a
  read-write operand onto ONE register that a memory operand does not have. Lifting it naively is a
  silent miscompile, not a diagnostic.

⚠ **Two of the orchestrator's own claims were refuted by the audit:** `239 wrap sites / 126 files`
does not reproduce (it got 249/128; the guard later measured **290/145**), and the ownership map was
**not** disjoint — 20 of one lane's 46 wall-clock sites sat in `tests/program/**`, owned outright by
another. The wave boundary was right *by accident*, and the reason written into the brief was wrong.

### ★★★ THE CYCLE'S REAL SUBJECT TURNED OUT TO BE THE INSTRUMENTS

Five of the nine lanes ended up fixing the tools that judge this project, because measuring the
queued rows kept exposing the thing measuring them.

- **`check-anchor-balance`'s `split_row` read an escaped pipe as a column separator**, in a registry
  whose own authoring contract escapes literal pipes: **161 of 2,252 rows mis-split**, feeding
  `is_mismarked_closure` the tail of the status cell instead of the closing-work cell. ★★ **TWO
  INDEPENDENT LANES FOUND THE SAME ROOT CAUSE FROM OPPOSITE DIRECTIONS** — one by censusing rows, the
  other by having its own census disagree with the design audit's and tracking down why. **Neither
  could have concluded it alone**: the first would have had a number nobody could check, the second a
  disagreement it might have written off. That convergence is why the two were deliberately forbidden
  to read each other.
- **`is_gated` read a PHRASE, not the operator's rule.** It matched three literal strings in the
  STATUS cell only, never the ⛔/⏳ glyphs and never the REMEDY cell where the house style actually
  puts `⏳ TRIGGER:`. ✔**98 rows the rule condemns were invisible to it**; 3 it accused declare no gate
  at all, one of them on the *negated* form *"no longer merely trigger-gated"*. And after the first
  widening: it enumerated **2** glyphs where **11** lead a closing-work cell, and `TRIGGER_FIRED`
  recognised **one spelling of six** — the un-adverbed `Trigger: FIRED` is the registry's COMMONEST
  spelling of the claim and matched NOTHING, while `ALREADY FIRED` was the only spelling that hit.
  ⚠ **THE PER-SPELLING FIGURES FIRST WRITTEN HERE (`317` vs `84`) DO NOT REPRODUCE, AND THE
  MECHANISM IS INSTRUCTIVE:** a greedy `Trigger:.*FIRED` over whole rows is ~395, and the split
  over-attributed nearly all of it to the first bucket. ✔RE-MEASURED over every recognized
  deferral-table row in the worktree (the guard's own harvester, case-insensitive, open and closed):
  `Trigger: FIRED` **105**, `ALREADY FIRED` **77**. **The conclusion is unchanged; the count was
  never what carried it** — which is the whole of
  [[D-TEST-CMAKE-COMMENT-QUOTES-A-CORPUS-COUNT-THE-TEST-IT-REGISTERS-FORBIDS]] restated against the
  section that anchored it. ⚠ The same unreproducible split is still written into
  `scripts/check-anchor-balance/check-anchor-balance.py`'s comment above `TRIGGER_FIRED`; that file
  is not this section's to edit and the correction is REPORTED, not smuggled.
- **A guard registered by a ctest entry that passes no flag can execute ZERO arms.** ✔MEASURED:
  `no_abort_in_tests_guard` ran **0** self-test arms at HEAD and now runs **16**, with no
  `CMakeLists.txt` change — the arms are printed by the registered no-argument form itself
  (`python scripts/check-no-abort-in-tests/check-no-abort-in-tests.py`), so the number is
  re-derivable and this line is not its source. ⚠ The first draft here said 15, counted by hand.
  And `check-plan-citations`' `main` returned before its self-test whenever the tree reddened,
  so in a shared tree its self-test had not run once all session. **P28 registered four unregistered
  guards; P29 found that registration alone does not mean the guard checks anything.**
- **`run-gate.ps1` silently swallowed `-V`, `-v` and `-D`** out of the pass-through array — rc 0,
  success witness still matching. ★ `ctest -V` is precisely the flag this repo uses to prove a
  registered entry executes its arms, so the one instrument for *"did this guard run anything"* was
  itself being discarded, and the wrapper's footer printed the command with the flag ALREADY GONE.
  ⚠⚠ **THE FIRST DIAGNOSIS WAS HALF RIGHT AND THE HALF IT NAMED DID NOT MATTER:** it blamed
  `[CmdletBinding()]` and sized the fix as "delete one line". ✔Applied and re-measured — `-V` was
  **still** dropped, because a `[Parameter()]` attribute on ANY parameter makes a script advanced on
  its own. **A row whose remedy does not work is worse than an open row**, because the next reader
  applies it and believes the defect is fixed. ✔The `.sh` twin uses `"$@"` and never had it: a live
  `.sh`/`.ps1` behavioural divergence inside the tool whose own header names divergence as its
  subject.
- **`check-ninja-deps` exited 0 on a directory that does not exist**, and the gate reference told
  every cycle to invoke it on `build-dbg` — a flat-layout path gone since the one-root migration. ⇒
  the build-verifiability check had been running against nothing and reading rc=0 as a pass.
- **`check-plan-citations --write` could RAISE a ratchet ceiling** — the command its own failure
  message tells you to run after a red. ✔Replaying every revision of the inventory across 7 commits:
  **no ceiling has ever been raised without the guard being widened in the same change** (17 raises in
  one commit, all reproduced exactly by re-running that commit's census over the prior tree). The
  claim is now measured rather than assumed — but nothing in the tool could have told a widening from
  a regression.

### ★★★ TWO LIVE SILENT MISCOMPILES, BOTH FOUND BY BYTE-DIFFING AGAINST THE REFERENCE

- **A value-producing x86-variable encoding variant with no `resultSlot` emitted register field 0.**
  `movw $42, %cx`, `%dx`, `%bx`, `%si` and `%r15w` all produced the identical `66 c7 c0 …` — every one
  of them writing `%ax`. rc=0, no diagnostic, **and the corpus example was GREEN through it**. Found
  only by a byte diff against gas. The loader now refuses the shape (scoped to the ENCODING SHAPE, not
  a target), with both exemptions measured against the shipped tables and pinned.
- **`"=m"` lowering the operand's VALUE where its ADDRESS belongs** compiles rc=0 with no diagnostic
  anywhere and dies at run time with `0xC0000005`. Only EXECUTION sees it — which is why the corpus
  example asserts a result rather than the absence of an error.

### ★★ THE CONFORMANCE WORK — and one divergence that is now a §B in front of the operator

`"=m"`, `"+m"` and `"=&m"` compile, link and **RUN** on both shipped targets at debug and release —
with **zero functional lines** changed in `mir_to_lir.cpp`, because a memory-form operand written in
the output section belongs in `MirAsmDescriptor::inputs`: its one MIR operand IS its address, and it
produces no result piece. Then x86_64 gained memory-destination arithmetic, the 8/16-bit ALU family,
a 2-byte immediate slot and 11 extending-move spellings: **83 of 86 probed spellings now compile**
where 0 did, 58 byte-identical to GNU as 2.42 and the rest re-decoding to the same instruction text.
The 3 still refused are refused by gas too — conformance running in both directions.

⚠⚠ **AND `%N` MEANS A DIFFERENT REGISTER WIDTH IN DSS THAN IN THE REFERENCES — ON aarch64 ONLY.**
✔MEASURED from emitted code: on aarch64 gcc and clang substitute bare `%N` as the 64-bit `x` register
regardless of operand type, while DSS substitutes at the operand's own type width; on **x86_64 all
three agree with DSS**. So *"the reference rule"* is a PER-TARGET property, and neither fix arm may be
an architecture branch. ★ The shape that makes this more than portability:
`long long m; int v; __asm__("str %1, %0" : "=m"(m) : "r"(v))` — **DSS exits 1** (4-byte store, high
half stale), **gcc exits 42**, and neither compiler says a word. ★ **BLAST RADIUS — MEASURE IT, DO
NOT RE-QUOTE IT.** The detector is the row's own and it is fail-closed:
`clang-19 --target=<t> -fsyntax-only -Wasm-operand-widths`, counting
*"value size does not match register size"* and refusing to read a zero out of a file whose parse rc
is not 0. ✔At the P29 lock it found **8** divergent operand references in
`examples/c-subset/c_inline_asm_memory_output_operand/main.c` and **0** anywhere on x86_64. ⚠⚠ **THE
`ONE FILE` HALF OF THAT SENTENCE IS ALREADY FALSE AND THAT IS THE POINT:** the same detector re-run
over the worktree finds **a second example diverging, `c_inline_asm_memory_arithmetic`, with 19**
aarch64 references, because its aarch64 arms bind `int` operands. **The universal claim is what
survives — every inline-asm example whose aarch64 arms use `long`/`long long` is unaffected, and
every one that binds a narrower type diverges** — so state the rule, run the detector, and never
trust the integer. ⇒ `D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE`, **§B, both arms sized,
and option (B) is not the cheap one**: diagnosing the divergence needs the same per-target
natural-width declaration that fixing it needs, and (B) can never make `char`/`short` work on aarch64
at all.

### ★★ THE WRAPPED-ANCHOR ARC CLOSED, AND THE DEFECT WAS WORSE THAN THE ROW SAID

Detector FIRST (registered, 35 self-test arms, fixtures synthesized at run time so nothing on disk is
a deliberate wrap), then the un-wrap in a quiet tree: **290 sites across 145 files → 0**, ceiling set
empty. ★★★ **A WRAP DOES NOT ONLY MAKE AN ID DISAPPEAR — IT MINTS ONE.** ✔Un-wrapping four sites in
this file removed **five phantom keys** from the harvest; the sharpest, `D-TO-PINNED-ARCHIVE`, is not a
truncation of anything — it is the trailing `D` of the word `GENERALISED` joined to `-TO-PINNED-ARCHIVE`.
✔And the row's central claim, finally measured: **six anchor ids had ZERO greppable citation anywhere
in the repository** and now have one. ⚠ **The independent review of the 288-edit log changed the
result**: six results ended a line on the file's own `--` with the next opening `D-SOMETHING` — the
defect's exact silhouette, invisible to every guard — so the whole pass was reverted to its byte
copies and re-run three times under four new rules.

### ⚠ WHAT THIS CYCLE OWES THE NEXT ONE, and it is a queue, not a mood

1. **`D-DIAG-OPERATOR-FACING-DIAGNOSTIC-CITES-A-CLOSED-DEFERRAL-ROW` — the row THIS cycle opened, so
   it takes slot 1 by the standing order's own refinement, not by preference.** ✔MEASURED against the
   registry's own CLOSED set: **476** anchor ids sit inside C++ string literals under `src/`, **271**
   name a CLOSED row, and **72** of those are inside a call to a reporting entrypoint — **71 after the
   one instance adjudicated and fixed in P29**. ★★★ **THE HARD HALF IS THAT THE REFUSAL CAN BE REAL
   WHILE THE ATTRIBUTION IS FALSE**, which is why no sweep can do this: the fixed instance refused
   correctly and deliberately, and only the row it blamed had moved. Citing a row as PROVENANCE stays
   legitimate after it closes; citing it as a LIVE BLOCKER does not — and only reading the site tells
   them apart. ⚠ **DO NOT CLOSE THIS BY WIDENING `check-stale-refusal-citations`**: that guard demands
   a persistence word near a refusal word, these carry none, and it is CORRECT to stay silent —
   widening it re-opens the false-positive surface its six measured narrowings (147 → 9) exist to
   close. The natural close is the census productionised as its own ratcheted guard, ratchet starting
   at the measured 71 so no NEW instance can land during the burn-down.
2. **`D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE` — ✅ RULED 2026-08-24, and the ruling
   dissolved the fork rather than picking an arm.** The operator's instruction was to VERIFY the
   premise before designing, and verifying it killed it: ✔MEASURED across 16 data points on both
   oracles, the references agree on the SPELLING and differ only on the DERIVATION — aarch64 bare
   `%0` renders `x0` for char/short/int/long, x86_64 renders `%al`/`%ax`/`%eax`/`%rax`. ★★
   **`%w` MEANS 32 BITS ON aarch64 AND 16 ON x86-64**, so any SHARED letter table is wrong by
   construction and the width-view vocabulary belongs to the DIALECT document — which is where
   P30 put it, with zero changes to `mir_to_lir.cpp`. ⚠ RESIDUAL: tranche 1 (respelling 27 operand
   refs in two corpus files, and 8 shipped examples whose comments still claim the modifier is
   `S0067`-refused) is unblocked and NOT yet done.
3. **✅ SUPERSEDED 2026-08-24 BY A BATCH RULING — and the figures this entry carried were both
   wrong.** ⚠ It said *“16 rows that need an operator decision and 16 that are INCONCLUSIVE”*.
   ✔RE-MEASURED from the registry: CLASS 4 is **11**, and there is **no INCONCLUSIVE census at
   all** — three rows use the word, about a test oracle's verdict, never as a census class. The
   16+16 came from re-quoting this file instead of re-deriving from the registry, which is the exact
   failure this file warns about in its own header.
   ★★★ **THE RULING THAT REPLACED ALL OF THEM (operator, verbatim):** *“SHIP
   EVERYTHING that is C conformance that gcc, msvc or clang accepts/rejects that we do wrong!
   EVERYTHING MUST BE DONE. JUST RESPECT THE 4 PARALLEL LANES AT A TIME”*, extended the same day
   to **“production backend pending stuff = MUST DO EVERYTHING, along with C + ASM full
   conformance”**. ⚠ I first narrowed this to exclude new TARGET/FORMAT decisions (ILP32,
   big-endian, static Mach-O) and was corrected on that exact sentence — **“-> this is also a
   MUST BE DONE!!!!!!!!”**. ⇒ **THERE IS NO OUT-OF-SCOPE-BECAUSE-IT-IS-A-TARGET BUCKET.** The
   only deferral is C++, gated on C + ASM being done. ★ Order the queue by the DEPENDENCY GRAPH:
   shipping a target FIRES triggers on rows gated for months.
   ⚠ `D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS` stands unchanged as the one row whose
   MUST-NOT-BUILD ruling sits over a trigger the row itself records as ALREADY TRUE.
4. **`D-PLANS-GATED-ROWS-NAME-NO-OPENER` IS STILL OPEN — the one addressable P28 row this cycle did
   not close, and the one whose debt it GREW.** ✔MEASURED, both predicates over the SAME tree so the
   predicate change is isolated from the registry change: the base-ref `is_gated` finds **30**
   openerless gated rows, the corrected one **70**
   (`python scripts/check-anchor-balance/check-anchor-balance.py` prints the live figure on every
   run — read it there, not here). That jump is the instrument finally seeing what the rule always
   condemned, **not debt this cycle created** — and the arm is DEBT-reporting, not fatal. ⚠ The row
   itself still quotes P28's `59`, measured at `cf27fe8b` under the old predicate; the registry is
   another lane's file this cycle, so that staleness is REPORTED here rather than edited.
5. **A structural gap the census found in the operator's own rule:** five rows wait on something
   schedulable that has **no ROW** — the OPT5 LIR address-mode phase, the separate-compilation driver,
   Plan-16 CS1, the `gui` profile. Plans 16/22/27 schedule these as phases and declare no anchor rows,
   so *"name the ROW"* cannot be satisfied without minting placeholders. Classified (4) and the
   DECISION named instead of a row invented.

6. **✅ DECIDED 2026-08-24 — BIG-ENDIAN IS `s390x`, AND THE CHAIN WAS VERIFIED BY EXECUTION BEFORE
   THE DECISION WAS WRITTEN.** Plan 23 **FC19** carries it in full. The short form: every conformance
   claim this project has ever made was measured on little-endian machines only, while struct/union
   layout, bit-field allocation, integer representation, `unsigned char` aliasing and the
   `__BYTE_ORDER__` predefines are all C-visible and all endianness-dependent. s390x is chosen because
   it is the ONLY commercially live big-endian platform — real distro port, real glibc, therefore the
   only one that can run the sqlite corpus, which is what *“real support”* has to mean.
   ✔MEASURED on WSL 2026-08-24: `s390x-linux-gnu-gcc 13.3.0` links a HOSTED binary (`ELF64`, big
   endian, `Machine: IBM S/390`); `qemu-s390x 8.2.2` RUNS it and a program returning the first byte of
   `0x12345678` exits **18 (0x12)**; a hosted `printf`+`malloc`+`strcpy` program prints
   `0102030405060708` and exits 42; `sizeof(long)==8`, so LP64 like our existing elf64 legs and NO
   data-model confound. ★★ s390x is big-endian in its INSTRUCTION STREAM too
   (`eb bf f0 58 00 24 stmg` reads MSB-first), which is strictly more than `aarch64_be` can ever test.
   ★ **This is what makes `D-ASM-TARGET-DECLARES-NO-BYTE-ORDER` dischargeable** — not by
   argument, but by a target existing. ⚠ The row was RIGHT to stay trigger-gated: an earlier pass
   proposed `aarch64_be` because it was CHEAP, and that was withdrawn — `appendLittleEndianBytes`
   being unconditional is **not a defect today**, and building a big-endian target purely to make our
   own code wrong is the speculative build the bar forbids. **A cheap way to build something is never a
   reason to build it; a real target on the other end is.**

7. **✅ DECIDED 2026-08-24 — C STANDARDS BECOME LANGUAGE DOCUMENTS, AND THE FRONT END IS RENAMED
   `c`.** Plan 23 **FC20 / §2.G** carries all seven decisions. The three a next cycle most needs:
   (a) the versioning relation is **NEW and SEPARATE** from `languageReferences` — embedding keeps
   its collision-refusal at full strength, the new relation gets override/removal as its whole purpose;
   (b) `baseLanguage` names the **PARENT**, so family is *walk to the root* and `--source=c` means the
   newest document whose root is `c`; (c) **the first shipped link is `c23` ALONE, parentless**,
   because ✔measured, DSS accepts none of the three constructs C23 removed — so a truthful
   `c11.lang.json` must CONTAIN K&R definitions, implicit int and implicit function declarations, none
   of which DSS has ever had. The edge direction and the ship order are independent because the
   MATERIALIZED SET is the contract; `c23` is re-parented onto `c17` later and proved byte-identical.
   ★ **The selector already exists** — ✔`--language c23` today resolves
   `src/dss-config/sources/c23.lang.json` and fails only because no such file exists.
   ⚠ TWO MEASURED CONSTRAINTS: transitive references are currently REFUSED (one hop only, by design),
   and **exactly one document may claim `.c`/`.h`** or every extension-resolved compile refuses.

8. **✅ DONE 2026-08-24 — THE RENAME LANDED AND THE CONFORMANCE QUEUE IS UNBLOCKED.** (Kept for its reasoning, which stays true of the next large mechanical change.) **It was the bottleneck for the whole conformance queue, and it needed an EMPTY TREE.**
   ✔MEASURED: **1,429 path renames and 7,583 content substitutions across 1,002 files**
   (`c-subset`→`c` · `c_subset`→`c` · `CSubset`→`C` · `csubset`→`c`), with
   `CSUBSET` — the 6,547 citations of the 426 frozen `D-CSUBSET-*` ids — structurally out of reach
   because every substitution is case-sensitive. ★★★ **OPERATOR RULING: the anchor ids are
   FROZEN and BOTH `D-C-*` AND `D-CSUBSET-*` DENOTE THE C LANGUAGE**; renaming them would break
   archaeology from every commit message that cites one, for a prefix that is opaque anyway.
   ⚠ It must land as its OWN COMMIT — a 7,583-substitution mechanical diff mixed with any logic
   change is unreviewable, and the whole value of a pure substitution is that reviewing it is a
   verification rather than a reading. ⚠ And it cannot run while any lane holds `src/**` or
   `examples/**`: renaming the language document underneath a lane does not merely conflict, it changes
   what that lane's binaries MEAN between two runs.
   ★ **Why it blocks the queue:** 17 of the 18 direction-A divergences are GNU extensions the front
   end must learn (`__extension__`, `__COUNTER__`, statement expressions, `__builtin_expect`, case
   ranges, `__label__`, `__builtin_types_compatible_p`, `__builtin_choose_expr`, `__builtin_offsetof`,
   `__real__`/`__imag__`, `__alignof__(expr)`, `__attribute__((constructor))`,
   `__attribute__((alias))`, `__thread`, `_Thread_local` definitions, `[[maybe_unused]]` on a
   parameter, `__try`/`__except`) — all of them edits to the very document the rename moves.

ⓘ **An incident recorded rather than buried:** one lane ran `git stash push` inside a compound command
by mistake and stashed the whole shared tree, recovering it immediately with byte-for-byte
verification. Nothing was lost and no other lane was live — but a stash in a nine-lane uncommitted
tree has no commit to recover from, and the next brief says so.

---

## 0.0000000000000000000000000000000 ★★★ CYCLE P28 — THE GATE THAT COULD NOT OPEN, TWO LIVE SILENT MISCOMPILES NOBODY WENT LOOKING FOR, AND A HIGH DEFECT THAT SHIPPED BEHIND A GREEN 1539-TEST SUITE

**Operator argument 2026-08-23:** *"let's resume the cycle per the handoff. we're on a clean branch
from main."* The pick was re-derived from the REGISTRY rather than from the queue list, and the
queue turned out to **contradict itself**: one entry made
`D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS` P26's mandatory first item (operator
Condition 3), while the entry directly above it said ⛔ MUST-NOT-BE-PICKED. The registry row —
written later the same day — governed, and the cycle reported the conflict rather than choosing.

### ★★★ THE OPERATOR RULING THAT RESHAPED THE CYCLE, AND THE MEASUREMENT THAT PROVED IT RIGHT

> **BUILD THE 128-BIT VR OPERAND. It is not gated — it is the GATE-OPENER, and treating it as an
> external precondition is what was turning a sequencing gate into a permanent one.**
> The ⛔ is on the CONSUMER (the allocator), never on the PRODUCER (the operand).
> **The trigger does not fire on its own. We fire it.**

✔**MEASURED, and it confirms the diagnosis with a mechanism attached:** the row's trigger —
*"a source construct can form a 128-bit operand that reaches the allocator as a VR-class value"* —
was **ALREADY TRUE, and was true before the row was written**. `long double y; __asm__("nop":"=w"(y));`
on arm64 reports `R_SpilledDueToPressure … spilled 1 vreg(s)`: a 128-bit value, VR class, a
**virtual** register in the allocator's own report.
★★★ **AND THE ROW'S OWN INSTRUMENT COULD NEVER HAVE SAID SO.** It prescribed *"assert the build
does NOT emit `R_SpilledDueToPressure`"* — which cannot become true until the free list is
non-empty, **which IS the fix**. That is a restatement of the remedy, not a detector. ⇒ **the gate
had been open the whole time and nothing could see it.** A tripwire that can actually flip now
lives in `lir/test_lir_aliased_view_allocability` arm (A), and Condition 4's marker — required
2026-08-21 and never built — now exists in `src/` and `tests/` rather than only in `.plans/`.

★★ **THE STAGING'S STEP 2 IS UNREACHABLE, AND THE OPERATOR ASKED TO BE TOLD.** *"Accept `"w"` for
the simple unaliased case; it should go green WITHOUT the allocator."* ✔MEASURED: **no.** arm64
declares **32** cross-class `dwarfNumber` collisions (d↔v, 64..95) and **all 32 d-views are already
allocatable**, so every V register aliases an allocatable one. **"Unaliased" is a property of the
SOURCE CONSTRUCT; the aliasing that breaks this is a property of the TARGET'S REGISTER FILE.** The
naive fix was built as a mutant and the double-count reproduced: `fadd d14, d7, d14` for the `"w"`
value and `fadd d14, d14, d7` for an ordinary `double` — **one physical register, two live values,
rc=0, no diagnostic.** ⇒ steps 2 and 4 are one step, and that failing case is the allocator arc's
witness, in hand.

### ★★★ TWO LIVE SILENT MISCOMPILES, NEITHER ON ANY QUEUE, BOTH FOUND ON THE WAY

* **`D-LIR-ASM-OPERAND-MOVE-IS-CLASS-BLIND`** — the inline-asm lowering resolved ONE module-wide
  integer `Mov` per asm block and used it for EVERY operand whatever class it bound. ✔arm64:
  `fmov d15, d0` / **`mov x29, x15`** — reads integer register 15 while the value is in **d15**.
  ✔x86_64: **`mov %r13,%r13`** — the copy into the operand's own xmm13 is the integer mov on r13, a
  no-op, so the template reads a register nothing ever wrote. Both rc=0, no diagnostic, reachable
  from ordinary C with no flag. ★ **The comment on `classOp` described this exact failure while the
  table sat unused** — a comment recording the FULL fact while the code used HALF of it.
* **`D-LIR-FRAME-SLOT-STRIDE-ENUMERATES-CLASSES-INSTEAD-OF-DERIVING`** — the frame stride was
  `max(GPR, FPR)`, a two-member enumeration of a longer vocabulary. arm64's `vr` is 16 bytes.
  ✔Two 16-byte slots **overlapped by 8 bytes** and the second read **8 bytes past the top of the
  frame** (`redZoneBytes` 0 ⇒ the caller's stack). ★ Zero blast radius proven on the corpus rather
  than argued: the sqlite3 amalgamation is **byte-identical** across the fix.

### ★★★ THE BRANCH'S HIGH ROW CLOSED — AND ITS SCOPE WAS MEASURED TOO NARROW TWICE

`D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`. The remap died with `UnitBuilder`, so the PARSE
tier was the only tier alive to use it. ✔Reproduced exactly at HEAD, then closed on all **5 legs**.
Two under-callings the row did not know:
* the **ASM/LIR tier carried both shapes too** — a semantic-only fix would have been a slice;
* the row's caveat *"on an elf leg with no `--define`s the shift is ZERO"* holds only for a
  **splice-free** TU: an elf, zero-define case was shifted **+2 by the spliced header's own lines**.
  ⇒ the shift is (target prologue) + (one per `--define`) + (lines spliced above), i.e. **every
  quote-including TU was shifted on every target.**
★ It also discharged FC18's blocker as a side effect: ✔the corpus golden pins `1:1-1:13` and the
CLI on pe64 now prints `1:1` — the two instruments AGREE, so FC18 may pin positions without the
caveat that a corpus would certify green a CLI printing shifted ones.

### ★★★ A HIGH DEFECT SHIPPED BEHIND A GREEN 1539-TEST GATE, AND THE REASON IS THE FINDING

`D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH` — `dss --compile main.c`
could not find `#include "sibling.h"` beside it, while `./main.c`, `sub/main.c` and an absolute
path all could; ✔gcc resolves the bare form on both hosts. Root cause: an **empty `parent_path()`
read as "no directory" when it means the PROCESS WORKING DIRECTORY**, in **four** copies of one
derivation. ★ The guard that skipped the arm **was load-bearing when written and protects nothing
today** — so it was KEPT as the contract for *"there is no including file"*, with the reasoning
written down, rather than deleted as the obvious culprit.
★★★ **AND THE REASON IT SURVIVED A GREEN SUITE:** ✔both example runners always hand `--compile` an
**ABSOLUTE** path, so **3 of the 4 source-argument shapes a user can type are exercised by nothing
in the corpus** (`D-HARNESS-EXAMPLE-RUNNERS-ALWAYS-COMPILE-AN-ABSOLUTE-SOURCE-PATH`, opened with a
named blocker: it moves the compile cwd for ~580 examples).

### THE GUARD LANE — INCLUDING THE ONE THAT JUDGES THIS CYCLE

* `D-GATE-BALANCE-EXEMPTS-A-DISCLOSED-OPENING-BUT-NOT-A-BOOKKEEPING-CLOSURE` — the mirror exists and
  is **net-neutral by construction**: the OPEN population drops, the cycle is credited with nothing.
  ✔`is_closed()` is BYTE-IDENTICAL, so every row carrying no new marker produces the number it
  produced before. Two corrections to the row's own method were measured: the walk-back test must
  read the CLOSING-WORK cell, and must NOT read the whole status cell (mean 1,881 chars, max
  39,753, so `HALF`/`RESIDUAL` occur by accident).
* `D-GATE-A-GATED-ROW-MUST-NAME-ITS-OPENER` — the operator's §4 rule, built. ★★ **The census changed
  the predicate:** ✔the ⛔ glyph LEADS only SIX status cells and five of those are REFUTED-DESIGN /
  NEGATIVE RESULT, which can never have an opener; the rows the ruling is about **declare themselves
  in words** (94 rows, 62 open, 59 nameless). ⚠ And the arm's first shipping **flagged the census row
  that recorded its own finding** — a description of a class classified as a member of it, the third
  instance of that shape in one cycle. `is_gated` now excludes a row stating its trigger has ALREADY
  FIRED. ✔The obvious alternative was measured and rejected: windowing the declaration drops the
  gated population **63 → 37** and loses 26 genuine gates, because in the registry shape that cell is
  the **Trigger column** — a trigger description legitimately fills it, while a closure verdict leads
  it. **Same cell, two conventions.**
* `D-TEST-A-NEW-WALL-CLOCK-LITERAL-IN-A-TEST-IS-UNGUARDED` — ★★★ **the row's three named shapes would
  have measured GREEN over the whole live population.** ✔`wait_for(`/`wait_until(`/`now() + ` account
  for FIVE sites, all already routed; the single idiom `runBinary(exe, chrono::milliseconds{5000})`
  accounts for **TWENTY-SIX**. So the guard refuses the COMPLEMENT: a chrono literal is a wall-clock
  literal unless it is a sleep.

### THE LINKER LANE — AND A DOCUMENT COUNT THAT WAS WRONG IN THE BRIEF

`D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS` closed with the accessor taken
in the same change (its stated precondition — a second consuming writer — arriving *in* it).
⚠ The design audit predicted **20 of 24** documents; ✔the measured answer is **16**, because the
Mach-O IMAGE arms encode no weak definition at all and declaring it there would have created the
very key-nobody-reads the parent row exists to prevent. ★ The lane went one step past its brief and
folded all three walkers onto ONE shared gate — which **fixed a live hole in the pe copy**: it
consulted `definedBinding` only, first-row-wins, so a weak ALIAS of a strong definition (what gcc
emits for `__attribute__((weak, alias(...)))`) passed the gate UNASKED while the alias pass still
put a weak definition on the wire.
`D-LK-COFF-NAMELESS-UNDEF-EXTERN-SILENTLY-DROPPED` — decision TAKEN, not escalated, because the bar
decides it once by-index reachability is measured: a dropped nameless record still occupies a
`NumberOfSymbols` slot, so a relocation names a SymbolId the reader never produced, and it re-emerges
at MERGE time as an unresolved symbol **with no name to print**. ✔Blast radius measured first — five
real `cl.exe`/`lib.exe` witnesses RAN (15,914 ms) and stayed green.

### THE CYCLE'S OWN MACHINERY BROKE, AND IT IS ANCHORED

`D-CYCLE-LANE-CTEST-PARALLELISM-IS-UNBOUNDED-IN-AGGREGATE` — the isolation rules divided the files,
the build trees and the scratchpads, and **nobody divided the machine**. ✔Four lanes each inheriting
P17's `-j 8` gave 32 concurrent test processes; example tests went **~6 s → ~200 s** and a lane
**abandoned a 639-test gate it had already earned**. ⚠⚠ **The first sizing written for the fix was
itself wrong within minutes:** `cores / N` on a 32-core host IS four-lanes-at-`-j 8` — the rule as
first written would not have prevented the failure it was written for. **A ctest ENTRY IS NOT A JOB**
(616 entries each spawning a CLI; builds compete too), so the budget starts at `cores / (2N)` and the
signal to watch is per-test wall clock against its baseline, not CPU%.

---

## 0.000000000000000000000000000000 ★★★ CYCLE P27 — THE BUDGETS WORKED, AND WHAT THEY UNCOVERED WAS TWO TESTS THAT HAD NEVER RUN HERE

**Operator argument 2026-08-22:** two still-failing jobs on CI run 32585879580. ★★ **The P26 fix did its
job and that is the first thing to read from this run:** `macos-clang-release` is **GREEN** — the bash 3.2
repair holds in CI — and both remaining legs now **REACH AND FINISH** ctest inside their budgets:

| leg | ctest | budget used | what remained |
| --- | --- | --- | --- |
| linux-clang-asan | 4528 s | **68%** of 6600 s | 1538/1539 — `lsp/test_workspace_project` |
| windows-msvc-release | **665.70 s** | **10%** of 6600 s | 1538/1539 — `harness/test_sqlite_harness_legs` |

★ That Windows figure is the one [[D-CI-WINDOWS-CTEST-COST-IS-UNMEASURED]] was opened for — the leg had
never completed a Build step, so its budget was 🧠INFERRED. It is measured now, the row is **CLOSED**, and
the budget is re-derived to **50/60** like the other release legs. ⚠ Its BUILD budget stays **120**: that
run had a **98.72%-hit** ccache and built in **1m48s**, while the run killed at 45 minutes had a 100% MISS.
**The cold Windows build has still never been seen to completion** — said, not assumed away.

### ★★★ THE asan FAILURE IS A TWO-SECOND DEADLINE, AND THE PROOF IS A REPRODUCTION, NOT AN ARGUMENT

`WorkspaceProjectE2E.ASaveThatChangesNoManifestRepublishesNothing` failed with `Which is: -1` — which
reads as a wrong exit status and is not one. `-1` is `runUntilExit`'s TIMEOUT sentinel, against a
hard-coded `std::chrono::seconds(2)`.

✔**MEASURED by rebuilding the CI leg exactly** (`clang-19`, Debug, `-fsanitize=address,undefined`, ✔665
`__asan` symbols in the linked binary):
* idle host: the test passes in **622 ms**;
* same binary, CPU oversubscribed 3x: **1912 ms** — 3.1x, and within **88 ms** of the deadline;
* CI is a harsher version of that experiment — 4 vCPUs running `ctest --parallel 4` of sanitized
  binaries — and it crossed, at 2204 ms.

★★ **The deadline was sized on an idle developer machine and had no margin for the slowest host that runs
it.** That is [[D-CI-BUILD-AND-CTEST-BUDGETS-WERE-ONE-NUMBER-FOR-FIVE-LEGS]] one level down, failing in the
same direction: a red on a leg where nothing is wrong. One shared `kWaitBudget` (60 s) now carries the
derivation once and the four hand-written deadlines under `tests/lsp/` point at it. ✔With it, the whole
12-test `WorkspaceProjectE2E` suite passes under that same 3x contention.
⚠ **And the message named the wrong event.** All ✔27 call sites compare the return against an expected
EXIT CODE, so expiry now adds a failure that says TIMEOUT and names the budget.
`D-TEST-LSP-WAIT-DEADLINE-IS-SIZED-FOR-AN-IDLE-HOST`

### ★★★ THE WINDOWS FAILURE IS `bash` MEANING WSL — THE REPOSITORY'S OWN LESSON, REACHING AN INSTRUMENT

`harness_legs.py --check-regions` is the only instrument that compares the two sqlite drivers BY
EXECUTION. It resolved its `.sh` arm with `shutil.which("bash")`. ✔On `windows-latest` that answers
`C:/Windows/System32/bash.exe` — **WSL's** — on a runner with **no distribution installed**, so every
`.sh` arm exited 1 with `Windows Subsystem for Linux has no installed distributions.` and **26
differential assertions** reddened on a host where nothing was wrong.

★★ **THE BATTERY'S RULE WAS RIGHT AND ITS PRESENCE ANSWER WAS WRONG.** *"A PRESENT interpreter that could
not run its arm is a FAILURE, never a skip"* is exactly what stops a host quietly dropping coverage. Fed a
`which` result it convicted the host — its diagnostic even reads *"this host HAS bash"*. It does; it is the
wrong one. ✔On this repository's Windows workstation `which("bash")` answers Git Bash (`$OSTYPE` **msys**)
and `System32\bash.exe` answers **linux-gnu**; the runner's PATH orders them the other way round, which is
the entire difference between green here and red there.

**FIXED:** the resolver PROVES its answer by running the candidate; on Windows a Git Bash is tried before
the PATH answer; and no usable bash makes the language ABSENT with a reason, which the existing
host-independent inventory already skips correctly instead of convicting.
`D-HARNESS-MIRROR-BASH-RESOLVES-TO-WSL-ON-A-WINDOWS-RUNNER`

### ★★★ AND THE FIX BROKE THE OTHER LANGUAGE — CAUGHT BY ASKING A THIRD HOST BEFORE COMMITTING

The bash repair replaced `have[lang] = which(interp[lang])` with `have[lang] = mirror_interpreter(lang)
is not None`. Right for `sh`; **wrong for `ps1`**, whose branch returns an argv unconditionally — so a host
with no PowerShell became "present". ✔MEASURED on the macOS host, same machine, pre-change against
post-change: **`passed=356 failed=0 skipped=15` → `passed=356 failed=25 skipped=0`**. Fifteen honest skips
became twenty-five failures on a host whose only fault is having no pwsh.

★★ **It is the same defect class as the one being fixed, pointing the other way** — and Windows and WSL
both have pwsh, so **both were green and neither could ever have shown it**. The fix restores `which`
for `ps1` (bash earned the stronger run-probe by being measured wrong; nothing has measured pwsh wrong,
and changing behaviour without a measurement is exactly what produced this), and presence is now pinned
in BOTH directions for EVERY language rather than for the one that broke.
`D-HARNESS-MIRROR-PS1-PRESENCE-BECAME-UNCONDITIONAL`

★ The carryable rule: **when a fix changes how a capability is DETECTED, the hosts that lack that
capability are the only ones that can review it.** A green two-host gate said nothing about this.

### THE GATE

| leg / check | result |
| --- | --- |
| Windows ctest, `build/dbg` (`-j 8`) | ✅ **1539/1539, 517.17 s** |
| Windows harness + guards subset | ✅ **13/13, 122.31 s** |
| WSL x86_64, `wsl-leg` clean build through `run-gate` | ✅ **1539/1539, 298.86 s** |
| WSL guards under a real POSIX shell | ✅ **7/7** |
| qemu arm64, `DSS_STRICT_ARM_VERDICTS=ON`, scoped | ✅ **636/636, 93.27 s** |
| asan LSP — all 5 binaries, 3x CPU contention | ✅ **65/65, rc=0** |
| `harness_legs --self-test` | ✅ **1985/0 on Windows, WSL AND macOS** |
| `harness_legs --check-regions` | ✅ **371/0/0** (Windows, WSL) · **356/0/15** (macOS, byte-identical to the pre-change baseline) |

★ The last two rows are the ones that matter for this cycle: the same totals on three hosts is what
says the presence fix did not quietly change what any of them checks.

### WHAT THIS CYCLE LEFT OPEN, STATED RATHER THAN ABSORBED

`D-TEST-A-NEW-WALL-CLOCK-LITERAL-IN-A-TEST-IS-UNGUARDED` — the wait-budget fix unified the FOUR deadlines
that existed and ✔left none behind under `tests/lsp/`, but **nothing refuses a FIFTH**. The next one fails
exactly as this one did: green where it was written, red on the slowest leg, naming the wrong event.
Deliberately NOT built this cycle — the subject was two named CI jobs and a third guard is its own arc —
so it is sized in the row instead of rediscovered later. Balance therefore **1036 → 1036, net ±0**: two
born closed, one closed, one opened.

★ **THE SHAPE BOTH FAILURES SHARE, AND IT IS THE ONE WORTH CARRYING:** each is an instrument that was
CORRECT about its subject and WRONG about its environment — a deadline that assumed an idle host, a
presence check that assumed a name meant a program. Neither was findable on the machine that wrote it.

---

## 0.00000000000000000000000000000 ★★★ CYCLE P26 — THREE RED CI LEGS, AND THE macOS ONE WAS A GUARD THAT HAD NEVER RUN

**Operator argument 2026-08-22:** *"CI failed for linux-clang-asan, macos and windows"*. Three legs, and
not three defects: the macOS one is a shell dialect, and the other two are ONE budget defect
wearing two faces.

✔**MEASURED, CI run 32533052057 (2026-08-21) — and the first fact is about the RUN, not the legs:** this was
the **first run on this branch where the matrix actually executed**. Every earlier Pipeline run stopped at
`label-check` ("Missing 'Run Pipes' label"), so every leg came up with a **100%-MISS ccache**, and nothing
these three legs report had ever been observed here before.

| leg | cold build | ctest | verdict |
| --- | --- | --- | --- |
| linux-gcc-release | 19m22s | 274.19s | green |
| linux-clang-asan | 13m04s | **2999.79s of a 3000s cap** | RED — `conformance/test_reference_conformance` (Timeout) |
| windows-msvc-release | **>45m, killed at 453 of 786 edges** | never reached | RED — `The action 'Build' has timed out after 45 minutes` |
| macos-clang-release | 12m53s | 286.37s | RED — `anchor_registry_guard` |
| linux-arm64-gcc-release | 13m07s | 178.58s | green |

### ★★★ THE macOS LEG IS THE ONE WORTH READING — A `case` INSIDE `$( … )` IS UNRUNNABLE ON bash 3.2

`anchor_registry_guard` did not fail on the registry. It died **inside its own self-test**, in
`SELF-TEST arm 'missing-root-refuses-and-says-so'`, with `command substitution: syntax error near
unexpected token`. **So the anchor registry had never actually been checked on macOS.**

bash 3.2 — which is what `/bin/bash` is on **every** macOS host, including `macos-latest`, and always will be
— does not recursively parse a command substitution. It scans forward for the matching `)`, counting parens.
The `)` that closes a **`case` PATTERN** therefore ends the `$( … )` early, and the arm expands to LITERAL
TEXT. ✔MEASURED on macOS 26.5.2 (`/bin/bash` 3.2.57) against bash 5.2.21 in WSL, one `bash -c` per construct,
verdict taken from the **OUTPUT** and not the exit status — **16 of 17 constructs unsupported on 3.2, 17 of 17
on 5.2**, the single survivor being the `(`-prefixed pattern form. The POSIX-optional leading `(` balances
the count and is the whole fix.

★★★ **AND `bash -n` IS BLIND TO IT.** ✔The probe file parses clean under 3.2 (exit 0) and fails only when the
substitution is **EXPANDED**, because that is when 3.2 parses the text it extracted. **The obvious instrument
— syntax-check every script under the old shell — cannot see this class at all.** That is why the fix is a
static guard and not a pre-check, and it is the reusable half of this cycle: *when an instrument is blind by
construction, adding more of it is not the answer.*

⚠ **SIX OF THE TWELVE bash-4 CONSTRUCTS THIS REPOSITORY USES LEAVE THE EXIT STATUS AT ZERO** — `mapfile`
prints `command not found` and the script continues; **`declare -A` fails and the name then behaves as an
INDEXED array, so every key subscripts to 0 and the table silently holds one entry**; `local -n` and `wait -n`
the same. Five of those six produce a **wrong answer** rather than a failure, on the one host that cannot be
checked from any other host.

**Shipped:** `scripts/check-shell-portability/check-shell-portability.py` (ctest entry
`shell_portability_guard`, **34 self-test arms**, every red arm asserting the MESSAGE). Two rules:
* **rule 1** — for every `$(`, the span bash 3.2 would extract must contain BALANCED `case`/`esac`. Stated as
  the property, not as a source pattern; a `case` that is **not** inside a substitution is perfectly safe on
  3.2 and this repository has dozens, so nothing is churned to prevent nothing.
* **rule 2** — a bash-4 construct requires a `BASH_VERSINFO` gate **above** its first use. The states each
  construct counts in are part of the measured table: an EXPANSION is a use inside double quotes, a COMMAND
  is not.

✔**RED-ON-DISABLE, AT THE LEVEL THAT MATTERS:** run against the tree exactly as it stood at `3ce4e336`, the
guard reports **five** violations — the three arms that killed the macOS leg, `test-driver-contracts.sh`'s
call-site arm, and `base-harness.sh`. On the fixed tree it is green.

⚠ **AND IT FOUND A DEFECT I SHIPPED LAST CYCLE.** `base-harness.sh` is `declare -A` and had **no** version
gate; the three entry scripts that source it each carried their own copy — and P25's `benchmark-speedtest1.sh`
became a fourth sourcer **without one**. On macOS it would have died at `declare: -A: invalid option`, an error
naming a shell builtin, raised by a file the caller never mentions. **A precondition checked once per CALLER is
a precondition a new caller can forget**, so the refusal now lives in the sourced file, where it cannot be.

### ★★★ AND THE GATE LEG CAUGHT THE NEW GUARD'S OWN FIRST DEFECT — ON THE HOST IT WAS WRITTEN FOR

The macOS leg run for THIS cycle reported `anchor_registry_guard` **PASSED (4.91 s)** — the fix works
under bash 3.2, which is the whole point — and in the same run `shell_portability_guard` **FAILED**,
with **seven violations against files that do not exist**.

✔MEASURED: the first version answered *"which scripts are ours?"* with `git ls-files`. The macOS
carriage's checkout sits at **`b52784a` (P5c)** with the working tree rsynced over it, so the INDEX
still named `tools/check-anchor-registry.sh`, `tools/run-gate.sh`, `scripts/build/local-build.sh` and
four more — every one deleted by the P17 `tools/` consolidation.

★ **THE INDEX DESCRIBES A COMMIT; THE GUARD IS ABOUT THE TREE.** Every carriage here has that shape,
so a guard keyed on the index reds on a HOST DIFFERENCE rather than a defect — and the next person to
see it red would be right to ignore it. ⚠ The earlier walk-based draft had the OPPOSITE defect (six
violations inside the gitignored `scratchpad/`, which holds verbatim lane backups of the very scripts
this cycle repaired), so **neither obvious answer was correct on its own**. The tree is walked and git
is asked only which of those files are IGNORED. `D-GATE-SHELL-PORTABILITY-SCANNED-THE-INDEX-NOT-THE-TREE`

★★ Worth stating plainly because it is the cycle's own control loop working: **a guard written to stop
a host-specific defect was itself host-specific, and the only thing that could have found that is the
leg on that host.** A local green would have shipped it.

### ★★ THE OTHER TWO LEGS WERE ONE DEFECT: TWO GLOBAL BUDGETS SET FROM THE LEGS THEY HAPPENED TO FIT

Neither leg is broken. The asan leg is Debug + ASan + UBSan — ✔**11,314 CPU-seconds against gcc-release's
935 (12x)**, which at `--parallel 4` is **47 minutes of PERFECT packing against a 50-minute budget**; the
slowdown is uniform (~27x per compute-bound test), so there is no pathological test to find. Windows compiled
**453 of 786** ninja edges in 45 minutes from a cold cache. **Neither number was ever wrong about its own leg.
One number for five legs was.**

`build_timeout_min` and `ctest_budget_min` are now **per-leg matrix fields** (windows build 120; asan and
windows ctest 110; the rest 45/50), the Test step's `timeout-minutes` is `ctest_budget_min + 10`, and the
measurement table sits above the matrix builder so the next reader meets the derivation rather than the
constants. ★ A budget is a **CAP, not a reservation** — a leg that finishes early costs nothing, and what a
cap prevents is a `timeout-minutes` SIGNAL kill that uploads no test log at all
([[D-CI-STEP-TIMEOUT-YIELDS-NO-TEST-LOG]]).

★★ **RAISING THE NUMBER WOULD HAVE DEFERRED THE SAME RED, NOT CLOSED IT.** ✔The asan leg reached **99.99% of
its budget with nothing having said it was close**, and **62%** of its cost is the **1,231-entry** corpus,
which grows every cycle. The Test step now times `ctest` with `$SECONDS`, prints
`ctest wall clock: Ns of a Ms budget (P%)`, and emits a `::warning::` naming the leg above 80% — so the next
breach is announced **while the run is still green**. ✔`$SECONDS` and no pipe, deliberately: `ctest` piped to
`tee` hands `$?` to `tee` under `bash -e {0}` (no `pipefail`), so a failing suite would report success.

✔**THE WORKFLOW BLOCK WAS EXERCISED, NOT READ**: the `Test` step was extracted verbatim, its GitHub
expressions substituted, and driven under `bash -e` against a stub `ctest` — green under budget (no warning),
**rc=8 preserved** on a failing suite, and exactly one `::warning::` at 55s of a 60s budget. The matrix builder
was likewise run through `jq` and emits the five rows with the intended numbers.

### ⚠ A THIRD DEFECT, FOUND BY WRITING THE FIRST FIX AGAINST THE HOUSE CONVENTION

`enum_name_table_guard`'s ctest entry **verified the tree and proved nothing**, while the CMake comment beside
it said *"It self-tests on every run (7 arms…)"*. ✔`main()` ran the self-test only under `--self-test`, and the
entry passes no flag — so it would have passed identically with every assertion inside `check()` deleted.
**The comment recorded the full fact while the code used half of it**, the shape this project keeps paying for.
Fixed to match its siblings (`check-scripts-index` and `check-orphan-tests` both had it right, which is what
made the odd one out visible), and the CMake comment now records that it was false rather than being quietly
corrected.

### ⚠⚠ AND THE FIRST PUSH SHIPPED A WORKFLOW GitHub REFUSES TO LOAD — WITH EVERY LOCAL INSTRUMENT GREEN

Commit `78f9a607` produced a CI run with **ZERO jobs**, `conclusion: failure`, and the FILE PATH where a
workflow name belongs. ✔The file parses as YAML, has no duplicate keys, no tabs, no CR, and validates
CLEAN against the official SchemaStore workflow schema — all four checked before the cause was found.

★★ **A WORKFLOW-LEVEL REJECTION LOOKS NOTHING LIKE A FAILING JOB**: no job, no log, no annotation
reachable through the API, and `gh run list` still prints the workflow's declared `name:`. The tell is
the EVENT — this workflow is `pull_request`-only, so a `push`-event run for it can only mean the file
was refused and reported against the push that changed it.

✔**MEASURED against GitHub's own validator**, two minimal `workflow_dispatch` files pushed side by side
on a throwaway branch (deleted once read): `timeout-minutes: ${{ matrix.t }}` is **ACCEPTED**;
`timeout-minutes: ${{ matrix.t + 10 }}` is **REJECTED**. The step timeout is a matrix FIELD now
(`ctest_step_timeout_min`), computed in the shell that builds the matrix.
`D-CI-STEP-TIMEOUT-MINUTES-REJECTS-AN-ARITHMETIC-EXPRESSION`

⚠ Worth carrying forward beyond this row: **a local YAML check and even the official JSON schema do not
tell you a workflow will load.** The only instrument that answers that question is a push, so a workflow
edit is not verified until a run appears with the jobs you expect — and "a run appeared and failed" is
not the same fact as "the jobs ran and failed".

### THE GATE — AND EVERY FIGURE SAYS WHICH TREE IT WAS TAKEN ON

★★ The guard was still being corrected while the long legs ran, so a full-suite number and a
final-tree number are NOT the same claim. Both are given, and which is which is said, rather than
quoting the convenient one. The rule is the cycle skill's own: a whole-tree gate number is not
attributable once the tree has moved under it — here applied to the orchestrator's numbers rather
than to a lane's.

| leg | result | tree |
| --- | --- | --- |
| Windows ctest, `build/dbg`, **serial** (no `-j`) | ✅ **1539/1539, 2470.00 s** | FINAL |
| Windows ctest, `local-build.ps1 -Test` (`-j 8`) | ✅ **1539/1539, 515.23 s** | mid-cycle |
| Windows guards subset (`-R` the seven guards) | ✅ **7/7, 68.01 s** | FINAL |
| WSL x86_64, `wsl-leg` clean build through `run-gate` | ✅ **1539/1539, 316.20 s** | mid-cycle |
| WSL guards under a real POSIX shell, `--mode guards` | ✅ **7/7** | FINAL |
| qemu arm64, `DSS_STRICT_ARM_VERDICTS=ON`, scoped `-R 'examples/.*\|lir/.*\|core/test_target.*'` | ✅ **636/636, 93.86 s** | mid-cycle |
| macOS arm64, `remote-leg` clean build through `run-gate` | ⚠ **1538/1539, 3601.49 s** | mid-cycle |
| macOS guards subset (`-R 'guard\|portab'`) | ✅ **12/12, 27.47 s** | FINAL |

⚠ **THE ONE RED IS THE ONE WORTH READING, AND IT IS NOT A PRODUCT FAILURE.** The macOS full suite's
single failure is `shell_portability_guard` — the copy synced BEFORE the index→tree correction, i.e.
the defect described two sections above, caught by the leg it was written for. In the same run
`anchor_registry_guard` **PASSED (4.91 s)**, which is the fix this cycle exists for. The corrected guard
was then re-synced and re-run on that host: **12/12**. Every other macOS entry was green in both runs.

★ The serial Windows figure (2470.00 s against 515.23 s) is not a regression: a bare `ctest` runs one
test at a time, while `local-build.ps1` and `run-gate` default `CTEST_PARALLEL_LEVEL=8`. Said here
because a number quoted without its `-j` is the shape this project has been misled by before.

### WHAT THIS CYCLE LEFT OPEN, AND WHY — both 🔵 DISCLOSED, both checkable in `3ce4e336`

* `D-CI-WINDOWS-CTEST-COST-IS-UNMEASURED` — the one budget in the new table **not measured on its own leg**,
  because that leg has never reached ctest. 110 is 🧠**INFERRED** from the operator's workstation figure
  (✔501.82s at 32 logical CPUs, P25) and said so rather than presented as measured. It closes by reading the
  budget-usage line off the first completed Windows run.
* `D-CI-PR-PIPELINE-IS-VENDORED-AND-ITS-SYNC-IS-UNCHECKED` — `pipeline-pr.yml` is a hand-copied fork of
  DSS.DevOps's `cpp-app-pr.yml@v2` (a PUBLIC repo cannot call a reusable in a private one), its sync obligation
  is stated in a comment and enforced by nothing, and this cycle made the divergence concrete. **An operator
  decision: that repository is outside this tree.**

⚠ **THE 🔵 CLASSIFICATION IS A JUDGEMENT AND IT IS FLAGGED FOR VETO.** Both rows describe conditions that
pre-date this cycle — the Windows leg had never reached ctest, and nothing has ever checked the vendored copy
against its upstream — so `DISCLOSED, not created` is the honest mark under the 2026-08-14 ruling. If the
operator reads either as created debt, they become P27's first work.

---

## 0.0000000000000000000000000000 ★★★ THE speedtest1 BENCHMARK — DSS BESIDE gcc AND MSVC, ON ONE MACHINE, AND WE ARE BEHIND

**Operator request 2026-08-21:** *"can you run a benchmark against gcc and msvc over test/speedtest1.c …
we put the comparison in our readme"*, then *"full source please"*. Delivered: `real-examples/c/sqlite/benchmark-speedtest1.{sh,ps1}`
+ the shared measurement core `speedtest1_bench.py`, and the README section they feed.

### The subject, and why it had to be derived

`test/speedtest1.c` **is** SQLite's own performance program. ⚠ **Upstream ships NO full-source recipe for
it** — `main.mk`'s `speedtest1` target and `Makefile.msc`'s `speedtest1.exe` BOTH link the amalgamation
(`sqlite3.c` / `$(SQLITE3C)`). ✔MEASURED by reading both files. So the TU list is derived from the
full-source CLI target `sqlite3d` (`shell.c` + `$(LIBOBJS0)`) through `base-harness.sh`'s
`dss_bh_emit_recipe` — the derivation this repository already proves daily — and then has its ONE
artifact TU substituted: `shell.c` out, `speedtest1.c` in. **103 TUs.** The substitution is asserted,
never assumed (`--selftest`, and R3 in the core refuses a surviving `shell.c`, two `speedtest1.c`, or a
`sqlite3.c` anywhere in the set).

### The numbers — native Windows 11, 32 logical CPUs, one machine, one source tree

Cold builds (fresh object dir per repeat), median of 3; runs median of 5 after an uncounted warm-up;
`time.monotonic()` only. Upstream SQLite `6f1110c`.

| compiler | optimization | build `-j1` | build `-j4` | `speedtest1 --size 25` |
| --- | --- | --- | --- | --- |
| DSS Code Prime | `--config=release` | 66.56 s | **34.81 s** | 3.473 s |
| gcc 13.2.0 MinGW-W64 | `-O2` | 26.41 s | **7.12 s** | 2.473 s |
| MSVC `cl.exe` | `/O2` | 13.26 s | **4.31 s** | 2.976 s |

At `-j4` DSS is **4.9× gcc** and **8.1× MSVC** to compile, and its output runs **1.40× slower than
gcc's**, **1.17× slower than MSVC's**. ★ That is published as-is. A compiler that reports only the axes
it wins is not measuring.

### ★★ THE FINDING THE TABLE ALONE DOES NOT CARRY

1 → 4 workers: DSS **1.91×**, gcc **3.71×**, MSVC **3.08×**. Inverting Amdahl on 1.91× at 4 gives a
parallel fraction of **~0.64** ⇒ **~36% of a full-source release build is on a SERIAL path**. That is a
place to look rather than an adjective, and it is why this is a registry row:
`D-PERF-CU-POOL-SCALES-HALF-AS-WELL-AS-SEPARATE-PROCESSES` (🔵 DISCLOSED — the defect pre-dates the
commit; nothing in it touches `src/`). ⚠ **Do not let it absorb the throughput gap** — "why the pool
stops scaling" and "why one CU is slow" are two questions and one row each.

### ✅ AND IT IS A CORRECTNESS CHECK, USING UPSTREAM'S OWN INSTRUMENT

The run passes `--verify`, which upstream's own comment describes as the *"Hash algorithm used to verify
that compilation is not miscompiled"*. **All three binaries produced the same hash.** Arms whose
normalized output disagrees are REFUSED outright (R6) rather than tabulated — three programs computing
different things do not have comparable times.

### ⚠ SIX DEFECTS THE PROVING RUN FOUND, EACH FIXED, TWO WORTH CARRYING

* **`libsqlite3.a` built without `USE_AMALGAMATION=0` holds ONE member, `sqlite3.o`** — the amalgamation,
  under exactly the right filename. The TU floor caught it only because 1 ≪ 100. ★ A count can never say
  WHICH thing is in the archive, so the driver now asserts the amalgamation object's ABSENCE **by name**.
  This is the one that would have silently inverted the whole subject.
* **`wslpath -w` is NOT idempotent and fails SILENTLY.** Applied to an already-Windows path it returns
  `CSourcesqlitex.c` — every separator gone, exit 0. Caught only because R1 checks the TUs exist.
* Also: the include list needed the generated-header dir (`-I.` in the recipe is relative to `$BLD` and is
  correctly dropped); `--output D` places the artifact at `D/<object-format>/name`, so the path is now READ
  from the build's own `dss-code-prime: artifact <spec> <path>` line instead of guessed; `cl.exe` must be
  resolved to an ABSOLUTE path out of the harvested vcvars PATH, because `CreateProcess` resolves the
  executable against the PARENT's PATH and the symptom reads as "MSVC is not installed"; and an arm that
  RAISES must be a named arm failure, not a dead run — one unresolvable compiler took down a run whose
  other two arms had already succeeded.

### Twin parity

The `.ps1` does not re-derive anything: it calls `benchmark-speedtest1.sh --derive-only --path-style
windows` through WSL (SQLite's build is autosetup + make + tclsh, so derivation has always been POSIX)
and then measures natively. One derivation, one measurement core, two callers. The MSVC arm is the only
asymmetry and even it is resolved by the SHARED core, so the `.sh` picks it up when run on Windows.

## 0.000000000000000000000000000 ★★★ CYCLE P25 — THE ARG CURSOR IS ONE OBJECT INSTEAD OF SIX, AND THE POOL TABLE ALONE FIXED NOTHING

**Priority:** `D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR` — the branch's only
🔴 HIGH, and a **LIVE SILENT MISCOMPILE** rather than a latent defect. ✅ CLOSED.

### The defect, and the measurement that says it was live

Argument placement selected its register pool with a two-way rule — `(cls == FPR) ? argFprs :
argGprs` — over a register-class vocabulary with **three** members, so a `VR`-class argument took the
`else` branch into the **integer** pool.

✔MEASURED at the disassembly, arm64 `--config=release`, `aarch64-linux-gnu-objdump -d`, on
`s3(a, b, y)` where `a`/`b` are `double` parameters and `y` is a `"w"` (VR-class) inline-asm output:

| arm | third argument | verdict |
| --- | --- | --- |
| mutant M1 (the two-way rule rebuilt) | `ldur q0, [sp,#24]` — **clobbers argument 0** | **rc=0, no diagnostic** |
| fixed tree | `ldur q2, [sp,#24]` — AAPCS64 NSRN 2 | rc=0 |
| `aarch64-linux-gnu-gcc 13.3.0 -O2` | the third `double` in `d2` | — |

Five shapes checked, every one correct after the fix: `s1(y)`→q0, `s2(a,y)`→q1, `s2(y,a)`→q0,
`s2(y,z)`→q0/q1, `s3(a,b,y)`→q2.

### ★★★ THE FINDING WORTH CARRYING: THE ROW TABLE ALONE WAS MEASURED INSUFFICIENT

With `argPassingRegister` already converted to a published row table, **the reproduction recompiled
byte-identical.** The register an outgoing argument lands in is decided by a **cursor walk**, and that
walk existed in **six** hand-kept copies — none of them the lookup:

`lir_callconv::computeMaxOutgoingStackArgs` · `lir_callconv::materializeOneFunc` (twice: the incoming
bound and the call placement) · `lir_rewrite::classifyCallRegArgs` · `lir_wide_call_args::lowerOneFunc`
· `lir_pass_util::incomingArgRegister` · `mir_to_lir`'s F128 marshal cursor.

Two of them carried comments promising to *"advance the shared cursors exactly as callconv does"* — a
promise kept by hand, which is the shape that lets six passes disagree. **One object (`ArgCursors`)
now owns the walk and the copies call it**, so the promise is structural.

### ★★ THE COUNTER IDENTITY IS DERIVED FROM THE TARGET, AND FROM THE ONE FIELD THAT DISCRIMINATES

AAPCS64 §6.4.2 stage C.1 gives the d-views and the v-views **ONE** NSRN. That is not a new schema key:
`dwarfNumber` is DWARF's identifier for a *physical* register, so two register rows carrying the same
number **are** one register wearing two widths.

⚠⚠ **AND THE FIELD THAT LOOKS RIGHT DOES NOT DISCRIMINATE AT ALL.** ✔MEASURED over both shipped
targets: `hwEncoding` is a per-file register NUMBER — arm64 `gpr × vr` share **all 32** values
(x0/w0/v0 all encode 0) and x86_64 `fpr × gpr` share **16** — so an hwEncoding-based derivation would
relate the integer and vector files on *both* targets, which is the same wrong answer the two-way rule
gave, reached from the other direction. `dwarfNumber`: arm64 `fpr × vr` = **32 shared**, `fpr × gpr`
= **0**, `gpr × vr` = **0**; x86_64 every pair **0**.

⚠ `subOf` is not the alternative either: `validate()` REFUSES a cc naming a register with a non-empty
`subOf`, so expressing d/v aliasing that way would make the arg pools unloadable.

### ★★★ TWO CLAIMS I WROTE WERE REFUTED BY READING THE THING THEY WERE ABOUT

Both are recorded here rather than quietly dropped, because both were about to become edits.

1. **The `"w"` view-selection premise was wrong, and the config had already measured it right.**
   I reported to the operator that arm64 `"w"` should name a register FILE and let the operand's
   width pick the VIEW (`fpr` for 8 bytes, `vr` for 16) — and began a `registerClass` →
   `registerClasses` schema migration on that basis. `arm64.target.json`'s own
   `$asmConstraintsComment` said otherwise, in a cell that begins *"AND THE MEASUREMENT SETTLES IT
   RATHER THAN INTUITION"*. ✔RE-MEASURED against the reference rather than against my notes:
   `aarch64-linux-gnu-gcc 13.3.0 -O2` prints **`v0` for every width** — `float`, `double`, and a
   16-byte vector all give `# W v0 v0`. The `d0`/`s0` spellings come from the `%d0`/`%s0` operand
   MODIFIERS, exactly as that comment says. **`registerClass: "vr"` is correct; the migration was
   reverted in full** (schema, loader, and both target files are byte-identical to `c6ef80e4`).
   ★ The rule this earns: *when the config carries a measurement and my notes carry a conclusion, the
   measurement wins* — and the cheapest way to find out is to re-run it, which cost one probe.
2. **`D-LIR-F128-ARG-NSRN-CURSOR-COUNTS-ONLY-F128-ARGS` was wrong when I wrote it, earlier in this
   same cycle.** It asserted the F128 marshal cursor "is advanced only when the argument's `TypeKind`
   is F128". ✔The loop already carried `else if (ak == F32 || ak == F64) { ++nsrn; }` with the comment
   *"a non-F128 FPR arg consumes an NSRN slot too"*. Closed on that measurement — and closed
   STRUCTURALLY, by converting that site to `ArgCursors` so the sharing is derived there too.

### The red-on-disable proof

`lir/test_lir_arg_cursor_projection`, 8 arms. Four mutants, each applied to the real tree, rebuilt,
and restored from a saved copy whose sha256 was re-verified in a `finally`:

| mutant | arms red |
| --- | --- |
| **M1** VR draws from the integer pool (the original else-branch) | 4, incl. the ABI corpus arm |
| **M2** no two pools ever share a cursor | 3 |
| **M3** an undeclared pool falls through to the exhaustion refusal | 1 |
| **M4** the by-value-aggregate exhaust clamp does nothing | 1 |

★ **The ABI corpus arm exists because of the P23 lesson on the return side:** a pin whose expectation
comes off the same table as the code moves BOTH HALVES OF THE COMPARISON TOGETHER, so deleting the VR
row reddened nothing. That arm states the ABI fact directly (`*vrPool == cc.argVrs`,
`!= cc.argGprs`) and refuses to pass vacuously if no shipped target populates a vector pool.

### ⛔ WHAT DID NOT LAND, AND IS NOT A FOLLOW-UP

`D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS` — **TRIGGER-GATED, MUST-NOT-BUILD,
MUST-NOT-CLOSE**, by operator ruling 2026-08-21. arm64 declares no vector registers in
`callerSaved`/`calleeSaved`, so the VR free list is empty and every `"w"` operand spills; at debug the
shape refuses loud with `rewriteOneFunc: … exhausted the per-class scratch pool`.

* **TRIGGER, as a measurable predicate:** *a source construct can form a 128-bit operand that reaches
  the allocator as a VR-class value.*
* **INSTRUMENT:** compile a corpus example declaring such an operand and assert the build does NOT
  emit `R_SpilledDueToPressure` for it. Today it always does.
* **A loud refusal is not a miscompile.** The miscompile is what P25 closed; the capability gap is
  what this row holds.
* ★ Operator ruling, verbatim: *"Do not let one absorb the other — that is how a gate disappears."*
  Closing the arg-pool row did **not** close this, and this row is **not** a follow-up of it.

### Found in passing, handled

* `D-TARGET-ARG-POOLS-WITHOUT-DWARF-NUMBERS-CANNOT-BE-RELATED` — **BORN CLOSED.** `validate()` now
  refuses a cc declaring two or more non-empty arg pools when any pool's slot-0 register carries no
  `dwarfNumber`: a fail-closed default is still a default, and a default is not a declaration.
* A `cc == nullptr` check that sat **inside** the per-argument loop in `mir_to_lir`'s call lowering,
  so a target with no calling convention was refused only if a 128-bit float happened to be in the
  signature. Asked once now, before the walk.
* **Three ways the arm64 dialect cannot use a `"w"` operand**, folded into
  `D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME` rather than opened as a new row:
  no FP/SIMD mnemonics, no SIMD **arrangement** suffix (`%0.16b` → `expected 'LineEnd' — got '.'`),
  and no width-**view** modifier (`%d0` → `S0067`). ✔The gcc build of the arrangement form RUNS
  correctly under qemu (prints `304.0`), so by §A.3b all three are required.
* **17 wrapped anchor ids** across the files this cycle touched — an id split over two lines is
  invisible to every grep, which is the one failure mode a fail-loud project cannot detect by
  watching for a failure. Unwrapped in `lir_callconv.{hpp,cpp}` and `parse_diagnostic.hpp`.

### Gate

| leg | result |
| --- | --- |
| Windows `build/dbg` ctest `-j 8` (`run-gate.ps1`, witness `100% tests passed`) | ✅ **1538/1538, 0 failed, 501.82 s** |
| WSL x86_64 ctest (`wsl-leg.sh`, clean configure + build) | ✅ **1538/1538, 0 failed, 344.65 s** |
| qemu arm64, `DSS_STRICT_ARM_VERDICTS=ON`, scoped `-R 'examples/.*\|lir/.*\|core/test_target.*'` | ✅ **636/636, 0 failed, 94.89 s** — arm-ledger `4 verified (4 ran)`, structural skips named, so the leg is NOT vacuous |
| **arm64 VPS, NATIVE aarch64** (`remote-leg.sh --carriage arm64-vps`, `ctest -j 4`) | ✅ **1538/1538, 0 failed, 1044.99 s**, clean build |
| **macOS arm64, REAL Apple Silicon** (`remote-leg.sh --carriage macos`, `ctest -j 10`) | ✅ **1538/1538, 0 failed, 3703.68 s**, clean build |

**Balance: 1033 → 1033, net ±0** — closed `D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR`,
opened `D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS`, one row born closed, one row opened and
closed inside the cycle.

---

## 0.00000000000000000000000000 ★★★ CYCLE P24 — `integrated_tests` IS 616 ENTRIES INSTEAD OF ONE, AND SEPARATING TWO FLOORS BY SCOPE WAS THE WHOLE PROBLEM

**Operator instruction, verbatim:** *"please create an anchor to paralelize integrated_tests + report
each integrated test item as pass or fail as a sub item of integrated tests unit (our runner to do
that)"*, then *"can you please address D-TEST-INTEGRATED-RUNNER-WALKS-EVERY-EXAMPLE-IN-ONE-THREAD now?
using a /dss-cycle"*.

**Anchors: OPEN 1034 → 1033, closed 1, opened 0, net −1** — plus **four rows BORN CLOSED**. The
balance gate PASSES; this is the burn-down cycle after P23's operator-authorised +14.

### ★★★ THE RULING THAT DECIDED IT — AND IT REJECTED ALL THREE OPTIONS AS OFFERED

The cycle paused on a §B: the optimized-arm instrument's floors are corpus-aggregate over EXECUTION,
and a per-example entry sees only its own example. Three options went up (a manifest key, a hand-listed
witness subset, a whole-corpus execution entry). **Operator ruling 2026-08-21, verbatim:** *"NONE OF THE
THREE AS OFFERED. The fork is really TWO floors with DIFFERENT SCOPES, and once they are separated only
ONE is homeless — and its home is not a manifest key. No new schema key. Nothing hand-listed. Nothing
deferred."*

★★★ **THE RULE, AND IT IS THE MOST PORTABLE THING THIS CYCLE PRODUCED: A UNIVERSAL CLAIM IS
PER-EXAMPLE; AN EXISTENCE CLAIM IS ABOUT THE CORPUS AND STAYS ONE.**
- **Floor A** — *"every declared optimized arm was BUILT through `--config`, or carries a classified
  not-built token"* — is universal, so it lives per-example as an ACCOUNTING IDENTITY rather than a
  `> 0` floor: `built + notExpressibleOnCli >= declared`, and any other shortfall reds. The token was
  not invented for this: `optimizedArmsNotExpressibleOnCli` already existed, already printed a
  classified `[SKIP]`, and is already witnessed by the in-process sibling. ✔The identity holds
  corpus-wide at **329 + 248 = 577 = declared** — ZERO unclassified not-built arms over 613 examples.
  ★ The house rule verbatim: **a not-done outcome carries a classified token from a closed vocabulary,
  and an unclassified not-done is the failure.** A third legitimate reason gets a TOKEN, never a waiver.
- **Floor B** — *"at least one artifact DIFFERS byte-wise"* — and the stdout-capture floor are
  existence claims and keep their scope. ✔**9 of 24 sampled** optimized-arm examples emit a
  byte-identical image, which is the CORRECT result where the optimizer has nothing to do; one such
  example refutes a universal must-differ floor and nine were found in the first sample.

⚠ **WHY THE MANIFEST KEY WAS REFUSED, recorded so it is not re-proposed:** `optimizerMustChangeImage`
would be set from **what the compiler emits today** — a guard configured from current behaviour asserts
nothing about correct behaviour — and it INVERTS THE BURDEN: mark the handful that differ and ~480
optimized-arm examples assert nothing about the optimizer ever again.

### ★★★ THE ANSWER: THE 613 ENTRIES HAD ALREADY ANSWERED THE QUESTION

Each per-example entry emits a **CELL** — declared / built / differed / notExpressible / stdout pins —
and ONE cheap adjudicator entry reads them and asserts both corpus-wide floors over exactly the same
population. No rebuild, no hand-list, and the subject is derived from the manifests so it cannot rot
when an example is renamed.
- ★★ **ONE FILE PER ENTRY, NEVER A SHARED APPEND.** 613 writers under `-j 8` appending to one file is a
  data race whose torn-line failure is intermittent and would be blamed on codegen.
- ★ **The cell is written even when the entry FAILS**, so a red example still contributes its
  observations and the adjudicator does not red for a second, unrelated reason.
- ★ A `FIXTURES_SETUP` entry clears the directory first. ✔Proven by planting a stale cell and watching
  it not survive — a cell from a previous run would otherwise satisfy a floor nobody measured today.

### ⚠ FIVE DEFECTS, ALL FOUND BY EXERCISING RATHER THAN READING — AND EVERY ONE WAS IN THE THING MEANT TO CATCH DEFECTS

1. **A floor that could not pass.** The first draft wired the corpus-wide DECLARATION floor into the
   parse-only `--only=corpus-lints` entry; ✔that counter is incremented during EXECUTION, and the entry
   binds no target and runs in 0.1 s, so the floor read `0 declared` and was **ALWAYS RED**. ★ The
   premise was named correctly and the CONSEQUENCE was not drawn — naming a premise is not the same as
   following it to what it breaks. Caught by the operator reading the diff.
2. **The adjudicator was scheduled at 611 of 617 and SKIPPED on a full run**, so the unit reported
   `100% tests passed` with the corpus-wide floors asserting nothing. `FIXTURES_REQUIRED` orders a test
   after the SETUP, not after the tests that share the fixture. ★★★ **A skip that always fires is
   exactly as vacuous as a pass that always fires, and it is harder to notice because it looks
   deliberate.** Fixed with `DEPENDS` on all 613 — and `DEPENDS` rather than a fixture *because* ctest
   skips a test whose fixture failed, which would hide the adjudicator the moment one example reddened.
3. **A torn cell returned 77** — the classified SKIP — because the short-population branch was reached
   before the parse failures were consulted. ★★ **A corrupt observation is not a smaller population**;
   filing it as "subset" puts it under the one verdict nobody investigates.
4. **The token-witness check could not fail.** `body.find("pipelineOverride")` is a substring search, so
   renaming the member to `pipelineOverrideXX` LEFT THE PIN GREEN — satisfied by the very rename it
   exists to detect. ★ **A witness that survives the disappearance of what it witnesses is not a
   witness.** Replaced with an identifier-boundary match.
5. **A fail-loud message that named the wrong subsystem.** Folding `corpus-lints` into `adjudicate`
   on the operator's instruction put the parse-fed lints in the SAME entry, so the global `failures`
   counter is no longer zero when the adjudicator starts — and it was branching on exactly that to
   decide whether cells were unreadable. ✔A failing LINT would have printed *"1 cell(s) could not be
   read. Refusing to adjudicate"*. ★★ **A fail-loud message that names the wrong subsystem is not
   fail-loud: it sends the reader to the one place the defect is not** — and this one was BORN of the
   fold, which is the argument for re-exercising a harness after a simplification rather than
   re-reading it. Counted locally now, and proven by three arms over the real 613-cell population:
   ✔control rc=0 with the identity `329 + 248 = 577`; ✔a torn cell ⇒ rc=1 naming **1** cell (not the
   77 skip); ✔a deliberately broken manifest ⇒ red on the LINT, `cells=613 population=613`, and no
   mention of unreadable cells at all.

### ★★ THE VOCABULARY GAP THE OPERATOR ASKED FOR BY NAME — AND IT WAS REAL

`runRunnerVocabularyPin` compares **manifest key literals** between the two runners and never covered
the classified-token set. It matters now because a per-example entry EXCUSES a declared-but-unbuilt arm
on the strength of `notExpressibleOnCli`, whose whole claim is that the in-process sibling drives
`CompileOptions::pipelineOverride` and IS the witness. Remove the witness and this runner keeps
excusing those arms while BOTH harnesses stay green — `D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE`
arriving through a CLASSIFICATION rather than a capability. The pin now asserts it.

### ⚠ THE SPLIT QUIETLY COST THE UN-SPLIT PATH A FLOOR, AND THE ASSERTION COUNT IS WHAT SAID SO

Relocating the three corpus-wide floors to the adjudicator dropped the whole-corpus default invocation
from **6659 to 6658** assertions. That path executes every example in ONE process and is perfectly able
to judge those floors. `corpusWideFloors` now has ONE definition and TWO callers — in-process totals for
the default run, cell-reconstructed totals for the adjudicator — rather than two copies that would drift.
✔Re-measured at this tree: **rc=0, 6661 passed, 0 failed**, arm ledger unchanged at `3675 of 4509`. The
+2 over baseline are both required by the ruling: assert the classification against the ledger, and
close the token-witness gap.

### ✔ MEASURED

| | before | after |
|---|---|---|
| `integrated_tests` wall clock | **677.97 s**, one ctest entry | **83.81 s**, 616 entries at `-j 8` — **8.1×** |
| ctest entries, whole suite | 922 | **1537** |
| per-process overhead | — | ✔**0.10 s** fixed, 1.15 s marginal (N=1/2/4/8 curve) |
| corpus accounting identity | not asserted | ✔**329 built + 248 classified = 577 declared** |
| byte-identical optimized example | reds under a per-example must-differ floor | **passes, and contributes its cell** |

★ One glob now feeds BOTH corpus harnesses (`DSS_EXAMPLE_MANIFESTS`, hoisted to the root
`CMakeLists.txt`, with a configure-time REFUSAL if it collapses below 100). Two independent globs would
let the two runners enumerate different corpora while both stayed green.

### ✔ THE FOUR-LEG GATE — ALL FOUR GREEN AT 1537/1537, AND THE SPLIT MADE EVERY LEG FASTER

Same carriage as P23: `scripts/remote-leg/remote-leg.sh` pushes the WORKING TREE to both remote
checkouts, because this cycle's work is uncommitted until the commit that carries this file. ⚠ Read
`.dss-leg-stamp` at the remote root, never the remote `git log`.

| leg | result |
| --- | --- |
| Windows `build/dbg` ctest (`run-gate.ps1`, witness `100% tests passed`) | ✅ **1537/1537, 0 failed, 516.88 s** — **67% more entries** than P23's 922 and **24% faster** than P23's 678.01 s |
| WSL x86_64 clean configure+build+ctest (`scripts/wsl-leg/wsl-leg.sh`) | ✅ **1537/1537, 0 failed, 350.84 s**, clean 766-target build |
| qemu arm64 (`QEMU_LD_PREFIX=/usr/aarch64-linux-gnu`, folded into the WSL leg) | ✅ same run |
| **arm64 VPS, NATIVE aarch64** (`remote-leg.sh --carriage arm64-vps`, probed `ctest -j 4`) | ✅ **1537/1537, 1031.98 s**, clean build |
| **macOS arm64, REAL Apple Silicon** (`remote-leg.sh --carriage macos`, probed `ctest -j 10`) | ✅ **1537/1537, 0 failed, 1651.95 s** — P23 spent **5030.41 s** on 922 entries here, so the split is **3.0× on the slowest leg** |
| `check-anchor-balance` | ✅ **1034 → 1033, closed 1, opened 0, net −1 — PASSES**, the burn-down after P23's operator-authorised +15 |
| `check-anchor-registry` | ✅ 0 cell-width violations across 300 tables / 4,178 rows in 41 files; every `src/` anchor resolves — ⚠ 300/4,178 and not the 299/4,168 measured minutes earlier, because THIS TABLE is itself a governed table in a governed document |
| `check-plan-citations` | ✅ 3,127 positional citations across 283 documents, ratchet unbroken |
| `check-scripts-index` · `check-line-endings` · `check-diagnostic-codes` · `check-enum-name-table-guards` | ✅ 25 scripts · no CR in 2,579 tracked paths · 386 codes, 0 collisions · 66 vocabularies, all guarded |

⚠ **THE VPS's ctest PRINTS `100% tests passed out of 1537` WITH NO `, 0 tests failed` CLAUSE** — an
older CMake on that host, and `run-gate.sh`'s witness regex matches either spelling. ★ So the green was
not taken from the summary line: it was CONFIRMED BY COUNTING the log — **1537 verdict lines, 1537
`Passed`, zero `***`, zero `Skipped`**. A summary line whose shape varies per host is exactly the kind
of witness this project has been burned by before; counting the verdicts is host-independent.

⚠ **THE REMOTE `-j` IS PROBED, NOT ASSUMED — AND WITHOUT THAT, TWO OF THESE FOUR LEGS WOULD STILL BE
SERIAL.** P23's remote legs both ran one test at a time and **neither said so**: `ssh` forwards no
environment without `SendEnv`/`AcceptEnv`, so `CTEST_PARALLEL_LEVEL` never crossed the carriage.
`remote-leg.sh` now reads the remote's own `getconf _NPROCESSORS_ONLN` — the portable probe, since
`nproc` is GNU-only and `sysctl -n hw.ncpu` is BSD-only — and passes `-j` explicitly, so the value is
visible in the recorded command line rather than inferred. ★ The confirming check is worth reusing:
**a ctest log states its own concurrency**, as started-minus-completed at each verdict line, with no
access to the host at all.

⚠ **THREE EDITS LANDED AFTER THE GATE AND ARE NAMED HERE RATHER THAN LEFT FOR A READER TO NOTICE.**
Re-measuring the headline figures at the frozen tree found them stale by exactly the one entry the
`corpus-lints` fold removed: the unit is **616** entries and the suite **1537**, not 617/1538, and the
wall clock re-measured **83.81 s** (from **677.97 s**, the figure in `build/ctest-p23-final.log`, not
the 677.95 s that had been re-quoted) — **8.1×**, not 7.6×. The corrections touch this file, the
registry row, and one COMMENT in `integrated_tests/CMakeLists.txt`. ✔The CMake edit was re-configured
and the entry count re-counted at **1537**, unchanged; all seven guards were re-run green afterwards.
⚠ **AND THEN A FIFTH DEFECT WAS FOUND AND FIXED AFTER THE LEGS RAN** (the misattributed
unreadable-cell count, above), so the Windows gate was RE-RUN on the committed tree: ✅**1537/1537, 0
failed, 404.35 s**. The three remote legs ran the pre-fix tree. That is said plainly rather than
implied away — and the reason it does not invalidate them is specific, not general: the change is
confined to the adjudicator's ERROR-REPORTING path, which no green run reaches.
★ This is the standing rule catching its own author: **never re-quote a gate figure from a previous
message — re-measure at the commit that carries it.** Every one of those four numbers had been carried
forward from before the fold that invalidated them.

---

## 0.0000000000000000000000000 ★★★ CYCLE P23 — WEAK DEFINITIONS *AND* WEAK ALIASES SHIP ON PE/COFF AND MACH-O, THE RETYPED-CLOSED-SET CLASS IS FINISHED, AND THE pe64 ACQUISITION ROW CLOSED ON A MEASUREMENT NOBODY HAD EVER TAKEN

**Operator argument for this cycle, verbatim:** *"keep going. address all three rows +
D-HARNESS-PE64-LIB-ACQUISITION-IS-HOST-DEPENDENT in this cycle"* — the three rows P22 opened plus the
only HIGH in the harness family. ✅ **All four are CLOSED**, and so are three more that fell out on the
way. Two operator rulings were taken mid-cycle; both are recorded below and **must not be
re-litigated**.

**Anchors: OPEN 1018 → 1033, closed 7, opened 22 (21 CREATED + 1 disclosed pre-existing), net +14
created-over-closed.** ⚠ **THE BALANCE GATE REFUSES THIS, AND IT SHIPPED ANYWAY ON AN OPERATOR RULING** —
see the audit-fold section below for the ruling verbatim and for what the +13 is made of. Every one of the
21 created is a defect this cycle FOUND rather than inherited, each was sized before being written down,
and all of them are the NEXT cycle's first work (§0.00000000000000000 P25). ★ The mid-cycle claim of
"net −1" was true of the tree at the time and is superseded here rather than deleted, because the reason
it moved — an independent audit refuting one of this cycle's own closure claims — is the finding.

### ★★★ RULING 1 — BUILD THE WEAK-DEFINITION MACHINERY, AND THE OPTION AS WRITTEN NAMED THE WRONG COFF MECHANISM

Closing `D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB` means emitting each alias name with ITS
OWN binding — decoupling name from binding is what caused
`D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION` in the first place. But gcc's alias shape
is WEAK-against-STRONG, and a weak defined symbol hit an existing loud refusal in the PE and Mach-O
relocatable writers (`D-LK-OBJECT-WEAK-DEF-RELOCATABLE`, open since 2026-07-23). Three options went
to the operator: declare a FORMAT INCAPABILITY and warn; let the refusal fire; or build the machinery.

**The ruling, verbatim:** *"OPTION 3 — build the weak-definition machinery — but its COFF half as
WRITTEN IN THE FORK IS THE WRONG MECHANISM and would break DSS's own round-trip."* The fork said
`IMAGE_SYM_CLASS_WEAK_EXTERNAL`; that is a weak *REFERENCE* with a fallback alias (the
`/ALTERNATENAME` shape), not a weak *DEFINITION*. **A weak definition in COFF is COMDAT select-any.**
Building WEAK_EXTERNAL here would have emitted objects DSS's OWN READER reads back with the wrong
semantics — a round-trip break in a project that keeps a round-trip encoding oracle.

★★★ **THE REUSABLE RULE, AND IT IS THE MOST PORTABLE THING THIS CYCLE PRODUCED** — operator's words:
*"An implementation gap and a format incapability are different facts. Never let the first be recorded
as the second."* A `.format.json` row saying *"this format cannot express a weak definition"* would
have been FALSE — gcc and clang emit weak definitions on PE/COFF and on Mach-O — and it would have put
that falsehood in the place most likely to be trusted later, permanently under-capabilitying DSS
against the references on two of three formats. It is also the decision that does not reverse cleanly
once a config document asserts it.

📄 Spec-verified before implementing (PE/COFF §5.5.3, §5.5.6): a weak external is *"a symbol table
record with EXTERNAL storage class, UNDEF section number, and a value of zero"* — it defines nothing;
`IMAGE_COMDAT_SELECT_ANY(2)` is *"any section that defines the same COMDAT symbol can be linked; the
rest are removed"*, with the Selection byte at offset 14 of Auxiliary Format 5, matching the reader's
own `kAuxSectionDefSelectionOff`. ✔The row's *"INFERRED, WITH ZERO CODE EVIDENCE IN THIS TREE"* caveat
was discharged **from the tree itself**: `coff_object_reader.cpp`'s `GATE 3` already lifted select-any
to `SymbolBinding::Weak` while `grep -c -i comdat src/link/format/pe.cpp` returned **0**. Reader and
writer disagreed about what a COFF weak definition IS, and that asymmetry was the evidence.

### ★★ AND THE ROW WAS WRONG ABOUT MACH-O TOO, IN A WAY THAT WOULD HAVE PUT A FALSE STATEMENT ON THE WIRE

The row asked for `N_WEAK_DEF` **plus `MH_WEAK_DEFINES`**. ✔MEASURED on the operator's Mac: clang's
`.o` for `__attribute__((weak))` carries `n_desc = 0x0080` with header flags `0x00002000` —
`MH_SUBSECTIONS_VIA_SYMBOLS` ONLY. `MH_WEAK_DEFINES (0x8000)` appears only AFTER linking (`otool -h`:
`0x00218085` exec / `0x00118085` dylib against a weak-free control's `0x00200085`), and
`<mach-o/loader.h>` documents it as a property of *the final linked image*. Setting it on an
`MH_OBJECT` would assert a fact about a linked image the file is not, and no reference encoder does
it. **The shipped pin asserts `N_WEAK_DEF` IS set and `MH_WEAK_DEFINES` is NOT**, with the measurement
in the test comment so the next reader does not "fix" it back.

⚠ This is the SECOND cycle running in which a brief handed a lane a wrong Mach-O constant or flag from
the registry (P22: `N_ALT_ENTRY` given as 0x0020, which is `N_NO_DEAD_STRIP`). The SDK header was
re-read this time and all four values confirmed: `N_WEAK_DEF 0x0080`, `N_WEAK_REF 0x0040`,
`N_NO_DEAD_STRIP 0x0020`, `N_ALT_ENTRY 0x0200`.

### ★★★ RULING 2 — THE COFF WEAK-EXTERNAL PAIR, READER FIRST. AND THE BRIEF NAMED THE WRONG FIELD

COFF puts the coalescing policy on the SECTION, and an alias shares its canonical's section by
definition — so **a WEAK alias of a STRONG definition is unrepresentable in the COMDAT shape**. ★ Note
the inversion, because it is the interesting part: `IMAGE_SYM_CLASS_WEAK_EXTERNAL` is WRONG for a weak
DEFINITION (ruling 1) and **PRECISELY RIGHT for a weak ALIAS** — §5.5.3's own words are *"sym1 is an
alias for sym2"*, via `IMAGE_WEAK_EXTERN_SEARCH_ALIAS`. The operator ruled: *"BUILD IT — the
WEAK_EXTERNAL reader+writer pair, READER FIRST. This is NOT a re-litigation of the weak-DEFINITION
ruling; it is a DIFFERENT MECHANISM FOR A DIFFERENT FACT."*

The ruling also said to route on the auxiliary record's **role**, never on storage class as a proxy.
**The lane measured that routing instruction FALSE before implementing anything.** ✔MEASURED with a
raw 18-byte aux dump over mingw gcc 13.2.0: **gcc emits `Characteristics = 1` for all four weak
shapes** — weak function definition, weak data definition, `weak, alias(...)`, and weak undefined
reference. Routing on it would have classified every gcc weak DEFINITION as unresolvable, which is
precisely the defect the lane existed to fix. **The field that discriminates is the record's own
`TagIndex`: whether the default symbol it names is section-backed.**

★★ This is `D-LK-MACHO-ISDATA-NO-CALL-SIGNAL` in a new costume — there a relocation's *arithmetic*
stood in for its *role*; here a search *policy* stood in for a definition *state*. **The trap is not
any particular field. It is reaching for whichever field sits nearest the decision and assuming it
carries it.** Third consecutive cycle in which a brief handed a lane a wrong fact about an object
format, and the first in which the wrong fact was a MECHANISM rather than a constant — so the existing
*"a brief may state an INTERFACE only if its author has run it"* rule is now widened to MECHANISMS.

**A second brief claim, refuted with the right control.** The brief said the witness should be
`gcc main.c dss.o` producing a running binary. ✔MEASURED: GNU `ld`'s PE backend does not resolve a
weak external across objects **for gcc's own object either** — at every `Characteristics` value, both
link orders, and through an archive. Pinning that would have asserted a capability *no reference has*,
and the pin would have gone red forever with the blame pointed at DSS. MSVC `link.exe` does resolve it
— links, runs, returns 42 — so bar §A.3b makes it the working reference and `ld`'s refusal a confound,
recorded in-source beside the witness so nobody "repairs" it back.

**`IMAGE_WEAK_EXTERN_SEARCH_ALIAS` (3) is the only value that works.** The lane first chose 1 on a
semantic reading of §5.5.3's *"if sym1 is not present"* clause; `link.exe` refuses 1 and 2. ★ The
registry row had said ALIAS(3) all along — **the lane's reasoning contradicted the record, and the
record won.**

**What the reader half turned out to be — and it was not in the row.** DSS's COFF reader had no
`WEAK_EXTERNAL` path at all and its storage-class dispatch was two constants with an open-ended
fallback, so a mingw weak undefined reference made **DSS refuse a link that gcc and clang accept**.
That is a live capability gap against both references, not plumbing for a writer. The dispatch is now
TOTAL: every storage class resolves to a named role or fails loud — and making it total is what
surfaced `D-LK-COFF-READER-COMMON-SYMBOL-READ-AS-IMPORT` (§5.4.2: EXTERNAL + UNDEF section +
**non-zero Value** is a tentative definition whose Value is its SIZE, and DSS was reading it as an
import), invisible while the UNDEF arm was a single untotalled branch.

### ★★ A CLASS MADE UNREPRESENTABLE RATHER THAN GUARDED, AND THE ONE PIN THAT CAUGHT IT

The design audit found that PE and Mach-O minted symbol-table indices in a REGISTRATION pass separate
from the EMISSION pass, coupled **only by a comment**. An added record shifts every later relocation's
symbol index — a well-formed file, wrong program, no diagnostic. The PE object writer now has ONE
ordered entry list with a single slot-minting site, so the class is unrepresentable, and both writers
gained a `minted == emitted` tripwire.

★ The tripwire FIRED during mutant testing. And under a mutant that added a record without counting it
**and** disabled the tripwire, **exactly one pin survived to catch it** — the relocation-index
assertion the design audit demanded, because *"both names present, one address"* stays GREEN in that
state. That is finding A-3 reproduced live: the obvious pin passes both ways.

### ★★ THE MACH-O DEAD-STRIP HAZARD IS MEASURED SAFE, NOT ASSUMED SAFE

P22 shipped `MH_SUBSECTIONS_VIA_SYMBOLS` as a licence for ld64 to dead-strip at SYMBOL granularity, so
a second defined symbol at an atom's start could have minted a ZERO-LENGTH atom and let the body be
stripped — perfect bytes, wrong program. ✔MEASURED on real Apple Silicon via a new
`scripts/macho-alias-ld64-matrix`: **8 of 8 cells green** — a plain second `.globl` label, an
`.alt_entry`, and a clang `.globl`+`.set` CONTROL, each ± `-dead_strip`, each linked against a caller
referencing ONLY the alias, plus a canonical-only control. Every cell: both names present, same
address, `__text` = 0x28, and **the program RAN and returned 42** — which is the observable that
catches a stripped body behind a zero-length atom. `rc=0` is not.

### ★★★ THE IMAGE TIER OF THE ALIAS ROW — AND THE ARM THAT CARRIED IT HAD A SECOND, OLDER DEFECT IN THE SAME TABLE

Item (2) of the alias row — the FINAL IMAGE's symbol table — is closed on all four image arms: ELF
static `ET_EXEC`, ELF dynamic, Mach-O exec and Mach-O dylib, each on both ports. PE images were never
affected and this was verified before touching anything: the PE image header writes
`PointerToSymbolTable = 0` / `NumberOfSymbols = 0`, so a PE image has no COFF symbol table to lose a
name from.

**Witnessed by execution on real hardware, not by re-reading the writer.** A gcc-built `dlopen` probe
against a DSS-emitted `.so` resolved BOTH names to the same pointer and both calls returned 42 (probe
exit 42); an Apple-clang-built probe did the same against a DSS-emitted arm64 dylib on real Apple
Silicon, `codesign -dv` intact. The reference control was measured on the matching platform first —
`gcc -shared -fPIC` of `__attribute__((weak, alias("strong_fn")))` puts `weak_alias` FUNC **WEAK** and
`strong_fn` FUNC **GLOBAL** at one `st_value` with one `st_size`, and that is the shape the pins
assert.

**`D-LINK-ELF-STATIC-EXEC-SYMTAB-ST-VALUE-NOT-A-VA` — found on the way, born CLOSED.** The static
`ET_EXEC` arm wrote every `.symtab` `st_value` as an offset within `.text`; gABI 4.18 defines that
spelling for RELOCATABLE files only — in an image `st_value` is a virtual address. `nm`, `gdb` and any
profiler would have placed every function near address zero. ★ It is the ADDRESS column of exactly the
defect `D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS` fixed the NAME column of — same table,
same arm, one cycle apart, and **the first fix did not go looking for the second.** ⚠ It survived
because the arm is unreachable from every shipped format: every exec/PIE format spells `processExit`
as a by-name import, so every real executable takes the DYNAMIC arm, which never had the bug. Nothing
inside DSS reads an image's `.symtab`, so no internal consumer could witness it either. It took a lane
building an image and running `readelf` over it.

**★★ The red-on-disable that earned its place: M5.** Mach-O's `encodeExecDynamic` derived `nextdefsym`
/ `iundefsym` and every `indirectSyms` origin from `module.functions.size()` — a PREDICTION of how
many defined nlists the loop would emit. Adding aliases falsifies the prediction. With the alias band
emitted but the origin still predicted, **every "both names present, one address" assertion stayed
GREEN**; only the index half went red, reporting an indirect entry pointing INSIDE the defined band —
a stub or `__got` slot that would bind to a local definition instead of its import. **A pin without
the index half would have shipped a silently mis-bound import.** The fix is not a guard: `numDefs` is
now read off the emitted band, so the prediction cannot exist. This is the third writer in two cycles
to carry the registration-split shape.

**A design choice worth knowing about.** The image tier reuses `definedAliases` — the same accessor
the object tier uses, visibility gate included — rather than gaining an ungated `imageAliases`
companion. Measured reason: the object readers fold clang's `ltmp0`, a module-private section label
sitting at the exact `n_value` of the first function of every ordinary TU, in as a
non-externally-visible extra name. Admitting those into an image would add a row denoting an address
the real name already denotes, and `ld`/`ld64` drop exactly these when they build an image. So both
tiers want the same externally-visible set, for two different reasons — both now written on the
accessor.

### ★★★ THE ALIAS ARC WAS UNREACHABLE FROM THE SHIPPED PATH, AND FIXING THAT FOUND A SILENT MISCOMPILE

`mergeModules` dedups the combined `.symtab` on the merged symbol id alone, and an alias is by
construction a second row for that id — so every alias name was dropped the moment more than one
module was linked, which the shipped static-archive path always is. **A capability nothing can reach
is not a capability**, so the arc was not finished until this closed.

The row named one defect. There were **four**, and the dedup was the least serious:

- **The merged-id pre-assignment minted one id per NAME rather than per WINNING KEY**, leaving the
  name→id map pointing at ids no definition carries. ✔MEASURED as a **clean link** — `img.ok()`, zero
  error diagnostics — whose call through the alias branched to `.text` base **4198400**, the injected
  entry trampoline, instead of the winning body at **4198431**. A wrong program that links.
- **The shadow test decided whether an atom's BYTES survive by reading the CANONICAL row**, so an atom
  that lost its canonical while *winning* an alias was deleted outright, and one whose first row is a
  compiler-private label escaped folding entirely.
- **No refusal existed** for a definition whose several names resolve to different definitions.

★★ Two pieces of discipline worth copying. The lane established **what the old dedup protected**
before changing it — ✔exactly one non-alias shape, a byte-identical repeated row, still reachable for
Weak and Local — so the re-key to `(merged id, name)` keeps that collapse and drops only the alias
suppression. And a mutant **survived**: rather than declaring it unreachable, the lane found the shape
it really guards (an atom that won nothing whose FIRST row is a private label — precisely the order a
re-read ELF object presents, locals before globals) and added the missing pin.

A second defect fell out of a pin written for a different property: a byte-identical repeated GLOBAL
row was ranked against its own key and refused a legal object with *"multiple strong definitions
across CompilationUnits (**CU #1 and CU #1**)"*. ★ The refusal is bad; **the diagnostic is worse** —
it sends the reader hunting a second definition that does not exist, in a second CU that is the same
CU, and `(CU #1 and CU #1)` reads as a formatting slip rather than as the message telling you exactly
what happened. A fail-loud that misdescribes its own cause costs more than a silent one, because it is
trusted.

### ★★★ THE RETYPED-CLOSED-SET CLASS — AND THE INSTRUMENT THAT MEASURED ITS OWN BLIND SPOT AS COMPLETION

A **retyped closed set** is a diagnostic that renders the accepted spellings as a string literal while
acceptance is decided by a table. The two owners drift, and the drift is invisible: the message is
what a config author reads.

★★ **The instrument was the story.** The harvester that reported the class *"finished"* in one tier
was **single-quote-only** and required **≥2 quoted tokens**. ✔MEASURED: adding a double-quoted arm
took the tree-wide census **40 → 77**, and the ≥2 threshold had been hiding single-element closed sets
entirely — three real sites in one file alone. So the earlier report of *"`target_schema_json.cpp`
15 → 0"* was true **only of the instrument that produced it**; two sites remained. A census is a
measurement, and a measurement that cannot see half its subject is not a smaller measurement — it is a
different one wearing the same number.

**Four live drifts, each telling a config author by name that a spelling the loader accepts is not
allowed:**
- `/sections/{}/kind` advertised **11 of 16** rows — and ✔the shipped corpus DECLARES all five omitted
  spellings (`shstrtab` 10 rows, `relro` 10, `tdata` 5, `tbss` 4, `tvars` 2). A config author copying
  a shipped document would have been told their copy was invalid.
- ELF `osabi` accepted **7** spellings and advertised **6**: `linux`, the conventional alias of
  `ELFOSABI_GNU`, was accepted and denied at once. ★ An ALIAS is the shape this class hides best — a
  human writing a list writes the names they were thinking of.
- The FFI descriptor's four format refusals named **3 of 5**.
- The accepted `format.kind` set is the backend REGISTRY, with an enum-derived list beside it as a
  second owner and ✔nothing in `test_object_format_backend_registry.cpp` tying them together.

**A pin that fails at COMPILE time.** `lirRegClassFromName` was a hand-written if-chain over
`kTargetRegClassTable`'s spellings. The two enums already carried `static_assert`s tying their VALUES
in lockstep — which is exactly what made this invisible, because *the file looks pinned*. A
target-side rename would have split `.target.json` from `.dsslir` silently. Now delegated, with a
totality `static_assert`: ✔the mutant that splits the spellings **with the assert left in fails to
build**, and the compiler names the assert's own text.

**A vocabulary table can now assert its own well-formedness.** `EnumNameTable<E,N>` written with `N−1`
initializers is legal C++ — the missing row value-initializes to `{E(0), ""}`, so `fromName("")`
starts resolving, and for a vocabulary whose zero is a sentinel an empty string silently selects
*"this knob does nothing"*. A dropped row would not break the build; it would make `"lowering": ""`
load clean and disable the intrinsic. **137 vocabularies were exposed.** New
`isWellFormedEnumNameTable` + `DSS_CHECK_ENUM_NAME_TABLE`, exercised out of tree in both directions
before wiring (well-formed control rc=0; under-filled rc=1; duplicate-spelling rc=1, each naming the
assert's text), and it catches one case the hand-written assert in `semantic_config.hpp` could not:
two rows sharing an *enumerator*.

### ★★★ TWO SILENT DEFECTS IN THE `.dssir` READER, FOUND BY A MESSAGE-QUALITY SWEEP

The block-header parser read `[marker]` and had **no `else`** — an unrecognized marker left `Linear`
and the parse *succeeded*, so a `.dssir` with a typo'd or merely newer marker round-tripped to a
module whose block had lost its structural role. The format's entire contract is that it is lossless,
which is exactly why this was invisible: the round trip that would have caught it is the thing that
was broken.

The second is worse. `parseType`'s `cc <name>` arm did the same and left `CcSysV` — not a lost
annotation but **a different calling convention applied to a function signature**: different argument
registers, different stack discipline, no diagnostic. It was found only because the first one was
found and the lane went looking for siblings. ★ That is the transferable part — **a silent default is
a SHAPE, and a file containing one is worth searching rather than a file to fix one line of.**

⚠ The worst message drift in the whole class was also here: a sentence saying a referenced document
*"may declare only"* seven blocks, while the loop accepts fifteen — and `asm.lang.json` declares four
of the eight it called forbidden. A config author reading that would have concluded a shipped document
was illegal.

★ The lane declined 26 remaining hits **with a classification for each** rather than converting to
satisfy a count — 2 where projecting the accepted table would make a required-field sentence false, 12
mutual-exclusion prose, 8 naming a block by role — and stated that the `--min-tokens 1` census is
**277** and was not reviewed, so the 26 is not read as a clean bill.

### ★★★ THE ENUM-VOCABULARY ROW CLOSED — AND ITS OWN HEADLINE CLAIM WAS FALSE, FOR A REASON WORTH KEEPING

`D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET` said in bold *"NOT A LIVE DEFECT AND THE
ROW SAYS SO"*. It was right about the five sites it listed and wrong about the class it named: **three
messages had already drifted**, each telling a config author by name that a spelling the loader
accepts is not allowed — `operandKinds` named 3 of 7 table rows, `resultSlot`/`slotKind` named 8 of 32
grouped by an encoding shape no table owns, `terminatorKind` named 6 of 7.

★★ **The reason is the transferable part: the row had CHECKED the five sites.** An inventory taken by
re-reading the places you already suspect measures your suspicion, not the tree. The lane replaced it
with a **generic** harvester — harvest every `EnumNameTable` and every `…FromName` if-chain, then find
every string literal whose quoted tokens are one of those vocabularies — and the class came back at
**29** sites needing conversion in the two loaders, not five. Same shape as the survey that *"searched
for the names it expected to find"* and missed `ubuntu-ports-arm64`.

The row also mis-classified its own list. `signatureByDataModel` and the `synthesizedTypes` role map
name **no set at all** — under-informative, not drifted. And the `synthesizedTypes` retype is a
hand-listed *enumerator* array whose comment claimed listing them explicitly is what makes a new data
model fail loudly. That is **backwards**: a hand-written list is precisely the construct a new
enumerator never reaches.

**★★ The blocker the row sized was not the blocker.** It said converting `dataModelFromName` was risky
because it is widely used, and told the lane to measure call sites. Measured: 9, and 8 for
`dataModelName`, and **none had to change** — the signatures are identical. The real blocker was
documented in the tree and not in the row: `EnumNameTable::name()` **falls back to `rows[0]`**, while
`ObjectFormatData::validate()` detects an undeclared data model via `dataModelName(dataModel).empty()`
— so a naive conversion would have rendered a never-declared model as `"LP64"` and **`validate()`
would have accepted it**. The fix belonged in the core (`nameOrEmpty`, a strict projection), not at
the call sites.

Two stale premises corrected in place: `data_model.hpp` and `header_name_matching.hpp` each justified
being hand-rolled with *"`EnumNameTable` lives in `target_schema.hpp`, which this header CANNOT
include"* — it has lived in the dependency-free `enum_name_table.hpp` since that extraction, and that
header's own comment names `data_model.hpp` as one of its two motivating cases.

✔Dry run over the shipped corpus: **84 documents, 0 violations, all 12 projected or narrowed sets
exercised, `src/dss-config/**` needed ZERO edits** — including the one behaviour narrowing
(`registerClassOps[].class` now refuses the `none` sentinel it previously loaded while the sentence
beside it said otherwise).

**The red-on-disable that states the property exactly.** M1 deletes `LLP64` from `kDataModelTable`: 11
of 59 core tests red, and the diagnostic follows the table with no edit — *before this change,
deleting the name from the MESSAGE reddened nothing*, which is the whole defect in one sentence.

### ★★★ THE WEAK-DEFINITION DIALECT IS NOW A KEY THE WRITER ACTUALLY READS

The operator ordered one config row naming which weak-definition dialect a format's writer emits —
default COMDAT, and explicitly **not** a capability flag, because
`D-LK-PE-ALTERNATENAME-DECLARE-AND-REFUSE` is the record of why an implementation gap recorded as a
format incapability is a false fact in the place most likely to be trusted later.

★★ **The consultation is ordered so that ABSENCE COSTS NOTHING**: the walker scans for a weak
definition first and only *then* asks the schema, so a format that never emits one needs no
declaration — pinned as its own cell, because the obvious implementation makes the key effectively
required, which is the shape the ruling rejected. The vocabulary names MECHANISMS (`comdat`,
`symbol-binding`, `symbol-flag`), not formats, so it says what is emitted rather than who emits it.

✔**The red-on-disable that proves the key is READ**: deleting the declaration from the shipped pe
object document — with **no rebuild**, observed through `ctest` because that is what sets
`DSS_CONFIG_ROOT` — reds seven pre-existing COMDAT writer pins plus the three new config pins.

⚠ **It ships with ONE consulting writer, and that asymmetry is deliberate.** ✔MEASURED: `elf.cpp` maps
Weak → `STB_WEAK` unconditionally and `macho.cpp` sets `N_WEAK_DEF` unconditionally. Declaring the key
on their documents today would create exactly what the parent row exists to prevent — a key the writer
does not read, drifting silently while reading as authoritative — so the lane declared it nowhere
rather than everywhere, and **pinned the scope** (2 of 24) so it is asserted rather than remembered.
`D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS` carries the rest, including the
generalization to take *at that moment and not before*: a backend accessor mirroring
`stackReserveVehicles()` so a mis-declared dialect fails at LOAD rather than at emit. Not built now
because with one consulting writer it would have had exactly one row to check — a mechanism ahead of
its second consumer.

### THE OTHER THREE ROWS

**`D-HARNESS-PE64-LIB-ACQUISITION-IS-HOST-DEPENDENT` (HIGH) — CLOSED, and its prescribed work had
already landed without the row being marked.** All five legs have declared `pinned-archive` since
`3e86a187`; what was outstanding was the clause that DECIDES the row — the cross-host re-measurement.
✔MEASURED 2026-08-20 on the native aarch64 VPS (`ls /mnt` EMPTY, `mount | grep -ci drvfs` = 0): **5 of
5 legs resolve build inputs from a COLD cache**, 8 pinned archives, 11,621,552 bytes, every sha256
re-verified independently 8/8, plus an OFFLINE warm 5/5 and a NEGATIVE CONTROL that correctly returns
**0/5** — so the instrument can say no. This is the measurement P13 never took, and it also closed
`D-HARNESS-ELF-LEG-HOST-SYSTEM-PROVIDER-UNSATISFIABLE-OFF-LINUX` on the same evidence. ⚠ Clause (2) of
the closing work was deliberately INVERTED (`search-paths` removed rather than kept as a fast path)
and that divergence is recorded IN the row with the three measurements that justify it, rather than
quietly satisfied.

**`D-TEST-STATIC-LINK-UNIT-SUITE-CANNOT-WITNESS-A-DRIVER-THREADING-GAP` — CLOSED, including the merged
route.** The finding nobody had named: **the classification is per ROUTE, not per entry point** — the
driver reaches `linkAndWriteWithStaticArchives` by three routes and `optimizeModule` by three. The
headline defect the row never mentioned: three link-and-write exports took `ImageRequest` DEFAULTED to
`{}`, which the file's own docblock forbids on a sibling export. Fourteen driver-level pins now exist;
under every mutant planted AT THE DRIVER, the in-process unit suites for the same callee stay GREEN —
that contrast IS the class. ⚠ Two witnesses had to CHANGE rather than two pins weaken: a
static-archive probe was silently vacuous (that route short-circuits before the merge), and
`dataModel` cannot be witnessed by an LP64↔LLP64 mis-supply at all, because `pointerBytes` returns 8
for both.

**`D-CONFIG-UNKNOWN-KEY-CHECK-HAND-ROLLED-SITES-REMAIN` — CLOSED.** Every closed-key block in both
loaders routes through the shared check: 12 table-less chained conditions → 0, 26 hand-rolled loops →
1 documented carve-out, 58 `isDocumentationKey` sites → 20, and **77 lines FEWER while adding comment
blocks**. Tables derived from the PARSE ARM and dry-run against all **84** shipped documents BEFORE
any code — 0 violations, so `src/dss-config/**` needed zero edits. ⚠ The row's own list said ELEVEN
table-less sites and there are TWELVE: it omits `gatedMarkers` and mis-describes `synthesizedTypes`,
so a sweep sized on that list leaves one behind.

### ★ FOUR THINGS THIS CYCLE FOUND THAT NOBODY WAS LOOKING FOR

1. **A retired provider lived in both harness drivers for ten days.** `ubuntu-ports-arm64` was gone
   from `LIBRARY_PROVIDERS` — so `--lint` refused any leg declaring it — while `build-and-test.sh`
   kept a LIVE dispatch arm, a ~45-line acquisition function and an env override, and BOTH drivers
   advertised the name as KNOWN in the one message whose job is to say what is accepted. The existing
   guard could not see it: it iterates the DECLARED set, so a name nothing declares is outside its
   domain by construction. ⚠ The cycle's own survey missed it too — **the grep searched for the names
   it expected to find.**
2. **A table that disagreed with its own block, exposed BY the routing.** `kAssemblyKeys` declared 14
   keys while `assembly` accepts 16, the other two carved out by `continue`s below the table.
   Survivable only while the diagnostic never RENDERED the list — the shared check does, so a naive
   route would have printed a list refusing two keys the loader reads two hundred lines later, and
   reddened both shipped asm dialects.
3. **A local lambda named `rejectUnknownKeys` shadowed the shared helper** for its whole scope, so the
   file grepped as though it used the shared check and had **zero** calls to it.
4. **Four `$`-carve-out absences are deliberate and the first sweep read all four as live defects.**
   `tokens`, `linkageSpecifiers` and `entryFunctions` are key-spaces whose keys are SOURCE TEXT, not
   loader vocabulary. The record won; working code was not "fixed".

### ⚠ THE CYCLE'S OWN PROCESS DEFECTS, MEASURED AND FIXED

* **Lane isolation stops at the build tree.** P22's per-lane build tree isolates ARTIFACTS only.
  ✔MEASURED twice here: four lanes shared one scratchpad and one lane's mutation harness was
  OVERWRITTEN mid-run (three red-on-disable cycles then ran the WRONG SCRIPT); and a lane left a
  shared source file non-compiling across ~25 minutes of another lane's cycle. Fixed as rules; the
  SOURCE-TREE half is a §B and is now its own OPEN row. ★ A third instance is the first that CORRUPTS
  a build rather than blocking it: a lane build failed with `nlohmann/json.hpp: No such file` on a TU
  whose target already linked the library — ninja compiled it with an include set captured before
  another lane's `CMakeLists.txt` edit landed mid-build. **It cost a mutant arm and was caught only
  because the subject binary's mtime had not advanced.**
* **The orchestrator's row-editing script truncated the registry to ZERO LINES** on an encoding error,
  recovered from HEAD only because that file happened to be clean at that moment. Root cause
  underneath: the repo's OWN documented heredoc-eats-backslashes trap, walked into three times because
  every failure was silent. Row editing is now atomic and refuses to shrink the document.
* **A brief stated an invocation its author had never run** (`run-gate.sh`'s argument form). Two lanes
  hit it, it refused fail-closed, one left a file named `--` in the repo root. New rule: a brief may
  state an INTERFACE only if its author has run it — P22's instrument rule, one level up — now widened
  to MECHANISMS (see ruling 2).
* **A lane message went to the WRONG LANE.** An ownership-narrowing message reassigning a file set was
  delivered to a lane that did not own it. Had it been obeyed, two lanes would have edited one file
  set and **both** reports would have become unattributable. It cost nothing for exactly one reason:
  **the recipient's brief named its own subject and listed those paths as FORBIDDEN**, so the
  instruction contradicted a written boundary instead of arriving into a vacuum. The lane refused it
  and answered with a measurement rather than a denial. ★ **An instruction that names the recipient's
  scope can be REFUTED by the recipient; one that only names the work cannot.** Now a rule in the
  skill.
* **A citation went stale INSIDE the cycle again**: a new comment cited `grep -c 'DSS_EXPORT'` → 21,
  and the comment's own text contains the token, so the real answer is 22.

### ⚠ THREE MORE INSTRUMENTS THAT LIED, ALL MINE OR THE CYCLE'S OWN

- **`local-build.sh` runs under `set -euo pipefail`, so the toolchain-read classifier added this cycle
  would never have executed** — `set -e` aborts the moment the build pipeline fails, before anything
  can name the failure, leaving exactly the misattribution the code exists to prevent. Caught by
  reading the script before writing the patch, not by running it. (The classifier exists because a
  transient g++ failure to *read* a standard-library header cascaded into ~6 bogus C++ diagnostics
  during a red-on-disable **restore** — the one measurement this project treats as proof.
  `local-build` now exits **9** for a toolchain READ failure so no reader can mistake it for a fact
  about the source.)
- **The PowerShell self-test's capture was empty on its first run** because `Write-Host` emits on the
  INFORMATION stream: a bare `| Out-String` catches nothing and the text goes to the console, so the
  arm reported *"incomplete"* over a message that was perfectly correct.
- **Self-test fixtures reddened the citation ratchet**: sample compiler output containing `path:line`
  is indistinguishable from a citation to `check-plan-citations`. The fixtures are now assembled
  rather than written literally, with the reason in a comment — the runtime strings are byte-identical
  to what gcc prints.

★ All three are the same lesson one level down: **an instrument is only proven by being driven the way
a caller drives it** — redirection, shell options and all.

### ★★★ THE STEP-10 AUDIT FOUND A FALSE CLOSURE CLAIM, AND SIX LANES OF FOLD FOUND TWENTY-SIX MORE DEFECTS

The independent self-audit at step 10 did not merely check the cycle's work — **it refuted one of its
claims.** `D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET` was marked ✅ CLOSED on a
count from a harvester that requires a `…FromName(std::string_view)` function to recognise a
vocabulary owner. An inline `==` chain in a loader body is also an owner, and it was outside that
instrument's domain entirely.

★★★ **THE RULE THIS PRODUCES IS THE MOST PORTABLE THING IN THE FOLD: A CLOSURE CLAIM INHERITS THE
BLINDNESS OF THE INSTRUMENT THAT MEASURED IT.** The count was honest about what the harvester found;
the *closure* is what was false. ✔MEASURED once the fourth owner shape was added, both halves in ONE
process: **+82 vocabulary owners and +22 hits** tree-wide — more owners than the three published arms
harvest between them — with **16 hits across 12 chain vocabularies still in `grammar_schema_json.cpp`
alone**. And the survivor had already drifted: three sentences about `compositeKind`, one saying
`'struct' or 'union'` three lines above a chain that accepts `enum`.

⚠ Read the row's *"13 → 3"* as **"3 of the shapes the instrument could see"**. The published **51**
was true when measured and is NOT stable — the same configuration read 53–56 an hour later under
concurrent lanes. Quote it with its timestamp and its arms, or not at all.

### ★★★ THE CYCLE SHIPPED A DEFECT AND THE FOLD CAUGHT IT: A COFF OBJECT THE REFERENCE LINKER REFUSES

Reachable **only** because the COMDAT weak-definition writer landed in this same commit. In
`pe::encode`'s Obj arm the block-symbol walk ran BEFORE the weak-definition group loop, so the first
symbol-table record bearing the COMDAT ordinal was a compiler-private block label rather than the
section definition symbol — violating PE/COFF §5.5.6.

✔**MEASURED WITH THREE INSTRUMENTS, NOT READ.** A walk over the emitted bytes; `dumpbin.exe`
14.51.36252.0 reading **`COMDAT; sym= sym_77`** before and **`COMDAT; sym= weakfn`** after; and
`link.exe` 14.51.36252.0 on a **single object with nothing else on the command line** —
**`fatal error LNK1162: expected aux symbol for COMDAT section 0x2`, NO IMAGE**, against a clean
1536-byte `.exe` after.

⚠⚠ **THE AUDIT PREDICTED A SILENT MIS-KEY ("two TUs' copies do not coalesce") AND THE TRUTH IS A HARD
REFUSAL.** Higher severity than the finding assumed. ★ DSS's own round trip is structurally blind to
it — the reader locates the section symbol BY NAME — so the oracle stayed green over the object
`link.exe` rejects. That limit is now written down: **when an oracle is defined by what it can
RECONSTRUCT, every property that survives reconstruction is invisible to it** — ordering, padding,
alignment, record position. Each needs a byte-level pin, never a round trip.

### ★★★ A NEGATIVE PIN WITHOUT A POSITIVE CONTROL CANNOT TELL "REFUSED" FROM "NEVER PARSED"

✔MEASURED: both `.dssir` fixtures in the text-tier suite spelled the terminator `ret`; the mnemonic is
`return`. **Every module those templates built was refused for `unknown opcode 'ret'`, so the
`EXPECT_FALSE(res->ok)` assertions had been VACUOUSLY TRUE since the day they were written** — including
the `cc` arm this very cycle had just added to close
`D-MIR-TEXT-UNKNOWN-CALLING-CONVENTION-SILENTLY-DEGRADED-TO-SYSV`. Found only because a lane added a
POSITIVE CONTROL and the control reddened. ⇒ any pin asserting a refusal owes a sibling asserting
acceptance of the same fixture minus the defect.

### ★★ THE TEXT TIERS HELD TEN SILENT DEFAULTS, NOT THE FOUR THE AUDIT LISTED

The audit named four; searching for the SHAPE rather than the list found ten. Two are worth carrying:

- **`parsePercentValue` let a class letter with no digits fall out as handle `0` — a LIVE SLOT, not a
  sentinel.** ✔MEASURED with the guard removed: in a function whose entry is `%b0`, `br %b` became a
  **self-loop on the entry block**. Well-formed module, wrong program, no diagnostic. ★★ The general
  hazard: **a malformed parse that lands on a VALID id is invisible in a way that landing on an
  invalid one never is** — every id space with a meaningful zero has it.
- **`resolveBlockRef`'s comment said the verifier would catch an undeclared branch target.** ✔MEASURED
  FALSE: verify-on-load runs *after* `finish()`, and `finish()` is what aborts (`0xc0000409`). The
  delegation reads as diligence while pointing at a pass downstream of the failure.

⚠ Severity is bounded by a measurement the brief did not have: ✔`emitMir`/`parseMir`/`emitHir`/
`parseHir` have **ZERO** callers in `src/` outside their own TUs. Both text tiers are debug/test
surfaces; `parseTypeFromText` is the one shipped door.

### ★★ A COMPILE-TIME GUARD THAT CANNOT FAIL, AND FOUR COMMENTS SWEARING IT DOES

`namesWhere<kTable.rows.size() - 1>(..., isSelectableX)` is `x == x` whenever the predicate rejects
exactly one row: both sides derive from the same table. ✔MEASURED with `g++ -fsyntax-only` over six
arms — a 4-row table with a **new selectable enumerator COMPILES** under the derived count and is a
**compile error** under the literal. So it fires only if someone widens the predicate without touching
the count — **never on a new enumerator, which is the case every comment on it claims.** Five sites
remain; `D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY` carries them.

### ⚠ THE BALANCE GATE FAILS, BY OPERATOR DECISION, AND THE NUMBER IS SAID PLAINLY

**OPEN 1018 → 1033: 7 closed, 22 opened (21 CREATED + 1 disclosed pre-existing), net +14 created-over-closed.** Operator ruling 2026-08-21, verbatim: *"we can
commit + push with the positive balance due to the really long cycle this time, but record in handoff
so these 4 are closed next cycle."*

★ **This is the §B escape hatch the rule reserves, used deliberately by the operator with the number in
front of them — NOT the gate being softened.** Every one of the 21 is a defect this cycle FOUND rather
than inherited, and each was sized before being written down. They are the next cycle's first work
(§0.00000000000000000 P25).

⚠⚠ **ONE THING WAS DELIBERATELY *NOT* DONE, AND IT MATTERS MORE THAN THE +14.** ✔MEASURED over 1,737
registry rows: **45** rows whose status cell OPENS with a closure word are not marked closed; a strict
filter (the row's own opening verdict is a closure AND nothing walks it back) leaves **6**. Repairing
those glyphs would have taken this cycle's failing gate from +3 to roughly −2 **while closing no
actual work.** The balance instrument exempts a *disclosed opening* from the net but has **no mirror
for a bookkeeping closure**, so the repair would have registered as burn-down. **That is motivated
measurement no matter how correct each individual repair is, so the repairs were not made** —
`D-GATE-BALANCE-EXEMPTS-A-DISCLOSED-OPENING-BUT-NOT-A-BOOKKEEPING-CLOSURE` carries them, and the
script's own header already records that this instrument has flattered the cycle three times before.

### ⚠ FOUR MORE PROCESS DEFECTS THE FOLD MEASURED

* **Per-lane build trees isolate ARTIFACTS, not SOURCE.** Two independent lanes were blocked or
  corrupted by a third's mid-edit file — one could not build at all (`'TypeInterner' was not declared`),
  one compiled against a half-written table and reported a config refusal **that reads exactly like a
  real regression**. The last step of every lane is the tree-wide gate, which is precisely the step a
  shared source tree makes unattributable. ⇒ a git worktree per lane, or "gate your own targets, then
  ONE serialized tree-wide gate". That is the §B this row already carries.
* **A row SKILL.md had cited for hours did not exist**, and the citation was LINE-WRAPPED, so neither
  the guard nor a grep could see it. ✔17 of the 78 anchor ids on this cycle's added lines are wrapped.
  ★ **A wrapped id does not fail — it DISAPPEARS**, the one failure mode a fail-loud project cannot
  catch by watching for failures.
* **`local-build`'s toolchain-failure detector saw READS and missed WRITES.** A write to gcc's own temp
  exits 1, indistinguishable from a real compile error — the exact misattribution it exists to prevent,
  in the half it could not see. Fixed in both twins (6 self-test arms each, parity proven by
  EXECUTION). ⚠ My own patch had two defects, both caught only by running it: the path fragment was
  `[^:]+`, which **cannot match a Windows path at all** because the drive colon ends the class.
* **Three of four positional citations in one file were already pointing at the wrong code** when
  measured — one at a range holding unrelated logic, one at pure comment, and one that **said "grep the
  predicate, not the line" and then wrote the line.**

### ★★★ THE FOURTH LEG EARNED ITS KEEP ON ITS FIRST RUN — AND FOUND SOMETHING NO AMOUNT OF RE-READING WOULD

The operator's instruction to add the macOS and arm64-VPS legs produced, in one run, a red that had
survived a full cycle of review: `CoffLocalFunctionInArchive.DssBuiltLibMemberCallingAStaticHelperExitsFortyTwo`
(added P22) builds a `pe64-x86_64-windows-exec` image from any host — correct, that is what a cross
compiler is for — and then **EXECUTES it with no host gate**. ✔MEASURED on the native aarch64 VPS:
`posix_spawn('.../main.exe') failed: rc=8` (ENOEXEC), the single red in 922.

★★★ **IT SHIPPED BECAUSE BOTH LEGS THAT COULD SEE IT CAN RUN A PE — Windows natively, and WSL through
the interop binfmt handler**, so `posix_spawn` succeeds under WSL and the arm reads as portable. ⚠ The
same shape would red any plain ubuntu CI runner. Fixed in this commit: the spawn is `_WIN32`-gated, the
BUILD half stays on every leg because it is host-neutral and it is the half that pins the reader —
the split `tests/program/test_static_link.cpp` already spells for its pe / elf / Mach-O run arms.

★★ **THE PORTABLE RULE, and it is the host-matrix twin of this cycle's instrument rule: A GATE'S BLIND
SPOT IS A PROPERTY OF ITS HOSTS, NOT OF ITS ASSERTIONS — so the way to test the gate is to ADD A HOST,
never to re-read the tests.** ✔The sweep that followed is the other half of the result: 44 `runBinary`
call sites, 27 platform-gated, 17 ungated, and every one of the other 16 spawns a NATIVE artifact or
retargets to the host — each confirmed passing on the VPS. One defect, and exactly one.
Carried by `D-TEST-COFF-ARCHIVE-RUN-ARM-NOT-HOST-GATED` and
`D-GATE-A-LEG-THAT-NEVER-RAN-CANNOT-REFUTE-A-HOST-ASSUMPTION`, both BORN CLOSED.

### ✔ GATE STATE — measured at the committed tree, not re-quoted from a lane

★★★ **THIS CYCLE'S GATE IS FOUR LEGS, ON THE OPERATOR'S EXPLICIT INSTRUCTION 2026-08-21:** *"don't forget
to run macos leg too, and vps arm64 in the end of cycle"*. Both remote legs go through
`scripts/remote-leg/remote-leg.sh` (added this cycle), which pushes the WORKING TREE — neither remote
checkout can be a `git pull`, because this cycle's work is uncommitted until the commit that carries this
file. ⚠ Read `.dss-leg-stamp` at the remote root, never the remote `git log`.

| leg | result |
| --- | --- |
| Windows `build/dbg` ctest (`run-gate.ps1`, witness `100% tests passed`) | ✅ **922/922, 0 failed, 678.01 s** |
| WSL x86_64 clean configure+build+ctest (`scripts/wsl-leg/wsl-leg.sh`) | ✅ **922/922, 0 failed, 336.99 s**, clean 766-target build |
| qemu arm64 (`QEMU_LD_PREFIX=/usr/aarch64-linux-gnu`, folded into the WSL leg) | ✅ same run |
| **arm64 VPS, NATIVE aarch64** (`remote-leg.sh --carriage arm64-vps`, `ctest -j 4`) | ✅ **922/922**, `rc=0`, clean 766-target build. ⚠ Its wall-clock figure is NOT citable — see `D-GATE-VPS-CTEST-TOTAL-TIME-REPEATED-EXACTLY-ACROSS-TWO-DIFFERENT-RUNS` |
| **macOS arm64, REAL Apple Silicon** (`remote-leg.sh --carriage macos`, `ctest -j 10`) | ✅ **922/922, 0 failed, 5030.41 s** |
| `check-anchor-balance` | ⚠ **FAILS BY DESIGN, ON AN OPERATOR RULING: 1018 → 1034, closed 7, opened 23 (22 created + 1 disclosed pre-existing), +15 created-over-closed.** The gate is NOT softened — it printed FAIL and this commit ships anyway, on the ruling recorded above, with all 22 queued as P25 |
| `check-anchor-registry` | ✅ 0 cell-width violations across 297 tables / 4,149 rows in 41 files; every `src/` anchor resolves |
| `check-plan-citations` | ✅ 3,127 positional citations across 283 documents, ratchet unbroken |
| `check-scripts-index` · `check-line-endings` · `check-diagnostic-codes` · `check-enum-name-table-guards` | ✅ 25 scripts · no CR in 2,558 paths · 386 codes, 0 collisions · 66 vocabularies, all guarded |

⚠ **WHAT THE FOUR-LEG GATE COST, SAID PLAINLY BECAUSE IT IS THE USEFUL PART:** it took **four**
rounds, and three of the four restarts were the ORCHESTRATOR'S OWN doing, not the hosts'. In order:
a `pe64` spawn arm with no host gate (`D-TEST-COFF-ARCHIVE-RUN-ARM-NOT-HOST-GATED`); a driver
differential needing a PowerShell interpreter neither remote host has
(`D-TEST-HARNESS-DIFFERENTIAL-NEEDS-A-POWERSHELL-INTERPRETER-AND-DOES-NOT-SAY-SO`); both legs
silently running SERIAL because `ssh` forwards no environment
(`D-SCRIPT-REMOTE-LEG-CTEST-TAKES-THE-REMOTE-SERIAL-DEFAULT`); and a leg rsync'd mid-edit, so it
carried a citation whose registry row did not exist yet and `anchor_registry_guard` correctly
reddened. ★★ **That last one is [[D-CYCLE-CONFIG-EDITS-NOT-SEQUENCED-AGAINST-LANE-OWNERSHIP]] with
a WIDER WINDOW: a remote leg SNAPSHOTS the tree at rsync, so "the orchestrator is a lane too" bites
harder across a carriage than it does locally — the tree it is testing stopped existing the moment
the next edit landed.** ⇒ freeze the tree BEFORE the first rsync, not before the commit.


---

## 0.000000000000000000000000 ★★★ CYCLE P22 — A `static` HELPER NOW LINKS OUT OF AN ARCHIVE ON EVERY LEG, AND THREE OF THE FOUR THINGS THAT MADE IT HARD WERE NOT IN THE ROW

**Closes `D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM` (HIGH, queued P22).**

The row said: pe64 and mach-o readers classify a non-external defined symbol as a bodyless block
label instead of an atom, so a file-local function pulled from an archive loses its bytes — loudly
when it is called, silently when it is not. That is true, and it was the smallest of the four
problems that had to be solved to make the shape work.

### ★★★ THE FOUR, AND ONLY THE FIRST WAS THE ONE THAT WAS QUEUED

1. **The classification itself.** COFF closes it from DECLARED evidence: a file-local function
   declares `IMAGE_SYM_DTYPE_FUNCTION`, and a file-local data object is a class-STATIC symbol in a
   non-code section. Both become atom boundaries BEFORE slicing, so a file-local symbol's POSITION
   stops mattering — including the trailing case the row warned the coverage guard could not see.
2. **Mach-O has no declared evidence at all** — `nlist_64` carries no size and no function bit — so
   the discrimination had to come from the FORMAT's own vocabulary rather than a DSS convention:
   `MH_SUBSECTIONS_VIA_SYMBOLS` in the header plus `N_ALT_ENTRY` on the writer's synthetic block
   labels. Together they say "every other defined symbol starts an atom of its own".
   ⚠ **This is a SHIPPING WIRE-FORMAT change and it was gated on hardware evidence, not on a DSS
   round trip** — the bit is a LICENCE for Apple's ld64 to dead-strip at symbol granularity in every
   artifact built from these objects. See the widened witness below.
3. **The archive member was being read with the wrong vocabulary.** Closed as
   `D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT`: the pull handed the
   member to a reader together with the FINAL IMAGE's schema, so its wire values were decoded
   against a document that never promised to describe them. It now resolves the member's own
   `container: "archive"` document and refuses loud if it cannot.
4. **The member's externs came back owning no library.** Two rows, one root — an object file records
   a symbol NAME and nothing else. See both below.

### ★★ THE FLAG WAS SAFE ONLY BECAUSE OF THE MARKERS, AND THAT WAS MEASURED IN BOTH DIRECTIONS

`MH_SUBSECTIONS_VIA_SYMBOLS` **without** `N_ALT_ENTRY` markers is catastrophic: five dense-switch
examples crash and ~85% of `__text` is stripped, because ld64 takes the licence and every synthetic
block label looks like an atom start. **With** the markers, all arms exit 42 and `__text` comes back
byte-identical. The final witness compared **525 objects** through Apple's real ld64 under
`-dead_strip`, flag ON vs OFF, and found **0 run disagreements**.

★ **NO C++ READS THE HEADER VALUE.** The writer copies `MachOIdentity::flags` verbatim; the reader
asks only whether the bit is present in the object it is READING. So setting that one number back to
0 is the entire rollback — the markers stay honest and the reader falls back to its narrower
external-only rule with the coverage guard still refusing any dropped body.

### ★★★ THE PART THAT COST THE MOST WAS A NUMBER IN MY OWN BRIEF

The brief handed to the mach-o lane said `N_ALT_ENTRY = 0x0020`. **It is 0x0200. 0x0020 is
`N_NO_DEAD_STRIP`.** Had it shipped, every `static` carrying `__attribute__((used))` would have had
its bytes dropped, and my earlier ld64 probes — which tested the wrong bit — had to be thrown away
and re-run. The lane caught it by measuring the constant on real clang output instead of taking it
from the brief.

⚠⚠ **THIS IS THE THIRD TIME IN SIX CYCLES A LANE BRIEF CARRIED AN ERROR WITH AUTHORITY ATTACHED.**
The rule already exists — *name the symptom, not the fix* — and it did not save this one, because
the error was not a prescription, it was a FACT stated without a citation. The rule needs the second
half: **a brief may state a measurement only with the instrument that produced it**, so the lane can
re-run it. A bare constant is not a measurement.

### ✅ THE TWO LIBRARY-BINDING ROWS — same root, and the second one predates the first's fix

`D-LK-ARCHIVE-MEMBER-EXTERN-LOSES-ITS-LIBRARY` (platform half) and
`D-LK-ARCHIVE-MEMBER-EXTERN-UNBOUND-BY-RESOLVE-LIBRARY` (operator-named half), both BORN CLOSED.
An object file records an undefined symbol's NAME and nothing else, so the binding has to be
re-derived at pull time. Two tiers were ruled out BY MEASUREMENT rather than by argument: the READER
cannot recover it (the emitted `.lib` and `.a` were dumped and carry the symbol name and no library
string at all), and the LINKER cannot either — binding must precede the cross-CU merge, whose dedup
key is `(mangledName, libraryPath, version)`, or one C runtime ends up with two import descriptors.

★ **THE PLATFORM HALF UNDERSTATED ITSELF BY ONE HALF:** a compiled TU emits the PLATFORM LINK NAME,
not the C identifier, so un-decorating recovers a spelling the descriptor corpus is not keyed on.
Exactly **nine** shipped rows realize to a link name the corpus does not separately declare
(`write`→`_write`, `lseek`, `getpid`, `_setjmp`→`__intrinsic_setjmp`, and five Darwin `$INODE64` /
`$DARWIN_EXTSN` forms). A reverse index keyed by each row's REALIZED link name closes it.

★★ **THE OPERATOR-NAMED HALF PRODUCED THE CYCLE'S SHARPEST TEST FINDING.** Dropping the new
threading at ONE driver call site reproduced the original CLI failure exactly while the entire
in-process suite stayed **green at 32/32** — every unit case calls `pullStaticArchiveMembers`
directly and constructs the argument the driver is supposed to supply, so no unit case can ever
witness a gap in the supplying. A driver-level pin now exists; the CLASS is anchored OPEN as
`D-TEST-STATIC-LINK-UNIT-SUITE-CANNOT-WITNESS-A-DRIVER-THREADING-GAP`.

### ⚠⚠ A CONFIG CORRECTION KILLED TWO PINS, AND THE PINS WERE THE ONES IN THE WRONG

Surveying relocation rows turned up `macho64-x86_64-darwin-exec` and `-dylib` declaring
`X86_64_RELOC_BRANCH` with a value that, under the packing **those same files state in their own
comments**, decodes to `r_type=SIGNED`, 8 bytes, not PC-relative — every field contradicting the
row's own name — and an `UNSIGNED_4` row declaring a TWO-byte slot. No external authority was needed;
the documents refute themselves. Corrected
(`D-CONFIG-MACHO-X86_64-EXEC-DYLIB-RELOC-NATIVEID-CONTRADICTS-ITS-OWN-ROW`).

Two pins then went red, and both had been **asserting that reading a member through the IMAGE
document REFUSES** — a refusal that only ever happened because of that typo. They were asserting a
CONSEQUENCE that rested on a defect, so they stopped discriminating the moment it was fixed.
★ Neither was reverted. Both were re-derived onto the CONTRACT — which document the member read
RESOLVES — which keeps discriminating even where two vocabularies legitimately agree. The
re-derivation was proved by a mutant that makes the resolver fall back to the image format: the
mach-o case now reds under it, and **under the old assertion it could not have.**

⚠ The ELF sibling arm was checked rather than assumed and KEPT: `elf64-x86_64-linux{,-staticlib}`
declare a PLT variant and an `emitOnly` alias that the `-exec` document does not, so a member calling
a library function emits a wire value no image document has a row for. That difference is
structural. Its failure guidance was inverted, though: it used to say "must be re-derived, not
deleted", which is exactly the advice that produced the mach-o situation. It now says that if those
vocabularies ever legitimately converge, the block should be DELETED, because the contract is pinned
elsewhere and hunting the corpus for a fresh coincidence is asserting the corpus.

### ★★★ THE UNKNOWN-KEY CLASS — "a fourth hand-rolled copy" was MEASURED to be 58

The cycle promoted `rejectUnknownKeys` into `config_key_vocabulary.hpp` because a fourth copy had just
been minted. ✔The survey that followed found **eight named helpers over 57 call sites plus ~50
open-coded inline loops** — 58 independent implementations of one check.

★★ **AND THE ARGUMENT FOR THE CLASS TURNED OUT NOT TO BE TIDINESS. Two copies held LIVE defects, both
of the INVERSE kind — loaders REFUSING a valid document.** `optimizer_json.cpp` had no `$`-documentation
carve-out at all, so a `$comment` in a pipeline document was rejected as a typo at all four of its
objects; `shipped_lib_descriptor.cpp` honoured it at ONE of NINETEEN objects and only for the literal
spelling `$comment`, so `$abiComment` — the shape shipped descriptors actually use — never worked even at
the root. ⇒ **a duplicated CHECK is not only a place a check can be MISSING; it is a place the check's own
EXEMPTIONS can be forgotten, and that failure direction is the one nobody thinks to test for.** Both are
fixed BY CONSTRUCTION: the caller no longer writes the loop.

Then the object-format loader, which had exactly TWO checks (document root, `relocations[]` row) against
its siblings' 28 / 23 / 7 / 6. **Ten blocks now checked, twelve call sites** — three of them (`format`,
`cSymbolDecoration`, `sehPersonality`) missed by the survey that scoped the work.
★ **The two PER-MECHANISM blocks are deliberately NOT unioned.** Flattening `processExit` /`processArgs`
would accept a cross-arm key the declared mechanism's parse code never reads —
`D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY` rebuilt in a new place — so a sibling-arm key is refused
with a diagnostic that NAMES the arm it belongs to. Every derived set was dry-run against all 24 shipped
documents BEFORE any code was written: zero violations.

★ **THREE SHIPPED DIAGNOSTICS TURNED OUT TO DESCRIBE A DIFFERENT SCHEMA THAN THE LOADER PARSES** —
`tlsAccess` listed 3 keys where the arm reads 4, `librarySynthesis` advertised `libraryPath` as a KEY when
it is a value resolved from `role`, and `processArgs` named one of its two accepted mechanisms. Harmless
while unknown keys were ignored; **actively wrong once they are refused**, because an author following the
message writes a key the guard now rejects. Found only because adding a discriminator forces the message
and the parse arm to be read side by side.

⚠⚠ **AND ONE LIVE SILENT DEFECT THAT WAS CLOSED RATHER THAN DEFERRED:** the grammar loader's `language`
block had NO discriminator at all, and the gap had already **bent the schema** — `isa` and
`identifierClass` sit at TOP LEVEL, where neither belongs, for the loader's own stated reason that
`language` could not check them. The direct failure was silent in the worst way: `fileExtensons` loaded
perfectly clean and produced an EMPTY extension list, i.e. a language that recognises no source file. Three
keys, derived from the parse arm, dry-run against all six shipped `.lang.json` documents first. It shares
ONE loop with `checkDocumentKeys`, so it did not become a 59th copy.

### ⚠ THE WORKING TREE MOVED UNDER A LANE, AND IT FLIPPED A VERDICT

A config document was rewritten DURING a lane's mutant build, changing a test's verdict between two
runs of the same binary; the lane then reported a stale state as a defect. Live instance of
`D-CYCLE-CANNOT-ASSUME-IT-OWNS-THE-WORKING-TREE`, and the new part is the consequence: **it can
silently corrupt a red-on-disable observation**, which is the one measurement this project treats as
proof. The scheduling rule that follows is not "be careful" — it is that config edits must be
sequenced against lanes exactly like source edits, because a lane's ownership boundary protects
`src/**` and said nothing about `src/dss-config/**`.

### ⚠ CARRY FORWARD

★★★ **OPERATOR RULING 2026-08-20, AND IT CHANGES THE QUEUE, NOT THE BAR:** *"we don't want no open
anchors, so this cycle can end with it opened, but next one must address it if addressable (use
/dss-cycle)"*. A cycle that finds a real defect too large to fix inside it MAY open the row and ship
rather than compressing the finding to fit — and the rows it opened become the NEXT cycle's FIRST
picks, ahead of whatever was queued. "If addressable" is a MEASURED judgement (trigger not fired, or
an operator §B decision is required), never a shrug, and it is stated IN THE ROW at the moment of
deferral. The balance gate is untouched and still refuses `after > before`.

**⇒ P23 IS RE-SCOPED TO THIS CYCLE'S OWN OPENED ROWS**, and
`D-HARNESS-PE64-LIB-ACQUISITION-IS-HOST-DEPENDENT` (HIGH) moves down one slot to P24.

★★ **OPERATOR NARROWED THE RULING THE SAME DAY, AND IT IS THE HALF THAT MATTERS:** rows may be left
open *"only if strictly needed due to cycle size, amount of changes or bigger stuff"*. That is not a
convenience clause, and applying it honestly cost one of this cycle's three deferrals and corrected
the sizing of the other two.

  * `D-CYCLE-CONFIG-EDITS-NOT-SEQUENCED-AGAINST-LANE-OWNERSHIP` — **NOT deferred; CLOSED in this
    commit.** Its whole fix is a rule written down, the same shape as the P18/P19 pairing rulings,
    and it is now step 5 of `dss-cycle/SKILL.md`. Deferring it would have been exactly the
    convenience the narrowing forbids.
  * `D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB` — deferred to P23. ⚠ **SIZING CORRECTED:**
    `definedName` has ~5–6 ET_REL call sites across `elf.cpp`/`macho.cpp`/`coff`, all fed by ONE
    `ObjectSymbolNames` helper, and the alias rows already carry the owner's `SymbolId`. This is an
    ordinary lane, not a monster.
  * `D-TEST-STATIC-LINK-UNIT-SUITE-CANNOT-WITNESS-A-DRIVER-THREADING-GAP` — deferred to P23.
    ⚠⚠ **SIZING WAS WRONG AND IS RETRACTED:** it was first reported as "76 test files, not
    mechanically separable". That number sizes a DIFFERENT question — tests asserting on
    shipped-config values — and was attached to this row by mistake. The real question is how many
    PIPELINE ENTRY POINTS take a parameter the driver must supply, and `compile_pipeline.hpp` exports
    about **fourteen**. Bounded audit, one lane.

★ **SO THE DEFERRAL RESTS ON THIS CYCLE'S SIZE, NOT ON THEIRS**, and that is the claim to check
rather than inherit: a shipping wire-format change gated on hardware evidence, a new object-format
schema field declared across thirteen documents, three object readers rewritten, four lanes, ~35
files. Two more substantive lanes on top is the pile-up that makes a cycle unreviewable. If a future
reader disagrees with that judgement, the rows are ordinary and can simply be taken.


⚠ **THE LESSON THIS CYCLE OWES THE NEXT ONE, and it is about briefs, not about linking:** a brief may
state a MEASUREMENT only together with the instrument that produced it. A bare constant handed to a
lane is not a measurement, and this cycle nearly shipped a wire-format bit dropped from the wrong
half of `n_desc` because of one.

---

## 0.00000000000000000000000 ★★★ CYCLE P21 — THE MANIFEST EDIT THE ROW ASKED FOR WOULD HAVE ASSERTED NOTHING, AND SIX CORPUS LINTS WERE NEVER RUNNING

**Priority:** `D-EXAMPLES-DEPENDSON-NO-RELEASE-OPTIMIZER-ARM` (queued P21). **Anchors: closed 1, opened 1,
net 0** — OPEN 1019 → 1019. Six further rows were opened and CLOSED in this same commit and carry no OPEN
weight. **Tests 904 → 906.**

### What the row asked for, and why doing only that would have been a lie

📄The row asked for a `release` arm on the corpus's only two `dependsOn` manifests. ✔**MEASURED: adding it
alone asserts nothing.** BOTH runners built the prerequisite library at the BASELINE configuration in every
arm — an arm labelled `release` re-compiled only the FINAL exec under the release pipeline and then linked
it against a DEBUG-pipeline archive. The `-staticlib` / `--resolve-library` path the row exists to cover
would have stayed exactly as un-optimized as before, under a manifest now claiming otherwise.

Fixed in both, the same fix in each runner's own vocabulary:
- `tests/examples/examples_runner.cpp` — `buildDependencyArtifact` takes a pipeline and a `CompileConfig`
  override, applies them to the prerequisite's own `Program`, and threads them down the RECURSION so a
  NESTED prerequisite is built under the arm too.
- `integrated_tests/runner.cpp` — `buildDependsOnArtifactCli` takes the arm's `configName` and emits
  `--config=<name>` on every prerequisite compile.

✔And the examples themselves had **nothing to optimize** — their helpers were straight-line returns, so the
release pipeline would have produced a byte-identical archive and `mustDifferFromBaseline` would have red
everywhere. Rewritten with a loop, a loop-invariant addend and promotable locals, so Inlining / Mem2Reg /
CSE / LICM / DCE are witnessed on the ARCHIVED TU and not only on the exec.

✔**MEASURED, debug → release, on the LIBRARY images — all 12 differ** (`dsslib.a` / `input.a` / `fatlib.a`;
the middle one is the NESTED prerequisite, which is what proves the recursion carries the config):
pe64-x86_64 866→664 / 886→674 / 1608→1192 · elf64-x86_64 1672→1464 / 1684→1468 / 2962→2530 ·
macho64-arm64 770→650 / 780→656 / 1474→1230 · elf64-aarch64 1512→1376 / 1524→1380 / 2642→2362.

### ★★★ THE FINDING THE ROW PREDICTED — AND IT WAS ONE KEYWORD

Writing the new helper as `static`, the spelling any C author would reach for, made the **pe64 and macho64
links FAIL**. ✔Root-caused: those readers classify a non-external DEFINED symbol as a bodyless block label
instead of an atom, so its bytes never enter the linked image. A third, independent bug surfaced on
macho64-x86_64 (conflicting relocation `nativeId`s between the object and image vocabularies).

Per the row's own instruction — *"do NOT drop the arm to get green"* — the arm stayed. By operator ruling
the `static` KEYWORD alone was removed (the helper stays, so every pass above is still witnessed), the
mechanism was opened HIGH as `D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM`, and its
**silent half was made fail-loud in this same commit** rather than deferred with it:
`everyDefinedSymbolIsCoveredByAnAtom` in the new `src/link/format/object_atom_coverage.hpp` — ONE inline
implementation called identically by all three readers, asking the question in the neutral
`(sectionKey, byteOffset)` coordinates every reader already stages, so there is **no format test anywhere in
it**. Refusal is a NEW code, `F_ObjectReaderSymbolBodyDropped`, deliberately not `F_CorruptedBinary`: every
other reader refusal means *these bytes are not a well-formed object*; this one means the opposite — the
object is fine and the READER cannot represent it. ✔**BLAST RADIUS ZERO** (38 `link/` entries, 7 `program/`
entries, 5 archive-touching examples, 7 real-toolchain witnesses), because no archived TU in the corpus
contains a file-local function.

⚠ **The guard does NOT catch everything, and the row says so rather than leaving it to be discovered:** a
file-local function that TRAILS an external one is covered by the preceding atom and rides along inside the
WRONG atom instead of vanishing. Telling that from a genuine interior label needs the size a non-external
symbol does not carry — i.e. exactly the wire-format work the classification fix owes.

### ★★★ THE FINDING NOBODY PREDICTED — SIX CORPUS LINTS HAD NO CTEST ENTRY

An implementation lane wrote three new pins into `ExamplesCorpusLint` and reported plainly that it could not
watch them fail. Planting mutants to watch them red is what revealed **they were never running**.

✔MEASURED: **8 tests defined in the suite, 2 selected by any ctest entry.** The per-example entries exclude
`ExamplesCorpusLint.*` wholesale, so nothing else could pick the rest up. ✔ROOT-CAUSED to `2fc87192`
(cycle P5b, 2026-08-15): splitting the carve-out census onto its own named entry narrowed the general
entry's filter from the whole suite to ONE test name — and **the comment directly above it, which says the
filter selects the whole suite, was left untouched**. Three lints went dark that day and stayed dark through
P6–P20; this cycle's three would have made six. `orphan_tests_guard` cannot see it and is not at fault: it
asks whether a test SOURCE is compiled and run, and this one is both.

★ **Fixed by making the registration DEFAULT-IN.** One pair list `EXAMPLES_LINT_NAMED_ENTRIES` holding
`<GTestName>` and `<ctest entry name>` per lint that earns its own name, and the SAME loop iteration creates
the dedicated entry AND subtracts that test from the catch-all's filter — so "moved, not dropped" is one
statement pair and the two halves cannot drift. ✔After the fix **8 of 8 selected**, none twice, and the
three long-dark lints all PASS — nothing had rotted behind the darkness, which is luck and is recorded as
luck.

### ✅ RED-ON-DISABLE, WATCHED RATHER THAN READ

Each pin under its own mutant, each reddening ALONE, binary mtime confirmed to advance every time:
- drop the config override in `buildDependencyArtifact` ⇒ both examples red, naming the dependency by its
  spec-and-artifact key and reporting the exec AND the library byte-identical;
- ignore the per-dependency opt-in ⇒ `DependencyMustDifferFromBaselineIsWiredEndToEnd` reds;
- open the `dependsOn` entry key set ⇒ `DependsOnEntryRefusesAnUnknownKeyAtEveryDepth` reds;
- open the per-target key set ⇒ `ManifestRefusesAnUnknownKeyAtTopLevelAndPerTarget` reds.

### ✅ EVERY LEG — AND TWO OF THEM BY EXECUTION ON REAL HARDWARE

- **pe64-x86_64** — Windows gate: both examples green at debug and release.
- **elf64-x86_64** — WSL gate: both examples green at debug and release.
- **elf64-aarch64** — native aarch64 VPS, RUN: 42 / 42 / 42 / 42.
- **macho64-arm64** — real Apple Silicon, RUN: 42 / 42 / 42 / 42.

### The five other rows opened and closed in this commit

`D-DOC-EXAMPLES-README-ASSERTS-A-REPAIRED-RUNNER-ASYMMETRY` (the author-facing schema doc asserted, with a
✔MEASURED mark and a date, a runner asymmetry repaired three days after it was written; every positional
citation converted, file now at ZERO — and the first repair MISSED A SECOND COPY of the false claim seven
lines below the correction, which is the finding worth carrying) · `D-GATE-CITATION-GUARD-BLIND-TO-MARKDOWN-OUTSIDE-THE-DOC-ROOTS`
(the guard enforcing *never cite a line number* could not see that document; `.md` joined the code family,
five roots added, `DOC_FLOOR` 40 → 45, self-test 12 → 23 arms) ·
`D-TEST-EXAMPLES-MANIFEST-KEYS-SILENTLY-IGNORED` (four of five manifest levels silently ignored unknown
keys, so `mustDifferFromBaseLine` would have parsed clean and disarmed the very assertion this cycle added;
five closed key sets now, byte-identical in both runners) · `D-PLANS-REGISTRY-UNMARKED-DUPLICATE-ROWS`
(✔1658 rows under 1653 names AT THIS COMMIT, 5 names duplicated; the two unmarked duplicates marked
SUPERSEDED, nothing retracted — the row's own 1651/1646 was measured before this cycle's 7 rows landed) ·
`D-BUILD-LANE-TREES-NOT-NAMED-LANE-SURVIVE-THE-COMPLETION-CHECK` (7.2 GiB of cycle-P10 lane scratch survived
ten cycles because the completion check keys on the NAME `build/lane-*`) ·
`D-GATE-CITATION-GUARD-BLIND-TO-CONTINUATION-CITATIONS` (the guard could see only the FIRST line number in
`<file>.cpp:<line>/:<line>/:<line>` — **35** references across **19** documents counted by nothing).

### ⚠⚠ THE INDEPENDENT AUDIT OF THIS CYCLE'S OWN DELTA IS THE PART TO READ

It returned **12 findings**, and the pattern across them is one thing: **this cycle repaired rot and then
produced fresh rot inside the repairs.** The three that changed the work rather than the wording:
1. **The `examples/README.md` repair fixed one of TWO copies.** The same claim, the same three zeros, the
   same ✔ mark, in the present tense, seven lines below the correction. ⇒ **when retracting a measured
   claim, grep the document for the CLAIM — never edit the paragraph you happened to find.**
2. **The replacement counts were already stale in the commit that shipped them** (26/21/25/17 against a live
   31/23/29/26), because they were taken mid-cycle while the file was still growing. ⇒ **a count of a token
   in a file the same commit is still editing must be taken LAST**, and the convention stated with it.
3. **The default-in lint registration left the STRING unguarded.** A typo in the pair list's GTest name
   selects nothing, and ✔a gtest filter matching nothing exits **0** — a permanently green entry asserting
   nothing, i.e. the very class the fix was for. Closed with a ctest `FAIL_REGULAR_EXPRESSION` on the
   runner's own *did not match any test*, applied to EVERY entry the file registers, red-on-disable watched.
⚠ And a fourth, recorded because it is funny and instructive: the comment introducing the new continuation
matcher illustrated it with a REAL three-site citation and moved that guard's own ceiling 3 → 7. Caught by
reading the re-derived inventory delta, not by review.

### ⚠ CARRY FORWARD

1. **P22 is `D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM`, operator-ordered for the
   immediate next cycle.** Read the row before planning: it carries the write-vs-read measurement (all three
   formats KEEP the bytes at object emit — the loss is exclusively on the archive-member READ-BACK path), the
   boundary the guard does not catch, and the macho64-x86_64 relocation-vocabulary bug.
2. **The corpus still contains no archived TU with a file-local function**, which is why the guard's blast
   radius is zero — and also why the classification bug can only be witnessed by an example that deliberately
   plants one. Any such example must land WITH the fix, not before it.
3. ⓘ A COFF archive embeds the source stem, so archive byte counts move with FILENAME LENGTH. Two honest
   measurements of the same pe64 delta differed by a constant 202 bytes for that reason. Never compare an
   archive size across a rename.
4. ⚠ **A BARE `ctest` RUNS THE 906 ENTRIES SERIALLY.** ✔Re-hit 2026-08-20: the `CTEST_PARALLEL_LEVEL=8`
   default P17 added lives in `scripts/run-gate/run-gate.sh` and `scripts/local-build`, NOT in the build
   tree, and CTest has no project-level way to set it — it reads the ENVIRONMENT only. So invoking `ctest`
   directly turns a ~12-minute gate into ~43 minutes with no warning. Route every gate through
   `run-gate.sh`, which is also what supplies the tool-emitted success witness.

---

## 0.0000000000000000000000 ★★★ CYCLE P20 — `asm goto` WORKS, AND THE ROW THAT ASKED FOR IT UNDERSTATED THE JOB BY ONE CFG EDGE

**Closed `D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER`; opened-and-closed five more; net −1.**

### What the operator can now write, and what it does

```c
__asm__ goto ("cmpl $0, %0\n\tjne %l[jumped]" : : "r"(x) : "cc" : jumped);
__asm__ goto ("movl $3, %0\n\tcmpl $0, %1\n\tjne %l2" : "=&r"(r) : "r"(x) : "cc" : one, two, three);
__asm__      ("movl %[in], %[out]" : [out] "=r"(out) : [in] "r"(a));
```

✔`examples/c-subset/asm_goto_labels` exits **42** in **both** configs on **all five legs**:
pe64-x86_64 (native Windows), elf64-x86_64 (WSL), elf64-aarch64 under **qemu**, elf64-aarch64 on
**NATIVE aarch64 hardware**, and macho64-arm64 on **REAL APPLE SILICON**. Six shapes, each
independently discriminating.

★★ **THE LAST TWO LEGS EXIST BECAUSE THE OPERATOR SAID SO MID-CYCLE.** The Mac is normally off
and the row was about to ship claiming macho64-arm64 *“compiles (no runner off-Mac)”* — a census
verdict, which is precisely what this project's standard rejects. When the operator said both
hosts were up, the cross-compiled binaries were shipped over the two carriages and RUN. ⇒ the
aarch64 answer no longer rests on emulation, and Mach-O is no longer compile-only.
⚠ Getting there re-ran two known traps in one sitting: an inline `scp` invocation built by hand
failed on quoting (the carriages have `--rsync` for exactly this), and an `&&` chain printed
`RSYNC-OK` over two rsync FAILURES because each stage was piped through `tail` and the pipeline's
status is `tail`'s. **Both are already written down. Writing them down is not what stops them —
using the script that exists is.**

### ★★★ THE FINDING, AND IT WAS NOT IN THE ROW

✔MEASURED through the shipped CLI *before any code changed*, on a program whose only statement after
the asm was a `return`: the refusal renders `succs.size()`, and it read **1** for one label and **2**
for two. **The fall-through contributed no successor.** `MirBuilder::addInlineAsmGoto` pushed one
entry per label and nothing else; `hir_to_mir` then opened a block its own comment called dead,
*“which the mandatory unreachable-prune drops”*.

✔MEASURED on gcc 13.3.0, clang 19.1.1 and aarch64-linux-gnu-gcc 13.3.0: `asm goto` **falls through**
when the template does not branch (exit 7 vs 3). ⇒ **DSS was deleting the fall-through path's code.**

★★ **IT COULD NOT SHIP, AND THAT IS THE LESSON RATHER THAN THE EXCUSE.** The LIR tier refused
`asm goto` outright, so the wrong CFG never reached codegen. The defect was invisible *because* a
capability was missing, and binding the label without fixing the edge would have converted a
fail-loud refusal into a silent miscompile in one commit. **A cycle that had done only what its row
asked would have shipped it.**

### ★★★ THE ARCHITECTURAL CALL: CARRY SPELLINGS, NOT NAMES

Three symptoms shared one root — no tier below the front end knew what the embedding language CALLS
an operand or a label. `mir_to_lir` synthesized `%N` from an index and had nothing else, so
`%[name]` was **refused on a program both references compile**, `%l[name]` had no carrier, and `%lN`
had no index rule.

The first design computed the numbering in `src/lir/` — **inside the agnosticism veto list**. An
independent design audit named the asymmetry that settles it: the template sigil BYTES already had a
config owner; the NUMBERING never did. ⇒ the front end mints every spelling from the declared
lexemes, the descriptor CARRIES them, every tier below only COMPARES, and `asmOperandSpelling` is
**deleted rather than kept as a fallback** — a fallback is a second owner of the fact being fixed.

★★ **AND IT DISSOLVED A DIVERGENCE INSTEAD OF PATCHING ONE.** `MirAsmDescriptor::inputs` carries
SYNTHESIZED entries (one tied input per `"+"` output), so an index computed at MIR overcounts by the
number of `+` operands while the source counts each once. ✔The discriminating program exists and gcc
compiles it. Minting at the front end deletes the second count.

### ⚠⚠ THE AUDITS EARNED THEIR KEEP — AND ONE OF THEM FOUND A MISCOMPILE THE CYCLE ITSELF CREATED

Two design audits ran before the diff and two adversarial audits after it. **The most valuable
finding was a defect that did not exist when the cycle started.**

1. ★★★ **A SILENT MISCOMPILE THIS CYCLE OPENED.** Two operands sharing a `[name]` bound the WRONG
   one: every spelling lookup below the front end is a FIRST-MATCH scan over a list whose outputs
   come first. ✔MEASURED out of the shipped CLI — `movl %[v], %[out]` with two `[v]` operands
   compiled rc=0 and returned **0 instead of 20**, at debug and release. Before `%[name]` bound at
   all the shape was refused BY NAME, so **making the feature work is exactly what converted a
   fail-loud refusal into a wrong answer.** ⇒ refused now at the tier that holds the operand list,
   with a fail-loud collision check at the LIR tier too (direct-API callers never run the C
   semantic pass). ★ The fix lane then MEASURED past its brief: GNU keeps operand names and
   `asm goto` label names in **ONE name space** — gcc and clang reject all three collision shapes
   (operand/operand, label/label, operand/label) with the same message — and the label side is a
   real defect too, because the LIR tier mints a distinct capture block per edge index.
2. **The design's central claim was refuted.** *“The numbering lives in exactly one place”* was
   **false**: `scanInlineAsmTemplate` re-derived `base = operandCount` in five places, and this
   cycle's own pin proved they were SEPARABLE — forcing the minting base to 0 left the semantic tier
   green, the divergence surfacing two tiers later. Fixed by making that tier look the written form
   up in the minted spellings, so the two now agree **by construction**. ★ The rewrite also stopped
   parsing an index into a number at all — it carries the DIGIT BYTES — so a monstrous index can no
   longer wrap a `size_t` into an in-range one, and `%00` is refused where the operand list lives
   instead of dying at a binding that matches on the exact spelling.
3. ★★ **MY OWN AGNOSTICISM ARGUMENT WAS WRONG.** I justified moving the numbering out of `src/lir/`
   as leaving the veto list. ✔`src/analysis/` is in the **same** shared-substrate list. The move is
   still right — three owners became one, and the count now comes from the SOURCE list, which is the
   correctness fix — but the headline reason was not. The residual is anchored trigger-gated
   (`D-CONFIG-INLINE-ASM-TEMPLATE-NUMBERING-HAS-NO-DECLARED-OWNER`) rather than glossed.
4. ★★ **THE HEADLINE PIN CARRIED A ✔ WITH NO RUN BEHIND IT**, and running it showed the *design* was
   wrong too. Passing `labels[0]` as the fall-through keeps the arity legal but makes the builder
   open one block twice and **abort** — the failure mode the plan had rejected a different mutant
   for, arrived at from the other side. ⇒ the discriminating mutant REPRODUCES THE PRE-CYCLE SHAPE:
   seal the continuation and strand the following statements. ✔It **compiles rc=0** and the program
   dies with an illegal instruction (**rc=132**) at both configs, GOOD and RESTORED 42/42.
5. **A closure row overstated its own measurement.** The label-index pin's *“33 instead of 11”* was
   measured on the shape **ISOLATED**; in the shipped composite the mutant is caught earlier, at
   build time, for a different shape's reason. The example's header said so and the row did not —
   the row was written from a lane's summary rather than from the artifact. ⇒ **a pin measured in
   isolation and the same pin in the composite can red for different reasons, and only the composite
   is what `ctest` runs.**
6. **A shape nothing covered**, found by reading and settled by running: a **pinned** output on an
   **unconditionally branching** template — the one case where the capture register is defined on the
   label edge and not on the fall-through. ✔compile 0, run 42 at both configs; now SHAPE 6.
7. **A brief premise of mine measured FALSE.** I predicted the piece-0 defect would be silent; it is
   fail-loud. The lane implemented as specified and corrected the claim rather than stopping.
8. **The new verifier guard had no test at all** — the guard created *because a docblock claimed a
   check that did not exist* was itself shipping unexercised. Five arms now drive it through the
   **direct `Mir` ctor**, which is the path `MirBuilder` does not own and the whole reason it exists.

### The four other rows this cycle opened and closed

- **`D-MIR-VERIFIER-DOES-NOT-CHECK-SUCCESSOR-ARITY`** — `recordSuccessors_`'s comment claimed *“ML3's
  verifier re-runs the same descriptor check on any frozen module”*. ✔MEASURED false: no
  `minSuccessors`/`maxSuccessors` reference existed anywhere in the MIR verifier. Found because a new
  silent seam needed a backstop and the backstop was imaginary — `cloneInlineAsmGoto` only
  count-checks against `[min, ∞)`, so a clone dropping the fall-through from a ≥2-label goto stays in
  range and the edge vanishes with no diagnostic.
- **`D-MIR-TEXT-INLINE-ASM-RENDERS-A-POOL-INDEX-AND-NO-EDGES`** — both asm opcodes fell into the
  writer's `default:`, rendering a raw descriptor-pool index and, for a terminator, **no CFG edges at
  all**; zero tests touched either mnemonic. ⚠ Two things surfaced while pinning it: the marker had
  to be a SINGLE TOKEN, because the parser's recovery re-tokenizes a refused instruction's tail and
  the bare word `not` **is** a MIR opcode (the first spelling aborted the process); and the parser's
  refusal was **not survivable** — its own comment says *“a refusal that crashes is not a refusal”*,
  and it crashed at the NEXT block's `beginBlock`, two steps before the `finalize()` guard that was
  supposed to catch it.
- **`D-GATE-CITATION-GUARD-BLIND-TO-SOURCE-AND-CONFIG`** — the guard for *“never document a line
  number”* read `.md` under two roots and could not see the code. ✔The evidence is a citation that
  went stale **inside this cycle**: one lane cited a `mir_opcode.hpp` line, a sibling lane's edit
  moved the row, nothing could report it. Now covers `src`/`tests`/`scripts`/`examples`; baseline 75
  documents / 2,374 citations → 278 files / 3,059.
- **`D-DIAG-INLINE-ASM-INDEX-DOCBLOCKS-DESCRIBE-A-REPLACED-REFUSAL`** — docblock-only, reported by a
  lane forbidden to touch the file.

### ⚠ CARRY FORWARD

- **`D-LIR-VERIFY-VREG-CLASS-RULE-ASSUMES-A-ONE-TO-ONE-LIR-TO-MIR-MAP` gained a second measurement**
  and its closing work now owes both shapes: for an inline-asm template the `lirToMir` map has **no
  entry at all** — not many-to-one, ZERO-to-one — because `lowerAsmTemplateToLirRun` appends straight
  into the `LirBuilder`, bypassing `recordSource`. Every instruction an `__asm__` lowers to has no MIR
  provenance.
- The `%w0`-style operand **modifier** vocabulary is still owed (it is why every aarch64 arm in the
  asm examples widens its operands to `long`). That belongs to
  `D-CSUBSET-INLINE-ASM-OPERANDS`' deferral tail and still has no row of its own.

## 0.000000000000000000000 ★★★ CYCLE P19 — THE SECOND PAIRING ANCHOR WITHDRAWN, AND THREE CITATIONS THAT HAD QUIETLY GONE FALSE

**One row closed, net −1. No code changed.**

`D-GATE-SCRIPT-PS1-CONTENT-DRIFT-UNCHECKED` was the behavioural half, split out of the existence half in
2026-07-31. **Operator ruling 2026-08-19:** *"the parity must be checked in the review, before the commit,
when the script is being created or modified. Not after and not a script to it. After committed it must be
already working. There is no easy way to script automate this because the scripts can do literally
anything."*

⭐⭐ **THE ROW'S DIAGNOSIS IS RIGHT AND ITS PRESCRIPTION IS NOT.** Its three instances stand — ✔the
anchor-registry twins reported **777 vs 781** because each reimplemented the scan; ✔a `.ps1` launcher
REJECTED an `--out` its `.sh` sibling accepted; ✔the `--rsync` transport exists in the `.sh` carriage and
*cannot* exist in the `.ps1` one. **The third is the refutation of the second:** a legitimate permanent
divergence sits in the same measurement as two real defects, so any detector needs a declared-exception
list to avoid convicting correct work — and the row already conceded that, delegating the design to the
row withdrawn one cycle earlier. Equivalence of two arbitrary programs is not decidable, and these are
shells.

**The obligation now lives in `dss-cycle/SKILL.md` step 6 (Review and fold) and the `dss-code-prime` skill.**
ⓘ **What survives as a DESIGN and beats both a detector and vigilance:** the row's own item (a) — collapse
the pair to ONE implementation with thin launchers, the shape `corpus-census` and `pragma-profile-census`
already use. That removes the possibility of drift instead of detecting it.

★★ **AND THE CITATION SWEEP WAS THE REAL FIND.** An independent audit classified all **50** surviving
citations of the two withdrawn rows (the estimate of ~34 was low — several rows cite them twice): 8 mean
EXISTENCE, 17 mean BEHAVIOURAL DRIFT, 19 are historical, 6 are the withdrawal narrative itself. **Three
asserted something FALSE** and were repaired: one cell claimed the sibling row *"STAYS 🔴 OPEN under its
own name"* (it is closed — a status claim about ANOTHER row that nothing re-checks), and two OPEN rows
carried live closing-work instructions to build the forbidden guard. ⚠ **No instrument will ever surface
the other 47**: the anchor guard's RETIRED-ID check matches a status cell opening `RETIRED-ID`, and these
open `WITHDRAWN`. That is correct — the ids were closed, not renamed — so a reader arriving from any
citation lands on a row that redirects them to the skills.

## 0.00000000000000000000 ★★★ CYCLE P18 — AN ANCHOR WITHDRAWN BECAUSE WHAT IT ASKED FOR WOULD HAVE BEEN WRONG

**One row closed, net −1. No code changed.**

`D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED` had been OPEN since 2026-07-29 demanding a guard that every `.sh`
carry a `.ps1` twin. **Operator ruling 2026-08-19:** *"some scripts are posix executed only, and don't have
a .ps1 pair. so we must just enforce the dss cycle and dss code prime skills to always create the pair,
except when the execution is posix only. and this enforcement D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED must not
exist."*

⭐⭐ **THE ROW'S PREMISE IS MEASURABLY FALSE.** It reads an unpaired `.sh` as a missing twin. ✔At closure
**11 of 21 script directories carry no `.ps1`, and every one is correct**: eight are Python — a `.py`
already runs on both hosts, so a twin would be a second implementation of something never split — and two
are POSIX-ONLY BY NATURE (`wsl-leg` runs inside a WSL distro where PowerShell is not the shell;
`profile-compile` drives a POSIX toolchain over a carriage).

★★ **A GATE CANNOT TELL A DELIBERATE POSIX-ONLY SCRIPT FROM A FORGOTTEN TWIN**, so an existence check
would have to be fed an allowlist of eleven exceptions — the convention written twice, in the place least
likely to be read, reddening honest work by default. **The rule is now a judgement the author makes and
writes down**, stated in `dss-cycle/SKILL.md` and in the `dss-code-prime` skill (the conventions
authority): create the twin whenever the capability must reach the Windows leg; omit it — and say so in
the header — when the script is already cross-platform or POSIX-only by nature; and where a pair does
exist, the two must not drift.

ⓘ **The row's second half survives as a rule rather than a gate:** pairing by EXISTENCE is not pairing by
BEHAVIOUR. `scripts_index_guard` covers the sharp edge (a sibling whose `PURPOSE:` contradicts its primary
is refused); the rest is the same-commit discipline the skills state.

## 0.000000000000000000 ★★★ CYCLE P17 — THE REPOSITORY NOW HAS AN INDEX OF ITS OWN SCRIPTS, AND THE GATE STOPPED RUNNING ONE TEST AT A TIME

**Six rows, all born CLOSED, net 0.** Operator-inserted. Two of the six were opened by the INDEPENDENT
AUDITS at the end of the cycle rather than by the work itself — see the audit paragraph below.

**HALF ONE — `tools/` AND `scripts/` BECAME ONE DIRECTORY, WITH AN INDEX.** ✔MEASURED before: 24 tracked
files in `tools/`, 4 script directories in `scripts/`, **408 references across 49 files**, no rule saying
which directory a new script belonged in, and — the actual defect — **no index of any kind**. Eighteen
scripts, named only piecemeal across EIGHT reference files wherever a gate step happened to need one.
★★ That makes the operator's rule unreachable: *"if a tool has a problem, fix before using again, not
workaround an own tool"* cannot be followed if nothing lists the tools. Now: every script lives at
`scripts/<name>/<name>.{sh,ps1,py}` with its siblings and assets inside its own directory; each declares
its purpose ONCE in a `PURPOSE:` line; `scripts/README.md` and `.claude/skills/dss-cycle/references/scripts.md`
are **GENERATED** from those declarations; and `scripts_index_guard` reds when the tree and either index
disagree. ⚠ A hand-written pair would have been two copies of one fact — the failure the whole
deferred-anchor discipline exists to answer — so the index is derived, never authored.

⚠⚠ **THE PART THAT WOULD HAVE FAILED SILENTLY:** **17 repo-root derivations** walked up from a script's
own location and were all one level short after the move. A wrong root does not throw — the guard scans a
smaller tree and reports a clean pass. Each was repaired and each edit asserted to apply exactly once;
✔all six ctest-wired guard twins then re-ran green from their new homes, `check-orphan-tests`'s own 12-arm
self-test included. ★ **Six** `tools/` strings were deliberately NOT rewritten and ALL are named in the row so a
future sweep does not "fix" them (a spec example for a USER's project, a synthetic spawn fixture, an
operator's `.dotnet/tools/pwsh.exe`).

★ **AND THE MOVE FOUND A GUARD THAT ASSERTED NOTHING.** The wsl-invocation rule in
`test_sqlite_harness_legs.cpp` says its coverage is BY DIRECTORY so a new script is governed the day it
lands — and enumerated that directory with a `directory_iterator` that **swallowed its error_code and
checked no count**. Moving the directory would have emptied the range while the test kept passing. It is
now recursive over `scripts/` with a floor, and the retarget WIDENED coverage to four scripts the rule had
never governed (✔none mentions `wsl`, so no new red).

**HALF THREE — THE AUDITS, AND THEY CHANGED THE CYCLE.** Three independent read-only audits ran over the
finished work. They found a HIGH break the migration had caused and hidden (`profile-compile-support.py` built
its gate path from COMPONENTS, so `tools/` appeared nowhere in the file, the sweep could not see it, and
`timed-gate` died on every leg while its diagnostic named a file that was fine); they measured **six** of this
cycle's own numeric claims wrong, including three wearing a ✔MEASURED label; and they got the new
`scripts_index_guard` to report GREEN over six conditions it claimed to refuse. ⚠⚠ **The most important finding
was that the guard's red-on-disable claim in its own row was FALSE** — deleting the floor outright left the
self-test printing *"PROVEN able to fail"*, because the floor arm asserted only an exit code that several
refusals share. The guard was rebuilt: **28 arms**, every red arm asserting the MESSAGE of the refusal it names,
and nine refusals sabotage-tested one at a time.

★★★ **AND THE OPERATOR CORRECTED THE CYCLE'S ANSWER TO ITS OWN WORST FINDING.** Inserting a `# PURPOSE:` line
moved **16** plan citations off their subjects; the rows written to record that then shipped three more wrong
numbers. The plan here was to build a checker that re-resolved citations against `HEAD` — keeping the fragile
reference and detecting its breakage after the fact. The operator's rule instead removes the fragility:
*"we must never document line numbers, we must document method names, comment ids or defined anchors."*
`plan_citations_guard` now refuses a new positional citation, as a ratchet over the **2376** that already exist
(2074 in the registry alone, nearly all in closed historical rows). ✔This cycle burned its own down, **2376 →
2365**, converting all eleven citations it had authored.

**HALF TWO — THE *LOCAL* GATE RAN SERIALLY ON EVERY HOST, BECAUSE NOTHING SUPPLIED A LEVEL.**
⚠ *Local*, and the qualifier is a correction: `.github/workflows/pipeline-pr.yml` (its `ctest --stop-time` step) has passed
`--parallel 4` all along and is untouched by this cycle, so "everywhere" was false. ✔MEASURED:
`local-build.{sh,ps1}` invoked `ctest --output-on-failure` with no `-j`, and `run-gate`'s own usage example
showed the same form, so the form propagated by being copied. The full Windows suite: **899 tests / 2602 s,
one at a time, on a 16C/32T box.** Six example tests: no level **9741 ms**; `CTEST_PARALLEL_LEVEL=8`
**2648 ms**; explicit `-j 8` **2446 ms**; env 8 with an explicit `-j 1` **9669 ms** — so the env var is
honoured *and* a caller's flag still wins, which is what makes this a DEFAULT rather than a policy.
`run-gate` and `local-build` now default it to **8** (operator's number: the Windows and WSL legs run
concurrently here by design, so the default leaves headroom instead of claiming the box). ✔**THE WHOLE-SUITE FIGURE IS MEASURED, NOT EXTRAPOLATED:** through `run-gate` at the new default the
suite ran **900 tests in 803.1 s** against serial **899 / 2602.6 s** — **3.24×**, while a clean WSL build
and three reasoning agents competed for the same box, so it is pessimistic. ⓘ The floor under any
parallelism is `integrated_tests` at **566.56 s** (both figures from this cycle's own P16 serial gate log), a quarter of the serial total in one entry — which is why
the speedup is ~3× and not ~8×.

## 0.0000000000000000 ★★★ READ THIS FIRST — CYCLE P14 (COMPLETE): THE pe64 "MISCOMPILE" DOES NOT EXIST. IT IS AN UPSTREAM SQLITE PORTABILITY BUG, AND THE REASON WE BELIEVED OTHERWISE IS ITS OWN DEFECT.

**✔MEASURED, and it overturns the two cycles above this line.** `D-HARNESS-PE64-CORPUS-WINE-ABORT-SCANSTATUS2`
is **CLOSED / DISCHARGED**: there is no DSS codegen defect at `scanstatus2-5.1`. Do not spend another cycle
hunting one.

**THE ROOT CAUSE.** `testPointerToString` (sqlite `src/test1.c`) registers every handle in a `Tcl_HashTable`
under a key built with **`sprintf(aKey, "ptr:%p", pPtr)`**. `scanstatus2.test`'s test-5.x `trace` proc rebuilds
that handle in Tcl as **`format "ptr:0x%llx" $stmt`**. Those agree only where the C library's `%p` emits
`0x`-prefixed minimal-width lowercase hex — glibc and macOS, not Windows. On Windows the lookup MISSES,
`testStringToPointer` returns 0, and `sqlite3_stmt_scanstatus_v2(NULL, …)` dereferences `[NULL+0x88]`.

**THE FOUR MEASUREMENTS** (each re-runnable; full detail in the registry row):
1. **The discriminating pair, on the SAME crashing binary.** Registry spelling `ptr:00007FFFFF925998` → **rc=0,
   no crash**. Test spelling for the SAME pointer, `ptr:0x7fffff925998` → `wine: Unhandled page fault on read
   access to 0000000000000088 at address 00000001405E0BC2` — address and RIP both identical to the recorded
   fault. One binary, one pointer, two spellings, opposite outcomes.
2. **The `%p` spellings.** DSS pe64 under wine: `ptr:00007FFFFF924B88`. DSS elf64: `ptr:0x386bd348`.
3. **The reference control, on the RIGHT platform.** `x86_64-w64-mingw32-gcc` under wine prints
   `ptr:00007ffffe2ffedc` (default ANSI stdio) and `ptr:00007FFFFE2FFEDC` (`-D__USE_MINGW_ANSI_STDIO=0`) —
   **neither carries `0x`**. Any Windows build of upstream hits this, DSS or not.
4. **The other leg.** The DSS-built **elf64-x86_64** testfixture runs `scanstatus2.test` to **0 errors out of 24
   tests**, 5.1 included — same C, same optimizer, same MIR→LIR.

**⚠⚠ WHY WE BELIEVED IT WAS OURS, AND THIS IS THE PART WORTH CARRYING FORWARD.** The exoneration P14 recorded —
*"the gcc reference … passes 5.1 clean"* — was run against **`reference-testfixture`, a LINUX ELF binary**, to
clear a divergence that exists **only on Windows**. A control built for a different platform than the leg under
test cannot discriminate, and it read as exculpatory. The leg's own same-platform oracle never existed:
`pe64-x86_64/reference-oracle.stderr` says *"the same-platform oracle for leg 'pe64-x86_64' did NOT build
(x86_64-w64-mingw32-gcc exited 1). The leg reports NO ORACLE"*. The harness said so **loudly**; two cycles read
past it. ★ **The rule that falls out: a reference control is not a control until you have checked WHICH TARGET
IT WAS BUILT FOR.**

**⚠ ALSO CORRECTED, so it is not re-quoted from the commits above.** The *"first scanstatus_v2 argument
statically receives a register last written by `mov $0x0,%r12d`"* reading is **FALSE for the shipped binary**.
Aligned (`.pdata`-bounded) disassembly shows arg1 is `mov 0x138(%rsp),%rcx` — a load from its home slot — at all
ten call sites; the store to that slot (`0x1404d0e2d`) **dominates** every one of them (CFG built from the dump,
reachability re-tested with the store's block removed); the `a||b` phi chain writes its carrying register on
every predecessor edge; arg5 correctly occupies the Win64 shadow slot at `[rsp+0x20]`. **That function's codegen
is correct.** The `r15`-as-sixth-argument reading is retracted for the second and last time.

---

## 0.000000000000000 ★★★ CYCLE P16 — THE VERDICT LINE NOW SAYS *WHICH* KIND OF "NO ORACLE" IT MEANS

Two rows closed, **net −1**, and the cheap half of the cycle is the lesson: `
D-HARNESS-FAILING-REFERENCE-ORACLE-COLLAPSES-TO-NO-ORACLE` had been **cited as a closing dependency since 2026-08-18 and never written**. A
`[[wikilink]]` to a non-existent anchor is invisible to every guard here — the anchor-balance instrument counts
ROWS, and a citation is not a row. Worth a sweep.

**THE DEFECT.** `oracle_class_for_leg` keyed the leg's oracle verdict on one fact — did a reference BINARY
survive — so three states with three different next actions printed as one sentence, *"NO ORACLE — no reference
binary was produced or preserved"*: the control **RAN AND FAILED** (its log is on disk, go read it), **no
compiler on this host targets this leg** (an environment limit), and **never attempted**.
⚠⚠ **The fact was never missing — the verdict line threw it away.** `--build-reference-oracle` has always
returned `built`/`build-failed`/`no-reference-compiler` and `attribute_build_failure` has always read it; only
the classifier ignored it. ✔That is the measured cost of a *reporting* defect: **P13 and P14 spent two full
cycles** hunting a DSS miscompile that was upstream, because pe64's verdict claimed it had no oracle when its
oracle had run, failed, and left `reference-oracle.log` on disk.

**THE FIX.** The status vocabulary gets ONE owner (`ORACLE_STATUSES` / `ORACLE_STATUSES_ATTEMPTED`, read by both
the classifier and `attribute_build_failure`, so a second literal list cannot drift); `oracle_class_for_leg`
takes the status and returns `build-failed` / `no-reference-compiler` as classes with their own prose; both
drivers pass `--oracle-status` to `--oracle-report`. ★ It was ALREADY passed to `--attribute-build` — which is
exactly why the bare option name was **not** a usable pin, and why the new assertions match the whole argument as
each driver spells it. ★★ Safety property pinned: **no status can conjure a control out of a missing binary** (a
`built` status with nothing on disk is still `absent`). An unrecognised status **RAISES** rather than degrading
to the pessimistic class.

**AND THE WINDIRENT ROW CLOSED WITH IT**, because its central claim is measured false: *"cannot be compiled by
ANY GNU-on-Windows compiler, DSS included"* — ✔DSS compiles that TU clean, and ✔`cl` compiles it rc=0. Two
references and DSS build it; only mingw-on-Windows does not, and upstream ships Windows via `Makefile.msc`. Under
bar §A.3b that is a NON-DSS CONFOUND, now rendered honestly as `build-failed` rather than "no control existed".

**GATE:** harness self-test **1962 → 1975, 0 failed**; `test-confound-scope.sh` **152/0**;
`test-confound-scope.ps1` **111/0** (both files' own assertion-accounting guards caught the additions — 150→152,
109→111, SKIP 48→49); **red-on-disable PROVEN fail-closed** (mutant differed by hash, lacked the witness, still
parsed, verified on disk before the reader ran, went RED, and the restore was verified byte-identical by sha256).

**NEXT — OPERATOR-SCHEDULED (2026-08-19), in this order:**
1. **The remaining residue above**: `D-HARNESS-FAILING-REFERENCE-ORACLE-COLLAPSES-TO-NO-ORACLE` — write the row
   (or retire the citation) and thread the oracle status into `oracle_class_for_leg` so *"the control ran and
   failed"* stops rendering as *"there is no control"*. Small, self-contained, and it is the last live piece of
   the pe64-attribution story.
2. **Operator conversation, then a cycle: grab the important anchors from the AP5/AP6 commit + the asm feature
   (this PR) pending deferrals.** Operator's stated ORDER: **pending feature completion → production errors →
   harness/test errors → others.**
3. **Operator decision still owed: report upstream to sqlite?** (a) the `%p` vs `format "ptr:0x%llx"` handle
   mismatch (`test/test1.c` + `test/scanstatus2.test`); (b) `fileio.c`/`windirent.h`'s `_MSC_VER` interlock.
   Both outward-facing, so neither was filed.
4. Then the standing loop: TLS-dylib corpus arm (P12 residual); asm remainder; C1 diagnostic coordinates
   (`D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`, HIGH).

⚠ **Housekeeping seen but NOT done this cycle** (no anchor — it is the already-open
`D-BUILD-LAYOUT-FLAT-ROOT-BUILD-DIRS-NOT-MIGRATED`'s neighbourhood): ✔MEASURED, **five** stale lane worktrees
survive — `.claude/worktrees/{agent-a33bf76aa1baf2e04, agent-aa5ee5ab409b3c519, dss-no-repo-ancestry-cwd}` plus the
SIBLING-DIRECTORY ones `C:/Source/DailySoftware/{dss-lane-m, dss-wt-bitwise, dss-wt-movzw}` (detached HEADs).
⚠ `agent-aa5ee5ab409b3c519` is on disk with its own `build/` but is **absent from `git worktree list`** — an
orphaned copy, which is the worst of the set: it answers repo-wide greps and nothing tracks it. Both
`.claude/worktrees` copies were hit while searching this cycle.
---

## 0.00000000000 ★★★ READ THIS FIRST — CYCLE P13: THE CROSS-HOST MATRIX RAN — AND FOUND A MISCOMPILE

**The queue re-derivation paid for itself again**: the C7 "provider conversions" the handoff queued
were ALREADY DONE — the pinned-archive-everywhere conversion landed 2026-08-10 in `3e86a187`
(PR #48) and three rows sat stale-open for 9 days (`
D-HARNESS-UBUNTU-PORTS-PROVIDER-NOT-GENERALISED-TO-PINNED-ARCHIVE`, `D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER` —
both closed this cycle; the "PS1 dispatch arm" sub-row never existed under the name I queued).
What remained was the ROW'S OWN STANDARD: *"the closing test is per-cell and by EXECUTION"*.

**The matrix, run by execution on four hosts** (sqlite harness, full driver per host):
- **Windows** × {elf64-arm64, elf64-x86_64}: built (after the discovery hazard below).
- **WSL** × {elf64-x86_64, pe64-x86_64, macho64-arm64, macho64-x86_64}: all built; elf64-x86_64 +
  pe64 corpus-VERIFIED (2 verified, 0 build-input-missing).
- **arm64 VPS** × elf64-arm64: built + corpus VERIFIED natively.
- **macOS** × {elf64-x86_64, pe64-x86_64}: built (runs structurally skipped by runOn, as designed).
⇒ **`D-HARNESS-CROSS-HOST-ANY-TARGET`'s BUILD half: every declared leg built on every host, by
execution.** (The row's close records the RUN matrix's documented restrictions.)

**⚠ THE DISCOVERY HAZARD, measured:** the first Windows attempt "failed" with pre-P10 loader
errors — the driver's compiler discovery (newest RELEASE tree, mtime-wins, build-type read from
each tree's CMakeCache) picked `build/rel`, a 2026-08-18 21:01 pre-grammar binary; the repo's
only current tree was Debug (excluded by design). **Not a harness defect** — the policy did
exactly what it documents; the fix was building a current `build/rel`. A stale Release tree that
SILENTLY answers discovery is now a standing trap: `cmake --build build/rel` after any
harness-relevant change, or clear the stale trees.

**★★★ THE FINDING — `D-HARNESS-PE64-CORPUS-WINE-ABORT-SCANSTATUS2` (OPEN, HIGH, silent-miscompile class), operator-directed root cause, three facts pinned:**
1. **DETERMINISTIC**: standalone `scanstatus2.test` crashes at `scanstatus2-5.1`, same RIP, 30/30.
2. **WINE EXONERATED**: the same WSL-built testfixture.exe on REAL Windows exits 0xC0000005 at
   the same test — no wine in the path.
3. **THE INSTRUCTION**: the callee (RVA [0x5E0B0F,0x5E1952)) homes SIX arguments — rcx/rdx/r8/r9,
   stack arg5 — **and reads its SIXTH from `r15`, a callee-saved register no x86-64 ABI passes
   arguments in**, then derefs `[r15+0x88]`. The caller (the 5,534-byte Tcl command dispatcher
   [0x4D098E,0x4D1F2C)) passes five args and holds r15 as an int counter (=0). Shape: a
   caller/callee ARGUMENT-CONVENTION disagreement (K&R prototype-vs-definition arg-count mismatch
   is the classic source shape) or a >4-arg pe64 codegen bug.
⚠ The pe64 corpus has passed on WINDOWS-built fixtures historically — the reproducer must decide
whether the trigger keys on the WSL cross-build or a path the Windows build didn't take.

**NEXT — P14, nothing above it:** root-cause and FIX
`D-HARNESS-PE64-CORPUS-WINE-ABORT-SCANSTATUS2`. Start from the pinned RVAs: identify the TU
(transpile the staged `tclsqlite.c`-family TUs, match the caller's `mov %esi,%eax` + 5-arg call
shape), build the minimal reproducer (a 6-arg call through a mismatched prototype), fix in the
shared calling-convention/prologue code — this is a FIX, no gate applies — then rerun the pe64
corpus leg to green. Then the loop continues: the TLS-dylib corpus arm (P12's stated residual),
asm remainder, C1 coordinates.

---

## 0.0000000000 ★★★ READ THIS FIRST — CYCLE P12 (COMPLETE): THE MAC-AWAKE WINDOW — FOUR ROWS CLOSED ON REAL HARDWARE

The operator turned the Mac on (*"mac is on. you can try it"*), which made the availability-gated
window the pick. **Every closure below was MEASURED on the Mac (arm64, macOS 26.5.2) before
anything was declared**, and the window's queue was RE-DERIVED at pick time — half of it had
CLOSED UNDERNEATH the list (C6 done TF-C109; C7's named blockers done TF-C117/C121/C124):

1. **`D-LK-MACHO-X8664-DYLIB-RUNTIME` ✅** — the Rosetta run leg: DSS-built x86_64 `.dylib` +
   client, `arch -x86_64` → **exit 42**. Detail recorded in the row: macOS strips `DYLD_*` env
   for these processes (measured), so the client links with the carriage's documented import-name
   override — the clang equivalent of linking by path.
2. **`D-CODEGEN-MACHO-ARM64-X29-ALLOCATED-WITH-NO-FRAME-RECORD` ✅ DISCHARGED** — the crash
   reporter walks a DSS arm64 image's mid-function fault **fully symbolicated**
   (`leaf←middle←outer←main`), identical to the clang control. The ABI inference was refuted on
   the exact surface the row named; x29 stays allocatable, no config change (a register saved
   from a non-defect).
3. **`D-LK1-MACHO-X8664-DARWIN-DATA-SECTION` ✅** — its config half shipped 2026-08-05 (the row
   was STALE — the exact class `D-FORMAT-MACHO-X86_64-EXEC-DECLARES-NO-DATA-SECTIONS` warned
   about); the Rosetta run leg landed now: rodata/data/bss TU → **exit 42**.
4. **`D-LK3-DYLIB-TLS-MODEL` ✅** — witnessed → declared → run end-to-end: (a) the clang probe
   measured the row's exact unknown (a thread alive BEFORE a dlopen sees a FRESH, INITIALIZED
   TLV instance); (b) the arm64 dylib format opts in (three `__thread_*` rows from the exec
   sibling + `tlsAccess` + `supportedDataSections`; x86_64 sibling gets its absent-rationale);
   (c) the walker belt removed — its own trigger ("until a Mac witness exists") fired; (d) the
   rejection pin replaced by `ThreadLocalDylibLinksAndCarriesTLV` (strict: 48 B of descriptors,
   `__tlv_bootstrap` in the bind stream); (e) a DSS `_Thread_local` dylib + client, run NATIVELY
   → **exit 42**.

Gates: Windows 898/898 · Mac full ctest green (both on the final tree) · WSL + arm64 VPS legs
(this commit). Anchor delta: **closed 4, opened 0, net −4**. Independent code audit: CLEAN on
correctness; its MEDIUM finding (no in-suite corpus arm builds a `_Thread_local` dylib — the
two-file client shape is needed) is recorded as the STATED RESIDUAL in the D-LK3 row and queued
below; its LOW stale-citation finding folded same-cycle; MH_HAS_TLV_DESCRIPTORS covered
structurally at the shared flag site (both polarities pinned exec-side).

⚠ **The Mac window is STILL OPEN and the queue for it is NOT empty** — remaining Mac-gated work,
next window: the C7 provider conversions (`
D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER` PARTIALLY CLOSED: generalize `ubuntu-ports` onto pinned-archive + declare routes for
`elf64-x86_64`/`pe64-x86_64` + the `.ps1` dispatch arm for elf64-arm64 — the four red BUILD-matrix
cells) — then validate the macOS×{elf64-x86_64, pe64-x86_64} cells ON the Mac. `
D-LK3-DYLIB-WEAK-EXPORT`'s validation also wants the Mac (implementation is format-side).

**NEXT — the loop continues:** (1) the C7 provider conversions above while the Mac is up;
(1b) the TLS-dylib CORPUS ARM (audit residual): a two-file `_Thread_local` dylib + client
example/integrated arm so a `lib`-profile regression reds in-tree (`test_ffi_resolve_library`'s
two-TU pattern);
(2) if the window closes: asm remainder (`D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER` label
half — the asm-goto BLOCK binding design; `D-ASM-TEMPLATE-IS-LEXED-TWICE` LOW;
`D-ASM-ARM64-GAS-SURFACE-INCOMPLETE` — re-measure, it predates the CFI producer landing);
(3) C1 diagnostic coordinates (`D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`, HIGH);
(4) §B pairs stay decisions (`D-ASM-RIP-RELATIVE…` + `D-ASM-ADDRESS-OPERAND…`;
`D-MIR-SEH-FILTER-CLONER` SUSPECT — verify or discharge). sqlite matrix re-run still queued for
the next codegen-changing cycle (P12 changes the DYLIB code path — a `lib`-profile corpus leg
would re-run it; the CLI/units corpus is unaffected).

---

## 0.000000000 ★★★ READ THIS FIRST — CYCLE P11 (COMPLETE) + THE STANDING BACKLOG LOOP

**The loop mandate (operator, 2026-08-19, verbatim):** *"/loop the /dss-cycle until all opened are
fixed. all closed by the best long term implementation with no workarounds."* The queue is the
operator's prioritized backlog (asm+miscompiles+errors → ap5/ap6 → tools → the rest), **verified
against the registry before P11 picked** — and that verification changed the top of the queue:

- **The operator's A1 and B1 cycles were already GONE**: `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`
  ✅ 2026-08-17, `D-DEPS-DEPENDENCY-CANNOT-DECLINE-A-TARGET` ✅ 2026-08-15 (operator decision),
  plus 4 more closed rows named in the backlog (P0016-detail, CRLF-divergence, PKG-config-tree,
  BUILD-LAYOUT) — all confirmed by measurement, not recalled.
- ⚠ **One correction to the answer I gave the operator:** I recommended
  `D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD` as the new top priority reading it OPEN HIGH — it has been
  **✅ CLOSED since 2026-06-03**; its closure record sat in the WRONG TABLE CELL (see below), which
  is why the balance gate and every list derived from it counted it open. Measured before P11
  picked; the recommendation is withdrawn.

**P11's deliverable — `D-ASM-TEMPLATE-DIAGNOSTIC-DOES-NOT-NAME-THE-C-STATEMENT` ✅ CLOSED:**
an inline-asm template diagnostic now names the **C statement as its PRIMARY locus** with the
template locus demoted to a related note. ✔Both references measured first (the row demanded it):
clang 18 (`gt.c:3:13: error: …` + `<inline asm>:1:2: note: instantiated into assembly here`) and
gcc 13.3 (`gt.c:3: Error: …`) **both lead with the C statement, neither has template-primary** —
so DSS's old template-primary shape was itself the divergence. Mechanism: `MirAsmDescriptor`
carries the statement locus (populated at HIR→MIR from the optional source map; sourceMap-less
callers degrade to the old render, nothing fabricated); `DiagnosticReporter::reanchorFrom`
(the `remapBuffers` post-admission precedent; demotes iff `buffer.valid()`) applied by an RAII
guard over the whole `expandInlineAsm` body and around `lowerInlineAsmGoto`'s refusal, so no
refusal site (or future one) can miss it; the text-only wrapper refusals gain the C primary
(they had no locus at all). Witness: `AsmTemplateDiagnosticNamesTheEmbeddingCStatement` — both
arms, exact rendered strings, red-on-disable demonstrated (guard deleted → new test RED, parent
test GREEN). Design audit (independent): 3 CONCUR + 1 objection adopted (demote discriminator =
buffer validity, not span emptiness — a zero-width span at a real buffer is a real locus).

**The registry sweep that fell out — `D-PLANS-REGISTRY-CLOSURE-MARK-IN-WRONG-CELL` ✅ born-closed:**
a closure record can sit in the Closing-work cell while the status cell (the one the balance gate
reads) carries only the description — the row then counts OPEN forever. Full-registry pass found
**8 rows with a cell-3 ✅ lead: 2 genuinely closed-but-miscounted, both repaired**
(`D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD` 2026-06-03, `D-LK10-ENTRY-RESOLVE-ENTRY-FN-IDX` 2026-06-02);
2 genuinely OPEN rows whose cell-3 ✅ is a stale over-claimed closure (left as live work, cell 2
authoritative); 4 🟢 design-record rows that stay counted OPEN by the gate's only-✅-closes
contract (reclassifying them is an operator semantics decision — **flagged in the P11 report**).

**Anchor delta: closed 3, opened 0, net −3** (balance gate OK; 707 → 704 registry-side).
Gates: Win 898/898 baseline + full battery, WSL + arm64 VPS legs (this commit).

**NEXT — the loop continues, this order (operator bands):**
1. **asm remainder**: `D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER` (narrowed to the `%l` label
   half — real design work: a binding that names a BLOCK, the asm-goto refusal names the blocker)
   · `D-ASM-TEMPLATE-IS-LEXED-TWICE` (LOW; ownership precondition ✅ landed; its consumer needs
   designing) · `D-ASM-ARM64-GAS-SURFACE-INCOMPLETE` (vocabulary rows; check trigger) ·
   `D-CSUBSET-ASM-LABEL-ON-SYNTHESIZED-SHIM-SYMBOL` (LOW).
2. **the Mac-awake window, C5+C6+C7 TOGETHER** (all gated on Mac availability):
   `D-LK-MACHO-X8664-DYLIB-RUNTIME` · `D-LK3-DYLIB-TLS-MODEL` · `D-LK3-DYLIB-WEAK-EXPORT` ·
   `D-HARNESS-CROSS-HOST-ANY-TARGET` (macho leg inputs) · `D-TEST-MACOS-HOST-SPAWNS-FOREIGN-BINARY`
   (over-claimed close, genuinely open) · `D-CODEGEN-MACHO-ARM64-X29` (SUSPECT — verify or
   discharge). The Mac is usually OFF — never wake it; run the window when the operator has it on.
3. **production errors**: C1 diagnostic coordinates
   (`D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`, HIGH, unconditional) → symbol tables ×5 →
   dynamic linking ×2 → arch identity ×2 → diagnostic quality ×2 → `D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD`.
4. **§B-gated, bring as decisions, do not build**: `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` +
   `D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL` (the §5.4 operator-decision pair) ·
   `D-MIR-SEH-FILTER-CLONER` (SUSPECT — verify or discharge, don't patch on suspicion).
5. **Still queued behind the loop**: the full sqlite matrix re-run (operator: *"we'll re run
   everything once our compile is fast enough"* — it is; P11 changes diagnostics only, not
   codegen), then FC18.

---

## 0.00000000 ★★★ READ THIS FIRST — CYCLE P10 (COMPLETE): FIRST-CLASS CONFIG-DRIVEN LTO

Operator mandate, verbatim: *"address it properly, long term solution, no workarounds, 100% config
driven. first class LTO implementation"* (`D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE`, ✅ CLOSED — the row
carries the full measurement table and the two §B rulings).

**Landed in two operator-steered steps, one behavior change each:**
1. **`2557a717` pre-step** — the P9 self-audit's hardening pins (production candidate-builder value
   pin with a mutant-proven red arm; front-half concurrency witness; `.sh`/`.ps1` case parity).
2. **`a0370fde` step 1 — the schedule GRAMMAR**: `passes` became a closed tree (leaf / `repeat` /
   `fixpoint`), the engine its interpreter, flat docs desugar at load; budgets fail loud at load.
   ✔Byte-identical to P9 (`1d117b1a…`), compile 41.7s.
3. **This commit, step 2 — the TOPOLOGY**: `PipelineStage` slots in the driver (Unit at
   `buildCuMir`; Program at every final-module site — merged, N==1, archive member), the document
   owns what runs (`unitPipeline` key; absent = both stages same doc, what `debug` ships).
   `resolveCompileConfigFromPipelineName` + the examples runner's real-channel switch.

**The shipped split (B′, decided by the operator-amended rule — direct measures, no proxies):**
`release-unit` = `[ConstFold, Mem2Reg, SimplifyCfg, Dce, Inlining]×2` per CU, full release at the
program stage. Against the incumbent: **exe −5.6%** (6,904,848 vs 7,314,848), **compile time
neutral** (38.4s median vs 38.7s), **runtime FASTER** (0.453s vs 0.470s median, 7 repeats —
`scripts/sqlite-runtime-bench/sqlite-runtime-bench.py`, the standing runtime-differential instrument). The control arm
(unit key removed) measured **byte-identical to pre-P10**, proving the restructure behavior-neutral.
⚠ The FIRST rule (inline counts + exe-vs-A′) picked A′ and its cheapness premise was refuted
(+60% compile); the operator amended the rule toward direct measures BEFORE the runtime column was
measured — recorded in the row as the pre-registration discipline working.

**§B rulings this cycle:** (a) strip-order — unit stage CONTAINS `Inlining` (operator; per-TU -O2
shape); (b) the A/B rule amendment itself. Both recorded in the D-OPT7 row.

**⚠ OWED NEXT CYCLE (nothing blocks, all small):** the stage trace line (`opt: stage=…`) is
parsed by agg-trace (verified: 103 unit / 1 program over the B′ log) but no TEST pins its format;
the audit's nodePath label fix landed same-cycle. Debug-config column measured and clean: the kit
compiles in **24.8s** at `--config=debug` (the new N==1/archive program-stage calls run
Identity+prune+strip — no give-back).


---

## 0.0000000 ★★★ READ THIS FIRST — CYCLE P9 (COMPLETE): COMPILE TIME — PROFILED ON FOUR LEGS, FOUR MEASURED CAUSES FIXED, OUTPUT BYTE-IDENTICAL

Operator instruction that opened the cycle, verbatim: *"now we need a comprehensive profiling
across the WHOLE pipeline compiler and with ALL available legs (windows, linux arm and x86, macos).
The compiled object will be the sqlite cli. We'll use D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX
anchor but the goal here is to make a first class compiling time (and, of course, the code must be
correct!)"*. Later, verbatim: *"even 2 minutes is bad... gcc and msvc timing on sqlite cli would be
less then a minute... and if rederiveStructCfMarkers is slow, lets fix it"*.

### ✅ WHAT LANDED — every cause below was measured first, then fixed, then pinned

1. **`rederiveStructCfMarkers` was O(functions × module blocks).** Fixed three ways, one per row:
   the module self-loop index is computed ONCE per module; the back-edge sweep is scoped to the
   function's own candidate range (`mirNaturalLoops(…, candidateSources)`, fail-loud ascending
   contract, completeness = order ∪ {module self-loops}); and the post-dominator scratch
   (`MirPostDomScratch`) reuses the eight whole-module buffers via two self-recorded touched lists,
   one `computePostDomInto` core keeping fresh and scratch byte-identical BY CONSTRUCTION.
   ✔29.0 s → **1.2 s** across the 320 whole-module calls of one sqlite release compile
   (5.3–6.6 s → 0.19–0.22 s per call). Rows: [[D-OPT-POSTDOM-SCRATCH-REUSE]] ✅ (the gated trigger
   FIRED at 28.3 s vs its ~10 s bar), [[D-OPT-NATURAL-LOOPS-MODULE-WIDE-SCAN]] ✅ born-closed,
   [[D-MIR-STRUCTCF-UNREACHABLE-BLOCK-CLAIMED]] ✅ born-closed (spec text reconciled to pinned
   behaviour), and [[D-MIR-STRUCTCF-DERIVATION-REACHES-PAST-THE-FUNCTION]] 🟠 OPEN — the derivation's
   cross-function reach is CANON, pinned, and narrowing it is a deliberate future decision, not a
   tidy-up.
2. **Every pass reserved a module-sized rewrite map per function.** Per-function `reserveHint`;
   the natural experiment that sized it: ConstFold ~500 ms vs 3,100–3,900 ms for the
   per-function-rebuilding passes. [[D-OPT-REBUILD-REWRITE-MAP-RESERVES-THE-WHOLE-MODULE]] ✅.
3. **The front half was strictly serial and the `--time` report lied about it.** `buildCus` now
   runs the pool with index-order slot collection (determinism from CU index, never completion
   order), a `std::latch` join, fail-loud on a disengaged slot, n≤1 inline; `kMaxAutoWorkers`
   16→32 with RSS measured at both (5.0 GB @16, 8.6 GB @32 — the raise is a memory decision, on
   the record). Phase timing is TWO-CLOCK (`cpuNanoseconds` Σ-thread vs `wallNanoseconds` interval
   union, `peakConcurrency`, `liveScopeCount`) and an invariant violation FAILS the run — the old
   `[other] 0ms` row was a negative remainder clamped to zero. `std::localtime` →
   `localtime_s`/`localtime_r` (thread safety under the now-parallel front half).
   [[D-PERF-4-CU-PARALLELISM]] carries the addendum.
4. **The harness timed a Debug compiler against Release twins.** `build-and-test.ps1` now READS
   the compiler's build type from its tree's `CMakeCache.txt`, refuses non-Release under
   `SKIP_DSS_BUILD=1`, offers a three-state `DSS_ALLOW_NONRELEASE_COMPILER` (typos refused) that
   opts out of the refusal, never the statement. [[D-HARNESS-PS1-TIMES-A-DEBUG-COMPILER-WHILE-THE-SH-TWIN-TIMES-A-RELEASE-ONE]]
   ✅ born-closed, its selector probed by AST-extracting the real function text.
   `scripts/profile-compile/profile-compile*.sh` + `-support.py` ship as the cross-leg profiling kit — ONE script,
   no `.ps1` twin, BY DECISION (a profiler whose value is "the host is the only variable" must
   not have a per-host implementation). ⚠ Two Windows-leg defects in the kit were found and fixed
   by RUNNING it: `os.path.join` backslashes eaten by bash, and bare-name `bash` resolved by
   CreateProcess to an extensionless PATH shadow that `shutil.which` never saw — the gate now
   passes the absolute shell and forward-slash paths.

### ✅ ACCEPTANCE — "the code must be correct!" ✔ALL GREEN, witness on disk

- **Byte-identity vs the PRE-P9 compiler**: pristine `d62405ba` worktree, Release, built this cycle
  (`C:\Source\DailySoftware\dss-p9-baseline`, deleted after) vs the P9 merged tree — same kit, same
  target, **image trees both sha256 `1d117b1aafce2e2c1f52b789fdf49f71bb3fe97cab3f4a39a1d2f46bb4f3a813`**
  (path-keyed hash over every emitted file; the sqlite3 binary itself `932cbdaf…` on both).
- **≥20× repeat determinism**: **20/20** parallel repeats of the P9 compiler, every image tree
  byte-identical (`ACCEPT-OK … repeats=20`, captured at `build/perf/accept-p9-result.log`).
- **Cross-host byte-identity**: the WSL-built P9 compiler emits the SAME `932cbdaf…` sqlite3 binary
  for the same kit+target — the determinism property holds across hosts, not just runs.
- **Post-fix timing (the point of the cycle)**, 103-TU sqlite CLI kit, `--config=release`:
  **Windows median 40.7 s** (20 repeats, 38.6–45.4 s) vs pre-fix 3m32.1 s; **WSL 33.6 s** vs
  pre-fix 1m40.8 s. The Windows-vs-WSL residual collapsed **2.1× → 1.2×** — the quadratic and
  allocation-churn work was exactly what Windows' allocator amplified. gcc yardstick stays
  21.5 s `-j1` / 4.8 s `-j32` on WSL.
- **sqlite re-probe**: the emitted CLI, FILE db, release config — CRUD `sum(x)=42` ✔.
- **3-leg gate**: Windows **898/898** · WSL **898/898** · arm64 VPS **898/898** — all three
  through the run-gate witness (`'100% tests passed'`).

### ✅ THE PRE-FIX PROFILE — settled measurements, re-quotable as the BEFORE record

**★★★ THE ANCHOR'S HEADLINE FIGURE WAS AN ARTEFACT OF THE HARNESS, NOT A PROPERTY OF WINDOWS.**
`D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX` compared a **Debug** compiler against a
**Release** one. ✔MEASURED: `build/real-examples/win-probe.log:64` shows the Windows run used
`build\dbg\bin\dss\dss-code-prime.exe`; that tree is `CMAKE_BUILD_TYPE=Debug` with `-g`, and its
`build.ninja` contains **zero** `-O` flags and **zero** `NDEBUG`. Meanwhile `build-and-test.sh:2767`
*unconditionally* builds `build/rel` with `-DCMAKE_BUILD_TYPE=Release`, while `build-and-test.ps1:2721`
takes the **newest existing** binary from five roots regardless of build type — and on a developer box
`build/dbg` is always newest. ⇒ anchored + fixed as
[[D-HARNESS-PS1-TIMES-A-DEBUG-COMPILER-WHILE-THE-SH-TWIN-TIMES-A-RELEASE-ONE]].
⚠ The trap that hid it: the harness's `Config` parameter says `release` and means **the artifact's
optimizer pipeline**; the **compiler binary's own build type** was never stated anywhere. Both read as
"release" in the log and only one was ever controlled.

**✅ THE FOUR-LEG PROFILE.** Identical sources (SQLite CLI, 103 TUs, copied — NOT re-staged, because
the harness pulls upstream every run and a re-staged subject is a different subject), identical target
`x86_64:elf64-x86_64-linux-exec`, Release compiler built on each host, each host reading **its own**
config tree:

| host | cores | wall | optimize | front half | semantic | everything else |
|---|---|---|---|---|---|---|
| WSL x86_64 | 32 | **1m40.8s** | 1m03.6s | 29.6s | 16.7s | 37s |
| arm64 VPS | 4 | **3m18.1s** | 1m50.7s | 66.8s | 12.2s | 87s |
| Windows x86_64 | 32 | **3m32.1s** | 2m24.2s | 41.3s | 21.6s | 68s |
| macOS arm64 | 10 | **5m10.0s** | 4m42.9s | 27.4s | 5.2s | **27s** |

★★ **READ THE macOS ROW BEFORE CONCLUDING ANYTHING ABOUT HOSTS.** It is the SLOWEST overall and has
the FASTEST front end of the four — its semantic phase is **5.2 s against Windows' 21.6 s**. The whole
of its deficit is `optimize`. Across the four hosts `optimize` varies **4.4×** while every other phase
varies under 3×. That is the signature of allocator behaviour under high churn, not of computation —
and the churn has a named source (below).
⚠ Windows-vs-WSL on the SAME hardware is **2.1×**, real but a fraction of the anchor's 8×.

**✅ THE YARDSTICK.** ✔MEASURED on WSL, the SAME 103 TUs, gcc 13 `-O2`, on a **monotonic** clock:
**`-j1` 21.5 s · `-j32` 4.8 s** — against DSS's 1m40.8 s on that host. So DSS is **4.7× serial gcc**
and **21× parallel gcc**. ⚠ The first attempt at this number used `date +%s%N` (CLOCK_REALTIME) and
reported a compile that took **minus 11.7 seconds** — see [[project_wsl2_clock_realtime_broken_2026_08_01]].
The compiler's own `--time` uses `steady_clock` and was never affected.

**✅ WHERE THE TIME GOES — four independent causes, each measured, none of them host-specific:**
1. **The merged whole-program optimize is 107.7 s of passes + 6.3 s of marker rederivation, on ONE
   thread, while 31 cores idle.** ✔Process sampling showed a ~2-minute stretch at exactly 1.0
   CPU-second per wall-second with `threads=1`. Four iterations, **never converged**, and the cost per
   iteration is FLAT — 25.6 / 26.3 / 27.6 / 28.2 s. It gets *slower*, not cheaper.
2. **Every pass clones the whole module whether or not it changes anything.** Every one of the nine
   passes is `for (i<nf) { policy.analyze(f); MirFunctionRebuilder rb{mir,builder,policy};
   rb.rebuildFunction(f); }` — ~3,900 functions rebuilt per pass per iteration. `Cse rebuild=9.94s vs
   analyze=0.73s`; `Licm rebuild=9.68s`. ★ Trace lines include `pass=Mem2Reg done 3454ms mutated=0` and
   `pass=Licm done 6129ms mutated=0` — **19.0 s of the 107.7 s is spent by passes that provably changed
   nothing.** This is the allocation churn that explains the 4.4× host spread.
3. **`rederiveStructCfMarkers` cost 28.3 s over 319 calls** — more than every per-CU pass combined.
   ✔MECHANISM FOUND BY READING: `deriveStructCfMarkers` (`mir_struct_markers.cpp:44`) opens with
   `std::vector<StructCfMarker> out(mir.blockCount(), ...)` — `Mir::blockCount()` is the **WHOLE
   MODULE's** block count — and the whole-module driver calls it **once per function**, while
   `applyDerived` reads back only that function's slots. O(functions × module blocks): quadratic.
4. **The front half is strictly serial.** preprocess → splice → tokenize → expand → parse →
   resolve-imports runs one TU at a time at `program.cpp:3206`; sampling showed 1.0× throughout. And
   `resolveCuPoolWidth` (`program.cpp:358-371`) caps the back half at `kMaxAutoWorkers = 16`, so a
   32-core host uses half the machine — though observed back-half concurrency was only ~3.7×, well
   under even that cap, so worker count is **not** the whole story there and the bump must not be
   claimed as its fix.

**✅ THE `--time` REPORT WAS LYING, AND IT HID EXACTLY THIS WORK.** Its `[other]` row printed **`0ms`
on all four legs** — not "nothing unaccounted" but `program.cpp:2374-2376` **clamping a negative
remainder to zero**. `PhaseTimers` sums self-time across every thread with relaxed atomics while the
back half runs 16 workers, so attributed time routinely exceeds process wall time. The header still
asserted the opposite (`phase_timers.hpp:29-31`: *"The pipeline itself is synchronous"*), stale since
`D-PERF-4`. ★ The per-pass numbers above had to be reconstructed from an env-gated `fprintf` that
prints and discards — duplicated in **five** files.

### ⚙️ THE LANES' FOLD — where the four lanes' work lives now

All four lanes merged into the working tree and were reviewed; their build trees
(`build/lane-{markers,opt,driver,harness}{,-rel}`) are CLEARED once the gate is green. The
merged tree was accepted as ONE subject: the byte-identity + repeat acceptance below was run
against the merged compiler, not per-lane binaries. ⚠ Two unattributable byte-identical sqlite
binaries found in `build/perf/{lane-before,scratch}` during fold review were NOT used as
acceptance evidence — the baseline below is a pristine `d62405ba` worktree built this cycle.

### ⚖️ NEXT CYCLE'S MANDATE — `D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE` (operator-promoted)

I recommended leaving it (deleting the redundant per-CU optimize buys ~5-10 s of a 213 s build and can
move codegen). **The operator overrode that**, verbatim: *"address it properly, long term solution, no
workarounds, 100% config driven. first class LTO implementation"*.
★★★ **MY SIZING MEASURED THE WRONG THING** — it priced *deleting the second call*, when the
deliverable is *the topology*. Today both call sites resolve **the same pipeline document**, so
`release.pipeline.json`'s 9 passes × 4 iterations run over every CU and then again over the merged
program; real toolchains run a per-TU pipeline and a link-time pipeline that are DIFFERENT. The
topology is hardcoded in the driver, and that — not the duplicated work — is the defect.
⚠ **THAT ROW'S OWN PREMISE IS UNDER CHALLENGE**: it says the per-CU pass *"changes no output"*, but
`Inlining`'s legality gate is a hard size cutoff (`inlineThreshold`, 50 MIR instructions), so per-CU
`ConstFold`/`Dce`/`Mem2Reg` can shrink a callee **under** that threshold before the merged pass sees
it. Default stage contents are therefore a MEASUREMENT, not an analogy. Full detail is in the row.

---

## 0.000000 ★★★ READ THIS FIRST — CYCLE P8: PATH IDENTITY IS NOW A TYPE, AND THE REBASE IS DONE

**✅ THE REBASE IS COMPLETE.** ✔MEASURED: HEAD `41a320ad` (Cycle P7) sits on `origin/main`'s
`fe031376` (AP6 #53); `backup..HEAD` = **555 files**, exactly AP6's own count, and the ONLY file
changed that AP6 never touched is `src/asm/asm_template_to_lir.cpp` (a deliberate port of AP6's
`CfClass::Switch` refusal into our relocated dispatch). Backups: `backup/pre-rebase-p7-8ecb8e8d`,
`backup/pre-rebase-main-0f47896f`, `backup/c23-burndown-3-pre-rebase-2026-08-17`.
⛔ **NOT PUSHED — operator instruction, verbatim: *"when it's time to commit, do it, but don't
push."*** That instruction is still live.

### ⚖️ THE sqlite MATRIX — CANCELLED MID-FLIGHT BY OPERATOR INSTRUCTION. WHAT IS AND IS NOT MEASURED

Operator, 2026-08-18: *"ok. let's cancel this, commit + push, we'll re run everything
once our compile is fast enough"*. ⚠ **The BUILD half is a real result and the RUN
half is mostly UNMEASURED. Do not read the build numbers as a matrix verdict.**

✅ **BUILD — measured, and it is the strongest statement this run supports:**
**three hosts each built ALL FIVE target legs** (`elf64-x86_64`, `elf64-arm64`,
`pe64-x86_64`, `macho64-arm64`, `macho64-x86_64`) from the full upstream sqlite
source set. Windows 5/5 · WSL 5/5 · arm64 VPS 5/5. ★ That is the
build-ANY-target-on-ANY-host requirement holding on three hosts at once.
⚠ **macOS never reached step 7** — it was at 6/9 (per-leg tcl/zlib resolution) when
cancelled, so it built NOTHING this run and its 5/5 is **not** claimed.

✅ **RUN — what actually executed, all of it from the WSL host:**
- `elf64-x86_64` corpus **GREEN** — 4 errors / **394,693** tests, all 4 known
  non-DSS confounds (`sessionnoact-4.3`, `walsetlk-2.1.6`, `walsetlk-2.2.14`,
  `zipfile-25.0`).
- `elf64-arm64` corpus **GREEN** — 7 errors / **394,697** tests, all known
  confounds.
- `pe64-x86_64` CLI smoke **14/14**; its corpus ran under Wine with 4 aborts and
  4 resumes, and 1 failure excused ONLY because the leg is emulated.
- arm64 VPS: `elf64-arm64` CLI smoke **14/14**; the other three legs' smokes were
  CLASSIFIED skips naming the host OS and the absent launcher, never silent.

⛔ **NOT MEASURED, and each is a hole, not a pass:**
- **No host produced a step-9 VERDICT BLOCK.** WSL's was destroyed by
  [[D-HARNESS-ABORT-SUMMARY-CRASHES-ON-THE-RUN-THAT-NEEDS-IT]]; the other three
  were cancelled before reaching it.
- **The Windows corpus never ran at all** — it was still in step 7b (per-leg CLI
  builds, ~11 min each) after ~2 hours, having spent ~89 min on the testfixtures.
- **The native arm64 corpus never ran.** ★★ THE REASON IS WORTH KEEPING: the VPS
  driver runs legs in catalogue order, so it spent **59+ minutes emulating x86_64
  under `qemu-x86_64`** — duplicating a leg WSL had already run NATIVELY green —
  while the one result only that machine can give sat queued behind it. ⚠ On a
  re-run, drive that host with `DSS_LEGS=elf64-arm64`.
- **macOS: no build, no run, no CLI smoke** this cycle. Its UNITS are green
  (898/898) and that is a separate, complete measurement.

### ⛔ TWO RE-RUNS ARE OWED AND ARE **DEFERRED BY OPERATOR INSTRUCTION** — DO NOT RUN THEM

Operator, 2026-08-18, verbatim: *"don't rerun qemu now please. we need to enhance
compile time before it, we'll do it later"*. ⚠ **This is a deliberate sequencing
decision, not an oversight, and a future reader must not discharge it as tidy-up.**

The two owed items, stated precisely so nobody re-derives them wrongly:
1. **arm64 VPS units re-run.** Its 897/898 was measured on a tree synced BEFORE the
   last three fixes landed; the sole failure was `anchor_registry_guard`, the
   registry cell-width violation now fixed. ⚠ On POSIX the only product-code
   delta since that sync (`make_preferred()`) is a NO-OP, so the compiler
   behaviour it measured is the shipped behaviour — but the trees are not
   byte-identical and the claim is stated at that strength, not stronger.
2. **WSL sqlite re-run.** Its run produced every corpus result and then died in
   step 9/9 (see `D-HARNESS-ABORT-SUMMARY-CRASHES-ON-THE-RUN-THAT-NEEDS-IT`), so
   there is **no verdict block** for it. The per-leg numbers below were read from
   the lines that printed before the crash and are sound; the aggregate verdict
   was never computed.

★ THE BLOCKER TO CLEAR FIRST IS [[D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX]]
— the reason the operator wants compile time addressed before paying for another
full matrix is in that row's numbers: ~18 min per leg on the Windows host against
~2m20s on WSL and ~5 min on a small ARM VPS.

### ★★★ THE FINDING: EVERY PATH-IDENTITY KEY IN THE COMPILER WAS 8.3-BLIND ON OUR OWN TOOLCHAIN

✔**MEASURED** with a standalone probe against `c++.exe (MinGW-W64 x86_64-ucrt-posix-seh) 13.2.0` —
the compiler `build/dbg` actually uses:

```
input            : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1
exists           : yes
weakly_canonical : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1   ec: <none>
canonical        : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1   ec: <none>
```

libstdc++ resolves `.`/`..` and symlinks and has **no concept of an 8.3 alias**, so two spellings of
ONE directory survived as TWO keys. ★★ **The MSVC STL happens to normalize them, which is why the
2026-08-17 CI fix looked like it worked: the property was never HELD, only accidentally satisfied by
one toolchain.** The consequences were live — the preprocessor's `#pragma once` re-entry guard was a
`vector<fs::path>` searched with `std::find` (a LEXICAL compare), so a header reached through both
spellings would be preprocessed TWICE into one TU; and the source-dedup keys would admit one file
twice, producing a duplicate CU and a duplicate-symbol link error naming no manifest.

★★★ **OPERATOR RULING 2026-08-18 — option (A) WITH TWO UPGRADES.** The test that decided it:
*"the only one whose invariant a check script can hold."* Boundary normalization (option B) was
rejected on its own analysis — an unbounded ingress set where a miss is silent *"is not an invariant,
it is a habit"* — and §4 of the ruling **forbids doing both**: a second normalizer is a second owner
of one fact, and the one that runs first wins silently.

- **Upgrade 1 — PATH IDENTITY IS A TYPE, NOT A FUNCTION RESULT.** `core::PathIdentity`
  (`src/core/substrate/path_identity.{hpp,cpp}`) has exactly one constructor, `of()`; the 14
  containers are keyed on it, so a raw `fs::path` **does not compile** as a key. Converting OUT is
  free; construction IN is the one gate.
- **Upgrade 2 — THE LINT DEFINES A COMPLEMENT.** `scripts/check-path-identity/check-path-identity.py`: rule 1 refuses
  `#include <filesystem>` in `src/` outside a 50-file allowlist (so a NEW file doing path work is a
  decision on the record); rule 2 is the enumeration underneath. Comment/string-stripped so it fires
  on CALLS not mentions, with a 4-control `--selftest`.

★★ **THE UPGRADE PAID FOR ITSELF IMMEDIATELY, AND THIS IS THE DURABLE LESSON: the type found FOUR
sites the originating grep was structurally incapable of seeing, because none of them called
`weakly_canonical` at all** — `dependency_resolver.cpp`'s artifact `seen` (raw `generic_string()`),
`shipped_lib_descriptor.cpp`'s `claimedPaths` (a THIRD normalizer), and the `visited` closure sets in
`import_resolver.cpp` and `preprocessor.cpp` (×2). **13 sites became 14.**

⚠ **ONE SITE WAS DELIBERATELY NOT CONVERTED** — `core/types/config_path_walk.cpp` resolves the
running executable's symlink to derive a directory to WALK; its result is never compared, never a
key, never printed as an identity. It carries an explicit, reasoned exemption in the lint.

### ⚠⚠ TWO TRAPS INSIDE THE FIX — BOTH MEASURED, ONE BEFORE AND ONE AFTER

1. ✔**BEFORE the code was written:** `GetLongPathNameW` **fails wholesale when any component is
   absent** — `DSS-SC~1` expands, `DSS-SC~1\not\yet\built.o` comes back **UNCHANGED**. The obvious
   one-call composition therefore no-ops on exactly the paths that do not exist yet (every output
   artifact) while looking correct on every input path that happens to exist. The implementation
   expands the longest EXISTING prefix and re-attaches the remainder verbatim.
2. ✔**AFTER, caught by the suite:** unifying the identity key on `generic_string()` was right, but
   leaving the conversion OUT generic too handed callers a forward-slashed Windows path. Three
   native-probe tests died with `'""C:' is not recognized as an internal command` — the probe builds
   a `.bat` path from a canonicalized base plus a natively-joined suffix, and **`cmd.exe` cannot run
   a forward-slashed path while every Win32 API accepts one happily**. ⇒ `string()` is the identity
   and stays generic; `path()` returns PREFERRED separators.

### ★ THE TEST THAT WAS RIGHT WHEN I ASSUMED IT WAS MIS-AIMED

`MatchesTheProductsOwnCanonicalizer` scanned `dependency_resolver.cpp`'s body for `weakly_canonical`.
Routing the product through `PathIdentity` emptied that body, so the pin failed — saying exactly the
true thing. **Its subject moved; it was not wrong.** It is replaced by
`FixtureHasNotGrownItsOwnCanonicalizer` (the fixture now CALLS the product, so "the two agree" would
be x==x — a test that passes both ways). ⚠ Its positive control was mutating `weakly_canonical`, a
token the fixture no longer contains, so it would have silently asserted NOTHING; re-pointed to the
token the pin now searches for.

### ★★ TWO HARNESS DEFECTS FIXED IN PASSING, BOTH OF THE SAME SHAPE

- **`mustDifferFromBaseline` was read by ONE of the two example runners.** ✔MEASURED **455 of 609**
  manifests declare it across **548 arms**; `integrated_tests/runner.cpp` never learned the key, so
  every one of those assertions was invisible on the CLI-subprocess side. Implemented (parse + allow
  + **read**) — whitelisting alone would have re-created, inside the fix, the exact defect the closed
  key set exists to prevent.
- **The macOS leg could not be staged at all.** ✔MEASURED: `printf 'X' \| scripts/ssh-macos/ssh-macos.sh cat`
  returns **nothing** — the Mac's login profile CONSUMES STDIN, so tar-over-ssh arrives truncated.
  The VPS survived the identical path only because its profile is quiet. Both `.sh` carriages gained
  a `--rsync` transport that never touches stdin; ✔verified by execution, not by exit code.

⚠ **AND THE MISTAKE I KEPT REPEATING, RECORDED BECAUSE IT COST FOUR ROUND TRIPS:** a `\| tail` after
a build or a gate replaces the status that matters with `tail`'s. It produced a "green" Windows leg
over a red ctest, a "successful" build that had failed on a brace error, and two "completed" legs
that had died on a path. **Take rc DIRECTLY; use `scripts/run-gate/run-gate.sh`, which exists for this.**

## 0.000001 ★★★ CYCLE P7 — pe64 GETS A POSIX LAYER, AND AN IDENTITY CLAIM BECOMES CHECKABLE

**Every claim below is ✔MEASURED unless labelled otherwise.**

### ★★★ THE HEADLINE, AND IT INVERTS WHAT THE CYCLE OPENED WITH

The cycle opened believing it had caused a **105-diagnostic pe64 regression** by deleting `_MSC_VER`.
✔MEASURED against the leg's OWN same-platform reference compiler (`x86_64-w64-mingw32-gcc`, gcc
13.2.0, driven by the 46 `-D`/`-I` flags lifted verbatim from the harness's own
`out/pe64-x86_64/reference-oracle.log`):

| TU | DSS | mingw-w64 gcc, same flags | owner |
|---|---|---|---|
| `bld/shell.c` (CLI) | 24 | **0** | DSS |
| `src/test_fs.c` | 38 | **0** | DSS |
| `src/test_thread.c` | 1 | **0** | DSS |
| `ext/misc/fileio.c` | 66 | **31** | **UPSTREAM** |

★★ **`ext/misc/fileio.c` cannot be built for any Windows target by any GNU-on-Windows compiler.**
`fileio.c:86` gates `#if !defined(_WIN32) && !defined(WIN32)` with **no `__MINGW32__` escape**, so
every Windows target takes `#include "windirent.h"`; `windirent.h:22` opens
`#if defined(_WIN32) && defined(_MSC_VER)`, so under any non-MSVC toolchain the shim body is elided
and `<windows.h>` never arrives — while `fileio.c` uses the Win32 vocabulary unconditionally.
Upstream ships Windows via `Makefile.msc`, so the configuration is never exercised there.
⇒ [[D-UPSTREAM-SQLITE-FILEIO-WINDIRENT-IS-MSVC-ONLY]].
★ **The identity fix did not CREATE that; it UNMASKED it.** pe64 had been compiling the TU only by
claiming an MSVC identity DSS does not have. Agreeing with gcc is the conformance outcome.
⚠ **AND `fileio.c` IS STILL CHARGED TO DSS**, because the new attribution machinery found a real DSS
gap inside it on its first run — `_wchmod` — which is exactly the false-amnesty a coarse rule would
have granted. Fixed this cycle; the TU's residue is now empty of DSS-attributable names.

### ★★★ WHAT LANDED — pe64 NOW HAS A POSIX LAYER, MODELLED ON WHAT THE REFERENCE ACTUALLY IS

`<unistd.h>` on pe is a **RE-EXPORT**, not a symbol dump, because that is what mingw's own header is:
it `#include`s `<io.h>`, `<process.h>`, `<getopt.h>` and declares only eight names itself. So
`unistd.json` gained pe via **format-gated `includes` edges** — new vocabulary this cycle
([[D-FFI-DESCRIPTOR-INCLUDES-EDGE-GATE]]) — plus its own constants.
★ **`getpid` is deliberately NOT in `unistd.json`.** It arrives on pe only over the pe-gated edge to
`process.h`, which makes it the witness that the surface checker walks the ACTIVE CLOSURE rather than
the named descriptor. **Do not "simplify" it into a direct declaration — that silently deletes the
witness.**

**DSS now ships bodies, not just declarations** — `runtime/platform/src/unistd.c` (139 lines, 7
realized bodies), the second witness for [[D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF]] after
`dirent.c`. Each one exists because a `linkName` would have been WRONG, not merely absent:
* `sleep`/`usleep` — ucrtbase exports `_sleep`, which takes **milliseconds** where POSIX takes
  **seconds**. A `linkName` onto it is a silent 1000× error. Red-on-disable proves the body: the
  mutant that treats the argument as ms reddens with a **289 ms** wall against the good run's 1289 ms.
* `ftruncate`/`ftruncate64`/`truncate`/`truncate64` — mingw realizes `ftruncate` as a header INLINE
  over `_chsize`, so no export of that name exists to bind to.
* `swab` — `_swab` is a real export, but DSS declared `swab` on **no** format, so it was closed on
  elf and macho too.

Also closed, each against the oracle: `dup`/`dup2`; `_fileno`/`_pclose` moved to `stdio.json` where
the reference has them; `open`/`close`/`read`/`fstat` converted from **macros to `linkName` symbol
rows** (the witness takes `&open`, impossible if they were macros); the `F_OK` family on `<io.h>`;
`S_ISBLK`/`S_ISSOCK` per-format (`S_IFBLK` was not merely absent on pe — it shipped **24576** flat
where mingw declares **12288**, a value NEITHER reference declares); `_wchmod` via an EDGE, because
mingw's `<sys/stat.h>` line 14 *is* `#include <io.h>`.

★★ **AND A LATENT SILENT MISCOMPILE, FOUND ONLY BECAUSE THE GAP HID THE GAP.** `clock` was declared
flat `fn() -> i64`, but `clock_t` on pe is `long` = **4 bytes**: reading a 64-bit return takes the
**undefined upper half of RAX**. It had never fired because `clock_t` had no pe arm at all, so
`clock()` died at its declaration. Fixing the declaration would have exposed the miscompile; fixing
both is the cycle's second `_sleep`-shaped defect — links clean, loads clean, wrong answer. The
width is now double-authored (`i64` elf/macho, `i32 "long"` pe) and pinned by a RUNTIME red-on-disable
(revert to flat `i64` ⇒ exit 2). `CLOCKS_PER_SEC`/`CLK_TCK` were declared on **no** format — which is
what made the facility unusable rather than merely awkward — and six pe `<time.h>` macros became
symbol rows.

### ★★★ `impliedSurface` — AN IDENTITY MACRO'S PRESENCE IS NOW A CHECKED CONSEQUENCE

[[D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE]]. Every `predefinedMacros` row in every document
declares one of three states; there is no bare `null` and no defaulted absence. The claim names
**SYMBOLS**, never just a header — header granularity is nearly vacuous once re-export exists, since
`<unistd.h>` resolves on pe the moment the `io.h` edge fires even if the only name that arrives is
`getpid`.

★★★ **THE REFUSAL HAS BEEN OBSERVED FIRING TWICE, AND ONE WAS AN ACCIDENT.**
1. ✔THROUGH `ctest` — the only channel that proves the mutant was READ, because `findShippedConfig`
   walks the CWD unless `DSS_CONFIG_ROOT` is set and only `dss_add_test` sets it. Control-green →
   mutant-red → restore-green, mutant sha-different:
   `error[C_UnbackedPredefinedMacro] predefined macro '__MINGW32__' (<lang>.lang.json
   /preprocess/predefinedMacros[21]) requires 'unistd.h' to declare 'sysconf' on object format 'pe',
   and the shipped surface reachable from that header does not.` — macro, CONFIG ROW, header, missing
   SYMBOL and format all render.
2. ★★ **UNPROMPTED, IN THE LIVE TREE.** A lane moved `_fileno` from `io.json` to `stdio.json`; a
   claim two files away went stale the same instant and the build refused. Nobody was testing for it.
   That is the mechanism's entire purpose, arrived at by accident, which is the strongest evidence
   available that it is not a formality.

⚠ **THE FIVE NULLS THAT PROVE WHY THE SHAPE HAD TO BE TAGGED.** The first cut was `null`-or-claim.
✔MEASURED against the real corpus: 80 of 84 rows were `null` — and FIVE of those were
surface-implying platform identities written as `null` **while populating the mechanism built to
prevent exactly that** (the two OS-selection rows against a 97-symbol/64-typedef/89-constant/16-struct
surface, and the three platform-vendor rows against the Darwin cluster). Operator's design bar: *the
question is not whether those five were wrong, it is WHAT FINDS THE SIXTH.* The obvious rule — force a
claim only on FORMAT-GATED macros — ✔MEASURED to reach **19 of 84** and to EXEMPT the four
compiler-identity rows, which are ungated and are the subtlest class there is. Hence: no bare `null`
anywhere, a closed `reason` tag (`erases-to-nothing` / `arch-property` / `standard-defined`), and a
third state **`not-expressible`** for a macro that DOES imply a surface this predicate cannot state.
★ The closed tag is what makes copy-paste decay REVIEWABLE — an inherited `null` is invisible in a
diff; an inherited TAG that is wrong for the new macro is not.

⚠ **THE KEY IS `impliedSurface`, NOT `requires`, AND THE RENAME WAS AN OPERATOR REVERSAL OF THEIR OWN
NAME.** ✔MEASURED: `requires` already carried TWO unrelated meanings — the document-level grammar-HOLES
contract (`asm.lang.json`) and the sqlite harness's confound environment-probe list (`legs.json`,
pinned at `tests/harness/test_sqlite_harness_legs.cpp:1584`). *"Different scopes, no parse ambiguity"
is a correct statement about the PARSER and the wrong test for a config key."* Renamed while the
surface was 84 rows and nothing pinned it.

### ✅ TWO LOAD-TIME INVARIANTS, AND THE GATE THAT UNCOVERED TWO OLDER DEFECTS

(i) **EDGE FIRES ⇒ CHILD AVAILABLE** and (ii) **NO EMPTY SURFACE ON A SERVED FORMAT**, corpus-wide and
format-INDEPENDENT (an arm no current target selects is exactly the arm that rots). ✔MEASURED over the
whole existing shipped set BEFORE any new row was added — 49 descriptors × {elf,pe,macho}, **0
diagnostics** — so neither is introduced on top of a live violation. An EMPTY served-format span is
refused: a sweep that cannot fail is not a sweep.

★★ **Turning the gate on exposed two PRE-EXISTING defects with nothing to do with the feature:**
an unavailable parent still recorded its **entire** `includes` closure, so the semantic tier injected
every sibling's surface for an `#include` it had just rejected — one diagnostic away from a silent
wrong-surface injection; and a closure sibling unavailable on the active format raised a diagnostic
naming a header the user never wrote while the preprocessor tier silently skipped the same case, which
is the tier drift the FC15c single-funnel design exists to prevent.

### ✅ THE HARNESS CAN NOW SAY WHOSE FAILURE IT IS

[[D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION]]. `poisoned` was decided by one predicate —
`grep -qE 'error\['` over the compile log — with no notion of whose error it was. ★★ **And the control
already existed; the REPORTING threw it away**: a reference build that RAN AND FAILED printed
`NO ORACLE`, identical to one that could not be attempted, so *"the reference agrees with us"* and
*"there is no reference"* rendered as one sentence. Now a third `CONFOUND_MATCH_KINDS` entry
(`build-tu`) beside `unit` and `abort-file`, same provenance discipline, same lint.
★★★ **A ROW ALONE EXCUSES NOTHING** — amnesty needs this run's oracle to have errored in THAT TU, an
active row naming it, AND every identifier-bearing DSS error subject in that TU to have been named by
the reference too. ⚠ Stated limit with its size: **cascade diagnostics naming no identifier are
unattributable — 44 of the 105 on the witness leg** ([[D-HARNESS-BUILD-ATTRIBUTION-BLIND-TO-CASCADE-DIAGNOSTICS]]);
and the CLI artifact has no oracle at all ([[D-HARNESS-CLI-ARTIFACT-HAS-NO-ATTRIBUTION-ORACLE]]).

Also landed: **run FIDELITY** — the resolver was computing `arch_ok` and discarding it, collapsing
`foreign-kernel` and `emulated` into one `launched` mode. ✔MEASURED on this project's own hardware:
`macho64-x86_64` on darwin/arm64 is `emulated` (Rosetta: foreign ISA, NATIVE kernel) while
`elf64-arm64` on windows/arm64 is `foreign-kernel`. `DSS_RUN_FIDELITY` selects on it in **both**
drivers. ⚠ Follow-on: `scope: emulated` and `fidelity: emulated` now disagree
([[D-HARNESS-CONFOUND-SCOPE-EMULATED-COLLIDES-WITH-RUN-FIDELITY-EMULATED]]).

### ★★★ THREE CONFORMANCE FIXES, ALL THE SAME SHAPE: **DSS DISAGREED WITH EVERY REFERENCE**

The rule is that reference compilers are the spec **BIDIRECTIONALLY** — DSS may not reject what they
accept, and may not silently accept what they diagnose. All three below were found by MEASURING both
directions rather than by chasing a complaint, and one of them exists only because the sweep was done.

**1. `P0014` incompatible macro redefinition was FATAL.** ✔MEASURED one TU per shape, mingw
`-std=c2x -pedantic`, cross-checked with MSVC `cl /Zs` (C4005): parameter spelling, replacement text,
arity, object-vs-function-like and variadic-vs-not **all warn at rc=0, none is fatal, and in every case
the SECOND definition is in effect at runtime** (verified by BUILDING AND RUNNING each shape, not by
reading the diagnostic). Only `-Werror` stops a build, which is a choice about warnings. DSS errored
AND retained the OLD definition. ★ **The value half is the one that could silently miscompile** — a
severity-only change would have left DSS agreeing about the diagnostic and disagreeing about the
program, so the early `return` had to go with it. Live consumer: sqlite `shell.c.in` defines
`S_ISLNK(mode) (0)` at line 141, BEFORE its `#include <sys/stat.h>` at 148.
⚠ **A LANE HAD ALREADY WORKED AROUND THIS AND SAID SO IN ITS OWN `$comment`** — it spelled our
descriptor's parameter `mode` to match shell.c, calling it *"a LOTTERY … Fixing P0014's severity in the
preprocessor is what actually closes the class"*. That fix landed, so the arm is back to `m`, matching
every other macro in the array, and the rationale now records that the spelling is **no longer
load-bearing** so nobody reintroduces it. ✔Re-measured: the shell.c gate order gives rc=0 with one
`warning[P0014]` and zero errors; `ext/misc/fileio.c` stays at 0 compile errors.

**2. The sweep found the OPPOSITE defect, which had no reporter.** `MacroDef::text` joined replacement
tokens with a single space UNCONDITIONALLY, so C 6.10.3p2's *presence, not amount* rule was erased:
`40+2` and `40 + 2` compared EQUAL and DSS said nothing where both references warn. Fixed with a
parallel `spacing` bitmap — kept a separate field rather than encoded into `text` with a sentinel byte
because **no byte is safe**: a string-literal token's text is its source SPELLING, and a source file may
legally contain any byte inside one. ★★ **This is why the shapes were swept individually.** The
over-strict half was reported by a consumer; this half had nobody to report it, and generalising from
the complaint would have shipped a fix that was still half wrong.

**3. A shipped OPAQUE tag could not be completed by the TU.** ✔MEASURED: completing `DIR` or `FILE`
was `rc=1 F_ShippedTypeIdentityConflict` on DSS and `rc=0 clean` on mingw; forward-declare-only, both
reverse-direction orders, and an unrelated-tag control were already clean. Root cause: **the descriptor
vocabulary had no spelling for "opaque"**, so an empty named struct stood in — and `type_interner.hpp`
says in its own words that `struct E {}` is *"a LEGAL COMPLETE zero-field struct (size 0)"*. The
descriptors' prose called those types OPAQUE and INCOMPLETE while the engine recorded COMPLETE: the
comment-holds-the-full-fact-while-the-code-uses-half pattern. ★ The incomplete machinery already
existed (`forwardComposite` / `isIncompleteComposite`); what was missing was a spelling and a
narrowing. The spelling follows the codec's OWN precedent — a bare keyword after the name, like
` packed` — and is TERMINAL, no braces. **50 spellings migrated across 3 files** (`FILE` alone is 46),
all together, because one leftover would mint a second COMPLETE `FILE` and split the identity
`sCtx.xCloser = pclose` is compared by. ⚠ NARROWED, NOT DELETED: two COMPLETE declarations that
disagree are still refused.

### ⚠ TWO SUSPICIONS OF MINE THAT MEASUREMENT KILLED — recorded because the near-misses were the useful part

* **"The opaque narrowing went too far."** A complete-vs-complete `struct dirent` case compiled clean,
  which looked like a hole the fix had just opened. ✔Disabling the arm and rebuilding showed it STILL
  compiled — so the case never reached the conflict path — and the same run confirmed the red-on-disable.
  The reason is [[D-FFI-DIRENT-API-DECLARED-OVER-VOID-NOT-ITS-OWN-STRUCTS]]: `readdir` is declared
  `fn(ptr<void>) -> ptr<void>` and NO shipped signature references the tag, so there is nothing to adopt.
  **No miscompile** — and a new row for the laxness that POSIX would not have.
* **"A registry row's cells are broken."** `D-LANG-PE64-HAS-NO-POSIX-DIRECTORY-API` read as six cells
  with its Closing-work column reduced to a bare backslash, and `check-anchor-registry.sh` passed anyway.
  ✔It has exactly 5 UNESCAPED pipes, its embedded C `||` is correctly written `\|\|`, and it matches its
  table's own header. **My reader split on every pipe including escaped ones**, and a scan I wrote
  assuming 4 columns everywhere produced 45 false positives — the file holds ~293 tables whose widths
  differ BY DESIGN. ★ A row's arity is only meaningful against its OWN table header. The guard was right.

### ★★★ INSTRUMENT LESSONS THAT OUTLIVE THIS CYCLE

* **A CROSS-REFERENCE SATISFIES `check-anchor-registry.sh`.** ✔MEASURED: two anchors reported
  UNREGISTERED, then reported OK after four *unrelated* rows were appended whose prose merely cites
  them — each still appearing exactly ONCE in the whole file, as that citation. The guard is meant to
  prove a deferral is RECORDED (trigger, closing work); a passing mention records none of it.
  [[D-GATE-ANCHOR-REGISTRY-GUARD-ACCEPTS-A-CROSS-REFERENCE-AS-A-REGISTRATION]].
* **A STRAY FILE NAMED AFTER A LANGUAGE BECOMES A SECOND LANGUAGE DOCUMENT.** `c-subset.lang.json.orig`
  and `.rej` both trip it; `zz.lang.json.bak` does not — the match is the language-NAME prefix, not the
  suffix. The message names neither file and reports a downstream extension collision instead. ⚠ Live
  tooling hazard: any script writing `<lang>.lang.json.tmp-*` beside the original and dying before its
  rename leaves the tree unbuildable with a message pointing elsewhere.
  [[D-CONFIG-STRAY-FILE-NAMED-AFTER-A-LANGUAGE-LOADS-AS-A-SECOND-DOCUMENT]].
* **A TEST PIN THAT MATCHES A WHOLE ROW VERBATIM BREAKS ON FIELDS IT SAYS NOTHING ABOUT.** Two pins
  reproduced `_WIN32`'s entire entry to locate it; both broke when `impliedSurface` landed. Re-anchored
  to the row's name plus the field each test is actually about. Not a weakening — the verbatim match
  only ever guaranteed the substitution happened.
* **ENCODE BEFORE YOU OPEN FOR WRITING.** A config-editing script opened a 433 KB `.lang.json` for
  writing and *then* hit a `UnicodeEncodeError` on a surrogate pair — truncating it to **0 bytes**.
  Recovered from the git index. Every editing script here now encodes and JSON-parses the payload
  first and `os.replace`s a temp file last.
* **A TEXT QUERY CANNOT ANSWER A CONTAINMENT QUESTION.** Adding the mandatory field to inline C++
  fixtures took three attempts: a POSITIONAL rule ("walk back to the nearest `predefinedMacros`")
  rewrote `"target":{"name":"X"}` across a raw-string boundary; a CONTENT rule (`"name"` + `"kind"`)
  rewrote 20 `relocations` entries that legitimately carry both. Only bracket-matching the array and
  verifying every occurrence fell inside it was correct. Same family as the anchor-count instrument
  that has been wrong twice.

---

## 0.0000 ★★★ CYCLE P6 — THE ASM BLOCKER BURN-DOWN, THEN THE IMPLEMENTATION HALF OF A TOOLCHAIN

**Operator argument: *"I WANT NO FAILURES, NO BLOCKERS, ADDRESS EVERYTHING."*** Lanes on disjoint
file sets. ✔**GATE, ALL THREE LEGS ON THE FINAL TREE: Windows 887/887 · WSL x86_64 887/887 · arm64
887/887** — the arm64 leg on the **REAL VPS hardware, not qemu**, each via `scripts/run-gate/run-gate.sh` with
its tool-emitted `100% tests passed` witness. Everything below is MEASURED by the lane that reports it.

### ★★★ THE HEADLINE: DSS NOW SHIPS THE IMPLEMENTATION HALF OF A TOOLCHAIN
`D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF` — operator §B ruling, **realized**. A shipped-header
descriptor may declare, per object format, that a symbol's body is PROVIDED by a shipped source unit
rather than IMPORTED: `"realization": { "pe": { "source": "runtime/platform/src/dirent.c" } }`.
✔**PREMISE MEASURED BEFORE ANY CODE:** a Windows host compiled a 2-CU program for pe64 and it **RAN,
exit 42**; a Linux host produced pe64 + elf64-aarch64 + macho64-arm64 and the aarch64 one **RAN under
qemu, exit 42** — the graph compiles a shipped unit FOR THE TARGET, cross, on every host.
★★★ **THE STRUCTURAL WIN IS PROVEN, NOT CLAIMED:** rename `d_name` in the descriptor's pe
`struct dirent` and `runtime/platform/src/dirent.c` **FAILS TO COMPILE** — the ABI agreement is
checked by the compiler, because the implementation `#include`s the declaration the descriptor
generates. ★ **COMPILE-ALWAYS ≠ LINK-ALWAYS**, measured both ways: `hello.exe` on pe64 carried
`_wfindfirst64i32`/`MultiByteToWideChar` before the split, **0 and 12288→3072 bytes** after, while the
consumer still links them and exits 42.

### ⚠ TWO THINGS THAT ARE **NOT** DELIVERED — do not read the cycle as though they were
1. **THE RUNTIME OBJECT CACHE HAS NO PRODUCTION CALLER.** ✔MEASURED: `resolveArchiveSiblingFormat`,
   `computeRuntimeObjectKey`, `lookupRuntimeObject`, `storeRuntimeObject` are referenced nowhere in
   `src/` outside their own TU. The invalidation property is proven in isolation; **every build still
   compiles the runtime unit from source every time.** Wiring it into the driver is the next step.
2. **THERE IS NO SHIPPED WARM CACHE.** Lane PK correctly declined to install an empty
   `dist/release/` — an `install(DIRECTORY)` at a non-existent tree creates an empty destination and
   reports success, reading for months as though the cache were shipping. ⇒ the *"users never pay the
   cold-build cost"* benefit that partly motivated choosing this option over shipping prebuilt objects
   **does not exist today**, and with two roots (shipped read-only + per-user writable) nothing yet
   bridges them.

### ✅ CLOSED THIS CYCLE
- **`.cfi_*` UNWIND PRODUCER, BOTH PORTS** (`D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`). gdb 15.1 walks
  a **3-frame stack and STOPS** out of a DSS ELF exec with **no frame pointers**; the matched control
  (same file, every `.cfi_*` deleted) runs off into a **stack address presented as a return address**.
  arm64's FDE is **rule-for-rule and delta-for-delta identical to `aarch64-linux-gnu-as` 2.42's**.
  ★★★ `.cfi_escape 0x10,0x06,0x02` **in this repo's own corpus file was a LIVE SILENT CORRUPTION** —
  it declares a 2-byte expression, supplies zero bytes, and makes GNU `as` itself eat the following
  `.cfi_restore 6` out of the unwind program at rc=0 with no diagnostic. gas validates nothing here.
  ⛔ pe64 is a MEASURED BOUNDARY: Win64 `UNWIND_INFO` needs `SizeOfProlog` and the GNU `.cfi_*` family
  has **no prologue-end verb** (checked over gcc's complete emitted set, nine spellings) — that is what
  `.seh_endprologue` is for. Refused by name.
- **`"+r"` TIED OPERANDS RUN** (`D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE`) — exit 42, debug and release.
  The tie is a **location identity**, not new machinery: bind both halves to one `LirReg` and x86's
  two-address `add` composes for free while arm64's three-address `add` needs nothing.
- **bare `asm`** (`D-CSUBSET-INLINE-ASM-SPELLING`) — and the omission was **never neutral**: DSS was
  **ACCEPTING `int asm = 42;`**, which no reference compiler accepts in GNU mode. An invented
  extension, not a missing feature.
- **positional `%lN` refused at the semantic tier** so the two tiers agree · **template diagnostics
  render their source line** (and the row **under-counted** — every lowering-refusal was equally
  unrenderable) · **the same defect fixed in the FFI header reader**, found by looking for the class ·
  **the CLI runner builds 313 optimizer arms, 277 artifacts byte-differing** · **the two-runners rule
  got a machine check** (25 keys, zero difference) · **the anchor guard now scans `tests/` +
  `integrated_tests/`** (837 citations had never been checked) · the `got ` fragment · the S0067
  docblock · the LIR fixture that could not see an `__asm__` at all.

### ★★★ THREE INSTRUMENT LESSONS THAT OUTLIVE THIS CYCLE
1. **A VACUOUS ASM PIN CAN SURVIVE *BOTH* ARMS — `release` is not a rescue.** A **single `"+r"`**
   example stayed GREEN over a live mutant at debug AND release: the read half's `mov` targets a dead
   vreg, the result vreg's range starts right after, they never overlap, and the linear scan hands the
   result that register already holding the right value. Two tied operands → red on both.
2. **CLAUSE 5 HAS A MIRROR — prove the RESTORED bytes reached the process.** A harness restored the
   source without rebuilding, so its "good" column measured the MUTANT. **Both columns agreed
   perfectly**, which reads exactly like a stable measurement, and produced a false verdict that
   briefly looked like a real bug. Now in `references/the-bar.md` §A.5.
3. **A SAFE PORT CAN MASK A DANGEROUS ONE.** A `for (p : {kX86, kArm})` loop with an `ASSERT` aborted
   at the x86_64 half — where the mutant fails LOUD — and never reached aarch64, where the same mutant
   resolves **silently** to physical x30. All 17 port loops now run through a `void` callable.

### ⚠ AND ONE ABOUT THE ORCHESTRATION ITSELF
`D-BUILD-LANES-SHARE-ONE-CONFIG-TREE-AND-ONE-WORKING-TREE`: a per-lane `build-<lane>` dir isolates
OBJECT CODE and nothing else. **Config is runtime data read from the shared source tree**, so one
lane's `.lang.json` edit is instantly live for every other lane's already-built binary — one lane's
"baseline 874/874" was measured across a config edit and **was not a baseline of anything**. A partial
build also relinks the shared DLL. ⇒ per-lane git WORKTREES, or stage config into the build dir.
The orchestrator launched six lanes into one tree; the method outran the isolation it assumed.

### ★★★ A GUARD WHOSE PASS WAS MANUFACTURED BY ITS OWN BUG REPORT — and the fix repeated the defect twice

`D-GATE-ANCHOR-CITATION-RESOLVES-VIA-ITS-OWN-BUG-REPORT` ✅ CLOSED. Citation resolution in
`check-anchor-registry.sh` is **substring-anywhere over `.plans/`**, and that tolerance is
**load-bearing, not sloppiness** — ✔MEASURED: of **1181** unique citations, **874** resolve to a
registry row key and **307 only via prose**, of which **160 are line-wrap fragments** of one real
name (`…-NOT`, `…-NOT-ADDRESSABLE`, `…-NOT-ADDRESSABLE-AT-AN` are ONE anchor split across comment
lines). ⇒ The cost of the tolerance is that **it cannot tell a LIVE name from a DEAD one.**
✔The proof: `D-ASM-INDIRECT-BRANCH-SUCCESSOR-SET-UNDERIVABLE` was cited in
`tests/asm/test_asm_text_to_lir.cpp:1528` after a **one-word rename** to `…-UNSTATED`, and the ONLY
text in `.plans/` supplying its resolution was **the row written to report it as stale**. It had also
never been checked at all before this cycle widened the guard to `tests/` — so it went from
UNSCANNED straight to FALSELY-RESOLVED, validated at no point.

**★★★ THE TRANSFERABLE PART IS THE FIX'S OWN TWO FALSE POSITIVES, each the previous rule's failure
mode:** (1) matching the words `RETIRED ID` in a status cell red on the row whose prose merely says
*"as a retired id"*; (2) matching the unspaced token `RETIRED-ID` then red on **the row that
DOCUMENTS the token** — the row explaining the rule was classified BY the rule, the same
self-reference the check exists to kill, one level up, within minutes. ⇒ **A marker that is merely
PRESENT can always be tripped by writing about it.** The marker is now **POSITIONAL** — the status
cell must OPEN with the retirement (`^ *\**✅ \*\*CLOSED <date> — RETIRED-ID`), where prose never
sits, which is the rule the anchor-balance instrument already used (CLOSED iff the cell BEGINS with
`✅`). Comparison against citations is **EQUALITY, not substring**, since a retired id may be a
PREFIX of a live one. Exit **4**, fail-closed at 0 marked rows, ✔red-on-disable measured BOTH
directions (plant a retired id ⇒ exit 4 naming the file:line; remove ⇒ green — the clause-5 mirror).
⚠ **NOT fixed, and anchored rather than glossed:** `D-GATE-ANCHOR-DECLARATION-SITE-UNDEFINED-ACROSS-PLAN-FORMS`
(OPEN). The general tightening — resolve only against a plan **declaration site** — reds **147**
citations, and spot-checks show most are legitimate plan deliverable ids declared in shapes a
first-cell table matcher cannot see. **Triage the 147 before tightening; a gate that reds 147 without
a per-name verdict is a wall, not a gate.** ⚠ An earlier pass this cycle cried *"46 dangling
citations"* from a regex broader than the guard's own (1404 vs 1181, catching `D-32-BIT-WORD`);
re-measured with the guard's EXACT regex and include-set, **0 resolve nowhere. The alarm was the
instrument.**

### ★★★ A PLATFORM SPLIT THAT WAS A DEFECT IN DISGUISE — and the GREEN platform was the liar
The WSL leg red one test the Windows leg passed: the new sigil detector's
`AnOmittedRoleIsRefusedAndAnExplicitNullIsHonoured`. Root-caused rather than retried, and it was not
a toolchain quirk.
✔MEASURED: **the two shipped dialects genuinely give OPPOSITE answers to `symbolicNameClose: null`**,
because the role's bound KIND has different provenance in each. `asm-x86_64-att` binds
`PlaceholderNameClose`, which **no other lexeme declares** — so the synthesized row was its only
declaration, null left the binding dangling, and the load FAILS (loud). `asm-arm64-gas` binds
`BracketClose`, which its **global `"]"` row already declares for the memory form `[x0, #8]` — so the
kind stays interned, the load SUCCEEDS, **and the template goes on accepting `%[name]` through the
mode's global fallback.** ⇒ **the language declared the form ABSENT and one dialect kept parsing it.**
An accept-and-do-nothing — the worse of the two outcomes — and it was the GREEN platform hiding it.
★★★ **THE MECHANISM OF THE DISGUISE, and it is the durable lesson:** the pin asked
`dialects.front()` — ONE member of a **discovered** set — and `directory_iterator` is **sorted on
NTFS, hash-ordered on ext4**. ✔The enumerations are the measurement: NTFS put arm64 first (the silent
accept ⇒ green), ext4 put att first (the loud path ⇒ red). ⇒ **A genuine two-outcome defect was
collapsed into a green/red platform split, which reads as an environment problem and invites entirely
the wrong investigation.** ⚠ **Discovering a set and sampling one member is STRICTLY WORSE than
hard-coding one, because it LOOKS exhaustive** — this file discovered its dialects deliberately so a
third would be covered automatically, then threw that away at the last step.
⇒ `D-TEST-PIN-SAMPLES-ONE-MEMBER-OF-A-DISCOVERED-SET` (OPEN). ⚠ A grep cannot sweep it: 24 files under
`tests/` use `directory_iterator`, and a naive `.front()`/`[0]` scan returns **391 hits across 18
files**, nearly all ordinary indexing. It is a data-flow question, so a mechanical sweep would be a
wall, not a gate.
★★ **THE FIX IS AT THE LOADER'S INTERSECTION, NOT IN EITHER DOCUMENT.** Dropping the shape or its
reference would let a LANGUAGE's null silently delete a production from ANOTHER document's grammar —
a second owner of the grammar, i.e. the closed row's own defect one tier up. Four cases now closed and
decidable from the two documents alone: declared+bound → row; declared+unbound → LOAD ERROR;
**null+bound → LOAD ERROR (new)**; null+unbound → HONOURED, and the scan stops recognizing the form.
A binding exists exactly when the entry's closure spells the hole, so the verdict can never again
depend on what else is interned, in what order, or on which filesystem.
✔**BOTH PLATFORMS 881/881, 0 FAILED.** ★ The two halves were measured NON-REDUNDANT: the test fix
alone reds BOTH platforms; the loader clause is what makes both green.

### ✅ MACH-O TOO — WITNESSED BY APPLE'S OWN CRASH REPORTER, and it found a REAL capability gap
The operator brought macOS up mid-cycle, which FIRED the trigger on
`D-LINK-MACHO-IMAGE-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS` — the row the ELF lane had **correctly
refused to close blind**, because the bar wants execution and the Mac was off. ✅ Now closed on real
Apple Silicon (macOS 26.5.2): **Apple's own crash reporter**, unwinding a genuinely faulting stack and
symbolicating from the image nlist, went from `#0 sym_83 / #1 sym_88 / #2 sym_92` to
`#0 static_helper / #1 global_helper / #2 main` — **every offset identical**, so name-only — with the
trampoline correctly still synthetic. The functional change is **2 lines**, routing both image sites
through the *same* shared `imageName` the ELF lane added; a format-selecting ternary was DELETED.
⚠ **HONEST LIMIT, stated rather than blurred:** lldb could not attach — the Mac reports *Developer
mode is currently disabled*, and enabling it is a `sudo` security change a lane must not make. lldb
witnessed only statically. **The live-stack witness is the crash reporter, not lldb.**
★ The dylib control was run with a probe built by **Apple's own clang**: `_lib_static_helper` present
in the nlist, `dlsym` for it **NULL**, `dlsym("dss_lib_entry")` resolves and CALLS. The image tier
moved; the ABI surface did not.

### ✅ …AND IT WAS CLOSED THE SAME DAY — APPLE'S `ld` NOW ACCEPTS A DSS MACH-O IMAGE
`D-LINK-MACHO-LINKEDIT-SYMTAB-MISALIGNED` ✅. Same `ld` invocation, before and after, on real Apple
Silicon (macOS 26.5.2, **ld-1267**): arm64 dylib as a link input **rc=1 → rc=0**, and the linked
client then **RAN**; arm64 exec via `-bundle_loader` rc=1 → rc=0; x86_64 dylib rc=1 → rc=0. ✔`symoff`
**50076 → 50080** — the anchor's own number, reproduced INDEPENDENTLY.
★★ **ROOT CAUSE: THE INVARIANT LIVED IN THE PRODUCERS, WHICH IS WHY IT HELD FOR THREE BLOBS AND NOT
THE FOURTH.** The rebase, bind and export-trie builders each pad their OWN tail "so the next payload
starts aligned". The indirect symbol table has no such step — it is `count * 4` — so an **ODD**
indirect count (any module with a DATA extern, i.e. every `#include <stdio.h>` TU) left the nlist at
4 mod 8. ⇒ **a producer padding its own tail asserts NOTHING about the next blob's start.** The rule
moved to the consumer, and the whole cursor chain was audited — including the ones already correct,
three of which were correct only *accidentally*, via those producer pads.
★★★ **THE MIRROR CAUGHT ITSELF, and this is the most transferable result of the cycle.** The lane's
first restore used `shutil.copy2`, which **preserves mtime** — so ninja said *no work to do*, the
binary never relinked, and the "restored" ctest was still rc=8. **The restored bytes had never
reached the process.** Re-run with an explicit `utime`: sha matches, binary relinked, rc=0. ⇒ clause
5's mirror is not ceremony — **a restore can silently fail to reach the process exactly as a mutant
can**, and without the mirror this would have read as "the fix is load-bearing" when nothing had
changed at all.
★★ **M3 caught a gap in the test's own first draft:** an offset-only pin would PASS a fix that
aligned the CURSORS while the BYTES landed 4 earlier — a correct `symoff` published over the WRONG
table, which is precisely the silent miscompile the row is about. The test now reads the nlist back
THROUGH the published offsets. ★ Each cell asserts its own reachability (`nIndirect % 2 == 1`),
because an EVEN count leaves the packed cursor aligned BY LUCK and every assertion would hold
pre-fix. ⚠ And the reloc kind is resolved BY NAME: `RelocationKind{4}` is `abs64` on arm64 but
`tls-tpoff32` on x86_64, so the copied literal had silently made the x86_64 cells test NOTHING.
★ Codesign re-verified rather than assumed (`--strict --deep`, rc=0 both tiers, plus the CodeDirectory
arithmetic read directly); the `.o` tier proven untouched TWICE — Apple `cc` links and runs it, and
the emitted `.o` is **byte-identical** across the fix.
⚠ **ONE HONEST ASYMMETRY:** the x86_64 exec was ACCEPTED as a `-bundle_loader` while misaligned where
arm64 was refused, and `nm -m` proves that link really did read the misaligned table. **Apple's
refusal is not uniform across (arch, link-role) — 3 of 4 pre-fix probes refused, 1 not.** One
accepting tool proves nothing.

### ★★★ AND THE BIGGER NEWS: APPLE'S `ld` CANNOT LINK AGAINST A DSS MACH-O IMAGE AT ALL
`D-LINK-MACHO-LINKEDIT-SYMTAB-MISALIGNED` 🔴 OPEN, HIGH. The dynamic image packs `symtabOff` hard
against the preceding `__LINKEDIT` blob with no alignment step; `nlist_64` carries an 8-byte `n_value`
and Apple requires 8-byte alignment. ✔MEASURED: `symoff` = **50076 (≡ 4 mod 8)** on both the exec and
the dylib, **before and after** the name fix ⇒ pre-existing and orthogonal.
★★ **THE FIRST READING WAS WRONG AND WAS CORRECTED RATHER THAN QUIETLY REWRITTEN.** `dyld_info`
refusing the image looked like one fussy inspector — until **Apple's own `ld`** was asked to link
against the dylib and refused in the same words, `ld: mis-aligned LINKEDIT content 'symbol table'`, on
all four probes. ⇒ **a DSS-built Mach-O dylib cannot be consumed as a LINK INPUT by the Apple
toolchain.** Control that makes it ours: the same `dyld_info` reads `/usr/lib/libSystem.B.dylib` fine.
★ Blast radius bounded by measurement: the MH_OBJECT writer lays out its own LINKEDIT and is
UNAFFECTED — a DSS `.o` linked by Apple `cc` against an Apple-compiled `main` gave rc=0 and RAN, so
the c139–c142 relocatable arc is intact; dyld still LOADS and RUNS these images. ⇒ **an inspector
disagreeing with you is a hypothesis; the reference toolchain refusing you is the finding.**

### ✅ DSS BACKTRACES NOW NAME REAL FUNCTIONS — and the row's own stated blocker was FALSE
`D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS` ✅. gdb over a DSS ELF exec went from
`#0 sym_84 / #1 sym_89 / #2 sym_93` to `#0 static_helper / #1 global_helper / #2 main`, at
**BYTE-IDENTICAL addresses** — name-only — on x86_64 native and aarch64 under qemu's gdbstub. Naming
`main` also restores gdb's stop-at-main policy. **Three code lines**, via a new
`ObjectSymbolNames::imageName` that sits in the SAME owner as `definedName` and differs in exactly one
clause (no `isExternallyVisible` gate).
★★ **THE TIERS LEGITIMATELY DISAGREE, and that is now written down rather than incidental:** a `.o`'s
names ARE a foreign linker's resolution keys, so a real-named `static` collides across TUs; a FINAL
IMAGE is never re-linked, so its `.symtab` resolves NOTHING and is read only by debuggers — where the
real name is the wanted answer, and is what gcc emits. ★ **The `.so` proves the tiers stayed
separate:** in ONE binary `static_helper` is in `.symtab` and correctly ABSENT from `.dynsym`.
★★ **BOTH image builders were broken and the LIVE one was the DYNAMIC one** — every shipped exec/PIE
spells `processExit` as a by-name import of `exit`, so every real executable routes through
`encodeElfExecDynamic`; the static ET_EXEC arm needs zero externs. ⇒ **a pin on the "minimal ET_EXEC"
alone would have tested the DEAD arm.**
★★★ **THE ROW'S OWN STATED BLOCKER WAS MEASURABLY FALSE.** The code comment justified the synthetic
name by claiming entry resolution matched `entryPoint` against the reconstructed `.symtab` name.
✔MEASURED: `resolveEntryFnIdx` RE-DERIVES `<prefix><SymbolId>` from the id and **never opens the
symtab** — all four entry tests stay green, including one with `entryPoint = "sym_42"`, and every
shipped exec declares `entryPoint: ""` anyway. **A comment recording a coupling the code did not have
is what kept this defect alive for its whole life.** Same family as the `hwtime.h` blocker and the
inverted `__text` alignment finding: *the blocker was in the prose, not the code.*
✔3 mutants, 5 clauses + mirror; **M2's line count identical (4687/4687)** with a differing hash; M1/M2
are EXACT COMPLEMENTS so neither arm can mask the other; all 17 cells run through a `void` callable.
★★ **A RESTORE THAT CAN REVERT MORE THAN THE MUTANT IS NOT A RESTORE.** The lane's first driver
restored with `git checkout -- <file>`, which silently reverted **the uncommitted fix itself**, so the
mirror read rc=1. ⇒ snapshot the working file and restore FROM THE SNAPSHOT; never use a VCS-relative
restore while the subject is uncommitted.
⇒ Three follow-ons anchored, all measured against **gcc on the same source**: statics emit
`STB_GLOBAL` where gcc emits `LOCAL` (deliberately not bundled — it moves statics into a local-first
prefix and falsifies a hardcoded `firstNonLocal = 2`, which would have hidden a symtab-layout change
inside a change whose whole proof was byte-identical addresses); an image `.symtab` carries **no data
symbols at all**, so a debugger can name every frame but not one variable; and the Mach-O sibling,
**correctly left unfixed** — the fix is two lines and the shared `imageName` already exists, but the
bar requires witnessing by EXECUTION and the Mac is off, so landing it would be the speculative build
§A.2 forbids.
✅ And a fourth, **fixed on the spot**: `D-TEST-BUDGET-THREADING-PRIVATE-REPO-ROOT-WALK-FAILS-OUT-OF-SOURCE`
— the last private `repoRoot()` in the tree, walking from cwd only, so the SAME binary passed inside
the repo and failed from an out-of-repo build dir. **Third instance this cycle of the
already-fixed-next-door pattern:** `test_header_name_matching.cpp:471` sits two files away carrying
the identical fix and the identical written reason. ⇒ **a consolidation is finished when nothing
private survives, and only a SCAN tells you which of those you have.**

### ✅ THE TWO `asm goto` / PARAMETER-OUTPUT ROWS CLOSED — and a POLICY OWNED A FACT IT COULD NOT KNOW
`D-OPT-ASM-GOTO-WITH-OUTPUTS-ABORTS-THE-MIR-REBUILDER` ✅ + `D-CSUBSET-ASM-OUTPUT-ON-A-PARAMETER-NOT-ADDRESS-TAKEN` ✅.
★★★ ROOT CAUSE of the first: `MirRebuildPolicy::recordTerminatorInRewrite()` let a **pass declare**
that nothing downstream reads a terminator as an operand — and `Dce` and `SimplifyCfg` both declared
it. **The declaration is FALSE:** a `ReturnPiece`'s single operand ANCHORS IT TO ITS PRODUCER, and for
an `asm goto` WITH OUTPUTS that producer IS the block's terminator. ⇒ **whether an old id is
referenced is spelled out in the MIR's OPERAND LISTS the rebuilder is already walking — it is not a
fact a pass can own**, so the hook was **DELETED** rather than re-answered. Neither `false` reason
survived contact: Dce's was cost (one hash insert per block), SimplifyCfg's a hypothetical about a
folded `CondBr` whose recorded entry is in fact ACCURATE *and* unreachable. Withholding it converted a
speculative wrong-shape read into a guaranteed `std::abort`.
★★ **A SECOND CLONER, FOUND BY PROBING RATHER THAN BY READING:** with the substrate fixed, the same
shape inside an inlined `static` callee STILL exited 127 — `Inlining` splices callee bodies with its
OWN walk and its OWN `local` map, which a shared-substrate fix does not reach. ⇒ *fixing the substrate
does not fix the code that declined to use it.*
✔28 CLI compiles (7 shapes × 2 configs × 2 formats) all rc=1 on the same honest refusal, ZERO aborts
(pre-fix release rc was 127). 3 mutants, each rc **0 → 8 → 0**, mtimes moved BOTH directions; two had
**IDENTICAL line counts** (661=661, 1655=1655) — exactly what a line-count criterion misses. Under
every mutant the four PRE-EXISTING carriage tests stayed GREEN; only the new anchor pins caught it.
★★ **THE LANE CORRECTED ITS OWN ROW'S CONTROL, and that is the third instance of the shared-tree
defect** — the row claimed `"+r"` on a parameter "compiles and exits 42"; re-measured at pristine HEAD
with `DSS_CONFIG_ROOT` pinned to a clean worktree it is REFUSED, because the original figure had been
taken against a build carrying **another lane's uncommitted `mir_to_lir.cpp`**. ⇒ **a shared tree's
damage outlives the session: a wrong build makes a wrong number, and the number gets written into a
row as ✔MEASURED.** Pin `DSS_CONFIG_ROOT` for any figure that will be WRITTEN DOWN.
⚠ **STILL UNMEASURED, and the lane retracted the claim rather than let it stand:** whether `"+r"` on a
parameter now COMPILES with Lane L's `tieAsmReadWriteOperands` in scope. It measured parity only in
its own worktree at pristine HEAD. **Re-measure against the merged tree before treating
`D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE` as reachable.** ✅ **THE PROBE RAN — THE COMPOSITION HOLDS, ✔MEASURED 2026-08-17 on the merged
tree:** a tied `"+r"` output bound to a **non-address-taken parameter** now COMPILES **and RUNS, exit
42, at BOTH `debug` and `release`** on pe64-x86_64 (`static int bump(int v){ __asm__("addl $20, %0" :
"+r"(v)); return v; }` over a `volatile` seed of 22). So Lane L's `tieAsmReadWriteOperands` and Lane
M's address-taken marking compose, and [[D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE]] is already ✅ CLOSED by
Lane L. ★ **The lane was RIGHT to retract rather than assert it** — it had measured only its own
worktree at pristine HEAD, where the refusal genuinely fires; the claim happened to be true, and it
still could not have known that. **A correct guess withdrawn is worth more than a correct guess
kept**, because the next one would not have been true.
⚠ **AND ITS GATE DID NOT TERMINATE — reported as unmeasured, which is the standard to copy.** An
earlier run of its did reach 875/875 and is **VOID**: it ran `ninja` mid-ctest, producing ~400
spurious `***Exception` lines and one invented failure (`double_to_unsigned`, green on a clean re-run).
⇒ **an overlapped ctest run can invent a failure, so it can equally mask one — void in BOTH
directions.** Second mechanism from the same incident: **killing the wrapper does not kill `ctest`**,
which kept running and writing. Both now recorded in `references/gate-and-cross-plan.md`.

### ★★★ A DEFECT FOUND TWICE BY HAND IS EVIDENCE OF A POPULATION, NOT OF A PAIR — 2 became 61
`D-TEST-SEMANTIC-FIXTURE-ABORTS-THE-WHOLE-BINARY` ✅ CLOSED: the semantic fixture did
`ADD_FAILURE()` then **`std::abort()`**, which kills the whole test PROCESS — every sibling test in
that executable loses its verdict. ✔MEASURED, not hypothetical: a config-mutating pin drove
`loadShipped` to a **legitimate** refusal and the binary died `0xc0000409` mid-suite, **taking nine
passing tests' results with it** and reporting an exception code instead of the load error. ⇒ **the
arm that was working correctly is the one that destroyed the evidence.**
★★ **The project had already made this exact fix one layer down and written down why**
(`tests/test_support/repo_root.hpp:59` — *"`std::abort()` kills the whole test BINARY … `repoRoot()`
throws instead"*), and it did not propagate. **A recorded lesson is not a fixed defect: nothing makes
a call site read a neighbour's comment.**
⇒ So it got a guard: **`scripts/check-no-abort-in-tests/check-no-abort-in-tests.py`**, wired into the gate battery, 14 stripper
self-tests + a line-number-preservation test. ★★ It strips comments/strings/raw-strings BEFORE
matching because **a bare token grep reds on the very file that documents the fix** — the same
merely-present-marker shape as the retired-id check above.
★★★ **THE SCOPE IS THE RESULT: the row named 2 sites; the first scan found 61 LIVE SITES ACROSS 29
FILES** — the identical `ADD_FAILURE(); std::abort();` idiom copy-pasted (worst:
`tests/hir/test_hir_lowering_c_subset.cpp`, **11**). Shipped as a **RATCHET** (per-file ceilings that
may only come DOWN; a new site reds, and a *fixed* site also reds until its ceiling drops, because
unclaimed headroom is where the next regression hides). ⚠ **`INVENTORY` is deliberately NOT
`ALLOWLIST`** — an allowlist claims *aborting here is right*; these 61 claim only *unfixed debt
predating the guard*. Merging them would launder 61 unexamined sites as 61 proofs. `ALLOWLIST` is
empty and that is measured. ✔4 arms, all discriminate. **`D-TEST-ABORT-IN-A-FIXTURE-HAS-NO-GUARD`
stays OPEN until the inventory is empty — a guard existing is not the debt being paid.** ⛔ The
61-site sweep was NOT attempted this cycle, and the reason is recorded rather than implied: three
lanes held `test_mir_lowering_c_subset.cpp` (5), `lowered_lir_fixture.hpp` (3) and the semantic tree,
and sweeping files under concurrent edit is how a fold loses somebody's work.

### ⚠ THE RELEASE-ARM GAP IS **184**, MEASURED — and it is the §B, not a shrug
✔MEASURED 2026-08-17: of **577 runnable** manifests (24 more are diagnostic-only), **393 carry a
shipped `release` arm**, **86 carry arms but no release arm**, and **98 carry no arms at all** ⇒
**184 runnable examples witness the front end and codegen and say NOTHING about the optimizer.** The
project's own rule in `examples/README.md` already mandates the arm; these manifests simply predate
it. ⇒ `D-EXAMPLES-OPTIMIZER-WITNESS-IS-A-HAND-LISTED-PASS-SUBSET`.
★ **Why it was NOT bundled into this cycle, and this is an attribution argument rather than a
capacity one:** 184 new arms is a large new validation surface, and any red would be indistinguishable
from a red caused by this cycle's asm/CFI/config work. **Control the variables** — it deserves its own
cycle and its own investigation budget, because a release-only failure among those 184 is exactly the
class this project most wants to find (`D-OPT-VARIADIC-RELEASE-MISCOMPILE` shipped that way).

### ★★ OPERATOR INSTRUCTION 2026-08-17 — ONE BUILD ROOT, AND LANE BUILDS GET CLEARED
Verbatim: *"EVERY build must be inside build directory (we can have multiple subdirectories for
distinct builds), but only 1 root build. Also, lane builds MUST be cleared once everything is fine,
so we keep the storage good and also organize build directories."* ⇒ written up as
**`.claude/skills/dss-cycle/references/build-layout.md`** and wired into that skill's file map (read
at step 5 before creating a build tree, and at step 11 before reporting a cycle complete).
★★★ **The argument is not tidiness — a flat `build-*` namespace has ALREADY shipped a bug here, this
cycle:** an rsync exclude written unanchored as `build*` silently matched
`src/program/build_scripts.cpp`, so the WSL leg configured against a tree missing a changed `.cpp`.
With ONE root the exclude is `/build/` — one anchored path, no glob, nothing for a SOURCE file named
`build_*` to collide with ⇒ **the class disappears instead of being re-fixed in each rsync, script,
doc, `.gitignore`, and the `dss-state` driver's auto-pick.** ✔MEASURED storage half: **11 root build
trees / 54.3 GiB**, seven of them one-cycle lane builds at ~5.6 GiB each — **27 GiB reclaimed on the
spot**, plus 16 GiB from two folded worktrees (`dss-lane-l`, verified by CONTAINMENT of its
contribution, not by byte-identity — later edits legitimately stack on top).
⚠ **NOT MIGRATED YET, and it is not a `mv`:** `build-dbg` is named in **47** files and `build-rel` in
**16** (scripts, CI, docs, `dss-state`'s driver) ⇒ `D-BUILD-LAYOUT-FLAT-ROOT-BUILD-DIRS-NOT-MIGRATED`,
to be sequenced for a QUIET tree because it edits the very scripts the gate runs. A missed reference
fails in the worst available way: a script that silently configures a new EMPTY build tree and reports
a pass over a scan of almost nothing. ⛔ Two worktrees (`dss-wt-bitwise`, `dss-wt-movzw`, ~25.7 GiB)
hold **118 and 124 uncommitted paths** at old commits and were deliberately NOT deleted — that is an
operator call, not a cleanup.

### ⚠ `examples/README.md` WAS STALE IN THREE PLACES, AND ONE WAS INVERTED INTO HARMFUL ADVICE
✔RE-MEASURED 2026-08-17: the optimizer-arm figures read **460/652/374/278**; the truth is
**477 of 600 manifests / 669 arms / 391 release / 278 inline**. Three of four stale — and `278`
survived only because **every arm added in between was a `release` arm**, which is exactly the
coincidence that makes a stale count look verified. The prose claimed `optimizedPipelines` and
`expectedStdout` are read by the in-process runner **ONLY**; this cycle taught the CLI runner both,
so the paragraph understated coverage. ★★ **Worst of the three: rule 3 told authors NOT to give a
project-mode example a release arm**, reasoning from that same false premise. ✔MEASURED at both
sites, the prohibition is dead: `--config` is a GLOBAL CLI flag (so the CLI runner applies an arm to
a `--project` build) and the in-process pipeline override is `Program` state read by the delegated
`compileFiles`/`compileUnits` (so it survives `compileProject`). ⇒ A project-mode example without a
release arm now witnesses the optimizer in **NEITHER** runner. **The corpus's ONE project-mode
example had 0 arms and its own `$comment` named its own trigger — *"add the release arm the day the
CLI runner grows `--config`"*. That day arrived this cycle; the arm is in.** ⇒ **A manifest comment
that states its own firing condition is a scheduled task, and nothing was scheduled to read it.**

---

## 0.000 ★★★ P5c — A SILENT MISCOMPILE IN SHIPPED EXTENDED ASM: EVERY UNPINNED INPUT READ AN UNDEFINED REGISTER

**✔MEASURED 2026-08-17 on the compiler that had just passed 873/873.**
`__asm__("movl %1, %0" : "=r"(r) : "r"(a))` with `a == 42` compiled **rc=0** and the program
**returned 0** — on **BOTH** `pe64-x86_64` and `elf64-x86_64`, at **BOTH** debug and release.
The disassembly named it outright: the input's load defined the register the OUTPUT had been
allocated, and the template's `%1` read one nothing ever wrote — `mov 0x0(%r14),%r14d` (load `a`),
then `mov %r15d,%r14d` (the template), with r15 untouched since the prologue.

**Cause (`expandInlineAsm`, `src/lir/lowering/mir_to_lir.cpp`):** the materialisation loop opened
`if (!ins[j].pinned) continue;`, so only PINNED inputs were moved into their bound register.
`bindAsmOperand` mints a FRESH vreg for an unpinned operand and, for an INPUT, **nothing else ever
writes it.**
★★ **THE FALSE SYMMETRY IS THE LESSON.** The capture loop twelve lines below skips unpinned OUTPUTS
for a correct reason its own comment states — *the template wrote that vreg, so a copy would be dead*
— and the input loop reads as its mirror image. It is not one: an output is written by the TEMPLATE,
an input must be written by the LOWERING.
★ **A second defect hid underneath and only surfaced once the first was fixed:** `needMoves` gated
resolution of `MnemonicSlot::Mov` on *any operand pinned*, so with unpinned inputs alone `movOp`
stayed disengaged and the corrected loop dereferenced an empty optional
(`LirBuilder::addInst: Invalid opcode`). One omission concealed both halves.

**✔MEASURED FIXED:** the four probes (`movl $42,%0` / `movl %1,%0` / two-input `"=&r"` / arm64
`add %0,%1,%2` on `long`) all exit **42**, debug AND release, on pe64 native, WSL elf64-x86_64 and
qemu-aarch64. Anchor `D-LIR-ASM-UNPINNED-INPUT-NEVER-MATERIALISED`, born ✅ CLOSED (balance net 0).

### ★★★ WHY 873 GREEN TESTS SAW NOTHING — the durable finding
The corpus's **three** inline-asm examples declared **ZERO input operands between them**:
`c_inline_asm` is the EMPTY template; `c_inline_asm_extended` is register-PINNED OUTPUTS (`rdtsc`)
on x86_64 and a pure CLOBBER list on aarch64. ⇒ **A FEATURE'S COVERAGE IS AS WIDE AS THE OPERAND
SHAPES ITS TESTS NAME, NEVER AS THE NUMBER OF TESTS.** Counting examples said inline asm was well
covered; counting SHAPES said inputs had never once run.
⚠ It also re-reads the cycle that shipped it: `hwtime.h` compiling was a TRUE result *because*
`rdtsc` has outputs and no inputs — the motivating construct could not have caught this.

### ★★ AND THE PIN ITSELF NEARLY SHIPPED A FALSE CLAIM
New example `examples/c-subset/c_inline_asm_operands`. I first wrote that it *"discriminates at both
arms, because a register nothing wrote is undefined at every optimization level"* — reasoning that
sounds airtight and is **WRONG**. ✔MEASURED with the mutant restored and the example reduced to the
call-shaped two-input helper alone: **baseline exited 42 (GREEN — the mutant SURVIVED)**, release
exited 1. Adding a single-input shape lowered **directly in `main`** made the same mutant fail the
BASELINE arm outright. ⇒ a pin that survives because an undefined register HAPPENED to hold the right
value is not a pin, and which shape gets that luck cannot be read off the source. The example
carries **both** shapes and must not be "simplified" to one.
⚠ The original claim came from a standalone probe of a DIFFERENT program shape — a property measured
on one subject and asserted about another.

### 0.001 ✅ THE TWO OPERATOR-QUEUED TASKS (§0.00) ARE DONE IN THIS SAME CYCLE

**TASK 1 — asm output store-back through `emitScalarStore`.** ✔**IT WAS A LIVE VERIFIER FAILURE, not
only a quality gap** — the fixture was written FIRST to settle exactly that, as §0.00 instructed:
`_Atomic int g; __asm__("movl $42, %0" : "=r"(g));` exited **rc=1** with
`error[I_AtomicAccessNotLowered] … plain 'store' to an _Atomic-qualified pointee`. Valid C, refused.
The `volatile` half was the SILENT one: it compiled, ran, and simply lost the flag.
Both sites now call `emitScalarStore(st, pieceTypeFor(k), kids[k])`; the second pass stays a second
pass (the producer→`ReturnPiece` adjacency window must still close first).
Pin `MirLoweringCSubset.AsmOutputStoreBackGoesThroughTheScalarFunnel` — volatile arm, `_Atomic` arm
(AtomicStore == 1 **and** plain Store == 0), and a **plain control** proving the lowering does not
stamp volatile on everything. ✔RED-ON-DISABLE: reverting one call reds all three arms, control green.
Anchor `D-MIR-ASM-OUTPUT-STORE-BACK-BYPASSES-THE-SCALAR-FUNNEL`, born ✅ CLOSED.

**TASK 2 — pin `isExtended` + `tiedOutput` through the MIR rebuild.** Sentinel sets both to
NON-DEFAULT values (`tiedOutput` names output **1**, so a drop re-defaulting to `0` stays visible);
`expectSentinel` asserts engagement with `ASSERT_TRUE(...has_value())` before the value, since
`.value()` on a dropped optional aborts instead of naming the site. ⚠ Adding an input made the
descriptor's input count non-zero, and `MirBuilder::checkAsmOperandAlignment_` **aborts** on a
descriptor/operand mismatch — so all three fixtures plant the asm through one `plantSentinelAsm`
helper instead of three call sites that can drift. ✔RED-ON-DISABLE: a field-by-field copy at rebuild
site 1 omitting exactly those two fields reds both new assertions; reverted.
Anchor `D-TEST-MIR-ASM-DESCRIPTOR-NEW-FIELDS-UNPINNED-THROUGH-REBUILD`, born ✅ CLOSED.

### 0.002 GATE STATE — ✅ ALL THREE LEGS GREEN ON THE FULL DIFF
✔ **Windows ctest 874/874, rc=0** · ✔ **WSL x86_64 + qemu-arm64 874/874, rc=0 under
`DSS_STRICT_ARM_VERDICTS=1`** · ✔ **`integrated_tests` (the CLI-subprocess runner) passed, and the new
example is genuinely IN it** — verified by name in `ctest -V`, not inferred from the glob, because a
capability that reaches one runner while its sibling shrugs is a silent harness bug.
Anchor balance **net 0** (three rows, all born ✅ CLOSED); registry guard OK (1047 src anchors
resolve); line-endings OK.
⚠ **An instrument bug worth carrying:** the WSL sync used `rsync --exclude 'build*'`, which is
UNANCHORED and therefore matched `src/program/build_scripts.cpp` — the mirror failed to configure with
*"Cannot find source file"*, which reads like a missing-file bug in the tree and is not one. Anchor
excludes with a leading slash (`/build*`).

### 0.003 ✅ RESOLVED 2026-08-17 — `--scanstatus` IS ON. IT WAS NOT A FORK.
I framed this as a three-way operator decision and was wrong to: one measurement collapsed it.
✔MEASURED at HEAD, the verbatim upstream construct (`__inline__ sqlite_uint64 sqlite3Hwtime(void)`
containing `__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi))`, plus a caller) **compiles at
`--config=release` on BOTH `pe64-x86_64` and `elf64-x86_64`, and the pe64 artifact runs, exit 42**;
at `debug` it fails `K_SymbolUndefined`, which is the conformant answer.
★ **The fact that dissolved the fork: `build-and-test.sh` sets `DSS_CONFIG="${DSS_CONFIG:-release}"`
and its own comment calls that load-bearing rather than a speed choice** — so the configuration this
harness actually exercises is the one in which `hwtime.h` links. There was no trade-off to bring;
asking would have billed the operator for a question a two-minute probe answers.
⇒ `--scanstatus` added to `configureFlags`, `SQLITE_ENABLE_STMT_SCANSTATUS` added to
`requiredDefines` (the fail-loud link that proves the define reached the compiler), and the stale
`$scanstatusComment` rewritten with the original kept as the record. `harness/test_sqlite_harness_legs`
passes with the new declaration. ⚠ **NOT YET PROVEN BY A FULL LEG RUN** — that is the cycle's sqlite
re-probe and it is still owed; until it runs, this is "declared and self-tested", not "green".
⚠ No `capabilityWitnesses` row was added and the omission is deliberate: a witness needs exactly one
decisive gate in its preamble, vetting that needs the upstream tree (cloned at run time, not
vendored), and an unvetted witness is how `mem5` and `analyze3` went red for reasons their capability
could not fix.

<details><summary>The 2026-08-17 brief that framed it as a decision, kept as the record</summary>

#### ⚠ A LOOSE END P5b LEFT, SURFACED NOT SILENTLY DECIDED — `--scanstatus` IS STILL OFF
`real-examples/c/sqlite/legs.json`'s `$scanstatusComment` instructs, in its own words, to *"re-add
the flag in the change that closes it"*. ✔MEASURED: **every blocker that comment names is now
closed** — `__volatile__`, the `:` operand lists, the asm TEXT, and (P5b) the `__inline__`-only
external-definition gap, after which `hwtime.h` compiles and runs at release on pe64,
elf64-x86_64 and macho64-x86_64. **The flag was not re-added, and the comment still concludes it
stays off for the `__inline__` reason it no longer has.**
⚠ **WHY THIS IS A DECISION AND NOT A CHORE:** `hwtime.h` still fails at **debug**, and that failure
is CONFORMANT — gcc 13.3.0, clang 18.1.3 and clang 19 all fail to link a called inline definition at
`-O0` and succeed from `-O1`. So re-adding `--scanstatus` buys sqlite's own `build(Default)`
configuration and its two corpus files, at the cost of a debug leg that goes red for a reason the
reference compilers share. ⇒ **Decide it explicitly (re-add release-only / re-add and accept the
debug red / keep off and correct the stale comment) — do not let it drift by inheriting a paragraph
that is now false.** 🧠 Not attempted this cycle: it is scope the operator has not asked for, and
guessing the answer would bake a policy into a config comment.

</details>

**STILL OWED on this cycle:** re-run both gate legs, commit + push.
🧠 **sqlite re-probe judged not proportionate here and the reasoning is stated rather than assumed:**
the whole diff touches the inline-asm lowering path plus one test, and `--scanstatus` is OFF, so no
sqlite TU in this configuration contains an `__asm__` at all. The in-suite
`harness/test_sqlite_harness_legs` runs as part of the 874. ⚠ If `--scanstatus` is ever re-enabled
(§0.003) that reasoning expires immediately — the asm path becomes live in sqlite and a full re-probe
becomes mandatory.
📄 PRs **#50, #51 and #52 are all MERGED**; this branch was cut clean from main. ⚠ The public-repo bot
rebases/squash-merges, so `4969e9e2` / `e5b60f6c` / `e42ae5a5` (the asm cycles) are **NOT ancestors of
HEAD** — their *content* is in main, their SHAs are not reachable. Do not `git show` them and conclude
the work is missing.

---

## 0. ★★★ P5 IS DELIVERED — GNU EXTENDED INLINE ASM COMPILES, LINKS AND RUNS

**✔MEASURED 2026-08-14/15.** `examples/c-subset/c_inline_asm_extended` exits **42** on **pe64 native,
WSL elf64-x86_64 and qemu-aarch64**, in **debug AND `--config=release`**, registered in **BOTH**
examples runners (in-process `tests/examples/` + CLI-subprocess `integrated_tests/`).
★★★ **The negative miscompile pin DISCRIMINATES:** eight values from `dssOp()` **CALLS** held live
across an `__asm__` clobbering `x21`–`x28`; delete the clobber list, rest byte-identical ⇒ **release
exits 1 instead of 42.** ⚠ Debug does NOT discriminate (locals are memory-resident pre-mem2reg) —
**the `release` arm is load-bearing, not decorative.**
✔ Conformance census genuinely AGREES with all four gnu oracles; `@acknowledged-gap` deleted.
⚠⚠ **AND EARLIER IN THE SAME CYCLE THAT CENSUS READ A SILENT MISCOMPILE AS CONFORMANCE** — with
capture landed but HIR→MIR still emitting a barrier, a clobber-bearing `__asm__` compiled rc=0 clean
and `objdump` showed it emitting **ZERO instructions**. *An oracle that only checks accept/reject
cannot tell "works" from "does nothing".*

**Anchors CLOSED this cycle (9):** `D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED` ·
`D-CSUBSET-INLINE-ASM-OPERANDS` · `D-CSUBSET-INLINE-ASM-GOTO` · `D-CSUBSET-INLINE-ASM-TEXT` ·
`D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED` · `D-ASM-ZERO-OPERAND-PLAIN-INSTRUCTION-UNLOWERABLE` ·
`D-ASM-ARM64-SETCC-W-FORM-UNDECLARED` · `D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE` ·
`D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED` (a **FALSE CLOSE** corrected — it had been witnessed
only on `eq`/`ne`, the two spellings where the substrate and gas vocabularies coincide).

### 0.00 ✅ OPERATOR-QUEUED CYCLE 2026-08-15 — BOTH TASKS DONE 2026-08-17 (see §0.001). Brief kept below.
⚠ **Sequencing, measured (now historical — the lane landed):** the in-flight lane owns `src/mir/lowering/hir_to_mir.cpp`, `src/mir/mir_asm_descriptor.{hpp,cpp}` and `tests/mir/**`, **and it is the lane ADDING the two fields task 2 pins**. Task 1 collides outright; task 2's subject does not exist until it lands. **Dispatch both only after that lane reports.**

**TASK 1 — route asm output store-back through `emitScalarStore` (a REAL silent defect).**
`Lowerer::lowerInlineAsm` stores each inline-asm OUTPUT back through its lvalue address with a bare
`mir.addInst(MirOpcode::Store, st)` at **TWO** sites — the `asm goto` successor-head path
(`std::array<MirInstId, 2> st{rp, outAddrs[k]};`) and the non-terminator path
(`std::array<MirInstId, 2> st{pieceVals[k], outAddrs[k]};`). Both **bypass `Lowerer::emitScalarStore`**
(same file, ~`:594`), the documented funnel that (a) routes an `_Atomic`-qualified lvalue to
`MirOpcode::AtomicStore` with `kAtomicOrderSeqCst` and (b) stamps the c21 `MirInstFlags::Volatile`
from `volatileFlagFor(node) | volatileFlagForType(accessedTy)`.
⇒ **Two silent consequences:** `volatile int x; __asm__("…" : "=r"(x));` loses the Volatile flag on
the write-back, so the optimizer may elide or reorder a store the source marked volatile; and
`_Atomic int x; …` emits a **plain Store on an atomic object**. ★ `emitScalarLoad`'s own docblock says
a missed funnel site is caught **LOUD** by the MIR verifier's atomic belt (`I_AtomicAccessNotLowered`),
so this is **likely a LIVE verifier failure rather than only a quality gap — confirm which by writing
the fixture FIRST.**
⚠ **The two halves of one operand are already asymmetric:** the READ half of a `"+r"` operand (added
2026-08-15) already goes through `emitScalarLoad`.
**Fix:** call `emitScalarStore(st, pieceTypeFor(k), kids[k])` at both sites instead of `addInst`.
⚠ **Keep the ordering** — the non-terminator path emits its stores in a SECOND pass, deliberately
after the producer→`ReturnPiece` adjacency window closes.
**Tests** in `tests/mir/test_mir_lowering_c_subset.cpp` (its `lowerCSubset` harness now threads both
the target schema and `hir->inlineAsmPool`, so a descriptor-carrying `__asm__` reaches MIR): assert
the volatile fixture's store-back carries the Volatile flag and the `_Atomic` fixture lowers to
`AtomicStore`. Red-on-disable by reverting each call individually and re-running through `ctest`.

**TASK 2 — pin the two new `MirAsmDescriptor` fields through the MIR rebuild (test hardening).**
`MirAsmDescriptor` gained `bool isExtended` (the BASIC/EXTENDED surface, carried because `:::` with
every section empty is **unreconstructable** downstream) and `std::optional<std::uint32_t> tiedOutput`
on `MirAsmOperand` (set on the synthesized INPUT entry carrying a `"+r"` operand's read half, naming
the output it shares a location with). `tests/opt/test_inline_asm_rebuild_carriage.cpp` pins that the
descriptor survives the rebuild passes, but its `sentinelDescriptor()`/`expectSentinel()` pair sets and
checks templateText, isVolatile, outputs, inputs, clobbers and the two clobber flags — **not the two
new fields**.
ⓘ **Today this cannot drop anything**: every rebuild site passes `src.asmDescriptor(id)` **WHOLE by
value**, so new members ride along. The exposure is a **future refactor to a field-by-field copy** —
precisely the silent-drop class `mir_asm_descriptor.hpp`'s own docblock guards (*"A POOL INDEX IS NOT
SELF-CARRYING, AND THAT IS THE WHOLE HAZARD"*).
**Fix:** extend `sentinelDescriptor()` to set `isExtended = true` and give one input a `tiedOutput`;
extend `expectSentinel()` to assert both. **Red-on-disable by making ONE rebuild site copy the
descriptor field-by-field, omitting the two new fields**, confirming the pin reds through `ctest`, then
reverting.

### 0.0 ✘ STALE — ALL FOUR OF THESE CLOSED IN CYCLE P5b (`4095c13b`). KEPT ONLY AS THE RECORD.
⛔ **Do not plan off this list.** ✔MEASURED 2026-08-17 that items 1–3 are done and item 4 shipped
(`hwtime.h` compiles and runs at release). The **current** open asm set is:
`D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER` — **narrowed to the LABEL half only**; `%N`, `%%` and
aarch64 outputs all measured working, `%l[name]` still fail-loud refused ·
`D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE` (`"+r"` carriage lands, `bindAsmOperand` still refuses it) ·
`D-CSUBSET-INLINE-ASM-POSITIONAL-LABEL-REF-ACCEPTED-WITH-NO-GRAMMAR` ·
`D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER` ·
`D-ASM-TEMPLATE-DIAGNOSTICS-RENDER-WITHOUT-SOURCE-CONTEXT` · `D-CSUBSET-INLINE-ASM-SPELLING` (bare
`asm`) · `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` ·
`D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL` · `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED` ·
`D-ASM-ARM64-GAS-SURFACE-INCOMPLETE` · `D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT`
(BLOCKING trigger). ⚠ Still trigger-gated and **meant to stay open**:
`D-ASM-TARGET-DECLARES-NO-BYTE-ORDER`, `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED`,
`D-ASM-SYSTEM-REGISTER-AS-ENCODED-DATA-UNMODELLED`.

<details><summary>The 2026-08-15 mandate, kept as the record</summary>

#### ⚠ THE NEXT CYCLE'S MANDATE — four blockers, operator-scheduled 2026-08-15
Balance ends **+6 by an explicit §B decision** (*"close everything already closed, commit, then
another cycle for the new blockers — I WANT ASM FULLY 100% DELIVERED"*). Not drift: this cycle
FOUND more than it fixed because it was a deep investigation. **Close these next:**
1. ★ **`D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER`** (HIGH) — the big one. No dialect declares
   `%N`/`%[name]`/`%l[label]`, nor a shape for the declared `PercentEscape`. Blocks `%%eax`, EVERY
   aarch64 asm OUTPUT (so `mrs %0, cntvct_el0` is unspellable), and `asm goto` WITH labels.
   ⚠ **NOT a sibling-alt addition** — `detectAmbiguousAlternatives` refuses two alts sharing a FIRST
   token and `%` is already `RegisterSigil`/`TypeSigil` (EXERCISED, two tests watch it refuse). Use
   the shipped `lexerModeTokens` per-mode override + an `assembly.templateLexerMode` key.
2. **`D-LIR-EARLYCLOBBER-FLAG-UNSETTABLE-AFTER-EMISSION`** (HIGH) — **one method**: a
   `LirBuilder::setInstFlags` sibling to `setInstRegConstraints`. Both ends already exist.
3. **`D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE`** — `"+r"`; replace the `requires2Address` bool with an
   operand INDEX at four literal-`0` sites. Core fix; every two-address target benefits.
4. **`D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED`** — what ACTUALLY blocks sqlite
   `hwtime.h`. ⚠ **NOT an asm defect**: the asm half is byte-proven (`0f 31 rdtsc`) and builds on all
   five legs. gcc **`-O0` fails identically**, so DSS's debug behaviour is CONFORMANT and the residue
   is our release inliner. Sole blocker on re-enabling `--scanstatus`.
Also open: `D-LIR-VERIFY-VREG-CLASS-RULE-ASSUMES-A-ONE-TO-ONE-LIR-TO-MIR-MAP` and
`D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED`; plus trigger-gated
`D-ASM-SYSTEM-REGISTER-AS-ENCODED-DATA-UNMODELLED` which is MEANT to stay open.

</details>

<details><summary>§0 as written mid-cycle, kept as the record</summary>

## 0.9 ⚠⚠ (SUPERSEDED) A CYCLE IS IN FLIGHT AND NOTHING BELOW §1 IS COMMITTED

**Cycle: inline-asm P5 — embedded `__asm__` in C.** Started 2026-08-14, **UNCOMMITTED**.
✔MEASURED 2026-08-14 (second session, after a context exhaustion): **41 modified + 4 untracked**
files, **+4722 / −238**. **The tree BUILDS: `cmake --build build-dbg` rc=0, 520 steps.**
If you are picking this up cold: the work is real but unlanded. **Run the baseline yourself.**

**Baseline** ✔MEASURED at `d4c2836b` before any edit: build-dbg 592 steps rc=0 · ctest
**860/860** · anchors **982 OPEN** (registry 657 + plans 325).
**Re-measured mid-flight on the dirty tree:** ctest **862 / 863**, rc **8**. Sole failure =
`anchor_registry_guard`, EXPECTED (see "OWED" below). Anchor balance still **982 → 982, net 0**.
⚠ **Never pipe `scripts/run-gate/run-gate.sh` into `tail`** — the harness then reports the PIPE's rc. It said
"exit code 0" over that rc=8 run. The script logged `rc : 8` correctly; the pipe hid it. Read the log.

### 0.1 ★★★ OPERATOR RULINGS TAKEN 2026-08-14 — full text in [plan 29](29-inline-asm-plan%20-%20tbd.md) §4.4/§4.5/§4.6. DO NOT RE-LITIGATE.
1. **Asm outputs are SSA values carried as pieces — REUSING `ReturnPiece` ITSELF. ZERO new
   value-producing opcodes.** ⛔ No `AsmOutputPiece`. `InlineAsm` takes `Call`'s ROW SHAPE — but
   ✔MEASURED `{0,N}`, **not** Call's `{1,N}` (Call's minimum operand is the callee; an asm block may
   have zero inputs). ⛔ Rejected: outputs as memory operands — it makes the C local address-taken,
   degrading SROA and the shipped LICM on exactly the hot paths asm exists for.
   ★ THE ONE-FACT TEST: two opcodes would encode ONE fact, so every consumer carries both arms
   forever and the next multi-result producer mints a third. **A CONSTRUCT-private verb breaks
   agnosticism exactly as a LANGUAGE-private one does.**
2. **Promote "where do my result pieces live" to a PRODUCER-DECLARED property** (Call → the cc's
   return regs, unchanged; InlineAsm → the constraint-bound regs). Key on DERIVABILITY, never on
   producer identity. ★ **HARD GATE: with the property landed and no asm in the source, x86_64 +
   arm64 + pe64 output must be BYTE-IDENTICAL. If not, STOP — do NOT re-baseline goldens.**
3. **Rename `ReturnPiece`/`ret_piece` → `ResultPiece`/`result_piece`, ONE mechanical commit** kept
   separate from the semantic work. Reaches the shipped target JSONs + `lir_text` goldens; the churn
   is at its global minimum before the P5 corpus exists and rises monotonically after.
4. **`asm goto` WITH outputs LANDS THIS CYCLE.** ⚠⚠ The operator ruled this AGAINST their own
   written refusal argument — I asked because the brief's §6 said "does NOT land" while the button
   said "build it". **The button stands.** ⇒ build the edge-placement rule: pieces at the head of
   every successor block, splitting critical edges. [[D-CSUBSET-INLINE-ASM-GOTO]] **CLOSES**; no
   sibling row. (The refusal argument is preserved in plan 29 §4.5 so a later cycle does not "fix"
   this back — its premise is real: `lir_callconv.cpp:2255`/`:2522-2529` require a piece to
   IMMEDIATELY follow its producer and a terminator has nothing after it.)
5. **`%N` binding is STRUCTURAL** — a dialect-declared placeholder resolving to the operand's VREG at
   expansion. ⇒ **no post-regalloc binding pass is needed at all** (an earlier phrasing said
   otherwise and was wrong). ⛔ Rejected: rendering operand text and re-lexing — the renderer would
   restate sigils the dialect token table already declares for parsing.

### 0.2 ★★★ THE §8 CONTINGENCY FIRED — `ReturnPiece`'s payload carries TWO facts, only ONE stored
The ruling pre-authorised the fix, so this is NOT a new fork. ✔MEASURED: `addReturnPiece(call,
ordinal, pieceType, …)` (`mir.hpp:534`) takes **no class parameter**; `hir_to_mir.cpp:7604-7614` runs
separate `gprRet`/`fprRet` counters, picks the ordinal from the piece's class, then **DISCARDS the
class**, re-encoding it as a TYPE (`:7272-7287`) — its own comment (`:7271`) says *"The piece's
register CLASS follows from the type."* Consumption is `returnReg(schema, cc, rpRes.regClass(),
payload, …)` where **the class selects the register POOL and the ordinal indexes it**
(`lir_callconv.cpp:1520`). ⇒ `"=x"` on an integer would index the **wrong pool, SILENTLY**, GPR being
the else-branch default. **Split the payload so the class is carried explicitly. Core fix, no new opcode.**

⚠ **LATENT SHIPPED BUG found in the same read — fix it, do not walk past it:** `cc.returnVrs` exists
(`target_schema.hpp:558`) but `returnReg` **never reads it** — `lir_callconv.cpp:1520` is a two-way
`(cls == FPR) ? returnFprs : returnGprs`, so **a VR-class piece silently takes the GPR branch.**

### 0.3 ✔MEASURED — the four constraint forms vs the core (full table: plan 29 §4.4.4)
| form | verdict |
|---|---|
| `"=r"` / `"=x"` | ✅ **EXISTS, no core change.** `newVReg(LirRegClass)` (`lir.hpp:216`) is the ONLY creation API and **40+ shipped sites already pass a class independent of the MIR type**. ⚠ Tripwire: `checkVregClassMatchesMirType` (`lir_verifier.cpp:397-436`) needs a 4th skip arm for `"=x"` — and it matters NOW because this cycle wires the verifiers into production. |
| `"+r"` | 🟠 **CORE GAP.** `requires2Address` is a per-opcode **bool** hardwired to *result == operand[0]*, `0` a literal at FOUR sites (`lir_2addr_legalize.cpp:133-149`, `:151-156`, `:179-185`, `lir_regalloc.cpp:1026-1039`). Only ONE result slot exists (`lir_node.hpp:340`). ⇒ replace the bool with an operand INDEX defaulting to 0; every two-address target benefits. |
| `"=&r"` | 🔴 **ABSENT — nothing exists** in `src/` or either shipped target. ★ Carrier settled by measurement: `LirInst::flags` is a `uint8_t` with only `0x01/0x02/0x04` used ⇒ **5 free bits** — and `flags` is threaded through **all 8** rebuild sites, so it survives rebuilds BY CONSTRUCTION where the `_pad2` handle explicitly does not. |
| `"=m"` | 🟠 **PARTIAL.** Memory as an OPERAND is fully modelled; memory as a **RESULT cannot be expressed** (`LirInst::result` is typed `LirReg`). ⇒ an `"=m"` output must lower to the store-class shape (`result: none` + operand list). |

### 0.4 ✅ ALL THREE KILLED LANES WERE RESUMED AND COMPLETED (2026-08-14, session 2)
| lane | outcome |
|---|---|
| **Lane 1 — LIR wiring** | ✅ DONE — §0.4.1. Found **2 shipped defects** (`verifyLir`'s false rule; the 2addr abort). |
| **Lane G — config/grammar** | ✅ DONE — 46 + 22 tests green. **Settled the blocking gcc/clang measurement (§0.4.2)** and found `D-ASM-ZERO-OPERAND-PLAIN-INSTRUCTION-UNLOWERABLE`. |
| **Lane T — template→LIR** | ✅ DONE — **715/715** on the affected surface (`asm/ lir/ mir/ hir/ program/ link/ conformance/ examples/`). `examples/asm` **12/12 before AND after** the extraction. **CLOSED the zero-operand blocker** as "SHAPE 0". |
| **Lane V — diagnostics** | ✅ DONE — `0xE065`..`0xE06B`, all unsuppressable. |
| **Lane S — positional selectors** | 🟠 IN FLIGHT — spec is [plan 29 §4.7](29-inline-asm-plan%20-%20tbd.md). |

#### 0.4.1b ✔MEASURED — FULL-SUITE STATE AFTER LANES 1/G/T/V
**863 / 864.** Affected surface (`asm/ lir/ mir/ hir/ program/ link/ conformance/ examples/`)
**715/715**; complement (`lsp/ analysis/ core/ harness/` + shuffled + guards) **148/149**.
`examples/asm` **12/12 before AND after** Lane T's extraction, fixtures unchanged.
**The single red is `core/test_unsuppressable_codes` → `EveryMemberHasAnEmitSiteOrIsMarkedRetired`**
— Lane V's seven `S_InlineAsm*` codes (`0xE065`..`0xE06B`) are in the closed table with no emit site
in `src/` yet. ✔Attribution proven, not assumed: all seven are ABSENT at HEAD, and the asm lane's
four files contain **zero** `S_InlineAsm*` references. **It goes green when the front-end lane lands
its emit sites IN THE SAME COMMIT** — the ordinary mid-flight shape, not a defect.

⚠ **COORDINATION POINT — two of the seven overlap the engine tier and MUST NOT DRIFT:**
`S_InlineAsmPlaceholderOutOfRange` (0xE06A) is the **semantic-tier twin** of the engine's
`AnUnboundOperandIsRefusedNamingTheBoundSet`. The front end will validate `%3` against the operand
list before the template ever reaches `lowerAsmTemplateToLirRun`, so **the engine refusal is a SECOND
line of defence, not the primary one** — correct, and worth keeping, but the two key on *different
facts*: the engine's on *"the caller bound no such spelling"*, the front end's on *the index*. Keep
both; do not let them drift into disagreeing about what "out of range" means.
`S_InlineAsmPlaceholderInBasicTemplate` (0xE06B) confirms the basic-vs-extended `%` discriminator is
owned at the **front-end/lexer** tier — which is where §0.4.2's measurement says it belongs, and why
the engine correctly needs no assumption either way.

#### 0.4.2 ★★★ THE §2a TABLE WAS WRONG — SETTLED, AND IT CHANGES THE MANDATORY PIN
✔RE-MEASURED twice, base64-fed sources, `gcc 13.3.0` + `clang 18.1.3` (unsuffixed `clang` ABSENT):
a bare `%eax` in an **EXTENDED** template is an **ERROR on BOTH** (gcc: *"operand number missing
after %-letter"*; clang: *"invalid % escape"*). **Any colon makes it extended.** `%%eax` works.
⇒ in an extended template `%`+letter is a **MODIFIER, not a sigil**, so the template surface and the
`.s` surface **genuinely differ lexically**. ⚠⚠ **THE NEGATIVE MISCOMPILE PIN MUST BE WRITTEN
`%%eax`** — written the §2a way it would not compile under the reference compilers at all.

#### 0.4.3 ⚠ TWO CORRECTIONS TO ORCHESTRATOR BRIEFS — both were MY error, recorded so they are not repeated
- **A token-declaration-ORDER pin cannot exist.** ✔MEASURED: `longestMatch` probes down from the
  longest declared lexeme, so 2-byte `%%` beats `%` **by LENGTH in any row order**; and
  `nlohmann::json`'s default object is a `std::map`, so **row order does not survive the parse**.
  The requested pin would have asserted **nothing**. What shipped pins the only mutation that CAN
  change the outcome — **deleting the `%%` row** — and fails if there is no row to erase.
- **The placeholder CANNOT be a sibling alt.** `%` is already `RegisterSigil`/`TypeSigil` and
  `detectAmbiguousAlternatives` refuses two sibling alts sharing a FIRST token — **exercised**, not
  read (two in-process tests watch the loader refuse it). ⇒ use the **shipped `lexerModeTokens`**
  per-mode override + an `assembly.templateLexerMode` key. Reuse, not invention.

#### 0.4.4 ⚠ FIVE STALE LINE-NUMBER CITATIONS created by Lane T's extraction — FIX BEFORE COMMIT
Three point **past EOF**. `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` `:1616`→`asm_text_to_lir.cpp:1224` ·
`D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET` `:2827`→`asm_template_to_lir.cpp:1529` ·
`D-ASM-DATA-SYMBOL-ABSENT-FROM-SYMTAB` `:272`→`asm_text_to_lir.cpp:50` ·
`D-ASM-ZERO-OPERAND-PLAIN-INSTRUCTION-UNLOWERABLE` `:2580`→`asm_template_to_lir.cpp:1298` **(and that
row is now CLOSEABLE)** · `_handoff.md` `:3360`→`asm_text_to_lir.cpp:1561`.

### 0.4.0 (historical) the three lanes as they stood when the first session died
Resume by re-reading each file, NOT by assuming the lane finished. Their transcripts are on disk.
| lane | owned paths | observed state |
|---|---|---|
| **Lane 1 — LIR wiring** | as below | ✅ **DONE** — see §0.4.1 |
| **Lane G — config/grammar** | `asm.lang.json`, both dialects, `assembly_config.hpp`, `grammar_schema_json.cpp`, `semantic_config.hpp`, `tests/core/test_language_references.cpp`, `tests/asm/test_asm_shipped_dialects.cpp` | all three `.lang.json` MODIFIED; added `operandRule`/`memoryClobber`/`conditionCodeClobber` as **REQUIRED** members of `semantics.inlineAsm` ⇒ **this RED is Lane G's own and it was mid-fix:** `LanguageReferenceRefusals.InlineAsmGateBaseLoadsClean` (`test_language_references.cpp:949`) — the in-test host fixture at ~`:936` still declares a now-PARTIAL `inlineAsm` object, which the all-or-nothing loader correctly refuses |
| **Lane T — template→LIR** | `src/asm/asm_{text,template}_to_lir.*`, `src/asm/CMakeLists.txt`, `tests/asm/test_asm_template_to_lir.cpp` | `asm_template_to_lir.hpp` CREATED (untracked), `asm_text_to_lir.cpp` MODIFIED, **`.cpp` NOT yet created**. Task: extract the per-instruction core (`emitInstruction` ~:1845, `decodeOperand` ~:1960, `buildLirInst` ~:2478, variant election ~:3015) behind a caller-supplied `LirBuilder` + operand-resolution callback. ★ MUST be behaviour-preserving; the 12 `examples/asm/` fixtures are the guard, green before AND after |

✅ **Lane V — diagnostics: DONE.** `0xE065`..`0xE06B` (`S0065`..`S006B`), all seven unsuppressable,
array 148→155. Values measured free two independent ways; band is gapless with **four** retired-but-
reserved values (`0xE015`, `0xE04E`, `0xE04F`, `0xE052`) that must never be reused. Red-on-disable
proven with the mutant shown to have been read (binary mtime).
⚠ **Its ONE red is a cross-lane ORDERING dependency, not a defect:**
`UnsuppressableCodes.EveryMemberHasAnEmitSiteOrIsMarkedRetired` fails with exactly those seven codes
because nothing emits them yet — **it goes green when the consumer lanes land in the SAME commit.**
Fallback if they slip: the shipped `D_DependencyGit*` precedent (drop the rows, restore 148, flip the
pin to its negative form). ⚠ `0xE06B`'s "does not diverge from the reference toolchain" half is
**INFERRED** (it rests on gas rejecting the emitted `%0`) and is labelled as such in-code — measure it
before the consumer lands, or narrow/withdraw the code.

#### 0.4.1 ✅ LANE 1 COMPLETE — what landed, and TWO shipped defects it found
**Landed:** the per-instruction handle rides all four rebuild passes; `copyLiteralPool` shim
**deleted** (zero refs left in `src/`); `verifyLirRebuild` wired after each of the four passes
(`compile_pipeline.cpp:1021, 1049, 1061, 1109`) plus `verifyLirPostRegalloc` after rewrite and
callconv, **always on, no debug gate**; the XFAIL replaced by per-pass positive pins **plus three
ORDERING pins** (the handle landed on the *right* instruction — which no reference count can see).
Red-on-disable done in **two** mutation classes per pass (delete the carry; move it to the end of the
iteration — the realistic misplacement), mutant-read proven by **both the `.obj` AND the `.dll`
mtimes advancing**, the DLL being what the pin's process actually loads.
**Cost ✔MEASURED: +3.3%** compile time (1162 ms vs 1125 ms on a 135 KB input; spread ~2%, so ~2× noise).

★★ **THE SURPRISE, and it is the interesting part:** the four passes are NOT uniformly 1:1, and
callconv has a **third class** — ~11 arms *materialize* a virtual op into a **different** opcode
(`arg`→mov/frame_load, `alloca`/`va_*`→lea, `frame_*`→class-routed memory op, `ret_piece`→consumed),
and several have **no correspondent at all** (`maybeMov` emits **zero** instructions when regalloc
already picked the source register). ⇒ *"carry one handle per source instruction"* is not even
well-defined there. Those arms deliberately carry nothing **and that is fail-loud**: the pool is
copied unconditionally, so a dropped handle leaves an unreferenced entry and `verifyLirRebuild`
reports `L_SideStructureReferenceLost` **naming the pass**.
Per-pass correspondent: 2addr → the **second** (the operation); wide-call-args → the **last** (the
Call); rewrite → the **middle**; callconv → varies by arm.

⚠⚠ **DEFECT 1 — `verifyLir` is DELIBERATELY NOT WIRED, because the rule is FALSE about the LIR this
compiler builds.** Wiring it reds **~200 `examples/` tests**, all on Rule 1
(`checkMemOperandPairing`) and no other rule: the rule demands every `load`/`store`/`lea` end with
`MemBase`+`MemOffset`, but `mir_to_lir.cpp:3809` emits `load result, [SymbolRef]` for a global (and
`lea result, [SymbolRef]`) — a legitimate symbol-addressed mode with **no base/offset pair**.
★ It survived because **it had only ever run on hand-built test modules** — the same
"pin that never met its subject's real input" family this project keeps catching. `lir_verifier.cpp`
was not Lane 1's to edit. **Anchor + fix; until then `verifyLir` cannot run in production.**

⚠⚠ **DEFECT 2 — `legalizeTwoAddress` ignores `emitTerminator`'s failure**: it never sets
`allFunctionsLegalized = false`, so the block is left unterminated and `finish()` **aborts the
process** instead of failing loud. Same *"a refusal that crashes is not a refusal"* class as
`D-LIR-TEXT-PARSE-UNSEALED-BLOCK-ABORT`. **Anchor + fix.**

★ **No collision with the queued Lane R:** Lane 1 touched `lir_rewrite.cpp` lines **674–687,
696–698, 700–702, 946–951** only; the `implicitForbidden` region at **541–554 is byte-identical to
HEAD** with unchanged line numbers.
✔ `ctest` in `build-lane-lir`: **860/863** (the 3 reds being the guard + the two concurrent lanes'
in-flight state). `build-dbg` never built into.

#### 0.4.5 ✅ LANE S (positional operand selectors) COMPLETE — plan 29 §4.7 shipped
17 new tests, `asm` suite 38/38. **One key minted (`operandSelectors`) + one struct; no new role,
verb, kind or diagnostic code.** Six red-on-disable mutants, each proven read by subject-binary mtime,
all through `ctest`. ★ Two of them mattered more than expected: mutant **B** was **too coarse** — two
tests stayed green because it made all twelve `cset` rows match, tripping the double-match guard
instead; the lane **refused that as evidence** and added finer mutants B2/C that actually
discriminate. And under mutant A the pin's own anti-vacuous `ASSERT_TRUE(row.contains(...))` fired,
**reporting itself BROKEN rather than passing vacuously** — the guard-on-the-guard working.
✔ Load-time ambiguity refusal (§4.7.1) **EXERCISED**, not read, with the complement pinned (twelve
`cset` rows sharing one spelling load clean) so it cannot degenerate into "all duplicates refused".
✔ Witnessed by EXECUTION: `aarch64-linux-gnu-objdump` reads DSS's own ELF back as
`mrs x0, cntvct_el0` / `cset x0, ls`, and it runs under qemu-aarch64 **exit 0, debug AND release**,
negative control exit 255.
⚠ **The cross-front-end pin's C half is NOT reachable — measured by EXERCISING it**, not read:
`__asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r"(v))` → `error[S0062]`, and `cntvct` has **one**
`src/` hit, a comment. What landed instead asserts the `.s` walker and `lowerAsmTemplateToLirRun`
elect the SAME opcode row and emit byte-identical output. **P5's exit criterion still needs the C
gate opened by the front-end lane.**
★★★ **AND IT CAUGHT A FALSE CLOSE — the highest-value find of the cycle. See §0.4.6.**

#### 0.4.6 ★★★ A SHIPPED `✅ CLOSED` ANCHOR WAS FALSE, AND THE FAILURE MODE IS GENERAL
[[D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED]] was marked **✅ CLOSED 2026-08-13**. ✔MEASURED
2026-08-14: `condCodeOfOperand` resolves a bare-name operand against `kTargetCondCodeTable`, which is
keyed on **SUBSTRATE** names (`slt`, `sle`, `ult`…) while gas writes `lt`, `le`, `lo`, `ls`, `hi`,
`hs`, `cc`, `cs`. It is correct on exactly **`eq` and `ne` — which are the two spellings the row cites
as its evidence** — and WRONG IN BOTH DIRECTIONS on the other ten (refuses `cset x0, lt` which gas
accepts; accepts `cset x0, slt` which gas rejects). ⇒ the close covered the ENGINE half on its two
easy cases; the DIALECT half stayed open and the ten hard spellings were never exercised.
★★★ **THE TRANSFERABLE LESSON, now in the row: A CLOSE WHOSE WITNESS SET IS THE SUBSET WHERE TWO
VOCABULARIES AGREE HAS TESTED THE COINCIDENCE, NOT THE MAPPING. Pick witnesses where they DISAGREE.**
The row is corrected in place — re-closed by the twelve selector rows, each byte-pinned to gas's
measured word, with the superseded claim kept in a `<details>` block rather than deleted.

#### 0.4.7 ⚠ A LANE CORRECTLY REFUSED AN OPERATOR INSTRUCTION — recorded so it is not "fixed" back
The ruling said: if gas is case-insensitive, the selector match must be too. ✔MEASURED: gas **is**
fully case-insensitive — but DSS's mnemonic match is exact and **already** breaks `MOV X0, X1`
dialect-wide. A case-insensitive selector beside a case-sensitive mnemonic leaves one half of a row
loose and the other strict, and `MRS X0, CNTVCT_EL0` would **still** fail at the mnemonic ⇒ it buys
nothing and **hides the real gap behind a half-fix.** The lane anchored the true, dialect-wide gap
instead: [[D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE]].

### 0.5 ⚠ STILL OWED BEFORE COMMIT
⚠⚠ **ANCHOR BALANCE IS `+8` AND THE GATE CORRECTLY FAILS.** ✔MEASURED 2026-08-14:
`anchor-registry: OK (1033 src anchors all resolve to plans)` but
`anchor-balance: FAIL — this cycle leaves 8 more row(s) OPEN than it found.`
**DO NOT COMMIT AND DO NOT WIDEN THE GATE.** The cycle has opened rows and closed one
(`D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED`, re-closed properly). The four big closures —
`D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED`, `D-CSUBSET-INLINE-ASM-OPERANDS`,
`D-CSUBSET-INLINE-ASM-GOTO`, and the embedded half of `D-CSUBSET-INLINE-ASM-TEXT` — all depend on the
front end, MIR carriage and the expansion, **none of which exist yet**. The balance goes negative
when they land; until then this gate failing is the truth, not an obstacle.
✅ **The `anchor_registry_guard` red is CLEARED** — ✔MEASURED 2026-08-14:
`anchor-registry: OK (1030 src anchors all resolve to plans)`, 3816 rows in 40 files, 0 cell-width
violations. Four rows were written: `D-LIR-PER-INST-REG-CONSTRAINTS` (the guard's actual cause),
`D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE`, `D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE`, and
`D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED`.
⚠ **All four are OPEN, so the anchor-BALANCE gate is now +4 and WILL FAIL until the cycle's closures
land** (`D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED`, `D-CSUBSET-INLINE-ASM-OPERANDS`,
`D-CSUBSET-INLINE-ASM-GOTO`, the embedded half of `D-CSUBSET-INLINE-ASM-TEXT`). That is expected
mid-flight, not a defect — but **do not commit while it is positive**.

★★★ **NEW BLOCKER, and it is a §B the operator has NOT yet answered:
`D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED` BLOCKS THE arm64 HALF OF P5's OWN EXIT
CRITERION.** The dialect lane correctly DECLINED to ship a `cntvct` row: gas spells it
`mrs <Xd>, cntvct_el0` — the system register is an **OPERAND** — while the target opcode is
**ZERO-operand** with the counter in the fixed word, so a `{"spelling":"mrs","opcodes":["cntvct"]}`
row would hand the lowering one leftover operand against `maxOperands: 0` and **every line using it
would fail loud**. ⚠ This NARROWS rather than contradicts the earlier refutation: that refutation is
still right about the ENCODING side (`cntvct` needs zero new slot vocabulary), but it never reached
the DIALECT side. P5's exit criterion is *"`hwtime.h` compiles"* and hwtime.h's arm64 arm **is** the
`mrs`. The two candidate shapes and a recommendation are in the row; **ask before building.**
- `D-TEST-SCHEMA-MUTATION-HELPER-FAILS-OPEN` — cited in `tests/test_support/CMakeLists.txt:42`, no
  row. (The guard scans `src/`+`examples/`+`real-examples/` only, so it does NOT fire — §A.7 still requires it.)
- Rows born ✅ CLOSED: the mutation-helper fail-open · the vacuous `VaListStrategyKeys.
  AKeyValidForTheDeclaredStrategyIsAccepted` pin · the `[[nodiscard]]` explicit discard at
  `asm_text_to_lir.cpp:3360` · **the `run-gate.sh`-piping trap above.**
- **NO ROW EXISTS** for the per-target **operand-modifier width-view facet** that `S0067` (`%w0`/`%k0`)
  refuses on — plan 29 never mentions it. Decide: build it, or a real trigger-gated row.
- The `returnVrs` blindness (§0.2) · the `"+r"` core gap (§0.3) · the `checkVregClassMatchesMirType`
  4th skip arm.
- **`--scanstatus` must be re-added to `real-examples/c/sqlite/legs.json`** (its `$scanstatusComment`
  at `:43` says so explicitly) **in the same change that closes the gap**; `requiredDefines` then
  proves it took. Buys 2 test files (`scanstatus`, `scanstatus2`).

### 0.6 REMAINING WORK, in dependency order
1. Finish Lanes 1 / G / T (above).
2. **Rename lane** (§0.1.3) — mechanical, blocked on Lane 1 (`lir_callconv.cpp`).
3. **Lane R — regalloc chokepoint + earlyclobber.** Blocked on Lane 1 (`lir_rewrite.cpp`). ★★ The
   `inputs ∪ clobbered` union is **hand-rolled at THREE sites** (`lir_regalloc.cpp:483`, `:1095`,
   `lir_rewrite.cpp:542`), ALL per-opcode-only ⇒ **regalloc cannot see the per-instruction pool at
   all**; carrying handles faithfully and then ignoring them is the exact silent miscompile P5 exists
   to prevent, and it passes any test that only checks the handle survived. §A.5 ⇒ ONE accessor
   (`effectiveForbiddenOrdinals`), three callers, and the closing test must exercise **each site
   individually** (one of them with a SPILLED operand — `lir_rewrite.cpp:538` names the miscompile it
   prevents: *"a spilled idiv DIVISOR reloads into rax … 121 not 160"*).
   ⚠ Earlyclobber: the fix recorded in an earlier handoff ("place the def at the FIRST expanded
   instruction") **DOES NOT WORK** — ✔MEASURED, for a single-instruction template the input's use at
   `earlyPos` and the def at `latePos` are already disjoint (`lir_liveness.cpp:143-144`, `:352`,
   `:360`; `expireActive` frees at `range.end <= currentStart`, `lir_regalloc.cpp:574-588`). The
   discriminating variable is **early slot vs late slot**, not which instruction. The multi-instruction
   case is already safe. **The test MUST be the single-instruction case with a matched plain-`"=r"`
   control demonstrating SHARING** — otherwise it passes on an allocator that never shares.
4. **Lane M — MIR carriage**: `InlineAsm` `{0,N}`, the `ResultPiece` payload split (§0.2), the
   producer-declared piece source (§0.1.2) — ✔MEASURED it needs a **CONSUMER** in `returnReg`, not a
   new carrier: `LirRegConstraintPool` already carries per-inst `outputNames`/`outputOrdinals` with
   zero consumers — `returnVrs` fix, `asm goto` CFG + edge placement + critical-edge splitting.
   ⚠ MIR is rebuilt by **three** live verbatim-copy sites that carry `instPayload` but re-add no side
   pool (`opt/passes/mir_rebuild_helper.cpp:428`, `inlining.cpp:617`, `inlining.cpp:1041`; LICM is
   excluded by construction at `licm.cpp:93`). ★ Structural fix, not a checklist: add the asm opcodes
   to `MirBuilder::addInst`'s dedicated-builder refusal list (`mir.cpp:637-647`) so a forgotten copy
   site **aborts loudly** instead of dropping the clobber list.
5. **Lane F — front end**: `InlineAsmFacts` captures **zero expression NodeIds** today
   (`semantic_analyzer.cpp:9498-9536`) — that is the hole. Gates at `:10950`/`:10990`/`:11013`+`:11028`/
   `:11042`. ⚠ **There is NO typed-view layer** — `docs/tree-model.md:124` records the 08.55 cleanup
   deleted it because role-position helpers drift silently; extend `gatherInlineAsmFacts`, do not mint
   a view. Locate the operand's value expression by **RuleId**, never by child position.
6. **Lane X — MIR→LIR expansion + corpus + goldens.** ★ Every register-pinned OUTPUT must ALSO enter
   the instruction's clobber set: `outputs ⊆ clobbered` is **loader-enforced only for the per-opcode
   path** (`target_schema_json.cpp:3235-3259`) and regalloc's forbidden set deliberately omits outputs
   (`lir_regalloc.cpp:452-466`) — the second is safe ONLY because of the first, and **the
   per-instruction path has no loader**. Attach the handle to **EVERY** instruction the block emits,
   not just the first (`collectImplicitClobberPositions` records one forbidden position per instruction).
   ⚠ **Test-design trap:** `__asm__("nop")` "compiles AND RUNS" is VACUOUS — a `nop` changes nothing
   observable, so an expander emitting ZERO instructions passes it, and no cleverer basic template
   fixes it (basic asm's register effects are invisible to the compiler by design). ⇒ the witness must
   be TWO-PART: a byte/structural pin that the instruction was emitted, PLUS the runtime arm.
   ⇒ **P5 cannot land without its negative miscompile pin**: a value obtained from a **CALL** (not
   computed inline — both reference compilers schedule around an inline computation and every arm
   looks identical), held live across an asm block that clobbers its register, asserted to survive.
   With a `{"shippedPipeline": "release"}` arm.

</details>

---

## 1. WHERE WE ARE

### The cycle in flight: AP6 — `dependsOn` resolution
Plan-06 §5.1 **B.1–B.12** are the decisions of record. **B.10 amends U-2** (consumer-driven
derivation) and **B.11 carries the design-audit rulings M2–M8 + the U-8 correction** — read both
before touching anything dependency-shaped.

✔**MEASURED baseline of the MERGED tree** (Windows MSVC-Debug, `build/`, 2026-08-14): build **rc=0,
ZERO warnings tree-wide**; full ctest **859/861** at the merge point, with both reds diagnosed and FIXED (§1.3). The pre-cycle baseline at `d4c2836` was 860/860; the gate after the resolver, the two corpus examples and the CR fix was 864/864 (§1.2).
✅ **CURRENT GATE: 866/866 ON ALL FOUR LEGS, 0 failed.** ✔MEASURED 2026-08-16 at `ea47e69`, each leg
synced **by `git fetch` + `reset --hard`, NOT by rsync** — the previous cycle's CRLF confound came
from rsyncing the Windows working tree, and syncing by git removes that class of confound entirely
(every leg reported `DIRTY=0` at the pushed HEAD before building).

### Assembly — where the two halves actually stand
📄 The durable owner is [`.plans/29-inline-asm-plan - tbd.md`](29-inline-asm-plan%20-%20tbd.md).
⚠⚠ **`.temp/PLAN-inline-asm-arc-2026-08-12.md` IS SUPERSEDED AND ITS PHASE NUMBERS ARE RETIRED.**
Plan 29 deleted its `P3`/`P4` rows on 2026-08-12 and says why: *"Two rows for one phase is not
history, it is an ambiguity about what 'P4 is done' means."* Quoting the `.temp` numbering will
mis-size the work — it happened on 2026-08-14 and cost a scope correction.

- ✅ **`.s` as a real input language WORKS.** P1+P2, P2.5 and P4 are done: `asm.lang.json` + two
  dialects, 12 runnable `examples/asm/`, text→LIR→bytes, a `.s` executing on x86_64 **and** arm64.
- 🟠 **Inline asm as a C FEATURE is the open half, and it is P5** — not "P3+P4". Today only the
  **empty** template compiles (→ one `MirOpcode::CompilerBarrier`); `__asm__("nop")` is refused by
  `S_InlineAsmNonEmptyTemplate` (0xE057) and any operand/clobber/label by `S0062`.

### Instrument health — the standing warnings
- ⚠⚠ **TWO independent mechanisms make a red-on-disable report GREEN over a LIVE mutant** — one
  where the mutant was never COMPILED IN (`ninja -t deps` = `#deps 0`, **10 of 403** objects) and
  one where it was never READ (cwd-walk config resolution). In both, every fail-closed clause was
  satisfied. ⇒ treat any green red-on-disable from this tree as UNPROVEN unless the mutant was shown
  to have been **read**. A config-level red-on-disable **MUST** run through `ctest`, never a bare `.exe`.
- ✔ **A THIRD was found 2026-08-14 and is now FIXED**: `tests/test_support/mutate_target_schema.hpp`
  had three fail-open routes (an unmatched `removeMnemonics` entry, an undocumented no-`opcodes`
  early return, and `erase(remove_if…)` returning `end()` on no match). It is this project's primary
  anti-vacuity instrument and it could mutate **nothing** and report success. Now throws on any
  no-op, including a `doc.dump()` before/after byte comparison. ★ It immediately caught a **genuinely
  vacuous shipped pin** (`VaListStrategyKeys.AKeyValidForTheDeclaredStrategyIsAccepted` wrote two
  values the shipped layout already declared, so its "mutant" was byte-identical) — fixed.
- ✔ Anchor guard resolves **truncated** citations by substring: **91 line-wrapped `D-*` names across
  48 files** pass silently → `D-GATE-ANCHOR-GUARD-RESOLVES-TRUNCATED-CITATIONS-BY-PREFIX`.
- ✔ Registry **line-number** citations rot silently. A stale path fails loudly when grepped; a stale
  line number still resolves, to the wrong code.
- ✔ **Counts written from memory keep erring LOW.** Never re-quote a gate figure — re-measure at the
  commit that carries it.

---

## 2. ✔MEASURED 2026-08-14 — the reference-compiler spec for inline asm

📄 Per [[feedback_reference_compilers_are_the_spec]] these tables ARE the specification. gcc 13.3.0
(x86_64 native + `aarch64-linux-gnu-gcc` cross) and clang 18.1.3, `-O2 -S`. **Re-state the versions
beside any re-quote.**

### 2a. What a clobber list MEANS — and it is not what "extended vs basic" suggests
| arm | gcc 13.3 | clang 18.1.3 | value survives? |
|---|---|---|---|
| `__asm__("xorl %eax,%eax")` — basic | `call h` → asm → `ret` | same | **NO — destroyed** |
| `… ::: "eax"` — extended, real clobber | `movl %eax,%edx` → asm → `movl %edx,%eax` | `%ecx` | **YES** |
| `… :::` — extended, EMPTY clobber list | identical to basic | identical | **NO — destroyed** |

⇒ **(1)** emitting a basic template verbatim while assuming ZERO clobbers **is** the reference
semantics — the corruption is the programmer's, by design; refusing it, or conservatively spilling
around it, are both divergences and the second is silent. **(2)** protection must key off the parsed
clobber **LIST being non-empty**, never off "is this the extended form".
⚠ The first version of this probe was WEAK — it computed `y = x*3` before the block and both
compilers simply *scheduled the multiply after* the asm, so nothing was live across it and every arm
looked identical. The value must come from a **call**.

### 2b. What `%0` expands to
| C source | x86_64 | aarch64 |
|---|---|---|
| `"=r"` / `"r"` | `%eax` / `%edi` | `x0` |
| `"=a"` | `%eax`, and it genuinely **PINS** (3 competing `"r"` operands, still `%eax`) | — |
| `"m"` | `(%rdi)` | — |
| `"i"(7)` | `$7` | `7` |
| `"%w0"` | — | `w0` (the 32-bit VIEW — this is `subOf` from the template side) |
| real sqlite `hwtime.h` | `"=a","=d"` → `rdtsc` | `"=r"` → `mrs x0, cntvct_el0` |

⇒ ★★★ **RENDERING is DIALECT vocabulary; the letter→register MAPPING is TARGET vocabulary, and they
must not be merged.** x86 renders `%eax`/`$7`, arm64 renders `x0`/`7` — different sigils for the same
abstract operand, and the sigil is an AT&T-vs-Intel fact (one CPU, two dialects). Collapsing them
re-creates `D-CONFIG-ASM-DIALECT-DECLARED-AS-TARGET-VOCABULARY`, the facet built, reviewed and
reverted the same day. ⇒ the six minimum-viable letters split across **three existing axes**:
`r`→`TargetRegClass`, `a`/`d`→a specific `registers[].name`, `i`/`m`→`OperandKindFilter`.

### 2c. ★ Earlyclobber IS observable, and ignoring it is a silent miscompile
| | `"=r"` plain | `"=&r"` |
|---|---|---|
| x86_64, 1 input | `%0=%eax %1=%eax` — **SHARED** | `%eax` / `%edi` |
| aarch64, 1 input | `x0 x0` — **SHARED** | `x0 x1` |
| aarch64, 2 inputs | `x0 x0 x1` — **SHARED with input 0** | `x0 x2 x1` |

The discriminating shape is an input that is a local temporary **dying at the asm** — with the input
in the ABI arg register and the output in the return register the allocator has no reason to share
and the probe says nothing. ⇒ a template that writes `%0` before reading `%1` destroys its own input.
**Accept-and-ignore is not available**; implement `&` or refuse it loud.
✔ DSS's allocator shares by exactly this rule: a **use** is recorded at `pos`, a **def** at `pos+1`
(`lir_liveness.cpp:347-361`), so single-instruction input and result ranges do not overlap.
★ The fix is ~5 lines inside existing machinery: place the `&` output's def at the **first** expanded
instruction — `firstDef` is a min over defs (`lir_liveness.cpp:360`), so its range then covers every
input's and sharing becomes structurally impossible.

---

## 3. OPERATOR DECISIONS TAKEN 2026-08-14 — do not re-litigate

Full rationale in [`29-inline-asm-plan`](29-inline-asm-plan%20-%20tbd.md) §4.0 and §5.

1. ✅ **The clobber carrier: a per-INSTRUCTION pool that REUSES the `ImplicitRegisterConstraint`
   STRUCT**, indexed from `LirInst::_pad2`. ⛔ **NO new `LirOperandKind`** (appending operands breaks
   `operandsMatchGuard`'s positional kind equality and the verifier's last-two-operands invariant,
   and a clobber-only kind structurally cannot express a fixed-register INPUT `"a"(x)`).
   ⛔ **DO NOT touch `TargetOpcodeInfo::implicitRegisters`** — it is config-written; a second writer
   is *"one field, two writers"*. Operator, verbatim: *"Same type, two owners is fine; one field, two
   writers is not."*
2. ✅ **Extend the ONE shared copy helper, do not hand-roll per-pass copies.** ⚠⚠ **There are FOUR
   rebuild passes, not the three that both the plan AND the independent audit claimed** — ✔MEASURED:
   `lir_2addr_legalize.cpp:80`, `lir_callconv.cpp:3971`, `lir_rewrite.cpp:929` **and
   `lir_wide_call_args.cpp:220`**, all the same `lir_pass_util::copyLiteralPool(src, b)` call. The
   fourth is exactly the one a hand-rolled approach forgets.
3. ✅ **A FAIL-LOUD BACKSTOP is mandatory** — after each rebuild, a `_pad2` index outside the pool, or
   a pool whose entry count dropped, fails loud. **This also fixes the shipped literal pool, which has
   the identical exposure today**; operator: *"the precedent carries the bug"*, so inheriting it is not
   acceptable.
4. ✅ **`asm goto` with NO label section: FOLLOW CLANG — ACCEPT.** gcc 13.3 rejects, clang 18.1.3 and
   19.1.1 accept. ⇒ `D-CSUBSET-INLINE-ASM-GOTO`'s named blocker (*"operator decision"*) is
   **DISCHARGED**, so the CFG half lands in the deciding cycle.
5. ✅ **NO P5a/P5b SPLIT** — the proposed blocker (arm64 `mrs` needs a sysreg table + a 15-bit
   encoding slot) **did not survive measurement**; see §4.

---

## 4. THREE REFUTATIONS WORTH MORE THAN THE CODE — recorded so they are not re-derived

1. ★★★ **The arm64 `mrs` blocker is REFUTED.** All three legs fell: `cntvct_el0`'s absence from
   `registers[]` is irrelevant (`TPIDR_EL0` is also absent and DSS already encodes `MRS Xd,
   TPIDR_EL0`); the shipped **`tlsbase`** row encodes MRS as a **zero-operand fixed word**
   `0xD53BD040 | Rd`, its own `$comment` saying *"zero new slot vocabulary"*, and `cntvct_el0` is
   structurally identical (`0xD53BE000 | Rd`); and *"`hwEncodingOf` hard-caps fixed32 at 5 bits"* was
   MISSTATED — it caps at the **caller-supplied** `maxBitWidth`, the 5 being `kFixed32RegFieldBits`,
   a register-field constant a fixed-word MRS never reaches. ⇒ one opcode row + one dialect row.
2. ★★ **"A side-table would be silently dropped by the rebuild passes" is REFUTED.** The literal pool
   is a module-level side structure that survives all four — because each pass explicitly copies it.
   The argument that forced a new operand kind was false.
3. ★★ **`outputs ⊆ clobbered` — both halves of an apparent contradiction are true.** The loader
   ENFORCES it (`target_schema_json.cpp:2870-2894`); regalloc's forbidden set is `inputs ∪ clobbered`
   and *"outputs are not forbidden"* (`lir_regalloc.cpp:452-466`) — the second is **safe because of
   the first**. ⚠ **That invariant is per-OPCODE and does NOT reach the embedded path**, where the
   clobber set is per-instruction and lowering-built ⇒ **the lowering must replicate the rule itself**:
   every register-pinned output (`"=a"`, `"=d"`) goes into the instruction's clobber set, or a value
   live in `eax` across the block dies with no diagnostic.

⚠ **The highest-severity omission the audit caught, carry it into wave 2:** `lir_rewrite.cpp:541-554`
reads `implicitRegisters` to build the **spill-scratch forbidden set**, and its docblock names the
failure it prevents — *"a spilled idiv DIVISOR reloads into rax and clobbers the dividend … a SILENT
miscompile (121 not 160)"*. The scratch pool is *"the allocatable pool MINUS every register already
assigned to a vreg"*, i.e. it preferentially harvests exactly the unassigned physicals an asm block
clobbers. It is an `if`, **not** a `switch` — the compiler forces nothing. Ship a pin with a
**spilled** `"r"` operand.

---

## 5. PRIORITIES

0. **`NEXT` — THE STANDING BACKLOG LOOP (operator 2026-08-19).** One `/dss-cycle` per invocation,
   working the operator's bands (asm+miscompiles+errors → ap5/ap6 → tools → the rest) **until all
   OPEN rows are fixed, best long-term, no workarounds**. THE LIVE QUEUE IS the TOP cycle block's
   "NEXT" list — read it there, never here; this item exists so the stepper finds the loop.
   ⚠ **RE-DERIVE THE QUEUE FROM THE REGISTRY AT EVERY PICK, NEVER FROM A PREVIOUS LIST.** ✔Proved
   twice: an earlier cycle's list named `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED` (already closed),
   and the 2026-08-19 backlog review — mine — recommended `D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD`
   reading it OPEN HIGH off the gate's count when it had been closed since 2026-06-03 with its
   mark in the wrong cell. Recalling a row is not measuring it.
0. **QUEUED BEHIND THE LOOP — the full sqlite MATRIX RE-RUN, now UNBLOCKED.** Operator sequencing:
   *"we'll re run everything once our compile is fast enough"* — it is (Windows 38–41 s, WSL 33.6 s,
   macOS 23.0 s on the CLI kit). Re-run when a loop cycle changes CODEGEN (diagnostics-only cycles
   do not trigger it). Then FC18 ([[D-DIAG-CORPUS-EVERY-CODE]], sole remaining C23 phase — read its
   BLOCKED note first; prerequisite `D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`, which is also
   the loop's C1 head).
0. ✅ **DONE 2026-08-18 (Cycle P10): FIRST-CLASS, CONFIG-DRIVEN LTO** — [[D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE]] CLOSED (see §0.00000000). The grammar + two-stage topology shipped; the split decided by the runtime-measured rule; runtime-differential is a standing instrument.
0. ✅ **DONE 2026-08-18 (Cycle P9): MAKE THE COMPILE FAST.** Four measured causes fixed,
   byte-identical output (§0.0000000). [[D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX]] stays
   🟠 OPEN at a ~2.1× residual — the row says step one is an experiment, not an edit.

1. **FINISH THE P5 CYCLE.** Wave 2 is unstarted: the typed inline-asm view, the four
   semantic gates re-expressed, HIR/MIR carriage, the MIR→LIR expansion, `asm goto`'s CFG, the
   corpus + diagnostics goldens. Then register the anchor rows and run the 3-leg gate.
   ★ **Projected anchor budget: closes 4, opens 2, net −2.** Four of the implementation plan's seven
   proposed deferrals are being DONE, not parked (arm64, the mutation helper, the empty-template
   register clobber, earlyclobber).
   ⚠ **A test-design trap already identified: `__asm__("nop")` "compiles AND RUNS" is VACUOUS as a
   runtime witness** — a `nop` changes nothing observable, so an expander emitting ZERO instructions
   passes it. And it cannot be fixed with a cleverer basic template, because basic asm's register
   effects are invisible to the compiler *by design* (§2a). ⇒ the witness must be two-part: a
   **byte/structural pin** that the instruction was emitted, PLUS the runtime arm.
2. **`NEXT` — the assembly `.cfi_*` producer** (`D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`), unblocked.
   ⚠ **Sharpened 2026-08-14: "the 18 spellings are accepted-and-dropped" is a ONE-DIALECT claim.**
   ✔MEASURED: x86_64-att carries them, **arm64-gas carries ZERO**, and directive dispatch REFUSES an
   unrecognised spelling — so `.cfi_startproc` on arm64 **fails loud**, it is not dropped. The fix is
   **not symmetric**, and declaring them `ignoredAnnotation` on arm64 "for consistency" would
   propagate the defect into a second dialect. `cfi_escape` must stay a REFUSAL.
3. **`NEXT` — COFF `.obj` unwind tables.** Effort, not knowledge; the MSVC reference is captured
   (`.pdata` 3 `ADDR32NB`, `.xdata` 1, ORDINARY NAMED SYMBOLS not aux section symbols). ⚠ Mach-O
   `MH_OBJECT` is **blocked** — no clang on this host to measure the reference.
4. **`OPERATOR DECISION` ×3** — `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` (declaring `rip` a
   `gpr` hands the instruction pointer to regalloc; blocks assembling real `gcc -S` output) ·
   `D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL` (`isData` picks GOT vs PLT, a **wire-format**
   consequence) · `D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS` (a type split across ~25 sites).
5. **`QUEUED` — the 91 wrapped anchor citations** (must land atomically with tightening the guard).
6. **`QUEUED`** — FC18 `D-DIAG-CORPUS-EVERY-CODE` ⚠ **which is BLOCKED, and the row says so**: the
   corpus harness renders RAW SOURCE positions (`UnitBuilder::addInMemory`, no target, no `--define`s)
   while the CLI shifts the line by the predefine prologue + one per `--define` — **0 on elf, +2 on
   pe64**. Its deliverable would certify green a CLI that prints shifted positions, once per code
   added. Prerequisite: `D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED` (HIGH, unconditional trigger —
   under sqlite's ~25 `--define`s **every** semantic diagnostic is ~25 lines off).
   · binary rename → `dsscp` · CI + pkg-publish INERT (PR #45) · public repo (PR #37) · the
   "byte-identical vs GCC" overclaim in `pitch.txt`.

### Two anchors that must NOT be closed — closing them would itself break the bar
- `D-ASM-TARGET-DECLARES-NO-BYTE-ORDER` — no big-endian target exists to key the facet from.
- `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED` — no shipped target declares `condCodeFromPayload` on
  a return or branch-with-link.

📄 Both trigger-gated. Building either is the speculative build §A.2 forbids *in the other direction*.

---

## 6. ENVIRONMENT — pre-flighted 2026-08-14

- ✔ **The WSL gate leg is TWO MERGES STALE**: `~/src/dss-code-prime` is on
  `feature/sqlite-green-full-57377343437` at `3e86a187` (PR #48) with **343 dirty files**. Its
  "untracked" files (`examples/asm/`, `src/asm/asm_text_to_lir.cpp`, the new plans) are things already
  in main that arrived by **rsync while its git branch stayed old** — it is a disposable mirror.
  ⚠⚠ **BUT it carries 4 stashes and 10+ unpushed July commits.** Sync it by overwriting the WORKTREE
  only; **never reset its git state**, and never `rsync --delete` on a variable-built path (that once
  became `rsync -a --delete / /`).
- ✔ Toolchain present: `qemu-aarch64`, `aarch64-linux-gnu-gcc` 13.3, `/usr/aarch64-linux-gnu`,
  cmake 4.3.2, ninja 1.11.1, g++ 13.3. ⚠ `clang` unsuffixed and `gcc-14` are **ABSENT**; `clang-18` is
  present.
- ★ **3-leg gate**: Win ctest (`build-dbg`) + WSL x86_64 + qemu arm64; the last REQUIRES
  `QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` or ~450 arm64 examples false-red at exit 255. Use
  `DSS_STRICT_ARM_VERDICTS=1` — with strict OFF a missing emulator is a WARNING and the suite still
  passes, so a green run alone is a partial run rounded up.
- ★ **From PowerShell always `wsl.exe -e`**; from Git Bash never `wsl.exe bash -c` with a variable.
  **Quoted heredocs eat backslashes** ⇒ write the script to a FILE and run the file.
- ★ Use `scripts/run-gate/run-gate.sh` with a **TOOL-EMITTED** witness (`'ninja: no work to do|^\[[0-9]+/[0-9]+\]'`,
  `'100% tests passed'`). It correctly REFUSES a caller-authored `BUILD OK` — a watcher polling for a
  self-written success string once span until killed **over a build that had succeeded**.

---

## 7. CONCURRENT BRANCHES

📄 PRs #50/#51/#52 are merged; this branch is cut from main and, as of 2026-08-14, **no overlap
hazard is known**. ⚠ A concurrent governance workstream has shared this tree before ⇒ **stage by
explicit path, never `git add -A`** (`D-CYCLE-CANNOT-ASSUME-IT-OWNS-THE-WORKING-TREE`), and watch for
stray build artifacts (`*.preMutant`, `*.orig`) left by tooling.
⚠ **DCO: every commit needs `Signed-off-by` (`git commit -s`).**

### Dormant branches (no open PR) — do not rebase onto these
`feature/c23-conformance-burndown-2` (the asm cycles; content merged via #51) ·
`feature/c23-conformance-burndown-1` (2026-08-12, GUI + GPU plans) ·
`feature/sqlite-green-full-57377343437` (2026-08-11 — **this is the WSL mirror's branch**) ·
`feature/finish-sqlite-full-green-5366546` (2026-08-10) · ~20 older `feature/0-0-2-p*` branches.

---

## 8. TIMELINE

*Newest first. Accumulates — new cycles are prepended. Includes cycles that did not go well.*

| Date | Commit | What shipped | Gate |
|---|---|---|---|
| 2026-08-21 | *(this cycle, P24)* | **`integrated_tests` became 616 ctest entries instead of one**, so the corpus parallelises and every example reports its own pass/fail. The operator ruling that shaped it is the portable part — **a UNIVERSAL claim is per-example; an EXISTENCE claim is about the corpus and stays one** — and it rejected all three options as offered. Four defects found by EXERCISING the new harness, every one of them in the thing meant to catch defects: a floor that could not pass, an adjudicator that always SKIPPED, a torn cell filed as a subset, and a witness satisfied by the rename it exists to detect. 922 → 1537 entries | **FOUR legs, all 1537/1537**: Win 516.88 s (24% faster than P23 at 67% more entries) · WSL 350.84 s (clean build) · native aarch64 VPS 1031.98 s · real Apple Silicon 1651.95 s (**3.0×** P23). balance ✅ **1034 → 1033, net −1 — PASSES** |
| 2026-08-21 | `649b0730` | **Weak DEFINITIONS and weak ALIASES ship on pe/coff and mach-o**, the retyped-closed-set class is finished for its three published owner shapes, and `D-HARNESS-PE64-LIB-ACQUISITION-IS-HOST-DEPENDENT` closes on the cross-host measurement P13 never took. Then a step-10 independent audit **refuted one of the cycle's own closure claims**, six fold lanes turned that into 26 more defects, and a FOURTH gate leg found two host-capability defects three legs could not see. +14 ctest entries (908 → 922); new `scripts/remote-leg/remote-leg.sh` | **FOUR legs, all 922/922**: Win 678.01 s · WSL 336.99 s (clean build) · **native aarch64 VPS** · **real Apple Silicon**. balance ⚠ **1018 → 1034, +15 created-over-closed — FAILS, shipped on an operator ruling** |
| 2026-08-18 | *(this cycle, P8)* | **Path identity becomes a TYPE.** `core::PathIdentity` + 14 containers re-keyed + `scripts/check-path-identity/check-path-identity.py`; the 8.3 blindness of `weakly_canonical` under libstdc++ measured and closed. Plus `mustDifferFromBaseline` on the CLI runner (455 manifests / 548 arms armed) and a `--rsync` transport on both ssh carriages (the Mac's profile eats stdin) | Win **898/898** · WSL **898/898** · arm64 + macOS below |
| 2026-08-16 | *(P7 predecessor)* | **`module` corpus example** — `project_module_standalone_build`, the first corpus proof that a `module` project builds standalone (B.13.3). Closed on the standalone half only; artifact-content and must-not-exist assertions are inexpressible in the corpus and stay in unit pins | Win **866/866** · ⚠ example not re-run on the 3 non-Windows legs |
| 2026-08-16 | `f0695b7` | **AP5/AP6 close-out**, 509 files: `scripts/check-diagnostic-codes/check-diagnostic-codes.py` (the ordinal-allocation gate, built after two lanes both took `0xD029`) · `-Werror=switch` tree-wide at one chokepoint (closes G-711) · the ISA-mismatch diagnostic + its unsuppressable row · corpus arming | **All four legs 865/865**: Win · WSL gcc · qemu-aarch64 strict · macOS arm64 |
| 2026-08-14 | `867fa81` | **AP6 in flight.** Resolver + driver wiring landed (the `D_PlanNotLanded` reject is gone); git acquisition; per-target library channel; wrong-format guard at both binders; 3 latent spawn defects fixed; recursive corpus staging. **Two reds found by the merged-tree baseline and fixed**, one of them a test its authoring lane never compiled | merged-tree build **rc=0, 0 warnings** · ctest **859/861** → both reds fixed · balance **982→982 OK** |
| 2026-08-12 | `ca2c6721` | DSS Axis + DSS HIR plan rework | — |
| 2026-08-14 | *(in flight, session 2)* | **Inline-asm P5 wave 2.** Operator rulings taken (§0.1): reuse `ReturnPiece`, ZERO new value-producing opcodes; producer-declared piece source; the `ResultPiece` rename; **`asm goto` WITH outputs BUILDS this cycle**; `%N` is structural. ★ The ruling's own §8 contingency **FIRED** — `ReturnPiece`'s payload carries two facts with one stored (§0.2). ✔MEASURED the four constraint forms vs the core (§0.3): `"=r"` already works, `"+r"` and `"=&r"` are core gaps. ★ Found by reading, not by report: **regalloc cannot see the per-instruction pool at all** — 3 hand-rolled per-opcode-only union sites. Lane V (7 diagnostics) DONE; Lanes 1/G/T killed mid-flight by a context exhaustion and resumed. | ⬜ **not run — UNCOMMITTED**; dirty-tree ctest was **862/863**, sole red `anchor_registry_guard` |
| 2026-08-14 | *(in flight)* | **Inline-asm P5 — embedded `__asm__` in C.** Scope corrected (the `.temp` plan's "P3+P4" is retired numbering; the work is P5). Reference spec measured on gcc+clang (§2). Carrier + `asm goto` decided (§3). Three blockers refuted (§4). Wave 1: mutation-helper fail-open **fixed** + a vacuous shipped pin caught; LIR carrier and target vocabulary in flight. | ⬜ **not run — UNCOMMITTED** |
| 2026-08-14 | — | Two in-passing fixes: a **stale red-on-disable recipe** pointing a mutator at `c-subset.lang.json` for a row that lives in `asm.lang.json`, and an unannotated `[[nodiscard]]` discard (live `-Wunused-result`). Plus the arm64 `.cfi_*` asymmetry folded into its owning row. | balance 982→982 |
| 2026-08-13 | `d4c2836b` | **PR #52 merged.** AP5: build-lifecycle hooks, `dependsOn`, the composition-verb table | — |
| 2026-08-13 | `f3057f42` | **PR #51 merged.** DSS Axis + DSS HIR plan rework — and the asm-cycle content (`4969e9e2`, `e5b60f6c`, `e42ae5a5`, `75ca4034`) | — |
| 2026-08-13 (post-push) | — | Two findings after the cycle closed, neither moving a verdict: a build watcher that could observe FAILURE but not SUCCESS (the producer wrote `BUILD OK` to stdout only), and a **stale anchor figure** — `1018` quoted, tree measures `1019`, parent `989` | guard OK 1019 · balance 983→983 |
| 2026-08-13 | `e42ae5a5` | **Unwind lands**: DWARF CFI + `.eh_frame` on ELF/Mach-O execs (gdb unwinds 4 DSS frames) and in ELF `.o` (9 frames vs 2 stripped) · 2 silent pe64 unwind miscompiles · interior labels end-to-end · **2 false-green red-on-disable mechanisms found** | **Win 851/851 · WSL 851/851 · arm64 594/594 strict** |
| 2026-08-13 | `75ca4034` | asm-anchor burn-down: net −4 anchors; a `.s` calls libc and RUNS | Win 838/838 · ⚠ **WSL + arm64 NOT run** |
| 2026-08-13 | `e5b60f6c` | Second assembly dialect (arm64). **Shipped 2 silent miscompiles** — negative scalars lost their sign; `[x29,#-8]` read as scale | Win 831/831 · ⚠ **1 leg of 3** |
| 2026-08-12 | `4969e9e2` | Inline asm P1+P2 — assembly becomes its own source language | — |
| 2026-08-12 | `60eb8ed8` | **PR #50 merged.** C23 burn-down: silent stringize miscompile, `__VA_OPT__`, GNU spellings, UCRT migration finished | — |
| 2026-08-11 | `0ecec160` | ELF copy relocations **deleted** — name-scoped copy reloc silently emptied glibc's `environ` alias set | 5/5 build · 2 legs by execution |
| 2026-08-10 | `3e86a187` | **PR #48.** pe CRT → UCRT; MIR call-site signature checking | — |
| 2026-08-03 | `f7c378be` | **PR #46.** SQLite compiled from full upstream source, suite green | — |
| 2026-07-20 | `4ccd6c6f` | **PR #47.** Static linking all formats · long double F80/F128 · type identity | — |
| 2026-07-15 | `d0c132c3` | **PR #41.** Cross-toolchain relocatable objects — DSS `.o` links + runs under gcc | — |
| 2026-07-09 | `c7a5377f` | **PR #36.** C23 FC16 + release-optimizer perf arc (>30 min → ~2 min) | — |
| ≤2026-07-08 | — | 🧠 Compressed: C23 FC17/17.5 (`_BitInt`, `thread_local`), C11 `<threads.h>`, arm64 Mach-O, `<stdbit.h>`, Apache-2.0 relicense (PRs #36–#45) | — |
