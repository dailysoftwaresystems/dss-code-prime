# The completeness guarantee — "leave no staleness behind"

This is the property that distinguishes a *sweep* from a spot-fix. It is proven, not asserted.

1. **Coverage:** every file in the inventory was scanned for every taxonomy class. The report names
   them — an unswept plan is a hole in the guarantee.
2. **Resolution:** every divergence found is in exactly one of {fixed, flagged}. The report's fix-list
   and flag-list together account for *all* of them. A divergence in neither list is a defect of the
   sweep, not acceptable output.
3. **Verification:** the mechanical classes re-scan clean after the fixes — the headline count equals
   `ctest`, no live "pending push", anchor guard OK, no closed-anchor/open-mention collision, links
   resolve. If a re-scan still shows a mechanical divergence, the sweep is not finished.
4. **The flags are the honest remainder:** "leave nothing behind" does **not** mean "fix everything" —
   it means *nothing is silently ignored*. A correctly-flagged doneness judgment is a *resolved* item,
   handed to the right decision-maker, not leftover staleness.

## Where this skill sits among its siblings

Three habits address plan drift at three different scopes:

- **`dss-cycle` step 8** updates the plans *in the same commit as the code*, per cycle — tactical,
  keeps things tidy as work lands.
- **`dss-audit` §I** fixes trivial staleness it *happens* to trip over — incidental, a side effect of
  auditing something else.
- **`dss-plan-sweep`** (this skill) sweeps *all* plans *systematically* and reconciles *everything* —
  the formalization of the repo's existing "Plans staleness sweep" commits.

The plans are a living contract: `.plans/NN-name - {ok,tbd}.md`, `_deferred-anchor-registry.md`,
`README.md`, and the sibling skills. Over a burst of cycles they drift — a count moves, a "pending
push" gets pushed, an anchor is struck in one plan but left open in another, a `⏳ planned` lands, a
description outlives the code it described.

## The sweep's creed

> Reconcile to truth, never to opinion; fix what the evidence proves, flag what needs a judgment,
> rewrite no history — and leave nothing silently behind. A plan that disagrees with the code is a bug
> in the plan; a sweep that ends with an unaccounted-for divergence is a bug in the sweep.
