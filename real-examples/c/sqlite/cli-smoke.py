#!/usr/bin/env python3
"""FAIL-LOUD smoke gate for a built `sqlite3` CLI, ATTRIBUTED against a MATCHED CONTROL.

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

═════════════════════════════════════════════════════════════════════════════
TWO PRECONDITIONS THAT MUST HOLD BEFORE ANY VERDICT IS A VERDICT
═════════════════════════════════════════════════════════════════════════════
✔MEASURED 2026-08-08, Windows host, leg elf64-arm64. This gate printed fourteen
`[X] ... DSS` rows and the driver reported "CLI smoke FAILED and is CHARGED TO
DSS". Both halves of that sentence were manufactured by this file, from two
INDEPENDENT defects that had nothing to do with the compiler:

  subject   : wsl.exe -e qemu-aarch64 <arm64 sqlite3>
              -> rc=255, "qemu-aarch64: Could not open '/lib/ld-linux-aarch64.so.1'"
  reference : wsl.exe -e <reference sqlite3>
              -> rc=0    (a host-native x86_64 gcc build, NO qemu in the argv)

  (a) THE REFERENCE WAS NOT A MATCHED CONTROL. It targeted a different
      (arch, container, targetOs) than the leg under test, so comparing the two
      answers a question nobody asked. This also fires with NOTHING red: on the
      pe64-x86_64/Windows leg the oracle is a Linux x86_64 ELF run under WSL, so
      every EXONERATION and every DSS CHARGE on that leg rested on a different
      target's behaviour. An oracle for target A says nothing about target B, in
      either direction, and "it agreed" is not evidence that it could have
      disagreed.

  (b) THE SUBJECT NEVER LAUNCHED. The loader refused it; no instruction of
      generated code ever executed. A process that did not reach `main()` is not
      an observation about generated code, under ANY circumstances — not a
      failing one, and not a passing one either.

So the gate now establishes both preconditions FIRST, and only then attributes:

  · `--leg-spec` (DECLARED, from the plan) vs `--cli-target` (MEASURED). A
    mismatch is IMMEDIATELY FATAL and is never compared to anything: the leg
    built the wrong thing, and a control matched to either side would be
    attributing the wrong subject.
  · `--reference-target` (MEASURED) vs `--cli-target`, plus "did the reference
    itself start". Only a reference that passes BOTH is a control; anything else
    is DEMOTED, and a demoted control can neither exonerate nor charge.
  · the SUBJECT LAUNCH WITNESS — a FACT, not a regex: did the version probe
    write at least one byte to the subject's OWN stdout. Every launcher writes
    its refusal to stderr, and `sqlite3 --version` writes to stdout, so the byte
    count answers "did the program itself get to run" without this file ever
    knowing that `qemu` or `wine` or `wsl.exe` exist. Pattern-matching a
    launcher's prose would put a launcher branch in the one file that has to
    stay launcher-agnostic, and would go quietly stale the day a launcher
    reworded its error.

THE VERDICT ALGEBRA (pure, testable, and self-tested — see `--self-test`)

    subject did not launch            -> subject-did-not-launch   (NEVER DSS)
    DSS pass                          -> pass
    DSS fail  + matched control fail  -> upstream-or-environment  (NOT DSS)
    DSS fail  + matched control pass  -> DSS                      (a real failure)
    DSS pass  + matched control fail  -> pass, reference-only fault reported
    DSS fail  + NO MATCHED CONTROL    -> unattributed (NO MATCHED CONTROL)

`unattributed (NO MATCHED CONTROL)` is ONE row covering three different ways of
having no oracle — the reference was ABSENT, it DID NOT LAUNCH, or it targeted
something else — because all three leave the identical hole. Which one it was is
reported separately, and loudly, so the operator can fix the right thing.

★★ AN ABSENT OR UNMATCHED ORACLE STILL MUST NOT SOFTEN A FAILURE, AND IT DOES
NOT. What changed is the ATTRIBUTION, not the severity: an unattributable
failure used to be CHARGED TO DSS (rc 1) and is now rc 4, which is still RED,
still non-zero, and still counted by both drivers. Charging it to DSS was not
rigour — it was a wrong answer given confidently, and (a) above is what that
costs. "0 files / 0 usable assertions is a pass" remains unreachable.

★★ ATTRIBUTION IS NOT ABSOLUTION — AND THIS GATE EXITS NON-ZERO EITHER WAY.
`upstream-or-environment` says WHO is at fault; it does not say the gate is
green. ✔MEASURED 2026-08-05 while red-on-disable testing this very file: passing
a deliberately WRONG `--expect-version 9.9.9` made both binaries fail assertion
2, which the first draft attributed as `upstream-or-environment` and then
reported as **PASS, rc 0** — an instrument that answered "fine" to a question it
had just got wrong. Four exit codes exist so a caller can tell the cases apart
without any of them ever reading as success:

    0  every assertion passed ON THE SUBJECT.
    1  RED, CHARGED TO DSS      — at least one failure that the MATCHED CONTROL
                                  passes. This is the only code that accuses the
                                  compiler, and it is now unreachable without a
                                  matched control and a launched subject.
    3  RED, NOT DSS             — everything that failed also failed on the
                                  matched control. DSS is explicitly not
                                  implicated; the leg's CLI is still red.
    4  RED, NOT A VERDICT       — nothing here attributes. Either the subject
                                  never launched, or there was no matched
                                  control to attribute against, or the leg built
                                  a target other than the one it was asked for.

  Precedence, in EXACTLY this order:
      subject did not launch -> 4 ; charged -> 1 ; unattributed -> 4 ;
      exonerated -> 3 ; else 0

A caller that wants "did DSS break?" tests for 1. A caller that wants "is this
leg trustworthy?" tests for non-zero. A caller that wants "did we learn
anything?" tests for 4. Neither question can be answered wrongly by accident.

★ `ok` IS `rc == 0`, AND `dssImplicated` IS THE THREE-VALUED ANSWER. The JSON
used to carry `ok = not charged`, which under rc 4 (where `charged` is empty by
construction) would have flipped TRUE on a run that proved nothing — the trap
this whole change exists to close, reintroduced one field lower down. `ok` is
now exactly `rc == 0`; "was DSS implicated" is a SEPARATE field that is allowed
to say `null`, because "this instrument cannot say" is a real answer and is not
the same as "no".

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
is a host decision and host decisions belong at the call site. For the same
reason the TARGET TRIPLES are MEASURED BY THE CALLER and passed in: this file
cannot open a launcher-namespace path to read an ELF/PE/Mach-O header, and a
second copy of that measurement is a second thing to drift.
─────────────────────────────────────────────────────────────────────────────
"""
import argparse
import collections
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile

# A run that hangs must not hang the harness. Generous: these are sub-second
# operations, and a slow emulated leg (qemu-aarch64) is still nowhere near this.
STEP_TIMEOUT = 300


# ─────────────────────────────────────────────────────────────────────────────
# THE TARGET TRIPLE
#
# ★ THREE FIELDS, BECAUSE A CONTROL HAS TO MATCH ON ALL THREE. Two binaries with
# the same arch and different containers (elf64 vs pe64) run under different
# loaders, link against different C runtimes and take different code paths
# through sqlite's own OS layer; two with the same container and different arch
# are different code generators entirely. "Same target" is the conjunction, and
# writing it as a value rather than three loose strings is what makes `==` the
# whole comparison.
#
# ★ THIS FILE NEVER MEASURES A TARGET AND NEVER INTERPRETS ONE. It parses, it
# compares for EQUALITY, and it prints. There is deliberately no table of known
# arches, containers or OSes anywhere below: such a table is an identity branch
# in disguise and would have to be edited every time a leg is added, which is
# precisely the coupling the bar forbids.
# ─────────────────────────────────────────────────────────────────────────────
class Target(collections.namedtuple("Target", ("arch", "container", "target_os"))):
    """⚠ IT IS A TUPLE, SO `"%s" % target` DOES NOT DO WHAT IT LOOKS LIKE. `%`
    treats a tuple on the right as the ARGUMENT LIST, so a lone Target becomes
    three arguments and raises "not all arguments converted". Every single-value
    format site below therefore spells the 1-tuple `% (target,)` explicitly.
    ✔MEASURED 2026-08-08: the self test added in this same change caught exactly
    this on its first run, in the report path, which is a line that only executes
    on a real leg."""

    __slots__ = ()

    def __str__(self):
        return "%s:%s:%s" % (self.arch, self.container, self.target_os)


def parse_target(text, who):
    """`arch:container:targetOs` -> Target. RAISES on anything else.

    Fail-loud with the offending value NAMED: a target string that quietly
    parsed wrong would decide whether a control matches, i.e. whether a failure
    gets exonerated, and it would decide it invisibly."""
    if not isinstance(text, str) or not text.strip():
        raise ValueError(
            "%s is empty. It must be a MEASURED target triple `arch:container:targetOs` "
            "(three non-empty colon-separated fields, e.g. `arm64:elf64:linux`)." % who)
    fields = text.split(":")
    if len(fields) != 3 or not all(f.strip() for f in fields):
        raise ValueError(
            "%s = %r is not a target triple. Expected EXACTLY three non-empty "
            "colon-separated fields `arch:container:targetOs` (e.g. `arm64:elf64:linux`); "
            "got %d field(s). This value decides whether the control is MATCHED, so it is "
            "never guessed at." % (who, text, len(fields)))
    return Target(*[f.strip() for f in fields])


