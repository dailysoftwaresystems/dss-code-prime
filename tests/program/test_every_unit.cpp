// THE ONE HOME OF "A SEVERAL-UNIT STEP REPORTS EVERY UNIT BEFORE IT FAILS" -- src/core/substrate/every_unit.hpp's verb
// `runEveryUnit` (P69 H6b, the review of H6's N1; the orchestrator's ledger 550), pinned over both cells: every unit
// failing, and a valid unit before an invalid one (with a valid one after it) -- each unit asked, the step failed --
// and the control, every unit succeeding (pure: no build, no host). Then the call sites' cells: the input-language
// claim's; since P69 fold 3e the C route's archive members, the input archives' members, the named objects and an
// assembly unit's sources (sites (a), (c) and (e) of the orchestrator's ledger 550), each over every archive document;
// the assembly route's archive branch and the archive member link's are in
// tests/program/test_archive_definition_visibility.cpp (EveryUnitsErrorsAreReportedAndABuildAnyUnitFailedWritesNoArchive,
// AnArchivesRequestIsRefusedOnceForTheWholeArchive).

#include "core/substrate/every_unit.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/mangling/c_mangle.hpp"   // applyCMangling: a name as the binary spells it
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/compile_pipeline.hpp"   // pullStaticArchiveMembers, pullEntryArchiveMembers
#include "program/dump_predefined_macros.hpp"
#include "program/git_acquire.hpp"
#include "program/program.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

TEST(EveryUnit, EveryUnitFailingIsEachAskedAndTheStepFails) {
    std::vector<std::size_t> asked;
    bool const every = dss::substrate::runEveryUnit(3, [&](std::size_t i) {
        asked.push_back(i);
        return false;
    });
    EXPECT_FALSE(every) << "three failed units were taken for success";
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1, 2})) << "a failed unit stopped the units after it";
}

TEST(EveryUnit, AValidUnitBeforeAnInvalidOneIsAskedAndTheStepFails) {
    std::vector<std::size_t> asked;
    bool const every = dss::substrate::runEveryUnit(3, [&](std::size_t i) {
        asked.push_back(i);
        return i != 1;   // valid, invalid, valid
    });
    EXPECT_FALSE(every) << "an invalid unit between two valid ones was taken for success";
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1, 2})) << "the invalid unit stopped the valid one after it";
}

TEST(EveryUnit, EveryUnitSucceedingSucceedsTheControl) {
    std::vector<std::size_t> asked;
    EXPECT_TRUE(dss::substrate::runEveryUnit(2, [&](std::size_t i) {
        asked.push_back(i);
        return true;
    }));
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1}));
    EXPECT_TRUE(dss::substrate::runEveryUnit(0, [](std::size_t) { return false; })) << "no unit asked is no unit failed";
}

// THE INPUT-LANGUAGE CLAIM (program.cpp `resolveGrammarForTarget`): with no `--language`, the language the target
// declares must claim every input, and EVERY input it does not claim is named before the build fails -- two inputs of
// extensions no shipped language claims, two D_UnknownFileExtension, each naming its own file.
TEST(EveryUnit, EveryInputTheTargetsLanguageDoesNotClaimIsNamedBeforeTheBuildFails) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-claim"};
    std::vector<std::string> sources;
    for (char const* name : {"first.zz1", "second.zz2"}) {
        std::filesystem::path const file = scratch.path() / name;
        std::ofstream(file, std::ios::binary) << "x\n";
        sources.push_back(file.string());
    }
    dss::Program p;
    p.setOutputDir(scratch.path());
    dss::DiagnosticReporter rep;
    EXPECT_NE(p.compileUnits(sources, "", {"x86_64:elf64-x86_64-linux-exec"}, rep), 0)
        << "two inputs no language claims were built";
    std::string said;
    for (auto const& d : rep.all()) said += "\n  " + d.actual;
    for (char const* name : {"first.zz1", "second.zz2"}) {
        bool named = false;
        for (auto const& d : rep.all()) {
            if (d.code == dss::DiagnosticCode::D_UnknownFileExtension && d.actual.find(name) != std::string::npos)
                named = true;
        }
        EXPECT_TRUE(named) << "the unclaimed input " << name << " was not named; the build said:" << said;
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// THREE MORE CALL SITES (P69 fold 3e, lane `xa`; sites (a), (c) and (e) of the orchestrator's ledger 550): the C
// route's static-archive member build (program.cpp), the reads of an input archive's members and of the named object
// inputs (compile_pipeline.cpp `extractStaticArchiveMembers`, `readObjectInputModules`), and an assembly unit's
// per-source loop (compile_pipeline.cpp `assembleAsmUnit`). Each is pinned over both cells -- every unit failing, and a
// valid unit before an invalid one -- and a build any unit failed writes no artifact.
// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
namespace {

namespace fs = std::filesystem;

void writeText(fs::path const& path, std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
}

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeBytes(fs::path const& path, std::vector<std::uint8_t> const& bytes) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::string allSaid(dss::DiagnosticReporter const& rep) {
    std::string said;
    for (auto const& d : rep.all()) said += "\n  " + d.actual;
    return said;
}

[[nodiscard]] std::size_t countSaying(dss::DiagnosticReporter const& rep, std::string_view text) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.actual.find(text) != std::string::npos) ++n;
    }
    return n;
}

// Every file under `dir` a build would have written as its artifact -- an `ar` archive or an object of any family,
// read by its first bytes -- and the names a failed build may NOT leave behind.
[[nodiscard]] std::vector<std::string> artifactsUnder(fs::path const& dir) {
    std::vector<std::string> found;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator{dir, ec}; !ec && it != fs::recursive_directory_iterator{};
         it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        auto const b = bytesOf(it->path());
        bool const ar    = b.size() >= 8 && std::string(b.begin(), b.begin() + 8) == "!<arch>\n";
        bool const elf   = b.size() >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
        bool const macho = b.size() >= 4 && b[0] == 0xcf && b[1] == 0xfa && b[2] == 0xed && b[3] == 0xfe;
        bool const coff  = b.size() >= 2 && ((b[0] == 0x64 && b[1] == 0x86) || (b[0] == 0x64 && b[1] == 0xaa));
        if (ar || elf || macho || coff) found.push_back(it->path().filename().string());
    }
    return found;
}

