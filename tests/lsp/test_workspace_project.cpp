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

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/project_config.hpp"   // dss::loadProjectConfig — the SHARED
#include "core/types/target_spec.hpp"     // dss::TargetSpec — the ONE
                                           // `<targetName>:<formatName>`
                                           // splitter, now reached DOWNWARD
                                           // parser the LSP must be running
#include "lsp/schema_cache.hpp"
#include "lsp/workspace_project.hpp"
#include "lsp_test_helpers.hpp"
#include "repo_root.hpp"
// The ONE test-side env override (D-TEST-SCOPED-ENV-DUPLICATED-THREE-WAYS).
// The local copy that used to sit below carried a note claiming the hoist was
// "outside this lane's files" — which was never true; `tests/test_support/**`
// was in the grant the whole time. A stated blocker that is WRONG is worse than
// none, because the next reader trusts it and routes around an open door.
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using dss::lsp::describeUnresolvedSchema;
using dss::lsp::pathFromFileUri;
using dss::lsp::resolveWorkspaceLanguagePreference;
using dss::lsp::SchemaCache;
using dss::lsp::SchemaResolveErrorKind;
using dss::lsp::WorkspaceProjectErrorKind;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
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
  "language": "c",
  "artifactProfile": "cli",
  "targets": )"} + std::string{targetsJsonArray} + R"(,
  "sources": ["main.c"]
})";
}

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
    // Refused on EVERY leg: neither is a `file:` URI at all.
    EXPECT_FALSE(pathFromFileUri("vscode-vfs://github/o/r").has_value());
    EXPECT_FALSE(pathFromFileUri("untitled:Untitled-1").has_value());

    // ★★★ AND THE NAMED-AUTHORITY CASE IS PLATFORM-SPLIT, BECAUSE THE SAME TEXT
    // MEANS TWO DIFFERENT THINGS. [[D-LSP-FILE-URI-WITH-A-UNC-AUTHORITY-DOES-NOT-ROUND-TRIP]]
    //
    // ⚠ THIS CASE USED TO ASSERT A FLAT `EXPECT_FALSE` HERE, and ✔MEASURED
    // 2026-08-28 that put it in DIRECT CONTRADICTION with
    // `FileUriRoundTrip` the moment MSVC ran this suite: MSVC's `fs::path`
    // models `//server/share` as a real `root_name()`, so `fileUriFromPath`
    // emits `file://server/share/x.c` and losslessness REQUIRES the inverse to
    // accept exactly the shape this line refused. Two live assertions, same
    // input, opposite verdicts — and the flat one had never been challenged
    // because no Windows leg could build MSVC.
    //
    // ⇒ Where UNC roots exist the URI NAMES A PATH and must round-trip; where
    // they do not, it names a remote HOST and accepting it would be the guess
    // this case is about. The expectation is derived from the SAME question the
    // implementation asks — `root_name()` on a `//host/share` spelling — and the
    // two arms are named explicitly, exactly as `FileUriRoundTrip` names its
    // three renderings rather than widening to "anything goes".
    const bool uncIsAPathHere =
        !std::filesystem::path{"//dss-unc-probe/share"}.root_name().empty();
    auto const unc = pathFromFileUri("file://someremotehost/share/x");
    if (uncIsAPathHere) {
        ASSERT_TRUE(unc.has_value())
            << "this platform models `//host/share` as a root name, so that URI "
               "names a UNC PATH — refusing it is what breaks the round trip on "
               "our own emitted form";
        EXPECT_EQ(unc->generic_string(), "//someremotehost/share/x")
            << "the authority must come back where it came from, unaltered";
    } else {
        EXPECT_FALSE(unc.has_value())
            << "this platform has no UNC root, so `//someremotehost/share/x` is "
               "not a path spelling — turning the authority into one would be a "
               "guess about a remote host";
    }
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
    EXPECT_EQ((*r)->name(), "C");
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
  "language": "c",
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

