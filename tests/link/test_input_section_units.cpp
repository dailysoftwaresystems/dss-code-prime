// ★★★ AN ELF OR PE/COFF INPUT SECTION IS THE UNIT OF PLACEMENT (P68 round 9).
//
// The readers cut a relocatable object's sections into symbol-bounded atoms.
// The object's FORMAT decides whether those atoms may then move independently
// (`inputSectionPlacement`): never for ELF and PE/COFF, and for Mach-O only
// when the object declares MH_SUBSECTIONS_VIA_SYMBOLS. Code in the object
// depends on the section's layout with nothing a reader can see.
//
// ✔MEASURED 2026-09-23 against the reference links, before this landed:
//   * gcc -O2 `static int y, x` stores (`gccStaticStoresObject`): DSS ran the
//     program to 33 where gcc's link runs to 42. gas reduced each store to
//     "section + offset" with the imm32 folded into the addend, the reader
//     bound it through the neighbouring object, and the merge moved the two
//     apart;
//   * the same store to the section's FIRST object: refused LOUD;
//   * `a+8` read as `b` (`gasSymbolPlusOffsetObject`): DSS ran to 2, not 42;
//   * objects walked between two labels in one named section: refused LOUD;
//   * mingw gcc's section-symbol references, and its long section names: refused.
// Each pin below computes, from the reconstructed module and the layout the
// writers use, the address the producer's instruction reaches, and checks it
// is the object that instruction names. A link that merely succeeds would not
// prove anything here: the 33 was a successful link.
//
// RED-ON-DISABLE, per pin family (each mutant BUILT and READ through ctest):
//   * `buildExecDataSection` ignoring `inputSection` (every item laid out as a
//     free item) -> the ELF address pins and `ACleanUnitLaysOutAtItsOwnOffsets`
//     fail on the moved object;
//   * `inputSectionsAreUnits` returning false -> the stamping and address
//     pins fail, and the offset-0 and end-marker pins go back to a refusal;
//   * `validateInputSectionUnits` returning true unconditionally -> every
//     `K_InputSectionSplit` pin fails (no diagnostic).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/exec_data_section.hpp"
#include "link/format/macho_object_reader.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include "input_section_unit_objects.inc"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

struct Shipped {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Shipped loadShipped(std::string const& arch, std::string const& format) {
    Shipped s;
    if (auto t = TargetSchema::loadShipped(arch)) s.target = *t;
    if (auto f = ObjectFormatSchema::loadShipped(format)) s.format = *f;
    return s;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) n += (d.code == code) ? 1u : 0u;
    return n;
}

// Every diagnostic's text, for a failure message that says WHY a read failed.
[[nodiscard]] std::string describe(DiagnosticReporter const& rep) {
    std::string out = "errors=" + std::to_string(rep.errorCount());
    for (auto const& d : rep.all()) out += "\n  " + d.actual;
    return out;
}

[[nodiscard]] std::optional<SymbolId> symbolNamed(AssembledModule const& m,
                                                  std::string const& name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return s.symbol;
    }
    return std::nullopt;
}

[[nodiscard]] AssembledFunction const* functionNamed(AssembledModule const& m,
                                                     std::string const& name) {
    auto const id = symbolNamed(m, name);
    if (!id.has_value()) return nullptr;
    for (auto const& f : m.functions) {
        if (f.symbol == *id) return &f;
    }
    return nullptr;
}

// Where every data item of `kind` lands in the section the writers build:
// the SAME `buildExecDataSection` every ELF / PE / Mach-O writer calls.
struct Layout {
    std::map<std::uint32_t, std::uint64_t> offsetBySymbol;   // SymbolId.v -> offset
    bool ok = false;
};

[[nodiscard]] Layout layOut(AssembledModule const& m, DataSectionKind kind,
                            DiagnosticReporter& rep) {
    Layout out;
    auto const layout = link::format::buildExecDataSection(
        m.dataItems, kind, /*sectionAlignFloor=*/1, "input-section-units test", rep,
        /*allowItemRelocations=*/true);
    if (!layout.has_value()) return out;
    for (std::size_t j = 0; j < layout->itemIndices.size(); ++j) {
        out.offsetBySymbol[m.dataItems[layout->itemIndices[j]].symbol.v] =
            layout->itemOffsets[j];
    }
    out.ok = true;
    return out;
}

