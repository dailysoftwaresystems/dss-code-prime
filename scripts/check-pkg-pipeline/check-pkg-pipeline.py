#!/usr/bin/env python3
# PURPOSE: pin the package pipeline's release-path step sequence and refuse an artifacts-only run that can reach a release.
"""check-pkg-pipeline.py -- the PACKAGE PIPELINE guard.

WHAT IT IS ABOUT. `.github/workflows/pipeline-pkg.yml` has two modes that share
one body. A push to `release/{beta,stable}` cuts a release exactly as it always
did; a `workflow_dispatch` on any ref runs the SAME jobs and the SAME steps minus
three, and produces the tarballs as GitHub Actions artifacts without creating,
attaching to or publishing anything.

★★★ THE PROPERTY THIS FILE EXISTS TO PIN IS THE RELEASE PATH'S STEP SEQUENCE.
Operator, 2026-09-09: *"Ensure the release process will not be changed. The goal
is to ensure it will work"*. The mode was added by three `if:` conditions and one
side-effect-free step, so the whole of clause R is a literal, ordered pin of every
(job, step) a release push runs, in order, plus which of them may carry a mode
gate. An added job, a deleted step, a reordering, a step that quietly stops
running on a release push -- each is a refusal with its own message.

★★★ THAT FREEZE WAS LIFTED THE SAME DAY, AND CLAUSE D IS THE CONSEQUENCE.
Operator, 2026-09-09: *"for this one ... you close based on your genuine analysis.
If this fails after it I'll tell me and you'll rework on this. So please, close
once you really thinks it will work"*. Cycle P66 lane `rp` then made three changes
to the release path -- the durable artifact upload moved AHEAD of the release
upload, an asset-set assertion added to `finalize-release`, and
`if-no-files-found: error` on the artifact upload -- and moved clause R's pin to
match.
⚠⚠ AND THAT IS EXACTLY WHERE A PIN STOPS BEING A GUARD. Clause R is a literal
TRANSCRIPTION of the file, so anyone who reverts the workflow AND edits the tuple
back in the same commit satisfies it: the two move together by construction, and R
would report nothing. Clause D below therefore states the SAME properties as
DIRECTIONAL INVARIANTS that do not mention the transcription -- the durable copy's
index must be LESS than the release upload's, the asset-set probe's line must come
BEFORE the publish line -- so a revert trips D whether or not R was updated with
it. R answers "did anything move?"; D answers "did it move the WRONG WAY?", and
only D survives a reverter who is paying attention.

WHY AN INSTRUMENT AT ALL, measured 2026-09-09 via
`gh run list --workflow=pipeline-pkg.yml`: that workflow has TWO runs in its
entire history, both `startup_failure` at 0 s on 2026-07-07 at `b196c336`, where
the file still read `uses: dailysoftwaresystems/DSS.DevOps/...@v2` -- the
public-repo-cannot-call-a-private-reusable refusal the vendoring exists to fix.
⇒ THE VENDORED BODY HAS NEVER BEEN EXECUTED BY GITHUB AT ALL. Nothing in this
repository runs this YAML, and CI cannot test its own trigger matrix -- a run
only ever demonstrates the mode it happened to be in. So for this file, reading
IS the verification, and a guard is the only way to make a reading stick.

THE FAILURE DIRECTIONS IT REFUSES, and they are not symmetric:
  * A RELEASE-PATH CHANGE -- silent, because nothing runs this file until the day
    a release is cut, and then it is too late to be a test failure.
  * A DISPATCH THAT REACHES A RELEASE -- silent until it publishes something from
    a feature branch.
  * AN ARTIFACTS RUN THAT PRODUCES NOTHING -- silent, because a green run with an
    empty artifact list looks exactly like a green run.
All three fail toward *clean*, which is why they get an instrument rather than a
convention.

★ EACH CLAUSE NAMES WHAT WOULD DISARM IT FROM OUTSIDE ITS OWN FIELD OF VIEW, and
pins that too. That lesson cost this lane's predecessor a clause: a per-job
`permissions` check saw nothing when the WORKFLOW-LEVEL default was flipped,
because a job with no block of its own inherits silently. Every clause below
carries a `DISARMED BY` note and the sibling clause that catches it.

★ NO ESCAPE HATCH, DELIBERATELY. There is no marker a job can carry to opt out;
a guard whose refusal every subject can decline refuses nothing.

★ STDLIB ONLY, AND THAT IS A MEASUREMENT NOT A PREFERENCE. ✔No script in this
repository imports PyYAML and no CI step installs it, so a guard that did would
red on an interpreter's contents rather than on the tree. The reader below
understands exactly the YAML this file uses -- block mappings and block scalars --
and REFUSES rather than guesses when the file stops looking like that (clause S).
Failing toward refusal is the only direction a gate may fail in.

Self-tests on every run: each clause is mutated on a MIRROR of the real file and
must produce ITS OWN message id, never merely a non-zero exit -- an arm that
collapses onto a sibling's refusal is an arm that proves nothing.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# ⚠ AT IMPORT, COVERING BOTH STREAMS, and not inside main(): on Windows a Python
# child whose stdout is a PIPE comes up cp1252, so printing any of the glyphs a
# refusal here carries would raise inside the report and lose the finding. The
# `guard_output_encoding_guard` ratchet re-measures this every run and refuses a
# new script that lacks it (D-GATE-NOTHING-RATCHETS-A-NEW-GUARD-INTO-RECONFIGURING-ITS-STREAMS).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

WORKFLOW = Path(".github/workflows/pipeline-pkg.yml")

# The exact release branches the push trigger may name. Widening this set is how
# a release would be cut from a branch nobody meant to release from.
RELEASE_BRANCHES = ("release/beta", "release/stable")

# `gh release <verb>` verbs that CHANGE a release. `view`, `download` and `list`
# are reads and are deliberately absent: a read cannot publish anything, so
# refusing them would be a clause about tidiness rather than about the deliverable.
MUTATING_RELEASE_VERBS = ("create", "edit", "upload", "delete")

# The build legs. A leg that vanishes takes its platform's artifact with it and
# says nothing.
REQUIRED_PLATFORMS = ("linux-x64", "windows-x64", "macos-arm64", "linux-arm64")

BUILDER_JOB = "build-artifact"

# ── R. THE RELEASE PATH, PINNED ──────────────────────────────────────────────
# Every (job, step) a push to release/{beta,stable} runs, in order, and whether
# that step is allowed to carry a mode gate. `gate=True` means "release-only, and
# it MUST say so"; `gate=False` means "runs in BOTH modes, and must not acquire a
# mode gate" -- because a mode gate on a False row silently removes a step from
# the artifacts rehearsal, and its absence on a True row lets a dispatch mutate a
# release.
#
# ⚠ THIS IS THE WHOLE POINT OF THE FILE. Anything that reorders, inserts, drops
# or renames a row here is a change to the release process, which is the one thing
# the operator ruled out.
RELEASE_PATH: tuple[tuple[str, str, bool], ...] = (
    ("resolve-version", "Decide mode", False),
    ("resolve-version", "Generate GitHub App token", False),
    ("resolve-version", "uses: actions/checkout@v6", False),
    ("resolve-version", "Resolve version + channel", False),
    ("resolve-version", "Create draft release", True),
    ("build-matrix", "id: set", False),
    (BUILDER_JOB, "Generate GitHub App token", False),
    (BUILDER_JOB, "uses: actions/checkout@v6", False),
    (BUILDER_JOB, "Configure Git for private dependencies", False),
    (BUILDER_JOB, "Install Linux toolchain", False),
    (BUILDER_JOB, "Setup MSVC environment", False),
    (BUILDER_JOB, "Setup macOS toolchain", False),
    (BUILDER_JOB, "Setup Ninja (Windows)", False),
    (BUILDER_JOB, "Setup CMake", False),
    (BUILDER_JOB, "Configure", False),
    (BUILDER_JOB, "Build", False),
    (BUILDER_JOB, "Stage install set", False),
    (BUILDER_JOB, "Package artifact", False),
    # ⚠ THESE TWO ARE IN THIS ORDER ON PURPOSE, AND THE ORDER IS THE POINT.
    # The durable copy is taken FIRST so a transient failure of the release
    # upload cannot destroy a ~90-minute build. Clause D restates it as an
    # invariant that survives an edit to this very tuple.
    (BUILDER_JOB, "uses: actions/upload-artifact@v7", False),
    (BUILDER_JOB, "Attach artifact to draft release", True),
    ("finalize-release", "Generate GitHub App token", False),
    ("finalize-release", "Publish release (or leave it a draft on build failure)", False),
)

# The jobs, in order, and which of them is release-only AT THE JOB level.
JOB_ORDER: tuple[tuple[str, bool], ...] = (
    ("resolve-version", False),
    ("build-matrix", False),
    (BUILDER_JOB, False),
    ("finalize-release", True),
)

# Byte-pinned `if:` expressions. These are release-process facts, not style: the
# first decides whether four runners are allocated, the second decides whether a
# draft is published.
BUILDER_JOB_IF = "${{ needs.build-matrix.outputs.empty == 'false' }}"
FINALIZE_JOB_IF = (
    "${{ always() && needs.resolve-version.result == 'success'"
    " && needs.resolve-version.outputs.mode == 'release' }}"
)

# The workflow-level permission block, byte-pinned to the base's.
WORKFLOW_PERMISSIONS = ["contents: write"]

# The App-token step must exist, UNGATED, in exactly these jobs. Operator,
# 2026-09-09: *"As for the app token, this works ... just review the logic is
# good enough."* -- so it is pinned in place, not redesigned.
APP_TOKEN_JOBS = ("resolve-version", BUILDER_JOB, "finalize-release")
APP_TOKEN_ACTION = "actions/create-github-app-token@v3"

MODE_GATE = re.compile(r"outputs\.mode\s*==\s*'release'")
ANY_MODE_READ = re.compile(r"outputs\.mode\b")

# ── D. THE THREE DURABILITY PROPERTIES, AS INVARIANTS ────────────────────────
# Named here so the clause reads as a statement about the release path rather
# than as a second transcription of it.
DURABLE_COPY_STEP = "uses: actions/upload-artifact@v7"
RELEASE_UPLOAD_STEP = "Attach artifact to draft release"
NO_FILES_FOUND = "if-no-files-found: error"

# The asset-set assertion, by the three things that make it one: the probe, the
# comparison against the matrix's own count, and the publish it must precede.
ASSET_PROBE = "--json assets"
ASSET_COMPARE = '[ "$asset_count" -ne "$LEG_COUNT" ]'
PUBLISH_LINE = 'gh release edit "$REL_TAG" --draft=false'
ASSET_PROBE_FAIL_CLOSED = "Could not read $REL_TAG's asset list to verify it (treating as fatal)"

# The leg count, published by the job that builds the matrix and read by the job
# that publishes. A literal in either place is the defect this replaces.
LEG_COUNT_OUTPUT = "leg-count: ${{ steps.set.outputs.leg_count }}"
LEG_COUNT_COMPUTE = "jq 'length'"
LEG_COUNT_READ = "LEG_COUNT: ${{ needs.build-matrix.outputs.leg-count }}"


# ── the reader ────────────────────────────────────────────────────────────────
# Slices `jobs:` into per-job line ranges and each job into per-step line ranges.
# Block scalars (`run: |`) are tracked so their CONTENT is never mistaken for
# structure -- a shell line indented like a job key is the one way a naive
# splitter would silently mis-slice this file.

_JOB_KEY = re.compile(r"^  ([A-Za-z][A-Za-z0-9_-]*):\s*(#.*)?$")
_BLOCK_SCALAR = re.compile(r"^(\s*)[^#\n]*:\s*[|>][-+]?\d*\s*$")
_STEP_START = re.compile(r"^      - ")


class Step:
    def __init__(self) -> None:
        self.lines: list[str] = []        # every line, block-scalar content included
        self.structural: list[str] = []   # lines that are YAML structure

    @property
    def label(self) -> str:
        """`name:` if the step has one, else `uses: X`, else `id: X`.

        ⚠ THE FALLBACKS ARE NOT DECORATION. Two steps in this file are anonymous
        -- the two bare `- uses:` steps and `build-matrix`'s `- id: set` -- and a
        reader that called them all `<step>` would let one be swapped for another
        without clause R noticing.
        """
        for ln in self.structural:
            m = re.match(r"^      - name:\s*(.+?)\s*$", ln)
            if m:
                return m.group(1)
        for ln in self.structural:
            m = re.match(r"^      - uses:\s*(\S+)\s*$", ln)
            if m:
                return f"uses: {m.group(1)}"
        for ln in self.structural:
            m = re.match(r"^      - id:\s*(\S+)\s*$", ln)
            if m:
                return f"id: {m.group(1)}"
        return "<unnamed step>"

    @property
    def condition(self) -> str:
        for ln in self.structural:
            if re.match(r"^        if:", ln):
                return ln.split(":", 1)[1].strip()
        return ""


class Job:
    def __init__(self, name: str) -> None:
        self.name = name
        self.lines: list[str] = []
        self.structural: list[str] = []
        self.steps: list[Step] = []

    @property
    def condition(self) -> str:
        for ln in self.structural:
            if re.match(r"^    if:", ln):
                return ln.split(":", 1)[1].strip()
        return ""

    @property
    def permissions(self) -> list[str] | None:
        """The job's OWN block, or None when it inherits the workflow default."""
        out: list[str] | None = None
        grabbing = False
        for ln in self.structural:
            if re.match(r"^    permissions:\s*$", ln):
                out, grabbing = [], True
                continue
            if re.match(r"^    permissions:\s*\S", ln):
                # scalar shorthand (`permissions: write-all`) -- a block with one
                # entry, so it is reported rather than read as "no block".
                return [ln.split(":", 1)[1].strip()]
            if grabbing:
                if re.match(r"^      \S", ln):
                    out.append(ln.strip())
                    continue
                grabbing = False
        return out


