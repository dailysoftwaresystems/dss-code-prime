# DssHarness — the harness this repository runs on

`DssHarness` is a cross-platform .NET tool, built in `dailysoftwaresystems/repo-harness` and published
on nuget.org. It is replacing `scripts/`: the anchor registries, the worktrees, the legs, the builds,
the test runs and the corpus runners all move to it, so that one implementation serves every host and
no capability depends on whether a `.sh` and a `.ps1` agree.

    dotnet tool install --global DssHarness      # or `dotnet tool update --global DssHarness`
    DssHarness --version                         # every leg must match the root host

⚠ **A leg installs only a PUBLISHED version.** A locally built tool cannot reach a remote leg, every
leg must run the same version as the machine driving it, a host ahead of the root refuses the run, and
there is no downgrade path. So a fix we need in the tool costs a release before any leg can use it.

## What it does today, and what is still a script

| Verb | State | What it replaces here |
|---|---|---|
| `init`, `legs`, `host-exec` | ships | host discovery, the leg catalogue, remote invocation |
| `create-worktree`, `delete-worktree`, `list-worktree` | ships | `scripts/lane-worktree` — once the evidence-preserving delete lands |
| `write-anchor`, `set-anchor`, `read-anchor`, `read-anchors`, `check-anchor-balance` | ships | `scripts/anchors`, `scripts/apply-registry-row`, `scripts/burndown-queue` |
| `build` | 0.6.0 | `scripts/local-build` |
| `test` | 0.7.0 | `scripts/run-gate` |
| `sync` | 0.8.0 | the leg drivers, `leg-tree`, the carriages |
| `run` | 0.9.0 | the sqlite corpus driver and the benchmark, as predefined runners |

⚠ **Until a verb ships, its script stays and is the only way to do that job.** Nothing is deleted
before the verb that replaces it has been proven on all four legs, and each release's deletions are
one commit — so a stall at one release leaves everything after it untouched.

## This repository's configuration

`.harness-config/config.json` is the whole contract and it is tracked. It declares:

- **eight legs** — `{Debug, Release}` across Windows, WSL x86_64, macOS arm64 and the arm64 VPS — and
  a `gate` leg set holding all eight, which is the gate a round owes;
- **four toolchains** (MSVC and MinGW GCC on Windows, GCC on Linux, Clang on macOS) and two build
  configs;
- **three emulators** — qemu for arm64 inside WSL, qemu for x86_64 on the VPS, Rosetta on the Mac.
  ⚠ **Not wine**: a DssHarness emulator changes the processor and never the operating system, and the
  pe64-under-wine arm is cross-OS, so that launcher stays in `real-examples/c/sqlite/legs.json` with
  the rest of the corpus's own leg catalogue;
- **every remote leg's repository directory**, because `sync` creates it when it is not there;
- the never-transfer floor, the contention tools, the worktree budget, and both registry paths.

Everything under `.harness-config/` that is NOT the contract or the runner actions is ignored by git:
the per-host connection data, the tool's lock, its worktrees. No secret is ever tracked, and no host
address, user name or key path appears in any tracked file.

## The anchor registries, after the migration

**Two documents, and every anchor is a production anchor.**

| file | holds |
|---|---|
| `.plans/_deferred-anchor-registry-production.md` | every still-open row |
| `.plans/_deferred-anchor-registry-done.md` | every closed row, in one table |

The harness registry is gone: a defect in the harness is repo-harness's to fix, and a defect in this
repository's own build wiring or tests is a production row like any other. Its 187 open rows and the
archive's 544 closed ones are readable in git at the parent of the commit that deleted them.

⛔ **The door is still `scripts/anchors/{write,set,read}-anchor` while those scripts exist**, and
`read-anchor <ID> --json` is still the only sanctioned way to read a cell. `write-anchor` gained
`--relocating` for a row that exists elsewhere in the repository and is being moved rather than named.

## Exit codes

`0` success · `10` usage · `11` not initialised · `12` invalid configuration · `13` refused ·
`14` a tool is missing · `15` a host is unavailable · `20` the command failed · `70` internal ·
`130` cancelled. Codes 1–9 are reserved per command.

⚠ **`host-exec` returns the wrapped command's exit code unchanged**, so a remote 20 and a local 20 are
the same number for two different reasons unless you know which leg was asked.

## The rule that matters most

**A defect in DssHarness is a repo-harness issue, never a local workaround.** The whole point of the
move is that one implementation serves every host; a patch here that routes around the tool re-creates
the drift the migration exists to end. Reproduce it, state the measurement, and file it there.
