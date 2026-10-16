#!/usr/bin/env python3
# PURPOSE: refuse an action that no index documents, an index entry that no action backs, and an action no runner can start.
"""check-scripts-index.py -- the ACTION INDEX guard.

★★★ WHY THIS EXISTS, and it is a measured gap rather than a tidiness rule.
Until 2026-08-19 this repository had eighteen scripts and NO index of them: no
`README` in the script root, and nothing in the `/dss-cycle` skill that said what
existed. They were named only piecemeal, across eight different reference files,
wherever some gate step happened to need one. ✔The cost is the ordinary one and
it had already been paid: work gets redone because the tool that already does it
is invisible, and a tool with a defect gets worked around instead of repaired,
because the reader does not know it is a shared tool at all.

★★ THE OPERATOR'S RULE THIS GUARD ENFORCES (2026-08-19): *"if a tool has a
problem, fix before using again, not workaround an own tool. reusable tools
exists to avoid bunch of problems like mangling or edge cases."* An index is how
that rule becomes reachable -- you cannot be told to prefer the existing tool if
nothing lists the existing tools.

★★★ 2026-09-18: THE SUBJECT IS DSSHARNESS'S ACTIONS DIRECTORY, NOT `scripts/`.
The operator's ruling finished the harness migration: *"there is no `scripts/`
directory and no `real-examples/` directory"* -- every program this repository
ships is an ACTION under `.harness-config/runner/actions/`, one directory per
program holding its `<name>.yml` and every file it runs, and `real-examples/c/sqlite`
keeps its shape as a GROUPED action. The guard keeps its name because a program
keeps its name; what it indexes is now defined by the TOOL rather than by this
file: an action is a directory holding a file of its own name with `.yml` or
`.yaml` (`dssharness help runners`), and every other directory under the actions
root is a GROUP of actions, recursed into.

★★★ WHY THE INDEX IS GENERATED AND NOT WRITTEN. Two documents describe the same
set from two audiences (the repository's own index beside the actions, and the
cycle-facing reference the `/dss-cycle` skill reads). A hand-written pair is two
copies of one fact, and this project has already measured what that costs -- the
whole `_deferred-anchor-registry` discipline exists because a second copy goes
stale silently. So the fact lives in each action's own `PURPOSE:` line, and both
documents are generated from it and verified against it here.
⚠ The declaration is a COMMENT line in the action file (the mark, then the
sentence), because DssHarness refuses an unknown key in an action file
(`ActionFileParser.FileKeys`); a comment is the one place the file can say it
without the tool refusing it.

THE CONTRACT, and every clause is a way the index can lie:
  1. every directory under the actions root is an ACTION (it holds
     `<dir>/<dir>.yml` or `.yaml`) or a GROUP of actions -- a group directly holds
     only directories and the index's own `README.md` / the tool's `.gitkeep`, and
     at least one action somewhere below it;
  2. an action's file declares EXACTLY ONE `PURPOSE:` line in its header;
  3. both indexes list exactly the actions that exist -- no missing entry (an
     action nothing documents) and no surplus entry (a document promising an
     action that was deleted);
  4. each entry's purpose text matches the action's own declaration exactly,
     after trimming the whitespace around it, so editing a purpose without
     updating the indexes is a red rather than a silent divergence;
  5. a declaration is non-empty, carries no raw `|` (it lands in a markdown
     cell) and no index marker (it lands between them);
  6. a PROGRAM beside the action file may repeat the declaration but may not
     CONTRADICT it;
  7. the layout holds: no program loose in a group directory, none buried in a
     subdirectory of its action, no action inside another -- each is invisible
     to an index keyed on action directories (the last one the tool refuses too);
  8. each document carries EXACTLY ONE marker pair, so a second, unverified
     index cannot sit below the real one;
  9. the scan has a FLOOR. A guard whose enumeration collapses to nothing
     reports a clean pass over a corpus it never read -- the exact failure this
     repository has measured more than once -- so too few actions is a refusal;
 10. every action is REACHABLE: `predefinedRunners` in `.harness-config/config.json`
     names it, and every runner's `action` names an action that exists.
     ✔MEASURED 2026-09-17: two actions shipped with their directories and WITHOUT
     their runner entries, so neither could be started -- `dssharness run` takes a
     runner "as predefinedRunners names it" and nothing else. The tool checks the
     other direction (a runner's action must resolve) only when a runner is used;
 11. no `.sh` and no `.ps1` sits anywhere under the actions root: an action's entry point
     is its `.yml`, and what the `.yml` starts is Python (operator ruling 2026-09-21, "they
     are specific per OS"); the tool's run directories are ignored output and exempt;
 12. every PROGRAM (a file with a `__main__` guard) that loads another program -- by path,
     through `sys.path`, or by importing a module beside it -- switches bytecode writing off
     at MODULE level before its first load: a `__pycache__` written beside another action
     moves the inputs of a `requireInputsUnmoved` action (see `bytecode_refusal`).

⚠ Clauses 5-8 exist because an INDEPENDENT AUDIT got the first draft of this
guard to report GREEN over each of them, and clause 9's arm was passing for the
wrong reason. Every refusal now has a self-test arm that asserts the MESSAGE, and
each was verified by sabotage: delete the refusal, and the self-test fails.

Exit codes: 0 OK · 1 index disagrees with the tree, or an action is unreachable ·
2 the scan collapsed (structural failure: fix the scan, never lower the floor) ·
3 usage error.

Usage -- each a step of the action, the program's own flag after the `#`:
    dssharness run check-scripts-index                           # verify, then the self-test
    dssharness run check-scripts-index-write                     # --write: regenerate, this machine's tree
    dssharness run check-scripts-index --manual-step self-test   # --selftest: prove it fails
"""
from __future__ import annotations

import ast
import contextlib
import functools
import importlib.util
import io
import json
import os
import re
import shutil
import sys
import tempfile
sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (the rule: check-scripts-index)

# ── OUTPUT ENCODING — NOT COSMETIC, AND THE STREAM IS HALF THE FACT ─────────────
# ✔MEASURED 2026-08-23 (CPython 3.14.3, Windows, BOTH streams PIPES, which is
# exactly how ctest runs every guard): `sys.stdout` comes up
# `encoding='cp1252' errors='surrogateescape'` and `sys.stderr` comes up
# `errors='backslashreplace'`. `surrogateescape` rescues only lone surrogates left
# by an earlier decode; it does NOTHING for an ordinary unencodable character. So a
# report printed on STDOUT — where this guard names every index document that
# disagrees with the tree —
# raises `UnicodeEncodeError` and kills the guard INSIDE ITS OWN REPORT: the run
# still reds, but the finding is lost and the traceback names a `print` rather than
# the thing that was wrong. STDERR merely mangles the glyph into an escape.
# ⚠ Paths are ASCII in this tree TODAY, which makes this prophylactic rather than a
# live red — and one non-ASCII action name away from not being.
# Applied at IMPORT, so every path this module can print on is covered.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):   # pragma: no cover - odd stream
        pass


# DssHarness's layout (`dssharness help layout`): where actions live and where the
# runners that start them are declared, relative to a tree root.
ACTIONS_REL = os.path.join(".harness-config", "runner", "actions")
CONFIG_REL = os.path.join(".harness-config", "config.json")

# The two documents. Both are generated between the markers and hand-written
# outside them, so the prose that explains the index is never machine-owned.
README_REL = os.path.join(ACTIONS_REL, "README.md")
SKILL_REL = os.path.join(".claude", "skills", "dss-cycle", "references", "actions.md")
DOC_RELS = (README_REL, SKILL_REL)

BEGIN = "<!-- BEGIN GENERATED ACTION INDEX -->"
END = "<!-- END GENERATED ACTION INDEX -->"

PURPOSE_MARK = "PURPOSE: "
HEADER_LINES = 40  # a declaration further down than this is not a header

# ★ THE DECLARATION BEGINS ITS LINE: optional indentation, an optional comment leader
# (`#`, `//`), then the mark. It used to be the mark ANYWHERE on a header line, which
# read ordinary prose as a declaration the first time the index met files that were
# not written for it: ✔MEASURED 2026-09-18, the sqlite harness's `test-confound-scope.sh`
# says "…a `local` prefix ON PURPOSE: if someone reintroduces…" in its header, and the
# guard collapsed on it as a program CONTRADICTING its action. Every real declaration in
# the tree begins its line (84 of 84 ✔MEASURED; `lane-worktree.ps1`'s sits bare inside a
# `<# … #>` block). Self-test arm 14c pins the prose case.
_DECLARATION = re.compile(r"^\s*(?:#+|//)?\s*" + re.escape(PURPOSE_MARK) + r"(.*)$")


def declarations(text):
    """Every `PURPOSE:` declaration in the header of `text`, stripped."""
    out = []
    for line in text.split("\n")[:HEADER_LINES]:
        m = _DECLARATION.match(line.rstrip("\r"))
        if m:
            out.append(m.group(1).strip())
    return out

# Far below the live figure (43 actions on 2026-09-18) so ordinary churn never trips
# it, and high enough that a collapsed enumeration cannot masquerade as a pass.
# ⓘ It was `SCRIPT_FLOOR = 12` over `scripts/` (52 then 42 directories); the NUMBER is
# unchanged because the subject was retargeted, not shrunk -- every program moved.
ACTION_FLOOR = 12

# What an action's PROGRAM is: Python, and only Python.
# ★★★ OPERATOR RULING 2026-09-21: *"I don't want .sh/.ps1 files inside
# .harness-config\runner\actions. entrypoint is .yml, you can call .py files, BUT NOT .sh/.ps1
# please. They are specific per OS."* Every such file was ported and deleted the same day
# (lane mig, part 4), and `FORBIDDEN_EXTS` is what keeps the rule true: a `.sh` or `.ps1`
# ANYWHERE under the actions root is refused by name (clause 11), so the ruling cannot decay
# into a convention the next migration forgets.
SCRIPT_EXTS = (".py",)
FORBIDDEN_EXTS = (".sh", ".ps1")

# An action's own file, in the order `dssharness help runners` names the spellings.
ACTION_EXTS = (".yml", ".yaml")

# The two directory names DssHarness gives every action for a run's working space and
# kept output, at whatever depth it is grouped. Both are gitignored run output, never
# tracked (`dssharness help runners`), so nothing in one is a program of this repository.
RUN_DIRS = ("build", "artifacts")

# What a GROUP directory may hold directly besides directories: this index's own
# document at the actions root, and the placeholder `dssharness init` writes there.
GROUP_FILES = ("README.md", ".gitkeep")

EXIT_OK, EXIT_DISAGREE, EXIT_COLLAPSE, EXIT_USAGE = 0, 1, 2, 3
# The regeneration a refusal names: the `write` manual step of this action (check-scripts-index.yml),
# run through the harness -- never this file started by hand -- on the runner that runs it on ONE leg,
# this machine's own tree (config.json `check-scripts-index-write`): this action's own runner keeps both
# local legs, and a `--manual-step write` through it would rewrite the WSL leg's synced copy too (P68
# round 13's audit, F1-A11).
WRITE_VERB = "dssharness run check-scripts-index-write"


class Collapse(Exception):
    """The enumeration failed structurally. Never reported as a clean pass."""


class Entry:
    """One action. `primary` is its entry-point PROGRAM, `<name>.py`, or None when the
    action ships none of that name; `action_file` is the file that makes it an action and
    carries its `PURPOSE:`."""

    __slots__ = ("name", "rel", "action_file", "primary", "siblings", "purpose")

    def __init__(self, name, rel, action_file, primary, siblings, purpose):
        self.name = name
        self.rel = rel
        self.action_file = action_file
        self.primary = primary
        self.siblings = siblings
        self.purpose = purpose


_OWNING_TREE = None


def _owning_tree():
    """`.harness-config/runner/actions/owning-tree/owning-tree.py` -- the one owner of
    "which tree is this file in?".

    Loaded by path from this file's sibling directory (a hyphen is not a module name). It
    FAILS LOUD when absent rather than falling back to a local walk: a second copy of the
    answer is the drift that owner exists to end.
    """
    global _OWNING_TREE
    if _OWNING_TREE is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))),
                            "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            raise Collapse("cannot find %s -- this guard's root is resolved there and "
                           "nowhere else" % path)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _OWNING_TREE = mod
    return _OWNING_TREE


def repo_root():
    """The tree THIS FILE lives in -- never the tree the caller's shell is standing in.

    ★ Still NOT derived by counting `..`: this file's own depth under the repo is the
    fact that changed twice -- on 2026-08-19 when `tools/` was merged into `scripts/`,
    and on 2026-09-18 when every program moved into `.harness-config/runner/actions/`
    -- and every guard that counted had to be edited by hand. The walk in
    `.harness-config/runner/actions/owning-tree/owning-tree.py` does not count either.
    ⚠ What it replaces -- a bare `git rev-parse --show-toplevel` -- answered from the
    CALLER's cwd. ✔MEASURED 2026-09-15 (P66): run by path with its cwd inside a different
    repository it indexed THAT repository and collapsed on a directory only that repository
    lacks; from a directory inside no repository it would not run at all. ctest pins
    `WORKING_DIRECTORY`, so no gate saw either.
    """
    ot = _owning_tree()
    try:
        return ot.resolve(__file__)
    except ot.Refusal as exc:
        raise Collapse(str(exc))


def action_file(directory):
    """`<dir>/<dir>.yml` or `.yaml` when `directory` is an action, else None.

    The TOOL's definition (`dssharness help runners`: "a directory holding a file of its
    own name is an action"), not this guard's, so the two cannot disagree about what an
    action is.
    """
    name = os.path.basename(os.path.normpath(directory))
    for ext in ACTION_EXTS:
        cand = os.path.join(directory, name + ext)
        if os.path.isfile(cand):
            return cand
    return None


def primary_script(action_dir, name):
    """`<dir>/<dir>.py` -- ONE rule, not a lookup table.

    A per-action table would be a third place to forget to update, which is the
    class of defect this whole guard exists to close. (It preferred `.sh`, then `.py`,
    then `.ps1` until 2026-09-21, when no action may ship the other two.)
    """
    cand = os.path.join(action_dir, name + ".py")
    return cand if os.path.isfile(cand) else None


def forbidden_programs(base):
    """Every `.sh`/`.ps1` under the actions root `base`, actions-relative, sorted -- the
    tool's run directories and bytecode caches excepted (ignored output, never the tree's)."""
    found = []
    for dirpath, dirs, files in os.walk(base):
        dirs[:] = sorted(d for d in dirs if d != "__pycache__" and d.lower() not in RUN_DIRS)
        for f in sorted(files):
            if os.path.splitext(f)[1].lower() in FORBIDDEN_EXTS:
                found.append(_rel(base, os.path.join(dirpath, f)))
    return found


def bytecode_husk(path):
    """True when `path` holds nothing the tree could: only Python's own bytecode cache, or nothing.

    ★★ WHY THIS IS NOT A LOOSENING. Deleting a Python program deletes the files git
    tracks; the `__pycache__/<name>.cpython-XY.pyc` Python wrote beside it when it was
    last imported is ignored, so neither git nor the harness's sync removes it, and the
    directory SURVIVES holding only that. ✔MEASURED 2026-09-18, eight-leg gate: the WSL
    leg host's copy still held a deleted program's directory, and both this guard and
    `check-guard-output-encoding` COLLAPSED on it on BOTH WSL legs, while Windows --
    where the program had never been imported -- passed. A directory holding only a
    bytecode cache is a byproduct of a program that no longer exists.
    ⚠ The exemption is deliberately NARROW: a single file that is not a `.pyc` inside a
    `__pycache__` directory disqualifies it, so a real directory that lost its action
    still refuses -- self-test arms `11b` and `11c` pin both directions.
    """
    for dirpath, _dirs, files in os.walk(path):
        in_cache = os.path.basename(dirpath) == "__pycache__"
        for f in files:
            if not (in_cache and f.endswith(".pyc")):
                return False
    return True


