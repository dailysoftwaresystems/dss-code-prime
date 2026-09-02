#!/usr/bin/env bash
# PURPOSE: refuse a tracked text blob that carries a CR, and a CR instrument that cannot see one.
# check-line-endings.sh — CI guard for D-REPO-GITATTRIBUTES-PINS-EOL-FOR-CONFIGS-BUT-NOT-FOR-SOURCES.
#
# Contract: NO tracked TEXT blob in this repository may contain a line-terminating
# CR — not in HEAD, and not staged in the index. A file that genuinely needs its
# 0x0D bytes preserved declares itself `binary` in `.gitattributes`, which this
# guard honours through `git grep -I` (that is how `examples/**/*.bin` keeps its
# `#embed` resource bytes).
#
# Why a gate and not just the pin. `.gitattributes` now pins `eol=lf` for the
# source tree, but a pin is only as good as its GLOB COVERAGE: the pin that
# existed before this cycle covered configs, goldens and harness drivers and
# still let six CRLF blobs land in `src/` and `tests/` because no `.cpp` was
# ever named. The next unpinned extension repeats that exactly. This check is
# the invariant the pin is only a mechanism for — it holds over every tracked
# text blob regardless of which globs anyone remembered to write.
#
# ⚠ INSTRUMENT NOTES, all MEASURED on this workstation 2026-08-06 while the
# defect was being diagnosed — read these before "improving" any command here:
#   · `git show HEAD:<path>` APPLIES eol conversion on the way out, so it can
#     NEVER tell you what was committed. `git cat-file blob` can. This script
#     uses `git grep <rev>`, which reads the BLOB, not a smudged checkout.
#   ┌─ CR-INSTRUMENT-QUOTED:BEGIN ─ the block below QUOTES the blind idioms in
#   │  order to explain them; it does not run one as a measurement. Check F
#   │  honours this region marker from any file, and uses it here rather than
#   │  exempting itself by path — a guard that needs a private escape cannot be
#   │  held to its own rule.
#   · ⚠⚠ THE COMMON CR INSTRUMENTS ARE BLIND ON THIS HOST, IN BOTH DIRECTIONS.
#     Counting is done here with `tr -dc '\r' | wc -c`, which is the only form
#     measured correct. The two traps, and an EARLIER VERSION OF THIS VERY NOTE
#     GOT THE FIRST ONE WRONG — it claimed `grep -c $'\r'` "matches the LETTER
#     `r`", which is refuted by a control containing no `r` at all (`grep -c 'r'`
#     returns 0 on it; the idiom returns the LINE COUNT). ✔RE-MEASURED 2026-08-27
#     (cycle P42) against a control built with `printf 'a\r\nb\n'` and verified
#     by `od -c` to hold exactly one CR:            CR-INSTRUMENT-QUOTED
#
#       TRAP 1 — FALSE POSITIVE, and it fires on a CLEAN file. The literal token
#       `$'\r'` written INSIDE a command substitution expands to the EMPTY
#       STRING, so `n=$(grep -c $'\r' f)` runs `grep -c ''` and returns the
#       file's LINE COUNT — 2 for the CRLF control and 2 for its pure-LF twin.
#       ✔The loss is at PARSE time and is CR-SPECIFIC: an argv-dumping shim shows
#       `bash shim $'\r'` delivering byte `0d` when run BARE and an EMPTY
#       argument when the same text sits inside `$( )`, while `$'\t'` and `$'A'`
#       in that identical position both survive. A CR held in a VARIABLE is
#       unaffected (`CR=$'\r'` at top level, then `"$CR"`, delivers `0d` either
#       way) — so the defect is the SPELLING, not command substitution.
#       ⓘ INFERRED, not measured: the likely cause is the substitution body
#       being re-scanned by the parser, which consumes the expanded CR as line
#       whitespace. The RULE above is measured; this sentence is a guess.
#
#       TRAP 2 — FALSE NEGATIVE, the dangerous one, and NO amount of quoting
#       care avoids it because the READER discards the byte. GNU grep and sed
#       here open files in TEXT MODE and strip the trailing CR BEFORE matching,
#       so on the `od`-proved CRLF control:      CR-INSTRUMENT-QUOTED
#           grep -c "$CR"   -> 0     grep -U -c "$CR" -> 1   (-U is the fix)
#           grep -a -c "$CR"-> 0     awk '/\r$/'      -> 0   (-a does NOT help)
#           sed -n '/\r/p'  -> 0     tr -dc '\r'|wc -c-> 1   (correct)
#       A MID-LINE CR is found by every one of them — the blindness is aimed
#       precisely at the only CR anyone ever hunts. This is why a lane can
#       certify a tree "pure LF" with `awk '/\r$/'` while measuring nothing.
#
#     ★★ AND THIS IS WHY IT SURVIVED: under WSL/Linux all three instruments are
#     CORRECT (1 on the CRLF control, 0 on the LF twin). An author who sanity-
#     checks the idiom on Linux — or reads the GNU manual — gets a right answer
#     and concludes it is sound. It then lies ONLY on Windows, the primary
#     development host. An instrument verified on the wrong leg is verified
#     nowhere, which is also why the `.ps1` twin below is held to its own
#     control rather than assumed to inherit this one's correctness.
#     ⇒ Check F refuses these spellings; `--files` is the entry point that
#       exists so nobody needs to hand-roll one. See `--help`.
#   └─ CR-INSTRUMENT-QUOTED:END ─────────────────────────────────────────────
#   · a `grep -P` that ABORTS (no PCRE support, a hostile locale) prints nothing
#     and is trivially misread as "measured zero offenders". That is why the
#     POSITIVE CONTROL below exists: the same `-P` engine, the same ref, a
#     pattern that MUST match thousands of files. If the instrument is dead the
#     control collapses and this guard FAILS — it never reports a clean tree it
#     did not actually read.
#
# Cross-platform: this is the bash variant for Linux/macOS CI; the companion
# `check-line-endings.ps1` runs on Windows CI and MUST stay behaviourally
# identical (D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED — pairing by EXISTENCE is not
# pairing by BEHAVIOUR, so any change here is mirrored there in the same commit).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# ⚠ CAPTURED BEFORE THE `cd` BELOW. `--files` takes paths from a CALLER who is
# somewhere else — very often a lane's own scratchpad, outside this repo
# entirely — so a relative path must resolve against where the user stood, not
# against the repo root. Resolving them after the `cd` silently answered about
# the wrong file (or "missing"), which is the failure this mode exists to end.
INVOKED_FROM="$(pwd -P)"