// The section offset a PC-relative load or store REACHES, from the relocation
// the reader reconstructed: the target's laid-out offset plus the DSS addend,
// plus the bytes the instruction carries after its field. The effective
// address is next-instruction + field, and the field is S + A - 4 - P.
[[nodiscard]] std::optional<std::int64_t> reached(Layout const& layout,
                                                  Relocation const& rel,
                                                  std::int64_t bytesAfterField) {
    auto const it = layout.offsetBySymbol.find(rel.target.v);
    if (it == layout.offsetBySymbol.end()) return std::nullopt;
    return static_cast<std::int64_t>(it->second) + rel.addend + bytesAfterField;
}

[[nodiscard]] std::optional<std::int64_t> offsetOf(Layout const& layout,
                                                   AssembledModule const& m,
                                                   std::string const& name) {
    auto const id = symbolNamed(m, name);
    if (!id.has_value()) return std::nullopt;
    auto const it = layout.offsetBySymbol.find(id->v);
    if (it == layout.offsetBySymbol.end()) return std::nullopt;
    return static_cast<std::int64_t>(it->second);
}

// ── hand-built modules for the unit invariant itself ─────────────────────────

[[nodiscard]] AssembledData dataItem(std::uint32_t id, DataSectionKind kind,
                                     std::size_t size, std::uint32_t align,
                                     std::optional<InputSectionSlice> unit) {
    AssembledData d;
    d.symbol       = SymbolId{id};
    d.section      = kind;
    d.alignment    = Alignment::fromBytes(align).value_or(Alignment{});
    d.inputSection = unit;
    if (isZeroFill(kind)) d.reservedSize = size;
    else                  d.bytes.assign(size, static_cast<std::uint8_t>(id));
    return d;
}

[[nodiscard]] AssembledFunction function(std::uint32_t id, std::size_t size,
                                         std::optional<InputSectionSlice> unit) {
    AssembledFunction f;
    f.symbol       = SymbolId{id};
    f.bytes.assign(size, 0x90);
    f.inputSection = unit;
    return f;
}

} // namespace

// ── THE UNIT INVARIANT: a clean unit, and every way to split one ──────────────

TEST(InputSectionUnits, ACleanUnitLaysOutAtItsOwnOffsets) {
    // A free 5-byte item first, so the unit has to find its own aligned base.
    // The unit's members are EMITTED OUT OF ORDER (the readers append gap
    // atoms after the named ones), and every member carries the section's
    // 16-byte alignment, which is what used to move `b` to a+16.
    AssembledModule m;
    m.dataItems.push_back(dataItem(1, DataSectionKind::Data, 5, 1, std::nullopt));
    m.dataItems.push_back(dataItem(4, DataSectionKind::Data, 16, 16, InputSectionSlice{7, 16}));
    m.dataItems.push_back(dataItem(2, DataSectionKind::Data, 4, 16, InputSectionSlice{7, 0}));
    m.dataItems.push_back(dataItem(3, DataSectionKind::Data, 4, 16, InputSectionSlice{7, 4}));
    DiagnosticReporter rep;
    ASSERT_TRUE(validateInputSectionUnits(m, rep));
    auto const layout = layOut(m, DataSectionKind::Data, rep);
    ASSERT_TRUE(layout.ok);
    EXPECT_EQ(rep.errorCount(), 0u);
    std::uint64_t const base = layout.offsetBySymbol.at(2);
    EXPECT_EQ(base % 16u, 0u) << "the unit starts at its strictest alignment";
    EXPECT_GE(base, 5u) << "after the free item, never over it";
    EXPECT_EQ(layout.offsetBySymbol.at(3), base + 4) << "offset 4 in the section stays 4";
    EXPECT_EQ(layout.offsetBySymbol.at(4), base + 16);
}

TEST(InputSectionUnits, ACodeUnitWithAGapIsRefused) {
    AssembledModule m;
    m.functions.push_back(function(1, 4, InputSectionSlice{3, 0}));
    m.functions.push_back(function(2, 4, InputSectionSlice{3, 8}));   // 4 bytes missing
    DiagnosticReporter rep;
    EXPECT_FALSE(validateInputSectionUnits(m, rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_InputSectionSplit), 1u);
}

