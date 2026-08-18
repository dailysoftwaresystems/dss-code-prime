# DSS Code Prime — HANDOFF

> **REWRITTEN at the end of every cycle** (`/dss-cycle` Step 8.1) and **READ FIRST at the start of
> every cycle** (Step 0). §1–§4 are a *replacement* — stale lines are deleted, not appended past.
> **§5 TIMELINE is the sole exception and accumulates.** State is what is true now; the timeline is
> how it got here.
>
> Every claim is labelled ✔**MEASURED** / 📄**DOCUMENTED** / 🧠**INFERRED**. An unlabelled claim here
> is a defect: this file is read by someone with no context, which is exactly when an unmarked
> inference does the most damage.

**Last updated:** 2026-08-18 — cycle **P7** complete (pe64 POSIX layer + `impliedSurface` + three bidirectional conformance fixes); P6 below.
**Branch:** `feature/c23-conformance-burndown-3` · **HEAD:** `b52784a6` ✔MEASURED (Cycle P5c),
this cycle's work committed on top.

---

## 0.00000 ★★★ READ THIS FIRST — THE NEXT ACTION IS A REBASE, AND THREE ANCHORS DO NOT EXIST HERE YET

★★★ **OPERATOR RULING 2026-08-18, IN THIS ORDER, NO STEP SKIPPED:** finish the lanes → commit + push
→ **cut a SECOND backup branch AFTER the commit** → *then* rebase, carefully.
⚠ **`backup/c23-burndown-3-pre-rebase-2026-08-17` at `b52784a6` PROTECTS ONLY THE THREE PRE-CYCLE
COMMITS.** It was cut while 118 files were uncommitted, and a branch ref cannot hold a working tree —
which is exactly why the post-commit branch is a separate, non-optional step rather than a duplicate.

★★★ **AND THE ANCHOR BALANCE IS RED BY OPERATOR DECISION, NOT BY OVERSIGHT.**
`python tools/check-anchor-balance.py` FAILS at **+31** — ✔RE-MEASURED at the P7 tree: **39 rows
opened, 8 closed**, OPEN 992 → 1023, via the gate's OWN `grep '^  + '` / `grep '^  - '` output. (P6
committed at +22; the P7 delta is +9, and every one of those rows is named in §0.000001 or in the
registry with its trigger and closing work.)
Operator ruling, verbatim: *"we'll handle ALL anchors once
rebased."* That is the §B escape the gate's own message names, exercised deliberately — the balance
is DEFERRED to a post-rebase anchor pass, never waived. ⛔ Do not "fix" it by widening the gate.
⚠ **INSTRUMENT WARNING, and it has now bitten three times:** an ad-hoc `grep -o 'D-[A-Z0-9-]*'` over
that output returns **44** "ids" including truncated fragments (`D-MI`, `D-SEMANTIC-ASM-`). **Use the
gate's own line output; never a hand-rolled regex over it.**

⚠⚠ **THREE ANCHORS THE POST-REBASE PASS MUST PICK UP DO NOT EXIST ON THIS BRANCH** — ✔MEASURED, they
arrive with `origin/main`'s single squashed commit `fe031376` (AP6 #53, 555 files):
`D-DEPS-DEPENDENCY-CANNOT-DECLINE-A-TARGET` (in `.plans/06-artifact-profile-plan - tbd.md`),
`D-TEST-CLI-CORPUS-RUNNER-IGNORES-OPTIMIZED-PIPELINES-AND-STDOUT` and
`D-DIAG-CODES-WITH-NO-COMPILED-TEST-REFERENCE` (both in main's registry). ⇒ **the backlog must be
re-derived AFTER the rebase, not carried across it.**
⚠ **REBASE CONFLICT SURFACE, ✔MEASURED:** `tools/check-anchor-registry.sh` (+112 lines upstream, and
edited this cycle), `tools/check-anchor-balance.py` (+38), `CMakeLists.txt` (+62 upstream, and this
cycle created `cmake/DssInstall.cmake` + `cmake/DssBuildStamp.cmake`), and
`src/dss-config/targets/{arm64,x86_64}.target.json` (+2 each — main's new `target.isa`).
★ **`isa` DOES NOT CLOSE `D-CONFIG-FORMAT-DECLARES-NO-UNIFORM-ARCHITECTURE`** — ✔MEASURED
`kTargetArchMachineCodes` is UNCHANGED on `origin/main`. `isa` is a language↔target compatibility
axis, not an arch→machine-code mapping. ⚠ And note the trap it creates: `target.name` is `"arm64"`
while `target.isa` is `"aarch64"` — **two spellings of one architecture**, and the C++ table keys on
the NAME.
⚠ **TWO STALE WORKTREES hold 118 and 124 dirty files at OLD commits** (`dss-wt-bitwise` `75ca4034`,
`dss-wt-movzw` `730e642a`). Detached, so they do not block a rebase; left in place because deleting
two trees of uncommitted files is the operator's call. 🧠INFERRED they are stale leftovers — one
lane found their `object_format_schema.*` edits already merged into HEAD.

---

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
887/887** — the arm64 leg on the **REAL VPS hardware, not qemu**, each via `tools/run-gate.sh` with
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
⇒ So it got a guard: **`tools/check-no-abort-in-tests.py`**, wired into the gate battery, 14 stripper
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
⚠ **Never pipe `tools/run-gate.sh` into `tail`** — the harness then reports the PIPE's rc. It said
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

0. **`NEXT` — REBASE ONTO `origin/main`, THEN RE-DERIVE THE ANCHOR BACKLOG. Nothing else starts
   first.** See §0.00000 for the full ruling, the conflict surface and the three anchors that do not
   exist on this branch yet. The order is fixed by the operator: **commit + push → cut a SECOND
   backup branch AFTER the commit → rebase → re-run all three legs.** ⚠ A green gate taken BEFORE a
   rebase says nothing about after, and two of the four known conflicts are the gate tools themselves.
   Then land the prioritized backlog in **plan-00 §0.1** (asm → ap → production errors → the rest) so
   the stepper and the operator read one list rather than two that drift.
   ⚠ **RE-DERIVE IT FROM THE REGISTRY, NEVER FROM THE PREVIOUS LIST.** ✔The list handed to the
   operator this cycle named `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED` as the top asm priority; it had
   been **CLOSED earlier in that same cycle**. Recalling a row is not measuring it.

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
- ★ Use `tools/run-gate.sh` with a **TOOL-EMITTED** witness (`'ninja: no work to do|^\[[0-9]+/[0-9]+\]'`,
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
