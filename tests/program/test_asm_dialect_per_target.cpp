// ★★★ THE ASSEMBLY DIALECT IS RESOLVED PER TARGET
// (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET, closed 2026-08-13).
//
// An assembly file is target-specific BY NATURE — no `.s` compiles for both
// x86_64 and arm64 — so "which dialect?" is a question the TARGET answers,
// exactly as `gcc main.s` does. Two dialects now ship
// (`asm-x86_64-att.lang.json`, `asm-arm64-gas.lang.json`) and BOTH declare
// `.s`/`.S`, so the extension alone cannot name one and first-match would be a
// silent wrong answer on any multi-dialect tree.
//
// The driver used to load ONE `GrammarSchema` for the whole invocation, before
// the per-target loop ever ran. It now resolves the grammar PER `CuBuildKey`:
// the caller's `--language` when there is one, else the target's declared
// `defaultAssemblyLanguage` (a NAME the `.target.json` carries as vocabulary —
// never assembly grammar; see `D-CONFIG-ASM-DIALECT-DECLARED-AS-TARGET-
// VOCABULARY` for the facet that was reverted for crossing that line).
//
// ── WHY THESE TESTS LIVE HERE AND NOT IN `examples/**` ────────────────────
// Same structural reason `TFC74CuCacheKeyIsPerTargetNotPerObjectFormat` gives:
// the corpus runner compiles each manifest target ROW in its own call with a
// SINGLE-element target vector, so every corpus build is a one-target build
// whose `CuBuildKey` is unique by construction. No example, however written,
// can produce the two-targets-one-invocation situation this anchor is ABOUT.
// The `UnitBuilder` case at the end sits here too, deliberately: it is the
// second half of one contract (two resolvers, one rule) and splitting the pair
// across test tiers is how the halves drift.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "program/input_resolver.hpp"
#include "program/program.hpp"
#include "scoped_env.hpp"    // the ONE env override (this file used to carry a copy)
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace {

// The x86_64 spec pair used throughout. `-exec` formats, because the assembly
// path emits a linked image (the `encode` pipeline tier) rather than an object.
constexpr std::string_view kX86Spec = "x86_64:elf64-x86_64-linux-exec";
constexpr std::string_view kArmSpec = "arm64:elf64-aarch64-linux-exec";

fs::path writeFile(fs::path const& dir, std::string_view name,
                   std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p, std::ios::binary);
    f << text;
    return p;
}

// ★ A `.s` whose FUNCTION-ENTRY MARKER is the arm64 gas spelling.
//
// The marker is what makes this pair of tests work, and it is not incidental:
// gas spells a function's type `%function` on AArch64 and `@function` on x86,
// and `@` is not even LEXABLE in the arm64 dialect. So `%function` is the
// spelling that lets ONE file be parsed by both dialects and ACCEPTED by
// exactly one of them — which is precisely the observable "two dialects were
// resolved" needs. Everything else in the file (`.text`, `.globl`, a label,
// `ret`) is spelled identically in both dialects.
constexpr std::string_view kArmMarkedAsm =
    "\t.text\n"
    "\t.globl\tmain\n"
    "\t.type\tmain, %function\n"
    "main:\n"
    "\tret\n";

// The same file with NO function-entry marker at all. Both dialects parse it
// and both REFUSE it — with a diagnostic that names the dialect that refused.
// Two refusals, two different names, one invocation.
constexpr std::string_view kUnmarkedAsm =
    "\t.text\n"
    "\t.globl\tmain\n"
    "main:\n"
    "\tret\n";

// Concatenate every diagnostic's rendered text so a test can assert on what the
// operator actually reads. `actual` carries the message body; `contextPrefix`
// carries the `[target=<spec>]` stamp the per-target merge adds, and BOTH
// matter here — the whole claim is that a given dialect was applied for a given
// TARGET, so an assertion that ignored the stamp would not be making it.
[[nodiscard]] std::string allDiagnosticText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += d.contextPrefix;
        out += ' ';
        out += d.actual;
        out += '\n';
    }
    return out;
}

