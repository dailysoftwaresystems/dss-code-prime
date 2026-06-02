// CLI argument-parsing tests — plan 14 LK10 cycle 3.
//
// Pins:
//   * Help / LSP / no-arg modes are recognized.
//   * --compile / --directory / --project mode flags are
//     mutually exclusive.
//   * --target is repeatable and required for compile / directory.
//   * --language is required for compile / directory.
//   * --recursive / --no-recursive toggles InputResolver::Mode.
//   * --warnings-as-errors / --suppress=<code> populate the
//     DiagnosticReporter::Config policy (D-LK10-7 closure).
//   * Unknown flags produce CliArgsError::UnknownFlag.
//   * Missing flag values fire CliArgsError::MissingFlagValue.

#include "core/types/parse_diagnostic.hpp"
#include "program/cli_args.hpp"
#include "program/input_resolver.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

using namespace dss;

namespace {

// gtest-friendly argv builder — keeps the lifetime of the
// `std::string` storage in the test fixture so `argv[]` pointers
// remain valid through the parseCliArgs call.
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*>       ptrs;

    explicit Argv(std::initializer_list<std::string> args) {
        storage.assign(args.begin(), args.end());
        ptrs.reserve(storage.size() + 1);
        for (auto& s : storage) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
    }
    [[nodiscard]] int   argc() const noexcept { return static_cast<int>(storage.size()); }
    [[nodiscard]] char** argv() noexcept { return ptrs.data(); }
};

} // namespace

// ── Help / no-arg / LSP modes ────────────────────────────────

TEST(CliArgs, NoArgsProducesEmptyResult) {
    Argv a{"dss-code-prime"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->lspMode);
    EXPECT_FALSE(r->helpMode);
    EXPECT_TRUE(r->sourceFiles.empty());
    EXPECT_FALSE(r->directoryPath.has_value());
}

TEST(CliArgs, HelpFlagSetsHelpMode) {
    Argv a{"dss-code-prime", "--help"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->helpMode);
}

TEST(CliArgs, ShortHelpFlagSetsHelpMode) {
    Argv a{"dss-code-prime", "-h"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->helpMode);
}

TEST(CliArgs, LspFlagSetsLspMode) {
    Argv a{"dss-code-prime", "--lsp"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->lspMode);
}

TEST(CliArgs, LspWithSchemaDir) {
    Argv a{"dss-code-prime", "--lsp", "--schema-dir=/tmp/schemas"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->lspMode);
    ASSERT_TRUE(r->lspSchemaDir.has_value());
    EXPECT_EQ(r->lspSchemaDir->generic_string(), "/tmp/schemas");
}

// ── Compile mode ─────────────────────────────────────────────

TEST(CliArgs, CompileModeWithSingleFile) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->sourceFiles.size(), 1u);
    EXPECT_EQ(r->sourceFiles[0], "hello.c");
    EXPECT_EQ(r->languageName, "c-subset");
    ASSERT_EQ(r->targets.size(), 1u);
    EXPECT_EQ(r->targets[0], "x86_64:elf64-x86_64-linux");
}

TEST(CliArgs, CompileModeWithMultipleFiles) {
    Argv a{"dss-code-prime", "--compile", "a.c", "b.c", "c.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->sourceFiles.size(), 3u);
    EXPECT_EQ(r->sourceFiles[0], "a.c");
    EXPECT_EQ(r->sourceFiles[1], "b.c");
    EXPECT_EQ(r->sourceFiles[2], "c.c");
}

TEST(CliArgs, MultipleTargetsAccepted) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--target", "x86_64:pe64-x86_64-windows"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->targets.size(), 2u);
    EXPECT_EQ(r->targets[0], "x86_64:elf64-x86_64-linux");
    EXPECT_EQ(r->targets[1], "x86_64:pe64-x86_64-windows");
}

TEST(CliArgs, CompileModeRejectsEmptyFileList) {
    Argv a{"dss-code-prime", "--compile",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::EmptyFileList);
}

TEST(CliArgs, CompileModeRequiresLanguage) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--target", "x86_64:elf64-x86_64-linux"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingLanguage);
}

TEST(CliArgs, CompileModeRequiresTarget) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::EmptyTargetList);
}

// ── Directory mode ───────────────────────────────────────────