# ── THE ONE ROOT, AND ONE GIT THAT CAN SEE IT ─────────────────────────────
# ⛔ D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE. This guard used a BARE `git`,
# which cannot describe a Windows-created lane worktree from the POSIX namespace:
# `.worktrees/<lane>/.git` is a FILE naming `C:/…/.git/worktrees/<lane>`, and a
# POSIX git JOINS that to the worktree path instead of following it.
# ✔RE-MEASURED 2026-09-01 (cycle P51, lane `gw`), from WSL over `.worktrees/gw`:
#     rc=2, "line-endings: FAIL — HEAD does not resolve; this is not a git work
#     tree with a commit. Refusing to report a pass over a tree it cannot read."
# while the SAME guard over the main checkout in the same shell returned rc=0.
# That is the SAFE direction — loud, refusing, correctly distrusting itself — but
# it left every lane unable to check its own tree from two of the four gate legs,
# and `.worktrees/` is the sanctioned lane mechanism under the 2026-08-26 ruling.
#
# ★ ONE OWNER, REUSED, NOT A THIRD SPELLING. `leg_tree_driver_identity` already
# resolves this exact question for the carriages
# (D-SCRIPT-CARRIAGES-CANNOT-IDENTIFY-A-CROSS-NAMESPACE-LANE-WORKTREE); its
# three ordered cases are the answer and re-deriving them here would be a second
# answer that drifts. The PowerShell twin has its own owner, written once, at
# `scripts/repo-tree/repo-tree.ps1`.
# ⚠⚠ THE EXPLICIT EMPTY ARGUMENT IS LOAD-BEARING AND WAS FOUND BY RUNNING IT.
# `leg-tree.sh` ends in a `case "${1:-}"` dispatch so it can be both sourced and
# run, and a SOURCED script sees the CALLER'S positional parameters. Sourcing it
# bare from `check-line-endings.sh --selftest` therefore handed leg-tree the
# string `--selftest` as a SUBCOMMAND: ✔MEASURED 2026-09-01, rc=4,
# "[X] leg-tree: unknown subcommand '--selftest' (expected: prepare, restore)".
# ⓘ An earlier probe of mine sourced it with NO arguments and concluded "no side
# effects" — a true answer to the wrong question, and the reason this note states
# the argument rather than the conclusion. `. file ""` selects leg-tree's own
# "sourced or inlined" branch, and bash restores THIS script's positional
# parameters when the source returns (✔verified by execution, `$1` intact after).
# shellcheck source=../leg-tree/leg-tree.sh
. "${REPO_ROOT}/scripts/leg-tree/leg-tree.sh" ""

# _le_git <git-arg>...
# ★ EVERY git query in this file goes through here, so the ENUMERATION ROOT is
# the tree this script lives in, by construction rather than by remembering.
_le_git() { leg_tree_driver_git "${REPO_ROOT}" "$@"; }

# ⚠ RESOLVED HERE, ABOVE THE ARGUMENT DISPATCH, not down in the preconditions.
# `--selftest` and `--audit-instruments` both ask git and both run BEFORE the main
# body, so an identity resolved later would leave those two modes on a bare
# `git -C` — i.e. still broken in exactly the namespace this row is about, while
# the default mode looked fixed. The loud refusal stays in the preconditions; this
# line only makes the answer available to every entry point.
leg_tree_driver_identity "${REPO_ROOT}" || true

# ── THE ONE CORRECT INSTRUMENT ────────────────────────────────────────────
# ✔MEASURED (see the instrument note above): `tr -dc '\r' | wc -c` is the only
# form that answers correctly on BOTH a CRLF file and a pure-LF file on BOTH
# Git Bash and Linux. Every CR count in this script goes through here so there
# is exactly ONE place to be right.                 CR-INSTRUMENT-QUOTED
_cr_count() { tr -dc '\r' < "$1" | wc -c | tr -d ' '; }

# ── WHAT CHECK F REFUSES — one ERE pair, shared with the `.ps1` twin ───────
# A DETECTOR VERB and a CR PATTERN on the same line, in either order. The verb
# co-requirement is what keeps prose out: `.gitattributes` discusses `$'\r'` in
# a sentence with no verb, and a Windows path `'Z:\home\rafael\test'` carries a
# bare `\r` that is not a CR pattern at all — ✔both MEASURED not to fire.
_CR_VERB="(grep|egrep|fgrep|rg|awk|sed|findstr|Select-String)"
_CR_PAT="(\\\$'(\\\\r|\\\\015)'|['\"/]\\\\r\\\$|['\"/]\\\\r['\"/])"
_CR_BLIND="(${_CR_VERB}.*${_CR_PAT}|${_CR_PAT}.*${_CR_VERB})"
# ★★ THE EXEMPTIONS, AND WHY THE MARKER EXISTS. A guard that refuses an idiom
# must still let people WRITE ABOUT the idiom — otherwise it cannot describe its
# own subject, which is the very defect it is guarding against. So the escape is
# an IN-BAND MARKER any file may use, `CR-INSTRUMENT-QUOTED`, and this script
# uses the SAME marker as everyone else rather than exempting itself by path.
# The other exemptions are MEASURED-SAFE forms, not conveniences:
#   · `git grep`/`ls-files`/`diff`/`cat-file` read BLOBS internally and never go
#     through stdio text mode — ✔probed against a purpose-built repo holding a
#     genuinely CRLF blob, and `git grep -I -l -P '\r$'` found it correctly;
#   · `grep -U`/`--binary` disables the CRLF stripping — ✔measured to return 1
#     where plain grep returns 0 (`-a` does NOT, and is deliberately absent);
#   · `sub(`/`gsub(`/`s/\r`/`-replace`/`tr -d`/`${v%$'\r'}` are CONVERTERS —
#     they REWRITE a CR rather than ask whether one is there, so they cannot
#     report a wrong answer. Four other guards strip CR this way, correctly.
_CR_EXEMPT="(git +(grep|ls-files|diff|cat-file)|-U |--binary|sub\\(|gsub\\(|s/\\\\r|-replace|tr +-d|%\\\$'|CR-INSTRUMENT-QUOTED)"
# ⚠ `.plans/**` is EXCLUDED, and this is a scope ruling rather than an oversight.
# That tree is the deferred-anchor REGISTRY: a historical record of defects,
# which necessarily QUOTES the broken instruments it recorded — ✔five rows do.
# Nothing executes it and nothing copies from it, and this repository's standing
# rule already excludes `.plans/**` from sweeps (a record must not be rewritten
# to satisfy a guard). Check F's subject is text a lane might RUN or COPY.
_CR_AUDIT_SCOPE=":(exclude).plans/"

