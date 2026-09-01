# Project Config (`.dss-project.json`) — Specification

> The **project config** points the driver (`Program::compileProject`) at *what to build*:
> which language, which artifact profile, which targets, which sources. It is the
> file-driven counterpart to the `dsscp` CLI flags. Owned by plan 06
> (`artifactProfile`, AP2/AP3/AP5/AP6) + the `program/` driver layer.
>
> Companion spec: the per-**language** declaration of which profiles a language *supports*
> lives in [`language-config-spec.md` §11.6](./language-config-spec.md) (`artifactProfiles[]`).

---

## 1. Shape

```jsonc
{
  "language":         "c",                       // required — resolves to a shipped .lang.json
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
  "dependsOn":        [{ "path": "../libfoo" }],        // optional — prerequisite projects, RESOLVED and composed in (§2.6)
  "dependencyArtifactCache": {                          // optional — cross-BUILD dependency artifact cache; see §2.8
    "enabled": true,                                    //   all three members REQUIRED when the object is present
    "rootOverrideVariable": "MY_DSS_CACHE_DIR",         //   NAMES the env var that overrides the location
    "eviction": "prune-superseded" },                   //   or "retain"

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
| `sources` | **yes** | non-empty array of non-empty strings | The source files — each entry is a literal path **or a glob pattern** (`D-AP2-SOURCES-GLOB`, see §2.1). In the **root** manifest, relative entries resolve against the process working directory; in a manifest reached through `dependsOn`, against **that manifest's own directory** (§2.6). An absolute entry resolves directly under either. |
| `output` | no | non-empty string when present | A user output hint. **Parsed + type-validated, but its path routing is not yet wired** (`D-AP2-OUTPUT-ROUTING`) — artifacts currently land at the per-target convention (§5). |
| `artifactName` | no | non-empty string when present, **no path separators** | The base **name** for the emitted binary (no extension). Absent ⇒ the source stem (unchanged). A project build routes each target's artifact to `<output-dir>/<formatName>/<artifactName-or-stem><ext>`; the base dir is the `--output` flag (or the default `<cwd>/target`). It is a bare *name*, not a path — a value with `/` or `\` fails loud at load (`C_MalformedJson`), and the router additionally rejects any name that would resolve **outside** the output dir (a `..` component, or a drive/root prefix) with a fail-loud `D_ArtifactNameEscapesOutputDir` (§5), so a bare name can never escape `--output`. The name + per-platform-subdir half of `D-AP2-OUTPUT-ROUTING` (§5, §7). |
| `includes` | no | array of non-empty strings | Quote-include search dirs (C 6.10.2). The file-driven form of the CLI `-I <dir>` (`Program::setIncludeDirs`). |
| `defines` | no | array of non-empty strings | `NAME[=VALUE]` preprocessor macros. The file-driven form of the CLI `--define` (`Program::setUserDefines`). |
| `resolveLibraries` | no | array of non-empty strings **or `{"path","importName"}` objects** | Library paths whose export surfaces resolve + validate this build's externs. The file-driven form of the CLI `--resolve-library <path>[=<import-name>]` (`Program::setResolveLibraries`). See §2.3. |
| `stackReserve` | no | JSON **unsigned** integer > 0 | The per-**program** stack reserve, in **bytes**. The file-driven twin of the CLI `--stack-reserve <bytes>`. Unlike the three arrays this is a **scalar** and therefore cannot merge — the **CLI wins** (§2.4). Absent ⇒ the object format's declared default stands. |
| `preBuildScripts` | no | array of `{"run","runOn"}` objects | Commands run **before** the build. `run` is an **argv vector**, spawned directly — never a shell. See §2.5. |
| `postBuildScripts` | no | array of `{"run","runOn"}` objects | Commands run **after** a build that **succeeded**. Same entry shape as `preBuildScripts`. See §2.5. |
| `dependsOn` | no | array of `{"path"}` **or** `{"git","ref"?}` objects | Prerequisite projects, **resolved recursively** and folded into this build by the composition verb the *dependency's* `artifactProfile` declares (§3). See §2.6. |
| `dependencyArtifactCache` | no | object with **all three** of `enabled` (boolean), `rootOverrideVariable` (non-empty string), `eviction` (`"prune-superseded"` \| `"retain"`) | The cross-**build** content-addressed cache for **dependency** artifacts. Read off the **root** manifest only — a dependency's own copy is never read, the same ruling as its `targets[]` (§2.6) and its `output`. Absent ⇒ no cache: nothing is looked up and nothing is written. See §2.8. |

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
- **Base directory.** In the **root** manifest, patterns resolve relative to the **process working
  directory** (the same base a literal source uses); an **absolute** pattern resolves directly. Only
  the subtree under the pattern's literal leading prefix is walked (`src/**/*.c` never scans a sibling
  `build/`). A manifest reached through `dependsOn` uses **its own directory** as that base instead,
  for both halves — literal *and* glob (§2.6). One function answers "which files does this manifest
  name" for both cases (`src/program/project_sources.hpp`), so the two bases cannot drift into two
  policies.
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
- **Working directory** = the base that manifest's own `sources[]` resolve against (§2.1), because a
  hook that writes `generated/main.c` and a manifest that reads `generated/*.c` must mean the same
  directory or the feature would not compose with itself. For the **root** manifest that is the
  **process working directory** — the driver passes the *empty-path sentinel*, which
  `spawnAndWaitInherit` documents as "inherit the caller's current directory", rather than a
  materialized `current_path()`: materializing would add a failure mode and let the two answers drift.
  For a manifest reached through `dependsOn` it is **that dependency's own directory** (§2.6), which
  is why the runner takes cwd as a *parameter* rather than reading the process cwd — one runner, one
  policy, no caller-kind branch.

**No timeout, no output capture.** A pre-build script is arbitrary user work — a code generator, a
submodule sync, a download — and there is no defensible number of seconds after which the compiler
knows better than the operator; a wall clock here would turn a slow-but-correct build into a
nondeterministic failure that reproduces only on a loaded CI machine. Your Ctrl-C and your CI job
timeout are the right instruments. The child **inherits** this process's stdio, so its progress and
error text stream to your terminal live and interleaved with the build rather than being buffered
until it finishes.

**Ordering against the rest of the build.** Within one manifest, `Program::compileProject` runs the
two profile gates (§4) and the CLI/manifest flag merges **before any hook**, then the pre-build
hooks → `sources[]` glob expansion (§2.1) → routing + compile → the post-build hooks. Two
consequences worth stating: a build that is going to be **refused** never runs your codegen hook (a
refused build must not write files into your tree on its way to saying no), and a **profile
mismatch** is caught before any hook runs. `dependsOn` resolution (§2.6) precedes this manifest's own
compile by necessity — a `SourceMerge` dependency contributes sources this build then compiles, and
an `ArtifactLink` dependency contributes an artifact this build then links — and each resolved
dependency runs **its own** hooks, in its own directory, under the rule stated here (§2.6).

### 2.6 `dependsOn` — prerequisite projects, resolved

`dependsOn` declares the projects this one needs. The driver **resolves** them: it finds each
dependency's own `.dss-project.json`, walks *its* `dependsOn` recursively, and folds every resolved
dependency into this build according to the **composition verb** that dependency's `artifactProfile`
declares (§3) — contributing sources, or building a separate artifact this build links against.

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

**A `path` dependency must be a DSS project, or the build fails loud.** The resolver looks for a
`.dss-project.json` **at the root of the named directory**; there is no search upward, no default
manifest name, and no "treat the directory as a bag of sources" fallback. A directory with no
manifest at its root is `D_DependencyManifestNotFound` (`D019`), and the message names both the
manifest that was looked for and the **absolute path** it was looked at.

> ★ **The distinction this code exists for is the whole reason it is not `D_FileNotFound`.** A `path`
> naming a directory that **does not exist** is `D_FileNotFound` — the ordinary "the thing you named
> is not there". `D019` is the *other* fact: the thing you named **is** there and is not a DSS
> project. That is overwhelmingly a wrong-**level** path — `../lib/src` written for `../lib` — and
> the two have different fixes, which is the split rule this codebase allocates codes by. Collapsing
> them would send a reader hunting for a missing directory that is sitting right where they put it.

**Composition is dispatched on the DEPENDENCY's own `artifactProfile`, through the verb.** Never on
a profile **name**, and never on the *consumer's* profile — the consumer's profile says what *this*
project produces, which has no bearing on how somebody else's product folds in. Each registered
profile's row carries the verb (§3), so the fork has exactly three arms no matter how many profiles
are registered:

| Dependency's verb | Profiles as shipped | What the driver does | Reject |
|---|---|---|---|
| `SourceMerge` | `module` | The dependency's expanded sources join **this** project's compilation and are lowered with them. It contributes **no separate artifact to this build** — nothing of its own is linked in, because what crossed the boundary was source, not a binary. ⚠ That is a statement about this ROLE, not about the profile: a `module` project **built standalone is a library and does emit an artifact** (it is served by all ten `lib`/`staticlib` formats, and the container comes from its own `<target>:<format>` spec). A module you could not build standalone would be one you could not test or get a diagnostic from until somebody imported it. | **Same `language` required** — `D_DependencyLanguageMismatch` (`D01C`). |
| `ArtifactLink` | `lib`, `staticlib` | The dependency is **built to its own artifact**, and that artifact is threaded into this target's `resolveLibraries`. The two builds stay separate. | — (see the format derivation below for `D022` / `D023`.) |
| `NotConsumable` | every other registered profile | **Fail loud** — the profile is a terminal deliverable. | `D_DependencyArtifactProfileUnsupported` (`D01B`). |

Two asymmetries in that table are deliberate and are not oversights:

- **`SourceMerge` requires the same `language`; `ArtifactLink` makes no language claim at all.**
  Merged sources are parsed by **the consumer's** grammar, so a language difference is not a
  preference mismatch but a guaranteed parse failure — one that would otherwise arrive as a pile of
  `P_UnexpectedToken`s pointing into a file the reader never wrote and may not know is in the build.
  Rejecting at resolve time names the actual cause once. An `ArtifactLink` dependency is consumed as
  a **built binary**, so its source language is irrelevant to the consumer: **cross-language linking
  is the entire point of the FFI surface** and must stay legal.
- **`NotConsumable` fails CLOSED, and an *unregistered* profile name is a different complaint.**
  Absorbing a `cli` dependency as sources would splice its `main()` into the consumer, and "linking"
  one is not something the profile can offer. But a profile name that is not registered at all (a
  typo — `"modul"`) reports the unknown **name** with the registered list
  (`C_UnknownArtifactProfile`), never the terminal-profile complaint: `dependencyCompositionForProfile()`
  returns an empty optional for an unregistered name precisely so the two stay distinguishable
  (`core/types/artifact_profile.hpp`). Every verb is a valid *instruction*, `NotConsumable` included;
  "no verb" is the absence, and the absence must not be reported as an instruction.

**Relative paths get two different bases, and that is correct.** The **root** manifest resolves its
relative `sources[]` and hook paths against the **process working directory** — unchanged, shipped
behaviour (§2.1), and it stays unchanged. A manifest reached through `dependsOn` resolves against
**its own directory**. Anything else would make a dependency's meaning depend on where the *depender*
happened to be invoked from — the same manifest would name different files on two machines.

> ★ **The LITERAL half of that rule matters exactly as much as the glob half, and it is the half that
> is easy to get wrong.** ✔MEASURED and recorded at `src/program/project_sources.hpp`: a `sources[]`
> entry with no glob metacharacter used to be kept verbatim and opened later against the **process**
> cwd. `"sources": ["src/lib.c"]` is the overwhelmingly common form, so re-basing only the glob
> expansion would have left the half everybody writes pointing at the wrong tree — and it would fail
> by **reading the consumer's own `src/lib.c`**, if it has one, rather than by failing loud. That is
> a silent miscompile, not a missing input. Both halves re-base together, through one function, or
> neither does.

**Which platforms a dependency is built for: the CONSUMER's, derived.** A dependency's own `targets[]`
is **not consulted** when it is built as a dependency. It still governs a standalone build of that
project; it simply does not participate here.

> ★ **The reason, because the verdict alone reverses what a reader would assume.** `targets[]` states
> *the platforms a project builds for itself*. *"Which platforms this code can support"* is a
> **different statement**. Reading the first as the second makes a dependency whose source is
> perfectly portable — but whose manifest simply has not listed arm64 — **reject an arm64 consumer**:
> the tool refuses work that would have succeeded, with a message that reads as a capability verdict
> while it is actually reporting a bookkeeping gap. That is a false negative carrying a confidently
> misleading diagnostic, and it sends the reader to the wrong place. A dependency that *genuinely*
> cannot serve a platform still fails — as a compile error **inside itself**, pointing at the actual
> unsupported construct. Neither behaviour is silent, so fail-loud does not decide between them; the
> diagnostic's **truthfulness** does. A late true negative beats an early false one. (A dependency
> that wants to decline a platform *deliberately* is designed and deliberately unbuilt — §7.)

For each consumer target spec `<targetName>:<consumerFormat>`, the dependency's object format is the
**unique** format `F` satisfying all three clauses:

1. `F`'s **format kind** equals the consumer format's — `elf` / `pe` / `macho`, a **declared** field;
2. the **dependency's** `artifactProfile` is in `F`'s served set (`artifactProfiles[]`, §3);
3. `crossValidateTargetFormat(target, F)` passes — the **existing** validator
   (`src/program/cross_validate_target_format.hpp`), never a parallel one.

**No format NAME is parsed anywhere** in that derivation: every clause reads a declared field, which
is what keeps it agnostic. It **fails closed** at both ends — **zero** candidates is
`D_DependencyTargetFormatUnresolvable` (`D022`), naming the consumer target, the dependency's profile
and the format kind; **two or more** is `D_DependencyTargetFormatAmbiguous` (`D023`), naming the
target and **every** candidate. Never a silent first match, and never a sort-order tiebreak — a
tiebreak is a policy nobody declared, invisible in the config, that would make the emitted artifact
depend on directory-iteration order. The **consumability gate (`D01B`) runs before this derivation**,
and the order is a correctness dependency rather than a convenience — §6.1 records why.

**The graph: diamonds are fine, cycles are not, and depth is capped.**

- **A cycle fails loud** — `D_DependencyCycle` (`D01A`), with the cycle **path** as its payload,
  because a bare "cycle detected" on a deep graph is nearly unactionable. Detection keys on the
  **resolved canonical** manifest path, so `../lib` and `../../proj/lib` naming one directory are
  correctly one node. Breaking the back edge and continuing was rejected: it would make the resolved
  dependency set depend on which node the walk started from, so two targets in one build could
  legitimately see **different** source sets.
- **A diamond is not a cycle and must not diagnose.** A manifest revisited but **not on the DFS
  stack** is a legitimate shared dependency; the memo table answers it and nothing is reported. The
  same is true of a repeated `path` entry, and of the same git URL at the same `ref` — those dedup
  silently.
- **A deep acyclic graph is caught by a recursion depth cap that fails loud.** Cycle detection
  catches cycles and **nothing else**: a legal, acyclic, very deep chain would otherwise exhaust the
  stack, and per this repo's standing note that failure class is CI-invisible — it surfaces on a
  local MSVC-Debug build, not on the Release/MinGW legs — with a failure mode of *process death and
  no diagnostic at all*. ✔MEASURED, the cap is **64** (`kMaxDependencyDepth`,
  `src/program/dependency_resolver.hpp`). The number is a **budget, not a measurement of the C++
  stack**: no real project nests prerequisites 64 deep, so a graph that does is either generated or
  wrong, and either way the operator needs telling rather than crashing at.

**Git dependencies and the `.dss-deps/` cache.** A `git` dependency is checked out to
`.dss-deps/<name>/`, and `{url, ref, resolvedCommit}` is recorded in the cache's lockfile.

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

★ **There is ONE `.dss-deps` per BUILD — at the ROOT consumer's manifest directory — not one per
node.** Stated rather than left to be inferred, because the obvious reading of "beside the consuming
project's manifest" is "beside *each* consuming manifest", and that reading breaks on the first
`path` dependency whose tree is **read-only** (a vendored checkout, a CI cache mount, somebody else's
repo). The whole graph's checkouts and the one lockfile live under the root's directory. The cache
takes that directory as an explicit argument rather than deriving it, so the process working
directory is never consulted (`src/program/dependency_cache.hpp`).

**Four outcomes, and the discriminator between the last two is not what you would guess:**

| Outcome | Condition | What happens |
|---|---|---|
| **Hit** | A checkout exists, the lockfile records this exact `(url, ref)`, and `HEAD` equals the recorded commit. | **No network access at all** — not a conditional request, not an `ls-remote`, nothing. Exactly one `git` invocation runs: the `rev-parse` that validated the claim. |
| **Miss** | No checkout, or no matching lock entry. | Clone → checkout → `rev-parse` → record. A `--force-git-cache` refresh reports **Miss** too: the flag bypasses the hit short-circuit and nothing else, so everything after it *is* the miss path. |
| **Fetch fallback** | Clone/fetch failed **and** a usable checkout is present. | `D_DependencyGitFetchFallback` (`D01F`) at **Info**, and the build **proceeds** on what it has. This is the offline guarantee. |
| **Acquire failed** | Clone/fetch failed **and** there is no usable checkout. | `D_DependencyGitAcquireFailed` (`D01E`), Error. The sources do not exist on this machine, so continuing would compile against a hole. |

The discriminator between the last two is exactly **"is there a usable checkout"** — never the git
exit status and never the operation name (clone vs fetch), or an offline build's outcome would depend
on how git happened to phrase the failure. "Usable" is answered by asking git (`rev-parse`), not by
`is_directory`: a clone interrupted halfway leaves a directory that is not a repository, and reading
that as a checkout would route a later failure to the "proceed on possibly-stale sources" arm over a
tree that has no sources at all.

**`git` itself is a hard requirement, not a capability probe.** If a manifest declares **any** git
dependency and `git` is absent from `PATH`, the build fails loud with `D_DependencyGitNotFound`
(`D01D`) — **once per build**, however many git dependencies asked, because N copies of "git is not
installed" is noise. There is **no degraded git-less mode**: even a cache *hit* needs git, since the
hit is validated by `rev-parse` and a stale-but-unnoticed checkout is the silent-miscompile direction.

**The `.dss-deps/<name>` derivation is part of the spec, not an implementation detail** — because
name **collisions** are a diagnostic (`D020`), and a diagnostic about a derived value is meaningless
unless the derivation is pinned. The rule: take the **last non-empty `/`-separated segment**, strip
**one** trailing `.git`, compare **case-sensitively**. So `…/org/bar.git` and `…/org/bar/` both give
`bar`; `…/org/Bar.git` gives `Bar`, a **different** name and not a collision; `…/x.git.git` gives
`x.git`, because exactly one suffix is stripped and not repeatedly. A derived name containing a
character outside `[A-Za-z0-9._-]`, or one that cannot be used as a directory under the cache root,
is `D_DependencyDerivedNameInvalid` (`D024`) — the message shows the URL, the derived name **and**
the offending character, because the name appears nowhere in the manifest and a message quoting only
the URL leaves the reader to run the derivation in their head.

> ⓘ **An honest edge, stated rather than quietly widened.** The rule splits on `/` only, so an
> scp-style URL with no path separator after the host (`git@host:bar.git`) derives `git@host:bar` and
> is **rejected** for its `@` and `:`. The common scp spelling (`git@github.com:org/bar.git`) has a
> slash and derives `bar` normally. Teaching the splitter about `:` would be inventing a rule the
> spec does not state, in the one function whose exact behaviour a user-visible diagnostic quotes;
> the remediation the reject already carries — re-spell the URL, e.g. `ssh://git@host/bar.git` —
> resolves it in one edit. The character set is a **conservative intersection**, not a host
> capability test: deriving legality from the host would make one manifest resolve on one machine and
> fail on another.

**Collisions are detected on the DERIVED NAMES, before anything is fetched.** Two git entries that
derive the same `.dss-deps/<name>` are `D_DependencyGitNameCollision` (`D020`) — and the
discriminator is the full **`(url, ref)` pair**, not the URL alone (see §6.1 for both shapes). The
ordering is mandatory, not merely preferable: the second entry's checkout target already exists and
reads as a cache hit, so post-acquisition detection cannot see the same-URL-different-`ref` shape at
all, and would in the other shape complain only after the first repo's working tree was already
damaged.

**`dss-lock.json` is the cache's one state of record** — machine-written, machine-read, **never
hand-edited** (the document carries a `$comment` saying so). It lives inside `.dss-deps/` with the
checkouts it describes, and it holds the whole graph: there is deliberately **no per-dependency state
file**, because the same fact in two places is the drift hazard this codebase rejects everywhere else.
The only other piece of state is the checkout's own `HEAD`, which is git's file and is read rather
than written. **Absent and unparseable are different facts**: an absent lockfile is an empty one with
**no** diagnostic (a first build has no lockfile and a miss is definitional), while a **present but
unparseable** lockfile is `C_MalformedJson` and the build **abandons** — treating it as a miss would
silently re-acquire everything, overwrite the damaged file, and never say that the state it was asked
to reproduce was unreadable. The remediation ("delete it; it is a regenerable cache") is in the
message.

**Re-fetching is an explicit opt-in: `--force-git-cache`, a global flag.** ✔MEASURED against
`src/program/cli_args.cpp`: the CLI has **no `build` subcommand** — its modes are flags
(`--compile` / `--transpile` / `--directory` / `--project` / …) and a bare positional argument is
rejected — so this is a global flag like every other, usable with `--project`. Its `--help` gloss is
imperative and says what it *does*: **"re-fetch git dependencies even when the cache is valid"**.

> ⚠ **The name reads as "force *use of* the cache" while it means "force *refresh of* the cache".**
> That is known; the spelling was chosen deliberately and is not up for renaming. The gloss above is
> the mitigation — read it as the definition, not the name.

The flag bypasses the **cache-hit short-circuit and nothing else**. Every other rule is unchanged by
it: `--force-git-cache` on an offline machine with an existing checkout still takes the fetch-fallback
arm, still emits `D01F` at Info, and still builds. It is a **silent no-op** when the manifest declares
no git dependency at all, consistent with an empty `preBuildScripts`. Without it, a **branch**
dependency is as reproducible as a tag: you get the commit you got last time.

`.dss-deps/` is **git-ignored** — it is *acquired* content, reproducible from the manifest's pins,
never source, and committing it would vendor someone else's history into your repository.

> ★ **Why `D01F` is `Info` and not `Warning`** is a decided design point with a one-line failure
> mode; the measurement and the reasoning are in §6.1. The short version: `--warnings-as-errors`
> promotes every Warning **code-agnostically**, so at Warning severity every project built with that
> flag would fail the moment the network did — the exact outcome the code exists to prevent.

**Hooks run for dependencies too, and the rule mirrors the root's.** `preBuildScripts` run for
**every** resolved dependency; `postBuildScripts` run **only** for an `ArtifactLink` dependency whose
own build returned 0 — the same "only when the compile succeeded" rule the root follows (§2.5). Each
dependency's hooks run with **that dependency's own directory** as their working directory. This is
written down rather than left implicit because *"the hooks silently didn't run"* is otherwise
unobservable: there is no output to be missing and no diagnostic to be absent.

**Where a dependency's artifact lands.** An `ArtifactLink` dependency's product is emitted under the
**consumer's** output base, at:

```
<consumer output base>/deps/<derived-dep-name>/<formatName>/<artifact>
```

The base is the `--output` directory when given, else the default `<cwd>/target` — the **same** rule
the consumer's own artifact follows (§5), and ✔MEASURED one function owns it
(`resolveArtifactOutputDir`, `src/program/program.cpp`), so the path a dependency is written to and
the path the consumer links against cannot be derived twice and drift. The layout is collision-free
by construction, and it **never writes into the dependency's own tree**, which may be read-only for
exactly the reasons the single-`.dss-deps` rule above gives.

**An empty `dependsOn` costs nothing.** A manifest with no dependencies — or with the key absent —
resolves to an empty result **having touched nothing**: no `.dss-deps` directory, no lockfile, no
`git` probe, no filesystem write of any kind. The overwhelmingly common manifest pays nothing for
this feature existing, and the cache is opened lazily, which is where the `--force-git-cache` no-op
above falls out of rather than being a special case somebody remembered to write.

**How far an artifact travels: up to the nearest enclosing build that can ABSORB it.** Transitive
`ArtifactLink` artifacts propagate upward, and a `SourceMerge` node is not a build at all, so they
pass straight through it. **Absorption**, not mere enclosure, is what stops the propagation — and
absorption is a **container** property the repo already dispatches on, never a profile name. An
archive absorbs archives: their members are bundled into it. It **cannot** absorb a shared library —
an `ar` archive records no import — so a shared-library artifact keeps travelling past that archive
to the root, where the final link resolves it. Stopping at the nearest enclosing build regardless of
what it can hold is the version of this rule that looks right and is wrong for exactly one of the
four shapes (a `staticlib` depending on a `lib`), and its symptom is an undefined symbol whose cause
is two hops away.

**Merged source order: the ROOT's own sources come FIRST.** When `artifactName` is absent, the
emitted binary is named from the **stem of `sources[0]`** (§5) — and archive member names follow the
same list. So if a `SourceMerge` dependency's files were prepended, adding a `module` dependency
would silently **rename the output binary**. Order is otherwise preserved exactly: manifest positions
are kept, each glob's own matches are sorted lexicographically, and the first occurrence of a
duplicate wins. Cross-manifest de-duplication normalizes with `weakly_canonical`, because once a
build draws sources from two manifests — one contributing absolute paths, the other relative ones —
the absolute-vs-relative spelling of one file stops being an exotic edge and becomes the normal case,
whose consequence is a duplicate compilation unit and a duplicate-symbol link error that no
diagnostic could tie back to a manifest.

**No per-target `resolveLibraries` vocabulary is added anywhere.** A dependency is built once per
consumer target, so the artifact linked into `x86_64:elf64-…` is a different file from the one for
`arm64:macho64-…` — but that is answered by an **internal** per-target channel in the driver, not by
new manifest or CLI syntax. `resolveLibraries` (§2.3) and `--resolve-library` stay **program-wide**
with their existing merge semantics: program-wide entries first, the resolver's per-target additions
after. A user-facing per-target declaration would be a mechanism with no consumer.

★ **THERE ARE TWO DIFFERENT `dependsOn` KEYS IN THIS REPOSITORY, AND THEY ARE NOT THE SAME FEATURE.**
This one — `dependsOn` in a `.dss-project.json` — is the **product** feature described above: a
user's project declaring the projects it needs. The corpus test harness has a key of the same name in
its `expected.json` manifests (`examples/README.md`), and that one builds a **test fixture**: a
prerequisite artifact the example runner stages so an arm has something to compile against. Two
files, two loaders, two audiences.

> ★ **They must stay independent, and that is the load-bearing half.** If the corpus's fixture path
> went through *this* resolver, a resolver bug would turn examples red that have nothing to do with
> dependencies, and the corpus would lose the one staging path that does **not** depend on the
> feature under test. A rename was considered and **rejected** (2026-08-12): the harness key appears
> in only two manifests, so renaming it was cheap, and renaming the product key was priced across
> loader, tests, spec, plan and the `D_Dependency*` code names — the decision was **neither**. Both
> keep the name; the distinction is carried by **documentation alone**, which is why it is written
> here and in `examples/README.md` rather than left to be re-derived. An undocumented duplicate name
> is how the wrong one gets deleted.

The `D_Dependency*` diagnostics are listed with their semantics in §6.1; the full rule set with its
rationale is plan 06 §5.1 (B.1–B.10).

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

### 2.8 `dependencyArtifactCache` — the cross-build dependency artifact cache (`D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION`)

Two builds of one project at one configuration rebuild every `dependsOn` prerequisite from scratch,
because nothing outlives the process. This member declares the policy for a **content-addressed**
store that lets the second build **serve** a dependency's artifact instead of recompiling it.

```jsonc
"dependencyArtifactCache": {
  "enabled": true,
  "rootOverrideVariable": "MY_DSS_CACHE_DIR",
  "eviction": "prune-superseded"
}
```

- **★ The ROOT manifest only.** A dependency's own copy is **never** read — the same ruling that
  ignores its `targets[]` (§2.6) and its `output`. Caching is a property of *the build*, and a graph
  whose nodes each declared a policy would make "is this build cached?" a question with *N* answers
  and no owner. The root's policy is propagated onto every sub-build alongside the build
  configuration, the job count and the executor.
- **★ It caches DEPENDENCY artifacts, not the root's own.** The root build is the thing the operator
  is running: its artifact is the deliverable they will inspect and its sources are the ones being
  edited. A prerequisite nobody is editing is the shape a content-addressed cache serves well.
- **All three members are required when the object is present**, and the degenerate spelling
  **rejects** rather than aliasing: `{"enabled": false}` alone would mean exactly what *omitting the
  key* means, said less clearly, and a cache whose location override is unnamed is environment
  sniffing with an extra step.
- **`rootOverrideVariable` NAMES an environment variable — the loader never reads one.** Its value,
  when set and non-empty, wins outright over every per-user platform default
  (`%LOCALAPPDATA%`, `$XDG_CACHE_HOME`, `$HOME/.cache`) and is taken **verbatim**.
- **`eviction`** — `"prune-superseded"` keeps one current entry per artifact name in a directory
  (bounded disk); `"retain"` keeps every entry, which is what a branch-switching or bisecting
  workflow needs, since under pruning two alternating revisions evict each other and both stay cold.
- **What the key covers.** The **union of every compilation unit's textual input closure**
  (`CompilationUnit::inputDigest()` — which is why a quote-`#include`d header that appears in **no**
  `sources[]` still moves the key), the target spec, the object format and its archive-writing
  sibling, debug/release, the LTO topology, the stack-reserve request, the artifact's base name,
  every link input by path **and content**, every loaded config document by its loader's own content
  digest, and the compiler's own build stamp.
- **Absent ⇒ no cache at all.** Nothing is looked up and nothing is written, so every manifest
  written before this member existed builds identically.
- **A cache that cannot verify an entry REFUSES it.** An artifact whose key document is missing,
  unreadable or different **fails the build** (`D_FileReadFailed`) rather than being quietly
  recompiled around — that verification is what makes the short path index safe. A key that cannot
  be *computed*, or a store that cannot *write*, is a different matter: it is an optimization made
  unavailable, so the build compiles normally and says so on stderr on **every** affected run.

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
declares that **no** shipped format serves (e.g. `c` + `module`, or `tsql-subset` + `script`)
passes AP2 and is caught by AP3 for **every** target.

An empty declared/served set ⇒ **reject** (fail-closed): a language or format that claims no profiles
is not project-buildable.

> ★ **A DEPENDENCY runs its own AP2 language gate, but not AP3 against its own `targets[]`.** Both
> gates above are about a manifest the user asked to build. A manifest reached through `dependsOn`
> still has its own `artifactProfile ∈` its own language's declared set checked — otherwise a `toy`
> manifest declaring `"module"` (which `toy` does not declare, §4's table) would be source-merged
> with no gate anywhere. What it does **not** get is AP3 against the targets *it* lists: its object
> format is **derived from the consumer's** target instead (§2.6), and clause (2) of that derivation
> — the dependency's profile must be in the candidate format's served set — *is* the format gate,
> applied to the format that will actually be used rather than to a list the dependency wrote for its
> own standalone build.

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
| `D_FileNotFound` | the project file can't be opened, or a hard I/O error occurs mid-read; **or** a `sources[]` **glob** pattern matched no files (§2.1) — the message names the unmatched pattern; **or** a `dependsOn` `path` names a directory that **does not exist** (§2.6 — a directory that exists but holds no manifest is `D019` instead, and the split is deliberate). |
| `D_DirectoryScanFailed` | a directory could not be read while expanding a `sources[]` glob pattern (§2.1). |
| `C_MalformedJson` | invalid JSON; non-object root; an **unknown** top-level key (`$`-prefixed keys excepted, §2.7); a field of the wrong type; a non-string / empty array entry; an empty `output` string; an empty `artifactName`, or one containing a path separator (`/` or `\`); a `stackReserve` that is not a positive unsigned integer (§2.4); any malformed `preBuildScripts` / `postBuildScripts` entry — unknown member, missing/empty `run`, empty `runOn`, unrecognized `runOn` token (§2.5); any malformed `dependsOn` entry — unknown member, both/neither of `path`+`git`, or `ref` without `git` (§2.6). Also the one **non-manifest** file this list covers: a `.dss-deps/dss-lock.json` that is present but unparseable, which **abandons** the build rather than being treated as a cache miss (§2.6). |
| `C_MissingField` | a required field (`language` / `artifactProfile` / `targets` / `sources`) is absent, an empty string, or an empty array. |
| `D_ArtifactProfileNotSupported` (`D0010`) | the language gate (§4). |
| `D_ArtifactProfileFormatMismatch` (`D0011`) | the format gate (§4). |
| `D_ArtifactNameEscapesOutputDir` (`D0015`) | a project `artifactName` resolved to a path **outside** the routed output directory (a `..` component, or a drive/root prefix) — the routing containment boundary (§5). Remediation-distinct from `D_OutputDirCreateFailed` (mkdir I/O): fix the `artifactName` to a plain filename. |
| `D_InvalidTargetSpec` / `D_SchemaLoadFailed` | a `targets[]` entry that doesn't parse as `<name>:<format>`, or names a format that won't load — emitted by the delegated compile (the gates skip such a target rather than double-report). |

### 6.1 Build-lifecycle + dependency codes (`D017`–`D024`)

The codes for `preBuildScripts` / `postBuildScripts` (§2.5) and `dependsOn` (§2.6). The build-lifecycle
pair is `D_ScriptSpawnFailed` / `D_ScriptExitedNonZero`; the dependency surface is every `D_Dependency*`
code, spanning **`D019`–`D024`** — the table below is the list, and
`core/types/parse_diagnostic.hpp` is the source of truth for both.

> ⚠ **The surface is named and ranged here, never counted.** A hand-maintained cardinal ("the eight
> `D_Dependency*` codes") is precisely the sentence that rots the next time a slot is allocated — and
> this one already did, having said *eight* while the band grew past it. If you need a count, derive
> it from the rows below or from the enum; do not write one down. Note also that the range is **not**
> contiguous by topic: `D021` (`D_SuppressRequestIgnored`) is a reporter-policy code that landed
> between the two halves of the dependency band, and the gap was kept rather than closed because the
> number is the operator-visible identity (`error[D0020]`) and renumbering an allocated code rewrites
> a published name.

All are **driver-band** (`D_`) because the decision is made at project-load time, before any grammar
or compilation unit exists to hang a source span on — the same placement argument that put
`D_ArtifactProfileNotSupported` here. Every split below is a **remediation** split: two conditions
share a code only when they share a fix.

| Code | Severity | When | Live? |
|---|---|---|---|
| `D_ScriptSpawnFailed` (`D017`) | Error | A script entry could **not be spawned at all** — `run[0]` is not on PATH, is not executable, or the OS process-creation call refused before the child ever ran. No exit status exists in this state. | ✅ emitted |
| `D_ScriptExitedNonZero` (`D018`) | Error | A script entry spawned and terminated with a **non-zero** status (abnormal termination — signal / structured exception — reports here too; the spawn layer's prose discriminates). Fail-loud, never fail-soft: a pre-build codegen step that silently failed would let the compile proceed against **stale** generated sources and produce a green build of the wrong bytes. | ✅ emitted |
| `D_DependencyManifestNotFound` (`D019`) | Error | A `path` dependency's directory **exists but has no `.dss-project.json` at its root**. Remediation-distinct from `D_FileNotFound`: that is "the thing you named is not there"; this is "it *is* there but is not a DSS project" — overwhelmingly a wrong-level path (`../lib/src` instead of `../lib`). | ✅ emitted |
| `D_DependencyCycle` (`D01A`) | Error | Recursive resolution reached a manifest **already on the DFS stack** (A → B → A, or any longer ring). Keyed on the **resolved canonical** manifest path, so two spellings of one directory are one node. A revisited-but-not-on-stack manifest is a **diamond** — a legitimate shared dependency — and must not diagnose at all. | ✅ emitted |
| `D_DependencyArtifactProfileUnsupported` (`D01B`) | Error | A resolved dependency's `artifactProfile` is **not consumable as a dependency** (§3's `NotConsumable` verb). A third axis, distinct from both existing profile rejects: `D0010` is "the *language* does not declare it", `D0011` is "the *format* does not produce it", this is "the profile is valid for itself but is not a thing another project can depend **on**". Fail-closed on an unrecognized/absent profile. | ✅ emitted |
| `D_DependencyLanguageMismatch` (`D01C`) | Error | A **source-merge** dependency declares a `language` different from the consumer's. Scoped deliberately to that shape: merged sources are parsed by *this* project's grammar, so a language difference is a guaranteed parse failure that would otherwise surface as a pile of `P_UnexpectedToken`s in a file the user never wrote. Makes **no** claim about binary-artifact dependencies — cross-language linking is the whole point of the FFI surface and stays legal. | ✅ emitted |
| `D_DependencyGitNotFound` (`D01D`) | Error | A `dependsOn` entry names a git URL but **`git` is not on PATH**. Split from `D01E` because the remediation is environmental and concrete ("install git"); split from `D017` because that code covers programs the **manifest** names while this covers the one program the **driver** reaches for. Emitted **once per build**, not once per git dependency. | ✅ emitted |
| `D_DependencyGitAcquireFailed` (`D01E`) | Error | **First** acquisition failed and there is **no existing checkout** to fall back to. Hard failure — the dependency's sources simply do not exist on this machine, so continuing would compile against a hole. | ✅ emitted |
| `D_DependencyGitFetchFallback` (`D01F`) | **Info** | The same network failure **with a usable checkout present**, which is **reused** — the build **proceeds** on possibly-stale sources. This is the offline-build guarantee (a laptop on a train; CI with a flaky network); the notice exists so "possibly stale" is never silent. The discriminator against `D01E` is exactly "is there a usable checkout", never the git exit status or the operation name. **★ Info is a decided design point, not an oversight — see the note below. Do not "tidy" it to Warning.** | ✅ emitted |
| `D_DependencyGitNameCollision` (`D020`) | Error | Two git entries deriving the same `.dss-deps/<name>` — either two **distinct** URLs, or the **same** URL declared twice with **different** `ref`s. Whichever is acquired second would clobber the first or be silently skipped, and the build would compile against a dependency it did not ask for. Detected on the **derived names before acquisition**, so no working tree is damaged by the time it complains — and for the same-URL-different-`ref` shape that ordering is *mandatory*, since after acquisition the second entry's checkout target already exists and reads as a cache hit. The same URL with the **same** ref is not a collision — that is the diamond case and dedups silently. The discriminator is the full `(url, ref)` pair. | ✅ emitted |
| `D_DependencyTargetFormatUnresolvable` (`D022`) | Error | For one of the **consumer's** target specs, **no** shipped object format can produce the **dependency's** artifact — zero candidates from the three-clause derivation (§2.6). Names all three coordinates the search ran over: the consumer target, the dependency's `artifactProfile`, and the format **kind**. Remediation-distinct from `D0011`, which is "the format *you named* does not serve the profile *you declared*": here the format was **derived**, so the user never named it and cannot see it — the fix is to ship or declare the missing backend for that kind, or drop the unservable target from the consumer. | ✅ emitted |
| `D_DependencyTargetFormatAmbiguous` (`D023`) | Error | The same derivation returned **two or more** candidate formats for one consumer target. Fails **closed**, naming the target and **every** candidate — never a silent first match, and never a sort-order tiebreak, which would be an undeclared policy that made the emitted artifact depend on directory-iteration order. | ✅ emitted — **no shipped config can produce it** (see below) |
| `D_DependencyDerivedNameInvalid` (`D024`) | Error | A git dependency's **derived** cache directory name is unusable — a character outside `[A-Za-z0-9._-]`, no non-empty last segment at all, or a name that cannot be a directory under the cache root. Fails loud rather than transliterating: folding the offending characters to `_` would manufacture a **second, invisible** collision source on top of the one `D020` exists to catch, and the resulting report would look like a compiler bug rather than a manifest problem. The message shows the URL, the derived name **and** the offending character, because the name appears nowhere in the manifest (§2.6). | ✅ emitted |

> ⓘ **`D023` is a fail-closed guard, not dead code, and that distinction is worth keeping.**
> ✔MEASURED at the code's own allocation (`core/types/parse_diagnostic.hpp`) over all 24 shipped
> object formats × the 2 shipped targets: there are ten reachable
> `(kind, target, ArtifactLink profile)` triples and **each has exactly one** qualifying format — ten
> triples, ten unique answers — so no shipped configuration can produce two candidates today. The
> guard is allocated **before** it can fire on purpose: the alternative is a derivation with no ≥2
> branch — i.e. one that silently takes `candidates[0]` the day someone adds a format — and that
> defect would otherwise ship in the same commit that creates the possibility. Recorded at the same
> site and load-bearing here: uniqueness is **not** a property of the format set on its own. `cli` has two ELF candidates per arch (`…-exec` and
> `…-pie`, same `machine`, both declaring `cli`), so the triple (elf, cli, x86_64) genuinely has two
> answers — it never reaches the derivation only because `cli` is `NotConsumable` and `D01B` rejects
> it first. **The order of those two gates is a correctness dependency, not a convenience.**

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
| plan 06 §B (`dependsOn` resolution) | **Realized** (§2.6). The resolver, both composition arms, git acquisition, the `.dss-deps/` cache and its lockfile, `--force-git-cache`, and every `D_Dependency*` code (§6.1) are built; the `D_PlanNotLanded` refusal this row used to describe is gone. |
| plan 06 §B (dependency hooks) | **Realized** (§2.6). `runBuildScripts` takes its working directory as a **parameter**, and the dependency caller now passes that dependency's own directory; the root caller still passes the inherit-cwd sentinel (§2.5). |
| `D-DEPS-DEPENDENCY-CANNOT-DECLINE-A-TARGET` | A dependency has no way to **decline** a platform it genuinely cannot serve: builds are consumer-driven (§2.6), so such a dependency fails as a compile error inside itself rather than as a clean resolve-time reject. Designed and **deliberately unbuilt** — no shipped dependency needs it, and a mechanism with no consumer is the defect in the other direction. **Trigger:** the first real dependency that must decline a target. The fix when it fires is a declared **constraint** (not a target list) on axes the config already carries — format kind, container, artifact profile — checked through `crossValidateTargetFormat`; **absent constraint ⇒ build whatever is asked**, so the common case keeps zero bookkeeping and cannot drift. |

---

## 8. Examples

A console executable from one C source, for Linux x86-64:

```jsonc
{ "language": "c", "artifactProfile": "cli",
  "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"] }
```

Multi-target (one profile, several formats — one artifact each):

```jsonc
{ "language": "c", "artifactProfile": "cli",
  "targets": ["x86_64:elf64-x86_64-linux-exec", "x86_64:pe64-x86_64-windows-exec"],
  "sources": ["a.c", "b.c"] }
```

A rejected request (lands on `D0011`): `c` *declares* `lib`, and shipped formats *do* serve
it (§3) — but not this one. The fix is the **target**, not the profile:

```jsonc
{ "language": "c", "artifactProfile": "lib",
  "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"] }
// → D_ArtifactProfileFormatMismatch: artifact profile 'lib' is not served by object format 'elf64-x86_64-linux-exec' (serves: cli).
// Fix: target `x86_64:elf64-x86_64-linux-dyn`, which serves `lib`.
```

A pre-build hook that generates the sources a glob then matches, plus a signing step that runs only
on a successful Windows build:

```jsonc
{ "language": "c", "artifactProfile": "cli",
  "targets": ["x86_64:pe64-x86_64-windows-exec"],
  "sources": ["src/**/*.c"],
  "stackReserve": 8388608,
  "preBuildScripts":  [{ "run": ["python", "tools/gen.py", "--out", "src/gen"] }],
  "postBuildScripts": [{ "run": ["pwsh", "-File", "scripts/sign.ps1"], "runOn": ["windows"] }] }
```

Both composition arms in one manifest (§2.6) — the `module` dependency contributes **sources** to
this compilation, the `staticlib` one is **built separately** and linked in:

```jsonc
{ "language": "c", "artifactProfile": "cli",
  "targets": ["arm64:elf64-aarch64-linux-exec"],
  "sources": ["main.c"],                       // ← the ROOT's own sources come FIRST in the merged
                                               //   list: `main` names the binary, and prepending a
                                               //   dependency's files would silently rename it
  "dependsOn": [
    { "path": "../shared_helpers" },           // artifactProfile "module"    → SourceMerge
    { "path": "../libmath" },                  // artifactProfile "staticlib" → ArtifactLink
    { "git": "https://github.com/org/bar.git", "ref": "v1.2.0" }
  ] }
```

Note what is **not** written there: no target list for the dependencies. Each is built for **this**
manifest's `arm64:elf64-aarch64-linux-exec`, with its object format derived — `../libmath` may list
only x86-64 in its own `targets[]` and still serves this build (§2.6).
