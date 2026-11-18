#!/usr/bin/env python3
# PURPOSE: refuse a registry row whose text a fold made untrue -- a reference claim with no carrier, or a name, key, path or decision the fold removed cited as current.
"""check-row-truth.py -- a row's text stays true to the record, mechanically.

★★★ WHY THIS EXISTS. Two classes kept coming back to review as findings, each time with the instances somebody
happened to read and never the class:
  * a REFERENCE's answer stated with nothing saying how it is known -- "link.exe refuses it", "ld64 keeps the first"
    -- where the reader cannot tell a measurement from a recollection, or from DSS's own code read as if it were the
    reference's (READ of DSS is no evidence of a reference);
  * a row citing a name, a document key, a path or a decision a later fold REMOVED or REVERSED, still in the present
    tense -- the row reads as true and sends the next reader after a thing that no longer exists.
An instance-driven sweep finds the instances; this program finds the class, at every fold, against the fold's parent.

── CHECK 1: EVERY REFERENCE CLAIM IS CARRIED ──────────────────────────────────────────────────────────────────────
A CLAIM is one sentence (split at ". ", "; ", " -- ", " — " and cell borders) that names a reference toolchain
(REFERENCES) AND states what it does (VERDICTS). It is CARRIED when the sentence, or the LEAD-IN that governs it (the
nearest earlier sentence of the same cell or comment block that ends in ":"), names how it is known:
  MEASURED (or ✔) with a run id or a probe-reference-cc item or a fixture's recorded build; DOCUMENTED with its
  source; INFERRED; UNMEASURED with its blocker.
`READ` is NOT a carrier. A claim whose only evidence is a gtest TEST name is resolved by READING that TEST's body:
a TEST that INVOKES a reference toolchain (INVOCATIONS) is a measurement and must cite the run that judged it; a TEST
that does not compares DSS with a constant and carries no reference claim at all.
SCOPE: the rows named (`--rows`, default every row whose text differs from `--base`) and the comment text of the
files a list names (`--files`: a file holding one tree-relative path per line, each a part of a path that never
leaves the tree; C/C++ `//` and `/* */` comments, JSON `$…Comment` strings).

── CHECK 2: NOTHING A FOLD REMOVED IS CITED AS CURRENT (needs `--base`) ───────────────────────────────────────────
Between the base commit and the working tree (untracked files included), BY DATA:
  (a) example directories deleted; (b) mutation arm ids removed; (c) DiagnosticCode enumerators removed;
  (d) gtest TEST names removed; (e) a backticked identifier a row cites that names code at base and none in the tree
      -- matched as CITED (qualified), against code with its comments and literals blanked, so a comment that tells
      of the deletion does not keep the name alive; (g) a shipped-document KEY removed or reshaped (scalar, list,
      object), lists aligned by their entries' `name` so an insertion is not read as a change of every later entry;
  (h) the fold's REVERSALS, read from `--reversals` (one per line: `<what> :: <subject words>`), each subject word
      searched in the rows; (i) a deleted path under src/, tests/ or examples/.
(f), the codes whose corpus witnesses moved, is REPORTED as leads, never judged: a count is a measurement to read.
A row citing one of them is a FINDING unless every citing sentence is in HISTORY form (HISTORY_MARKERS: removed,
retired, renamed, deleted, replaced, reversed, superseded, "no longer", "until", "was", "were", "had", …).
Every row of both registries is read; a finding names the row, the name and the sentence.

Exit 0 with no finding; 1 with any (a JUDGE, not a report); 2 when the scan itself cannot run.
THE ADJUDICATION RECORD (`--adjudications`): a fold's READ verdicts, one per line --
  `claim <row or path> :: <the claim sentence's start> :: <the carrier>` for a claim its paragraph carries, and
  `kept <row> :: <a reshaped key's or a reversal's subject> :: <why the sentence is still true>`.
Each entry exempts exactly the findings it names and is reported (CARRIED / KEPT); an entry that exempts nothing is
itself a finding (STALE), and a REMOVED name is never kept -- the row is restated. A lane's rows directory
(`--cells`) is laid over the registries, so its cells are judged before the fold merges them.
USAGE
    dssharness run check-row-truth --input base=<commit> [--input rows=<ids>] [--input files=<list file>]
                                   [--input reversals=<path>] [--input cells=<dir>] [--input adjudications=<path>]
    python3 check-row-truth.py --selftest       every arm against a repository SYNTHESIZED in a temp directory
"""
import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True  # a by-path load must not write __pycache__ beside another action (check-scripts-index)
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError, OSError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "anchors"))
import anchors  # noqa: E402  (the ONE row reader: storage escapes `|`, a raw read doubles it)

_PATH_TYPES_OWNER = []


