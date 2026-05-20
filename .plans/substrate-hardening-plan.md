# Substrate Hardening — Sub-Plan

> Closes three concrete gaps surfaced during v2 close-out **before** parent plan #5 (tokenizer) opens. Small, mechanical, ~1–2 days of work. No new substrate features — only de-risking of what already exists.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🔵 **in progress.** SH2 + SH3 done. SH1 remains. Trigger: v2 sub-plan ✅ done (PR8 + `4547301` followup). Successor: parent plan #5 (`tokenizer`). |
| Predecessors  | ✅ v1 T0–T12 (`tree-node-model-plan.md`); ✅ v2 PR0–PR8 (`schema-expressiveness-v2-plan.md`). |
| Successors    | Parent plan phase #5 (tokenizer). Cleanly verifying the schema against real tokens is what makes phase #7 (parser) tractable and what surfaces v3 schema gaps in time to act on them. |
| Scope         | **Bounded.** SH1: landing-log generator. SH2: Linux CI matrix entry. SH3: cross-tree NodeId guard. No other items will be added to this plan — anything new becomes a separate sub-plan. |

### PR landing log

| PR | Status | Commit / notes |
|---|---|---|
| SH2 | ✅ done | Multi-OS matrix already shipped by `DSS.DevOps@v2` (`cpp-app-pr.yml`): Linux/GCC-13/Release, Linux/Clang-19/Debug+ASan+UBSan, Windows/MSVC/Release, all default-enabled and green on recent PRs. This PR opts the consumer wiring into the `run-macos` leg (AppleClang on Homebrew LLVM). <!-- LANDING-LOG-HASHES: SH2 --> |
| SH3 | ✅ done | `NodeId` grew an 8-byte `treeTag` field; `TreeBuilder` mints `TreeId` eagerly at ctor and stamps every emitted id; `NodeAttribute<T>::validateId_` + `Tree::node_` abort on tag mismatch with both ids in the message. 523 ctest cases / 26 suites still 100% pass; 14 new death tests added. <!-- LANDING-LOG-HASHES: SH3 --> |
| SH1 | ⏳ pending | Landing-log generator script. |

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

### SH2 — Linux CI matrix entry ✅

**Goal.** Verify the cross-platform claim by running the full ctest suite on Ubuntu GCC in CI on every PR.

**Outcome.** Closed by upstream + a one-line opt-in here. The plan as originally written assumed CI was MinGW-on-Windows only and that a new matrix axis had to be added. Reality on inspection: `DSS.DevOps/.github/workflows/cpp-app-pr.yml@v2` already builds the matrix dynamically from `run-linux-gcc` / `run-linux-clang` / `run-windows-msvc` / `run-macos` boolean inputs, with the first three defaulting to true. The consumer wiring at `dss-code-prime/.github/workflows/pipeline-pr.yml` was already inheriting all three default-enabled legs and they were already green on recent PRs (verified on PR runs for `feature/v2`):

- `linux-gcc-release` — ubuntu-latest, GCC 13 / g++ 13, Release ✅
- `linux-clang-asan` — ubuntu-latest, Clang 19 / clang++ 19, Debug + ASan + UBSan ✅
- `windows-msvc-release` — windows-latest, MSVC 2022, Release ✅

So the original "Linux GCC" deliverable was already met, and the original "out of scope" ASAN/UBSAN bullet was also already met (Linux Clang leg). The "MinGW vs MSVC on Windows" caveat in the original surface description is also now stale: CI standardised on MSVC; MinGW remains the local-dev convention for the project author. Tests are green on both toolchains because the code path is the same.

**Diff in this PR.**
- Flip `run-macos: true` on the consumer call site (`pipeline-pr.yml`). The reusable workflow handles the Homebrew-LLVM stdlib bootstrap so AppleClang's lagging C++23 stdlib coverage doesn't bite.
- Refresh master-plan §0 status table to reflect the actual matrix that has been running. The "v1 tagged" label was rotted; it's `v2` now.