TEST(InputSectionUnits, ACodeUnitInterruptedByOtherCodeIsRefused) {
    AssembledModule m;
    m.functions.push_back(function(1, 4, InputSectionSlice{3, 0}));
    m.functions.push_back(function(9, 6, std::nullopt));
    m.functions.push_back(function(2, 4, InputSectionSlice{3, 4}));
    DiagnosticReporter rep;
    EXPECT_FALSE(validateInputSectionUnits(m, rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_InputSectionSplit), 1u);
}

TEST(InputSectionUnits, AContiguousCodeUnitIsAccepted) {
    AssembledModule m;
    m.functions.push_back(function(9, 6, std::nullopt));
    m.functions.push_back(function(1, 4, InputSectionSlice{3, 0}));
    m.functions.push_back(function(2, 12, InputSectionSlice{3, 4}));
    m.functions.push_back(function(5, 8, InputSectionSlice{4, 0}));   // the next object's unit
    DiagnosticReporter rep;
    EXPECT_TRUE(validateInputSectionUnits(m, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(InputSectionUnits, ADataUnitAcrossTwoKindsIsRefused) {
    AssembledModule m;
    m.dataItems.push_back(dataItem(1, DataSectionKind::Data, 4, 4, InputSectionSlice{2, 0}));
    m.dataItems.push_back(dataItem(2, DataSectionKind::Rodata, 4, 4, InputSectionSlice{2, 4}));
    DiagnosticReporter rep;
    EXPECT_FALSE(validateInputSectionUnits(m, rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_InputSectionSplit), 1u);
    // ...and the layout refuses it on its own, for a caller that skipped the check.
    DiagnosticReporter layoutRep;
    EXPECT_FALSE(layOut(m, DataSectionKind::Data, layoutRep).ok);
    EXPECT_EQ(countCode(layoutRep, DiagnosticCode::K_InputSectionSplit), 1u);
}

TEST(InputSectionUnits, ADataUnitWithOverlappingMembersIsRefused) {
    AssembledModule m;
    m.dataItems.push_back(dataItem(1, DataSectionKind::Data, 8, 4, InputSectionSlice{2, 0}));
    m.dataItems.push_back(dataItem(2, DataSectionKind::Data, 4, 4, InputSectionSlice{2, 4}));
    DiagnosticReporter rep;
    EXPECT_FALSE(validateInputSectionUnits(m, rep));
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_InputSectionSplit), 1u);
}

TEST(InputSectionUnits, AUnitSplitByExcludedItemsIsRefused) {
    AssembledModule m;
    m.dataItems.push_back(dataItem(1, DataSectionKind::Data, 4, 4, InputSectionSlice{2, 0}));
    m.dataItems.push_back(dataItem(2, DataSectionKind::Data, 4, 4, InputSectionSlice{2, 4}));
    std::vector<std::size_t> const excluded{1};   // a writer laying item #1 out elsewhere
    DiagnosticReporter rep;
    auto const layout = link::format::buildExecDataSection(
        m.dataItems, DataSectionKind::Data, 1, "input-section-units test", rep,
        /*allowItemRelocations=*/true, excluded);
    EXPECT_FALSE(layout.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_InputSectionSplit), 1u);
}

// ── THE MEASURED SHAPES, through the readers and the writers' layout ─────────

TEST(InputSectionUnits, GccStaticStoresReachTheObjectsTheyName) {
    auto const sh = loadShipped("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = elf::readRelocatableObject(dss::test::gccStaticStoresObject(),
                                              *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value()) << "errors=" << rep.errorCount();

    // Every `.data` atom names ONE unit, at its own section offset.
    std::optional<std::uint32_t> unit;
    for (auto const& d : m->dataItems) {
        if (d.section != DataSectionKind::Data) continue;
        ASSERT_TRUE(d.inputSection.has_value()) << "an ELF data atom placed freely";
        if (!unit.has_value()) unit = d.inputSection->section;
        EXPECT_EQ(d.inputSection->section, *unit);
    }

    auto const layout = layOut(*m, DataSectionKind::Data, rep);
    ASSERT_TRUE(layout.ok) << "errors=" << rep.errorCount();
    auto const* set = functionNamed(*m, "set");
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->relocations.size(), 3u);
    // `movl $imm32, sym(%rip)` x3, in source order: y, x, t[1]. Each carries
    // 4 immediate bytes after its field.
    auto const y = offsetOf(layout, *m, "y");
    auto const x = offsetOf(layout, *m, "x");
    auto const t = offsetOf(layout, *m, "t");
    ASSERT_TRUE(y && x && t);
    EXPECT_EQ(reached(layout, set->relocations[0], 4), *y)
        << "`y = 5` must store into y (it landed in padding and the program "
           "ran to 33 before the section became a unit)";
    EXPECT_EQ(reached(layout, set->relocations[1], 4), *x) << "`x = 7`";
    EXPECT_EQ(reached(layout, set->relocations[2], 4), *t + 4) << "`t[1] = 9`";
}