// ★★ THE ANTI-DUPLICATION PIN (D-LSP-PROJECT-CONFIG-LIVES-ABOVE-ITS-CONSUMERS).
//
// `project_config.{hpp,cpp}` moved from `src/program/` down to
// `src/core/types/` so the editor and the compiler could share it without the
// LSP reaching UP into the driver tier. The move is only worth anything if the
// LSP is still running THAT parser — the standing temptation, whenever the
// include looks awkward, is to write a ten-line "just read `targets`" reader
// here instead. This pin makes that choice RED, on two independent axes:
//
//   1. BEHAVIOURAL. The fixture is well-formed as far as a `targets`-only
//      reader is concerned — `language`, `artifactProfile`, `targets` and
//      `sources` are all valid and would yield a clean arm64 preference. What
//      it also carries is a `resolveLibraries` entry in the OBJECT form with no
//      `importName`, which the shared loader REFUSES (an object that states no
//      identity is a typo, and a silent drop would lose the very thing the
//      entry exists to carry). A duplicate reader would neither know nor care
//      about that key and would happily return a preference.
//   2. TEXTUAL, and byte-for-byte. The expected message is not typed into this
//      file — it is COMPUTED by running `dss::loadProjectConfig` on the same
//      file and rendering its first diagnostic the way `workspace_project.cpp`
//      does. A second parser would have to reproduce the shared one's code AND
//      its prose exactly to stay green, which is indistinguishable from just
//      calling it.
TEST(WorkspaceProject, LspAndDriverParseAManifestThroughTheSameLoader) {
    ScratchDir ws{Location::Temp, "lsp-ws-sameparser"};
    // Valid to a `targets`-only reader; rejected by the shared loader.
    writeFile(ws.path(), "app.dss-project.json", R"({
  "language": "c",
  "artifactProfile": "cli",
  "targets": ["arm64:elf64-aarch64-linux-exec"],
  "sources": ["main.c"],
  "resolveLibraries": [{"path": "libfoo.so"}]
})");
    const auto manifestPath = ws.path() / "app.dss-project.json";

    // ── What the DRIVER's parser says about this exact file, measured now ──
    dss::DiagnosticReporter rep;
    auto cfg = dss::loadProjectConfig(manifestPath, rep);
    ASSERT_FALSE(cfg.has_value())
        << "fixture is stale: the shared loader must REFUSE an object-form "
           "`resolveLibraries` entry with no `importName`";
    ASSERT_FALSE(rep.all().empty())
        << "the shared loader must report a reason, not fail mutely";
    const std::string driverSays =
        std::string{dss::diagnosticCodeName(rep.all()[0].code)} + ": "
        + rep.all()[0].actual;

    // ── What the LSP says about it ──
    const std::array<fs::path, 1> roots{ws.path()};
    auto pref = resolveWorkspaceLanguagePreference(roots);

    ASSERT_FALSE(pref.has_value())
        << "axis 1 (behaviour): a `targets`-only re-implementation would have "
           "returned a preference here. The LSP must inherit the shared "
           "loader's refusal, key vocabulary and all";
    EXPECT_EQ(pref.error().kind,
              WorkspaceProjectErrorKind::ProjectFileLoadFailed);
    EXPECT_NE(pref.error().detail.find(driverSays), std::string::npos)
        << "axis 2 (text): the LSP's reason must CONTAIN the driver's own "
           "rendered diagnostic byte-for-byte.\n  driver: " << driverSays
        << "\n  lsp:    " << pref.error().detail;
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

[[nodiscard]] std::string didSave(fs::path const& file) {
    json params;
    params["textDocument"]["uri"] = fileUriFromPath(file);
    json n;
    n["jsonrpc"] = "2.0";
    n["method"]  = "textDocument/didSave";
    n["params"]  = std::move(params);
    return n.dump();
}

// A `workspace/did*` notification. Params are built spec-shaped even though the
// server ignores them: a fixture that sent `{}` would pass against a handler
// that crashed on real client traffic.
[[nodiscard]] std::string workspaceFileNotification(std::string_view method,
                                                    fs::path const& file) {
    json n;
    n["jsonrpc"] = "2.0";
    n["method"]  = std::string{method};
    if (method == "workspace/didChangeWatchedFiles") {
        // FileEvent.type: 1 = Created, 2 = Changed, 3 = Deleted (LSP §3.17).
        n["params"]["changes"] = json::array(
            {json{{"uri", fileUriFromPath(file)}, {"type", 1}}});
    } else {
        n["params"]["files"] = json::array(
            {json{{"uri", fileUriFromPath(file)}}});
    }
    return n.dump();
}

