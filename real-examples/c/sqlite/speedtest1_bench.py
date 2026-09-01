#!/usr/bin/env python3
"""speedtest1_bench.py — the MEASUREMENT CORE of the DSS-vs-reference benchmark.

Subject: SQLite's OWN performance program, `test/speedtest1.c`, built FROM FULL
SOURCE — the same ~103 translation units the reference `sqlite3` CLI is built
from, with `shell.c` swapped for `speedtest1.c`. NOT the amalgamation.

⚠ UPSTREAM SHIPS NO FULL-SOURCE speedtest1 RECIPE, and that is a measured fact
rather than an oversight to route around: `main.mk`'s `speedtest1` target and
`Makefile.msc`'s `speedtest1.exe` target BOTH link `$(SQLITE3C)` / `sqlite3.c`,
the amalgamation. So the TU list here is derived from the FULL-SOURCE `sqlite3`
CLI recipe (`make -n sqlite3`, the derivation `base-harness.sh` already owns and
`build-and-test.{sh,ps1}` already prove daily) and then has its one artifact-
specific TU substituted. That substitution is the ONLY difference, and the
refusals below check it rather than trusting it.

★★ WHY THE MEASUREMENT LIVES HERE AND NOT IN THE TWO DRIVERS. A `.sh` and a
`.ps1` that each timed their own builds would be two implementations of one
number, and this repository has already measured what that costs — `base-
harness.sh` exists because three hand-kept copies of one decision had drifted
three ways. The drivers resolve the ENVIRONMENT (where SQLite is, where dss is,
what the recipe says); everything that produces a NUMBER is in this file, so the
two drivers cannot report differently.

THE DISCIPLINE, each clause measured and each one having bitten someone:

- MONOTONIC clock only, both reads inside THIS process. Never a wall-clock date:
  WSL2's CLOCK_REALTIME oscillates ±34.47 s every ~5 s on this project's own
  hosts (project_wsl2_clock_realtime_broken_2026_08_01), and the gcc reference
  suffers it identically — so a wall-clock differential there measures the
  hypervisor, not the compiler. Same rule `sqlite-runtime-bench.py` states.
- COLD BUILDS. Every build repeat gets a FRESH object/output directory. A second
  build into a warm tree measures the build system's change detection, not the
  compiler.
- WARM-UP RUN, never counted, before the timed run repeats — first-touch page
  cache is not the program.
- MEDIAN over repeats, with min/max and the raw samples printed. One sample is
  an anecdote; a mean hides the outlier that says the machine was busy.
- THE ARMS MUST HAVE DONE THE SAME WORK, AND UPSTREAM ALREADY SHIPS THE
  INSTRUMENT THAT SAYS SO. `speedtest1 --verify` prints a hash over the query
  results, and upstream's own comment on it reads "Hash algorithm used to verify
  that compilation is not miscompiled" — so it is not a benchmark bolt-on, it is
  the reference project's own miscompile detector. `--verify` is therefore ON by
  default here. Every arm's output is normalized (elapsed times stripped,
  alignment whitespace collapsed) and compared including that hash; arms that
  disagree are a REFUSAL, not a footnote. A benchmark whose subjects computed
  different things is not a comparison — it is three programs.

★ WHAT IS DELIBERATELY NOT EQUALIZED, and is reported instead of hidden: DSS
compiles all CUs inside ONE process on a worker-thread pool (`--jobs N`), while
gcc and cl are driven as N concurrent `-c` PROCESSES plus a link. That IS the
architectural difference under measurement — flattening it would require either
crippling DSS to one CU per process or pretending gcc has an in-process pool.
The report names the mechanism next to every number.

Refusals (all fail-loud, all exercised by --selftest):
  R1  a plan TU that is not on disk
  R2  a TU list below the floor — a collapsed subject reports a fast build
  R3  the main TU is not speedtest1.c — the subject was substituted wrongly
  R4  a source tree reached over a UNC / \\wsl$ share while measuring natively
  R5  an arm that fails to build is NAMED and the run continues (the harness
      must survive everything); zero surviving arms is a non-zero exit
  R6  two arms whose normalized speedtest1 output differs
  R7  repeats < 1, or a jobs arm < 1

Exit codes: 0 measured · 1 a refusal · 2 usage · 3 no arm produced a binary.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

# ── floors ───────────────────────────────────────────────────────────────────
# A guard whose corpus collapses to nothing reports a clean pass over something
# it never read. The full-source CLI recipe is ~103 TUs on every leg this
# project builds; 50 is far below the live count and far above "the derivation
# broke and handed us four files".
MIN_TUS = 50
MAIN_TU_BASENAME = "speedtest1.c"

# speedtest1 prints one line per test case ending in an elapsed time, then a
# TOTAL. The times are what we are MEASURING, so they are exactly what must not
# take part in the "did the arms do the same work" comparison.
#
# ⚠ ERASING THE TIME IS NOT ENOUGH, AND THE FIRST DRAFT OF THIS FILE GOT IT
# WRONG. speedtest1's format string is `"%4d.%03ds\n"` — the seconds are RIGHT-
# ALIGNED IN A FIELD OF FOUR, so a 0.123 s run and a 1234.567 s run differ in
# the PADDING before the number as well as in the number. Substituting the time
# alone left `TOTAL....    <t>` against `TOTAL....  <t>`, and the arms would have
# been declared to have done different work purely because one was faster. The
# whitespace runs are collapsed for that reason, not for tidiness.
#
# ⚠⚠ AND A NEGATIVE ELAPSED TIME IS STILL AN ELAPSED TIME, WHICH THE FIRST
# PATTERN HERE (`\b\d+\.\d+s\b`) COULD NOT MATCH.
# [[D-TEST-SPEEDTEST1-NORMALIZER-LETS-A-BACKWARD-CLOCK-LOOK-LIKE-DIFFERENT-WORK]]
# `"%4d.%03ds"` formats a NEGATIVE delta with a sign in BOTH fields, so a clock
# that steps backwards mid-run prints `-29.-29s` — no leading word boundary and a
# `-` after the dot, so the old pattern skipped it and the corrupted token
# survived into the normalized text. R6 then compared it against the other arm's
# `<t>` and declared *"the two binaries did not do the same work"*, which is a
# false accusation against both compilers.
#
# ✔MEASURED 2026-08-28 on the WSL x86_64 leg: the gcc arm printed
# `160 - 100 DELETEs using rowid.... -29.-29s` and the whole benchmark refused
# with R6. ★ This host family's WSL2 `CLOCK_REALTIME` is the known cause — it
# oscillates ±34.47 s every ~5 s, which this file's own header already warns
# about two screens above, for the harness's OWN timing. The gap was that the
# warning had never been extended to the SUBJECT's self-reported times.
#
# ⇒ Both spellings are normalized, so R6 goes back to comparing WORK (test
# names, order, counts, the `--verify` hash). ⓘ It does NOT invalidate the run:
# the numbers this harness reports come from `time.monotonic()` in this process,
# never from speedtest1's internal timer — but a host whose clock runs backwards
# is worth saying out loud, so `malformed_elapsed_times()` below lets the caller
# report it instead of silently swallowing it.
_ELAPSED = re.compile(r"(?<![\w.])-?\d+\.-?\d+s\b")
_ELAPSED_BACKWARD = re.compile(r"-\d+\.-?\d+s|\d+\.-\d+s")
_RUNS = re.compile(r"[ \t]+")


def malformed_elapsed_times(text: str) -> int:
    """How many elapsed times in `text` carry a negative field.

    Non-zero means the HOST's clock stepped backwards during the run, not that
    the program is wrong. Reported rather than hidden.
    """
    return len(_ELAPSED_BACKWARD.findall(text))


def die(msg: str, code: int = 1) -> "NoReturn":  # type: ignore[valid-type]
    print(f"speedtest1_bench: {msg}", file=sys.stderr)
    sys.exit(code)


def normalize_output(text: str) -> str:
    """speedtest1 stdout with every elapsed time replaced by a placeholder.

    What survives is the test names, their order, any counts the program prints,
    and — under `--verify` — the Verification Hash: exactly the evidence that two
    binaries ran the same workload and got the same answers.
    """
    out = []
    for line in text.replace("\r\n", "\n").split("\n"):
        line = _RUNS.sub(" ", _ELAPSED.sub("<t>", line)).strip()
        if line:
            out.append(line)
    return "\n".join(out)


# ── timing ───────────────────────────────────────────────────────────────────
def timed(fn) -> tuple[float, object]:
    """Run `fn`, return (monotonic seconds, its result).

    Both clock reads are in this process, which is the only place
    `time.monotonic()` differences are defined.
    """
    t0 = time.monotonic()
    result = fn()
    return time.monotonic() - t0, result


def stats(samples: list[float]) -> dict:
    return {
        "median": statistics.median(samples),
        "min": min(samples),
        "max": max(samples),
        "samples": samples,
    }


def run_capture(argv: list[str], env: dict | None = None, cwd: str | None = None):
    return subprocess.run(argv, capture_output=True, text=True, errors="replace",
                          env=env, cwd=cwd)


# ── MSVC environment ─────────────────────────────────────────────────────────
# ★ RESOLVED HERE, IN THE SHARED CORE, ON PURPOSE. `cl.exe` is useless without
# INCLUDE/LIB/PATH, and those come from `vcvarsall.bat`. Had each driver
# resolved them, the PowerShell twin would have had an MSVC arm and the POSIX
# twin (Git Bash on the same Windows host) would silently have had none — one
# driver enforcing while its sibling shrugs is this project's named
# silent-harness shape. One implementation, both drivers, same arm.
VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"


def msvc_env(arch: str = "x64") -> tuple[dict | None, str]:
    """(environment for cl.exe, reason-if-absent). Never raises."""
    if platform.system() != "Windows":
        return None, "not a Windows host — cl.exe does not exist here"
    # Already inside a developer prompt: use it as-is rather than shelling out.
    if os.environ.get("INCLUDE") and shutil.which("cl"):
        return dict(os.environ), ""
    if not os.path.isfile(VSWHERE):
        return None, f"vswhere.exe not found at {VSWHERE}"
    r = run_capture([VSWHERE, "-latest", "-products", "*",
                     "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                     "-property", "installationPath"])
    root = (r.stdout or "").strip().splitlines()
    if r.returncode != 0 or not root:
        return None, f"vswhere reported no VC toolset (rc={r.returncode})"
    vcvars = os.path.join(root[0], "VC", "Auxiliary", "Build", "vcvarsall.bat")
    if not os.path.isfile(vcvars):
        return None, f"vcvarsall.bat not found under {root[0]}"
    # `set` after vcvars, harvested through cmd.exe — the only supported way to
    # learn what that batch file did.
    #
    # ⚠ VIA A FILE, NOT AN INLINE `cmd.exe /c "…"`, AND THAT IS MEASURED. The
    # inline form needs the vcvarsall path quoted (it lives under "Program
    # Files"), but `subprocess` escapes an embedded quote as `\"`, which cmd.exe
    # does not understand — so the command silently became a bare `cmd.exe`
    # printing its version banner and exiting 0-or-1 with no environment at all.
    # It read as "this host has no MSVC". Writing the script to a file and
    # running the file is this repository's standing answer to exactly this
    # class of quoting trap.
    env = {}
    fd, bat = tempfile.mkstemp(suffix=".bat", prefix="dss-vcvars-")
    try:
        with os.fdopen(fd, "w", newline="\r\n") as fh:
            fh.write("@echo off\r\n"
                     f'call "{vcvars}" {arch} >NUL\r\n'
                     "if errorlevel 1 exit /b 1\r\n"
                     "set\r\n")
        r = run_capture(["cmd.exe", "/c", bat])
    finally:
        with __import__("contextlib").suppress(OSError):
            os.remove(bat)
    if r.returncode != 0:
        return None, (f"vcvarsall.bat {arch} failed (rc={r.returncode}): "
                      + (r.stdout or r.stderr or "")[:200].replace("\n", " "))
    for line in (r.stdout or "").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    if "INCLUDE" not in env:
        return None, "vcvarsall.bat produced no INCLUDE — the toolset is incomplete"
    return env, ""


# ── build arms ───────────────────────────────────────────────────────────────
def _compile_pool(jobs: int, make_argv, tus: list[str], env: dict | None,
                  cwd: str | None) -> tuple[bool, str]:
    """Compile every TU with `jobs` concurrent processes. (ok, first-failure)."""
    failure = ""
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(run_capture, make_argv(tu), env, cwd): tu for tu in tus}
        for fut in concurrent.futures.as_completed(futures):
            r = fut.result()
            if r.returncode != 0 and not failure:
                tu = futures[fut]
                failure = (f"{os.path.basename(tu)}: rc={r.returncode}\n"
                           + (r.stderr or r.stdout or "")[:1200])
    return (not failure), failure


def build_unix_cc(arm: dict, subject: dict, jobs: int, objdir: str,
                  binpath: str) -> tuple[bool, str]:
    """gcc / clang: `jobs` concurrent `-c` processes, then one link."""
    cc = arm["bin"]
    inc = [f"-I{d}" for d in subject["includes"]]
    dfn = [f"-D{d}" for d in subject["defines"]]
    common = list(arm.get("optFlags", [])) + inc + dfn + list(arm.get("extraFlags", []))

    def one(tu: str) -> list[str]:
        obj = os.path.join(objdir, os.path.basename(tu) + ".o")
        return [cc, "-c", tu, "-o", obj] + common

    ok, err = _compile_pool(jobs, one, subject["tus"], None, None)
    if not ok:
        return False, err
    objs = [os.path.join(objdir, os.path.basename(tu) + ".o") for tu in subject["tus"]]
    r = run_capture([cc, "-o", binpath] + objs + list(arm.get("linkFlags", [])))
    if r.returncode != 0:
        return False, f"link: rc={r.returncode}\n" + (r.stderr or "")[:1200]
    return True, ""


def build_msvc(arm: dict, subject: dict, jobs: int, objdir: str,
               binpath: str) -> tuple[bool, str]:
    """cl.exe: `jobs` concurrent `/c` processes, then one link.

    ⚠ `/Fo` NEEDS A DISTINCT NAME PER TU AND THE BASENAMES COLLIDE. SQLite's
    full source has more than one `.c` with the same stem across src/ and ext/,
    and `cl /c a.c /Fo:dir\\` would write `dir\\a.obj` for each — the last writer
    winning, silently, with a link that then reports a missing symbol nowhere
    near the cause. The object name carries an index for that reason.
    """
    cl = arm["bin"]
    env = arm.get("_env") or dict(os.environ)
    inc = [f"/I{d}" for d in subject["includes"]]
    dfn = [f"/D{d}" for d in subject["defines"]]
    common = list(arm.get("optFlags", [])) + inc + dfn + list(arm.get("extraFlags", []))
    index = {tu: i for i, tu in enumerate(subject["tus"])}

    def objname(tu: str) -> str:
        stem = os.path.splitext(os.path.basename(tu))[0]
        return os.path.join(objdir, f"{index[tu]:03d}-{stem}.obj")

    def one(tu: str) -> list[str]:
        return [cl, "/nologo", "/c", tu, f"/Fo{objname(tu)}"] + common

    ok, err = _compile_pool(jobs, one, subject["tus"], env, None)
    if not ok:
        return False, err
    objs = [objname(tu) for tu in subject["tus"]]
    r = run_capture([cl, "/nologo", f"/Fe{binpath}"] + objs
                    + ["/link"] + list(arm.get("linkFlags", [])), env=env)
    if r.returncode != 0:
        return False, f"link: rc={r.returncode}\n" + (r.stdout or r.stderr or "")[:1200]
    return True, ""


def build_dss(arm: dict, subject: dict, jobs: int, objdir: str,
              binpath: str) -> tuple[bool, str]:
    """dsscp: ONE `--project` invocation, in-process CU pool.

    ⚠ dsscp RETURNS EXIT 0 EVEN ON FATAL ERRORS, so the verdict is
    taken from `error[` in the log plus the artifact's existence — never from
    the process exit status. Same rule `base-harness.sh`'s
    `dss_bh_build_artifact` states; restated rather than imported because this
    file must run standalone on a host with no bash.
    """
    # ★★ THE CONFIG ROOT IS PINNED BY THE PLAN, NOT INHERITED FROM THE CWD.
    # `findShippedConfig` walks upward from the working directory unless
    # `DSS_CONFIG_ROOT` says otherwise, so an unpinned run pairs the compiler
    # with whatever config tree happens to sit above wherever the benchmark was
    # launched. That is not a reproducibility nicety: ✔MEASURED 2026-08-21, a
    # two-day-stale `build/rel` was paired with the CURRENT config and refused
    # with `unknown key 'templateLabelRule' in 'assembly'`. A benchmark is a
    # statement about one binary; which config it read has to be part of that
    # statement.
    argv = [arm["bin"], "--project", arm["manifest"],
            f"--config={arm.get('config', 'release')}",
            "--output", objdir, "--jobs", str(jobs)]
    env = None
    if arm.get("configRoot"):
        env = dict(os.environ)
        env["DSS_CONFIG_ROOT"] = arm["configRoot"]
    r = run_capture(argv, env=env)
    log = (r.stdout or "") + (r.stderr or "")
    if "error[" in log:
        first = next((ln for ln in log.splitlines() if "error[" in ln), "")
        return False, f"diagnostics emitted: {first[:400]}"
    # ★★ THE ARTIFACT PATH IS **READ FROM THE BUILD'S OWN REPORT**, NEVER GUESSED.
    # ✔MEASURED 2026-08-21: `--output D` does not put the executable at `D/name`
    # — it puts it at `D/<object-format>/name` (here
    # `obj-dss-j4-0/elf64-x86_64-linux-exec/speedtest1`). Guessing cost one full
    # 103-TU build that succeeded and was reported as having produced nothing.
    # base-harness.sh names this exact trap
    # (`D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING`) and answers it
    # the same way: grep the log line the compiler writes.
    # ⚠ TWO DIFFERENT PATHS FOR ONE SPEC IS AN ERROR, not an arbitrary pick — the
    # same rule `dss_bh_reported_artifact` enforces. Handing a caller its
    # sibling's binary silently is the worst outcome available here, because
    # every number downstream would then describe a file nobody meant to measure.
    spec = arm.get("target", "")
    marker = f"dsscp: artifact {spec} "
    reported, seen = [], set()
    for line in log.splitlines():
        i = line.find(marker)
        if i >= 0:
            p = line[i + len(marker):].strip()
            if p not in seen:
                seen.add(p)
                reported.append(p)
    if len(reported) > 1:
        return False, ("the build log reports %d DIFFERENT artifacts for target spec "
                       "'%s': %s — refusing to guess which one was meant."
                       % (len(reported), spec, ", ".join(reported)))
    if reported and os.path.isfile(reported[0]):
        shutil.copy2(reported[0], binpath)
        return True, ""
    if reported:
        return False, (f"the build reported an artifact that is not on disk: "
                       f"{reported[0]}")
    return False, ("the build emitted no diagnostics and reported no artifact for "
                   f"target spec '{spec}' — rc={r.returncode}. A compiler that "
                   f"says nothing and claims nothing is a compiler defect, not a "
                   f"benchmark option.\n" + log[-800:])


# ── THE TWO WAYS A PRE-FLIGHT FAILS ARE DIFFERENT QUESTIONS ──────────────────
# ★★ "THE COMPILER REFUSED" AND "I COULD NOT RUN THE CHECK" MUST NEVER SHARE AN
# ANSWER. [[D-HARNESS-SQLITE-REUSES-A-RELEASE-BINARY-OLDER-THAN-THE-CONFIG-IT-IS-GIVEN]]
# The first is a VERDICT ABOUT THE BINARY — it ran, it read the config tree, and
# it rejected vocabulary that tree now contains, which is the stale-binary
# signature this check exists to catch. The second is a verdict about THIS
# MACHINE: no config tree at the pin, or a path that cannot be launched at all.
# Collapsing the two would make every environment problem read as "your compiler
# is stale" and send the operator to rebuild a compiler that was never the
# subject — a check that answers the adjacent question, which is worse than no
# check because it is confidently wrong.
# ⓘ The kinds are a CLOSED SET returned as a third tuple element rather than
# sniffed out of the prose: `why` is free text and every attempt to classify free
# text drifts the moment someone improves a sentence.
PREFLIGHT_OK = "ok"
PREFLIGHT_REFUSED = "refused"          # the compiler RAN and produced diagnostics
PREFLIGHT_UNRUNNABLE = "unrunnable"    # the check itself could not be performed


def preflight_dss(binary: str, config_root: str,
                  target: str = "") -> tuple[bool, str, str]:
    """Can this compiler compile three lines against that config, FOR THE TARGET
    THE MEASUREMENT WILL USE? (ok, why-not, kind) — `kind` from the closed set
    above, so a caller can tell a STALE BINARY from a check that never ran.

    ★ ONE IMPLEMENTATION, CALLED BY BOTH DRIVERS, ALWAYS ON THE HOST THAT WILL
    RUN THE COMPILER. The check has to happen before a configure, a 102-object
    reference build and a recipe derivation are spent — but it can only be run
    where the binary is native, and for the PowerShell twin the deriving shell
    (WSL) is NOT that host. Putting it here rather than in either driver is what
    keeps it one check instead of two that drift.
    ✔MEASURED 2026-08-21: a two-day-stale `build/rel` against the CURRENT config
    refuses with `unknown key 'templateLabelRule' in 'assembly'`. Correct and
    well-named — but it arrived three minutes in.

    ★★ AND `target` IS WHY THIS CHECK EXISTS AT ALL, NOT A REFINEMENT OF IT.
    ✔MEASURED 2026-08-26 on TWO hosts in one run
    (D-BENCH-COMPILER-AND-CONFIG-MAY-COME-FROM-DIFFERENT-COMMITS): this preflight
    printed `preflight: OK` and the measurement then died on a config-schema
    error in the very document it was supposed to be vouching for — on macOS
    `C_MalformedJson` at `/opcodes/10/encoding/variants/0/resultSlot` for
    `arm64:macho64-arm64-darwin-exec`, on Windows `unknown key 'registerClass'`
    for `x86_64:pe64-x86_64-windows-exec`. The probe compiled with NO `--target`,
    so it validated whatever DSS defaults to rather than the document under
    measurement. **A CONTROL MUST MATCH THE TARGET** — the same rule this
    repository already learned about reference compilers, arriving a second time
    wearing config's clothes.
    ⚠ A check that passes on a DIFFERENT input than the one that then fails is
    worse than no check: it converts "this might be stale" into a printed OK.
    ⓘ `target` is optional ONLY so a caller that genuinely has no target yet can
    still get the weaker check; every caller in this repository passes one, and a
    caller that omits it is TOLD so rather than silently downgraded.
    """
    # ⚠ CHECK THE COMPOSED PATH, NOT THE PIN ITSELF.
    # [[D-BENCH-CONFIG-ROOT-PIN-IS-ONE-LEVEL-TOO-DEEP-AND-SILENTLY-DOES-NOTHING]]
    # `DSS_CONFIG_ROOT` names the directory that CONTAINS `src/dss-config`, and
    # the resolver composes the rest. A pin whose own directory exists but whose
    # config tree does not passed this check for its whole life and then fell
    # through to the CWD ancestor walk -- silently, because a set-but-miss
    # falling through is documented, deliberate behaviour in the resolver.
    if not os.path.isdir(os.path.join(config_root, "src", "dss-config")):
        return False, (f"no config tree at {config_root}/src/dss-config -- "
                       f"DSS_CONFIG_ROOT names the checkout root that CONTAINS "
                       f"src/dss-config, not that directory itself"), \
            PREFLIGHT_UNRUNNABLE
    env = dict(os.environ)
    env["DSS_CONFIG_ROOT"] = config_root
    with tempfile.TemporaryDirectory(prefix="dss-preflight-") as td:
        src = os.path.join(td, "probe.c")
        with open(src, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("int main(void){return 0;}\n")
        # ⚠ `--language` IS REQUIRED HERE, AND ITS ABSENCE IS THE SAME BUG THIS
        # DOCSTRING ALREADY RECORDS ABOUT `--target`, ONE DIMENSION OVER.
        # ✔MEASURED 2026-08-27: `dsscp --compile probe.c --target
        # x86_64:pe64-x86_64-windows-exec` (no --language) is REFUSED with
        #   error[D_UnknownFileExtension]: no source language for 'probe.c':
        #   no --language was given, so target 'x86_64' selected its declared
        #   defaultAssemblyLanguage 'asm-x86_64-att' — which claims .s
        # That is documented, intended CLI behaviour, not a regression (the
        # message is byte-identical at HEAD): omitting `--language` selects the
        # TARGET'S ASSEMBLY DIALECT, which is how one invocation compiles a `.s`
        # for two CPUs. So the preflight refused before measuring anything.
        #
        # ★ THE ROOT CAUSE IS THAT THIS CONTROL DOES NOT MATCH ITS MEASUREMENT.
        # `build_dss` compiles via `--project <manifest>`, where the manifest
        # declares the language; this probe compiles via `--compile` on a bare
        # `.c`, where nothing does. A CONTROL MUST MATCH THE THING IT VOUCHES
        # FOR — the docstring above learned that about `--target` on 2026-08-26
        # and the same sentence applies to the invocation FORM.
        # ⓘ Hardcoding `c` is not a language-dispatch decision: this function
        # writes the probe itself, three lines above, and it is C by construction.
        argv = [binary, "--compile", src, "--language", "c", "--output", td]
        if target:
            argv += ["--target", target]
        # ⚠ THE LAUNCH ITSELF CAN FAIL, AND THAT IS NOT A VERDICT ABOUT THE
        # COMPILER. `subprocess` raises OSError when the path is not something
        # this OS can execute — ✔MEASURED 2026-08-31 on Windows, an extensionless
        # `#!/bin/sh` file gives `[WinError 193] not a valid Win32 application`
        # while a `.cmd` launches fine. Uncaught, that is a TRACEBACK where a
        # diagnostic belongs, and the caller sees a non-zero exit it would
        # naturally read as "the compiler refused". Named as UNRUNNABLE instead.
        try:
            r = run_capture(argv, env=env)
        except OSError as exc:
            return False, (f"{binary} could not be launched at all ({exc}) -- so "
                           f"NOTHING was learned about the compiler here, and "
                           f"this is a fact about the path given, not about "
                           f"whether the binary is current"), PREFLIGHT_UNRUNNABLE
        # ⚠ rc is NOT the verdict — dsscp returns 0 on fatal errors, so
        # the log is what decides, exactly as build_dss does.
        log = (r.stdout or "") + (r.stderr or "")
        if "error[" in log:
            return (False,
                    next(ln for ln in log.splitlines() if "error[" in ln)[:400],
                    PREFLIGHT_REFUSED)
        # It ran, it said nothing, and it still failed. That IS a verdict about
        # the binary — a compiler that cannot compile three lines and cannot say
        # why is unusable for the run about to be spent on it.
        if r.returncode != 0 and not log.strip():
            return (False,
                    f"the compiler exited {r.returncode} and said nothing at all",
                    PREFLIGHT_REFUSED)
    return True, "", PREFLIGHT_OK


BUILDERS = {"dss": build_dss, "unix-cc": build_unix_cc, "msvc": build_msvc}
# How each arm reaches N-way parallelism — printed beside every number, because
# it is the one thing the comparison does NOT equalize.
MECHANISM = {
    "dss": "in-process CU thread pool (--jobs N)",
    "unix-cc": "N concurrent `cc -c` processes + link",
    "msvc": "N concurrent `cl /c` processes + link",
}


# ── the run half ─────────────────────────────────────────────────────────────
def run_workload(binpath: str, workload: dict, repeats: int) -> tuple[dict, str, str]:
    """(timing stats, normalized output, failure). speedtest1 gets a FILE db.

    ★ A FILE db, never `:memory:` — an in-memory database changes what the pager
    does, so it measures a different program. Same rule as
    `sqlite-runtime-bench.py`, and this project has a standing note about it.
    """
    # ★ `--verify` IS ADDED HERE, IN THE CORE, RATHER THAN LEFT TO THE DRIVERS.
    # It is what makes the cross-arm comparison a CORRECTNESS check and not just
    # a shape check, and a benchmark whose correctness instrument is optional at
    # the call site is one that will eventually be run without it. `verify:
    # false` in the plan is the deliberate opt-out, and it is recorded in the
    # report so a reader can see the check was declined.
    argv_tail = (["--size", str(workload.get("size", 25))]
                 + (["--testset", workload["testset"]] if workload.get("testset") else [])
                 + (["--verify"] if workload.get("verify", True) else [])
                 + list(workload.get("extraArgs", [])))
    times: list[float] = []
    normalized = ""
    with tempfile.TemporaryDirectory(prefix="dss-st1-") as td:
        # Warm-up: page cache and first touch, never counted.
        warm = run_capture([binpath] + argv_tail + [os.path.join(td, "warm.db")])
        if warm.returncode != 0:
            return {}, "", (f"warm-up rc={warm.returncode}: "
                            + (warm.stderr or warm.stdout or "")[:800])
        for i in range(repeats):
            db = os.path.join(td, f"run{i}.db")
            elapsed, r = timed(lambda: run_capture([binpath] + argv_tail + [db]))
            if r.returncode != 0:
                return {}, "", (f"repeat {i} rc={r.returncode}: "
                                + (r.stderr or r.stdout or "")[:800])
            times.append(elapsed)
            if not normalized:
                raw = r.stdout or ""
                normalized = normalize_output(raw)
                # ⚠ SAY IT, DO NOT SWALLOW IT. The normalizer above now absorbs a
                # NEGATIVE elapsed time so a backward clock cannot masquerade as
                # "the arms did different work" — but absorbing it silently would
                # trade a false refusal for a hidden fact about the host.
                # ⓘ The numbers this harness reports are unaffected: they come
                # from `time.monotonic()` in THIS process, never from
                # speedtest1's own timer.
                backward = malformed_elapsed_times(raw)
                if backward:
                    print(f"  !  {os.path.basename(binpath)}: {backward} elapsed "
                          f"time(s) came back NEGATIVE — this host's clock "
                          f"stepped backwards during the run. The comparison "
                          f"below is unaffected (it ignores times); the "
                          f"program's own per-test seconds on this host are not "
                          f"trustworthy.")
    return stats(times), normalized, ""


# ── plan validation (the refusals) ───────────────────────────────────────────
def validate_plan(plan: dict) -> None:
    subject = plan.get("subject") or {}
    tus = subject.get("tus") or []

    if len(tus) < MIN_TUS:                                                  # R2
        die(f"R2 the TU list holds {len(tus)} translation units, below the floor "
            f"of {MIN_TUS}. A collapsed subject reports a fast build over source "
            f"it never compiled — the derivation is what to fix, never the floor.")

    missing = [t for t in tus if not os.path.isfile(t)][:8]
    if missing:                                                             # R1
        die("R1 the plan names translation units that are not on disk:\n      "
            + "\n      ".join(missing))

    mains = [t for t in tus if os.path.basename(t) == MAIN_TU_BASENAME]
    if len(mains) != 1:                                                     # R3
        die(f"R3 the subject must carry exactly one {MAIN_TU_BASENAME}; it carries "
            f"{len(mains)}. The full-source CLI recipe's `shell.c` is SUBSTITUTED "
            f"for it — neither left in place beside it nor dropped along with it.")
    if any(os.path.basename(t) == "shell.c" for t in tus):                  # R3
        die("R3 the subject still carries `shell.c`. That is the sqlite3 CLI's "
            "main; leaving it in links two `main`s or benchmarks the wrong "
            "program.")
    if any(os.path.basename(t) == "sqlite3.c" for t in tus):                # R3
        die("R3 the subject carries `sqlite3.c` — that is the AMALGAMATION. This "
            "benchmark's subject is the full source; upstream's own speedtest1 "
            "target is the amalgamation build and is deliberately not what this "
            "measures.")

    if platform.system() == "Windows":                                      # R4
        for p in [subject.get("sqliteSrc", "")] + tus[:1]:
            if p.startswith("\\\\"):
                die("R4 the source tree is reached over a UNC share "
                    f"({p}). Compiling across \\\\wsl$ (9P) costs several times "
                    "the local-disk I/O and does not cost every compiler the "
                    "same, so a build-time comparison taken there measures the "
                    "filesystem. Put the checkout on the local disk.")

    for name, val in (("build", plan.get("repeats", {}).get("build", 0)),
                      ("run", plan.get("repeats", {}).get("run", 0))):
        if int(val) < 1:                                                    # R7
            die(f"R7 repeats.{name} is {val}; a measurement needs at least one "
                f"sample.")
    for j in plan.get("jobsArms", []):
        if int(j) < 1:                                                      # R7
            die(f"R7 jobs arm {j} is not a worker count.")


# ── the measurement ──────────────────────────────────────────────────────────
def measure(plan: dict) -> dict:
    validate_plan(plan)
    subject = plan["subject"]
    outdir = plan["outDir"]
    os.makedirs(outdir, exist_ok=True)
    jobs_arms = [int(j) for j in plan.get("jobsArms", [1, 4])]
    build_repeats = int(plan["repeats"]["build"])
    run_repeats = int(plan["repeats"]["run"])

    results: list[dict] = []
    for arm in plan["compilers"]:
        kind = arm["kind"]
        if kind not in BUILDERS:
            die(f"R7 unknown compiler kind '{kind}' for arm '{arm['id']}'", 2)
        record = {"id": arm["id"], "kind": kind, "label": arm.get("label", arm["id"]),
                  "version": arm.get("version", ""),
                  "optimization": arm.get("optimizationLabel", ""),
                  "mechanism": MECHANISM[kind], "builds": {}, "run": None,
                  "skipped": arm.get("_skipReason", "")}
        if record["skipped"]:
            print(f"  {arm['id']:<6} SKIPPED — {record['skipped']}")
            results.append(record)
            continue

        binpath = ""
        for jobs in jobs_arms:
            samples: list[float] = []
            failure = ""
            for rep in range(build_repeats):
                # COLD every time: a fresh object dir, so no repeat is measuring
                # the previous repeat's leftovers.
                objdir = os.path.join(outdir, f"obj-{arm['id']}-j{jobs}-{rep}")
                shutil.rmtree(objdir, ignore_errors=True)
                os.makedirs(objdir, exist_ok=True)
                cand = os.path.join(objdir, f"speedtest1-{arm['id']}"
                                    + (".exe" if platform.system() == "Windows" else ""))
                # ⚠ AN ARM THAT RAISES IS AN ARM THAT FAILED — NOT A RUN THAT
                # DIED. R5 says a failing arm is named and the others continue,
                # and an uncaught exception is the one path that silently
                # violated it: ✔MEASURED 2026-08-21, a `FileNotFoundError` from
                # one unresolvable compiler took down a run in which the other
                # two arms had already built successfully, discarding both.
                # The harness must survive everything it measures.
                def _attempt() -> tuple[bool, str]:
                    try:
                        return BUILDERS[kind](arm, subject, jobs, objdir, cand)
                    except Exception as exc:          # noqa: BLE001 — see above
                        return False, f"{type(exc).__name__}: {exc}"

                elapsed, (ok, err) = timed(_attempt)
                if not ok:
                    failure = err
                    break
                samples.append(elapsed)
                binpath = cand
            if failure:                                                     # R5
                record["builds"][str(jobs)] = {"failed": failure}
                print(f"  {arm['id']:<6} -j{jobs}  BUILD FAILED — {failure.splitlines()[0]}")
                break
            record["builds"][str(jobs)] = stats(samples)
            print(f"  {arm['id']:<6} -j{jobs}  build median "
                  f"{statistics.median(samples):7.2f}s  "
                  f"(min {min(samples):.2f}s, {build_repeats} cold repeats)")

        if binpath and os.path.isfile(binpath):
            keep = os.path.join(outdir, os.path.basename(binpath))
            if os.path.abspath(keep) != os.path.abspath(binpath):
                shutil.copy2(binpath, keep)
            try:
                rstats, normalized, rerr = run_workload(keep, plan["workload"],
                                                        run_repeats)
            except Exception as exc:                  # noqa: BLE001 — R5, as above
                rstats, normalized, rerr = {}, "", f"{type(exc).__name__}: {exc}"
            if rerr:                                                        # R5
                record["run"] = {"failed": rerr}
                print(f"  {arm['id']:<6} RUN FAILED — {rerr.splitlines()[0]}")
            else:
                record["run"] = rstats
                record["_normalized"] = normalized
                record["binary"] = keep
                print(f"  {arm['id']:<6} run   median {rstats['median']:7.3f}s  "
                      f"(min {rstats['min']:.3f}s, {run_repeats} repeats)")
        results.append(record)

    ran = [r for r in results if r.get("_normalized")]
    if not ran:                                                             # R3exit
        die("no arm produced a runnable speedtest1 — nothing was measured.", 3)

    # R6 — the arms must have done the same work.
    reference = ran[0]
    for other in ran[1:]:
        if other["_normalized"] != reference["_normalized"]:
            a, b = reference["_normalized"].split("\n"), other["_normalized"].split("\n")
            diff = next((f"{x!r} != {y!r}" for x, y in zip(a, b) if x != y),
                        f"line counts differ: {len(a)} vs {len(b)}")
            die(f"R6 arm '{other['id']}' produced different speedtest1 output from "
                f"'{reference['id']}' once elapsed times are removed — the two "
                f"binaries did not do the same work, so their times are not "
                f"comparable.\n      first divergence: {diff}")

    for r in results:
        r.pop("_normalized", None)
    return {
        "host": {"system": platform.system(), "release": platform.release(),
                 "machine": platform.machine(),
                 "cpus": os.cpu_count()},
        "subject": {"tuCount": len(subject["tus"]),
                    "sqliteSrc": subject.get("sqliteSrc", ""),
                    "upstreamCommit": subject.get("upstreamCommit", ""),
                    "defineCount": len(subject.get("defines", [])),
                    "mainTu": next(t for t in subject["tus"]
                                   if os.path.basename(t) == MAIN_TU_BASENAME)},
        "workload": plan["workload"],
        "repeats": plan["repeats"],
        "jobsArms": jobs_arms,
        "arms": results,
    }


# ── reporting ────────────────────────────────────────────────────────────────
def render_markdown(report: dict) -> str:
    host, subj, wl = report["host"], report["subject"], report["workload"]
    jobs_arms = report["jobsArms"]
    lines = [
        "### SQLite `speedtest1` — DSS Code Prime vs the reference compilers",
        "",
        f"Subject: SQLite's own `test/speedtest1.c` linked against **{subj['tuCount']} "
        f"full-source translation units** (no amalgamation), upstream "
        f"`{subj['upstreamCommit'] or 'unknown'}`.",
        f"Host: {host['system']} {host['release']} / {host['machine']}, "
        f"{host['cpus']} logical CPUs. Workload: `--size {wl.get('size')}`"
        + (f" `--testset {wl['testset']}`" if wl.get("testset") else "") + ".",
        f"Build times are **cold** (fresh object directory per repeat), median of "
        f"{report['repeats']['build']}; run times are median of "
        f"{report['repeats']['run']} after an uncounted warm-up. Monotonic clock.",
        "",
    ]
    head = ["Compiler", "Optimization"] + [f"Build −j{j}" for j in jobs_arms] + \
           ["Run", "Parallel mechanism"]
    lines.append("| " + " | ".join(head) + " |")
    lines.append("| " + " | ".join("---" for _ in head) + " |")
    for arm in report["arms"]:
        if arm.get("skipped"):
            lines.append(f"| {arm['label']} | — | "
                         + " | ".join("skipped" for _ in jobs_arms)
                         + f" | skipped | {arm['skipped']} |")
            continue
        cells = [arm["label"] + (f" {arm['version']}" if arm["version"] else ""),
                 arm["optimization"] or "—"]
        for j in jobs_arms:
            b = arm["builds"].get(str(j))
            cells.append("**build failed**" if not b or "failed" in b
                         else f"{b['median']:.2f} s")
        r = arm.get("run")
        cells.append("**run failed**" if not r or "failed" in r
                     else f"{r['median']:.3f} s")
        cells.append(arm["mechanism"])
        lines.append("| " + " | ".join(cells) + " |")
    lines += [
        "",
        "⚠ The parallel mechanism is **not** equalized, and the column says so: "
        "DSS compiles every translation unit inside one process on a worker-thread "
        "pool, while gcc and MSVC are driven as N concurrent `-c` processes plus a "
        "link. That difference is the architecture under measurement; flattening it "
        "would mean either crippling DSS to one CU per process or inventing an "
        "in-process pool the reference compilers do not have.",
    ]
    failures = [(a["id"], k, v["failed"]) for a in report["arms"]
                for k, v in list(a.get("builds", {}).items())
                + ([("run", a["run"])] if isinstance(a.get("run"), dict) else [])
                if isinstance(v, dict) and "failed" in v]
    if failures:
        lines += ["", "Failures (named rather than dropped — silence about a unit "
                      "is a harness bug):", ""]
        for arm_id, where, why in failures:
            lines.append(f"- `{arm_id}` at `{where}`: {why.splitlines()[0]}")
    return "\n".join(lines) + "\n"


# ── self-test ────────────────────────────────────────────────────────────────
def selftest() -> int:
    """Prove the normalizer and the refusals actually refuse.

    A guard nobody has watched fail is a guard nobody has tested. Each arm here
    was verified by sabotage: delete the refusal it targets and this fails.
    """
    fails = []

    def check(name: str, cond: bool) -> None:
        print(f"  {'ok  ' if cond else 'FAIL'}  {name}")
        if not cond:
            fails.append(name)

    # The padding differs because speedtest1 right-aligns `%4d.%03ds`; that is
    # the case the first draft failed, so it leads.
    a = "  1 - 1000 INSERTs.....    0.123s\n\n       TOTAL.....    1.500s\n"
    b = "  1 - 1000 INSERTs..... 1230.123s\n       TOTAL..... 9999.001s\n"
    c = "  1 - 999 INSERTs.....    0.123s\n       TOTAL.....    1.500s\n"
    check("normalizer erases elapsed times AND their right-alignment padding",
          normalize_output(a) == normalize_output(b))
    check("normalizer keeps the workload identity", normalize_output(a) != normalize_output(c))
    check("normalizer drops blank lines", "\n\n" not in normalize_output(a))
    # The verification hash must SURVIVE normalization — it is the whole reason
    # the comparison is a correctness check. A normalizer that ate it would make
    # every arm agree, which is the vacuous-pass shape.
    h1 = "Verification Hash: 1234 abcdef\n"
    h2 = "Verification Hash: 1234 fedcba\n"
    check("normalizer preserves the verification hash",
          normalize_output(h1) != normalize_output(h2))

    # ★★ A BACKWARD CLOCK IS NOT A WORK DIFFERENCE, and this arm is the pin for
    # the live refusal that produced it. `"%4d.%03ds"` signs BOTH fields, so a
    # clock that steps backwards mid-run prints `-29.-29s`; the original pattern
    # could not match that, the token survived into the normalized text, and R6
    # declared two identical workloads to have "done different work".
    # ✔MEASURED 2026-08-28 on the WSL x86_64 leg, which is a host whose
    # CLOCK_REALTIME is known to oscillate ±34.47 s.
    # [[D-TEST-SPEEDTEST1-NORMALIZER-LETS-A-BACKWARD-CLOCK-LOOK-LIKE-DIFFERENT-WORK]]
    fwd = "160 - 100 DELETEs using rowid.....    1.234s\n"
    back = "160 - 100 DELETEs using rowid.....  -29.-29s\n"
    check("normalizer absorbs a NEGATIVE elapsed time (a backward host clock)",
          normalize_output(fwd) == normalize_output(back))
    check("a negative elapsed time is COUNTED, not silently swallowed",
          malformed_elapsed_times(back) == 1 and malformed_elapsed_times(fwd) == 0)
    # ...and the absorbing must not have eaten the identity along with the time.
    other = "160 - 100 UPDATEs using rowid.....  -29.-29s\n"
    check("absorbing a negative time still keeps the workload identity",
          normalize_output(back) != normalize_output(other))

    import io
    import contextlib

    def refuses(plan: dict, marker: str) -> bool:
        buf = io.StringIO()
        try:
            with contextlib.redirect_stderr(buf):
                validate_plan(plan)
        except SystemExit:
            return marker in buf.getvalue()
        return False

    here = os.path.abspath(__file__)
    base = {"subject": {"tus": [here] * MIN_TUS, "sqliteSrc": os.path.dirname(here)},
            "repeats": {"build": 1, "run": 1}, "jobsArms": [1]}
    check("R2 refuses a collapsed TU list",
          refuses({**base, "subject": {**base["subject"], "tus": [here]}}, "R2"))
    check("R1 refuses a TU that is not on disk",
          refuses({**base, "subject": {**base["subject"],
                                       "tus": [here] * (MIN_TUS - 1) + ["/nope/x.c"]}}, "R1"))
    check("R3 refuses a subject with no speedtest1.c",
          refuses(base, "R3"))

    with tempfile.TemporaryDirectory() as td:
        st1 = os.path.join(td, MAIN_TU_BASENAME)
        shell = os.path.join(td, "shell.c")
        amal = os.path.join(td, "sqlite3.c")
        for p in (st1, shell, amal):
            open(p, "w").close()
        good = [here] * (MIN_TUS - 1) + [st1]
        check("R3 refuses two speedtest1.c",
              refuses({**base, "subject": {**base["subject"],
                                           "tus": good + [st1]}}, "R3"))
        check("R3 refuses a surviving shell.c",
              refuses({**base, "subject": {**base["subject"],
                                           "tus": good + [shell]}}, "R3"))
        check("R3 refuses the amalgamation",
              refuses({**base, "subject": {**base["subject"],
                                           "tus": good + [amal]}}, "R3"))
        ok_plan = {**base, "subject": {**base["subject"], "tus": good}}
        check("R7 refuses zero build repeats",
              refuses({**ok_plan, "repeats": {"build": 0, "run": 1}}, "R7"))
        check("R7 refuses a zero jobs arm",
              refuses({**ok_plan, "jobsArms": [0]}, "R7"))
        # The complement that keeps the refusals honest: a well-formed plan must
        # pass. Without this arm, a validate_plan() that refused EVERYTHING would
        # score a perfect self-test.
        passed = True
        try:
            validate_plan(ok_plan)
        except SystemExit:
            passed = False
        check("a well-formed plan is NOT refused", passed)

    env, why = msvc_env()
    print(f"  info  MSVC environment: {'resolved' if env else 'absent — ' + why}")
    if fails:
        print(f"\nspeedtest1_bench selftest: {len(fails)} FAILED", file=sys.stderr)
        return 1
    print("\nspeedtest1_bench selftest: all arms green")
    return 0


def main() -> int:
    # The report is UTF-8 and the Windows console is not. Without this, printing
    # the finished table raises UnicodeEncodeError on a cp1252/cp437 console
    # AFTER every measurement has been taken — losing the whole run at the last
    # line. The FILES are always written UTF-8; only the console view degrades.
    for stream in (sys.stdout, sys.stderr):
        with __import__("contextlib").suppress(Exception):
            stream.reconfigure(encoding="utf-8", errors="replace")

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--plan", help="path of the benchmark plan JSON a driver wrote")
    ap.add_argument("--json-out", help="where to write the raw measurement JSON")
    ap.add_argument("--md-out", help="where to write the README-ready markdown")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the normalizer and every refusal actually refuse")
    ap.add_argument("--resolve-msvc", action="store_true",
                    help="print the resolved cl.exe environment as JSON, or the "
                         "reason there is none, and exit")
    ap.add_argument("--preflight-dss", metavar="BIN",
                    help="prove this dsscp can compile three lines "
                         "against --config-root, and exit; run it BEFORE paying "
                         "for a configure and a reference build. Exit 0 = it "
                         "compiles; 1 = THE COMPILER REFUSED (the stale-binary "
                         "signature); 3 = THE CHECK COULD NOT RUN (nothing was "
                         "learned about the compiler)")
    ap.add_argument("--config-root", metavar="DIR",
                    help="the DSS_CONFIG_ROOT to pin for --preflight-dss")
    ap.add_argument("--preflight-target", metavar="SPEC", default="",
                    help="the --target the MEASUREMENT will use, so the preflight "
                         "validates the same target document rather than whatever "
                         "dsscp defaults to (see preflight_dss)")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.preflight_dss:
        if not args.config_root:
            ap.error("--preflight-dss needs --config-root: leaving it unset lets "
                     "the working directory decide which config the measured "
                     "binary reads, which is the thing being pinned")
        ok, why, kind = preflight_dss(args.preflight_dss, args.config_root,
                                      args.preflight_target)
        if ok:
            # ⚠ SAY WHICH TARGET WAS VOUCHED FOR, and say plainly when the answer
            # is "the default one". An unqualified `preflight: OK` is what let a
            # stale arm64/pe64 document reach a 3-minute measurement twice in one
            # run on 2026-08-26 -- the reader had no way to see the check and the
            # measurement were looking at different documents.
            scope = (f"for target {args.preflight_target}" if args.preflight_target
                     else "for the DEFAULT target -- NOT the one under measurement, "
                          "so a target-specific config break will NOT be caught here")
            print(f"preflight: OK - {args.preflight_dss} compiles against "
                  f"{args.config_root} {scope}")
            return 0
        # ★ THE HEADLINE NAMES THE KIND, AND SO DOES THE EXIT CODE. A caller that
        # cannot tell "your binary is stale" from "I could not run the check"
        # will report the first for the second — see the closed set above.
        if kind == PREFLIGHT_UNRUNNABLE:
            print(f"speedtest1_bench: preflight COULD NOT RUN — the compiler was "
                  f"NOT judged\n"
                  f"      binary : {args.preflight_dss}\n"
                  f"      config : {args.config_root}\n"
                  f"      target : {args.preflight_target or '(default)'}\n"
                  f"      says   : {why}\n"
                  f"      This says NOTHING about whether that binary is current. "
                  f"Fix what is named above and ask again; rebuilding the "
                  f"compiler would not change this answer.",
                  file=sys.stderr)
            return 3
        print(f"speedtest1_bench: preflight FAILED\n"
              f"      binary : {args.preflight_dss}\n"
              f"      config : {args.config_root}\n"
              f"      target : {args.preflight_target or '(default)'}\n"
              f"      says   : {why}\n"
              f"      The usual cause is a STALE binary against a CURRENT config "
              f"tree, or a CURRENT binary against a config tree some cleanup "
              f"rolled back -- both were seen on 2026-08-26, in mirror image, in "
              f"one run. Rebuild it before measuring, and check WHICH side moved.",
              file=sys.stderr)
        return 1
    if args.resolve_msvc:
        env, why = msvc_env()
        print(json.dumps({"env": env, "reason": why}))
        return 0 if env else 1
    if not args.plan:
        ap.error("--plan is required (or --selftest / --resolve-msvc)")

    with open(args.plan, encoding="utf-8") as fh:
        plan = json.load(fh)

    # An arm whose toolchain is not on this host is SKIPPED BY NAME, never
    # dropped: a benchmark that silently compares two compilers when the reader
    # was promised three is the same defect class as a test that passes
    # vacuously.
    for arm in plan["compilers"]:
        if arm["kind"] == "msvc":
            env, why = msvc_env(arm.get("arch", "x64"))
            if env is None:
                arm["_skipReason"] = why
                continue
            arm["_env"] = env
            # ⚠⚠ `cl.exe` IS RESOLVED TO AN ABSOLUTE PATH *OUT OF THE HARVESTED
            # ENVIRONMENT'S OWN PATH*, and this is not tidiness — it is a Windows
            # trap with a misleading symptom. `subprocess` passes `env` to the
            # child, but `CreateProcess` resolves the EXECUTABLE NAME against the
            # PARENT process's PATH. So handing it a bare "cl.exe" plus a perfect
            # vcvars environment fails with `FileNotFoundError: [WinError 2]`,
            # which reads as "MSVC is not installed" when in fact it was found,
            # harvested, and correct. ✔MEASURED 2026-08-21: exactly that, and it
            # took down the whole run rather than one arm.
            resolved = shutil.which(arm.get("bin") or "cl.exe", path=env.get("PATH", ""))
            if not resolved:
                arm["_skipReason"] = ("vcvarsall produced an environment whose PATH "
                                      "holds no cl.exe — the toolset is incomplete")
            else:
                arm["bin"] = resolved
        elif not shutil.which(arm["bin"]) and not os.path.isfile(arm["bin"]):
            # ⚠ NAME THE HOST IN THE REASON. The commonest way to land here is a
            # plan derived on one host and measured on another: a POSIX `cc` path
            # in a Windows plan is not "gcc is missing", it is "the driver that
            # wrote the plan resolved the wrong host's compiler".
            arm["_skipReason"] = (f"{arm['bin']} is not executable on this "
                                  f"{platform.system()} host — if that is a path "
                                  f"for a DIFFERENT host, the plan was written by "
                                  f"a driver that resolved the wrong one")

    report = measure(plan)

    out_json = args.json_out or os.path.join(plan["outDir"], "benchmark-speedtest1.json")
    with open(out_json, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(report, fh, indent=2)
        fh.write("\n")
    md = render_markdown(report)
    out_md = args.md_out or os.path.join(plan["outDir"], "benchmark-speedtest1.md")
    with open(out_md, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(md)

    print()
    print(md)
    print(f"raw JSON : {out_json}")
    print(f"markdown : {out_md}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