# ★★★ CLAUSE 12: A PROGRAM THAT LOADS ANOTHER PROGRAM SWITCHES BYTECODE WRITING OFF FIRST, AT MODULE LEVEL.
# Python writes `__pycache__` beside every module it loads by path or imports, unless
# `sys.dont_write_bytecode` is already set -- and such a cache lands in ANOTHER action's directory (or in
# the program's own), while an action is `requireInputsUnmoved`. ✔MEASURED 2026-09-23 (lane mig): 34 of the
# 44 programs that loaded another program never switched it off, and one repo-guard run left `__pycache__`
# beside anchors/, burndown-queue/ and check-anchor-balance/.
# ⚠ WHY NOT ONE LOADER THAT SWITCHES IT OFF, which was tried on paper first: the flag is process-global and
# must precede the process's FIRST load, which in most programs is the bootstrap load of owning-tree itself
# -- a loader cannot load itself -- and which load comes first differs by entry point. ✔MEASURED the same
# day: `apply-registry-row --self-test` loads anchors.py before owning-tree, and wrote
# `anchors/__pycache__/anchors.cpython-314.pyc` (18:50:20) although its owning-tree loader comes first in
# the file. No static check could have proved a loader-based owner.
# ⇒ Each program switches it off at MODULE level, textually before its first load construct. That is
# EXACT: module-level statements run in order, and a load inside a function runs only after the function's
# definition has run. A LIBRARY (no `__main__` guard) is judged by the program that imports it.
# ★★ AND THE SAME RULE ACROSS A PROCESS BOUNDARY (12b). The switch reaches only its own process. ✔MEASURED
# 2026-09-23 (lane mig), after the 34 switches landed: holding every existing cache aside and running
# repo-guard plus the harness entries left exactly ONE new one, `check-no-abort-in-tests/__pycache__/` -- its
# pipe-encoding self-test loads ITS OWN FILE in a `python -c` child, which writes bytecode unless its own
# argv says `-B`. So a child told to load a file of this action's directory -- its inline code, or an
# argument after it, built from `__file__` -- passes `-B` before `-c`, or hands the child an environment
# that moves the cache (PYTHONPYCACHEPREFIX, as check-plan-citations' own bytecode-ON control does), or
# switches it off inside the code. This half binds a LIBRARY too: whoever spawns the child owns it.
# ✔MEASURED the same day over all 67 `.py` files under the actions root: 55 argv displays carry `-c`, and
# the rule refused exactly two -- that self-load, and test-corpus-census' child, which loads a COPY whose
# SOURCE path is built from `__file__`. The second is this rule's deliberate over-approximation (it cannot
# tell a copy's source from its destination) and costs one harmless `-B`; the opposite choice would fail
# toward clean.
# ⓘ OUT OF SCOPE BY CONSTRUCTION: a child loading a COPY in a temporary directory with no `__file__` in its
# path (the deliberate bytecode-ON negatives, e.g. check-guard-output-encoding's) writes its cache there.
LOAD_CALLS = ("spec_from_file_location", "SourceFileLoader", "import_module", "exec_module", "run_path")
CHILD_LOAD_MARKS = LOAD_CALLS + ("sys.path.insert", "sys.path.append")
BYTECODE_ENV = ("PYTHONPYCACHEPREFIX", "PYTHONDONTWRITEBYTECODE")
BINDING_DEPTH = 4


def _is_bytecode_off(node):
    """`sys.dont_write_bytecode = True`, as a statement."""
    return (isinstance(node, ast.Assign) and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Attribute)
            and node.targets[0].attr == "dont_write_bytecode"
            and isinstance(node.targets[0].value, ast.Name) and node.targets[0].value.id == "sys"
            and isinstance(node.value, ast.Constant) and node.value.value is True)


def first_load(tree, beside):
    """-> (line, what) of the first construct in `tree` that loads another program, or None: a
    by-path load (`LOAD_CALLS`), a `sys.path.insert/append`, or an import of a module that sits
    beside the program (`beside`: those modules' names)."""
    found = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            f = node.func
            name = f.attr if isinstance(f, ast.Attribute) else f.id if isinstance(f, ast.Name) else ""
            if name in LOAD_CALLS:
                found.append((node.lineno, name))
            elif (name in ("insert", "append") and isinstance(f, ast.Attribute)
                  and isinstance(f.value, ast.Attribute) and f.value.attr == "path"
                  and isinstance(f.value.value, ast.Name) and f.value.value.id == "sys"):
                found.append((node.lineno, "sys.path." + name))
        elif isinstance(node, ast.Import):
            hits = [a.name for a in node.names if a.name.split(".")[0] in beside]
            if hits:
                found.append((node.lineno, "import " + hits[0]))
        elif isinstance(node, ast.ImportFrom):
            if node.level == 0 and node.module and node.module.split(".")[0] in beside:
                found.append((node.lineno, "from %s import" % node.module))
    return min(found) if found else None


def _const_str(node):
    """A string constant's value, else None (an `ast.Index` wrapper is unwrapped for old Pythons)."""
    node = getattr(node, "value", node) if type(node).__name__ == "Index" else node
    return node.value if isinstance(node, ast.Constant) and isinstance(node.value, str) else None


def _dict_keys(node):
    """The string keys a `dict(...)` call or a `{...}` display gives -- the environment it builds."""
    keys = set()
    if isinstance(node, ast.Dict):
        keys.update(k for k in (_const_str(k) for k in node.keys if k is not None) if k)
    elif isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "dict":
        keys.update(kw.arg for kw in node.keywords if kw.arg)
        for arg in node.args:
            keys |= _dict_keys(arg)
    return keys


def _scope_facts(node):
    """-> (bindings, env_keys) of one scope: `name -> [value expressions]` for plain and annotated
    assignments, and `name -> {keys}` stored into it (`env["K"] = v`, `env.update(K=v)`, or given by the
    `dict(...)`/`{...}` it was bound to). A function's scope is its whole body; the module's is every
    statement outside its functions."""
    if isinstance(node, ast.Module):
        nodes, stack = [], list(node.body)
        while stack:
            n = stack.pop()
            nodes.append(n)
            stack.extend(c for c in ast.iter_child_nodes(n)
                         if not isinstance(c, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)))
    else:
        nodes = list(ast.walk(node))
    bindings, env_keys = {}, {}
    for n in nodes:
        if isinstance(n, (ast.Assign, ast.AnnAssign)) and n.value is not None:
            for target in (n.targets if isinstance(n, ast.Assign) else [n.target]):
                if isinstance(target, ast.Name):
                    bindings.setdefault(target.id, []).append(n.value)
                    env_keys.setdefault(target.id, set()).update(_dict_keys(n.value))
                elif isinstance(target, ast.Subscript) and isinstance(target.value, ast.Name):
                    key = _const_str(target.slice)
                    if key:
                        env_keys.setdefault(target.value.id, set()).add(key)
        elif (isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr == "update"
              and isinstance(n.func.value, ast.Name)):
            keys = {kw.arg for kw in n.keywords if kw.arg}
            for arg in n.args:
                keys |= _dict_keys(arg)
            env_keys.setdefault(n.func.value.id, set()).update(keys)
    return bindings, env_keys


def _lookup(name, scopes):
    """The value expressions `name` is bound to in the innermost scope that binds it."""
    for bindings, _env in scopes:
        if name in bindings:
            return bindings[name]
    return []


def _derives_from_file(expr, scopes, depth=0):
    """Is `expr` built from `__file__`, directly or through names bound to such expressions? Bounded by
    BINDING_DEPTH, so a self-referencing binding (`s = s + "..."`) cannot loop."""
    for n in ast.walk(expr):
        if isinstance(n, ast.Name):
            if n.id == "__file__":
                return True
            if depth < BINDING_DEPTH and any(_derives_from_file(v, scopes, depth + 1)
                                              for v in _lookup(n.id, scopes)):
                return True
    return False


def _code_text(expr, scopes, depth=0):
    """Every string constant `expr` is built from, following names bound to other expressions."""
    parts = []
    for n in ast.walk(expr):
        if isinstance(n, ast.Constant) and isinstance(n.value, str):
            parts.append(n.value)
        elif isinstance(n, ast.Name) and depth < BINDING_DEPTH:
            parts.extend(_code_text(v, scopes, depth + 1) for v in _lookup(n.id, scopes))
    return "\n".join(p for p in parts if p)


def child_bytecode_refusals(tree, name):
    """Clause 12b: every argv display in `tree` that runs inline Python (`-c`) told to load a file of this
    action's own directory -- the code, or an argument after it, built from `__file__` -- with bytecode
    writing on. Inline code whose text cannot be read at all is judged as loading: this clause cannot tell
    what it runs, so it does not assume it runs nothing."""
    parents = {}
    for p in ast.walk(tree):
        for c in ast.iter_child_nodes(p):
            parents[c] = p
    facts = {}

    def scopes_of(node):
        chain, cur = [], node
        while cur is not None:
            if isinstance(cur, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Module)):
                if cur not in facts:
                    facts[cur] = _scope_facts(cur)
                chain.append(facts[cur])
            cur = parents.get(cur)
        return chain

    def env_moves_the_cache(node, scopes):
        cur = parents.get(node)
        while cur is not None and not isinstance(cur, ast.stmt):
            if isinstance(cur, ast.Call):
                for kw in cur.keywords:
                    if kw.arg != "env":
                        continue
                    keys = set(_dict_keys(kw.value))
                    if isinstance(kw.value, ast.Name):
                        for _bindings, env in scopes:
                            keys |= env.get(kw.value.id, set())
                    if keys & set(BYTECODE_ENV):
                        return True
            cur = parents.get(cur)
        return False

    out = []
    for node in ast.walk(tree):
        if not isinstance(node, (ast.List, ast.Tuple)):
            continue
        elts = node.elts
        at = [i for i, e in enumerate(elts) if _const_str(e) == "-c"]
        if not at or at[0] + 1 >= len(elts):
            continue
        i = at[0]
        scopes = scopes_of(node)
        if not any(_derives_from_file(e, scopes) for e in elts[i + 1:]):
            continue
        text = _code_text(elts[i + 1], scopes)
        if text and not any(mark in text for mark in CHILD_LOAD_MARKS):
            continue
        if "dont_write_bytecode" in text or any(_const_str(e) == "-B" for e in elts[:i]):
            continue
        if env_moves_the_cache(node, scopes):
            continue
        out.append("%s line %d: a child Python (`-c`) loads by path, and its code or an argument after it "
                   "is built from `__file__` -- so what it loads may sit in an action's directory -- with "
                   "bytecode writing ON: the child writes `__pycache__` beside whatever it loads. Pass `-B` "
                   "before `-c`, or hand the child an environment that sets %s."
                   % (name, node.lineno, " or ".join(BYTECODE_ENV)))
    return out


def bytecode_refusal(path):
    """Why the file at `path` would write bytecode beside an action, or None (clause 12): a PROGRAM
    that loads another with the switch still on, and -- in ANY file, a library too -- a child Python
    told to load a file of this action's directory with bytecode writing on (12b). A file that does
    not read or parse is refused by name -- never skipped."""
    name = os.path.basename(path)
    try:
        text = io.open(path, "r", encoding="utf-8", newline="").read()
    except (UnicodeDecodeError, ValueError) as exc:
        return "%s cannot be read, so whether it caches bytecode cannot be told: %s" % (path, exc)
    beside = frozenset(os.path.splitext(f)[0] for f in os.listdir(os.path.dirname(path))
                       if os.path.splitext(f)[1] in SCRIPT_EXTS) - {os.path.splitext(name)[0]}
    return _bytecode_verdict(text, beside, name)


# ★ MEMOIZED ON ITS WHOLE INPUT -- the file's text, the modules beside it, its name -- because the self-test
# re-runs the scan over a mirror whose files are byte-identical to the tree's except the one an arm sabotages.
# ✔MEASURED 2026-09-23: one pass of this clause over the 67 files costs ~4 s (the parse alone ~1.8 s), and the
# self-test makes ~55 passes; unmemoized, 12b took the ctest entry from ~83 s to ~160 s and past its 315 s
# budget under load. A verdict is a pure function of these three inputs, so a changed byte is a cache miss.
@functools.lru_cache(maxsize=None)
def _bytecode_verdict(text, beside, name):
    try:
        tree = ast.parse(text, filename=name)
    except (SyntaxError, ValueError) as exc:
        return "%s cannot be parsed, so whether it caches bytecode cannot be told: %s" % (name, exc)
    whys = []
    if any(isinstance(n, ast.If) and isinstance(n.test, ast.Compare)
           and isinstance(n.test.left, ast.Name) and n.test.left.id == "__name__"
           for n in tree.body):
        load = first_load(tree, beside)
        flags = [n.lineno for n in tree.body if _is_bytecode_off(n)]
        if load is not None and not (flags and flags[0] < load[0]):
            whys.append(
                "%s loads another program at line %d (%s) with bytecode writing ON%s. Python then writes "
                "`__pycache__` beside that program, inside an action's directory. Put "
                "`sys.dont_write_bytecode = True` at MODULE level, before line %d."
                % (name, load[0], load[1],
                   "" if not flags else " (the module-level switch at line %d comes after it)" % flags[0],
                   load[0]))
    whys.extend(child_bytecode_refusals(tree, name))
    return "\n    ".join(whys) if whys else None


def read_purpose(path):
    """The single `PURPOSE:` declaration in a file's header.

    Raises on zero or many: an ambiguous declaration is worse than none, because
    a reader believes the first one they find.
    """
    found = declarations(io.open(path, "r", encoding="utf-8", newline="").read())
    if not found:
        raise Collapse(
            "%s declares no `%s` line in its first %d lines. Every action "
            "declares its purpose once, in its action file's header; both indexes are "
            "generated from that declaration." % (path, PURPOSE_MARK.strip(), HEADER_LINES))
    if len(found) > 1:
        raise Collapse(
            "%s declares %d `%s` lines. Exactly one is required -- a reader "
            "believes the first one they find." % (path, len(found), PURPOSE_MARK.strip()))
    purpose = found[0]

    # THREE REFUSALS ON THE TEXT ITSELF, each one found by audit as a way a
    # declaration can satisfy this guard while telling the reader nothing, or
    # telling the DOCUMENT something it cannot survive.
    if not purpose:
        raise Collapse(
            "%s declares an EMPTY purpose. Declaring nothing is not declaring "
            "a purpose -- the byte-identity check would then hold vacuously "
            "against a blank table cell." % path)
    if "|" in purpose:
        raise Collapse(
            "%s declares a purpose containing a raw pipe: %s. The purpose is "
            "interpolated into a markdown table cell, and a stray pipe makes the "
            "renderer SILENTLY DROP every cell after it. Rewrite the sentence; "
            "escaping it here would only move the surprise into the generated "
            "document." % (path, purpose))
    for _mark in (BEGIN, END):
        if _mark in purpose:
            raise Collapse(
                "%s declares a purpose containing the generated-index marker %s. "
                "Writing it into the body would create a second marker and the "
                "index would never converge." % (path, _mark))
    return purpose


def _rel(root, path):
    return os.path.relpath(path, root).replace(os.sep, "/")