TEST(CliArgs, DirectoryModeWithRecursive) {
    Argv a{"dss-code-prime", "--directory", "src/",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--recursive"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->directoryPath.has_value());
    EXPECT_EQ(*r->directoryPath, "src/");
    EXPECT_EQ(r->directoryMode, InputResolver::Mode::Recursive);
}

TEST(CliArgs, DirectoryModeWithNoRecursive) {
    Argv a{"dss-code-prime", "--directory", "src/",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--no-recursive"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->directoryMode, InputResolver::Mode::Flat);
}

TEST(CliArgs, DirectoryDefaultIsRecursive) {
    Argv a{"dss-code-prime", "--directory", "src/",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->directoryMode, InputResolver::Mode::Recursive);
}

// ── Project mode ─────────────────────────────────────────────

TEST(CliArgs, ProjectModeAccepted) {
    Argv a{"dss-code-prime", "--project", "myproj.dsp"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->projectPath.has_value());
    EXPECT_EQ(*r->projectPath, "myproj.dsp");
}

// ── Mutually-exclusive modes ─────────────────────────────────

TEST(CliArgs, RejectsCompileAndDirectoryTogether) {
    Argv a{"dss-code-prime",
           "--compile", "hello.c",
           "--directory", "src/",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::DuplicateModeFlag);
}

TEST(CliArgs, RejectsCompileAndLspTogether) {
    Argv a{"dss-code-prime",
           "--compile", "hello.c", "--lsp"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::DuplicateModeFlag);
}

// ── Diagnostic policy (D-LK10-7 closure) ─────────────────────

TEST(CliArgs, WarningsAsErrorsToggle) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--warnings-as-errors"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->warningsAsErrors);
}

TEST(CliArgs, SuppressByCodeName) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--suppress=D_FileNotFound"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->suppress.size(), 1u);
    EXPECT_EQ(r->suppress[0], DiagnosticCode::D_FileNotFound);
}

TEST(CliArgs, SuppressByHexValue) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--suppress=0xD001"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->suppress.size(), 1u);
    EXPECT_EQ(r->suppress[0], DiagnosticCode::D_FileNotFound);  // 0xD001
}

TEST(CliArgs, SuppressMultipleCodesAccumulates) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--suppress=D_FileNotFound",
           "--suppress=D_DuplicateFile"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->suppress.size(), 2u);
}

TEST(CliArgs, SuppressRejectsUnknownCode) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--suppress=No_Such_Code"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::InvalidSuppressCode);
}

// ── Error paths ──────────────────────────────────────────────

TEST(CliArgs, RejectsUnknownFlag) {
    Argv a{"dss-code-prime", "--no-such-flag"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::UnknownFlag);
    EXPECT_NE(r.error().detail.find("--no-such-flag"), std::string::npos);
}

TEST(CliArgs, RejectsMissingFlagValue) {
    Argv a{"dss-code-prime", "--target"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

TEST(CliArgs, HelpTextContainsCoreFlags) {
    auto const text = cliHelpText();
    EXPECT_NE(text.find("--compile"), std::string::npos);
    EXPECT_NE(text.find("--transpile"), std::string::npos);
    EXPECT_NE(text.find("--config"), std::string::npos);
    EXPECT_NE(text.find("--directory"), std::string::npos);
    EXPECT_NE(text.find("--language"), std::string::npos);
    EXPECT_NE(text.find("--target"), std::string::npos);
    EXPECT_NE(text.find("--recursive"), std::string::npos);
    EXPECT_NE(text.find("--no-recursive"), std::string::npos);
    EXPECT_NE(text.find("--warnings-as-errors"), std::string::npos);
    EXPECT_NE(text.find("--suppress"), std::string::npos);
    EXPECT_NE(text.find("--lsp"), std::string::npos);
}

// ── --transpile mode (plan 10 dispatch — fail-loud today) ────

TEST(CliArgs, ParsesTranspileMode) {
    Argv a{"dss-code-prime",
           "--transpile", "in.c",
           "--language", "c-subset",
           "--target", "wasm32-v1-link-wasi"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    EXPECT_TRUE(r->sourceFiles.empty());
    ASSERT_EQ(r->transpileFiles.size(), 1u);
    EXPECT_EQ(r->transpileFiles[0], "in.c");
    EXPECT_EQ(r->languageName, "c-subset");
    ASSERT_EQ(r->targets.size(), 1u);
}

TEST(CliArgs, TranspileAndCompileAreMutuallyExclusive) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--transpile", "b.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::DuplicateModeFlag);
}

// ── --config=debug|release (plan 22 wiring slot) ─────────────

TEST(CliArgs, ConfigDefaultsToDebug) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->config, CompileConfig::Debug);
}