// One entry per `textDocument/publishDiagnostics` FOR `uri`, in wire order:
// the `D_UnknownFileExtension` message that publish carried, or "" when it
// carried none. Publish COUNT is as load-bearing as publish CONTENT here — the
// claim under test is that an already-open document is RE-published, and a
// matcher that only unioned messages could not tell "republished clean" from
// "never republished".
[[nodiscard]] std::vector<std::string> schemaErrorPerPublish(
    std::vector<std::string> const& wireMessages, std::string const& uri) {
    std::vector<std::string> out;
    for (auto const& raw : wireMessages) {
        auto msg = json::parse(raw);
        auto m = msg.find("method");
        if (m == msg.end() || *m != "textDocument/publishDiagnostics") continue;
        if (msg.at("params").at("uri") != uri) continue;
        std::string found;
        for (auto const& d : msg.at("params").at("diagnostics")) {
            if (d.at("code") == "D_UnknownFileExtension") {
                found = d.at("message").get<std::string>();
            }
        }
        out.push_back(std::move(found));
    }
    return out;
}

[[nodiscard]] std::size_t countPublishes(
    std::vector<std::string> const& wireMessages, std::string const& uri) {
    return schemaErrorPerPublish(wireMessages, uri).size();
}

// Drain the server's outbound traffic into `sink` until it has published
// `want` diagnostics notifications for `uri`. FAILS the test on timeout.
//
// ⚠ THE HARNESS IS ASYNCHRONOUS AND A MID-SESSION TEST MUST SYNCHRONISE.
// `push` only enqueues; the server thread consumes at its own pace, and
// `initialize` alone does a cold shipped-config scan. A test that pushed
// `didOpen` and then immediately wrote a manifest would be RACING the server
// for the open's resolution — and losing that race silently converts "the
// manifest arrived mid-session" into "the manifest was there all along", which
// is precisely the scenario under test. Waiting for the open's publish is what
// makes the fixture state a fact instead of a hope.
void awaitPublishes(LspTestHarness& h, std::string const& uri,
                    std::size_t want, std::vector<std::string>& sink) {
    const auto deadline =
        std::chrono::steady_clock::now() + dss::test_support::kWaitBudget;
    while (countPublishes(sink, uri) < want) {
        for (auto& m : h.takeServerMessages()) sink.push_back(std::move(m));
        if (countPublishes(sink, uri) >= want) return;
        ASSERT_LT(std::chrono::steady_clock::now(), deadline)
            << "timed out waiting for publish #" << want << " of `" << uri
            << "`; saw " << countPublishes(sink, uri);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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

// ── ★★ LIVENESS: THE PREFERENCE IS NO LONGER FROZEN AT `initialize` ─────────
// (D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE)
//
// The preference used to be resolved exactly once, in `handleInitialize_`, so a
// user who added their first `.dss-project.json` mid-session had to RESTART the
// server. Re-reading per `didOpen` was rejected at the time for a good reason —
// it would silently change what an ALREADY-OPEN document means without
// republishing it — and the answer is to do the second half, not to refuse to
// look. Every pin below therefore asserts on the REPUBLISH, not on the internal
// preference: an implementation that updated its own state and left the editor
// showing diagnostics computed under the old grammar would be the original
// defect wearing a fresh coat.
//
// None of this needs a server→client REQUEST. `client/registerCapability` is
// genuinely unavailable (`JsonRpc` exposes no `serializeRequest`), and it is
// genuinely not needed: every trigger below is an INBOUND notification, and the
// one capability that IS advertised rides in the `initialize` RESULT.

TEST(WorkspaceProjectE2E, AddingAManifestMidSessionRepublishesAnOpenDocument) {
    ScratchDir ws{Location::Temp, "lsp-live-add"};
    const auto file = ws.path() / "boot.s";
    const auto uri  = fileUriFromPath(file);

    std::vector<std::string> wire;
    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "nop\n"));          // no manifest yet: `.s` ambiguous
    ASSERT_NO_FATAL_FAILURE(awaitPublishes(h, uri, 1, wire));

    // The user creates their first project file, then saves the document they
    // were already editing. No restart, no re-open, no client capability.
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["arm64:elf64-aarch64-linux-exec"])"));
    h.push(didSave(file));
    ASSERT_NO_FATAL_FAILURE(awaitPublishes(h, uri, 2, wire));

    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto perPublish = schemaErrorPerPublish(wire, uri);
    ASSERT_EQ(perPublish.size(), 2u)
        << "expected TWO publishes for this URI: the open (unserved) and the "
           "republish once the manifest named a CPU. A size of 1 means the "
           "already-open document was never revisited -- the frozen-preference "
           "defect itself";
    EXPECT_NE(perPublish[0].find("ProjectFileNotFound"), std::string::npos)
        << "the open must still say why it could not serve the file; got: "
        << perPublish[0];
    EXPECT_EQ(perPublish[1], "")
        << "after the manifest named arm64 the document resolves, so the "
           "republish must carry NO schema complaint; got: " << perPublish[1];
}