def read_jobs(text: str) -> tuple[list[Job], list[str]]:
    lines = text.splitlines()
    problems: list[str] = []

    try:
        start = next(i for i, ln in enumerate(lines) if ln == "jobs:")
    except StopIteration:
        return [], ["PKG-SHAPE-NO-JOBS-MAPPING: no top-level `jobs:` key."]

    jobs: list[Job] = []
    current: Job | None = None
    i = start + 1
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped == "":
            i += 1
            continue

        indent = len(line) - len(line.lstrip())
        if indent == 0 and not stripped.startswith("#"):
            break  # left the jobs mapping

        m = _JOB_KEY.match(line)
        if m and not stripped.startswith("#"):
            current = Job(m.group(1))
            jobs.append(current)
            i += 1
            continue

        if current is not None:
            current.lines.append(line)
            current.structural.append(line)
            if _STEP_START.match(line):
                current.steps.append(Step())
            if current.steps:
                current.steps[-1].lines.append(line)
                current.steps[-1].structural.append(line)

        scalar = _BLOCK_SCALAR.match(line)
        if scalar and not stripped.startswith("#"):
            key_indent = len(scalar.group(1))
            i += 1
            while i < len(lines):
                inner = lines[i]
                if inner.strip() == "":
                    if current is not None:
                        current.lines.append(inner)
                        if current.steps:
                            current.steps[-1].lines.append(inner)
                    i += 1
                    continue
                if len(inner) - len(inner.lstrip()) <= key_indent:
                    break
                if current is not None:
                    # content only -- NEVER structure, and never a step boundary
                    current.lines.append(inner)
                    if current.steps:
                        current.steps[-1].lines.append(inner)
                i += 1
            continue

        i += 1

    for job in jobs:
        if not any(re.match(r"^    runs-on:", ln) for ln in job.structural):
            problems.append(
                f"PKG-SHAPE-JOB-WITHOUT-RUNS-ON: job `{job.name}` has no job-level "
                f"`runs-on:`; this reader mis-sliced the file."
            )
        for step in job.steps:
            if step.label == "<unnamed step>":
                problems.append(
                    f"PKG-SHAPE-UNREADABLE-STEP: a step in job `{job.name}` has no "
                    f"`name:`, `uses:` or `id:` this reader can key on, so clause R "
                    f"cannot tell it from any other step."
                )
    return jobs, problems


