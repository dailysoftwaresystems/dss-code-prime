// P69 fold 3c, lane p69/xa — the values a narrow Linear field holds, and the relocation types the references write for
// ordinary C that DSS's documents did not declare.
//
//   D-LK-LINEAR-RELOCATION-FIELD-RANGE-CHECKED-AS-SIGNED-FOR-EVERY-KIND — every Linear field narrower than 8 bytes
//     was checked SIGNED, whatever its wire type: R_X86_64_32 (read back zero-extended) refused [2^31, 2^32), which GNU
//     ld and ld.lld link, and LINKED [-2^31, 0), which both refuse — -1 written 0xffffffff, read back by `movl` as
//     2^32 - 1. Each row now states its `fieldRange` and `linearFieldHolds` is the one check.
//   D-ASM-DATA-WORD-RELOCATION-KIND-PICKED-BY-DECLARATION-ORDER — the assembler's 4-byte data-slot scan read no formula, so
//     an arm64 `.long main` took `call26` (the first 4-byte non-pc-relative row) and was written R_AARCH64_CALL26 in
//     `.rela.data`: GNU ld and ld.lld linked DSS's object with the word 0x03ffbffe where &main was 0x4000f0.
//   D-LK-RELOCATION-TYPES-REFERENCE-COMPILERS-EMIT-FOR-C-UNDECLARED — R_AARCH64_JUMP26 (gcc -O2's tail call) and
//     R_X86_64_32S (every -fno-pic indexed access to a global) were declared by no document, so every object carrying
//     one was refused at READ as corrupted.
//
// The fixtures (`tests/link/data/`, each built twice byte-identical; their sources state how):
//   reference_relocation_types_aarch64_elf_gcc_O2.o        md5 9cfe4e7ce4305bca6060c9c1a98b89ff  JUMP26 `callee`
//   reference_relocation_types_x86_64_elf_gcc_O2_nopic.o   md5 c3de5dd33b2ab400be746ea657c35fd8  32S `table`
//   linear_field_edge_32_hi_x86_64_elf_gas.o               md5 b6eba85af885f284ac66a2cb61f6bb09  32  ext + 2^31
//   linear_field_edge_32_lo_x86_64_elf_gas.o               md5 eb867940c20228fe33966e5be4826399  32  ext - 2^31
//   linear_field_edge_32s_hi_x86_64_elf_gas.o              md5 f66bc925c9f8531e111860f51f6cd790  32S ext + 2^31
//   linear_field_edge_32s_lo_x86_64_elf_gas.o              md5 5e4b7c63ac1416423189a3563d921639  32S ext - 2^31
// No guard enforces an md5: it records WHICH BYTES were measured.
//
// Every TEST but the `…Native` ones runs in-process on every leg; those run the image on its own host.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/format/exec_reloc_apply.hpp"
#include "link/object_format_schema.hpp"
#include "link/pointer_reloc.hpp"
#include "program/program.hpp"

#include "arm_verdict_ledger.hpp"
#include "host_native_target.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

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

[[nodiscard]] bool said(DiagnosticReporter const& rep, std::string_view text) {
    return std::any_of(rep.all().begin(), rep.all().end(),
                       [&](auto const& d) { return d.actual.find(text) != std::string::npos; });
}

[[nodiscard]] std::shared_ptr<TargetSchema const> shippedTarget(char const* name) {
    auto t = TargetSchema::loadShipped(name);
    EXPECT_TRUE(t.has_value()) << "the shipped " << name << " target";
    return t.has_value() ? *t : nullptr;
}

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const> shippedFormat(char const* name) {
    auto f = ObjectFormatSchema::loadShipped(name);
    EXPECT_TRUE(f.has_value()) << "the shipped " << name << " document";
    return f.has_value() ? *f : nullptr;
}

[[nodiscard]] TargetRelocationInfo const& row(TargetSchema const& t, std::string_view name) {
    auto const* r = t.relocationByName(name);
    if (r == nullptr) {
        ADD_FAILURE() << "the target declares no '" << name << "' row";
        static TargetRelocationInfo const none{};
        return none;
    }
    return *r;
}

// A synthetic target with one relocation row (`rowJson`), loaded through the shipped loader.
[[nodiscard]] std::string loadErrorsOf(std::string const& rowJson) {
    std::string const json = R"({ "dssTargetVersion": 1, "target": {"name":"field_range_test"},
        "opcodes": [ {"mnemonic":"invalid","result":"none"} ], "relocations": [ )" + rowJson + " ] }";
    auto const t = TargetSchema::loadFromText(json, "field_range_test.json");
    if (t.has_value()) return {};
    std::string s;
    for (auto const& d : t.error()) s += d.message + "\n";
    return s.empty() ? std::string{"<refused, no message>"} : s;
}

constexpr std::int64_t kTwo31 = std::int64_t{1} << 31;
constexpr std::int64_t kTwo32 = std::int64_t{1} << 32;

}  // namespace

// ══ THE KEY: required on a narrow Linear row, refused anywhere else ════════════════════════════════════════════════

TEST(LinearFieldRange, ANarrowLinearRowWithNoRangeIsRefusedByName) {
    auto const errors =
        loadErrorsOf(R"({ "name": "w4", "kind": 1, "formula": "linear", "pcRelative": false, "addendBias": 0,
                         "widthBytes": 4 })");
    ASSERT_FALSE(errors.empty()) << "a 4-byte Linear row must state the values its field holds";
    EXPECT_NE(errors.find("must state 'fieldRange'"), std::string::npos) << errors;
    EXPECT_NE(errors.find("w4"), std::string::npos) << "the refusal names the row: " << errors;
}

