// The MSVC developer-environment ENTRY (`native_c_probe.hpp`), pinned from the parents a
// real process can have.
//
// A process started from a developer environment — a gate leg imports vcvars64 into the
// shell that runs ctest — hands that environment to the probe's one entry, and an entry
// that only ADDS to it grows PATH by what vcvars64 prepends each time. ✔MEASURED
// 2026-09-24 on the Windows gate host: 2,655 → 4,328 → 6,001 → 7,674 characters over three
// nested entries, and the entry from 7,674 died inside VsDevCmd with cmd.exe's
// line-too-long message and exit 255. Six native witnesses reddened
// NATIVE-PROBE-ENVIRONMENT-FAILED that way, and the entry's `>nul 2>&1` had thrown the
// message away. The entry now undoes a prior entry the way VsDevCmd's own `-clean_env`
// does, and keeps vcvars64's output for the detail of an entry that fails.
//
// THE PARENTS ARE SYNTHESIZED FROM THIS PROCESS'S OWN ENTRY rather than by entering again:
// every list VsDevCmd grows gets what the one entry added around its saved pre-entry value,
// again, the way a nested entry adds it — which reproduces the ladder above to the character
// on the gate host. Each arm enters in its own directory.
//
// ★★ HOW DEEP IS A PROPERTY OF cmd.exe's LINE LIMIT, NEVER A NUMBER OF LEVELS. The first cut
// nested three levels, the gate host's count, and it was wrong from Git Bash: a process
// started there carries a longer PATH (✔MEASURED 3,339 characters against PowerShell's
// 2,655), three levels came to 8,358 — PAST the limit — and there the kept entry does not die
// at all; it succeeds and silently loses the parent's PATH (kCmdLineLimit says why). So the
// arms nest to the two places the limit defines, from this process's own numbers: the deepest
// level cmd.exe can still expand, and one level past it.
//
// ★ THE CONTROL ARMS ARE WHAT MAKE THE PINS MEAN SOMETHING. At the limit, the batch WITHOUT
// the undo must DIE from the synthesized parent, with cmd.exe's own line-too-long text in its
// detail — read from a one-line reproduction run by the same cmd.exe, so the pin quotes this
// host's language and code page instead of hardcoding one. Past it, the same batch exits 0
// with the parent's PATH silently gone, and the entry must REFUSE that environment by name.
// A parent that reproduced neither would pass the pins beside them vacuously.
//
// ★★ AND PAST THE LIMIT IS A PRODUCT CASE, NOT ONLY A FIXTURE ONE. A parent that never entered
// but whose OWN PATH is past the limit gives the undo nothing to restore; ✔MEASURED, the entry
// exited 0 from 8,607 characters with the installation's directories only (1,486 characters,
// System32 gone). The entry now refuses an environment that did not keep the PATH it was
// entered from, and `AParentWhosePathIsPastTheLimitIsRefusedNotSilentlyDropped` pins it.
//
// Only an absent toolchain skips (the probe's rule); a located one that cannot be entered
// is red.

#include "native_c_probe.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace np = dss::test_support::native_probe;

// cmd.exe's line limit: "the maximum length of the string that you can use at the command
// prompt is 8191 characters" (DOCUMENTED, Microsoft's command-line string limitation). What
// cmd.exe does at it decides what a nested entry does, and it does two different things
// (✔MEASURED 2026-09-24 on the Windows gate host):
//   * a line whose expansion passes 8,191 characters DIES — `A linha de entrada é muito
//     longa.` and a syntax error, exit 255 — so a parent PATH at or under the limit that an
//     entry pushes past it kills the entry: the gate host's failure (2,655 → … → 7,674);
//   * a VARIABLE longer than 8,191 characters expands to NOTHING, silently: an entry from a
//     parent PATH already past the limit SUCCEEDS with every inherited PATH directory dropped
//     (a kept entry from 8,387 characters left 1,486, the installation's own directories).
constexpr std::size_t kCmdLineLimit = 8191;

using Variables = std::vector<std::wstring>;

[[nodiscard]] std::wstring widen(std::string_view s) { return {s.begin(), s.end()}; }

[[nodiscard]] std::wstring upper(std::wstring_view s) {
    std::wstring u;
    u.reserve(s.size());
    for (wchar_t const c : s) u.push_back(np::internal::asciiUpper(c));
    return u;
}

