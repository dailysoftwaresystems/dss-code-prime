#!/usr/bin/env bash
# TF-C85 — THE PROFILE-CENSUS GUARD, TIER (b): the CORPUS census.
#
# ═══ WHY THIS IS A SCRIPT AND NOT A CTEST ENTRY — STATED PLAINLY ═════════════
#
# Tier (a) — `Preprocessor.TfC85NoUnclaimedPragmaUnderAnyPredefineClass` plus its
# non-vacuity twin — is ALWAYS ON in ctest, over the in-repo fixture
# `tests/corpus/c-subset/pragma_profile_census.c`. It is the guard that cannot
# rot.
#
# Tier (b) is THIS: the same census over the REAL 189-TU sqlite corpus. It is NOT
# wired into ctest, and that is a limitation being reported rather than papered
# over. The corpus lives OUTSIDE the repo (~/src/sqlite, cloned and configured by
# `real-examples/c/sqlite/build-and-test.sh`), and no ctest entry in this project
# reads an out-of-repo path — the established shape for a witness that cannot run
# unattended is a `DISABLED_`-gated test (see tests/link/test_ar_writer.cpp:468,
# :522), which would be a test that never runs. A runnable script that a human or
# a CI job invokes deliberately is the honest form.
#
# ═══ WHAT IT MEASURES ════════════════════════════════════════════════════════
#
# The FULL reached pragma vocabulary, per predefine class, by DISARMING the
# registry: it copies `src/dss-config` to a scratch dir, empties
# `preprocess.pragmaEffects` there, and points DSS at the copy via
# `DSS_CONFIG_ROOT`. With no row claiming anything, EVERY reached pragma emits a
# `P0020` naming itself — which is exactly a census. The repo's own config is
# never touched.
#
# The result is diffed against the CHECKED-IN expected set
# (`scripts/pragma-profile-census.expected`). A non-empty diff is a REVIEWABLE
# CHANGE, not automatically a failure.
#
# ═══ ★★ THE EXPECTED SET IS A FLOOR, NOT A TOTAL — DO NOT TREAT A DIFF AS A BUG ═
#
# A reached-set is a function of (i) the manifest's defines, (ii) the predefine
# class, and (iii) HOW FAR EACH TU GETS before a hard error stops it. All three
# move. MEASURED examples of each:
#   * (i)/(iii): sqlite's `ext/rtree/rtree.c` carries two `#pragma intrinsic`
#     lines that contribute ZERO, because the manifest defines `SQLITE_CORE`
#     without `SQLITE_ENABLE_RTREE` and the whole file body is therefore an
#     elided `#if` branch (C 6.10p1 — an elided pragma is entirely silent).
#   * (ii): the ENTIRE `warning`/`intrinsic`/`optimize` vocabulary — 2135 lines —
#     is invisible on macho and elf and visible only on pe, because its guard is
#     `#if defined(_MSC_VER)`.
# So as other cycles clear blockers, TUs get further and this set GROWS. That is
# expected. The point of the diff is that new pragma vocabulary arrives as
# something a human reads and decides about, instead of as a silent pass or a
# surprise build break three cycles later.
#
# ═══ USAGE ═══════════════════════════════════════════════════════════════════
#
#   scripts/pragma-profile-census.sh [--update]
#
#   --update   rewrite the expected file from this run (review the diff first!)
#
# Environment:
#   DSS_BIN        the compiler (default: the newest build/**/dss-code-prime)
#   SQLITE_MANIFEST a .dss-project.json to census (default: the host manifest the
#                  sqlite harness generates under build/real-examples/)
#
# Exit: 0 = census matches the expected set; 1 = it differs (review + --update);
#       2 = the census could not run at all (missing corpus, missing binary).
#
# NOTE FOR MAINTAINERS: never end this script with a pipeline whose tail swallows
# the status. Every exit code below is captured DIRECTLY from the command.

set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/dss-pragma-census.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

EXPECTED_FILE="$REPO_ROOT/scripts/pragma-profile-census.expected"
UPDATE=0
[[ "${1:-}" == "--update" ]] && UPDATE=1

die()  { printf 'pragma-census: FATAL: %s\n' "$*" >&2; exit 2; }
info() { printf 'pragma-census: %s\n' "$*" >&2; }

# ── the compiler ─────────────────────────────────────────────────────────────
if [[ -z "${DSS_BIN:-}" ]]; then
  DSS_BIN="$(find "$REPO_ROOT/build" -type f -name dss-code-prime -perm -u+x -print -quit 2>/dev/null)"
fi
[[ -n "${DSS_BIN:-}" && -x "$DSS_BIN" ]] \
  || die "dss-code-prime not found; build the project or set DSS_BIN."