TEST(LinearFieldRange, EachStatedRangeLoads) {
    for (char const* range : {"signed", "unsigned", "either"}) {
        SCOPED_TRACE(range);
        EXPECT_EQ(loadErrorsOf(std::string{R"({ "name": "w4", "kind": 1, "formula": "linear", "pcRelative": false,
                                               "addendBias": 0, "widthBytes": 4, "fieldRange": ")"} + range + "\" }"),
                  "");
    }
}

TEST(LinearFieldRange, ARangeNothingReadsIsRefused) {
    auto const wide = loadErrorsOf(R"({ "name": "w8", "kind": 1, "formula": "linear", "pcRelative": false,
                                        "addendBias": 0, "widthBytes": 8, "fieldRange": "unsigned" })");
    EXPECT_NE(wide.find("a range nothing reads"), std::string::npos) << "an 8-byte field holds every value: " << wide;
    auto const word = loadErrorsOf(R"({ "name": "b", "kind": 1, "formula": "aarch64_call26",
                                        "fieldRange": "signed" })");
    EXPECT_NE(word.find("a range nothing reads"), std::string::npos)
        << "an instruction formula's field is the formula's own: " << word;
    auto const spelled = loadErrorsOf(R"({ "name": "w4", "kind": 1, "formula": "linear", "pcRelative": false,
                                           "addendBias": 0, "widthBytes": 4, "fieldRange": "zero-extended" })");
    EXPECT_NE(spelled.find("is none of 'signed', 'unsigned', 'either'"), std::string::npos) << spelled;
}

// The width-less Linear row (D-LK6-1: loaded, refused at apply) has no field, so it states no range.
TEST(LinearFieldRange, AWidthlessLinearRowStatesNoRange) {
    EXPECT_EQ(loadErrorsOf(R"({ "name": "weird", "kind": 1, "formula": "linear" })"), "");
}

// ══ THE CHECK, PER KIND AND EDGE — the instruction fixups (`applyExecRelocations`) ═════════════════════════════════

namespace {

struct Edge {
    std::int64_t value;
    bool         holds;
};

// Applies one relocation of `kind` whose S + A is `value` (S = 0) into a 4-byte field; the field, or nullopt.
[[nodiscard]] std::optional<std::uint32_t> applyOne(TargetSchema const& t, TargetRelocationInfo const& r,
                                                    std::int64_t value, std::string& messages) {
    AssembledModule   mod;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes.assign(4, 0);
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{2};
    rel.kind   = r.kind;
    rel.addend = value;
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    std::vector<std::uint8_t>                   text(4, 0);
    std::vector<std::uint64_t>                  starts{0};
    std::unordered_map<SymbolId, std::uint64_t> va{{SymbolId{2}, 0}};
    DiagnosticReporter                          rep;
    bool const ok = link::format::applyExecRelocations(text, mod, starts, va, t, 0x400000, "test", rep);
    messages      = diagnosticsOf(rep);
    if (!ok) return std::nullopt;
    return static_cast<std::uint32_t>(text[0]) | (static_cast<std::uint32_t>(text[1]) << 8)
         | (static_cast<std::uint32_t>(text[2]) << 16) | (static_cast<std::uint32_t>(text[3]) << 24);
}

void expectEdges(TargetSchema const& t, std::string_view kind, std::initializer_list<Edge> edges,
                 std::string_view rangeWord) {
    auto const& r = row(t, kind);
    for (Edge const e : edges) {
        SCOPED_TRACE(std::string{kind} + " V=" + std::to_string(e.value));
        std::string msgs;
        auto const  field = applyOne(t, r, e.value, msgs);
        EXPECT_EQ(field.has_value(), e.holds) << msgs;
        if (field.has_value()) {
            EXPECT_EQ(*field, static_cast<std::uint32_t>(e.value)) << "the low 32 bits of V";
        } else {
            EXPECT_NE(msgs.find(std::string{"the "} + std::string{rangeWord} + " 32-bit range"), std::string::npos)
                << "the refusal names the field's range:" << msgs;
        }
    }
}

}  // namespace

// ✔MEASURED 2026-10-20 (GNU ld 2.42 and ld.lld 18.1.3 alike): R_X86_64_32 links exactly [0, 2^32), R_X86_64_32S
// exactly [-2^31, 2^31). COFF's ADDR32 is GNU ld's [-2^31, 2^32) (mingw ld 2.41.90, its in-place addend read signed);
// R_AARCH64_ABS32 is ld.lld's [-2^31, 2^32) (a program reads the -1 back; GNU ld refuses it).
TEST(LinearFieldRange, EachKindHoldsItsOwnRangeAndNothingPastIt) {
    auto const x = shippedTarget("x86_64");
    auto const a = shippedTarget("arm64");
    ASSERT_TRUE(x && a);
    expectEdges(*x, "abs32",
                {{kTwo31 - 1, true}, {kTwo31, true}, {kTwo32 - 1, true}, {kTwo32, false}, {-1, false},
                 {-kTwo31, false}},
                "unsigned");
    expectEdges(*x, "abs32s",
                {{kTwo31 - 1, true}, {kTwo31, false}, {kTwo32 - 1, false}, {-1, true}, {-kTwo31, true},
                 {-kTwo31 - 1, false}},
                "signed");
    expectEdges(*x, "addr32",
                {{kTwo31, true}, {kTwo32 - 1, true}, {kTwo32, false}, {-1, true}, {-kTwo31, true},
                 {-kTwo31 - 1, false}},
                "either");
    expectEdges(*a, "abs32",
                {{kTwo31, true}, {kTwo32 - 1, true}, {kTwo32, false}, {-1, true}, {-kTwo31, true},
                 {-kTwo31 - 1, false}},
                "either");
}