def _scan_action(root, base, directory, afile):
    """One action directory -> an Entry, refusing every layout it cannot index."""
    name = os.path.basename(directory)
    rel = _rel(base, directory)

    # NOTHING BURIED, NOTHING NESTED. A program one level deeper than its action file is
    # as invisible as a loose one; an action inside an action has two owners, which the
    # tool refuses too (`ActionPath.RefuseNesting`). Assets in subdirectories stay fine
    # -- only programs and action files are refused -- and the tool's own run
    # directories are skipped: they are ignored output at any depth.
    for sub_dir, dirs, files in os.walk(directory):
        dirs[:] = [d for d in dirs if d != "__pycache__" and d.lower() not in RUN_DIRS]
        if os.path.abspath(sub_dir) == os.path.abspath(directory):
            continue
        inner = action_file(sub_dir)
        if inner is not None:
            raise Collapse(
                "%s/ holds %s, so an action sits INSIDE the action %s. An action owns its "
                "directory and everything beside its file, so one cannot contain another "
                "-- DssHarness refuses the inner one when a runner names it."
                % (_rel(base, sub_dir), os.path.basename(inner), rel))
        buried = sorted(f for f in files if os.path.splitext(f)[1] in SCRIPT_EXTS)
        if buried:
            raise Collapse(
                "%s/ buries program(s) in a subdirectory: %s. Programs live BESIDE the "
                "action file, not under it -- a buried program is indexed by nothing and "
                "governed by nothing."
                % (rel, ", ".join(
                    os.path.join(os.path.relpath(sub_dir, directory), f).replace(os.sep, "/")
                    for f in buried)))

    purpose = read_purpose(afile)
    sibs = sorted(f for f in os.listdir(directory)
                  if os.path.isfile(os.path.join(directory, f))
                  and os.path.splitext(f)[1] in SCRIPT_EXTS)

    # CLAUSE 12: nothing here loads a program -- in this process or in a child -- with bytecode writing on.
    caching = [why for why in (bytecode_refusal(os.path.join(directory, s)) for s in sibs) if why]
    if caching:
        raise Collapse("%s/: %d file(s) would write bytecode beside an action:\n    %s"
                       % (rel, len(caching), "\n    ".join(caching)))

    # A PROGRAM MAY STAY SILENT, BUT IT MAY NOT DISAGREE. Requiring every program to
    # repeat the declaration would be dozens of copies of one sentence; letting one
    # CONTRADICT the action file would let a capability-paired twin describe two
    # different capabilities. Found by audit: a .ps1 twin declaring the opposite
    # purpose passed.
    for sib in sibs:
        sib_path = os.path.join(directory, sib)
        if not declarations(io.open(sib_path, "r", encoding="utf-8", newline="").read()):
            continue
        declared = read_purpose(sib_path)
        if declared != purpose:
            raise Collapse(
                "%s/%s declares a purpose that differs from its action file (%s).\n"
                "    action  : %s\n    program : %s\n  A program may omit the "
                "declaration; it may not contradict it."
                % (rel, sib, os.path.basename(afile), purpose, declared))

    prim = primary_script(directory, name)
    return Entry(name, rel, _rel(root, afile), None if prim is None else _rel(root, prim),
                 sibs, purpose)


def scan(root):
    """Every action under the actions root, grouped or not, in path order."""
    base = os.path.join(root, ACTIONS_REL)
    if not os.path.isdir(base):
        raise Collapse("no %s directory under %s" % (ACTIONS_REL.replace(os.sep, "/"), root))

    # NO SHELL AND NO POWERSHELL, anywhere under the actions root (operator ruling
    # 2026-09-21): an action's entry point is its `.yml`, and what the `.yml` starts is
    # Python. A `.sh` or `.ps1` is specific to one OS, which is the reason it was ruled out.
    shells = forbidden_programs(base)
    if shells:
        raise Collapse(
            "%d shell/PowerShell program(s) sit under %s: %s. An action ships no .sh and no "
            ".ps1 -- its entry point is its .yml, and what the .yml starts is a .py (operator "
            "ruling 2026-09-21: they are specific per OS). Port the program to Python beside "
            "its action file."
            % (len(shells), ACTIONS_REL.replace(os.sep, "/"), ", ".join(shells)))

    entries = []

    def group(directory):
        rel = _rel(base, directory) if os.path.abspath(directory) != os.path.abspath(base) \
            else "."
        # A PROGRAM THAT IS NOT IN AN ACTION OF ITS OWN IS INVISIBLE TO AN INDEX KEYED
        # ON ACTIONS -- so it is refused, not skipped. Found by audit under the old
        # layout: a loose script passed while documented nowhere. A migration is when
        # a stray file gets left at the top.
        loose = sorted(f for f in os.listdir(directory)
                       if os.path.isfile(os.path.join(directory, f))
                       and f not in GROUP_FILES)
        if loose:
            programs = [f for f in loose if os.path.splitext(f)[1] in SCRIPT_EXTS]
            raise Collapse(
                "file(s) sit directly in the group directory %s instead of in an action "
                "of their own: %s. The layout is one directory per action "
                "(<name>/<name>.yml beside every file it runs); a loose %s is owned by "
                "no action and indexed by nothing."
                % (rel, ", ".join(loose), "program" if programs else "file"))
        found = 0
        for n in sorted(os.listdir(directory)):
            sub = os.path.join(directory, n)
            if not os.path.isdir(sub) or n == "__pycache__" or bytecode_husk(sub):
                continue
            if n.lower() in RUN_DIRS:
                raise Collapse(
                    "%s/%s/ is a directory called '%s' where actions are grouped. Every "
                    "action already owns one of that name for a run's output, which is "
                    "gitignored, so DssHarness refuses the name anywhere along an action "
                    "path." % (rel, n, n))
            afile = action_file(sub)
            if afile is not None:
                entries.append(_scan_action(root, base, sub, afile))
                found += 1
                continue
            if primary_script(sub, n) is not None:
                raise Collapse(
                    "%s/ holds a program named for it (%s) but no action file %s.yml. A "
                    "directory addressable by its own name is an action only when it "
                    "declares one -- without it no runner can start the program and no "
                    "index lists it." % (_rel(base, sub), os.path.basename(
                        primary_script(sub, n)), n))
            found += group(sub)
        if found == 0 and rel != ".":
            raise Collapse(
                "%s/ holds no action at any depth. A directory under the actions root is "
                "an action or a group of actions; one that is neither is a folder someone "
                "left behind." % rel)
        return found

    group(base)

    if len(entries) < ACTION_FLOOR:
        raise Collapse(
            "found only %d actions under %s, floor is %d. The "
            "enumeration COLLAPSED -- fix the scan, do not lower the floor."
            % (len(entries), base, ACTION_FLOOR))
    return sorted(entries, key=lambda e: e.rel)



def runner_actions(root):
    """{runner name: its `action` value} from `predefinedRunners` in `config.json`.

    A runner that declares `phases` instead of an action has no action to reach and is
    not listed. A config that cannot be read is a COLLAPSE: without it the reachability
    clause would pass over nothing.
    """
    path = os.path.join(root, CONFIG_REL)
    if not os.path.isfile(path):
        raise Collapse("%s does not exist, so no action's runner can be checked"
                       % CONFIG_REL.replace(os.sep, "/"))
    try:
        # ★ The tree's ONE JSONC reader (`owning-tree.strip_jsonc`, which this file used to
        # hold privately until `lane-worktree` needed the same file, 2026-09-21).
        ot = _owning_tree()
        cfg = json.loads(ot.strip_jsonc(io.open(path, "r", encoding="utf-8").read(),
                                        CONFIG_REL.replace(os.sep, "/")))
    except ot.Refusal as exc:
        raise Collapse(str(exc))
    except ValueError as exc:
        raise Collapse("%s could not be read as JSON once its comments were removed: %s"
                       % (CONFIG_REL.replace(os.sep, "/"), exc))
    runners = cfg.get("predefinedRunners")
    if not isinstance(runners, dict):
        raise Collapse("%s declares no `predefinedRunners` object"
                       % CONFIG_REL.replace(os.sep, "/"))
    return {name: str(r["action"]).replace("\\", "/")
            for name, r in runners.items()
            if isinstance(r, dict) and r.get("action")}


# ── CLAUSE 13: EVERY VERB A PROGRAM OFFERS IS STARTED BY A STEP (2026-09-30, cycle P69) ─────────────────────────
# ★★★ THE OPERATOR'S ORDER OF 2026-09-24 routes the repository's operations through DssHarness, and work an action
# owns but a plain run must not do is a MANUAL STEP of that action. A verb a program offers that no step, no ctest
# entry and no reached program starts is a script someone must start BY HAND -- the class the registry row on
# action program verbs measured at five instances plus a heuristic tail, and which grows unseen unless a guard
# counts it. So every command-line token a program dispatches on is either REACHED -- a token on a run line of a
# step that starts the program (anchor-rows' `--step` table included), or after the program's path in an
# `add_test` COMMAND of the root CMakeLists.txt -- or RECORDED in `verbs.json` under a category this clause
# VERIFIES every run; a record that no longer verifies is as red as a missing one. There is no category for
# "run by hand". ⚠ A pipe FILTER is no exception to write down: it dispatches on no token (standard input is the
# no-argument form), and a runner step could not feed it anyway -- a step reads no standard input (✔MEASURED
# 2026-09-30, run 20260930-164007-b87cbb24).
# ★★ AND A VERB'S LEGS FIT IT: a step that WRITES this machine's tree (declared in `verbs.json` as a tree writer,
# and any step whose run line carries a write token must be declared one) is reachable through a runner of ONE
# leg that names no other host, whose `steps` name it -- and every `dssharness run <runner> --manual-step <step>`
# a program prints names such a runner for such a step (a two-leg runner's `--manual-step write` rewrote the WSL
# leg's synced copy too: the round-13 audit's F1-A11). This clause is the ONE statement of that fact; the guards
# that each re-checked their own runner no longer do. A step whose write goes into its OWN KEPT OUTPUT (`persist`,
# its `outputs`, `{stepBuild}` on its run line) writes no tree: it may run on any leg, its file pulled back and
# reviewed -- and it is never declared a tree writer (2026-10-01, the P69 review's MAJOR 2).
VERBS_REL = os.path.join(ACTIONS_REL, "check-scripts-index", "verbs.json")
CMAKE_REL = "CMakeLists.txt"
_OPTION = re.compile(r"^--[a-z][a-z0-9-]*$")
_BARE_VERB = re.compile(r"^[a-z][a-z0-9-]*$")
HELP_TOKENS = frozenset(("--help", "-h"))
# The tokens that make a run line a WRITE of the tree, and so a step that must be declared a tree writer.
WRITE_TOKENS = frozenset(("--write", "--apply", "--update", "--regen", "--baseline", "--insert"))
VERB_CATEGORIES = ("driven", "self-driven", "alias", "test-source")
_RUN_TOKEN = re.compile(r'"[^"]*"|[^\s"]+')
_REMEDY = re.compile(r"dssharness run ([A-Za-z0-9_.-]+)(?:[^\n`'\"]*?--manual-step ([A-Za-z0-9_,-]+))?")
# A program of an action started by hand: any Python launcher (`python`, `python3`, `python3.12`, `python.exe`, the
# Windows `py -3`), any slash -- a Windows path is one too (the P69 review: the rule saw forward slashes alone).
_SCRIPT_REMEDY = re.compile(r"(?i)\b(?:python(?:3(?:\.\d+)?)?(?:\.exe)?|py(?:\.exe)?\s+-3)\s+\S*\.harness-config"
                            r"[\\/]+runner[\\/]+actions[\\/]+\S+\.py\b")


def yml_steps(path):
    """-> [{name, manual, unmoved, persist, outputs, run: [lines], inputs: [names]}] of one action file, read in the
    ONE layout every action here keeps (a step at two spaces, its keys at four, a `run: |` block below, inputs at
    six, `outputs` an inline list) -- which `verb_census` holds: a file this reader finds no step in is a COLLAPSE,
    never an action with nothing to reach."""
    steps, cur, where = [], None, None
    for raw in io.open(path, "r", encoding="utf-8").read().splitlines():
        if raw.lstrip().startswith("#"):
            continue
        m = re.match(r"^  - name: *['\"]?([^'\"\s]+)['\"]?\s*$", raw)
        if m:
            cur, where = {"name": m.group(1), "manual": False, "unmoved": False, "run": [], "inputs": [],
                          "persist": False, "outputs": []}, None
            steps.append(cur)
            continue
        if cur is None:
            continue
        if re.match(r"^\S", raw):
            cur, where = None, None
            continue
        key = re.match(r"^    ([A-Za-z]+):\s*(.*)$", raw)
        if key:
            where = {"run": "run", "inputs": "inputs"}.get(key.group(1))
            if key.group(1) == "manual":
                cur["manual"] = key.group(2).strip() == "true"
            elif key.group(1) == "requireInputsUnmoved":
                cur["unmoved"] = key.group(2).strip() == "true"
            elif key.group(1) == "persist":
                cur["persist"] = key.group(2).strip() == "true"
            elif key.group(1) == "outputs":
                cur["outputs"] = [s.strip().strip("'\"") for s in key.group(2).strip().strip("[]").split(",")
                                  if s.strip()]
            continue
        if where == "run" and raw.strip():
            cur["run"].append(raw.strip())
        elif where == "inputs":
            m = re.match(r"^      ([A-Za-z_][A-Za-z0-9_]*):\s*$", raw)
            if m:
                cur["inputs"].append(m.group(1))
    return steps


def run_tokens(line):
    """A run line as DssHarness splits it: whitespace, double quotes only, no escapes; `--x=v` is also `--x`."""
    out = []
    for t in _RUN_TOKEN.findall(line):
        t = t.strip('"')
        out.append(t)
        if t.startswith("--") and "=" in t:
            out.append(t.split("=", 1)[0])
    return out


def _const(node):
    return node.value if isinstance(node, ast.Constant) and isinstance(node.value, str) else None


# ★★ WHICH EXPRESSIONS CARRY THE COMMAND LINE: a DATA PATH, never a variable's NAME. The first draft seeded every
# name spelled `argv`, `args` or `argv_` and walked every constant inside a tainted side, and ✔MEASURED 2026-09-30 on
# this tree it counted record fields (`row["kind"] == "armed"`, a subscript KEY), an argparse Namespace's values and
# a child process's argv (`spawn(argv, ...)`) as verbs: 40 of `harness_legs.py`'s 58 unreached tokens were data. So
# the taint starts ONLY at `sys.argv` and flows -- per SCOPE, so a name is tainted where its value came from the
# command line and nowhere else -- into a name assigned from (or looping over) a tainted value; through the
# conversions that keep the elements (`list`, `tuple`, `sorted`, ...), a conditional, a boolean choice, a
# concatenation, a comprehension over it and a method called on it (`a.partition("=")`); and from a call's tainted
# argument into the called function's parameter. An argparse Namespace (`ap.parse_args(argv)`) is NOT tainted: its
# options are counted where they are DECLARED (`add_argument`), and a choice among one option's values is not a verb.
# ★ WHAT a tainted value is decides what its comparison declares, so the taint keeps three KINDS: the LIST (`argv`,
# a slice of it), one whole TOKEN of it (`argv[0]`, a loop variable over it) and a PART of one (what a method
# returns: `a.partition("=")`, and so an input's value after `name=`). An option (`--x`) is declared against any of
# the three; a bare verb only against a whole token, or by membership in the list (`"clean" in argv`) -- the second
# draft took `liveOnly=true`'s `true` for a verb. Only the LITERAL side of a comparison is read (and a module-level
# tuple, list or set of literals it names) -- never a constant inside the tainted side, where a subscript key sits.
_NODES = {}


def _all_nodes(tree):
    """Every node of a parsed program, in ast.walk's order, walked ONCE: the census asks each program several
    questions (its surface, its passed literals, its imports, its remedies), and a walk per question was most of
    its time (✔MEASURED 2026-09-30, census_profile: 5.4 million ast.walk steps for one census)."""
    got = _NODES.get(id(tree))
    if got is None or got[0] is not tree:
        got = (tree, list(ast.walk(tree)))
        _NODES[id(tree)] = got
    return got[1]


_TAINT_KEEPERS = frozenset(("list", "tuple", "iter", "sorted", "reversed", "enumerate", "zip"))
_SCOPES = (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)
_LIST, _TOKEN, _PART = "list", "token", "part"