[[nodiscard]] std::size_t lengthOf(Variables const& vars, std::wstring_view name) {
    std::optional<std::wstring> const v = np::internal::variableIn(vars, name);
    return v ? v->size() : 0;
}

// Replace `name`'s value in `vars`, keeping the spelling of its name there; append it when
// absent. Names match the way Windows matches them.
void put(Variables& vars, std::wstring_view name, std::wstring const& value) {
    std::wstring const key = upper(name);
    for (std::wstring& v : vars) {
        std::size_t const eq = v.find(L'=');
        if (upper(std::wstring_view{v}.substr(0, eq)) == key) {
            v = v.substr(0, eq) + L"=" + value;
            return;
        }
    }
    vars.push_back(std::wstring{name} + L"=" + value);
}

// The process's one entry, and where it was made — the base every arm synthesizes from.
struct Base {
    np::MsvcLocation    loc;
    np::MsvcEnvironment once;
    fs::path            root;  // each arm makes its own directory under it
};

[[nodiscard]] Base const& base() {
    static dss::test_support::ScratchDir const scratch{dss::test_support::Location::Temp,
                                                       "msvc-entry"};
    static Base const b = [] {
        Base r;
        r.root = scratch.path();
        r.loc  = np::locateMsvcToolchain(r.root);
        if (r.loc.ok()) r.once = np::msvcEnvironment(r.loc, r.root);
        return r;
    }();
    return b;
}

[[nodiscard]] fs::path armDirectory(std::string_view arm) {
    fs::path const d = base().root / std::string{arm};
    fs::create_directories(d);
    return d;
}

// `once` as a parent that entered `levels` times. An entry WRAPS each list around what it
// held: VsDevCmd prepends most of what it adds and appends the rest — ✔MEASURED, PATH on
// the gate host: 1,292 characters before the saved pre-entry value and 381 after it (the
// CMake, Ninja, Linux connection-manager and vcpkg directories). So every list carries the
// one entry's prefix and suffix `levels` times around its saved value, and when nothing was
// saved the whole value and the `;` VsDevCmd's normalization took off it are the prefix.
// Empty, with `why`, when a list does not contain its saved value: then it is not VsDevCmd's
// shape and nothing here can nest it faithfully.
[[nodiscard]] Variables nestedParent(Variables const& once, int levels, std::string& why) {
    Variables parent = once;
    for (char const* list : np::internal::kVsDevCmdRestoredLists) {
        std::wstring const               name  = widen(list);
        std::optional<std::wstring> const value = np::internal::variableIn(once, name);
        if (!value) continue;  // the entry left no such list: nothing to nest
        std::wstring const saved =
            np::internal::variableIn(once, L"__VSCMD_PREINIT_" + name).value_or(L"");
        std::wstring prefix;
        std::wstring suffix;
        if (saved.empty()) {
            prefix = *value + L";";
        } else {
            // The saved value as it sits in the list: whole, or without the trailing `;`
            // the normalization removes when nothing was appended after it.
            std::wstring inner = saved;
            std::size_t  at    = value->find(inner);
            if (at == std::wstring::npos && inner.back() == L';') {
                inner.pop_back();
                at = value->find(inner);
            }
            if (at == std::wstring::npos) {
                why = std::string{list} + " (" + std::to_string(value->size())
                    + " characters) does not contain its saved pre-entry value ("
                    + std::to_string(saved.size()) + " characters)";
                return {};
            }
            prefix = value->substr(0, at);
            suffix = value->substr(at + inner.size());
        }
        std::wstring nested = *value;
        for (int i = 1; i < levels; ++i) nested = prefix + nested + suffix;
        put(parent, name, nested);
    }
    return parent;
}

// A nested parent, and the level it is nested to.
struct Nest {
    Variables   parent;
    int         levels = 0;
    std::string why;  // set iff `parent` is empty
};