// ══ THE CHECK on the data fixups (`applyDataItemRelocations`, through the ELF image writer) ═══════════════════════

namespace {

// An image whose `slot` (a 4-byte datum) holds `ext + A` through `kind`; `ext` sits below 2^31 in an
// elf64-x86_64-linux-exec image, so A = +2^31 puts V in [2^31, 2^32) and A = -2^31 in [-2^31, 0).
[[nodiscard]] bool linksDataSlot(TargetSchema const& t, ObjectFormatSchema const& f, std::string_view kind,
                                 std::int64_t addend, std::string& messages) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction entry;
    entry.symbol = SymbolId{1};
    entry.bytes  = {0xC3};
    mod.functions.push_back(std::move(entry));
    mod.imageEntryOverride = 0u;   // the entry the pipeline's trampoline would name; this image is never run
    AssembledData ext;
    ext.symbol  = SymbolId{2};
    ext.section = DataSectionKind::Data;
    ext.bytes.assign(8, 0);
    mod.dataItems.push_back(std::move(ext));
    AssembledData slot;
    slot.symbol  = SymbolId{3};
    slot.section = DataSectionKind::Data;
    slot.bytes.assign(4, 0);
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{2};
    rel.kind   = row(t, kind).kind;
    rel.addend = addend;
    slot.relocations.push_back(rel);
    mod.dataItems.push_back(std::move(slot));
    DiagnosticReporter rep;
    auto const         bytes = elf::encode(mod, t, f, rep);
    messages                 = diagnosticsOf(rep);
    return !bytes.empty();
}

}  // namespace

TEST(LinearFieldRange, ADataSlotHoldsItsKindsRangeToo) {
    auto const x = shippedTarget("x86_64");
    auto const f = shippedFormat("elf64-x86_64-linux-exec");
    ASSERT_TRUE(x && f);
    std::string m;
    EXPECT_TRUE(linksDataSlot(*x, *f, "abs32", kTwo31, m)) << "R_X86_64_32 holds [2^31, 2^32):" << m;
    EXPECT_FALSE(linksDataSlot(*x, *f, "abs32", -kTwo31, m)) << "R_X86_64_32 holds no negative value";
    EXPECT_NE(m.find("the unsigned 32-bit range"), std::string::npos) << m;
    EXPECT_TRUE(linksDataSlot(*x, *f, "abs32s", -kTwo31, m)) << "R_X86_64_32S holds [-2^31, 0):" << m;
    EXPECT_FALSE(linksDataSlot(*x, *f, "abs32s", kTwo31, m)) << "R_X86_64_32S holds nothing at or past 2^31";
    EXPECT_NE(m.find("the signed 32-bit range"), std::string::npos) << m;
}

// THE PE ADDR32 OWNER. A COFF IMAGE_REL_AMD64_ADDR32 read from an object is the row whose field holds BOTH halves —
// `addr32`, [-2^31, 2^32), GNU ld's range (✔MEASURED 2026-10-20, mingw GNU ld 2.41.90: 0x80000000 and 0xffffffff link,
// and 0 + (-1) writes 0xffffffff; link.exe 14.44 and lld-link check nothing against an absolute symbol) — on every PE
// document, the object's, the archive member's, the image's and the library's; and DSS's own 4-byte absolute word
// (`abs32`) is still WRITTEN as ADDR32, through the emit-only alias. Read as the signed `abs32s`, an object's `.long sym`
// would refuse the upper half of the addresses GNU ld links; read as the unsigned `abs32`, its in-place -1 would.
TEST(LinearFieldRange, ACoffAddr32IsReadAsTheRowThatHoldsBothHalves) {
    auto const x = shippedTarget("x86_64");
    ASSERT_TRUE(x);
    auto const& addr32 = row(*x, "addr32");
    auto const& abs32  = row(*x, "abs32");
    ASSERT_TRUE(addr32.fieldRange.has_value() && *addr32.fieldRange == RelocFieldRange::Either)
        << "addr32 holds both halves";
    constexpr std::uint32_t kImageRelAmd64Addr32 = 2;
    std::array<std::uint8_t, 4> const site{0xff, 0xff, 0xff, 0xff};
    for (char const* stem : {"pe64-x86_64-windows", "pe64-x86_64-windows-staticlib", "pe64-x86_64-windows-exec",
                             "pe64-x86_64-windows-dll"}) {
        SCOPED_TRACE(stem);
        auto const f = shippedFormat(stem);
        ASSERT_TRUE(f);
        auto const table = f->relocationDecodeTable();
        ASSERT_TRUE(table.has_value()) << table.error();
        auto const read = table->decode(kImageRelAmd64Addr32, site);
        ASSERT_TRUE(read.has_value()) << "ADDR32 is declared";
        EXPECT_EQ(*read, addr32.kind) << "ADDR32 is read as `addr32`, the row that holds both halves";
        auto const* written = f->relocationByKind(abs32.kind);
        ASSERT_NE(written, nullptr) << "DSS's `abs32` has a PE spelling";
        EXPECT_EQ(written->nativeId, kImageRelAmd64Addr32) << "`abs32` is written as ADDR32";
        EXPECT_TRUE(written->emitOnly) << "through the emit-only alias, which no reader decodes";
        auto const* owner = f->relocationByKind(addr32.kind);
        ASSERT_NE(owner, nullptr);
        EXPECT_EQ(owner->nativeId, kImageRelAmd64Addr32);
        EXPECT_FALSE(owner->emitOnly) << "`addr32` owns the wire type";
    }
}

