# Long branches: what relaxes, what is witnessed, and what the edges cost

A block-relative branch is encoded into a field of fixed width, and that width bounds how far it
can reach. When a function grows past that bound the assembler cannot simply emit the branch, and
what it does instead is called *relaxation*. This page records **which tiers of relaxation a real C
program has been shown to reach**, how that was proved, and — for the tier a gate cannot afford —
what it costs to reach at all.

It exists because "the assembler handles long branches" is the kind of claim that is easy to make
from a unit fixture and hard to justify end to end. The distinction this page keeps is between a
path the assembler takes **when handed the right LIR**, and a path a **C program** reaches.

## The three tiers

| Tier | When | What the assembler does |
|---|---|---|
| **In range** | the displacement fits the field | encode it and move on |
| **Escape** | it does not fit, and the same opcode declares a variant with a wider block-relative slot | promote the instruction and re-encode the function, so the wider word carries the branch |
| **Island** | it does not fit and the field is already the widest its ISA declares | place a landing pad part of the way to the target and hop through it |

The escape word is *quoted from config*, never synthesized: the resolver searches the declared
variants of the same opcode for a self-contained word whose block-relative slot reaches strictly
further. On AArch64 that means a one-word `b.cond <target>` is promoted into the two-word form
whose trailing `b` — a ±128 MiB `Imm26` — carries the far target, with the condition inverted so
the short hop skips it.

The field geometry that decides all of this (first bit, width, scale, PC bias) lives in one table
read by the range check, the patch write and the escape election alike.

## Where each ISA's edges are

| Field | ISA | Reach | Escape available? |
|---|---|---|---|
| `Arm64Imm14` (`TBZ`/`TBNZ`) | AArch64 | ±32 KiB | yes — no shipped opcode emits it today |
| `Arm64Imm19` (`B.cond`) | AArch64 | **±1 MiB** | yes, into `Imm26` |
| `Arm64Imm26` (`B`) | AArch64 | **±128 MiB** | no — widest field the ISA declares ⇒ island |
| `X86Rel32` | x86-64 | **±2 GiB** | no — widest field the ISA declares ⇒ island |

## What a C program has been shown to reach

### ±1 MiB `Imm19` escape — witnessed, and it is an ordinary corpus example

`examples/c/long_branch_imm19_escape` compiles a program carrying two functions that differ only
in size, runs it, and asserts its exit code. Both halves are in one binary so they share a driver,
a pipeline and an assembler pass.

★ **The obvious C shape does not reach this path, and that is the most useful thing on this page.**
An `if (gate) { <1.3 MiB body> }` emits `b.ne <fallthrough>; b <far>` — the far target already
rides the wide `Imm26` word, so the `Imm19` never leaves range and no escape is elected. A test
written that way compiles, runs, passes, and witnesses nothing. Only a disassembly tells the
difference.

The shape that does reach it is a `do { } while` latch: its taken target is the loop **head** and
its fallthrough is the loop exit, so the latch emits the one-word `b.cond <head>` form — and that
word's field *is* the `Imm19`.

Measured with `aarch64-linux-gnu-objdump` over the shipped build, in **both** pipelines:

```
near latch, 0.64 MiB body            far latch, 1.28 MiB body
  cmp   w16, w14                       cmp   w16, w14
  b.ne  <head>       <- ONE word,      b.eq  <next+4>   <- condition INVERTED, hops one word
  mov   w0, #1          not escaped    b     <head>     <- the wide word carries the target
                                       <the function's own return path>
```

So the escape fires above the boundary and does not fire below it, in one image, under both
pipelines. The program then **runs** — the latch is executed, not merely encoded, so a
displacement wrong by a single word would not survive.

### ±128 MiB `Imm26` island — witnessed once, deliberately, and not by any gate

This tier cannot be reached by a program a gate can afford: the branch must miss by more than
128 MiB, so the function must emit more than 134 MB of code. It is therefore a **manual** run:

- the corpus entry lives at
  `.harness-config/runner/actions/manual-end-to-end/corpus/c/branch_island_arm64_edge/`,
  grouped by language the way `examples/<lang>/<name>/` and
  `.harness-config/runner/actions/real-examples/<lang>/<name>/` are
- it declares `manualRun: true`, and the driver skips any entry that does not
- it is executed by `DssHarness run manual-end-to-end`

⚠ **It is registered as no ctest entry at all, and that is structural rather than careful.** Both
corpus harnesses derive every ctest entry from one glob over `examples/*/*/expected.json` — exactly
that ROOT. An entry outside it is invisible to them, so the default test counts
cannot move and the cross-leg identity the gate checks cannot be perturbed. A label would have
relied on every future invocation remembering to exclude it; a disabled entry would still appear in
`ctest -N`.

<!-- ISLAND-EVIDENCE-START -->
#### The run, and what it found

✔MEASURED, one run, on a Windows workstation cross-compiling to
`arm64:elf64-aarch64-linux-exec`, through `manual-end-to-end.py` (the driver the runner ships):
a 3 432-byte source expanding to 262 144 volatile aggregate copies — about 171 MB of code, roughly
28% past the 134 MB edge.

| | |
|---|---|
| peak resident set | **9 250 468 KB ≈ 9.03 GB**, reached at ~255 s and flat thereafter |
| CPU | 100% of one core throughout — CPU time tracked wall clock to within 1% |
| resident set late in the run | fell back to ~150 MB while CPU stayed pinned |
| compiler output at 83.7 minutes | **zero bytes**; no artifact, no diagnostic, no progress line |
| relaxation passes / islands placed | **unreported** — the compiler prints neither, and a zero here would be a measurement failing toward clean |

