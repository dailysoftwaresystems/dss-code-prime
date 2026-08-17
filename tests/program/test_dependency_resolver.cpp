// `dependsOn` RESOLUTION — `src/program/dependency_resolver.{hpp,cpp}` and its
// wiring into `Program::compileProject` (plan 06 §5.1, AP6).
//
// ★ EVERY PIN HERE DRIVES THE RESOLVER THROUGH `Program::compileProject`, WHICH
// IS ITS REAL INPUT PATH. The resolver's inputs are a `.dss-project.json` on
// disk, a target list, and an output base — all three arriving from the driver.
// A pin that constructed a `DependencyResolveRequest` by hand and skipped the
// manifest would be testing a shape the subject never receives: the
// canonicalization, the gate ORDER, the M4(b) source join and the per-target
// additions channel all live in the driver, and a hand-built request exercises
// none of them. The one thing that IS injected is the git seam, for the reason
// `git_acquire.hpp` records — two of B.4's four outcomes are network failures
// no test can reach deterministically through real git.
//
// ── WHAT BREAKS SILENTLY IN THIS SUBJECT, WHICH IS WHY THE PINS LOOK PARANOID ─
//
//   * THE DEPENDENCY'S OWN `targets[]` GETTING CONSULTED. That is the design
//     B.10 reversed, and its symptom is a REJECT of work that would have
//     succeeded — a portable dependency that merely has not listed arm64
//     refusing an arm64 consumer, with a message that reads as a capability
//     claim while reporting a bookkeeping gap.
//   * THE CONSUMABILITY GATE RUNNING AFTER THE FORMAT DERIVATION. ✔MEASURED,
//     `(elf, cli, x86_64)` has TWO qualifying formats (`-exec` and `-pie`);
//     uniqueness holds ONLY because `cli` is `NotConsumable` and 0xD01B rejects
//     first. Reorder them and the commonest profile in the repo becomes
//     genuinely ambiguous.
//   * `crossValidateTargetFormat` DROPPED FROM THE CANDIDATE FILTER. ✔MEASURED
//     over the shipped formats: on `elf` and on `macho`, kind + profile alone
//     leave TWO candidates (the two architectures), and only the machine/ABI
//     validation cuts each pair to one. `pe` is the sole kind that is unique
//     without it — so a PE-only pin would go green over a resolver that never
//     validated the architecture at all, i.e. over an aarch64 archive linked
//     into an x86_64 image. Every derivation pin below therefore covers an ELF
//     and a Mach-O consumer.
//   * THE MERGED SOURCE ORDER. `sourceFiles.front()`'s stem NAMES the artifact
//     when the manifest states no `artifactName`, so putting a `module`
//     dependency's sources first silently RENAMES the output binary — green
//     build, zero diagnostics, missing file.
//   * A TRANSITIVE ARTIFACT ABSORBED BY A BUILD THAT CANNOT ABSORB IT. An `ar`
//     archive records no import, so a shared library that stops at a staticlib
//     dependency never reaches the root's link and the reference is undefined
//     TWO HOPS from its cause.

#include "core/types/artifact_profile.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/dependency_resolver.hpp"
#include "program/git_acquire.hpp"
#include "program/program.hpp"

#include "diagnostic_count.hpp"
#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using dss::DependencyComposition;
using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::GitCommandResult;
using dss::Program;
using dss::kRegisteredArtifactProfiles;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace fs = std::filesystem;

