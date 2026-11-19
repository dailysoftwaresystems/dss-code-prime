// P69 fold 3f (lane `xa`) -- `#pragma comment`'s Mach-O half: LC_LINKER_OPTION, the load command in which a
// relocatable Mach-O object hands its final linker options.
//
// ✔MEASURED (P69 fold 3f, Apple clang 21 `-fms-extensions` with the host's ld64): `#pragma comment(lib, "x")` writes
// ONE LC_LINKER_OPTION (45) holding the one string `-lx`, and `comment(linker, "t")` one holding `t` as one string,
// each pragma its own command, cmdsize 12 + the NUL-terminated strings zero-filled to 8 (16 for `-lz`); ld64
// searches a library either kind spells, and warns "unknown linker option from object file ignored" of the one
// string `-framework CoreFoundation`. The command's number, the library prefix and the framework word are the
// relocatable documents' data (`macho.linkerOptions`), with what a DSS image link does with them (`inImage`):
//   * the shipped relocatable and static-library documents state the command, an image document none;
//   * the loader refuses the block on an image document, an empty field, an unbuilt verdict, an unknown key;
//   * the writer writes each library and each option as its own command, after LC_SYMTAB, its bytes clang's;
//   * the reader reads each command as ld64 does -- a library into the unit's libraries, a framework (two
//     strings) kept, any other option kept and warned by the link that makes an image -- and refuses a command
//     whose strings run past it;
//   * a document stating no command refuses to write a unit that asks for one, by name.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/unit_linker_requests.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "format_reject_support.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::rejectSummary;

namespace {

constexpr std::uint32_t kLcLinkerOption = 45;   // LC_LINKER_OPTION, as the documents state it

[[nodiscard]] std::shared_ptr<TargetSchema const> arm64() {
    auto t = TargetSchema::loadShipped("arm64");
    if (!t.has_value()) {
        ADD_FAILURE() << "TargetSchema::loadShipped(arm64) failed";
        return nullptr;
    }
    return std::move(t).value();
}

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> format(char const* name) {
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

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

// A one-function arm64 unit (`dss_fn`, `ret`) asking its final linker for `libraries` and `options`.
[[nodiscard]] AssembledModule unitAsking(std::vector<std::string> libraries,
                                         std::vector<std::vector<std::string>> options) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_fn", SymbolBinding::Global, SymbolVisibility::Default});
    mod.linkerRequests.dependentLibraries = std::move(libraries);
    mod.linkerRequests.linkerOptions      = std::move(options);
    return mod;
}

[[nodiscard]] std::uint32_t u32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | static_cast<std::uint32_t>(b[o + 1]) << 8
         | static_cast<std::uint32_t>(b[o + 2]) << 16 | static_cast<std::uint32_t>(b[o + 3]) << 24;
}

struct Command {
    std::uint32_t             cmd = 0;
    std::vector<std::uint8_t> bytes;   // the whole command, cmd and cmdsize included
};

// Every load command of a 64-bit Mach-O file, in order.
[[nodiscard]] std::vector<Command> commandsOf(std::vector<std::uint8_t> const& b) {
    std::vector<Command> out;
    std::uint32_t const ncmds = u32(b, 16);
    std::size_t at = 32;
    for (std::uint32_t i = 0; i < ncmds && at + 8 <= b.size(); ++i) {
        std::uint32_t const size = u32(b, at + 4);
        if (size < 8 || at + size > b.size()) break;
        out.push_back(Command{u32(b, at), std::vector<std::uint8_t>(b.begin() + at, b.begin() + at + size)});
        at += size;
    }
    return out;
}

// One LC_LINKER_OPTION's bytes, as clang writes it: cmd, cmdsize, count, the strings, zero-filled to 8.
[[nodiscard]] std::vector<std::uint8_t> clangCommand(std::vector<std::string> const& words) {
    std::vector<std::uint8_t> body;
    for (auto const& w : words) {
        body.insert(body.end(), w.begin(), w.end());
        body.push_back(0);
    }
    std::size_t const size = (12u + body.size() + 7u) & ~std::size_t{7};
    std::vector<std::uint8_t> out;
    for (std::uint32_t const v : {kLcLinkerOption, static_cast<std::uint32_t>(size),
                                  static_cast<std::uint32_t>(words.size())}) {
        for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
    out.insert(out.end(), body.begin(), body.end());
    out.resize(size, 0);
    return out;
}

}  // namespace

