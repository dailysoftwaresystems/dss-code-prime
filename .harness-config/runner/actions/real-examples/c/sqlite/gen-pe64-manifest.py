#!/usr/bin/env python3
"""Emit a `.dss-project.json` for the full-source SQLite testfixture.

★ ONE MANIFEST GENERATOR. This file is named for the leg it was written for,
and the name is now the only pe-specific thing about it (a rename is a separate
change). The old bash driver (`build-and-test.sh`) used to carry a SECOND, inline
python heredoc that emitted the same manifest shape; the two then had to be kept
in step by hand, which is the duplication that produced "a capability in one
driver and not the other is a silent harness bug". The driver calls THIS file for
both artefacts (`sqlite_build.py`, through `sqlite_base.generate_manifest`), and
everything that differs between legs arrives as an ARGUMENT — the target spec,
the resolve-library binaries, the recipe transform, the stack reserve.

Arrays reach here via FILES (tus.txt / includes.txt / defines.txt, one entry per
line — never argv) so ~185 paths can't overflow the Windows command line, and no
caller has to escape `$`/quotes through a command line (the old PowerShell
driver's problem, and the reason files were chosen).

Manifest fields: language c / profile cli / one target / artifactName /
the TU set as `sources` / the `includes` dirs / the `defines` (a defensive
leading `-D` stripped; an empty `SQLITE_PRIVATE=` value preserved) / the
(tcl, z) libraries as `resolveLibraries` / an optional `stackReserve`.

`--tu-preludes FILE` (a leg's declared `build.tuPreludes`, as JSON) compiles a
named TU AFTER lines its leg declares for it: the TU's `sources` entry becomes a
generated WRAPPER, `<manifest dir>/tu-preludes/<digest>/<tu>`, holding those lines
and then `#include "<the real TU>"`. Every consumer of the manifest -- dsscp and the
same-platform reference oracle alike -- compiles the wrapper, so both compilers
are handed the same prelude by construction. See `apply_tu_preludes`.

`--self-test` runs this file's own arms (the prelude application and its refusals).

DEFAULTS ARE THE PE64 LEG'S, deliberately (history: the old PowerShell driver's
call site passed neither `--recipe-transform` nor `--stack-reserve`, and its
behaviour could not change when the bash driver started sharing this generator).
The driver passes both explicitly, from the leg's own declaration in legs.json:
`sqlite_base.generate_manifest` requires them, because an omitted flag takes these
pe64 defaults.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tempfile

# The action is `requireInputsUnmoved`: no __pycache__ may appear beside its programs.
sys.dont_write_bytecode = True

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
    """Declared import names: one `--resolve-library` argument -> one
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


# ── TU PRELUDES: a TU compiled after lines its leg declares for it ─────────────────
#
# ★ WHY A WRAPPER TU, AND NOT A FORCED INCLUDE OR A SHADOW HEADER. A dsscp project
# manifest has no per-TU forced include, and a shadow header on the include path
# would change EVERY TU that includes it, not the one that needs it. A wrapper is
# plain C both compilers already accept: its own lines, then `#include` of the real
# TU by its absolute path -- so a quoted include INSIDE the real TU still resolves
# beside the real file, exactly as before. The wrapper keeps the TU's relative path
# as its own tail, so everything keyed on the TU's name (a `build-tu` row's
# pattern, a diagnostic's file) still names the same TU.
# ★ THE DIRECTORY IS NAMED BY ITS CONTENT, so a wrapper an earlier generation wrote
# for other lines can never be the one a manifest names.
TU_PRELUDE_KEYS = ("tu", "artifacts", "lines")
TU_PRELUDE_DIR = "tu-preludes"


class PreludeRefused(Exception):
    """A declared prelude this generator will not apply; the message says why."""


