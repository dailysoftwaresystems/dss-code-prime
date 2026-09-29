// ★★★ A REFERENCE THAT STATES NO KIND TAKES IT FROM THE DEFINITION
// (D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL, P68 round 9).
//
// A `.s` address operand, memory displacement or data slot naming a symbol the
// file does not define mints an import whose code-vs-data kind is `Pending`
// (`ExternKindOrigin`; the lowering half is pinned in
// `tests/asm/test_asm_undefined_symbol_reference.cpp`). The kind then comes
// from what DEFINES the name, as it does for ld:
//   * a SIBLING unit's definition binds the reference directly, whatever it is;
//   * a library that states FUNCTION: the reference gets the stub the format's
//     call dispatch provides, the address every DSS reference to that function
//     gets (D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB is why that is not
//     yet the process's address);
//   * a library that states DATUM: refused by name, because the reference would
//     need a copy relocation, which DSS does not make;
//   * a library that states NOTHING (a NOTYPE export): refused by name.
// Nothing is defaulted. ✔MEASURED 2026-09-23 against the references, gcc 13
// with GNU ld 2.42 on x86_64 Linux:
//   * sibling object: gcc -no-pie and -pie both run to 42;
//   * `movq stdout(%rip)`: -no-pie runs to 42 with a copy relocation, and -pie
//     also runs to 42, through a copy relocation of its own;
//   * `leaq puts(%rip)` then a call through it: -no-pie runs to 42, and -pie
//     refuses the PC32 against `puts`;
//   * a NOTYPE export read as a datum: -no-pie and -pie both run to 0, which
//     is WRONG (the datum holds 42). ld copies a symbol of size 0 and warns
//     "type and size of dynamic symbol `nt_sym' are not defined".

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include "import_kind_objects.inc"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

struct Schemas {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

Schemas shipped(std::string_view format) {
    Schemas s;
    auto t = TargetSchema::loadShipped("x86_64");
    EXPECT_TRUE(t.has_value());
    if (t.has_value()) s.target = *t;
    auto f = ObjectFormatSchema::loadShipped(format);
    EXPECT_TRUE(f.has_value()) << "cannot load shipped format " << format;
    if (f.has_value()) s.format = *f;
    return s;
}

std::vector<ParseDiagnostic> withCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::vector<ParseDiagnostic> out;
    for (auto const& d : rep.all()) {
        if (d.code == code) out.push_back(d);
    }
    return out;
}

bool mentions(ParseDiagnostic const& d, std::string_view text) {
    return d.actual.find(text) != std::string::npos;
}

constexpr std::uint32_t kImport = 99;

// One unit of assembly: `main` is `leaq <import>(%rip), %rax; ret`, the
// relocation naming `ext`, with the target's RIP-relative kind, as the
// lowering writes it.
AssembledModule referencingUnit(Schemas const& s, std::uint32_t cu, ExternImport ext) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3};
    auto const* rip = s.target->relocationByName("riprel32");
    EXPECT_NE(rip, nullptr);
    fn.relocations.push_back(
        Relocation{3u, ext.symbol, rip != nullptr ? rip->kind : RelocationKind{}, 0});
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    m.externImports.push_back(std::move(ext));
    return m;
}

ExternImport importRow(std::string name, std::string library, ExternKindOrigin origin,
                       bool isData) {
    ExternImport e;
    e.symbol      = SymbolId{kImport};
    e.mangledName = std::move(name);
    e.libraryPath = std::move(library);
    e.kindOrigin  = origin;
    e.isData      = isData;
    return e;
}

}  // namespace

// ── the gate, on an IMAGE ──────────────────────────────────────────────────

TEST(ImportKindFromDefinition, ALibraryThatStatesNoKindIsRefusedByName) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format && s.format->isImageFlavor());
    auto const m = referencingUnit(
        s, 1, importRow("nt_sym", "libnotype.so", ExternKindOrigin::Pending, false));
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    auto const refused = withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable);
    ASSERT_EQ(refused.size(), 1u) << "a kind nothing states must be refused, once";
    EXPECT_TRUE(mentions(refused[0], "'nt_sym'")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "'libnotype.so'")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "reports no kind")) << refused[0].actual;
    EXPECT_FALSE(image.ok());
    EXPECT_EQ(image.resolvedFuncCount, 0u);
}

