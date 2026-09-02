#!/usr/bin/env bash
# PURPOSE: reach the operator's physical macOS host (the carriage).
# ssh-macos.sh — reach the operator's physical macOS host. Sibling of ssh-macos.ps1;
# CAPABILITY-PAIRED (a change to one lands in the other, or the pair is broken).
#
# WHY. macOS is the ONE target with no off-Mac emulator: nothing on Windows or Linux runs
# a Mach-O. Every other leg is verifiable where it is built (pe64 natively, elf64 under
# WSL, arm64 under qemu); a Darwin artefact can only be proven on real hardware. This is
# that carriage, made reproducible instead of living in one person's shell history.
#
# ★★ PERSONAL MACHINE, NOT CI. It is usually OFF. Never assume it is reachable, never wake
#    it, never run against it without the operator saying it is on. Failure to connect is
#    the EXPECTED state, not something to route around.
# ★ NO HOST DETAILS ARE TRACKED. Precedence: env > .secrets/macos.env > FAIL LOUD.
#   `.secrets/` is gitignored because this repo is slated to go public (PR #37).
# ★ KEY-BASED ONLY, DELIBERATELY. `BatchMode=yes` makes ssh FAIL rather than sit at a
#   password prompt. No password is accepted, stored or forwarded here — a password in a
#   repo script is a committed credential, and password auth cannot be automated without
#   putting the secret in the environment. Put the public key in the Mac's
#   ~/.ssh/authorized_keys; the operator performs that step, so no secret passes through
#   the tooling at all.
# ★ HOST-KEY POLICY matches the operator's original (StrictHostKeyChecking=no +
#   UserKnownHostsFile=/dev/null) because the Mac is a DHCP LAN host whose address moves.
#   STATED, not hidden: that forgoes MITM protection — defensible on a home LAN, not on an
#   untrusted network. Set DSS_MACOS_STRICT_HOSTKEY=1 there.
#
# Usage: scripts/ssh-macos/ssh-macos.sh                # interactive
#        scripts/ssh-macos/ssh-macos.sh uname -m       # run a command, exit with ITS status
#
# ⚠⚠ SSH JOINS THE REMAINING ARGUMENTS WITH SPACES AND HANDS THE RESULT TO THE REMOTE
# SHELL, SO YOUR LOCAL QUOTING IS GONE BY THE TIME IT ARRIVES. `uname -m` above works
# only because it contains none. ✔MEASURED 2026-08-20: `ssh-macos.sh sh -c 'mkdir -p /tmp/x
# && …'` reached the Mac as `sh -c mkdir -p /tmp/x && …`, and the remote `mkdir` printed
# its usage line — a failure that looks like a broken carriage and is not one. ⇒ ANYTHING
# CARRYING QUOTES, PIPES, `&&` OR REDIRECTION MUST BE PASSED AS ONE ARGUMENT:
#        scripts/ssh-macos/ssh-macos.sh "cd /tmp/x && clang -c a.c && nm -n a.o"
# This is ssh's own contract, not a defect here — recorded because the usage line above
# invites the multi-argument form and the failure it produces names the wrong culprit.
set -uo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
# ★ `.secrets/` IS GITIGNORED, SO A LANE WORKTREE HAS NONE -- `git worktree add`
# writes only tracked files, and `.worktrees/` is the SANCTIONED home for lane
# worktrees. Resolved through the one owner rather than assumed to sit beside this
# script: `scripts/repo-secrets/` follows a linked worktree back to the main checkout
# that holds the file. Without it this carriage refused rc=3 for every lane, naming a
# path inside the worktree and inviting the one repair that must never be made (a
# second copy of key material, per lane).
# ⚠ THE EMPTY ARGUMENT IS LOAD-BEARING: `.` forwards THIS script's positional
# parameters unless given its own, and this carriage is routinely called with a remote
# command as `$1`.
# shellcheck source=../repo-secrets/repo-secrets.sh
. "$REPO/scripts/repo-secrets/repo-secrets.sh" "" \
    || { echo "ssh-macos: cannot load $REPO/scripts/repo-secrets/repo-secrets.sh" >&2; exit 3; }
SECRETS=$(repo_secrets_dir "$REPO")
CONF=$SECRETS/macos.env

# ★ RESOLVED BY EXECUTION, NOT BY `command -v` -- which LIES on these hosts, and a
# `WindowsApps` stub answers yes and then does nothing. `python` is the name a
# Windows install puts on PATH, `python3` the one WSL and macOS use; a carriage that
# hardcodes either fails elsewhere with a message that reads as a missing dependency
# rather than as a missing name. ⓘ Only the `--push` path needs it, so a host with no
# python can still use every other subcommand: the refusal is at the use, not here.
PY=""
for _c in python3 python; do
    if "$_c" -c 'import sys; sys.exit(0)' >/dev/null 2>&1; then PY="$_c"; break; fi
done

