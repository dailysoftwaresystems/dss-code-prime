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

    # pe cross-compile auto-config. The recipe defines are captured from a LINUX
    # `make -n`, so they carry that host's `configure`/zlib feature-probe results
    # (HAVE_*/Z_HAVE_*) — facts about the build HOST, not the Windows TARGET.
    # Feeding a foreign host's probe results to a `_WIN32` build is a cross-compile
    # category error: SQLite/zlib must self-configure from the target's own
    # predefined macros. So drop the host-probe defines (match ONLY a leading
    # HAVE_/Z_HAVE_, never a substring — project feature flags like SQLITE_HAVE_ZLIB
    # must survive) and let the target configure itself; then add SQLITE_OS_WIN=1
    # as an explicit bridge for test helpers that probe it before sqliteInt.h. This
    # generator is pe-only, so keying the rule on the pe target is inherent here.
    #   _HAVE_SQLITE_CONFIG_H is dropped for the same reason: it makes sqliteInt.h
    # `#include "config.h"` — the LINUX `configure`-generated header carrying the
    # build host's HAVE_LOCALTIME_R etc. Inheriting it on a Windows TARGET is the
    # same cross-compile category error (date.c then picks localtime_r over plain
    # localtime). Dropping it makes sqlite self-configure its Windows build from
    # _WIN32/SQLITE_OS_WIN (date.c falls to plain localtime → resolves via msvcrt).
    import re
    _host_probe = re.compile(r"^(HAVE_|Z_HAVE_)")
    _config_h = "_HAVE_SQLITE_CONFIG_H"
    _kept, _dropped = [], []
    for _d in defines:
        _name = _d.split("=", 1)[0]
        (_dropped if (_host_probe.match(_name) or _name == _config_h) else _kept).append(_d)
    defines = _kept
    _added = []
    if not any(_d.split("=", 1)[0] == "SQLITE_OS_WIN" for _d in defines):
        defines.append("SQLITE_OS_WIN=1")
        _added.append("SQLITE_OS_WIN")
    _summary = "pe-config: dropped %d host-probe defines" % len(_dropped)
    if _added:
        _summary += ", added " + ", ".join(_added)
    if _dropped:
        _summary += "  [dropped: " + ", ".join(_d.split("=", 1)[0] for _d in _dropped) + "]"
    print(_summary)

    if not sources:
        sys.stderr.write("gen-pe64-manifest.py: error: no TUs (empty %s)\n" % args.tus)
        return 1

    # ── stackReserve: why this is here, and why THIS number ────────────────────
    # sqlite's `full`/`all` tiers contain e_fkey-63.1.x, which recurses to
    # SQLITE_MAX_TRIGGER_DEPTH (1000) levels of NESTED TRIGGER through a
    # PREPARE-TIME codegen cycle (sqlite3DeleteFrom -> sqlite3GenerateRowDelete
    # -> sqlite3FkActions -> sqlite3CodeRowTriggerDirect -> getRowTrigger ->
    # codeRowTrigger -> ...). Windows' DEFAULT process stack is 1 MiB; Linux
    # gives 8 MiB, which is exactly why this never surfaced on the elf legs.
    #
    # MEASURED (not guessed), same machine, same test file, reserve matched by
    # patching the PE header so the comparison is controlled:
    #   * a NATIVE gcc-built testfixture ALSO stack-overflows at 1 MiB. Its own
    #     bisected minimum is 1.75 MiB. So this is NOT merely a DSS defect —
    #     no compiler passes this test at the Windows default.
    #   * DSS's bisected minimum is 3.38 MiB (~1.93x gcc's, tracked separately
    #     as D-CODEGEN-FRAME-SIZE-VS-NATIVE-2X — that gap is real and is why
    #     2 MiB would fix gcc and NOT fix DSS).
    # Upstream sets no /STACK in Makefile.msc and no -Wl,--stack anywhere; its
    # CI does not hit this because it tests on Linux. e_fkey.test itself already
    # self-gates the 63.x block against high-stack (ASan) builds.
    #
    # 8 MiB = 2.4x DSS's measured minimum, and is deliberately the SAME figure
    # Linux hands the process for free — so the pe64 leg is asking for parity
    # with the leg that already passes, not for an arbitrary indulgence. Sizing
    # off gcc's 1.75 MiB would be the trap: it is the wrong compiler's number.
    # Full evidence: D-SQLITE-PE64-FULL-TIER-STACK-DEPTH.
    stack_reserve = 8 * 1024 * 1024

    manifest = {
        "language":         "c-subset",
        "artifactProfile":  "cli",
        "targets":          [args.target],
        "artifactName":     args.artifact_name,
        "sources":          sources,
        "includes":         includes,
        "defines":          defines,
        "resolveLibraries": args.resolve_library,
        "stackReserve":     stack_reserve,
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
