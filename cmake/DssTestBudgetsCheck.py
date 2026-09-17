#!/usr/bin/env python3
"""Pin: every ctest entry of THIS build carries the TIMEOUT that
cmake/DssTestBudgets.cmake assigns, for the class cmake/DssBuildClass.cmake — the
ONE owner of "what kind of build is this" — selects from this build's own
configuration.

Born of an integrated runner that hung before creating its own directory.

Registered as `ctest/entry-budgets` by integrated_tests/CMakeLists.txt, and
deliberately NOT by the module it pins: a pin inside the mechanism disappears with
it, and deleting the module's `include()` line is the first way to lose it.

Each refusal carries its own tag:
  self-test            one of this pin's own checks did not refuse its synthetic defect
  listing-vacuous      ctest listed fewer than FLOOR entries, so nothing below is evidence
  timeout-missing      an entry will run with no TIMEOUT
  timeout-below-rule   a registration set its own TIMEOUT below the rule's budget for
                       this build's class
  report-missing       the module's report is absent: the module did not run
  report-disagrees     the report and ctest's listing name different entries, or an
                       entry's tier, budget or enforced TIMEOUT is not the rule's
  named-row-stale      a measured row names no registered entry
  class-probe          a synthetic configuration selects the wrong class or tier
  class-of-this-build  this build's CMakeCache.txt selects a class other than the one applied
  class-readers        a known consumer of the class did not read it, or read another answer
  class-consistency    the shuffle arm's published observation disagrees with the class
  class-owner          a CMake file this build reads tests for a sanitizer anywhere but
                       cmake/DssBuildClass.cmake
  ci-configure-line    CI's own configure lines, re-read from
                       .github/workflows/pipeline-pr.yml, cannot be read, or select a
                       class other than the one their leg declares
It self-tests every check on synthetic input BEFORE judging the build, so it cannot
pass without proving each can fail.

usage: DssTestBudgetsCheck.py --build-dir <dir> --source-dir <dir> --ctest <exe>
                              --cmake <exe> --module <DssTestBudgets.cmake>
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

FLOOR = 100
CORPUS_PREFIXES = ("examples/", "integrated_tests/")
REPORT_NAME = "ctest-entry-budgets.txt"
OWNER = "cmake/DssBuildClass.cmake"
REQUIRED_READERS = ("tests/CMakeLists.txt shuffle-arm", "cmake/DssTestBudgets.cmake")
WORKFLOW = ".github/workflows/pipeline-pr.yml"
RELEASE_TYPES = ("release", "relwithdebinfo", "minsizerel")
SHOWN = 12  # lines of detail per refusal; the count is always complete

# (build type, multi-config, compile flags, entry, expected class, expected tier)
PROBES = [
    ("Debug", "0", "-fsanitize=address,undefined -fno-omit-frame-pointer -g",
     "examples/c/any", "sanitized", "corpus"),
    ("Release", "0", "/fsanitize=address",
     "program/test_emit_hir_round_trips_every_example", "sanitized", "named"),
    ("RelWithDebInfo", "0", "-O2 -fsanitize=thread", "core/test_strong_ids", "sanitized", "unit"),
    # NEGATIVE: sanitizer-adjacent flags that enable no sanitizer
    ("Debug", "0", "-fno-sanitize-recover=all -fsanitize-recover=address",
     "integrated_tests/c/any", "debug", "corpus"),
    ("", "0", "", "core/test_strong_ids", "debug", "unit"),
    ("Profile", "0", "", "core/test_strong_ids", "debug", "unit"),
    ("Release", "1", "", "core/test_strong_ids", "debug", "unit"),
    ("Release", "0", "-O2", "integrated_tests/cli-surface", "release", "corpus"),
    ("release", "0", "", "run_gate_guard", "release", "named"),
    ("MinSizeRel", "0", "", "core/test_strong_ids", "release", "unit"),
    # NEGATIVE: a name that only LOOKS like a corpus entry
    ("RelWithDebInfo", "0", "", "examples_extra/not-corpus", "release", "unit"),
]

BRACKET_OPEN = re.compile(r"\[(=*)\[")
BRACKET_COMMENT = re.compile(r"#\[(=*)\[")


# ── readers of what the build produced ───────────────────────────────────────
def read_listing(ctest, build_dir):
    proc = subprocess.run([ctest, "--test-dir", build_dir, "-N", "--show-only=json-v1"],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError(f"ctest -N exited {proc.returncode}: {proc.stderr.strip()[:400]}")
    listing, duplicates = {}, []
    for test in json.loads(proc.stdout).get("tests", []):
        timeout = None
        for prop in test.get("properties", []):
            if prop.get("name") == "TIMEOUT":
                timeout = float(prop.get("value"))
        if test["name"] in listing:
            duplicates.append(test["name"])
        listing[test["name"]] = timeout
    return listing, duplicates


def read_report(text):
    header, named, entries, readers, walked = {}, {}, {}, [], []
    for raw in text.splitlines():
        cells = raw.split("\t")
        if cells[0] == "named" and len(cells) == 4:
            named[cells[1]] = (int(cells[2]), cells[3] == "1")
        elif cells[0] == "entry" and len(cells) == 6:
            entries[cells[1]] = {"tier": cells[2], "budget": int(cells[3]),
                                 "source": cells[4], "explicit": cells[5]}
        elif cells[0] == "class.reader" and len(cells) == 3:
            readers.append((cells[1], cells[2]))
        elif cells[0] == "walked" and len(cells) == 2:
            walked.append(cells[1])
        elif len(cells) == 2:
            header[cells[0]] = cells[1]
    return header, named, entries, readers, walked


def cache_configuration(build_dir):
    values = {}
    cache = pathlib.Path(build_dir) / "CMakeCache.txt"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        found = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):[A-Z_]+=(.*)$", line)
        if found:
            values[found.group(1)] = found.group(2)
    build_type = values.get("CMAKE_BUILD_TYPE", "")
    types = values.get("CMAKE_CONFIGURATION_TYPES", "").split(";") if values.get(
        "CMAKE_CONFIGURATION_TYPES") else [build_type]
    flags = []
    for lang in ("C", "CXX"):
        flags.append(values.get(f"CMAKE_{lang}_FLAGS", ""))
        for kind in types:
            if kind:
                flags.append(values.get(f"CMAKE_{lang}_FLAGS_{kind.upper()}", ""))
    return build_type, " ".join(f for f in flags if f)


# ── the rule, restated independently of the module ───────────────────────────
def derive(header, named, name):
    """(tier, budget, ceiling) for `name` under the report's class ceilings."""
    unit = int(header["ceiling.unit"])
    if name.startswith(CORPUS_PREFIXES):
        tier, ceiling = "corpus", int(header["ceiling.corpus"])
    elif name in named:
        tier, ceiling = "named", max(named[name][0], unit)
    else:
        tier, ceiling = "unit", unit
    return tier, int(header["headroom"]) * ceiling, ceiling


