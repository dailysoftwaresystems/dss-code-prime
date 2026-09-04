# P60 — PREPARED, NOT STARTED

> **Written 2026-09-04 at the end of the P59 session, by the orchestrator, because the session
> ran out of budget after P59 landed and P60 was dispatched.** The four P60 lanes were created
> and killed within a minute; **they produced nothing and their worktrees are removed.** Nothing
> in the repository is half-done.
>
> ⚠ **THIS DOCUMENT EXISTS BECAUSE THE P60 BRIEFS LIVED IN A SESSION SCRATCHPAD THAT IS NOW
> GONE.** Everything a next session needs to dispatch P60 without re-deriving it is inlined
> below. It is a PLAN, not a record — ⚠ **every figure in §1 must be re-derived before it is
> acted on**, per this project's standing rule that a count is a dated inventory.
>
> Authority order is unchanged: `.plans/_handoff.md` (READ FIRST) →
> `_deferred-anchor-registry-production.md` → `-harness.md` → the plans → git log.

---

## §1 — WHERE WE ARE

| fact | value at the time of writing |
|---|---|
| branch | `feature/c23-conformance-burndown-6` |
| HEAD | `8143b691` |
| P59's own commit | `98a8c8ba` — committed **and pushed**, `b1f31420..98a8c8ba` |
| on top of it | two commits from the concurrent **Axis** governance workstream, `.plans/` only — **no source, tests or config touched**, so they collide with nothing planned here |
| working tree | clean; `.worktrees/` holds only `.manifests` |
| production **P0** | **0 rows** |
| production **P1** | ~84 rows |
| registry OPEN | ~467 (plans-side ~339; total ~806) |

**The four-leg gate, ✔MEASURED at P59's folded tree, every leg through `scripts/run-gate/`:**

| leg | result |
|---|---|
| Windows x86_64 | **2054 / 2054** |
| Linux x86_64 (WSL2) | **2054 / 2054** |
| macOS arm64 (Apple Silicon) | **2028 / 2028** |
| Linux arm64 (native VPS) | **2028 / 2028** |

⚠⚠ **THE CROSS-LEG IDENTITY IS NOW `N − 26 = M`, NOT `N − 24 = M`.** P59 added two repo guards
(`doc_census_guard`, `doc_census_selftest_guard`), and the `repo-guard` label is applied
automatically to every ctest entry whose name ends in `guard`. **Do not quote 26 either** —
re-derive it from the configure line `repo-guard label applied to N test(s)` or from
`ctest -N -L repo-guard`. Two comments in the tree hard-coded **18** and were stale by eight
before anyone noticed.

---

## §2 — WHAT P59 LEFT BEHIND, IN ONE PARAGRAPH

Ten rows closed, one opened (minted and closed in-cycle), net −9 counted. **Both P0s were
silent miscompiles and neither was on the queue when the cycle began** — one surfaced by
re-banding a P2 after probing whether its construct was merely deferred or actually
accepted-and-wrong, the other reported by a lane working outside its own grant. The full
account, the eight blind-but-green instruments, and the traps are in **`_handoff.md` §0** and
are not repeated here.

★ The one methodological result worth carrying into P60: **P58 doubted the ROW; P59 doubted the
GUARD.** Eight instruments were green, correct, and structurally incapable of seeing the defect
beside them. When something passes, ask what it could not have seen.

---

## §3 — THE P60 PLAN: FOUR LANES

