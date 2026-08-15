# Examples

Curated source programs the DSS compiler must compile + run end-to-end. Each example sits under `<language>/<name>/` and ships:

- a source file (`main.<ext>`) — or several, via a `sources` array for the multi-CU cases (15 examples today) — or, in PROJECT MODE, a `.dss-project.json` named by the manifest's `project` key, which owns the input list instead (3 examples today): its own `sources` may name committed files, or name only a pattern nothing matches until a `preBuildScripts` hook has written it
- a manifest (`expected.json`) declaring the target spec + expected exit code
- optionally, whole SUBDIRECTORIES — an example is a directory TREE, not a flat list of files. Both runners stage the example's neighbourhood into their scratch dir RECURSIVELY, relative subpaths intact, so a dependency that is itself a project (`<example>/dep_module/.dss-project.json` plus its sources, two or more levels deep) arrives whole and an intentionally EMPTY directory arrives too. Only the TOP-LEVEL `expected.json` is withheld — it is the harness's own input, and a scratch copy of it would shadow the file the runner actually parsed — so a file of that name *inside* a subdirectory is the example's own data and IS staged. *2 examples ship a subdirectory today across the 595 manifests — `project_dependson_module_source_merge` (`scalemod/`) and `project_dependson_staticlib_artifact_link` (`foldlib/`), the two corpus witnesses for the compiler's `dependsOn` resolution, each of which nests a whole second project one level down. The capability is ALSO pinned by fixture (`StagesNestedSubdirectoriesWithContentIntact`, and the same self-test inside `integrated_tests`) and both pins are kept: the fixture was added the cycle a nested dependency was silently dropped whole and reported as a missing MANIFEST, and it reds host-independently even when no corpus entry happens to exercise the shape.*

**Two runners drive the same corpus, and a capability must land in BOTH:**

- `tests/examples/examples_runner.cpp` — **in-process**, via `Program::compileFiles` (the API + library link path).
- `integrated_tests/runner.cpp` — **CLI subprocess**, driving the built `dss-code-prime` binary (argv parsing, exit codes, filesystem layout, output routing).

One runner enforcing something its sibling shrugs at is a silent harness bug, not a shortcut — that is how `D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT` survived in both for as long as it did. They already share their skip vocabulary, strict parse and emulator lint through `tests/test_support/arm_verdict_ledger.hpp`, and their recursive neighbour staging — plus its self-test — through `tests/test_support/stage_tree.hpp`.

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

Counts below are MEASURED over the **595** manifests in the tree, ✔re-derived by parsing every `expected.json` on **2026-08-15**. ⚠ **They are a DATED INVENTORY, not an invariant — re-derive rather than trust.** The 2026-08-14 set was stale within a day: the two `dependsOn` corpus examples landed hours later and moved **eleven distinct figures across eighteen slots** in this file at once, which is exactly what a hand-maintained count does. The block is kept only because several figures (three project-mode examples, 23 `expectDiagnostics` manifests, the two harness-`dependsOn` manifests, the two subdirectory-shipping examples) are the *point* of a sentence rather than decoration. **Re-derive by PARSING every `examples/**/expected.json` and counting the key in question — never by grepping the text**, because `$comment` prose in this corpus mentions key names (`dependsOn` appears in prose in manifests that do not declare it), so a text count over-reports. The corpus directory is the source of truth, and any figure here that disagrees with it is this file's bug.

**Top level**