// ══ THE SLOT'S ROW, BY RANGE — `addressSlotRelocationKind` ═══════════════════════════════════════════════════════════

TEST(LinearFieldRange, AnAddressSlotTakesTheRowWhoseRangeIsTheAddresses) {
    auto const x = shippedTarget("x86_64");
    auto const a = shippedTarget("arm64");
    ASSERT_TRUE(x && a);
    // x86_64 declares three 4-byte absolute rows; the slot is the zero-extending one, whatever the list's order.
    EXPECT_EQ(linker::absolutePointerRelocKind(*x, 4), row(*x, "abs32").kind);
    EXPECT_EQ(linker::absolutePointerRelocKind(*x, 8), row(*x, "abs64").kind);
    // arm64 declares one, `either` (R_AARCH64_ABS32) — never `call26`, the first 4-byte non-pc-relative row.
    EXPECT_EQ(linker::absolutePointerRelocKind(*a, 4), row(*a, "abs32").kind);
    // A signed row is never a slot: a target whose only 4-byte absolute row is signed has no 4-byte slot (a slot of
    // that width is then refused where one is needed, by name — the callers' own pins).
    auto const signedOnly = TargetSchema::loadFromText(R"({ "dssTargetVersion": 1, "target": {"name":"t"},
        "opcodes": [ {"mnemonic":"invalid","result":"none"} ], "relocations": [
        { "name": "s", "kind": 1, "formula": "linear", "pcRelative": false, "addendBias": 0, "widthBytes": 4, "fieldRange": "signed" } ] })",
                                                       "t.json");
    ASSERT_TRUE(signedOnly.has_value());
    EXPECT_FALSE(linker::absolutePointerRelocKind(**signedOnly, 4).has_value());
}

namespace {

[[nodiscard]] TargetRelocationInfo linearRow(char const* name, std::uint32_t kind, std::uint8_t width,
                                             std::optional<RelocFieldRange> range) {
    TargetRelocationInfo r;
    r.name        = name;
    r.kind        = RelocationKind{kind};
    r.formulaKind = RelocFormulaKind::Linear;
    r.widthBytes  = width;
    r.fieldRange  = range;
    return r;
}

}  // namespace

// The rule itself, on rows no loader saw: an `unsigned` row is the slot when there is one, else an `either` one; a
// signed, pc-relative, thread-local or image-relative row never is; two rows the rule picks equally answer nothing.
TEST(AddressSlot, TheRuleOnRowsNoLoaderSaw) {
    using R = RelocFieldRange;
    auto const pick = [](std::vector<TargetRelocationInfo> const& rows, std::uint8_t width) {
        return addressSlotRelocationKind(rows, width);
    };
    auto pc = linearRow("pc", 1, 4, R::Signed);
    pc.pcRelative = true;
    auto tls = linearRow("tls", 2, 4, R::Unsigned);
    tls.tls = true;
    auto rva = linearRow("rva", 3, 4, R::Unsigned);
    rva.imageRelative = true;
    std::vector<TargetRelocationInfo> rows{pc, tls, rva, linearRow("s", 4, 4, R::Signed)};
    EXPECT_FALSE(pick(rows, 4).has_value()) << "no row of these is a slot";
    rows.push_back(linearRow("e", 5, 4, R::Either));
    EXPECT_EQ(pick(rows, 4), RelocationKind{5}) << "`either` when no row is exactly the addresses";
    rows.push_back(linearRow("u", 6, 4, R::Unsigned));
    EXPECT_EQ(pick(rows, 4), RelocationKind{6}) << "`unsigned` whatever the list's order";
    rows.push_back(linearRow("u2", 7, 4, R::Unsigned));
    EXPECT_FALSE(pick(rows, 4).has_value()) << "two `unsigned` rows: the document does not say which";
    EXPECT_EQ(addressSlotCandidates(rows, 4).size(), 2u);
    std::vector<TargetRelocationInfo> wide{linearRow("a", 8, 8, std::nullopt), linearRow("b", 9, 8, std::nullopt)};
    EXPECT_FALSE(pick(wide, 8).has_value()) << "two 8-byte absolute rows: the document does not say which";
    wide.pop_back();
    EXPECT_EQ(pick(wide, 8), RelocationKind{8});
}