# CR-INSTRUMENT-QUOTED:BEGIN — the help text NAMES the blind idiom so a reader
# recognises it; the marker is out here as a comment so the token never appears
# in `--help` output itself.
_usage() {
    cat <<'USAGE'
check-line-endings.sh — the LF-contract guard, and the repo's CR instrument.

  (no arguments)      Verify the whole repository: committed blobs, the index,
                      and the working tree (checks A–E), plus Check F, which
                      refuses a CR instrument that cannot see a CR. Self-tests
                      first, so it cannot pass without proving it can fail.
  --files PATH...     Ask about SPECIFIC files: "are these clean?" Works on
                      tracked, untracked and outside-the-repo paths alike.
                      Exit 0 all clean · 1 a CR was found · 2 unreadable.
  --files-from FILE   The same, one path per line (`-` reads stdin).
  --audit-instruments Run Check F alone.
  --selftest          Run the self-test alone.
  --help              This text.

★ WHY `--files` EXISTS. Until 2026-08-27 this guard answered exactly one
question — "is the whole repo clean?" — and took no arguments. A lane holding
thirteen specific files had NO entry point, so it hand-rolled `awk '/\r$/'`,
which reports a clean tree over a fully CRLF file, and certified all thirteen
while measuring nothing. The guard was not missing; its REACH was.
                                                  CR-INSTRUMENT-QUOTED
USAGE
}
# CR-INSTRUMENT-QUOTED:END

