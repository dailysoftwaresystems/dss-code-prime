// ★★★ THE EDITOR AND THE COMPILER MUST GIVE THE SAME VERDICT ON THE SAME FILE.
//
// [[D-LSP-HAS-NO-SYSTEM-INCLUDE-DIRS-AND-DROPS-THE-CU-DRIVER-DIAGNOSTICS]].
// This file is the AGREEMENT itself, in BOTH directions, with a control — not a
// test that the LSP "produces some diagnostics".
//
// ── WHAT WAS BROKEN, MEASURED THROUGH A REAL `dsscp --lsp` CHILD ────────────
// `LspServer::enqueueParse_` built its `UnitBuilder` with NO `addSystemDir`
// call, so `systemDirs` was EMPTY and no shipped descriptor resolved; and it
// published only `cu->trees()[0].diagnostics()` + `model->diagnostics()`, never
// `CompilationUnit::driverDiagnostics()` — where the import resolver puts
// `F_ShippedHeaderNotFound` / `D_UnresolvedImport` / `D_FileNotFound`. The two
// halves produced OPPOSITE user-visible harms on the same day:
//
//   * `#include <stdbool.h>` — CLI rc=0, editor a red squiggle. A FALSE ALARM.
//   * `#include <no_such_header.h>` — CLI rc=1 `F_ShippedHeaderNotFound`,
//     editor SILENT. A fatal error the editor hid.
//
// The second is the same class of harm as a silent miscompile: the instrument
// the user is watching said nothing about a failure the compiler calls fatal.
//
// ── WHY THE TWO HALVES ARE ONE TEST AND NOT TWO ────────────────────────────
// ✔MEASURED: publishing `driverDiagnostics()` ALONE — without the system dirs —
// puts ELEVEN spurious `C_UnbackedPredefinedMacro` diagnostics on EVERY open
// document, including one that compiles rc=0 and has no `#include` at all.
// (The COUNT is a property of `c.lang.json`'s predefine set and moves when that
// document does; the pin below asserts ZERO, never a number, for exactly that
// reason.) `UnitBuilder::finish()` validates
// every predefined macro's `impliedSurface` claim against the shipped corpus
// reached through `systemDirs_`, and with no system dirs every claim is
// unbacked. So `NoSpuriousUnbackedPredefinedMacro` below is not a nice-to-have:
// it is the pin that stops half of this fix from being landed alone.
//
// ── THE TWO TRAPS THIS FILE IS SHAPED AROUND ───────────────────────────────
// ★ A BROKEN CLI ARM AND A GENUINELY-AGREEING PAIR ARE INDISTINGUISHABLE.
// ✔MEASURED: a probe whose CLI invocation was malformed returned rc=2 and its
// harness printed `AGREEMENT: YES`. Every case here therefore asserts what the
// CLI arm did — an accepting case asserts rc==0 AND that an artifact was
// written; the rejecting case asserts the SPECIFIC code — and
// `ControlAgreesWithNoSystemInclude` is a case that MUST agree, so a shared
// failure for an unrelated reason (a mis-set config root, a dead schema) shows
// up as a red control rather than as agreement.
//
// ★ AND AN AGREEMENT CAN BE VACUOUS. `#include <stdio.h>` used to agree at rc=0
// only because nothing in the file USED the header — an empty system path gave
// the same answer as a populated one. Every header case here consumes the
// header at BOTH tiers: a `#error` guarded on a macro the descriptor declares
// (so the macro splice must have run) and a call to a function it declares (so
// the extern surface must have arrived).

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
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using dss::lsp::testing::LspTestHarness;
using dss::lsp::testing::fileUriFromPath;
using dss::lsp::testing::lspExit;
using dss::lsp::testing::lspInitialize;
using dss::lsp::testing::lspShutdown;
using json = nlohmann::json;

namespace {

// A fixed ELF x86_64 target: nothing is EXECUTED here, so the pin cross-builds
// on every leg (x64 / arm64, Windows / Linux / macOS) — the same choice
// `tests/program/test_include_dirs.cpp` makes and for the same reason.
constexpr std::string_view kTarget = "x86_64:elf64-x86_64-linux-exec";

[[nodiscard]] fs::path writeSource(fs::path const& dir, std::string_view name,
                                   std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p, std::ios::binary);
    f << text;
    return p;
}

// What the DRIVER says about one file. `rc` is the driver's own verdict and
// `rep` is its diagnostic stream, so a case can assert the REASON and not only
// the verdict.
struct CliVerdict {
    int                     rc = -1;
    dss::DiagnosticReporter rep;
    bool                    wroteArtifact = false;
};