class ArgvTaint(object):
    """The command line's data path through one parsed program (the block above says how it flows)."""

    def __init__(self, tree):
        self.tree = tree
        self.parent = {}
        for n in _all_nodes(tree):
            for c in ast.iter_child_nodes(n):
                self.parent[id(c)] = n
        self.defs = {}
        for n in _all_nodes(tree):
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)):
                self.defs.setdefault(n.name, []).append(n)
        self.names = {}          # id(scope node) -> {name: {kind, ...}}
        self.collections = {}    # module-level NAME = (literal, ...) -> its literals
        for n in tree.body:
            if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name):
                lits = _display_literals(n.value)
                if lits is not None:
                    self.collections[n.targets[0].id] = lits
        # the nodes a pass reads, collected ONCE: a pass over them repeats, a walk of the whole program need not
        self.binders = [n for n in _all_nodes(tree) if isinstance(n, (ast.Assign, ast.AnnAssign, ast.AugAssign,
                                                                   ast.NamedExpr, ast.For, ast.AsyncFor,
                                                                   ast.comprehension, ast.Call))]
        for _ in range(12):
            if not self._pass():
                break

    def scope_of(self, node):
        n = self.parent.get(id(node))
        while n is not None and not isinstance(n, _SCOPES):
            n = self.parent.get(id(n))
        return self.tree if n is None else n

    def _chain(self, scope):
        """The scope and every scope enclosing it: a closure reads its enclosing scopes' names."""
        out = [scope]
        while scope is not self.tree:
            scope = self.scope_of(scope)
            out.append(scope)
        return out

    def kinds(self, node, scope=None):
        """-> the set of kinds `node` carries (empty: it does not carry the command line)."""
        scope = self.scope_of(node) if scope is None else scope
        if isinstance(node, ast.Attribute):
            if node.attr == "argv" and isinstance(node.value, ast.Name) and node.value.id == "sys":
                return {_LIST}
            return {_PART} if self.kinds(node.value, scope) else set()
        if isinstance(node, ast.Name):
            out = set()
            for s in self._chain(scope):
                out |= self.names.get(id(s), {}).get(node.id, set())
            return out
        if isinstance(node, ast.Starred):
            return self.kinds(node.value, scope)
        if isinstance(node, ast.Subscript):
            k = self.kinds(node.value, scope)
            if not k:
                return set()
            if isinstance(node.slice, ast.Slice):
                return {_LIST if x == _LIST else _PART for x in k}
            return {_TOKEN if x == _LIST else _PART for x in k}
        if isinstance(node, ast.Call):
            if isinstance(node.func, ast.Attribute):
                return {_PART} if self.kinds(node.func.value, scope) else set()
            if isinstance(node.func, ast.Name) and node.func.id in _TAINT_KEEPERS:
                k = set()
                for a in node.args:
                    k |= self.kinds(a, scope)
                return {_LIST} if _LIST in k else ({_PART} if k else set())
            return set()
        if isinstance(node, ast.IfExp):
            return self.kinds(node.body, scope) | self.kinds(node.orelse, scope)
        if isinstance(node, ast.BoolOp):
            out = set()
            for v in node.values:
                out |= self.kinds(v, scope)
            return out
        if isinstance(node, ast.BinOp):
            k = self.kinds(node.left, scope) | self.kinds(node.right, scope)
            return {_LIST} if _LIST in k else ({_PART} if k else set())
        if isinstance(node, (ast.ListComp, ast.SetComp, ast.GeneratorExp)):
            return {_LIST} if any(self.kinds(g.iter, scope) for g in node.generators) else set()
        if isinstance(node, ast.NamedExpr):
            return self.kinds(node.value, scope)
        return set()

    def tainted(self, node, scope=None):
        return bool(self.kinds(node, scope))

    def _add(self, scope, name, kinds):
        have = self.names.setdefault(id(scope), {}).setdefault(name, set())
        if kinds <= have:
            return False
        have |= kinds
        return True

    def _bind(self, target, kinds, scope, element):
        """Bind `target` to a value of `kinds`; `element` means one ELEMENT of it (a loop, an unpacking)."""
        if element:
            kinds = {_TOKEN if k == _LIST else _PART for k in kinds}
        grew = False
        if isinstance(target, ast.Name):
            grew |= self._add(scope, target.id, kinds)
        elif isinstance(target, ast.Starred):
            grew |= self._bind(target.value, {_LIST if k == _TOKEN else k for k in kinds} if element else kinds,
                               scope, False)
        elif isinstance(target, (ast.Tuple, ast.List)):
            for e in target.elts:
                grew |= self._bind(e, {_TOKEN if k == _LIST else _PART for k in kinds} if not element else kinds,
                                   scope, False)
        return grew

    def _pass(self):
        grew = False
        for n in self.binders:
            if isinstance(n, (ast.Assign, ast.AnnAssign, ast.AugAssign, ast.NamedExpr)):
                targets = n.targets if isinstance(n, ast.Assign) else [n.target]
                scope = self.scope_of(n)
                # `t, inline = argv[i], None`: a tuple assigned from a tuple display binds PAIRWISE.
                if isinstance(n.value, (ast.Tuple, ast.List)):
                    for t in targets:
                        if isinstance(t, (ast.Tuple, ast.List)) and len(t.elts) == len(n.value.elts):
                            for te, ve in zip(t.elts, n.value.elts):
                                kv = self.kinds(ve)
                                if kv:
                                    grew |= self._bind(te, kv, scope, False)
                    continue
                k = self.kinds(n.value) if n.value is not None else set()
                if k:
                    for t in targets:
                        grew |= self._bind(t, k, scope, False)
            elif isinstance(n, (ast.For, ast.AsyncFor, ast.comprehension)):
                k = self.kinds(n.iter)
                if k:
                    grew |= self._bind(n.target, k, self.scope_of(n), True)
            elif isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in self.defs:
                for d in self.defs[n.func.id]:
                    params = [a.arg for a in d.args.posonlyargs + d.args.args]
                    for i, a in enumerate(n.args):
                        if isinstance(a, ast.Starred):
                            break
                        k = self.kinds(a) if i < len(params) else set()
                        if k:
                            grew |= self._add(d, params[i], k)
                    for kw in n.keywords:
                        k = self.kinds(kw.value) if kw.arg else set()
                        if k:
                            grew |= self._add(d, kw.arg, k)
        return grew

    def literals(self, side):
        """The string literals a NON-tainted comparison side offers: itself, a display's elements, or the elements
        of the module-level collection it names."""
        lits = _display_literals(side)
        if lits is not None:
            return lits
        if isinstance(side, ast.Name):
            return self.collections.get(side.id, [])
        c = _const(side)
        return [c] if c is not None else []


def _display_literals(node):
    """A tuple/list/set display of string literals, or `frozenset(...)`/`set(...)`/`tuple(...)` of one -> its
    literals; anything else -> None."""
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in ("frozenset", "set", "tuple") \
            and len(node.args) == 1:
        node = node.args[0]
    if isinstance(node, (ast.Tuple, ast.List, ast.Set)):
        return [c for c in (_const(e) for e in node.elts) if c is not None]
    return None


def program_surface(tree):
    """-> {token: kind} a PROGRAM dispatches on: `add_parser` names, `add_argument` long options, and the literals a
    comparison (==, !=, in, not in) or a `startswith` holds against the COMMAND LINE (`ArgvTaint`) -- an option
    (`--x`) against any part of it, a bare verb only against a whole token of it or by membership in it."""
    taint = ArgvTaint(tree)
    found = {}
    # ★ An option DECLARED IN A LOOP (`for flag in ("--sqlite-dir", "--dss", ...): p.add_argument(flag, ...)`) is
    # declared as surely as a literal one: the loop variable stands for every literal the loop walks. ✔MEASURED
    # 2026-09-30: benchmark_speedtest1 declares seven options that way, and the census could not see five of them.
    looped = {}
    for n in _all_nodes(tree):
        if isinstance(n, (ast.For, ast.AsyncFor)) and isinstance(n.target, ast.Name):
            lits = taint.literals(n.iter)
            if lits:
                looped.setdefault(n.target.id, []).extend(lits)
    for n in _all_nodes(tree):
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute):
            if n.func.attr == "add_parser" and n.args and _const(n.args[0]):
                found.setdefault(_const(n.args[0]), "subcommand")
            elif n.func.attr == "add_argument":
                for a in n.args:
                    for c in ([_const(a)] if _const(a) else looped.get(a.id, []) if isinstance(a, ast.Name) else []):
                        if c and _OPTION.match(c):
                            found.setdefault(c, "option")
            elif n.func.attr == "startswith" and taint.tainted(n.func.value):
                for a in n.args:
                    for c in taint.literals(a):
                        if _OPTION.match(c.rstrip("=")):
                            found.setdefault(c.rstrip("="), "option")
        elif isinstance(n, ast.Compare):
            sides = [n.left] + list(n.comparators)
            hot = [(s, taint.kinds(s)) for s in sides]
            hot = [(s, k) for s, k in hot if k]
            if not hot:
                continue
            bare = any(_TOKEN in k for _s, k in hot) or (
                any(_LIST in k for _s, k in hot) and any(isinstance(op, (ast.In, ast.NotIn)) for op in n.ops))
            for s in sides:
                if any(s is h for h, _k in hot):
                    continue
                for c in taint.literals(s):
                    if _OPTION.match(c):
                        found.setdefault(c, "option")
                    elif bare and _BARE_VERB.match(c) and len(c) > 2:
                        found.setdefault(c, "verb")
    return found


_LITERALS = {}


def argument_literals(tree):
    """(Computed once per parsed program -- `_argument_literals` says what it is.)"""
    got = _LITERALS.get(id(tree))
    if got is None or got[0] is not tree:
        got = (tree, _argument_literals(tree))
        _LITERALS[id(tree)] = got
    return got[1]


def _argument_literals(tree):
    """The string constants a program PASSES: elements of a list, tuple or set display, and call arguments --
    never a comparison's operand (that is where a token is dispatched on), nor a display bound to a name a
    comparison reads (`known = ("--a", "--b")` then `a not in known`: a declaration spelled in two statements --
    ✔MEASURED 2026-09-30, such a tuple made every option of `check-line-endings` look self-driven), nor an
    add_argument/add_parser argument (where it is declared). How a `driven` or `self-driven` record is verified: by
    position in the parsed program, never by the text appearing somewhere in it."""
    skip, member_of = set(), set()
    for n in _all_nodes(tree):
        if isinstance(n, ast.Compare):
            skip.update(id(s) for s in ast.walk(n))
            # the NAMES a membership test reads (`a not in known`): what such a name holds is a vocabulary
            sides = [n.left] + list(n.comparators)
            for op, right in zip(n.ops, sides[1:]):
                if isinstance(op, (ast.In, ast.NotIn)) and isinstance(right, ast.Name):
                    member_of.add(right.id)
        elif isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and n.func.attr in ("add_argument",
                                                                                               "add_parser"):
            skip.update(id(s) for s in ast.walk(n))
    # ...and only a display of LITERALS ALONE bound to such a name is a declaration. `argv = [python, gen, "--tus",
    # tus]` is a command line whatever later compares it, and `plan = resolver.json([...])` passes its list (both
    # ✔MEASURED 2026-09-30 in the sqlite driver: an unqualified rule hid eleven passed options).
    for n in _all_nodes(tree):
        if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in member_of for t in n.targets):
            lits = _display_literals(n.value)
            shown = n.value.args[0] if isinstance(n.value, ast.Call) and n.value.args else n.value
            if lits is not None and len(lits) == len(getattr(shown, "elts", ())):
                skip.update(id(s) for s in ast.walk(n.value))
    out = set()
    for n in _all_nodes(tree):
        elts = []
        if isinstance(n, (ast.List, ast.Tuple, ast.Set)):
            elts = n.elts
        elif isinstance(n, ast.Call):
            elts = list(n.args) + [k.value for k in n.keywords]
        for e in elts:
            c = _const(e)
            if c is not None and id(e) not in skip:
                out.add(c)
                if c.startswith("--") and "=" in c:
                    out.add(c.split("=", 1)[0])
    # ★ A FORMATTED OPTION TOKEN IS PASSED AS SURELY AS A LITERAL ONE (2026-09-30): `"--failure=%s" % t` and
    # `f"--assert-translated={a}"` hand the child `--failure` and `--assert-translated`, and the sqlite driver's
    # modules spell every repeated option that way. Only the option NAME before `=` is counted, never the value.
    for n in _all_nodes(tree):
        lead = None
        if isinstance(n, ast.BinOp) and isinstance(n.op, ast.Mod):
            lead = _const(n.left)
        elif isinstance(n, ast.JoinedStr) and n.values:
            lead = _const(n.values[0])
        if lead and id(n) not in skip and "=" in lead and _OPTION.match(lead.split("=", 1)[0]):
            out.add(lead.split("=", 1)[0])
    return out


_INDEX = {}


def _index(tree):
    """-> (the program's string constants, the module names it imports), walked ONCE per parsed program: the census
    asks both of every pair of programs, and `harness_legs.py` alone is some 19 000 lines (✔MEASURED 2026-09-30: a
    walk per question took the census past two minutes). The tree is kept beside its id, so a reused id is a miss."""
    got = _INDEX.get(id(tree))
    if got is None or got[0] is not tree:
        consts, imports = [], set()
        for n in _all_nodes(tree):
            c = _const(n)
            if c is not None:
                consts.append(c)
            elif isinstance(n, ast.Import):
                imports.update(a.name.split(".")[0] for a in n.names)
            elif isinstance(n, ast.ImportFrom) and n.module:
                imports.add(n.module.split(".")[0])
        got = (tree, consts, imports)
        _INDEX[id(tree)] = got
    return got[1], got[2]


def imports_module(tree, module):
    """Does the parsed program IMPORT `module` by name (`import m`, `import m as x`, `from m import y`)?"""
    return module in _index(tree)[1]


def running_set(parsed, started):
    """The programs whose code RUNS: every started one, and -- to a fixpoint -- every module a running one imports
    by name (the sqlite driver imports `sqlite_units`, whose code starts `harness_legs.py` with the tokens it
    passes: the process is the driver's, the argument literals are the module's)."""
    by_module = {}
    for q in parsed:
        by_module.setdefault(os.path.splitext(os.path.basename(q))[0], []).append(q)
    running, stack = set(started), sorted(started)
    while stack:
        p = stack.pop()
        if p not in parsed:
            continue
        for module in _index(parsed[p])[1]:
            for q in by_module.get(module, ()):
                if q not in running:
                    running.add(q)
                    stack.append(q)
    return running


def references_program(tree, basename):
    """Does the parsed program name `basename` (a `.py` file of the same action) -- a string holding its file name,
    or an import of its module?"""
    consts, imports = _index(tree)
    return os.path.splitext(basename)[0] in imports or any(basename in c for c in consts)


def step_table(tree):
    """anchor-rows' convention: a module-level `STEP_INPUTS` mapping verb -> ((input, kind), ...). -> {verb: [the
    options its inputs become]} (a `dir` input is positional, every other kind `--<kebab>`)."""
    for n in tree.body:
        if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "STEP_INPUTS" for t in n.targets):
            table = {}
            for pair in ast.walk(n.value):
                if isinstance(pair, ast.Tuple) and len(pair.elts) == 2 and _const(pair.elts[0]) \
                        and isinstance(pair.elts[1], ast.Tuple):
                    opts = []
                    for inp in pair.elts[1].elts:
                        if isinstance(inp, ast.Tuple) and len(inp.elts) == 2 and _const(inp.elts[1]) != "dir":
                            opts.append("--" + re.sub(r"([A-Z])", lambda m: "-" + m.group(1).lower(),
                                                      _const(inp.elts[0])))
                    table[_const(pair.elts[0])] = opts
            return table
    return {}


def ctest_tokens(root):
    """{action-relative program path: tokens after it} from every `add_test` COMMAND in the root CMakeLists.txt."""
    path = os.path.join(root, CMAKE_REL)
    out = {}
    if not os.path.isfile(path):
        return out
    text = io.open(path, "r", encoding="utf-8").read()
    for block in re.finditer(r"add_test\s*\((.*?)\)\s*\n", text, re.S):
        body = block.group(1)
        m = re.search(r"\.harness-config/runner/actions/([^\s\"]+\.py)\"?(.*?)(?:WORKING_DIRECTORY|$)", body, re.S)
        if m:
            toks = [t.strip('"') for t in re.findall(r'"[^"]*"|\S+', m.group(2))]
            out.setdefault(m.group(1), set()).update(t.split("=", 1)[0] if t.startswith("--") else t for t in toks)
    return out


def load_config(root):
    ot = _owning_tree()
    return json.loads(ot.strip_jsonc(io.open(os.path.join(root, CONFIG_REL), "r", encoding="utf-8").read(),
                                     CONFIG_REL.replace(os.sep, "/")))


