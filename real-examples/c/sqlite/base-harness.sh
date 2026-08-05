#!/usr/bin/env bash
# real-examples/c/sqlite/base-harness.sh
# ─────────────────────────────────────────────────────────────────────────────
# THE SHARED POSIX CORE OF THE SQLITE HARNESSES.
#
# ★ WHY THIS FILE EXISTS. This harness builds MORE THAN ONE ARTIFACT out of one
# staged sqlite tree (the TCL `testfixture` and the `sqlite3` CLI), from TWO
# drivers (build-and-test.sh and build-and-test.ps1), for FIVE legs. Before this
# file, the recipe derivation, the artifact read-back and the manifest call were
# written out once per DRIVER — and the two copies had already drifted in three
# measured ways:
#
#   · build-and-test.ps1:1343 carried the UNFIXED BSD-sed continuation join
#     (`sed ':a;N;$!ba;…'`), which BSD/macOS sed reads as one enormous LABEL and
#     dies on — silently emitting only the recipe's first line
#     (D-HARNESS-SELFTEST-BSD-SED-PORTABILITY). build-and-test.sh:1759 had the
#     portable one-`-e`-per-label form. One driver was portable; its twin was not.
#   · build-and-test.sh:1794 iterated `for f in "${!TU[@]}"` — BASH HASH ORDER —
#     to decide which of two same-basename paths survives dedup, while
#     build-and-test.ps1:1368 already sorted first
#     (D-HARNESS-SH-TU-DEDUP-DEPENDS-ON-BASH-HASH-ORDER).
#   · the artifact read-back existed twice, and an earlier third copy of the
#     output-suffix table cost this project a false build FAILURE on a perfectly
#     good cross-built `testfixture.exe`
#     (D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING).
#
# "A capability in one driver and not the other is a silent harness bug" is the
# house rule; three copies of one decision is how that bug gets manufactured. So
# the decision lives HERE, once, and both drivers reach it.
#
# ★★ AND "ONCE" IS A COUNT, SO IT GETS COUNTED. Extracting this file did not on
# its own reduce anything: the FIRST pass added the shared implementation and
# routed only the CLI path through it, leaving build-and-test.sh's private
# `dss_reported_artifact` and build-and-test.ps1's `Get-ReportedArtifact` in
# place for the FIXTURE path — so the copy count went 2 → 4, and the two
# survivors still implemented the "take the LAST match" rule this file argues at
# length is unsafe. That is worse than not extracting: a header claiming one
# implementation while two more sit downstream is how the next reader is misled.
# The count now, and it is checkable by grep (✔MEASURED 2026-08-05):
#   · artifact read-back  — one per language (dss_bh_reported_artifact /
#     Get-DssReportedArtifact), and NEITHER DRIVER CALLS EITHER. The only
#     non-self-test caller of each is its own build wrapper
#     (dss_bh_build_artifact / Invoke-DssBuild) one screen below it, so no driver
#     reads a compile log for an artefact at all.
#   · recipe derivation   — one (dss_bh_emit_recipe), FOUR call sites: the
#     fixture and the CLI, in each driver. Neither driver runs `make -n` itself.
#   · compile-time suffix — one (dss_bh_compile_time_suffix).
# If you add a fifth caller, it calls these. If you find yourself writing a
# sixth copy, that is the defect, not the inconvenience.
#
# ★ WHO SOURCES THIS FILE — and note that BOTH of them are bash:
#   · build-and-test.sh sources it directly.
#   · build-and-test.ps1 does NOT reimplement any of it in PowerShell. Its
#     recipe derivation has ALWAYS run in a POSIX shell (the `$deriveScript`
#     heredoc it hands to WSL), so it sources this same file from that script.
#     That is why base-harness.ps1 carries no recipe-derivation functions and is
#     NOT missing a capability: on the PowerShell side the capability is
#     delivered by THIS file, running in the shell that was always doing the work.
#
# ★ WHAT DELIBERATELY DID **NOT** MOVE HERE. Per TF-C117's rule — logic that can
# be shared goes into the PYTHON, not duplicated into two shells — the manifest
# SCHEMA lives in gen-pe64-manifest.py, the leg catalogue in harness_legs.py, and
# the CLI smoke assertions in cli-smoke.py. This file holds only what is
# irreducibly shell: driving `make -n`, `ar`, and the compiler process.
#
# ★ NO HOST TESTS. Nothing in this file asks what kind of machine it is on.
# Every decision it makes is keyed on its ARGUMENTS — a make target, an archive,
# a target spec. The one host-shaped concept it touches (a path spelling) is
# handled by the CALLER, which is where the host decision already lives.
#
# EVERY FUNCTION HERE IS `dss_bh_*`-PREFIXED and depends on NOTHING the drivers
# define. It cannot call `die`/`warn`/`info`: build-and-test.sh defines those at
# line ~553, and the .ps1's derive script does not define them at all. Reporting
# is plain stdout/stderr, and failure is a NON-ZERO RETURN the caller renders in
# its own vocabulary. A shared core that killed the process would take the
# caller's per-leg verdict with it.
# ─────────────────────────────────────────────────────────────────────────────

# Guard against double-sourcing: build-and-test.ps1's derive script and
# build-and-test.sh can both be in scope in a nested invocation.
[ -n "${DSS_BASE_HARNESS_SH:-}" ] && return 0
DSS_BASE_HARNESS_SH=1

# The version both drivers assert against. Bump it when a function's CONTRACT
# changes so a driver paired with a stale copy fails loud instead of silently
# losing a capability — the exact failure mode this file exists to end.
#
# 2 — the DROP LEDGER (dss_bh_note_drop and the two TU recoverers reporting what
#     they could not resolve), an explicitly-named-but-absent --archive is now a
#     named failure, and dss_bh_emit_recipe returns 1 on a LOST archive member.
#     A driver on version 1 would silently lose TUs; hence the bump.
DSS_BASE_HARNESS_VERSION=2

dss_bh_err() { printf 'base-harness: %s\n' "$*" >&2; }

# ─────────────────────────────────────────────────────────────────────────────
# THE DROP LEDGER — A TU THAT QUIETLY DOES NOT GET COMPILED IS THE WORST OUTCOME
#
# Both TU recoverers below walk a list and keep the entries they can resolve.
# They used to DISCARD the rest with no output at all: `dss_bh_span_tus` let a
# token that matched neither path fall off the end of its `if/elif`, and
# `dss_bh_archive_tus` guarded its emit with `[ -n "$hit" ] &&`. The only
# backstop was the TU FLOOR — `--min-tus 100` against ~103 — so losing three
# sources cleared the floor, built a strictly smaller program, and failed much
# later at link looking like a codegen bug. The floors' own contract (see
# dss_bh_emit_recipe) is "an immediate, named stop"; silence is neither.
#
# Every drop is now NAMED on stderr and RECORDED, and the two classes are kept
# apart because they carry different certainty:
#
#   archive-member  UNAMBIGUOUS LOSS. The `.o` is IN the archive, so the `.c`
#                   that produced it exists somewhere; not finding it means the
#                   search roots are wrong and a real TU is gone. FATAL.
#   recipe-token    A `.c`-suffixed TOKEN in shell text is not necessarily a
#                   translation unit (a redirection written `>gen.c`, a token
#                   inside an `echo`), so this is reported loudly and counted in
#                   the summary rather than assumed fatal — a hard stop here
#                   could red on a recipe that is perfectly fine.
#
# The log is a FILE, not a variable, and that is load-bearing: dss_bh_emit_recipe
# collects the TUs through a PIPELINE, whose left-hand side bash runs in a
# SUBSHELL — a counter incremented in there would be discarded at the `|`.
# ─────────────────────────────────────────────────────────────────────────────
DSS_BH_DROP_LOG=""