TEST(InputSectionUnits, AStoreToTheSectionsFirstObjectIsNoLongerRefused) {
    auto const sh = loadShipped("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = elf::readRelocatableObject(dss::test::gccFirstObjectStoreObject(),
                                              *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value())
        << "`y = 5` with y at `.data+0` searches offset -4, before every atom; it "
           "was refused. errors=" << rep.errorCount();
    auto const layout = layOut(*m, DataSectionKind::Data, rep);
    ASSERT_TRUE(layout.ok);
    auto const* set = functionNamed(*m, "set");
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->relocations.size(), 3u);
    auto const y = offsetOf(layout, *m, "y");
    auto const x = offsetOf(layout, *m, "x");
    ASSERT_TRUE(y && x);
    EXPECT_EQ(reached(layout, set->relocations[0], 4), *y);
    EXPECT_EQ(reached(layout, set->relocations[1], 4), *x);
}

TEST(InputSectionUnits, ASymbolPlusOffsetRunsIntoTheNextObject) {
    auto const sh = loadShipped("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = elf::readRelocatableObject(dss::test::gasSymbolPlusOffsetObject(),
                                              *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value()) << "errors=" << rep.errorCount();
    auto const layout = layOut(*m, DataSectionKind::Data, rep);
    ASSERT_TRUE(layout.ok);
    auto const* get = functionNamed(*m, "get");
    ASSERT_NE(get, nullptr);
    ASSERT_EQ(get->relocations.size(), 2u);
    auto const a = offsetOf(layout, *m, "a");
    auto const b = offsetOf(layout, *m, "b");
    ASSERT_TRUE(a && b);
    // `movl a+8(%rip)` binds to `a` BY NAME, with 8 past its 8 bytes -- which
    // is `b` only while b keeps its place. It read padding and the program ran
    // to 2 before.
    EXPECT_EQ(reached(layout, get->relocations[0], 0), *b);
    EXPECT_EQ(reached(layout, get->relocations[1], 0), *a + 4);
}

TEST(InputSectionUnits, ObjectsInOneNamedSectionStayAnArrayBetweenItsLabels) {
    auto const sh = loadShipped("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = elf::readRelocatableObject(dss::test::gasNamedSectionArrayObject(),
                                              *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value())
        << "`tbl_end` marks the section's END, where no atom lies; it was "
           "refused. errors=" << rep.errorCount();
    auto const layout = layOut(*m, DataSectionKind::Rodata, rep);
    ASSERT_TRUE(layout.ok);
    auto const e1 = offsetOf(layout, *m, "e1");
    auto const e2 = offsetOf(layout, *m, "e2");
    auto const e3 = offsetOf(layout, *m, "e3");
    ASSERT_TRUE(e1 && e2 && e3);
    EXPECT_EQ(*e2, *e1 + 4) << "no padding between the array's members";
    EXPECT_EQ(*e3, *e1 + 8);
    auto const* sum = functionNamed(*m, "sum");
    ASSERT_NE(sum, nullptr);
    ASSERT_EQ(sum->relocations.size(), 2u);
    EXPECT_EQ(reached(layout, sum->relocations[0], 0), *e1) << "tbl_begin";
    EXPECT_EQ(reached(layout, sum->relocations[1], 0), *e3 + 4) << "tbl_end: one past e3";
}

// ── PE/COFF: the section symbol, the in-place addend, the long section name ──

