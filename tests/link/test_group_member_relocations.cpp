// P69 folds 3 and 3c (lane `xa`) — SECTION GROUPS: THE LATER UNIT'S IS DISCARDED WHOLE.
// D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE
//
// ★ WHAT THIS FILE PINS. An ELF COMDAT group is kept or discarded WHOLE: the first unit that carries a signature keeps
// its group, every later unit's goes with everything in it — its bytes, the definitions in it, the relocations
// standing in it — before any name is resolved. Since P69 fold 3c DSS does the same (`discardLaterSectionGroups`,
// link/linker.cpp): the unit states which of its input sections are members of which group
// (`AssembledModule::sectionGroupOfInputSection`), because a member section need hold no named definition — a jump
// table's holds only its section symbol.
//
// ✔MEASURED 2026-10-12 and 2026-10-20 on the tracked objects below (gcc 13.3.0; GNU ld 2.42 and ld.lld 18.1.3, both
// orders, an image and `-r`): the jump-table pair 44 / 88, the two-groups pair 6 / 60, the hidden pair 77 / 99; `-r`
// keeps one copy of each group's sections. Before fold 3c DSS kept the loser's bytes (unreachable) and, for the
// hidden pair, REFUSED two strong definitions of `h` (`K_SymbolRedefinedAcrossUnits`) where both references link.
// The regression of fold 2 this file was first written after (it refused the jump-table pair in both orders, asking
// (unit, target) alone and never where the relocation stands) is gone with the refusal it was in.
//
// Fixtures (x86_64 ELF; each `elf_group_*.source.s` beside them is its source and states its rebuild command; GNU as
// 2.42 through gcc 13.3.0, the same bytes from a second build):
//     elf_group_jump_table_a_x86_64_elf_gas.o   md5 36086c6ca57f6e900b02d10abedff4fa   f(1) = 22
//     elf_group_jump_table_b_x86_64_elf_gas.o   md5 f0324ac6a30ed6c511e2c8db43716020   f(1) = 44, a padded body
//     elf_group_two_groups_{a,b}_x86_64_elf_gas.o   (read at their test below)
//     elf_group_hidden_a_x86_64_elf_gas.o       md5 4b50defb746e4b7727127089babe888d   h() = 7
//     elf_group_hidden_b_x86_64_elf_gas.o       md5 56b84b0015f81312add89d21099b5a7e   h() = 9, after a `nop`
// and, for the refusal that stays (a relocation from outside the group naming its bytes), two of fold 2's:
//     elf_group_copy_x86_64_elf_gas.o, elf_group_label_x86_64_elf_gas.o   (`test_common_symbols.cpp` states them)
//
// ★ NO IMAGE IS RUN HERE: every host reads the linked bytes. Whose body the callers reach, which body each table's
// entries land in, and how many bodies the image holds are read off the image itself.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/macho.hpp"
#include "link/format/pe.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
        // The losing group is DISCARDED, its body with it (until P69 fold 3c DSS kept it, unreachable: 2).
        EXPECT_EQ(bodies, 1u) << "the kept group's body alone: the later unit's group is discarded whole";
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

// ══ A relocation standing in ANOTHER discarded group of its unit (P69 fold 3b, the review of fold 3a, F2) ═══════════
//
// Two units that both carry the COMDAT groups `f` and `g` (`elf_group_two_groups_{a,b}.source.s`): unit B's `g` jumps
// to a non-external label of its own `f` group, so its `.text.g` holds `R_X86_64_PC32 .text.f + 2`. Behind unit A
// BOTH of B's groups are discarded, and no linker processes a relocation of a discarded section.
// ✔MEASURED 2026-10-15 (gcc 13.3.0's objects, md5 a 0f9d895d4f847ae93345972d57dbe2df, b
// 2d922a0b9d866b589754dcaadaf8b963; lane xa's `sb6/groups/ggroups.log`), main returning two_groups_a() +
// two_groups_b(): GNU ld 2.42 and ld.lld 18.1.3, A first 6 (1 + 2, twice), B first 60 (10 + 20, twice). Fold 3a's
// merge refused A first, asking only whether the relocation stood in the SAME group as its target.
// What is read off the image: every call of `f` and of `g` by name lands on the FIRST unit's body, and B's own `g`,
// which jumps to B's own `.Lf_inner`, is in the image only where B's groups are the ones kept.
namespace {

// `pushq %rbx; call f; movl %eax,%ebx; call g; ...` — both callers open so; the calls' displacements at +2 and +9.
[[nodiscard]] bool opensAsTwoGroupsCaller(Image const& img, std::size_t at) {
    return holdsAt(img, at, {0x53, 0xE8}) && holdsAt(img, at + 6, {0x89, 0xC3, 0xE8});
}
[[nodiscard]] std::optional<std::size_t> callTargetOf(Image const& img, std::vector<LoadSegment> const& segs,
                                                      std::size_t displacementAt) {
    auto const next = addressOf(segs, displacementAt + 4);
    if (!next.has_value()) return std::nullopt;
    auto const target =
        offsetOf(segs, static_cast<std::uint64_t>(static_cast<std::int64_t>(*next) + rdS32(img, displacementAt)));
    if (!target.has_value()) return std::nullopt;
    return static_cast<std::size_t>(*target);
}
// `movl $imm, %eax; ret` at `at`.
[[nodiscard]] bool answersAt(Image const& img, std::size_t at, std::uint8_t imm) {
    return holdsAt(img, at, {0xB8, imm, 0x00, 0x00, 0x00, 0xC3});
}

}  // namespace