# dss_bh_note_drop <class> <message>
# ALWAYS returns 0: this is called from inside `while` loops in files that run
# under `set -e`, and a trailing test that happens to be false must not turn a
# report into a control-flow event.
dss_bh_note_drop() {
  dss_bh_err "DROPPED [$1] $2"
  if [ -n "${DSS_BH_DROP_LOG:-}" ]; then
    printf '%s\t%s\n' "$1" "$2" >> "$DSS_BH_DROP_LOG"
  fi
  return 0
}

# ─────────────────────────────────────────────────────────────────────────────
# RECIPE DERIVATION
#
# `make -n <target>` prints the commands make WOULD run. That output is the only
# honest source for a target's translation units, its -D defines and its -I
# dirs: it is upstream's own answer, re-derived on every run, rather than a list
# this harness maintains by hand against a moving tree.
# ─────────────────────────────────────────────────────────────────────────────

# dss_bh_recipe_blob <recipe-file>
# Join make's backslash-continuations so one logical command is one line.
#
# ★ ONE `-e` PER LABEL — never `sed ':a;N;$!ba;…'`
# (D-HARNESS-SELFTEST-BSD-SED-PORTABILITY). GNU sed lets a `:label` be terminated
# by `;`; BSD/macOS sed does NOT — it swallows the rest of the script as part of
# the LABEL NAME, dies `unused label 'a;N;$!ba;…'`, and emits only the FIRST
# line. The join then SILENTLY did not happen and every downstream extraction
# read a truncated blob. Splitting the script across separate -e arguments makes
# the label end at the argument boundary, which is the portable form: MEASURED
# byte-identical output from BSD sed and GNU sed.
dss_bh_recipe_blob() {
  sed -e ':a' -e 'N;$!ba' -e 's/\\\n/ /g' "$1" | tr '\t' ' '
}

# dss_bh_recipe_defines   (blob on stdin) -> NAME[=VALUE] per line, sorted -u
# make's literal `""` is stripped so `SQLITE_PRIVATE=""` becomes an EMPTY value
# rather than a two-quote-character one, and bare shell quoting is dropped.
dss_bh_recipe_defines() {
  grep -oE '\-D[A-Za-z0-9_]+(=[^ ]*)?' | sed 's/^-D//; s/"//g' | sort -u
}

# dss_bh_recipe_includes  (blob on stdin) -> -I dirs, sorted -u
# The bare `.` is dropped: it is the build dir, which the caller adds explicitly
# under its real name (a relative `.` would resolve against the DRIVER's cwd,
# not make's).
dss_bh_recipe_includes() {
  grep -oE '\-I ?[^ ]+' | sed 's/^-I *//' | grep -v '^\.$' | sort -u
}

# dss_bh_recipe_span <blob> <mode> <make-target>
# The slice of the recipe whose tokens name the target's INPUTS.
#
# ★ THE TWO MODES ARE NOT INTERCHANGEABLE, AND CHOOSING WRONG IS SILENT.
#
#   whole-blob  every command line in the recipe. Correct when the recipe is
#               essentially the ONE link command (the case `make -n testfixture`
#               is run in: the reference build has just built every prerequisite,
#               so make prints only the final link).
#
#   link-line   ONLY the command that writes `-o <make-target>`. Correct when the
#               recipe may still contain the BOOTSTRAP — and for `sqlite3d` it
#               routinely does, because its prerequisites are the 102 `.o` files
#               and make re-prints their compile lines whenever they are stale.
#
# ✔MEASURED 2026-08-05 on the live tree: a whole-blob derive of `make -n sqlite3d`
# absorbs `tool/lemon.c`, `tool/lempar.c` and `tool/mksourceid.c` — BUILD-HOST
# TOOLS that must never be cross-compiled into a target artifact, and `lempar.c`
# is not even standalone C (it is lemon's parser TEMPLATE). Link-line mode yields
# exactly `shell.c` + 102 `.o` tokens, which is upstream's own declared
# prerequisite list (`main.mk:2185`: `sqlite3d$(T.exe): shell.c $(LIBOBJS0)`).
#
# The `\( \|$\)` is load-bearing: without it `-o sqlite3` would also match the
# `-o sqlite3d` line, and a target that is a PREFIX of another would silently
# harvest its sibling's inputs.
dss_bh_recipe_span() {
  local blob="$1" mode="$2" target="$3"
  case "$mode" in
    whole-blob) printf '%s\n' "$blob" ;;
    link-line)  printf '%s\n' "$blob" | grep -- "-o $target\( \|\$\)" ;;
    *)          dss_bh_err "dss_bh_recipe_span: unknown mode '$mode' (want whole-blob|link-line)"; return 2 ;;
  esac
}

# dss_bh_recipe_token_span <blob> <target>
# The lines whose -D / -I tokens describe OUR PROGRAM: every COMPILE line
# (`-c`) plus the LINK line for <target>. This is the span the defines and the
# include dirs must be read from — NOT the whole blob.
#
# ★ WHY THIS IS NOT THE SAME QUESTION AS "WHICH TUs". Two measured facts force it
# apart, and getting either wrong is silent:
#
#  1. UPSTREAM COMPILES THE LIBRARY AND shell.c WITH DIFFERENT -D SETS.
#     ✔MEASURED 2026-08-05 on `sqlite3d`: the 102 library objects are built with
#     8 defines INCLUDING `SQLITE_CORE`, while `shell.c` is compiled as part of
#     the LINK command with 18 that do NOT include it (main.mk:2160-2166 explains
#     the split — the shell wants library features the canonical library build
#     does not enable). DSS builds ONE program from ONE `defines` array, so the
#     answer has to be the UNION of the two, which is what this span yields.
#     Reading the link line alone drops `SQLITE_CORE`, and that is not cosmetic:
#     ext/icu/icu.c:31-33 is `#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_ICU) …`,
#     so without it a file that should compile to NOTHING instead demands
#     <unicode/utypes.h> and the whole CLI fails with four `error[F001A]`.
#
#  2. THE BOOTSTRAP CARRIES FOREIGN -D TOKENS. `make -B -n` also prints the jimsh
#     and lemon builds, and ✔MEASURED those contribute `JIM_COMPAT`,
#     `HAVE_REALPATH`, `HAVE__FULLPATH` and `_FILE_OFFSET_BITS=64` — configuration
#     for a BUILD-HOST tool, not for our target. A whole-blob read absorbs them.
#
# `-c` is matched with surrounding spaces so it cannot hit a path containing "-c".
dss_bh_recipe_token_span() {
  local blob="$1" target="$2"
  printf '%s\n' "$blob" | grep -e ' -c ' -e "-o $target\( \|\$\)"
}

