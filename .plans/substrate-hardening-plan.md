# Substrate Hardening — Sub-Plan

> Closes three concrete gaps surfaced during v2 close-out **before** parent plan #5 (tokenizer) opens. Small, mechanical, ~1–2 days of work. No new substrate features — only de-risking of what already exists.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **Not started.** Trigger: v2 sub-plan ✅ done (PR8 + `4547301` followup). Successor: parent plan #5 (`tokenizer`). |
| Predecessors  | ✅ v1 T0–T12 (`tree-node-model-plan.md`); ✅ v2 PR0–PR8 (`schema-expressiveness-v2-plan.md`). |
| Successors    | Parent plan phase #5 (tokenizer). Cleanly verifying the schema against real tokens is what makes phase #7 (parser) tractable and what surfaces v3 schema gaps in time to act on them. |
| Scope         | **Bounded.** SH1: landing-log generator. SH2: Linux CI matrix entry. SH3: cross-tree NodeId guard. No other items will be added to this plan — anything new becomes a separate sub-plan. |

### PR landing log

| PR | Status | Commit / notes |
|---|---|---|
| SH1 | ⏳ pending | Landing-log generator script. |
| SH2 | ⏳ pending | Linux CI matrix entry. |
| SH3 | ⏳ pending | Cross-tree NodeId guard. |

---

## 1. Motivation

The PR8 review surfaced three classes of substrate risk that no single PR caused but that compound over time. Listed in order of "how often have we already paid this tax":

1. **Plan-doc drift.** The PR8 review found 4 missing commit hashes in the landing log (PR2a, PR3, PR4, PR5) that prior cross-plan refreshes forgot to backfill, plus a self-referential `_initial commit pending review_` on the close-out PR itself. The pattern is recurring — every cross-plan refresh has caught at least one stale row. Hand-maintenance won't scale to the tokenizer/parser/semantic phases that are still ahead.

2. **Cross-platform claim is unverified.** `CLAUDE.md` lists cross-platform as a stated goal; only Windows + GCC 13 MinGW has ever run. The DLL-load `STATUS_ENTRYPOINT_NOT_FOUND` error has surfaced repeatedly in test runs. A green build matrix with one Linux job collapses this risk to "verified."

3. **Cross-tree `NodeId` confusion is documented but unactioned.** `NodeAttribute<T>` bounds-checks every access against its bound Tree's `nodeCount`, but a `NodeId` from Tree A whose `.v` happens to fit in Tree B's range slips through. The skill docs already call this a "documented caveat in §5.10" — but multi-source compilation (any non-trivial real project) will hit this within the first incremental build that holds two trees simultaneously.

None of these block forward progress today. All three become substantially more expensive to fix later. SH lands them now while the substrate is still small.

---

## 2. PRs

### SH1 — Landing-log generator script

**Goal.** Eliminate the entire class of "missing commit hash in landing log" bugs by generating the landing-log tables from `git log` instead of hand-editing them.

**Surface.**
- `tools/refresh_landing_log.ps1` (PowerShell, Windows-first per repo convention) — also a `tools/refresh_landing_log.py` cross-platform variant if SH2 lands Linux CI first.
- Scans `git log feature/v2..HEAD` (or whatever range arg is passed) for commit subjects matching `^(PR\w+)(?:\s+review)?(?:\s+round-\d+)?:` — pairs initial commits with their `review followup` / `round-N` siblings into landing-log row entries.
- Rewrites the `### PR landing log` table in `schema-expressiveness-v2-plan.md`, `substrate-hardening-plan.md`, and any future sub-plan with the same convention. The script reads a small `tools/landing-log-config.json` declaring which plan files have landing logs.
- Body-text of each row stays hand-written (the script never overwrites human prose) — only the **hash column** is regenerated. The script uses a single-line marker (`<!-- LANDING-LOG-HASHES: PR3 -->`) inside the row to find which line to rewrite; missing markers mean the row is hand-managed.
- Exits non-zero if any landed PR commit is missing from a landing log → CI can run this as a check.

**Tests.** Unit tests in PowerShell using Pester (or pytest if Python variant) covering:
- Single-commit PR row.
- Initial + one review followup.
- Initial + two review rounds (PR5 shape).
- Missing-from-log detection.
- Marker-absent row is left untouched.
- Idempotency: running twice produces identical output.

**Diagnostics.**
- Script prints a unified diff of what changed.
- `--check` flag exits non-zero on any drift (CI-friendly).
- `--write` flag actually applies the rewrite.

**Out of scope.** Auto-writing the body prose of new rows (humans still author that). Auto-detecting new PR landings without a marker (intentional — the marker is the "this is tracked" opt-in).

---

### SH2 — Linux CI matrix entry

**Goal.** Verify the cross-platform claim by running the full ctest suite on Ubuntu GCC in CI on every PR.

