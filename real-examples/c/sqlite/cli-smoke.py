#!/usr/bin/env python3
"""FAIL-LOUD smoke gate for a built `sqlite3` CLI, ATTRIBUTED against a reference.

★ ONE IMPLEMENTATION, BOTH DRIVERS. build-and-test.sh and build-and-test.ps1
both call this file. The assertions could have been written twice — once in bash
and once in PowerShell — and that is exactly how "a capability in one driver and
not the other" gets manufactured; TF-C117 already established the answer (logic
that can be shared goes into the PYTHON, not duplicated into two shells). Fourteen
assertions kept in step by hand across two languages would have drifted on the
first edit.

★ WHY A SMOKE GATE AT ALL, WHEN THE UNIT CORPUS EXISTS. The corpus runs through
`testfixture`, which is a Tcl interpreter that links the sqlite LIBRARY. It never
executes `shell.c`. So every CLI-only surface — argv handling, the dot-commands,
the `.dump` writer, the startup version guard, stdio behaviour — is covered by
NOTHING until this runs. `D-SQLITE-CLI-BUILT-ON-NO-LEG`.

★ EVERY ASSERTION IS ATTRIBUTED. Each one is executed twice: once against the
DSS-built CLI and once against a gcc-built REFERENCE CLI — upstream's own
`make sqlite3d`, i.e. the SAME target built from the SAME translation units in
the SAME staged tree by upstream's own unmodified rule.

  What that holds constant: the sources, the tree, the include dirs.
  What it does NOT: the compiler (the thing under test) and, on shell.c only,
  `SQLITE_CORE`. ✔MEASURED 2026-08-05: upstream compiles the library objects
  with 8 defines INCLUDING `SQLITE_CORE` and shell.c with 18 that do not
  (main.mk:2160-2166), and the reference gets that split because it IS
  upstream's make. DSS builds one program from one defines array, so it gets
  the UNION. "The only variable is the compiler" is therefore not true and is
  not claimed; a difference landing on that seam is a real result this oracle
  cannot attribute, which is worth knowing rather than glossing.

The verdict per assertion is therefore:

    DSS pass                     -> pass
    DSS fail  + reference fail   -> upstream-or-environment   (NOT charged to DSS)
    DSS fail  + reference pass   -> DSS                       (a real failure)
    DSS pass  + reference fail   -> pass, reference-only fault reported
    DSS fail  + NO reference     -> unattributed              (charged, loudly)

The last row is the important one and it is deliberately NOT a free pass. An
oracle whose limits are not stated is a trap; an ABSENT oracle that silently
downgrades a failure to a warning is worse. A DSS failure with nothing to compare
against fails the gate and says why.

★★ ATTRIBUTION IS NOT ABSOLUTION — AND THIS GATE EXITS NON-ZERO EITHER WAY.
`upstream-or-environment` says WHO is at fault; it does not say the gate is
green. ✔MEASURED 2026-08-05 while red-on-disable testing this very file: passing
a deliberately WRONG `--expect-version 9.9.9` made both binaries fail assertion
2, which the first draft attributed as `upstream-or-environment` and then
reported as **PASS, rc 0** — an instrument that answered "fine" to a question it
had just got wrong. Three of these exit codes exist so a caller can tell the
cases apart without either of them ever reading as success:

    0  every assertion passed on the DSS binary.
    1  RED, CHARGED TO DSS      — at least one DSS-only failure, or a failure
                                  with no reference to attribute it against.
    3  RED, NOT DSS             — everything that failed also failed on gcc.
                                  The CLI leg is red for an upstream/environment
                                  reason and DSS is explicitly not implicated.

A caller that wants "did DSS break?" tests for 1. A caller that wants "is this
leg trustworthy?" tests for non-zero. Neither question can be answered wrongly by
accident, and "0 files / 0 usable assertions is a pass" is unreachable.

─────────────────────────────────────────────────────────────────────────────
PATHS, LAUNCHERS, AND WHY EVERY FILE ARGUMENT IS RELATIVE

A leg may run through a LAUNCHER (`wsl.exe -e`, `qemu-aarch64`, `wine`, `arch
-x86_64`) whose filesystem namespace is not the driver's. Rather than plumb a
path translator in here — a second copy of a decision harness_legs.py already
owns — this gate NEVER passes an absolute path to the binary under test. It sets
the child's WORKING DIRECTORY and passes bare relative names (`smoke.db`).

That is correct for every declared translation, not a lucky coincidence:
`qemu-*`, `wine` and `arch` inherit the parent's cwd unchanged, and `wsl.exe`
started from a Windows directory enters the translated form of that same
directory. A relative name means the same file on both sides of all four.

The `--cli` / `--reference` paths themselves are the ONE exception: they are
spelled by the CALLER as the LAUNCHER sees them, because choosing that spelling
is a host decision and host decisions belong at the call site.
─────────────────────────────────────────────────────────────────────────────
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

# A run that hangs must not hang the harness. Generous: these are sub-second
# operations, and a slow emulated leg (qemu-aarch64) is still nowhere near this.
STEP_TIMEOUT = 300


class Runner:
    """One binary under test, with its own launcher argv and its own work dir."""

    def __init__(self, name, exe, launcher, workdir):
        self.name = name
        self.exe = exe
        self.launcher = list(launcher or [])
        self.workdir = workdir
        os.makedirs(workdir, exist_ok=True)

    def run(self, args, stdin_text=None):
        """-> (rc, stdout+stderr). rc -1 means the process could not be started;
        rc -2 means it timed out. Both are FAILURES with a readable reason, never
        an exception that would take the whole gate down with one bad leg."""
        argv = self.launcher + [self.exe] + list(args)
        try:
            p = subprocess.run(
                argv, cwd=self.workdir, input=(stdin_text or ""),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                universal_newlines=True, timeout=STEP_TIMEOUT)
            return p.returncode, p.stdout or ""
        except subprocess.TimeoutExpired:
            return -2, "<TIMED OUT after %ds: %s>" % (STEP_TIMEOUT, " ".join(argv))
        except OSError as e:
            return -1, "<COULD NOT EXECUTE: %s (%s)>" % (" ".join(argv), e)

    def sql(self, db, sql_text):
        """Run SQL against a FILE database, as a SEPARATE PROCESS.

        ★ A FILE DATABASE, NEVER `:memory:`. This is an operational rule with a
        measured cause, not a preference: `:memory:` has already hidden a crash in
        this project once. It also cannot express the assertion that matters most
        here — that a SECOND process reopens what the first one wrote, which is
        the whole pager/VFS/OS-layer path that a memory db skips entirely."""
        return self.run([db], stdin_text=sql_text)


def tokens(s):
    return s.split()


# ─────────────────────────────────────────────────────────────────────────────
# THE FOURTEEN ASSERTIONS
#
# Each is a function (Runner, ctx) -> (ok: bool, detail: str). They run IN ORDER
# and share the runner's work dir, because several of them are only meaningful as
# a sequence (write, then reopen, then update, then dump, then reload). A failure
# does not stop the sequence: every assertion gets a verdict, because silence
# about one is a harness bug.
# ─────────────────────────────────────────────────────────────────────────────

DB = "smoke.db"            # relative BY DESIGN — see the module docstring
DB2 = "smoke-reload.db"


def a01_version_runs(r, ctx):
    rc, out = r.run(["--version"])
    # THE RC IS RECORDED, NOT JUST THE TEXT. Three later assertions read
    # ctx["version_out"], and when the binary could not be started at all that key
    # holds this file's own `<COULD NOT EXECUTE …>` / `<TIMED OUT …>` placeholder
    # rather than anything the binary said. An assertion that only inspects the
    # TEXT cannot tell those apart from real output — see a04.
    ctx["version_rc"] = rc
    ctx["version_out"] = out
    return rc == 0, "rc=%d out=%r" % (rc, out.strip()[:200])


def a02_version_token_exact(r, ctx):
    """★ AN EXACT TOKEN COMPARE AGAINST THE STAGED TREE'S OWN VERSION.

    Not a substring test, and not a hardcoded "3.54.0". A substring test passes
    on "3.54.01" and on "13.54.0"; a hardcoded literal silently stops testing
    anything the day upstream bumps. The expected value is read by the CALLER out
    of the staged sqlite3.h that this very binary was compiled against."""
    t = tokens(ctx.get("version_out", ""))
    if not t:
        return False, "--version produced no tokens"
    return t[0] == ctx["expect_version"], \
        "token[0]=%r expected %r" % (t[0], ctx["expect_version"])


def a03_source_id_token_exact(r, ctx):
    """`sqlite3 --version` prints `<version> <date> <time> <hash>`; tokens 1..3
    are SQLITE_SOURCE_ID. Comparing it EXACTLY is the mixed-vintage detector
    (D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE) applied to the finished binary
    rather than to the tree it came from."""
    t = tokens(ctx.get("version_out", ""))
    if len(t) < 4:
        return False, "--version had %d tokens, expected >=4: %r" % (len(t), t)
    got = " ".join(t[1:4])
    return got == ctx["expect_source_id"], \
        "source id %r expected %r" % (got, ctx["expect_source_id"])


def a04_no_version_mismatch(r, ctx):
    """shell.c's own startup guard prints `SQLite header and source version
    mismatch` and exit(1) when the library and the header disagree. That looks
    EXACTLY like a miscompile from the outside — it is a clean compile, a clean
    link, and a binary that refuses to run — so it is asserted by name.

    ★ IT IS GATED ON THE RUN HAVING HAPPENED, and that gate is the whole
    correctness of this assertion. A pure `"version mismatch" not in out` test
    reads [OK] on a CLI THAT CANNOT EXECUTE AT ALL: a01 stores its
    `<COULD NOT EXECUTE …>` / `<TIMED OUT …>` placeholder in the same key, and
    neither placeholder contains the needle. So a binary that never ran once
    failed a01-a03 and then reported this one PASSING — an absence of evidence
    printed as evidence of absence, on the assertion whose entire job is to
    catch a binary that refuses to run."""
    if ctx.get("version_rc") != 0:
        return False, ("--version did not run (rc=%s), so this assertion has NOTHING to inspect; "
                       "the guard it looks for could not have been printed either way: %r"
                       % (ctx.get("version_rc"), ctx.get("version_out", "").strip()[:300]))
    out = ctx.get("version_out", "")
    return "version mismatch" not in out, \
        "--version output mentioned a version mismatch: %r" % out.strip()[:300]


def a05_create_insert(r, ctx):
    for f in (DB, DB2):
        p = os.path.join(r.workdir, f)
        if os.path.exists(p):
            os.remove(p)
    rc, out = r.sql(DB, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, qty INTEGER);\n"
                        "INSERT INTO t(name,qty) VALUES('alpha',10),('beta',20),('gamma',12);\n")
    if rc != 0:
        return False, "rc=%d out=%r" % (rc, out.strip()[:300])
    p = os.path.join(r.workdir, DB)
    if not os.path.isfile(p) or os.path.getsize(p) == 0:
        return False, "no non-empty database FILE was created at %s" % DB
    return True, "%s = %d bytes" % (DB, os.path.getsize(p))


def a06_select_rows(r, ctx):
    rc, out = r.sql(DB, "SELECT name||':'||qty FROM t ORDER BY id;")
    got = [l.strip() for l in out.splitlines() if l.strip()]
    want = ["alpha:10", "beta:20", "gamma:12"]
    return (rc == 0 and got == want), "rc=%d got=%r want=%r" % (rc, got, want)


def a07_select_aggregate(r, ctx):
    rc, out = r.sql(DB, "SELECT count(*)||'/'||sum(qty)||'/'||max(qty) FROM t;")
    got = out.strip()
    return (rc == 0 and got == "3/42/20"), "rc=%d got=%r want='3/42/20'" % (rc, got)


def a08_reopen_second_process(r, ctx):
    """★ THE ASSERTION `:memory:` CANNOT MAKE. Every `sql()` above was already a
    separate process, so the rows have survived one full close/reopen of the file
    by the time this runs; this states it as its own verdict and additionally
    proves the schema — not just the data — round-tripped through the pager."""
    rc, out = r.sql(DB, "SELECT count(*) FROM t WHERE qty > 10;")
    got = out.strip()
    return (rc == 0 and got == "2"), "rc=%d got=%r want='2'" % (rc, got)


def a09_update(r, ctx):
    rc, _ = r.sql(DB, "UPDATE t SET qty = qty + 5 WHERE name = 'alpha';")
    if rc != 0:
        return False, "the UPDATE process exited rc=%d" % rc
    rc2, out = r.sql(DB, "SELECT qty FROM t WHERE name='alpha';")
    got = out.strip()
    return (rc2 == 0 and got == "15"), "rc=%d got=%r want='15'" % (rc2, got)


def a10_delete(r, ctx):
    rc, _ = r.sql(DB, "DELETE FROM t WHERE name = 'gamma';")
    if rc != 0:
        return False, "the DELETE process exited rc=%d" % rc
    rc2, out = r.sql(DB, "SELECT count(*)||'/'||sum(qty) FROM t;")
    got = out.strip()
    return (rc2 == 0 and got == "2/35"), "rc=%d got=%r want='2/35'" % (rc2, got)


def a11_dot_tables(r, ctx):
    rc, out = r.sql(DB, ".tables\n")
    return (rc == 0 and "t" in tokens(out)), "rc=%d out=%r" % (rc, out.strip()[:200])


def a12_dot_schema(r, ctx):
    rc, out = r.sql(DB, ".schema t\n")
    flat = " ".join(out.split())
    ok = rc == 0 and "CREATE TABLE t(" in flat and "qty INTEGER" in flat
    return ok, "rc=%d out=%r" % (rc, flat[:300])


def a13_dot_dump(r, ctx):
    rc, out = r.sql(DB, ".dump\n")
    ctx["dump"] = out
    flat = " ".join(out.split())
    ok = (rc == 0
          and "BEGIN TRANSACTION;" in flat
          and "CREATE TABLE t(" in flat
          and "INSERT INTO t VALUES(1,'alpha',15)" in flat
          and "COMMIT;" in flat)
    return ok, "rc=%d bytes=%d head=%r" % (rc, len(out), flat[:240])


def a14_dump_reload_roundtrip(r, ctx):
    """★ THE ROUND TRIP IS THE POINT. `.dump` is the CLI's own SQL writer and the
    reload goes back through the parser; comparing the two databases' contents
    exercises both directions of a surface the unit corpus never touches, and it
    is an END-TO-END equality rather than a spot check of the dump TEXT."""
    dump = ctx.get("dump", "")
    if not dump.strip():
        return False, "no dump text was captured by the previous assertion"
    p2 = os.path.join(r.workdir, DB2)
    if os.path.exists(p2):
        os.remove(p2)
    rc, out = r.sql(DB2, dump)
    if rc != 0:
        return False, "reload into %s exited rc=%d: %r" % (DB2, rc, out.strip()[:300])
    probe = "SELECT count(*)||'/'||sum(qty)||'/'||group_concat(name,',') FROM (SELECT * FROM t ORDER BY id);"
    rc_a, out_a = r.sql(DB, probe)
    rc_b, out_b = r.sql(DB2, probe)
    ok = rc_a == 0 and rc_b == 0 and out_a.strip() == out_b.strip() and out_a.strip() != ""
    return ok, "original=%r reloaded=%r" % (out_a.strip(), out_b.strip())


ASSERTIONS = [
    ("version-runs",            a01_version_runs),
    ("version-token-exact",     a02_version_token_exact),
    ("source-id-token-exact",   a03_source_id_token_exact),
    ("no-version-mismatch",     a04_no_version_mismatch),
    ("create-insert-file-db",   a05_create_insert),
    ("select-rows",             a06_select_rows),
    ("select-aggregate",        a07_select_aggregate),
    ("reopen-second-process",   a08_reopen_second_process),
    ("update-and-verify",       a09_update),
    ("delete-and-verify",       a10_delete),
    ("dot-tables",              a11_dot_tables),
    ("dot-schema",              a12_dot_schema),
    ("dot-dump",                a13_dot_dump),
    ("dump-reload-roundtrip",   a14_dump_reload_roundtrip),
]
assert len(ASSERTIONS) == 14, "the gate is specified as 14 assertions; got %d" % len(ASSERTIONS)


def run_all(runner, expect_version, expect_source_id):
    """-> (results, ctx). The ctx is returned, not discarded: whether the binary
    could be STARTED at all (ctx["version_rc"]) is a fact about the run that the
    caller has to be able to see — see the reference-usability check in main()."""
    ctx = {"expect_version": expect_version, "expect_source_id": expect_source_id}
    results = {}
    for name, fn in ASSERTIONS:
        try:
            ok, detail = fn(runner, ctx)
        except Exception as e:                     # noqa: BLE001 - a broken assertion is a verdict
            ok, detail = False, "the assertion itself raised %s: %s" % (type(e).__name__, e)
        results[name] = (bool(ok), detail)
    return results, ctx


def main(argv=None):
    p = argparse.ArgumentParser(prog="cli-smoke.py")
    p.add_argument("--cli", required=True,
                   help="the DSS-built sqlite3 CLI, spelled as its LAUNCHER sees it")
    # ★ THE `=` FORM IS THE ONLY ONE THAT WORKS, AND THIS HELP TEXT USED TO SAY
    # OTHERWISE. It read `e.g. --launcher wsl.exe --launcher -e` — byte-for-byte the
    # invocation ✔MEASURED to fail on 2026-08-05 (TF-C121): a launcher TOKEN may
    # itself begin with a dash, and argparse then refuses the SPACE form with
    # "expected one argument" instead of taking the next word as the value. That
    # killed the pe64 CLI smoke gate before a single assertion ran, and the caller
    # classified the argv defect as `smoke: FAIL — CHARGED TO DSS` — the harness
    # accusing the compiler of its own command-line bug. Advertising the broken
    # form in this tool's own --help is how that gets reintroduced.
    # (D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION)
    p.add_argument("--launcher", action="append", default=[], metavar="TOKEN",
                   help="one launcher argv token (repeatable). ALWAYS the `=` form, "
                        "e.g. --launcher=arch --launcher=-x86_64 : a token may start "
                        "with a dash and the space form then fails")
    p.add_argument("--reference", default="",
                   help="a gcc-built reference sqlite3 (upstream `make sqlite3d`), spelled as ITS launcher sees it")
    p.add_argument("--reference-launcher", action="append", default=[], metavar="TOKEN",
                   help="one reference-launcher argv token (repeatable); same `=` "
                        "form rule as --launcher above")
    p.add_argument("--expect-version", required=True,
                   help="SQLITE_VERSION read from the STAGED sqlite3.h this binary was compiled against")
    p.add_argument("--expect-source-id", required=True,
                   help="SQLITE_SOURCE_ID read from that same staged sqlite3.h")
    p.add_argument("--workdir", required=True, help="scratch dir; two subdirs are created under it")
    p.add_argument("--label", default="cli", help="leg label, for the report")
    p.add_argument("--json", default="", help="also write the machine-readable result here")
    args = p.parse_args(argv)

    dss_dir = os.path.join(args.workdir, "dss")
    ref_dir = os.path.join(args.workdir, "reference")
    for d in (dss_dir, ref_dir):
        if os.path.isdir(d):
            shutil.rmtree(d, ignore_errors=True)

    dss = Runner("dss", args.cli, args.launcher, dss_dir)
    dss_res, _dss_ctx = run_all(dss, args.expect_version, args.expect_source_id)

    # ── IS THE REFERENCE USABLE AS AN ORACLE AT ALL? ─────────────────────────
    # ★ A REFERENCE THAT CANNOT RUN IS NOT A REFERENCE, AND TREATING IT AS ONE
    # CORRUPTS EVERY VERDICT IN BOTH DIRECTIONS. If the reference binary cannot
    # be started (wrong path, no launcher on this host, not executable), all
    # FOURTEEN of its assertions fail — and the attribution table then reads:
    #   · every DSS-PASSING assertion  -> "pass (REFERENCE-ONLY fault)", so a run
    #     with a broken oracle printed "PASS 14/14", rc 0, with `charged` and
    #     `exonerated` both empty and attribution silently unavailable;
    #   · every DSS-FAILING assertion  -> "upstream-or-environment", i.e. a real
    #     DSS failure EXONERATED by a binary that never executed. That is the
    #     worse half, and it is the one the rc-3 exit would have hidden.
    # So an unusable reference is demoted to NO reference — which restores the
    # honest "unattributed (NO REFERENCE) -> charged, loudly" row — and it is
    # announced exactly as loudly as `--reference ""` already is.
    # The probe is the reference's OWN a01: it is the one assertion that only
    # asks "did this binary start and exit 0", which is precisely the question.
    ref_res = None
    ref_unusable = ""
    if args.reference:
        ref = Runner("reference", args.reference, args.reference_launcher, ref_dir)
        ref_res, ref_ctx = run_all(ref, args.expect_version, args.expect_source_id)
        if ref_ctx.get("version_rc") != 0:
            ref_unusable = ("`--version` on it exited rc=%s: %s"
                            % (ref_ctx.get("version_rc"),
                               (ref_ctx.get("version_out") or "").strip()[:300]))
            ref_res = None

    # ── attribute, report, verdict ────────────────────────────────────────────
    charged, exonerated, ref_only, passed = [], [], [], []
    rows = []
    for name, _ in ASSERTIONS:
        d_ok, d_detail = dss_res[name]
        r_ok, r_detail = (ref_res[name] if ref_res else (None, "<no reference>"))
        if d_ok and (r_ok is not False):
            verdict = "pass"; passed.append(name)
        elif d_ok and r_ok is False:
            verdict = "pass (REFERENCE-ONLY fault)"; passed.append(name); ref_only.append(name)
        elif r_ok is False:
            verdict = "upstream-or-environment"; exonerated.append(name)
        elif r_ok is True:
            verdict = "DSS"; charged.append(name)
        else:
            verdict = "unattributed (NO REFERENCE)"; charged.append(name)
        rows.append({"assertion": name, "dss": d_ok, "reference": r_ok,
                     "verdict": verdict, "dssDetail": d_detail, "referenceDetail": r_detail})

    print("== sqlite3 CLI smoke gate: %s ==" % args.label)
    print("   binary    : %s%s" % (" ".join(args.launcher) + " " if args.launcher else "", args.cli))
    if args.reference and not ref_unusable:
        print("   reference : %s%s" % (" ".join(args.reference_launcher) + " " if args.reference_launcher else "", args.reference))
    elif args.reference:
        print("   reference : GIVEN BUT UNUSABLE -- %s%s"
              % (" ".join(args.reference_launcher) + " " if args.reference_launcher else "", args.reference))
        print("               %s" % ref_unusable)
        print("               It could not be EXECUTED, so it is not an oracle: every one of its 14")
        print("               assertions would fail for that one reason. It is treated as ABSENT --")
        print("               no failure on this leg can be exonerated, and an unattributable")
        print("               failure is CHARGED TO DSS by design. Fix the reference or its launcher.")
    else:
        print("   reference : ABSENT -- no failure on this leg can be EXONERATED against a non-DSS build.")
    print("   expecting : version %s / source id %s" % (args.expect_version, args.expect_source_id))
    for row in rows:
        mark = "[OK]" if row["dss"] else "[X] "
        print("   %s %-24s %s" % (mark, row["assertion"], row["verdict"]))
        if not row["dss"]:
            print("        dss      : %s" % row["dssDetail"])
            print("        reference: %s" % row["referenceDetail"])

    # ── the verdict — see "ATTRIBUTION IS NOT ABSOLUTION" in the module docstring
    # An exonerated failure still makes this gate RED; it only changes WHO it is
    # charged to. `rc` and `ok` are deliberately different questions: `ok` means
    # "DSS is clean", `rc == 0` means "this leg's CLI is trustworthy".
    ok = not charged                      # DSS is not implicated
    if charged:
        rc, state = 1, "FAIL (CHARGED TO DSS)"
    elif exonerated:
        rc, state = 3, "FAIL (NOT DSS -- upstream or environment)"
    else:
        rc, state = 0, "PASS"
    summary = ("%d/14 passed, %d charged to DSS, %d exonerated (upstream-or-environment)"
               % (len(passed), len(charged), len(exonerated)))
    if ref_only:
        print("   NOTE: the REFERENCE failed these while DSS passed: %s" % ", ".join(ref_only))
        print("         that is a fault in the reference build or its environment, not in DSS.")
    if ref_unusable:
        # Repeated at the BOTTOM as well as the top: this line changes what the
        # verdict above MEANS (nothing on this leg was attributable), and a run
        # is read from its last lines.
        # ASCII ONLY, like every other print in this file, and that is not style:
        # ✔MEASURED 2026-08-05 — a `*` U+2605 here raised UnicodeEncodeError on a
        # Windows cp1252 console and took the whole gate down at the last line,
        # turning a reported verdict into a traceback.
        print("   >> ATTRIBUTION WAS UNAVAILABLE FOR THIS ENTIRE RUN -- the reference could not be executed.")
    print("   %s -- %s" % (state, summary))
    if charged:
        print("   charged to DSS: %s" % ", ".join(charged))
    if exonerated:
        print("   exonerated (the gcc reference fails these identically): %s" % ", ".join(exonerated))
        print("   ...DSS is NOT implicated, but this leg's CLI is still RED: rc=3.")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"label": args.label, "ok": ok, "rc": rc, "state": state,
                       "summary": summary,
                       "charged": charged, "exonerated": exonerated,
                       "referenceOnly": ref_only, "hasReference": bool(args.reference),
                       # A consumer must be able to tell "no oracle was asked for"
                       # from "an oracle was asked for and could not run" — the two
                       # produce identical attribution and mean different things.
                       "referenceUsable": ref_res is not None,
                       "referenceUnusableReason": ref_unusable,
                       "assertions": rows}, f, indent=2)
            f.write("\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
