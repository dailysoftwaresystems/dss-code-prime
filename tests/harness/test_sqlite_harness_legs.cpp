// BUILD ANY TARGET INSIDE ANY HOST, ITEM (2) — THE GATE ON THE DE-HOST-LOCKED
// SQLITE HARNESS DRIVERS.
//
// THE REQUIREMENT (user, 2026-07-25): "build ANY target inside ANY host, this
// MUST work." DSS-the-compiler already satisfies it — target selection is
// config-driven, and a Windows host has produced pe64 + elf64 + mach-o sqlite3
// binaries that RAN on their respective machines. The DRIVERS did not:
// `build-and-test.sh` derived its leg list from `uname` and `build-and-test.ps1`
// refused to start unless `$IsWindows`, so each conflated HOST with TARGET.
//
// WHAT THIS FILE ASSERTS, AND WHY IT IS HERE RATHER THAN IN THE DRIVER.
// The driver runs by hand, takes hours, and a corpus run is NOT part of ctest.
// Since 2026-09-21 it is ONE Python program — `build_and_test.py` and the flat
// `sqlite_*.py` modules beside it, which replaced the shell/PowerShell twins
// `build-and-test.sh` and `build-and-test.ps1` (the operator's order that no
// `.sh`/`.ps1` lives under the actions directory). Its leg decision is a single
// host-independent resolver (.harness-config/runner/actions/real-examples/c/sqlite/
// harness_legs.py) over a single declared catalogue (legs.json), and BOTH are
// cheap to interrogate. So the gate can hold the architecture to account on
// every leg, every run, in ~1 second:
//
//   1. VOCABULARY PIN — the resolver's verdict names are EXACTLY
//      `armVerdictName()` over `kAllArmVerdicts`, in order. The sqlite harness
//      does not get its own private words for "did not run".
//   2. HOST-INVARIANCE — for nine simulated hosts (including two the project
//      has never run on), the set of legs planned for BUILD is identical and
//      complete. This is the requirement restated as an executable property.
//   3. EXECUTION CAPABILITY IS THE ONLY THING A HOST CHANGES — the run plan
//      moves between native / launched / a NAMED skip, and never removes a leg.
//   4. NO SILENT SKIP — every non-run carries a verdict from (1) and a reason.
//   5. THE DRIVER ACTUALLY CONSULTS IT — it invokes the resolver, and no module
//      of it carries a hardcoded target spec or a host-keyed refusal. This is
//      the anti-regression pin: re-adding the retired twins' shape — the .ps1
//      opened with `if (-not $IsWindows) { Die ... }` — as
//      `if platform.system() != ...: die(...)`, or a target-spec literal, reds
//      here.
//   6. LAUNCHER VOCABULARY AGREEMENT — where the catalogue and the examples
//      corpus both declare a launcher for the same (targetArch, hostOs), they
//      must spell it the same way, so a reader cannot conclude the project has
//      two qemus.
//   7. EVERY LEG COMPILES AGAINST A THIRD-PARTY HEADER CONFIGURED FOR ITS OWN
//      TARGET — THE STAGED zconf.h USED TO BE PE-SHAPED. One staged zconf.h
//      used to serve all five legs, carrying the pe leg's `Z_HAVE_UNISTD_H`
//      answer — so the .ps1 refused every other leg and the .sh, which never
//      applied the flip at all, could not build its pe leg. The catalogue now
//      declares each target's answer, the driver stages one zinc/ per
//      recipeTransform through stage-zinc.py, and these tests hold all three
//      pieces to account: the declaration matches the target, the stage plan is
//      host-free, and the staging tool actually writes what was declared.
//
// PYTHON IS A HARD DEPENDENCY, NOT AN OPTIONAL ONE. The driver IS a Python
// program, and so are the resolver and the manifest generators. A machine
// without it cannot run the sqlite harness at all, so this test FAILS rather
// than skipping — a skip here would be the exact no-verdict defect the ledger
// it pins was created to end. It is also what reads the driver's code for the
// structural pins below (`pythonInspection`: Python's own tokenizer and parser
// decide what is a comment, a docstring or code).

#include "arm_verdict_ledger.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "test_wait_budget.hpp"  // kWaitBudget / kHelperScriptBudget

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using dss::test_support::ArmVerdict;
using dss::test_support::armVerdictName;
using dss::test_support::findOnPath;
using dss::test_support::kAllArmVerdicts;
using json = nlohmann::json;

namespace {

// The repo root, through the ONE test-side resolver (`repo_root.hpp`:
// `$DSS_CONFIG_ROOT` → the root CMake bakes in → a 12-hop cwd walk, every
// candidate validated as a directory containing `src/dss-config`).
//
// This used to be a private copy that diverged in two ways that matter: it
// took `DSS_CONFIG_ROOT` UNVALIDATED (so a stale export poisoned the whole
// file rather than falling through), and it had no knowledge of the baked
// `DSS_TEST_REPO_ROOT`, so running this binary directly — outside ctest, which
// always exports the variable — resolved nothing out-of-tree and returned an
// EMPTY path that then composed into `""/real-examples/c/sqlite` (the harness's
// home until 2026-09-18, when it moved under `.harness-config/runner/actions/`). The shared
// resolver throws instead, which GoogleTest reports as a failure of the one
// test that asked, naming all three sources it tried.
using dss::test::repoRoot;

// Where every program this repository ships lives: DssHarness's actions directory
// (`dssharness help layout`). The sqlite harness is the action `sqlite`, grouped under
// `real-examples/c` inside it — the operator's own subdirectory shape, kept when
// `real-examples/` and `scripts/` stopped existing on 2026-09-18.
[[nodiscard]] fs::path actionsDir() {
    return repoRoot() / ".harness-config" / "runner" / "actions";
}

[[nodiscard]] fs::path harnessDir() {
    return actionsDir() / "real-examples" / "c" / "sqlite";
}

// The interpreter. Resolved to a FULL PATH because `runBinary`'s Windows arm
// passes it as CreateProcessW's lpApplicationName, which does not search PATH.
[[nodiscard]] std::string pythonPath() {
    for (char const* name : {"python3", "python"}) {
        auto const p = findOnPath(name);
        if (!p.empty()) return p;
    }
    return {};
}

struct PyRun {
    bool        spawned = false;
    std::uint32_t exitCode = 0;
    std::string output;
    std::string diagnostic;
};

// Run the resolver under an EXPLICIT deadline. `trailing` is the LAST argv
// element and must be a real, existing file: `runBinary`'s POSIX arm chmods its
// `binaryPath` to 0755 before spawning, so pointing it at a repo file would
// mutate a tracked mode bit. Every call therefore ends in
// `--catalogue <scratch copy of legs.json>`, which also means the resolver is
// exercised against a COPY of the shipped catalogue rather than a fixture
// invented here.
[[nodiscard]] PyRun spawnResolver(std::vector<std::string> const& args,
                                  fs::path const&                 catalogueCopy,
                                  std::chrono::milliseconds       deadline) {
    PyRun out;
    auto const py = pythonPath();
    if (py.empty()) {
        out.diagnostic =
            "python3 (or python) is not on PATH. The sqlite harness driver"
            " (build_and_test.py) and its resolver ARE Python programs, so this"
            " is a real unmet dependency and not a reason to skip the check.";
        return out;
    }
    std::vector<std::string> prefix{py,
                                    (harnessDir() / "harness_legs.py").string()};
    for (auto const& a : args) prefix.push_back(a);
    prefix.push_back("--catalogue");
    auto const res = dss::test_support::runBinary(
        catalogueCopy, deadline, /*captureStdout=*/true, prefix);
    out.spawned    = res.spawned && !res.timedOut;
    out.exitCode   = res.exitCode;
    out.output     = res.capturedStdout;
    out.diagnostic = res.diagnostic;
    return out;
}

// ── THE DEADLINE THE SPAWNS BELOW RUN UNDER IS THE SCRIPT'S, NOT THIS FILE'S ─
//
// THE ENV-PROBE TEST TIMEOUT WAS A MAGIC NUMBER, NOT THE DERIVED BUDGET.
//
// Every spawn here used to be bounded by a `std::chrono::seconds{120}` typed
// into the line above, while the subject of those spawns DERIVES the same
// quantity from the catalogue's declared sample windows and hands it to its own
// children (`harness_legs.py`: `kernel_probe_budget_seconds` =
// `KERNEL_PROBE_ENTRY_ALLOWANCE_SECONDS + KERNEL_PROBE_SAMPLE_SLACK x window`,
// under a comment reading "THE BUDGET IS DERIVED FROM THE DECLARED SAMPLE
// WINDOWS, not a magic number"). Two numbers for one quantity, and the typed one
// was SMALLER than the derived one for the shipped catalogue — 120 s against
// 200 s — so the caller SIGKILLed a probe that was doing exactly the work the
// catalogue asked for and reported `child timed out after 120000 ms`, which
// reads as a defect in the measurement rather than in the caller's arithmetic.
//
// ⇒ THE SCRIPT IS ASKED. `--print-probe-budget` prints, without sampling
// anything, what a `--probe-environment` run of those same probes may take. One
// computation, one owner, and tightening a `sampleSeconds` retightens this
// gate with no edit here at all.
//
// ★ ONE NUMBER FOR EVERY SPAWN IN THIS FILE, AND THAT IS THE POINT. Only ONE
// case below actually samples; every other spawn lints, resolves or refuses.
// Giving each call site the tighter budget its own argv deserves would put a
// per-call-site judgement back in a file whose whole defect was a per-call-site
// judgement — and the script's own arithmetic already proves the sampling
// budget DOMINATES the non-sampling one (same function, a zero window), so one
// number is an honest bound for all of them. `SetUpTestSuite` ASSERTS that
// domination against the script's own two answers rather than assuming it.
//
// ★★ THE MARGIN IS 1.0 — the script's number, unmultiplied — and since a margin
// is a judgement, here is the sentence. That budget is `120 s of KERNEL-ENTRY
// ALLOWANCE + 4x the declared window`, and the allowance is money set aside for
// ENTERING A COLD WSL DISTRO: work this spawn does not do, because it runs the
// probe in this host's own kernel. The margin is therefore already inside the
// number and it is large — 200 s against a probe measured at 20.3 s on the
// slowest host in the matrix (macOS 26.5 arm64, 2026-08-13), i.e. ~10x, of which
// 120 s is allowance that cannot be spent here at all. Multiplying it again
// would layer a second slack factor on top of the one the script declares, and
// two derivations of one quantity is the defect this anchor names.
struct SpawnBudget {
    std::chrono::milliseconds probeEnvironment{0};  // a run that SAMPLES
    std::chrono::milliseconds noSample{0};          // one that does not
};

// The ONE deadline this file still owns, and the only one it can: a process
// cannot ask a program how long it may take without first running it. It is now
// the SHARED cap `dss::test_support::kWaitBudget` (60 s) rather than a number
// written here — the wait is for something the child SHOULD ALREADY HAVE DONE,
// which is exactly what that budget was derived for.
//
// It bounds `--print-probe-budget`, which loads one JSON document and prints two
// floats — no sampling, no kernel entry, no dependence on the declared windows.
// That last clause is what makes it unable to repeat the defect above: the
// number it bounds cannot drift with the catalogue, because it does not read the
// catalogue's windows for anything but arithmetic. ✔MEASURED 2026-08-13, both
// ends of the matrix: 0.05 s on macOS 26.5 arm64 and a 0.154 s median (0.277 s
// worst of five) on this Windows box, so 60 s is ~220x the worst observed cost
// and ~9x the worst spawn-admission latency ever recorded in this repo (6841 ms
// at 16-way concurrency, `kAdmissionBudget`). It exists to stop a hang, not to
// police a slow machine — and that measurement is why the SHARED 60 s cap is the
// right one here rather than merely the convenient one.

// Ask the script what a probe run of `catalogueCopy` may cost. `diagnostic` is
// set (and the budget left zero) on any failure — a budget that could not be
// fetched must never fall back to a number chosen here, which is the whole
// defect one level up.
[[nodiscard]] SpawnBudget askSpawnBudget(fs::path const& catalogueCopy,
                                         std::string&    diagnostic) {
    SpawnBudget out;
    auto const r = spawnResolver({"--print-probe-budget"}, catalogueCopy,
                                 dss::test_support::kWaitBudget);
    if (!r.spawned) {
        diagnostic = "could not ask harness_legs.py for its spawn budget: "
                   + r.diagnostic;
        return out;
    }
    if (r.exitCode != 0u) {
        diagnostic = "harness_legs.py --print-probe-budget exited "
                   + std::to_string(r.exitCode) + ":\n" + r.output;
        return out;
    }
    auto const toMs = [](double seconds) {
        return std::chrono::milliseconds{
            static_cast<std::int64_t>(std::llround(seconds * 1000.0))};
    };
    try {
        auto const doc = json::parse(r.output);
        out.probeEnvironment =
            toMs(doc.at("probeEnvironmentSeconds").get<double>());
        out.noSample = toMs(doc.at("noSampleSeconds").get<double>());
    } catch (std::exception const& exc) {
        diagnostic = std::string{"harness_legs.py --print-probe-budget did not "
                                 "answer with the two budgets ("}
                   + exc.what() + "):\n" + r.output;
        return out;
    }
    if (out.probeEnvironment <= std::chrono::milliseconds::zero()) {
        diagnostic = "harness_legs.py --print-probe-budget answered a "
                     "non-positive budget, which bounds nothing:\n" + r.output;
    }
    return out;
}

// Fetched ONCE by the fixture, before any other spawn. Zero until then, and
// `runResolver` refuses to spawn under a zero rather than substituting one —
// a spawn that ran before the budget arrived would be back under a deadline
// nobody stated.
[[nodiscard]] std::chrono::milliseconds& resolverDeadline() {
    static std::chrono::milliseconds deadline{0};
    return deadline;
}

[[nodiscard]] PyRun runResolver(std::vector<std::string> const& args,
                                fs::path const&                 catalogueCopy) {
    if (resolverDeadline() <= std::chrono::milliseconds::zero()) {
        PyRun out;
        out.diagnostic =
            "the spawn deadline has not been fetched from harness_legs.py yet."
            " Every spawn in this file runs under the budget the SCRIPT derives"
            " from the catalogue's declared sample windows"
            " (--print-probe-budget); there is no local number to fall back to,"
            " because a local number IS the defect: a magic number standing in"
            " for the derived budget.";
        return out;
    }
    return spawnResolver(args, catalogueCopy, resolverDeadline());
}

// One scratch copy of the shipped catalogue, made once and shared. A per-test
// copy would be tidier in isolation and would also make every test pay the file
// copy; the catalogue is read-only to the resolver, so one copy is honest.
class HarnessLegs : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        scratch_ = new dss::test_support::ScratchDir(
            dss::test_support::Location::Temp, "sqlite-harness-legs");
        auto const src = harnessDir() / "legs.json";
        ASSERT_TRUE(fs::exists(src))
            << "the sqlite harness leg catalogue is missing: " << src
            << "\nIt is the declared, host-free answer to 'which targets does"
               " this harness build?' — without it the driver has nothing to"
               " read and would fall back to keying on the host.";
        catalogue_ = scratch_->path() / "legs.json";
        std::error_code ec;
        fs::copy_file(src, catalogue_, fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "could not stage the catalogue: " << ec.message();
        // …and then ask the script how long it may take, BEFORE any spawn that
        // needs the answer. See the SpawnBudget block above for why the number
        // is fetched rather than typed.
        std::string why;
        budget_ = askSpawnBudget(catalogue_, why);
        ASSERT_TRUE(why.empty()) << why;
        ASSERT_GE(budget_.probeEnvironment, budget_.noSample)
            << "the script's budget for a run that SAMPLES came out below its"
               " budget for one that samples nothing, so the single deadline"
               " this file applies to every spawn is no longer an upper bound"
               " for the cheap ones. Both come from kernel_probe_budget_seconds"
               " — the sampling one at the catalogue's declared windows, the"
               " other at a zero window — so this ordering is arithmetic, and"
               " its failure means the derivation changed shape.";
        resolverDeadline() = budget_.probeEnvironment;
    }
    static void TearDownTestSuite() {
        delete scratch_;
        scratch_ = nullptr;
    }

    [[nodiscard]] static PyRun run(std::vector<std::string> const& args) {
        return runResolver(args, catalogue_);
    }

    // `--plan` WITHOUT the 20 s environment-probe sample. See the note above the
    // definition in the .cpp; `planShape` is for cases about leg RESOLUTION, and
    // the gate's own cases spell `run` with the flag by hand, so the measurement
    // really happens where the measurement is the subject.
    [[nodiscard]] static PyRun planShape(std::vector<std::string> args) {
        args.emplace_back("--environment-probes");
        args.emplace_back("skip");
        return runResolver(args, catalogue_);
    }

    static dss::test_support::ScratchDir* scratch_;
    static fs::path                       catalogue_;
    static SpawnBudget                    budget_;
};

dss::test_support::ScratchDir* HarnessLegs::scratch_   = nullptr;
fs::path                       HarnessLegs::catalogue_ = {};
SpawnBudget                    HarnessLegs::budget_    = {};

// One resolved leg out of a `--plan` document, by label. Returns an EMPTY object
// when the label is absent rather than throwing, so a caller's ASSERT names the
// missing leg instead of the test dying inside nlohmann.
[[nodiscard]] json legFrom(json const& plan, std::string const& label) {
    for (auto const& leg : plan.at("legs")) {
        if (leg.at("label").get<std::string>() == label) return leg;
    }
    return json::object();
}

[[nodiscard]] std::vector<std::string> splitLines(std::string const& s) {
    std::vector<std::string> out;
    std::istringstream in{s};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// Every simulated host the property must hold for. The last three are hosts
// this project has NEVER run on: an unrecognised OS, an unrecognised arch, and
// both at once. They are here because "build nothing on a machine I do not
// recognise" is the host-locking defect wearing a different hat.
struct SimHost {
    char const* os;
    char const* arch;
};
constexpr SimHost kSimHosts[] = {
    {"linux", "x86_64"},  {"linux", "arm64"},
    {"windows", "x86_64"}, {"windows", "arm64"},
    {"darwin", "arm64"},  {"darwin", "x86_64"},
    {"unknown", "x86_64"}, {"linux", "riscv64"}, {"unknown", "unknown"},
};

[[nodiscard]] std::string fileText(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ── WHAT COUNTS AS CODE ─────────────────────────────────────────────────────
//
// The dialect a script is read in, from its extension.
enum class Dialect { Shell, PowerShell, Python };

[[nodiscard]] Dialect dialectOf(fs::path const& p) {
    auto const ext = p.extension().string();
    if (ext == ".ps1") return Dialect::PowerShell;
    if (ext == ".py") return Dialect::Python;
    return Dialect::Shell;
}

[[nodiscard]] std::string trimmedLeft(std::string const& s) {
    auto const at = s.find_first_not_of(" \t");
    return at == std::string::npos ? std::string{} : s.substr(at);
}

// Both ends, newlines included: a statement's text joins its lines with '\n', so
// an argument that starts a continuation line begins with one.
[[nodiscard]] std::string trimmedBoth(std::string const& s) {
    auto const first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto const last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

[[nodiscard]] bool startsWith(std::string const& s, std::string_view prefix) {
    return s.rfind(prefix, 0) == 0;
}

// ── THE PYTHON HALF: PYTHON'S OWN TOKENIZER AND PARSER DECIDE ───────────────
//
// ★ WHY THE PYTHON RULE IS A PYTHON PROGRAM AND NOT A LINE RULE HERE. The shell
// rule below is a line rule because a shell comment is a line that starts with
// `#`. Python has two kinds of prose, and neither is a line shape: a `#` comment
// may TRAIL code (`import x  # why`), and a DOCSTRING is a string literal that is
// prose only by POSITION — the first statement of a module, class or function
// body — while the identical literal one statement later is code. Telling them
// apart needs a tokenizer that knows every string form (prefixes, triple quotes,
// f-strings) and a parser that knows statement boundaries. Re-implementing both
// here would be a second copy of the grammar that drifts with every Python
// release; the interpreter this file already hard-requires carries the one
// definition (`tokenize` + `ast`). So this file WRITES the program below into a
// scratch directory and runs it, ONCE per batch of files, and every answer is
// cached by (path, size, mtime) — ✔MEASURED 2026-09-22, Windows, Python 3.14.3,
// a loaded box: all 66 `.py` files under the actions directory in 2.13 s.
//
// THE RULE IT IMPLEMENTS, per file:
//   * PROSE = every COMMENT token (whole-line or trailing) and every DOCSTRING
//     (the first statement of a module/class/def/async def body, when that
//     statement is a plain `str` constant — an f-string or a bytes literal is
//     never a docstring). Prose is blanked to spaces, so what survives keeps its
//     RAW line number and its column; a line that is blank afterwards is dropped.
//   * CODE = everything else, and an ordinary string literal IS code: a message
//     a driver prints, a path it builds and an argv it spawns are all strings.
//   * STATEMENTS = each logical line's first and last raw line and the column of
//     its first token (tokenize's NEWLINE), so a structural pin can bound a block
//     by indentation and read a call that spans lines as ONE statement.
//   * WSL = the argv literals (a list or tuple whose first element names
//     wsl/wsl.exe) and the shell command lines that name it — see section 9.
//   * HOST GUARDS = a refusal (`raise`, or a call to die/exit/_exit/abort) that a
//     host automatic (`sys.platform`, `os.name`, `platform.system`) guards — see
//     `TheDriversAreNotHostKeyed`.
//   A file that cannot be read, decoded, tokenized or parsed answers an ERROR,
//   and every caller fails loud naming it: a subject this file cannot read is a
//   subject it cannot govern, never one it governed and cleared.
// `TheLiveLineRuleReadsPythonProseAsProseAndStringsAsCode` proves the rule on a
// synthetic file: a docstring or comment hit is dropped, a code hit is kept.
//
// ⚠ PYTHON 3.8 IS THE FLOOR of the program below (`end_lineno`), and it must stay
// runnable by the oldest interpreter on the four legs.
constexpr char const* kPyInspect = R"PYINSPECT(import ast
import io
import json
import re
import sys
import tokenize

WSL_NAMES = ("wsl", "wsl.exe")
WSL_WORD = re.compile(r"(?<![\w./\\$-])wsl(?:\.exe)?(?![\w.-])")
SHELL_FUNCS = {("os", "system"), ("os", "popen"), ("subprocess", "getoutput"),
               ("subprocess", "getstatusoutput")}
HOST_AUTOMATICS = {("sys", "platform"), ("os", "name"), ("platform", "system")}
REFUSAL_CALLS = {"die", "exit", "_exit", "abort"}


def const_str(n):
    return n.value if isinstance(n, ast.Constant) and isinstance(n.value, str) else None


def is_wsl(n):
    return (const_str(n) or "").replace("\\", "/").split("/")[-1].lower() in WSL_NAMES


def text_of(n):
    s = const_str(n)
    if s is not None:
        return s
    if isinstance(n, ast.JoinedStr):
        return "".join(const_str(v) or "" for v in n.values)
    if isinstance(n, ast.BinOp) and isinstance(n.op, ast.Add):
        a, b = text_of(n.left), text_of(n.right)
        return None if a is None and b is None else (a or "") + (b or "")
    return None


def dotted(n):
    if isinstance(n, ast.Attribute) and isinstance(n.value, ast.Name):
        return (n.value.id, n.attr)
    return None


def host_automatic(expr):
    for n in ast.walk(expr):
        if dotted(n) in HOST_AUTOMATICS:
            return "%s.%s" % dotted(n)
    return ""


def is_refusal(node):
    if isinstance(node, ast.Raise):
        return True
    call = node.value if isinstance(node, ast.Expr) else node
    f = call.func if isinstance(call, ast.Call) else None
    return f is not None and (f.id if isinstance(f, ast.Name) else getattr(f, "attr", "")) in REFUSAL_CALLS


def argv_verdict(elts):
    i = 1
    while i < len(elts) and const_str(elts[i]) == "--cd":
        i += 2
    if i >= len(elts):
        return "the argv literal ends before `-e`: nothing in it says the command is EXECUTED"
    s = const_str(elts[i])
    if s in ("-e", "--exec"):
        return ""
    if s == "--":
        return "spells `--`, which is NOT `--exec`: it hands the rest of the argv to the default shell"
    if isinstance(elts[i], ast.Starred):
        return "splices a starred value where `-e` must stand, so nothing here proves it is `-e`"
    return "puts %s where `-e` must stand" % (repr(s) if s is not None else "a computed value")


def inspect(path):
    with open(path, "rb") as fh:
        src = fh.read().decode("utf-8")
    src = src[1:] if src.startswith(chr(0xFEFF)) else src
    src = src.replace("\r\n", "\n")
    if "\r" in src:
        raise ValueError("a CR outside a CRLF: the line numbers here would not be the file's")
    lines = src.split("\n")
    grid = [list(ln) for ln in lines]

    def blank(r1, c1, r2, c2):
        for r in range(r1, r2 + 1):
            row = grid[r - 1]
            for i in range(c1 if r == r1 else 0, min(c2 if r == r2 else len(row), len(row))):
                row[i] = " "

    def col(r, byte_col):
        return len(lines[r - 1].encode("utf-8")[:byte_col].decode("utf-8", "replace"))

    stmts, start = [], None
    for tok in tokenize.generate_tokens(io.StringIO(src).readline):
        if tok.type == tokenize.COMMENT:
            blank(tok.start[0], tok.start[1], tok.end[0], tok.end[1])
        elif tok.type == tokenize.NEWLINE:
            if start is not None:
                stmts.append([start[0], tok.start[0], start[1]])
            start = None
        elif tok.type not in (tokenize.NL, tokenize.INDENT, tokenize.DEDENT,
                              tokenize.ENDMARKER) and start is None:
            start = tok.start
    tree = ast.parse(src, filename=path)
    members = {id(c) for n in ast.walk(tree) if isinstance(n, ast.Compare)
               for op, c in zip(n.ops, n.comparators) if isinstance(op, (ast.In, ast.NotIn))}
    inv, wsl, guards = 0, [], []
    for node in ast.walk(tree):
        body = getattr(node, "body", None)
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) \
                and body and isinstance(body[0], ast.Expr) and const_str(body[0].value) is not None:
            d = body[0].value
            blank(d.lineno, col(d.lineno, d.col_offset), d.end_lineno,
                  col(d.end_lineno, d.end_col_offset))
        if isinstance(node, (ast.List, ast.Tuple)) and node.elts and is_wsl(node.elts[0]) \
                and id(node) not in members \
                and not (len(node.elts) > 1 and all(is_wsl(e) for e in node.elts)):
            inv += 1
            why = argv_verdict(node.elts)
            if why:
                wsl.append([node.lineno, why])
        elif isinstance(node, ast.Call):
            shell = any(k.arg == "shell" and isinstance(k.value, ast.Constant)
                        and k.value.value is True for k in node.keywords)
            if shell or dotted(node.func) in SHELL_FUNCS:
                cmd = node.args[0] if node.args else next(
                    (k.value for k in node.keywords if k.arg in ("args", "cmd", "command")), None)
                text = text_of(cmd) if cmd is not None else None
                if text is not None and WSL_WORD.search(text):
                    inv += 1
                    wsl.append([node.lineno, "hands a command LINE naming wsl to a SHELL, which "
                                             "parses it before wsl.exe runs; pass an argv list"])
        elif isinstance(node, (ast.If, ast.IfExp, ast.Assert)) and host_automatic(node.test):
            h = host_automatic(node.test)
            if isinstance(node, ast.Assert):
                guards.append([node.lineno, "an assertion on `%s`" % h])
            else:
                arms = node.body + node.orelse if isinstance(node, ast.If) else [node.body, node.orelse]
                for a in arms:
                    if is_refusal(a):
                        guards.append([a.lineno, "a refusal guarded by `%s`" % h])
    live = [[i + 1, "".join(r).rstrip()] for i, r in enumerate(grid) if "".join(r).strip()]
    return {"live": live, "stmts": stmts, "wslInvocations": inv, "wslRefusals": wsl,
            "hostGuards": guards}


def main(argv):
    if len(argv) != 2:
        print("usage: pyinspect.py <request.json>", file=sys.stderr)
        return 2
    with open(argv[1], "r", encoding="utf-8") as fh:
        req = json.load(fh)
    answer = {}
    for path in req["files"]:
        try:
            answer[path] = inspect(path)
        except (OSError, UnicodeDecodeError, SyntaxError, ValueError, tokenize.TokenError) as exc:
            answer[path] = {"error": "%s: %s" % (type(exc).__name__, exc)}
    with open(req["out"], "w", encoding="utf-8", newline="\n") as fh:
        json.dump({"python": "%d.%d.%d" % sys.version_info[:3], "files": answer}, fh)
    print("pyinspect: %d file(s)" % len(answer))
    return 0


sys.exit(main(sys.argv))
)PYINSPECT";

// One statement (logical line) of a Python file, as the inspector bounded it.
struct PyStatement {
    std::size_t first  = 0;   // the raw line it starts on
    std::size_t last   = 0;   // the raw line it ends on
    std::size_t indent = 0;   // the column of its first token
    std::string text;         // its LIVE lines first..last, joined with '\n'
};

struct PyNote {
    std::size_t line = 0;     // the raw line
    std::string why;
};

struct PyInspection {
    std::string                                      error;   // non-empty: unreadable
    std::vector<std::pair<std::size_t, std::string>> live;    // (raw line, code)
    std::vector<PyStatement>                         statements;
    std::size_t                                      wslInvocations = 0;
    std::vector<PyNote>                              wslRefusals;
    std::vector<PyNote>                              hostGuards;
};

// The inspector's answers, cached for the life of the process by (path, size,
// mtime): a file rewritten in place is inspected again, a file read by twelve
// pins is inspected once.
class PyInspector {
public:
    [[nodiscard]] static PyInspector& instance() {
        static PyInspector the;
        return the;
    }

    // Inspect every file of `files` not yet answered, in ONE spawn. Throws —
    // which GoogleTest reports as a failure of the test that asked, with this
    // message — when the inspector cannot be run or its answer cannot be read.
    void prefetch(std::vector<fs::path> const& files) {
        std::vector<std::pair<std::string, std::string>> todo;   // key, path sent
        for (auto const& f : files) {
            auto const key = keyOf(f);
            if (cache_.count(key) != 0u) continue;
            bool const queued = std::any_of(todo.begin(), todo.end(),
                                            [&key](auto const& t) { return t.first == key; });
            if (!queued) todo.emplace_back(key, fs::absolute(f).lexically_normal().string());
        }
        if (todo.empty()) return;
        auto const py = pythonPath();
        if (py.empty()) {
            throw std::runtime_error(
                "python3 (or python) is not on PATH, so the Python driver's code cannot be"
                " read: this file asks Python's own tokenizer and parser what is a comment,"
                " a docstring or code. The driver itself IS Python, so this is a real unmet"
                " dependency and not a reason to skip.");
        }
        auto const helper = dir_.path() / "pyinspect.py";
        if (!fs::exists(helper)) {
            std::ofstream out(helper, std::ios::binary);
            out << kPyInspect;
        }
        ++spawns_;
        auto const tag     = std::to_string(spawns_);
        auto const request = dir_.path() / ("request-" + tag + ".json");
        auto const answer  = dir_.path() / ("answer-" + tag + ".json");
        json req = json::object();
        req["out"]   = answer.string();
        req["files"] = json::array();
        for (auto const& [k, p] : todo) req["files"].push_back(p);
        {
            std::ofstream out(request, std::ios::binary);
            out << req.dump();
        }
        // The REQUEST rides last: `runBinary`'s POSIX arm chmods its last argv
        // element, and a scratch file is the only thing it may touch.
        auto const res = dss::test_support::runBinary(
            request, dss::test_support::kHelperScriptBudget, /*captureStdout=*/true,
            {py, "-B", helper.string()});
        if (!res.spawned || res.timedOut) {
            throw std::runtime_error("the Python inspector (" + helper.string()
                                     + ") could not be run: " + res.diagnostic);
        }
        if (res.exitCode != 0u) {
            throw std::runtime_error("the Python inspector exited "
                                     + std::to_string(res.exitCode) + ":\n"
                                     + res.capturedStdout);
        }
        json doc;
        try {
            doc = json::parse(fileText(answer));
            python_ = doc.at("python").get<std::string>();
            for (auto const& [key, path] : todo) {
                cache_[key] = fromJson(doc.at("files").at(path));
            }
        } catch (std::exception const& exc) {
            throw std::runtime_error(std::string{"the Python inspector's answer "}
                                     + answer.string() + " could not be read ("
                                     + exc.what() + "):\n" + res.capturedStdout);
        }
    }

    [[nodiscard]] PyInspection const& at(fs::path const& file) {
        prefetch({file});
        return cache_.at(keyOf(file));
    }

    [[nodiscard]] std::string const& python() const { return python_; }

private:
    PyInspector() = default;

    [[nodiscard]] static std::string keyOf(fs::path const& p) {
        std::error_code sizeEc, timeEc;
        auto const size = fs::file_size(p, sizeEc);
        auto const when = fs::last_write_time(p, timeEc);
        // The count goes through `nanoseconds` because `file_time_type`'s own rep is the library's
        // choice: libc++ counts in `__int128`, which no `std::to_string` overload takes (✔MEASURED
        // 2026-09-23: "call to 'to_string' is ambiguous" on AppleClang 21, the only leg that failed).
        auto const ns = std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch());
        return fs::absolute(p).lexically_normal().generic_string() + '|'
             + (sizeEc ? std::string{"?"} : std::to_string(size)) + '|'
             + (timeEc ? std::string{"?"} : std::to_string(ns.count()));
    }

    [[nodiscard]] static PyInspection fromJson(json const& rec) {
        PyInspection in;
        if (rec.contains("error")) {
            in.error = rec.at("error").get<std::string>();
            return in;
        }
        for (auto const& l : rec.at("live")) {
            in.live.emplace_back(l.at(0).get<std::size_t>(), l.at(1).get<std::string>());
        }
        for (auto const& s : rec.at("stmts")) {
            PyStatement st;
            st.first  = s.at(0).get<std::size_t>();
            st.last   = s.at(1).get<std::size_t>();
            st.indent = s.at(2).get<std::size_t>();
            auto it = std::lower_bound(
                in.live.begin(), in.live.end(), st.first,
                [](auto const& l, std::size_t n) { return l.first < n; });
            for (; it != in.live.end() && it->first <= st.last; ++it) {
                if (!st.text.empty()) st.text += '\n';
                st.text += it->second;
            }
            if (!st.text.empty()) in.statements.push_back(std::move(st));
        }
        in.wslInvocations = rec.at("wslInvocations").get<std::size_t>();
        for (auto const& r : rec.at("wslRefusals")) {
            in.wslRefusals.push_back({r.at(0).get<std::size_t>(), r.at(1).get<std::string>()});
        }
        for (auto const& g : rec.at("hostGuards")) {
            in.hostGuards.push_back({g.at(0).get<std::size_t>(), g.at(1).get<std::string>()});
        }
        return in;
    }

    dss::test_support::ScratchDir       dir_{dss::test_support::Location::Temp,
                                             "sqlite-harness-pyinspect"};
    std::map<std::string, PyInspection> cache_;
    std::string                         python_;
    unsigned                            spawns_ = 0;
};

// What the inspector said about `p`, errors included (the WSL walk reports an
// unreadable file as a refusal rather than stopping at it).
[[nodiscard]] PyInspection const& pythonInspection(fs::path const& p) {
    return PyInspector::instance().at(p);
}

// …and the same, for a caller that can only proceed on code it could read.
[[nodiscard]] PyInspection const& pythonCode(fs::path const& p) {
    auto const& in = pythonInspection(p);
    if (!in.error.empty()) {
        throw std::runtime_error(
            p.string() + " could not be read as Python by python "
            + PyInspector::instance().python() + ": " + in.error
            + "\nA file this test cannot read is a file its pins cannot govern, and"
              " the driver could not run on this leg's interpreter either.");
    }
    return in;
}

// The live text of raw line `n`, for a diagnostic ("" when the line is prose).
[[nodiscard]] std::string liveTextAt(PyInspection const& in, std::size_t n) {
    for (auto const& [line, text] : in.live) {
        if (line == n) return text;
    }
    return {};
}

// The body of the block that statement `head` opens: the statements after it
// indented deeper than it, up to the first that is not. Empty for a statement
// that opens nothing (or whose body sits on its own line, `else: return`).
[[nodiscard]] std::vector<PyStatement> blockOf(std::vector<PyStatement> const& stmts,
                                               std::size_t                     head) {
    std::vector<PyStatement> out;
    for (std::size_t i = head + 1; i < stmts.size(); ++i) {
        if (stmts[i].indent <= stmts[head].indent) break;
        out.push_back(stmts[i]);
    }
    return out;
}

// The index of the first statement at or after `from` whose text, left-trimmed,
// starts with `prefix`; `stmts.size()` when there is none.
[[nodiscard]] std::size_t statementStarting(std::vector<PyStatement> const& stmts,
                                            std::string_view prefix,
                                            std::size_t      from = 0) {
    for (std::size_t i = from; i < stmts.size(); ++i) {
        if (startsWith(trimmedLeft(stmts[i].text), prefix)) return i;
    }
    return stmts.size();
}

// The body of `def <name>(` in `stmts`, joined; empty when there is no such def.
[[nodiscard]] std::string functionBody(std::vector<PyStatement> const& stmts,
                                       std::string const&              name) {
    auto const at = statementStarting(stmts, "def " + name + "(");
    std::string out;
    if (at == stmts.size()) return out;
    for (auto const& s : blockOf(stmts, at)) out += s.text + '\n';
    return out;
}

