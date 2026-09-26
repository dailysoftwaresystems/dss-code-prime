#pragma once

// THE CROSS-ARCH GATE of both corpus runners, ONE decision (see the block below).
// Its own header, not `arm_verdict_ledger.hpp`: that header is included by
// run_binary.hpp and stage_tree.hpp, so by targets that link no JSON library;
// this one is included by the two runners and its unit pins only.

#include "arm_verdict_ledger.hpp"
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace dss::test_support {

// ── THE CROSS-ARCH GATE: ONE DECISION FOR BOTH CORPUS RUNNERS ───────────────
//
// An arm whose target arch is not the host's cannot exec natively. Before this
// block the two runners each decided it inline, from the manifest alone: an
// `emulator` or a skip. That left no place for a fact about the HOST — and a
// host can run a foreign ISA by itself: macOS on Apple Silicon runs x86_64
// images under Rosetta 2. So every x86_64 Mach-O arm skipped on the one darwin
// leg as `SkippedNoEmulatorDeclared`, a MANIFEST verdict for a gap that is not
// the manifest's (D-TEST-EXAMPLES-X8664-MACHO-ARMS-NEVER-RUN-ON-THE-DARWIN-LEG).
//
// THE ORDER, and each step is a DECLARATION, never a branch on a name:
//   1. the host's OWN translation, from `tests/test_support/host_translations.json`
//      (keyed on the host tokens and the guest arch) — its `command` runs the
//      image, and each `requires` entry must exist, else the verdict is
//      `SkippedLauncherPrerequisiteMissing` (environmental: strict reds it),
//      whose reason names what is absent (the entry's `provides`), where
//      (its path) and, when declared, how to install it;
//   2. else the manifest's `emulator`, exactly as before: absent key →
//      `SkippedNoEmulatorDeclared`, absent from PATH → `SkippedEmulatorMissing`.
// The runners hold no copy of this: they call `crossArchDecision` and act on it.
struct HostTranslationRequirement {
    std::string kind;       // "directory" (the only kind declared today; another is refused by name)
    std::string path;
    std::string provides;   // WHAT the path is, in words: the skip reason names it, not only where it is
    std::string install;    // how an operator supplies it; optional, quoted in the skip reason when present
};

struct HostTranslation {
    std::string hostOs;
    std::string hostArch;
    std::string guestArch;
    std::vector<std::string> command;                    // argv prefix; command[0] a bare name on PATH
    std::vector<HostTranslationRequirement> requires_;   // beyond argv[0]
};

// Parse the table's text. Throws `std::runtime_error` naming `where` and the defect
// — a malformed row, an unknown key, an empty command, an unknown requirement
// kind, a requirement that does not say what it provides — because a
// translation table that silently loses a row would put its arms straight back
// to "skipped" with no one told why.
[[nodiscard]] inline std::vector<HostTranslation>
parseHostTranslations(std::string const& text, std::string const& where) {
    auto fail = [&where](std::string const& why) -> std::runtime_error {
        return std::runtime_error(where + ": " + why);
    };
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (nlohmann::json::exception const& e) {
        throw fail(std::string{"not JSON ("} + e.what() + ")");
    }
    if (!doc.is_object() || !doc.contains("translations") || !doc["translations"].is_array()) {
        throw fail("the document must be an object carrying a `translations` array");
    }
    std::vector<HostTranslation> out;
    for (auto const& row : doc["translations"]) {
        if (!row.is_object()) throw fail("a `translations` row is not an object");
        for (auto const& [key, value] : row.items()) {
            (void)value;
            if (key.starts_with("$")) continue;
            if (key != "hostOs" && key != "hostArch" && key != "guestArch" && key != "command"
                && key != "requires") {
                throw fail("a row carries the unknown key `" + key
                           + "` (the keys are hostOs, hostArch, guestArch, command, requires)");
            }
        }
        HostTranslation t;
        for (auto const* key : {"hostOs", "hostArch", "guestArch"}) {
            if (!row.contains(key) || !row[key].is_string() || row[key].get<std::string>().empty()) {
                throw fail(std::string{"a row needs a non-empty string `"} + key + "`");
            }
        }
        t.hostOs = row["hostOs"].get<std::string>();
        t.hostArch = row["hostArch"].get<std::string>();
        t.guestArch = row["guestArch"].get<std::string>();
        if (!row.contains("command") || !row["command"].is_array() || row["command"].empty()) {
            throw fail("the row for " + t.hostOs + "/" + t.hostArch + " -> " + t.guestArch
                       + " needs a non-empty `command` array");
        }
        for (auto const& a : row["command"]) {
            if (!a.is_string() || a.get<std::string>().empty()) {
                throw fail("a `command` word is not a non-empty string");
            }
            t.command.push_back(a.get<std::string>());
        }
        if (row.contains("requires")) {
            if (!row["requires"].is_array()) throw fail("`requires` must be an array");
            for (auto const& r : row["requires"]) {
                if (!r.is_object() || !r.contains("kind") || !r.contains("path") || !r["kind"].is_string()
                    || !r["path"].is_string()) {
                    throw fail("a `requires` entry needs a string `kind` and `path`");
                }
                for (auto const& [rkey, rvalue] : r.items()) {
                    (void)rvalue;
                    if (rkey.starts_with("$")) continue;
                    if (rkey != "kind" && rkey != "path" && rkey != "provides" && rkey != "why"
                        && rkey != "install") {
                        throw fail("a `requires` entry carries the unknown key `" + rkey
                                   + "` (the keys are kind, path, provides, why, install)");
                    }
                }
                auto const kind = r["kind"].get<std::string>();
                if (kind != "directory") {
                    throw fail("a `requires` entry of kind `" + kind
                               + "` has no check here (declared kinds: directory)");
                }
                // `provides` is REQUIRED: a prerequisite that is absent must be NAMED in the
                // arm's skip reason, and a bare path says where to look, not what is missing.
                if (!r.contains("provides") || !r["provides"].is_string()
                    || r["provides"].get<std::string>().empty()) {
                    throw fail("the `requires` entry for " + r["path"].get<std::string>()
                               + " needs a non-empty string `provides` (what it is, for the skip reason)");
                }
                if (r.contains("install") && !r["install"].is_string()) {
                    throw fail("the `requires` entry for " + r["path"].get<std::string>()
                               + " carries an `install` that is not a string");
                }
                t.requires_.push_back({kind, r["path"].get<std::string>(), r["provides"].get<std::string>(),
                                       r.contains("install") ? r["install"].get<std::string>() : std::string{}});
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

// The shipped table, read ONCE per process from the checkout. A missing or
// malformed file THROWS: a runner that silently fell back to "no translations"
// would skip the very arms this table exists to run, and say nothing.
[[nodiscard]] inline std::vector<HostTranslation> const& shippedHostTranslations() {
    static std::vector<HostTranslation> const table = [] {
        auto const path = ::dss::test::repoRoot() / "tests" / "test_support" / "host_translations.json";
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error(path.generic_string() + ": cannot be read");
        std::ostringstream text;
        text << in.rdbuf();
        return parseHostTranslations(text.str(), path.generic_string());
    }();
    return table;
}

// The row for (host, guest), if this host declares one.
[[nodiscard]] inline HostTranslation const*
hostTranslationFor(std::vector<HostTranslation> const& table, std::string const& hostOs,
                   std::string const& hostArch, std::string const& guestArch) {
    for (auto const& t : table) {
        if (t.hostOs == hostOs && t.hostArch == hostArch && t.guestArch == guestArch) return &t;
    }
    return nullptr;
}

struct CrossArchDecision {
    bool                     runs = false;   // true: run under `launcherPrefix`
    ArmVerdict               skip = ArmVerdict::SkippedNoEmulatorDeclared;   // when !runs
    std::string              why;            // the ledger detail, either way
    std::vector<std::string> launcherPrefix;
    // Whether the launcher EXECS the image. A host's own translation does: the
    // kernel admits the freshly written image and the OS translates it (Rosetta 2
    // under `arch -x86_64`), so `runBinary` must pay that admission in its untimed
    // warm-up. A manifest emulator does not: qemu-user reads the image as DATA.
    bool                     launcherExecsImage = false;
};

// THE decision, pure: every host fact and every machine probe is a parameter, so a
// unit test can ask it for a host this run is not on. `findExe` resolves a bare
// name on PATH ("" when absent); `dirExists` answers a `requires` directory.
template <typename FindExe, typename DirExists>
[[nodiscard]] CrossArchDecision
crossArchDecision(std::vector<HostTranslation> const& table, std::string const& hostOs,
                  std::string const& hostArch, std::string const& targetArch,
                  std::string const& manifestEmulator, FindExe findExe, DirExists dirExists) {
    CrossArchDecision d;
    if (auto const* t = hostTranslationFor(table, hostOs, hostArch, targetArch)) {
        std::string spelled;
        for (auto const& w : t->command) spelled += (spelled.empty() ? "" : " ") + w;
        auto const exe = findExe(t->command.front());
        if (exe.empty()) {
            d.skip = ArmVerdict::SkippedEmulatorMissing;
            d.why = "this host's own translation `" + spelled + "` for " + targetArch
                    + " images is declared, but `" + t->command.front() + "` is not on PATH";
            return d;
        }
        for (auto const& r : t->requires_) {
            if (!dirExists(r.path)) {
                d.skip = ArmVerdict::SkippedLauncherPrerequisiteMissing;
                d.why = "this host's own translation `" + spelled + "` for " + targetArch + " images needs "
                        + r.provides + " (the directory " + r.path + "), which is absent"
                        + (r.install.empty() ? std::string{} : "; install it with `" + r.install + "`");
                return d;
            }
        }
        d.runs = true;
        d.launcherExecsImage = true;
        d.launcherPrefix.push_back(exe);
        d.launcherPrefix.insert(d.launcherPrefix.end(), t->command.begin() + 1, t->command.end());
        d.why = "runs under this host's own translation `" + spelled + "` (target arch '" + targetArch
                + "' != host arch '" + hostArch + "')";
        return d;
    }
    if (manifestEmulator.empty()) {
        d.skip = ArmVerdict::SkippedNoEmulatorDeclared;
        d.why = "target arch '" + targetArch + "' != host arch '" + hostArch
                + "', this host declares no translation for it, and the manifest declares no 'emulator'";
        return d;
    }
    auto const emu = findExe(manifestEmulator);
    if (emu.empty()) {
        d.skip = ArmVerdict::SkippedEmulatorMissing;
        d.why = "declared emulator '" + manifestEmulator + "' is not on PATH (target arch '" + targetArch
                + "' != host arch '" + hostArch + "')";
        return d;
    }
    d.runs = true;
    d.launcherPrefix.push_back(emu);
    d.why = "runs under the manifest's emulator '" + manifestEmulator + "'";
    return d;
}

// The runners' entry: this host, the shipped table, PATH and the filesystem.
[[nodiscard]] inline CrossArchDecision crossArchDecisionForThisHost(std::string const& targetArch,
                                                                    std::string const& manifestEmulator) {
    return crossArchDecision(
        shippedHostTranslations(), currentHostOs(), currentHostArch(), targetArch, manifestEmulator,
        [](std::string const& name) { return findOnPath(name); },
        [](std::string const& dir) {
            std::error_code ec;
            return std::filesystem::is_directory(dir, ec);
        });
}

}  // namespace dss::test_support
