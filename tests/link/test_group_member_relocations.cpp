// P69 fold 3 (lane `xa`) — A RELOCATION THAT STANDS INSIDE A SECTION GROUP, AND NAMES A MEMBER OF IT.
// D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE
//
// ★ WHAT THIS FILE PINS, AND THE REGRESSION IT WAS WRITTEN AFTER. An ELF COMDAT group is kept or discarded WHOLE:
// the first unit that carries a signature keeps its group, every later unit's goes with everything in it. P69
// fold 2 taught the merge to REFUSE a relocation that names the BYTES of a member of a discarded group — the one
// program in which keeping the bytes (DSS discards no group yet) and discarding them differ, which GNU ld and
// ld.lld refuse too. It asked (unit, target) and never where the relocation STANDS, so it also refused a relocation
// from INSIDE the losing group: both of whose ends go together, which is no error at all — and every function gcc
// gives a jump table holds one (`.rodata.<f>`, `R_X86_64_PC32 .text.<f> + offset`, both sections members of `f`).
//
// ✔MEASURED 2026-10-12 on the two tracked objects below, under a `main` returning
// `jump_table_a() + jump_table_b()` (gcc 13.3.0):
//     GNU ld 2.42 and ld.lld 18.1.3            unit A first: 44      unit B first: 88      (4 of 4 cells)
//     a DSS build WITHOUT fold 2               44 and 88
//     DSS at fold 2                            REFUSED, both orders (`K_CrossCuMergeUnsupported`)
// A regression of the PR that made it. What restores it: the refusal asks whether the item the relocation stands
// in is a member of the same group of the same unit as its target (`relocationTargetFor`, link/linker.cpp), and the
// unit states which of its input sections are members of which group
// (`AssembledModule::sectionGroupOfInputSection`), because a member section need hold no named definition — the
// table's holds only its section symbol, so no ROW could answer for it.
//
// Fixtures (x86_64 ELF; each `elf_group_jump_table_*.source.s` beside them is its source and states its rebuild
// command; GNU as 2.42 through gcc 13.3.0, the same bytes from a second build):
//     elf_group_jump_table_a_x86_64_elf_gas.o   md5 36086c6ca57f6e900b02d10abedff4fa   f(1) = 22
//     elf_group_jump_table_b_x86_64_elf_gas.o   md5 f0324ac6a30ed6c511e2c8db43716020   f(1) = 44, a padded body
// and, for the refusal that must STAY (a relocation from outside the group), two of fold 2's:
//     elf_group_copy_x86_64_elf_gas.o, elf_group_label_x86_64_elf_gas.o   (`test_common_symbols.cpp` states them)
//
// ★ NO IMAGE IS RUN HERE: every host reads the linked bytes. Whose body the two callers reach, and which body each
// table's entries land in, are read off the image itself.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr char const* kUnitA = "elf_group_jump_table_a_x86_64_elf_gas.o";
constexpr char const* kUnitB = "elf_group_jump_table_b_x86_64_elf_gas.o";

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> relocatable;
    std::shared_ptr<ObjectFormatSchema const> exec;
};

[[nodiscard]] Loaded loadElfX8664() {
    Loaded l;
    auto   t = TargetSchema::loadShipped("x86_64");
    auto   r = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    auto   x = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    EXPECT_TRUE(t.has_value() && r.has_value() && x.has_value());
    if (t.has_value()) l.target = *t;
    if (r.has_value()) l.relocatable = *r;
    if (x.has_value()) l.exec = *x;
    return l;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

[[nodiscard]] std::vector<std::uint8_t> trackedObject(char const* name) {
    auto const    path = dss::test::repoRoot() / "tests" / "link" / "data" / name;
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    std::string const raw{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return std::vector<std::uint8_t>(raw.begin(), raw.end());
}

[[nodiscard]] std::optional<AssembledModule> readUnit(Loaded const& L, char const* file, std::uint32_t cu,
                                                      DiagnosticReporter& rep) {
    return elf::readRelocatableObject(trackedObject(file), *L.target, *L.relocatable, rep, CompilationUnitId{cu});
}

[[nodiscard]] ModuleSymbol const* definitionNamed(AssembledModule const& m, std::string_view name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// ── reading the linked ELF64 image (little-endian) ───────────────────────────────────────────────────────────────
using Image = std::vector<std::uint8_t>;

[[nodiscard]] std::uint64_t rd(Image const& img, std::size_t at, unsigned bytes) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        if (at + i >= img.size()) return 0;
        v |= static_cast<std::uint64_t>(img[at + i]) << (8u * i);
    }
    return v;
}
[[nodiscard]] std::int64_t rdS32(Image const& img, std::size_t at) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(rd(img, at, 4)));
}