def _check_tu_prelude(i, entry):
    at = "entry %d" % i
    if not isinstance(entry, dict):
        raise PreludeRefused("%s is not an object {tu, artifacts, lines}" % at)
    extra = sorted(k for k in entry if not k.startswith("$") and k not in TU_PRELUDE_KEYS)
    if extra:
        raise PreludeRefused("%s declares unknown key(s) %s (known: %s)"
                             % (at, ", ".join(extra), ", ".join(TU_PRELUDE_KEYS)))
    tu = entry.get("tu")
    if (not isinstance(tu, str) or not tu or tu.startswith("/") or "\\" in tu
            or re.match(r"^[A-Za-z]:", tu) or ".." in tu.split("/") or "*/" in tu
            or not tu.endswith(".c")):
        raise PreludeRefused("%s: tu must be a relative, '/'-separated path to a .c file "
                             "inside the source tree (got %r)" % (at, tu))
    arts = entry.get("artifacts")
    if (not isinstance(arts, list) or not arts
            or not all(isinstance(a, str) and a for a in arts)):
        raise PreludeRefused("%s <%s>: artifacts must be a non-empty list of artifact names "
                             "(got %r)" % (at, tu, arts))
    lines = entry.get("lines")
    if (not isinstance(lines, list) or not lines
            or not all(isinstance(ln, str) and ln.strip() and "\n" not in ln and "\r" not in ln
                       for ln in lines)):
        raise PreludeRefused("%s <%s>: lines must be a non-empty list of non-empty, single-line "
                             "strings (got %r)" % (at, tu, lines))
    # A prelude CONFIGURES THE PREPROCESSOR for the TU -- it never adds code of its own. And a
    # QUOTED include would search the WRAPPER's directory first, not the TU's: only <...>.
    for ln in lines:
        if not ln.lstrip().startswith("#"):
            raise PreludeRefused("%s <%s>: %r is not a preprocessor directive; a prelude only "
                                 "configures the preprocessor, it never adds code" % (at, tu, ln))
        if re.match(r"\s*#\s*include\s*\"", ln):
            raise PreludeRefused("%s <%s>: %r is a QUOTED include, which would be searched from the "
                                 "generated wrapper's directory, not the TU's; name a header <...>"
                                 % (at, tu, ln))
    return tu, arts, lines


def check_tu_preludes(entries):
    """A leg's `build.tuPreludes`, shape-checked -> [(tu, artifacts, lines)]; `PreludeRefused`
    names the first thing wrong. THE ONE CHECK: `apply_tu_preludes` runs it before writing
    anything, and harness_legs.py's `--lint` runs this same function over the catalogue."""
    if not isinstance(entries, list) or not entries:
        raise PreludeRefused("the declaration must be a non-empty list of {tu, artifacts, lines}; "
                             "an empty one is a second spelling of declaring none")
    checked, seen = [], set()
    for i, entry in enumerate(entries):
        tu, arts, lines = _check_tu_prelude(i, entry)
        if tu in seen:
            raise PreludeRefused("<%s> is declared twice" % tu)
        seen.add(tu)
        checked.append((tu, arts, lines))
    return checked