def strip_comments(text: str) -> str:
    """Blank every whole-line comment, keeping the line count.

    ⚠ EVERY CLAUSE BELOW READS THIS, NOT THE RAW FILE, and it is not tidiness:
    an earlier draft refused the real, correct file three times over prose --
    `gh release upload` named in a comment explaining the upload, a
    `cmake --install` named in the trigger rationale, and a key named in the
    comment above it, which made a mutation arm rewrite the sentence instead of
    the key. A guard that cannot tell an invocation from a sentence about one is
    answering an adjacent question, and it fails toward NOISE, which is how a
    guard gets disabled.
    ⓘ Whole-line only. A trailing `# ...` after code is left alone, because
    cutting at a `#` would also cut one inside a quoted shell string, and a
    reader that corrupts the line it is judging is worse than one that reads a
    little too much.
    """
    return "\n".join("" if ln.lstrip().startswith("#") else ln for ln in text.splitlines())


# ── the clauses ───────────────────────────────────────────────────────────────

def check(raw: str) -> list[str]:
    bad: list[str] = []
    text = strip_comments(raw)
    jobs, shape = read_jobs(text)
    bad.extend(shape)
    if not jobs:
        return bad
    by_name = {j.name: j for j in jobs}

    # ── R. THE RELEASE PATH'S JOB AND STEP SEQUENCE ─────────────────────────
    # DISARMED BY: nothing inside a job -- which is why it is written as an
    # absolute ordered pin rather than as a set of per-job assertions. It is in
    # turn what catches an edit that would disarm clauses C, D and U by adding,
    # deleting or reordering the step they judge.
    got_jobs = [j.name for j in jobs]
    want_jobs = [n for n, _ in JOB_ORDER]
    if got_jobs != want_jobs:
        bad.append(
            f"PKG-JOB-SEQUENCE-CHANGED: the file defines jobs {got_jobs}; the release "
            f"path is {want_jobs}, in that order. A job added, removed or reordered is "
            "a change to the release process."
        )
    else:
        got_path = [(j.name, s.label) for j in jobs for s in j.steps]
        want_path = [(job, step) for job, step, _ in RELEASE_PATH]
        aligned = got_path == want_path
        if not aligned:
            missing = [p for p in want_path if p not in got_path]
            added = [p for p in got_path if p not in want_path]
            bad.append(
                "PKG-RELEASE-PATH-STEP-SEQUENCE-CHANGED: the (job, step) sequence is not "
                f"the pinned release path. missing={missing} added={added} "
                f"reordered={not (missing or added)}. "
                "Operator 2026-09-09: the release process will not be changed."
            )

        # Which steps carry a mode gate, and which must not.
        # ⚠ ONLY WHEN THE SEQUENCE ALIGNS. Pairing a pinned row against a step it
        # is not describing would name the wrong step in every message -- a true
        # refusal with a false reason, which is how a guard gets distrusted and
        # then disabled.
        for (job_name, step_label, want_gate), step in zip(
            RELEASE_PATH, (s for j in jobs for s in j.steps) if aligned else ()
        ):
            gated = bool(MODE_GATE.search(step.condition))
            if want_gate and not gated:
                bad.append(
                    f"PKG-MODE-GATE-REMOVED: step `{step_label}` in job `{job_name}` no "
                    "longer carries `outputs.mode == 'release'`, so an artifacts-only "
                    "dispatch would run it and reach a release."
                )
            if not want_gate and ANY_MODE_READ.search(step.condition):
                bad.append(
                    f"PKG-MODE-GATE-ADDED: step `{step_label}` in job `{job_name}` reads "
                    "the mode in its `if:`. That step runs in BOTH modes; gating it makes "
                    "the artifacts run something other than a rehearsal of the release build."
                )
            if step.condition and ANY_MODE_READ.search(step.condition) and not gated:
                bad.append(
                    f"PKG-MODE-GATE-POLARITY: step `{step_label}` in job `{job_name}` reads "
                    f"the mode as `{step.condition}` rather than `== 'release'`. Only the "
                    "positive form keeps a dispatch away from a release."
                )

    for name, want_gate in JOB_ORDER:
        job = by_name.get(name)
        if job is None:
            continue
        gated = bool(MODE_GATE.search(job.condition))
        if want_gate and not gated:
            bad.append(
                f"PKG-JOB-MODE-GATE-REMOVED: job `{name}` is release-only but its `if:` "
                f"({job.condition!r}) does not test `outputs.mode == 'release'`."
            )
        if not want_gate and ANY_MODE_READ.search(job.condition):
            bad.append(
                f"PKG-JOB-MODE-GATE-ADDED: job `{name}` is gated on the mode, so one of "
                "the two modes skips it entirely. Both modes must run it unchanged."
            )

    if by_name.get(BUILDER_JOB) and by_name[BUILDER_JOB].condition != BUILDER_JOB_IF:
        bad.append(
            f"PKG-BUILDER-JOB-IF-CHANGED: `{BUILDER_JOB}`'s `if:` is "
            f"{by_name[BUILDER_JOB].condition!r}; the release path's is {BUILDER_JOB_IF!r}."
        )
    if by_name.get("finalize-release") and by_name["finalize-release"].condition != FINALIZE_JOB_IF:
        bad.append(
            f"PKG-FINALIZE-JOB-IF-CHANGED: `finalize-release`'s `if:` is "
            f"{by_name['finalize-release'].condition!r}; expected {FINALIZE_JOB_IF!r} -- the "
            "base expression plus the mode conjunct, which is TRUE on every release push."
        )

    # ── T. trigger integrity ────────────────────────────────────────────────
    # DISARMED BY: nothing -- but note T2 is DIRECTIONAL and only about `push`.
    # Any other event maps to artifacts by construction (clause M), so a new
    # trigger is safe and is not refused; a WIDER push is the unsafe direction,
    # because push is the one event that cuts a release.
    if not re.search(r"^  workflow_dispatch:\s*$", text, re.M):
        bad.append(
            "PKG-TRIGGER-DISPATCH-MISSING: `on:` has no `workflow_dispatch:`, so there "
            "is no way to produce artifacts without pushing a release branch."
        )
    push_branches = re.search(r"^  push:\n    branches:\s*\[(.*)\]\s*$", text, re.M)
    if not push_branches:
        bad.append(
            "PKG-TRIGGER-PUSH-UNBOUNDED: the `push` trigger does not name an explicit "
            "`branches: [...]` list, so a push to any branch would run in release mode."
        )
    else:
        named = [b.strip().strip('"').strip("'") for b in push_branches.group(1).split(",")]
        if named != list(RELEASE_BRANCHES):
            bad.append(
                f"PKG-TRIGGER-PUSH-CHANGED: the `push` trigger names {named}; the release "
                f"branches are {list(RELEASE_BRANCHES)}. A push anywhere else would cut a release."
            )
    if re.search(r"^  push:\n(?:    .*\n)*?    tags:", text, re.M):
        bad.append(
            "PKG-TRIGGER-PUSH-ON-TAGS: the `push` trigger names `tags:`. A tag push would "
            "enter release mode with `GITHUB_REF` pointing at a tag, and the channel "
            "derivation reads a BRANCH."
        )

    # ── M. one mode decision, and nothing else re-derives it ────────────────
    # DISARMED BY: re-deriving the mode from `github.event_name` somewhere else,
    # which M3 is what catches. M1 alone would not: a second gate spelled
    # `github.event_name == 'push'` writes no `mode=` at all.
    writes = [ln for ln in text.splitlines() if 'echo "mode=' in ln and "GITHUB_OUTPUT" in ln]
    if len(writes) != 1:
        bad.append(
            f"PKG-MODE-NOT-ONE-DECISION: {len(writes)} step(s) write `mode=` to "
            "$GITHUB_OUTPUT; exactly one may. Two spellings of one decision are two "
            "things that can disagree, and the disagreement is `a dispatch cut a release`."
        )
    if not re.search(
        r'if \[ "\$GITHUB_EVENT_NAME" = "push" \]; then\s*\n\s*mode=release\s*\n\s*else\s*\n\s*mode=artifacts',
        text,
    ):
        bad.append(
            "PKG-MODE-MAPPING-CHANGED: the `Decide mode` step no longer maps exactly "
            "`push -> release` and everything else -> `artifacts`."
        )
    decide = None
    for job in jobs:
        for step in job.steps:
            if step.label == "Decide mode":
                decide = step
    event_reads = len(re.findall(r"GITHUB_EVENT_NAME|github\.event_name", text))
    inside = 0 if decide is None else len(
        re.findall(r"GITHUB_EVENT_NAME|github\.event_name", "\n".join(decide.lines))
    )
    if event_reads != inside:
        bad.append(
            f"PKG-EVENT-NAME-READ-OUTSIDE-DECIDE-MODE: the event name is read "
            f"{event_reads} time(s) in the file but only {inside} of those are inside the "
            "`Decide mode` step. Every other gate must read the mode OUTPUT, or the file "
            "carries two spellings of one decision."
        )
    if not re.search(r"^      mode: \$\{\{ steps\.mode\.outputs\.mode \}\}\s*$", text, re.M):
        bad.append(
            "PKG-MODE-NOT-A-JOB-OUTPUT: `resolve-version` does not publish "
            "`mode: ${{ steps.mode.outputs.mode }}`, so no downstream job can read the "
            "one decision and each would have to re-derive it."
        )

    # ── C. every release mutation sits behind the mode gate ─────────────────
    # DISARMED BY: moving the invocation into a step or job the pin above says is
    # ungated -- clause R sees that, because the step sequence is absolute.
    for job in jobs:
        job_gated = bool(MODE_GATE.search(job.condition))
        for step in job.steps:
            step_gated = job_gated or bool(MODE_GATE.search(step.condition))
            for ln in step.lines:
                m = re.search(r"gh release (\w+)", ln)
                if m and m.group(1) in MUTATING_RELEASE_VERBS and not step_gated:
                    bad.append(
                        f"PKG-RELEASE-MUTATION-UNGATED: job `{job.name}` step "
                        f"`{step.label}` runs `gh release {m.group(1)}` with no "
                        "`outputs.mode == 'release'` on the step or the job. An "
                        "artifacts-only run would mutate a release."
                    )

    # ── P. permissions are the base's, and no job overrides them ────────────
    # ⚠⚠ P1 AND P2 ARE ONE CLAUSE IN TWO HALVES, and either alone refuses nothing.
    # A job with no `permissions:` block INHERITS the workflow-level default, so a
    # per-job check cannot see the default move; and a default check cannot see a
    # job that overrides it. The predecessor of this file shipped only the per-job
    # half and its central clause was disarmed from outside its field of view.
    default_perms = re.search(r"^permissions:\n((?:  \S.*\n)+)", text, re.M)
    if not default_perms:
        bad.append(
            "PKG-WORKFLOW-PERMISSIONS-GONE: the workflow declares no top-level "
            "`permissions:` block, so every job inherits the repository default. The "
            "release path declares `contents: write` and must keep declaring it."
        )
    else:
        granted = [ln.strip() for ln in default_perms.group(1).splitlines() if ln.strip()]
        if granted != WORKFLOW_PERMISSIONS:
            bad.append(
                f"PKG-WORKFLOW-PERMISSIONS-CHANGED: the top-level `permissions:` block is "
                f"{granted}; the release path's is {WORKFLOW_PERMISSIONS}. Both directions "
                "are refusals: a widening hands every job more than it needs, and a "
                "narrowing changes what a release run can do."
            )
    for job in jobs:
        if job.permissions is not None:
            bad.append(
                f"PKG-JOB-OVERRIDES-PERMISSIONS: job `{job.name}` declares its own "
                f"`permissions:` ({job.permissions}), silently overriding the workflow "
                "block. The release path has no per-job permissions at all."
            )

    # ── U. the release path's identity: App token, token'd checkout, insteadOf ─
    # DISARMED BY: leaving the step in place and gating it off, which would remove
    # it from a release run while every `in this job` check still passed -- so the
    # UNGATED half is asserted with the same breath as the existence half.
    for name in APP_TOKEN_JOBS:
        job = by_name.get(name)
        if job is None:
            continue
        owners = [s for s in job.steps if any(APP_TOKEN_ACTION in ln for ln in s.lines)]
        if len(owners) != 1:
            bad.append(
                f"PKG-APP-TOKEN-STEP-GONE: job `{name}` has {len(owners)} step(s) using "
                f"`{APP_TOKEN_ACTION}`; the release path has exactly one. Operator "
                "2026-09-09: the App token works and stays as it is."
            )
        elif owners[0].condition:
            bad.append(
                f"PKG-APP-TOKEN-STEP-GATED: job `{name}`'s App-token step carries "
                f"`if: {owners[0].condition}`. A gated token step is a token that is "
                "absent on some path, and the steps after it would run with an empty one."
            )
    builder = by_name.get(BUILDER_JOB)
    if builder is not None:
        checkout = [s for s in builder.steps if s.label == "uses: actions/checkout@v6"]
        if not checkout or not any(
            "token: ${{ steps.app-token.outputs.token }}" in ln for ln in checkout[0].lines
        ):
            bad.append(
                f"PKG-BUILDER-CHECKOUT-UNTOKENED: `{BUILDER_JOB}`'s `actions/checkout` no "
                "longer passes the App token. That is what the release path checks out with."
            )
        insteadof = [s for s in builder.steps if any('.insteadOf "git@github.com:"' in ln for ln in s.lines)]
        if len(insteadof) != 1 or insteadof[0].condition:
            bad.append(
                f"PKG-BUILDER-INSTEADOF-GONE: `{BUILDER_JOB}` no longer runs an ungated "
                "`git config --global url....insteadOf \"git@github.com:\"` step. It may "
                "match nothing today, but removing it is a change to the release process."
            )

    # ── E. one owner of build + package ─────────────────────────────────────
    # DISARMED BY: forking the body into a second job -- so the clause counts
    # OCCURRENCES as well as owners; a copy inside the same job still trips it.
    for needle, what in (
        ("cmake --build", "the build"),
        ("cmake --install", "the install staging"),
        ("tar -czf", "the tarball"),
    ):
        owners = [j.name for j in jobs if any(needle in ln for ln in j.lines)]
        occurrences = text.count(needle)
        if owners != [BUILDER_JOB] or occurrences != 1:
            bad.append(
                f"PKG-PACKAGING-FORKED: `{needle}` ({what}) occurs {occurrences} time(s) in "
                f"job(s) {owners}; it must occur exactly once, in `{BUILDER_JOB}`. A second "
                "copy of the packaging body is a second owner of it."
            )

    # ── N. the artifacts mode actually produces something ───────────────────
    # DISARMED BY: gating the upload -- covered by clause R's `gate=False` row for
    # it -- or by giving all four legs ONE artifact name, which `upload-artifact`
    # v4+ refuses at RUNTIME, i.e. after a 90-minute build.
    upload = None
    if builder is not None:
        for s in builder.steps:
            if s.label == "uses: actions/upload-artifact@v7":
                upload = s
    if upload is None:
        bad.append(
            f"PKG-ARTIFACT-UPLOAD-GONE: `{BUILDER_JOB}` no longer runs "
            "`actions/upload-artifact`, so an artifacts-only run produces nothing at all "
            "and a release run loses its second copy of the tarball."
        )
    else:
        name_line = [ln for ln in upload.lines if re.match(r"^          name:", ln)]
        if not name_line or not re.search(r"matrix\.(name|platform)", name_line[0]):
            bad.append(
                "PKG-ARTIFACT-NAME-NOT-PER-LEG: the uploaded artifact's `name:` does not "
                f"vary with the matrix leg ({name_line or '<absent>'}). Four legs uploading "
                "one name is a runtime refusal from upload-artifact v4+, after the build."
            )

    # ── D. THE DURABILITY INVARIANTS, STATED WITHOUT THE TRANSCRIPTION ──────
    # ★★ WHY THIS IS NOT A DUPLICATE OF CLAUSE R. R compares the file against a
    # literal tuple in this same source, so a reverter who edits BOTH files in one
    # commit satisfies it and R says nothing -- a guard that merely follows the
    # code. D states the same three properties as DIRECTIONS (an index ordering, a
    # line ordering, a value) with no tuple to edit, so the revert still refuses.
    # Both clauses are kept: R catches every unintended move, D catches the
    # intended-but-wrong one.
    #
    # DISARMED BY, and each is pinned:
    #   * deleting the durable copy entirely -> PKG-ARTIFACT-UPLOAD-GONE (clause N),
    #     which is why D1 stays silent when the step is absent rather than
    #     inventing a second refusal for one edit;
    #   * gating the durable copy off with a NON-mode `if:`, which
    #     PKG-MODE-GATE-ADDED cannot see because it only looks for `outputs.mode`
    #     -> PKG-ARTIFACT-UPLOAD-GATED;
    #   * `continue-on-error` on the release upload (or on the job), which makes
    #     the ordering moot by making a failed upload green -> PKG-CONTINUE-ON-ERROR;
    #   * replacing the leg count with a literal, which keeps the assertion's SHAPE
    #     while making it agree with a four-asset release forever
    #     -> PKG-LEG-COUNT-NOT-FROM-THE-MATRIX.
    if builder is not None and upload is not None:
        labels = [s.label for s in builder.steps]
        if RELEASE_UPLOAD_STEP in labels:
            if labels.index(DURABLE_COPY_STEP) > labels.index(RELEASE_UPLOAD_STEP):
                bad.append(
                    f"PKG-DURABLE-COPY-NOT-FIRST: in `{BUILDER_JOB}`, `{DURABLE_COPY_STEP}` "
                    f"runs AFTER `{RELEASE_UPLOAD_STEP}`. Steps run in file order and a "
                    "failed step stops the ones after it, so in that order a transient "
                    "failure of the release upload destroys the only copy of a build "
                    "carrying `timeout-minutes: 90`. The durable copy is taken FIRST."
                )
        if upload.condition:
            bad.append(
                f"PKG-ARTIFACT-UPLOAD-GATED: the durable copy carries `if: "
                f"{upload.condition}`. A conditional durable copy is no durable copy on "
                "the path where the condition is false, and a non-mode condition is "
                "invisible to the mode-gate clause."
            )
        if not any(NO_FILES_FOUND in ln for ln in upload.lines):
            bad.append(
                f"PKG-ARTIFACT-UPLOAD-NO-FILES-NOT-ERROR: the durable copy does not set "
                f"`{NO_FILES_FOUND}`. The action's default is `warn`, which is a GREEN run "
                "that shipped nothing -- and this step is the last line of defence for the "
                "tarball."
            )

    # `continue-on-error` at EITHER level. The job level is the more dangerous of
    # the two and the one a per-step reading would miss: it stops a failed leg from
    # making the run red, which is the whole mechanism by which a failed upload
    # leaves the draft unpublished.
    for m in re.finditer(r"^( +)continue-on-error:\s*(\S+)\s*$", text, re.M):
        level = "job" if len(m.group(1)) == 4 else "step"
        bad.append(
            f"PKG-CONTINUE-ON-ERROR: a {level}-level `continue-on-error: {m.group(2)}` is "
            "declared. The release path has none: it is what turns a failed release "
            "upload into a green leg, and a green leg is what lets `finalize-release` "
            "publish a release whose assets were never attached."
        )

    # The asset-set assertion: it must exist, count against the MATRIX's own
    # output, fail closed on a broken API call, and run BEFORE the publish.
    finalize = by_name.get("finalize-release")
    publish = None
    if finalize is not None:
        for s in finalize.steps:
            if any(PUBLISH_LINE in ln for ln in s.lines):
                publish = s
    if publish is None:
        pass  # PKG-PUBLISH-STEP-GONE (clause G) already names this.
    else:
        body = publish.lines
        probe_at = next((i for i, ln in enumerate(body) if ASSET_PROBE in ln), None)
        publish_at = next((i for i, ln in enumerate(body) if PUBLISH_LINE in ln), None)
        if probe_at is None or not any(ASSET_COMPARE in ln for ln in body):
            bad.append(
                "PKG-ASSET-SET-NOT-VERIFIED: `finalize-release` does not read the release's "
                f"assets (`gh release view ... {ASSET_PROBE}`) and compare the tarball count "
                f"against the matrix (`{ASSET_COMPARE}`) before publishing. Checking job "
                "RESULTS is a statement about what ran, not about what is attached."
            )
        elif publish_at is not None and probe_at > publish_at:
            bad.append(
                "PKG-ASSET-SET-CHECK-AFTER-PUBLISH: the asset-set assertion appears AFTER "
                f"`{PUBLISH_LINE}`. A check that runs after the irreversible act is not a "
                "check; publishing is what freezes an immutable release's assets."
            )
        if ASSET_PROBE_FAIL_CLOSED not in "\n".join(body):
            bad.append(
                "PKG-ASSET-SET-PROBE-NOT-FAIL-CLOSED: the asset probe does not tell a "
                "genuine short count apart from a failed API call. One that treats a 5xx as "
                "`no assets` turns a transient into a refused release with a misleading "
                "message; one that treats it as `fine` is not an assertion."
            )
        if LEG_COUNT_READ not in "\n".join(body):
            bad.append(
                f"PKG-LEG-COUNT-NOT-FROM-THE-MATRIX: `finalize-release` does not take its "
                f"expectation from `{LEG_COUNT_READ}`. A literal count keeps a five-leg "
                "release passing a four-asset check, which is the silent half of the defect "
                "the assertion exists to close."
            )
    matrix_job = by_name.get("build-matrix")
    if matrix_job is not None:
        joined = "\n".join(matrix_job.lines)
        if LEG_COUNT_OUTPUT not in joined or LEG_COUNT_COMPUTE not in joined:
            bad.append(
                "PKG-LEG-COUNT-NOT-PUBLISHED: `build-matrix` does not compute the leg count "
                f"with `{LEG_COUNT_COMPUTE}` over the array it built and publish it as "
                f"`{LEG_COUNT_OUTPUT}`. The expectation must come from the job that decides "
                "how many legs there are, or it is a second owner of that number."
            )

    # ── G. the fail-loud checks the packaging path already had ──────────────
    # DISARMED BY: nothing structural -- these are the refusals themselves, so
    # they are pinned by their message text, which is what a run would print.
    for needle, message in (
        (
            "Package produced an archive with no files",
            "PKG-EMPTY-ARCHIVE-CHECK-GONE: the `Package artifact` step no longer refuses "
            "an archive with zero regular files.",
        ),
        (
            "the staged install set carries no dss-config tree at all",
            "PKG-CONFIG-TREE-CHECK-GONE: the `Stage install set` step no longer refuses a "
            "staged prefix with no dss-config tree, so a package that cannot resolve "
            "`#include <stdio.h>` would ship green.",
        ),
        (
            "Could not verify whether $tag exists (treating as fatal)",
            "PKG-TAG-PROBE-NOT-FAIL-CLOSED: the release-existence check no longer tells a "
            "genuine 404 apart from a transient error, so a 5xx would race ahead and "
            "create a duplicate release.",
        ),
        (
            'gh release edit "$REL_TAG" --draft=false',
            "PKG-PUBLISH-STEP-GONE: `finalize-release` no longer flips the draft to "
            "published, so a release run would leave every release a draft.",
        ),
    ):
        if needle not in text:
            bad.append(message)

    # ── H. the platforms ────────────────────────────────────────────────────
    for platform in REQUIRED_PLATFORMS:
        if f'"platform":"{platform}"' not in text:
            bad.append(
                f"PKG-PLATFORM-LEG-GONE: the build matrix no longer emits a `{platform}` "
                "leg, so no artifact is produced for it."
            )

    return bad