TEST(InputSectionUnits, MingwSectionSymbolReferencesReachTheirObjects) {
    auto const sh = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = pe::readRelocatableObject(dss::test::mingwGccSectionSymbolObject(),
                                             *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value()) << "errors=" << rep.errorCount();
    auto const layout = layOut(*m, DataSectionKind::Data, rep);
    ASSERT_TRUE(layout.ok);
    auto const* setCounter = functionNamed(*m, "set_counter");
    ASSERT_NE(setCounter, nullptr);
    ASSERT_EQ(setCounter->relocations.size(), 2u);
    auto const counter = offsetOf(layout, *m, "counter");
    auto const flags   = offsetOf(layout, *m, "flags");
    ASSERT_TRUE(counter && flags);
    for (auto const& rel : setCounter->relocations) {
        EXPECT_TRUE(layout.offsetBySymbol.contains(rel.target.v))
            << "a section-symbol reference must be rebound to an atom -- the "
               "section symbol itself owns no body (K_SymbolUndefined before)";
    }
    EXPECT_EQ(reached(layout, setCounter->relocations[0], 4), *counter);
    EXPECT_EQ(reached(layout, setCounter->relocations[1], 4), *flags + 8);
}

TEST(InputSectionUnits, ClangCoffNegativeInPlaceAddendIsRead) {
    auto const sh = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = pe::readRelocatableObject(dss::test::clangCoffInPlaceAddendObject(),
                                             *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value()) << "errors=" << rep.errorCount();
    auto const* setCounter = functionNamed(*m, "set_counter");
    ASSERT_NE(setCounter, nullptr);
    ASSERT_EQ(setCounter->relocations.size(), 2u);
    auto const counterId = symbolNamed(*m, "counter");
    ASSERT_TRUE(counterId.has_value());
    EXPECT_EQ(setCounter->relocations[0].target, *counterId);
    EXPECT_EQ(setCounter->relocations[0].addend, -4)
        << "`fc ff ff ff` in the field IS the addend (it was read as 0, and the "
           "program ran to 32 instead of 42)";
    EXPECT_EQ(setCounter->relocations[1].addend, 4) << "flags+8, less 4";
}

TEST(InputSectionUnits, CoffLongSectionNameIsReadFromTheStringTable) {
    auto const sh = loadShipped("x86_64", "pe64-x86_64-windows");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = pe::readRelocatableObject(dss::test::mingwGasLongSectionNameObject(),
                                             *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value())
        << "`.rdata$tbl` is spelled \"/4\" in the section header; read as the "
           "NAME it resolved to no kind and every symbol in it was refused. errors="
        << rep.errorCount();
    auto const layout = layOut(*m, DataSectionKind::Rodata, rep);
    ASSERT_TRUE(layout.ok);
    auto const e1 = offsetOf(layout, *m, "e1");
    auto const e3 = offsetOf(layout, *m, "e3");
    ASSERT_TRUE(e1 && e3);
    EXPECT_EQ(*e3, *e1 + 8);
}

// ── Mach-O: divisible only when the object says so ──────────────────────────

