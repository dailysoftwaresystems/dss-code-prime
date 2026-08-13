// ── THE EDITOR'S TARGET CHANNEL (D-LSP-ASSEMBLY-DIALECT-UNSERVABLE) ──────────
//
// `asm-x86_64-att` and `asm-arm64-gas` BOTH declare `.s`/`.S`, so the extension
// alone cannot name a language and `SchemaCache::resolveByExtension` refuses.
// Refusing is honest, and it leaves an editor with no language service at all
// on every assembly file. The tie-breaker is the compile TARGET, which is a
// workspace fact — read from a `*.dss-project.json` at the workspace root, via
// the SAME parser `Program::compileProject` uses.
//
// ★★ THE PROPERTY THIS FILE EXISTS TO PIN: byte-identical `.s`, two different
// workspaces, TWO DIFFERENT ANSWERS. Anything that resolves `.s` to a fixed
// language — however it is spelled — fails
// `SameExtensionInTwoWorkspacesResolvesToTwoDifferentDialects`.
//
// EVERY fixture is a REAL manifest written to a REAL directory and read by the
// real `dss::loadProjectConfig`. Nothing here hand-builds a `ProjectConfig`: a
// pin whose subject is a struct literal tests the literal, and the whole claim
// being made is about a file on disk.

#include "core/types/parse_diagnostic.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/workspace_project.hpp"
#include "lsp_test_helpers.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using dss::lsp::describeUnresolvedSchema;
using dss::lsp::pathFromFileUri;
using dss::lsp::resolveWorkspaceLanguagePreference;
using dss::lsp::SchemaCache;
using dss::lsp::SchemaResolveErrorKind;
using dss::lsp::WorkspaceProjectErrorKind;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
using json = nlohmann::json;

namespace {

// Write `body` to `<dir>/<name>`. Throws on failure — a fixture we could not
// build is a setup error the suite must SEE, never something to skip past.
void writeFile(fs::path const& dir, std::string_view name, std::string_view body) {
    std::ofstream out{dir / name, std::ios::binary};
    if (!out) {
        throw std::runtime_error("fixture write failed: "
                                 + (dir / name).string());
    }
    out << body;
    if (!out) {
        throw std::runtime_error("fixture write incomplete: "
                                 + (dir / name).string());
    }
}

// A manifest naming exactly `targets`. Every REQUIRED field of the shared
// schema is present, because the shared parser is the one that reads it —
// these fixtures are manifests a human would actually commit.
[[nodiscard]] std::string manifest(std::string_view targetsJsonArray) {
    return std::string{R"({
  "language": "c-subset",
  "artifactProfile": "cli",
  "targets": )"} + std::string{targetsJsonArray} + R"(,
  "sources": ["main.c"]
})";
}

// Portable RAII env override. Restores the prior value (or clears) on exit so
// the CMake-set `DSS_CONFIG_ROOT` (`dss_add_test`) survives the test.
//
// ⚠ THIRD COPY IN THE TREE (`tests/core/test_config_path_walk.cpp` and
// `tests/test_support/test_repo_root.cpp` carry the other two) ⇒
// `D-TEST-SCOPED-ENV-DUPLICATED-THREE-WAYS`. Hoisting it into
// `tests/test_support/` is a `tests/test_support/**` change, outside this
// lane's files.
class ScopedEnv {
public:
    ScopedEnv(char const* name, std::string const& value) : name_(name) {
        if (char const* prev = std::getenv(name)) { had_ = true; prev_ = prev; }
        set(value);
    }
    ~ScopedEnv() { had_ ? set(prev_) : clear(); }
    ScopedEnv(ScopedEnv const&)            = delete;
    ScopedEnv& operator=(ScopedEnv const&) = delete;

private:
    void set(std::string const& v) {
#ifdef _WIN32
        ::_putenv_s(name_, v.c_str());
#else
        ::setenv(name_, v.c_str(), /*overwrite=*/1);
#endif
    }
    void clear() {
#ifdef _WIN32
        ::_putenv_s(name_, "");
#else
        ::unsetenv(name_);
#endif
    }
    char const* name_;
    bool        had_ = false;
    std::string prev_;
};

} // namespace