def _path_types():
    """owning-tree, loaded ONCE by path (a hyphen is no module name): the argparse kinds every argument that takes a
    value declares (check-scripts-index's clause 14). Fails loud when owning-tree is absent."""
    if not _PATH_TYPES_OWNER:
        import importlib.util
        path = os.path.join(os.path.dirname(HERE), "owning-tree", "owning-tree.py")
        if not os.path.isfile(path):
            raise SystemExit("check-row-truth: cannot find %s -- what each kind of its arguments refuses is said "
                             "there and nowhere else" % path)
        spec = importlib.util.spec_from_file_location("dss_owning_tree", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _PATH_TYPES_OWNER.append(mod)
    return _PATH_TYPES_OWNER[0]


# ── the vocabularies (each the ONE place its words are stated) ─────────────────────────────────────────────────────
REFERENCES = re.compile(r"\b(link\.exe|lld-link|ld\.lld|GNU ld|ld64|ld-prime|Apple's ld|Apple clang|MinGW(?:-w64)?|"
                        r"gcc|clang(?:-cl)?|cl(?= |\.exe|\b19)|MSVC|lib\.exe|LNK\d{4}|libtool)\b")
VERDICTS = re.compile(r"\b(runs?|ran|exits?|refuses?|refused|keeps?|kept|picks?|elects?|starts?|pulls?|pulled|"
                      r"links?|linked|accepts?|accepted|warns?|warned|writes?|wrote|binds?|resolves?|chooses?|chose|"
                      r"emits?|takes?|gives?|drops?|ignores?|reads?)\b")
RUN_ID = r"\b20\d{6}-\d{6}-[0-9a-f]{8}\b"
CARRIERS = re.compile(r"\bMEASURED\b|✔|\bDOCUMENTED\b|\bINFERRED\b|\bUNMEASURED\b|probe-reference-cc|" + RUN_ID
                      + r"|\bfixture\b")
# A carrier that is only MEASURED/✔ names its evidence: a run, a probe item, a fixture, or a TEST to resolve.
EVIDENCE = re.compile(RUN_ID + r"|probe-reference-cc|\bfixture\b|\bDOCUMENTED\b|\bINFERRED\b|\bUNMEASURED\b")
TEST_NAME = re.compile(r"`?\b([A-Z][A-Za-z0-9]*)\.([A-Z][A-Za-z0-9_]{3,})\b`?")
# What a TEST spells when it runs a reference toolchain itself (its control): a command word or a locator.
INVOCATIONS = re.compile(r"\"(cl|link|lib|lld-link|gcc|cc|clang|clang-cl|ld\.lld|ld|xcrun|ar|libtool)[ \"]|"
                         r"\blocateMsvcToolchain\b|\bfindCompiler\b|\bxcrun\b|\bthe reference's link\b")
# Only words that put a sentence in the past of the thing it names: a looser list ("then", "before", "dropped") would
# read present-tense sentences as history and fail toward CLEAN.
HISTORY_MARKERS = re.compile(r"\b(removed|retired|renamed|deleted|replaced|reversed|reshaped|superseded|withdrawn|"
                             r"no longer|was|were|formerly|in history form|until (?:fold|P\d+|checkpoint))\b", re.I)
SENTENCE_SPLIT = re.compile(r"(?<=[.;])\s+|\s--\s|\s—\s")


def sentences(text):
    return [s.strip() for s in SENTENCE_SPLIT.split(text) if s.strip()]


CODE_SPANS = re.compile(r"`[^`]*`|\S*/\S*")


def prose_of(sentence):
    """The sentence with its backticked spans, its paths and its reference names blanked: a VERDICT is a word of the
    prose, never a component of `src/link/...`, of an arm id `link-the-...` or of `link.exe` itself (each of which
    spells `link`)."""
    return REFERENCES.sub(" ", CODE_SPANS.sub(" ", sentence))


def claim_findings(text, tests_index):
    """-> [(why, sentence)] for every uncarried claim of one block of text (a cell, a comment block)."""
    out = []
    lead = ""
    for s in sentences(text):
        if s.endswith(":"):
            lead = s
        if not (REFERENCES.search(s) and VERDICTS.search(prose_of(s))):
            continue
        scope = lead + " " + s
        if not CARRIERS.search(scope):
            out.append(("uncarried", s))
            continue
        if EVIDENCE.search(scope):
            continue
        # MEASURED / ✔ with no run, probe item or fixture: the evidence can only be a TEST it names.
        named = [m.group(1) + "." + m.group(2) for m in TEST_NAME.finditer(scope)]
        if not named:
            out.append(("MEASURED with no evidence named (a run, a probe item, a fixture, a TEST)", s))
            continue
        for t in named:
            body = tests_index.get(t)
            if body is None:
                out.append(("names TEST %s, which no tests/ file defines" % t, s))
            elif INVOCATIONS.search(body):
                out.append(("names TEST %s, a reference CONTROL, with no run that judged it" % t, s))
            else:
                out.append(("names TEST %s, which compares DSS with a constant: it carries no reference claim" % t,
                            s))
    return out


# ── comment text of a file ─────────────────────────────────────────────────────────────────────────────────────────
def comment_blocks(path, text):
    """-> [str]: the comment blocks of a C/C++ file (consecutive `//` lines joined, each `/* */` whole) or the
    `$…Comment` strings of a JSON document."""
    if path.endswith(".json"):
        out = []

        def walk(n):
            if isinstance(n, dict):
                for k, v in n.items():
                    if k.startswith("$") and isinstance(v, str):
                        out.append(v)
                    else:
                        walk(v)
            elif isinstance(n, list):
                for v in n:
                    walk(v)
        walk(json.loads(text))
        return out
    # A `/* */` comment is one block; a run of lines that are only `//` comments is one block (a trailing `//` after
    # code is a block of its own). Literals are matched first so a `//` inside a string is not a comment.
    blocks, run_ = [], []
    for m in re.finditer(r"\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'|/\*(.*?)\*/|//([^\n]*)", text, re.S):
        if m.group(1) is not None:
            blocks.append(" ".join(x.strip(" *") for x in m.group(1).splitlines()))
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("//"):
            run_.append(s[2:].strip())
            continue
        if run_:
            blocks.append(" ".join(run_))
            run_ = []
        k = line.find("//")
        if k > 0 and line[:k].count('"') % 2 == 0:
            blocks.append(line[k + 2:].strip())
    if run_:
        blocks.append(" ".join(run_))
    return blocks


def blank_comments_and_literals(text):
    """C/C++ code with every comment and string/char literal replaced by spaces (line structure kept)."""
    def repl(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", repl, text, flags=re.S)


# ── the two trees: a commit's (git objects) and the working tree's (files) ─────────────────────────────────────────
class Tree(object):
    def __init__(self, root, commit=None):
        self.root, self.commit, self._files = root, commit, None

    def git(self, *a, binary=False, data=None):
        r = subprocess.run(["git", "-C", self.root] + list(a), input=data, capture_output=True)
        if r.returncode != 0:
            raise SystemExit("check-row-truth: git %s failed: %s" % (" ".join(a[:3]),
                                                                       r.stderr.decode("utf-8", "replace").strip()))
        return r.stdout if binary else r.stdout.decode("utf-8", "replace")

    def files(self):
        """{path: text} of src/, tests/, examples/ and the arms registry (text files only)."""
        if self._files is not None:
            return self._files
        want = lambda p: p.startswith(("src/", "tests/", "examples/")) and p.endswith(
            (".cpp", ".hpp", ".h", ".inc", ".json", ".c", ".s", ".registry"))
        out = {}
        if self.commit:
            rows = []
            for ln in self.git("ls-tree", "-r", self.commit, "--", "src", "tests", "examples").splitlines():
                meta, path = ln.split("\t", 1)
                if want(path):
                    rows.append((path, meta.split()[2]))
            data = self.git("cat-file", "--batch", binary=True,
                            data=("\n".join(b for _, b in rows) + "\n").encode("ascii"))
            off = 0
            for path, _b in rows:
                nl = data.index(b"\n", off)
                size = int(data[off:nl].split()[2])
                out[path] = data[nl + 1:nl + 1 + size].decode("utf-8", "replace")
                off = nl + 1 + size + 1
        else:
            listed = self.git("ls-files", "--cached", "--others", "--exclude-standard", "--", "src", "tests",
                              "examples").splitlines()
            for path in listed:
                full = os.path.join(self.root, path)
                if want(path) and os.path.isfile(full):
                    out[path] = io.open(full, encoding="utf-8", errors="replace").read()
        self._files = out
        return out

    def paths(self):
        if self.commit:
            return set(self.git("ls-tree", "-r", "--name-only", self.commit, "--", "src", "tests",
                                "examples").splitlines())
        return {p for p in self.git("ls-files", "--cached", "--others", "--exclude-standard", "--", "src", "tests",
                                    "examples").splitlines() if os.path.isfile(os.path.join(self.root, p))}

    def registry_text(self):
        if self.commit:
            return {b: self.git("show", "%s:%s" % (self.commit, anchors.REL[b])) for b in anchors.BUCKETS}
        return {b: io.open(os.path.join(self.root, anchors.REL[b]), encoding="utf-8").read() for b in anchors.BUCKETS}


def tests_index(tree):
    """{Suite.Name: body} of every gtest TEST/TEST_F/TEST_P in tests/ (the body up to the next TEST)."""
    out = {}
    for path, text in tree.files().items():
        if not path.endswith(".cpp"):
            continue
        ms = list(re.finditer(r"^TEST(?:_F|_P)?\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)", text, re.M))
        for i, m in enumerate(ms):
            end = ms[i + 1].start() if i + 1 < len(ms) else len(text)
            out["%s.%s" % (m.group(1), m.group(2))] = text[m.start():end]
    return out


# ── CHECK 2's removed names ────────────────────────────────────────────────────────────────────────────────────────
def removed_names(base, work):
    """-> {name: why} of everything (a)-(e), (g), (i) the work tree removed against the base; and the (f) leads."""
    b, w = base.files(), work.files()
    names, leads = {}, []
    # (a) example directories
    ex = lambda paths: {"/".join(p.split("/")[:3]) for p in paths if p.startswith("examples/") and p.count("/") >= 3}
    for d in sorted(ex(base.paths()) - ex(work.paths())):
        names[d.split("/")[2]] = "(a) the example %s was deleted" % d
    # (b) arms
    arm = lambda files: set(re.findall(r"^A \| ([a-z0-9][a-z0-9-]+) \|", files.get(
        "tests/mutations/arms.registry", ""), re.M))
    for a in sorted(arm(b) - arm(w)):
        names[a] = "(b) the arm %s was removed" % a
    # (c) diagnostic codes
    code = lambda files: set(re.findall(r"^\s*([A-Z][A-Za-z]?_[A-Za-z0-9]+)\s*[,=]",
                                        files.get("src/core/types/parse_diagnostic.hpp", ""), re.M))
    for k in sorted(code(b) - code(w)):
        names[k] = "(c) the diagnostic code %s was removed" % k
    # (d) tests
    tb, tw = set(tests_index(base)), set(tests_index(work))
    for t in sorted(tb - tw):
        names[t] = "(d) the TEST %s was removed" % t
    # (i) paths
    for p in sorted(base.paths() - work.paths()):
        if not p.startswith("examples/"):
            names[p] = "(i) the file %s was deleted" % p
    # (g) document keys: a key REMOVED from every document that had it is a removed name; a key whose shape changed
    # is RESHAPED (a row the fold itself rewrote is presumed to state the new shape; an untouched one is not).
    # Only the TOP of each moved subtree is a subject: the children of a removed or reshaped key moved WITH it, and
    # their leaves ("name", "address") are words every row uses -- a subject of them fails toward NOISE, which buries
    # the one finding that matters. A named list entry's subject is its name, never the list's key.
    removed_keys, reshaped = {}, {}
    still = set()
    for p in sorted(x for x in w if x.endswith(".json") and x.startswith("src/")):
        try:
            still |= {key_subject(k) for k in doc_keys(json.loads(w[p]))}
        except ValueError:
            pass
    for p in sorted(x for x in b if x.endswith(".json") and x.startswith("src/")):
        if p not in w or b[p] == w[p]:
            continue
        try:
            kb, kw = doc_keys(json.loads(b[p])), doc_keys(json.loads(w[p]))
        except ValueError:
            continue
        moved = {k for k, shape in kb.items() if k not in kw or kw[k] != shape}
        # The keys each list's entries still carry in the work document: a key ONE entry drops while another entry of
        # the same list keeps it is an edit of that entry (a `reason` a refused row no longer needs, a `linkName` one
        # symbol no longer states) -- the key is the list's vocabulary still, and its name is no subject: every row
        # citing the key for any other entry would be a finding, which buries the one that matters.
        entry_keys = {}
        for k2 in kw:
            head, _, leaf = k2.rpartition("/")
            if head and KEY_ENTRY.match(head.rsplit("/", 1)[-1]):
                entry_keys.setdefault(list_of_entry(head), set()).add(leaf)
        for k in sorted(moved):
            if any(a in moved for a in key_ancestors(k)):
                continue
            subject = key_subject(k)
            doc = p.rsplit("/", 1)[-1]
            head, _, leaf = k.rpartition("/")
            if (k not in kw and head in kw and KEY_ENTRY.match(head.rsplit("/", 1)[-1])
                    and leaf in entry_keys.get(list_of_entry(head), ())):
                continue
            if k not in kw:
                (removed_keys if subject not in still else reshaped).setdefault(
                    subject, "(g) the key %s of %s was removed" % (k, doc))
            else:
                reshaped.setdefault(subject, "(g) the key %s of %s changed shape (%s -> %s)"
                                    % (k, doc, kb[k], kw[k]))
    for k, v in removed_keys.items():
        names.setdefault(k, v)
    keys = set(removed_keys) | set(reshaped)
    # (f) leads: corpus examples per code
    def code_counts(files):
        out = {}
        for p, t in files.items():
            if p.startswith("examples/") and p.endswith("expected.json"):
                for k in set(re.findall(r'"([A-Z]_[A-Z][A-Za-z0-9]+)"', t)):
                    out[k] = out.get(k, 0) + 1
        return out
    n0, n1 = code_counts(b), code_counts(w)
    for k in sorted(set(n0) | set(n1)):
        if n0.get(k, 0) != n1.get(k, 0):
            leads.append("(f) %s: corpus examples %d -> %d" % (k, n0.get(k, 0), n1.get(k, 0)))
    return names, reshaped, leads, keys


KEY_ENTRY = re.compile(r"^(?P<key>[^\[]*)\[(?P<entry>[^\]]*)\]$")


def key_subject(path):
    """The word a row cites a document key path by: a named list entry's NAME, else the key's own name."""
    last = path.rsplit("/", 1)[-1]
    m = KEY_ENTRY.match(last)
    if m and not m.group("entry").isdigit():
        return m.group("entry")
    return last.split("[", 1)[0]


def list_of_entry(path):
    """The list an entry path belongs to: `/a/b[x]` -> `/a/b`."""
    head, sep, last = path.rpartition("/")
    return head + sep + KEY_ENTRY.match(last).group("key")


def key_ancestors(path):
    """Every key path above `path` in doc_keys' spelling: `/a[x]/b` -> `/a[x]`, `/a`."""
    out = []
    while True:
        head, sep, last = path.rpartition("/")
        m = KEY_ENTRY.match(last)
        if m:
            path = head + sep + m.group("key")
        elif head:
            path = head
        else:
            return out
        out.append(path)


def cites_key(text, name):
    """A document key is cited AS A KEY: inside a backticked span (rows) or a string literal (TEST bodies), so the
    plain word -- a row's "name", a body's `sym.name` -- is no citation of it."""
    word = r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])"
    return re.search(r"`[^`\n]*" + word + r"[^`\n]*`|\"[^\"\n]*" + word + r"[^\"\n]*\"", text) is not None


