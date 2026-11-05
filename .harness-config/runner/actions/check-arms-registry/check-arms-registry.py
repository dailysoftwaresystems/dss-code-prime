#!/usr/bin/env python3
# PURPOSE: check what the mutation arms registry promises that `dssharness check-mutations` does not: every row stands in the tree, each section is in order, one case count per (runner, leg), and a sweep's reds and diagnostics against its rows.
"""Check the mutation arms registry (tests/mutations/arms.registry) for what the TOOL does not check.

`dssharness check-mutations` reads the registry's grammar before it touches a host (every problem listed, exit 12) and,
when it sweeps an arm, judges what reddened against the arm's rows and whether its diagnostic was said. Nothing of
that is repeated here. What the tool does NOT check, and every hand-back of an arm rests on, is checked here:

  stand     every A and M row's before-text stands EXACTLY ONCE in its site as the tree holds it now, and differs
            from its after-text -- a row that no longer stands is found here, before a sweep, never by one;
  order     the arm blocks are grouped by area (the first directory of the A row's site under src/, `tests` for a
            site outside src/), the areas in alphabetical order, the arms sorted within each;
  cases     the arms that run ONE runner on ONE leg (their S row's legs) state ONE `cases` -- a binary runs one
            number of cases on a leg; two counts there mean one of them is wrong. (A binary's count may differ by
            leg -- a case compiled only on one host -- so counts on different legs are not compared.)
  sweep     given a sweep's JSON (`check-mutations --json`), for every arm the sweep drove: the cases it read red
            beside the C rows (red, not declared; declared, not red), a G row's case red, and whether the arm's
            diagnostic text stands in its run log (its `records` directory, read in this tree -- bring a remote
            leg's back with `dssharness sync --legs <leg> --pull <records>/run.log`). A report for rewriting an
            arm's rows FROM the tool's record: it re-judges nothing the tool's verdict says.

Usage:
  check-arms-registry.py                      verify the registry of the tree this file lives in (stand, order,
                                              cases), then self-test
  check-arms-registry.py --sweep <json> ...   the sweep report, for each JSON document given
  check-arms-registry.py --self-test          every check against a synthesized negative, and a control

The self-test prints one verdict line per arm, `check-arms-registry self-test: ok      <arm>` or
`check-arms-registry self-test: FAILED  <arm> -- <what>`; the ctest entry `harness/arms_registry_selftest` reads each
into a case of its own. The tree is judged by the entry `arms_registry_guard` (the top-level CMakeLists).

Exit: 0 clean, 1 a problem found (each listed), 2 usage or an unreadable input.
Python 3.9+, the standard library only.
"""
import argparse
import collections
import importlib.util
import json
import os
import sys
import tempfile

# The program loads owning-tree by path: no `__pycache__` is written beside it, inside an action's directory.
sys.dont_write_bytecode = True

# Both streams speak UTF-8 from import on: a row's text, a path or a test name may hold a character outside the
# console's code page, and a pipe on Windows would otherwise lose or mangle it.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

TOOL = "check-arms-registry"
SELFTEST_MARKER = TOOL + " self-test: "
# The self-test's own ratchet: an arm lost (or one added without raising this) is a FAIL no arm reports.
EXPECTED_ARMS = 17
REGISTRY = os.path.join("tests", "mutations", "arms.registry")


def tree_of_this_script():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "..", ".."))


def owning_tree():
    """`owning-tree`, loaded BY PATH (a hyphen is not a module name): the kind of every value a command line names
    is its `*_arg` -- never a local respelling of the rule. Missing, it is a refusal naming what lives there."""
    name = "check_arms_registry_owning_tree"
    if name in sys.modules:
        return sys.modules[name]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "owning-tree", "owning-tree.py")
    if not os.path.isfile(path):
        raise SystemExit("%s: cannot find %s -- the kind of a value a command line names is decided there" % (TOOL, path))
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    held, sys.argv = sys.argv, [path]
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.argv = held
    sys.modules[name] = mod
    return mod


class Unreadable(Exception):
    pass