// The two parents a nest of this process's own entry leads to, one for each thing cmd.exe
// does at its line limit (the block above kCmdLineLimit): AT the limit, the DEEPEST level
// whose PATH cmd.exe can still expand — so the entry from it, adding what an entry adds, must
// cross the limit and die, as the gate host's did; PAST it, one level deeper — a PATH cmd.exe
// silently expands to nothing. The level follows from this process's own pre-entry PATH, never
// from another host's: Git Bash hands a process a longer PATH than PowerShell does, and three
// levels, the gate host's number, are past the limit there.
[[nodiscard]] Nest nestAt(bool pastTheLimit) {
    Variables const& once = base().once.variables;
    Nest             at;
    for (int levels = 1; levels <= 64; ++levels) {
        Nest n;
        n.levels = levels;
        n.parent = nestedParent(once, levels, n.why);
        if (n.parent.empty()) return n;
        if (lengthOf(n.parent, L"PATH") > kCmdLineLimit) {
            if (at.levels == 0) {
                n.parent.clear();
                n.why = "the process's own entry already leaves a PATH past cmd.exe's line limit "
                        "(" + std::to_string(lengthOf(once, L"PATH")) + " characters), so no "
                        "parent can be nested up to it";
                return n;
            }
            return pastTheLimit ? n : at;
        }
        at = std::move(n);
    }
    Nest none;
    none.why = "64 levels of what the entry added to PATH never passed cmd.exe's line limit: "
               "the entry added nothing to PATH";
    return none;
}

[[nodiscard]] Nest const& nestUpToTheLimit() {
    static Nest const n = nestAt(/*pastTheLimit=*/false);
    return n;
}

[[nodiscard]] Nest const& nestPastTheLimit() {
    static Nest const n = nestAt(/*pastTheLimit=*/true);
    return n;
}

// What one entry adds to PATH: the process's own entry over its saved pre-entry value.
[[nodiscard]] std::size_t pathAddedByOneEntry() {
    Variables const& once = base().once.variables;
    return lengthOf(once, L"PATH") - lengthOf(once, L"__VSCMD_PREINIT_PATH");
}

void noteNest(char const* which, Nest const& n) {
    Variables const& once = base().once.variables;
    std::cout << "[   NOTE   ] parent nested " << which << " cmd.exe's line limit: " << n.levels
              << " level(s), PATH " << lengthOf(n.parent, L"PATH") << " characters over a pre-entry "
              << lengthOf(once, L"__VSCMD_PREINIT_PATH") << " (the process's own entry: "
              << lengthOf(once, L"PATH") << ", one entry adds " << pathAddedByOneEntry() << ")\n";
}

// Every variable whose value differs between `a` and `b` (or that only one has), as
// `NAME (lengths)` — values carry user paths, so only their lengths are printed — leaving
// out VsDevCmd's restore bookkeeping, the `__VSCMD_PREINIT_` variables its components add.
[[nodiscard]] std::string differences(Variables const& a, Variables const& b) {
    std::map<std::wstring, std::pair<std::optional<std::wstring>, std::optional<std::wstring>>>
        both;
    for (std::wstring const& v : a) {
        std::size_t const eq = v.find(L'=');
        both[upper(std::wstring_view{v}.substr(0, eq))].first = v.substr(eq + 1);
    }
    for (std::wstring const& v : b) {
        std::size_t const eq = v.find(L'=');
        both[upper(std::wstring_view{v}.substr(0, eq))].second = v.substr(eq + 1);
    }
    std::string out;
    for (auto const& [name, values] : both) {
        if (name.rfind(L"__VSCMD_PREINIT_", 0) == 0) continue;
        if (values.first == values.second) continue;
        auto const len = [](std::optional<std::wstring> const& v) {
            return v ? std::to_string(v->size()) : std::string{"absent"};
        };
        out += "\n    " + np::internal::asciiOf(name) + " (" + len(values.first) + " vs "
             + len(values.second) + " characters)";
    }
    return out;
}

// cmd.exe's own line-too-long message, as THIS host's cmd.exe prints it (language and code
// page), with the exit code it ends a batch with: a batch whose third line expands to
// 9,000 characters, past the 8,191 cmd.exe reads.
struct LineTooLong {
    std::intptr_t exit = -1;
    std::string   text;  // the first line it printed
};