# ★ ENV MUST WIN OVER THE CONFIG FILE, AND THAT TAKES DELIBERATE CODE. `.` sources
# the file INTO THIS SHELL, so a bare `. "$CONF"` OVERWRITES whatever the caller put
# in the environment — the exact reverse of the precedence stated at the top of this
# file. ⚠ MEASURED 2026-08-13: `DSS_MACOS_HOST=<ip> scripts/ssh-macos/ssh-macos.sh` was silently
# ignored, and the script still tried to resolve the `.local` name from the config.
# That is the single worst place for this bug to live: the override it defeats is the
# one THIS SCRIPT'S OWN resolve-failure message instructs you to use ("Put a literal
# IP in DSS_MACOS_HOST"), on the WSL path where the same message says mDNS is expected
# to fail. So the documented workaround for the documented failure could not work.
# The PowerShell sibling always had this right (`if ($env:DSS_MACOS_HOST) {...} else
# {$conf[...]}`) — this is the CAPABILITY PAIR having drifted, which is precisely what
# the header's pairing note exists to prevent.
_envHost=${DSS_MACOS_HOST:-} ; _envUser=${DSS_MACOS_USER:-} ; _envKey=${DSS_MACOS_KEY:-}
# ⚠ THE PATH PREFIX CAPTURES *SET-NESS*, NOT EMPTINESS, AND THE OTHER THREE DELIBERATELY
# DO NOT — D-SCRIPT-SSH-MACOS-PATH-PREFIX-CANNOT-BE-DISABLED-BY-AN-EMPTY-VALUE.
# For host/user/key an EMPTY value is meaningless and `:-` is right. For this one an
# empty value is the DISABLE switch, so `:-` erases the very state the caller is
# expressing. ✔MEASURED 2026-08-28, and it made this script's own commissioning A/B
# VACUOUS: `DSS_MACOS_PATH_PREFIX="" ssh-macos.sh 'command -v cmake'` was run as the
# CONTROL arm — the arm that is supposed to reproduce the pre-fix breakage — and the
# `${VAR:=default}` below re-assigned the default over the empty string, so the control
# ran WITH the repair and returned `/opt/homebrew/bin/cmake`. ★ The failure direction is
# the dangerous one: a control that silently becomes a second treatment arm agrees with
# the treatment, which reads as "the fix works" no matter what the fix does.
_envPathPrefixSet=${DSS_MACOS_PATH_PREFIX+set}
_envPathPrefix=${DSS_MACOS_PATH_PREFIX:-}
# shellcheck disable=SC1090
[ -f "$CONF" ] && . "$CONF"
if [ -n "$_envHost" ]; then DSS_MACOS_HOST=$_envHost ; fi
if [ -n "$_envUser" ]; then DSS_MACOS_USER=$_envUser ; fi
if [ -n "$_envKey"  ]; then DSS_MACOS_KEY=$_envKey   ; fi
if [ -n "$_envPathPrefixSet" ]; then DSS_MACOS_PATH_PREFIX=$_envPathPrefix ; fi
unset _envHost _envUser _envKey _envPathPrefix _envPathPrefixSet

: "${DSS_MACOS_HOST:=}" ; : "${DSS_MACOS_USER:=}" ; : "${DSS_MACOS_KEY:=}"

# ★★★ THE REMOTE PATH IS REPAIRED BEFORE ANY PAYLOAD RUNS —
# D-TOOLS-SSH-MACOS-NONINTERACTIVE-PATH-IS-CLOBBERED-BY-EMSDK.
#
# ✔MEASURED 2026-08-04 and re-faced 2026-08-27: this host runs an emsdk shell hook
# that REPLACES `PATH` with `<emsdk dirs>:/usr/bin:/bin:/usr/sbin:/sbin` for a
# NON-INTERACTIVE session, dropping `/opt/homebrew/bin` AND `/usr/local/bin`. So
# `command -v cmake` answers MISSING while `ls /opt/homebrew/bin` shows cmake,
# ninja, tclsh and tclsh9.0 all present.
#
# ★★ THE DIRECTION OF THE WRONG ANSWER IS WHAT MAKES THIS WORTH REPAIRING RATHER
# THAN DOCUMENTING: it is a FALSE NEGATIVE — "the tool is not installed" — which
# silently SHRINKS what the project believes it can do, and it has already put a
# refuted sentence ("Homebrew is not installed") into a commit message as a measured
# fact used to justify a design decision. A capability question answered wrongly
# does not fail; it re-plans the work around a machine that was never limited.
#
# ⚠ WHY NOT A LOGIN SHELL, which is the other arm the anchor offered: a login shell
# runs the profile, and THIS host's profile is exactly what the `.local`-resolver
# note below records CONSUMING A SCRIPT PIPED TO `bash -s`. Repairing PATH by
# invoking the thing that breaks stdin trades a false negative for a silent
# truncation, which is strictly worse. The prefix is deterministic and touches
# nothing else.
#
# ⚠ IT IS A PREFIX, NOT A REPLACEMENT. The remote `PATH` is kept and appended, so a
# tool that lives only in the emsdk dirs is still found; and `$PATH` is written to
# expand ON THE FAR SIDE (single-quoted here) — expanding it locally would ship
# THIS machine's PATH to macOS.
#
# ⚠ IT IS `export ...;`, A STATEMENT, NOT `PATH=... cmd`, AN ASSIGNMENT PREFIX. ssh
# hands the payload to the remote shell as ONE string, so an assignment prefix binds
# only to the FIRST command: `PATH=x cd d && make` would run `make` with the broken
# PATH. The `export` form covers a compound payload, and — the case that matters
# most here — it is inherited by `bash -s`, so the piped-script transports get the
# repair without a second mechanism and without touching their stdin.
#
# ⚠ `+set`, NOT `:=` — an EMPTY value is the caller's DISABLE switch and must survive.
# See the set-ness note in the env-precedence block above for the measurement.
if [ -z "${DSS_MACOS_PATH_PREFIX+set}" ]; then
    DSS_MACOS_PATH_PREFIX=/opt/homebrew/bin:/usr/local/bin
fi
remote_path_stmt=
if [ -n "$DSS_MACOS_PATH_PREFIX" ]; then
    remote_path_stmt='export PATH="'$DSS_MACOS_PATH_PREFIX':$PATH"; '
fi
DSS_MACOS_KEY=$(eval printf '%s' "\"$DSS_MACOS_KEY\"")