namespace {

// ── target specs, spelled once ──────────────────────────────────────────────
//
// All four CROSS-EMIT, so every pin below stays live on every leg of the matrix
// rather than being `#if`-ed out on three of them. None of them spawns its
// output; the assertions are about what was RESOLVED, which is a host-
// independent property.
constexpr std::string_view kElfX64Exec   = "x86_64:elf64-x86_64-linux-exec";
constexpr std::string_view kElfArm64Exec = "arm64:elf64-aarch64-linux-exec";
constexpr std::string_view kMachoArm64Exec = "arm64:macho64-arm64-darwin-exec";

// The formats the derivation must land on for a `staticlib` dependency of each
// consumer above. Written out rather than derived, because a pin that computed
// the expected answer the same way the subject does would agree with the
// subject by construction.
constexpr std::string_view kElfX64StaticLib   = "elf64-x86_64-linux-staticlib";
constexpr std::string_view kElfArm64StaticLib = "elf64-aarch64-linux-staticlib";
constexpr std::string_view kMachoArm64StaticLib =
    "macho64-arm64-darwin-staticlib";

[[nodiscard]] std::string formatOf(std::string_view spec) {
    return std::string{spec.substr(spec.find(':') + 1)};
}

void writeText(fs::path const& p, std::string_view text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
    out.flush();
    ASSERT_TRUE(out) << "could not write fixture file " << p.generic_string();
}

[[nodiscard]] std::string readText(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

// JSON string quoting — `\` matters because Windows paths are interpolated into
// these manifests, and a raw one would be read as an escape introducer.
[[nodiscard]] std::string jsonQuote(std::string_view s) {
    std::string out = "\"";
    for (char const c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    out += '"';
    return out;
}

[[nodiscard]] std::string jsonArray(std::vector<std::string> const& quoted) {
    std::string out = "[";
    for (std::size_t i = 0; i < quoted.size(); ++i) {
        if (i != 0) out += ", ";
        out += quoted[i];
    }
    out += "]";
    return out;
}

[[nodiscard]] std::string jsonStringArray(std::vector<std::string> const& raw) {
    std::vector<std::string> quoted;
    quoted.reserve(raw.size());
    for (auto const& r : raw) quoted.push_back(jsonQuote(r));
    return jsonArray(quoted);
}

// One `.dss-project.json`. Optional keys are emitted only when non-empty: an
// ABSENT key and a present `[]` are two distinct inputs to the loader, and this
// file exercises both.
struct ManifestSpec {
    std::string              language = "c-subset";
    std::string              profile  = "cli";
    std::vector<std::string> targets;
    std::vector<std::string> sources;
    std::vector<std::string> dependsOn;   // RAW JSON objects
    std::string              artifactName;
    std::string              preBuildScripts;   // RAW JSON array
    std::string              postBuildScripts;  // RAW JSON array
};

[[nodiscard]] std::string renderManifest(ManifestSpec const& m) {
    std::string out = "{\n";
    out += "  \"language\": " + jsonQuote(m.language) + ",\n";
    out += "  \"artifactProfile\": " + jsonQuote(m.profile) + ",\n";
    out += "  \"targets\": " + jsonStringArray(m.targets) + ",\n";
    out += "  \"sources\": " + jsonStringArray(m.sources);
    if (!m.artifactName.empty()) {
        out += ",\n  \"artifactName\": " + jsonQuote(m.artifactName);
    }
    if (!m.dependsOn.empty()) {
        out += ",\n  \"dependsOn\": " + jsonArray(m.dependsOn);
    }
    if (!m.preBuildScripts.empty()) {
        out += ",\n  \"preBuildScripts\": " + m.preBuildScripts;
    }
    if (!m.postBuildScripts.empty()) {
        out += ",\n  \"postBuildScripts\": " + m.postBuildScripts;
    }
    out += "\n}\n";
    return out;
}

[[nodiscard]] std::string pathEntry(std::string_view p) {
    return "{\"path\": " + jsonQuote(p) + "}";
}

[[nodiscard]] std::string manifestPathIn(fs::path const& dir) {
    return (dir / std::string{dss::kDependencyManifestName}).generic_string();
}

// The whole message text for the FIRST diagnostic carrying `code`, or a marker
// naming the miss. Assertions below compare CONTENT rather than counts wherever
// the content is what the reader of the diagnostic actually needs.
[[nodiscard]] std::string messageFor(DiagnosticReporter const& rep,
                                     DiagnosticCode            code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return d.contextPrefix + d.actual;
    }
    return "<no diagnostic with that code>";
}

// Where a dependency artifact must land (U-9):
// `<consumer output base>/deps/<derived-dep-name>/<formatName>/<file>`.
[[nodiscard]] fs::path depArtifactDir(fs::path const& outBase,
                                      std::string_view depName,
                                      std::string_view formatName) {
    return outBase / std::string{dss::kDependencyOutputDirName}
         / std::string{depName} / std::string{formatName};
}

// ── the scripted git seam ───────────────────────────────────────────────────
//
// Deliberately minimal and LOCAL to this file: `test_dependency_git_cache.cpp`
// owns the four-outcome state machine, and re-testing it here would be a second
// copy of somebody else's subject. What this fake exists for is the DRIVER
// wiring — that `Program::setGitRunner` reaches the resolver, that a git
// `dependsOn` entry resolves into `.dss-deps/<name>` and composes, and that
// `--force-git-cache` arrives.
class FakeGitRunner final : public dss::IGitRunner {
public:
    // What a clone materializes at `dest`, relative-path → contents.
    std::map<std::string, std::string> tree;
    std::string clonedCommit = "commit-aaa";

    int cloneCalls    = 0;
    int fetchCalls    = 0;
    int checkoutCalls = 0;
    int revParseCalls = 0;
    std::vector<std::string> clonedUrls;

    bool isAvailable() override { return true; }

    GitCommandResult clone(std::string const& url, fs::path const& dest) override {
        ++cloneCalls;
        clonedUrls.push_back(url);
        std::error_code ec;
        fs::create_directories(dest, ec);
        for (auto const& [rel, body] : tree) {
            fs::path const p = dest / rel;
            fs::create_directories(p.parent_path(), ec);
            std::ofstream out{p, std::ios::binary | std::ios::trunc};
            out << body;
        }
        writeHead(dest, clonedCommit);
        return ok();
    }
    GitCommandResult fetch(fs::path const&, std::string const&) override {
        ++fetchCalls;
        return ok();
    }
    GitCommandResult checkout(fs::path const& dir, std::string const&) override {
        ++checkoutCalls;
        writeHead(dir, clonedCommit);
        return ok();
    }
    GitCommandResult revParse(fs::path const& dir, std::string const&) override {
        ++revParseCalls;
        std::ifstream in{dir / "HEAD"};
        if (!in) {
            GitCommandResult bad;
            bad.detail = "fatal: not a git repository: " + dir.generic_string();
            return bad;
        }
        GitCommandResult out;
        out.ok = true;
        std::getline(in, out.output);
        return out;
    }

private:
    static void writeHead(fs::path const& dir, std::string const& commit) {
        std::ofstream out{dir / "HEAD", std::ios::trunc};
        out << commit << "\n";
    }
    static GitCommandResult ok() {
        GitCommandResult r;
        r.ok = true;
        return r;
    }
};

// ── the self-spawn hook fixture ─────────────────────────────────────────────
//
// A hook program that writes a file AT A PATH RELATIVE TO ITS OWN WORKING
// DIRECTORY. The relative write is half the subject: U-7 says a dependency's
// hooks run in THAT dependency's directory, and an absolute path would make the
// pin pass under a driver that spawned hooks anywhere at all. Re-execs this
// test binary (the repo's established fixture mechanism — no external
// dependency, no second build target, an exact path via `argv[0]`).
constexpr std::string_view kHookWriteFlag = "--dss-dep-hook-write=";

int runHookFixture(int argc, char** argv) {
    std::string_view const arg{argv[1]};
    fs::path const target{std::string{arg.substr(kHookWriteFlag.size())}};
    if (target.empty() || argc < 3) return 91;
    std::error_code ec;
    if (!target.parent_path().empty()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) return 93;
    }
    std::ofstream out{target, std::ios::binary | std::ios::trunc};
    if (!out) return 94;
    out << argv[2];
    out.flush();
    return out ? 0 : 94;
}

[[nodiscard]] std::string selfExecutableForJson() {
    auto const& argvs = ::testing::internal::GetArgvs();
    if (argvs.empty()) return {};
    std::error_code ec;
    fs::path const abs = fs::absolute(fs::path{argvs[0]}, ec);
    return (ec ? fs::path{argvs[0]} : abs).generic_string();
}

[[nodiscard]] std::string hookEntryJson(std::string_view writeTarget,
                                        std::string_view contents) {
    return "[{\"run\": [" + jsonQuote(selfExecutableForJson()) + ", "
         + jsonQuote(std::string{kHookWriteFlag} + std::string{writeTarget})
         + ", " + jsonQuote(contents) + "]}]";
}

// ── a scratch `src/dss-config` mirror ───────────────────────────────────────
//
// Two of the six B.10 pins need a shipped-format inventory the real tree cannot
// produce: a SECOND candidate (0xD023) and NO candidate (0xD022). Both are
// properties of the config DIRECTORY, so the honest way to reach them is to
// give the compiler a different directory rather than to stub the derivation —
// the real `findShippedConfigDir` / `loadShipped` path runs unchanged, and the
// diagnostics come out of the real emit sites with real candidate names.
//
// The WHOLE tree is mirrored (85 files, ~2 MB), not just `object-formats/`,
// because `DSS_CONFIG_ROOT` is checked first and a set-but-miss FALLS THROUGH
// to the cwd walk — so a partial mirror would resolve targets and grammars from
// wherever the test binary's cwd happened to sit, which is precisely the
// "the mutant was never LOADED" failure the fail-closed rules exist to catch.
class ConfigMirror {
public:
    explicit ConfigMirror(fs::path const& root) : root_(root) {
        std::error_code ec;
        fs::path const dst = root_ / "src" / "dss-config";
        // ⚠⚠ CREATE THE PARENT, NEVER `dst` ITSELF — and this is a cross-leg
        // defect, not a style preference. `fs::copy(src, dst, recursive)` into a
        // directory that ALREADY EXISTS returns `File exists` on libstdc++
        // (✔MEASURED on MinGW gcc 13.2) while succeeding on MSVC. Pre-creating
        // `dst` therefore redded BOTH of this file's B.10 fail-closed pins —
        // `AmbiguousDerivationFailsClosedNamingEveryCandidate` and
        // `NoCandidateRejectsAtResolveTimeNamingTheAxis` — on every gcc leg,
        // while the Windows gate stayed green and could never see it.
        // ★ The failure shape is the dangerous one: the two tests that go dark
        // are the ones asserting that a zero-candidate and an ambiguous format
        // derivation each REJECT LOUDLY, so a leg without them is a leg where
        // B.10's central safety property is unverified — and the red looks like
        // a product failure, which invites "fixing" it by weakening the test.
        // Letting `fs::copy` create the leaf sidesteps the whole disagreement.
        fs::create_directories(dst.parent_path(), ec);
        fs::copy(dss::test::configRoot(), dst,
                 fs::copy_options::recursive
                     | fs::copy_options::overwrite_existing,
                 ec);
        ok_ = !ec;
        why_ = ec ? ec.message() : std::string{};
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::string const& why() const noexcept { return why_; }
    [[nodiscard]] fs::path formats() const {
        return root_ / "src" / "dss-config" / "object-formats";
    }
    // `DSS_CONFIG_ROOT` wants the directory CONTAINING `src/dss-config`.
    [[nodiscard]] std::string envValue() const { return root_.string(); }

private:
    fs::path    root_;
    bool        ok_ = false;
    std::string why_;
};

// ── the C sources every build here compiles ─────────────────────────────────
constexpr std::string_view kLeafSource  = "int dep_answer(void){ return 7; }\n";
constexpr std::string_view kMainSource =
    "extern int dep_answer(void);\nint main(void){ return dep_answer(); }\n";
// A `module` contribution: no entry point, absorbed into the consumer's CUs.
constexpr std::string_view kModuleSource = "int module_answer(void){ return 5; }\n";
constexpr std::string_view kMainUsingModule =
    "extern int module_answer(void);\nint main(void){ return module_answer(); }\n";

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// THE SIX B.10 PINS
// ════════════════════════════════════════════════════════════════════════════

// ── B.10 #1 — THE WHOLE POINT ───────────────────────────────────────────────
//
// A consumer targeting **arm64** with a dependency whose manifest lists **only
// x86_64** must RESOLVE AND BUILD. This is the case the rejected superset rule
// refuses, and refusing it is a FALSE NEGATIVE with a confidently misleading
// message: the dependency's source is perfectly portable, its manifest simply
// has not listed the platform, and `targets[]` states what a project builds FOR
// ITSELF rather than what its code can support.
//
// The `targets[]` in the dependency's manifest is not merely absent — it names
// a DIFFERENT architecture AND a different format kind, so a resolver that
// consulted it could not accidentally agree.
TEST(DependencyResolverB10, Arm64ConsumerBuildsAnX8664OnlyDependency) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "leafutil";

    writeText(dep / "lib.c", kLeafSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "staticlib",
                              // Deliberately a spec the CONSUMER never names.
                              .targets = {std::string{kElfX64StaticLib}
                                              .insert(0, "x86_64:")},
                              .sources = {"lib.c"}}));

    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfArm64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "an arm64 consumer must build an x86_64-only dependency: `targets[]` "
           "is what a project builds for ITSELF, not a capability claim.\n"
        << messageFor(rep, DiagnosticCode::D_DependencyTargetFormatUnresolvable);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyTargetFormatUnresolvable), 0u);

    // POSITIVE, not merely "no reject": the dependency really was built, for
    // the CONSUMER's architecture, and its artifact is where U-9 says.
    fs::path const artifactDir =
        depArtifactDir(dir / "out", "leafutil", kElfArm64StaticLib);
    EXPECT_TRUE(fs::exists(artifactDir / "lib.a"))
        << "expected the dependency's archive at "
        << (artifactDir / "lib.a").generic_string();
    EXPECT_TRUE(fs::exists(dir / "out" / formatOf(kElfArm64Exec) / "main"))
        << "the consumer's own artifact must exist too";
}