def doc_keys(node, path=""):
    """{key path: shape} -- a list of objects is aligned by each entry's `name` (or its index when it has none), a
    list of scalars is one value; `$…` keys skipped."""
    out = {}
    if isinstance(node, dict):
        if path:
            out[path] = "object"
        for k, v in node.items():
            if not k.startswith("$"):
                out.update(doc_keys(v, path + "/" + k))
    elif isinstance(node, list):
        if path:
            out[path] = "list"
        for i, v in enumerate(node):
            if isinstance(v, dict):
                out.update(doc_keys(v, "%s[%s]" % (path, v.get("name", i))))
    elif path:
        out[path] = "scalar"
    return out


def removed_identifiers(base, work, cited):
    """(e): each identifier a row cites (backticked), present in base's CODE (comments and literals blanked) and in
    none of the work tree's -- matched as cited, a qualified name as a whole."""
    def code_words(tree):
        words = set()
        for p, t in tree.files().items():
            if p.endswith((".cpp", ".hpp", ".h", ".inc")):
                blanked = blank_comments_and_literals(t)
                words |= set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*", blanked))
        return words
    wb, ww = code_words(base), code_words(work)
    flat_b = {x.rsplit("::", 1)[-1] for x in wb} | wb
    flat_w = {x.rsplit("::", 1)[-1] for x in ww} | ww
    out = {}
    for n in sorted(cited):
        if len(n) < 5:
            continue
        if n in flat_b and n not in flat_w:
            out[n] = "(e) %s names code at the base and none in the tree" % n
    return out


