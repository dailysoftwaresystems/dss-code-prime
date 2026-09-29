// THE ONE BASE RULE FOR A MANIFEST'S PATHS — field by field, from three working
// directories.
//
// [[D-PROJECT-ROOT-MANIFEST-PATHS-RESOLVE-AGAINST-THE-INVOCATION-DIRECTORY]]:
// every relative path a `.dss-project.json` holds resolves against THAT
// MANIFEST'S OWN DIRECTORY — the root manifest exactly as a dependency's, from
// wherever the build was started (`docs/project-config-spec.md` §2.2). ONE
// function states it (`resolveManifestPath`, `src/core/types/project_sources.hpp`)
// and every reader resolves through it.
//
// ── THE MATRIX ──────────────────────────────────────────────────────────────
//
// One TEST per path-bearing field, so a red NAMES the field. Each builds a fresh
// tree three times and compiles it from three working directories:
//
//   * the MANIFEST'S OWN DIRECTORY, naming the manifest by its bare filename (the
//     manifest's directory is then the EMPTY path, i.e. the working directory);
//   * an EMPTY directory, naming the manifest by a RELATIVE path
//     (`../proj/app.dss-project.json`), so every resolved path is itself relative
//     and is walked by the OS from a directory that holds nothing;
//   * a DECOY directory holding a LOOK-ALIKE of every relative path the manifest
//     names — each one producing a different exit value — naming the manifest by
//     an ABSOLUTE path.
//
// ✔MEASURED before the rule changed (lane `cr`, P68 round 8 part 6): from the
// decoy directory a root build compiled the DECOY's source, took the DECOY's
// header and ran the DECOY's hook, exit status 0 every time; from the empty one
// it failed naming only the relative spelling. So the DECOY column is the one
// that proves a build read the RIGHT file rather than merely a file: every arm
// RUNS its artifact and compares the exit value, and a decoy's value is always
// a different number from the author's.
//
// ★ ISOLATION. In every arm ONLY the field under test is spelled relative;
// everything else the manifest names is absolute. So disabling the resolution
// of one field reds that field's arm and not its neighbours, and disabling the
// base itself reds the empty and decoy columns of EVERY arm.
//
// RED-ON-DISABLE: `resolveManifestPath` returning `entry` unchanged must red
// EVERY matrix TEST below, each in its empty and decoy columns — except the
// bare-name hook arm, which names no path and is the missing-tool arms' control.
// The root arms' manifest-directory column is the control and stays green: a
// manifest named by its bare filename has an EMPTY directory, which the rule
// leaves to the working directory by definition. The measured transcript is
// recorded with the rows.
//
// Two arms are REFUSED builds rather than runs (`runRefusedArm`): a hook whose
// program is missing, named by a path and by a bare name. Their verdict is what
// the refusal SAYS — the one file probed, or the PATH search — from every column.
//
// Host-native targets, because the verdict is the RUN: an artifact that cannot
// run on this host could not tell the author's source from the decoy's.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/project_sources.hpp"
#include "program/program.hpp"

#include "diagnostic_count.hpp"
#include "host_native_target.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::hostNativeTarget;
using dss::test_support::Location;
using dss::test_support::runBinary;
using dss::test_support::ScratchDir;

namespace fs = std::filesystem;