TEST(GroupMemberRelocations, ARelocationStandingInAnotherDiscardedGroupOfItsUnitLinksInBothOrders) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable && L.exec);
    struct Order {
        char const*  name;
        char const*  first;
        char const*  second;
        char const*  entry;
        std::uint8_t f;   // what the kept `f` answers
        bool         gJumps;   // the kept `g` is B's (`jmp .Lf_inner`), else A's (`movl $2`)
    };
    for (Order const& order :
         {Order{"unit A first", "elf_group_two_groups_a_x86_64_elf_gas.o", "elf_group_two_groups_b_x86_64_elf_gas.o",
                "two_groups_a", 1, false},
          Order{"unit B first", "elf_group_two_groups_b_x86_64_elf_gas.o", "elf_group_two_groups_a_x86_64_elf_gas.o",
                "two_groups_b", 10, true}}) {
        SCOPED_TRACE(order.name);
        LinkedPair linked;
        linkPair(L, order.first, order.second, order.entry, linked);
        ASSERT_TRUE(linked.ok) << "GNU ld 2.42 and ld.lld 18.1.3 link this pair in both orders (6 and 60): the "
                                  "relocation stands in a group the link discards, and is processed by no linker"
                               << linked.diagnostics;
        EXPECT_EQ(linked.reporter.errorCount(), 0u) << linked.diagnostics;
        Image const& img  = linked.image;
        auto const   segs = loadSegmentsOf(img);
        ASSERT_FALSE(segs.empty());
        std::size_t callers = 0;
        for (std::size_t at = 0; at + 16 <= img.size(); ++at) {
            if (!opensAsTwoGroupsCaller(img, at)) continue;
            ++callers;
            auto const f = callTargetOf(img, segs, at + 2);
            auto const g = callTargetOf(img, segs, at + 9);
            ASSERT_TRUE(f.has_value() && g.has_value()) << "a caller's call leaves the image";
            EXPECT_TRUE(answersAt(img, *f, order.f)) << "a call of `f` by name runs the first unit's `f`";
            if (order.gJumps) {
                ASSERT_TRUE(holdsAt(img, *g, {0xE9})) << "a call of `g` by name runs the first unit's `g` (B's jump)";
                auto const lands = callTargetOf(img, segs, *g + 1);
                ASSERT_TRUE(lands.has_value());
                EXPECT_TRUE(answersAt(img, *lands, 20)) << "B's `g` reaches B's own `.Lf_inner`";
            } else {
                EXPECT_TRUE(answersAt(img, *g, 2)) << "a call of `g` by name runs the first unit's `g` (A's)";
            }
        }
        EXPECT_EQ(callers, 2u) << "one caller per unit";
        // B's `g` jumps to B's own `.Lf_inner`; behind unit A it is discarded with both of B's groups.
        std::size_t bJumps = 0;
        for (std::size_t at = 0; at + 5 <= img.size(); ++at) {
            if (img[at] != 0xE9) continue;
            auto const lands = callTargetOf(img, segs, at + 1);
            if (lands.has_value() && answersAt(img, *lands, 20)) ++bJumps;
        }
        EXPECT_EQ(bJumps, order.gJumps ? 1u : 0u)
            << "B's `g` is in the image only where its group is the one kept (until P69 fold 3c: always, unreachable)";
    }
}