[[nodiscard]] LineTooLong cmdLineTooLong(fs::path const& dir) {
    fs::path const bat = dir / "line_too_long.bat";
    fs::path const log = dir / "line_too_long.log";
    {
        std::ofstream b{bat, std::ios::binary};
        b << "@echo off\r\n"
          << "set \"X=" << std::string(100, 'x') << "\"\r\n"
          << "set \"X=%X%%X%%X%%X%%X%%X%%X%%X%%X%%X%\"\r\n"
          << "set \"X=%X%%X%%X%%X%%X%%X%%X%%X%%X%\"\r\n";
    }
    LineTooLong r;
    r.exit = np::internal::spawnInterpreter(
        np::internal::thisProcessComspec(),
        L"/d /c \"\"" + bat.wstring() + L"\" > \"" + log.wstring() + L"\" 2>&1\"", nullptr);
    std::ifstream in{log};
    std::getline(in, r.text);
    if (!r.text.empty() && r.text.back() == '\r') r.text.pop_back();
    return r;
}

}  // namespace

// The entry from `n` must leave every variable a tool reads exactly as the process's own entry
// left it — the environment no longer depends on what the parent did — and nothing it started
// may still hold its files when it returns.
void expectEntersAsTheShellDoes(Nest const& n, char const* arm) {
    Base const& b    = base();
    fs::path const work = armDirectory(arm);
    np::MsvcEnvironment const entered = np::internal::enterMsvcEnvironment(b.loc, work, &n.parent);
    ASSERT_TRUE(entered.ok())
        << "entering from a parent nested " << n.levels << " level(s) (PATH "
        << lengthOf(n.parent, L"PATH") << " characters) FAILED:\n"
        << entered.describe();
    EXPECT_EQ(differences(b.once.variables, entered.variables), "")
        << "the entry from the nested parent left an environment that differs from the "
           "process's own entry (process's vs nested) — PATH "
        << lengthOf(b.once.variables, L"PATH") << " vs " << lengthOf(entered.variables, L"PATH");
    std::error_code ec;
    fs::remove(work / "dss_msvc_environment.log", ec);
    EXPECT_FALSE(ec) << "the entry had returned, yet its log could not be deleted ("
                     << ec.message()
                     << "): something the entry started still holds it open";
}

// THE FIX, AT THE LIMIT. From the deepest nest whose PATH cmd.exe can still expand (the
// PREINIT variables set, an entry from it bound to cross the limit), the entry succeeds as the
// shell's does.
TEST(MsvcEnvironmentEntry, AParentNestedUpToTheLineLimitEntersAsTheShellDoes) {
    Base const& b = base();
    if (b.loc.toolAbsent()) GTEST_SKIP() << b.loc.detail;
    ASSERT_TRUE(b.loc.ok()) << b.loc.describe();
    ASSERT_TRUE(b.once.ok()) << b.once.describe();
    Nest const& n = nestUpToTheLimit();
    ASSERT_FALSE(n.parent.empty()) << "the nested parent could not be synthesized: " << n.why;
    ASSERT_GT(lengthOf(n.parent, L"__VSCMD_PREINIT_PATH"), 0u)
        << "the process's entry left no __VSCMD_PREINIT_PATH, so the parent is not one a "
           "VsDevCmd entry leaves";
    ASSERT_LE(lengthOf(n.parent, L"PATH"), kCmdLineLimit);
    ASSERT_GT(lengthOf(n.parent, L"PATH") + pathAddedByOneEntry(), kCmdLineLimit)
        << "an entry from this parent does not reach cmd.exe's line limit";
    noteNest("up to", n);
    expectEntersAsTheShellDoes(n, "nested-up-to-the-limit");
}