[[nodiscard]] CliVerdict runDriver(fs::path const& src, fs::path const& outDir) {
    CliVerdict v;
    dss::Program p;
    p.setOutputDir(outDir);
    v.rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                          std::vector<std::string>{std::string{kTarget}}, v.rep);
    std::error_code ec;
    // ⚠ THE ARM MUST HAVE COMPILED, not merely returned a number. An
    // argument-shaped refusal also returns non-zero, and on the accepting side a
    // driver that never reached codegen would still be "rc==0" if it had exited
    // early. `artifactPaths()` answers the question the return code cannot.
    for (auto const& a : p.artifactPaths()) {
        if (a.has_value() && fs::exists(*a, ec)) v.wroteArtifact = true;
    }
    return v;
}

// Every diagnostic CODE the LSP published for `src`, from a real `LspServer`
// driven over `InMemoryTransport` (the same harness every other e2e test uses).
[[nodiscard]] std::vector<std::string> lspPublishedCodes(fs::path const& src,
                                                         std::string_view text) {
    std::string const uri = fileUriFromPath(src);
    json didOpen = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", {{"textDocument",
                     {{"uri", uri}, {"languageId", "c"},
                      {"version", 1}, {"text", std::string{text}}}}}}};

    std::vector<std::string> codes;
    LspTestHarness h;
    h.push(lspInitialize(1));
    h.push(didOpen.dump());
    h.push(lspShutdown(2));
    h.push(std::string{lspExit});
    EXPECT_EQ(h.runUntilExit(), 0);

    for (auto const& raw : h.takeServerMessages()) {
        auto const m = json::parse(raw, nullptr, false);
        if (m.is_discarded()) continue;
        if (!m.contains("method")) continue;
        if (m.at("method") != "textDocument/publishDiagnostics") continue;
        auto const& params = m.at("params");
        if (params.at("uri") != uri) continue;
        // The LAST publish for this uri is the document's current state: an
        // earlier one may predate the parse job.
        codes.clear();
        for (auto const& d : params.at("diagnostics")) {
            codes.push_back(d.value("code", std::string{}));
        }
    }
    return codes;
}

[[nodiscard]] bool has(std::vector<std::string> const& v, std::string_view c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

[[nodiscard]] std::string join(std::vector<std::string> const& v) {
    std::string s;
    for (auto const& e : v) { if (!s.empty()) s += ", "; s += e; }
    return s.empty() ? std::string{"<none>"} : s;
}

// ── the corpus ─────────────────────────────────────────────────────────────
// The CONTROL: no include at all. Both tiers must accept it. A run in which
// THIS case fails tells you nothing about any other case in the file.
constexpr std::string_view kControl =
    "int main(void) { int x = 41; return x + 1 - 42; }\n";

// stdbool, consumed at BOTH tiers.
constexpr std::string_view kStdbool =
    "#include <stdbool.h>\n"
    "#ifndef __bool_true_false_are_defined\n"
    "#error \"stdbool.h did not arrive\"\n"
    "#endif\n"
    "int main(void) { bool b = true; return b ? 0 : 1; }\n";

// stdio, consumed at BOTH tiers: `__has_include` asks the PREPROCESSOR whether
// the header is on the system path (the fact under test, asked directly), and
// `puts` makes the SEMANTIC tier consume the surface the descriptor declares.
//
// ⚠ THE FIRST DRAFT GUARDED ON `#ifndef stdin` AND WAS REFUTED BY MEASUREMENT:
// it fired for the DRIVER too, because `stdio.json` gives `stdin` variants for
// `pe` and `macho` only — on an elf target it is not a macro at all. That is a
// real and SEPARATE conformance question (C23 7.23.1 lists stdin/stdout/stderr
// among the macros a conforming `<stdio.h>` defines); guarding on it here would
// have made this fixture measure the wrong fact and gone red for a reason that
// has nothing to do with the LSP.
constexpr std::string_view kStdio =
    "#include <stdio.h>\n"
    "#if !__has_include(<stdio.h>)\n"
    "#error \"stdio.h is not on the system include path\"\n"
    "#endif\n"
    "int main(void) { puts(\"r\"); return 0; }\n";

constexpr std::string_view kMissing =
    "#include <definitely_not_a_header_xyz.h>\nint main(void){return 0;}\n";

} // namespace

// ── DIRECTION 0: the control. Both accept; the editor is silent. ───────────
TEST(LspDriverAgreement, ControlAgreesWithNoSystemInclude) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo,
                                          "lsp-driver-agreement-control"};
    auto const src = writeSource(scratch.path(), "control.c", kControl);

    auto const cli = runDriver(src, scratch.path() / "out");
    ASSERT_EQ(cli.rc, 0) << "the CONTROL must compile; with it red, no other "
                            "case in this file is interpretable";
    ASSERT_TRUE(cli.wroteArtifact)
        << "rc==0 with no artifact means the driver arm never really compiled";

    auto const codes = lspPublishedCodes(src, kControl);
    EXPECT_TRUE(codes.empty())
        << "the editor squiggled a file the compiler accepts: " << join(codes);
}