def target_of_leg_spec(text, who="--leg-spec"):
    """`arch:container-fmtarch-targetOs[-linkage]` -> Target. RAISES on anything else.

    The DECLARED side of the precondition. The leg spec is the string legs.json
    already carries and the driver already passes to DSS (`--target`), so it is
    quoted, not re-derived.

    ★★ THE ARCH COMES FROM FIELD 0 OF THE SPEC, NEVER FROM THE FORMAT NAME, AND
    THAT IS NOT A STYLE CHOICE. The two halves SPELL THE SAME ARCH DIFFERENTLY:
    `arm64:elf64-aarch64-linux-exec` is arch `arm64` with format arch `aarch64`.
    Reading the format's arch field would produce a triple that can never equal a
    MEASURED one without a translation table — and a translation table of arch
    spellings is exactly the arch identity branch this file is not allowed to
    contain. Field 1 is therefore parsed past and deliberately not read."""
    if not isinstance(text, str) or not text.strip():
        raise ValueError(
            "%s is empty. It must be the leg's DECLARED spec `arch:objectFormat` "
            "(e.g. `arm64:elf64-aarch64-linux-exec`)." % who)
    parts = text.split(":")
    if len(parts) != 2 or not all(p.strip() for p in parts):
        raise ValueError(
            "%s = %r is not a leg spec. Expected EXACTLY two non-empty colon-separated "
            "fields `arch:objectFormat` (e.g. `arm64:elf64-aarch64-linux-exec`); got %d "
            "field(s)." % (who, text, len(parts)))
    arch, fmt = parts[0].strip(), parts[1].strip()
    fields = fmt.split("-")
    if len(fields) < 3 or not all(f.strip() for f in fields[:3]):
        raise ValueError(
            "%s = %r has an object format (%r) this file cannot read positionally. The "
            "format grammar is `<container>-<formatArch>-<targetOs>[-<linkage>]`, so at "
            "least three non-empty dash-separated fields are required (e.g. "
            "`elf64-aarch64-linux-exec`)." % (who, text, fmt))
    return Target(arch, fields[0].strip(), fields[2].strip())