// Rule (i): a document whose two rows the rule picks equally is refused at LOAD, before any slot asks, naming the
// target, the width and the rows.
TEST(AddressSlot, TwoRowsTheRulePicksEquallyAreRefusedAtLoad) {
    auto const loadErrors = [](std::string const& rows) {
        std::string const json = R"({ "dssTargetVersion": 1, "target": {"name":"two_slots"},
            "opcodes": [ {"mnemonic":"invalid","result":"none"} ], "relocations": [ )" + rows + " ] }";
        auto const t = TargetSchema::loadFromText(json, "two_slots.json");
        if (t.has_value()) return std::string{};
        std::string s;
        for (auto const& d : t.error()) s += d.message + "\n";
        return s;
    };
    auto const four = loadErrors(R"(
        { "name": "u1", "kind": 1, "formula": "linear", "pcRelative": false, "addendBias": 0, "widthBytes": 4, "fieldRange": "unsigned" },
        { "name": "u2", "kind": 2, "formula": "linear", "pcRelative": false, "addendBias": 0, "widthBytes": 4, "fieldRange": "unsigned" })");
    ASSERT_FALSE(four.empty()) << "two `unsigned` 4-byte absolute rows";
    EXPECT_NE(four.find("target 'two_slots': 2 relocations fill a 4-byte address slot equally ('u1', 'u2')"),
              std::string::npos) << four;
    EXPECT_NE(four.find("of the 'unsigned' range"), std::string::npos) << four;
    auto const eight = loadErrors(R"(
        { "name": "a64", "kind": 1, "formula": "linear", "pcRelative": false, "addendBias": 0, "widthBytes": 8 },
        { "name": "b64", "kind": 2, "formula": "linear", "pcRelative": false, "addendBias": 0, "widthBytes": 8 })");
    EXPECT_NE(eight.find("2 relocations fill a 8-byte address slot equally ('a64', 'b64')"), std::string::npos)
        << eight;
    // The control: one `unsigned` and one `either` row of a width are no ambiguity — the rule picks the first.
    EXPECT_EQ(loadErrors(R"(
        { "name": "u", "kind": 1, "formula": "linear", "pcRelative": false, "addendBias": 0, "widthBytes": 4, "fieldRange": "unsigned" },
        { "name": "e", "kind": 2, "formula": "linear", "pcRelative": false, "addendBias": 0, "widthBytes": 4, "fieldRange": "either" })"),
              "");
}

// Main's condition (b): each SHIPPED target's address slot per width is the row named here, so a row added later
// cannot silently change what `.long sym`, a pointer or an import slot is written as. A target shipped without a line
// here fails; so does a line naming a target no longer shipped.
TEST(AddressSlot, EachShippedTargetsSlotPerWidthIsTheRowNamedHere) {
    std::map<std::string, std::map<std::uint8_t, std::string>> const named{
        {"x86_64", {{1, ""}, {2, ""}, {4, "abs32"}, {8, "abs64"}}},
        {"arm64", {{1, ""}, {2, ""}, {4, "abs32"}, {8, "abs64"}}},
    };
    std::set<std::string> shipped;
    for (auto const& e : fs::directory_iterator{dss::test::configRoot() / "targets"}) {
        std::string const file = e.path().filename().string();
        std::string_view const suffix = ".target.json";
        if (file.size() <= suffix.size() || !file.ends_with(suffix)) continue;
        shipped.insert(file.substr(0, file.size() - suffix.size()));
    }
    ASSERT_FALSE(shipped.empty());
    for (auto const& [name, _] : named) {
        EXPECT_TRUE(shipped.contains(name)) << "'" << name << "' is named here and no longer shipped";
    }
    for (auto const& name : shipped) {
        SCOPED_TRACE(name);
        auto const it = named.find(name);
        if (it == named.end()) {
            ADD_FAILURE() << "shipped target '" << name << "' states no address slot per width here: name its rows";
            continue;
        }
        auto const t = shippedTarget(name.c_str());
        ASSERT_TRUE(t);
        for (auto const& [width, rowName] : it->second) {
            SCOPED_TRACE(static_cast<int>(width));
            auto const kind = linker::absolutePointerRelocKind(*t, width);
            if (rowName.empty()) {
                EXPECT_FALSE(kind.has_value()) << "no slot of this width";
            } else {
                EXPECT_EQ(kind, row(*t, rowName).kind) << "the slot is '" << rowName << "'";
            }
        }
    }
}

// ══ THE ASSEMBLER'S DATA WORD — `.long main` on arm64 ════════════════════════════════════════════════════════════════

namespace {

constexpr char const* kLongMainArm64 =
    "        .text\n"
    "        .globl main\n"
    "        .type main, %function\n"
    "main:\n"
    "        adrp x1, tbl\n"
    "        add  x1, x1, :lo12:tbl\n"
    "        ldr  w2, [x1]\n"
    "        adrp x3, main\n"
    "        add  x3, x3, :lo12:main\n"
    "        mov  x0, #7\n"
    "        cmp  x2, x3\n"
    "        b.ne Ldone\n"
    "        mov  x0, #42\n"
    "Ldone:\n"
    "        ret\n"
    "        .data\n"
    "        .globl tbl\n"
    "tbl:\n"
    "        .long main\n";

// Builds `source` (`language`) for `target` into `dir`; the artifact's path, or empty with `rc` set.
[[nodiscard]] fs::path build(fs::path const& dir, std::vector<fs::path> const& inputs, std::string const& language,
                             std::string const& target, DiagnosticReporter& rep, int& rc) {
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    std::vector<std::string> paths;
    for (auto const& i : inputs) paths.push_back(i.string());
    rc = p.compileUnits(paths, language, std::vector<std::string>{target}, rep);
    if (rc != 0) return {};
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file()) return e.path();
    }
    return {};
}

[[nodiscard]] fs::path write(fs::path const& dir, std::string_view name, std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

[[nodiscard]] fs::path writeFixture(fs::path const& dir, std::string_view name) {
    auto const bytes = fixture(name);
    auto const p     = dir / std::string{name};
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                             static_cast<std::streamsize>(bytes.size()));
    return p;
}