// ── B.10 #2 — THE EXACT DERIVED FORMAT, ON THE TWO KINDS THAT NEED IT ───────
//
// ✔MEASURED over the shipped formats: filtering on kind + served profile alone
// leaves TWO candidates for an `elf` staticlib (aarch64 and x86_64) and TWO for
// a `macho` staticlib — only `crossValidateTargetFormat` cuts each pair to one.
// `pe` is unique WITHOUT that clause, so a PE-only pin would pass over a
// resolver that never validated the architecture, i.e. over an aarch64 archive
// fed into an x86_64 link. Both kinds that discriminate are covered here.
//
// The assertion is the EXACT resolved format STRING — read off the artifact's
// own directory, which is `<formatName>` by U-9's layout — never merely
// "the build succeeded".
TEST(DependencyResolverB10, DerivedFormatIsTheUniqueOneForConsumerAndProfile) {
    struct Case {
        std::string_view consumer;
        std::string_view expectedDepFormat;
        std::string_view label;
    };
    constexpr Case kCases[] = {
        {kElfX64Exec, kElfX64StaticLib, "elf x86_64"},
        {kElfArm64Exec, kElfArm64StaticLib, "elf aarch64"},
        {kMachoArm64Exec, kMachoArm64StaticLib, "macho arm64"},
    };

    for (auto const& c : kCases) {
        SCOPED_TRACE(std::string{c.label});
        ScratchDir scratch{Location::Temp, "dep-resolver"};
        fs::path const dir = scratch.path();
        fs::path const dep = dir / "leafutil";

        writeText(dep / "lib.c", kLeafSource);
        writeText(fs::path{manifestPathIn(dep)},
                  renderManifest({.profile = "staticlib",
                                  .targets = {std::string{kElfX64Exec}},
                                  .sources = {"lib.c"}}));
        writeText(dir / "main.c", kMainSource);
        fs::path const proj = dir / "app.dss-project.json";
        writeText(proj,
                  renderManifest({.targets   = {std::string{c.consumer}},
                                  .sources   = {(dir / "main.c").generic_string()},
                                  .dependsOn = {pathEntry(dep.generic_string())}}));

        Program prog;
        prog.setOutputDir(dir / "out");
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
            << messageFor(rep, DiagnosticCode::D_DependencyTargetFormatUnresolvable)
            << "\n"
            << messageFor(rep, DiagnosticCode::D_DependencyTargetFormatAmbiguous);

        // The per-target additions channel carries the artifact, and its PATH
        // spells the derived format. Asserting the map (rather than only the
        // filesystem) is what proves the artifact was threaded to the consumer
        // and not merely written somewhere.
        auto const& byTarget = prog.resolveLibraryAdditionsByTarget();
        auto const  it       = byTarget.find(std::string{c.consumer});
        ASSERT_NE(it, byTarget.end())
            << "no per-target additions were recorded for the consumer target";
        ASSERT_EQ(it->second.size(), 1u);
        std::string const parent =
            it->second.front().path.parent_path().filename().string();
        EXPECT_EQ(parent, std::string{c.expectedDepFormat})
            << "the dependency was built with the WRONG object format; the "
               "derivation must be unique for (kind of the consumer's format, "
               "the DEPENDENCY's profile, machine/ABI validity against the "
               "consumer's target). Full path: "
            << it->second.front().path.generic_string();
        EXPECT_TRUE(fs::exists(it->second.front().path))
            << "the artifact the consumer was told to link does not exist";
    }
}

// ── B.10 #3 — NON-UNIQUENESS FAILS CLOSED, NAMING EVERY CANDIDATE ──────────
//
// ✔No shipped configuration can produce two candidates, which is exactly why
// this pin gives the compiler a DIFFERENT configuration directory rather than
// stubbing the derivation: a second `staticlib` format of the same kind and the
// same machine is one file, and everything downstream of it — the search, the
// reject, the message — is the shipped code path.
//
// THREE-SIDED: the build fails, exactly one 0xD023 is emitted, and the message
// names the consumer target AND BOTH candidates. Counting alone would be
// satisfied by a message that named neither, which is the one thing the reader
// cannot work around.
TEST(DependencyResolverB10, AmbiguousDerivationFailsClosedNamingEveryCandidate) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();

    ConfigMirror mirror{dir / "cfg"};
    ASSERT_TRUE(mirror.ok()) << "could not mirror the shipped config: "
                             << mirror.why();
    // The twin: byte-identical to the real x86_64 ELF staticlib format, under a
    // second name. Same kind, same machine, same served profile ⇒ a genuine
    // second answer to the derivation's question.
    std::string const twinName = "elf64-x86_64-linux-staticlib-twin";
    std::string const original =
        readText(mirror.formats() / (std::string{kElfX64StaticLib} + ".format.json"));
    ASSERT_FALSE(original.empty()) << "the mirrored format document is empty";
    // The document's own `name` field is not what `loadShipped` keys on (the
    // FILENAME is), so the copy needs no edit at all — and leaving it unedited
    // keeps the twin provably identical in every field the derivation reads.
    writeText(mirror.formats() / (twinName + ".format.json"), original);

    fs::path const dep = dir / "leafutil";
    writeText(dep / "lib.c", kLeafSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "staticlib",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"lib.c"}}));
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    ScopedEnv const configRoot{"DSS_CONFIG_ROOT", mirror.envValue()};
    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0)
        << "two candidate formats must FAIL the build, never silently pick one";
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyTargetFormatAmbiguous), 1u);

    std::string const m =
        messageFor(rep, DiagnosticCode::D_DependencyTargetFormatAmbiguous);
    EXPECT_NE(m.find(std::string{kElfX64Exec}), std::string::npos)
        << "the message must name the CONSUMER TARGET; got: " << m;
    EXPECT_NE(m.find(std::string{kElfX64StaticLib}), std::string::npos)
        << "the message must name candidate 1; got: " << m;
    EXPECT_NE(m.find(twinName), std::string::npos)
        << "the message must name candidate 2 — the remediation is to look at "
           "exactly those formats and decide which should have declined the "
           "profile, which is impossible from a message naming only a count; "
           "got: "
        << m;
    // No artifact may be produced for a refused derivation.
    EXPECT_FALSE(fs::exists(dir / "out" / formatOf(kElfX64Exec) / "main"));
}

// ── B.10 #4 — NO COMPATIBLE FORMAT REJECTS AT RESOLVE TIME, NAMING THE AXIS ─
//
// The mirror again, this time with BOTH ELF staticlib documents removed, so an
// ELF consumer's `staticlib` dependency has genuinely zero producers. The pin
// is on the MESSAGE, not the rejection: the three coordinates the search ran
// over ARE the search key, and without them the reader has a dead end rather
// than a claim they can check against the config directory in one pass.
TEST(DependencyResolverB10, NoCandidateRejectsAtResolveTimeNamingTheAxis) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();

    ConfigMirror mirror{dir / "cfg"};
    ASSERT_TRUE(mirror.ok()) << mirror.why();
    std::error_code ec;
    for (auto const& gone : {kElfX64StaticLib, kElfArm64StaticLib}) {
        ASSERT_TRUE(fs::remove(
            mirror.formats() / (std::string{gone} + ".format.json"), ec))
            << "the fixture must actually REMOVE " << gone
            << " or the pin proves nothing";
    }

    fs::path const dep = dir / "leafutil";
    writeText(dep / "lib.c", kLeafSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "staticlib",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"lib.c"}}));
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    ScopedEnv const configRoot{"DSS_CONFIG_ROOT", mirror.envValue()};
    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyTargetFormatUnresolvable),
              1u);
    // ★ AND NOT THE BUILD-FAILURE CODE — the other half of the split this pin
    // is now paired with (see the 0xD022-vs-0xD029 section below). Nothing was
    // compiled here: the derivation came back with zero candidates, so the
    // dependency never reached `buildNode_`. Without this clause the two facts
    // could drift back onto one ordinal and this pin would not notice, which is
    // exactly how they came to share one in the first place.
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyBuildFailed), 0u)
        << "a zero-candidate derivation is not a failed build; the dependency "
           "was never compiled at all";

    std::string const m =
        messageFor(rep, DiagnosticCode::D_DependencyTargetFormatUnresolvable);
    EXPECT_NE(m.find(std::string{kElfX64Exec}), std::string::npos)
        << "must name the consumer target; got: " << m;
    EXPECT_NE(m.find("staticlib"), std::string::npos)
        << "must name the DEPENDENCY's artifactProfile; got: " << m;
    EXPECT_NE(m.find("'elf'"), std::string::npos)
        << "must name the format KIND the search ran over; got: " << m;
}

// ── B.10 #5 — 0xD01B STILL FIRES, AND STILL FIRES FIRST ────────────────────
//
// Proves the derivation did not swallow a reject that already worked — and, in
// the same breath, pins the ORDER the derivation depends on. ✔MEASURED,
// `(elf, cli, x86_64)` has TWO qualifying formats (`-exec` and `-pie`), so if
// the derivation ran ahead of the consumability gate this manifest would emit
// 0xD023 instead. Asserting BOTH derivation codes are absent is what makes the
// ordering observable rather than incidental.
TEST(DependencyResolverB10, TerminalProfileStillRejectsAndRejectsBeforeDerivation) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "someapp";

    writeText(dep / "tool.c", "int main(void){ return 0; }\n");
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "cli",   // terminal — NotConsumable
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"tool.c"}}));
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    EXPECT_EQ(
        countCode(rep, DiagnosticCode::D_DependencyArtifactProfileUnsupported), 1u)
        << "a terminal profile named in `dependsOn` must still be rejected";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyTargetFormatAmbiguous), 0u)
        << "the consumability gate must run BEFORE the format derivation: "
           "(elf, cli, x86_64) has two qualifying formats, so a derivation that "
           "ran first would report an ambiguity instead of the real problem";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyTargetFormatUnresolvable),
              0u);
    EXPECT_FALSE(fs::exists(dir / "out" / formatOf(kElfX64Exec) / "main"));
}

// ── B.10 #6 — THE RETIRED SEMANTIC HAS NO REFERENCES, AND THE GUARD IS GREEN ─
//
// `D_DependencyTargetNotBuilt` was designed, measured to be UNSATISFIABLE, and
// then never minted — the *semantic* was dropped while the slot stayed free.
// The failure this guards is a later reader re-deriving the rejected rule from
// a stale mention: a name that appears nowhere cannot be revived by accident.
// A source scan is the only instrument that can assert the absence of a symbol
// that does not exist, since a test cannot reference one.
TEST(DependencyResolverB10, RetiredDependencyTargetNotBuiltSemanticIsUnreferenced) {
    fs::path const root = dss::test::repoRoot();
    constexpr std::string_view kRetired = "DependencyTargetNotBuilt";

    std::vector<std::string> hits;
    std::error_code          ec;
    for (auto const& sub : {"src", "tests"}) {
        for (fs::recursive_directory_iterator it{root / sub,
                                                 fs::directory_options::none, ec},
             end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string const ext = it->path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".json") continue;
            // This file NAMES the retired semantic in order to search for it;
            // excluding itself by path is the only exclusion, and it is stated
            // rather than pattern-matched.
            if (it->path().filename() == "test_dependency_resolver.cpp") continue;
            if (readText(it->path()).find(kRetired) != std::string::npos) {
                hits.push_back(it->path().lexically_relative(root).generic_string());
            }
        }
    }
    EXPECT_TRUE(hits.empty())
        << "the retired `D_DependencyTargetNotBuilt` semantic is referenced in "
        << hits.size()
        << " file(s); it was measured UNSATISFIABLE and dropped, and a lingering "
           "mention is how a later cycle re-derives the rejected superset rule. "
           "First: "
        << (hits.empty() ? std::string{} : hits.front());
}

