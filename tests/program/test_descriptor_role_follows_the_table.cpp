// D-CONFIG-DESCRIPTOR-LIBRARY-LITERAL-DUPLICATES-THE-FORMAT-ROLE-TABLE — THE
// DRIVER'S HALF: the image the LINKER RECEIVES follows the format's role row.
//
// The row's closing witness, in its own words: "repoint a `cLibrary` row and
// require the role-bound descriptors to FOLLOW while `math.json`'s literal
// provably does not" — and it must prove the image the linker actually
// receives, not only a decoded string, because DSS imports every function a
// descriptor lists and a wrong image breaks EVERY binary's load
// ([[D-FFI-DESCRIPTOR-EAGER-IMPORT]]).
//
// So every arm here runs the real front half (`buildCuMir`) over the REAL
// shipped corpus (`applySystemDirs` — the migrated `stdio.json`, `math.json`,
// `windows.json`) and reads `externImports[].libraryPath`, which IS the
// linker's input; the repoint arms then LINK the module and read the emitted
// image's own dependency table. No C++ recompiles between the two halves of
// an arm: only a config VALUE differs.
//
// ★ WHY THE MUTANT IS A `loadFromText` OF ONE DOCUMENT AND THAT IS ENOUGH. The
// resolver answers from the ACTIVE document when it declares the role, so a
// mutant `…-exec` (which declares `cLibrary`) is consistent by itself — no
// sibling is consulted for it, and the shipped `…-exec` it supersedes is
// skipped by name. The FAMILY path is exercised separately, on the flavours
// that declare no row (`-dll`, `-dyn`, `-dylib`, `-staticlib`), and the
// family's disagreement refusal on a doctored copy of the shipped tree.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/compile_pipeline.hpp"

#include "image_dependency_table.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace {

[[nodiscard]] std::string readShippedFormatText(std::string_view name) {
    ShippedConfigLocator loc;
    loc.name            = name;
    loc.subdir          = "object-formats";
    loc.suffix          = ".format.json";
    loc.kindLabel       = "object format";
    loc.invalidNameCode = DiagnosticCode::C_InvalidFormatName;
    auto const p = findShippedConfig(loc);
    if (!p.has_value()) return {};
    std::ifstream in(*p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

[[nodiscard]] std::size_t occurrences(std::string const& text, std::string_view needle) {
    std::size_t n = 0;
    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) ++n;
    return n;
}

[[nodiscard]] bool substituteOnce(std::string& text, std::string_view from,
                                  std::string_view to) {
    auto const at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), to);
    return true;
}

[[nodiscard]] std::string joinDiags(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += d.actual; out += "\n"; }
    return out;
}

// The shipped C grammar and a shipped target, loaded once per arm.
struct Toolchain {
    std::shared_ptr<GrammarSchema> grammar;
    std::shared_ptr<TargetSchema>  target;

    [[nodiscard]] static std::optional<Toolchain> load(char const* targetName) {
        auto grammarR = GrammarSchema::loadShipped("c");
        auto targetR  = TargetSchema::loadShipped(targetName);
        EXPECT_TRUE(grammarR.has_value()) << "c.lang.json must load";
        EXPECT_TRUE(targetR.has_value()) << targetName << " must load";
        if (!grammarR || !targetR) return std::nullopt;
        return Toolchain{*grammarR, *targetR};
    }
};

[[nodiscard]] std::optional<std::uint16_t>
callingConventionIndex(Toolchain const& tc, ObjectFormatSchema const& format,
                       DiagnosticReporter& rep) {
    auto const abi = dss::ffi::resolveAbi(*tc.target, format, rep);
    if (!abi.has_value() || abi->cc == nullptr) return std::nullopt;
    auto const ccSpan = tc.target->callingConventions();
    return static_cast<std::uint16_t>(std::distance(ccSpan.data(), abi->cc));
}

// Every extern import the front half produced for `source`, keyed by its
// on-binary name — the exact rows the linker is handed.
struct FrontHalf {
    bool                               ok = false;
    std::string                        why;
    std::map<std::string, std::string> libraryByName;
};