// The same claim through `workspace/didChangeWatchedFiles`. The server does not
// REGISTER a watcher (that needs `client/registerCapability`), but a client that
// sends one unprompted used to be dropped on the dispatcher's
// unknown-notification path. Handling it costs one method-table row.
TEST(WorkspaceProjectE2E, DidChangeWatchedFilesRepublishesAnOpenDocument) {
    ScratchDir ws{Location::Temp, "lsp-live-watch"};
    const auto file         = ws.path() / "boot.s";
    const auto uri          = fileUriFromPath(file);
    const auto manifestFile = ws.path() / "app.dss-project.json";

    std::vector<std::string> wire;
    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "nop\n"));
    ASSERT_NO_FATAL_FAILURE(awaitPublishes(h, uri, 1, wire));
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["arm64:elf64-aarch64-linux-exec"])"));
    h.push(workspaceFileNotification("workspace/didChangeWatchedFiles",
                                     manifestFile));
    ASSERT_NO_FATAL_FAILURE(awaitPublishes(h, uri, 2, wire));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto perPublish = schemaErrorPerPublish(wire, uri);
    ASSERT_EQ(perPublish.size(), 2u);
    EXPECT_NE(perPublish[0].find("ProjectFileNotFound"), std::string::npos)
        << perPublish[0];
    EXPECT_EQ(perPublish[1], "");
}

// `workspace/didCreateFiles` is the channel the `initialize` result now
// advertises STATICALLY (`capabilities.workspace.fileOperations.didCreate`) --
// the half that proves no outbound request was ever required here.
TEST(WorkspaceProjectE2E, DidCreateFilesRepublishesAnOpenDocument) {
    ScratchDir ws{Location::Temp, "lsp-live-create"};
    const auto file         = ws.path() / "boot.s";
    const auto uri          = fileUriFromPath(file);
    const auto manifestFile = ws.path() / "app.dss-project.json";

    std::vector<std::string> wire;
    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "nop\n"));
    ASSERT_NO_FATAL_FAILURE(awaitPublishes(h, uri, 1, wire));
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["arm64:elf64-aarch64-linux-exec"])"));
    h.push(workspaceFileNotification("workspace/didCreateFiles", manifestFile));
    ASSERT_NO_FATAL_FAILURE(awaitPublishes(h, uri, 2, wire));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto perPublish = schemaErrorPerPublish(wire, uri);
    ASSERT_EQ(perPublish.size(), 2u);
    EXPECT_NE(perPublish[0].find("ProjectFileNotFound"), std::string::npos)
        << perPublish[0];
    EXPECT_EQ(perPublish[1], "");
}