namespace {

// ── the values every arm tells apart ────────────────────────────────────────
//
// The AUTHOR's files produce these; every DECOY file produces `kDecoy`, and a
// decoy archive's `lib_value` does too. All distinct, all small, all non-zero.
constexpr std::uint32_t kAuthor      = 7;   // a root source, header or library
constexpr std::uint32_t kDecoy       = 3;   // every look-alike
constexpr std::uint32_t kHook        = 21;  // what the author's hook program writes
constexpr std::uint32_t kDepInclude  = 11;  // a dependency's own header
constexpr std::uint32_t kDepLibrary  = 11;  // a dependency folding the author's library: 7 + 4
constexpr std::uint32_t kModule      = 5;   // a module's own source / header

constexpr std::string_view kManifestName = "app.dss-project.json";
constexpr std::string_view kDepManifest  = ".dss-project.json";

[[nodiscard]] std::string execSpec() { return std::string{hostNativeTarget().execTarget}; }

// The host's STATIC-archive twin of its executable format. Every shipped
// `<name>-exec` format has a `<name>-staticlib` sibling
// (`src/dss-config/object-formats/`); an archive is what lets a library's
// CODE — and therefore its answer — land in the executable, so the run can tell
// the author's library from a decoy's. (A shared library would be found by the
// LOADER at run time under one import name whichever file the build read.)
[[nodiscard]] std::string staticLibSpec() {
    std::string spec = execSpec();
    constexpr std::string_view kExec = "-exec";
    spec.replace(spec.size() - kExec.size(), kExec.size(), "-staticlib");
    return spec;
}

[[nodiscard]] std::string formatName() {
    std::string const spec = execSpec();
    return spec.substr(spec.find(':') + 1);
}

[[nodiscard]] std::string exeName(std::string_view stem) {
    return std::string{stem} + std::string{hostNativeTarget().exeSuffix};
}

void writeText(fs::path const& p, std::string_view text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
    out.flush();
    ASSERT_TRUE(out) << "could not write fixture file " << p.generic_string();
}

// JSON string quoting. Every path this file interpolates is spelled generically
// (forward slashes), but a `\` would otherwise be read as an escape introducer.
[[nodiscard]] std::string q(std::string_view s) {
    std::string out = "\"";
    for (char const c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

[[nodiscard]] std::string g(fs::path const& p) { return p.generic_string(); }

[[nodiscard]] std::string returning(std::uint32_t v) {
    return "int main(void){ return " + std::to_string(v) + "; }\n";
}

// A manifest. `extra` is raw JSON members, each already `"key": value`.
[[nodiscard]] std::string manifest(std::string_view profile,
                                   std::vector<std::string> const& sources,
                                   std::vector<std::string> const& extra = {}) {
    std::string out = "{\n  \"language\": \"c\",\n  \"artifactProfile\": "
                    + q(profile) + ",\n  \"targets\": [" + q(execSpec())
                    + "],\n  \"sources\": [";
    for (std::size_t i = 0; i < sources.size(); ++i) {
        out += (i == 0 ? "" : ", ") + q(sources[i]);
    }
    out += "]";
    for (auto const& e : extra) out += ",\n  " + e;
    out += "\n}\n";
    return out;
}

[[nodiscard]] std::string rootManifest(std::vector<std::string> const& sources,
                                       std::vector<std::string> extra = {}) {
    extra.push_back("\"artifactName\": \"app\"");
    return manifest("cli", sources, extra);
}

[[nodiscard]] std::string dependsOnPath(std::string_view path) {
    return "\"dependsOn\": [{\"path\": " + q(path) + "}]";
}

[[nodiscard]] std::string preBuildRun(std::string_view program) {
    return "\"preBuildScripts\": [{\"run\": [" + q(program) + "]}]";
}

[[nodiscard]] std::string renderDiagnostics(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += "\n  " + std::string{dss::diagnosticCodeName(d.code)} + ": "
             + d.contextPrefix + d.actual;
    }
    return out.empty() ? std::string{" <none>"} : out;
}

// ── programs the fixtures need, built ONCE per process by DSS itself ────────
//
// A hook program that writes `hook_out.c` INTO ITS OWN WORKING DIRECTORY (so a
// hook started in the wrong directory writes the file where the build will not
// look), a decoy twin that writes a different answer, and two static archives
// with ONE file name whose `lib_value` differs. Built with the compiler under
// test for the host, so no toolchain, interpreter or PATH entry is assumed.
struct Helpers {
    std::string error;           // non-empty ⇒ a helper did not build
    fs::path    authorWriter;
    fs::path    decoyWriter;
    fs::path    authorArchive;
    fs::path    decoyArchive;
};

[[nodiscard]] std::string writerSource(std::uint32_t value) {
    return "#include <stdio.h>\n"
           "int main(void) {\n"
           "    FILE *f = fopen(\"hook_out.c\", \"wb\");\n"
           "    if (f == 0) return 90;\n"
           "    fputs(\"int hook_value(void){ return " + std::to_string(value)
         + "; }\\n\", f);\n"
           "    return fclose(f) == 0 ? 0 : 91;\n"
           "}\n";
}

[[nodiscard]] std::optional<fs::path>
buildHelper(fs::path const& dir, std::string_view file, std::string_view text,
            std::string const& spec, std::string& error) {
    writeText(dir / std::string{file}, text);
    Program prog;
    prog.setOutputDir(dir / "out");
    DiagnosticReporter rep;
    int const rc = prog.compileFiles({g(dir / std::string{file})}, "c", {spec}, rep);
    auto const& paths = prog.artifactPaths();
    if (rc != 0 || paths.size() != 1 || !paths[0].has_value()) {
        error += "helper '" + std::string{file} + "' for '" + spec
               + "' did not build:" + renderDiagnostics(rep) + "\n";
        return std::nullopt;
    }
    return fs::absolute(*paths[0]);
}

Helpers const& helpers() {
    static ScratchDir scratch{Location::InsideRepo, "manifest-path-base-helpers"};
    static Helpers const built = [] {
        Helpers h;
        fs::path const root = scratch.path();
        if (auto p = buildHelper(root / "author-writer", "gen.c",
                                 writerSource(kHook), execSpec(), h.error)) {
            h.authorWriter = *p;
        }
        if (auto p = buildHelper(root / "decoy-writer", "gen.c",
                                 writerSource(kDecoy), execSpec(), h.error)) {
            h.decoyWriter = *p;
        }
        if (auto p = buildHelper(root / "author-lib", "ver.c",
                                 "int lib_value(void){ return "
                                     + std::to_string(kAuthor) + "; }\n",
                                 staticLibSpec(), h.error)) {
            h.authorArchive = *p;
        }
        if (auto p = buildHelper(root / "decoy-lib", "ver.c",
                                 "int lib_value(void){ return "
                                     + std::to_string(kDecoy) + "; }\n",
                                 staticLibSpec(), h.error)) {
            h.decoyArchive = *p;
        }
        return h;
    }();
    return built;
}

// Copy a built program or archive into a fixture, keeping it executable.
void place(fs::path const& from, fs::path const& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "could not place " << g(from) << " at " << g(to) << ": "
                     << ec.message();
    fs::permissions(to,
                    fs::perms::owner_exec | fs::perms::group_exec
                        | fs::perms::others_exec,
                    fs::perm_options::add, ec);
}

[[nodiscard]] std::string archiveName() {
    return helpers().authorArchive.filename().string();
}

// ── the matrix driver ───────────────────────────────────────────────────────

enum class Column { ManifestDir, Empty, Decoy };

[[nodiscard]] char const* columnName(Column c) {
    switch (c) {
        case Column::ManifestDir: return "from the manifest's own directory";
        case Column::Empty:       return "from an empty directory";
        case Column::Decoy:       return "from a directory holding look-alikes";
    }
    return "?";
}

// Restores the working directory on scope exit, whatever the body did.
class CwdScope {
public:
    explicit CwdScope(fs::path const& dir) : saved_{fs::current_path()} {
        fs::current_path(dir);
    }
    CwdScope(CwdScope const&)            = delete;
    CwdScope& operator=(CwdScope const&) = delete;
    ~CwdScope() {
        std::error_code ec;
        fs::current_path(saved_, ec);
    }

private:
    fs::path saved_;
};

// What one arm writes, and what its build must produce.
struct Arm {
    // Writes `<root>/<projectDir>/…` (the project) and `<root>/decoy/…` (the
    // look-alikes). `root` is absolute.
    std::function<void(fs::path const& root, fs::path const& proj)> write;
    // The run's expected exit value.
    std::uint32_t exitCode = kAuthor;
    // false ⇒ the build is given NO command-line output directory, so the
    // manifest's `output` or the default `target/` decides where it goes.
    bool cliOutput = true;
    // Extra checks, run with the working directory still in place.
    std::function<void(fs::path const& root, fs::path const& proj,
                       fs::path const& cwd, fs::path const& artifact)> check;
    std::string projectDir = "proj";
};

// Where one column starts the build, and how it names the manifest.
struct Invocation {
    fs::path    cwd;
    std::string projectArg;
};

[[nodiscard]] Invocation invocationFor(Column column, fs::path const& root,
                                       std::string const& projectDir,
                                       fs::path const& proj) {
    switch (column) {
        case Column::ManifestDir:
            return {proj, std::string{kManifestName}};
        case Column::Empty:
            return {root / "empty",
                    "../" + projectDir + "/" + std::string{kManifestName}};
        case Column::Decoy:
            return {root / "decoy", g(proj / std::string{kManifestName})};
    }
    return {};
}

void runArm(Arm const& arm) {
    Helpers const& h = helpers();
    ASSERT_TRUE(h.error.empty()) << h.error;

    for (Column const column : {Column::ManifestDir, Column::Empty, Column::Decoy}) {
        SCOPED_TRACE(columnName(column));
        ScratchDir scratch{Location::InsideRepo, "manifest-path-base"};
        fs::path const root = scratch.path();
        fs::path const proj = root / arm.projectDir;
        fs::create_directories(proj);
        fs::create_directories(root / "empty");
        fs::create_directories(root / "decoy");
        arm.write(root, proj);
        if (::testing::Test::HasFatalFailure()) return;

        auto const [cwd, projectArg] = invocationFor(column, root, arm.projectDir, proj);
        CwdScope const inCwd{cwd};
        Program prog;
        if (arm.cliOutput) prog.setOutputDir(root / "out");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(projectArg, rep);
        EXPECT_EQ(rc, 0) << "--project " << projectArg << renderDiagnostics(rep);
        if (rc != 0) continue;

        auto const& paths = prog.artifactPaths();
        ASSERT_EQ(paths.size(), 1u);
        ASSERT_TRUE(paths[0].has_value());
        fs::path const artifact = fs::absolute(*paths[0]);
        auto const run = runBinary(artifact);
        ASSERT_TRUE(run.spawned) << g(artifact) << ": " << run.diagnostic;
        EXPECT_FALSE(run.timedOut);
        EXPECT_EQ(run.exitCode, arm.exitCode)
            << "the build read the wrong file: " << kDecoy
            << " is every look-alike's answer";
        if (arm.check) arm.check(root, proj, cwd, artifact);
    }
}

// An arm whose build must be REFUSED from every column, and what the refusal
// must say. `check` reads the diagnostics with the working directory in place.
struct RefusedArm {
    std::function<void(fs::path const& root, fs::path const& proj)> write;
    std::function<void(DiagnosticReporter const& rep, fs::path const& proj)> check;
    std::string projectDir = "proj";
};

void runRefusedArm(RefusedArm const& arm) {
    Helpers const& h = helpers();
    ASSERT_TRUE(h.error.empty()) << h.error;

    for (Column const column : {Column::ManifestDir, Column::Empty, Column::Decoy}) {
        SCOPED_TRACE(columnName(column));
        ScratchDir scratch{Location::InsideRepo, "manifest-path-base"};
        fs::path const root = scratch.path();
        fs::path const proj = root / arm.projectDir;
        fs::create_directories(proj);
        fs::create_directories(root / "empty");
        fs::create_directories(root / "decoy");
        arm.write(root, proj);
        if (::testing::Test::HasFatalFailure()) return;

        auto const [cwd, projectArg] = invocationFor(column, root, arm.projectDir, proj);
        CwdScope const inCwd{cwd};
        Program prog;
        prog.setOutputDir(root / "out");
        DiagnosticReporter rep;
        int const rc = prog.compileProject(projectArg, rep);
        EXPECT_NE(rc, 0) << "--project " << projectArg
                         << ": the build must be refused" << renderDiagnostics(rep);
        arm.check(rep, proj);
    }
}

// The text of the ONE `D_ScriptSpawnFailed` a refused hook build carries.
[[nodiscard]] std::string spawnFailureText(DiagnosticReporter const& rep) {
    std::string text;
    int         count = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::D_ScriptSpawnFailed) {
            ++count;
            text = d.contextPrefix + d.actual;
        }
    }
    EXPECT_EQ(count, 1) << "exactly one hook spawn failure" << renderDiagnostics(rep);
    return text;
}