[[nodiscard]] FrontHalf frontHalf(Toolchain const& tc, ObjectFormatSchema const& format,
                                  std::string const& source,
                                  std::optional<fs::path> const& extraSystemDir = std::nullopt) {
    FrontHalf out;
    DiagnosticReporter rep;
    auto const ccIndex = callingConventionIndex(tc, format, rep);
    if (!ccIndex.has_value()) {
        out.why = "no calling convention resolved: " + joinDiags(rep);
        return out;
    }
    UnitBuilder builder{tc.grammar, DiagnosticBudget::libraryDefault()};
    if (extraSystemDir.has_value()) builder.addSystemDir(*extraSystemDir);
    dss::applySystemDirs(builder, *tc.grammar);
    builder.addInMemory(source, "witness.c");
    CompilationUnit cu = std::move(builder).finish();
    auto mir = buildCuMir(cu, *tc.grammar, *tc.target, format, *ccIndex, rep,
                          CompileOptions{DiagnosticBudget::libraryDefault()});
    if (!mir.has_value() || rep.errorCount() != 0) {
        out.why = "buildCuMir refused: " + joinDiags(rep);
        return out;
    }
    for (auto const& e : mir->externImports)
        out.libraryByName.emplace(e.mangledName, e.libraryPath);
    out.ok = true;
    return out;
}

// The linked IMAGE for `source`, or empty with `why`.
[[nodiscard]] std::vector<std::uint8_t>
linkedImage(Toolchain const& tc, ObjectFormatSchema const& format,
            std::string const& source, std::string& why) {
    DiagnosticReporter rep;
    auto const ccIndex = callingConventionIndex(tc, format, rep);
    if (!ccIndex.has_value()) { why = "no calling convention"; return {}; }
    UnitBuilder builder{tc.grammar, DiagnosticBudget::libraryDefault()};
    dss::applySystemDirs(builder, *tc.grammar);
    builder.addInMemory(source, "witness.c");
    CompilationUnit cu = std::move(builder).finish();
    auto mod = assembleUnit(cu, *tc.grammar, *tc.target, format, *ccIndex, rep,
                            CompileOptions{DiagnosticBudget::libraryDefault()});
    if (!mod.has_value() || rep.errorCount() != 0) {
        why = "assembleUnit refused: " + joinDiags(rep);
        return {};
    }
    auto image = linker::link(*mod, *tc.target, format, rep);
    if (rep.errorCount() != 0 || !image.ok() || image.bytes.empty()) {
        why = "link failed: " + joinDiags(rep);
        return {};
    }
    return image.bytes;
}

// `puts` on a format whose C symbols carry a leading underscore is `_puts`.
[[nodiscard]] std::string lookupEither(std::map<std::string, std::string> const& m,
                                       std::string const& plain) {
    if (auto it = m.find(plain); it != m.end()) return it->second;
    if (auto it = m.find("_" + plain); it != m.end()) return it->second;
    return "<absent>";
}

constexpr std::string_view kPeSource =
    "#include <stdio.h>\n"
    "#include <windows.h>\n"
    "int main(void) { SRWLOCK l; InitializeSRWLock(&l); puts(\"hi\"); return 0; }\n";

constexpr std::string_view kElfSource =
    "#include <stdio.h>\n"
    "#include <math.h>\n"
    "int main(void) { puts(\"hi\"); return (int)sin(0.0); }\n";

constexpr std::string_view kLibrarySource =
    "#include <stdio.h>\n"
    "int say(void) { puts(\"hi\"); return 0; }\n";

}  // namespace