def apply_tu_preludes(sources, entries, artifact, out_dir):
    """-> (sources with each declared TU replaced by its wrapper, report lines). A declared
    TU this artifact builds must be in `sources` EXACTLY ONCE and must exist; anything else is
    a `PreludeRefused` -- a declaration for a TU the recipe no longer has is stale, and a
    stale prelude applied to nothing would read as one that worked."""
    checked = check_tu_preludes(entries)
    out, report = list(sources), []
    for tu, arts, lines in checked:
        if artifact not in arts:
            report.append("tu-prelude: <%s> is declared for %s, not for %s -- not applied"
                          % (tu, ", ".join(arts), artifact))
            continue
        hits = [i for i, s in enumerate(out) if s.replace("\\", "/").endswith("/" + tu)]
        if len(hits) != 1:
            raise PreludeRefused("<%s> is declared for %s, and %s's recipe names it %d time(s), "
                                 "not exactly once" % (tu, artifact, artifact, len(hits)))
        real = out[hits[0]]
        if not os.path.isfile(real):
            raise PreludeRefused("<%s>: the TU %s is not a file" % (tu, real))
        spelled = real.replace("\\", "/")
        if '"' in spelled:
            raise PreludeRefused("<%s>: the TU's path %r cannot be spelled in a quoted #include"
                                 % (tu, spelled))
        text = ("/* GENERATED by gen-pe64-manifest.py from this leg's build.tuPreludes -- rewritten\n"
                "** by every manifest generation: change legs.json, never this file. The TU is\n"
                "** %s, compiled after the %d line(s) its leg declares for it. */\n" % (tu, len(lines))
                + "".join(ln + "\n" for ln in lines)
                + '#include "%s"\n' % spelled)
        digest = hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]
        wrapper = os.path.join(out_dir, TU_PRELUDE_DIR, digest, *tu.split("/"))
        os.makedirs(os.path.dirname(wrapper), exist_ok=True)
        with open(wrapper, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        out[hits[0]] = wrapper.replace("\\", "/") if "/" in real and "\\" not in real else wrapper
        report.append("tu-prelude: <%s> compiled after %d declared line(s) -> %s"
                      % (tu, len(lines), out[hits[0]]))
    return out, report


def prelude_wrapper_target(path):
    """The REAL TU a manifest source compiles when that source is a prelude wrapper THIS generator
    wrote -> the real TU's path as the wrapper's `#include` spells it; None when `path` is not
    wrapper-shaped (no `tu-preludes/<16-hex digest>/` in its directory chain). A diagnostic raised
    INSIDE the real TU names the real file, not the wrapper, so whoever matches diagnostics to a
    manifest's sources needs both. A wrapper-shaped path that is NOT what this generator writes --
    unreadable, its content not the content its digest directory names, or not ending in one
    `#include "<path>"` whose tail is the wrapper's own -- is a PreludeRefused: never guessed at."""
    parts = str(path).replace("\\", "/").split("/")
    at = [i for i in range(len(parts) - 2)
          if parts[i] == TU_PRELUDE_DIR and re.fullmatch(r"[0-9a-f]{16}", parts[i + 1])]
    if not at:
        return None
    i = at[-1]
    tail = "/".join(parts[i + 2:])
    try:
        with open(path, "r", encoding="utf-8", newline="") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError) as exc:
        raise PreludeRefused("%s is shaped like a TU-prelude wrapper but cannot be read: %s" % (path, exc))
    if hashlib.sha256(text.encode("utf-8")).hexdigest()[:16] != parts[i + 1]:
        raise PreludeRefused("%s is shaped like a TU-prelude wrapper, but its content is not the content "
                             "its digest directory names, so this generator did not write it as it is"
                             % path)
    body = [ln for ln in text.splitlines() if ln.strip()]
    m = re.fullmatch(r'#include "([^"]+)"', body[-1]) if body else None
    if not m or not m.group(1).endswith("/" + tail):
        raise PreludeRefused("%s does not end with the #include of a TU whose path ends in <%s>"
                             % (path, tail))
    return m.group(1)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] in (["--self-test"], ["--selftest"]) and len(argv) == 1:
        return self_test()
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
                        "generator shipped with (kept for the old PowerShell "
                        "driver's call site, which passed none; the driver now "
                        "always passes one).")
    p.add_argument("--stack-reserve", type=int, default=8 * 1024 * 1024,
                   metavar="BYTES",
                   help="per-program stack reserve to request in the emitted "
                        "image (legs.json `build.stackReserveBytes`). 0 OMITS "
                        "the key entirely, leaving the object format's declared "
                        "default. DEFAULT 8388608 (8 MiB) — see the note below.")
    p.add_argument("--tu-preludes", default="", metavar="FILE",
                   help="file: the leg's declared build.tuPreludes as JSON -- each declared TU this "
                        "artifact builds is compiled through a generated wrapper holding the "
                        "declared lines, then #include of the real TU (both compilers read the "
                        "same manifest). Absent: no TU is wrapped.")
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
    # — which stopped being true the moment the old bash driver started calling
    # it for five legs. The decision now lives in the catalogue (legs.json declares
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
    if args.tu_preludes:
        try:
            with open(args.tu_preludes, "r", encoding="utf-8") as f:
                entries = json.load(f)
            sources, report = apply_tu_preludes(sources, entries, args.artifact_name,
                                                os.path.dirname(os.path.abspath(args.output)))
        except (OSError, ValueError, PreludeRefused) as exc:
            sys.stderr.write("gen-pe64-manifest.py: error: --tu-preludes %s: %s\n"
                             % (args.tu_preludes, exc))
            return 1
        for line in report:
            print(line)
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
        "language":         "c",
        "artifactProfile":  "cli",
        "targets":          [args.target],
        "artifactName":     args.artifact_name,
        "sources":          sources,
        "includes":         includes,
        "defines":          defines,
        # Declared import names: a bare PATH stays a plain string (every
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
    missing = [s for s in sources if not os.path.isfile(s)]
    print("sources=%d includes=%d defines=%d resolveLibraries=%d stackReserve=%s missing=%d" %
          (len(sources), len(includes), len(defines),
           len(manifest["resolveLibraries"]),
           manifest.get("stackReserve", "<format default>"), len(missing)))
    for m in missing[:10]:
        print("  MISSING SOURCE: " + m, file=sys.stderr)
    return 1 if missing else 0


# ── the self-test: this file's own arms, each over a scratch tree ──────────────────────

EXPECTED_ARMS = 10


