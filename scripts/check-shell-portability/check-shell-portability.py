#!/usr/bin/env python3
# PURPOSE: refuse a tracked shell script that cannot run on bash 3.2 without declaring it.
"""Shell-portability gate: macOS is a supported host, and macOS bash is 3.2 forever.

WHY THIS GUARD EXISTS -- a green suite that had never run
--------------------------------------------------------
2026-08-22, `macos-clang-release` on GitHub Actions: `anchor_registry_guard` FAILED,
and not on the tree it guards. It died inside its own self-test with

    check-anchor-registry.sh: command substitution: syntax error near unexpected
    token `newline'
    anchor-registry: SELF-TEST arm 'missing-root-refuses-and-says-so' FAILED

The line it choked on was an assertion arm shaped like

    "$(case "${got}" in *"does not exist"*) echo yes ;; *) echo "${got}" ;; esac)"

bash 3.2 does not recursively parse a command substitution: it scans forward for the
matching `)`, counting parens. The `)` that CLOSES A `case` PATTERN therefore ends the
`$( ... )` early, the arm expands to the LITERAL text ` echo yes ;; *) echo ... esac)`,
and every arm after it in that file is dead. ✔MEASURED that day on macOS 26.5.2,
`/bin/bash` 3.2.57 -- and MEASURED again under bash 5.2.21, where the identical line
yields `yes`. The POSIX-optional LEADING `(` on each pattern balances the count and
works on 3.2, 4.x and 5.x alike; that one character is the entire fix.

star star star THE PART THAT MAKES THIS A GUARD AND NOT A COMMENT: `bash -n` IS BLIND TO IT.
✔MEASURED in the same run -- the probe file passes `bash -n` (exit 0) under bash 3.2 and
fails only when the substitution is EXPANDED, because that is when 3.2 parses the text it
extracted. So the obvious instrument ("syntax-check every script under the old shell")
cannot see this class at all, and the only other oracle is a macOS host actually running
the script. This repository has one macOS CI leg; before 2026-08-22 that leg had never
completed a run on this branch, so a guard nobody could run had been shipping since it
was written. (D-SCRIPT-CASE-IN-COMMAND-SUBSTITUTION-BREAKS-BASH-3-2)

WHAT IS CHECKED, AND WHY EXACTLY THESE TWO THINGS
-------------------------------------------------
RULE 1 -- `bash-3.2-truncates-this-command-substitution`.
For every `$(` in a tracked `.sh`, the span bash 3.2 would extract (scan forward,
counting parens that are not inside quotes or a comment) must contain BALANCED
`case`/`esac` keywords. An unbalanced `case` means 3.2 stopped inside a `case`
statement -- which is the measured failure, stated as the property rather than as a
pattern-match on the source text. A `case` counts only when an `in` follows it in the
span, because a bare word is not a keyword and `$(echo case)` must not red.
The rule is deliberately NOT "every case arm must carry a leading paren": a `case`
that is not inside a substitution is completely safe on 3.2, and this repository has
dozens of them. A guard that reddened those would demand churn to prevent nothing.

RULE 2 -- `undeclared-bash-4-dependency`.
A file using a bash-4+ construct must carry a `BASH_VERSINFO` gate, and the gate must
come BEFORE the first such use. ✔MEASURED 2026-08-22 on bash 3.2.57 vs bash 5.2.21,
one `bash -c` per construct, comparing OUTPUT and not merely exit status -- 16 of 17
constructs are unsupported on 3.2 and all 17 work on 5.2. The measurement is in
BASH4_CONSTRUCTS below, one row per construct, and the reason it had to be a
measurement is the third column: SIX of them leave the SHELL'S EXIT STATUS AT ZERO.

    mapfile / readarray     -> `mapfile: command not found`, script continues
    declare -A / local -A   -> `declare: -A: invalid option`, and the name then
                               behaves as an INDEXED array, so every key subscripts
                               to 0 and the table silently holds one entry
    local -n / wait -n      -> `invalid option`, script continues
    case inside $( ... )    -> no error from the script at all; the substitution
                               simply yields the wrong text

That is the whole argument for a gate rather than a convention: five of these six
produce a WRONG ANSWER rather than a failure, on the one host that cannot be checked
from any other host.

star WHY A `.py` AND NOT A `.sh`/`.ps1` PAIR. Same reason as `check-scripts-index`:
Python already runs on every host here, so a twin would be one contract implemented
twice -- and this guard's whole subject is a shell dialect, so writing it in shell
would put the instrument inside the population it measures. Deliberate, per the
`/dss-cycle` pairing rule; not a forgotten sibling.

star THE SCAN HAS A FLOOR. A scan that silently finds nothing must RED, not pass.
SCAN_FLOOR is 15 against ✔ 24 tracked `.sh` files (measured 2026-08-22) -- set below
the count with room for a script to be retired, never AT it: a floor equal to the
population reds on every honest deletion.

star WHAT IS SCANNED IS THE TREE ON THIS DISK, MINUS WHAT `.gitignore` EXCLUDES --
and BOTH halves of that were measured, in opposite directions, hours apart.
✔A plain walk reported six violations under the gitignored `scratchpad/`, which holds
verbatim lane backups of the very scripts this cycle repaired: a guard that reds on a
developer's scratch copy is a guard that gets switched off.
✔And the obvious correction -- scan what `git ls-files` names -- was WORSE, caught on
the macOS gate leg: that carriage's checkout sits at an old commit with the working
tree rsynced over it, so the INDEX still named seven paths the `tools/` consolidation
had deleted, and the guard reported seven violations on a host where nothing was
wrong. **The index describes a COMMIT; this guard is about the TREE.** So the tree is
walked and git is asked only which of those files are IGNORED.

Usage:
    check-shell-portability.py              # verify the tree, then prove it can fail
    check-shell-portability.py --selftest   # only the self-test
    check-shell-portability.py --list       # what was scanned, and what each file uses

Exit codes: 0 clean - 1 a violation - 2 usage or a collapsed scan.
"""