TEST(ImportKindFromDefinition, ALibraryDatumNamedDirectlyIsRefusedByName) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto const m = referencingUnit(
        s, 1, importRow("stdout", "libc.so.6", ExternKindOrigin::FromLibrary, true));
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    auto const refused = withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable);
    ASSERT_EQ(refused.size(), 1u)
        << "a direct reference to a library datum would read its slot";
    EXPECT_TRUE(mentions(refused[0], "'stdout' is a DATUM")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "copy relocation")) << refused[0].actual;
    EXPECT_FALSE(image.ok());
}

// The CONTROL the two refusals need: the same reference, to a library FUNCTION,
// is not refused. A gate that refused every `Pending`-born row would pass both
// tests above and fail this one.
TEST(ImportKindFromDefinition, ALibraryFunctionNamedByAddressIsNotRefused) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto const m = referencingUnit(
        s, 1, importRow("puts", "libc.so.6", ExternKindOrigin::FromLibrary, false));
    DiagnosticReporter rep;
    (void)linker::link(m, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty());
}

// A C unit's `extern FILE *stdout;` is `Stated`, and its own references go
// through the loader-filled slot — MIR→LIR says so on the row
// (`readThroughSlot`) — so the gate has nothing to judge. (A `Stated` row whose
// unit names the datum DIRECTLY — every row an object reader mints — IS judged
// since P68 round 11: test_got_slot_lowering.cpp, section C.)
TEST(ImportKindFromDefinition, ACUnitsStatedRowReadThroughItsSlotIsNotJudged) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto row = importRow("stdout", "libc.so.6", ExternKindOrigin::Stated, true);
    row.readThroughSlot = true;
    auto const m = referencingUnit(s, 1, std::move(row));
    DiagnosticReporter rep;
    (void)linker::link(m, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty());
}

// An UNBOUND pending row is an undefined symbol, which the reference gate names;
// it is not reported twice under two codes.
TEST(ImportKindFromDefinition, AnUnboundPendingRowIsAnUndefinedSymbol) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    auto const m = referencingUnit(
        s, 1, importRow("nowhere", "", ExternKindOrigin::Pending, false));
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty());
    EXPECT_EQ(withCode(rep, DiagnosticCode::K_SymbolUndefined).size(), 1u);
    EXPECT_FALSE(image.ok());
}

// ── a relocatable object keeps the name, as gas does ──────────────────────

TEST(ImportKindFromDefinition, ARelocatableObjectKeepsAPendingReference) {
    auto const s = shipped("elf64-x86_64-linux");
    ASSERT_TRUE(s.target && s.format);
    ASSERT_FALSE(s.format->isImageFlavor());
    auto const m = referencingUnit(
        s, 1, importRow("nt_sym", "libnotype.so", ExternKindOrigin::Pending, false));
    DiagnosticReporter rep;
    auto const image = linker::link(m, *s.target, *s.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "the linker that consumes the object judges the name, as it does gas's";
    EXPECT_TRUE(image.ok());
    EXPECT_FALSE(image.bytes.empty());
}

// ── a sibling definition decides ──────────────────────────────────────────

TEST(ImportKindFromDefinition, ASiblingDefinitionDecidesAPendingReference) {
    auto const s = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(s.target && s.format);
    std::vector<AssembledModule> mods;
    // Bound to a library that states no kind: alone, that is refused (above).
    mods.push_back(referencingUnit(
        s, 1, importRow("sib_data", "libnotype.so", ExternKindOrigin::Pending, false)));
    AssembledModule sibling;
    sibling.cuId = CompilationUnitId{2};
    sibling.symbols.push_back(ModuleSymbol{SymbolId{7}, "sib_data", SymbolBinding::Global,
                                           SymbolVisibility::Default});
    mods.push_back(std::move(sibling));
    DiagnosticReporter rep;
    auto const image = linker::link(
        std::span<AssembledModule const>{mods.data(), mods.size()}, *s.target, *s.format, rep);
    EXPECT_TRUE(withCode(rep, DiagnosticCode::K_ImportReferenceUnbindable).empty())
        << "the sibling's definition is what the reference binds to";
    ASSERT_EQ(image.resolvedCrossCuRefs.size(), 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].reference.cuId.v, 1u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].reference.symbol.v, kImport);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].definition.cuId.v, 2u);
    EXPECT_EQ(image.resolvedCrossCuRefs[0].definition.symbol.v, 7u);
}

// ── the merge: a pending row adopts a statement instead of conflicting ────

