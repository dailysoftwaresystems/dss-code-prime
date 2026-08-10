#!/usr/bin/env python3
"""Emit a `.dss-project.json` for the full-source SQLite testfixture.

★ ONE MANIFEST GENERATOR, BOTH DRIVERS. This file is named for the leg it was
written for, and the name is now the only pe-specific thing about it (a rename
is a separate change). build-and-test.sh used to carry a SECOND, inline python
heredoc that emitted the same manifest shape; the two then had to be kept in
step by hand, which is the duplication that produced "a capability in one driver
and not the other is a silent harness bug". Both drivers now call THIS file, and
everything that differs between legs arrives as an ARGUMENT — the target spec,
the resolve-library binaries, the recipe transform, the stack reserve.

Arrays reach here via FILES (tus.txt / includes.txt / defines.txt, one entry per
line — never argv) so ~185 paths can't overflow the Windows command line, and
PowerShell never has to escape `$`/quotes through a python body.

Manifest fields: language c-subset / profile cli / one target / artifactName /
the TU set as `sources` / the `includes` dirs / the `defines` (a defensive
leading `-D` stripped; an empty `SQLITE_PRIVATE=` value preserved) / the
(tcl, z) libraries as `resolveLibraries` / an optional `stackReserve`.

DEFAULTS ARE THE PE64 LEG'S, deliberately: build-and-test.ps1's call site passes
neither `--recipe-transform` nor `--stack-reserve`, and its behaviour must not
change because the .sh started sharing this generator. build-and-test.sh always
passes both explicitly, from the leg's own declaration in legs.json.
"""
import argparse
import json
import re
import sys

# The recipe transforms this generator implements. The NAMES are the catalogue's
# (`build.recipeTransform` in legs.json, validated against RECIPE_TRANSFORMS in
# harness_legs.py) — one vocabulary, so a leg's declaration reads the same in the
# catalogue, in the resolver and here.
RECIPE_TRANSFORMS = ("none", "windows-selfconfig")


def read_lines(path):
    with open(path, "r", encoding="utf-8") as f:
        return [ln.strip() for ln in f if ln.strip()]


def strip_d(d):
    return d[2:] if d.startswith("-D") else d   # tus/defines files are pre-stripped; defensive


def resolve_library_entry(spec):
    """D-FFI-DECLARED-IMPORT-NAME: one `--resolve-library` argument -> one
    `resolveLibraries` manifest entry.

    `PATH`                  -> the PLAIN string entry (nothing stated). Byte-for-byte
                               what this generator has always emitted, so every
                               existing leg's manifest is unchanged.
    `PATH=IMPORT_NAME`      -> the EXTENDED object entry
                               `{"path": …, "importName": …}`, which STATES the runtime
                               identity to record (DT_NEEDED / LC_LOAD_DYLIB / PE import
                               name) instead of the binary's own embedded soname.

    Needed for CROSS-BUILDS off a stand-in library: a MacPorts `libtcl8.6.dylib`
    carries `LC_ID_DYLIB = /opt/local/lib/libtcl8.6.dylib`, so a testfixture linked
    against it would demand MacPorts at that prefix on the target Mac. Symbols from
    the downloaded dylib, install name from the declaration.

    Split on the LAST `=`, matching the DSS CLI's `--resolve-library
    <path>[=<import-name>]`: the PATH is the `=`-tolerant side, an import name is not.
    Both sides must be non-empty — a driver that emitted `{"path": ""}` would only move
    the failure into the compiler, and the point of generating the manifest here is to
    fail where the operator can see which argument was wrong.
    """
    path, sep, name = spec.rpartition("=")
    if not sep:
        return spec                      # no '=' at all: the plain form
    if not path or not name:
        raise SystemExit(
            "gen-pe64-manifest: --resolve-library '%s' has an empty %s; the form is "
            "PATH[=IMPORT_NAME] and both sides must be non-empty (omit '=' entirely "
            "to record the binary's own embedded soname)."
            % (spec, "PATH" if not path else "IMPORT_NAME"))
    return {"path": path, "importName": name}