def history_form(sentence):
    return HISTORY_MARKERS.search(sentence) is not None


def cites(text, name):
    return re.search(r"(?<![A-Za-z0-9_./-])" + re.escape(name) + r"(?![A-Za-z0-9_-])", text) is not None


def row_text(row):
    return " | ".join(row.cell(i) for i in (anchors.C_TRIGGER, anchors.C_CLOSING, anchors.C_XREF))


def row_findings(rows, changed, names, reshaped, reversals, keys=frozenset()):
    """A REMOVED name cited in a present-tense sentence is a finding in any row. A RESHAPED key or a REVERSED
    decision is a finding only in a row the fold did not rewrite (`changed` holds the ones it did): the fold's own
    text is presumed to state what it changed; an untouched row cannot have. A document key (`keys`) is matched only
    where it is cited as a key (cites_key)."""
    out = []
    for row in rows:
        text = row_text(row)
        untouched = row.name.strip("`") not in changed
        subjects = [(n, why) for n, why in names.items()]
        if untouched:
            subjects += [(n, why) for n, why in reshaped.items()]
            subjects += [(word, "(h) reversed by the fold: %s" % what) for what, words in reversals for word in words]
        for i, (name, why) in enumerate(subjects):
            match = cites_key if name in keys and why.startswith("(g)") else cites
            if not match(text, name):
                continue
            for s in sentences(text):
                if match(s, name) and not history_form(s):
                    # the last field: KEEPABLE -- a reshaped key or a reversal (a sentence that may still be true,
                    # judged by reading); a REMOVED name (the first len(names) subjects) never is.
                    out.append((row, name, why, s, i >= len(names)))
    return out


def test_leads(base_idx, work_idx, names, reshaped, reversals, keys=frozenset()):
    """The same subjects, read in every TEST body under tests/: a TEST naming a removed name, or -- when the fold did
    not rewrite its body -- a reshaped key or a reversed decision's subject, is a LEAD. A TEST's truth is its run's
    verdict, not its text, so this lists and the run judges: each lead is a TEST the fold must RUN on every leg its
    cells run on (a TEST asserting a decision the fold reversed reds only where it runs)."""
    out = []
    for test, body in sorted(work_idx.items()):
        untouched = base_idx.get(test) == body
        subjects = list(names.items())
        if untouched:
            subjects += list(reshaped.items())
            subjects += [(word, "(h) reversed by the fold: %s" % what) for what, words in reversals for word in words]
        for name, why in subjects:
            match = cites_key if name in keys and why.startswith("(g)") else cites
            if name != test and match(body, name):
                out.append("TEST %s names %s -- %s: RUN it on every leg its cells run on" % (test, name, why))
    return out


def read_adjudications(path):
    """-> [[kind, owner, key, why, used]] from the fold's adjudication record, one per line:
         claim <row id or tree path> :: <the claim sentence's start> :: <the paragraph or source that carries it>
         kept <row id> :: <a reshaped key's or a reversal's subject> :: <why the sentence is still true>
    Each entry exempts exactly the findings it names, and is itself a finding when it names none (a stale entry), so
    the record cannot outlive what it judged. A REMOVED name is never kept: the row is restated."""
    out = []
    if not path:
        return out
    for n, line in enumerate(io.open(path, encoding="utf-8").read().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(" :: ")]  # spaced: a claim's own `linker::name` is no separator
        head = parts[0].split(None, 1)
        if len(parts) != 3 or len(head) != 2 or head[0] not in ("claim", "kept") or not parts[1] or not parts[2]:
            raise SystemExit("check-row-truth: %s line %d is no adjudication (`claim|kept <owner> :: <key> :: "
                             "<why>`): %s" % (path, n, line))
        out.append([head[0], head[1].strip("`"), parts[1], parts[2], False])
    return out


def adjudicated(adjudications, kind, owner, sentence_or_subject):
    hit = False
    for a in adjudications:
        if a[0] != kind or a[1] != owner:
            continue
        if (kind == "claim" and sentence_or_subject.startswith(a[2])) or (kind == "kept" and
                                                                           sentence_or_subject == a[2]):
            a[4] = True
            hit = True
    return hit


class CellOverlay(object):
    """A row of the registries with the cells a lane's rows directory holds for it (<dir>/<ROW-ID>/trigger.txt,
    closing.txt, cross-refs.txt): the judge reads a lane's rows before they are folded."""
    FILES = (("trigger.txt", None), ("closing.txt", None), ("cross-refs.txt", None))

    def __init__(self, row, folder):
        self.name, self.raw, self.cells = row.name, row.raw, list(row.cells)
        for (fname, _), idx in zip(self.FILES, (anchors.C_TRIGGER, anchors.C_CLOSING, anchors.C_XREF)):
            p = os.path.join(folder, fname)
            if os.path.isfile(p) and len(self.cells) > idx:
                self.cells[idx] = " %s " % io.open(p, encoding="utf-8").read()

    def cell(self, idx):
        return self.cells[idx] if len(self.cells) > idx else ""