// One archive document of the universe: its target, the archive, relocatable and assembly documents, the dialect's
// valid one-function body and its function marker, and how to make an object of the family unreadable past its
// header (the field the identity probe never reads, pointed past the end of the file).
struct UnitDocument {
    std::string_view arch;
    std::string_view archive;
    std::string_view relocatable;
    std::string_view assembly;
    std::string_view asmBody;
    std::string_view marker;
};
constexpr UnitDocument kUnitDocuments[] = {
    {"x86_64", "elf64-x86_64-linux-staticlib", "elf64-x86_64-linux", "asm-x86_64-att", "\tmovl\t$1, %eax\n\tret\n",
     "@function"},
    {"x86_64", "pe64-x86_64-windows-staticlib", "pe64-x86_64-windows", "asm-x86_64-att", "\tmovl\t$1, %eax\n\tret\n",
     "@function"},
    {"x86_64", "macho64-x86_64-darwin-staticlib", "macho64-x86_64-darwin", "asm-x86_64-att",
     "\tmovl\t$1, %eax\n\tret\n", "@function"},
    {"arm64", "elf64-aarch64-linux-staticlib", "elf64-aarch64-linux", "asm-arm64-gas", "\tmov\tw0, #1\n\tret\n",
     "%function"},
    {"arm64", "macho64-arm64-darwin-staticlib", "macho64-arm64-darwin", "asm-arm64-gas", "\tmov\tw0, #1\n\tret\n",
     "%function"},
};

[[nodiscard]] std::string spec(std::string_view arch, std::string_view doc) {
    return std::string{arch} + ":" + std::string{doc};
}

// A DSS object of `int <name>(void)` for the document `doc`: its bytes, or empty with a failure recorded.
[[nodiscard]] std::vector<std::uint8_t> objectOf(fs::path const& dir, std::string_view arch, std::string_view doc,
                                                 std::string const& name) {
    fs::create_directories(dir);
    fs::path const src = dir / (name + ".c");
    writeText(src, "int " + name + "(void) { return 7; }\n");
    dss::Program p;
    p.setOutputDir(dir);
    dss::DiagnosticReporter rep;
    if (p.compileFiles({src.string()}, "c", {spec(arch, doc)}, rep) != 0 || p.artifactPaths().size() != 1
        || !p.artifactPaths().front().has_value()) {
        ADD_FAILURE() << "the object of " << name << " for " << doc << " did not build:" << allSaid(rep);
        return {};
    }
    return bytesOf(*p.artifactPaths().front());
}

void putU32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void putU64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
[[nodiscard]] std::uint32_t getU32(std::vector<std::uint8_t> const& b, std::size_t at) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= std::uint32_t{b[at + i]} << (8 * i);
    return v;
}

// The object made unreadable PAST its header, so it passes the link's identity check (magic and machine) and is
// refused by its format's reader: ELF's section-header offset, COFF's symbol-table pointer, Mach-O's LC_SYMTAB symbol
// offset -- each pointed past the end of the file. The family is read off the magic.
// A Mach-O DYNAMIC LIBRARY's surface is its EXPORT TRIE, which the FF1 reader reads in preference to LC_SYMTAB
// whenever the image carries one (READ: src/ffi/binary_readers/macho_reader.cpp, "the export-trie path"; ✔MEASURED
// on the Mac leg, P69 fold 3e: DSS's dylib with LC_SYMTAB alone pointed past its end was READ, and the build bound
// against it succeeded) -- so the trie's offset, in LC_DYLD_INFO(_ONLY) or LC_DYLD_EXPORTS_TRIE, is pointed past the
// end too, and the reader refuses the library for the trie.
[[nodiscard]] std::vector<std::uint8_t> unreadable(std::vector<std::uint8_t> b) {
    constexpr std::uint32_t kPastTheEnd = 0x7fff0000u;
    if (b.size() >= 64 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        putU64(b, 0x28, kPastTheEnd);   // e_shoff
    } else if (b.size() >= 32 && getU32(b, 0) == 0xfeedfacfu) {
        std::uint32_t const ncmds = getU32(b, 16);
        std::size_t         at    = 32;   // mach_header_64
        bool                found = false;
        for (std::uint32_t i = 0; i < ncmds && at + 16 <= b.size(); ++i) {
            std::uint32_t const cmd = getU32(b, at);
            if (cmd == 0x2u) {   // LC_SYMTAB: cmd, cmdsize, symoff, nsyms, stroff, strsize
                putU32(b, at + 8, kPastTheEnd);
                found = true;
            } else if ((cmd == 0x22u || cmd == 0x80000022u) && at + 48 <= b.size() && getU32(b, at + 44) != 0u) {
                putU32(b, at + 40, kPastTheEnd);   // LC_DYLD_INFO(_ONLY): export_off (export_size at +44)
            } else if (cmd == 0x80000033u && at + 16 <= b.size()) {
                putU32(b, at + 8, kPastTheEnd);    // LC_DYLD_EXPORTS_TRIE: dataoff
            }
            at += getU32(b, at + 4);
        }
        if (!found) ADD_FAILURE() << "the Mach-O object has no LC_SYMTAB to point past its end";
    } else if (b.size() >= 20) {
        putU32(b, 8, kPastTheEnd);      // COFF PointerToSymbolTable
    } else {
        ADD_FAILURE() << "an object of " << b.size() << " bytes";
    }
    return b;
}

// An `ar` archive of `members` (GNU names, no symbol index): what an input archive is, byte for byte.
[[nodiscard]] std::vector<std::uint8_t> arOf(std::vector<std::pair<std::string, std::vector<std::uint8_t>>> const& members) {
    std::string out = "!<arch>\n";
    for (auto const& [name, bytes] : members) {
        char header[61];
        std::snprintf(header, sizeof header, "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", (name + "/").c_str(), "0", "0", "0",
                      "644", bytes.size());
        out.append(header, 60);
        out.append(bytes.begin(), bytes.end());
        if (bytes.size() % 2 != 0) out.push_back('\n');
    }
    return {out.begin(), out.end()};
}

// What every reader says when it refuses an object (each format's own prefix ends in this).
constexpr std::string_view kReaderRefusal = "readRelocatableObject:";

}  // namespace