// ── file:// URI → path ──────────────────────────────────────────────────────

TEST(WorkspaceProject, FileUriRoundTripsThroughPathConversion) {
    ScratchDir scratch{Location::Temp, "lsp-uri"};
    const auto uri = dss::lsp::testing::fileUriFromPath(scratch.path());
    auto back = pathFromFileUri(uri);
    ASSERT_TRUE(back.has_value()) << "could not decode " << uri;
    EXPECT_TRUE(fs::equivalent(*back, scratch.path()))
        << "decoded " << back->string() << " from " << uri;
}

TEST(WorkspaceProject, PercentEncodedUriIsDecoded) {
    auto p = pathFromFileUri("file:///tmp/a%20b/c");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->generic_string(), "/tmp/a b/c");
}

// A remote/virtual workspace has NO local directory to scan. Answering with a
// guessed path would search the wrong tree and report the wrong project — the
// exact silent-answer shape this cycle removed from the resolver.
TEST(WorkspaceProject, NonFileUriIsRefusedRatherThanGuessed) {
    EXPECT_FALSE(pathFromFileUri("vscode-vfs://github/o/r").has_value());
    EXPECT_FALSE(pathFromFileUri("file://someremotehost/share/x").has_value());
    EXPECT_FALSE(pathFromFileUri("untitled:Untitled-1").has_value());
}

// ── ★★ THE PROPERTY: one extension, two workspaces, two answers ─────────────

namespace {

// Build a workspace whose single manifest declares `targetSpec`, then resolve
// `ext` through the real SchemaCache using that workspace's preference.
struct WorkspaceResolution {
    std::vector<std::string> preferred;
    std::string              languageName;   // display name of the schema
};

[[nodiscard]] WorkspaceResolution resolveInWorkspace(
    fs::path const& root, std::string_view targetSpec, std::string_view ext) {
    writeFile(root, "app.dss-project.json",
              manifest(R"([")" + std::string{targetSpec} + R"("])"));
    const std::array<fs::path, 1> roots{root};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    EXPECT_TRUE(pref.has_value())
        << "workspace preference failed for " << targetSpec;
    if (!pref.has_value()) return {};

    SchemaCache cache;
    auto resolved = cache.resolveByExtension(ext, pref->languages);
    EXPECT_TRUE(resolved.has_value())
        << "resolveByExtension(" << ext << ") failed: "
        << (resolved.has_value() ? std::string{} : resolved.error().detail);
    if (!resolved.has_value()) return {pref->languages, {}};
    return {pref->languages, std::string{(*resolved)->name()}};
}

} // namespace

// ★★★ THE PIN. Same extension. Same shipped languages. Same resolver. TWO
// workspaces differing ONLY in the target their manifest declares — and two
// different answers. A resolver that maps `.s` to a fixed language cannot pass
// this, no matter which language it picks, because it must be wrong in one of
// the two halves.
TEST(WorkspaceProject, SameExtensionInTwoWorkspacesResolvesToTwoDifferentDialects) {
    ScratchDir armWs{Location::Temp, "lsp-ws-arm"};
    ScratchDir x86Ws{Location::Temp, "lsp-ws-x86"};

    auto arm = resolveInWorkspace(armWs.path(), "arm64:elf64-aarch64-linux-exec",
                                  ".s");
    auto x86 = resolveInWorkspace(x86Ws.path(), "x86_64:elf64-x86_64-linux-exec",
                                  ".s");

    // The preference each workspace derived, from its target's
    // `defaultAssemblyLanguage`.
    EXPECT_EQ(arm.preferred, (std::vector<std::string>{"asm-arm64-gas"}));
    EXPECT_EQ(x86.preferred, (std::vector<std::string>{"asm-x86_64-att"}));

    // The schemas those preferences selected. Display names come from the
    // `.lang.json` documents themselves.
    EXPECT_EQ(arm.languageName, "AsmArm64Gas");
    EXPECT_EQ(x86.languageName, "AsmX86_64Att");
    EXPECT_NE(arm.languageName, x86.languageName)
        << "the whole point: the SAME `.s` must mean different things in these "
           "two workspaces";
}