TEST(InputSectionUnits, MachOAtomsAreFreeOnlyWhenTheObjectDeclaresSubsections) {
    auto const sh = loadShipped("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(sh.target && sh.format);
    std::vector<std::uint8_t> declared = dss::test::clangMachOSignedFamilyObject();
    constexpr std::size_t kFlagsOffset = 24;                // mach_header_64.flags
    constexpr std::uint32_t kSubsectionsViaSymbols = 0x2000u;
    ASSERT_NE(declared[kFlagsOffset + 1] & (kSubsectionsViaSymbols >> 8), 0u)
        << "the premise: clang sets MH_SUBSECTIONS_VIA_SYMBOLS";

    DiagnosticReporter rep;
    auto const free = macho::readRelocatableObject(declared, *sh.target, *sh.format, rep);
    ASSERT_TRUE(free.has_value()) << describe(rep);
    ASSERT_GE(free->dataItems.size(), 2u);
    for (auto const& d : free->dataItems) {
        EXPECT_FALSE(d.inputSection.has_value())
            << "a subsections-declaring object's atoms are placed freely (ld64's rule)";
    }

    std::vector<std::uint8_t> undeclared = declared;
    undeclared[kFlagsOffset + 1] = static_cast<std::uint8_t>(
        undeclared[kFlagsOffset + 1] & ~(kSubsectionsViaSymbols >> 8));
    DiagnosticReporter rep2;
    auto const units = macho::readRelocatableObject(undeclared, *sh.target, *sh.format, rep2);
    ASSERT_TRUE(units.has_value()) << describe(rep2);
    ASSERT_EQ(units->dataItems.size(), free->dataItems.size());
    for (auto const& d : units->dataItems) {
        EXPECT_TRUE(d.inputSection.has_value())
            << "without the declaration, every section is one unit";
    }
    for (auto const& f : units->functions) {
        EXPECT_TRUE(f.inputSection.has_value());
    }
}

TEST(InputSectionUnits, ClangMachOSignedFamilyReadsWithEachAddend) {
    auto const sh = loadShipped("x86_64", "macho64-x86_64-darwin");
    ASSERT_TRUE(sh.target && sh.format);
    DiagnosticReporter rep;
    auto const m = macho::readRelocatableObject(dss::test::clangMachOSignedFamilyObject(),
                                                *sh.target, *sh.format, rep);
    ASSERT_TRUE(m.has_value())
        << "clang's SIGNED_1/_2/_4 were unknown wire types; " << describe(rep);
    auto const* probe = functionNamed(*m, "_probe");
    ASSERT_NE(probe, nullptr);
    auto const* riprel = sh.target->relocationByName("riprel32");
    ASSERT_NE(riprel, nullptr);
    // In field order: SIGNED_4 (movl $5), SIGNED_2 (movw $imm16), SIGNED_1
    // (cmpb $0), SIGNED (movl). The addend in each field is the one the bytes
    // after it make: -4, -2, -1, 0.
    std::map<std::uint32_t, std::int64_t> addendByOffset;
    for (auto const& r : probe->relocations) {
        EXPECT_EQ(r.kind.v, riprel->kind.v) << "every SIGNED form is a memory displacement";
        addendByOffset[r.offset] = r.addend;
    }
    ASSERT_EQ(addendByOffset.size(), 4u);
    std::vector<std::int64_t> inOrder;
    for (auto const& [offset, addend] : addendByOffset) inOrder.push_back(addend);
    EXPECT_EQ(inOrder, (std::vector<std::int64_t>{-4, -2, -1, 0}));
}

// ── THE TWO FORMAT FACTS ARE REQUIRED, by name ─────────────────────────────
//
// Both answers to "where does the addend live" and "may an input section be
// split" produce a well-formed image, and only one of each is the format. A
// document that declares relocations and states neither is refused at load,
// naming the missing key. The fixture is the SHIPPED document minus exactly
// that key, so it cannot drift from the real file.

namespace {

[[nodiscard]] std::vector<ConfigDiagnostic> loadShippedWithout(std::string const& format,
                                                               std::string const& key) {
    auto const path = dss::test::configRoot() / "object-formats" / (format + ".format.json");
    std::ifstream in{path};
    auto doc = nlohmann::json::parse(in);
    EXPECT_TRUE(doc.contains(key)) << "the premise: " << format << " declares " << key;
    EXPECT_FALSE(doc.at("relocations").empty()) << "the premise: it declares relocations";
    doc.erase(key);
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(), format + " minus " + key);
    if (r.has_value()) return {};
    return r.error();
}

[[nodiscard]] bool namesPath(std::vector<ConfigDiagnostic> const& diags, std::string const& path) {
    for (auto const& d : diags) {
        if (d.path == path) return true;
    }
    return false;
}

}  // namespace

TEST(InputSectionUnits, AFormatWithRelocationsMustSayWhereTheAddendLives) {
    for (std::string const format : {"elf64-x86_64-linux", "pe64-x86_64-windows",
                                     "macho64-arm64-darwin"}) {
        auto const diags = loadShippedWithout(format, "relocationAddends");
        EXPECT_FALSE(diags.empty()) << format << " loaded without `relocationAddends`";
        EXPECT_TRUE(namesPath(diags, "/relocationAddends")) << format;
    }
}

TEST(InputSectionUnits, AFormatWithRelocationsMustSayWhetherASectionMaySplit) {
    for (std::string const format : {"elf64-x86_64-linux", "pe64-x86_64-windows",
                                     "macho64-x86_64-darwin"}) {
        auto const diags = loadShippedWithout(format, "inputSectionPlacement");
        EXPECT_FALSE(diags.empty()) << format << " loaded without `inputSectionPlacement`";
        EXPECT_TRUE(namesPath(diags, "/inputSectionPlacement")) << format;
    }
}

// ── THE WITNESSES: reference-built objects, linked by DSS, RUN ──────────────
//
// Windows only, and only where a mingw-w64 gcc is on PATH: the objects are
// built at test time by the reference toolchain, so what DSS reads is what
// that toolchain wrote today. A toolchain that is absent is a skip. One that
// is present and fails is a red, the discipline `test_coff_object_reader.cpp`
// states for its own mingw witnesses.

