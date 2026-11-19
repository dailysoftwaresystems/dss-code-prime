// P69 fold 3d (lane `xa`): THE ELF READER ROUTES EVERY RELOCATION, OR SAYS WHY IT DOES NOT.
//
// Step (7) of `elf::readRelocatableObject` routes each relocation into the function or data item that holds its
// site. A relocation TABLE whose section produced no item at all used to be skipped with no word. ✔MEASURED P69 fold 3d
// (`skip7_census.py` of lane `xa`'s work directory, over 1345 objects and archive members of tests/ and the lane's
// probes, and 4868 members of the WSL system archives): the tables that reached the skip were
//   * a constructor or destructor table's (`.init_array`, `.init_array.NNNNN`, `.fini_array`) -- an ALLOCATED section
//     the program runs through, whose relocations dropped were constructors that never ran
//     (D-LK-FOREIGN-OBJECT-INITIALIZER-TABLES-NEVER-RUN): now REFUSED BY NAME;
//   * a NON-ALLOC section's (`.note.stapsdt` in libc.a; `-g` DWARF): not in the process image, and DSS carries no
//     foreign non-alloc section into its output (D-LK-FOREIGN-NON-ALLOC-SECTIONS-ARE-NOT-CARRIED), so its relocations
//     go with it -- READ, and said;
//   * a table with NO entries against an item-less section (DSS's own data-only archive members): it drops nothing.
//
// The subjects are reference bytes, `tests/link/data/elf_{constructor,destructor}_table.o` and
// `elf_debug_info_g.o` (gcc 13.3.0; sources and REBUILD lines beside them, each built twice byte-identical):
//   elf_constructor_table.o  1848 B  412558658dde75cb848823566de29eab
//   elf_destructor_table.o   1824 B  fccc49073c7037990bd940de504841d3
//   elf_debug_info_g.o       3536 B  1606ba7238210959d21e5eebd6136cdc
// and the member `member_definer.o` of the checked-in `libthread_storage_x86_64_elf.a` (an empty `.rela.text`).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::vector<std::uint8_t> fixture(std::string_view name) {
    auto const    path = dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

struct Read {
    std::optional<AssembledModule> mod;
    DiagnosticReporter             rep;
};

[[nodiscard]] std::unique_ptr<Read> readX86(std::vector<std::uint8_t> const& bytes) {
    auto out = std::make_unique<Read>();
    auto t   = TargetSchema::loadShipped("x86_64");
    auto f   = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    if (!t.has_value() || !f.has_value()) {
        ADD_FAILURE() << "the shipped x86_64 target or ELF format did not load";
        return out;
    }
    out->mod = elf::readRelocatableObject(bytes, **t, **f, out->rep);
    return out;
}

// The bytes of member `want` of a GNU `ar` archive. A name of more than 15 bytes is `/<offset>` into the `//` table,
// where it ends in "/\n".
[[nodiscard]] std::vector<std::uint8_t> memberOf(std::vector<std::uint8_t> const& ar, std::string_view want) {
    std::size_t pos = 8;
    std::string longNames;
    while (pos + 60 <= ar.size()) {
        std::string name(reinterpret_cast<char const*>(ar.data() + pos), 16);
        std::string size(reinterpret_cast<char const*>(ar.data() + pos + 48), 10);
        std::size_t const n = static_cast<std::size_t>(std::stoull(size));
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name == "//") {
            longNames.assign(reinterpret_cast<char const*>(ar.data() + pos + 60), n);
        } else if (name.size() > 1 && name.front() == '/' && name[1] >= '0' && name[1] <= '9') {
            std::size_t const at  = static_cast<std::size_t>(std::stoull(name.substr(1)));
            std::size_t const end = longNames.find("/\n", at);
            name = at < longNames.size() && end != std::string::npos ? longNames.substr(at, end - at) : std::string{};
        } else if (!name.empty() && name.back() == '/') {
            name.pop_back();
        }
        if (name == want) return {ar.begin() + static_cast<std::ptrdiff_t>(pos + 60),
                                  ar.begin() + static_cast<std::ptrdiff_t>(pos + 60 + n)};
        pos += 60 + n + (n & 1u);
    }
    ADD_FAILURE() << "no member " << want;
    return {};
}

}  // namespace

// A constructor table the reader made no item of is REFUSED, naming the table and its section: dropping its relocation
// is a constructor that never runs, and the program would look linked.
TEST(ElfReaderUnroutedRelocations, AConstructorTableIsRefusedByNameNotDropped) {
    for (char const* name : {"elf_constructor_table.o", "elf_destructor_table.o"}) {
        SCOPED_TRACE(name);
        auto const bytes = fixture(name);
        if (bytes.empty()) continue;
        auto const r = readX86(bytes);
        EXPECT_FALSE(r->mod.has_value()) << "a constructor or destructor table was read and its relocation dropped";
        std::string const table = std::string_view{name} == "elf_constructor_table.o" ? ".init_array" : ".fini_array";
        EXPECT_NE(diagnosticsOf(r->rep).find("patches section '" + table + "', which the program loads"),
                  std::string::npos)
            << diagnosticsOf(r->rep);
    }
}

