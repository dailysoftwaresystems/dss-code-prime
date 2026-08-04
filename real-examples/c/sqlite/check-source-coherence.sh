#!/usr/bin/env bash
#
# real-examples/c/sqlite/check-source-coherence.sh
# ─────────────────────────────────────────────────────────────────────────────
# FAIL-LOUD coherence gate for a STAGED SQLITE SOURCE SET.
# D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE
#
# WHY THIS EXISTS — the defect it was written for, MEASURED 2026-08-04.
#
# The harness reuses ONE sqlite build directory (`$SQLITE_DIR/bld-dss`) across
# runs, and Step 3 `git pull --rebase`es the sqlite checkout underneath it on
# every run. `make` then regenerates ONLY the derived files the requested target
# needs. The harness's target is `testfixture USE_AMALGAMATION=0`, whose object
# list is LIBOBJS0 — the ~100 per-TU objects — so the AMALGAMATION artifacts
# (`sqlite3.c`, `shell.c`, `tclsqlite3.c`, and the `tsrc/` tree they are built
# from) are prerequisites of NOTHING the harness ever asks for. They are orphans:
# they sit in the build directory looking current while every file around them
# marches forward with upstream.
#
# The observed state on 2026-08-04 (all MEASURED, `SQLITE_VERSION` "3.54.0" in
# every one of them, so a version-string check sees nothing wrong):
#
#     sqlite3.c        2026-07-06  7f49a7a9…   (orphan — 4 weeks stale)
#     shell.c          2026-07-06  (no id of its own)
#     tclsqlite3.c     2026-07-06  7f49a7a9…   (orphan)
#     tsrc/sqlite3.h   2026-07-06  7f49a7a9…   (orphan)
#     sqlite3.h        2026-08-03  0f873f56…   (regenerated: a make prerequisite)
#     opcodes.c/h/.o   2026-08-03              (regenerated)
#     101 × *.o, libsqlite3.a
#                      2026-08-04  0f873f56…   (rebuilt)
#
# WHAT THAT COSTS. The amalgamation `sqlite3.c` does NOT `#include "sqlite3.h"` —
# mksqlite3c.tcl comments every such directive out and INLINES the header, so
# `sqlite3.c` carries its own `#define SQLITE_SOURCE_ID` (line ~472) and the
# LIBRARY is self-coherent whatever the vintage. `shell.c:161`, however, has a
# LIVE `#include "sqlite3.h"`, which resolves to the header sitting beside it.
# So the CLI links a JULY library against an AUGUST header, and sqlite's own
# guard fires at startup (shell.c, `main()`):
#
#     if( cli_strncmp(sqlite3_sourceid(),SQLITE_SOURCE_ID,60)!=0 ){
#       cli_printf(stderr,"SQLite header and source version mismatch\n%s\n%s\n", …);
#       exit(1);
#     }
#
# The binary COMPILES and LINKS clean and then refuses to run. That is the whole
# class this gate closes: an instrument whose input silently changed vintage,
# reporting a pass over work it did not do.
#
# ─────────────────────────────────────────────────────────────────────────────
# TWO ID CLASSES, AND WHY ONLY ONE OF THEM IS AN ERROR
#
# A staged sqlite tree carries 64-hex fossil check-in ids in TWO distinct roles,
# and conflating them would make this gate red on a perfectly healthy tree:
#
#   IDENTITY class — `SQLITE_SOURCE_ID`, emitted into sqlite3.h by mksqlite3h.tcl
#     and inlined into sqlite3.c / tclsqlite3.c / tsrc/sqlite3.h, and compiled
#     into libsqlite3.a (main.o) as the return value of sqlite3_sourceid().
#     THIS is what shell.c's guard compares. Every artifact carrying one MUST
#     agree — divergence here is the defect above. ENFORCED.
#
#   FTS5 class — `fts5: <date> <id>`, the string returned by the fts5_source_id()
#     SQL function, baked into the generated `fts5.c` by mkfts5c.tcl from the
#     checkout uuid AT GENERATION TIME. `fts5.c`'s make prerequisites are
#     $(FTS5_SRC) only, so make legitimately leaves it alone while the rest of
#     the tree moves — and in a REUSED build dir its stamp therefore LAGS.
#     MEASURED 2026-08-04: fts5.c stamped 2026-07-24 (2f1f4f73…) while HEAD was
#     2026-08-03 (0f873f56…) — and `git log -1 -- ext/fts5/` says the last fts5
#     change was 2026-07-14, i.e. the CONTENT was current and only the stamp was
#     old. Requiring it to equal the identity id would red a correct tree, so it
#     is REPORTED, never asserted against the identity class. It IS asserted
#     against ITSELF: two different fts5 stamps inside one stage means one of the
#     copies really is stale.
#
# Silence about either class is a gate bug: both are printed on every run.
#
# ─────────────────────────────────────────────────────────────────────────────
# USAGE
#   check-source-coherence.sh [--checkout <sqlite-git-dir>] [--require-cli]
#                             [--label <name>] <dir> [<dir>…]
#   check-source-coherence.sh --self-test
#
#   --checkout   also assert the single identity id equals the checkout's
#                manifest.uuid — i.e. the stage really came from THAT source
#                state, not merely from ONE state.
#   --require-cli  additionally assert the CLI triple (sqlite3.c, shell.c,
#                sqlite3.h) is present and that shell.c's QUOTED include can only
#                resolve inside the stage.
#   --self-test  run the red-on-disable battery and exit.
#
# EXIT: 0 coherent · 1 INCOHERENT (names the files and their ids) · 2 usage/IO.
# There is no warn-and-continue path and no skip: a stage in which no identity id
# can be found FAILS, because a gate that finds nothing must never report OK.
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