// The file the spawn layer says it probed: the text between
// "The path probed was '" and the next quote, read as UTF-8.
[[nodiscard]] std::optional<fs::path> probedPathIn(std::string const& text) {
    constexpr std::string_view kLead = "The path probed was '";
    auto const at = text.find(kLead);
    if (at == std::string::npos) return std::nullopt;
    auto const from = at + kLead.size();
    auto const to   = text.find('\'', from);
    if (to == std::string::npos) return std::nullopt;
    std::u8string const u8(text.begin() + static_cast<std::ptrdiff_t>(from),
                           text.begin() + static_cast<std::ptrdiff_t>(to));
    return fs::path{u8};
}

// Two spellings of ONE file: each made canonical as far as it exists (so a
// `..` and a symlinked ancestor compare by what they name), and case-blind on
// Windows, whose default filesystem is.
[[nodiscard]] bool sameFile(fs::path const& a, fs::path const& b) {
    auto spell = [](fs::path const& p) {
        std::error_code ec;
        fs::path const c = fs::weakly_canonical(p, ec);
        std::string s = (ec ? p.lexically_normal() : c.lexically_normal()).generic_string();
#if defined(_WIN32)
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
#endif
        return s;
    };
    return spell(a) == spell(b);
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// THE RULE ITSELF — lexical, no filesystem
// ════════════════════════════════════════════════════════════════════════════

// The four cases of the one function. LEXICAL: none of these paths exist, and
// the answers must not depend on whether they do — a pre-build hook's output is
// resolved before the hook has written it.
TEST(ResolveManifestPath, IsLexicalAndKeepsARootedEntry) {
    fs::path const dir = fs::path{"no-such-parent"} / "proj";
    EXPECT_EQ(dss::resolveManifestPath(dir, "src/main.c"), dir / "src/main.c");
    EXPECT_EQ(dss::resolveManifestPath({}, "src/main.c"), fs::path{"src/main.c"})
        << "an EMPTY directory is the working directory: the entry stays as written";
    EXPECT_EQ(dss::resolveManifestPath(dir, "."), dir)
        << "`.` names the manifest's directory itself, with no `.` component";
    fs::path const absolute = fs::absolute("elsewhere") / "x.c";
    EXPECT_EQ(dss::resolveManifestPath(dir, absolute), absolute);
    // A multi-separator authority is ROOTED although `is_absolute()` may answer
    // false for it; re-basing it would glue the manifest's drive onto another
    // machine's name.
    EXPECT_EQ(dss::resolveManifestPath(dir, "//host/share/x.c"),
              fs::path{"//host/share/x.c"});
    // The spelling form keeps an untouched entry CHARACTER FOR CHARACTER.
    EXPECT_EQ(dss::resolveManifestPathSpelling({}, "./a//b.c"), "./a//b.c");
    EXPECT_EQ(dss::resolveManifestPathSpelling(dir, "b.c"), g(dir / "b.c"));
}

TEST(ResolveManifestPath, ManifestDirectoryIsTheLexicalParent) {
    EXPECT_EQ(dss::manifestDirectoryOf("app.dss-project.json"), fs::path{});
    EXPECT_EQ(dss::manifestDirectoryOf("../proj/app.dss-project.json"),
              fs::path{"../proj"});
}

// ════════════════════════════════════════════════════════════════════════════
// THE MATRIX — one TEST per field
// ════════════════════════════════════════════════════════════════════════════

TEST(ManifestPathBase, SourcesLiteral) {
    Arm arm;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        writeText(proj / "main.c", returning(kAuthor));
        writeText(root / "decoy" / "main.c", returning(kDecoy));
        writeText(proj / std::string{kManifestName}, rootManifest({"main.c"}));
    };
    runArm(arm);
}

