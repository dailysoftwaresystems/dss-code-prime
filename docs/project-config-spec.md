# Project Config (`.dss-project.json`) — Specification

> The **project config** points the driver (`Program::compileProject`) at *what to build*:
> which language, which artifact profile, which targets, which sources. It is the
> file-driven counterpart to the `dss-code-prime` CLI flags. Owned by plan 06
> (`artifactProfile`, AP2/AP3/AP5) + the `program/` driver layer.
>
> Companion spec: the per-**language** declaration of which profiles a language *supports*
> lives in [`language-config-spec.md` §11.6](./language-config-spec.md) (`artifactProfiles[]`).

---

## 1. Shape

```jsonc
{
  "language":         "c-subset",                       // required — resolves to a shipped .lang.json
  "artifactProfile":  "cli",                            // required — one profile (see §3)
  "targets":          ["x86_64:elf64-x86_64-linux-exec"], // required — ≥1 "<targetName>:<formatName>" spec
  "sources":          ["src/**/*.c"],                   // required — ≥1 source path OR glob pattern (§2)
  "output":           "dist/myprog",                    // optional — see §6 (parsed, not yet routed)
  "artifactName":     "myapp",                          // optional — binary base NAME (no ext / path sep); see §5
  "includes":         ["vendor/include"],               // optional — quote-include dirs   (mirrors CLI -I)
  "defines":          ["NDEBUG", "MAX=64"],             // optional — NAME[=VALUE] macros   (mirrors CLI --define)
  "resolveLibraries": ["dist/libfoo.so"],               // optional — extern-resolving libs (mirrors CLI --resolve-library); see §2.3 for the extended entry
  "stackReserve":     4194304,                          // optional — per-PROGRAM stack reserve in BYTES; see §2.4 (CLI --stack-reserve WINS)
  "preBuildScripts":  [{ "run": ["bash", "gen.sh"] }],  // optional — argv hooks run BEFORE the build; see §2.5
  "postBuildScripts": [{ "run": ["./sign.sh"],          // optional — argv hooks run AFTER a SUCCESSFUL build; see §2.5
                         "runOn": ["linux", "darwin"] }],
  "dependsOn":        [],                               // optional — prerequisite projects; PARSED but NOT resolved (§2.6)

  "$comment":         "prose — every `$`-prefixed key is documentation (§2.7)"
}
```

The driver loads this file, validates it, enforces the two `artifactProfile` gates (§4), then
delegates to the existing compile path — routing by source **count** (§5).

---

## 2. Fields

| Field | Required | Type | Meaning |
|---|---|---|---|
| `language` | **yes** | non-empty string | The shipped language to compile (`src/dss-config/sources/<language>.lang.json`). |
| `artifactProfile` | **yes** | non-empty string | The **single** output shape to produce (§3). Singular — the language declares a *set*, the project picks *one*. |
| `targets` | **yes** | non-empty array of non-empty strings | Each entry is a `"<targetName>:<formatName>"` spec (e.g. `"x86_64:elf64-x86_64-linux-exec"`). One artifact is produced per target. |
| `sources` | **yes** | non-empty array of non-empty strings | The source files — each entry is a literal path **or a glob pattern** (`D-AP2-SOURCES-GLOB`, see §2.1). Resolved relative to the process working directory (an absolute entry resolves directly). |
| `output` | no | non-empty string when present | A user output hint. **Parsed + type-validated, but its path routing is not yet wired** (`D-AP2-OUTPUT-ROUTING`) — artifacts currently land at the per-target convention (§5). |
| `artifactName` | no | non-empty string when present, **no path separators** | The base **name** for the emitted binary (no extension). Absent ⇒ the source stem (unchanged). A project build routes each target's artifact to `<output-dir>/<formatName>/<artifactName-or-stem><ext>`; the base dir is the `--output` flag (or the default `<cwd>/target`). It is a bare *name*, not a path — a value with `/` or `\` fails loud at load (`C_MalformedJson`), and the router additionally rejects any name that would resolve **outside** the output dir (a `..` component, or a drive/root prefix) with a fail-loud `D_ArtifactNameEscapesOutputDir` (§5), so a bare name can never escape `--output`. The name + per-platform-subdir half of `D-AP2-OUTPUT-ROUTING` (§5, §7). |
| `includes` | no | array of non-empty strings | Quote-include search dirs (C 6.10.2). The file-driven form of the CLI `-I <dir>` (`Program::setIncludeDirs`). |
| `defines` | no | array of non-empty strings | `NAME[=VALUE]` preprocessor macros. The file-driven form of the CLI `--define` (`Program::setUserDefines`). |
| `resolveLibraries` | no | array of non-empty strings **or `{"path","importName"}` objects** | Library paths whose export surfaces resolve + validate this build's externs. The file-driven form of the CLI `--resolve-library <path>[=<import-name>]` (`Program::setResolveLibraries`). See §2.3. |
| `stackReserve` | no | JSON **unsigned** integer > 0 | The per-**program** stack reserve, in **bytes**. The file-driven twin of the CLI `--stack-reserve <bytes>`. Unlike the three arrays this is a **scalar** and therefore cannot merge — the **CLI wins** (§2.4). Absent ⇒ the object format's declared default stands. |
| `preBuildScripts` | no | array of `{"run","runOn"}` objects | Commands run **before** the build. `run` is an **argv vector**, spawned directly — never a shell. See §2.5. |
| `postBuildScripts` | no | array of `{"run","runOn"}` objects | Commands run **after** a build that **succeeded**. Same entry shape as `preBuildScripts`. See §2.5. |
| `dependsOn` | no | array of `{"path"}` **or** `{"git","ref"?}` objects | Prerequisite projects. **Parsed and shape-validated only — resolution has not landed; a non-empty value fails loud.** See §2.6. |