// ══ A GROUP WHOSE SIGNATURE IS A STRONG, HIDDEN GLOBAL (P69 fold 3c; the row's part (c)) ═══════════════════════════
//
// `elf_group_hidden_{a,b}.source.s`: each unit carries group `h`, whose member `.text.h` DEFINES `h` — global,
// hidden, STRONG — and calls it by name from `.text`. Two strong definitions of one name: no rule of name resolution
// settles them, and the format never asks one to, because the later group is gone before any name is resolved.
// ✔MEASURED 2026-10-20 (lane xa's `sb11/probes/g1`), main returning group_hidden_a() * 10 + group_hidden_b(): GNU ld
// 2.42 and ld.lld 18.1.3 link both orders, 77 with unit A first and 99 with unit B first. Before fold 3c DSS refused
// both (`K_SymbolRedefinedAcrossUnits`). Read off the image: both callers land on the FIRST unit's `h`, and the
// image holds that body alone.
namespace {

constexpr char const* kHiddenA = "elf_group_hidden_a_x86_64_elf_gas.o";
constexpr char const* kHiddenB = "elf_group_hidden_b_x86_64_elf_gas.o";

// The two bodies of `h`: A's `movl $7,%eax; ret`, B's `nop; movl $9,%eax; ret`.
constexpr std::uint8_t kHiddenBodyA[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};
constexpr std::uint8_t kHiddenBodyB[] = {0x90, 0xB8, 0x09, 0x00, 0x00, 0x00, 0xC3};

template <std::size_t N>
[[nodiscard]] std::vector<std::size_t> occurrencesOf(Image const& img, std::uint8_t const (&body)[N]) {
    std::vector<std::size_t> at;
    for (std::size_t k = 0; k + N <= img.size(); ++k) {
        if (std::equal(std::begin(body), std::end(body), img.begin() + static_cast<std::ptrdiff_t>(k))) at.push_back(k);
    }
    return at;
}

// Every SHT_GROUP record of an ELF64 little-endian relocatable object: its flag word, the name of the symbol its
// `sh_info` names (the signature), its member sections, and whether each member carries SHF_GROUP.
struct GroupRecord {
    bool                       comdat = false;
    std::string                signature;
    std::vector<std::uint32_t> members;
    bool                       everyMemberFlagged = true;
};
[[nodiscard]] std::vector<GroupRecord> groupRecordsOf(Image const& b) {
    std::vector<GroupRecord> out;
    if (b.size() < 64) return out;
    std::uint64_t const shoff = rd(b, 0x28, 8);
    std::uint64_t const shnum = rd(b, 0x3C, 2);
    auto const sh = [&](std::uint64_t i, unsigned field, unsigned bytes) {
        return rd(b, static_cast<std::size_t>(shoff + i * 64 + field), bytes);
    };
    for (std::uint64_t i = 0; i < shnum; ++i) {
        if (sh(i, 4, 4) != 17u) continue;   // SHT_GROUP
        GroupRecord rec;
        std::uint64_t const off = sh(i, 24, 8), size = sh(i, 32, 8);
        rec.comdat = (rd(b, static_cast<std::size_t>(off), 4) & 1u) != 0u;
        for (std::uint64_t k = 4; k + 4 <= size; k += 4) {
            auto const member = static_cast<std::uint32_t>(rd(b, static_cast<std::size_t>(off + k), 4));
            rec.members.push_back(member);
            rec.everyMemberFlagged = rec.everyMemberFlagged && (sh(member, 8, 8) & 0x200u) != 0u;
        }
        std::uint64_t const symtab = sh(i, 40, 4), symIdx = sh(i, 44, 4);
        std::uint64_t const strtab = sh(symtab, 40, 4);
        std::uint64_t const nameOff = rd(b, static_cast<std::size_t>(sh(symtab, 24, 8) + symIdx * 24), 4);
        for (std::size_t c = static_cast<std::size_t>(sh(strtab, 24, 8) + nameOff); c < b.size() && b[c] != 0; ++c) {
            rec.signature.push_back(static_cast<char>(b[c]));
        }
        out.push_back(std::move(rec));
    }
    return out;
}

[[nodiscard]] bool namesTheDiscardedGroup(DiagnosticReporter const& rep, std::string_view group) {
    for (auto const& d : rep.all()) {
        if (d.actual.find("section group '" + std::string{group} + "'") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(GroupMemberRelocations, AGroupWhoseSignatureIsAStrongHiddenNameLinksInBothOrdersOntoTheFirstUnitsCopy) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable && L.exec);
    struct Order {
        char const* name;
        char const* first;
        char const* second;
        char const* entry;
        bool        keptIsA;
    };
    for (Order const& order : {Order{"unit A first", kHiddenA, kHiddenB, "group_hidden_a", true},
                               Order{"unit B first", kHiddenB, kHiddenA, "group_hidden_b", false}}) {
        SCOPED_TRACE(order.name);
        LinkedPair linked;
        linkPair(L, order.first, order.second, order.entry, linked);
        ASSERT_TRUE(linked.ok) << "GNU ld 2.42 and ld.lld 18.1.3 link this pair in both orders (77 and 99): the later "
                                  "unit's group is discarded before its `h` meets the first" << linked.diagnostics;
        EXPECT_EQ(linked.reporter.errorCount(), 0u) << linked.diagnostics;
        Image const& img  = linked.image;
        auto const   segs = loadSegmentsOf(img);
        ASSERT_FALSE(segs.empty());
        auto const a = occurrencesOf(img, kHiddenBodyA);
        auto const b = occurrencesOf(img, kHiddenBodyB);
        ASSERT_EQ(a.size(), order.keptIsA ? 1u : 0u) << "the image holds the FIRST unit's `h` alone";
        ASSERT_EQ(b.size(), order.keptIsA ? 0u : 1u) << "the image holds the FIRST unit's `h` alone";
        std::size_t const kept = order.keptIsA ? a.front() : b.front();
        std::size_t callers = 0;
        for (std::size_t at = 0; at + 5 <= img.size(); ++at) {
            if (img[at] != 0xE9) continue;
            auto const lands = callTargetOf(img, segs, at + 1);
            if (lands.has_value() && *lands == kept) ++callers;
        }
        EXPECT_EQ(callers, 2u) << "both units' `jmp h` land on the kept `h`";
    }
}

// `-r` discards the later group too (✔MEASURED 2026-10-20: `ld -r` and `ld.lld -r` keep one copy of each group's
// sections, and the kept group is written back as a group). Read off the artifact DSS writes, read back: ONE body
// of `h` — the first unit's — under one strong row, and both callers name it; the jump-table pair keeps ONE body of
// `f` and ONE table, both the first unit's.
TEST(GroupMemberRelocations, ARelocatableLinkDiscardsTheLaterGroupToo) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable);
    Image lastBytes;
    auto const relink = [&](char const* first, char const* second, DiagnosticReporter& rep)
        -> std::optional<AssembledModule> {
        std::vector<AssembledModule> units;
        for (char const* file : {first, second}) {
            auto unit = readUnit(L, file, static_cast<std::uint32_t>(units.size() + 1), rep);
            if (!unit.has_value()) return std::nullopt;
            units.push_back(std::move(*unit));
        }
        auto const obj = linker::link(std::span<AssembledModule const>{units}, *L.target, *L.relocatable, rep);
        if (!obj.ok()) return std::nullopt;
        lastBytes = obj.bytes;
        return elf::readRelocatableObject(obj.bytes, *L.target, *L.relocatable, rep, CompilationUnitId{1});
    };
    // The artifact hands the kept group on AS A GROUP (P69 fold 3c, the writer's half): exactly one SHT_GROUP record,
    // GRP_COMDAT, naming the signature, its members flagged SHF_GROUP — and the DSS reader reads the group back.
    auto const handsTheGroupOn = [&](AssembledModule const& artifact, std::string_view signature,
                                     std::size_t membersAtLeast) {
        auto const records = groupRecordsOf(lastBytes);
        ASSERT_EQ(records.size(), 1u) << "one SHT_GROUP record: the kept group's";
        EXPECT_TRUE(records[0].comdat) << "GRP_COMDAT";
        EXPECT_EQ(records[0].signature, signature);
        EXPECT_GE(records[0].members.size(), membersAtLeast);
        EXPECT_TRUE(records[0].everyMemberFlagged) << "every member carries SHF_GROUP";
        std::size_t stated = 0;
        for (auto const& [section, group] : artifact.sectionGroupOfInputSection) {
            EXPECT_EQ(group, signature);
            ++stated;
        }
        EXPECT_GE(stated, membersAtLeast) << "the DSS reader reads the group back off the artifact";
    };
    for (bool const aFirst : {true, false}) {
        SCOPED_TRACE(aFirst ? "the hidden pair, unit A first" : "the hidden pair, unit B first");
        DiagnosticReporter rep;
        auto const artifact = aFirst ? relink(kHiddenA, kHiddenB, rep) : relink(kHiddenB, kHiddenA, rep);
        ASSERT_TRUE(artifact.has_value()) << "both reference linkers link this pair under -r" << diagnosticsOf(rep);
        std::size_t rows = 0;
        SymbolId    h{};
        for (auto const& s : artifact->symbols) {
            if (s.name != "h") continue;
            ++rows;
            h = s.symbol;
            EXPECT_EQ(s.binding, SymbolBinding::Global);
        }
        ASSERT_EQ(rows, 1u) << "one definition of `h`";
        std::size_t bodies = 0;
        for (auto const& fn : artifact->functions) {
            Image const bytes(fn.bytes.begin(), fn.bytes.end());
            std::size_t const ofA = occurrencesOf(bytes, kHiddenBodyA).size();
            std::size_t const ofB = occurrencesOf(bytes, kHiddenBodyB).size();
            bodies += ofA + ofB;
            if (ofA + ofB != 0u) {
                EXPECT_EQ(fn.symbol, h) << "the body is the definition `h` names";
                EXPECT_EQ(ofA, aFirst ? 1u : 0u) << "the FIRST unit's body is the one kept";
            }
        }
        EXPECT_EQ(bodies, 1u) << "the later unit's group is discarded under -r too";
        handsTheGroupOn(*artifact, "h", 1u);
        for (char const* caller : {"group_hidden_a", "group_hidden_b"}) {
            EXPECT_NE(definitionNamed(*artifact, caller), nullptr) << caller << " is kept: it lies in no group";
        }
        for (auto const& ext : artifact->externImports) {
            EXPECT_NE(ext.mangledName, "h") << "the later unit's `jmp h` is bound to the kept `h`, never left undefined";
        }
    }
    for (bool const aFirst : {true, false}) {
        SCOPED_TRACE(aFirst ? "the jump-table pair, unit A first" : "the jump-table pair, unit B first");
        DiagnosticReporter rep;
        auto const artifact = aFirst ? relink(kUnitA, kUnitB, rep) : relink(kUnitB, kUnitA, rep);
        ASSERT_TRUE(artifact.has_value()) << diagnosticsOf(rep);
        std::size_t bodies = 0;
        for (auto const& fn : artifact->functions) {
            Image const bytes(fn.bytes.begin(), fn.bytes.end());
            for (std::size_t at = 0; at < bytes.size(); ++at) {
                Body const* body = bodyAt(bytes, at);
                if (body == nullptr) continue;
                ++bodies;
                EXPECT_STREQ(body->unit, aFirst ? kBodyA.unit : kBodyB.unit) << "the FIRST unit's `f` is kept";
            }
        }
        EXPECT_EQ(bodies, 1u) << "one body of `f`";
        std::size_t tables = 0;
        for (auto const& di : artifact->dataItems) tables += di.relocations.size() == 2u ? 1u : 0u;
        EXPECT_EQ(tables, 1u) << "one jump table: the later unit's goes with its group";
        handsTheGroupOn(*artifact, "f", 2u);   // `.text.f` and `.rodata.f` (and their tables)
    }
}

