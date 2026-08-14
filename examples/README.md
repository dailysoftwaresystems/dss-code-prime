# Examples

Curated source programs the DSS compiler must compile + run end-to-end. Each example sits under `<language>/<name>/` and ships:

- a source file (`main.<ext>`) — or several, via a `sources` array for the multi-CU cases (15 examples today) — or, in PROJECT MODE, no committed source at all: a `.dss-project.json` named by the manifest's `project` key owns the input list (1 example today)
- a manifest (`expected.json`) declaring the target spec + expected exit code

**Two runners drive the same corpus, and a capability must land in BOTH:**

- `tests/examples/examples_runner.cpp` — **in-process**, via `Program::compileFiles` (the API + library link path).
- `integrated_tests/runner.cpp` — **CLI subprocess**, driving the built `dss-code-prime` binary (argv parsing, exit codes, filesystem layout, output routing).

One runner enforcing something its sibling shrugs at is a silent harness bug, not a shortcut — that is how `D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT` survived in both for as long as it did. They already share their skip vocabulary, strict parse and emulator lint through `tests/test_support/arm_verdict_ledger.hpp`.

**The run gate is `runOn`, then `emulator` — not "the exec format matches the host OS".** The in-process runner COMPILES every declared target on every host (a cross-format emission regression must surface even on the wrong host, so the zero-diagnostic assert still applies there). It then SPAWNS only if (a) `runOn` names the current host OS, and (b) where the target's arch differs from the host's, the target declares an `emulator` that is on `PATH`. Missing either is a *named skip* in the arm ledger, never a pass: no `emulator` key is a manifest defect the corpus lint reds host-independently, and an absent emulator binary is a machine property that becomes a hard failure under `DSS_STRICT_ARM_VERDICTS=1`. The CLI runner instead binds the FIRST target whose `runOn` matches the host, and ledgers the rest.

The harness ASSERTS strictly: binary on disk, spawn success, no timeout, exit code `==` manifest value, and **zero compile-time diagnostics unless the manifest declares `expectDiagnostics`** — 23 examples do, and those invert the contract: the compile MUST fail with exactly the declared diagnostic set and nothing is spawned. Any drift breaks ctest.

## Manifest schema

```json
{
  "language": "c-subset",
  "source": "main.c",
  "exitCode": 42,
  "optimizedPipelines": [
    { "label": "release", "shippedPipeline": "release" }
  ],
  "targets": [
    {
      "spec": "x86_64:pe64-x86_64-windows-exec",
      "artifact": "main.exe",
      "runOn": ["windows"]
    },
    {
      "spec": "arm64:elf64-aarch64-linux-exec",
      "artifact": "main",
      "runOn": ["linux"],
      "emulator": "qemu-aarch64"
    }
  ]
}
```

Counts below are MEASURED over the 581 manifests in the tree (re-derived by counting the files, 2026-08-12 — the previous figures were stale by up to 20 manifests and one of them, `targets[]`, was never right).

**Top level**