# dss_bh_span_tus <span> <build-dir>   -> one existing .c path per line
# Every `.c` token the span names directly. Most are ABSOLUTE ($(TOP)/…); the few
# make names RELATIVELY (ctime.c / fts5.c / parse.c / tclsqlite-ex.c / shell.c)
# live in the BUILD DIR, which is make -n's cwd — so an unrooted token is
# resolved against it.
#
# ★ THE BUILD-DIR FALLBACK IS ORDERED SECOND ON PURPOSE and the order is not
# cosmetic: this function runs at the DRIVER's cwd, which is NOT the build dir.
# Testing `-f "$c"` first would, in a shell that happened to be sitting in the
# build dir, record the BARE relative spelling — a path that stops resolving the
# moment anything changes directory. ✔MEASURED 2026-08-05: running this loop with
# cwd=$BLD produced 196 raw paths containing four bare/absolute duplicate pairs;
# with the driver's real cwd it produces 192 with ZERO duplicates.
#
# ★ AND A TOKEN THAT RESOLVES TO NEITHER IS REPORTED, NEVER DROPPED IN SILENCE
# — see THE DROP LEDGER above for why the `else` arm exists and why it is a loud
# report rather than a hard stop.
dss_bh_span_tus() {
  local span="$1" bld="$2" c
  while IFS= read -r c; do
    [ -n "$c" ] || continue
    if   [ -f "$c" ];      then printf '%s\n' "$c"
    elif [ -f "$bld/$c" ]; then printf '%s\n' "$bld/$c"
    else dss_bh_note_drop recipe-token \
      "the recipe span names '$c' as a .c input and it resolves to NO file (tried '$c' and '$bld/$c') — it is NOT in the TU set"
    fi
  done < <(printf '%s\n' "$span" | tr ' ' '\n' | grep -E '\.c$' | sort -u)
}

# dss_bh_archive_tus <archive> <object-filter> <search-root>…
#   -> one existing .c path per line, recovered from the archive's members
#
# DSS cannot consume a gcc `.a`, so the CORE sources compiled into libsqlite3.a
# have to be recovered as SOURCE. `ar t` names the members; each `<base>.o` is
# mapped back to the `<base>.c` that produced it by searching the roots.
#
# `<object-filter>` is a newline-separated list of `.o` basenames to keep, or
# EMPTY for "every member". The filter is what lets a caller recover exactly the
# object list a specific link line named, rather than assuming the archive and
# the link agree — they need not, and an archive is a superset far more often
# than it is an exact match.
#
# The `/tsrc/` exclusion is a FIRST-CHOICE preference, not a ban: sqlite's
# amalgamation staging tree duplicates generated sources under `bld/tsrc/`, and
# `bld/` is the copy make actually compiles. If a member exists ONLY under tsrc/
# the second search finds it rather than dropping the TU silently.
#
# ★ A MEMBER WHOSE SOURCE IS NOT FOUND IS AN UNAMBIGUOUS LOST TU and is reported
# as one (THE DROP LEDGER above): the object is IN the archive, so the `.c` that
# produced it exists — not finding it means the SEARCH ROOTS are wrong.
# dss_bh_emit_recipe turns that report into a hard stop.
dss_bh_archive_tus() {
  local ar="$1" filter="$2"; shift 2
  [ -f "$ar" ] || return 0
  local obj base hit
  while read -r obj; do
    [ -n "$obj" ] || continue
    if [ -n "$filter" ]; then
      printf '%s\n' "$filter" | grep -qxF "$obj" || continue
    fi
    base="${obj%.o}"
    hit="$(find "$@" -name "$base.c" 2>/dev/null | grep -v '/tsrc/' | head -1)"
    [ -z "$hit" ] && hit="$(find "$@" -name "$base.c" 2>/dev/null | head -1)"
    if [ -n "$hit" ]; then printf '%s\n' "$hit"
    else dss_bh_note_drop archive-member \
      "archive '$ar' has member '$obj' but NO '$base.c' exists under any search root ($*) — the TU it was compiled from is LOST from the source set"
    fi
  done < <(ar t "$ar" 2>/dev/null | grep '\.o$')
}

# dss_bh_dedup_by_basename   (paths on stdin) -> deduped paths on stdout, sorted
#
# ★ D-HARNESS-SH-TU-DEDUP-DEPENDS-ON-BASH-HASH-ORDER — THIS IS THE FIX.
# The generated sources exist under two paths (bld/x.c and bld/tsrc/x.c); the
# same FILE must reach the compiler once. build-and-test.sh decided which path
# won by iterating `"${!TU[@]}"`, i.e. BASH'S INTERNAL HASH ORDER over the key
# set — deterministic for a fixed set of keys, and therefore invisible, but free
# to reorder the moment the key set changes. Its documented trigger is literally
# "a SECOND artifact's TUs enter the same map", which is what building the CLI
# alongside the fixture does.
#
# Sorting FIRST makes the surviving path a property of the path set alone.
# ✔MEASURED 2026-08-05, before adopting it: on the live fixture recipe the sorted
# and hash-ordered results are BYTE-IDENTICAL (192 TUs, zero duplicate
# basenames), so this closes the latent defect without moving today's TU set —
# which is the only reason it was safe to change under a green leg.
#
# `${f##*/}` rather than `$(basename "$f")`: one fork per TU across ~200 TUs and
# two artifacts is real time, and the parameter expansion is exact for the
# absolute paths this only ever receives.
dss_bh_dedup_by_basename() {
  local f b
  declare -A _seen=()
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    b="${f##*/}"
    [ -n "${_seen[$b]:-}" ] && continue
    _seen["$b"]="$f"
    printf '%s\n' "$f"
  done < <(sort -u)
}

