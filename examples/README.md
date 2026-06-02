# Examples

Curated source programs the DSS compiler must compile + run end-to-end. Each example sits under `<language>/<name>/` and ships:

- one source file (`main.<ext>`)
- a manifest (`expected.json`) declaring the target spec + expected exit code

A ctest entry per example compiles via `Program::compileFiles` (in-process, same path the CLI uses) and — when the target's exec format matches the host OS — spawns the produced binary via `tests/test_support/run_binary.hpp` and asserts the exact exit code.

The harness ASSERTS strictly: zero compile-time diagnostics, binary on disk, spawn success, no timeout, exit code `==` manifest value. Any drift breaks ctest.

## Manifest schema

```json
{
  "language": "c-subset",
  "source": "main.c",
  "exitCode": 42,
  "targets": [
    {
      "spec": "x86_64:pe64-x86_64-windows-exec",
      "artifact": "main.exe",
      "runOn": ["windows"]
    }
  ]
}
```

- `language` — language name passed to `Program::compileFiles` (must match a `.lang.json` in `src/dss-config/sources/`).
- `source` — source file name relative to the example dir.
- `exitCode` — exact OS exit code the spawned binary must produce.
- `targets[]` — list of (target spec, artifact filename, host platforms allowed to RUN it). Cross-compilation builds compile but skip the run step on mismatched hosts.

## Adding a new example

1. Create `examples/<lang>/<name>/`.
2. Drop your source + `expected.json`.
3. Re-run cmake (the harness globs at configure time).
4. `ctest -R examples/<lang>/<name>` to verify.

The new example surfaces in CI immediately.