// ════════════════════════════════════════════════════════════════════════════
// THE DERIVATION-GAP / BUILD-FAILURE SPLIT — 0xD022 vs 0xD029
// (D-DEPS-BUILD-FAILURE-REUSES-THE-DERIVATION-UNRESOLVABLE-CODE)
// ════════════════════════════════════════════════════════════════════════════
//
// ★ TWO FACTS, PINNED SEPARATELY AND EACH ASSERTING THE OTHER'S CODE IS ABSENT,
// BECAUSE THAT IS THE ONLY SHAPE THAT CAN CATCH THE DEFECT THAT WAS HERE.
// `buildNode_` used to report a dependency's FAILED BUILD as
// `D_DependencyTargetFormatUnresolvable` — a code allocated for the ZERO-
// CANDIDATE outcome of the format derivation. Both facts made the resolve fail,
// both named the dependency, and both produced a non-zero rc, so every
// "an error fired" assertion in this file stayed green over them. What the two
// do NOT share is the remediation: one is fixed by editing a manifest or
// shipping a backend, the other by fixing source code — and an operator running
// `--suppress=D_DependencyTargetFormatUnresolvable` to quiet a known config gap
// was silently muting compile failures too.
//
// So the discriminating assertion is EXCLUSIVE, not merely positive: each pin
// asserts its own code exactly once AND the sibling code exactly zero times.
// Merging the two codes back together fails both pins in the same run, from
// opposite directions.
//
//   * B.10 #4 above (`NoCandidateRejectsAtResolveTimeNamingTheAxis`) owns the
//     DERIVATION-GAP half and carries the 0xD029-is-absent clause;
//   * the pin below owns the BUILD-FAILURE half.

// ── THE DEPENDENCY COMPILES AND FAILS ───────────────────────────────────────
//
// A REAL compile failure in a REAL dependency build: `nosuchsymbol` is an
// undeclared identifier, so the dependency's own sub-build returns non-zero
// after the derivation has already succeeded and picked its format. Nothing is
// stubbed — the format inventory is the shipped one, and the dependency really
// is compiled for the consumer's target before it fails.
//
// The `staticlib` profile is load-bearing rather than incidental: it composes as
// `ArtifactLink`, which is what routes the dependency through `buildNode_`'s own
// `Program`. A `module` dependency is `SourceMerge` — its sources are folded
// into the CONSUMER's compile, so a broken one fails in the consumer's build and
// never reaches this emit site at all.
//
// Three things are asserted, and the third is the one most likely to be
// regressed by someone "simplifying" the failure path:
//   (1) the new code fires exactly once;
//   (2) 0xD022 does NOT fire — the derivation succeeded, and saying otherwise
//       is the defect this pin exists for;
//   (3) the INNER diagnostics are still merged into the caller's reporter with
//       the `[dependency=<outputName> target=<derived spec>] ` prefix. That
//       attribution is the load-bearing half of this whole design — it is what
//       lets a failure be reported AT THE DEPENDENCY EDGE instead of two hops
//       from its cause — and the summary line is worthless without it, since it
//       says the reason is "in the diagnostic(s) above".
TEST(DependencyResolverBuildFailure, DependencyCompileFailureIsItsOwnCodeNotTheDerivationGap) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "leafbad";

    // Real source, real compile, real failure: `nosuchsymbol` is undeclared.
    writeText(dep / "lib.c", "int dep_answer(void){ return nosuchsymbol; }\n");
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "staticlib",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"lib.c"}}));
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0)
        << "the dependency's source does not compile — the build must fail";

    // (1) THE NEW CODE, exactly once.
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyBuildFailed), 1u)
        << "a dependency that compiled and FAILED must report its own code";
    // (2) AND NOT THE DERIVATION GAP. The derivation ran and succeeded — it is
    // what chose `elf64-x86_64-linux-staticlib` — so reporting "no shipped
    // object format can build this dependency" would be a confident falsehood
    // pointing the reader at the format inventory instead of at their code.
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyTargetFormatUnresolvable),
              0u)
        << "a FAILED BUILD is not a zero-candidate derivation; these two facts "
           "have different remediations and must not share an ordinal";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyTargetFormatAmbiguous), 0u);

    // The message still names all three coordinates a reader needs to act.
    std::string const m = messageFor(rep, DiagnosticCode::D_DependencyBuildFailed);
    EXPECT_NE(m.find(manifestPathIn(dep)), std::string::npos)
        << "must name the DEPENDENCY's manifest; got: " << m;
    EXPECT_NE(m.find(std::string{kElfX64Exec}), std::string::npos)
        << "must name the CONSUMER's target spec; got: " << m;
    EXPECT_NE(m.find(std::string{kElfX64StaticLib}), std::string::npos)
        << "must name the DERIVED dependency spec; got: " << m;

    // (3) THE ATTRIBUTION SURVIVES. The prefix is compared in FULL — a check for
    // the substring "dependency=" would stay green over a prefix that lost the
    // target half, and the target half is what distinguishes one consumer
    // target's failure from another's in a multi-target build.
    std::string const expectedPrefix =
        "[dependency=leafbad target=x86_64:" + std::string{kElfX64StaticLib} + "] ";
    std::size_t innerPrefixed = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::D_DependencyBuildFailed) continue;
        if (d.contextPrefix == expectedPrefix) ++innerPrefixed;
    }
    EXPECT_GT(innerPrefixed, 0u)
        << "the dependency's own diagnostics must reach the caller's reporter "
           "carrying `" << expectedPrefix
        << "` — without them the summary above points at nothing";
    // And the summary line itself is NOT prefixed: it is the resolver speaking
    // about the dependency, not the dependency speaking.
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::D_DependencyBuildFailed) continue;
        EXPECT_TRUE(d.contextPrefix.empty())
            << "the resolver's own summary must not wear the dependency's "
               "prefix; got: " << d.contextPrefix;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// GRAPH STRUCTURE — cycles, diamonds, depth, and the two not-found codes
// ════════════════════════════════════════════════════════════════════════════

// A ring must fail loud with the PATH as payload. RED-ON-DISABLE is taken by
// mutating the DIAGNOSTIC EMISSION with the short-circuit intact — never by
// letting the walk recurse into a stack overflow, because a crash is not a red
// and cannot discriminate "detection removed" from "graph too deep".
TEST(DependencyResolverGraph, CycleFailsLoudWithThePathAsPayload) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const a   = dir / "alpha";
    fs::path const b   = dir / "beta";

    writeText(a / "a.c", kModuleSource);
    writeText(b / "b.c", "int beta_answer(void){ return 9; }\n");
    writeText(fs::path{manifestPathIn(a)},
              renderManifest({.profile   = "module",
                              .targets   = {std::string{kElfX64Exec}},
                              .sources   = {"a.c"},
                              .dependsOn = {pathEntry(b.generic_string())}}));
    writeText(fs::path{manifestPathIn(b)},
              renderManifest({.profile   = "module",
                              .targets   = {std::string{kElfX64Exec}},
                              .sources   = {"b.c"},
                              .dependsOn = {pathEntry(a.generic_string())}}));

    writeText(dir / "main.c", kMainUsingModule);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(a.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyCycle), 1u);
    std::string const m = messageFor(rep, DiagnosticCode::D_DependencyCycle);
    EXPECT_NE(m.find(manifestPathIn(a)), std::string::npos)
        << "the cycle PATH is the payload — a bare 'cycle detected' on a deep "
           "graph is nearly unactionable; got: "
        << m;
    EXPECT_NE(m.find(manifestPathIn(b)), std::string::npos) << m;
}

