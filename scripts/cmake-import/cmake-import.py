#!/usr/bin/env python3
"""Transform a CMake compile_commands.json into a DSS `.dss-project.json`.

This is the SINGLE SOURCE OF TRUTH for the compile_commands -> manifest
transform. Both `cmake-import.sh` and `cmake-import.ps1` are thin wrappers
that run `cmake` to produce the compile_commands.json, then invoke this file
to emit the manifest — so the two shells produce byte-identical output by
construction (they run the same code).

It aggregates, across EVERY translation unit in compile_commands.json:
  * sources  — each entry's `file`      (absolute, forward-slash, deduped, sorted)
  * includes — every -I / -isystem dir   (resolved vs the entry `directory`,
               deduped, sorted)
  * defines  — every -D NAME[=VALUE]     (the -D stripped, deduped, sorted)
All other compiler flags (-c, -o, -O, -g, warnings, -std=...) are ignored —
DSS derives those itself. Link libraries are NOT present in compile_commands
(it is compile-only), so `resolveLibraries` is never emitted.
"""

import argparse
import json
import sys


# ─────────────────────────────────────────────────────────────────────────────
# path canonicalization (pure string ops — no filesystem access)
# ─────────────────────────────────────────────────────────────────────────────
def is_alpha(c):
    o = ord(c)
    return (97 <= o <= 122) or (65 <= o <= 90)


def is_absolute(p):
    if p.startswith("/"):
        return True
    if len(p) >= 2 and p[1] == ":" and is_alpha(p[0]):
        return True
    return False


def collapse(p):
    """Lexical path normalization on a forward-slash string."""
    drive = ""
    root = ""
    rest = p
    if rest.startswith("/"):
        root = "/"; rest = rest[1:]
    elif len(rest) >= 2 and rest[1] == ":" and is_alpha(rest[0]):
        drive = rest[0:2]; rest = rest[2:]
        if rest.startswith("/"):
            root = "/"; rest = rest[1:]
    out = []
    for seg in rest.split("/"):
        if seg == "" or seg == ".":
            continue
        if seg == "..":
            if out and out[-1] != "..":
                out.pop()
            elif root == "":
                out.append("..")
            continue
        out.append(seg)
    return drive + root + "/".join(out)


def canon(raw, directory):
    """Make `raw` absolute (relative to `directory`) with forward slashes."""
    p = raw.replace("\\", "/")
    if not is_absolute(p):
        d = directory.replace("\\", "/")
        if d and not d.endswith("/"):
            d += "/"
        p = d + p
    return collapse(p)


# ─────────────────────────────────────────────────────────────────────────────
# command-string tokenizer
# ─────────────────────────────────────────────────────────────────────────────
def tokenize(s):
    """Minimal shell-like tokenizer for a compile_commands.json `command`.

    Whitespace separates tokens; "..." and '...' spans preserve inner spaces.
    Backslash is handled ONLY for an escaped double-quote (\\" -> literal ")
    and, inside a "..." span, \\\\ -> literal \\ ; every other backslash stays
    literal, so Windows paths (C:\\Users\\..., a lone backslash before a letter)
    survive. This matches how CMake/Ninja shell-escape string-valued -D
    defines: -DVER=\\"1.2.3\\" -> the define VER="1.2.3", and
    -DMSG="\\"hi there\\"" -> MSG="hi there". Single-quoted spans are fully
    literal (POSIX).
    """
    toks = []
    cur = None
    quote = None                       # None, '"', or "'"
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if quote == '"':
            if c == "\\" and i + 1 < n and s[i + 1] in ('"', "\\"):
                cur += s[i + 1]; i += 2; continue
            if c == '"':
                quote = None; i += 1; continue
            cur += c; i += 1; continue
        if quote == "'":
            if c == "'":
                quote = None; i += 1; continue
            cur += c; i += 1; continue
        # not inside a quote span
        if c in " \t\r\n":
            if cur is not None:
                toks.append(cur); cur = None
            i += 1; continue
        if c == "\\" and i + 1 < n and s[i + 1] == '"':
            if cur is None: cur = ""
            cur += '"'; i += 2; continue
        if c == '"':
            if cur is None: cur = ""
            quote = '"'; i += 1; continue
        if c == "'":
            if cur is None: cur = ""
            quote = "'"; i += 1; continue
        if cur is None: cur = ""
        cur += c
        i += 1
    if cur is not None:
        toks.append(cur)
    return toks


def relativize(abs_path, base):
    """Lexical: `abs_path` relative to `base` when under it, else `abs_path`.

    Both are already normalized forward-slash strings. `abs == base` -> "." ;
    `abs` under `base` -> the remainder (e.g. "src/main.c") ; otherwise the
    absolute path is kept (e.g. a toolchain -isystem dir outside the project).
    Pure string op — no filesystem access.
    """
    if base.endswith("/"):
        base = base[:-1]                 # tolerate a root-ish base
    if abs_path == base:
        return "."
    prefix = base + "/"
    if abs_path.startswith(prefix):
        return abs_path[len(prefix):]
    return abs_path


def sanitize_name(s):
    """Reduce a raw string to a bare file name ([A-Za-z0-9._-], else '_')."""
    out = []
    for c in s:
        o = ord(c)
        if (97 <= o <= 122) or (65 <= o <= 90) or (48 <= o <= 57) or c in "._-":
            out.append(c)
        else:
            out.append("_")
    return "".join(out)


ISYS = "-isystem"