def main(argv=None):
    p = argparse.ArgumentParser(prog="gen-pe64-manifest.py")
    p.add_argument("--tus", required=True, help="file: one absolute TU path per line")
    p.add_argument("--includes", required=True, help="file: one include dir per line")
    p.add_argument("--defines", required=True, help="file: one NAME[=VALUE] per line")
    p.add_argument("--target", required=True, help="<targetName>:<formatName> spec")
    p.add_argument("--resolve-library", action="append", default=[],
                   metavar="PATH[=IMPORT_NAME]",
                   help="a resolve-library binary (repeatable) — the tcl + zlib DLLs. "
                        "The optional '=IMPORT_NAME' STATES the runtime identity to "
                        "record (DT_NEEDED / LC_LOAD_DYLIB / PE import name) instead of "
                        "the binary's own embedded soname — for cross-building against "
                        "a stand-in library (e.g. a MacPorts dylib whose LC_ID_DYLIB is "
                        "/opt/local/lib/...). Split on the LAST '=', matching the DSS "
                        "CLI's --resolve-library <path>[=<import-name>].")
    p.add_argument("--artifact-name", default="testfixture")
    p.add_argument("--extra-define", action="append", default=[], metavar="NAME[=VALUE]",
                   help="an extra define prepended to the recipe defines (opt-in shim only)")
    p.add_argument("--recipe-transform", default="windows-selfconfig",
                   choices=RECIPE_TRANSFORMS,
                   help="how to adapt the POSIX-derived recipe defines for this "
                        "leg's TARGET (legs.json `build.recipeTransform`). "
                        "DEFAULT 'windows-selfconfig' — the pe64 behaviour this "
                        "generator shipped with, kept so build-and-test.ps1's "
                        "call site is unchanged.")
    p.add_argument("--stack-reserve", type=int, default=8 * 1024 * 1024,
                   metavar="BYTES",
                   help="per-program stack reserve to request in the emitted "
                        "image (legs.json `build.stackReserveBytes`). 0 OMITS "
                        "the key entirely, leaving the object format's declared "
                        "default. DEFAULT 8388608 (8 MiB) — see the note below.")
    p.add_argument("--output", required=True, help="path of the .dss-project.json to write")
    args = p.parse_args(argv)

    sources = read_lines(args.tus)
    includes = read_lines(args.includes)
    defines = [strip_d(d) for d in args.extra_define] + [strip_d(d) for d in read_lines(args.defines)]

    # ── recipe transform ──────────────────────────────────────────────────────
    # `windows-selfconfig`: the recipe defines are captured from a POSIX
    # `make -n`, so they carry that host's `configure`/zlib feature-probe results
    # (HAVE_*/Z_HAVE_*) — facts about the DERIVING host, not about the target.
    # Feeding a foreign host's probe results to a `_WIN32` build is a cross-compile
    # category error: SQLite/zlib must self-configure from the target's own
    # predefined macros. So drop the host-probe defines (match ONLY a leading
    # HAVE_/Z_HAVE_, never a substring — project feature flags like SQLITE_HAVE_ZLIB
    # must survive) and let the target configure itself; then add SQLITE_OS_WIN=1
    # as an explicit bridge for test helpers that probe it before sqliteInt.h.
    #   _HAVE_SQLITE_CONFIG_H is dropped for the same reason: it makes sqliteInt.h
    # `#include "sqlite_cfg.h"` (sqlite/src/sqliteInt.h:214 — the name is
    # `sqlite_cfg.h`, NOT `config.h`; this comment said `config.h` for a while and
    # contradicted the correct name 25 lines below) — the POSIX
    # `configure`-generated header carrying the
    # build host's HAVE_LOCALTIME_R etc. Inheriting it on a Windows TARGET is the
    # same cross-compile category error (date.c then picks localtime_r over plain
    # localtime). Dropping it makes sqlite self-configure its Windows build from
    # _WIN32/SQLITE_OS_WIN (date.c falls to plain localtime → resolves via msvcrt).
    #
    # ★ THE RULE IS KEYED ON THE LEG, NOT ON THIS FILE. It used to say "this
    # generator is pe-only, so keying the rule on the pe target is inherent here"
    # — which stopped being true the moment the .sh started calling it for five
    # legs. The decision now lives in the catalogue (legs.json declares
    # `recipeTransform: "windows-selfconfig"` on the pe64 leg and `"none"` on the
    # four POSIX-target legs) and arrives here as an argument.
    #
    # ⛔ AND THE SENTENCE THAT USED TO FOLLOW WAS THE BUG. It read: "An elf/mach-o
    # leg must keep its HAVE_* defines: dropping them there would discard real,
    # correct configuration" — TRUE for elf, FALSE for mach-o, and stated as one
    # rule over both. It is the sentence that licensed `recipeTransform: "none"`
    # on the two Darwin legs, whose CLI then failed on `off64_t`/`pread64`/
    # `pwrite64`: the deriving host's probes are the target's only when the target
    # IS that host's platform family, which for a Darwin leg on a Linux deriving
    # box it is not.
    #
    # ★ THE REMEDY IS NOT IN THIS FILE, and that is the point of correcting the
    # prose rather than adding a transform. The Darwin leak does not travel on the
    # command line at all: `_HAVE_SQLITE_CONFIG_H` (which DOES survive here,
    # deliberately) makes sqliteInt.h `#include "sqlite_cfg.h"`, and the answers
    # arrive INSIDE that header. A recipe transform cannot reach inside a header;
    # the only one that could would drop `_HAVE_SQLITE_CONFIG_H` wholesale and
    # throw ~49 correct answers away with the 3 wrong ones (HAVE_GMTIME_R,
    # HAVE_FDATASYNC, HAVE_USLEEP among them). So each leg instead DECLARES
    # `build.configureAnswers` and stage-zinc.py writes it a target-specific
    # sqlite_cfg.h, placed ahead of the deriving host's on the include list. See
    # legs.json's $configureAnswersComment.
    #
    # ✔MEASURED 2026-08-05, macho64-arm64 CLI, 103 TUs, `--config=release`, from a
    # WINDOWS host: recipe verbatim -> 4 errors (off64_t x2, pread64, pwrite64);
    # with the staged Darwin header and this transform still `none` -> ZERO.
    #
    # ⚠ AND WHAT IS *NOT* THE REASON `none` IS RIGHT FOR THE FOUR POSIX LEGS —
    # ✔MEASURED 2026-08-05 (TF-C121), against the recipe this harness actually
    # derives: `stage/defines.txt` and `stage/cli-defines.txt` contain NO bare
    # `HAVE_*` and NO `Z_HAVE_*` AT ALL. The only two that even look like one are
    # `SQLITE_HAVE_ZLIB=1` (a project feature flag, which `^(HAVE_|Z_HAVE_)`
    # deliberately does not match) and `_HAVE_SQLITE_CONFIG_H`. So the `^(HAVE_|
    # Z_HAVE_)` arm below is a NO-OP on today's recipe, and the single define this
    # transform actually removes is `_HAVE_SQLITE_CONFIG_H`.
    # The regex arm STAYS as a standing guard — upstream's recipe is re-derived from
    # `make -n` every run and may grow such a define without warning — but it must
    # not be described as load-bearing, and "dropping those would discard real
    # configuration" is not why `none` is correct for elf/mach-o. `none` is correct
    # for them because `_HAVE_SQLITE_CONFIG_H` must SURVIVE (it is the whole
    # delivery mechanism for the ~49 answers that ARE the target's), and the three
    # answers that differ are corrected inside the staged header instead.
    # harness_legs.py and legs.json's `$configureAnswersComment` state the same
    # fact — "_HAVE_SQLITE_CONFIG_H is the ONE host-probe define left on the
    # command line" — and this paragraph used to contradict both.
    if args.recipe_transform == "windows-selfconfig":
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
        _summary = "recipe-transform 'windows-selfconfig': dropped %d host-probe defines" % len(_dropped)
        if _added:
            _summary += ", added " + ", ".join(_added)
        if _dropped:
            _summary += "  [dropped: " + ", ".join(_d.split("=", 1)[0] for _d in _dropped) + "]"
        print(_summary)
    else:
        # Stated out loud rather than left silent: "no transform" is a DECISION the
        # leg made, and a reader of the build log must be able to tell it apart
        # from "the transform ran and happened to change nothing".
        print("recipe-transform 'none': the %d recipe defines are passed through "
              "verbatim (this leg's target shares the deriving host's ABI "
              "assumptions)" % len(defines))

    if not sources:
        sys.stderr.write("gen-pe64-manifest.py: error: no TUs (empty %s)\n" % args.tus)
        return 1
    if args.stack_reserve < 0:
        sys.stderr.write("gen-pe64-manifest.py: error: --stack-reserve must not be "
                         "negative (got %d)\n" % args.stack_reserve)
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
    #
    # ★ 0 OMITS THE KEY — it does not write `"stackReserve": 0`. That is not a
    # style choice: src/program/project_config.cpp REFUSES a zero
    # ("a zero-byte stack reserve cannot start a program. Omit the field to take
    # the object format's declared default"), and the ELF/Mach-O formats declare
    # no stack-reserve capability at all, so a request they cannot carry is
    # refused outright rather than silently dropped. The four POSIX-target legs
    # declare `stackReserveBytes: 0` in legs.json precisely to mean "omit".
    manifest = {
        "language":         "c-subset",
        "artifactProfile":  "cli",
        "targets":          [args.target],
        "artifactName":     args.artifact_name,
        "sources":          sources,
        "includes":         includes,
        "defines":          defines,
        # D-FFI-DECLARED-IMPORT-NAME: a bare PATH stays a plain string (every
        # existing leg's manifest byte-identical); `PATH=IMPORT_NAME` becomes the
        # extended `{"path", "importName"}` object.
        "resolveLibraries": [resolve_library_entry(s) for s in args.resolve_library],
    }
    if args.stack_reserve > 0:
        manifest["stackReserve"] = args.stack_reserve
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    # sanity: every source exists on disk (a staged tree miss fails loud here,
    # not mid-compile 6 minutes later).
    import os
    missing = [s for s in sources if not os.path.isfile(s)]
    print("sources=%d includes=%d defines=%d resolveLibraries=%d stackReserve=%s missing=%d" %
          (len(sources), len(includes), len(defines),
           len(manifest["resolveLibraries"]),
           manifest.get("stackReserve", "<format default>"), len(missing)))
    for m in missing[:10]:
        print("  MISSING SOURCE: " + m, file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