def overlay_cells(rows, cells_dir):
    """-> (rows with a lane's cell files laid over, the ids so overlaid). A folder naming no row is refused."""
    if not cells_dir:
        return rows, set()
    by = {r.name.strip("`"): r for r in rows}
    ids = sorted(d for d in os.listdir(cells_dir) if os.path.isdir(os.path.join(cells_dir, d)))
    missing = [d for d in ids if d not in by]
    if missing:
        raise SystemExit("check-row-truth: --cells holds %s, which no registry row is" % ", ".join(missing))
    over = {d: CellOverlay(by[d], os.path.join(cells_dir, d)) for d in ids}
    # a folder whose cells are the registry's own is no rewrite: it does not earn a rewritten row's exemption
    rewritten = {d for d, o in over.items()
                 if any(o.cell(i).strip() != by[d].cell(i).strip()
                        for i in (anchors.C_TRIGGER, anchors.C_CLOSING, anchors.C_XREF))}
    return [over.get(r.name.strip("`"), r) for r in rows], rewritten


def read_reversals(path):
    out = []
    if not path:
        return out
    for line in io.open(path, encoding="utf-8").read().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "::" not in line:
            continue
        what, words = line.split("::", 1)
        out.append((what.strip(), [w for w in re.split(r"[,\s]+", words.strip()) if w]))
    return out


def read_files_list(path):
    """The tree files a `--files` list names, one per line (blank lines and `#` comments skipped), each held to
    owning-tree's `path_part` -- joined under the tree, never absolute, never through `..` -- or `anchors.Refused`."""
    out = []
    if not path:
        return out
    pt = _path_types()
    for line in io.open(path, encoding="utf-8").read().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            out.append(pt.path_part(line, "--files"))
        except pt.UsageRefusal as exc:
            raise anchors.Refused(str(exc))
    return out


# ── the run ────────────────────────────────────────────────────────────────────────────────────────────────────────
def run(root, base, rows_arg, files_arg, reversals_arg, out=print, cells_arg="", adjudications_arg=""):
    work = Tree(root)
    rows, rewritten = overlay_cells(anchors.read_rows(root, anchors.BUCKETS), cells_arg)
    adjudications = read_adjudications(adjudications_arg)
    base_tree = Tree(root, base) if base else None
    if rows_arg:
        wanted = {r.strip().strip("`") for r in rows_arg.split(",") if r.strip()}
        chosen = [r for r in rows if r.name.strip("`") in wanted]
        missing = wanted - {r.name.strip("`") for r in chosen}
        if missing:
            raise SystemExit("check-row-truth: no row named %s in either registry" % ", ".join(sorted(missing)))
    elif not base_tree:
        raise SystemExit("check-row-truth: name the rows (--rows) or the base they changed since (--base)")
    changed = set()
    if base_tree:
        before = {}
        for txt in base_tree.registry_text().values():
            for line in txt.splitlines():
                m = re.match(r"\|\s*`?([A-Za-z][A-Za-z0-9_-]+)`?\s*\|", line)
                if m:
                    before[m.group(1)] = line.rstrip("\r")
        changed = {r.name.strip("`") for r in rows if before.get(r.name.strip("`")) != r.raw.rstrip("\r")}
    changed |= rewritten
    if base_tree and not rows_arg:
        chosen = [r for r in rows if r.name.strip("`") in changed]
    findings = 0
    idx = tests_index(work)
    # CHECK 1
    for row in chosen:
        owner = row.name.strip("`")
        for cell in (row.cell(anchors.C_TRIGGER), row.cell(anchors.C_CLOSING), row.cell(anchors.C_XREF)):
            for why, s in claim_findings(cell, idx):
                if adjudicated(adjudications, "claim", owner, s):
                    out("CARRIED (adjudicated) %s: %s" % (owner, s[:160]))
                    continue
                findings += 1
                out("CLAIM %s: %s: %s" % (owner, why, s[:300]))
    files = read_files_list(files_arg)
    for f in files:
        full = os.path.join(root, f)
        if not os.path.isfile(full):
            raise SystemExit("check-row-truth: --files names %s, which is no file of the tree" % f)
        for block in comment_blocks(f, io.open(full, encoding="utf-8").read()):
            for why, s in claim_findings(block, idx):
                if adjudicated(adjudications, "claim", f, s):
                    out("CARRIED (adjudicated) %s: %s" % (f, s[:160]))
                    continue
                findings += 1
                out("CLAIM %s: %s: %s" % (f, why, s[:300]))
    # CHECK 2
    leads = []
    if base_tree:
        names, reshaped, leads, keys = removed_names(base_tree, work)
        cited = set()
        for r in rows:
            cited |= set(re.findall(r"`([A-Za-z_][A-Za-z0-9_:]*[A-Za-z0-9_])`", row_text(r)))
        names.update({k: v for k, v in removed_identifiers(base_tree, work, cited).items() if k not in names})
        reversals = read_reversals(reversals_arg)
        for row, name, why, s, keepable in row_findings(rows, changed, names, reshaped, reversals, keys):
            owner = row.name.strip("`")
            if keepable and adjudicated(adjudications, "kept", owner, name):
                out("KEPT (adjudicated) %s cites %s: %s" % (owner, name, s[:160]))
                continue
            findings += 1
            out("ROW %s cites %s as current -- %s: %s" % (owner, name, why, s[:300]))
        leads += test_leads(tests_index(base_tree), idx, names, reshaped, reversals, keys)
    for a in adjudications:
        if not a[4]:
            findings += 1
            out("STALE ADJUDICATION %s %s :: %s -- it exempts no finding of this run (a removed name is never kept;"
                " a restated sentence needs no entry): drop it" % (a[0], a[1], a[2]))
    for lead in leads:
        out("LEAD " + lead)
    out("check-row-truth: %d finding(s) over %d row(s) and %d file(s)%s"
        % (findings, len(chosen), len(files), (" against %s" % base) if base else ""))
    return 1 if findings else 0