**Surface.**
- Extend `dss-code-prime/.github/workflows/cpp-app-pr.yml` (the consumer wiring) to add a `matrix.os: [windows-latest, ubuntu-latest]` axis.
- Reuse the existing `DSS.DevOps/.github/workflows/cpp-app-pr.yml` reusable workflow — confirm it already supports Linux GCC; if not, add the toolchain there as the smallest possible edit.
- Use Ubuntu 22.04 + system GCC 13 (or 13.2 to match Windows MinGW exactly — pick whichever is available without manual install).
- ctest runs the full 509-case suite. Expect 0 failures.
- Fix any Windows-isms surfaced — most likely: forward-slash path assumptions, line-ending sensitivity in test goldens, `.dll` vs `.so` lookup paths.

**Acceptance.**
- PR review CI shows two green checks (Windows + Linux) on PRs.
- README badge or status table in `compiler-implementation-plan.md` §0 reflects both.

**Out of scope.** macOS (deferred to a later SH-2.x or to the targets-expand phase). ASAN/UBSAN/TSAN jobs (deferred — though if a Linux GCC job is trivially extensible to ASAN, opportunistically add it). MSVC on Windows (deferred — MinGW is the verified Windows toolchain).

---

### SH3 — Cross-tree `NodeId` guard

**Goal.** Make cross-tree `NodeId` usage a loud failure instead of a silent bounds-check pass.

**Design call.** Two viable approaches, picked at implementation time based on which is smaller:

- **Option A — `Tree` stamps each Node with a generation counter, `NodeAttribute<T>` validates on access.** Adds 4 bytes per `Node` (currently ~40 bytes → ~44 bytes; ~10% growth). On `NodeAttribute<T>::get/set/tryGet`, the stored Tree pointer's generation is compared against the `Node`'s stamp. Cross-tree usage trips a `*Fatal` with both tree IDs in the message. Cost: per-access load + compare. Bounded.

- **Option B — Debug-only NodeId → TreeId registry.** A static-singleton `unordered_map<uint64_t, TreeId>` (NodeId.v + a small disambiguator) populated on every NodeId allocation in debug builds. `NodeAttribute<T>::get/set/tryGet` consults it. Free in release. Catches everything in debug. Cost: every NodeId allocation does a map insert in debug builds.

**Recommendation: Option A.** The 4-byte per-Node cost is paid once (immutable arena), the validation is per-access but trivial, and the protection is permanent (works in release builds). The `Node` struct is already 40 bytes — growing to 44 is well within the comfort zone. SH3 PR implements Option A unless concrete profiling pushes back.

**Surface.**
- `Tree` gets `treeId() -> TreeId` (already exists from skill description). On `TreeBuilder::finish()`, every `Node` in the arena gets stamped with this TreeId.
- `NodeAttribute<T>` captures `tree.treeId()` at ctor; every accessor validates the incoming `NodeId`'s stamp matches.
- `*Fatal` message: `"NodeAttribute<T> bound to TreeId=N got NodeId from TreeId=M"` — both IDs in the message so a death test can pin both.
- Death tests in `test_tree_attrs.cpp` covering: read-after-cross-set, write-with-foreign-id, `tryGet` returns nullptr-or-aborts (pick one — recommend abort, matches the rest of the file's discipline).
- `docs/tree-model.md` §5.10 caveat updated: strike "cross-tree confusion slips through"; add "cross-tree usage aborts with both TreeIds in the message."

**Tests.** 8–12 new test cases:
- Same-tree happy path (already covered — pin the count).
- Two-Tree cross-attack on `set`, `get`, `tryGet`, `erase`.
- Iterator validity across Tree drop (existing test — pin the count).
- Death-test regex matches both TreeIds in fatal message.
- Move-construction transfers the bound TreeId.

**Out of scope.** Cross-tree `Tree::children(NodeId)` walks — same class of bug, but the walks already abort on out-of-range; the same-range case is fixable by checking `node.treeId == tree.treeId` before returning the span. Bundle into the same PR; reuse the same `*Fatal` path.

---

## 3. Sequencing

SH1, SH2, SH3 are independent — any order works. Recommended: **SH2 first** (gets us the cross-platform safety net before SH3 touches hot-path code), then **SH3** (real substrate change benefits from a working CI matrix), then **SH1** (the script will sweep the SH2/SH3 hashes into the landing log on first run, dogfooding itself).

Per-PR cadence reuses the v2 discipline: implement → 5-agent review → comprehensive fix-all → cross-plan refresh → commit.

## 4. What comes after

Once SH1–SH3 land:
- `compiler-implementation-plan.md` §0 status table: SH row flips to ✅ done.
- Parent plan phase #5 (tokenizer) opens. The lexer can now be the *first* consumer of the schema with confidence that the substrate beneath it is verified across two platforms with no cross-tree footguns.
- v2 schema gaps that the tokenizer surfaces (lookahead variants, custom literal patterns, etc.) become candidates for a future `schema-expressiveness-v3-plan.md`.