TEST(MachOLinkerOptions, TheRelocatableDocumentsStateTheCommandAndAnImageDocumentNone) {
    for (char const* name : {"macho64-arm64-darwin", "macho64-x86_64-darwin", "macho64-arm64-darwin-staticlib",
                             "macho64-x86_64-darwin-staticlib"}) {
        SCOPED_TRACE(name);
        auto const f = format(name);
        ASSERT_NE(f, nullptr);
        auto const& block = f->macho().linkerOptions;
        ASSERT_TRUE(block.has_value()) << "a relocatable document states no 'macho.linkerOptions'";
        EXPECT_EQ(block->command, kLcLinkerOption);
        EXPECT_EQ(block->library, "-l");
        EXPECT_EQ(block->framework, "-framework");
        EXPECT_FALSE(block->inImageReason.empty());
    }
    for (char const* name : {"macho64-arm64-darwin-exec", "macho64-x86_64-darwin-exec", "macho64-arm64-darwin-dylib",
                             "macho64-x86_64-darwin-dylib"}) {
        SCOPED_TRACE(name);
        auto const f = format(name);
        ASSERT_NE(f, nullptr);
        EXPECT_FALSE(f->macho().linkerOptions.has_value()) << "an image document states the command";
    }
}

TEST(MachOLinkerOptions, TheLoaderRefusesAnImageDocumentAnEmptyFieldAnUnbuiltVerdictAndAnUnknownKey) {
    struct Case {
        char const* label;
        char const* stem;
        void (*mutate)(nlohmann::json&);
        char const* path;
    };
    Case const kCases[] = {
        {"an image document", "macho64-arm64-darwin-exec",
         [](nlohmann::json& d) {
             d["macho"]["linkerOptions"] = nlohmann::json::parse(
                 R"({"command": 45, "library": "-l", "framework": "-framework", "inImage": "ignored",
                     "reason": "a reason"})");
         },
         "/macho/linkerOptions"},
        {"an empty library prefix", "macho64-arm64-darwin",
         [](nlohmann::json& d) { d["macho"]["linkerOptions"]["library"] = ""; }, "/macho/linkerOptions/library"},
        {"an empty framework word", "macho64-arm64-darwin",
         [](nlohmann::json& d) { d["macho"]["linkerOptions"]["framework"] = ""; },
         "/macho/linkerOptions/framework"},
        {"an empty reason", "macho64-arm64-darwin",
         [](nlohmann::json& d) { d["macho"]["linkerOptions"]["reason"] = ""; }, "/macho/linkerOptions/reason"},
        {"no command", "macho64-arm64-darwin",
         [](nlohmann::json& d) { d["macho"]["linkerOptions"]["command"] = 0; }, "/macho/linkerOptions/command"},
        {"a verdict no link has built", "macho64-arm64-darwin",
         [](nlohmann::json& d) { d["macho"]["linkerOptions"]["inImage"] = "honoured"; },
         "/macho/linkerOptions/inImage"},
        {"a key the block does not know", "macho64-arm64-darwin",
         [](nlohmann::json& d) { d["macho"]["linkerOptions"]["align"] = 8; }, "/macho/linkerOptions/align"},
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

// The writer writes each library and then each option as its own command, after LC_SYMTAB, each byte for byte the
// command clang writes for the same request (✔MEASURED P69 fold 3f: `-lz` is 2D 00 00 00 | 10 00 00 00 | 01 00 00 00
// | `-lz\0`), and counts each in ncmds and sizeofcmds.
TEST(MachOLinkerOptions, TheWriterWritesEachRequestAsClangDoes) {
    auto const t = arm64();
    auto const f = format("macho64-arm64-darwin");
    ASSERT_TRUE(t && f);
    AssembledModule const mod = unitAsking({"z", "dss lib"}, {{"-framework", "CoreFoundation"}, {"-lz -lm"}});
    DiagnosticReporter rep;
    auto const bytes = macho::encode(mod, *t, *f, rep);
    ASSERT_FALSE(bytes.empty()) << diagnosticsOf(rep);
    auto const commands = commandsOf(bytes);
    std::vector<std::vector<std::uint8_t>> options;
    std::size_t symtabAt = commands.size();
    for (std::size_t k = 0; k < commands.size(); ++k) {
        if (commands[k].cmd == 0x2u) symtabAt = k;
        if (commands[k].cmd == kLcLinkerOption) {
            EXPECT_GT(k, symtabAt) << "an LC_LINKER_OPTION stands before LC_SYMTAB";
            options.push_back(commands[k].bytes);
        }
    }
    std::vector<std::vector<std::uint8_t>> const clang{
        clangCommand({"-lz"}), clangCommand({"-ldss lib"}), clangCommand({"-framework", "CoreFoundation"}),
        clangCommand({"-lz -lm"})};
    EXPECT_EQ(options, clang);
    EXPECT_EQ(clang.front(), (std::vector<std::uint8_t>{0x2D, 0, 0, 0, 0x10, 0, 0, 0, 1, 0, 0, 0, '-', 'l', 'z', 0}))
        << "the measured command";
    std::size_t total = 0;
    for (auto const& c : commands) total += c.bytes.size();
    EXPECT_EQ(u32(bytes, 20), total) << "sizeofcmds is not every command's bytes";
}

// The reader reads each command as ld64 does: a library into the unit's libraries (once, in order), a framework --
// two strings -- kept silent, and an option ld64 would not take kept and warned by the link that makes an image.
TEST(MachOLinkerOptions, TheReaderReadsEachCommandAsLd64Does) {
    auto const t = arm64();
    auto const f = format("macho64-arm64-darwin");
    ASSERT_TRUE(t && f);
    AssembledModule const mod =
        unitAsking({"z", "dss lib"}, {{"-framework", "CoreFoundation"}, {"-framework CoreFoundation"}});
    DiagnosticReporter rep;
    auto const bytes = macho::encode(mod, *t, *f, rep);
    ASSERT_FALSE(bytes.empty()) << diagnosticsOf(rep);
    DiagnosticReporter readRep;
    auto const read = macho::readRelocatableObject(bytes, *t, *f, readRep);
    ASSERT_TRUE(read.has_value()) << diagnosticsOf(readRep);
    EXPECT_EQ(readRep.warningCount(), 0u) << "the reader says nothing itself" << diagnosticsOf(readRep);
    EXPECT_EQ(read->linkerRequests.dependentLibraries, (std::vector<std::string>{"z", "dss lib"}));
    EXPECT_EQ(read->linkerRequests.linkerOptions,
              (std::vector<std::vector<std::string>>{{"-framework", "CoreFoundation"}, {"-framework CoreFoundation"}}));
    ASSERT_EQ(read->linkerRequests.warnings.size(), 1u) << "only the option ld64 warns of is warned";
    EXPECT_NE(read->linkerRequests.warnings.front().find("'-framework CoreFoundation'"), std::string::npos)
        << read->linkerRequests.warnings.front();
    EXPECT_NE(read->linkerRequests.warnings.front().find("unknown linker option from object file ignored"),
              std::string::npos)
        << read->linkerRequests.warnings.front();
}

// A command whose count states more strings than it holds is refused by name, not read past its end.
TEST(MachOLinkerOptions, TheReaderRefusesACommandWhoseStringsRunPastIt) {
    auto const t = arm64();
    auto const f = format("macho64-arm64-darwin");
    ASSERT_TRUE(t && f);
    DiagnosticReporter rep;
    auto bytes = macho::encode(unitAsking({"z"}, {}), *t, *f, rep);
    ASSERT_FALSE(bytes.empty()) << diagnosticsOf(rep);
    std::size_t at = 32;
    bool patched = false;
    for (std::uint32_t i = 0; i < u32(bytes, 16); ++i) {
        if (u32(bytes, at) == kLcLinkerOption) {
            bytes[at + 8] = 2;   // the count: two strings, where the command holds one
            patched = true;
        }
        at += u32(bytes, at + 4);
    }
    ASSERT_TRUE(patched);
    DiagnosticReporter readRep;
    EXPECT_FALSE(macho::readRelocatableObject(bytes, *t, *f, readRep).has_value());
    bool named = false;
    for (auto const& d : readRep.all()) {
        named = named || (d.code == DiagnosticCode::F_CorruptedBinary
                          && d.actual.find("LC_LINKER_OPTION") != std::string::npos);
    }
    EXPECT_TRUE(named) << diagnosticsOf(readRep);
}

TEST(MachOLinkerOptions, ADocumentStatingNoCommandRefusesAUnitAskingForOne) {
    auto const t = arm64();
    ASSERT_NE(t, nullptr);
    nlohmann::json doc = nlohmann::json::parse(documentText("macho64-arm64-darwin"));
    doc["macho"].erase("linkerOptions");
    auto const f = ObjectFormatSchema::loadFromText(doc.dump(), "macho64 relocatable stating no linker options");
    ASSERT_TRUE(f.has_value()) << rejectSummary(f);
    DiagnosticReporter rep;
    EXPECT_TRUE(macho::encode(unitAsking({"z"}, {}), *t, **f, rep).empty())
        << "the object was written without the library it names";
    bool named = false;
    for (auto const& d : rep.all()) {
        named = named
             || (d.code == DiagnosticCode::K_NoMatchingObjectFormat && d.actual.find("'z'") != std::string::npos
                 && d.actual.find("macho.linkerOptions") != std::string::npos);
    }
    EXPECT_TRUE(named) << "the refusal does not name the library and the missing command" << diagnosticsOf(rep);
}