**Acceptance — verification path.**
- PR labeled `Run Pipes` triggers all four legs.
- All four conclude `success` on `ci-check`. Linux/macOS will surface any Windows-isms (forward-slash path assumptions, line-ending sensitivity in test goldens, `.dll` vs `.so` / `.dylib` lookup paths). If macOS fails, the smallest fix-it pattern is: a `target_compile_definitions` shim or a conditional `set(CMAKE_INSTALL_RPATH …)`. Larger Apple-Clang stdlib gaps revert the `run-macos: true` opt-in and file a follow-up SH-2.x — the multi-OS coverage from the three default legs is what closes SH2's stated goal.

**Out of scope.** Cross-compile docker toolchains (deferred — `docker/` is still empty). MSVC version pinning (handled by the runner image). Any change to the reusable workflow itself.

---

### SH3 — Cross-tree `NodeId` guard ✅

**Goal.** Make cross-tree `NodeId` usage a loud failure instead of a silent bounds-check pass.

**Design as shipped (deviates from the plan as originally written).** The original plan offered Option A ("stamp each Node with a TreeId; `NodeAttribute<T>` validates on access") and Option B (debug-only registry). On implementation, Option A as literally written is tautological: a `NodeAttribute<T>` reading the stamp from its *own bound tree* always observes a stamp equal to that bound tree's id, so cross-tree access — where a foreign `NodeId` is used to index into the bound tree's arena — never trips. The only way to detect provenance is to **carry it on the `NodeId` itself**.

Shipped design:

- **`NodeId` grows to 8 bytes**: `{ uint32_t v; uint32_t treeTag; }`. The `treeTag` is the source tree's id; `0` is the untagged sentinel.
- **Equality / `std::hash` compare `.v` only.** The tag is provenance metadata, not identity. Two `NodeId{3}` values compare equal even with different tags — this keeps existing tests that mix literal `NodeId{N}` with tagged-from-tree ids working without churn. The cross-tree validator is the enforcement point, not equality.
- **`TreeBuilder` mints `TreeId` eagerly at construction** (was: at `finish()` from a local static counter). Tags every NodeId it emits. The static counter moved to `TreeBuilder::nextTreeId()` so test helpers can share it.
- **`RawTreeBuilder`** accepts an explicit `TreeId` at construction (default `TreeId{1}` preserved for the existing `test_tree.cpp` assertion); tags every NodeId it creates and retags input parent/child references so callers can keep using literal `NodeId{N}` shorthand.
- **`Tree::node_`** (the universal accessor used by `kind`/`flags`/`children`/`parent`/`text`/...) validates `id.treeTag == 0 || id.treeTag == this.id().v`; mismatch aborts via `treeFatal` with both ids in the message.
- **`NodeAttribute<T>::validateId_`** does the same check against its captured `treeId_`; mismatch aborts via a new `crossTreeFatal(boundId, idTag)` helper. Untagged literals continue to bypass the tag check (test ergonomics); bounds check still catches genuinely-bad untagged ids.
- **Dense-iterator NodeIds synthesized post-promotion get the bound tree's tag** so iteration after sparse→dense promotion doesn't silently strip provenance.