TEST(CliArgs, ConfigParsesReleaseEqualsForm) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf",
           "--config=release"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    EXPECT_EQ(r->config, CompileConfig::Release);
}

TEST(CliArgs, ConfigParsesDebugSpaceForm) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf",
           "--config", "debug"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    EXPECT_EQ(r->config, CompileConfig::Debug);
}

TEST(CliArgs, ConfigRejectsInvalidValue) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf",
           "--config=fast"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::InvalidConfig);
    EXPECT_NE(r.error().detail.find("fast"), std::string::npos);
}

TEST(CliArgs, CompileConfigNameRoundTrip) {
    EXPECT_EQ(compileConfigName(CompileConfig::Debug),   "debug");
    EXPECT_EQ(compileConfigName(CompileConfig::Release), "release");
}

// ── --target=spec equals form ───────────────────────────────

TEST(CliArgs, TargetAcceptsEqualsForm) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target=x86_64-v1-link-elf"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    ASSERT_EQ(r->targets.size(), 1u);
    EXPECT_EQ(r->targets[0], "x86_64-v1-link-elf");
}

// ── Silent-failure-hunter F1 fold: "Unknown" sentinel reject ──

TEST(CliArgs, SuppressRejectsUnknownSentinel) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf",
           "--suppress=Unknown"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::InvalidSuppressCode);
}

// ── F2 fold: bare `-` and empty filename rejected ────────────

TEST(CliArgs, CompileRejectsBareHyphenPositional) {
    Argv a{"dss-code-prime",
           "--compile", "-",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::EmptyFilename);
}

TEST(CliArgs, CompileRejectsEmptyStringPositional) {
    Argv a{"dss-code-prime",
           "--compile", "",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::EmptyFilename);
}

// ── F3 fold: no-mode with options → NoModeSelected, not silent ──

TEST(CliArgs, NoModeWithLanguageOptionRejects) {
    Argv a{"dss-code-prime", "--language", "c-subset"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::NoModeSelected);
}

// ── Post-fold #1: silent-failure audit (C1/C2/H1/H2) ─────────

// C1: `--suppress=0xFFFF` (hex form, unenumerated value) must reject —
// symmetric with the F1 fold's `--suppress=Unknown` symbolic-name
// reject. Otherwise the user inserts a useless entry into the suppress
// set and thinks something is suppressed.
TEST(CliArgs, SuppressRejectsUnenumeratedHexCode) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf",
           "--suppress=0xFFFF"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::InvalidSuppressCode);
}

// C1: but a real hex-form code is accepted. Validates round-trip:
// parse the hex form of `D_FileNotFound`'s value back to the symbol.
TEST(CliArgs, SuppressAcceptsEnumeratedHexCode) {
    auto const code = static_cast<std::uint16_t>(DiagnosticCode::D_FileNotFound);
    char hexBuf[8];
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%04X", code);
    std::string const flag = std::string{"--suppress="} + hexBuf;
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf",
           flag};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    ASSERT_EQ(r->suppress.size(), 1u);
    EXPECT_EQ(r->suppress[0], DiagnosticCode::D_FileNotFound);
}

// C2: empty RHS in --target= must reject as MissingFlagValue (was
// silently accepted as "" target spec).
TEST(CliArgs, TargetEqualsEmptyRhsRejects) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target="};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

// C2: same for --config= empty RHS.
TEST(CliArgs, ConfigEqualsEmptyRhsRejects) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf",
           "--config="};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

// H1: --schema-dir=<path> outside --lsp mode is silently dropped today —
// must reject so the user sees the directive isn't being honored.
TEST(CliArgs, SchemaDirOutsideLspModeRejects) {
    Argv a{"dss-code-prime",
           "--compile", "a.c",
           "--language", "c-subset",
           "--target", "x86_64-v1-link-elf",
           "--schema-dir=/tmp/schemas"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::NoModeSelected);
    EXPECT_NE(r.error().detail.find("--schema-dir"), std::string::npos);
}