import io
import os
import subprocess
import re
import sys

EXIT_OK = 0
EXIT_VIOLATION = 1
EXIT_USAGE = 2
EXIT_COLLAPSE = 2

SCAN_FLOOR = 15

# ★★ ONE ANSWER TO "IS THIS FILE OURS", AND IT IS `.gitignore`. The only name skipped
# before the ignore query is `.git` itself -- enormous, never a home for a repository
# script, and the thing the query is asked THROUGH.
# ⚠ An earlier draft also hard-skipped `build`, `.venv`, `node_modules` and
# `__pycache__` at the scan root. Every one of those is already gitignored here, so the
# list was a SECOND answer to the question `.gitignore` already answers -- and this
# cycle exists because two answers to one question drift. The failure it invites is the
# silent kind: the day a `build/` directory stops being ignored, a hardcoded skip hides
# every script in it and nothing says so.
# ✔MEASURED: walking `build/` too costs 0.12s against 0.04s on this tree (128 `.sh`
# found instead of 66), and the ignore query removes all of them. That is not a price
# worth a second source of truth.
EXCLUDED_TOP = (".git",)

CODE, DQUOTE, SQUOTE, COMMENT, HEREDOC = "code", "dquote", "squote", "comment", "heredoc"

# ── the measured table ───────────────────────────────────────────────────────
# (rule name, compiled pattern, what bash 3.2 does)
# ✔EVERY ROW MEASURED 2026-08-22: bash 3.2.57 (macOS 26.5.2) vs bash 5.2.21 (WSL),
# one `bash -c` per row, verdict taken from the OUTPUT and not the exit status.
BASH4_CONSTRUCTS = (
    ("mapfile", re.compile(r"(?<![\w./-])(?:mapfile|readarray)(?![\w-])"), (CODE,),
     "mapfile: command not found -- and the script CONTINUES with an empty array"),
    ("associative-array",
     re.compile(r"(?<![\w./-])(?:declare|local|typeset)\s+-[A-Za-z]*A(?![\w-])"), (CODE,),
     "declare: -A: invalid option -- and the name then behaves as an INDEXED array,"
     " so every key subscripts to 0"),
    ("nameref",
     re.compile(r"(?<![\w./-])(?:declare|local|typeset)\s+-[A-Za-z]*n(?![\w-])"), (CODE,),
     "declare: -n: invalid option -- and the script CONTINUES with a plain variable"),
    ("case-expansion",
     re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]*\])?(?:\^\^?|,,?)"), (CODE, DQUOTE),
     "bad substitution"),
    ("parameter-transform",
     re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]*\])?@[QEPAaKkLUu]\}"), (CODE, DQUOTE),
     "bad substitution"),
    ("append-stdout-stderr", re.compile(r"&>>"), (CODE,),
     "syntax error near unexpected token `>'"),
    ("pipe-both", re.compile(r"(?<![|&>])\|&"), (CODE,),
     "syntax error near unexpected token `&'"),
    ("coproc", re.compile(r"(?<![\w./-])coproc(?![\w-])"), (CODE,),
     "syntax error"),
    ("globstar", re.compile(r"(?<![\w./-])shopt\s+(?:-[a-z]+\s+)*globstar(?![\w-])"), (CODE,),
     "shopt: globstar: invalid shell option name"),
    ("wait-n", re.compile(r"(?<![\w./-])wait\s+-[a-z]*n(?![\w-])"), (CODE,),
     "wait: -n: invalid option -- and the script CONTINUES"),
    ("printf-time", re.compile(r"%\([^)]*\)T"), (CODE, DQUOTE),
     "printf: `(': invalid format character"),
    ("test-v", re.compile(r"\[\[\s+-v\s"), (CODE,),
     "conditional binary operator expected"),
)

GATE = re.compile(r"BASH_VERSINFO")

# `case` and `esac` as WORDS. A variable named `casement` or a path `.../esac/` must
# not count -- the same word-boundary discipline the anchor guard learned the hard way.
CASE_WORD = re.compile(r"(?<![\w./-])case(?![\w-])")
ESAC_WORD = re.compile(r"(?<![\w./-])esac(?![\w-])")
IN_WORD = re.compile(r"(?<![\w./-])in(?![\w-])")


class Collapse(Exception):
    """The scan found less than the floor: the SCAN changed, not the tree."""


# ── the lexer ────────────────────────────────────────────────────────────────
# It answers one question per character -- CODE, DQUOTE, SQUOTE, COMMENT or
# HEREDOC -- and both rules are expressed on top of that, so neither has to
# re-derive quoting. A `declare -A` written inside an explanatory comment (this
# repository's harness headers are full of them) is then not a use, and a `)`
# inside `'...'` is not a paren.
#
# star star THE ONE THING THAT MAKES THIS MORE THAN A REGEX: `$(` RESETS THE QUOTE
# STATE, AND `)` RESTORES IT. In `"$(LC_ALL=C awk '/x=\(\)/ {p=1}' "$f")"` the awk
# program IS single-quoted, because the `$(` opened a fresh command context inside
# the double-quoted string -- so the `\(` in it is inert. ✔MEASURED while writing
# this guard: without the frame stack, that one line in test-driver-contracts.sh
# read as a 21,989-character runaway substitution and was reported as a violation
# it is not. A guard's first false positive is the one that teaches everyone to
# ignore it.
#
# star PARENS ARE COUNTED IN `CODE` ONLY -- a `(` inside quotes is not a paren to
# bash either, which is exactly why `*'...=('*)` in a case pattern still breaks 3.2:
# the OPENING paren was quoted and the CLOSING one was not.