def self_test():
    """The prelude application, its control and its refusals, each through `main` exactly as the
    driver calls it. Prints one line per arm and `passed=N failed=N skipped=0`; exit 0 only when
    every arm passed and every declared arm ran."""
    passed = failed = ran = 0

    def arm(label, ok, detail=""):
        nonlocal passed, failed, ran
        ran += 1
        if ok:
            passed += 1
            print("  [PASS] %s" % label)
        else:
            failed += 1
            print("  [FAIL] %s" % label)
            for ln in str(detail).splitlines():
                print("         %s" % ln)

    root = tempfile.mkdtemp(prefix="gen-manifest-st-")
    try:
        tree = os.path.join(root, "stage", "sqlite")
        real = {}
        for rel in ("src/a.c", "ext/misc/fileio.c", "src/b.c", "dup/ext/misc/fileio.c"):
            path = os.path.join(tree, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("int x_%d;\n" % len(real))
            real[rel] = path.replace("\\", "/")
        recipe = [real["src/a.c"], real["ext/misc/fileio.c"], real["src/b.c"]]

        def write(name, obj):
            path = os.path.join(root, name)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(obj if isinstance(obj, str) else json.dumps(obj))
            return path
        tus = write("tus.txt", "\n".join(recipe) + "\n")
        incs = write("incs.txt", os.path.join(tree, "src") + "\n")
        defs = write("defs.txt", "NDEBUG\n")
        lines = ["#include <dirent.h>", "#ifndef S_ISLNK", "# define S_ISLNK(mode) (0)", "#endif"]
        entry = {"$comment": "why", "tu": "ext/misc/fileio.c", "artifacts": ["testfixture"],
                 "lines": lines}

        def gen(tag, artifact="testfixture", preludes=None, tus_file=tus):
            out = os.path.join(root, "out-%s" % tag, "%s.json" % artifact)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            argv = ["--tus", tus_file, "--includes", incs, "--defines", defs, "--target",
                    "x86_64:pe64-x86_64-windows-exec", "--artifact-name", artifact,
                    "--recipe-transform", "none", "--stack-reserve", "0", "--output", out]
            if preludes is not None:
                argv[-2:-2] = ["--tu-preludes", write("preludes-%s.json" % tag, preludes)]
            so, se = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(so), contextlib.redirect_stderr(se):
                rc = main(argv)
            sources = []
            if rc == 0:
                with open(out, encoding="utf-8") as f:
                    sources = json.load(f)["sources"]
            return rc, sources, so.getvalue(), se.getvalue(), out

        rc, src, so, se, out = gen("apply", preludes=[entry])
        wrapper = src[1] if len(src) == 3 else ""
        wrapped = ""
        if wrapper and os.path.isfile(wrapper):
            with open(wrapper, encoding="utf-8") as f:
                wrapped = f.read()
        base = os.path.join(os.path.dirname(out), TU_PRELUDE_DIR).replace("\\", "/")
        arm("t01 a declared TU is compiled through a wrapper under <manifest dir>/tu-preludes/<digest>/, "
            "keeping its own relative path as the wrapper's tail",
            rc == 0 and re.fullmatch(re.escape(base) + r"/[0-9a-f]{16}/ext/misc/fileio\.c", wrapper) is not None,
            "rc=%d sources=%r stderr=%r" % (rc, src, se))
        body = wrapped.splitlines()
        arm("t02 ...the wrapper holds the declared lines IN ORDER, then #include of the real TU, and "
            "nothing after it",
            body[-len(lines) - 1:-1] == lines and body[-1] == '#include "%s"' % real["ext/misc/fileio.c"],
            wrapped)
        arm("t03 ...every OTHER source is untouched and keeps its place, and the generation says what "
            "it applied", src[0] == recipe[0] and src[2] == recipe[2]
            and "tu-prelude: <ext/misc/fileio.c> compiled after 4 declared line(s)" in so, "%r\n%s" % (src, so))
        rc, src, so, se, _o = gen("control")
        arm("t04 CONTROL: with no --tu-preludes the sources are the recipe, verbatim", rc == 0 and src == recipe,
            "rc=%d %r" % (rc, src))
        rc, src, so, se, _o = gen("other", artifact="sqlite3", preludes=[entry])
        arm("t05 an entry declared for ANOTHER artifact is not applied, and the generation says so",
            rc == 0 and src == recipe and "not for sqlite3 -- not applied" in so, "rc=%d %r\n%s" % (rc, src, so))
        rc, src, so, se, _o = gen("absent", preludes=[dict(entry, tu="ext/misc/zipfile.c")])
        arm("t06 a declared TU the recipe does not name is REFUSED (a stale declaration), naming it",
            rc == 1 and "<ext/misc/zipfile.c>" in se and "0 time(s)" in se, "rc=%d %s" % (rc, se))
        twice = write("tus-twice.txt", "\n".join(recipe + [real["dup/ext/misc/fileio.c"]]) + "\n")
        rc, src, so, se, _o = gen("twice", preludes=[entry], tus_file=twice)
        arm("t07 a declared TU the recipe names TWICE is REFUSED, never applied to the first",
            rc == 1 and "2 time(s)" in se, "rc=%d %s" % (rc, se))
        bad = [[entry, dict(entry)], [dict(entry, lines=[])], [dict(entry, lines=["#x\n#y"])],
               [dict(entry, tu="/abs/fileio.c")], [dict(entry, tu="../fileio.c")],
               [dict(entry, artifacts=[])], [dict(entry, extra=1)], [],
               [dict(entry, lines=["int x;"])], [dict(entry, lines=['#include "windirent.h"'])]]
        refused = [gen("bad%d" % i, preludes=b)[0] == 1 for i, b in enumerate(bad)]
        arm("t08 a malformed declaration is REFUSED: a TU declared twice, empty lines, a multi-line line, "
            "an absolute path, a '..' path, no artifacts, an unknown key, an empty list, a line of CODE "
            "(not a directive), a QUOTED include (it would search the wrapper's directory)",
            all(refused), "refused: %r" % refused)
        rc, src2, so, se, _o = gen("changed", preludes=[dict(entry, lines=lines + ["#define X 1"])])

        def digest_of(path):
            return os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(path))))
        arm("t09 other lines -> ANOTHER wrapper directory: a wrapper written for different lines can never "
            "be the one a manifest names", rc == 0 and len(src2) == 3 and bool(wrapper)
            and digest_of(src2[1]) != digest_of(wrapper), "%r vs %r" % (src2[1:2], wrapper))
        # t10: a wrapper maps back to the real TU it compiles; anything else is None or refused.
        forged = os.path.join(root, "forged", TU_PRELUDE_DIR, "0123456789abcdef", "ext", "misc", "fileio.c")
        os.makedirs(os.path.dirname(forged), exist_ok=True)
        with open(forged, "w", encoding="utf-8", newline="\n") as f:
            f.write('#include "%s"\n' % real["ext/misc/fileio.c"])
        tampered = ""
        if wrapper and os.path.isfile(wrapper):
            tampered = os.path.join(root, "tampered", *wrapper.replace("\\", "/").split("/")[-5:])
            os.makedirs(os.path.dirname(tampered), exist_ok=True)
            with open(tampered, "w", encoding="utf-8", newline="\n") as f:
                f.write(wrapped.replace('#include "', '#include "/elsewhere', 1))
        # Its digest IS its content's, but the TU it includes is not the one its path names.
        other = '#include "%s"\n' % real["src/a.c"]
        mislabelled = os.path.join(root, "mislabelled", TU_PRELUDE_DIR,
                                   hashlib.sha256(other.encode("utf-8")).hexdigest()[:16],
                                   "ext", "misc", "fileio.c")
        os.makedirs(os.path.dirname(mislabelled), exist_ok=True)
        with open(mislabelled, "w", encoding="utf-8", newline="\n") as f:
            f.write(other)

        def refused_target(p):
            try:
                prelude_wrapper_target(p)
            except PreludeRefused:
                return True
            return False
        arm("t10 prelude_wrapper_target maps a generated wrapper back to the real TU it #includes; a "
            "source that is no wrapper answers None; a wrapper-shaped file this generator did not write "
            "(content not its digest's, its include edited, a TU other than its path names, or no file) "
            "is REFUSED",
            bool(wrapper) and prelude_wrapper_target(wrapper) == real["ext/misc/fileio.c"]
            and prelude_wrapper_target(real["src/a.c"]) is None
            and refused_target(forged) and bool(tampered) and refused_target(tampered)
            and refused_target(mislabelled)
            and refused_target(os.path.join(root, "absent", TU_PRELUDE_DIR, "0123456789abcdef", "x.c")),
            "wrapper=%r -> %r" % (wrapper, prelude_wrapper_target(wrapper) if wrapper else None))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    if ran != EXPECTED_ARMS:
        failed += 1
        print("  [FAIL] ran %d arm(s), but EXPECTED_ARMS declares %d" % (ran, EXPECTED_ARMS))
    print("\npassed=%d failed=%d skipped=0" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
