#!/usr/bin/env python3
# PURPOSE: refuse a configured build in which an entry that SELF-TESTS a program carries the `repo-guard` label -- a self-test's verdict reads its own fixtures, which can depend on the host, so every leg runs it.
"""check-repo-guard-label.py -- WHICH CTEST ENTRIES MAY CARRY `repo-guard`.

THE RULE is stated once, where the label is defined: the root CMakeLists.txt, in the block that gives it. The
label lets an indirect leg skip an entry whose verdict reads the REPOSITORY'S STATE -- the same bytes on every
leg. An entry that SELF-TESTS a program reads its own fixtures instead, and that verdict CAN depend on the host:
case, path separators, path spelling, the host's own git. So a self-test entry never carries the label, and every
leg, the indirect ones included, runs it. This program holds a configured build's registrations to that rule.

WHAT IT READS: `ctest --show-only=json-v1` in the build directory -- what ctest will actually run, each entry's
command line and labels -- never a CMakeLists text, which says what a configure MIGHT register.

AN ENTRY SELF-TESTS a program when
  * a word of its command line after the program is a SELF-TEST FLAG: `--self-test` or `--selftest`, alone or
    followed by `-<word>` (`--selftest-watchdog`); a word that merely begins so (`--self-tests`) is none; or
  * its name holds `selftest`: the name of every entry that drives a program's self-test one case per arm
    (`<x>_selftest_guard`, `harness/<x>_selftest`), whose command line is the test binary alone.
An entry that checks the tree -- a guard run with no flag, even one that proves its own refusals after -- keeps
the label: its verdict is the repository's.

FAIL LOUD: ctest failing, a listing that is not ctest's JSON, a listing with fewer than FLOOR entries, or one in
which no entry carries the label or none self-tests (the rule would judge nothing) is a refusal to judge -- never
a pass.

usage:
  check-repo-guard-label.py --build-dir <dir> --ctest <ctest>    judge a configured build (ctest entry
                                                                   `harness/repo_guard_label_check`)
  check-repo-guard-label.py --self-test                            prove each refusal on synthetic listings, one
                                                                   verdict line per arm (`self-test` step; ctest
                                                                   entry `harness/repo_guard_label_selftest`)
exit: 0 OK . 1 an entry refused . 2 cannot judge . 3 usage
"""
import json
import os
import re
import subprocess
import sys

# A cp1252 console turns a printed glyph into a traceback; reconfigure before anything
# can print (the property `guard_output_encoding_guard` ratchets for every script).
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):  # pragma: no cover - an odd stream
        pass

LABEL = "repo-guard"
SELF_TEST_FLAG = re.compile(r"^--(?:self-test|selftest)(?:-[a-z0-9]+(?:-[a-z0-9]+)*)?$")
FLOOR = 50
TOOL = "check-repo-guard-label"
SELFTEST_MARKER = TOOL + " self-test: "
# ⚠ Raising this is the claim that the arms you added actually RUN.
EXPECTED_ARMS = 8


class CannotJudge(Exception):
    """The listing cannot be judged: never reported as a pass."""


def labels_of(entry):
    for prop in entry.get("properties") or ():
        if prop.get("name") == "LABELS":
            value = prop.get("value")
            return list(value) if isinstance(value, list) else [value]
    return []


def self_test_why(entry):
    """Why the entry self-tests a program, or None."""
    command = entry.get("command") or []
    flags = [w for w in command[1:] if isinstance(w, str) and SELF_TEST_FLAG.match(w)]
    if flags:
        return "its command line holds %s" % flags[0]
    if "selftest" in (entry.get("name") or ""):
        return "its name holds `selftest`"
    return None


def judge(listing):
    """-> (refusals, counts). CannotJudge when the listing cannot say anything."""
    if not isinstance(listing, dict) or not isinstance(listing.get("tests"), list):
        raise CannotJudge("the listing is not ctest's JSON (no `tests` list)")
    tests = listing["tests"]
    if len(tests) < FLOOR:
        raise CannotJudge("ctest listed %d entries, fewer than %d: nothing below would be evidence" % (len(tests), FLOOR))
    labelled = [t for t in tests if LABEL in labels_of(t)]
    selftests = [t for t in tests if self_test_why(t)]
    if not labelled:
        raise CannotJudge("no entry carries `%s`: the rule has nothing to judge" % LABEL)
    if not selftests:
        raise CannotJudge("no entry self-tests a program: the rule has nothing to judge")
    refusals = ["%s: %s, and it carries `%s` -- a self-test runs on every leg" % (t.get("name"), self_test_why(t), LABEL)
                for t in labelled if self_test_why(t)]
    return refusals, (len(tests), len(labelled), len(selftests))