// A DIAMOND is a legitimate shared dependency and must NOT diagnose — the memo
// table answers it. The pin is three-sided: no cycle code, a successful build,
// and the shared module's source contributed EXACTLY ONCE (a second copy would
// be a duplicate CU and a duplicate-symbol link error).
TEST(DependencyResolverGraph, DiamondResolvesSilentlyAndContributesOnce) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir    = scratch.path();
    fs::path const shared = dir / "sharedmod";
    fs::path const left   = dir / "leftmod";
    fs::path const right  = dir / "rightmod";

    writeText(shared / "s.c", kModuleSource);
    writeText(fs::path{manifestPathIn(shared)},
              renderManifest({.profile = "module",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"s.c"}}));
    for (auto const& [d, stem] :
         std::vector<std::pair<fs::path, std::string>>{{left, "l"}, {right, "r"}}) {
        writeText(d / (stem + ".c"),
                  "int " + stem + "_answer(void){ return 1; }\n");
        writeText(fs::path{manifestPathIn(d)},
                  renderManifest({.profile   = "module",
                                  .targets   = {std::string{kElfX64Exec}},
                                  .sources   = {stem + ".c"},
                                  .dependsOn = {pathEntry(shared.generic_string())}}));
    }

    writeText(dir / "main.c", kMainUsingModule);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj,
              renderManifest({.targets   = {std::string{kElfX64Exec}},
                              .sources   = {(dir / "main.c").generic_string()},
                              .dependsOn = {pathEntry(left.generic_string()),
                                            pathEntry(right.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "a revisited-but-not-on-stack manifest is a DIAMOND, not a cycle";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyCycle), 0u);
    // The shared module's symbol appearing twice would have failed the link, so
    // a zero exit is itself the de-dup proof — but assert the artifact too,
    // because "no diagnostic" is also what a resolver that dropped every
    // dependency would produce.
    EXPECT_TRUE(fs::exists(dir / "out" / formatOf(kElfX64Exec) / "main"));
}

// The DEPTH CAP (plan v2 §3 item 9). Its own code, because a deep ACYCLIC graph
// is not a cycle and telling its author to "break the cycle" sends them looking
// for something that is not there.
TEST(DependencyResolverGraph, DeepAcyclicGraphIsRefusedWithItsOwnDiagnostic) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();

    // One past the cap, so the pin fails if the limit is quietly widened AND if
    // it is quietly narrowed (the chain below the cap builds in the sibling
    // pins).
    std::size_t const depth = dss::kMaxDependencyDepth + 2;
    for (std::size_t i = 0; i < depth; ++i) {
        fs::path const node = dir / ("m" + std::to_string(i));
        writeText(node / "n.c",
                  "int n" + std::to_string(i) + "_answer(void){ return 1; }\n");
        std::vector<std::string> deps;
        if (i + 1 < depth) {
            deps.push_back(
                pathEntry((dir / ("m" + std::to_string(i + 1))).generic_string()));
        }
        writeText(fs::path{manifestPathIn(node)},
                  renderManifest({.profile   = "module",
                                  .targets   = {std::string{kElfX64Exec}},
                                  .sources   = {"n.c"},
                                  .dependsOn = deps}));
    }
    writeText(dir / "main.c", kMainUsingModule);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj,
              renderManifest({.targets   = {std::string{kElfX64Exec}},
                              .sources   = {(dir / "main.c").generic_string()},
                              .dependsOn = {pathEntry((dir / "m0").generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGraphTooDeep), 1u)
        << "a deep acyclic graph must be REFUSED with a diagnostic, not walked "
           "until the process stack runs out — that failure class is invisible "
           "on the Release legs and arrives with no diagnostic at all";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyCycle), 0u)
        << "this graph is ACYCLIC; reporting a cycle would send the author "
           "looking for a ring that is not there";
}

// ── M6: 0xD019 vs `D_FileNotFound`, and the distinction is the whole point ──
//
// 0xD019 is remediation-distinct from `D_FileNotFound` BECAUSE the thing you
// named IS there and is simply not a DSS project — overwhelmingly a
// wrong-LEVEL path. Both polarities are pinned, each asserting the OTHER code
// is absent, because a resolver that emitted one code for both cases would pass
// either pin alone.
TEST(DependencyResolverPathEntry, MissingDirectoryIsFileNotFoundNotManifestNotFound) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj,
              renderManifest({.targets   = {std::string{kElfX64Exec}},
                              .sources   = {(dir / "main.c").generic_string()},
                              .dependsOn = {pathEntry(
                                  (dir / "no-such-directory").generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 1u)
        << "a path naming nothing at all is 'the thing you named is not there'";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyManifestNotFound), 0u)
        << "0xD019 must stay reserved for 'it IS there but is not a DSS "
           "project' — collapsing the two sends every typo'd path looking for a "
           "manifest it never had";
}

TEST(DependencyResolverPathEntry, DirectoryWithoutAManifestIsD019NamingBoth) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "notaproject";
    // The directory EXISTS and holds real files — it just is not a DSS project.
    writeText(dep / "src" / "lib.c", kLeafSource);

    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyManifestNotFound), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_FileNotFound), 0u)
        << "the directory is there; reporting it missing is the wrong claim";
    std::string const m =
        messageFor(rep, DiagnosticCode::D_DependencyManifestNotFound);
    EXPECT_NE(m.find(std::string{dss::kDependencyManifestName}), std::string::npos)
        << "must name the MANIFEST it looked for; got: " << m;
    EXPECT_NE(m.find(dep.generic_string()), std::string::npos)
        << "must name the ABSOLUTE path it looked at; got: " << m;
}

// ── 0xD01C: a source-merge dependency must share the consumer's language ────
TEST(DependencyResolverComposition, SourceMergeAcrossLanguagesRejectsAtResolveTime) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "toymod";

    writeText(dep / "m.toy", "let x = 1\n");
    // `toy` declares only `["cli"]`, so this manifest ALSO exercises the
    // dependency-side AP2 language gate — see the dedicated pin below. Here the
    // profile is one `toy` does declare, so the language mismatch is what fires.
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.language = "c-subset",
                              .profile  = "module",
                              .targets  = {std::string{kElfX64Exec}},
                              .sources  = {"m.toy"}}));
    // Re-write the dependency manifest with a DIFFERENT language than the
    // consumer's, keeping a profile that language declares.
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.language = "toy",
                              .profile  = "cli",
                              .targets  = {std::string{kElfX64Exec}},
                              .sources  = {"m.toy"}}));

    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    // `cli` is terminal, so THIS manifest is caught by the consumability gate;
    // the language check needs a SOURCE-MERGE profile, which `toy` does not
    // declare — which is itself the finding the AP2 dependency-side gate exists
    // for. Both rejects are loud and neither is a parse-error cascade.
    EXPECT_EQ(countCode(rep, DiagnosticCode::P_UnexpectedToken), 0u)
        << "the whole point of rejecting at RESOLVE time is that the user never "
           "sees a parse-error pile in a file they did not write";
}

// The dependency-side AP2 LANGUAGE gate (plan v2 §3 item 1). ✔`toy` declares
// `["cli"]` only, so a `toy` manifest saying `"module"` passes the
// LANGUAGE-BLIND composition lookup and would be source-merged with no gate
// anywhere. The root gets this gate in `compileProject`; a dependency is never
// routed through that function, so it has to get it in the resolver.
TEST(DependencyResolverComposition, DependencyGetsItsOwnAp2LanguageGate) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "toymod";

    writeText(dep / "m.toy", "let x = 1\n");
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.language = "toy",
                              .profile  = "module",   // toy declares ["cli"]
                              .targets  = {std::string{kElfX64Exec}},
                              .sources  = {"m.toy"}}));
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_ArtifactProfileNotSupported), 1u)
        << "a dependency's own `artifactProfile` must be in ITS language's "
           "declared set — otherwise a `toy` manifest saying \"module\" is "
           "source-merged with no gate anywhere";
    EXPECT_FALSE(fs::exists(dir / "out" / formatOf(kElfX64Exec) / "main"));
}

// An UNREGISTERED profile name is a TYPO, not a terminal profile. Reporting
// "this profile cannot be a dependency" about a name that does not exist is a
// confidently wrong answer, so the two must not share a code.
TEST(DependencyResolverComposition, UnregisteredProfileReportsTheUnknownName) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "typomod";

    writeText(dep / "m.c", kModuleSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "modul",   // one character short
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"m.c"}}));
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    EXPECT_EQ(
        countCode(rep, DiagnosticCode::D_DependencyArtifactProfileUnsupported), 0u)
        << "an unregistered NAME must not be reported as a terminal profile";
}

// ════════════════════════════════════════════════════════════════════════════
// COMPOSITION EFFECTS — merged-source order, U-7 hooks, U-8 absorption, U-9
// ════════════════════════════════════════════════════════════════════════════

// ── M4(b): THE ROOT'S OWN SOURCES COME FIRST, PINNED THROUGH THE ARTIFACT NAME
//
// ✔MEASURED: `sourceFiles.front()`'s stem NAMES the artifact when the manifest
// states no `artifactName`. So the consequence of getting the order wrong is
// not a diagnostic — it is a SILENTLY RENAMED BINARY. The manifest here states
// no `artifactName` on purpose; the file name IS the assertion.
TEST(DependencyResolverSources, RootSourcesLeadSoTheArtifactKeepsItsName) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "zzzmodule";

    // The dependency's stem sorts AFTER the root's and is spelled differently,
    // so a wrong order is unambiguous rather than a coin flip.
    writeText(dep / "zzz_helper.c", kModuleSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "module",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"zzz_helper.c"}}));
    writeText(dir / "appmain.c", kMainUsingModule);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj,
              renderManifest({.targets   = {std::string{kElfX64Exec}},
                              .sources   = {(dir / "appmain.c").generic_string()},
                              .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);
    fs::path const outDir = dir / "out" / formatOf(kElfX64Exec);
    EXPECT_TRUE(fs::exists(outDir / "appmain"))
        << "the artifact is named from the FIRST source's stem, so the ROOT's "
           "own sources must lead the merged list — otherwise adding a `module` "
           "dependency silently renames the output binary";
    EXPECT_FALSE(fs::exists(outDir / "zzz_helper"))
        << "the dependency's stem must NOT have named the artifact";
}

// ── U-7: a dependency's pre-build hooks run in THAT dependency's directory ──
//
// The marker is asserted PRESENT in the dependency's own directory and ABSENT
// from the two others. "Present in the right one" alone is satisfied by a hook
// that wrote to all three, which is exactly the mistake a cwd-inheriting spawn
// would make.
TEST(DependencyResolverHooks, DependencyPreBuildHookRunsInItsOwnDirectory) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir      = scratch.path();
    fs::path const dep      = dir / "genmodule";
    fs::path const bystander = dir / "bystander";
    std::error_code ec;
    fs::create_directories(bystander, ec);

    // The hook GENERATES the dependency's only source, at a RELATIVE path — so
    // the manifest's `sources` and the hook's write agree only if the hook ran
    // in the dependency's directory AND before the expansion.
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile         = "module",
                              .targets         = {std::string{kElfX64Exec}},
                              .sources         = {"generated/mod.c"},
                              .preBuildScripts = hookEntryJson(
                                  "generated/mod.c",
                                  std::string{kModuleSource})}));
    writeText(dir / "main.c", kMainUsingModule);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "the dependency's pre-build hook must run BEFORE its sources[] are "
           "expanded — expansion is fail-loud on zero matches";
    EXPECT_TRUE(fs::exists(dep / "generated" / "mod.c"))
        << "the hook must run in the DEPENDENCY's own directory";
    EXPECT_FALSE(fs::exists(dir / "generated" / "mod.c"))
        << "not in the consumer's directory";
    EXPECT_FALSE(fs::exists(bystander / "generated" / "mod.c"))
        << "and not in a bystander directory either — 'present in the right "
           "place' alone is satisfied by a hook that wrote everywhere";
    EXPECT_EQ(readText(dep / "generated" / "mod.c"), std::string{kModuleSource})
        << "right path, WRONG BYTES is what a weaker existence check calls "
           "success";
}