- `language` — language name passed to `Program::compileFiles` (must match a `.lang.json` in `src/dss-config/sources/`). In PROJECT MODE the `.dss-project.json` is the authority and this is a MIRROR — both runners fail loud if the two disagree. *581.*
- `source` — the single source file name, relative to the example dir. *565.*
- `sources` — a non-empty ARRAY of source file names, for a multi-CU example. An `expectDiagnostics` example must be single-source (one buffer, so a diagnostic's offset maps unambiguously). *15.*
- `project` — PROJECT MODE: the path (relative to the example dir) of a `.dss-project.json` that owns the build. The runners drive `Program::compileProject` / `dss-code-prime --project` instead of `compileFiles` / `--compile`, which is the ONLY entry point that expands the manifest's source globs and runs its `preBuildScripts` / `postBuildScripts` hooks. *1.* See **Project mode** below for the three things it changes.
- `exitCode` — exact OS exit code the spawned binary must produce. REQUIRED unless `expectDiagnostics` is present. *558.*
- `expectedStdout` — exact stdout the binary must print; declaring it is what routes the capture pipe. A per-target entry of the same name overrides it. *157 top-level + 71 per-target.*
- `expectDiagnostics` — a non-empty array of `{code, line, col}` (+ optional `positioned`) inverting the contract: the compile MUST fail, producing exactly this diagnostic set, and nothing is spawned. *23.*
- `optimizedPipelines` — extra ARMS of the same example, each re-compiled under a different optimizer pipeline and re-run against the same assertions. Each arm is `{"label": …}` plus EXACTLY ONE of `passes` (an inline array of `PassId` names) or `shippedPipeline` (a name under `src/dss-config/pipelines/` — `debug` or `release`). Both or neither fails loud. *460 manifests carry the key, declaring 652 arms: 374 name `shippedPipeline: "release"`, 278 use an inline `passes` list.* ⚠ **A manifest that ALSO declares `expectDiagnostics` gets its arms PARSED AND THEN SILENTLY DISCARDED** — the in-process runner reaches them only from `runOneTarget`, never from the `runErrorTarget` branch, and the arm ledger records the example as covered anyway. Declaring one there asserts nothing; no manifest in the corpus does, and none should until [[D-TEST-EXAMPLES-OPTIMIZED-ARM-DROPPED-ON-DIAGNOSTIC-MANIFEST]] closes.
- `targets[]` — see below. *581 — every manifest, which is the only value this row can legitimately have: both runners reject a manifest without one.*
- `$comment` — the repo-wide config-documentation convention: provenance, what the example witnesses, the red-on-disable argument. Ignored by the runners. *458.*

**EXACTLY ONE of `project` / `sources` / `source`.** All three answer the same question — *what does this example compile* — so declaring two is a hard parse error in BOTH runners, naming the pair. `sources` used to "take precedence over" `source`, which is the polite spelling of silently dropping one of them; zero of the 581 manifests declared both, so the rule protected nothing and only stood ready to swallow a rename typo. `project` + `expectDiagnostics` is likewise rejected: the expect-error path needs one named source buffer and a project build has none.

**`targets[]` entries** *(1,957 across the corpus)*

- `spec` — the combined `<arch>:<object-format>` target spec. *1,957.*
- `artifact` — the produced binary's FILENAME (never a path — see **Project mode** for where a project build actually puts it). *1,933.*
- `runOn` — host OS names (`windows` / `linux` / `darwin`) allowed to SPAWN this binary. *1,933.* The same 24 target entries omit BOTH `artifact` and `runOn`: they are the `expectDiagnostics` arms, which build nothing and spawn nothing.
- `emulator` — the launcher to prefix when the target arch differs from the host arch, e.g. `qemu-aarch64`. Required for a cross-ARCH arm — see the run gate above. *952.*
- `exitCode` / `expectedStdout` — per-target OVERRIDES of the manifest-level pin, for a source whose observable is legitimately platform-divergent (e.g. `sizeof(wchar_t)`). *30 / 71.*
- `dependsOn` — prerequisite artifacts built FIRST and threaded into this target's `--resolve-library` (a static library the example links against). Each entry is `{sources, spec, artifact}`, optionally `multiCu` (accepted, unused by the corpus today), and may carry its OWN nested `dependsOn`, so a fat archive can merge an input archive. *8 target entries across 2 manifests; 12 dependency entries in all, 4 of them nested.*
- `$comment` — same documentation convention as at the top level, scoped to one target. *6.*

## Project mode

`"project": "<file>.dss-project.json"` replaces `source`/`sources`. The example dir then ships the project manifest (and whatever its build scripts need) instead of a committed source file. `examples/c-subset/project_prebuild_script_codegen` is the worked example: its `preBuildScripts` hook GENERATES the C source, the manifest's `sources` is the pattern `generated_*.c` and matches nothing until the hook has run, so a green run witnesses the entire chain — script spawn → file written → glob expanded → compile → link → spawn → exit 42.

```json
{
  "language": "c-subset",
  "project": ".dss-project.json",
  "exitCode": 42,
  "targets": [
    { "spec": "x86_64:pe64-x86_64-windows-exec", "artifact": "prebuild_codegen.exe", "runOn": ["windows"] }
  ]
}
```

Three things differ from a `--compile` example, and both runners implement all three identically:

1. **The project manifest's `targets[]` is the build authority; the per-target `spec` is a MIRROR.** `Program::compileProject` takes no targets argument, so `spec` cannot drive a project build — it selects which of the project's own targets this arm runs, gates `runOn`/`emulator`, and names the artifact subdirectory. Each runner cross-checks it against the project manifest's `targets[]` and fails loud on a miss, so a stale mirror is caught at the mirror rather than surfacing later as a missing artifact. (One project manifest lists every target; the alternative — one manifest per corpus target — would make an example ship N near-identical copies of a user-facing file to work around a harness limitation.)
2. **The artifact is under a per-format subdirectory.** A project build forces `setPerFormatOutputSubdir(true)`, so it lands at `<outDir>/<formatName>/<artifactName-or-stem><ext>` even for a single target. `artifact` still names the FILE; the runners compose the subdirectory from `spec`. Do not spell the subdirectory into `artifact` — it would make one key mean two things.
3. **The COMPILE runs with the example's staged directory as the working directory.** A project manifest's relative `sources[]` globs expand against the process cwd, and a generator script is spawned there too. The in-process runner already had this (`ScratchDir::useAsCwd()`); the CLI runner's `CwdGuard` used to wrap only the RUN and now wraps the project-mode compile as well.

A declared build script whose interpreter is absent from the machine is ledgered `skipped-build-input-missing` — an ENVIRONMENTAL skip, warned by default and a hard failure under `DSS_STRICT_ARM_VERDICTS=1`. Everything else in project mode (a missing project manifest, a spec the project does not build, a language mirror that disagrees, a failed compile, a missing artifact) is a hard runner failure, never a skip.

⚠ **Where the two runners currently diverge:** `project` is read by BOTH, with the same validation, the same skip vocabulary and the same artifact-path rule — that is the rule working. `optimizedPipelines` and `expectedStdout` are read by the in-process runner ONLY — the CLI runner parses neither, so an optimizer arm and a stdout pin get no CLI-surface verdict. That is the capability-pairing rule above being broken, not a design; fix it in both when you next touch either key. ★ And the in-process runner drops `optimizedPipelines` a SECOND way that is easy to miss because the manifest still passes: on the `expectDiagnostics` branch it never looks at the field at all, so a declared arm vanishes with no verdict and no ledger row — [[D-TEST-EXAMPLES-OPTIMIZED-ARM-DROPPED-ON-DIAGNOSTIC-MANIFEST]]. Unknown top-level keys are also silently ignored rather than rejected, which is why a `$`-prefixed documentation key (`$comment`, `$targetGating`) is safe but a typo in a real key is not caught.

## Adding a new example

1. Create `examples/<lang>/<name>/`.
2. Drop your source + `expected.json`.
3. **If the feature is runtime-observable, give the manifest a `release` arm** — *except in project mode, where you must not.* `optimizedPipelines` is read by the in-process runner ONLY (the CLI runner passes no `--config` and parses neither key), so a release arm on a project-mode example would be exercised in one of the two runners, breaking the very capability-pairing rule at the top of this file. Record the omission as a `$comment` so it reads as a decision; `project_prebuild_script_codegen` does. Everywhere else, the arm is `"optimizedPipelines": [{"label": "release", "shippedPipeline": "release"}]`. Not a hand-listed `passes` subset, and never baseline-only. **Why:** the baseline arm is `Identity`-only (so is the shipped `debug` pipeline — literally `["Identity"]`), so a baseline-only example witnesses the front end and codegen and says **nothing** about the optimizer; a release-only miscompile passes it silently. Naming a `passes` subset is the same hole with extra steps: it exercises the passes you already thought about, not the shipped pipeline your users get. `shippedPipeline` loads `src/dss-config/pipelines/release.pipeline.json` itself, so the arm cannot drift from the configuration it claims to cover. This is not theoretical — `D-OPT-VARIADIC-RELEASE-MISCOMPILE` is a silent wrong-answer bug that shipped precisely because the variadic corpus was baseline-only, and closing it uncovered two more of the same class.
4. Re-run cmake (the harness globs at configure time).
5. `ctest -R examples/<lang>/<name>` to verify.

The new example surfaces in CI immediately.