# ★★★ THE SCRIPT FINDS THE KEY — the caller never does anything manually.
# Operator instruction 2026-08-17. Same reasoning as the arm64-vps twin (read the
# long note there): `$HOME` names a DIFFERENT directory in WSL, Git Bash and
# PowerShell, so a `$HOME`-relative key makes a host reachable or not depending on
# which shell you started in. `$REPO/.secrets/` is the same directory in all of
# them and is gitignored, so key material never reaches a commit.
# ⚠ CAPABILITY-PAIRED with `ssh-macos.ps1` and with BOTH arm64-vps carriages: a
# change to one lands in all four, or the pairing this file's header claims is a
# claim nothing checks.
_repo_key=$SECRETS/macos.key
if [ -n "$DSS_MACOS_KEY" ] && [ ! -f "$DSS_MACOS_KEY" ] && [ -f "$_repo_key" ]; then
    echo "ssh-macos: DSS_MACOS_KEY='$DSS_MACOS_KEY' does not exist; using the repo-local key $_repo_key" >&2
    DSS_MACOS_KEY=$_repo_key
elif [ -z "$DSS_MACOS_KEY" ] && [ -f "$_repo_key" ]; then
    DSS_MACOS_KEY=$_repo_key
fi
unset _repo_key

if [ -z "$DSS_MACOS_HOST" ] || [ -z "$DSS_MACOS_USER" ]; then
    {
      echo "ssh-macos: connection data missing."
      echo "  Create $CONF with DSS_MACOS_HOST, DSS_MACOS_USER and DSS_MACOS_KEY (a key PATH)."
    } >&2
    exit 3
fi