// ── U-7's other half: post-build hooks run only for a BUILT ArtifactLink dep ─
TEST(DependencyResolverHooks, PostBuildHookRunsForABuiltArtifactLinkDependency) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "leafutil";

    writeText(dep / "lib.c", kLeafSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile          = "staticlib",
                              .targets          = {std::string{kElfX64Exec}},
                              .sources          = {"lib.c"},
                              .postBuildScripts = hookEntryJson("post-ran.txt",
                                                                "ran")}));
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);
    EXPECT_TRUE(fs::exists(dep / "post-ran.txt"))
        << "an ArtifactLink dependency whose build returned 0 runs its "
           "post-build hooks, in its own directory";
    EXPECT_FALSE(fs::exists(dir / "post-ran.txt"));
}

// A SOURCE-MERGE dependency builds nothing, so it has no post-build hooks to
// run — mirroring the root's "only when the compile returned 0" rule. The
// marker's ABSENCE is the whole test, and the hook exits 0 so it would change
// no rc and add no diagnostic if it ran.
TEST(DependencyResolverHooks, SourceMergeDependencyRunsNoPostBuildHook) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "puremodule";

    writeText(dep / "m.c", kModuleSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile          = "module",
                              .targets          = {std::string{kElfX64Exec}},
                              .sources          = {"m.c"},
                              .postBuildScripts = hookEntryJson("post-ran.txt",
                                                                "ran")}));
    writeText(dir / "main.c", kMainUsingModule);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);
    EXPECT_FALSE(fs::exists(dep / "post-ran.txt"))
        << "a source-merge dependency produces no build product, so it has no "
           "post-build to run — the marker's absence is the only observable";
}

// ── U-8 (as corrected): a staticlib that depends on a SHARED library ────────
//
// ✔MEASURED: an archive build folds in static archives ONLY; a dynamic library
// handed to it goes to the per-CU FFI path and is "resolved at the FINAL link"
// — a link an `ar` archive never performs, and an archive records no import.
// So the shared library must keep travelling PAST the archive to the root, or
// the root never learns of it and the reference is undefined two hops from its
// cause. Absorption is the format's declared CONTAINER, never a profile name.
TEST(DependencyResolverAbsorption, SharedLibraryPropagatesPastAStaticArchive) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir  = scratch.path();
    fs::path const arch = dir / "midarchive";
    fs::path const shlib = dir / "deepshared";

    writeText(shlib / "s.c", "int shared_answer(void){ return 3; }\n");
    writeText(fs::path{manifestPathIn(shlib)},
              renderManifest({.profile = "lib",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"s.c"}}));
    // ⓘ THE ARCHIVE MEMBER DELIBERATELY MAKES NO EXTERN CALL, and the reason is
    // a defect OUTSIDE this subject rather than a weakening of it. ✔MEASURED on
    // the plain CLI path with no `dependsOn` anywhere — build a `staticlib`
    // whose member calls an extern, then `--resolve-library` it into an
    // `…-linux-exec` link — the read fails with
    // `F_CorruptedBinary: relocation type 4 in '.rela.text' is not declared by
    // ELF format 'elf64-x86_64-linux-exec'`: the staticlib format emits
    // `pltNativeId` 4 (R_X86_64_PLT32) for a call to an undefined extern, and
    // the exec format declares no row that maps type 4 back. Reported for its
    // own anchor. What U-8 is about is which artifacts REACH which build, and
    // that is exercised in full below — by the ROOT calling the transitive
    // shared library's symbol, so a propagation failure is an undefined symbol
    // rather than a map that merely looks wrong.
    writeText(arch / "a.c", "int dep_answer(void){ return 7; }\n");
    writeText(fs::path{manifestPathIn(arch)},
              renderManifest({.profile   = "staticlib",
                              .targets   = {std::string{kElfX64Exec}},
                              .sources   = {"a.c"},
                              .dependsOn = {pathEntry(shlib.generic_string())}}));
    // The ROOT calls BOTH: `dep_answer` from the direct archive dependency and
    // `shared_answer` from the TRANSITIVE shared library two hops away. If the
    // shared library stopped at the archive, this link fails with
    // `K_SymbolUndefined` — which is exactly the failure U-8 exists to prevent,
    // reproduced as a test rather than described.
    writeText(dir / "main.c",
              "extern int dep_answer(void);\n"
              "extern int shared_answer(void);\n"
              "int main(void){ return dep_answer() + shared_answer(); }\n");
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(arch.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << "the root calls a symbol two hops away; a shared library that "
           "stopped at the intervening archive would leave it undefined\n"
        << messageFor(rep, DiagnosticCode::K_SymbolUndefined);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u);

    auto const& byTarget = prog.resolveLibraryAdditionsByTarget();
    auto const  it       = byTarget.find(std::string{kElfX64Exec});
    ASSERT_NE(it, byTarget.end());
    bool sawArchive = false;
    bool sawShared  = false;
    for (auto const& spec : it->second) {
        std::string const name = spec.path.filename().string();
        if (name == "a.a") sawArchive = true;
        if (name == "s.so") sawShared = true;
    }
    EXPECT_TRUE(sawArchive) << "the direct dependency's archive must reach the "
                               "consumer's link";
    EXPECT_TRUE(sawShared)
        << "the TRANSITIVE shared library must ALSO reach the root: an `ar` "
           "archive absorbs archives and cannot absorb a shared library, so "
           "stopping it at the archive leaves an undefined reference two hops "
           "from its cause";
}

// A static archive DOES absorb another static archive, so that one stops there
// rather than also arriving at the root — the other half of the same rule, and
// the half that would be missed by a resolver that simply forwarded everything.
TEST(DependencyResolverAbsorption, StaticArchiveAbsorbsAStaticArchive) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir   = scratch.path();
    fs::path const outer = dir / "outerarchive";
    fs::path const inner = dir / "innerarchive";

    writeText(inner / "i.c", "int inner_answer(void){ return 2; }\n");
    writeText(fs::path{manifestPathIn(inner)},
              renderManifest({.profile = "staticlib",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"i.c"}}));
    // No extern call from the archive member — see the ✔MEASURED note in the
    // sibling pin above (the exec format cannot read the PLT32 the staticlib
    // format emits for one; a pre-existing, AP6-independent defect). The ROOT
    // calls BOTH symbols instead, so the fat-archive merge is what has to have
    // carried `inner_answer` into `o.a`: if the inner archive were dropped
    // rather than absorbed, this link fails with `K_SymbolUndefined`.
    writeText(outer / "o.c", "int dep_answer(void){ return 7; }\n");
    writeText(fs::path{manifestPathIn(outer)},
              renderManifest({.profile   = "staticlib",
                              .targets   = {std::string{kElfX64Exec}},
                              .sources   = {"o.c"},
                              .dependsOn = {pathEntry(inner.generic_string())}}));
    writeText(dir / "main.c",
              "extern int dep_answer(void);\n"
              "extern int inner_answer(void);\n"
              "int main(void){ return dep_answer() + inner_answer(); }\n");
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(outer.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << messageFor(rep, DiagnosticCode::K_SymbolUndefined);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "`inner_answer` must have been carried INTO the outer archive by the "
           "fat-archive merge — that is what absorption means here";

    auto const& byTarget = prog.resolveLibraryAdditionsByTarget();
    auto const  it       = byTarget.find(std::string{kElfX64Exec});
    ASSERT_NE(it, byTarget.end());
    ASSERT_EQ(it->second.size(), 1u)
        << "the inner archive is ABSORBED by the outer one (the fat-archive "
           "merge carries every member), so exactly one artifact reaches the "
           "root — forwarding it as well would be the other half of U-8 got "
           "wrong";
    EXPECT_EQ(it->second.front().path.filename().string(), "o.a");
}

// ── U-9 placement, and the per-target channel not leaking across targets ────
//
// TWO targets with DIFFERENT `formatName`s, or the `<formatName>` component of
// the layout is unproven. The additions must reach the RIGHT target and NOT the
// other one: broadcasting the union would hand an ELF archive to a Mach-O link,
// which binds silently.
TEST(DependencyResolverPerTarget, ArtifactsAreFiledAndThreadedPerTarget) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "leafutil";

    writeText(dep / "lib.c", kLeafSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "staticlib",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"lib.c"}}));
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets = {std::string{kElfX64Exec},
                                                std::string{kMachoArm64Exec}},
                                    .sources = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    Program prog;
    fs::path const outBase = dir / "out";
    prog.setOutputDir(outBase);
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << messageFor(rep, DiagnosticCode::D_DependencyTargetFormatUnresolvable);

    // U-9: one directory per (dependency, format), under the consumer's base.
    EXPECT_TRUE(fs::exists(depArtifactDir(outBase, "leafutil", kElfX64StaticLib)
                           / "lib.a"));
    EXPECT_TRUE(fs::exists(
        depArtifactDir(outBase, "leafutil", kMachoArm64StaticLib) / "lib.a"));
    EXPECT_FALSE(fs::exists(dep / "target"))
        << "nothing may be written into the DEPENDENCY's own tree, which may be "
           "read-only";

    auto const& byTarget = prog.resolveLibraryAdditionsByTarget();
    ASSERT_EQ(byTarget.size(), 2u);
    for (auto const& [spec, libs] : byTarget) {
        ASSERT_EQ(libs.size(), 1u) << spec;
        std::string const wanted =
            spec == std::string{kElfX64Exec}
                ? std::string{kElfX64StaticLib}
                : std::string{kMachoArm64StaticLib};
        EXPECT_EQ(libs.front().path.parent_path().filename().string(), wanted)
            << "target '" << spec
            << "' was handed another target's artifact — broadcasting the union "
               "feeds an ELF archive to a Mach-O link, which binds SILENTLY";
    }
}