def read_listing(build_dir, ctest, run=subprocess.run):
    try:
        p = run([ctest, "--show-only=json-v1"], cwd=build_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=300)
    except (OSError, subprocess.SubprocessError) as exc:
        raise CannotJudge("ctest could not be run (%s)" % exc)
    if p.returncode != 0:
        raise CannotJudge("ctest --show-only=json-v1 exited %d: %s" % (
            p.returncode, (p.stderr or b"").decode("utf-8", "replace").strip()[:300]))
    try:
        return json.loads((p.stdout or b"").decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise CannotJudge("ctest's listing is not JSON (%s)" % exc)


def verify(build_dir, ctest, run=subprocess.run, out=None):
    """Judge a configured build: 0 OK, 1 refused, 2 cannot judge. Prints its verdict to `out`."""
    out = out or sys.stdout
    try:
        refusals, (total, labelled, selftests) = judge(read_listing(build_dir, ctest, run))
    except CannotJudge as exc:
        out.write("%s: CANNOT JUDGE -- %s\n" % (TOOL, exc))
        return 2
    if refusals:
        out.write("%s: FAIL -- %d entr%s self-test%s a program and carr%s `%s`:\n" % (
            TOOL, len(refusals), "y" if len(refusals) == 1 else "ies", "s" if len(refusals) == 1 else "",
            "ies" if len(refusals) == 1 else "y", LABEL))
        for r in refusals:
            out.write("    %s\n" % r)
        out.write("  Take the label off where the entry is registered; the rule is stated in the root "
                  "CMakeLists.txt, in the block that gives the label.\n")
        return 1
    out.write("%s: OK -- %d entries, %d carry `%s`, %d self-test a program, none both\n" % (
        TOOL, total, labelled, LABEL, selftests))
    return 0


# ── THE SELF-TEST: synthetic listings, no build ──────────────────────────────────────────────────────────────
def _entry(name, command, labels=()):
    props = [{"name": "LABELS", "value": list(labels)}] if labels else []
    return {"name": name, "command": list(command), "properties": props}


def _listing(*extra, filler=FLOOR):
    tests = [_entry("filler/test_%d" % i, ["bin/filler_%d" % i]) for i in range(filler)]
    tests.append(_entry("anchor_registry_guard", ["python", "check-anchor-registry.py"], [LABEL]))
    tests.append(_entry("harness/redact_selftest", ["bin/dss_harness_redact_selftest"]))
    tests.append(_entry("harness/x_check", ["python", "x.py", "--self-test"]))
    return {"kind": "ctestInfo", "tests": tests + list(extra)}


class _Ran:
    def __init__(self, rc, stdout, stderr=b""):
        self.returncode, self.stdout, self.stderr = rc, stdout, stderr


def self_test():
    import io
    ran = []

    def arm(label, ok, detail=""):
        ran.append(bool(ok))
        if ok:
            print("%sok      %s" % (SELFTEST_MARKER, label))
        else:
            print("%sFAILED  %s -- %s" % (SELFTEST_MARKER, label, (detail or "no detail").replace("\n", " ")[:600]))

    def refused(listing, name):
        try:
            refusals, _counts = judge(listing)
        except CannotJudge as exc:
            return False, "cannot judge: %s" % exc
        return any(r.startswith(name + ":") for r in refusals), "refusals=%r" % (refusals,)

    def kept(listing):
        try:
            refusals, _counts = judge(listing)
        except CannotJudge as exc:
            return False, "cannot judge: %s" % exc
        return not refusals, "refusals=%r" % (refusals,)

    def cannot(listing):
        try:
            judge(listing)
        except CannotJudge as exc:
            return True, str(exc)
        return False, "judged"

    arm("a self-test flag on the command line makes an entry a self-test, whatever its name: refused with the label",
        *refused(_listing(_entry("landing_log_git_environment_guard", ["python", "refresh_landing_log.py",
                                                                         "--self-test"], [LABEL])),
                 "landing_log_git_environment_guard"))
    said = [refused(_listing(_entry("x_guard", ["python", "x.py", flag], [LABEL])), "x_guard")
            for flag in ("--selftest", "--selftest-watchdog", "--self-test-arms")]
    arm("--selftest, and either flag followed by -<word>, are self-test flags too",
        all(ok for ok, _ in said), "%r" % ([d for _, d in said],))
    arm("a name holding selftest makes a driver entry a self-test: refused with the label",
        *refused(_listing(_entry("owning_tree_selftest_guard", ["bin/dss_owning_tree_selftest_guard"], [LABEL])),
                 "owning_tree_selftest_guard"))
    ok1, d1 = kept(_listing(_entry("orphan_tests_guard", ["bin/dss_orphan_tests_guard"], [LABEL]),
                            _entry("y_guard", ["python", "y.py", "--self-tests", "--no-selftest"], [LABEL]),
                            _entry("z_guard", ["--selftest"], [LABEL]),
                            _entry("w_check", ["python", "w.py", "--self-test"])))
    arm("a tree check keeps the label, a word that only begins like a flag is none, the program's own path is not "
        "read, and a self-test without the label passes (the control)", ok1, d1)
    said = [cannot({"tests": "no"}), cannot(_listing(filler=FLOOR - 4)),
            cannot({"tests": [t for t in _listing()["tests"] if LABEL not in labels_of(t)]}),
            cannot({"tests": [t for t in _listing()["tests"] if not self_test_why(t)]})]
    arm("a listing that is no listing, too short, without the label or without a self-test is refused, never judged",
        all(ok for ok, _ in said), "%r" % ([d for _, d in said],))

    def verify_with(result):
        buf = io.StringIO()
        rc = verify("build-dir", "ctest", run=lambda *a, **k: result, out=buf)
        return rc, buf.getvalue()

    bad = _listing(_entry("anchors_selftest_guard", ["python", "anchors.py", "--self-test"], [LABEL]))
    rc, said = verify_with(_Ran(0, json.dumps(bad).encode("utf-8")))
    arm("the build's listing is read through ctest and judged: a labelled self-test is a FAIL (exit 1) naming it",
        rc == 1 and "anchors_selftest_guard:" in said, "exit %d: %s" % (rc, said[:300]))
    results = [verify_with(_Ran(1, json.dumps(_listing()).encode("utf-8"), b"ctest: interrupted")),
               verify_with(_Ran(0, b"not json"))]
    arm("ctest failing, or a listing that is not JSON, cannot be judged (exit 2), never a pass",
        all(rc == 2 and "CANNOT JUDGE" in s for rc, s in results), "%r" % (results,))
    rc, said = verify_with(_Ran(0, json.dumps(_listing()).encode("utf-8")))
    arm("a build whose self-tests carry no label is OK (exit 0) and says what it counted (the control)",
        rc == 0 and ("%d entries" % (FLOOR + 3)) in said, "exit %d: %s" % (rc, said[:300]))

    failed = ran.count(False)
    if len(ran) != EXPECTED_ARMS:
        print("%s%d arm(s) ran, %d expected -- FAIL" % (SELFTEST_MARKER, len(ran), EXPECTED_ARMS))
        return 1
    print("%s%d arm(s), %d failed%s" % (SELFTEST_MARKER, len(ran), failed, "" if not failed else " -- FAIL"))
    return 0 if not failed else 1


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) == 4 and argv[0] == "--build-dir" and argv[2] == "--ctest" and argv[1] and argv[3]:
        if not os.path.isdir(argv[1]):
            print("%s: CANNOT JUDGE -- the build directory %s does not exist" % (TOOL, argv[1]))
            return 2
        return verify(argv[1], argv[3])
    print("%s: USAGE -- %s --build-dir <dir> --ctest <ctest> | --self-test" % (TOOL, TOOL), file=sys.stderr)
    return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