# dss_bh_emit_recipe — derive ONE make target's recipe into three files.
#
#   dss_bh_emit_recipe --build-dir D --make-target T --recipe-file F
#                      [--make-var NAME=VALUE]…   (repeatable)
#                      [--prereq-mode whole-blob|link-line]   (default whole-blob)
#                      [--archive A] [--archive-from-span 0|1]
#                      [--search-root R]…         (repeatable)
#                      [--min-tus N] [--min-defines N]
#                      --out-tus F --out-defines F --out-includes F
#
# Prints a one-line summary on stdout. Returns non-zero — with a diagnostic
# naming the recipe file — when a floor is not met.
#
# ★ THE FLOORS ARE THE POINT, not decoration. A recipe parse that breaks does
# not error: it yields a SHORT list, and a short list compiles and links into a
# smaller program that fails much later in a way that looks like a codegen bug.
# The floors turn that into an immediate, named stop. They are per-target
# because the two targets have honestly different sizes.
dss_bh_emit_recipe() {
  local bld="" target="" recipe="" mode="whole-blob" archive="" arch_from_span=0
  local min_tus=0 min_defs=0 out_tus="" out_defs="" out_incs=""
  local always_make=0 token_scope="all"
  local -a make_vars=() roots=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --build-dir)   bld="$2";        shift 2 ;;
      --make-target) target="$2";     shift 2 ;;
      --recipe-file) recipe="$2";     shift 2 ;;
      --make-var)    make_vars+=("$2"); shift 2 ;;
      --prereq-mode) mode="$2";       shift 2 ;;
      --always-make) always_make="$2"; shift 2 ;;
      --token-scope) token_scope="$2"; shift 2 ;;
      --archive)     archive="$2";    shift 2 ;;
      --archive-from-span) arch_from_span="$2"; shift 2 ;;
      --search-root) roots+=("$2");   shift 2 ;;
      --min-tus)     min_tus="$2";    shift 2 ;;
      --min-defines) min_defs="$2";   shift 2 ;;
      --out-tus)     out_tus="$2";    shift 2 ;;
      --out-defines) out_defs="$2";   shift 2 ;;
      --out-includes) out_incs="$2";  shift 2 ;;
      *) dss_bh_err "dss_bh_emit_recipe: unknown argument '$1'"; return 2 ;;
    esac
  done
  for _r in bld target recipe out_tus out_defs out_incs; do
    [ -n "${!_r}" ] || { dss_bh_err "dss_bh_emit_recipe: --${_r//_/-} is required"; return 2; }
  done

  # ★ `--always-make 1` ADDS `-B`, AND IT IS WHAT MAKES THIS DETERMINISTIC.
  # `make -n` prints only what it WOULD DO, so its output depends on which
  # objects happen to be up to date — a recipe that is 239 lines on a cold tree
  # and ONE line on a warm one. ✔MEASURED 2026-08-05: with the 102 objects
  # current, `make -n sqlite3d` printed only the link line, whose -D set omits
  # `SQLITE_CORE`, and the CLI failed with four `error[F001A] got unicode/*.h`
  # — a build that succeeded or failed depending on the state of a build
  # directory nobody thought of as an input. `-B` (--always-make) makes make
  # print every command as if nothing were current. It is STILL A DRY RUN: `-n`
  # is in force, so nothing is compiled and no object is touched.
  local -a make_flags=(-n)
  [ "$always_make" = 1 ] && make_flags+=(-B)
  # rc is DELIBERATELY not checked here. `make -n` legitimately returns non-zero
  # on a tree whose bootstrap it would have had to run, and the recipe text it
  # printed on the way is still exactly what we came for. What must never be
  # tolerated is a SHORT parse, and that is what the floors below catch — an
  # honest gate on the OUTPUT rather than a proxy gate on the exit code.
  ( cd "$bld" && make "${make_flags[@]}" "$target" "${make_vars[@]}" ) > "$recipe" 2>&1 || true

  local blob span tokens
  blob="$(dss_bh_recipe_blob "$recipe")"
  span="$(dss_bh_recipe_span "$blob" "$mode" "$target")" || return 2

  # WHERE THE -D / -I TOKENS COME FROM — see dss_bh_recipe_token_span for the two
  # measurements that force this to be a separate question from "which TUs".
  #   all     the whole blob. Correct when the recipe IS essentially one command
  #           (the fixture), and the behaviour every existing caller had.
  #   recipe  the compile lines UNION the link line — required with `-B`, whose
  #           output also contains the jimsh/lemon bootstrap and its foreign -D.
  case "$token_scope" in
    all)    tokens="$blob" ;;
    recipe) tokens="$(dss_bh_recipe_token_span "$blob" "$target")" ;;
    *)      dss_bh_err "dss_bh_emit_recipe: unknown --token-scope '$token_scope' (want all|recipe)"; return 2 ;;
  esac
  printf '%s\n' "$tokens" | dss_bh_recipe_defines  > "$out_defs"
  printf '%s\n' "$tokens" | dss_bh_recipe_includes > "$out_incs"

  # ★ AN ARCHIVE THE CALLER NAMED AND THAT IS NOT THERE IS A NAMED FAILURE, not a
  # quiet zero. `dss_bh_archive_tus` returns 0 for an absent archive because
  # "this caller has no archive" is expressed by passing none; a caller that DID
  # pass one is asserting the recovery matters, and for `sqlite3d` it is 102 of
  # the 103 TUs. The floor would eventually notice, but it would blame "the
  # recipe parse broke" for a missing file — a diagnostic pointing at the wrong
  # thing costs a triage cycle.
  if [ -n "$archive" ] && [ ! -f "$archive" ]; then
    dss_bh_err "recipe derivation for '$target' was told to recover TUs from the archive '$archive', which does NOT exist."
    dss_bh_err "Nothing was silently substituted for it: the core sources compiled into that archive are the bulk of this target."
    return 1
  fi

  # The object filter: in link-line mode, recover ONLY the `.o` the link named.
  local filter=""
  if [ "$arch_from_span" = 1 ]; then
    filter="$(printf '%s\n' "$span" | tr ' ' '\n' | grep -E '\.o$' | sed 's#.*/##' | sort -u)"
  fi
  # THE DROP LEDGER, collected through a FILE — the pipeline below runs its left
  # side in a subshell, so a shell variable would be discarded at the `|`. It
  # lives beside the recipe file the diagnostics already name, so a reader can
  # open it.
  DSS_BH_DROP_LOG="$recipe.drops"
  : > "$DSS_BH_DROP_LOG"
  {
    dss_bh_span_tus "$span" "$bld"
    [ -n "$archive" ] && dss_bh_archive_tus "$archive" "$filter" "${roots[@]}"
  } | dss_bh_dedup_by_basename | sort > "$out_tus"
  local drop_log="$DSS_BH_DROP_LOG"
  DSS_BH_DROP_LOG=""

  local n_tus n_defs n_incs n_lost n_unresolved
  n_tus="$(grep -c . "$out_tus"  || true)"
  n_defs="$(grep -c . "$out_defs" || true)"
  n_incs="$(grep -c . "$out_incs" || true)"
  n_lost="$(grep -c '^archive-member' "$drop_log" || true)"
  n_unresolved="$(grep -c '^recipe-token' "$drop_log" || true)"
  # ★ ORDERED BEFORE THE FLOORS ON PURPOSE. A lost archive member is a NAMED
  # cause; the floor is a symptom that may or may not fire depending on how many
  # were lost. Reporting the cause first is what makes this "an immediate, named
  # stop" rather than a count that has to be interpreted.
  if [ "$n_lost" -gt 0 ]; then
    dss_bh_err "recipe derivation for '$target' LOST $n_lost archive member(s): the object is in '$archive' but no matching .c exists under any --search-root."
    dss_bh_err "Each one is a translation unit that would silently NOT be compiled, producing a smaller program that fails much later at link. See $drop_log"
    return 1
  fi
  if [ "$n_tus" -lt "$min_tus" ]; then
    dss_bh_err "recipe derivation for '$target' yielded only $n_tus TUs (<$min_tus) — the recipe parse broke; see $recipe"
    return 1
  fi
  if [ "$n_defs" -lt "$min_defs" ]; then
    dss_bh_err "recipe derivation for '$target' yielded only $n_defs defines (<$min_defs) — the recipe parse broke; see $recipe"
    return 1
  fi
  # The unresolved-token count RIDES ON THE SUMMARY, pass or fail. A recipe that
  # started naming sources this derivation cannot find must not need someone to
  # go looking through stderr to discover it. Built in a VARIABLE rather than in
  # a `$( … && … )` inside printf's arguments: the callers run under `set -Eeuo
  # pipefail` WITH AN ERR TRAP, and a false test inside a command substitution
  # is exactly the shape that trips one.
  local note=""
  if [ "$n_unresolved" -gt 0 ]; then
    note=" — ★ $n_unresolved .c token(s) in the recipe resolved to NO file (each named on stderr; see $drop_log)"
  fi
  printf '%s: %s TUs, %s defines, %s -I dirs (mode %s)%s\n' "$target" "$n_tus" "$n_defs" "$n_incs" "$mode" "$note"
}

