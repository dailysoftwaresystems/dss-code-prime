#!/usr/bin/env bash
# test-leg-tree.sh -- the refusals and the git-environment rules of scripts/leg-tree/leg-tree.sh.
#
# ⓘ No purpose declaration of its own: `check-scripts-index` lets a sibling omit it (its rule
# 6), and this file is the primary's test, not a tool of its own.
#
# ★★★ WHAT IS PINNED, EACH MEASURED BEFORE THE FIX (P66 lane rr, 2026-09-15, fixtures only):
#   * `cd ""` is a silent no-op on WSL bash 5.2.21 and dash, a refusal on Git Bash 5.3.15.
#     So `leg_tree_restore ""`, `leg_tree_restore` with no argument, and `leg-tree.sh restore`
#     run from INSIDE a repository wiped THAT repository's dirty paths on WSL bash and dash --
#     "leg-tree: restored  -- 2 dirty path(s) discarded", rc=0.
#   * `leg_tree_driver_git "" status --porcelain` answered with the CALLER'S cwd repository.
#   * with the caller exporting ANOTHER repository's GIT_DIR + GIT_WORK_TREE,
#     `leg_tree_restore <named clone>` left the named clone dirty and wiped the other one.
#   * `leg_tree_owning_root` answered `<tree>/scripts/probe` under GIT_DIR, the other
#     repository under GIT_DIR + GIT_WORK_TREE, and the OUTER checkout for an untracked DSS
#     tree copied inside it; `leg_tree_driver_git ls-files` listed a decoy repository's file
#     under GIT_INDEX_FILE and under GIT_DIR.
#
# ★★ EVERY STEERING ARM PROVES ITS NEGATIVE FIRST: the same query run by plain git under the
# same environment must reach the other repository, or the arm fails saying so -- an arm
# whose negative never materialised would pass over the broken code for the wrong reason.
# ★ THE ARMS THAT THE ACCIDENT OF A SHELL COULD HIDE ASSERT THE RESULT, NOT THE PATH TAKEN:
# a refusal is checked by its exit code AND by the victim repository still being dirty, so a
# reverted refusal reddens here on Git Bash (where `cd ""` fails, rc 0 "skipped") and on
# bash 5.2 / dash (where it wipes the victim) alike.
#
# ⚠ SAFETY IS STRUCTURAL: every repository below lives in a fresh `mktemp -d` proven to be
# outside this checkout and outside every git repository before anything runs, and the
# only destructive verb is ever pointed at those.
#
# Exit codes: 0 every arm held · 1 an arm failed · 2 cannot run.
set -u

_here="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd -P)" || { echo "test-leg-tree: CANNOT RUN -- cannot resolve this file's directory"; exit 2; }
LT="$_here/leg-tree.sh"
REPO="$(cd "$_here/../.." && pwd -P)" || exit 2
[ -f "$LT" ] || { echo "test-leg-tree: CANNOT RUN -- no $LT"; exit 2; }
command -v git >/dev/null 2>&1 || { echo "test-leg-tree: CANNOT RUN -- git is not on PATH"; exit 2; }
# The fixtures below are built with plain git, so a steering environment this test was STARTED
# in must not reach them; every arm that wants one exports it explicitly.
for _v in $(git rev-parse --local-env-vars 2>/dev/null); do unset "$_v"; done

