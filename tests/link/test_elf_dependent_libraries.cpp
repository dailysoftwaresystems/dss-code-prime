// P69 (lane `xa`, fold 3f) -- `#pragma comment(lib, ...)`'s ELF half: the DEPENDENT-LIBRARIES section.
//
// A relocatable ELF object names the libraries it asks its final link to search in one section, each a
// NUL-terminated name. ✔MEASURED (P69 fold 3f, clang 18.1.3 x86_64-linux-gnu): `#pragma comment(lib, "m")` writes
// `.deplibs` (SHT_LLVM_DEPENDENT_LIBRARIES 0x6fff4c04, SHF_MERGE|SHF_STRINGS, entsize 1) holding `m\0`; ld.lld
// honours it, GNU ld ignores it. The section's name, type, flags and entry size are the relocatable documents'
// data (`elf.dependentLibraries`), with what a DSS image link does with the names (`inImage: ignored`, and why):
//   * the shipped relocatable and static-library documents state the section, an image document none;
//   * the relocatable writer names every library the unit states, once, in order, in that section;
//   * the reader reads such a section back into the unit's requests, in order;
//   * a document stating no section refuses to write a unit that names a library, by name;
//   * no ELF document states an option a unit hands its linker: the relocatable writer refuses a module carrying
//     one, by name;
//   * the loader refuses an unbuilt verdict (`honoured`), a block with no reason, and the block on an image
//     document;
//   * two units naming one library name it once;
//   * NATIVE (Linux x86_64 with ld.lld): ld.lld searches the library a DSS object names, and the image runs.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/unit_linker_requests.hpp"
#include "link/format/elf.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "format_reject_support.hpp"
#include "link_test_support.hpp"
#include "arm_verdict_ledger.hpp"   // findOnPath (the native cell's `cc` and `ld.lld`)
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
// `captureCmd` / `tailOf` -- the shared spelling of a host tool's command with both streams kept. Included by
// relative path, as test_coff_object_reader.cpp includes it: `tests/core` is not on this target's include path.
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;
using dss::link_format::test::rejectSummary;

namespace {

constexpr std::uint32_t kShtLlvmDependentLibraries = 0x6fff4c04u;   // as clang writes it
constexpr std::uint64_t kShfMergeStrings           = 0x30u;         // SHF_MERGE | SHF_STRINGS

[[nodiscard]] std::shared_ptr<TargetSchema> target(char const* name) {
    auto t = TargetSchema::loadShipped(name);
    if (!t.has_value()) {
        ADD_FAILURE() << "TargetSchema::loadShipped(" << name << ") failed";
        return nullptr;
    }
    return std::move(t).value();
}

[[nodiscard]] std::shared_ptr<ObjectFormatSchema> format(char const* name) {
    auto f = ObjectFormatSchema::loadShipped(name);
    if (!f.has_value()) {
        ADD_FAILURE() << "ObjectFormatSchema::loadShipped(" << name << ") failed: " << rejectSummary(f);
        return nullptr;
    }
    return std::move(f).value();
}

[[nodiscard]] std::string documentText(char const* stem) {
    auto const root = dss::test::findConfigRoot();
    if (!root.has_value()) return {};
    std::ifstream in{*root / "object-formats" / (std::string{stem} + ".format.json"), std::ios::binary};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A one-function unit (`ret`) that names `libraries` as libraries its final link must search.
[[nodiscard]] AssembledModule unitNaming(std::vector<std::string> libraries) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{42};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    mod.linkerRequests.dependentLibraries = std::move(libraries);
    return mod;
}

struct Section {
    std::uint32_t type = 0;
    std::uint64_t flags = 0;
    std::uint64_t entrySize = 0;
    std::vector<std::uint8_t> body;
};

// The section named `name` in an ELF64 object's header table, or nullopt.
[[nodiscard]] std::optional<Section> sectionNamed(std::vector<std::uint8_t> const& bytes, std::string const& name) {
    std::uint64_t const shoff    = readU64LE(bytes, 40);
    std::uint16_t const shnum    = readU16LE(bytes, 60);
    std::uint16_t const shstrndx = readU16LE(bytes, 62);
    std::uint64_t const shstrOff = readU64LE(bytes, shoff + shstrndx * 64u + 24u);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const h = shoff + i * 64u;
        std::string read;
        for (std::size_t p = shstrOff + readU32LE(bytes, h); p < bytes.size() && bytes[p] != 0; ++p)
            read.push_back(static_cast<char>(bytes[p]));
        if (read != name) continue;
        Section s;
        s.type      = readU32LE(bytes, h + 4);
        s.flags     = readU64LE(bytes, h + 8);
        std::uint64_t const off  = readU64LE(bytes, h + 24);
        std::uint64_t const size = readU64LE(bytes, h + 32);
        s.entrySize = readU64LE(bytes, h + 56);
        s.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(off),
                      bytes.begin() + static_cast<std::ptrdiff_t>(off + size));
        return s;
    }
    return std::nullopt;
}

} // namespace