// ── 0xD025: two dependencies deriving one output name ───────────────────────
//
// U-9 calls `deps/<name>` collision-free by construction, and for a GIT
// dependency it is (U-5 derives from the URL, 0xD020 rejects a clash before
// anything is fetched). For a `path` dependency nothing derived a name and
// nothing detected a clash — `a/util` and `b/util` both want `deps/util`, and
// the second build silently overwrites the first.
TEST(DependencyResolverOutputName, TwoDependenciesDerivingOneNameFailLoud) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const a   = dir / "a" / "util";
    fs::path const b   = dir / "b" / "util";

    for (auto const& [d, fn] : std::vector<std::pair<fs::path, std::string>>{
             {a, "a_answer"}, {b, "b_answer"}}) {
        writeText(d / "u.c", "int " + fn + "(void){ return 1; }\n");
        writeText(fs::path{manifestPathIn(d)},
                  renderManifest({.profile = "staticlib",
                                  .targets = {std::string{kElfX64Exec}},
                                  .sources = {"u.c"}}));
    }
    writeText(dir / "main.c", kMainSource);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets = {std::string{kElfX64Exec}},
                                    .sources = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(a.generic_string()),
                                                  pathEntry(b.generic_string())}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(proj.string(), rep), 0);
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyOutputNameCollision), 1u)
        << "silently overwriting one dependency's artifact with another's is "
           "the substitution 0xD020 exists to forbid, with the detection removed";
    std::string const m =
        messageFor(rep, DiagnosticCode::D_DependencyOutputNameCollision);
    EXPECT_NE(m.find(a.generic_string()), std::string::npos)
        << "must name the FIRST dependency; got: " << m;
    EXPECT_NE(m.find(b.generic_string()), std::string::npos)
        << "must name the SECOND dependency; got: " << m;
}

// ════════════════════════════════════════════════════════════════════════════
// THE GIT SEAM REACHES THE RESOLVER
// ════════════════════════════════════════════════════════════════════════════

// The DRIVER wiring, not the cache state machine (`test_dependency_git_cache.cpp`
// owns that): an injected runner reaches the resolver, a `git` entry composes
// like a `path` entry, and the ONE `.dss-deps` lands at the ROOT consumer's
// manifest directory rather than the process cwd or the dependency's tree.
TEST(DependencyResolverGit, GitDependencyResolvesThroughTheInjectedRunner) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const projDir = dir / "consumer";

    FakeGitRunner git;
    git.tree[std::string{dss::kDependencyManifestName}] =
        renderManifest({.profile = "module",
                        .targets = {std::string{kElfX64Exec}},
                        .sources = {"m.c"}});
    git.tree["m.c"] = std::string{kModuleSource};

    writeText(projDir / "main.c", kMainUsingModule);
    fs::path const proj = projDir / "app.dss-project.json";
    writeText(proj,
              renderManifest({.targets   = {std::string{kElfX64Exec}},
                              .sources   = {(projDir / "main.c").generic_string()},
                              .dependsOn = {
                                  "{\"git\": \"https://example.invalid/org/gitmod.git\"}"}}));

    Program prog;
    prog.setOutputDir(dir / "out");
    prog.setGitRunner(&git);
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0)
        << messageFor(rep, DiagnosticCode::D_DependencyGitAcquireFailed);
    EXPECT_EQ(git.cloneCalls, 1) << "the injected runner must be the one used";

    // M2 / B.4: ONE `.dss-deps`, at the ROOT consumer's MANIFEST directory.
    EXPECT_TRUE(fs::exists(projDir / ".dss-deps" / "gitmod"
                           / std::string{dss::kDependencyManifestName}))
        << "the checkout must land beside the consuming project's manifest";
    EXPECT_TRUE(fs::exists(projDir / ".dss-deps" / "dss-lock.json"))
        << "the lockfile is the cache's state of record and is written once "
           "after the walk";
    EXPECT_FALSE(fs::exists(dir / ".dss-deps"))
        << "never at the scratch root, and never at the process cwd";
    EXPECT_TRUE(fs::exists(dir / "out" / formatOf(kElfX64Exec) / "main"));
}

// ── `--force-git-cache` REACHES THE CACHE, END TO END THROUGH THE DRIVER ────
//
// ★ THE PIN IS THE NEW COMMIT AND THE REWRITTEN LOCKFILE, NOT A CALL COUNT. A
// fetch alone never moves `HEAD` — it updates remote-tracking refs — so a flag
// implemented as "fetch and stop" is a network round trip that changes nothing,
// and a test asserting `fetchCallCount == 1` goes GREEN over an entirely absent
// mechanism. What this asserts is the OBSERVABLE the flag exists to produce:
// the checkout is at a DIFFERENT commit afterwards and the lockfile says so.
//
// Three builds, because the no-op direction has to be pinned too: a first build
// that clones, a forced build that must move, and a plain build that must be a
// HIT with no network at all.
TEST(DependencyResolverGit, ForceGitCacheMovesHeadAndRewritesTheLockfile) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir     = scratch.path();
    fs::path const projDir = dir / "consumer";

    FakeGitRunner git;
    git.tree[std::string{dss::kDependencyManifestName}] =
        renderManifest({.profile = "module",
                        .targets = {std::string{kElfX64Exec}},
                        .sources = {"m.c"}});
    git.tree["m.c"] = std::string{kModuleSource};
    git.clonedCommit = "commit-before";

    writeText(projDir / "main.c", kMainUsingModule);
    fs::path const proj = projDir / "app.dss-project.json";
    writeText(proj,
              renderManifest({.targets   = {std::string{kElfX64Exec}},
                              .sources   = {(projDir / "main.c").generic_string()},
                              .dependsOn = {
                                  "{\"git\": \"https://example.invalid/org/movingmod.git\"}"}}));
    fs::path const lock = projDir / ".dss-deps" / "dss-lock.json";

    // (1) first build — a MISS: clone, then record.
    {
        Program prog;
        prog.setOutputDir(dir / "out1");
        prog.setGitRunner(&git);
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);
    }
    ASSERT_EQ(git.cloneCalls, 1);
    ASSERT_NE(readText(lock).find("commit-before"), std::string::npos)
        << "the first build must record what it checked out";

    // (2) the remote moves, and the FLAG is what makes the build see it.
    git.clonedCommit = "commit-after";
    int const fetchBefore = git.fetchCalls;
    {
        Program prog;
        prog.setOutputDir(dir / "out2");
        prog.setGitRunner(&git);
        prog.setForceGitCache(true);
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);
    }
    EXPECT_EQ(git.cloneCalls, 1) << "an existing checkout is refreshed, not re-cloned";
    EXPECT_GT(git.fetchCalls, fetchBefore) << "the flag must reach the cache";
    EXPECT_GT(git.checkoutCalls, 0)
        << "a fetch alone leaves HEAD where it was, so the flag MUST check out "
           "afterwards — otherwise it is a network round trip that changes "
           "nothing and its call-count test passes over nothing";
    std::string const after = readText(lock);
    EXPECT_NE(after.find("commit-after"), std::string::npos)
        << "the lockfile must be REWRITTEN with the commit the refresh landed "
           "on; got: " << after;
    EXPECT_EQ(after.find("commit-before"), std::string::npos)
        << "the stale commit must be gone, not merely joined; got: " << after;

    // (3) without the flag: a HIT, and B.4's guarantee is "NO NETWORK ACCESS AT
    // ALL — not a conditional request, not an `ls-remote`, nothing".
    int const cloneAt = git.cloneCalls;
    int const fetchAt = git.fetchCalls;
    {
        Program prog;
        prog.setOutputDir(dir / "out3");
        prog.setGitRunner(&git);
        DiagnosticReporter rep;
        ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);
    }
    EXPECT_EQ(git.cloneCalls, cloneAt);
    EXPECT_EQ(git.fetchCalls, fetchAt)
        << "a cache hit must perform no network access at all — a stray fetch "
           "still builds, it is just no longer offline, and nobody finds out "
           "until a train";
}

// U-10 + the flag's wiring: `--force-git-cache` reaches the resolver and is a
// SILENT no-op when the project declares no git dependency. Three-sided: the
// build succeeds, nothing is reported, and NO `.dss-deps` is materialized (the
// cache is opened lazily, which is what makes the no-op a property rather than
// a remembered check).
TEST(DependencyResolverGit, ForceGitCacheIsASilentNoOpWithoutAGitDependency) {
    ScratchDir scratch{Location::Temp, "dep-resolver"};
    fs::path const dir = scratch.path();
    fs::path const dep = dir / "puremodule";

    writeText(dep / "m.c", kModuleSource);
    writeText(fs::path{manifestPathIn(dep)},
              renderManifest({.profile = "module",
                              .targets = {std::string{kElfX64Exec}},
                              .sources = {"m.c"}}));
    writeText(dir / "main.c", kMainUsingModule);
    fs::path const proj = dir / "app.dss-project.json";
    writeText(proj, renderManifest({.targets   = {std::string{kElfX64Exec}},
                                    .sources   = {(dir / "main.c").generic_string()},
                                    .dependsOn = {pathEntry(dep.generic_string())}}));

    FakeGitRunner git;
    Program       prog;
    prog.setOutputDir(dir / "out");
    prog.setGitRunner(&git);
    prog.setForceGitCache(true);
    DiagnosticReporter rep;
    ASSERT_EQ(prog.compileProject(proj.string(), rep), 0);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a project with no git dependency must not be told anything about a "
           "flag that had nothing to do";
    EXPECT_EQ(git.cloneCalls, 0);
    EXPECT_EQ(git.fetchCalls, 0);
    EXPECT_FALSE(fs::exists(dir / ".dss-deps"))
        << "no git entry ⇒ the cache is never opened, so no directory is "
           "materialized";
}