_HEREDOC_START = re.compile(r"<<-?\s*(?:'([^']*)'|\"([^\"]*)\"|([A-Za-z_][A-Za-z0-9_]*))")
# `#` opens a comment only at the START OF A WORD, so the test is on the character
# IMMEDIATELY before it. ⚠ An earlier draft asked about the last non-space CODE
# character and included `{` in the set, which made `${#lines[@]}` read as a comment
# and hid the remainder of its line -- a FALSE NEGATIVE, the one direction a guard
# must not fail in. Self-test arm 20 pins it.
_COMMENT_OPENS_AFTER = " \t\n;&|()"


class _Frame(object):
    """One open `$( ... )`: where it started, and the quoting to restore at its `)`."""

    __slots__ = ("start", "quote", "depth")

    def __init__(self, start, quote):
        self.start = start
        self.quote = quote
        self.depth = 0


def classify(text):
    """Return (kind, substitutions).

    `kind` holds one state per character. `substitutions` holds (start, end) for
    every `$( ... )`, where `end` is the `)` at which the parenthesis count returns
    to zero -- which is exactly where bash 3.2 stops, and therefore the span rule 1
    has to reason about.
    """
    n = len(text)
    kind = [CODE] * n
    subs = []
    frames = []
    quote = None                 # None, '"' or "'"
    pending_heredocs = []
    i = 0
    while i < n:
        c = text[i]

        if quote == "'":
            kind[i] = SQUOTE
            if c == "'":
                quote = None
            i += 1
            continue

        if quote == '"':
            kind[i] = DQUOTE
            if c == "\\" and i + 1 < n and text[i + 1] in '"\\$`\n':
                kind[i + 1] = DQUOTE
                i += 2
                continue
            if c == '"':
                quote = None
                i += 1
                continue
            if c == "$" and text.startswith("$((", i):
                i = _skip_arithmetic(text, i, kind)
                continue
            if c == "$" and i + 1 < n and text[i + 1] == "(":
                kind[i] = kind[i + 1] = CODE
                frames.append(_Frame(i, '"'))
                quote = None
                i += 2
                continue
            if c == "`":
                i = _skip_backticks(text, i, kind)
                continue
            i += 1
            continue

        # ── CODE ──
        kind[i] = CODE
        if c == "\\":
            if i + 1 < n:
                kind[i + 1] = CODE
            i += 2
            continue
        if c == "\n":
            i += 1
            if pending_heredocs:
                i = _consume_heredocs(text, i, pending_heredocs, kind)
            continue
        if c == "'":
            quote = "'"
            kind[i] = SQUOTE
            i += 1
            continue
        if c == '"':
            quote = '"'
            kind[i] = DQUOTE
            i += 1
            continue
        if c == "#" and (i == 0 or text[i - 1] in _COMMENT_OPENS_AFTER):
            j = text.find("\n", i)
            if j < 0:
                j = n
            for k in range(i, j):
                kind[k] = COMMENT
            i = j
            continue
        if c == "`":
            i = _skip_backticks(text, i, kind)
            continue
        if text.startswith("$((", i):
            i = _skip_arithmetic(text, i, kind)
            continue
        if c == "$" and i + 1 < n and text[i + 1] == "(":
            frames.append(_Frame(i, None))
            i += 2
            continue
        if c == "(":
            if frames:
                frames[-1].depth += 1
            i += 1
            continue
        if c == ")":
            if frames:
                if frames[-1].depth:
                    frames[-1].depth -= 1
                else:
                    f = frames.pop()
                    subs.append((f.start, i))
                    quote = f.quote
            i += 1
            continue
        if text.startswith("<<", i) and not text.startswith("<<<", i):
            m = _HEREDOC_START.match(text, i)
            if m:
                pending_heredocs.append(m.group(1) or m.group(2) or m.group(3))
                i = m.end()
                continue
        i += 1

    # An unterminated `$(` is itself a defect, but it is a PARSE defect every shell
    # reports; recording the span to end-of-file keeps rule 1 total rather than
    # silently dropping the tail of a file it could not follow.
    for f in frames:
        subs.append((f.start, n - 1))
    subs.sort()
    return kind, subs


def _skip_arithmetic(text, i, kind):
    """`$(( ... ))` is not a command substitution; mark it and step over it."""
    n = len(text)
    j = i + 3
    depth = 2
    kind[i] = kind[i + 1] = kind[i + 2] = CODE
    while j < n and depth:
        kind[j] = CODE
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
        j += 1
    return j


def _skip_backticks(text, i, kind):
    """Old-style `` `...` `` substitution: opaque, and this repository has none."""
    n = len(text)
    j = i + 1
    kind[i] = CODE
    while j < n:
        kind[j] = CODE
        if text[j] == "\\":
            j += 2
            continue
        if text[j] == "`":
            return j + 1
        j += 1
    return n


def _consume_heredocs(text, i, pending, kind):
    """Mark the bodies of every here-doc queued on the line that just ended."""
    n = len(text)
    while pending:
        term = pending.pop(0)
        while i < n:
            eol = text.find("\n", i)
            if eol < 0:
                eol = n
            if text[i:eol].strip() == term:
                i = eol + 1
                break
            for k in range(i, min(eol + 1, n)):
                kind[k] = HEREDOC
            i = eol + 1
        else:
            break
    return i