TEST(ElfDependentLibraries, TheRelocatableDocumentsStateTheSectionAndAnImageDocumentNone) {
    for (char const* name : {"elf64-x86_64-linux", "elf64-aarch64-linux", "elf64-x86_64-linux-staticlib",
                             "elf64-aarch64-linux-staticlib"}) {
        auto const f = format(name);
        ASSERT_NE(f, nullptr);
        auto const& block = f->elf().dependentLibraries;
        ASSERT_TRUE(block.has_value()) << name << " states no dependent-libraries section";
        EXPECT_EQ(block->section, ".deplibs") << name;
        EXPECT_EQ(block->type, kShtLlvmDependentLibraries) << name;
        EXPECT_EQ(block->flags, kShfMergeStrings) << name;
        EXPECT_EQ(block->entrySize, 1u) << name;
        EXPECT_FALSE(block->inImageReason.empty()) << name;
    }
    for (char const* name : {"elf64-x86_64-linux-exec", "elf64-aarch64-linux-pie"}) {
        auto const f = format(name);
        ASSERT_NE(f, nullptr);
        EXPECT_FALSE(f->elf().dependentLibraries.has_value()) << name << ": an image document states the section";
    }
}

TEST(ElfDependentLibraries, TheRelocatableWriterNamesEveryLibraryInItsSection) {
    auto const t = target("x86_64");
    auto const f = format("elf64-x86_64-linux");
    ASSERT_TRUE(t && f);
    DiagnosticReporter rep;
    auto const bytes = elf::encode(unitNaming({"m", "dss lib"}), *t, *f, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto const s = sectionNamed(bytes, ".deplibs");
    ASSERT_TRUE(s.has_value()) << "the object names no dependent library";
    EXPECT_EQ(s->type, kShtLlvmDependentLibraries);
    EXPECT_EQ(s->flags, kShfMergeStrings);
    EXPECT_EQ(s->entrySize, 1u);
    std::string const body{s->body.begin(), s->body.end()};
    EXPECT_EQ(body, std::string("m\0dss lib\0", 10)) << "the libraries are not written each NUL-terminated, in order";

    DiagnosticReporter none;
    auto const plain = elf::encode(unitNaming({}), *t, *f, none);
    ASSERT_EQ(none.errorCount(), 0u);
    EXPECT_FALSE(sectionNamed(plain, ".deplibs").has_value()) << "a unit naming no library writes the section";
}

TEST(ElfDependentLibraries, TheReaderReadsTheSectionBackInOrder) {
    auto const t = target("x86_64");
    auto const f = format("elf64-x86_64-linux");
    ASSERT_TRUE(t && f);
    DiagnosticReporter rep;
    auto const bytes = elf::encode(unitNaming({"m", "dss lib", "z"}), *t, *f, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    DiagnosticReporter readRep;
    auto const read = elf::readRelocatableObject(bytes, *t, *f, readRep, CompilationUnitId{7});
    ASSERT_TRUE(read.has_value()) << (readRep.all().empty() ? "" : readRep.all()[0].actual);
    EXPECT_EQ(read->linkerRequests.dependentLibraries, (std::vector<std::string>{"m", "dss lib", "z"}))
        << "the object's dependent libraries are not read back in order";
}

TEST(ElfDependentLibraries, ADocumentStatingNoSectionRefusesAUnitNamingALibrary) {
    auto const t = target("x86_64");
    ASSERT_NE(t, nullptr);
    nlohmann::json doc = nlohmann::json::parse(documentText("elf64-x86_64-linux"));
    doc["elf"].erase("dependentLibraries");
    auto const f = ObjectFormatSchema::loadFromText(doc.dump(), "elf64 relocatable stating no dependent libraries");
    ASSERT_TRUE(f.has_value()) << rejectSummary(f);
    DiagnosticReporter rep;
    auto const bytes = elf::encode(unitNaming({"m"}), *t, **f, rep);
    EXPECT_TRUE(bytes.empty()) << "the object was written without the library it names";
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat && d.actual.find("'m'") != std::string::npos
            && d.actual.find("dependentLibraries") != std::string::npos)
            named = true;
    }
    EXPECT_TRUE(named) << "the refusal does not name the library and the missing section";
}