# ── the checks, each a pure function the self-test also drives ───────────────
def missing_timeouts(listing):
    return sorted(name for name, timeout in listing.items() if timeout is None)


def budget_disagreements(listing, header, named, entries):
    problems = []
    for name in sorted(set(listing) - set(entries)):
        problems.append(f"{name}: listed by ctest but never seen by the walk")
    for name in sorted(set(entries) - set(listing)):
        problems.append(f"{name}: in the report but not in ctest's listing")
    for name, entry in sorted(entries.items()):
        if name not in listing:
            continue
        tier, budget, _ = derive(header, named, name)
        if (entry["tier"], entry["budget"]) != (tier, budget):
            problems.append(f"{name}: the module computed tier {entry['tier']} / {entry['budget']} s,"
                            f" the rule gives tier {tier} / {budget} s")
        want = float(entry["explicit"]) if entry["source"] == "explicit" else float(budget)
        if listing[name] is not None and abs(listing[name] - want) > 1e-6:
            problems.append(f"{name}: ctest will enforce TIMEOUT {listing[name]:g} s,"
                            f" the report says {want:g} s")
    return problems


def below_rule(header, named, entries):
    problems = []
    for name, entry in sorted(entries.items()):
        if entry["source"] != "explicit":
            continue
        _, budget, ceiling = derive(header, named, name)
        own = float(entry["explicit"])
        if own < budget:
            problems.append(f"{name}: its registration sets TIMEOUT {own:g} s, below the rule's"
                            f" {budget} s for class {header.get('class')} ({own / ceiling:.2f}x its"
                            f" ceiling of {ceiling} s against a headroom of {header.get('headroom')}x)")
    return problems