**The three flag arrays mirror the CLI flags and *merge* with them.** Each is **optional** and defaults to
empty; an **absent** field and a **present-but-empty `[]`** both mean "no entries" (no error). A present value
that is not an array, or an entry that is not a non-empty string, fails loud (`C_MalformedJson`). At build time
`Program::compileProject` **appends** these onto the Program's current state — so a manifest value **adds to**
(never replaces) any flag `Program::run` already stamped from the command line. The two sources compose.

**Unknown top-level keys are rejected** (`C_MalformedJson`) — a typo like `"ouput"` fails loud rather
than being silently ignored, matching the grammar/target/format loaders. The one carve-out is the
`$`-prefixed **documentation key** (§2.7). The message's "recognized fields" list is **derived** from
the loader's own key table (`projectConfigKnownKeys()` / `projectConfigKnownKeyList()`), so the list a
reject shows you can never drift from the set the check actually enforces.

### 2.3 `resolveLibraries` — declaring the recorded import identity (`D-FFI-DECLARED-IMPORT-NAME`)

Reading a library binary answers **two** questions, and an entry can now answer both:

* **which symbols exist** — always the file at `path`;
* **what identity to record for them** — the ELF `DT_NEEDED` / Mach-O `LC_LOAD_DYLIB` / PE
  import-descriptor name the *loader* resolves at runtime.

An entry is therefore **either** shape:

```jsonc
"resolveLibraries": [
  "dist/libfoo.so",                                    // PLAIN — nothing stated
  { "path": "/opt/local/lib/libtcl8.6.dylib",          // EXTENDED — identity stated
    "importName": "@rpath/libtcl8.6.dylib" }
]
```

The recorded identity follows a **three-level precedence**, highest first:

1. the entry's **`importName`** (this field, or the CLI's `=<import-name>` suffix);
2. the binary's **own embedded soname** — ELF `DT_SONAME` / Mach-O `LC_ID_DYLIB` install name /
   PE export DllName, read by the FF1 binary readers (`D-FF1-READER-SONAME`);
3. the **file basename** fallback (`D-FF1-READER-CONSUMER`).

The plain string form is level 2/3 only and is **unchanged** — every manifest written before this
capability behaves byte-for-byte as it did.

**Why level 1 exists:** cross-compiling from a *stand-in* library. Reading Tcl symbols out of a
MacPorts `.dylib` whose `LC_ID_DYLIB` is `/opt/local/lib/libtcl8.6.dylib` would otherwise stamp that
MacPorts prefix into the artifact's `LC_LOAD_DYLIB`, and the artifact would then demand MacPorts at
that exact path on the target Mac — a dyld load failure at runtime, with no build error anywhere.
This is the mechanism a conventional toolchain spells as a sysroot stub, a `.tbd`, or `-dylib_file`:
**symbols from the file you can read, identity from the declaration.** It is object-format agnostic —
the readers normalise all three formats' embedded identities into one field.

The object form also has **no separator character**, so it is the way to express a `path` that itself
contains `=` (which the CLI's last-`=` split cannot).

**Fail-loud rules for the extended entry** (all `C_MalformedJson`): an entry that is neither a string
nor an object; a missing, non-string, or empty `path`; a missing, non-string, or empty `importName`
(the object form exists *solely* to state an identity — use the plain string when stating none); and
any **unknown member** inside the object, so a mistyped `"importname"` cannot silently discard the
identity the entry exists to carry.

### 2.1 `sources[]` glob expansion (`D-AP2-SOURCES-GLOB`)

A `sources[]` entry may be a **glob pattern**. The loader keeps each entry **verbatim** (a glob string
is just a non-empty source string — no filesystem access at parse time); `Program::compileProject` then
**expands** any entry containing a glob metacharacter into its matching files — as a driver pre-pass,
**before** the source-count routing decision (§5) — so a pattern routes exactly as if its matches had
been listed literally. The matcher lives in `core/types/glob_match.hpp` (language / target / format
agnostic — pure path-text matching + a filesystem walk).

- **Literal vs glob.** An entry with **no** metacharacter (`*`, `?`, `[`) is a literal path, kept
  **verbatim** — unchanged behavior (a missing literal still fails downstream at CU build; it is *not*
  newly rejected here). An entry with **any** metacharacter is expanded against the filesystem.
- **Syntax** (standard glob):
  - `*` — any run of characters **within one path segment** (does **not** cross `/`);
  - `**` — a whole segment matching **zero or more** segments (recursive: `a/**/b` matches `a/b`,
    `a/x/b`, `a/x/y/b`; `**/*.c` matches `x.c` *and* `sub/x.c`);
  - `?` — exactly one character within a segment;
  - `[...]` — a character class (ranges `a-z`, negation `[!...]` / `[^...]`).
- **Base directory.** Patterns resolve relative to the **process working directory** (the same base a
  literal source uses); an **absolute** pattern resolves directly. Only the subtree under the pattern's
  literal leading prefix is walked (`src/**/*.c` never scans a sibling `build/`).
- **Determinism.** Matches are **sorted lexicographically**, so the compilation-unit order is stable
  and reproducible across platforms.
- **Overlap composes.** Entries that overlap — two globs matching a shared file, or a literal alongside
  a glob that also matches it — yield the **union of unique files**: each file is compiled **once**
  (cross-entry de-duplication on the normalized path, first occurrence wins). A redundant overlap just
  works; it never produces a duplicate compilation unit.
- **Separator + case.** Patterns are matched on **forward-slash (`/`) segments** — a hand-written
  Windows manifest must use `/` (a backslash `src\*.c` is out of contract; JSON authors and generated
  manifests use `/`). Matching is **case-sensitive** (the POSIX glob default) even on a
  case-insensitive filesystem.
- **Zero match fails loud.** A glob pattern that matches **no files** is an error (`D_FileNotFound`,
  naming the unmatched pattern) — a source pattern that names nothing is a mistake, not an empty
  no-op. (A *literal* entry that matches nothing is **not** rejected here — see above.)
- **I/O failure fails loud.** A directory that cannot be read during expansion fails loud
  (`D_DirectoryScanFailed`) rather than being silently skipped.

Example — `"sources": ["src/**/*.c"]` matching `src/a.c` and `src/lib/b.c` expands to two concrete
sources ⇒ count `2` ⇒ the multi-CU route (§5), exactly as `["src/a.c", "src/lib/b.c"]` would.

> The CLI `--compile <files>` path takes explicit files and is **not** glob-expanded — globbing is a
> project-manifest convenience only.

### 2.4 `stackReserve` — the per-program stack reserve (`D-SQLITE-PE64-FULL-TIER-STACK-DEPTH`)

A **positive** byte count naming the stack the emitted program reserves. It belongs in the manifest
because the number is a property of *this* program's deepest call chain (the motivating case: deeply
nested SQL trigger recursion overflowing the Windows 1 MiB default), so it travels with the project
rather than with an invocation.