# ─────────────────────────────────────────────────────────────────────────────
# WHAT DID THE COMPILER ACTUALLY WRITE? ASK IT, DO NOT GUESS.
# ★ ANCHOR, ONE LINE, DO NOT WRAP: D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING
#
# The drivers used to assemble the artifact's name themselves. DSS names a PE
# executable `<name>.exe`, so a POSIX host cross-building the pe64 leg produced a
# real 5,387,264-byte `PE32+ executable` with ZERO `error[` in the log — and the
# leg was recorded as a build FAILURE and marked POISONED, a false negative on
# this project's headline capability manufactured by three copies of one suffix
# table. `TargetSpec::outputExtension` (src/program/target_spec.cpp) owns that
# table, keyed on the CLOSED object-format enum, and src/program/program.cpp:216
# now reports every artifact it commits:
#
#     dss-code-prime: artifact <targetSpec> <absolute path>
#
# A target spec cannot contain whitespace (DSS refuses one that does), so the
# path is the whole REMAINDER of the line and an output dir containing a space
# survives intact.
# ─────────────────────────────────────────────────────────────────────────────

# dss_bh_reported_artifacts <log> <spec> -> every DISTINCT path reported, one per line
dss_bh_reported_artifacts() {
  local log="$1" spec="$2" hits
  # rc DIRECTLY off grep, never after a pipe. `-F`: a target spec is a LITERAL
  # and carries `+`/`.` characters a regex would reinterpret.
  hits="$(grep -F "dss-code-prime: artifact $spec " "$log" 2>/dev/null)" || return 1
  [ -n "$hits" ] || return 1
  printf '%s\n' "$hits" | sed "s|^dss-code-prime: artifact $spec ||" | awk '!seen[$0]++'
}

# dss_bh_reported_artifact <log> <spec> -> THE path this build produced for <spec>
#
# ★ THE SEAM THIS CLOSES, AND WHY "LAST WINS" WAS NOT SAFE ANY MORE.
# The old rule was "select by (log, spec), take the LAST match", written when a
# re-run could append to a log and an EARLIER build's artifact must not be
# resurrected. That rule silently assumes one artifact per (log, spec) — true
# while the harness built exactly one thing per leg, and FALSE the moment a
# second `--project` invocation for the same target lands in the same log, which
# is precisely what adding the CLI does. "Last wins" would then hand a caller its
# SIBLING's binary with no diagnostic at all.
#
# THE FIX HAS TWO HALVES, and it needs both:
#   1. STRUCTURAL — the callers give each artifact its OWN compile log and its
#      OWN `--output` directory, so the ambiguity cannot arise. That is the real
#      answer; this function only has to notice if it ever does.
#   2. FAIL-LOUD HERE — two DIFFERENT paths for one spec in one log is now an
#      ERROR, not an arbitrary pick. The legitimate re-run case (the same path
#      reported again) still passes, because the comparison is over DISTINCT
#      paths: a repeated identical line collapses to one and "last wins" and
#      "only one" agree.
# A silent wrong-binary hand-back is the single worst outcome available here —
# every downstream verdict would be about a file nobody meant to test.
#
# ★ ABSENCE IS A REAL ANSWER, NOT AN ERROR TO PAPER OVER. A build that wrote
# nothing reports nothing; rc 1 is what makes the caller's "0 error[ but no
# artefact" branch fire on exactly that case.
dss_bh_reported_artifact() {
  local log="$1" spec="$2" all n
  all="$(dss_bh_reported_artifacts "$log" "$spec")" || return 1
  n="$(printf '%s\n' "$all" | grep -c .)"
  if [ "$n" -gt 1 ]; then
    dss_bh_err "the build log '$log' reports $n DIFFERENT artifacts for the target spec '$spec':"
    printf '%s\n' "$all" | sed 's/^/  base-harness:     /' >&2
    dss_bh_err "refusing to guess which one the caller meant. Each artifact must be built"
    dss_bh_err "into its OWN --output directory with its OWN compile log; see"
    dss_bh_err "D-HARNESS-FIXTURE-PATH-ASSUMES-THE-POSIX-ARTIFACT-SPELLING."
    return 2
  fi
  printf '%s\n' "$all"
}

# ─────────────────────────────────────────────────────────────────────────────
# BUILD ONE ARTIFACT
# ─────────────────────────────────────────────────────────────────────────────

# dss_bh_generate_manifest <gen.py> <out-manifest> <artifact-name> <target-spec>
#                          <tus-file> <includes-file> <defines-file>
#                          <recipe-transform> <stack-reserve> [<library argv>…]
#
# A thin, ORDERED wrapper over the ONE manifest generator both drivers already
# share (gen-pe64-manifest.py). It is here so the ARGUMENT ORDER and the set of
# things a caller must remember to pass is itself shared — the generator having
# one implementation does not help if two callers disagree about which flags
# matter. Everything leg-specific arrives as an argument; nothing is read from
# the environment.
#
# The library argv is passed through as TOKENS, never re-spelled: a resolved
# library may carry a declared runtime identity (`<path>=<import-name>`) and this
# file must not know that vocabulary (D-FFI-DECLARED-IMPORT-NAME).
dss_bh_generate_manifest() {
  local gen="$1" out="$2" name="$3" spec="$4" tus="$5" incs="$6" defs="$7"
  local transform="$8" reserve="$9"; shift 9
  python3 "$gen" \
    --tus       "$tus" \
    --includes  "$incs" \
    --defines   "$defs" \
    --target    "$spec" \
    "$@" \
    --artifact-name    "$name" \
    --recipe-transform "$transform" \
    --stack-reserve    "$reserve" \
    --output    "$out"
}