Why `NodeId` carries the tag rather than just the `Node` (as the plan's Option A suggested): see the tautology argument above. The plan's "4 bytes per `Node`" cost estimate maps onto an effectively identical cost: `Node.parent` is a `NodeId`, so growing `NodeId` from 4 → 8 grows `Node.parent` by 4. The existing 4-byte trailing padding on `Node` (40-byte struct with `alignas(8)`, 36 bytes of named fields) absorbs the growth — `sizeof(detail::Node)` stays at 40. The real memory hit is `Tree::data_.childIndex` doubling (now 8 bytes / entry).

**Surface — final.**
- `src/core/types/strong_ids.hpp`: special-case `NodeId` outside the `DSS_STRONG_ID` macro to add `treeTag`. `std::hash<NodeId>` still keys on `.v` (the macro-emitted spec works as-is).
- `src/core/types/tree_builder.{hpp,cpp}`: new `treeId_` member, new `static TreeId nextTreeId() noexcept` factory, new `[[nodiscard]] TreeId treeId() const noexcept` accessor. `emit_` returns `NodeId{value, treeId_.v}`. `finish()` reuses `treeId_` for `td.id` and tags `td.root`.
- `src/core/types/tree.cpp`: `node_` validates the tag and emits `"Tree::node_: NodeId from TreeId={B} used on TreeId={A}"` on mismatch.
- `src/core/types/tree_attrs.hpp`: new `detail::attr::crossTreeFatal(boundId, idTag)` helper emits `"NodeAttribute bound to TreeId={A} got NodeId from TreeId={B}"`; `validateId_` calls it on mismatch. Dense iterator carries `treeTag_` so synthesized NodeIds inherit the bound tree's tag.
- `tests/core/raw_tree_builder.hpp`: ctor takes `TreeId` (default `TreeId{1}`); `addNode` retags untagged parent/child references; `finish` takes `optional<TreeId>` override.

**Tests added (14 new cases in `test_tree_attrs.cpp`, +3 in `test_strong_ids.cpp`):**
- `NodeAttributeDeath`: cross-tree {Set, Get, Has, TryGet, Erase} all match the expected "TreeId=111 ... TreeId=222" message.
- `NodeAttributeDeath.MovedAttributeRejectsForeignNodeId` — move transfers the bound TreeId.
- `NodeAttributeDeath.IteratorYieldsTaggedIdsAfterPromotion` — pins the dense-iter tagging fix.
- `NodeAttribute.UntaggedLiteralPassesValidator` + `MoveTransfersBoundTreeId` — same-tree happy paths.
- `TreeDeath`: cross-tree `children`, `kind` abort via `node_`.
- `Tree.ChildrenOnSameTreeReturnsTaggedIds` — pins the round-trip tagging on the child-table return.
- `StrongIds`: `NodeIdEqualityIgnoresTreeTag`, `NodeIdTwoArgCtorStoresTag`, `IsTriviallyCopyable` updated for `sizeof(NodeId) == 8`.

`docs/tree-model.md` §5 caveat rewritten: strikes "same-range cross-tree confusion slips through"; adds the new fatal message format. Skill (`SKILL.md`) sections updated for both the `NodeId` shape and the `NodeAttribute` invariant.

**Out of scope (carried forward to potential future SH or v3 work).**
- `TreeCursor::Bookmark`'s existing `TreeId` ABA-detection is unchanged — it was already correct and remains so.
- The `SchemaTreeBuilder` test helper in `test_tree_views.cpp` doesn't tag its NodeIds (it predates SH3 and has no cross-tree test needs). Same-tree access works because untagged passes the validator.

---

## 3. Sequencing

SH1, SH2, SH3 are independent — any order works. Original recommended order: **SH2 first** (cross-platform safety net), then **SH3**, then **SH1** (dogfoods itself). With SH2 collapsed to a no-op (matrix already shipped upstream), realised order is **SH2 → SH3 → SH1**.

Per-PR cadence reuses the v2 discipline: implement → 5-agent review → comprehensive fix-all → cross-plan refresh → commit.

## 4. What comes after

Once SH1–SH3 land:
- `compiler-implementation-plan.md` §0 status table: SH row flips to ✅ done.
- Parent plan phase #5 (tokenizer) opens. The lexer can now be the *first* consumer of the schema with confidence that the substrate beneath it is verified across two platforms with no cross-tree footguns.
- v2 schema gaps that the tokenizer surfaces (lookahead variants, custom literal patterns, etc.) become candidates for a future `schema-expressiveness-v3-plan.md`.