// ★ THE ADVERTISEMENT ITSELF. A handler for a notification no client was ever
// told to send would be a guard that can never fire. The `initialize` RESULT is
// where this one is asked for -- a response, not a server-originated request.
TEST(WorkspaceProjectE2E, InitializeAdvertisesStaticManifestFileOperations) {
    ScratchDir ws{Location::Temp, "lsp-live-caps"};

    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    json caps;
    for (auto const& raw : h.takeServerMessages()) {
        auto msg = json::parse(raw);
        if (msg.contains("result") && msg["result"].contains("capabilities")) {
            caps = msg["result"]["capabilities"];
            break;
        }
    }
    ASSERT_FALSE(caps.is_null()) << "no initialize result was published";
    auto const& ops = caps.at("workspace").at("fileOperations");
    for (char const* key : {"didCreate", "didRename", "didDelete"}) {
        ASSERT_TRUE(ops.contains(key)) << key;
        auto const& filters = ops.at(key).at("filters");
        ASSERT_EQ(filters.size(), 1u) << key;
        EXPECT_EQ(filters[0].at("scheme"), "file") << key;
        EXPECT_EQ(filters[0].at("pattern").at("matches"), "file") << key;
        // Derived from `kProjectFileSuffix`, never re-spelled -- asserted
        // against the constant so renaming the manifest suffix cannot leave a
        // stale glob advertised to every client in the world.
        EXPECT_EQ(filters[0].at("pattern").at("glob"),
                  std::string{"**/*"}
                      + std::string{dss::lsp::kProjectFileSuffix}) << key;
    }
    // `will*` are REQUESTS the client blocks on, expecting a WorkspaceEdit back.
    // We have no edit to contribute, so claiming them would stall the editor.
    for (char const* key : {"willCreate", "willRename", "willDelete"}) {
        EXPECT_FALSE(ops.contains(key))
            << key << " must not be advertised -- it is a request we cannot "
                      "answer";
    }
}

// ★★ LIVENESS RUNS BOTH WAYS. A refresh that only ever IMPROVES an answer is
// half a guard: removing the manifest must take the language service away
// again, loudly, rather than leaving the editor parsing `.s` under a dialect
// the workspace no longer declares.
TEST(WorkspaceProjectE2E, RemovingTheManifestRepublishesTheReason) {
    ScratchDir ws{Location::Temp, "lsp-live-remove"};
    const auto file = ws.path() / "boot.s";
    const auto uri  = fileUriFromPath(file);
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["arm64:elf64-aarch64-linux-exec"])"));

    std::vector<std::string> wire;
    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "nop\n"));          // resolves cleanly
    ASSERT_NO_FATAL_FAILURE(awaitPublishes(h, uri, 1, wire));

    std::error_code ec;
    fs::remove(ws.path() / "app.dss-project.json", ec);
    ASSERT_FALSE(ec) << "fixture teardown failed: " << ec.message();
    h.push(didSave(file));
    ASSERT_NO_FATAL_FAILURE(awaitPublishes(h, uri, 2, wire));

    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto perPublish = schemaErrorPerPublish(wire, uri);
    ASSERT_EQ(perPublish.size(), 2u);
    EXPECT_EQ(perPublish[0], "")
        << "the open happened while the manifest existed; got: " << perPublish[0];
    EXPECT_NE(perPublish[1].find("ProjectFileNotFound"), std::string::npos)
        << "with the manifest gone the document must be told so; got: "
        << perPublish[1];
}

// ★ NO CHURN. `setSchema` reports "changed" only when the (schema, reason) pair
// actually moved, so a save that changed nothing about the manifest set costs
// ZERO republishes. Without that check every save would re-publish every open
// document -- a different bug wearing this feature's clothes.
TEST(WorkspaceProjectE2E, ASaveThatChangesNoManifestRepublishesNothing) {
    ScratchDir ws{Location::Temp, "lsp-live-nochurn"};
    const auto file = ws.path() / "boot.s";
    const auto uri  = fileUriFromPath(file);
    writeFile(ws.path(), "app.dss-project.json",
              manifest(R"(["arm64:elf64-aarch64-linux-exec"])"));

    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {ws.path()}));
    h.push(didOpen(file, "nop\n"));
    h.push(didSave(file));                   // nothing on disk moved
    h.push(didSave(file));
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto perPublish = schemaErrorPerPublish(h.takeServerMessages(), uri);
    EXPECT_EQ(perPublish.size(), 1u)
        << "the open publishes; two saves that changed no manifest must "
           "publish nothing further";
}