def verb_census(root, entries):
    """Clause 13 -> (problems, counts). `counts` says how many tokens were REACHED, RECORDED and left RED."""
    base = os.path.join(root, ACTIONS_REL)
    cfg = load_config(root)
    runners = cfg.get("predefinedRunners") or {}
    legs = cfg.get("legs") or {}
    try:
        record = json.loads(io.open(os.path.join(root, VERBS_REL), "r", encoding="utf-8").read())
    except (OSError, ValueError) as exc:
        raise Collapse("%s cannot be read (%s): the verb census has no record to verify, so it would pass over "
                       "nothing" % (VERBS_REL.replace(os.sep, "/"), exc))
    records = record.get("verbs") or []
    writers = set(record.get("treeWriters") or [])
    problems, counts = [], {"programs": 0, "tokens": 0, "reached": 0, "recorded": 0, "red": 0}
    ctest = ctest_tokens(root)
    parsed, surfaces, reached, started = {}, {}, {}, set()
    steps_of = {}
    for e in entries:
        adir = os.path.join(base, *e.rel.split("/"))
        steps = yml_steps(os.path.join(root, e.action_file))
        if not steps:
            raise Collapse("%s declares no step this clause can read -- a census over an action with no step would "
                           "report every verb unreached, or none" % e.action_file)
        steps_of[e.rel] = steps
        for sib in e.siblings:
            prog = "%s/%s" % (e.rel, sib)
            text = io.open(os.path.join(adir, sib), "r", encoding="utf-8").read()
            try:
                tree = ast.parse(text, filename=sib)
            except SyntaxError as exc:
                raise Collapse("%s cannot be parsed, so its verbs cannot be counted: %s" % (prog, exc))
            parsed[prog] = tree
            if "__main__" not in text:
                continue
            surfaces[prog] = program_surface(tree)
            got = set(ctest.get(prog, set()))
            if prog in ctest:
                started.add(prog)
            table = step_table(tree)
            for st in steps:
                for line in st["run"]:
                    toks = run_tokens(line)
                    if not any(t in ("./" + sib, sib) or t.endswith("/" + sib) for t in toks):
                        continue
                    started.add(prog)
                    got.update(toks)
                    if "--step" in toks and toks.index("--step") + 1 < len(toks):
                        verb = toks[toks.index("--step") + 1]
                        got.add(verb)
                        got.update(table.get(verb, []))
            reached[prog] = got
    by_prog = {}
    for r in records:
        by_prog.setdefault(r.get("program"), []).append(r)

    running = running_set(parsed, started)

    literals_of, tie_of = {}, {}

    def ties(b, basename):
        """`b` names the program -- or imports a module that does (ONE step: the sqlite driver's modules call
        `run.resolver.call([...])` and `sqlite_common` holds the program's path)."""
        if (b, basename) not in tie_of:
            tie_of[(b, basename)] = references_program(parsed[b], basename) or any(
                q != b and imports_module(parsed[b], os.path.splitext(os.path.basename(q))[0])
                and references_program(parsed[q], basename) for q in parsed)
        return tie_of[(b, basename)]

    def driven_by(prog, token, bys):
        for b in bys:
            t = parsed.get(b)
            if t is None or b not in running or not ties(b, os.path.basename(prog)):
                continue
            if b not in literals_of:
                literals_of[b] = argument_literals(t)
            if token in literals_of[b]:
                return True
        return False

    # A program a RUNNING program starts is itself started -- a `driven` record says so, and so does a running
    # program holding the program's FILE NAME as a string of its own, exactly (`"harness_legs.py"`, or a path
    # ending in it): the sqlite driver's Step 0 starts each suite by name, from a tuple its loop reads, with no
    # argument a record could name. A sentence that merely mentions a file is never exactly its name. Iterated to a
    # fixpoint.
    for _ in range(8):
        grew = False
        for r in records:
            if r.get("category") == "driven" and r.get("program") in surfaces and r["program"] not in started \
                    and any(b in running for b in _as_list(r.get("by"))):
                started.add(r["program"])
                grew = True
        for prog in surfaces:
            if prog in started:
                continue
            name = os.path.basename(prog)
            if any(any(c == name or c.replace("\\", "/").endswith("/" + name) for c in _index(parsed[b])[0])
                   for b in running if b != prog):
                started.add(prog)
                grew = True
        running = running_set(parsed, started)
        if not grew:
            break
    for prog in sorted(surfaces):
        counts["programs"] += 1
        recs = by_prog.get(prog, [])
        for token in sorted(surfaces[prog]):
            counts["tokens"] += 1
            if token in HELP_TOKENS:
                counts["reached"] += 1
                continue
            if token in reached[prog]:
                counts["reached"] += 1
                if any(r.get("token") == token for r in recs):
                    problems.append("%s: %s is RECORDED in %s but a step or ctest entry reaches it -- a stale "
                                    "record; remove it" % (prog, token, VERBS_REL.replace(os.sep, "/")))
                continue
            # an exact record wins over the program's `*` record: a token its program passes to itself is
            # self-driven although every other token of that program is driven
            rec = next((r for r in recs if r.get("token") == token), None) or \
                next((r for r in recs if r.get("token") == "*"), None)
            why = _verify_record(rec, prog, token, parsed, reached, running, driven_by, root, recs) if rec else \
                "no step, no ctest entry and no record reaches it"
            if why:
                counts["red"] += 1
                problems.append("%s: the verb %s is started by nothing (%s). Make it a MANUAL step of its action "
                                "(anchor-rows' STEP_INPUTS/--step pattern), retire it, or record in %s the "
                                "category that reaches it (%s)"
                                % (prog, token, why, VERBS_REL.replace(os.sep, "/"), ", ".join(VERB_CATEGORIES)))
            else:
                counts["recorded"] += 1
        for r in recs:
            if r.get("token") not in ("*",) and r.get("token") not in surfaces[prog]:
                problems.append("%s: %s records %s, which the program no longer dispatches on -- a stale record"
                                % (prog, VERBS_REL.replace(os.sep, "/"), r.get("token")))
    for prog in sorted(set(by_prog) - set(surfaces) - {None}):
        problems.append("%s records verbs of %s, which is not a program of any action (no `__main__`)"
                        % (VERBS_REL.replace(os.sep, "/"), prog))
    problems.extend(_leg_fit(steps_of, runners, legs, writers))
    problems.extend(_remedies(parsed, runners, legs, writers))
    return problems, counts


def _as_list(v):
    return v if isinstance(v, list) else [v] if v else []


def _verify_record(rec, prog, token, parsed, reached, running, driven_by, root, recs=None):
    """None when `rec` verifies for `token`, else why not."""
    cat = rec.get("category")
    if cat not in VERB_CATEGORIES:
        return "its record's category %r is not one of %s" % (cat, ", ".join(VERB_CATEGORIES))
    if not rec.get("why"):
        return "its record states no `why`"
    if cat == "driven":
        bys = _as_list(rec.get("by"))
        if not bys:
            return "a `driven` record names no program by `by`"
        if not driven_by(prog, token, bys):
            return ("recorded driven by %s, but no REACHED one of them names %s and passes %s in an argument list"
                    % (", ".join(bys), os.path.basename(prog), token))
        return None
    if cat == "self-driven":
        if prog not in running:
            return "recorded self-driven, but nothing starts the program itself"
        if token not in argument_literals(parsed[prog]):
            return "recorded self-driven, but the program never passes %s in an argument list" % token
        return None
    if cat == "alias":
        of = rec.get("of")
        covered = bool(of) and of in reached.get(prog, set())
        if not covered and of and recs:
            # the spelling it aliases may itself be reached through a record (`--self-test`, which the sqlite
            # driver's Step 0 passes each suite): judged the same way, never through another alias
            base = next((r for r in recs if r.get("token") == of), None) or \
                next((r for r in recs if r.get("token") == "*"), None)
            covered = base is not None and base.get("category") != "alias" and not _verify_record(
                base, prog, of, parsed, reached, running, driven_by, root)
        if not covered:
            return "recorded an alias of %r, which nothing reaches" % of
        for n in ast.walk(parsed[prog]):
            if isinstance(n, (ast.Compare, ast.Tuple, ast.List, ast.Call)):
                consts = {_const(s) for s in ast.walk(n)}
                if token in consts and of in consts:
                    return None
        return "recorded an alias of %s, but no one comparison or declaration holds both" % of
    source = rec.get("by")
    path = os.path.join(root, *str(source).split("/")) if source else None
    if not path or not os.path.isfile(path):
        return "recorded reached by the test source %r, which does not exist" % source
    text = io.open(path, "r", encoding="utf-8", errors="replace").read()
    if os.path.basename(prog) not in text or token not in text:
        return "recorded reached by %s, which does not name both %s and %s" % (source, os.path.basename(prog), token)
    return None


def _one_leg_here(runner, legs):
    names = runner.get("legs") or []
    leg = legs.get(names[0]) if len(names) == 1 else None
    return isinstance(leg, dict) and not any(k in leg for k in ("wsl", "ssh"))


def persists_its_write(st):
    """A step whose write goes into its OWN build directory and is KEPT -- `persist: true`, its `outputs` named, and
    `{stepBuild}` on its run line -- writes no tree: what it made is pulled back (`dssharness sync --pull`) and
    reviewed, so it may run on any leg (2026-10-01, the P69 review's MAJOR 2: pragma-profile-census's update)."""
    return bool(st.get("persist") and st.get("outputs") and any("{stepBuild}" in line for line in st["run"]))


def _leg_fit(steps_of, runners, legs, writers):
    problems = []
    declared = set()
    for rel, steps in steps_of.items():
        for st in steps:
            key = "%s/%s" % (rel, st["name"])
            writes = any(t in WRITE_TOKENS for line in st["run"] for t in run_tokens(line))
            if persists_its_write(st):
                if key in writers:
                    declared.add(key)
                    problems.append("%s is declared a tree writer, but its write goes into its own kept output "
                                    "(`persist`, `outputs`, `{stepBuild}`), which writes no tree -- a stale record "
                                    "in %s" % (key, VERBS_REL.replace(os.sep, "/")))
                continue
            if key in writers:
                declared.add(key)
                if not st["manual"] or st["unmoved"]:
                    problems.append("%s is declared a tree writer but is %s -- a writer is a MANUAL step without "
                                    "requireInputsUnmoved" % (key, "not manual" if not st["manual"]
                                                              else "requireInputsUnmoved"))
                action = "%s/%s.yml" % (rel, rel.split("/")[-1])
                # A one-leg runner of this tree reaches the step when its `steps` name it, or when it names none --
                # then `--manual-step <step>` runs it there (anchor-rows' `rows`).
                if not any(r.get("action") == action and (st["name"] in (r.get("steps") or [])
                                                          or not r.get("steps"))
                           and _one_leg_here(r, legs) for r in runners.values() if isinstance(r, dict)):
                    problems.append("%s writes this machine's tree, but no runner of ONE leg of this machine's own "
                                    "tree names it in `steps` -- following a remedy through a runner of several "
                                    "legs writes every leg's synced copy" % key)
            elif writes:
                problems.append("%s carries a write token on its run line but is not declared a tree writer in %s"
                                % (key, VERBS_REL.replace(os.sep, "/")))
    for key in sorted(writers - declared):
        problems.append("%s declares the tree writer %s, which no action file holds -- a stale record"
                        % (VERBS_REL.replace(os.sep, "/"), key))
    return problems


def _remedies(parsed, runners, legs, writers):
    """A printed remedy names a runner that exists, fits the step it names, and never a script started by hand --
    and a DOCSTRING is printed text too (`--help`, a reader's first stop): its USAGE block names the harness step
    (the P69 review: the docstrings were exempt, and sixteen programs taught a start by hand there)."""
    problems = []
    writer_steps = {w.rsplit("/", 1)[-1] for w in writers}
    for prog, tree in sorted(parsed.items()):
        for n in _all_nodes(tree):
            c = _const(n)
            if c is None:
                continue
            if _SCRIPT_REMEDY.search(c):
                problems.append("%s line %d prints a remedy that starts an action's program by hand (%r) -- name "
                                "the harness step instead" % (prog, n.lineno, _SCRIPT_REMEDY.search(c).group(0)))
            for m in _REMEDY.finditer(c):   # a template (`%s`, `<runner>`, `{x}`) never matches a runner's name
                name, step = m.group(1), m.group(2)
                if name not in runners:
                    problems.append("%s line %d names the runner %r, which config.json does not declare"
                                    % (prog, n.lineno, name))
                    continue
                r = runners[name]
                # ★ Without `--manual-step`, the remedy runs whatever the runner's `steps` name -- which is how a
                # writer's own runner is named (`dssharness run check-scripts-index-write`). Judged the same way.
                runs = step.split(",") if step else (list(r.get("steps") or []) if isinstance(r, dict) else [])
                if isinstance(r, dict) and any(s in writer_steps for s in runs) and not _one_leg_here(r, legs):
                    problems.append("%s line %d names `dssharness run %s%s`: that runner has several legs, so "
                                    "the write reaches every leg's synced copy -- name the writer's one-leg runner"
                                    % (prog, n.lineno, name, (" --manual-step %s" % step) if step else ""))
    return problems


def render(entries):
    """The generated table body. Identical in both documents by construction."""
    out = [
        "| Action | Runs | Purpose |",
        "| --- | --- | --- |",
    ]
    for e in entries:
        runs = ", ".join("`" + s + "`" for s in e.siblings) or "—"
        out.append("| **`%s`** | %s | %s |" % (e.rel, runs, e.purpose))
    return "\n".join(out)


def splice(doc_text, body, doc_rel):
    b = doc_text.find(BEGIN)
    t = doc_text.find(END)
    if b < 0 or t < 0 or t < b:
        raise Collapse(
            "%s is missing its generated-index markers (%s / %s). Without them "
            "there is nothing to verify and the document silently stops being "
            "an index." % (doc_rel, BEGIN, END))
    # EXACTLY ONE PAIR. `find` takes the FIRST of each, so a second pair appended
    # below the real one is never looked at: found by audit, a fabricated block
    # naming a script that has never existed passed cleanly -- and whether it
    # passed depended on whether the forgery sat above or below the real one.
    if doc_text.count(BEGIN) != 1 or doc_text.count(END) != 1:
        raise Collapse(
            "%s carries %d BEGIN and %d END index markers; exactly one of each is "
            "required. Only the first pair is ever verified, so a second block is "
            "an UNCHECKED index inside a document that claims to be machine-checked."
            % (doc_rel, doc_text.count(BEGIN), doc_text.count(END)))
    return doc_text[:b] + BEGIN + "\n" + body + "\n" + doc_text[t:]


def run(root, write, census=True):
    # ★★★ THE GUARD'S OWN CONFIGURATION IS PART OF WHAT IT CHECKS. Both documents
    # are named constants, and dropping either from DOC_RELS would stop verifying
    # it while every message still claimed both were machine-checked. ✔MEASURED
    # by audit: halving this tuple left the whole self-test green. Pinned here
    # rather than in the self-test so it reds on a REAL run too.
    for required in (README_REL, SKILL_REL):
        if required not in DOC_RELS:
            raise Collapse(
                "%s is not in DOC_RELS, so it is no longer verified -- while this "
                "guard, both documents, and the /dss-cycle skill all still say it "
                "is. Verifying fewer documents is a change to the contract, not a "
                "configuration tweak." % required)
    entries = scan(root)
    body = render(entries)
    problems = []
    for rel in DOC_RELS:
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            raise Collapse("index document %s does not exist" % rel)
        cur = io.open(path, "r", encoding="utf-8", newline="").read()
        want = splice(cur, body, rel)
        if cur == want:
            continue
        if write:
            tmp = path + ".tmp"
            with io.open(tmp, "w", encoding="utf-8", newline="") as f:
                f.write(want)
            os.replace(tmp, path)
            print("check-scripts-index: rewrote %s" % rel)
        else:
            problems.append(rel)

    # ── REACHABILITY: config and tree must name the same set of actions ──────────
    runners = runner_actions(root)
    reached = set(runners.values())
    have = {}
    for e in entries:
        have[_rel(os.path.join(root, ACTIONS_REL), os.path.join(root, e.action_file))] = e
    unreached = sorted(a for a in have if a not in reached)
    dangling = sorted((n, a) for n, a in runners.items() if a not in have)

    if problems:
        print("check-scripts-index: FAIL -- the index disagrees with the tree in:")
        for rel in problems:
            print("    %s" % rel)
        print("")
        print("  The actions under %s and the rows in these documents must name the"
              % ACTIONS_REL.replace(os.sep, "/"))
        print("  same set, and each row's purpose must be the action's own `PURPOSE:` line.")
        print("  An action that no index documents is a program the next reader re-implements;")
        print("  an entry that no action backs sends them looking for a file that is gone.")
        print("")
        print("  Regenerate with:")
        print("      %s" % WRITE_VERB)
    if unreached or dangling:
        print("check-scripts-index: FAIL -- actions and runners disagree in %s:"
              % CONFIG_REL.replace(os.sep, "/"))
        for a in unreached:
            print("    action %s has no runner in predefinedRunners, so nothing can start it" % a)
        for n, a in dangling:
            print("    runner %s names action %s, which does not exist" % (n, a))
        print("")
        print("  `dssharness run <runner>` reaches an action ONLY through predefinedRunners;")
        print("  an action without an entry is a program nothing can start, and an entry")
        print("  without an action refuses the day someone runs it.")
    # ── CLAUSE 13: every verb a program offers is started by a step, or recorded and verified ──
    # (`census=False` only in a self-test arm that judges another clause: the census reads every program of the
    # mirror, and each such arm would pay for it again.)
    verb_problems, counts = verb_census(root, entries) if census else ([], None)
    if verb_problems:
        print("check-scripts-index: FAIL -- %d verb census problem(s):" % len(verb_problems))
        for p in verb_problems:
            print("    %s" % p)
        print("")
        print("  Work an action owns but a plain run must not do is a MANUAL step of that action (the")
        print("  operator's order of 2026-09-24): never a program started by hand.")
    if problems or unreached or dangling or verb_problems:
        return EXIT_DISAGREE

    census_said = ("verb census: %d program(s), %d token(s) -- %d reached by a step or a ctest entry, %d recorded "
                   "and verified, %d red" % (counts["programs"], counts["tokens"], counts["reached"],
                                              counts["recorded"], counts["red"])) if counts else \
        "verb census: not run in this self-test arm"
    print("check-scripts-index: OK (%d actions, both indexes agree with the tree and with "
          "each action's own PURPOSE line, and every action has a runner; %s)" % (len(entries), census_said))
    return EXIT_OK