# ── rule 1 ───────────────────────────────────────────────────────────────────
def truncated_substitutions(text, kind, subs):
    """Every `$( ... )` bash 3.2 would cut short inside a `case`.

    The span is the one the lexer closed by paren count -- the same place 3.2 stops.
    Inside it, `case` and `esac` must balance: an unclosed `case` means the
    substitution ended in the middle of one, which is the measured failure.
    """
    out = []
    for start, end in subs:
        span = text[start:end + 1]
        span_kind = kind[start:end + 1]
        if _count_case_statements(span, span_kind) > _count_word(span, span_kind, ESAC_WORD):
            out.append((start, span))
    return out


def _count_word(span, span_kind, pattern):
    return sum(1 for m in pattern.finditer(span) if span_kind[m.start()] == CODE)


def _count_case_statements(span, span_kind):
    """`case` keywords that actually OPEN a case statement.

    ⚠ A bare word is not a keyword: `$(echo case)` and `$(grep -o case f)` both put
    `case` in code position with no statement anywhere. Requiring the `in` that every
    `case` statement carries keeps the shape this guard exists for -- in the failing
    form `$(case "$g" in *x*)` the `in` sits INSIDE the truncated span -- while
    refusing to red an honest substitution that merely mentions the word.
    """
    total = 0
    for m in CASE_WORD.finditer(span):
        if span_kind[m.start()] != CODE:
            continue
        for w in IN_WORD.finditer(span, m.end()):
            if span_kind[w.start()] == CODE:
                total += 1
                break
    return total


# ── rule 2 ───────────────────────────────────────────────────────────────────
def bash4_uses(text, kind):
    """Every bash-4+ construct used for real, in source order.

    star THE STATES A CONSTRUCT CAN LIVE IN ARE PART OF THE TABLE, not a blanket
    rule. An EXPANSION (`"${v^^}"`, `"${v@Q}"`, `printf "%(%F)T"`) is a use inside
    double quotes; a COMMAND or a REDIRECTION (`mapfile`, `declare -A`, `&>>`) inside
    double quotes is a string that happens to read like one. Treating both the same
    is how a guard earns a suppression comment.
    """
    hits = []
    for name, pattern, states, symptom in BASH4_CONSTRUCTS:
        for m in pattern.finditer(text):
            if kind[m.start()] in states:
                hits.append((m.start(), name, symptom, m.group(0)))
    hits.sort()
    return hits


def gate_offset(text, kind):
    """Offset of the first `BASH_VERSINFO` that is code rather than prose."""
    for m in GATE.finditer(text):
        if kind[m.start()] in (CODE, DQUOTE):
            return m.start()
    return None


# ── the walk ─────────────────────────────────────────────────────────────────
def shell_scripts(root):
    """Every `.sh` on THIS disk that the repository does not ignore.

    ★★ THE QUESTION IS "IS THIS FILE IGNORED", NOT "IS IT IN THE INDEX", AND THE
    DIFFERENCE IS NOT ACADEMIC. ✔MEASURED 2026-08-22 on the macOS gate leg: the first
    version asked `git ls-files`, and the carriage's checkout sits at an OLD COMMIT
    with the working tree rsynced over it — so the index still named `tools/*.sh` and
    `scripts/build/local-build.sh`, paths the `tools/` consolidation deleted in P17.
    The guard reported SEVEN violations against files that do not exist, on a host
    where nothing was wrong. **A guard that reds on a host difference is worse than no
    guard**, because the next person to see it red will be right to ignore it.

    The index describes a COMMIT; `.gitignore` describes the TREE. This guard is about
    the scripts that are actually here, so the tree is the right subject and the only
    thing git is asked is which of them are ignored — which is also what keeps the
    gitignored `scratchpad/` lane backups (verbatim copies of the very scripts this
    cycle repaired) from reddening a developer's checkout.

    ⓘ `git check-ignore` deliberately does NOT report a TRACKED file as ignored, so a
    tracked file that happens to match an ignore rule still gets scanned. That is the
    behaviour we want and the reason `--no-index` is not passed.
    """
    found = _walk_shell_scripts(root)
    ignored = _git_ignored(root, found)
    return [p for p in found if p not in ignored]