- **Shape only at load.** `is_number_unsigned()` rejects a negative *and* a float in one check (so a
  `-1` can never wrap into a huge `u64`), and `0` is rejected explicitly — a zero-byte reserve cannot
  start a program, and *omit the field* is the way to say "take the default".
- **Range and alignment are NOT decided here.** Those bounds are declared by the chosen **object
  format** (`stackReserveControl` in its `.format.json`) and enforced at the linker gate, which also
  **refuses the request outright** on a format that declares no such capability (see
  `src/link/image_request.cpp`).
- **★ Precedence — the CLI `--stack-reserve` WINS.** The three flag *arrays* above merge because
  appending composes; a **scalar** cannot compose — one of the two numbers must be the answer — so it
  gets an explicit rule instead. `Program::compileProject` applies the manifest value **only if**
  `Program::run` left the stamp unset, so an explicit command-line argument overrides a committed
  file (the universal convention), and a user can probe a different reserve while bisecting a stack
  overflow without editing — and risking committing — the manifest.
- Absent ⇒ the object format's declared default stands (unchanged behavior).

### 2.5 `preBuildScripts` / `postBuildScripts` — build-lifecycle hooks

Two optional arrays of the **same** entry shape, declared in the manifest so a build step travels
with the project rather than with an invocation:

```jsonc
"preBuildScripts": [
  { "run": ["python", "tools/gen_tables.py", "--out", "src/gen"] },
  { "run": ["bash", "sync-submodules.sh"], "runOn": ["linux", "darwin"] }
],
"postBuildScripts": [
  { "run": ["./scripts/sign.ps1"], "runOn": ["windows"] }
]
```

**Entry shape — a closed two-key set.** `run` and `runOn`, nothing else; an unknown member (a
mistyped `"runon"`) fails loud `C_MalformedJson` naming the recognized members, and a `$`-prefixed
member is prose (§2.7). Everything below is `C_MalformedJson` on violation.

| Member | Required | Rule |
|---|---|---|
| `run` | **yes** | A **non-empty array of non-empty strings**. `run[0]` is the executable; `run[1..]` are its arguments. |
| `runOn` | no | When present, a **non-empty** array of platform tokens. Absent ⇒ every host. |

**★ `run` IS AN ARGV VECTOR, AND `run[0]` IS SPAWNED DIRECTLY — THERE IS NO SHELL.** Not
`std::system`, not `cmd /c`, not `sh -c`, at any depth beneath the runner
(`src/program/build_scripts.cpp` → `dss::substrate::spawnAndWaitInherit`). The array type is the
contract, not a convenience: a single command **string** would have to be split by somebody, and
every splitter is a different quoting dialect (`cmd.exe` vs POSIX `sh`) — which is both a
portability hole (one manifest meaning two things on two hosts) and an injection surface (a path
containing a space or a `;` becoming executable text). Consequences you can rely on:

- Each element crosses the exec boundary **byte-for-byte**. An argument containing `$HOME`, `%PATH%`,
  `;`, `&&`, `*` or a space is **data** — no glob expansion, no variable substitution, no word
  splitting, no operator parsing.
- **Shell builtins and shell-only spellings do not resolve.** `{"run": ["cd", "x"]}` or
  `{"run": ["echo", "hi", ">", "f"]}` is a mistake; spell it `["bash", "-c", "…"]` if you genuinely
  want a shell, and own that choice explicitly in the manifest.
- A **bare name** in `run[0]` is resolved against `PATH`; a path is used as given.
- **No word may contain a NUL** — an escaped `U+0000` inside a `run` string is a loud
  `C_MalformedJson` naming the entry and element index (and deliberately *not* echoing the byte,
  which is unprintable). It is the one character that cannot cross the exec boundary at all: an argv
  word is handed to the OS **NUL-terminated**, so the word would arrive truncated on POSIX and would
  end the whole Windows command line — silently discarding every argument after it while the hook
  still ran and plausibly exited 0.

**`runOn` — the closed host-token vocabulary.** Exactly `windows`, `linux`, `darwin`
(`src/program/platform_token.hpp`), **case-sensitive**. It is `darwin` (what `uname -s` prints),
never `macos`/`macOS`/`osx`; `windows`, never `win32`. A near-miss is a **loud reject** naming the
offending token *and* the full recognized list — never a silently-never-matching gate that would
quietly drop a build step and produce a subtly different artifact. `runOn` is the **HOST** the
compiler runs on, never the compile target; no codegen/link decision may read it.