# dss_bh_build_artifact <dss-bin> <manifest> <config> <output-dir> <log> <spec>
#   -> the artifact path on stdout
#      rc 0 built · 1 no artifact reported · 2 ambiguous · 3 diagnostics
#         4 an artifact was REPORTED and is not on disk
#
# dss-code-prime RETURNS EXIT 0 EVEN ON FATAL ERRORS, so the verdict is taken
# from `error[` in the log PLUS the artifact the build itself reported — never
# from the process exit status. The caller renders the verdict; this returns the
# facts. The four failure codes are kept apart because they have four different
# remedies: diagnostics were emitted (a source/config problem) / the compiler
# said nothing and claimed nothing (a compiler defect) / it claimed two things
# (a harness structuring defect) / it claimed a file that is not there.
#
# ★ rc 4 EXISTS TO PAIR WITH Invoke-DssBuild. The PowerShell twin has always
# ended with `Test-Path` on the reported path, and this function did not — so
# "the build claimed an artefact that is not on disk" was a named verdict in one
# driver and an unnoticed success in the other, which is the exact silent-harness
# shape this pair was extracted to end. The EXISTENCE question is asked here
# because it is target-agnostic; whether the file is EXECUTABLE is not (a
# staticlib leg's artifact is not), so that stays with the caller that intends
# to exec it.
# ★ `|| return $?`, NEVER `cmd; rc=$?`. This file declares itself a shared
# library, so it may not depend on how a caller happens to invoke it. The
# previous shape — `bin="$(…)"; rc=$?` — is only safe when errexit is suppressed
# AT THE CALL SITE, because a command substitution assignment that returns
# non-zero trips `set -e` BEFORE the next line reads `$?`. It was safe by
# accident: its one caller (build-and-test.sh's CLI loop) happens to invoke it
# under `|| _rc=$?`. That is an undocumented invariant a second caller would
# break by writing perfectly ordinary code, and the failure would be a whole-run
# abort instead of one leg's verdict. The `|| return $?` list form suppresses
# errexit for the assignment and propagates the status in the same breath.
dss_bh_build_artifact() {
  local dss="$1" manifest="$2" config="$3" outdir="$4" log="$5" spec="$6" bin
  "$dss" --project "$manifest" --config="$config" --output "$outdir" --time >"$log" 2>&1 || true
  if grep -qE 'error\[' "$log"; then return 3; fi
  bin="$(dss_bh_reported_artifact "$log" "$spec")" || return $?
  [ -n "$bin" ] || return 1
  [ -e "$bin" ] || { printf '%s\n' "$bin"; return 4; }
  printf '%s\n' "$bin"
}

# dss_bh_compile_time_suffix <log> — "  (1m2.3s)" or empty.
dss_bh_compile_time_suffix() {
  local t
  t="$(grep -oE 'compile time [^[:space:]]+' "$1" 2>/dev/null | tail -1)" || true
  [ -n "$t" ] && printf '  (%s)' "$t" || true
}

# ─────────────────────────────────────────────────────────────────────────────
# ARTIFACT VERDICT LEDGER
#
# One artifact on one leg gets ONE verdict, and Step 9 must be able to prove that
# every (leg, artifact) pair it declared reached one. "Silence about a unit is a
# harness bug" applies per ARTIFACT now that there is more than one, and a ledger
# keyed only by leg cannot express "the fixture built and the CLI did not".
# ─────────────────────────────────────────────────────────────────────────────
declare -A DSS_BH_VERDICT=() DSS_BH_DETAIL=()

dss_bh_set_verdict() {          # dss_bh_set_verdict <leg> <artifact> <verdict> <detail>
  DSS_BH_VERDICT["$1/$2"]="$3"
  DSS_BH_DETAIL["$1/$2"]="$4"
}
dss_bh_get_verdict() {          # dss_bh_get_verdict <leg> <artifact>
  printf '%s' "${DSS_BH_VERDICT["$1/$2"]:-}"
}
dss_bh_get_detail() {           # dss_bh_get_detail <leg> <artifact>
  printf '%s' "${DSS_BH_DETAIL["$1/$2"]:-}"
}
# dss_bh_assert_verdicts <artifact> <leg>… -> rc 1 and a list if any leg has none.
# The inert-instrument guard: a ledger nobody filled in must never read as clean.
dss_bh_assert_verdicts() {
  local artifact="$1"; shift
  local leg missing=""
  for leg in "$@"; do
    [ -n "${DSS_BH_VERDICT["$leg/$artifact"]:-}" ] || missing="$missing $leg"
  done
  [ -z "$missing" ] && return 0
  dss_bh_err "no '$artifact' verdict was ever recorded for leg(s):$missing"
  dss_bh_err "every declared leg must reach a verdict — silence about one is a harness bug."
  return 1
}

# ─────────────────────────────────────────────────────────────────────────────
# SELF-TEST — red-on-disable, by construction.
#
# Runs only when this file is EXECUTED, never when it is sourced, so the drivers
# pay nothing for it. Every assertion below builds a fixture that DIFFERS FROM A
# PASSING ONE IN ONE WAY and demands the function change its answer: a core that
# cannot be made to fail is not evidence of anything, and this one guards
# decisions (which duplicate path wins, which artifact a log names) whose wrong
# answers are SILENT.
#   bash base-harness.sh --self-test
# ─────────────────────────────────────────────────────────────────────────────
_bh_st_pass=0; _bh_st_fail=0
_bh_eq() {                       # _bh_eq <label> <want> <got>
  if [ "$2" = "$3" ]; then printf '  [PASS] %-56s\n' "$1"; _bh_st_pass=$((_bh_st_pass+1))
  else printf '  [FAIL] %-56s\n         want: %s\n         got : %s\n' "$1" "$2" "$3"; _bh_st_fail=$((_bh_st_fail+1)); fi
}
_bh_rc() {                       # _bh_rc <label> <want-rc> <got-rc>
  if [ "$2" = "$3" ]; then printf '  [PASS] %-56s rc=%s\n' "$1" "$3"; _bh_st_pass=$((_bh_st_pass+1))
  else printf '  [FAIL] %-56s want rc=%s got %s\n' "$1" "$2" "$3"; _bh_st_fail=$((_bh_st_fail+1)); fi
}