// The ELF relocation types of `section` in the relocatable object at `path` (a reader of the test's own).
[[nodiscard]] std::vector<std::uint32_t> elfRelocationTypes(fs::path const& path, std::string_view section) {
    std::ifstream                   in{path, std::ios::binary};
    std::vector<std::uint8_t> const b{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto const u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8)); };
    auto const u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    auto const u64 = [&](std::size_t o) { return static_cast<std::uint64_t>(u32(o)) | (std::uint64_t{u32(o + 4)} << 32); };
    std::vector<std::uint32_t> out;
    if (b.size() < 64 || b[0] != 0x7F || b[4] != 2) return out;
    std::size_t const shoff = u64(0x28), shentsize = u16(0x3A), shnum = u16(0x3C), shstrndx = u16(0x3E);
    std::size_t const strOff = u64(shoff + shstrndx * shentsize + 0x18);
    for (std::size_t i = 0; i < shnum; ++i) {
        std::size_t const sh = shoff + i * shentsize;
        if (u32(sh + 4) != 4) continue;   // SHT_RELA
        std::string const name{reinterpret_cast<char const*>(b.data() + strOff + u32(sh))};
        if (name != std::string{".rela"} + std::string{section}) continue;
        std::size_t const off = u64(sh + 0x18), size = u64(sh + 0x20);
        for (std::size_t r = off; r + 24 <= off + size; r += 24) out.push_back(u32(r + 8));
    }
    return out;
}

}  // namespace

// Every leg: DSS's arm64 object writes the word R_AARCH64_ABS32 (258), as GNU as does — never R_AARCH64_CALL26 (283).
TEST(AssemblerDataWord, AnArm64LongNamingAFunctionIsAnAbsoluteWord) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "linear-field-range-asm"};
    auto const               src = write(scratch.path(), "main.s", kLongMainArm64);
    DiagnosticReporter       rep;
    int                      rc  = 0;
    auto const obj = build(scratch.path(), {src}, "asm-arm64-gas", "arm64:elf64-aarch64-linux", rep, rc);
    ASSERT_EQ(rc, 0) << diagnosticsOf(rep);
    ASSERT_FALSE(obj.empty());
    EXPECT_EQ(elfRelocationTypes(obj, ".data"), (std::vector<std::uint32_t>{258u}))
        << "R_AARCH64_ABS32 = 258; R_AARCH64_CALL26 = 283 is a branch displacement ORed into a data word";
}

// linux arm64, THE OBJECT ROUTE (main's ruling): the miscompile was in DSS's OBJECT, which GNU ld and ld.lld LINKED,
// writing the word 0x03ffbffe where &main was 0x4000f0 (✔MEASURED 2026-10-20) — while DSS's own image link refused the
// `call26` data fixup. So the pin is DSS's object of the program, linked by the host's `cc -no-pie` (GNU ld) and run:
// the word read back is &main (exit 42); a branch displacement exits 7. DSS's own image link of the same program runs
// to 42 too.
TEST(AssemblerDataWordNative, AnArm64LongReadsBackAsTheFunctionsAddress) {
#if !defined(__linux__) || !defined(__aarch64__)
    GTEST_SKIP() << "DSS's object is linked by the host's cc and run only on linux arm64";
#else
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "linear-field-range-asm-run"};
    auto const               dir = scratch.path();
    auto const               src = write(dir, "main.s", kLongMainArm64);
    // The host's cc is the reference linker here: its absence is the arm NOT RUN, said by name — a failure in a
    // strict run (the gate's), a printed line in any other — never a silent pass.
    if (std::system(test_support::native_probe::captureCmd("cc --version", dir / "cc-version.txt").c_str()) != 0) {
        auto const strict = test_support::readStrictArmVerdicts();
        if (strict.on || strict.malformed) {
            FAIL() << "[native-arm] the object route: no working `cc` on this linux arm64 host, and "
                   << test_support::kStrictArmVerdictsEnv << " makes that a failure"
                   << test_support::native_probe::tailOf(dir / "cc-version.txt", 5, "cc --version");
        }
        GTEST_SKIP() << "[native-arm] the object route: not run -- no working `cc` on this host";
    }
    {
        DiagnosticReporter rep;
        int                rc  = 0;
        auto const         obj = build(dir / "obj", {src}, "asm-arm64-gas", "arm64:elf64-aarch64-linux", rep, rc);
        ASSERT_EQ(rc, 0) << diagnosticsOf(rep);
        ASSERT_FALSE(obj.empty());
        auto const        exe = dir / "via-gnu-ld";
        std::string const cmd = "cc -no-pie -o \"" + exe.string() + "\" \"" + obj.string() + "\"";
        ASSERT_EQ(std::system(test_support::native_probe::captureCmd(cmd, dir / "cc.txt").c_str()), 0)
            << cmd << test_support::native_probe::tailOf(dir / "cc.txt", 20, "cc");
        auto const r = test_support::runBinary(exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, 42u) << "DSS's object linked by GNU ld -- 42: the word is &main; 7: it is not";
    }
    DiagnosticReporter rep;
    int                rc    = 0;
    auto const image = build(dir / "image", {src}, "asm-arm64-gas", "arm64:elf64-aarch64-linux-exec", rep, rc);
    ASSERT_EQ(rc, 0) << diagnosticsOf(rep);
    ASSERT_FALSE(image.empty());
    auto const r = test_support::runBinary(image);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u) << "DSS's own image -- 42: the word is &main; 7: it is not";
