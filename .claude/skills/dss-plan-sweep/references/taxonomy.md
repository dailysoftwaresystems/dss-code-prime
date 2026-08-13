# The staleness taxonomy — the sweep checklist

Ten classes. For each: the **tell**, the **source of truth** to reconcile against, and **fix|flag**.

| # | Class | Tell | Source of truth | Action |
|---|---|---|---|---|
| 1 | **Headline status marker** | `✅`/`⏳`/`🟦`/`WIP`/`DONE` on a current item | git + src + ctest | **fix** if it contradicts observable state *and* needs no scope-judgment; else **flag** |
| 2 | **Counts** (ctest / anchors / examples) | "N/N ctest", "M anchors", "K examples" in a *current/headline* line | actual `ctest` run; anchor guard count; `examples/` listing | **fix** the headline; **never** touch historical progression rows |
| 3 | **Commit / push state** | `pending push`, `commit-pending`, `pending-push`, `(commit-pending)` | `git log` / `git rev-list … origin/<b>` | **fix** to the real hash / strike "pending" once pushed |
| 4 | **Anchor closure consistency** | an anchor `✅ CLOSED` in one plan but listed open / as a live blocker elsewhere; or cited in `src/` but unstruck | the anchor guard + grep across all plans | **fix** the inconsistency *toward the evidenced state*; if the evidenced state itself is a doneness judgment → **flag** |
| 5 | **Cross-references** | dead relative link, `§N` ref to a moved section, ref to a renamed/closed plan file | the target file/section exists? | **fix** |
| 6 | **Numbering drift** | a sub-plan label (`§3 OPT-N`) diverging from the as-built `§0`/`§0.1` numbering | the plan's own stated "live numbering surface" note | **fix** per the plan's convention; if no convention is stated → **flag** (don't invent one) |
| 7 | **Duplicate / contradictory rows** | same item twice with different statuses (e.g. the OPT6 "DONE" + "planned" pair) | the evidenced state | **fix** (collapse to the true row); preserve any legitimately-distinct cycle rows |
| 8 | **Temporal-provenance prose** | `Next = X` / `⏳ planned` / "future" / "as of <date>" that is now past or done | git history (did X land?) | **fix** the forward-pointer if X is unambiguously done; else **flag** |
| 9 | **Description drift** | prose describing an implementation that has since changed (e.g. "dominators land in OPT1" when they landed OPT4) | `src/` + git history | **flag** unless the correction is unambiguous and scope-complete (then **fix**) |
| 10 | **Filename status** (`- ok.md` / `- tbd.md`) | a `- tbd.md` whose work is fully done, or a `- ok.md` reopened | git + src + the plan's own §0 | **flag** (rename touches every cross-ref + is a doneness judgment) |

## Useful starting greps

`scripts/scan_staleness.py` automates classes 2, 3, 5 and the class-4 co-occurrence check. The rest
need reading. Manual starting points:

- `pending push|commit-pending`
- the headline `[0-9]+/[0-9]+ ctest` count
- `✅|⏳|🟦|WIP|TBD|Next =|⏳ planned`
- each closed anchor's id, to check for open/blocker co-occurrence elsewhere
- relative-link targets

## Filename renames are always a flag

Changing `- tbd.md → - ok.md` is both a doneness judgment *and* a multi-file operation (every
cross-reference plus a `git mv`). Surface it as a recommendation with the evidence; let the
implementer or user execute it through a cycle.