// H1: but with --lsp it's accepted.
TEST(CliArgs, SchemaDirInLspModeAccepted) {
    Argv a{"dss-code-prime", "--lsp", "--schema-dir=/tmp/schemas"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    EXPECT_TRUE(r->lspMode);
    ASSERT_TRUE(r->lspSchemaDir.has_value());
}

// ── Post-fold #2: 7-agent audit folds ────────────────────────

// C2 symmetry (pr-test-analyzer Rating 9): every value-bearing flag
// must reject empty RHS. Cover the 5 untested call sites.
TEST(CliArgs, DirectoryEqualsEmptyRhsRejects) {
    Argv a{"dss-code-prime", "--directory=", "--language", "c-subset",
           "--target", "x86_64-v1-link-elf"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

TEST(CliArgs, ProjectEqualsEmptyRhsRejects) {
    Argv a{"dss-code-prime", "--project="};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

TEST(CliArgs, LanguageEqualsEmptyRhsRejects) {
    Argv a{"dss-code-prime", "--compile", "a.c", "--language=",
           "--target", "x86_64-v1-link-elf"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

TEST(CliArgs, LangAliasEqualsEmptyRhsRejects) {
    Argv a{"dss-code-prime", "--compile", "a.c", "--lang=",
           "--target", "x86_64-v1-link-elf"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

TEST(CliArgs, SuppressEqualsEmptyRhsRejects) {
    Argv a{"dss-code-prime", "--compile", "a.c", "--language", "c-subset",
           "--target", "x86_64-v1-link-elf", "--suppress="};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

// post-fold #2: takeFlagValue tighten — space form with empty next
// arg must also reject (was silently accepted as ""). Comment-analyzer
// caught the asymmetry between equalsValue and takeFlagValue.
TEST(CliArgs, TargetSpaceFormEmptyNextArgRejects) {
    Argv a{"dss-code-prime", "--compile", "a.c", "--language", "c-subset",
           "--target", ""};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::MissingFlagValue);
}

// C1 boundary (pr-test-analyzer Rating 8): 0x0000 must reject because
// no enumerated code at value 0.
TEST(CliArgs, SuppressRejectsHexZero) {
    Argv a{"dss-code-prime", "--compile", "a.c", "--language", "c-subset",
           "--target", "x86_64-v1-link-elf", "--suppress=0x0000"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::InvalidSuppressCode);
}

// silent-failure post-fold #2: --config case-insensitive.
TEST(CliArgs, ConfigAcceptsAllCapsRelease) {
    Argv a{"dss-code-prime", "--compile", "a.c", "--language", "c-subset",
           "--target", "x86_64-v1-link-elf", "--config=RELEASE"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    EXPECT_EQ(r->config, CompileConfig::Release);
}

TEST(CliArgs, ConfigAcceptsAllCapsDebug) {
    Argv a{"dss-code-prime", "--compile", "a.c", "--language", "c-subset",
           "--target", "x86_64-v1-link-elf", "--config=DEBUG"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value()) << cliArgsErrorName(r.error().kind) << ": " << r.error().detail;
    EXPECT_EQ(r->config, CompileConfig::Debug);
}

// type-design post-fold #2: UnknownFlag (bad spelling) vs
// UnexpectedPositional (file outside --compile) split.
TEST(CliArgs, BarePositionalEmitsUnexpectedPositional) {
    Argv a{"dss-code-prime", "stray.c"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, CliArgsError::UnexpectedPositional);
}

// ── D-LK10-ENTRY Slice C companion: `--output <dir>` ──────────────

TEST(CliArgs, OutputFlagSpaceFormSetsOutputDir) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--output", "build/bin"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->outputDir.has_value());
    EXPECT_EQ(r->outputDir->generic_string(), "build/bin");
}

TEST(CliArgs, OutputFlagEqualsFormSetsOutputDir) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--output=dist/x86_64"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->outputDir.has_value());
    EXPECT_EQ(r->outputDir->generic_string(), "dist/x86_64");
}

TEST(CliArgs, OutputFlagDefaultsToNullopt) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux"};
    auto r = parseCliArgs(a.argc(), a.argv());
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->outputDir.has_value())
        << "absent --output → nullopt; driver uses cwd/target/...";
}

TEST(CliArgs, OutputFlagEmptyValueRejected) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--output="};
    auto r = parseCliArgs(a.argc(), a.argv());
    EXPECT_FALSE(r.has_value())
        << "--output= with empty value must reject";
}

TEST(CliArgs, OutputFlagMissingValueRejected) {
    Argv a{"dss-code-prime", "--compile", "hello.c",
           "--language", "c-subset",
           "--target", "x86_64:elf64-x86_64-linux",
           "--output"};  // no following arg
    auto r = parseCliArgs(a.argc(), a.argv());
    EXPECT_FALSE(r.has_value())
        << "--output with no following arg must reject";
}