// ★ THE PRE-`initialize` REASON SURVIVES. The placeholder preference says "the
// client has not sent `initialize` yet", which is a DIFFERENT claim from "the
// client named no workspace folder". A refresh that ran before `initialize`
// would overwrite the first with the second and quietly lose the distinction,
// so the refresh refuses to run until `initialize` has been handled.
TEST(WorkspaceProjectE2E, ADocumentOpenedBeforeInitializeSaysExactlyThat) {
    ScratchDir ws{Location::Temp, "lsp-live-preinit"};
    const auto file = ws.path() / "boot.s";

    LspTestHarness h;
    h.push(didOpen(file, "nop\n"));          // no `initialize` at all
    h.push(didSave(file));                   // would trigger a refresh
    h.push(lspShutdown(1));
    h.push(std::string{lspExit});
    ASSERT_EQ(h.runUntilExit(), 0);

    auto errs = schemaErrorMessages(h.takeServerMessages());
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_NE(errs[0].find("has not sent `initialize` yet"), std::string::npos)
        << "the pre-initialize reason must not be replaced by the "
           "no-workspace-folder one; got: " << errs[0];
}

// ══ D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES ════════
//
// `fileUriFromPath` is the EXACT INVERSE of `pathFromFileUri`, and it did not
// exist in `src/` at all until this row -- only `tests/lsp/lsp_test_helpers.hpp`
// had a copy. That is why production could name no file but the one the request
// arrived on, and a definition inside a header was reported as living in the
// open document.
//
// ★ THE TEST-TREE COPY ALSO DID LESS THAN IT CLAIMED: it did no
// percent-encoding whatever, while its comment asserted that fixtures built
// with it "round-trip through `dss::lsp::pathFromFileUri`". A path containing a
// space decoded back to a DIFFERENT path, so the round-trip it advertised was
// never closed. The helper now forwards here, and this pins the three shapes
// that actually have to survive the trip.
TEST(WorkspaceProject, FileUriRoundTrip) {
    // (1) Plain POSIX-shaped absolute path.
    {
        const std::filesystem::path p{"/home/user/src/main.c"};
        const std::string uri = dss::lsp::fileUriFromPath(p);
        EXPECT_EQ(uri, "file:///home/user/src/main.c");
        auto back = dss::lsp::pathFromFileUri(uri);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->generic_string(), p.generic_string());
    }
    // (2) WINDOWS DRIVE LETTER. `C:/dir/x.c` is `file:///C:/dir/x.c`: the URI
    // grammar's leading slash is added by the encoder and stripped by the
    // decoder, and `:` is deliberately NOT encoded -- that is what every LSP
    // client emits, so encoding it would produce a uri no editor matches.
    {
        const std::filesystem::path p{"C:/dev/proj/main.c"};
        const std::string uri = dss::lsp::fileUriFromPath(p);
        EXPECT_EQ(uri, "file:///C:/dev/proj/main.c");
        auto back = dss::lsp::pathFromFileUri(uri);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->generic_string(), p.generic_string());
    }
    // (3) PERCENT-ENCODING. The character the old test-tree copy silently got
    // wrong: a space must encode, or the uri terminates early in a client.
    {
        const std::filesystem::path p{"C:/dev/my proj/a b.c"};
        const std::string uri = dss::lsp::fileUriFromPath(p);
        EXPECT_EQ(uri, "file:///C:/dev/my%20proj/a%20b.c");
        auto back = dss::lsp::pathFromFileUri(uri);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->generic_string(), p.generic_string())
            << "a space must survive the round trip; the pre-row helper did no "
               "encoding at all and this decoded to a different path";
    }
    // A `#` would otherwise be read as a fragment delimiter.
    {
        const std::filesystem::path p{"/tmp/a#b.c"};
        const std::string uri = dss::lsp::fileUriFromPath(p);
        EXPECT_EQ(uri, "file:///tmp/a%23b.c");
        auto back = dss::lsp::pathFromFileUri(uri);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->generic_string(), p.generic_string());
    }
    // (4) A UNC-SHAPED PATH. ⚠ THE INVARIANT IS THE ROUND TRIP; THE EXACT URI
    // TEXT IS PLATFORM-DEFINED, AND PINNING ONE PLATFORM'S TEXT WAS A DEFECT.
    // ✔MEASURED on TWO legs, and they disagree for a good reason:
    //   * MinGW/GCC on Windows — `fs::path{"//server/share/x.c"}` reports an
    //     EMPTY `root_name()` and renders with a SINGLE leading slash: the path
    //     type discards the UNC authority at construction, so this encodes as an
    //     ordinary absolute path → `file:///server/share/x.c`.
    //   * libc++ on Darwin — POSIX makes a leading `//` IMPLEMENTATION-DEFINED
    //     and libc++ PRESERVES it, because `//server/share` there is a LOCAL
    //     path, not a network authority. `root_name()` is still empty (POSIX has
    //     none), so the generic string keeps both slashes →
    //     `file:////server/share/x.c`, an empty authority plus that path.
    // ★ BOTH ARE CORRECT FOR THEIR PLATFORM, and treating Darwin's `server` as
    // a URI authority would be the actual bug — it is a directory there.
    // ⚠ THIS ASSERTION USED TO HARDCODE THE FIRST FORM, and the macOS leg is
    // what caught it: the lane that wrote it said, accurately, that it pinned
    // "what THIS leg actually does rather than a portability claim it cannot
    // keep" — but "this leg" is FOUR legs. The two known-correct renderings are
    // named explicitly rather than derived from `p`, because deriving the
    // expectation would make this test re-implement the function it tests and
    // assert nothing; a THIRD rendering still fails loudly here.
    {
        const std::filesystem::path p{"//server/share/x.c"};
        const std::string uri = dss::lsp::fileUriFromPath(p);
        //   * MSVC STL on Windows — models `//server/share` as a REAL
        //     `root_name()`, so the authority survives and this renders as
        //     `file://server/share/x.c`, the RFC 8089 spelling of a UNC share.
        //     ✔MEASURED 2026-08-28 (cycle P43) on the first MSVC run of this
        //     suite. ★ THIS IS THE "third arm" THE PARAGRAPH ABOVE PREDICTED,
        //     added as one rather than by widening the check — and the round
        //     trip below is what caught that `pathFromFileUri` refused our own
        //     output, [[D-LSP-FILE-URI-WITH-A-UNC-AUTHORITY-DOES-NOT-ROUND-TRIP]].
        const bool knownForm = uri == "file:///server/share/x.c"
                            || uri == "file:////server/share/x.c"
                            || uri == "file://server/share/x.c";
        EXPECT_TRUE(knownForm)
            << "unexpected UNC rendering '" << uri << "' — neither the "
               "authority-discarding form nor the POSIX `//`-preserving one. "
               "If a platform models a real UNC root this needs a third arm, "
               "NOT a widened check";
        auto back = dss::lsp::pathFromFileUri(uri);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(back->generic_string(), p.generic_string())
            << "whatever the platform makes of a UNC spelling, the trip must "
               "be lossless in the form the path type actually holds — THIS is "
               "the property, and it holds on every leg";
    }
    // The inbound REFUSAL of a real remote authority is the property that
    // matters for safety, and it is pinned separately by
    // `NonFileUriIsRefusedRatherThanGuessed`: a network host is not a local
    // directory to scan, so `file://someremotehost/share/x` yields nullopt.
}