PROG="${0##*/}"
# One regex, used for both classes; the optional `fts5: ` prefix is what
# separates them. `-a` on every grep: libsqlite3.a is binary.
ID_RE='(fts5: )?20[0-9]{2}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} [0-9a-f]{64}'

die()  { printf '%s: %s\n' "$PROG" "$*" >&2; exit 2; }
line() { printf '%s\n' "------------------------------------------------------------------"; }

# scan_dir <dir> <out-identity-tsv> <out-fts5-tsv>
# Appends "<relpath>\t<id>" rows. One row per DISTINCT id per file, so a file
# holding two different ids (sqlite3.c inlines fts5.c) reports both.
scan_dir() {
  local d="$1" ident="$2" fts="$3" f rel raw id
  while IFS= read -r f; do
    while IFS= read -r raw; do
      [ -n "$raw" ] || continue
      rel="${f#"$d"/}"
      case "$raw" in
        "fts5: "*) id="${raw#fts5: }"; printf '%s\t%s\n' "$rel" "$id" >> "$fts"   ;;
        *)         id="$raw";          printf '%s\t%s\n' "$rel" "$id" >> "$ident" ;;
      esac
    done < <(grep -aoE "$ID_RE" "$f" 2>/dev/null | sort -u)
  done < <(find "$d" -maxdepth 2 -type f \( -name '*.c' -o -name '*.h' -o -name '*.a' \) 2>/dev/null | sort)
}

# report_class <label> <tsv> <enforce:0|1> -> 0 ok, 1 divergent
# `enforce` decides whether a divergence is fatal; the report is printed either
# way, because a class we do not enforce is still a fact the reader is owed.
report_class() {
  local label="$1" tsv="$2" enforce="$3" n
  n=$(cut -f2 "$tsv" 2>/dev/null | sort -u | grep -c . || true)
  if [ "$n" -eq 0 ]; then
    printf '   %-16s (none present)\n' "$label:"
    return 0
  fi
  if [ "$n" -eq 1 ]; then
    printf '   %-16s %s\n' "$label:" "$(cut -f2 "$tsv" | sort -u)"
    printf '   %-16s %s file(s) agree\n' "" "$(wc -l < "$tsv" | tr -d ' ')"
    return 0
  fi
  printf '   %-16s %s DIFFERENT ids across %s file(s)%s\n' "$label:" "$n" \
         "$(wc -l < "$tsv" | tr -d ' ')" "$([ "$enforce" = 1 ] && echo '  <-- INCOHERENT' || echo '')"
  sort -k2 "$tsv" | while IFS=$'\t' read -r rel id; do
    printf '        %-34s %s\n' "$rel" "$id"
  done
  [ "$enforce" = 1 ] && return 1
  return 0
}