class Runner:
    """One binary under test, with its own launcher argv and its own work dir."""

    def __init__(self, name, exe, launcher, workdir):
        self.name = name
        self.exe = exe
        self.launcher = list(launcher or [])
        self.workdir = workdir
        os.makedirs(workdir, exist_ok=True)

    def run(self, args, stdin_text=None):
        """-> (rc, stdout+stderr MERGED). rc -1 means the process could not be
        started; rc -2 means it timed out. Both are FAILURES with a readable
        reason, never an exception that would take the whole gate down with one
        bad leg.

        ⚠⚠ THE MERGE IS LOAD-BEARING AND MUST NOT BE "CLEANED UP". `stderr` is
        redirected INTO `stdout` on purpose: assertion `a04_no_version_mismatch`
        looks for shell.c's startup guard, which shell.c prints to STDERR. Split
        these two streams and a04 searches a text that can no longer contain the
        string it exists to find — it goes green on exactly the binary it was
        written to catch, silently, with no test failing. The LAUNCH WITNESS
        needs the opposite (stdout alone), and it gets it from `run_split`
        below as a SECOND, SEPARATE capture rather than by splitting this one.
        There is a regression pin for precisely this in `--self-test`."""
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

    def run_split(self, args, stdin_text=None):
        """-> (rc, stdout, stderr) with the two streams SEPARATE. Used by ONE
        caller, for ONE question: did the subject itself write anything to its
        own stdout, i.e. did it launch.

        ⚠ ON A FAILURE TO START OR A TIMEOUT, `stdout` IS NEVER SYNTHESISED.
        `run` above returns its `<COULD NOT EXECUTE ...>` / `<TIMED OUT ...>`
        placeholder AS the output, which is right for a text the assertions
        read and catastrophic for a byte-count witness: a placeholder in the
        stdout slot would make "the process could not be started" look exactly
        like "the program launched and printed something". The placeholder goes
        in the STDERR slot; stdout stays empty unless the child really wrote to
        it. A timeout keeps whatever the child had already written, because a
        subject that printed its banner and then hung DID launch."""
        argv = self.launcher + [self.exe] + list(args)
        try:
            p = subprocess.run(
                argv, cwd=self.workdir, input=(stdin_text or ""),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True, timeout=STEP_TIMEOUT)
            return p.returncode, p.stdout or "", p.stderr or ""
        except subprocess.TimeoutExpired as e:
            partial = e.stdout or ""
            if isinstance(partial, bytes):
                partial = partial.decode("utf-8", "replace")
            return -2, partial, "<TIMED OUT after %ds: %s>" % (STEP_TIMEOUT, " ".join(argv))
        except OSError as e:
            return -1, "", "<COULD NOT EXECUTE: %s (%s)>" % (" ".join(argv), e)

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
    # ── THE LAUNCH WITNESS: A SECOND, SEPARATELY CAPTURED PROBE ───────────────
    # ⚠⚠ DO NOT "SIMPLIFY" THIS INTO ONE RUN. The two captures answer two
    # different questions and need two different pipe topologies:
    #   · ctx["version_out"]    MERGED   — what the assertions read. a04 needs
    #                                      shell.c's stderr guard to be IN it.
    #   · ctx["version_stdout"] SPLIT    — the launch witness, and nothing else.
    # Deriving the merged text by concatenating a split capture is NOT the same
    # thing either: the merge order would become this file's invention rather
    # than the child's actual interleaving, and a02/a03 read token POSITIONS out
    # of it. Two probes of an idempotent, sub-second, side-effect-free command is
    # the cheap half of that trade.
    split_rc, split_out, split_err = r.run_split(["--version"])
    ctx["version_split_rc"] = split_rc
    ctx["version_stdout"] = split_out
    ctx["version_stderr"] = split_err
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
    catch a binary that refuses to run.

    ⚠⚠ IT READS THE **MERGED** CAPTURE, AND THAT IS THE POINT. shell.c prints
    this guard to STDERR. `ctx["version_out"]` is merged (Runner.run redirects
    stderr into stdout); `ctx["version_stdout"]` is NOT and exists only for the
    launch witness. Pointing this assertion at the stdout-only capture makes it
    pass on the exact binary it was written to catch. `--self-test` pins that."""
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
ASSERTION_NAMES = tuple(n for n, _ in ASSERTIONS)


def run_all(runner, expect_version, expect_source_id):
    """-> (results, ctx). The ctx is returned, not discarded: whether the binary
    LAUNCHED (ctx["version_stdout"]) and whether it exited 0 (ctx["version_rc"])
    are facts about the run that the caller has to be able to see — they are the
    two preconditions the module docstring describes, and neither of them is a
    property of any individual assertion."""
    ctx = {"expect_version": expect_version, "expect_source_id": expect_source_id}
    results = {}
    for name, fn in ASSERTIONS:
        try:
            ok, detail = fn(runner, ctx)
        except Exception as e:                     # noqa: BLE001 - a broken assertion is a verdict
            ok, detail = False, "the assertion itself raised %s: %s" % (type(e).__name__, e)
        results[name] = (bool(ok), detail)
    return results, ctx


def subject_launched_from_ctx(ctx):
    """THE LAUNCH WITNESS, AS A FACT: did the version probe write at least one
    byte to the subject's OWN stdout?

    ★ WHY THIS AND NOT A REGEX ON THE ERROR TEXT. Every launcher this harness
    declares writes its refusal to STDERR (`qemu-aarch64: Could not open
    '/lib/ld-linux-aarch64.so.1'` was the measured one), and `sqlite3 --version`
    writes its banner to STDOUT. So one byte on stdout is a positive statement
    that the program itself ran, obtained by counting rather than by recognising.
    Matching a launcher's prose would (a) put a per-launcher branch in the one
    file that must stay launcher-agnostic and (b) rot silently the day any of
    them rewords, at which point a subject that never ran is charged to DSS
    again.

    ★ NOT STRIPPED, ON PURPOSE. "At least one byte" is the rule; a lone newline
    is a byte the subject wrote. The residual risk is a hypothetical launcher
    that prints its own chatter to the CHILD's stdout before failing, which would
    read as launched — that is a smaller, louder error (the assertions then fail
    on garbage text) than the one being fixed, and closing it would require
    exactly the launcher knowledge this file refuses to hold."""
    return len(ctx.get("version_stdout") or "") > 0


# ─────────────────────────────────────────────────────────────────────────────
# THE VERDICT ALGEBRA
#
# Everything below this line is PURE: no subprocesses, no filesystem, no clock,
# no argv. That is what makes `--self-test` able to drive the real thing over the
# full cross product instead of a stub of it — and it is why the two preconditions
# arrive as PARAMETERS (`subject_launched`, `control_state`) rather than being
# sniffed out of the results, which is how (a) and (b) in the docstring happened.
# ─────────────────────────────────────────────────────────────────────────────

# ★ A CLOSED VOCABULARY WITH NO DEFAULT. An unrecognised control state RAISES.
# There is no `else: treat it as matched` and no `else: treat it as absent`,
# because either default silently decides whether failures get exonerated.
CONTROL_MATCHED = "matched"
CONTROL_TARGET_MISMATCH = "target-mismatch"
CONTROL_DID_NOT_LAUNCH = "did-not-launch"
CONTROL_ABSENT = "absent"
CONTROL_STATES = (CONTROL_MATCHED, CONTROL_TARGET_MISMATCH,
                  CONTROL_DID_NOT_LAUNCH, CONTROL_ABSENT)

# ★ AND A CLOSED VERDICT SET AT SIX NAMES. Every row that this file emits is one
# of these; the closure is asserted at the end of `attribute` so a future edit
# cannot introduce a seventh verdict that no caller knows how to read.
V_PASS = "pass"
V_PASS_REFERENCE_FAULT = "pass (REFERENCE-ONLY fault)"
V_UPSTREAM_OR_ENV = "upstream-or-environment"
V_DSS = "DSS"
V_UNATTRIBUTED = "unattributed (NO MATCHED CONTROL)"
V_DID_NOT_LAUNCH = "subject-did-not-launch"
VERDICTS = (V_PASS, V_PASS_REFERENCE_FAULT, V_UPSTREAM_OR_ENV,
            V_DSS, V_UNATTRIBUTED, V_DID_NOT_LAUNCH)

RC_PASS = 0
RC_CHARGED_TO_DSS = 1
RC_NOT_DSS = 3
RC_UNATTRIBUTABLE = 4

STATE_PASS = "PASS"
STATE_CHARGED = "FAIL (CHARGED TO DSS)"
STATE_NOT_DSS = "FAIL (NOT DSS -- upstream or environment)"
STATE_NO_LAUNCH = "FAIL (NOT A VERDICT -- THE SUBJECT NEVER LAUNCHED)"
STATE_UNATTRIBUTED = "FAIL (NOT A VERDICT -- NO MATCHED CONTROL)"
STATE_WRONG_TARGET = "FAIL (NOT A VERDICT -- THE LEG BUILT THE WRONG TARGET)"

Attribution = collections.namedtuple("Attribution", (
    "rows", "rc", "state", "ok", "dss_implicated", "summary",
    "passed", "charged", "exonerated", "reference_only",
    "unattributed", "did_not_launch",
    "control_state", "subject_launched",
))


def classify_control(has_reference, reference_target, cli_target, reference_version_rc):
    """-> (control_state, reason). PURE, and the ONLY place `matched` is decided.

    `matched` requires BOTH halves and is checked in this order because the
    answers are not interchangeable: a reference aimed at a different target was
    never a control at all, whereas one aimed at the right target that failed to
    start is a control the operator can repair. Both are reported by name so the
    operator fixes the right thing; both collapse to the same hole in the
    attribution."""
    if not has_reference:
        return CONTROL_ABSENT, "no --reference was given"
    if reference_target != cli_target:
        return CONTROL_TARGET_MISMATCH, (
            "the reference targets %s but the SUBJECT targets %s. A binary for another "
            "target is not a control for this one: it exercises a different code "
            "generator, a different container, a different loader and a different C "
            "runtime, so neither its agreement nor its disagreement is evidence about "
            "this leg%s" % (reference_target, cli_target,
                            "" if reference_version_rc == 0 else
                            " (and it also failed to start: `--version` exited rc=%s)"
                            % (reference_version_rc,)))
    if reference_version_rc != 0:
        return CONTROL_DID_NOT_LAUNCH, (
            "`--version` on the reference exited rc=%s, so it never ran. All fourteen of "
            "its assertions would fail for that one reason, which would EXONERATE every "
            "DSS failure on this leg against a binary that never executed."
            % (reference_version_rc,))
    return CONTROL_MATCHED, ""


def attribute(dss_res, ref_res, subject_launched, control_state):
    """THE WHOLE VERDICT ALGEBRA, as one pure function. -> Attribution.

    dss_res / ref_res : {assertion name: (ok: bool, detail: str)}; ref_res is
                        None unless control_state is `matched`.
    subject_launched  : the FACT from `subject_launched_from_ctx`, passed in.
    control_state     : one of CONTROL_STATES. No default, ever."""
    if control_state not in CONTROL_STATES:
        raise ValueError(
            "unrecognised control_state %r. The vocabulary is CLOSED: %s. There is no "
            "default here on purpose - any default would silently decide whether a "
            "failure gets exonerated, which is the decision this argument exists to make "
            "explicit." % (control_state, ", ".join(CONTROL_STATES)))
    if subject_launched is not True and subject_launched is not False:
        raise TypeError(
            "subject_launched must be exactly True or False; got %r (%s). It is a MEASURED "
            "fact (did the version probe write to the subject's own stdout), not a truthy "
            "value to be inferred - passing the captured text here would make every empty "
            "string a non-launch and every placeholder a launch."
            % (subject_launched, type(subject_launched).__name__))

    has_control = control_state == CONTROL_MATCHED
    if has_control and ref_res is None:
        raise ValueError(
            "control_state is %r but no reference results were supplied. A matched control "
            "is the only thing that can exonerate or charge; claiming one without it would "
            "make every failure unattributable while reporting that it was attributed."
            % CONTROL_MATCHED)
    if (not has_control) and ref_res is not None:
        raise ValueError(
            "control_state is %r yet reference results WERE supplied. An unmatched control "
            "must be DEMOTED to None by the caller, never passed through: passing it is "
            "exactly how a real DSS failure gets exonerated by a binary that was never a "
            "control for this target." % control_state)

    missing = [n for n in ASSERTION_NAMES if n not in dss_res]
    if missing:
        raise ValueError(
            "the subject's result table is missing %d of the %d assertions: %s. Every "
            "assertion gets a verdict; silence about one is a harness bug."
            % (len(missing), len(ASSERTION_NAMES), ", ".join(missing)))
    if ref_res is not None:
        missing_ref = [n for n in ASSERTION_NAMES if n not in ref_res]
        if missing_ref:
            raise ValueError(
                "the control's result table is missing %d of the %d assertions: %s. A "
                "partial control cannot attribute the rows it has no answer for."
                % (len(missing_ref), len(ASSERTION_NAMES), ", ".join(missing_ref)))

    rows = []
    passed, charged, exonerated = [], [], []
    reference_only, unattributed, did_not_launch = [], [], []
    for name in ASSERTION_NAMES:
        d_ok, d_detail = dss_res[name]
        d_ok = bool(d_ok)
        if ref_res is None:
            r_ok, r_detail = None, "<no matched control: %s>" % control_state
        else:
            r_ok, r_detail = ref_res[name]
            r_ok = bool(r_ok)

        if not subject_launched:
            # ★ EVERY ROW, INCLUDING THE ONES THAT "PASSED". A pass produced by a
            # process that never reached main() is the vacuity this whole change
            # exists to kill (a04's `needle not in text` reading [OK] on a binary
            # the loader refused is the canonical instance). It is not evidence in
            # either direction, so it is not reported as either.
            verdict = V_DID_NOT_LAUNCH
            did_not_launch.append(name)
        elif d_ok and r_ok is not False:
            verdict = V_PASS
            passed.append(name)
        elif d_ok:                                  # r_ok is False
            verdict = V_PASS_REFERENCE_FAULT
            passed.append(name)
            reference_only.append(name)
        elif r_ok is False:
            verdict = V_UPSTREAM_OR_ENV
            exonerated.append(name)
        elif r_ok is True:
            verdict = V_DSS
            charged.append(name)
        else:                                       # r_ok is None: no matched control
            verdict = V_UNATTRIBUTED
            unattributed.append(name)

        rows.append({"assertion": name, "dss": d_ok, "reference": r_ok,
                     "verdict": verdict, "dssDetail": d_detail, "referenceDetail": r_detail})

    # ── PRECEDENCE, IN EXACTLY THIS ORDER ────────────────────────────────────
    # `charged` is deliberately ahead of `unattributed` even though the two can
    # never both be non-empty (charging needs a matched control; unattributed
    # rows only exist without one). The order is the contract, not an accident of
    # which branch happens to be reachable, so it is written the way it is
    # specified and stays correct if the row rules ever widen.
    if did_not_launch:
        rc, state = RC_UNATTRIBUTABLE, STATE_NO_LAUNCH
    elif charged:
        rc, state = RC_CHARGED_TO_DSS, STATE_CHARGED
    elif unattributed:
        rc, state = RC_UNATTRIBUTABLE, STATE_UNATTRIBUTED
    elif exonerated:
        rc, state = RC_NOT_DSS, STATE_NOT_DSS
    else:
        rc, state = RC_PASS, STATE_PASS

    # ★ `ok` IS `rc == 0` AND NOTHING ELSE. The old `ok = not charged` would read
    # TRUE under rc 4, where `charged` is empty by construction - a run that
    # proved nothing reporting itself clean.
    ok = rc == RC_PASS
    # ★ THREE-VALUED ON PURPOSE. `None` is not "no"; it is "this instrument
    # cannot say", which is the honest answer whenever the subject never launched
    # or there was nothing to attribute against.
    if rc == RC_CHARGED_TO_DSS:
        dss_implicated = True
    elif rc == RC_UNATTRIBUTABLE:
        dss_implicated = None
    else:
        dss_implicated = False

    summary = ("%d/%d passed on the subject, %d charged to DSS, %d exonerated "
               "(upstream-or-environment), %d unattributed (no matched control), "
               "%d not an observation (the subject never launched)"
               % (len(passed), len(ASSERTION_NAMES), len(charged), len(exonerated),
                  len(unattributed), len(did_not_launch)))

    emitted = set(r["verdict"] for r in rows)
    unknown = emitted - set(VERDICTS)
    if unknown:
        raise AssertionError(
            "the verdict set is CLOSED at %d names and this run emitted %s. Every caller "
            "reads these by name; a seventh verdict is a silent contract break."
            % (len(VERDICTS), ", ".join(sorted(unknown))))

    return Attribution(rows=rows, rc=rc, state=state, ok=ok, dss_implicated=dss_implicated,
                       summary=summary, passed=passed, charged=charged, exonerated=exonerated,
                       reference_only=reference_only, unattributed=unattributed,
                       did_not_launch=did_not_launch, control_state=control_state,
                       subject_launched=subject_launched)


# ─────────────────────────────────────────────────────────────────────────────
# REPORT + CLI
# ─────────────────────────────────────────────────────────────────────────────
# ASCII ONLY IN EVERY `print` IN THIS FILE, and that is not style: ✔MEASURED
# 2026-08-05 - a `*` U+2605 in one of these lines raised UnicodeEncodeError on a
# Windows cp1252 console and took the whole gate down at its last line, turning a
# reported verdict into a traceback.

ROW_MARKS = {V_PASS: "[OK]", V_PASS_REFERENCE_FAULT: "[OK]", V_DID_NOT_LAUNCH: "[--]"}


def _spelled(launcher, path):
    return "%s%s" % (" ".join(launcher) + " " if launcher else "", path)


def main(argv=None):
    argv = list(sys.argv[1:]) if argv is None else list(argv)
    # ── `--self-test` IS CHECKED BEFORE argparse, NOT DECLARED IN IT ──────────
    # Declared as an option it would have to co-exist with six REQUIRED ones, so
    # running the self test would mean inventing values for --cli, --workdir and
    # the two expectations - i.e. the self test would only run in a shape that
    # never occurs. It also keeps argparse's required-argument message starting
    # with `--cli`, which test-confound-scope.{sh,ps1} assert on by substring.
    if "--self-test" in argv:
        if len(argv) != 1:
            sys.stderr.write(
                "cli-smoke.py: --self-test runs the attribution self test and takes no "
                "other arguments; got %r\n" % (argv,))
            return 2
        return self_test()

    p = argparse.ArgumentParser(prog="cli-smoke.py")
    # ⚠ `--cli` STAYS THE FIRST REQUIRED ARGUMENT. argparse lists missing
    # required arguments in DECLARATION order, and both driver self-tests assert
    # on the substring "the following arguments are required: --cli".
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
    # ── THE TWO PRECONDITION ARGUMENTS ───────────────────────────────────────
    # MEASURED BY THE CALLER, not by this file, and that division is deliberate:
    # the binary lives in the LAUNCHER's filesystem namespace (`wsl.exe`, `arch`,
    # a Mac over ssh), which this file cannot open - the same reason every path
    # it hands the child is relative. The driver already resolves that namespace;
    # a second resolver here would be a second thing to drift.
    p.add_argument("--leg-spec", required=True, metavar="ARCH:FORMAT",
                   help="the leg's DECLARED spec from the plan, e.g. "
                        "arm64:elf64-aarch64-linux-exec")
    p.add_argument("--cli-target", required=True, metavar="ARCH:CONTAINER:OS",
                   help="the MEASURED target of --cli, e.g. arm64:elf64:linux")
    p.add_argument("--reference", default="",
                   help="a gcc-built reference sqlite3 (upstream `make sqlite3d`), spelled as ITS launcher sees it")
    p.add_argument("--reference-launcher", action="append", default=[], metavar="TOKEN",
                   help="one reference-launcher argv token (repeatable); same `=` "
                        "form rule as --launcher above")
    p.add_argument("--reference-target", default="", metavar="ARCH:CONTAINER:OS",
                   help="the MEASURED target of --reference; REQUIRED whenever "
                        "--reference is given. It is a control only if it equals "
                        "--cli-target")
    p.add_argument("--expect-version", required=True,
                   help="SQLITE_VERSION read from the STAGED sqlite3.h this binary was compiled against")
    p.add_argument("--expect-source-id", required=True,
                   help="SQLITE_SOURCE_ID read from that same staged sqlite3.h")
    p.add_argument("--workdir", required=True, help="scratch dir; two subdirs are created under it")
    p.add_argument("--label", default="cli", help="leg label, for the report")
    p.add_argument("--json", default="", help="also write the machine-readable result here")
    args = p.parse_args(argv)

    if args.reference and not args.reference_target.strip():
        p.error("--reference was given without --reference-target. A reference whose target "
                "is unknown cannot be shown to be a MATCHED control, and an unmatched "
                "control silently exonerates real failures - so the target is required, "
                "not optional.")

    # The parsers RAISE on an unparseable value, naming it. These strings decide
    # whether anything measured below is about the leg that was asked for, so
    # there is no lenient path - but a TRACEBACK is the wrong way to say so from
    # a CLI: python exits 1 on an uncaught exception, and 1 is this gate's "CHARGED
    # TO DSS". A malformed argument would then accuse the compiler, which is the
    # exact shape of D-HARNESS-DASH-LEADING-LAUNCHER-TOKEN-MISPARSED-AS-AN-OPTION.
    # argparse's own usage exit (2) is outside the verdict codes 0/1/3/4 and
    # carries the raised diagnostic verbatim.
    try:
        cli_target = parse_target(args.cli_target, "--cli-target")
        leg_target = target_of_leg_spec(args.leg_spec)
        reference_target = (parse_target(args.reference_target, "--reference-target")
                            if args.reference_target.strip() else None)
    except ValueError as e:
        p.error(str(e))

    print("== sqlite3 CLI smoke gate: %s ==" % args.label)
    print("   leg (declared) : %s  ->  %s" % (args.leg_spec, leg_target))
    print("   subject        : %s" % _spelled(args.launcher, args.cli))
    print("   subject target : %s  (MEASURED)" % (cli_target,))

    # ── PRECONDITION 1: DID THE LEG BUILD WHAT IT WAS ASKED FOR? ─────────────
    # SEPARATE from attribution and IMMEDIATELY FATAL. Nothing measured on a
    # binary for another target is a statement about this leg, so it is not
    # measured: a control matched to either side would be attributing the wrong
    # subject, and fourteen rows of confident output about the wrong binary is
    # worse than no rows at all.
    if cli_target != leg_target:
        print("   >> FATAL: THE LEG BUILT THE WRONG TARGET.")
        print("      declared --leg-spec   %s  ->  %s" % (args.leg_spec, leg_target))
        print("      measured --cli-target %s" % (cli_target,))
        print("      These are not the same target. The assertions were NOT run and this")
        print("      binary was NOT compared to anything: every verdict below would be")
        print("      about a subject nobody asked for. Fix the build or the plan.")
        print("   %s -- no assertion was executed" % STATE_WRONG_TARGET)
        _write_json(args, {
            "label": args.label, "ok": False, "rc": RC_UNATTRIBUTABLE,
            "state": STATE_WRONG_TARGET,
            "summary": "the leg declares %s but the built CLI is %s; no assertion was run"
                       % (leg_target, cli_target),
            "dssImplicated": None,
            "legSpec": args.leg_spec, "legTarget": str(leg_target),
            "cliTarget": str(cli_target), "legTargetMismatch": True,
            "referenceTarget": str(reference_target) if reference_target else "",
            "controlState": None, "controlReason": "not evaluated: the subject is the wrong target",
            "subjectLaunched": None, "subjectLaunchWitness": "not probed",
            "charged": [], "exonerated": [], "referenceOnly": [],
            "unattributed": [], "subjectDidNotLaunch": [],
            "hasReference": bool(args.reference), "referenceUsable": False,
            "referenceUnusableReason": "not evaluated: the subject is the wrong target",
            "assertions": [],
        })
        return RC_UNATTRIBUTABLE

    dss_dir = os.path.join(args.workdir, "dss")
    ref_dir = os.path.join(args.workdir, "reference")
    for d in (dss_dir, ref_dir):
        if os.path.isdir(d):
            shutil.rmtree(d, ignore_errors=True)

    dss = Runner("dss", args.cli, args.launcher, dss_dir)
    dss_res, dss_ctx = run_all(dss, args.expect_version, args.expect_source_id)

    # ── PRECONDITION 2: DID THE SUBJECT LAUNCH? ──────────────────────────────
    launched = subject_launched_from_ctx(dss_ctx)
    witness = ("the version probe wrote %d char(s) to the subject's OWN stdout"
               % len(dss_ctx.get("version_stdout") or ""))

    # ── PRECONDITION 3: IS THE REFERENCE A MATCHED CONTROL? ──────────────────
    # The reference is run REGARDLESS of whether it can end up matched, because
    # `matched` needs the answer to "did it start" and that answer only exists
    # once it has been tried. What changes is what is DONE with the results: an
    # unmatched control is demoted to None here and can then neither exonerate a
    # DSS failure nor charge one - `attribute` raises if a caller tries to pass
    # an unmatched control's results through anyway.
    ref_res = None
    ref_ctx = {}
    if args.reference:
        ref = Runner("reference", args.reference, args.reference_launcher, ref_dir)
        ref_res, ref_ctx = run_all(ref, args.expect_version, args.expect_source_id)
    control_state, control_reason = classify_control(
        has_reference=bool(args.reference),
        reference_target=reference_target,
        cli_target=cli_target,
        reference_version_rc=ref_ctx.get("version_rc"))
    if control_state != CONTROL_MATCHED:
        ref_res = None

    if args.reference:
        print("   reference      : %s" % _spelled(args.reference_launcher, args.reference))
        print("   reference tgt  : %s  (MEASURED)"
              % (reference_target if reference_target else "<none>",))
    elif reference_target is not None:
        print("   reference      : ABSENT -- but --reference-target %s was passed anyway."
              % (reference_target,))
        print("                    Nothing was run for it; it attributes nothing.")
    else:
        print("   reference      : ABSENT")
    print("   control        : %s%s" % (control_state, "" if not control_reason else " --"))
    if control_reason:
        for line in _wrap(control_reason, 74):
            print("                    %s" % line)
    if control_state != CONTROL_MATCHED:
        print("                    => NO failure on this leg can be EXONERATED, and none")
        print("                       can be CHARGED TO DSS either: an unattributable")
        print("                       failure is RED and rc=%d, never a DSS verdict."
              % RC_UNATTRIBUTABLE)
    print("   launched       : %s -- %s" % ("YES" if launched else "NO", witness))
    print("   expecting      : version %s / source id %s"
          % (args.expect_version, args.expect_source_id))

    result = attribute(dss_res, ref_res, launched, control_state)

    for row in result.rows:
        print("   %s %-24s %s"
              % (ROW_MARKS.get(row["verdict"], "[X] "), row["assertion"], row["verdict"]))
        if not row["dss"]:
            print("        dss      : %s" % row["dssDetail"])
            print("        reference: %s" % row["referenceDetail"])

    if result.reference_only:
        print("   NOTE: the REFERENCE failed these while DSS passed: %s"
              % ", ".join(result.reference_only))
        print("         that is a fault in the reference build or its environment, not in DSS.")
    if not launched:
        # Repeated at the BOTTOM as well as the top: it changes what every row
        # above MEANS, and a run is read from its last lines.
        print("   >> THE SUBJECT NEVER LAUNCHED. It wrote nothing to its own stdout, so no")
        print("      instruction of generated code ran and NOTHING below is an observation")
        print("      about the compiler. Not one row is charged to DSS. The subject's own")
        print("      stderr, which is where a launcher writes its refusal, was:")
        for line in ((dss_ctx.get("version_stderr") or "").strip()[:600].splitlines()
                     or ["<empty>"]):
            print("        | %s" % line)
    elif control_state != CONTROL_MATCHED:
        print("   >> ATTRIBUTION WAS UNAVAILABLE FOR THIS ENTIRE RUN -- there was no matched")
        print("      control (%s). A failure here is RED and UNATTRIBUTED; it is not"
              % control_state)
        print("      evidence for or against DSS.")
    print("   %s -- %s" % (result.state, result.summary))
    if result.charged:
        print("   charged to DSS: %s" % ", ".join(result.charged))
    if result.exonerated:
        print("   exonerated (the matched control fails these identically): %s"
              % ", ".join(result.exonerated))
        print("   ...DSS is NOT implicated, but this leg's CLI is still RED: rc=%d." % RC_NOT_DSS)
    if result.unattributed:
        print("   unattributed (no matched control): %s" % ", ".join(result.unattributed))

    _write_json(args, {
        "label": args.label,
        # `ok` == (rc == 0). NOT "DSS is clean" - that question is `dssImplicated`
        # and it is allowed to answer null.
        "ok": result.ok, "rc": result.rc, "state": result.state, "summary": result.summary,
        "dssImplicated": result.dss_implicated,
        "legSpec": args.leg_spec, "legTarget": str(leg_target),
        "cliTarget": str(cli_target), "legTargetMismatch": False,
        "referenceTarget": str(reference_target) if reference_target else "",
        "controlState": result.control_state, "controlReason": control_reason,
        "subjectLaunched": result.subject_launched, "subjectLaunchWitness": witness,
        "charged": result.charged, "exonerated": result.exonerated,
        "referenceOnly": result.reference_only, "unattributed": result.unattributed,
        "subjectDidNotLaunch": result.did_not_launch,
        # A consumer must be able to tell "no oracle was asked for" from "an
        # oracle was asked for and could not be used" - they produce identical
        # attribution and mean different things.
        "hasReference": bool(args.reference),
        "referenceUsable": result.control_state == CONTROL_MATCHED,
        "referenceUnusableReason": control_reason,
        "assertions": result.rows,
    })
    return result.rc


def _wrap(text, width):
    out, line = [], ""
    for word in text.split():
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = word if not line else line + " " + word
    if line:
        out.append(line)
    return out


def _write_json(args, payload):
    if not args.json:
        return
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)
        f.write("\n")


# ─────────────────────────────────────────────────────────────────────────────
# THE SELF TEST
#
# ★★ THIS FILE HAD NO TEST OF ANY KIND UNTIL 2026-08-08, WHICH IS WHY IT SHIPPED
# TWO ATTRIBUTION DEFECTS AT ONCE. It is also the file that decides whether the
# compiler gets blamed, so "it looked right" is not a standard it is allowed to
# be held to. Run: `python3 cli-smoke.py --self-test`.
#
# What is asserted:
#   · the FULL CROSS PRODUCT of {launched, not launched} x {matched,
#     matched-but-unlaunched, target-mismatch, absent} x {row passes, row fails},
#     by ROW VERDICT NAME, rc, and dssImplicated - never by counting rows;
#   · RED-ON-DISABLE for both preconditions: defeat the launch witness, or force
#     the control to `matched`, and the table must CHANGE - stated as the exact
#     wrong verdict each defeat produces, because "something changed" is not a
#     test;
#   · a REGRESSION PIN for the stderr/stdout merge, driven through REAL
#     subprocesses. Nothing here re-types a subject's output into a stub: the two
#     live cases run a real child and feed ITS captured data to the real code.
# ─────────────────────────────────────────────────────────────────────────────

# A stand-in subject. It ignores argv, so every probe the fourteen assertions
# make behaves the same way - which is all these two cases need, because both are
# about what happens BEFORE any SQL could matter. It is NOT a fake sqlite3 and is
# not pretending to be one.
_FAKE_SUBJECT = '''\
import sys, time
try:
    sys.stdin.read()
except Exception:
    pass
if %(stdout_text)r:
    sys.stdout.write(%(stdout_text)r)
    sys.stdout.flush()
if %(stderr_text)r:
    sys.stderr.write(%(stderr_text)r)
    sys.stderr.flush()
if %(sleep)d:
    time.sleep(%(sleep)d)
sys.exit(%(rc)d)
'''


class _T(object):
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.failures = []

    def eq(self, what, got, want):
        if got == want:
            self.passed += 1
        else:
            self.failed += 1
            self.failures.append("%s\n        got : %r\n        want: %r" % (what, got, want))

    def truth(self, what, got):
        self.eq(what, bool(got), True)

    def raises(self, what, fn, needle):
        try:
            fn()
        except Exception as e:
            if needle in str(e):
                self.passed += 1
            else:
                self.failed += 1
                self.failures.append("%s\n        raised %s but the message never named %r: %s"
                                     % (what, type(e).__name__, needle, e))
            return
        self.failed += 1
        self.failures.append("%s\n        did NOT raise" % what)


def _table(fail_names, tag):
    """A FULL result table built from the SHIPPED assertion list, so it cannot
    drift out of step with it."""
    return {n: (n not in fail_names, "%s:%s" % (tag, n)) for n in ASSERTION_NAMES}


def _verdict_of(result, name):
    """A MISSING ROW IS RETURNED, NOT RAISED. ✔MEASURED 2026-08-08 while
    mutation-testing this file: one broken variant made `main` bail before
    running any assertion, and an unguarded subscript in the self test turned
    ~30 remaining checks into a traceback - the test harness reporting over what
    it could no longer observe. Every check gets a verdict, including this one."""
    for row in result.rows:
        if row["assertion"] == name:
            return row["verdict"]
    return "<NO ROW FOR %s>" % name


def _by_name(payload):
    """-> f(assertion name) -> verdict, or a readable sentinel. Same rule as
    `_verdict_of`: a missing row is a FAILED CHECK, never a KeyError that stops
    the checks after it from running."""
    table = dict((a["assertion"], a["verdict"]) for a in payload.get("assertions", []))
    return lambda name: table.get(name, "<NO ROW FOR %s>" % name)


def _write_fake(dirpath, stem, stdout_text, stderr_text, rc, sleep=0):
    path = os.path.join(dirpath, stem + ".py")
    with open(path, "w", encoding="utf-8") as f:
        f.write(_FAKE_SUBJECT % {"stdout_text": stdout_text, "stderr_text": stderr_text,
                                 "rc": rc, "sleep": sleep})
    return path


def _capture(fn):
    """Run fn() with stdout captured. -> (return value, printed text). The text is
    then ASSERTED ASCII: ✔MEASURED 2026-08-05, one U+2605 in a report line raised
    UnicodeEncodeError on a cp1252 Windows console and turned a verdict into a
    traceback at the last line of the run."""
    buf = io.StringIO()
    saved = sys.stdout
    sys.stdout = buf
    try:
        value = fn()
    finally:
        sys.stdout = saved
    return value, buf.getvalue()


def _capture_exit(fn):
    """-> (the code the process WOULD exit with, everything printed to stdout AND
    stderr).

    `main` reports usage errors two ways - argparse raises SystemExit, the
    pre-parse guard returns - and `sys.exit(main())` makes those the same thing,
    so this collapses them the same way. Catching SystemExit explicitly is the
    point: it is a BaseException, so it would otherwise sail past a test harness
    that catches Exception and take the whole self test down with it."""
    buf_out, buf_err = io.StringIO(), io.StringIO()
    saved = (sys.stdout, sys.stderr)
    sys.stdout, sys.stderr = buf_out, buf_err
    try:
        try:
            code = fn()
        except SystemExit as e:
            code = e.code
    finally:
        sys.stdout, sys.stderr = saved
    return code, buf_out.getvalue() + buf_err.getvalue()


def _is_ascii(text):
    try:
        text.encode("ascii")
        return True
    except UnicodeEncodeError:
        return False


def self_test():
    t = _T()
    PASSING = "version-runs"          # a row the subject passes in every case below
    FAILING = "select-rows"           # ...and one it fails, so verdicts differ BY ROW

    # ═══ 1. THE CROSS PRODUCT ════════════════════════════════════════════════
    # subject LAUNCHED x each control state x {the failing row, the passing row}.
    all_pass = _table(set(), "dss")
    one_fail = _table({FAILING}, "dss")
    ref_all_pass = _table(set(), "ref")
    ref_same_fail = _table({FAILING}, "ref")
    ref_other_fail = _table({PASSING}, "ref")

    # -- control MATCHED -------------------------------------------------------
    r = attribute(one_fail, ref_all_pass, True, CONTROL_MATCHED)
    t.eq("matched / subject fails, control passes -> DSS", _verdict_of(r, FAILING), V_DSS)
    t.eq("matched / the row the subject passes stays a pass", _verdict_of(r, PASSING), V_PASS)
    t.eq("matched / charged rc", r.rc, RC_CHARGED_TO_DSS)
    t.eq("matched / charged dssImplicated", r.dss_implicated, True)
    t.eq("matched / charged ok", r.ok, False)
    t.eq("matched / charged names the row", r.charged, [FAILING])

    r = attribute(one_fail, ref_same_fail, True, CONTROL_MATCHED)
    t.eq("matched / both fail -> upstream-or-environment",
         _verdict_of(r, FAILING), V_UPSTREAM_OR_ENV)
    t.eq("matched / exonerated rc", r.rc, RC_NOT_DSS)
    t.eq("matched / exonerated dssImplicated is FALSE, not None", r.dss_implicated, False)
    t.eq("matched / exonerated ok", r.ok, False)
    t.eq("matched / exonerated names the row", r.exonerated, [FAILING])

    r = attribute(all_pass, ref_other_fail, True, CONTROL_MATCHED)
    t.eq("matched / control-only fault -> pass (REFERENCE-ONLY fault)",
         _verdict_of(r, PASSING), V_PASS_REFERENCE_FAULT)
    t.eq("matched / control-only fault is still rc 0", r.rc, RC_PASS)
    t.eq("matched / control-only fault ok", r.ok, True)
    t.eq("matched / control-only fault dssImplicated", r.dss_implicated, False)
    t.eq("matched / control-only fault is reported", r.reference_only, [PASSING])

    # ★ THE ONE PRECEDENCE PAIR THAT IS OBSERVABLE AT ALL, AND IT WAS FOUND BY
    # MUTATION, NOT BY READING. Reordering the rc ladder is otherwise a no-op:
    # a non-launched subject empties every other bucket (the no-launch loop below
    # pins that), and `charged`/`unattributed` cannot co-occur by construction.
    # `charged` + `exonerated` CAN co-occur - one row DSS-only, another failing on
    # both - and then the ladder's order is the whole answer. Without this case an
    # implementation that reported rc 3 ("DSS is NOT implicated") on a run holding
    # a genuine DSS charge stayed green.
    both = attribute(_table({FAILING, "select-aggregate"}, "dss"),
                     _table({"select-aggregate"}, "ref"), True, CONTROL_MATCHED)
    t.eq("mixed / the DSS-only failure is charged", _verdict_of(both, FAILING), V_DSS)
    t.eq("mixed / the shared failure is exonerated in the SAME table",
         _verdict_of(both, "select-aggregate"), V_UPSTREAM_OR_ENV)
    t.eq("mixed / charged OUTRANKS exonerated: rc 1, not 3", both.rc, RC_CHARGED_TO_DSS)
    t.eq("mixed / dssImplicated is True", both.dss_implicated, True)
    t.eq("mixed / both buckets are still reported by name",
         (both.charged, both.exonerated), ([FAILING], ["select-aggregate"]))

    r = attribute(all_pass, ref_all_pass, True, CONTROL_MATCHED)
    t.eq("matched / all green -> every row passes",
         sorted(set(row["verdict"] for row in r.rows)), [V_PASS])
    t.eq("matched / all green rc", r.rc, RC_PASS)
    t.eq("matched / all green ok", r.ok, True)
    t.eq("matched / all green dssImplicated", r.dss_implicated, False)

    # -- control UNMATCHED, all three ways, must behave IDENTICALLY ------------
    for state in (CONTROL_DID_NOT_LAUNCH, CONTROL_TARGET_MISMATCH, CONTROL_ABSENT):
        r = attribute(one_fail, None, True, state)
        t.eq("%s / a failing row is UNATTRIBUTED, never DSS" % state,
             _verdict_of(r, FAILING), V_UNATTRIBUTED)
        t.eq("%s / a passing row is still a pass" % state,
             _verdict_of(r, PASSING), V_PASS)
        t.eq("%s / rc is 4, not 1" % state, r.rc, RC_UNATTRIBUTABLE)
        t.eq("%s / dssImplicated is None ('cannot say')" % state, r.dss_implicated, None)
        t.eq("%s / ok is False -- rc 4 is RED" % state, r.ok, False)
        t.eq("%s / nothing is charged to DSS" % state, r.charged, [])
        t.eq("%s / nothing is exonerated either" % state, r.exonerated, [])
        t.eq("%s / the row is named as unattributed" % state, r.unattributed, [FAILING])

        r = attribute(all_pass, None, True, state)
        t.eq("%s / all-green needs no oracle -> every row passes" % state,
             sorted(set(row["verdict"] for row in r.rows)), [V_PASS])
        t.eq("%s / all-green rc is 0" % state, r.rc, RC_PASS)
        t.eq("%s / all-green ok" % state, r.ok, True)
        t.eq("%s / all-green dssImplicated is FALSE" % state, r.dss_implicated, False)

    # -- subject NOT LAUNCHED, against every control state ---------------------
    for state in CONTROL_STATES:
        ref_for = ref_all_pass if state == CONTROL_MATCHED else None
        for tag, dss_table in (("fails", one_fail), ("passes", all_pass)):
            r = attribute(dss_table, ref_for, False, state)
            t.eq("no-launch / %s / %s -> EVERY row is subject-did-not-launch" % (state, tag),
                 sorted(set(row["verdict"] for row in r.rows)), [V_DID_NOT_LAUNCH])
            t.eq("no-launch / %s / %s -> rc 4" % (state, tag), r.rc, RC_UNATTRIBUTABLE)
            t.eq("no-launch / %s / %s -> dssImplicated None" % (state, tag),
                 r.dss_implicated, None)
            t.eq("no-launch / %s / %s -> ok False" % (state, tag), r.ok, False)
            t.eq("no-launch / %s / %s -> NOTHING charged" % (state, tag), r.charged, [])
            t.eq("no-launch / %s / %s -> nothing exonerated" % (state, tag), r.exonerated, [])
            t.eq("no-launch / %s / %s -> nothing counted as passed" % (state, tag),
                 r.passed, [])
            t.eq("no-launch / %s / %s -> the state names the cause" % (state, tag),
                 r.state, STATE_NO_LAUNCH)

    # ═══ 2. RED-ON-DISABLE: defeat each precondition, name what breaks ════════
    # (a) PIN subject_launched=True unconditionally. This is the MEASURED
    #     2026-08-08 defect reproduced exactly: an arm64 binary the loader
    #     refused, compared against a control, charged to the compiler.
    broken = attribute(one_fail, ref_all_pass, True, CONTROL_MATCHED)
    t.eq("RED-ON-DISABLE (launch witness pinned True): the failing row is CHARGED TO DSS",
         _verdict_of(broken, FAILING), V_DSS)
    t.eq("RED-ON-DISABLE (launch witness pinned True): rc becomes 1",
         broken.rc, RC_CHARGED_TO_DSS)
    t.eq("RED-ON-DISABLE (launch witness pinned True): DSS is implicated by a binary "
         "that never ran", broken.dss_implicated, True)
    fixed = attribute(one_fail, ref_all_pass, False, CONTROL_MATCHED)
    t.eq("...and with the witness INTACT the same inputs give subject-did-not-launch",
         _verdict_of(fixed, FAILING), V_DID_NOT_LAUNCH)
    t.truth("...i.e. the witness changes the verdict, so it is load-bearing",
            _verdict_of(broken, FAILING) != _verdict_of(fixed, FAILING)
            and broken.rc != fixed.rc)

    # (b) FORCE control_state="matched" on a control that targets something else.
    #     Same tables both times; only the gate differs. This is the pe64 shape:
    #     a Linux x86_64 ELF exonerating a Windows pe64 failure.
    forced = attribute(one_fail, ref_same_fail, True, CONTROL_MATCHED)
    t.eq("RED-ON-DISABLE (control forced 'matched'): a real failure is EXONERATED",
         _verdict_of(forced, FAILING), V_UPSTREAM_OR_ENV)
    t.eq("RED-ON-DISABLE (control forced 'matched'): rc becomes 3 -- 'NOT DSS'",
         forced.rc, RC_NOT_DSS)
    t.eq("RED-ON-DISABLE (control forced 'matched'): dssImplicated flips to a confident False",
         forced.dss_implicated, False)
    honest = attribute(one_fail, None, True, CONTROL_TARGET_MISMATCH)
    t.eq("...and with the target gate INTACT the same failure is UNATTRIBUTED",
         _verdict_of(honest, FAILING), V_UNATTRIBUTED)
    t.truth("...i.e. the target match changes the verdict, so it is load-bearing",
            _verdict_of(forced, FAILING) != _verdict_of(honest, FAILING)
            and forced.rc != honest.rc)

    # ═══ 3. FAIL LOUD: no default, ever ══════════════════════════════════════
    t.raises("an unrecognised control_state RAISES, naming the bad value",
             lambda: attribute(all_pass, None, True, "probably-fine"), "'probably-fine'")
    t.raises("...and lists the closed vocabulary",
             lambda: attribute(all_pass, None, True, ""), CONTROL_TARGET_MISMATCH)
    t.raises("claiming 'matched' with no control results RAISES",
             lambda: attribute(all_pass, None, True, CONTROL_MATCHED), "matched")
    t.raises("smuggling an UNMATCHED control's results through RAISES",
             lambda: attribute(all_pass, ref_all_pass, True, CONTROL_TARGET_MISMATCH),
             "target-mismatch")
    t.raises("a truthy-but-not-bool launch witness RAISES",
             lambda: attribute(all_pass, None, "yes", CONTROL_ABSENT), "subject_launched")
    t.raises("a partial subject table RAISES, naming the missing assertion",
             lambda: attribute({n: (True, "") for n in ASSERTION_NAMES[:-1]}, None,
                               True, CONTROL_ABSENT), ASSERTION_NAMES[-1])
    t.raises("a partial control table RAISES too",
             lambda: attribute(all_pass, {n: (True, "") for n in ASSERTION_NAMES[:-1]},
                               True, CONTROL_MATCHED), ASSERTION_NAMES[-1])

    # ═══ 4. THE TARGET STRINGS ═══════════════════════════════════════════════
    t.eq("a leg spec normalises to a triple",
         target_of_leg_spec("arm64:elf64-aarch64-linux-exec"),
         Target("arm64", "elf64", "linux"))
    t.eq("...taking the ARCH from the spec, NOT from the format (arm64 vs aarch64)",
         target_of_leg_spec("arm64:elf64-aarch64-linux-exec").arch, "arm64")
    t.eq("...and a bare (linkage-less) format still parses",
         target_of_leg_spec("x86_64:pe64-x86_64-windows"),
         Target("x86_64", "pe64", "windows"))
    t.eq("...for every leg the catalogue declares, positionally and with no table",
         [target_of_leg_spec(s) for s in ("x86_64:elf64-x86_64-linux-exec",
                                          "x86_64:pe64-x86_64-windows-exec",
                                          "arm64:macho64-arm64-darwin-exec")],
         [Target("x86_64", "elf64", "linux"), Target("x86_64", "pe64", "windows"),
          Target("arm64", "macho64", "darwin")])
    t.eq("a measured triple parses", parse_target("arm64:elf64:linux", "--cli-target"),
         Target("arm64", "elf64", "linux"))
    t.raises("a two-field triple RAISES, naming the value",
             lambda: parse_target("arm64:elf64", "--cli-target"), "'arm64:elf64'")
    t.raises("an empty field RAISES", lambda: parse_target("arm64::linux", "--cli-target"),
             "--cli-target")
    t.raises("an empty triple RAISES", lambda: parse_target("", "--cli-target"), "--cli-target")
    t.raises("a leg spec with no format RAISES, naming the value",
             lambda: target_of_leg_spec("arm64"), "'arm64'")
    t.raises("a leg spec whose format is too short RAISES, naming the format",
             lambda: target_of_leg_spec("arm64:elf64"), "'elf64'")

    # ═══ 5. classify_control -- where `matched` is actually decided ═══════════
    A = Target("arm64", "elf64", "linux")
    B = Target("x86_64", "elf64", "linux")
    t.eq("no reference -> absent",
         classify_control(False, None, A, None)[0], CONTROL_ABSENT)
    t.eq("a reference for another target -> target-mismatch (even when it ran fine)",
         classify_control(True, B, A, 0)[0], CONTROL_TARGET_MISMATCH)
    t.eq("...and the reason names BOTH targets",
         (str(A) in classify_control(True, B, A, 0)[1]
          and str(B) in classify_control(True, B, A, 0)[1]), True)
    t.eq("the right target but it never started -> did-not-launch",
         classify_control(True, A, A, 255)[0], CONTROL_DID_NOT_LAUNCH)
    t.eq("...and the reason names the rc", "255" in classify_control(True, A, A, 255)[1], True)
    t.eq("right target AND rc 0 -> matched", classify_control(True, A, A, 0)[0], CONTROL_MATCHED)
    t.eq("...with no reason to report", classify_control(True, A, A, 0)[1], "")
    t.eq("a reference that both mismatches AND failed to start reports the mismatch first",
         classify_control(True, B, A, 255)[0], CONTROL_TARGET_MISMATCH)
    t.truth("...while still mentioning that it also failed to start",
            "255" in classify_control(True, B, A, 255)[1])

    # ═══ 6. LIVE, THROUGH REAL SUBPROCESSES ══════════════════════════════════
    # Everything above is algebra. These two run a real child through the real
    # Runner and feed ITS OWN captured output to the real assertions - the two
    # facts the algebra takes on trust have to be measured somewhere.
    tmp = tempfile.mkdtemp(prefix="cli-smoke-selftest-")
    try:
        VER = "3.54.0"
        SRCID = "2026-01-01 00:00:00 deadbeefcafe"
        BANNER = "%s %s\n" % (VER, SRCID)

        # (A) THE MEASURED 2026-08-08 DEFECT, in miniature: the subject never
        #     runs and the refusal arrives on STDERR, exactly as every declared
        #     launcher delivers it.
        refusal = _write_fake(tmp, "refused", "", "loader: cannot open the interpreter\n", 255)
        res_a, ctx_a = run_all(Runner("dss", refusal, [sys.executable],
                                      os.path.join(tmp, "wd-a")), VER, SRCID)
        t.eq("live/no-launch: nothing reached the subject's own stdout",
             ctx_a.get("version_stdout"), "")
        t.truth("live/no-launch: the refusal WAS captured -- on stderr",
                "cannot open the interpreter" in (ctx_a.get("version_stderr") or ""))
        t.eq("live/no-launch: the witness says NOT launched",
             subject_launched_from_ctx(ctx_a), False)
        t.eq("live/no-launch: a01 failed, so this is a genuinely red run",
             res_a["version-runs"][0], False)
        live = attribute(res_a, ref_all_pass, subject_launched_from_ctx(ctx_a), CONTROL_MATCHED)
        t.eq("live/no-launch: EVERY row is subject-did-not-launch",
             sorted(set(row["verdict"] for row in live.rows)), [V_DID_NOT_LAUNCH])
        t.eq("live/no-launch: rc 4, not 1", live.rc, RC_UNATTRIBUTABLE)
        t.eq("live/no-launch: nothing is charged to the compiler", live.charged, [])
        t.eq("live/no-launch: dssImplicated is None", live.dss_implicated, None)
        pinned = attribute(res_a, ref_all_pass, True, CONTROL_MATCHED)
        t.eq("live/RED-ON-DISABLE: pin the witness True and the SAME real run is "
             "charged to DSS", pinned.rc, RC_CHARGED_TO_DSS)
        t.truth("...on 14 rows of a binary that never executed",
                len(pinned.charged) == len(ASSERTION_NAMES))

        # (B) THE MERGE TRAP. shell.c's guard goes to STDERR while the banner
        #     goes to STDOUT and the process exits 0. a04 MUST fail (it reads the
        #     MERGED capture) and the subject MUST count as launched.
        guard = "SQLite header and source version mismatch\n"
        mixed = _write_fake(tmp, "guarded", BANNER, guard, 0)
        res_b, ctx_b = run_all(Runner("dss", mixed, [sys.executable],
                                      os.path.join(tmp, "wd-b")), VER, SRCID)
        t.eq("live/merge: the subject DID launch (it wrote its banner to stdout)",
             subject_launched_from_ctx(ctx_b), True)
        t.eq("live/merge: a01 passes -- the process ran and exited 0",
             res_b["version-runs"][0], True)
        t.eq("live/merge: a02 still reads the version out of the merged text",
             res_b["version-token-exact"][0], True)
        t.eq("live/merge: a03 still reads the source id out of it",
             res_b["source-id-token-exact"][0], True)
        t.eq("live/merge: a04 FAILS on the stderr-only guard", res_b["no-version-mismatch"][0], False)
        t.truth("live/merge: ...and says why, quoting the guard",
                "version mismatch" in res_b["no-version-mismatch"][1])
        live_b = attribute(res_b, None, subject_launched_from_ctx(ctx_b), CONTROL_ABSENT)
        t.eq("live/merge: a04 is UNATTRIBUTED (no control), NOT subject-did-not-launch",
             _verdict_of(live_b, "no-version-mismatch"), V_UNATTRIBUTED)
        t.eq("live/merge: and a04's sibling a01 is a pass, so the run is not blanket-red",
             _verdict_of(live_b, "version-runs"), V_PASS)
        # RED-ON-DISABLE for the merge itself: hand a04 the STDOUT-ONLY capture
        # this very run produced - the exact "cleanup" the comments warn against -
        # and watch the assertion go green on a binary that refuses to run.
        cleaned = dict(ctx_b)
        cleaned["version_out"] = ctx_b["version_stdout"]
        t.eq("live/RED-ON-DISABLE (merge): a04 against the STDOUT-ONLY capture PASSES, "
             "which is why the merge must stay",
             a04_no_version_mismatch(None, cleaned)[0], True)
        t.truth("...i.e. splitting the streams flips a04 from red to green on the same binary",
                a04_no_version_mismatch(None, cleaned)[0] != res_b["no-version-mismatch"][0])

        # (C) THE PROCESS THAT COULD NOT BE STARTED AT ALL - a DIFFERENT arm from
        #     (A), and the more dangerous one. FOUND BY MUTATION: putting the
        #     `<COULD NOT EXECUTE ...>` placeholder in run_split's STDOUT slot
        #     (which is what `run` legitimately does with the MERGED text) left
        #     the whole battery green, while making a binary that never started
        #     look launched - i.e. fourteen rows charged to the compiler for a
        #     wrong path. Nothing exercised this arm until now.
        missing_exe = os.path.join(tmp, "there-is-no-such-binary")
        r_missing = Runner("dss", missing_exe, [], os.path.join(tmp, "wd-c"))
        rc_m, merged_m = r_missing.run(["--version"])
        rc_s, out_s, err_s = r_missing.run_split(["--version"])
        t.eq("live/no-exec: the MERGED capture reports the failure as text (rc -1)", rc_m, -1)
        t.truth("live/no-exec: ...and that text is this file's own placeholder",
                "<COULD NOT EXECUTE" in merged_m)
        t.eq("live/no-exec: run_split leaves STDOUT EMPTY -- never the placeholder",
             out_s, "")
        t.truth("live/no-exec: the placeholder goes to the STDERR slot instead",
                "<COULD NOT EXECUTE" in err_s)
        t.eq("live/no-exec: run_split rc", rc_s, -1)
        res_c, ctx_c = run_all(r_missing, VER, SRCID)
        t.eq("live/no-exec: the witness says NOT launched",
             subject_launched_from_ctx(ctx_c), False)
        live_c = attribute(res_c, ref_all_pass, subject_launched_from_ctx(ctx_c),
                           CONTROL_MATCHED)
        t.eq("live/no-exec: a wrong path is NOT a compiler defect",
             (live_c.rc, live_c.charged, live_c.dss_implicated),
             (RC_UNATTRIBUTABLE, [], None))

        # (C2) ONE BYTE IS ONE BYTE. The witness counts, it does not judge: a
        #      subject whose stdout holds only a newline DID write to stdout, and
        #      a strip() here would quietly reclassify it as never having
        #      launched. FOUND BY MUTATION - the rule was documented in the
        #      predicate's docstring and pinned by nothing, so `.strip()` could be
        #      added as a tidy-up with the whole battery staying green.
        whitespace = _write_fake(tmp, "whitespace-only", "\n",
                                 "loader: refused, and it happened after a newline\n", 255)
        _res_w, ctx_w = run_all(Runner("dss", whitespace, [sys.executable],
                                       os.path.join(tmp, "wd-c2")), VER, SRCID)
        t.eq("live/one-byte: the subject wrote exactly one char to stdout",
             len(ctx_w.get("version_stdout") or ""), 1)
        t.eq("live/one-byte: ...so it LAUNCHED -- the witness counts bytes, it does "
             "not strip them", subject_launched_from_ctx(ctx_w), True)

        # (D) LAUNCHED, THEN HUNG. The comment on run_split claims a subject that
        #     printed its banner and then stalled DID launch; an untested claim in
        #     a comment is how a01 got its placeholder bug. STEP_TIMEOUT is lowered
        #     for this one probe so the pin costs a second rather than five minutes.
        stalled = _write_fake(tmp, "stalled", BANNER, "", 0, sleep=30)
        saved_timeout = globals()["STEP_TIMEOUT"]
        globals()["STEP_TIMEOUT"] = 1
        try:
            ctx_d = {"expect_version": VER, "expect_source_id": SRCID}
            a01_version_runs(Runner("dss", stalled, [sys.executable],
                                    os.path.join(tmp, "wd-d")), ctx_d)
        finally:
            globals()["STEP_TIMEOUT"] = saved_timeout
        t.eq("live/timeout: the merged capture reports a timeout (rc -2)",
             ctx_d.get("version_rc"), -2)
        t.truth("live/timeout: ...as this file's own placeholder text",
                "<TIMED OUT" in (ctx_d.get("version_out") or ""))
        t.truth("live/timeout: the PARTIAL stdout the child had already written is KEPT",
                VER in (ctx_d.get("version_stdout") or ""))
        t.eq("live/timeout: so a subject that ran and then hung counts as LAUNCHED",
             subject_launched_from_ctx(ctx_d), True)

        # (E) main() END TO END - the argv surface, the preconditions, the report
        #     and the JSON, exercised as the drivers exercise them.
        wrong_json = os.path.join(tmp, "wrong-target.json")
        rc_e, text_e = _capture(lambda: main([
            "--cli", missing_exe,
            "--leg-spec", "arm64:elf64-aarch64-linux-exec",
            "--cli-target", "x86_64:pe64:windows",
            "--expect-version", VER, "--expect-source-id", SRCID,
            "--workdir", os.path.join(tmp, "wd-e"), "--json", wrong_json,
            "--label", "selftest-wrong-target"]))
        j_e = json.load(open(wrong_json, encoding="utf-8"))
        t.eq("main/wrong-target: rc 4", rc_e, RC_UNATTRIBUTABLE)
        t.eq("main/wrong-target: the mismatch is flagged", j_e.get("legTargetMismatch"), True)
        t.eq("main/wrong-target: NOT ONE assertion was executed", j_e.get("assertions"), [])
        t.eq("main/wrong-target: nothing charged, and no claim either way",
             (j_e.get("charged"), j_e.get("dssImplicated"), j_e.get("ok")), ([], None, False))
        t.truth("main/wrong-target: the report names BOTH targets",
                "arm64:elf64:linux" in text_e and "x86_64:pe64:windows" in text_e)
        t.truth("main/wrong-target: the report is ASCII-only (cp1252 consoles)",
                _is_ascii(text_e))

        live_json = os.path.join(tmp, "no-launch.json")
        rc_f, text_f = _capture(lambda: main([
            "--cli", refusal, "--launcher=" + sys.executable,
            "--leg-spec", "arm64:elf64-aarch64-linux-exec",
            "--cli-target", "arm64:elf64:linux",
            "--expect-version", VER, "--expect-source-id", SRCID,
            "--workdir", os.path.join(tmp, "wd-f"), "--json", live_json,
            "--label", "selftest-no-launch"]))
        j_f = json.load(open(live_json, encoding="utf-8"))
        t.eq("main/no-launch: rc 4 -- NOT the rc 1 this leg used to report", rc_f,
             RC_UNATTRIBUTABLE)
        t.eq("main/no-launch: every row is subject-did-not-launch",
             sorted(set(a["verdict"] for a in j_f.get("assertions"))), [V_DID_NOT_LAUNCH])
        t.eq("main/no-launch: all 14 rows are present", len(j_f.get("assertions")),
             len(ASSERTION_NAMES))
        t.eq("main/no-launch: ok is False and dssImplicated is null",
             (j_f.get("ok"), j_f.get("dssImplicated")), (False, None))
        t.eq("main/no-launch: the launch fact is recorded", j_f.get("subjectLaunched"), False)
        t.eq("main/no-launch: with no --reference the control is 'absent'",
             (j_f.get("controlState"), j_f.get("hasReference"), j_f.get("referenceUsable")),
             (CONTROL_ABSENT, False, False))
        t.eq("main/no-launch: the declared and measured targets are both recorded",
             (j_f.get("legSpec"), j_f.get("legTarget"), j_f.get("cliTarget"), j_f.get("legTargetMismatch")),
             ("arm64:elf64-aarch64-linux-exec", "arm64:elf64:linux", "arm64:elf64:linux", False))
        t.truth("main/no-launch: the report says so in words, at the bottom",
                "THE SUBJECT NEVER LAUNCHED" in text_f)
        t.truth("main/no-launch: and quotes the subject's own stderr",
                "cannot open the interpreter" in text_f)
        t.truth("main/no-launch: the report is ASCII-only (cp1252 consoles)",
                _is_ascii(text_f))
        t.truth("main/no-launch: --launcher=<token> reached the child -- the child's "
                "own stderr came back through it",
                any("cannot open the interpreter" in a["dssDetail"]
                    for a in j_f.get("assertions", [])))

        # ── USAGE ERRORS EXIT 2, WHICH IS NOT ONE OF THE VERDICT CODES ───────
        # An argv defect must never be able to read as a verdict about the
        # compiler. Both of these used to be impossible to get wrong because
        # neither argument existed; both are now load-bearing.
        # (E2) --reference-target WITHOUT --reference. Tolerated (a driver may
        #      always pass the leg's target and only sometimes have a reference
        #      built) but ANNOUNCED, never silent. Pinned because the line that
        #      announces it formats a Target, which is the exact `"%s" % tuple`
        #      hazard that this self test caught in the report path on its first
        #      run: a print-only branch still crashes a real leg.
        orphan_json = os.path.join(tmp, "orphan-target.json")
        rc_o, text_o = _capture(lambda: main([
            "--cli", refusal, "--launcher=" + sys.executable,
            "--reference-target", "arm64:elf64:linux",
            "--leg-spec", "arm64:elf64-aarch64-linux-exec",
            "--cli-target", "arm64:elf64:linux",
            "--expect-version", VER, "--expect-source-id", SRCID,
            "--workdir", os.path.join(tmp, "wd-o"), "--json", orphan_json,
            "--label", "selftest-orphan-target"]))
        j_o = json.load(open(orphan_json, encoding="utf-8"))
        t.eq("main/orphan target: still 'absent', with no reference run",
             (j_o.get("controlState"), j_o.get("hasReference"), rc_o),
             (CONTROL_ABSENT, False, RC_UNATTRIBUTABLE))
        t.truth("main/orphan target: it is ANNOUNCED, not swallowed",
                "was passed anyway" in text_o)
        t.truth("main/orphan target: the report survives formatting a Target and is ASCII",
                _is_ascii(text_o) and "arm64:elf64:linux" in text_o)

        # (F) main() WITH A REFERENCE, both ways round. This is the only place the
        #     DEMOTION line (`unmatched -> ref_res = None`) executes, and it is
        #     the line that decides whether a failure gets exonerated by a binary
        #     for another target - the MEASURED 2026-08-08 defect, end to end.
        mm_json = os.path.join(tmp, "mismatch.json")
        rc_mm, text_mm = _capture(lambda: main([
            "--cli", mixed, "--launcher=" + sys.executable,
            "--reference", mixed, "--reference-launcher=" + sys.executable,
            "--reference-target", "x86_64:elf64:linux",
            "--leg-spec", "arm64:elf64-aarch64-linux-exec",
            "--cli-target", "arm64:elf64:linux",
            "--expect-version", VER, "--expect-source-id", SRCID,
            "--workdir", os.path.join(tmp, "wd-mm"), "--json", mm_json,
            "--label", "selftest-mismatched-control"]))
        j_mm = json.load(open(mm_json, encoding="utf-8"))
        by_name_mm = _by_name(j_mm)
        t.eq("main/mismatched control: the control is DEMOTED, not used",
             (j_mm.get("controlState"), j_mm.get("referenceUsable"), j_mm.get("hasReference")),
             (CONTROL_TARGET_MISMATCH, False, True))
        t.eq("main/mismatched control: a04 is UNATTRIBUTED, not exonerated",
             by_name_mm("no-version-mismatch"), V_UNATTRIBUTED)
        t.eq("main/mismatched control: the subject DID launch, so no did-not-launch row",
             j_mm.get("subjectLaunched"), True)
        t.eq("main/mismatched control: rc 4 and dssImplicated null",
             (rc_mm, j_mm.get("dssImplicated"), j_mm.get("ok")), (RC_UNATTRIBUTABLE, None, False))
        t.eq("main/mismatched control: nothing exonerated by a foreign target",
             (j_mm.get("exonerated"), j_mm.get("charged")), ([], []))
        t.truth("main/mismatched control: the reason names both targets",
                "x86_64:elf64:linux" in j_mm.get("referenceUnusableReason")
                and "arm64:elf64:linux" in j_mm.get("referenceUnusableReason"))
        t.truth("main/mismatched control: report is ASCII", _is_ascii(text_mm))

        ok_json = os.path.join(tmp, "matched.json")
        rc_ok, text_ok = _capture(lambda: main([
            "--cli", mixed, "--launcher=" + sys.executable,
            "--reference", mixed, "--reference-launcher=" + sys.executable,
            "--reference-target", "arm64:elf64:linux",
            "--leg-spec", "arm64:elf64-aarch64-linux-exec",
            "--cli-target", "arm64:elf64:linux",
            "--expect-version", VER, "--expect-source-id", SRCID,
            "--workdir", os.path.join(tmp, "wd-ok"), "--json", ok_json,
            "--label", "selftest-matched-control"]))
        j_ok = json.load(open(ok_json, encoding="utf-8"))
        by_name_ok = _by_name(j_ok)
        t.eq("main/matched control: the SAME reference at the SAME target IS used",
             (j_ok.get("controlState"), j_ok.get("referenceUsable"), j_ok.get("referenceUnusableReason")),
             (CONTROL_MATCHED, True, ""))
        t.eq("main/matched control: the shared a04 failure is EXONERATED",
             by_name_ok("no-version-mismatch"), V_UPSTREAM_OR_ENV)
        t.eq("main/matched control: rc 3 -- red, but explicitly not DSS",
             (rc_ok, j_ok.get("dssImplicated"), j_ok.get("ok")), (RC_NOT_DSS, False, False))
        t.eq("main/matched control: nothing is charged", j_ok.get("charged"), [])
        t.truth("main/matched control: report is ASCII", _is_ascii(text_ok))
        t.truth("main/matched control: the SAME inputs differ ONLY by the control's "
                "declared target, and that alone moves rc from 4 to 3",
                rc_mm != rc_ok)

        def _argv(**over):
            a = {"--cli": refusal, "--leg-spec": "arm64:elf64-aarch64-linux-exec",
                 "--cli-target": "arm64:elf64:linux", "--expect-version": VER,
                 "--expect-source-id": SRCID, "--workdir": os.path.join(tmp, "wd-g")}
            a.update({k.replace("_", "-"): v for k, v in over.items()})
            out = []
            for k, v in a.items():
                out += [k if k.startswith("--") else "--" + k, v]
            return out

        code_g, text_g = _capture_exit(lambda: main(_argv(reference=refusal)))
        t.eq("main: --reference without --reference-target exits 2 (usage), not 1",
             code_g, 2)
        t.truth("...naming the option it wants", "--reference-target" in text_g)
        code_h, text_h = _capture_exit(lambda: main(_argv(cli_target="arm64-elf64-linux")))
        t.eq("main: an unparseable --cli-target exits 2, never a traceback (rc 1 "
             "would read as CHARGED TO DSS)", code_h, 2)
        t.truth("...quoting the bad value back", "'arm64-elf64-linux'" in text_h)
        code_i, text_i = _capture_exit(lambda: main(_argv(leg_spec="arm64")))
        t.eq("main: an unparseable --leg-spec exits 2 as well", code_i, 2)
        t.truth("...quoting that bad value too", "'arm64'" in text_i)
        code_j, _ = _capture_exit(lambda: main(["--self-test", "--cli", "x"]))
        t.eq("main: --self-test refuses to run alongside other arguments", code_j, 2)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ═══ 7. THE CLOSED SETS ══════════════════════════════════════════════════
    t.eq("the control vocabulary is exactly four names", list(CONTROL_STATES),
         ["matched", "target-mismatch", "did-not-launch", "absent"])
    t.eq("the verdict set is exactly six names", list(VERDICTS),
         ["pass", "pass (REFERENCE-ONLY fault)", "upstream-or-environment", "DSS",
          "unattributed (NO MATCHED CONTROL)", "subject-did-not-launch"])
    t.eq("the exit codes are 0/1/3/4",
         [RC_PASS, RC_CHARGED_TO_DSS, RC_NOT_DSS, RC_UNATTRIBUTABLE], [0, 1, 3, 4])

    print("cli-smoke.py --self-test: %d passed, %d failed" % (t.passed, t.failed))
    for f in t.failures:
        print("  FAIL %s" % f)
    if t.failed:
        print("cli-smoke.py --self-test: FAILED")
        return 1
    if t.passed == 0:
        # An empty battery reporting success is the same vacuity this file exists
        # to refuse. It cannot happen quietly.
        print("cli-smoke.py --self-test: NO ASSERTION RAN -- that is a failure, not a pass")
        return 1
    print("cli-smoke.py --self-test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