// The env override this file uses (`DSS_CONFIG_ROOT`, restored on scope exit so
// one test cannot leak a patched config tree into the next) is
// `dss::test_support::ScopedEnv` — see the using-declaration above. The local
// copy that used to sit here was one of five byte-identical hand-rolls; it is
// gone, and `tests/test_support/test_scoped_env.cpp` is where the restore
// semantics are actually measured.

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// THE HEADLINE: one invocation, one `.s`, two CPUs, TWO DIALECTS.
// ════════════════════════════════════════════════════════════════════════════
//
// This is the anchor's own stated trigger — "the first build that compiles `.s`
// for two different CPUs in one invocation" — and the observable is chosen so
// that BOTH halves of the claim are witnessed, one positively and one by name:
//
//   • arm64 ACCEPTS the file and an artifact lands on disk. Only
//     `AsmArm64Gas` accepts `%function`, so the artifact existing IS the proof
//     that the arm64 leg was parsed under the arm64 dialect.
//   • x86_64 REFUSES it, and the refusal NAMES `AsmX86_64Att` and target
//     `x86_64`. The same source text, in the same invocation, was therefore
//     also parsed under a DIFFERENT grammar.
//
// ── RED-ON-DISABLE (measured, not assumed) ──────────────────────────────────
// Revert the per-key threading — delete `languageName` from `CuBuildKey` and
// from its `operator<` in src/program/program.cpp — and the two targets
// collapse onto ONE cache key. The CU is then built once, under whichever
// grammar the FIRST target resolved (x86_64 → `AsmX86_64Att`), and handed to
// arm64. `%function` under the AT&T dialect lexes `%` as the REGISTER sigil, so
// the single shared CU fails at parse, no artifact is produced for either
// target, and the arm64 assertion below reds.
//
// ⚠ NOTE WHAT A WEAKER FIXTURE WOULD HAVE MISSED. A dialect-NEUTRAL `.s`
// (`.text` / `.globl` / label / `ret`, spelled identically in both dialects)
// compiles to different machine code per target — `ret` is 0xC3 on x86_64 and
// 0xD65F03C0 on arm64 — and looks like a fine two-dialect witness. It is NOT:
// with the key collapsed, the shared CU's CST still carries the right
// instruction and `compileOneTarget` still receives the right target, so BOTH
// artifacts come out CORRECT and the test stays green through the defect. The
// fixture has to make the two dialects DISAGREE about the source, not merely
// about the bytes.
TEST(AsmDialectPerTarget, OneInvocationTwoCpusResolvesTwoDialects) {
    ScratchDir scratch{Location::InsideRepo, "asm-dialect"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "twocpu.s", kArmMarkedAsm);

    DiagnosticReporter rep;
    Program            prog;
    // NOTE THE EMPTY LANGUAGE. That is the whole interface change: nothing
    // names a grammar, so each target names its own.
    int const rc = prog.compileFiles({src.generic_string()}, /*language*/ "",
                                     {std::string{kX86Spec},
                                      std::string{kArmSpec}},
                                     rep);
    std::string const text = allDiagnosticText(rep);

    // ── half 1: the arm64 leg PARSED AND COMPILED it ────────────────────────
    auto const armArtifact =
        scratch.path() / "target" / "elf64-aarch64-linux-exec" / "twocpu";
    EXPECT_TRUE(fs::exists(armArtifact))
        << "the arm64 target must have parsed this file under `AsmArm64Gas` — "
           "it is the only shipped dialect that accepts `%function` — and "
           "emitted an image at " << armArtifact << "\ndiagnostics:\n" << text;

    // ── half 2: the x86_64 leg used a DIFFERENT dialect, and says so ────────
    EXPECT_NE(text.find("AsmX86_64Att"), std::string::npos)
        << "the x86_64 leg must have been parsed under the AT&T dialect; the "
           "diagnostic names the dialect it applied.\ndiagnostics:\n" << text;
    EXPECT_NE(text.find(std::string{kX86Spec}), std::string::npos)
        << "the refusal must be attributed to the x86_64 target.\n"
           "diagnostics:\n" << text;

    // The run as a whole fails, because one of its two targets did. Asserted so
    // the test cannot be satisfied by a build that quietly succeeded at
    // everything — a green rc here would mean the x86_64 leg did NOT refuse.
    EXPECT_NE(rc, 0) << "one target refused the source; the run must be red";
}