run_check() {                       # run_check <label> <checkout|""> <require_cli> <dir…>
  local label="$1" checkout="$2" require_cli="$3"; shift 3
  local tmp ident fts rc=0 d nident uniq want
  tmp="$(mktemp -d)" || die "mktemp failed"
  # shellcheck disable=SC2064
  trap "rm -rf '$tmp'" RETURN
  ident="$tmp/ident.tsv"; fts="$tmp/fts5.tsv"; : > "$ident"; : > "$fts"

  printf '\n== sqlite source coherence: %s ==\n' "$label"
  for d in "$@"; do
    [ -d "$d" ] || { printf ' [X] not a directory: %s\n' "$d" >&2; return 2; }
    printf '   scanning        %s\n' "$d"
    scan_dir "$d" "$ident" "$fts"
  done

  report_class "SQLITE_SOURCE_ID" "$ident" 1 || rc=1
  report_class "fts5 stamp"       "$fts"   1 || rc=1

  nident=$(cut -f2 "$ident" 2>/dev/null | sort -u | grep -c . || true)
  if [ "$nident" -eq 0 ]; then
    line
    printf ' [X] NO SQLITE_SOURCE_ID FOUND in: %s\n' "$*" >&2
    printf '     A coherence gate that finds nothing must not report OK. Either these\n' >&2
    printf '     are not sqlite source dirs, or sqlite3.h/sqlite3.c were never generated.\n' >&2
    return 1
  fi
  uniq="$(cut -f2 "$ident" | sort -u | head -1)"

  # --checkout: ONE state is necessary but not sufficient — say WHICH state.
  if [ -n "$checkout" ]; then
    [ -f "$checkout/manifest.uuid" ] || die "no manifest.uuid under $checkout"
    want="$(tr -d ' \n' < "$checkout/manifest.uuid")"
    if [ "$rc" -eq 0 ] && [ "${uniq##* }" != "$want" ]; then
      line
      printf ' [X] the stage is internally coherent but does NOT match the checkout.\n' >&2
      printf '     stage    : %s\n' "$uniq"   >&2
      printf '     checkout : %s  (%s/manifest.uuid)\n' "$want" "$checkout" >&2
      rc=1
    else
      printf '   %-16s matches %s/manifest.uuid\n' "checkout:" "$checkout"
    fi
  fi

  # --require-cli: shell.c's include is QUOTED, so it resolves beside shell.c
  # FIRST. A stage that ships shell.c without its own sqlite3.h will silently
  # pick one up from an -I dir — which is exactly how this defect reached a
  # binary in the first place.
  if [ "$require_cli" = 1 ]; then
    local sdir="" missing=""
    for d in "$@"; do [ -f "$d/shell.c" ] && sdir="$d"; done
    [ -n "$sdir" ] || missing="shell.c"
    for f in sqlite3.c sqlite3.h; do
      [ -n "$sdir" ] && [ -f "$sdir/$f" ] || missing="$missing $f"
    done
    if [ -n "$missing" ]; then
      line
      printf ' [X] --require-cli: the CLI triple is incomplete; missing:%s\n' " $missing" >&2
      printf '     shell.c does #include "sqlite3.h" (QUOTED) — the header MUST sit\n' >&2
      printf '     beside it or the build silently takes one from an -I dir.\n' >&2
      rc=1
    else
      printf '   %-16s sqlite3.c + shell.c + sqlite3.h all present in %s\n' "CLI triple:" "$sdir"
    fi
  fi

  line
  if [ "$rc" -eq 0 ]; then
    printf ' [OK] coherent at %s\n' "$uniq"
  else
    printf ' [X] INCOHERENT SQLITE SOURCE STAGE — refusing to certify this tree.\n' >&2
    printf '     D-HARNESS-SQLITE-STAGED-TREE-MIXED-VINTAGE. A build from these inputs can\n' >&2
    printf '     COMPILE AND LINK CLEAN and then die at startup with sqlite'"'"'s own\n' >&2
    printf '     "SQLite header and source version mismatch". Regenerate the derived files\n' >&2
    printf '     from ONE source state using the tree'"'"'s own rules, e.g. in the build dir:\n' >&2
    printf '         make sqlite3.c shell.c tclsqlite3.c\n' >&2
    printf '     (hand-copying one file is a workaround: it fixes the symptom you noticed\n' >&2
    printf '     and leaves the ones you did not.)\n' >&2
  fi
  return $rc
}