// A NON-ALLOC section's relocations go with the section, which DSS does not carry: `-g` DWARF is read past, the
// object's code and data are read, and nothing is reported.
TEST(ElfReaderUnroutedRelocations, ADebugSectionsRelocationsGoWithTheSectionAndTheObjectIsRead) {
    auto const bytes = fixture("elf_debug_info_g.o");
    ASSERT_FALSE(bytes.empty());
    auto const r = readX86(bytes);
    ASSERT_TRUE(r->mod.has_value()) << diagnosticsOf(r->rep);
    EXPECT_TRUE(r->rep.all().empty()) << diagnosticsOf(r->rep);
    std::size_t named = 0;
    for (auto const& row : r->mod->symbols) named += row.name == "add" || row.name == "twice" ? 1u : 0u;
    EXPECT_EQ(named, 2u) << "both functions are read";
}

// THE BLAST RADIUS, MEASURED: an image document declares no `unwind` row (elf64-x86_64-linux-exec does not), and an
// object whose `.eh_frame` the reader made no item of is refused at step 7 -- so a link that read a foreign object
// through the IMAGE document would refuse every gcc object. It does not: the link reads a named object through the
// format's archive-writing sibling (`compile_pipeline.cpp`, `archiveMemberFormat`), which declares the row. gcc's
// `-g` object, which carries `.eh_frame` (checked below), links beside a C `main` through the production driver for
// the exec document.
TEST(ElfReaderUnroutedRelocations, AGccObjectWithAnUnwindTableLinksThroughTheImageDocument) {
    auto const bytes = fixture("elf_debug_info_g.o");
    ASSERT_FALSE(bytes.empty());
    // The premise, read with the test's own reader: the object carries `.eh_frame` and the relocations that patch it.
    {
        auto const rd = [&](std::size_t at, unsigned w) {
            std::uint64_t v = 0;
            for (unsigned i = 0; i < w; ++i) v |= static_cast<std::uint64_t>(bytes[at + i]) << (8u * i);
            return v;
        };
        std::size_t const shoff = static_cast<std::size_t>(rd(0x28, 8));
        std::size_t const shnum = static_cast<std::size_t>(rd(0x3C, 2)), shstr = static_cast<std::size_t>(rd(0x3E, 2));
        std::size_t const strOff = static_cast<std::size_t>(rd(shoff + shstr * 64u + 0x18, 8));
        bool unwind = false, unwindRela = false;
        for (std::size_t i = 0; i < shnum; ++i) {
            std::string name;
            for (std::size_t k = strOff + static_cast<std::size_t>(rd(shoff + i * 64u, 4)); k < bytes.size() && bytes[k] != 0;
                 ++k) {
                name.push_back(static_cast<char>(bytes[k]));
            }
            unwind     = unwind || name == ".eh_frame";
            unwindRela = unwindRela || name == ".rela.eh_frame";
        }
        ASSERT_TRUE(unwind && unwindRela) << "the subject carries `.eh_frame` and `.rela.eh_frame`";
    }
    // The hazard is real: read through the exec document, which names no unwind section, the object is refused.
    {
        auto t = TargetSchema::loadShipped("x86_64");
        auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
        ASSERT_TRUE(t.has_value() && f.has_value());
        DiagnosticReporter rep;
        EXPECT_FALSE(elf::readRelocatableObject(bytes, **t, **f, rep).has_value());
        EXPECT_NE(diagnosticsOf(rep).find("patches section '.eh_frame', which the program loads"), std::string::npos)
            << diagnosticsOf(rep);
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "elf-unrouted-blast-radius"};
    auto const               dir = scratch.path();
    auto const               obj = dir / "debug_info_g.o";
    std::ofstream(obj, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
    auto const src = dir / "main.c";
    std::ofstream(src, std::ios::binary) << "int add(int, int); int twice(int);\n"
                                            "int main(void) { return add(twice(10), 22); }\n";
    auto const out = dir / "dss.out";
    std::filesystem::create_directories(out);
    Program p;
    p.setOutputDir(out);
    DiagnosticReporter rep;
    EXPECT_EQ(p.compileUnits(std::vector<std::string>{obj.string(), src.string()}, "c",
                             std::vector<std::string>{"x86_64:elf64-x86_64-linux-exec"}, rep),
              0)
        << "the link reads gcc's object through a document that declares its unwind section:" << diagnosticsOf(rep);
}

// A relocation table with no entries drops nothing, whatever its section holds: DSS's own data-only archive member
// writes an empty `.rela.text` beside a `.text` that holds no function, and it is read.
TEST(ElfReaderUnroutedRelocations, AnEmptyTableOfASectionWithNoItemIsRead) {
    auto const ar = fixture("libthread_storage_x86_64_elf.a");
    ASSERT_FALSE(ar.empty());
    auto const member = memberOf(ar, "member_definer.o");
    ASSERT_FALSE(member.empty());
    auto const r = readX86(member);
    EXPECT_TRUE(r->mod.has_value()) << diagnosticsOf(r->rep);
}