// THE CONTROL AT THE LIMIT. The batch without the undo — every entry before it — DIES from
// the same parent, and says why: exit code and cmd.exe's line-too-long text, both as this
// host's cmd.exe produces them for a line past its limit.
TEST(MsvcEnvironmentEntry, ControlAnEntryThatKeepsTheNestDiesSayingWhy) {
    Base const& b = base();
    if (b.loc.toolAbsent()) GTEST_SKIP() << b.loc.detail;
    ASSERT_TRUE(b.loc.ok()) << b.loc.describe();
    ASSERT_TRUE(b.once.ok()) << b.once.describe();
    Nest const& n = nestUpToTheLimit();
    ASSERT_FALSE(n.parent.empty()) << "the nested parent could not be synthesized: " << n.why;

    LineTooLong const tooLong = cmdLineTooLong(armDirectory("line-too-long"));
    ASSERT_NE(tooLong.exit, 0) << "a line past cmd.exe's limit did not fail the batch";
    ASSERT_FALSE(tooLong.text.empty()) << "cmd.exe printed nothing for a line past its limit";

    np::MsvcEnvironment const kept = np::internal::enterMsvcEnvironment(
        b.loc, armDirectory("kept-up-to-the-limit"), &n.parent, np::internal::PriorEntry::Kept);
    ASSERT_FALSE(kept.ok())
        << "an entry that KEEPS a parent nested " << n.levels << " level(s) (PATH "
        << lengthOf(n.parent, L"PATH")
        << " characters) succeeded, so this parent does not reproduce the gate host's "
           "failure and the pin beside this one proves nothing";
    EXPECT_EQ(kept.status, np::ProbeStatus::EnvironmentFailed) << kept.describe();
    EXPECT_NE(kept.detail.find("(exit " + std::to_string(tooLong.exit) + ")"), std::string::npos)
        << "cmd.exe ends a batch with exit " << tooLong.exit
        << " on a line past its limit; the failed entry says:\n" << kept.detail;
    EXPECT_NE(kept.detail.find(tooLong.text), std::string::npos)
        << "the failed entry's detail does not quote cmd.exe's line-too-long text ("
        << tooLong.text << "); it says:\n" << kept.detail;
}

// THE FIX, PAST THE LIMIT. One level deeper, PATH is longer than cmd.exe will expand; the
// undo restores the short saved one before anything expands it, and the entry succeeds as
// the shell's does.
TEST(MsvcEnvironmentEntry, AParentNestedPastTheLineLimitEntersAsTheShellDoes) {
    Base const& b = base();
    if (b.loc.toolAbsent()) GTEST_SKIP() << b.loc.detail;
    ASSERT_TRUE(b.loc.ok()) << b.loc.describe();
    ASSERT_TRUE(b.once.ok()) << b.once.describe();
    Nest const& n = nestPastTheLimit();
    ASSERT_FALSE(n.parent.empty()) << "the nested parent could not be synthesized: " << n.why;
    ASSERT_GT(lengthOf(n.parent, L"PATH"), kCmdLineLimit);
    noteNest("past", n);
    expectEntersAsTheShellDoes(n, "nested-past-the-limit");
}

// The detail an entry gives when it did not keep the PATH it was entered from — the words the
// check at the end of `enterMsvcEnvironment` uses.
constexpr char const* kDidNotKeepThePath = "did not keep the PATH it was entered from";

// THE CONTROL PAST THE LIMIT. The batch without the undo does not DIE from that parent —
// cmd.exe expands a PATH that long to nothing, so vcvars64 builds its own from an empty one
// and exits 0 — and the entry REFUSES what it built, by name, instead of handing on an
// environment with every inherited directory gone. Were the parent not past the limit, the
// kept entry would keep its PATH and this arm would go red.
TEST(MsvcEnvironmentEntry, ControlAnEntryThatKeepsAPathPastTheLimitIsRefusedForLosingIt) {
    Base const& b = base();
    if (b.loc.toolAbsent()) GTEST_SKIP() << b.loc.detail;
    ASSERT_TRUE(b.loc.ok()) << b.loc.describe();
    ASSERT_TRUE(b.once.ok()) << b.once.describe();
    Nest const& n = nestPastTheLimit();
    ASSERT_FALSE(n.parent.empty()) << "the nested parent could not be synthesized: " << n.why;

    np::MsvcEnvironment const kept = np::internal::enterMsvcEnvironment(
        b.loc, armDirectory("kept-past-the-limit"), &n.parent, np::internal::PriorEntry::Kept);
    ASSERT_FALSE(kept.ok())
        << "an entry that KEEPS a parent whose PATH is past cmd.exe's line limit ("
        << lengthOf(n.parent, L"PATH")
        << " characters) was handed on as a usable environment: either the parent does not "
           "reproduce the silent loss, or the entry no longer refuses it";
    EXPECT_EQ(kept.status, np::ProbeStatus::EnvironmentFailed) << kept.describe();
    EXPECT_NE(kept.detail.find(kDidNotKeepThePath), std::string::npos)
        << "the kept entry failed, but not for losing the PATH it was entered from:\n"
        << kept.detail;
}