// P69 fold 3f: no ELF document states an option a unit hands its linker, so the relocatable writer refuses a module
// that still carries one, by name, rather than write an object without it.
TEST(ElfDependentLibraries, TheRelocatableWriterRefusesAnOptionNoElfDocumentStates) {
    auto const t = target("x86_64");
    ASSERT_NE(t, nullptr);
    auto const f = format("elf64-x86_64-linux");
    ASSERT_NE(f, nullptr);
    AssembledModule mod = unitNaming({});
    mod.linkerRequests.linkerOptions = {{"-framework", "CoreFoundation"}};
    DiagnosticReporter rep;
    EXPECT_TRUE(elf::encode(mod, *t, *f, rep).empty()) << "the object was written without the option";
    bool named = false;
    for (auto const& d : rep.all()) {
        named = named || (d.code == DiagnosticCode::K_NoMatchingObjectFormat
                          && d.actual.find("'-framework CoreFoundation'") != std::string::npos);
    }
    EXPECT_TRUE(named) << "the refusal does not name the option";
}

TEST(ElfDependentLibraries, TheLoaderRefusesAnUnbuiltVerdictAMissingReasonAndAnImageDocument) {
    struct Case {
        char const* label;
        char const* stem;
        void (*mutate)(nlohmann::json&);
        char const* path;
    };
    Case const kCases[] = {
        {"a verdict no link has built", "elf64-x86_64-linux",
         [](nlohmann::json& d) { d["elf"]["dependentLibraries"]["inImage"] = "honoured"; },
         "/elf/dependentLibraries/inImage"},
        {"no reason", "elf64-x86_64-linux",
         [](nlohmann::json& d) { d["elf"]["dependentLibraries"].erase("reason"); },
         "/elf/dependentLibraries/reason"},
        {"a key the block does not know", "elf64-x86_64-linux",
         [](nlohmann::json& d) { d["elf"]["dependentLibraries"]["align"] = 1; },
         "/elf/dependentLibraries/align"},
        {"an image document", "elf64-x86_64-linux-exec",
         [](nlohmann::json& d) {
             d["elf"]["dependentLibraries"] = nlohmann::json::parse(
                 R"({"section": ".deplibs", "type": 1879002116, "flags": 48, "entrySize": 1,
                     "inImage": "ignored", "reason": "a reason"})");
         },
         "/elf/dependentLibraries"},
    };
    for (Case const& c : kCases) {
        SCOPED_TRACE(c.label);
        nlohmann::json doc = nlohmann::json::parse(documentText(c.stem));
        c.mutate(doc);
        auto const r = ObjectFormatSchema::loadFromText(doc.dump(), c.label);
        ASSERT_FALSE(r.has_value()) << "the document loads";
        EXPECT_GE(countAtPath(r, c.path), 1u) << rejectSummary(r);
    }
}