namespace {

std::size_t conflictsLinking(ExternImport first, ExternImport second) {
    auto const s = shipped("elf64-x86_64-linux");
    std::vector<AssembledModule> mods;
    mods.push_back(referencingUnit(s, 1, std::move(first)));
    auto m2 = referencingUnit(s, 2, std::move(second));
    m2.symbols[0].name = "other";          // one `main`
    m2.userEntrySymbol.reset();
    mods.push_back(std::move(m2));
    DiagnosticReporter rep;
    (void)linker::link(std::span<AssembledModule const>{mods.data(), mods.size()},
                       *s.target, *s.format, rep);
    return withCode(rep, DiagnosticCode::K_ExternImportAttributeConflict).size();
}

}  // namespace

TEST(ImportKindFromDefinition, APendingRowAdoptsTheKindAnotherUnitStates) {
    auto const pending = importRow("x", "libc.so.6", ExternKindOrigin::Pending, false);
    auto const datum   = importRow("x", "libc.so.6", ExternKindOrigin::Stated, true);
    EXPECT_EQ(conflictsLinking(pending, datum), 0u) << "pending first";
    EXPECT_EQ(conflictsLinking(datum, pending), 0u) << "pending second";
}

// The negative the adoption must not swallow: two STATED rows that disagree
// are still a conflict.
TEST(ImportKindFromDefinition, TwoStatementsThatDisagreeStillConflict) {
    auto const code  = importRow("x", "libc.so.6", ExternKindOrigin::Stated, false);
    auto const datum = importRow("x", "libc.so.6", ExternKindOrigin::Stated, true);
    EXPECT_EQ(conflictsLinking(code, datum), 1u);
}

// ── through the driver ────────────────────────────────────────────────────

namespace {

struct Built {
    int                     rc = -1;
    std::vector<ParseDiagnostic> diagnostics;
    std::filesystem::path   out;
};

Built compileAsm(std::filesystem::path const& dir, std::string const& source,
                 std::string const& spec, std::vector<std::filesystem::path> libraries = {},
                 std::string const& language = "asm-x86_64-att") {
    Built b;
    auto const src = dir / "main.s";
    { std::ofstream f{src, std::ios::binary}; f << source; }
    b.out = dir / "out";
    std::filesystem::create_directories(b.out);
    Program p;
    p.setOutputDir(b.out);
    if (!libraries.empty()) p.setResolveLibraries(libraries);
    DiagnosticReporter rep;
    b.rc = p.compileFiles(std::vector<std::string>{src.string()}, language,
                          std::vector<std::string>{spec}, rep);
    b.diagnostics.assign(rep.all().begin(), rep.all().end());
    return b;
}

std::vector<ParseDiagnostic> withCode(Built const& b, DiagnosticCode code) {
    std::vector<ParseDiagnostic> out;
    for (auto const& d : b.diagnostics) {
        if (d.code == code) out.push_back(d);
    }
    return out;
}

constexpr char const* kReadNoKindExport =
    "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\n"
    "main:\n\tmovl\tnt_sym(%rip), %eax\n\tret\n";
constexpr char const* kReadLibraryDatum =
    "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\n"
    "main:\n\tmovq\tstdout(%rip), %rcx\n\tmovl\t$42, %eax\n\tret\n";
// `leaq puts(%rip)`, then a call through that address: the reference that
// states nothing and the one that states code, on one library function.
constexpr char const* kCallThroughLibraryFunctionAddress =
    "\t.text\n\t.globl\tmain\n\t.type\tmain, @function\n"
    "main:\n\tsubq\t$40, %rsp\n\tleaq\tputs(%rip), %rax\n"
    "\tleaq\tmsg(%rip), %rcx\n\tmovq\t%rcx, %rdi\n\tcall\t*%rax\n"
    "\tmovl\t$42, %eax\n\taddq\t$40, %rsp\n\tret\n"
    "\t.data\nmsg:\t.byte\t111, 107, 0\n";

}  // namespace

TEST(ImportKindFromDefinitionProgram, AnAddressOfANoKindLibraryExportIsRefused) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const so = scratch.path() / "libnotype.so";
    {
        auto const bytes = test::notypeSharedLibrary();
        std::ofstream f{so, std::ios::binary};
        f.write(reinterpret_cast<char const*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    auto const b = compileAsm(scratch.path(), kReadNoKindExport,
                              "x86_64:elf64-x86_64-linux-exec", {so});
    EXPECT_NE(b.rc, 0);
    auto const refused = withCode(b, DiagnosticCode::K_ImportReferenceUnbindable);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_TRUE(mentions(refused[0], "'nt_sym'")) << refused[0].actual;
    EXPECT_TRUE(mentions(refused[0], "reports no kind")) << refused[0].actual;
}

TEST(ImportKindFromDefinitionProgram, ADirectReferenceToALibraryDatumIsRefused) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const b = compileAsm(scratch.path(), kReadLibraryDatum,
                              "x86_64:elf64-x86_64-linux-exec");
    EXPECT_NE(b.rc, 0);
    auto const refused = withCode(b, DiagnosticCode::K_ImportReferenceUnbindable);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_TRUE(mentions(refused[0], "'stdout' is a DATUM")) << refused[0].actual;
}