// A parent that NEVER entered, whose own PATH is past cmd.exe's line limit: nothing is saved
// for the undo to restore, so no entry can keep that PATH — the environment is refused by name
// rather than handed on with the parent's directories (System32 among them) silently gone.
// ✔MEASURED before the check: exit 0 from 8,607 characters, the entered PATH 1,486, the
// installation's own directories only. The padding is directories that do not exist.
TEST(MsvcEnvironmentEntry, AParentWhosePathIsPastTheLimitIsRefusedNotSilentlyDropped) {
    Base const& b = base();
    if (b.loc.toolAbsent()) GTEST_SKIP() << b.loc.detail;
    ASSERT_TRUE(b.loc.ok()) << b.loc.describe();
    ASSERT_TRUE(b.once.ok()) << b.once.describe();

    // The process's own environment as a parent that never entered: every VsDevCmd list back
    // at its saved pre-entry value (cleared where it had none), the saved values and the
    // installation variables gone — then PATH padded past the limit.
    Variables parent;
    for (std::wstring const& v : b.once.variables) {
        std::wstring const name = upper(std::wstring_view{v}.substr(0, v.find(L'=')));
        if (name.rfind(L"__VSCMD_PREINIT_", 0) == 0 || name == L"VSINSTALLDIR" || name == L"DEVENVDIR"
            || name == L"VSCMD_VER" || name == L"VISUALSTUDIOVERSION")
            continue;
        parent.push_back(v);
    }
    for (char const* list : np::internal::kVsDevCmdRestoredLists) {
        std::wstring const name  = widen(list);
        std::wstring const saved =
            np::internal::variableIn(b.once.variables, L"__VSCMD_PREINIT_" + name).value_or(L"");
        if (!saved.empty()) {
            put(parent, name, saved);
            continue;
        }
        std::wstring const key = upper(name);
        std::erase_if(parent, [&](std::wstring const& v) {
            return upper(std::wstring_view{v}.substr(0, v.find(L'='))) == key;
        });
    }
    std::wstring path = np::internal::variableIn(parent, L"PATH").value_or(L"");
    for (int i = 0; path.size() <= kCmdLineLimit + 256; ++i)
        path += L";C:\\dss-absent-directory-" + std::to_wstring(i);
    put(parent, L"PATH", path);

    np::MsvcEnvironment const entered = np::internal::enterMsvcEnvironment(
        b.loc, armDirectory("never-entered-past-the-limit"), &parent);
    ASSERT_FALSE(entered.ok())
        << "an entry from a parent whose own PATH is " << path.size()
        << " characters — past cmd.exe's line limit — was handed on as a usable environment";
    EXPECT_EQ(entered.status, np::ProbeStatus::EnvironmentFailed) << entered.describe();
    EXPECT_NE(entered.detail.find(kDidNotKeepThePath), std::string::npos)
        << "the entry failed, but not for losing the PATH it was entered from:\n"
        << entered.detail;
    EXPECT_NE(entered.detail.find(std::to_string(path.size()) + " characters"), std::string::npos)
        << "the refusal does not name the length of the PATH it was entered from:\n"
        << entered.detail;
}

// A parent that entered ANOTHER installation does not choose the tools: vsdevcmd_start.bat
// trusts an inherited VSINSTALLDIR (and DevEnvDir), so the undo clears them as `-clean_env`
// does, and the entry is the located installation's, the same as the process's own.
TEST(MsvcEnvironmentEntry, AParentThatEnteredAnotherInstallationDoesNotChooseTheTools) {
    Base const& b = base();
    if (b.loc.toolAbsent()) GTEST_SKIP() << b.loc.detail;
    ASSERT_TRUE(b.loc.ok()) << b.loc.describe();
    ASSERT_TRUE(b.once.ok()) << b.once.describe();

    fs::path const work    = armDirectory("foreign");
    fs::path const another = work / "another-installation";
    fs::create_directories(another / "Common7" / "IDE");
    Variables parent = b.once.variables;
    put(parent, L"VSINSTALLDIR", another.wstring() + L"\\");
    put(parent, L"DevEnvDir", (another / "Common7" / "IDE").wstring() + L"\\");

    np::MsvcEnvironment const entered = np::internal::enterMsvcEnvironment(b.loc, work, &parent);
    ASSERT_TRUE(entered.ok()) << entered.describe();
    EXPECT_EQ(differences(b.once.variables, entered.variables), "")
        << "an entry from a parent naming another installation left an environment that "
           "differs from the process's own entry (process's vs this one)";
}