#if defined(_WIN32)
namespace {

struct MingwWitness {
    std::filesystem::path dir;
    bool                  usable = false;

    [[nodiscard]] bool build(std::string const& file, std::string const& source,
                             std::string const& flags, std::filesystem::path& obj) const {
        auto const src = dir / file;
        { std::ofstream s{src}; s << source; }
        obj = dir / (src.stem().string() + ".o");
        std::error_code ec;
        std::filesystem::remove(obj, ec);   // a stale object must not vouch
        std::string const cmd = "gcc " + flags + " -c -o \"" + obj.string() + "\" \""
                              + src.string() + "\" >nul 2>&1";
        return std::system(cmd.c_str()) == 0 && std::filesystem::exists(obj);
    }

    // Link `mainSource` (compiled by DSS) against `obj` and run it.
    [[nodiscard]] std::optional<std::uint32_t> linkAndRun(std::string const& mainSource,
                                                          std::filesystem::path const& obj,
                                                          std::string& why) const {
        auto const mainPath = dir / "main.c";
        { std::ofstream s{mainPath}; s << mainSource; }
        auto const out = dir / "out";
        std::filesystem::create_directories(out);
        Program p;
        p.setOutputDir(out);
        p.setResolveLibraries(std::vector<std::filesystem::path>{obj});
        DiagnosticReporter rep;
        int const rc = p.compileFiles(std::vector<std::string>{mainPath.string()}, "c",
                                      std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"},
                                      rep);
        if (rc != 0) {
            why = "the link failed; errors=" + std::to_string(rep.errorCount());
            for (auto const& d : rep.all()) why += "\n  " + d.actual;
            return std::nullopt;
        }
        auto const r = test_support::runBinary(out / "main.exe");
        if (!r.spawned || r.timedOut) { why = r.diagnostic; return std::nullopt; }
        return r.exitCode;
    }
};

[[nodiscard]] MingwWitness mingwWitness(std::filesystem::path const& dir) {
    MingwWitness w;
    w.dir = dir;
    if (std::system("where gcc >nul 2>&1") != 0) return w;
    std::filesystem::path probe;
    w.usable = w.build("toolchain_check.c", "int p(void){return 0;}\n", "-O0", probe);
    return w;
}

constexpr char const* kStaticStoresSource =
    "static int t[4] = {1, 2, 3, 4};\n"
    "static int x = 1;\n"
    "static int y = 2;\n"
    "static volatile int w = 11;\n"
    "void set(void) { y = 5; x = 7; t[1] = 9; }\n"
    "int get(void) { return x + y + t[1] + w + 10; }\n";
constexpr char const* kFirstObjectStoreSource =
    "static int t[4] = {1, 2, 3, 4};\n"
    "static int x = 1;\n"
    "static int y = 2;\n"
    "void set(void) { y = 5; x = 7; t[1] = 9; }\n"
    "int get(void) { return x + y + t[1] + 21; }\n";
constexpr char const* kSetGetMain =
    "extern void set(void);\nextern int get(void);\n"
    "int main(void) { set(); return get(); }\n";

}  // namespace
#endif  // _WIN32

TEST(InputSectionUnitsNative, MingwGccStaticStoresRunToFortyTwo) {
#if !defined(_WIN32)
    GTEST_SKIP() << "builds its object with mingw-w64 gcc; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "input-section-units"};
    auto const gcc = mingwWitness(scratch.path());
    if (!gcc.usable) GTEST_SKIP() << "no usable gcc on PATH -- the mingw witness is inert here";
    std::filesystem::path obj;
    ASSERT_TRUE(gcc.build("stores.c", kStaticStoresSource, "-O2", obj));
    std::string why;
    auto const exit = gcc.linkAndRun(kSetGetMain, obj, why);
    ASSERT_TRUE(exit.has_value()) << why;
    EXPECT_EQ(*exit, 42u);
#endif
}

TEST(InputSectionUnitsNative, MingwGccFirstObjectStoreRunsToFortyTwo) {
#if !defined(_WIN32)
    GTEST_SKIP() << "builds its object with mingw-w64 gcc; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "input-section-units"};
    auto const gcc = mingwWitness(scratch.path());
    if (!gcc.usable) GTEST_SKIP() << "no usable gcc on PATH -- the mingw witness is inert here";
    std::filesystem::path obj;
    ASSERT_TRUE(gcc.build("first.c", kFirstObjectStoreSource, "-O2", obj));
    std::string why;
    auto const exit = gcc.linkAndRun(kSetGetMain, obj, why);
    ASSERT_TRUE(exit.has_value()) << why;
    EXPECT_EQ(*exit, 42u);
#endif
}

