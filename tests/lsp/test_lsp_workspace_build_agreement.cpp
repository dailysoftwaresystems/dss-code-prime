// ★★★ THE EDITOR ANSWERS WITH THE BUILD'S CONFIGURATION —
// [[D-LSP-HEADER-CASE-RULE-NOT-WORKSPACE-AWARE]].
//
// Every case here is ONE workspace, ONE set of project manifests and ONE file,
// asked twice: of the BUILD (`Program::compileProject` on the workspace's own
// manifest) and of the EDITOR (a real `LspServer`, initialized with the
// workspace as its root, `didOpen` on the same file). The assertion is that the
// two AGREE — the same verdict, and the same reason — never merely that the
// editor produced something.
//
// ── WHAT WAS BROKEN, ✔MEASURED 2026-09-19 WITH A REAL `dsscp --lsp` ────────────
// The editor built every document with NO `<target>:<format>` pair: no active
// format, the POSIX header-name case rule, no target or format predefines, no
// plain-`char` sign, and an `analyze()` with every default (LP64, no aggregate
// layout, no `long double`, no availability gate). Against the build of the
// SAME manifest:
//   * pe64: `#include <Windows.h>` — build rc=0, editor F_ShippedHeaderNotFound;
//   * pe64: `#include <pthread.h>` — build F_ShippedHeaderUnavailableForTarget,
//     editor CLEAN (the silent wrong accept);
//   * elf:  `#include <direct.h>` — the same, mirrored;
//   * pe64: `#include <windows.h>` — build rc=0, editor two
//     F_ShippedTypeIdentityConflict (`long` analyzed as 64-bit);
//   * every `long double`, and every `_Static_assert(sizeof(…))`, refused by the
//     editor only.
// The row's own closing test is the first two lines below: `<Windows.h>` on a
// pe64 workspace and `<Stdio.h>` on an elf one — the second is the silent-
// wrong-accept direction, which a "just fold case everywhere" fix would break.
//
// ★ THE BUILD ARM MUST HAVE COMPILED, and the refusing arm must refuse for the
// NAMED reason: a manifest the driver could not even load returns non-zero too,
// and would make a refusing case pass for the wrong reason. Every accepting case
// asserts rc==0 AND an artifact; every refusing case asserts the specific code.
//
// Every case builds x86_64 pairs only and EXECUTES nothing, so the file runs on
// every leg (a pe64 or elf artifact is written, never run).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "lsp_test_helpers.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using dss::lsp::testing::LspTestHarness;
using dss::lsp::testing::fileUriFromPath;
using dss::lsp::testing::lspExit;
using dss::lsp::testing::lspInitializeWithRoots;
using dss::lsp::testing::lspShutdown;
using json = nlohmann::json;