// The purest form of the same claim, with no artifact involved: an unmarked
// `.s` is refused by BOTH dialects, and the two refusals carry DIFFERENT
// dialect names in ONE run's output. Two grammars ran over one file.
//
// RED-ON-DISABLE: with `languageName` out of `CuBuildKey`, one CU is built
// under the first target's grammar and BOTH diagnostics name `AsmX86_64Att` —
// the `AsmArm64Gas` assertion fails.
TEST(AsmDialectPerTarget, BothDialectNamesAppearInOneRun) {
    ScratchDir scratch{Location::InsideRepo, "asm-dialect"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "unmarked.s", kUnmarkedAsm);

    DiagnosticReporter rep;
    Program            prog;
    (void)prog.compileFiles({src.generic_string()}, /*language*/ "",
                            {std::string{kX86Spec}, std::string{kArmSpec}},
                            rep);
    std::string const text = allDiagnosticText(rep);

    EXPECT_NE(text.find("AsmX86_64Att"), std::string::npos)
        << "diagnostics:\n" << text;
    EXPECT_NE(text.find("AsmArm64Gas"), std::string::npos)
        << "the second target must have been parsed under ITS dialect, not the "
           "first target's.\ndiagnostics:\n" << text;
}

// ════════════════════════════════════════════════════════════════════════════
// EXPLICIT `--language` WINS OVER THE TARGET DEFAULT
// ════════════════════════════════════════════════════════════════════════════
//
// Naming the language is the CORRECT interface when a file is written for one
// CPU — the analogue of `gcc -x` — and it is what `examples/asm/*/expected.json`
// uses. So a named language must beat the target's declared default even when
// the target disagrees: here the arm64 target declares `asm-arm64-gas`, and the
// caller's `asm-x86_64-att` must still be the grammar that runs.
//
// Observable: the refusal names `AsmX86_64Att` under an arm64 target. If the
// target default had won, it would name `AsmArm64Gas` — and the file would have
// been ACCEPTED (it carries `%function`), so this is not a message-only
// difference.
TEST(AsmDialectPerTarget, ExplicitLanguageBeatsTheTargetDefault) {
    ScratchDir scratch{Location::InsideRepo, "asm-dialect"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "explicit.s", kArmMarkedAsm);

    DiagnosticReporter rep;
    Program            prog;
    int const rc = prog.compileFiles({src.generic_string()}, "asm-x86_64-att",
                                     {std::string{kArmSpec}}, rep);
    std::string const text = allDiagnosticText(rep);

    EXPECT_NE(rc, 0)
        << "the AT&T dialect cannot accept `%function`; had the arm64 target's "
           "default won, this would have compiled";
    EXPECT_NE(text.find("AsmX86_64Att"), std::string::npos)
        << "the CALLER's language must be the one applied.\ndiagnostics:\n"
        << text;
    EXPECT_EQ(text.find("AsmArm64Gas"), std::string::npos)
        << "the target's default must NOT override an explicitly named "
           "language.\ndiagnostics:\n" << text;
}