// The uppercase spelling travels the same index and must reach the same
// verdict — a `.S` that resolved differently from a `.s` would be the
// collision surviving behind a case difference.
TEST(WorkspaceProject, UppercaseExtensionResolvesThroughTheSamePreference) {
    ScratchDir ws{Location::Temp, "lsp-ws-upper"};
    auto r = resolveInWorkspace(ws.path(), "arm64:elf64-aarch64-linux-exec", ".S");
    EXPECT_EQ(r.languageName, "AsmArm64Gas");
}

// A preference must break TIES ONLY. `.c` has exactly one claimant, so a
// workspace preferring an assembly dialect must not perturb it — if it did,
// this feature would have broken every C file in every workspace.
TEST(WorkspaceProject, PreferenceDoesNotPerturbAnUnambiguousExtension) {
    ScratchDir ws{Location::Temp, "lsp-ws-unambig"};
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["arm64:elf64-aarch64-linux-exec"])"));
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_TRUE(pref.has_value());

    SchemaCache cache;
    auto r = cache.resolveByExtension(".c", pref->languages);
    ASSERT_TRUE(r.has_value()) << "exactly one shipped language claims .c";
    EXPECT_EQ((*r)->name(), "CSubset");
}

// ★ TWO TARGETS THAT DISAGREE LEAVE THE TIE UNBROKEN. This is the case a
// first-wins implementation passes by accident and gets WRONG: it would answer
// with whichever target the manifest listed first. Two preferred languages both
// claiming `.s` is not an answer, it is a restated question.
TEST(WorkspaceProject, WorkspaceDeclaringBothCpusStaysAmbiguous) {
    ScratchDir ws{Location::Temp, "lsp-ws-both"};
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["arm64:elf64-aarch64-linux-exec",)"
                       R"("x86_64:elf64-x86_64-linux-exec"])"));
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_TRUE(pref.has_value());
    EXPECT_EQ(pref->languages.size(), 2u);

    SchemaCache cache;
    auto r = cache.resolveByExtension(".s", pref->languages);
    ASSERT_FALSE(r.has_value())
        << "a workspace that declares BOTH CPUs has not chosen a dialect; "
           "answering would be manifest order deciding what the source means";
    EXPECT_EQ(r.error().kind, SchemaResolveErrorKind::AmbiguousExtension);
    EXPECT_NE(r.error().detail.find("matched 2 of them"), std::string::npos)
        << "the error must say the preference matched TWO — 'ambiguous' alone "
           "does not distinguish this from having no preference at all; got: "
        << r.error().detail;
}

// Several manifests at one root (the shipped convention — one per leg) UNION.
// Agreeing manifests must still resolve: deduping by NAME is what stops a
// multi-format workspace from looking self-contradictory.
TEST(WorkspaceProject, SeveralManifestsAgreeingOnOneCpuStillResolve) {
    ScratchDir ws{Location::Temp, "lsp-ws-multi"};
    writeFile(ws.path(), "app.elf.dss-project.json",
              manifest(R"(["x86_64:elf64-x86_64-linux-exec"])"));
    writeFile(ws.path(), "app.pe.dss-project.json",
              manifest(R"(["x86_64:pe64-x86_64-windows-exec"])"));
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_TRUE(pref.has_value());
    EXPECT_EQ(pref->projectFiles.size(), 2u);
    EXPECT_EQ(pref->languages, (std::vector<std::string>{"asm-x86_64-att"}));

    SchemaCache cache;
    auto r = cache.resolveByExtension(".s", pref->languages);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_EQ((*r)->name(), "AsmX86_64Att");
}