// What a KEPT part of the unit names that is a DEFINITION, never a name — the unit's entry, its image entry, a
// constructor schedule, an SEH scope — cannot follow a name to the kept copy. No object reader states one beside a
// group; the link refuses each by name rather than point it at another unit's copy. CONTROL: the same statement on
// the KEEPER's unit names a definition that is in the link, and no discard is refused.
TEST(GroupMemberRelocations, AKeptPartOfAUnitThatNamesADiscardedDefinitionItselfIsRefusedByName) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.exec);
    enum class Site : std::uint8_t { Entry, ImageEntry, ConstructorSchedule, SehScope };
    struct Cell {
        Site        site;
        char const* says;
    };
    for (Cell const& cell : {Cell{Site::Entry, "the entry"}, Cell{Site::ImageEntry, "the image entry"},
                             Cell{Site::ConstructorSchedule, "a constructor schedule"},
                             Cell{Site::SehScope, "an SEH scope"}}) {
        for (std::size_t const at : {std::size_t{1}, std::size_t{0}}) {
            SCOPED_TRACE(std::string{cell.says} + (at == 1 ? ", on the unit whose group is discarded"
                                                           : ", on the keeper (CONTROL)"));
            std::vector<AssembledModule> units;
            DiagnosticReporter           readRep;
            for (char const* file : {kHiddenA, kHiddenB}) {
                auto unit = readUnit(L, file, static_cast<std::uint32_t>(units.size() + 1), readRep);
                ASSERT_TRUE(unit.has_value()) << file << diagnosticsOf(readRep);
                units.push_back(std::move(*unit));
            }
            AssembledModule& u = units[at];
            auto const*      h = definitionNamed(u, "h");
            ASSERT_NE(h, nullptr);
            SymbolId const hId = h->symbol;
            if (cell.site != Site::Entry) units[0].userEntrySymbol = definitionNamed(units[0], "group_hidden_a")->symbol;
            switch (cell.site) {
                case Site::Entry:
                    u.userEntrySymbol = hId;
                    break;
                case Site::ImageEntry:
                    for (std::size_t k = 0; k < u.functions.size(); ++k) {
                        if (u.functions[k].symbol == hId) u.imageEntryOverride = k;
                    }
                    ASSERT_TRUE(u.imageEntryOverride.has_value());
                    break;
                case Site::ConstructorSchedule:
                    u.staticInitSchedule.push_back(LirStaticInitEntry{hId, {}});
                    break;
                case Site::SehScope: {
                    auto const* caller = definitionNamed(u, at == 0 ? "group_hidden_a" : "group_hidden_b");
                    ASSERT_NE(caller, nullptr);
                    for (auto& fn : u.functions) {
                        if (fn.symbol == caller->symbol) fn.sehScopes.push_back(SehScopeEntry{0, 1, 0, hId, {}});
                    }
                    break;
                }
            }
            DiagnosticReporter rep;
            auto const img = linker::link(std::span<AssembledModule const>{units}, *L.target, *L.exec, rep,
                                          ImageRequest{.artifactFileName = "group_hidden_site"});
            if (at == 1) {
                EXPECT_FALSE(img.ok());
                bool refused = false;
                for (auto const& d : rep.all()) {
                    refused = refused || (d.code == DiagnosticCode::K_CrossCuMergeUnsupported
                                          && d.actual.find(std::string{cell.says} + " of linked unit #1 names")
                                                 != std::string::npos
                                          && d.actual.find("section group 'h'") != std::string::npos
                                          && d.actual.find("linked unit #0 carries too") != std::string::npos);
                }
                EXPECT_TRUE(refused) << diagnosticsOf(rep);
            } else {
                EXPECT_FALSE(namesTheDiscardedGroup(rep, "h"))
                    << "the keeper's `h` is in the link: nothing names a discarded definition" << diagnosticsOf(rep);
            }
        }
    }
}