// ════════════════════════════════════════════════════════════════════════════
// A TARGET THAT DECLARES NO DIALECT FAILS LOUD — NAMING ITSELF
// ════════════════════════════════════════════════════════════════════════════
//
// Absence of `defaultAssemblyLanguage` is a legitimate target state, so the
// loud failure belongs to the driver, not the config loader. The message must
// name the TARGET, because the operator's fix is either "pass --language" or
// "declare the key in THAT target file" — and neither is actionable without the
// name.
//
// The fixture patches a COPY of the shipped config tree and reaches it through
// `DSS_CONFIG_ROOT` (the same mechanism `tests/core/test_config_path_walk.cpp`
// uses), so the shipped x86_64 target is never modified and the test is
// hermetic. The copy is asserted to have actually lost the key — a patch that
// silently no-op'd would leave this test green against the unpatched tree,
// proving nothing.
TEST(AsmDialectPerTarget, TargetWithoutADeclaredDialectFailsLoudByName) {
    ScratchDir scratch{Location::InsideRepo, "asm-dialect"};

    // 1. Copy the whole shipped config tree next to the scratch dir.
    auto const repoConfig = []() -> fs::path {
        // The test binary runs with cwd inside the repo (dss_add_test), and
        // `DSS_CONFIG_ROOT` points at the repo root — use it directly rather
        // than re-implementing the 8-hop walk.
        char const* root = std::getenv("DSS_CONFIG_ROOT");
        return root != nullptr ? fs::path{root} / "src" / "dss-config"
                               : fs::path{"src"} / "dss-config";
    }();
    ASSERT_TRUE(fs::is_directory(repoConfig))
        << "shipped config tree not found at " << repoConfig;

    auto const fakeRoot   = scratch.path() / "fakeroot";
    auto const fakeConfig = fakeRoot / "src" / "dss-config";
    std::error_code ec;
    // Create the PARENT only — `fs::copy` creates the destination directory
    // itself, and pre-creating it makes the recursive copy report "File
    // exists" on this toolchain.
    fs::create_directories(fakeConfig.parent_path(), ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy(repoConfig, fakeConfig, fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << "config-tree copy failed: " << ec.message();

    // 2. Strip the key from the copy's x86_64 target.
    auto const patched = fakeConfig / "targets" / "x86_64.target.json";
    ASSERT_TRUE(fs::exists(patched));
    std::string body;
    {
        std::ifstream in(patched, std::ios::binary);
        body.assign(std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>());
    }
    constexpr std::string_view kKey = "\"defaultAssemblyLanguage\"";
    auto const keyAt = body.find(kKey);
    ASSERT_NE(keyAt, std::string::npos)
        << "the shipped x86_64 target must DECLARE the key for this test to "
           "have anything to remove — if this fires, the feature regressed";
    auto const lineStart = body.rfind('\n', keyAt);
    auto const lineEnd   = body.find('\n', keyAt);
    ASSERT_NE(lineStart, std::string::npos);
    ASSERT_NE(lineEnd, std::string::npos);
    body.erase(lineStart, lineEnd - lineStart);
    {
        std::ofstream out(patched, std::ios::binary | std::ios::trunc);
        out << body;
    }
    ASSERT_EQ(body.find(kKey), std::string::npos)
        << "the patch must actually have removed the key";

    // 3. Point discovery at the patched tree and compile a `.s` with no
    //    --language. cwd stays outside the fake root, so ONLY the env override
    //    can be what resolves config — if it were ignored, the shipped tree
    //    would answer and the test would silently pass for the wrong reason
    //    (which is why the assertion below is on the MESSAGE, not just on rc).
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "nodialect.s", kUnmarkedAsm);
    ScopedEnv env{"DSS_CONFIG_ROOT", fakeRoot.generic_string()};

    DiagnosticReporter rep;
    Program            prog;
    int const rc = prog.compileFiles({src.generic_string()}, /*language*/ "",
                                     {std::string{kX86Spec}}, rep);
    std::string const text = allDiagnosticText(rep);

    EXPECT_NE(rc, 0) << "there is no language to parse under; guessing one "
                        "would be a silent wrong answer.\n" << text;
    EXPECT_NE(text.find("x86_64"), std::string::npos)
        << "the diagnostic must NAME the target that declares nothing — the "
           "operator's fix lives in that target's file.\ndiagnostics:\n"
        << text;
    EXPECT_NE(text.find("defaultAssemblyLanguage"), std::string::npos)
        << "the diagnostic must name the KEY that is missing, not merely "
           "report a failure.\ndiagnostics:\n" << text;
}

// ════════════════════════════════════════════════════════════════════════════
// `--directory` COLLECTS A `.s`
// ════════════════════════════════════════════════════════════════════════════
//
// The anchor's second stated trigger. The directory scan runs ONCE, before any
// target loop, so its extension filter cannot be per-target — and a scan filter
// drops files without a word, so a too-narrow filter is a SILENT skip rather
// than an error. With no `--language`, the filter is the union over the named
// targets of each target's declared assembly language's extensions.
//
// RED-ON-DISABLE: restore the filter to `grammar->fileExtensions()` and the
// scan finds no files at all (there is no `grammar`), or — under the old
// required-`--language` shape — the `.s` is never collected and the build
// reports success having compiled nothing.
TEST(AsmDialectPerTarget, DirectoryBuildCollectsAnAssemblyFile) {
    ScratchDir scratch{Location::InsideRepo, "asm-dialect"};
    scratch.useAsCwd();
    auto const srcDir = scratch.path() / "asmsrc";
    std::error_code ec;
    fs::create_directories(srcDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    writeFile(srcDir, "unit.s", kArmMarkedAsm);

    Program prog;
    int const rc = prog.compileDirectory(srcDir.generic_string(),
                                         /*language*/ "",
                                         {std::string{kArmSpec}},
                                         InputResolver::Mode::Recursive);
    EXPECT_EQ(rc, 0) << "the .s must be collected AND compiled";
    EXPECT_TRUE(fs::exists(scratch.path() / "target"
                           / "elf64-aarch64-linux-exec" / "unit"))
        << "an artifact must exist — a scan that collected nothing would also "
           "have produced rc 0 from an empty build, which is the silent skip "
           "this widening exists to prevent";
}

// ★ AND THE NAMED-LANGUAGE CASE IS DELIBERATELY UNCHANGED. `--language
// c-subset` over a directory holding a `.s` must NOT sweep the assembly file
// in: one CU carries one grammar to codegen, so widening here would trade a
// silent skip for a silent MIS-PARSE (the `.s` read as C). The scan reports the
// honest "no matching files" instead.
TEST(AsmDialectPerTarget, DirectoryBuildWithANamedLanguageIgnoresForeignFiles) {
    ScratchDir scratch{Location::InsideRepo, "asm-dialect"};
    scratch.useAsCwd();
    auto const srcDir = scratch.path() / "csrc";
    std::error_code ec;
    fs::create_directories(srcDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    writeFile(srcDir, "only.s", kArmMarkedAsm);

    Program prog;
    int const rc = prog.compileDirectory(srcDir.generic_string(), "c-subset",
                                         {std::string{kX86Spec}},
                                         InputResolver::Mode::Recursive);
    EXPECT_NE(rc, 0)
        << "a c-subset directory build must not silently adopt a .s; with no "
           "c-subset file present the scan must report an empty input";
}

// ════════════════════════════════════════════════════════════════════════════
// THE SECOND RESOLVER: `UnitBuilder` MUST NOT PICK A WINNER EITHER
// ════════════════════════════════════════════════════════════════════════════
//
// `UnitBuilder::schemasForPath_` used to return the FIRST registered schema
// whose `fileExtensions` matched, which made REGISTRATION ORDER decide what a
// `.s` means the moment two dialects were registered. It never produced a parse
// error — it produced a plausible WRONG PARSE. The builder has no target and
// therefore no basis for a choice, so it names both claimants and refuses.
//
// RED-ON-DISABLE: restore `return s;` inside the loop in `schemasForPath_` (and
// the single-claimant branch in `addFile`) and this test fails — the file
// parses silently under whichever dialect was registered first.
TEST(AsmDialectPerTarget, TwoRegisteredLanguagesClaimingOneExtensionFailLoud) {
    ScratchDir scratch{Location::InsideRepo, "asm-dialect"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "ambiguous.s", kUnmarkedAsm);

    auto att = GrammarSchema::loadShipped("asm-x86_64-att");
    auto gas = GrammarSchema::loadShipped("asm-arm64-gas");
    ASSERT_TRUE(att.has_value());
    ASSERT_TRUE(gas.has_value());

    UnitBuilder builder{*att};
    builder.registerSchema(*gas);
    builder.addFile(src);
    auto cu = std::move(builder).finish();

    bool sawExtensionError = false;
    std::string text;
    for (auto const& d : cu.driverDiagnostics().all()) {
        text += d.actual;
        text += '\n';
        if (d.code == DiagnosticCode::D_UnknownFileExtension) {
            sawExtensionError = true;
        }
    }
    EXPECT_TRUE(sawExtensionError)
        << "two registered languages claim `.s`; the builder must refuse.\n"
        << text;
    EXPECT_NE(text.find("AsmX86_64Att"), std::string::npos)
        << "both claimants must be NAMED — 'ambiguous' without the names "
           "leaves no action available.\n" << text;
    EXPECT_NE(text.find("AsmArm64Gas"), std::string::npos) << text;
    EXPECT_TRUE(cu.trees().empty())
        << "refusing means NOT parsing: a tree here would mean the file was "
           "parsed under an arbitrarily-chosen grammar after all";
}

// The same builder, ONE dialect registered, must still route the `.s` — the
// ambiguity fix must not have turned "one claimant" into "no claimant".
TEST(AsmDialectPerTarget, OneRegisteredClaimantStillRoutesByExtension) {
    ScratchDir scratch{Location::InsideRepo, "asm-dialect"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "single.s", kUnmarkedAsm);

    auto c   = GrammarSchema::loadShipped("c-subset");
    auto att = GrammarSchema::loadShipped("asm-x86_64-att");
    ASSERT_TRUE(c.has_value());
    ASSERT_TRUE(att.has_value());

    UnitBuilder builder{*c};
    builder.registerSchema(*att);
    builder.addFile(src);
    auto cu = std::move(builder).finish();

    for (auto const& d : cu.driverDiagnostics().all()) {
        EXPECT_NE(d.code, DiagnosticCode::D_UnknownFileExtension)
            << "exactly one registered language claims `.s`; routing must "
               "succeed: " << d.actual;
    }
    ASSERT_EQ(cu.trees().size(), 1u);
    EXPECT_EQ(cu.trees()[0].schema().name(), "AsmX86_64Att")
        << "the file must be parsed under the language that CLAIMS `.s`, not "
           "under the primary";
}