// The top-level arguments of the call whose `(` sits at `open` in `text`, each
// trimmed, as written. Quote-aware for '…' and "…" (a backslash escapes the next
// character, as the tokenizer reads both raw and ordinary strings), nesting of
// ()/[]/{} tracked. EMPTY when the call never closes — so a text this cannot
// read fails the caller's check instead of passing it.
[[nodiscard]] std::vector<std::string> callArguments(std::string const& text,
                                                     std::size_t        open) {
    std::vector<std::string> args;
    std::string              cur;
    int                      depth  = 0;
    char                     quote  = 0;
    bool                     closed = false;
    for (std::size_t i = open + 1; i < text.size() && !closed; ++i) {
        char const c = text[i];
        if (quote != 0) {
            cur += c;
            if (c == '\\' && i + 1 < text.size()) {
                cur += text[++i];
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        switch (c) {
            case '\'': case '"': quote = c; cur += c; break;
            case '(': case '[': case '{': ++depth; cur += c; break;
            case ')': case ']': case '}':
                if (depth == 0) {
                    closed = (c == ')');
                    if (!closed) return {};
                } else {
                    --depth;
                    cur += c;
                }
                break;
            case ',':
                if (depth == 0) {
                    args.push_back(trimmedBoth(cur));
                    cur.clear();
                } else {
                    cur += c;
                }
                break;
            default: cur += c; break;
        }
    }
    if (!closed) return {};
    if (!trimmedBoth(cur).empty()) args.push_back(trimmedBoth(cur));
    return args;
}

// The driver's LIVE lines — prose removed.
//
// ★ THIS IS LOAD-BEARING, and it was learned the hard way inside this cycle: the
// first version of `TheDriversAreNotHostKeyed` searched the RAW text for the
// removed host gate, and it went red against a correctly-converted driver —
// because the driver's header now DOCUMENTS the gate it removed ("Until TF-C114
// this driver opened with `if (-not $IsWindows) { Die ... }`"). A pin that
// punishes writing down what you removed teaches the wrong lesson and would
// have been "fixed" by deleting the explanation. Comments are prose ABOUT the
// code; only the code is the code.
//
// PYTHON: the inspector above (comments AND docstrings are prose; an ordinary
// string literal is code; a line keeps its text minus its trailing comment).
// SHELL/POWERSHELL: `#` line comments cover both shells; `<# … #>` block
// comments are PowerShell only and are passed over as well. Trailing comments
// after code are left in place deliberately — stripping them needs a
// quote-state parser, and a needle that only ever appears in a trailing comment
// is not a shape these rules look for.
[[nodiscard]] std::vector<std::string> liveLines(fs::path const& p, Dialect dialect) {
    std::vector<std::string> out;
    if (dialect == Dialect::Python) {
        for (auto const& [n, text] : pythonCode(p).live) out.push_back(text);
        return out;
    }
    bool const powershell = dialect == Dialect::PowerShell;
    std::istringstream in{fileText(p)};
    std::string line;
    bool inBlockComment = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto const first = line.find_first_not_of(" \t");
        std::string const trimmed =
            first == std::string::npos ? std::string{} : line.substr(first);
        if (powershell) {
            if (inBlockComment) {
                if (trimmed.find("#>") != std::string::npos) {
                    inBlockComment = false;
                }
                continue;
            }
            if (trimmed.rfind("<#", 0) == 0) {
                if (trimmed.find("#>") == std::string::npos) {
                    inBlockComment = true;
                }
                continue;
            }
        }
        if (trimmed.rfind("#", 0) == 0) continue;
        out.push_back(line);
    }
    return out;
}

// ── THE DRIVER, AS FILES ────────────────────────────────────────────────────
//
// The modules transcribed from the two retired drivers: `build_and_test.py` and
// every flat `sqlite_*.py` beside it, ENUMERATED so a module added later is
// governed the day it lands — except the two that port files these pins never
// read: `sqlite_base.py` (was `base-harness.{sh,ps1}`, which the artefact
// section below reads as the shared core) and `sqlite_coherence.py` (was
// `check-source-coherence.sh`, a program of its own). Both carry their
// `--self-test` fixtures inline, and those fixtures legitimately spell target
// specs, compiler flags and recipe file names that the driver itself must not
// (✔MEASURED 2026-09-22: every hit of the driver pins' negative rules outside
// this set was a `sqlite_base.py` fixture). ✔MEASURED 2026-09-22: 13 modules.
[[nodiscard]] std::vector<fs::path> driverModules() {
    std::vector<fs::path> out;
    std::error_code       ec;
    for (auto const& e : fs::directory_iterator(harnessDir(), ec)) {
        auto const name = e.path().filename().string();
        bool const module = name == "build_and_test.py"
                         || (startsWith(name, "sqlite_") && e.path().extension() == ".py");
        if (!module || name == "sqlite_base.py" || name == "sqlite_coherence.py") continue;
        out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    PyInspector::instance().prefetch(out);
    return out;
}

// ★ THE ENUMERATION'S FLOOR IS NAMED, NOT COUNTED: every module some pin below
// reads by name must be in it, so a collapsed walk (a moved directory, a renamed
// module) cannot leave the set-wide rules governing nothing while reporting a
// pass. Returns the names that are missing.
[[nodiscard]] std::string missingDriverModules(std::vector<fs::path> const& modules) {
    std::string missing;
    for (char const* name : {"build_and_test.py", "sqlite_build.py", "sqlite_common.py",
                             "sqlite_corpus.py", "sqlite_launch.py", "sqlite_libs.py",
                             "sqlite_units.py"}) {
        bool const found = std::any_of(modules.begin(), modules.end(), [name](fs::path const& m) {
            return m.filename().string() == name;
        });
        if (!found) missing += std::string{missing.empty() ? "" : " "} + name;
    }
    return missing;
}

// A scratch copy of the shipped catalogue with one edit applied, for the tests
// that must witness a LINT REFUSAL. Mutating the shipped file is not an option
// and inventing a fixture catalogue would let the mutation tests pass against a
// document the driver never reads, so each one starts from the real thing.
class MutatedCatalogue {
public:
    MutatedCatalogue(fs::path const& source, char const* tag)
        : dir_{dss::test_support::Location::Temp, tag} {
        doc_  = json::parse(fileText(source));
        path_ = dir_.path() / "legs.json";
    }
    [[nodiscard]] json& doc() { return doc_; }
    // Write and return the path. Called once the caller has edited `doc()`.
    [[nodiscard]] fs::path const& commit() {
        std::ofstream out(path_, std::ios::binary);
        out << doc_.dump(2) << '\n';
        out.close();
        return path_;
    }
    // The first launcher entry of the named leg — every mutation below targets
    // one, and finding it by label keeps the tests readable when the catalogue
    // grows a leg.
    [[nodiscard]] json& firstLauncherOf(std::string const& label) {
        for (auto& leg : doc_.at("legs")) {
            if (leg.at("label").get<std::string>() != label) continue;
            EXPECT_FALSE(leg.at("launchers").empty())
                << label << " declares no launcher to mutate";
            return leg.at("launchers").at(0);
        }
        ADD_FAILURE() << "no leg labelled '" << label << '\'';
        return doc_;  // unreachable in a passing run
    }
    // The leg itself, for the declarations that live beside `launchers` rather
    // than inside one — `confounds` is per LEG, which is the whole point: the
    // ledger used to be per DRIVER rather than per LEG.
    [[nodiscard]] json& legOf(std::string const& label) {
        for (auto& leg : doc_.at("legs")) {
            if (leg.at("label").get<std::string>() == label) return leg;
        }
        ADD_FAILURE() << "no leg labelled '" << label << '\'';
        return doc_;  // unreachable in a passing run
    }

private:
    dss::test_support::ScratchDir dir_;
    json                          doc_;
    fs::path                      path_;
};

// A `<arch>:<format>-exec` target spec written as a literal. The catalogue is
// the only place a leg's spec may be stated.
[[nodiscard]] bool mentionsTargetSpecLiteral(std::string const& line) {
    static constexpr std::string_view kArches[] = {"x86_64:", "arm64:"};
    for (auto const& arch : kArches) {
        std::size_t at = line.find(arch);
        while (at != std::string::npos) {
            auto const rest = line.find_first_of(" \t\"'`)]},;", at + arch.size());
            auto const tok  = line.substr(
                at, rest == std::string::npos ? std::string::npos : rest - at);
            if (tok.size() > 5 && tok.substr(tok.size() - 5) == "-exec") return true;
            at = line.find(arch, at + 1);
        }
    }
    return false;
}

// `x86_64:pe64-x86_64-windows-exec` -> `windows`. The python twin is
// `spec_target_os()`; the format names are `<container><bits>-<arch>-<os>-<kind>`.
[[nodiscard]] std::string specTargetOs(std::string const& spec) {
    auto const colon = spec.find(':');
    std::string const  fmt = colon == std::string::npos ? spec : spec.substr(colon + 1);
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= fmt.size(); ++i) {
        if (i == fmt.size() || fmt[i] == '-') {
            parts.push_back(fmt.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts.size() >= 3 ? parts[parts.size() - 2] : std::string{};
}

// The `./configure` block shape stage-zinc.py rewrites, reproduced as a FIXTURE.
// Not the machine's real zconf.h: this test must give the same answer on a box
// with no zlib installed, and the shipped header's exact line numbers are not
// this test's business. What IS its business is that the tool, driven by the
// SHIPPED catalogue, writes a per-stage header whose guard matches each leg's
// own target.
constexpr char const* kZconfFixture =
    "/* fixture standing in for a ./configure'd zconf.h */\n"
    "#ifndef ZCONF_H\n"
    "#define ZCONF_H\n"
    "#if 1    /* was set to #if 1 by ./configure */\n"
    "#  define Z_HAVE_UNISTD_H\n"
    "#endif\n"
    "\n"
    "#if 1    /* was set to #if 1 by ./configure */\n"
    "#  define Z_HAVE_STDARG_H\n"
    "#endif\n"
    "\n"
    "#ifndef Z_HAVE_UNISTD_H\n"
    "#  ifdef __WATCOMC__\n"
    "#    define Z_HAVE_UNISTD_H\n"
    "#  endif\n"
    "#endif\n"
    "#endif /* ZCONF_H */\n";

// The effective state of one guard in a staged zconf.h: the `#if N` line
// immediately above `#  define <guard>`. Mirrors what a preprocessor would do
// with the ./configure block, and deliberately does NOT reuse stage-zinc.py's
// own parser — a test that asked the tool to check itself would pass on any
// self-consistent mistake.
[[nodiscard]] int guardStateIn(fs::path const& zconf, std::string const& guard) {
    auto const lines = splitLines(fileText(zconf));
    std::string const needle = "#  define " + guard;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i] != needle) continue;
        auto const& prev = lines[i - 1];
        if (prev.rfind("#if 1", 0) == 0) return 1;
        if (prev.rfind("#if 0", 0) == 0) return 0;
        return -1;  // present but not a plain #if 1/#if 0
    }
    return -2;  // no ./configure site at all
}

}  // namespace