def _git_ignored(root, paths, runner=None):
    """The subset of `paths` that this repository ignores.

    Returns an empty set when `root` is not a git checkout — the self-test drives
    fixture trees that are not repositories, and a plain directory ignores nothing.
    ⚠ A git checkout whose `check-ignore` FAILS is a broken instrument, not an empty
    answer: it raises `Collapse` rather than quietly scanning everything, because the
    quiet version would red on exactly the scratch copies this call exists to skip.
    """
    # `os.path.exists`, not `isdir`: in a git WORKTREE (this repository uses them --
    # see the cycle skill's worktrees reference) `.git` is a FILE pointing at the real
    # git dir. Testing for a directory would silently skip the ignore filter there and
    # scan every ignored scratch copy.
    if not paths or not os.path.exists(os.path.join(root, ".git")):
        return set()
    run = runner or subprocess.run
    payload = "\0".join(os.path.relpath(p, root).replace("\\", "/") for p in paths)
    try:
        proc = run(["git", "-C", root, "check-ignore", "-z", "--stdin"],
                   input=payload.encode("utf-8"),
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except (OSError, ValueError) as exc:
        raise Collapse("cannot ask git what is ignored under %s (%s); refusing to scan"
                       " a tree whose ignored files it cannot identify" % (root, exc))
    # 0 = some paths are ignored, 1 = none are. Anything else is a broken query.
    if proc.returncode not in (0, 1):
        raise Collapse("`git check-ignore` under %s exited %d (%s); refusing to scan a"
                       " tree whose ignored files it cannot identify"
                       % (root, proc.returncode,
                          proc.stderr.decode("utf-8", "replace").strip() or "no message"))
    out = proc.stdout.decode("utf-8", "replace")
    return {os.path.normpath(os.path.join(root, rel)) for rel in out.split("\0") if rel}


def _walk_shell_scripts(root):
    found = []
    for entry in sorted(os.listdir(root)):
        if entry in EXCLUDED_TOP:
            continue
        path = os.path.join(root, entry)
        if os.path.isdir(path):
            for dirpath, dirs, files in os.walk(path):
                dirs.sort()
                for fn in sorted(files):
                    if fn.endswith(".sh"):
                        found.append(os.path.normpath(os.path.join(dirpath, fn)))
        elif entry.endswith(".sh"):
            found.append(os.path.normpath(path))
    return found


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def inspect(path):
    text = io.open(path, encoding="utf-8", newline="").read()
    kind, subs = classify(text)
    return text, kind, subs


def check(root, floor=SCAN_FLOOR, out=sys.stdout):
    scripts = shell_scripts(root)
    if len(scripts) < floor:
        raise Collapse("found only %d shell script(s) under %s, floor is %d"
                       % (len(scripts), root, floor))

    violations = []
    for path in scripts:
        rel = os.path.relpath(path, root).replace("\\", "/")
        try:
            text, kind, subs = inspect(path)
        except (OSError, UnicodeDecodeError) as exc:
            violations.append((rel, 0, "unreadable",
                               "cannot be read as UTF-8 (%s); a script this guard cannot"
                               " read is a script it cannot vouch for" % exc))
            continue

        for offset, span in truncated_substitutions(text, kind, subs):
            excerpt = " ".join(span[:110].split())
            violations.append((
                rel, line_of(text, offset), "bash-3.2-truncates-this-command-substitution",
                "a `case` inside `$( ... )` whose pattern is not `(`-prefixed:\n"
                "        %s\n"
                "      bash 3.2 stops the substitution at the `)` that closes the case\n"
                "      PATTERN, so this expands to literal text and everything after it\n"
                "      in the file is dead. Put a `(` in front of every pattern in it:\n"
                "      `in (*foo*) ... ;; (*) ... ;; esac` -- valid on 3.2, 4.x and 5.x."
                % excerpt))

        uses = bash4_uses(text, kind)
        if uses:
            gate = gate_offset(text, kind)
            first_offset, first_name, first_symptom, first_text = uses[0]
            if gate is None:
                violations.append((
                    rel, line_of(text, first_offset), "undeclared-bash-4-dependency",
                    "uses `%s` (%s) and carries no BASH_VERSINFO gate.\n"
                    "      On macOS `bash` is 3.2 and gives you: %s\n"
                    "      Add the gate this repository already uses in build-and-test.sh,\n"
                    "      or move the construct out. A file SOURCED by others states the\n"
                    "      requirement itself -- see base-harness.sh."
                    % (first_text.strip(), first_name, first_symptom)))
            elif gate > first_offset:
                violations.append((
                    rel, line_of(text, first_offset), "bash-4-gate-comes-too-late",
                    "uses `%s` (%s) at line %d but its BASH_VERSINFO gate is at line %d.\n"
                    "      A gate below the construct it guards runs after the damage."
                    % (first_text.strip(), first_name, line_of(text, first_offset),
                       line_of(text, gate))))

    if violations:
        print("check-shell-portability: %d violation(s) in %d shell script(s)"
              % (len(violations), len(scripts)), file=out)
        for rel, line, rule, detail in violations:
            print("  %s:%d  [%s]" % (rel, line, rule), file=out)
            print("      %s" % detail, file=out)
        print("", file=out)
        print("macOS is a supported host and its /bin/bash is 3.2.57 -- there is no newer", file=out)
        print("one to switch to, and `bash -n` cannot see the first rule at all.", file=out)
        return EXIT_VIOLATION

    print("check-shell-portability: OK (%d shell scripts; none truncates a command"
          " substitution on bash 3.2, every bash-4 dependency declares itself)"
          % len(scripts), file=out)
    return EXIT_OK


def _reported_line(msg, basename):
    """The line number the guard reported for `basename`, or None.

    Derived rather than written: see the note in `selftest`.
    """
    m = re.search(re.escape(basename) + ":([0-9]+)", msg)
    return int(m.group(1)) if m else None


def _line_of_case(body):
    """1-based line of the `case` keyword in a fixture body."""
    for n, line in enumerate(body.split("\n"), 1):
        if "case " in line:
            return n
    raise AssertionError("fixture has no case keyword: %r" % body)


# ── the self-test: red-on-disable for the instrument itself ──────────────────
def selftest(out=sys.stdout):
    """Every red arm asserts the MESSAGE of the refusal it names, never just a code."""
    import shutil
    import tempfile

    arms = 0
    tmp = tempfile.mkdtemp(prefix="csp-")
    OPEN, CLOSE = "$(", ")"
    try:
        d = os.path.join(tmp, "scripts")
        os.makedirs(d)

        def write(name, body):
            with io.open(os.path.join(d, name), "w", encoding="utf-8", newline="") as f:
                f.write(body)

        def filler(count):
            for i in range(count):
                write("filler%02d.sh" % i, "#!/bin/sh\necho %d\n" % i)

        def run(floor=3):
            buf = io.StringIO()
            try:
                rc = check(tmp, floor=floor, out=buf)
            except Collapse as exc:
                return EXIT_COLLAPSE, "SCAN COLLAPSED -- %s" % exc
            return rc, buf.getvalue()

        filler(4)

        # arm 1 -- GREEN on a clean tree, and the message states the population.
        rc, msg = run()
        assert rc == EXIT_OK and "OK (4 shell scripts" in msg, (rc, msg)
        arms += 1

        # arm 2 -- THE MEASURED FAILURE: a `case` in `$( )` with bare patterns.
        bad_body = ('g=hello\n'
                    'echo "' + OPEN + 'case "$g" in *"hell"*' + CLOSE +
                    ' echo yes ;; *' + CLOSE + ' echo no ;; esac' + CLOSE + '"\n')
        write("bad.sh", bad_body)
        rc, msg = run()
        assert rc == EXIT_VIOLATION, (rc, msg)
        assert "bash-3.2-truncates-this-command-substitution" in msg, msg
        assert "scripts/bad.sh" in msg, msg
        # The LINE is asserted too -- a guard that names the wrong line sends the
        # reader somewhere else -- but derived from the fixture rather than written
        # as a literal `<file>:<line>` string, which would go stale silently the moment
        # the fixture grows a line -- and which `plan_citations_guard` refuses on
        # exactly those grounds.
        assert _reported_line(msg, "bad.sh") == _line_of_case(bad_body), msg
        arms += 1

        # arm 3 -- GREEN once every pattern carries the leading paren. The FIX is
        # what this arm pins: an arm that only proved the red would let a "fix"
        # that changes nothing pass review.
        write("bad.sh", 'g=hello\n'
                        'echo "' + OPEN + 'case "$g" in (*"hell"*' + CLOSE +
                        ' echo yes ;; (*' + CLOSE + ' echo no ;; esac' + CLOSE + '"\n')
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        arms += 1

        # arm 4 -- a plain `case`, NOT inside a substitution, is never flagged. This
        # is the no-churn boundary: the repository has dozens of these and every one
        # is safe on 3.2.
        write("plain.sh", 'case "$1" in\n  a) echo a ;;\n  *) echo b ;;\nesac\n')
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        arms += 1

        # arm 5 -- a NESTED substitution inside the case-bearing one still balances,
        # so the naive scan must not mistake the inner `)` for the outer close.
        write("nested.sh", 'g=x\n'
                           'echo "' + OPEN + 'case "' + OPEN + 'printf %s "$g"' + CLOSE +
                           '" in (*x*' + CLOSE + ' echo y ;; (*' + CLOSE +
                           ' echo n ;; esac' + CLOSE + '"\n')
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        arms += 1

        # arm 6 -- a `)` hidden in single quotes does not make a substitution look
        # unbalanced, because rule 1 fires on an unclosed `case`, not on paren
        # arithmetic. `$(grep -o ')' f)` is legal and common.
        write("quoted.sh", "f=/dev/null\necho \"" + OPEN + "grep -o ')' \"$f\"" + CLOSE + "\"\n")
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        os.remove(os.path.join(d, "quoted.sh"))
        arms += 1

        # arm 7 -- EVERY row of the measured table is wired. A table whose rows are
        # declared but never consulted is the failure mode this repository keeps
        # finding; one arm per row is what makes the table load-bearing.
        samples = {
            "mapfile": "mapfile -t x < f\n",
            "associative-array": "declare -A m=()\n",
            "nameref": "local -n r=$1\n",
            "case-expansion": 'v=ab; echo "${v^^}"\n',
            "parameter-transform": 'v=ab; echo "${v@Q}"\n',
            "append-stdout-stderr": "true &>> /dev/null\n",
            "pipe-both": "echo hi |& cat\n",
            "coproc": "coproc CP { cat; }\n",
            "globstar": "shopt -s globstar\n",
            "wait-n": "sleep 0 & wait -n\n",
            "printf-time": 'printf "%(%Y)T" -1\n',
            "test-v": 'x=1; [[ -v x ]] && echo ok\n',
        }
        assert set(samples) == {row[0] for row in BASH4_CONSTRUCTS}, \
            "the self-test sample set and BASH4_CONSTRUCTS have drifted apart"
        for name, body in sorted(samples.items()):
            write("b4.sh", body)
            rc, msg = run()
            assert rc == EXIT_VIOLATION, (name, rc, msg)
            assert "undeclared-bash-4-dependency" in msg, (name, msg)
            assert name in msg, (name, msg)
            arms += 1

        # arm 8 -- GREEN with the gate above the construct.
        write("b4.sh", 'if [ "${BASH_VERSINFO[0]:-0}" -lt 4 ]; then exit 1; fi\n'
                       "declare -A m=()\n")
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        arms += 1

        # arm 9 -- RED when the gate is BELOW the construct. A gate that runs after
        # the damage is not a gate, and nothing else in this repository checks order.
        write("b4.sh", "declare -A m=()\n"
                       'if [ "${BASH_VERSINFO[0]:-0}" -lt 4 ]; then exit 1; fi\n')
        rc, msg = run()
        assert rc == EXIT_VIOLATION and "bash-4-gate-comes-too-late" in msg, (rc, msg)
        assert "line 1" in msg and "line 2" in msg, msg
        arms += 1

        # arm 10 -- a construct named in a COMMENT is not a use. This file, and the
        # three harness headers, are full of prose about `declare -A`.
        write("b4.sh", "# declare -A is bash 4; mapfile too\necho ok\n")
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        arms += 1

        # arm 11 -- nor is one inside single quotes: test-driver-contracts.sh EMITS
        # `declare -A ...` as fixture text, and that is data, not a dependency.
        write("b4.sh", "printf '%s' 'declare -A LEG=()'\n")
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        arms += 1

        # arm 12 -- nor is one inside a here-document body.
        write("b4.sh", "cat > /dev/null <<'EOF'\ndeclare -A m=()\nmapfile -t x < f\nEOF\necho ok\n")
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        arms += 1

        # arm 13 -- an UNQUOTED here-doc terminator is still a body.
        write("b4.sh", "cat > /dev/null <<EOF\ndeclare -A m=()\nEOF\necho ok\n")
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        os.remove(os.path.join(d, "b4.sh"))
        arms += 1

        # arm 14 -- `.git` IS skipped, and it is the ONLY name that is. A `.sh` planted
        # inside it must not be scanned: git's own hooks directory ships sample scripts
        # and they are not this repository's. Driven against a REAL repository, because
        # a hand-made `.git` directory is not one -- and this guard rightly refuses a
        # tree whose ignore query fails, which is what a fake one produces.
        hook_tree = os.path.join(tmp, "hookrepo")
        os.makedirs(hook_tree)
        _env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        _init = subprocess.run(["git", "-C", hook_tree, "init", "-q"], env=_env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert _init.returncode == 0, _init.stderr
        for k in range(3):
            with io.open(os.path.join(hook_tree, "keep%d.sh" % k), "w",
                         encoding="utf-8", newline="") as f:
                f.write("#!/bin/sh\necho %d\n" % k)
        with io.open(os.path.join(hook_tree, ".git", "hooks", "pre-commit.sh"), "w",
                     encoding="utf-8", newline="") as f:
            f.write("declare -A m=()\n")
        buf = io.StringIO()
        rc = check(hook_tree, floor=3, out=buf)
        assert rc == EXIT_OK and "OK (3 shell scripts" in buf.getvalue(), buf.getvalue()
        assert "pre-commit.sh" not in buf.getvalue(), buf.getvalue()
        shutil.rmtree(hook_tree, ignore_errors=True)
        arms += 1

        # arm 15 -- A GITIGNORED FILE IS NOT SCANNED, AND THE SAME FILE UNIGNORED IS.
        # This is the arm that pins the `scratchpad/` finding: a gitignored lane backup
        # holding a verbatim copy of a defective script must not red the guard. BOTH
        # halves in one arm on purpose -- the ignore half alone would pass with the
        # scan switched off entirely.
        # star AND IT IS `.gitignore`, NOT `git ls-files`, DELIBERATELY: arm 16 below
        # is the macOS leg's finding, and this pair is what makes the two answers
        # distinguishable.
        import subprocess as _sp
        git_tree = os.path.join(tmp, "repo")
        os.makedirs(git_tree)
        broken = ('g=hello\n'
                  'echo "' + OPEN + 'case "$g" in *"hell"*' + CLOSE +
                  ' echo yes ;; *' + CLOSE + ' echo no ;; esac' + CLOSE + '"\n')
        env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)

        def git(*args):
            p = _sp.run(["git", "-C", git_tree] + list(args), env=env,
                        stdout=_sp.PIPE, stderr=_sp.PIPE)
            assert p.returncode == 0, (args, p.stderr)

        def gwrite(name, body):
            path = os.path.join(git_tree, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with io.open(path, "w", encoding="utf-8", newline="") as f:
                f.write(body)

        for k in range(4):
            gwrite("ok%d.sh" % k, "#!/bin/sh\necho %d\n" % k)
        gwrite(".gitignore", "scratch/\n")
        gwrite("scratch/backup.sh", broken)
        git("init", "-q")
        buf = io.StringIO()
        rc = check(git_tree, floor=3, out=buf)
        assert rc == EXIT_OK and "OK (4 shell scripts" in buf.getvalue(), buf.getvalue()
        assert "backup.sh" not in buf.getvalue(), buf.getvalue()
        # ...and the identical content OUTSIDE the ignored directory does red.
        gwrite("watched.sh", broken)
        buf = io.StringIO()
        rc = check(git_tree, floor=3, out=buf)
        assert rc == EXIT_VIOLATION and "watched.sh" in buf.getvalue(), buf.getvalue()
        assert "backup.sh" not in buf.getvalue(), buf.getvalue()
        os.remove(os.path.join(git_tree, "watched.sh"))
        arms += 1

        # arm 16 -- THE macOS LEG'S FINDING, PINNED. A file the INDEX names and the
        # working tree does not have must not be scanned or reported: on the macOS
        # carriage the checkout sits at an old commit with the tree rsynced over it, so
        # `git ls-files` still named seven paths the `tools/` consolidation deleted, and
        # the first version of this guard reported all seven as violations on a host
        # where nothing was wrong. The index describes a COMMIT; this guard is about
        # the TREE.
        gwrite("ghost.sh", broken)
        git("add", "ghost.sh")
        os.remove(os.path.join(git_tree, "ghost.sh"))
        buf = io.StringIO()
        rc = check(git_tree, floor=3, out=buf)
        assert rc == EXIT_OK, (rc, buf.getvalue())
        assert "ghost.sh" not in buf.getvalue(), buf.getvalue()
        arms += 1

        # arm 17 -- A BROKEN IGNORE QUERY REFUSES; it does not quietly scan everything.
        # The quiet version would red on exactly the scratch copies arm 15 exists to
        # skip, so the failure mode is worse than the failure. Driven through the
        # injected runner rather than by breaking git, which no test should do.
        class _Rc(object):
            returncode = 128
            stdout = b""
            stderr = b"fatal: something broke"

        try:
            _git_ignored(git_tree, [os.path.join(git_tree, "ok0.sh")],
                         runner=lambda *a, **k: _Rc())
        except Collapse as exc:
            assert "exited 128" in str(exc) and "something broke" in str(exc), exc
        else:
            raise AssertionError("a failing check-ignore must raise Collapse")
        arms += 1
        shutil.rmtree(git_tree, ignore_errors=True)

        # arm 18 -- `build/` IS NOT SPECIAL; `.gitignore` IS. A directory this guard
        # used to hard-skip by NAME is walked like any other when nothing ignores it,
        # and skipped when the repository says to. That is the single rule, pinned in
        # both directions -- the version that skipped by name would have passed the
        # first half while hiding every script in a `build/` somebody had un-ignored.
        deep = os.path.join(d, "build")
        os.makedirs(deep)
        with io.open(os.path.join(deep, "deep.sh"), "w", encoding="utf-8", newline="") as f:
            f.write("declare -A m=()\n")
        rc, msg = run()
        assert rc == EXIT_VIOLATION and "scripts/build/deep.sh" in msg, (rc, msg)
        shutil.rmtree(deep, ignore_errors=True)
        arms += 1

        # arm 19 -- A BARE `case` WORD IS NOT A `case` STATEMENT. `$(echo case)` puts
        # the word in code position with no statement anywhere; reddening it would be
        # a false positive, and a guard's first false positive is the one that teaches
        # everyone to ignore it.
        write("word.sh", 'echo "' + OPEN + 'echo case' + CLOSE + '"\n')
        rc, msg = run()
        assert rc == EXIT_OK, (rc, msg)
        os.remove(os.path.join(d, "word.sh"))
        arms += 1

        # arm 20 -- `${#arr[@]}` IS NOT A COMMENT, AND THE CONSTRUCT AFTER IT ON THE
        # SAME LINE IS STILL SEEN. An earlier draft read that `#` as a comment opener
        # and blanked the rest of the line -- a false NEGATIVE, which is the direction
        # that lets a defect through. Both halves are asserted: the count is used, and
        # the `declare -A` after it still reds.
        write("hash.sh", 'arr=(a b); n=${#arr[@]}; declare -A m=(); echo "$n"\n')
        rc, msg = run()
        assert rc == EXIT_VIOLATION and "undeclared-bash-4-dependency" in msg, (rc, msg)
        assert "associative-array" in msg, msg
        os.remove(os.path.join(d, "hash.sh"))
        arms += 1

        # arm 21 -- THE OUTPUT PATH ITSELF MUST NOT RAISE. `--help` prints this file's
        # documentation, which carries non-ASCII, and on a cp1252 console Python's
        # stdout wrapper refuses it -- ✔MEASURED: `UnicodeEncodeError` instead of a
        # verdict. The arm drives `main` with a genuinely cp1252-encoded stdout, which
        # is the exact shape that failed.
        import codecs
        real_stdout = sys.stdout
        sink = io.TextIOWrapper(io.BytesIO(), encoding="cp1252", errors="strict")
        try:
            sys.stdout = sink
            rc = main(["check-shell-portability.py", "--help"])
        finally:
            sys.stdout = real_stdout
        assert rc == EXIT_OK, rc
        del codecs
        arms += 1

        # arm 22 -- SCAN COLLAPSED: a floor above the tree REDS rather than passing.
        rc, msg = run(floor=99)
        assert rc == EXIT_COLLAPSE and "SCAN COLLAPSED" in msg and "floor is 99" in msg, msg
        arms += 1

        # arm 23 -- GREEN after restore: the red arms were the change, not the fixture.
        rc, msg = run()
        assert rc == EXIT_OK and "OK (7 shell scripts" in msg, (rc, msg)
        arms += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("check-shell-portability: self-test OK - %d arms exercised, every red arm asserting"
          " the MESSAGE of the refusal it names; this guard is PROVEN able to fail." % arms,
          file=out)
    return EXIT_OK


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, "..", ".."))