#endif
}

// ══ THE REFERENCES' TYPES — JUMP26 and 32S, read and linked ═════════════════════════════════════════════════════════

namespace {

[[nodiscard]] std::optional<AssembledModule> readElf(std::string_view name, char const* target, char const* format,
                                                     DiagnosticReporter& rep) {
    auto const t = shippedTarget(target);
    auto const f = shippedFormat(format);
    if (!t || !f) return std::nullopt;
    auto const bytes = fixture(name);
    return elf::readRelocatableObject(bytes, *t, *f, rep, CompilationUnitId{2});
}

[[nodiscard]] bool carriesKind(AssembledModule const& m, RelocationKind kind) {
    for (auto const& fn : m.functions) {
        for (auto const& r : fn.relocations) {
            if (r.kind == kind) return true;
        }
    }
    return false;
}

constexpr char const* kJump26Main =
    "int tail(int);\n"
    "int callee(int x) { return x * 2; }\n"
    "int main(void) { return tail(20) == 42 ? 42 : 7; }\n";
constexpr char const* kPickMain =
    "int pick(int);\n"
    "int callee(int x) { return x; }\n"
    "int main(void) { return pick(3) == 44 ? 42 : 7; }\n";

}  // namespace

TEST(ReferenceRelocationTypes, GccsTailCallIsReadAsJump26) {
    DiagnosticReporter rep;
    auto const m = readElf("reference_relocation_types_aarch64_elf_gcc_O2.o", "arm64", "elf64-aarch64-linux", rep);
    ASSERT_TRUE(m.has_value()) << "an object gcc -O2 writes for a tail call is read:" << diagnosticsOf(rep);
    auto const t = shippedTarget("arm64");
    ASSERT_TRUE(t);
    EXPECT_TRUE(carriesKind(*m, row(*t, "jump26").kind)) << "R_AARCH64_JUMP26 reads as `jump26`";
}

TEST(ReferenceRelocationTypes, AnIndexedGlobalOfANonPicObjectIsReadAs32S) {
    DiagnosticReporter rep;
    auto const m = readElf("reference_relocation_types_x86_64_elf_gcc_O2_nopic.o", "x86_64", "elf64-x86_64-linux", rep);
    ASSERT_TRUE(m.has_value()) << "a -fno-pic object's `table(,%rdi,4)` is read:" << diagnosticsOf(rep);
    auto const t = shippedTarget("x86_64");
    ASSERT_TRUE(t);
    EXPECT_TRUE(carriesKind(*m, row(*t, "abs32s").kind)) << "R_X86_64_32S reads as `abs32s`";
}

// Every leg: the image links. GNU ld 2.42 links both objects into a non-PIE image; the programs exit 42.
TEST(ReferenceRelocationTypes, DssLinksEachObjectBesideACMain) {
    struct Case {
        char const* object;
        char const* main;
        char const* target;
    };
    for (Case const c : {Case{"reference_relocation_types_aarch64_elf_gcc_O2.o", kJump26Main, "arm64:elf64-aarch64-linux-exec"},
                         Case{"reference_relocation_types_x86_64_elf_gcc_O2_nopic.o", kPickMain,
                              "x86_64:elf64-x86_64-linux-exec"}}) {
        SCOPED_TRACE(c.object);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "reference-relocation-types"};
        DiagnosticReporter       rep;
        int                      rc = 0;
        auto const image = build(scratch.path(), {writeFixture(scratch.path(), c.object), write(scratch.path(), "main.c", c.main)},
                                 "c", c.target, rep, rc);
        EXPECT_EQ(rc, 0) << diagnosticsOf(rep);
        EXPECT_FALSE(image.empty());
    }
}

// A slid image cannot hold a 32S absolute: refused BY NAME, as GNU ld refuses it ("relocation R_X86_64_32S against
// symbol `table' can not be used when making a PIE object; recompile with -fPIE", and "… a shared object" — MEASURED
// 2026-10-20, GNU ld 2.42; ld.lld 18.1.3: "relocation R_X86_64_32S cannot be used against symbol 'table'").
TEST(ReferenceRelocationTypes, ASlidImageRefuses32SByName) {
    for (char const* target : {"x86_64:elf64-x86_64-linux-pie", "x86_64:elf64-x86_64-linux-dyn"}) {
        SCOPED_TRACE(target);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "reference-relocation-types-slid"};
        DiagnosticReporter       rep;
        int                      rc = 0;
        auto const image = build(scratch.path(),
                                 {writeFixture(scratch.path(), "reference_relocation_types_x86_64_elf_gcc_O2_nopic.o"),
                                  write(scratch.path(), "main.c", kPickMain)},
                                 "c", target, rep, rc);
        EXPECT_NE(rc, 0) << "a 32-bit absolute cannot follow a slide";
        // Both undeclared-kind refusals an ELF image can meet say "not declared by" — the linker's pre-flight ("… is not
        // declared by object format …") and the ELF writer's ("… not declared by ELF format …") — so this asks the
        // question both answer, not one writer's spelling.
        EXPECT_FALSE(said(rep, "not declared by"))
            << "refused by the image's rule, not as an undeclared kind:" << diagnosticsOf(rep);
        EXPECT_TRUE(said(rep, "abs32s")) << "the refusal names the kind:" << diagnosticsOf(rep);
    }
}