def reader_problems(header, readers):
    problems = []
    names = [name for name, _ in readers]
    for required in REQUIRED_READERS:
        if required not in names:
            problems.append(f"{required} did not read the class through {OWNER}")
    for name, got in readers:
        if got != header.get("class"):
            problems.append(f"{name} read class {got}, the budgets applied {header.get('class')}")
    return problems


def consistency_problems(header):
    observed = header.get("consumer.shuffle_arm_sanitized", "UNSET")
    if observed not in ("ON", "OFF"):
        return [f"tests/CMakeLists.txt published no shuffle-arm observation (got {observed!r})"]
    expected = "ON" if header.get("class") == "sanitized" else "OFF"
    if observed != expected:
        return [f"the shuffle arm observed sanitized={observed} in a build of class"
                f" {header.get('class')}, which means sanitized={expected}"]
    return []


def strip_cmake_comments(text):
    """The text with every CMake comment removed; strings and bracket arguments kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif c == "#":
            opened = BRACKET_COMMENT.match(text, i)
            if opened:
                close = "]" + opened.group(1) + "]"
                j = text.find(close, opened.end())
                j = n if j < 0 else j + len(close)
                out.append("\n" * text.count("\n", i, j))
                i = j
            else:
                j = text.find("\n", i)
                i = n if j < 0 else j
        elif c == "[" and BRACKET_OPEN.match(text, i):
            opened = BRACKET_OPEN.match(text, i)
            close = "]" + opened.group(1) + "]"
            j = text.find(close, opened.end())
            j = n if j < 0 else j + len(close)
            out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def sanitizer_tests_in(text):
    return [number for number, line in enumerate(strip_cmake_comments(text).splitlines(), 1)
            if "fsanitize" in line]


def owner_problems(source_dir, walked):
    source = pathlib.Path(source_dir).resolve()
    files = set((source / "cmake").glob("*.cmake"))
    for directory in walked:
        path = pathlib.Path(directory).resolve()
        if path != source and source not in path.parents:
            continue
        files.add(path / "CMakeLists.txt")
        files.update(path.glob("*.cmake"))
    owner = (source / OWNER).resolve()
    problems = []
    for path in sorted(files):
        if path.resolve() == owner or not path.is_file():
            continue
        for number in sanitizer_tests_in(path.read_text(encoding="utf-8", errors="replace")):
            problems.append(f"{path.relative_to(source).as_posix()}:{number} tests for a sanitizer;"
                            f" only {OWNER} may")
    return problems, len(files)


def ci_leg_configurations(text):
    """[(leg, build type, compile flags, declared sanitizers)] exactly as CI configures."""
    legs = []
    for found in re.finditer(r'\{"name":"[^{}]*\}', text):
        leg = json.loads(found.group(0))
        if "build_type" in leg and "sanitizers" in leg:
            legs.append(leg)
    if not legs:
        raise ValueError('no leg objects carrying "build_type" and "sanitizers" were found')
    compose = re.findall(r'^\s*flags="([^"]*)"\s*$', text, re.M)
    if len(compose) != 1 or "${{ matrix.sanitizers }}" not in compose[0]:
        raise ValueError(f"expected ONE `flags=\"...${{{{ matrix.sanitizers }}}}...\"` composition line,"
                         f" found {compose!r}")
    for needle in ('echo "cflags=$flags"', "CXXFLAGS: ${{ steps.san.outputs.cflags }}",
                   '-DCMAKE_BUILD_TYPE="${{ matrix.build_type }}"', "-G Ninja"):
        if text.count(needle) != 1:
            raise ValueError(f"expected exactly one {needle!r}, found {text.count(needle)}")
    if re.search(r"-DCMAKE_(C|CXX)_FLAGS", text):
        raise ValueError("a configure line sets CMAKE_C_FLAGS/CMAKE_CXX_FLAGS directly; this reading"
                         " of how CI passes its flags no longer describes it")
    return [(leg["name"], leg["build_type"],
             compose[0].replace("${{ matrix.sanitizers }}", leg["sanitizers"]) if leg["sanitizers"] else "",
             leg["sanitizers"]) for leg in legs]


def declared_class(build_type, sanitizers):
    if sanitizers:
        return "sanitized"
    return "release" if build_type.lower() in RELEASE_TYPES else "debug"


def run_probe(cmake, module, build_type, multi, flags, entry):
    proc = subprocess.run(
        [cmake, f"-DDSS_BUDGET_PROBE_BUILD_TYPE={build_type}", f"-DDSS_BUDGET_PROBE_MULTI_CONFIG={multi}",
         f"-DDSS_BUDGET_PROBE_FLAGS={flags}", f"-DDSS_BUDGET_PROBE_TEST={entry}", "-P", module],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    found = re.search(r"class=(\w+) tier=(\w+) budget=(\d+)", proc.stdout + proc.stderr)
    if proc.returncode != 0 or not found:
        return None, (proc.stdout + proc.stderr).strip()[:300]
    return (found.group(1), found.group(2), int(found.group(3))), ""


# ── the self-test: every check refuses its synthetic defect ──────────────────
def self_test():
    failures = []
    if missing_timeouts({"a": 5.0, "b": None}) != ["b"]:
        failures.append("missing-TIMEOUT: a synthetic entry without one was not named")
    header = {"class": "debug", "headroom": "9", "ceiling.corpus": "40", "ceiling.unit": "35",
              "consumer.shuffle_arm_sanitized": "OFF"}
    named = {"heavy": (182, True)}
    right = {"examples/c/x": {"tier": "corpus", "budget": 360, "source": "assigned", "explicit": "-"},
             "heavy": {"tier": "named", "budget": 1638, "source": "assigned", "explicit": "-"}}
    good = {"examples/c/x": 360.0, "heavy": 1638.0}
    if budget_disagreements(good, header, named, right):
        failures.append("budgets: a synthetic build that follows the rule was refused")
    wrong_tier = dict(right, heavy={"tier": "unit", "budget": 315, "source": "assigned", "explicit": "-"})
    if not budget_disagreements({"examples/c/x": 360.0, "heavy": 315.0}, header, named, wrong_tier):
        failures.append("budgets: a synthetic entry given the wrong tier passed")
    if not budget_disagreements({"examples/c/x": 99.0, "heavy": 1638.0}, header, named, right):
        failures.append("budgets: a synthetic enforced TIMEOUT that is not the report's passed")
    if not any("never seen" in p for p in budget_disagreements(dict(good, late=1.0), header, named, right)):
        failures.append("budgets: a synthetic entry the walk never saw passed")
    explicit_low = dict(right, heavy={"tier": "named", "budget": 1638, "source": "explicit", "explicit": "900"})
    explicit_high = dict(right, heavy={"tier": "named", "budget": 1638, "source": "explicit", "explicit": "2000"})
    if not below_rule(header, named, explicit_low) or below_rule(header, named, explicit_high):
        failures.append("below-rule: an own TIMEOUT of 900 s against 1638 s was not refused, or 2000 s was")
    readers = [("tests/CMakeLists.txt shuffle-arm", "debug"), ("cmake/DssTestBudgets.cmake", "debug")]
    if reader_problems(header, readers) or not reader_problems(header, readers[1:]) \
            or not reader_problems(header, [readers[0], ("cmake/DssTestBudgets.cmake", "sanitized")]):
        failures.append("readers: a missing reader or a reader with another answer was not refused")
    if consistency_problems(header) or not consistency_problems(dict(header, **{
            "consumer.shuffle_arm_sanitized": "ON"})) or not consistency_problems(dict(header, **{
            "consumer.shuffle_arm_sanitized": "UNSET"})):
        failures.append("consistency: a shuffle observation that disagrees with the class was not refused")
    code = ('# a comment naming -fsanitize=address\n'
            '#[[ a bracket comment naming -fsanitize=address ]]\n'
            'set(x "# not a comment")\n'
            'if(CMAKE_CXX_FLAGS MATCHES "[-/]fsanitize=")\n'
            'message(STATUS [=[fsanitize inside a bracket argument]=])\n')
    if sanitizer_tests_in(code) != [4, 5]:
        failures.append(f"owner: the synthetic CMake text reported lines {sanitizer_tests_in(code)}, not [4, 5]")
    workflow = ('legs=\'[{"name":"a","build_type":"Release","sanitizers":""},'
                '{"name":"b","build_type":"Debug","sanitizers":"address"}]\'\n'
                '  flags="-fsanitize=${{ matrix.sanitizers }} -g"\n'
                '  echo "cflags=$flags"\n  CXXFLAGS: ${{ steps.san.outputs.cflags }}\n'
                '  cmake -G Ninja -DCMAKE_BUILD_TYPE="${{ matrix.build_type }}"\n')
    try:
        legs = ci_leg_configurations(workflow)
        if legs != [("a", "Release", "", ""), ("b", "Debug", "-fsanitize=address -g", "address")]:
            failures.append(f"ci: the synthetic workflow read as {legs}")
    except ValueError as error:
        failures.append(f"ci: the synthetic workflow was not readable: {error}")
    try:
        ci_leg_configurations(workflow.replace("flags=", "FLAGS="))
        failures.append("ci: a synthetic workflow with no flags composition was read anyway")
    except ValueError:
        pass
    return failures


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser()
    for flag in ("--build-dir", "--source-dir", "--ctest", "--cmake", "--module"):
        parser.add_argument(flag, required=True)
    args = parser.parse_args()
    refusals = []

    def refuse(tag, lines):
        refusals.append(tag)
        print(f"ctest-entry-budgets: FAIL {tag} ({len(lines)})")
        for line in lines[:SHOWN]:
            print(f"    {line}")
        if len(lines) > SHOWN:
            print(f"    ... and {len(lines) - SHOWN} more")

    broken = self_test()
    if broken:
        refuse("self-test", broken)
        return 1
    print("ctest-entry-budgets: self-test OK - every check refused its synthetic defect and passed"
          " its synthetic control")

    listing, duplicates = read_listing(args.ctest, args.build_dir)
    if len(listing) < FLOOR:
        refuse("listing-vacuous", [f"ctest listed {len(listing)} entries; at least {FLOOR} expected"])
        return 1
    if duplicates:
        refuse("report-disagrees", [f"{name}: registered more than once" for name in sorted(set(duplicates))])
    missing = missing_timeouts(listing)
    if missing:
        refuse("timeout-missing", missing)

    report = pathlib.Path(args.build_dir) / REPORT_NAME
    if not report.is_file():
        refuse("report-missing", [f"{report} does not exist: cmake/DssTestBudgets.cmake did not run"
                                  " for this build (is its include() still the LAST line of the root"
                                  " CMakeLists.txt?)"])
        return 1
    header, named, entries, readers, walked = read_report(report.read_text(encoding="utf-8"))
    for tag, problems in (("report-disagrees", budget_disagreements(listing, header, named, entries)),
                          ("timeout-below-rule", below_rule(header, named, entries)),
                          ("named-row-stale", [f"{name}: measured, but no registered entry has this name"
                                               for name, (_, matched) in sorted(named.items()) if not matched]),
                          ("class-readers", reader_problems(header, readers)),
                          ("class-consistency", consistency_problems(header))):
        if problems:
            refuse(tag, problems)

    wrong = []
    for build_type, multi, flags, entry, want_class, want_tier in PROBES:
        got, detail = run_probe(args.cmake, args.module, build_type, multi, flags, entry)
        if got is None:
            wrong.append(f"probe {entry!r} type={build_type!r} multi={multi} flags={flags!r}: no verdict ({detail})")
        elif got[:2] != (want_class, want_tier):
            wrong.append(f"type={build_type!r} multi={multi} flags={flags!r} entry={entry!r}:"
                         f" class {got[0]} tier {got[1]}, expected {want_class} {want_tier}")
    if wrong:
        refuse("class-probe", wrong)
    else:
        print(f"ctest-entry-budgets: class probes OK - {len(PROBES)} synthetic configurations,"
              " sanitized ones included, each selected its expected class and tier")

    build_type, cache_flags = cache_configuration(args.build_dir)
    flags = f"{cache_flags} {header.get('evidence.directory_compile_options', '')}".strip()
    got, detail = run_probe(args.cmake, args.module, build_type,
                            header.get("evidence.multi_config", "0"), flags, "unit/any")
    if got is None or got[0] != header.get("class"):
        refuse("class-of-this-build",
               [f"CMakeCache.txt says build type {build_type!r} with compile flags {cache_flags!r}, which"
                f" selects {got[0] if got else 'nothing (' + detail + ')'}, but the module applied"
                f" {header.get('class')!r}"])

    owners, scanned = owner_problems(args.source_dir, walked)
    if owners:
        refuse("class-owner", owners)
    else:
        print(f"ctest-entry-budgets: one owner OK - no sanitizer test in the code of {scanned - 1}"
              f" CMake file(s) this build reads, other than {OWNER}")

    workflow = pathlib.Path(args.source_dir) / WORKFLOW
    try:
        legs = ci_leg_configurations(workflow.read_text(encoding="utf-8"))
        ci_wrong, sanitized_legs = [], []
        for name, leg_type, leg_flags, sanitizers in legs:
            want = declared_class(leg_type, sanitizers)
            got, detail = run_probe(args.cmake, args.module, leg_type, "0", leg_flags, "unit/any")
            if got is None or got[0] != want:
                ci_wrong.append(f"{name}: -DCMAKE_BUILD_TYPE={leg_type} with CXXFLAGS={leg_flags!r} selects"
                                f" {got[0] if got else 'nothing (' + detail + ')'}, the leg declares {want}")
            if sanitizers:
                sanitized_legs.append(f"{name} ({leg_type}, CXXFLAGS '{leg_flags}')")
        if not sanitized_legs:
            ci_wrong.append(f"{WORKFLOW} declares no sanitized leg, so the sanitized class has no CI"
                            " configuration to be proved against")
        if ci_wrong:
            refuse("ci-configure-line", ci_wrong)
        else:
            print(f"ctest-entry-budgets: CI configure lines OK - {len(legs)} legs re-read from {WORKFLOW},"
                  f" each selects the class it declares; sanitized: {', '.join(sanitized_legs)}")
    except (OSError, ValueError) as error:
        refuse("ci-configure-line", [f"{WORKFLOW} could not be read as CI configures: {error}"])

    tiers = {}
    for name, entry in entries.items():
        if entry["source"] == "assigned":
            tiers.setdefault(entry["tier"], []).append(entry["budget"])
    summary = ", ".join(f"{tier} {len(budgets)} ({min(budgets)}-{max(budgets)} s)"
                        for tier, budgets in sorted(tiers.items()))
    own = sum(1 for entry in entries.values() if entry["source"] == "explicit")
    print(f"ctest-entry-budgets: this build is class {header.get('class')} (build type"
          f" {header.get('evidence.build_type')!r}), headroom {header.get('headroom')}; {len(listing)} entries"
          f" listed, budgets assigned: {summary}; {own} with their own TIMEOUT; readers:"
          f" {', '.join(f'{name}={got}' for name, got in readers)}")

    if refusals:
        print(f"ctest-entry-budgets: FAIL - {len(refusals)} refusal(s): {', '.join(refusals)}")
        return 1
    print("ctest-entry-budgets: OK - every entry carries the budget the rule gives it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