- **Absent ⇒ every host** (the unfiltered default).
- **Present-but-empty `[]` is rejected**: "run nowhere" is what *deleting the entry* says, more
  clearly, so the degenerate spelling rejects rather than aliasing. This also gives "absent" one
  unambiguous meaning at the runner.
- An entry whose `runOn` does not name this host is **skipped silently** — no diagnostic, no marker.
  A cross-platform manifest working as designed must not be noisy on two hosts out of three.
- **A `"$runOn"` is prose, not a filter** — the `$` prefix (§2.7) makes the key documentation, so the
  entry has no `runOn` at all and runs on **every** host. A stray `$` therefore *disables* the
  platform gate silently. See §2.7 for why this is stated rather than guarded.

**Ordering, and stopping.** Entries run in **manifest order** — no sort, no partition of
applicable-vs-skipped, no parallelism — because hooks are a *sequence* (entry 2 routinely consumes
what entry 1 produced). The run **stops at the first failure**, which fails the build. Continuing
past a failure would run later steps against inputs that were never generated and bury the one
diagnostic that mattered under a cascade of derived ones. Two remediation-distinct codes, and
exactly one is emitted per run — the one belonging to the entry that stopped it:

| Code | Meaning | Fix |
|---|---|---|
| `D_ScriptSpawnFailed` (`D017`) | The OS **never created the process**, so no exit status exists. | The environment or the `run[0]` spelling. |
| `D_ScriptExitedNonZero` (`D018`) | The child **ran** and returned non-zero (abnormal termination reports here too). | Whatever the script itself printed. |

**When each array runs.**

- **`preBuildScripts` run BEFORE `sources[]` glob expansion (§2.1)** — deliberately, so a pre-build
  script may **generate sources** that a `sources[]` pattern then matches. A hook that ran after
  expansion could only ever generate files the build would not see.
- **`postBuildScripts` run only when the compile SUCCEEDED** (`rc == 0`). A failed build has no
  artifact to post-process, and running a packaging/signing step over the previous build's stale
  output is a silent wrong-bytes hazard. **A post-build script's own failure fails the whole build
  (non-zero exit) even though the artifact exists on disk** — the hook is part of what the author
  declared "building this project" to mean, and reporting exit 0 with a diagnostic on stderr invites
  every CI system on earth to proceed to the next stage. The artifact deliberately **stays where it
  is** rather than being deleted: the compile genuinely succeeded and the bytes are genuinely
  correct, so unwinding it because a deploy script's credentials expired would destroy re-usable
  work.
- **Working directory** = the **process working directory**, the same base `sources[]` globs and
  literal sources resolve against (§2.1) — a hook that writes `generated/main.c` and a manifest that
  reads `generated/*.c` must mean the same directory, or the feature would not compose with itself.
  (The driver passes the *empty-path sentinel*, which `spawnAndWaitInherit` documents as "inherit the
  caller's current directory", rather than a materialized `current_path()`: materializing would add a
  failure mode and let the two answers drift. The runner takes cwd as a *parameter* at all because a
  future `dependsOn` dependency's hooks must run in **that dependency's own** directory — §2.6.)

**No timeout, no output capture.** A pre-build script is arbitrary user work — a code generator, a
submodule sync, a download — and there is no defensible number of seconds after which the compiler
knows better than the operator; a wall clock here would turn a slow-but-correct build into a
nondeterministic failure that reproduces only on a loaded CI machine. Your Ctrl-C and your CI job
timeout are the right instruments. The child **inherits** this process's stdio, so its progress and
error text stream to your terminal live and interleaved with the build rather than being buffered
until it finishes.

**Ordering against the rest of the build.** `Program::compileProject` runs, in order: the
`dependsOn` refusal (§2.6) → the two profile gates (§4) → the CLI/manifest flag merges → the
pre-build hooks → `sources[]` glob expansion (§2.1) → routing + compile → the post-build hooks. Two
consequences worth stating: a build that is going to be **refused** never runs your codegen hook (a
refused build must not write files into your tree on its way to saying no), and a **profile
mismatch** is caught before any hook runs.

### 2.6 `dependsOn` — parsed, **not** resolved

`dependsOn` declares prerequisite projects. **This cycle ships the manifest surface only.** The
entries are read and shape-validated; **nothing is resolved, acquired, built or merged.**

> ★ **A non-empty `dependsOn` REFUSES the build — `D_PlanNotLanded` (`D009`), emitted before
> anything else `compileProject` does.** Accept-and-ignore is the single worst behaviour available
> here: the manifest states a prerequisite, the build reports success, and the artifact is missing
> exactly the thing its author declared it needs — surfacing later as an undefined symbol at link,
> or far worse as a link against a *stale* copy of the dependency that happened to be lying around,
> with nothing pointing back at the key that was silently dropped. The message names how many
> entries were declared and the first one, and tells you the interim route: remove the key, build
> the prerequisite separately, and wire it in via `resolveLibraries` or an explicit `sources[]`
> entry.
>
> Three details are deliberate. **(a)** `D_PlanNotLanded` rather than a new code — its allocation
> defines it as "an entry point reached an arm whose backing plan substrate is not yet shipped", and
> future plan-gated arms re-use it; a fourth "not landed yet" ordinal would say nothing new and
> would be dead the day the resolver lands. It is distinct from
> `D_TargetAbiModelUnsupportedByDriver`, which is a *permanent* architectural exclusion. **(b)** It
> is a member of `kUnsuppressableCodes`, which is load-bearing: `--suppress` must not be able to
> convert this loud reject back into the silent no-op it exists to replace. **(c)** None of the
> eight `D_Dependency*` codes fits, because every one of them describes an *outcome of a resolution
> attempt*; this is the prior condition — no attempt is made at all.