def report(root, out=sys.stdout):
    for path in shell_scripts(root):
        rel = os.path.relpath(path, root).replace("\\", "/")
        text, kind, subs = inspect(path)
        uses = sorted({name for _o, name, _s, _t in bash4_uses(text, kind)})
        gate = gate_offset(text, kind)
        print("  %-58s gate=%-5s %s"
              % (rel, "yes" if gate is not None else "no", ",".join(uses) or "-"), file=out)
    return EXIT_OK


def _make_output_utf8_safe():
    """Stop a console codepage from turning a diagnostic into a traceback.

    ⚠ ✔MEASURED 2026-08-22 on this Windows workstation: `--help` died with
    `UnicodeEncodeError: 'charmap' codec can't encode character '\u2714'` because
    Python wraps stdout in the console's cp1252 codec, and this file's own
    documentation carries the MEASURED tick. **A guard whose OUTPUT PATH can raise is
    a guard that reports a crash instead of a verdict**, and it would do it on the
    host whose ctest run this repository treats as primary.
    star Both streams, and `errors="replace"` as well as the re-encode: reconfiguring
    can itself be unavailable (a redirected non-TextIO stream), so this must never be
    the thing that fails.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass


def main(argv):
    _make_output_utf8_safe()
    known = ("--selftest", "--list", "--help", "-h")
    unknown = [a for a in argv[1:] if a not in known]
    if unknown:
        print("check-shell-portability: unknown argument(s): %s" % " ".join(unknown))
        print(__doc__.rsplit("Usage:", 1)[-1].strip())
        return EXIT_USAGE
    if "--help" in argv[1:] or "-h" in argv[1:]:
        print(__doc__)
        return EXIT_OK
    root = repo_root()
    if "--list" in argv[1:]:
        return report(root)
    if "--selftest" in argv[1:]:
        return selftest()
    try:
        # star star THE NO-ARGUMENT FORM -- the one ctest uses -- VERIFIES THE REAL TREE
        # AND THEN PROVES IT CAN FAIL, in that order. Same shape as check-scripts-index
        # and for the same reason: an entry that only verified would pass identically
        # if every check inside it had been commented out.
        rc = check(root)
        if rc != EXIT_OK:
            return rc
        return selftest()
    except Collapse as exc:
        print("check-shell-portability: FAIL (structural) -- %s" % exc)
        print("  This does NOT mean the scripts are clean - it means the SCAN COLLAPSED.")
        print("  Refusing to report a pass; fix the scan, do not lower the floor.")
        return EXIT_COLLAPSE


if __name__ == "__main__":
    sys.exit(main(sys.argv))