struct LoadSegment {
    std::uint64_t offset = 0, vaddr = 0, fileSize = 0;
};
[[nodiscard]] std::vector<LoadSegment> loadSegmentsOf(Image const& img) {
    std::vector<LoadSegment> out;
    if (img.size() < 0x40) return out;
    std::uint64_t const phoff     = rd(img, 0x20, 8);
    std::uint64_t const phentsize = rd(img, 0x36, 2);
    std::uint64_t const phnum     = rd(img, 0x38, 2);
    for (std::uint64_t i = 0; i < phnum; ++i) {
        auto const at = static_cast<std::size_t>(phoff + i * phentsize);
        if (rd(img, at, 4) != 1u) continue;   // PT_LOAD
        out.push_back(LoadSegment{rd(img, at + 0x08, 8), rd(img, at + 0x10, 8), rd(img, at + 0x20, 8)});
    }
    return out;
}
[[nodiscard]] std::optional<std::uint64_t> addressOf(std::vector<LoadSegment> const& segs, std::uint64_t offset) {
    for (auto const& s : segs) {
        if (offset >= s.offset && offset < s.offset + s.fileSize) return s.vaddr + (offset - s.offset);
    }
    return std::nullopt;
}
[[nodiscard]] std::optional<std::uint64_t> offsetOf(std::vector<LoadSegment> const& segs, std::uint64_t address) {
    for (auto const& s : segs) {
        if (address >= s.vaddr && address < s.vaddr + s.fileSize) return s.offset + (address - s.vaddr);
    }
    return std::nullopt;
}

[[nodiscard]] bool holdsAt(Image const& img, std::size_t at, std::initializer_list<std::uint8_t> bytes) {
    if (at + bytes.size() > img.size()) return false;
    std::size_t i = 0;
    for (std::uint8_t const b : bytes) {
        if (img[at + i++] != b) return false;
    }
    return true;
}

// The two bodies of `f`, as their sources wrote them. Both open `cmpl $1,%edi; ja; movl %edi,%edi;
// leaq table(%rip),%rdx` — the `leaq` at +7, its displacement at +10, the next instruction at +14 — and differ in
// where their first case stands and what it answers.
struct Body {
    char const*   unit;
    std::size_t   firstCase;      // offset of `.L0` in the body; `.L1` is 6 bytes on
    std::uint8_t  firstAnswer;    // the immediate of `.L0`'s `movl`
};
constexpr Body kBodyA{"unit A", 0x17, 11};
constexpr Body kBodyB{"unit B", 0x1e, 33};

[[nodiscard]] bool opensAsF(Image const& img, std::size_t at) {
    return holdsAt(img, at, {0x83, 0xFF, 0x01, 0x77}) && holdsAt(img, at + 5, {0x89, 0xFF, 0x48, 0x8D, 0x15});
}
[[nodiscard]] Body const* bodyAt(Image const& img, std::size_t at) {
    if (!opensAsF(img, at)) return nullptr;
    for (Body const* b : {&kBodyA, &kBodyB}) {
        if (holdsAt(img, at + b->firstCase, {0xB8, b->firstAnswer, 0x00, 0x00, 0x00, 0xC3})) return b;
    }
    return nullptr;
}

// Where the caller that opens at `at` (`movl $1,%edi; jmp rel32`) lands, as a file offset.
[[nodiscard]] std::optional<std::size_t> jumpTargetOf(Image const& img, std::vector<LoadSegment> const& segs,
                                                      std::size_t at) {
    auto const next = addressOf(segs, at + 10);
    if (!next.has_value()) return std::nullopt;
    auto const target = offsetOf(segs, static_cast<std::uint64_t>(static_cast<std::int64_t>(*next) + rdS32(img, at + 6)));
    if (!target.has_value()) return std::nullopt;
    return static_cast<std::size_t>(*target);
}

struct LinkedPair {
    bool               ok = false;
    Image              image;
    std::string        diagnostics;
    DiagnosticReporter reporter;
};