// ── pe: a `cLibrary` repoint moves every role-bound import and no other ─────
//
// `puts` (stdio.json → `cLibrary`) follows; `InitializeSRWLock` (windows.json
// → `systemPrimitives`) stays on kernel32 — a different role, the control that
// shows the repoint moved exactly the role it named. Asserted on the linker's
// input rows AND on the emitted image's import table.
TEST(DescriptorRoleFollowsTheTable, PeRepointOfCLibraryMovesRoleBoundImportsOnly) {
    auto const tc = Toolchain::load("x86_64");
    ASSERT_TRUE(tc.has_value());
    std::string const text = readShippedFormatText("pe64-x86_64-windows-exec");
    ASSERT_FALSE(text.empty());

    auto witnessFmt = ObjectFormatSchema::loadFromText(text);
    ASSERT_TRUE(witnessFmt.has_value());
    auto const witness = frontHalf(*tc, **witnessFmt, std::string{kPeSource});
    ASSERT_TRUE(witness.ok) << witness.why;
    EXPECT_EQ(lookupEither(witness.libraryByName, "puts"), "ucrtbase.dll");
    EXPECT_EQ(lookupEither(witness.libraryByName, "InitializeSRWLock"), "kernel32.dll");

    // ONE value repointed — the same coherent alternative the role table's own
    // pin uses (`vcruntime140.dll` is a real image Microsoft names separately).
    constexpr std::string_view kRow =
        R"({ "role": "cLibrary",          "image": "ucrtbase.dll" },)";
    ASSERT_EQ(occurrences(text, kRow), 1u)
        << "the mutation anchor must appear exactly once";
    std::string mutant = text;
    ASSERT_TRUE(substituteOnce(
        mutant, kRow,
        R"({ "role": "cLibrary",          "image": "vcruntime140.dll" },)"));
    ASSERT_NE(mutant, text);
    auto mutantFmt = ObjectFormatSchema::loadFromText(mutant, "<mutant>");
    ASSERT_TRUE(mutantFmt.has_value()) << "the mutant must still load";

    auto const moved = frontHalf(*tc, **mutantFmt, std::string{kPeSource});
    ASSERT_TRUE(moved.ok) << moved.why;
    EXPECT_EQ(lookupEither(moved.libraryByName, "puts"), "vcruntime140.dll")
        << "stdio.json names `cLibrary`; the import the linker receives must "
           "FOLLOW the row";
    EXPECT_EQ(lookupEither(moved.libraryByName, "InitializeSRWLock"), "kernel32.dll")
        << "windows.json names `systemPrimitives`; a `cLibrary` repoint must "
           "not move it";

    // THE IMAGE. The witness imports the UCRT and kernel32; the mutant imports
    // vcruntime140 and kernel32 and NOT the UCRT — this format points only
    // `cLibrary` and `unwindPersonality` at it, and no SEH region is resolved.
    std::string whyA;
    auto const witnessBytes = linkedImage(*tc, **witnessFmt, std::string{kPeSource}, whyA);
    ASSERT_FALSE(witnessBytes.empty()) << whyA;
    auto const witnessLibs = dss::test_support::peImportedLibraries(witnessBytes);
    EXPECT_GE(dss::test_support::dependencyOccurrences(witnessLibs, "ucrtbase.dll"), 1u)
        << dss::test_support::joinDependencies(witnessLibs);
    EXPECT_GE(dss::test_support::dependencyOccurrences(witnessLibs, "kernel32.dll"), 1u)
        << dss::test_support::joinDependencies(witnessLibs);
    EXPECT_EQ(dss::test_support::dependencyOccurrences(witnessLibs, "vcruntime140.dll"), 0u)
        << dss::test_support::joinDependencies(witnessLibs);

    std::string whyB;
    auto const mutantBytes = linkedImage(*tc, **mutantFmt, std::string{kPeSource}, whyB);
    ASSERT_FALSE(mutantBytes.empty()) << whyB;
    EXPECT_NE(witnessBytes, mutantBytes);
    auto const mutantLibs = dss::test_support::peImportedLibraries(mutantBytes);
    EXPECT_GE(dss::test_support::dependencyOccurrences(mutantLibs, "vcruntime140.dll"), 1u)
        << dss::test_support::joinDependencies(mutantLibs);
    EXPECT_GE(dss::test_support::dependencyOccurrences(mutantLibs, "kernel32.dll"), 1u)
        << dss::test_support::joinDependencies(mutantLibs);
    EXPECT_EQ(dss::test_support::dependencyOccurrences(mutantLibs, "ucrtbase.dll"), 0u)
        << "the ORIGINAL image must be gone from the mutant's import table: "
        << dss::test_support::joinDependencies(mutantLibs);
}