# `.local` is mDNS/Bonjour: resolves on macOS and on Linux WITH avahi, and very often NOT
# from WSL, which has no mDNS responder. That is a real cross-host difference, so the
# diagnostic names the override rather than leaving it to be discovered.
target=$DSS_MACOS_HOST
if ! printf '%s' "$DSS_MACOS_HOST" | grep -qE '^[0-9]{1,3}(\.[0-9]{1,3}){3}$'; then
    # ★★★ EVERY RESOLVER BELOW READS FROM /dev/null, AND THAT REDIRECT IS THE WHOLE
    # POINT OF THIS BLOCK RATHER THAN AN AFTERTHOUGHT.
    #
    # ⚠ D-SCRIPT-SSH-MACOS-RESOLVER-DRAINS-THE-CALLER-S-STDIN: a command substitution
    # INHERITS this script's stdin. `macos-leg.sh` pipes its entire remote body into
    # `ssh-macos.sh ... bash -s`, so a resolver that reads stdin CONSUMES THE SCRIPT
    # IT WAS SUPPOSED TO SEND, and `bash -s` on the far side receives ZERO BYTES.
    # ✔MEASURED 2026-08-25 (cycle P36, lane B), same host and payload, only the
    # resolution path changing:
    #     hostname form : printf '12345678\n' | ssh-macos.sh "wc -c"  ->  0
    #     IP form       : DSS_MACOS_HOST=<ip> ...                     ->  9
    # ⇒ EVERY macOS leg taken through the `.local` name ended in "no witness came
    # back ... UNKNOWN". `--push` was immune only because it builds its own pipe.
    #
    # ★ IT IS FAIL-CLOSED — no false green was ever produced — and the witness
    # discipline is what surfaced it. But a leg that cannot run is not a leg that
    # passed, and this file's header previously asserted the OPPOSITE ("stdin
    # survives byte-exact ... the 2026-08-18 note claiming the login profile eats
    # stdin is expired"). That note was RIGHT; the measurement which overturned it
    # tested DATA over stdin, not a shell READING A SCRIPT from it.
    # ★★★ THE WHOLE LADDER IS ATTEMPTED SEVERAL TIMES, BECAUSE ONE mDNS QUERY IS NOT
    # A MEASUREMENT OF WHETHER A HOST IS UP.
    # ✔MEASURED 2026-08-31 (P47), this host, the Mac demonstrably UP the whole time
    # (`Test-NetConnection -Port 22` -> True, `ssh` by literal IP -> rc=0):
    #   * two consecutive `ssh-macos.sh` invocations printed "cannot resolve ...";
    #   * a 5-arm control run seconds later resolved 192.168.0.71 on ALL FIVE arms,
    #     including the byte-identical `powershell.exe` command this script uses.
    # ⇒ the ladder is CORRECT and INTERMITTENT. mDNS has no authoritative negative:
    # a responder that does not answer within the resolver's window is indistinguish-
    # able from one that is not there, and the answer is then cached either way.
    # ⛔⛔ AND THE COST OF TREATING ONE MISS AS AN ANSWER IS THE LARGEST THIS ROW HAS
    # PAID: P46 concluded from exactly this failure that "the Bonjour responder is up
    # and sshd is not", wrote the macOS carriage off as a BLOCKER in
    # [[D-SQLITE-CLI-BUILT-ON-NO-LEG]], and 22 of that row's 40 obligations were
    # declared blocked on a host that was answering on port 22.
    #   D-SCRIPT-MACOS-CARRIAGE-CALLS-ONE-MDNS-MISS-A-DOWN-HOST
    # ★ BOUNDED AND SILENT WHEN IT SUCCEEDS FIRST TIME, so the ordinary path costs
    # nothing; only a retry says so, and only on stderr, because a carriage that
    # narrates its own plumbing into stdout corrupts every caller that reads a witness.
    #
    # ── THE LADDER, AS ONE FUNCTION, SO THE RETRY CANNOT DRIFT FROM IT ──────────
    # ★★★ DELEGATE TO THE WINDOWS RESOLVER — THE ONE THAT ACTUALLY SPEAKS mDNS HERE.
    # ✔MEASURED 2026-08-17: from Git Bash and WSL, `getent` and `ping` both fail on a
    # `.local` name because neither has an mDNS responder — but WINDOWS resolves it
    # natively: `[System.Net.Dns]::GetHostAddresses('<name>.local')` returns the
    # address, as does `Resolve-DnsName`. (`ping.exe` does NOT, so it is not the probe
    # to use — testing with ping is what makes this look unresolvable.)
    # ⇒ We are running under Windows in both those shells, so the resolver is right
    # there; we were simply asking the wrong one.
    # ⛔ THIS IS WHY THE CONFIG HOLDS A NAME AND NEVER AN IP: the Mac is on DHCP and
    # a pinned address is wrong the moment the lease changes — a hardcoded IP turns a
    # transient lookup problem into a permanently wrong file.
    _mac_resolve_once() {
        _r=$(getent hosts "$DSS_MACOS_HOST" 2>/dev/null </dev/null | awk '{print $1; exit}')
        [ -z "$_r" ] && _r=$(ping -c1 -W1 "$DSS_MACOS_HOST" 2>/dev/null </dev/null \
            | sed -n 's/.*(\([0-9.]\{7,15\}\)).*/\1/p' | head -1)
        # ★ avahi, when this is a Linux box that actually has an mDNS responder.
        if [ -z "$_r" ] && command -v avahi-resolve-host-name >/dev/null 2>&1; then
            _r=$(avahi-resolve-host-name -4 "$DSS_MACOS_HOST" 2>/dev/null </dev/null | awk '{print $2; exit}')
        fi
        if [ -z "$_r" ]; then
            for _psh in powershell.exe pwsh.exe; do
                command -v "$_psh" >/dev/null 2>&1 || continue
                _r=$("$_psh" -NoProfile -NonInteractive -Command \
                    "try { [System.Net.Dns]::GetHostAddresses('$DSS_MACOS_HOST') | Where-Object { \$_.AddressFamily -eq 'InterNetwork' } | Select-Object -First 1 -ExpandProperty IPAddressToString } catch { }" \
                    2>/dev/null </dev/null | tr -d '\r' | grep -oE '^[0-9]{1,3}(\.[0-9]{1,3}){3}$' | head -1)
                [ -n "$_r" ] && break
            done
            unset _psh
        fi
        printf '%s' "$_r"
    }
    # ★★★ A SHORT-LIVED CACHE, BECAUSE A DRIVER CALLS THIS CARRIAGE MANY TIMES AND
    # EVERY CALL WAS ROLLING THE SAME DICE.
    # ✔MEASURED 2026-08-31 (P47): a three-hop round trip (mkdir, rsync, run) against a
    # Mac answering on port 22 the whole time resolved on hops 1 and 2 and then MISSED
    # EIGHT CONSECUTIVE ATTEMPTS on hop 3. Retrying makes ONE lookup reliable; it does
    # nothing about N independent lookups, and a driver's failure probability grows
    # with N. Resolving ONCE per TTL is the fix for that, and it is also politer to a
    # link-local protocol that answers by multicast.
    # ⚠ TTL-BOUNDED AND SHORT (default 900 s) BECAUSE THE MAC IS ON DHCP, and this file
    # already says why an address must never be PINNED: a stale lease would make a
    # cached address permanently wrong. A stale entry fails LOUDLY at `ssh` with a
    # connection error naming the address, which is actionable; it can never produce a
    # wrong ANSWER, only a wrong destination that refuses to connect.
    # ⓘ Deleting the cache file is always safe: the ladder simply runs again.
    _mac_cache="${XDG_CACHE_HOME:-$HOME/.cache}/dsscp/macos-addr-$(printf '%s' "$DSS_MACOS_HOST" | tr -c 'A-Za-z0-9._-' '_')"
    _mac_ttl=${DSS_MACOS_RESOLVE_TTL:-900}
    resolved=""
    if [ -f "$_mac_cache" ] && [ "$_mac_ttl" -gt 0 ] 2>/dev/null; then
        _mac_age=$(( $(date +%s) - $(awk 'NR==1{print $1+0}' "$_mac_cache" 2>/dev/null || echo 0) ))
        if [ "$_mac_age" -ge 0 ] && [ "$_mac_age" -lt "$_mac_ttl" ]; then
            resolved=$(awk 'NR==1{print $2}' "$_mac_cache" 2>/dev/null \
                | grep -oE '^[0-9]{1,3}(\.[0-9]{1,3}){3}$' | head -1)
        fi
    fi
    _mac_tries=${DSS_MACOS_RESOLVE_TRIES:-6}
    _mac_try=1
    while [ -z "$resolved" ]; do
        resolved=$(_mac_resolve_once)
        [ -n "$resolved" ] && break
        [ "$_mac_try" -ge "$_mac_tries" ] && break
        # ⚠ ON STDERR, NEVER STDOUT. Callers of this carriage read stdout as the
        # remote command's own output; a progress line there is a corrupted witness.
        printf 'ssh-macos: mDNS did not answer for %s (attempt %s of %s) -- retrying\n' \
            "$DSS_MACOS_HOST" "$_mac_try" "$_mac_tries" >&2
        sleep 2
        _mac_try=$((_mac_try + 1))
    done
    if [ -n "$resolved" ] && [ "$_mac_ttl" -gt 0 ] 2>/dev/null; then
        mkdir -p "$(dirname "$_mac_cache")" 2>/dev/null \
            && printf '%s %s\n' "$(date +%s)" "$resolved" > "$_mac_cache" 2>/dev/null || true
    fi
    unset -f _mac_resolve_once 2>/dev/null || true
    if [ -z "$resolved" ]; then
        {
          echo "ssh-macos: cannot resolve '$DSS_MACOS_HOST' after $_mac_tries attempt(s) from this host."
          # ⛔⛔ THE ORDER OF THESE CAUSES IS DELIBERATE AND IT WAS THE OTHER WAY ROUND
          # UNTIL 2026-08-31. "The Mac is OFF" led the list, and a P46 lane read it as
          # the verdict: it recorded "the Bonjour responder is up and sshd is not" in
          # [[D-SQLITE-CLI-BUILT-ON-NO-LEG]] and declared 22 of that row's 40
          # obligations BLOCKED. ✔MEASURED 2026-08-31, minutes apart, the Mac
          # answering on port 22 the whole time: this same message, twice, then a
          # 5-arm control resolving 192.168.0.71 on every arm. A DIAGNOSIS IS NOT A
          # MEASUREMENT, and the most confident-sounding cause was the wrong one.
          echo "  Most likely: mDNS did not answer — it is UNRELIABLE, not authoritative, and a miss"
          echo "               is INDISTINGUISHABLE from an absent host. Re-run, or raise"
          echo "               DSS_MACOS_RESOLVE_TRIES. ⚠ A miss is NOT evidence the Mac is down:"
          echo "               check it from Windows first — Test-NetConnection <name> -Port 22."
          echo "  Or:          no mDNS responder here at all (usual under WSL when the Windows"
          echo "               delegation below is also unavailable). Put a literal IP in DSS_MACOS_HOST."
          echo "  Or:          the Mac really is OFF or asleep — personal machine, not CI. Ask before waking it."
          # ★★ AND THE THIRD CAUSE IS THE ONE THAT DEFEATS THE ADVICE ON THE LINE
          # ABOVE, WHICH IS WHY IT IS PRINTED RATHER THAN LEFT TO BE REDISCOVERED.
          # D-SCRIPT-MACOS-HOST-OVERRIDE-DOES-NOT-CROSS-THE-WSLENV-BOUNDARY:
          # Windows→WSL forwards ONLY the variables `WSLENV` names, so
          # `$env:DSS_MACOS_HOST=...; wsl.exe -e bash ...` arrives here UNSET and
          # this script falls back to the `.local` name from `.secrets/macos.env`.
          # ✔MEASURED 2026-08-21: `DSS_MACOS_HOST=[<UNSET>] WSLENV=[<UNSET>]`
          # inside WSL after setting it in PowerShell — and mDNS then answered for
          # the rsync and failed for the build minutes later, so the run got FAR
          # enough to look like the override had worked.
          # ⓘ `run-gate.sh` already solves exactly this for CTEST_PARALLEL_LEVEL by
          # APPENDING to WSLENV; the same one-liner is the fix at any call site.
          if [ -n "${WSL_DISTRO_NAME:-}${WSL_INTEROP:-}" ]; then
            echo "  Or:          you are under WSL and set DSS_MACOS_HOST in the WINDOWS parent."
            echo "               Windows→WSL forwards only what WSLENV names. Prepend it:"
            echo '                 $env:DSS_MACOS_HOST="<ip>"; $env:WSLENV="DSS_MACOS_HOST"; wsl.exe -e bash ...'
            echo "               (or export it inside the WSL shell instead)."
          fi
        } >&2
        exit 3
    fi
    target=$resolved
