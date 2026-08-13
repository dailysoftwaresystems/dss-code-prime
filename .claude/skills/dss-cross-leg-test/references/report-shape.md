# The full report shape — worked detail

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
| macOS      | elf64-arm64    | Linux / VPS          |  ✅  |  ✅   | 14/14 native on VPS   |
| macOS      | elf64-x86_64   | Linux x86_64         |  ✅  |  ⬜   | 14/14 native on WSL   |
| macOS      | pe64-x86_64    | Windows              |  ✅  |  ⬜   | 14/14 native on Win   |
| Linux/WSL  | elf64-x86_64   | Linux (native)       |  ✅  |  ✅   |                       |
| Linux/WSL  | elf64-arm64    | qemu / VPS           |  ⬜  |  ✅   |                       |
| Linux/WSL  | pe64-x86_64    | Windows              |  ✅  |  ⬜   | banner+CRUD+integrity |
| Linux/WSL  | macho64-arm64  | macOS                |  ✅  |  ⬜   | 14/14 native on Mac   |
| Linux/WSL  | macho64-x86_64 | macOS                |  ✅  |  ⬜   | 14/14 Rosetta on Mac  |
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

★★ **AND CARRY THE RUNTIME LIBRARY CLOSURE — a one-file transport produces a failure that
reads exactly like a compiler bug.** ✔MEASURED 2026-08-07, and it nearly reached a commit as
a cross-host emission defect: a macOS-built `sqlite3.exe` was scp'd ALONE to a scratch
directory on Windows and every one of the 14 smoke assertions failed with
`rc=3221225781` = `0xC0000135` = STATUS_DLL_NOT_FOUND. The binary was byte-identical across
the transport, so corruption was already excluded — the obvious reading was "macOS emits a
broken PE". ★ It was killed by DIFFING THE IMPORT TABLES against the Windows-built binary:
`kernel32.dll`, `msvcrt.dll`, `ucrtbase.dll`, `zlib.dll` — **identical**. The artifact was
fine; the probe had left `zlib.dll` behind. Re-run with it: 14/14.
- **Carry the NON-SYSTEM libraries only.** The OS ones (`kernel32`/`msvcrt`/`ucrtbase`, or
  `libc`/`libm`) are resolved by the target's own loader, and shipping them is worse than
  useless — a stale local copy shadowing the system one is its own defect class.
- **The cheapest correct-by-construction transport is the leg's OUTPUT DIRECTORY**, which
  already contains the staged libraries beside the binary (`libtcl8.6.so`, `libz.so.1`,
  `zlib.dll`). Copy the directory, not the file. Hand-picking is how the omission happens.
- ⚠ **A PASSING one-file round trip is not evidence the probe was right.** The earlier
  VPS→WSL ELF cell passed 14/14 from a one-file transport — but only because *that* leg's
  `libtcl`/`libz` happened to resolve from the destination's system paths. On a host without
  them it fails identically, and nothing in the result would have pointed at the probe.

★★ **READ `--expect-source-id` OFF THE BINARY'S OWN VINTAGE, NEVER OFF "the tree" — A
CONCURRENT RUN MOVES THE STAGED TREE UNDER YOUR ARTIFACT.** ✔MEASURED 2026-08-07: two
WSL-built Mach-O binaries were smoke-tested on the Mac and came back **13/14, the single
failure being `source-id-token-exact`, CHARGED TO DSS**. Both binaries report
`2026-08-06 17:20:07 ee8ef5cd…`; WSL's `bld-dss/sqlite3.h` at that moment said
`2026-08-07 05:31:08 fafefb59…`. Neither was wrong — a Windows harness run was IN FLIGHT
against the same `~/src/sqlite`, and its Step-3 `git pull` had moved the tree forward AFTER
those artifacts were built. ⇒ **the expectation was stale, not the binary**, and a probe that
reads the id from whatever the tree says today will fabricate a DSS-charged failure whenever
any other run has pulled since. Ask the ARTIFACT what vintage it is (`<cli> --version`) and
use that, or pin the upstream commit for the whole comparison. This is
[[D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE]] reaching the round-trip probes.

⚠ **`--launcher` and dash-leading tokens:** `--launcher arch --launcher -x86_64` makes
argparse read `-x86_64` as an option and die with `expected one argument`. Use the `=` form —
`--launcher=arch --launcher=-x86_64`. Already anchored as
[[D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION]]; noted here because the
round-trip path is where a human types it by hand.

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