TEST(ManifestPathBase, SourcesGlob) {
    Arm arm;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        writeText(proj / "src" / "main.c", returning(kAuthor));
        writeText(root / "decoy" / "src" / "main.c", returning(kDecoy));
        writeText(proj / std::string{kManifestName}, rootManifest({"src/m*.c"}));
    };
    runArm(arm);
}

// A project whose OWN directory name holds a glob metacharacter. The pattern's
// literal prefix is re-based onto the directory — never the directory joined
// onto the pattern first, which would read `[1]` as a character class matching
// `1` and walk from the directory's parent. The sibling `proj1/` is exactly
// what that misreading would match, so it holds a decoy.
TEST(ManifestPathBase, SourcesGlobInADirectoryNamedWithAMetacharacter) {
    Arm arm;
    arm.projectDir = "proj[1]";
    arm.write = [](fs::path const& root, fs::path const& proj) {
        writeText(proj / "src" / "main.c", returning(kAuthor));
        writeText(root / "proj1" / "src" / "main.c", returning(kDecoy));
        writeText(root / "decoy" / "src" / "main.c", returning(kDecoy));
        writeText(proj / std::string{kManifestName}, rootManifest({"src/*.c"}));
    };
    runArm(arm);
}

TEST(ManifestPathBase, Includes) {
    Arm arm;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        writeText(proj / "main.c",
                  "#include \"cfg.h\"\nint main(void){ return CFG_VALUE; }\n");
        writeText(proj / "inc" / "cfg.h",
                  "#define CFG_VALUE " + std::to_string(kAuthor) + "\n");
        writeText(root / "decoy" / "inc" / "cfg.h",
                  "#define CFG_VALUE " + std::to_string(kDecoy) + "\n");
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}, {"\"includes\": [\"inc\"]"}));
    };
    runArm(arm);
}