// ── Every failure mode, by KIND and by MESSAGE ──────────────────────────────

TEST(WorkspaceProject, NoWorkspaceRootIsItsOwnFailure) {
    auto pref = resolveWorkspaceLanguagePreference({});
    ASSERT_FALSE(pref.has_value());
    EXPECT_EQ(pref.error().kind, WorkspaceProjectErrorKind::NoWorkspaceRoot);
    EXPECT_NE(pref.error().detail.find("workspaceFolders"), std::string::npos)
        << "the message must name the protocol field the client omitted; got: "
        << pref.error().detail;
}

TEST(WorkspaceProject, EmptyWorkspaceReportsProjectFileNotFound) {
    ScratchDir ws{Location::Temp, "lsp-ws-empty"};
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_FALSE(pref.has_value());
    EXPECT_EQ(pref.error().kind,
              WorkspaceProjectErrorKind::ProjectFileNotFound);
    EXPECT_NE(pref.error().detail.find(".dss-project.json"), std::string::npos)
        << "the message must name the suffix that was looked for; got: "
        << pref.error().detail;
    EXPECT_NE(pref.error().detail.find(ws.path().string()), std::string::npos)
        << "the message must name the directory that was searched; got: "
        << pref.error().detail;
}

// ★ A MANIFEST NAMING NO TARGET IS REJECTED BY THE SHARED PARSER, NOT BY A
// SECOND CHECK HERE. `targets` is a required non-empty array in the ONE
// project-config schema, so `[]` never reaches this layer as a parsed config.
// That is why there is no `ProjectDeclaresNoTarget` kind — it would be an
// unreachable branch. What matters to the operator is that the parser's OWN
// diagnostic survives the trip into the editor, naming the field.
TEST(WorkspaceProject, ManifestNamingNoTargetIsRejectedByTheSharedParser) {
    ScratchDir ws{Location::Temp, "lsp-ws-notarget"};
    writeFile(ws.path(), "app.dss-project.json", manifest("[]"));
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_FALSE(pref.has_value());
    EXPECT_EQ(pref.error().kind,
              WorkspaceProjectErrorKind::ProjectFileLoadFailed);
    EXPECT_NE(pref.error().detail.find("targets"), std::string::npos)
        << "the shared parser's message names the offending field and must "
           "reach the editor verbatim; got: " << pref.error().detail;
    EXPECT_NE(pref.error().detail.find("C_MissingField"), std::string::npos)
        << "the shared parser's diagnostic CODE must survive too; got: "
        << pref.error().detail;
}

// Same kind, different message: proving the parser is genuinely being run
// rather than a shape-check being imitated.
TEST(WorkspaceProject, UnknownManifestKeyIsRejectedByTheSharedParser) {
    ScratchDir ws{Location::Temp, "lsp-ws-badkey"};
    writeFile(ws.path(), "app.dss-project.json", R"({
  "language": "c-subset",
  "artifactProfile": "cli",
  "targets": ["x86_64:elf64-x86_64-linux-exec"],
  "sources": ["main.c"],
  "targetz": ["typo"]
})");
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_FALSE(pref.has_value());
    EXPECT_EQ(pref.error().kind,
              WorkspaceProjectErrorKind::ProjectFileLoadFailed);
    EXPECT_NE(pref.error().detail.find("targetz"), std::string::npos)
        << "the shared parser's CLOSED key vocabulary is inherited, not "
           "re-implemented; got: " << pref.error().detail;
}

TEST(WorkspaceProject, MalformedTargetSpecIsItsOwnFailure) {
    ScratchDir ws{Location::Temp, "lsp-ws-badspec"};
    writeFile(ws.path(), "app.dss-project.json", manifest(R"(["x86_64"])"));
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_FALSE(pref.has_value());
    EXPECT_EQ(pref.error().kind,
              WorkspaceProjectErrorKind::TargetSpecMalformed);
    EXPECT_NE(pref.error().detail.find("MissingColon"), std::string::npos)
        << "the driver's own spec-error vocabulary must be carried through, "
           "not flattened; got: " << pref.error().detail;
}