Chosen from the P1 production band on the standing ruling (*production errors first, in priority
order; previous cycles' pending items matter too*). **File sets were intersected before
dispatch** — the four are disjoint. Cap is 4; the orchestrator is a lane too and takes the
registry, the docs and the gate.

### 3.1 — Lane `rc` — the recursion residue that CRASHES, and a gate blind to it

**Row:** `D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED` (P1).
**This is the highest-value row in the band and should be dispatched first.**

It serves the operator's own ruling — *"big projects like sqlite will for sure explode the
stack"*: convert to an explicit heap work stack, and **the nesting limit survives as a COUNTER
THAT STILL FAILS LOUD**.

✔**P59 turned it from theoretical into measured.** Built with the **Visual Studio 18 / MSVC**
generator, three ctest entries **SEGFAULT** at the base commit — `core/test_deep_type_layout_costs_heap`,
`analysis/syntactic/test_parser_speculation_ceilings`, `mir/test_deep_nesting_costs_heap` —
confirmed by **three independent observers**, each with a **byte-exact revert control** proving
no cycle change was responsible, and reproduced with the three run **alone** (not a `-j`
artefact). ⇒ It does not fail loud. **It crashes.**

⚠⚠ **AND THE GATE CANNOT SEE IT.** `build/dbg` is **Ninja + mingw-w64 g++**, and it passes all
three. The toolchain that shows the defect is not the toolchain that is measured; the three
greens are a **margin, not a proof**.

🧠 **UNPROVEN HYPOTHESIS — the lane's first job is to measure it, not inherit it:** MSVC Debug
frames (`/Ob0 /Od`) are fatter, so the same uncapped depth overshoots the same 1 MB Windows
stack. **What would settle it and has not been done: a stack trace or a frame-size measurement
at the crash.**

**Closing requires BOTH, and this is stricter than the row's original plan:**
1. a cap proven against the **fattest supported frame**, with a MEASURED per-frame worst case, a
   stated stack budget, and a **re-derivable** margin — not calibrated to whichever toolchain the
   gate happens to build with;
2. a proof that **runs where the gate can see it** — either the MSVC configuration joins the
   gate, or those tests assert a **BOUND** (a measured stack high-water mark, or a fail-loud
   diagnostic at the cap) instead of merely completing.

⛔ **DO NOT CLOSE THIS ROW ON A GREEN NINJA RUN.**

**File set:** `src/core/types/**`, `src/analysis/syntactic/**`, `src/mir/**`, `CMakeLists.txt`
(gate half only), `tests/{core,analysis/syntactic,mir}/**`.
⚠ The row's site list also reaches `src/analysis/semantic/**` and `src/ffi/**`; those are OUT
OF SCOPE for this lane because sibling lanes hold them — report, do not take.
⚠ Editing `CMakeLists.txt` can change the `repo-guard` count and therefore the cross-leg
identity. Any guard added or renamed must be stated in the lane's report.

### 3.2 — Lane `dl2` — finish the descriptor/role migration P59 measured but could not land

**Row:** `D-CONFIG-DESCRIPTOR-LIBRARY-LITERAL-DUPLICATES-THE-FORMAT-ROLE-TABLE` (P1).
Most of the discovery is done and is **in the row**; read it rather than re-measuring.

✔**The coincidence:** 49 descriptor files, 31 carry a `library` map, **69** (descriptor, format)
entries, of which **67 restate a declared role's image**. Sharpest instance, one symbol: the
three `-exec` formats declare `processExit{role:"cLibrary", importMangledName:"exit"}` while
`stdlib.json` **also** declares `exit` with a literal.

✅ **P59 landed the fail-loud half:** `tests/link/test_descriptor_library_role_agreement.cpp`
refuses a divergence between the two owners, keyed on **`(format kind, image) → owning role`**
because the image alone is ambiguous. A role repoint is now RED instead of a silent load
failure. **Extend it; do not weaken it.**

⚠⚠ **THE FORK THAT MUST BE SETTLED FIRST, AND IT IS WHY P59 STOPPED.** Only **7 of the 24**
shipped format documents declare a `cLibrary` row — the four `-exec` plus the two elf `-pie`.
The 17 that do not include every `-staticlib`, `-dyn`, `-dll` and `-dylib`, all live build
targets. ⇒ A role resolved against the **active document**, refusing loud when undeclared (the
correct refusal under this project's bar), **would break every static-library and DLL build that
includes a C header the day the first descriptor migrates.** Either declare the missing rows, or
key resolution on the format **FAMILY**.
🧠 **Recommendation, explicitly unverified:** the FAMILY — the suite's own
`RuntimeLibraryRoles.EveryFlavourOfAFormatKindNamesOneProviderPerRole` asserts every flavour of
a format KIND names one provider per role, and declaring 17 rows nothing resolves against would
be inert config. **Check that test says this before building on it.**

✔**The plumbing, measured, needs no new library dependency** (`RuntimeLibraryTable` is a *core*
type): a trailing defaulted `dss::RuntimeLibraryTable const*` on `analyze()`, threaded to the
three ffi entry points in `semantic_analyzer.cpp`, passed as `&format.runtimeLibraries()` from
`compile_pipeline.cpp` where `format` is already in scope.

⚠ Keep the two `libm.so.6` literals — **but NOT `kernel32.dll`**, which P59 refuted: it IS the
pe `systemPrimitives` image and both descriptors spelling it are genuine matches.
⚠ DSS **eager-imports** every function a descriptor lists, so a wrong image breaks **every**
binary's load, not one.

**File set:** `src/ffi/**`, `src/link/object_format_schema*`,
`src/analysis/semantic/semantic_analyzer.{cpp,hpp}`, `src/program/compile_pipeline.{cpp,hpp}`,
`src/dss-config/shippedLibs/**`, `src/dss-config/object-formats/**`, `tests/ffi/**`, and its own
test file under `tests/link/`.

### 3.3 — Lane `pe` — the C23 `#embed` family

**Rows:** `D-PP-EMBED-PARAMS`, `D-PP-EMBED-ANGLE`, `D-PP-EMBED-MACRO-ARG` (all P1).
⚠ `D-PP-EMBED-STREAMING` is deliberately **excluded**: its gate is a resource-size condition
(>16 MiB) rather than a conformance one, and its budget already fails loud.

⚠⚠ **THE UNUSUAL PART, AND IT SHOULD SHAPE THE WHOLE LANE.** ✔MEASURED 2026-09-04, each
reference probed separately with a plain quoted `#embed` as the **control**: **gcc 13.3.0 and
clang 18.1.3 refuse `#embed` ENTIRELY** — plain, angle and parameterized — at `-std=c17`,
`-std=c2x` **and** `-std=c23`. ⇒ **DSS is AHEAD of both available implementation references**,
which already ship the plain quoted form here. The gap is measured only against the **ISO C
vertex** (C23 §6.10.4), the union's last resort.

⇒ **Two consequences:** the oracle is the **standard text, not a compiler** — so be explicit
about which clause each behaviour comes from and say where the standard is ambiguous rather than
inventing; and there is **no accept-vs-refuse disjunction to lean on**, which makes a
misreading harder to catch.

★ Angle delimiters are matched by **schema token KIND** (`hasIncludeAngleOpenToken` /
`hasIncludeAngleCloseToken`), never the `<`/`>` bytes — reuse them; a private angle path here is
the slow agnosticism break, and the config comment says so.
⚠ `__has_embed` with a parameter clause currently answers NOT_FOUND(0) per C23 **precisely
because parameters are unsupported.** Decide and STATE what that answer becomes.
⚠ `limit` is the trap for the witness: a resource shorter than the limit cannot distinguish
"limit applied" from "limit ignored". Make the resource longer and assert the count.

**File set:** `src/analysis/preprocess/**`, `src/dss-config/sources/c.lang.json`,
`src/core/types/preprocess_config.hpp` and its loader `grammar_schema_json.cpp`,
`tests/analysis/preprocess/**`, a new example directory.

### 3.4 — Lane `mo` — two Mach-O emission defects, in the order the row demands

**Rows:** `D-LINK-MACHO-IMAGE-STATIC-FN-EMITTED-N-EXT` and
`D-LINK-MACHO-OBJECT-SYMTAB-MISALIGNED` (both P1). **The Mac is UP.**

**Row 1:** both image nlist builders stamp `N_SECT | N_EXT` on every defined function, so a
`static` is emitted with the external bit set where clang emits no `N_EXT`. ⚠ **Not an ABI
leak** — dyld resolves `dlsym` through the export trie, and an Apple-clang `dlopen` probe on
real Apple Silicon confirmed the symbol is NULL. The fix **re-shapes `LC_DYSYMTAB`'s
local/extdef bands** (locals sort first) and the indirect-symbol indices, which is why it needs
its own witness rather than riding on a name-only change.

**Row 2:** the `MH_OBJECT` writer's symtab offset has no alignment step (✔measured ≡ 4 mod 8),
and ✔**measured harmless** — a relocatable carries no `__LINKEDIT`, so the check that refused
the image tier never runs, and Apple `cc` links and runs a DSS `.o`.

⛔⛔ **DO NOT FIX ROW 2 BY ANALOGY — the row forbids it and says why.** The `.o` tier's
byte-identity across the image fix is currently a **CONTROL** proving the image change did not
leak. ⇒ **Do Row 1 first and use that byte-identity as its control**; only then do Row 2, with
its own witness: an Apple `cc` link **and RUN** of the changed `.o`, plus a byte-diff explaining
exactly what moved.

★ A second instance on the same surface is recorded and was deliberately not taken: a weak
CANONICAL on the two Mach-O exec arms is the same silent downgrade a sibling row closed for
aliases; widening the refusal is **one line**, and it was left because a new refusal rejects
inputs a macOS corpus leg may be compiling green today. **The Mac being up is what makes this
the cycle to settle it** — if taken, prove the corpus still builds there.

**File set:** `src/link/format/macho.cpp`, its own new file under `tests/link/`,
`tests/link/CMakeLists.txt` (append only — do not touch lane `dl2`'s entry), a new example.

---

## §4 — MECHANICS A NEXT SESSION MUST NOT REDISCOVER

- **Lane worktrees:** `bash scripts/lane-worktree/lane-worktree.sh add <name>` /
  `remove <name>`. They live in `.worktrees/`, which must **never** travel to a gate host.
  `scripts/lane-fold/lane-fold.py fold <lane> [--apply]` folds; `list` shows state.
- ⚠⚠ **`-G Ninja` IS LOAD-BEARING IN A LANE'S CONFIGURE LINE.**
  `cmake -S . -B build/<lane> -G Ninja -DCMAKE_BUILD_TYPE=Debug`. Without it, CMake picks the
  **Visual Studio** generator on this host — not the mingw/Ninja toolchain `build/dbg` uses —
  and the three deep-recursion tests of §3.1 SEGFAULT at the base commit, so a lane sees three
  reds it did not cause. `CMAKE_BUILD_TYPE` is also IGNORED by a multi-config generator, and
  `ctest` there needs `-C Debug` or every test reports "Not Run". Two P59 lanes lost time to
  this before the brief was corrected.
- ⛔ **`lane-fold` REFUSES a fold when the destination drifted**, which is correct and happened
  twice in P59. **A straight copy would revert the earlier lane's work while reporting
  success.** The remedy is a real three-way merge — every lane branches from the same commit, so
  `git merge-file <main> <base> <lane>` is doing the merge it was designed for — then re-run the
  fold naming each reconciled path with `--settled <path>`. **`--settled` is an assertion that
  you already merged; it is not a `--force`.**
- ⚠⚠ **AND A CLEAN THREE-WAY MERGE IS NOT A CORRECT MERGE.** In P59 two lanes appended arms to
  the same corpus example from the same base and **both reached for `return 18` / `return 19`**;
  the merge had zero conflicts and the exit code was unaffected, but a failing run could no
  longer say which arm failed. **After a merge, check the merged ARTEFACT for semantic
  collisions the text could not show.**
- **Gate:** `wsl.exe -e bash scripts/wsl-leg/wsl-leg.sh`,
  `... scripts/remote-leg/remote-leg.sh --carriage macos`, `... --carriage arm64-vps`. Every leg
  goes through `scripts/run-gate/` so a gate that never ran cannot report green.
- **Docs that must be refreshed at fold time**, both of which ratchet and will red otherwise:
  `python scripts/check-doc-census/check-doc-census.py --write` (corpus figures in
  `examples/README.md`) and `python scripts/check-plan-citations/check-plan-citations.py --write`
  (lowers the positional-citation ceiling; it only ever lowers).
- **The registry has one door:** `scripts/anchors/{write,set,read}-anchor`. Never hand-edit a
  table; never hand-*read* a cell (storage escapes `|`, and a raw-line read compounds it).

---

## §5 — STANDING CONSTRAINTS (unchanged, and none is optional)

- ⛔ **NEVER `git stash` / `git checkout --` / `git clean` / `git reset`** in this tree — a
  concurrent governance workstream shares it, and its commits land on this branch (two did
  during P59). The only sanctioned undo is a scratch-directory copy made **before** editing.
  The same prohibition holds on the macOS and arm64 hosts; use `scripts/leg-tree/` there.
- **Stage by explicit path, never `git add -A`.** Every commit `-s` (DCO). **Check
  `git config user.email` before committing.**
- **Any throwaway script that writes or deletes goes in a FILE**, written with the Write tool —
  never an inline `bash -c`, never a quoted heredoc, never inline `python -c`. A heredoc
  collapses a doubled backslash; this cost time again in P59 and a P60 lane hit it in its first
  minute. Such a script must assert it is not inside the repository by comparing
  **`os.path.realpath` prefixes**, never substrings.
- From Git Bash never `wsl.exe bash -c` with an interpolated variable; from PowerShell always
  `wsl.exe -e`. Over ssh, **anything with quotes, pipes or `&&` must be ONE argument**.
- **Never cite a line number** in a plan or a row — cite a symbol, a comment id or an anchor id.
  **Never wrap an anchor id across two lines**: a wrap does not fail, it makes the id invisible
  to every grep and mints a false one.
- **Never kill a process by name.**

---

## §6 — OWED BEYOND THE FOUR LANES

- **`D-TARGET-ENCODING-WIDTH-GUARD` keeps P1.** `(unsigned long long)ld` is still walled on both
  axes, deliberately and pinned: the x87 half needs a **conditional** sequence the straight-line
  lowerer cannot emit, and shipping the arm64 half alone would make the same cast compile on one
  target and refuse on the other. F16 remains a separate, still-true residual.
- **Eleven P1 rows were re-verdicted GATED → OPEN in P59** after measuring that gcc and clang
  compile and RUN six of the constructs their *"wait for a consumer"* gates were holding. The
  atomics trio (RMW, `_Atomic(T)` specifier form, the `<stdatomic.h>` monomorphized surface),
  `D-CSUBSET-TGMATH-SURFACE`, `D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL` and
  `D-CSUBSET-SEH-EARLY-EXIT` are all now honest OPEN work and are the obvious P61 candidates.
  ⚠ Several of them collide on `c.lang.json` or `src/mir/**`; intersect file sets before pairing.
- **`D-CSUBSET-SEH-LABEL-ADDR` stays GATED on purpose** and is the control that shows the P59
  sweep was reading rows rather than flipping them: `&&label` is a GNU extension MSVC lacks,
  inside a `__try` that gcc lacks, so **no vertex can compile the combination** and the union
  does not require it.
- **Two divergences measured in passing and not fixed**, recorded on
  `D-CSUBSET-VLA-SIZEOF-TYPEFORM`: `sizeof(R)` where `typedef int R[n]` (gcc and clang give 12,
  DSS refuses) and `_Alignof(int[n])` (both give 4, DSS refuses). Loud, below the union.
- **`MirLoweringConfig::charIsUnsigned` is still a plain `bool` defaulting to `false`** — the one
  remaining place where "not supplied" silently means SIGNED. Every channel P59 added is an
  `optional` that refuses instead.
- **The exact repair for the VLA multi-declarator teardown is a HIR one** — give the declarator
  group a node that is not a `Block`, in `cst_to_hir.cpp`'s `lowerVarLike`. P59's MIR fix is
  deliberately conservative in a statement position and errs **late, never early**.