TEST(ManifestPathBase, ResolveLibraries) {
    Arm arm;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        writeText(proj / "main.c",
                  "extern int lib_value(void);\n"
                  "int main(void){ return lib_value(); }\n");
        place(helpers().authorArchive, proj / "libs" / archiveName());
        place(helpers().decoyArchive, root / "decoy" / "libs" / archiveName());
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")},
                               {"\"resolveLibraries\": [" + q("libs/" + archiveName())
                                    + "]"}));
    };
    runArm(arm);
}

// The artifact goes to `<manifest dir>/dist`, and nothing goes to `<cwd>/dist`.
TEST(ManifestPathBase, Output) {
    Arm arm;
    arm.cliOutput = false;
    arm.write = [](fs::path const&, fs::path const& proj) {
        writeText(proj / "main.c", returning(kAuthor));
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}, {"\"output\": \"dist\""}));
    };
    arm.check = [](fs::path const&, fs::path const& proj, fs::path const& cwd,
                   fs::path const& artifact) {
        fs::path const expected = proj / "dist" / formatName() / exeName("app");
        EXPECT_TRUE(fs::exists(expected)) << g(expected);
        EXPECT_TRUE(fs::equivalent(artifact, expected)) << g(artifact);
        if (!fs::equivalent(cwd, proj)) {
            EXPECT_FALSE(fs::exists(cwd / "dist")) << g(cwd / "dist");
        }
    };
    runArm(arm);
}