// ══ THE ONE INSTANCE OF ITS NAME IN THE PROCESS (P69 fold 3c) ═══════════════════════════════════════════════════════
//
// g++ 13.3 writes a C++ inline variable as `OBJECT UNIQUE` (STB_GNU_UNIQUE, binding 10) in a COMDAT group named by the
// symbol, under OS/ABI GNU. The ELF documents state the binding (`elf.uniqueBinding`: 10, gnu, read as global); the
// reader reads it through the document and carries the property (`ModuleSymbol::oneInstancePerProcess`); the writers
// spell it back, and every writer whose document states no such binding refuses it by name.
// ✔MEASURED 2026-10-20 (g++ 13.3.0, GNU ld 2.42, ld.lld 18.1.3): the two units' `&counter` are ONE address in both
// orders (the native program 41); `ld -r` and `ld.lld -r` keep binding 10, one group and OS/ABI GNU; a shared library of
// the -fPIC twins exports `counter` with binding 10 in `.dynsym` under both. Until fold 3c the reader read binding 10
// as LOCAL, and DSS linked the two units to TWO objects, silently (✔MEASURED: the native program 5).
//
// Fixtures (x86_64 ELF, g++ 13.3.0 -std=c++17 -O1 -c; `elf_unique_counter_{a,b}.source.cpp` is the source):
//     elf_unique_counter_a_x86_64_elf_gxx.o       md5 9778a674f692012126d43b011e02bbfb   addr1() { return &counter; }
//     elf_unique_counter_b_x86_64_elf_gxx.o       md5 bb03cc7e347a4ff0262a20ff5350cba8   addr2() { return &counter; }
//     elf_unique_counter_a_pic_x86_64_elf_gxx.o   md5 d58b2d3466dbd41beceab45c74e12015   the same, -fPIC
//     elf_unique_counter_b_pic_x86_64_elf_gxx.o   md5 8d68c5c9068c408b81df8ffe2b1a2ed7   the same, -fPIC
// The non-PIC bodies are `endbr64; lea counter(%rip),%rax; ret`: the displacement at +7, the next instruction at +11.
namespace {

constexpr char const* kUniqueA    = "elf_unique_counter_a_x86_64_elf_gxx.o";
constexpr char const* kUniqueB    = "elf_unique_counter_b_x86_64_elf_gxx.o";
constexpr char const* kUniquePicA = "elf_unique_counter_a_pic_x86_64_elf_gxx.o";
constexpr char const* kUniquePicB = "elf_unique_counter_b_pic_x86_64_elf_gxx.o";
constexpr std::uint8_t kAddrOpen[] = {0xF3, 0x0F, 0x1E, 0xFA, 0x48, 0x8D, 0x05};

// One symbol record of an ELF64 little-endian file.
struct SymbolRecord {
    std::string   name;
    unsigned      binding = 0;
    std::uint16_t shndx   = 0;
    std::size_t   at      = 0;   // the record's file offset
};
// The records named `name` of every section of type `shType` (2: SHT_SYMTAB, 11: SHT_DYNSYM).
[[nodiscard]] std::vector<SymbolRecord> recordsNamed(Image const& b, std::uint64_t shType, std::string_view name) {
    std::vector<SymbolRecord> out;
    if (b.size() < 64) return out;
    std::uint64_t const shoff = rd(b, 0x28, 8);
    std::uint64_t const shnum = rd(b, 0x3C, 2);
    auto const sh = [&](std::uint64_t i, unsigned field, unsigned bytes) {
        return rd(b, static_cast<std::size_t>(shoff + i * 64 + field), bytes);
    };
    for (std::uint64_t i = 0; i < shnum; ++i) {
        if (sh(i, 4, 4) != shType) continue;
        std::uint64_t const off = sh(i, 24, 8), size = sh(i, 32, 8), strOff = sh(sh(i, 40, 4), 24, 8);
        for (std::uint64_t r = off; r + 24 <= off + size; r += 24) {
            SymbolRecord rec;
            rec.at      = static_cast<std::size_t>(r);
            rec.binding = static_cast<unsigned>(rd(b, rec.at + 4, 1) >> 4);
            rec.shndx   = static_cast<std::uint16_t>(rd(b, rec.at + 6, 2));
            for (auto c = static_cast<std::size_t>(strOff + rd(b, rec.at, 4)); c < b.size() && b[c] != 0; ++c) {
                rec.name.push_back(static_cast<char>(b[c]));
            }
            if (rec.name == name) out.push_back(std::move(rec));
        }
    }
    return out;
}

// `b` with the binding of its one `.symtab` record named `name` set to `binding`.
[[nodiscard]] Image withBinding(Image b, std::string_view name, unsigned binding) {
    auto const named = recordsNamed(b, 2u, name);
    EXPECT_EQ(named.size(), 1u) << name;
    if (named.size() == 1u) {
        b[named[0].at + 4] = static_cast<std::uint8_t>((binding << 4) | (b[named[0].at + 4] & 0x0Fu));
    }
    return b;
}

// The shipped document `stem` WITHOUT its `uniqueBinding` statement (and the prose beside it), loaded from text: a
// document that states no meaning for the binding.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> withoutTheUniqueBinding(char const* stem) {
    auto const path = dss::test::repoRoot() / "src" / "dss-config" / "object-formats" / (std::string{stem} + ".format.json");
    std::ifstream in{path, std::ios::binary};
    std::string   text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto const             key  = text.find("\"$uniqueBindingComment\"");
    auto const             from = key == std::string::npos ? std::string::npos : text.rfind(',', key);
    std::string_view const last = R"("readAs": "global" })";
    auto const             to   = from == std::string::npos ? std::string::npos : text.find(last, from);
    EXPECT_NE(to, std::string::npos) << stem << " states no uniqueBinding to take out";
    if (to == std::string::npos) return {};
    text.erase(from, to + last.size() - from);
    auto loaded = ObjectFormatSchema::loadFromText(text, stem);
    EXPECT_TRUE(loaded.has_value()) << stem << " without its uniqueBinding must still load";
    if (!loaded.has_value()) return {};
    EXPECT_FALSE((*loaded)->elf().uniqueBinding.has_value());
    return *loaded;
}