# ─────────────────────────────────────────────────────────────────────────────
# SELF-TEST — red-on-disable, by construction.
# Every assertion below builds a fixture that DIFFERS FROM A PASSING ONE IN ONE
# WAY and demands the gate change verdict. A gate that cannot be made to fail is
# not evidence of anything.
# ─────────────────────────────────────────────────────────────────────────────
ID_A='2026-07-06 16:26:30 7f49a7a90eda01753c0dff65197bd7bc48a751e24a46919d30af6e2baf0788fc'
ID_B='2026-08-03 15:05:05 0f873f565192e7d1e0bfa1f1c147d02f6cca5b91492f4498736d2c8896599e9d'
ID_F='2026-07-24 16:28:47 2f1f4f73535386549c12694dc57cfe555eec689ae6824c6241aaf8d5befcd74d'

st_pass=0; st_fail=0
st_assert() {                        # st_assert <label> <want-rc> <got-rc> [<must-contain> <output>]
  local label="$1" want="$2" got="$3" needle="${4:-}" out="${5:-}"
  if [ "$got" != "$want" ]; then
    printf '  [FAIL] %-52s want rc=%s got rc=%s\n' "$label" "$want" "$got"; st_fail=$((st_fail+1)); return
  fi
  # -F: the needles are literals, and several ("[OK] coherent", "[X] …") would be
  # read as regex character classes — a self-test that fails for its own reasons
  # is the one thing worse than no self-test.
  if [ -n "$needle" ] && ! printf '%s' "$out" | grep -qF -- "$needle"; then
    printf '  [FAIL] %-52s rc ok but output never named "%s"\n' "$label" "$needle"; st_fail=$((st_fail+1)); return
  fi
  printf '  [PASS] %-52s rc=%s\n' "$label" "$got"; st_pass=$((st_pass+1))
}
mk_hdr() { printf '#define SQLITE_VERSION "3.54.0"\n#define SQLITE_SOURCE_ID      "%s"\n' "$2" > "$1"; }
mk_fts() { printf 'sqlite3_result_text(p,"fts5: %s",-1,0);\n' "$2" > "$1"; }