★★★ **THE ISLAND TIER IS REACHABLE IN MEMORY AND NOT IN TIME.** Memory was never the wall: 9 GB is
ordinary, and the peak arrives early and then *falls*, which says the large allocation is the IR and
that it is released before the phase that did not finish. What did not finish is a CPU-bound phase
with a small working set — so the cost that bounds this tier is compute over an already-built
structure, not the structure itself.

⚠ **This is a bound, not a completion, and it is written as one. The run was STOPPED, it did not
fail.** After 5 023 s — 83.7 minutes — of one core at 100%, with no output of any kind, it was
terminated deliberately so the rest of the round could proceed. The driver's own line records it:

```
manual-end-to-end EVIDENCE branch_island_arm64_edge rc=<killed> wall_s=5023.5
  peak_rss_kb=9250468 emitted_bytes=0 text_bytes=unreported relax_passes=unreported
  islands=unreported host=Windows-AMD64-11
  rss_method="GetProcessMemoryInfo.PeakWorkingSetSize (exact peak)"
```

Nothing here says the island path is correct, or that it is broken — only that reaching it end to
end costs more than 83 minutes of one core on this host, and that whoever re-takes it must budget
for that rather than for the minutes the ±1 MiB tier needs. Whoever re-takes it should expect to leave it running
and should say what it did, whatever that is: a completion, a loud `A_FunctionEncodeAborted` from
`relaxBound`, or another bound like this one. All three are results.

ⓘ **Why the driver kept reporting throughout, and why that matters more than it looks.** A phase is
bounded by output *stall*, never by wall clock, so a run that printed nothing for 83 minutes would be
indistinguishable from a hang and no bound could be chosen for it honestly. The driver emits a
heartbeat every 15 s carrying elapsed time, sampled peak RSS and the compiler's output size, which is
what makes `stallSeconds: 120` a meaningful bound on a phase whose duration is the thing being
measured.

ⓘ **The unit fixtures reach this tier cheaply and this run does not replace them.** `Arm64Imm14`
(`TBZ`/`TBNZ`, ±32 KiB) exists in the geometry table with no shipped opcode precisely so a test can
reach the island machinery with an 8 192-word body instead of a 134 MB one. What the unit fixtures
cannot say is whether a C program gets there — and the honest answer, for now, is that one cannot in
practical time.
<!-- ISLAND-EVIDENCE-END -->

### ±2 GiB `X86Rel32` island — NOT taken, and this says so rather than implying otherwise

x86-64's block-relative slot is `rel32`. Reaching its island tier needs a single function emitting
more than 2 GiB of code, held in the assembler across every relaxation pass — the resolver
re-encodes the whole function from scratch on each one.

**That run was not taken, and the reason is a measurement rather than a budget.** See the scaling
figures below: at the measured ratio of resident memory to emitted code, a 2 GiB function needs
memory in the hundreds of gigabytes. Attempting it on hardware that cannot hold it would produce an
out-of-memory report and nothing else, which is not evidence about the island path. If this is ever
taken it belongs beside the AArch64 entry, on a host chosen for its memory.

## What it costs — measured, so the next person sizes from data

Reaching any of these tiers means emitting a lot of code, and the cost is driven by the **emitted
instruction count**, never by the source size. The construct that gets there for the fewest source
bytes and the fewest front-end statements is a `volatile` aggregate copy: it is dense, and
`volatile` forbids the optimizer eliding, merging or hoisting any of it, so the release pipeline
crosses the same boundary the baseline one does instead of shrinking back under it.

Measured on one host (a Windows workstation, cross-compiling to `arm64:elf64-aarch64-linux-exec`,
baseline pipeline), with a `volatile struct { long a[32]; }` copy as the repeated statement:

| statements | emitted artifact | wall | peak working set |
|---|---|---|---|
| 1 024 | 0.68 MB | ~3 s | — |
| 2 048 | 1.35 MB | ~5 s | — |
| 32 768 | 21.5 MB | 98 s | 2 325 MB |

⚠ **A one-host measurement is a portability claim.** These are this workstation's figures under the
load stated in the lane findings that produced them; another host will differ, and the shape of the
curve is the part to carry forward, not the seconds.

Two ratios are what a future reader actually needs, and both are ✔MEASURED rather than assumed:

- **~655 emitted bytes per statement**, in both pipelines, for that construct. An `int` statement
  gives ~40 bytes in the baseline pipeline and ~24 under `release` — so the naive shape needs
  roughly twenty times as many statements for the same distance.
- **~108 bytes of resident memory per emitted byte.** This is the ratio that decides *feasibility*,
  and it bites long before wall clock does: it is why the x86-64 island tier is out of reach here
  and why the AArch64 one needs a host with room to spare.

## Re-taking any of this

```
# the ±1 MiB escape, as an ordinary corpus entry
ctest -R 'examples/c/long_branch_imm19_escape|integrated_tests/c/long_branch_imm19_escape'

# the ±128 MiB island, manually
DssHarness run manual-end-to-end
```

⚠ `DssHarness run` exit **21** is not a pass — it means nothing failed but a leg reached no
verdict. Read `complete` beside `passed`. The step declares a success witness, and the line that
satisfies it is printed only on the path where at least one entry actually executed.