// The shipped document `stem` with its header's OS ABI (`elf.osabi`, "sysv" as shipped) spelled `spelling`, loaded from
// text; its `uniqueBinding` statement stays whole.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> withHeaderOsAbi(char const* stem, std::string_view spelling) {
    auto const path = dss::test::repoRoot() / "src" / "dss-config" / "object-formats" / (std::string{stem} + ".format.json");
    std::ifstream in{path, std::ios::binary};
    std::string   text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::string_view const shipped = R"("osabi": "sysv")";
    auto const             at      = text.find(shipped);
    EXPECT_NE(at, std::string::npos) << stem << " states no header OS ABI to respell";
    EXPECT_EQ(text.find(shipped, at + 1), std::string::npos) << stem << " states it once";
    if (at == std::string::npos) return {};
    text.replace(at, shipped.size(), std::string{R"("osabi": ")"} + std::string{spelling} + "\"");
    auto loaded = ObjectFormatSchema::loadFromText(text, stem);
    EXPECT_TRUE(loaded.has_value()) << stem << " with header OS ABI '" << spelling << "' must load";
    if (!loaded.has_value()) return {};
    EXPECT_TRUE((*loaded)->elf().uniqueBinding.has_value());
    return *loaded;
}

[[nodiscard]] bool refusedNamingCounter(DiagnosticReporter const& rep, std::string_view words) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolBindingNotModelled && d.actual.find("'counter'") != std::string::npos
            && d.actual.find(words) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Both units of a pair, read in order (cu 1, 2).
[[nodiscard]] std::vector<AssembledModule> uniquePair(Loaded const& L, char const* first, char const* second,
                                                      DiagnosticReporter& rep) {
    std::vector<AssembledModule> units;
    for (char const* file : {first, second}) {
        auto unit = readUnit(L, file, static_cast<std::uint32_t>(units.size() + 1), rep);
        EXPECT_TRUE(unit.has_value()) << file << diagnosticsOf(rep);
        if (unit.has_value()) units.push_back(std::move(*unit));
    }
    return units;
}

}  // namespace

// The reader reads the document's binding as the document says — a GLOBAL definition that is the one instance — whatever
// OS ABI the object's header states, as both references do (✔MEASURED 2026-10-20, headers patched to NONE and FreeBSD:
// 41 under GNU ld and ld.lld). A binding no format shares and the document does not state is refused by name, as
// ld.lld refuses it ("has invalid binding"); and so is binding 10 under a document that states no meaning for it —
// never read as LOCAL, as it was until fold 3c.
TEST(OneInstancePerProcess, TheReaderReadsTheDocumentsBindingAsAGlobalDefinitionThatIsTheOneInstance) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable);
    Image const bytes = trackedObject(kUniqueA);
    auto const  wrote = recordsNamed(bytes, 2u, "counter");
    ASSERT_EQ(wrote.size(), 1u);
    ASSERT_EQ(wrote[0].binding, 10u) << "the fixture is what g++ wrote: OBJECT UNIQUE";
    ASSERT_EQ(bytes[7], 3u) << "under OS/ABI GNU";
    for (std::uint8_t const osabi : {std::uint8_t{3}, std::uint8_t{0}, std::uint8_t{9}}) {
        SCOPED_TRACE("EI_OSABI " + std::to_string(osabi));
        Image b = bytes;
        b[7]    = osabi;
        DiagnosticReporter rep;
        auto const unit = elf::readRelocatableObject(b, *L.target, *L.relocatable, rep, CompilationUnitId{1});
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        auto const* counter = definitionNamed(*unit, "counter");
        ASSERT_NE(counter, nullptr);
        EXPECT_EQ(counter->binding, SymbolBinding::Global) << "read as the document says: global";
        EXPECT_TRUE(counter->oneInstancePerProcess) << "and the property is carried";
        auto const* addr1 = definitionNamed(*unit, "addr1");
        ASSERT_NE(addr1, nullptr);
        EXPECT_FALSE(addr1->oneInstancePerProcess) << "an ordinary global is no such instance";
    }
    for (unsigned const binding : {3u, 11u, 13u}) {
        SCOPED_TRACE("binding " + std::to_string(binding));
        DiagnosticReporter rep;
        EXPECT_FALSE(elf::readRelocatableObject(withBinding(bytes, "counter", binding), *L.target, *L.relocatable, rep,
                                                CompilationUnitId{1})
                         .has_value());
        EXPECT_TRUE(refusedNamingCounter(rep, "has binding " + std::to_string(binding) + ", which format"))
            << diagnosticsOf(rep);
    }
    auto const silent = withoutTheUniqueBinding("elf64-x86_64-linux");
    ASSERT_TRUE(silent);
    DiagnosticReporter rep;
    EXPECT_FALSE(elf::readRelocatableObject(bytes, *L.target, *silent, rep, CompilationUnitId{1}).has_value());
    EXPECT_TRUE(refusedNamingCounter(rep, "states no binding for a definition of which a process holds one instance"))
        << diagnosticsOf(rep);
}