# ── self-test ─────────────────────────────────────────────────────────────────
# Every arm mutates the REAL file text and asserts ITS OWN message id. Asserting
# the id rather than the exit status is what stops an arm passing because the
# mutation happened to trip a sibling clause.

def _arms(text: str) -> list[tuple[str, str, str]]:
    """(arm name, mutated text, message id that must appear)."""

    def sub(old: str, new: str, count: int = 1) -> str:
        assert old in text, f"self-test fixture is stale: {old!r} not in the workflow"
        return text.replace(old, new, count)

    builder_anchor = "  build-artifact:\n"
    attach_gate = (
        "      - name: Attach artifact to draft release\n"
        "        if: ${{ needs.resolve-version.outputs.mode == 'release' }}\n"
    )
    draft_gate = (
        "      - name: Create draft release\n"
        "        if: ${{ steps.mode.outputs.mode == 'release' }}\n"
    )
    upload_block = (
        "      - uses: actions/upload-artifact@v7\n"
        "        with:\n"
        "          name: ${{ matrix.name }}-${{ needs.resolve-version.outputs.version }}\n"
        "          path: ${{ '.' }}/dist/*.tar.gz\n"
        "          if-no-files-found: error\n"
    )
    attach_run = '        run: gh release upload "$TAG" dist/*.tar.gz --clobber\n'
    result_gate = (
        '          if [ "$MATRIX_EMPTY" != "true" ] && [ "$BUILD_RESULT" != "success" ]; then\n'
    )
    return [
        # ── R: the release path ──────────────────────────────────────────────
        (
            "a step deleted from the release path",
            sub(
                "      - name: Configure Git for private dependencies\n"
                '        run: git config --global url."https://x-access-token:${{ steps.app-token.outputs.token }}@github.com/".insteadOf "git@github.com:"\n\n',
                "",
            ),
            "PKG-RELEASE-PATH-STEP-SEQUENCE-CHANGED",
        ),
        (
            "the release upload moved into a job of its own",
            sub(builder_anchor, "  attach-artifacts:\n    runs-on: ubuntu-latest\n    steps:\n"
                               "      - name: attach\n        run: echo hi\n" + builder_anchor),
            "PKG-JOB-SEQUENCE-CHANGED",
        ),
        (
            "the draft creation loses its mode gate",
            sub(draft_gate, "      - name: Create draft release\n"),
            "PKG-MODE-GATE-REMOVED",
        ),
        (
            "a both-modes step acquires a mode gate",
            sub(
                "      - name: Package artifact\n",
                "      - name: Package artifact\n        if: ${{ needs.resolve-version.outputs.mode == 'release' }}\n",
            ),
            "PKG-MODE-GATE-ADDED",
        ),
        (
            "the attach gate flipped to the wrong polarity",
            sub(attach_gate, "      - name: Attach artifact to draft release\n"
                             "        if: ${{ needs.resolve-version.outputs.mode != 'artifacts' }}\n"),
            "PKG-MODE-GATE-POLARITY",
        ),
        (
            "finalize-release loses its mode gate",
            sub(
                "    if: ${{ always() && needs.resolve-version.result == 'success' && needs.resolve-version.outputs.mode == 'release' }}",
                "    if: ${{ always() && needs.resolve-version.result == 'success' }}",
            ),
            "PKG-FINALIZE-JOB-IF-CHANGED",
        ),
        (
            # The same cell as the arm above, mutated the OTHER way it realistically
            # goes: re-spelled rather than deleted. It proves the mode-gate clause
            # itself, which the byte-pin would otherwise mask.
            "finalize-release gated on the event name instead of the mode",
            sub(
                "&& needs.resolve-version.outputs.mode == 'release' }}",
                "&& github.event_name == 'push' }}",
            ),
            "PKG-JOB-MODE-GATE-REMOVED",
        ),
        (
            "the build job gated on the mode",
            sub(
                "    if: ${{ needs.build-matrix.outputs.empty == 'false' }}",
                "    if: ${{ needs.resolve-version.outputs.mode == 'release' }}",
            ),
            "PKG-JOB-MODE-GATE-ADDED",
        ),
        (
            "the build job's empty-matrix gate retargeted",
            sub(
                "    if: ${{ needs.build-matrix.outputs.empty == 'false' }}",
                "    if: ${{ needs.build-matrix.outputs.empty != 'true' }}",
            ),
            "PKG-BUILDER-JOB-IF-CHANGED",
        ),
        # ── T: triggers ──────────────────────────────────────────────────────
        (
            "dispatch trigger removed",
            sub("  workflow_dispatch:\n", ""),
            "PKG-TRIGGER-DISPATCH-MISSING",
        ),
        (
            "push trigger widened to a feature branch",
            sub(
                'branches: ["release/beta", "release/stable"]',
                'branches: ["release/beta", "release/stable", "main"]',
            ),
            "PKG-TRIGGER-PUSH-CHANGED",
        ),
        (
            "push trigger left unbounded",
            sub('  push:\n    branches: ["release/beta", "release/stable"]', "  push:"),
            "PKG-TRIGGER-PUSH-UNBOUNDED",
        ),
        (
            "push trigger given tags",
            sub(
                '  push:\n    branches: ["release/beta", "release/stable"]',
                '  push:\n    branches: ["release/beta", "release/stable"]\n    tags: ["v*"]',
            ),
            "PKG-TRIGGER-PUSH-ON-TAGS",
        ),
        # ── M: one mode decision ─────────────────────────────────────────────
        (
            "mode decided twice",
            sub(
                'echo "mode=$mode" >> "$GITHUB_OUTPUT"',
                'echo "mode=$mode" >> "$GITHUB_OUTPUT"\n          echo "mode=release" >> "$GITHUB_OUTPUT"',
            ),
            "PKG-MODE-NOT-ONE-DECISION",
        ),
        (
            "dispatch remapped to release",
            sub("            mode=release\n", "            mode=RELEASE\n"),
            "PKG-MODE-MAPPING-CHANGED",
        ),
        (
            # ⚠ The arm for the disarm: this re-derives the decision somewhere
            # else, writing no `mode=` at all, so PKG-MODE-NOT-ONE-DECISION is
            # blind to it.
            "a second gate re-derives the mode from the event name",
            sub(attach_gate, "      - name: Attach artifact to draft release\n"
                             "        if: ${{ needs.resolve-version.outputs.mode == 'release' && github.event_name == 'push' }}\n"),
            "PKG-EVENT-NAME-READ-OUTSIDE-DECIDE-MODE",
        ),
        (
            "the mode stops being a job output",
            sub("      mode: ${{ steps.mode.outputs.mode }}\n", ""),
            "PKG-MODE-NOT-A-JOB-OUTPUT",
        ),
        # ── C: release mutations ─────────────────────────────────────────────
        (
            "an ungated release upload added to the build leg",
            sub(
                "      - uses: actions/upload-artifact@v7\n",
                "      - name: sneak\n        run: gh release upload x y\n"
                "      - uses: actions/upload-artifact@v7\n",
            ),
            "PKG-RELEASE-MUTATION-UNGATED",
        ),
        # ── P: permissions ───────────────────────────────────────────────────
        (
            "the workflow permission block narrowed",
            sub("\npermissions:\n  contents: write\n", "\npermissions:\n  contents: read\n"),
            "PKG-WORKFLOW-PERMISSIONS-CHANGED",
        ),
        (
            "the workflow permission block deleted entirely",
            sub("\npermissions:\n  contents: write\n", "\n"),
            "PKG-WORKFLOW-PERMISSIONS-GONE",
        ),
        (
            # ⚠ The other half of the disarm pair: this changes what a job may do
            # without touching the workflow block the clause above reads.
            "a job overrides the workflow permissions",
            sub(builder_anchor, builder_anchor + "    permissions:\n      contents: read\n"),
            "PKG-JOB-OVERRIDES-PERMISSIONS",
        ),
        # ── U: the release path's identity ───────────────────────────────────
        (
            "the build leg's App token step deleted",
            sub(
                "      - name: Generate GitHub App token\n"
                "        id: app-token\n"
                "        uses: actions/create-github-app-token@v3\n"
                "        with:\n"
                "          client-id: ${{ secrets.DSS_PUBLIC_BOTS_APP_ID }}\n"
                "          private-key: ${{ secrets.DSS_PUBLIC_BOTS_APP_PRIVATE_KEY }}\n"
                "          owner: ${{ github.repository_owner }}\n\n"
                "      - uses: actions/checkout@v6\n"
                "        with:\n"
                "          token: ${{ steps.app-token.outputs.token }}\n",
                "      - uses: actions/checkout@v6\n"
                "        with:\n"
                "          token: ${{ steps.app-token.outputs.token }}\n",
            ),
            # ⚠ ASSERTS THE SPECIFIC ID, not the sequence one this also trips: an
            # arm that settled for the broadest refusal would leave
            # PKG-APP-TOKEN-STEP-GONE unproved and free to be dead code.
            "PKG-APP-TOKEN-STEP-GONE",
        ),
        (
            # ⚠ The disarm the existence half cannot see: the step is still there.
            "the App token step gated off",
            sub(
                "      - name: Generate GitHub App token\n        id: app-token\n",
                "      - name: Generate GitHub App token\n        id: app-token\n"
                "        if: ${{ github.actor != 'nobody' }}\n",
                1,
            ),
            "PKG-APP-TOKEN-STEP-GATED",
        ),
        (
            "the build leg's checkout loses the App token",
            sub(
                "      - uses: actions/checkout@v6\n        with:\n          token: ${{ steps.app-token.outputs.token }}\n\n"
                "      - name: Configure Git for private dependencies\n",
                "      - uses: actions/checkout@v6\n\n      - name: Configure Git for private dependencies\n",
            ),
            "PKG-BUILDER-CHECKOUT-UNTOKENED",
        ),
        (
            "the insteadOf rewrite gated off",
            sub(
                "      - name: Configure Git for private dependencies\n",
                "      - name: Configure Git for private dependencies\n        if: ${{ github.actor != 'nobody' }}\n",
            ),
            "PKG-BUILDER-INSTEADOF-GONE",
        ),
        # ── E / N / G / H ────────────────────────────────────────────────────
        (
            "packaging body forked into a second job",
            sub(
                builder_anchor,
                "  forked-packager:\n    runs-on: ubuntu-latest\n    steps:\n"
                "      - name: copy\n        run: ( cd stage && tar -czf a.tar.gz . )\n" + builder_anchor,
            ),
            "PKG-PACKAGING-FORKED",
        ),
        (
            "the four legs given one artifact name",
            sub(
                "          name: ${{ matrix.name }}-${{ needs.resolve-version.outputs.version }}",
                "          name: dsscp-${{ needs.resolve-version.outputs.version }}",
            ),
            "PKG-ARTIFACT-NAME-NOT-PER-LEG",
        ),
        (
            "empty-archive refusal deleted",
            sub("Package produced an archive with no files", "Package looks fine"),
            "PKG-EMPTY-ARCHIVE-CHECK-GONE",
        ),
        (
            "config-tree refusal deleted",
            sub("the staged install set carries no dss-config tree at all", "no config tree, carrying on"),
            "PKG-CONFIG-TREE-CHECK-GONE",
        ),
        (
            "the tag probe stops failing closed",
            sub("Could not verify whether $tag exists (treating as fatal)", "Assuming $tag is free"),
            "PKG-TAG-PROBE-NOT-FAIL-CLOSED",
        ),
        (
            "the publish step deleted",
            sub('gh release edit "$REL_TAG" --draft=false', "echo would publish"),
            "PKG-PUBLISH-STEP-GONE",
        ),
        (
            "the macOS leg deleted",
            sub('"platform":"macos-arm64"', '"platform":"macos-x86"'),
            "PKG-PLATFORM-LEG-GONE",
        ),
        (
            "the artifact upload deleted",
            sub(upload_block, ""),
            "PKG-ARTIFACT-UPLOAD-GONE",
        ),
        # ── D: the durability invariants ─────────────────────────────────────
        (
            # ★ THE ARM THIS CLAUSE EXISTS FOR. Note it also trips clause R --
            # but ONLY because RELEASE_PATH above was updated to the new order.
            # This arm asserts the D id, so it still fires for a reverter who
            # edits both files, which is the case R cannot see.
            "the durable copy put back AFTER the release upload",
            sub(upload_block, "").replace(attach_run, attach_run + "\n" + upload_block, 1),
            "PKG-DURABLE-COPY-NOT-FIRST",
        ),
        (
            # ⚠ A NON-MODE condition, deliberately: PKG-MODE-GATE-ADDED only
            # looks for `outputs.mode`, so this is the disarm it cannot see.
            "the durable copy gated on something other than the mode",
            sub(
                "      - uses: actions/upload-artifact@v7\n        with:\n",
                "      - uses: actions/upload-artifact@v7\n"
                "        if: ${{ github.actor != 'nobody' }}\n        with:\n",
            ),
            "PKG-ARTIFACT-UPLOAD-GATED",
        ),
        (
            "if-no-files-found downgraded to the action's fail-open default",
            sub("          if-no-files-found: error\n", "          if-no-files-found: warn\n"),
            "PKG-ARTIFACT-UPLOAD-NO-FILES-NOT-ERROR",
        ),
        (
            # ⚠ THE JOB LEVEL, which is the half a per-step reading would miss.
            "the build job made continue-on-error",
            sub(builder_anchor, builder_anchor + "    continue-on-error: true\n"),
            "PKG-CONTINUE-ON-ERROR",
        ),
        (
            "the release upload made continue-on-error",
            sub(attach_run, "        continue-on-error: true\n" + attach_run),
            "PKG-CONTINUE-ON-ERROR",
        ),
        (
            "the asset probe stops reading the assets",
            sub("--json assets", "--json name"),
            "PKG-ASSET-SET-NOT-VERIFIED",
        ),
        (
            "the release published before the asset set is checked",
            sub(result_gate, '          gh release edit "$REL_TAG" --draft=false\n' + result_gate),
            "PKG-ASSET-SET-CHECK-AFTER-PUBLISH",
        ),
        (
            "the asset probe stops failing closed",
            sub(
                "Could not read $REL_TAG's asset list to verify it (treating as fatal)",
                "No assets found, carrying on",
            ),
            "PKG-ASSET-SET-PROBE-NOT-FAIL-CLOSED",
        ),
        (
            # ⚠ THE MUTATION THAT KEEPS THE ASSERTION'S SHAPE. Every other
            # asset-set clause stays green here: the probe runs, the comparison
            # runs, it fails closed -- and it agrees with a four-asset release
            # forever, including after a fifth leg is added.
            "the expected count hard-coded instead of read from the matrix",
            sub(
                "          LEG_COUNT: ${{ needs.build-matrix.outputs.leg-count }}\n",
                "          LEG_COUNT: 4\n",
            ),
            "PKG-LEG-COUNT-NOT-FROM-THE-MATRIX",
        ),
        (
            "build-matrix stops publishing the leg count",
            sub("      leg-count: ${{ steps.set.outputs.leg_count }}\n", ""),
            "PKG-LEG-COUNT-NOT-PUBLISHED",
        ),
        # ── S: the reader's own integrity ────────────────────────────────────
        (
            "the jobs mapping renamed out from under the reader",
            sub("\njobs:\n", "\nstages:\n"),
            "PKG-SHAPE-NO-JOBS-MAPPING",
        ),
        (
            "a job loses its runs-on",
            sub("  build-matrix:\n    runs-on: ${{ 'ubuntu-latest' }}\n", "  build-matrix:\n"),
            "PKG-SHAPE-JOB-WITHOUT-RUNS-ON",
        ),
        (
            "a step this reader cannot key on",
            sub("      - id: set\n", "      - env:\n          UNUSED: '1'\n"),
            "PKG-SHAPE-UNREADABLE-STEP",
        ),
    ]