// SITE (a): a several-unit C build into an archive builds EVERY member, and reports what each says, before it fails.
// The member's refusal is its back half's -- an inline `asm` of an unknown mnemonic, refused where MIR becomes LIR, on
// every archive document -- so the front half, which already reports every unit, cannot answer for it.
TEST(EveryUnit, EveryMemberOfACArchiveIsBuiltBeforeTheBuildFails) {
    for (UnitDocument const& d : kUnitDocuments) {
        for (bool const mixed : {false, true}) {
            SCOPED_TRACE(std::string{d.archive} + (mixed ? " / one valid unit, then one invalid" : " / two invalid"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-c-archive"};
            std::vector<std::string> sources;
            std::vector<std::string> refused;
            for (char const* stem : {"first", "second"}) {
                std::string const fn = std::string{stem} + "_api";
                std::string text = "int " + fn + "(void) {\n";
                if (!(mixed && std::string_view{stem} == "first")) {
                    std::string const mnemonic = std::string{"nosuchmnemonic_"} + stem;
                    text += "    __asm__ volatile(\"" + mnemonic + "\");\n";
                    refused.push_back("'" + mnemonic + "'");
                }
                text += "    return 1;\n}\n";
                fs::path const file = scratch.path() / (std::string{stem} + ".c");
                writeText(file, text);
                sources.push_back(file.string());
            }
            fs::path const out = scratch.path() / "out";
            dss::Program p;
            p.setOutputDir(out);
            dss::DiagnosticReporter rep;
            EXPECT_NE(p.compileUnits(sources, "c", {spec(d.arch, d.archive)}, rep), 0)
                << "a build with an invalid unit succeeded" << allSaid(rep);
            for (std::string const& mnemonic : refused) {
                EXPECT_GE(countSaying(rep, mnemonic), 1u)
                    << "the unit whose `asm` writes " << mnemonic << " was not refused by its own diagnostic; the "
                       "build said:" << allSaid(rep);
            }
            EXPECT_EQ(artifactsUnder(out), std::vector<std::string>{}) << "the failed build wrote an artifact";
            for (auto const& artifact : p.artifactPaths()) {
                EXPECT_FALSE(artifact.has_value()) << "the failed build names an artifact: " << artifact->string();
            }
        }
    }
}

// SITE (c), the input archives: a static library built with input archives packages EVERY member of every one, so it
// reads every member of every archive, and reports what each refusal says, before it fails. Two archives of two
// unreadable members each are four refusals; one archive of a readable member and one of a readable member then an
// unreadable one is one refusal and no library.
TEST(EveryUnit, EveryInputArchiveAndEveryMemberIsReadBeforeTheBuildFails) {
    for (UnitDocument const& d : kUnitDocuments) {
        for (bool const mixed : {false, true}) {
            SCOPED_TRACE(std::string{d.archive} + (mixed ? " / readable members, then one unreadable" : " / all unreadable"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-input-archive"};
            fs::path const objs = scratch.path() / "objs";
            auto const good = objectOf(objs, d.arch, d.relocatable, "good_api");
            auto const bad  = unreadable(objectOf(objs, d.arch, d.relocatable, "bad_api"));
            ASSERT_FALSE(good.empty() || bad.empty());
            std::vector<std::vector<std::pair<std::string, std::vector<std::uint8_t>>>> archives;
            if (mixed) {
                archives = {{{"g1.o", good}}, {{"g2.o", good}, {"b2.o", bad}}};
            } else {
                archives = {{{"b1.o", bad}, {"b2.o", bad}}, {{"b3.o", bad}, {"b4.o", bad}}};
            }
            std::vector<fs::path> inputs;
            for (std::size_t i = 0; i < archives.size(); ++i) {
                fs::path const a = scratch.path() / ("input" + std::to_string(i) + ".a");
                writeBytes(a, arOf(archives[i]));
                inputs.push_back(a);
            }
            fs::path const unit = scratch.path() / "unit.c";
            writeText(unit, "int unit_api(void) { return 1; }\n");
            fs::path const out = scratch.path() / "out";
            dss::Program p;
            p.setOutputDir(out);
            p.setResolveLibraries(inputs);
            dss::DiagnosticReporter rep;
            EXPECT_NE(p.compileFiles({unit.string()}, "c", {spec(d.arch, d.archive)}, rep), 0)
                << "a library with an unreadable input member was built" << allSaid(rep);
            EXPECT_EQ(countSaying(rep, kReaderRefusal), mixed ? 1u : 4u)
                << "each unreadable member is refused once, whatever came before it; the build said:" << allSaid(rep);
            // Each refusal names its member, as `archive(member)`: four members with one defect are four refusals.
            for (std::size_t i = 0; i < archives.size(); ++i) {
                for (auto const& [member, bytes] : archives[i]) {
                    if (bytes != bad) continue;
                    std::string const named = "input" + std::to_string(i) + ".a(" + member + "): ";
                    EXPECT_EQ(countSaying(rep, named), 1u)
                        << "the unreadable member is not refused once by name, '" << named << "'; the build said:"
                        << allSaid(rep);
                }
            }
            EXPECT_EQ(artifactsUnder(out), std::vector<std::string>{}) << "the failed build wrote an artifact";
            for (auto const& artifact : p.artifactPaths()) {
                EXPECT_FALSE(artifact.has_value()) << "the failed build names an artifact: " << artifact->string();
            }
        }
    }
}

// SITE (c), the named objects: EVERY object named to a build is read, and each refusal reported, before it fails.
TEST(EveryUnit, EveryObjectInputIsReadBeforeTheBuildFails) {
    for (UnitDocument const& d : kUnitDocuments) {
        for (bool const mixed : {false, true}) {
            SCOPED_TRACE(std::string{d.relocatable} + (mixed ? " / a readable object, then an unreadable one"
                                                             : " / two unreadable objects"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-object-input"};
            fs::path const objs = scratch.path() / "objs";
            auto const good = objectOf(objs, d.arch, d.relocatable, "good_api");
            auto const bad  = unreadable(objectOf(objs, d.arch, d.relocatable, "bad_api"));
            ASSERT_FALSE(good.empty() || bad.empty());
            std::vector<fs::path> inputs;
            for (std::size_t i = 0; i < 2; ++i) {
                fs::path const o = scratch.path() / ("input" + std::to_string(i) + ".o");
                writeBytes(o, (mixed && i == 0) ? good : bad);
                inputs.push_back(o);
            }
            fs::path const unit = scratch.path() / "unit.c";
            writeText(unit, "int unit_api(void) { return 1; }\n");
            fs::path const out = scratch.path() / "out";
            dss::Program p;
            p.setOutputDir(out);
            p.setResolveLibraries(inputs);
            dss::DiagnosticReporter rep;
            EXPECT_NE(p.compileFiles({unit.string()}, "c", {spec(d.arch, d.archive)}, rep), 0)
                << "a build with an unreadable object was built" << allSaid(rep);
            EXPECT_EQ(countSaying(rep, kReaderRefusal), mixed ? 1u : 2u)
                << "each unreadable object is refused once, whatever came before it; the build said:" << allSaid(rep);
            for (std::size_t i = mixed ? 1 : 0; i < 2; ++i) {   // each refusal names its object, by its path
                std::string const named = "input" + std::to_string(i) + ".o: ";
                EXPECT_EQ(countSaying(rep, named), 1u)
                    << "the unreadable object is not refused once by name, '" << named << "'; the build said:"
                    << allSaid(rep);
            }
            EXPECT_EQ(artifactsUnder(out), std::vector<std::string>{}) << "the failed build wrote an artifact";
            for (auto const& artifact : p.artifactPaths()) {
                EXPECT_FALSE(artifact.has_value()) << "the failed build names an artifact: " << artifact->string();
            }
        }
    }
}

// SITE (e): an assembly unit of several sources lowers and assembles EVERY source, and reports what each says, before
// it fails; and a unit of more than one source is refused whatever its first source holds -- a first source of DATA
// alone was dropped in silence (its data gone, the second source kept) until fold 3e asked the sources' count.
TEST(EveryUnit, EveryAssemblySourceOfAUnitIsAssembledBeforeTheUnitFails) {
    enum class Cell { TwoInvalid, ValidThenInvalid, DataAloneThenValid };
    for (UnitDocument const& d : kUnitDocuments) {
        for (Cell const cell : {Cell::TwoInvalid, Cell::ValidThenInvalid, Cell::DataAloneThenValid}) {
            SCOPED_TRACE(std::string{d.relocatable} + (cell == Cell::TwoInvalid         ? " / two invalid sources"
                                                       : cell == Cell::ValidThenInvalid ? " / one valid, then one invalid"
                                                                                        : " / data alone, then a function"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-asm-sources"};
            std::vector<std::string> sources;
            std::vector<std::string> refused;
            for (char const* stem : {"first", "second"}) {
                bool const isFirst = std::string_view{stem} == "first";
                std::string const fn = std::string{stem} + "_api";
                std::string text;
                if (cell == Cell::DataAloneThenValid && isFirst) {
                    text = "\t.data\n\t.globl\tfirst_datum\nfirst_datum:\n\t.byte\t42\n";
                } else {
                    text = "\t.text\n\t.globl\t" + fn + "\n\t.type\t" + fn + ", " + std::string{d.marker} + "\n" + fn
                         + ":\n";
                    bool const valid = cell == Cell::DataAloneThenValid || (cell == Cell::ValidThenInvalid && isFirst);
                    if (valid) {
                        text += d.asmBody;
                    } else {
                        std::string const mnemonic = std::string{"nosuchmnemonic_"} + stem;
                        text += "\t" + mnemonic + "\n";
                        refused.push_back("'" + mnemonic + "'");
                    }
                }
                fs::path const file = scratch.path() / (std::string{stem} + ".s");
                writeText(file, text);
                sources.push_back(file.string());
            }
            fs::path const out = scratch.path() / "out";
            dss::Program p;
            p.setOutputDir(out);
            dss::DiagnosticReporter rep;
            EXPECT_NE(p.compileFiles(sources, std::string{d.assembly}, {spec(d.arch, d.relocatable)}, rep), 0)
                << "a unit of two assembly sources was built" << allSaid(rep);
            for (std::string const& mnemonic : refused) {
                EXPECT_GE(countSaying(rep, mnemonic), 1u)
                    << "the source that writes " << mnemonic << " was not refused by its own diagnostic; the build "
                       "said:" << allSaid(rep);
            }
            if (cell == Cell::DataAloneThenValid) {
                EXPECT_EQ(countSaying(rep, "more than one assembly source"), 1u)
                    << "a first source of data alone was dropped in silence, the second kept; the build said:"
                    << allSaid(rep);
            }
            EXPECT_EQ(artifactsUnder(out), std::vector<std::string>{}) << "the failed build wrote an artifact";
            for (auto const& artifact : p.artifactPaths()) {
                EXPECT_FALSE(artifact.has_value()) << "the failed build names an artifact: " << artifact->string();
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// THE HOME'S GROWING FORM, AND THE SITES OF THE ORCHESTRATOR'S LEDGER 568 (P69 fold 3e, lane `xa`): the census of the
// class (every loop over units, inputs, members, libraries, archives, trees or targets that reports and stops) found
// seven more sites in src/program -- (f) the operator-named libraries' surfaces, (g) the archive pull's archive reads,
// (h) the pull search's member reads (its worklist, a GROWING set, and its commons round), (i) the entry pull's names,
// (j) the project's artifact-profile gate over its targets, (k) the macro dump's targets, (l) the dependency walk's
// entries -- and one in src/link, (m) the archive writer's members (tests/link/test_ar_writer.cpp). Each is pinned
// over both cells: every unit failing, and a valid unit before an invalid one.
// ════════════════════════════════════════════════════════════════════════════════════════════════════════════════

// The growing form asks every unit, those a step appends included, whatever an earlier one answered; it leaves the
// cursor past the last unit asked.
TEST(EveryUnit, EveryUnitOfAGrowingSetFailingIsEachAskedAndTheStepFails) {
    std::vector<std::size_t> units{0, 1};
    std::vector<std::size_t> asked;
    std::size_t              cursor = 0;
    bool const every = dss::substrate::runEveryUnitOfAGrowingSet(
        cursor, [&] { return units.size(); },
        [&](std::size_t i) {
            asked.push_back(i);
            if (i == 0) units.push_back(2);   // the first unit names a third
            return i == 2;                    // invalid, invalid, then the appended one valid
        });
    EXPECT_FALSE(every) << "two failed units were taken for success";
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1, 2})) << "a failed unit stopped the units after it, or the unit "
                                                              "a step appended was never asked";
    EXPECT_EQ(cursor, 3u) << "the cursor is not past the last unit asked";
}

// A valid unit before an invalid one: both asked, the step failed; then the set grows and a resumed search asks the
// new unit alone, from the cursor -- and every unit succeeding succeeds.
TEST(EveryUnit, AValidUnitBeforeAnInvalidOneOfAGrowingSetIsAskedAndTheStepFails) {
    std::vector<bool>        valid{true, false};
    std::vector<std::size_t> asked;
    std::size_t              cursor = 0;
    auto const step = [&](std::size_t i) {
        asked.push_back(i);
        return static_cast<bool>(valid[i]);
    };
    EXPECT_FALSE(dss::substrate::runEveryUnitOfAGrowingSet(cursor, [&] { return valid.size(); }, step))
        << "an invalid unit after a valid one was taken for success";
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1}));
    valid.push_back(true);
    EXPECT_TRUE(dss::substrate::runEveryUnitOfAGrowingSet(cursor, [&] { return valid.size(); }, step))
        << "a resumed search of one valid unit failed";
    EXPECT_EQ(asked, (std::vector<std::size_t>{0, 1, 2})) << "the resumed search asked an earlier unit again";
}

namespace {

// The archive document's image sibling, by the shipped names' one rule (`<base>-staticlib` -> `<base>-exec`).
[[nodiscard]] std::string execOf(UnitDocument const& d) {
    std::string doc{d.archive};
    return doc.substr(0, doc.size() - std::string_view{"-staticlib"}.size()) + "-exec";
}

// Every object member of an `ar` archive's bytes, as [offset, size) slices, in archive order (the linker members, the
// long-name table and any other non-object member skipped by their first bytes).
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> objectMembersOf(std::vector<std::uint8_t> const& b) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (b.size() < 8 || std::string(b.begin(), b.begin() + 8) != "!<arch>\n") return out;
    for (std::size_t at = 8; at + 60 <= b.size();) {
        std::size_t size = 0;
        for (std::size_t k = 48; k < 58 && b[at + k] >= '0' && b[at + k] <= '9'; ++k) {
            size = size * 10 + static_cast<std::size_t>(b[at + k] - '0');
        }
        std::size_t const data = at + 60;
        if (size > b.size() - data) break;
        bool const elf   = size >= 4 && b[data] == 0x7f && b[data + 1] == 'E';
        bool const macho = size >= 4 && getU32(b, data) == 0xfeedfacfu;
        bool const coff  = size >= 2 && b[data] == 0x64 && (b[data + 1] == 0x86 || b[data + 1] == 0xaa);
        if (elf || macho || coff) out.emplace_back(data, size);
        at = data + size + (size & 1u);
    }
    return out;
}

// A DSS static library of `int first_api(void)` and `int second_api(void)` for `d`'s archive document, with each
// member `unreadableMember` marks made unreadable past its header (the identity check reads the header alone): its
// path, or empty with a failure recorded.
[[nodiscard]] fs::path libraryOfTwo(fs::path const& dir, UnitDocument const& d, std::vector<bool> const& unreadableMember) {
    fs::create_directories(dir);
    std::vector<std::string> sources;
    for (char const* fn : {"first_api", "second_api"}) {
        fs::path const src = dir / (std::string{fn} + ".c");
        writeText(src, "int " + std::string{fn} + "(void) { return 7; }\n");
        sources.push_back(src.string());
    }
    dss::Program p;
    p.setOutputDir(dir / "lib");
    dss::DiagnosticReporter rep;
    if (p.compileUnits(sources, "c", {spec(d.arch, d.archive)}, rep) != 0 || p.artifactPaths().size() != 1
        || !p.artifactPaths().front().has_value()) {
        ADD_FAILURE() << "the library of two for " << d.archive << " did not build:" << allSaid(rep);
        return {};
    }
    fs::path const lib = *p.artifactPaths().front();
    auto bytes = bytesOf(lib);
    auto const members = objectMembersOf(bytes);
    if (members.size() != 2) {
        ADD_FAILURE() << d.archive << ": the library holds " << members.size() << " object members, not two";
        return {};
    }
    for (std::size_t m = 0; m < 2; ++m) {
        if (!unreadableMember[m]) continue;
        auto const [at, size] = members[m];
        auto const patched = unreadable(std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                                                  bytes.begin() + static_cast<std::ptrdiff_t>(at + size)));
        std::copy(patched.begin(), patched.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
    }
    fs::path const out = dir / ("two" + lib.extension().string());
    writeBytes(out, bytes);
    return out;
}

// An archive the `ar` reader refuses: the magic, then a member header whose size field is not a number.
[[nodiscard]] std::vector<std::uint8_t> corruptArchive() {
    std::string s = "!<arch>\n";
    s += "member.o/       0           0     0     644     zz        `\n";
    s += "xx";
    return {s.begin(), s.end()};
}

struct Loaded {
    std::shared_ptr<dss::TargetSchema const>       target;
    std::shared_ptr<dss::ObjectFormatSchema const> format;
};

[[nodiscard]] Loaded loadedFor(std::string_view arch, std::string_view doc) {
    Loaded l;
    if (auto t = dss::TargetSchema::loadShipped(std::string{arch}); t.has_value()) l.target = *t;
    if (auto f = dss::ObjectFormatSchema::loadShipped(doc); f.has_value()) l.format = *f;
    EXPECT_TRUE(l.target && l.format) << "the shipped " << arch << " target and " << doc << " documents";
    return l;
}

}  // namespace

// SITE (g): the archive pull reads EVERY archive before it fails (`pullStaticArchiveMembers`, called directly: the
// link's identity check refuses a corrupt archive first on the driver's routes, so only a direct call reaches the
// pull's own read).
TEST(EveryUnit, EveryArchiveOfTheArchivePullIsReadBeforeThePullFails) {
    for (UnitDocument const& d : kUnitDocuments) {
        for (bool const mixed : {false, true}) {
            SCOPED_TRACE(std::string{d.archive} + (mixed ? " / a readable archive, then a corrupt one" : " / two corrupt"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-pull-archives"};
            Loaded const l = loadedFor(d.arch, execOf(d));
            ASSERT_TRUE(l.target && l.format);
            std::vector<fs::path> archives;
            if (mixed) {
                fs::path const good = libraryOfTwo(scratch.path() / "good", d, {false, false});
                ASSERT_FALSE(good.empty());
                archives.push_back(good);
            } else {
                writeBytes(scratch.path() / "corrupt_one.a", corruptArchive());
                archives.push_back(scratch.path() / "corrupt_one.a");
            }
            writeBytes(scratch.path() / "corrupt_two.a", corruptArchive());
            archives.push_back(scratch.path() / "corrupt_two.a");
            dss::DiagnosticReporter rep;
            auto const pulled = dss::pullStaticArchiveMembers({}, archives, {}, *l.target, *l.format, rep);
            EXPECT_FALSE(pulled.has_value()) << "a pull over a corrupt archive succeeded";
            for (char const* name : {"corrupt_one.a", "corrupt_two.a"}) {
                if (mixed && std::string_view{name} == "corrupt_one.a") continue;
                EXPECT_GE(countSaying(rep, name), 1u)
                    << "the corrupt archive " << name << " was not refused by name; the pull said:" << allSaid(rep);
            }
        }
    }
}

// SITE (h): an image build's archive search reads EVERY member it reaches -- a member it cannot read is refused and the
// search goes on for the names the others answer -- and fails at its fixed point, before any link: two unreadable
// members are two refusals, a readable member then an unreadable one is one, and no name is reported undefined.
TEST(EveryUnit, EveryMemberTheArchiveSearchReachesIsReadBeforeTheSearchFails) {
    for (UnitDocument const& d : kUnitDocuments) {
        for (bool const mixed : {false, true}) {
            SCOPED_TRACE(execOf(d) + (mixed ? " / a readable member, then an unreadable one" : " / both unreadable"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-pull-members"};
            fs::path const lib = libraryOfTwo(scratch.path() / "lib", d, {!mixed, true});
            ASSERT_FALSE(lib.empty());
            fs::path const unit = scratch.path() / "main.c";
            writeText(unit, "int first_api(void);\nint second_api(void);\n"
                            "int main(void) { return first_api() + second_api(); }\n");
            fs::path const out = scratch.path() / "out";
            dss::Program p;
            p.setOutputDir(out);
            p.setResolveLibraries(std::vector<fs::path>{lib});
            dss::DiagnosticReporter rep;
            EXPECT_NE(p.compileFiles({unit.string()}, "c", {spec(d.arch, execOf(d))}, rep), 0)
                << "an image of an unreadable archive member was built" << allSaid(rep);
            EXPECT_EQ(countSaying(rep, kReaderRefusal), mixed ? 1u : 2u)
                << "each unreadable member is refused once, whatever came before it; the build said:" << allSaid(rep);
            EXPECT_EQ(countSaying(rep, "undefined symbol"), 0u)
                << "the search's failure reached the link, which refused a name the failed member defines; the build "
                   "said:" << allSaid(rep);
            EXPECT_EQ(artifactsUnder(out), std::vector<std::string>{}) << "the failed build wrote an artifact";
        }
    }
}

// SITE (h), the commons round: two tentative definitions whose names an archive's members define, the members
// unreadable -- each common's member is refused, and the round goes on to the next common (the ELF members' document
// fetches a definition for a common, so the round reads the member to decide).
TEST(EveryUnit, EveryCommonOfTheCommonsRoundIsAskedBeforeTheSearchFails) {
    UnitDocument const& d = kUnitDocuments[0];   // elf64-x86_64-linux: `archiveCommonResolution` fetchDefinition
    for (bool const mixed : {false, true}) {
        SCOPED_TRACE(mixed ? "a readable member, then an unreadable one" : "both unreadable");
        dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-pull-commons"};
        fs::path const dir = scratch.path() / "lib";
        fs::create_directories(dir);
        std::vector<std::string> sources;
        for (char const* name : {"first_c", "second_c"}) {
            fs::path const src = dir / (std::string{name} + ".c");
            writeText(src, "int " + std::string{name} + " = 7;\n");
            sources.push_back(src.string());
        }
        dss::Program lp;
        lp.setOutputDir(dir / "lib");
        dss::DiagnosticReporter lrep;
        ASSERT_EQ(lp.compileUnits(sources, "c", {spec(d.arch, d.archive)}, lrep), 0) << allSaid(lrep);
        ASSERT_EQ(lp.artifactPaths().size(), 1u);
        auto bytes = bytesOf(*lp.artifactPaths().front());
        auto const members = objectMembersOf(bytes);
        ASSERT_EQ(members.size(), 2u);
        for (std::size_t m = (mixed ? 1u : 0u); m < 2; ++m) {
            auto const [at, size] = members[m];
            auto const patched = unreadable(std::vector<std::uint8_t>(
                bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + size)));
            std::copy(patched.begin(), patched.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        }
        fs::path const lib = scratch.path() / "commons.a";
        writeBytes(lib, bytes);
        fs::path const unit = scratch.path() / "main.c";
        writeText(unit, "int first_c;\nint second_c;\nint main(void) { return first_c + second_c; }\n");
        fs::path const out = scratch.path() / "out";
        dss::Program p;
        p.setOutputDir(out);
        p.setResolveLibraries(std::vector<fs::path>{lib});
        dss::DiagnosticReporter rep;
        EXPECT_NE(p.compileFiles({unit.string()}, "c", {spec(d.arch, execOf(d))}, rep), 0)
            << "an image whose commons' members cannot be read was built" << allSaid(rep);
        EXPECT_EQ(countSaying(rep, "it could not be read (above)"), mixed ? 1u : 2u)
            << "each common whose member cannot be read is refused once, whatever came before it; the build said:"
            << allSaid(rep);
        EXPECT_EQ(artifactsUnder(out), std::vector<std::string>{}) << "the failed build wrote an artifact";
    }
}

// SITE (i): the entry pull answers EVERY entry name before it fails (`pullEntryArchiveMembers`, called directly with
// two names an archive's members define).
TEST(EveryUnit, EveryEntryNameOfTheEntryPullIsAnsweredBeforeThePullFails) {
    for (UnitDocument const& d : kUnitDocuments) {
        for (bool const mixed : {false, true}) {
            SCOPED_TRACE(std::string{d.archive} + (mixed ? " / a readable member, then an unreadable one" : " / both unreadable"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-entry-pull"};
            Loaded const l = loadedFor(d.arch, execOf(d));
            ASSERT_TRUE(l.target && l.format);
            fs::path const lib = libraryOfTwo(scratch.path() / "lib", d, {!mixed, true});
            ASSERT_FALSE(lib.empty());
            // Each name as the program's own election spells it on the binary: the format's C decoration (Mach-O's
            // leading underscore), as `entryLinkNames` applies it.
            auto const onTheBinary = [&](std::string_view name) {
                return dss::ffi::applyCMangling(name, l.format->cSymbolDecoration().scheme);
            };
            std::vector<dss::EntryLinkName> const names{
                {onTheBinary("first_api"), "first_api", dss::EntryMaterialization::None},
                {onTheBinary("second_api"), "second_api", dss::EntryMaterialization::None}};
            std::vector<fs::path> const archives{lib};
            dss::DiagnosticReporter rep;
            auto const pulled = dss::pullEntryArchiveMembers(names, archives, *l.target, *l.format, rep);
            EXPECT_FALSE(pulled.has_value()) << "an entry pull of an unreadable member succeeded";
            EXPECT_EQ(countSaying(rep, kReaderRefusal), mixed ? 1u : 2u)
                << "each unreadable member is refused once, whatever came before it; the pull said:" << allSaid(rep);
        }
    }
}

namespace {

// A DSS dynamic library of `lib_fn` for `doc` (a dylib or a DLL document), made unreadable past its header when asked:
// a Mach-O image's LC_SYMTAB symbol offset and its export trie's offset (`unreadable`), or a PE image's export
// directory RVA, pointed past the end of the file -- 2147418112 (0x7fff0000) each.
[[nodiscard]] fs::path dynamicLibraryOf(fs::path const& dir, std::string_view arch, std::string_view doc,
                                        std::string const& stem, bool unreadablePastItsHeader) {
    fs::create_directories(dir);
    fs::path const src = dir / (stem + ".c");
    bool const pe = doc.starts_with("pe64");
    writeText(src, std::string{pe ? "__declspec(dllexport) " : ""} + "int lib_fn(void) { return 7; }\n");
    dss::Program p;
    p.setOutputDir(dir / stem);
    dss::DiagnosticReporter rep;
    if (p.compileFiles({src.string()}, "c", {spec(arch, doc)}, rep) != 0 || p.artifactPaths().size() != 1
        || !p.artifactPaths().front().has_value()) {
        ADD_FAILURE() << "the dynamic library " << stem << " for " << doc << " did not build:" << allSaid(rep);
        return {};
    }
    fs::path const built = *p.artifactPaths().front();
    auto bytes = bytesOf(built);
    if (unreadablePastItsHeader) {
        if (pe) {
            std::uint32_t const peHeader = getU32(bytes, 0x3c);
            putU32(bytes, std::size_t{peHeader} + 24 + 112, 0x7fff0000u);   // PE32+ data directory 0: the export table
        } else {
            bytes = unreadable(std::move(bytes));
        }
    }
    fs::path const out = dir / (stem + built.extension().string());
    writeBytes(out, bytes);
    return out;
}

}  // namespace

// SITE (f): every library named to an assembly unit's build is read, and each one whose surface cannot be read is
// refused once, by name and by its reader, before the binding fails -- the eager probe reads the header alone, so a
// library unreadable past it reaches the binder. `refusal` is what the family's reader says of the surface
// `dynamicLibraryOf` corrupts (READ: src/ffi/binary_readers/macho_reader.cpp, the export-trie path, which a dylib's
// surface takes before LC_SYMTAB; pe_reader.cpp, the export directory); a readable library is refused by nothing.
TEST(EveryUnit, EveryOperatorNamedLibraryIsReadBeforeTheBindingFails) {
    struct Family {
        std::string_view arch, exec, dynamic, assembly, marker, call, refusal;
    };
    constexpr Family kFamilies[] = {
        {"x86_64", "macho64-x86_64-darwin-exec", "macho64-x86_64-darwin-dylib", "asm-x86_64-att", "@function", "\tcall\t",
         "Mach-O reader: export trie (off=2147418112 + "},
        {"arm64", "macho64-arm64-darwin-exec", "macho64-arm64-darwin-dylib", "asm-arm64-gas", "%function", "\tbl\t",
         "Mach-O reader: export trie (off=2147418112 + "},
        {"x86_64", "pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll", "asm-x86_64-att", "@function", "\tcall\t",
         "PE reader: export-directory RVA 2147418112 does not lie in any section's raw data"},
    };
    for (Family const& f : kFamilies) {
        for (bool const mixed : {false, true}) {
            SCOPED_TRACE(std::string{f.dynamic} + (mixed ? " / a readable library, then an unreadable one" : " / both unreadable"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-named-libraries"};
            fs::path const libs = scratch.path() / "libs";
            fs::path const one = dynamicLibraryOf(libs, f.arch, f.dynamic, "lib_one", !mixed);
            fs::path const two = dynamicLibraryOf(libs, f.arch, f.dynamic, "lib_two", true);
            ASSERT_FALSE(one.empty() || two.empty());
            // An assembly unit names what the binary holds: `main` and `lib_fn` under the format's C decoration
            // (Mach-O's leading underscore), as a reference compiler's assembly output spells them.
            Loaded const l = loadedFor(f.arch, f.exec);
            ASSERT_TRUE(l.target && l.format);
            auto const onTheBinary = [&](std::string_view name) {
                return dss::ffi::applyCMangling(name, l.format->cSymbolDecoration().scheme);
            };
            std::string const entry = onTheBinary("main");
            fs::path const unit = scratch.path() / "main.s";
            writeText(unit, "\t.text\n\t.globl\t" + entry + "\n\t.type\t" + entry + ", " + std::string{f.marker} + "\n"
                                + entry + ":\n" + std::string{f.call} + onTheBinary("lib_fn") + "\n\tret\n");
            fs::path const out = scratch.path() / "out";
            dss::Program p;
            p.setOutputDir(out);
            p.setResolveLibraries(std::vector<fs::path>{one, two});
            dss::DiagnosticReporter rep;
            EXPECT_NE(p.compileFiles({unit.string()}, std::string{f.assembly}, {spec(f.arch, f.exec)}, rep), 0)
                << "a unit bound against an unreadable library was built" << allSaid(rep);
            for (std::string_view const name : {"lib_one", "lib_two"}) {
                bool const readable = mixed && name == "lib_one";
                std::size_t refused = 0;   // diagnostics naming the library AND saying its reader's refusal
                for (auto const& d : rep.all()) {
                    if (d.actual.find(name) != std::string::npos && d.actual.find(f.refusal) != std::string::npos) ++refused;
                }
                EXPECT_EQ(refused, readable ? 0u : 1u)
                    << (readable ? "the readable library " : "the unreadable library ") << name
                    << (readable ? " is refused" : " is not refused once by name") << " for its surface ('" << f.refusal
                    << "'); the build said:" << allSaid(rep);
            }
            EXPECT_EQ(artifactsUnder(out), std::vector<std::string>{}) << "the failed build wrote an artifact";
        }
    }
}

namespace {

void writeManifest(fs::path const& path, std::string const& profile, std::vector<std::string> const& targets,
                   std::vector<std::string> const& sources, std::vector<std::string> const& dependsOn) {
    auto const list = [](std::vector<std::string> const& items, bool quote) {
        std::string s = "[";
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0) s += ", ";
            s += quote ? "\"" + items[i] + "\"" : items[i];
        }
        return s + "]";
    };
    std::string text = "{\n  \"language\": \"c\",\n  \"artifactProfile\": \"" + profile + "\",\n  \"targets\": "
                     + list(targets, true) + ",\n  \"sources\": " + list(sources, true);
    if (!dependsOn.empty()) text += ",\n  \"dependsOn\": " + list(dependsOn, false);
    writeText(path, text + "\n}\n");
}

}  // namespace

// SITE (j): a project's artifact-profile gate names EVERY target whose format does not serve the profile before the
// build fails.
TEST(EveryUnit, EveryTargetTheProfileGateRejectsIsNamedBeforeTheBuildFails) {
    for (bool const mixed : {false, true}) {
        SCOPED_TRACE(mixed ? "a serving target, then a rejected one" : "two rejected targets");
        dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-profile-gate"};
        writeText(scratch.path() / "main.c", "int main(void) { return 0; }\n");
        std::vector<std::string> const targets =
            mixed ? std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec", "arm64:elf64-aarch64-linux"}
                  : std::vector<std::string>{"x86_64:elf64-x86_64-linux", "arm64:elf64-aarch64-linux"};
        fs::path const manifest = scratch.path() / "app.dss-project.json";
        writeManifest(manifest, "cli", targets, {(scratch.path() / "main.c").generic_string()}, {});
        dss::Program p;
        p.setOutputDir(scratch.path() / "out");
        dss::DiagnosticReporter rep;
        EXPECT_NE(p.compileProject(manifest.string(), rep), 0) << "a profile no target serves was built" << allSaid(rep);
        for (char const* format : {"elf64-x86_64-linux", "elf64-aarch64-linux"}) {
            if (mixed && std::string_view{format} == "elf64-x86_64-linux") continue;
            EXPECT_EQ(countSaying(rep, "object format '" + std::string{format} + "'"), 1u)
                << "the rejected target of " << format << " was not named once; the build said:" << allSaid(rep);
        }
    }
}

// SITE (k): the macro dump renders EVERY target's section, and names each target that fails, before it fails -- and
// prints nothing.
TEST(EveryUnit, EveryTargetOfTheMacroDumpIsRenderedBeforeTheDumpFails) {
    for (bool const mixed : {false, true}) {
        SCOPED_TRACE(mixed ? "a valid target, then an invalid one" : "two invalid targets");
        dss::CliArgs args;
        args.dumpPredefinedMacros = true;
        args.languageName         = "c";
        args.targets = mixed ? std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec", "x86_64:no-such-format-two"}
                             : std::vector<std::string>{"x86_64:no-such-format-one", "x86_64:no-such-format-two"};
        std::ostringstream out;
        std::ostringstream err;
        EXPECT_EQ(dss::dumpPredefinedMacros(args, out, err), 1);
        EXPECT_EQ(out.str(), "") << "a failed dump printed a section";
        for (char const* name : {"no-such-format-one", "no-such-format-two"}) {
            if (mixed && std::string_view{name} == "no-such-format-one") continue;
            EXPECT_NE(err.str().find(name), std::string::npos)
                << "the invalid target " << name << " was not named; the dump said:\n" << err.str();
        }
    }
}

namespace {

// A git seam whose clone of a URL naming "bad" fails; any other clone materializes a dependency project.
class ScriptedGit final : public dss::IGitRunner {
public:
    bool isAvailable() override { return true; }
    dss::GitCommandResult clone(std::string const& url, fs::path const& dest) override {
        if (url.find("bad") != std::string::npos) return {false, {}, "fatal: repository '" + url + "' not found"};
        fs::create_directories(dest);
        writeText(dest / "lib.c", "int dep_api(void) { return 7; }\n");
        writeManifest(dest / ".dss-project.json", "staticlib", {"x86_64:elf64-x86_64-linux-staticlib"}, {"lib.c"}, {});
        writeText(dest / "HEAD", "commit-aaa\n");
        return {true, {}, {}};
    }
    dss::GitCommandResult fetch(fs::path const&, std::string const&) override { return {true, {}, {}}; }
    dss::GitCommandResult checkout(fs::path const& dir, std::string const&) override {
        writeText(dir / "HEAD", "commit-aaa\n");
        return {true, {}, {}};
    }
    dss::GitCommandResult revParse(fs::path const& dir, std::string const&) override {
        std::ifstream in{dir / "HEAD"};
        if (!in) return {false, {}, "fatal: not a git repository: " + dir.generic_string()};
        dss::GitCommandResult r{true, {}, {}};
        std::getline(in, r.output);
        return r;
    }
};

}  // namespace

// SITE (l): the dependency walk asks EVERY entry in each of its steps before the step fails -- every path entry is
// located, every git entry's name registered and its checkout acquired.
TEST(EveryUnit, EveryDependencyEntryIsAskedBeforeTheWalkFails) {
    enum class Step { Locate, Register, Acquire };
    for (Step const step : {Step::Locate, Step::Register, Step::Acquire}) {
        for (bool const mixed : {false, true}) {
            SCOPED_TRACE(std::string{step == Step::Locate ? "locate" : step == Step::Register ? "register" : "acquire"}
                         + (mixed ? " / a valid entry, then an invalid one" : " / every entry invalid"));
            dss::test_support::ScratchDir scratch{dss::test_support::Location::InsideRepo, "every-unit-dependencies"};
            fs::path const dir = scratch.path();
            writeText(dir / "main.c", "int main(void) { return 0; }\n");
            std::vector<std::string> entries;
            std::vector<std::string> named;   // what each invalid entry's refusal names
            if (step == Step::Locate) {
                if (mixed) {
                    fs::create_directories(dir / "good_dep");
                    writeText(dir / "good_dep" / "lib.c", "int dep_api(void) { return 7; }\n");
                    writeManifest(dir / "good_dep" / ".dss-project.json", "staticlib",
                                  {"x86_64:elf64-x86_64-linux-staticlib"}, {"lib.c"}, {});
                    entries.push_back("{\"path\": \"" + (dir / "good_dep").generic_string() + "\"}");
                } else {
                    entries.push_back("{\"path\": \"" + (dir / "missing_one").generic_string() + "\"}");
                    named.push_back("missing_one");
                }
                entries.push_back("{\"path\": \"" + (dir / "missing_two").generic_string() + "\"}");
                named.push_back("missing_two");
            } else if (step == Step::Register) {
                // Two URLs whose derived names collide make one collision: the second refused.
                entries = {"{\"git\": \"https://a.invalid/org/lib.git\"}", "{\"git\": \"https://b.invalid/org/lib.git\"}"};
                named   = {"b.invalid/org/lib.git"};
                if (!mixed) {
                    entries.push_back("{\"git\": \"https://a.invalid/org/util.git\"}");
                    entries.push_back("{\"git\": \"https://b.invalid/org/util.git\"}");
                    named.push_back("b.invalid/org/util.git");
                }
            } else {
                entries.push_back(mixed ? "{\"git\": \"https://good.invalid/org/one.git\"}"
                                        : "{\"git\": \"https://bad.invalid/org/one.git\"}");
                if (!mixed) named.push_back("bad.invalid/org/one.git");
                entries.push_back("{\"git\": \"https://bad.invalid/org/two.git\"}");
                named.push_back("bad.invalid/org/two.git");
            }
            fs::path const manifest = dir / "app.dss-project.json";
            writeManifest(manifest, "cli", {"x86_64:elf64-x86_64-linux-exec"}, {(dir / "main.c").generic_string()},
                          entries);
            ScriptedGit git;
            dss::Program p;
            p.setOutputDir(dir / "out");
            p.setGitRunner(&git);
            dss::DiagnosticReporter rep;
            EXPECT_NE(p.compileProject(manifest.string(), rep), 0)
                << "a project with an invalid dependency was built" << allSaid(rep);
            for (std::string const& n : named) {
                EXPECT_GE(countSaying(rep, n), 1u)
                    << "the invalid entry " << n << " was not refused by name; the build said:" << allSaid(rep);
            }
        }
    }
}
