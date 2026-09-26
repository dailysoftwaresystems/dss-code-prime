# Program conventions — every program is a DssHarness action

Where a program this repository ships lives, how it is laid out and indexed, and the ruling that
retired `.sh`/`.ps1` programs. The generated index itself is `dss-cycle/references/actions.md`.

## Program conventions (`.harness-config/runner/actions/`)

**Layout.** Every program this repository ships is a DssHarness ACTION: one directory per program,
named for it, holding its `<name>.yml` and every file it runs —
`.harness-config/runner/actions/[<group>/...]<name>/<name>.{yml,py}`, assets alongside, started by
`dssharness run <name>` through a `predefinedRunners` entry in `.harness-config/config.json`. There is no
`scripts/` and no `real-examples/`. Nothing loose in a group directory, no program buried a level
deeper, no action inside another. A program that another LOADS is loaded as a sibling of the loader's
own directory, and a program finds the tree it lives in through the one owner, `owning-tree`, never
by counting `..`. Each action declares its purpose once, in a
`PURPOSE:` comment line in its `<name>.yml`; both indexes
(`.harness-config/runner/actions/README.md`, `dss-cycle/references/actions.md`) are generated from it and held to the
tree — and to the runners — by `scripts_index_guard`.

**No `.sh` and no `.ps1` under `.harness-config/runner/actions` — one Python program per action.**
Operator ruling 2026-09-21: *"I don't want .sh/.ps1 files inside .harness-config\runner\actions.
entrypoint is .yml, you can call .py files, BUT NOT .sh/.ps1 please. They are specific per OS. I
don't want this anymore"*. Every step of every action starts `python3 <file>.py`, and one program
runs on every host, so there is no twin to keep in step. `scripts_index_guard` refuses a `.sh` or a
`.ps1` anywhere under the actions root, by name. The 2026-08-19 convention it replaced — a `.ps1`
twin for every `.sh` that had to reach the Windows leg, their parity checked in review — retired with
the last pair on 2026-09-21.