namespace {

constexpr std::string_view kPe64 = "x86_64:pe64-x86_64-windows-exec";
constexpr std::string_view kElf  = "x86_64:elf64-x86_64-linux-exec";

void writeFile(fs::path const& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// One manifest, written with ABSOLUTE paths: a root manifest's relative entries
// resolve against the process working directory in the build, and the tests must
// not depend on where ctest runs them.
struct Manifest {
    std::string              name;       // file name inside the workspace root
    std::vector<std::string> targets;
    std::vector<fs::path>    sources;
    std::vector<fs::path>    includes;
    std::vector<std::string> defines;
};

[[nodiscard]] fs::path writeManifest(fs::path const& root, Manifest const& m) {
    json doc;
    doc["language"]        = "c";
    doc["artifactProfile"] = "cli";
    doc["targets"]         = m.targets;
    json sources = json::array();
    for (auto const& s : m.sources) sources.push_back(s.generic_string());
    doc["sources"] = sources;
    if (!m.includes.empty()) {
        json inc = json::array();
        for (auto const& i : m.includes) inc.push_back(i.generic_string());
        doc["includes"] = inc;
    }
    if (!m.defines.empty()) doc["defines"] = m.defines;
    auto const path = root / m.name;
    writeFile(path, doc.dump(2));
    return path;
}

// The BUILD's verdict on one manifest.
struct BuildVerdict {
    int                     rc = -1;
    dss::DiagnosticReporter rep;
    bool                    wroteArtifact = false;
};

[[nodiscard]] BuildVerdict build(fs::path const& manifest, fs::path const& outDir) {
    BuildVerdict v;
    dss::Program p;
    p.setOutputDir(outDir);
    v.rc = p.compileProject(manifest.string(), v.rep);
    std::error_code ec;
    for (auto const& a : p.artifactPaths()) {
        if (a.has_value() && fs::exists(*a, ec)) v.wroteArtifact = true;
    }
    return v;
}

struct Published {
    std::string code;
    std::string message;
};

// The EDITOR's verdict on each of `files`: a real `LspServer` initialized with
// `root` as its workspace folder, one `didOpen` per file, and the LAST publish
// per document (an earlier one can predate the parse job).
[[nodiscard]] std::map<fs::path, std::vector<Published>>
editor(fs::path const& root, std::vector<fs::path> const& files) {
    LspTestHarness h;
    h.push(lspInitializeWithRoots(1, {root}));
    std::map<std::string, fs::path> byUri;
    for (auto const& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::string const text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        std::string const uri = fileUriFromPath(f);
        byUri[uri] = f;
        json didOpen = {
            {"jsonrpc", "2.0"},
            {"method", "textDocument/didOpen"},
            {"params", {{"textDocument",
                         {{"uri", uri}, {"languageId", "c"},
                          {"version", 1}, {"text", text}}}}}};
        h.push(didOpen.dump());
    }
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    std::map<fs::path, std::vector<Published>> out;
    for (auto const& raw : h.takeServerMessages()) {
        auto const m = json::parse(raw, nullptr, false);
        if (m.is_discarded() || !m.contains("method")) continue;
        if (m.at("method") != "textDocument/publishDiagnostics") continue;
        auto const& params = m.at("params");
        auto const it = byUri.find(params.at("uri").get<std::string>());
        if (it == byUri.end()) continue;
        auto& list = out[it->second];
        list.clear();
        for (auto const& d : params.at("diagnostics")) {
            list.push_back(Published{d.value("code", std::string{}),
                                     d.value("message", std::string{})});
        }
    }
    return out;
}

[[nodiscard]] bool hasCode(std::vector<Published> const& v, std::string_view code) {
    return std::any_of(v.begin(), v.end(),
                       [&](Published const& p) { return p.code == code; });
}

[[nodiscard]] std::string render(std::vector<Published> const& v) {
    std::string s;
    for (auto const& p : v) s += "\n  [" + p.code + "] " + p.message;
    return s.empty() ? std::string{" <none>"} : s;
}

// One file, one manifest: the file's text, and the pairs the manifest builds.
struct OneFileWorkspace {
    dss::test_support::ScratchDir scratch;
    fs::path                      source;
    fs::path                      manifest;

    OneFileWorkspace(std::string const& tag, std::string_view text,
                     std::vector<std::string> targets)
        : scratch{dss::test_support::Location::InsideRepo, tag} {
        source = scratch.path() / "main.c";
        writeFile(source, text);
        manifest = writeManifest(scratch.path(),
                                 Manifest{"main.dss-project.json", std::move(targets),
                                          {source}, {}, {}});
    }
};

// The two verdicts on one file under one manifest.
struct Agreement {
    BuildVerdict           built;
    std::vector<Published> edited;
};

[[nodiscard]] Agreement askBoth(OneFileWorkspace const& ws) {
    Agreement a;
    a.built  = build(ws.manifest, ws.scratch.path() / "out");
    a.edited = editor(ws.scratch.path(), {ws.source})[ws.source];
    return a;
}

void expectBothAccept(Agreement const& a, std::string_view what) {
    ASSERT_EQ(a.built.rc, 0) << what << ": the BUILD must accept it";
    ASSERT_TRUE(a.built.wroteArtifact)
        << what << ": rc==0 with no artifact means the build never compiled";
    EXPECT_TRUE(a.edited.empty())
        << what << ": the editor refused what the build accepts:" << render(a.edited);
}

void expectBothRefuse(Agreement const& a, dss::DiagnosticCode code,
                      std::string_view what) {
    ASSERT_NE(a.built.rc, 0) << what << ": the BUILD must refuse it";
    ASSERT_GT(dss::test_support::countCode(a.built.rep, code), 0u)
        << what << ": the build refused for some OTHER reason — the case is void";
    EXPECT_TRUE(hasCode(a.edited, dss::diagnosticCodeName(code)))
        << what << ": the editor did not refuse it for the build's reason ("
        << dss::diagnosticCodeName(code) << "):" << render(a.edited);
}

} // namespace

// ── THE ROW'S CLOSING TEST, BOTH DIRECTIONS ─────────────────────────────────
TEST(LspWorkspaceBuildAgreement, PeWorkspaceAcceptsWindowsHeaderSpelledAnyCase) {
    OneFileWorkspace ws{"lsp-wba-pe-windows",
                        "#include <Windows.h>\nint main(void) { return 0; }\n",
                        {std::string{kPe64}}};
    expectBothAccept(askBoth(ws), "pe64 `#include <Windows.h>`");
}

TEST(LspWorkspaceBuildAgreement, ElfWorkspaceRefusesStdioHeaderSpelledWrongCase) {
    OneFileWorkspace ws{"lsp-wba-elf-stdio",
                        "#include <Stdio.h>\nint main(void) { puts(\"r\"); return 0; }\n",
                        {std::string{kElf}}};
    expectBothRefuse(askBoth(ws), dss::DiagnosticCode::F_ShippedHeaderNotFound,
                     "elf `#include <Stdio.h>`");
}

// ── THE SEMANTIC TIER'S HALF OF THE SAME PAIR ───────────────────────────────
// The availability gate for a directly-included header is `analyze()`'s, so these
// are the pins that fail if the editor builds with the pair but ANALYZES without
// it (derives its own inputs, or none).
TEST(LspWorkspaceBuildAgreement, PeWorkspaceRefusesAPosixOnlyHeader) {
    OneFileWorkspace ws{"lsp-wba-pe-pthread",
                        "#include <pthread.h>\nint main(void) { return 0; }\n",
                        {std::string{kPe64}}};
    expectBothRefuse(askBoth(ws),
                     dss::DiagnosticCode::F_ShippedHeaderUnavailableForTarget,
                     "pe64 `#include <pthread.h>`");
}

TEST(LspWorkspaceBuildAgreement, ElfWorkspaceRefusesAWindowsOnlyHeader) {
    OneFileWorkspace ws{"lsp-wba-elf-direct",
                        "#include <direct.h>\nint main(void) { return 0; }\n",
                        {std::string{kElf}}};
    expectBothRefuse(askBoth(ws),
                     dss::DiagnosticCode::F_ShippedHeaderUnavailableForTarget,
                     "elf `#include <direct.h>`");
}

TEST(LspWorkspaceBuildAgreement, PeWorkspaceReadsWindowsLongAsTheFormatsLong) {
    // `windows.json` spells `long` as the pe data model's 32-bit integer; under the
    // LP64 default the editor used to call that a type-identity conflict.
    OneFileWorkspace ws{"lsp-wba-pe-long",
                        "#include <windows.h>\nint main(void) { return 0; }\n",
                        {std::string{kPe64}}};
    expectBothAccept(askBoth(ws), "pe64 `#include <windows.h>`");
}

TEST(LspWorkspaceBuildAgreement, LongDoubleAndSizeofFoldPerPair) {
    OneFileWorkspace pe{"lsp-wba-pe-widths",
                        "long double g = 1.0L;\n"
                        "_Static_assert(sizeof(long) == 4, \"LLP64\");\n"
                        "_Static_assert(sizeof(L\"a\") == 4, \"UTF-16 wchar_t\");\n"
                        "int main(void) { return (int)g - 1; }\n",
                        {std::string{kPe64}}};
    expectBothAccept(askBoth(pe), "pe64 widths");

    OneFileWorkspace elf{"lsp-wba-elf-widths",
                         "long double g = 1.0L;\n"
                         "_Static_assert(sizeof(long) == 4, \"LLP64\");\n"
                         "int main(void) { return (int)g - 1; }\n",
                         {std::string{kElf}}};
    expectBothRefuse(askBoth(elf), dss::DiagnosticCode::S_StaticAssertFailed,
                     "elf `sizeof(long) == 4`");
}

// ── EVERY PAIR THE WORKSPACE BUILDS ─────────────────────────────────────────
// A manifest naming pe64 AND elf builds the file twice, and the build's verdict
// is the UNION. The editor must refuse what either pair refuses — the first-pair
// rule clangd uses (`Candidates.front()`) would accept `<Windows.h>` here under
// pe64 while the elf build fails — and must say WHICH pair refused.
TEST(LspWorkspaceBuildAgreement, EveryPairIsCheckedAndTheRefusingPairIsNamed) {
    OneFileWorkspace ws{"lsp-wba-multi",
                        "#include <Windows.h>\nint main(void) { return 0; }\n",
                        {std::string{kPe64}, std::string{kElf}}};
    auto const a = askBoth(ws);
    ASSERT_NE(a.built.rc, 0) << "the elf half of the build must refuse <Windows.h>";
    ASSERT_GT(dss::test_support::countCode(
                  a.built.rep, dss::DiagnosticCode::F_ShippedHeaderNotFound), 0u);
    auto const hit = std::find_if(a.edited.begin(), a.edited.end(),
        [](Published const& p) { return p.code == "F_ShippedHeaderNotFound"; });
    ASSERT_NE(hit, a.edited.end())
        << "the editor accepted what the elf build refuses:" << render(a.edited);
    EXPECT_NE(hit->message.find(std::string{"[target="} + std::string{kElf} + "]"),
              std::string::npos)
        << "a refusal only ONE pair makes must name that pair: " << hit->message;
    EXPECT_EQ(hit->message.find(std::string{kPe64}), std::string::npos)
        << "the pe64 pair accepts <Windows.h> and must not be named: " << hit->message;
}

// ── THE BUILD'S OWN MEMBERSHIP ──────────────────────────────────────────────
// Two manifests in one root, each building its own file for its own pair. Each
// file is checked under the manifest that LISTS it, decided by the build's own
// `sources[]` expansion — not under every manifest in the folder, which would
// refuse `a.c` for an elf pair no build compiles it for.
TEST(LspWorkspaceBuildAgreement, AFileIsCheckedUnderTheManifestsThatListIt) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo,
                                          "lsp-wba-membership"};
    auto const a = scratch.path() / "a.c";
    auto const b = scratch.path() / "b.c";
    std::string_view const text = "#include <Windows.h>\nint main(void) { return 0; }\n";
    writeFile(a, text);
    writeFile(b, text);
    auto const ma = writeManifest(scratch.path(),
        Manifest{"a.dss-project.json", {std::string{kPe64}}, {a}, {}, {}});
    auto const mb = writeManifest(scratch.path(),
        Manifest{"b.dss-project.json", {std::string{kElf}}, {b}, {}, {}});

    auto const builtA = build(ma, scratch.path() / "outA");
    auto const builtB = build(mb, scratch.path() / "outB");
    ASSERT_EQ(builtA.rc, 0) << "the pe64 manifest builds a.c";
    ASSERT_NE(builtB.rc, 0) << "the elf manifest refuses b.c";

    auto const edited = editor(scratch.path(), {a, b});
    EXPECT_TRUE(edited.at(a).empty())
        << "a.c is built for pe64 only, yet the editor refused it:"
        << render(edited.at(a));
    EXPECT_TRUE(hasCode(edited.at(b), "F_ShippedHeaderNotFound"))
        << "b.c is built for elf, and the editor accepted what that build refuses:"
        << render(edited.at(b));
}