Resolution is the next cycle (plan 06 §B).

An entry names **exactly one** source:

```jsonc
"dependsOn": [
  { "path": "../libfoo" },
  { "git": "https://github.com/org/bar.git", "ref": "v1.2.0" }
]
```

| Member | Rule |
|---|---|
| `path` | A local checkout. Kept **verbatim** — no filesystem access, no normalization, no resolution at parse time (exactly as `sources[]` is). |
| `git` | A remote URL. |
| `ref` | Optional branch / tag / commit. **Only valid alongside `git`.** |

Closed three-key set; an unknown member fails loud. Three shapes are **rejected**, each because the
tolerant reading would silently discard something the author wrote:

- **both** `path` and `git` — two sources for one dependency is ambiguous, and silently preferring
  one makes the other a no-op the user cannot see;
- **neither** — an entry naming nothing is noise;
- `ref` **without** `git` — a ref names a revision inside a git remote and has no meaning for a
  local path, so accepting it would silently ignore the pin.

**Acquisition cache — `.dss-deps/` (decided, not yet built).** A `git` dependency is checked out to
`.dss-deps/<name>/`, with `{url, ref, resolvedCommit}` recorded as cache state.

★ **WHERE IT IS CREATED, because this is a correctness rule and not a convention: `.dss-deps/` belongs
to YOUR project.** It is created beside **your** `.dss-project.json`, inside the repository being
compiled — never inside the dss-code-prime installation or repository, and never in a shared or
user-home location. Compiling a project must not write a byte into the compiler's own tree. A direct
consequence: the cache is **per consuming project**, so two projects that depend on the same git URL
each get their own checkout. That is deliberate — a cross-project shared store would make one
project's build depend on what some other project fetched last, and de-duplicating it would need its
own invalidation and concurrency design, which this is explicitly not. (`.dss-deps/` also appears in
the dss-code-prime repository's own `.gitignore`, but only because that repository contains example
manifests of its own under `examples/` and `real-examples/`.)

The cache is keyed on the **resolved commit**, and a hit is *total*: an existing checkout whose
`HEAD` matches the recorded commit for that `(url, ref)` means the build performs **no network
access at all** — not a conditional request, not an `ls-remote`. So a branch dependency is as
reproducible as a tag; you get the commit you got last time. Re-fetching is an explicit opt-in,
**`--force-git-cache`** on the `build` command, which forces the pull even when the cache is valid.
`.dss-deps/` is git-ignored: it is acquired content reproducible from the manifest's pins, never
source. Offline behavior is a guarantee, not an accident — a network failure **with** a usable
checkout reuses it and proceeds (`D01F`, Info); a network failure with **no** checkout fails loud
(`D01E`). None of this is implemented yet; it is recorded here because the cross-references above
point at it, and the full rule set with its rationale is plan 06 §5.1 B.4.

The eight `D_Dependency*` diagnostics (`D019`–`D020`) are **allocated with their semantics fixed**
(§6) but have **no emit site yet** — they are the resolver's vocabulary, reserved now so the code
space is decided once rather than per-lane.

### 2.7 `$`-prefixed documentation keys

**A key whose name begins with `$` is PROSE, never config, and is skipped by every unknown-key
check** — at the top level *and* inside every nested entry object (`resolveLibraries` entries,
`preBuildScripts` / `postBuildScripts` entries, `dependsOn` entries). One shared predicate,
`dss::detail::isDocumentationKey` (`src/core/types/config_key_vocabulary.hpp`), is the same one the
grammar / target / object-format loaders use, so "documentation key" has exactly one definition in
the codebase.

```jsonc
{
  "$comment":        "these are --project builds, not harness-leg builds",
  "$sourcesComment": "generated by preBuildScripts; do not hand-edit",
  "sources":         ["src/**/*.c"]
}
```

This is a **prefix** rule, not a `$comment` allow-list: a `$comment`-only carve-out would still
reject `$sourcesComment` and re-open the same inconsistency for the next annotation someone writes.
The closed set is **not relaxed** — a non-`$` typo (`"ouput"`) still fails loud, exactly as before.

> ★ **The consequence, stated outright: `$`-prefixing a REAL key silently disables it.**
> `{"run": […], "$runOn": ["linux"]}` parses as an entry with **no** `runOn` — so it runs on *every*
> host, and the platform filter its author wrote is gone with no diagnostic anywhere. Nothing can
> distinguish a deliberate annotation from a typo'd field, because "a `$` key is prose" is the whole
> rule. Two reasons this is a **spec sentence rather than a code change**: it fails in the *safe*
> direction (a hook **over**-runs rather than being silently skipped — the opposite polarity, a step
> that never runs, is the one that produces a subtly wrong artifact), and `$` is a deliberate,
> documented convention, so a loader that second-guessed `$`-prefixed near-misses would be
> second-guessing the feature. **If a manifest's platform gate looks ignored, check for a stray `$`.**

> The FFI shipped-library descriptor schema (`src/dss-config/shippedLibs/README.md`) remains
> **stricter**: it permits `$comment` only, and a `$macrosComment`-style key fails loud there.

---

## 3. Artifact profiles

`artifactProfile` must be a name in the **registered profile set** (loader-owned vocabulary, shared
with the language side — `core/types/artifact_profile.hpp`):

`cli` · `gui` · `lib` · `staticlib` · `script` · `sproc` · `transpile` · `shader` · `hdl` · `module`