// ── D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS ────────────────────
//
// ★★ THE LAYER IS THE SUBJECT, SO THE PIN HAS TO BE ABLE TO SEE A LAYER. A
// behaviour test cannot: `TargetSpec::parse` returns the same answer from
// whichever tier it is declared in, so a test that only calls it stays green
// through the exact regression this row closed. What CAN go wrong is an
// `#include` — the LSP reaching UP into the driver tier for a splitter — and
// that is a fact about the SOURCE, so the source is what gets read.
//
// ⚠ TWO HALVES, AND NEITHER IS SUFFICIENT ALONE. The scan below reds if any
// `src/lsp/**` file re-acquires a `#include "program/..."`. The `#include
// "core/types/target_spec.hpp"` at the top of THIS file reds at COMPILE time if
// the type ever leaves `core` — which a text scan of `src/lsp/` could never
// see, because a type that moved back up would take the LSP's include with it
// and the scan would keep passing while the layering was gone.
//
// ★ WHY THE LIST IS ENUMERATED FROM THE DIRECTORY rather than hand-kept: a
// hand-kept list silently stops covering the file someone adds next week, which
// is precisely when a fresh cross-tier include appears.
TEST(WorkspaceProject, TheLspNeverReachesUpIntoTheDriverTier) {
    namespace fs = std::filesystem;
    fs::path const lspDir = fs::path{DSS_TEST_REPO_ROOT} / "src" / "lsp";
    ASSERT_TRUE(fs::is_directory(lspDir))
        << "cannot find src/lsp under DSS_TEST_REPO_ROOT — the scan below "
           "would pass by having nothing to read";

    std::size_t scanned = 0;
    for (auto const& entry : fs::directory_iterator{lspDir}) {
        if (!entry.is_regular_file()) continue;
        auto const ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        ++scanned;
        std::ifstream in{entry.path(), std::ios::binary};
        ASSERT_TRUE(in.good()) << "could not read " << entry.path().string();
        std::string line;
        std::size_t lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            // The INCLUDE DIRECTIVE only — a `program/...` mention inside a
            // comment is documentation (this repo's comments name the tier
            // they deliberately do NOT depend on, and must stay able to).
            auto const hash = line.find('#');
            if (hash == std::string::npos) continue;
            if (line.find("include") == std::string::npos) continue;
            if (line.find("\"program/") == std::string::npos) continue;
            if (line.substr(0, hash).find_first_not_of(" \t") != std::string::npos) {
                continue;   // not a directive: `#` appeared after other code
            }
            FAIL() << entry.path().filename().string() << ":" << lineNo
                   << " includes the DRIVER tier: " << line
                   << "\n`lsp` has no link edge to `program` and must not grow "
                      "one — `program` already links `lsp` for the `--lsp` mode "
                      "dispatch, so the reverse edge closes a cycle CMake "
                      "refuses between OBJECT libraries. Move the shared fact "
                      "DOWN into `core`, as `project_config` and `target_spec` "
                      "both were "
                      "(D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS).";
        }
    }
    EXPECT_GT(scanned, 5U)
        << "the scan read almost nothing, so it cannot have proved anything — "
           "check the directory walk before trusting a green here";
}