// THE ADDRESS-EQUALITY CASE, read off the image: both units' `&counter` reach ONE object holding 5, in both orders, and
// the header states the OS ABI the binding requires, as GNU ld's image does. Read as LOCAL — the mapping until fold 3c —
// the later unit's kept relocation names a definition its discarded group took with it, and the link fails here.
TEST(OneInstancePerProcess, TwoUnitsInlineVariableIsOneObjectInBothOrders) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable && L.exec);
    for (bool const aFirst : {true, false}) {
        SCOPED_TRACE(aFirst ? "unit A first" : "unit B first");
        LinkedPair linked;
        linkPair(L, aFirst ? kUniqueA : kUniqueB, aFirst ? kUniqueB : kUniqueA, aFirst ? "addr1" : "addr2", linked);
        ASSERT_TRUE(linked.ok) << "both reference linkers link this pair" << linked.diagnostics;
        auto const segs   = loadSegmentsOf(linked.image);
        auto const bodies = occurrencesOf(linked.image, kAddrOpen);
        ASSERT_EQ(bodies.size(), 2u) << "addr1 and addr2";
        std::vector<std::uint64_t> targets;
        for (std::size_t const at : bodies) {
            ASSERT_TRUE(holdsAt(linked.image, at + 11, {0xC3}));
            auto const next = addressOf(segs, at + 11);
            ASSERT_TRUE(next.has_value());
            targets.push_back(static_cast<std::uint64_t>(static_cast<std::int64_t>(*next) + rdS32(linked.image, at + 7)));
        }
        EXPECT_EQ(targets[0], targets[1]) << "both units' `&counter` are one address: ONE object";
        auto const counterAt = offsetOf(segs, targets[0]);
        ASSERT_TRUE(counterAt.has_value());
        EXPECT_TRUE(holdsAt(linked.image, *counterAt, {0x05, 0x00, 0x00, 0x00})) << "`counter` holds its 5";
        EXPECT_EQ(linked.image[7], 3u) << "the header states OS/ABI GNU, as GNU ld's image does";
    }
}

// The relocatable link hands the one instance on as g++ wrote it — ONE `counter` record, binding 10, in the kept group,
// under OS/ABI GNU, what `ld -r` writes — and the DSS reader reads it back as the same definition. Written under a
// document that states no binding for it, or bound otherwise than the document reads it, it is refused by name. Under a
// header that states an OS ABI, the binding's own (GNU) or another (FreeBSD), the binding is written all the same, as
// ld.lld 18.1.3 writes it under SYSV and FreeBSD headers (✔MEASURED 2026-10-21; GNU ld 2.42 as Ubuntu ships it carries
// no output format of another OS ABI to measure). That the references link the FreeBSD artifact to ONE object is
// `OneInstancePerProcessNative.TwoUnitsInlineVariableIsOneObjectAsUnderGnuLdAndLld`'s cell.
TEST(OneInstancePerProcess, ARelocatableLinkWritesTheDocumentsBindingBackAndRefusesWhereItCannot) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable);
    for (bool const aFirst : {true, false}) {
        SCOPED_TRACE(aFirst ? "unit A first" : "unit B first");
        DiagnosticReporter rep;
        auto const units = uniquePair(L, aFirst ? kUniqueA : kUniqueB, aFirst ? kUniqueB : kUniqueA, rep);
        ASSERT_EQ(units.size(), 2u);
        auto const obj = linker::link(std::span<AssembledModule const>{units}, *L.target, *L.relocatable, rep);
        ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
        Image const& b = obj.bytes;
        EXPECT_EQ(b[7], 3u) << "OS/ABI GNU, as `ld -r` writes it";
        auto const records = recordsNamed(b, 2u, "counter");
        ASSERT_EQ(records.size(), 1u) << "one `counter` record: the later unit's group is discarded, its reference bound";
        EXPECT_NE(records[0].shndx, 0u) << "a definition";
        EXPECT_EQ(records[0].binding, 10u) << "with the document's binding, as g++ wrote it";
        auto const groups = groupRecordsOf(b);
        ASSERT_EQ(groups.size(), 1u);
        EXPECT_EQ(groups[0].signature, "counter");
        auto const back = elf::readRelocatableObject(b, *L.target, *L.relocatable, rep, CompilationUnitId{1});
        ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
        auto const* counter = definitionNamed(*back, "counter");
        ASSERT_NE(counter, nullptr);
        EXPECT_EQ(counter->binding, SymbolBinding::Global);
        EXPECT_TRUE(counter->oneInstancePerProcess) << "the artifact's `counter` reads back as the one instance";
    }
    {
        SCOPED_TRACE("written under a document that states no binding for it");
        auto const silent = withoutTheUniqueBinding("elf64-x86_64-linux");
        ASSERT_TRUE(silent);
        DiagnosticReporter rep;
        auto const units = uniquePair(L, kUniqueA, kUniqueB, rep);
        ASSERT_EQ(units.size(), 2u);
        EXPECT_FALSE(linker::link(std::span<AssembledModule const>{units}, *L.target, *silent, rep).ok());
        EXPECT_TRUE(refusedNamingCounter(rep, "states no binding that spells one")) << diagnosticsOf(rep);
    }
    {
        SCOPED_TRACE("a row bound otherwise than the document reads it");
        DiagnosticReporter rep;
        auto unit = readUnit(L, kUniqueA, 1, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        for (auto& s : unit->symbols) {
            if (s.name == "counter") s.binding = SymbolBinding::Weak;
        }
        std::vector<AssembledModule> one;
        one.push_back(std::move(*unit));
        EXPECT_FALSE(linker::link(std::span<AssembledModule const>{one}, *L.target, *L.relocatable, rep).ok());
        EXPECT_TRUE(refusedNamingCounter(rep, "bound 'weak'")) << diagnosticsOf(rep);
    }
    // Under a header that states an OS ABI — the binding's own (GNU) or ANOTHER (FreeBSD) — the header keeps it and the
    // one instance keeps the document's binding, as ld.lld writes it; DSS reads the artifact back as the one instance.
    struct HeaderCase {
        char const*  spelling;
        std::uint8_t osabi;
    };
    for (HeaderCase const h : {HeaderCase{"gnu", 3}, HeaderCase{"freebsd", 9}}) {
        SCOPED_TRACE(std::string{"written under a header that states OS ABI '"} + h.spelling + "'");
        auto const doc = withHeaderOsAbi("elf64-x86_64-linux", h.spelling);
        ASSERT_TRUE(doc);
        DiagnosticReporter rep;
        auto const units = uniquePair(L, kUniqueA, kUniqueB, rep);
        ASSERT_EQ(units.size(), 2u);
        auto const obj = linker::link(std::span<AssembledModule const>{units}, *L.target, *doc, rep);
        ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
        EXPECT_EQ(obj.bytes[7], h.osabi) << "the header the document states";
        auto const records = recordsNamed(obj.bytes, 2u, "counter");
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].binding, 10u) << "under the header's OS ABI too, the document's binding";
        auto const back = elf::readRelocatableObject(obj.bytes, *L.target, *L.relocatable, rep, CompilationUnitId{1});
        ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
        auto const* counter = definitionNamed(*back, "counter");
        ASSERT_NE(counter, nullptr);
        EXPECT_TRUE(counter->oneInstancePerProcess);
    }
}