# ── the corpus manifest ──────────────────────────────────────────────────────
MANIFEST="${SQLITE_MANIFEST:-$REPO_ROOT/build/real-examples/c/sqlite/host/host.dss-project.json}"
[[ -f "$MANIFEST" ]] \
  || die "no corpus manifest at $MANIFEST — run real-examples/c/sqlite/build-and-test.sh first, or set SQLITE_MANIFEST."

# ── the DISARMED config copy (the repo's own config is never modified) ───────
cp -R "$REPO_ROOT/src" "$SCRATCH/src" || die "could not copy src/ to the scratch dir"
LANG_JSON="$SCRATCH/src/dss-config/sources/c-subset.lang.json"
[[ -f "$LANG_JSON" ]] || die "no c-subset.lang.json in the scratch copy"
python3 - "$LANG_JSON" <<'PY' || die "could not disarm the pragma registry"
import json, sys
p = sys.argv[1]
d = json.load(open(p))
pp = d.get("preprocess")
if pp is None or "pragmaEffects" not in pp:
    sys.exit("c-subset.lang.json no longer has preprocess.pragmaEffects")
pp["pragmaEffects"] = []          # claim NOTHING
pp["unknownPragmaIsError"] = True # so every reached pragma names itself
json.dump(d, open(p, "w"), indent=2, ensure_ascii=False)
PY
export DSS_CONFIG_ROOT="$SCRATCH"

# ── census, one pass per predefine class ─────────────────────────────────────
# MEASURED: `availableObjectFormats` keys on format KIND, so the 24 shipped
# format files collapse to exactly these three classes.
declare -a LEGS=(
  "pe:x86_64:pe64-x86_64-windows-exec"
  "macho:arm64:macho64-arm64-darwin-exec"
  "elf:x86_64:elf64-x86_64-linux-exec"
)

ACTUAL="$SCRATCH/actual.txt"
: > "$ACTUAL"
for leg in "${LEGS[@]}"; do
  name="${leg%%:*}"; spec="${leg#*:}"
  info "censusing predefine class '$name' ($spec) ..."
  python3 - "$MANIFEST" "$spec" "$SCRATCH/$name.dss-project.json" <<'PY' \
    || die "could not derive the $name manifest"
import json, sys
src, spec, out = sys.argv[1], sys.argv[2], sys.argv[3]
m = json.load(open(src))
m["targets"] = [spec]
m.pop("resolveLibraries", None)   # link-tier only; the census stops at PP
json.dump(m, open(out, "w"), indent=2)
PY
  "$DSS_BIN" --project "$SCRATCH/$name.dss-project.json" \
             --output "$SCRATCH/$name-out" > "$SCRATCH/$name.log" 2>&1
  rc=$?   # captured DIRECTLY, never after a pipe
  info "  (compiler exit $rc — a census run is EXPECTED to fail the build)"
  # `unrecognized pragma '<text>'` -> the pragma's leading WORD, counted.
  grep -oE "unrecognized pragma '[^']*'" "$SCRATCH/$name.log" 2>/dev/null \
    | sed -E "s/unrecognized pragma '([^ ']*).*/\1/" \
    | sort | uniq -c | awk -v leg="$name" '{printf "%s %s %s\n", leg, $2, $1}' \
    >> "$ACTUAL"
done
sort -o "$ACTUAL" "$ACTUAL"

if [[ "$UPDATE" == "1" ]]; then
  { grep '^#' "$EXPECTED_FILE" 2>/dev/null || true; cat "$ACTUAL"; } > "$EXPECTED_FILE.new"
  mv "$EXPECTED_FILE.new" "$EXPECTED_FILE"
  info "expected set UPDATED -> $EXPECTED_FILE"
  cat "$EXPECTED_FILE" >&2
  exit 0
fi

if [[ ! -f "$EXPECTED_FILE" ]]; then
  info "no expected set yet; run with --update after reviewing:"
  cat "$ACTUAL" >&2
  exit 1
fi

# `#`-comment lines in the expected file are documentation, not data.
grep -v '^#' "$EXPECTED_FILE" | grep -v '^[[:space:]]*$' > "$SCRATCH/expected.data"
if diff -u "$SCRATCH/expected.data" "$ACTUAL" > "$SCRATCH/diff.txt"; then
  info "census MATCHES the checked-in expected set."
  exit 0
fi
info "census DIFFERS from the checked-in expected set."
info "This is a REVIEWABLE CHANGE, not automatically a bug — the reached set"
info "grows as other cycles let TUs get further. Read the diff, decide whether"
info "each new prefix needs a 'preprocess.pragmaEffects' row, then --update."
cat "$SCRATCH/diff.txt" >&2
exit 1