// ── elf: the `libm.so.6` literal provably does NOT follow ───────────────────
TEST(DescriptorRoleFollowsTheTable, ElfRepointOfCLibraryLeavesTheLibmLiteralAlone) {
    auto const tc = Toolchain::load("x86_64");
    ASSERT_TRUE(tc.has_value());
    std::string const text = readShippedFormatText("elf64-x86_64-linux-exec");
    ASSERT_FALSE(text.empty());

    auto witnessFmt = ObjectFormatSchema::loadFromText(text);
    ASSERT_TRUE(witnessFmt.has_value());
    auto const witness = frontHalf(*tc, **witnessFmt, std::string{kElfSource});
    ASSERT_TRUE(witness.ok) << witness.why;
    EXPECT_EQ(lookupEither(witness.libraryByName, "puts"), "libc.so.6");
    EXPECT_EQ(lookupEither(witness.libraryByName, "sin"), "libm.so.6");

    constexpr std::string_view kRow = R"({ "role": "cLibrary", "image": "libc.so.6" })";
    ASSERT_EQ(occurrences(text, kRow), 1u);
    std::string mutant = text;
    ASSERT_TRUE(substituteOnce(
        mutant, kRow, R"({ "role": "cLibrary", "image": "libc-repointed.so.6" })"));
    auto mutantFmt = ObjectFormatSchema::loadFromText(mutant, "<mutant>");
    ASSERT_TRUE(mutantFmt.has_value());

    auto const moved = frontHalf(*tc, **mutantFmt, std::string{kElfSource});
    ASSERT_TRUE(moved.ok) << moved.why;
    EXPECT_EQ(lookupEither(moved.libraryByName, "puts"), "libc-repointed.so.6")
        << "the role-bound import must FOLLOW";
    EXPECT_EQ(lookupEither(moved.libraryByName, "sin"), "libm.so.6")
        << "math.json's elf literal names no role and must NOT follow";

    std::string whyA;
    auto const witnessBytes = linkedImage(*tc, **witnessFmt, std::string{kElfSource}, whyA);
    ASSERT_FALSE(witnessBytes.empty()) << whyA;
    auto const witnessNeeded = dss::test_support::elfNeededLibraries(witnessBytes);
    EXPECT_GE(dss::test_support::dependencyOccurrences(witnessNeeded, "libc.so.6"), 1u)
        << dss::test_support::joinDependencies(witnessNeeded);
    EXPECT_GE(dss::test_support::dependencyOccurrences(witnessNeeded, "libm.so.6"), 1u)
        << dss::test_support::joinDependencies(witnessNeeded);

    std::string whyB;
    auto const mutantBytes = linkedImage(*tc, **mutantFmt, std::string{kElfSource}, whyB);
    ASSERT_FALSE(mutantBytes.empty()) << whyB;
    auto const mutantNeeded = dss::test_support::elfNeededLibraries(mutantBytes);
    EXPECT_GE(dss::test_support::dependencyOccurrences(mutantNeeded, "libc-repointed.so.6"), 1u)
        << dss::test_support::joinDependencies(mutantNeeded);
    EXPECT_GE(dss::test_support::dependencyOccurrences(mutantNeeded, "libm.so.6"), 1u)
        << "the literal's image must still be needed: "
        << dss::test_support::joinDependencies(mutantNeeded);
    EXPECT_EQ(dss::test_support::dependencyOccurrences(mutantNeeded, "libc.so.6"), 0u)
        << dss::test_support::joinDependencies(mutantNeeded);
}