// Links `first` then `second` as an executable image whose entry is `first`'s caller.
void linkPair(Loaded const& L, char const* first, char const* second, char const* entryName, LinkedPair& out) {
    std::vector<AssembledModule> units;
    DiagnosticReporter           readRep;
    for (char const* file : {first, second}) {
        auto unit = readUnit(L, file, static_cast<std::uint32_t>(units.size() + 1), readRep);
        ASSERT_TRUE(unit.has_value()) << file << diagnosticsOf(readRep);
        units.push_back(std::move(*unit));
    }
    auto const* entry = definitionNamed(units[0], entryName);
    ASSERT_NE(entry, nullptr) << entryName;
    units[0].userEntrySymbol = entry->symbol;
    auto img = linker::link(std::span<AssembledModule const>{units}, *L.target, *L.exec, out.reporter,
                            ImageRequest{.artifactFileName = "group_jump_table_pair"});
    out.ok          = img.ok();
    out.diagnostics = diagnosticsOf(out.reporter);
    out.image       = std::move(img.bytes);
}

}  // namespace

// The reader states, for every input section of a group, the group's signature — the table's section too, which
// holds no named definition and so no row that could say it.
TEST(GroupMemberRelocations, TheElfReaderStatesTheGroupOfEveryMemberSection) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable);
    for (char const* file : {kUnitA, kUnitB}) {
        SCOPED_TRACE(file);
        DiagnosticReporter rep;
        auto const         unit = readUnit(L, file, 1, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        for (auto const& [section, signature] : unit->sectionGroupOfInputSection) {
            EXPECT_EQ(signature, "f") << "input section #" << section;
        }

        // The function: a named member.
        auto const* f = definitionNamed(*unit, "f");
        ASSERT_NE(f, nullptr);
        AssembledFunction const* body = nullptr;
        for (auto const& fn : unit->functions) {
            if (fn.symbol == f->symbol) body = &fn;
        }
        ASSERT_NE(body, nullptr);
        ASSERT_TRUE(body->inputSection.has_value());
        EXPECT_TRUE(unit->sectionGroupOfInputSection.contains(body->inputSection->section))
            << "the section of `f` is a member of group 'f' and the unit must say so of the SECTION";

        // The jump table: the one data item that holds relocations — and no row names it.
        AssembledData const* table = nullptr;
        for (auto const& di : unit->dataItems) {
            if (!di.relocations.empty()) {
                EXPECT_EQ(table, nullptr) << "one table is expected";
                table = &di;
            }
        }
        ASSERT_NE(table, nullptr) << "the fixture's jump table was not read as a data item";
        EXPECT_EQ(table->relocations.size(), 2u);
        for (auto const& s : unit->symbols) {
            EXPECT_NE(s.symbol, table->symbol) << "the table is expected to have NO row ('" << s.name
                                               << "'): that is why the group is stated on the section";
        }
        ASSERT_TRUE(table->inputSection.has_value());
        EXPECT_TRUE(unit->sectionGroupOfInputSection.contains(table->inputSection->section))
            << "the jump table's section is a member of group 'f' and the unit must say so of the SECTION";
        for (auto const& rel : table->relocations) {
            EXPECT_EQ(rel.target, f->symbol) << "each entry of the table names the BYTES of `f`";
        }

        // CONTROL: the caller lies in `.text`, a section of no group.
        auto const* caller = definitionNamed(*unit, file == kUnitA ? "jump_table_a" : "jump_table_b");
        ASSERT_NE(caller, nullptr);
        for (auto const& fn : unit->functions) {
            if (fn.symbol != caller->symbol) continue;
            ASSERT_TRUE(fn.inputSection.has_value());
            EXPECT_FALSE(unit->sectionGroupOfInputSection.contains(fn.inputSection->section))
                << "`.text` is in no group";
        }
    }
    // CONTROL: an object with no group states none.
    DiagnosticReporter rep;
    auto const         plain = readUnit(L, "elf_group_strong_x86_64_elf_gas.o", 1, rep);
    ASSERT_TRUE(plain.has_value()) << diagnosticsOf(rep);
    EXPECT_TRUE(plain->sectionGroupOfInputSection.empty());
}