// ════════════════════════════════════════════════════════════════════════════
// AGNOSTICISM — the §A.1 claim, enforced rather than asserted in prose
// ════════════════════════════════════════════════════════════════════════════

// Plan 06 AP5 CLAIMS "zero profile-value string comparisons in `src/`" and
// nothing enforced it. The composition fork is the exact place that claim gets
// broken — `if (profile == "module") … else if (profile == "staticlib") …` is
// the naive spelling, and it goes SILENTLY wrong for the eleventh profile (it
// lands in the `else`, and whether that is correct is an accident of which arm
// the author wrote last). The engine switches on the composition VERB; this pin
// is what keeps it that way.
//
// The scanner strips comments and string-internal `//` before looking, because
// every docblock in this subsystem NAMES the profiles it is reasoning about —
// a naive `grep` would be a permanent false positive and would be deleted.
namespace {

// Source text with `//` and block comments removed, string literals preserved.
[[nodiscard]] std::string stripComments(std::string const& src) {
    std::string out;
    out.reserve(src.size());
    enum class State { Code, Line, Block, Str, Chr } st = State::Code;
    for (std::size_t i = 0; i < src.size(); ++i) {
        char const c    = src[i];
        char const next = (i + 1 < src.size()) ? src[i + 1] : '\0';
        switch (st) {
            case State::Code:
                if (c == '/' && next == '/') { st = State::Line; ++i; }
                else if (c == '/' && next == '*') { st = State::Block; ++i; }
                else {
                    if (c == '"')  st = State::Str;
                    if (c == '\'') st = State::Chr;
                    out += c;
                }
                break;
            case State::Line:
                if (c == '\n') { st = State::Code; out += c; }
                break;
            case State::Block:
                if (c == '*' && next == '/') { st = State::Code; ++i; }
                break;
            case State::Str:
                out += c;
                if (c == '\\') { if (i + 1 < src.size()) out += src[++i]; }
                else if (c == '"') st = State::Code;
                break;
            case State::Chr:
                out += c;
                if (c == '\\') { if (i + 1 < src.size()) out += src[++i]; }
                else if (c == '\'') st = State::Code;
                break;
        }
    }
    return out;
}

// ★ THE SUBJECT OF THE COMPARISON IS PART OF THE PATTERN, AND LEAVING IT OUT
// MAKES THE PIN UNUSABLE. ✔MEASURED: a scan for "any `==` against a profile
// name" reported THREE findings in `src/hir/hir_text.cpp`, all of them the HIR
// text format parsing its own attribute vocabulary (`kind == "shader"`,
// `kind == "transpile"`) — the words overlap, the meanings do not. A pin that
// fires on unrelated code gets an exclusion list, then gets deleted. So the
// shape it looks for is the shape of the ACTUAL violation: a comparison whose
// OTHER operand is a profile-valued expression, i.e. an identifier chain
// containing "rofile" (`profile`, `artifactProfile`, `depProfile`, `Profile`).
// That is exactly what `if (pc.artifactProfile == "module")` looks like, and it
// is what nothing else in `src/` looks like.
[[nodiscard]] bool isProfileValuedComparison(std::string const& code,
                                             std::size_t pos, std::size_t len) {
    auto const isIdentChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'
            || c == '.' || c == '>' || c == '-' || c == ':' || c == ')'
            || c == '(';
    };
    auto const skipWsBack = [&](std::size_t i) {
        while (i > 0 && std::isspace(static_cast<unsigned char>(code[i - 1]))) --i;
        return i;
    };
    auto const skipWsFwd = [&](std::size_t i) {
        while (i < code.size()
               && std::isspace(static_cast<unsigned char>(code[i]))) {
            ++i;
        }
        return i;
    };
    auto const mentionsProfile = [](std::string_view s) {
        return s.find("rofile") != std::string_view::npos;
    };

    // literal on the RIGHT: `<expr> == "name"`
    std::size_t const beforeOp = skipWsBack(pos);
    if (beforeOp >= 2
        && (code.compare(beforeOp - 2, 2, "==") == 0
            || code.compare(beforeOp - 2, 2, "!=") == 0)) {
        std::size_t end = skipWsBack(beforeOp - 2);
        std::size_t beg = end;
        while (beg > 0 && isIdentChar(code[beg - 1])) --beg;
        if (mentionsProfile(std::string_view{code}.substr(beg, end - beg))) {
            return true;
        }
    }
    // literal on the LEFT: `"name" == <expr>`
    std::size_t const afterLit = skipWsFwd(pos + len);
    if (afterLit + 1 < code.size()
        && (code.compare(afterLit, 2, "==") == 0
            || code.compare(afterLit, 2, "!=") == 0)) {
        std::size_t beg = skipWsFwd(afterLit + 2);
        std::size_t end = beg;
        while (end < code.size() && isIdentChar(code[end])) ++end;
        if (mentionsProfile(std::string_view{code}.substr(beg, end - beg))) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(DependencyResolverAgnosticism, NoProfileNameIsCompiledIntoAComparisonInSrc) {
    fs::path const root = dss::test::repoRoot();
    // The registered TABLE is the one place profile names are legitimately
    // written as literals — it IS the vocabulary. Every other site must reach
    // the answer through the table.
    constexpr std::string_view kVocabularyOwner = "artifact_profile.hpp";

    std::vector<std::string> findings;
    std::error_code          ec;
    for (fs::recursive_directory_iterator it{root / "src",
                                             fs::directory_options::none, ec},
         end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string const ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        if (it->path().filename() == kVocabularyOwner) continue;

        std::string const code = stripComments(readText(it->path()));
        for (auto const& row : kRegisteredArtifactProfiles) {
            std::string const literal = "\"" + std::string{row.name} + "\"";
            for (std::size_t pos = code.find(literal); pos != std::string::npos;
                 pos             = code.find(literal, pos + 1)) {
                if (!isProfileValuedComparison(code, pos, literal.size())) continue;
                findings.push_back(
                    it->path().lexically_relative(root).generic_string() + ": "
                    + literal);
            }
        }
    }
    EXPECT_TRUE(findings.empty())
        << "an artifact-profile NAME is compared against a literal in " << findings.size()
        << " place(s) in src/. The engine must switch on the composition VERB "
           "carried by the registered row — a name comparison goes silently "
           "wrong for the next profile, which lands in whichever `else` the "
           "author wrote last. First: "
        << (findings.empty() ? std::string{} : findings.front());
}

// The scanner itself must be able to FIND something, or the pin above is
// vacuous by construction: a stripper with an off-by-one that eats all code
// would report zero findings forever and read as coverage.
// THE SCANNER'S OWN GUARD. A pin that can only report zero is not coverage, it
// is the shape of coverage — and a stripper with an off-by-one that ate all
// code would report zero forever. All THREE polarities are exercised: the
// violation is found, prose is not, and the coincidental vocabulary overlap
// that ✔actually exists in `src/hir/hir_text.cpp` is not.
TEST(DependencyResolverAgnosticism, TheProfileNameScannerActuallyDetects) {
    // (a) the real violation shape.
    {
        std::string const code = stripComments(
            "int f(ProjectConfig const& pc){ "
            "return pc.artifactProfile == \"module\" ? 1 : 0; }\n");
        std::size_t const pos = code.find("\"module\"");
        ASSERT_NE(pos, std::string::npos)
            << "the comment stripper must preserve string literals in code";
        EXPECT_TRUE(isProfileValuedComparison(code, pos, 8))
            << "a profile-valued expression compared against a profile NAME is "
               "exactly what this pin exists to forbid";
    }
    // (b) prose. Every docblock in this subsystem names the profiles it reasons
    // about, so a pin that fired on comments would be deleted rather than
    // obeyed.
    {
        std::string const code = stripComments(
            "// the fork must never say profile == \"module\" here\n"
            "int g(void){ return 0; }\n");
        EXPECT_EQ(code.find("\"module\""), std::string::npos);
    }
    // (c) the coincidental overlap, ✔MEASURED in `src/hir/hir_text.cpp`: the
    // HIR text format parses its OWN attribute vocabulary, which happens to
    // share two words with the profile vocabulary. Flagging it would make this
    // pin permanently red on unrelated code.
    {
        std::string const code =
            stripComments("void h(std::string kind){ "
                          "if (kind == \"transpile\") {} }\n");
        std::size_t const pos = code.find("\"transpile\"");
        ASSERT_NE(pos, std::string::npos);
        EXPECT_FALSE(isProfileValuedComparison(code, pos, 11))
            << "an unrelated vocabulary that shares a word must NOT be "
               "reported — the subject of the comparison is part of the pattern";
    }
}

int main(int argc, char** argv) {
    if (argc >= 2
        && std::string_view{argv[1]}.substr(0, kHookWriteFlag.size())
               == kHookWriteFlag) {
        return runHookFixture(argc, argv);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