// ── the FAMILY path: a flavour that declares no `cLibrary` row ──────────────
//
// `-dll`, `-dyn`, `-dylib` and `-staticlib` declare no `cLibrary` (the loader
// would refuse one as inert config), and before this row they imported the C
// library through the descriptors' literals. Now they reach the family's row.
// The assertion that the flavour's OWN table lacks the row is what proves the
// family path, not the own-row path, produced the image.
TEST(DescriptorRoleFollowsTheTable, AFlavourDeclaringNoCLibraryRowBindsThroughItsFamily) {
    struct Arm {
        char const* target;
        char const* format;
        char const* image;
    };
    constexpr Arm kArms[] = {
        {"x86_64", "pe64-x86_64-windows-dll",       "ucrtbase.dll"},
        {"x86_64", "pe64-x86_64-windows-staticlib", "ucrtbase.dll"},
        {"x86_64", "elf64-x86_64-linux-dyn",        "libc.so.6"},
        {"x86_64", "elf64-x86_64-linux-staticlib",  "libc.so.6"},
        {"arm64",  "elf64-aarch64-linux-staticlib", "libc.so.6"},
        {"arm64",  "macho64-arm64-darwin-dylib",    "/usr/lib/libSystem.B.dylib"},
        {"arm64",  "macho64-arm64-darwin-staticlib","/usr/lib/libSystem.B.dylib"},
    };
    for (auto const& arm : kArms) {
        SCOPED_TRACE(arm.format);
        auto const tc = Toolchain::load(arm.target);
        ASSERT_TRUE(tc.has_value());
        auto formatR = ObjectFormatSchema::loadShipped(arm.format);
        ASSERT_TRUE(formatR.has_value());
        ASSERT_EQ((*formatR)->runtimeLibraries().rowForRole(RuntimeLibraryRole::CLibrary),
                  nullptr)
            << arm.format << " now declares `cLibrary` itself, so this arm no "
                             "longer exercises the family path — pick a flavour "
                             "that does not";
        auto const bound = frontHalf(*tc, **formatR, std::string{kLibrarySource});
        ASSERT_TRUE(bound.ok) << bound.why;
        EXPECT_EQ(lookupEither(bound.libraryByName, "puts"), arm.image)
            << "the import must carry the family's `cLibrary` image";
    }
}