# ── RED-ON-DISABLE SELF-TEST ────────────────────────────────────────────────
# ★★★ The guard PROVES it can fail, and it does so on a MIRROR of the tree, never
# on the tree itself: a self-test that mutates the working copy is one crash away
# from leaving a repository in the mutated state. Every arm asserts an EXIT CODE,
# not the absence of an exception -- "it did not throw" is exactly how a guard
# that stopped checking anything reports success.

_RAN = None   # set by selftest() so every arm is counted where it is judged


def _arm(label, root, expect, says=None, not_says=None, census=False):
    """Run the guard against a mutated mirror and judge the WHOLE verdict.

    ★★★ `says` / `not_says` are the load-bearing halves, not decoration.
    EXIT_COLLAPSE is shared by many distinct refusals, so an arm that asserts only
    an exit code proves that SOMETHING refused -- not that the mechanism it names
    did. ✔MEASURED by an independent audit 2026-08-19: the floor arm passed with
    the floor DELETED, because moving every entry out of the mirror also moved the
    index document that lives there, and the run collapsed on the missing
    document instead.
    """
    buf = io.StringIO()
    detail = ""
    try:
        with contextlib.redirect_stdout(buf):
            rc = run(root, write=False, census=census)
    except Collapse as exc:
        rc = EXIT_COLLAPSE
        detail = str(exc)
    text = buf.getvalue() + detail

    if _RAN is not None:
        _RAN.append(label)
    ok, why = rc == expect, ""
    if not ok:
        why = "EXPECTED rc=%d" % expect
    elif says is not None and [w for w in ((says,) if isinstance(says, str) else says) if w not in text]:
        ok, why = False, ("rc was right but the message never said %r"
                          % [w for w in ((says,) if isinstance(says, str) else says) if w not in text][0])
    elif not_says is not None and not_says in text:
        ok, why = False, ("rc was right but the message said %r, so this arm "
                          "proved a DIFFERENT refusal than it claims" % not_says)

    first = (detail or buf.getvalue()).split("\n")[0][:78]
    print("scripts-index: self-test arm %-26s rc=%d %s%s"
          % (label, rc, "as expected" if ok else why,
             (" (" + first + ")") if first else ""))
    return ok


# ⓘ `remedy_runner_fact()` (P68 round 13, F1-A11) retired on 2026-09-30 with its arm 2c: clause 13 is the ONE
# statement that a remedy names a runner config.json declares, and that a writer's runner has ONE leg of this
# machine's own tree -- for every program, not for this one alone (arms 22y..22z1).


