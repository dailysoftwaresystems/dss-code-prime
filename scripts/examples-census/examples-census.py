#!/usr/bin/env python3
# PURPOSE: re-derive every corpus-manifest figure examples/README.md states, by parsing the manifests.
"""examples-census.py -- THE CORPUS MANIFEST CENSUS.

★★★ WHY THIS EXISTS AS A SCRIPT, and it is this repository's own recorded
experience rather than tidiness.

`examples/README.md` carries ~20 figures about the corpus -- how many manifests
declare `source` vs `sources`, how many targets carry an `emulator`, how many
optimizer arms assert `mustDifferFromBaseline`. Every one has been re-derived at
least three times, each time by an AD-HOC parse written for that cycle and then
thrown away. The README itself records what that costs:

  * the 2026-08-14 set was stale WITHIN A DAY -- two new examples moved eleven
    distinct figures across eighteen slots at once;
  * one re-derivation looked for `optimizedPipelines` under `targets[]` when it
    is a TOP-LEVEL key and confidently reported **0** for every manifest --
    "a census that returns a plausible zero is worse than one that crashes";
  * a repair published 26/21/25/17 and those four numbers were ALREADY STALE
    when it shipped them, because they were measured mid-cycle.

⇒ The instrument is the deliverable, not the numbers. Anyone can now re-derive
the whole block in one command instead of re-writing the parser and re-earning
its bugs.

★ IT PARSES, IT NEVER GREPS. `$comment` prose in this corpus mentions key names
(`dependsOn` appears in prose inside manifests that do not declare it), so a text
count OVER-REPORTS. The README says this in its own words and it is enforced here
by construction: every figure comes from `json.load`.

★ AND IT FAILS CLOSED ON A COLLAPSED SCAN. A census that finds no manifests would
otherwise print a tidy set of zeroes, which is the failure mode above wearing a
different hat.

USAGE
    python scripts/examples-census/examples-census.py            # print the census
    python scripts/examples-census/examples-census.py --json     # machine-readable

★ NO `.ps1` TWIN, DELIBERATELY: a `.py` runs unchanged on every host this project
gates on, so a PowerShell sibling would be a second implementation of something
that was never split.
"""
import io
import json
import os
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CORPUS = os.path.join(REPO, "examples")

# A corpus this size cannot legitimately shrink to a handful; a scan that finds
# fewer than this has broken, not measured.
FLOOR = 400


def manifests():
    out = []
    for dirpath, dirnames, filenames in os.walk(CORPUS):
        if "expected.json" not in filenames:
            continue
        full = os.path.join(dirpath, "expected.json")
        rel = os.path.relpath(full, REPO).replace("\\", "/")
        try:
            with io.open(full, encoding="utf-8") as f:
                out.append((rel, json.load(f)))
        except (ValueError, OSError) as e:
            sys.exit("MALFORMED MANIFEST %s: %s" % (rel, e))
    return sorted(out)


def census(ms):
    c = {"manifests": len(ms)}

    def top(key):
        return sum(1 for _, m in ms if key in m)

    for k in ("language", "source", "sources", "project", "exitCode",
              "expectedStdout", "expectDiagnostics", "optimizedPipelines",
              "targets"):
        c["top." + k] = top(k)

    tgts = [t for _, m in ms for t in m.get("targets", [])]
    c["targets"] = len(tgts)
    for k in ("spec", "artifact", "runOn", "emulator", "exitCode",
              "expectedStdout", "dependsOn", "prebuiltLibraries", "$comment"):
        c["target." + k] = sum(1 for t in tgts if k in t)

    # `dependsOn` nests; count entries at every depth and the manifests involved.
    def walk_deps(entries):
        n = nested = must = 0
        for d in entries:
            n += 1
            if d.get("mustDifferFromBaseline") is True:
                must += 1
            kids = d.get("dependsOn", [])
            if kids:
                kn, knested, kmust = walk_deps(kids)
                n += kn
                nested += kn + knested
                must += kmust
        return n, nested, must

    dep_entries = dep_nested = dep_must = 0
    dep_manifests = set()
    for rel, m in ms:
        got = False
        for t in m.get("targets", []):
            if t.get("dependsOn"):
                got = True
                a, b, d = walk_deps(t["dependsOn"])
                dep_entries += a
                dep_nested += b
                dep_must += d
        if got:
            dep_manifests.add(rel)
    c["dependsOn.entries"] = dep_entries
    c["dependsOn.nested"] = dep_nested
    c["dependsOn.mustDiffer"] = dep_must
    c["dependsOn.manifests"] = len(dep_manifests)

    # `prebuiltLibraries` -- the P32 key. Counted the same way, so it can never
    # become the next figure nobody re-derived.
    pre_entries = 0
    pre_manifests = set()
    for rel, m in ms:
        for t in m.get("targets", []):
            n = len(t.get("prebuiltLibraries", []))
            if n:
                pre_entries += n
                pre_manifests.add(rel)
    c["prebuiltLibraries.entries"] = pre_entries
    c["prebuiltLibraries.manifests"] = len(pre_manifests)

    arms = [a for _, m in ms for a in m.get("optimizedPipelines", [])]
    c["arms"] = len(arms)
    c["arms.shippedPipeline"] = sum(1 for a in arms if "shippedPipeline" in a)
    c["arms.passes"] = sum(1 for a in arms if "passes" in a)
    c["arms.mustDifferTrue"] = sum(1 for a in arms
                                   if a.get("mustDifferFromBaseline") is True)
    c["arms.mustDifferFalse"] = sum(1 for a in arms
                                    if a.get("mustDifferFromBaseline") is False)

    c["targets.runOnEmpty"] = sum(1 for t in tgts if t.get("runOn") == [])
    c["manifests.allRunOnEmpty"] = sum(
        1 for _, m in ms
        if m.get("targets") and all(t.get("runOn") == [] for t in m["targets"]))
    return c


def main(argv):
    ms = manifests()
    if len(ms) < FLOOR:
        sys.exit("examples-census: FAIL -- the scan COLLAPSED: found only %d manifest(s) "
                 "under %s, floor is %d. A census over an empty set prints a tidy set of "
                 "zeroes; fix the scan rather than the floor." % (len(ms), CORPUS, FLOOR))
    c = census(ms)
    if "--json" in argv:
        print(json.dumps(c, indent=2, sort_keys=True))
        return 0
    for k in sorted(c):
        print("  %-32s %d" % (k, c[k]))
    print("\nexamples-census: %d manifest(s), %d target entr(ies), %d optimizer arm(s)"
          % (c["manifests"], c["targets"], c["arms"]))
    print("⚠ These are a DATED INVENTORY, not an invariant. Re-run rather than quote.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