TEST(WorkspaceProject, UnknownTargetIsItsOwnFailure) {
    ScratchDir ws{Location::Temp, "lsp-ws-badtarget"};
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["no-such-cpu:elf64-x86_64-linux-exec"])"));
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_FALSE(pref.has_value());
    EXPECT_EQ(pref.error().kind,
              WorkspaceProjectErrorKind::TargetConfigLoadFailed);
    EXPECT_NE(pref.error().detail.find("no-such-cpu"), std::string::npos)
        << "the message must name the target that could not be loaded; got: "
        << pref.error().detail;
}

// ★ A REAL TARGET DOCUMENT WITH THE KEY REMOVED, THROUGH THE REAL LOADER.
// Both shipped targets declare `defaultAssemblyLanguage`, so this mode needs a
// target document that does not. It is DERIVED from the shipped `x86_64`
// document (parse → erase the key → rename → write), so every other required
// field is whatever the real schema demands rather than whatever a hand-typed
// stub guessed — and it is reached through `TargetSchema::loadShipped` via the
// documented `DSS_CONFIG_ROOT` override, not through an injected loader.
TEST(WorkspaceProject, TargetWithoutAssemblyLanguageIsItsOwnFailure) {
    ScratchDir configRoot{Location::Temp, "lsp-cfg-noasm"};
    const auto targetsDir = configRoot.path() / "src" / "dss-config" / "targets";
    fs::create_directories(targetsDir);

    constexpr char const* kName = "x86-64-lsp-noasm";
    std::ifstream in{dss::test::configRoot() / "targets" / "x86_64.target.json",
                     std::ios::binary};
    ASSERT_TRUE(in) << "shipped x86_64.target.json must be readable";
    json doc = json::parse(in);
    ASSERT_TRUE(doc.contains("defaultAssemblyLanguage"))
        << "the fixture derives its meaning from REMOVING this key; if the "
           "shipped document no longer has it, this test is measuring nothing";
    doc.erase("defaultAssemblyLanguage");
    doc.erase("$defaultAssemblyLanguageComment");
    doc["target"]["name"] = kName;
    writeFile(targetsDir, std::string{kName} + ".target.json", doc.dump(2));

    ScratchDir ws{Location::Temp, "lsp-ws-noasm"};
    writeFile(ws.path(), "app.dss-project.json",
              manifest(std::string{R"([")"} + kName
                       + R"(:elf64-x86_64-linux-exec"])"));

    // Env override first, cwd-walk second: a set-but-MISS falls through, so
    // pointing at this scratch tree adds the no-asm target without hiding any
    // other shipped config.
    ScopedEnv env{"DSS_CONFIG_ROOT", configRoot.path().string()};
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_FALSE(pref.has_value())
        << "a target that declares no assembly dialect cannot break a dialect "
           "tie, and narrowing the preference by skipping it would break the "
           "tie by accident";
    EXPECT_EQ(pref.error().kind,
              WorkspaceProjectErrorKind::TargetDeclaresNoAssemblyLanguage);
    EXPECT_NE(pref.error().detail.find("defaultAssemblyLanguage"),
              std::string::npos)
        << "the message must name the key to add; got: " << pref.error().detail;
    EXPECT_NE(pref.error().detail.find(kName), std::string::npos)
        << "the message must name the target that is silent; got: "
        << pref.error().detail;
}

// ── The composed message an editor actually sees ────────────────────────────