def read_bytes_text(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8")
    except (OSError, UnicodeDecodeError) as e:
        raise Unreadable("%s: %s" % (path, e))


def text_as_the_tool_reads(path, site_text):
    """A text file as the tool reads it: less a UTF-8 BOM at its start and ONE line ending at its end, and its lines
    ended as the site ends them (a CRLF site reads an LF text's line endings as CRLF)."""
    t = read_bytes_text(path)
    if t.startswith("﻿"):
        t = t[1:]
    if t.endswith("\r\n"):
        t = t[:-2]
    elif t.endswith("\n"):
        t = t[:-1]
    if "\r\n" in site_text and "\r\n" not in t:
        t = t.replace("\n", "\r\n")
    elif "\r\n" not in site_text and "\r\n" in t:
        t = t.replace("\r\n", "\n")
    return t


def rows_of(registry_text):
    """(line number, kind, fields) for each row: fields split on '|' and trimmed, the last taking the rest."""
    widths = {"A": 11, "C": 4, "G": 4, "B": 5, "M": 6, "S": 4}
    out = []
    for n, line in enumerate(registry_text.split("\n"), 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        kind = s.split("|", 1)[0].strip()
        if kind not in widths:
            continue   # the tool refuses an unknown kind; nothing to add here
        parts = [p.strip() for p in s.split("|", widths[kind] - 1)]
        out.append((n, kind, parts))
    return out


def area_of(site):
    parts = site.split("/")
    return parts[1] if parts[0] == "src" and len(parts) > 2 else "tests"


def check_stand(tree, rows):
    problems = []
    for n, kind, f in rows:
        if kind not in ("A", "M") or len(f) < 5:
            continue
        arm, site, before, after = f[1], f[2], f[3], f[4]
        try:
            site_text = read_bytes_text(os.path.join(tree, *site.split("/")))
        except Unreadable:
            problems.append("line %d: arm %s: its site %s cannot be read" % (n, arm, site))
            continue
        try:
            b = text_as_the_tool_reads(os.path.join(tree, *before.split("/")), site_text)
            a = text_as_the_tool_reads(os.path.join(tree, *after.split("/")), site_text)
        except Unreadable as e:
            problems.append("line %d: arm %s: a text it cites cannot be read (%s)" % (n, arm, e))
            continue
        count = site_text.count(b) if b else 0
        if count != 1:
            problems.append("line %d: arm %s: its before-text (%s) stands %d time(s) in %s, not once"
                            % (n, arm, before, count, site))
        if a == b:
            problems.append("line %d: arm %s: its after-text (%s) is its before-text: the mutant changes nothing"
                            % (n, arm, after))
    return problems


def check_order(rows):
    arms = [(n, f[1], area_of(f[2])) for n, kind, f in rows if kind == "A" and len(f) >= 3]
    problems = []
    seen = []
    for i, (n, arm, area) in enumerate(arms):
        if not seen or seen[-1] != area:
            if area in seen:
                problems.append("line %d: arm %s (area %s) stands after section %s: its section is earlier"
                                % (n, arm, area, seen[-1]))
                continue
            seen.append(area)
        elif arms[i - 1][1] > arm:
            problems.append("line %d: arm %s stands after %s in section %s: out of order" % (n, arm, arms[i - 1][1], area))
    if seen != sorted(seen):
        problems.append("the sections are not in alphabetical order: %s" % ", ".join(seen))
    return problems


def check_cases(rows):
    legs = {}
    for n, kind, f in rows:
        if kind == "S" and len(f) >= 3:
            legs[f[1]] = [x.strip() for x in f[2].replace(" ", ",").split(",") if x.strip()]
    by = collections.defaultdict(lambda: collections.defaultdict(list))
    for n, kind, f in rows:
        if kind != "A" or len(f) < 9 or f[7] == "-":
            continue
        for leg in legs.get(f[1], ["<every leg>"]):
            by[(f[7], leg)][f[8]].append((n, f[1]))
    problems = []
    for (runner, leg), counts in sorted(by.items()):
        if len(counts) > 1:
            problems.append("runner %s on %s: %s" % (runner, leg, "; ".join(
                "cases %s by %d arm(s), the first %s (line %d)" % (c, len(a), a[0][1], a[0][0])
                for c, a in sorted(counts.items()))))
    return problems


def verify(tree):
    reg = os.path.join(tree, REGISTRY)
    rows = rows_of(read_bytes_text(reg))
    found = [("stand", p) for p in check_stand(tree, rows)]
    found += [("order", p) for p in check_order(rows)]
    found += [("cases", p) for p in check_cases(rows)]
    arms = sum(1 for _, k, _ in rows if k == "A")
    return found, arms, len(rows)


def records_in_tree(tree, records):
    """A sweep's `records` directory, as this tree holds it: the part from `.harness-config/` on."""
    r = records.replace("\\", "/")
    at = r.find(".harness-config/")
    return os.path.join(tree, *r[at:].split("/")) if at >= 0 else None


def sweep_report(tree, rows, doc):
    a_rows = {f[1]: f for _, k, f in rows if k == "A" and len(f) >= 10}
    guards = collections.defaultdict(set)
    for _, k, f in rows:
        if k == "G" and len(f) >= 3:
            guards[f[1]].add(f[2])
    problems, lines = [], []
    for leg in doc.get("legs", []):
        for arm in leg.get("arms", []):
            if "reds" not in arm:
                continue   # not driven: nothing read
            name, legname = arm.get("arm", "?"), leg.get("leg", "?")
            reds, declared = set(arm.get("reds") or []), set(arm.get("declaredReds") or [])
            extra, missing = sorted(reds - declared), sorted(declared - reds)
            guard_red = sorted(reds & guards.get(name, set()))
            said = "no run log here"
            row = a_rows.get(name)
            folder = records_in_tree(tree, arm.get("records", "")) if arm.get("records") else None
            if row is None:
                said = "no A row declares it in this registry"
            elif folder and os.path.isfile(os.path.join(folder, "run.log")):
                log = read_bytes_text(os.path.join(folder, "run.log"))
                diag = text_as_the_tool_reads(os.path.join(tree, *row[9].split("/")), log)
                said = "its diagnostic stands in its run" if diag in log else "its diagnostic is NOT in its run"
            bad = extra or missing or guard_red or said.endswith("NOT in its run")
            line = "%s %s on %s (%s): ran %s, declares %s; red-not-declared %s; declared-not-red %s; G red %s; %s" % (
                "BAD" if bad else "ok ", name, legname, arm.get("verdict"), arm.get("cases"), arm.get("declaredCases"),
                extra or "none", missing or "none", guard_red or "none", said)
            lines.append(line)
            if bad:
                problems.append(line)
    return problems, lines


# ── THE SELF-TEST: each check against a synthesized negative, beside a control that must stay clean ─────────────

def _write(root, rel, text):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)


def _fixture(root, registry_lines, site_text="int f() { return 1; }\n"):
    _write(root, "src/core/x.cpp", site_text)
    _write(root, "tests/mutations/texts/a.before", "return 1;\n")
    _write(root, "tests/mutations/texts/a.after", "return 2;\n")
    _write(root, "tests/mutations/texts/a.diag", "Expected equality\n")
    _write(root, REGISTRY, "# a fixture\n" + "".join(line + "\n" for line in registry_lines))


def _a(arm, site="src/core/x.cpp", runner="bin", cases="5", after="tests/mutations/texts/a.after"):
    return ("A | %s | %s | tests/mutations/texts/a.before | %s | TEST-RED | lib | %s | %s | "
            "tests/mutations/texts/a.diag | why" % (arm, site, after, runner, cases))


def self_test():
    ran = []   # (label, ok, what it found)

    def arm(label, registry_lines, check, expect_problem, site_text="int f() { return 1; }\n"):
        with tempfile.TemporaryDirectory(prefix="arms-registry-selftest-") as root:
            _fixture(root, registry_lines, site_text)
            rows = rows_of(read_bytes_text(os.path.join(root, REGISTRY)))
            got = {"stand": lambda: check_stand(root, rows), "order": lambda: check_order(rows),
                   "cases": lambda: check_cases(rows)}[check]()
            ran.append((label, bool(got) == expect_problem, got, expect_problem))

    control = [_a("core-a"), "C | core-a | S.T | why", "S | core-a | macos-arm64-debug | why",
               _a("core-b"), "C | core-b | S.U | why", "S | core-b | macos-arm64-debug | why"]
    arm("R1 the control registry, standing, in order, one count per runner and leg: the stand check finds nothing",
        control, "stand", False)
    arm("R2 the control registry: the order check finds nothing", control, "order", False)
    arm("R3 the control registry: the cases check finds nothing", control, "cases", False)
    arm("S1 a before-text the site does not hold is refused", control, "stand", True,
        site_text="int f() { return 3; }\n")
    arm("S2 a before-text the site holds twice is refused", control, "stand", True,
        site_text="int f() { return 1; }\nint g() { return 1; }\n")
    arm("S3 an after-text that is the before-text is refused: the mutant changes nothing",
        [_a("core-a", after="tests/mutations/texts/a.before"), "C | core-a | S.T | why"], "stand", True)
    arm("S4 a CRLF site still reads an LF text once", control, "stand", False,
        site_text="int f() {\r\n return 1; }\r\n")
    arm("O1 an arm out of order in its section is refused", [_a("core-b"), _a("core-a")], "order", True)
    arm("O2 an arm in a section already closed is refused",
        [_a("core-a"), _a("tests-a", site="tests/x.cpp"), _a("core-b")], "order", True)
    arm("O3 sections out of alphabetical order are refused",
        [_a("tests-a", site="tests/x.cpp"), _a("core-a")], "order", True)
    arm("C1 two case counts for one runner on one leg are refused",
        [_a("core-a", cases="5"), "S | core-a | macos-arm64-debug | why",
         _a("core-b", cases="6"), "S | core-b | macos-arm64-debug | why"], "cases", True)
    arm("C2 two case counts for one runner on two legs are not compared",
        [_a("core-a", cases="5"), "S | core-a | macos-arm64-debug | why",
         _a("core-b", cases="6"), "S | core-b | windows-x86_64-debug | why"], "cases", False)

    # The sweep report: a synthesized JSON and run log per negative.
    def sweep_arm(label, entry, run_log, expect_problem, g_rows=()):
        with tempfile.TemporaryDirectory(prefix="arms-registry-selftest-") as root:
            _fixture(root, control + list(g_rows))
            rows = rows_of(read_bytes_text(os.path.join(root, REGISTRY)))
            rec = ".harness-config/runs/r/macos-arm64-debug/arms/core-a"
            _write(root, rec + "/run.log", run_log)
            base = {"arm": "core-a", "verdict": "x", "cases": 5, "declaredCases": 5,
                    "declaredReds": ["S.T"], "records": "~/src/copy/" + rec}
            base.update(entry)
            got, _ = sweep_report(root, rows, {"legs": [{"leg": "macos-arm64-debug", "arms": [base]}]})
            ran.append((label, bool(got) == expect_problem, got, expect_problem))

    said = "[  FAILED  ] S.T\nExpected equality of these values\n"
    sweep_arm("W1 the sweep control, the declared red with its diagnostic said, is clean", {"reds": ["S.T"]}, said,
              False)
    sweep_arm("W2 a red no C row declares is reported", {"reds": ["S.T", "S.V"]}, said, True)
    sweep_arm("W3 a declared red that did not red is reported", {"reds": []}, said, True)
    sweep_arm("W4 a G row's case red is reported", {"reds": ["S.T", "S.W"], "declaredReds": ["S.T", "S.W"]}, said,
              True, g_rows=["G | core-a | S.W | why"])
    sweep_arm("W5 the diagnostic not in the arm's run is reported", {"reds": ["S.T"]},
              "[  FAILED  ] S.T\nsomething else\n", True)

    failed = 0
    for label, ok, got, expect_problem in ran:
        if ok:
            print("%sok      %s" % (SELFTEST_MARKER, label))
        else:
            failed += 1
            what = ("found nothing, where a problem was synthesized" if expect_problem
                    else "found %r on a fixture that holds none" % (got,))
            print("%sFAILED  %s -- %s" % (SELFTEST_MARKER, label, what.replace("\n", " ")[:600]))
    if len(ran) != EXPECTED_ARMS:
        print("%s%d arm(s) ran, %d expected -- FAIL" % (SELFTEST_MARKER, len(ran), EXPECTED_ARMS))
        return 1
    print("%s%d arm(s), %d failed%s" % (SELFTEST_MARKER, len(ran), failed, "" if not failed else " -- FAIL"))
    if failed:
        print("%s: self-test FAILED (%d of %d arms)" % (TOOL, failed, len(ran)))
        return 1
    print("%s: self-test OK (%d arms)" % (TOOL, len(ran)))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sweep", nargs="+", default=None, metavar="JSON", type=owning_tree().read_path_arg("--sweep"))
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    tree = tree_of_this_script()
    try:
        if a.sweep:
            rows = rows_of(read_bytes_text(os.path.join(tree, REGISTRY)))
            problems = []
            for path in a.sweep:
                doc = json.loads(read_bytes_text(path))
                got, lines = sweep_report(tree, rows, doc)
                for line in lines:
                    print("%s: %s" % (TOOL, line))
                problems += got
            print("%s: %s (%d arm(s) not as their rows say)" % (TOOL, "OK" if not problems else "PROBLEMS",
                                                                len(problems)))
            return 1 if problems else 0
        found, arms, nrows = verify(tree)
    except (Unreadable, json.JSONDecodeError) as e:
        print("%s: cannot read an input: %s" % (TOOL, e), file=sys.stderr)
        return 2
    for check, p in found:
        print("%s: %s: %s" % (TOOL, check, p))
    if found:
        print("%s: PROBLEMS (%d) over %d arm(s), %d row(s)" % (TOOL, len(found), arms, nrows))
        return 1
    print("%s: OK (%d arm(s), %d row(s): every row stands, each section in order, one case count per runner and leg)"
          % (TOOL, arms, nrows))
    return self_test()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