// ── the family REFUSES two siblings that disagree ───────────────────────────
//
// On a copy of the shipped tree, one elf flavour that declares `cLibrary`
// (`-pie`) is repointed while its siblings are not. A flavour with no own row
// (`-staticlib`) must be REFUSED — naming both providers and the document —
// never answered by scan order; a flavour with its own row (`-exec`) still
// answers from itself, because its own spine blocks already bind that row and
// the build stays self-consistent.
TEST(DescriptorRoleFollowsTheTable, TheFamilyRefusesTwoSiblingsThatDisagree) {
    auto const shippedDir = findShippedConfigDir("object-formats");
    ASSERT_TRUE(shippedDir.has_value());
    // A REPO-SHAPED root: `DSS_CONFIG_ROOT` names the directory that CONTAINS
    // `src/dss-config` (✔MEASURED the wrong way first — pointing it at the config
    // directory itself makes the override miss silently and the cwd walk answers
    // with the REAL, agreeing tree).
    ScratchDir scratch{Location::Temp, "descriptor-role-family"};
    fs::path const tree = scratch.path();
    fs::path const root = tree / "src" / "dss-config";
    // Only the PARENTS are created here: `fs::copy` creates each target
    // directory itself, and on this toolchain copying onto a directory that
    // already exists reports "File exists" instead of merging.
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy(*shippedDir, root / "object-formats", fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << "copying object-formats: " << ec.message();
    // The pe documents realize `atomicsRuntime` from a shipped source; keep the
    // runtime SOURCE tree beside them so every sibling still loads on the copy.
    auto const runtimeDir = findShippedConfigDir("runtime");
    ASSERT_TRUE(runtimeDir.has_value());
    fs::create_directories(root / "runtime" / "platform", ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy(*runtimeDir / "platform" / "src", root / "runtime" / "platform" / "src",
             fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << "copying runtime/platform/src: " << ec.message();

    fs::path const pie = root / "object-formats" / "elf64-x86_64-linux-pie.format.json";
    std::string text;
    {
        std::ifstream in(pie, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    ASSERT_FALSE(text.empty());
    constexpr std::string_view kRow = R"({ "role": "cLibrary", "image": "libc.so.6" })";
    ASSERT_EQ(occurrences(text, kRow), 1u);
    ASSERT_TRUE(substituteOnce(text, kRow,
                               R"({ "role": "cLibrary", "image": "libc-other.so.6" })"));
    std::ofstream(pie, std::ios::binary) << text;

    ScopedEnv const env{"DSS_CONFIG_ROOT", tree.string()};

    auto staticlib = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-staticlib");
    ASSERT_TRUE(staticlib.has_value());
    ASSERT_EQ((*staticlib)->runtimeLibraries().rowForRole(RuntimeLibraryRole::CLibrary),
              nullptr);
    // Through the ONE adapter the driver uses — the own-row-then-family rule and
    // the family cache both live there, so asking any other way would pin a
    // second reading of which row answers.
    FormatRuntimeLibraryRoleResolver const resolver{**staticlib};
    std::string refusal;
    auto const* const answer =
        resolver.rowForRole(RuntimeLibraryRole::CLibrary, refusal);
    ASSERT_EQ(answer, nullptr)
        << "two elf flavours name different `cLibrary` images; a flavour with no "
           "row of its own must be REFUSED, not answered by whichever the scan "
           "met first";
    ASSERT_FALSE(refusal.empty()) << "a null row with no refusal reads as 'no "
                                    "document declares it', which is a different "
                                    "answer entirely";
    EXPECT_NE(refusal.find("two different providers"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("libc-other.so.6"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("libc.so.6"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("elf64-x86_64-linux-pie"), std::string::npos)
        << "the refusal must name the disagreeing document: " << refusal;

    // CONTROL: a flavour with its own row is untouched by the siblings' quarrel.
    auto exec = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(exec.has_value());
    FormatRuntimeLibraryRoleResolver const execResolver{**exec};
    std::string execRefusal;
    auto const* const own =
        execResolver.rowForRole(RuntimeLibraryRole::CLibrary, execRefusal);
    ASSERT_NE(own, nullptr) << execRefusal;
    EXPECT_TRUE(execRefusal.empty()) << execRefusal;
    EXPECT_EQ(own->image, "libc.so.6");
}

// ── S1: THE FAMILY SCAN MAY NOT BE CACHED ON THE MEMOIZED SCHEMA ────────────
//
// `ObjectFormatSchema` instances are memoized by (label, content digest) with an
// EMPTY dependency ledger, so a hit is served for a document whose OWN bytes are
// unchanged. The flavour-family scan reads 23 OTHER documents. Caching its result
// on the instance therefore folds their bytes into an entry keyed on bytes that
// did not change — the stale-hit shape `config_document_memo.hpp` names a silent
// miscompile. This pin holds the boundary from the outside: within ONE process,
// with the member document byte-for-byte identical, editing a SIBLING must move
// the answer at the next binding operation.
TEST(DescriptorRoleFollowsTheTable, ASiblingEditIsSeenThoughTheMemberIsAMemoHit) {
    auto const shippedDir = findShippedConfigDir("object-formats");
    ASSERT_TRUE(shippedDir.has_value());
    // The same REPO-SHAPED root the sibling arm above builds, for the same
    // measured reason: `DSS_CONFIG_ROOT` names the directory CONTAINING
    // `src/dss-config`.
    ScratchDir scratch{Location::Temp, "role-sibling-edit"};
    fs::path const tree = scratch.path();
    fs::path const root = tree / "src" / "dss-config";
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy(*shippedDir, root / "object-formats", fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << "copying object-formats: " << ec.message();
    // The pe documents realize `atomicsRuntime` from a shipped source, and the
    // family scan is TOTAL — a sibling that fails to load refuses the whole
    // family — so the runtime source tree comes along.
    auto const runtimeDir = findShippedConfigDir("runtime");
    ASSERT_TRUE(runtimeDir.has_value());
    fs::create_directories(root / "runtime" / "platform", ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::copy(*runtimeDir / "platform" / "src", root / "runtime" / "platform" / "src",
             fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << "copying runtime/platform/src: " << ec.message();

    ScopedEnv const env{"DSS_CONFIG_ROOT", tree.string()};

    // The member: a flavour with NO `cLibrary` row of its own, so every answer
    // comes from a sibling.
    auto const memberOf = [](auto const& schema) {
        return schema->contentDigest();
    };
    auto first = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ((*first)->runtimeLibraries().rowForRole(RuntimeLibraryRole::CLibrary),
              nullptr);
    {
        FormatRuntimeLibraryRoleResolver const resolver{**first};
        std::string refusal;
        auto const* const row =
            resolver.rowForRole(RuntimeLibraryRole::CLibrary, refusal);
        ASSERT_NE(row, nullptr) << refusal;
        EXPECT_EQ(row->image, "libc.so.6");
    }

    // Edit the SIBLINGS — the member document is not touched. EVERY document
    // declaring the row moves together: the family is keyed on the format KIND,
    // so the elf family spans both architectures (x86_64 and aarch64, exec and
    // pie), and moving only some of them would measure the DISAGREEMENT refusal
    // instead of staleness. The population is discovered rather than listed, so
    // a new flavour cannot silently split it.
    constexpr std::string_view kRow = R"({ "role": "cLibrary", "image": "libc.so.6" })";
    constexpr std::string_view kMoved =
        R"({ "role": "cLibrary", "image": "libc-moved.so.6" })";
    std::size_t moved = 0;
    for (auto const& entry :
         fs::directory_iterator{root / "object-formats"}) {
        if (!entry.is_regular_file()) continue;
        if (!entry.path().filename().string().ends_with(".format.json")) continue;
        std::string text;
        {
            std::ifstream in(entry.path(), std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        if (occurrences(text, kRow) == 0) continue;
        ASSERT_EQ(occurrences(text, kRow), 1u) << entry.path().string();
        ASSERT_TRUE(substituteOnce(text, kRow, kMoved));
        std::ofstream(entry.path(), std::ios::binary) << text;
        ++moved;
    }
    ASSERT_GE(moved, 2u) << "the elf `cLibrary` row is declared by more than one "
                            "flavour; finding fewer means the substitution missed "
                            "and this arm would prove nothing";

    // Same process, same member document, byte-for-byte: a memo HIT by
    // construction — asserted, so this arm cannot pass by accidentally missing.
    auto second = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(memberOf(*first), memberOf(*second))
        << "the member document was not edited; its digest must not have moved";
    EXPECT_EQ(first->get(), second->get())
        << "the member must be served from the memo — if it is not, this arm "
           "proves nothing about a cached family";

    FormatRuntimeLibraryRoleResolver const after{**second};
    std::string refusal;
    auto const* const row = after.rowForRole(RuntimeLibraryRole::CLibrary, refusal);
    ASSERT_NE(row, nullptr) << refusal;
    EXPECT_EQ(row->image, "libc-moved.so.6")
        << "the sibling edit was not observed: the flavour family was answered "
           "from a cache that outlived the documents it was assembled from";
}

// ── the driver refuses a role the family realizes, or does not declare ──────
//
// Through `buildCuMir`, so the refusal is the one a user sees: a descriptor
// naming a role the family REALIZES from a shipped source (pe `atomicsRuntime`)
// has no image to import from; one naming a role NO flavour declares (elf
// `unwindPersonality`) has nothing to bind. Both are refused at the read,
// naming the role and the family — never bound to a guess.
TEST(DescriptorRoleFollowsTheTable, TheDriverRefusesARoleTheFamilyRealizesOrLacks) {
    ScratchDir sysDir{Location::Temp, "descriptor-role-refusal"};
    std::string const source =
        "#include <rolewitness.h>\nint main(void) { return witness_fn(); }\n";
    auto writeDescriptor = [&](std::string const& library) {
        std::ofstream(sysDir.path() / "rolewitness.json", std::ios::binary)
            << "{ \"header\": \"rolewitness.h\", \"library\": " << library
            << ", \"symbols\": [ { \"name\": \"witness_fn\", \"signature\": \"fn() -> i32\" } ] }";
    };
    {
        auto const tc = Toolchain::load("x86_64");
        ASSERT_TRUE(tc.has_value());
        auto formatR = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
        ASSERT_TRUE(formatR.has_value());
        writeDescriptor(R"({ "pe": { "role": "atomicsRuntime" } })");
        auto const r = frontHalf(*tc, **formatR, source, sysDir.path());
        EXPECT_FALSE(r.ok) << "a REALIZED role has no image to import from";
        EXPECT_NE(r.why.find("REALIZES"), std::string::npos) << r.why;
        EXPECT_NE(r.why.find("atomicsRuntime"), std::string::npos) << r.why;
    }
    {
        auto const tc = Toolchain::load("x86_64");
        ASSERT_TRUE(tc.has_value());
        auto formatR = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
        ASSERT_TRUE(formatR.has_value());
        writeDescriptor(R"({ "elf": { "role": "unwindPersonality" } })");
        auto const r = frontHalf(*tc, **formatR, source, sysDir.path());
        EXPECT_FALSE(r.ok) << "a role no elf flavour declares binds nothing";
        EXPECT_NE(r.why.find("no shipped 'elf' object-format document declares"),
                  std::string::npos)
            << r.why;
    }
    {
        // CONTROL: the same descriptor naming a role the family DOES declare
        // builds, and binds the family's image.
        auto const tc = Toolchain::load("x86_64");
        ASSERT_TRUE(tc.has_value());
        auto formatR = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-staticlib");
        ASSERT_TRUE(formatR.has_value());
        writeDescriptor(R"({ "elf": { "role": "cLibrary" } })");
        auto const r = frontHalf(*tc, **formatR, source, sysDir.path());
        ASSERT_TRUE(r.ok) << r.why;
        EXPECT_EQ(lookupEither(r.libraryByName, "witness_fn"), "libc.so.6");
    }
}

// ── S3: `activeFormat` AND THE RESOLVER ARE ONE STATEMENT, AND THEY MUST AGREE
//
// The two arguments describe the SAME format, and nothing outside `analyze`
// makes them. A caller passing one kind with the other's resolver binds NOTHING
// — a role entry resolves only for the key the resolver answers for — so every
// shipped import goes out unbound and, since DSS eager-imports every function a
// descriptor lists, the program fails at LOAD with no diagnostic at any compile
// stage. Failing loud here costs a comparison; the alternative is a silent
// unbound build. The check is in `analyze` rather than `analyzeImpl` so it runs
// BEFORE the large-stack worker thread exists.
TEST(DescriptorRoleFollowsTheTableDeath, AResolverForAnotherKindRefusesTheAnalysis) {
    auto const tc = Toolchain::load("x86_64");
    ASSERT_TRUE(tc.has_value());
    auto pe = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(pe.has_value());
    UnitBuilder builder{tc->grammar, DiagnosticBudget::libraryDefault()};
    builder.addInMemory("int main(void) { return 0; }\n", "witness.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    FormatRuntimeLibraryRoleResolver const peRoles{**pe};
    EXPECT_DEATH(
        {
            (void)analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                          std::nullopt, std::nullopt, ObjectFormatKind::Elf,
                          std::optional<std::string_view>{"x86_64"},
                          LongDoubleFormat::None, nullptr, 0, &peRoles);
        },
        "role resolver answers for");
}

// CONTROL, and it is the arm that stops the refusal widening: NO resolver is not
// a mismatch. Every direct-API caller — the LSP, the header parser, the ~40 test
// call sites that pass an `activeFormat` and bind no import — relies on it, and
// a check that fired here would redden all of them.
TEST(DescriptorRoleFollowsTheTable, NoResolverBesideAnActiveFormatIsNotAMismatch) {
    auto const tc = Toolchain::load("x86_64");
    ASSERT_TRUE(tc.has_value());
    UnitBuilder builder{tc->grammar, DiagnosticBudget::libraryDefault()};
    builder.addInMemory("int main(void) { return 0; }\n", "witness.c");
    auto cu = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto const model =
        analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                std::nullopt, std::nullopt, ObjectFormatKind::Elf,
                std::optional<std::string_view>{"x86_64"});
    EXPECT_EQ(model.diagnostics().errorCount(), 0u) << joinDiags(model.diagnostics());
}