def _mirror(root, dst):
    """The REAL actions tree, config and both index documents, copied -- without run
    output and bytecode, which are not the repository's -- and what the verb census reads
    beside them: the root CMakeLists.txt (a ctest COMMAND reaches a verb) and each test
    source a `test-source` record names."""
    shutil.copytree(os.path.join(root, ACTIONS_REL), os.path.join(dst, ACTIONS_REL),
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", *RUN_DIRS))
    try:
        sources = [str(r.get("by")) for r in json.loads(_read(os.path.join(root, VERBS_REL))).get("verbs", [])
                   if r.get("category") == "test-source" and r.get("by")]
    except (OSError, ValueError):
        sources = []
    for rel in (CONFIG_REL, CMAKE_REL) + DOC_RELS + tuple(s.replace("/", os.sep) for s in sources):
        d = os.path.join(dst, rel)
        if os.path.isfile(d):
            continue
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(os.path.join(root, rel), d)


def _read(p):
    return io.open(p, "r", encoding="utf-8", newline="").read()


def _write(p, text):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with io.open(p, "w", encoding="utf-8", newline="") as f:
        f.write(text)


# ★★★ THE FIXTURE'S SUBJECT IS NAMED ONCE, AND EVERY SABOTAGE PROVES IT LANDED.
#
# This self-test does not synthesize its subject: `_mirror` copies the REAL actions
# tree, the REAL config and both REAL index documents, so the arms sabotage an action
# that exists. That is deliberate -- a synthetic subject would prove the guard can
# read a fixture, not that it reads THIS repository -- and it has TWO failure modes,
# which are different and were ✔MEASURED separately, each through ctest:
#
#   (1) THE SUBJECT IS RETIRED: the first read raises, under a name that says
#       nothing about a retired fixture. `_subject_paths` now refuses by name.
#   (2) THE SUBJECT SURVIVES AND A NEEDLE DRIFTS -- a purpose reworded, an index
#       row respelled. Nothing raises; the document stays pristine and the arm
#       that expects a REFUSAL asserts against an untouched tree. **This is the
#       dangerous one**: a sabotage that changes nothing proves nothing, and it
#       fails toward CLEAN.
#
# ⇒ TWO RULES, and the second is the one that generalises:
#   · the subject is `SELFTEST_SUBJECT`, and its action file, its programs, the
#     index row and the purpose text are all DERIVED from it rather than re-typed;
#   · every sabotage goes through `_sabotage`, which RAISES when its needle is
#     absent. A future migration that retires this subject gets a loud refusal
#     naming the needle, in the same run, instead of a green self-test.
#
# The subject must be an action that survives: an action file carrying a `PURPOSE:`
# declaration, at least one program beside it that does NOT repeat the declaration
# (arm 14 needs something to contradict), no programs buried in its subdirectories,
# a runner, and a row in both index documents. `_subject_paths` checks the first
# three before arm 0 runs; arm 0 checks the rest.
SELFTEST_SUBJECT = "cmake-import"


def _sabotage(text, old, new, count=1):
    """`text.replace(old, new)`, refusing when `old` is not there.

    ★★★ The needle is the expectation. `str.replace` is silent about a miss, so
    a stale literal turns a refusal arm into a no-op that still reports the
    refusal it never caused.
    """
    if old not in text:
        raise Collapse(
            "self-test sabotage found nothing to replace: %r is absent from the "
            "text it was about to mutate. The arm would have run against a "
            "PRISTINE fixture and proved nothing. This is what a retired subject "
            "looks like -- point SELFTEST_SUBJECT at an action that exists, and "
            "derive the needle from it rather than re-typing it." % (old,))
    return text.replace(old, new, count)


def _subject_paths(tmp):
    """The mirrored subject's action file, every program that REPEATS its purpose, a
    contradictable program, and the purpose itself.

    Refuses here, before any arm runs, rather than letting a subject that no
    longer fits produce a confusing red six arms later.
    """
    here = os.path.join(tmp, ACTIONS_REL, SELFTEST_SUBJECT)
    afile = action_file(here) if os.path.isdir(here) else None
    if afile is None:
        raise Collapse(
            "SELFTEST_SUBJECT names %s/%s/, which is not an action (no %s.yml). The "
            "self-test sabotages a REAL action; point it at one that exists."
            % (ACTIONS_REL.replace(os.sep, "/"), SELFTEST_SUBJECT, SELFTEST_SUBJECT))
    purpose = read_purpose(afile)
    repeating, silent = [], []
    for f in sorted(os.listdir(here)):
        p = os.path.join(here, f)
        if not (os.path.isfile(p) and os.path.splitext(f)[1] in SCRIPT_EXTS):
            continue
        (repeating if declarations(_read(p)) else silent).append(p)
    if not silent:
        raise Collapse(
            "SELFTEST_SUBJECT names %s/, which has no program beside its action file "
            "that leaves the purpose undeclared. Arm 14 needs one to contradict."
            % SELFTEST_SUBJECT)
    return afile, repeating, silent[0], purpose


def _add_runner(tmp, name, action):
    """Declare a runner in the MIRRORED config, as its first entry. Text, not JSON, so
    the mirror stays the same JSONC the real file is."""
    path = os.path.join(tmp, CONFIG_REL)
    text = _read(path)
    _write(path, _sabotage(text, '"predefinedRunners": {',
                           '"predefinedRunners": {\n    "%s": { "action": "%s" },'
                           % (name, action)))


def _newcomer(tmp, rel, runner):
    """A synthesized action at `rel` (an actions-relative path), with a runner or not."""
    name = os.path.basename(rel)
    d = os.path.join(tmp, ACTIONS_REL, *rel.split("/"))
    _write(os.path.join(d, name + ".yml"),
           "# PURPOSE: exist, undocumented.\nname: %s\nsteps:\n  - name: s\n"
           "    run: |\n      python3 ./%s.py\n" % (name, name))
    _write(os.path.join(d, name + ".py"), "print('hello')\n")
    if runner:
        _add_runner(tmp, "zz-" + name, rel + "/" + name + ".yml")


# ★★ THE EXPECTED ARM COUNT IS A CONSTANT THE RUN CHECKS, NOT A SENTENCE IT
# PRINTS. The summary used to assert "11 arms exercised" in prose while the code
# ran a different number, and an audit found two of the counts in it wrong.
# A HARD constant. It was briefly derived as `fixed + 2 * len(DOC_RELS)`, which
# defeated its own purpose: deleting a document from DOC_RELS then lowered BOTH
# sides of the comparison and the sabotage passed. An expectation that follows
# the change it is meant to catch is not an expectation.
# The 2c arm retired (M6); clause 13's fixture arms, 30, then 32 (22w2 and 22w3, the widened remedy rule), then 35
# (22v1 to 22v3, a write into its own kept output); and the one arm numbered 23, CENSUS-WIRED.
EXPECTED_ARMS = 55 - 1 + 35 + 1


# ── CLAUSE 13's ARMS, over a SYNTHESIZED fixture ───────────────────────────────────────────────────────────────
# ★ The census reads every program of a tree, so an arm over the mirror costs a whole census; these arms build a
# fixture of three small actions instead and ask `verb_census` directly, judging the MESSAGE of each problem. The
# one arm over the mirror (23) proves `run()` refuses on what the census finds.
_FX_ACTIONS = ".harness-config/runner/actions/"
# ★ A REMEDY IN A FIXTURE IS COMPOSED, NEVER WRITTEN LITERALLY (check-doc-census's `_mark()` rule): the census reads
# every string of every action program, this one's included, so a literal `dssharness run alpha` or a `python3` start
# of `<actions>/alpha/alpha.py` here is a claim about THIS tree's runners and scripts -- deliberately wrong ones.
# ✔MEASURED 2026-09-30: written literally, arms 22w-22z1's five fixtures were five census problems of the live tree.
# Composing is not an exemption: those arms assert the census catches the same text written literally in a
# synthesized program.
_FX_RUN = "dssharness" + " run "
_FX_BY_HAND = "python3 " + _FX_ACTIONS + "alpha/alpha.py"
_FX_CONFIG = json.dumps({
    "legs": {"here": {"host": "local"}, "far": {"host": "wsl", "wsl": "fixture"}},
    "predefinedRunners": {"alpha": {"legs": ["here", "far"], "action": "alpha/alpha.yml"},
                          "alpha-write": {"legs": ["here"], "steps": ["write"], "action": "alpha/alpha.yml"},
                          "beta": {"legs": ["here"], "action": "beta/beta.yml"}}})
_FX_ALPHA_YML = ("# PURPOSE: a census fixture.\nname: alpha\nsteps:\n"
                 "  - name: go\n    workingDirectoryRoot: action\n    run: |\n      python3 ./alpha.py --go\n"
                 "  - name: write\n    manual: true\n    workingDirectoryRoot: action\n    run: |\n"
                 "      python3 ./alpha.py --write\n")
_FX_ALPHA = ('import subprocess\nimport sys\n\n\ndef main(argv):\n    if "--go" in argv:\n        return 0\n'
             '    if "--write" in argv:\n        return 0\n%s    return 2\n\n\nif __name__ == "__main__":\n'
             '    sys.exit(main(sys.argv[1:]))\n')
_FX_BETA_YML = ("# PURPOSE: a census fixture driver.\nname: beta\nsteps:\n"
                "  - name: drive\n    workingDirectoryRoot: action\n    run: |\n      python3 ./beta.py\n")
_FX_BETA = 'import subprocess\nimport sys\n%s\n\nif __name__ == "__main__":\n%s'


def _fx(alpha_extra="", beta_body="    pass\n", beta_head="", verbs=(), writers=("alpha/write",), extra=None,
        config=None):
    files = {_FX_ACTIONS + "alpha/alpha.yml": _FX_ALPHA_YML,
             _FX_ACTIONS + "alpha/alpha.py": _FX_ALPHA % alpha_extra,
             _FX_ACTIONS + "beta/beta.yml": _FX_BETA_YML,
             _FX_ACTIONS + "beta/beta.py": _FX_BETA % (beta_head, beta_body),
             CONFIG_REL.replace(os.sep, "/"): config or _FX_CONFIG,
             VERBS_REL.replace(os.sep, "/"): json.dumps({"treeWriters": list(writers), "verbs": list(verbs)})}
    files.update(extra or {})
    return files


def _rec(token, category, **kw):
    r = {"program": "alpha/alpha.py", "token": token, "category": category, "why": "a fixture"}
    r.update(kw)
    return r


def _census_case(label, files, expect=(), clean=False):
    """An arm: `files` written into a fresh tree, the census run over its actions, the verdict judged by message --
    `clean` wants no problem at all, `expect` wants each needle inside some problem."""
    root = tempfile.mkdtemp(prefix="scripts-index-census-")
    try:
        for rel, text in files.items():
            _write(os.path.join(root, *rel.split("/")), text)
        base = os.path.join(root, ACTIONS_REL)
        entries = []
        for dp, _dn, fn in os.walk(base):
            name = os.path.basename(dp)
            if name + ".yml" in fn:
                rel = os.path.relpath(dp, base).replace(os.sep, "/")
                entries.append(Entry(name, rel, os.path.join(dp, name + ".yml"),
                                     name + ".py" if name + ".py" in fn else None,
                                     sorted(f for f in fn if f.endswith(".py")), "a census fixture"))
        try:
            problems, _counts = verb_census(root, entries)
        except Collapse as exc:
            problems = ["COLLAPSE: %s" % exc]
    finally:
        shutil.rmtree(root, ignore_errors=True)
    if _RAN is not None:
        _RAN.append(label)
    missing = [w for w in expect if not any(w in p for p in problems)]
    ok = (not problems) if clean else (bool(problems) and not missing)
    why = "" if ok else ("wanted no problem, got %r" % problems[:3] if clean else
                         "never said %r; said %r" % (missing[:1], problems[:3]))
    print("scripts-index: self-test arm %-26s %s" % (label, "as expected" if ok else "FAILED (%s)" % why[:400]))
    return ok


def _census_arms():
    """Every family clause 13 verifies or refuses, each red arm beside its control."""
    ok = True
    idle = '    if "--zz-idle" in argv:\n        return 0\n'
    ok &= _census_case("22 CENSUS-GREEN", _fx(), clean=True)
    ok &= _census_case("22a UNREACHED-OPTION", _fx(idle), expect=("--zz-idle is started by nothing",))
    ok &= _census_case("22b CTEST-REACHES-IT", _fx(idle, extra={"CMakeLists.txt": (
        'add_test(NAME fx COMMAND py "${CMAKE_SOURCE_DIR}/.harness-config/runner/actions/alpha/alpha.py" '
        '--zz-idle\n    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")\n')}), clean=True)
    driven = _rec("--zz-idle", "driven", by=["beta/beta.py"])
    ok &= _census_case("22c DRIVEN", _fx(idle, beta_body='    subprocess.run([sys.executable, "alpha.py", '
                                                          '"--zz-idle"])\n', verbs=[driven]), clean=True)
    ok &= _census_case("22d DRIVEN-LITERAL-GONE", _fx(idle, beta_body='    subprocess.run([sys.executable, '
                                                                      '"alpha.py"])\n', verbs=[driven]),
                       expect=("recorded driven by beta/beta.py",))
    ok &= _census_case("22e DRIVEN-THROUGH-AN-IMPORT", _fx(
        idle, beta_head="import resolver\n", beta_body='    resolver.call(["--zz-idle"])\n', verbs=[driven],
        extra={_FX_ACTIONS + "beta/resolver.py": 'ALPHA = "alpha.py"\n\n\ndef call(args):\n'
                                                 '    return [ALPHA] + list(args)\n'}), clean=True)
    ok &= _census_case("22f FORMATTED-OPTION-PASSES", _fx(idle, beta_body='    subprocess.run([sys.executable, '
                                                                          '"alpha.py", "--zz-idle=%s" % 1])\n',
                                                          verbs=[driven]), clean=True)
    selfd = _rec("--zz-idle", "self-driven")
    ok &= _census_case("22g SELF-DRIVEN", _fx(idle + '    subprocess.run([sys.executable, __file__, '
                                                     '"--zz-idle"])\n', verbs=[selfd]), clean=True)
    ok &= _census_case("22h SELF-DRIVEN-NEVER-PASSED", _fx(idle, verbs=[selfd]),
                       expect=("never passes --zz-idle",))
    ok &= _census_case("22i A-DECLARATION-PASSES-NOTHING", _fx(
        idle + '    known = ("--go", "--write", "--zz-idle")\n    if [a for a in argv if a not in known]:\n'
               '        return 2\n', verbs=[selfd]), expect=("never passes --zz-idle",))
    gogo = '    if argv and argv[0] in ("--go", "--zz-gogo"):\n        return 0\n'
    ok &= _census_case("22j ALIAS", _fx(gogo, verbs=[_rec("--zz-gogo", "alias", of="--go")]), clean=True)
    ok &= _census_case("22k ALIAS-OF-NOTHING", _fx(gogo, verbs=[_rec("--zz-gogo", "alias", of="--nowhere")]),
                       expect=("alias of '--nowhere', which nothing reaches",))
    src = {"tests/fx_test.cpp": '// runs alpha.py --zz-idle\n'}
    ok &= _census_case("22l TEST-SOURCE", _fx(idle, verbs=[_rec("--zz-idle", "test-source",
                                                                by="tests/fx_test.cpp")], extra=src), clean=True)
    ok &= _census_case("22m TEST-SOURCE-MISSING", _fx(idle, verbs=[_rec("--zz-idle", "test-source",
                                                                        by="tests/none.cpp")]),
                       expect=("which does not exist",))
    nowhy = _rec("--zz-idle", "self-driven")
    del nowhy["why"]
    ok &= _census_case("22n RECORD-STATES-NO-WHY", _fx(idle, verbs=[nowhy]), expect=("states no `why`",))
    ok &= _census_case("22o UNKNOWN-CATEGORY", _fx(idle, verbs=[_rec("--zz-idle", "by-hand")]),
                       expect=("is not one of",))
    ok &= _census_case("22p STALE-RECORD", _fx(verbs=[_rec("--zz-gone", "self-driven")]),
                       expect=("which the program no longer dispatches on",))
    ok &= _census_case("22q RECORDED-BUT-REACHED", _fx(verbs=[_rec("--go", "self-driven")]),
                       expect=("a stale record; remove it",))
    ok &= _census_case("22r DATA-IS-NOT-A-VERB", _fx(
        '    import json\n    row = json.loads("{}")\n    if row.get("kind") == "armed":\n        return 0\n'
        '    child = ["wsl.exe", "-e"]\n    if child[0] == "wsl.exe":\n        return 0\n'), clean=True)
    ok &= _census_case("22s BARE-VERB-UNREACHED", _fx('    if argv and argv[0] == "cleanup":\n        return 0\n'),
                       expect=("cleanup is started by nothing",))
    ok &= _census_case("22t LOOP-DECLARED-OPTION", _fx(
        '    import argparse\n    p = argparse.ArgumentParser()\n    for flag in ("--zz-loop",):\n'
        '        p.add_argument(flag)\n'), expect=("--zz-loop is started by nothing",))
    ok &= _census_case("22u WRITE-TOKEN-UNDECLARED", _fx(writers=()),
                       expect=("carries a write token on its run line but is not declared a tree writer",))
    two_legs = json.loads(_FX_CONFIG)
    del two_legs["predefinedRunners"]["alpha-write"]
    ok &= _census_case("22v WRITER-WITHOUT-ONE-LEG-RUNNER", _fx(config=json.dumps(two_legs)),
                       expect=("no runner of ONE leg of this machine's own tree names it",))
    # A write into the step's OWN kept output writes no tree (2026-10-01, the P69 review's MAJOR 2): it runs on any
    # leg, needs no declaration, and a declaration of it is stale. Each half of the rule is load-bearing.
    keeps = {_FX_ACTIONS + "alpha/alpha.yml": _FX_ALPHA_YML.replace(
        "      python3 ./alpha.py --write\n",
        "      python3 ./alpha.py --write {stepBuild}\n    outputs: [made.txt]\n    persist: true\n")}
    bare = {_FX_ACTIONS + "alpha/alpha.yml": _FX_ALPHA_YML.replace(
        "      python3 ./alpha.py --write\n", "      python3 ./alpha.py --write {stepBuild}\n")}
    ok &= _census_case("22v1 KEPT-OUTPUT-WRITER-ON-ANY-LEGS", _fx(writers=(), config=json.dumps(two_legs),
                                                                  extra=keeps), clean=True)
    ok &= _census_case("22v2 STEPBUILD-WRITE-NOT-KEPT", _fx(writers=(), config=json.dumps(two_legs), extra=bare),
                       expect=("carries a write token on its run line but is not declared a tree writer",))
    ok &= _census_case("22v3 KEPT-OUTPUT-WRITER-DECLARED", _fx(extra=keeps),
                       expect=("its write goes into its own kept output",))
    ok &= _census_case("22w SCRIPT-REMEDY", _fx('    print("run %s --go")\n' % _FX_BY_HAND),
                       expect=("starts an action's program by hand",))
    ok &= _census_case("22x SCRIPT-REMEDY-IN-A-DOCSTRING", _fx(extra={_FX_ACTIONS + "alpha/alpha.py": (
        '"""Usage: %s --go"""\n' % _FX_BY_HAND + _FX_ALPHA % "")}),
        expect=("starts an action's program by hand",))
    ok &= _census_case("22w2 SCRIPT-REMEDY-WINDOWS-PATH", _fx('    print(r"run %s --go")\n' % _FX_BY_HAND.replace(
        "python3 ", "python ").replace("/", "\\")), expect=("starts an action's program by hand",))
    ok &= _census_case("22w3 SCRIPT-REMEDY-PY-LAUNCHER", _fx('    print("run %s --go")\n' % _FX_BY_HAND.replace(
        "python3 ", "py -3 ")), expect=("starts an action's program by hand",))
    ok &= _census_case("22y REMEDY-UNKNOWN-RUNNER", _fx('    print("%szz-nosuch")\n' % _FX_RUN),
                       expect=("names the runner 'zz-nosuch', which config.json does not declare",))
    ok &= _census_case("22z REMEDY-WRITER-ON-TWO-LEGS", _fx('    print("%salpha --manual-step write")\n' % _FX_RUN),
                       expect=("that runner has several legs",))
    ok &= _census_case("22z1 REMEDY-WRITER-ONE-LEG", _fx('    print("%salpha-write")\n' % _FX_RUN), clean=True)
    gamma = {_FX_ACTIONS + "gamma/gamma.yml": "# PURPOSE: a census fixture.\nname: gamma\nsteps:\n"
                                              "  - name: nothing\n    workingDirectoryRoot: action\n    run: |\n"
                                              "      python3 ./other.py\n",
             _FX_ACTIONS + "gamma/other.py": "print('x')\n",
             _FX_ACTIONS + "gamma/gamma.py": ('import subprocess\nimport sys\n\nif __name__ == "__main__":\n'
                                              '    if "--zz-child" in sys.argv:\n        sys.exit(0)\n'
                                              '    subprocess.run([sys.executable, __file__, "--zz-child"])\n')}
    childrec = {"program": "gamma/gamma.py", "token": "--zz-child", "category": "self-driven", "why": "a fixture"}
    ok &= _census_case("22z2 STARTED-BY-ITS-FILE-NAME", _fx(beta_head='GAMMA = "gamma.py"\n', verbs=[childrec],
                                                            extra=gamma), clean=True)
    ok &= _census_case("22z3 STARTED-BY-NOTHING", _fx(verbs=[childrec], extra=gamma),
                       expect=("nothing starts the program itself",))
    return ok


def selftest(root):
    tmp = tempfile.mkdtemp(prefix="scripts-index-selftest-")
    ok = True
    ran = []
    try:
        globals()["_RAN"] = ran
        _mirror(root, tmp)
        afile, repeating, twin, subject_purpose = _subject_paths(tmp)
        subject_row = "| **`" + SELFTEST_SUBJECT + "`**"
        subject_row_typo = "| **`" + SELFTEST_SUBJECT + "-TYPO`**"
        pristine = _read(afile)
        pristine_repeating = {p: _read(p) for p in repeating}
        config = os.path.join(tmp, CONFIG_REL)
        pristine_config = _read(config)
        readme = os.path.join(tmp, README_REL)
        acts = os.path.join(tmp, ACTIONS_REL)

        ok &= _arm("0 GREEN-CONTROL", tmp, EXIT_OK)

        # ── the index and the tree disagree (EXIT_DISAGREE) ──────────────────
        _newcomer(tmp, "zz-selftest-newcomer", runner=True)
        ok &= _arm("1 ACTION-NOT-IN-INDEX", tmp, EXIT_DISAGREE, says=README_REL,
                   not_says="has no runner")
        shutil.rmtree(os.path.join(acts, "zz-selftest-newcomer"))
        _write(config, pristine_config)
        ok &= _arm("1b RESTORED", tmp, EXIT_OK)

        # ★ A GROUPED action is indexed too: the scan RECURSES through group
        # directories, exactly as the tool resolves `<group>/<name>/<name>.yml`.
        _newcomer(tmp, "zz-group/zz-inner", runner=True)
        ok &= _arm("1c GROUPED-ACTION-NOT-IN-INDEX", tmp, EXIT_DISAGREE, says=README_REL,
                   not_says="has no runner")
        shutil.rmtree(os.path.join(acts, "zz-group"))
        _write(config, pristine_config)

        # ★ Moved OUT of the actions root, not renamed in place: a rename leaves a
        # directory not addressable by its own name, which trips a structural refusal
        # instead of the disagreement this arm exists to prove.
        gone = os.path.join(acts, SELFTEST_SUBJECT)
        stash = tempfile.mkdtemp(prefix="scripts-index-stash-")
        shutil.move(gone, os.path.join(stash, SELFTEST_SUBJECT))
        ok &= _arm("2 INDEX-ENTRY-NOT-AN-ACTION", tmp, EXIT_DISAGREE, says=(README_REL, WRITE_VERB))
        shutil.move(os.path.join(stash, SELFTEST_SUBJECT), gone)
        shutil.rmtree(stash, ignore_errors=True)
        ok &= _arm("2b RESTORED", tmp, EXIT_OK)

        # The purpose drifts EVERYWHERE it is declared -- the action file and every
        # program that repeats it -- so the only disagreement is with the indexes.
        _write(afile, _sabotage(pristine, PURPOSE_MARK, PURPOSE_MARK + "MUTATED -- "))
        for p, text in pristine_repeating.items():
            _write(p, _sabotage(text, PURPOSE_MARK, PURPOSE_MARK + "MUTATED -- "))
        ok &= _arm("3 PURPOSE-DRIFTED", tmp, EXIT_DISAGREE, says=README_REL)
        _write(afile, pristine)
        for p, text in pristine_repeating.items():
            _write(p, text)
        ok &= _arm("3b RESTORED", tmp, EXIT_OK)

        # ★★ EACH DOCUMENT IS CHECKED SEPARATELY. Every arm above invalidates BOTH
        # documents at once, so an implementation that verified only the first would
        # be indistinguishable from one that verified both -- ✔MEASURED by audit:
        # halving DOC_RELS left the self-test green while the skill reference
        # silently stopped being checked. Each arm asserts its own document is named
        # AND that the other one is not.
        pristine_docs = {rel: _read(os.path.join(tmp, rel)) for rel in DOC_RELS}
        for i, rel in enumerate(DOC_RELS):
            others = [o for o in DOC_RELS if o != rel]
            _write(os.path.join(tmp, rel),
                   _sabotage(pristine_docs[rel], subject_row, subject_row_typo))
            ok &= _arm("4.%d ONLY-%s-DRIFTS" % (i, os.path.basename(rel)), tmp,
                       EXIT_DISAGREE, says=rel,
                       not_says=others[0] if len(others) == 1 else None)
            _write(os.path.join(tmp, rel), pristine_docs[rel])
            ok &= _arm("4.%db RESTORED" % i, tmp, EXIT_OK)
        pristine_readme = pristine_docs[README_REL]

        # ── reachability (EXIT_DISAGREE) ─────────────────────────────────────
        # ★ ONE HALF OF CLAUSE 10 PER ARM. An action with no runner and a runner with
        # no action are two different failures; an arm producing both would prove
        # neither, so each asserts its own message and the ABSENCE of the other's.
        _newcomer(tmp, "zz-selftest-unrun", runner=False)
        ok &= _arm("5 ACTION-WITHOUT-RUNNER", tmp, EXIT_DISAGREE, says="has no runner",
                   not_says="which does not exist")
        shutil.rmtree(os.path.join(acts, "zz-selftest-unrun"))
        _add_runner(tmp, "zz-ghost", "zz-ghost/zz-ghost.yml")
        ok &= _arm("5b RUNNER-WITHOUT-ACTION", tmp, EXIT_DISAGREE,
                   says="which does not exist", not_says="has no runner")
        _write(config, pristine_config)
        ok &= _arm("5c RESTORED", tmp, EXIT_OK)

        # ── the declaration itself (EXIT_COLLAPSE) ───────────────────────────
        _write(afile, _sabotage(pristine, "# " + PURPOSE_MARK, "# (removed) "))
        ok &= _arm("6 NO-PURPOSE-DECLARED", tmp, EXIT_COLLAPSE, says="declares no")
        _write(afile, pristine)
        ok &= _arm("6b RESTORED", tmp, EXIT_OK)

        _write(afile, _sabotage(pristine, "# " + PURPOSE_MARK,
                                "# " + PURPOSE_MARK + "one.\n# " + PURPOSE_MARK))
        ok &= _arm("7 TWO-PURPOSE-LINES", tmp, EXIT_COLLAPSE, says="Exactly one is required")
        _write(afile, pristine)

        # ★ THE NEEDLE IS THE SUBJECT'S OWN PURPOSE TEXT, READ BACK OUT OF THE FILE.
        _write(afile, _sabotage(pristine, "# " + PURPOSE_MARK + subject_purpose,
                                "# " + PURPOSE_MARK + "\n# was: " + subject_purpose))
        ok &= _arm("8 EMPTY-PURPOSE", tmp, EXIT_COLLAPSE, says="EMPTY purpose")
        _write(afile, pristine)

        _write(afile, _sabotage(pristine, "# " + PURPOSE_MARK,
                                "# " + PURPOSE_MARK + "a | b "))
        ok &= _arm("9 PIPE-IN-PURPOSE", tmp, EXIT_COLLAPSE, says="raw pipe")
        _write(afile, pristine)

        _write(afile, _sabotage(pristine, "# " + PURPOSE_MARK,
                                "# " + PURPOSE_MARK + END + " "))
        ok &= _arm("10 MARKER-IN-PURPOSE", tmp, EXIT_COLLAPSE, says="never converge")
        _write(afile, pristine)
        ok &= _arm("10b RESTORED", tmp, EXIT_OK)

        # ── the layout (EXIT_COLLAPSE) ───────────────────────────────────────
        orphan = os.path.join(acts, "zz-selftest-folder")
        _write(os.path.join(orphan, "zz-selftest-folder.py"), "print('no action file')\n")
        ok &= _arm("11 NO-ACTION-FILE", tmp, EXIT_COLLAPSE, says="no action file")
        shutil.rmtree(orphan)

        # A deleted program's BYTECODE HUSK is not an action directory -- and a husk that
        # also holds one real file IS a directory that is neither action nor group. Both
        # directions, so the exemption cannot quietly widen into "any directory with a
        # __pycache__".
        husk = os.path.join(acts, "zz-selftest-husk")
        _write(os.path.join(husk, "__pycache__", "zz-selftest-husk.cpython-312.pyc"), "")
        ok &= _arm("11b BYTECODE-HUSK", tmp, EXIT_OK, not_says="holds no action")
        _write(os.path.join(husk, "notes.txt"), "not bytecode\n")
        ok &= _arm("11c HUSK-PLUS-A-FILE", tmp, EXIT_COLLAPSE, says="group directory")
        shutil.rmtree(husk)

        # A group holding only what a group MAY hold -- a placeholder -- and no action at
        # any depth. (An EMPTY directory is a husk: git tracks none, so none reaches a
        # checkout, and skipping it is the husk rule above.)
        _write(os.path.join(acts, "zz-empty-group", "deeper", ".gitkeep"), "")
        ok &= _arm("11d GROUP-WITH-NO-ACTION", tmp, EXIT_COLLAPSE, says="holds no action")
        shutil.rmtree(os.path.join(acts, "zz-empty-group"))

        loose = os.path.join(acts, "zz-loose.py")
        _write(loose, "# PURPOSE: sit where no index looks.\n")
        ok &= _arm("12 LOOSE-PROGRAM", tmp, EXIT_COLLAPSE, says="directly in the group directory")
        os.remove(loose)

        buried_dir = os.path.join(acts, SELFTEST_SUBJECT, "sub")
        _write(os.path.join(buried_dir, "sub.py"), "print('buried')\n")
        ok &= _arm("13 BURIED-PROGRAM", tmp, EXIT_COLLAPSE, says="buries program")
        shutil.rmtree(buried_dir)

        # ★ The tool's own run directories are EXEMPT from the buried rule -- a run's
        # output under `<action>/build/` is ignored and never the repository's -- and the
        # exemption is pinned so it cannot widen to any other name.
        run_dir = os.path.join(acts, SELFTEST_SUBJECT, "build", "20260918-run", "step")
        _write(os.path.join(run_dir, "out.py"), "print('run output')\n")
        ok &= _arm("13b RUN-OUTPUT-IS-NOT-BURIED", tmp, EXIT_OK, not_says="buries program")
        shutil.rmtree(os.path.join(acts, SELFTEST_SUBJECT, "build"))

        nested = os.path.join(acts, SELFTEST_SUBJECT, "inner")
        _write(os.path.join(nested, "inner.yml"), "# PURPOSE: hide.\nname: inner\n")
        ok &= _arm("13c ACTION-INSIDE-AN-ACTION", tmp, EXIT_COLLAPSE, says="INSIDE the action")
        shutil.rmtree(nested)

        # A GROUP called `artifacts` holding an action -- the path the tool refuses. The
        # action inside is what makes the directory more than an ignorable husk.
        reserved = os.path.join(acts, "artifacts")
        _newcomer(tmp, "artifacts/zz-kept", runner=True)
        ok &= _arm("13d RESERVED-GROUP-NAME", tmp, EXIT_COLLAPSE, says="gitignored")
        shutil.rmtree(reserved)
        _write(config, pristine_config)

        pristine_twin = _read(twin)
        _write(twin, "# " + PURPOSE_MARK + "something else entirely.\n" + pristine_twin)
        ok &= _arm("14 PROGRAM-CONTRADICTS", tmp, EXIT_COLLAPSE, says="differs from its action file")
        _write(twin, pristine_twin)
        ok &= _arm("14b RESTORED", tmp, EXIT_OK)

        # ★ PROSE IS NOT A DECLARATION: the same words mid-sentence in a header comment must
        # neither contradict the action nor count as a second declaration. The substring
        # grammar this replaced COLLAPSED here (the sqlite harness's own header measured it).
        _write(twin, "# kept on " + PURPOSE_MARK + "a sentence, not a declaration.\n"
               + pristine_twin)
        ok &= _arm("14c PROSE-IS-NOT-A-DECLARATION", tmp, EXIT_OK,
                   not_says="differs from its action file")
        _write(twin, pristine_twin)

        # ── the documents' structure (EXIT_COLLAPSE) ─────────────────────────
        _write(readme, pristine_readme.replace(BEGIN, "<!-- gone -->", 1))
        ok &= _arm("15 MARKERS-MISSING", tmp, EXIT_COLLAPSE, says="missing its generated-index")
        _write(readme, pristine_readme)

        _write(readme, pristine_readme + "\n" + BEGIN + "\n| **`ghost`** | `ghost.py` | an "
               "action that has never existed. |\n" + END + "\n")
        ok &= _arm("16 DUPLICATE-MARKERS", tmp, EXIT_COLLAPSE,
                   says="index markers", not_says="missing its generated-index")
        _write(readme, pristine_readme)

        os.remove(readme)
        ok &= _arm("17 INDEX-DOC-DELETED", tmp, EXIT_COLLAPSE, says="does not exist")
        _write(readme, pristine_readme)

        # ── THE FLOOR, isolated ──────────────────────────────────────────────
        # ★★★ Only DIRECTORIES move. The index document lives inside the actions root,
        # so moving it too is what let this arm pass with the floor deleted.
        # `not_says` pins that: if the document ever goes missing again, this arm
        # fails instead of quietly proving the wrong thing.
        held = tempfile.mkdtemp(prefix="scripts-index-held-")
        for name in os.listdir(acts):
            if os.path.isdir(os.path.join(acts, name)):
                shutil.move(os.path.join(acts, name), os.path.join(held, name))
        ok &= _arm("18 SCAN-COLLAPSED", tmp, EXIT_COLLAPSE,
                   says="floor is", not_says="does not exist")
        for name in os.listdir(held):
            shutil.move(os.path.join(held, name), os.path.join(acts, name))
        shutil.rmtree(held, ignore_errors=True)

        # ── the config the reachability clause reads (EXIT_COLLAPSE) ─────────
        _write(config, pristine_config + "\n}\n")
        ok &= _arm("18b CONFIG-UNREADABLE", tmp, EXIT_COLLAPSE, says="could not be read")
        _write(config, pristine_config)

        # ── the root is the tree THIS FILE lives in, whatever the caller's cwd ──
        # Arms, oracle and synthesized negatives are owned by
        # .harness-config/runner/actions/owning-tree/owning-tree.py.
        for _ok, _why, _detail in _owning_tree().root_arms(repo_root, (Collapse,), False,
                                                           __file__):
            ran.append("R " + _why)
            print("scripts-index: self-test arm %-26s %s%s"
                  % ("R " + _why, "as expected" if _ok else "FAILED",
                     "" if _ok else " (" + _detail + ")"))
            ok &= _ok

        # ── no shell and no PowerShell under the actions root (EXIT_COLLAPSE) ────────
        # ★ Clause 11, the operator's 2026-09-21 ruling made permanent. Both spellings, at
        # two depths, and the control: a run directory's output is exempt (ignored output).
        shell = os.path.join(acts, SELFTEST_SUBJECT, "zz-selftest.sh")
        _write(shell, "echo per-OS\n")
        ok &= _arm("20 SHELL-PROGRAM-REFUSED", tmp, EXIT_COLLAPSE,
                   says="shell/PowerShell program")
        os.remove(shell)
        pwsh = os.path.join(acts, SELFTEST_SUBJECT, "assets", "deep", "zz-selftest.ps1")
        _write(pwsh, "Write-Host per-OS\n")
        ok &= _arm("20b POWERSHELL-PROGRAM-REFUSED", tmp, EXIT_COLLAPSE,
                   says="zz-selftest.ps1")
        shutil.rmtree(os.path.join(acts, SELFTEST_SUBJECT, "assets"))
        run_sh = os.path.join(acts, SELFTEST_SUBJECT, "build", "20260921-run", "gen.sh")
        _write(run_sh, "echo run output\n")
        ok &= _arm("20c RUN-OUTPUT-SHELL-IS-EXEMPT", tmp, EXIT_OK,
                   not_says="shell/PowerShell program")
        shutil.rmtree(os.path.join(acts, SELFTEST_SUBJECT, "build"))

        # ── clause 12: no program loads another with bytecode writing on (EXIT_COLLAPSE) ──
        # ★ Each arm sabotages a REAL program in the mirror -- one loading by path, one through
        # sys.path, a library made a program, and one importing a module beside it -- and the
        # GREEN arms around them are the control: every other program of this tree complies.
        def _unswitch(rel):
            path = os.path.join(acts, *rel.split("/"))
            text = _read(path)
            line = [ln for ln in text.splitlines() if ln.startswith("sys.dont_write_bytecode = True")]
            _write(path, _sabotage(text, line[0] if line else "sys.dont_write_bytecode = True",
                                   "# (the bytecode switch, removed by the self-test)"))
            return path, text
        path, text = _unswitch("check-scripts-index/check-scripts-index.py")
        ok &= _arm("21 BYTECODE-ON-AT-FIRST-LOAD", tmp, EXIT_COLLAPSE,
                   says="check-scripts-index.py loads another program at line")
        _write(path, text)
        path, text = _unswitch("check-stale-blockers/check-stale-blockers.py")
        ok &= _arm("21b SYS-PATH-LOADER", tmp, EXIT_COLLAPSE, says="(sys.path.insert)")
        _write(path, text)
        lib = os.path.join(acts, "real-examples", "c", "sqlite", "sqlite_compiler.py")
        lib_text = _read(lib)
        _write(lib, lib_text + '\n\nif __name__ == "__main__":\n    pass\n')
        ok &= _arm("21c A-LIBRARY-MADE-A-PROGRAM", tmp, EXIT_COLLAPSE,
                   says="sqlite_compiler.py loads another program")
        _write(lib, lib_text)
        importer = os.path.join(acts, "real-examples", "c", "sqlite", "zz_selftest_importer.py")
        _write(importer, 'import sqlite_common\n\nif __name__ == "__main__":\n    pass\n')
        ok &= _arm("21d IMPORTS-A-MODULE-BESIDE-IT", tmp, EXIT_COLLAPSE, says="(import sqlite_common)")
        os.remove(importer)

        # ── clause 12b: the same rule across a process boundary ──
        # ★ Two REAL children, each stripped of the one thing that keeps it clean: the self-load's `-B`,
        # and the bytecode-ON control's environment that moves the cache -- the second proves the
        # environment is honoured rather than ignored. Then a LIBRARY spawning a child whose ARGUMENT is
        # built from `__file__`, and its control: the same child pointed at a temporary copy stays green.
        nabort = os.path.join(acts, "check-no-abort-in-tests", "check-no-abort-in-tests.py")
        nabort_text = _read(nabort)
        _write(nabort, _sabotage(nabort_text, "[sys.executable, '-B', '-c', driver]",
                                 "[sys.executable, '-c', driver]"))
        ok &= _arm("21e CHILD-SELF-LOAD-WITHOUT-B", tmp, EXIT_COLLAPSE,
                   says="check-no-abort-in-tests.py line")
        _write(nabort, nabort_text)
        plan = os.path.join(acts, "check-plan-citations", "check-plan-citations.py")
        plan_text = _read(plan)
        _write(plan, _sabotage(plan_text, '        env["PYTHONPYCACHEPREFIX"] = prefix\n', ""))
        ok &= _arm("21f CHILD-CACHE-MOVED-BY-ENV", tmp, EXIT_COLLAPSE, says="check-plan-citations.py line")
        _write(plan, plan_text)
        # (Both are APPENDED to a real library rather than written as a new file: a new file changes the
        # tree the index describes, and the green arm would then prove the index, not this clause.)
        body = ('\n\ndef _zz_selftest_spawn():\n    import tempfile\n'
                '    return subprocess.run([sys.executable, "-c", "import runpy, sys; runpy.run_path(sys.argv[1])",\n'
                '                           %s])\n')
        _write(lib, lib_text + body % 'os.path.join(os.path.dirname(os.path.abspath(__file__)), "x.py")')
        ok &= _arm("21g A-LIBRARY-SPAWNS-A-FILE-CHILD", tmp, EXIT_COLLAPSE, says="sqlite_compiler.py line")
        _write(lib, lib_text + body % 'os.path.join(tempfile.mkdtemp(), "copy.py")')
        ok &= _arm("21h A-CHILD-OF-A-TEMP-COPY", tmp, EXIT_OK, not_says="sqlite_compiler.py line")
        _write(lib, lib_text)

        # ── clause 13, over a synthesized fixture, then ONCE over the mirror ─────────────────────────────────
        ok &= _census_arms()
        prog = os.path.join(acts, SELFTEST_SUBJECT, SELFTEST_SUBJECT + ".py")
        pristine_prog = _read(prog)
        _write(prog, pristine_prog + '\n\nif "--zz-selftest-unreached" in sys.argv:\n    pass\n')
        ok &= _arm("23 CENSUS-WIRED", tmp, EXIT_DISAGREE, census=True,
                   says=("verb census problem", "--zz-selftest-unreached is started by nothing"))
        _write(prog, pristine_prog)

        ok &= _arm("19 GREEN-AFTER-RESTORE", tmp, EXIT_OK)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if len(ran) != EXPECTED_ARMS:
        print("scripts-index: self-test FAILED -- %d arms ran, %d expected. An arm that "
              "silently stops running is a property that silently stops being proven; if "
              "the change was deliberate, update EXPECTED_ARMS in the same commit.\n"
              "  ran: %s" % (len(ran), EXPECTED_ARMS, ", ".join(ran)))
        return EXIT_COLLAPSE
    if not ok:
        print("scripts-index: self-test FAILED -- this guard is NOT proven able to fail.")
        return EXIT_COLLAPSE
    print("scripts-index: self-test OK - %d arms exercised, every red arm asserting the "
          "MESSAGE of the refusal it names rather than merely a non-zero exit; this guard "
          "is PROVEN able to fail." % len(ran))
    return EXIT_OK


def main(argv):
    write = "--write" in argv[1:]
    self_ = "--selftest" in argv[1:]
    unknown = [a for a in argv[1:] if a not in ("--write", "--selftest")]
    if unknown:
        print("check-scripts-index: unknown argument(s): %s" % " ".join(unknown))
        print(__doc__.rsplit("Usage:", 1)[-1].strip())
        return EXIT_USAGE
    try:
        root = repo_root()
        if self_:
            return selftest(root)
        if write:
            return run(root, write=True)
        # ★★ THE NO-ARGUMENT FORM — the one ctest uses — VERIFIES THE REAL TREE
        # AND THEN PROVES IT CAN FAIL, in that order. Same shape as
        # check-orphan-tests, and for the same reason: an entry that only
        # verified would pass identically if every check inside it had been
        # commented out, so the ctest run itself has to witness the red arms.
        rc = run(root, write=False)
        if rc != EXIT_OK:
            return rc
        return selftest(root)
    except Collapse as exc:
        print("check-scripts-index: FAIL (structural) -- %s" % exc)
        print("  This does NOT mean the index is clean - it means the SCAN COLLAPSED.")
        print("  Refusing to report a pass; fix the scan, do not lower the floor.")
        return EXIT_COLLAPSE


if __name__ == "__main__":
    sys.exit(main(sys.argv))
