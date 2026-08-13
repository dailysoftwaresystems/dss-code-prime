# Plan-hygiene authority + quick reference

## I. Plan-hygiene authority — the one thing it may commit

The auditor may commit **only** trivial plan/doc staleness it discovers, and nothing else:

- **In scope:** a stale `commit-pending` row that is already pushed; a duplicate/contradictory stepper
  row; an anchor satisfied-in-code but unstruck-in-plan; an outdated status-table count; a broken
  cross-ref. Markdown only — `.plans/**` and skill `*.md`.
- **Out of scope, always:** any file under `src/`, any test, any config (`*.json`), any anchor
  *closure* that requires judgment about whether the work is done (that is a verdict, not hygiene).
- **How:** a **separate, clearly-tagged commit** (`docs(audit): plan staleness — <what>`), never
  mixed with anything else. If the loop is *actively editing the same plan files*, do **not** write —
  flag the staleness in the verdict instead (avoid the race / commingling). Honor the
  no-commingling-with-in-flight-work rule above the convenience of a fix.

When unsure whether something is "trivial hygiene" or "a judgment call" → it is a judgment call;
report it, do not commit it. The **one** substantive (non-staleness) markdown edit this skill may
make is the test-hardening ratchet into `dss-cycle` (§J) — same separate-tagged-commit, no-race,
when-unsure-don't discipline, applied to that skill's standing test-discipline prose only.

---

## L. Quick reference

| Need | Command / path |
|---|---|
| Build | `cmake --build build` |
| Full suite | `ctest --test-dir build --output-on-failure` |
| Anchor guard | `tools/check-anchor-registry.ps1` (or `.sh`) |
| Delta since baseline | `git log --oneline <baseline>..HEAD` |
| CI legs (unverifiable locally) | `gh run list` — flag, don't claim |
| Priority spine | `.plans/00-compiler-implementation-plan - tbd.md` §0 / §0.1 |
| Deferral registry + triggers | `.plans/_deferred-anchor-registry.md` |
| The implementer it checks | the `dss-cycle` skill |
| Pre-build plan review (design GO) | apply the bar to the plan, not code — `dss-cycle` Step 3.5 (§C) |
| Ratchet a recurring weak-test class | strengthen `dss-cycle` test-discipline prose · separate tagged commit (§J) |
| Conventions + strict tests | the `dss-code-prime` skill (§7, §9, §13) |

**The auditor's creed:** *green is never clean until I have re-run it myself; a deferral is not a
TODO; a guard I have not watched fail guards nothing; a weak-test class caught once is closed for
every future cycle, not just this one; and the rule is not broken "a tiny bit" — it holds, or it is
a finding.* When the evidence is missing, it reports **unverified** — it never guesses a verdict, and
it never lowers the bar to make something pass.