// A shared library hands the one instance to the dynamic linker: its `.dynsym` export of `counter` carries the
// document's binding, the record by which ld.so binds every image's `counter` to one definition (✔MEASURED 2026-10-20:
// GNU ld 2.42 and ld.lld 18.1.3, `-shared` of the -fPIC twins, `counter` UNIQUE in `.dynsym`; both refuse the non-PIC
// pair).
TEST(OneInstancePerProcess, ASharedLibraryExportsTheOneInstanceWithTheDocumentsBinding) {
    auto const L   = loadElfX8664();
    auto const dyn = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(L.target && L.relocatable && dyn.has_value());
    DiagnosticReporter rep;
    auto const units = uniquePair(L, kUniquePicA, kUniquePicB, rep);
    ASSERT_EQ(units.size(), 2u);
    auto const so = linker::link(std::span<AssembledModule const>{units}, *L.target, **dyn, rep,
                                 ImageRequest{.artifactFileName = "libunique.so"});
    ASSERT_TRUE(so.ok()) << diagnosticsOf(rep);
    auto const exported = recordsNamed(so.bytes, 11u, "counter");
    ASSERT_EQ(exported.size(), 1u) << "one `.dynsym` export of `counter`";
    EXPECT_EQ(exported[0].binding, 10u) << "the document's binding";
    EXPECT_EQ(so.bytes[7], 3u) << "OS/ABI GNU, as GNU ld's library states it";
}

// Every writer whose document states no binding for the one instance refuses it by name, before any byte: PE's and
// Mach-O's, whose documents state none, as the ELF writer does under a document stripped of its statement — never a
// plain definition, one instance per image.
TEST(OneInstancePerProcess, EveryWriterWhoseDocumentStatesNoBindingForItRefusesTheOneInstance) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable);
    DiagnosticReporter readRep;
    auto const unit = readUnit(L, kUniqueA, 1, readRep);
    ASSERT_TRUE(unit.has_value()) << diagnosticsOf(readRep);
    auto const pe   = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    auto const mach = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(pe.has_value() && mach.has_value());
    {
        SCOPED_TRACE("pe::encode");
        DiagnosticReporter rep;
        EXPECT_TRUE(pe::encode(*unit, *L.target, **pe, rep).empty()) << "no byte for a one instance it cannot spell";
        EXPECT_TRUE(refusedNamingCounter(rep, "pe::encode")) << diagnosticsOf(rep);
    }
    {
        SCOPED_TRACE("macho::encode");
        DiagnosticReporter rep;
        EXPECT_TRUE(macho::encode(*unit, *L.target, **mach, rep).empty())
            << "no byte for a one instance it cannot spell";
        EXPECT_TRUE(refusedNamingCounter(rep, "macho::encode")) << diagnosticsOf(rep);
    }
}

// A group whose signature NAMES no definition of the unit is named, in DSS's relocatable artifact, by a LOCAL symbol of
// that name at the start of its first member — what GNU as writes for `.section <s>,"axG",@progbits,<signature>,comdat`
// when nothing in the object defines the signature — and the DSS reader reads the group back under that signature.
// The jump-table unit's group `f` is restated under a signature no row carries.
TEST(GroupMemberRelocations, AGroupWhoseSignatureNamesNoDefinitionIsNamedByALocalSymbolOfThatName) {
    auto const L = loadElfX8664();
    ASSERT_TRUE(L.target && L.relocatable);
    DiagnosticReporter rep;
    auto unit = readUnit(L, kUnitA, 1, rep);
    ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
    ASSERT_FALSE(unit->sectionGroupOfInputSection.empty());
    for (auto& [section, group] : unit->sectionGroupOfInputSection) group = "f.signature";
    std::vector<AssembledModule> one;
    one.push_back(std::move(*unit));
    auto const obj = linker::link(std::span<AssembledModule const>{one}, *L.target, *L.relocatable, rep);
    ASSERT_TRUE(obj.ok()) << diagnosticsOf(rep);
    auto const groups = groupRecordsOf(obj.bytes);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].signature, "f.signature");
    auto const named = recordsNamed(obj.bytes, 2u, "f.signature");
    ASSERT_EQ(named.size(), 1u) << "one record names the signature";
    EXPECT_EQ(named[0].binding, 0u) << "a LOCAL symbol: nothing of the unit defines that name";
    auto const back = elf::readRelocatableObject(obj.bytes, *L.target, *L.relocatable, rep, CompilationUnitId{1});
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    std::size_t stated = 0;
    for (auto const& [section, group] : back->sectionGroupOfInputSection) {
        EXPECT_EQ(group, "f.signature");
        ++stated;
    }
    EXPECT_GE(stated, 2u) << "`.text.f` and `.rodata.f` read back as members of the group";
}