# ── `--files`: the per-file question, answered with the one correct tool ──
# Fail-loud by construction: a path that does not exist, is a directory, or
# cannot be read is exit 2 (UNMEASURED), never a silent skip that reads as a
# pass. That distinction is the whole point — a skip shaped like a pass is the
# failure class this guard exists to refuse.
_run_files_mode() {
    local _rc=0 _n=0 _bad=0 _unmeasured=0 _p _abs _cr
    for _p in "$@"; do
        _n=$((_n + 1))
        case "${_p}" in /*|[A-Za-z]:[/\\]*) _abs="${_p}" ;; *) _abs="${INVOKED_FROM}/${_p}" ;; esac
        if [[ -d "${_abs}" ]]; then
            echo "  UNMEASURED  ${_p} — is a directory, not a file" >&2
            _unmeasured=$((_unmeasured + 1)); continue
        fi
        if [[ ! -e "${_abs}" ]]; then
            echo "  UNMEASURED  ${_p} — no such file" >&2
            _unmeasured=$((_unmeasured + 1)); continue
        fi
        if [[ ! -r "${_abs}" ]]; then
            echo "  UNMEASURED  ${_p} — not readable" >&2
            _unmeasured=$((_unmeasured + 1)); continue
        fi
        _cr="$(_cr_count "${_abs}")"
        if [[ "${_cr}" -eq 0 ]]; then
            echo "  LF          ${_p}"
        else
            echo "  CR  ${_cr}    ${_p}"
            _bad=$((_bad + 1))
        fi
    done
    if [[ "${_n}" -eq 0 ]]; then
        echo "line-endings: FAIL — --files was given no paths. Refusing to report a" >&2
        echo "  pass over an empty list; that is a vacuous green, not a clean tree." >&2
        return 2
    fi
    if [[ "${_unmeasured}" -gt 0 ]]; then
        echo "line-endings: FAIL — ${_unmeasured} of ${_n} path(s) could NOT be measured (above)." >&2
        echo "  A guard that cannot read a file must say so, never imply it was clean." >&2
        _rc=2
    fi
    if [[ "${_bad}" -gt 0 ]]; then
        echo "line-endings: FAIL — ${_bad} of ${_n} file(s) carry a CR." >&2
        echo "  Convert with \`tr -d '\\r' < f > f.tmp && mv f.tmp f\`, or see --help." >&2
        [[ "${_rc}" -eq 0 ]] && _rc=1
    fi
    [[ "${_rc}" -eq 0 ]] && echo "line-endings: OK (${_n} file(s), none carries a CR; measured with tr -dc)"
    return "${_rc}"
}

# ── Check F: refuse a CR instrument that cannot see a CR ───────────────────
_run_instrument_audit() {
    local _hits _marked _raw _h _hf _rest _hl
    _raw="$(_le_git grep -n -I -E "${_CR_BLIND}" -- . "${_CR_AUDIT_SCOPE}" 2>/dev/null \
             | grep -v -E "${_CR_EXEMPT}" || true)"
    # ── REGION EXEMPTION, and it is why this guard can document itself ──────
    # ★★ A same-line marker is right for a one-off, and USELESS for a paragraph
    # that explains the trap — which is exactly the text most worth writing.
    # ✔MEASURED while building this: the corrected instrument note at the top of
    # this file tripped Check F TWELVE times, and per-line markers would have put
    # the token on twelve consecutive lines of prose. A guard whose escape hatch
    # is unusable for documentation punishes the documentation that prevents the
    # defect. So a BEGIN/END region marks a whole block at once.
    # ⓘ Scanned per offending FILE rather than by pairing markers globally: the
    # hit list is short, and reading the file itself cannot get the pairing wrong.
    _hits=""
    while IFS= read -r _h; do
        [[ -z "${_h}" ]] && continue
        _hf="${_h%%:*}"; _rest="${_h#*:}"; _hl="${_rest%%:*}"
        if [[ -f "${_hf}" ]] && awk -v L="${_hl}" '
                /CR-INSTRUMENT-QUOTED:BEGIN/ { r = 1 }
                { if (r && NR == L) found = 1 }
                /CR-INSTRUMENT-QUOTED:END/   { r = 0 }
                END { exit !found }' "${_hf}" 2>/dev/null; then
            continue
        fi
        _hits+="${_h}"$'\n'
    done <<< "${_raw}"
    _hits="${_hits%$'\n'}"
    # A census of the escape hatch on every run, so silencing trends are visible
    # rather than accumulating unseen — the ratchet shape used elsewhere here.
    _marked="$(_le_git grep -c -I -e 'CR-INSTRUMENT-QUOTED' -- . "${_CR_AUDIT_SCOPE}" 2>/dev/null | wc -l | tr -d ' ')"
    if [[ -n "${_hits}" ]]; then
        echo "line-endings: FAIL — a CR instrument that cannot see a CR:" >&2
        echo "" >&2
        printf '%s\n' "${_hits}" | sed 's/^/  /' >&2
        # CR-INSTRUMENT-QUOTED:BEGIN — the refusal must SHOW the spellings it
        # refuses, or the reader cannot recognise their own line in it.
        cat >&2 <<'FIXF'

These spellings do not measure what they appear to measure on this host
(both directions are MEASURED in the note at the top of this script):
  · `grep -c $'\r'`  returns the LINE COUNT — of a CLEAN file too;
  · `awk '/\r$/'`, `sed -n '/\r/p'`, `grep -P '\r$'` return 0 over a
    file that is entirely CRLF, because the reader strips the CR first.
Use instead:
  (a) `scripts/check-line-endings/check-line-endings.sh --files PATH...`
      — the supported way to ask about specific files; or
  (b) `tr -dc '\r' < f | wc -c`  (expect 0) if you must inline it; or
  (c) `git grep`/`git ls-files --eol`, which read blobs and are unaffected.
If the line is DOCUMENTATION that quotes the idiom on purpose, put the
marker CR-INSTRUMENT-QUOTED on it — the same escape this script uses for
its own warnings, so the guard can describe its own subject.
FIXF
        # CR-INSTRUMENT-QUOTED:END
        return 1
    fi
    echo "line-endings: Check F OK (no blind CR instrument; ${_marked} file(s) carry the quoted-idiom marker)"
    return 0
}

# ── THE SELF-TEST — it synthesizes the NEGATIVE, which is the only direction ──
# ★★ A fixture that feeds this guard a CLEAN file and asserts a pass stays green
# forever after the guard stops working. So every arm below is built to FAIL if
# the instrument is blind: the CR control is constructed with `printf` and
# verified by `od` to hold exactly one 0x0D before it is ever used as a control,
# and the blind-idiom arms assert the detector FIRES.
#
# ⚠ THE SYNTHETIC BLIND LINES ARE ASSEMBLED FROM FRAGMENTS AT RUN TIME, never
# written literally. A literal `grep -c $'\r'`   CR-INSTRUMENT-QUOTED
# sitting in this file would be a
# true positive for Check F scanning this very script — the guard would refuse
# itself. Building the string at run time keeps the source honest AND still
# exercises the detector against a genuinely blind line.
_run_selftest() {
    local _t _fail=0 _q="'" _d='$' _b='\' _crlf _lf _n
    _t="$(mktemp -d 2>/dev/null)" || { echo "line-endings: FAIL — selftest cannot mktemp -d" >&2; return 2; }
    # Never write inside the repository, compared by RESOLVED prefix (not substring).
    local _tr; _tr="$(cd "${_t}" && pwd -P)"
    case "${_tr}/" in "${REPO_ROOT}/"*) echo "line-endings: FAIL — selftest temp dir '${_tr}' is inside the repo" >&2; return 2 ;; esac
    trap 'rm -rf "${_t}"' RETURN

    _crlf="${_t}/ctl_crlf.txt"; _lf="${_t}/ctl_lf.txt"
    printf 'a\r\nb\n' > "${_crlf}"
    printf 'a\nb\n'   > "${_lf}"
    # ARM 0 — prove the CONTROL before trusting any verdict taken with it.
    _n="$(od -An -tx1 -v < "${_crlf}" | tr ' ' '\n' | grep -c '^0d$' || true)"
    [[ "${_n}" -eq 1 ]] || { echo "line-endings: FAIL — selftest control is not one CR (od says ${_n})" >&2; _fail=1; }
    _n="$(od -An -tx1 -v < "${_lf}" | tr ' ' '\n' | grep -c '^0d$' || true)"
    [[ "${_n}" -eq 0 ]] || { echo "line-endings: FAIL — selftest LF twin carries a CR (od says ${_n})" >&2; _fail=1; }

    # ARM 1 — THE NEGATIVE: the counter must SEE the CR.
    [[ "$(_cr_count "${_crlf}")" -eq 1 ]] || { echo "line-endings: FAIL — selftest: _cr_count reported no CR on the CRLF control. The instrument is blind." >&2; _fail=1; }
    # ARM 2 — and must not invent one.
    [[ "$(_cr_count "${_lf}")" -eq 0 ]] || { echo "line-endings: FAIL — selftest: _cr_count invented a CR on the pure-LF control." >&2; _fail=1; }
    # ARM 3/4 — `--files` must RED on the dirty control and pass on the clean one.
    if _run_files_mode "${_crlf}" >/dev/null 2>&1; then
        echo "line-endings: FAIL — selftest: --files reported success over a file holding a CR." >&2; _fail=1
    fi
    if ! _run_files_mode "${_lf}" >/dev/null 2>&1; then
        echo "line-endings: FAIL — selftest: --files refused a pure-LF file." >&2; _fail=1
    fi
    # ARM 5 — a missing path is UNMEASURED (exit 2), never a quiet pass.
    _run_files_mode "${_t}/does-not-exist" >/dev/null 2>&1
    [[ $? -eq 2 ]] || { echo "line-endings: FAIL — selftest: a missing path did not exit 2." >&2; _fail=1; }

    # ARM 6 — the Check F detector must FIRE on genuinely blind lines...
    {
        printf 'n=%s(grep -c %s%s%sr%s f)\n' "${_d}" "${_d}" "${_q}" "${_b}" "${_q}"
        printf 'awk %s/%sr%s/ {bad++}%s f\n'  "${_q}" "${_b}" "${_d}" "${_q}"
        printf 'sed -n %s/%sr/p%s f | wc -l\n' "${_q}" "${_b}" "${_q}"
    } > "${_t}/blind.txt"
    _n="$(grep -c -E "${_CR_BLIND}" "${_t}/blind.txt" || true)"
    [[ "${_n}" -eq 3 ]] || { echo "line-endings: FAIL — selftest: Check F saw ${_n}/3 blind instruments. The detector is broken." >&2; _fail=1; }
    # ...and must NOT fire on the measured-safe forms.
    {
        printf 'tr -dc %s%sr%s < f | wc -c\n'        "${_q}" "${_b}" "${_q}"
        printf 'git grep -I -l -P %s%sr%s%s HEAD\n'  "${_q}" "${_b}" "${_d}" "${_q}"
        printf 'sub(/%sr%s/, "", line)\n'            "${_b}" "${_d}"
        printf 'grep -U -c "%sCR" f\n'               "${_d}"
    } > "${_t}/safe.txt"
    _n="$(grep -E "${_CR_BLIND}" "${_t}/safe.txt" 2>/dev/null | grep -v -c -E "${_CR_EXEMPT}" || true)"
    [[ "${_n}" -eq 0 ]] || { echo "line-endings: FAIL — selftest: Check F fired on ${_n} SAFE form(s)." >&2; _fail=1; }
    # ARM 7 — the marker must exempt, or nobody can document the idiom.
    printf 'awk %s/%sr%s/%s f   CR-INSTRUMENT-QUOTED\n' "${_q}" "${_b}" "${_d}" "${_q}" > "${_t}/marked.txt"
    _n="$(grep -E "${_CR_BLIND}" "${_t}/marked.txt" 2>/dev/null | grep -v -c -E "${_CR_EXEMPT}" || true)"
    [[ "${_n}" -eq 0 ]] || { echo "line-endings: FAIL — selftest: the CR-INSTRUMENT-QUOTED marker did not exempt a documented idiom." >&2; _fail=1; }

    # ARM 8 — ONE ROOT (D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE).
    # Every arm above judges BYTES; this one judges WHICH TREE the bytes came
    # from. The twin of the `.ps1` sibling's arm 8, kept here so the pairing is by
    # BEHAVIOUR and not by existence: the defect this pins is far worse in that
    # shell, and a property held in only one twin is how the last four pairing
    # defects in this repository started.
    local _sr _st
    _sr="$(pwd -P)"
    _st="$(_le_git rev-parse --show-toplevel 2>/dev/null)"
    if [[ -z "${_st}" ]]; then
        echo "line-endings: FAIL — selftest: git will not name a top level; the guard cannot say which tree it is judging." >&2; _fail=1
    elif [[ "$(cd "${_st}" 2>/dev/null && pwd -P)" != "${_sr}" ]]; then
        echo "line-endings: FAIL — selftest: git enumerates from '${_st}' while this shell reads at '${_sr}'." >&2; _fail=1
    fi
    # ...and the identity must be the one THIS script's tree owns, not whichever
    # tree the caller happened to be standing in.
    if [[ "$(cd "${REPO_ROOT}" && pwd -P)" != "${_sr}" ]]; then
        echo "line-endings: FAIL — selftest: the guard is reading at '${_sr}' but lives in '${REPO_ROOT}'." >&2; _fail=1
    fi

    [[ "${_fail}" -eq 0 ]] || { echo "line-endings: FAIL — the SELF-TEST failed (above). This guard cannot be trusted until it passes; do not silence it." >&2; return 2; }
    return 0
}

case "${1:-}" in
    --help|-h)           _usage; exit 0 ;;
    --files)             shift; _run_files_mode "$@"; exit $? ;;
    --files-from)
        [[ $# -ge 2 ]] || { echo "line-endings: FAIL — --files-from needs a FILE (or -)" >&2; exit 2; }
        _ff="$2"
        if [[ "${_ff}" == "-" ]]; then mapfile -t _ffpaths
        elif [[ -r "${_ff}" ]]; then mapfile -t _ffpaths < "${_ff}"
        else echo "line-endings: FAIL — cannot read path list '${_ff}'" >&2; exit 2; fi
        _kept=(); for _l in "${_ffpaths[@]}"; do [[ -n "${_l}" ]] && _kept+=("${_l}"); done
        _run_files_mode "${_kept[@]+"${_kept[@]}"}"; exit $? ;;
    --audit-instruments) cd "${REPO_ROOT}"; _run_instrument_audit; exit $? ;;
    --selftest)          cd "${REPO_ROOT}"; _run_selftest; exit $? ;;
    "")                  : ;;
    *) echo "line-endings: FAIL — unknown argument '$1' (see --help)" >&2; exit 2 ;;
esac

cd "${REPO_ROOT}"

# ── fail-closed preconditions ─────────────────────────────────────────────
# A guard that cannot run must FAIL, never skip. `git rev-parse` proves both
# that git exists and that this really is a work tree with a resolvable HEAD.
if ! command -v git >/dev/null 2>&1; then
    echo "line-endings: FAIL — git is not on PATH. This guard reads BLOBS, so it" >&2
    echo "  cannot fall back to the working tree (a CRLF checkout would false-red" >&2
    echo "  and an LF checkout would false-green). Refusing to report a pass." >&2
    exit 2
fi
# ★ THE IDENTITY WAS RESOLVED ABOVE, IN WHICHEVER NAMESPACE CAN SEE THE TREE. This
# refusal replaces a bare `git rev-parse HEAD`, which answered "no" for every lane
# worktree reached from WSL while the tree was perfectly readable.
if [[ -z "${LEG_TREE_DRIVER_SHA:-}" ]]; then
    echo "line-endings: FAIL — HEAD does not resolve; this is not a git work tree" >&2
    echo "  with a commit. Refusing to report a pass over a tree it cannot read." >&2
    echo "  (tried a plain \`git -C\`, then the gitdir named by ${REPO_ROOT}/.git)" >&2
    exit 2
fi
if ! _le_git rev-parse --verify --quiet HEAD >/dev/null; then
    echo "line-endings: FAIL — HEAD does not resolve; this is not a git work tree" >&2
    echo "  with a commit. Refusing to report a pass over a tree it cannot read." >&2
    exit 2
fi

# ── THE ENUMERATION ROOT AND THE READ ROOT MUST BE THE SAME ROOT ──────────
# ⓷ of D-SCRIPT-GUARDS-ASK-GIT-FROM-THE-LANE-WORKTREE: PROVEN, not assumed. This
# shell has ONE working directory, so `cd` moves the reads and the git together —
# unlike the PowerShell twin, where `Set-Location` moves only one of the two and
# the split is invisible until a path exists at exactly one root. The assertion is
# carried in BOTH twins anyway: a property checked in one sibling and not the
# other is how every pairing defect in this repository has started.
_enum_root="$(_le_git rev-parse --show-toplevel 2>/dev/null)"
_read_root="$(pwd -P)"
if [[ -z "${_enum_root}" ]]; then
    echo "line-endings: FAIL — git will not name a top level for ${REPO_ROOT}." >&2
    echo "  Refusing to report a verdict about a tree it cannot locate." >&2
    exit 2
fi
if [[ "$(cd "${_enum_root}" 2>/dev/null && pwd -P)" != "${_read_root}" ]]; then
    echo "line-endings: FAIL — the enumeration root and the read root are NOT the same root." >&2
    echo "    git enumerates from : ${_enum_root}" >&2
    echo "    this shell reads at : ${_read_root}" >&2
    echo "  Every path below would be listed from one tree and read from the other." >&2
    exit 2
fi

# ── SELF-TEST FIRST — this entry cannot pass without proving it can fail ──
# ★ The same arrangement `orphan_tests_guard` uses, and for the same reason: a
# guard wired into ctest is only evidence if it is still capable of redding. The
# arms below construct a file that genuinely holds a CR and assert this script
# SEES it, so a regression that blinds the instrument reds here instead of
# quietly reporting a clean tree forever (D-TEST-NONFATAL-GUARD-DEGRADES-TO-A-VACUOUS-PASS).
_run_selftest || exit 2

# ── POSITIVE CONTROL ──────────────────────────────────────────────────────
# The offender scan below PASSES by returning nothing, which is exactly what a
# BROKEN scan also returns. So first prove the instrument answers: same `git
# grep`, same `-I -l -P`, same ref, against a pattern every non-empty text blob
# matches. MEASURED at the commit that added this: 2214 blobs in HEAD, 2214 in
# the index, over 2238 tracked paths. The floor sits far below that so ordinary
# churn never trips it — it catches COLLAPSE (a dead PCRE engine, an unreadable
# ref, a moved tree), not drift. Fix the scan; never lower the floor.
SCAN_FLOOR=1500
_control_head="$(_le_git grep -I -l -P '^.' HEAD 2>/dev/null | wc -l | tr -d ' ')"
_control_index="$(_le_git grep --cached -I -l -P '^.' 2>/dev/null | wc -l | tr -d ' ')"
_control_failed=0
for _pair in "HEAD:${_control_head}" "index:${_control_index}"; do
    _what="${_pair%%:*}"; _n="${_pair#*:}"
    if [[ "${_n}" -lt "${SCAN_FLOOR}" ]]; then
        echo "line-endings: FAIL — the ${_what} scan saw only ${_n} text blobs, below its floor of ${SCAN_FLOOR}." >&2
        echo "  This does NOT mean the tree is clean — it means the SCAN collapsed" >&2
        echo "  (git built without PCRE for -P, an unresolvable ref, or a moved tree)." >&2
        echo "  A guard that reports success over what it could not read is the exact" >&2
        echo "  failure class this repository keeps anchoring. Refusing to pass." >&2
        _control_failed=1
    fi
done
[[ "${_control_failed}" -eq 0 ]] || exit 2

# ── the check ─────────────────────────────────────────────────────────────
# `-I` skips blobs git detects as BINARY, which is correct (a `.bin` #embed
# fixture legitimately carries 0x0D) but leaves one blind spot: a TEXT source
# that git happens to detect as binary would be skipped silently. Check C below
# closes exactly that hole for the extensions the pin claims to cover.
#
# ★ THERE IS DELIBERATELY NO `eol=crlf` ESCAPE HATCH, and the reason is a
# MEASURED property of git rather than a policy choice. Probed here 2026-08-06:
# a file declared `text eol=crlf`, written CRLF on disk and staged, lands in the
# index as `i/lf` with ZERO CR in the blob — `eol=` governs the SMUDGE
# (checkout) direction only; the clean filter always stores LF for a `text`
# file. So "declare eol=crlf" could never make a CR-carrying blob legitimate:
# it would only ever have exempted a LEGACY blob committed before its own
# declaration existed, while reading like a sanctioned way to store CRLF. An
# escape hatch that cannot do what it advertises is worse than none.
# The REAL exemption is `binary` / `-text`, and this guard already honours it
# through `-I` — that is how `examples/**/*.bin` keeps its 0x0D bytes.
_offenders_head="$(_le_git grep -I -l -P '\r$' HEAD 2>/dev/null | sed 's|^HEAD:||')"
_offenders_index="$(_le_git grep --cached -I -l -P '\r$' 2>/dev/null)"

# ── STALE-CHECKOUT DETECTION — the git history here may not describe this tree ──
#
# ★ MEASURED 2026-08-06 (TF-C123), and it cost a false red on a green gate: the
#   WSL leg of the 3-leg gate rsyncs the Windows tree with `--exclude '.git/'`
#   (D-GATE-WSL-SYNC-LEAVES-GIT-HEAD-STALE). The CONTENT is current; the `.git`
#   directory is whatever that checkout last fetched. So checks A and C — which
#   read HEAD and the index — answer about a DIFFERENT COMMIT than the files on
#   disk, and reported the ten pre-normalisation CRLF blobs as live violations
#   while every one of them was LF on disk.
#
# ⇒ A guard that cannot tell whose history it is reading must SAY SO, not
#   convict. The detection is exact rather than heuristic: if a file that HEAD
#   or the index calls CRLF carries NO CR on disk, then the recorded history is
#   not the history of this working tree. A genuine violation has CR in BOTH.
#
# ⚠ Check D (worktree rewritten under a pin) is UNAFFECTED — it reads the disk —
#   so it keeps running. Only the history-scoped checks are suspended, and the
#   run is still a FAILURE if check D finds anything.
_stale_evidence=""
_all_history_offenders="$(printf '%s\n%s\n' "${_offenders_head}" "${_offenders_index}" | sort -u)"
while IFS= read -r _f; do
    [[ -z "${_f}" ]] && continue
    [[ -f "${_f}" ]] || continue
    if [[ "$(tr -dc '\r' < "${_f}" | wc -c | tr -d ' ')" -eq 0 ]]; then
        _stale_evidence="${_f}"
        break
    fi
done <<< "${_all_history_offenders}"

if [[ -n "${_stale_evidence}" ]]; then
    echo "line-endings: HISTORY SCAN SKIPPED — this work tree's .git does not describe its files." >&2
    echo "    evidence: '${_stale_evidence}' is recorded as CRLF in HEAD/index but carries ZERO CR on disk." >&2
    echo "    HEAD here is $(_le_git rev-parse --short HEAD 2>/dev/null || echo '<unresolved>')." >&2
    echo "    This is the expected shape of a tree synced WITHOUT .git (see the WSL leg of the" >&2
    echo "    3-leg gate, D-GATE-WSL-SYNC-LEAVES-GIT-HEAD-STALE). Convicting on that history would" >&2
    echo "    report violations belonging to another commit — so checks A and C are suspended here." >&2
    echo "    Run this guard on the AUTHORITATIVE checkout for history hygiene." >&2
    _offenders_head=""
    _offenders_index=""
    _skip_binary_check=1
fi

_report=""
_check_set() {
    local _what="$1"; shift
    local _list="$1"
    [[ -z "${_list}" ]] && return 0
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  ${_what}: ${_f}"$'\n'
    done <<< "${_list}"
}
_check_set "committed (HEAD)" "${_offenders_head}"
_check_set "staged (index)"   "${_offenders_index}"

# ── ONE `git ls-files --eol` read, shared by checks C/D/E, WITH A FLOOR ───────
# Checks D and E both answer from this table, and a table that comes back EMPTY
# would make both of them report nothing — the "guard that passes over what it
# never read" shape this file already fails closed against on the history side.
# So the row count carries the same floor as the positive control above.
# ✔MEASURED 2026-08-10: 2,265 tracked paths.
_eol_rows="$(_le_git ls-files --eol 2>/dev/null)"
_eol_row_count="$(printf '%s' "${_eol_rows}" | grep -c . || true)"
if [[ "${_eol_row_count}" -lt "${SCAN_FLOOR}" ]]; then
    echo "line-endings: FAIL — \`git ls-files --eol\` returned only ${_eol_row_count} rows, below its floor of ${SCAN_FLOOR}." >&2
    echo "  Checks D and E answer from that table, so an empty one makes BOTH report a clean" >&2
    echo "  worktree over files they never looked at. Refusing to pass; fix the scan." >&2
    exit 2
fi

# ── Check C: the pin's own glob set must contain no binary-detected blob ──
# Without this, a source file git mis-detects as binary is skipped by `-I` above
# and this guard would report a clean tree over a file it never opened.
# ⚠ Suspended alongside checks A/C when the history does not describe this tree:
# `i/-text` is an INDEX fact, so a stale index answers about a different commit.
_binary_in_pinned=""
[[ -n "${_skip_binary_check:-}" ]] || \
_binary_in_pinned="$(_le_git ls-files --eol -- '*.c' '*.h' '*.cpp' '*.hpp' '*.cmake' 'CMakeLists.txt' '*/CMakeLists.txt' '*.md' 'VERSION' 2>/dev/null \
    | awk '$1 == "i/-text" { sub(/^[^\t]*\t/, ""); print }')"
if [[ -n "${_binary_in_pinned}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  staged (index): binary-detected inside the eol=lf pin, so the blob scan above never opened it: ${_f}"$'\n'
    done <<< "${_binary_in_pinned}"
fi

# ── Check D: a PINNED file's WORKING TREE must not have been rewritten ────
# ★ THE PIN'S OWN BLIND SPOT, and the sharpest lesson in the anchor this guard
# closes. `.gitattributes` normalises on `git add`, so once a file is pinned
# `eol=lf`, a tool that rewrites it CRLF ON DISK changes every byte of the file
# while changing NOTHING git will ever show you as a change. That is not
# hypothetical: it is what happened to the `src/dss-config/**` files when an
# agent edited 159 fixtures through Python `pathlib.write_text`.
# ⚠ MEASURED EXACTLY, because the imprecise version of this claim is easy to
# repeat and wrong: rewrite a pinned `.cpp` to CRLF on disk and
#   · `git status` DOES report ` M <path>` (twice running — it does not settle);
#   · `git diff` and `git diff --stat` are EMPTY;
#   · `git add` + `git diff --cached` are EMPTY too, so the rewrite can never
#     be committed and simply LIVES in the working tree indefinitely.
# So the file is flagged as changed and then no view will tell you WHAT changed
# — an author who checks `git diff` on that `M` concludes it is noise. Checks
# A–C cannot see it BY CONSTRUCTION: the blob is fine; the working tree is not.
#
# `git ls-files --eol` reports BOTH sides — `i/` (index) and `w/` (worktree). A
# file DECLARED `eol=lf` whose worktree form is `crlf` or `mixed` was rewritten
# by something AFTER checkout, on any host and regardless of `core.autocrlf`,
# because with `eol=lf` declared a checkout always produces LF.
#
# ⚠ Deliberately a CLOSED set (`w/crlf`, `w/mixed`) rather than "anything that
# is not `w/lf`": an unmeasured `w/` state — a sparse or partial checkout, a
# file not materialised — must not be guessed at and turned into a red.
# MEASURED when this landed: 2166 tracked files declare `eol=lf`, and ZERO have
# a non-LF worktree form.
_worktree_rewritten="$(printf '%s\n' "${_eol_rows}" | awk -F'\t' '
    NF >= 2 && index($1, "eol=lf") > 0 {
        split($1, f, /[ \t]+/)
        if (f[2] == "w/crlf" || f[2] == "w/mixed") print $2
    }')"
if [[ -n "${_worktree_rewritten}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  working (tracked, eol=lf pinned): rewritten to CRLF on disk — git diff shows NOTHING to review: ${_f}"$'\n'
    done <<< "${_worktree_rewritten}"
fi

# ── Check E: THE UNSTAGED WORKING TREE — the tier the other checks cannot see ──
# ★★ THE BLIND SPOT, ✔MEASURED 2026-08-10 on this tree. Checks A and B read
# BLOBS (HEAD and the index); check C is an index fact; check D reads the disk but
# ONLY for files that DECLARE `eol=lf`. So a CRLF introduced into a working file
# that is neither staged nor covered by the pin was invisible to every tier — and
# "before commit" is exactly when a line-ending mistake is cheap to fix and the
# only moment this guard can prevent rather than diagnose.
# The gap is not theoretical arithmetic: of 2,265 tracked paths, 2,186 declare
# `eol=lf` and 55 are tracked TEXT with NO such declaration (the rest are empty or
# binary). `.gitattributes` carries no `* text=auto`, and this workstation reports
# `core.autocrlf=false`, so for those 55 git performs NO normalisation on `git add`
# — a CRLF rewrite of any one of them LANDS CRLF IN THE COMMIT. Same for a NEW
# untracked file outside the pin.
# ⚠ `core.autocrlf=true` (the Git-for-Windows INSTALLER DEFAULT) makes a CRLF
# WORKING COPY of an unpinned text file the LEGITIMATE result of checkout, not a
# rewrite. Convicting there would be a false red on a correctly-configured host,
# so E1 states the situation instead of convicting — and E2 (untracked files,
# which were never checked out) keeps convicting either way.
_autocrlf="$(_le_git config core.autocrlf 2>/dev/null || true)"
[[ -n "${_autocrlf}" ]] || _autocrlf='<unset>'
# E1 — TRACKED, text, NOT covered by an `eol=lf` pin, CRLF/mixed on disk.
# `i/-text` is binary (its 0x0D is legitimate) and `i/none` is empty; both are
# excluded by NAME rather than by "anything else", for the same reason check D
# uses a closed `w/` set: an unmeasured state must not be guessed into a red.
_worktree_unpinned="$(printf '%s\n' "${_eol_rows}" | awk -F'\t' '
    NF >= 2 && index($1, "eol=lf") == 0 {
        split($1, f, /[ \t]+/)
        if (f[1] == "i/-text" || f[1] == "i/none") next
        if (f[2] == "w/crlf" || f[2] == "w/mixed") print $2
    }')"
if [[ -n "${_worktree_unpinned}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        if [[ "${_autocrlf}" == "true" ]]; then
            echo "line-endings: NOTE — '${_f}' is CRLF on disk and carries NO eol=lf pin, but core.autocrlf=true," >&2
            echo "    so a CRLF checkout is the expected result here and this is NOT convicted. Add an eol=lf pin" >&2
            echo "    for its extension if this repo should own its bytes regardless of a host's git config." >&2
        else
            _report+="  working (tracked, NOT covered by an eol=lf pin): CRLF on disk and core.autocrlf=${_autocrlf}, so \`git add\` will NOT normalise it — this WILL land CRLF in the commit: ${_f}"$'\n'
        fi
    done <<< "${_worktree_unpinned}"
fi
# E2 — UNTRACKED (not ignored), text, NOT covered by an `eol=lf` pin, has a CR.
# A pinned untracked file is deliberately NOT reported: its clean filter
# normalises it on `git add`, so it cannot land CRLF, and a guard that reds on a
# state git is about to fix teaches people to ignore it.
# ⚠ `tr -dc '\r' | wc -c`, never `grep -c $'\r'` (which returns the LINE COUNT,
# on a clean file too) and never `awk '/\r$/'`   CR-INSTRUMENT-QUOTED
# (which returns 0 over a fully
# CRLF file). Both traps are measured in the instrument note at the top of this
# file, and Check F refuses them repo-wide.        CR-INSTRUMENT-QUOTED
_worktree_untracked=""
while IFS= read -r _f; do
    [[ -z "${_f}" ]] && continue
    [[ -f "${_f}" ]] || continue
    # BINARY skip, using grep's own detection (the same family as `git grep -I`)
    # rather than a hand-rolled NUL scan.
    grep -qI . -- "${_f}" 2>/dev/null || continue
    # An explicit `binary` / `-text` declaration is an exemption, exactly as `-I`
    # honours it for the blob tiers.
    case "$(_le_git check-attr text -- "${_f}" 2>/dev/null)" in *": text: unset") continue;; esac
    case "$(_le_git check-attr eol  -- "${_f}" 2>/dev/null)" in *": eol: lf")     continue;; esac
    if [[ "$(tr -dc '\r' < "${_f}" | wc -c | tr -d ' ')" -ne 0 ]]; then
        _worktree_untracked+="${_f}"$'\n'
    fi
done <<< "$(_le_git ls-files --others --exclude-standard 2>/dev/null)"
if [[ -n "${_worktree_untracked}" ]]; then
    while IFS= read -r _f; do
        [[ -z "${_f}" ]] && continue
        _report+="  working (untracked, not yet added): carries CR and NO eol=lf pin covers it, so \`git add\` will NOT normalise it — this WILL land CRLF in the commit: ${_f}"$'\n'
    done <<< "${_worktree_untracked}"
fi

# ── Check F: the INSTRUMENT tier — a CR detector that cannot detect a CR ──
# ★★ THE TIER CHECKS A–E CANNOT REACH, and the one that let this whole class
# through. A–E judge BYTES: what is in a blob, an index, a working file. Check F
# judges the MEASUREMENT — text that will be RUN or COPIED to answer "is there a
# CR here?" and that answers wrongly on this host. ✔MEASURED 2026-08-27: a lane
# certified thirteen files "pure LF" with `awk '/\r$/'`,  CR-INSTRUMENT-QUOTED
# which returns 0 over a
# file that is entirely CRLF. Every byte-tier check was green throughout, and
# correctly so — the tree WAS clean. The claim about it was worthless.
# ⇒ A guard cannot only ask whether the tree is clean; it must also refuse the
#   instruments that will be used to answer that question next time.
_instrument_rc=0
_run_instrument_audit || _instrument_rc=$?

if [[ -z "${_report}" && "${_instrument_rc}" -eq 0 ]]; then
    if [[ -n "${_skip_binary_check:-}" ]]; then
        # Say plainly what was NOT checked. A guard reporting "OK" while having
        # silently skipped half its checks is the self-blind-instrument shape
        # this repo keeps finding — the message must not outrun the evidence.
        echo "line-endings: OK (WORKTREE ONLY — the history scan was SKIPPED, see above; the ${_control_head} HEAD / ${_control_index} index blobs were NOT judged; ${_eol_row_count} working-tree paths were)"
    else
        # ★ The summary NAMES EVERY TIER it judged. A guard that says "OK" without
        # saying over what invites the reader to assume it covered the tier they
        # care about — and for four tiers of this guard's life, one of them
        # (the unstaged working tree) was the tier it did NOT cover.
        echo "line-endings: OK (committed ${_control_head} + staged ${_control_index} text blobs, working tree ${_eol_row_count} tracked paths + untracked, core.autocrlf=${_autocrlf}; none carries CR)"
    fi
    exit 0
fi

# Check F failed on its own: it has already printed its finding and its fix, and
# the byte tiers found nothing. Do not head that with an "LF contract violated"
# banner over an empty list — a report whose heading outruns its evidence is the
# same self-blindness this guard is about.
if [[ -z "${_report}" ]]; then
    exit 1
fi

echo "line-endings: FAIL — the LF contract is violated (tier named per line):"
echo ""
printf '%s' "${_report}"
echo ""
echo "Fix:"
echo "  ★ A \`working (...)\` line is the CHEAP one: the bytes are only on your disk,"
echo "    nothing is committed yet, and converting the file to LF right now costs a"
echo "    single command. A \`committed (HEAD)\` line is the same defect after it"
echo "    became history. Fix the working tier BEFORE you stage."
echo "  (a) convert the file to LF — \`sed 's/\\r\$//' f > f.tmp && mv f.tmp f\` —"
echo "      and commit that rewrite ON ITS OWN, never beside real changes; a"
echo "      whole-file EOL diff sitting next to logic is unreviewable, which is"
echo "      how the original six got in unnoticed; OR"
echo "  (b) add an \`eol=lf\` pin for its extension in \`.gitattributes\` so a"
echo "      tool's platform default can never decide this repo's bytes again"
echo "      (\`pathlib.write_text\` on Windows is the measured culprit); OR"
echo "  (c) if the file genuinely REQUIRES its 0x0D bytes preserved, declare it"
echo "      \`binary\` in \`.gitattributes\` (the \`examples/**/*.bin\` precedent)."
echo "      Do NOT reach for \`eol=crlf\`: MEASURED — a \`text eol=crlf\` file"
echo "      still stages as an LF blob, so it cannot make this red go away."
echo ""
echo "See D-REPO-GITATTRIBUTES-PINS-EOL-FOR-CONFIGS-BUT-NOT-FOR-SOURCES in"
echo ".plans/_deferred-anchor-registry*.md for why this is machine-checked."
exit 1