TEST(InputSectionUnitsNative, MingwSymbolPlusOffsetIntoTheNextObjectRunsToFortyTwo) {
#if !defined(_WIN32)
    GTEST_SKIP() << "assembles its object with mingw-w64 gas; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "input-section-units"};
    auto const gcc = mingwWitness(scratch.path());
    if (!gcc.usable) GTEST_SKIP() << "no usable gcc on PATH -- the mingw witness is inert here";
    std::filesystem::path obj;
    ASSERT_TRUE(gcc.build("symoff.s",
                          "\t.data\n\t.p2align 4\n\t.globl\ta\na:\t.long\t1\n\t.long\t2\n"
                          "\t.globl\tb\nb:\t.long\t40\n"
                          "\t.text\n\t.globl\tget\nget:\n"
                          "\tmovl\ta+8(%rip), %eax\n\taddl\ta+4(%rip), %eax\n\tret\n",
                          "", obj));
    std::string why;
    auto const exit = gcc.linkAndRun("extern int get(void);\nint main(void) { return get(); }\n",
                                     obj, why);
    ASSERT_TRUE(exit.has_value()) << why;
    EXPECT_EQ(*exit, 42u) << "b (40) + a[1] (2)";
#endif
}

TEST(InputSectionUnitsNative, MingwObjectsInOneNamedSectionIterateToFortyTwo) {
#if !defined(_WIN32)
    GTEST_SKIP() << "assembles its object with mingw-w64 gas; Windows only";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "input-section-units"};
    auto const gcc = mingwWitness(scratch.path());
    if (!gcc.usable) GTEST_SKIP() << "no usable gcc on PATH -- the mingw witness is inert here";
    std::filesystem::path obj;
    ASSERT_TRUE(gcc.build("tbl.s",
                          "\t.section\t.rdata$tbl,\"dr\"\n\t.p2align 4\n"
                          "\t.globl\ttbl_begin\ntbl_begin:\n"
                          "\t.globl\te1\ne1:\t.long\t10\n\t.globl\te2\ne2:\t.long\t12\n"
                          "\t.globl\te3\ne3:\t.long\t20\n\t.globl\ttbl_end\ntbl_end:\n"
                          "\t.text\n\t.globl\tsum\nsum:\n"
                          "\tleaq\ttbl_begin(%rip), %rcx\n\tleaq\ttbl_end(%rip), %rdx\n"
                          "\txorl\t%eax, %eax\n1:\tcmpq\t%rdx, %rcx\n\tje\t2f\n"
                          "\taddl\t(%rcx), %eax\n\taddq\t$4, %rcx\n\tjmp\t1b\n2:\tret\n",
                          "", obj));
    std::string why;
    auto const exit = gcc.linkAndRun("extern int sum(void);\nint main(void) { return sum(); }\n",
                                     obj, why);
    ASSERT_TRUE(exit.has_value()) << why;
    EXPECT_EQ(*exit, 42u) << "10 + 12 + 20, walked between the two labels";
#endif
}

TEST(InputSectionUnitsNative, ClangCoffObjectRunsToFortyTwo) {
#if !defined(_WIN32)
    GTEST_SKIP() << "runs a pe64 image; Windows only";
#else
    // The golden clang object needs no toolchain at test time.
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "input-section-units"};
    MingwWitness w;
    w.dir = scratch.path();
    auto const obj = w.dir / "lib2_clang.o";
    {
        auto const bytes = dss::test::clangCoffInPlaceAddendObject();
        std::ofstream o{obj, std::ios::binary};
        o.write(reinterpret_cast<char const*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    std::string why;
    auto const exit = w.linkAndRun(
        "extern void set_counter(void);\nextern int get_sum(void);\n"
        "int main(void) { set_counter(); return get_sum() + 30; }\n",
        obj, why);
    ASSERT_TRUE(exit.has_value()) << why;
    EXPECT_EQ(*exit, 42u) << "5 + 7 + 30; the in-place -4 read as 0 ran it to 32";
#endif
}