# ── the self-test: every arm against a repository synthesized in a temp directory ──────────────────────────────────
def self_test():
    """Each check must FIRE on a synthesized negative and STAY SILENT on its positive. ⚠ No fixture spells an anchor
    id: this directory is a citation root, and an id-shaped fixture reds check-anchor-citations."""
    failures = [0]
    count = [0]

    def arm(cond, what, detail=""):
        count[0] += 1
        print("  [%s] %s%s" % ("ok  " if cond else "FAIL", what, "" if cond else " -- " + detail))
        if not cond:
            failures[0] += 1

    idx = {"FixtureNative.RunsTheReference": 'TEST(FixtureNative, RunsTheReference) { run("cl /c a.c"); }',
           "Fixture.ComparesWithAConstant": "TEST(Fixture, ComparesWithAConstant) { EXPECT_EQ(rc, 42); }"}
    f = lambda t: [w for w, _ in claim_findings(t, idx)]
    arm(f("link.exe refuses the pair.") == ["uncarried"], "CHECK 1: an uncarried claim is a finding")
    arm(f("READ: link.exe refuses the pair.") == ["uncarried"], "CHECK 1: READ carries no reference claim")
    arm(f("MEASURED (run 20261114-092257-9d3a3547): link.exe refuses the pair.") == [],
        "CHECK 1: a claim with its run is carried")
    arm(f("MEASURED (run 20261114-092257-9d3a3547): the cells. link.exe refuses the pair.") != [],
        "CHECK 1: a run in an EARLIER sentence that is no lead-in carries nothing")
    arm(f("MEASURED (run 20261114-092257-9d3a3547) on two legs: link.exe refuses the pair.") == [],
        "CHECK 1: a lead-in ending in ':' carries the sentence after it")
    arm(f("DOCUMENTED (lld's SymbolTable.cpp): lld-link takes the lazy symbol.") == [],
        "CHECK 1: DOCUMENTED with a source carries")
    arm(f("link.exe refuses the pair, MEASURED by `Fixture.ComparesWithAConstant`.")
        and "constant" in claim_findings("link.exe refuses the pair, MEASURED by `Fixture.ComparesWithAConstant`.",
                                         idx)[0][0], "CHECK 1: a constant TEST carries no reference claim")
    arm(f("link.exe refuses the pair, MEASURED by `FixtureNative.RunsTheReference`.")
        and "CONTROL" in claim_findings("link.exe refuses the pair, MEASURED by `FixtureNative.RunsTheReference`.",
                                        idx)[0][0], "CHECK 1: a control TEST must cite the run that judged it")
    arm(f("the layout keeps its order.") == [], "CHECK 1: a sentence naming no reference is no claim")
    arm(f("the arm `link-the-pe-writer-moves-it` pins link.exe's field in src/link/format/pe.cpp.") == [],
        "CHECK 1: a verdict word inside an arm id or a path is no verdict (no claim)")
    arm(f("the arm `link-the-pe-writer-moves-it` pins it: link.exe links the pair.") == ["uncarried"],
        "CHECK 1: a verdict in the prose beside an arm id is still a claim")
    arm(blank_comments_and_literals('int a; // gone_function\n"gone_literal"; b();').count("gone") == 0,
        "the code reader blanks comments and literals")
    arm(comment_blocks("a.json", '{"$xComment": "ld64 keeps the first.", "k": 1}') == ["ld64 keeps the first."],
        "the comment reader takes a document's $Comment strings")
    kb = doc_keys({"list": [{"name": "a", "v": 1}]})
    kw = doc_keys({"list": [{"name": "z", "v": 0}, {"name": "a", "v": 1}]})
    arm(all(k in kw for k in kb), "CHECK 2 (g): an inserted entry does not read as every later entry changed")
    arm(history_form("the key was removed in the fold") and not history_form("the key states the startups"),
        "CHECK 2: the history form is told from the present tense")
    arm(not history_form("the reader drops it before the link, then refuses"),
        "CHECK 2: a present-tense sentence with 'before'/'then'/'drops' is NOT history (no fail toward clean)")

    # The repository arms: a base commit, then a working tree that removes an arm, a TEST, a key, a path, a symbol.
    if shutil.which("git") is None:
        arm(False, "git is on this host's PATH", "the repository arms need git")
    else:
        tmp = tempfile.mkdtemp(prefix="check-row-truth-")
        try:
            def w(rel, text):
                p = os.path.join(tmp, rel)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                io.open(p, "w", encoding="utf-8", newline="\n").write(text)

            def g(*a):
                subprocess.run(["git", "-C", tmp] + list(a), check=True, capture_output=True)
            reg_head = ("# r\n\n" + anchors.TABLE_HEADER + "\n" + anchors.SEP_ROW_TEXT + "\n")
            w(anchors.REL["production"], reg_head
              + "| `FIXTURE-ROW-ONE` | P1 | 🟠 OPEN | closes on the arm `fixture-arm-gone` and `goneFunction` | "
                "the key `oldStartups` states it; the key `goneSymbols` holds the entry | - |\n"
              + "| `FIXTURE-ROW-TWO` | P1 | 🟠 OPEN | the arm `fixture-arm-gone` was retired in the fold | - | - |\n"
              + "| `FIXTURE-ROW-THREE` | P1 | 🟠 OPEN | each name has an address and the entry is main; `goneEntry` is a word; "
                "a refused row states its `why` | - | - |\n"
              + "| `FIXTURE-ROW-FIVE` | P1 | 🟠 OPEN | every row's key `goneTally` holds its count | - | - |\n")
            w(anchors.REL["done"], reg_head)
            w("tests/mutations/arms.registry", "A | fixture-arm-gone | x |\nA | fixture-arm-kept | x |\n")
            w("src/a.cpp", "int goneFunction() { return 1; }\nint keptFunction() { return 2; }\n")
            # goneSymbols is removed WITH its named entries: its children `name`/`address` and its entry `goneEntry`
            # moved with it and are no subjects (only the top of a moved subtree is). `entry`, removed too, is a
            # word rows use in prose: only its citation AS A KEY is a finding. In `rows`, ONE entry drops `why` while
            # the other keeps it (an edit of that entry: no subject), and EVERY entry drops `goneTally` (a subject).
            w("src/dss-config/d.json", '{"oldStartups": ["a"], "kept": 1, "entry": "main", '
                                       '"goneSymbols": [{"name": "goneEntry", "address": "header"}], '
                                       '"rows": [{"name": "r1", "why": "x", "goneTally": 1}, '
                                       '{"name": "r2", "why": "y", "goneTally": 2}]}\n')
            w("tests/t.cpp", "TEST(FixtureSuite, GoneCase) {}\nTEST(FixtureSuite, KeptCase) {}\n"
                             "TEST(FixtureSuite, AssertsTheOldRefusal) { EXPECT_EQ(count(K_FixtureRefused), 1u); }\n"
                             "TEST(FixtureSuite, RestatedCase) { EXPECT_EQ(count(K_FixtureRefused), 1u); }\n"
                             "TEST(FixtureSuite, CitesTheGoneArm) { /* claimed by fixture-arm-gone */ }\n"
                             "TEST(FixtureSuite, NamesAMembersName) { EXPECT_EQ(sym.name, sym.address); }\n"
                             "TEST(FixtureSuite, ReadsTheGoneKey) { EXPECT_TRUE(doc.has(\"goneSymbols\")); }\n")
            g("init", "-q")
            g("-c", "user.email=selftest@invalid", "-c", "user.name=selftest", "add", "-A")
            g("-c", "user.email=selftest@invalid", "-c", "user.name=selftest", "commit", "-q", "-m", "base")
            base = subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"], capture_output=True,
                                  text=True).stdout.strip()
            w("tests/mutations/arms.registry", "A | fixture-arm-kept | x |\n")
            w("src/a.cpp", "// goneFunction was removed\nint keptFunction() { return 2; }\n")
            w("src/dss-config/d.json", '{"oldStartups": {"a": ["v"]}, "kept": 1, '
                                       '"rows": [{"name": "r1"}, {"name": "r2", "why": "y"}]}\n')
            w("tests/t.cpp", "TEST(FixtureSuite, KeptCase) {}\n"
                             "TEST(FixtureSuite, AssertsTheOldRefusal) { EXPECT_EQ(count(K_FixtureRefused), 1u); }\n"
                             "TEST(FixtureSuite, RestatedCase) { EXPECT_EQ(count(K_FixtureRefused), 0u); }\n"
                             "TEST(FixtureSuite, CitesTheGoneArm) { /* claimed by fixture-arm-gone */ }\n"
                             "TEST(FixtureSuite, NamesAMembersName) { EXPECT_EQ(sym.name, sym.address); }\n"
                             "TEST(FixtureSuite, ReadsTheGoneKey) { EXPECT_TRUE(doc.has(\"goneSymbols\")); }\n")
            rev_dir = tempfile.mkdtemp(prefix="check-row-truth-reversals-")
            reversals = os.path.join(rev_dir, "reversals.txt")
            io.open(reversals, "w", encoding="utf-8", newline="\n").write(
                "# the fold's one reversal\nthe refusal became a warning :: K_FixtureRefused\n")
            files_list = os.path.join(rev_dir, "files.txt")
            io.open(files_list, "w", encoding="utf-8", newline="\n").write("# the comments to read\nsrc/a.cpp\n\n")
            arm(read_files_list(files_list) == ["src/a.cpp"], "--files: a list names tree files, one per line")
            for bad in ("../outside.cpp", "/abs/a.cpp"):
                io.open(files_list, "w", encoding="utf-8", newline="\n").write(bad + "\n")
                try:
                    read_files_list(files_list)
                    refused = False
                except anchors.Refused:
                    refused = True
                arm(refused, "--files: a listed path that leaves the tree (%s) is refused" % bad)
            lines = []
            rc = run(tmp, base, "", "", reversals, out=lines.append)
            text = "\n".join(lines)
            shutil.rmtree(rev_dir, ignore_errors=True)
            arm("LEAD TEST FixtureSuite.AssertsTheOldRefusal names K_FixtureRefused" in text,
                "CHECK 2 (h): a TEST the fold left untouched that names a reversed subject is a LEAD", text)
            arm("FixtureSuite.RestatedCase names" not in text,
                "CHECK 2 (h): a TEST the fold rewrote is presumed to state the reversal (no lead)", text)
            arm("FixtureSuite.KeptCase names" not in text, "CHECK 2 (h): a TEST naming no subject is no lead", text)
            arm("LEAD TEST FixtureSuite.CitesTheGoneArm names fixture-arm-gone" in text,
                "CHECK 2 (b): a TEST naming a removed arm is a LEAD", text)
            arm(rc == 1, "CHECK 2: a fold's removals cited as current are a finding (exit 1)", text)
            arm("ROW FIXTURE-ROW-ONE cites fixture-arm-gone" in text, "CHECK 2 (b): a removed arm cited as current",
                text)
            arm("ROW FIXTURE-ROW-ONE cites goneFunction" in text,
                "CHECK 2 (e): a removed symbol cited as current, though a comment still names it", text)
            arm("ROW FIXTURE-ROW-ONE cites oldStartups" in text, "CHECK 2 (g): a reshaped key cited as current", text)
            arm("ROW FIXTURE-ROW-ONE cites goneSymbols" in text, "CHECK 2 (g): a removed key cited as current", text)
            arm("FIXTURE-ROW-TWO" not in text, "CHECK 2: a citation in history form is no finding", text)
            arm("FIXTURE-ROW-THREE" not in text,
                "CHECK 2 (g): a removed key's children and entries are no subjects, and a plain word cites no key",
                text)
            arm("cites why" not in text,
                "CHECK 2 (g): a key ONE entry drops while another entry of its list keeps it is no subject", text)
            arm("ROW FIXTURE-ROW-FIVE cites goneTally" in text,
                "CHECK 2 (g): a key EVERY entry of its list drops is a subject, cited as current", text)
            arm("FixtureSuite.NamesAMembersName names" not in text,
                "CHECK 2 (g): a TEST naming a member that shares a moved key's child leaf is no lead", text)
            arm("LEAD TEST FixtureSuite.ReadsTheGoneKey names goneSymbols" in text,
                "CHECK 2 (g): a TEST naming a removed key in a string literal is a LEAD", text)
            # the positive: restate row one in history form; no finding remains
            w(anchors.REL["production"], reg_head
              + "| `FIXTURE-ROW-ONE` | P1 | 🟠 OPEN | the arm `fixture-arm-gone` and `goneFunction` were removed | "
                "the key `oldStartups` was reshaped to a map; the key `goneSymbols` was removed | - |\n"
              + "| `FIXTURE-ROW-TWO` | P1 | 🟠 OPEN | the arm `fixture-arm-gone` was retired in the fold | - | - |\n"
              + "| `FIXTURE-ROW-THREE` | P1 | 🟠 OPEN | each name has an address and the entry is main; `goneEntry` is a word | - | - |\n")
            lines = []
            rc = run(tmp, base, "", "", "", out=lines.append)
            arm(rc == 0, "CHECK 2: every citation restated in history form passes (exit 0)", "\n".join(lines))

            # THE ADJUDICATION RECORD: an entry exempts exactly what it names; a stale one and a kept removed name
            # are findings. A row the fold did not rewrite cites the reversed subject in a sentence still true.
            w(anchors.REL["done"], reg_head
              + "| `FIXTURE-ROW-FOUR` | P1 | ✅ DONE | a refused pair gives `K_FixtureRefused` and nothing is "
                "written | - | - |\n"
              + "| `FIXTURE-ROW-FIVE` | P1 | ✅ DONE | link.exe refuses the pair. lld-link keeps the first. | - | - |\n"
              + "| `FIXTURE-ROW-SIX` | P1 | ✅ DONE | it closes on `fixture-arm-gone` | - | - |\n")
            g("-c", "user.email=selftest@invalid", "-c", "user.name=selftest", "add", "-A")
            g("-c", "user.email=selftest@invalid", "-c", "user.name=selftest", "commit", "-q", "-m", "rows")
            base2 = subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"], capture_output=True,
                                   text=True).stdout.strip()
            rev_dir = tempfile.mkdtemp(prefix="check-row-truth-adj-")
            try:
                reversals = os.path.join(rev_dir, "reversals.txt")
                io.open(reversals, "w", encoding="utf-8", newline="\n").write(
                    "the refusal became a warning :: K_FixtureRefused\n")
                adj = os.path.join(rev_dir, "adjudications.txt")

                def judge(adj_text, rows="", cells="", against=None):
                    io.open(adj, "w", encoding="utf-8", newline="\n").write(adj_text)
                    got = []
                    code = run(tmp, against or base2, rows, "", reversals, out=got.append, cells_arg=cells,
                               adjudications_arg=adj)
                    return code, "\n".join(got)
                rc, text = judge("")
                arm("ROW FIXTURE-ROW-FOUR cites K_FixtureRefused" in text,
                    "CHECK 2 (h): a row the fold did not rewrite, citing a reversed subject, is a finding", text)
                rc, text = judge("kept FIXTURE-ROW-FOUR :: K_FixtureRefused :: a refused pair still writes nothing\n")
                arm("KEPT (adjudicated) FIXTURE-ROW-FOUR cites K_FixtureRefused" in text
                    and "ROW FIXTURE-ROW-FOUR" not in text,
                    "ADJUDICATION: a kept entry exempts the row's reversed subject, and says so", text)
                rc, text = judge("kept FIXTURE-ROW-FOUR :: K_FixtureRefused :: still true\n"
                                 "kept FIXTURE-ROW-FIVE :: K_FixtureRefused :: names nothing there\n")
                arm("STALE ADJUDICATION kept FIXTURE-ROW-FIVE" in text and rc == 1,
                    "ADJUDICATION: an entry that exempts no finding is itself a finding", text)
                # against the FIRST base the arm is removed: row six's citation of it is a finding no entry keeps
                rc, text = judge("kept FIXTURE-ROW-SIX :: fixture-arm-gone :: a removed arm kept\n", against=base)
                arm("ROW FIXTURE-ROW-SIX cites fixture-arm-gone" in text
                    and "STALE ADJUDICATION kept FIXTURE-ROW-SIX" in text,
                    "ADJUDICATION: a REMOVED name is never kept (the finding stays, the entry is stale)", text)
                rc, text = judge("kept FIXTURE-ROW-FOUR :: K_FixtureRefused :: still true\n"
                                 "claim FIXTURE-ROW-FIVE :: link.exe refuses the pair :: the probe's paragraph\n",
                                 rows="FIXTURE-ROW-FIVE")
                arm("CARRIED (adjudicated) FIXTURE-ROW-FIVE: link.exe refuses the pair" in text
                    and "CLAIM FIXTURE-ROW-FIVE: uncarried: lld-link keeps the first" in text,
                    "ADJUDICATION: a claim entry exempts exactly its sentence, never its neighbour", text)
                rc, text = judge("kept FIXTURE-ROW-FOUR :: K_FixtureRefused :: still true\n"
                                 "claim FIXTURE-ROW-FIVE :: link.exe refuses the pair :: read by `linker::x` there\n",
                                 rows="FIXTURE-ROW-FIVE")
                arm("CARRIED (adjudicated) FIXTURE-ROW-FIVE: link.exe refuses the pair" in text,
                    "ADJUDICATION: a qualified `a::b` inside an entry is no separator", text)
                # THE OVERLAY: a lane's cell file restating row four in history form clears it; a folder whose cells
                # are the registry's own earns no rewritten row's exemption.
                cells = os.path.join(rev_dir, "rows")
                os.makedirs(os.path.join(cells, "FIXTURE-ROW-FOUR"))
                io.open(os.path.join(cells, "FIXTURE-ROW-FOUR", "trigger.txt"), "w", encoding="utf-8",
                        newline="\n").write("a refused pair gave `K_FixtureRefused` until the fold reversed it")
                rc, text = judge("", cells=cells)
                arm("FIXTURE-ROW-FOUR" not in text,
                    "OVERLAY: a lane's cell file restated in history form clears the row", text)
                io.open(os.path.join(cells, "FIXTURE-ROW-FOUR", "trigger.txt"), "w", encoding="utf-8",
                        newline="\n").write("a refused pair gave `K_FixtureRefused` until the fold. link.exe refuses it")
                rc, text = judge("", cells=cells)
                arm("CLAIM FIXTURE-ROW-FOUR: uncarried: link.exe refuses it" in text,
                    "OVERLAY: the judge reads the lane's cell file, not the registry's cell", text)
                io.open(os.path.join(cells, "FIXTURE-ROW-FOUR", "trigger.txt"), "w", encoding="utf-8",
                        newline="\n").write("a refused pair gives `K_FixtureRefused` and nothing is written")
                rc, text = judge("", cells=cells)
                arm("ROW FIXTURE-ROW-FOUR cites K_FixtureRefused" in text,
                    "OVERLAY: a folder holding the registry's own cells is no rewrite (the finding stays)", text)
            finally:
                shutil.rmtree(rev_dir, ignore_errors=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("check-row-truth: selftest: %s (%d arm(s), %d failure(s))"
          % ("OK" if failures[0] == 0 else "FAILED", count[0], failures[0]))
    return 0 if failures[0] == 0 else 1


def main(argv):
    pt = _path_types()
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--base", default="", type=pt.text_arg("--base"),
                    help="the fold's parent commit: CHECK 2's base, and the rows changed since it")
    ap.add_argument("--rows", default="", type=pt.text_arg("--rows"), help="row ids, comma-separated")
    ap.add_argument("--files", default="", type=pt.read_path_arg("--files", empty_means_absent=True),
                    help="a file naming the tree files whose comments CHECK 1 reads, one per line, relative to the "
                         "tree")
    ap.add_argument("--reversals", default="", type=pt.read_path_arg("--reversals", empty_means_absent=True),
                    help="the fold's reversals, one per line: <what> :: <subject words>")
    ap.add_argument("--cells", default="", type=pt.read_path_arg("--cells", empty_means_absent=True),
                    help="a lane's rows directory (<ROW-ID>/trigger.txt, closing.txt, cross-refs.txt), laid over the "
                         "registries' cells before the judge reads them")
    ap.add_argument("--adjudications", default="", type=pt.read_path_arg("--adjudications", empty_means_absent=True),
                    help="the fold's adjudication record: `claim|kept <owner> :: <key> :: <why>` per line")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return self_test()
    try:
        return run(anchors.ROOT, a.base, a.rows, a.files, a.reversals, cells_arg=a.cells,
                   adjudications_arg=a.adjudications)
    except anchors.Refused as exc:
        print("check-row-truth: %s" % exc)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