See [`06-artifact-profile-plan`](../.plans/06-artifact-profile-plan%20-%20tbd.md) §3 for each
profile's meaning. A profile is only *usable* in a project when **both** gates in §4 accept it.

> ★ **The vocabulary above is a transcription; the table in `core/types/artifact_profile.hpp` is the
> source of truth.** Read `kRegisteredArtifactProfiles` there rather than trusting this line — and
> note that the *order* in that table is load-bearing (every "registered profiles: …" diagnostic is
> derived from it in place), so a new profile is **appended**.

**Each row also declares a composition verb.** A registered profile is not just a name: its row
carries a `DependencyComposition` saying how an artifact built under that profile folds into a
*consumer's* build when it is named in `dependsOn` (§2.6) — `SourceMerge` (contribute sources),
`ArtifactLink` (build separately, link against the product), or `NotConsumable` (a terminal
deliverable; naming it as a dependency is an error). As shipped: `module` → `SourceMerge`;
`lib` and `staticlib` → `ArtifactLink`; every other profile → `NotConsumable`. **The engine
switches on the VERB, never the profile name** — adding a profile adds a *row*, never a branch,
which is the standing agnosticism veto and the entire reason the verb lives in the row.

> **What ships today — MEASURED 2026-08-12 over `src/dss-config/object-formats/*.format.json`
> (24 files).** Three profiles are **served** by a shipped object format, and each serving format
> serves exactly one:
>
> | Profile | Shipped formats that serve it |
> |---|---|
> | `cli` | 7 — the ELF `exec` + `pie` pair on x86-64 and aarch64, the two Mach-O `exec`, and the PE `exec`. |
> | `lib` | 5 — the two ELF `dyn`, the two Mach-O `dylib`, and the PE `dll`. |
> | `staticlib` | 5 — the ELF, Mach-O and PE `staticlib` formats (2 + 2 + 1). |
>
> The remaining 7 formats (the four bare relocatable ELF/Mach-O/PE formats, plus `spirv-1.6` and
> `wasm32-v1`) declare **no** profiles — an empty served set is a **fail-closed reject** (§4), by
> design: a format may only claim a profile it can actually produce. `gui` / `script` / `sproc` /
> `transpile` / `shader` / `hdl` / `module` are registered names a language may *declare* but no
> shipped format *serves*, so a project requesting one is rejected at the format gate until the
> corresponding backend ships.
>
> ⚠ **Do not re-hardcode these counts.** The number of formats serving a profile changes whenever a
> `.format.json` is added or its `artifactProfiles[]` edited; the config directory is the source of
> truth. (This paragraph replaces a claim — *"the only profile any shipped object format serves is
> `cli` (the four runnable exec formats)"* — that was false on **both** halves by the time it was
> read: `lib` and `staticlib` are served, and `cli` is served by 7 formats, not 4.)

---

## 4. The two driver gates

Both gates are a single generic set-membership test (no per-profile-name or per-format branch —
the agnosticism veto). They run **before any compilation**, so a bad profile fails fast and cheap.

| # | Gate | Rule | On failure |
|---|---|---|---|
| AP2 | **Language gate** | `artifactProfile ∈` the language's declared `artifactProfiles[]` | `D_ArtifactProfileNotSupported` (`D0010`) — *fix the request or the `.lang.json`*. Message names the language + lists its supported profiles. |
| AP3 | **Format gate** (per target) | `artifactProfile ∈` each target's object-format `artifactProfiles[]` (the profiles that format *serves*) | `D_ArtifactProfileFormatMismatch` (`D0011`) — *pick a target whose format produces this profile, or ship that backend*. Checked for **every** target. |

The two codes are **remediation-distinct**: `D0010` means the *language* can't produce the profile;
`D0011` means the *chosen format* can't. Note that the format gate is **per target**, so `D0011` is
also how you learn that a perfectly serviceable profile was paired with the wrong format: `lib` is
served by 5 shipped formats (§3), but not by `elf64-x86_64-linux-exec`. A profile the language
declares that **no** shipped format serves (e.g. `c-subset` + `module`, or `tsql-subset` + `script`)
passes AP2 and is caught by AP3 for **every** target.

An empty declared/served set ⇒ **reject** (fail-closed): a language or format that claims no profiles
is not project-buildable.

---

## 5. Routing & output

- **Source count routing** (shared `routesToMultiUnit`, identical to the CLI dispatcher): `>1`
  source ⇒ N independent compilation units the linker merges (`compileUnits`, `cc a.c b.c`
  semantics); `≤1` ⇒ the single-CU path (`compileFiles`). The count is taken **after** glob
  expansion (§2.1) — a 2-match glob routes as two sources, exactly as two literal entries would.
- **Output path.** A **project build** routes each target's artifact to
  `<base>/<formatName>/<artifactName-or-stem><ext>`:
  - `<base>` — the `--output` directory when given, else the default `<cwd>/target`.
  - `<formatName>/` — the **per-platform subdir**, applied to **every** project build
    (single- *and* multi-target), so a project's artifacts never collide across platforms and
    the on-disk layout is uniform. `<formatName>` already encodes machine + OS.
  - `<artifactName-or-stem>` — the manifest's `artifactName` when present, else the source stem
    (the unchanged default). The router enforces **containment**: the resolved artifact must be a
    direct child of the output dir. A name that would escape it — a `..` component, or a differing
    drive/root prefix that `std::filesystem`'s path join would let slip past the loader's separator
    check — fails loud (`D_ArtifactNameEscapesOutputDir`), never silently writing outside `--output`.
  - `<ext>` — from `ObjectFormatKind × objectType` (ELF/Mach-O executable ⇒ no extension; PE
    executable ⇒ `.exe`; relocatable ⇒ `.o`).

  The `output` field does **not** yet redirect `<base>` (`D-AP2-OUTPUT-ROUTING`, §7) — use
  `--output`.

  > **The CLI `--compile` path is unchanged.** A non-project compile keeps the legacy layout:
  > single-target is **flat** at `<output>/<stem><ext>` (no `<formatName>/` subdir), and only a
  > multi-target compile subdir's by `<formatName>`. The forced per-platform subdir + the
  > `artifactName` override apply to **project builds only**.