// THE CLAIM. The pair links in both orders, both callers reach the FIRST unit's body, and each body's table stays
// on its own body's cases.
TEST(GroupMemberRelocations, APairWhoseGroupsHoldAJumpTableLinksInBothOrdersAndEachTableStaysOnItsOwnBody) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable && L.exec);
    struct Order {
        char const* name;
        char const* first;
        char const* second;
        char const* entry;
        Body const* kept;     // the body of the unit named first: the group a link keeps
    };
    for (Order const& order : {Order{"unit A first", kUnitA, kUnitB, "jump_table_a", &kBodyA},
                               Order{"unit B first", kUnitB, kUnitA, "jump_table_b", &kBodyB}}) {
        SCOPED_TRACE(order.name);
        LinkedPair linked;
        linkPair(L, order.first, order.second, order.entry, linked);
        ASSERT_TRUE(linked.ok) << "every reference linker links this pair (GNU ld 2.42 and ld.lld 18.1.3: 44 and "
                                  "88); a relocation that stands inside a group and names a member of it is no "
                                  "reference to discarded bytes" << linked.diagnostics;
        EXPECT_EQ(linked.reporter.errorCount(), 0u) << linked.diagnostics;
        Image const& img  = linked.image;
        auto const   segs = loadSegmentsOf(img);
        ASSERT_FALSE(segs.empty()) << "the image has no loadable segment to read";

        // Both callers (`movl $1,%edi; jmp f`) reach one body: the first unit's.
        std::size_t callers = 0;
        for (std::size_t at = 0; at + 10 <= img.size(); ++at) {
            if (!holdsAt(img, at, {0xBF, 0x01, 0x00, 0x00, 0x00, 0xE9})) continue;
            ++callers;
            auto const target = jumpTargetOf(img, segs, at);
            ASSERT_TRUE(target.has_value()) << "a caller's jump leaves the image";
            Body const* reached = bodyAt(img, *target);
            ASSERT_NE(reached, nullptr) << "a caller's jump does not land on a body of `f`";
            EXPECT_STREQ(reached->unit, order.kept->unit)
                << "a call of `f` by name runs the body of the FIRST unit that carries the group";
        }
        EXPECT_EQ(callers, 2u) << "one caller per unit";

        // Every body of `f` in the image has a table whose entries are ITS OWN cases.
        std::size_t bodies = 0;
        for (std::size_t at = 0; at + 0x30 <= img.size(); ++at) {
            Body const* body = bodyAt(img, at);
            if (body == nullptr) continue;
            ++bodies;
            SCOPED_TRACE(std::string{"the body of "} + body->unit);
            auto const afterLea = addressOf(segs, at + 14);
            ASSERT_TRUE(afterLea.has_value());
            std::uint64_t const tableAddress =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(*afterLea) + rdS32(img, at + 10));
            auto const tableAt = offsetOf(segs, tableAddress);
            ASSERT_TRUE(tableAt.has_value()) << "the body's table is not in the image";
            for (std::size_t entry = 0; entry < 2; ++entry) {
                std::uint64_t const lands = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(tableAddress)
                    + rdS32(img, static_cast<std::size_t>(*tableAt) + 4 * entry));
                auto const own = addressOf(segs, at + body->firstCase + 6 * entry);
                ASSERT_TRUE(own.has_value());
                EXPECT_EQ(lands, *own)
                    << "entry " << entry << " of the table must land on case " << entry
                    << " of the body it was written for: the relocation stands in the group and stays on its "
                       "unit's bytes";
            }
        }
        // ⓘ Until the discard of a losing group exists its bytes are KEPT, unreachable: two bodies. The fold that
        // builds the discard removes the loser's, and this figure becomes 1.
        EXPECT_EQ(bodies, 2u) << "the kept group's body and the loser's kept copy";
    }
}

// THE COMPLEMENT, so the claim cannot pass by refusing nothing: a relocation from OUTSIDE a discarded group that
// names its bytes is still refused by name (fold 2's fixtures; GNU ld: "defined in discarded section").
TEST(GroupMemberRelocations, ARelocationFromOutsideADiscardedGroupIsStillRefusedByName) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable && L.exec);
    std::vector<AssembledModule> units;
    DiagnosticReporter           readRep;
    for (char const* file : {"elf_group_copy_x86_64_elf_gas.o", "elf_group_label_x86_64_elf_gas.o"}) {
        auto unit = readUnit(L, file, static_cast<std::uint32_t>(units.size() + 1), readRep);
        ASSERT_TRUE(unit.has_value()) << file << diagnosticsOf(readRep);
        units.push_back(std::move(*unit));
    }
    auto const* entry = definitionNamed(units[1], "direct_g");
    ASSERT_NE(entry, nullptr);
    units[1].userEntrySymbol = entry->symbol;
    DiagnosticReporter rep;
    auto const img = linker::link(std::span<AssembledModule const>{units}, *L.target, *L.exec, rep,
                                  ImageRequest{.artifactFileName = "group_loser_from_outside"});
    EXPECT_FALSE(img.ok()) << "both reference linkers refuse this pair";
    bool refused = false;
    for (auto const& d : rep.all()) {
        refused = refused || (d.code == DiagnosticCode::K_CrossCuMergeUnsupported
                              && d.actual.find("section group 'shared_g'") != std::string::npos
                              && d.actual.find("names the BYTES") != std::string::npos);
    }
    EXPECT_TRUE(refused) << "the refusal of a relocation from outside the group must stay" << diagnosticsOf(rep);
}