fi

# ★★★ `--resolve` — PRINT THE ADDRESS AND EXIT, SO A CALLER CAN ASK THE QUESTION
# ONCE INSTEAD OF ONCE PER CARRIAGE CALL.
#
# ⚠ D-SCRIPT-MACOS-LEG-RERESOLVES-THE-HOST-AT-EVERY-CARRIAGE-CALL. A leg invokes
# this script several times — `leg-tree prepare`, the rsync, the build, the
# ctest — and each invocation re-runs the whole resolver above. Under WSL that
# resolver's only working arm is a hop out to the Windows mDNS resolver, so any
# ONE transient miss kills a leg that has already done minutes of work.
# ✔MEASURED 2026-08-28: `remote-leg --carriage macos` resolved for
# `leg-tree prepare` (reaching 192.168.0.71 and moving the clone from `301e2a63`
# + 2854 dirty paths to a pristine `73f74972`) and then FAILED to resolve for the
# rsync seconds later. ★ This file's own header already recorded the same shape
# from the other side — *"mDNS then answered for the rsync and failed for the
# build minutes later"* — as a symptom of a different defect; it is a property of
# ASKING REPEATEDLY, and that is what this mode removes.
#
# ⛔ IT DOES NOT PUT AN IP IN THE CONFIG, and that distinction is the whole design:
# the Mac is on DHCP, so a pinned address is wrong the moment the lease changes.
# The config keeps the NAME; the RUN gets one resolution, valid for its lifetime.
if [ "${1:-}" = "--resolve" ]; then
    printf '%s\n' "$target"
    exit 0
fi

args=(-o ConnectTimeout=10 -o BatchMode=yes)
# ★★ A KEY ON A WINDOWS DRIVE IS WORLD-READABLE TO WSL, AND ssh REFUSES IT.
# ✔MEASURED 2026-08-17 on the arm64-vps twin: the repo lives on `/mnt/c`, whose
# DrvFs mount maps Windows ACLs to mode 0444 regardless of `icacls`, so ssh says
# "UNPROTECTED PRIVATE KEY FILE … 0444 … too open" and then fails as "Permission
# denied (publickey)" — blaming the SERVER for a LOCAL file-mode problem. `chmod`
# cannot fix it (DrvFs ignores mode without the `metadata` mount option), so the
# only fix is a private COPY. The script does it; the caller never does anything
# manually. Kept identical to `ssh-arm64-vps.sh` — these are a capability pair.
if [ -n "$DSS_MACOS_KEY" ] && [ -r "$DSS_MACOS_KEY" ]; then
    _mode=$(stat -c '%a' "$DSS_MACOS_KEY" 2>/dev/null || echo '')
    case "$_mode" in
        ''|*00) : ;;
        *)
            chmod 600 "$DSS_MACOS_KEY" 2>/dev/null || true
            if [ "$(stat -c '%a' "$DSS_MACOS_KEY" 2>/dev/null || echo 600)" != "600" ]; then
                _priv=$(mktemp "${TMPDIR:-/tmp}/.dss-macos-key.XXXXXX") || {
                    echo "ssh-macos: cannot create a private copy of the key." >&2; exit 3; }
                chmod 600 "$_priv" && cat "$DSS_MACOS_KEY" > "$_priv" || {
                    rm -f "$_priv"; echo "ssh-macos: failed to stage a private key copy." >&2; exit 3; }
                trap 'rm -f "$_priv"' EXIT HUP INT TERM
                DSS_MACOS_KEY=$_priv
            fi
            ;;
    esac
    unset _mode