dss_bh_self_test() {
  local T; T="$(mktemp -d)" || return 2
  # shellcheck disable=SC2064
  trap "rm -rf '$T'" RETURN
  printf '== base-harness.sh --self-test (version %s) ==\n' "$DSS_BASE_HARNESS_VERSION"

  # ── 1. the continuation join ────────────────────────────────────────────────
  # The whole point of the portable form: a MULTI-LINE recipe must collapse to
  # one logical line. A BSD sed running the old `:a;N;$!ba;…` emitted only line 1,
  # so asserting the join HAPPENED is asserting the portability fix.
  printf 'cc -c a.c \\\n  -DFOO=1 \\\n  -Ione\ncc -o prog a.o \\\n  -Itwo\n' > "$T/recipe.txt"
  local blob; blob="$(dss_bh_recipe_blob "$T/recipe.txt")"
  _bh_eq "recipe_blob joins continuations into 2 lines" "2" "$(printf '%s\n' "$blob" | grep -c .)"
  # Both tokens must land on the SAME line — that is what "the join happened"
  # means. Note the join does NOT normalise whitespace: the recipe's own
  # indentation survives as runs of spaces, so this greps for co-location rather
  # than for a single-space spelling (a needle that would fail for the wrong
  # reason and read as a portability regression).
  _bh_eq "recipe_blob puts -DFOO=1 and -Ione on ONE line" \
         "1" "$(printf '%s\n' "$blob" | grep -c -- '-DFOO=1.*-Ione')"

  # ── 2. defines / includes extraction ────────────────────────────────────────
  _bh_eq "recipe_defines strips -D and make's literal quotes" \
         "FOO=1" "$(printf '%s\n' "$blob" | dss_bh_recipe_defines | tr '\n' ' ' | sed 's/ $//')"
  _bh_eq "recipe_includes drops the bare '.'" \
         "one two" "$(printf 'cc -I. -Ione -Itwo\n' | dss_bh_recipe_includes | tr '\n' ' ' | sed 's/ $//')"

  # ── 3. span selection — the mode that keeps BUILD-HOST TOOLS out ────────────
  # This is the sqlite3d case in miniature: the recipe contains a bootstrap line
  # naming a tool source, and only link-line mode may exclude it.
  printf 'cc -o lemon /tool/lemon.c\ncc -o prog shell.c a.o b.o\n' > "$T/r2.txt"
  local b2; b2="$(dss_bh_recipe_blob "$T/r2.txt")"
  _bh_eq "span whole-blob sees BOTH lines"  "2" "$(dss_bh_recipe_span "$b2" whole-blob prog | grep -c .)"
  _bh_eq "span link-line sees ONLY the link" "1" "$(dss_bh_recipe_span "$b2" link-line prog | grep -c .)"
  _bh_eq "span link-line EXCLUDES the tool source" \
         "0" "$(dss_bh_recipe_span "$b2" link-line prog | grep -c 'lemon\.c')"
  # A target that is a strict PREFIX of another must not harvest its sibling's
  # inputs — the reason the matcher anchors on a following space or end-of-line.
  printf 'cc -o prog x.c\ncc -o progd y.c\n' > "$T/r3.txt"
  local b3; b3="$(dss_bh_recipe_blob "$T/r3.txt")"
  _bh_eq "span link-line 'prog' does NOT match 'progd'" \
         "1" "$(dss_bh_recipe_span "$b3" link-line prog | grep -c .)"
  _bh_eq "  … and it picked the RIGHT one" \
         "1" "$(dss_bh_recipe_span "$b3" link-line prog | grep -c 'x\.c')"
  dss_bh_recipe_span "$b3" nonsense prog >/dev/null 2>&1
  _bh_rc "span rejects an unknown mode" 2 "$?"

  # ── 3b. the TOKEN span — the -D/-I question, which is NOT the TU question ───
  # The sqlite3d shape in miniature: a bootstrap line with FOREIGN defines, a
  # compile line carrying the one define that matters, and a link line carrying
  # the rest. All three are needed to reproduce the failure this closes.
  printf 'cc -DJIM_COMPAT -o jimsh /tool/jim.c\ncc -DCORE_ONLY -c a.c\ncc -DLINK_ONLY -o prog shell.c a.o\n' > "$T/r6.txt"
  local b6 tok
  b6="$(dss_bh_recipe_blob "$T/r6.txt")"
  tok="$(dss_bh_recipe_token_span "$b6" prog)"
  _bh_eq "token_span takes the compile line AND the link line" "2" "$(printf '%s\n' "$tok" | grep -c .)"
  _bh_eq "token_span keeps the COMPILE-only define (the SQLITE_CORE case)" \
         "1" "$(printf '%s\n' "$tok" | dss_bh_recipe_defines | grep -cx 'CORE_ONLY')"
  _bh_eq "token_span keeps the LINK-only define" \
         "1" "$(printf '%s\n' "$tok" | dss_bh_recipe_defines | grep -cx 'LINK_ONLY')"
  # ★ THE ASSERTION THAT CLOSES THE MEASURED DEFECT: the bootstrap's define must
  # NOT reach the target, and the whole-blob read is shown to let it through.
  _bh_eq "token_span EXCLUDES the bootstrap's foreign define" \
         "0" "$(printf '%s\n' "$tok" | dss_bh_recipe_defines | grep -cx 'JIM_COMPAT')"
  _bh_eq "  (control) whole-blob DOES let it through" \
         "1" "$(printf '%s\n' "$b6" | dss_bh_recipe_defines | grep -cx 'JIM_COMPAT')"

  # ── 4. deterministic dedup ──────────────────────────────────────────────────
  # THE defect this closes: the surviving path must depend on the SET, not on the
  # order it happens to be presented in. Feeding both orders and demanding the
  # SAME answer is the assertion; bash hash order fails it by construction.
  local o1 o2
  o1="$(printf '%s\n' /z/dup.c /a/dup.c /a/uniq.c | dss_bh_dedup_by_basename)"
  o2="$(printf '%s\n' /a/uniq.c /a/dup.c /z/dup.c | dss_bh_dedup_by_basename)"
  _bh_eq "dedup is ORDER-INDEPENDENT (the anchor's whole point)" "$o1" "$o2"
  _bh_eq "dedup keeps one path per basename" "2" "$(printf '%s\n' "$o1" | grep -c .)"
  _bh_eq "dedup keeps the SORT-FIRST duplicate" "1" "$(printf '%s\n' "$o1" | grep -c '^/a/dup\.c$')"

  # ── 5. the artifact reader ──────────────────────────────────────────────────
  local spec='x86_64:elf64-x86_64-linux-exec'
  printf 'noise\ndss-code-prime: artifact %s /out/a\nmore noise\n' "$spec" > "$T/log1"
  _bh_eq "reported_artifact reads the path"  "/out/a" "$(dss_bh_reported_artifact "$T/log1" "$spec")"
  # A path with a SPACE: the spec is one token by construction, so the path is
  # the whole remainder of the line and must survive intact.
  printf 'dss-code-prime: artifact %s /out dir/a\n' "$spec" > "$T/log1s"
  _bh_eq "reported_artifact keeps a path containing a space" \
         "/out dir/a" "$(dss_bh_reported_artifact "$T/log1s" "$spec")"
  # The LEGITIMATE re-run case: the same path reported twice is still one answer.
  printf 'dss-code-prime: artifact %s /out/a\ndss-code-prime: artifact %s /out/a\n' "$spec" "$spec" > "$T/log2"
  _bh_eq "a REPEATED identical report is still one artefact" "/out/a" "$(dss_bh_reported_artifact "$T/log2" "$spec")"
  # ★ THE SEAM. Two DIFFERENT artefacts for one spec used to silently return the
  # last; it must now REFUSE. This is the assertion that the CLI work made necessary.
  printf 'dss-code-prime: artifact %s /out/a\ndss-code-prime: artifact %s /out/b\n' "$spec" "$spec" > "$T/log3"
  dss_bh_reported_artifact "$T/log3" "$spec" >/dev/null 2>&1
  _bh_rc "TWO DIFFERENT artefacts for one spec is REFUSED" 2 "$?"
  local amb; amb="$(dss_bh_reported_artifact "$T/log3" "$spec" 2>&1 >/dev/null)"
  _bh_eq "  … and the refusal NAMES both paths" "1" \
         "$(printf '%s\n' "$amb" | grep -c '/out/b')"
  # A sibling spec's line must never be handed back for ours.
  printf 'dss-code-prime: artifact arm64:elf64-aarch64-linux-exec /out/other\n' > "$T/log4"
  dss_bh_reported_artifact "$T/log4" "$spec" >/dev/null 2>&1
  _bh_rc "a DIFFERENT spec's artefact is not returned" 1 "$?"
  dss_bh_reported_artifact "$T/nosuchlog" "$spec" >/dev/null 2>&1
  _bh_rc "an absent log is rc 1, not a crash" 1 "$?"

  # ── 6. the verdict ledger ───────────────────────────────────────────────────
  dss_bh_set_verdict legA sqlite3 built 'ok'
  _bh_eq "verdict round-trips" "built" "$(dss_bh_get_verdict legA sqlite3)"
  _bh_eq "verdicts are keyed per ARTIFACT, not per leg" "" "$(dss_bh_get_verdict legA testfixture)"
  dss_bh_assert_verdicts sqlite3 legA >/dev/null 2>&1
  _bh_rc "assert_verdicts passes when every leg has one" 0 "$?"
  dss_bh_assert_verdicts sqlite3 legA legB >/dev/null 2>&1
  _bh_rc "assert_verdicts FAILS on a leg with no verdict" 1 "$?"

  # ── 7. the floors ───────────────────────────────────────────────────────────
  # A recipe parse that breaks yields a SHORT list, not an error — the floor is
  # what turns that into a named stop instead of a smaller program.
  ( cd "$T" && printf '#!/bin/sh\necho "cc -o prog one.c"\n' > make && chmod +x make )
  : > "$T/one.c"
  ( PATH="$T:$PATH"; dss_bh_emit_recipe --build-dir "$T" --make-target prog \
      --recipe-file "$T/r4.txt" --prereq-mode link-line --min-tus 50 \
      --out-tus "$T/t.txt" --out-defines "$T/d.txt" --out-includes "$T/i.txt" ) >/dev/null 2>&1
  _bh_rc "emit_recipe FAILS below the TU floor" 1 "$?"
  ( PATH="$T:$PATH"; dss_bh_emit_recipe --build-dir "$T" --make-target prog \
      --recipe-file "$T/r5.txt" --prereq-mode link-line --min-tus 1 --min-defines 0 \
      --out-tus "$T/t.txt" --out-defines "$T/d.txt" --out-includes "$T/i.txt" ) >/dev/null 2>&1
  _bh_rc "emit_recipe PASSES at the floor (the control)" 0 "$?"
  _bh_eq "  … and resolved the relative TU against the build dir" \
         "$T/one.c" "$(cat "$T/t.txt")"
  dss_bh_emit_recipe --build-dir "$T" >/dev/null 2>&1
  _bh_rc "emit_recipe rejects a missing required argument" 2 "$?"

  # ── 8. the DROP LEDGER — the silent-TU-loss guard ───────────────────────────
  # Red-on-disable by construction: each assertion feeds a recoverer something it
  # CANNOT resolve and demands that the drop be reported. Delete the `else` arm
  # in either function and these go red; that is the whole point, because the
  # defect they close produces a program that is merely SMALLER, never an error.
  local dl="$T/drops.txt"
  DSS_BH_DROP_LOG="$dl"; : > "$dl"
  : > "$T/real.c"
  _bh_eq "span_tus still emits the token it CAN resolve" \
         "$T/real.c" "$(dss_bh_span_tus "$T/real.c missing-tu.c" "$T" 2>/dev/null)"
  _bh_eq "span_tus REPORTS the token it cannot resolve" \
         "1" "$(grep -c '^recipe-token.*missing-tu\.c' "$dl" || true)"
  DSS_BH_DROP_LOG=""
  # The archive half: a member with no matching source anywhere is a LOST TU.
  # `ar` is used for real here — the archive reader is the thing under test.
  DSS_BH_DROP_LOG="$dl"; : > "$dl"
  ( cd "$T" && : > lost.o && ar rcs libx.a lost.o ) >/dev/null 2>&1
  if [ -f "$T/libx.a" ]; then
    dss_bh_archive_tus "$T/libx.a" "" "$T" >/dev/null 2>&1
    _bh_eq "archive_tus REPORTS a member whose .c is nowhere" \
           "1" "$(grep -c '^archive-member.*lost\.o' "$dl" || true)"
    DSS_BH_DROP_LOG=""
    # …and emit_recipe turns that report into a HARD STOP, which is what makes it
    # "an immediate, named stop" rather than a floor that may or may not fire.
    ( PATH="$T:$PATH"; dss_bh_emit_recipe --build-dir "$T" --make-target prog \
        --recipe-file "$T/r7.txt" --prereq-mode link-line --min-tus 1 --min-defines 0 \
        --archive "$T/libx.a" --search-root "$T" \
        --out-tus "$T/t.txt" --out-defines "$T/d.txt" --out-includes "$T/i.txt" ) >/dev/null 2>&1
    _bh_rc "emit_recipe FAILS on a LOST archive member" 1 "$?"
  else
    printf '  [SKIP] archive drop assertions — `ar` could not create a test archive here\n'
  fi
  DSS_BH_DROP_LOG=""
  # An archive the caller NAMED and that is not there is its own named failure,
  # never a quiet zero-TU recovery.
  ( PATH="$T:$PATH"; dss_bh_emit_recipe --build-dir "$T" --make-target prog \
      --recipe-file "$T/r8.txt" --prereq-mode link-line --min-tus 1 --min-defines 0 \
      --archive "$T/no-such.a" --search-root "$T" \
      --out-tus "$T/t.txt" --out-defines "$T/d.txt" --out-includes "$T/i.txt" ) >/dev/null 2>&1
  _bh_rc "emit_recipe FAILS on an --archive that does not exist" 1 "$?"

  printf '\npassed=%s failed=%s\n' "$_bh_st_pass" "$_bh_st_fail"
  [ "$_bh_st_fail" -eq 0 ]
}

# Executed directly (not sourced) -> run the self-test. `return` from a sourced
# file is not an error; ${BASH_SOURCE[0]} == $0 only when this file IS the script.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1:-}" in
    --self-test) dss_bh_self_test; exit $? ;;
    *) printf 'base-harness.sh is a SOURCED library.\n  usage: bash %s --self-test\n' "$0"; exit 2 ;;
  esac
fi