// Neither half is actionable alone: the resolver knows the claimants, the
// workspace knows why no preference exists. The editor gets both.
TEST(WorkspaceProject, UnresolvedSchemaMessageJoinsResolverAndWorkspaceReasons) {
    ScratchDir ws{Location::Temp, "lsp-ws-compose"};
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);   // no manifest
    ASSERT_FALSE(pref.has_value());

    SchemaCache cache;
    auto r = cache.resolveByExtension(".s");
    ASSERT_FALSE(r.has_value());

    const auto msg = describeUnresolvedSchema(".s", r.error(), pref);
    EXPECT_NE(msg.find("asm-x86_64-att"), std::string::npos) << msg;
    EXPECT_NE(msg.find("asm-arm64-gas"), std::string::npos) << msg;
    EXPECT_NE(msg.find("ProjectFileNotFound"), std::string::npos)
        << "the WORKSPACE half must be there — 'ambiguous' alone sends the "
           "operator to the wrong file; got: " << msg;
}

// The workspace half is attached ONLY to an ambiguity. A missing language is
// not a project-file problem, and saying so would send the operator to edit a
// manifest that cannot help.
TEST(WorkspaceProject, UnresolvedSchemaMessageOmitsWorkspaceForNonAmbiguity) {
    ScratchDir ws{Location::Temp, "lsp-ws-compose2"};
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);
    ASSERT_FALSE(pref.has_value());

    SchemaCache cache;
    auto r = cache.resolveByExtension(".no-such-ext-anywhere");
    ASSERT_FALSE(r.has_value());
    ASSERT_EQ(r.error().kind, SchemaResolveErrorKind::NoExtensionMatch);

    const auto msg = describeUnresolvedSchema(".no-such-ext-anywhere",
                                              r.error(), pref);
    EXPECT_EQ(msg.find("ProjectFileNotFound"), std::string::npos)
        << "a missing language is not a manifest problem; got: " << msg;
}

// ── END TO END, OVER THE WIRE ───────────────────────────────────────────────
//
// The unit pins above prove the resolution. These prove it is WIRED: that
// `initialize` carries the workspace in, and that a document which still has no
// language SAYS SO on the wire instead of publishing an empty array that an
// editor renders as "this file is clean".

namespace {

using dss::lsp::testing::fileUriFromPath;
using dss::lsp::testing::LspTestHarness;
using dss::lsp::testing::lspExit;
using dss::lsp::testing::lspInitialize;
using dss::lsp::testing::lspInitializeWithRoots;
using dss::lsp::testing::lspShutdown;

[[nodiscard]] std::string didOpen(fs::path const& file, std::string_view text) {
    json params;
    params["textDocument"]["uri"]        = fileUriFromPath(file);
    params["textDocument"]["languageId"] = "asm";
    params["textDocument"]["version"]    = 1;
    params["textDocument"]["text"]       = text;
    json n;
    n["jsonrpc"] = "2.0";
    n["method"]  = "textDocument/didOpen";
    n["params"]  = std::move(params);
    return n.dump();
}

[[nodiscard]] std::string didChange(fs::path const& file, std::string_view text) {
    json params;
    params["textDocument"]["uri"]     = fileUriFromPath(file);
    params["textDocument"]["version"] = 2;
    params["contentChanges"]          = json::array({json{{"text", text}}});
    json n;
    n["jsonrpc"] = "2.0";
    n["method"]  = "textDocument/didChange";
    n["params"]  = std::move(params);
    return n.dump();
}

// Every `D_UnknownFileExtension` diagnostic message across all published
// notifications. Matching on the CODE (not on prose) is what lets the
// red-on-disable mutation below be detected by the same matcher the pin uses.
[[nodiscard]] std::vector<std::string> schemaErrorMessages(
    std::vector<std::string> const& wireMessages) {
    std::vector<std::string> out;
    for (auto const& raw : wireMessages) {
        auto msg = json::parse(raw);
        auto m = msg.find("method");
        if (m == msg.end() || *m != "textDocument/publishDiagnostics") continue;
        for (auto const& d : msg.at("params").at("diagnostics")) {
            if (d.at("code") == "D_UnknownFileExtension") {
                out.push_back(d.at("message").get<std::string>());
            }
        }
    }
    return out;
}

} // namespace