// ── DIRECTION 1: the compiler ACCEPTS ⇒ the editor must be SILENT. ─────────
// Pre-fix this was `P_PreprocessorErrorDirective` "stdbool.h did not arrive":
// the editor rejected what the build accepts.
TEST(LspDriverAgreement, EditorAcceptsAShippedHeaderTheDriverAccepts) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo,
                                          "lsp-driver-agreement-stdbool"};
    auto const src = writeSource(scratch.path(), "stdbool_use.c", kStdbool);

    auto const cli = runDriver(src, scratch.path() / "out");
    ASSERT_EQ(cli.rc, 0) << "the driver must resolve <stdbool.h>";
    ASSERT_TRUE(cli.wroteArtifact);
    EXPECT_EQ(dss::test_support::countCode(
                  cli.rep, dss::DiagnosticCode::P_PreprocessorErrorDirective), 0u)
        << "the `#error` guard fired for the DRIVER — the fixture is not "
           "consuming the header the way it claims to";

    auto const codes = lspPublishedCodes(src, kStdbool);
    EXPECT_TRUE(codes.empty())
        << "the editor rejected a buffer the compiler accepts: " << join(codes);
}

TEST(LspDriverAgreement, EditorAcceptsAConsumedStdioTheDriverAccepts) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo,
                                          "lsp-driver-agreement-stdio"};
    auto const src = writeSource(scratch.path(), "stdio_use.c", kStdio);

    auto const cli = runDriver(src, scratch.path() / "out");
    ASSERT_EQ(cli.rc, 0) << "the driver must resolve <stdio.h> and declare puts";
    ASSERT_TRUE(cli.wroteArtifact);

    auto const codes = lspPublishedCodes(src, kStdio);
    EXPECT_TRUE(codes.empty())
        << "the editor rejected a buffer the compiler accepts: " << join(codes);
}

// ── DIRECTION 2: the compiler REJECTS ⇒ the editor must SPEAK. ─────────────
// This is the SILENT half. Pre-fix the driver failed `F_ShippedHeaderNotFound`
// and the editor published an EMPTY diagnostics array — which every editor
// renders as "clean".
TEST(LspDriverAgreement, EditorReportsTheMissingSystemHeaderTheDriverRejects) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo,
                                          "lsp-driver-agreement-missing"};
    auto const src = writeSource(scratch.path(), "missing.c", kMissing);

    auto const cli = runDriver(src, scratch.path() / "out");
    ASSERT_NE(cli.rc, 0) << "a missing system header is a fatal C error";
    // The SPECIFIC code, not merely "non-zero": an argument-shaped refusal is
    // also non-zero, and would make this case pass for the wrong reason.
    ASSERT_GT(dss::test_support::countCode(
                  cli.rep, dss::DiagnosticCode::F_ShippedHeaderNotFound), 0u)
        << "the driver arm failed for some OTHER reason — this case is void";

    auto const codes = lspPublishedCodes(src, kMissing);
    ASSERT_FALSE(codes.empty())
        << "the editor was SILENT about a header miss the compiler calls fatal";
    EXPECT_TRUE(has(codes, "F_ShippedHeaderNotFound"))
        << "the editor spoke, but not about the header: " << join(codes);
}

// ── THE COUPLING PIN: (b) may not land without (a). ────────────────────────
// Publishing the CU's driver diagnostics while `systemDirs` is empty puts a
// `C_UnbackedPredefinedMacro` on every predefined macro that claims a shipped
// surface — ✔MEASURED at ELEVEN per document, on buffers the driver compiles
// rc=0. The assertion is ZERO rather than a count: the number belongs to
// `c.lang.json`'s predefine set, and a pin that named it would go red on an
// unrelated config edit while proving nothing more.
TEST(LspDriverAgreement, NoSpuriousUnbackedPredefinedMacro) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo,
                                          "lsp-driver-agreement-unbacked"};
    struct Case { std::string_view name; std::string_view text; };
    Case const cases[] = {{"control.c", kControl},
                          {"stdbool_use.c", kStdbool},
                          {"stdio_use.c", kStdio},
                          {"missing.c", kMissing}};
    for (auto const& c : cases) {
        auto const src = writeSource(scratch.path(), c.name, c.text);
        auto const codes = lspPublishedCodes(src, c.text);
        EXPECT_FALSE(has(codes, "C_UnbackedPredefinedMacro"))
            << c.name << ": the editor reported an unbacked predefined macro — "
               "the shipped corpus did not reach the LSP's CompilationUnit. "
               "Published: " << join(codes);
    }
}