// ── A MULTI-SOURCE BUILD DECLARES THE PAIR ON EVERY UNIT ────────────────────
// Two sources route the build through the driver's one-unit-per-source path
// (`compileUnits`), whose builder site declares the pair separately from the
// single-unit path the cases above exercise. Both must say what the editor says.
TEST(LspWorkspaceBuildAgreement, AMultiSourceBuildDeclaresThePairOnEveryUnit) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo,
                                          "lsp-wba-multi-source"};
    auto const a = scratch.path() / "a.c";
    auto const b = scratch.path() / "b.c";
    writeFile(a, "#include <Windows.h>\nint helper(void) { return 0; }\n");
    writeFile(b, "int helper(void);\nint main(void) { return helper(); }\n");
    auto const manifest = writeManifest(scratch.path(),
        Manifest{"app.dss-project.json", {std::string{kPe64}}, {a, b}, {}, {}});
    Agreement agreement;
    agreement.built  = build(manifest, scratch.path() / "out");
    agreement.edited = editor(scratch.path(), {a})[a];
    expectBothAccept(agreement, "pe64 `#include <Windows.h>` in a two-source build");
}

// ── THE FILE-DRIVEN `-I` AND `--define`, AND THE DOCUMENT'S OWN DIRECTORY ───
TEST(LspWorkspaceBuildAgreement, IncludesDefinesAndTheDocumentsDirectoryReachTheEditor) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo,
                                          "lsp-wba-includes"};
    auto const src = scratch.path() / "main.c";
    // `local.h` beside the document (the quote form's first search place, which
    // the editor could not search while the document's buffer was named by its
    // URI), `cfg.h` only in the manifest's include directory, `WANT` only in its
    // defines — each consumed by a `#error` guard, so none can agree vacuously.
    writeFile(scratch.path() / "local.h", "#define LOCAL_ANSWER 42\n");
    writeFile(scratch.path() / "inc" / "cfg.h", "#define CFG_ANSWER 7\n");
    writeFile(src,
              "#include \"local.h\"\n"
              "#include \"cfg.h\"\n"
              "#if LOCAL_ANSWER != 42\n#error \"local.h did not arrive\"\n#endif\n"
              "#if CFG_ANSWER != 7\n#error \"cfg.h did not arrive\"\n#endif\n"
              "#if WANT != 3\n#error \"the define did not arrive\"\n#endif\n"
              "int main(void) { return 0; }\n");
    auto const manifest = writeManifest(scratch.path(),
        Manifest{"main.dss-project.json", {std::string{kElf}}, {src},
                 {scratch.path() / "inc"}, {"WANT=3"}});
    Agreement a;
    a.built  = build(manifest, scratch.path() / "out");
    a.edited = editor(scratch.path(), {src})[src];
    expectBothAccept(a, "manifest includes + defines + a header beside the document");
}

// ── A MANIFEST THE BUILD REFUSES IS SAID, NOT SILENTLY IGNORED ──────────────
TEST(LspWorkspaceBuildAgreement, AManifestTheBuildRefusesIsNamedOnTheDocument) {
    OneFileWorkspace ws{"lsp-wba-broken",
                        "int main(void) { return 0; }\n",
                        {"x86_64"}};   // no `:<formatName>` — a malformed spec
    auto const a = askBoth(ws);
    ASSERT_NE(a.built.rc, 0) << "the build must refuse a malformed target spec";
    ASSERT_GT(dss::test_support::countCode(
                  a.built.rep, dss::DiagnosticCode::D_InvalidTargetSpec), 0u);
    EXPECT_TRUE(hasCode(a.edited, "D_InvalidTargetSpec"))
        << "the editor must say why it cannot answer with the build's "
           "configuration:" << render(a.edited);
}