TEST(WorkspaceProjectE2E, ArmWorkspaceServesADotSFileWithNoSchemaComplaint) {
    ScratchDir ws{Location::Temp, "lsp-e2e-arm"};
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["arm64:elf64-aarch64-linux-exec"])"));
    const auto file = ws.path() / "boot.s";

    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "nop\n"));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto errs = schemaErrorMessages(h.takeServerMessages());
    EXPECT_TRUE(errs.empty())
        << "the workspace names arm64, so `.s` resolves and there is nothing "
           "to complain about; got: " << (errs.empty() ? std::string{} : errs[0]);
}

// ★★ THE SILENT SCHEMA-LESS OPEN, MADE LOUD. Before this cycle `handleDidOpen_`
// dropped the resolve error on the floor and the parse worker published an
// EMPTY diagnostics array — on the wire, indistinguishable from a clean file.
// The two states an editor most needs to tell apart looked identical.
TEST(WorkspaceProjectE2E, WorkspaceWithNoProjectFileSaysWhyItCannotServeDotS) {
    ScratchDir ws{Location::Temp, "lsp-e2e-bare"};
    const auto file = ws.path() / "boot.s";

    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "nop\n"));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto errs = schemaErrorMessages(h.takeServerMessages());
    ASSERT_EQ(errs.size(), 1u)
        << "a document opened with no schema must publish exactly one reason, "
           "not an empty array";
    EXPECT_NE(errs[0].find("asm-x86_64-att"), std::string::npos) << errs[0];
    EXPECT_NE(errs[0].find("asm-arm64-gas"), std::string::npos) << errs[0];
    EXPECT_NE(errs[0].find("ProjectFileNotFound"), std::string::npos)
        << "the message must say what to DO — add a project file; got: "
        << errs[0];
}

// A client that opens no folder at all still gets a reason. This is the
// accepted cost of resolving the dialect from a manifest, stated to the user
// rather than buried in a design note.
TEST(WorkspaceProjectE2E, FilelessWorkspaceGetsADiagnosticNotSilence) {
    ScratchDir ws{Location::Temp, "lsp-e2e-noroot"};
    const auto file = ws.path() / "boot.s";

    LspTestHarness h;
    h.push(lspInitialize(1));            // params {} — no workspace folder
    h.push(didOpen(file, "nop\n"));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto errs = schemaErrorMessages(h.takeServerMessages());
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_NE(errs[0].find("NoWorkspaceRoot"), std::string::npos) << errs[0];
}

// ★ THE REASON MUST SURVIVE AN EDIT. It lives on the DOCUMENT, so every
// republish restates it. A message that appears once and vanishes on the next
// keystroke is barely better than no message — the file is still unserved.
TEST(WorkspaceProjectE2E, TheReasonIsRepublishedOnEveryEdit) {
    ScratchDir ws{Location::Temp, "lsp-e2e-edit"};
    const auto file = ws.path() / "boot.s";

    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "nop\n"));
    h.push(didChange(file, "ret\n"));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto errs = schemaErrorMessages(h.takeServerMessages());
    EXPECT_EQ(errs.size(), 2u)
        << "open + edit ⇒ two publishes, each carrying the reason";
}

// A document whose URI has NO extension has nothing to resolve BY — also a
// reason, also said out loud. This arm never reached the resolver at all, so
// an implementation that only fixed the resolver's failure path would still be
// silent here.
TEST(WorkspaceProjectE2E, ExtensionlessUriGetsADiagnosticNotSilence) {
    ScratchDir ws{Location::Temp, "lsp-e2e-noext"};
    const auto file = ws.path() / "Makefile";

    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "all:\n"));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto errs = schemaErrorMessages(h.takeServerMessages());
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_NE(errs[0].find("no file extension"), std::string::npos) << errs[0];
}