---

## 6. Loader diagnostics

| Code | When |
|---|---|
| `D_FileNotFound` | the project file can't be opened, or a hard I/O error occurs mid-read; **or** a `sources[]` **glob** pattern matched no files (§2.1) — the message names the unmatched pattern. |
| `D_DirectoryScanFailed` | a directory could not be read while expanding a `sources[]` glob pattern (§2.1). |
| `C_MalformedJson` | invalid JSON; non-object root; an **unknown** top-level key (`$`-prefixed keys excepted, §2.7); a field of the wrong type; a non-string / empty array entry; an empty `output` string; an empty `artifactName`, or one containing a path separator (`/` or `\`); a `stackReserve` that is not a positive unsigned integer (§2.4); any malformed `preBuildScripts` / `postBuildScripts` entry — unknown member, missing/empty `run`, empty `runOn`, unrecognized `runOn` token (§2.5); any malformed `dependsOn` entry — unknown member, both/neither of `path`+`git`, or `ref` without `git` (§2.6). |
| `C_MissingField` | a required field (`language` / `artifactProfile` / `targets` / `sources`) is absent, an empty string, or an empty array. |
| `D_ArtifactProfileNotSupported` (`D0010`) | the language gate (§4). |
| `D_ArtifactProfileFormatMismatch` (`D0011`) | the format gate (§4). |
| `D_ArtifactNameEscapesOutputDir` (`D0015`) | a project `artifactName` resolved to a path **outside** the routed output directory (a `..` component, or a drive/root prefix) — the routing containment boundary (§5). Remediation-distinct from `D_OutputDirCreateFailed` (mkdir I/O): fix the `artifactName` to a plain filename. |
| `D_InvalidTargetSpec` / `D_SchemaLoadFailed` | a `targets[]` entry that doesn't parse as `<name>:<format>`, or names a format that won't load — emitted by the delegated compile (the gates skip such a target rather than double-report). |

### 6.1 Build-lifecycle + dependency codes (`D017`–`D020`)

Ten codes allocated for `preBuildScripts` / `postBuildScripts` (§2.5) and `dependsOn` (§2.6). All are
**driver-band** (`D_`) because the decision is made at project-load time, before any grammar or
compilation unit exists to hang a source span on — the same placement argument that put
`D_ArtifactProfileNotSupported` here. Every split below is a **remediation** split: two conditions
share a code only when they share a fix.

| Code | Severity | When | Live? |
|---|---|---|---|
| `D_ScriptSpawnFailed` (`D017`) | Error | A script entry could **not be spawned at all** — `run[0]` is not on PATH, is not executable, or the OS process-creation call refused before the child ever ran. No exit status exists in this state. | ✅ emitted |
| `D_ScriptExitedNonZero` (`D018`) | Error | A script entry spawned and terminated with a **non-zero** status (abnormal termination — signal / structured exception — reports here too; the spawn layer's prose discriminates). Fail-loud, never fail-soft: a pre-build codegen step that silently failed would let the compile proceed against **stale** generated sources and produce a green build of the wrong bytes. | ✅ emitted |
| `D_DependencyManifestNotFound` (`D019`) | Error | A `path` dependency's directory **exists but has no `.dss-project.json` at its root**. Remediation-distinct from `D_FileNotFound`: that is "the thing you named is not there"; this is "it *is* there but is not a DSS project" — overwhelmingly a wrong-level path (`../lib/src` instead of `../lib`). | reserved |
| `D_DependencyCycle` (`D01A`) | Error | Recursive resolution reached a manifest **already on the DFS stack** (A → B → A, or any longer ring). Keyed on the **resolved canonical** manifest path, so two spellings of one directory are one node. A revisited-but-not-on-stack manifest is a **diamond** — a legitimate shared dependency — and must not diagnose at all. | reserved |
| `D_DependencyArtifactProfileUnsupported` (`D01B`) | Error | A resolved dependency's `artifactProfile` is **not consumable as a dependency** (§3's `NotConsumable` verb). A third axis, distinct from both existing profile rejects: `D0010` is "the *language* does not declare it", `D0011` is "the *format* does not produce it", this is "the profile is valid for itself but is not a thing another project can depend **on**". Fail-closed on an unrecognized/absent profile. | reserved |
| `D_DependencyLanguageMismatch` (`D01C`) | Error | A **source-merge** dependency declares a `language` different from the consumer's. Scoped deliberately to that shape: merged sources are parsed by *this* project's grammar, so a language difference is a guaranteed parse failure that would otherwise surface as a pile of `P_UnexpectedToken`s in a file the user never wrote. Makes **no** claim about binary-artifact dependencies — cross-language linking is the whole point of the FFI surface and stays legal. | reserved |
| `D_DependencyGitNotFound` (`D01D`) | Error | A `dependsOn` entry names a git URL but **`git` is not on PATH**. Split from `D01E` because the remediation is environmental and concrete ("install git"); split from `D017` because that code covers programs the **manifest** names while this covers the one program the **driver** reaches for. Emitted **once per build**, not once per git dependency. | reserved |
| `D_DependencyGitAcquireFailed` (`D01E`) | Error | **First** acquisition failed and there is **no existing checkout** to fall back to. Hard failure — the dependency's sources simply do not exist on this machine, so continuing would compile against a hole. | reserved |
| `D_DependencyGitFetchFallback` (`D01F`) | **Info** | The same network failure **with a usable checkout present**, which is **reused** — the build **proceeds** on possibly-stale sources. This is the offline-build guarantee (a laptop on a train; CI with a flaky network); the notice exists so "possibly stale" is never silent. The discriminator against `D01E` is exactly "is there a usable checkout", never the git exit status or the operation name. **★ Info is a decided design point, not an oversight — see the note below. Do not "tidy" it to Warning.** | reserved |
| `D_DependencyGitNameCollision` (`D020`) | Error | Two **distinct** git URLs derive the same `.dss-deps/<name>` directory. Whichever is acquired second would clobber the first or be silently skipped, and the build would compile against a dependency it did not ask for. Detected on the **derived names before acquisition**, so no working tree is damaged by the time it complains. The **same** URL twice is not a collision — that is the diamond case and dedups silently. | reserved |

**"reserved"** = the code and its semantics are allocated (`core/types/parse_diagnostic.hpp`), but no
emit site exists yet — `dependsOn` resolution is next cycle (§2.6).

> ★ **Why `D_DependencyGitFetchFallback` is `Info` and not `Warning`.** MEASURED in
> `src/core/types/diagnostic_reporter.cpp` (`applyPolicy`): the `--warnings-as-errors` arm promotes
> **every** Warning to Error **code-agnostically** — `if (policy.warningsAsErrors && d.severity ==
> Warning) d.severity = Error;` — with no per-code exemption anywhere in the codebase, and
> membership in `kUnsuppressableCodes` explicitly does *not* exempt a code from that elevation (it
> gates *silencing* only). At Warning severity, therefore, any project built with
> `--warnings-as-errors` would **fail the moment the network did** — destroying the exact guarantee
> this code exists to uphold. `--suppress` is the wrong instrument in the other direction: it would
> silence the staleness notice rather than preserve the build. Info is not promoted by that arm and
> does not count toward `errorCount()`, and it is still **visible at default verbosity**
> (`drainDiagnosticsToStderr` renders every diagnostic with no severity filter and no verbosity gate
> — the CLI has no `--quiet`/`--verbose` — and runs unconditionally on the success path).

---

## 7. Deferred (pinned)

These are intentional gaps, each pinned in the deferred-anchor registry — the doc states them so it
does not over-promise:

| Anchor | Gap |
|---|---|
| `D-AP2-SOURCES-GLOB` | **Realized** (§2.1). `sources[]` entries with a glob metacharacter (`* ? [`) are expanded against the filesystem in `Program::compileProject` before routing; literals are kept verbatim, zero-match fails loud. |
| `D-AP2-OUTPUT-ROUTING` | **Partially addressed.** The `artifactName` field (binary base name) **and** the per-platform `<formatName>/` subdir for project builds are now wired (§5). Still deferred: the **`output` field as an output directory** — it is parsed + validated but does not yet redirect the base dir; use `--output` to set it. |
| `D-AP2-TARGET-NAME-DEFAULT-FORMAT` | `targets[]` require the explicit `:<formatName>` half; bare names (`"linux-x86_64"`) with an inferred default format aren't resolved yet. |
| `D-AP2-COMPILATION-CONTEXT` | the resolved profile is **not** threaded to codegen (entry-symbol / subsystem / extension); deferred until a profile drives a codegen difference its `(target:format)` doesn't already encode (e.g. `gui`). |
| plan 06 §B (`dependsOn` resolution) | `dependsOn` is parsed and shape-validated; **resolution has not landed**, so a non-empty value **refuses the build** with `D_PlanNotLanded` (§2.6). The `.dss-deps/` cache rules, the composition-verb dispatch, and the eight `D_Dependency*` codes (§6.1) are decided but unbuilt. |
| plan 06 §B (dependency hooks) | `runBuildScripts` takes its working directory as a **parameter** so a dependency manifest's hooks can run in that dependency's own tree; the driver has only the ROOT caller today, which passes the inherit-cwd sentinel (§2.5). |

---

## 8. Examples

A console executable from one C source, for Linux x86-64:

```jsonc
{ "language": "c-subset", "artifactProfile": "cli",
  "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"] }
```

Multi-target (one profile, several formats — one artifact each):

```jsonc
{ "language": "c-subset", "artifactProfile": "cli",
  "targets": ["x86_64:elf64-x86_64-linux-exec", "x86_64:pe64-x86_64-windows-exec"],
  "sources": ["a.c", "b.c"] }
```

A rejected request (lands on `D0011`): `c-subset` *declares* `lib`, and shipped formats *do* serve
it (§3) — but not this one. The fix is the **target**, not the profile:

```jsonc
{ "language": "c-subset", "artifactProfile": "lib",
  "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"] }
// → D_ArtifactProfileFormatMismatch: artifact profile 'lib' is not served by object format 'elf64-x86_64-linux-exec' (serves: cli).
// Fix: target `x86_64:elf64-x86_64-linux-dyn`, which serves `lib`.
```

A pre-build hook that generates the sources a glob then matches, plus a signing step that runs only
on a successful Windows build:

```jsonc
{ "language": "c-subset", "artifactProfile": "cli",
  "targets": ["x86_64:pe64-x86_64-windows-exec"],
  "sources": ["src/**/*.c"],
  "stackReserve": 8388608,
  "preBuildScripts":  [{ "run": ["python", "tools/gen.py", "--out", "src/gen"] }],
  "postBuildScripts": [{ "run": ["pwsh", "-File", "scripts/sign.ps1"], "runOn": ["windows"] }] }
```
