# Examples

Curated source programs the DSS compiler must compile + run end-to-end. Each example sits under `<language>/<name>/` and ships:

- a source file (`main.<ext>`) — or several, via a `sources` array for the multi-CU cases (15 examples today)
- a manifest (`expected.json`) declaring the target spec + expected exit code

**Two runners drive the same corpus, and a capability must land in BOTH:**

- `tests/examples/examples_runner.cpp` — **in-process**, via `Program::compileFiles` (the API + library link path).
- `integrated_tests/runner.cpp` — **CLI subprocess**, driving the built `dss-code-prime` binary (argv parsing, exit codes, filesystem layout, output routing).

One runner enforcing something its sibling shrugs at is a silent harness bug, not a shortcut — that is how `D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT` survived in both for as long as it did. They already share their skip vocabulary, strict parse and emulator lint through `tests/test_support/arm_verdict_ledger.hpp`.

**The run gate is `runOn`, then `emulator` — not "the exec format matches the host OS".** The in-process runner COMPILES every declared target on every host (a cross-format emission regression must surface even on the wrong host, so the zero-diagnostic assert still applies there). It then SPAWNS only if (a) `runOn` names the current host OS, and (b) where the target's arch differs from the host's, the target declares an `emulator` that is on `PATH`. Missing either is a *named skip* in the arm ledger, never a pass: no `emulator` key is a manifest defect the corpus lint reds host-independently, and an absent emulator binary is a machine property that becomes a hard failure under `DSS_STRICT_ARM_VERDICTS=1`. The CLI runner instead binds the FIRST target whose `runOn` matches the host, and ledgers the rest.

The harness ASSERTS strictly: binary on disk, spawn success, no timeout, exit code `==` manifest value, and **zero compile-time diagnostics unless the manifest declares `expectDiagnostics`** — 19 examples do, and those invert the contract: the compile MUST fail with exactly the declared diagnostic set and nothing is spawned. Any drift breaks ctest.

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

Counts below are MEASURED over the 561 manifests in the tree.

**Top level**

- `language` — language name passed to `Program::compileFiles` (must match a `.lang.json` in `src/dss-config/sources/`). *561.*
- `source` — the single source file name, relative to the example dir. *546.*
- `sources` — a non-empty ARRAY of source file names, for a multi-CU example. Takes precedence over `source`; an `expectDiagnostics` example must be single-source (one buffer, so a diagnostic's offset maps unambiguously). *15.*
- `exitCode` — exact OS exit code the spawned binary must produce. REQUIRED unless `expectDiagnostics` is present. *542.*
- `expectedStdout` — exact stdout the binary must print; declaring it is what routes the capture pipe. A per-target entry of the same name overrides it. *150 top-level + 61 per-target.*
- `expectDiagnostics` — a non-empty array of `{code, line, col}` (+ optional `positioned`) inverting the contract: the compile MUST fail, producing exactly this diagnostic set, and nothing is spawned. *19.*
- `optimizedPipelines` — extra ARMS of the same example, each re-compiled under a different optimizer pipeline and re-run against the same assertions. Each arm is `{"label": …}` plus EXACTLY ONE of `passes` (an inline array of `PassId` names) or `shippedPipeline` (a name under `src/dss-config/pipelines/` — `debug` or `release`). Both or neither fails loud. *444 manifests carry the key, declaring 632 arms: 358 name `shippedPipeline: "release"`, 274 use an inline `passes` list.*
- `targets[]` — see below. *561.*
- `$comment` — the repo-wide config-documentation convention: provenance, what the example witnesses, the red-on-disable argument. Ignored by the runners. *438.*

**`targets[]` entries** *(1,901 across the corpus)*

- `spec` — the combined `<arch>:<object-format>` target spec. *1,901.*
- `artifact` — the produced binary's filename. *1,882.*
- `runOn` — host OS names (`windows` / `linux` / `darwin`) allowed to SPAWN this binary. *1,882.* The same 19 targets omit BOTH `artifact` and `runOn`: they are the `expectDiagnostics` arms, which build nothing and spawn nothing.
- `emulator` — the launcher to prefix when the target arch differs from the host arch, e.g. `qemu-aarch64`. Required for a cross-ARCH arm — see the run gate above. *928.*
- `exitCode` / `expectedStdout` — per-target OVERRIDES of the manifest-level pin, for a source whose observable is legitimately platform-divergent (e.g. `sizeof(wchar_t)`). *30 / 61.*
- `dependsOn` — prerequisite artifacts built FIRST and threaded into this target's `--resolve-library` (a static library the example links against). Each entry is `{sources, spec, artifact}`, optionally `multiCu` (accepted, unused by the corpus today), and may carry its OWN nested `dependsOn`, so a fat archive can merge an input archive. *8 target entries across 2 manifests, 4 of them nested.*
- `$comment` — same documentation convention as at the top level, scoped to one target. *6.*

⚠ **Where the two runners currently diverge:** `optimizedPipelines` and `expectedStdout` are read by the in-process runner ONLY — the CLI runner parses neither, so an optimizer arm and a stdout pin get no CLI-surface verdict. That is the capability-pairing rule above being broken, not a design; fix it in both when you next touch either key. Unknown top-level keys are also silently ignored rather than rejected, which is why a `$`-prefixed documentation key (`$comment`, `$targetGating`) is safe but a typo in a real key is not caught.

## Adding a new example

1. Create `examples/<lang>/<name>/`.
2. Drop your source + `expected.json`.
3. **If the feature is runtime-observable, give the manifest a `release` arm** — `"optimizedPipelines": [{"label": "release", "shippedPipeline": "release"}]`. Not a hand-listed `passes` subset, and never baseline-only. **Why:** the baseline arm is `Identity`-only (so is the shipped `debug` pipeline — literally `["Identity"]`), so a baseline-only example witnesses the front end and codegen and says **nothing** about the optimizer; a release-only miscompile passes it silently. Naming a `passes` subset is the same hole with extra steps: it exercises the passes you already thought about, not the shipped pipeline your users get. `shippedPipeline` loads `src/dss-config/pipelines/release.pipeline.json` itself, so the arm cannot drift from the configuration it claims to cover. This is not theoretical — `D-OPT-VARIADIC-RELEASE-MISCOMPILE` is a silent wrong-answer bug that shipped precisely because the variadic corpus was baseline-only, and closing it uncovered two more of the same class.
4. Re-run cmake (the harness globs at configure time).
5. `ctest -R examples/<lang>/<name>` to verify.

The new example surfaces in CI immediately.