// linux arm64 / x86_64: each image runs to 42 (GNU ld's image of the same object does, MEASURED).
TEST(ReferenceRelocationTypesNative, EachObjectLinkedByDssRuns) {
    auto const host = test_support::hostNativeTarget().execTarget;
    char const* object = nullptr;
    char const* main   = nullptr;
    if (host == "arm64:elf64-aarch64-linux-exec") {
        object = "reference_relocation_types_aarch64_elf_gcc_O2.o";
        main   = kJump26Main;
    } else if (host == "x86_64:elf64-x86_64-linux-exec") {
        object = "reference_relocation_types_x86_64_elf_gcc_O2_nopic.o";
        main   = kPickMain;
    } else {
        GTEST_SKIP() << "the image runs only where the host is linux arm64 or linux x86_64";
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "reference-relocation-types-run"};
    DiagnosticReporter       rep;
    int                      rc = 0;
    auto const image = build(scratch.path(), {writeFixture(scratch.path(), object), write(scratch.path(), "main.c", main)},
                             "c", std::string{host}, rep, rc);
    ASSERT_EQ(rc, 0) << diagnosticsOf(rep);
    auto const r = test_support::runBinary(image);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u) << "42: the reference's branch / indexed read reached its target; 7: it did not";
}

// ══ THE EDGES, LINKED AND RUN — the measured miscompile class ═══════════════════════════════════════════════════════

namespace {

// `edge()` returns `ext ± 2^31` through the fixture's field; main checks it against the value computed in C, the
// field read back the way the instruction reads it (`movl`: zero-extended to 64 bits; `movq $imm32`: sign-extended).
constexpr char const* kEdgeMainZx =
    "extern char ext[]; unsigned long edge(void);\n"
    "int main(void) { unsigned long want = (unsigned long)(unsigned)((unsigned long)ext + %s);\n"
    "  return edge() == want ? 42 : 7; }\n";
constexpr char const* kEdgeMainSx =
    "extern char ext[]; unsigned long edge(void);\n"
    "int main(void) { unsigned long want = (unsigned long)ext + %s;\n"
    "  return edge() == want ? 42 : 7; }\n";

struct EdgeObject {
    char const* object;
    char const* main;
    char const* offset;   // the C spelling of ±2^31
    bool        links;
    char const* range;    // the range a refusal names
};
constexpr EdgeObject kEdgeObjects[] = {
    {"linear_field_edge_32_hi_x86_64_elf_gas.o", kEdgeMainZx, "0x80000000UL", true, "unsigned"},
    {"linear_field_edge_32_lo_x86_64_elf_gas.o", kEdgeMainZx, "-0x80000000UL", false, "unsigned"},
    {"linear_field_edge_32s_hi_x86_64_elf_gas.o", kEdgeMainSx, "0x80000000UL", false, "signed"},
    {"linear_field_edge_32s_lo_x86_64_elf_gas.o", kEdgeMainSx, "-0x80000000UL", true, "signed"},
};

[[nodiscard]] std::string edgeMain(EdgeObject const& e) {
    std::string s{e.main};
    auto const  at = s.find("%s");
    return s.replace(at, 2, e.offset);
}

}  // namespace

// Every leg: each edge links or is refused exactly as GNU ld 2.42 and ld.lld 18.1.3 link or refuse it (MEASURED
// 2026-10-20); a refusal names the field's range.
TEST(LinearFieldRange, EachEdgeObjectLinksAsTheReferenceLinkersLinkIt) {
    for (auto const& e : kEdgeObjects) {
        SCOPED_TRACE(e.object);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "linear-field-edge"};
        DiagnosticReporter       rep;
        int                      rc = 0;
        (void)build(scratch.path(), {writeFixture(scratch.path(), e.object), write(scratch.path(), "main.c", edgeMain(e))},
                    "c", "x86_64:elf64-x86_64-linux-exec", rep, rc);
        EXPECT_EQ(rc == 0, e.links) << diagnosticsOf(rep);
        if (!e.links) {
            EXPECT_TRUE(said(rep, std::string{"the "} + e.range + " 32-bit range")) << diagnosticsOf(rep);
        }
    }
}

// linux x86_64: each linked edge runs to 42 — the value the program reads is the one it computed. And each REFUSED
// edge, were it linked, is the miscompile this row records: before `fieldRange` DSS linked the 32 LO object, `movl`
// zero-extended its field, and `edge()` returned 2^32 + V (exit 7).
TEST(LinearFieldRangeNative, EachLinkedEdgeReadsBackTheValueItsProgramComputed) {
    if (test_support::hostNativeTarget().execTarget != "x86_64:elf64-x86_64-linux-exec") {
        GTEST_SKIP() << "the image runs only where the host is linux x86_64";
    }
    for (auto const& e : kEdgeObjects) {
        SCOPED_TRACE(e.object);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "linear-field-edge-run"};
        DiagnosticReporter       rep;
        int                      rc = 0;
        auto const image = build(scratch.path(), {writeFixture(scratch.path(), e.object), write(scratch.path(), "main.c", edgeMain(e))},
                                 "c", "x86_64:elf64-x86_64-linux-exec", rep, rc);
        if (rc != 0) {
            EXPECT_FALSE(e.links) << "a reference links it:" << diagnosticsOf(rep);
            continue;
        }
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_TRUE(e.links) << "DSS linked an edge both reference linkers refuse, and it ran to " << r.exitCode
                             << " (7: the field read back is not the value the program computed)";
        EXPECT_EQ(r.exitCode, 42u);
    }
}