TMP="$(mktemp -d 2>/dev/null)" || { echo "test-leg-tree: CANNOT RUN -- mktemp -d failed"; exit 2; }
TMP="$(cd "$TMP" && pwd -P)" || exit 2
case "$TMP/" in
    "$REPO"/*) echo "test-leg-tree: REFUSING -- the fixture root $TMP is inside $REPO"; exit 2 ;;
esac
if git -C "$TMP" rev-parse --show-toplevel >/dev/null 2>&1; then
    echo "test-leg-tree: REFUSING -- the fixture root $TMP is inside a git repository"; exit 2
fi
trap 'rm -rf -- "$TMP"' EXIT

EXPECTED=27
RAN=0; BAD=0
_ok()   { RAN=$((RAN + 1)); printf '  ok   %s\n' "$1"; }
_fail() { RAN=$((RAN + 1)); BAD=$((BAD + 1)); printf '  FAIL %s\n' "$1"; shift
          for _l in "$@"; do printf '       %s\n' "$_l"; done; }

_commit() { _c_repo="$1"; shift; git -C "$_c_repo" -c user.email=t@example.invalid -c user.name=test-leg-tree -c commit.gpgsign=false commit -q --no-verify "$@" 2>/dev/null; }
# <dir>: a repository with one committed file, then made DIRTY -- a tracked edit and an untracked file.
mkrepo() {
    git init -q "$1" && printf 'tracked\n' > "$1/t.txt" && git -C "$1" add t.txt \
    && git -C "$1" -c user.email=t@example.invalid -c user.name=test-leg-tree -c commit.gpgsign=false commit -q --no-verify -m t \
    && printf 'EDITED\n' > "$1/t.txt" && printf 'u\n' > "$1/${2:-untracked}.txt"
}
dirty()    { grep -q EDITED "$1/t.txt" 2>/dev/null && [ -f "$1/${2:-untracked}.txt" ]; }
pristine() { grep -q '^tracked$' "$1/t.txt" 2>/dev/null && [ ! -e "$1/${2:-untracked}.txt" ]; }
same_dir() { [ -n "$1" ] && [ -n "$2" ] && [ "$(cd "$1" 2>/dev/null && pwd -P)" = "$(cd "$2" 2>/dev/null && pwd -P)" ]; }

echo "test-leg-tree: $LT"

# ── E: an EMPTY repository argument is refused before anything runs ─────────────────────
V="$TMP/e1"; mkrepo "$V"
out=$(cd "$V" && . "$LT" "" && leg_tree_restore "" 2>&1); rc=$?
if [ "$rc" = 4 ] && dirty "$V" && printf '%s' "$out" | grep -q 'restore needs'; then
    _ok "(E1) leg_tree_restore \"\" run from inside a dirty repository is REFUSED (rc 4) and that repository keeps its dirty paths"
else
    _fail "(E1) leg_tree_restore \"\" from inside a dirty repository" "rc=$rc victim-still-dirty=$(dirty "$V" && echo yes || echo NO)" "said: $out"
fi

V="$TMP/e2"; mkrepo "$V"
out=$(cd "$V" && . "$LT" "" && leg_tree_restore 2>&1); rc=$?
if [ "$rc" = 4 ] && dirty "$V" && printf '%s' "$out" | grep -q 'restore needs'; then
    _ok "(E2) leg_tree_restore with NO argument is REFUSED the same way"
else
    _fail "(E2) leg_tree_restore with no argument" "rc=$rc victim-still-dirty=$(dirty "$V" && echo yes || echo NO)" "said: $out"
fi

V="$TMP/e3"; mkrepo "$V"
rcs=""; ok=1
out=$(cd "$V" && bash "$LT" restore 2>&1); rc=$?; rcs="bash=$rc"
{ [ "$rc" = 4 ] && dirty "$V"; } || ok=0
if command -v sh >/dev/null 2>&1; then
    out2=$(cd "$V" && sh "$LT" restore 2>&1); rc2=$?; rcs="$rcs sh=$rc2"
    { [ "$rc2" = 4 ] && dirty "$V"; } || ok=0
fi
if [ "$ok" = 1 ]; then
    _ok "(E3) the dispatch \`leg-tree.sh restore\` with no argument is REFUSED under bash and sh ($rcs)"
else
    _fail "(E3) the dispatch \`leg-tree.sh restore\` with no argument" "$rcs victim-still-dirty=$(dirty "$V" && echo yes || echo NO)" "said: $out ${out2:-}"
fi

V="$TMP/e4"; mkrepo "$V"
out=$(cd "$V" && . "$LT" "" && leg_tree_prepare "" some-branch some-sha 2>&1); rc=$?
if [ "$rc" = 4 ] && dirty "$V" && printf '%s' "$out" | grep -q 'prepare needs'; then
    _ok "(E4) leg_tree_prepare with an empty repository is REFUSED (rc 4) before touching anything"
else
    _fail "(E4) leg_tree_prepare with an empty repository" "rc=$rc victim-still-dirty=$(dirty "$V" && echo yes || echo NO)" "said: $out"
fi

V="$TMP/e5"; mkrepo "$V"
out=$(cd "$V" && . "$LT" "" && leg_tree_driver_git "" status --porcelain 2>&1); rc=$?
if [ "$rc" = 4 ] && ! printf '%s' "$out" | grep -q 'untracked.txt' && printf '%s' "$out" | grep -q 'driver_git needs'; then
    _ok "(E5) leg_tree_driver_git with an empty source is REFUSED (rc 4) instead of answering for the caller's cwd repository"
else
    _fail "(E5) leg_tree_driver_git with an empty source" "rc=$rc" "said: $out"
fi

out=$(. "$LT" "" && leg_tree_owning_root "" 2>&1); rc=$?
if [ "$rc" = 1 ] && [ -z "$(printf '%s' "$out" | grep -v '^$')" ]; then
    _ok "(E6) leg_tree_owning_root with an empty path answers nothing (rc 1)"
else
    _fail "(E6) leg_tree_owning_root with an empty path" "rc=$rc" "said: $out"
fi

out=$(. "$LT" "" && leg_tree_driver_identity "" 2>&1); rc=$?
if [ "$rc" = 1 ]; then
    _ok "(E7) leg_tree_driver_identity with an empty source answers nothing (rc 1)"
else
    _fail "(E7) leg_tree_driver_identity with an empty source" "rc=$rc" "said: $out"
fi

# ── C: the CONTROL -- a restore aimed at a real clone still works ───────────────────────
V="$TMP/c1"; mkrepo "$V"
out=$(. "$LT" "" && leg_tree_restore "$V" 2>&1); rc=$?
if [ "$rc" = 0 ] && pristine "$V"; then
    _ok "(C1) CONTROL: leg_tree_restore <clone> still restores that clone -- the refusals above are not refusing everything"
else
    _fail "(C1) CONTROL: leg_tree_restore <clone>" "rc=$rc pristine=$(pristine "$V" && echo yes || echo NO)" "said: $out"
fi

# ── S: the CALLER'S git environment reaches no git call ────────────────────────────────
NAMED="$TMP/s1-named"; VICT="$TMP/s1-victim"; mkrepo "$NAMED"; mkrepo "$VICT" victim-only
neg=$(cd "$NAMED" && GIT_DIR="$VICT/.git" GIT_WORK_TREE="$VICT" git status --porcelain 2>/dev/null)
real=0; printf '%s' "$neg" | grep -q 'victim-only.txt' && real=1
out=$(. "$LT" "" && GIT_DIR="$VICT/.git" && GIT_WORK_TREE="$VICT" && GIT_INDEX_FILE="$VICT/.git/index" \
      && export GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE && leg_tree_restore "$NAMED" 2>&1); rc=$?
if [ "$real" = 1 ] && [ "$rc" = 0 ] && pristine "$NAMED" && dirty "$VICT" victim-only; then
    _ok "(S1) with ANOTHER repository's GIT_DIR / GIT_WORK_TREE / GIT_INDEX_FILE exported, leg_tree_restore restores the clone it was NAMED and leaves the other untouched"
else
    _fail "(S1) leg_tree_restore under a steering git environment" \
          "negative-synthesized=$real rc=$rc named-pristine=$(pristine "$NAMED" && echo yes || echo NO) other-still-dirty=$(dirty "$VICT" victim-only && echo yes || echo NO)" "said: $out"
fi

DSS="$TMP/dss"; DECOY="$TMP/decoy"
git init -q "$DSS" && printf 'own\n' > "$DSS/own.txt" && git -C "$DSS" add own.txt && _commit "$DSS" -m own
git init -q "$DECOY" && printf 'd\n' > "$DECOY/zz-steer-decoy.txt" && git -C "$DECOY" add zz-steer-decoy.txt && _commit "$DECOY" -m d
mkdir -p "$DSS/.plans" "$DSS/scripts/probe" && printf 'x\n' > "$DSS/scripts/probe/probe.sh"
for _case in "GIT_INDEX_FILE=$DECOY/.git/index" "GIT_DIR=$DECOY/.git"; do
    _var=${_case%%=*}; _val=${_case#*=}
    neg=$(env "$_var=$_val" git -C "$DSS" ls-files -co --exclude-standard 2>/dev/null | grep -c zz-steer-decoy)
    got=$(. "$LT" "" && leg_tree_driver_identity "$DSS" >/dev/null 2>&1 && export "$_var=$_val" \
          && leg_tree_driver_git "$DSS" ls-files -co --exclude-standard 2>&1); rc=$?
    n=$(printf '%s\n' "$got" | grep -c zz-steer-decoy)
    tag=$([ "$_var" = GIT_INDEX_FILE ] && echo S2a || echo S2b)
    if [ "${neg:-0}" -ge 1 ] && [ "$rc" = 0 ] && [ "$n" = 0 ] && printf '%s\n' "$got" | grep -q '^own.txt$'; then
        _ok "($tag) leg_tree_driver_git ls-files IGNORES a caller's $_var naming another repository"
    else
        _fail "($tag) leg_tree_driver_git ls-files under a caller's $_var" "negative-synthesized=$([ "${neg:-0}" -ge 1 ] && echo yes || echo NO) rc=$rc decoy-lines=$n" "said: $(printf '%s' "$got" | tr '\n' ' ' | cut -c1-200)"
    fi
done

# ── O: the owning root is the tree the path lives in ────────────────────────────────────
P="$DSS/scripts/probe/probe.sh"
FOREIGN="$TMP/foreign"; git init -q "$FOREIGN"
got=$(. "$LT" "" && leg_tree_owning_root "$P" 2>/dev/null); rc=$?
if [ "$rc" = 0 ] && same_dir "$got" "$DSS"; then
    _ok "(O0) CONTROL: leg_tree_owning_root <tree>/scripts/probe/probe.sh -> the tree"
else
    _fail "(O0) CONTROL: leg_tree_owning_root" "rc=$rc got=$got want=$DSS"
fi

neg=$(GIT_DIR="$FOREIGN/.git" git -C "$DSS/scripts/probe" rev-parse --show-toplevel 2>/dev/null)
got=$(. "$LT" "" && GIT_DIR="$FOREIGN/.git" && export GIT_DIR && leg_tree_owning_root "$P" 2>/dev/null); rc=$?
if [ -n "$neg" ] && ! same_dir "$neg" "$DSS" && [ "$rc" = 0 ] && same_dir "$got" "$DSS"; then
    _ok "(O1) under a caller's GIT_DIR (raw git said $neg) the owning root is still the tree"
else
    _fail "(O1) leg_tree_owning_root under a caller's GIT_DIR" "raw-git=$neg rc=$rc got=$got want=$DSS"
fi

neg=$(GIT_DIR="$FOREIGN/.git" GIT_WORK_TREE="$FOREIGN" git -C "$DSS/scripts/probe" rev-parse --show-toplevel 2>/dev/null)
got=$(. "$LT" "" && GIT_DIR="$FOREIGN/.git" && GIT_WORK_TREE="$FOREIGN" && export GIT_DIR GIT_WORK_TREE && leg_tree_owning_root "$P" 2>/dev/null); rc=$?
if same_dir "$neg" "$FOREIGN" && [ "$rc" = 0 ] && same_dir "$got" "$DSS"; then
    _ok "(O2) under a caller's GIT_DIR + GIT_WORK_TREE (raw git said the other repository) the owning root is still the tree"
else
    _fail "(O2) leg_tree_owning_root under a caller's GIT_DIR + GIT_WORK_TREE" "raw-git=$neg rc=$rc got=$got want=$DSS"
fi

OUTER="$TMP/outer"; git init -q "$OUTER"; COPY="$OUTER/.temp/copy"
mkdir -p "$COPY/.plans" "$COPY/scripts/probe" && printf 'x\n' > "$COPY/scripts/probe/probe.sh"
neg=$(git -C "$COPY/scripts/probe" rev-parse --show-toplevel 2>/dev/null)
got=$(. "$LT" "" && leg_tree_owning_root "$COPY/scripts/probe/probe.sh" 2>&1); rc=$?
if same_dir "$neg" "$OUTER" && [ "$rc" = 1 ] && printf '%s' "$got" | grep -q 'nested inside the checkout'; then
    _ok "(O3) an untracked DSS tree copied inside another checkout is REFUSED (raw git answered the outer checkout)"
else
    _fail "(O3) leg_tree_owning_root on a nested untracked DSS copy" "raw-git=$neg rc=$rc" "said: $got"
fi

PLAIN="$TMP/plain"; git init -q "$PLAIN"
got=$(. "$LT" "" && leg_tree_owning_root "$PLAIN" 2>/dev/null); rc=$?
if [ "$rc" = 0 ] && same_dir "$got" "$PLAIN"; then
    _ok "(O4) a plain git repository with no DSS tree around it is still answered -- lane-worktree's explicit --repo keeps working"
else
    _fail "(O4) leg_tree_owning_root on a plain repository" "rc=$rc got=$got want=$PLAIN"
fi

INNER="$DSS/.temp/inner"; mkdir -p "$INNER" && git init -q "$INNER"
got=$(. "$LT" "" && leg_tree_owning_root "$INNER" 2>/dev/null); rc=$?
if [ "$rc" = 0 ] && same_dir "$got" "$INNER"; then
    _ok "(O5) a git repository nested INSIDE a DSS tree is its own working tree -- the inner one is answered"
else
    _fail "(O5) leg_tree_owning_root on a repository nested inside a DSS tree" "rc=$rc got=$got want=$INNER"
fi

# ── G: a CONSUMER -- check-root-litter.sh asks git only through leg_tree_git_unsteered ──
# ✔MEASURED 2026-09-15 (P66 lane rr, probe3/probe4): a copy of that guard in a fixture tree with
# ONE loose root file said "OK -- no untracked files" (rc 0) under another repository's
# GIT_INDEX_FILE, and under its GIT_DIR + GIT_WORK_TREE; with its own `.git/index` corrupt,
# `git status` exited 128 and the guard still said OK. Its copy runs here beside a copy of THIS
# leg-tree.sh, so a guard that stops using the helper, or stops reading git's exit status,
# reddens this entry by arm name.
_steered() {  # <GIT_INDEX_FILE|GIT_DIR+GIT_WORK_TREE> <command...>
    _sc="$1"; shift
    if [ "$_sc" = GIT_INDEX_FILE ]; then
        GIT_INDEX_FILE="$DECOY/.git/index" "$@"
    else
        GIT_DIR="$DECOY/.git" GIT_WORK_TREE="$DECOY" "$@"
    fi
}
LIT="$TMP/litter-tree"
mkdir -p "$LIT/.plans" "$LIT/scripts/check-root-litter" "$LIT/scripts/leg-tree"
cp "$REPO/scripts/check-root-litter/check-root-litter.sh" "$LIT/scripts/check-root-litter/check-root-litter.sh"
cp "$LT" "$LIT/scripts/leg-tree/leg-tree.sh"
printf 'own\n' > "$LIT/README.md"
git init -q "$LIT" && git -C "$LIT" add README.md scripts && _commit "$LIT" -m fixture
printf 'litter\n' > "$LIT/loose.txt"
for _case in GIT_INDEX_FILE GIT_DIR+GIT_WORK_TREE; do
    tag=$([ "$_case" = GIT_INDEX_FILE ] && echo G1a || echo G1b)
    neg=$(_steered "$_case" git -C "$LIT" status --porcelain=v1 --untracked-files=normal 2>/dev/null | grep -c '^?? loose.txt$')
    out=$(_steered "$_case" bash "$LIT/scripts/check-root-litter/check-root-litter.sh" 2>&1); rc=$?
    if [ "${neg:-0}" = 0 ] && [ "$rc" = 1 ] && printf '%s' "$out" | grep -q 'loose.txt' \
       && ! printf '%s' "$out" | grep -q 'README.md'; then
        _ok "($tag) check-root-litter.sh under a caller's $_case still names the loose root file, and only it (raw git did not name it)"
    else
        _fail "($tag) check-root-litter.sh under a caller's $_case" "raw-git-named-loose=${neg:-0} (0 is the steered negative) rc=$rc" "said: $(printf '%s' "$out" | tr '\n' ' ' | cut -c1-240)"
    fi
done

CORRUPT="$TMP/litter-corrupt"
cp -R "$LIT" "$CORRUPT"
printf 'not an index\n' > "$CORRUPT/.git/index"
git -C "$CORRUPT" status --porcelain >/dev/null 2>&1; rawrc=$?
out=$(bash "$CORRUPT/scripts/check-root-litter/check-root-litter.sh" 2>&1); rc=$?
if [ "$rawrc" != 0 ] && [ "$rc" = 2 ] && printf '%s' "$out" | grep -q 'CANNOT RUN'; then
    _ok "(G2) check-root-litter.sh REFUSES (rc 2) when git status itself fails (raw git exited $rawrc) instead of calling the root clean"
else
    _fail "(G2) check-root-litter.sh when git status fails" "raw-git-rc=$rawrc (non-zero is the negative) rc=$rc" "said: $(printf '%s' "$out" | tr '\n' ' ' | cut -c1-240)"
fi

# ── U: the owner runs ANY command unsteered, not only git (P66 lane ge) ─────────────────
# ✔MEASURED 2026-09-15: gh resolves its repository by running git, so under another repository's
# GIT_DIR `check-ci-legs.sh` asked about THAT repository's CI (see `leg_tree_unsteered`). A child
# `sh` stands in for gh here: it prints the three variables it can see and exits 7.
_seen='printf "%s|%s|%s" "${GIT_DIR-unset}" "${GIT_WORK_TREE-unset}" "${GIT_INDEX_FILE-unset}"; exit 7'
neg=$(GIT_DIR="$DECOY/.git" GIT_WORK_TREE="$DECOY" GIT_INDEX_FILE="$DECOY/.git/index" sh -c "$_seen" 2>/dev/null)
got=$(. "$LT" "" && GIT_DIR="$DECOY/.git" && GIT_WORK_TREE="$DECOY" && GIT_INDEX_FILE="$DECOY/.git/index" \
      && export GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE \
      && { leg_tree_unsteered sh -c "$_seen"; printf ' rc=%s after=%s' "$?" "${GIT_DIR-unset}"; } 2>&1)
if [ "$neg" = "$DECOY/.git|$DECOY|$DECOY/.git/index" ] && [ "$got" = "unset|unset|unset rc=7 after=$DECOY/.git" ]; then
    _ok "(U1) leg_tree_unsteered runs a NON-git command with the caller's GIT_DIR / GIT_WORK_TREE / GIT_INDEX_FILE removed, returns its exit status, and leaves the caller's own environment set"
else
    _fail "(U1) leg_tree_unsteered on a non-git command" "a bare child saw (the negative): $neg" "through the owner: $got"
fi

out=$(. "$LT" "" && leg_tree_unsteered 2>&1); rc=$?
if [ "$rc" = 4 ] && printf '%s' "$out" | grep -q 'unsteered needs'; then
    _ok "(U2) leg_tree_unsteered with NO command is REFUSED (rc 4) -- an empty exec would run nothing and report success"
else
    _fail "(U2) leg_tree_unsteered with no command" "rc=$rc" "said: $out"
fi

# ── L: the REMOTE transport -- the helper on stdin, a small loader on the command line (P66 lane ge) ──
# ✔MEASURED 2026-09-15: every driver sent this file's TEXT as one command-line argument, so each worked
# only under the smallest ceiling its path crossed (native ssh.exe refused 32,700 characters; WSL's execve
# 131,072 bytes). `leg_tree_remote_command` builds `sh -c '<loader>' leg-tree <bytes> <verb> '<arg>'...`
# and the carriage's stdin carries the bytes. Each command below is run by `sh`, as a host would run it,
# in a private TMPDIR, against a destination that does not exist -- so no verb touches anything.
LT_TMP="$TMP/lt-tmp"; mkdir -p "$LT_TMP"
_remote() { # <stdin-file> <command> -> "rc=<rc> left=<temp files left> said=<output>"
    _r_out=$(TMPDIR="$LT_TMP" sh -c "$2" < "$1" 2>&1); _r_rc=$?
    printf 'rc=%s left=%s said=%s' "$_r_rc" "$(ls -A "$LT_TMP" | wc -l | tr -d ' ')" "$(printf '%s' "$_r_out" | tr '\n' ' ')"
}
MISSING="$TMP/lt-no-such-clone"
cmd=$(. "$LT" "" && leg_tree_remote_command "$LT" restore "$MISSING"); rc=$?
got=$(_remote "$LT" "$cmd")
if [ "$rc" = 0 ] && [ "${#cmd}" -lt 2048 ] && ! printf '%s' "$cmd" | grep -q 'leg_tree_git_unsteered' \
   && printf '%s' "$got" | grep -q "^rc=0 left=0 said=.*restore skipped, no $MISSING"; then
    _ok "(L1) CONTROL: the command leg_tree_remote_command builds (${#cmd} bytes, no helper text in it), run by sh with this file on stdin, runs the verb with its argument and leaves no temp file"
else
    _fail "(L1) CONTROL: the remote command with the whole helper on stdin" "builder rc=$rc bytes=${#cmd}" "$got"
fi

cut=$(grep -b -m1 '^# ── dispatch, so this file is BOTH' "$LT" | cut -d: -f1)
head -c "$cut" "$LT" > "$TMP/lt-nodispatch.sh"
direct=$(sh "$TMP/lt-nodispatch.sh" restore "$MISSING" 2>&1); direct_rc=$?
got=$(_remote "$TMP/lt-nodispatch.sh" "$cmd")
if [ "$direct_rc" = 0 ] && [ -z "$direct" ] \
   && printf '%s' "$got" | grep -q '^rc=71 left=0 said=.*TRUNCATED on stdin' && ! printf '%s' "$got" | grep -q 'restore skipped'; then
    _ok "(L2) a helper cut before its dispatch is REFUSED by the loader (rc 71) and runs NOTHING -- the same cut file run directly by sh exits 0 silently (the negative)"
else
    _fail "(L2) a helper cut short on stdin" "run directly by sh: rc=$direct_rc output=[$direct] (rc 0 and empty is the negative)" "through the loader: $got"
fi

cmd2=$(. "$LT" "" && leg_tree_remote_command "$LT" prepare "$MISSING" HEAD 0123abcd)
got=$(_remote "$LT" "$cmd2")
if printf '%s' "$got" | grep -q "^rc=2 left=0 said=.*no such directory: $MISSING"; then
    _ok "(L3) a verb's own failure status (prepare's rc 2) comes back through the loader, and its temp file still goes"
else
    _fail "(L3) a failing verb through the loader" "$got"
fi

{ cat "$LT"; head -c 200000 /dev/zero | tr '\0' '#' | fold -w 100 | sed 's/^/# /'; } > "$TMP/lt-padded.sh"
cmd3=$(. "$LT" "" && leg_tree_remote_command "$TMP/lt-padded.sh" restore "$TMP/it's here")
got=$(_remote "$TMP/lt-padded.sh" "$cmd3")
if [ "${#cmd3}" -lt 2048 ] && printf '%s' "$got" | grep -q "^rc=0 left=0 said=.*restore skipped, no $TMP/it's here"; then
    _ok "(L4) SIZE: a helper padded past 200 KB still travels (command ${#cmd3} bytes), and an argument holding a single quote arrives intact"
else
    _fail "(L4) a padded helper and a quoted argument" "command bytes=${#cmd3}" "$got"
fi

out=$(. "$LT" "" && leg_tree_remote_command "$LT" 'rm -rf' x 2>&1); rc=$?
out2=$(. "$LT" "" && leg_tree_remote_command "$TMP/lt-no-such-helper.sh" restore x 2>&1); rc2=$?
if [ "$rc" = 4 ] && printf '%s' "$out" | grep -q 'not a lower-case word' && [ "$rc2" = 4 ] && printf '%s' "$out2" | grep -q 'no readable helper'; then
    _ok "(L5) the builder REFUSES (rc 4) a verb that is not a lower-case word, and a helper file that does not exist"
else
    _fail "(L5) the builder's refusals" "verb: rc=$rc said=$out" "missing helper: rc=$rc2 said=$out2"
fi

if [ "$RAN" != "$EXPECTED" ]; then
    echo "test-leg-tree: FAIL -- $RAN arm(s) ran, $EXPECTED expected. An arm that silently stops running is a property that silently stops being proven."
    exit 1
fi
if [ "$BAD" != 0 ]; then
    echo "test-leg-tree: FAIL -- $BAD of $RAN arm(s)"
    exit 1
fi
echo "test-leg-tree: OK -- $RAN arm(s): empty-argument refusals, a control, git-environment immunity, owning-root rules, a consumer guard (check-root-litter), and the owner running a non-git command unsteered"
exit 0