// The splitter still splits, from its new home. Cheap, and it is what stops the
// move from being a rename that lost a behaviour on the way down.
TEST(WorkspaceProject, TheSplitterStillSplitsFromCore) {
    auto const ok = dss::TargetSpec::parse("x86_64:elf64-x86_64-linux-exec");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->targetName, "x86_64");
    EXPECT_EQ(ok->formatName, "elf64-x86_64-linux-exec");

    // The four remediation-distinct refusals travelled with it. A move that
    // dropped `TargetSpecError` would leave the LSP unable to tell an operator
    // WHICH way their manifest's target line is wrong.
    EXPECT_EQ(dss::TargetSpec::parse("x86_64").error(),
              dss::TargetSpecError::MissingColon);
    EXPECT_EQ(dss::TargetSpec::parse("a:b:c").error(),
              dss::TargetSpecError::MultipleColons);
    EXPECT_EQ(dss::TargetSpec::parse(":fmt").error(),
              dss::TargetSpecError::EmptyTargetName);
    EXPECT_EQ(dss::TargetSpec::parse("tgt:").error(),
              dss::TargetSpecError::EmptyFormatName);
    EXPECT_EQ(dss::TargetSpec::parse("tg t:fmt").error(),
              dss::TargetSpecError::WhitespaceInName);
}