ST_TMP=""                            # GLOBAL on purpose: the EXIT trap fires after
trap '[ -n "${ST_TMP:-}" ] && rm -rf "$ST_TMP"' EXIT   # self_test's locals are gone.
self_test() {
  local T out rc
  ST_TMP="$(mktemp -d)" || die "mktemp failed"
  T="$ST_TMP"
  printf '== %s --self-test ==\n' "$PROG"

  # 1 coherent
  mkdir -p "$T/ok"; mk_hdr "$T/ok/sqlite3.h" "$ID_B"; mk_hdr "$T/ok/sqlite3.c" "$ID_B"
  out="$(run_check ok "" 0 "$T/ok" 2>&1)"; rc=$?
  st_assert "coherent stage passes" 0 "$rc" "[OK] coherent" "$out"

  # 2 the real defect: two identity ids
  mkdir -p "$T/mix"; mk_hdr "$T/mix/sqlite3.h" "$ID_B"; mk_hdr "$T/mix/sqlite3.c" "$ID_A"
  out="$(run_check mix "" 0 "$T/mix" 2>&1)"; rc=$?
  st_assert "mixed SQLITE_SOURCE_ID fails" 1 "$rc" "INCOHERENT" "$out"
  st_assert "  … and NAMES the divergent files" 1 "$rc" "sqlite3.c" "$out"
  st_assert "  … and PRINTS both ids"            1 "$rc" "7f49a7a9" "$out"

  # 3 an empty stage must NOT pass (the inert-guard trap)
  mkdir -p "$T/empty"
  out="$(run_check empty "" 0 "$T/empty" 2>&1)"; rc=$?
  st_assert "stage with no id at all fails" 1 "$rc" "NO SQLITE_SOURCE_ID FOUND" "$out"

  # 4 a LAGGING fts5 stamp must NOT red a coherent stage (false-red control)
  mkdir -p "$T/fts"; mk_hdr "$T/fts/sqlite3.h" "$ID_B"; mk_fts "$T/fts/fts5.c" "$ID_F"
  out="$(run_check fts "" 0 "$T/fts" 2>&1)"; rc=$?
  st_assert "lagging fts5 stamp does NOT false-red" 0 "$rc" "2f1f4f73" "$out"

  # 5 …but two DIFFERENT fts5 stamps do
  mkdir -p "$T/fts2/tsrc"; mk_hdr "$T/fts2/sqlite3.h" "$ID_B"
  mk_fts "$T/fts2/fts5.c" "$ID_F"; mk_fts "$T/fts2/tsrc/fts5.c" "$ID_A"
  out="$(run_check fts2 "" 0 "$T/fts2" 2>&1)"; rc=$?
  st_assert "two different fts5 stamps fail" 1 "$rc" "fts5 stamp" "$out"

  # 6 --checkout: internally coherent but the WRONG state
  mkdir -p "$T/co"; printf '%s\n' "${ID_A##* }" > "$T/co/manifest.uuid"
  out="$(run_check ok "$T/co" 0 "$T/ok" 2>&1)"; rc=$?
  st_assert "coherent-but-wrong-checkout fails" 1 "$rc" "does NOT match the checkout" "$out"
  printf '%s\n' "${ID_B##* }" > "$T/co/manifest.uuid"
  out="$(run_check ok "$T/co" 0 "$T/ok" 2>&1)"; rc=$?
  st_assert "coherent-and-right-checkout passes" 0 "$rc" "matches" "$out"

  # 7 --require-cli: shell.c shipped without its own header
  mkdir -p "$T/cli"; mk_hdr "$T/cli/sqlite3.c" "$ID_B"; printf '#include "sqlite3.h"\n' > "$T/cli/shell.c"
  out="$(run_check cli "" 1 "$T/cli" 2>&1)"; rc=$?
  st_assert "CLI stage missing sqlite3.h fails" 1 "$rc" "CLI triple is incomplete" "$out"
  mk_hdr "$T/cli/sqlite3.h" "$ID_B"
  out="$(run_check cli "" 1 "$T/cli" 2>&1)"; rc=$?
  st_assert "complete CLI stage passes" 0 "$rc" "CLI triple:" "$out"

  printf '\npassed=%s failed=%s\n' "$st_pass" "$st_fail"
  [ "$st_fail" -eq 0 ]
}

# ── argv ─────────────────────────────────────────────────────────────────────
CHECKOUT=""; REQUIRE_CLI=0; LABEL=""; DIRS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --self-test)   self_test; exit $? ;;
    --checkout)    CHECKOUT="${2:-}"; [ -n "$CHECKOUT" ] || die "--checkout needs a directory"; shift 2 ;;
    --require-cli) REQUIRE_CLI=1; shift ;;
    --label)       LABEL="${2:-}"; shift 2 ;;
    -h|--help)     sed -n '/^# USAGE/,/^# ─/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)            die "unknown option: $1" ;;
    *)             DIRS+=("$1"); shift ;;
  esac
done
[ "${#DIRS[@]}" -gt 0 ] || die "usage: $PROG [--checkout DIR] [--require-cli] <dir>… | --self-test"
run_check "${LABEL:-${DIRS[0]}}" "$CHECKOUT" "$REQUIRE_CLI" "${DIRS[@]}"