fi
[ -n "$DSS_MACOS_KEY" ] && [ -f "$DSS_MACOS_KEY" ] && args+=(-i "$DSS_MACOS_KEY")
[ "${DSS_MACOS_STRICT_HOSTKEY:-0}" != "1" ] && \
    args+=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
# ★★★ `--rsync <src...> <dest>` — A TRANSPORT THAT DOES NOT USE STDIN.
#
# ✔MEASURED 2026-08-18: pushing a tree with `tar czf - . | <carriage> 'tar xzf -'`
# DIED on the macOS host with "Unrecognized archive format", because that host's
# login profile CONSUMES STDIN before the remote tar ever reads it —
# `printf 'X' | ssh-macos.sh cat` returns NOTHING. The arm64 VPS survived the
# identical code path purely because its profile is quiet, which means the old
# transport's correctness depended on a remote shell's chattiness: not a property
# any caller can test, and not one worth depending on.
#
# ⚠ IT ALSO FIXES THE SILENT HALF. When the tar fallback failed, the caller could
# not tell a successful push from one whose archive arrived truncated — the remote
# tar reported the error, the local tar reported SIGPIPE, and the driver printed
# "FAIL push" carrying neither. This `exec`s rsync directly, so the caller's `$?`
# is rsync's own status and never a pipeline's last stage.
#
# `-a` preserves mtimes DELIBERATELY (ninja keys on them); a caller that needs a
# rebuild touches what moved, which every leg driver here already does.
# ★★★ `--push <local-dir> <remote-dir>` — THE TRANSPORT FOR A SHELL WITH NO LOCAL
# rsync, WHICH IS THE PRIMARY WINDOWS SHELL THIS PROJECT USES.
#
# ⚠ `--rsync` above `exec`s the LOCAL rsync, and ✔MEASURED 2026-08-25 there is no
# rsync in Git Bash (`tar`, `ssh` and `scp` are all present; `rsync` is not). So the
# macOS carriage was unreachable for a tree push from the one shell the Windows leg
# actually runs in, and the four-leg gate had no way to reach the Mac at all.
#
# ★★ THIS REVIVES THE TAR TRANSPORT THE `--rsync` BLOCK RETIRED, BECAUSE THE FACT
# THAT RETIRED IT IS NO LONGER TRUE. That block records: ✔MEASURED 2026-08-18, a
# `tar | ssh 'tar -x'` push DIED with "Unrecognized archive format" because the
# host's login profile CONSUMED STDIN, and `printf 'X' | ssh-macos.sh cat` returned
# NOTHING. ✔RE-MEASURED 2026-08-25 on the same host: `printf 'XSTDINX' | ssh-macos.sh
# cat` returns `XSTDINX`, and 1000 bytes of /dev/urandom arrive with an IDENTICAL
# md5 (`cc0d71470a8dbc8463d30278692af1d6` both ends). Stdin survives byte-exact.
# ⇒ the premise expired; the mode it justified did not. `--rsync` KEEPS its place
# for callers that have rsync, and this is the sibling for callers that do not.
#
# ★★ AND IT DOES NOT REINTRODUCE THE SILENT HALF, which is the real reason the old
# tar transport deserved retiring. That header's second complaint stands on its own:
# when the old push failed, the caller could not distinguish success from a
# TRUNCATED archive — the remote tar reported the error, the local tar reported
# SIGPIPE, and the driver printed "FAIL push" carrying neither. Two defences here:
#   * BOTH ends are checked via PIPESTATUS, not just the pipeline's last stage;
#   * the remote side emits a WITNESS token only after tar exits 0, and this refuses
#     unless that token comes back — the same tool-emitted-witness discipline
#     `scripts/run-gate` uses, for the same reason: an exit status can be produced by
#     the wrong process, a witness cannot.
#
# ⚠ EXCLUDES ARE ANCHORED (`./build`, never `build`). ✔MEASURED and recorded in
# `scripts/wsl-leg`: an UNANCHORED exclude once silently skipped
# `src/program/build_scripts.cpp`, and a gate leg was configured against a tree
# missing a changed `.cpp`. An exclude that over-matches does not fail — it produces
# a green run over the wrong source, which is the failure this project cares most about.
#
# ⚠ tar PRESERVES MTIMES, exactly as `rsync -a` does, so a pushed source whose mtime
# lands behind an existing build output makes ninja skip it. A caller that needs a
# rebuild builds CLEAN; do not "fix" that here by discarding times.
if [ "${1:-}" = "--push" ]; then
    shift
    # ★★★ `--prune` MAKES THE PUSH A SYNC. Without it this mode is an ACCUMULATION: tar
    # extraction never deletes, so a file removed locally lives forever on the remote and
    # the gate tests a tree that exists nowhere. ✔MEASURED 2026-08-25 -- see the header.
    # ⚠ It is OPT-IN and named, for the same reason `--reset-to` is: this deletes files on
    # somebody's machine, and a transport that does that as a silent default is a transport
    # that eventually deletes the wrong tree. A gate LEG passes it, because a leg's whole
    # contract is "test THIS tree"; an ad-hoc push should not have to.
    _prune=0
    if [ "${1:-}" = "--prune" ]; then _prune=1; shift; fi
    if [ $# -ne 2 ]; then
        echo "ssh-macos: --push needs exactly <local-dir> <remote-dir> (optionally after --prune)" >&2
        exit 2
    fi
    _src=$1
    _dst=$2
    if [ ! -d "$_src" ]; then
        echo "ssh-macos: --push source '$_src' is not a directory" >&2
        exit 2
    fi
    case "$_dst" in
        ""|"/"|"~"|"~/"|"\$HOME") echo "ssh-macos: --push refuses destination '$_dst'" >&2; exit 2 ;;
    esac
    # ⚠⚠ THE DESTINATION GOES TO THE REMOTE SHELL **UNQUOTED**, AND THAT IS DELIBERATE —
    # A TILDE ONLY EXPANDS UNQUOTED. ✔MEASURED 2026-08-25, and it is the exact
    # silent-success this mode exists to prevent: with the destination single-quoted,
    # `--push <src> '~/dss-pushtest'` reported OK, the witness fired, and the files
    # landed in a directory literally NAMED `~` — tar genuinely succeeded, just not
    # where the caller asked. An exit status and a success token both said yes.
    # ⇒ unquoted, and therefore a destination carrying whitespace or a shell
    # metacharacter is REFUSED rather than mangled. A remote path is the remote
    # shell's to resolve; the caller's job is to hand it something unambiguous.
    case "$_dst" in
        *[[:space:]]*|*';'*|*'&'*|*'|'*|*'`'*|*'$('*|*'>'*|*'<'*)
            echo "ssh-macos: --push refuses a destination with whitespace or shell metacharacters: '$_dst'" >&2
            exit 2 ;;
    esac
    _witness="DSS_PUSH_OK_$$"
    args+=("$DSS_MACOS_USER@$target")
    # ★ THE WITNESS CARRIES THE RESOLVED PATH, so "it worked" and "it worked in the
    # place you asked for" are ONE check rather than two. The measurement above is
    # why: a witness that only proves success is satisfied by success elsewhere.
    # ⚠ `./.claude/worktrees` IS EXCLUDED, AND IT IS THE LARGEST EXCLUDE HERE.
    # ✔MEASURED 2026-08-25: the Mac held 16,312 files against a local 6,660, and 9,638 of
    # the difference was `.claude/worktrees/**` -- a FULL COPY OF THE REPO PER LIVE AGENT,
    # shipped to the gate host on every push and left there. That is not merely transport
    # waste: the examples runner GLOBS `examples/<lang>/*`, and a worktree carries its own
    # `examples/` tree, so a gate host that holds one can run a corpus that belongs to
    # somebody's uncommitted lane.
    #
    # ★ `-m` ON EXTRACT (do not extract modification time), for TWO reasons.
    # (1) It retires D-SYNC-RSYNC-PRESERVED-MTIME-DEFEATS-THE-REBUILD at the source: a
    #     pushed file can no longer land BEHIND an existing build output, so ninja can no
    #     longer silently skip it. The mtime hazard was the whole reason the macOS leg
    #     builds clean.
    # (2) It is what makes `--prune` below SAFE -- "was this file in the archive" becomes
    #     "is it newer than the stamp", which is a question the remote can answer without
    #     trusting a manifest that could itself have arrived truncated.
    # ★★★ AND THE EXCLUDE LIST IS DERIVED, NOT TYPED. This enumeration was one of
    # four, and every one of them spelled `node_modules` ANCHORED at the top level,
    # which `.kilo/node_modules` is not. ✔MEASURED 2026-08-26 by running THIS list
    # against the live tree: it ships **9,284 members where the derivation ships
    # 3,583** -- 3,938 of `.kilo/node_modules`, 1,431 of `test-scratch/`, 268 of
    # `.temp/`, 49 `__pycache__` entries, `.claude/settings.local.json`, and FIVE
    # FILES OF `.secrets/` -- connection data and key PATHS for real machines, in a
    # repository slated to go public. This list named none of them.
    # ⚠ The Mac itself holds only 4 `.kilo` files, because the pushes that built it
    # ran through the `.ps1`/bsdtar path; the leak here is latent, not historical.
    # A host is not interchangeable with the list that feeds it.
    # D-SCRIPT-CARRIAGE-EXCLUDES-ARE-A-HAND-LIST-AND-MISS-NESTED-IGNORED-TREES
    # ⓘ `.git` stays a POLICY withhold (`--also`): this carriage syncs no history,
    # and the remote `.git` would otherwise name an unrelated commit.
    _excl="${TMPDIR:-/tmp}/ssh-macos-excludes.$$"
    if [ -z "$PY" ]; then
        echo "ssh-macos: --push needs python3/python on PATH to derive the exclude list" >&2
        exit 1
    fi
    if ! "$PY" "$REPO/scripts/carriage-excludes/carriage-excludes.py" \
            --format tar --repo "$_src" --also .git --out "$_excl"; then
        echo "ssh-macos: carriage-excludes refused -- refusing to push a list it would not vouch for" >&2
        rm -f "$_excl"; exit 1
    fi
    tar -c -C "$_src" -X "$_excl" \
        -f - . \
      | ssh "${args[@]}" "$remote_path_stmt" "mkdir -p $_dst && cd $_dst && mkdir -p build && touch build/.dss-push-stamp && sleep 1 && tar -x -m -f - && echo $_witness:\$(pwd -P)" \
      > "${TMPDIR:-/tmp}/ssh-macos-push.$$" 2>&1
    _st=("${PIPESTATUS[@]}")
    _out=$(cat "${TMPDIR:-/tmp}/ssh-macos-push.$$" 2>/dev/null)
    rm -f "${TMPDIR:-/tmp}/ssh-macos-push.$$" "$_excl"
    if [ "${_st[0]}" != "0" ] || [ "${_st[1]}" != "0" ]; then
        echo "ssh-macos: --push FAILED (local tar rc=${_st[0]}, remote rc=${_st[1]})" >&2
        echo "$_out" >&2
        exit 1
    fi
    _landed=$(printf '%s\n' "$_out" | sed -n "s/^.*$_witness://p" | tail -1)
    if [ -z "$_landed" ]; then
        echo "ssh-macos: --push produced no witness — the archive may have arrived TRUNCATED" >&2
        echo "$_out" >&2
        exit 1
    fi
    case "$_landed" in
        /*) ;;
        *) echo "ssh-macos: --push witness carried a non-absolute path '$_landed'" >&2; exit 1 ;;
    esac
    echo "ssh-macos: --push OK -> $_landed"

    if [ "$_prune" = "1" ]; then
        # The stamp was created BEFORE the archive was extracted and every extracted file
        # carries a fresh mtime (`-m` above), so "older than the stamp" is exactly "was not
        # in the archive". The script goes over STDIN as a FILE, never quoted into a `-c`
        # string -- that is the trap that once turned a variable into `rsync -a --delete / /`.
        _prune_body=$(cat <<'PRUNE_EOF'
set -uo pipefail
cd "$D" || { echo "[X] prune: $D missing"; exit 1; }
S=build/.dss-push-stamp
[ -f "$S" ] || { echo "[X] prune: no push stamp -- REFUSING (cannot tell fresh from stale)"; exit 1; }

# ⚠ THE RAIL THAT MATTERS. If the archive arrived truncated, almost nothing is fresh and
# a prune would delete the tree it was supposed to update. Refuse loudly instead.
_fresh=$(find . -type f -newer "$S" \
    -not -path './build/*' -not -path './.git/*' -not -path './scratchpad/*' \
    -not -path './target/*' -not -path './.venv/*' -not -path './node_modules/*' \
    | wc -l | tr -d ' ')
if [ "${_fresh:-0}" -lt 100 ]; then
    echo "[X] prune: only ${_fresh:-0} file(s) look freshly extracted -- REFUSING."
    echo "    A truncated archive here would delete the tree this push was updating."
    exit 1
fi

find . -type f ! -newer "$S" \
    -not -path './build/*' -not -path './.git/*' -not -path './scratchpad/*' \
    -not -path './target/*' -not -path './.venv/*' -not -path './node_modules/*' \
    -print -delete > "$S.pruned" 2>/dev/null
echo "PRUNED=$(wc -l < "$S.pruned" | tr -d ' ') FRESH=$_fresh"
head -25 "$S.pruned"
rm -f "$S" "$S.pruned"

# The files are gone; the directories that held them are not. A hollow
# `.claude/worktrees/agent-*` still reads as "this host holds worktrees" to the next
# person who looks. `-delete` implies `-depth` on BSD find, so nested empties collapse
# in one pass. Every empty directory here is a leftover: git tracks no empty directory,
# so anything the repo genuinely wants arrives holding a file.
_dirs=$(find . -type d -empty \
    -not -path './build' -not -path './build/*' -not -path './.git' -not -path './.git/*' \
    -not -path './scratchpad' -not -path './scratchpad/*' -not -path './target/*' \
    -not -path './.venv/*' -not -path './node_modules/*' -not -path '.' \
    -print -delete | wc -l | tr -d ' ')
echo "PRUNED_DIRS=$_dirs"
PRUNE_EOF
)
        printf 'D=%s\n%s\n' "$_dst" "$_prune_body" \
          | ssh "${args[@]}" "$remote_path_stmt" 'bash -s' > "${TMPDIR:-/tmp}/ssh-macos-prune.$$" 2>&1
        _pout=$(cat "${TMPDIR:-/tmp}/ssh-macos-prune.$$" 2>/dev/null)
        rm -f "${TMPDIR:-/tmp}/ssh-macos-prune.$$"
        # A witness, not an exit code: the status of a pipeline is the last stage's, and
        # this project has been bitten by that exact substitution before.
        case "$_pout" in
            *PRUNED=*) printf '%s\n' "$_pout" | sed -n '1,26p' ;;
            *) echo "ssh-macos: --prune produced no PRUNED= witness -- the remote tree may still hold stale files" >&2
               printf '%s\n' "$_pout" >&2
               exit 1 ;;
        esac
    fi
    exit 0
fi

if [ "${1:-}" = "--rsync" ]; then
    shift
    if [ $# -lt 2 ]; then
        echo "ssh-macos: --rsync needs at least <src> and <dest>" >&2
        exit 2
    fi
    # Build the rsh command with EACH option quoted: the key path can contain
    # spaces (it does under a Windows user profile) and rsync word-splits the
    # `-e` string, so joining the array with bare spaces would corrupt it.
    _rsh=ssh
    for _o in "${args[@]}"; do
        _rsh="$_rsh '$_o'"
    done
    # The LAST argument is the remote destination; everything before it is local.
    _dest=${!#}
    set -- "${@:1:$#-1}"
    exec rsync -a --delete -e "$_rsh" "$@" "$DSS_MACOS_USER@$target:$_dest"
fi

args+=("$DSS_MACOS_USER@$target")
# An EMPTY payload is an interactive session; prefixing it would turn a login into a
# one-shot `export` and hand back a shell that exits immediately.
if [ $# -gt 0 ]; then
    [ -n "$remote_path_stmt" ] && args+=("$remote_path_stmt")
    args+=("$@")
fi

ssh "${args[@]}"
exit $?