- `language` — language name passed to `Program::compileFiles` (must match a `.lang.json` in `src/dss-config/sources/`). In PROJECT MODE the `.dss-project.json` is the authority and this is a MIRROR — both runners fail loud if the two disagree. *595.*
- `source` — the single source file name, relative to the example dir. *577.*
- `sources` — a non-empty ARRAY of source file names, for a multi-CU example. An `expectDiagnostics` example must be single-source (one buffer, so a diagnostic's offset maps unambiguously). *15.*
- `project` — PROJECT MODE: the path (relative to the example dir) of a `.dss-project.json` that owns the build. The runners drive `Program::compileProject` / `dss-code-prime --project` instead of `compileFiles` / `--compile`, which is the ONLY entry point that expands the manifest's source globs, runs its `preBuildScripts` / `postBuildScripts` hooks, and RESOLVES its `dependsOn` graph. *3.* See **Project mode** below for the three things it changes.
- `exitCode` — exact OS exit code the spawned binary must produce. REQUIRED unless `expectDiagnostics` is present. *572.*
- `expectedStdout` — exact stdout the binary must print; declaring it is what routes the capture pipe. A per-target entry of the same name overrides it. *158 top-level + 71 per-target.*
- `expectDiagnostics` — a non-empty array of `{code, line, col}` (+ optional `positioned`) inverting the contract: the compile MUST fail, producing exactly this diagnostic set, and nothing is spawned. *23.*
- `optimizedPipelines` — extra ARMS of the same example, each re-compiled under a different optimizer pipeline and re-run against the same assertions. Each arm is `{"label": …}` plus EXACTLY ONE of `passes` (an inline array of `PassId` names) or `shippedPipeline` (a name under `src/dss-config/pipelines/` — `debug` or `release`). Both or neither fails loud. *473 manifests carry the key, declaring 665 arms: 387 name `shippedPipeline: "release"`, 278 use an inline `passes` list.* ⚠ **A manifest that ALSO declares `expectDiagnostics` gets its arms PARSED AND THEN SILENTLY DISCARDED** — the in-process runner reaches them only from `runOneTarget`, never from the `runErrorTarget` branch, and the arm ledger records the example as covered anyway. Declaring one there asserts nothing; no manifest in the corpus does, and none should until [[D-TEST-EXAMPLES-OPTIMIZED-ARM-DROPPED-ON-DIAGNOSTIC-MANIFEST]] closes.
- `targets[]` — see below. *595 — every manifest, which is the only value this row can legitimately have: both runners reject a manifest without one.*
- `$comment` — the repo-wide config-documentation convention: provenance, what the example witnesses, the red-on-disable argument. Ignored by the runners. *472.*

**EXACTLY ONE of `project` / `sources` / `source`.** All three answer the same question — *what does this example compile* — so declaring two is a hard parse error in BOTH runners, naming the pair. `sources` used to "take precedence over" `source`, which is the polite spelling of silently dropping one of them; zero of the 595 manifests declare both, so the rule protected nothing and only stood ready to swallow a rename typo. `project` + `expectDiagnostics` is likewise rejected: the expect-error path needs one named source buffer and a project build has none.

**`targets[]` entries** *(1,986 across the corpus)*

- `spec` — the combined `<arch>:<object-format>` target spec. *1,986.*
- `artifact` — the produced binary's FILENAME (never a path — see **Project mode** for where a project build actually puts it). *1,962.*
- `runOn` — host OS names (`windows` / `linux` / `darwin`) allowed to SPAWN this binary. *1,962.* The same 24 target entries omit BOTH `artifact` and `runOn`: they are the `expectDiagnostics` arms, which build nothing and spawn nothing.
- `emulator` — the launcher to prefix when the target arch differs from the host arch, e.g. `qemu-aarch64`. Required for a cross-ARCH arm — see the run gate above. *958.*
- `exitCode` / `expectedStdout` — per-target OVERRIDES of the manifest-level pin, for a source whose observable is legitimately platform-divergent (e.g. `sizeof(wchar_t)`). *30 / 71.*
- `dependsOn` — prerequisite artifacts built FIRST and threaded into this target's `--resolve-library` (a static library the example links against). Each entry is `{sources, spec, artifact}`, optionally `multiCu` (accepted, unused by the corpus today), and may carry its OWN nested `dependsOn`, so a fat archive can merge an input archive. *8 target entries across 2 manifests; 12 dependency entries in all, 4 of them nested.* ★ **This is NOT the compiler's `dependsOn` — see below.**
- `$comment` — same documentation convention as at the top level, scoped to one target. *6.*

★ **THE `dependsOn` ABOVE IS THE HARNESS'S, AND THERE IS A SECOND KEY OF THE SAME NAME THAT IS A DIFFERENT FEATURE.** The one documented here lives in `expected.json`, is read by the two corpus runners, and builds a **test FIXTURE**: a prerequisite artifact the runner stages so an arm has something to compile against. The compiler's `dependsOn` lives in a user's `.dss-project.json`, is read by the project-config loader, and is a **PRODUCT feature** — a real dependency-resolution surface with a `.dss-deps` cache, a composition-verb dispatch and its own diagnostic band ([`docs/project-config-spec.md` §2.6](../docs/project-config-spec.md)). Two files, two loaders, two audiences.

**They COEXIST deliberately, and the reason is INDEPENDENCE.** If the corpus's fixture staging went through the compiler's resolver, a resolver bug would turn examples red that have nothing to do with dependencies — and the corpus would lose the one staging path that does **not** depend on the feature under test. A harness that cannot build its own fixtures without the feature it is testing has no independent verdict left to give. A rename was considered and **REJECTED** (operator, 2026-08-12): the harness key was measured first — it appears in exactly the two manifests counted above, so renaming it would have been cheap, and renaming the *product* key instead was priced across loader, tests, spec, plan and the `D_Dependency*` code names. The call was **neither**; both keep the name, and the distinction is carried by **documentation alone** — this paragraph and its twin in the project-config spec. That is why it is written down rather than left to be re-derived: **an undocumented duplicate name is how the wrong one gets deleted.** Do not re-propose the rename, and do not "unify" the two.

## Project mode

`"project": "<file>.dss-project.json"` replaces `source`/`sources`. The example dir then ships the project manifest, plus whatever that manifest's own `sources`, hooks and dependencies need. Three examples use it today, one per capability the mode unlocks:

- **`project_prebuild_script_codegen`** — build-lifecycle hooks. Its `preBuildScripts` hook GENERATES the C source; the manifest's `sources` is the pattern `generated_*.c` and matches nothing until the hook has run, so a green run witnesses the entire chain — script spawn → file written → glob expanded → compile → link → spawn → exit 42.
- **`project_dependson_module_source_merge`** — the compiler's `dependsOn`, `SourceMerge` arm. `scalemod/` is a nested project declaring `"artifactProfile": "module"`; its sources are MERGED into this compilation, and `main.c` calls a function only that module defines.
- **`project_dependson_staticlib_artifact_link`** — the compiler's `dependsOn`, `ArtifactLink` arm. `foldlib/` declares `"artifactProfile": "staticlib"`, so it is BUILT to its own archive under `<output>/deps/foldlib/<formatName>/` and threaded into the consumer's `resolveLibraries`. Its own `targets[]` lists one ELF spec while the consumer builds four targets — deliberately, because a dependency's `targets[]` is not consulted when it is built as a dependency.

Both `dependson` examples take a **`path`** dependency only. No corpus example may reach the network.

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

⚠ **Where the two runners currently diverge:** `project` is read by BOTH, with the same validation, the same skip vocabulary and the same artifact-path rule — that is the rule working, and it is what lets the three project-mode examples above (including their nested `dependsOn` trees) assert on both surfaces. `optimizedPipelines` and `expectedStdout` are read by the in-process runner ONLY — the CLI runner parses neither (✔**0** occurrences of `optimizedPipelines` / `shippedPipeline` / `--config` in `integrated_tests/runner.cpp`, re-measured 2026-08-15), so an optimizer arm and a stdout pin get no CLI-surface verdict. That is the capability-pairing rule above being broken, not a design; fix it in both when you next touch either key. ★ It is broken for **473 manifests**, not for a corner — which is why the fix is to teach the CLI runner `--config` (a flag the driver already has: `--config=release|debug`, `src/program/cli_args.hpp:105`), never to stop declaring arms. ★ And the in-process runner drops `optimizedPipelines` a SECOND way that is easy to miss because the manifest still passes: on the `expectDiagnostics` branch it never looks at the field at all, so a declared arm vanishes with no verdict and no ledger row — [[D-TEST-EXAMPLES-OPTIMIZED-ARM-DROPPED-ON-DIAGNOSTIC-MANIFEST]]. Unknown top-level keys are also silently ignored rather than rejected, which is why a `$`-prefixed documentation key (`$comment`, `$targetGating`) is safe but a typo in a real key is not caught.

## Adding a new example

1. Create `examples/<lang>/<name>/`.
2. Drop your source + `expected.json`.
3. **If the feature is runtime-observable, give the manifest a `release` arm — in project mode too.** The arm is `"optimizedPipelines": [{"label": "release", "shippedPipeline": "release"}]`. Not a hand-listed `passes` subset, and never baseline-only. **Why:** the baseline arm is `Identity`-only (so is the shipped `debug` pipeline — literally `["Identity"]`), so a baseline-only example witnesses the front end and codegen and says **nothing** about the optimizer; a release-only miscompile passes it silently. Naming a `passes` subset is the same hole with extra steps: it exercises the passes you already thought about, not the shipped pipeline your users get. `shippedPipeline` loads `src/dss-config/pipelines/release.pipeline.json` itself, so the arm cannot drift from the configuration it claims to cover. This is not theoretical — `D-OPT-VARIADIC-RELEASE-MISCOMPILE` is a silent wrong-answer bug that shipped precisely because the variadic corpus was baseline-only, and closing it uncovered two more of the same class.
   - ★ **THIS RULE PREVIOUSLY SAID *"except in project mode, where you must not"*, AND THAT CARVE-OUT WAS WITHDRAWN ON 2026-08-15 BY MEASUREMENT.** Its ground — `optimizedPipelines` is read by the in-process runner only, so a project-mode arm lands in one of the two runners — is TRUE (✔`optimizedPipelines` / `shippedPipeline` / `--config` each occur **0** times in `integrated_tests/runner.cpp`) and **proves too much**: 473 manifests carry the key and the CLI runner sees **none** of them, so abstaining in project mode does not restore the pairing rule, it only discards the one runner that *can* check. The carve-out also read as "the arm would be inert there", which is false: ✔`setOptimizerPipelineOverride` is `Program` **state** set before the `compileProject` dispatch (`tests/examples/examples_runner.cpp`), `compileProject` delegates to `compileUnits`/`compileFiles` (`src/program/program.cpp:2416-2417`), both thread `optimizerPipelineOverride_` into `runCusToTargets` (`:2688` / `:2798` → `:1894`), and `compile_pipeline.cpp:148` prefers it over the config-derived pipeline. ✔Measured end-to-end: both `dependson` examples ledger `2 verified (2 ran)` on windows — baseline **and** release — and `--config=release` vs `--config=debug` emit **different** pe64 bytes for each. `project_prebuild_script_codegen` still carries no arm; that is now a per-example judgement (a build-lifecycle observable the optimizer cannot move), not a mode-wide prohibition.
   - ⚠ **CHECK THE ARM IS NOT A NO-OP BEFORE YOU SHIP IT.** ✔MEASURED 2026-08-15: `project_dependson_staticlib_artifact_link` first shipped a consumer that was a single call into the dependency's archive, and `release` vs `debug` produced a **byte-identical** image — the only call was external, so the optimizer had nothing to transform and the arm asserted that a no-op stayed a no-op. Give the consumer code the shipped passes actually move (an inlinable helper, a value Mem2Reg promotes, a loop SimplifyCfg folds) and confirm the two configs differ. An arm that cannot change anything is the masked-coverage trap wearing the shape of compliance.
4. Re-run cmake (the harness globs at configure time). ⚠ The in-process entries are added at **configure** time (`file(GLOB_RECURSE … CONFIGURE_DEPENDS)`), while `integrated_tests` walks `examples/` at **runtime** — so a new example reaches the CLI runner immediately and the in-process runner only after a reconfigure. Do not read one green as both.
5. `ctest -R examples/<lang>/<name>` to verify.

The new example surfaces in CI immediately.
