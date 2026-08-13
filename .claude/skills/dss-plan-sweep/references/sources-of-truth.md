# Sources of truth + the four-surface invariant

The sweep **never invents** a value; it reads it from the authority and reconciles the plan to it.

## What is authoritative for each fact

| Fact | Authority |
|---|---|
| Current ctest count | a fresh `ctest --test-dir build --output-on-failure` |
| Push / commit state | `git log`, `git rev-list … origin/<branch>` |
| Anchor exists / is cited in src | `tools/check-anchor-registry.{ps1,sh}` + grep `src/` |
| Anchor is *closed* | code present + test green + full anchor scope covered; else a flag |
| What an implementation actually does | `src/` + the cycle that landed it (git) |
| Plan-file lifecycle (`- ok`/`- tbd`) | the plan's own §0 status + git (flag the rename) |
| Cross-ref target exists | the referenced file/section |

If an authority is unavailable — CI legs the sweep cannot run locally, or a closure needing a scope
read it cannot fully make — the item is **flagged unverified**, never fixed on a guess.

## The four-surface consistency invariant

For any tier or anchor, these must all agree. They are one truth seen four ways:

1. **Plan-00 §0** status table (the project-wide headline).
2. **Plan-00 §0.1** stepper (next-up / done by block).
3. The **owning sub-plan** §0 / §3.1 row.
4. The **`_deferred-anchor-registry.md`** row (for anchors).

When the sweep fixes one, it reconciles all four in the same pass — a fix that leaves the other three
stale just relocates the drift. The plan *filename* (`- ok.md` / `- tbd.md`) is a fifth surface, and
it is always a flag rather than an auto-fix.

## Command quick reference

| Need | Command / path |
|---|---|
| Real suite count | `ctest --test-dir build --output-on-failure` |
| Anchor guard (src↔registry) | `tools/check-anchor-registry.ps1` (or `.sh`) |
| Push state | `git rev-list --left-right --count origin/<branch>...HEAD` |
| All plans | `.plans/` (numbered `00`–`22`, `08.x`, registry, `v2-gap-catalog`, `ZZ-final-goal`) |
| Anchor registry | `.plans/_deferred-anchor-registry.md` |
| Sibling: per-cycle plan update | the `dss-cycle` skill, step 8 |
| Sibling: incidental plan hygiene | the `dss-audit` skill §I |
| `.plans/` system + conventions | the `dss-code-prime` skill — **it wins on any conflict** |
