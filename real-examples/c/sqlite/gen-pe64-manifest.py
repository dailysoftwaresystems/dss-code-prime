#!/usr/bin/env python3
"""Emit the pe64 `.dss-project.json` for the full-source SQLite testfixture.

The Windows companion to the `generate_manifest` python heredoc inside
build-and-test.sh — same manifest shape, driven by the recipe files the WSL
derivation stages onto Windows (tus.txt / includes.txt / defines.txt, one entry
per line, ABSOLUTE forward-slash Windows paths) plus the pe64 target + the two
resolve-library DLLs. Kept as a real file (not a here-string) so PowerShell
never has to escape `$`/quotes through a python body.

Arrays reach here via the FILES (never argv) so 185 paths can't overflow the
Windows command line. Mirrors the .sh generator's fields exactly:
language c-subset / profile cli / one target / artifactName testfixture /
the TU set as `sources` / the `includes` dirs / the `defines` (a defensive
leading `-D` stripped; an empty `SQLITE_PRIVATE=` value preserved) / the
(tcl, z) DLLs as `resolveLibraries`.
"""
import argparse
import json
import sys


def read_lines(path):
    with open(path, "r", encoding="utf-8") as f:
        return [ln.strip() for ln in f if ln.strip()]


def strip_d(d):
    return d[2:] if d.startswith("-D") else d   # tus/defines files are pre-stripped; defensive


def main(argv=None):
    p = argparse.ArgumentParser(prog="gen-pe64-manifest.py")
    p.add_argument("--tus", required=True, help="file: one absolute TU path per line")
    p.add_argument("--includes", required=True, help="file: one include dir per line")
    p.add_argument("--defines", required=True, help="file: one NAME[=VALUE] per line")
    p.add_argument("--target", required=True, help="<targetName>:<formatName> spec")
    p.add_argument("--resolve-library", action="append", default=[], metavar="PATH",
                   help="a resolve-library binary (repeatable) — the tcl + zlib DLLs")
    p.add_argument("--artifact-name", default="testfixture")
    p.add_argument("--extra-define", action="append", default=[], metavar="NAME[=VALUE]",
                   help="an extra define prepended to the recipe defines (opt-in shim only)")
    p.add_argument("--output", required=True, help="path of the .dss-project.json to write")
    args = p.parse_args(argv)

    sources = read_lines(args.tus)
    includes = read_lines(args.includes)
    defines = [strip_d(d) for d in args.extra_define] + [strip_d(d) for d in read_lines(args.defines)]

    if not sources:
        sys.stderr.write("gen-pe64-manifest.py: error: no TUs (empty %s)\n" % args.tus)
        return 1

    manifest = {
        "language":         "c-subset",
        "artifactProfile":  "cli",
        "targets":          [args.target],
        "artifactName":     args.artifact_name,
        "sources":          sources,
        "includes":         includes,
        "defines":          defines,
        "resolveLibraries": args.resolve_library,
    }
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    # sanity: every source exists on disk (a staged tree miss fails loud here,
    # not mid-compile 6 minutes later).
    import os
    missing = [s for s in sources if not os.path.isfile(s)]
    print("sources=%d includes=%d defines=%d resolveLibraries=%d missing=%d" %
          (len(sources), len(includes), len(defines),
           len(manifest["resolveLibraries"]), len(missing)))
    for m in missing[:10]:
        print("  MISSING SOURCE: " + m, file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