_ID = re.compile(r"\b(PKG-[A-Z0-9]+(?:-[A-Z0-9]+)+)\b")


def unarmed_clauses(arms: list[tuple[str, str, str]]) -> list[str]:
    """Every message id this file can EMIT must have an arm that asserts it.

    ★ THE RATCHET FOR THE CLAUSE NOBODY PROVED. `PKG-JOB-MODE-GATE-REMOVED` was
    written, was reachable, and had no arm -- its realistic mutation also tripped
    a byte-pin, and the arm settled for that broader refusal. A clause with no arm
    is a clause nothing has ever seen fire, which is the same thing as dead code
    with a comment claiming otherwise. This reads THIS FILE'S OWN SOURCE, so the
    next clause added without an arm is a refusal rather than a silence.

    An id counts as EMITTED where it is followed by `: ` inside a message string,
    and as ARMED where it stands alone as an arm's expectation. Prose mentions
    (`... which PKG-X is blind to`) carry no colon and so are neither.
    """
    src = Path(__file__).read_text(encoding="utf-8")
    emitted = {m for m in _ID.findall(src) if f"{m}: " in src}
    armed = {expect for _, _, expect in arms}
    return sorted(emitted - armed)


def selftest(text: str) -> int:
    failures = 0
    arms = _arms(text)
    unarmed = unarmed_clauses(arms)
    if unarmed:
        print(f"    NO ARM PROVES {len(unarmed)} clause(s): {unarmed}")
        failures += len(unarmed)
    print(f"  self-test: {len(arms)} arm(s), each asserting its own message id")
    for name, mutated, expect in arms:
        found = check(mutated)
        if any(expect in m for m in found):
            print(f"    RED as required  [{expect}]  {name}")
        else:
            failures += 1
            print(f"    ARM DID NOT FIRE [{expect}]  {name}")
            for m in found:
                print(f"        instead got: {m.split(':', 1)[0]}")
    return failures


def main() -> int:
    if not WORKFLOW.is_file():
        print(f"FAIL: {WORKFLOW.as_posix()} not found (run from the repository root).")
        return 2
    text = WORKFLOW.read_text(encoding="utf-8")

    print(f"check-pkg-pipeline: {WORKFLOW.as_posix()}")
    print(f"  release path pinned at {len(RELEASE_PATH)} step(s) across {len(JOB_ORDER)} job(s)")
    problems = check(text)
    if problems:
        for p in problems:
            print(f"  FAIL: {p}")
    else:
        print("  CONTROL arm (the real, unmutated file): 0 refusals")

    failures = selftest(text)
    if problems or failures:
        print(f"REFUSED: {len(problems)} tree problem(s), {failures} self-test arm(s) that did not fire.")
        return 1
    print("OK (the release path is pinned, the modes are separated, and every clause proved it can fail)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