// ── 1. The vocabulary pin ──────────────────────────────────────────────────
//
// RED-ON-DISABLE: rename, reorder or drop any entry of `VERDICTS` in
// harness_legs.py (or add an ArmVerdict enumerator without mirroring it) and
// this fails naming both lists.
TEST_F(HarnessLegs, TheResolverSpeaksExactlyTheLedgersVerdictVocabulary) {
    auto const r = run({"--verdict-vocabulary"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;

    std::vector<std::string> want;
    for (ArmVerdict const v : kAllArmVerdicts) {
        want.emplace_back(armVerdictName(v));
    }
    auto const got = splitLines(r.output);
    EXPECT_EQ(got, want)
        << "the sqlite harness resolver's verdict names must be EXACTLY"
           " armVerdictName() over kAllArmVerdicts, in order — one vocabulary"
           " across all three corpus harnesses, or a reader has to learn two"
           " sets of words for the same fact.\n  ledger:   "
        << [&] {
               std::string s;
               for (auto const& w : want) s += w + " ";
               return s;
           }()
        << "\n  resolver: " << r.output;
}

// ── The resolver's own self-test + lint, run by the gate ───────────────────
//
// The driver runs these at its Step 0 so a broken plan refuses the run. That
// only helps someone who starts the driver. Running them here means a defect is
// caught by the ordinary gate instead of by a person three hours into a corpus.
TEST_F(HarnessLegs, TheResolverSelfTestPasses) {
    auto const r = run({"--self-test"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 0u) << r.output;
    EXPECT_NE(r.output.find("failed=0"), std::string::npos) << r.output;
}

TEST_F(HarnessLegs, TheLegCatalogueLintsClean) {
    auto const r = run({"--lint"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 0u) << r.output;
    EXPECT_NE(r.output.find("findings=0"), std::string::npos) << r.output;
}

// ── The SMOKE GATE's own self-test, run by the gate ────────────────────────
//
// `cli-smoke.py` is the file that decides whether a sqlite3 CLI failure is
// CHARGED TO THE COMPILER. Until TF-C136 it had no self-test and no coverage of
// any kind here, which is the wrong file to leave unwatched: it is an
// ATTRIBUTION instrument, so its failure mode is not a red gate but a confident
// wrong answer. It shipped one — a leg whose binary never launched (the guest
// loader was absent) was reported as fourteen DSS defects — the CLI smoke
// charging a LAUNCH failure to the COMPILER.
//
// Its `--self-test` is self-contained: no network, no build, no sqlite, a few
// seconds. It asserts the full cross product of {subject launched, not launched}
// x {control matched, matched-but-unlaunched, target-mismatch, absent} x {row
// passes, row fails} against verdict, rc and `dssImplicated`, and carries its own
// red-on-disable mutations.
//
// ⚠ TWO CONSTRAINTS COLLIDE HERE, AND THE SHIM IS WHAT RECONCILES THEM.
// `runBinary` builds `[launcherPrefix..., binaryPath]` and — on the POSIX arm —
// `chmod`s `binaryPath`, so the LAST argv element must be a real file. The gate,
// for its own good reasons, requires `--self-test` to be its ONLY argument, i.e.
// the last element. Passing the flag as `binaryPath` "works" on Windows and fails
// on Linux with a bare `spawned=false`, which is how it was found.
// So a SCRATCH COPY of the script rides last as the file the helper expects (a
// COPY, for the chmod reason spelled out at the staging step below), and a
// one-line `-c` shim forges the argv the gate wants. Do NOT "simplify" this back
// to putting the flag last: the Windows arm will accept it and the Linux gate
// will not.
TEST_F(HarnessLegs, TheCliSmokeGateSelfTestPasses) {
    auto const py = pythonPath();
    ASSERT_FALSE(py.empty())
        << "python3 (or python) is not on PATH. The driver IS Python, so this is a"
           " real unmet dependency and not a reason to skip the check.";
    auto const script = harnessDir() / "cli-smoke.py";
    ASSERT_TRUE(fs::exists(script))
        << "the sqlite3 CLI smoke gate is missing: " << script;
    // ★★ THE GATE RUNS FROM A SCRATCH COPY, NEVER FROM THE REPO FILE.
    // THE SELF-TEST USED TO chmod TRACKED SOURCE — CLOSED BY THIS BLOCK.
    // `runBinary`'s POSIX arm `chmod`s its `binaryPath` to 0755 before spawning —
    // it has to, because the linker writes emitted binaries 0644 and `execve`
    // needs the bit. Handing it a path under `real-examples/` therefore MUTATES A
    // TRACKED SOURCE FILE as a side effect of running the suite.
    // First SEEN on macOS (a `cmake --build` + `ctest` run left
    // `real-examples/c/sqlite/cli-smoke.py` at 100755 — mode only, zero content
    // change — restored clean, re-run, dirty again).
    // ✔MEASURED HERE 2026-08-12 on Linux, in a throwaway clone with
    // `core.filemode=true`, as a two-armed control rather than a sighting:
    //   arm 1, this call site UNFIXED — before: mode 644, `git status --porcelain`
    //     empty; after `ctest -R harness/test_sqlite_harness_legs` (which PASSED):
    //     mode 755 and ` M real-examples/c/sqlite/cli-smoke.py`;
    //   arm 2, with the staging below — restore, rebuild, re-run: mode 644 and an
    //     EMPTY porcelain, and then a FULL 826/826 suite left it empty too.
    // A test run that edits the working tree makes
    // `git status` lie, turns a clean CI checkout dirty, and on any
    // `core.filemode=true` host (macOS/Linux) the bit is eventually committed by
    // accident. It went unnoticed for so long because Windows — where this suite
    // is usually driven — sets `core.filemode=false` and simply cannot see it.
    // ⚠ The fix is NOT to commit the executable bit. That hides the mutation
    // instead of removing it, and leaves the same code ready to chmod whatever it
    // is pointed at next. The note above `runResolver` already named this trap and
    // the two other `runBinary` call sites in this file already dodge it with a
    // scratch copy; this was the ONE that did not.
    dss::test_support::ScratchDir work{dss::test_support::Location::Temp,
                                       "sqlite-cli-smoke-selftest"};
    // Copied under its OWN NAME: the gate prints `cli-smoke.py --self-test: …`
    // from a literal, but argv[0] and any traceback the self-test emits read as
    // the real script only if the basename survives the staging.
    auto const staged = work.path() / "cli-smoke.py";
    std::error_code stageEc;
    fs::copy_file(script, staged, fs::copy_options::overwrite_existing, stageEc);
    ASSERT_FALSE(stageEc) << "could not stage the smoke gate at " << staged
                          << ": " << stageEc.message();
    auto const modeBefore = fs::status(script).permissions();
    static constexpr char const* kSelfTestShim =
        "import runpy,sys;p=sys.argv[1];sys.argv=[p,'--self-test'];"
        "runpy.run_path(p,run_name='__main__')";
    auto const res = dss::test_support::runBinary(
        staged, dss::test_support::kHelperScriptBudget,
        /*captureStdout=*/true,
        {py, "-c", kSelfTestShim});
    // THE PIN, checked BEFORE the spawn assertion on purpose: a spawn that FAILED
    // has already run the chmod, so gating this behind `res.spawned` would let the
    // mutation through in exactly the case that also hides it. Reverting the
    // staging above to `runBinary(script, …)` reds HERE on any POSIX host instead
    // of silently dirtying the working tree. On Windows `permissions()` is
    // synthesised from one attribute and is equal on both sides, so this neither
    // fires nor false-fires there — the leg that CAN observe the defect is the leg
    // that enforces it.
    EXPECT_EQ(fs::status(script).permissions(), modeBefore)
        << "running the smoke gate CHANGED THE MODE of the tracked repo file "
        << script
        << ". The suite must never mutate its own source tree: that makes"
           " `git status` lie and eventually commits the bit by accident. Run the"
           " script from a scratch copy — do not 'fix' this by committing the"
           " executable bit.";
    ASSERT_TRUE(res.spawned && !res.timedOut) << res.diagnostic;
    EXPECT_EQ(res.exitCode, 0u) << res.capturedStdout;
    // CONTENT, not a count — and BOTH numbers, because either alone is
    // satisfiable by a run that asserted nothing. "0 failed" is trivially true of
    // an empty battery; "0 passed" is what catches it. The gate prints
    // `cli-smoke.py --self-test: <N> passed, <M> failed`.
    EXPECT_NE(res.capturedStdout.find("0 failed"), std::string::npos)
        << res.capturedStdout;
    EXPECT_EQ(res.capturedStdout.find("0 passed"), std::string::npos)
        << "the smoke gate's self-test reported ZERO assertions — present but"
           " exercising nothing, which reads exactly like coverage:\n"
        << res.capturedStdout;
}

// ── The pipe-buffer pin: draining AFTER exit deadlocks ────────────────────
//
// This test is the reason that anchor exists. `runBinary` used to drain the
// capture pipe only AFTER the child exited, so a child that outgrew the pipe's
// kernel buffer (4 KiB on Windows) blocked in write() while the parent blocked
// in WaitForSingleObject — a mutual stall that ended with the parent KILLING a
// perfectly healthy child at the timeout. This resolver's JSON plan is the
// first caller big enough to trip it, and it did, immediately.
//
// RED-ON-DISABLE (measured, both numbers in the cycle report): restore the
// drain-after-exit ordering and the two plan tests time out at 120 s each.
TEST_F(HarnessLegs, ALargePlanIsCapturedWholeRatherThanDeadlockingTheHarness) {
    auto const r = planShape({"--plan", "--host-os", "linux", "--host-arch", "x86_64",
                        "--launchers-none", "--format", "json"});
    ASSERT_TRUE(r.spawned)
        << r.diagnostic
        << "\n(a TIMEOUT here reads as 'the child hung' and means the exact"
           " opposite: the child is blocked writing into a pipe nobody is"
           " reading)";
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    // 4096 = the Windows anonymous-pipe default. Anything at or below it would
    // pass with the broken ordering too, which would make this pin theatre.
    EXPECT_GT(r.output.size(), 4096u)
        << "the plan no longer exceeds one pipe buffer, so this test no longer"
           " witnesses the deadlock it was written for. Do not delete it —"
           " give it a payload that does.";
    json parsed;
    EXPECT_NO_THROW(parsed = json::parse(r.output))
        << "captured output is not whole JSON — a truncated capture is the"
           " other half of the same defect";
    EXPECT_TRUE(parsed.contains("legs"));
}

// ── 2 + 3 + 4. The requirement, as an executable property ──────────────────
//
// RED-ON-DISABLE: make the leg list depend on the host in ANY way — filter
// `legs` by `runOn` containing the host, gate a leg on the host arch, restore
// the old `if [[ $HOST_OS == linux ]]` shape inside the resolver — and the
// build-set comparison fails on at least six of the nine hosts.
TEST_F(HarnessLegs, EveryDeclaredLegIsBuiltOnEveryHost) {
    // The declared truth, read straight from the catalogue.
    auto const doc = json::parse(fileText(catalogue_));
    std::vector<std::string> declared;
    for (auto const& leg : doc.at("legs")) {
        declared.push_back(leg.at("label").get<std::string>());
    }
    ASSERT_GE(declared.size(), 2u)
        << "a catalogue with fewer than two legs cannot witness"
           " host-invariance of the leg set";

    std::set<std::string> vocabulary;
    for (ArmVerdict const v : kAllArmVerdicts) {
        vocabulary.emplace(armVerdictName(v));
    }

    for (auto const& host : kSimHosts) {
        // `--launchers-none` pins the plan: no PATH lookup, so the result is
        // identical on every machine that runs this test.
        auto const r = planShape({"--plan", "--host-os", host.os, "--host-arch",
                            host.arch, "--launchers-none", "--format", "json"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const plan = json::parse(r.output);

        std::vector<std::string> got;
        for (auto const& leg : plan.at("legs")) {
            got.push_back(leg.at("label").get<std::string>());
        }
        EXPECT_EQ(got, declared)
            << "host " << host.os << '/' << host.arch
            << " plans a DIFFERENT set of legs than the catalogue declares."
               " The leg set is a property of the harness, not of the machine"
               " — build ANY target inside ANY host, item 2.";

        for (auto const& leg : plan.at("legs")) {
            auto const label = leg.at("label").get<std::string>();
            EXPECT_TRUE(leg.at("build").at("attempt").get<bool>())
                << host.os << '/' << host.arch << ' ' << label
                << ": the BUILD of a declared leg is unconditional. Whether a"
                   " host can EXECUTE the artifact is a separate question and"
                   " must not suppress the compile.";

            auto const& runPlan = leg.at("run");
            auto const  mode    = runPlan.at("mode").get<std::string>();
            EXPECT_TRUE(mode == "native" || mode == "launched" || mode == "skip")
                << host.os << '/' << host.arch << ' ' << label
                << ": unknown run mode '" << mode << '\'';
            EXPECT_FALSE(runPlan.at("detail").get<std::string>().empty())
                << host.os << '/' << host.arch << ' ' << label
                << ": every outcome must carry a REASON. A skip with no reason"
                   " is the no-verdict defect this whole ledger exists to end.";

            if (mode == "skip") {
                ASSERT_FALSE(runPlan.at("verdict").is_null())
                    << host.os << '/' << host.arch << ' ' << label
                    << ": a skip must be NAMED";
                auto const verdict = runPlan.at("verdict").get<std::string>();
                EXPECT_TRUE(vocabulary.count(verdict) == 1)
                    << host.os << '/' << host.arch << ' ' << label
                    << ": verdict '" << verdict
                    << "' is not in the ledger's closed vocabulary";
            } else {
                EXPECT_TRUE(runPlan.at("verdict").is_null())
                    << host.os << '/' << host.arch << ' ' << label
                    << ": a leg that WILL run carries no verdict yet — `ran` /"
                       " `poisoned` is the driver's to record once it has.";
            }
        }
    }
}

// A host that can execute nothing must still build everything, and must say
// exactly why each leg will not run. This is the cell the old drivers got
// wrong in both directions (the .ps1 refused to start at all; the .sh silently
// produced a shorter leg list).
TEST_F(HarnessLegs, AHostThatCanRunNothingStillBuildsEverythingAndSaysWhy) {
    auto const r = planShape({"--plan", "--host-os", "unknown", "--host-arch",
                        "unknown", "--launchers-none", "--format", "json"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    auto const plan = json::parse(r.output);
    ASSERT_FALSE(plan.at("legs").empty());
    for (auto const& leg : plan.at("legs")) {
        EXPECT_TRUE(leg.at("build").at("attempt").get<bool>());
        EXPECT_EQ(leg.at("run").at("mode").get<std::string>(), "skip");
        EXPECT_EQ(leg.at("run").at("verdict").get<std::string>(),
                  armVerdictName(ArmVerdict::SkippedByRunOn));
    }
}

// ── What the Python rule calls code, proved on a synthetic file ────────────
//
// Every driver pin below reads `liveLines(…, Dialect::Python)` and the
// inspector's statements, so the rule itself is pinned here, on a file this test
// writes, with each NEEDLE_ in exactly one shape. A fixture must synthesize the
// NEGATIVE as well, so every prose shape has a CODE neighbour that a rule
// dropping too much would drop with it: an f-string or a bytes literal in the
// docstring position is never a docstring, a string one statement later is code,
// and a triple-quoted string that is ASSIGNED is code. The statements carry the
// two shapes a line rule gets wrong: a call split over lines is ONE statement,
// and a continuation line at column 0 does not end the block it sits in.
TEST_F(HarnessLegs, TheLiveLineRuleReadsPythonProseAsProseAndStringsAsCode) {
    auto const p = scratch_->path() / "live-lines-fixture.py";
    {
        std::ofstream out(p, std::ios::binary);
        out << "#!/usr/bin/env python3\n"                         // 1
               "\"\"\"NEEDLE_MODULE_DOC\"\"\"\n"                 // 2
               "import os  # NEEDLE_TRAILING_COMMENT\n"          // 3
               "# NEEDLE_LINE_COMMENT\n"                         // 4
               "\n"                                               // 5
               "class Thing:\n"                                  // 6
               "    \"\"\"NEEDLE_CLASS_DOC_FIRST_LINE\n"         // 7
               "    NEEDLE_CLASS_DOC_LAST_LINE\"\"\"\n"          // 8
               "    def method(self):\n"                         // 9
               "        '''NEEDLE_METHOD_DOC'''\n"               // 10
               "        return \"NEEDLE_CODE_STRING\"\n"         // 11
               "def fn():\n"                                     // 12
               "    r\"\"\"NEEDLE_RAW_DOC\"\"\"\n"               // 13
               "    x = 1\n"                                     // 14
               "    \"NEEDLE_NOT_FIRST_STATEMENT\"\n"            // 15
               "    return x\n"                                  // 16
               "async def afn():\n"                              // 17
               "    \"\"\"NEEDLE_ASYNC_DOC\"\"\"\n"              // 18
               "    return f\"NEEDLE_FSTRING_RETURNED {1}\"\n"   // 19
               "def gn():\n"                                     // 20
               "    f\"NEEDLE_FSTRING_FIRST {1}\"\n"             // 21
               "def bn():\n"                                     // 22
               "    b\"NEEDLE_BYTES_FIRST\"\n"                   // 23
               "def cn():\n"                                     // 24
               "    \"NEEDLE_CONCAT_A\" \"NEEDLE_CONCAT_B\"\n"   // 25
               "VALUE = \"\"\"NEEDLE_ASSIGNED_TRIPLE\"\"\"\n"    // 26
               "CALL = dict(a=1,\n"                              // 27
               "            b=\"NEEDLE_CONTINUATION\")\n"        // 28
               "def dn():\n"                                     // 29
               "    return (1,\n"                                // 30
               "2)\n";                                            // 31
    }
    auto const& in = pythonCode(p);
    auto const lineOf = [&in](std::string const& needle) -> std::size_t {
        for (auto const& [n, text] : in.live) {
            if (text.find(needle) != std::string::npos) return n;
        }
        return 0;
    };
    struct Want {
        char const* needle;
        std::size_t line;    // its RAW line when it is code; 0 when it is prose
        char const* shape;
    };
    Want const wants[] = {
        {"NEEDLE_MODULE_DOC", 0, "a module docstring"},
        {"NEEDLE_TRAILING_COMMENT", 0, "a comment trailing code"},
        {"NEEDLE_LINE_COMMENT", 0, "a comment line"},
        {"NEEDLE_CLASS_DOC_FIRST_LINE", 0, "a class docstring's first line"},
        {"NEEDLE_CLASS_DOC_LAST_LINE", 0, "a class docstring's last line"},
        {"NEEDLE_METHOD_DOC", 0, "a method docstring in single quotes"},
        {"NEEDLE_RAW_DOC", 0, "a raw-string docstring"},
        {"NEEDLE_ASYNC_DOC", 0, "an async function's docstring"},
        {"NEEDLE_CONCAT_A", 0, "the first half of an implicitly concatenated docstring"},
        {"NEEDLE_CONCAT_B", 0, "the second half of an implicitly concatenated docstring"},
        {"NEEDLE_CODE_STRING", 11, "a returned string"},
        {"NEEDLE_NOT_FIRST_STATEMENT", 15, "a string statement that is NOT first"},
        {"NEEDLE_FSTRING_RETURNED", 19, "a returned f-string"},
        {"NEEDLE_FSTRING_FIRST", 21, "an f-string in the docstring position"},
        {"NEEDLE_BYTES_FIRST", 23, "a bytes literal in the docstring position"},
        {"NEEDLE_ASSIGNED_TRIPLE", 26, "an ASSIGNED triple-quoted string"},
        {"NEEDLE_CONTINUATION", 28, "a keyword argument on a continuation line"},
    };
    for (auto const& w : wants) {
        EXPECT_EQ(lineOf(w.needle), w.line)
            << w.needle << " sits in " << w.shape << ", which is "
            << (w.line == 0 ? "PROSE and must be dropped" : "CODE and must be kept")
            << (w.line == 0 ? "" : " on its raw line") << ".";
    }
    EXPECT_EQ(lineOf("#!/usr/bin/env"), 0u) << "the shebang is a comment";
    std::string importLine;
    for (auto const& [n, text] : in.live) {
        if (n == 3) importLine = text;
    }
    EXPECT_EQ(importLine, "import os")
        << "a trailing comment is removed from the line and the CODE kept, on the"
           " line's own number";
    EXPECT_EQ(liveLines(p, Dialect::Python).size(), in.live.size());

    // THE STATEMENTS. `def fn` holds three (its docstring is prose, so it is no
    // statement at all); `def dn` holds ONE, although its second physical line
    // sits at column 0; the call on lines 27-28 is one statement.
    auto const fn = statementStarting(in.statements, "def fn(");
    ASSERT_LT(fn, in.statements.size());
    EXPECT_EQ(blockOf(in.statements, fn).size(), 3u);
    auto const dn = statementStarting(in.statements, "def dn(");
    ASSERT_LT(dn, in.statements.size());
    auto const dnBody = blockOf(in.statements, dn);
    ASSERT_EQ(dnBody.size(), 1u)
        << "a continuation line at column 0 ended the block it belongs to";
    EXPECT_NE(dnBody[0].text.find("2)"), std::string::npos) << dnBody[0].text;
    auto const call = statementStarting(in.statements, "CALL = dict(");
    ASSERT_LT(call, in.statements.size());
    EXPECT_EQ(in.statements[call].first, 27u);
    EXPECT_EQ(in.statements[call].last, 28u);
    auto const& callText = in.statements[call].text;
    auto const  args     = callArguments(callText, callText.find('('));
    ASSERT_EQ(args.size(), 2u) << callText;
    EXPECT_EQ(args[1], "b=\"NEEDLE_CONTINUATION\"");
}

// ── 5. The driver actually consults it ─────────────────────────────────────
//
// A resolver nothing reads would be theatre. These are structural pins on the
// shipped driver: it must invoke the resolver, and it must not carry the shapes
// that host-locked the twins it replaced.
//
// ⓘ THE TEST NAMES BELOW PREDATE THE SINGLE DRIVER (`BothDrivers…`,
// `NeitherDriver…`: two twins until 2026-09-21). They are kept, so every record
// that cites a pin by name still finds it; each pin now states which FILES of the
// one Python driver carry its property.
//
// RED-ON-DISABLE: put a host-keyed refusal (`if platform.system() != "Windows":
// C.die(...)`) into any driver module and `TheDriversAreNotHostKeyed` fails;
// delete the resolver or the catalogue from `sqlite_common.py`, or the `--plan`
// request from `build_and_test.py`, and `TheDriversConsultTheResolver` fails.
TEST_F(HarnessLegs, TheDriversConsultTheResolver) {
    // ONE driver since 2026-09-21, in modules: the resolver and its catalogue are
    // named ONCE, in `sqlite_common.py` (the `Resolver` every step asks), and the
    // leg set is the answer `build_and_test.py` gets to `--plan`. Over LIVE code,
    // which is stricter than the twins' raw-text check: a module that only
    // DOCUMENTS the resolver no longer satisfies it.
    auto const common = harnessDir() / "sqlite_common.py";
    auto const entry  = harnessDir() / "build_and_test.py";
    for (auto const& p : {common, entry}) ASSERT_TRUE(fs::exists(p)) << p;
    auto const commonLines = liveLines(common, Dialect::Python);
    auto const namesInCode = [&commonLines](std::string const& file) {
        return std::any_of(commonLines.begin(), commonLines.end(),
                           [&file](std::string const& l) {
                               return l.find('"' + file + '"') != std::string::npos;
                           });
    };
    EXPECT_TRUE(namesInCode("harness_legs.py"))
        << "sqlite_common.py does not name harness_legs.py in code. The driver's leg"
           " set is therefore decided somewhere else — which is how both retired"
           " drivers came to key on the host in the first place.";
    EXPECT_TRUE(namesInCode("legs.json"))
        << "sqlite_common.py never names the leg catalogue in code.";
    auto const entryLines = liveLines(entry, Dialect::Python);
    EXPECT_TRUE(std::any_of(entryLines.begin(), entryLines.end(), [](std::string const& l) {
        return l.find("resolver.") != std::string::npos
            && l.find("\"--plan\"") != std::string::npos;
    })) << "build_and_test.py never asks the resolver for `--plan`, so the legs it"
           " builds are not the ones legs.json declares.";
}

TEST_F(HarnessLegs, TheDriversAreNotHostKeyed) {
    // Three rules, each naming a shape the twins REMOVED, each evaluated over LIVE
    // code only (see `liveLines` — the first version of this test searched raw
    // text and went red on the driver's own explanation of the gate it had
    // deleted), over EVERY module of the driver (`driverModules`).
    //
    // What these rules deliberately do NOT forbid: a host automatic in the ONE
    // canonical host-identification function (`sqlite_common.host_os`/`host_arch`,
    // which read `platform.system()` into a local and refuse only an OS they
    // cannot NAME), a fatal on failing to IDENTIFY the host at all, and a fatal
    // on a missing host TOOLCHAIN (a program not on PATH). All three are the
    // legitimate host question — "what machine am I, and what can it do" — and a
    // pin that outlawed them would be pushing the driver toward guessing.
    auto const modules = driverModules();
    auto const missing = missingDriverModules(modules);
    ASSERT_TRUE(missing.empty())
        << "the driver's module enumeration under " << harnessDir().string()
        << " is missing: " << missing
        << ". The set-wide rules below would then govern fewer files than the driver"
           " has and still report a pass. Fix the walk.";
    for (auto const& module : modules) {
        auto const  name = module.filename().string();
        auto const& code = pythonCode(module);
        ASSERT_FALSE(code.live.empty()) << name << " has no live lines";
        for (auto const& [n, line] : code.live) {
            // RULE 1 — a leg's target spec comes from the catalogue, never from
            // a literal in the driver. This is what the .ps1's `$Spec =
            // 'x86_64:pe64-x86_64-windows-exec'` was: a sixth leg, invisible to
            // the catalogue's host-invariance property and to the verdict
            // accounting.
            EXPECT_FALSE(mentionsTargetSpecLiteral(line))
                << name << ':' << n << " writes a target spec as a literal:\n  " << line
                << "\nEvery leg this harness builds must come from legs.json"
                   " through harness_legs.py, or it escapes both the"
                   " host-invariance property and the ledger.";

            // RULE 2 — the .sh's host-keyed leg CONSTRUCTOR is gone. `add_leg`
            // was called once unconditionally and once under
            // `if [[ $HOST_OS == linux && $HOST_ARCH == x86_64 ]]`, which is
            // precisely how pe64 and mach-o became unreachable from that
            // driver. The Python driver builds `Leg` records from the plan only.
            EXPECT_EQ(line.find("add_leg"), std::string::npos)
                << name << ':' << n << " still constructs legs itself:\n  " << line
                << "\nThe leg set is read, not built.";
        }
        // RULE 3 — no host automatic may guard a REFUSAL. This is the .ps1's
        // `if (-not $IsWindows) { Die ... }` exactly, spelled in Python: an `if`
        // (or conditional expression) testing `sys.platform`, `os.name` or
        // `platform.system()` whose arm raises, dies or exits, or an `assert` on
        // one — the driver declining to run because of the machine it is on.
        // The inspector reads it from the syntax tree, because in Python the
        // guard and its refusal are rarely on one line (the twins' rule was a
        // same-line rule, which a Python `die(` message naming `platform.system()`
        // in its TEXT would have tripped).
        for (auto const& g : code.hostGuards) {
            ADD_FAILURE() << name << ':' << g.line << " refuses to run based on the host ("
                          << g.why << "):\n  " << liveTextAt(code, g.line)
                          << "\nThe host decides what can be EXECUTED, never what can be"
                             " BUILT — and never whether this driver runs. Where the"
                             " POSIX half runs is decided ONCE (sqlite_common.PosixSide);"
                             " a refusal belongs behind a capability, not the OS name.";
        }
    }
}

// ── 6. Launcher vocabulary agreement with the examples corpus ──────────────
//
// `lintDeclaredEmulators` already enforces "one (arch, runOn-OS) pair, one
// emulator spelling" WITHIN the examples corpus. The sqlite catalogue is a
// second corpus of declarations about the same machines, so a divergence
// ("qemu-aarch64" here, "qemu-aarch64-static" there) would mean the project
// believes two different things about the same host. Only the pairs BOTH
// corpora declare are compared: the catalogue deliberately declares launchers
// the corpus does not (qemu-x86_64 for an arm64 Linux host, Wine for pe on
// Linux), and requiring the corpus to match those would be a different — and
// much larger — change than this one.
TEST_F(HarnessLegs, LauncherSpellingsAgreeWithTheExamplesCorpus) {
    auto const examples = repoRoot() / "examples" / "c";
    ASSERT_TRUE(fs::exists(examples)) << examples;

    // (targetArch, runOn-OS) -> emulator spelling, from the corpus.
    std::map<std::pair<std::string, std::string>, std::set<std::string>> corpus;
    for (auto const& entry : fs::directory_iterator(examples)) {
        auto const manifest = entry.path() / "expected.json";
        if (!fs::exists(manifest)) continue;
        json doc;
        try {
            doc = json::parse(fileText(manifest));
        } catch (std::exception const&) {
            continue;  // the corpus runners own manifest validity
        }
        if (!doc.contains("targets")) continue;
        for (auto const& t : doc.at("targets")) {
            if (!t.contains("spec") || !t.contains("runOn")) continue;
            auto const emulator = t.value("emulator", std::string{});
            if (emulator.empty()) continue;
            auto const arch = dss::test_support::specTargetArch(
                t.at("spec").get<std::string>());
            for (auto const& osName : t.at("runOn")) {
                corpus[{arch, osName.get<std::string>()}].insert(emulator);
            }
        }
    }
    ASSERT_FALSE(corpus.empty())
        << "no example manifest declares an emulator — this comparison would"
           " be vacuously green";

    auto const doc = json::parse(fileText(catalogue_));
    std::size_t compared = 0;
    for (auto const& leg : doc.at("legs")) {
        auto const arch = dss::test_support::specTargetArch(
            leg.at("spec").get<std::string>());
        for (auto const& entry : leg.at("launchers")) {
            auto const hostOs = entry.at("hostOs").get<std::string>();
            auto const cmd    = entry.at("command");
            ASSERT_FALSE(cmd.empty());
            auto const head = cmd.at(0).get<std::string>();
            auto const it   = corpus.find({arch, hostOs});
            if (it == corpus.end()) continue;
            ++compared;
            EXPECT_EQ(it->second.count(head), 1u)
                << "the sqlite catalogue launches (arch=" << arch
                << ", host OS=" << hostOs << ") with '" << head
                << "' while the examples corpus declares '"
                << *it->second.begin()
                << "' for the same pair — one pair, one vocabulary";
        }
    }
    EXPECT_GT(compared, 0u)
        << "no (arch, host OS) pair is declared by BOTH the sqlite catalogue and"
           " the examples corpus, so this test compared nothing. That is a"
           " silent-vacuity failure, not a pass.";
}

// ── 7. Every leg's staged zlib header is configured for ITS OWN target ─────
//
// THE STAGED zconf.h WAS PE-SHAPED. Three tests, one per link in the
// chain: the DECLARATION (does the catalogue say the right thing?), the PLAN (is
// the recipeTransform -> zinc/ mapping host-free and one-stage-per-transform?),
// and the ARTEFACT (does stage-zinc.py actually write it?).

// THE DECLARATION. `Z_HAVE_UNISTD_H` governs `#include <unistd.h>`, which exists
// on POSIX and not on Windows — so its correct value is DERIVABLE from the leg's
// own target and can be checked rather than trusted.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): give the pe64 leg the
// POSIX answer — or give the four POSIX legs the pe answer, which is exactly the
// shared pe-shaped stage this anchor is about — and this fails naming the leg,
// its target OS and both values.
TEST_F(HarnessLegs, EveryLegsZlibHeaderIsConfiguredForItsOwnTarget) {
    auto const doc = json::parse(fileText(catalogue_));
    std::size_t checked = 0;
    std::set<std::string> osSeen;
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        auto const spec  = leg.at("spec").get<std::string>();
        auto const os    = specTargetOs(spec);
        ASSERT_FALSE(os.empty())
            << label << ": cannot derive a target OS from spec '" << spec << '\'';
        osSeen.insert(os);
        ASSERT_TRUE(leg.at("build").contains("zconfGuards"))
            << label
            << ": declares no build.zconfGuards. Without it the driver has"
               " nothing to stage this leg's zlib header FROM, and the only"
               " remaining option is the copy some other leg's target wanted —"
               " which is the defect.";
        auto const& guards = leg.at("build").at("zconfGuards");
        ASSERT_TRUE(guards.contains("Z_HAVE_UNISTD_H")) << label;
        ASSERT_TRUE(guards.at("Z_HAVE_UNISTD_H").is_boolean())
            << label << ": Z_HAVE_UNISTD_H must be a JSON boolean — a string is"
                        " truthy in bash, PowerShell and python alike";
        bool const posix = (os == "linux" || os == "darwin");
        EXPECT_EQ(guards.at("Z_HAVE_UNISTD_H").get<bool>(), posix)
            << label << " targets OS '" << os
            << "' but declares Z_HAVE_UNISTD_H="
            << guards.at("Z_HAVE_UNISTD_H").get<bool>()
            << ". That guard decides whether the staged zconf.h does"
               " `#include <unistd.h>` and therefore whether z_off_t is off_t or"
               " long. MEASURED TF-C115: on darwin those are `long long` vs"
               " `long` — same width, DIFFERENT type — and on pe the POSIX answer"
               " does not compile at all (error[F001D] got unistd.h).";
        ++checked;
    }
    EXPECT_GE(checked, 2u);
    EXPECT_GE(osSeen.size(), 2u)
        << "every leg targets the same OS, so this test cannot witness a"
           " per-target difference — it would pass on a single shared header";
}

// THE PLAN. One stage per recipeTransform, the same on every host, and each
// leg's key is its own transform.
//
// RED-ON-DISABLE: make `headerStageKey` depend on anything but the leg's
// declared transform (a host, a label) and the cross-host comparison fails; give
// two legs the same transform but different guards and `--header-stages` exits
// non-zero with the conflict named.
TEST_F(HarnessLegs, TheHeaderStagePlanIsPerTransformAndHostFree) {
    auto const stagesRun = run({"--header-stages"});
    ASSERT_TRUE(stagesRun.spawned) << stagesRun.diagnostic;
    ASSERT_EQ(stagesRun.exitCode, 0u) << stagesRun.output;
    std::map<std::string, std::string> stages;  // key -> guard string
    for (auto const& line : splitLines(stagesRun.output)) {
        auto const tab = line.find('\t');
        ASSERT_NE(tab, std::string::npos) << "malformed stage line: " << line;
        stages[line.substr(0, tab)] = line.substr(tab + 1);
    }
    ASSERT_GE(stages.size(), 2u)
        << "the catalogue declares fewer than two header stages, so per-target"
           " staging is untested by the catalogue it ships with:\n"
        << stagesRun.output;

    auto const doc = json::parse(fileText(catalogue_));
    std::set<std::string> transforms;
    for (auto const& leg : doc.at("legs")) {
        transforms.insert(
            leg.at("build").at("recipeTransform").get<std::string>());
    }
    std::set<std::string> stageKeys;
    for (auto const& [k, _] : stages) stageKeys.insert(k);
    EXPECT_EQ(stageKeys, transforms)
        << "the stage set must be exactly the distinct recipeTransforms — one"
           " zinc/ per transform, no more (a stage nothing uses) and no fewer"
           " (two targets sharing one header).";

    // Same answer on every host, including three this project has never run on.
    std::map<std::string, std::string> firstHostKeys;
    for (auto const& host : kSimHosts) {
        auto const r = planShape({"--plan", "--host-os", host.os, "--host-arch",
                            host.arch, "--launchers-none", "--format", "json"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const plan = json::parse(r.output);
        std::map<std::string, std::string> keys;
        for (auto const& leg : plan.at("legs")) {
            auto const label = leg.at("label").get<std::string>();
            auto const& build = leg.at("build");
            ASSERT_TRUE(build.contains("headerStageKey")) << label;
            auto const key = build.at("headerStageKey").get<std::string>();
            EXPECT_EQ(key, build.at("recipeTransform").get<std::string>())
                << host.os << '/' << host.arch << ' ' << label
                << ": the staged-header key must BE the declared"
                   " recipeTransform. Anything else is a second mapping a"
                   " reader would have to find.";
            EXPECT_EQ(stages.count(key), 1u)
                << host.os << '/' << host.arch << ' ' << label
                << ": names stage '" << key << "', which is not in the plan";
            keys[label] = key;
        }
        if (firstHostKeys.empty()) {
            firstHostKeys = keys;
        } else {
            EXPECT_EQ(keys, firstHostKeys)
                << "host " << host.os << '/' << host.arch
                << " assigns DIFFERENT staged headers than the first host."
                   " Which zlib header a leg compiles against is a fact about"
                   " its TARGET; a host that changes it has re-locked the"
                   " harness one level down from the leg set.";
        }
    }
}

// THE ARTEFACT. Drive the REAL staging tool with the SHIPPED catalogue over a
// zconf.h fixture, then read every leg's include dir back off disk and confirm
// the guard it got is the one its own target wanted.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): revert to one shared
// zinc/ — i.e. point every leg at a single staged directory, however that
// directory was configured — and the legs on the other side of the guard fail
// here with the value they actually got.
TEST_F(HarnessLegs, StageZincWritesOneHeaderPerTargetAndEveryLegGetsItsOwn) {
    dss::test_support::ScratchDir work{dss::test_support::Location::Temp,
                                       "sqlite-harness-zinc"};
    auto const zconfSrc = work.path() / "zconf.h";
    auto const zlibSrc  = work.path() / "zlib.h";
    {
        std::ofstream out(zconfSrc, std::ios::binary);
        out << kZconfFixture;
        std::ofstream z(zlibSrc, std::ios::binary);
        z << "#include \"zconf.h\"\n";
    }
    auto const dest = work.path() / "zinc";

    auto const py = pythonPath();
    ASSERT_FALSE(py.empty())
        << "python3 is a hard dependency of the driver (it IS Python); a skip here"
           " would be the no-verdict defect this file exists to end.";
    std::vector<std::string> argv{
        py,
        (harnessDir() / "stage-zinc.py").string(),
        "--zlib-h",  zlibSrc.string(),
        "--zconf-h", zconfSrc.string(),
        "--dest",    dest.string(),
        // `runBinary` APPENDS its `binaryPath` as the final argv element (see
        // `runResolver`), so the catalogue's VALUE is supplied by that
        // append — and it is the scratch copy, which is also what keeps this
        // from chmod'ing a tracked repo file on POSIX.
        "--catalogue"};
    auto const res = dss::test_support::runBinary(
        catalogue_, dss::test_support::kHelperScriptBudget,
        /*captureStdout=*/true, argv);
    ASSERT_TRUE(res.spawned && !res.timedOut) << res.diagnostic;
    ASSERT_EQ(res.exitCode, 0u)
        << "stage-zinc.py could not produce every declared stage:\n"
        << res.capturedStdout;
    EXPECT_NE(res.capturedStdout.find("ZINC-STAGES="), std::string::npos)
        << res.capturedStdout;

    auto const doc = json::parse(fileText(catalogue_));
    std::map<std::string, int> observed;  // stage key -> Z_HAVE_UNISTD_H state
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        auto const key =
            leg.at("build").at("recipeTransform").get<std::string>();
        auto const zconf = dest / key / "zconf.h";
        ASSERT_TRUE(fs::exists(zconf))
            << label << ": no staged zlib header at " << zconf
            << ". A leg without its own zinc/ has nowhere to go but another"
               " target's copy, and stage-zinc.py refuses that on purpose —"
               " so the driver poisons the leg instead.";
        bool const want =
            leg.at("build").at("zconfGuards").at("Z_HAVE_UNISTD_H").get<bool>();
        int const got = guardStateIn(zconf, "Z_HAVE_UNISTD_H");
        EXPECT_EQ(got, want ? 1 : 0)
            << label << " (target OS "
            << specTargetOs(leg.at("spec").get<std::string>())
            << ") compiles against " << zconf
            << ", whose Z_HAVE_UNISTD_H reads " << got << " — it declared "
            << want
            << ". A leg parsing a zlib header configured for a different target"
               " is the pe-shaped staged zconf.h defect exactly.";
        // Z_HAVE_STDARG_H is the CONTROL: it is declared true on every leg, so
        // if the tool were rewriting whole blocks rather than the one declared
        // guard, this would move too.
        EXPECT_EQ(guardStateIn(zconf, "Z_HAVE_STDARG_H"), 1)
            << label << ": the sibling guard must be untouched at #if 1";
        observed[key] = got;
    }
    ASSERT_GE(observed.size(), 2u);
    std::set<int> distinct;
    for (auto const& [k, v] : observed) distinct.insert(v);
    EXPECT_EQ(distinct.size(), 2u)
        << "every staged header came out with the SAME Z_HAVE_UNISTD_H, so this"
           " test would pass against the single shared zinc/ it was written to"
           " outlaw. That is a silent-vacuity failure, not a pass.";
}

// THE DRIVER. It must use the shared tool, and no module of it may carry the
// shapes that made one zinc/ serve every leg.
//
// "Stages one zinc per transform" is carried by TWO files of the Python driver:
// `sqlite_common.py` names the tool ONCE (`STAGE_ZINC`) and `sqlite_build.py`
// (`stage_headers`, Step 6b) spawns it. Both are read as LIVE code — the twins'
// check read raw text, which a comment naming the tool satisfies.
//
// RED-ON-DISABLE: put the `perl -0777 … Z_HAVE_UNISTD_H` flip back into any
// driver module, or hand every leg one include list, and this fails naming the
// line.
TEST_F(HarnessLegs, BothDriversStageOneZincPerTransform) {
    auto const common = harnessDir() / "sqlite_common.py";
    auto const build  = harnessDir() / "sqlite_build.py";
    for (auto const& p : {common, build}) ASSERT_TRUE(fs::exists(p)) << p;
    auto const commonLines = liveLines(common, Dialect::Python);
    EXPECT_TRUE(std::any_of(commonLines.begin(), commonLines.end(), [](std::string const& l) {
        return l.find("\"stage-zinc.py\"") != std::string::npos;
    })) << "sqlite_common.py does not name stage-zinc.py in code. Per-target header"
           " staging then lives somewhere else in the driver — which is how the .sh"
           " came to build its pe64 leg against a POSIX zconf.h.";
    auto const buildLines = liveLines(build, Dialect::Python);
    EXPECT_TRUE(std::any_of(buildLines.begin(), buildLines.end(), [](std::string const& l) {
        return l.find("STAGE_ZINC") != std::string::npos
            && l.find("python_argv(") != std::string::npos;
    })) << "sqlite_build.py never SPAWNS stage-zinc.py (no live line runs STAGE_ZINC"
           " through python_argv). A tool the driver names and never runs stages"
           " nothing.";

    // The shared-list spellings: the .sh named `recipe-includes.txt`, the .ps1
    // ended a literal in `includes.txt'`. A Python string naming ONE list ends
    // `includes.txt"` or `includes.txt'`; the per-stage lists are
    // `recipe-includes.%s.%s.txt` and `cli-includes.%s.%s.txt`, one per
    // (zinc stage, config stage) pair.
    auto const modules = driverModules();
    auto const missing = missingDriverModules(modules);
    ASSERT_TRUE(missing.empty()) << "driver modules missing: " << missing;
    for (auto const& module : modules) {
        auto const  name = module.filename().string();
        auto const& code = pythonCode(module);
        for (auto const& [n, line] : code.live) {
            // The macro names belong to the CATALOGUE and to stage-zinc.py. A
            // driver that spells one is deciding a target's header itself.
            EXPECT_EQ(line.find("Z_HAVE_UNISTD_H"), std::string::npos)
                << name << ':' << n << " names a zconf guard in live code:\n  " << line
                << "\nGuards are declared per leg in legs.json and applied by"
                   " stage-zinc.py; a driver that edits one has re-created the"
                   " single pe-shaped stage.";
            EXPECT_EQ(line.find("perl -0777"), std::string::npos)
                << name << ':' << n << " patches a staged header in place:\n  " << line;
            for (char const* shared : {"recipe-includes.txt", "includes.txt\"", "includes.txt'"}) {
                EXPECT_EQ(line.find(shared), std::string::npos)
                    << name << ':' << n << " still names ONE shared include list:\n  "
                    << line
                    << "\nThe list's last entry is the staged zlib dir, which is"
                       " per target — so there is one list per header stage.";
            }
        }
    }
}

// ── 8. A launcher's PATH NAMESPACE is declared, closed, and enforced ───────
//
// THERE WAS NO WSL LAUNCHER FOR ELF ON WINDOWS. The catalogue declared Wine for
// pe-on-Linux and NOTHING for elf-on-Windows, so a Windows host recorded
// `skipped-by-runOn` for a leg whose testfixture it had just built — and that
// same artefact, run under WSL BY HAND, passed 330,436 tests.
//
// WHY IT WAS NOT A ONE-LINE CONFIG ADDITION, which is what these tests are
// really about: a launcher does not always share a filesystem NAMESPACE with the
// driver that spawns it. `wine /home/me/x.exe` takes the driver's own path;
// `wsl.exe` needs `/mnt/c/...` where the driver holds `C:\...`. An untranslated
// path is NOT reported as a bad path — the callee opens a RELATIVE file of that
// name, misses, and the run reads as a broken binary. So every launcher declares
// `pathTranslation`, the resolver owns the closed vocabulary AND performs the
// translation (`--translate-path`), and `--assert-translated` is the net under
// the driver.
//
// ★ WHAT IS NOT TESTED HERE AND WHY: none of these tests invokes `wslpath`.
// This file runs on Windows, on WSL and on an arm64 Linux VPS, and a test whose
// green depended on `wsl.exe` would be a skip-or-red on two of the three. The
// translator CONTRACT (the path handed over VERBATIM, rc, empty output, an
// output still in the source namespace, a source path in the wrong namespace)
// is exercised by `harness_legs.py --self-test` with an INJECTED translator —
// which this file runs, as `TheResolverSelfTestPasses`. The live `wslpath` path
// is measured by the driver run recorded in the cycle report.
// ⚠ "VERBATIM" replaced "separator normalisation before the call" on
// 2026-08-04: that normalisation was a workaround for `wslpath: C:ab`, a symptom
// whose cause was misattributed to wslpath eating backslashes when it was
// actually the local shell `wsl.exe` runs without `-e` (section 9 below).

// The vocabulary is CLOSED and the catalogue may not step outside it. Asserted
// from both directions: the resolver prints the set, and every verb any launcher
// declares is in it.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): give a launcher a verb
// the resolver does not implement and this fails naming the leg and the verb.
TEST_F(HarnessLegs, EveryLauncherDeclaresAVerbFromTheClosedNamespaceVocabulary) {
    auto const r = run({"--path-translations"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;

    std::map<std::string, std::string> known;  // verb -> translator argv
    for (auto const& line : splitLines(r.output)) {
        auto const tab = line.find('\t');
        ASSERT_NE(tab, std::string::npos) << "malformed line: " << line;
        known[line.substr(0, tab)] = line.substr(tab + 1);
    }
    ASSERT_EQ(known.count("none"), 1u)
        << "`none` must always exist: it is how a launcher that takes this"
           " driver's paths verbatim SAYS SO, and without it the absence of a"
           " key would have to mean it.";
    EXPECT_TRUE(known.at("none").empty())
        << "`none` names a translator ('" << known.at("none")
        << "') — it is the identity by definition.";
    ASSERT_GE(known.size(), 2u)
        << "only one verb is declared, so 'closed vocabulary' is untested by the"
           " resolver it ships with";

    auto const  doc      = json::parse(fileText(catalogue_));
    std::size_t declared = 0;
    std::set<std::string> verbsSeen;
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        for (auto const& entry : leg.at("launchers")) {
            ASSERT_TRUE(entry.contains("pathTranslation"))
                << label << ": a launcher for (" << entry.at("hostOs")
                << ", " << entry.at("hostArch")
                << ") declares no pathTranslation. Every launcher states the"
                   " PATH NAMESPACE its argv lives in — a default would make"
                   " 'nobody thought about it' indistinguishable from 'it takes"
                   " our paths verbatim'.";
            auto const verb = entry.at("pathTranslation").get<std::string>();
            EXPECT_EQ(known.count(verb), 1u)
                << label << ": launcher for (" << entry.at("hostOs") << ", "
                << entry.at("hostArch") << ") declares pathTranslation '" << verb
                << "', which the resolver does not implement";
            verbsSeen.insert(verb);
            ++declared;
        }
    }
    EXPECT_GE(declared, 2u) << "no launcher declared a namespace — vacuous";
    EXPECT_GE(verbsSeen.size(), 2u)
        << "every launcher in the catalogue declares the SAME namespace ("
        << *verbsSeen.begin()
        << "), so this catalogue cannot witness the distinction the key exists"
           " to make. That is a silent-vacuity failure, not a pass.";

    // ── the ENVIRONMENT namespace, the same three ways ────────────────────
    // A launcher in another OS namespace does not inherit the driver's
    // environment either, and that failure is quieter than the path one: the
    // child runs with an EMPTY run environment and simply does something else.
    auto const e = run({"--env-transfers"});
    ASSERT_TRUE(e.spawned) << e.diagnostic;
    ASSERT_EQ(e.exitCode, 0u) << e.output;
    std::map<std::string, std::string> envVerbs;  // verb -> carrier variable
    for (auto const& line : splitLines(e.output)) {
        auto const tab = line.find('\t');
        ASSERT_NE(tab, std::string::npos) << "malformed line: " << line;
        envVerbs[line.substr(0, tab)] = line.substr(tab + 1);
    }
    ASSERT_EQ(envVerbs.count("inherit"), 1u);
    EXPECT_TRUE(envVerbs.at("inherit").empty())
        << "`inherit` names a carrier variable ('" << envVerbs.at("inherit")
        << "') — inheriting is precisely needing none.";
    ASSERT_GE(envVerbs.size(), 2u)
        << "only one environment-transfer verb is declared, so the vocabulary is"
           " untested by the resolver it ships with";
    std::set<std::string> envSeen;
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        for (auto const& entry : leg.at("launchers")) {
            ASSERT_TRUE(entry.contains("envTransfer"))
                << label << ": a launcher for (" << entry.at("hostOs") << ", "
                << entry.at("hostArch")
                << ") declares no envTransfer. MEASURED 2026-08-04: a"
                   " wsl.exe-launched fixture saw SQLITE_TEST_PATTERN_LIST as"
                   " EMPTY, and the corpus resume engine — which selects its"
                   " files through that variable — re-ran the whole corpus"
                   " instead of the tail after the abort.";
            auto const verb = entry.at("envTransfer").get<std::string>();
            EXPECT_EQ(envVerbs.count(verb), 1u)
                << label << ": envTransfer '" << verb
                << "' is not implemented by the resolver";
            envSeen.insert(verb);
        }
    }
    EXPECT_GE(envSeen.size(), 2u)
        << "every launcher declares the SAME environment transfer ("
        << *envSeen.begin() << ") — vacuous, as above.";
}

// The lint REFUSES the three ways a declaration can be wrong. Each mutation is
// applied to a scratch copy of the SHIPPED catalogue, so what is refused is a
// realistic edit and not a straw fixture.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): delete any of the
// three lint rules in harness_legs.py and the matching case here goes green with
// `findings=0`.
TEST_F(HarnessLegs, AMalformedPathTranslationDeclarationFailsLint) {
    struct Case {
        char const* tag;
        char const* leg;
        char const* needle;  // must appear in the lint output
    };
    // (1) an unknown verb, (2) the key omitted entirely, (3) a verb declared on
    // a host whose namespace it does not describe.
    {
        MutatedCatalogue m{catalogue_, "legs-unknown-verb"};
        m.firstLauncherOf("pe64-x86_64")["pathTranslation"] = "windows-to-posix";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown pathTranslation verb LINTED CLEAN:\n"
            << r.output
            << "\nA verb nothing implements is a declaration that reads as"
               " configuration and is not — the launcher would be handed"
               " untranslated paths and the failure would look like a broken"
               " binary.";
        EXPECT_NE(r.output.find("windows-to-posix"), std::string::npos)
            << "the refusal must NAME the verb:\n" << r.output;
        EXPECT_NE(r.output.find("pe64-x86_64"), std::string::npos)
            << "the refusal must NAME the leg:\n" << r.output;
    }
    {
        MutatedCatalogue m{catalogue_, "legs-missing-verb"};
        m.firstLauncherOf("pe64-x86_64").erase("pathTranslation");
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a launcher with NO pathTranslation linted clean:\n"
            << r.output
            << "\nThe key is required on every entry for the same reason every"
               " zconf guard is: a reader must not have to know a default.";
        EXPECT_NE(r.output.find("pathTranslation"), std::string::npos)
            << r.output;
    }
    {
        // `windows-to-wsl` translates FROM a drive-letter path using a Windows
        // tool. On a Linux host there is neither. The lint can DERIVE that, so
        // it checks the declaration instead of trusting it.
        MutatedCatalogue m{catalogue_, "legs-verb-wrong-host"};
        m.firstLauncherOf("pe64-x86_64")["pathTranslation"] = "windows-to-wsl";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "pathTranslation 'windows-to-wsl' declared on a LINUX host"
               " launcher linted clean:\n"
            << r.output;
        EXPECT_NE(r.output.find("windows-to-wsl"), std::string::npos) << r.output;
    }
    // ── the same three, for the ENVIRONMENT namespace ──────────────────────
    {
        MutatedCatalogue m{catalogue_, "legs-unknown-envverb"};
        m.firstLauncherOf("pe64-x86_64")["envTransfer"] = "copy-the-block";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown envTransfer verb LINTED CLEAN:\n" << r.output;
        EXPECT_NE(r.output.find("copy-the-block"), std::string::npos) << r.output;
    }
    {
        MutatedCatalogue m{catalogue_, "legs-missing-envverb"};
        m.firstLauncherOf("pe64-x86_64").erase("envTransfer");
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a launcher with NO envTransfer linted clean:\n" << r.output;
        EXPECT_NE(r.output.find("envTransfer"), std::string::npos) << r.output;
    }
    {
        MutatedCatalogue m{catalogue_, "legs-envverb-wrong-host"};
        m.firstLauncherOf("pe64-x86_64")["envTransfer"] = "wslenv";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "envTransfer 'wslenv' declared on a LINUX host launcher linted"
               " clean:\n"
            << r.output;
    }
    // ── and the same, for the launcher's FILESYSTEM ────────────────────────
    // A WSL-LAUNCHED LEG'S RUNDIR IS DrvFs. The third namespace, and the
    // one whose absence cost 55 unit failures across 6 families plus a fixture
    // ABORT — every one of them non-DSS, all of them reported as if they were.
    {
        MutatedCatalogue m{catalogue_, "legs-unknown-fsverb"};
        m.firstLauncherOf("pe64-x86_64")["runFilesystem"] = "somewhere-else";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown runFilesystem verb LINTED CLEAN:\n" << r.output;
        EXPECT_NE(r.output.find("somewhere-else"), std::string::npos) << r.output;
    }
    {
        MutatedCatalogue m{catalogue_, "legs-missing-fsverb"};
        m.firstLauncherOf("pe64-x86_64").erase("runFilesystem");
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a launcher with NO runFilesystem linted clean:\n"
            << r.output
            << "\n`driver` is a CLAIM — that the launched process writes onto"
               " this driver's filesystem with this filesystem's semantics — and"
               " it was the unexamined one.";
        EXPECT_NE(r.output.find("runFilesystem"), std::string::npos) << r.output;
    }
    {
        // The pairing that actually bites: a launcher whose paths must be
        // RE-SPELLED to reach it is reaching this driver's files through a
        // compatibility mount, so `driver` is exactly the wrong answer there.
        // This is the shipped defect, restored.
        MutatedCatalogue m{catalogue_, "legs-drvfs-restored"};
        auto& wsl = m.firstLauncherOf("elf64-x86_64");
        // entry 0 is the qemu one; find the translating launcher by its verb.
        json* target = nullptr;
        for (auto& leg : m.doc().at("legs")) {
            if (leg.at("label").get<std::string>() != "elf64-x86_64") continue;
            for (auto& e : leg.at("launchers")) {
                if (e.at("pathTranslation").get<std::string>() != "none") {
                    target = &e;
                }
            }
        }
        ASSERT_NE(target, nullptr)
            << "no translating launcher to mutate — this test would assert"
               " nothing";
        (void)wsl;
        (*target)["runFilesystem"] = "driver";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a TRANSLATING launcher declaring runFilesystem 'driver' linted"
               " clean — that is the exact declaration that put a Linux sqlite"
               " corpus onto DrvFs:\n"
            << r.output;
        EXPECT_NE(r.output.find("runFilesystem"), std::string::npos) << r.output;
    }
}

// ── THE EARNED-CONFOUND LEDGER LIVES IN THE CATALOGUE ───────────────────────
//
// THE CONFOUND LEDGER WAS PER DRIVER, NOT PER LEG, and the confounds were not
// declared per leg at all — the two halves of
// D-SQLITE-CONFOUND-LIST-DRIVER-ASYMMETRY seen from the harness side.
//
// A confound asserts THE COMPILER IS INNOCENT of a failing test, and the count
// of genuine failures is what every verdict this harness renders rests on. So
// the declaration has to show its work, and the lint is what makes that
// non-optional. ✔MEASURED consequence of the old per-driver lists: the same
// elf64-x86_64 artefact's `zipfile-25.0` was a "known non-DSS confound" under
// one driver and a "genuine failure" under the other, on the same day.
//
// RED-ON-DISABLE: delete the matching rule from harness_legs.py's lint and the
// case below goes green with `findings=0`.
TEST_F(HarnessLegs, EveryLegDeclaresItsEarnedConfoundsWithProvenance) {
    auto const doc = json::parse(fileText(catalogue_));
    std::size_t rows = 0;
    std::set<std::string> distinct;
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        ASSERT_TRUE(leg.contains("confounds"))
            << label
            << " declares no `confounds`. The key is REQUIRED even when the"
               " answer is `[]`: a missing key cannot be told from an empty one,"
               " and the difference decides whether a failing test is reported"
               " as a compiler defect.";
        std::string set;
        for (auto const& row : leg.at("confounds")) {
            ++rows;
            // ⓘ `anchor` is OPTIONAL since 2026-09-18 (a pointer to the registry row
            // holding the long form; the harness registry most of them named was
            // retired) -- but a PRESENT one must not be empty, pinned just below.
            if (row.contains("anchor")) {
                EXPECT_FALSE(row.at("anchor").get<std::string>().empty())
                    << label << ": a confound row's `anchor` is present and EMPTY --"
                       " a pointer at nothing that reads as a citation.";
            }
            for (char const* k :
                 {"pattern", "earnedOn", "earnedAt", "mechanism"}) {
                ASSERT_TRUE(row.contains(k))
                    << label << ": a confound row omits '" << k << '\'';
                EXPECT_FALSE(row.at(k).get<std::string>().empty())
                    << label << ": a confound row's '" << k << "' is EMPTY."
                    << " An unearned confound is how a real defect becomes"
                       " furniture.";
            }
            // ★★ `requires` REPLACED `scope` AS THE REQUIRED CONDITION FIELD.
            // [a confound's condition is a RUN MODE, not a host.] `scope`
            // matched a pattern against the leg's RUN MODE, which is the wrong
            // axis for any row whose mechanism is a property of the MACHINE: the
            // three clock rows sat at `scope: any` and would have excused a
            // GENUINE walsetlk failure on the arm64 VPS, where the clock has
            // never been shown to step. `requires` names environment PROBES and
            // the row is honoured only where one MEASURES its defect; `[]` is the
            // unconditional claim, and it is required for the same reason
            // `confounds: []` is — a missing key cannot be told from an empty one.
            ASSERT_TRUE(row.contains("requires"))
                << label << ": confound '"
                << row.at("pattern").get<std::string>()
                << "' declares no `requires`. `[]` is the ordinary answer;"
                   " omitting it makes 'this excusal depends on nothing"
                   " measurable' indistinguishable from a row nobody finished.";
            EXPECT_TRUE(row.at("requires").is_array())
                << label << ": `requires` must be an array of probe names";
            for (auto const& nm : row.at("requires")) {
                EXPECT_TRUE(doc.contains("environmentProbes") &&
                            doc.at("environmentProbes")
                                .contains(nm.get<std::string>()))
                    << label << ": confound requires probe '"
                    << nm.get<std::string>()
                    << "', which `environmentProbes` does not declare. An"
                       " undeclared probe cannot be measured, so the row would be"
                       " honoured on nothing — the exact state `scope: any` was"
                       " in.";
            }
            // ⚠ `scope` IS LEGACY, NOT AN ALTERNATIVE. It survives only on rows
            // whose real mechanism has no probe yet, and each of those must NAME
            // its blocker — otherwise the axis becomes an inert alternative the
            // next row reaches for, which is how a proxy gets re-cut to fit each
            // new case — a pin weakened by its own subject.
            if (row.contains("scope")) {
                auto const scope = row.at("scope").get<std::string>();
                EXPECT_TRUE(scope == "native" || scope == "emulated")
                    << label << ": confound scope '" << scope
                    << "' — `any` is RETIRED (it is now `requires: []`) and"
                       " anything else was never a scope.";
                EXPECT_TRUE(row.contains("scopeLegacyBlocker") &&
                            !row.at("scopeLegacyBlocker")
                                 .get<std::string>()
                                 .empty())
                    << label << ": confound '"
                    << row.at("pattern").get<std::string>()
                    << "' stays on the LEGACY `scope` axis and names no"
                       " `scopeLegacyBlocker`.";
            }
            set += row.at("pattern").get<std::string>() + '|';
        }
        distinct.insert(set);
    }
    EXPECT_GT(rows, 0u) << "no leg declares any confound — this test would"
                           " assert nothing";
    // ★ THE ASYMMETRY IS THE POINT. If every leg carried the same rows this
    // would be the old GLOBAL list wearing a per-leg costume, and the defect
    // would be back with the paperwork done.
    EXPECT_GT(distinct.size(), 1u)
        << "every leg declares the SAME confound set, which is the global list"
           " again — a confound must be EARNED per platform, never copied from"
           " a sibling leg.";
}

TEST_F(HarnessLegs, AConfoundWithoutProvenanceFailsLint) {
    {
        // ★ THE POINTER IS OPTIONAL, NEVER EMPTY. A row with every required key
        // and an `anchor` of "" must red, naming the key; the SAME row with the
        // key deleted is the control and must lint clean of that complaint.
        MutatedCatalogue m{catalogue_, "legs-confound-empty-anchor"};
        auto& leg = m.legOf("pe64-x86_64");
        leg["confounds"].push_back(json{{"pattern", "^empty-pointer-"},
                                        {"requires", json::array()},
                                        {"earnedOn", "pe64-x86_64: a pin fixture"},
                                        {"earnedAt", "2026-09-18"},
                                        {"mechanism", "a pin fixture, not a row"},
                                        {"anchor", ""}});
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a confound with an EMPTY `anchor` LINTED CLEAN:\n" << r.output;
        EXPECT_NE(r.output.find("declares an EMPTY 'anchor'"), std::string::npos)
            << r.output;
        MutatedCatalogue c{catalogue_, "legs-confound-no-anchor"};
        auto& cleg = c.legOf("pe64-x86_64");
        cleg["confounds"].push_back(json{{"pattern", "^no-pointer-"},
                                         {"requires", json::array()},
                                         {"earnedOn", "pe64-x86_64: a pin fixture"},
                                         {"earnedAt", "2026-09-18"},
                                         {"mechanism", "a pin fixture, not a row"}});
        auto const rc = runResolver({"--lint"}, c.commit());
        ASSERT_TRUE(rc.spawned) << rc.diagnostic;
        EXPECT_EQ(rc.output.find("EMPTY 'anchor'"), std::string::npos)
            << "the CONTROL (no `anchor` key at all) drew the empty-pointer"
               " refusal:\n" << rc.output;
        EXPECT_EQ(rc.output.find("^no-pointer-"), std::string::npos)
            << "the CONTROL row, complete but for the optional pointer, drew a"
               " finding:\n" << rc.output;
    }
    {
        MutatedCatalogue m{catalogue_, "legs-confound-no-provenance"};
        auto& leg = m.legOf("pe64-x86_64");
        leg["confounds"] = json::array(
            {json{{"pattern", "^made-up-"}, {"requires", json::array()}}});
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a confound with NO provenance LINTED CLEAN:\n"
            << r.output
            << "\nA confound asserts the compiler is innocent of a failing"
               " test; it has to show its work.";
        EXPECT_NE(r.output.find("^made-up-"), std::string::npos) << r.output;
    }
    {
        // The key omitted entirely — "nobody filled this in" must not be
        // readable as "nothing was ever earned here".
        MutatedCatalogue m{catalogue_, "legs-confounds-missing"};
        m.legOf("elf64-x86_64").erase("confounds");
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a leg with NO `confounds` key linted clean:\n" << r.output;
        EXPECT_NE(r.output.find("confounds"), std::string::npos) << r.output;
    }
    {
        // A pattern that does not compile matches NOTHING, so every failure it
        // names is reported as a DSS defect — the loud half of the same lie.
        MutatedCatalogue m{catalogue_, "legs-confound-bad-regex"};
        auto& leg = m.legOf("pe64-x86_64");
        leg["confounds"] = json::array({json{{"pattern", "^broken["},
                                             {"requires", json::array()},
                                             {"earnedOn", "nowhere"},
                                             {"earnedAt", "never"},
                                             {"mechanism", "none"},
                                             {"anchor", "D-NONE"}}});
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a confound pattern that does not COMPILE linted clean:\n"
            << r.output;
    }
    {
        // A scope no host can satisfy is dead config that reads as coverage.
        MutatedCatalogue m{catalogue_, "legs-confound-dead-scope"};
        auto& leg = m.legOf("macho64-arm64");  // declares NO launcher at all
        leg["confounds"] = json::array(
            {json{{"pattern", "^never-fires-"},
                  {"requires", json::array()},
                  {"scope", "emulated"},
                  {"scopeLegacyBlocker", "a pin fixture, not a real blocker"},
                  {"earnedOn", "nowhere"},
                  {"earnedAt", "never"},
                  {"mechanism", "none"},
                  {"anchor", "D-NONE"}}});
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an `emulated`-scoped confound on a leg with NO launcher linted"
               " clean — it can never fire, and it reads as a documented"
               " confound:\n"
            << r.output;
    }
}

// ── THE CONDITION UNDER WHICH AN EXCUSE HOLDS IS A MEASUREMENT ──────────────
//
// A CONFOUND'S CONDITION IS A RUN MODE, NOT A HOST.
//
// `scope` matched a pattern against the leg's RUN MODE. That is the wrong axis
// for a row whose mechanism is a property of the MACHINE, and the catalogue could
// not say so — so `^walsetlk-` sat at `scope: any` on both ELF legs and would
// have silently excused a GENUINE walsetlk failure on the arm64 VPS, where the
// clock has never been shown to step. The condition was ALREADY WRITTEN DOWN, in
// legs[1]'s `$confoundsComment` prose ("…WHEN THIS HARNESS IS DRIVEN FROM THIS
// BOX"), where nothing read it — the same failure as `earnedOn`, one field along.
//
// ★★ THE ERRORS ARE NOT SYMMETRIC, WHICH IS WHY EVERY CASE BELOW LEANS ONE WAY.
// A probe that says ABSENT on a defective box produces noisy reds somebody then
// investigates. A probe that says PRESENT on a HEALTHY box SILENTLY EXCUSES a real
// compiler defect. So: `indeterminate` is honoured as absent, an unprobed plan
// honours no conditional row, and config may only ever TIGHTEN a threshold.
//
// RED-ON-DISABLE: drop the `requires`/floor rules from harness_legs.py's lint and
// the mutants below lint clean.
TEST_F(HarnessLegs, ConditionalConfoundsAreGatedOnAMeasuredEnvironmentProbe) {
    auto const doc = json::parse(fileText(catalogue_));
    ASSERT_TRUE(doc.contains("environmentProbes"))
        << "the catalogue declares no `environmentProbes` registry, so every"
           " `requires` name is unresolvable and nothing is gated.";
    auto const& probes = doc.at("environmentProbes");
    ASSERT_TRUE(probes.contains("clock-realtime-steps"))
        << "the clock families require it by name.";
    auto const& clock = probes.at("clock-realtime-steps");
    if (clock.contains("anchor")) {
        EXPECT_FALSE(clock.at("anchor").get<std::string>().empty())
            << "environmentProbes['clock-realtime-steps'] carries an EMPTY `anchor`"
               " -- the key is optional, an empty value is a pointer at nothing.";
    }
    for (char const* k : {"verb", "measures", "presentMeans"}) {
        EXPECT_TRUE(clock.contains(k) &&
                    !clock.at(k).get<std::string>().empty())
            << "environmentProbes['clock-realtime-steps'] omits '" << k
            << "'. A probe decides whether a failing test is excused; it states"
               " what it measures and what a PRESENT verdict would mean.";
    }
    // ★ THE THRESHOLDS LIVE IN CONFIG so tightening one is an edit and not a code
    //   change. The FLOORS live in harness_legs.py so config can only ever move
    //   them in the safe direction.
    auto const& cfg = clock.at("config");
    EXPECT_GE(cfg.at("sampleSeconds").get<double>(), 15.0)
        << "the sample must be long enough to see at least two steps of a clock"
           " that flips every ~5 s — never a single pair of readings.";
    EXPECT_GE(cfg.at("minStepsRequired").get<int>(), 2)
        << "one jump is a suspend/resume, not a stepping clock.";
    EXPECT_GE(cfg.at("minStepSeconds").get<double>(), 1.0)
        << "a sub-second threshold would fire on scheduler noise, which would"
           " excuse the clock family on every loaded machine.";

    // THE THREE CLOCK FAMILIES ARE GATED; the host-independent rows are NOT.
    // `^recoverfault` is the one that matters here: its mechanism is an OOM
    // ORACLE, not a clock, and gating a host-independent proof on a transient
    // host defect would be wrong in the noisy direction for no reason.
    std::map<std::string, std::vector<std::string>> want{
        {"^walsetlk-", {"clock-realtime-steps"}},
        {"^walsetlk_recover-", {"clock-realtime-steps"}},
        {"^busy2-", {"clock-realtime-steps"}},
        {"^recoverfault", {}},
        {"^zipfile-25\\.0$", {}},
    };
    std::set<std::string> exercised;
    for (auto const& leg : doc.at("legs")) {
        for (auto const& row : leg.at("confounds")) {
            auto const pat = row.at("pattern").get<std::string>();
            auto const it = want.find(pat);
            if (it == want.end()) { continue; }
            exercised.insert(pat);
            EXPECT_EQ(row.at("requires").get<std::vector<std::string>>(),
                      it->second)
                << leg.at("label").get<std::string>() << ": confound '" << pat
                << "' declares the wrong `requires`. A clock-mechanism row must"
                   " be gated on the clock probe, and a host-INDEPENDENT row must"
                   " not be gated at all.";
        }
    }
    // ★★ EVERY EXPECTATION WAS EXERCISED. Without this the `continue` above makes
    // the whole map optional: RENAME `^walsetlk-` in the catalogue and this test
    // silently stops checking three clock families while still reporting green.
    // A pin whose subject can be renamed out from under it asserts nothing, and
    // "it passed" would then mean "no row matched any key I know".
    for (auto const& [pat, requires_] : want) {
        (void)requires_;
        EXPECT_TRUE(exercised.count(pat) == 1)
            << "no leg in the catalogue declares a confound with pattern '" << pat
            << "', so this test's expectation for it was never checked. Either the"
               " row was renamed (update this map) or it was deleted (say so here"
               " deliberately) — a silently-unexercised expectation is the same"
               " defect as no expectation at all.";
    }

    {   // `scope: any` re-added: the retired spelling must be refused, not
        // silently honoured as "excused however this leg runs".
        MutatedCatalogue m{catalogue_, "legs-confound-scope-any-retired"};
        m.legOf("elf64-x86_64").at("confounds")[0]["scope"] = "any";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "`scope: any` linted clean. It is the widest possible excusal and"
               " it is now spelled `requires: []`; accepting both lets the"
               " unconditional claim be made two ways, one of which is the one"
               " that hid the clock condition:\n"
            << r.output;
    }
    {   // A `requires` naming a probe the registry does not declare.
        MutatedCatalogue m{catalogue_, "legs-confound-unknown-probe"};
        m.legOf("elf64-x86_64").at("confounds")[0]["requires"] =
            json::array({"clock-goes-backwards"});
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a confound requiring an UNDECLARED probe linted clean — it would"
               " be honoured on nothing:\n"
            << r.output;
    }
    {   // The key omitted entirely.
        MutatedCatalogue m{catalogue_, "legs-confound-requires-missing"};
        m.legOf("elf64-x86_64").at("confounds")[0].erase("requires");
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a confound with NO `requires` linted clean:\n" << r.output;
    }
    {   // ★★ THE FAIL-SAFE FLOOR, WHICH IS THE ONE A FUTURE READER WILL BE
        // TEMPTED BY: a 5 s sample "to make the run faster". It cannot see two
        // steps of a clock that flips every ~5 s, so it would report ABSENT on
        // the very box the defect was measured on — and then somebody would
        // "fix" that by lowering minStepSeconds instead.
        MutatedCatalogue m{catalogue_, "legs-probe-window-too-short"};
        m.doc()["environmentProbes"]["clock-realtime-steps"]["config"]
               ["sampleSeconds"] = 5;
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a sample window below the floor linted clean:\n" << r.output;
    }
    {   // A single pair of readings can never satisfy the probe.
        MutatedCatalogue m{catalogue_, "legs-probe-one-step"};
        m.doc()["environmentProbes"]["clock-realtime-steps"]["config"]
               ["minStepsRequired"] = 1;
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "requiring only ONE step linted clean:\n" << r.output;
    }
    {   // A threshold in scheduler-noise territory would excuse the clock family
        // on every loaded machine — the FALSE POSITIVE direction, the dangerous
        // one, because it excuses a real miscompile in silence.
        MutatedCatalogue m{catalogue_, "legs-probe-noise-threshold"};
        m.doc()["environmentProbes"]["clock-realtime-steps"]["config"]
               ["minStepSeconds"] = 0.01;
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a 10 ms step threshold linted clean:\n" << r.output;
    }
    {   // A typo'd threshold must not be silently ignored: the probe would then
        // run at a sensitivity nobody chose, which reads exactly like one that
        // was configured.
        MutatedCatalogue m{catalogue_, "legs-probe-unknown-config-key"};
        m.doc()["environmentProbes"]["clock-realtime-steps"]["config"]
               ["minStepSecs"] = 5;
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown probe config key linted clean:\n" << r.output;
    }
    {   // An unknown VERB: a probe's verb IS its measured procedure, and there is
        // no defensible default for one.
        MutatedCatalogue m{catalogue_, "legs-probe-unknown-verb"};
        m.doc()["environmentProbes"]["clock-realtime-steps"]["verb"] =
            "guess-the-clock";
        auto const r = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown environment-probe verb linted clean:\n" << r.output;
    }
}

// THE PLAN SAYS WHETHER ITS GATING WAS MEASURED, AND AN UNMEASURED PLAN HONOURS
// NO CONDITIONAL ROW. [a confound's condition is a RUN MODE, not a host.]
//
// `--environment-probes skip` is the structural door — it exists so a caller that
// only wants the plan's SHAPE need not sample a clock for 20 s. What it must never
// do is produce a plan that looks measured: the driver refuses `unprobed`
// (`sqlite_verdicts.py`), and this is the resolver half of that contract.
TEST_F(HarnessLegs, AnUnprobedPlanHonoursNoConditionalConfound) {
    auto const r = run({"--plan", "--host-os", "linux", "--host-arch", "x86_64",
                        "--environment-probes", "skip"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    auto const plan = json::parse(r.output);
    EXPECT_FALSE(plan.at("environmentProbesRun").get<bool>());
    for (auto const& leg : plan.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        EXPECT_EQ(leg.at("confoundGating").get<std::string>(), "unprobed")
            << label << ": a plan resolved without measuring must SAY so, or a"
                        " driver cannot tell it from a measured one.";
        // ★ THE FAIL-SAFE DIRECTION: conditional rows dropped, never honoured.
        auto const wire = leg.at("confounds").get<std::vector<std::string>>();
        for (char const* gated : {"^walsetlk-", "^walsetlk_recover-",
                                  "^busy2-"}) {
            EXPECT_EQ(std::find(wire.begin(), wire.end(), gated), wire.end())
                << label << ": '" << gated
                << "' was honoured on a plan that measured NOTHING. Honouring a"
                   " conditional row on an unmeasured machine is how a real"
                   " miscompile gets excused in silence.";
        }
        // ★★ AND THE ACCOUNT IS PRESENT AND NON-EMPTY. `earnedOn` failed because
        // it is prose nothing reads; a probe decision nobody sees is the same
        // failure with extra steps.
        ASSERT_TRUE(leg.contains("confoundReport"));
        EXPECT_FALSE(leg.at("confoundReport").empty())
            << label << ": the plan carries an EMPTY confound report.";
        bool sawInactive = false;
        for (auto const& line : leg.at("confoundReport")) {
            auto const t = line.get<std::string>();
            if (t.find("INACTIVE") != std::string::npos) { sawInactive = true; }
            for (unsigned char c : t) {
                EXPECT_LT(c, 127u)
                    << label << ": the report line is not ASCII: " << t
                    << "\n  The driver prints this account verbatim into its run"
                       " log and the console, whose encodings differ by host, so"
                       " a non-ASCII character reaches a reader as whatever that"
                       " host's codepage makes of it (the resolver's generator"
                       " refuses one for the same reason).";
            }
        }
        if (label == "elf64-x86_64" || label == "elf64-arm64") {
            EXPECT_TRUE(sawInactive)
                << label << ": three rows were withheld and the report says"
                            " nothing about it. An exclusion nobody can explain"
                            " is not an earned one — and neither is a"
                            " NON-exclusion.";
        }
    }
}

// ── A VERDICT MEASURED IN THIS KERNEL DOES NOT DECIDE A LEG THAT RUNS IN ANOTHER ─
//
// THE ENVIRONMENT PROBE MEASURED THE DRIVER'S KERNEL, NOT THE LAUNCHED ONE.
//
// ★★★ THE CAVEAT WAS TRUE PROSE AND A FALSE STATEMENT AT THE SAME TIME. ✔MEASURED
// at 0ecec160 with `--host-os windows --host-arch x86_64 --launchers-available
// wsl.exe` and a `present` verdict: the report printed "runFilesystem 'wsl-linux'
// does NOT share this driver's kernel ... rows go INACTIVE and such a failure is
// reported as GENUINE", and THE NEXT LINE printed `confound rows ACTIVE (7 of 7)`.
// The decision function had never heard of the launcher; only the printout had.
//
// ⛔ THE SCENARIO THAT MAKES IT DANGEROUS, and it is ordinary: a Windows host whose
// OWN CLOCK_REALTIME steps — VM checkpoint/migration, a time-sync storm, chrony
// `makestep` — driving the ELF legs through wsl.exe. The probe samples the WINDOWS
// clock, answers `present`, and every ^walsetlk-/^busy2- failure produced inside
// the WSL2 kernel is silently excused, including a genuine WAL blocking-lock
// miscompile that the ^walsetlk- row's own mechanism text says must stay red.
//
// ★ EVERY DIRECTION IS PINNED, because a fix that turned the clock rows off
// everywhere would satisfy the dangerous one while deleting the mechanism.
//
// ⓘ AND THIS IS THE READER `confoundDecisions` DID NOT HAVE. That field was emitted
// and consumed by nothing for a cycle; the per-row ACTIVE/INACTIVE decision is what
// this test needs, and reading it beats grepping the prose report for a substring.
//
// ★★★ V2 (2026-08-12) REMOVES THE FILTER BY REMOVING ITS SUBJECT. Verdicts are
// measured PER KERNEL and filed under that kernel's name, so a leg reads only the
// drawer for the kernel IT executes in. The dangerous direction is no longer
// filtered out — it cannot be expressed. What replaces the old force-to-
// indeterminate is the case where the RIGHT kernel could not be measured, which is
// `outcome: unreachable`, INDETERMINATE verdicts, and a caveat that says so.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): make probe_gate() read
// the driver's drawer when the leg's own kernel is absent from the map, and the
// first case below goes green with 7 of 7 rows active on a verdict about Windows.
TEST_F(HarnessLegs, ACrossKernelLegDoesNotHonourAVerdictMeasuredByThisDriver) {
    // The verdicts are INJECTED so the case is deterministic on every host: what is
    // under test is the DECISION, not this machine's clock. An injected plan is
    // stamped `confoundGating: injected` on purpose — see the gating test below.
    // ★ THE FILE NAMES THE KERNEL. That is the whole subject: a verdict with no
    // machine attached is what let a Windows measurement decide a WSL2 fixture.
    auto const write = [&](char const* name, std::string const& body) {
        auto const p = scratch_->path() / name;
        std::ofstream out(p, std::ios::binary);
        out << body;
        return p;
    };
    std::string const entry =
        R"({"verdict": "present", "why": "pinned fixture: a stepping clock",)"
        R"( "verb": "wall-clock-step", "evidence": {"steps": 4}})";
    auto const driverFile = write(
        "present-in-driver.json",
        R"({"driver": {"clock-realtime-steps": )" + entry + "}}");
    auto const wslFile = write(
        "present-in-wsl.json",
        R"({"wsl-linux": {"clock-realtime-steps": )" + entry + "}}");
    auto const clockRows = std::set<std::string>{"^walsetlk-",
                                                 "^walsetlk_recover-", "^busy2-"};

    auto planWith = [&](std::filesystem::path const& vfile,
                        std::vector<std::string> args) {
        args.emplace_back("--probe-verdicts");
        args.emplace_back(vfile.string());
        return run(args);
    };

    {   // ★ THE DANGEROUS DIRECTION: a Windows host whose OWN clock steps, driving
        // the ELF leg through wsl.exe. The `present` describes the DRIVER's kernel;
        // the fixture executes in WSL2. It must not reach that leg — and because
        // the leg's own drawer is then empty of a probe its rows require, the
        // resolution REFUSES rather than guessing either way.
        auto const r = planWith(driverFile,
                                {"--plan", "--host-os", "windows", "--host-arch",
                                 "x86_64", "--launchers-available", "wsl.exe"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a verdict measured in the DRIVER's kernel was accepted for a leg"
               " whose fixture executes in WSL2. Guessing 'present' silently"
               " excuses a real WAL blocking-lock miscompile; guessing 'absent'"
               " hides a broken probe run behind plausible reds:\n" << r.output;
        EXPECT_EQ(r.output.find("Traceback"), std::string::npos)
            << "the refusal must be a named diagnostic, not a python traceback:\n"
            << r.output;
        EXPECT_NE(r.output.find("clock-realtime-steps"), std::string::npos)
            << "the refusal must name the probe whose verdict is missing:\n"
            << r.output;
    }
    {   // ★ THE CAPABILITY THIS CYCLE ADDS: the SAME verdict, filed under the
        // kernel the fixture actually runs in, IS honoured there. This is the
        // withheld-excusal recovery — 4 walsetlk reds on elf64-x86_64 and 3 on
        // elf64-arm64 at 52cf784d that the arm64 VPS ran green from the same
        // commit against the same upstream tree.
        auto const r = planWith(wslFile,
                                {"--plan", "--host-os", "windows", "--host-arch",
                                 "x86_64", "--launchers-available", "wsl.exe"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const plan = json::parse(r.output);
        auto const leg = legFrom(plan, "elf64-x86_64");
        ASSERT_FALSE(leg.empty());
        ASSERT_EQ(leg.at("run").at("runFilesystem").get<std::string>(),
                  "wsl-linux")
            << "this case is only about a launcher in another kernel; if the plan"
               " stopped resolving one, it proves nothing.";
        auto const wire = leg.at("confounds").get<std::set<std::string>>();
        for (auto const& pat : clockRows) {
            EXPECT_EQ(wire.count(pat), 1u)
                << pat << " was NOT honoured on a leg whose OWN kernel measured"
                          " the defect. Those excusals are real and were being"
                          " withheld only because the probe looked at the wrong"
                          " machine.";
        }
        // ★★ AND THE ACCOUNT NO LONGER CARRIES THE NOT-APPLIED CAVEAT. Its old
        // condition was "this leg runs somewhere else", which was the right thing
        // to say only while the probe could not GO there. Printing it now would be
        // a fresh instance of the claim-rot this anchor exists to remove.
        // ⚠ THE COUNT IS DERIVED, NEVER TYPED.
        // [a withholding pin coupled to the confound catalogue's SIZE]
        // What this assertion is ABOUT is that NO row was withheld — `N of N` for
        // whatever N this leg declares. Spelling N as the literal 7 coupled a
        // WITHHOLDING claim to the catalogue's SIZE, so earning one more confound
        // reddened a test that has nothing to say about how many there are. It
        // fails in the safe direction, which is exactly why it survived: the red
        // names a kernel-attribution test and the cause is an unrelated `legs.json`
        // row. Deriving it keeps the real claim — withhold one and `wire` shrinks
        // while the report reads `(N-1 of N)`, so neither side matches.
        auto const activeLine = "confound rows ACTIVE ("
                                + std::to_string(wire.size()) + " of "
                                + std::to_string(wire.size()) + ")";
        bool namesKernel = false, caveat = false, allActive = false;
        for (auto const& l : leg.at("confoundReport")) {
            auto const t = l.get<std::string>();
            if (t.find("executes in kernel 'wsl-linux'") != std::string::npos)
                namesKernel = true;
            if (t.find("CAVEAT") != std::string::npos) caveat = true;
            if (t.find(activeLine) != std::string::npos)
                allActive = true;
        }
        EXPECT_TRUE(namesKernel)
            << "the report must say WHICH KERNEL the verdicts below it describe;"
               " a verdict with no machine attached is this anchor's subject.";
        EXPECT_FALSE(caveat)
            << "the measurement came from the right kernel, so a 'NOT APPLIED'"
               " caveat would be false — and it would sit one line above ACTIVE"
               " (N of N), which is exactly the pairing V1 shipped.";
        EXPECT_TRUE(allActive)
            << "the caveat's absence must be TRUE: the rows must be in force."
               " Looked for: " << activeLine;
        // AND THE PER-KERNEL RECORD IS AT THE TOP OF THE PLAN, keyed on the kernel.
        ASSERT_TRUE(plan.at("environmentProbes").contains("wsl-linux"))
            << "the plan must file the measurement under the kernel it describes.";
        EXPECT_EQ(plan.at("environmentProbes").at("wsl-linux")
                      .at("kernel").get<std::string>(), "wsl-linux");
    }
    {   // ★ SAME KERNEL, SAME VERDICT: the mechanism must still work natively.
        auto const r = planWith(driverFile,
                                {"--plan", "--host-os", "linux", "--host-arch",
                                 "x86_64", "--launchers-none"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const leg = legFrom(json::parse(r.output), "elf64-x86_64");
        ASSERT_FALSE(leg.empty());
        ASSERT_EQ(leg.at("run").at("mode").get<std::string>(), "native");
        auto const wire = leg.at("confounds").get<std::set<std::string>>();
        for (auto const& pat : clockRows) {
            EXPECT_EQ(wire.count(pat), 1u)
                << pat << " was NOT honoured on a leg that runs in the very kernel"
                          " the verdict describes. Turning the rows off everywhere"
                          " would pass the cross-kernel case above while deleting"
                          " the mechanism it protects.";
        }
    }
    {   // ★ AND A FILE WITH NO KERNEL ON IT — the shape this harness used before it
        // could measure more than one — is REFUSED, not read as the driver's.
        auto const flat = write(
            "present-flat.json",
            R"({"clock-realtime-steps": )" + entry + "}");
        auto const r = planWith(flat, {"--plan", "--host-os", "linux",
                                       "--host-arch", "x86_64",
                                       "--launchers-none"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "a verdict about an unnamed machine was accepted:\n" << r.output;
        EXPECT_NE(r.output.find("keyed on PROBE NAMES"), std::string::npos)
            << "the refusal must say what is wrong and how to spell it:\n"
            << r.output;
    }
}

// ── AN INJECTED VERDICT IS VALIDATED, VISIBLE, AND CANNOT RUN A CORPUS ──────
//
// THE PROBE-VERDICTS FLAG INJECTED AN UNVALIDATED `present`.
//
// ✔MEASURED at 0ecec160: `--probe-verdicts` accepted any JSON object, checked only
// `isinstance(dict)`, stamped the plan `confoundGating: probed` and honoured
// whatever it said — a hand-written `{"verdict":"present","why":"I said so"}` gave
// 7 of 7 rows ACTIVE and a report line indistinguishable from a measurement. The
// old operator door (DSS_CONFOUNDS) announces itself per leg; the new one was
// quieter, in the direction that hides a compiler defect. And a verdict file
// captured on the WSL2 box and replayed on the arm64 VPS would have been honoured
// without complaint — restoring by flag exactly the blind spot this cycle closed.
TEST_F(HarnessLegs, AnInjectedProbeVerdictIsAnnouncedAndCannotRunACorpus) {
    auto write = [&](char const* name, std::string const& body) {
        auto const p = scratch_->path() / name;
        std::ofstream out(p, std::ios::binary);
        out << body;
        return p;
    };
    auto const good = write(
        "inject-good.json",
        R"({"driver": {"clock-realtime-steps": {"verdict": "present", "why": )"
        R"("captured elsewhere", "verb": "wall-clock-step", "evidence": {}}}})");
    {
        auto const r = run({"--plan", "--host-os", "linux", "--host-arch",
                            "x86_64", "--launchers-none", "--probe-verdicts",
                            good.string()});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const plan = json::parse(r.output);
        for (auto const& leg : plan.at("legs")) {
            EXPECT_EQ(leg.at("confoundGating").get<std::string>(), "injected")
                << leg.at("label").get<std::string>()
                << ": a plan built from a FILE must not be stamped `probed`. The"
                   " driver runs only on `probed`, and that refusal is what stops"
                   " a verdict captured on another machine from excusing failures"
                   " here.";
        }
        // ⚠ THE LEG IS HOISTED INTO A NAMED LOCAL, NOT ITERATED THROUGH A
        // TEMPORARY. `for (auto const& l : legFrom(plan, …).at("confoundReport"))`
        // binds a reference INTO a temporary json that dies at the end of the
        // range-init — lifetime extension does not reach through `.at()` — so the
        // loop walks freed memory. ✔MEASURED: this test failed with "no report line
        // mentioned the probe at all" while the resolver was printing the line.
        auto const elf = legFrom(plan, "elf64-x86_64");
        ASSERT_FALSE(elf.empty());
        bool said = false;
        for (auto const& l : elf.at("confoundReport")) {
            auto const t = l.get<std::string>();
            if (t.find("environment probe clock-realtime-steps") ==
                std::string::npos)
                continue;
            EXPECT_NE(t.find("INJECTED by --probe-verdicts"), std::string::npos)
                << "a log reader must never mistake an injected verdict for a"
                   " measurement: " << t;
            said = true;
        }
        EXPECT_TRUE(said) << "no report line mentioned the probe at all, so the"
                             " INJECTED announcement was never checked.";
    }
    // ── EVERY MALFORMED SHAPE IS A NAMED REFUSAL, NEVER A PYTHON TRACEBACK ──
    // ✔MEASURED before this: `{"clock-realtime-steps":"present"}` raised
    // ValueError three frames deeper, and `{"...":{"why":"x"}}` raised KeyError —
    // in the one place whose job is to say what this harness believes about a
    // machine.
    struct Bad {
        char const* name;
        char const* body;
        char const* why;
    };
    Bad const bad[] = {
        {"inject-bare.json", R"({"driver": {"clock-realtime-steps": "present"}})",
         "a bare string cannot say what was measured or how"},
        {"inject-noverdict.json",
         R"({"driver": {"clock-realtime-steps": {"why": "x"}}})",
         "a verdict object with no verdict"},
        {"inject-unknown.json",
         R"({"driver": {"clock-goes-backwards": {"verdict": "absent", )"
         R"("why": "x", "verb": "wall-clock-step", "evidence": {}}}})",
         "a probe the registry does not declare can gate nothing"},
        {"inject-invented.json",
         R"({"driver": {"clock-realtime-steps": {"verdict": "probably", )"
         R"("why": "x", "verb": "wall-clock-step", "evidence": {}}}})",
         "an invented verdict word"},
        {"inject-wrongverb.json",
         R"({"driver": {"clock-realtime-steps": {"verdict": "present", )"
         R"("why": "x", "verb": "guess-the-clock", "evidence": {}}}})",
         "a verdict naming another procedure's verb"},
        {"inject-nowhy.json",
         R"({"driver": {"clock-realtime-steps": {"verdict": "present", )"
         R"("why": "  ", "verb": "wall-clock-step", "evidence": {}}}})",
         "a verdict with no stated evidence is the `earnedOn` defect"},
        // ★ THE KERNEL NAME IS VALIDATED TOO: a drawer nobody opens would let a
        // typo'd kernel decide nothing, silently — the direction that hides an
        // unapplied measurement.
        // [the probe measuring the DRIVER's kernel, not the launched one]
        {"inject-unknownkernel.json",
         R"({"some-other-box": {"clock-realtime-steps": {"verdict": "present", )"
         R"("why": "x", "verb": "wall-clock-step", "evidence": {}}}})",
         "a kernel no declared runFilesystem resolves to"},
    };
    for (auto const& b : bad) {
        auto const p = write(b.name, b.body);
        auto const r = run({"--plan", "--host-os", "linux", "--host-arch",
                            "x86_64", "--launchers-none", "--probe-verdicts",
                            p.string()});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << b.name << " was ACCEPTED (" << b.why << "):\n" << r.output;
        EXPECT_EQ(r.output.find("Traceback"), std::string::npos)
            << b.name << " produced a python traceback rather than a named"
                         " diagnostic:\n" << r.output;
    }
}

// THE PROBE IS A MEASUREMENT AND IT REPORTS ITS EVIDENCE.
// [a confound's condition is a RUN MODE, not a host.]
//
// ⓘ THIS RUNS THE REAL PROBE, so it costs the declared sample window once. That is
// deliberate: the verb's arms are all driven with INJECTED clocks by
// `harness_legs.py --self-test` (present, absent, indeterminate, and both sides of
// each threshold), and what only an end-to-end run can establish is that the CLI
// really samples this machine and really publishes what it saw. A verdict with no
// evidence beside it is the `earnedOn` defect again.
//
// ⚠ IT IS ALSO THE ONE SPAWN IN THIS FILE THAT SAMPLES, which is why the deadline
// it runs under is asked of the script rather than typed here — see the
// SpawnBudget block at the top. This case is where the old typed 120 s was spent
// on a child the script had budgeted 200 s for.
TEST_F(HarnessLegs, TheEnvironmentProbeReportsAVerdictWithItsEvidence) {
    auto const r = run({"--probe-environment"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    // rc 0 whatever the verdict: ABSENT is a SUCCESSFUL measurement. A non-zero
    // rc for "your clock is fine" would teach a driver to treat a healthy machine
    // as a broken run.
    EXPECT_EQ(r.exitCode, 0u) << r.output;
    auto const v = json::parse(r.output);
    ASSERT_TRUE(v.contains("clock-realtime-steps")) << r.output;
    auto const& got = v.at("clock-realtime-steps");
    auto const verdict = got.at("verdict").get<std::string>();
    EXPECT_TRUE(verdict == "present" || verdict == "absent" ||
                verdict == "indeterminate")
        << "invented verdict '" << verdict << '\'';
    EXPECT_FALSE(got.at("why").get<std::string>().empty())
        << "a verdict with no stated evidence is the `earnedOn` defect wearing a"
           " JSON key.";
    EXPECT_EQ(got.at("verb").get<std::string>(), "wall-clock-step");
    if (verdict != "indeterminate") {
        // The measurement must actually have HAPPENED — a probe that reports
        // ABSENT having taken no samples is the false-negative direction, safe
        // but useless, and it must be visible rather than inferred.
        EXPECT_GE(got.at("evidence").at("samples").get<int>(), 2)
            << "a decisive verdict from fewer than two samples: " << r.output;
    }
}

// THE DEADLINE THAT SPAWN RAN UNDER IS THE SCRIPT'S OWN, AND IT MOVES WHEN THE
// DECLARED WINDOW MOVES.
// [a test timeout that is a magic number instead of the derived budget]
//
// ⓘ COSTS NOTHING — `--print-probe-budget` samples nothing, so all three spawns
// here are ~0.1 s. That is deliberate: the property is "the number tracks the
// declaration", and paying a sample window to observe it would make the pin too
// expensive to keep.
//
// ★ THE SHAPE OF THE ASSERTION IS THE WHOLE POINT, so it is spelled out. It does
// NOT re-state the formula — re-deriving `allowance + slack x window` here would
// be the two-derivations defect in the file that names it. It states only what
// no constant can satisfy: widen the declared window by `d` and the budget grows
// by some g > 0; widen it by `2d` and the budget grows by EXACTLY `2g`. A
// re-typed literal yields g == 0 and fails the first clause; anything that
// tracks the declaration linearly passes both without this file knowing the
// slope. And the zero-window budget must NOT move, because it prices the
// invocations that sample nothing.
//
// RED-ON-REVERT (measured, numbers in the report): restore the typed
// `std::chrono::seconds{120}` and this test fails at the first EXPECT — a
// constant cannot grow.
TEST_F(HarnessLegs, TheSpawnDeadlineIsTheScriptsOwnBudgetAndTracksTheWindow) {
    auto const declaredWindow = [](json& doc) -> json& {
        return doc.at("environmentProbes").at("clock-realtime-steps")
                  .at("config").at("sampleSeconds");
    };
    std::string why;
    auto const shipped = askSpawnBudget(catalogue_, why);
    ASSERT_TRUE(why.empty()) << why;
    // ★ THE DEADLINE THAT WAS ACTUALLY APPLIED, not merely one that could have
    // been fetched. Everything below proves the SCRIPT's number tracks the
    // declaration; this proves the number the spawns ran under IS that one. A
    // literal put back in the fixture reds here and nowhere else.
    EXPECT_EQ(resolverDeadline(), shipped.probeEnvironment)
        << "every spawn in this file ran under " << resolverDeadline().count()
        << " ms while the script's own budget for the heaviest of them is "
        << shipped.probeEnvironment.count()
        << " ms. A deadline the subject never agreed to is the whole defect —"
           " a magic number standing in for the derived budget — and it kills a"
           " healthy child rather than a hung one.";
    EXPECT_GT(shipped.probeEnvironment, shipped.noSample)
        << "the shipped catalogue DECLARES a sample window, so a run that"
           " measures it must be priced above one that measures nothing. Equal"
           " budgets mean the declared window reached the arithmetic as zero.";

    auto const base = json::parse(fileText(catalogue_))
                          .at("environmentProbes").at("clock-realtime-steps")
                          .at("config").at("sampleSeconds").get<double>();
    // Both widenings are RAISES, which is the only direction the verb's
    // `raiseOnly` floors permit — a narrowed window would be refused by the lint
    // and would prove nothing about the budget.
    MutatedCatalogue oneStep{catalogue_, "legs-window-1x"};
    declaredWindow(oneStep.doc()) = base + 30.0;
    auto const widened = askSpawnBudget(oneStep.commit(), why);
    ASSERT_TRUE(why.empty()) << why;

    MutatedCatalogue twoSteps{catalogue_, "legs-window-2x"};
    declaredWindow(twoSteps.doc()) = base + 60.0;
    auto const widenedTwice = askSpawnBudget(twoSteps.commit(), why);
    ASSERT_TRUE(why.empty()) << why;

    auto const grew = widened.probeEnvironment - shipped.probeEnvironment;
    EXPECT_GT(grew, std::chrono::milliseconds::zero())
        << "widening the DECLARED sample window by 30 s did not move the"
           " deadline this gate spawns under, so the deadline is not derived"
           " from the declaration at all — which is a number typed somewhere,"
           " and it will be the wrong number the day the window is widened for"
           " real.";
    EXPECT_EQ(widenedTwice.probeEnvironment - shipped.probeEnvironment, 2 * grew)
        << "the budget does not scale LINEARLY with the declared window (+30 s"
           " bought " << grew.count() << " ms, +60 s bought "
        << (widenedTwice.probeEnvironment - shipped.probeEnvironment).count()
        << " ms), so it is tracking the declaration through something other"
           " than the script's own arithmetic.";
    EXPECT_EQ(widenedTwice.noSample, shipped.noSample)
        << "the ZERO-window budget moved when a window was widened. It prices"
           " the invocations that sample nothing — every lint and plan spawn in"
           " this file — and a window has no business in it.";
}

// ── THE DOORS AROUND THAT INSTRUMENT ARE FAIL-CLOSED ────────────────────────
//
// THE ENVIRONMENT PROBE MEASURED THE DRIVER'S KERNEL, NOT THE LAUNCHED ONE.
//
// `--probe-environment` is what `--plan` re-enters this script with INSIDE each
// kernel a leg executes in, so its contract is now load-bearing in a second place:
// it measures where it IS, it measures exactly what it was ASKED for, and it never
// prints anything it did not measure. Each case below is refused BEFORE any clock
// is sampled, so this whole test costs nothing.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): drop the `--probe-only`
// membership check and the undeclared-name case goes green with an empty
// measurement — a map that looks successful and measured nothing.
TEST_F(HarnessLegs, TheProbeMeasurementFlagsRefuseWhatTheyCannotAnswer) {
    struct Case {
        char const* why;
        std::vector<std::string> args;
        char const* says;
    };
    auto const vfile = scratch_->path() / "probe-doors.json";
    {
        std::ofstream out(vfile, std::ios::binary);
        out << R"({"driver": {"clock-realtime-steps": {"verdict": "present",)"
               R"( "why": "x", "verb": "wall-clock-step", "evidence": {}}}})";
    }
    Case const cases[] = {
        {"an UNDECLARED probe name would silently narrow the measurement to"
         " nothing while still printing a successful-looking map",
         {"--probe-environment", "--probe-only", "clock-goes-backwards"},
         "environmentProbes"},
        {"--probe-only outside --probe-environment would look like it had"
         " narrowed something and do nothing at all",
         {"--plan", "--host-os", "linux", "--host-arch", "x86_64",
          "--launchers-none", "--probe-only", "clock-realtime-steps"},
         "--probe-only"},
        {"measuring and reading a file at once would print somebody else's answer"
         " under the name of a measurement",
         {"--probe-environment", "--probe-verdicts", vfile.string()},
         "MEASURES"},
        {"measuring and forbidding measurement at once",
         {"--probe-environment", "--environment-probes", "skip"},
         "forbids measuring"},
        // ★ AND THE DOOR THE BUDGET FLAG ADDED. One flag PRICES a measurement,
        // the other PERFORMS it; answering both would hand a caller a number
        // while it believed it had measured this kernel — the same
        // "somebody else's answer under the name of a measurement" shape as
        // the --probe-verdicts door above, one flag along.
        {"pricing a measurement and performing it at once",
         {"--probe-environment", "--print-probe-budget"},
         "prices a measurement"},
    };
    for (auto const& c : cases) {
        auto const r = run(c.args);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u) << "ACCEPTED, and " << c.why << ":\n"
                                  << r.output;
        EXPECT_EQ(r.output.find("Traceback"), std::string::npos)
            << "the refusal must be a named diagnostic:\n" << r.output;
        EXPECT_NE(r.output.find(c.says), std::string::npos)
            << "the refusal must name what is wrong (looking for '" << c.says
            << "'):\n" << r.output;
    }
}

// THE RESUME ENGINE'S OWN HOOK MUST CROSS. `--env-transfer` turns a verb plus a
// list of variable NAMES into the assignments a driver must make, and this is
// the part that decides whether a cross-OS launcher's fixture can be steered at
// all. Needs no WSL: the merge and the carrier are pure string work.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): make
// `env_carrier_assignments` return [] for every verb and the wslenv cases fail.
TEST_F(HarnessLegs, TheRunEnvironmentIsForwardedIntoTheLaunchersNamespace) {
    auto forward = [&](char const* verb, std::vector<std::string> const& names,
                       char const* current) {
        std::vector<std::string> args{"--env-transfer", verb, "--carrier-current",
                                      current};
        for (auto const& n : names) { args.push_back("--forward"); args.push_back(n); }
        return run(args);
    };
    {  // `inherit` must add NOTHING — a native run stays byte-for-byte itself.
        auto const r = forward("inherit", {"SQLITE_TEST_PATTERN_LIST"}, "");
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 0u) << r.output;
        EXPECT_TRUE(splitLines(r.output).empty())
            << "an inheriting launcher was given environment assignments:\n"
            << r.output;
    }
    {
        auto const r = forward("wslenv",
                               {"SQLITE_TEST_PATTERN_LIST", "QUICKTEST_OMIT"}, "");
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const lines = splitLines(r.output);
        ASSERT_EQ(lines.size(), 1u) << r.output;
        EXPECT_EQ(lines[0], "WSLENV=SQLITE_TEST_PATTERN_LIST:QUICKTEST_OMIT")
            << "the resume engine steers the corpus through"
               " SQLITE_TEST_PATTERN_LIST; a launcher that cannot see it re-runs"
               " the whole corpus and looks like it is working";
    }
    {  // an operator's own carrier value survives.
        auto const r = forward("wslenv", {"QUICKTEST_OMIT"}, "MYVAR/u");
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const lines = splitLines(r.output);
        ASSERT_EQ(lines.size(), 1u) << r.output;
        EXPECT_EQ(lines[0], "WSLENV=MYVAR/u:QUICKTEST_OMIT")
            << "an operator's existing carrier setting was clobbered";
    }
    {  // an unknown verb is FATAL, never a silent "inherit".
        auto const r = forward("copy-the-block", {"A"}, "");
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an unknown envTransfer verb resolved silently:\n" << r.output;
    }
}

// THE FAILURE THE BAR NAMES: a translation that covers argv[0] and nothing else.
// `--assert-translated` is the guard the driver calls at the ONE point the
// child is spawned (`sqlite_units.run_one_segment`, through
// `sqlite_launch.assert_translated`), and it must see the fixture AND every file
// argument.
//
// This test needs no translator, which is why it can run on every gate leg.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): make
// `assert_translated` inspect only args[0] and the second case goes green.
TEST_F(HarnessLegs, TheLauncherArgvGuardCoversTheFixtureAndEveryFileArgument) {
    // The real shapes: a fixture, a `.test` script, and a tester.tcl flag.
    constexpr char const* kFixtureOk = "/mnt/c/out/elf64-x86_64/testfixture";
    constexpr char const* kScriptOk  = "/mnt/c/stage/test/veryquick.test";
    constexpr char const* kFixtureBad =
        "C:\\build\\out\\elf64-x86_64\\testfixture";
    constexpr char const* kScriptBad = "C:\\build\\stage\\test\\veryquick.test";

    auto assertTranslated = [&](std::vector<std::string> const& argv) {
        std::vector<std::string> args{"--path-translation", "windows-to-wsl"};
        for (auto const& a : argv) args.push_back("--assert-translated=" + a);
        return run(args);
    };

    {  // everything translated — the only shape that may spawn.
        auto const r = assertTranslated({kFixtureOk, kScriptOk, "--start=full:"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 0u)
            << "a fully-translated argv was REFUSED:\n" << r.output;
    }
    {  // argv[0] untranslated.
        auto const r = assertTranslated({kFixtureBad, kScriptOk});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an untranslated FIXTURE path was accepted:\n" << r.output;
    }
    {  // ★ THE ONE THE BAR NAMES: argv[0] fine, the file argument not.
        auto const r = assertTranslated({kFixtureOk, kScriptBad});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "the FIXTURE was translated and a FILE ARGUMENT was not, and the"
               " guard accepted it:\n"
            << r.output
            << "\nThat is exactly the half-done translation this guard exists"
               " for: the fixture would load, the first test would fail to open"
               " its script, and the failure would read as a test bug.";
        EXPECT_NE(r.output.find("veryquick.test"), std::string::npos)
            << "the refusal must NAME the offending argument:\n" << r.output;
    }
    {  // a path hiding inside a flag is still a path.
        auto const r = assertTranslated({kFixtureOk, "--testdir=D:\\scratch"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_NE(r.exitCode, 0u)
            << "an untranslated path EMBEDDED IN A FLAG was accepted:\n"
            << r.output;
    }
    {  // and a launcher that declared `none` translates nothing, by definition.
        auto const r = run({"--path-translation", "none",
                            std::string{"--assert-translated="} + kFixtureBad});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 0u)
            << "a `none` launcher refused this driver's own path spelling:\n"
            << r.output;
    }
}

// THE CELL THE ANCHOR IS ABOUT. On a Windows host the elf64 leg must be
// LAUNCHED, with a translating launcher — and when the launcher is absent the
// verdict must be the ENVIRONMENTAL skip, not the structural one, because
// "nobody can ever run this here" and "this machine lacks a tool" are different
// facts and only the second is actionable.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): delete the wsl.exe
// launcher entry from legs.json and both halves fail with `skipped-by-runOn`.
TEST_F(HarnessLegs, AWindowsHostLaunchesTheLinuxLegRatherThanSkippingIt) {
    struct Cell {
        char const* hostArch;
        char const* leg;
    };
    // WSL runs the HOST's architecture: an x86_64 Windows box reaches the
    // x86_64 Linux leg, an arm64 one the arm64 leg.
    constexpr Cell kCells[] = {{"x86_64", "elf64-x86_64"},
                               {"arm64", "elf64-arm64"}};
    for (auto const& cell : kCells) {
        auto const r = planShape({"--plan", "--host-os", "windows", "--host-arch",
                            cell.hostArch, "--launchers-available", "wsl.exe",
                            "--format", "json"});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const plan  = json::parse(r.output);
        bool       found = false;
        for (auto const& leg : plan.at("legs")) {
            if (leg.at("label").get<std::string>() != cell.leg) continue;
            found = true;
            auto const& runPlan = leg.at("run");
            EXPECT_EQ(runPlan.at("mode").get<std::string>(), "launched")
                << "windows/" << cell.hostArch << ' ' << cell.leg
                << " is not launched: " << runPlan.at("detail")
                << "\nNo WSL launcher is declared for ELF on Windows: this driver"
                   " BUILDS this leg on this host and the artefact has been"
                   " MEASURED to pass 330,436 tests under WSL. A skip beside a"
                   " working binary is a declaration gap, not a capability one.";
            ASSERT_FALSE(runPlan.at("launcher").empty());
            EXPECT_EQ(runPlan.at("launcher").at(0).get<std::string>(), "wsl.exe");
            EXPECT_EQ(runPlan.at("pathTranslation").get<std::string>(),
                      "windows-to-wsl")
                << "the launcher lives in another path namespace and must say"
                   " so; a plain command entry would spawn and then fail in a"
                   " way that looks like a broken binary";
            EXPECT_FALSE(runPlan.at("pathTranslator").empty())
                << "a translating verb must resolve to the argv that performs"
                   " it, so the driver never has to name the tool";
            EXPECT_EQ(runPlan.at("envTransfer").get<std::string>(), "wslenv")
                << "the launcher does not inherit this driver's environment"
                   " either — MEASURED: SQLITE_TEST_PATTERN_LIST arrives EMPTY,"
                   " and the resume engine then re-runs the whole corpus";
        }
        EXPECT_TRUE(found) << cell.leg << " is not in the plan at all";

        // Launcher absent -> ENVIRONMENTAL, and it must name what is missing.
        auto const none = planShape({"--plan", "--host-os", "windows", "--host-arch",
                               cell.hostArch, "--launchers-none", "--format",
                               "json"});
        ASSERT_TRUE(none.spawned) << none.diagnostic;
        ASSERT_EQ(none.exitCode, 0u) << none.output;
        // ★ The parsed document is NAMED, not a temporary iterated in place:
        // this file is C++23 but the gate compiles with GCC 13.2, which predates
        // P2718R0's lifetime extension for range-for temporaries — so
        // `for (x : json::parse(s).at("legs"))` iterates a DESTROYED object and
        // silently yields nothing, which an EXPECT-only loop reads as a pass.
        // MEASURED in this cycle: the first draft of the Wine test did exactly
        // that and failed with "pe64-x86_64 is not in the plan".
        auto const  nonePlan  = json::parse(none.output);
        bool        sawNoneCell = false;
        for (auto const& leg : nonePlan.at("legs")) {
            if (leg.at("label").get<std::string>() != cell.leg) continue;
            sawNoneCell = true;
            EXPECT_EQ(leg.at("run").at("verdict").get<std::string>(),
                      armVerdictName(ArmVerdict::SkippedEmulatorMissing))
                << "windows/" << cell.hostArch << ' ' << cell.leg
                << " without wsl.exe must be the ENVIRONMENTAL skip — a"
                   " structural one would say no Windows host can ever run it,"
                   " which is false and which DSS_STRICT_ARM_VERDICTS could not"
                   " act on.";
        }
        EXPECT_TRUE(sawNoneCell)
            << cell.leg << " is absent from the launcher-less plan, so the"
                           " environmental-skip half of this test compared"
                           " nothing";
    }
}

// WINE IS UNCHANGED. It is the one launcher that was already working, it takes
// the driver's own paths, and the whole mechanism above must not have moved it.
//
// RED-ON-DISABLE: give the Wine launchers a translating verb and this fails on
// both the declaration and the resolved plan.
TEST_F(HarnessLegs, WineStillTakesTheDriversOwnPathsUnchanged) {
    auto const doc = json::parse(fileText(catalogue_));
    std::size_t wineEntries = 0;
    for (auto const& leg : doc.at("legs")) {
        for (auto const& entry : leg.at("launchers")) {
            if (entry.at("command").at(0).get<std::string>() != "wine") continue;
            ++wineEntries;
            EXPECT_EQ(entry.at("pathTranslation").get<std::string>(), "none")
                << "Wine takes a unix path on a unix host — it shares this"
                   " driver's namespace, and translating for it would break the"
                   " one launcher that already worked.";
            EXPECT_EQ(entry.at("envTransfer").get<std::string>(), "inherit")
                << "a Wine child is an ordinary process of this host and gets"
                   " this driver's environment block; carrying it would be"
                   " machinery for nothing.";
        }
    }
    ASSERT_GE(wineEntries, 1u) << "no Wine launcher to check — vacuous";

    auto const r = planShape({"--plan", "--host-os", "linux", "--host-arch", "x86_64",
                        "--launchers-available", "wine,qemu-aarch64", "--format",
                        "json"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    bool       checked = false;
    auto const plan    = json::parse(r.output);  // NAMED — see the note above
    for (auto const& leg : plan.at("legs")) {
        if (leg.at("label").get<std::string>() != "pe64-x86_64") continue;
        checked = true;
        auto const& runPlan = leg.at("run");
        EXPECT_EQ(runPlan.at("mode").get<std::string>(), "launched");
        ASSERT_FALSE(runPlan.at("launcher").empty());
        EXPECT_EQ(runPlan.at("launcher").at(0).get<std::string>(), "wine");
        EXPECT_EQ(runPlan.at("pathTranslation").get<std::string>(), "none");
        EXPECT_TRUE(runPlan.at("pathTranslator").empty())
            << "a `none` launcher resolved a translator argv — the driver"
               " would then spawn one per path for no reason";
        EXPECT_EQ(runPlan.at("envTransfer").get<std::string>(), "inherit");
    }
    EXPECT_TRUE(checked) << "pe64-x86_64 is not in the plan";
}

// THE FIXTURE AND EVERY SEGMENT SCRIPT, TRANSLATED. A translation landing in one
// half only was the recurring capability-pair defect of the twin drivers — it is
// what let the .sh build its pe64 leg against a POSIX zconf.h for a whole cycle —
// and with ONE driver the same defect is a translation that covers some of the
// arguments a launched fixture receives and not the others.
//
// The pins are structural because the alternative is running a corpus, and each
// one names a shape that would silently half-work:
//   · the FIXTURE must be translated — otherwise the launcher cannot even be
//     handed the binary;
//   · EVERY segment record's script argument must be translated — the "argv[0]
//     only" failure, caught at the construction site rather than at run time;
//   · the names those records carry must be BOUND to the real translation call,
//     so an intermediary cannot quietly become a passthrough;
//   · the spawn-point guard must be called, because translating at construction
//     is only safe with a net;
//   · and the translation must be ASKED OF THE RESOLVER (`--translate-path`),
//     not hand-rolled, so there is one implementation.
// The property lives in TWO modules of the Python driver: `sqlite_units.py`
// builds the fixture path and the segment queue and spawns each segment;
// `sqlite_launch.py` is the door (`launch_path`, `assert_translated`, the
// environment carrier).
//
// RED-ON-DISABLE: build a segment whose script is not translated, bind a script
// name to anything but `launch_path(...)`, or drop the spawn-point
// `assert_translated` call, and this fails naming the file and the line.
TEST_F(HarnessLegs, BothDriversTranslateTheFixtureAndEverySegmentScript) {
    auto const units  = harnessDir() / "sqlite_units.py";
    auto const launch = harnessDir() / "sqlite_launch.py";
    for (auto const& p : {units, launch}) ASSERT_TRUE(fs::exists(p)) << p;
    PyInspector::instance().prefetch({units, launch});
    auto const& u = pythonCode(units);
    auto const& l = pythonCode(launch);

    // THE NAMES THE TRANSLATION IS BOUND TO. The .sh stored each translated script
    // in a `LAUNCH_*` variable (never `$(launch_path …)` inline: `die` inside a
    // command substitution exits only the subshell) and the .ps1 routed segments
    // through `Get-SegmentArgs`; the Python driver binds `launch_bin`,
    // `tier_script` and `perm_script` to `launch_path(...)` ONCE, before the queue
    // exists. A name counts only when its own binding statement CALLS the
    // translator, so an intermediary that stopped translating is not one.
    std::set<std::string> translated;
    for (auto const& s : u.statements) {
        auto const t  = trimmedLeft(s.text);
        auto const eq = t.find('=');
        if (eq == std::string::npos || eq + 1 >= t.size() || t[eq + 1] == '=') continue;
        auto const name = trimmedBoth(t.substr(0, eq));
        bool const identifier =
            !name.empty() && std::isdigit(static_cast<unsigned char>(name[0])) == 0
            && std::all_of(name.begin(), name.end(), [](char c) {
                   return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
               });
        if (identifier && t.find("launch_path(", eq) != std::string::npos) {
            translated.insert(name);
        }
    }
    EXPECT_EQ(translated.count("launch_bin"), 1u)
        << "sqlite_units.py never binds `launch_bin` to launch_path(...) — the FIXTURE"
           " path is not translated. A launcher in another namespace cannot even be"
           " handed the binary.";

    // THE RECORD'S SHAPE, READ FROM ITS DEFINITION rather than typed here: which
    // field is the script is a fact about `Segment`, so a reordered namedtuple
    // moves the index this pin reads instead of silently checking another field.
    std::size_t scriptField = std::string::npos;
    for (auto const& s : u.statements) {
        auto const at = s.text.find("namedtuple(\"Segment\"");
        if (at == std::string::npos) continue;
        auto const def = callArguments(s.text, s.text.find('(', at));
        if (def.size() < 2) break;
        std::size_t field = 0;
        for (auto q = def[1].find('"'); q != std::string::npos; q = def[1].find('"', q + 1)) {
            auto const end = def[1].find('"', q + 1);
            if (end == std::string::npos) break;
            if (def[1].substr(q + 1, end - q - 1) == "script") scriptField = field;
            ++field;
            q = end;
        }
    }
    ASSERT_NE(scriptField, std::string::npos)
        << "sqlite_units.py no longer defines `Segment = collections.namedtuple("
           "\"Segment\", [...])` with a \"script\" field, so this pin cannot tell which"
           " argument of a segment record is the script. That is a changed structure,"
           " not a passing test.";

    // EVERY RECORD. Each `Segment(...)` construction — one STATEMENT, however many
    // lines it spans — must hand the script field a translated name: positionally
    // at `scriptField`, or as `script=`.
    std::size_t records = 0;
    for (auto const& s : u.statements) {
        for (auto at = s.text.find("Segment("); at != std::string::npos;
             at = s.text.find("Segment(", at + 1)) {
            if (at > 0 && (std::isalnum(static_cast<unsigned char>(s.text[at - 1])) != 0
                           || s.text[at - 1] == '_')) {
                continue;   // another name ending in `Segment`
            }
            ++records;
            std::vector<std::string> positional;
            std::string              script;
            for (auto const& a : callArguments(s.text, at + std::string_view{"Segment"}.size())) {
                auto const eq = a.find('=');
                bool const keyword =
                    eq != std::string::npos && eq > 0 && eq + 1 < a.size() && a[eq + 1] != '='
                    && std::all_of(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(eq),
                                   [](char c) {
                                       return std::isalnum(static_cast<unsigned char>(c)) != 0
                                           || c == '_';
                                   });
                if (!keyword) {
                    positional.push_back(a);
                } else if (a.substr(0, eq) == "script") {
                    script = trimmedBoth(a.substr(eq + 1));
                }
            }
            if (script.empty() && scriptField < positional.size()) {
                script = positional[scriptField];
            }
            EXPECT_EQ(translated.count(script), 1u)
                << "sqlite_units.py:" << s.first
                << " builds a fixture segment whose script argument (`" << script
                << "`) is NOT a name bound to launch_path(...):\n  " << s.text
                << "\nThe first argument of every segment is the .test script the"
                   " fixture sources. Under a translating launcher an untranslated one"
                   " is opened as a RELATIVE file, missed, and reported as a test"
                   " failure rather than a harness bug.";
        }
    }
    EXPECT_GE(records, 3u)
        << "sqlite_units.py has only " << records
        << " segment record(s) — the corpus engine builds three (tier, permutation"
           " resume, tier resume), so this pin has stopped seeing them and is vacuous."
           " Fix the reading, do not delete the test.";

    // THE NET AT THE SPAWN POINT, AND THE DOOR BEHIND BOTH HALVES. The spawn
    // (`run_one_segment`) must assert EVERY argument before the child exists, and
    // each half must ASK harness_legs.py: `launch_path` through `--translate-path`,
    // `assert_translated` through `--assert-translated` — pinned INSIDE the two
    // functions, so a door that stopped asking cannot hide behind a flag spelled
    // somewhere else in the module.
    // ⚠ Deliberately NOT a blanket ban on the word `wslpath`: the recipe-DERIVATION
    // step spells a WSL path the Windows way for the manifest — the opposite
    // direction and a different mechanism — and a pin that outlawed the word would
    // be "fixed" by renaming a variable.
    EXPECT_TRUE(std::any_of(u.live.begin(), u.live.end(), [](auto const& ln) {
        return ln.second.find("assert_translated(") != std::string::npos
            && !startsWith(trimmedLeft(ln.second), "def ");
    })) << "sqlite_units.py never calls assert_translated. Translating at construction"
           " is only safe with a net at the spawn point: a future segment kind that"
           " adds a path argument must be refused by name, not discovered three hours"
           " in.";
    EXPECT_NE(functionBody(l.statements, "launch_path").find("--translate-path"),
              std::string::npos)
        << "sqlite_launch.launch_path no longer asks harness_legs.py to translate"
           " (--translate-path). The launcher translation is then implemented inside"
           " the driver — a second copy of the resolver's one.";
    EXPECT_NE(functionBody(l.statements, "assert_translated").find("--assert-translated"),
              std::string::npos)
        << "sqlite_launch.assert_translated no longer asks harness_legs.py to guard the"
           " argv (--assert-translated), so the net at the spawn point checks nothing.";

    // THE ENVIRONMENT DOOR, pinned by the flags only its call sites carry:
    // `--env-transfers` reads the vocabulary, but the RESOLUTION is
    // `--carrier-current`, and that is what a driver losing the capability
    // actually deletes.
    auto const eitherHas = [&u, &l](char const* flag) {
        auto const has = [flag](PyInspection const& in) {
            return std::any_of(in.live.begin(), in.live.end(), [flag](auto const& ln) {
                return ln.second.find(flag) != std::string::npos;
            });
        };
        return has(u) || has(l);
    };
    EXPECT_TRUE(eitherHas("--env-transfers") && eitherHas("--carrier-current"))
        << "the driver does not ask harness_legs.py how its run environment reaches a"
           " launched process (--env-transfers: " << eitherHas("--env-transfers")
        << ", --carrier-current: " << eitherHas("--carrier-current")
        << "). A launched fixture then runs with an EMPTY run environment, which does"
           " not fail — it silently changes what the corpus does.";

    // THE TWO FORWARD GROUPS, EACH ON ITS OWN CONSTRUCT. It is checked on the
    // defining line rather than "somewhere in the file": the first version of this
    // pin looked for a line naming both variables anywhere and was satisfied by a
    // RESTORE line (`$env:QUICKTEST_OMIT = $oldOmit; …`), so emptying the real list
    // stayed GREEN. Measured, and fixed then.
    // ★ TWO CONSTRUCTS SINCE TF-C124, AND THE SPLIT IS THE POINT
    // [TCL_LIBRARY was not forwarded across the WSL boundary]. The variable has to
    // cross (a leg whose Tcl was acquired cannot find init.tcl without it) and it
    // holds a HOST path, so it must cross TRANSLATED: `FORWARD_PLAIN` is the
    // NAMESPACE-NEUTRAL group and TCL_LIBRARY is banned from it; `FORWARD_PATHS`
    // is the DRIVER-PATH group and TCL_LIBRARY must be there. Deleting the path
    // group to satisfy the ban reds the second assertion.
    bool forwardsThePatternList = false, forwardsTclLibraryAsAPath = false;
    for (auto const& [n, line] : l.live) {
        if (line.find("FORWARD_PATHS = ") != std::string::npos
            && line.find("TCL_LIBRARY") != std::string::npos) {
            forwardsTclLibraryAsAPath = true;
        }
        if (line.find("FORWARD_PLAIN = ") == std::string::npos) continue;
        if (line.find("SQLITE_TEST_PATTERN_LIST") != std::string::npos
            && line.find("QUICKTEST_OMIT") != std::string::npos) {
            forwardsThePatternList = true;
        }
        // NAMESPACE-NEUTRAL VALUES ONLY *IN THIS GROUP*. A HOST path named here
        // would cross verbatim: the child gets a path it cannot resolve and uses
        // it anyway. TCL_LIBRARY belongs in the path group, and so does any
        // `*PATH` list (the twins' needle was the literal ending `PATH'`).
        EXPECT_EQ(line.find("TCL_LIBRARY"), std::string::npos)
            << "sqlite_launch.py:" << n << " names a HOST PATH in its NAMESPACE-NEUTRAL"
            << " forward group, where it would cross untranslated:\n  " << line;
        EXPECT_TRUE(line.find("PATH\"") == std::string::npos
                    && line.find("PATH'") == std::string::npos)
            << "sqlite_launch.py:" << n << " forwards a host PATH list into the"
            << " launcher's environment:\n  " << line;
    }
    EXPECT_TRUE(forwardsThePatternList)
        << "sqlite_launch.py's FORWARD_PLAIN does not name both SQLITE_TEST_PATTERN_LIST"
           " and QUICKTEST_OMIT. The first is how the RESUME ENGINE selects its files:"
           " MEASURED 2026-08-04, a launched fixture that could not see it re-ran the"
           " corpus from the beginning after an abort and reported it as progress.";
    EXPECT_TRUE(forwardsTclLibraryAsAPath)
        << "sqlite_launch.py's DRIVER-PATH forward group (FORWARD_PATHS) does not name"
           " TCL_LIBRARY. A leg whose Tcl came from acquisition cannot find init.tcl"
           " without it, and the failure is reported against the acquisition rather"
           " than the boundary: TCL_LIBRARY is not forwarded across the WSL boundary.";
    EXPECT_TRUE(eitherHas("--forward-path"))
        << "the driver never spells --forward-path, so whatever it names in its"
           " driver-path group crosses UNTRANSLATED — a Windows path handed to a Linux"
           " process, which does not fail as a path error.";

    // ★ AND BOTH GROUPS REACH THE CARRIER. A group that is defined and never handed
    // to `carrier_assignments` carries nothing, with both definitions above green.
    bool bothGroupsCarried = false;
    for (auto const& s : l.statements) {
        auto const at = s.text.find("carrier_assignments(");
        if (at == std::string::npos || startsWith(trimmedLeft(s.text), "def ")) continue;
        auto const args =
            callArguments(s.text, at + std::string_view{"carrier_assignments"}.size());
        bool const plain = std::find(args.begin(), args.end(), "FORWARD_PLAIN") != args.end();
        bool const paths = std::find(args.begin(), args.end(), "FORWARD_PATHS") != args.end();
        bothGroupsCarried = bothGroupsCarried || (plain && paths);
    }
    EXPECT_TRUE(bothGroupsCarried)
        << "no call of carrier_assignments in sqlite_launch.py hands it BOTH"
           " FORWARD_PLAIN and FORWARD_PATHS, so a forward group is defined and never"
           " carried.";
}

// A SUMMARY LINE IS NOT PROOF THAT A SUITE RAN. The driver must refuse a
// segment run that completed ZERO test files, whatever tester.tcl printed.
//
// MEASURED 2026-08-04 (TF-C116) and it is why this pin exists: an environment
// variable that arrived EMPTY-BUT-SET through a cross-OS launcher made the tier
// select no files at all; tester.tcl finalised and printed `0 errors out of 1
// tests`; the driver reported "corpus GREEN — 0 errors out of 1 tests" beside
// "0 test file(s) completed". A false pass is the worst outcome this harness can
// produce, and the floor belongs in the verdict ladder rather than beside the
// one cause that happened to expose it.
//
// The ladder is `sqlite_units.judge_leg`; the floor is its `files_done == 0`
// branch, and the branch must FAIL the leg: the twins' needle asked only that the
// branch EXIST, and a branch that exists and says PASS satisfied it.
//
// RED-ON-DISABLE: delete the zero-files-completed branch, or make it anything but
// a FAIL verdict, and this fails naming it.
TEST_F(HarnessLegs, NeitherDriverCallsAZeroFileRunGreen) {
    auto const units = harnessDir() / "sqlite_units.py";
    ASSERT_TRUE(fs::exists(units)) << units;
    auto const& u = pythonCode(units);
    bool hasFloor = false, floorFails = false;
    for (std::size_t i = 0; i < u.statements.size(); ++i) {
        auto const t = trimmedLeft(u.statements[i].text);
        if (!startsWith(t, "if ") && !startsWith(t, "elif ")) continue;
        if (t.find("files_done == 0") == std::string::npos) continue;
        hasFloor = true;
        for (auto const& body : blockOf(u.statements, i)) {
            floorFails = floorFails || body.text.find("\"FAIL:") != std::string::npos;
        }
    }
    EXPECT_TRUE(hasFloor)
        << "sqlite_units.py has no verdict branch testing `files_done == 0`. Without it"
           " a run that executed NOTHING is reported GREEN on the strength of"
           " tester.tcl's summary line — measured 2026-08-04, `corpus GREEN — 0 errors"
           " out of 1 tests` beside `0 test file(s) completed`.";
    EXPECT_TRUE(floorFails)
        << "sqlite_units.py's `files_done == 0` branch does not record a `FAIL:`"
           " verdict — the floor exists and no longer fails the leg.";
}

// A CRASH THAT SAYS NOTHING MUST NOT COST A LEG ITS WHOLE RESUME BUDGET.
//
// THE PRECONDITION DISCRIMINATOR WAS BLIND TO A SILENT CRASH.
//
// ✔MEASURED 2026-08-10, ONE Windows run, TWO legs, same commit, same root cause,
// and the A/B is the whole argument for this pin:
//   · elf64-arm64 ran under qemu, which PRINTS `qemu: uncaught target signal 11
//     (Segmentation fault) - core dumped`. The precondition discriminator fired on
//     the second zero-file segment and the remaining resume budget was NOT spent.
//   · elf64-x86_64 ran natively and died SILENTLY: every `corpus*.log` was 0 bytes
//     and every facts file carried only N/D/M/K/Q — no `A` fact at all. The
//     discriminator also required a NON-EMPTY first diagnostic, so it could never
//     be satisfied: the leg burned all 10 resumes and reported `11 fixture
//     ABORT(s)` with every abort unnameable and 12 unit groups NOT REACHED.
// THE LEG WHOSE CRASH TALKS WAS HANDLED; THE LEG WHOSE CRASH IS SILENT WAS NOT.
//
// WHAT THIS PIN ADDS OVER THE SELF-TESTS, which is why it is here as well as
// there: `sqlite_corpus.py --self-test` drives the signature and the
// discriminator BEHAVIOURALLY (its zero-progress cases are the answers the twin
// drivers agreed on before they were deleted), but only when something runs it.
// This is the always-runs STRUCTURAL half, over the two modules that carry the
// capability: `sqlite_corpus.py` defines the signature and the discriminator,
// `sqlite_units.py` is the resume engine that consults them and carries the
// signature from one segment to the next.
//
// RED-ON-DISABLE: delete the signature helper, test the raw first diagnostic
// instead of the signature, carry anything but the signature, or spell the
// sentinel a second time anywhere in the driver, and this fails naming the file.
TEST_F(HarnessLegs, NeitherDriverSpendsItsBudgetOnASilentCrash) {
    // The sentinel is ASCII on purpose: it is the one value two consecutive
    // silent segments are compared on, and it is printed into the run log, so an
    // encoding question must never sit inside it. With one driver the twins'
    // byte-identity reason is gone; the ONE-COPY rule is not.
    constexpr char const* kSentinel =
        "<SILENT: the fixture produced no diagnostic, no test result and no test "
        "name>";
    auto const corpus = harnessDir() / "sqlite_corpus.py";
    auto const units  = harnessDir() / "sqlite_units.py";
    for (auto const& p : {corpus, units}) ASSERT_TRUE(fs::exists(p)) << p;
    PyInspector::instance().prefetch({corpus, units});
    auto const& c = pythonCode(corpus);
    auto const& u = pythonCode(units);
    auto const anyLive = [](PyInspection const& in, auto const& pred) {
        return std::any_of(in.live.begin(), in.live.end(),
                           [&pred](auto const& ln) { return pred(ln.second); });
    };

    EXPECT_TRUE(anyLive(c, [](std::string const& l) {
        return startsWith(l, "def zero_progress_signature(");
    })) << "sqlite_corpus.py does not define `zero_progress_signature` at column 0."
           " Without it the discriminator has nothing to compare for a segment that"
           " produced no output, and a fixture that never started consumes the whole"
           " resume budget one silent segment at a time.";

    // THE DISCRIMINATOR TESTS THE SIGNATURE: `is_precondition_failure` computes it
    // and requires it NON-EMPTY (`sig != ""`). Testing the raw first diagnostic
    // instead is the defect: a fixture that writes 0 bytes has no diagnostic, so
    // the condition is unsatisfiable and the branch is dead for exactly the crash
    // that needs it most.
    auto const discriminator = functionBody(c.statements, "is_precondition_failure");
    EXPECT_TRUE(discriminator.find("zero_progress_signature(") != std::string::npos
                && discriminator.find("sig != \"\"") != std::string::npos)
        << "sqlite_corpus.is_precondition_failure no longer tests the zero-progress"
           " SIGNATURE (looked for a call of zero_progress_signature and `sig != \"\"`"
           " in its body):\n" << discriminator;
    EXPECT_TRUE(anyLive(u, [](std::string const& l) {
        return l.find("is_precondition_failure(prev_zero_sig") != std::string::npos;
    })) << "sqlite_units.py's resume engine never consults is_precondition_failure"
           " with the CARRIED signature, so the discriminator decides nothing.";

    // …AND THE CARRY STORES THE SIGNATURE. A condition that is right about one
    // segment decides nothing if what the next one compares against is the empty
    // string.
    EXPECT_TRUE(anyLive(u, [](std::string const& l) {
        return l.find("zero_sig = ") != std::string::npos
            && l.find("zero_progress_signature(") != std::string::npos;
    }) && anyLive(u, [](std::string const& l) {
        return l.find("prev_zero_sig = zero_sig") != std::string::npos;
    })) << "sqlite_units.py never carries the SIGNATURE to the next segment (looked for"
           " `zero_sig = …zero_progress_signature(…)` and `prev_zero_sig = zero_sig`).";

    // ONE occurrence across the WHOLE driver: the value lives in its one constant
    // (`sqlite_corpus.SILENT_SENTINEL`) and nowhere else, so a second copy cannot
    // drift away from the first. (The module's own self-test assembles its expected
    // value from two halves for exactly this reason.)
    std::size_t sentinels = 0;
    std::string where;
    auto const  modules = driverModules();
    auto const  missing = missingDriverModules(modules);
    ASSERT_TRUE(missing.empty()) << "driver modules missing: " << missing;
    for (auto const& module : modules) {
        for (auto const& [n, line] : pythonCode(module).live) {
            if (line.find(kSentinel) == std::string::npos) continue;
            ++sentinels;
            where += "\n  " + module.filename().string() + ':' + std::to_string(n) + "  "
                   + line;
        }
    }
    EXPECT_EQ(sentinels, 1u)
        << "the driver spells the silent-crash sentinel " << sentinels
        << " time(s); expected exactly 1 (sqlite_corpus.SILENT_SENTINEL). Zero means"
           " the driver cannot sign a silent segment at all; two means a second copy"
           " that can drift from the first:" << where;
}

// ── 8. The declared library-acquisition route ──────────────────────────────
//
// LIBRARY ACQUISITION WAS BUILT FOR ONE LEG, IN ONE DRIVER. Operator
// principle, 2026-08-04: "we should be able to build macho on linux. ANY LEG
// MUST BE ABLE TO BUILD TO ANY LEG."
//
// The catalogue already says every leg is BUILT on every host and only the RUN
// is gated (§2/§3 above), so a leg whose libraries exist only on one kind of
// machine made that promise false in practice: before this, both macho legs
// were `host-system`, which off a Mac means "hope this box has a Darwin
// libtcl". The mechanism was never missing — `build-and-test.sh` had downloaded
// Ubuntu ports `.deb`s for the arm64 leg since TF-C68 — what was missing was
// GENERALITY (one hand-written provider serving one leg) and a SECOND
// IMPLEMENTATION (`build-and-test.ps1` could not acquire at all).
//
// These pins hold the general form to account: the route is DECLARED, it is
// CHECKSUM-PINNED, it REFUSES rather than improvising, the identity an acquired
// stand-in is recorded under is DECLARED rather than inherited from whoever
// packaged it, and — the anti-regression pin that matters most — the driver
// implements every provider the catalogue declares (`sqlite_libs.resolve_leg`,
// the ONE dispatch since the twins were retired on 2026-09-21).

namespace {

// Every provider name the catalogue actually uses.
[[nodiscard]] std::set<std::string> declaredProviders(fs::path const& catalogue) {
    std::set<std::string> out;
    auto const doc = json::parse(fileText(catalogue));
    for (auto const& leg : doc.at("legs")) {
        out.insert(leg.at("build").at("libraries").at("provider")
                       .get<std::string>());
    }
    return out;
}

// The label of the first leg declaring the acquisition route, or "".
[[nodiscard]] std::string firstAcquiringLeg(fs::path const& catalogue) {
    auto const doc = json::parse(fileText(catalogue));
    for (auto const& leg : doc.at("legs")) {
        if (leg.at("build").at("libraries").at("provider").get<std::string>()
            == "pinned-archive") {
            return leg.at("label").get<std::string>();
        }
    }
    return {};
}

}  // namespace

// AN ACQUIRED LIBRARY IS A STAND-IN, AND ITS EMBEDDED IDENTITY IS THE
// PACKAGER'S, NOT THE TARGET'S.
//
// MEASURED 2026-08-04 on this host, and it is the whole reason the key exists:
// the MacPorts Tcl/zlib dylibs carry `LC_ID_DYLIB = /opt/local/lib/...`, DSS
// records a resolved library's embedded identity as the LC_LOAD_DYLIB, and a
// Mach-O cross-built against them came out demanding
// `/opt/local/lib/libtcl8.6.dylib`. That is a dyld LOAD failure on the target
// Mac — not a build error, and not something this host can observe. So the
// identity to record is DECLARED per acquired member, and the plan carries it
// to the driver so the driver never has to decide it.
//
// RED-ON-DISABLE (measured, numbers in the cycle report): remove an
// `importName` from any acquired member and the lint refuses the catalogue —
// asserted here directly rather than described.
TEST_F(HarnessLegs, AnAcquiredLibraryDeclaresTheIdentityItIsRecordedUnder) {
    auto const r = planShape({"--plan", "--host-os", "linux", "--host-arch", "x86_64"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    auto const plan = json::parse(r.output);
    unsigned acquiring = 0;
    for (auto const& leg : plan.at("legs")) {
        auto const  label    = leg.at("label").get<std::string>();
        auto const& libs     = leg.at("build").at("libraries");
        auto const  provider = libs.at("provider").get<std::string>();
        auto const  tcl      = libs.value("tclImportName", std::string{});
        auto const  z        = libs.value("zImportName", std::string{});
        if (provider == "pinned-archive") {
            ++acquiring;
            EXPECT_FALSE(tcl.empty())
                << label << " acquires its Tcl but declares no identity to"
                            " record it under. The downloaded file's own"
                            " LC_ID_DYLIB/DT_SONAME belongs to whoever packaged"
                            " it, so inheriting it bakes that packager's prefix"
                            " into the artefact and fails at LOAD time on the"
                            " target machine.";
            EXPECT_FALSE(z.empty()) << label << " (zlib): same.";
        } else {
            EXPECT_TRUE(tcl.empty() && z.empty())
                << label << " uses provider '" << provider << "', which hands"
                            " over a library already carrying the right embedded"
                            " identity — overriding it would replace a true name"
                            " with a declared one.";
        }
    }
    EXPECT_GT(acquiring, 0u)
        << "no leg declares the acquisition route, so this test asserts nothing."
           " If the last acquiring leg was deliberately removed, remove this pin"
           " in the same commit rather than leaving it vacuous.";

    // The refusal, witnessed rather than asserted about.
    MutatedCatalogue m{catalogue_, "legs-no-import-name"};
    bool stripped = false;
    for (auto& leg : m.doc().at("legs")) {
        auto& libs = leg.at("build").at("libraries");
        if (libs.at("provider").get<std::string>() != "pinned-archive") continue;
        for (auto& a : libs.at("acquire").at("archives")) {
            for (auto& mem : a.at("members")) {
                mem.erase("importName");
                stripped = true;
            }
        }
        break;
    }
    ASSERT_TRUE(stripped) << "nothing to strip — the mutation is vacuous";
    auto const bad = runResolver({"--lint"}, m.commit());
    ASSERT_TRUE(bad.spawned) << bad.diagnostic;
    EXPECT_NE(bad.exitCode, 0u)
        << "the lint accepted an acquired member with no declared identity:\n"
        << bad.output;
    EXPECT_NE(bad.output.find("importName"), std::string::npos) << bad.output;
}

// A LIBRARY IS NOT ALWAYS SELF-CONTAINED: AN ACQUIRED Tcl DYLIB SHIPS NO
// SCRIPT LIBRARY.
//
// The macho leg's testfixture BUILT (189 TUs, 0 diagnostics) and ran an
// individual `.test` file correctly, and then the TIER driver died instantly at
// `interp create` because `permutations.test` runs every unit in a fresh SLAVE
// interpreter and `tclInit` needs Tcl's SCRIPT LIBRARY — which acquisition had
// never obtained. Nothing at build time could see it.
//
// So: every leg that acquires a Tcl must ALSO stage that Tcl's scripts, and must
// SAY where, because a driver sets TCL_LIBRARY from `scriptLibraryDir` and from
// nothing else.
//
// RED-ON-DISABLE: delete a `dataDirs` entry from any acquiring leg and this
// fails naming the leg; empty `scriptLibraryDir` and it fails too.
TEST_F(HarnessLegs, EveryAcquiredTclStagesItsScriptLibraryAndSaysWhere) {
    auto const doc      = json::parse(fileText(catalogue_));
    unsigned   acquired = 0;
    for (auto const& leg : doc.at("legs")) {
        auto const  label = leg.at("label").get<std::string>();
        auto const& libs  = leg.at("build").at("libraries");
        if (libs.at("provider").get<std::string>() != "pinned-archive") continue;
        ++acquired;
        auto const r = run({"--acquire-plan", label});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const ap = json::parse(r.output);

        std::set<std::string> staged;
        for (auto const& a : ap.at("archives")) {
            for (auto const& m : a.at("members")) {
                // WHICH COPY RUNS is what decides whether a baked-in data
                // directory has to be staged. There is no safe default.
                auto const copy = m.at("runtimeCopy").get<std::string>();
                EXPECT_TRUE(copy == "staged-beside-artefact"
                            || copy == "target-supplies-its-own")
                    << label << " :: " << m.at("as").get<std::string>()
                    << ": runtimeCopy is '" << copy << "'";
                for (auto const& d : m.at("dataDirs")) {
                    if (d.at("role").get<std::string>() == "tclScriptLibrary")
                        staged.insert(d.at("path").get<std::string>());
                }
                // A path the LIBRARY ITSELF bakes in and calls runtime data must
                // have a staged directory answering to it. This is the exact
                // state the macho legs shipped in.
                for (auto const& e : m.at("embeddedPaths")) {
                    if (e.at("kind").get<std::string>() != "runtime-data")
                        continue;
                    bool provided = false;
                    for (auto const& d : m.at("dataDirs")) {
                        if (d.at("role") == e.at("role")) provided = true;
                    }
                    EXPECT_TRUE(provided)
                        << label << " :: " << m.at("as").get<std::string>()
                        << " bakes in " << e.at("path").get<std::string>()
                        << " as runtime data and stages nothing for it — the"
                           " library would look for it at the PACKAGER's prefix"
                           " on the target machine.";
                }
            }
        }
        EXPECT_EQ(staged.size(), 1u)
            << label << ": a leg that acquires Tcl must stage exactly ONE script"
                        " library. TCL_LIBRARY names one directory.";
        auto const said = ap.at("scriptLibraryDir").get<std::string>();
        EXPECT_FALSE(said.empty())
            << label << ": the plan stages a script library and does not say"
                        " where. A driver cannot set TCL_LIBRARY from silence.";
        EXPECT_TRUE(staged.count(said))
            << label << ": scriptLibraryDir '" << said
            << "' is not one of the directories the plan stages.";
    }
    EXPECT_GT(acquired, 0u) << "no acquiring leg inspected — vacuous";
}

// THE FAILURE RETURN CARRIES THE SAME FIELDS AS THE SUCCESS RETURN.
// The pinned-archive FAILURE return omitting `acquired` was one instance of
// this ("a function whose SUCCESS return and FAILURE return carry different
// field sets is a silent-omission generator"); its own closing note asks for ONE
// record type on both paths. `--acquire` therefore prints the record even when
// acquisition fails — with the rc still naming the failure.
//
// RED-ON-DISABLE: drop the failure-path print, or omit any field from it, and
// this fails naming the missing key.
TEST_F(HarnessLegs, AcquisitionAnswersWithTheSameRecordShapeWhenItFails) {
    auto const label = firstAcquiringLeg(catalogue_);
    ASSERT_FALSE(label.empty()) << "no acquiring leg — vacuous";
    auto const cold = scratch_->path() / "cold-record";
    ASSERT_FALSE(fs::exists(cold)) << "the cache root must be absent: " << cold;

    auto const bad = run({"--acquire", label, "--cache-root", cold.string(),
                          "--offline"});
    ASSERT_TRUE(bad.spawned) << bad.diagnostic;
    EXPECT_NE(bad.exitCode, 0u)
        << "acquisition must FAIL on a cold cache with --offline:\n"
        << bad.output;
    auto const brace = bad.output.find('{');
    ASSERT_NE(brace, std::string::npos)
        << "a failed --acquire printed no record at all. The driver needs"
           " scriptLibraryDir most when acquisition has just failed and it is"
           " reporting why:\n"
        << bad.output;
    auto const rec = json::parse(bad.output.substr(brace));
    for (char const* key : {"leg", "targetArch", "cacheDir", "scriptLibraryDir",
                            "libraries", "fromCache", "remediated",
                            "loaderDependencies", "error"}) {
        EXPECT_TRUE(rec.contains(key))
            << "the FAILURE record omits '" << key
            << "' — the exact shape of the bug this pin exists to prevent.";
    }
    EXPECT_FALSE(rec.at("error").get<std::string>().empty())
        << "a failure record with an empty `error` reads as a success.";
    EXPECT_FALSE(rec.at("scriptLibraryDir").get<std::string>().empty())
        << "scriptLibraryDir is computed from the PURE plan and must survive a"
           " failed acquisition.";
}

// THE DECLARATIONS THAT CARRY THE GUARD ARE THEMSELVES REQUIRED.
// A rule enforced only when someone remembers to declare it is not enforced.
// Each mutation below is the omission a hurried author would actually make.
TEST_F(HarnessLegs, TheLintRefusesAnUnderdeclaredRuntimeDataSurface) {
    struct Case {
        char const* name;
        char const* needle;      // must appear in the refusal
        void (*mutate)(json&, bool&);
    };
    constexpr Case kCases[] = {
        {"legs-no-runtime-copy", "runtimeCopy",
         [](json& mem, bool& did) { mem.erase("runtimeCopy"); did = true; }},
        {"legs-unstaged-script-library", "runtime-data",
         [](json& mem, bool& did) {
             // The EXACT defect: the library still declares that it bakes in a
             // script directory, and nothing stages it any more.
             if (mem.at("dataDirs").empty()) return;
             mem["dataDirs"] = json::array();
             did              = true;
         }},
        {"legs-inert-without-why", "$why",
         [](json& mem, bool& did) {
             for (auto& e : mem.at("embeddedPaths")) {
                 if (e.at("kind").get<std::string>() != "inert") continue;
                 e.erase("$why");
                 did = true;
             }
         }},
    };
    for (auto const& c : kCases) {
        MutatedCatalogue m{catalogue_, c.name};
        bool             did = false;
        for (auto& leg : m.doc().at("legs")) {
            auto& libs = leg.at("build").at("libraries");
            if (libs.at("provider").get<std::string>() != "pinned-archive")
                continue;
            for (auto& a : libs.at("acquire").at("archives")) {
                for (auto& mem : a.at("members")) c.mutate(mem, did);
            }
            if (did) break;
        }
        ASSERT_TRUE(did) << c.name << ": nothing to mutate — vacuous";
        auto const bad = runResolver({"--lint"}, m.commit());
        ASSERT_TRUE(bad.spawned) << bad.diagnostic;
        EXPECT_NE(bad.exitCode, 0u)
            << c.name << ": the lint accepted it:\n" << bad.output;
        EXPECT_NE(bad.output.find(c.needle), std::string::npos)
            << c.name << ": the refusal never mentions '" << c.needle
            << "', so a reader cannot act on it:\n"
            << bad.output;
    }
}

// A BUILD THAT FETCHES THIRD-PARTY BINARIES IS A SUPPLY-CHAIN SURFACE.
// Every archive pins a sha256, the download is filed UNDER that digest (so "is
// the cached copy the thing we pinned?" is answered by re-hashing rather than by
// trusting a file name), and the source is https.
//
// RED-ON-DISABLE: replace any `sha256` with a non-digest and the lint refuses —
// asserted here.
TEST_F(HarnessLegs, TheAcquisitionRouteIsChecksumPinnedAndContentAddressed) {
    auto const providers = declaredProviders(catalogue_);
    ASSERT_TRUE(providers.count("pinned-archive"))
        << "no leg declares 'pinned-archive'; this pin would be vacuous";
    auto const doc      = json::parse(fileText(catalogue_));
    unsigned   archives = 0;
    for (auto const& leg : doc.at("legs")) {
        auto const  label = leg.at("label").get<std::string>();
        auto const& libs  = leg.at("build").at("libraries");
        if (libs.at("provider").get<std::string>() != "pinned-archive") continue;
        auto const r = run({"--acquire-plan", label});
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        ASSERT_EQ(r.exitCode, 0u) << r.output;
        auto const  ap   = json::parse(r.output);
        auto const  spec = leg.at("spec").get<std::string>();
        std::string const specArch = spec.substr(0, spec.find(':'));
        EXPECT_EQ(ap.at("targetArch").get<std::string>(), specArch)
            << label << ": the slice taken must be the LEG's target arch, never"
                        " anything about the host doing the acquiring.";
        for (auto const& a : ap.at("archives")) {
            ++archives;
            auto const url = a.at("url").get<std::string>();
            auto const sha = a.at("sha256").get<std::string>();
            EXPECT_EQ(sha.size(), 64u) << label << " " << url;
            EXPECT_EQ(sha.find_first_not_of("0123456789abcdef"),
                      std::string::npos)
                << label << " " << url << ": sha256 is not lowercase hex";
            EXPECT_EQ(url.rfind("https://", 0), 0u)
                << label << ": " << url << " is not https. The digest"
                                           " authenticates the CONTENT; TLS"
                                           " authenticates the SOURCE.";
            EXPECT_NE(a.at("download").get<std::string>().find(sha),
                      std::string::npos)
                << label << ": the download is not filed under its digest, so a"
                            " corrupt cache entry cannot be detected by name.";
        }
    }
    EXPECT_GT(archives, 0u) << "no archives inspected — vacuous";

    MutatedCatalogue m{catalogue_, "legs-unpinned"};
    bool             unpinned = false;
    for (auto& leg : m.doc().at("legs")) {
        auto& libs = leg.at("build").at("libraries");
        if (libs.at("provider").get<std::string>() != "pinned-archive") continue;
        libs.at("acquire").at("archives").at(0)["sha256"] = "not-a-digest";
        unpinned = true;
        break;
    }
    ASSERT_TRUE(unpinned) << "nothing to unpin — the mutation is vacuous";
    auto const bad = runResolver({"--lint"}, m.commit());
    ASSERT_TRUE(bad.spawned) << bad.diagnostic;
    EXPECT_NE(bad.exitCode, 0u)
        << "the lint accepted an archive with no pinned digest:\n" << bad.output;
}

// THE CAPABILITY PIN — the half of this anchor that is about the DRIVER.
//
// `build-and-test.ps1` used to end its provider switch with "library provider
// '$provider' is NOT IMPLEMENTED by build-and-test.ps1", which made the one
// working provider Linux-driver-only. This project's rule was that a capability
// in one driver and not the other is a SILENT harness bug, and that shape cost
// three cycles before the twins were replaced by ONE Python driver on 2026-09-21.
//
// The check is deliberately STRUCTURAL — a provider must appear as a real ARM of
// the driver's provider dispatch, not merely as a word somewhere in the file.
// Naming it inside a "not implemented" message would otherwise satisfy a naive
// substring search, which is exactly the state being repaired. The dispatch is
// `sqlite_libs.resolve_leg`'s `if provider == "…":` / `elif provider == "…":`
// chain, so an ARM is a live `if`/`elif` line testing `provider == "<name>"` —
// the Python spelling of the twins' `<name>)` case arm and `'<name>' {` switch arm.
//
// ⓘ THE DRIVER-LOCAL EXEMPTION LIST IS RETIRED WITH ITS SUBJECT. It named
// providers implemented in ONE twin and not the other; it held exactly one entry,
// `ubuntu-ports-arm64` (~80 lines of `curl` + `dpkg-deb` inside the .sh and
// nowhere else), retired itself when TF-C123 converted that leg to the shared
// `pinned-archive` route, and sat at ZERO from then on. With one driver there is
// no "other" driver for a provider to be missing from, so what it guarded is now
// the unconditional assertion below: EVERY declared provider has an arm, with no
// list anyone could add an escape to.

namespace {

// Does a live line of the dispatch open an ARM for `provider` — an `if`/`elif`
// testing `provider == "<name>"`? A mention anywhere else (a message, a log
// line) is not an arm.
[[nodiscard]] bool dispatchesProvider(std::vector<std::string> const& lines,
                                      std::string const&              provider) {
    std::string const needle = "provider == \"" + provider + "\"";
    return std::any_of(lines.begin(), lines.end(), [&needle](std::string const& line) {
        auto const t = trimmedLeft(line);
        return (startsWith(t, "if ") || startsWith(t, "elif "))
            && t.find(needle) != std::string::npos;
    });
}

}  // namespace

// RED-ON-DISABLE: delete the driver's arm for any declared provider and this
// fails naming the provider.
TEST_F(HarnessLegs, BothDriversImplementEveryProviderTheCatalogueDeclares) {
    auto const providers = declaredProviders(catalogue_);
    // ⚠ THIS USED TO DEMAND TWO OR MORE PROVIDERS ("one provider — the pin proves
    // nothing"), and TF-C123 made that reasoning obsolete rather than merely
    // inconvenient: converging on a single shared route is the GOAL, not a
    // vacuity. Every provider the catalogue declares — all one of them — must
    // have a real dispatch arm, and that is the strongest form this pin has had.
    ASSERT_GE(providers.size(), 1u) << "no provider declared at all";
    auto const libs = harnessDir() / "sqlite_libs.py";
    ASSERT_TRUE(fs::exists(libs)) << libs;
    auto const lines = liveLines(libs, Dialect::Python);
    ASSERT_FALSE(lines.empty()) << libs;
    for (auto const& p : providers) {
        EXPECT_TRUE(dispatchesProvider(lines, p))
            << "sqlite_libs.py has no dispatch arm (`if`/`elif provider == \"" << p
            << "\":`) for library provider '" << p
            << "', which the catalogue DECLARES. A leg is then declared buildable and"
               " cannot be built — library acquisition built for one leg in one"
               " driver, the defect this pin exists to end.";
    }
}

// ── THE OTHER DIRECTION OF THE SAME CONTRACT ────────────────────────────────
//
// `BothDriversImplementEveryProviderTheCatalogueDeclares` walks from the
// DECLARATION to the driver. Nothing walked the other way, and a whole provider
// survived ten days in the gap (`3e86a187`, 2026-08-10, to 2026-08-20):
// `ubuntu-ports-arm64` was retired from
// `LIBRARY_PROVIDERS` when the elf64-arm64 leg moved to `pinned-archive`, but
// build-and-test.sh kept its `case` arm, its ~45-line `ensure_arm64_libs`
// acquisition and its `ARM64_LIBDIR` override, and BOTH twins went on naming it
// as KNOWN in the very message whose job is to tell a reader what is accepted.
// build-and-test.ps1's read "Exactly one DECLARED provider still lands in this
// arm: ubuntu-ports-arm64" — false in both halves, and it cited a CLOSED row as
// live. A reader who believed either would have written a declaration
// `harness_legs.py --lint` refuses outright.
// THE UBUNTU-PORTS PROVIDER WAS NOT GENERALISED TO THE PINNED ARCHIVE.
//
// The existing pin could not see any of it: every one of those sites is a
// provider the catalogue does NOT declare, so its loop never looked at the name.
//
// ★ WHY THE RULE IS ABOUT *LIVE* LINES AND NOT ABOUT MENTIONS. A comment
// narrating a retirement — including the one directly above — is exactly what a
// reader needs, and banning the word would delete the history along with the
// defect. What must not survive is a live line: a dispatch arm, or a diagnostic
// that advertises the name to someone deciding what to declare. `liveLines`
// draws that line already, and the same helper the forward pin uses draws it the
// same way, so the two cannot disagree about what counts as code.
[[nodiscard]] std::set<std::string> retiredLibraryProviders() {
    // Names that were once dispatchable and are now REFUSED by the resolver.
    // Self-retiring: if one of these ever re-enters the vocabulary, the test
    // below reds demanding the entry be deleted rather than silently permitting
    // the name again.
    return {"ubuntu-ports-arm64"};
}

// RED-ON-DISABLE: put `elif provider == "ubuntu-ports-arm64":` back into the
// dispatch, or name it in any live line of the driver (its unknown-provider
// refusal included), and this fails naming the module, the provider and the line.
TEST_F(HarnessLegs, NeitherDriverKeepsAProviderTheResolverNoLongerKnows) {
    auto const r = run({"--library-providers"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    std::set<std::string> vocabulary;
    {
        std::istringstream in{r.output};
        std::string        line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) vocabulary.insert(line);
        }
    }
    // The verb is the whole basis of this test, so a collapsed read must fail
    // rather than vacuously pass: an empty vocabulary would satisfy every loop
    // below without checking one thing.
    ASSERT_FALSE(vocabulary.empty())
        << "harness_legs.py --library-providers printed no provider names, so"
           " this pin has nothing to check. It is the OWNER of the closed set"
           " LIBRARY_PROVIDERS — an empty print means the verb broke, not that"
           " the vocabulary is empty.\n"
        << r.output;

    // Every DECLARED provider must be in the vocabulary, or the shipped
    // catalogue would not lint — stated here because it is what makes the
    // vocabulary the right set for the driver checks below.
    for (auto const& p : declaredProviders(catalogue_)) {
        EXPECT_TRUE(vocabulary.count(p))
            << "the catalogue declares provider '" << p << "', which"
               " LIBRARY_PROVIDERS does not contain.";
    }

    // (a) EVERY name in the vocabulary is dispatchable — the forward pin only
    //     demands this of the providers the catalogue currently declares, so a
    //     vocabulary entry no leg happens to use today could be added to the
    //     resolver and not to the driver, and nothing would notice until a leg
    //     declared it.
    auto const libs = harnessDir() / "sqlite_libs.py";
    ASSERT_TRUE(fs::exists(libs)) << libs;
    auto const dispatch = liveLines(libs, Dialect::Python);
    for (auto const& p : vocabulary) {
        EXPECT_TRUE(dispatchesProvider(dispatch, p))
            << "sqlite_libs.py has no dispatch arm for library provider '" << p
            << "', which harness_legs.py --library-providers lists as KNOWN. A leg"
               " declaring it would lint clean and then not be buildable.";
    }
    // (b) NO retired name survives in a live line of ANY driver module.
    auto const modules = driverModules();
    auto const missing = missingDriverModules(modules);
    ASSERT_TRUE(missing.empty()) << "driver modules missing: " << missing;
    for (auto const& p : retiredLibraryProviders()) {
        if (vocabulary.count(p)) {
            ADD_FAILURE()
                << "provider '" << p << "' is listed as RETIRED but"
                   " LIBRARY_PROVIDERS contains it again. Delete it from"
                   " retiredLibraryProviders() — a stale retirement is how a"
                   " reopened route comes to look closed.";
            continue;
        }
        for (auto const& module : modules) {
            for (auto const& [n, line] : pythonCode(module).live) {
                EXPECT_EQ(line.find(p), std::string::npos)
                    << module.filename().string() << ':' << n
                    << " still carries the RETIRED library provider '" << p
                    << "' in a LIVE line:\n  " << line
                    << "\nThe resolver REFUSES that name, so this is either dead"
                       " dispatch code or a diagnostic advertising a"
                       " declaration that cannot load. A comment may narrate the"
                       " retirement; code may not keep performing it.";
            }
        }
    }
}

// ── A PROVIDER NO ARM DISPATCHES IS A POISONED LEG, AND ONLY THAT LEG ──────
//
// THE TWIN DRIVERS DISAGREED ON THE UNKNOWN-PROVIDER VERDICT.
//
// ★★ WHAT THE TWO PINS ABOVE COULD NOT SEE, AND WHY. Both of them walk dispatch
// arm EXISTENCE and retired-name ABSENCE. Neither asks what the default arm DOES.
// ✔MEASURED 2026-08-20, for a leg declaring a library provider NEITHER twin
// implemented:
//   * build-and-test.sh's `*)` arm called `die` = `exit 1`, from a TOP-LEVEL
//     `for leg in "${LEG_ORDER[@]}"` loop — so ONE leg's bad declaration cost
//     the ENTIRE run and the other four legs never reached any verdict at all;
//   * build-and-test.ps1's `default` arm returned `Ok = $false`, which its own
//     docblock defined as ALWAYS `skipped-build-input-missing` — an
//     ENVIRONMENTAL skip, which merely WARNS unless DSS_STRICT_ARM_VERDICTS=1.
// One tree, one condition, exit 1 on one host and exit 0 on the other; and the
// exit-0 half filed a HARNESS/CATALOGUE bug as a fact about the operator's
// machine, which this project treats as worse than a loud failure.
//
// ★ THE ANSWER IS `poisoned`, PER LEG, AND THE RUN CONTINUES. `poisoned` is the
// closed vocabulary's FAILURE class, the same one the ledger records for a token
// it cannot classify ("HARNESS DEFECT: …"). The decision is ONE pure function,
// `sqlite_libs.unknown_library_provider_verdict`; the dispatch's `else:` arm
// records its answer and RETURNS from that leg's resolution, so `step6`'s loop
// goes on to the next leg.
//
// ★★★ AND THE ASSERTION EXECUTES THE DECISION, NOT ONLY READS IT. While there
// were two copies, (c) compared them BY EXECUTION (`harness_legs.py
// --check-regions` extracted both mirrored copies from the shipped twins, ran
// them on byte-identical input and compared the answers, with an `expect` so two
// copies agreeing on a WRONG token still redded) — and it had to SKIP wherever
// PowerShell was absent, the arm64 VPS and macOS. With one copy there is nothing
// to compare, and the pin is stronger for it: (c) now runs THE decision on the
// retired differential case's six inputs, in this leg's own Python, and requires
// the stated answer byte for byte — on every leg, no skip.
TEST_F(HarnessLegs, BothDriversRefuseAnUnimplementedProviderWithTheSameVerdict) {
    auto const libs = harnessDir() / "sqlite_libs.py";
    ASSERT_TRUE(fs::exists(libs)) << libs;
    auto const modules = driverModules();
    auto const missing = missingDriverModules(modules);
    ASSERT_TRUE(missing.empty()) << "driver modules missing: " << missing;

    // ── (a) THE DRIVER ROUTES THROUGH THE ONE DECISION ──────────────────────
    // Defined once and CALLED at least once, across every driver module. A driver
    // that grew a second, local answer would still pass (c) — the one decision
    // would be untouched and simply unused — so the CALL is asserted too.
    std::size_t defined = 0, called = 0;
    for (auto const& module : modules) {
        for (auto const& [n, line] : pythonCode(module).live) {
            if (line.find("unknown_library_provider_verdict") == std::string::npos) continue;
            if (startsWith(trimmedLeft(line), "def unknown_library_provider_verdict(")) {
                ++defined;
            } else {
                ++called;
            }
        }
    }
    EXPECT_EQ(defined, 1u)
        << "the driver defines `unknown_library_provider_verdict` " << defined
        << " time(s); the unknown-provider verdict is ONE decision and a second"
           " definition is a second answer.";
    EXPECT_GE(called, 1u)
        << "the driver never CALLS `unknown_library_provider_verdict`, so the decision"
           " (c) proves correct is dead code while the dispatch answers the condition"
           " some other way.";

    // ── (b) THE DEFAULT ARM RECORDS THAT ANSWER AND ENDS ONLY ITS OWN LEG ──
    // Bounded by STRUCTURE, never by a line number: the `else:` that closes the
    // `if provider == "…":` / `elif provider == "…":` chain in sqlite_libs.py, and
    // its indented body.
    auto const& code = pythonCode(libs);
    auto const& st   = code.statements;
    auto const  head = statementStarting(st, "if provider == \"");
    ASSERT_LT(head, st.size())
        << "sqlite_libs.py has no `if provider == \"…\":` dispatch, so this pin cannot"
           " locate the provider dispatch it is about. That is a changed structure,"
           " not a passing test.";
    std::size_t defaultArm = st.size();
    for (std::size_t i = head + 1; i < st.size(); ++i) {
        if (st[i].indent > st[head].indent) continue;   // an arm's body
        if (st[i].indent < st[head].indent) break;      // the chain's scope ended
        auto const t = trimmedLeft(st[i].text);
        if (startsWith(t, "elif provider == \"")) continue;
        if (startsWith(t, "else:")) defaultArm = i;
        break;
    }
    ASSERT_LT(defaultArm, st.size())
        << "the provider dispatch in sqlite_libs.py has no `else:` arm, so a leg"
           " declaring an unknown provider would fall out of it SILENTLY to an empty"
           " library pair and be reported as a missing build input — the exact"
           " silence that arm exists to prevent.";
    auto const body = blockOf(st, defaultArm);
    ASSERT_FALSE(body.empty())
        << "the `else:` arm of the provider dispatch has no body this pin can read.";
    bool bindsTheDecision = false, recordsItsToken = false, endsTheLeg = false;
    for (auto const& s : body) {
        auto const t = trimmedLeft(s.text);
        bindsTheDecision =
            bindsTheDecision
            || t.find("verdict, detail = unknown_library_provider_verdict(") != std::string::npos;
        // The live text that proves the decision's VERDICT TOKEN is consumed rather
        // than computed and thrown away. Calling the decision and then recording a
        // HARD-CODED token is the original defect wearing the new structure as a
        // costume, and every other assertion here would stay green through it.
        recordsItsToken = recordsItsToken
                       || t.find("marks_harness_defect(leg, verdict, detail)") != std::string::npos;
        endsTheLeg = endsTheLeg || t == "return" || t == "continue";
        // `die`, `exit` and `raise` END THE RUN. One leg's bad declaration must
        // never cost the other legs their verdicts.
        bool const terminates =
            t == "raise" || startsWith(t, "raise ") || t.find("die(") != std::string::npos
            || t.find("exit(") != std::string::npos || t.find("abort(") != std::string::npos;
        EXPECT_FALSE(terminates)
            << "sqlite_libs.py:" << s.first << " — the unknown-provider arm ENDS THE RUN:\n  "
            << s.text
            << "\nOne leg declaring a provider no arm dispatches must cost THAT LEG a"
               " `poisoned` verdict and nothing else; a terminating call here is the .sh"
               " twin's `die` from inside the leg loop, restored.";
    }
    EXPECT_TRUE(bindsTheDecision && recordsItsToken)
        << "the unknown-provider arm does not record the ONE decision's token (looked for"
           " `verdict, detail = unknown_library_provider_verdict(` and"
           " `marks_harness_defect(leg, verdict, detail)` in its body). A driver that"
           " computes the token and then writes a different one into the ledger has"
           " re-opened the divergence this test exists for.";
    EXPECT_TRUE(endsTheLeg)
        << "the unknown-provider arm does not `return`. Falling out of the arm reaches"
           " the missing-libraries check below it, which OVERWRITES the harness-defect"
           " verdict with `skipped-build-input-missing` — filing our bug as a fact about"
           " the operator's machine.";

    // ── (c) THE DECISION, EXECUTED, AGAINST ITS STATED ANSWER ───────────────
    // The six inputs are the retired differential case's (`unknown-library-provider`),
    // COPIED here so the pin owns them: the provider is the RETIRED name on purpose,
    // the one that really did reach this arm, and `known` is INPUT DATA — a fixed
    // argument, not a claim about the live vocabulary. The answer is STATED here,
    // byte for byte, and never computed by the code it judges: a derived
    // expectation is the same code twice.
    // ⓘ THE STATED ANSWER WAS RESTATED FOR THE SINGLE IMPLEMENTATION. The twin-parity
    // case expected "… Add the arm to BOTH drivers: a capability in one driver and
    // not the other is this project's canonical silent harness bug. …"; the one
    // decision says "Add its dispatch arm here in that change: …" instead, and is
    // otherwise unchanged. ✔MEASURED 2026-09-22: `poisoned` plus the 835-character
    // detail below, ASCII, byte-identical to what the decision returns.
    // The module is imported from the harness directory with bytecode off (`-B`):
    // the action is `requireInputsUnmoved`, so no `__pycache__` may appear beside it.
    std::vector<std::string> const inputs{
        "driver-under-test", "elf64-arm64", "ubuntu-ports-arm64",
        "libtcl8.6.so libtcl8.6.so.0", "libz.so.1",
        "host-system pinned-archive search-paths"};
    std::string const wantDetail =
        "HARNESS DEFECT: leg 'elf64-arm64' declares library provider 'ubuntu-ports-arm64',"
        " which driver-under-test has no dispatch arm for, so this driver cannot obtain"
        " that leg's DECLARED inputs (tcl: libtcl8.6.so libtcl8.6.so.0 / z: libz.so.1)."
        " ACQUISITION IS NOT DRIVER-LOCAL - pinned-archive is performed by harness_legs.py"
        " --acquire, on every host - so NO declared provider should reach this arm;"
        " reaching it means the catalogue or LIBRARY_PROVIDERS grew a provider and this"
        " driver was not extended in the same change. Add its dispatch arm here in that"
        " change: a provider the catalogue declares and the driver cannot dispatch is a"
        " capability that exists on paper only, this project's canonical silent harness"
        " bug. Known providers: host-system pinned-archive search-paths (printed by"
        " harness_legs.py --library-providers, never copied here).";
    auto const py = pythonPath();
    ASSERT_FALSE(py.empty()) << "python3 (or python) is not on PATH; the driver IS Python.";
    auto const request = scratch_->path() / "unknown-provider-request.json";
    {
        json req = json::object();
        req["dir"]  = harnessDir().string();
        req["args"] = inputs;
        std::ofstream out(request, std::ios::binary);
        out << req.dump();
    }
    static constexpr char const* kDecisionShim =
        "import json,sys\n"
        "r=json.load(open(sys.argv[1],encoding='utf-8'))\n"
        "sys.path.insert(0,r['dir'])\n"
        "import sqlite_libs\n"
        "v=sqlite_libs.unknown_library_provider_verdict(*r['args'])\n"
        "print(json.dumps({'answer':list(v)}))\n";
    // The REQUEST rides last (the POSIX arm of `runBinary` chmods its last argv
    // element, and a scratch file is the only thing it may touch).
    auto const res = dss::test_support::runBinary(
        request, dss::test_support::kHelperScriptBudget, /*captureStdout=*/true,
        {py, "-B", "-c", kDecisionShim});
    ASSERT_TRUE(res.spawned && !res.timedOut) << res.diagnostic;
    ASSERT_EQ(res.exitCode, 0u)
        << "sqlite_libs.unknown_library_provider_verdict could not be run from "
        << harnessDir().string() << ":\n" << res.capturedStdout;
    json answer;
    auto const brace = res.capturedStdout.find('{');
    ASSERT_NE(brace, std::string::npos) << res.capturedStdout;
    ASSERT_NO_THROW(answer = json::parse(res.capturedStdout.substr(brace)))
        << res.capturedStdout;
    ASSERT_TRUE(answer.contains("answer") && answer.at("answer").is_array()
                && answer.at("answer").size() == 2u)
        << "the decision did not answer a (verdict, detail) pair:\n" << res.capturedStdout;
    auto const verdict = answer.at("answer").at(0).get<std::string>();
    auto const detail  = answer.at("answer").at(1).get<std::string>();
    EXPECT_EQ(verdict, "poisoned")
        << "the unknown-provider verdict is no longer `poisoned`, the closed"
           " vocabulary's FAILURE class. `skipped-build-input-missing` (the .ps1's old"
           " answer) is an ENVIRONMENTAL skip that exits 0 over a harness defect.";
    EXPECT_EQ(detail, wantDetail)
        << "the decision's detail is not the stated answer, byte for byte. The message"
           " is part of the verdict: it is what tells a reader why the leg was"
           " poisoned and what to do.";
    for (unsigned char const ch : verdict + detail) {
        ASSERT_LT(ch, 0x80u)
            << "the decision answered a non-ASCII byte; it promises ASCII, because the"
               " detail is printed verbatim into logs whose encodings differ by host:\n"
            << detail;
    }
}

// IT REFUSES RATHER THAN IMPROVISING.
// Offline with a cold cache is the case that matters: the tempting behaviour is
// to fall back to "whatever tcl is on this machine", which would build the leg
// against a foreign library and report it green.
TEST_F(HarnessLegs, AcquisitionRefusesRatherThanImprovisingWithAColdCache) {
    auto const label = firstAcquiringLeg(catalogue_);
    ASSERT_FALSE(label.empty()) << "no acquiring leg — vacuous";
    auto const cold = scratch_->path() / "cold-cache";
    ASSERT_FALSE(fs::exists(cold)) << "the cache root must be absent: " << cold;

    auto const r =
        run({"--acquire", label, "--offline", "--cache-root", cold.string()});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_NE(r.exitCode, 0u)
        << "--offline with a cold cache SUCCEEDED. Somewhere it improvised:\n"
        << r.output;
    EXPECT_NE(r.output.find("https://"), std::string::npos)
        << "the refusal must name the source it could not reach:\n" << r.output;

    // And it refused BEFORE creating anything: a run that cannot succeed must
    // not leave a half-built cache tree behind for the next run to trust.
    EXPECT_FALSE(fs::exists(cold))
        << "a failed offline acquisition created " << cold;
}

// THE COMPILER FLAG IS NAMED IN ONE FILE, NOT IN THE DRIVER — the same argument
// `--translate-path` makes for `wslpath`. While there were two twins, a driver
// that spelled it itself was a capability that could exist in one and not the
// other; with one driver it is still a second place the flag's shape (and its
// `=<import-name>` suffix) would have to be kept in step.
TEST_F(HarnessLegs, TheRecordedIdentityFlagIsNamedInExactlyOneFile) {
    // Without an override, the argv is what every leg has always passed.
    auto const plain = run({"--resolve-library-argv", "/tmp/libz.so.1"});
    ASSERT_TRUE(plain.spawned) << plain.diagnostic;
    ASSERT_EQ(plain.exitCode, 0u) << plain.output;
    auto const plainTokens = splitLines(plain.output);
    ASSERT_EQ(plainTokens.size(), 2u) << plain.output;
    EXPECT_EQ(plainTokens[0], "--resolve-library");
    EXPECT_EQ(plainTokens[1], "/tmp/libz.so.1");

    // With one, the flag appears — and it is the RESOLVER that chose it.
    auto const over = run({"--resolve-library-argv", "/tmp/libz.1.dylib",
                           "--import-name", "@loader_path/libz.1.dylib"});
    ASSERT_TRUE(over.spawned) << over.diagnostic;
    ASSERT_EQ(over.exitCode, 0u) << over.output;
    // ★ THE OVERRIDE IS A VALUE SUFFIX, NOT A SECOND FLAG:
    // `--resolve-library <path>[=<import-name>]`, which the compiler splits on
    // its LAST `=`. An earlier draft of this test asserted the opposite — that
    // the first token must NOT be `--resolve-library` — which was a guess made
    // before the compiler side landed, and it went red against a correct
    // resolver. The shape below is the one that is actually true, and it is
    // asserted on the TOKENS rather than on the joined string so a change in
    // either half reds here, in the one place that has to be updated.
    auto const overTokens = splitLines(over.output);
    ASSERT_EQ(overTokens.size(), 2u) << over.output;
    EXPECT_EQ(overTokens[0], "--resolve-library") << over.output;
    EXPECT_EQ(overTokens[1], "/tmp/libz.1.dylib=@loader_path/libz.1.dylib")
        << "the identity must ride as a `=<import-name>` suffix on the path:\n"
        << over.output;

    // A compiler whose --help does not carry the flag is a LOUD refusal. The
    // stand-in is the python interpreter: a real executable whose help text
    // certainly lacks it, so the probe is exercised rather than mocked.
    auto const py = pythonPath();
    ASSERT_FALSE(py.empty());
    auto const refused = run({"--resolve-library-argv", "/tmp/libz.1.dylib",
                              "--import-name", "@loader_path/libz.1.dylib",
                              "--dss", py});
    ASSERT_TRUE(refused.spawned) << refused.diagnostic;
    EXPECT_NE(refused.exitCode, 0u)
        << "a compiler that cannot record the declared identity was accepted."
           " Dropping the override links clean here and fails at LOAD time on a"
           " machine this host cannot observe:\n"
        << refused.output;

    // No driver module may spell the compiler's flag itself — it must come from
    // the resolver. Matched as a WHOLE TOKEN: `--resolve-library-argv` is the
    // RESOLVER's own subcommand and the driver has to spell that to call it, so a
    // naive substring scan would forbid the very thing being required. (That is
    // not hypothetical — the first draft of this check did exactly that.) In
    // Python the flag would sit in a string, so the character after it is the
    // closing quote — not `-`, not alphanumeric — and the same token rule holds.
    auto const modules = driverModules();
    auto const missing = missingDriverModules(modules);
    ASSERT_TRUE(missing.empty()) << "driver modules missing: " << missing;
    std::string const compilerFlag = "--resolve-library";
    for (auto const& module : modules) {
        for (auto const& [n, line] : pythonCode(module).live) {
            for (std::size_t at = line.find(compilerFlag);
                 at != std::string::npos;
                 at = line.find(compilerFlag, at + 1)) {
                char const next = at + compilerFlag.size() < line.size()
                                      ? line[at + compilerFlag.size()]
                                      : '\0';
                if (next == '-' || std::isalnum(static_cast<unsigned char>(next))) {
                    continue;   // `--resolve-library-argv`, the resolver's verb
                }
                ADD_FAILURE()
                    << module.filename().string() << ':' << n
                    << " spells the COMPILER's --resolve-library flag itself:\n  " << line
                    << "\nIt belongs in harness_legs.py alone (--resolve-library"
                       "-argv), so the driver cannot drift from it and the"
                       " import-name suffix can never be silently dropped.";
            }
        }
    }
}

// ── 9. NOTHING INVOKES `wsl.exe` WITHOUT `-e` ──────────────────────────────
//
// `wsl.exe` WITHOUT `-e` RUNS A LOCAL SHELL.
//
// THE FORBIDDEN SHAPE, in one line: `wsl.exe <anything-but--e> …`.
//
// WHY. `wsl.exe <cmd>` does not run <cmd>. WSL reconstructs a command LINE from
// the remaining argv and feeds it to the distro's DEFAULT SHELL, which strips
// quoting and performs expansions BEFORE the named binary is ever reached — so
// the payload is parsed twice and the first pass happens where nobody is
// looking. ✔MEASURED 2026-08-04 on this host, one variable changed, same input:
//     wsl.exe    bash -lc "printf '[%s]\n' 'echo A=$(uname -m)'"  ->  [echo A=x86_64]
//     wsl.exe -e bash -lc "printf '[%s]\n' 'echo A=$(uname -m)'"  ->  [echo A=$(uname -m)]
//
// ★ QUOTING IS NOT THE FIX, and that is why this is a test and not a review
// note. ✔MEASURED at the real call site (Invoke-PosixCommand's payload): with no
// `-e`, a SINGLE-QUOTED `$HOME` inside the payload still expanded, arriving as
// `[lit /home/rafael and * and a\b]` instead of `[lit $HOME and * and a\b]`,
// because the outer shell removed the quotes first. Every escaping fix
// therefore looks correct and still leaks.
//
// ★ `--` IS NOT `-e` EITHER — it is documented as "pass the remaining command
// line as is", and "as is" means "to the shell". ✔MEASURED the same day:
// `wsl.exe -- /nope` answers `/bin/bash: line 1: /nope: No such file` while
// `wsl.exe -e /nope` answers `execvpe(/nope) failed`; and one argument
// `…/g/*.test` reached the callee as TWO arguments under `--` and as ONE under
// `-e`. That is why `--` is called out by name below instead of being lumped in
// with "some other token" — it reads like the safe spelling and is not.
//
// WHAT IT COST, so nobody re-litigates the severity. ✔MEASURED on the carriage
// script that used to reach the aarch64 VPS: it ran `wsl.exe bash -lc "ssh …
// $Command"`, so `-Command 'hostname; uname -m'` printed the VPS hostname and
// then the LOCAL WSL architecture — x86_64 for an aarch64 box — while exiting 0.
// A cross-host verification instrument answering with the wrong host's data,
// silently. The same defect sat under `wslpath: C:ab`, where it was
// misattributed to wslpath eating backslashes and papered over with a separator
// rewrite (section 8's corrected comment).
// ⓘ That script is GONE — the DssHarness migration replaced every carriage
// script with one tool reading one configuration — and the measurement is kept
// because it is why this rule exists, not because the file is still there to
// look at. Nothing below depends on it: the shapes are proved against fixtures.
//
// THE RULE, and why it is shaped this way rather than "the file must not
// contain `wsl.exe` without `-e`":
//   · over LIVE lines (`liveLines`), because every driver DOCUMENTS the shape it
//     removed — a raw-text rule would be satisfied by deleting the explanation,
//     which is the lesson `liveLines` already exists for;
//   · only in COMMAND POSITION (first token, or after `&` / `;` / `(` / `{` /
//     backtick), because `Get-Command wsl.exe -ErrorAction …` RESOLVES the
//     launcher without running it, and a diagnostic string may legitimately name
//     it in prose. This needs no suppression list: neither shape is command
//     position, by construction;
//   · `|` is deliberately NOT a command-position marker. The retired
//     build-and-test.sh carried `grep -qiE 'microsoft|wsl' /proc/version`, and a
//     rule that read that as a pipeline would red on a regex. A pipeline INTO
//     wsl.exe is not a shape this harness uses, and buying it would cost a false
//     positive;
//   · a bare `wsl` counts only when followed by WHITESPACE — an invocation is
//     always followed by its argv. That one condition is what keeps the same
//     regex, `build-wsl/` and `$wslKey` out of it without naming any of them;
//   · a PowerShell SPLAT (`& wsl.exe @a`) is the correct fix's own shape — there
//     is no string left to escape — so it is accepted only when the array it
//     splats is bound to `-e` nearby. ★ THAT EXEMPTION IS PROVED BY FIXTURE, NOT
//     BY CENSUS: `TheWslExecRuleJudgesEverySyntheticShape` drives the bound form
//     (accepted), the unbound form (refused) and a binding outside the window
//     (refused). It used to be exercised by ONE live carriage script, which the
//     DssHarness migration deleted — and an escape whose only case leaves the
//     tree is an escape nothing exercises, which is a disarmed guard rather than
//     dead weight;
//   · PYTHON has no command position: a Python program spawns an argv LIST, so
//     its arm reads the syntax tree (`pythonInspection`). Every list or tuple
//     literal whose FIRST element names wsl/wsl.exe (bare, or as a path's last
//     part) is an invocation, and its next element must be `-e`/`--exec` — after
//     any `--cd <dir>` pair, the one wsl.exe option that takes a value and may
//     precede `-e` (the resolver's own `workingDirArgv`; its self-test measured
//     `wsl.exe --cd /tmp -e pwd` -> /tmp). `--` there, a starred splice, a
//     computed value, or a literal that ENDS before `-e` is refused. A command
//     LINE naming wsl handed to a SHELL (`shell=True`, `os.system`, `os.popen`,
//     `subprocess.getoutput`/`getstatusoutput`) is refused whatever it says,
//     because a shell parses it before wsl.exe runs. A membership operand
//     (`x in ("wsl", "wsl.exe")`) and a tuple made only of launcher names are
//     NAMES, not argv. And `wsl.exe --` written in any string of LIVE code is
//     refused exactly as the shell rule refuses it: as advice a reader pastes.
//
// COVERAGE IS BY DIRECTORY, NOT BY LIST: every `.py`, `.ps1` and `.sh` anywhere
// under DssHarness's actions directory, so a NEW program is governed the day it
// lands. The walk is RECURSIVE because that directory is one directory per action
// with the siblings inside it, and actions may be GROUPED (`real-examples/c/sqlite`
// is), so a program can sit any number of levels down.
// ⓘ Until 2026-09-18 the walk was `scripts/`, and the sqlite harness's OTHER shell
// programs (its self-tests, the base harness, the benchmark driver) were governed by
// nothing, because only the two drivers were named. Until 2026-09-21 it read only
// `.sh`/`.ps1`; since the operator's order that no `.sh`/`.ps1` lives under the
// actions directory its programs are Python, so the Python arm above is where the
// coverage lives, and the shell arm stays for any shell file that lands there anyway
// (the scripts index refuses one). The catalogue's launcher argv and the resolver's
// translator argv are held to the same rule from their DATA, in the second test —
// and those are the two places that actually carried `--`.
//
// ⚠ ONE THING THIS RULE DOES NOT COVER, labelled rather than left implicit:
// `.claude/skills/dss-state/driver.mjs` spawns WSL from a JS argv ARRAY
// (`spawnSync('wsl', ['-e', …])`), not from a command line, so none of the
// shapes above apply to it. It was VERIFIED BY READING on 2026-08-04 — lines
// 171, 244, 248-249 and 250 all put `-e` first — and it is left unscanned
// because a line-oriented rule would either miss it or invent false positives.

namespace {

struct Script {
    fs::path path;
    Dialect  dialect;
};

// Every program this rule governs. The actions directory is ENUMERATED rather
// than listed so a new program there cannot land outside the rule.
//
// ★★★ AND THE ENUMERATION HAS A FLOOR, which is not defensive clutter: the
// walk swallows its error_code, so a directory that is missing or renamed
// yields an EMPTY range and every assertion below iterates nothing — a guard
// that reads exactly like a guard that found nothing. ✔That is not hypothetical,
// TWICE: this walk said `tools/` until 2026-08-19, when that directory was merged
// into `scripts/`, and it said `scripts/` until 2026-09-18, when every program moved
// into the actions directory — ✔MEASURED that day, the unretargeted walk found 0
// and this floor is what said so. Retargeted, never lowered.
// ⓘ THE FLOOR MOVED TO THE `.py` POPULATION on 2026-09-21, with the programs: a
// floor on the `.sh`/`.ps1` count, which the operator ordered to zero, could only
// have been lowered to zero — the one thing it must never be. It is re-derived by
// the rule that set it, about half the live figure (it was 16 against 32), so
// ordinary churn never trips it and a collapsed walk cannot pass: ✔MEASURED
// 2026-09-22, 66 `.py` files under the actions directory (16 shell files were
// still awaiting deletion then; they carry no floor).
[[nodiscard]] std::vector<Script> scriptsUnderTest() {
    std::vector<Script> out;
    std::error_code     ec;
    auto const          scriptsDir = actionsDir();
    for (auto it = fs::recursive_directory_iterator(scriptsDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        auto const& e    = *it;
        auto const  name = e.path().filename().string();
        // DssHarness gives every action a `build/` for a run's working space and an
        // `artifacts/` for what it keeps (`help runners`): gitignored OUTPUT, never a
        // program this repository ships, so the walk does not descend into either.
        if (e.is_directory() && (name == "build" || name == "artifacts" ||
                                 name == "__pycache__")) {
            it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file()) continue;
        auto const ext = e.path().extension().string();
        if (ext != ".py" && ext != ".ps1" && ext != ".sh") continue;
        out.push_back({e.path(), dialectOf(e.path())});
    }
    std::sort(out.begin(), out.end(), [](Script const& a, Script const& b) {
        return a.path < b.path;   // a stable failure order
    });

    std::vector<fs::path> python;
    for (auto const& s : out) {
        if (s.dialect == Dialect::Python) python.push_back(s.path);
    }
    constexpr std::size_t kPythonFloor = 33;
    EXPECT_GE(python.size(), kPythonFloor)
        << "only " << python.size() << " .py programs were found under "
        << scriptsDir.string()
        << (ec ? " (walk error: " + ec.message() + ")" : "")
        << ".\nThe enumeration COLLAPSED — this test would then govern a fraction of"
           " the programs it exists for and still report a pass. Fix the walk; do"
           " not lower the floor.";
    PyInspector::instance().prefetch(python);   // ONE inspector spawn for all of them
    return out;
}

[[nodiscard]] bool isWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The character before `at`, ignoring spaces/tabs; '\0' at the start of a line.
[[nodiscard]] char precedingSymbol(std::string const& line, std::size_t at) {
    while (at > 0 && (line[at - 1] == ' ' || line[at - 1] == '\t')) --at;
    return at == 0 ? '\0' : line[at - 1];
}

// The whitespace-delimited token FOLLOWING the one that starts at `at`.
[[nodiscard]] std::string tokenAfter(std::string const& line, std::size_t at) {
    std::size_t i = at;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    std::size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    return line.substr(i, j - i);
}

[[nodiscard]] std::vector<std::string> splitWords(std::string const& s) {
    std::vector<std::string> out;
    std::istringstream       in{s};
    std::string              w;
    while (in >> w) out.push_back(w);
    return out;
}

// The 1-based line number of `content` in the RAW file, or 0.
//
// ★ NOT the index into `liveLines`, and this is not a nicety: the first version
// of this test reported the live index, which named line 45 of build-and-test.ps1
// for a defect on line 373 — a diagnostic that sends the reader to an unrelated
// line is worse than one that gives no line at all. `liveLines` hands back its
// lines verbatim, so the raw number is recoverable by looking the text back up.
[[nodiscard]] std::size_t rawLineNumberOf(fs::path const&    p,
                                          std::string const& content) {
    std::istringstream in{fileText(p)};
    std::string        line;
    for (std::size_t n = 1; std::getline(in, line); ++n) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == content) return n;
    }
    return 0;
}

// `@a` -> `a`; "" when `tok` is not a PowerShell splat.
[[nodiscard]] std::string splattedVariable(std::string const& tok) {
    if (tok.size() < 2 || tok[0] != '@') return {};
    std::string name;
    for (std::size_t i = 1; i < tok.size() && isWordChar(tok[i]); ++i) {
        name.push_back(tok[i]);
    }
    return name;
}

// Is `$<name>` bound to an argv naming `-e` within the window ABOVE `at`? The
// window mirrors `boundToTranslator`'s reasoning: wide enough for a construct
// split over a few lines, far too narrow to pair two unrelated mentions.
[[nodiscard]] bool splatIsBoundToExec(std::vector<std::string> const& lines,
                                      std::size_t                     at,
                                      std::string const&              name) {
    std::string const needle = "$" + name;
    std::size_t const first  = at >= 12 ? at - 12 : 0;
    for (std::size_t i = first; i <= at; ++i) {
        auto const pos = lines[i].find(needle);
        if (pos == std::string::npos) continue;
        auto const after = pos + needle.size();
        if (after < lines[i].size() && isWordChar(lines[i][after])) continue;
        if (lines[i].find('=') == std::string::npos) continue;
        if (lines[i].find("'-e'") != std::string::npos ||
            lines[i].find("\"-e\"") != std::string::npos ||
            lines[i].find("'--exec'") != std::string::npos ||
            lines[i].find("\"--exec\"") != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct Mention {
    std::size_t at;
    std::string spelling;
};

// One refusal the rule raised, with the text that caused it.
struct WslRefusal {
    std::string subject;   // the file's name
    std::size_t line;      // its RAW line number, 0 when not recoverable
    std::string text;      // the live line
    std::string why;       // the rule's own diagnosis
};

// What one scan saw. `invocations` counts every mention in COMMAND POSITION —
// the shapes this rule governs — whether it accepted or refused them.
struct WslScan {
    std::size_t             invocations = 0;
    std::vector<WslRefusal> refusals;
};

// Every offset at which this line names the WSL launcher AS A COMMAND WORD.
[[nodiscard]] std::vector<Mention> wslMentions(std::string const& line) {
    std::vector<Mention> out;
    for (std::size_t at = line.find("wsl"); at != std::string::npos;
         at = line.find("wsl", at + 1)) {
        // LEFT boundary: `build-wsl/`, `$wslKey`, `--no-wsl` and
        // `$script:HostNeedsWsl` are names, not invocations.
        if (at > 0) {
            char const p = line[at - 1];
            if (isWordChar(p) || p == '-' || p == '.' || p == '/' ||
                p == '\\' || p == '$') {
                continue;
            }
        }
        std::string spelling = "wsl";
        if (line.compare(at, 7, "wsl.exe") == 0) spelling = "wsl.exe";
        // RIGHT boundary, and it must be WHITESPACE: an invocation is always
        // followed by its argv, while `'microsoft|wsl'` is followed by a quote.
        auto const after = at + spelling.size();
        if (after >= line.size()) continue;
        if (line[after] != ' ' && line[after] != '\t') continue;
        out.push_back({at, spelling});
    }
    return out;
}

// THE RULE ITSELF, over one set of scripts, answering rather than asserting.
//
// ★★★ EXTRACTED SO THE RULE CAN BE POINTED AT A FIXTURE. It used to be the body
// of the tree test, which meant every shape it recognises was proved only by the
// tree happening to contain that shape — and the exemption below was proved by
// exactly ONE file. A rule whose escape hatch has no synthetic case is an escape
// nothing exercises: it can be broken in either direction and stay green, and
// the day its one live example leaves the tree it becomes untested logic.
// Which live lines are the BODY of a PowerShell here-string. A here-string is a
// STRING LITERAL: `@"` or `@'` ENDING a line opens one and `"@` or `'@` at the
// START of a line closes it (PowerShell requires both positions), so every line
// strictly between is text, not a command line — `Die @" … wsl.exe not found … "@`
// names the launcher in a message and invokes nothing.
// ✔MEASURED 2026-09-18: the first scan of the sqlite harness's OTHER scripts —
// governed by nothing until they moved under the actions directory with the rest —
// refused `benchmark-speedtest1.ps1`'s missing-WSL message, whose here-string line
// begins `wsl.exe not found.`, as an invocation. Only this rule skips the bodies:
// `liveLines` also fed the twin drivers' checks of the .ps1's EMBEDDED bash,
// which lived in here-strings on purpose, and it keeps that meaning for any
// PowerShell file it is given.
[[nodiscard]] std::vector<bool> powershellHereStringBody(
    std::vector<std::string> const& lines) {
    std::vector<bool> body(lines.size(), false);
    char              open = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string t = lines[i];
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        if (open != 0) {
            if (t.size() >= 2 && t[0] == open && t[1] == '@') {
                open = 0;   // the closing line: code resumes after it
            } else {
                body[i] = true;
            }
            continue;
        }
        if (t.size() >= 2 && t[t.size() - 2] == '@' &&
            (t.back() == '"' || t.back() == '\'')) {
            open = t.back();
        }
    }
    return body;
}

// The one message every `--` refusal carries, whichever arm found it.
constexpr char const* kDashDashWhy =
    " --`. `--` is NOT `--exec`. MEASURED: `wsl.exe -- /nope` answers `/bin/bash:"
    " line 1: /nope: No such file` where `wsl.exe -e /nope` answers"
    " `execvpe(/nope) failed` — so `--` hands the whole argv to the distro's"
    " default shell, which re-expands it. Use `-e`.";

// THE PYTHON ARM (see "THE RULE" above): the inspector's verdict on every wsl
// argv literal and every shell command line naming wsl, then the `--` rule over
// every LIVE line — strings included, because a message is code, and prose
// excluded, because a docstring may say what `--` does. A file the inspector
// could not read is a REFUSAL here, never a silent skip: a subject this rule
// cannot read is indistinguishable from one it governed and cleared.
void scanPythonForWsl(fs::path const& path, std::string const& name, WslScan& scan) {
    auto const& in = pythonInspection(path);
    if (!in.error.empty()) {
        scan.refusals.push_back(
            {name, 0, "",
             "could not be read as Python by python " + PyInspector::instance().python()
                 + " (" + in.error + "), so this rule cannot govern it."});
        return;
    }
    scan.invocations += in.wslInvocations;
    for (auto const& r : in.wslRefusals) {
        scan.refusals.push_back(
            {name, r.line, liveTextAt(in, r.line),
             "invokes wsl through an argv that " + r.why
                 + ". `wsl.exe` without `-e` runs a LOCAL shell: it hands the"
                   " reconstructed command line to the distro's DEFAULT SHELL, which"
                   " strips quoting and expands ON THIS MACHINE first. Quoting cannot"
                   " fix it — put `-e` right after the launcher (after `--cd <dir>`"
                   " when there is one)."});
    }
    for (auto const& [n, line] : in.live) {
        for (auto const& m : wslMentions(line)) {
            if (tokenAfter(line, m.at) != "--") continue;
            scan.refusals.push_back({name, n, line, "spells `" + m.spelling + kDashDashWhy});
        }
    }
}

[[nodiscard]] WslScan scanForWslExec(std::vector<Script> const& scripts) {
    WslScan scan;
    for (auto const& script : scripts) {
        auto const name  = script.path.filename().string();
        if (script.dialect == Dialect::Python) {
            scanPythonForWsl(script.path, name, scan);
            continue;
        }
        bool const powershell = script.dialect == Dialect::PowerShell;
        auto const lines      = liveLines(script.path, script.dialect);
        auto const hereBody = powershell ? powershellHereStringBody(lines)
                                         : std::vector<bool>(lines.size(), false);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (hereBody[i]) continue;   // a string literal, never a command line
            auto const& line     = lines[i];
            auto const  mentions = wslMentions(line);
            auto const  at = mentions.empty() ? 0 : rawLineNumberOf(script.path, line);
            for (auto const& m : mentions) {
                auto const next = tokenAfter(line, m.at);

                // `--` first and UNCONDITIONALLY, command position or not: it is
                // wrong even as advice an operator pastes, so it is refused
                // wherever it is written.
                // ⚠ AND IT DOES NOT SHORT-CIRCUIT. A `--` in command position is
                // ALSO an invocation without `-e`, so it earns the second refusal
                // below and counts toward what the rule was seen to govern.
                // Returning early here would have made `wsl.exe --` invisible to
                // the non-vacuity census — the rule's worst spelling being the one
                // that stopped proving the rule was awake.
                if (next == "--") {
                    scan.refusals.push_back(
                        {name, at, line, "spells `" + m.spelling + kDashDashWhy});
                }

                char const prev = precedingSymbol(line, m.at);
                bool const commandPosition =
                    prev == '\0' || prev == '&' || prev == ';' || prev == '(' ||
                    prev == '{' || prev == '`';
                if (!commandPosition) continue;   // a mention, not an invocation
                ++scan.invocations;

                if (next == "-e" || next == "--exec") continue;
                std::string const var = splattedVariable(next);
                if (!var.empty()) {
                    if (!splatIsBoundToExec(lines, i, var)) {
                        scan.refusals.push_back(
                            {name, at, line,
                             "splats `" + next + "` into " + m.spelling +
                                 " but nothing within 12 live lines above binds"
                                 " `$" + var +
                                 "` to an argv naming '-e'. A real argv is the"
                                 " RIGHT fix for this defect — there is no string"
                                 " left for a shell to re-expand — but only if"
                                 " `-e` is actually in it."});
                    }
                    continue;
                }

                scan.refusals.push_back(
                    {name, at, line,
                     "invokes `" + m.spelling + "` without `-e`. The next token"
                     " is '" + next +
                         "'. `wsl.exe` without `-e` runs a LOCAL shell:"
                         " `wsl.exe <cmd>` does not run <cmd>, it hands the"
                         " reconstructed command line to the distro's DEFAULT"
                         " SHELL, which strips quoting and expands ON THIS"
                         " MACHINE first. MEASURED: the same input string gives"
                         " [echo A=x86_64] without `-e` and [echo A=$(uname -m)]"
                         " with it, and a SINGLE-QUOTED $HOME in a payload still"
                         " expanded. Quoting cannot fix it — pass `-e`, or build"
                         " a real argv and splat it."});
            }
        }
    }
    return scan;
}

}  // namespace

TEST_F(HarnessLegs, NoScriptInvokesWslWithoutExec) {
    auto const scripts = scriptsUnderTest();
    for (auto const& s : scripts) {
        ASSERT_TRUE(fs::exists(s.path)) << s.path;
        // A subject with nothing live in it is a subject this rule cannot govern,
        // and it would be indistinguishable from one it governed and cleared. (A
        // Python file the inspector could not read at all is reported by the scan
        // below, as a refusal naming why.)
        if (s.dialect == Dialect::Python) {
            auto const& in = pythonInspection(s.path);
            if (in.error.empty()) {
                EXPECT_FALSE(in.live.empty())
                    << s.path.filename().string() << " has no live lines";
            }
        } else {
            ASSERT_FALSE(liveLines(s.path, s.dialect).empty())
                << s.path.filename().string() << " has no live lines";
        }
    }

    auto const scan = scanForWslExec(scripts);
    for (auto const& r : scan.refusals) {
        ADD_FAILURE() << r.subject << ':' << r.line << ' ' << r.why << "\n  "
                      << r.text;
    }

    // NON-VACUITY, AND IT IS PER SUPPLIER NOW.
    //
    // ⛔ THE UNION FLOOR WAS WRONG IN A WAY ITS OWN NUMBER HID. It read
    // `invocationsSeen >= 4` over every scanned file at once, with a comment
    // naming its three suppliers — "the two sqlite drivers and ssh-arm64-vps.ps1".
    // ✔MEASURED before the DssHarness migration removed the carriage scripts:
    // the live total was 5 — THREE from build-and-test.ps1, one from
    // ssh-arm64-vps.ps1 and one from ssh-macos.sh — so a supplier could go to
    // zero and the union still cleared 4. That is the same defect the per-root
    // floors elsewhere in this repository exist to close: one big contributor
    // satisfies the total while another silently empties.
    //
    // ⇒ THE FLOOR IS ATTACHED TO THE FILE THAT SUPPLIES IT. It was
    // `build-and-test.ps1`, which supplied three and always had. That driver is
    // retired, and the capability moved: the Python driver AND the speedtest1
    // benchmark reach WSL through ONE door, `sqlite_common.PosixSide`, whose three
    // argv literals (`argv`, `to_posix`, `to_host`) are the invocations the
    // harness cannot lose (✔MEASURED 2026-09-22: 3 in sqlite_common.py; the
    // benchmark holds none outside a self-test stand-in, and `build_and_test.py`
    // one more of its own). The number is not lowered — it moved with the door.
    // Programs that shell out to WSL may come and go; this module is the one the
    // harness cannot lose.
    //
    // ★ AND THE RECOGNISER'S COVERAGE IS NO LONGER PROVED BY A CENSUS AT ALL —
    // TheWslExecRuleJudgesEverySyntheticShape below drives every shape, accepted
    // and refused, against fixtures this test writes. A census can only ever
    // prove that the tree still happens to contain an example.
    auto const door     = harnessDir() / "sqlite_common.py";
    auto const doorScan = scanForWslExec({{door, Dialect::Python}});
    EXPECT_GE(doorScan.invocations, 3u)
        << "only " << doorScan.invocations
        << " wsl invocation(s) were RECOGNISED in " << door.string()
        << ", the one door through which the driver and the benchmark reach WSL"
           " (PosixSide: argv, to_posix, to_host). The rule has stopped seeing the"
           " shape it governs in the one file that cannot stop using it. Fix the"
           " recogniser, do not lower this number.";
}

// ── The rule's own arms, synthesized — including the one the tree stopped
//    supplying ──────────────────────────────────────────────────────────────
//
// ★★★ WHY THIS EXISTS. The PowerShell SPLAT exemption (`& wsl.exe @a`, accepted
// only when `$a` is bound to an argv naming `-e` nearby) had exactly one live
// example in the whole repository, and the DssHarness migration deleted the file
// that carried it. ✔MEASURED before the deletion with the recogniser's own
// boundary rules: `grep` for a splatted wsl invocation returned one hit, in the
// carriage script; the nearest survivor uses the DIRECT `& wsl.exe -e …` form,
// which a different clause accepts. So after the deletion the exemption had no
// positive case anywhere — an escape nothing exercises, which this repository
// treats as a disarmed guard rather than as dead weight.
//
// A fixture must synthesize the NEGATIVE as well, so each shape appears twice:
// the spelling that must be ACCEPTED and the neighbouring spelling that must be
// REFUSED. Without the negative half a recogniser that accepts everything passes.
// ★ THE PYTHON ARM IS PROVED THE SAME WAY, and it is the arm the tree now
// exercises: since 2026-09-21 every program under the actions directory is
// Python, so the shell shapes below are the only cases the shell arm has left.
TEST_F(HarnessLegs, TheWslExecRuleJudgesEverySyntheticShape) {
    auto const dir = scratch_->path() / "wsl-exec-shapes";
    fs::create_directories(dir);

    struct Shape {
        char const* name;        // fixture file name: .ps1, .sh or .py
        char const* body;        // its whole text
        bool        refused;     // the verdict this shape must get
        std::size_t invocations; // how many invocations the rule must recognise
        char const* why;         // what the shape is for, quoted on failure
    };

    static constexpr Shape kShapes[] = {
        // ── the splat exemption, both halves ──────────────────────────────
        {"splat-bound-accepted.ps1",
         "$a = @('-e', 'bash', '-lc', $payload)\n& wsl.exe @a\n",
         false, 1,
         "a splatted argv whose array names -e is the CORRECT fix and must be"
         " accepted: there is no command string left for a shell to re-expand"},
        {"splat-unbound-refused.ps1",
         "$a = @('bash', '-lc', $payload)\n& wsl.exe @a\n",
         true, 1,
         "a splatted argv with no -e in it is the DEFECT wearing the fix's"
         " clothes, and must be refused"},
        // ⚠ THE FILLER IS LIVE CODE, NOT COMMENTS, AND THAT IS THE WHOLE POINT
        // OF THE FIXTURE. The window counts LIVE lines, and `liveLines` drops
        // `#` lines entirely — so a comment-padded version of this fixture puts
        // the binding one live line above the call and is ACCEPTED, proving the
        // opposite of what it claims. Caught by reading the fixture against the
        // recogniser before it ever ran.
        {"splat-bound-too-far-refused.ps1",
         "$a = @('-e', 'bash')\n"
         "$f1 = 1\n$f2 = 1\n$f3 = 1\n$f4 = 1\n$f5 = 1\n$f6 = 1\n$f7 = 1\n"
         "$f8 = 1\n$f9 = 1\n$f10 = 1\n$f11 = 1\n$f12 = 1\n$f13 = 1\n"
         "& wsl.exe @a\n",
         true, 1,
         "the binding window is twelve LIVE lines; a binding further away is not"
         " evidence about this call site"},

        // ── the direct spellings ──────────────────────────────────────────
        {"exec-short-accepted.ps1", "& wsl.exe -e bash -lc $payload\n",
         false, 1, "`-e` is the shape the rule exists to require"},
        {"exec-long-accepted.ps1", "& wsl.exe --exec bash -lc $payload\n",
         false, 1, "`--exec` is `-e` spelled out"},
        {"bare-refused.sh", "wsl.exe bash -lc \"$payload\"\n",
         true, 1,
         "a bare invocation hands the reconstructed command line to the distro's"
         " DEFAULT SHELL, which expands it on THIS machine first"},
        {"dashdash-refused.sh", "wsl.exe -- bash -lc \"$payload\"\n",
         true, 1,
         "`--` reads like the safe spelling and is not: it means `pass the rest"
         " as is`, and `as is` means `to the shell`"},

        // ── what must NOT be refused: the boundary rules ──────────────────
        {"mention-not-invocation.ps1",
         "$probe = Get-Command wsl.exe -ErrorAction SilentlyContinue\n",
         false, 0,
         "resolving the launcher is not invoking it, and a diagnostic may name"
         " it in prose — neither is command position"},
        {"name-not-launcher.sh",
         "grep -qiE 'microsoft|wsl' /proc/version\nkey=$wslKey\ncd build-wsl/\n",
         false, 0,
         "`$wslKey`, `build-wsl/` and a regex alternation are NAMES; an"
         " invocation is always followed by whitespace and its argv"},
        {"comment-only.ps1",
         "# wsl.exe bash -lc 'the shape this file used to have'\n$x = 1\n",
         false, 0,
         "comments are prose ABOUT the code — a rule that punished writing down"
         " what you removed would be satisfied by deleting the explanation"},

        // ── a here-string is a STRING, in both directions ─────────────────
        {"here-string-message-accepted.ps1",
         "Die @\"\nwsl.exe not found.\n      it names the launcher it looked for.\n\"@\n",
         false, 0,
         "a PowerShell here-string BODY is a string literal: a message naming the"
         " launcher at the start of one of its lines invokes nothing"},
        {"after-here-string-refused.ps1",
         "Die @'\nwsl.exe not found.\n'@\nwsl.exe bash -lc $payload\n",
         true, 1,
         "the here-string ENDS at its closing marker: the same launcher on the next"
         " line is code again, and without `-e` it is the defect"},

        // ── PYTHON: an argv list, read from the syntax tree ───────────────
        {"argv-exec-accepted.py",
         "import subprocess\nsubprocess.run([\"wsl.exe\", \"-e\", \"printenv\", \"HOME\"])\n",
         false, 1, "`-e` second in the argv literal is the shape the rule requires"},
        {"argv-exec-long-tuple-accepted.py", "ARGV = (\"wsl\", \"--exec\", \"true\")\n",
         false, 1, "`--exec` in a TUPLE whose first element is a bare `wsl`"},
        {"argv-cd-then-exec-accepted.py",
         "def argv(d):\n    return [\"wsl.exe\", \"--cd\", d, \"-e\", \"pwd\"]\n",
         false, 1,
         "`--cd <dir>` is wsl.exe's own option and takes one value; `-e` after it is"
         " the resolver's own working-directory launcher"},
        {"argv-bare-refused.py",
         "import subprocess\nsubprocess.run([\"wsl.exe\", \"bash\", \"-lc\", \"x\"])\n",
         true, 1,
         "an argv whose second element is the COMMAND hands the reconstructed line to"
         " the distro's default shell"},
        {"argv-dashdash-refused.py", "ARGV = [\"wsl.exe\", \"--\", \"bash\"]\n",
         true, 1, "`--` in the argv is the same `--` as on a command line"},
        {"argv-cd-swallows-exec-refused.py", "ARGV = [\"wsl.exe\", \"--cd\", \"-e\", \"pwd\"]\n",
         true, 1,
         "`--cd` takes the NEXT element as its value, so this `-e` is a directory and"
         " `pwd` runs through the shell"},
        {"argv-prefix-only-refused.py", "def argv(rest):\n    return [\"wsl.exe\"] + rest\n",
         true, 1, "a literal that ENDS before `-e` proves nothing about the argv it starts"},
        {"argv-starred-refused.py", "def argv(opts):\n    return [\"wsl.exe\", *opts, \"pwd\"]\n",
         true, 1, "a starred splice where `-e` must stand cannot be shown to be `-e`"},
        {"argv-full-path-refused.py", "ARGV = [\"C:/Windows/System32/wsl.exe\", \"bash\"]\n",
         true, 1, "the launcher named by its PATH is still the launcher"},
        {"shell-true-refused.py",
         "import subprocess\nsubprocess.run(\"wsl.exe -e true\", shell=True)\n",
         true, 1,
         "a command LINE handed to a shell is parsed by that shell first, `-e` or not"},
        {"os-system-refused.py",
         "import os\ndef go(cmd):\n    os.system(f\"wsl.exe bash -lc {cmd}\")\n",
         true, 1, "os.system is a shell command line by definition"},
        {"string-advice-refused.py", "ADVICE = \"run: wsl.exe -- /nope\"\n",
         true, 0, "`wsl.exe --` in a live string is advice a reader pastes"},
        {"docstring-advice-accepted.py",
         "\"\"\"`wsl.exe -- /nope` runs a shell; this docstring only SAYS so.\"\"\"\nX = 1\n",
         false, 0, "a docstring is prose: it may say what `--` does"},
        {"message-accepted.py",
         "def die(m):\n    raise SystemExit(m)\ndef need():\n"
         "    die(\"wsl.exe not found. It is where this host finds its POSIX toolchain.\")\n",
         false, 0, "a message NAMING the launcher invokes nothing"},
        {"membership-accepted.py",
         "def launcher(name):\n    return name in (\"wsl\", \"wsl.exe\")\n",
         false, 0, "a membership operand is a set of NAMES, not an argv"},
        {"name-set-accepted.py", "WSL_NAMES = (\"wsl\", \"wsl.exe\")\n",
         false, 0, "a tuple made only of launcher names is not an argv"},
        {"comment-accepted.py", "# subprocess.run([\"wsl.exe\", \"bash\"])\nX = 1\n",
         false, 0, "comments are prose ABOUT the code"},
    };

    std::vector<fs::path> python;
    for (auto const& shape : kShapes) {
        fs::path const p = dir / shape.name;
        std::ofstream out(p, std::ios::binary);
        ASSERT_TRUE(out.good()) << p;
        out << shape.body;
        if (dialectOf(p) == Dialect::Python) python.push_back(p);
    }
    PyInspector::instance().prefetch(python);   // one inspector spawn for all of them

    for (auto const& shape : kShapes) {
        fs::path const p = dir / shape.name;
        auto const scan = scanForWslExec({{p, dialectOf(p)}});

        std::string got;
        for (auto const& r : scan.refusals) got += "\n    " + r.why;

        EXPECT_EQ(!scan.refusals.empty(), shape.refused)
            << shape.name << " was " << (shape.refused ? "ACCEPTED" : "REFUSED")
            << " and must be the other:\n  " << shape.why << "\n  body:\n"
            << shape.body << (got.empty() ? "" : "\n  refusals:" + got);
        EXPECT_EQ(scan.invocations, shape.invocations)
            << shape.name << ": the rule recognised " << scan.invocations
            << " invocation(s) (command position in a shell, an argv literal or a"
               " shell command line in Python), expected " << shape.invocations
            << ". The recogniser's BOUNDARY moved, which changes what the rule"
               " governs without changing what it says:\n  "
            << shape.why;
    }
}

// The same rule over the two places the spelling is DATA rather than script
// text — and they are the places that actually carried `--`: the catalogue's
// launcher argv, and the resolver's declared translator argv. Neither is a
// command line, so the line rule above cannot see them; both end up as argv[0]
// of a real process, so the defect is identical.
TEST_F(HarnessLegs, NoDeclaredWslArgvOmitsExec) {
    auto const isWsl = [](std::string const& s) {
        auto const  slash = s.find_last_of("/\\");
        std::string tail  = slash == std::string::npos ? s : s.substr(slash + 1);
        for (auto& c : tail) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return tail == "wsl" || tail == "wsl.exe";
    };

    std::size_t launchers = 0;
    auto const  doc       = json::parse(fileText(catalogue_));
    for (auto const& leg : doc.at("legs")) {
        auto const label = leg.at("label").get<std::string>();
        for (auto const& entry : leg.at("launchers")) {
            auto const cmd = entry.at("command").get<std::vector<std::string>>();
            if (cmd.empty() || !isWsl(cmd.front())) continue;
            ++launchers;
            ASSERT_GE(cmd.size(), 2u)
                << "leg '" << label << "': launcher for ("
                << entry.at("hostOs") << ", " << entry.at("hostArch")
                << ") is a bare `" << cmd.front() << "` with no `-e`.";
            EXPECT_TRUE(cmd[1] == "-e" || cmd[1] == "--exec")
                << "leg '" << label << "': launcher for ("
                << entry.at("hostOs") << ", " << entry.at("hostArch")
                << ") is declared as `" << cmd.front() << ' ' << cmd[1]
                << "`. Only `-e`/`--exec` EXECUTES the fixture; anything else"
                   " (including `--`) hands the whole argv to the distro's"
                   " default shell, which re-expands it. MEASURED: one argument"
                   " `…/g/*.test` arrived at the callee as TWO under `--` and as"
                   " ONE under `-e` — a launcher that cannot be trusted to have"
                   " run the file the driver named.";
        }
    }
    EXPECT_GE(launchers, 2u)
        << "no wsl launcher was found in the catalogue, so this pin is vacuous"
           " (both elf64 legs declare one for a Windows host).";

    // The resolver's own translator vocabulary, read the way a driver reads it.
    auto const r = run({"--path-translations"});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_EQ(r.exitCode, 0u) << r.output;
    std::size_t translators = 0;
    for (auto const& line : splitLines(r.output)) {
        auto const tab = line.find('\t');
        ASSERT_NE(tab, std::string::npos) << "malformed line: " << line;
        auto const verb = line.substr(0, tab);
        auto const argv = splitWords(line.substr(tab + 1));
        if (argv.empty() || !isWsl(argv.front())) continue;
        ++translators;
        EXPECT_TRUE(argv.size() >= 2 && (argv[1] == "-e" || argv[1] == "--exec"))
            << "pathTranslation '" << verb << "' declares translator argv `"
            << line.substr(tab + 1)
            << "`. Without `-e` the path is parsed by WSL's default shell before"
               " wslpath sees it, and a backslash is that shell's ESCAPE"
               " character — which is exactly how `wslpath: C:ab` came to be"
               " blamed on wslpath. MEASURED through the real call path (python"
               " subprocess.run): 'C:\\a\\b' is rc=1 `wslpath: C:ab` without"
               " `-e` and rc=0 /mnt/c/a/b with it.";
    }
    EXPECT_GE(translators, 1u)
        << "no wsl-based translator was found, so this half of the pin is"
           " vacuous (windows-to-wsl declares one).";
}

// ── 10. THE DRIVER NEVER NAMES THE ARTEFACT — THE COMPILER DOES ────────────
//
// ★ THE FIXTURE PATH ASSUMED THE POSIX ARTIFACT SPELLING.
//
// ✔MEASURED 2026-08-04, WSL x86_64, HEAD a3af1320: the .sh driver CROSS-BUILT the
// Windows testfixture — 189 TUs compiled, the link ran, ZERO `error[` and zero
// `error:`, and `…/pe64-x86_64-windows-exec/testfixture.exe` landed on disk
// (`file(1)`: PE32+ executable (console) x86-64, 8 sections, 5,387,264 bytes). The
// driver reported `build FAILED — 0 error[ but no executable at …/testfixture` and
// marked the leg POISONED: it was looking for a suffix-less name, because nothing
// in the build had ever told it what the artefact was CALLED.
//
// ★ WHY THIS IS WORSE THAN AN ORDINARY BUG, and why it earns a gate test. It is a
// false negative on the project's headline capability — build ANY target
// inside ANY host —
// manufactured by the instrument that measures it — and it hid ITSELF:
// only a POSIX host cross-building for Windows can reach it, which is exactly the
// case this harness exists to observe. On the arm64 VPS the leg never got that far;
// on Windows the .ps1 sibling had its own, different copy of the suffix table.
//
// THE THREE RULES BELOW, and each names a shape that was REMOVED:
//
//   1. NO SUFFIX TABLE. `TargetSpec::outputExtension`
//      (src/program/target_spec.cpp) derives the artefact extension from the
//      CLOSED object-format enum. The .ps1 carried a second copy —
//      `$sfx = if ($fmt -like 'pe*') { '.exe' } else { '' }` — matched on a format
//      NAME PREFIX, wrong for every non-exec format, and its own comment claimed
//      it was "DERIVED FROM THE OBJECT FORMAT, never hardcoded". The .sh carried a
//      third copy by having none at all. Three copies of one table is how they
//      came to disagree. A quoted bare extension used as a VALUE is a table entry;
//      one inside an `endswith(...)` TEST is not — it classifies a file name the
//      catalogue declared (a library name's format family) and cannot name
//      anything, which is why the twins' spelling of the same test (an unquoted
//      shell glob) never met this rule either.
//   2. NO DRIVER ASSEMBLES THE ARTEFACT PATH. `bin="$outd/$fmt/testfixture"` and
//      `Join-Path (Join-Path $legOut $fmt) "testfixture$sfx"` are the two shapes
//      that did; both put the per-format subdir variable and the artefact's base
//      name on ONE line, which is the shape this rule forbids. In Python the
//      per-format output directory is `fmt_dir = os.path.join(outd, leg.format)`,
//      so a per-format token is any name containing `fmt` or a `.format`
//      ATTRIBUTE (never the `str.format(` method), and the base names are the
//      artefacts this harness builds: `testfixture`, `sqlite3`, `speedtest1`.
//      (The gcc REFERENCE fixture, which make names and the driver legitimately
//      copies, never mentions the format.)
//   3. THE MARKER IS DEFINED ONCE, READ BY NAME, AND EMITTED BY THE COMPILER. It is
//      a WIRE FORMAT. `sqlite_base.ARTIFACT_MARKER` is its ONE spelling across the
//      Python that reads artefacts; every reader uses that constant; program.cpp
//      prints it. A second literal spelling of the PREFIX — `"dsscp: artifact "`
//      again, or an f-string with a placeholder after it — is a second definition
//      that can drift. Two spellings are NOT definitions, because neither can be
//      used to read anything: a whole report line in a self-test fixture (the
//      marker, then a spec — written out or as a placeholder — AND a path) is
//      DATA the reader is tested on, and a literal that is exactly the prefix and
//      an operand of `==`/`!=` is a PIN on the constant (the resolver's self-test
//      checks its imported prefix that way).
//
// ★ THE SET IS WIDER THAN THE TWINS' PAIR ({driver, base-harness}), on purpose:
// `harness_legs.py` (the loadext helper's artefact) and `speedtest1_bench.py` (the
// speedtest1 benchmark core) read artefacts too and were guarded by nothing. The
// readers are three suppliers, each held to rule 3's positive half: the driver
// with its core (`sqlite_base.py`, the port of `base-harness.{sh,ps1}`),
// `harness_legs.py`, and `speedtest1_bench.py`.
//
// RED-ON-DISABLE: restore a suffix table or a hand-assembled artefact path and
// rule 1 or 2 fails naming the line; spell the marker a second time, or read it
// anywhere but through `ARTIFACT_MARKER`, and rule 3 fails naming the site; delete
// the marker from program.cpp and the producer half fails.

namespace {

// Is the quoted text at `at` inside an `endswith(...)` call on this line?
[[nodiscard]] bool insideEndswith(std::string const& line, std::size_t at) {
    auto const call = line.rfind("endswith(", at);
    if (call == std::string::npos) return false;
    int depth = 1;
    for (std::size_t i = call + std::string_view{"endswith("}.size(); i < at; ++i) {
        if (line[i] == '(') ++depth;
        if (line[i] == ')' && --depth == 0) return false;
    }
    return true;
}

// Does the literal holding the marker END right after it — after, at most, ONE
// placeholder for the spec (`{spec}`, `%s`) and spaces? Then it spells the PREFIX:
// a definition, or a reader's needle, which is the same thing. Anything more — a
// concrete spec, or a placeholder followed by a path — is a whole report line: the
// DATA a self-test feeds a reader. (rule 3)
[[nodiscard]] bool spellsThePrefix(std::string const& line, std::size_t after) {
    std::size_t i = after;
    if (i < line.size() && line[i] == '{') {
        auto const close = line.find('}', i);
        if (close == std::string::npos) return false;
        i = close + 1;
    } else if (i < line.size() && line[i] == '%') {
        ++i;
        if (i < line.size() && line[i] == '(') {
            auto const close = line.find(')', i);
            if (close == std::string::npos) return false;
            i = close + 1;
        }
        while (i < line.size()
               && (std::string_view{"-+#0.123456789"}.find(line[i]) != std::string_view::npos)) {
            ++i;
        }
        if (i >= line.size() || std::isalpha(static_cast<unsigned char>(line[i])) == 0) {
            return false;
        }
        ++i;
    }
    while (i < line.size() && line[i] == ' ') ++i;
    return i < line.size() && (line[i] == '"' || line[i] == '\'');
}

// Is the literal that the marker at `at` STARTS, and that ends right after it, an
// operand of `==`/`!=`? Then it is an expected value checked against the
// constant — a pin — and not a spelling any reader could read with. (rule 3)
[[nodiscard]] bool comparedPrefixLiteral(std::string const& line, std::size_t at,
                                         std::size_t len) {
    if (at == 0) return false;
    char const quote = line[at - 1];
    if (quote != '"' && quote != '\'') return false;
    auto const close = at + len;
    if (close >= line.size() || line[close] != quote) return false;
    std::size_t left = at - 1;
    while (left > 0 && line[left - 1] == ' ') --left;
    if (left >= 2
        && (line.compare(left - 2, 2, "==") == 0 || line.compare(left - 2, 2, "!=") == 0)) {
        return true;
    }
    std::size_t right = close + 1;
    while (right < line.size() && line[right] == ' ') ++right;
    return line.compare(right, 2, "==") == 0 || line.compare(right, 2, "!=") == 0;
}

// Does this line name the per-format output directory? (rule 2)
[[nodiscard]] bool namesPerFormatDir(std::string const& line) {
    if (line.find("fmt") != std::string::npos) return true;
    for (auto at = line.find(".format"); at != std::string::npos;
         at = line.find(".format", at + 1)) {
        auto after = at + std::string_view{".format"}.size();
        while (after < line.size()
               && (std::isalnum(static_cast<unsigned char>(line[after])) != 0
                   || line[after] == '_')) {
            ++after;
        }
        while (after < line.size() && line[after] == ' ') ++after;
        if (after < line.size() && line[after] == '(') continue;   // a method call
        return true;
    }
    return false;
}

}  // namespace

TEST_F(HarnessLegs, NeitherDriverNamesTheArtefactTheCompilerDoes) {
    // The report line the compiler emits per artefact it commits:
    //   dsscp: artifact <targetSpec> <absolute path>
    constexpr char const* kMarker = "dsscp: artifact ";
    std::string_view const marker{kMarker};

    // DSS's artifact-extension table, as a reader would be tempted to spell one
    // entry of it: a bare, quoted extension. `wsl.exe` / `dsscp.exe` are
    // whole file names and never match; a `$sfx`-style row always does.
    constexpr std::string_view kSuffixes[] = {".exe", ".dll", ".so", ".dylib", ".lib"};
    constexpr char const*      kArtefactNames[] = {"testfixture", "sqlite3", "speedtest1"};

    struct Reader {
        std::string           name;
        std::vector<fs::path> files;
    };
    auto driver = driverModules();
    auto const missing = missingDriverModules(driver);
    ASSERT_TRUE(missing.empty()) << "driver modules missing: " << missing;
    driver.push_back(harnessDir() / "sqlite_base.py");
    std::vector<Reader> const readers{
        {"the driver (build_and_test.py, its modules, and its core sqlite_base.py)", driver},
        {"harness_legs.py", {harnessDir() / "harness_legs.py"}},
        {"speedtest1_bench.py", {harnessDir() / "speedtest1_bench.py"}}};
    std::vector<fs::path> all;
    for (auto const& r : readers) all.insert(all.end(), r.files.begin(), r.files.end());
    PyInspector::instance().prefetch(all);

    std::size_t definitions = 0, readersReading = 0;
    std::string definitionSites;
    bool        definedInTheCore = false;
    for (auto const& r : readers) {
        std::size_t reads = 0;
        for (auto const& file : r.files) {
            ASSERT_TRUE(fs::exists(file)) << file;
            auto const  name = file.filename().string();
            auto const& code = pythonCode(file);
            ASSERT_FALSE(code.live.empty()) << name << " has no live lines";
            for (auto const& [n, line] : code.live) {
                // RULE 1 — a bare quoted artifact extension used as a value is a
                // copy of DSS's table.
                for (auto const& sfx : kSuffixes) {
                    for (char const q : {'\'', '"'}) {
                        std::string const needle = q + std::string{sfx} + q;
                        for (auto at = line.find(needle); at != std::string::npos;
                             at = line.find(needle, at + 1)) {
                            if (insideEndswith(line, at)) continue;
                            ADD_FAILURE()
                                << name << ':' << n
                                << " spells an artifact extension as a literal:\n  " << line
                                << "\nThe suffix belongs to the object format"
                                   " (TargetSpec::outputExtension, keyed on the closed"
                                   " format enum) and the compiler REPORTS the file it"
                                   " wrote. A copy here is a second table to keep in"
                                   " step — and the last time there were two, they"
                                   " disagreed and threw away a real cross-host build.";
                        }
                    }
                }

                // RULE 2 — the per-format output subdir and an artefact's base name
                // on one line is a path being ASSEMBLED.
                if (namesPerFormatDir(line)) {
                    for (char const* artefact : kArtefactNames) {
                        EXPECT_EQ(line.find(artefact), std::string::npos)
                            << name << ':' << n << " assembles the artefact path itself:\n  "
                            << line
                            << "\nThe file name is the compiler's to decide and to REPORT"
                               " (`" << kMarker << "<spec> <path>`); a reader that"
                               " rebuilds it needs the extension table it must not have.";
                    }
                }

                // RULE 3 — every literal spelling of the marker PREFIX (the literal
                // ends there, or after one spec placeholder) is a definition.
                for (auto at = line.find(marker); at != std::string::npos;
                     at = line.find(marker, at + 1)) {
                    if (!spellsThePrefix(line, at + marker.size())) {
                        continue;   // a whole report line: fixture DATA, not a definition
                    }
                    if (comparedPrefixLiteral(line, at, marker.size())) {
                        continue;   // an expected value compared with the constant: a PIN
                    }
                    ++definitions;
                    definitionSites += "\n  " + name + ':' + std::to_string(n) + "  "
                                     + trimmedLeft(line);
                    definedInTheCore =
                        definedInTheCore
                        || (name == "sqlite_base.py"
                            && startsWith(trimmedLeft(line),
                                          "ARTIFACT_MARKER = \"" + std::string{marker} + "\""));
                }
                // A READ is any use of the constant by name — importing it from its
                // owner included — except the one line that defines it.
                if (line.find("ARTIFACT_MARKER") != std::string::npos
                    && line.find(marker) == std::string::npos) {
                    ++reads;
                }
            }
        }
        // RULE 3 (reader half) — each reader reads the report THROUGH THE ONE
        // CONSTANT. For the driver, its core satisfies it: after TF-C119 the reader
        // legitimately lives there, and demanding it in the driver's own modules
        // would forbid the extraction.
        EXPECT_GE(reads, 1u)
            << r.name << " never reads the build's report through"
               " sqlite_base.ARTIFACT_MARKER, so it is either not reading the"
               " compiler's own statement of what it wrote, or reading it through a"
               " spelling of its own that can drift from the one the compiler prints.";
        if (reads > 0) ++readersReading;
    }
    EXPECT_TRUE(definedInTheCore)
        << "sqlite_base.py no longer defines `ARTIFACT_MARKER = \"" << kMarker
        << "\"` in code — the ONE spelling of the wire format every reader uses.";
    EXPECT_EQ(definitions, 1u)
        << "the artefact marker's prefix is spelled " << definitions
        << " time(s) in the code that reads artefacts; it must be DEFINED ONCE"
           " (sqlite_base.ARTIFACT_MARKER) and used by name everywhere else:"
        << definitionSites;

    // RULE 3 (producer half) — and the compiler actually emits it. Without this
    // the readers could agree perfectly on a string nothing ever prints.
    //
    // ★ OVER LIVE LINES, and this was learned the hard way INSIDE this cycle,
    // exactly as `liveLines` itself was: the first version searched program.cpp's
    // raw text, and renaming the emitted marker left the test GREEN — because the
    // emitter DOCUMENTS the wire format it prints, and the docblock still spelled
    // it. `liveLines`' comment rules are shell/PowerShell/Python, so C++ needs its
    // own one-line strip here.
    auto const emitter = repoRoot() / "src" / "program" / "program.cpp";
    ASSERT_TRUE(fs::exists(emitter)) << emitter;
    std::string emitterCode;
    for (auto const& line : splitLines(fileText(emitter))) {
        auto const first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 2, "//") == 0) continue;
        emitterCode += line;
        emitterCode += '\n';
    }
    EXPECT_NE(emitterCode.find(kMarker), std::string::npos)
        << emitter.generic_string() << " does not emit `" << kMarker
        << "`. Every reader parses that exact prefix, so this is a wire format:"
           " renaming it here without renaming it there leaves every leg"
           " reporting 'the build produced no artefact' on a build that"
           " succeeded — the original defect, restored.";

    // NON-VACUITY, PER SUPPLIER. Rules 1 and 2 are NEGATIVE and go green by seeing
    // nothing, so the positive half carries the floor: it was "two drivers, at
    // least one reader each"; it is now each of the THREE readers, one each.
    EXPECT_EQ(readersReading, readers.size())
        << "only " << readersReading << " of the " << readers.size()
        << " artefact readers were seen reading the report. Fix the reader, do not"
           " delete the test.";
}

// The three rules' classifiers, proved on synthetic lines — each spelling beside the
// neighbour that must get the OTHER answer, because a classifier that answers the
// same thing for both passes every tree that happens not to contain the other.
TEST_F(HarnessLegs, TheArtefactRulesTellATableFromATestAndADefinitionFromData) {
    std::string const marker = "dsscp: artifact ";
    struct Line {
        char const* text;
        bool        definition;   // rule 3: spells the prefix and is not a pin
        char const* why;
    };
    Line const lines[] = {
        {"ARTIFACT_MARKER = \"dsscp: artifact \"", true, "the one definition's shape"},
        {"marker = f\"dsscp: artifact {spec} \"", true, "a reader's own needle, f-string"},
        {"needle = \"dsscp: artifact %s \" % spec", true, "a reader's own needle, %-format"},
        {"needle = \"dsscp: artifact \" + spec + \" \"", true, "a reader's own needle, joined"},
        {"_log = \"dsscp: artifact x86_64:pe64-x86_64-windows-dll /out/a.dll\\n\"", false,
         "a whole report line with a written-out spec: fixture DATA"},
        {"got = read(\"dsscp: artifact %s /out/a.dll\\n\" % spec, spec)", false,
         "a whole report line with a spec placeholder AND a path: fixture DATA"},
        {"check(\"pin\", PREFIX == \"dsscp: artifact \")", false,
         "an expected value compared with `==`: a PIN on the constant"},
        {"ok = \"dsscp: artifact \" != PREFIX", false, "the same pin, operand first, `!=`"},
    };
    for (auto const& l : lines) {
        std::string const text{l.text};
        auto const at = text.find(marker);
        ASSERT_NE(at, std::string::npos) << text;
        bool const definition = spellsThePrefix(text, at + marker.size())
                             && !comparedPrefixLiteral(text, at, marker.size());
        EXPECT_EQ(definition, l.definition) << text << "\n  " << l.why;
    }

    struct Suffix {
        char const* text;
        char const* needle;
        bool        tableEntry;   // rule 1: a quoted bare extension used as a VALUE
    };
    Suffix const suffixes[] = {
        {"if name.endswith(\".dll\"):", "\".dll\"", false},
        {"if name.lower().endswith((\".dll\", \".so\")):", "\".so\"", false},
        {"path = base + \".exe\"", "\".exe\"", true},
        {"cand = x + (\".exe\" if windows else \"\")", "\".exe\"", true},
        {"if a.endswith(\".so\") or b + \".so\":", "\".so\"", true},   // its SECOND use
    };
    for (auto const& s : suffixes) {
        std::string const text{s.text};
        bool entry = false;
        for (auto at = text.find(s.needle); at != std::string::npos;
             at = text.find(s.needle, at + 1)) {
            entry = entry || !insideEndswith(text, at);
        }
        EXPECT_EQ(entry, s.tableEntry) << text;
    }

    struct Join {
        char const* text;
        bool        perFormat;    // rule 2: names the per-format output directory
    };
    Join const joins[] = {
        {"bin = os.path.join(fmt_dir, \"testfixture\")", true},
        {"cli = os.path.join(outd, leg.format, \"sqlite3\")", true},
        {"msg = \"{} built\".format(label) + \" testfixture\"", false},
        {"log.info(\"testfixture built\")", false},
    };
    for (auto const& j : joins) {
        EXPECT_EQ(namesPerFormatDir(j.text), j.perFormat) << j.text;
    }
}

// ── 8. THE FOURTH, PER-LEG Tcl COHERENCE CHECK ─────────────────────────────
// [the Tcl HEADER is host-chosen while every leg's LIBRARY is pinned]
//
// THE DEFECT, ✔MEASURED 2026-08-06 by the first native macOS run of
// build-and-test.sh. The harness picks the Tcl HEADER from the HOST (tclsh on
// PATH -> its tclConfig.sh -> TCL_INCLUDE_SPEC) while EVERY leg's Tcl LIBRARY is
// pinned by its own target-keyed provider. On a Mac whose default Homebrew
// tcl-tk is 9.0.3 the fixture compiled against a 9.0 header and linked an 8.6
// library, and sqlite's tclsqlite.c gates live code on TCL_MAJOR_VERSION>8 — so
// the leg died on four K_SymbolUndefined (Tcl_GetBool, Tcl_GetBoolFromObj,
// Tcl_GetBytesFromObj, Tcl_GetChild) that a human had to reverse-engineer back
// to a version skew. On Linux the host tclsh is 8.6, so header and library had
// agreed BY ACCIDENT OF THE HOST — which is why hundreds of green runs on the
// same compiler never saw it.
//
// The harness already had THREE Tcl coherence checks and ALL THREE ARE HOST-
// SCOPED: interpreter-vs-staging, header-vs-tclConfig, recipe-vs-staging. Not
// one compared the staged header against the library a LEG WILL LINK. This
// section pins the missing FOURTH one, and the four properties that make it
// worth having:
//
//   1. IT IS PER-LEG AND IT LIVES IN THE SHARED RESOLVER, so it could not exist
//      in one twin driver and not the other — which is how library acquisition
//      came to be built for ONE leg, in ONE driver. The driver must actually CALL
//      it (`sqlite_libs.tcl_coherence`, from Step 6).
//   2. IT REFUSES, IT DOES NOT WARN. A warn ships a binary that links clean and
//      then misbehaves — the exact class this harness exists to prevent.
//   3. IT MEASURES THE LIBRARY'S BYTES, NOT ITS FILE NAME. `libtcl8.6.so` is
//      what somebody CALLED the file; a name being trusted is the whole anchor.
//   4. "CANNOT DETERMINE" IS A LOUD WARNING, NEVER A SILENT PASS.
//
// THE FIXTURE IS A SYNTHETIC ELF64, BUILT HERE, IN C++. The resolver's own
// self-test builds its own synthetic images in Python; this one is deliberately
// an INDEPENDENT implementation of the same container, so a reader defect that
// happened to match one builder's quirks cannot pass both. It carries only what
// the reader walks: .dynsym + .dynstr + .dynamic(DT_SONAME).

namespace {

void putLE(std::string& b, std::uint64_t v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

// A minimal ELF64 ET_DYN exporting `exports` and declaring DT_SONAME `soname`.
[[nodiscard]] std::string synthElfLibrary(std::vector<std::string> const& exports,
                                          std::string const&              soname) {
    std::string                          strtab(1, '\0');
    std::map<std::string, std::uint64_t> at;
    auto intern = [&](std::string const& s) -> std::uint64_t {
        if (s.empty()) return 0;
        auto it = at.find(s);
        if (it != at.end()) return it->second;
        auto const off = static_cast<std::uint64_t>(strtab.size());
        at.emplace(s, off);
        strtab += s;
        strtab.push_back('\0');
        return off;
    };
    for (auto const& e : exports) intern(e);
    auto const sonameOff = intern(soname);

    std::string syms(24, '\0');   // index 0 is the null symbol
    for (auto const& e : exports) {
        putLE(syms, at[e], 4);
        syms.push_back(static_cast<char>(0x12));   // STB_GLOBAL | STT_FUNC
        syms.push_back('\0');                      // st_other
        putLE(syms, 1, 2);                         // st_shndx: defined
        putLE(syms, 0, 8);                         // st_value
        putLE(syms, 0, 8);                         // st_size
    }
    std::string dyn;
    putLE(dyn, 14, 8);            // DT_SONAME
    putLE(dyn, sonameOff, 8);
    putLE(dyn, 0, 8);             // DT_NULL
    putLE(dyn, 0, 8);

    std::uint64_t const oStr = 64;
    std::uint64_t const oSym = oStr + strtab.size();
    std::uint64_t const oDyn = oSym + syms.size();
    std::uint64_t const oSh  = oDyn + dyn.size();

    std::string sh;
    auto section = [&](std::uint32_t type, std::uint64_t off, std::uint64_t size,
                       std::uint32_t link, std::uint64_t entsize) {
        putLE(sh, 0, 4);          // sh_name — the reader never reads section names
        putLE(sh, type, 4);
        putLE(sh, 0, 8);          // sh_flags
        putLE(sh, 0, 8);          // sh_addr
        putLE(sh, off, 8);
        putLE(sh, size, 8);
        putLE(sh, link, 4);
        putLE(sh, 0, 4);          // sh_info
        putLE(sh, 0, 8);          // sh_addralign
        putLE(sh, entsize, 8);
    };
    section(0, 0, 0, 0, 0);                            // SHT_NULL
    section(3, oStr, strtab.size(), 0, 0);             // 1 .dynstr
    section(11, oSym, syms.size(), 1, 24);             // 2 .dynsym
    section(6, oDyn, dyn.size(), 1, 16);               // 3 .dynamic

    std::string eh(64, '\0');
    eh[0] = 0x7F; eh[1] = 'E'; eh[2] = 'L'; eh[3] = 'F';
    eh[4] = 2;    // ELFCLASS64
    eh[5] = 1;    // ELFDATA2LSB
    eh[16] = 3;   // e_type = ET_DYN
    std::string tmp;
    putLE(tmp, oSh, 8);
    eh.replace(0x28, 8, tmp);
    tmp.clear();
    putLE(tmp, 64, 2);   // e_shentsize
    putLE(tmp, 4, 2);    // e_shnum
    eh.replace(0x3A, 4, tmp);
    return eh + strtab + syms + dyn + sh;
}

// The exact four names the fixture failed on. Spelled here rather than read out
// of the resolver so the two sides cannot agree on a set that discriminates
// nothing.
std::vector<std::string> const kTcl9Only = {"Tcl_GetBool", "Tcl_GetBoolFromObj",
                                            "Tcl_GetBytesFromObj",
                                            "Tcl_GetChild"};

[[nodiscard]] std::string tcl86Library() {
    return synthElfLibrary({"Tcl_CreateInterp", "Tcl_GetBoolean",
                            "Tcl_GetBooleanFromObj"},
                           "libtcl8.6.so");
}

[[nodiscard]] std::string tcl90Library() {
    std::vector<std::string> ex = kTcl9Only;
    ex.push_back("Tcl_CreateInterp");
    return synthElfLibrary(ex, "libtcl9.0.so");
}

void writeBytes(fs::path const& p, std::string const& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A staged tcl.h that declares exactly one version. Tcl 9 indents its
// `#   define`; 8.6 does not — both spellings appear below on purpose.
[[nodiscard]] fs::path writeTclHeader(fs::path const& dir, std::string const& body) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto const p = dir / "tcl.h";
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "#ifndef _TCL\n#define _TCL\n" << body << "#endif\n";
    return p;
}

}   // namespace

// 1. THE DRIVER CALLS IT. This is the anti-regression pin that matters most:
//    the harness already HAD three Tcl checks, and the defect was that none of
//    them was per-leg. A fourth check nobody invokes would be the same failure
//    wearing a newer hat — and while there were two twins, a fourth check only
//    ONE of them invoked was this project's canonical silent harness bug.
//    "The per-leg Tcl coherence check" is `sqlite_libs.py`: `tcl_coherence`
//    builds the call, and Step 6 (`step6`) makes it.
TEST_F(HarnessLegs, BothDriversRunThePerLegTclCoherenceCheck) {
    auto const libs = harnessDir() / "sqlite_libs.py";
    ASSERT_TRUE(fs::exists(libs)) << libs;
    auto const& code = pythonCode(libs);
    ASSERT_FALSE(code.live.empty()) << libs;
    // Over LIVE code: the module DOCUMENTS this check at length, and a raw-text
    // rule would be satisfied by the prose alone. And INSIDE the function that
    // makes the call, so a flag spelled in some other function cannot stand in.
    auto const body = functionBody(code.statements, "tcl_coherence");
    for (char const* flag : {"--tcl-coherence", "--staged-tcl-header",
                             "--leg-tcl-library"}) {
        EXPECT_NE(body.find(flag), std::string::npos)
            << "sqlite_libs.tcl_coherence passes no `" << flag
            << "` to harness_legs.py. The staged Tcl header is the ONE Tcl input this"
               " harness still takes from the HOST while every leg's library is"
               " pinned by its target-keyed provider; a driver that does not compare"
               " them builds a fixture against one Tcl and links another — a"
               " host-chosen header against pinned leg libraries.";
    }
    EXPECT_NE(functionBody(code.statements, "step6").find("tcl_coherence(run)"),
              std::string::npos)
        << "sqlite_libs.step6 never calls tcl_coherence(run), so the check exists and"
           " is never made — the same failure as no check, wearing a newer hat.";
}

// 2. THE DEFECT ITSELF, END TO END THROUGH THE CLI — with its MATCHED CONTROL.
//    The control is the point: the same two library files under an 8.6 header
//    must pass, or this test would go green on a check that refuses everything.
TEST_F(HarnessLegs, AStagedTclHeaderThatDisagreesWithALegsLibraryIsRefused) {
    auto const      dir = scratch_->path() / "tcl-skew";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto const lib86 = dir / "libtcl8.6.so";
    writeBytes(lib86, tcl86Library());
    ASSERT_TRUE(fs::exists(lib86)) << lib86;
    auto const h86 = writeTclHeader(dir / "inc86", "#define TCL_VERSION \"8.6\"\n");
    auto const h90 = writeTclHeader(dir / "inc90", "#   define TCL_VERSION\t\"9.0\"\n");

    // THE CONTROL — 8.6 header, 8.6 library, two legs. Must be silent.
    auto const ok = run({"--tcl-coherence", "--staged-tcl-header", h86.string(),
                         "--leg-tcl-library", "elf64-arm64=" + lib86.string(),
                         "--leg-tcl-library", "macho64-arm64=" + lib86.string()});
    ASSERT_TRUE(ok.spawned) << ok.diagnostic;
    EXPECT_EQ(ok.exitCode, 0u)
        << "a run whose staged header and every leg's library are the SAME Tcl"
           " must build. If this reds, the check refuses everything and the"
           " refusal below proves nothing.\n"
        << ok.output;
    EXPECT_NE(ok.output.find("elf64-arm64\t8.6"), std::string::npos) << ok.output;

    // THE DEFECT — the Mac's 9.0 header over the pinned 8.6 library.
    auto const bad = run({"--tcl-coherence", "--staged-tcl-header", h90.string(),
                          "--leg-tcl-library", "elf64-arm64=" + lib86.string()});
    ASSERT_TRUE(bad.spawned) << bad.diagnostic;
    EXPECT_EQ(bad.exitCode, 5u)
        << "a 9.0 staged header over a leg's PINNED 8.6 library must REFUSE."
           " Building anyway is what produced four K_SymbolUndefined on the"
           " first native macOS run.\n"
        << bad.output;
    // The diagnostic has to be actionable without reading the driver: the leg,
    // both versions, and the remedy the operator actually used.
    for (char const* needle : {"elf64-arm64", "9.0", "8.6", "DSS_TCL_VERSION=8.6",
                               "Tcl_GetBytesFromObj"}) {
        EXPECT_NE(bad.output.find(needle), std::string::npos)
            << "the refusal never says `" << needle
            << "`, so it sends its reader nowhere.\n"
            << bad.output;
    }

    // AND THE MIRROR IMAGE — an 8.6 header over a 9.0 library. Same skew, other
    // direction; a check that only knew one direction would be half a check.
    auto const lib90 = dir / "libtcl9.0.so";
    writeBytes(lib90, tcl90Library());
    ASSERT_TRUE(fs::exists(lib90)) << lib90;
    auto const rev = run({"--tcl-coherence", "--staged-tcl-header", h86.string(),
                          "--leg-tcl-library", "pe64-x86_64=" + lib90.string()});
    ASSERT_TRUE(rev.spawned) << rev.diagnostic;
    EXPECT_EQ(rev.exitCode, 5u) << rev.output;
}

// 3. THE HEADERS ARE STAGED ONCE FOR EVERY LEG, so legs that resolve DIFFERENT
//    Tcls are structurally incoherent whatever the header says. That is a
//    property of the RUN, and it must be named as such rather than reported as
//    "one leg is wrong".
TEST_F(HarnessLegs, LegsThatResolveDifferentTclVersionsAreStructurallyIncoherent) {
    auto const      dir = scratch_->path() / "tcl-split";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto const lib86 = dir / "libtcl8.6.so";
    auto const lib90 = dir / "libtcl9.0.so";
    writeBytes(lib86, tcl86Library());
    writeBytes(lib90, tcl90Library());
    ASSERT_TRUE(fs::exists(lib86) && fs::exists(lib90)) << dir;
    auto const h86 = writeTclHeader(dir / "inc", "#define TCL_VERSION \"8.6\"\n");

    auto const r = run({"--tcl-coherence", "--staged-tcl-header", h86.string(),
                        "--leg-tcl-library", "elf64-arm64=" + lib86.string(),
                        "--leg-tcl-library", "macho64-arm64=" + lib90.string()});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 5u) << r.output;
    EXPECT_NE(r.output.find("structurally incoherent"), std::string::npos)
        << "two legs resolved two Tcls and the refusal blamed a single leg."
           " One set of headers is staged for all of them, so no header can be"
           " correct for both — say that, or the operator pins the wrong one.\n"
        << r.output;
    for (char const* needle : {"elf64-arm64", "macho64-arm64"}) {
        EXPECT_NE(r.output.find(needle), std::string::npos) << r.output;
    }
}

// 4. THE SOFT OUTCOMES, WHICH ARE WHERE A CHECK LIKE THIS GOES QUIETLY VACUOUS.
//    "cannot determine" must be LOUD and must name the leg; and the version must
//    come from the library's BYTES, never from what the file is called.
TEST_F(HarnessLegs, AnUnmeasurableTclLibraryWarnsAndTheNameNeverDecidesTheVersion) {
    auto const      dir = scratch_->path() / "tcl-soft";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto const h86 = writeTclHeader(dir / "inc", "#define TCL_VERSION \"8.6\"\n");

    // (a) A file that is not an object file at all. Not a pass, not a refusal:
    //     a warning that names the leg.
    auto const junk = dir / "libtcl8.6.so";
    writeBytes(junk, "this is not an object file");
    ASSERT_TRUE(fs::exists(junk)) << junk;
    auto const soft = run({"--tcl-coherence", "--staged-tcl-header", h86.string(),
                           "--leg-tcl-library", "pe64-x86_64=" + junk.string()});
    ASSERT_TRUE(soft.spawned) << soft.diagnostic;
    EXPECT_EQ(soft.exitCode, 0u)
        << "a library whose version cannot be MEASURED is not a skew — refusing"
           " here would make an unreadable third-party binary kill a run that is"
           " otherwise fine.\n"
        << soft.output;
    EXPECT_NE(soft.output.find("WARN"), std::string::npos) << soft.output;
    EXPECT_NE(soft.output.find("pe64-x86_64"), std::string::npos) << soft.output;

    // (b) ★ THE ANCHOR'S OWN LESSON, AS A TEST. The file is CALLED libtcl9.0.so
    //     and its contents are an 8.6 library. If the check read the name it
    //     would pass; it reads the export table and the binary's own DT_SONAME,
    //     so it refuses under a 9.0 header and passes under an 8.6 one.
    auto const lying = dir / "libtcl9.0.so";
    writeBytes(lying, tcl86Library());
    ASSERT_TRUE(fs::exists(lying)) << lying;
    auto const h90 = writeTclHeader(dir / "inc90", "#define TCL_VERSION \"9.0\"\n");
    auto const byName = run({"--tcl-coherence", "--staged-tcl-header", h90.string(),
                             "--leg-tcl-library", "elf64-x86_64=" + lying.string()});
    ASSERT_TRUE(byName.spawned) << byName.diagnostic;
    EXPECT_EQ(byName.exitCode, 5u)
        << "a file NAMED libtcl9.0.so whose bytes are Tcl 8.6 was accepted under"
           " a 9.0 header. The check is reading the file name — which is the"
           " defect this anchor is about, one level down.\n"
        << byName.output;
    auto const byBytes = run({"--tcl-coherence", "--staged-tcl-header", h86.string(),
                              "--leg-tcl-library", "elf64-x86_64=" + lying.string()});
    ASSERT_TRUE(byBytes.spawned) << byBytes.diagnostic;
    EXPECT_EQ(byBytes.exitCode, 0u)
        << "the same file, whose BYTES are Tcl 8.6, must satisfy an 8.6 header"
           " however it happens to be named.\n"
        << byBytes.output;

    // (c) A staged header that states no version is a REFUSAL, not a pass: it is
    //     the one Tcl input taken from the host, and an unmeasurable one cannot
    //     be checked against anything.
    auto const mute = writeTclHeader(dir / "incmute",
                                     "#define TCL_PATCH_LEVEL \"8.6.14\"\n");
    auto const r = run({"--tcl-coherence", "--staged-tcl-header", mute.string(),
                        "--leg-tcl-library", "elf64-arm64=" + lying.string()});
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 2u) << r.output;
}