// THE NATIVE CELL: a DSS relocatable object naming `m` in its section is linked by ld.lld (through the host's `cc`,
// `-fuse-ld=lld`), which SEARCHES the library -- the image needs libm.so.6 -- and runs to 42; the same object naming
// nothing links no libm. `--no-as-needed` keeps a library nothing references in the image's needs, so the need is
// ld.lld's answer to the section alone. The unit's one function is `_start` (exit 42 by the Linux system call), so
// no C runtime start file is needed.
TEST(ElfDependentLibraries, LdLldSearchesTheLibraryADssObjectNames) {
#if defined(__linux__) && defined(__x86_64__)
    std::string const cc  = test_support::findOnPath("cc");
    std::string const lld = test_support::findOnPath("ld.lld");
    if (cc.empty() || lld.empty())
        GTEST_SKIP() << "this host has no `cc` or no `ld.lld` on PATH (the Ubuntu x86_64 leg has both)";
    auto const t = target("x86_64");
    auto const f = format("elf64-x86_64-linux");
    ASSERT_TRUE(t && f);
    auto const objectNaming = [&](std::vector<std::string> libraries) {
        AssembledModule mod;
        mod.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{42};
        // _start: mov edi, 42 ; mov eax, 60 (exit) ; syscall
        fn.bytes = {0xBF, 0x2A, 0x00, 0x00, 0x00, 0xB8, 0x3C, 0x00, 0x00, 0x00, 0x0F, 0x05};
        mod.functions.push_back(std::move(fn));
        mod.symbols.push_back(
            ModuleSymbol{SymbolId{42}, "_start", SymbolBinding::Global, SymbolVisibility::Default});
        mod.linkerRequests.dependentLibraries = std::move(libraries);
        DiagnosticReporter rep;
        auto bytes = elf::encode(mod, *t, *f, rep);
        EXPECT_EQ(rep.errorCount(), 0u);
        return bytes;
    };
    test_support::ScratchDir scratch{test_support::Location::Temp, "elf-dependent-libraries"};
    auto const linkAndRun = [&](char const* stem, std::vector<std::uint8_t> const& object) {
        auto const obj = scratch.path() / (std::string{stem} + ".o");
        auto const exe = scratch.path() / stem;
        {
            std::ofstream out{obj, std::ios::binary};
            out.write(reinterpret_cast<char const*>(object.data()), static_cast<std::streamsize>(object.size()));
        }
        // The host's `cc` runs through the shell, both streams kept (`captureCmd`), as every native cell runs a host
        // tool: `runBinary` is for a binary the test PRODUCED -- it makes its path executable first, which a tool the
        // test does not own refuses (chmod of the host's `cc`: EPERM, MEASURED on the Ubuntu leg).
        auto const log = scratch.path() / (std::string{stem} + ".link.log");
        int const linked = std::system(test_support::native_probe::captureCmd(
            "\"" + cc + "\" -fuse-ld=lld -nostartfiles -Wl,--no-as-needed \"" + obj.string() + "\" -o \""
                + exe.string() + "\"",
            log).c_str());
        EXPECT_EQ(linked, 0) << stem << ": " << test_support::native_probe::tailOf(log, 20, "the link");
        auto const ran = test_support::runBinary(exe);
        std::ifstream in{exe, std::ios::binary};
        std::string const image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return std::pair<std::uint32_t, std::string>{ran.exitCode, image};
    };
    auto const named = linkAndRun("names_m", objectNaming({"m"}));
    EXPECT_EQ(named.first, 42u);
    EXPECT_NE(named.second.find("libm.so.6"), std::string::npos)
        << "ld.lld did not search the library the DSS object names";
    auto const plain = linkAndRun("names_none", objectNaming({}));
    EXPECT_EQ(plain.first, 42u);
    EXPECT_EQ(plain.second.find("libm.so.6"), std::string::npos) << "the control links libm without being asked";
#else
    GTEST_SKIP() << "the cell links an x86_64 Linux object with ld.lld; it runs on Linux x86_64 hosts";
#endif
}

TEST(ElfDependentLibraries, TwoUnitsNamingOneLibraryNameItOnce) {
    UnitLinkerRequests into;
    UnitLinkerRequests a;
    a.dependentLibraries = {"m", "z"};
    UnitLinkerRequests b;
    b.dependentLibraries = {"z", "dl"};
    appendUnitLinkerRequests(into, a);
    appendUnitLinkerRequests(into, b);
    EXPECT_EQ(into.dependentLibraries, (std::vector<std::string>{"m", "z", "dl"}))
        << "a library two units name is not named once, at its first statement";
    EXPECT_FALSE(into.empty());
}