# ─────────────────────────────────────────────────────────────────────────────
def aggregate(entries):
    """Return (sorted_sources, sorted_includes, sorted_defines)."""
    sources, includes, defines = set(), set(), set()
    for e in entries:
        if not isinstance(e, dict):
            continue
        directory = e.get("directory", "") or ""
        args = e.get("arguments")
        if isinstance(args, list):
            toks = [str(x) for x in args]
        else:
            toks = tokenize(e.get("command", "") or "")

        f = e.get("file", "") or ""
        if f:
            sources.add(canon(f, directory))

        j, m = 0, len(toks)
        while j < m:
            t = toks[j]
            if t == "-I":
                if j + 1 < m:
                    includes.add(canon(toks[j + 1], directory)); j += 2; continue
            elif t.startswith("-I") and len(t) > 2:
                includes.add(canon(t[2:], directory)); j += 1; continue
            elif t == ISYS:
                if j + 1 < m:
                    includes.add(canon(toks[j + 1], directory)); j += 2; continue
            elif t.startswith(ISYS) and len(t) > len(ISYS):
                includes.add(canon(t[len(ISYS):], directory)); j += 1; continue
            elif t == "-D":
                if j + 1 < m:
                    defines.add(toks[j + 1]); j += 2; continue
            elif t.startswith("-D") and len(t) > 2:
                defines.add(t[2:]); j += 1; continue
            j += 1

    return sorted(sources), sorted(includes), sorted(defines)


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="cmake-import.py",
        description="Transform a CMake compile_commands.json into a DSS "
                    ".dss-project.json manifest. (Shared transform invoked by "
                    "cmake-import.sh / cmake-import.ps1.)")
    p.add_argument("--compile-commands", required=True, metavar="PATH",
                   help="path to compile_commands.json")
    p.add_argument("--output", required=True, metavar="FILE",
                   help="path of the .dss-project.json to write")
    p.add_argument("--target", required=True, action="append", metavar="SPEC",
                   help="DSS <targetName>:<formatName> spec (repeatable)")
    p.add_argument("--language", required=True, metavar="NAME",
                   help="DSS language name (e.g. c-subset)")
    p.add_argument("--profile", required=True, metavar="NAME",
                   help="DSS artifactProfile (e.g. cli)")
    p.add_argument("--artifact-name", metavar="NAME", default=None,
                   help="explicit binary base name (a bare name, no path "
                        "separators)")
    p.add_argument("--default-artifact-name", metavar="RAW", default=None,
                   help="fallback name (sanitized to a bare name) used when "
                        "--artifact-name is absent")
    p.add_argument("--relative-to", metavar="DIR", default=None,
                   help="project root: sources (and includes under it) are "
                        "emitted RELATIVE to this dir; includes outside it stay "
                        "absolute. Supplied by the wrappers from the root arg.")
    args = p.parse_args(argv)

    if not args.language:
        p.error("--language must be non-empty")
    if not args.profile:
        p.error("--profile must be non-empty")

    # ── artifact name: explicit (validated verbatim) or sanitized fallback ──
    if args.artifact_name is not None:
        name = args.artifact_name
        if "/" in name or "\\" in name:
            p.error("--artifact-name must be a bare file name (no '/' or "
                    "'\\'): %r" % name)
        artifact = name
    elif args.default_artifact_name is not None:
        artifact = sanitize_name(args.default_artifact_name)
    else:
        artifact = ""

    # ── parse compile_commands.json ────────────────────────────────────────
    try:
        with open(args.compile_commands, "r", encoding="utf-8") as f:
            entries = json.load(f)
    except FileNotFoundError:
        sys.stderr.write("cmake-import.py: error: compile_commands.json not "
                         "found: %s\n" % args.compile_commands)
        return 1
    except Exception as e:
        sys.stderr.write("cmake-import.py: error: could not read "
                         "compile_commands.json: %s\n" % e)
        return 1

    if not isinstance(entries, list):
        sys.stderr.write("cmake-import.py: error: compile_commands.json is not "
                         "a JSON array\n")
        return 1

    sources, includes, defines = aggregate(entries)

    # Emit paths RELATIVE to the project root when they live under it (sources
    # always do; a system/toolchain -isystem dir may not — it stays absolute),
    # so the manifest is portable. Re-sort the final (relativized) strings.
    if args.relative_to is not None:
        base = collapse(args.relative_to.replace("\\", "/"))
        sources = sorted({relativize(s, base) for s in sources})
        includes = sorted({relativize(s, base) for s in includes})

    if not sources:
        sys.stderr.write("cmake-import.py: error: no source files found in "
                         "compile_commands.json (nothing to import)\n")
        return 1

    # ── dedup-preserve-order for targets ───────────────────────────────────
    seen = set(); targets = []
    for t in args.target:
        if t not in seen:
            seen.add(t); targets.append(t)

    # ── build manifest (fixed key order) ───────────────────────────────────
    manifest = {}
    manifest["language"] = args.language
    manifest["artifactProfile"] = args.profile
    manifest["targets"] = targets
    if artifact:
        manifest["artifactName"] = artifact
    manifest["sources"] = sources
    if includes:
        manifest["includes"] = includes
    if defines:
        manifest["defines"] = defines

    text = json.dumps(manifest, indent=2, ensure_ascii=False)
    try:
        with open(args.output, "wb") as f:
            f.write(text.encode("utf-8"))
            f.write(b"\n")
    except OSError as e:
        sys.stderr.write("cmake-import.py: error: could not write %s: %s\n"
                         % (args.output, e))
        return 1

    # ── summary ────────────────────────────────────────────────────────────
    sys.stdout.write("cmake-import: %d sources, %d includes, %d defines -> %s\n"
                     % (len(sources), len(includes), len(defines), args.output))
    sys.stdout.write("note: link libraries are NOT captured from "
                     "compile_commands.json (compile-only). Add a "
                     "\"resolveLibraries\" array by hand if this project links "
                     "external libraries.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