// With neither `--output` nor `output`: `<manifest dir>/target`, never
// `<cwd>/target` (Cargo's and MSBuild's precedent).
TEST(ManifestPathBase, DefaultOutputIsBesideTheManifest) {
    Arm arm;
    arm.cliOutput = false;
    arm.write = [](fs::path const&, fs::path const& proj) {
        writeText(proj / "main.c", returning(kAuthor));
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}));
    };
    arm.check = [](fs::path const&, fs::path const& proj, fs::path const& cwd,
                   fs::path const& artifact) {
        fs::path const expected = proj / "target" / formatName() / exeName("app");
        EXPECT_TRUE(fs::exists(expected)) << g(expected);
        EXPECT_TRUE(fs::equivalent(artifact, expected)) << g(artifact);
        if (!fs::equivalent(cwd, proj)) {
            EXPECT_FALSE(fs::exists(cwd / "target")) << g(cwd / "target");
        }
    };
    runArm(arm);
}

// The hook's WORKING DIRECTORY: its program is named ABSOLUTELY here, so only
// the directory it starts in is under test. It writes `hook_out.c` where it
// starts, and the manifest compiles `<manifest dir>/hook_out.c`.
TEST(ManifestPathBase, HookWorkingDirectory) {
    Arm arm;
    arm.exitCode = kHook;
    arm.write = [](fs::path const&, fs::path const& proj) {
        writeText(proj / "main.c",
                  "extern int hook_value(void);\n"
                  "int main(void){ return hook_value(); }\n");
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c"), g(proj / "hook_out.c")},
                               {preBuildRun(g(helpers().authorWriter))}));
    };
    runArm(arm);
}

// The hook's PROGRAM, named by a relative path: the author's tool is beside the
// manifest, the decoy directory holds a tool of the same name that writes a
// different answer.
TEST(ManifestPathBase, HookProgramPath) {
    Arm arm;
    arm.exitCode = kHook;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        std::string const tool = "tools/" + exeName("gen");
        place(helpers().authorWriter, proj / tool);
        place(helpers().decoyWriter, root / "decoy" / tool);
        writeText(proj / "main.c",
                  "extern int hook_value(void);\n"
                  "int main(void){ return hook_value(); }\n");
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c"), g(proj / "hook_out.c")},
                               {preBuildRun(tool)}));
    };
    runArm(arm);
}