// The green control through the driver: the definition is CODE, so the address
// is the function's stub, and the program links. Built for ELF on every host;
// the PE twin RUNS where it can.
TEST(ImportKindFromDefinitionProgram, TheAddressOfALibraryFunctionLinks) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const b = compileAsm(scratch.path(), kCallThroughLibraryFunctionAddress,
                              "x86_64:elf64-x86_64-linux-exec");
    std::string why;
    for (auto const& d : b.diagnostics) why += "\n  " + d.actual;
    EXPECT_EQ(b.rc, 0) << why;
    EXPECT_TRUE(std::filesystem::exists(b.out / "main")) << why;
}

TEST(ImportKindFromDefinitionProgram, TheAddressOfALibraryFunctionRunsOnPe) {
#if !defined(_WIN32)
    GTEST_SKIP() << "runs a PE image; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const b = compileAsm(scratch.path(), kCallThroughLibraryFunctionAddress,
                              "x86_64:pe64-x86_64-windows-exec");
    std::string why;
    for (auto const& d : b.diagnostics) why += "\n  " + d.actual;
    ASSERT_EQ(b.rc, 0) << why;
    auto const r = test_support::runBinary(b.out / "main.exe");
    ASSERT_TRUE(r.spawned && !r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u);
#endif
}

// The sibling route through the driver: the example's own source and the
// reference-built archive it names. The definitions decide both kinds.
TEST(ImportKindFromDefinitionProgram, ASiblingArchiveDecidesBothKinds) {
    auto const root = test::repoRoot();
    std::string source;
    {
        std::ifstream f{root / "examples/asm/asm_x86_64_address_of_an_undefined_symbol/main.s",
                        std::ios::binary};
        ASSERT_TRUE(f.good());
        source.assign(std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{});
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const elf = compileAsm(scratch.path(), source, "x86_64:elf64-x86_64-linux-exec",
                                {root / "tests/link/data/libasm_sibling_definitions_x86_64_elf.a"});
    std::string why;
    for (auto const& d : elf.diagnostics) why += "\n  " + d.actual;
    EXPECT_EQ(elf.rc, 0) << why;
#if defined(_WIN32)
    test_support::ScratchDir peScratch{test_support::Location::InsideRepo, "import-kind"};
    auto const pe = compileAsm(peScratch.path(), source, "x86_64:pe64-x86_64-windows-exec",
                               {root / "tests/link/data/libasm_sibling_definitions_x86_64_pe.a"});
    why.clear();
    for (auto const& d : pe.diagnostics) why += "\n  " + d.actual;
    ASSERT_EQ(pe.rc, 0) << why;
    auto const r = test_support::runBinary(pe.out / "main.exe");
    ASSERT_TRUE(r.spawned && !r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u) << "30 + 10 from the datum, + 2 from the function";
#endif
}

// The arm64 twin: the same lowering, another dialect and another target. Its
// function is never CALLED, so only the sibling's definition states its kind.
// Built on every host; the examples runner runs it under qemu.
TEST(ImportKindFromDefinitionProgram, TheArm64TwinLinksAgainstItsSiblingArchive) {
    auto const root = test::repoRoot();
    std::string source;
    {
        std::ifstream f{root / "examples/asm/asm_arm64_address_of_an_undefined_symbol/main.s",
                        std::ios::binary};
        ASSERT_TRUE(f.good());
        source.assign(std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{});
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "import-kind"};
    auto const b = compileAsm(scratch.path(), source, "arm64:elf64-aarch64-linux-exec",
                              {root / "tests/link/data/libasm_sibling_definitions_aarch64_elf.a"},
                              "asm-arm64-gas");
    std::string why;
    for (auto const& d : b.diagnostics) why += "\n  " + d.actual;
    EXPECT_EQ(b.rc, 0) << why;
    EXPECT_TRUE(std::filesystem::exists(b.out / "main")) << why;
}