// ── the MISSING-TOOL arms: what the operator is told ────────────────────────
//
// [[D-SPAWN-PATH-FORM-PROGRAM-REPORTED-AS-LOOKED-UP-IN-PATH]]. The hook's
// program, named by a relative path, is NOT beside the manifest — and the decoy
// directory holds a tool of that very name, so the decoy column also proves the
// build did not fall back to the working directory. From every column the
// refusal must name the ONE file probed — the manifest's own `tools/gen`, by
// whatever spelling reached it — and say that no PATH search happened.
// ✔MEASURED before the fix, through `dsscp --project` from the empty column:
// "'../proj/tools/gen.exe' was not found as an executable. It was looked up in
// PATH (the current directory is deliberately never searched); pass a path
// containing a directory separator to run a local tool." — no PATH lookup had
// happened, the relative path WAS resolved against the current directory, and
// the name already had the separator the advice asked for.
// RED-ON-DISABLE: collapse the spawn layer's branch (`whereItWasLookedFor`
// always returning the PATH sentence) and this arm reds in all three columns,
// while the bare-name arm below — the control — stays green.
TEST(ManifestPathBase, HookProgramMissingNamesTheFileProbed) {
    RefusedArm arm;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        std::string const tool = "tools/" + exeName("gen");
        place(helpers().decoyWriter, root / "decoy" / tool);
        writeText(proj / "main.c", returning(kAuthor));
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}, {preBuildRun(tool)}));
    };
    arm.check = [](DiagnosticReporter const& rep, fs::path const& proj) {
        std::string const text = spawnFailureText(rep);
        EXPECT_NE(text.find("was NOT searched for in PATH"), std::string::npos)
            << "a path-form program is never searched for; the text must say so: "
            << text;
        EXPECT_EQ(text.find("It was looked up in PATH"), std::string::npos)
            << "the PATH sentence describes a lookup that did not happen: " << text;
        EXPECT_EQ(text.find("pass a path containing a directory separator"),
                  std::string::npos)
            << "the advice asks for the separator the name already has: " << text;
        EXPECT_NE(text.find(", and no regular file exists there. The process was "
                            "never created"),
                  std::string::npos)
            << "the text must say what is at the probed path, and the hook "
               "wrapper's next sentence must follow ONE period: "
            << text;
        auto const probed = probedPathIn(text);
        ASSERT_TRUE(probed.has_value()) << "the text names no probed path: " << text;
        EXPECT_TRUE(sameFile(*probed, proj / "tools" / exeName("gen")))
            << "the probed path must be the MANIFEST's own tools/"
            << exeName("gen") << ", got '" << g(*probed) << "': " << text;
#if defined(_WIN32)
        EXPECT_NE(text.find("(as given, then with each PATHEXT extension appended: "),
                  std::string::npos)
            << "on Windows the extension rule decides which files were tried: " << text;
#endif
    };
    runRefusedArm(arm);
}

// THE CONTROL, and the bare-name branch pinned UNCHANGED: a bare `run[0]` is a
// PATH lookup from every column — never re-based onto the manifest — and its
// sentence is the one it always was.
TEST(ManifestPathBase, HookBareProgramMissingKeepsThePathSentence) {
    RefusedArm arm;
    arm.write = [](fs::path const&, fs::path const& proj) {
        writeText(proj / "main.c", returning(kAuthor));
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")},
                               {preBuildRun("dss-no-such-hook-3f1c9a2e")}));
    };
    arm.check = [](DiagnosticReporter const& rep, fs::path const&) {
        std::string const text = spawnFailureText(rep);
        EXPECT_NE(text.find("spawnAndWaitInherit: 'dss-no-such-hook-3f1c9a2e' was "
                            "not found as an executable. It was looked up in PATH "
                            "(the current directory is deliberately never "
                            "searched); pass a path containing a directory "
                            "separator to run a local tool."),
                  std::string::npos)
            << "the bare-name sentence must be unchanged: " << text;
        // The hook wrapper's next sentence follows ONE period — it used to add
        // its own after the spawn text's, rendering "local tool.. The process".
        EXPECT_NE(text.find("to run a local tool. The process was never created"),
                  std::string::npos)
            << text;
        EXPECT_EQ(text.find("was NOT searched for in PATH"), std::string::npos) << text;
        EXPECT_EQ(text.find("(the manifest's '"), std::string::npos)
            << "a bare name is never re-based onto the manifest: " << text;
    };
    runRefusedArm(arm);
}

// `dependsOn` was manifest-relative before the rule changed; this arm keeps it so.
TEST(ManifestPathBase, DependsOnPath) {
    Arm arm;
    arm.exitCode = kModule;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        for (auto const& [dir, value] :
             {std::pair{proj / "mod", kModule}, std::pair{root / "decoy" / "mod", kDecoy}}) {
            writeText(dir / "m.c", "int module_value(void){ return "
                                       + std::to_string(value) + "; }\n");
            writeText(dir / std::string{kDepManifest}, manifest("module", {"m.c"}));
        }
        writeText(proj / "main.c",
                  "extern int module_value(void);\n"
                  "int main(void){ return module_value(); }\n");
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}, {dependsOnPath("mod")}));
    };
    runArm(arm);
}

// ── a DEPENDENCY's own paths — the dependency is named absolutely, so only its
// own field is under test. The look-alikes sit in the decoy directory because
// the defect these arms close resolved a dependency's paths against the
// CONSUMER's working directory.

// [[D-DEPS-ARTIFACTLINK-INCLUDES-AND-LIBRARIES-RESOLVE-AGAINST-THE-CONSUMERS-CWD]]
TEST(ManifestPathBase, DependencyIncludes) {
    Arm arm;
    arm.exitCode = kDepInclude;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        fs::path const dep = proj / "dep";
        writeText(dep / "dep.c",
                  "#include \"libcfg.h\"\nint dep_value(void){ return LIBCFG_VALUE; }\n");
        writeText(dep / "inc" / "libcfg.h",
                  "#define LIBCFG_VALUE " + std::to_string(kDepInclude) + "\n");
        writeText(root / "decoy" / "inc" / "libcfg.h",
                  "#define LIBCFG_VALUE " + std::to_string(kDecoy) + "\n");
        writeText(dep / std::string{kDepManifest},
                  manifest("staticlib", {"dep.c"}, {"\"includes\": [\"inc\"]"}));
        writeText(proj / "main.c",
                  "extern int dep_value(void);\n"
                  "int main(void){ return dep_value(); }\n");
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}, {dependsOnPath(g(dep))}));
    };
    runArm(arm);
}

// [[D-DEPS-ARTIFACTLINK-INCLUDES-AND-LIBRARIES-RESOLVE-AGAINST-THE-CONSUMERS-CWD]]
TEST(ManifestPathBase, DependencyResolveLibraries) {
    Arm arm;
    arm.exitCode = kDepLibrary;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        fs::path const dep = proj / "dep";
        writeText(dep / "dep.c",
                  "extern int lib_value(void);\n"
                  "int dep_value(void){ return lib_value() + 4; }\n");
        place(helpers().authorArchive, dep / "libs" / archiveName());
        place(helpers().decoyArchive, root / "decoy" / "libs" / archiveName());
        writeText(dep / std::string{kDepManifest},
                  manifest("staticlib", {"dep.c"},
                           {"\"resolveLibraries\": [" + q("libs/" + archiveName()) + "]"}));
        writeText(proj / "main.c",
                  "extern int dep_value(void);\n"
                  "int main(void){ return dep_value(); }\n");
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}, {dependsOnPath(g(dep))}));
    };
    runArm(arm);
}

// [[D-DEPS-HOOK-PROGRAM-PATH-RESOLVES-AGAINST-THE-CONSUMERS-CWD]]: the
// dependency's hook ran in the dependency's directory, but its program was
// looked up from the consumer's.
TEST(ManifestPathBase, DependencyHookProgramPath) {
    Arm arm;
    arm.exitCode = kHook;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        fs::path const dep = proj / "dep";
        std::string const tool = "tools/" + exeName("gen");
        place(helpers().authorWriter, dep / tool);
        place(helpers().decoyWriter, root / "decoy" / tool);
        writeText(dep / std::string{kDepManifest},
                  manifest("staticlib", {"hook_out.c"}, {preBuildRun(tool)}));
        writeText(proj / "main.c",
                  "extern int hook_value(void);\n"
                  "int main(void){ return hook_value(); }\n");
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}, {dependsOnPath(g(dep))}));
    };
    runArm(arm);
}

// [[D-DEPS-MODULE-INCLUDES-AND-DEFINES-SILENTLY-DROPPED]]: a module's own
// `includes` reach its own merged source, resolved against the module's
// directory. (Its `defines`, and the half that keeps both away from the
// consumer's sources, are pinned in `test_dependency_resolver.cpp`.)
TEST(ManifestPathBase, ModuleIncludes) {
    Arm arm;
    arm.exitCode = kModule;
    arm.write = [](fs::path const& root, fs::path const& proj) {
        fs::path const mod = proj / "mod";
        writeText(mod / "m.c",
                  "#include \"modcfg.h\"\nint module_value(void){ return MODCFG_VALUE; }\n");
        writeText(mod / "inc" / "modcfg.h",
                  "#define MODCFG_VALUE " + std::to_string(kModule) + "\n");
        writeText(root / "decoy" / "inc" / "modcfg.h",
                  "#define MODCFG_VALUE " + std::to_string(kDecoy) + "\n");
        writeText(mod / std::string{kDepManifest},
                  manifest("module", {"m.c"}, {"\"includes\": [\"inc\"]"}));
        writeText(proj / "main.c",
                  "extern int module_value(void);\n"
                  "int main(void){ return module_value(); }\n");
        writeText(proj / std::string{kManifestName},
                  rootManifest({g(proj / "main.c")}, {dependsOnPath(g(mod))}));
    };
    runArm(arm);
}
